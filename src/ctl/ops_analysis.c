/* ops_analysis.c - setup validation, analysis submission and job control */
#include "../core/sha256.h"
#include "matlib.h"
#include "selection.h"
#include "ops_internal.h"
#include "static_analysis.h"
#include "transient_analysis.h"
#include "../mech/mech_struct.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_settings(OpResult *out, const JsonValue *p, StaticSettings *s) {
    memset(s, 0, sizeof *s);
    s->formulation = strcmp(json_get_str(p, "formulation", "incompatible_modes"), "full_integration") ? HEX8_INCOMPATIBLE : HEX8_FULL;
    const char *sv = json_get_str(p, "solver", "auto");
    s->solver = !strcmp(sv, "direct") ? SOLID_SOLVER_DIRECT : (!strcmp(sv, "iterative") ? SOLID_SOLVER_PCG : SOLID_SOLVER_AUTO);
    s->pcg_tol = json_get_num(p, "tolerance", 1e-10);
    s->reference_temperature_k = 293.15;
    s->printed = !strcmp(json_get_str(json_get(p, "material_model"), "type", ""), "orthotropic"); /* G17: the printed model's stiffness */
    bool present = false;
    double t = 0;
    if (!op_quantity(out, p, "reference_temperature", DIM_TEMPERATURE, "degC", &t, &present)) return false;
    if (present) s->reference_temperature_k = t;
    return true;
}

static const char *solver_name(SolidSolver s) { return s == SOLID_SOLVER_DIRECT ? "direct" : (s == SOLID_SOLVER_PCG ? "iterative" : "auto"); }

/* the complete, hashable description of a run: settings, software, setup, mesh and the resolved material records */
static JsonValue *run_spec(Engine *e, const StaticSettings *s, const StaticModel *m, const JsonValue *material_model, char hash[65]) {
    Project *proj = e->proj;
    JsonValue *spec = json_object();
    json_set_string(spec, "format", "navier-am-run-spec");
    json_set_int(spec, "format_version", 1);
    json_set_string(spec, "analysis", "static_structural");
    JsonValue *st = json_set_object(spec, "settings");
    json_set_string(st, "formulation", static_formulation_name(s->formulation));
    json_set_string(st, "solver", solver_name(s->solver));
    json_set_number(st, "tolerance", s->pcg_tol);
    json_set_number(st, "reference_temperature_c", s->reference_temperature_k - 273.15);
    JsonValue *sw = json_set_object(spec, "software");
    json_set_string(sw, "name", "OpenPhysicsAI");
    json_set_string(sw, "version", OPENPHYSICS_VERSION);
    json_set_string(sw, "contract_version", ops_contract_version());
    json_set_string(sw, "compiler", __VERSION__);
    json_set_string(sw, "floating_point", "IEEE 754 double precision; no -ffast-math; no floating-point contraction");
    JsonValue *pj = project_to_json(proj);
    json_remove(pj, "created");
    json_remove(pj, "modified");
    json_remove(pj, "revision");
    json_set(spec, "project", pj);
    JsonValue *mesh = json_set_object(spec, "mesh");
    json_set_string(mesh, "hash", m->mesh_hash);
    json_set_int(mesh, "nodes", m->nnodes);
    json_set_int(mesh, "elements", m->nelems);
    json_set(mesh, "element_size_mm", json_vec3(1e3 * m->h[0], 1e3 * m->h[1], 1e3 * m->h[2]));
    JsonValue *mats = json_set_array(spec, "materials_resolved");
    for (int i = 0; i < m->nmat; i++) {
        MaterialRecord rec;
        char err[300];
        if (material_lookup(proj->user_materials, m->mat_id[i], &rec, err, sizeof err)) json_push(mats, json_clone(rec.json));
    }
    if (material_model) json_set(spec, "material_model", json_clone(material_model)); /* a printed part: another run */
    size_t n = 0;
    char *text = json_dump(spec, JSON_SORTED, &n, NULL);
    sha256_hex_of(text ? text : "", text ? n : 0, hash);
    free(text);
    return spec;
}

