/* orchestrator.c - shared lifecycle of participating solvers: graph, coupled iteration, commit and rollback
 * (see orchestrator.h) */
#include "orchestrator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    OrchCoupling c;
    int n;
    double *buf;             /* the value last transferred */
    bool cut;                /* enters its cycle backwards: predicted and relaxed */
    int comp;                /* component of its target */
} Link;

struct Orchestrator {
    OrchSettings set;
    EventSchedule ev;
    OrchParticipant part[ORCH_MAX_PARTICIPANTS];
    int np;
    Link link[ORCH_MAX_COUPLINGS];
    int nl;
    int comp_of[ORCH_MAX_PARTICIPANTS];
    int ncomp;
    int order[ORCH_MAX_PARTICIPANTS];     /* components in topological order */
    int comp_size[ORCH_MAX_PARTICIPANTS];
    bool comp_cycle[ORCH_MAX_PARTICIPANTS];
    bool trialed[ORCH_MAX_PARTICIPANTS];   /* has a pending trial in this attempt */
    bool ran[ORCH_MAX_PARTICIPANTS];       /* attempted a trial in this attempt, successfully or not */
    double t;
    long fixed_index;
    StepController ctl;
    OrchWork work;
    int order_min;
    /* Aitken work vectors over all cut links of one cycle, and a scratch export buffer */
    double *r, *r_prev, *scratch;
    size_t rcap;
};

const char *orch_status_name(OrchStatus s) {
    static const char *const N[] = {"STEPPED",        "FINISHED",          "INPUT",      "MIN_STEP", "TOO_MANY_REJECTIONS", "SOLVER_FAILURE",
                                    "COUPLING_FAILURE", "EVALUATION_FAILURE", "STEP_LIMIT", "RESOURCE_LIMIT"};
    return s >= ORCH_STEPPED && s <= ORCH_FAIL_RESOURCE ? N[s] : "UNKNOWN";
}

void orch_free(Orchestrator *o) {
    if (!o) return;
    for (int i = 0; i < o->nl; i++) free(o->link[i].buf);
    events_free(&o->ev);
    free(o->r), free(o->r_prev), free(o->scratch);
    free(o);
}

/* ---- graph: Tarjan's strongly connected components, then a topological order of the components ---------------- */

typedef struct {
    const Orchestrator *o;
    int index[ORCH_MAX_PARTICIPANTS], low[ORCH_MAX_PARTICIPANTS], stack[ORCH_MAX_PARTICIPANTS];
    bool on[ORCH_MAX_PARTICIPANTS];
    int next, sp, ncomp;
    int *comp_of;
} Tarjan;

static void strongconnect(Tarjan *tj, int v) {
    tj->index[v] = tj->low[v] = tj->next++;
    tj->stack[tj->sp++] = v, tj->on[v] = true;
    for (int i = 0; i < tj->o->nl; i++) {
        if (tj->o->link[i].c.from != v) continue;
        int w = tj->o->link[i].c.to;
        if (tj->index[w] < 0) {
            strongconnect(tj, w);
            if (tj->low[w] < tj->low[v]) tj->low[v] = tj->low[w];
        } else if (tj->on[w] && tj->index[w] < tj->low[v]) {
            tj->low[v] = tj->index[w];
        }
    }
    if (tj->low[v] == tj->index[v]) {
        int w;
        do {
            w = tj->stack[--tj->sp];
            tj->on[w] = false;
            tj->comp_of[w] = tj->ncomp;
        } while (w != v);
        tj->ncomp++;
    }
}

