/* orchtest.c - verification of the shared orchestrator (src/fem/orchestrator.c) with thermal participants
 *
 * Criteria were fixed before the first run: "Thermal Sim/verification/stageD-criteria.md" (D1..D5; D6 is the job path,
 * covered by the MCP suites).
 *   make build/orchtest && ./build/orchtest */
#include "../src/fem/orchestrator.h"
#include "../src/fem/thermal.h"
#include "../src/fem/thermal_integrator.h"
#include "../src/fem/thermal_participant.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

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

/* ---- bar meshes: nx elements along x, 1 x 1 across --------------------------------------------------------------- */

typedef struct {
    int nx, nn, ne;
    double *xyz;
    int *conn;
} Bar;

static int bnid(int nx, int i, int j, int k) { return i + (nx + 1) * (j + 2 * k); }

static void bar_init(Bar *b, int nx, double x0, double L, double W) {
    b->nx = nx, b->nn = (nx + 1) * 4, b->ne = nx;
    b->xyz = malloc(3 * (size_t)b->nn * sizeof(double));
    b->conn = malloc(8 * (size_t)b->ne * sizeof(int));
    for (int k = 0; k <= 1; k++)
        for (int j = 0; j <= 1; j++)
            for (int i = 0; i <= nx; i++) {
                double *x = b->xyz + 3 * (size_t)bnid(nx, i, j, k);
                x[0] = x0 + L * i / nx, x[1] = W * j, x[2] = W * k;
            }
    for (int i = 0; i < nx; i++) {
        int *c = b->conn + 8 * (size_t)i;
        c[0] = bnid(nx, i, 0, 0), c[1] = bnid(nx, i + 1, 0, 0), c[2] = bnid(nx, i + 1, 1, 0), c[3] = bnid(nx, i, 1, 0);
        c[4] = bnid(nx, i, 0, 1), c[5] = bnid(nx, i + 1, 0, 1), c[6] = bnid(nx, i + 1, 1, 1), c[7] = bnid(nx, i, 1, 1);
    }
}

static void bar_free(Bar *b) { free(b->xyz), free(b->conn); }

typedef struct {
    double kA, kB, rcA, rcB; /* conductivities and rho cp */
} Materials;

typedef struct {
    double tt[1], k[2], rho[2], cp[1];
    ThermalMaterial mat[2];
} MatSet;

static void matset(MatSet *s, const Materials *m) {
    memset(s, 0, sizeof *s);
    s->tt[0] = 300, s->k[0] = m->kA, s->k[1] = m->kB, s->rho[0] = m->rcA, s->rho[1] = m->rcB, s->cp[0] = 1;
    for (int i = 0; i < 2; i++) {
        s->mat[i].k = (ThermalTable){1, s->tt, s->k + i};
        s->mat[i].rho = (ThermalTable){1, s->tt, s->rho + i};
        s->mat[i].cp = (ThermalTable){1, s->tt, s->cp};
    }
}

enum { NX = 20, NOUT = 10 };
static const double L = 0.1, W = 0.005, T_HOT = 400, T_AMB = 300, H_CONV = 200, T_END = 2000;

static void add_events(EventSchedule *ev) {
    events_init(ev, 0, T_END);
    for (int i = 1; i <= NOUT; i++) events_add(ev, T_END * i / NOUT, EVENT_OUTPUT);
    events_finalize(ev);
}

static ThermalIntegratorSettings integ_settings(bool adaptive, double dt, double rel, double atol, double dt0) {
    ThermalIntegratorSettings s = {0};
    s.mode = adaptive ? THERMAL_STEPPING_ADAPTIVE : THERMAL_STEPPING_FIXED;
    s.t_start = 0, s.t_end = T_END, s.dt_fixed = dt;
    s.tol.relative = rel, s.tol.temperature = atol;
    s.control.dt_initial = dt0;
    s.T_reference = T_AMB;
    return s;
}

