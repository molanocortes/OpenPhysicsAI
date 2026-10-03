/* ops_mesh.c - volume mesh generation and inspection */
#include "../core/sha256.h"
#include "../fem/tet.h"
#include "../geom/bvh.h"
#include "../geom/tetmesh.h"
#include "../threads.h"
#include "ops_internal.h"
#include "selection.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* two significant digits, rounded down so the mesh is never coarser than the rule asked for */
static double nice_size(double h) {
    double p = pow(10, floor(log10(h)));
    return floor(h / p * 10) / 10 * p;
}

static void mesh_hash(Project *p, char out[65]) {
    Sha256 s;
    sha256_init(&s);
    static const char TAG[] = "navier-am voxel-hex8 mesh v1";
    sha256_update(&s, TAG, sizeof TAG);
    sha256_update(&s, p->mesh.h, sizeof p->mesh.h);
    unsigned char plate = p->mesh.include_plate;
    sha256_update(&s, &plate, 1);
    sha256_update(&s, &p->mesh.plate_thickness, sizeof(double));
    sha256_update(&s, &p->mesh.plate_margin, sizeof(double));
    for (int i = 0; i < p->mesh.nbodies; i++) {
        const Body *b = project_body(p, p->mesh.body_name[i]);
        if (!b) continue;
        sha256_update(&s, b->name, strlen(b->name) + 1);
        sha256_update(&s, b->sha256, 64);
        int role = (int)b->role;
        sha256_update(&s, &role, sizeof role);
        sha256_update(&s, &b->unit_scale, sizeof(double));
        sha256_update(&s, b->place.R, sizeof b->place.R);
        sha256_update(&s, b->place.t, sizeof b->place.t);
        unsigned char rep[3] = {b->repair.remove_degenerate, b->repair.remove_duplicate_faces, b->repair.orient_outward};
        sha256_update(&s, rep, sizeof rep);
    }
    unsigned char dg[32];
    sha256_final(&s, dg);
    sha256_hex(dg, out);
}

static JsonValue *tet_summary_json(Project *p);

static JsonValue *mesh_summary_json(Project *p) {
    const MeshState *ms = &p->mesh;
    const HexMesh *hm = &ms->hm;
    if (hm->elem_type) return tet_summary_json(p);
    JsonValue *o = json_object();
    json_set_string(o, "method", "voxel_hex8");
    json_set_string(o, "description",
                    "layer-aligned voxel mesh of 8-node hexahedra: undistorted boxes whose boundary is a staircase approximation of the STL surface");
    json_set_int(o, "nodes", hm->nnodes);
    json_set_int(o, "elements", hm->nelems);
    json_set_int(o, "dofs_structural", 3LL * hm->nnodes);
    int counts[HEX_REGION_COUNT] = {0};
    for (int e = 0; e < hm->nelems; e++)
        if (hm->region[e] < HEX_REGION_COUNT) counts[hm->region[e]]++;
    JsonValue *byr = json_set_object(o, "elements_by_region");
    json_set_int(byr, "part", counts[HEX_REGION_PART]);
    json_set_int(byr, "support", counts[HEX_REGION_SUPPORT]);
    json_set_int(byr, "build_plate", counts[HEX_REGION_PLATE]);
    json_set(o, "element_size_mm", json_vec3(1e3 * hm->h[0], 1e3 * hm->h[1], 1e3 * hm->h[2]));
    json_set(o, "grid_cells", json_vec3(hm->dims[0], hm->dims[1], hm->dims[2]));
    if (ms->include_plate) {
        JsonValue *pl = json_set_object(o, "build_plate");
        json_set_number(pl, "thickness_mm", 1e3 * ms->plate_thickness);
        json_set_number(pl, "margin_mm", 1e3 * ms->plate_margin);
        json_set_int(pl, "element_layers", hm->plate_layers);
    }
    double hmin = fmin(hm->h[0], fmin(hm->h[1], hm->h[2])), hmax = fmax(hm->h[0], fmax(hm->h[1], hm->h[2]));
    JsonValue *q = json_set_object(o, "quality");
    json_set_string(q, "jacobian", "constant and positive in every element (det J = hx hy hz / 8); no inverted or distorted elements");
    json_set_number(q, "jacobian_determinant_mm3", 1e9 * hm->h[0] * hm->h[1] * hm->h[2] / 8);
    json_set_number(q, "aspect_ratio", hmin > 0 ? hmax / hmin : 0);
    json_set_int(q, "face_connected_regions", hm->regions);
    /* how sure the inside test was: cells the three axis rays did not agree on, and the box they sit in */
    JsonValue *ins = json_set_object(q, "inside_test");
    json_set_string(ins, "method", "parity of ray crossings along x, y and z through one sample point per cell, majority of three");
    json_set_int(ins, "uncertain_cells", hm->uncertain_cells);
    json_set_int(ins, "abstained_rays", hm->abstained_rays);
    json_set_string(ins, "abstained_rays_meaning",
                    "rays whose crossing count was odd: they went through a hole or a sheet of zero thickness, so that "
                    "axis did not vote for that column");
    json_set_number(ins, "uncertain_fraction_of_elements", hm->nelems > 0 ? (double)hm->uncertain_cells / hm->nelems : 0);
    json_set_number(ins, "refused_above_fraction", HEXMESH_UNCERTAIN_LIMIT);
    if (hm->uncertain_cells > 0 && isfinite(hm->uncertain_min[0])) {
        json_set(ins, "uncertain_box_min_mm", json_vec3(1e3 * hm->uncertain_min[0], 1e3 * hm->uncertain_min[1], 1e3 * hm->uncertain_min[2]));
        json_set(ins, "uncertain_box_max_mm", json_vec3(1e3 * hm->uncertain_max[0], 1e3 * hm->uncertain_max[1], 1e3 * hm->uncertain_max[2]));
    }
    json_set_int(q, "edge_or_vertex_contact_nodes", hm->edge_contacts);
    JsonValue *bodies = json_set_array(o, "bodies");
    for (int i = 0; i < ms->nbodies && i < hm->nbodies; i++) {
        JsonValue *bo = json_object();
        json_set_string(bo, "name", ms->body_name[i]);
        double vs = hm->body_volume_stl[i], vm = hm->body_volume_mesh[i], as = hm->body_area_stl[i], am = hm->body_area_mesh[i];
        json_set_number(bo, "volume_stl_mm3", 1e9 * vs);
        json_set_number(bo, "volume_mesh_mm3", 1e9 * vm);
        if (vs > 0) json_set_number(bo, "volume_error_percent", 100 * (vm - vs) / vs);
        json_set_number(bo, "area_stl_mm2", 1e6 * as);
        json_set_number(bo, "area_mesh_mm2", 1e6 * am);
        if (as > 0) json_set_number(bo, "area_ratio_mesh_to_stl", am / as);
        json_push(bodies, bo);
    }
    JsonValue *bd = json_set_object(o, "boundary");
    json_set_int(bd, "faces", hm->nfaces);
    json_set_number(bd, "mean_distance_to_stl_mm", 1e3 * hm->mean_face_distance);
    json_set_string(bd, "mean_distance_weighting", "boundary-face area");
    json_set_number(bd, "max_distance_to_stl_mm", 1e3 * hm->max_face_distance);
    JsonValue *sels = json_set_array(o, "selections");
    for (int i = 0; i < p->nselections; i++) {
        const Selection *s = &p->selections[i];
        if (s->stale) continue;
        double ma = 0;
        int nf = selection_mesh_faces(p, s, NULL, 0, &ma);
        JsonValue *so = json_object();
        json_set_string(so, "name", s->name);
        json_set_int(so, "mesh_faces", nf);
        json_set_number(so, "mesh_area_mm2", 1e6 * ma);
        json_set_number(so, "stl_area_mm2", 1e6 * s->area);
        if (s->area > 0) json_set_number(so, "area_ratio", ma / s->area);
        json_push(sels, so);
    }
    json_set_string(o, "hash", ms->hash);
    json_set_int(o, "generated_at_revision", (long long)ms->revision);
    json_set_number(o, "seconds", hm->seconds);
    return o;
}