static bool build_graph(Orchestrator *o, char *err, size_t errlen) {
    Tarjan tj = {.o = o, .comp_of = o->comp_of};
    for (int i = 0; i < o->np; i++) tj.index[i] = -1;
    for (int i = 0; i < o->np; i++)
        if (tj.index[i] < 0) strongconnect(&tj, i);
    o->ncomp = tj.ncomp;
    for (int k = 0; k < o->ncomp; k++) o->comp_size[k] = 0, o->comp_cycle[k] = false;
    for (int i = 0; i < o->np; i++) o->comp_size[o->comp_of[i]]++;
    for (int i = 0; i < o->nl; i++)
        if (o->comp_of[o->link[i].c.from] == o->comp_of[o->link[i].c.to]) o->comp_cycle[o->comp_of[o->link[i].c.from]] = true;
    /* Kahn on the component graph */
    int indeg[ORCH_MAX_PARTICIPANTS] = {0};
    bool edge[ORCH_MAX_PARTICIPANTS][ORCH_MAX_PARTICIPANTS] = {{false}};
    for (int i = 0; i < o->nl; i++) {
        int a = o->comp_of[o->link[i].c.from], b = o->comp_of[o->link[i].c.to];
        if (a != b && !edge[a][b]) edge[a][b] = true, indeg[b]++;
    }
    int n = 0;
    bool done[ORCH_MAX_PARTICIPANTS] = {false};
    while (n < o->ncomp) {
        int pick = -1;
        for (int k = 0; k < o->ncomp && pick < 0; k++)
            if (!done[k] && indeg[k] == 0) pick = k;
        if (pick < 0) {
            snprintf(err, errlen, "internal error: the component graph is not acyclic");
            return false;
        }
        done[pick] = true;
        o->order[n++] = pick;
        for (int k = 0; k < o->ncomp; k++)
            if (edge[pick][k]) indeg[k]--;
    }
    for (int k = 0; k < o->ncomp; k++) {
        if (!o->comp_cycle[k]) continue;
        for (int i = 0; i < o->np; i++)
            if (o->comp_of[i] == k && !o->part[i].transient) {
                snprintf(err, errlen, "participant '%s' is quasi-static but inside a feedback cycle: iterating a quasi-static participant with a transient one is not supported",
                         o->part[i].name);
                return false;
            }
    }
    /* backward links of a cycle (to a participant that runs no later than its source) are predicted and relaxed */
    for (int i = 0; i < o->nl; i++) {
        Link *l = &o->link[i];
        l->comp = o->comp_of[l->c.to];
        l->cut = o->comp_of[l->c.from] == l->comp && l->c.from >= l->c.to;
    }
    return true;
}

