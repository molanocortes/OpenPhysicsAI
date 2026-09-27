/* orchestrator.h - one numerical lifecycle for every participating solver
 *
 * A study is a set of PARTICIPANTS joined by COUPLINGS. A coupling carries one field from the participant that exports
 * it to the one that imports it, and is either INTENSIVE (a value per point, e.g. temperature, which is copied) or
 * CONSERVATIVE (an integrated quantity per point, e.g. the heat in joules that crossed a node's share of an interface
 * during the step, whose sum must be preserved).
 *
 * The couplings form a directed graph. Strongly connected components are found (Tarjan) and executed in topological
 * order:
 *   - a component of one participant without a self-loop runs once per attempt (one-way, sequential);
 *   - a component of several participants is a feedback cycle and is iterated (block Gauss-Seidel) until the fields
 *     entering its first member stop changing, with Aitken-relaxed updates of those fields.
 *
 * Participants are TRANSIENT (they integrate in time and hold an accepted state) or QUASI-STATIC (they are evaluated at
 * a time from their inputs, e.g. linear elastic structure driven by temperature). A quasi-static participant declares
 * whether its material has memory: a history-dependent one is evaluated at every accepted step; one without memory
 * only at synchronisation events (stored times), which is exact for it. Output frequency therefore never becomes the
 * integration frequency of a model with history. A quasi-static participant inside a feedback cycle is not supported.
 *
 * One step attempt, for all participants at once:
 *   1. every transient participant starts from its accepted state at t0
 *   2. the fields entering a cycle are predicted from the accepted states (constant extrapolation)
 *   3. participants advance trial solutions over [t0, t1] in topological order
 *   4. exported fields are transferred to the importers
 *   5. the change of the cycle's entering fields is the coupling residual
 *   6. those fields are relaxed and the cycle is iterated, each member again from its accepted state
 *   7. once every cycle has converged and every temporal error estimate is <= 1, all participants commit together,
 *      then quasi-static participants are evaluated
 *   8. otherwise all trials are dropped and the step is retried smaller (or the run fails with a stated class)
 * Coupling error is reported separately from each participant's temporal error, and only the converged sweep's
 * temporal estimates count. A participant that estimates its error from several solves per trial (step doubling) must
 * resolve the coupled fields that vary within a step per solve, or its estimate describes a different problem than
 * the coupled one (thermal_participant.h does this; the orchestrator itself only moves vectors). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"
#include "thermal_integrator.h"
#include "timestep.h"

enum { ORCH_MAX_PARTICIPANTS = 8, ORCH_MAX_COUPLINGS = 16 };

typedef enum { ORCH_FAILURE_NONE = 0, ORCH_FAILURE_INPUT, ORCH_FAILURE_SOLVER, ORCH_FAILURE_RESOURCE } OrchFailureClass;

/* how an attempt ended, told to every transient participant that attempted a trial in it */
typedef enum { ORCH_ATTEMPT_ACCEPTED = 0, ORCH_ATTEMPT_REJECTED_ERROR, ORCH_ATTEMPT_REJECTED_COUPLING, ORCH_ATTEMPT_FAILED } OrchAttemptOutcome;

typedef struct OrchParticipant {
    const char *name;
    void *self;
    bool transient;
    bool history_dependent; /* quasi-static only */
    bool time_independent;  /* quasi-static only: its result does not depend on time (a steady flow with fixed boundary
                               conditions), so it is evaluated once, at initialisation */
    int order;              /* transient: order p of the accepted solution */
    /* transient: a trial over [t0, t1] from the accepted state with the fields imported now. *error receives the
     * normalised temporal error estimate (<= 1 acceptable, negative when not estimated). */
    bool (*trial)(void *self, double t0, double t1, bool estimate, double *error, OrchFailureClass *failure, char *err, size_t errlen);
    void (*commit)(void *self);
    /* quasi-static: evaluate at time t with the fields imported now */
    bool (*evaluate)(void *self, double t, char *err, size_t errlen);
    /* fields: size, export (trial = from the pending trial, else from the accepted state), import */
    int (*field_size)(void *self, const char *field);
    bool (*export_field)(void *self, const char *field, bool trial, double *out);
    bool (*import_field)(void *self, const char *field, const double *in);
    /* transient, optional: the outcome of an attempt in which this participant ran a trial (its last trial of the
     * attempt), with the controller's decision; after the commit when accepted. For per-participant diagnostics. */
    void (*attempt_done)(void *self, OrchAttemptOutcome outcome, StepDecision decision);
} OrchParticipant;

