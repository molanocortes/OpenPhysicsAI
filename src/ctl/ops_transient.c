/* ops_transient.c - operations for transient thermal and thermomechanical analyses: submission and results over time */
#include "../core/sha256.h"
#include "../fem/hex8.h"
#include "matlib.h"
#include "contact.h"
#include "ops_internal.h"
#include "transient_analysis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool op_thermal_settings(OpResult *out, const JsonValue *p, const char *analysis, ThermalSettings *s) {
    memset(s, 0, sizeof *s);
    s->cht = !strcmp(analysis, "conjugate_heat_transfer");
    s->mechanical = !strcmp(analysis, "thermomechanical") || (s->cht && json_get_bool(p, "structural_response", false));
    bool present = false;
    if (!op_quantity(out, p, "end_time", DIM_TIME, "s", &s->end_time, &present) ||
        !op_quantity(out, p, "time_step", DIM_TIME, "s", &s->time_step, &present))
        return false;
    s->theta = json_get_num(p, "theta", 1.0);
    s->consistent_capacity = !strcmp(json_get_str(p, "capacity", "lumped"), "consistent");
    s->output_every = (int)json_get_int(p, "output_every", 1);
    s->max_picard = (int)json_get_int(p, "max_iterations", 50);
    s->picard_tol = json_get_num(p, "temperature_tolerance", 1e-6);
    s->initial_temperature = 293.15;
    s->reference_temperature = 293.15;
    if (!op_quantity(out, p, "initial_temperature", DIM_TEMPERATURE, "degC", &s->initial_temperature, &present)) return false;
    double ref = s->initial_temperature;
    if (!op_quantity(out, p, "stress_free_temperature", DIM_TEMPERATURE, "degC", &ref, &present)) return false;
    s->reference_temperature = ref;
    s->formulation = strcmp(json_get_str(p, "formulation", "incompatible_modes"), "full_integration") ? HEX8_INCOMPATIBLE : HEX8_FULL;
    const char *sv = json_get_str(p, "solver", "auto");
    s->solver = !strcmp(sv, "direct") ? SOLID_SOLVER_DIRECT : (!strcmp(sv, "iterative") ? SOLID_SOLVER_PCG : SOLID_SOLVER_AUTO);
    s->pcg_tol = json_get_num(p, "tolerance", 1e-10);
    s->property_eval = strcmp(json_get_str(p, "property_evaluation", "quadrature_point"), "element_mean") ? THERMAL_PROP_QUADRATURE : THERMAL_PROP_ELEMENT;
    s->element_capacity = !strcmp(json_get_str(p, "capacity_model", "enthalpy_secant"), "element_rho_cp");
    s->phase_change = json_get_bool(p, "phase_change", true);
    /* ---- time stepping: fixed (default, as before) or adaptive with error control ---- */
    s->stepping = strcmp(json_get_str(p, "time_stepping", "fixed"), "adaptive") ? THERMAL_STEPPING_FIXED : THERMAL_STEPPING_ADAPTIVE;
    if (!op_quantity(out, p, "min_time_step", DIM_TIME, "s", &s->dt_min, &present) ||
        !op_quantity(out, p, "max_time_step", DIM_TIME, "s", &s->dt_max, &present) ||
        !op_quantity(out, p, "output_interval", DIM_TIME, "s", &s->output_interval, &present))
        return false;
    s->temporal_relative = json_get_num(p, "temporal_relative_tolerance", 1e-3);
    s->temporal_temperature = 1e-2;
    if (!op_quantity(out, p, "temporal_temperature_tolerance", DIM_TEMPERATURE_DIFFERENCE, "K", &s->temporal_temperature, &present) ||
        !op_quantity(out, p, "temporal_enthalpy_tolerance", DIM_ENERGY_DENSITY, "J/m^3", &s->temporal_enthalpy, &present))
        return false;
    s->temporal_temperature_only = !strcmp(json_get_str(p, "temporal_error_measure", "temperature_and_enthalpy"), "temperature");
    s->checkpoint_wall_interval = 60;
    if (!op_quantity(out, p, "checkpoint_interval", DIM_TIME, "s", &s->checkpoint_interval, &present) ||
        !op_quantity(out, p, "checkpoint_wall_interval", DIM_TIME, "s", &s->checkpoint_wall_interval, &present))
        return false;
    s->step_safety = json_get_num(p, "step_safety", 0);
    s->max_step_growth = json_get_num(p, "max_step_growth", 0);
    s->max_step_shrink = json_get_num(p, "max_step_shrink", 0);
    s->max_rejections = (int)json_get_int(p, "max_rejections", 0);
    s->max_steps = (long)json_get_int(p, "max_steps", 0);
    const JsonValue *ot = json_get(p, "output_times");
    if (ot) {
        if (json_len(ot) > TC_MAX_OUTPUT_TIMES) {
            op_fail(out, NV_ERR_OUT_OF_RANGE, "use output_interval for long regular series", "at most %d output_times (got %zu)", TC_MAX_OUTPUT_TIMES, json_len(ot));
            return false;
        }
        for (size_t i = 0; i < json_len(ot); i++) {
            char uerr[256];
            if (!quantity_from_json(json_at(ot, i), DIM_TIME, "s", &s->output_times[i], uerr, sizeof uerr)) {
                op_fail(out, NV_ERR_INVALID_UNIT, NULL, "output_times[%zu]: %s", i, uerr);
                return false;
            }
            if (i && !(s->output_times[i] > s->output_times[i - 1])) {
                op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "output_times must increase strictly (entry %zu is %g s after %g s)", i, s->output_times[i],
                        s->output_times[i - 1]);
                return false;
            }
        }
        s->noutput_times = (int)json_len(ot);
    }
    if (s->stepping == THERMAL_STEPPING_ADAPTIVE) {
        if (!(s->temporal_relative >= 0) || !(s->temporal_temperature >= 0) || !(s->temporal_relative > 0 || s->temporal_temperature > 0)) {
            op_fail(out, NV_ERR_OUT_OF_RANGE, "give a positive temporal_relative_tolerance or temporal_temperature_tolerance",
                    "adaptive stepping needs a temporal tolerance (relative %g, temperature %g K)", s->temporal_relative, s->temporal_temperature);
            return false;
        }
        if (s->dt_min > 0 && s->dt_max > 0 && s->dt_min > s->dt_max) {
            op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "min_time_step (%g s) exceeds max_time_step (%g s)", s->dt_min, s->dt_max);
            return false;
        }
    } else if (json_get(p, "temporal_relative_tolerance") || json_get(p, "temporal_temperature_tolerance") || json_get(p, "min_time_step") ||
               json_get(p, "max_time_step")) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "set time_stepping to \"adaptive\", or drop the adaptive settings",
                "temporal tolerances and step limits apply to adaptive stepping only; this run uses fixed steps");
        return false;
    }
    if (s->element_capacity && s->phase_change) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "use the default capacity_model 'enthalpy_secant', or set phase_change false",
                "capacity_model 'element_rho_cp' cannot carry latent heat: rho cp at the element mean temperature has no enthalpy jump to store it in");
        return false;
    }
    if (!(s->theta >= 0.5 && s->theta <= 1)) {
        op_fail(out, NV_ERR_OUT_OF_RANGE, "1 is backward Euler, 0.5 Crank-Nicolson", "theta must lie between 0.5 and 1");
        return false;
    }
    if (s->output_every < 1) {
        op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "output_every must be at least 1");
        return false;
    }
    /* ---- conjugate heat transfer: the flow domain and how fluid and solids are coupled ---- */
    if (!s->cht) {
        if (json_get(p, "flow") || json_get(p, "coupling") || json_get(p, "structural_response")) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "use analysis \"conjugate_heat_transfer\" for a resolved flow",
                    "flow, coupling and structural_response belong to the conjugate_heat_transfer analysis, not to %s", analysis);
            return false;
        }
        return true;
    }
    const JsonValue *fl = json_get(p, "flow");
    const char *fb = json_get_str(fl, "fluid_body", NULL);
    if (!fl || !fb || !fb[0]) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "give flow: {\"fluid_body\": ..., \"inlet_velocity\": \"0.05 m/s\", \"inlet_temperature\": \"20 degC\"}",
                "conjugate_heat_transfer needs flow.fluid_body, the body that is the flow domain");
        return false;
    }
    snprintf(s->fluid_body, sizeof s->fluid_body, "%s", fb);
    present = false;
    if (!op_quantity(out, fl, "inlet_velocity", DIM_SPEED, "m/s", &s->inlet_velocity, &present)) return false;
    if (!present || !(s->inlet_velocity > 0)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "give a positive flow.inlet_velocity (m/s along +x)", "the flow needs a positive inlet velocity");
        return false;
    }
    s->inlet_temperature = s->initial_temperature;
    if (!op_quantity(out, fl, "inlet_temperature", DIM_TEMPERATURE, "degC", &s->inlet_temperature, &present)) return false;
    if (!(s->inlet_temperature > 0)) {
        op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "the inlet temperature must lie above absolute zero");
        return false;
    }
    static const char *const SIDE[4] = {"y_min", "y_max", "z_min", "z_max"};
    const JsonValue *walls = json_get(fl, "walls");
    for (int w = 0; w < 4; w++) {
        const char *v = json_get_str(walls, SIDE[w], "no_slip");
        s->flow_wall[w] = !strcmp(v, "slip") ? 1 : (!strcmp(v, "periodic") ? 2 : 0);
    }
    if ((s->flow_wall[0] == 2) != (s->flow_wall[1] == 2) || (s->flow_wall[2] == 2) != (s->flow_wall[3] == 2)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "make both sides of an axis periodic, or neither", "a periodic flow side needs its opposite side periodic too");
        return false;
    }
    s->flow_steady_tolerance = json_get_num(fl, "steady_tolerance", 0);
    const JsonValue *cp = json_get(p, "coupling");
    const char *scheme = json_get_str(cp, "scheme", "partitioned");
    s->coupling_monolithic = !strcmp(scheme, "monolithic");
    s->coupling_relative = json_get_num(cp, "relative_tolerance", 1e-8);
    s->coupling_max_iterations = (int)json_get_int(cp, "max_iterations", 50);
    s->coupling_relaxation = json_get_num(cp, "relaxation", 0.5);
    if (!(s->coupling_relative > 0 && s->coupling_relative < 1e-2) || s->coupling_max_iterations < 2 || s->coupling_max_iterations > 1000 ||
        !(s->coupling_relaxation > 0 && s->coupling_relaxation <= 1) || !(s->flow_steady_tolerance >= 0 && s->flow_steady_tolerance < 1e-2)) {
        op_fail(out, NV_ERR_OUT_OF_RANGE, "coupling.relative_tolerance in (0, 0.01), max_iterations 2..1000, relaxation in (0, 1], flow.steady_tolerance in [0, 0.01)",
                "a coupling or flow setting is out of range");
        return false;
    }
    return true;
}