Orchestrator *orch_create(const OrchSettings *s, const EventSchedule *events, const OrchParticipant *parts, int np, const OrchCoupling *coup, int nc,
                          char *err, size_t errlen) {
    if (np < 1 || np > ORCH_MAX_PARTICIPANTS || nc < 0 || nc > ORCH_MAX_COUPLINGS) {
        snprintf(err, errlen, "an orchestration needs 1 to %d participants and at most %d couplings", ORCH_MAX_PARTICIPANTS, ORCH_MAX_COUPLINGS);
        return NULL;
    }
    Orchestrator *o = calloc(1, sizeof *o);
    if (!o) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    o->set = *s;
    if (o->set.max_coupling_iterations <= 0) o->set.max_coupling_iterations = 50;
    if (!(o->set.coupling_relative > 0) && !(o->set.coupling_absolute > 0)) o->set.coupling_relative = 1e-8;
    if (!(o->set.relaxation > 0 && o->set.relaxation <= 1)) o->set.relaxation = 0.5;
    o->np = np;
    memcpy(o->part, parts, (size_t)np * sizeof *parts);
    o->order_min = 8;
    int ntransient = 0;
    for (int i = 0; i < np; i++) {
        const OrchParticipant *p = &o->part[i];
        if (p->transient) {
            ntransient++;
            if (!p->trial || !p->commit) {
                snprintf(err, errlen, "transient participant '%s' needs trial and commit", p->name);
                goto fail;
            }
            if (p->order < o->order_min) o->order_min = p->order;
        } else if (!p->evaluate) {
            snprintf(err, errlen, "quasi-static participant '%s' needs evaluate", p->name);
            goto fail;
        }
    }
    if (ntransient == 0) {
        snprintf(err, errlen, "an orchestration needs at least one transient participant to advance time");
        goto fail;
    }
    if (o->order_min < 1) o->order_min = 1;
    o->nl = nc;
    size_t rsize = 0;
    for (int i = 0; i < nc; i++) {
        Link *l = &o->link[i];
        l->c = coup[i];
        if (l->c.from < 0 || l->c.from >= np || l->c.to < 0 || l->c.to >= np || l->c.from == l->c.to) {
            snprintf(err, errlen, "coupling %d joins invalid participants (%d -> %d)", i, l->c.from, l->c.to);
            goto fail;
        }
        const OrchParticipant *pf = &o->part[l->c.from], *pt = &o->part[l->c.to];
        if (!pf->field_size || !pf->export_field || !pt->field_size || !pt->import_field) {
            snprintf(err, errlen, "coupling %s -> %s needs field functions on both participants", pf->name, pt->name);
            goto fail;
        }
        int nout = pf->field_size(pf->self, l->c.field_out), nin = pt->field_size(pt->self, l->c.field_in);
        if (nout <= 0 || nout != nin) {
            snprintf(err, errlen, "coupling %s.%s -> %s.%s: field sizes %d and %d do not match", pf->name, l->c.field_out, pt->name, l->c.field_in, nout, nin);
            goto fail;
        }
        l->n = nout;
        l->buf = calloc((size_t)nout, sizeof(double));
        if (!l->buf) {
            snprintf(err, errlen, "out of memory");
            goto fail;
        }
        rsize += (size_t)nout;
    }
    o->rcap = rsize ? rsize : 1;
    o->r = calloc(o->rcap, sizeof(double)), o->r_prev = calloc(o->rcap, sizeof(double)), o->scratch = calloc(o->rcap, sizeof(double));
    if (!o->r || !o->r_prev || !o->scratch) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }
    if (!build_graph(o, err, errlen)) goto fail;
    double duration = o->set.t_end - o->set.t_start;
    events_init(&o->ev, o->set.t_start, o->set.t_end);
    for (int i = 0; events && i < events->n; i++)
        if (!events_add(&o->ev, events->ev[i].t, events->ev[i].kinds)) {
            snprintf(err, errlen, "out of memory");
            goto fail;
        }
    if (!events_finalize(&o->ev)) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }
    if (o->set.mode == THERMAL_STEPPING_FIXED) {
        if (!(o->set.dt_fixed > 0)) {
            snprintf(err, errlen, "fixed stepping needs a positive step");
            goto fail;
        }
    } else {
        StepControllerSettings cs = o->set.control;
        cs.order = o->order_min + 1;
        if (!stepctl_defaults(&cs, duration, err, errlen) || !stepctl_init(&o->ctl, &cs, err, errlen)) goto fail;
        o->set.control = cs;
    }
    o->t = o->set.t_start;
    return o;
fail:
    orch_free(o);
    return NULL;
}

double orch_time(const Orchestrator *o) { return o->t; }
bool orch_finished(const Orchestrator *o) { return o->t >= o->set.t_end - o->ev.tol; }
const OrchWork *orch_work(const Orchestrator *o) { return &o->work; }
StepController *orch_controller(Orchestrator *o) { return &o->ctl; }
long orch_fixed_index(const Orchestrator *o) { return o->fixed_index; }

void orch_set_clock(Orchestrator *o, double t, const StepController *ctl, long fixed_index) {
    o->t = t;
    if (ctl) o->ctl = *ctl;
    o->fixed_index = fixed_index;
}

void orch_set_work(Orchestrator *o, const OrchWork *w) { o->work = *w; }

/* ---- transfers ---------------------------------------------------------------------------------------------------- */

/* fills the link buffer from the exporting participant: its pending trial when it has one in this attempt */
static bool pull(Orchestrator *o, Link *l) {
    const OrchParticipant *pf = &o->part[l->c.from];
    return pf->export_field(pf->self, l->c.field_out, o->trialed[l->c.from], l->buf);
}

static bool push(Orchestrator *o, const Link *l) {
    const OrchParticipant *pt = &o->part[l->c.to];
    return pt->import_field(pt->self, l->c.field_in, l->buf);
}

