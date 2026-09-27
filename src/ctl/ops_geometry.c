/* ops_geometry.c - STL import, build placement and geometry diagnostics */
#include "../geom/repair.h"
#include "ops_internal.h"
#include "../core/sha256.h"
#include "../geom/bvh.h"
#include "selection.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void default_body_name(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path_basename(path));
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
    path_sanitize_name(out);
    if (strlen(out) > 63) out[63] = 0;
    if (!out[0] || out[0] == '.') snprintf(out, cap, "part");
}

/* an angle given as a plain number is already in degrees; strings go through the unit parser */
static bool angle_deg(OpResult *out, const JsonValue *v, const char *what, double *deg) {
    if (v && v->type == JSON_NUMBER) {
        *deg = v->u.number;
        return true;
    }
    double rad;
    char err[256];
    if (!quantity_from_json(v, DIM_ANGLE, "deg", &rad, err, sizeof err)) {
        op_fail(out, NV_ERR_INVALID_UNIT, NULL, "%s: %s", what, err);
        return false;
    }
    *deg = rad * 180.0 / M_PI;
    return true;
}

static bool length_m(OpResult *out, const JsonValue *v, const char *what, double *m) {
    char err[256];
    if (!quantity_from_json(v, DIM_LENGTH, "mm", m, err, sizeof err)) {
        op_fail(out, NV_ERR_INVALID_UNIT, NULL, "%s: %s", what, err);
        return false;
    }
    return true;
}

static void note_up_axis(Project *proj, const Body *b) {
    char subj[96];
    snprintf(subj, sizeof subj, "bodies/%s/up_axis", b->name);
    if (b->place.up_source != PROV_USER)
        project_note_assumption(proj, subj, b->place.up_source, "file axis %s of body '%s' is taken as the build direction (+Z)", b->place.up_axis, b->name);
    else
        project_clear_assumptions(proj, subj);
}