/* the hashable description of a transient run */
static JsonValue *thermal_spec(Engine *e, const ThermalSettings *s, const ThermalCase *c, const char *analysis, char hash[65]) {
    Project *proj = e->proj;
    JsonValue *spec = json_object();
    json_set_string(spec, "format", "navier-am-run-spec");
    json_set_int(spec, "format_version", 1);
    json_set_string(spec, "analysis", analysis);
    JsonValue *st = json_set_object(spec, "settings");
    json_set_number(st, "end_time_s", s->end_time);
    json_set_number(st, "time_step_s", s->time_step);
    json_set_number(st, "theta", s->theta);
    json_set_string(st, "capacity", s->consistent_capacity ? "consistent" : "lumped");
    json_set_number(st, "initial_temperature_c", s->initial_temperature - 273.15);
    json_set_int(st, "output_every", s->output_every);
    json_set_number(st, "temperature_tolerance_k", s->picard_tol);
    /* the constitutive choices change the answer, so they belong in the hashed specification */
    json_set_string(st, "property_evaluation", s->property_eval == THERMAL_PROP_ELEMENT ? "element_mean" : "quadrature_point");
    json_set_string(st, "capacity_model", s->element_capacity ? "element_rho_cp" : "enthalpy_secant");
    json_set_bool(st, "phase_change", s->phase_change);
    json_set_string(st, "time_stepping", s->stepping == THERMAL_STEPPING_ADAPTIVE ? "adaptive" : "fixed");
    if (s->stepping == THERMAL_STEPPING_ADAPTIVE) {
        JsonValue *ad = json_set_object(st, "adaptive");
        json_set_number(ad, "temporal_relative_tolerance", s->temporal_relative);
        json_set_number(ad, "temporal_temperature_tolerance_k", s->temporal_temperature);
        json_set_number(ad, "temporal_enthalpy_tolerance_j_m3", s->temporal_enthalpy);
        json_set_string(ad, "temporal_error_measure", s->temporal_temperature_only ? "temperature" : "temperature_and_enthalpy");
        json_set_number(ad, "min_time_step_s", s->dt_min);
        json_set_number(ad, "max_time_step_s", s->dt_max);
        json_set_number(ad, "step_safety", s->step_safety);
        json_set_number(ad, "max_step_growth", s->max_step_growth);
        json_set_number(ad, "max_step_shrink", s->max_step_shrink);
        json_set_int(ad, "max_rejections", s->max_rejections);
        json_set_int(ad, "max_steps", s->max_steps);
    }
    if (s->noutput_times > 0) json_set(st, "output_times_s", json_numbers(s->output_times, (size_t)s->noutput_times));
    if (s->output_interval > 0) json_set_number(st, "output_interval_s", s->output_interval);
    if (s->mechanical) {
        json_set_number(st, "stress_free_temperature_c", s->reference_temperature - 273.15);
        json_set_string(st, "formulation", static_formulation_name(s->formulation));
    }
    if (s->cht) {
        static const char *const SIDE[4] = {"y_min", "y_max", "z_min", "z_max"}, *const WALL[3] = {"no_slip", "slip", "periodic"};
        JsonValue *fl = json_set_object(st, "flow");
        json_set_string(fl, "fluid_body", s->fluid_body);
        json_set_number(fl, "inlet_velocity_m_s", s->inlet_velocity);
        json_set_number(fl, "inlet_temperature_c", s->inlet_temperature - 273.15);
        JsonValue *ws = json_set_object(fl, "walls");
        for (int w = 0; w < 4; w++) json_set_string(ws, SIDE[w], WALL[s->flow_wall[w]]);
        json_set_number(fl, "steady_tolerance", s->flow_steady_tolerance);
        json_set_string(fl, "solver", "D3Q19 lattice Boltzmann, regularized recursive collision, run to a steady state");
        json_set_string(fl, "energy_stabilization", "SUPG, time-step independent parameter");
        JsonValue *cpl = json_set_object(st, "coupling");
        json_set_string(cpl, "scheme", s->coupling_monolithic ? "monolithic" : "partitioned");
        json_set_number(cpl, "relative_tolerance", s->coupling_relative);
        json_set_int(cpl, "max_iterations", s->coupling_max_iterations);
        json_set_number(cpl, "relaxation", s->coupling_relaxation);
        json_set_bool(st, "structural_response", s->mechanical);
    }
    JsonValue *sw = json_set_object(spec, "software");
    json_set_string(sw, "name", "NAVIER-AM");
    json_set_string(sw, "version", NAVIER_AM_VERSION);
    json_set_string(sw, "contract_version", ops_contract_version());
    json_set_string(sw, "compiler", __VERSION__);
    json_set_string(sw, "floating_point", "IEEE 754 double precision; no -ffast-math; no floating-point contraction");
    JsonValue *pj = project_to_json(proj);
    json_remove(pj, "created");
    json_remove(pj, "modified");
    json_remove(pj, "revision");
    json_set(spec, "project", pj);
    JsonValue *mesh = json_set_object(spec, "mesh");
    json_set_string(mesh, "hash", c->mesh_hash);
    json_set_int(mesh, "nodes", c->nnodes);
    json_set_int(mesh, "elements", c->nelems);
    JsonValue *mats = json_set_array(spec, "materials_resolved");
    for (int i = 0; i < c->nmat; i++) {
        MaterialRecord rec;
        char err[300];
        MaterialAssignment *ma = project_material_for(proj, c->mat_id[i]);
        if (ma && material_lookup(proj->user_materials, ma->material, &rec, err, sizeof err)) json_push(mats, json_clone(rec.json));
    }
    size_t n = 0;
    char *text = json_dump(spec, JSON_SORTED, &n, NULL);
    sha256_hex_of(text ? text : "", text ? n : 0, hash);
    free(text);
    return spec;
}

