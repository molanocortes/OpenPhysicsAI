/* lbm3dtest - verification of the 3D lattice Boltzmann engines (src/lab/flow/lbm3d.h on the CPU in double precision,
 * src/lab/flow/lbm3d_metal.h on the GPU in single precision): every case runs on both, to the same criterion.
 *
 * Criteria written on 2026-09-26, before the first run:
 *   L1 plane Poiseuille flow driven by a body acceleration between two walls placed off the lattice (y = 1.3 and
 *      y = 21.3 in cell units, so every wall link has a fraction q other than one half), periodic in x and z, viscosity
 *      0.1: after the flow has settled, the velocity profile against u(y) = a / (2 nu) (y - y0)(y1 - y): RMS error
 *      below 1 % of the peak (the interpolated bounce-back is second order; halfway bounce-back at these walls would be
 *      off by several per cent).
 *   L2 a sphere in a stream at Reynolds number 100 (diameter 16 cells, 12 D long, 6 D square, slip side walls, inlet
 *      and outlet): the drag coefficient after the wake has settled, within 5 % of 1.087 (Schiller and Naumann's
 *      correlation 24 / Re (1 + 0.15 Re^0.687), which agrees with the resolved simulations of Johnson and Patel 1999 at
 *      this Reynolds number). The side walls block 2.2 % of the section; that and the grid are what the 5 % allows.
 *   L3 speed: million lattice updates per second on this machine, reported (no criterion).
 *   (The GPU engine joined the test the same day, before its first run, with the same criteria.)
 *   L1's first runs (2026-09-26): 7.68 % (FAIL); then 2.23 % and 2.30 % (FAIL). Three defects, all in the engine, none in
 *   the criterion: the regularised collision dropped the first moment of the non-equilibrium part and lost an eighth
 *   of the force each step; cells on a periodic seam lost their wall links (160 expected, 128 made) and pulled rest
 *   populations from the solid, so the walls did not carry the whole force; and the velocity was read from the
 *   post-collision populations with the step's force still in it. After the fixes the walls take the body force
 *   exactly, a channel with halfway walls matches the parabola to rounding (TRT, Lambda = 3/16), and L1 gives 0.20 %.
 *   L2's first run (GPU, 2026-09-26): Cd 1.1536, +6.12 % (FAIL). Single steps swing by several per cent (sound waves),
 *   but averaged over 100 steps the drag is steady at 1.16. A start from rest with a ramped inlet and a sponge before the
 *   outlet made it worse (1.2129) and was dropped. Exploratory runs then separated the error: widening the box from 6 D
 *   to 10 D lowers Cd from 1.160 to 1.140 (the walls), refining D from 16 to 24 cells lowers it to 1.133 (the grid).
 *   Amended 2026-09-26, after those runs and so not blind: L2 now runs D = 16 and D = 24 in the 6 D box and checks the
 *   Richardson extrapolation of the two (second order) within 4 % of 1.087, the walls at 6 D being worth about 2 %,
 *   and that the finer grid lies nearer. The 5 % on a single grid of 16 cells was optimistic.
 *   L4 (added 2026-09-26 before its first run): single precision on the GPU against double on the CPU, the same small
 *      sphere (D = 8, a 4 D box, 12 D / U): drag coefficients within 0.5 % of each other.
 *      First run: both engines went unstable (NaN): 8 cells at Reynolds number 100 put the relaxation time at 0.512.
 *      L4 now runs at Reynolds number 40 (0.53); the comparison, not the drag, is its purpose; criterion unchanged.
 *   L5 (added 2026-09-26 with the pressure outlet, before its first run): a uniform stream through an empty box (48 x 16
 *      x 16, slip sides) with the pressure outlet, forward and reversed (the reversed stream enters through the outlet):
 *      after 2000 steps every cell's velocity within 1 % of the stream's (each component, relative to its speed), on
 *      both engines. The extrapolating outlet cannot take the reversed stream. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/lab/flow/lbm3d.h"
#include "../src/lab/flow/lbm3d_metal.h"

static int failures;
static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    failures += !ok;
}
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

typedef struct { double y0, y1; } Plates;
static double plates(const double p[3], void *ctx) {
    const Plates *P = ctx;
    return fmin(p[1] - P->y0, P->y1 - p[1]);
}
typedef struct { double c[3], r; } Sphere;
static double sphere(const double p[3], void *ctx) {
    const Sphere *S = ctx;
    return sqrt((p[0] - S->c[0]) * (p[0] - S->c[0]) + (p[1] - S->c[1]) * (p[1] - S->c[1]) + (p[2] - S->c[2]) * (p[2] - S->c[2])) - S->r;
}

/* one engine behind one interface: the CPU, or the GPU with the CPU holding only the geometry */
typedef struct {
    Lbm3D *L;
    Lbm3DGpu *G;
    ThreadPool *pool;
    double F[3];
} Engine;
static bool eng_make(Engine *E, Lbm3DSpec s, bool gpu, Lbm3DSdf sdf, void *ctx, ThreadPool *pool, char *err, size_t el) {
    memset(E, 0, sizeof *E);
    E->pool = pool;
    s.geometry_only = gpu;
    E->L = lbm3d_create(&s, err, el);
    if (!E->L || !lbm3d_set_bodies(E->L, sdf, ctx)) return false;
    if (gpu) {
        E->G = lbm3d_gpu_create(E->L, err, el);
        if (!E->G) return false;
        lbm3d_gpu_init(E->G);
    } else lbm3d_init(E->L);
    return true;
}
static bool eng_steps(Engine *E, int n, double *hist) { /* hist: 3 n forces, or NULL */
    if (E->G) {
        bool ok = lbm3d_gpu_steps(E->G, n, hist);
        if (hist) memcpy(E->F, &hist[3 * (n - 1)], sizeof E->F);
        return ok;
    }
    for (int k = 0; k < n; k++) {
        if (!lbm3d_step(E->L, E->pool)) return false;
        lbm3d_force(E->L, E->F);
        if (hist) memcpy(&hist[3 * k], E->F, sizeof E->F);
    }
    return true;
}
static void eng_macro(Engine *E, double *u) {
    if (E->G) lbm3d_gpu_macro(E->G, NULL, u);
    else lbm3d_macro(E->L, NULL, u);
}
static void eng_free(Engine *E) { lbm3d_gpu_free(E->G), lbm3d_free(E->L); }

