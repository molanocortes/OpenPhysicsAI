/* ops_topopt.c - the topology_optimize operation and its job (docs/contracts/topology-optimisation.md)
 *
 * The same model the static analysis builds (mesh, materials, supports, loads), handed to the optimiser of
 * src/fem/topopt.c. The run directory keeps summary.json and density.f32 (one little-endian float per element, in
 * element order), so the field can be read back after the process ends. topology_result returns the summary and
 * the history, and the density itself when it is asked for. */
#include "../core/base64.h"
#include "../core/sha256.h"
#include "../fem/topopt.h"
#include "../threads.h"
#include "ops_internal.h"
#include "static_analysis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TopoJob {
    StaticModel model;
    TopOptSettings set;
    TopOptResult res;
    JsonValue *summary;
    char job_id[64];
    char run_dir[NV_PATH_MAX];
} TopoJob;

static void topo_free(void *data) {
    TopoJob *t = data;
    if (!t) return;
    static_model_free(&t->model);
    topopt_result_free(&t->res);
    json_free(t->summary);
    free(t);
}

typedef struct TopoProgress {
    Job *job;
    int max_iter;
} TopoProgress;

static bool topo_progress(void *ctx, int it, double change) {
    TopoProgress *pr = ctx;
    if (job_cancel_requested(pr->job)) return false;
    char stage[160];
    snprintf(stage, sizeof stage, "iteration %d of at most %d, largest density change %.3f", it, pr->max_iter, change);
    double f = pr->max_iter > 0 ? (double)it / pr->max_iter : 0;
    job_progress(pr->job, 0.05 + 0.90 * (f > 1 ? 1 : f), stage);
    return true;
}

static JsonValue *topo_summary(const TopoJob *t) {
    const TopOptResult *r = &t->res;
    JsonValue *v = json_object();
    json_set_string(v, "analysis", "topology_optimization");
    json_set_string(v, "job_id", t->job_id);
    json_set_string(v, "mesh_hash", t->model.mesh_hash);
    json_set_int(v, "elements", t->model.nelems);
    json_set_int(v, "nodes", t->model.nnodes);
    JsonValue *s = json_set_object(v, "settings");
    json_set_number(s, "volume_fraction", t->set.volume_fraction);
    json_set_number(s, "penalty", t->set.penalty);
    json_set_number(s, "filter_radius_m", t->set.filter_radius);
    json_set_number(s, "move_limit", t->set.move_limit);
    json_set_int(s, "max_iterations", t->set.max_iter);
    json_set_number(s, "change_tolerance", t->set.change_tol);
    json_set_int(s, "passive_layers", t->set.passive_layers);
    json_set_string(s, "method", "modified SIMP, compliance objective, sensitivity filter, optimality criteria");
    json_set_int(v, "iterations", r->iterations);
    json_set_int(v, "solves", r->solves);
    json_set_int(v, "passive_elements", r->passive_elements);
    json_set_number(v, "compliance_j", r->compliance);
    json_set_number(v, "compliance_initial_j", r->compliance_initial);
    json_set_number(v, "volume_fraction", r->volume_fraction);
    json_set_number(v, "last_change", r->change);
    json_set_number(v, "energy_mismatch", r->energy_mismatch);
    json_set_number(v, "seconds", r->seconds);
    json_set_string(v, "stop_reason", r->stop_reason);
    JsonValue *h = json_set_array(v, "history");
    for (int i = 0; i < r->iterations; i++) {
        JsonValue *o = json_object();
        json_set_int(o, "iteration", r->history[i].iter);
        json_set_number(o, "compliance_j", r->history[i].compliance);
        json_set_number(o, "volume_fraction", r->history[i].volume_fraction);
        json_set_number(o, "change", r->history[i].change);
        json_push(h, o);
    }
    int above = 0;
    for (int e = 0; e < t->model.nelems; e++)
        if (r->density[e] >= 0.5) above++;
    json_set_int(v, "elements_above_half", above);
    json_set_string(v, "means",
                    "a compliance-optimal density field on this mesh, at this volume fraction, for this one load "
                    "case. No stress constraint, no manufacturing constraint, no second load case: the part that "
                    "comes out is the elements above 0.5 density and still has to be checked as a design");
    return v;
}

static bool topo_write_density(const TopoJob *t, char *err, size_t errlen) {
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, t->run_dir, "density.f32");
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errlen, "cannot write %s", path);
        return false;
    }
    for (int e = 0; e < t->model.nelems; e++) {
        float v = (float)t->res.density[e];
        unsigned char b[4];
        memcpy(b, &v, 4); /* the build is little-endian on both supported platforms; the header records the order */
        if (fwrite(b, 1, 4, f) != 4) {
            fclose(f);
            snprintf(err, errlen, "cannot write %s", path);
            return false;
        }
    }
    fclose(f);
    return true;
}