static void tet_warnings(Project *p, JsonValue *w);

static void mesh_warnings(Project *p, JsonValue *w) {
    const MeshState *ms = &p->mesh;
    const HexMesh *hm = &ms->hm;
    if (hm->elem_type) {
        tet_warnings(p, w);
        return;
    }
    double hmax = fmax(hm->h[0], fmax(hm->h[1], hm->h[2]));
    bool unchecked = false;
    for (int i = 0; i < ms->nbodies && i < hm->nbodies; i++) {
        double vs = hm->body_volume_stl[i], vm = hm->body_volume_mesh[i];
        if (vs > 0 && fabs(vm - vs) > 0.03 * vs)
            json_push(w, json_stringf("the mesh of '%s' differs from the STL volume by %.1f%%: refine the mesh for stiffness- or weight-sensitive results",
                                      ms->body_name[i], 100 * (vm - vs) / vs));
        const Body *b = project_body(p, ms->body_name[i]);
        if (!b) continue;
        double t05 = b->diag.thickness_p05 * b->unit_scale, tmin = b->diag.min_thickness * b->unit_scale;
        if (!isfinite(t05)) unchecked = true;
        else if (t05 < 2 * hmax)
            json_push(w, json_stringf("walls of '%s' down to %.3g mm (5th percentile; minimum %.3g mm) are thinner than twice the largest element spacing of %.3g mm: "
                                      "depending on wall orientation they may be poorly resolved or lost; refine the spacing across the wall",
                                      b->name, 1e3 * t05, 1e3 * tmin, 1e3 * hmax));
    }
    if (unchecked) json_push(w, json_string("wall thickness has not been measured: run geometry_diagnostics (thickness: run) to find walls thinner than two elements"));
    int expected = ms->include_plate ? 1 : ms->nbodies;
    if (hm->regions > expected)
        json_push(w, json_stringf("the mesh has %d face-connected regions where %d are expected: thin connections were lost, or parts do not touch%s", hm->regions,
                                  expected, ms->include_plate ? " the plate" : ""));
    if (hm->edge_contacts > 0)
        json_push(w, json_stringf("%d nodes join mesh regions only along edges or corners, which is not a realistic load path", hm->edge_contacts));
    for (int i = 0; i < p->nselections; i++) {
        const Selection *s = &p->selections[i];
        bool meshed = false;
        for (int k = 0; k < ms->nbodies; k++) meshed |= !strcmp(ms->body_name[k], s->body);
        if (meshed && !s->stale && selection_mesh_faces(p, s, NULL, 0, NULL) == 0)
            json_push(w, json_stringf("no mesh face carries selection '%s' (%.4g mm2): it is smaller than the elements", s->name, 1e6 * s->area));
    }
}

