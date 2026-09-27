/* advtest.c - verification of the nonsymmetric solver and of advective energy transport
 *
 * Criteria were fixed before the first run: "Thermal Sim/verification/stageE-criteria.md".
 *   make build/advtest && ./build/advtest */
#include "../src/fem/dense.h"
#include "../src/fem/flow.h"
#include "../src/fem/hex8.h"
#include "../src/fem/krylov.h"
#include "../src/fem/sparse.h"
#include "../src/fem/orchestrator.h"
#include "../src/fem/thermal.h"
#include "../src/fem/thermal_integrator.h"
#include "../src/fem/thermal_participant.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- small CSR builders ---------------------------------------------------------------------------------------------- */

static bool csr_from_dense(CsrMatrix *A, const double *D, int n) {
    memset(A, 0, sizeof *A);
    A->n = n;
    int64_t nnz = 0;
    for (int i = 0; i < n * n; i++) nnz += D[i] != 0;
    A->rowptr = malloc(((size_t)n + 1) * sizeof(int64_t));
    A->col = malloc((size_t)(nnz ? nnz : 1) * sizeof(int));
    A->val = malloc((size_t)(nnz ? nnz : 1) * sizeof(double));
    if (!A->rowptr || !A->col || !A->val) return false;
    int64_t k = 0;
    for (int i = 0; i < n; i++) {
        A->rowptr[i] = k;
        for (int j = 0; j < n; j++)
            if (D[i * n + j] != 0) A->col[k] = j, A->val[k++] = D[i * n + j];
    }
    A->rowptr[n] = k;
    A->nnz = k;
    return true;
}

/* 7-point operator on an m^3 grid of unknowns (Dirichlet zero outside): diag + stencil, central advection along x */
static bool csr_grid(CsrMatrix *A, int m, double diag_extra, double peclet) {
    int n = m * m * m;
    memset(A, 0, sizeof *A);
    A->n = n;
    A->rowptr = malloc(((size_t)n + 1) * sizeof(int64_t));
    A->col = malloc(7 * (size_t)n * sizeof(int));
    A->val = malloc(7 * (size_t)n * sizeof(double));
    if (!A->rowptr || !A->col || !A->val) return false;
    int64_t k = 0;
    for (int z = 0; z < m; z++)
        for (int y = 0; y < m; y++)
            for (int x = 0; x < m; x++) {
                int i = x + m * (y + m * z);
                A->rowptr[i] = k;
                /* columns in increasing order: z-, y-, x-, diag, x+, y+, z+ */
                if (z > 0) A->col[k] = i - m * m, A->val[k++] = -1;
                if (y > 0) A->col[k] = i - m, A->val[k++] = -1;
                if (x > 0) A->col[k] = i - 1, A->val[k++] = -1 - 0.5 * peclet;
                A->col[k] = i, A->val[k++] = 6 + diag_extra;
                if (x < m - 1) A->col[k] = i + 1, A->val[k++] = -1 + 0.5 * peclet;
                if (y < m - 1) A->col[k] = i + m, A->val[k++] = -1;
                if (z < m - 1) A->col[k] = i + m * m, A->val[k++] = -1;
            }
    A->rowptr[n] = k;
    A->nnz = k;
    return true;
}

static double rel_diff(const double *a, const double *b, int n) {
    double d = 0, s = 0;
    for (int i = 0; i < n; i++) d += (a[i] - b[i]) * (a[i] - b[i]), s += b[i] * b[i];
    return s > 0 ? sqrt(d / s) : sqrt(d);
}

static unsigned long g_rng = 12345;
static double urand(void) {
    g_rng = g_rng * 6364136223846793005ul + 1442695040888963407ul;
    return (double)(g_rng >> 11) / 9007199254740992.0;
}

static void test_floor(void) {
    /* added after the E5h 0.5 mm run (stageE-criteria.md, findings): a relative tolerance below the rounding floor of a
     * large nonsymmetric system is met as far as floating point allows, and an unconverged solve is still refused */
    printf("== E1g rounding floor (added after the 0.5 mm finding)\n");
    char err[512];
    SolveStats st;
    int m = 30, n = m * m * m;
    CsrMatrix A;
    csr_grid(&A, m, 0, 50);
    double *xs = malloc((size_t)n * sizeof(double)), *b = malloc((size_t)n * sizeof(double)), *x = calloc((size_t)n, sizeof(double));
    for (int i = 0; i < n; i++) xs[i] = 300 + sin(0.013 * i);
    csr_spmv(&A, xs, b, NULL);
    KrylovOptions o = {KRYLOV_PRECOND_ILU0, 1e-18, 5000, 0, NULL};
    bool ok = bicgstab_solve(&A, b, x, &o, &st, err, sizeof err);
    double *r = malloc((size_t)n * sizeof(double)), rinf = 0, xinf = 0, binf = 0, anorm = 0;
    csr_spmv(&A, x, r, NULL);
    for (int i = 0; i < n; i++) {
        double row = 0;
        for (int64_t k = A.rowptr[i]; k < A.rowptr[i + 1]; k++) row += fabs(A.val[k]);
        anorm = fmax(anorm, row), rinf = fmax(rinf, fabs(b[i] - r[i])), xinf = fmax(xinf, fabs(x[i])), binf = fmax(binf, fabs(b[i]));
    }
    double backward = rinf / (anorm * xinf + binf), e = rel_diff(x, xs, n);
    printf("  tolerance 1e-18: %s after %d iterations, true residual %.2e, backward error %.2e, solution error %.2e\n", ok ? "converged" : "failed",
           st.iterations, st.true_residual, backward, e);
    CHECK(ok && backward <= 64 * 2.220446049250313e-16 && e <= 1e-10, "an unattainable tolerance is met at the rounding floor (%s)", err);
    memset(x, 0, (size_t)n * sizeof(double));
    o.tol = 1e-12, o.max_iter = 3;
    err[0] = 0;
    ok = bicgstab_solve(&A, b, x, &o, &st, err, sizeof err);
    printf("  3 iterations: %s\n", ok ? "reported converged (wrong)" : err);
    CHECK(!ok && !st.converged, "a solve stopped early is still refused");
    csr_free(&A);
    free(xs), free(b), free(x), free(r);
}

