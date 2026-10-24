/* study_design.c - one design on its private engine: project, geometry, placement, material, regions, conditions;
 * equivalence between designs; the coarsest-mesh check */
#include "project.h"
#include "study_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double norm3(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static double angle_deg(const double a[3], const double b[3]) {
    double na = norm3(a), nb = norm3(b);
    if (!(na > 0) || !(nb > 0)) return 180;
    double c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (na * nb);
    return acos(c > 1 ? 1 : (c < -1 ? -1 : c)) * 180.0 / 3.14159265358979323846;
}

static bool get3(const JsonValue *a, double out[3]) { return json_get_numbers(a, out, 3); }

void study_design_free(StudyDesign *d) {
    if (d->eng) engine_destroy(d->eng);
    json_free(d->inspection);
    free(d->preview_png);
    memset(d, 0, sizeof *d);
}

/* region queries are written in the design frame (mm); selections resolve in the build frame = design + t */
static JsonValue *translate_query(const JsonValue *q, const double t[3], char *err, size_t errlen) {
    if (!q || q->type != JSON_OBJECT || json_len(q) != 1) return json_clone(q);
    const char *key = json_key_at(q, 0);
    const JsonValue *val = json_value_at(q, 0);
    JsonValue *out = json_object();
    if (!strcmp(key, "all") || !strcmp(key, "any")) {
        JsonValue *arr = json_set_array(out, key);
        for (size_t i = 0; i < json_len(val); i++) {
            JsonValue *sub = translate_query(json_at(val, i), t, err, errlen);
            if (!sub) {
                json_free(out);
                return NULL;
            }
            json_push(arr, sub);
        }
        return out;
    }
    if (!strcmp(key, "not")) {
        JsonValue *sub = translate_query(val, t, err, errlen);
        if (!sub) {
            json_free(out);
            return NULL;
        }
        json_set(out, key, sub);
        return out;
    }
    if (!strcmp(key, "pick")) {
        json_free(out);
        snprintf(err, errlen, "a pick query refers to a rendered view of one session and cannot define a reproducible study region; use the patch ids or coordinates it resolved to");
        return NULL;
    }
    JsonValue *nv = json_clone(val);
    if (nv && nv->type == JSON_OBJECT) {
        static const char *const POINT_KEYS[] = {"min", "max", "center", "point", NULL};
        for (int k = 0; POINT_KEYS[k]; k++) {
            if (!strcmp(key, "plane") && strcmp(POINT_KEYS[k], "point") != 0) continue;
            if ((!strcmp(key, "box") && strcmp(POINT_KEYS[k], "min") && strcmp(POINT_KEYS[k], "max")) ||
                (!strcmp(key, "sphere") && strcmp(POINT_KEYS[k], "center")) || (!strcmp(key, "near") && strcmp(POINT_KEYS[k], "point")))
                continue;
            double p[3];
            if (get3(json_get(nv, POINT_KEYS[k]), p)) json_set(nv, POINT_KEYS[k], json_vec3(p[0] + t[0], p[1] + t[1], p[2] + t[2]));
        }
        if (!strcmp(key, "plane")) {
            const JsonValue *at = json_get(nv, "at");
            const char *axis = json_get_str(nv, "axis", "");
            int ax = !strcmp(axis, "x") ? 0 : (!strcmp(axis, "y") ? 1 : (!strcmp(axis, "z") ? 2 : -1));
            if (at && at->type == JSON_NUMBER && ax >= 0) json_set_number(nv, "at", at->u.number + t[ax]);
        }
    }
    json_set(out, key, nv);
    return out;
}

static JsonValue *design_mm(const double b[3], const double t[3]) { return json_vec3(b[0] - t[0], b[1] - t[1], b[2] - t[2]); }