static OrchSettings orch_settings(bool adaptive, double dt, double dt0) {
    OrchSettings o = {0};
    o.mode = adaptive ? THERMAL_STEPPING_ADAPTIVE : THERMAL_STEPPING_FIXED;
    o.t_start = 0, o.t_end = T_END, o.dt_fixed = dt;
    o.control.dt_initial = dt0;
    o.coupling_relative = 1e-12;
    o.max_coupling_iterations = 60;
    o.aitken = true;
    o.relaxation = 0.5;
    return o;
}

/* frames: NOUT stored fields of the full bar (monolithic node numbering) */
typedef struct {
    double frames[NOUT][(NX + 1) * 4];
    int stored;
    long frame_step[NOUT]; /* accepted step after which each frame was stored */
    double *t1;            /* partitioned runs: end time of every accepted step */
    long steps, coupling_rejections, coupling_iterations, error_rejections;
    ThermalWork work; /* single-model runs: the integrator's bookkeeping */
    ThermalAttempt recent[THERMAL_ATTEMPT_RING];
    int nrecent;
    double worst_heat_mismatch, coupling_residual_max, error_max;
    int desync;
    char err[1024];
    bool ok;
} Result;

/* ---- monolithic ---------------------------------------------------------------------------------------------------- */

static void run_monolithic(const Materials *mt, bool adaptive, double dt, double rel, double atol, double dt0, bool through_orch, Result *r) {
    memset(r, 0, sizeof *r);
    Bar b;
    bar_init(&b, NX, 0, L, W);
    MatSet ms;
    matset(&ms, mt);
    int emat[NX];
    for (int e = 0; e < NX; e++) emat[e] = e < NX / 2 ? 0 : 1;
    unsigned char fixed[(NX + 1) * 4] = {0};
    double fixT[(NX + 1) * 4] = {0}, T0[(NX + 1) * 4];
    for (int n = 0; n < b.nn; n++) T0[n] = T_AMB;
    for (int j = 0; j <= 1; j++)
        for (int k = 0; k <= 1; k++) fixed[bnid(NX, 0, j, k)] = 1, fixT[bnid(NX, 0, j, k)] = T_HOT, T0[bnid(NX, 0, j, k)] = T_HOT;
    ThermalFace face = {NX - 1, 3, THERMAL_CONVECTION, H_CONV, T_AMB};
    ThermalModel m = {.nnodes = b.nn, .nelems = b.ne, .xyz = b.xyz, .conn = b.conn, .elem_mat = emat, .nmat = 2, .mat = ms.mat, .fixed = fixed,
                      .fixed_T = fixT, .nfaces = 1, .faces = &face};
    ThermalOptions opt = {.theta = 1, .picard_tol = 1e-12, .pcg_tol = 1e-14};
    ThermalSolver *s = thermal_create(&m, &opt, r->err, sizeof r->err);
    EventSchedule ev;
    add_events(&ev);
    ThermalIntegratorSettings is = integ_settings(adaptive, dt, rel, atol, dt0);
    ThermalIntegrator *ti = s ? tint_create(s, &m, &is, &ev, T0, r->err, sizeof r->err) : NULL;
    r->ok = ti != NULL;
    if (!through_orch) {
        ThermalStepReport rep;
        ThermalIntegratorStatus st;
        while (r->ok && (st = tint_step(ti, &rep, r->err, sizeof r->err)) == TINT_STEPPED) {
            r->steps++;
            if ((rep.event_kinds & EVENT_OUTPUT) && r->stored < NOUT) memcpy(r->frames[r->stored++], tint_temperature(ti), (size_t)b.nn * sizeof(double));
        }
        r->ok = r->ok && st == TINT_FINISHED;
    } else if (r->ok) {
        ThermalParticipant tp = {.name = "bar", .ti = ti, .m = &m, .fixed = fixed, .fixed_T = fixT, .estimate = adaptive};
        OrchParticipant op;
        r->ok = thermal_participant_bind(&tp, &op, r->err, sizeof r->err);
        OrchSettings os = orch_settings(adaptive, dt, dt0);
        Orchestrator *o = orch_create(&os, &ev, &op, 1, NULL, 0, r->err, sizeof r->err);
        OrchStepReport rep;
        OrchStatus st = ORCH_FINISHED;
        r->ok = o && orch_initialize(o, r->err, sizeof r->err);
        while (r->ok && (st = orch_step(o, &rep, r->err, sizeof r->err)) == ORCH_STEPPED) {
            r->steps++;
            if ((rep.event_kinds & EVENT_OUTPUT) && r->stored < NOUT) memcpy(r->frames[r->stored++], tint_temperature(ti), (size_t)b.nn * sizeof(double));
        }
        r->ok = r->ok && st == ORCH_FINISHED;
        orch_free(o);
        thermal_participant_release(&tp);
    }
    if (ti) {
        r->work = *tint_work(ti);
        r->nrecent = tint_recent_attempts(ti, r->recent, THERMAL_ATTEMPT_RING);
    }
    events_free(&ev);
    tint_free(ti), thermal_free(s);
    bar_free(&b);
}

