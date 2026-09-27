/* tsteptest.c - verification of adaptive time integration (src/fem/timestep.c, src/fem/thermal_integrator.c)
 *
 * Acceptance criteria were fixed before the first run: "Thermal Sim/verification/stageA-criteria.md" (A1..A11).
 *   make build/tsteptest && ./build/tsteptest */
#include "../src/fem/thermal.h"
#include "../src/fem/thermal_integrator.h"
#include "../src/fem/timestep.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail, g_pass;
#define CHECK(cond, ...)                                                                                                                              \
    do {                                                                                                                                              \
        if (cond) g_pass++;                                                                                                                           \
        else {                                                                                                                                        \
            g_fail++;                                                                                                                                 \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                                                             \
            printf(__VA_ARGS__);                                                                                                                      \
            printf("\n");                                                                                                                             \
        }                                                                                                                                             \
    } while (0)

static double wall(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* ---- structured bar meshes ------------------------------------------------------------------------------------ */

typedef struct {
    int nx, ny, nz, nn, ne;
    double L[3];
    double *xyz;
    int *conn;
} Box;

static int nid(const Box *b, int i, int j, int k) { return i + (b->nx + 1) * (j + (b->ny + 1) * k); }
static int eid(const Box *b, int i, int j, int k) { return i + b->nx * (j + b->ny * k); }

static void box_init(Box *b, int nx, int ny, int nz, double Lx, double Ly, double Lz) {
    b->nx = nx, b->ny = ny, b->nz = nz;
    b->L[0] = Lx, b->L[1] = Ly, b->L[2] = Lz;
    b->nn = (nx + 1) * (ny + 1) * (nz + 1);
    b->ne = nx * ny * nz;
    b->xyz = malloc(3 * (size_t)b->nn * sizeof(double));
    b->conn = malloc(8 * (size_t)b->ne * sizeof(int));
    for (int k = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) {
                double *x = b->xyz + 3 * (size_t)nid(b, i, j, k);
                x[0] = Lx * i / nx, x[1] = Ly * j / ny, x[2] = Lz * k / nz;
            }
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                int *c = b->conn + 8 * (size_t)eid(b, i, j, k);
                c[0] = nid(b, i, j, k), c[1] = nid(b, i + 1, j, k), c[2] = nid(b, i + 1, j + 1, k), c[3] = nid(b, i, j + 1, k);
                c[4] = nid(b, i, j, k + 1), c[5] = nid(b, i + 1, j, k + 1), c[6] = nid(b, i + 1, j + 1, k + 1), c[7] = nid(b, i, j + 1, k + 1);
            }
}

static void box_free(Box *b) { free(b->xyz), free(b->conn); }

/* ---- problems -------------------------------------------------------------------------------------------------- */

typedef struct {
    Box b;
    double tt[1], kv[1], rhov[1], cpv[1];
    ThermalMaterial mat;
    unsigned char *fixed;
    double *fixT, *src, *Tinit;
    ThermalFace faces[64];
    int nf;
    ThermalModel m;
    double t_end;
    int nout;
    double tout[16];
    /* schedule: a volumetric source Q on the elements flagged in src_mask during [q_on, q_off) */
    bool sched;
    double q_on, q_off, Q;
    unsigned char *src_mask;
} Problem;

static void problem_free(Problem *p) {
    free(p->fixed), free(p->fixT), free(p->src), free(p->Tinit), free(p->src_mask);
    box_free(&p->b);
}

static void problem_base(Problem *p, int nx, double L, double W, double k, double rho, double cp, double T0) {
    memset(p, 0, sizeof *p);
    box_init(&p->b, nx, 1, 1, L, W, W);
    p->tt[0] = 293.15, p->kv[0] = k, p->rhov[0] = rho, p->cpv[0] = cp;
    p->mat.k = (ThermalTable){1, p->tt, p->kv};
    p->mat.rho = (ThermalTable){1, p->tt, p->rhov};
    p->mat.cp = (ThermalTable){1, p->tt, p->cpv};
    int nn = p->b.nn, ne = p->b.ne;
    p->fixed = calloc((size_t)nn, 1);
    p->fixT = calloc((size_t)nn, sizeof(double));
    p->src = calloc((size_t)ne, sizeof(double));
    p->src_mask = calloc((size_t)ne, 1);
    p->Tinit = malloc((size_t)nn * sizeof(double));
    for (int n = 0; n < nn; n++) p->Tinit[n] = T0;
    p->m = (ThermalModel){.nnodes = nn, .nelems = ne, .xyz = p->b.xyz, .conn = p->b.conn, .nmat = 1, .mat = &p->mat, .fixed = p->fixed, .fixed_T = p->fixT,
                          .elem_source = p->src};
}

static void fix_end(Problem *p, int i, double T) {
    for (int j = 0; j <= p->b.ny; j++)
        for (int k = 0; k <= p->b.nz; k++) {
            int n = nid(&p->b, i, j, k);
            p->fixed[n] = 1, p->fixT[n] = T, p->Tinit[n] = T;
        }
}

/* smooth: the lowest discrete eigenmode of a bar with both ends held, which lumped linear elements reproduce exactly */
static void problem_sine(Problem *p) {
    problem_base(p, 20, 0.1, 0.005, 15, 8000, 500, 300);
    fix_end(p, 0, 300), fix_end(p, 20, 300);
    for (int n = 0; n < p->b.nn; n++) p->Tinit[n] = 300 + 50 * sin(M_PI * p->b.xyz[3 * (size_t)n] / 0.1);
    p->t_end = 400;
}