/* imports every link into participant i; cut links keep their (predicted or relaxed) buffers */
static bool import_all(Orchestrator *o, int i, char *err, size_t errlen) {
    for (int k = 0; k < o->nl; k++) {
        Link *l = &o->link[k];
        if (l->c.to != i) continue;
        if (!l->cut && !pull(o, l)) {
            snprintf(err, errlen, "%s could not export %s", o->part[l->c.from].name, l->c.field_out);
            return false;
        }
        if (!push(o, l)) {
            snprintf(err, errlen, "%s could not import %s", o->part[i].name, l->c.field_in);
            return false;
        }
    }
    return true;
}

/* ---- one attempt over [t0, t1] -------------------------------------------------------------------------------------- */

typedef struct {
    bool ok;
    OrchStatus fail;              /* when !ok */
    OrchFailureClass failure;     /* solver failures */
    double error;                 /* largest estimate, -1 when none */
    int coupling_iterations;
    double coupling_residual;     /* largest final normalised change */
    char err[1024];
} Attempt;

/* one participant's trial; its temporal error estimate raises *error (the attempt's for a sequential participant, the
 * sweep's for a member of a cycle, so that only the converged sweep counts) */
static bool trial_one(Orchestrator *o, int i, double t0, double t1, bool estimate, Attempt *a, double *error) {
    const OrchParticipant *p = &o->part[i];
    double e = -1;
    OrchFailureClass f = ORCH_FAILURE_NONE;
    char perr[768] = "";
    if (!import_all(o, i, a->err, sizeof a->err)) {
        a->fail = ORCH_FAIL_INPUT;
        return false;
    }
    o->ran[i] = true;
    if (!p->trial(p->self, t0, t1, estimate, &e, &f, perr, sizeof perr)) {
        a->failure = f;
        a->fail = f == ORCH_FAILURE_INPUT ? ORCH_FAIL_INPUT : (f == ORCH_FAILURE_RESOURCE ? ORCH_FAIL_RESOURCE : ORCH_FAIL_SOLVER);
        snprintf(a->err, sizeof a->err, "%s: %s", p->name, perr);
        return false;
    }
    o->trialed[i] = true;
    if (e > *error) *error = e;
    return true;
}