void op_thermal_submit(Engine *e, JsonValue *p, OpResult *out, const char *analysis) {
    Project *proj = e->proj;
    ThermalSettings s;
    if (!op_thermal_settings(out, p, analysis, &s)) return;
    char err[1024], tmp[NV_PATH_MAX];
    if (!engine_resolve_write_path(e, NULL, proj->dir, true, false, tmp, sizeof tmp, err, sizeof err)) {
        op_fail(out, NV_ERR_PERMISSION, "the project folder must be writable to store the run", "%s", err);
        return;
    }
    ThermalCase *c = calloc(1, sizeof *c);
    if (!c) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    JsonValue *errors = json_array(), *warnings = json_array();
    if (!thermal_case_build(proj, &s, c, errors, warnings)) {
        const JsonValue *first = json_at(errors, 0);
        NvErr code = nv_err_from_name(json_get_str(first, "code", "PRECONDITION_FAILED"));
        char msg[1024], hint[512];
        snprintf(msg, sizeof msg, "%s", json_get_str(first, "message", "the setup is invalid"));
        snprintf(hint, sizeof hint, "%s", json_get_str(first, "hint", "run setup_validate for the full report"));
        op_fail(out, code, hint, "the setup cannot be solved: %s%s", msg, json_len(errors) > 1 ? " (further problems in details.errors)" : "");
        op_fail_detail(out, "errors", errors);
        op_fail_detail(out, "warnings", warnings);
        thermal_case_free(c);
        return;
    }
    json_free(errors);
    char hash[65];
    JsonValue *spec = thermal_spec(e, &s, c, analysis, hash);
    char active[64];
    if (!json_get_bool(p, "allow_duplicate", false) && jobs_find_active(e->jobs, hash, active, sizeof active)) {
        JsonValue *v = json_object();
        json_set_string(v, "job_id", active);
        json_set_bool(v, "deduplicated", true);
        json_set_string(v, "spec_hash", hash);
        json_set_string(v, "message", "an identical analysis is already queued or running; its job is returned instead of starting a second one");
        json_free(spec), json_free(warnings);
        thermal_case_free(c);
        op_succeed(out, v);
        return;
    }
    char id[64], runs[NV_PATH_MAX], dir[NV_PATH_MAX], specpath[NV_PATH_MAX], now[32];
    jobs_new_id(e->jobs, id, sizeof id);
    path_join(runs, sizeof runs, proj->dir, "runs");
    path_join(dir, sizeof dir, runs, id);
    if (path_exists(dir) || !engine_resolve_write_path(e, NULL, dir, true, true, dir, sizeof dir, err, sizeof err)) {
        json_free(spec), json_free(warnings);
        thermal_case_free(c);
        op_fail(out, NV_ERR_IO, NULL, "cannot create the run directory %s", dir);
        return;
    }
    iso_time_now(now, sizeof now);
    /* the request as validated (defaults applied), so that job_resume can rebuild exactly this run without any AI */
    JsonValue *req = json_clone(p);
    json_remove(req, "expected_revision");
    json_remove(req, "idempotency_key");
    json_remove(req, "allow_duplicate");
    json_set(spec, "request", req);
    JsonValue *hs = json_set_object(spec, "resolved_hashes");
    json_set_string(hs, "mesh", c->hashes.mesh);
    json_set_string(hs, "materials", c->hashes.materials);
    json_set_string(hs, "conditions", c->hashes.conditions);
    json_set_string(hs, "settings", c->hashes.settings);
    json_set_string(spec, "job_id", id);
    json_set_string(spec, "created", now);
    json_set_int(spec, "project_revision", (long long)proj->revision);
    json_set_string(spec, "spec_hash", hash);
    path_join(specpath, sizeof specpath, dir, "spec.json");
    bool written = json_write_file(specpath, spec, JSON_PRETTY | JSON_SORTED);
    json_free(spec);
    if (!written) {
        json_free(warnings);
        thermal_case_free(c);
        op_fail(out, NV_ERR_IO, NULL, "cannot write %s", specpath);
        return;
    }
    JsonValue *model = thermal_case_model_json(c), *warn_copy = json_clone(warnings);
    snprintf(c->job_id, sizeof c->job_id, "%s", id);
    snprintf(c->run_dir, sizeof c->run_dir, "%s", dir);
    c->build_warnings = warnings;
    JobSpec js = {id, analysis, json_get_str(p, "label", ""), dir, hash, proj->id, proj->name, proj->revision, c, thermal_job_run, thermal_case_free};
    if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
        json_free(model), json_free(warn_copy);
        op_fail(out, NV_ERR_BUSY, "check job_list and retry when jobs have finished", "%s", err);
        return;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "state", "queued");
    json_set_string(v, "analysis", analysis);
    json_set_string(v, "run_directory", dir);
    json_set_string(v, "spec_file", specpath);
    json_set_string(v, "spec_hash", hash);
    json_set(v, "model", model);
    json_set(v, "warnings", warn_copy);
    json_set_string(v, "next_step",
                    "poll job_status with {\"job_id\": \"...\", \"wait_seconds\": 30}; then results_query, results_probe, results_render and results_export "
                    "accept a time_s or time_index");
    op_succeed(out, v);
}

