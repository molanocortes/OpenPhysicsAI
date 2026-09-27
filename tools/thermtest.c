/* thermtest.c - verification of the transient heat conduction solver (src/fem/thermal.c) against analytical solutions:
 *   steady conduction with a heat flux, temperature-dependent conductivity (Kirchhoff transform) and its O(h^2)
 *   convergence, semi-infinite solid after a surface temperature step (erf) with the absorbed heat, lumped-capacitance
 *   cooling and its time-step convergence (backward Euler first order, Crank-Nicolson second order), radiation
 *   equilibrium, a fin with lateral convection, energy balance of a moving source (constant and temperature-dependent
 *   specific heat), element activation, table integrals, invalid input, and the time of a 100k-element step.
 *   make build/thermtest && ./build/thermtest */
#include "../src/fem/hex8.h"
#include "../src/fem/solid.h"
#include "../src/fem/thermal.h"
#include "../src/threads.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (cond) g_pass++;                                                                                           \
        else {                                                                                                        \
            g_fail++;                                                                                                 \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                             \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

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

/* faces of one box side; side numbers equal hex8 local faces: 0 z-, 1 z+, 2 y-, 3 x+, 4 y+, 5 x- */
static int box_faces(const Box *b, int side, unsigned char kind, double value, double ambient, ThermalFace *out, int n) {
    for (int k = 0; k < b->nz; k++)
        for (int j = 0; j < b->ny; j++)
            for (int i = 0; i < b->nx; i++) {
                bool on = side == 0 ? k == 0 : side == 1 ? k == b->nz - 1 : side == 2 ? j == 0 : side == 3 ? i == b->nx - 1 : side == 4 ? j == b->ny - 1 : i == 0;
                if (on) out[n++] = (ThermalFace){eid(b, i, j, k), (unsigned char)side, kind, value, ambient};
            }
    return n;
}

typedef struct {
    double t[4], k[4], cp[4], rho[4];
    ThermalMaterial m;
} Mat;

static void mat_const(Mat *s, double k, double rho, double cp) {
    memset(s, 0, sizeof *s); /* ThermalMaterial has optional fields (anisotropy, latent heat): they must start at zero */
    s->t[0] = 293.15, s->k[0] = k, s->rho[0] = rho, s->cp[0] = cp;
    s->m.k = (ThermalTable){1, s->t, s->k};
    s->m.rho = (ThermalTable){1, s->t, s->rho};
    s->m.cp = (ThermalTable){1, s->t, s->cp};
}

typedef struct {
    unsigned char *fixed;
    double *fixT, *T0, *T1;
} Fields;

static void fields_init(Fields *f, int nn, double T) {
    f->fixed = calloc((size_t)nn, 1);
    f->fixT = calloc((size_t)nn, sizeof(double));
    f->T0 = malloc((size_t)nn * sizeof(double));
    f->T1 = malloc((size_t)nn * sizeof(double));
    for (int i = 0; i < nn; i++) f->T0[i] = f->T1[i] = T;
}

static void fields_free(Fields *f) { free(f->fixed), free(f->fixT), free(f->T0), free(f->T1); }

static void swap(Fields *f) {
    double *t = f->T0;
    f->T0 = f->T1, f->T1 = t;
}

/* steady bar with a Kirchhoff-transform solution for k = 10 (1 + 0.005 (T - 300)); returns the largest nodal error */
static double kirchhoff_error(int nx, int *picard) {
    Box b;
    box_init(&b, nx, 1, 1, 0.1, 0.01, 0.01);
    static double tt[2] = {300, 500}, kk[2] = {10, 20}, one[1] = {1}, rho[1] = {8000}, cp[1] = {500};
    ThermalMaterial mt = {{2, tt, kk}, {1, one, cp}, {1, one, rho}};
    Fields f;
    fields_init(&f, b.nn, 400);
    for (int j = 0; j <= 1; j++)
        for (int k = 0; k <= 1; k++) {
            f.fixed[nid(&b, 0, j, k)] = f.fixed[nid(&b, nx, j, k)] = 1;
            f.fixT[nid(&b, 0, j, k)] = 500, f.fixT[nid(&b, nx, j, k)] = 300;
        }
    ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt, NULL, f.fixed, f.fixT, NULL, 0, NULL, NULL};
    char err[256];
    ThermalSolver *s = thermal_create(&m, NULL, err, sizeof err);
    ThermalStepStats st;
    double worst = INFINITY;
    if (s && thermal_step(s, &m, f.T0, f.T1, 0, 0, &st, err, sizeof err)) {
        worst = 0;
        for (int n = 0; n < b.nn; n++) {
            double x = b.xyz[3 * (size_t)n], theta = 3000 * (1 - x / 0.1), u = (-1 + sqrt(1 + 2 * 0.005 * theta / 10)) / 0.005;
            worst = fmax(worst, fabs(f.T1[n] - (300 + u)));
        }
        *picard = st.picard_iterations;
    } else {
        printf("  (kirchhoff run failed: %s)\n", err);
    }
    thermal_free(s);
    fields_free(&f);
    box_free(&b);
    return worst;
}

/* lumped-capacitance cooling of a copper cube; returns the error of the mean temperature at t = 200 s */
static double cooling_error(double theta, double dt, double *spread, double *balance) {
    Box b;
    box_init(&b, 4, 4, 4, 0.01, 0.01, 0.01);
    Mat mt;
    mat_const(&mt, 400, 8900, 385);
    ThermalFace faces[600];
    int nf = 0;
    for (int side = 0; side < 6; side++) nf = box_faces(&b, side, THERMAL_CONVECTION, 50, 300, faces, nf);
    Fields f;
    fields_init(&f, b.nn, 500);
    ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt.m, NULL, NULL, NULL, NULL, nf, faces, NULL};
    ThermalOptions o = {.theta = theta, .pool = NULL};
    char err[256];
    ThermalSolver *s = thermal_create(&m, &o, err, sizeof err);
    int steps = (int)lround(200 / dt);
    double stored = 0, flux = 0;
    *balance = 0;
    for (int i = 0; s && i < steps; i++) {
        ThermalStepStats st;
        if (!thermal_step(s, &m, f.T0, f.T1, i * dt, dt, &st, err, sizeof err)) {
            printf("  (cooling step failed: %s)\n", err);
            break;
        }
        *balance = fmax(*balance, st.balance_error);
        stored += st.stored_energy, flux += st.boundary_energy;
        swap(&f);
    }
    double tau = 8900 * 385 * 1e-6 / (50 * 6e-4), mean = 300 + (stored + 8900 * 385 * 1e-6 * 200) / (8900 * 385 * 1e-6);
    double lo = INFINITY, hi = -INFINITY;
    for (int n = 0; n < b.nn; n++) lo = fmin(lo, f.T0[n]), hi = fmax(hi, f.T0[n]);
    *spread = hi - lo;
    (void)flux;
    thermal_free(s);
    fields_free(&f);
    box_free(&b);
    return mean - (300 + 200 * exp(-200 / tau));
}

