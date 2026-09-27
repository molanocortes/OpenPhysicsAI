/* study_run.c - the comparison-study job: set up every design on its private engine, solve each at every mesh size,
 * then at the declared sensitivities, and write the evidence record and the report */
#include "../core/sha256.h"
#include "study_internal.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

void study_job_free(void *data) {
    StudyJob *sj = data;
    if (!sj) return;
    json_free(sj->resolved);
    json_free(sj->check);
    free(sj);
}

typedef struct RunCtx {
    Job *job;
    StudyJob *sj;
    StudyLog log;
    double t0, wall_limit;
    long long max_elements;
    JsonValue *failures;
    bool cancelled, out_of_time, out_of_storage;
    int runs_done, runs_planned;
} RunCtx;

static double peak_rss_mb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return (double)ru.ru_maxrss / 1048576.0; /* bytes */
#else
    return (double)ru.ru_maxrss / 1024.0; /* kilobytes */
#endif
}

static const char *class_of(const char *code) {
    if (!strcmp(code, "RESOURCE_LIMIT") || !strcmp(code, "INSUFFICIENT_STORAGE")) return "resource_exhaustion";
    if (!strcmp(code, "SOLVER_FAILED")) return "numerical_failure";
    if (!strcmp(code, "UNSUPPORTED")) return "unsupported_physics";
    if (!strcmp(code, "CANCELLED")) return "cancelled";
    if (!strcmp(code, "IO_ERROR") || !strcmp(code, "PERMISSION_DENIED")) return "io";
    if (!strcmp(code, "TIME_BUDGET")) return "resource_exhaustion";
    return "invalid_input";
}

static const char *recovery_of(const char *code) {
    if (!strcmp(code, "RESOURCE_LIMIT") || !strcmp(code, "LEVEL_OVER_ELEMENT_LIMIT"))
        return "use the completed coarser levels (they are in the evidence record), raise limits.max_elements if memory allows, or remove the finest size";
    if (!strcmp(code, "TIME_BUDGET")) return "raise limits.max_wall_time and replay the study, or accept the levels completed within the budget";
    if (!strcmp(code, "INSUFFICIENT_STORAGE"))
        return "free disk space, or set retain_results to none, then replay the study; the levels completed before storage ran short are in the record";
    if (!strcmp(code, "IO_ERROR"))
        return "check that the study directory is writable and its file system has free space, then replay the study; completed runs remain in the design projects";
    if (!strcmp(code, "SOLVER_FAILED")) return "retry with analysis.solver direct, or inspect the design's project (project_open) and the run's summary";
    if (!strcmp(code, "INSUFFICIENT_CONSTRAINTS")) return "change the mounting region or idealisation so every part of the design is held";
    if (!strcmp(code, "MESH_INVALID")) return "a region or feature is smaller than the elements at this size: refine, or check the region";
    return "open the design's project with project_open and inspect the failing step; the operations log lists every call";
}

static void failure(RunCtx *cx, const char *stage, const char *design, double h, const char *code, const char *message) {
    JsonValue *f = json_object();
    json_set_string(f, "stage", stage);
    if (design) json_set_string(f, "design", design);
    if (h > 0) json_set_number(f, "element_size_mm", h);
    json_set_string(f, "failure_class", class_of(code));
    json_set_string(f, "code", code);
    json_set_string(f, "message", message);
    json_set_string(f, "recovery", recovery_of(code));
    json_set_bool(f, "partial_results_available", true);
    json_push(cx->failures, f);
}

static bool over_budget(RunCtx *cx) {
    if (job_cancel_requested(cx->job)) cx->cancelled = true;
    if (!cx->cancelled && study_now() - cx->t0 > cx->wall_limit && !cx->out_of_time) {
        cx->out_of_time = true;
        char m[200];
        snprintf(m, sizeof m, "the wall-time budget of %.0f s was used up; no further analysis was started", cx->wall_limit);
        failure(cx, "schedule", NULL, 0, "TIME_BUDGET", m);
    }
    return cx->cancelled || cx->out_of_time || cx->out_of_storage;
}

/* setup_validate takes the model settings; analysis_run also the solver */
static JsonValue *analysis_params(const JsonValue *res, bool with_solver) {
    const JsonValue *an = json_get(res, "analysis");
    JsonValue *p = json_object();
    json_set_string(p, "analysis", "static_structural");
    json_set_string(p, "formulation", json_get_str(an, "formulation", "incompatible_modes"));
    if (with_solver) json_set_string(p, "solver", json_get_str(an, "solver", "auto"));
    json_set_number(p, "reference_temperature", json_get_num(an, "reference_temperature_c", 20));
    return p;
}

static void progress(RunCtx *cx, const char *fmt, const char *design, double h) {
    char stage[160];
    snprintf(stage, sizeof stage, fmt, design, h);
    double f = cx->runs_planned > 0 ? 0.05 + 0.9 * (double)cx->runs_done / (double)cx->runs_planned : 0.5;
    job_progress(cx->job, f > 0.95 ? 0.95 : f, stage);
}

