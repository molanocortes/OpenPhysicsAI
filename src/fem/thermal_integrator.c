/* thermal_integrator.c - transactional fixed and adaptive time stepping of a thermal model (see thermal_integrator.h) */
#include "thermal_integrator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct ThermalIntegrator {
    ThermalSolver *s;
    ThermalModel *m;
    ThermalIntegratorSettings set;
    EventSchedule ev;
    int nn, order;
    double factor; /* 1 / (2^p - 1) */
    /* ---- accepted state: written only by commit() and tint_set_state() ---- */
    double t;
    double *T;
    long fixed_index;
    ThermalBudget budget;
    /* ---- control state ---- */
    StepController ctl;
    /* ---- trial buffers ---- */
    double *T_full, *T_half, *T_two;
    /* ---- diagnostics ---- */
    ThermalWork work;
    ThermalAttempt ring[THERMAL_ATTEMPT_RING];
    int ring_next, ring_count;
    /* ---- node reaction capture ---- */
    int ncapture;
    int *capture;
    double *capture_energy;                          /* over the candidate solves */
    double *capture_stage[THERMAL_TRIAL_STAGES];     /* per stage */
    /* ---- per-stage coupling hook and what the last trial solved ---- */
    ThermalStageFn stage_hook;
    void *stage_ctx;
    bool trial_estimated;
};