static void test_solver(void) {
    char err[512];
    SolveStats st;

    printf("== E1a tridiagonal: ILU(0) is the exact LU\n");
    {
        enum { N = 50 };
        static double D[N * N];
        memset(D, 0, sizeof D);
        for (int i = 0; i < N; i++) {
            D[i * N + i] = 4;
            if (i > 0) D[i * N + i - 1] = -2.5;
            if (i < N - 1) D[i * N + i + 1] = 0.7;
        }
        CsrMatrix A;
        csr_from_dense(&A, D, N);
        double xs[N], b[N], x[N] = {0};
        for (int i = 0; i < N; i++) xs[i] = urand() - 0.5;
        csr_spmv(&A, xs, b, NULL);
        KrylovOptions o = {KRYLOV_PRECOND_ILU0, 1e-13, 100, 0, NULL};
        bool ok = bicgstab_solve(&A, b, x, &o, &st, err, sizeof err);
        double e = rel_diff(x, xs, N);
        printf("  %d iterations, error %.2e\n", st.iterations, e);
        CHECK(ok && st.iterations == 1 && e <= 1e-12, "converges in one iteration to the exact solution (%d, %.2e): %s", st.iterations, e, err);
        csr_free(&A);
    }

    printf("== E1b random sparse nonsymmetric system against dense LU\n");
    {
        enum { N = 60 };
        static double D[N * N], Dc[N * N];
        memset(D, 0, sizeof D);
        for (int i = 0; i < N; i++) {
            double row = 0;
            for (int j = 0; j < N; j++)
                if (j != i && urand() < 0.12) D[i * N + j] = 2 * urand() - 1, row += fabs(D[i * N + j]);
            D[i * N + i] = row + 0.5 + urand();
        }
        CsrMatrix A;
        csr_from_dense(&A, D, N);
        double b[N], bc[N], xs[N];
        for (int i = 0; i < N; i++) b[i] = urand() - 0.5;
        memcpy(Dc, D, sizeof D), memcpy(bc, b, sizeof b);
        bool dense_ok = dense_solve(Dc, bc, N);
        memcpy(xs, bc, sizeof bc);
        for (int pc = 0; pc < 2; pc++) {
            double x[N] = {0};
            KrylovOptions o = {pc == 0 ? KRYLOV_PRECOND_ILU0 : KRYLOV_PRECOND_JACOBI, 1e-12, 1000, 0, NULL};
            bool ok = bicgstab_solve(&A, b, x, &o, &st, err, sizeof err);
            double e = rel_diff(x, xs, N);
            printf("  %s: %d iterations, true residual %.2e, difference to dense LU %.2e\n", st.method, st.iterations, st.true_residual, e);
            CHECK(dense_ok && ok && e <= 1e-9, "%s agrees with dense LU (%.2e): %s", st.method, e, err);
        }
        csr_free(&A);
    }

    printf("== E1c advection-diffusion matrices, manufactured solution\n");
    {
        const double PE[4] = {0.1, 1, 10, 100};
        int m = 20, n = m * m * m;
        double *xs = malloc((size_t)n * sizeof(double)), *b = malloc((size_t)n * sizeof(double)), *x = malloc((size_t)n * sizeof(double));
        for (int i = 0; i < n; i++) xs[i] = sin(0.37 * i) + 0.1 * (urand() - 0.5);
        for (int k = 0; k < 4; k++) {
            CsrMatrix A;
            csr_grid(&A, m, 0, PE[k]);
            csr_spmv(&A, xs, b, NULL);
            int iters[2] = {0, 0};
            bool conv[2] = {false, false};
            double errs[2] = {0, 0};
            for (int pc = 0; pc < 2; pc++) {
                memset(x, 0, (size_t)n * sizeof(double));
                KrylovOptions o = {pc == 0 ? KRYLOV_PRECOND_ILU0 : KRYLOV_PRECOND_JACOBI, 1e-10, 4000, 0, NULL};
                conv[pc] = bicgstab_solve(&A, b, x, &o, &st, err, sizeof err);
                iters[pc] = st.iterations;
                errs[pc] = rel_diff(x, xs, n);
            }
            printf("  cell Peclet %5g: ILU(0) %s in %d iterations (error %.2e); Jacobi %s in %d iterations (error %.2e)\n", PE[k],
                   conv[0] ? "converged" : "FAILED", iters[0], errs[0], conv[1] ? "converged" : "failed", iters[1], errs[1]);
            CHECK(conv[0] && errs[0] <= 1e-7, "ILU(0) at cell Peclet %g converges to the manufactured solution (%.2e)", PE[k], errs[0]);
            csr_free(&A);
        }
        free(xs), free(b), free(x);
    }

    printf("== E1d symmetric positive-definite system: BiCGSTAB equals PCG\n");
    {
        int m = 20, n = m * m * m;
        CsrMatrix A;
        csr_grid(&A, m, 1.0, 0);
        double *b = malloc((size_t)n * sizeof(double)), *x1 = calloc((size_t)n, sizeof(double)), *x2 = calloc((size_t)n, sizeof(double));
        for (int i = 0; i < n; i++) b[i] = cos(0.11 * i);
        KrylovOptions o = {KRYLOV_PRECOND_ILU0, 1e-12, 4000, 0, NULL};
        bool ok1 = bicgstab_solve(&A, b, x1, &o, &st, err, sizeof err);
        int it1 = st.iterations;
        PcgOptions po = {PRECOND_JACOBI, NULL, 1e-12, 4000, NULL, NULL, NULL};
        bool ok2 = pcg_solve(&A, b, x2, &po, &st, err, sizeof err);
        double d = rel_diff(x1, x2, n);
        printf("  BiCGSTAB-ILU(0) %d iterations, PCG %d iterations, difference %.2e\n", it1, st.iterations, d);
        CHECK(ok1 && ok2 && d <= 1e-9, "the two solutions agree (%.2e)", d);
        csr_free(&A);
        free(b), free(x1), free(x2);
    }

    printf("== E1e singular and defective matrices are refused\n");
    {
        enum { N = 8 };
        double D[N * N] = {0};
        for (int i = 0; i < N; i++) {
            if (i == 3) continue; /* a zero row */
            D[i * N + i] = 2;
            if (i > 0) D[i * N + i - 1] = -1;
            if (i < N - 1) D[i * N + i + 1] = -1;
        }
        D[3 * N + 3] = 0;
        CsrMatrix A;
        csr_from_dense(&A, D, N);
        double b[N], x[N] = {0};
        for (int i = 0; i < N; i++) b[i] = 1;
        /* a zero row has no diagonal in the pattern: both preconditioners must refuse */
        for (int pc = 0; pc < 2; pc++) {
            memset(x, 0, sizeof x);
            err[0] = 0;
            KrylovOptions o = {pc == 0 ? KRYLOV_PRECOND_ILU0 : KRYLOV_PRECOND_JACOBI, 1e-12, 200, 0, NULL};
            bool ok = bicgstab_solve(&A, b, x, &o, &st, err, sizeof err);
            printf("  zero row, %s: %s\n", pc == 0 ? "ILU(0)" : "Jacobi", err);
            CHECK(!ok && !st.converged && err[0], "a zero row is refused with a message (%s)", pc == 0 ? "ILU(0)" : "Jacobi");
        }
        csr_free(&A);
        /* the pure-Neumann Laplacian (rows sum to zero) with a right-hand side outside its range has no solution */
        double S[N * N] = {0};
        for (int i = 0; i < N; i++) {
            S[i * N + i] = (i == 0 || i == N - 1) ? 1 : 2;
            if (i > 0) S[i * N + i - 1] = -1;
            if (i < N - 1) S[i * N + i + 1] = -1;
        }
        csr_from_dense(&A, S, N);
        for (int i = 0; i < N; i++) b[i] = 1; /* inconsistent: the sum is not zero */
        for (int pc = 0; pc < 2; pc++) {
            memset(x, 0, sizeof x);
            err[0] = 0;
            KrylovOptions o = {KRYLOV_PRECOND_JACOBI, 1e-12, 500, 0, NULL};
            bool ok = bicgstab_solve(&A, b, x, &o, &st, err, sizeof err);
            if (pc == 1) break;
            printf("  inconsistent singular system: %s\n", err);
            CHECK(!ok && !st.converged && err[0], "an inconsistent singular system never reports convergence (true residual %.3g)", st.true_residual);
        }
        csr_free(&A);
    }
}

/* ---- box meshes: nx x ny x nz elements over [0,Lx] x [0,Ly] x [0,Lz] ---------------------------------------------------
 * hex8 local faces: 0 z-, 1 z+, 2 y-, 3 x+, 4 y+, 5 x- */
enum { F_ZM = 0, F_ZP = 1, F_YM = 2, F_XP = 3, F_YP = 4, F_XM = 5 };

typedef struct {
    int nx, ny, nz, nn, ne;
    double *xyz;
    int *conn;
} Box;

static int bn(const Box *b, int i, int j, int k) { return i + (b->nx + 1) * (j + (b->ny + 1) * k); }
static int be_(const Box *b, int i, int j, int k) { return i + b->nx * (j + b->ny * k); }

static void box_init(Box *b, int nx, int ny, int nz, double Lx, double Ly, double Lz) {
    b->nx = nx, b->ny = ny, b->nz = nz;
    b->nn = (nx + 1) * (ny + 1) * (nz + 1), b->ne = nx * ny * nz;
    b->xyz = malloc(3 * (size_t)b->nn * sizeof(double));
    b->conn = malloc(8 * (size_t)b->ne * sizeof(int));
    for (int k = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) {
                double *x = b->xyz + 3 * (size_t)bn(b, i, j, k);
                x[0] = Lx * i / nx, x[1] = Ly * j / ny, x[2] = Lz * k / nz;
            }
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                int *c = b->conn + 8 * (size_t)be_(b, i, j, k);
                c[0] = bn(b, i, j, k), c[1] = bn(b, i + 1, j, k), c[2] = bn(b, i + 1, j + 1, k), c[3] = bn(b, i, j + 1, k);
                c[4] = bn(b, i, j, k + 1), c[5] = bn(b, i + 1, j, k + 1), c[6] = bn(b, i + 1, j + 1, k + 1), c[7] = bn(b, i, j + 1, k + 1);
            }
}