/* selection_create of a region; the region summary (design frame) or NULL with a question */
static JsonValue *make_region(StudyDesign *d, StudyLog *lg, StudyIssues *is, const char *sel_name, const char *label, const JsonValue *region,
                              const JsonValue *patches, double *area, double c[3], double n[3]) {
    char err[600], qid[160];
    snprintf(qid, sizeof qid, "%s.%s_region", d->name, label);
    JsonValue *q = translate_query(json_get(region, "query"), d->t_mm, err, sizeof err);
    if (!q) {
        si_question(is, qid, true, false, "the study must resolve the same faces every time it is replayed", NULL, "Design '%s', %s region: %s", d->name,
                    label, err);
        return NULL;
    }
    JsonValue *p = json_object();
    json_set_string(p, "name", sel_name);
    json_set_string(p, "body", "part");
    json_set(p, "query", q);
    json_set_string(p, "mode", json_get_str(region, "mode", "patch"));
    json_set_number(p, "coverage", json_get_num(region, "coverage", 0.5));
    const char *desc = json_get_str(region, "description", "");
    json_set_string(p, "description", desc[0] ? desc : label);
    json_set_string(p, "source", study_project_source(json_get_str(region, "source", "user")));
    OpResult r;
    if (!study_op(d->eng, lg, "selection_create", p, &r)) {
        JsonValue *det = json_object();
        json_set(det, "query_design_frame", json_clone(json_get(region, "query")));
        study_op_error(&r, err, sizeof err);
        json_set_string(det, "error", err);
        si_question(is, qid, true, false, "a condition applied to the wrong faces, or to none, makes the comparison meaningless", det,
                    "In design '%s' the %s region query does not resolve (%s). Which faces are the %s region? surfaces_list and view_render on the "
                    "design's project show the available faces.",
                    d->name, label, json_get_str(r.error, "code", ""), label);
        op_result_free(&r);
        return NULL;
    }
    const JsonValue *sel = json_get(r.value, "selection") ? json_get(r.value, "selection") : r.value;
    double cb[3] = {0, 0, 0}, mn[3] = {0, 0, 0};
    get3(json_get(sel, "centroid_mm"), cb);
    get3(json_get(sel, "mean_normal"), mn);
    *area = json_get_num(sel, "area_mm2", 0);
    for (int k = 0; k < 3; k++) c[k] = cb[k] - d->t_mm[k], n[k] = mn[k];
    JsonValue *o = json_object();
    json_set_string(o, "selection", sel_name);
    json_set_string(o, "description", desc);
    json_set_string(o, "source", json_get_str(region, "source", "user"));
    json_set(o, "query_design_frame", json_clone(json_get(region, "query")));
    json_set_number(o, "area_mm2", *area);
    json_set(o, "centroid_design_mm", json_vec3(c[0], c[1], c[2]));
    json_set(o, "mean_normal", json_vec3(n[0], n[1], n[2]));
    json_set_int(o, "triangles", json_get_int(sel, "triangles", 0));
    json_set(o, "patches", json_clone(json_get(sel, "patches")));
    json_set_string(o, "set_hash", json_get_str(sel, "set_hash", ""));
    double bmin[3], bmax[3];
    if (get3(json_get(json_get(sel, "bounds_mm"), "min"), bmin) && get3(json_get(json_get(sel, "bounds_mm"), "max"), bmax)) {
        JsonValue *bo = json_set_object(o, "bounds_design_mm");
        json_set(bo, "min", design_mm(bmin, d->t_mm));
        json_set(bo, "max", design_mm(bmax, d->t_mm));
    }
    /* ambiguity: planar faces of the region that face the same way but lie in different planes (e.g. the back face of a
     * wall plate and the end of an arm) are rarely one mounting or load surface */
    const JsonValue *ids = json_get(sel, "patches");
    double lo = INFINITY, hi = -INFINITY;
    int planar = 0;
    double diag = norm3((double[3]){d->bbox_max[0] - d->bbox_min[0], d->bbox_max[1] - d->bbox_min[1], d->bbox_max[2] - d->bbox_min[2]});
    for (size_t i = 0; i < json_len(ids); i++) {
        const JsonValue *idv = json_at(ids, i);
        long long pid = idv && idv->type == JSON_NUMBER ? (long long)idv->u.number : json_get_int(idv, "id", -1);
        for (size_t k = 0; k < json_len(patches); k++) {
            const JsonValue *pt = json_at(patches, k);
            if (json_get_int(pt, "id", -2) != pid || strcmp(json_get_str(pt, "type", ""), "planar") != 0) continue;
            double pn[3], pc[3];
            if (!get3(json_get(pt, "normal"), pn) || !get3(json_get(pt, "centroid_mm"), pc) || angle_deg(pn, n) > 10) continue;
            double off = (pc[0] * n[0] + pc[1] * n[1] + pc[2] * n[2]) / (norm3(n) > 0 ? norm3(n) : 1);
            lo = fmin(lo, off), hi = fmax(hi, off);
            planar++;
        }
    }
    double spread = planar > 1 ? hi - lo : 0, tol = fmax(0.5, 0.005 * diag);
    json_set_number(o, "plane_spread_mm", spread);
    if (spread > tol) {
        char aid[180];
        snprintf(aid, sizeof aid, "%s.%s_region.ambiguous", d->name, label);
        JsonValue *det = json_object();
        json_set_number(det, "plane_spread_mm", spread);
        json_set(det, "patches", json_clone(ids));
        si_question(is, aid, true, true,
                    "faces in different planes are different physical surfaces; supporting or loading all of them may not be what the bracket experiences",
                    det,
                    "In design '%s' the %s region covers faces in parallel planes %.3g mm apart. Is that one %s surface (confirm by accepting) or should "
                    "the query select only one of them?",
                    d->name, label, spread, label);
    }
    op_result_free(&r);
    return o;
}