static bool topo_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    TopoJob *t = data;
    StaticModel *m = &t->model;
    job_progress(job, 0.02, "starting the optimisation");
    int threads = cpu_perf_count();
    ThreadPool *pool = pool_create(threads > 0 ? threads : 1);
    HexModel hmod = {m->nnodes, m->nelems, m->xyz, m->conn, m->elem_mat, m->nmat, m->mat, m->settings.formulation, NULL, m->elem_type};
    SolidLoads L = {m->fixed, m->fixed_value, m->nodal_force, NULL, {m->gravity[0], m->gravity[1], m->gravity[2]}};
    TopoProgress pr = {job, t->set.max_iter};
    SolidOptions opt = {m->settings.solver, m->settings.pcg_tol > 0 ? m->settings.pcg_tol : 1e-10, 0, 0, pool, NULL, NULL};
    t->set.progress = topo_progress;
    t->set.ctx = &pr;
    bool ok = topopt_run(&hmod, &L, &opt, &t->set, &t->res, err, errlen);
    if (pool) pool_destroy(pool);
    if (!ok) {
        const char *c = "SOLVER_FAILED";
        if (job_cancel_requested(job)) c = "CANCELLED";
        else if (strstr(err, "rigid") || strstr(err, "freely") || strstr(err, "singular")) c = "INSUFFICIENT_CONSTRAINTS";
        else if (strstr(err, "out of memory") || strstr(err, "limit")) c = "RESOURCE_LIMIT";
        else if (strstr(err, "volume fraction") || strstr(err, "hexahedra")) c = "PRECONDITION_FAILED";
        snprintf(code, codelen, "%s", c);
        return false;
    }
    job_progress(job, 0.96, "writing the density field");
    json_free(t->summary);
    t->summary = topo_summary(t);
    if (t->run_dir[0]) {
        char path[NV_PATH_MAX], hex[65];
        uint64_t bytes = 0;
        if (!topo_write_density(t, err, errlen)) {
            snprintf(code, codelen, "IO_ERROR");
            return false;
        }
        path_join(path, sizeof path, t->run_dir, "density.f32");
        JsonValue *files = json_set_array(t->summary, "files");
        if (sha256_file(path, hex, &bytes)) {
            JsonValue *fo = json_object();
            json_set_string(fo, "name", "density.f32");
            json_set_string(fo, "format", "little-endian float32, one per element, in element order");
            json_set_int(fo, "bytes", (long long)bytes);
            json_set_string(fo, "sha256", hex);
            json_push(files, fo);
        }
        path_join(path, sizeof path, t->run_dir, "summary.json");
        if (!json_write_file(path, t->summary, JSON_PRETTY | JSON_SORTED)) {
            snprintf(code, codelen, "IO_ERROR");
            snprintf(err, errlen, "cannot write %s", path);
            return false;
        }
    }
    job_set_summary(job, json_clone(t->summary));
    return true;
}