/* ---- partitioned: B holds the interface at A's temperature and returns the heat that left it; A receives that heat -- */

typedef struct {
    ThermalParticipant *A, *B;
    Result *r;
} Watch;

static bool g_force_estimate; /* participants estimate even when the orchestrator steps at fixed sizes */

static void run_partitioned(const Materials *mt, bool adaptive, double dt, double rel, double atol, double dt0, const OrchSettings *override, Result *r) {
    memset(r, 0, sizeof *r);
    int half = NX / 2;
    Bar ba, bb;
    bar_init(&ba, half, 0, L / 2, W);
    bar_init(&bb, half, L / 2, L / 2, W);
    MatSet ms;
    matset(&ms, mt);
    int ea[NX] = {0}, eb[NX];
    for (int e = 0; e < half; e++) eb[e] = 1;
    int nn = ba.nn;
    unsigned char fa[64] = {0}, fb[64] = {0};
    double Ta[64] = {0}, Tb[64] = {0}, T0a[64], T0b[64], pa[64] = {0}, pb[64] = {0};
    int ia[4], ib[4], q = 0;
    for (int k = 0; k <= 1; k++)
        for (int j = 0; j <= 1; j++) ia[q] = bnid(half, half, j, k), ib[q] = bnid(half, 0, j, k), q++;
    for (int n = 0; n < nn; n++) T0a[n] = T0b[n] = T_AMB;
    for (int j = 0; j <= 1; j++)
        for (int k = 0; k <= 1; k++) fa[bnid(half, 0, j, k)] = 1, Ta[bnid(half, 0, j, k)] = T_HOT, T0a[bnid(half, 0, j, k)] = T_HOT;
    for (int i = 0; i < 4; i++) fb[ib[i]] = 1, Tb[ib[i]] = T_AMB; /* held by the imported interface temperature */
    ThermalFace face = {half - 1, 3, THERMAL_CONVECTION, H_CONV, T_AMB};
    ThermalModel ma = {.nnodes = nn, .nelems = half, .xyz = ba.xyz, .conn = ba.conn, .elem_mat = ea, .nmat = 2, .mat = ms.mat, .fixed = fa, .fixed_T = Ta,
                       .node_power = pa};
    ThermalModel mb = {.nnodes = nn, .nelems = half, .xyz = bb.xyz, .conn = bb.conn, .elem_mat = eb, .nmat = 2, .mat = ms.mat, .fixed = fb, .fixed_T = Tb,
                       .nfaces = 1, .faces = &face, .node_power = pb};
    ThermalOptions opt = {.theta = 1, .picard_tol = 1e-12, .pcg_tol = 1e-14};
    ThermalSolver *sa = thermal_create(&ma, &opt, r->err, sizeof r->err), *sb = thermal_create(&mb, &opt, r->err, sizeof r->err);
    EventSchedule ev;
    add_events(&ev);
    ThermalIntegratorSettings is = integ_settings(adaptive, dt, rel, atol, dt0);
    ThermalIntegrator *ta = sa ? tint_create(sa, &ma, &is, &ev, T0a, r->err, sizeof r->err) : NULL;
    ThermalIntegrator *tb = sb ? tint_create(sb, &mb, &is, &ev, T0b, r->err, sizeof r->err) : NULL;
    ThermalParticipant A = {.name = "A", .ti = ta, .m = &ma, .fixed = fa, .fixed_T = Ta, .node_power = pa, .ninterface = 4, .interface_nodes = ia,
                            .estimate = adaptive || g_force_estimate};
    ThermalParticipant B = {.name = "B", .ti = tb, .m = &mb, .fixed = fb, .fixed_T = Tb, .node_power = pb, .ninterface = 4, .interface_nodes = ib,
                            .estimate = adaptive || g_force_estimate};
    OrchParticipant parts[2];
    r->ok = ta && tb && thermal_participant_bind(&B, &parts[0], r->err, sizeof r->err) && thermal_participant_bind(&A, &parts[1], r->err, sizeof r->err);
    OrchCoupling coup[2] = {{1, 0, "interface_temperature", "interface_temperature", false}, {0, 1, "interface_heat_out", "interface_heat_in", true}};
    OrchSettings os = override ? *override : orch_settings(adaptive, dt, dt0);
    Orchestrator *o = r->ok ? orch_create(&os, &ev, parts, 2, coup, 2, r->err, sizeof r->err) : NULL;
    r->ok = o && orch_initialize(o, r->err, sizeof r->err);
    OrchStepReport rep;
    OrchStatus st = ORCH_FINISHED;
    while (r->ok && (st = orch_step(o, &rep, r->err, sizeof r->err)) == ORCH_STEPPED) {
        r->steps++;
        double *grown = realloc(r->t1, (size_t)r->steps * sizeof(double));
        if (!grown) {
            r->ok = false;
            break;
        }
        r->t1 = grown, r->t1[r->steps - 1] = rep.t1;
        r->coupling_iterations += rep.coupling_iterations;
        if (tint_time(ta) != orch_time(o) || tint_time(tb) != orch_time(o)) r->desync++;
        /* conservation of the transfer, per solve of the committed trials: what left B is what A received. Blocks of a trial
         * that did not estimate are derived from its single solve, not solved, and carry no separate balance. */
        for (int b = 0; b < (B.trial.estimated ? A.blocks : 1); b++) {
            const double *eB = tint_trial_stage_node_energy(tb, b);
            double out = 0, in = 0;
            for (int i = 0; i < 4; i++) out += eB ? -eB[i] : 0, in += A.heat_in[b * 4 + i];
            double big = fmax(fabs(out), fabs(in));
            if (!eB) r->worst_heat_mismatch = INFINITY;
            else if (big > 0) r->worst_heat_mismatch = fmax(r->worst_heat_mismatch, fabs(out - in) / big);
        }
        if ((rep.event_kinds & EVENT_OUTPUT) && r->stored < NOUT) {
            const double *TA = tint_temperature(ta), *TB = tint_temperature(tb);
            for (int k = 0; k <= 1; k++)
                for (int j = 0; j <= 1; j++)
                    for (int i = 0; i <= NX; i++) {
                        double v = i <= half ? TA[bnid(half, i, j, k)] : TB[bnid(half, i - half, j, k)];
                        r->frames[r->stored][bnid(NX, i, j, k)] = v;
                    }
            r->frame_step[r->stored++] = r->steps;
        }
    }
    if (o) {
        const OrchWork *w = orch_work(o);
        r->coupling_rejections = w->rejected_coupling, r->error_rejections = w->rejected_error;
        r->coupling_residual_max = w->coupling_residual_max, r->error_max = w->error_max;
    }
    r->ok = r->ok && st == ORCH_FINISHED;
    orch_free(o);
    thermal_participant_release(&A), thermal_participant_release(&B);
    events_free(&ev);
    tint_free(ta), tint_free(tb), thermal_free(sa), thermal_free(sb);
    bar_free(&ba), bar_free(&bb);
}