static void op_geometry_import(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    const char *path = json_get_str(p, "path", "");
    char src[NV_PATH_MAX], err[1024];
    if (!path_real(path, src, sizeof src)) {
        op_fail(out, NV_ERR_NOT_FOUND, "check the path; relative paths are resolved against the server's working directory", "file not found: %s", path);
        return;
    }
    if (!engine_resolve_read_path(e, path, src, sizeof src, err, sizeof err)) {
        op_fail(out, NV_ERR_PERMISSION, NULL, "%s", err);
        return;
    }
    if (!path_is_file(src)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "'%s' is not a regular file", src);
        return;
    }
    long long size = path_file_size(src);
    if (size < 0 || (uint64_t)size > e->cfg.max_stl_bytes) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, "the server operator can raise the limit (--max-stl-bytes)", "'%s' is %lld bytes; the limit is %llu",
                src, size, (unsigned long long)e->cfg.max_stl_bytes);
        return;
    }
    const char *units = json_get_str(p, "units", "mm");
    Provenance usrc = (Provenance)provenance_from_name(json_get_str(p, "units_source", "user"));
    const char *note = json_get_str(p, "units_note", "");
    char name[64];
    const char *given = json_get_str(p, "name", NULL);
    if (given) {
        if (!body_name_valid(given)) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "use 1-63 letters, digits, '_', '-' or '.'", "invalid body name '%s'", given);
            return;
        }
        snprintf(name, sizeof name, "%s", given);
    } else {
        default_body_name(src, name, sizeof name);
    }
    Body *existing = project_body(proj, name);
    bool replace = json_get_bool(p, "replace", false);
    if (existing && !replace) {
        op_fail(out, NV_ERR_ALREADY_EXISTS, "pass replace: true to replace it, or give another name", "the project already has a body named '%s'", name);
        return;
    }
    SurfaceRepairOptions ro;
    surface_repair_defaults(&ro);
    const JsonValue *rep = json_get(p, "repair");
    ro.remove_degenerate = json_get_bool(rep, "remove_degenerate", true);
    ro.remove_duplicate_faces = json_get_bool(rep, "remove_duplicate_faces", true);
    ro.orient_outward = json_get_bool(rep, "orient_outward", true);

    /* copy into the project first and analyse the copy, so the hash, the stored file and the geometry agree */
    char inputs[NV_PATH_MAX];
    if (!path_join(inputs, sizeof inputs, proj->dir, "inputs") ||
        !engine_resolve_write_path(e, NULL, inputs, true, true, inputs, sizeof inputs, err, sizeof err)) {
        op_fail(out, NV_ERR_PERMISSION, NULL, "cannot write the project inputs folder: %s", err);
        return;
    }
    static atomic_uint counter; /* several engines (a study's designs) may import in one process */
    char tmp[NV_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/.import-%ld-%u.stl", inputs, (long)getpid(), (unsigned)atomic_fetch_add(&counter, 1) + 1);
    if (!path_copy_file(src, tmp, err, sizeof err)) {
        op_fail(out, NV_ERR_IO, NULL, "%s", err);
        return;
    }
    char sha[65];
    uint64_t bytes = 0;
    if (!sha256_file(tmp, sha, &bytes)) {
        remove(tmp);
        op_fail(out, NV_ERR_IO, NULL, "cannot hash the copied input");
        return;
    }
    char stored_rel[256], stored_abs[NV_PATH_MAX];
    snprintf(stored_rel, sizeof stored_rel, "inputs/%s-%.12s.stl", name, sha);
    path_join(stored_abs, sizeof stored_abs, proj->dir, stored_rel);
    bool existed = path_is_file(stored_abs);
    if (existed) {
        remove(tmp);
    } else if (rename(tmp, stored_abs) != 0) {
        remove(tmp);
        op_fail(out, NV_ERR_IO, NULL, "cannot store the input as %s", stored_abs);
        return;
    }
    Body *b = body_load(stored_abs, name, units, &ro, e->cfg.max_stl_bytes, e->cfg.max_triangles, err, sizeof err);
    if (!b) {
        if (!existed) remove(stored_abs);
        NvErr code = strstr(err, "limit") ? NV_ERR_RESOURCE_LIMIT : NV_ERR_GEOMETRY_INVALID;
        op_fail(out, code, "check that the file is a valid ASCII or binary STL", "cannot import '%s': %s", src, err);
        return;
    }
    snprintf(b->source_path, sizeof b->source_path, "%s", src);
    snprintf(b->stored_file, sizeof b->stored_file, "%s", stored_rel);
    int role = body_role_from_name(json_get_str(p, "role", "part"));
    b->role = role >= 0 ? (BodyRole)role : ROLE_PART;
    b->units_source = usrc;
    snprintf(b->units_note, sizeof b->units_note, "%s", note);
    snprintf(b->place.up_axis, sizeof b->place.up_axis, "%s", json_get_str(p, "up_axis", "+Z"));
    b->place.up_source = (Provenance)provenance_from_name(json_get_str(p, "up_axis_source", "default"));
    if (!placement_apply(b, err, sizeof err)) {
        body_free(b);
        op_fail(out, NV_ERR_INTERNAL, NULL, "%s", err);
        return;
    }
    if (existing) project_remove_body(proj, name);
    if (!project_add_body(proj, b)) {
        body_free(b);
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    if (existing) {
        SelectionContext sctx = {NULL, NULL};
        for (int i = 0; i < proj->nselections; i++)
            if (!strcmp(proj->selections[i].body, name)) selection_revalidate(b, &proj->selections[i], &sctx);
    }
    char subj[96];
    snprintf(subj, sizeof subj, "bodies/%s/units", name);
    if (usrc != PROV_USER)
        project_note_assumption(proj, subj, usrc, "coordinates in the file of body '%s' are taken to be in %s%s%s", name, units, note[0] ? ": " : "", note);
    else
        project_clear_assumptions(proj, subj);
    note_up_axis(proj, b);
    engine_touch(e, "geometry_import", "imported body %s from %s (%d triangles, unit %s)", name, path_basename(src), b->surf.nt, units);

    JsonValue *v = json_object();
    json_set(v, "body", body_json(b, true));
    json_set(v, "diagnostics", body_diagnostics_json(b));
    json_set(v, "units_check", units_plausibility_json(b));
    JsonValue *warn = json_set_array(v, "warnings");
    if (usrc != PROV_USER && !note[0])
        json_push(warn, json_stringf("units_source is '%s' but units_note is empty: record why %s was chosen", provenance_name(usrc), units));
    if (existing) json_push(warn, json_stringf("replaced the previous body '%s'", name));
    json_set(v, "assumptions", assumptions_json(proj));
    op_succeed(out, v);
}

static void op_geometry_place(Engine *e, JsonValue *p, OpResult *out) {
    Body *b = op_need_body(e, p, "body", out);
    if (!b) return;
    Placement old = b->place;
    const char *up = json_get_str(p, "up_axis", NULL);
    if (up) {
        snprintf(b->place.up_axis, sizeof b->place.up_axis, "%s", up);
        b->place.up_source = (Provenance)provenance_from_name(json_get_str(p, "up_axis_source", "user"));
    }
    const JsonValue *rz = json_get(p, "rotate_z");
    if (rz && !angle_deg(out, rz, "rotate_z", &b->place.rotate_z_deg)) goto restore;
    const JsonValue *rots = json_get(p, "rotations");
    if (rots) {
        b->place.nrot = 0;
        for (size_t i = 0; i < json_len(rots); i++) {
            const JsonValue *r = json_at(rots, i);
            if (!angle_deg(out, json_get(r, "angle"), "rotations[].angle", &b->place.rot_deg[i])) goto restore;
            b->place.rot_axis[i] = json_get_str(r, "axis", "z")[0];
            b->place.nrot++;
        }
    }
    const JsonValue *pos = json_get(p, "position");
    if (pos)
        for (size_t i = 0; i < 2; i++)
            if (!length_m(out, json_at(pos, i), "position", &b->place.position_m[i])) goto restore;
    const JsonValue *zo = json_get(p, "z_offset");
    if (zo) {
        if (!length_m(out, zo, "z_offset", &b->place.z_offset_m)) goto restore;
        if (b->place.z_offset_m < 0) {
            op_fail(out, NV_ERR_OUT_OF_RANGE, "a body cannot extend below the plate top (z = 0)", "z_offset must be >= 0 mm (got %g mm)", 1e3 * b->place.z_offset_m);
            goto restore;
        }
    }
    char err[256];
    if (!placement_apply(b, err, sizeof err)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "%s", err);
        goto restore;
    }
    b->geom_revision = ++e->proj->geom_counter;
    note_up_axis(e->proj, b);
    JsonValue *stale = json_array();
    SelectionContext sctx = {NULL, NULL};
    for (int i = 0; i < e->proj->nselections; i++) {
        Selection *s = &e->proj->selections[i];
        if (strcmp(s->body, b->name) != 0) continue;
        bool was = s->stale;
        selection_revalidate(b, s, &sctx);
        if (s->stale && !was) json_push(stale, json_stringf("%s: %s", s->name, s->stale_reason));
    }
    engine_touch(e, "geometry_place", "placed body %s: up %s, rotate_z %g deg, %d extra rotations, position (%g, %g) mm, z_offset %g mm", b->name,
                 b->place.up_axis, b->place.rotate_z_deg, b->place.nrot, 1e3 * b->place.position_m[0], 1e3 * b->place.position_m[1],
                 1e3 * b->place.z_offset_m);
    JsonValue *v = json_object();
    json_set(v, "body", body_json(b, true));
    json_set(v, "stale_selections", stale);
    char why[256];
    if (e->proj->mesh.valid && !project_mesh_current(e->proj, why, sizeof why)) json_set_string(v, "mesh", "stale: regenerate with mesh_generate");
    op_succeed(out, v);
    return;
restore:
    b->place = old;
    placement_apply(b, err, sizeof err);
}