typedef struct OrchCoupling {
    int from, to;
    const char *field_out, *field_in;
    bool conservative;
} OrchCoupling;

typedef struct OrchSettings {
    ThermalStepping mode;           /* fixed or adaptive, as for a single thermal run */
    double t_start, t_end, dt_fixed;
    StepControllerSettings control; /* adaptive; order is taken from the transient participants (lowest) */
    int max_coupling_iterations;    /* default 50 */
    double coupling_relative;       /* default 1e-8: max |change| <= abs + rel * max |field| */
    double coupling_absolute;       /* default 0 */
    double relaxation;              /* first relaxation factor, default 0.5 */
    bool aitken;                    /* default true */
} OrchSettings;

typedef enum {
    ORCH_STEPPED = 0,
    ORCH_FINISHED,
    ORCH_FAIL_INPUT,
    ORCH_FAIL_MIN_STEP,
    ORCH_FAIL_REJECTIONS,
    ORCH_FAIL_SOLVER,
    ORCH_FAIL_COUPLING,
    ORCH_FAIL_EVALUATION,
    ORCH_FAIL_STEP_LIMIT,
    ORCH_FAIL_RESOURCE
} OrchStatus;

const char *orch_status_name(OrchStatus s);

typedef struct OrchStepReport {
    double t0, t1, dt;
    int event;
    unsigned event_kinds;
    int attempts;
    int coupling_iterations;  /* of the accepted attempt, summed over cycles */
    double coupling_residual; /* largest final relative coupling change of the accepted attempt */
    double error;             /* largest temporal error estimate of the accepted attempt (-1 when none) */
    bool evaluated;           /* quasi-static participants were evaluated at t1 */
} OrchStepReport;

typedef struct OrchWork {
    long attempts, accepted, rejected_error, rejected_coupling, solver_failures;
    long coupling_iterations;
    double coupling_residual_max; /* over accepted steps */
    double error_max;
    long evaluations;
} OrchWork;

typedef struct Orchestrator Orchestrator;

/* copies the participants, couplings and events; fails for an unsupported graph (a quasi-static participant in a cycle,
 * an unknown field, mismatched field sizes) */
Orchestrator *orch_create(const OrchSettings *s, const EventSchedule *events, const OrchParticipant *parts, int np, const OrchCoupling *coup, int nc,
                          char *err, size_t errlen);
void orch_free(Orchestrator *o);
/* evaluates quasi-static participants at the start time from the initial transient states */
bool orch_initialize(Orchestrator *o, char *err, size_t errlen);
OrchStatus orch_step(Orchestrator *o, OrchStepReport *rep, char *err, size_t errlen);
double orch_time(const Orchestrator *o);
bool orch_finished(const Orchestrator *o);
const OrchWork *orch_work(const Orchestrator *o);
StepController *orch_controller(Orchestrator *o);
long orch_fixed_index(const Orchestrator *o);
/* restores time, controller and grid index (the participants restore their own states) */
void orch_set_clock(Orchestrator *o, double t, const StepController *ctl, long fixed_index);
/* restores the coupling statistics of a resumed run */
void orch_set_work(Orchestrator *o, const OrchWork *w);
/* the execution plan: participants, couplings, order, which components iterate, synchronisation rule per participant */
JsonValue *orch_describe(const Orchestrator *o);