/* smooth data: decaying sine mode between fixed ends, T = 300 + 100 exp(-alpha pi^2 t / L^2) sin(pi x / L), evaluated at
 * t = 1 s with Crank-Nicolson and a small time step, so the spatial error dominates */
static double sine_error(int nx) {
    Box b;
    box_init(&b, nx, 1, 1, 0.01, 0.001, 0.001);
    Mat mt;
    mat_const(&mt, 15, 8000, 500);
    Fields f;
    fields_init(&f, b.nn, 300);
    for (int n = 0; n < b.nn; n++) {
        double x = b.xyz[3 * (size_t)n];
        f.T0[n] = 300 + 100 * sin(M_PI * x / 0.01);
        if (x < 1e-12 || x > 0.01 - 1e-12) f.fixed[n] = 1, f.fixT[n] = 300, f.T0[n] = 300;
    }
    ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt.m, NULL, f.fixed, f.fixT, NULL, 0, NULL, NULL};
    ThermalOptions o = {.theta = 0.5, .pool = NULL};
    char err[256];
    ThermalSolver *s = thermal_create(&m, &o, err, sizeof err);
    for (int i = 0; s && i < 1000; i++) {
        ThermalStepStats st;
        if (!thermal_step(s, &m, f.T0, f.T1, i * 0.001, 0.001, &st, err, sizeof err)) break;
        swap(&f);
    }
    double worst = 0, alpha = 15.0 / (8000 * 500), amp = 100 * exp(-alpha * M_PI * M_PI * 1.0 / (0.01 * 0.01));
    for (int n = 0; n < b.nn; n++) worst = fmax(worst, fabs(f.T0[n] - (300 + amp * sin(M_PI * b.xyz[3 * (size_t)n] / 0.01))));
    thermal_free(s);
    fields_free(&f);
    box_free(&b);
    return worst;
}