/* the monolithic model advanced over exactly the accepted steps of a partitioned run, with the same candidate
 * (step doubling), stored after the same steps: if rejected attempts leave no trace, the two agree to the coupling
 * tolerance */
static void run_replay(const Materials *mt, const Result *part, Result *r) {
    memset(r, 0, sizeof *r);
    Bar b;
    bar_init(&b, NX, 0, L, W);
    MatSet ms;
    matset(&ms, mt);
    int emat[NX];
    for (int e = 0; e < NX; e++) emat[e] = e < NX / 2 ? 0 : 1;
    unsigned char fixed[(NX + 1) * 4] = {0};
    double fixT[(NX + 1) * 4] = {0}, T0[(NX + 1) * 4];
    for (int n = 0; n < b.nn; n++) T0[n] = T_AMB;
    for (int j = 0; j <= 1; j++)
        for (int k = 0; k <= 1; k++) fixed[bnid(NX, 0, j, k)] = 1, fixT[bnid(NX, 0, j, k)] = T_HOT, T0[bnid(NX, 0, j, k)] = T_HOT;
    ThermalFace face = {NX - 1, 3, THERMAL_CONVECTION, H_CONV, T_AMB};
    ThermalModel m = {.nnodes = b.nn, .nelems = b.ne, .xyz = b.xyz, .conn = b.conn, .elem_mat = emat, .nmat = 2, .mat = ms.mat, .fixed = fixed,
                      .fixed_T = fixT, .nfaces = 1, .faces = &face};
    ThermalOptions opt = {.theta = 1, .picard_tol = 1e-12, .pcg_tol = 1e-14};
    ThermalSolver *s = thermal_create(&m, &opt, r->err, sizeof r->err);
    EventSchedule ev;
    add_events(&ev);
    ThermalIntegratorSettings is = integ_settings(true, 0, 1e-4, 1e-3, 1);
    ThermalIntegrator *ti = s ? tint_create(s, &m, &is, &ev, T0, r->err, sizeof r->err) : NULL;
    r->ok = ti != NULL;
    ThermalTrial tr;
    for (long k = 0; r->ok && k < part->steps; k++) {
        r->ok = tint_trial(ti, part->t1[k], true, &tr);
        if (!r->ok) {
            snprintf(r->err, sizeof r->err, "%s", tr.err);
            break;
        }
        tint_commit_trial(ti, &tr);
        r->steps++;
        if (r->stored < part->stored && part->frame_step[r->stored] == r->steps) memcpy(r->frames[r->stored++], tint_temperature(ti), (size_t)b.nn * sizeof(double));
    }
    events_free(&ev);
    tint_free(ti), thermal_free(s);
    bar_free(&b);
}

