/* poissontest - verification of the multigrid Poisson solver (src/lab/mg/mg3d.h).
 *
 * Criteria written on 2026-09-26, before the first run:
 *   Q1 u = sin(pi x) cos(pi y) cos(pi z / 2) on the unit cube, Dirichlet on the x faces and at z = 1, Neumann on the y
 *      faces and at z = 0 (the solution meets them), lap u = -(9/4) pi^2 u: the largest error against u at 32 and 64 cells
 *      a side, falling by a factor between 3.5 and 4.5 (second order), and below 1e-3 at 64.
 *   Q2 the residual falls by 1e-10 within 15 V-cycles at 64 cells, and at 128.
 *   Q3 every face Neumann (singular): u = cos(pi x) cos(pi y) cos(pi z), solved to 1e-10, the error of its mean-free
 *      part falling by 3.5 to 4.5 from 32 to 64.
 *   First run: all pass (error ratios 3.993 and 3.991, 11 V-cycles at 64 and 128).
 *   Added 2026-09-26 with variable coefficients, before their first run:
 *   Q4 div(beta grad u) = f with beta = 1 + x / 2 on the faces, u as in Q1: error ratio 32 -> 64 between 3.5 and 4.5, and
 *      the residual down 1e-10 within 20 V-cycles at 64.
 *   Q5 a sevenfold jump (beta from 1 to 7 across a tanh layer two cells thick at x = 1/2, as across a flame): converged to
 *      1e-10 within 30 V-cycles at 64.
 *   Q4 and Q5's first run: pass (ratio 3.997, 11 cycles; 13 cycles).
 *   Since 2026-09-26 the solve is conjugate gradients preconditioned by one V-cycle an iteration (multigrid alone
 *   stalled in a room joined to the outside by a doorway, docs/lab/rooms.md); the counts printed are iterations, one
 *   V-cycle each, the criteria unchanged: 8, 8 and 9 (Q1, Q2), 8 and 9 (Q3), 8 (Q4), 10 (Q5), all pass. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/lab/mg/mg3d.h"

static int failures;
static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    failures += !ok;
}

static int coef_mode = 0; /* 0 none, 1 smooth, 2 jump */
static double beta_at(double x) { return coef_mode == 1 ? 1 + 0.5 * x : 1 + 3 * (1 + tanh((x - 0.5) / 0.03)); }
static double run(int N, bool neumann, int *cycles, double *secs, ThreadPool *pool) {
    int bc[6] = {MG_DIRICHLET, MG_DIRICHLET, MG_NEUMANN, MG_NEUMANN, MG_NEUMANN, MG_DIRICHLET};
    if (neumann)
        for (int f = 0; f < 6; f++) bc[f] = MG_NEUMANN;
    char err[128];
    double h = 1.0 / N;
    Mg3D *M = mg3d_create(N, N, N, h, bc, err, sizeof err);
    size_t n = (size_t)N * N * N;
    double *x = calloc(n, sizeof(double)), *f = malloc(n * sizeof(double)), *u = malloc(n * sizeof(double));
    if (coef_mode) {
        size_t nf = (size_t)(N + 1) * N * N;
        double *bx = malloc(nf * 8), *by = malloc(nf * 8), *bz = malloc(nf * 8);
        for (int k = 0; k < N; k++)
            for (int j = 0; j < N; j++)
                for (int i = 0; i <= N; i++) bx[(size_t)i + (size_t)(N + 1) * ((size_t)j + (size_t)N * k)] = beta_at(i * h);
        for (int k = 0; k < N; k++)
            for (int j = 0; j <= N; j++)
                for (int i = 0; i < N; i++) by[(size_t)i + (size_t)N * ((size_t)j + (size_t)(N + 1) * k)] = beta_at((i + 0.5) * h);
        for (int k = 0; k <= N; k++)
            for (int j = 0; j < N; j++)
                for (int i = 0; i < N; i++) bz[(size_t)i + (size_t)N * ((size_t)j + (size_t)N * k)] = beta_at((i + 0.5) * h);
        mg3d_set_coefficients(M, bx, by, bz);
        free(bx), free(by), free(bz);
    }
    for (int k = 0; k < N; k++)
        for (int j = 0; j < N; j++)
            for (int i = 0; i < N; i++) {
                double X = (i + 0.5) * h, Y = (j + 0.5) * h, Z = (k + 0.5) * h, v, lam;
                if (neumann) v = cos(M_PI * X) * cos(M_PI * Y) * cos(M_PI * Z), lam = 3 * M_PI * M_PI;
                else v = sin(M_PI * X) * cos(M_PI * Y) * cos(M_PI * Z / 2), lam = 2.25 * M_PI * M_PI;
                size_t c = (size_t)i + (size_t)N * ((size_t)j + (size_t)N * k);
                u[c] = v, f[c] = -lam * v;
                if (coef_mode == 1) f[c] = (1 + 0.5 * X) * (-lam * v) + 0.5 * M_PI * cos(M_PI * X) * cos(M_PI * Y) * cos(M_PI * Z / 2);
                if (coef_mode == 2) f[c] = v; /* any right side: Q5 checks convergence only */
            }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    *cycles = mg3d_solve(M, x, f, 1e-10, 40, pool);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *secs = (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);
    double um = 0, err_max = 0;
    if (neumann) {
        for (size_t c = 0; c < n; c++) um += u[c];
        um /= n;
    }
    for (size_t c = 0; c < n; c++) err_max = fmax(err_max, fabs(x[c] - (u[c] - um)));
    printf("  %d^3 cells, %d levels: %d V-cycles (residual %.2e), %.3f s; largest error %.3e\n", N, mg3d_levels(M), *cycles, mg3d_last_residual(M),
           *secs, err_max);
    free(x), free(f), free(u);
    mg3d_free(M);
    return err_max;
}

int main(void) {
    ThreadPool *pool = pool_create(cpu_perf_count());
    int c32, c64, c128;
    double s;
    printf("== Q1, Q2: mixed faces\n");
    double e32 = run(32, false, &c32, &s, pool), e64 = run(64, false, &c64, &s, pool);
    run(128, false, &c128, &s, pool);
    printf("  error ratio 32 -> 64: %.3f\n", e32 / e64);
    verdict(e32 / e64 > 3.5 && e32 / e64 < 4.5 && e64 < 1e-3, "Q1");
    verdict(c64 > 0 && c64 <= 15 && c128 > 0 && c128 <= 15, "Q2");
    printf("== Q3: every face Neumann\n");
    double n32 = run(32, true, &c32, &s, pool), n64 = run(64, true, &c64, &s, pool);
    printf("  error ratio 32 -> 64: %.3f\n", n32 / n64);
    verdict(c32 > 0 && c64 > 0 && n32 / n64 > 3.5 && n32 / n64 < 4.5, "Q3");
    printf("== Q4: div(beta grad u) = f, beta = 1 + x / 2\n");
    coef_mode = 1;
    double b32 = run(32, false, &c32, &s, pool), b64 = run(64, false, &c64, &s, pool);
    printf("  error ratio 32 -> 64: %.3f\n", b32 / b64);
    verdict(b32 / b64 > 3.5 && b32 / b64 < 4.5 && c64 > 0 && c64 <= 20, "Q4");
    printf("== Q5: a sevenfold jump in beta\n");
    coef_mode = 2;
    run(64, false, &c64, &s, pool);
    verdict(c64 > 0 && c64 <= 30, "Q5");
    pool_destroy(pool);
    printf(failures ? "poissontest: %d FAILED\n" : "poissontest: all passed\n", failures);
    return failures ? 1 : 0;
}