static void box_free(Box *b) { free(b->xyz), free(b->conn); }

typedef struct {
    double tt, k, rho, cp;
    ThermalMaterial mat;
} Mat1;

static void mat1(Mat1 *m, double k, double rho, double cp) {
    memset(m, 0, sizeof *m);
    m->tt = 300, m->k = k, m->rho = rho, m->cp = cp;
    m->mat.k = (ThermalTable){1, &m->tt, &m->k};
    m->mat.rho = (ThermalTable){1, &m->tt, &m->rho};
    m->mat.cp = (ThermalTable){1, &m->tt, &m->cp};
}

/* faces on one side of the box */
static int side_faces(const Box *b, int side, int kind, double value, double ambient, ThermalFace *out, int at) {
    int n = at;
    for (int k = 0; k < b->nz; k++)
        for (int j = 0; j < b->ny; j++)
            for (int i = 0; i < b->nx; i++) {
                bool on = (side == F_XM && i == 0) || (side == F_XP && i == b->nx - 1) || (side == F_YM && j == 0) || (side == F_YP && j == b->ny - 1) ||
                          (side == F_ZM && k == 0) || (side == F_ZP && k == b->nz - 1);
                if (on) out[n++] = (ThermalFace){be_(b, i, j, k), (unsigned char)side, (unsigned char)kind, value, ambient};
            }
    return n;
}

/* E2a: steady 1-D advection-diffusion; returns the max nodal error and the extreme values */
static double run_1d_steady(int N, double Pe, int stab, double *tmin, double *tmax, const char **method) {
    Box b;
    double h = 1.0 / N;
    box_init(&b, N, 1, 1, 1.0, h, h);
    Mat1 mt;
    mat1(&mt, 1, 1, 1); /* alpha = 1, L = 1: u = Pe */
    double *vel = calloc(3 * (size_t)b.nn, sizeof(double)), *T0 = malloc((size_t)b.nn * sizeof(double)), *T1 = malloc((size_t)b.nn * sizeof(double)),
           *fT = calloc((size_t)b.nn, sizeof(double));
    unsigned char *fixed = calloc((size_t)b.nn, 1);
    for (int n = 0; n < b.nn; n++) {
        vel[3 * (size_t)n] = Pe;
        double x = b.xyz[3 * (size_t)n];
        T0[n] = 300.5; /* temperatures are absolute: the unit profile is carried from 300 K to 301 K */
        if (x < 1e-12) fixed[n] = 1, fT[n] = 300;
        if (x > 1 - 1e-12) fixed[n] = 1, fT[n] = 301;
    }
    ThermalModel m = {.nnodes = b.nn, .nelems = b.ne, .xyz = b.xyz, .conn = b.conn, .nmat = 1, .mat = &mt.mat, .fixed = fixed, .fixed_T = fT, .velocity = vel};
    ThermalOptions o = {.theta = 1, .pcg_tol = 1e-13, .advection_stabilization = stab};
    char err[512];
    ThermalSolver *s = thermal_create(&m, &o, err, sizeof err);
    ThermalStepStats st;
    double emax = INFINITY;
    *tmin = INFINITY, *tmax = -INFINITY;
    if (s && thermal_step(s, &m, T0, T1, 0, 0, &st, err, sizeof err)) {
        emax = 0;
        for (int n = 0; n < b.nn; n++) {
            double x = b.xyz[3 * (size_t)n], ex = expm1(Pe * x) / expm1(Pe);
            emax = fmax(emax, fabs(T1[n] - 300 - ex));
            *tmin = fmin(*tmin, T1[n] - 300), *tmax = fmax(*tmax, T1[n] - 300);
        }
        if (method) *method = st.linear_method;
    } else {
        printf("  run failed: %s\n", err);
    }
    thermal_free(s);
    free(vel), free(T0), free(T1), free(fT), free(fixed);
    box_free(&b);
    return emax;
}

static void test_advection(void) {
    char err[512];

    printf("== E2a steady 1-D advection-diffusion against the exponential solution (global Peclet 20)\n");
    {
        const int NS[4] = {10, 20, 40, 80};
        double eg[4], es[4], lo, hi;
        const char *method = NULL;
        for (int i = 0; i < 4; i++) {
            eg[i] = run_1d_steady(NS[i], 20, THERMAL_STAB_NONE, &lo, &hi, NULL);
            es[i] = run_1d_steady(NS[i], 20, THERMAL_STAB_SUPG, &lo, &hi, &method);
            printf("  %2d elements (cell Peclet %.2g): Galerkin %.3e, SUPG %.3e\n", NS[i], 20.0 / NS[i], eg[i], es[i]);
        }
        double pg = log2(eg[2] / eg[3]), ps = log2(es[2] / es[3]);
        printf("  observed order 40 -> 80: Galerkin %.2f, SUPG %.2f\n", pg, ps);
        CHECK(pg >= 1.8, "Galerkin converges at order >= 1.8 (%.2f)", pg);
        CHECK(ps >= 1.8, "SUPG converges at order >= 1.8 (%.2f)", ps);
        printf("== E1f the advective step uses BiCGSTAB\n");
        CHECK(method && !strcmp(method, "bicgstab/ilu0"), "the linear method is bicgstab/ilu0 (%s)", method ? method : "none");
    }

    printf("== E2a' cell Peclet 10: SUPG stays bounded\n");
    {
        double lo, hi;
        double eg = run_1d_steady(20, 200, THERMAL_STAB_NONE, &lo, &hi, NULL);
        printf("  Galerkin: error %.3e, values %.4f .. %.4f (above 300 K)\n", eg, lo, hi);
        double es = run_1d_steady(20, 200, THERMAL_STAB_SUPG, &lo, &hi, NULL);
        printf("  SUPG:     error %.3e, values %.4f .. %.4f (above 300 K)\n", es, lo, hi);
        CHECK(lo >= -0.01 && hi <= 1.01, "SUPG values within [-0.01, 1.01] (%.4f .. %.4f)", lo, hi);
    }

    printf("== E2b a uniform temperature stays uniform under a non-solenoidal velocity\n");
    {
        Box b;
        box_init(&b, 6, 5, 4, 0.06, 0.05, 0.04);
        Mat1 mt;
        mat1(&mt, 0.026, 1.2, 1005);
        double *vel = malloc(3 * (size_t)b.nn * sizeof(double)), *T0 = malloc((size_t)b.nn * sizeof(double)), *T1 = malloc((size_t)b.nn * sizeof(double));
        for (int i = 0; i < 3 * b.nn; i++) vel[i] = 0.2 * (urand() - 0.5);
        for (int n = 0; n < b.nn; n++) T0[n] = 350;
        ThermalFace *faces = malloc(2 * (size_t)(b.nx * b.ny + b.ny * b.nz + b.nx * b.nz) * sizeof(ThermalFace));
        int nf = 0;
        for (int side = 0; side < 6; side++) nf = side_faces(&b, side, THERMAL_OPEN, 0, 350, faces, nf);
        ThermalModel m = {.nnodes = b.nn, .nelems = b.ne, .xyz = b.xyz, .conn = b.conn, .nmat = 1, .mat = &mt.mat, .nfaces = nf, .faces = faces, .velocity = vel};
        ThermalOptions o = {.theta = 1};
        ThermalSolver *s = thermal_create(&m, &o, err, sizeof err);
        ThermalStepStats st;
        double dev = 0;
        bool ok = s != NULL;
        for (int step = 0; ok && step < 10; step++) {
            ok = thermal_step(s, &m, T0, T1, step * 0.1, 0.1, &st, err, sizeof err);
            for (int n = 0; n < b.nn; n++) dev = fmax(dev, fabs(T1[n] - 350));
            memcpy(T0, T1, (size_t)b.nn * sizeof(double));
        }
        printf("  largest deviation after 10 steps: %.3e K (%s)\n", dev, ok ? "ok" : err);
        CHECK(ok && dev <= 1e-9, "uniform temperature preserved (%.3e K)", dev);
        thermal_free(s);
        free(vel), free(T0), free(T1), free(faces);
        box_free(&b);
    }
}