/* multi-scale: one end suddenly held 100 K above the initial temperature, the other insulated */
static void problem_heat(Problem *p) {
    problem_base(p, 20, 0.1, 0.005, 15, 8000, 500, 300);
    fix_end(p, 0, 400);
    p->t_end = 2000;
    p->nout = 5;
    double to[5] = {1, 10, 100, 1000, 2000};
    memcpy(p->tout, to, sizeof to);
}

/* one-phase Stefan problem: solid at the melting temperature, one end raised to Tw */
static void problem_stefan(Problem *p, double tend) {
    problem_base(p, 160, 0.08, 0.002, 2, 1000, 2000, 300);
    p->mat.latent_heat = 1e5, p->mat.solidus = 300, p->mat.liquidus = 300.5;
    fix_end(p, 0, 350);
    p->t_end = tend;
}

/* volumetric source on the left half, convection at the right end */
static void problem_source(Problem *p) {
    problem_base(p, 20, 0.1, 0.005, 15, 8000, 500, 300);
    for (int e = 0; e < p->b.ne; e++)
        if (e < 10) p->src[e] = 2e6;
    p->nf = 0;
    for (int j = 0; j < p->b.ny; j++)
        for (int k = 0; k < p->b.nz; k++) p->faces[p->nf++] = (ThermalFace){eid(&p->b, 19, j, k), 3, THERMAL_CONVECTION, 100, 300};
    p->m.nfaces = p->nf, p->m.faces = p->faces;
    p->t_end = 600;
    p->nout = 3;
    double to[3] = {50, 200, 600};
    memcpy(p->tout, to, sizeof to);
}

static bool schedule_fn(void *ctx, ThermalModel *m, double t0, double t1, char *err, size_t errlen) {
    Problem *p = ctx;
    if (!p->sched) return true;
    double tm = 0.5 * (t0 + t1); /* piecewise constant between events that steps land on: the midpoint is unambiguous */
    bool on = tm >= p->q_on && tm < p->q_off;
    for (int e = 0; e < m->nelems; e++) p->src[e] = on && p->src_mask[e] ? p->Q : 0;
    (void)err, (void)errlen;
    return true;
}

/* ---- running ---------------------------------------------------------------------------------------------------- */

typedef struct {
    ThermalIntegratorStatus status;
    char err[1024];
    double *frames; /* nout * nn */
    int frames_stored;
    ThermalBudget budget;
    ThermalWork work;
    double seconds;
    double t_final;
    double *T_final;
    int accepted_steps;
    /* accepted step intervals, for event checks */
    int nint, cap;
    double *iv;
    double liquid_final;
} Run;

static void run_free(Run *r) { free(r->frames), free(r->T_final), free(r->iv); }

static EventSchedule problem_events(const Problem *p) {
    EventSchedule ev;
    events_init(&ev, 0, p->t_end);
    for (int i = 0; i < p->nout; i++) events_add(&ev, p->tout[i], EVENT_OUTPUT);
    if (p->sched) events_add(&ev, p->q_on, EVENT_LOAD), events_add(&ev, p->q_off, EVENT_LOAD);
    events_finalize(&ev);
    return ev;
}

static void run_problem(Problem *p, const ThermalIntegratorSettings *base, double theta, double picard_tol, Run *r) {
    memset(r, 0, sizeof *r);
    int nn = p->b.nn;
    r->frames = calloc((size_t)(p->nout ? p->nout : 1) * (size_t)nn, sizeof(double));
    r->T_final = malloc((size_t)nn * sizeof(double));
    ThermalOptions o = {.theta = theta, .picard_tol = picard_tol, .max_picard = 400, .pcg_tol = 1e-13};
    ThermalSolver *s = thermal_create(&p->m, &o, r->err, sizeof r->err);
    if (!s) {
        r->status = TINT_FAIL_INPUT;
        return;
    }
    ThermalIntegratorSettings set = *base;
    set.t_start = 0, set.t_end = p->t_end;
    set.schedule = schedule_fn, set.schedule_ctx = p;
    EventSchedule ev = problem_events(p);
    double w0 = wall();
    ThermalIntegrator *ti = tint_create(s, &p->m, &set, &ev, p->Tinit, r->err, sizeof r->err);
    events_free(&ev);
    if (!ti) {
        thermal_free(s);
        r->status = TINT_FAIL_INPUT;
        return;
    }
    ThermalStepReport rep;
    for (;;) {
        r->status = tint_step(ti, &rep, r->err, sizeof r->err);
        if (r->status != TINT_STEPPED) break;
        r->accepted_steps++;
        if (r->nint == r->cap) {
            r->cap = r->cap ? 2 * r->cap : 256;
            r->iv = realloc(r->iv, 2 * (size_t)r->cap * sizeof(double));
        }
        r->iv[2 * r->nint] = rep.t0, r->iv[2 * r->nint + 1] = rep.t1;
        r->nint++;
        r->liquid_final = rep.last.liquid_volume;
        if (rep.event_kinds & EVENT_OUTPUT)
            for (int i = 0; i < p->nout; i++)
                if (rep.t1 == p->tout[i]) { /* exact landing: equality, not a tolerance */
                    memcpy(r->frames + (size_t)i * (size_t)nn, tint_temperature(ti), (size_t)nn * sizeof(double));
                    r->frames_stored++;
                }
    }
    if (r->status == TINT_FINISHED) r->status = TINT_STEPPED; /* reached the end */
    r->seconds = wall() - w0;
    r->budget = *tint_budget(ti);
    r->work = *tint_work(ti);
    r->t_final = tint_time(ti);
    memcpy(r->T_final, tint_temperature(ti), (size_t)nn * sizeof(double));
    tint_free(ti);
    thermal_free(s);
}

static ThermalIntegratorSettings fixed_settings(double dt) {
    ThermalIntegratorSettings s = {0};
    s.mode = THERMAL_STEPPING_FIXED;
    s.dt_fixed = dt;
    s.T_reference = 300;
    return s;
}