bool study_design_setup(StudyDesign *d, const EngineConfig *cfg, const JsonValue *res, int index, const char *project_dir, StudyLog *lg,
                        StudyIssues *is) {
    const JsonValue *dj = json_at(json_get(res, "designs"), (size_t)index);
    snprintf(d->name, sizeof d->name, "%s", json_get_str(dj, "name", "design"));
    if (lg) snprintf(lg->design, sizeof lg->design, "%s", d->name);
    d->inspection = json_object();
    json_set_string(d->inspection, "name", d->name);
    char err[900];
    d->eng = engine_create(cfg, err, sizeof err);
    if (!d->eng) {
        si_unsupported(is, "engine", true, "cannot start an engine for design '%s': %s", d->name, err);
        return false;
    }
    int blocking0 = is->blocking;
    OpResult r;
    JsonValue *p = json_object();
    json_set_string(p, "name", d->name);
    json_set_string(p, "directory", project_dir);
    char desc[400];
    snprintf(desc, sizeof desc, "design '%s' of comparison study '%s'", d->name, json_get_str(res, "name", ""));
    json_set_string(p, "description", desc);
    if (!study_op(d->eng, lg, "project_create", p, &r)) {
        study_op_error(&r, err, sizeof err);
        si_unsupported(is, "project", true, "design '%s': cannot create its project in %s: %s", d->name, project_dir, err);
        op_result_free(&r);
        return false;
    }
    op_result_free(&r);

    /* geometry: the unit is required; without it the file is imported with a provisional unit only to report its size */
    const JsonValue *g = json_get(dj, "geometry");
    const char *units = json_get_str(g, "units", NULL);
    bool units_known = units != NULL;
    p = json_object();
    json_set_string(p, "path", json_get_str(g, "path", ""));
    json_set_string(p, "units", units_known ? units : "mm");
    json_set_string(p, "units_source", units_known ? study_project_source(json_get_str(g, "units_source", "user")) : "default");
    if (!units_known) json_set_string(p, "units_note", "provisional unit to report the size under each candidate unit; the study cannot run until the unit is given");
    json_set_string(p, "name", "part");
    if (!study_op(d->eng, lg, "geometry_import", p, &r)) {
        study_op_error(&r, err, sizeof err);
        si_unsupported(is, "geometry_import", true, "design '%s': the geometry cannot be imported: %s", d->name, err);
        op_result_free(&r);
        return false;
    }
    const JsonValue *body = json_get(r.value, "body"), *diag = json_get(r.value, "diagnostics");
    const JsonValue *topo = json_get(diag, "topology");
    bool closed = json_get_bool(body, "closed_solid", false);
    double bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0}, t0[3] = {0, 0, 0};
    get3(json_get(json_get(body, "bounds_mm"), "min"), bmin);
    get3(json_get(json_get(body, "bounds_mm"), "max"), bmax);
    get3(json_get(json_get(body, "placement"), "translation_mm"), t0);
    for (int k = 0; k < 3; k++) d->bbox_min[k] = bmin[k] - t0[k], d->bbox_max[k] = bmax[k] - t0[k];
    d->volume_mm3 = json_get_num(body, "volume_mm3", 0);
    JsonValue *geo = json_set_object(d->inspection, "geometry");
    const JsonValue *src = json_get(body, "source");
    json_set_string(geo, "file", json_get_str(src, "original_path", ""));
    json_set_string(geo, "stored_copy", json_get_str(src, "stored_file", ""));
    json_set_string(geo, "sha256", json_get_str(src, "sha256", ""));
    json_set_int(geo, "bytes", json_get_int(src, "bytes", 0));
    json_set(geo, "units", units_known ? json_string(units) : json_null());
    json_set(geo, "bounds_design_mm", json_object());
    json_set(json_get(geo, "bounds_design_mm"), "min", json_vec3(d->bbox_min[0], d->bbox_min[1], d->bbox_min[2]));
    json_set(json_get(geo, "bounds_design_mm"), "max", json_vec3(d->bbox_max[0], d->bbox_max[1], d->bbox_max[2]));
    json_set(geo, "size_mm", json_clone(json_get(body, "size_mm")));
    json_set_number(geo, "volume_mm3", d->volume_mm3);
    json_set_number(geo, "surface_area_mm2", json_get_num(body, "surface_area_mm2", 0));
    json_set_int(geo, "triangles", json_get_int(body, "triangles", 0));
    json_set_int(geo, "surface_patches", json_get_int(body, "surface_patches", 0));
    json_set_bool(geo, "closed_solid", closed);
    json_set_int(geo, "components", json_get_int(topo, "components", 0));
    json_set(geo, "topology", json_clone(topo));
    json_set(geo, "repairs", json_clone(json_get(diag, "repairs")));
    json_set(geo, "units_check", json_clone(json_get(r.value, "units_check")));
    if (!units_known) {
        char qid[128];
        snprintf(qid, sizeof qid, "%s.units.candidates", d->name);
        si_warn(is, "UNIT_CANDIDATES", json_clone(json_get(json_get(r.value, "units_check"), "if_unit_were")),
                "design '%s' measures %.4g file units across its largest extent; details list its size under each candidate unit", d->name,
                json_get_num(json_get(r.value, "units_check"), "largest_extent_in_file_units", 0));
    } else if (!json_get_bool(json_get(r.value, "units_check"), "plausible", true)) {
        char qid[128];
        snprintf(qid, sizeof qid, "%s.units_implausible", d->name);
        si_question(is, qid, true, true, "a wrong unit changes stiffness by orders of magnitude", json_clone(json_get(r.value, "units_check")),
                    "Design '%s' is %.4g mm across with unit '%s', outside the plausible range of the import check. Is the unit right?", d->name,
                    json_get_num(json_get(r.value, "units_check"), "largest_extent_mm", 0), units);
    }
    if (!closed)
        si_unsupported(is, "geometry_not_closed", true,
                       "design '%s': the surface is not a closed solid (%lld open and %lld non-manifold edges); the volume mesher needs a watertight "
                       "surface and holes are not closed automatically. Repair the model in CAD and export it again.",
                       d->name, json_get_int(topo, "open_edges", -1), json_get_int(topo, "nonmanifold_edges", -1));
    if (json_get_int(topo, "components", 1) > 1)
        si_warn(is, "MULTIPLE_COMPONENTS", NULL,
                "design '%s' consists of %lld separate surface components; each must be held by the mounting region or joined to one that is", d->name,
                json_get_int(topo, "components", 1));
    op_result_free(&r);
    if (!closed || !units_known) return true;

    /* placement without rotation: the build frame is the design frame shifted so the part lies at z >= 0 */
    p = json_object();
    json_set_string(p, "body", "part");
    json_set_string(p, "up_axis", "+Z");
    json_set_string(p, "up_axis_source", "default");
    json_set_number(p, "rotate_z", 0);
    JsonValue *pos = json_set_array(p, "position");
    json_push(pos, json_number(0.5 * (d->bbox_min[0] + d->bbox_max[0])));
    json_push(pos, json_number(0.5 * (d->bbox_min[1] + d->bbox_max[1])));
    json_set_number(p, "z_offset", d->bbox_min[2] > 0 ? d->bbox_min[2] : 0);
    if (!study_op(d->eng, lg, "geometry_place", p, &r)) {
        study_op_error(&r, err, sizeof err);
        si_unsupported(is, "placement", true, "design '%s': cannot place the geometry: %s", d->name, err);
        op_result_free(&r);
        return false;
    }
    get3(json_get(json_get(json_get(r.value, "body"), "placement"), "translation_mm"), d->t_mm);
    op_result_free(&r);
    json_set(geo, "design_to_build_translation_mm", json_vec3(d->t_mm[0], d->t_mm[1], d->t_mm[2]));

    /* wall thickness, for the element sizes */
    p = json_object();
    json_set_string(p, "body", "part");
    json_set_string(p, "thickness", "run");
    json_set_string(p, "self_intersections", "run");
    d->thickness_min_mm = d->thickness_p05_mm = NAN;
    if (study_op(d->eng, lg, "geometry_diagnostics", p, &r)) {
        const JsonValue *checks = json_get(json_get(r.value, "diagnostics"), "checks");
        const JsonValue *wt = json_get(checks, "wall_thickness");
        d->thickness_min_mm = json_get_num(wt, "min_mm", NAN);
        d->thickness_p05_mm = json_get_num(wt, "p05_mm", NAN);
        JsonValue *wo = json_set_object(geo, "wall_thickness");
        json_set_number(wo, "min_mm", d->thickness_min_mm);
        json_set_number(wo, "p05_mm", d->thickness_p05_mm);
        json_set_number(wo, "median_mm", json_get_num(wt, "median_mm", NAN));
        double ml[3];
        if (get3(json_get(wt, "min_location_mm"), ml)) json_set(wo, "min_location_design_mm", design_mm(ml, d->t_mm));
        json_set_string(wo, "method", json_get_str(wt, "method", ""));
        json_set_int(geo, "self_intersecting_face_pairs", json_get_int(checks, "self_intersecting_face_pairs", -1));
    }
    op_result_free(&r);

    /* material */
    const JsonValue *mat = json_get(dj, "material");
    if (!mat || mat->type != JSON_OBJECT) mat = json_get(res, "material");
    if (!mat || mat->type != JSON_OBJECT) return true; /* the question is already recorded */
    if (!json_get_bool(mat, "library", false)) {
        p = json_object();
        json_set(p, "material", json_clone(json_get(mat, "record")));
        json_set_bool(p, "replace", true);
        if (!study_op(d->eng, lg, "material_define", p, &r)) {
            study_op_error(&r, err, sizeof err);
            si_question(is, "material.record", true, false, "the material record is invalid", NULL, "The material record cannot be stored: %s", err);
            op_result_free(&r);
            return true;
        }
        op_result_free(&r);
    }
    p = json_object();
    json_set_string(p, "body", "part");
    json_set_string(p, "material", json_get_str(mat, "id", ""));
    json_set_string(p, "source", study_project_source(json_get_str(mat, "source", "user")));
    char note[256];
    snprintf(note, sizeof note, "comparison study: source %s; %.180s", json_get_str(mat, "source", "user"), json_get_str(mat, "reference", ""));
    json_set_string(p, "note", note);
    if (!study_op(d->eng, lg, "material_assign", p, &r)) {
        study_op_error(&r, err, sizeof err);
        si_unsupported(is, "material_assign", true, "design '%s': %s", d->name, err);
        op_result_free(&r);
        return false;
    }
    op_result_free(&r);

    /* regions */
    p = json_object();
    json_set_string(p, "body", "part");
    json_set_int(p, "limit", 500);
    json_set_string(p, "sort", "id");
    JsonValue *patches = NULL;
    if (study_op(d->eng, lg, "surfaces_list", p, &r)) patches = json_clone(json_get(r.value, "patches"));
    op_result_free(&r);
    JsonValue *regions = json_set_object(d->inspection, "regions");
    JsonValue *mo = make_region(d, lg, is, "mounting", "mounting", json_get(dj, "mounting_region"), patches, &d->mount_area, d->mount_c, d->mount_n);
    JsonValue *lo = make_region(d, lg, is, "load", "load", json_get(dj, "load_region"), patches, &d->load_area, d->load_c, d->load_n);
    if (mo) json_set(regions, "mounting", mo);
    if (lo) json_set(regions, "load", lo);
    const JsonValue *alts = json_get(json_get(res, "sensitivity"), "mounting_alternatives");
    for (size_t i = 0; i < json_len(alts); i++) {
        const JsonValue *override = json_get(json_get(json_at(alts, i), "regions"), d->name);
        if (!override) continue;
        char sel[32], label[64];
        double a, c[3], n[3];
        snprintf(sel, sizeof sel, "mount_alt%zu", i + 1);
        snprintf(label, sizeof label, "alternative mounting %zu", i + 1);
        JsonValue *ao = make_region(d, lg, is, sel, label, override, patches, &a, c, n);
        if (ao) json_set(regions, sel, ao);
    }
    json_free(patches);
    if (!mo || !lo) return true;
    double arm[3] = {d->load_c[0] - d->mount_c[0], d->load_c[1] - d->mount_c[1], d->load_c[2] - d->mount_c[2]};
    json_set(regions, "lever_arm_design_mm", json_vec3(arm[0], arm[1], arm[2]));
    json_set_number(regions, "lever_arm_length_mm", norm3(arm));

    /* conditions */
    const JsonValue *mt = json_get(res, "mounting"), *ld = json_get(res, "load");
    p = json_object();
    json_set_string(p, "name", "mount");
    json_set_string(p, "kind", json_get_str(mt, "boundary_kind", "fixed"));
    json_set_string(p, "selection", "mounting");
    json_set_string(p, "source", study_project_source(json_get_str(mt, "source", "user")));
    json_set_string(p, "description", json_get_str(mt, "description", "mounting"));
    if (!study_op(d->eng, lg, "boundary_apply", p, &r)) {
        study_op_error(&r, err, sizeof err);
        char qid[128];
        snprintf(qid, sizeof qid, "%s.mounting_condition", d->name);
        si_question(is, qid, true, false, "the mounting idealisation must be applicable to the mounting region", NULL,
                    "Design '%s': the mounting cannot be applied: %s", d->name, err);
        op_result_free(&r);
        return true;
    }
    op_result_free(&r);
    double fv[3];
    if (get3(json_get(ld, "force_vector_n"), fv)) {
        p = json_object();
        json_set_string(p, "name", "payload");
        json_set_string(p, "kind", "force");
        json_set_string(p, "selection", "load");
        json_set(p, "force", json_vec3(fv[0], fv[1], fv[2]));
        json_set_string(p, "source", study_project_source(json_get_str(ld, "source", "user")));
        char ldesc[500];
        if (!strcmp(json_get_str(ld, "kind", ""), "payload_mass"))
            snprintf(ldesc, sizeof ldesc, "payload %.6g kg x %.6g m/s^2 = %.6g N, uniform over the load region", json_get_num(ld, "mass_kg", 0),
                     json_get_num(ld, "gravity_m_s2", 0), json_get_num(ld, "force_n", 0));
        else
            snprintf(ldesc, sizeof ldesc, "force %.6g N, uniform over the load region", json_get_num(ld, "force_n", 0));
        json_set_string(p, "description", ldesc);
        if (!study_op(d->eng, lg, "boundary_apply", p, &r)) {
            study_op_error(&r, err, sizeof err);
            si_unsupported(is, "load", true, "design '%s': the load cannot be applied: %s", d->name, err);
            op_result_free(&r);
            return false;
        }
        op_result_free(&r);
    } else {
        return true; /* the load magnitude question is recorded */
    }
    if (json_get_bool(ld, "self_weight", false)) {
        double dir[3] = {0, 0, -1}, gg = json_get_num(ld, "gravity_m_s2", 9.80665);
        get3(json_get(ld, "direction"), dir);
        p = json_object();
        json_set_string(p, "name", "self_weight");
        json_set_string(p, "kind", "gravity");
        json_set(p, "acceleration", json_vec3(gg * dir[0], gg * dir[1], gg * dir[2]));
        json_set_string(p, "source", study_project_source(json_get_str(ld, "source", "user")));
        json_set_string(p, "description", "the bracket's own weight, along the load direction");
        if (!study_op(d->eng, lg, "boundary_apply", p, &r)) {
            study_op_error(&r, err, sizeof err);
            si_unsupported(is, "self_weight", true, "design '%s': the self-weight cannot be applied: %s", d->name, err);
            op_result_free(&r);
            return false;
        }
        op_result_free(&r);
    }

    /* labelled preview of both regions */
    p = json_object();
    JsonValue *names = json_set_array(p, "names");
    json_push(names, json_string("mounting"));
    json_push(names, json_string("load"));
    char title[160];
    snprintf(title, sizeof title, "design %s: mounting region (first colour), load region (second colour)", d->name);
    json_set_string(p, "title", title);
    json_set_bool(p, "show_build_plate", false);
    if (study_op(d->eng, lg, "selection_preview", p, &r) && r.nimages > 0) {
        d->preview_png = malloc(r.images[0].len);
        if (d->preview_png) memcpy(d->preview_png, r.images[0].data, r.images[0].len), d->preview_len = r.images[0].len;
    }
    op_result_free(&r);
    d->ready = is->blocking == blocking0;
    return true;
}