/* ---- the conforming tetrahedral mesh (docs/contracts/tet-mesh.md) ------------------------------------------- */

static void mesh_hash_tet(Project *p, char out[65]) {
    Sha256 s;
    sha256_init(&s);
    static const char TAG[] = "navier-am tetrahedral mesh v1";
    sha256_update(&s, TAG, sizeof TAG);
    sha256_update(&s, p->mesh.h, sizeof p->mesh.h);
    sha256_update(&s, &p->mesh.tet_order, sizeof p->mesh.tet_order);
    sha256_update(&s, &p->mesh.tet_interior, sizeof p->mesh.tet_interior);
    sha256_update(&s, &p->mesh.tet_thin, sizeof p->mesh.tet_thin);
    for (int i = 0; i < p->mesh.nbodies; i++) {
        const Body *b = project_body(p, p->mesh.body_name[i]);
        if (!b) continue;
        sha256_update(&s, b->name, strlen(b->name) + 1);
        sha256_update(&s, b->sha256, 64);
        sha256_update(&s, &b->unit_scale, sizeof(double));
        sha256_update(&s, b->place.R, sizeof b->place.R);
        sha256_update(&s, b->place.t, sizeof b->place.t);
        unsigned char rep[3] = {b->repair.remove_degenerate, b->repair.remove_duplicate_faces, b->repair.orient_outward};
        sha256_update(&s, rep, sizeof rep);
    }
    unsigned char dg[32];
    sha256_final(&s, dg);
    sha256_hex(dg, out);
}

static JsonValue *tet_summary_json(Project *p) {
    const MeshState *ms = &p->mesh;
    const HexMesh *hm = &ms->hm;
    const TetMesh *q = &ms->tetq;
    JsonValue *o = json_object();
    json_set_string(o, "method", ms->tet_order == 2 ? "tet10" : "tet4");
    json_set_string(o, "description",
                    ms->tet_order == 2
                        ? "conforming mesh of 10-node quadratic tetrahedra by isosurface stuffing; surface nodes and the mid-edge nodes of surface edges lie "
                          "on the STL, sharp edges are rounded at the surface cell scale"
                        : "conforming mesh of 4-node linear tetrahedra by isosurface stuffing; surface nodes lie on the STL, sharp edges are rounded at "
                          "the surface cell scale; linear tetrahedra lock in bending, use order 2 for results");
    json_set_int(o, "nodes", hm->nnodes);
    json_set_int(o, "elements", hm->nelems);
    json_set_int(o, "dofs_structural", 3LL * hm->nnodes);
    json_set_number(o, "surface_size_mm", 1e3 * ms->h[0]);
    json_set_number(o, "interior_size_mm", 1e3 * ms->tet_interior);
    json_set_int(o, "thin_wall_levels", ms->tet_thin);
    JsonValue *qq = json_set_object(o, "quality");
    json_set_number(qq, "min_dihedral_deg", q->min_dihedral);
    json_set_number(qq, "max_dihedral_deg", q->max_dihedral);
    json_set(qq, "min_dihedral_at_mm", json_vec3(1e3 * q->min_dihedral_at[0], 1e3 * q->min_dihedral_at[1], 1e3 * q->min_dihedral_at[2]));
    json_set_number(qq, "worst_aspect_ratio", q->worst_aspect);
    json_set_string(qq, "aspect_ratio_definition", "circumradius over three times the inradius; 1 for a regular tetrahedron");
    json_set_number(qq, "refused_below_deg", 5);
    json_set_int(qq, "face_connected_regions", hm->regions);
    json_set_int(qq, "curved_surface_edges", q->curved_edges);
    json_set_int(qq, "surface_edges_kept_straight", q->straightened_edges);
    JsonValue *sd = json_set_object(o, "surface_distance_to_stl_mm");
    json_set_number(sd, "nodes_max", 1e3 * q->surf_node_dist_max);
    json_set_number(sd, "nodes_mean", 1e3 * q->surf_node_dist_mean);
    json_set_number(sd, "face_centroids_max", 1e3 * q->surf_face_dist_max);
    json_set_number(sd, "face_centroids_mean", 1e3 * q->surf_face_dist_mean);
    JsonValue *how = json_set_object(o, "construction");
    json_set_int(how, "octree_leaves", q->octree_leaves);
    json_set_int(how, "thin_wall_splits", q->thin_splits);
    json_set_int(how, "thin_walls_not_resolved", q->unresolved_thin);
    json_set_number(how, "finest_cell_mm", 1e3 * q->finest_cell);
    json_set_int(how, "pinch_splits", q->pinch_splits);
    json_set_int(how, "warped_vertices", q->warped_vertices);
    json_set_int(how, "unwarped_vertices", q->unwarped_vertices);
    json_set_int(how, "smoothed_nodes", q->smoothed_nodes);
    json_set_int(how, "steiner_vertices", q->steiner_vertices);
    JsonValue *ins = json_set_object(how, "inside_test");
    json_set_string(ins, "method", "parity of ray crossings along three axes, majority of the axes that voted");
    json_set_int(ins, "evaluations_not_unanimous", hm->uncertain_cells);
    JsonValue *bodies = json_set_array(o, "bodies");
    for (int i = 0; i < ms->nbodies && i < hm->nbodies; i++) {
        JsonValue *bo = json_object();
        json_set_string(bo, "name", ms->body_name[i]);
        double vs = hm->body_volume_stl[i], vm = hm->body_volume_mesh[i];
        json_set_number(bo, "volume_stl_mm3", 1e9 * vs);
        json_set_number(bo, "volume_mesh_mm3", 1e9 * vm);
        if (vs > 0) json_set_number(bo, "volume_error_percent", 100 * (vm - vs) / vs);
        json_push(bodies, bo);
    }
    JsonValue *bd = json_set_object(o, "boundary");
    json_set_int(bd, "faces", hm->nfaces);
    JsonValue *sels = json_set_array(o, "selections");
    for (int i = 0; i < p->nselections; i++) {
        const Selection *s = &p->selections[i];
        if (s->stale) continue;
        double ma = 0;
        int nf = selection_mesh_faces(p, s, NULL, 0, &ma);
        JsonValue *so = json_object();
        json_set_string(so, "name", s->name);
        json_set_int(so, "mesh_faces", nf);
        json_set_number(so, "mesh_area_mm2", 1e6 * ma);
        json_set_number(so, "stl_area_mm2", 1e6 * s->area);
        if (s->area > 0) json_set_number(so, "area_ratio", ma / s->area);
        json_push(sels, so);
    }
    json_set_string(o, "hash", ms->hash);
    json_set_int(o, "generated_at_revision", (long long)ms->revision);
    json_set_number(o, "seconds", q->seconds);
    return o;
}