/* validate, solve and evaluate the current setup of design d; NULL (with the failure recorded) when it did not finish */
static JsonValue *run_analysis(RunCtx *cx, StudyDesign *d, const char *label, double h, bool keep_results) {
    OpResult r;
    char err[1000];
    double t0 = study_now();
    /* free space for this analysis's field before starting it (the field is written even when it is not retained) */
    double need = study_run_bytes(d->volume_mm3 / (h * h * h)), reserve = study_storage_reserve_bytes();
    char at[NV_PATH_MAX];
    double free_b = study_free_bytes(cx->sj->dir, at, sizeof at);
    if (free_b >= 0 && free_b - reserve < need) {
        snprintf(err, sizeof err, "not enough free storage to start this analysis: about %.0f MB needed, %.0f MB free at %s, %.0f MB kept in reserve", need / 1048576,
                 free_b / 1048576, at, reserve / 1048576);
        failure(cx, "storage", d->name, h, "INSUFFICIENT_STORAGE", err);
        cx->out_of_storage = true;
        return NULL;
    }
    if (!study_op(d->eng, &cx->log, "setup_validate", analysis_params(cx->sj->resolved, false), &r)) {
        study_op_error(&r, err, sizeof err);
        failure(cx, "validation", d->name, h, json_get_str(r.error, "code", "INTERNAL"), err);
        op_result_free(&r);
        return NULL;
    }
    if (!json_get_bool(r.value, "ready", false)) {
        const JsonValue *e0 = json_at(json_get(r.value, "errors"), 0);
        snprintf(err, sizeof err, "%s (%s)", json_get_str(e0, "message", "the setup is not ready"), json_get_str(e0, "hint", ""));
        failure(cx, "validation", d->name, h, json_get_str(e0, "code", "PRECONDITION_FAILED"), err);
        op_result_free(&r);
        return NULL;
    }
    op_result_free(&r);
    JsonValue *p = analysis_params(cx->sj->resolved, true);
    json_set_string(p, "label", label);
    json_set_bool(p, "allow_duplicate", true);
    if (!study_op(d->eng, &cx->log, "analysis_run", p, &r)) {
        study_op_error(&r, err, sizeof err);
        failure(cx, "solve", d->name, h, json_get_str(r.error, "code", "INTERNAL"), err);
        op_result_free(&r);
        return NULL;
    }
    char job_id[64], run_dir[NV_PATH_MAX], spec_hash[65];
    snprintf(job_id, sizeof job_id, "%s", json_get_str(r.value, "job_id", ""));
    snprintf(run_dir, sizeof run_dir, "%s", json_get_str(r.value, "run_directory", ""));
    snprintf(spec_hash, sizeof spec_hash, "%s", json_get_str(r.value, "spec_hash", ""));
    op_result_free(&r);
    JsonValue *st = NULL;
    for (;;) {
        if (job_cancel_requested(cx->job)) {
            cx->cancelled = true;
            JsonValue *cp = json_object();
            json_set_string(cp, "job_id", job_id);
            study_op(d->eng, &cx->log, "job_cancel", cp, &r);
            op_result_free(&r);
            return NULL;
        }
        JsonValue *sp = json_object();
        json_set_string(sp, "job_id", job_id);
        json_set_number(sp, "wait_seconds", 2);
        if (!study_op(d->eng, NULL, "job_status", sp, &r)) {
            study_op_error(&r, err, sizeof err);
            failure(cx, "solve", d->name, h, "INTERNAL", err);
            op_result_free(&r);
            return NULL;
        }
        const char *state = json_get_str(r.value, "state", "");
        if (!strcmp(state, "succeeded") || !strcmp(state, "failed") || !strcmp(state, "cancelled")) {
            st = json_clone(r.value);
            op_result_free(&r);
            break;
        }
        op_result_free(&r);
    }
    if (strcmp(json_get_str(st, "state", ""), "succeeded") != 0) {
        const JsonValue *er = json_get(st, "error");
        snprintf(err, sizeof err, "job %s %s: %s", job_id, json_get_str(st, "state", ""), json_get_str(er, "message", ""));
        failure(cx, "solve", d->name, h, json_get_str(er, "code", "SOLVER_FAILED"), err);
        json_free(st);
        return NULL;
    }
    const JsonValue *dir = json_get(json_get(cx->sj->resolved, "load"), "direction");
    JsonValue *qp = json_object();
    json_set_string(qp, "job_id", job_id);
    JsonValue *regions = json_set_array(qp, "regions");
    JsonValue *r1 = json_object(), *r2 = json_object();
    json_set_string(r1, "selection", "load");
    json_set(r1, "direction", json_clone(dir));
    json_set_string(r2, "selection", "mounting");
    json_set(r2, "direction", json_clone(dir));
    json_push(regions, r1);
    json_push(regions, r2);
    JsonValue *loads = json_set_array(qp, "loads");
    json_push(loads, json_string("payload"));
    json_set(qp, "about", json_vec3(d->mount_c[0] + d->t_mm[0], d->mount_c[1] + d->t_mm[1], d->mount_c[2] + d->t_mm[2]));
    if (!study_op(d->eng, &cx->log, "results_quantities", qp, &r)) {
        study_op_error(&r, err, sizeof err);
        failure(cx, "quantities", d->name, h, json_get_str(r.error, "code", "INTERNAL"), err);
        op_result_free(&r);
        json_free(st);
        return NULL;
    }
    JsonValue *o = json_object();
    json_set_string(o, "label", label);
    json_set_string(o, "job_id", job_id);
    const char *rel = strncmp(run_dir, cx->sj->dir, strlen(cx->sj->dir)) == 0 ? run_dir + strlen(cx->sj->dir) + 1 : run_dir;
    json_set_string(o, "run_directory", rel);
    json_set_string(o, "spec_hash", spec_hash);
    char path[NV_PATH_MAX], hex[65];
    uint64_t bytes = 0;
    path_join(path, sizeof path, run_dir, "results.nvr");
    if (sha256_file(path, hex, &bytes)) json_set_string(o, "results_sha256", hex);
    /* retain_results: the full field of this run is removed once its quantities and hash are recorded (replay regenerates it) */
    json_set_bool(o, "results_retained", keep_results);
    if (!keep_results) remove(path);
    json_set_number(o, "wall_seconds", study_now() - t0);
    json_set_number(o, "process_peak_rss_mb", peak_rss_mb());
    json_set(o, "summary", json_take(st, "summary"));
    json_set(o, "quantities", json_clone(r.value));
    op_result_free(&r);
    json_free(st);
    cx->runs_done++;
    return o;
}