/* E2c: the exact moving, spreading Gaussian */
static double pulse_exact(double x, double t, double u, double alpha, double x0, double s0) {
    double s2 = s0 * s0 + 2 * alpha * t;
    return 300 + sqrt(s0 * s0 / s2) * exp(-(x - x0 - u * t) * (x - x0 - u * t) / (2 * s2));
}

static double run_pulse(int N, bool consistent) {
    Box b;
    double h = 1.0 / N, u = 1e-3, alpha = 1e-5, x0 = 0.25, s0 = 0.05, dt = 0.5, t_end = 300;
    box_init(&b, N, 1, 1, 1.0, h, h);
    Mat1 mt;
    mat1(&mt, 10, 1000, 1000);
    double *vel = calloc(3 * (size_t)b.nn, sizeof(double)), *T0 = malloc((size_t)b.nn * sizeof(double)), *T1 = malloc((size_t)b.nn * sizeof(double));
    for (int n = 0; n < b.nn; n++) {
        vel[3 * (size_t)n] = u;
        T0[n] = pulse_exact(b.xyz[3 * (size_t)n], 0, u, alpha, x0, s0);
    }
    ThermalFace faces[4];
    int nf = side_faces(&b, F_XM, THERMAL_OPEN, 0, 300, faces, 0);
    nf = side_faces(&b, F_XP, THERMAL_OPEN, 0, 300, faces, nf);
    ThermalModel m = {.nnodes = b.nn, .nelems = b.ne, .xyz = b.xyz, .conn = b.conn, .nmat = 1, .mat = &mt.mat, .nfaces = nf, .faces = faces, .velocity = vel};
    ThermalOptions o = {.theta = 0.5, .consistent_capacity = consistent, .pcg_tol = 1e-13};
    char err[512];
    ThermalSolver *s = thermal_create(&m, &o, err, sizeof err);
    ThermalStepStats st;
    bool ok = s != NULL;
    int nsteps = (int)llround(t_end / dt);
    for (int k = 0; ok && k < nsteps; k++) {
        ok = thermal_step(s, &m, T0, T1, k * dt, dt, &st, err, sizeof err);
        double *tmp = T0;
        T0 = T1, T1 = tmp;
    }
    double e = INFINITY;
    if (ok) {
        e = 0;
        for (int n = 0; n < b.nn; n++) e = fmax(e, fabs(T0[n] - pulse_exact(b.xyz[3 * (size_t)n], t_end, u, alpha, x0, s0)));
    } else {
        printf("  run failed: %s\n", err);
    }
    thermal_free(s);
    free(vel), free(T0), free(T1);
    box_free(&b);
    return e;
}

static void test_pulse(void) {
    printf("== E2c transient pulse against the exact solution (Crank-Nicolson, dt 0.5 s, 300 s)\n");
    const int NS[3] = {100, 200, 400};
    double ec[3], el[3];
    for (int i = 0; i < 3; i++) {
        ec[i] = run_pulse(NS[i], true);
        el[i] = run_pulse(NS[i], false);
        printf("  %3d elements (cell Peclet %.2g): consistent capacity %.3e K, lumped %.3e K\n", NS[i], 1e-3 * (1.0 / NS[i]) / 1e-5, ec[i], el[i]);
    }
    double pc = log2(ec[1] / ec[2]), pl = log2(el[1] / el[2]);
    printf("  observed order 200 -> 400: consistent %.2f, lumped %.2f\n", pc, pl);
    CHECK(pc >= 1.7, "consistent capacity converges at order >= 1.7 (%.2f)", pc);
    CHECK(ec[2] <= 5e-4, "finest error <= 5e-4 K (%.3e K)", ec[2]);
}

static void test_energy(void) {
    printf("== E2d energy accounting with advection (channel, source, open ends, convective wall)\n");
    char err[512];
    Box b;
    box_init(&b, 10, 4, 4, 0.1, 0.04, 0.04);
    Mat1 mt;
    mat1(&mt, 0.6, 1000, 4000);
    double *vel = calloc(3 * (size_t)b.nn, sizeof(double)), *T0 = malloc((size_t)b.nn * sizeof(double)), *T1 = malloc((size_t)b.nn * sizeof(double));
    double *src = calloc((size_t)b.ne, sizeof(double));
    for (int n = 0; n < b.nn; n++) vel[3 * (size_t)n] = 0.01, T0[n] = 300;
    for (int k = 0; k < b.nz; k++)
        for (int j = 0; j < b.ny; j++)
            for (int i = 4; i < 6; i++) src[be_(&b, i, j, k)] = 1e4;
    ThermalFace faces[2 * 4 * 4 + 10 * 4]; /* the two x ends (16 faces each) and the y- side (40): sized from the box */
    int nf = side_faces(&b, F_XM, THERMAL_OPEN, 0, 300, faces, 0);
    nf = side_faces(&b, F_XP, THERMAL_OPEN, 0, 300, faces, nf);
    int nconv0 = nf;
    nf = side_faces(&b, F_YM, THERMAL_CONVECTION, 50, 290, faces, nf);
    ThermalModel m = {.nnodes = b.nn, .nelems = b.ne, .xyz = b.xyz, .conn = b.conn, .nmat = 1, .mat = &mt.mat, .elem_source = src, .nfaces = nf,
                      .faces = faces, .velocity = vel};
    ThermalOptions o = {.theta = 1, .pcg_tol = 1e-13};
    ThermalSolver *s = thermal_create(&m, &o, err, sizeof err);
    ThermalStepStats st;
    bool ok = s != NULL;
    double worst_balance = 0, worst_div = 0;
    for (int step = 0; ok && step < 50; step++) {
        ok = thermal_step(s, &m, T0, T1, 5.0 * step, 5.0, &st, err, sizeof err);
        worst_balance = fmax(worst_balance, st.balance_error);
        double big = fmax(fabs(st.advection_energy), fmax(fabs(st.enthalpy_inflow), fabs(st.enthalpy_outflow)));
        if (big > 0) worst_div = fmax(worst_div, fabs(st.divergence_energy) / big);
        memcpy(T0, T1, (size_t)b.nn * sizeof(double));
    }
    printf("  50 steps of 5 s: worst balance %.2e, worst unexplained advective energy %.2e (%s)\n", worst_balance, worst_div, ok ? "ok" : err);
    CHECK(ok && worst_balance <= 1e-10, "every step balances with the advection and inflow terms (%.2e)", worst_balance);
    CHECK(ok && worst_div <= 1e-10, "the solenoidal field explains all advected energy through the open faces (%.2e)", worst_div);
    /* steady state: what the source delivers leaves with the fluid and through the convective wall */
    ok = ok && thermal_step(s, &m, T0, T1, 250, 0, &st, err, sizeof err);
    double power = 1e4 * 2 * 0.01 * 0.04 * 0.04, loss = -st.boundary_energy, carried = st.enthalpy_outflow - st.enthalpy_inflow;
    double closure = fabs(carried + loss - power) / power;
    printf("  steady: source %.6g W, enthalpy out - in %.6g W, convective loss %.6g W, closure %.2e (%s)\n", power, carried, loss, closure,
           ok ? "ok" : err);
    CHECK(ok && closure <= 1e-6, "steady state: enthalpy out - in + convective loss = source power (%.2e)", closure);
    thermal_free(s);
    /* the same model without a velocity keeps conjugate gradients */
    ThermalModel m2 = m;
    m2.velocity = NULL, m2.nfaces = nf - nconv0, m2.faces = faces + nconv0;
    for (int n = 0; n < b.nn; n++) T0[n] = 300;
    s = thermal_create(&m2, &o, err, sizeof err);
    ok = s && thermal_step(s, &m2, T0, T1, 0, 5, &st, err, sizeof err);
    CHECK(ok && st.linear_method && !strcmp(st.linear_method, "pcg/jacobi"), "a step without advection keeps pcg/jacobi (%s)",
          ok && st.linear_method ? st.linear_method : err);
    thermal_free(s);
    free(vel), free(T0), free(T1), free(src);
    box_free(&b);
}

