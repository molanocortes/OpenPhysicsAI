/* sheettest - verification of thin sheets (src/lab/sheet/sheet.h).
 *
 * Criteria written on 2026-09-26, before the first run:
 *   C1 a rubber balloon (a sphere of radius 0.1 m meshed from an icosahedron, wall 1 mm, shear modulus 0.3 MPa) blown up
 *      slowly (heavy damping, pressure held at each level until still): at pressures of 0.5 and 0.9 of the peak, the
 *      stretch lambda = (V / V0)^(1/3) on the rising branch of the incompressible neo-Hookean membrane's
 *      p = (2 H0 mu / R0) (1 / lambda - 1 / lambda^7) (whose peak is at lambda = 7^(1/6) = 1.383), within 1 %.
 *   C2 a strip 0.1 m long, 20 mm wide, 2 mm thick, clamped at one end, sagging under its own weight: the tip's
 *      deflection against Euler-Bernoulli's w L^4 / (8 D) with D the plate rigidity E H^3 / (12 (1 - nu^2)), within 5 %
 *      (small deflection, a fiftieth of the length).
 *   C2 AMENDED (2026-09-26, after its first run, recorded): the strip is now as wide as it is long (0.1 x 0.1 m), and the
 *      tip deflection is read along its middle. First run: -57.6 % (FAIL). Two causes: the engine's hinge coefficient was
 *      twice the plate's (its weights sum theta^2 to twice the integral of curvature squared; now D / 4), and a strip 20
 *      mm wide bends anticlastically, which the plate formula for cylindrical bending leaves out. The criterion is
 *      unchanged. C3's first run drifted 3.8 %: the step was semi-implicit Euler, which reads kinetic energy half a step
 *      off; the sheet now steps by velocity Verlet. Criterion unchanged.
 *   C3 a free square sheet, flat, set vibrating in its plane by a sinusoidal stretch, no damping: membrane plus kinetic
 *      energy stays within 1 % over 2000 steps at the stable step.
 *   Second run (hinge coefficient D / 4, Verlet): C2 -20.6 %, C3 0.81 %. The hinge energy was then measured on a bent
 *   cylinder: 1.46 times the plate's along a right-triangle grid and 4.4 times across it, so it cannot be calibrated by a
 *   factor. A squared-Laplacian energy (isotropic, but blind to saddles) gave +130 %: a plate with free sides bends
 *   anticlastically at no cost. The sheet now bends by a per-triangle shape operator from its edges' angles (the
 *   mid-edge normal model, sheet.c), measured within 3.5 % of the plate on cylinders in three directions and on a saddle,
 *   on right-triangle and equilateral grids (the shortfall is the free edges' relaxation).
 *   Third run: C2 +15.3 %. The reference itself was wrong: the middle of a square cantilever plate's free edge has no
 *   closed form; w L^4 / (8 D) holds only for cylindrical bending (infinitely wide), and a narrow strip tends to the beam's
 *   w L^4 / (8 E I), a third more. +15 % lies between the two.
 *   C2 REPLACED (2026-09-26, written before its first run): a circular plate of radius 50 mm, 1 mm thick, shear modulus
 *      1 MPa, clamped at its rim, under a uniform pressure giving a twentieth of the thickness: the centre's deflection
 *      against Kirchhoff's q a^4 / (64 D) with D = E H^3 / (12 (1 - nu^2)), within 3 %, on a mesh of 24 rings. The exact
 *      plate solution uses both curvatures and Poisson's ratio, and there are no free edges.
 *      Its first run: +7.06 % (FAIL), converging at first order (+19.6, +10.3, +7.1, +5.4 % on 8, 16, 24, 32 rings): the
 *      clamp sat half a cell out, since a rim edge's mid-edge normal was tilted by half the angle. Edges against held
 *      triangles now take the whole angle; then -2.18, -1.44, -0.94, -0.69 % on the same meshes. Criterion unchanged.
 *   C4 (added 2026-09-26 with the new bending, before its first run) a free square sheet released from a bent shape (a
 *      2 mm bump, not developable, so it stretches and bends), no damping, no gravity: membrane plus bending plus kinetic
 *      energy within 1 % over 2000 steps.
 *   Written 2026-09-26 with the sheet's self contact, before their first run:
 *   C5 two rubber squares (0.1 m, 1 mm), one resting on a rigid floor, the other dropped onto it from 20 mm, contact
 *      distance 2 mm: once settled, the mean gap between them within 10 % of 2 mm, and no node of the upper sheet ever
 *      below 1 mm above the lower one.
 *   C6 two squares thrown at each other at 0.5 m/s each, no gravity, no damping, the upper grid offset by part of a cell:
 *      total momentum within 1e-9 of the momentum scale, the sheets never closer than a quarter of the contact
 *      distance, and the kinetic energy after the bounce not above the energy before.
 *   First run: C5 pass (gap -0.0 %). C6 FAIL on "closest -1.49 mm", but the sheets had bounced (each moving back at
 *   0.06 m/s): the test measured the lowest upper node against the highest lower node anywhere, which the waving free
 *   edges cross without the sheets meeting. It now measures what the criterion says, each upper node's distance to the
 *   lower sheet's triangles, every step. Criterion unchanged. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lab/sheet/sheet.h"

static int failures;
static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    failures += !ok;
}

/* an icosphere: the icosahedron subdivided `levels` times and pushed onto the sphere, triangles facing outwards */
static int icosphere(double R, int levels, double **xyz, int **tri, int *nt) {
    const double t = (1 + sqrt(5.0)) / 2;
    double V[12][3] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    int F[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                    {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    int nv = 12, nf = 20, capv = 12 * (1 << (2 * levels)) + 10, capf = 20 * (1 << (2 * levels));
    double *X = malloc(3 * (size_t)capv * sizeof(double));
    int *T = malloc(3 * (size_t)capf * sizeof(int));
    for (int i = 0; i < 12; i++) memcpy(&X[3 * i], V[i], sizeof V[i]);
    for (int f = 0; f < 20; f++) memcpy(&T[3 * f], F[f], sizeof F[f]);
    for (int l = 0; l < levels; l++) {
        int *T2 = malloc(3 * (size_t)nf * 4 * sizeof(int)), n2 = 0;
        /* midpoints, shared through a small hash of edge -> vertex */
        int hcap = 16 * nf;
        long long *hk = malloc((size_t)hcap * sizeof(long long));
        int *hv = malloc((size_t)hcap * sizeof(int));
        for (int i = 0; i < hcap; i++) hk[i] = -1;
        for (int f = 0; f < nf; f++) {
            int a[3] = {T[3 * f], T[3 * f + 1], T[3 * f + 2]}, m[3];
            for (int e = 0; e < 3; e++) {
                int p = a[e], q = a[(e + 1) % 3];
                long long key = (long long)(p < q ? p : q) * capv + (p < q ? q : p);
                int slot = (int)(key % hcap);
                while (hk[slot] != -1 && hk[slot] != key) slot = (slot + 1) % hcap;
                if (hk[slot] == -1) {
                    hk[slot] = key, hv[slot] = nv;
                    for (int k = 0; k < 3; k++) X[3 * nv + k] = 0.5 * (X[3 * p + k] + X[3 * q + k]);
                    nv++;
                }
                m[e] = hv[slot];
            }
            int nt4[4][3] = {{a[0], m[0], m[2]}, {a[1], m[1], m[0]}, {a[2], m[2], m[1]}, {m[0], m[1], m[2]}};
            for (int s = 0; s < 4; s++) memcpy(&T2[3 * n2++], nt4[s], sizeof nt4[s]);
        }
        free(hk), free(hv), free(T), T = T2, nf = n2;
    }
    for (int i = 0; i < nv; i++) {
        double l = sqrt(X[3 * i] * X[3 * i] + X[3 * i + 1] * X[3 * i + 1] + X[3 * i + 2] * X[3 * i + 2]);
        for (int k = 0; k < 3; k++) X[3 * i + k] *= R / l;
    }
    *xyz = X, *tri = T, *nt = nf;
    return nv;
}

/* a rectangle of nx x ny nodes, triangulated, in the x-y plane */
static int grid(double Lx, double Ly, int nx, int ny, double **xyz, int **tri, int *nt) {
    double *X = malloc(3 * (size_t)nx * ny * sizeof(double));
    int *T = malloc(6 * (size_t)(nx - 1) * (ny - 1) * sizeof(int)), k = 0;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) X[3 * (j * nx + i)] = Lx * i / (nx - 1), X[3 * (j * nx + i) + 1] = Ly * j / (ny - 1), X[3 * (j * nx + i) + 2] = 0;
    for (int j = 0; j + 1 < ny; j++)
        for (int i = 0; i + 1 < nx; i++) {
            int a = j * nx + i, b = a + 1, c = a + nx, d = c + 1;
            T[k++] = a, T[k++] = b, T[k++] = d, T[k++] = a, T[k++] = d, T[k++] = c;
        }
    *xyz = X, *tri = T, *nt = k / 3;
    return nx * ny;
}

typedef struct {
    double z;
} FloorCtx;
static double floor_sdf(const double p[3], void *ctx) { return p[2] - ((FloorCtx *)ctx)->z; }

/* the distance from p to triangle abc, by sampling the triangle's plane and its edges exactly */
static double seg_dist(const double *p, const double *a, const double *b) {
    double ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    double t = (ab[0] * ap[0] + ab[1] * ap[1] + ab[2] * ap[2]) / (ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double q[3] = {a[0] + t * ab[0] - p[0], a[1] + t * ab[1] - p[1], a[2] + t * ab[2] - p[2]};
    return sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
}
static double point_tri_distance(const double *p, const double *a, const double *b, const double *c) {
    double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]}, n[3];
    n[0] = e1[1] * e2[2] - e1[2] * e2[1], n[1] = e1[2] * e2[0] - e1[0] * e2[2], n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    double nl = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int k = 0; k < 3; k++) n[k] /= nl;
    double h = (p[0] - a[0]) * n[0] + (p[1] - a[1]) * n[1] + (p[2] - a[2]) * n[2], q[3] = {p[0] - h * n[0], p[1] - h * n[1], p[2] - h * n[2]};
    /* the projection inside the triangle (same side of all three edges)? */
    const double *v[3] = {a, b, c};
    bool inside = true;
    for (int k = 0; k < 3 && inside; k++) {
        const double *u = v[k], *w = v[(k + 1) % 3];
        double ed[3] = {w[0] - u[0], w[1] - u[1], w[2] - u[2]}, uq[3] = {q[0] - u[0], q[1] - u[1], q[2] - u[2]}, cr[3];
        cr[0] = ed[1] * uq[2] - ed[2] * uq[1], cr[1] = ed[2] * uq[0] - ed[0] * uq[2], cr[2] = ed[0] * uq[1] - ed[1] * uq[0];
        inside = cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] >= 0;
    }
    if (inside) return fabs(h);
    return fmin(seg_dist(p, a, b), fmin(seg_dist(p, b, c), seg_dist(p, c, a)));
}