static ThermalIntegratorSettings adaptive_settings(double rel, double atolT, double dt0) {
    ThermalIntegratorSettings s = {0};
    s.mode = THERMAL_STEPPING_ADAPTIVE;
    s.tol.relative = rel, s.tol.temperature = atolT;
    s.control.dt_initial = dt0;
    s.T_reference = 300;
    return s;
}

/* max over output frames of the largest nodal difference */
static double frame_error(const Problem *p, const Run *a, const double *ref) {
    double e = 0;
    for (size_t i = 0; i < (size_t)p->nout * (size_t)p->b.nn; i++) e = fmax(e, fabs(a->frames[i] - ref[i]));
    return e;
}

/* melt front: where the temperature crosses the middle of the mushy interval, interpolated along the bar */
static double front_position(const Problem *p, const double *T) {
    double Tf = 0.5 * (p->mat.solidus + p->mat.liquidus);
    for (int i = 0; i < p->b.nx; i++) {
        double Ta = T[nid(&p->b, i, 0, 0)], Tb = T[nid(&p->b, i + 1, 0, 0)];
        if (Ta >= Tf && Tb < Tf) return p->b.L[0] * (i + (Ta - Tf) / (Ta - Tb)) / p->b.nx;
    }
    return 0;
}

static double budget_closure(const ThermalBudget *b) {
    double supplied = b->source + b->boundary + b->prescribed + b->interface_net;
    double big = fmax(fabs(b->stored), fabs(supplied));
    return big > 0 ? fabs(b->stored - supplied) / big : 0;
}

static double budget_mismatch(const ThermalBudget *b) {
    double big = fmax(fabs(b->stored), fabs(b->enthalpy));
    return big > 0 ? fabs(b->stored - b->enthalpy) / big : 0;
}

/* ---- A10 support ------------------------------------------------------------------------------------------------ */

typedef struct {
    int nn, ne;
    double t;
    double *T;
    ThermalBudget budget;
    double *src;
    int compared, mismatches;
    const Problem *p;
} Snapshot;

static void snap_take(Snapshot *s, const ThermalIntegrator *ti, const Problem *p) {
    s->t = tint_time(ti);
    memcpy(s->T, tint_temperature(ti), (size_t)s->nn * sizeof(double));
    s->budget = *tint_budget(ti);
    memcpy(s->src, p->src, (size_t)s->ne * sizeof(double));
}

static void on_attempt_compare(void *ctx, const ThermalIntegrator *ti, const ThermalAttempt *a) {
    Snapshot *s = ctx;
    if (a->accepted) return;
    s->compared++;
    bool same = memcmp(&s->t, &(double){tint_time(ti)}, sizeof(double)) == 0 &&
                memcmp(s->T, tint_temperature(ti), (size_t)s->nn * sizeof(double)) == 0 &&
                memcmp(&s->budget, tint_budget(ti), sizeof s->budget) == 0;
    /* the schedule sets the source for the attempted interval; it must be a pure function of that interval, so the
     * model arrays equal the snapshot again as soon as the schedule is evaluated for the accepted interval */
    if (!same) s->mismatches++;
}

/* ================================================================================================================ */