static void tet_warnings(Project *p, JsonValue *w) {
    const MeshState *ms = &p->mesh;
    const HexMesh *hm = &ms->hm;
    const TetMesh *q = &ms->tetq;
    /* a wall the finest cell could not resolve is perforated or gone: say so in words, with the size that would */
    if (q->unresolved_thin > 0) {
        double vol_err = 0, vs = 0, vm = 0;
        for (int i = 0; i < ms->nbodies && i < hm->nbodies; i++) vm += hm->body_volume_mesh[i], vs += hm->body_volume_stl[i];
        if (vs > 0) vol_err = 100 * (vm - vs) / vs;
        json_push(w, json_stringf("walls thinner than about %.3g mm show holes: the finest cell here is %.3g mm and %d places are still thinner than "
                                  "1.5 of it; the volume is %+.1f %% against the STL. Mesh again with surface_size %.3g mm (or one more "
                                  "thin_wall_levels) to resolve them",
                                  1e3 * 1.5 * q->finest_cell, 1e3 * q->finest_cell, q->unresolved_thin, vol_err, 0.5e3 * q->finest_cell));
    }
    if (q->min_dihedral < 10)
        json_push(w, json_stringf("the smallest dihedral angle is %.1f degrees (at %.4g, %.4g, %.4g mm), below the 10 degree target: stresses in that "
                                  "element are less accurate",
                                  q->min_dihedral, 1e3 * q->min_dihedral_at[0], 1e3 * q->min_dihedral_at[1], 1e3 * q->min_dihedral_at[2]));
    for (int i = 0; i < ms->nbodies && i < hm->nbodies; i++) {
        double vs = hm->body_volume_stl[i], vm = hm->body_volume_mesh[i];
        if (vs > 0 && fabs(vm - vs) > 0.03 * vs)
            json_push(w, json_stringf("the mesh of '%s' differs from the STL volume by %.1f%%: sharp edges are rounded at the surface cell scale and features "
                                      "below it are lost; refine surface_size",
                                      ms->body_name[i], 100 * (vm - vs) / vs));
    }
    if (hm->regions > ms->nbodies)
        json_push(w, json_stringf("the mesh has %d face-connected regions where %d are expected: a feature is thinner than the finest cell, or parts do "
                                  "not touch",
                                  hm->regions, ms->nbodies));
    if (q->straightened_edges > 0)
        json_push(w, json_stringf("%d surface edges stayed straight because curving them would have distorted an element", q->straightened_edges));
    for (int i = 0; i < p->nselections; i++) {
        const Selection *s = &p->selections[i];
        bool meshed = false;
        for (int k = 0; k < ms->nbodies; k++) meshed |= !strcmp(ms->body_name[k], s->body);
        if (meshed && !s->stale && selection_mesh_faces(p, s, NULL, 0, NULL) == 0)
            json_push(w, json_stringf("no mesh face carries selection '%s' (%.4g mm2): it is smaller than the surface cells", s->name, 1e6 * s->area));
    }
}