static void op_geometry_diagnostics(Engine *e, JsonValue *p, OpResult *out) {
    Body *b = op_need_body(e, p, "body", out);
    if (!b) return;
    const char *si = json_get_str(p, "self_intersections", "auto"), *th = json_get_str(p, "thickness", "auto");
    bool run_si = !strcmp(si, "run") || (!strcmp(si, "auto") && b->surf.nt <= 500000);
    bool run_th = !strcmp(th, "run") || (!strcmp(th, "auto") && b->surf.nt <= 500000);
    if (run_si || run_th) {
        Bvh bvh;
        if (!bvh_build(&bvh, b->surf.v, b->surf.tri, b->surf.nt)) {
            op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory building the search tree");
            return;
        }
        if (run_si) surface_check_self_intersections(&b->surf, &bvh, 100000, &b->diag);
        if (run_th) surface_check_thickness(&b->surf, &bvh, (int)json_get_int(p, "thickness_samples", 4000), &b->diag);
        bvh_free(&bvh);
    }
    JsonValue *v = json_object();
    json_set(v, "body", body_json(b, false));
    json_set(v, "diagnostics", body_diagnostics_json(b));
    json_set(v, "units_check", units_plausibility_json(b));
    op_succeed(out, v);
}


/* geometry_repair - make a triangle soup into something the volume mesher can trust, counting every change.
 *
 * A repair changes the part, so it is never automatic and never silent: the caller asks for it, states how large a
 * hole may be filled and where that number comes from, and the answer lists every facet dropped and every facet
 * added, with the diagnostics before and after. The repaired surface is stored in the project like any other input,
 * so the part on screen is the part the numbers came from. */