int main(void) {
    char err[1024];

    printf("== A1 event schedule\n");
    {
        EventSchedule ev;
        events_init(&ev, 0, 10);
        events_add(&ev, 3.0, EVENT_OUTPUT);
        events_add(&ev, 3.0 + 1e-15, EVENT_LOAD); /* coincident within the tolerance: merged */
        events_add(&ev, 7.25, EVENT_LOAD);
        events_add(&ev, -1, EVENT_OUTPUT);        /* before the start: ignored */
        events_add(&ev, 11, EVENT_OUTPUT);        /* after the end: ignored */
        events_finalize(&ev);
        CHECK(ev.n == 3, "three events after merging (got %d)", ev.n);
        CHECK(ev.n == 3 && ev.ev[0].kinds == (EVENT_OUTPUT | EVENT_LOAD) && ev.ev[2].kinds == EVENT_END && ev.ev[2].t == 10,
              "coincident kinds are merged and the end event is exact");
        int e;
        bool lim;
        double dt = events_limit_step(&ev, 0, 5, &e, &lim);
        CHECK(dt == 3 && e == 0 && lim, "a step past an event lands on it (dt %g, event %d)", dt, e);
        dt = events_limit_step(&ev, 3, 4.0, &e, &lim);
        CHECK(e == 1 && fabs(dt - 4.25) < 1e-15 && !lim, "a sliver of 0.25 s after a 4 s step is absorbed by a <=10 %% stretch (dt %g)", dt);
        dt = events_limit_step(&ev, 3, 3.5, &e, &lim);
        CHECK(e == -1 && fabs(dt - 2.125) < 1e-15 && lim, "a gap of 1.21 steps becomes two equal steps (dt %g)", dt);
        dt = events_limit_step(&ev, 3, 1.0, &e, &lim);
        CHECK(e == -1 && dt == 1.0 && !lim, "a step well short of the event is untouched");
        CHECK(events_next(&ev, 3.0 + 1e-15) == 1, "an event within the tolerance of the current time counts as reached");
        events_free(&ev);
    }

    printf("== A2 step-size controller\n");
    {
        StepControllerSettings cs = {.dt_initial = 1, .dt_min = 1e-3, .dt_max = 100, .order = 2};
        CHECK(stepctl_defaults(&cs, 1000, err, sizeof err), "defaults: %s", err);
        StepController c;
        CHECK(stepctl_init(&c, &cs, err, sizeof err), "init: %s", err);
        StepDecision d = stepctl_error(&c, 1, 0.25, false);
        CHECK(d == STEP_ACCEPT && fabs(c.dt - 0.9 * pow(0.25, -0.5)) < 1e-12, "first acceptance uses the elementary controller (dt %g)", c.dt);
        d = stepctl_error(&c, c.dt, 1e-12, false);
        CHECK(d == STEP_ACCEPT && c.last_factor <= cs.max_growth + 1e-12, "growth is capped at %g (factor %g)", cs.max_growth, c.last_factor);
        double before = c.dt;
        d = stepctl_error(&c, before, 5.0, false);
        CHECK(d == STEP_RETRY && c.dt < before, "a rejection shrinks the step (%g -> %g)", before, c.dt);
        d = stepctl_error(&c, c.dt, 1e-6, false);
        CHECK(d == STEP_ACCEPT && c.last_factor <= 1 + 1e-15, "the first acceptance after a rejection does not grow (factor %g)", c.last_factor);
        StepController m;
        stepctl_init(&m, &cs, err, sizeof err);
        d = stepctl_error(&m, cs.dt_min, 2.0, false);
        CHECK(d == STEP_FAIL_MIN_STEP, "a rejected step already at dt_min fails (%s)", step_decision_name(d));
        StepControllerSettings cs2 = cs;
        cs2.max_rejections = 3;
        StepController r;
        stepctl_init(&r, &cs2, err, sizeof err);
        int k = 0;
        do d = stepctl_error(&r, r.dt, 1.01, false), k++;
        while (d == STEP_RETRY && k < 50);
        CHECK(d == STEP_FAIL_REJECTIONS && k == 3, "three consecutive rejections give up (%s after %d)", step_decision_name(d), k);
        StepController f;
        stepctl_init(&f, &cs, err, sizeof err);
        d = stepctl_solver_failure(&f, 1.0);
        CHECK(d == STEP_RETRY && fabs(f.dt - 0.25) < 1e-15, "a solver failure shrinks by the solver factor (dt %g)", f.dt);
        StepControllerSettings bad = {.dt_initial = 1e-5, .dt_min = 1e-3, .order = 2};
        CHECK(!stepctl_defaults(&bad, 10, err, sizeof err) && strstr(err, "initial_time_step"), "an initial step below the minimum is rejected: %s", err);
    }

    printf("== A3 estimator order on a smooth problem\n");
    {
        /* from the lowest eigenmode the exact semi-discrete solution is T_ref + a exp(-lambda t) at every node */
        for (int pass = 0; pass < 2; pass++) {
            double theta = pass ? 0.5 : 1.0;
            Problem p;
            problem_sine(&p);
            ThermalOptions o = {.theta = theta, .pcg_tol = 1e-14};
            ThermalSolver *s = thermal_create(&p.m, &o, err, sizeof err);
            int nn = p.b.nn, mid = nid(&p.b, 10, 0, 0);
            double *Tf = malloc((size_t)nn * sizeof(double)), *Th = malloc((size_t)nn * sizeof(double)), *T2 = malloc((size_t)nn * sizeof(double));
            ThermalStepStats st;
            /* the discrete eigenvalue from one tiny backward-Euler-like probe is not needed: use a very small CN step
             * pair to get lambda to high accuracy */
            ThermalOptions oc = {.theta = 0.5, .pcg_tol = 1e-15};
            ThermalSolver *sc = thermal_create(&p.m, &oc, err, sizeof err);
            double hp = 1e-3;
            thermal_step(sc, &p.m, p.Tinit, Tf, 0, hp, &st, err, sizeof err);
            double g = (Tf[mid] - 300) / (p.Tinit[mid] - 300); /* CN amplification (1 - x/2)/(1 + x/2) */
            double lambda = 2 * (1 - g) / ((1 + g) * hp);
            thermal_free(sc);
            ThermalTimeTolerance tol = {.relative = 0, .temperature = 1, .temperature_only = true};
            double est[4], tru[4];
            double factor = pass ? 1.0 / 3 : 1.0;
            for (int i = 0; i < 4; i++) {
                double dt = 20.0 / (1 << i);
                thermal_step(s, &p.m, p.Tinit, Tf, 0, dt, &st, err, sizeof err);
                thermal_step(s, &p.m, p.Tinit, Th, 0, dt / 2, &st, err, sizeof err);
                thermal_step(s, &p.m, Th, T2, dt / 2, dt / 2, &st, err, sizeof err);
                ThermalErrorNorm norm;
                thermal_error_norm(&p.m, p.Tinit, Tf, T2, factor, 300, &tol, &norm);
                est[i] = norm.max_dT;
                double exact = 300 + (p.Tinit[mid] - 300) * exp(-lambda * dt);
                tru[i] = fabs(T2[mid] - exact);
            }
            double r1 = est[0] / est[1], r2 = est[1] / est[2], r3 = est[2] / est[3];
            printf("  %s: estimate %.3e %.3e %.3e %.3e K for dt 20..2.5 s; halving ratios %.3f %.3f %.3f; true error %.3e K at 2.5 s "
                   "(estimate/true %.3f)\n",
                   pass ? "Crank-Nicolson" : "backward Euler", est[0], est[1], est[2], est[3], r1, r2, r3, tru[3], est[3] / tru[3]);
            if (!pass) {
                CHECK(r2 > 3.5 && r2 < 4.5 && r3 > 3.5 && r3 < 4.5, "backward Euler estimate scales as dt^2 (ratios %.3f, %.3f)", r2, r3);
                CHECK(est[3] / tru[3] > 0.5 && est[3] / tru[3] < 2, "the estimate is within a factor 2 of the true local error (%.3f)", est[3] / tru[3]);
            } else {
                CHECK(r2 > 7 && r2 < 9 && r3 > 7 && r3 < 9, "Crank-Nicolson estimate scales as dt^3 (ratios %.3f, %.3f)", r2, r3);
            }
            free(Tf), free(Th), free(T2);
            thermal_free(s);
            problem_free(&p);
        }
    }

    /* reference for the sudden-heating problem: Richardson extrapolation of two fine backward-Euler runs */
    Problem ph;
    problem_heat(&ph);
    int nnh = ph.b.nn;
    /* three fine runs: extrapolating the pairs (0.04, 0.02) and (0.02, 0.01) gives two second-order references whose
     * difference, divided by 3, estimates the error of the finer one */
    Run rf0, rf1, rf2;
    ThermalIntegratorSettings fs = fixed_settings(0.04);
    run_problem(&ph, &fs, 1.0, 1e-12, &rf0);
    fs.dt_fixed = 0.02;
    run_problem(&ph, &fs, 1.0, 1e-12, &rf1);
    fs.dt_fixed = 0.01;
    run_problem(&ph, &fs, 1.0, 1e-12, &rf2);
    double *ref = malloc((size_t)ph.nout * (size_t)nnh * sizeof(double));
    double ref_unc = 0;
    for (size_t i = 0; i < (size_t)ph.nout * (size_t)nnh; i++) {
        double coarse = 2 * rf1.frames[i] - rf0.frames[i];
        ref[i] = 2 * rf2.frames[i] - rf1.frames[i];
        ref_unc = fmax(ref_unc, fabs(ref[i] - coarse) / 3);
    }
    printf("  reference: %d, %d and %d backward Euler steps, Richardson extrapolated; estimated reference error %.2e K\n", rf0.accepted_steps,
           rf1.accepted_steps, rf2.accepted_steps, ref_unc);
    CHECK(rf0.status == TINT_STEPPED && rf1.status == TINT_STEPPED && rf2.status == TINT_STEPPED && rf2.frames_stored == ph.nout,
          "reference runs stored every output time (%d of %d): %s", rf2.frames_stored, ph.nout, rf2.err);

    printf("== A4 tolerance convergence on sudden surface heating\n");
    double tols[4] = {1e-2, 1e-3, 1e-4, 1e-5}, errs[4];
    Run ra[4];
    for (int i = 0; i < 4; i++) {
        ThermalIntegratorSettings as = adaptive_settings(tols[i], 100 * tols[i], 1e-3);
        run_problem(&ph, &as, 1.0, 1e-12, &ra[i]);
        errs[i] = frame_error(&ph, &ra[i], ref);
        printf("  tolerance %.0e (abs %.0e K): %4d accepted, %3ld rejected, %5ld solves, error %.3e K, closure %.1e, %.3f s\n", tols[i], 100 * tols[i],
               ra[i].accepted_steps, ra[i].work.rejected, ra[i].work.solves, errs[i], budget_closure(&ra[i].budget), ra[i].seconds);
        printf("      error at t =");
        for (int f = 0; f < ph.nout; f++) {
            double e = 0;
            for (int n = 0; n < nnh; n++) e = fmax(e, fabs(ra[i].frames[(size_t)f * (size_t)nnh + (size_t)n] - ref[(size_t)f * (size_t)nnh + (size_t)n]));
            printf(" %g s: %.2e K%s", ph.tout[f], e, f + 1 < ph.nout ? "," : "");
        }
        printf("; largest accepted local estimate %.2f, sum %.1f\n", ra[i].work.error_max, ra[i].work.error_sum);
        CHECK(ra[i].status == TINT_STEPPED && ra[i].frames_stored == ph.nout, "adaptive run at %.0e finished with every frame: %s", tols[i], ra[i].err);
    }
    bool mono = errs[0] > errs[1] && errs[1] > errs[2] && errs[2] > errs[3];
    CHECK(mono, "error decreases monotonically with the tolerance (%.2e %.2e %.2e %.2e)", errs[0], errs[1], errs[2], errs[3]);
    CHECK(errs[0] / errs[3] >= 10, "three decades of tolerance reduce the error at least tenfold (%.1fx)", errs[0] / errs[3]);
    CHECK(errs[3] > 5 * ref_unc, "the tightest error (%.2e K) is at least 5x the reference error (%.2e K), so the comparison is meaningful", errs[3], ref_unc);

    printf("== A11 work at equal accuracy: adaptive against fixed steps\n");
    {
        enum { NF = 11 };
        double dts[NF] = {50, 20, 10, 5, 2, 1, 0.5, 0.2, 0.1, 0.05, 0.04};
        Run rx[NF];
        double ex[NF];
        for (int i = 0; i < NF; i++) {
            ThermalIntegratorSettings f2 = fixed_settings(dts[i]);
            run_problem(&ph, &f2, 1.0, 1e-12, &rx[i]);
            ex[i] = frame_error(&ph, &rx[i], ref);
        }
        printf("  %-26s %9s %7s %8s %8s %9s %8s\n", "run", "error K", "solves", "assemb", "picard", "cg iters", "seconds");
        for (int i = 0; i < 4; i++) {
            printf("  adaptive tol %-13.0e %9.2e %7ld %8ld %8ld %9ld %8.3f\n", tols[i], errs[i], ra[i].work.solves, ra[i].work.assemblies,
                   ra[i].work.picard_iterations, ra[i].work.linear_iterations, ra[i].seconds);
            /* the cheapest fixed step at least as accurate */
            int best = -1;
            for (int j = 0; j < NF; j++)
                if (ex[j] <= errs[i] && (best < 0 || rx[j].work.solves < rx[best].work.solves)) best = j;
            if (best >= 0) {
                printf("  fixed dt %-17g %9.2e %7ld %8ld %8ld %9ld %8.3f   (cheapest fixed step at least as accurate: %.0fx the solves)\n", dts[best], ex[best],
                       rx[best].work.solves, rx[best].work.assemblies, rx[best].work.picard_iterations, rx[best].work.linear_iterations, rx[best].seconds,
                       (double)rx[best].work.solves / (double)ra[i].work.solves);
                if (i == 1) CHECK(rx[best].work.solves > ra[i].work.solves, "at tolerance 1e-3 the adaptive run needs fewer solves than the equally accurate fixed step");
            } else {
                printf("  (no fixed step down to %g s is as accurate)\n", dts[NF - 1]);
            }
        }
        printf("  fixed-step errors:");
        for (int j = 0; j < NF; j++) printf(" %g s %.2e%s", dts[j], ex[j], j + 1 < NF ? "," : "\n");
        for (int i = 0; i < NF; i++) run_free(&rx[i]);
    }

    printf("== A8 recovery from an oversized initial step\n");
    {
        ThermalIntegratorSettings big = adaptive_settings(1e-3, 0.1, ph.t_end);
        Run rb;
        run_problem(&ph, &big, 1.0, 1e-12, &rb);
        double eb = frame_error(&ph, &rb, ref);
        printf("  first step %.0f s: %ld rejected, %d accepted, error %.3e K (from a 1e-3 s start: %.3e K)\n", ph.t_end, rb.work.rejected, rb.accepted_steps, eb,
               errs[1]);
        CHECK(rb.status == TINT_STEPPED && rb.work.rejected >= 1, "the oversized first step is rejected and the run finishes (%ld rejections): %s",
              rb.work.rejected, rb.err);
        CHECK(eb <= 3 * errs[1], "its accuracy is within a factor 3 of a sensible start (%.2e vs %.2e K)", eb, errs[1]);
        run_free(&rb);
    }

    printf("== A9 minimum step too large for the tolerance\n");
    {
        ThermalIntegratorSettings tight = adaptive_settings(1e-5, 1e-3, 100);
        tight.control.dt_min = 100;
        Run rm;
        run_problem(&ph, &tight, 1.0, 1e-12, &rm);
        printf("  %s: %s\n", tint_status_name(rm.status), rm.err);
        CHECK(rm.status == TINT_FAIL_MIN_STEP, "fails with MIN_STEP (%s)", tint_status_name(rm.status));
        CHECK(strstr(rm.err, "t = ") && strstr(rm.err, "normalised error"), "the message names the time and the error measure");
        bool finite = true;
        for (int n = 0; n < nnh; n++) finite &= isfinite(rm.T_final[n]);
        CHECK(finite && rm.t_final < ph.t_end && budget_closure(&rm.budget) < 1e-9, "the accepted state stays finite and balanced (t = %g s, closure %.1e)",
              rm.t_final, budget_closure(&rm.budget));
        run_free(&rm);
    }
    for (int i = 0; i < 4; i++) run_free(&ra[i]);
    run_free(&rf0), run_free(&rf1), run_free(&rf2);
    free(ref);
    problem_free(&ph);

    printf("== A6 discontinuous heating lands on the switch times\n");
    for (int pass = 0; pass < 2; pass++) {
        Problem p;
        problem_base(&p, 2, 0.02, 0.02, 15, 8000, 500, 300);
        for (int e = 0; e < p.b.ne; e++) p.src_mask[e] = 1;
        box_free(&p.b);
        box_init(&p.b, 2, 2, 2, 0.02, 0.02, 0.02);
        free(p.fixed), free(p.fixT), free(p.src), free(p.Tinit), free(p.src_mask);
        p.fixed = calloc((size_t)p.b.nn, 1), p.fixT = calloc((size_t)p.b.nn, sizeof(double)), p.src = calloc((size_t)p.b.ne, sizeof(double));
        p.src_mask = malloc((size_t)p.b.ne), p.Tinit = malloc((size_t)p.b.nn * sizeof(double));
        memset(p.src_mask, 1, (size_t)p.b.ne);
        for (int n = 0; n < p.b.nn; n++) p.Tinit[n] = 300;
        p.m = (ThermalModel){.nnodes = p.b.nn, .nelems = p.b.ne, .xyz = p.b.xyz, .conn = p.b.conn, .nmat = 1, .mat = &p.mat, .fixed = p.fixed,
                             .fixed_T = p.fixT, .elem_source = p.src};
        p.sched = true, p.q_on = 1.37, p.q_off = 4.21, p.Q = 1e6, p.t_end = 10;
        ThermalIntegratorSettings set = pass ? adaptive_settings(1e-3, 1e-2, 0.5) : fixed_settings(1.0);
        Run r;
        run_problem(&p, &set, 1.0, 1e-12, &r);
        double V = 0.02 * 0.02 * 0.02, E = p.Q * V * (p.q_off - p.q_on), Texact = 300 + p.Q * (p.q_off - p.q_on) / (8000.0 * 500);
        bool landed_on = false, landed_off = false, straddle = false;
        for (int i = 0; i < r.nint; i++) {
            double a = r.iv[2 * i], b = r.iv[2 * i + 1];
            landed_on |= b == p.q_on, landed_off |= b == p.q_off;
            straddle |= (a < p.q_on && b > p.q_on) || (a < p.q_off && b > p.q_off);
        }
        double Tmax = 0, Tmin = INFINITY;
        for (int n = 0; n < p.b.nn; n++) Tmax = fmax(Tmax, r.T_final[n]), Tmin = fmin(Tmin, r.T_final[n]);
        printf("  %s: %d steps, deposited %.15g J (exact %.15g J), final %.12f K (exact %.12f K)\n", pass ? "adaptive" : "fixed 1 s", r.accepted_steps,
               r.budget.source, E, Tmax, Texact);
        CHECK(r.status == TINT_STEPPED && landed_on && landed_off && !straddle, "%s steps land on 1.37 s and 4.21 s and never straddle them",
              pass ? "adaptive" : "fixed");
        CHECK(fabs(r.budget.source - E) <= 1e-12 * E, "%s deposited energy exact to 1e-12 (%.3e relative)", pass ? "adaptive" : "fixed",
              fabs(r.budget.source - E) / E);
        CHECK(fabs(Tmax - Texact) < 1e-9 && fabs(Tmin - Texact) < 1e-9, "%s final temperature exact to 1e-9 K", pass ? "adaptive" : "fixed");
        run_free(&r);
        problem_free(&p);
    }

    printf("== A7 rejections do not enter the accepted budget\n");
    {
        Problem p;
        problem_source(&p);
        ThermalIntegratorSettings set = adaptive_settings(1e-4, 1e-3, p.t_end); /* oversized start: forces rejections */
        Run r;
        run_problem(&p, &set, 1.0, 1e-12, &r);
        double Vsrc = 10 * (0.1 / 20) * 0.005 * 0.005, E = 2e6 * Vsrc * p.t_end;
        printf("  %d accepted, %ld rejected: source %.15g J (exact %.15g J), closure %.2e, enthalpy mismatch %.2e\n", r.accepted_steps, r.work.rejected,
               r.budget.source, E, budget_closure(&r.budget), budget_mismatch(&r.budget));
        CHECK(r.status == TINT_STEPPED && r.work.rejected >= 1, "rejections occurred (%ld): %s", r.work.rejected, r.err);
        CHECK(fabs(r.budget.source - E) <= 1e-12 * E, "accepted source energy is exact to 1e-12 (%.3e relative)", fabs(r.budget.source - E) / E);
        CHECK(budget_closure(&r.budget) < 1e-9 && budget_mismatch(&r.budget) < 1e-9, "the accepted budget closes and matches the enthalpy");
        run_free(&r);
        problem_free(&p);
    }

    printf("== A10 accepted state is untouched by a rejected trial\n");
    {
        /* the Stefan problem rejects often: every rejected trial is compared bit for bit with the state before the step,
         * including the model arrays the integrator must never write */
        Problem p;
        problem_stefan(&p, 200);
        int nn = p.b.nn, ne = p.b.ne;
        ThermalOptions o = {.theta = 1, .picard_tol = 1e-10, .max_picard = 400, .pcg_tol = 1e-13};
        ThermalSolver *s = thermal_create(&p.m, &o, err, sizeof err);
        Snapshot snap = {.nn = nn, .ne = ne, .T = malloc((size_t)nn * sizeof(double)), .src = malloc((size_t)ne * sizeof(double)), .p = &p};
        unsigned char *fixed0 = malloc((size_t)nn);
        double *fixT0 = malloc((size_t)nn * sizeof(double));
        memcpy(fixed0, p.fixed, (size_t)nn), memcpy(fixT0, p.fixT, (size_t)nn * sizeof(double));
        ThermalIntegratorSettings set = adaptive_settings(1e-3, 1e-2, 1e-3);
        set.t_end = p.t_end;
        set.on_attempt = on_attempt_compare, set.attempt_ctx = &snap;
        EventSchedule ev = problem_events(&p);
        ThermalIntegrator *ti = tint_create(s, &p.m, &set, &ev, p.Tinit, err, sizeof err);
        CHECK(ti != NULL, "integrator: %s", err);
        ThermalStepReport rep;
        int steps = 0, steps_after_rejection = 0, replays = 0, replay_ok = 0, model_changed = 0;
        double *pre_T = malloc((size_t)nn * sizeof(double)), *post_T = malloc((size_t)nn * sizeof(double));
        ThermalIntegratorState pre;
        ThermalIntegratorStatus stt = TINT_FINISHED;
        if (ti) snap_take(&snap, ti, &p);
        while (ti) {
            tint_get_state(ti, &pre); /* the state before this step, for the replay */
            memcpy(pre_T, tint_temperature(ti), (size_t)nn * sizeof(double));
            long rej0 = tint_work(ti)->rejected;
            stt = tint_step(ti, &rep, err, sizeof err); /* every rejected trial inside is compared with snap */
            if (stt != TINT_STEPPED) break;
            steps++;
            if (memcmp(fixed0, p.fixed, (size_t)nn) || memcmp(fixT0, p.fixT, (size_t)nn * sizeof(double))) model_changed++;
            if (tint_work(ti)->rejected > rej0) {
                steps_after_rejection++;
                if (replays < 8) {
                    /* replay the same step from the saved state in a fresh solver and integrator */
                    replays++;
                    memcpy(post_T, tint_temperature(ti), (size_t)nn * sizeof(double));
                    ThermalBudget post_b = *tint_budget(ti);
                    ThermalSolver *s2 = thermal_create(&p.m, &o, err, sizeof err);
                    ThermalIntegratorSettings set2 = set;
                    set2.on_attempt = NULL;
                    ThermalIntegrator *t2 = tint_create(s2, &p.m, &set2, &ev, p.Tinit, err, sizeof err);
                    bool ok = t2 && tint_set_state(t2, &pre, pre_T, err, sizeof err);
                    ThermalStepReport rep2;
                    ok = ok && tint_step(t2, &rep2, err, sizeof err) == TINT_STEPPED;
                    if (ok && memcmp(post_T, tint_temperature(t2), (size_t)nn * sizeof(double)) == 0 && memcmp(&post_b, tint_budget(t2), sizeof post_b) == 0 &&
                        tint_time(t2) == tint_time(ti))
                        replay_ok++;
                    tint_free(t2), thermal_free(s2);
                }
            }
            snap_take(&snap, ti, &p); /* the accepted state the next step's rejected trials must leave untouched */
        }
        printf("  %d accepted steps, %d after rejections; %d rejected trials compared, %d differed; %d replays, %d bit-identical\n", steps,
               steps_after_rejection, snap.compared, snap.mismatches, replays, replay_ok);
        CHECK(stt == TINT_STEPPED || stt == TINT_FINISHED, "the run finished: %s", err);
        CHECK(snap.compared >= 20 && snap.mismatches == 0, "every rejected trial left time, temperature and budget bit-identical (%d of %d)",
              snap.compared - snap.mismatches, snap.compared);
        CHECK(model_changed == 0, "the integrator never wrote the model's prescribed-temperature arrays");
        CHECK(replays >= 3 && replay_ok == replays, "fresh integrators restored from the pre-step state reproduce %d of %d rejected-then-accepted steps bit for bit",
              replay_ok, replays);
        free(pre_T), free(post_T), free(snap.T), free(snap.src), free(fixed0), free(fixT0);
        events_free(&ev);
        tint_free(ti), thermal_free(s);
        problem_free(&p);
    }

    printf("== A5 phase change against a refined reference\n");
    {
        Problem p;
        problem_stefan(&p, 1000);
        Run r0, r1, r2;
        ThermalIntegratorSettings f0 = fixed_settings(1.0), f1 = fixed_settings(0.5), f2 = fixed_settings(0.25);
        run_problem(&p, &f0, 1.0, 1e-10, &r0);
        run_problem(&p, &f1, 1.0, 1e-10, &r1);
        run_problem(&p, &f2, 1.0, 1e-10, &r2);
        double s0 = front_position(&p, r0.T_final), s1 = front_position(&p, r1.T_final), s2 = front_position(&p, r2.T_final);
        double v0 = r0.liquid_final, v1 = r1.liquid_final, v2 = r2.liquid_final;
        double sref = 2 * s2 - s1, vref = 2 * v2 - v1;
        /* the error of the extrapolated reference, from the difference with the coarser extrapolation */
        double sunc = fabs(sref - (2 * s1 - s0)) / 3, vunc = fabs(vref - (2 * v1 - v0)) / 3;
        printf("  reference (backward Euler 1 / 0.5 / 0.25 s, extrapolated): front %.5f mm (error ~%.1e mm, %.1e %%), molten %.6e m^3 (error ~%.1e, "
               "%.1e %%)\n",
               1e3 * sref, 1e3 * sunc, 100 * sunc / sref, vref, vunc, 100 * vunc / vref);
        CHECK(r0.status == TINT_STEPPED && r1.status == TINT_STEPPED && r2.status == TINT_STEPPED, "reference runs finished: %s %s", r1.err, r2.err);
        Run rh, rt;
        ThermalIntegratorSettings ah = adaptive_settings(1e-3, 1e-2, 1e-3), at = ah;
        at.tol.temperature_only = true;
        run_problem(&p, &ah, 1.0, 1e-10, &rh);
        run_problem(&p, &at, 1.0, 1e-10, &rt);
        double sh = front_position(&p, rh.T_final), st2 = front_position(&p, rt.T_final);
        double Lvol = 1000 * 1e5; /* latent energy per molten cubic metre */
        double eh = fabs(rh.liquid_final - vref) * Lvol, et = fabs(rt.liquid_final - vref) * Lvol;
        printf("  temperature + enthalpy control: front %.5f mm (%+.3f %%), molten %+.3f %%, latent error %.4g J, %d accepted / %ld rejected, %ld solves\n",
               1e3 * sh, 100 * (sh - sref) / sref, 100 * (rh.liquid_final - vref) / vref, eh, rh.accepted_steps, rh.work.rejected, rh.work.solves);
        printf("  temperature-only control:       front %.5f mm (%+.3f %%), molten %+.3f %%, latent error %.4g J, %d accepted / %ld rejected, %ld solves\n",
               1e3 * st2, 100 * (st2 - sref) / sref, 100 * (rt.liquid_final - vref) / vref, et, rt.accepted_steps, rt.work.rejected, rt.work.solves);
        CHECK(rh.status == TINT_STEPPED && rt.status == TINT_STEPPED, "adaptive phase-change runs finished: %s %s", rh.err, rt.err);
        CHECK(fabs(sh - sref) <= 0.02 * sref, "front within 2 %% of the reference (%.3f %%)", 100 * fabs(sh - sref) / sref);
        CHECK(fabs(rh.liquid_final - vref) <= 0.02 * vref, "molten volume within 2 %% of the reference (%.3f %%)", 100 * fabs(rh.liquid_final - vref) / vref);
        CHECK(et >= eh, "temperature-only control is not more accurate in latent energy (%.4g J vs %.4g J)", et, eh);
        printf("  resolution: the temperature + enthalpy molten-volume error is %.1fx the reference error, temperature-only %.1fx\n",
               fabs(rh.liquid_final - vref) / vunc, fabs(rt.liquid_final - vref) / vunc);
        CHECK(budget_mismatch(&rh.budget) < 1e-9 && budget_closure(&rh.budget) < 1e-8, "adaptive melting conserves energy (closure %.1e, mismatch %.1e)",
              budget_closure(&rh.budget), budget_mismatch(&rh.budget));
        run_free(&r0), run_free(&r1), run_free(&r2), run_free(&rh), run_free(&rt);
        problem_free(&p);
    }

    printf("\n%s: %d passed, %d failed\n", g_fail ? "TIME INTEGRATION TESTS FAILED" : "ALL TIME INTEGRATION TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