static double balloon_p(double lam) { return 1 / lam - pow(lam, -7); }

int main(void) {
    ThreadPool *pool = pool_create(cpu_perf_count());
    char err[256];
    printf("== C1: a rubber balloon blown up, against the neo-Hookean membrane\n");
    {
        const double R0 = 0.1, H0 = 1e-3, mu = 3e5;
        double *X;
        int *T, nt;
        int nn = icosphere(R0, 4, &X, &T, &nt);
        SheetSpec sp = {.shear_modulus = mu, .thickness = H0, .density = 1000, .bending_scale = 1, .damping = 200};
        Sheet *S = sheet_create(&sp, nn, X, nt, T, err, sizeof err);
        if (!S) {
            printf("  %s\n", err);
            verdict(false, "C1");
        } else {
            double V0 = sheet_volume(S), pmax = 2 * H0 * mu / R0 * balloon_p(pow(7, 1.0 / 6)), dt = sheet_stable_dt(S), worst = 0;
            printf("  %d nodes, %d triangles, step %.3g s; the peak pressure %.1f Pa at lambda 1.383\n", nn, nt, dt, pmax);
            double levels[2] = {0.5, 0.9};
            for (int L = 0; L < 2; L++) {
                double p = levels[L] * pmax;
                sheet_set_pressure(S, p);
                for (int s = 0; s < 40000; s++) sheet_step(S, dt, pool);
                double lam = cbrt(sheet_volume(S) / V0), lo = 1, hi = pow(7, 1.0 / 6), target = p / (2 * H0 * mu / R0);
                for (int it = 0; it < 60; it++) { /* the rising branch's stretch at this pressure */
                    double mid = 0.5 * (lo + hi);
                    if (balloon_p(mid) < target) lo = mid;
                    else hi = mid;
                }
                double exact = 0.5 * (lo + hi);
                printf("  p = %.2f of the peak: lambda %.5f against %.5f (%+.3f %%)\n", levels[L], lam, exact, 100 * (lam / exact - 1));
                worst = fmax(worst, fabs(lam / exact - 1));
            }
            verdict(worst < 0.01, "C1");
            sheet_free(S);
        }
        free(X), free(T);
    }
    printf("== C2: a clamped circular plate under pressure, against Kirchhoff\n");
    {
        const double a = 0.05, H = 1e-3, mu = 1e6, D = mu * H * H * H / 3;
        const int N = 24; /* rings inside the rim; one more outside, both held (the clamp) */
        const double q = 64 * D * 0.05 * H / pow(a, 4);
        int nr = N + 1, nn = 1;
        for (int i = 1; i <= nr; i++) nn += 6 * i;
        double *X = malloc(3 * (size_t)nn * sizeof(double));
        int *T = malloc(3 * (size_t)6 * nr * nr * sizeof(int)), nt = 0, *start = malloc((size_t)(nr + 2) * sizeof(int));
        X[0] = X[1] = X[2] = 0, start[0] = 0, start[1] = 1;
        for (int i = 1; i <= nr; i++) {
            start[i + 1] = start[i] + 6 * i;
            for (int j = 0; j < 6 * i; j++) {
                double r = a * i / N, ph = 2 * M_PI * j / (6 * i);
                double *p = &X[3 * (start[i] + j)];
                p[0] = r * cos(ph), p[1] = r * sin(ph), p[2] = 0;
            }
        }
        for (int i = 0; i < nr; i++) { /* between ring i and ring i + 1, walking round by angle */
            int n1 = i ? 6 * i : 1, n2 = 6 * (i + 1), p = 0, qq = 0;
            while (p < (i ? n1 : 0) || qq < n2) {
                double ap = i ? 2 * M_PI * (p + 1) / n1 : 1e9, aq = 2 * M_PI * (qq + 1) / n2;
                int ip = start[i] + (i ? p % n1 : 0), iq = start[i + 1] + qq % n2;
                if (aq <= ap) T[nt++] = ip, T[nt++] = iq, T[nt++] = start[i + 1] + (qq + 1) % n2, qq++;
                else T[nt++] = ip, T[nt++] = iq, T[nt++] = start[i] + (p + 1) % n1, p++;
            }
        }
        nt /= 3;
        SheetSpec sp = {.shear_modulus = mu, .thickness = H, .density = 1000, .bending_scale = 1, .damping = 150, .pressure = q};
        Sheet *S = sheet_create(&sp, nn, X, nt, T, err, sizeof err);
        if (!S) {
            printf("  %s\n", err);
            verdict(false, "C2");
        } else {
            for (int k = start[N]; k < nn; k++) sheet_pin(S, k);
            double dt = sheet_stable_dt(S), mid = 0;
            const int steps = 40000;
            for (int s = 0; s < steps; s++) {
                sheet_step(S, dt, pool);
                if (s == steps / 2) mid = sheet_positions(S)[2];
            }
            double w = sheet_positions(S)[2], exact = q * pow(a, 4) / (64 * D);
            printf("  %d nodes, %d triangles, step %.3g s; centre %.6e m against %.6e (%+.2f %%; halfway %+.3f %%)\n", nn, nt, dt, w, exact,
                   100 * (w / exact - 1), 100 * (mid / exact - 1));
            verdict(fabs(w / exact - 1) < 0.03, "C2");
            sheet_free(S);
        }
        free(X), free(T), free(start);
    }
    printf("== C3: a free sheet vibrating in its plane, energy\n");
    {
        double *X;
        int *T, nt;
        int nn = grid(0.1, 0.1, 21, 21, &X, &T, &nt);
        for (int i = 0; i < nn; i++) X[3 * i] *= 1 + 0.01 * sin(M_PI * X[3 * i] / 0.1); /* stretched, released */
        SheetSpec sp = {.shear_modulus = 3e5, .thickness = 1e-3, .density = 1000, .bending_scale = 1};
        Sheet *S = sheet_create(&sp, nn, X, nt, T, err, sizeof err);
        free(X), free(T);
        nn = grid(0.1, 0.1, 21, 21, &X, &T, &nt); /* the rest shape is the flat square: rebuild from it and move the nodes */
        sheet_free(S);
        S = sheet_create(&sp, nn, X, nt, T, err, sizeof err);
        double *x = (double *)sheet_positions(S);
        for (int i = 0; i < nn; i++) x[3 * i] *= 1 + 0.01 * sin(M_PI * x[3 * i] / 0.1);
        double em0, eb0, ek0, em, eb, ek, dt = sheet_stable_dt(S), worst = 0;
        sheet_energy(S, &em0, &eb0, &ek0);
        for (int s = 0; s < 2000; s++) {
            sheet_step(S, dt, pool);
            if (s % 20 == 19) {
                sheet_energy(S, &em, &eb, &ek);
                worst = fmax(worst, fabs((em + eb + ek) / (em0 + eb0 + ek0) - 1));
            }
        }
        printf("  start %.4g J; largest change %.3f %%\n", em0 + eb0 + ek0, 100 * worst);
        verdict(worst < 0.01, "C3");
        sheet_free(S), free(X), free(T);
    }
    printf("== C4: a free sheet released from a bump, energy with bending\n");
    {
        double *X;
        int *T, nt;
        int nn = grid(0.1, 0.1, 21, 21, &X, &T, &nt);
        SheetSpec sp = {.shear_modulus = 3e5, .thickness = 1e-3, .density = 1000, .bending_scale = 1};
        Sheet *S = sheet_create(&sp, nn, X, nt, T, err, sizeof err);
        double *x = (double *)sheet_positions(S);
        for (int i = 0; i < nn; i++) x[3 * i + 2] = 0.002 * sin(M_PI * x[3 * i] / 0.1) * sin(M_PI * x[3 * i + 1] / 0.1);
        double em0, eb0, ek0, em, eb, ek, dt = sheet_stable_dt(S), worst = 0;
        sheet_energy(S, &em0, &eb0, &ek0);
        for (int s = 0; s < 2000; s++) {
            sheet_step(S, dt, pool);
            if (s % 20 == 19) {
                sheet_energy(S, &em, &eb, &ek);
                worst = fmax(worst, fabs((em + eb + ek) / (em0 + eb0 + ek0) - 1));
            }
        }
        printf("  start %.4g J (membrane %.3g, bending %.3g); end bending %.3g, kinetic %.3g; largest change %.3f %%\n", em0 + eb0 + ek0, em0, eb0, eb, ek, 100 * worst);
        verdict(worst < 0.01, "C4");
        sheet_free(S), free(X), free(T);
    }
    /* two squares in one mesh, lower at z0 and upper at z1 (offset in the plane by ox, oy) */
    double *X2 = NULL;
    int *T2 = NULL, nt2 = 0, nper = 0;
    {
        double *Xa;
        int *Ta, nta;
        nper = grid(0.1, 0.1, 21, 21, &Xa, &Ta, &nta);
        X2 = malloc(6 * (size_t)nper * sizeof(double)), T2 = malloc(6 * (size_t)nta * sizeof(int)), nt2 = 2 * nta;
        for (int i = 0; i < nper; i++)
            for (int k = 0; k < 3; k++) X2[3 * i + k] = Xa[3 * i + k], X2[3 * (nper + i) + k] = Xa[3 * i + k];
        for (int t = 0; t < 3 * nta; t++) T2[t] = Ta[t], T2[3 * nta + t] = Ta[t] + nper;
        free(Xa), free(Ta);
    }
    printf("== C5: a sheet dropped onto another on a floor, the contact gap\n");
    {
        const double d = 0.002;
        for (int i = 0; i < nper; i++) X2[3 * i + 2] = 0.001, X2[3 * (nper + i) + 2] = 0.021, X2[3 * (nper + i)] += 0.0025, X2[3 * (nper + i) + 1] += 0.0017;
        SheetSpec sp = {.shear_modulus = 3e5, .thickness = 1e-3, .density = 1000, .bending_scale = 1, .gravity = 9.80665, .damping = 60,
                        .contact_distance = 0.001, .self_contact = d};
        Sheet *S = sheet_create(&sp, 2 * nper, X2, nt2, T2, err, sizeof err);
        FloorCtx fc = {0};
        sheet_set_bodies(S, floor_sdf, &fc);
        double dt = sheet_stable_dt(S), worst = 1e9;
        for (int k = 0; k < (int)(0.5 / dt); k++) {
            sheet_step(S, dt, pool);
            if (k % 50 == 0) {
                const double *x = sheet_positions(S);
                double top_min = 1e9, bot_max = -1e9;
                for (int i = 0; i < nper; i++) bot_max = fmax(bot_max, x[3 * i + 2]), top_min = fmin(top_min, x[3 * (nper + i) + 2]);
                worst = fmin(worst, top_min - bot_max);
            }
        }
        const double *x = sheet_positions(S);
        double zb = 0, zt = 0;
        for (int i = 0; i < nper; i++) zb += x[3 * i + 2] / nper, zt += x[3 * (nper + i) + 2] / nper;
        printf("  mean gap %.4f mm against %.1f mm (%+.1f %%); closest the upper sheet came to the lower %.4f mm\n", 1e3 * (zt - zb), 1e3 * d,
               100 * ((zt - zb) / d - 1), 1e3 * worst);
        verdict(fabs((zt - zb) / d - 1) < 0.1 && worst > 0.5 * d, "C5");
        sheet_free(S);
    }
    printf("== C6: two sheets thrown at each other: momentum, no passing through, energy\n");
    {
        const double d = 0.002, V = 0.5;
        for (int i = 0; i < nper; i++) X2[3 * i + 2] = 0, X2[3 * (nper + i) + 2] = 0.01;
        SheetSpec sp = {.shear_modulus = 3e5, .thickness = 1e-3, .density = 1000, .bending_scale = 1, .self_contact = d};
        Sheet *S = sheet_create(&sp, 2 * nper, X2, nt2, T2, err, sizeof err);
        double *v = (double *)sheet_velocities(S), ke0 = 0, pscale = 0;
        for (int i = 0; i < nper; i++) v[3 * i + 2] = V, v[3 * (nper + i) + 2] = -V;
        for (int i = 0; i < 2 * nper; i++) ke0 += 0.5 * sheet_node_mass(S, i) * V * V, pscale += sheet_node_mass(S, i) * V;
        double dt = sheet_stable_dt(S), closest = 1e9, pworst = 0;
        for (int k = 0; k < (int)(0.04 / dt); k++) {
            sheet_step(S, dt, pool);
            const double *x = sheet_positions(S);
            double P[3] = {0, 0, 0};
            for (int i = 0; i < 2 * nper; i++)
                for (int c = 0; c < 3; c++) P[c] += sheet_node_mass(S, i) * v[3 * i + c];
            pworst = fmax(pworst, sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]) / pscale);
            for (int i = nper; i < 2 * nper; i++) /* each upper node's distance to the lower sheet */
                for (int t = 0; t < nt2 / 2; t++) {
                    const int *Tr = &T2[3 * t];
                    closest = fmin(closest, point_tri_distance(&x[3 * i], &x[3 * Tr[0]], &x[3 * Tr[1]], &x[3 * Tr[2]]));
                }
        }
        double em, eb, ek;
        sheet_energy(S, &em, &eb, &ek);
        printf("  closest %.4f mm (a quarter of the distance: %.2f mm); momentum drift %.2e of scale; energy before %.5g J, after: kinetic %.5g, "
               "elastic %.3g J; the sheets now move at %+.3f and %+.3f m/s\n", 1e3 * closest, 250 * d, pworst, ke0, ek, em + eb, v[3 * (nper / 2) + 2],
               v[3 * (nper + nper / 2) + 2]);
        verdict(pworst < 1e-9 && closest > 0.25 * d && ek + em + eb <= ke0, "C6");
        sheet_free(S);
    }
    free(X2), free(T2);
    pool_destroy(pool);
    printf(failures ? "sheettest: %d FAILED\n" : "sheettest: all passed\n", failures);
    return failures ? 1 : 0;
}