static void op_geometry_repair(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    const char *name = json_get_str(p, "body", "");
    Body *b = project_body(proj, name[0] ? name : NULL);
    if (!b) {
        op_fail(out, NV_ERR_NOT_FOUND, "import a part with geometry_import", "no body named '%s'", name);
        return;
    }
    char bname[64];
    snprintf(bname, sizeof bname, "%s", b->name);

    MeshRepairOptions ro;
    mesh_repair_defaults(&ro);
    ro.keep_largest_component = json_get_bool(p, "keep_largest_shell", true);
    const JsonValue *hv = json_get(p, "max_hole_edges");
    if (hv) {
        long long n = json_get_int(p, "max_hole_edges", 64);
        if (n < 0 || n > 100000) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "0 fills no hole; 64 is the default", "max_hole_edges %lld is out of range", n);
            return;
        }
        ro.max_hole_edges = (int)n;
    }
    const char *prov = json_get_str(p, "provenance", "");
    int pv = provenance_from_name(prov);
    if (pv < 0) {
        op_fail(out, NV_ERR_PRECONDITION,
                "give provenance (user, inferred, default or calibrated): filling a hole invents material, so the "
                "number that says how big a hole may be has to come from somewhere",
                "the repair carries no provenance");
        return;
    }

    Mesh fixed;
    MeshRepairReport rr;
    char err[512];
    if (!mesh_repair(&b->file_mesh, &ro, &fixed, &rr, err, sizeof err)) {
        op_fail(out, NV_ERR_GEOMETRY_INVALID, NULL, "cannot repair '%s': %s", bname, err);
        return;
    }
    if (rr.triangles_out == 0) {
        mesh_free(&fixed);
        op_fail(out, NV_ERR_GEOMETRY_INVALID, NULL, "the repair of '%s' left no triangles", bname);
        return;
    }

    /* store the repaired surface beside the original and reload the body from it */
    char inputs[NV_PATH_MAX], stored_rel[256], stored_abs[NV_PATH_MAX];
    if (!path_join(inputs, sizeof inputs, proj->dir, "inputs") ||
        !engine_resolve_write_path(e, NULL, inputs, true, true, inputs, sizeof inputs, err, sizeof err)) {
        mesh_free(&fixed);
        op_fail(out, NV_ERR_PERMISSION, NULL, "cannot write the project inputs folder: %s", err);
        return;
    }
    snprintf(stored_rel, sizeof stored_rel, "inputs/%s-repaired.stl", bname);
    path_join(stored_abs, sizeof stored_abs, proj->dir, stored_rel);
    if (!mesh_save_stl(stored_abs, &fixed, err, sizeof err)) {
        mesh_free(&fixed);
        op_fail(out, NV_ERR_IO, NULL, "cannot store the repaired surface: %s", err);
        return;
    }
    mesh_free(&fixed);

    char units[8];
    snprintf(units, sizeof units, "%s", b->units);
    BodyRole role = b->role;
    Placement place = b->place;
    Provenance usrc = b->units_source;
    char note[512];
    snprintf(note, sizeof note, "%s", b->units_note);
    char source_path[NV_PATH_MAX];
    snprintf(source_path, sizeof source_path, "%s", b->source_path);
    SurfaceRepairOptions sro = b->repair;

    Body *nb = body_load(stored_abs, bname, units, &sro, e->cfg.max_stl_bytes, e->cfg.max_triangles, err, sizeof err);
    if (!nb) {
        op_fail(out, NV_ERR_GEOMETRY_INVALID, NULL, "the repaired surface of '%s' could not be read back: %s", bname, err);
        return;
    }
    snprintf(nb->source_path, sizeof nb->source_path, "%s", source_path);
    snprintf(nb->stored_file, sizeof nb->stored_file, "%s", stored_rel);
    nb->role = role;
    nb->place = place;
    nb->units_source = usrc;
    snprintf(nb->units_note, sizeof nb->units_note, "%s", note);
    if (!placement_apply(nb, err, sizeof err)) {
        body_free(nb);
        op_fail(out, NV_ERR_INTERNAL, NULL, "%s", err);
        return;
    }
    project_remove_body(proj, bname);
    if (!project_add_body(proj, nb)) {
        body_free(nb);
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    SelectionContext sctx = {NULL, NULL};
    for (int i = 0; i < proj->nselections; i++)
        if (!strcmp(proj->selections[i].body, bname)) selection_revalidate(nb, &proj->selections[i], &sctx);
    project_note_assumption(proj, "geometry/repair", (Provenance)pv,
                            "body '%s' was repaired: %u facets dropped with %d shell(s), %d hole(s) filled with %u "
                            "facets, at most %d boundary edges per hole",
                            bname, rr.triangles_dropped, rr.components_dropped, rr.holes_filled, rr.triangles_added,
                            ro.max_hole_edges);
    engine_touch(e, "geometry_repair", "repaired body %s (%u -> %u facets)", bname, rr.triangles_in, rr.triangles_out);

    JsonValue *v = json_object();
    JsonValue *ch = json_set_object(v, "changes");
    json_set_int(ch, "facets_before", rr.triangles_in);
    json_set_int(ch, "facets_after", rr.triangles_out);
    json_set_int(ch, "vertices_merged", rr.merged_vertices);
    json_set_int(ch, "degenerate_facets_removed", rr.degenerate_removed);
    json_set_int(ch, "duplicate_facets_removed", rr.duplicate_removed);
    json_set_int(ch, "shells_found", rr.components_found);
    json_set_int(ch, "shells_dropped", rr.components_dropped);
    json_set_int(ch, "facets_dropped", rr.triangles_dropped);
    json_set_number(ch, "dropped_area_mm2", 1e0 * rr.dropped_area);
    json_set_int(ch, "holes_filled", rr.holes_filled);
    json_set_int(ch, "facets_added", rr.triangles_added);
    json_set_int(ch, "holes_left_too_large", rr.holes_too_large);
    json_set_int(ch, "largest_hole_left_edges", rr.largest_hole_edges_left);
    json_set_string(ch, "provenance", provenance_name((Provenance)pv));
    JsonValue *bf = json_set_object(v, "before");
    json_set_int(bf, "open_edges", rr.open_edges_before);
    json_set_int(bf, "nonmanifold_edges", rr.nonmanifold_edges_before);
    json_set_bool(bf, "closed_solid", rr.closed_solid_before);
    json_set_number(bf, "area", rr.area_before);
    json_set_number(bf, "volume", rr.volume_before);
    JsonValue *af = json_set_object(v, "after");
    json_set_int(af, "open_edges", rr.open_edges_after);
    json_set_int(af, "nonmanifold_edges", rr.nonmanifold_edges_after);
    json_set_bool(af, "closed_solid", rr.closed_solid_after);
    json_set_number(af, "area", rr.area_after);
    json_set_number(af, "volume", rr.volume_after);
    json_set_string(v, "stored_file", stored_rel);
    json_set(v, "body", body_json(nb, true));
    json_set(v, "diagnostics", body_diagnostics_json(nb));
    json_set_string(v, "scope",
                    "a repair changes the part: the facets dropped and added are listed above, and the volume before "
                    "and after says how much of the part they were");
    op_succeed(out, v);
}

const OpBinding OPS_GEOMETRY_BINDINGS[] = {
    {"geometry_import", op_geometry_import},
    {"geometry_place", op_geometry_place},
    {"geometry_diagnostics", op_geometry_diagnostics},
    {"geometry_repair", op_geometry_repair},
    {NULL, NULL},
};