/* E2e: laminar parallel-plate channel, uniform flux on the bottom wall, top adiabatic: fully developed Nusselt number */
static double run_channel(int ny) {
    char err[512];
    const double H = 0.01, L = 30 * H, k = 0.6, rho = 1000, cp = 4000, alpha = k / (rho * cp), q = 100;
    double U = 50 * alpha / (2 * H); /* Peclet U D_h / alpha = 50 */
    int nx = 15 * ny;
    Box b;
    box_init(&b, nx, ny, 1, L, H, H / ny);
    Mat1 mt;
    mat1(&mt, k, rho, cp);
    double *vel = calloc(3 * (size_t)b.nn, sizeof(double)), *T0 = malloc((size_t)b.nn * sizeof(double)), *T1 = malloc((size_t)b.nn * sizeof(double));
    for (int n = 0; n < b.nn; n++) {
        double y = b.xyz[3 * (size_t)n + 1] / H;
        vel[3 * (size_t)n] = 6 * U * y * (1 - y);
        T0[n] = 300;
    }
    ThermalFace *faces = malloc((size_t)(2 * nx + 2 * ny + 4) * sizeof(ThermalFace));
    int nf = side_faces(&b, F_XM, THERMAL_OPEN, 0, 300, faces, 0);
    nf = side_faces(&b, F_XP, THERMAL_OPEN, 0, 300, faces, nf);
    nf = side_faces(&b, F_YM, THERMAL_FLUX, q, 0, faces, nf);
    ThermalModel m = {.nnodes = b.nn, .nelems = b.ne, .xyz = b.xyz, .conn = b.conn, .nmat = 1, .mat = &mt.mat, .nfaces = nf, .faces = faces, .velocity = vel};
    ThermalOptions o = {.theta = 1, .pcg_tol = 1e-13};
    ThermalSolver *s = thermal_create(&m, &o, err, sizeof err);
    ThermalStepStats st;
    double nu = NAN;
    if (s && thermal_step(s, &m, T0, T1, 0, 0, &st, err, sizeof err)) {
        int i = 20 * ny / 2; /* x = 20 H */
        double tw = 0.5 * (T1[bn(&b, i, 0, 0)] + T1[bn(&b, i, 0, 1)]), num = 0, den = 0;
        for (int j = 0; j < ny; j++) {
            double hy = H / ny;
            double u0 = vel[3 * (size_t)bn(&b, i, j, 0)], u1 = vel[3 * (size_t)bn(&b, i, j + 1, 0)];
            double t0 = 0.5 * (T1[bn(&b, i, j, 0)] + T1[bn(&b, i, j, 1)]), t1 = 0.5 * (T1[bn(&b, i, j + 1, 0)] + T1[bn(&b, i, j + 1, 1)]);
            num += hy * (u0 * t0 / 3 + u0 * t1 / 6 + u1 * t0 / 6 + u1 * t1 / 3); /* exact for linear u and T */
            den += hy * 0.5 * (u0 + u1);
        }
        double tb = num / den;
        nu = q * 2 * H / (k * (tw - tb));
    } else {
        printf("  channel with %d elements across failed: %s\n", ny, err);
    }
    thermal_free(s);
    free(vel), free(T0), free(T1), free(faces);
    box_free(&b);
    return nu;
}

static void test_nusselt(void) {
    printf("== E2e parallel-plate channel: fully developed Nusselt number (exact 5.385)\n");
    const int NY[3] = {8, 16, 32};
    double nu[3];
    for (int i = 0; i < 3; i++) {
        nu[i] = run_channel(NY[i]);
        printf("  %2d elements across: Nu = %.5f (%+.3f %%)\n", NY[i], nu[i], 100 * (nu[i] / 5.385 - 1));
    }
    double p = log2(fabs(nu[0] - nu[1]) / fabs(nu[1] - nu[2]));
    double pe = isfinite(p) && p > 0.5 ? p : 2;
    double ext = nu[2] + (nu[2] - nu[1]) / (pow(2, pe) - 1);
    printf("  observed order %.2f; extrapolated Nu = %.5f (%+.3f %%)\n", p, ext, 100 * (ext / 5.385 - 1));
    CHECK(fabs(nu[2] / 5.385 - 1) <= 0.01, "finest mesh within 1 %% (%.5f)", nu[2]);
    CHECK(fabs(ext / 5.385 - 1) <= 0.005, "extrapolated value within 0.5 %% (%.5f)", ext);
}

/* ---- E3: lattice Boltzmann flow ------------------------------------------------------------------------------------- */

#include <time.h>
static double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* plane channel: no-slip plates at y = 0 and y = H, periodic in z, Re_H = 10 */
static FlowSpec channel_spec(int H, int Lcells, ThreadPool *pool) {
    FlowSpec sp = {0};
    sp.nx = Lcells, sp.ny = H, sp.nz = 4;
    sp.dx = 1e-3;
    sp.viscosity = 1.5e-5;
    sp.inlet_velocity = 10 * sp.viscosity / (H * sp.dx);
    sp.wall[0] = sp.wall[1] = FLOW_WALL_NOSLIP;
    sp.wall[2] = sp.wall[3] = FLOW_WALL_PERIODIC;
    sp.pool = pool;
    return sp;
}

/* profile deviation from the parabola with the measured flow rate, and the flow rate relative to U H */
static void profile_error(const FlowSpec *sp, const FlowField *f, int i, double *dev, double *rate) {
    int H = sp->ny;
    double q = 0, umax_fit, worst = 0;
    for (int j = 0; j < H; j++) q += f->u[3 * ((size_t)i + (size_t)sp->nx * ((size_t)j + (size_t)H * 1))];
    double mean = q / H;
    umax_fit = 1.5 * mean;
    for (int j = 0; j < H; j++) {
        double y = (j + 0.5) / H, par = umax_fit * 4 * y * (1 - y);
        double u = f->u[3 * ((size_t)i + (size_t)sp->nx * ((size_t)j + (size_t)H * 1))];
        worst = fmax(worst, fabs(u - par));
    }
    *dev = worst / umax_fit;
    *rate = mean / sp->inlet_velocity;
}