static void op_setup_validate(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    const char *analysis = json_get_str(p, "analysis", "static_structural");
    if (strcmp(analysis, "static_structural") != 0) {
        ThermalSettings ts;
        if (!op_thermal_settings(out, p, analysis, &ts)) return;
        ThermalCase *c = calloc(1, sizeof *c);
        if (!c) {
            op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
            return;
        }
        JsonValue *errors = json_array(), *warnings = json_array();
        bool ready = thermal_case_build(e->proj, &ts, c, errors, warnings);
        JsonValue *v = json_object();
        json_set_string(v, "analysis", analysis);
        json_set_bool(v, "ready", ready);
        json_set(v, "errors", errors);
        json_set(v, "warnings", warnings);
        if (c->nnodes) json_set(v, "model", thermal_case_model_json(c));
        json_set(v, "assumptions", assumptions_json(e->proj));
        json_set_string(v, "next_step", ready ? "analysis_run starts the solve as a job" : "fix the listed errors, then validate again");
        thermal_case_free(c);
        op_succeed(out, v);
        return;
    }
    StaticSettings s;
    if (!parse_settings(out, p, &s)) return;
    StaticModel m;
    JsonValue *errors = json_array(), *warnings = json_array();
    bool ready = static_model_build(e->proj, &s, &m, errors, warnings);
    JsonValue *v = json_object();
    json_set_string(v, "analysis", "static_structural");
    json_set_bool(v, "ready", ready);
    json_set(v, "errors", errors);
    json_set(v, "warnings", warnings);
    if (m.fixed) json_set(v, "model", static_model_summary_json(&m));
    if (ready) {
        char hash[65];
        json_free(run_spec(e, &s, &m, NULL, hash));
        json_set_string(v, "spec_hash", hash);
    }
    json_set(v, "assumptions", assumptions_json(e->proj));
    json_set_string(v, "next_step", ready ? "analysis_run starts the solve as a job" : "fix the listed errors (each carries a hint), then validate again");
    static_model_free(&m);
    op_succeed(out, v);
}

/* GOALS.md G17 step 9: a printed answer without walls and infill, asked on a voxel mesh of a part with surfaces inclined
 * to the grid, is solved on TET10, because the voxels' steps distort the stress there (step 8: the ratio 5 to 24 % low on
 * a ring and a hole, and 18 % higher than TET10's at the same size on a small fillet). The part is meshed as
 * mesh_generate with method tet and order 2 would mesh it, at the voxel size within 150 000 elements (the voxel
 * autosize's budget), the size growing by half again when the mesher refuses that many, at most three times (twice the
 * voxel size, tried first, left a coarse edge's peak 12 % low on a ring); the project keeps that mesh.
 * material_model.mesh "as_meshed" keeps the voxels. *record receives what was done (NULL when nothing applies); false
 * only for an invalid request. When TET10 meshing fails the voxels stay, and the record says why. */