static void attempt(Orchestrator *o, double t0, double t1, bool estimate, Attempt *a) {
    memset(a, 0, sizeof *a);
    a->error = -1;
    a->ok = true;
    for (int i = 0; i < o->np; i++) o->trialed[i] = o->ran[i] = false;
    for (int ci = 0; ci < o->ncomp && a->ok; ci++) {
        int k = o->order[ci];
        if (!o->comp_cycle[k]) {
            for (int i = 0; i < o->np && a->ok; i++)
                if (o->comp_of[i] == k && o->part[i].transient) a->ok = trial_one(o, i, t0, t1, estimate, a, &a->error);
            continue;
        }
        /* feedback cycle: predict the backward links from the accepted states */
        size_t nr = 0;
        for (int l = 0; l < o->nl; l++)
            if (o->link[l].comp == k && o->link[l].cut) {
                if (!pull(o, &o->link[l])) {
                    a->ok = false, a->fail = ORCH_FAIL_INPUT;
                    snprintf(a->err, sizeof a->err, "%s could not export %s", o->part[o->link[l].c.from].name, o->link[l].c.field_out);
                    return;
                }
                nr += (size_t)o->link[l].n;
            }
        double omega = o->set.relaxation;
        bool converged = false;
        int it;
        double rnorm = INFINITY, sweep_error = -1;
        for (it = 1; it <= o->set.max_coupling_iterations && a->ok; it++) {
            sweep_error = -1; /* the estimates of unconverged sweeps describe a different problem and are not kept */
            for (int i = 0; i < o->np && a->ok; i++)
                if (o->comp_of[i] == k) a->ok = trial_one(o, i, t0, t1, estimate, a, &sweep_error);
            if (!a->ok) break;
            /* the change of the backward links: x_new - x */
            size_t off = 0;
            double worst = 0, rd = 0, dd = 0;
            for (int l = 0; l < o->nl; l++) {
                Link *lk = &o->link[l];
                if (lk->comp != k || !lk->cut) continue;
                const OrchParticipant *pf = &o->part[lk->c.from];
                double *tmp = o->scratch;
                if (!pf->export_field(pf->self, lk->c.field_out, true, tmp)) {
                    a->ok = false, a->fail = ORCH_FAIL_INPUT;
                    snprintf(a->err, sizeof a->err, "%s could not export %s", pf->name, lk->c.field_out);
                    break;
                }
                double scale = 0;
                for (int q = 0; q < lk->n; q++) scale = fmax(scale, fabs(tmp[q]));
                for (int q = 0; q < lk->n; q++) {
                    double r = tmp[q] - lk->buf[q];
                    double tol = o->set.coupling_absolute + o->set.coupling_relative * scale;
                    double nrm = tol > 0 ? fabs(r) / tol : (r != 0 ? INFINITY : 0);
                    if (nrm > worst) worst = nrm;
                    if (it > 1) {
                        double dr = r - o->r_prev[off + (size_t)q];
                        rd += o->r_prev[off + (size_t)q] * dr, dd += dr * dr;
                    }
                    o->r[off + (size_t)q] = r;
                }
                off += (size_t)lk->n;
            }
            if (!a->ok) break;
            rnorm = worst;
            if (worst <= 1) {
                converged = true;
                if (sweep_error > a->error) a->error = sweep_error;
                break;
            }
            if (o->set.aitken && it > 1 && dd > 0) {
                double w = -omega * rd / dd;
                if (isfinite(w)) omega = fmin(1.0, fmax(0.01, w));
            }
            off = 0;
            for (int l = 0; l < o->nl; l++) {
                Link *lk = &o->link[l];
                if (lk->comp != k || !lk->cut) continue;
                for (int q = 0; q < lk->n; q++) lk->buf[q] += omega * o->r[off + (size_t)q];
                off += (size_t)lk->n;
            }
            memcpy(o->r_prev, o->r, nr * sizeof(double));
            for (int i = 0; i < o->np; i++)
                if (o->comp_of[i] == k) o->trialed[i] = false; /* the next sweep starts every member again from its accepted state */
        }
        a->coupling_iterations += it > o->set.max_coupling_iterations ? o->set.max_coupling_iterations : it;
        if (isfinite(rnorm) && rnorm > a->coupling_residual) a->coupling_residual = rnorm;
        if (a->ok && !converged) {
            a->ok = false, a->fail = ORCH_FAIL_COUPLING;
            snprintf(a->err, sizeof a->err, "the feedback cycle did not converge in %d iterations over [%.6g, %.6g] s (normalised change %.3g)",
                     o->set.max_coupling_iterations, t0, t1, rnorm);
        }
    }
}

static bool evaluate_quasistatic(Orchestrator *o, double t, bool sync, bool initial, char *err, size_t errlen) {
    for (int ci = 0; ci < o->ncomp; ci++) {
        int k = o->order[ci];
        for (int i = 0; i < o->np; i++) {
            const OrchParticipant *p = &o->part[i];
            if (o->comp_of[i] != k || p->transient) continue;
            if (p->time_independent && !initial) continue;
            if (!sync && !p->history_dependent) continue;
            if (!import_all(o, i, err, errlen)) return false;
            char perr[768];
            if (!p->evaluate(p->self, t, perr, sizeof perr)) {
                snprintf(err, errlen, "%s at t = %.6g s: %s", p->name, t, perr);
                return false;
            }
            o->work.evaluations++;
        }
    }
    return true;
}

bool orch_initialize(Orchestrator *o, char *err, size_t errlen) {
    for (int i = 0; i < o->np; i++) o->trialed[i] = false;
    return evaluate_quasistatic(o, o->t, true, true, err, errlen);
}

static void notify(Orchestrator *o, OrchAttemptOutcome outcome, StepDecision d) {
    for (int i = 0; i < o->np; i++)
        if (o->part[i].transient && o->ran[i] && o->part[i].attempt_done) o->part[i].attempt_done(o->part[i].self, outcome, d);
}