static void test_flow(void) {
    char err[768];
    ThreadPool *pool = pool_create(4);
    printf("== E3a plane Poiseuille flow from the lattice Boltzmann solver (Re_H = 10)\n");
    double devs[2] = {0, 0};
    FlowField keep = {0};
    FlowSpec keep_spec = {0};
    for (int r = 0; r < 2; r++) {
        int H = r == 0 ? 16 : 32;
        FlowSpec sp = channel_spec(H, 12 * H, pool);
        FlowField f;
        double w0 = wall_s();
        bool ok = flow_solve(&sp, &f, err, sizeof err);
        double secs = wall_s() - w0;
        if (!ok) {
            printf("  H = %d cells: %s\n", H, err);
            CHECK(false, "the H = %d channel reaches a steady state", H);
            continue;
        }
        double dev, rate;
        profile_error(&sp, &f, 8 * H, &dev, &rate);
        devs[r] = dev;
        printf("  H = %2d cells: tau %.3f, u_lb %.4f, %ld steps (%.3g s simulated, %.1f s wall, %.0f MLUPS), change %.2e, density %.5f..%.5f\n", H, f.tau,
               f.u_lattice, f.steps, f.physical_time, secs, (double)f.steps * sp.nx * sp.ny * sp.nz / secs / 1e6, f.change, f.density_min, f.density_max);
        printf("            profile deviation %.3f %% of the centreline, flow rate %.4f of U H\n", 100 * dev, rate);
        CHECK(f.converged, "H = %d converges to the steady tolerance", H);
        CHECK(dev <= (H == 16 ? 0.015 : 0.005), "H = %d profile within %.1f %% of the parabola (%.3f %%)", H, H == 16 ? 1.5 : 0.5, 100 * dev);
        CHECK(fabs(rate - 1) <= 0.03, "H = %d flow rate within 3 %% of U H (%.4f)", H, rate);
        if (H == 16) keep = f, keep_spec = sp;
        else flow_field_free(&f);
    }
    printf("  deviation ratio 16 -> 32: %.2f\n", devs[0] / devs[1]);

    printf("== E3b parameter policy\n");
    {
        FlowSpec sp = channel_spec(16, 64, pool);
        sp.inlet_velocity = 10; /* air at 1 mm cells: Re_c = 667 */
        double tau, u, rc;
        bool ok = flow_parameters(&sp, &tau, &u, &rc, err, sizeof err);
        printf("  %s\n", ok ? "accepted (unexpected)" : err);
        CHECK(!ok && strstr(err, "cell Reynolds number") && strstr(err, "largest admissible inlet velocity"), "a too-fast flow is refused with its remedy");
    }

    printf("== E3c nodal mapping\n");
    if (keep.u) {
        const FlowSpec *sp = &keep_spec;
        int nx = sp->nx, ny = sp->ny, nz = sp->nz;
        size_t nn = (size_t)(nx + 1) * (size_t)(ny + 1) * (size_t)(nz + 1);
        double *vel = malloc(3 * nn * sizeof(double));
        FlowMapStats ms = {0};
        bool ok = flow_nodal_velocity(sp, &keep, vel, &ms, err, sizeof err);
        double smin = ms.scale_min, smax = ms.scale_max;
        double wall_max = 0, worst_plane = 0, inlet = 0, area = sp->dx * sp->dx * ny * nz;
        for (int i = 0; ok && i <= nx; i++) {
            double q = 0;
            for (int k = 0; k < nz; k++)
                for (int j = 0; j < ny; j++) {
                    double s4 = 0;
                    for (int dk = 0; dk <= 1; dk++)
                        for (int dj = 0; dj <= 1; dj++) s4 += 0.25 * vel[3 * ((size_t)i + (size_t)(nx + 1) * ((size_t)(j + dj) + (size_t)(ny + 1) * (size_t)(k + dk)))];
                    q += sp->dx * sp->dx * s4;
                }
            if (i == 0) inlet = q;
            worst_plane = fmax(worst_plane, fabs(q / (sp->inlet_velocity * area) - 1));
            for (int k = 0; k <= nz; k++)
                for (int j = 0; j <= ny; j += ny)
                    for (int d = 0; d < 3; d++) wall_max = fmax(wall_max, fabs(vel[3 * ((size_t)i + (size_t)(nx + 1) * ((size_t)j + (size_t)(ny + 1) * (size_t)k)) + d]));
        }
        printf("  per-section scale factors %.5f .. %.5f; largest no-slip nodal speed %.3g m/s; inlet flux error %.2e; worst cross-section flux error %.3f %%\n", smin, smax, wall_max,
               fabs(inlet / (sp->inlet_velocity * area) - 1), 100 * worst_plane);
        CHECK(ok && wall_max == 0, "zero velocity at every no-slip node");
        CHECK(ok && fabs(inlet / (sp->inlet_velocity * area) - 1) <= 1e-12, "inlet flux equals U A after scaling");
        CHECK(ok && worst_plane <= 0.02, "every cross-section carries U A within 2 %% (%.3f %%)", 100 * worst_plane);
        free(vel);
        flow_field_free(&keep);
    }
    pool_destroy(pool);
}

/* E3d: the parallel-plate Nusselt problem with the lattice Boltzmann velocity */
static void test_flow_nusselt(void) {
    char err[768];
    ThreadPool *pool = pool_create(4);
    printf("== E3d lattice Boltzmann velocity in the channel energy equation (H = 16 cells, L = 30 H)\n");
    int H = 16;
    FlowSpec sp = channel_spec(H, 30 * H, pool);
    FlowField f;
    double w0 = wall_s();
    if (!flow_solve(&sp, &f, err, sizeof err)) {
        printf("  flow failed: %s\n", err);
        CHECK(false, "the channel flow for E3d reaches a steady state");
        pool_destroy(pool);
        return;
    }
    printf("  flow: %ld steps, %.1f s wall\n", f.steps, wall_s() - w0);
    int nx = sp.nx, ny = sp.ny, nz = sp.nz;
    Box b;
    box_init(&b, nx, ny, nz, nx * sp.dx, ny * sp.dx, nz * sp.dx);
    double *vel = malloc(3 * (size_t)b.nn * sizeof(double));
    FlowMapStats ms = {0};
    bool ok = flow_nodal_velocity(&sp, &f, vel, &ms, err, sizeof err);
    double smin = ms.scale_min, smax = ms.scale_max;
    /* thermal: Pe = U D_h / alpha = 50 with water-like rho cp */
    double Hm = ny * sp.dx, U = sp.inlet_velocity, rc = 4e6, alpha = U * 2 * Hm / 50, k = alpha * rc, q = 100;
    Mat1 mt;
    mat1(&mt, k, 1000, 4000);
    double *T0 = malloc((size_t)b.nn * sizeof(double)), *T1 = malloc((size_t)b.nn * sizeof(double));
    for (int n = 0; n < b.nn; n++) T0[n] = 300;
    ThermalFace *faces = malloc((size_t)(2 * ny * nz + nx * nz + 4) * sizeof(ThermalFace));
    int nf = side_faces(&b, F_XM, THERMAL_OPEN, 0, 300, faces, 0);
    nf = side_faces(&b, F_XP, THERMAL_OPEN, 0, 300, faces, nf);
    nf = side_faces(&b, F_YM, THERMAL_FLUX, q, 0, faces, nf);
    ThermalModel m = {.nnodes = b.nn, .nelems = b.ne, .xyz = b.xyz, .conn = b.conn, .nmat = 1, .mat = &mt.mat, .nfaces = nf, .faces = faces, .velocity = vel};
    ThermalOptions o = {.theta = 1, .pcg_tol = 1e-12, .pool = pool};
    ThermalSolver *s = ok ? thermal_create(&m, &o, err, sizeof err) : NULL;
    ThermalStepStats st;
    double nu = NAN;
    if (s && thermal_step(s, &m, T0, T1, 0, 0, &st, err, sizeof err)) {
        int i = 20 * H;
        double tw = 0, num = 0, den = 0;
        for (int kk = 0; kk <= nz; kk++) tw += T1[bn(&b, i, 0, kk)] / (nz + 1);
        for (int j = 0; j < ny; j++) {
            double hy = sp.dx, u0 = 0, u1 = 0, t0 = 0, t1 = 0;
            for (int kk = 0; kk <= nz; kk++) {
                u0 += vel[3 * (size_t)bn(&b, i, j, kk)] / (nz + 1), u1 += vel[3 * (size_t)bn(&b, i, j + 1, kk)] / (nz + 1);
                t0 += T1[bn(&b, i, j, kk)] / (nz + 1), t1 += T1[bn(&b, i, j + 1, kk)] / (nz + 1);
            }
            num += hy * (u0 * t0 / 3 + u0 * t1 / 6 + u1 * t0 / 6 + u1 * t1 / 3);
            den += hy * 0.5 * (u0 + u1);
        }
        nu = q * 2 * Hm / (k * (tw - num / den));
        printf("  Nu = %.4f (%+.2f %%), section scale factors %.5f .. %.5f, unexplained advective energy %.2e of the outflow\n", nu, 100 * (nu / 5.385 - 1), smin, smax,
               fabs(st.divergence_energy) / fmax(fabs(st.enthalpy_outflow), 1e-300));
    } else {
        printf("  energy solve failed: %s\n", err);
    }
    CHECK(fabs(nu / 5.385 - 1) <= 0.03, "Nusselt number within 3 %% of 5.385 (%.4f)", nu);
    thermal_free(s);
    flow_field_free(&f);
    free(vel), free(T0), free(T1), free(faces);
    box_free(&b);
    pool_destroy(pool);
}

