/* ops_surface.c - surface patches and named selections */
#include "ops_internal.h"
#include "selection.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *facing_of(const double n[3]) { return n[2] >= 0.866 ? "up" : (n[2] <= -0.866 ? "down" : "side"); }

static const char *dominant_axis(const double n[3]) {
    int k = 0;
    for (int i = 1; i < 3; i++)
        if (fabs(n[i]) > fabs(n[k])) k = i;
    static const char *POS[3] = {"+x", "+y", "+z"}, *NEG[3] = {"-x", "-y", "-z"};
    return n[k] >= 0 ? POS[k] : NEG[k];
}

static JsonValue *patch_json(const Body *b, int pid, const PatchGeom *g) {
    JsonValue *o = json_object();
    double s2 = b->unit_scale * b->unit_scale;
    json_set_int(o, "id", pid);
    json_set_string(o, "type", patch_type_name(g->type));
    json_set_number(o, "area_mm2", g->area * s2 * 1e6);
    json_set(o, "centroid_mm", json_vec3(1e3 * g->centroid[0], 1e3 * g->centroid[1], 1e3 * g->centroid[2]));
    json_set(o, "normal", json_vec3(round(g->normal[0] * 1e6) / 1e6, round(g->normal[1] * 1e6) / 1e6, round(g->normal[2] * 1e6) / 1e6));
    json_set_string(o, "facing", facing_of(g->normal));
    json_set_string(o, "dominant_axis", dominant_axis(g->normal));
    JsonValue *bd = json_set_object(o, "bounds_mm");
    json_set(bd, "min", json_vec3(1e3 * g->bmin[0], 1e3 * g->bmin[1], 1e3 * g->bmin[2]));
    json_set(bd, "max", json_vec3(1e3 * g->bmax[0], 1e3 * g->bmax[1], 1e3 * g->bmax[2]));
    json_set_int(o, "triangles", g->ntri);
    JsonValue *adj = json_set_array(o, "adjacent_patches");
    for (int k = b->patches.adj_start[pid]; k < b->patches.adj_start[pid + 1] && k - b->patches.adj_start[pid] < 32; k++) json_push(adj, json_number(b->patches.adj[k]));
    if (g->type == PATCH_PLANAR) {
        json_set_number(o, "plane_offset_mm", 1e3 * g->plane_d);
        json_set_number(o, "flatness_rms_mm", 1e3 * g->fit_rms);
    } else {
        json_set_number(o, "normal_spread_deg", g->normal_spread_deg);
        if (g->type == PATCH_CYLINDRICAL) {
            json_set_number(o, "radius_mm", 1e3 * g->radius);
            json_set(o, "axis", json_vec3(g->axis[0], g->axis[1], g->axis[2]));
            json_set(o, "axis_point_mm", json_vec3(1e3 * g->axis_point[0], 1e3 * g->axis_point[1], 1e3 * g->axis_point[2]));
            json_set_string(o, "kind", g->concave ? "hole (concave)" : "boss (convex)");
        }
    }
    double size = fmax(b->bmax[0] - b->bmin[0], fmax(b->bmax[1] - b->bmin[1], b->bmax[2] - b->bmin[2]));
    double tol = 1e-5 + 1e-6 * size;
    JsonValue *tags = json_set_array(o, "tags");
    bool down = g->normal[2] <= -0.866, up = g->normal[2] >= 0.866;
    bool bottom = down && g->bmax[2] <= b->bmin[2] + tol;
    if (bottom) json_push(tags, json_string("bottom"));
    if (bottom && b->bmin[2] <= 1e-9) json_push(tags, json_string("on_build_plate"));
    if (up && g->bmin[2] >= b->bmax[2] - tol) json_push(tags, json_string("top"));
    if (g->normal[2] < -0.707 && !bottom) json_push(tags, json_string("overhang_below_45deg"));
    return o;
}

typedef struct {
    int id;
    double key;
} SortRow;

static int cmp_row_desc(const void *a, const void *b) {
    const SortRow *x = a, *y = b;
    if (x->key != y->key) return x->key > y->key ? -1 : 1;
    return x->id - y->id;
}