static void commit_all(Orchestrator *o) {
    for (int i = 0; i < o->np; i++)
        if (o->part[i].transient) o->part[i].commit(o->part[i].self);
    notify(o, ORCH_ATTEMPT_ACCEPTED, STEP_ACCEPT);
    for (int i = 0; i < o->np; i++) o->trialed[i] = o->ran[i] = false;
}

static OrchStatus finish_step(Orchestrator *o, const Attempt *a, double t0, double t1, int event, OrchStepReport *rep, char *err, size_t errlen) {
    commit_all(o);
    o->t = t1;
    o->work.accepted++;
    o->work.coupling_iterations += a->coupling_iterations;
    o->work.coupling_residual_max = fmax(o->work.coupling_residual_max, a->coupling_residual);
    o->work.error_max = fmax(o->work.error_max, a->error);
    rep->t0 = t0, rep->t1 = t1, rep->dt = t1 - t0;
    rep->event = event;
    rep->event_kinds = event >= 0 ? o->ev.ev[event].kinds : 0;
    rep->coupling_iterations = a->coupling_iterations;
    rep->coupling_residual = a->coupling_residual;
    rep->error = a->error;
    bool sync = (rep->event_kinds & (EVENT_OUTPUT | EVENT_END)) != 0;
    rep->evaluated = true;
    if (!evaluate_quasistatic(o, t1, sync, false, err, errlen)) return ORCH_FAIL_EVALUATION;
    return ORCH_STEPPED;
}

OrchStatus orch_step(Orchestrator *o, OrchStepReport *rep, char *err, size_t errlen) {
    memset(rep, 0, sizeof *rep);
    rep->event = -1, rep->error = -1;
    if (err && errlen) err[0] = 0;
    if (orch_finished(o)) return ORCH_FINISHED;
    Attempt a;
    if (o->set.mode == THERMAL_STEPPING_FIXED) {
        double g = o->set.t_start + (double)(o->fixed_index + 1) * o->set.dt_fixed;
        bool grid = true;
        if (g > o->set.t_end - o->ev.tol) g = o->set.t_end;
        int k = events_next(&o->ev, o->t), event = -1;
        double t0 = o->t, t1 = g;
        if (k >= 0 && o->ev.ev[k].t <= g + o->ev.tol) {
            grid = fabs(o->ev.ev[k].t - g) <= o->ev.tol;
            t1 = o->ev.ev[k].t;
            event = k;
        }
        o->work.attempts++;
        rep->attempts = 1;
        attempt(o, t0, t1, false, &a);
        if (!a.ok) {
            if (a.fail == ORCH_FAIL_COUPLING) o->work.rejected_coupling++;
            else o->work.solver_failures++;
            /* fixed steps are not retried: the step size is the user's choice */
            notify(o, a.fail == ORCH_FAIL_COUPLING ? ORCH_ATTEMPT_REJECTED_COUPLING : ORCH_ATTEMPT_FAILED, STEP_FAIL_SOLVER);
            snprintf(err, errlen, "%s", a.err);
            return a.fail;
        }
        OrchStatus s = finish_step(o, &a, t0, t1, event, rep, err, errlen);
        if (grid) o->fixed_index++;
        return s;
    }
    if (o->ctl.accepted >= o->set.control.max_steps) {
        snprintf(err, errlen, "the limit of %ld accepted steps was reached at t = %.6g s", o->set.control.max_steps, o->t);
        return ORCH_FAIL_STEP_LIMIT;
    }
    for (;;) {
        bool limited;
        int event;
        double dt = events_limit_step(&o->ev, o->t, o->ctl.dt, &event, &limited);
        double t0 = o->t, t1 = event >= 0 ? o->ev.ev[event].t : t0 + dt;
        dt = t1 - t0;
        o->work.attempts++;
        rep->attempts++;
        attempt(o, t0, t1, true, &a);
        if (!a.ok) {
            OrchAttemptOutcome outcome = a.fail == ORCH_FAIL_COUPLING ? ORCH_ATTEMPT_REJECTED_COUPLING : ORCH_ATTEMPT_FAILED;
            if (a.fail == ORCH_FAIL_INPUT || a.fail == ORCH_FAIL_RESOURCE) {
                o->work.solver_failures++;
                notify(o, outcome, STEP_FAIL_SOLVER);
                snprintf(err, errlen, "%s", a.err);
                return a.fail;
            }
            if (a.fail == ORCH_FAIL_COUPLING) o->work.rejected_coupling++;
            else o->work.solver_failures++;
            StepDecision d = stepctl_solver_failure(&o->ctl, dt);
            notify(o, outcome, d);
            if (d == STEP_RETRY) continue;
            snprintf(err, errlen, "%s at t = %.6g s with a step of %.4g s (%s): %s", a.fail == ORCH_FAIL_COUPLING ? "coupling failure" : "solver failure", t0, dt,
                     step_decision_name(d), a.err);
            return d == STEP_FAIL_MIN_STEP ? ORCH_FAIL_MIN_STEP : a.fail;
        }
        double e = a.error < 0 ? 0 : a.error;
        StepDecision d = stepctl_error(&o->ctl, dt, e, limited);
        if (d == STEP_ACCEPT) return finish_step(o, &a, t0, t1, event, rep, err, errlen);
        o->work.rejected_error++;
        notify(o, ORCH_ATTEMPT_REJECTED_ERROR, d);
        if (d == STEP_RETRY) continue;
        snprintf(err, errlen, "%s at t = %.6g s: a step of %.4g s has a normalised error of %.3g (the largest temporal estimate of the participants) and the controller cannot reduce it further",
                 step_decision_name(d), t0, dt, e);
        return d == STEP_FAIL_MIN_STEP ? ORCH_FAIL_MIN_STEP : ORCH_FAIL_REJECTIONS;
    }
}