/* ---- E4: conjugate heat transfer ------------------------------------------------------------------------------------ */

enum { CHT_NX = 80, CHT_NS = 4, CHT_NF = 8, CHT_NZ = 4, CHT_NOUT = 10 };
static const double CHT_H = 0.5e-3, CHT_U = 0.01, CHT_Q = 1e6, CHT_TEND = 20;

typedef struct {
    double frames[CHT_NOUT][(CHT_NX + 1) * (CHT_NS + CHT_NF + 1) * (CHT_NZ + 1)];
    int stored;
    long steps;
    double worst_heat, worst_continuity;
    double stored_energy, carried, source_energy;
    bool ok;
    char err[1024];
} ChtResult;

/* nodes (i, j, k) of the full box; j = 0..NS is the slab, NS..NS+NF the channel */
static int cht_node(int i, int j, int k) { return i + (CHT_NX + 1) * (j + (CHT_NS + CHT_NF + 1) * k); }

static double cht_velocity(int j) {
    if (j <= CHT_NS) return 0;
    double y = (double)(j - CHT_NS) / CHT_NF;
    return 6 * CHT_U * y * (1 - y);
}

typedef struct {
    Box b;
    int *elem_mat;
    unsigned char *advect, *fixed;
    double *vel, *fixT, *src, *power, *T0;
    ThermalFace *faces;
    int nf;
    Mat1 mats[2];
    ThermalMaterial tm[2];
    ThermalModel m;
} ChtModel;

/* part: 0 monolithic, 1 slab only, 2 channel only (local boxes keep the global node order within their layers) */
static void cht_build(ChtModel *c, int part) {
    memset(c, 0, sizeof *c);
    int j0 = part == 2 ? CHT_NS : 0, j1 = part == 1 ? CHT_NS : CHT_NS + CHT_NF, ny = j1 - j0;
    box_init(&c->b, CHT_NX, ny, CHT_NZ, CHT_NX * CHT_H, ny * CHT_H, CHT_NZ * CHT_H);
    for (int n = 0; n < c->b.nn; n++) c->b.xyz[3 * (size_t)n + 1] += j0 * CHT_H;
    mat1(&c->mats[0], 200, 2700, 900);
    mat1(&c->mats[1], 0.6, 1000, 4180);
    c->tm[0] = c->mats[0].mat, c->tm[1] = c->mats[1].mat;
    c->elem_mat = calloc((size_t)c->b.ne, sizeof(int));
    c->advect = calloc((size_t)c->b.ne, 1);
    c->src = calloc((size_t)c->b.ne, sizeof(double));
    c->fixed = calloc((size_t)c->b.nn, 1);
    c->fixT = calloc((size_t)c->b.nn, sizeof(double));
    c->power = calloc((size_t)c->b.nn, sizeof(double));
    c->vel = calloc(3 * (size_t)c->b.nn, sizeof(double));
    c->T0 = malloc((size_t)c->b.nn * sizeof(double));
    c->faces = malloc(2 * (size_t)(ny * CHT_NZ) * sizeof(ThermalFace));
    for (int k = 0; k < CHT_NZ; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < CHT_NX; i++) {
                int e = be_(&c->b, i, j, k);
                bool fluid = j + j0 >= CHT_NS;
                c->elem_mat[e] = fluid;
                c->advect[e] = fluid;
                c->src[e] = fluid ? 0 : CHT_Q;
                if (fluid && i == 0) c->faces[c->nf++] = (ThermalFace){e, F_XM, THERMAL_OPEN, 0, 300};
                if (fluid && i == CHT_NX - 1) c->faces[c->nf++] = (ThermalFace){e, F_XP, THERMAL_OPEN, 0, 300};
            }
    for (int k = 0; k <= CHT_NZ; k++)
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= CHT_NX; i++) {
                int n = bn(&c->b, i, j, k);
                c->vel[3 * (size_t)n] = cht_velocity(j + j0);
                c->T0[n] = 300;
                c->fixT[n] = 300;
                if (part == 2 && j == 0) c->fixed[n] = 1; /* the channel holds the interface at the slab's temperature */
            }
    c->m = (ThermalModel){.nnodes = c->b.nn, .nelems = c->b.ne, .xyz = c->b.xyz, .conn = c->b.conn, .elem_mat = c->elem_mat, .nmat = 2, .mat = c->tm,
                          .fixed = c->fixed, .fixed_T = c->fixT, .elem_source = c->src, .nfaces = c->nf, .faces = c->faces,
                          .node_power = part == 1 ? c->power : NULL, .velocity = part == 1 ? NULL : c->vel, .advect = c->advect};
}

static void cht_free(ChtModel *c) {
    free(c->elem_mat), free(c->advect), free(c->fixed), free(c->vel), free(c->fixT), free(c->src), free(c->power), free(c->T0), free(c->faces);
    box_free(&c->b);
}

static void cht_events(EventSchedule *ev) {
    events_init(ev, 0, CHT_TEND);
    for (int i = 1; i <= CHT_NOUT; i++) events_add(ev, CHT_TEND * i / CHT_NOUT, EVENT_OUTPUT);
    events_finalize(ev);
}

static ThermalIntegratorSettings cht_isettings(bool adaptive) {
    ThermalIntegratorSettings s = {0};
    s.mode = adaptive ? THERMAL_STEPPING_ADAPTIVE : THERMAL_STEPPING_FIXED;
    s.t_end = CHT_TEND, s.dt_fixed = 0.5;
    s.tol.relative = 1e-4, s.tol.temperature = 1e-3;
    s.control.dt_initial = 0.05;
    s.T_reference = 300;
    return s;
}

static void cht_monolithic(bool adaptive, ChtResult *r) {
    memset(r, 0, sizeof *r);
    ChtModel c;
    cht_build(&c, 0);
    ThermalOptions o = {.theta = 1, .pcg_tol = 1e-13, .picard_tol = 1e-12};
    ThermalSolver *s = thermal_create(&c.m, &o, r->err, sizeof r->err);
    EventSchedule ev;
    cht_events(&ev);
    ThermalIntegratorSettings is = cht_isettings(adaptive);
    ThermalIntegrator *ti = s ? tint_create(s, &c.m, &is, &ev, c.T0, r->err, sizeof r->err) : NULL;
    r->ok = ti != NULL;
    ThermalStepReport rep;
    ThermalIntegratorStatus st = TINT_FINISHED;
    while (r->ok && (st = tint_step(ti, &rep, r->err, sizeof r->err)) == TINT_STEPPED) {
        r->steps++;
        if ((rep.event_kinds & EVENT_OUTPUT) && r->stored < CHT_NOUT) memcpy(r->frames[r->stored++], tint_temperature(ti), (size_t)c.b.nn * sizeof(double));
    }
    r->ok = r->ok && st == TINT_FINISHED;
    if (ti) {
        const ThermalBudget *b = tint_budget(ti);
        r->stored_energy = b->stored, r->carried = b->enthalpy_outflow - b->enthalpy_inflow, r->source_energy = b->source;
    }
    events_free(&ev);
    tint_free(ti), thermal_free(s);
    cht_free(&c);
}