static void op_mesh_generate_tet(Engine *e, JsonValue *p, OpResult *out, Body **bl, int nb, bool stored) {
    Project *proj = e->proj;
    char err[512];
    if (json_get_bool(p, "include_build_plate", false)) {
        op_fail(out, NV_ERR_UNSUPPORTED, "use method voxel for analyses with the build plate", "the build plate belongs to the voxel mesh; a tetrahedral mesh "
                                                                                                "is for static structural analyses of the parts");
        return;
    }
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY}, vol = 0;
    int nt = 0, nv = 0;
    for (int i = 0; i < nb; i++) {
        Body *b = bl[i];
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], b->bmin[k]), hi[k] = fmax(hi[k], b->bmax[k]);
        vol += fabs(b->diag.volume) * b->unit_scale * b->unit_scale * b->unit_scale;
        nt += b->surf.nt, nv += b->surf.nv;
    }
    double h = 0, isize = 0;
    int order = 2;
    const JsonValue *ss = json_get(p, "surface_size");
    if (!ss) ss = json_get(p, "element_size");
    bool autosize = false;
    if (ss && !(json_str(ss) && !strcmp(json_str(ss), "auto"))) {
        if (!quantity_from_json(ss, DIM_LENGTH, "mm", &h, err, sizeof err)) {
            op_fail(out, NV_ERR_INVALID_UNIT, NULL, "surface_size: %s", err);
            return;
        }
    } else if (stored && proj->mesh.method == 1) {
        h = proj->mesh.h[0];
    } else {
        double dmin = fmin(hi[0] - lo[0], fmin(hi[1] - lo[1], hi[2] - lo[2]));
        h = dmin / 4;
        if (vol / (h * h * h) > 30000) h = cbrt(vol / 30000);
        h = nice_size(h);
        autosize = true;
    }
    if (!(h > 0)) {
        op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "surface_size must be positive");
        return;
    }
    if (json_get(p, "interior_size")) {
        if (!op_quantity(out, p, "interior_size", DIM_LENGTH, "mm", &isize, NULL)) return;
    } else if (stored && proj->mesh.method == 1 && proj->mesh.tet_interior > 0) {
        isize = proj->mesh.tet_interior;
    } else {
        isize = 4 * h;
    }
    if (isize < h) {
        op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "interior_size (%.4g mm) must not be smaller than surface_size (%.4g mm)", 1e3 * isize, 1e3 * h);
        return;
    }
    if (json_get(p, "order")) order = (int)json_get_int(p, "order", 2);
    else if (stored && proj->mesh.method == 1) order = proj->mesh.tet_order;
    if (order != 1 && order != 2) {
        op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "order must be 1 or 2");
        return;
    }
    long long maxe = json_get_int(p, "max_elements", 400000);
    if ((uint64_t)maxe > e->cfg.max_elements) maxe = (long long)e->cfg.max_elements;
    /* one triangle set for all bodies; each triangle remembers its body */
    double *V = malloc(3 * (size_t)(nv ? nv : 1) * sizeof(double));
    int *T = malloc(3 * (size_t)(nt ? nt : 1) * sizeof(int)), *tb = malloc((size_t)(nt ? nt : 1) * sizeof(int)), *toff = malloc((size_t)nb * sizeof(int));
    if (!V || !T || !tb || !toff) {
        free(V), free(T), free(tb), free(toff);
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory preparing the surface");
        return;
    }
    int vo = 0, to = 0;
    for (int i = 0; i < nb; i++) {
        Body *b = bl[i];
        memcpy(V + 3 * (size_t)vo, b->build_v, 3 * (size_t)b->surf.nv * sizeof(double));
        toff[i] = to;
        for (int t = 0; t < b->surf.nt; t++) {
            for (int k = 0; k < 3; k++) T[3 * (size_t)(to + t) + k] = b->surf.tri[3 * t + k] + vo;
            tb[to + t] = i;
        }
        vo += b->surf.nv, to += b->surf.nt;
    }
    TetStlGeometry g;
    if (!tet_stl_init(&g, V, T, nt, err, sizeof err)) {
        free(V), free(T), free(tb), free(toff);
        op_fail(out, NV_ERR_MESH_INVALID, NULL, "%s", err);
        return;
    }
    int thin = json_get(p, "thin_wall_levels") ? (int)json_get_int(p, "thin_wall_levels", 2)
                                               : (stored && proj->mesh.method == 1 ? proj->mesh.tet_thin : 2);
    TetMeshSettings st = {0};
    st.surface_size = h, st.interior_size = isize, st.order = order;
    st.thin_levels = thin == 0 ? -1 : thin;
    memcpy(st.lo, g.lo, sizeof st.lo), memcpy(st.hi, g.hi, sizeof st.hi);
    st.sdf = tet_stl_sdf, st.project = tet_stl_project, st.cross = tet_stl_cross, st.ctx = &g, st.ref_volume = vol;
    st.max_elements = (uint64_t)maxe;
    int threads = cpu_perf_count();
    ThreadPool *pool = pool_create(threads > 0 ? threads : 1);
    st.pool = pool;
    TetMesh tm;
    bool ok = tetmesh_generate(&st, &tm, err, sizeof err);
    if (pool) pool_destroy(pool);
    if (ok && g.evaluations > 0 && (double)g.votes_split > HEXMESH_UNCERTAIN_LIMIT * (double)g.evaluations) {
        snprintf(err, sizeof err, "the inside test was not unanimous on %lld of %lld points: the surface has gaps too large to call this a mesh of the part",
                 g.votes_split, g.evaluations);
        tetmesh_free(&tm);
        ok = false;
    }
    if (!ok) {
        tet_stl_free(&g);
        free(V), free(T), free(tb), free(toff);
        NvErr code = strstr(err, "limit") ? NV_ERR_RESOURCE_LIMIT : NV_ERR_MESH_INVALID;
        op_fail(out, code, "choose a surface_size well below the part size and large enough to stay within the element limit",
                "tetrahedral mesh generation failed: %s", err);
        return;
    }
    /* carry it in the project's mesh: bodies, boundary faces mapped onto STL triangles, volumes per body */
    HexMesh hm = {0};
    int ne = tm.nelems, npe = tm.npe;
    hm.nnodes = tm.nnodes, hm.nelems = ne, hm.xyz = tm.xyz, hm.conn = tm.conn, hm.nfaces = tm.nfaces, hm.face_elem = tm.face_elem,
    hm.face_local = tm.face_local;
    hm.elem_type = order == 2 ? SOLID_ELEM_TET10 : SOLID_ELEM_TET4, hm.npe = npe;
    tm.xyz = NULL, tm.conn = NULL, tm.face_elem = NULL, tm.face_local = NULL;
    hm.region = malloc((size_t)(ne ? ne : 1));
    hm.body = malloc((size_t)(ne ? ne : 1));
    hm.face_tri = malloc((size_t)(hm.nfaces ? hm.nfaces : 1) * sizeof(int));
    hm.face_dist = malloc((size_t)(hm.nfaces ? hm.nfaces : 1) * sizeof(float));
    if (!hm.region || !hm.body || !hm.face_tri || !hm.face_dist) {
        hexmesh_free(&hm);
        tet_stl_free(&g);
        free(V), free(T), free(tb), free(toff);
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory storing the mesh");
        return;
    }
    const Bvh *bvh = g.bvh;
    for (int el = 0; el < ne; el++) {
        double c[3] = {0, 0, 0}, q[3], d;
        int tri = -1;
        for (int a = 0; a < 4; a++)
            for (int k = 0; k < 3; k++) c[k] += 0.25 * hm.xyz[3 * (size_t)hm.conn[(size_t)npe * el + a] + k];
        bvh_closest(bvh, c, INFINITY, &tri, q, &d);
        int bi = tri >= 0 ? tb[tri] : 0;
        hm.body[el] = (signed char)bi;
        hm.region[el] = bl[bi]->role == ROLE_SUPPORT ? HEX_REGION_SUPPORT : HEX_REGION_PART;
    }
    double dsum = 0, dmax = 0;
    for (int f = 0; f < hm.nfaces; f++) {
        int el = hm.face_elem[f], lf = hm.face_local[f];
        double c[3] = {0, 0, 0}, q[3], d = 0;
        for (int a = 0; a < 3; a++)
            for (int k = 0; k < 3; k++) c[k] += hm.xyz[3 * (size_t)hm.conn[(size_t)npe * el + TET_FACE[lf][a]] + k] / 3;
        int tri = -1;
        bvh_closest(bvh, c, INFINITY, &tri, q, &d);
        int bi = hm.body[el];
        hm.face_tri[f] = tri >= 0 && tb[tri] == bi ? tri - toff[bi] : -1;
        hm.face_dist[f] = (float)d;
        dsum += d, dmax = fmax(dmax, d);
    }
    hm.mean_face_distance = hm.nfaces ? dsum / hm.nfaces : 0, hm.max_face_distance = dmax;
    hm.nbodies = nb;
    for (int i = 0; i < nb; i++) {
        hm.body_volume_stl[i] = fabs(bl[i]->diag.volume) * bl[i]->unit_scale * bl[i]->unit_scale * bl[i]->unit_scale;
        hm.body_volume_mesh[i] = 0;
    }
    for (int el = 0; el < ne; el++) {
        double X[10][3];
        for (int a = 0; a < npe; a++) memcpy(X[a], hm.xyz + 3 * (size_t)hm.conn[(size_t)npe * el + a], sizeof X[a]);
        int bi = hm.body[el];
        if (bi >= 0 && bi < 16) hm.body_volume_mesh[bi] += tet_volume(hm.elem_type, (const double (*)[3])X);
    }
    hm.h[0] = hm.h[1] = hm.h[2] = h;
    hm.regions = tm.regions;
    hm.uncertain_cells = g.votes_split;
    hm.seconds = tm.seconds;
    tet_stl_free(&g);
    free(V), free(T), free(tb), free(toff);
    char previous_hash[65];
    snprintf(previous_hash, sizeof previous_hash, "%s", stored ? proj->mesh.hash : "");
    mesh_state_free(&proj->mesh);
    MeshState *ms = &proj->mesh;
    ms->hm = hm;
    ms->tetq = tm; /* the numbers; its arrays moved into hm above */
    ms->valid = true;
    ms->has_settings = true;
    ms->method = 1, ms->tet_order = order, ms->tet_interior = isize, ms->tet_thin = thin;
    ms->h[0] = ms->h[1] = ms->h[2] = h;
    ms->include_plate = false;
    ms->nbodies = nb;
    for (int i = 0; i < nb; i++) {
        snprintf(ms->body_name[i], sizeof ms->body_name[i], "%s", bl[i]->name);
        ms->body_geom_revision[i] = bl[i]->geom_revision;
    }
    mesh_hash_tet(proj, ms->hash);
    engine_touch(e, "mesh_generate", "tetrahedral %s mesh: %d elements, %d nodes, surface %.4g mm, dihedral %.1f to %.1f degrees",
                 order == 2 ? "TET10" : "TET4", hm.nelems, hm.nnodes, 1e3 * h, tm.min_dihedral, tm.max_dihedral);
    ms->revision = proj->revision;
    JsonValue *v = json_object();
    json_set(v, "mesh", mesh_summary_json(proj));
    json_set_string(v, "element_size_source", stored && !ss ? "stored" : (autosize ? "auto" : "user"));
    if (stored && previous_hash[0] && !ss) json_set_bool(v, "reproduces_stored_mesh", !strcmp(previous_hash, ms->hash));
    if (autosize)
        json_set_string(v, "element_size_rule",
                        "surface_size a quarter of the smallest bounding-box dimension, coarsened to stay near 30000 cells of that size in the part's "
                        "volume: a numerical choice to verify by refinement, not a physical assumption");
    mesh_warnings(proj, json_set_array(v, "warnings"));
    json_set_string(v, "resolution_dependence",
                    "Stresses at fillets and holes converge with the surface size; repeat the analysis with half the surface_size to check the "
                    "quantities of interest. Sharp edges are rounded at the surface cell scale, which bounds the stress there instead of letting "
                    "it grow without limit.");
    op_succeed(out, v);
}