JsonValue *study_equivalence(const StudyDesign *d, int n, const JsonValue *res, StudyIssues *is) {
    const JsonValue *eq = json_get(res, "equivalence");
    double ptol = json_get_num(eq, "position_tolerance_mm", 1), rtol = json_get_num(eq, "relative_tolerance", 0.02),
           atol = json_get_num(eq, "angle_tolerance_deg", 5);
    JsonValue *o = json_object();
    json_set_string(o, "reference_design", n > 0 ? d[0].name : "");
    JsonValue *tol = json_set_object(o, "tolerances");
    json_set_number(tol, "position_mm", ptol);
    json_set_number(tol, "relative", rtol);
    json_set_number(tol, "angle_deg", atol);
    json_set_string(o, "definition",
                    "every design against the first: the lever arm from the mounting-region centroid to the load-region centroid (design frame), the "
                    "areas of both regions and their mean normals. Equal face ids are not evidence of equivalence; these physical quantities are");
    JsonValue *checks = json_set_array(o, "checks");
    for (int i = 0; i < n; i++)
        if (!(d[i].mount_area > 0) || !(d[i].load_area > 0)) {
            json_set_string(o, "status", "not_checked");
            json_set_string(o, "reason", "the regions of every design must resolve first");
            return o;
        }
    bool all = true;
    char failed[1200] = "";
    double a0[3] = {d[0].load_c[0] - d[0].mount_c[0], d[0].load_c[1] - d[0].mount_c[1], d[0].load_c[2] - d[0].mount_c[2]};
    for (int i = 1; i < n; i++) {
        double a[3] = {d[i].load_c[0] - d[i].mount_c[0], d[i].load_c[1] - d[i].mount_c[1], d[i].load_c[2] - d[i].mount_c[2]};
        double da[3] = {a[0] - a0[0], a[1] - a0[1], a[2] - a0[2]};
        struct {
            const char *what, *unit;
            double ref, val, diff, limit;
        } C[5] = {
            {"lever_arm", "mm", norm3(a0), norm3(a), norm3(da), fmax(ptol, rtol * norm3(a0))},
            {"load_region_area", "mm2", d[0].load_area, d[i].load_area, fabs(d[i].load_area - d[0].load_area), rtol * d[0].load_area},
            {"mounting_region_area", "mm2", d[0].mount_area, d[i].mount_area, fabs(d[i].mount_area - d[0].mount_area), rtol * d[0].mount_area},
            {"mounting_normal_angle", "deg", 0, angle_deg(d[i].mount_n, d[0].mount_n), angle_deg(d[i].mount_n, d[0].mount_n), atol},
            {"load_normal_angle", "deg", 0, angle_deg(d[i].load_n, d[0].load_n), angle_deg(d[i].load_n, d[0].load_n), atol},
        };
        for (int k = 0; k < 5; k++) {
            bool ok = C[k].diff <= C[k].limit;
            JsonValue *c = json_object();
            json_set_string(c, "design", d[i].name);
            json_set_string(c, "quantity", C[k].what);
            json_set_string(c, "unit", C[k].unit);
            json_set_number(c, "reference", C[k].ref);
            json_set_number(c, "value", C[k].val);
            json_set_number(c, "difference", C[k].diff);
            json_set_number(c, "tolerance", C[k].limit);
            json_set_bool(c, "ok", ok);
            if (k == 0) json_set(c, "difference_vector_mm", json_vec3(da[0], da[1], da[2]));
            json_push(checks, c);
            if (!ok) {
                all = false;
                size_t len = strlen(failed);
                snprintf(failed + len, sizeof failed - len, "%s%s of '%s' differs by %.4g %s (tolerance %.4g)", len ? "; " : "", C[k].what, d[i].name,
                         C[k].diff, C[k].unit, C[k].limit);
            }
        }
    }
    json_set_string(o, "status", all ? "equivalent" : "differs");
    if (!all)
        si_question(is, "equivalence", true, true,
                    "a comparison is only meaningful when every design is held and loaded the same way; different lever arms or supports change the "
                    "result by themselves",
                    json_clone(checks),
                    "The designs are not mounted and loaded equivalently: %s. Is this difference part of the design change (accept it with the reason), "
                    "or should the regions be redefined?",
                    failed);
    return o;
}