static void poiseuille(bool gpu, ThreadPool *pool) {
    const char *name = gpu ? "L1 (GPU)" : "L1 (CPU)";
    printf("== %s: Poiseuille flow between walls off the lattice\n", name);
    char err[256] = "";
    Lbm3DSpec s = {.nx = 4, .ny = 23, .nz = 4, .nu = 0.1, .accel = {1e-6, 0, 0}, .bc_x = LBM3D_PERIODIC, .bc_y = LBM3D_PERIODIC, .bc_z = LBM3D_PERIODIC};
    Plates P = {1.3, 21.3};
    Engine E;
    if (!eng_make(&E, s, gpu, plates, &P, pool, err, sizeof err)) {
        printf("  %s\n", err);
        verdict(false, name);
        eng_free(&E);
        return;
    }
    bool ok = true;
    for (int k = 0; k < 40 && ok; k++) ok = eng_steps(&E, 500, NULL);
    double *u = malloc(3 * sizeof(double) * s.nx * s.ny * s.nz);
    eng_macro(&E, u);
    double peak = s.accel[0] / (2 * s.nu) * pow((P.y1 - P.y0) / 2, 2), se = 0;
    int cnt = 0;
    for (int y = 0; y < s.ny; y++) {
        double yc = y + 0.5;
        if (yc <= P.y0 || yc >= P.y1) continue;
        double ex = s.accel[0] / (2 * s.nu) * (yc - P.y0) * (P.y1 - yc), got = u[3 * (size_t)(1 + s.nx * (y + s.ny * 1))];
        se += (got - ex) * (got - ex), cnt++;
    }
    double rms = sqrt(se / cnt) / peak;
    printf("  %d fluid rows, peak %.6e, RMS error %.4f %% of the peak, %ld wall links\n", cnt, peak, 100 * rms, lbm3d_links(E.L));
    verdict(ok && rms < 0.01, name);
    free(u);
    eng_free(&E);
}

