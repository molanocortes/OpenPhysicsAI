/* thermal_integrator.h - transactional time integration of a thermal model: fixed or adaptive steps, events, rollback
 *
 * The integrator owns the ACCEPTED physical state of a transient thermal run and changes it in one place only, when a
 * step is committed:
 *
 *     accepted state   time t, temperature T, accepted cumulative energy budget, fixed-step grid index
 *     control state    the step controller (proposed step, previous error, consecutive rejections)
 *     trial buffers    T_full, T_half, T_two: written by trial solves, never read as state
 *     diagnostics      work counters and a ring of recent attempts, accepted or not
 *
 * Why restoring T is enough here, and what would break it. ThermalSolver keeps no history between steps: its element
 * coefficients, linearised radiation faces, moving-source load vector, active-node flags and face grouping are all
 * rebuilt inside thermal_step from (T0, model, t, dt). The phase state is the equilibrium liquid fraction f(T), a
 * function of temperature with no memory. Time-dependent model data (loads, source position, activation) must be a
 * PURE FUNCTION OF TIME supplied through the schedule callback, piecewise constant between schedule events that steps
 * land on; the callback is called again before every trial solve, so a rejected trial leaves nothing to undo. A future
 * constitutive model with memory (hysteresis, plasticity, damage) breaks this and must add its history variables to
 * the accepted state and to the checkpoint.
 *
 * Adaptive mode estimates the temporal error by step doubling: from the accepted state one full step (T_full) and,
 * independently, two half steps (T_two). For a method of order p the error of T_two is (T_two - T_full) / (2^p - 1),
 * measured in temperature and in enthalpy (thermal_error_norm). T_two is the state that is committed, together with the
 * energy contributions of its two half steps; the full step is only used for the estimate and its energy never enters
 * the budget. No Richardson extrapolation is applied: an extrapolated state satisfies none of the discrete equations,
 * so its energy balance would not close and its liquid fraction could leave [0, 1].
 *
 * Fixed mode takes grid steps t_start + k dt, shortened only to land on an event, and commits every successful solve
 * without an error estimate. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "thermal.h"
#include "timestep.h"

typedef enum { THERMAL_STEPPING_FIXED = 0, THERMAL_STEPPING_ADAPTIVE = 1 } ThermalStepping;

/* accepted cumulative energy, J; only committed steps contribute */
typedef struct ThermalBudget {
    double source, boundary, prescribed, interface_net, stored, enthalpy;
    double worst_balance, worst_mismatch; /* worst per-step values over accepted steps */
    double liquid_volume_max;             /* m^3 */
    int picard_max;                       /* largest Picard count of an accepted solve */
    /* per group (see ThermalModel): accepted cumulative J */
    double group_stored[THERMAL_MAX_GROUPS], group_source[THERMAL_MAX_GROUPS];
    double group_boundary[THERMAL_MAX_GROUPS], group_prescribed[THERMAL_MAX_GROUPS];
    double interface_heat[THERMAL_MAX_GROUPS];
    /* advection (see ThermalStepStats): accepted cumulative J */
    double advection, inflow, enthalpy_inflow, enthalpy_outflow, divergence;
    double group_advection[THERMAL_MAX_GROUPS], group_inflow[THERMAL_MAX_GROUPS];
} ThermalBudget;

/* diagnostics of every attempt, accepted or not; never physical */
typedef struct ThermalWork {
    long attempts, accepted, rejected;
    long nonlinear_failures, linear_failures, state_failures;
    long solves, assemblies, picard_iterations, linear_iterations;
    double seconds_solve, seconds_estimate;
    double error_max, error_sum; /* normalised estimates of accepted steps (adaptive) */
    double dt_min, dt_max;       /* accepted step sizes */
} ThermalWork;

/* one attempted step, kept in a ring for diagnostics */
typedef struct ThermalAttempt {
    double t0, t1;
    bool accepted;
    StepDecision decision;
    ThermalFailure failure; /* THERMAL_FAIL_NONE when every solve of the attempt succeeded */
    double error;           /* normalised estimate, adaptive mode; -1 when not estimated */
    double error_T, error_H;
    int worst_node, worst_elem;
    int picard; /* Picard iterations of the attempt's solves */
} ThermalAttempt;