static void op_mesh_generate(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    HexMeshBody hb[MESH_MAX_BODIES];
    Body *bl[MESH_MAX_BODIES];
    int nb = 0;
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY}, vol = 0;
    for (int i = 0; i < proj->nbodies; i++) {
        Body *b = proj->bodies[i];
        if (b->role == ROLE_BUILD_PLATE) continue;
        if (nb == MESH_MAX_BODIES) {
            op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "at most %d bodies can be meshed together", MESH_MAX_BODIES);
            return;
        }
        /* A surface that is not closed is meshed, not refused: the inside test votes over three axes, so a gap has to
         * line up with two of them before it can change a cell. How many cells the axes disagreed on is reported, and
         * hexmesh_generate refuses when that fraction is too large to call the result a mesh of this part. */
        if (!b->diag.closed_solid && b->diag.open_edges + b->diag.nonmanifold_edges == 0) {
            op_fail(out, NV_ERR_GEOMETRY_INVALID, "check the STL: it has no usable surface",
                    "body '%s' has no closed surface and no edges to repair", b->name);
            return;
        }
        hb[nb] = (HexMeshBody){b->build_v, b->surf.tri, b->surf.nt, b->build_normal, b->role == ROLE_SUPPORT ? HEX_REGION_SUPPORT : HEX_REGION_PART};
        bl[nb++] = b;
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], b->bmin[k]), hi[k] = fmax(hi[k], b->bmax[k]);
        vol += fabs(b->diag.volume) * b->unit_scale * b->unit_scale * b->unit_scale;
    }
    if (!nb) {
        op_fail(out, NV_ERR_PRECONDITION, "import a part with geometry_import", "the project has no part or support bodies to mesh");
        return;
    }
    {
        /* the method: given, or the stored one when the call carries no sizes (a project reopened) */
        bool sizes = json_get(p, "element_size") || json_get(p, "element_size_z") || json_get(p, "surface_size");
        const char *method = json_get_str(p, "method", !sizes && proj->mesh.has_settings && proj->mesh.method == 1 ? "tet" : "voxel");
        if (!strcmp(method, "tet")) {
            op_mesh_generate_tet(e, p, out, bl, nb, proj->mesh.has_settings && !sizes);
            return;
        }
        if (strcmp(method, "voxel")) {
            op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "method must be voxel or tet, not '%s'", method);
            return;
        }
    }
    double h[3];
    char err[512], previous_hash[65];
    const JsonValue *es = json_get(p, "element_size");
    /* without sizes, a project that stores mesh settings (e.g. after project_open) regenerates exactly that mesh */
    bool stored = !es && !json_get(p, "element_size_z") && proj->mesh.has_settings;
    bool autosize = (!es && !stored) || (es && json_str(es) && !strcmp(json_str(es), "auto"));
    snprintf(previous_hash, sizeof previous_hash, "%s", stored ? proj->mesh.hash : "");
    if (stored) {
        memcpy(h, proj->mesh.h, sizeof h);
    } else if (autosize) {
        double dmin = fmin(hi[0] - lo[0], fmin(hi[1] - lo[1], hi[2] - lo[2]));
        double hh = dmin / 4;
        if (vol / (hh * hh * hh) > 150000) hh = cbrt(vol / 150000);
        hh = nice_size(hh);
        h[0] = h[1] = h[2] = hh;
    } else {
        if (!quantity_from_json(es, DIM_LENGTH, "mm", &h[0], err, sizeof err)) {
            op_fail(out, NV_ERR_INVALID_UNIT, NULL, "element_size: %s", err);
            return;
        }
        h[1] = h[2] = h[0];
    }
    const JsonValue *ez = json_get(p, "element_size_z");
    if (ez && !stored && !quantity_from_json(ez, DIM_LENGTH, "mm", &h[2], err, sizeof err)) {
        op_fail(out, NV_ERR_INVALID_UNIT, NULL, "element_size_z: %s", err);
        return;
    }
    for (int k = 0; k < 3; k++)
        if (!(h[k] > 0)) {
            op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "element sizes must be positive");
            return;
        }
    bool plate = stored ? proj->mesh.include_plate : json_get_bool(p, "include_build_plate", false);
    double pt = stored ? proj->mesh.plate_thickness : 0, pm = stored ? proj->mesh.plate_margin : 0;
    if (!stored && (!op_quantity(out, p, "plate_thickness", DIM_LENGTH, "mm", &pt, NULL) || !op_quantity(out, p, "plate_margin", DIM_LENGTH, "mm", &pm, NULL)))
        return;
    if (plate && !(pt > 0)) {
        op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "plate_thickness must be positive");
        return;
    }
    long long maxe = json_get_int(p, "max_elements", 400000);
    if ((uint64_t)maxe > e->cfg.max_elements) maxe = (long long)e->cfg.max_elements;
    double estimate = vol / (h[0] * h[1] * h[2]);
    if (estimate > (double)maxe) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, "increase element_size, or raise max_elements within the server limit",
                "element size %.4g mm gives about %.0f part elements; the limit is %lld", 1e3 * h[0], estimate, maxe);
        op_fail_detail(out, "server_max_elements", json_number((double)e->cfg.max_elements));
        return;
    }
    HexMeshSettings st = {{h[0], h[1], h[2]}, plate, pt, pm, (uint64_t)maxe, NULL};
    int threads = cpu_perf_count();
    ThreadPool *pool = pool_create(threads > 0 ? threads : 1);
    st.pool = pool;
    HexMesh hm;
    bool ok = hexmesh_generate(hb, nb, &st, &hm, err, sizeof err);
    if (pool) pool_destroy(pool);
    if (!ok) {
        NvErr code = strstr(err, "limit") ? NV_ERR_RESOURCE_LIMIT : (strstr(err, "larger than") ? NV_ERR_OUT_OF_RANGE : NV_ERR_MESH_INVALID);
        op_fail(out, code, "choose an element size well below the part size and large enough to stay within the element limit", "mesh generation failed: %s", err);
        return;
    }
    mesh_state_free(&proj->mesh);
    MeshState *ms = &proj->mesh;
    ms->hm = hm;
    ms->valid = true;
    ms->has_settings = true;
    ms->method = 0;
    memcpy(ms->h, h, sizeof h);
    ms->include_plate = plate;
    ms->plate_thickness = pt;
    ms->plate_margin = pm;
    ms->nbodies = nb;
    for (int i = 0; i < nb; i++) {
        snprintf(ms->body_name[i], sizeof ms->body_name[i], "%s", bl[i]->name);
        ms->body_geom_revision[i] = bl[i]->geom_revision;
    }
    mesh_hash(proj, ms->hash);
    engine_touch(e, "mesh_generate", "voxel hex8 mesh: %d elements, %d nodes, %.4g x %.4g x %.4g mm", hm.nelems, hm.nnodes, 1e3 * h[0], 1e3 * h[1], 1e3 * h[2]);
    ms->revision = proj->revision;
    JsonValue *v = json_object();
    json_set(v, "mesh", mesh_summary_json(proj));
    json_set_string(v, "element_size_source", stored ? "stored" : (autosize ? "auto" : "user"));
    if (stored && previous_hash[0]) json_set_bool(v, "reproduces_stored_mesh", !strcmp(previous_hash, ms->hash));
    if (autosize)
        json_set_string(v, "element_size_rule",
                        "a quarter of the smallest bounding-box dimension, coarsened to stay near 150000 elements: a numerical choice to verify by "
                        "refinement, not a physical assumption");
    mesh_warnings(proj, json_set_array(v, "warnings"));
    json_set_string(v, "resolution_dependence",
                    "Volume, surface area and stresses at inclined or curved faces change with the element size. Repeat the analysis with half the "
                    "element size to check that the quantities of interest have converged.");
    op_succeed(out, v);
}