static double max_diff(const Result *a, const Result *b) {
    double d = 0;
    int n = a->stored < b->stored ? a->stored : b->stored;
    for (int f = 0; f < n; f++)
        for (int i = 0; i < (NX + 1) * 4; i++) d = fmax(d, fabs(a->frames[f][i] - b->frames[f][i]));
    return a->stored == b->stored ? d : INFINITY;
}

/* ---- mocks for the graph and synchronisation tests ----------------------------------------------------------------- */

typedef struct {
    double value;
    int evaluations, trials;
    double last_eval_t;
} Mock;

static bool mock_trial(void *self, double t0, double t1, bool est, double *err, OrchFailureClass *f, char *e, size_t el) {
    Mock *m = self;
    m->trials++;
    *err = -1;
    (void)t0, (void)t1, (void)est, (void)f, (void)e, (void)el;
    return true;
}
static void mock_commit(void *self) { (void)self; }
static bool mock_eval(void *self, double t, char *e, size_t el) {
    Mock *m = self;
    m->evaluations++;
    m->last_eval_t = t;
    (void)e, (void)el;
    return true;
}
static int mock_size(void *self, const char *f) {
    (void)self, (void)f;
    return 1;
}
static bool mock_export(void *self, const char *f, bool trial, double *out) {
    (void)f, (void)trial;
    out[0] = ((Mock *)self)->value;
    return true;
}
static bool mock_import(void *self, const char *f, const double *in) {
    (void)f;
    ((Mock *)self)->value = in[0];
    return true;
}

static OrchParticipant mock_part(const char *name, Mock *m, bool transient, bool history) {
    OrchParticipant p = {.name = name, .self = m, .transient = transient, .history_dependent = history, .order = 1};
    if (transient) p.trial = mock_trial, p.commit = mock_commit;
    else p.evaluate = mock_eval;
    p.field_size = mock_size, p.export_field = mock_export, p.import_field = mock_import;
    return p;
}