static bool printed_mesh_choice(Engine *e, const JsonValue *mm, JsonValue **record, OpResult *out) {
    *record = NULL;
    if (!mm || mm->type != JSON_OBJECT || strcmp(json_get_str(mm, "type", ""), "orthotropic")) return true;
    const char *choice = json_get_str(mm, "mesh", "auto");
    if (strcmp(choice, "auto") && strcmp(choice, "as_meshed")) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "auto (the default) or as_meshed", "material_model.mesh '%s' is neither auto nor as_meshed", choice);
        return false;
    }
    Project *proj = e->proj;
    MeshState *ms = &proj->mesh;
    if (!strcmp(choice, "as_meshed") || json_get(mm, "walls") || json_get(mm, "infill") || !ms->valid || ms->hm.elem_type != SOLID_ELEM_HEX8 ||
        ms->include_plate)
        return true;
    int nstair = 0;
    for (int f = 0; f < ms->hm.nfaces; f++) {
        int bi = ms->hm.body[ms->hm.face_elem[f]];
        const Body *b = bi >= 0 && bi < ms->nbodies ? project_body(proj, ms->body_name[bi]) : NULL;
        if (b && b->role != ROLE_SUPPORT && mesh_face_staircase(&ms->hm, b, f) > 1 + 1e-9) nstair++;
    }
    if (!nstair) return true;
    double hv = fmin(ms->h[0], fmin(ms->h[1], ms->h[2])), hs = hv;
    int voxels = ms->hm.nelems;
    char why[600] = "";
    for (int attempt = 0; attempt < 4; attempt++, hs *= 1.5) {
        JsonValue *mp = json_object();
        char size[64];
        snprintf(size, sizeof size, "%.6g mm", 1e3 * hs);
        json_set_string(mp, "method", "tet"), json_set_string(mp, "surface_size", size), json_set_int(mp, "order", 2);
        json_set_int(mp, "max_elements", 150000);
        OpResult mr;
        memset(&mr, 0, sizeof mr);
        op_mesh_generate_call(e, mp, &mr);
        json_free(mp);
        if (mr.ok) {
            const JsonValue *me = json_get(mr.value, "mesh");
            JsonValue *r = json_object();
            json_set_string(r, "solved_on", "TET10");
            json_set_number(r, "surface_size_mm", 1e3 * hs);
            json_set_int(r, "elements", json_get_int(me, "elements", 0));
            json_set_number(r, "replaced_voxel_size_mm", 1e3 * hv);
            json_set_int(r, "replaced_voxels", voxels);
            json_set_int(r, "inclined_voxel_faces", nstair);
            json_set_string(r, "why", "the part has surfaces inclined to the voxel grid, whose steps distort a printed part's stress there (GOALS.md "
                                      "G17 step 8: the strength ratio 5 to 24 % low on a ring and a hole, 18 % higher than TET10's on a small "
                                      "fillet); TET10 follows the surface (within 1.2 % on that ring and hole), meshed at the voxel size within "
                                      "150 000 elements. The project keeps this mesh; mesh_generate makes voxels again for the "
                                      "analyses that need them, and material_model.mesh \"as_meshed\" solves on the mesh as it is (GOALS.md G17 "
                                      "step 9)");
            json_set_string(r, "check", "a fillet or hole only a few surface cells across reads differently at half the surface size "
                                        "(GOALS.md G17 step 9: a 5 mm fillet moved 8 % between 2.5 and 1.25 mm): before relying on a stress "
                                        "there, repeat with mesh_generate {method: tet, order: 2} at half this surface size");
            op_result_free(&mr);
            *record = r;
            return true;
        }
        snprintf(why, sizeof why, "%s", json_get_str(mr.error, "message", "TET10 meshing failed"));
        bool limit = !strcmp(json_get_str(mr.error, "code", ""), nv_err_name(NV_ERR_RESOURCE_LIMIT));
        op_result_free(&mr);
        if (!limit) break;
    }
    JsonValue *r = json_object();
    json_set_string(r, "solved_on", "voxels");
    json_set_string(r, "tet10_refused", why);
    json_set_int(r, "inclined_voxel_faces", nstair);
    json_set_string(r, "why", "TET10 meshing failed, so the voxels stay: where a surface is inclined to their grid the stated ratio reads low, "
                              "and the answer says where (GOALS.md G17 steps 8 and 9)");
    *record = r;
    return true;
}