int main(void) {
    char err[512];

    printf("== tables\n");
    {
        double t[2] = {300, 400}, v[2] = {1, 3};
        ThermalTable tb = {2, t, v};
        CHECK(fabs(thermal_table_integral(&tb, 250, 450) - 400) < 1e-9, "integral across and beyond the table: %g", thermal_table_integral(&tb, 250, 450));
        CHECK(fabs(thermal_table_integral(&tb, 450, 250) + 400) < 1e-9, "reversed bounds change the sign");
        CHECK(fabs(thermal_table_integral(&tb, 320, 380) - 120) < 1e-9, "integral inside one segment");
        CHECK(fabs(thermal_table_eval(&tb, 350) - 2) < 1e-12 && thermal_table_eval(&tb, 1000) == 3, "interpolation and constant extrapolation");
    }

    printf("== steady conduction with a heat flux\n");
    {
        Box b;
        box_init(&b, 20, 2, 2, 0.1, 0.01, 0.01);
        Mat mt;
        mat_const(&mt, 15, 8000, 500);
        Fields f;
        fields_init(&f, b.nn, 300);
        for (int j = 0; j <= 2; j++)
            for (int k = 0; k <= 2; k++) f.fixed[nid(&b, 20, j, k)] = 1, f.fixT[nid(&b, 20, j, k)] = 300;
        ThermalFace faces[16];
        int nf = box_faces(&b, 5, THERMAL_FLUX, 15000, 0, faces, 0);
        ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt.m, NULL, f.fixed, f.fixT, NULL, nf, faces, NULL};
        ThermalSolver *s = thermal_create(&m, NULL, err, sizeof err);
        CHECK(s != NULL, "create: %s", err);
        ThermalStepStats st;
        bool ok = s && thermal_step(s, &m, f.T0, f.T1, 0, 0, &st, err, sizeof err);
        CHECK(ok, "steady solve: %s", err);
        double worst = 0;
        for (int n = 0; ok && n < b.nn; n++) worst = fmax(worst, fabs(f.T1[n] - (300 + 15000 * (0.1 - b.xyz[3 * (size_t)n]) / 15)));
        CHECK(ok && worst < 1e-6, "linear profile reproduced exactly (largest error %.3g K)", worst);
        CHECK(ok && fabs(st.boundary_energy - 1.5) < 1e-9 && fabs(st.prescribed_energy + 1.5) < 1e-6 && st.balance_error < 1e-8,
              "1.5 W enter through the flux face and leave through the cold end (%.9g, %.9g W)", st.boundary_energy, st.prescribed_energy);
        thermal_free(s);
        fields_free(&f);
        box_free(&b);
    }

    printf("== temperature-dependent conductivity\n");
    {
        int it20 = 0, it40 = 0;
        double e20 = kirchhoff_error(20, &it20), e40 = kirchhoff_error(40, &it40);
        /* an element-mean conductivity integrates a linear k(T) exactly across each element, so 1D nodal values are exact */
        CHECK(e20 < 1e-6 && e40 < 1e-6 && it20 > 2 && it40 > 2, "k(T) steady bar matches the Kirchhoff solution at the nodes (%.2g / %.2g K, %d / %d iterations)",
              e20, e40, it20, it40);
    }

    printf("== semi-infinite solid after a surface temperature step\n");
    for (int pass = 0; pass < 2; pass++) {
        double theta = pass ? 0.5 : 1.0;
        Box b;
        box_init(&b, 100, 1, 1, 0.05, 0.002, 0.002);
        Mat mt;
        mat_const(&mt, 15, 8000, 500);
        Fields f;
        fields_init(&f, b.nn, 300);
        for (int j = 0; j <= 1; j++)
            for (int k = 0; k <= 1; k++) f.fixed[nid(&b, 0, j, k)] = 1, f.fixT[nid(&b, 0, j, k)] = 400;
        ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt.m, NULL, f.fixed, f.fixT, NULL, 0, NULL, NULL};
        ThermalOptions o = {.theta = theta, .pool = NULL};
        ThermalSolver *s = thermal_create(&m, &o, err, sizeof err);
        double dt = 0.05, absorbed = 0, balance = 0, alpha = 15.0 / (8000 * 500);
        for (int i = 0; s && i < 400; i++) {
            ThermalStepStats st;
            if (!thermal_step(s, &m, f.T0, f.T1, i * dt, dt, &st, err, sizeof err)) {
                printf("  (step failed: %s)\n", err);
                break;
            }
            absorbed += st.prescribed_energy;
            balance = fmax(balance, st.balance_error);
            swap(&f);
        }
        double worst = 0, t = 20;
        for (int i = 0; i <= 40; i++) {
            double x = b.xyz[3 * (size_t)nid(&b, i, 0, 0)];
            worst = fmax(worst, fabs(f.T0[nid(&b, i, 0, 0)] - (400 - 100 * erf(x / (2 * sqrt(alpha * t))))));
        }
        double q = 2 * 15 * 100 * sqrt(t / (M_PI * alpha)) * 0.002 * 0.002;
        CHECK(worst < (pass ? 0.3 : 0.6), "%s: temperatures within %.1f K of the erf solution at 20 s (%.3f K)", pass ? "Crank-Nicolson" : "backward Euler",
              pass ? 0.3 : 0.6, worst);
        CHECK(fabs(absorbed - q) / q < 0.03, "%s: absorbed heat %.4f J vs %.4f J", pass ? "Crank-Nicolson" : "backward Euler", absorbed, q);
        CHECK(balance < 1e-9, "energy balance closes every step (worst %.2g)", balance);
        thermal_free(s);
        fields_free(&f);
        box_free(&b);
    }

    printf("== spatial convergence\n");
    {
        double coarse = sine_error(10), fine = sine_error(20);
        CHECK(coarse / fine > 3.5 && coarse / fine < 4.5, "second order in space: sine-mode errors %.4f K (h = 1 mm) and %.4f K (h = 0.5 mm), ratio %.2f", coarse,
              fine, coarse / fine);
    }

    printf("== lumped cooling and time-step convergence\n");
    {
        double sp, bal, bal_max = 0;
        double be10 = cooling_error(1, 10, &sp, &bal);
        bal_max = fmax(bal_max, bal);
        double be5 = cooling_error(1, 5, &sp, &bal);
        bal_max = fmax(bal_max, bal);
        double cn10 = cooling_error(0.5, 10, &sp, &bal);
        bal_max = fmax(bal_max, bal);
        double cn5 = cooling_error(0.5, 5, &sp, &bal);
        bal_max = fmax(bal_max, bal);
        /* reference: Crank-Nicolson with dt = 0.25 s on the same mesh, which removes the small spatial error from the order checks */
        double ref = cooling_error(0.5, 0.25, &sp, &bal);
        bal_max = fmax(bal_max, bal);
        CHECK(fabs(be10 - 2.59) < 0.1 && fabs(be5 - 1.30) < 0.1, "backward Euler errors %.3f K (dt 10 s) and %.3f K (dt 5 s) match the discrete theory", be10, be5);
        CHECK((be10 - ref) / (be5 - ref) > 1.8 && (be10 - ref) / (be5 - ref) < 2.2, "backward Euler is first order in time (ratio %.2f)", (be10 - ref) / (be5 - ref));
        CHECK(fabs(cn10 - ref) < 0.1 && (cn10 - ref) / (cn5 - ref) > 3.5 && (cn10 - ref) / (cn5 - ref) < 4.5,
              "Crank-Nicolson is second order in time (%.5f / %.5f K, ratio %.2f)", cn10 - ref, cn5 - ref, (cn10 - ref) / (cn5 - ref));
        CHECK(fabs(ref) < 0.05, "converged solution within %.4f K of the lumped-capacitance model (Biot number 2e-4)", ref);
        CHECK(sp < 0.05 && bal_max < 1e-9, "small Biot number keeps the cube uniform (%.4f K); balance %.2g", sp, bal_max);
    }

    printf("== radiation equilibrium\n");
    {
        Box b;
        box_init(&b, 2, 2, 2, 0.01, 0.01, 0.01);
        Mat mt;
        mat_const(&mt, 4000, 8900, 385);
        ThermalFace faces[32];
        int nf = 0;
        for (int side = 0; side < 6; side++) nf = box_faces(&b, side, THERMAL_RADIATION, 0.8, 300, faces, nf);
        double src[8];
        for (int e = 0; e < 8; e++) src[e] = 1e7;
        Fields f;
        fields_init(&f, b.nn, 300);
        ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt.m, NULL, NULL, NULL, src, nf, faces, NULL};
        ThermalSolver *s = thermal_create(&m, NULL, err, sizeof err);
        ThermalStepStats st;
        bool ok = s && thermal_step(s, &m, f.T0, f.T1, 0, 0, &st, err, sizeof err);
        double Tref = pow(pow(300, 4) + 1e7 * 1e-6 / (0.8 * THERMAL_SIGMA * 6e-4), 0.25), worst = 0;
        for (int n = 0; ok && n < b.nn; n++) worst = fmax(worst, fabs(f.T1[n] - Tref));
        CHECK(ok && worst < 0.05, "steady radiation temperature %.3f K (analytic %.3f K, %d iterations): %s", ok ? f.T1[0] : 0.0, Tref, st.picard_iterations, err);
        CHECK(ok && fabs(st.source_energy - 10) < 1e-9 && fabs(st.boundary_energy + 10) < 1e-6, "10 W generated and radiated (%.6f W)", st.boundary_energy);
        thermal_free(s);
        fields_free(&f);
        box_free(&b);
    }

    printf("== fin with lateral convection\n");
    {
        Box b;
        box_init(&b, 40, 2, 2, 0.1, 0.005, 0.005);
        Mat mt;
        mat_const(&mt, 200, 2700, 900);
        ThermalFace faces[400];
        int nf = 0;
        nf = box_faces(&b, 0, THERMAL_CONVECTION, 20, 293.15, faces, nf);
        nf = box_faces(&b, 1, THERMAL_CONVECTION, 20, 293.15, faces, nf);
        nf = box_faces(&b, 2, THERMAL_CONVECTION, 20, 293.15, faces, nf);
        nf = box_faces(&b, 4, THERMAL_CONVECTION, 20, 293.15, faces, nf);
        Fields f;
        fields_init(&f, b.nn, 293.15);
        for (int j = 0; j <= 2; j++)
            for (int k = 0; k <= 2; k++) f.fixed[nid(&b, 0, j, k)] = 1, f.fixT[nid(&b, 0, j, k)] = 373.15;
        ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt.m, NULL, f.fixed, f.fixT, NULL, nf, faces, NULL};
        ThermalSolver *s = thermal_create(&m, NULL, err, sizeof err);
        ThermalStepStats st;
        bool ok = s && thermal_step(s, &m, f.T0, f.T1, 0, 0, &st, err, sizeof err);
        double mfin = sqrt(20 * 0.02 / (200 * 2.5e-5)), tip = 293.15 + 80 / cosh(mfin * 0.1), q = sqrt(20 * 0.02 * 200 * 2.5e-5) * 80 * tanh(mfin * 0.1);
        CHECK(ok && fabs(f.T1[nid(&b, 40, 1, 1)] - tip) < 0.3, "tip temperature %.3f K vs fin theory %.3f K", ok ? f.T1[nid(&b, 40, 1, 1)] : 0.0, tip);
        CHECK(ok && fabs(st.prescribed_energy - q) / q < 0.01 && st.balance_error < 1e-8, "base heat flow %.4f W vs %.4f W", st.prescribed_energy, q);
        thermal_free(s);
        fields_free(&f);
        box_free(&b);
    }

    printf("== moving source energy\n");
    for (int pass = 0; pass < 2; pass++) {
        Box b;
        box_init(&b, 40, 20, 8, 0.02, 0.01, 0.004);
        static double tt[2] = {300, 1500}, cpv[2] = {500, 700}, one[1] = {300}, kv[1] = {15}, rv[1] = {7950}, cpc[1] = {500};
        ThermalMaterial mt = {{1, one, kv}, pass ? (ThermalTable){2, tt, cpv} : (ThermalTable){1, one, cpc}, {1, one, rv}};
        double path[8] = {0.002, 0.005, 0.004, 0.0, 0.018, 0.005, 0.004, 0.4};
        ThermalMovingSource src = {50, 1e-3, 3e-4, 2, path};
        Fields f;
        fields_init(&f, b.nn, 300);
        ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt, NULL, NULL, NULL, NULL, 0, NULL, &src};
        ThreadPool *pool = pool_create(4);
        ThermalOptions o = {.theta = 1, .pool = pool};
        ThermalSolver *s = thermal_create(&m, &o, err, sizeof err);
        double stored = 0, enthalpy = 0, input = 0, balance = 0, scale = 0;
        for (int i = 0; s && i < 40; i++) {
            ThermalStepStats st;
            if (!thermal_step(s, &m, f.T0, f.T1, i * 0.01, 0.01, &st, err, sizeof err)) {
                printf("  (source step failed: %s)\n", err);
                break;
            }
            stored += st.stored_energy, enthalpy += st.enthalpy_change, input += st.source_energy;
            balance = fmax(balance, st.balance_error);
            scale = st.source_scale;
            swap(&f);
        }
        int hot = 0;
        for (int n = 1; n < b.nn; n++)
            if (f.T0[n] > f.T0[hot]) hot = n;
        const char *name = pass ? "temperature-dependent cp" : "constant cp";
        CHECK(fabs(input - 20) < 1e-9 && fabs(stored - 20) < 1e-6 && balance < 1e-9, "%s: 20 J delivered and stored (%.9f J, balance %.2g, scale %.3f)", name, stored,
              balance, scale);
        CHECK(fabs(enthalpy - 20) / 20 < (pass ? 0.01 : 1e-9), "%s: physical enthalpy change %.6f J", name, enthalpy);
        CHECK(fabs(b.xyz[3 * (size_t)hot] - 0.018) < 0.003 && fabs(b.xyz[3 * (size_t)hot + 2] - 0.004) < 1e-12 && f.T0[hot] > 400,
              "%s: hottest point %.1f K at x = %.2f mm on the top surface, near the final source position", name, f.T0[hot], 1e3 * b.xyz[3 * (size_t)hot]);
        thermal_free(s);
        pool_destroy(pool);
        fields_free(&f);
        box_free(&b);
    }

    printf("== element activation\n");
    {
        Box b;
        box_init(&b, 10, 1, 1, 0.01, 0.001, 0.001);
        Mat mt;
        mat_const(&mt, 15, 8000, 500);
        unsigned char active[10];
        for (int e = 0; e < 10; e++) active[e] = e < 5;
        Fields f;
        fields_init(&f, b.nn, 300);
        for (int j = 0; j <= 1; j++)
            for (int k = 0; k <= 1; k++) f.fixed[nid(&b, 0, j, k)] = 1, f.fixT[nid(&b, 0, j, k)] = 400;
        ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt.m, active, f.fixed, f.fixT, NULL, 0, NULL, NULL};
        ThermalSolver *s = thermal_create(&m, NULL, err, sizeof err);
        ThermalStepStats st;
        bool ok = s && thermal_step(s, &m, f.T0, f.T1, 0, 1, &st, err, sizeof err);
        bool frozen = true, heated = ok && f.T1[nid(&b, 5, 0, 0)] > 300;
        for (int i = 6; i <= 10; i++) frozen &= f.T1[nid(&b, i, 0, 0)] == 300 && f.T1[nid(&b, i, 1, 1)] == 300;
        CHECK(ok && frozen && heated && st.balance_error < 1e-9, "nodes of inactive elements keep their temperature; the active part heats (balance %.2g)",
              st.balance_error);
        thermal_free(s);
        fields_free(&f);
        box_free(&b);
    }

    printf("== invalid input\n");
    {
        Box b;
        box_init(&b, 2, 1, 1, 0.01, 0.01, 0.01);
        Mat mt;
        mat_const(&mt, 15, 8000, 500);
        Fields f;
        fields_init(&f, b.nn, 300);
        ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt.m, NULL, NULL, NULL, NULL, 0, NULL, NULL};
        ThermalOptions bad = {.theta = 0.3, .pool = NULL};
        ThermalSolver *s = thermal_create(&m, &bad, err, sizeof err);
        CHECK(!s && strstr(err, "theta"), "theta below 0.5 rejected: %s", err);
        s = thermal_create(&m, NULL, err, sizeof err);
        ThermalStepStats st;
        CHECK(s && !thermal_step(s, &m, f.T0, f.T1, 0, 0, &st, err, sizeof err) && strstr(err, "undefined"), "steady state without a heat sink refused: %s", err);
        thermal_free(s);
        int t0 = b.conn[0];
        b.conn[0] = b.conn[4], b.conn[4] = t0;
        s = thermal_create(&m, NULL, err, sizeof err);
        CHECK(!s && strstr(err, "inverted"), "inverted element rejected: %s", err);
        fields_free(&f);
        box_free(&b);
    }

    printf("== thermoelastic coupling\n");
    for (int pass = 0; pass < 2; pass++) {
        /* bar at a uniform 500 K, stress-free at 300 K, expansion coefficient and modulus tabulated in temperature */
        Box b;
        box_init(&b, 10, 2, 2, 0.1, 0.01, 0.01);
        double at[2] = {300, 500}, av[2] = {1e-5, 2e-5}, et[2] = {300, 500}, ev[2] = {200e9, 150e9};
        ThermalTable alpha = {2, at, av}, Et = {2, et, ev};
        double eth = thermal_table_integral(&alpha, 300, 500), Eh = thermal_table_eval(&Et, 500);
        unsigned char *fixed = calloc(3 * (size_t)b.nn, 1);
        for (int n = 0; n < b.nn; n++) {
            const double *x = b.xyz + 3 * (size_t)n;
            if (x[0] == 0 || (pass && fabs(x[0] - 0.1) < 1e-12)) fixed[3 * (size_t)n] = 1;
            if (x[1] == 0) fixed[3 * (size_t)n + 1] = 1;
            if (x[2] == 0) fixed[3 * (size_t)n + 2] = 1;
        }
        double *eps0 = calloc(6 * (size_t)b.ne, sizeof(double)), *scale = malloc((size_t)b.ne * sizeof(double));
        for (int e = 0; e < b.ne; e++) {
            for (int k = 0; k < 3; k++) eps0[6 * (size_t)e + k] = eth;
            scale[e] = Eh / 200e9;
        }
        SolidMaterial mat = {200e9, 0.3, 7850};
        HexModel hm = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mat, HEX8_INCOMPATIBLE, scale};
        SolidLoads L = {fixed, NULL, NULL, eps0, {0, 0, 0}};
        SolidOptions so = {0};
        SolidResult r;
        bool ok = solid_solve(&hm, &L, &so, &r, err, sizeof err);
        CHECK(ok, "thermoelastic solve: %s", err);
        if (ok) {
            double ux = r.u[3 * (size_t)nid(&b, 10, 1, 1)], sxx_err = 0, other = 0, ref = Eh * eth;
            for (int e = 0; e < b.ne; e++)
                for (int g = 0; g < 8; g++) {
                    const double *sg = r.gp_stress + 48 * (size_t)e + 6 * (size_t)g;
                    sxx_err = fmax(sxx_err, fabs(sg[0] - (pass ? -ref : 0)));
                    for (int k = 1; k < 6; k++) other = fmax(other, fabs(sg[k]));
                }
            if (!pass) {
                CHECK(fabs(ux - 0.1 * eth) < 1e-6 * 0.1 * eth, "free expansion: end displacement %.9e m equals L times the integral of alpha (%.9e m)", ux, 0.1 * eth);
                CHECK(sxx_err < 1e-6 * ref && other < 1e-6 * ref, "free expansion is stress-free (largest %.3g Pa)", fmax(sxx_err, other));
            } else {
                CHECK(fabs(ux) < 1e-12 && sxx_err < 1e-6 * ref && other < 1e-6 * ref,
                      "restrained bar: axial stress -E(T) * thermal strain = %.6g MPa everywhere (deviation %.3g Pa, other components %.3g Pa)", -1e-6 * ref, sxx_err,
                      other);
            }
            solid_result_free(&r);
        }
        free(fixed), free(eps0), free(scale);
        box_free(&b);
    }

    printf("== anisotropic conduction\n");
    {
        /* (a) patch test: a linear field solves div(K grad T) = 0 exactly for any constant K, so with every boundary
         * node prescribed the interior must reproduce it to round-off - off-diagonal terms of a rotated tensor
         * included. This is the test that catches a transform applied in the wrong direction. */
        Box b;
        box_init(&b, 4, 3, 3, 0.04, 0.03, 0.03);
        static double one[1] = {1}, k1v[1] = {40}, k2v[1] = {8}, k3v[1] = {2}, rhov[1] = {7800}, cpv[1] = {480};
        double th = M_PI / 4, ct = cos(th), stt = sin(th);
        ThermalMaterial mt = {0};
        mt.k = (ThermalTable){1, one, k1v}, mt.k2 = (ThermalTable){1, one, k2v}, mt.k3 = (ThermalTable){1, one, k3v};
        mt.cp = (ThermalTable){1, one, cpv}, mt.rho = (ThermalTable){1, one, rhov};
        double ax[9] = {ct, stt, 0, -stt, ct, 0, 0, 0, 1}; /* rows: material directions in global coordinates */
        memcpy(mt.axes, ax, sizeof ax);
        Fields f;
        fields_init(&f, b.nn, 300);
        int interior = 0;
        for (int k = 0; k <= b.nz; k++)
            for (int j = 0; j <= b.ny; j++)
                for (int i = 0; i <= b.nx; i++) {
                    int n = nid(&b, i, j, k);
                    double *x = b.xyz + 3 * (size_t)n;
                    double Tlin = 300 + 700 * x[0] + 250 * x[1] - 130 * x[2];
                    if (i == 0 || i == b.nx || j == 0 || j == b.ny || k == 0 || k == b.nz) f.fixed[n] = 1, f.fixT[n] = Tlin;
                    else interior++;
                }
        ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt, NULL, f.fixed, f.fixT, NULL, 0, NULL, NULL};
        ThermalSolver *s = thermal_create(&m, NULL, err, sizeof err);
        CHECK(s != NULL, "anisotropic create: %s", err);
        ThermalStepStats st;
        bool ok = s && thermal_step(s, &m, f.T0, f.T1, 0, -1, &st, err, sizeof err);
        CHECK(ok, "anisotropic steady solve: %s", err);
        double worst = 0;
        if (ok)
            for (int n = 0; n < b.nn; n++) {
                double *x = b.xyz + 3 * (size_t)n;
                worst = fmax(worst, fabs(f.T1[n] - (300 + 700 * x[0] + 250 * x[1] - 130 * x[2])));
            }
        CHECK(interior > 0 && worst < 1e-7, "45 deg rotated tensor, linear patch test: %d interior nodes, largest error %.3g K", interior, worst);
        thermal_free(s);
        fields_free(&f);
        box_free(&b);
    }
    {
        /* (b) a slab losing heat by convection transports the principal conductivity that lies along the flow.
         * Rotating the material axes by 90 degrees must change the transported value, and the steady power is
         *   q = (T_hot - T_amb) / (L / (k A) + 1 / (h A))
         * which is read from the reported boundary energy - a public number, not an internal residual. */
        static double one[1] = {1}, k1v[1] = {40}, k2v[1] = {8}, k3v[1] = {2}, rhov[1] = {7800}, cpv[1] = {480};
        const double L = 0.05, A = 1e-4, hconv = 500, Thot = 400, Tamb = 300;
        const double kx[3] = {40, 8, 2};
        const double axes[3][9] = {{1, 0, 0, 0, 1, 0, 0, 0, 1},  /* k1 along x */
                                   {0, -1, 0, 1, 0, 0, 0, 0, 1}, /* 90 deg about z: k2 along x */
                                   {0, 0, -1, 0, 1, 0, 1, 0, 0}};/* 90 deg about y: k3 along x */
        for (int c = 0; c < 3; c++) {
            Box b;
            box_init(&b, 10, 1, 1, L, 0.01, 0.01);
            ThermalMaterial mt = {0};
            mt.k = (ThermalTable){1, one, k1v}, mt.k2 = (ThermalTable){1, one, k2v}, mt.k3 = (ThermalTable){1, one, k3v};
            mt.cp = (ThermalTable){1, one, cpv}, mt.rho = (ThermalTable){1, one, rhov};
            memcpy(mt.axes, axes[c], sizeof mt.axes);
            Fields f;
            fields_init(&f, b.nn, 350);
            for (int j = 0; j <= 1; j++)
                for (int k = 0; k <= 1; k++) f.fixed[nid(&b, 0, j, k)] = 1, f.fixT[nid(&b, 0, j, k)] = Thot;
            ThermalFace faces[8];
            int nf = box_faces(&b, 3, THERMAL_CONVECTION, hconv, Tamb, faces, 0);
            ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt, NULL, f.fixed, f.fixT, NULL, nf, faces, NULL};
            ThermalSolver *s = thermal_create(&m, NULL, err, sizeof err);
            ThermalStepStats st;
            bool ok = s && thermal_step(s, &m, f.T0, f.T1, 0, -1, &st, err, sizeof err);
            CHECK(ok, "aligned anisotropic solve %d: %s", c, err);
            double qexp = (Thot - Tamb) / (L / (kx[c] * A) + 1 / (hconv * A));
            double qgot = ok ? -st.boundary_energy : 0; /* boundary_energy is into the body: cooling is negative */
            CHECK(ok && fabs(qgot - qexp) < 1e-9 * qexp + 1e-12, "k_x = %g W/(m K): %.9g W removed, analytic %.9g W", kx[c], qgot, qexp);
            thermal_free(s);
            fields_free(&f);
            box_free(&b);
        }
    }

    printf("== phase change: latent heat is conserved and not counted twice\n");
    {
        /* an insulated block with a uniform volumetric source stays spatially uniform, so its temperature is the
         * exact inverse of the enthalpy function. Heating from below the solidus into the mushy zone and cooling
         * back must return the original temperature: any double counting or loss of latent heat shows up here. */
        Box b;
        box_init(&b, 3, 2, 2, 0.03, 0.02, 0.02);
        static double one[1] = {1}, kv[1] = {25}, rhov[1] = {7800}, cpv[1] = {500};
        ThermalMaterial mt = {0};
        mt.k = (ThermalTable){1, one, kv}, mt.cp = (ThermalTable){1, one, cpv}, mt.rho = (ThermalTable){1, one, rhov};
        mt.latent_heat = 270000, mt.solidus = 1700, mt.liquidus = 1750;
        double H0 = thermal_enthalpy(&mt, 1600), Htarget = thermal_enthalpy(&mt, 1725);
        double dH = Htarget - H0, tend = 100, qv = dH / tend;
        CHECK(fabs(dH - (7800.0 * 500 * 125 + 7800.0 * 270000 * 0.5)) < 1e-3, "enthalpy of 1600 -> 1725 K: %.6g J/m^3", dH);
        Fields f;
        fields_init(&f, b.nn, 1600);
        double *src = malloc((size_t)b.ne * sizeof(double));
        for (int e = 0; e < b.ne; e++) src[e] = qv;
        ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt, NULL, f.fixed, f.fixT, src, 0, NULL, NULL};
        /* inside the mushy zone dH/dT is rho L / (Tl - Ts) = 4.2e10 J/(m^3 K) here, so the default 1e-6 K
         * temperature tolerance is a coarse *energy* tolerance: latent heat needs a tighter Picard tolerance */
        ThermalOptions po = {.theta = 1, .picard_tol = 1e-10, .max_picard = 400};
        ThermalSolver *s = thermal_create(&m, &po, err, sizeof err);
        CHECK(s != NULL, "phase-change create: %s", err);
        ThermalStepStats st;
        double dt = 5, worst_mismatch = 0, in = 0, stored = 0;
        int maxit = 0;
        bool ok = s != NULL;
        for (int i = 0; i < 20 && ok; i++) {
            ok = thermal_step(s, &m, f.T0, f.T1, i * dt, dt, &st, err, sizeof err);
            worst_mismatch = fmax(worst_mismatch, st.enthalpy_mismatch);
            in += st.source_energy, stored += st.stored_energy;
            if (st.picard_iterations > maxit) maxit = st.picard_iterations;
            swap(&f);
        }
        CHECK(ok, "melting steps: %s", err);
        double Tmelted = ok ? f.T0[0] : 0;
        CHECK(ok && fabs(Tmelted - 1725) < 1e-6, "uniform heating through the mushy zone reaches %.9g K (exact 1725 K)", Tmelted);
        CHECK(worst_mismatch < 1e-12, "discrete stored energy equals the integrated enthalpy change (worst mismatch %.3g)", worst_mismatch);
        CHECK(fabs(stored - in) < 1e-11 * fabs(in), "energy in %.12g J, stored %.12g J", in, stored);
        double liq = ok ? st.liquid_volume : -1, vol = 0.03 * 0.02 * 0.02;
        CHECK(ok && fabs(liq - 0.5 * vol) < 1e-12, "half the volume is molten: %.6g of %.6g m^3", liq, vol);
        /* now freeze back with the sign reversed */
        for (int e = 0; e < b.ne; e++) src[e] = -qv;
        for (int i = 0; i < 20 && ok; i++) {
            ok = thermal_step(s, &m, f.T0, f.T1, i * dt, dt, &st, err, sizeof err);
            worst_mismatch = fmax(worst_mismatch, st.enthalpy_mismatch);
            if (st.picard_iterations > maxit) maxit = st.picard_iterations;
            swap(&f);
        }
        CHECK(ok, "freezing steps: %s", err);
        double Tback = ok ? f.T0[0] : 0;
        CHECK(ok && fabs(Tback - 1600) < 1e-6, "melt and re-freeze returns to %.9g K (started at 1600 K)", Tback);
        CHECK(ok && st.liquid_volume < 1e-15, "nothing is left molten: %.3g m^3", ok ? st.liquid_volume : -1);
        printf("  melt and re-freeze of %g J/m^3 latent heat: worst enthalpy mismatch %.2g, at most %d Picard iterations per step\n",
               mt.rho.v[0] * mt.latent_heat, worst_mismatch, maxit);
        thermal_free(s);
        free(src);
        fields_free(&f);
        box_free(&b);
    }

    printf("== Stefan melting front against the similarity solution\n");
    {
        /* one-phase Stefan problem: a semi-infinite solid at the melting temperature, surface raised to Tw.
         *   s(t) = 2 lambda sqrt(alpha t),   lambda exp(lambda^2) erf(lambda) = St / sqrt(pi),  St = cp (Tw - Tm) / L
         * The apparent-capacity method smears the front over the mushy interval, so this is an approximation whose
         * error is dominated by that interval and by the element size - both reported below. */
        const double kv0 = 2, rho0 = 1000, cp0 = 2000, Lheat = 1e5, Tm = 300, Tw = 350, mushy = 0.5;
        const double alpha = kv0 / (rho0 * cp0), tend = 1000;
        double St = cp0 * (Tw - Tm) / Lheat, rhs = St / sqrt(M_PI), lam = 0.5;
        for (int i = 0; i < 200; i++) { /* Newton on lambda exp(lambda^2) erf(lambda) = St/sqrt(pi) */
            double f0 = lam * exp(lam * lam) * erf(lam) - rhs;
            double d = 1e-7, f1 = (lam + d) * exp((lam + d) * (lam + d)) * erf(lam + d) - rhs;
            double step = f0 * d / (f1 - f0);
            lam -= step;
            if (fabs(step) < 1e-14) break;
        }
        double s_exact = 2 * lam * sqrt(alpha * tend);
        Box b;
        int nx = 160;
        double Lx = 0.08;
        box_init(&b, nx, 1, 1, Lx, 0.002, 0.002);
        static double one[1] = {1}, kvv[1] = {2}, rhovv[1] = {1000}, cpvv[1] = {2000};
        ThermalMaterial mt = {0};
        mt.k = (ThermalTable){1, one, kvv}, mt.rho = (ThermalTable){1, one, rhovv}, mt.cp = (ThermalTable){1, one, cpvv};
        mt.latent_heat = Lheat, mt.solidus = Tm, mt.liquidus = Tm + mushy;
        Fields f;
        fields_init(&f, b.nn, Tm);
        for (int j = 0; j <= 1; j++)
            for (int k = 0; k <= 1; k++) f.fixed[nid(&b, 0, j, k)] = 1, f.fixT[nid(&b, 0, j, k)] = Tw;
        ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt, NULL, f.fixed, f.fixT, NULL, 0, NULL, NULL};
        ThermalOptions o = {.theta = 1, .max_picard = 400, .picard_tol = 1e-10};
        ThermalSolver *s = thermal_create(&m, &o, err, sizeof err);
        CHECK(s != NULL, "Stefan create: %s", err);
        ThermalStepStats st;
        double dt = 2, worst_bal = 0, worst_mis = 0;
        bool ok = s != NULL;
        int nsteps = (int)(tend / dt), maxit = 0, totit = 0;
        for (int i = 0; i < nsteps && ok; i++) {
            ok = thermal_step(s, &m, f.T0, f.T1, i * dt, dt, &st, err, sizeof err);
            worst_bal = fmax(worst_bal, st.balance_error), worst_mis = fmax(worst_mis, st.enthalpy_mismatch);
            if (st.picard_iterations > maxit) maxit = st.picard_iterations;
            totit += st.picard_iterations;
            swap(&f);
        }
        printf("  %d steps: %d Picard iterations at most, %.1f on average\n", nsteps, maxit, (double)totit / nsteps);
        CHECK(ok, "Stefan steps: %s", err);
        /* front = where the liquid fraction crosses 0.5, i.e. T = Tm + mushy/2, found by linear interpolation */
        double s_num = 0, Tfront = Tm + 0.5 * mushy;
        for (int i = 0; ok && i < nx; i++) {
            double Ta = f.T0[nid(&b, i, 0, 0)], Tb = f.T0[nid(&b, i + 1, 0, 0)];
            if (Ta >= Tfront && Tb < Tfront) {
                double w = (Ta - Tfront) / (Ta - Tb);
                s_num = Lx * (i + w) / nx;
                break;
            }
        }
        double relerr = s_exact > 0 ? fabs(s_num - s_exact) / s_exact : 1;
        printf("  Stefan number %.3g, lambda %.6f: front %.5f mm (similarity solution %.5f mm, %+.2f %%); "
               "mushy interval %g K, element %.3f mm, dt %g s\n",
               St, lam, 1000 * s_num, 1000 * s_exact, 100 * (s_num - s_exact) / s_exact, mushy, 1000 * Lx / nx, dt);
        CHECK(ok && relerr < 0.05, "melt front within 5 %% of the similarity solution (%.2f %%)", 100 * relerr);
        CHECK(worst_bal < 1e-8, "energy balance closes at every step (worst %.3g)", worst_bal);
        CHECK(worst_mis < 1e-10, "stored energy equals the enthalpy change at every step (worst %.3g)", worst_mis);
        thermal_free(s);
        fields_free(&f);
        box_free(&b);
    }

    printf("== thermal contact resistance across an interface\n");
    {
        /* two slabs meeting at x = L1 with a finite contact conductance hc. In steady state
         *   q = dT / (L1/(k1 A) + 1/(hc A) + L2/(k2 A))
         * and the temperature jumps by q/(hc A) across the interface. The two sides have separate nodes, joined
         * only by interface pairs, so perfect contact would give no jump at all. */
        const double L1 = 0.02, L2 = 0.02, W = 0.01, A = W * W, k1 = 50, k2 = 10, hc = 2000, Thot = 400, Tcold = 300;
        Box a, b;
        box_init(&a, 8, 2, 2, L1, W, W);
        box_init(&b, 8, 2, 2, L2, W, W);
        int nn = a.nn + b.nn, ne = a.ne + b.ne;
        double *xyz = malloc(3 * (size_t)nn * sizeof(double));
        int *conn = malloc(8 * (size_t)ne * sizeof(int)), *emat = malloc((size_t)ne * sizeof(int));
        memcpy(xyz, a.xyz, 3 * (size_t)a.nn * sizeof(double));
        for (int n = 0; n < b.nn; n++) {
            xyz[3 * (size_t)(a.nn + n)] = b.xyz[3 * (size_t)n] + L1;
            xyz[3 * (size_t)(a.nn + n) + 1] = b.xyz[3 * (size_t)n + 1];
            xyz[3 * (size_t)(a.nn + n) + 2] = b.xyz[3 * (size_t)n + 2];
        }
        memcpy(conn, a.conn, 8 * (size_t)a.ne * sizeof(int));
        for (int e = 0; e < b.ne; e++)
            for (int i = 0; i < 8; i++) conn[8 * (size_t)(a.ne + e) + i] = b.conn[8 * (size_t)e + i] + a.nn;
        for (int e = 0; e < ne; e++) emat[e] = e < a.ne ? 0 : 1;
        static double one[1] = {1}, k1v[1] = {50}, k2v[1] = {10}, rhov[1] = {8000}, cpv[1] = {500};
        ThermalMaterial mats[2] = {{0}, {0}};
        mats[0].k = (ThermalTable){1, one, k1v}, mats[0].rho = (ThermalTable){1, one, rhov}, mats[0].cp = (ThermalTable){1, one, cpv};
        mats[1].k = (ThermalTable){1, one, k2v}, mats[1].rho = (ThermalTable){1, one, rhov}, mats[1].cp = (ThermalTable){1, one, cpv};
        /* nodal interface areas: each of the 2x2 interface faces gives a quarter of its area to its four nodes */
        ThermalInterfaceNode iface[9];
        int ni = 0;
        double narea[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        double fa = (W / 2) * (W / 2);
        for (int j = 0; j < 2; j++)
            for (int k = 0; k < 2; k++)
                for (int dj = 0; dj <= 1; dj++)
                    for (int dk = 0; dk <= 1; dk++) narea[j + dj][k + dk] += fa / 4;
        for (int j = 0; j <= 2; j++)
            for (int k = 0; k <= 2; k++) iface[ni++] = (ThermalInterfaceNode){nid(&a, 8, j, k), a.nn + nid(&b, 0, j, k), hc, narea[j][k]};
        double asum = 0;
        for (int i = 0; i < ni; i++) asum += iface[i].area;
        CHECK(fabs(asum - A) < 1e-15, "nodal interface areas sum to the interface area (%.9g of %.9g m^2)", asum, A);
        unsigned char *fixed = calloc((size_t)nn, 1);
        double *fixT = calloc((size_t)nn, sizeof(double)), *T0 = malloc((size_t)nn * sizeof(double)), *T1 = malloc((size_t)nn * sizeof(double));
        for (int n = 0; n < nn; n++) T0[n] = T1[n] = 350;
        for (int j = 0; j <= 2; j++)
            for (int k = 0; k <= 2; k++) {
                fixed[nid(&a, 0, j, k)] = 1, fixT[nid(&a, 0, j, k)] = Thot;
                fixed[a.nn + nid(&b, 8, j, k)] = 1, fixT[a.nn + nid(&b, 8, j, k)] = Tcold;
            }
        ThermalModel m = {nn, ne, xyz, conn, emat, 2, mats, NULL, fixed, fixT, NULL, 0, NULL, NULL, ni, iface};
        ThermalSolver *s = thermal_create(&m, NULL, err, sizeof err);
        CHECK(s != NULL, "interface create: %s", err);
        ThermalStepStats st;
        bool ok = s && thermal_step(s, &m, T0, T1, 0, -1, &st, err, sizeof err);
        CHECK(ok, "interface steady solve: %s", err);
        double R = L1 / (k1 * A) + 1 / (hc * A) + L2 / (k2 * A), q = (Thot - Tcold) / R;
        double TaI = ok ? T1[nid(&a, 8, 1, 1)] : 0, TbI = ok ? T1[a.nn + nid(&b, 0, 1, 1)] : 0;
        double Ta_exp = Thot - q * L1 / (k1 * A), Tb_exp = Ta_exp - q / (hc * A);
        CHECK(ok && fabs(TaI - Ta_exp) < 1e-9, "hot side of the interface %.9g K (analytic %.9g K)", TaI, Ta_exp);
        CHECK(ok && fabs(TbI - Tb_exp) < 1e-9, "cold side of the interface %.9g K (analytic %.9g K)", TbI, Tb_exp);
        CHECK(ok && fabs((TaI - TbI) - q / (hc * A)) < 1e-9, "temperature jump %.9g K (analytic %.9g K)", TaI - TbI, q / (hc * A));
        printf("  series resistance %.4g K/W: %.6g W through the wall, %.4g K jump at a %g W/(m^2 K) contact\n", R, q, q / (hc * A), hc);
        /* perfect contact (a very large conductance) must remove the jump */
        for (int i = 0; i < ni; i++) iface[i].conductance = 1e9;
        ThermalSolver *s2 = thermal_create(&m, NULL, err, sizeof err);
        bool ok2 = s2 && thermal_step(s2, &m, T0, T1, 0, -1, &st, err, sizeof err);
        double jump2 = ok2 ? T1[nid(&a, 8, 1, 1)] - T1[a.nn + nid(&b, 0, 1, 1)] : 1;
        CHECK(ok2 && fabs(jump2) < 1e-4, "a 1e9 W/(m^2 K) contact leaves a %.3g K jump (perfect contact)", jump2);
        thermal_free(s), thermal_free(s2);
        free(xyz), free(conn), free(emat), free(fixed), free(fixT), free(T0), free(T1);
        box_free(&a), box_free(&b);
    }

    printf("== timing\n");
    {
        Box b;
        box_init(&b, 60, 40, 40, 0.06, 0.04, 0.04);
        Mat mt;
        mat_const(&mt, 15, 7950, 500);
        ThermalFace *faces = malloc(2400 * sizeof *faces);
        int nf = box_faces(&b, 1, THERMAL_CONVECTION, 25, 293.15, faces, 0);
        Fields f;
        fields_init(&f, b.nn, 293.15);
        for (int n = 0; n < b.nn; n++)
            if (b.xyz[3 * (size_t)n + 2] == 0) f.fixed[n] = 1, f.fixT[n] = 473.15;
        ThreadPool *pool = pool_create(cpu_perf_count());
        ThermalModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mt.m, NULL, f.fixed, f.fixT, NULL, nf, faces, NULL};
        ThermalOptions o = {.theta = 1, .pool = pool};
        ThermalSolver *s = thermal_create(&m, &o, err, sizeof err);
        ThermalStepStats st;
        bool ok = s && thermal_step(s, &m, f.T0, f.T1, 0, 1, &st, err, sizeof err);
        CHECK(ok, "100k-element step: %s", err);
        printf("  %d elements, %d nodes: %.3f s per backward Euler step (assembly %.3f s, solve %.3f s, energy %.3f s; %d CG iterations, %d threads)\n", b.ne,
               b.nn, st.seconds, st.seconds_assembly, st.seconds_solve, st.seconds_energy, st.linear_iterations, pool_size(pool));
        thermal_free(s);
        pool_destroy(pool);
        free(faces);
        fields_free(&f);
        box_free(&b);
    }

    printf("\n%s: %d passed, %d failed\n", g_fail ? "THERMAL TESTS FAILED" : "ALL THERMAL VERIFICATION TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