/* ---- results ------------------------------------------------------------------------------------ */

bool op_thermal_job(Engine *e, const char *id) {
    JobState st;
    void *data = NULL;
    char kind[32] = "", dir[NV_PATH_MAX] = "";
    if (jobs_acquire(e->jobs, id, &st, &data, kind, sizeof kind, dir, sizeof dir)) {
        if (data) jobs_release(e->jobs, id);
        return transient_results_kind(kind); /* print-analysis: a print writes the same result file */
    }
    /* unknown to this session: a transient run directory holds results.nvt */
    char path[NV_PATH_MAX];
    if (!op_project_run_dir(e, id, dir, sizeof dir)) return false;
    path_join(path, sizeof path, dir, "results.nvt");
    return path_is_file(path);
}

static ThermalCase *acquire_thermal(Engine *e, const char *id, OpResult *out) {
    JobState st = JOB_QUEUED;
    void *data = NULL;
    char dir[NV_PATH_MAX] = "", kind[32] = "";
    bool known = jobs_acquire(e->jobs, id, &st, &data, kind, sizeof kind, dir, sizeof dir);
    if (data) return data;
    if (known && st != JOB_SUCCEEDED) {
        op_fail(out, NV_ERR_PRECONDITION,
                st == JOB_QUEUED || st == JOB_RUNNING                  ? "wait with job_status {\"wait_seconds\": 30}"
                : st == JOB_PAUSED || st == JOB_CANCELLED ? "job_resume continues the run from its checkpoint; a partial history is not published as results"
                                                                       : "job_status shows why it did not finish",
                "job '%s' is %s and has no results", id, job_state_name(st));
        return NULL;
    }
    if (!known && !op_project_run_dir(e, id, dir, sizeof dir)) {
        op_fail(out, NV_ERR_NOT_FOUND, "job_list shows the jobs of this session", "no job '%s' in this session or in the runs folder of the open project", id);
        return NULL;
    }
    char path[NV_PATH_MAX], err[700];
    path_join(path, sizeof path, dir, "results.nvt");
    ThermalCase *c = thermal_results_load(path, err, sizeof err);
    if (!c) {
        op_fail(out, NV_ERR_IO, NULL, "cannot load the results of '%s': %s", id, err);
        return NULL;
    }
    snprintf(c->run_dir, sizeof c->run_dir, "%s", dir);
    if (!c->job_id[0]) snprintf(c->job_id, sizeof c->job_id, "%s", id);
    ThermalCase *use = jobs_attach(e->jobs, id,
                                   c->elem_birth ? "fff_print" /* print-analysis: element activation marks a print */
                                                 : (c->settings.cht ? "conjugate_heat_transfer" : (c->settings.mechanical ? "thermomechanical" : "transient_thermal")),
                                   dir, c, thermal_case_free,
                                   c->summary ? json_clone(c->summary) : NULL);
    if (!use) op_fail(out, NV_ERR_BUSY, NULL, "cannot register the results of '%s'", id);
    return use;
}

/* stored time selected by index or by the nearest stored time to time_s; -1 when the request is invalid */
static int pick_time(const ThermalCase *c, const JsonValue *p, OpResult *out) {
    const JsonValue *idx = json_get(p, "time_index"), *ts = json_get(p, "time_s");
    if (idx && ts) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "give time_index or time_s, not both");
        return -1;
    }
    if (idx) {
        long long i = (long long)idx->u.number;
        if (i < 0 || i >= c->noutputs) {
            op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "time_index %lld is outside 0..%d", i, c->noutputs - 1);
            return -1;
        }
        return (int)i;
    }
    if (ts) {
        int best = 0;
        for (int i = 1; i < c->noutputs; i++)
            if (fabs(c->times[i] - ts->u.number) < fabs(c->times[best] - ts->u.number)) best = i;
        return best;
    }
    return c->noutputs - 1; /* the end of the analysis */
}

typedef struct {
    int kind; /* 0 temperature, 1 displacement, 2 von Mises */
    int comp; /* displacement: 0-2 or 3 = magnitude */
    const char *unit, *label;
    double scale, offset;
} TQuantity;

static bool thermal_quantity(OpResult *out, const JsonValue *p, const ThermalCase *c, TQuantity *q) {
    const char *name = json_get_str(p, "quantity", "temperature"), *comp = json_get_str(p, "component", NULL);
    memset(q, 0, sizeof *q);
    if (!strcmp(name, "temperature")) {
        if (!c->T) { /* lpbf-build: a build stores no temperature field */
            op_fail(out, NV_ERR_UNSUPPORTED, "ask for displacement or von_mises", "this analysis stores no temperature field");
            return false;
        }
        q->kind = 0, q->unit = "degC", q->label = "temperature", q->scale = 1, q->offset = -273.15;
        return true;
    }
    if (!c->has_mech) {
        op_fail(out, NV_ERR_UNSUPPORTED, "run a thermomechanical analysis to get displacements and stresses", "this run stored temperatures only");
        return false;
    }
    if (!strcmp(name, "displacement")) {
        q->kind = 1, q->unit = "mm", q->label = "displacement", q->scale = 1e3;
        q->comp = !comp || !strcmp(comp, "magnitude") ? 3 : (!strcmp(comp, "x") ? 0 : (!strcmp(comp, "y") ? 1 : (!strcmp(comp, "z") ? 2 : -1)));
        if (q->comp < 0) {
            op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "component must be magnitude, x, y or z");
            return false;
        }
        return true;
    }
    if (!strcmp(name, "von_mises")) {
        q->kind = 2, q->unit = "MPa", q->label = "von Mises stress", q->scale = 1e-6;
        return true;
    }
    op_fail(out, NV_ERR_INVALID_PARAMS, "temperature, displacement or von_mises", "unknown quantity '%s' for a transient analysis", name);
    return false;
}

static double thermal_node_value(const ThermalCase *c, const TQuantity *q, int time, int nd) {
    if (q->kind == 0) return c->T[(size_t)time * (size_t)c->nnodes + (size_t)nd] + q->offset;
    if (q->kind == 2) return q->scale * c->mech_vm[(size_t)time * (size_t)c->nnodes + (size_t)nd];
    const double *u = c->mech_u + (size_t)time * 3 * (size_t)c->nnodes + 3 * (size_t)nd;
    return q->scale * (q->comp == 3 ? sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) : u[q->comp]);
}