enum { THERMAL_ATTEMPT_RING = 64 };

/* sets the time-dependent parts of the model for a solve over [t0, t1]. Must depend on the interval only. */
typedef bool (*ThermalScheduleFn)(void *ctx, ThermalModel *m, double t0, double t1, char *err, size_t errlen);

typedef struct ThermalIntegrator ThermalIntegrator;
typedef void (*ThermalAttemptFn)(void *ctx, const ThermalIntegrator *ti, const ThermalAttempt *a);

typedef struct ThermalIntegratorSettings {
    ThermalStepping mode;
    double t_start, t_end;
    double dt_fixed;                /* fixed mode */
    StepControllerSettings control; /* adaptive mode; `order` is ignored: it follows from the solver's theta */
    ThermalTimeTolerance tol;       /* adaptive mode */
    double T_reference;             /* K: relative tolerances apply to changes from this temperature */
    ThermalScheduleFn schedule;     /* optional */
    void *schedule_ctx;
    ThermalAttemptFn on_attempt;    /* optional: after every attempt (after the commit when accepted) */
    void *attempt_ctx;
} ThermalIntegratorSettings;

typedef enum {
    TINT_STEPPED = 0,     /* one step was accepted */
    TINT_FINISHED,        /* the end time had already been reached */
    TINT_FAIL_INPUT,      /* malformed model or settings: a smaller step cannot help */
    TINT_FAIL_MIN_STEP,   /* the error estimate or the solver needs a step below the minimum */
    TINT_FAIL_REJECTIONS, /* too many consecutive rejections */
    TINT_FAIL_NONLINEAR,  /* repeated nonlinear failures */
    TINT_FAIL_LINEAR,     /* repeated linear-solver failures */
    TINT_FAIL_STATE,      /* repeated non-finite or sub-zero states */
    TINT_FAIL_RESOURCE,   /* out of memory */
    TINT_FAIL_STEP_LIMIT  /* the accepted-step limit */
} ThermalIntegratorStatus;

const char *tint_status_name(ThermalIntegratorStatus s); /* e.g. "MIN_STEP" */

typedef struct ThermalStepReport {
    double t0, t1, dt;
    int event;              /* index into the event schedule, -1 */
    unsigned event_kinds;   /* EVENT_* bits of the event reached */
    int attempts;           /* for this accepted step, including rejections and solver failures */
    ThermalErrorNorm norm;  /* adaptive: the estimate of the accepted step */
    ThermalStepStats last;  /* the last solve of the accepted step */
} ThermalStepReport;

/* the solver and model stay owned by the caller; events are copied (and finalised if needed). T_initial holds nnodes
 * temperatures at t_start. */
ThermalIntegrator *tint_create(ThermalSolver *s, ThermalModel *m, const ThermalIntegratorSettings *set, const EventSchedule *events,
                               const double *T_initial, char *err, size_t errlen);
void tint_free(ThermalIntegrator *ti);

/* advances by one accepted step (retrying internally after rejections and recoverable solver failures) */
ThermalIntegratorStatus tint_step(ThermalIntegrator *ti, ThermalStepReport *rep, char *err, size_t errlen);

double tint_time(const ThermalIntegrator *ti);
bool tint_finished(const ThermalIntegrator *ti);
const double *tint_temperature(const ThermalIntegrator *ti);
const ThermalBudget *tint_budget(const ThermalIntegrator *ti);
const ThermalWork *tint_work(const ThermalIntegrator *ti);
const StepController *tint_controller(const ThermalIntegrator *ti);
const EventSchedule *tint_events(const ThermalIntegrator *ti);
int tint_order(const ThermalIntegrator *ti); /* p of the accepted solution */
int tint_node_count(const ThermalIntegrator *ti);
/* the most recent attempts, oldest first; returns how many were written (at most cap) */
int tint_recent_attempts(const ThermalIntegrator *ti, ThermalAttempt *out, int cap);