static bool get3(const JsonValue *a, double out[3]) { return json_get_numbers(a, out, 3); }
static double norm3(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

/* the level record: mesh, geometry approximation, region mapping, checks, quantities, stresses, cost */
static JsonValue *level_record(RunCtx *cx, StudyDesign *d, double h, const JsonValue *meshv, JsonValue *run) {
    JsonValue *o = json_object();
    json_set_number(o, "element_size_mm", h);
    const JsonValue *mesh = json_get(meshv, "mesh");
    JsonValue *mo = json_set_object(o, "mesh");
    json_set_int(mo, "elements", json_get_int(mesh, "elements", 0));
    json_set_int(mo, "nodes", json_get_int(mesh, "nodes", 0));
    json_set_int(mo, "dofs", json_get_int(mesh, "dofs_structural", 0));
    json_set_number(mo, "seconds", json_get_num(mesh, "seconds", NAN));
    const JsonValue *b0 = json_at(json_get(mesh, "bodies"), 0);
    json_set_number(mo, "volume_mesh_mm3", json_get_num(b0, "volume_mesh_mm3", NAN));
    json_set_number(mo, "volume_geometry_mm3", json_get_num(b0, "volume_stl_mm3", d->volume_mm3));
    json_set_number(mo, "volume_error_percent", json_get_num(b0, "volume_error_percent", NAN));
    long long regions = json_get_int(mesh, "face_connected_regions", json_get_int(b0, "face_connected_regions", 1));
    json_set_int(mo, "face_connected_regions", regions);
    json_set(mo, "warnings", json_clone(json_get(meshv, "warnings")));

    const JsonValue *sm = json_get(run, "summary"), *q = json_get(run, "quantities");
    JsonValue *ro = json_set_object(o, "run");
    json_set_string(ro, "project", d->name);
    json_set_string(ro, "job_id", json_get_str(run, "job_id", ""));
    json_set_string(ro, "run_directory", json_get_str(run, "run_directory", ""));
    json_set_string(ro, "spec_hash", json_get_str(run, "spec_hash", ""));
    json_set_string(ro, "results_sha256", json_get_str(run, "results_sha256", ""));
    json_set_bool(ro, "results_retained", json_get_bool(run, "results_retained", true));

    const JsonValue *ql = json_at(json_get(q, "regions"), 0), *qm = json_at(json_get(q, "regions"), 1), *qw = json_at(json_get(q, "loads"), 0);
    const JsonValue *bal = json_get(q, "balance");
    const JsonValue *bload = NULL;
    for (size_t i = 0; i < json_len(json_get(bal, "loads")); i++)
        if (!strcmp(json_get_str(json_at(json_get(bal, "loads"), i), "condition", ""), "payload")) bload = json_at(json_get(bal, "loads"), i);
    const JsonValue *bsup = json_at(json_get(bal, "supports"), 0);

    double arm = norm3((double[3]){d->load_c[0] - d->mount_c[0], d->load_c[1] - d->mount_c[1], d->load_c[2] - d->mount_c[2]});
    JsonValue *qo = json_set_object(o, "quantities");
    double dmean = json_get_num(ql, "mean_along_direction_mm", NAN), dw = json_get_num(qw, "conjugate_displacement_mm", NAN);
    json_set_number(qo, "load_region_displacement_mm", dmean);
    JsonValue *range = json_set_array(qo, "load_region_displacement_range_mm");
    json_push(range, json_number(json_get_num(ql, "min_along_direction_mm", NAN)));
    json_push(range, json_number(json_get_num(ql, "max_along_direction_mm", NAN)));
    json_set_number(qo, "work_conjugate_displacement_mm", dw);
    json_set_number(qo, "stiffness_n_per_mm", json_get_num(qw, "stiffness_n_per_mm", NAN));
    json_set_number(qo, "load_n", json_get_num(qw, "force_magnitude_n", NAN));
    json_set_number(qo, "mounting_region_mean_displacement_mm", json_get_num(qm, "mean_along_direction_mm", NAN));
    const JsonValue *disp = json_get(sm, "displacement");
    json_set_number(qo, "peak_displacement_mm", json_get_num(disp, "max_magnitude_mm", NAN));
    double pk[3];
    if (get3(json_get(disp, "at_mm"), pk)) json_set(qo, "peak_displacement_at_design_mm", json_vec3(pk[0] - d->t_mm[0], pk[1] - d->t_mm[1], pk[2] - d->t_mm[2]));
    const JsonValue *mass = json_get(q, "mass");
    json_set_number(qo, "mass_geometry_kg", json_get_num(mass, "total_geometry_kg", NAN));
    json_set_number(qo, "mass_mesh_kg", json_get_num(mass, "total_mesh_kg", NAN));
    json_set(qo, "reaction_force_n", json_clone(json_get(bsup, "force_n")));
    json_set(qo, "reaction_moment_nm", json_clone(json_get(bsup, "moment_nm")));
    json_set_string(qo, "moment_reference", "centroid of the mounting region (design frame)");

    const JsonValue *vm = json_get(sm, "von_mises");
    JsonValue *so = json_set_object(o, "stress");
    json_set_number(so, "gauss_point_max_mpa", json_get_num(vm, "gauss_point_max_mpa", NAN));
    json_set_number(so, "nodal_average_max_mpa", json_get_num(vm, "nodal_average_max_mpa", NAN));
    json_set_number(so, "nodal_p99_mpa", json_get_num(vm, "nodal_p99_mpa", NAN));
    json_set_bool(so, "peak_at_support", json_get_bool(vm, "nodal_max_at_supported_node", false) || json_get_bool(vm, "gauss_point_max_element_touches_support", false));
    json_set_bool(so, "peak_near_reentrant_corner", json_get_bool(vm, "peak_near_reentrant_corner", false));
    JsonValue *codes = json_set_array(o, "result_warnings");
    for (size_t i = 0; i < json_len(json_get(sm, "result_warnings")); i++)
        json_push(codes, json_string(json_get_str(json_at(json_get(sm, "result_warnings"), i), "code", "")));

    /* checks */
    const JsonValue *ck = json_get(sm, "checks"), *sv = json_get(sm, "solver"), *df = json_get(q, "deformation");
    JsonValue *co = json_set_object(o, "checks");
    JsonValue *reasons = json_array();
    bool eq_ok = json_get_bool(ck, "equilibrium_ok", false), mb_ok = json_get_bool(ck, "moment_balance_ok", false), en_ok = json_get_bool(ck, "energy_ok", false);
    bool conv = json_get_bool(sv, "converged", false);
    json_set_number(co, "equilibrium_error", json_get_num(ck, "equilibrium_error", NAN));
    json_set_bool(co, "equilibrium_ok", eq_ok);
    json_set_number(co, "moment_balance_error", json_get_num(ck, "moment_balance_error", NAN));
    json_set_bool(co, "moment_balance_ok", mb_ok);
    json_set_number(co, "energy_ratio", json_get_num(ck, "energy_ratio", NAN));
    json_set_bool(co, "energy_ok", en_ok);
    json_set_string(co, "solver_method", json_get_str(sv, "method", ""));
    json_set_bool(co, "solver_converged", conv);
    json_set_number(co, "solver_true_relative_residual", json_get_num(sv, "true_relative_residual", NAN));
    const char *cls = json_get_str(df, "classification", "");
    json_set_string(co, "deformation_classification", cls);
    json_set_number(co, "displacement_to_size_ratio", json_get_num(df, "displacement_to_size_ratio", NAN));
    json_set_number(co, "max_rotation_rad", json_get_num(df, "max_rotation_rad", NAN));
    json_set_number(co, "max_strain_indicator", json_get_num(df, "max_strain_indicator", NAN));
    double consistency = isfinite(dmean) && fabs(dmean) > 0 ? fabs(dw - dmean) / fabs(dmean) : NAN;
    json_set_number(co, "conjugate_vs_region_mean", consistency);
    /* the load region on the mesh: area, centroid, and the line of action of the applied load */
    double shift = json_get_num(bload, "line_of_action_shift_mm", NAN), crit = fmax(0.5 * h, 0.01 * arm);
    double mesh_area = json_get_num(ql, "area_mm2", NAN), c_mesh[3] = {0, 0, 0};
    get3(json_get(ql, "centroid_mm"), c_mesh);
    double dc[3] = {c_mesh[0] - d->t_mm[0] - d->load_c[0], c_mesh[1] - d->t_mm[1] - d->load_c[1], c_mesh[2] - d->t_mm[2] - d->load_c[2]};
    JsonValue *lm = json_set_object(co, "load_region_mapping");
    json_set_int(lm, "mesh_faces", json_get_int(ql, "mesh_faces", 0));
    json_set_number(lm, "mesh_area_mm2", mesh_area);
    json_set_number(lm, "geometry_area_mm2", d->load_area);
    json_set_number(lm, "area_ratio", mesh_area / d->load_area);
    json_set_number(lm, "centroid_shift_mm", norm3(dc));
    json_set_number(lm, "line_of_action_shift_mm", shift);
    json_set_number(lm, "criterion_mm", crit);
    json_set_number(lm, "force_deviation", json_get_num(bload, "force_deviation", NAN));
    bool lm_ok = isfinite(shift) && shift <= crit && json_get_num(bload, "force_deviation", 1) <= 1e-6;
    json_set_bool(lm, "ok", lm_ok);
    JsonValue *mm = json_set_object(co, "mounting_region_mapping");
    double m_area = json_get_num(qm, "area_mm2", NAN);
    json_set_int(mm, "mesh_faces", json_get_int(qm, "mesh_faces", 0));
    json_set_number(mm, "mesh_area_mm2", m_area);
    json_set_number(mm, "geometry_area_mm2", d->mount_area);
    json_set_number(mm, "area_ratio", m_area / d->mount_area);
    bool mm_ok = json_get_int(qm, "mesh_faces", 0) > 0 && m_area / d->mount_area > 0.5 && m_area / d->mount_area < 2;
    json_set_bool(mm, "ok", mm_ok);
    json_set_bool(co, "mesh_topology_ok", regions == 1);
    if (!eq_ok) json_push(reasons, json_string("force equilibrium not satisfied"));
    if (!mb_ok) json_push(reasons, json_string("moment balance not satisfied"));
    if (!en_ok) json_push(reasons, json_string("strain energy does not match the external work"));
    if (!conv) json_push(reasons, json_string("linear solver did not converge"));
    if (!lm_ok) json_push(reasons, json_string("the mesh moved the load's line of action beyond the criterion"));
    if (!mm_ok) json_push(reasons, json_string("the mounting region is not represented on this mesh"));
    if (regions != 1) json_push(reasons, json_string("the mesh split the part into separate regions: a thin connection was lost"));
    double verr = json_get_num(mesh, "volume_error_percent", NAN);
    if (!isfinite(verr)) verr = json_get_num(json_at(json_get(mesh, "bodies"), 0), "volume_error_percent", NAN);
    if (isfinite(verr) && fabs(verr) > 10)
        json_push(reasons, json_stringf("the mesh volume differs from the geometry by %.1f%%: this mesh does not represent the design (walls or features too thin for "
                                        "the element size)",
                                        verr));
    if (!strcmp(cls, "outside_small_deformation_assumption")) json_push(reasons, json_string("deformation outside the small-deformation assumption"));
    json_set_bool(o, "valid", json_len(reasons) == 0);
    json_set(o, "invalid_reasons", reasons);

    JsonValue *cost = json_set_object(o, "cost");
    json_set_number(cost, "wall_seconds", json_get_num(run, "wall_seconds", NAN));
    json_set_number(cost, "solve_seconds", json_get_num(sv, "seconds", NAN));
    json_set_number(cost, "factor_mb", json_get_num(sv, "factor_mb", NAN));
    json_set_number(cost, "process_peak_rss_mb", json_get_num(run, "process_peak_rss_mb", NAN));
    json_set_string(cost, "memory_note", "factor_mb is the direct solver's factor; process_peak_rss_mb is the high-water mark of the whole process so far");
    (void)cx;
    return o;
}

static bool mesh_at(RunCtx *cx, StudyDesign *d, double h, JsonValue **mesh_out) {
    OpResult r;
    char hs[48], err[900];
    snprintf(hs, sizeof hs, "%.9g mm", h);
    JsonValue *p = json_object();
    json_set_string(p, "element_size", hs);
    json_set_int(p, "max_elements", cx->max_elements);
    if (!study_op(d->eng, &cx->log, "mesh_generate", p, &r)) {
        study_op_error(&r, err, sizeof err);
        failure(cx, "mesh", d->name, h, json_get_str(r.error, "code", "MESH_INVALID"), err);
        op_result_free(&r);
        return false;
    }
    if (mesh_out) *mesh_out = json_clone(r.value);
    op_result_free(&r);
    return true;
}

/* a material record like the study's with scaled modulus or another Poisson ratio */
static JsonValue *material_variant(const JsonValue *mat, const char *suffix, double e_scale, double nu) {
    JsonValue *rec = json_clone(json_get(mat, "record"));
    char id[64], name[160];
    snprintf(id, sizeof id, "%.40s_%s", json_get_str(mat, "id", "material"), suffix);
    snprintf(name, sizeof name, "%.100s (sensitivity %s)", json_get_str(mat, "name", ""), suffix);
    json_set_string(rec, "id", id);
    json_set_string(rec, "name", name);
    if (json_get(rec, "status") && strcmp(json_get_str(rec, "status", ""), "calibrated") == 0) json_set_string(rec, "status", "user_supplied");
    if (e_scale != 1) {
        JsonValue *e = json_get(rec, "youngs_modulus_pa");
        JsonValue *v = json_get(e, "value");
        if (v && v->type == JSON_NUMBER) v->u.number *= e_scale;
        for (size_t i = 0; v && v->type == JSON_ARRAY && i < json_len(v); i++) json_at(v, i)->u.number *= e_scale;
    }
    if (isfinite(nu)) {
        JsonValue *pv = json_object();
        json_set_number(pv, "value", nu);
        json_set(rec, "poisson_ratio", pv);
    }
    return rec;
}

static bool use_material(RunCtx *cx, StudyDesign *d, JsonValue *record, const char *id) {
    OpResult r;
    if (record) {
        JsonValue *p = json_object();
        json_set(p, "material", record);
        json_set_bool(p, "replace", true);
        bool ok = study_op(d->eng, &cx->log, "material_define", p, &r);
        op_result_free(&r);
        if (!ok) return false;
    }
    JsonValue *p = json_object();
    json_set_string(p, "body", "part");
    json_set_string(p, "material", id);
    json_set_string(p, "source", "user");
    json_set_string(p, "note", "comparison study sensitivity");
    bool ok = study_op(d->eng, &cx->log, "material_assign", p, &r);
    op_result_free(&r);
    return ok;
}

static bool condition(RunCtx *cx, StudyDesign *d, const char *op, JsonValue *p) {
    OpResult r;
    bool ok = study_op(d->eng, &cx->log, op, p, &r);
    op_result_free(&r);
    return ok;
}

static JsonValue *mount_params(const JsonValue *res, const char *name, const char *kind, const char *selection) {
    JsonValue *p = json_object();
    json_set_string(p, "name", name);
    json_set_string(p, "kind", kind);
    json_set_string(p, "selection", selection);
    json_set_string(p, "source", study_project_source(json_get_str(json_get(res, "mounting"), "source", "user")));
    json_set_string(p, "description", json_get_str(json_get(res, "mounting"), "description", "mounting"));
    return p;
}

static JsonValue *remove_params(const char *name) {
    JsonValue *p = json_object();
    json_set_string(p, "name", name);
    return p;
}

static void sensitivity_run(RunCtx *cx, StudyDesign *d, JsonValue *list, const char *kind, const char *parameter, double value, double h) {
    char label[200];
    snprintf(label, sizeof label, "%s / %s / %s %s", json_get_str(cx->sj->resolved, "name", ""), d->name, kind, parameter);
    progress(cx, "design %s: sensitivity at %.4g mm", d->name, h);
    JsonValue *run = run_analysis(cx, d, label, h, !strcmp(json_get_str(cx->sj->resolved, "retain_results", "refinement"), "all"));
    JsonValue *o = json_object();
    json_set_string(o, "kind", kind);
    json_set_string(o, "parameter", parameter);
    if (isfinite(value)) json_set_number(o, "value", value);
    json_set_number(o, "element_size_mm", h);
    if (!run) {
        json_set_bool(o, "completed", false);
        json_push(list, o);
        return;
    }
    JsonValue *rec = level_record(cx, d, h, NULL, run);
    json_set_bool(o, "completed", true);
    json_set(o, "run", json_take(rec, "run"));
    json_set(o, "quantities", json_take(rec, "quantities"));
    json_set(o, "checks", json_take(rec, "checks"));
    json_set_bool(o, "valid", json_get_bool(rec, "valid", false));
    json_free(rec);
    json_free(run);
    json_push(list, o);
}

static void sensitivities(RunCtx *cx, StudyDesign *d, double h, JsonValue *list) {
    const JsonValue *res = cx->sj->resolved, *sens = json_get(res, "sensitivity");
    const JsonValue *dj = NULL;
    for (size_t i = 0; i < json_len(json_get(res, "designs")); i++)
        if (!strcmp(json_get_str(json_at(json_get(res, "designs"), i), "name", ""), d->name)) dj = json_at(json_get(res, "designs"), i);
    const JsonValue *mat = json_get(dj, "material");
    if (!mat || mat->type != JSON_OBJECT) mat = json_get(res, "material");
    const char *mat_id = json_get_str(mat, "id", "");
    bool library = json_get_bool(mat, "library", false);
    const JsonValue *er = json_get(sens, "youngs_modulus_relative");
    if (json_len(er) == 2 && !over_budget(cx)) {
        double s = json_at(er, 1)->u.number;
        char suffix[48], par[64];
        snprintf(suffix, sizeof suffix, "E_x%.4g", s);
        snprintf(par, sizeof par, "Young's modulus x %.4g", s);
        JsonValue *rec = material_variant(mat, suffix, s, NAN);
        char id[64];
        snprintf(id, sizeof id, "%s", json_get_str(rec, "id", ""));
        if (use_material(cx, d, rec, id)) sensitivity_run(cx, d, list, "youngs_modulus", par, s, h);
        use_material(cx, d, NULL, mat_id);
        (void)library;
    }
    const JsonValue *nus = json_get(sens, "poisson_ratio");
    for (size_t i = 0; i < json_len(nus) && !over_budget(cx); i++) {
        double nu = json_at(nus, i)->u.number;
        char suffix[48], par[64];
        snprintf(suffix, sizeof suffix, "nu_%.4g", nu);
        snprintf(par, sizeof par, "Poisson's ratio %.4g", nu);
        JsonValue *rec = material_variant(mat, suffix, 1, nu);
        char id[64];
        snprintf(id, sizeof id, "%s", json_get_str(rec, "id", ""));
        if (use_material(cx, d, rec, id)) sensitivity_run(cx, d, list, "poisson_ratio", par, nu, h);
        use_material(cx, d, NULL, mat_id);
    }
    const JsonValue *alts = json_get(sens, "mounting_alternatives");
    for (size_t i = 0; i < json_len(alts) && !over_budget(cx); i++) {
        const JsonValue *alt = json_at(alts, i);
        char sel[32];
        snprintf(sel, sizeof sel, "mount_alt%zu", i + 1);
        bool override = json_get(json_get(alt, "regions"), d->name) != NULL;
        const char *kind = !strcmp(json_get_str(alt, "idealization", "fixed"), "frictionless_normal") ? "frictionless_support" : "fixed";
        if (!condition(cx, d, "boundary_remove", remove_params("mount"))) continue;
        if (condition(cx, d, "boundary_apply", mount_params(res, "mount_alternative", kind, override ? sel : "mounting")))
            sensitivity_run(cx, d, list, "mounting_alternative", json_get_str(alt, "name", sel), NAN, h);
        condition(cx, d, "boundary_remove", remove_params("mount_alternative"));
        condition(cx, d, "boundary_apply", mount_params(res, "mount", json_get_str(json_get(res, "mounting"), "boundary_kind", "fixed"), "mounting"));
    }
    if (json_get_bool(sens, "self_weight", false) && !over_budget(cx)) {
        const JsonValue *ld = json_get(res, "load");
        double dir[3] = {0, 0, -1}, g = json_get_num(ld, "gravity_m_s2", 9.80665);
        get3(json_get(ld, "direction"), dir);
        JsonValue *p = json_object();
        json_set_string(p, "name", "self_weight_sensitivity");
        json_set_string(p, "kind", "gravity");
        json_set(p, "acceleration", json_vec3(g * dir[0], g * dir[1], g * dir[2]));
        json_set_string(p, "source", "default");
        json_set_string(p, "description", "sensitivity: the bracket's own weight added to the payload");
        if (condition(cx, d, "boundary_apply", p)) sensitivity_run(cx, d, list, "self_weight", "own weight added", NAN, h);
        condition(cx, d, "boundary_remove", remove_params("self_weight_sensitivity"));
    }
}

static bool write_text(const char *dir, const char *name, const char *text, size_t len, char *err, size_t errlen) {
    char path[NV_PATH_MAX];
    if (!path_join(path, sizeof path, dir, name)) {
        snprintf(err, errlen, "path too long for %s", name);
        return false;
    }
    return path_write_file_atomic(path, text, len, err, errlen);
}

static bool write_json(const char *dir, const char *name, const JsonValue *v, char *err, size_t errlen) {
    size_t n = 0;
    char *text = json_dump(v, JSON_PRETTY | JSON_SORTED, &n, NULL);
    if (!text) {
        snprintf(err, errlen, "cannot serialise %s (out of memory)", name);
        return false;
    }
    char *nl = realloc(text, n + 2);
    if (nl) text = nl, text[n++] = '\n', text[n] = 0;
    bool ok = write_text(dir, name, text, n, err, errlen);
    free(text);
    return ok;
}

bool study_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    StudyJob *sj = data;
    const JsonValue *res = sj->resolved;
    RunCtx cx;
    memset(&cx, 0, sizeof cx);
    cx.job = job, cx.sj = sj, cx.t0 = study_now(), cx.failures = json_array();
    cx.wall_limit = json_get_num(json_get(res, "limits"), "max_wall_seconds", 1800);
    cx.max_elements = json_get_int(json_get(res, "limits"), "max_elements", 400000);
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, sj->dir, "operations.jsonl");
    cx.log.f = fopen(path, "a");
    int log_errno = cx.log.f ? 0 : errno;
    const JsonValue *designs = json_get(res, "designs"), *sizes = json_get(json_get(res, "refinement"), "element_sizes_mm");
    int nd = (int)json_len(designs), nl = (int)json_len(sizes);
    StudyDesign d[STUDY_MAX_DESIGNS];
    memset(d, 0, sizeof d);
    JsonValue *levels[STUDY_MAX_DESIGNS] = {0}, *sens[STUDY_MAX_DESIGNS] = {0};
    int nsens = (int)json_len(json_get(json_get(res, "sensitivity"), "poisson_ratio")) + (int)json_len(json_get(json_get(res, "sensitivity"), "mounting_alternatives")) +
                (json_len(json_get(json_get(res, "sensitivity"), "youngs_modulus_relative")) == 2) + json_get_bool(json_get(res, "sensitivity"), "self_weight", false);
    cx.runs_planned = nd * (nl + nsens);
    StudyIssues is;
    si_init(&is, json_get(res, "accept"));
    bool setup_ok = true;
    if (!cx.log.f) {
        char m[NV_PATH_MAX + 200];
        snprintf(m, sizeof m, "cannot open the operations log %s: %s; the study was not started, because its steps could not be recorded", path,
                 strerror(log_errno));
        failure(&cx, "setup", NULL, 0, "IO_ERROR", m);
        setup_ok = false;
    }
    job_progress(job, 0.01, "setting up the designs");
    for (int i = 0; i < nd; i++) {
        levels[i] = json_array(), sens[i] = json_array();
        if (!setup_ok) continue;
        char pdir[NV_PATH_MAX], sub[128];
        snprintf(sub, sizeof sub, "designs/%s", json_get_str(json_at(designs, (size_t)i), "name", "design"));
        path_join(pdir, sizeof pdir, sj->dir, sub);
        if (!study_design_setup(&d[i], &sj->cfg, res, i, pdir, &cx.log, &is) || !d[i].ready) {
            char *q = json_dump(is.questions, 0, NULL, NULL), *u = json_dump(is.unsupported, 0, NULL, NULL);
            char m[1500];
            snprintf(m, sizeof m, "design '%s' could not be set up as it was at submission (did an input file change?): questions %.600s; unsupported %.600s",
                     d[i].name, q ? q : "", u ? u : "");
            free(q), free(u);
            failure(&cx, "setup", d[i].name, 0, "PRECONDITION_FAILED", m);
            setup_ok = false;
            break;
        }
        if (d[i].preview_png) {
            char pv[NV_PATH_MAX], name[128], perr[200];
            snprintf(name, sizeof name, "previews/%s-regions.png", d[i].name);
            path_join(pv, sizeof pv, sj->dir, "previews");
            path_mkdirs(pv);
            path_join(pv, sizeof pv, sj->dir, name);
            if (!path_write_file_atomic(pv, d[i].preview_png, d[i].preview_len, perr, sizeof perr)) failure(&cx, "record", d[i].name, 0, "IO_ERROR", perr);
        }
    }
    int completed_levels = 0;
    for (int li = 0; setup_ok && li < nl; li++) {
        double h = json_at(sizes, (size_t)li)->u.number;
        if (over_budget(&cx)) break;
        bool within = true;
        for (int i = 0; i < nd; i++) within &= d[i].volume_mm3 / (h * h * h) <= (double)cx.max_elements;
        if (!within) {
            char m[300];
            snprintf(m, sizeof m, "the %.4g mm level needs more than the %lld elements allowed per analysis; the refinement study ends at the previous level", h,
                     cx.max_elements);
            failure(&cx, "mesh", NULL, h, "LEVEL_OVER_ELEMENT_LIMIT", m);
            break;
        }
        bool all = true;
        for (int i = 0; i < nd && !over_budget(&cx); i++) {
            progress(&cx, "design %s: %.4g mm mesh", d[i].name, h);
            JsonValue *meshv = NULL, *run = NULL;
            char label[200];
            snprintf(label, sizeof label, "%s / %s / %.4g mm", json_get_str(res, "name", ""), d[i].name, h);
            if (mesh_at(&cx, &d[i], h, &meshv)) run = run_analysis(&cx, &d[i], label, h, strcmp(json_get_str(res, "retain_results", "refinement"), "none") != 0);
            if (!run) {
                JsonValue *lf = json_object();
                json_set_number(lf, "element_size_mm", h);
                json_set_bool(lf, "completed", false);
                json_set_bool(lf, "valid", false);
                json_push(levels[i], lf);
                all = false;
            } else {
                JsonValue *rec = level_record(&cx, &d[i], h, meshv, run);
                json_set_bool(rec, "completed", true);
                json_push(levels[i], rec);
            }
            json_free(meshv), json_free(run);
        }
        if (!all || cx.cancelled || cx.out_of_time || cx.out_of_storage) break;
        completed_levels = li + 1;
    }

    /* sensitivities at the requested level among those every design completed */
    if (setup_ok && completed_levels > 0 && !cx.cancelled) {
        const char *which = json_get_str(json_get(res, "sensitivity"), "level", "finest");
        int ls = completed_levels - 1;
        if (!strcmp(which, "coarsest")) ls = 0;
        else if (!strcmp(which, "second_finest") && completed_levels > 1) ls = completed_levels - 2;
        double h = json_at(sizes, (size_t)ls)->u.number;
        for (int i = 0; i < nd && nsens > 0 && !over_budget(&cx); i++) {
            if (ls != completed_levels - 1 && !mesh_at(&cx, &d[i], h, NULL)) continue;
            sensitivities(&cx, &d[i], h, sens[i]);
        }
    }
    for (int i = 0; i < nd; i++) {
        if (!d[i].eng) continue;
        OpResult r;
        study_op(d[i].eng, &cx.log, "project_save", json_object(), &r);
        op_result_free(&r);
    }

    /* a failed write to the operations log leaves the record of executed steps incomplete: say so in the evidence */
    if (cx.log.f && (fflush(cx.log.f) != 0 || ferror(cx.log.f)))
        failure(&cx, "record", NULL, 0, "IO_ERROR", "a write to operations.jsonl failed (file system full or not writable): the operations log is incomplete");
    const char *status = cx.cancelled ? "cancelled" : (!setup_ok || completed_levels == 0 ? "failed" : (json_len(cx.failures) ? "partial" : "complete"));
    job_progress(job, 0.97, "writing the evidence record");
    JsonValue *ev = study_build_evidence(sj, d, nd, levels, sens, cx.failures, status, study_now() - cx.t0);
    char ev_err[NV_PATH_MAX + 300] = "", md_err[NV_PATH_MAX + 300] = "";
    bool ev_ok = write_json(sj->dir, "evidence.json", ev, ev_err, sizeof ev_err), md_ok = false;
    char *md = study_report_markdown(ev);
    if (md) md_ok = write_text(sj->dir, "report.md", md, strlen(md), md_err, sizeof md_err);
    else snprintf(md_err, sizeof md_err, "the report could not be generated (out of memory)");
    free(md);
    JsonValue *cmp_for_error = ev_ok && md_ok ? NULL : json_clone(json_get(ev, "comparison"));
    JsonValue *summary = json_object();
    json_set_string(summary, "analysis", "comparison_study");
    json_set_string(summary, "status", status);
    json_set_string(summary, "directory", sj->dir);
    json_set_string(summary, "study_hash", sj->hash);
    json_set_string(summary, "evidence_file", "evidence.json");
    json_set_bool(summary, "evidence_written", ev_ok);
    json_set_string(summary, "report_file", "report.md");
    json_set_bool(summary, "report_written", md_ok);
    json_set(summary, "comparison", json_clone(json_get(ev, "comparison")));
    json_set(summary, "failures", json_clone(cx.failures));
    json_set_string(summary, "next_step", "read the record with study_evidence (detail: summary or full); report.md is the readable version");
    if (sj->reference_dir[0]) {
        char rerr[400];
        JsonValue *ref = study_read_evidence(sj->reference_dir, rerr, sizeof rerr);
        JsonValue *cmp = ref ? study_compare_replay(ref, ev) : NULL;
        if (!cmp) {
            cmp = json_object();
            json_set_string(cmp, "outcome", "not_compared");
            json_set_string(cmp, "reason", rerr);
        }
        json_set_string(cmp, "reference_directory", sj->reference_dir);
        char rp_err[NV_PATH_MAX + 300];
        if (!write_json(sj->dir, "replay.json", cmp, rp_err, sizeof rp_err)) json_set_string(cmp, "write_error", rp_err);
        json_set(summary, "replay", cmp);
        json_free(ref);
    }
    job_set_summary(job, summary);
    json_free(ev);
    if (cx.log.f) fclose(cx.log.f);
    si_free(&is);
    for (int i = 0; i < nd; i++) {
        study_design_free(&d[i]);
        json_free(levels[i]), json_free(sens[i]);
    }
    bool ok = !strcmp(status, "complete") || !strcmp(status, "partial");
    if (ok && (!ev_ok || !md_ok)) {
        /* the analyses finished but their record is not on disk: the job fails, with the comparison still in its summary */
        JsonValue *details = json_object();
        json_set_string(details, "evidence_write", ev_ok ? "written" : ev_err);
        json_set_string(details, "report_write", md_ok ? "written" : md_err);
        json_set(details, "comparison", cmp_for_error), cmp_for_error = NULL;
        job_set_error_details(job, details);
        snprintf(code, codelen, "IO_ERROR");
        snprintf(err, errlen,
                 "the analyses finished, but %s could not be written (%s). Free space or fix the permissions, then run study_replay on the directory; the "
                 "design projects keep the runs and this job's error details hold the comparison",
                 !ev_ok ? "the evidence record evidence.json" : "the report report.md", !ev_ok ? ev_err : md_err);
        json_free(cx.failures);
        return false;
    }
    if (!ok) {
        JsonValue *details = json_object();
        json_set(details, "failures", json_clone(cx.failures));
        json_set_string(details, "evidence_file", "evidence.json (partial results, when any, are recorded there)");
        job_set_error_details(job, details);
        snprintf(code, codelen, "%s", cx.cancelled ? "CANCELLED" : json_get_str(json_at(cx.failures, 0), "code", "SOLVER_FAILED"));
        snprintf(err, errlen, "%s", cx.cancelled ? "the study was cancelled; completed levels are in evidence.json"
                                                 : json_get_str(json_at(cx.failures, 0), "message", "the study did not complete"));
    }
    json_free(cmp_for_error);
    json_free(cx.failures);
    return ok;
}