void op_thermal_query(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    ThermalCase *c = acquire_thermal(e, id, out);
    if (!c) return;
    TQuantity q;
    int time = 0;
    if (!thermal_quantity(out, p, c, &q) || (time = pick_time(c, p, out)) < 0) {
        jobs_release(e->jobs, id);
        return;
    }
    const char *selname = json_get_str(p, "selection", NULL);
    const SaSet *set = NULL;
    if (selname) {
        for (int i = 0; i < c->nsets; i++)
            if (!strcmp(c->set[i].name, selname)) set = &c->set[i];
        if (!set) {
            jobs_release(e->jobs, id);
            op_fail(out, NV_ERR_NOT_FOUND, "use a selection that was mapped onto the mesh when the analysis ran", "selection '%s' is not part of this analysis",
                    selname);
            return;
        }
    }
    int n = set ? set->nnodes : c->nnodes;
    double lo = INFINITY, hi = -INFINITY, sum = 0;
    int imin = 0, imax = 0;
    for (int i = 0; i < n; i++) {
        int nd = set ? set->nodes[i] : i;
        double v = thermal_node_value(c, &q, time, nd);
        sum += v;
        if (v < lo) lo = v, imin = nd;
        if (v > hi) hi = v, imax = nd;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "quantity", q.label);
    json_set_string(v, "unit", q.unit);
    json_set_int(v, "time_index", time);
    json_set_number(v, "time_s", c->times[time]);
    json_set_string(v, "scope", set ? selname : "whole model");
    json_set_int(v, "count", n);
    JsonValue *st = json_set_object(v, "statistics");
    json_set_number(st, "min", lo);
    json_set_number(st, "max", hi);
    json_set_number(st, "mean", n ? sum / n : 0);
    json_set(st, "min_at_mm", json_vec3(1e3 * c->xyz[3 * (size_t)imin], 1e3 * c->xyz[3 * (size_t)imin + 1], 1e3 * c->xyz[3 * (size_t)imin + 2]));
    json_set(st, "max_at_mm", json_vec3(1e3 * c->xyz[3 * (size_t)imax], 1e3 * c->xyz[3 * (size_t)imax + 1], 1e3 * c->xyz[3 * (size_t)imax + 2]));
    if (json_get_bool(p, "history", false)) {
        JsonValue *hist = json_set_array(v, "history");
        for (int t = 0; t < c->noutputs; t++) {
            double a = INFINITY, b = -INFINITY;
            for (int i = 0; i < n; i++) {
                double val = thermal_node_value(c, &q, t, set ? set->nodes[i] : i);
                a = fmin(a, val), b = fmax(b, val);
            }
            JsonValue *row = json_object();
            json_set_number(row, "time_s", c->times[t]);
            json_set_number(row, "min", a);
            json_set_number(row, "max", b);
            json_push(hist, row);
        }
    }
    JsonValue *notes = json_set_array(v, "notes");
    json_push(notes, json_string("Temperatures are nodal values of the conduction solution; the voxel boundary is a staircase, so surface temperatures on "
                                 "inclined or curved faces depend on the element size."));
    if (c->has_mech)
        json_push(notes, json_string("Stresses come from the sequential elastic step at this time; they are not residual stresses (no plasticity, no element "
                                     "activation)."));
    jobs_release(e->jobs, id);
    op_succeed(out, v);
}