static void op_topology_optimize(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    TopoJob *t = calloc(1, sizeof *t);
    if (!t) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    StaticSettings ss;
    memset(&ss, 0, sizeof ss);
    ss.formulation = strcmp(json_get_str(p, "formulation", "incompatible_modes"), "full_integration") ? HEX8_INCOMPATIBLE : HEX8_FULL;
    const char *sv = json_get_str(p, "solver", "auto");
    ss.solver = !strcmp(sv, "direct") ? SOLID_SOLVER_DIRECT : (!strcmp(sv, "iterative") ? SOLID_SOLVER_PCG : SOLID_SOLVER_AUTO);
    ss.pcg_tol = json_get_num(p, "tolerance", 1e-10);
    ss.reference_temperature_k = 293.15;

    JsonValue *errors = json_array(), *warnings = json_array();
    if (!static_model_build(proj, &ss, &t->model, errors, warnings)) {
        const JsonValue *first = json_at(errors, 0);
        NvErr code = nv_err_from_name(json_get_str(first, "code", "PRECONDITION_FAILED"));
        char msg[1024], hint[512];
        snprintf(msg, sizeof msg, "%s", json_get_str(first, "message", "the setup is invalid"));
        snprintf(hint, sizeof hint, "%s", json_get_str(first, "hint", "run setup_validate for the full report"));
        op_fail(out, code, hint, "the setup cannot be optimised: %s", msg);
        op_fail_detail(out, "errors", errors);   /* both arrays are taken by the error, not freed here */
        op_fail_detail(out, "warnings", warnings);
        topo_free(t);
        return;
    }
    json_free(errors);
    if (t->model.elem_type != SOLID_ELEM_HEX8) {
        op_fail(out, NV_ERR_UNSUPPORTED, "mesh the part again with mesh_generate and no method, which gives the voxel mesh",
                "topology optimisation runs on the voxel mesh; this project is meshed with %s elements", sa_element_name(&t->model));
        json_free(warnings);
        topo_free(t);
        return;
    }

    topopt_settings_default(&t->set);
    t->set.volume_fraction = json_get_num(p, "volume_fraction", 0.3);
    t->set.penalty = json_get_num(p, "penalty", 3.0);
    t->set.move_limit = json_get_num(p, "move_limit", 0.2);
    t->set.max_iter = (int)json_get_int(p, "max_iterations", 60);
    t->set.change_tol = json_get_num(p, "change_tolerance", 0.01);
    t->set.passive_layers = (int)json_get_int(p, "passive_layers", 1);
    bool present = false;
    double radius = 0;
    if (!op_quantity(out, p, "filter_radius", DIM_LENGTH, "mm", &radius, &present)) {
        json_free(warnings);
        topo_free(t);
        return;
    }
    t->set.filter_radius = present ? radius : 1.5 * cbrt(t->model.h[0] * t->model.h[1] * t->model.h[2]);
    if (!(t->set.volume_fraction > 0 && t->set.volume_fraction <= 1)) {
        op_fail(out, NV_ERR_INVALID_REQUEST, "volume_fraction is a fraction of the meshed volume, between 0 and 1",
                "volume_fraction %g is outside (0, 1]", t->set.volume_fraction);
        json_free(warnings);
        topo_free(t);
        return;
    }

    char err[1024], runs[NV_PATH_MAX], dir[NV_PATH_MAX], id[64];
    jobs_new_id(e->jobs, id, sizeof id);
    path_join(runs, sizeof runs, proj->dir, "runs");
    path_join(dir, sizeof dir, runs, id);
    if (path_exists(dir) || !engine_resolve_write_path(e, NULL, dir, true, true, dir, sizeof dir, err, sizeof err)) {
        op_fail(out, NV_ERR_IO, NULL, "cannot create the run directory %s%s%s", dir, err[0] ? ": " : "", err);
        json_free(warnings);
        topo_free(t);
        return;
    }
    snprintf(t->job_id, sizeof t->job_id, "%s", id);
    snprintf(t->run_dir, sizeof t->run_dir, "%s", dir);
    JobSpec js = {id, "topology_optimization", json_get_str(p, "label", ""), dir, "", proj->id, proj->name, proj->revision, t, topo_job_run, topo_free};
    if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
        op_fail(out, NV_ERR_BUSY, "check job_list and retry when jobs have finished", "%s", err);
        json_free(warnings);
        topo_free(t);
        return;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "state", "queued");
    json_set_int(v, "elements", t->model.nelems);
    json_set_number(v, "volume_fraction", t->set.volume_fraction);
    json_set_number(v, "filter_radius_mm", 1e3 * t->set.filter_radius);
    json_set_int(v, "max_iterations", t->set.max_iter);
    json_set(v, "warnings", warnings);
    json_set_string(v, "next_step", "job_status follows it; topology_result reads the density field when it is done");
    op_succeed(out, v);
}

static void op_topology_result(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    const char *id = json_get_str(p, "job_id", "");
    char dir[NV_PATH_MAX];
    if (!id[0] || !op_project_run_dir(e, id, dir, sizeof dir)) {
        op_fail(out, NV_ERR_NOT_FOUND, "job_list shows the runs of this project", "no run directory for job '%s'", id);
        return;
    }
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, dir, "summary.json");
    JsonError je;
    JsonValue *summary = json_read_file(path, 64u << 20, &je);
    if (!summary) {
        op_fail(out, NV_ERR_NOT_FOUND, "the run may still be going: job_status tells", "cannot read %s", path);
        return;
    }
    if (strcmp(json_get_str(summary, "analysis", ""), "topology_optimization") != 0) {
        op_fail(out, NV_ERR_UNSUPPORTED, "topology_result reads the runs of topology_optimize", "job '%s' is a %s run", id,
                json_get_str(summary, "analysis", "different"));
        json_free(summary);
        return;
    }
    JsonValue *v = json_clone(summary);
    int nelems = (int)json_get_int(summary, "elements", 0);
    json_free(summary);
    if (json_get_bool(p, "include_density", false)) {
        path_join(path, sizeof path, dir, "density.f32");
        FILE *f = fopen(path, "rb");
        if (!f) {
            op_fail(out, NV_ERR_NOT_FOUND, NULL, "cannot read %s", path);
            json_free(v);
            return;
        }
        size_t n = (size_t)nelems * 4;
        unsigned char *buf = malloc(n ? n : 1);
        if (!buf || fread(buf, 1, n, f) != n) {
            fclose(f);
            free(buf);
            op_fail(out, NV_ERR_IO, NULL, "%s is shorter than the %d elements of the run", path, nelems);
            json_free(v);
            return;
        }
        fclose(f);
        size_t blen = 0;
        char *b64 = base64_encode(buf, n, &blen);
        free(buf);
        if (!b64) {
            op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory encoding the density field");
            json_free(v);
            return;
        }
        json_set_string(v, "density_base64", b64);
        json_set_string(v, "density_format", "little-endian float32, one per element, in element order");
        free(b64);
    }
    op_succeed(out, v);
}

const OpBinding OPS_TOPOPT_BINDINGS[] = {
    {"topology_optimize", op_topology_optimize},
    {"topology_result", op_topology_result},
    {NULL, NULL},
};