static void op_analysis_run(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    const char *analysis = json_get_str(p, "analysis", "static_structural");
    if (strcmp(analysis, "static_structural") != 0) {
        op_thermal_submit(e, p, out, analysis);
        return;
    }
    Project *proj = e->proj;
    StaticSettings s;
    if (!parse_settings(out, p, &s)) return;
    char err[1024], tmp[NV_PATH_MAX];
    if (!engine_resolve_write_path(e, NULL, proj->dir, true, false, tmp, sizeof tmp, err, sizeof err)) {
        op_fail(out, NV_ERR_PERMISSION, "the project folder must be writable to store the run", "%s", err);
        return;
    }
    StaticResults *r = calloc(1, sizeof *r);
    if (!r) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    JsonValue *mesh_choice = NULL; /* a printed answer on a curved voxel part: TET10 first (G17 step 9) */
    if (!printed_mesh_choice(e, json_get(p, "material_model"), &mesh_choice, out)) {
        static_results_free(r);
        return;
    }
    JsonValue *errors = json_array(), *warnings = json_array();
    if (mesh_choice) {
        JsonValue *w = json_object();
        bool tet = !strcmp(json_get_str(mesh_choice, "solved_on", ""), "TET10");
        json_set_string(w, "code", tet ? "MESHED_TET10_FOR_PRINTED_ANSWER" : "TET10_MESHING_FAILED");
        json_set_string(w, "message", json_get_str(mesh_choice, "why", ""));
        json_push(warnings, w);
    }
    if (!static_model_build(proj, &s, &r->model, errors, warnings)) {
        const JsonValue *first = json_at(errors, 0);
        NvErr code = nv_err_from_name(json_get_str(first, "code", "PRECONDITION_FAILED"));
        char msg[1024];
        snprintf(msg, sizeof msg, "%s", json_get_str(first, "message", "the setup is invalid"));
        char hint[512];
        snprintf(hint, sizeof hint, "%s", json_get_str(first, "hint", "run setup_validate for the full report"));
        op_fail(out, code, hint, "the setup cannot be solved: %s%s", msg, json_len(errors) > 1 ? " (further problems in details.errors)" : "");
        op_fail_detail(out, "errors", errors);
        op_fail_detail(out, "warnings", warnings);
        static_results_free(r), json_free(mesh_choice);
        return;
    }
    json_free(errors);
    /* a printed part (G17): material_model "orthotropic" solves it as a printed solid with its strength criterion */
    const JsonValue *mm = json_get(p, "material_model");
    if (mm && mm->type == JSON_OBJECT && strcmp(json_get_str(mm, "type", "project_material"), "project_material")) {
        if (strcmp(json_get_str(mm, "type", ""), "orthotropic")) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "material_model.type is project_material or orthotropic", "unknown material model '%s'",
                    json_get_str(mm, "type", ""));
            json_free(warnings), static_results_free(r), json_free(mesh_choice);
            return;
        }
        if (!mech_static_printed_attach(proj, mm, r, out)) {
            json_free(warnings), static_results_free(r), json_free(mesh_choice);
            return;
        }
    }
    char hash[65];
    JsonValue *spec = run_spec(e, &s, &r->model, mech_static_printed_description(r), hash);
    char active[64];
    if (!json_get_bool(p, "allow_duplicate", false) && jobs_find_active(e->jobs, hash, active, sizeof active)) {
        JsonValue *v = json_object();
        json_set_string(v, "job_id", active);
        json_set_bool(v, "deduplicated", true);
        json_set_string(v, "spec_hash", hash);
        json_set_string(v, "message", "an identical analysis is already queued or running; its job is returned instead of starting a second one");
        if (mesh_choice) json_set(v, "mesh_choice", mesh_choice);
        json_free(spec), json_free(warnings);
        static_results_free(r);
        op_succeed(out, v);
        return;
    }
    char id[64], runs[NV_PATH_MAX], dir[NV_PATH_MAX], specpath[NV_PATH_MAX];
    jobs_new_id(e->jobs, id, sizeof id);
    path_join(runs, sizeof runs, proj->dir, "runs");
    path_join(dir, sizeof dir, runs, id);
    if (path_exists(dir) || !engine_resolve_write_path(e, NULL, dir, true, true, dir, sizeof dir, err, sizeof err)) {
        json_free(spec), json_free(warnings), json_free(mesh_choice);
        static_results_free(r);
        op_fail(out, NV_ERR_IO, NULL, "cannot create the run directory %s%s%s", dir, err[0] ? ": " : "", err);
        return;
    }
    char now[32];
    iso_time_now(now, sizeof now);
    json_set_string(spec, "job_id", id);
    json_set_string(spec, "created", now);
    json_set_int(spec, "project_revision", (long long)proj->revision);
    json_set_string(spec, "spec_hash", hash);
    json_set_string(spec, "spec_hash_covers", "every field except job_id, created, project_revision and spec_hash");
    path_join(specpath, sizeof specpath, dir, "spec.json");
    bool written = json_write_file(specpath, spec, JSON_PRETTY | JSON_SORTED);
    json_free(spec);
    if (!written) {
        json_free(warnings), json_free(mesh_choice);
        static_results_free(r);
        op_fail(out, NV_ERR_IO, NULL, "cannot write %s", specpath);
        return;
    }
    JsonValue *model = static_model_summary_json(&r->model), *warn_copy = json_clone(warnings);
    snprintf(r->job_id, sizeof r->job_id, "%s", id);
    snprintf(r->run_dir, sizeof r->run_dir, "%s", dir);
    r->build_warnings = warnings;
    JobSpec js = {id, "static_structural", json_get_str(p, "label", ""), dir, hash, proj->id, proj->name, proj->revision, r, static_job_run, static_results_free};
    if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
        json_free(model), json_free(warn_copy), json_free(mesh_choice);
        op_fail(out, NV_ERR_BUSY, "check job_list and retry when jobs have finished", "%s", err);
        return;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "state", "queued");
    json_set_string(v, "analysis", "static_structural");
    json_set_string(v, "run_directory", dir);
    json_set_string(v, "spec_file", specpath);
    json_set_string(v, "spec_hash", hash);
    json_set(v, "model", model);
    json_set(v, "warnings", warn_copy);
    if (mesh_choice) json_set(v, "mesh_choice", mesh_choice);
    json_set_string(v, "next_step",
                    "poll job_status with {\"job_id\": \"...\", \"wait_seconds\": 30} until the state is succeeded, failed or cancelled; then use "
                    "results_query, results_render and results_export");
    op_succeed(out, v);
}