void op_thermal_render(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    ViewOptions o;
    if (!op_parse_view(out, p, &o)) return;
    ThermalCase *c = acquire_thermal(e, id, out);
    if (!c) return;
    TQuantity q;
    int time = 0;
    if (!thermal_quantity(out, p, c, &q) || (time = pick_time(c, p, out)) < 0) {
        jobs_release(e->jobs, id);
        return;
    }
    double *val = malloc((size_t)(c->nnodes ? c->nnodes : 1) * sizeof(double));
    if (!val) {
        jobs_release(e->jobs, id);
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    /* a build stores when each element is born and when it is cut away: a stored time is drawn with the elements that
     * exist then, and its range and maximum are those of the nodes that exist then */
    /* "groups": which of part, support and plate are drawn (all three by default) */
    bool group_shown[3] = {true, true, true};
    const JsonValue *groups = json_get(p, "groups");
    if (groups && json_len(groups) > 0) {
        group_shown[0] = group_shown[1] = group_shown[2] = false;
        for (size_t gi = 0; gi < json_len(groups); gi++) {
            const char *gn = json_str(json_at(groups, gi)) ? json_str(json_at(groups, gi)) : "";
            if (!strcmp(gn, "part")) group_shown[0] = true;
            else if (!strcmp(gn, "support")) group_shown[1] = true;
            else if (!strcmp(gn, "plate")) group_shown[2] = true;
        }
    }
    unsigned char *alive = NULL, *node_alive = NULL;
    int *live_elem = NULL, nlive = -1;
    unsigned char *live_local = NULL;
    if (c->elem_birth) {
        alive = calloc((size_t)(c->nelems ? c->nelems : 1), 1), node_alive = calloc((size_t)(c->nnodes ? c->nnodes : 1), 1);
        if (alive && node_alive) {
            for (int el = 0; el < c->nelems; el++) {
                bool born = c->elem_birth[el] >= 0 && c->elem_birth[el] <= time;
                bool gone = c->elem_death && c->elem_death[el] >= 0 && c->elem_death[el] <= time;
                alive[el] = born && !gone && (!c->elem_group || group_shown[c->elem_group[el] < 3 ? c->elem_group[el] : 0]);
                if (alive[el])
                    for (int k2 = 0; k2 < 8; k2++) node_alive[c->conn[8 * (size_t)el + k2]] = 1;
            }
            nlive = view_faces_of_alive(c->conn, 0, c->nelems, alive, &live_elem, &live_local);
        }
    }
    double lo = INFINITY, hi = -INFINITY;
    int imax = 0;
    for (int n = 0; n < c->nnodes; n++) {
        val[n] = thermal_node_value(c, &q, time, n);
        if (node_alive && nlive >= 0 && !node_alive[n]) continue;
        if (val[n] > hi) hi = val[n], imax = n;
        lo = fmin(lo, val[n]);
    }
    if (!(hi >= lo)) lo = 0, hi = 1;
    const JsonValue *range = json_get(p, "range");
    double rg[2];
    if (range && json_get_numbers(range, rg, 2) && rg[1] > rg[0]) lo = rg[0], hi = rg[1];
    const char *cm = json_get_str(p, "colormap", q.kind == 0 ? "inferno" : "turbo");
    SwColormap cmap = sw_colormap_find(cm) >= 0 ? (SwColormap)sw_colormap_find(cm) : SW_CMAP_TURBO; /* the renderer's own names */
    char title[256], marker[64];
    if (!o.title[0]) snprintf(title, sizeof title, "%s at t = %.4g s", q.label, c->times[time]);
    else snprintf(title, sizeof title, "%s", o.title);
    snprintf(marker, sizeof marker, "max %.4g %s", val[imax], q.unit);
    const double *u = c->has_mech && json_get_bool(p, "show_deformation", false) ? c->mech_u + (size_t)time * 3 * (size_t)c->nnodes : NULL;
    double scale = u ? json_get_num(p, "deformation_scale", 1.0) : 0;
    FieldView fv = {c->xyz, u,     c->conn, nlive >= 0 ? live_elem : c->face_elem, nlive >= 0 ? live_local : c->face_local, nlive >= 0 ? nlive : c->nfaces, val,   lo,     hi,
                    scale,  false, cmap,    title,        q.label,       q.unit,    c->xyz + 3 * (size_t)imax, marker};
    ViewResult vr;
    char err[512];
    bool ok = view_render_field(&fv, &o, &vr, err, sizeof err);
    double vmax = val[imax];
    free(val), free(alive), free(node_alive), free(live_elem), free(live_local);
    if (!ok) {
        jobs_release(e->jobs, id);
        op_fail(out, NV_ERR_PRECONDITION, NULL, "%s", err);
        return;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "quantity", q.label);
    json_set_int(v, "time_index", time);
    json_set_number(v, "time_s", c->times[time]);
    JsonValue *lg = json_set_object(v, "legend");
    json_set_number(lg, "min", lo);
    json_set_number(lg, "max", hi);
    json_set_string(lg, "unit", q.unit);
    json_set_string(lg, "colormap", cm);
    json_set_number(v, "max_value", vmax);
    json_set_string(v, "caution", "A colour image is not evidence of correctness: check the energy balance in the summary and the warnings.");
    engine_lock(e);
    op_finish_view(e, out, &vr, "result", id, "result.png", v);
    engine_unlock(e);
    jobs_release(e->jobs, id);
    op_succeed(out, v);
}

void op_thermal_probe(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    ThermalCase *c = acquire_thermal(e, id, out);
    if (!c) return;
    int time = pick_time(c, p, out);
    if (time < 0) {
        jobs_release(e->jobs, id);
        return;
    }
    const JsonValue *pts = json_get(p, "points_mm");
    bool history = json_get_bool(p, "history", false);
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_int(v, "time_index", time);
    json_set_number(v, "time_s", c->times[time]);
    JsonValue *arr = json_set_array(v, "probes");
    for (size_t i = 0; i < json_len(pts); i++) {
        double x[3];
        json_get_numbers(json_at(pts, i), x, 3);
        for (int k = 0; k < 3; k++) x[k] *= 1e-3;
        JsonValue *o = json_object();
        json_set(o, "point_mm", json_vec3(1e3 * x[0], 1e3 * x[1], 1e3 * x[2]));
        int found = -1;
        double N[8];
        for (int el = 0; el < c->nelems && found < 0; el++) {
            double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY}, X[8][3];
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) {
                    X[a][k] = c->xyz[3 * (size_t)c->conn[8 * (size_t)el + a] + k];
                    lo[k] = fmin(lo[k], X[a][k]), hi[k] = fmax(hi[k], X[a][k]);
                }
            double tol = 1e-9 * fmax(hi[0] - lo[0], 1e-12);
            if (x[0] < lo[0] - tol || x[0] > hi[0] + tol || x[1] < lo[1] - tol || x[1] > hi[1] + tol || x[2] < lo[2] - tol || x[2] > hi[2] + tol) continue;
            double xi[3];
            for (int k = 0; k < 3; k++) xi[k] = hi[k] > lo[k] ? 2 * (x[k] - lo[k]) / (hi[k] - lo[k]) - 1 : 0;
            double dN[8][3];
            hex8_shape(xi[0], xi[1], xi[2], N, dN);
            found = el;
        }
        if (found < 0) {
            json_set_bool(o, "inside", false);
            json_set_string(o, "note", "the point is outside the meshed volume");
            json_push(arr, o);
            continue;
        }
        json_set_bool(o, "inside", true);
        json_set_int(o, "element", found);
        if (c->T) { /* lpbf-build: a build stores no temperature field */
            double temp = 0;
            for (int a = 0; a < 8; a++) temp += N[a] * c->T[(size_t)time * (size_t)c->nnodes + (size_t)c->conn[8 * (size_t)found + a]];
            json_set_number(o, "temperature_c", temp - 273.15);
        }
        if (c->has_mech) {
            double u[3] = {0, 0, 0}, vm = 0;
            for (int a = 0; a < 8; a++) {
                int nd = c->conn[8 * (size_t)found + a];
                const double *un = c->mech_u + (size_t)time * 3 * (size_t)c->nnodes + 3 * (size_t)nd;
                for (int k = 0; k < 3; k++) u[k] += N[a] * un[k];
                vm += N[a] * c->mech_vm[(size_t)time * (size_t)c->nnodes + (size_t)nd];
            }
            JsonValue *d = json_set_object(o, "displacement_mm");
            json_set_number(d, "x", 1e3 * u[0]);
            json_set_number(d, "y", 1e3 * u[1]);
            json_set_number(d, "z", 1e3 * u[2]);
            json_set_number(o, "von_mises_mpa", 1e-6 * vm);
        }
        if (history) {
            JsonValue *hs = json_set_array(o, "history");
            for (int t = 0; t < c->noutputs; t++) {
                JsonValue *row = json_object();
                json_set_number(row, "time_s", c->times[t]);
                if (c->T) { /* lpbf-build: a build stores no temperature field */
                    double tt = 0;
                    for (int a = 0; a < 8; a++) tt += N[a] * c->T[(size_t)t * (size_t)c->nnodes + (size_t)c->conn[8 * (size_t)found + a]];
                    json_set_number(row, "temperature_c", tt - 273.15);
                }
                if (c->has_mech) { /* the quantity a build has: the vertical displacement of the point */
                    double uz = 0;
                    for (int a = 0; a < 8; a++)
                        uz += N[a] * c->mech_u[(size_t)t * 3 * (size_t)c->nnodes + 3 * (size_t)c->conn[8 * (size_t)found + a] + 2];
                    json_set_number(row, "uz_mm", 1e3 * uz);
                }
                json_push(hs, row);
            }
        }
        json_push(arr, o);
    }
    jobs_release(e->jobs, id);
    op_succeed(out, v);
}

void op_thermal_export(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    ThermalCase *c = acquire_thermal(e, id, out);
    if (!c) return;
    char dir[NV_PATH_MAX], err[700];
    const char *dest = json_get_str(p, "directory", NULL);
    if (dest) {
        if (!engine_resolve_write_path(e, NULL, dest, true, true, dir, sizeof dir, err, sizeof err)) {
            jobs_release(e->jobs, id);
            op_fail(out, NV_ERR_PERMISSION, NULL, "%s", err);
            return;
        }
    } else {
        snprintf(dir, sizeof dir, "%s", c->run_dir);
    }
    if (!dir[0]) {
        jobs_release(e->jobs, id);
        op_fail(out, NV_ERR_PRECONDITION, "pass directory", "the job has no run directory");
        return;
    }
    const JsonValue *formats = json_get(p, "formats");
    JsonValue *files = json_array();
    for (size_t i = 0; i < json_len(formats); i++) {
        const char *fmt = json_str(json_at(formats, i));
        char path[NV_PATH_MAX], hex[65] = "";
        uint64_t bytes = 0;
        bool ok = true;
        int written = 0;
        if (!strcmp(fmt, "vtu")) {
            ok = thermal_export_vtu_series(c, dir, &written, err, sizeof err);
            path_join(path, sizeof path, dir, "series.pvd");
        } else if (!strcmp(fmt, "csv")) {
            path_join(path, sizeof path, dir, "history.csv");
            ok = thermal_export_csv(c, path, err, sizeof err);
        } else {
            path_join(path, sizeof path, dir, "summary.json");
            ok = c->summary && json_write_file(path, c->summary, JSON_PRETTY | JSON_SORTED);
        }
        if (!ok) {
            json_free(files);
            jobs_release(e->jobs, id);
            op_fail(out, NV_ERR_IO, NULL, "cannot export %s: %s", fmt, err[0] ? err : "write failed");
            return;
        }
        sha256_file(path, hex, &bytes);
        JsonValue *fo = json_object();
        json_set_string(fo, "format", fmt);
        json_set_string(fo, "path", path);
        json_set_int(fo, "bytes", (long long)bytes);
        json_set_string(fo, "sha256", hex);
        if (written) json_set_int(fo, "files_written", written);
        json_set_string(fo, "contents",
                        !strcmp(fmt, "vtu") ? "one VTK XML file per stored time (temperature_c, and displacement and von Mises for thermomechanical runs) "
                                              "with a series.pvd collection carrying the times"
                        : !strcmp(fmt, "csv") ? "one row per stored time: temperature range and, when present, the mechanical peaks"
                                              : "analysis summary: model, history, energy balance, warnings and interpretation");
        json_push(files, fo);
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "directory", dir);
    json_set(v, "files", files);
    jobs_release(e->jobs, id);
    op_succeed(out, v);
}