static void op_mesh_inspect(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    JsonValue *v = json_object();
    char why[300];
    bool current = project_mesh_current_any(proj, why, sizeof why);
    json_set_bool(v, "generated", proj->mesh.valid);
    json_set_bool(v, "current", current);
    if (!current) json_set_string(v, "reason", why);
    if (proj->mesh.has_settings) {
        JsonValue *s = json_set_object(v, "settings");
        json_set(s, "element_size_mm", json_vec3(1e3 * proj->mesh.h[0], 1e3 * proj->mesh.h[1], 1e3 * proj->mesh.h[2]));
        json_set_bool(s, "include_build_plate", proj->mesh.include_plate);
        json_set_number(s, "plate_thickness_mm", 1e3 * proj->mesh.plate_thickness);
        json_set_number(s, "plate_margin_mm", 1e3 * proj->mesh.plate_margin);
    }
    if (proj->mesh.valid) {
        json_set(v, "mesh", mesh_summary_json(proj));
        mesh_warnings(proj, json_set_array(v, "warnings"));
    }
    if (json_get_bool(p, "render", false)) {
        if (!proj->mesh.valid) {
            json_free(v);
            op_fail(out, NV_ERR_PRECONDITION, "run mesh_generate first", "there is no mesh to render");
            return;
        }
        ViewOptions o;
        if (!op_parse_view(out, p, &o)) {
            json_free(v);
            return;
        }
        o.show_mesh = true;
        o.label_patches = false;
        if (!o.title[0] && proj->mesh.hm.elem_type)
            snprintf(o.title, sizeof o.title, "tetrahedral mesh (%s): %d elements, surface %.3g mm, dihedral %.1f to %.1f deg",
                     proj->mesh.tet_order == 2 ? "TET10" : "TET4", proj->mesh.hm.nelems, 1e3 * proj->mesh.h[0], proj->mesh.tetq.min_dihedral,
                     proj->mesh.tetq.max_dihedral);
        if (!o.title[0])
            snprintf(o.title, sizeof o.title, "voxel mesh: %d elements, %.3g mm", proj->mesh.hm.nelems, 1e3 * proj->mesh.hm.h[0]);
        ViewResult vr;
        char err[512];
        if (!view_render_project(proj, &o, &vr, err, sizeof err)) {
            json_free(v);
            op_fail(out, NV_ERR_PRECONDITION, NULL, "%s", err);
            return;
        }
        op_finish_view(e, out, &vr, "mesh", NULL, "mesh.png", v);
    }
    op_succeed(out, v);
}

const OpBinding OPS_MESH_BINDINGS[] = {
    {"mesh_generate", op_mesh_generate},
    {"mesh_inspect", op_mesh_inspect},
    {NULL, NULL},
};