/* ---- trials, for a coupled orchestrator ------------------------------------------------------------------------------
 *
 * tint_trial attempts [t_accepted, t1] from the accepted state, with whatever the model's time-dependent data and
 * imported loads are now, and keeps the result as a pending trial. It never changes the accepted state and never asks
 * the step controller anything: accepting (tint_commit_trial) or dropping it (just attempting another trial) is the
 * caller's decision. Every trial starts again from the accepted state, so a coupled iteration never integrates the same
 * interval twice from an advanced state. tint_step is exactly: choose t1, trial, controller decision, commit. */
typedef struct ThermalTrial {
    double t0, t1;
    bool estimated;          /* step doubling was used */
    ThermalFailure failure;  /* THERMAL_FAIL_NONE when every solve succeeded */
    ThermalErrorNorm norm;   /* the estimate, when estimated */
    ThermalStepStats s1, s2; /* the solves that make up the candidate (s2 only with an estimate) */
    int ncandidate;
    int picard;              /* Picard iterations of all solves of the trial */
    char err[512];
} ThermalTrial;

/* false when a solve failed (tr->failure says why) */
bool tint_trial(ThermalIntegrator *ti, double t1, bool estimate, ThermalTrial *tr);
/* the candidate end temperature of the last trial (valid until the next trial) */
const double *tint_trial_temperature(const ThermalIntegrator *ti);
/* makes the last trial the accepted state (it must be the last trial and must have succeeded) */
void tint_commit_trial(ThermalIntegrator *ti, const ThermalTrial *tr);
/* nodes whose constraint reaction is integrated over the candidate solves of every trial (J over [t0, t1]) */
bool tint_capture_nodes(ThermalIntegrator *ti, const int *nodes, int n);
const double *tint_trial_node_energy(const ThermalIntegrator *ti);

/* STAGES of a trial: the solves it consists of. Without an estimate there is one, stage 0 over [t0, t1]. With step
 * doubling there are three: stage 0 is the full step over [t0, t1] (the comparison solution), stages 1 and 2 are the two
 * half steps over [t0, tm] and [tm, t1] (the candidate). Coupled data that varies within a step must be supplied per
 * stage, or the two solutions of the estimate see different problems than a monolithic model would and the estimate
 * no longer measures the temporal error of the coupled system. The stage hook runs before every solve, after the
 * schedule callback. */
enum { THERMAL_TRIAL_STAGES = 3 };
typedef bool (*ThermalStageFn)(void *ctx, ThermalModel *m, int stage, double t0, double t1, char *err, size_t errlen);
void tint_set_stage_hook(ThermalIntegrator *ti, ThermalStageFn fn, void *ctx);
/* for a driver that decides about trials itself (an orchestrator): the bookkeeping tint_step does for an attempt that
 * consisted of trial tr (attempt count, error rejection count, diagnostic ring, on_attempt), after the commit when
 * accepted; and the step controller and grid index that belong to the accepted state saved by tint_get_state */
void tint_record_trial(ThermalIntegrator *ti, const ThermalTrial *tr, StepDecision decision, bool accepted, bool error_rejection);
void tint_adopt_control(ThermalIntegrator *ti, const StepController *ctl, long fixed_index);
/* the end temperature of one stage of the last trial, NULL for a stage it did not solve */
const double *tint_trial_stage_temperature(const ThermalIntegrator *ti, int stage);
/* the captured reaction energy of one stage of the last trial (J over that stage), NULL for a stage it did not solve */
const double *tint_trial_stage_node_energy(const ThermalIntegrator *ti, int stage);

/* ---- restart ----------------------------------------------------------------------------------------------------- */

/* everything besides T that the accepted state consists of, for checkpoints */
typedef struct ThermalIntegratorState {
    double t;
    long fixed_index;
    StepController controller;
    ThermalBudget budget;
    ThermalWork work;
} ThermalIntegratorState;

void tint_get_state(const ThermalIntegrator *ti, ThermalIntegratorState *st);
/* replaces the accepted state (time, temperature, controller, budget, work) with a saved one */
bool tint_set_state(ThermalIntegrator *ti, const ThermalIntegratorState *st, const double *T, char *err, size_t errlen);