JsonValue *orch_describe(const Orchestrator *o) {
    JsonValue *v = json_object();
    JsonValue *ps = json_set_array(v, "participants");
    for (int i = 0; i < o->np; i++) {
        const OrchParticipant *p = &o->part[i];
        JsonValue *po = json_object();
        json_set_string(po, "name", p->name);
        json_set_string(po, "kind", p->transient ? "transient" : "quasi-static");
        if (p->transient) json_set_int(po, "order", p->order);
        else
            json_set_string(po, "evaluated", p->time_independent    ? "once, at the start: its result does not depend on time"
                                             : p->history_dependent ? "at every accepted step (the model has history)"
                                                                    : "at stored times only, which is exact for a model without history");
        json_push(ps, po);
    }
    JsonValue *cs = json_set_array(v, "couplings");
    for (int i = 0; i < o->nl; i++) {
        const Link *l = &o->link[i];
        JsonValue *co = json_object();
        json_set_string(co, "from", o->part[l->c.from].name);
        json_set_string(co, "field_out", l->c.field_out);
        json_set_string(co, "to", o->part[l->c.to].name);
        json_set_string(co, "field_in", l->c.field_in);
        json_set_string(co, "transfer", l->c.conservative ? "conservative (integrated quantity)" : "intensive (point values)");
        json_set_int(co, "values", l->n);
        json_push(cs, co);
    }
    JsonValue *ord = json_set_array(v, "execution_order");
    bool any_cycle = false;
    for (int ci = 0; ci < o->ncomp; ci++) {
        int k = o->order[ci];
        JsonValue *g = json_object(), *mem = json_set_array(g, "participants");
        for (int i = 0; i < o->np; i++)
            if (o->comp_of[i] == k) json_push(mem, json_string(o->part[i].name));
        json_set_string(g, "coupling", o->comp_cycle[k] ? "feedback cycle: iterated to convergence each step (strong coupling)" : "sequential (one-way)");
        any_cycle |= o->comp_cycle[k];
        json_push(ord, g);
    }
    json_set_string(v, "character",
                    any_cycle ? "contains strongly coupled feedback cycles" : "one-way: every coupling runs forward only, nothing feeds back within a step");
    return v;
}