static bool valid_job_id(const char *id) {
    if (!id || strncmp(id, "job-", 4) != 0 || strlen(id) > 63) return false;
    for (const char *c = id; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-')) return false;
    return true;
}

bool op_project_run_dir(Engine *e, const char *id, char *out, size_t cap) {
    bool ok = false;
    engine_lock(e);
    Project *proj = engine_project_locked(e);
    if (proj && valid_job_id(id)) {
        char runs[NV_PATH_MAX];
        ok = path_join(runs, sizeof runs, proj->dir, "runs") && path_join(out, cap, runs, id) && path_is_dir(out);
    }
    engine_unlock(e);
    return ok;
}

static void op_job_status(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    JsonValue *st = jobs_status_json(e->jobs, id, json_get_num(p, "wait_seconds", 0));
    if (st) {
        op_succeed(out, st);
        return;
    }
    char dir[NV_PATH_MAX], path[NV_PATH_MAX];
    if (op_project_run_dir(e, id, dir, sizeof dir)) {
        path_join(path, sizeof path, dir, "summary.json");
        JsonError je;
        JsonValue *summary = json_read_file(path, (size_t)64 << 20, &je);
        JsonValue *v = json_object();
        json_set_string(v, "job_id", id);
        json_set_string(v, "run_directory", dir);
        json_set_string(v, "source", "run directory of the open project (the job ran in an earlier session)");
        char err[512];
        ThermalCheckpoint *ck = summary ? NULL : thermal_checkpoint_read(dir, true, err, sizeof err);
        if (summary) {
            json_set_string(v, "state", "succeeded");
            json_set(v, "summary", summary);
        } else if (ck) {
            /* the process that ran the job ended before it finished (killed, crashed or shut down) */
            json_set_string(v, "state", "interrupted");
            json_set(v, "checkpoint", thermal_checkpoint_info_json(ck));
            json_set_string(v, "message", "the run stopped before it finished, but it has a checkpoint");
            JsonValue *rec = json_set_array(v, "recovery_options");
            json_push(rec, json_string("job_resume continues it from the checkpoint time, provided mesh, materials, conditions and settings are unchanged"));
            json_push(rec, json_string("analysis_run starts it again from the beginning"));
            thermal_checkpoint_free(ck);
        } else {
            json_set_string(v, "state", "unknown");
            json_set_string(v, "message", "the run directory has no readable summary.json and no checkpoint: the run did not finish");
        }
        op_succeed(out, v);
        return;
    }
    op_fail(out, NV_ERR_NOT_FOUND, "job_list shows the jobs of this session", "no job '%s' in this session or in the runs folder of the open project", id);
}

static void op_job_cancel(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    JobState st;
    if (!jobs_cancel(e->jobs, id, &st)) {
        op_fail(out, NV_ERR_NOT_FOUND, "job_list shows the jobs of this session", "no job '%s' in this session", id);
        return;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "state", job_state_name(st));
    json_set_string(v, "note",
                    st == JOB_RUNNING ? "cancellation requested: the solve stops at its next progress check; poll job_status"
                                      : (st == JOB_CANCELLED ? "the job is cancelled" : "the job had already finished and is unchanged"));
    op_succeed(out, v);
}

static void checkpoint_request(Engine *e, JsonValue *p, OpResult *out, bool pause) {
    const char *id = json_get_str(p, "job_id", "");
    JobState st;
    void *data = NULL;
    char kind[32] = "";
    if (!jobs_acquire(e->jobs, id, &st, &data, kind, sizeof kind, NULL, 0)) {
        op_fail(out, NV_ERR_NOT_FOUND, "job_list shows the jobs of this session", "no job '%s' in this session", id);
        return;
    }
    if (data) jobs_release(e->jobs, id);
    if (!transient_analysis_kind(kind)) {
        op_fail(out, NV_ERR_UNSUPPORTED, "a static analysis is one solve; cancel it with job_cancel if needed",
                "job '%s' is a %s analysis, which has no checkpoints", id, kind);
        return;
    }
    jobs_request_checkpoint(e->jobs, id, pause, &st);
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "state", job_state_name(st));
    bool active = st == JOB_RUNNING || st == JOB_QUEUED;
    json_set_bool(v, "requested", active);
    json_set_string(v, "note", !active ? "the job is not active: nothing to checkpoint"
                               : pause ? "the job writes a checkpoint at its next accepted step and stops in state paused; job_resume continues it"
                                       : "a checkpoint is written at the next accepted step and the job continues; job_status reports it under checkpoint");
    op_succeed(out, v);
}

static void op_job_checkpoint(Engine *e, JsonValue *p, OpResult *out) { checkpoint_request(e, p, out, false); }
static void op_job_pause(Engine *e, JsonValue *p, OpResult *out) { checkpoint_request(e, p, out, true); }

static void op_job_resume(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    op_thermal_resume(e, p, out);
}

static void op_job_list(Engine *e, JsonValue *p, OpResult *out) {
    JsonValue *v = json_object();
    json_set(v, "jobs", jobs_list_json(e->jobs, (int)json_get_int(p, "limit", 32)));
    op_succeed(out, v);
}

const OpBinding OPS_ANALYSIS_BINDINGS[] = {
    {"setup_validate", op_setup_validate},
    {"analysis_run", op_analysis_run},
    {"job_status", op_job_status},
    {"job_cancel", op_job_cancel},
    {"job_checkpoint", op_job_checkpoint},
    {"job_pause", op_job_pause},
    {"job_resume", op_job_resume},
    {"job_list", op_job_list},
    {NULL, NULL},
};