const char *tint_status_name(ThermalIntegratorStatus s) {
    static const char *const N[] = {"STEPPED", "FINISHED", "INPUT", "MIN_STEP", "TOO_MANY_REJECTIONS", "NONLINEAR_FAILURE", "LINEAR_FAILURE",
                                    "INVALID_STATE", "RESOURCE_LIMIT", "STEP_LIMIT"};
    return s >= TINT_STEPPED && s <= TINT_FAIL_STEP_LIMIT ? N[s] : "UNKNOWN";
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

void tint_free(ThermalIntegrator *ti) {
    if (!ti) return;
    events_free(&ti->ev);
    free(ti->T), free(ti->T_full), free(ti->T_half), free(ti->T_two);
    free(ti->capture), free(ti->capture_energy);
    for (int k = 0; k < THERMAL_TRIAL_STAGES; k++) free(ti->capture_stage[k]);
    free(ti);
}

ThermalIntegrator *tint_create(ThermalSolver *s, ThermalModel *m, const ThermalIntegratorSettings *set, const EventSchedule *events,
                               const double *T_initial, char *err, size_t errlen) {
    ThermalIntegrator *ti = calloc(1, sizeof *ti);
    if (!ti) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    ti->s = s, ti->m = m, ti->set = *set, ti->nn = m->nnodes;
    const ThermalIntegratorSettings *st = &ti->set;
    double duration = st->t_end - st->t_start;
    if (!(duration > 0) || !isfinite(duration)) {
        snprintf(err, errlen, "the end time must lie after the start time (%g .. %g s)", st->t_start, st->t_end);
        goto fail;
    }
    /* the order of the accepted solution: only theta = 1/2 (Crank-Nicolson) is second order; every other theta in
     * [0.5, 1] is first order */
    ti->order = fabs(thermal_theta(s) - 0.5) < 1e-12 ? 2 : 1;
    events_init(&ti->ev, st->t_start, st->t_end);
    if (events)
        for (int i = 0; i < events->n; i++)
            if (!events_add(&ti->ev, events->ev[i].t, events->ev[i].kinds)) {
                snprintf(err, errlen, "out of memory for the event schedule");
                goto fail;
            }
    if (!events_finalize(&ti->ev)) {
        snprintf(err, errlen, "out of memory for the event schedule");
        goto fail;
    }
    if (st->mode == THERMAL_STEPPING_FIXED) {
        if (!(st->dt_fixed > 0) || !isfinite(st->dt_fixed)) {
            snprintf(err, errlen, "fixed time stepping needs a positive time step (got %g s)", st->dt_fixed);
            goto fail;
        }
    } else {
        StepControllerSettings cs = st->control;
        cs.order = ti->order + 1; /* the local error estimate of an order-p method scales as dt^(p+1) */
        if (!stepctl_defaults(&cs, duration, err, errlen) || !stepctl_init(&ti->ctl, &cs, err, errlen)) goto fail;
        ti->set.control = cs;
        const ThermalTimeTolerance *tol = &st->tol;
        if (!(tol->relative >= 0) || !(tol->temperature >= 0) || !(tol->enthalpy >= 0) || !(tol->relative > 0 || tol->temperature > 0)) {
            snprintf(err, errlen,
                     "adaptive stepping needs a non-negative relative tolerance and absolute temperature tolerance, not both zero (got %g and %g K)",
                     tol->relative, tol->temperature);
            goto fail;
        }
        if (!tol->temperature_only && !(tol->enthalpy > 0) && !(tol->temperature > 0)) {
            snprintf(err, errlen, "the enthalpy measure needs an absolute enthalpy tolerance or an absolute temperature tolerance to derive it from");
            goto fail;
        }
    }
    ti->factor = 1.0 / (double)((1 << ti->order) - 1);
    size_t bytes = (size_t)ti->nn * sizeof(double);
    ti->T = malloc(bytes), ti->T_full = malloc(bytes), ti->T_half = malloc(bytes), ti->T_two = malloc(bytes);
    if (!ti->T || !ti->T_full || !ti->T_half || !ti->T_two) {
        snprintf(err, errlen, "out of memory for the temperature fields (%d nodes)", ti->nn);
        goto fail;
    }
    for (int n = 0; n < ti->nn; n++)
        if (!isfinite(T_initial[n]) || T_initial[n] < 0) {
            snprintf(err, errlen, "the initial temperature of node %d is %g K", n, T_initial[n]);
            goto fail;
        }
    memcpy(ti->T, T_initial, bytes);
    ti->t = st->t_start;
    ti->work.dt_min = INFINITY;
    return ti;
fail:
    tint_free(ti);
    return NULL;
}

/* ---- accepted-state views ---- */

double tint_time(const ThermalIntegrator *ti) { return ti->t; }
bool tint_finished(const ThermalIntegrator *ti) { return ti->t >= ti->set.t_end - ti->ev.tol; }
const double *tint_temperature(const ThermalIntegrator *ti) { return ti->T; }
const ThermalBudget *tint_budget(const ThermalIntegrator *ti) { return &ti->budget; }
const ThermalWork *tint_work(const ThermalIntegrator *ti) { return &ti->work; }
const StepController *tint_controller(const ThermalIntegrator *ti) { return &ti->ctl; }
const EventSchedule *tint_events(const ThermalIntegrator *ti) { return &ti->ev; }
int tint_order(const ThermalIntegrator *ti) { return ti->order; }
int tint_node_count(const ThermalIntegrator *ti) { return ti->nn; }

int tint_recent_attempts(const ThermalIntegrator *ti, ThermalAttempt *out, int cap) {
    int n = ti->ring_count < cap ? ti->ring_count : cap;
    int start = (ti->ring_next - n + THERMAL_ATTEMPT_RING) % THERMAL_ATTEMPT_RING;
    for (int i = 0; i < n; i++) out[i] = ti->ring[(start + i) % THERMAL_ATTEMPT_RING];
    return n;
}

void tint_get_state(const ThermalIntegrator *ti, ThermalIntegratorState *st) {
    memset(st, 0, sizeof *st);
    st->t = ti->t;
    st->fixed_index = ti->fixed_index;
    st->controller = ti->ctl;
    st->budget = ti->budget;
    st->work = ti->work;
}

bool tint_set_state(ThermalIntegrator *ti, const ThermalIntegratorState *st, const double *T, char *err, size_t errlen) {
    if (!(st->t >= ti->set.t_start - ti->ev.tol) || !(st->t <= ti->set.t_end + ti->ev.tol)) {
        snprintf(err, errlen, "the saved time %.17g s lies outside the run interval %g .. %g s", st->t, ti->set.t_start, ti->set.t_end);
        return false;
    }
    for (int n = 0; n < ti->nn; n++)
        if (!isfinite(T[n]) || T[n] < 0) {
            snprintf(err, errlen, "the saved temperature of node %d is %g K", n, T[n]);
            return false;
        }
    if (ti->set.mode == THERMAL_STEPPING_ADAPTIVE) {
        const StepControllerSettings *a = &ti->set.control, *b = &st->controller.set;
        if (a->order != b->order || a->dt_min != b->dt_min || a->dt_max != b->dt_max || a->safety != b->safety || a->max_growth != b->max_growth ||
            a->max_shrink != b->max_shrink) {
            snprintf(err, errlen, "the saved step controller was configured differently from this run");
            return false;
        }
        if (!(st->controller.dt >= a->dt_min && st->controller.dt <= a->dt_max)) {
            snprintf(err, errlen, "the saved proposed step %g s lies outside %g .. %g s", st->controller.dt, a->dt_min, a->dt_max);
            return false;
        }
    }
    memcpy(ti->T, T, (size_t)ti->nn * sizeof(double));
    ti->t = st->t;
    ti->fixed_index = st->fixed_index;
    ti->ctl = st->controller;
    ti->budget = st->budget;
    ti->work = st->work;
    ti->ring_count = ti->ring_next = 0;
    return true;
}

/* ---- stepping ---- */

static void record(ThermalIntegrator *ti, const ThermalAttempt *a) {
    ti->ring[ti->ring_next] = *a;
    ti->ring_next = (ti->ring_next + 1) % THERMAL_ATTEMPT_RING;
    if (ti->ring_count < THERMAL_ATTEMPT_RING) ti->ring_count++;
    if (ti->set.on_attempt) ti->set.on_attempt(ti->set.attempt_ctx, ti, a);
}

/* one trial solve (stage) over [t0, t1] from `from` into `to`; counts the work whether or not it succeeds */
static bool trial_solve(ThermalIntegrator *ti, int stage, const double *from, double *to, double t0, double t1, ThermalStepStats *st, char *err,
                        size_t errlen) {
    if ((ti->set.schedule && !ti->set.schedule(ti->set.schedule_ctx, ti->m, t0, t1, err, errlen)) ||
        (ti->stage_hook && !ti->stage_hook(ti->stage_ctx, ti->m, stage, t0, t1, err, errlen))) {
        memset(st, 0, sizeof *st);
        st->failure = THERMAL_FAIL_INPUT;
        return false;
    }
    double w0 = now_s();
    bool ok = thermal_step(ti->s, ti->m, from, to, t0, t1 - t0, st, err, errlen);
    ti->work.seconds_solve += now_s() - w0;
    ti->work.solves++;
    ti->work.assemblies += st->assemblies;
    ti->work.picard_iterations += st->picard_iterations;
    ti->work.linear_iterations += st->linear_iterations;
    if (!ok) {
        if (st->failure == THERMAL_FAIL_NONE) st->failure = THERMAL_FAIL_INPUT;
        if (st->failure == THERMAL_FAIL_NONLINEAR) ti->work.nonlinear_failures++;
        else if (st->failure == THERMAL_FAIL_LINEAR) ti->work.linear_failures++;
        else if (st->failure == THERMAL_FAIL_STATE) ti->work.state_failures++;
    }
    return ok;
}

static void add_budget(ThermalBudget *b, const ThermalStepStats *st) {
    b->source += st->source_energy;
    b->boundary += st->boundary_energy;
    b->prescribed += st->prescribed_energy;
    b->interface_net += st->interface_energy;
    b->stored += st->stored_energy;
    b->enthalpy += st->enthalpy_change;
    b->worst_balance = fmax(b->worst_balance, st->balance_error);
    b->worst_mismatch = fmax(b->worst_mismatch, st->enthalpy_mismatch);
    b->liquid_volume_max = fmax(b->liquid_volume_max, st->liquid_volume);
    if (st->picard_iterations > b->picard_max) b->picard_max = st->picard_iterations;
    for (int g = 0; g < THERMAL_MAX_GROUPS; g++) {
        b->group_stored[g] += st->group_stored[g];
        b->group_source[g] += st->group_source[g];
        b->group_boundary[g] += st->group_boundary[g];
        b->group_prescribed[g] += st->group_prescribed[g];
        b->interface_heat[g] += st->interface_heat[g];
    }
    b->advection += st->advection_energy;
    b->inflow += st->inflow_energy;
    b->enthalpy_inflow += st->enthalpy_inflow;
    b->enthalpy_outflow += st->enthalpy_outflow;
    b->divergence += st->divergence_energy;
    for (int g = 0; g < THERMAL_MAX_GROUPS; g++) {
        b->group_advection[g] += st->group_advection[g];
        b->group_inflow[g] += st->group_inflow[g];
    }
}

/* the only place where accepted physical state changes */
static void commit(ThermalIntegrator *ti, double t1, const double *T_new, const ThermalStepStats *const *stats, int nstats) {
    memcpy(ti->T, T_new, (size_t)ti->nn * sizeof(double));
    double dt = t1 - ti->t;
    ti->t = t1;
    for (int i = 0; i < nstats; i++) add_budget(&ti->budget, stats[i]);
    ti->work.accepted++;
    ti->work.dt_min = fmin(ti->work.dt_min, dt);
    ti->work.dt_max = fmax(ti->work.dt_max, dt);
}

static ThermalIntegratorStatus failure_status(ThermalFailure f) {
    switch (f) {
    case THERMAL_FAIL_NONLINEAR: return TINT_FAIL_NONLINEAR;
    case THERMAL_FAIL_LINEAR: return TINT_FAIL_LINEAR;
    case THERMAL_FAIL_STATE: return TINT_FAIL_STATE;
    case THERMAL_FAIL_RESOURCE: return TINT_FAIL_RESOURCE;
    default: return TINT_FAIL_INPUT;
    }
}

static const char *failure_name(ThermalFailure f) {
    static const char *const N[] = {"none", "input", "nonlinear", "linear", "invalid state", "resource"};
    return f >= THERMAL_FAIL_NONE && f <= THERMAL_FAIL_RESOURCE ? N[f] : "unknown";
}

bool tint_capture_nodes(ThermalIntegrator *ti, const int *nodes, int n) {
    free(ti->capture), free(ti->capture_energy);
    ti->capture = NULL, ti->capture_energy = NULL, ti->ncapture = 0;
    for (int k = 0; k < THERMAL_TRIAL_STAGES; k++) free(ti->capture_stage[k]), ti->capture_stage[k] = NULL;
    if (n <= 0) return true;
    ti->capture = malloc((size_t)n * sizeof(int));
    ti->capture_energy = calloc((size_t)n, sizeof(double));
    if (!ti->capture || !ti->capture_energy) return false;
    for (int k = 0; k < THERMAL_TRIAL_STAGES; k++)
        if (!(ti->capture_stage[k] = calloc((size_t)n, sizeof(double)))) return false;
    for (int i = 0; i < n; i++) {
        if (nodes[i] < 0 || nodes[i] >= ti->nn) return false;
        ti->capture[i] = nodes[i];
    }
    ti->ncapture = n;
    return true;
}

const double *tint_trial_node_energy(const ThermalIntegrator *ti) { return ti->capture_energy; }
const double *tint_trial_temperature(const ThermalIntegrator *ti) { return ti->T_two; }

void tint_set_stage_hook(ThermalIntegrator *ti, ThermalStageFn fn, void *ctx) {
    ti->stage_hook = fn;
    ti->stage_ctx = ctx;
}

const double *tint_trial_stage_temperature(const ThermalIntegrator *ti, int stage) {
    if (!ti->trial_estimated) return stage == 0 ? ti->T_two : NULL;
    return stage == 0 ? ti->T_full : stage == 1 ? ti->T_half : stage == 2 ? ti->T_two : NULL;
}

const double *tint_trial_stage_node_energy(const ThermalIntegrator *ti, int stage) {
    if (stage < 0 || stage >= THERMAL_TRIAL_STAGES || (!ti->trial_estimated && stage > 0)) return NULL;
    return ti->capture_stage[stage];
}

/* the reaction energy of the captured nodes over the stage just solved; candidate stages also add to the total */
static void capture_add(ThermalIntegrator *ti, int stage, bool candidate, double dt) {
    for (int i = 0; i < ti->ncapture; i++) {
        double e = dt * thermal_node_reaction(ti->s, ti->m, ti->capture[i]);
        ti->capture_stage[stage][i] = e;
        if (candidate) ti->capture_energy[i] += e;
    }
}

bool tint_trial(ThermalIntegrator *ti, double t1, bool estimate, ThermalTrial *tr) {
    memset(tr, 0, sizeof *tr);
    double t0 = ti->t, tm = t0 + 0.5 * (t1 - t0);
    tr->t0 = t0, tr->t1 = t1, tr->estimated = estimate;
    ti->trial_estimated = estimate;
    for (int i = 0; i < ti->ncapture; i++) ti->capture_energy[i] = 0;
    if (!estimate) {
        bool ok = trial_solve(ti, 0, ti->T, ti->T_two, t0, t1, &tr->s1, tr->err, sizeof tr->err);
        tr->picard = tr->s1.picard_iterations;
        if (!ok) {
            tr->failure = tr->s1.failure;
            return false;
        }
        capture_add(ti, 0, true, t1 - t0);
        tr->ncandidate = 1;
        return true;
    }
    ThermalStepStats sf;
    bool ok = trial_solve(ti, 0, ti->T, ti->T_full, t0, t1, &sf, tr->err, sizeof tr->err);
    tr->picard += sf.picard_iterations;
    ThermalFailure fail = ok ? THERMAL_FAIL_NONE : sf.failure;
    if (ok) {
        capture_add(ti, 0, false, t1 - t0);
        ok = trial_solve(ti, 1, ti->T, ti->T_half, t0, tm, &tr->s1, tr->err, sizeof tr->err);
        tr->picard += tr->s1.picard_iterations;
        if (!ok) fail = tr->s1.failure;
        else capture_add(ti, 1, true, tm - t0);
    }
    if (ok) {
        ok = trial_solve(ti, 2, ti->T_half, ti->T_two, tm, t1, &tr->s2, tr->err, sizeof tr->err);
        tr->picard += tr->s2.picard_iterations;
        if (!ok) fail = tr->s2.failure;
        else capture_add(ti, 2, true, t1 - tm);
    }
    if (!ok) {
        tr->failure = fail;
        return false;
    }
    double w0 = now_s();
    thermal_error_norm(ti->m, ti->T, ti->T_full, ti->T_two, ti->factor, ti->set.T_reference, &ti->set.tol, &tr->norm);
    ti->work.seconds_estimate += now_s() - w0;
    tr->ncandidate = 2;
    return true;
}

/* the diagnostic record of an attempt that consisted of the trial tr */
static ThermalAttempt attempt_of(const ThermalTrial *tr, StepDecision decision, bool accepted) {
    ThermalAttempt a = {tr->t0, tr->t1, accepted, decision, tr->failure, -1, -1, -1, -1, -1, tr->picard};
    if (tr->failure == THERMAL_FAIL_NONE && tr->estimated) {
        a.error = tr->norm.err, a.error_T = tr->norm.err_T, a.error_H = tr->norm.err_H;
        a.worst_node = tr->norm.worst_node, a.worst_elem = tr->norm.worst_elem;
    }
    return a;
}

void tint_record_trial(ThermalIntegrator *ti, const ThermalTrial *tr, StepDecision decision, bool accepted, bool error_rejection) {
    ThermalAttempt a = attempt_of(tr, decision, accepted);
    ti->work.attempts++;
    if (error_rejection) ti->work.rejected++;
    record(ti, &a);
}

void tint_adopt_control(ThermalIntegrator *ti, const StepController *ctl, long fixed_index) {
    if (ctl) ti->ctl = *ctl;
    ti->fixed_index = fixed_index;
}

void tint_commit_trial(ThermalIntegrator *ti, const ThermalTrial *tr) {
    const ThermalStepStats *list[2] = {&tr->s1, &tr->s2};
    commit(ti, tr->t1, ti->T_two, list, tr->ncandidate);
    if (tr->estimated) {
        ti->work.error_max = fmax(ti->work.error_max, tr->norm.err);
        ti->work.error_sum += tr->norm.err;
    }
}

static ThermalIntegratorStatus fixed_step(ThermalIntegrator *ti, ThermalStepReport *rep, char *err, size_t errlen) {
    const ThermalIntegratorSettings *st = &ti->set;
    double g = st->t_start + (double)(ti->fixed_index + 1) * st->dt_fixed; /* multiplication, not accumulation */
    bool grid = true;
    if (g > st->t_end - ti->ev.tol) g = st->t_end;
    int k = events_next(&ti->ev, ti->t);
    double t1 = g;
    int event = -1;
    if (k >= 0 && ti->ev.ev[k].t <= g + ti->ev.tol) {
        grid = fabs(ti->ev.ev[k].t - g) <= ti->ev.tol;
        t1 = ti->ev.ev[k].t;
        event = k;
    }
    ThermalTrial tr;
    rep->attempts = 1;
    double t0 = ti->t;
    if (!tint_trial(ti, t1, false, &tr)) {
        tint_record_trial(ti, &tr, STEP_FAIL_SOLVER, false, false);
        snprintf(err, errlen, "%s", tr.err);
        return failure_status(tr.failure); /* fixed steps are not retried: the step size is the user's choice */
    }
    tint_commit_trial(ti, &tr);
    if (grid) ti->fixed_index++;
    tint_record_trial(ti, &tr, STEP_ACCEPT, true, false);
    rep->t0 = t0, rep->t1 = t1, rep->dt = t1 - t0;
    rep->event = event;
    rep->event_kinds = event >= 0 ? ti->ev.ev[event].kinds : 0;
    rep->last = tr.s1;
    return TINT_STEPPED;
}

ThermalIntegratorStatus tint_step(ThermalIntegrator *ti, ThermalStepReport *rep, char *err, size_t errlen) {
    memset(rep, 0, sizeof *rep);
    rep->event = -1;
    if (err && errlen) err[0] = 0;
    if (tint_finished(ti)) return TINT_FINISHED;
    if (ti->set.mode == THERMAL_STEPPING_FIXED) return fixed_step(ti, rep, err, errlen);
    const ThermalIntegratorSettings *st = &ti->set;
    if (ti->ctl.accepted >= st->control.max_steps) {
        snprintf(err, errlen, "the limit of %ld accepted steps was reached at t = %.6g s of %.6g s", st->control.max_steps, ti->t, st->t_end);
        return TINT_FAIL_STEP_LIMIT;
    }
    for (;;) {
        bool limited;
        int event;
        double dt = events_limit_step(&ti->ev, ti->t, ti->ctl.dt, &event, &limited);
        double t0 = ti->t, t1 = event >= 0 ? ti->ev.ev[event].t : t0 + dt;
        dt = t1 - t0;
        rep->attempts++;
        ThermalTrial tr;
        bool ok = tint_trial(ti, t1, true, &tr);
        if (!ok) {
            ThermalFailure fail = tr.failure;
            if (fail == THERMAL_FAIL_INPUT || fail == THERMAL_FAIL_RESOURCE) {
                tint_record_trial(ti, &tr, STEP_FAIL_SOLVER, false, false);
                snprintf(err, errlen, "%s", tr.err);
                return failure_status(fail);
            }
            StepDecision d = stepctl_solver_failure(&ti->ctl, dt);
            tint_record_trial(ti, &tr, d, false, false);
            if (d == STEP_RETRY) continue;
            snprintf(err, errlen, "%s failure at t = %.6g s with a step of %.4g s (%s): %s", failure_name(fail), t0, dt, step_decision_name(d), tr.err);
            return d == STEP_FAIL_MIN_STEP ? TINT_FAIL_MIN_STEP : failure_status(fail);
        }
        StepDecision d = stepctl_error(&ti->ctl, dt, tr.norm.err, limited);
        if (d == STEP_ACCEPT) {
            tint_commit_trial(ti, &tr);
            tint_record_trial(ti, &tr, d, true, false);
            rep->t0 = t0, rep->t1 = t1, rep->dt = dt;
            rep->event = event;
            rep->event_kinds = event >= 0 ? ti->ev.ev[event].kinds : 0;
            rep->norm = tr.norm;
            rep->last = tr.s2;
            return TINT_STEPPED;
        }
        tint_record_trial(ti, &tr, d, false, true);
        if (d == STEP_RETRY) continue;
        const ThermalErrorNorm *norm = &tr.norm;
        snprintf(err, errlen,
                 "%s at t = %.6g s: a step of %.4g s has a normalised error of %.3g (temperature %.3g at node %d, enthalpy %.3g at element %d; "
                 "estimated %.3g K / %.3g J/m^3) and the controller cannot reduce it further (minimum step %.3g s, %d consecutive rejections)",
                 step_decision_name(d), t0, dt, norm->err, norm->err_T, norm->worst_node, norm->err_H, norm->worst_elem, norm->max_dT, norm->max_dH,
                 st->control.dt_min, ti->ctl.rejections);
        return d == STEP_FAIL_MIN_STEP ? TINT_FAIL_MIN_STEP : TINT_FAIL_REJECTIONS;
    }
}