static void cht_partitioned(bool adaptive, ChtResult *r) {
    memset(r, 0, sizeof *r);
    ChtModel cs, cf;
    cht_build(&cs, 1);
    cht_build(&cf, 2);
    ThermalOptions o = {.theta = 1, .pcg_tol = 1e-13, .picard_tol = 1e-12};
    ThermalSolver *ss = thermal_create(&cs.m, &o, r->err, sizeof r->err), *sf = thermal_create(&cf.m, &o, r->err, sizeof r->err);
    EventSchedule ev;
    cht_events(&ev);
    ThermalIntegratorSettings is = cht_isettings(adaptive);
    ThermalIntegrator *tis = ss ? tint_create(ss, &cs.m, &is, &ev, cs.T0, r->err, sizeof r->err) : NULL;
    ThermalIntegrator *tif = sf ? tint_create(sf, &cf.m, &is, &ev, cf.T0, r->err, sizeof r->err) : NULL;
    int ni = (CHT_NX + 1) * (CHT_NZ + 1);
    int *is_nodes = malloc((size_t)ni * sizeof(int)), *if_nodes = malloc((size_t)ni * sizeof(int));
    for (int k = 0, q = 0; k <= CHT_NZ; k++)
        for (int i = 0; i <= CHT_NX; i++, q++) is_nodes[q] = bn(&cs.b, i, CHT_NS, k), if_nodes[q] = bn(&cf.b, i, 0, k);
    ThermalParticipant fluid = {.name = "channel", .ti = tif, .m = &cf.m, .fixed = cf.fixed, .fixed_T = cf.fixT, .node_power = cf.power, .ninterface = ni,
                                .interface_nodes = if_nodes, .estimate = adaptive};
    ThermalParticipant solid = {.name = "slab", .ti = tis, .m = &cs.m, .fixed = cs.fixed, .fixed_T = cs.fixT, .node_power = cs.power, .ninterface = ni,
                                .interface_nodes = is_nodes, .estimate = adaptive};
    OrchParticipant parts[2];
    r->ok = tis && tif && thermal_participant_bind(&fluid, &parts[0], r->err, sizeof r->err) &&
            thermal_participant_bind(&solid, &parts[1], r->err, sizeof r->err);
    OrchCoupling coup[2] = {{1, 0, "interface_temperature", "interface_temperature", false}, {0, 1, "interface_heat_out", "interface_heat_in", true}};
    OrchSettings os = {.mode = is.mode, .t_end = CHT_TEND, .dt_fixed = 0.5, .control = is.control, .coupling_relative = 1e-12,
                       .max_coupling_iterations = 60, .aitken = true, .relaxation = 0.5};
    Orchestrator *orch = r->ok ? orch_create(&os, &ev, parts, 2, coup, 2, r->err, sizeof r->err) : NULL;
    r->ok = orch && orch_initialize(orch, r->err, sizeof r->err);
    OrchStepReport rep;
    OrchStatus st = ORCH_FINISHED;
    while (r->ok && (st = orch_step(orch, &rep, r->err, sizeof r->err)) == ORCH_STEPPED) {
        r->steps++;
        for (int bl = 0; bl < (fluid.trial.estimated ? solid.blocks : 1); bl++) {
            const double *e = tint_trial_stage_node_energy(tif, bl);
            double out = 0, in = 0;
            for (int q = 0; q < ni; q++) out += e ? -e[q] : NAN, in += solid.heat_in[bl * ni + q];
            double big = fmax(fabs(out), fabs(in));
            r->worst_heat = fmax(r->worst_heat, big > 0 ? fabs(out - in) / big : (isfinite(out) ? 0 : INFINITY));
        }
        if ((rep.event_kinds & EVENT_OUTPUT) && r->stored < CHT_NOUT) {
            const double *Ts = tint_temperature(tis), *Tf = tint_temperature(tif);
            for (int k = 0; k <= CHT_NZ; k++)
                for (int j = 0; j <= CHT_NS + CHT_NF; j++)
                    for (int i = 0; i <= CHT_NX; i++)
                        r->frames[r->stored][cht_node(i, j, k)] = j <= CHT_NS ? Ts[bn(&cs.b, i, j, k)] : Tf[bn(&cf.b, i, j - CHT_NS, k)];
            for (int q = 0; q < ni; q++) r->worst_continuity = fmax(r->worst_continuity, fabs(Ts[is_nodes[q]] - Tf[if_nodes[q]]));
            r->stored++;
        }
    }
    r->ok = r->ok && st == ORCH_FINISHED;
    if (tis && tif) {
        const ThermalBudget *bs = tint_budget(tis), *bf = tint_budget(tif);
        double vol_source = CHT_Q * CHT_NX * CHT_NS * CHT_NZ * CHT_H * CHT_H * CHT_H * CHT_TEND;
        r->stored_energy = bs->stored + bf->stored, r->carried = bf->enthalpy_outflow - bf->enthalpy_inflow, r->source_energy = vol_source;
    }
    orch_free(orch);
    thermal_participant_release(&fluid), thermal_participant_release(&solid);
    events_free(&ev);
    tint_free(tis), tint_free(tif), thermal_free(ss), thermal_free(sf);
    free(is_nodes), free(if_nodes);
    cht_free(&cs), cht_free(&cf);
}

static double cht_diff(const ChtResult *a, const ChtResult *b) {
    if (a->stored != b->stored || a->stored == 0) return INFINITY;
    double d = 0;
    int nn = (CHT_NX + 1) * (CHT_NS + CHT_NF + 1) * (CHT_NZ + 1);
    for (int f = 0; f < a->stored; f++)
        for (int n = 0; n < nn; n++) d = fmax(d, fabs(a->frames[f][n] - b->frames[f][n]));
    return d;
}

static void test_cht(void) {
    static ChtResult mono, part;
    printf("== E4a conjugate heat transfer, fixed steps: partitioned against monolithic\n");
    double w0 = wall_s();
    cht_monolithic(false, &mono);
    double w1 = wall_s();
    cht_partitioned(false, &part);
    double w2 = wall_s();
    double d = cht_diff(&mono, &part);
    printf("  %ld steps; max difference %.3e K; heat mismatch %.2e; interface continuity %.2e K; %.2f s monolithic, %.2f s partitioned\n", part.steps, d,
           part.worst_heat, part.worst_continuity, w1 - w0, w2 - w1);
    if (!mono.ok || !part.ok) printf("  %s | %s\n", mono.err, part.err);
    CHECK(mono.ok && part.ok && d <= 1e-6, "partitioned equals monolithic within 1e-6 K (%.3e)", d);
    CHECK(part.worst_heat <= 1e-12, "heat leaving the fluid equals heat entering the solid per solve (%.2e)", part.worst_heat);
    printf("== E4c conservation\n");
    double closure = fabs(part.source_energy - part.stored_energy - part.carried) / part.source_energy;
    printf("  source %.6g J = stored %.6g J + carried away %.6g J; closure %.2e (monolithic %.2e)\n", part.source_energy, part.stored_energy, part.carried,
           closure, fabs(mono.source_energy - mono.stored_energy - mono.carried) / mono.source_energy);
    CHECK(part.worst_continuity <= 1e-7, "temperature continuity at the interface (%.2e K)", part.worst_continuity);
    CHECK(closure <= 1e-9, "source energy = stored + carried away (%.2e)", closure);

    printf("== E4b adaptive: partitioned against monolithic\n");
    w0 = wall_s();
    cht_monolithic(true, &mono);
    w1 = wall_s();
    cht_partitioned(true, &part);
    w2 = wall_s();
    d = cht_diff(&mono, &part);
    printf("  monolithic %ld steps (%.2f s), partitioned %ld steps (%.2f s); max difference %.3e K\n", mono.steps, w1 - w0, part.steps, w2 - w1, d);
    if (!mono.ok || !part.ok) printf("  %s | %s\n", mono.err, part.err);
    CHECK(mono.ok && part.ok && mono.steps == part.steps, "the same number of accepted steps (%ld, %ld)", mono.steps, part.steps);
    CHECK(d <= 1e-6, "partitioned equals monolithic within 1e-6 K (%.3e)", d);
}

int main(void) {
    if (getenv("ADVTEST_ONLY_CHT")) {
        test_cht();
        printf("\n%s: %d passed, %d failed\n", g_fail ? "ADVECTION TESTS FAILED" : "ALL ADVECTION TESTS PASSED", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    test_solver();
    test_floor();
    test_advection();
    test_pulse();
    test_energy();
    test_nusselt();
    test_flow();
    test_flow_nusselt();
    test_cht();
    printf("\n%s: %d passed, %d failed\n", g_fail ? "ADVECTION TESTS FAILED" : "ALL ADVECTION TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