static void op_surfaces_list(Engine *e, JsonValue *p, OpResult *out) {
    Body *b = op_need_body(e, p, "body", out);
    if (!b) return;
    const char *type = json_get_str(p, "type", "any"), *facing = json_get_str(p, "facing", "any"), *sort = json_get_str(p, "sort", "area");
    int limit = (int)json_get_int(p, "limit", 60);
    double min_area = 1e-6 * json_get_num(p, "min_area_mm2", 0);
    double s2 = b->unit_scale * b->unit_scale;
    int n = b->patches.n;
    PatchGeom *geo = malloc((size_t)(n ? n : 1) * sizeof(PatchGeom));
    SortRow *rows = malloc((size_t)(n ? n : 1) * sizeof(SortRow));
    if (!geo || !rows) {
        free(geo), free(rows);
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        patch_geometry(&b->surf, b->build_v, b->place.R, &b->patches, i, &geo[i]);
        if (strcmp(type, "any") && strcmp(type, patch_type_name(geo[i].type))) continue;
        if (strcmp(facing, "any") && strcmp(facing, facing_of(geo[i].normal))) continue;
        if (geo[i].area * s2 < min_area) continue;
        double key = geo[i].area;
        if (!strcmp(sort, "z_max")) key = geo[i].bmax[2];
        else if (!strcmp(sort, "z_min")) key = -geo[i].bmin[2];
        else if (!strcmp(sort, "id")) key = -i;
        rows[m++] = (SortRow){i, key};
    }
    qsort(rows, (size_t)m, sizeof *rows, cmp_row_desc);
    JsonValue *v = json_object();
    json_set_string(v, "body", b->name);
    json_set_int(v, "patches_total", n);
    json_set_int(v, "patches_matching", m);
    json_set_number(v, "planar_tolerance_deg", b->patches.planar_tol_deg);
    json_set_number(v, "feature_angle_deg", b->patches.feature_angle_deg);
    json_set_string(v, "frame", "build frame, mm; +z is the build direction, z = 0 the plate top");
    JsonValue *arr = json_set_array(v, "patches");
    for (int i = 0; i < m && i < limit; i++) json_push(arr, patch_json(b, rows[i].id, &geo[rows[i].id]));
    json_set_bool(v, "truncated", m > limit);
    json_set_string(v, "hint",
                    "Use patch ids, or geometric predicates (facing, plane, extreme, box, near, cylinder), in selection_create. view_render labels patch ids on "
                    "an image so the user's words or an annotated view can be matched to faces.");
    free(geo), free(rows);
    op_succeed(out, v);
}

static bool query_has_pick(const JsonValue *q) {
    if (!q) return false;
    if (q->type == JSON_OBJECT) {
        for (size_t i = 0; i < json_len(q); i++) {
            if (!strcmp(json_key_at(q, i), "pick")) return true;
            if (query_has_pick(json_value_at(q, i))) return true;
        }
    } else if (q->type == JSON_ARRAY) {
        for (size_t i = 0; i < json_len(q); i++)
            if (query_has_pick(json_at(q, i))) return true;
    }
    return false;
}

