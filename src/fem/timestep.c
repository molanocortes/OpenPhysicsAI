/* timestep.c - event schedule and PI step-size controller (see timestep.h for the control law) */
#include "timestep.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- events ------------------------------------------------------------------------------------------ */

void events_init(EventSchedule *s, double t_start, double t_end) {
    memset(s, 0, sizeof *s);
    s->t_start = t_start, s->t_end = t_end;
    /* a relative tolerance on the magnitude of the times involved: 64 ulps of the largest time */
    s->tol = 64 * DBL_EPSILON * fmax(1.0, fmax(fabs(t_start), fabs(t_end)));
}

void events_free(EventSchedule *s) {
    free(s->ev);
    memset(s, 0, sizeof *s);
}

bool events_add(EventSchedule *s, double t, unsigned kinds) {
    if (!isfinite(t) || t <= s->t_start + s->tol || t > s->t_end + s->tol) return true;
    if (fabs(t - s->t_end) <= s->tol) t = s->t_end;
    if (s->n == s->cap) {
        int cap = s->cap ? 2 * s->cap : 32;
        TimeEvent *ne = realloc(s->ev, (size_t)cap * sizeof *ne);
        if (!ne) return false;
        s->ev = ne, s->cap = cap;
    }
    s->ev[s->n++] = (TimeEvent){t, kinds};
    s->finalized = false;
    return true;
}

static int cmp_event(const void *a, const void *b) {
    double x = ((const TimeEvent *)a)->t, y = ((const TimeEvent *)b)->t;
    return (x > y) - (x < y);
}

bool events_finalize(EventSchedule *s) {
    if (!events_add(s, s->t_end, EVENT_END)) return false;
    qsort(s->ev, (size_t)s->n, sizeof *s->ev, cmp_event);
    int m = 0;
    for (int i = 0; i < s->n; i++) {
        if (m && s->ev[i].t - s->ev[m - 1].t <= s->tol) {
            /* merged: the end time and output times keep their exact value */
            if (s->ev[i].kinds & EVENT_END) s->ev[m - 1].t = s->ev[i].t;
            s->ev[m - 1].kinds |= s->ev[i].kinds;
            continue;
        }
        s->ev[m++] = s->ev[i];
    }
    s->n = m;
    s->finalized = true;
    return true;
}

int events_next(const EventSchedule *s, double t) {
    int lo = 0, hi = s->n; /* first index with ev.t > t + tol */
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s->ev[mid].t > t + s->tol) hi = mid;
        else lo = mid + 1;
    }
    return lo < s->n ? lo : -1;
}

double events_limit_step(const EventSchedule *s, double t, double dt, int *event, bool *limited) {
    *event = -1, *limited = false;
    int k = events_next(s, t);
    if (k < 0) return dt;
    double gap = s->ev[k].t - t;
    if (dt >= gap - s->tol) { /* reaches or passes the event: land on it */
        *event = k;
        *limited = dt > gap + s->tol;
        return gap;
    }
    if (gap - dt < 0.25 * dt) { /* the step would leave a sliver before the event */
        if (gap <= 1.1 * dt) {  /* stretch by at most 10 %, which the controller's safety factor covers */
            *event = k;
            return gap;
        }
        *limited = true; /* otherwise two roughly equal steps */
        return 0.5 * gap;
    }
    return dt;
}

/* ---- controller -------------------------------------------------------------------------------------- */

const char *step_decision_name(StepDecision d) {
    static const char *const N[] = {"accept", "retry", "minimum_step", "too_many_rejections", "solver_failures", "step_limit"};
    return d >= STEP_ACCEPT && d <= STEP_FAIL_STEP_LIMIT ? N[d] : "unknown";
}

