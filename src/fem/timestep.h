/* timestep.h - time-step control shared by the transient solvers
 *
 * Two pure-numerics pieces with no solver and no I/O, so the coupled orchestrator can reuse them unchanged:
 *
 *   EventSchedule   the physical times a run must land on exactly: output times, load and source changes,
 *                   activation, checkpoints and the end time. Steps are shortened to hit an event and never step over
 *                   one, and a step is never left with a sliver of the interval behind it.
 *
 *   StepController  a PI step-size controller with bounded growth and shrinkage, separate handling of error-estimator
 *                   rejections and solver failures, a minimum step, a limit on consecutive rejections and a limit on
 *                   accepted steps. The error estimate e of the accepted solution is assumed to scale as dt^k, where
 *                   k = p + 1 for a method of order p (2 for backward Euler, 3 for Crank-Nicolson).
 *
 * Controller law (Gustafsson's PI controller in the form of Hairer, Norsett and Wanner):
 *     accepted, with a previous accepted error:   fac = safety * e^(-0.7/k) * e_prev^(0.4/k)
 *     first accepted step, or right after a rejection:  fac = safety * e^(-1/k)       (elementary I controller)
 *     rejected:                                          fac = safety * e^(-1/k), and at most 1
 *     fac is clamped to [max_shrink, max_growth], and to at most 1 on the first attempt after a rejection
 * e is floored at 1e-10 so that an exact step cannot request an unbounded step. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* ---- events ------------------------------------------------------------------------------------------ */

enum {
    EVENT_END = 1u << 0,
    EVENT_OUTPUT = 1u << 1,
    EVENT_LOAD = 1u << 2,       /* a load, source or boundary condition changes value */
    EVENT_ACTIVATION = 1u << 3, /* material is activated or removed */
    EVENT_CHECKPOINT = 1u << 4
};

typedef struct TimeEvent {
    double t;
    unsigned kinds; /* EVENT_* bits of every event merged at this time */
} TimeEvent;

typedef struct EventSchedule {
    TimeEvent *ev;
    int n, cap;
    double t_start, t_end;
    double tol; /* event times closer than this are the same event; also the landing tolerance */
    bool finalized;
} EventSchedule;

void events_init(EventSchedule *s, double t_start, double t_end);
void events_free(EventSchedule *s);
/* events at or before t_start or after t_end are ignored (returns true); false only when out of memory */
bool events_add(EventSchedule *s, double t, unsigned kinds);
/* sorts, merges coincident events and adds the end event; must be called before stepping */
bool events_finalize(EventSchedule *s);
/* index of the first event strictly after t (beyond the tolerance), or -1 */
int events_next(const EventSchedule *s, double t);
/* limits a proposed step from t so that it lands exactly on the next event and leaves no sliver of the interval
 * behind it. Returns the step to take; *event receives the index of the event reached (-1 when none) and *limited
 * whether the proposal was shortened. When an event is reached, the caller must set the new time to ev[*event].t
 * rather than t + dt, so that repeated landings do not accumulate round-off. */
double events_limit_step(const EventSchedule *s, double t, double dt, int *event, bool *limited);

/* ---- step-size controller ------------------------------------------------------------------------------ */

typedef struct StepControllerSettings {
    double dt_initial, dt_min, dt_max; /* s */
    double safety;                     /* (0, 1], default 0.9 */
    double max_growth;                 /* > 1, default 3 */
    double max_shrink;                 /* (0, 1): a step is reduced by at most this factor at once, default 0.2 */
    double solver_shrink;              /* (0, 1): step factor after a nonlinear or linear solver failure, default 0.25 */
    int max_rejections;                /* consecutive error-estimator rejections before giving up, default 25 */
    int max_solver_failures;           /* consecutive solver failures handled by shrinking, default 8 */
    long max_steps;                    /* accepted steps, default 1000000 */
    int order;                         /* k: the estimate scales as dt^k */
} StepControllerSettings;

typedef enum {
    STEP_ACCEPT = 0,       /* the attempt is accepted; dt holds the proposal for the next step */
    STEP_RETRY,            /* rejected or failed; dt holds the smaller step to retry with */
    STEP_FAIL_MIN_STEP,    /* the controller would need a step below dt_min */
    STEP_FAIL_REJECTIONS,  /* max_rejections consecutive rejections */
    STEP_FAIL_SOLVER,      /* max_solver_failures consecutive solver failures */
    STEP_FAIL_STEP_LIMIT   /* max_steps accepted steps */
} StepDecision;

typedef struct StepController {
    StepControllerSettings set;
    double dt;          /* the step to attempt next (before event limiting) */
    double err_prev;    /* normalised error of the last accepted step; 0 when there is none */
    bool after_failure; /* the previous attempt was rejected or failed: the next factor may not exceed 1 */
    int rejections;     /* consecutive */
    int solver_failures;
    long accepted, rejected_total, solver_failures_total;
    double last_factor; /* the factor applied by the last decision */
} StepController;

const char *step_decision_name(StepDecision d);
/* fills unset (zero) fields with the defaults above and validates the rest */
bool stepctl_defaults(StepControllerSettings *s, double duration, char *err, size_t errlen);
bool stepctl_init(StepController *c, const StepControllerSettings *s, char *err, size_t errlen);
/* the normalised error estimate err of an attempted step of size dt (accepted when err <= 1). `limited` tells that the
 * attempted step had been shortened to land on an event, in which case an accepted step keeps the controller's own
 * proposal instead of shrinking it to the event-limited size. */
StepDecision stepctl_error(StepController *c, double dt, double err, bool limited);
/* a solver failure on an attempted step of size dt */
StepDecision stepctl_solver_failure(StepController *c, double dt);