/* ---- interfaces ---------------------------------------------------------------------------------------------- */

void op_thermal_interface_results(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    ThermalCase *c = acquire_thermal(e, id, out);
    if (!c) return;
    if (c->ncontacts == 0) {
        jobs_release(e->jobs, id);
        op_fail(out, NV_ERR_PRECONDITION, "declare interfaces with interface_define before running", "run '%s' has no thermal interfaces", id);
        return;
    }
    int time = pick_time(c, p, out);
    if (time < 0) {
        jobs_release(e->jobs, id);
        return;
    }
    const char *want = json_get_str(p, "interface", NULL);
    size_t nn = (size_t)c->nnodes;
    const double *T = c->T + (size_t)time * nn;
    JsonValue *v = json_object(), *arr = json_set_array(v, "interfaces");
    json_set_string(v, "job_id", id);
    json_set_int(v, "time_index", time);
    json_set_number(v, "time_s", c->times[time]);
    int found = 0;
    for (int i = 0; i < c->ncontacts; i++) {
        if (want && strcmp(want, c->contact[i].name)) continue;
        found++;
        JsonValue *o = json_object();
        json_set_string(o, "name", c->contact[i].name);
        json_set_string(o, "model", contact_model_name((ContactModel)c->contact[i].model));
        json_set_string(o, "body_a", c->contact[i].body_a);
        json_set_string(o, "body_b", c->contact[i].body_b);
        json_set_int(o, "faces", c->contact[i].faces);
        json_set_number(o, "area_mm2", 1e6 * c->contact[i].area);
        if (c->contact[i].model == CONTACT_PERFECT) {
            json_set_number(o, "temperature_jump_k", 0);
            json_set_string(o, "note", "perfect contact shares nodes: there is no jump, and the heat crossing it is not a measured quantity");
            json_push(arr, o);
            continue;
        }
        double A = 0, TaA = 0, TbA = 0, rate = 0, amin = INFINITY, amax = -INFINITY, bmin = INFINITY, bmax = -INFINITY, jmin = INFINITY, jmax = -INFINITY;
        int npairs = 0;
        for (int k = 0; k < c->ninterface; k++) {
            if (c->interface_contact[k] != i) continue;
            const ThermalInterfaceNode *it = &c->interfaces[k];
            double ta = T[it->node_a], tb = T[it->node_b];
            A += it->area, TaA += it->area * ta, TbA += it->area * tb;
            rate += it->conductance * it->area * (tb - ta);
            amin = fmin(amin, ta), amax = fmax(amax, ta), bmin = fmin(bmin, tb), bmax = fmax(bmax, tb);
            jmin = fmin(jmin, ta - tb), jmax = fmax(jmax, ta - tb);
            npairs++;
        }
        if (!(A > 0)) {
            json_set_string(o, "note", "no node pairs were recorded for this interface");
            json_push(arr, o);
            continue;
        }
        JsonValue *sa = json_set_object(o, "side_a");
        json_set_number(sa, "mean_c", TaA / A - 273.15);
        json_set_number(sa, "min_c", amin - 273.15);
        json_set_number(sa, "max_c", amax - 273.15);
        JsonValue *sb = json_set_object(o, "side_b");
        json_set_number(sb, "mean_c", TbA / A - 273.15);
        json_set_number(sb, "min_c", bmin - 273.15);
        json_set_number(sb, "max_c", bmax - 273.15);
        JsonValue *jp = json_set_object(o, "temperature_jump_a_minus_b_k");
        json_set_number(jp, "mean", (TaA - TbA) / A);
        json_set_number(jp, "min", jmin);
        json_set_number(jp, "max", jmax);
        json_set_number(o, "heat_rate_b_to_a_w", rate);
        if (c->contact[i].model != CONTACT_INSULATED) json_set_number(o, "conductance_w_m2k", c->contact[i].conductance);
        json_set_int(o, "node_pairs", npairs);
        json_set_number(o, "pair_area_mm2", 1e6 * A);
        json_set_number(o, "cumulative_heat_b_to_a_j", c->group_budget.interface_heat[i]);
        json_set_string(o, "integration",
                        "the interface flux is lumped at node pairs: each pair carries a quarter of the area of every interface face around it, so "
                        "the total conductance is h times the interface area regardless of the node count");
        json_push(arr, o);
    }
    if (!found) {
        json_free(v);
        jobs_release(e->jobs, id);
        op_fail(out, NV_ERR_NOT_FOUND, "interface_list shows the interfaces", "run '%s' has no interface named '%s'", id, want ? want : "");
        return;
    }
    const JsonValue *bodies = c->summary ? json_get(c->summary, "bodies") : NULL;
    if (bodies) json_set(v, "body_energy_balances", json_clone(bodies));
    json_set_string(v, "reading",
                    "heat_rate_b_to_a_w is instantaneous at time_s; cumulative_heat_b_to_a_j covers the whole run and is checked independently by "
                    "each body's own energy balance (body_energy_balances: stored - sources - boundary - prescribed = through_interfaces)");
    jobs_release(e->jobs, id);
    op_succeed(out, v);
}

/* ---- resume ------------------------------------------------------------------------------------------------------ */

static void mismatch(JsonValue *arr, const char *component, const char *then, const char *now, const char *what, const char *hint) {
    JsonValue *o = json_object();
    json_set_string(o, "component", component);
    json_set_string(o, "checkpoint_hash", then);
    json_set_string(o, "current_hash", now);
    json_set_string(o, "changed", what);
    json_set_string(o, "hint", hint);
    json_push(arr, o);
}