bool stepctl_defaults(StepControllerSettings *s, double duration, char *err, size_t errlen) {
    if (!(duration > 0) || !isfinite(duration)) {
        snprintf(err, errlen, "the time interval must be positive and finite (got %g s)", duration);
        return false;
    }
    if (!(s->dt_max > 0)) s->dt_max = duration;
    if (!(s->dt_min > 0)) s->dt_min = 1e-10 * duration;
    /* a default never contradicts a limit the caller gave: only an explicit initial step below the minimum is an error */
    if (!(s->dt_initial > 0)) s->dt_initial = fmin(s->dt_max, fmax(s->dt_min, 1e-3 * duration));
    if (!(s->safety > 0)) s->safety = 0.9;
    if (!(s->max_growth > 0)) s->max_growth = 3;
    if (!(s->max_shrink > 0)) s->max_shrink = 0.2;
    if (!(s->solver_shrink > 0)) s->solver_shrink = 0.25;
    if (s->max_rejections <= 0) s->max_rejections = 25;
    if (s->max_solver_failures <= 0) s->max_solver_failures = 8;
    if (s->max_steps <= 0) s->max_steps = 1000000;
    if (s->order <= 0) s->order = 2;
    const char *bad = NULL;
    if (!(s->dt_min <= s->dt_max)) bad = "min_time_step must not exceed max_time_step";
    else if (!(s->dt_initial >= s->dt_min)) bad = "initial_time_step must not be below min_time_step";
    else if (!(s->safety <= 1)) bad = "the safety factor must lie in (0, 1]";
    else if (!(s->max_growth > 1)) bad = "max_step_growth must exceed 1";
    else if (!(s->max_shrink < 1)) bad = "max_step_shrink must lie in (0, 1)";
    else if (!(s->solver_shrink < 1)) bad = "the solver-failure shrink factor must lie in (0, 1)";
    else if (s->order > 8) bad = "the error order must lie in 1..8";
    if (bad) {
        snprintf(err, errlen, "%s (min %g s, initial %g s, max %g s)", bad, s->dt_min, s->dt_initial, s->dt_max);
        return false;
    }
    s->dt_initial = fmin(s->dt_initial, s->dt_max);
    return true;
}

bool stepctl_init(StepController *c, const StepControllerSettings *s, char *err, size_t errlen) {
    memset(c, 0, sizeof *c);
    c->set = *s;
    if (!(s->dt_min > 0) || !(s->dt_max >= s->dt_min) || !(s->dt_initial >= s->dt_min) || s->order <= 0 || !(s->safety > 0) || !(s->max_growth > 1) ||
        !(s->max_shrink > 0 && s->max_shrink < 1)) {
        snprintf(err, errlen, "step controller settings are incomplete: call stepctl_defaults first");
        return false;
    }
    c->dt = fmin(s->dt_initial, s->dt_max);
    c->last_factor = 1;
    return true;
}

static double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

StepDecision stepctl_error(StepController *c, double dt, double err, bool limited) {
    const StepControllerSettings *s = &c->set;
    double k = (double)s->order;
    double e = isfinite(err) ? fmax(err, 1e-10) : 1e10;
    if (err <= 1) { /* NaN compares false: a non-finite estimate is rejected */
        double fac = (c->err_prev > 0 && !c->after_failure) ? s->safety * pow(e, -0.7 / k) * pow(c->err_prev, 0.4 / k) : s->safety * pow(e, -1.0 / k);
        fac = clampd(fac, s->max_shrink, c->after_failure ? 1.0 : s->max_growth);
        double next = dt * fac;
        if (limited) next = fmax(next, c->dt); /* a step shortened by an event says nothing against the own proposal */
        c->dt = clampd(next, s->dt_min, s->dt_max);
        c->err_prev = e;
        c->after_failure = false;
        c->rejections = 0;
        c->solver_failures = 0;
        c->accepted++;
        c->last_factor = fac;
        return STEP_ACCEPT;
    }
    c->rejections++;
    c->rejected_total++;
    c->after_failure = true;
    double fac = clampd(s->safety * pow(e, -1.0 / k), s->max_shrink, 1.0);
    c->last_factor = fac;
    if (dt <= s->dt_min * (1 + 1e-12)) return STEP_FAIL_MIN_STEP; /* already tried at the minimum */
    if (c->rejections >= s->max_rejections) return STEP_FAIL_REJECTIONS;
    c->dt = clampd(dt * fac, s->dt_min, s->dt_max);
    return STEP_RETRY;
}

StepDecision stepctl_solver_failure(StepController *c, double dt) {
    const StepControllerSettings *s = &c->set;
    c->solver_failures++;
    c->solver_failures_total++;
    c->after_failure = true;
    c->last_factor = s->solver_shrink;
    if (c->solver_failures > s->max_solver_failures) return STEP_FAIL_SOLVER;
    if (dt <= s->dt_min * (1 + 1e-12)) return STEP_FAIL_MIN_STEP;
    c->dt = clampd(dt * s->solver_shrink, s->dt_min, s->dt_max);
    return STEP_RETRY;
}