JsonValue *study_coarse_check(StudyDesign *d, const JsonValue *res, StudyLog *lg, StudyIssues *is) {
    const JsonValue *sizes = json_get(json_get(res, "refinement"), "element_sizes_mm");
    size_t nl = json_len(sizes);
    if (!d->ready || !nl) return NULL;
    long long maxe = json_get_int(json_get(res, "limits"), "max_elements", 400000);
    JsonValue *o = json_object();
    JsonValue *plan = json_set_array(o, "levels");
    double hf = json_at(sizes, nl - 1)->u.number;
    for (size_t i = 0; i < nl; i++) {
        double h = json_at(sizes, i)->u.number, est = d->volume_mm3 / (h * h * h);
        JsonValue *lv = json_object();
        json_set_number(lv, "element_size_mm", h);
        json_set_number(lv, "estimated_elements", round(est));
        json_set_bool(lv, "within_element_limit", est <= (double)maxe);
        if (isfinite(d->thickness_p05_mm)) json_set_number(lv, "elements_across_thin_walls", d->thickness_p05_mm / h);
        json_push(plan, lv);
        if (est > (double)maxe)
            si_warn(is, "LEVEL_OVER_ELEMENT_LIMIT", NULL,
                    "design '%s' needs about %.0f elements at %.4g mm, above the limit of %lld: that level will be skipped, and the refinement study ends "
                    "at the previous size",
                    d->name, est, h, maxe);
    }
    if (isfinite(d->thickness_p05_mm) && d->thickness_p05_mm < 2 * hf)
        si_warn(is, "THIN_WALLS_UNDER_RESOLVED", NULL,
                "design '%s' has walls down to %.3g mm (5th percentile of sampled thickness), fewer than two elements even at the finest size %.4g mm: "
                "bending of those walls is poorly resolved, and the refinement study shows whether that matters",
                d->name, d->thickness_p05_mm, hf);
    if (isfinite(d->thickness_min_mm) && d->thickness_min_mm < hf)
        si_warn(is, "THIN_FEATURE_MAY_VANISH", NULL,
                "design '%s' has a wall %.3g mm thick, thinner than the finest element (%.4g mm): the voxel mesh may lose it, and a lost load path makes "
                "that mesh unusable (reported per level, never repaired silently)",
                d->name, d->thickness_min_mm, hf);
    OpResult r;
    char err[800], hs[48];
    snprintf(hs, sizeof hs, "%.9g mm", json_at(sizes, 0)->u.number);
    JsonValue *p = json_object();
    json_set_string(p, "element_size", hs);
    json_set_int(p, "max_elements", maxe);
    if (!study_op(d->eng, lg, "mesh_generate", p, &r)) {
        study_op_error(&r, err, sizeof err);
        si_warn(is, "COARSE_MESH_FAILED", NULL, "design '%s': the coarsest mesh (%s) cannot be generated: %s", d->name, hs, err);
        op_result_free(&r);
        return o;
    }
    const JsonValue *mesh = json_get(r.value, "mesh");
    JsonValue *mo = json_set_object(o, "coarsest_mesh");
    json_set_number(mo, "element_size_mm", json_at(sizes, 0)->u.number);
    json_set_int(mo, "elements", json_get_int(mesh, "elements", 0));
    json_set_int(mo, "nodes", json_get_int(mesh, "nodes", 0));
    json_set(mo, "bodies", json_clone(json_get(mesh, "bodies")));
    json_set(mo, "selections", json_clone(json_get(mesh, "selections")));
    json_set(mo, "warnings", json_clone(json_get(r.value, "warnings")));
    op_result_free(&r);
    const JsonValue *an = json_get(res, "analysis");
    p = json_object();
    json_set_string(p, "analysis", "static_structural");
    json_set_string(p, "formulation", json_get_str(an, "formulation", "incompatible_modes"));
    json_set_number(p, "reference_temperature", json_get_num(an, "reference_temperature_c", 20));
    if (!study_op(d->eng, lg, "setup_validate", p, &r)) {
        study_op_error(&r, err, sizeof err);
        si_warn(is, "VALIDATION_FAILED", NULL, "design '%s': %s", d->name, err);
        op_result_free(&r);
        return o;
    }
    bool ready = json_get_bool(r.value, "ready", false);
    json_set_bool(mo, "setup_ready", ready);
    const JsonValue *errors = json_get(r.value, "errors");
    JsonValue *codes = json_set_array(mo, "setup_errors");
    for (size_t i = 0; i < json_len(errors); i++) {
        const JsonValue *er = json_at(errors, i);
        const char *code = json_get_str(er, "code", "");
        json_push(codes, json_clone(er));
        char qid[160];
        if (!strcmp(code, "INSUFFICIENT_CONSTRAINTS")) {
            snprintf(qid, sizeof qid, "%s.mounting_underconstrained", d->name);
            si_question(is, qid, true, false,
                        "a part that can move as a rigid body has no static solution: no stiffness can be computed until the mounting holds it",
                        json_clone(json_get(er, "details")),
                        "Design '%s' is not held against rigid-body motion by its mounting: %s Which further faces are supported, or how is it attached?",
                        d->name, json_get_str(er, "message", ""));
        } else if (!strcmp(code, "MESH_INVALID")) {
            si_warn(is, "REGION_NOT_ON_COARSEST_MESH", json_clone(er),
                    "design '%s': %s (at the coarsest size; finer levels are checked when they run)", d->name, json_get_str(er, "message", ""));
        } else {
            snprintf(qid, sizeof qid, "%s.setup.%s", d->name, code);
            si_question(is, qid, true, false, "the analysis cannot start with this setup", json_clone(er), "Design '%s': %s (%s)", d->name,
                        json_get_str(er, "message", ""), json_get_str(er, "hint", ""));
        }
    }
    op_result_free(&r);
    return o;
}