int main(void) {
    char err[1024];
    Materials m1 = {40, 8, 4e6, 4e6};

    printf("== D6 (core) a single participant through the orchestrator equals tint_step bit for bit\n");
    for (int mode = 0; mode < 2; mode++) {
        bool adaptive = mode == 1;
        Result a, b;
        /* overhead of the orchestrator on a single participant: best of 5 of each */
        double ta = INFINITY, tb = INFINITY;
        for (int rep = 0; rep < 5; rep++) {
            double w0 = now_s();
            run_monolithic(&m1, adaptive, 20, 1e-4, 1e-3, 1, false, &a);
            double w1 = now_s();
            run_monolithic(&m1, adaptive, 20, 1e-4, 1e-3, 1, true, &b);
            double w2 = now_s();
            ta = fmin(ta, w1 - w0), tb = fmin(tb, w2 - w1);
        }
        printf("  %s: %ld steps, %ld solves; tint_step %.4f s, orchestrated %.4f s (best of 5)\n", adaptive ? "adaptive" : "fixed", a.steps, a.work.solves, ta, tb);
        bool same = a.ok && b.ok && a.stored == b.stored && a.steps == b.steps && memcmp(a.frames, b.frames, sizeof a.frames) == 0;
        CHECK(same, "%s: orchestrated single-participant run identical to tint_step (%ld vs %ld steps): %s %s", adaptive ? "adaptive" : "fixed", a.steps,
              b.steps, a.err, b.err);
        /* the integrator's own bookkeeping is the same whichever driver decided about its trials */
        bool books = a.work.attempts == b.work.attempts && a.work.accepted == b.work.accepted && a.work.rejected == b.work.rejected &&
                     a.work.solves == b.work.solves && a.work.error_max == b.work.error_max && a.nrecent == b.nrecent;
        for (int i = 0; books && i < a.nrecent; i++)
            books = a.recent[i].t0 == b.recent[i].t0 && a.recent[i].t1 == b.recent[i].t1 && a.recent[i].accepted == b.recent[i].accepted &&
                    a.recent[i].decision == b.recent[i].decision && a.recent[i].error == b.recent[i].error && a.recent[i].picard == b.recent[i].picard;
        CHECK(books, "%s: identical attempt counts (%ld / %ld), rejections (%ld / %ld) and recent-attempt records", adaptive ? "adaptive" : "fixed",
              a.work.attempts, b.work.attempts, a.work.rejected, b.work.rejected);
    }

    printf("== D1 fixed-step partitioned against monolithic\n");
    {
        Result mono, part;
        double w0 = now_s();
        run_monolithic(&m1, false, 20, 0, 0, 0, false, &mono);
        double w1 = now_s();
        run_partitioned(&m1, false, 20, 0, 0, 0, NULL, &part);
        double w2 = now_s();
        double d = max_diff(&mono, &part);
        printf("  %ld steps, %ld coupling iterations (%.2f per step); max difference %.3e K; worst heat mismatch %.2e\n", part.steps, part.coupling_iterations,
               (double)part.coupling_iterations / (double)(part.steps ? part.steps : 1), d, part.worst_heat_mismatch);
        printf("  cost: monolithic %.3f s, partitioned %.3f s\n", w1 - w0, w2 - w1);
        CHECK(mono.ok && part.ok, "both runs finished: %s %s", mono.err, part.err);
        CHECK(d <= 1e-6, "partitioned equals monolithic within 1e-6 K at every stored time (%.3e K)", d);
        CHECK(part.worst_heat_mismatch <= 1e-12, "heat leaving B equals heat entering A (%.2e)", part.worst_heat_mismatch);
        CHECK(part.desync == 0, "both participants always committed at the orchestrator's time");
        free(part.t1);
        /* regression check added with the stage-resolved fields: participants that estimate, driven at fixed steps, use the
         * three-block layout with blocks derived from their single solve */
        Result part3;
        g_force_estimate = true;
        run_partitioned(&m1, false, 20, 0, 0, 0, NULL, &part3);
        g_force_estimate = false;
        double d3 = max_diff(&mono, &part3);
        printf("  three-block fields at fixed steps: %ld coupling iterations, max difference %.3e K, heat mismatch %.2e\n", part3.coupling_iterations, d3,
               part3.worst_heat_mismatch);
        CHECK(part3.ok && d3 <= 1e-6 && part3.worst_heat_mismatch <= 1e-12, "estimating participants at fixed steps give the same answer (%.3e K)", d3);
        free(part3.t1);
    }

    printf("== D2 adaptive partitioned against adaptive monolithic\n");
    {
        Result ref, mono, part;
        run_monolithic(&m1, false, 0.25, 0, 0, 0, false, &ref);
        double w0 = now_s();
        run_monolithic(&m1, true, 0, 1e-4, 1e-3, 1, false, &mono);
        double w1 = now_s();
        run_partitioned(&m1, true, 0, 1e-4, 1e-3, 1, NULL, &part);
        double w2 = now_s();
        printf("  cost: monolithic adaptive %.3f s (%ld solves), partitioned adaptive %.3f s\n", w1 - w0, mono.work.solves, w2 - w1);
        double em = max_diff(&mono, &ref), ep = max_diff(&part, &ref);
        printf("  reference 8000 fixed steps; monolithic %ld steps, error %.3e K; partitioned %ld steps (%ld coupling iterations), error %.3e K\n", mono.steps,
               em, part.steps, part.coupling_iterations, ep);
        printf("  partitioned: %ld error rejections, %ld coupling rejections; largest accepted temporal error %.3f, largest final coupling change %.3g "
               "(both normalised by their own tolerances)\n",
               part.error_rejections, part.coupling_rejections, part.error_max, part.coupling_residual_max);
        CHECK(ref.ok && mono.ok && part.ok, "runs finished: %s %s %s", ref.err, mono.err, part.err);
        CHECK(ep <= 3 * em && em <= 3 * ep, "errors within a factor 3 of each other (%.3e vs %.3e K)", ep, em);
        CHECK(part.worst_heat_mismatch <= 1e-12 && part.desync == 0, "conservative transfer and synchronous commits (%.2e, %d)", part.worst_heat_mismatch,
              part.desync);
        CHECK(part.coupling_iterations > 0 && part.error_max > 0 && part.error_max <= 1 && part.coupling_residual_max <= 1,
              "coupling iterations, coupling change and temporal error are reported separately");
        /* regression guard added after the first run: stage-resolved coupling data reproduces the monolithic estimate */
        CHECK(part.steps == mono.steps, "the partitioned run takes the monolithic step sequence (%ld vs %ld steps)", part.steps, mono.steps);
        free(part.t1);
    }

    printf("== D3 rollback after coupling failures\n");
    {
        /* plain Dirichlet-Neumann (no relaxation) diverges when the Neumann side is the more resistive one and the step is
         * long, and converges for short steps where the capacities dominate: the controller must shrink after a coupling
         * failure and still produce the monolithic answer */
        Materials m3 = {4, 40, 4e6, 1e6};
        OrchSettings os = orch_settings(true, 0, 500);
        os.aitken = false, os.relaxation = 1.0, os.max_coupling_iterations = 40, os.coupling_relative = 1e-10;
        Result ref, part;
        run_monolithic(&m3, false, 0.25, 0, 0, 0, false, &ref);
        run_partitioned(&m3, true, 0, 1e-4, 1e-3, 500, &os, &part);
        Result mono, replay;
        run_monolithic(&m3, true, 0, 1e-4, 1e-3, 500, false, &mono);
        run_replay(&m3, &part, &replay);
        double ep = max_diff(&part, &ref), em = max_diff(&mono, &ref), er = max_diff(&part, &replay);
        printf("  %ld accepted steps after %ld coupling rejections and %ld error rejections; error %.3e K (monolithic adaptive %ld steps, %.3e K)\n",
               part.steps, part.coupling_rejections, part.error_rejections, ep, mono.steps, em);
        printf("  monolithic replay of the accepted steps: max difference %.3e K\n", er);
        CHECK(part.ok && replay.ok, "the partitioned run and its replay finished: %s %s", part.err, replay.err);
        CHECK(part.coupling_rejections >= 1, "at least one attempt was rejected for coupling (%ld)", part.coupling_rejections);
        CHECK(part.desync == 0, "no participant committed at a different time from the orchestrator");
        /* amended criterion (stageD-criteria.md, Amendments): the literal two-sided factor also bounded how much MORE
         * accurate the run may be, which coupling-limited steps make it */
        CHECK(ep <= 3 * em, "the committed solution is not less accurate than the monolithic adaptive run by more than a factor 3 (%.3e vs %.3e K)", ep, em);
        CHECK(er <= 1e-6, "rejected attempts left no trace: the monolithic replay of the accepted steps agrees within 1e-6 K (%.3e K)", er);
        printf("  (original two-sided D3 criterion, recorded: %s)\n", ep <= 3 * em && em <= 3 * ep ? "met" : "not met");
        free(part.t1);
    }

    printf("== D4 graph analysis\n");
    {
        Mock a = {0}, b = {0};
        EventSchedule ev;
        add_events(&ev);
        OrchSettings os = orch_settings(false, 100, 0);
        OrchParticipant chain[2] = {mock_part("a", &a, true, false), mock_part("b", &b, true, false)};
        OrchCoupling one = {0, 1, "x", "x", false};
        Orchestrator *o = orch_create(&os, &ev, chain, 2, &one, 1, err, sizeof err);
        JsonValue *d = o ? orch_describe(o) : NULL;
        CHECK(d && strstr(json_get_str(d, "character", ""), "one-way"), "a chain is described as one-way");
        json_free(d), orch_free(o);
        OrchCoupling loop[2] = {{0, 1, "x", "x", false}, {1, 0, "x", "x", false}};
        o = orch_create(&os, &ev, chain, 2, loop, 2, err, sizeof err);
        d = o ? orch_describe(o) : NULL;
        CHECK(d && strstr(json_get_str(d, "character", ""), "feedback"), "a loop is described as a strongly coupled feedback cycle");
        json_free(d), orch_free(o);
        OrchParticipant mixed[2] = {mock_part("a", &a, true, false), mock_part("q", &b, false, false)};
        o = orch_create(&os, &ev, mixed, 2, loop, 2, err, sizeof err);
        CHECK(!o && strstr(err, "quasi-static"), "a quasi-static participant in a cycle is refused: %s", err);
        orch_free(o);
        events_free(&ev);
    }

    printf("== D5 synchronisation of quasi-static participants\n");
    {
        Mock drv = {0}, plain = {0}, hist = {0};
        EventSchedule ev;
        add_events(&ev);
        OrchSettings os = orch_settings(false, 30, 0); /* 30 s steps; the 10 stored times every 200 s are off the grid and add steps */
        OrchParticipant parts[3] = {mock_part("driver", &drv, true, false), mock_part("elastic", &plain, false, false), mock_part("plastic", &hist, false, true)};
        OrchCoupling cp[2] = {{0, 1, "x", "x", false}, {0, 2, "x", "x", false}};
        Orchestrator *o = orch_create(&os, &ev, parts, 3, cp, 2, err, sizeof err);
        bool ok = o && orch_initialize(o, err, sizeof err);
        OrchStepReport rep;
        long steps = 0;
        while (ok && orch_step(o, &rep, err, sizeof err) == ORCH_STEPPED) steps++;
        printf("  %ld steps: elastic evaluated %d times, history-dependent %d times\n", steps, plain.evaluations, hist.evaluations);
        CHECK(plain.evaluations == 1 + NOUT, "without history: the initial time and the %d stored times (%d)", NOUT, plain.evaluations);
        CHECK(hist.evaluations == 1 + steps, "with history: the initial time and every accepted step (%d of %ld)", hist.evaluations, 1 + steps);
        orch_free(o);
        events_free(&ev);
    }

    printf("\n%s: %d passed, %d failed\n", g_fail ? "ORCHESTRATOR TESTS FAILED" : "ALL ORCHESTRATOR TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