void op_thermal_resume(Engine *e, JsonValue *p, OpResult *out) {
    Project *proj = e->proj;
    const char *id = json_get_str(p, "job_id", "");
    JobState st = JOB_QUEUED;
    void *data = NULL;
    char kind[32] = "", jdir[NV_PATH_MAX] = "";
    bool known = jobs_acquire(e->jobs, id, &st, &data, kind, sizeof kind, jdir, sizeof jdir);
    if (data) jobs_release(e->jobs, id);
    if (known && (st == JOB_QUEUED || st == JOB_RUNNING)) {
        JsonValue *v = json_object();
        json_set_string(v, "job_id", id);
        json_set_string(v, "state", job_state_name(st));
        json_set_bool(v, "deduplicated", true);
        json_set_string(v, "note", "the job is already active; nothing was resumed");
        op_succeed(out, v);
        return;
    }
    if (known && st == JOB_SUCCEEDED) {
        op_fail(out, NV_ERR_PRECONDITION, "query its results with results_query", "job '%s' already completed", id);
        return;
    }
    if (known && !transient_analysis_kind(kind)) {
        op_fail(out, NV_ERR_UNSUPPORTED, "run the analysis again with analysis_run", "job '%s' is a %s analysis, which has no checkpoints", id, kind);
        return;
    }
    char runs[NV_PATH_MAX], dir[NV_PATH_MAX], path[NV_PATH_MAX], err[1024];
    if (strncmp(id, "job-", 4) || strchr(id, '/') || strchr(id, '.') || !path_join(runs, sizeof runs, proj->dir, "runs") ||
        !path_join(dir, sizeof dir, runs, id) || !path_is_dir(dir)) {
        op_fail(out, NV_ERR_NOT_FOUND, "open the project the job ran in (project_open), then retry", "no run directory for '%s' in the open project", id);
        return;
    }
    path_join(path, sizeof path, dir, "results.nvt");
    if (path_is_file(path)) {
        op_fail(out, NV_ERR_PRECONDITION, "query its results with results_query", "the run '%s' already finished and has results", id);
        return;
    }
    path_join(path, sizeof path, dir, "spec.json");
    JsonError je;
    JsonValue *spec = json_read_file(path, (size_t)64 << 20, &je);
    const JsonValue *req = json_get(spec, "request");
    const char *analysis = json_get_str(spec, "analysis", "");
    if (!spec || !req || !transient_analysis_kind(analysis)) {
        json_free(spec);
        op_fail(out, NV_ERR_PRECONDITION, "start a new run with analysis_run", "the run '%s' has no resumable transient specification", id);
        return;
    }
    JsonValue *params = json_clone(req);
    /* run control may change on resume; physics may not */
    static const char *const CONTROL[] = {"checkpoint_interval", "checkpoint_wall_interval", "max_steps", NULL};
    for (int i = 0; CONTROL[i]; i++)
        if (json_get(p, CONTROL[i])) json_set(params, CONTROL[i], json_clone(json_get(p, CONTROL[i])));
    char spec_hash[65];
    snprintf(spec_hash, sizeof spec_hash, "%s", json_get_str(spec, "spec_hash", ""));
    char label[128];
    snprintf(label, sizeof label, "%s", json_get_str(req, "label", ""));
    char analysis_copy[32];
    snprintf(analysis_copy, sizeof analysis_copy, "%s", analysis);
    json_free(spec);
    ThermalSettings s;
    if (!op_thermal_settings(out, params, analysis_copy, &s)) {
        json_free(params);
        return;
    }
    json_free(params);
    ThermalCheckpoint *ck = thermal_checkpoint_read(dir, false, err, sizeof err);
    if (!ck) {
        op_fail(out, NV_ERR_PRECONDITION, "without a checkpoint the run cannot continue: start it again with analysis_run", "%s", err);
        return;
    }
    if (strcmp(ck->job_id, id)) {
        thermal_checkpoint_free(ck);
        op_fail(out, NV_ERR_PRECONDITION, NULL, "the checkpoint in '%s' belongs to job '%s'", dir, ck->job_id);
        return;
    }
    ThermalCase *c = calloc(1, sizeof *c);
    JsonValue *errors = json_array(), *warnings = json_array();
    if (!c || !thermal_case_build(proj, &s, c, errors, warnings)) {
        const JsonValue *first = json_at(errors, 0);
        op_fail(out, nv_err_from_name(json_get_str(first, "code", "PRECONDITION_FAILED")), json_get_str(first, "hint", "run setup_validate"),
                "the project cannot rebuild this run: %s", json_get_str(first, "message", "out of memory"));
        op_fail_detail(out, "errors", errors);
        json_free(warnings);
        thermal_case_free(c);
        thermal_checkpoint_free(ck);
        return;
    }
    json_free(errors);
    JsonValue *mm = json_array();
    if (strcmp(ck->hashes.mesh, c->hashes.mesh))
        mismatch(mm, "mesh", ck->hashes.mesh, c->hashes.mesh, "the volume mesh (geometry, placement or element size)",
                 "regenerate the mesh with the element_size of the original run (spec.json) and unchanged geometry");
    if (strcmp(ck->hashes.materials, c->hashes.materials))
        mismatch(mm, "materials", ck->hashes.materials, c->hashes.materials, "a material assignment or a material's property values",
                 "restore the assignments and material definitions of the original run");
    if (strcmp(ck->hashes.conditions, c->hashes.conditions))
        mismatch(mm, "conditions", ck->hashes.conditions, c->hashes.conditions, "a thermal or mechanical condition, its selection, value or schedule",
                 "restore the boundary conditions of the original run; a changed condition starts a new run");
    if (strcmp(ck->hashes.settings, c->hashes.settings))
        mismatch(mm, "settings", ck->hashes.settings, c->hashes.settings, "physics or time-integration settings",
                 "only checkpoint_interval, checkpoint_wall_interval and max_steps may change on resume");
    if (json_len(mm) > 0) {
        char list[256] = "";
        for (size_t i = 0; i < json_len(mm); i++)
            snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", json_get_str(json_at(mm, i), "component", ""));
        op_fail(out, NV_ERR_PRECONDITION,
                "restore the project to the state it had when the run started, or start a new run with analysis_run; continuing with different inputs would "
                "splice two different physical problems into one history",
                "cannot continue '%s' from t = %.6g s: %s changed since the checkpoint", id, ck->state.t, list);
        op_fail_detail(out, "mismatches", mm);
        json_free(warnings);
        thermal_case_free(c);
        thermal_checkpoint_free(ck);
        return;
    }
    json_free(mm);
    snprintf(c->job_id, sizeof c->job_id, "%s", id);
    snprintf(c->run_dir, sizeof c->run_dir, "%s", dir);
    if (!thermal_frames_restore(c, ck, err, sizeof err)) {
        json_free(warnings);
        thermal_case_free(c);
        thermal_checkpoint_free(ck);
        op_fail(out, NV_ERR_IO, "the stored frames are damaged: start a new run with analysis_run", "%s", err);
        return;
    }
    double t_resume = ck->state.t;
    int frames = ck->frames;
    JsonValue *info = thermal_checkpoint_info_json(ck);
    c->resume = ck;
    c->build_warnings = warnings;
    JobSpec js = {id, analysis_copy, label, dir, spec_hash, proj->id, proj->name, proj->revision, c, thermal_job_run, thermal_case_free};
    if (!jobs_resubmit(e->jobs, &js, err, sizeof err)) {
        json_free(info);
        op_fail(out, NV_ERR_BUSY, "check job_status and job_list", "%s", err);
        return;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "state", "queued");
    json_set_number(v, "resumed_from_time_s", t_resume);
    json_set_number(v, "end_time_s", s.end_time);
    json_set_int(v, "frames_restored", frames);
    json_set(v, "checkpoint", info);
    json_set_string(v, "compatibility", "mesh, materials, conditions and physics settings hash identically to the checkpoint");
    json_set_string(v, "next_step", "poll job_status; the run continues the same accepted state, so its results are those of one uninterrupted run");
    op_succeed(out, v);
}