static void op_selection_create(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    const char *name = json_get_str(p, "name", "");
    if (!body_name_valid(name)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "use 1-63 letters, digits, '_', '-' or '.'", "invalid selection name '%s'", name);
        return;
    }
    Body *b = op_need_body(e, p, "body", out);
    if (!b) return;
    Selection *existing = project_selection(e->proj, name);
    if (existing && !json_get_bool(p, "replace", false)) {
        op_fail(out, NV_ERR_ALREADY_EXISTS, "pass replace: true to redefine it, or choose another name", "a selection named '%s' already exists", name);
        return;
    }
    Selection tmp;
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.name, sizeof tmp.name, "%s", name);
    snprintf(tmp.body, sizeof tmp.body, "%s", b->name);
    tmp.query = json_clone(json_get(p, "query"));
    tmp.mode = strcmp(json_get_str(p, "mode", "patch"), "triangle") ? SEL_MODE_PATCH : SEL_MODE_TRIANGLE;
    tmp.coverage = json_get_num(p, "coverage", 0.5);
    tmp.keep_largest = (int)json_get_int(p, "keep_largest", 0);
    snprintf(tmp.description, sizeof tmp.description, "%s", json_get_str(p, "description", ""));
    tmp.source = (Provenance)provenance_from_name(json_get_str(p, "source", "inferred"));
    SelectionContext sctx = {engine_selection_pick, e};
    NvErr code;
    char err[768];
    if (!selection_resolve(b, &tmp, &sctx, &code, err, sizeof err)) {
        selection_free_data(&tmp);
        op_fail(out, code, code == NV_ERR_NOT_FOUND ? "inspect surfaces_list, loosen tolerances or angles, or render a labelled view" : NULL, "%s", err);
        return;
    }
    JsonValue *warnings = json_array();
    if (query_has_pick(tmp.query)) {
        /* views are transient: store the picked result in a form that reproduces it when the project is reopened */
        JsonValue *q = json_object();
        JsonValue *ids = json_set_array(q, tmp.mode == SEL_MODE_PATCH ? "patches" : "triangles");
        if (tmp.mode == SEL_MODE_PATCH)
            for (int i = 0; i < tmp.npatches; i++) json_push(ids, json_number(tmp.patches[i]));
        else
            for (int i = 0; i < tmp.ntri; i++) json_push(ids, json_number(tmp.tris[i]));
        json_free(tmp.query);
        tmp.query = q;
        json_push(warnings, json_string("picked faces are stored by id so the selection reproduces without the view"));
    }
    double spread = 0;
    for (int i = 0; i < tmp.ntri; i++) {
        const double *n = b->build_normal + 3 * (size_t)tmp.tris[i];
        double c = n[0] * tmp.normal[0] + n[1] * tmp.normal[1] + n[2] * tmp.normal[2];
        spread = fmax(spread, acos(fmax(-1.0, fmin(1.0, c))) * 180.0 / M_PI);
    }
    if (spread > 45)
        json_push(warnings, json_stringf("the selected faces point in different directions (up to %.0f deg from their mean normal); directions defined as "
                                         "'normal to the surface' are ambiguous on it",
                                         spread));
    if (tmp.source != PROV_USER && !tmp.description[0])
        json_push(warnings, json_string("describe how this selection interprets the user's words in 'description'; it is recorded with the setup"));
    bool hash_changed = existing && strcmp(existing->set_hash, tmp.set_hash) != 0;
    if (existing) {
        selection_free_data(existing);
        *existing = tmp;
    } else {
        Selection *s = project_add_selection(e->proj);
        if (!s) {
            selection_free_data(&tmp);
            json_free(warnings);
            op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
            return;
        }
        *s = tmp;
    }
    Selection *sel = project_selection(e->proj, name);
    if (hash_changed)
        for (int i = 0; i < e->proj->nbcs; i++)
            if (!strcmp(e->proj->bcs[i].selection, name))
                json_push(warnings, json_stringf("boundary condition '%s' refers to the previous faces of '%s'; re-apply it with boundary_apply", e->proj->bcs[i].name,
                                                 name));
    engine_touch(e, "selection_create", "%s selection %s on %s: %d patches, %.4g mm2", existing ? "redefined" : "created", name, b->name, sel->npatches,
                 sel->area * 1e6);
    JsonValue *v = json_object();
    json_set(v, "selection", selection_json(e->proj, sel, true));
    json_set(v, "warnings", warnings);
    if (json_get_bool(p, "preview", false)) {
        ViewOptions o;
        memset(&o, 0, sizeof o);
        snprintf(o.preset, sizeof o.preset, "iso");
        o.fov_deg = 30, o.width = 900, o.height = 650, o.label_patches = true, o.show_plate = true;
        o.highlight[0] = sel;
        o.nhighlight = 1;
        snprintf(o.title, sizeof o.title, "selection '%s' on %s", name, b->name);
        ViewResult vr;
        if (view_render_project(e->proj, &o, &vr, err, sizeof err)) op_finish_view(e, out, &vr, "geometry", NULL, "selection.png", v);
    }
    op_succeed(out, v);
}

static void op_selection_list(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    const char *body = json_get_str(p, "body", NULL);
    JsonValue *v = json_object();
    JsonValue *arr = json_set_array(v, "selections");
    for (int i = 0; i < e->proj->nselections; i++) {
        const Selection *s = &e->proj->selections[i];
        if (body && strcmp(body, s->body)) continue;
        json_push(arr, selection_json(e->proj, s, json_get_bool(p, "detailed", false)));
    }
    op_succeed(out, v);
}

static void op_selection_delete(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    const char *name = json_get_str(p, "name", "");
    if (!project_selection(e->proj, name)) {
        op_fail(out, NV_ERR_NOT_FOUND, "list selections with selection_list", "no selection named '%s'", name);
        return;
    }
    bool cascade = json_get_bool(p, "cascade", false);
    JsonValue *users = json_array();
    for (int i = 0; i < e->proj->nbcs; i++)
        if (!strcmp(e->proj->bcs[i].selection, name)) json_push(users, json_string(e->proj->bcs[i].name));
    if (json_len(users) && !cascade) {
        op_fail(out, NV_ERR_PRECONDITION, "remove those boundary conditions first, or pass cascade: true to remove them too",
                "selection '%s' is used by %zu boundary conditions", name, json_len(users));
        op_fail_detail(out, "boundary_conditions", users);
        return;
    }
    for (size_t i = 0; i < json_len(users); i++) project_remove_bc(e->proj, json_str(json_at(users, i)));
    project_remove_selection(e->proj, name);
    engine_touch(e, "selection_delete", "deleted selection %s%s", name, json_len(users) ? " and its boundary conditions" : "");
    JsonValue *v = json_object();
    json_set_string(v, "deleted", name);
    json_set(v, "removed_boundary_conditions", users);
    op_succeed(out, v);
}