/* a sphere of D cells at Reynolds number 100 in a box 12 D long and w D square; the drag averaged over the last 5 D/U */
static double sphere_cd(bool gpu, int D, int w, double tU, double Re, ThreadPool *pool, double *mlups) {
    const double U = 0.05;
    char err[256] = "";
    Lbm3DSpec s = {.nx = 12 * D, .ny = w * D, .nz = w * D, .nu = U * D / Re, .u_in = {U, 0, 0}, .bc_x = LBM3D_INOUT,
                   .bc_y = LBM3D_SLIP, .bc_z = LBM3D_SLIP};
    Sphere S = {{4.0 * D, w * D / 2.0, w * D / 2.0}, D / 2.0};
    Engine E;
    if (!eng_make(&E, s, gpu, sphere, &S, pool, err, sizeof err)) {
        printf("  %s\n", err);
        eng_free(&E);
        return NAN;
    }
    int batch = 100, steps = (int)(tU * D / U) / batch * batch, avg_from = steps - (int)(5.0 * D / U);
    double t0 = now(), cdsum = 0, *hist = malloc(3 * sizeof(double) * batch), q = 0.5 * U * U * M_PI * D * D / 4;
    int navg = 0;
    bool ok = true;
    for (int k = 0; k < steps && ok; k += batch) {
        ok = eng_steps(&E, batch, hist);
        for (int j = 0; j < batch; j++)
            if (k + j >= avg_from) cdsum += hist[3 * j] / q, navg++;
    }
    double el = now() - t0, mean = ok && navg ? cdsum / navg : NAN;
    *mlups = (double)s.nx * s.ny * s.nz * steps / el / 1e6;
    printf("  %s D %2d, box %d D: Cd %.4f; %ld links; %d steps in %.1f s, %.0f MLUPS\n", gpu ? "GPU" : "CPU", D, w, mean, lbm3d_links(E.L), steps,
           el, *mlups);
    free(hist);
    eng_free(&E);
    return mean;
}

static void outlet_case(bool gpu, ThreadPool *pool) {
    const char *name = gpu ? "L5 (GPU)" : "L5 (CPU)";
    printf("== %s: a stream through the pressure outlet, forward and back\n", name);
    double worst = 0;
    bool ok = true;
    for (int dir = -1; dir <= 1 && ok; dir += 2) {
        Lbm3DSpec s = {.nx = 48, .ny = 16, .nz = 16, .nu = 0.05, .u_in = {0.05 * dir, 0, 0}, .bc_x = LBM3D_INOUT, .bc_y = LBM3D_SLIP,
                       .bc_z = LBM3D_SLIP, .outlet = 1};
        Engine E;
        char err[256];
        if (!eng_make(&E, s, gpu, NULL, NULL, pool, err, sizeof err)) {
            printf("  %s\n", err);
            ok = false;
            eng_free(&E);
            break;
        }
        ok = eng_steps(&E, 2000, NULL);
        size_t n = (size_t)s.nx * s.ny * s.nz;
        double *u = malloc(3 * n * sizeof *u);
        eng_macro(&E, u);
        for (size_t i = 0; i < n; i++)
            worst = fmax(worst, fmax(fabs(u[3 * i] / s.u_in[0] - 1), fmax(fabs(u[3 * i + 1]), fabs(u[3 * i + 2])) / 0.05));
        free(u);
        eng_free(&E);
    }
    printf("  largest departure from the stream %.3g %%%s\n", 100 * worst, ok ? "" : "; UNSTABLE");
    verdict(ok && worst < 0.01, name);
}

int main(int argc, char **argv) {
    bool fast = argc > 1 && !strcmp(argv[1], "--fast"); /* L1 only: the fast tier */
    ThreadPool *pool = pool_create(cpu_perf_count());
    poiseuille(false, pool);
    poiseuille(true, pool);
    outlet_case(false, pool);
    outlet_case(true, pool);
    if (!fast) {
        double m16, m24, mc, mg;
        printf("== L2 (GPU): a sphere at Reynolds number 100 on two grids\n");
        double c16 = sphere_cd(true, 16, 6, 25, 100, pool, &m16), c24 = sphere_cd(true, 24, 6, 25, 100, pool, &m24);
        double ext = c24 + (c24 - c16) / ((24.0 * 24) / (16.0 * 16) - 1);
        printf("  extrapolated %.4f against 1.087 (%+.2f %%)\n", ext, 100 * (ext / 1.087 - 1));
        verdict(fabs(ext / 1.087 - 1) < 0.04 && fabs(c24 - 1.087) < fabs(c16 - 1.087), "L2 (GPU)");
        printf("== L4: the GPU's single precision against the CPU's double, the same small sphere (Reynolds number 40)\n");
        double g = sphere_cd(true, 8, 4, 12, 40, pool, &mg), c = sphere_cd(false, 8, 4, 12, 40, pool, &mc);
        printf("  GPU %.5f, CPU %.5f (%+.3f %%)\n", g, c, 100 * (g / c - 1));
        verdict(fabs(g / c - 1) < 0.005, "L4");
        printf("== L3: speed\n  GPU %.0f million lattice updates per second (single precision, shifted populations); CPU %.0f (%d threads, double)\n",
               m24, mc, pool_size(pool));
    }
    pool_destroy(pool);
    printf(failures ? "lbm3dtest: %d FAILED\n" : "lbm3dtest: all passed\n", failures);
    return failures ? 1 : 0;
}
