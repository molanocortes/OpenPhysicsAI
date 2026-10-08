/* fsitest - verification of the immersed boundary on the GPU flow engine (lbm3d_metal.h) and of the sheet in a flow
 * (src/lab/fsi/fsi.h).
 *
 * Criteria written on 2026-09-26, before the first run:
 *   F1 Couette flow made by an immersed plane: a plane of points 24 cells above a no-slip floor moves along x at 0.01
 *      (lattice units), free slip far above it. Once steady, the velocity 12 cells above the floor within 2 % of half
 *      the plane's (the exact profile is linear), and the fluid at the plane within 2 % of the plane's speed.
 *   F1R (added 2026-09-26 before its first run, after the regularised collision's forcing was found to give the fluid
 *      j + F (3/2 - omega/2) instead of j + F): F1 again with the regularised collision, the same 2 %.
 *      AMENDED before any run could tell: its first run (+1.22 %, pass) was at F1's relaxation time 1, where the wrong and
 *      the right forms agree; it now runs at relaxation time 0.55 (viscosity 1/60), where they differ by 40 %.
 *   F2 Stokes drag of a sphere in a periodic array: an immersed sphere of radius 12 cells (points on its surface, held
 *      still) in a periodic box of 96 cells, the fluid driven by a uniform acceleration; at steady state the sphere's
 *      drag is the whole body force, and the mean velocity of the fluid outside it within 5 % of Hasimoto's (1959)
 *      F = 6 pi mu a U / (1 - 1.7601 c^(1/3) + c - 1.5593 c^2) (c the sphere's volume fraction).
 *   F3 action and reaction: a free square sheet (the sheet solver, two-way coupled through fsi.c) set moving through
 *      still fluid in a periodic box; the momentum of fluid and sheet together within 1 % of the sheet's initial
 *      momentum after 2000 flow steps.
 *   First run: F1 +1.31 % and -0.01 %, F2 -3.78 % (it ran the full 60 000 steps: the steadiness test was too strict for
 *   single precision), F3 unstable: the sheet, as heavy as one cell of fluid per node, took the whole direct-forcing
 *   force explicitly (the added-mass instability of partitioned coupling). The forcing now accounts for each point's
 *   mass, so that fluid and sheet end the step at one velocity. Criteria unchanged.
 *
 * Session subset, declared before its first run, 2026-10-03: --fast selects the existing F3 free-sheet momentum
 * case only, with the SAME 2000 flow steps and original 1 percent momentum criterion. F1/F1R steady Couette and F2
 * periodic-array drag are explicitly not exercised. The default full test and all its criteria remain unchanged. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lab/flow/lbm3d.h"
#include "../src/lab/flow/lbm3d_metal.h"
#include "../src/lab/fsi/fsi.h"
#include "../src/lab/sheet/sheet.h"

static int failures;
static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    failures += !ok;
}

static Lbm3DGpu *gpu(Lbm3DSpec s, Lbm3D **L, char *err, size_t el) {
    s.geometry_only = true;
    *L = lbm3d_create(&s, err, el);
    if (!*L || !lbm3d_set_bodies(*L, NULL, NULL)) return NULL;
    Lbm3DGpu *G = lbm3d_gpu_create(*L, err, el);
    if (G) lbm3d_gpu_init(G);
    return G;
}

int main(int argc, char **argv) {
    bool fast = argc == 2 && !strcmp(argv[1], "--fast");
    if (argc > 1 && !fast) {
        fprintf(stderr, "usage: fsitest [--fast]\n");
        return 2;
    }
    char err[256];
    ThreadPool *pool = pool_create(cpu_perf_count());
    if (!fast) {
    for (int reg = 0; reg < 2; reg++) {
    printf(reg ? "== F1R: the same with the regularised collision\n" : "== F1: Couette flow made by an immersed plane\n");
    {
        const int nx = 4, ny = 4, nz = 40;
        const double H = 24, U = 0.01;
        Lbm3DSpec s = {.nx = nx, .ny = ny, .nz = nz, .nu = reg ? 1.0 / 60 : 1.0 / 6, .bc_x = LBM3D_PERIODIC, .bc_y = LBM3D_PERIODIC, .bc_z = LBM3D_SLIP, .floor = true, .collision = reg ? LBM3D_REGULARIZED : LBM3D_TRT};
        Lbm3D *L = NULL;
        Lbm3DGpu *G = gpu(s, &L, err, sizeof err);
        if (!G || !lbm3d_gpu_ib_enable(G, nx * ny, 4, 1.0, err, sizeof err)) {
            printf("  %s\n", err);
            verdict(false, reg ? "F1R" : "F1");
        } else {
            float *pt = lbm3d_gpu_ib_points(G), *vel = lbm3d_gpu_ib_velocities(G);
            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx; i++) {
                    float *p = &pt[4 * (j * nx + i)], *v = &vel[4 * (j * nx + i)];
                    p[0] = i + 0.5f, p[1] = j + 0.5f, p[2] = (float)H, p[3] = 1;
                    v[0] = (float)U, v[1] = v[2] = v[3] = 0;
                }
            bool ok = true;
            for (int k = 0; k < (reg ? 150000 : 30000) && ok; k++) ok = lbm3d_gpu_ib_step(G, NULL);
            double *u = malloc(3 * (size_t)nx * ny * nz * sizeof(double));
            lbm3d_gpu_macro(G, NULL, u);
            double mid = u[3 * ((size_t)nx * ny * 11)], /* the cell whose centre is 11.5 above the floor */
                at = 0.5 * (u[3 * ((size_t)nx * ny * 23)] + u[3 * ((size_t)nx * ny * 24)]); /* centres 23.5 and 24.5: the plane at 24 */
            double emid = U * 11.5 / H;
            printf("  u(11.5) %.6f against %.6f (%+.3f %%); u at the plane %.6f against %.6f (%+.3f %%)%s\n", mid, emid, 100 * (mid / emid - 1), at, U,
                   100 * (at / U - 1), ok ? "" : "; UNSTABLE");
            verdict(ok && fabs(mid / emid - 1) < 0.02 && fabs(at / U - 1) < 0.02, reg ? "F1R" : "F1");
            free(u);
        }
        lbm3d_gpu_free(G), lbm3d_free(L);
    }
    }
    printf("== F2: an immersed sphere in a periodic array, against Hasimoto\n");
    {
        const int N = 96;
        const double a = 12, nu = 1.0 / 3, g = 2e-7, c = 4.0 / 3 * M_PI * a * a * a / ((double)N * N * N);
        Lbm3DSpec s = {.nx = N, .ny = N, .nz = N, .nu = nu, .accel = {g, 0, 0}, .bc_x = LBM3D_PERIODIC, .bc_y = LBM3D_PERIODIC, .bc_z = LBM3D_PERIODIC};
        /* points on the sphere: a subdivided icosahedron (level 5: 10242 points, about 0.4 cells apart) */
        int lev = 5, cap = 10 * (1 << (2 * lev)) + 2, nv = 12;
        double (*V)[3] = malloc((size_t)cap * sizeof *V);
        int (*T)[3] = malloc(20 * (size_t)(1 << (2 * lev)) * sizeof *T), nt = 20;
        {
            const double t = (1 + sqrt(5.0)) / 2;
            double V0[12][3] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
            int F0[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                             {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
            memcpy(V, V0, sizeof V0), memcpy(T, F0, sizeof F0);
            for (int l = 0; l < lev; l++) {
                int (*T2)[3] = malloc(4 * (size_t)nt * sizeof *T2), n2 = 0, hcap = 8 * nt;
                long long *hk = malloc((size_t)hcap * sizeof *hk);
                int *hv = malloc((size_t)hcap * sizeof *hv);
                for (int i = 0; i < hcap; i++) hk[i] = -1;
                for (int f = 0; f < nt; f++) {
                    int m[3];
                    for (int e = 0; e < 3; e++) {
                        int p = T[f][e], q = T[f][(e + 1) % 3];
                        long long key = (long long)(p < q ? p : q) * cap + (p < q ? q : p);
                        int sl = (int)(key % hcap);
                        while (hk[sl] != -1 && hk[sl] != key) sl = (sl + 1) % hcap;
                        if (hk[sl] == -1) {
                            hk[sl] = key, hv[sl] = nv;
                            for (int k = 0; k < 3; k++) V[nv][k] = 0.5 * (V[p][k] + V[q][k]);
                            nv++;
                        }
                        m[e] = hv[sl];
                    }
                    int A = T[f][0], B = T[f][1], C = T[f][2];
                    int add[4][3] = {{A, m[0], m[2]}, {B, m[1], m[0]}, {C, m[2], m[1]}, {m[0], m[1], m[2]}};
                    memcpy(T2[n2], add, sizeof add), n2 += 4;
                }
                free(T), free(hk), free(hv);
                T = T2, nt = n2;
            }
        }
        Lbm3D *L = NULL;
        Lbm3DGpu *G = gpu(s, &L, err, sizeof err);
        if (!G || !lbm3d_gpu_ib_enable(G, nv, 4, 1.0, err, sizeof err)) {
            printf("  %s\n", err);
            verdict(false, "F2");
        } else {
            float *pt = lbm3d_gpu_ib_points(G), *vel = lbm3d_gpu_ib_velocities(G);
            const double dA = 4 * M_PI * a * a / nv; /* each point's share of the surface, times one cell */
            for (int i = 0; i < nv; i++) {
                double l = sqrt(V[i][0] * V[i][0] + V[i][1] * V[i][1] + V[i][2] * V[i][2]);
                for (int k = 0; k < 3; k++) pt[4 * i + k] = (float)(0.5 * N + a * V[i][k] / l), vel[4 * i + k] = 0;
                pt[4 * i + 3] = (float)dA, vel[4 * i + 3] = 0;
            }
            bool ok = true;
            double Uf = 0, Uprev = -1;
            double *u = malloc(3 * (size_t)N * N * N * sizeof(double));
            int k = 0;
            for (; k < 60000 && ok; k++) {
                ok = lbm3d_gpu_ib_step(G, NULL);
                if (k % 2000 == 1999) { /* steady when the mean speed changes by less than 2e-4 over 2000 steps */
                    lbm3d_gpu_macro(G, NULL, u);
                    double sum = 0;
                    long cnt = 0;
                    for (int z = 0; z < N; z++)
                        for (int y = 0; y < N; y++)
                            for (int x = 0; x < N; x++) {
                                double dx = x + 0.5 - 0.5 * N, dy = y + 0.5 - 0.5 * N, dz = z + 0.5 - 0.5 * N;
                                if (dx * dx + dy * dy + dz * dz <= a * a) continue;
                                sum += u[3 * ((size_t)x + (size_t)N * (y + (size_t)N * z))], cnt++;
                            }
                    Uf = sum / cnt;
                    if (fabs(Uf - Uprev) < 2e-4 * fabs(Uf)) break;
                    Uprev = Uf;
                }
            }
            double F = g * (double)N * N * N, K = 1 - 1.7601 * cbrt(c) + c - 1.5593 * c * c, Uh = F * K / (6 * M_PI * nu * a);
            const float *Fp = lbm3d_gpu_ib_forces(G);
            double Fib = 0;
            for (int i = 0; i < nv; i++) Fib -= Fp[4 * i];
            printf("  %d points, %d steps; drag %.6g (points %.6g); mean fluid velocity %.6g against Hasimoto %.6g (%+.2f %%), Re %.3f%s\n", nv, k + 1, F,
                   Fib, Uf, Uh, 100 * (Uf / Uh - 1), 2 * a * Uf / nu, ok ? "" : "; UNSTABLE");
            verdict(ok && fabs(Uf / Uh - 1) < 0.05, "F2");
            free(u);
        }
        free(V), free(T);
        lbm3d_gpu_free(G), lbm3d_free(L);
    }
    } else {
        printf("== session subset: existing F3 two-way momentum, 2000 steps\n"
               "  NOT EXERCISED: F1/F1R steady Couette; F2 periodic-array sphere drag\n"
               "  Run default fsitest detached for those full validations.\n");
        fflush(stdout);
    }
    printf("== F3: a free sheet coasting through still fluid, momentum\n");
    {
        const int N = 48, ns = 17; /* 17 x 17 nodes a cell apart */
        Lbm3DSpec s = {.nx = N, .ny = N, .nz = N, .nu = 0.05, .bc_x = LBM3D_PERIODIC, .bc_y = LBM3D_PERIODIC, .bc_z = LBM3D_PERIODIC};
        Lbm3D *L = NULL;
        Lbm3DGpu *G = gpu(s, &L, err, sizeof err);
        /* lattice units throughout (dx = dt = 1, fluid density 1): a sheet as heavy as a layer of fluid 1 cell thick */
        double *X = malloc(3 * (size_t)ns * ns * sizeof(double));
        int *Tr = malloc(6 * (size_t)(ns - 1) * (ns - 1) * sizeof(int)), q = 0;
        for (int j = 0; j < ns; j++)
            for (int i = 0; i < ns; i++) {
                double *p = &X[3 * (j * ns + i)];
                p[0] = 16 + i, p[1] = 16 + j, p[2] = 24;
            }
        for (int j = 0; j + 1 < ns; j++)
            for (int i = 0; i + 1 < ns; i++) {
                int a = j * ns + i, b = a + 1, c = a + ns, d = c + 1;
                Tr[q++] = a, Tr[q++] = b, Tr[q++] = d, Tr[q++] = a, Tr[q++] = d, Tr[q++] = c;
            }
        SheetSpec sp = {.shear_modulus = 0.05, .thickness = 0.1, .density = 10, .bending_scale = 1};
        Sheet *S = sheet_create(&sp, ns * ns, X, 2 * (ns - 1) * (ns - 1), Tr, err, sizeof err);
        const double o[3] = {0, 0, 0};
        Fsi *C = G && S ? fsi_create(G, S, 1, 1, 1, o, 4, 1.0, err, sizeof err) : NULL;
        if (!C) {
            printf("  %s\n", err);
            verdict(false, "F3");
        } else {
            double *v = (double *)sheet_velocities(S), P0 = 0, m = 0;
            for (int i = 0; i < ns * ns; i++) v[3 * i + 2] = 0.01, P0 += sheet_node_mass(S, i) * 0.01, m += sheet_node_mass(S, i);
            bool ok = true;
            for (int k = 0; k < 2000 && ok; k++) ok = fsi_step(C, pool, NULL);
            double *u = malloc(3 * (size_t)N * N * N * sizeof(double)), *rho = malloc((size_t)N * N * N * sizeof(double)), Pf = 0, Ps = 0;
            lbm3d_gpu_macro(G, rho, u);
            for (size_t i = 0; i < (size_t)N * N * N; i++) Pf += rho[i] * u[3 * i + 2];
            for (int i = 0; i < ns * ns; i++) Ps += sheet_node_mass(S, i) * v[3 * i + 2];
            printf("  sheet mass %.4g, %d sheet steps per flow step; momentum: start %.6g, now fluid %.6g + sheet %.6g = %.6g (%+.3f %%)%s\n", m,
                   fsi_substeps(C), P0, Pf, Ps, Pf + Ps, 100 * ((Pf + Ps) / P0 - 1), ok ? "" : "; UNSTABLE");
            verdict(ok && fabs((Pf + Ps) / P0 - 1) < 0.01, "F3");
            free(u), free(rho);
        }
        fsi_free(C), sheet_free(S), free(X), free(Tr);
        lbm3d_gpu_free(G), lbm3d_free(L);
    }
    pool_destroy(pool);
    if (fast) printf(failures ? "fsitest --fast: %d FAILED\n" : "fsitest --fast: F3 passed; full validation not run\n", failures);
    else printf(failures ? "fsitest: %d FAILED\n" : "fsitest: all passed\n", failures);
    return failures ? 1 : 0;
}