static void op_selection_preview(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    ViewOptions o;
    if (!op_parse_view(out, p, &o)) return;
    const JsonValue *names = json_get(p, "names");
    JsonValue *v = json_object();
    JsonValue *list = json_set_array(v, "selections");
    for (size_t i = 0; i < json_len(names); i++) {
        const char *name = json_str(json_at(names, i));
        Selection *s = project_selection(e->proj, name);
        if (!s) {
            json_free(v);
            op_fail(out, NV_ERR_NOT_FOUND, "list selections with selection_list", "no selection named '%s'", name ? name : "");
            return;
        }
        o.highlight[o.nhighlight++] = s;
        json_push(list, selection_json(e->proj, s, false));
    }
    if (o.show_mesh && !e->proj->mesh.valid) {
        json_free(v);
        op_fail(out, NV_ERR_PRECONDITION, "run mesh_generate first, or omit show_mesh", "there is no mesh to show");
        return;
    }
    if (!o.title[0]) snprintf(o.title, sizeof o.title, "%s", o.nhighlight == 1 ? o.highlight[0]->name : "selections");
    ViewResult vr;
    char err[512];
    if (!view_render_project(e->proj, &o, &vr, err, sizeof err)) {
        json_free(v);
        op_fail(out, NV_ERR_PRECONDITION, NULL, "%s", err);
        return;
    }
    op_finish_view(e, out, &vr, o.show_mesh ? "mesh" : "geometry", NULL, "selection_preview.png", v);
    op_succeed(out, v);
}

/* A ray cast against the analysis surface, for an application that draws the part itself: the 3D view sends the ray
 * under the cursor and gets back the patch it hit, which is what a selection is made of. The rendered-view pick
 * (view_pick, selection query "pick") answers the same question for an image; this one needs no image. */
static void op_geometry_pick(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    double o[3], d[3];
    if (!json_get_numbers(json_get(p, "origin_mm"), o, 3) || !json_get_numbers(json_get(p, "direction"), d, 3)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "origin_mm and direction are three numbers each, in the build frame",
                "a ray needs an origin and a direction");
        return;
    }
    double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 0)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "give a direction with a non-zero length", "the ray direction is zero");
        return;
    }
    for (int i = 0; i < 3; i++) o[i] *= 1e-3, d[i] /= len; /* the operations speak millimetres, the geometry metres */
    int bi = -1;
    BvhHit h;
    JsonValue *v = json_object();
    if (!engine_ray_hit(e, o, d, &bi, &h)) {
        json_set_bool(v, "hit", false);
        json_set_string(v, "message", "the ray passes beside the geometry");
        op_succeed(out, v);
        return;
    }
    Body *b = e->proj->bodies[bi];
    json_set_bool(v, "hit", true);
    json_set_string(v, "body", b->name);
    json_set_int(v, "triangle", h.tri);
    json_set_number(v, "distance_mm", h.t * 1e3);
    json_set(v, "point_mm", json_vec3(1e3 * (o[0] + h.t * d[0]), 1e3 * (o[1] + h.t * d[1]), 1e3 * (o[2] + h.t * d[2])));
    if (b->build_normal && h.tri >= 0 && h.tri < b->surf.nt)
        json_set(v, "triangle_normal", json_vec3(b->build_normal[3 * (size_t)h.tri], b->build_normal[3 * (size_t)h.tri + 1],
                                                 b->build_normal[3 * (size_t)h.tri + 2]));
    int pid = (b->patches.tri_patch && h.tri >= 0 && h.tri < b->surf.nt) ? b->patches.tri_patch[h.tri] : -1;
    json_set_int(v, "patch", pid);
    if (pid >= 0 && pid < b->patches.n) {
        PatchGeom g;
        patch_geometry(&b->surf, b->build_v, b->place.R, &b->patches, pid, &g);
        JsonValue *po = json_set_object(v, "patch_info");
        double s2 = b->unit_scale * b->unit_scale;
        json_set_int(po, "id", pid);
        json_set_string(po, "type", patch_type_name(g.type));
        json_set_number(po, "area_mm2", g.area * s2 * 1e6);
        json_set_int(po, "triangles", b->patches.start[pid + 1] - b->patches.start[pid]);
        json_set(po, "normal", json_vec3(g.normal[0], g.normal[1], g.normal[2]));
        json_set_string(po, "facing", facing_of(g.normal));
        json_set(po, "centroid_mm", json_vec3(1e3 * g.centroid[0], 1e3 * g.centroid[1], 1e3 * g.centroid[2]));
    }
    op_succeed(out, v);
}

const OpBinding OPS_SURFACE_BINDINGS[] = {
    {"surfaces_list", op_surfaces_list},
    {"geometry_pick", op_geometry_pick},
    {"selection_create", op_selection_create},
    {"selection_list", op_selection_list},
    {"selection_delete", op_selection_delete},
    {"selection_preview", op_selection_preview},
    {NULL, NULL},
};
