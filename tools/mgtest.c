/* mgtest.c - verification of two-dimensional magnetostatics (src/lab/magnet).
 *
 * Criteria, written on 2026-09-26 before the first run and not to be changed without a dated note:
 *   M1  a straight conductor of radius a = 12 cells carrying a uniform current density J, in a box 40 a across with
 *       A = 0 on its edge: the mean azimuthal field on the ring r = a/2 within 1 % of mu0 J r / 2, and on the ring
 *       r = 3 a within 1 % of mu0 I / (2 pi r), with I the current of the cells actually painted.
 *   M2  an iron shell (mu_r 100, radii 25 and 50 cells) in a uniform applied field B0: the mean field inside r < 12
 *       cells within 3 % of the closed form 4 mu_r B0 / ((mu_r + 1)^2 - (mu_r - 1)^2 (a/b)^2).
 *   M3  a long cylinder of a permanent magnet (mu_r 1, remanence Br 1 T) magnetised across its axis, alone in space:
 *       the field at its centre within 2 % of Br / 2, along the magnetisation (the demagnetising factor 1/2).
 *   M4  (added 2026-09-26, before its first run, after a motor showed 6.6 T in the iron beside its magnets) the same
 *       magnet (mu_r 1.05) embedded in iron (mu_r 1000, a disk ten radii across): from the scalar potential, the field
 *       inside is Br mu2 / (mu_m + mu2); criterion: within 2 %. It tests the remanence at a magnet-iron interface,
 *       which M3, with mu_r 1 everywhere, cannot.
 * Added 2026-09-26 before their first run, for the motor that runs and heats (src/lab/magnet/mthermal.c, drive.c):
 *   M5  the rotor's equation of motion: a constant driving torque A against a fan load c omega^2 from rest has the closed
 *       form omega(t) = sqrt(A / c) tanh(t sqrt(A c) / J); the integrated speed at t = tau and t = 3 tau
 *       (tau = J / sqrt(A c)) within 1e-6 relative.
 *   M6  conduction: a disk of radius 40 cells with a uniform heat source q, in a ring held at T0, run to steady state by
 *       the implicit solver: the centre within 1 % of T0 + q R^2 / (4 k) (the staircase boundary costs about dx / R).
 *       First run, 2026-09-26: FAIL, +1.618 %. The criterion put the held temperature at R, but it is held at the centres
 *       of the held cells, which begin where a centre reaches R: 0.4 cells further out on average. Amended the same day:
 *       the closed form for a source in r < R inside a ring held at R_b, T0 + q R^2 / (4 k) + q R^2 / (2 k) ln(R_b / R),
 *       with R_b the mean radius of the held cells that touch free ones; tolerance unchanged.
 *   M7  the implicit conduction step conserves energy: over a transient of the same disk, the heat released equals the
 *       heat stored plus the heat taken by the held ring within 1e-6 of the heat released. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/lab/magnet/drive.h"
#include "../src/lab/magnet/magnet.h"
#include "../src/lab/magnet/mthermal.h"

#define MU0 (4e-7 * M_PI)

static int failures;

static double const_torque(double theta, void *ctx) {
    (void)theta;
    return *(const double *)ctx;
}

static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    fflush(stdout);
    if (!ok) failures++;
}

/* mean azimuthal and radial field on a ring about (cx, cy) */
static void ring(const Magnet *m, double cx, double cy, double r, double *bt, double *br) {
    const int N = 720;
    double st = 0, sr = 0;
    for (int k = 0; k < N; k++) {
        double th = 2 * M_PI * (k + 0.5) / N, B[2];
        mg_field(m, cx + r * cos(th), cy + r * sin(th), B);
        sr += B[0] * cos(th) + B[1] * sin(th), st += -B[0] * sin(th) + B[1] * cos(th);
    }
    *bt = st / N, *br = sr / N;
}

int main(void) {
    printf("== M1: a conductor with uniform current density\n");
    {
        MgSpec s;
        mg_spec_defaults(&s);
        const int a = 12, N = 40 * a;
        const double dx = 1e-3, J = 1e6, c = N / 2 * dx;
        s.nx = s.ny = N, s.dx = dx;
        s.nmaterials = 2;
        s.materials[1] = (MgMaterial){.mu_r = 1, .conductivity = 5.8e7};
        s.regions[s.nregions++] = (MgRegion){.shape = MG_CIRCLE, .a = {c, c, a * dx}, .material = 1, .J = J};
        char err[256];
        Magnet *m = mg_create(&s, err, sizeof err);
        int it = mg_solve(m);
        long cells = 0;
        for (int j = 0; j < N; j++)
            for (int i = 0; i < N; i++) cells += mg_material_at(m, i, j) == 1;
        double I = J * cells * dx * dx, bt1, br1, bt2, br2;
        ring(m, c, c, 0.5 * a * dx, &bt1, &br1);
        ring(m, c, c, 3.0 * a * dx, &bt2, &br2);
        double e1 = MU0 * J * 0.5 * a * dx / 2, e2 = MU0 * I / (2 * M_PI * 3.0 * a * dx);
        printf("  %d iterations; r = a/2: %.6e T against %.6e (%+.3f %%); r = 3a: %.6e T against %.6e (%+.3f %%); radial parts %.1e, %.1e\n", it, bt1, e1,
               100 * (bt1 / e1 - 1), bt2, e2, 100 * (bt2 / e2 - 1), br1, br2);
        verdict(it > 0 && fabs(bt1 / e1 - 1) < 0.01 && fabs(bt2 / e2 - 1) < 0.01, "M1");
        mg_free(m);
    }
    printf("== M2: an iron shell in a uniform field\n");
    printf("  recorded first run (2026-09-26): the field inside equal to B0, FAIL: a sector from 0 to 360 degrees had a span of zero\n"
           "  after the modulo and the shell was never painted; whole rings are now recognised\n");
    {
        MgSpec s;
        mg_spec_defaults(&s);
        const int N = 600, ra = 25, rb = 50;
        const double dx = 1e-3, c = N / 2 * dx, mur = 100, B0 = 0.1;
        s.nx = s.ny = N, s.dx = dx;
        s.nmaterials = 2;
        s.materials[1] = (MgMaterial){.mu_r = mur};
        s.regions[s.nregions++] = (MgRegion){.shape = MG_SECTOR, .a = {c, c, ra * dx, rb * dx, 0, 360}, .material = 1};
        s.applied_B[0] = B0;
        char err[256];
        Magnet *m = mg_create(&s, err, sizeof err);
        int it = mg_solve(m);
        double sb = 0;
        int k = 0;
        for (int j = 0; j < N; j++)
            for (int i = 0; i < N; i++) {
                double x = (i + 0.5) * dx - c, y = (j + 0.5) * dx - c;
                if (hypot(x, y) >= 12 * dx) continue;
                double B[2];
                mg_field(m, x + c, y + c, B);
                sb += B[0], k++;
            }
        double q = (double)ra / rb, e = 4 * mur * B0 / ((mur + 1) * (mur + 1) - (mur - 1) * (mur - 1) * q * q), Bin = sb / k;
        printf("  %d iterations; field inside %.6e T against %.6e (shielding %.4f, %+.3f %%)\n", it, Bin, e, e / B0, 100 * (Bin / e - 1));
        verdict(it > 0 && fabs(Bin / e - 1) < 0.03, "M2");
        mg_free(m);
    }
    printf("== M3: a transversely magnetised cylinder\n");
    {
        MgSpec s;
        mg_spec_defaults(&s);
        const int N = 480, a = 12;
        const double dx = 1e-3, c = N / 2 * dx;
        s.nx = s.ny = N, s.dx = dx;
        s.nmaterials = 2;
        s.materials[1] = (MgMaterial){.mu_r = 1, .Br = 1.0};
        s.regions[s.nregions++] = (MgRegion){.shape = MG_CIRCLE, .a = {c, c, a * dx}, .material = 1, .magnetisation = MG_MAG_PARALLEL, .mag_angle_deg = 90};
        char err[256];
        Magnet *m = mg_create(&s, err, sizeof err);
        int it = mg_solve(m);
        double B[2];
        mg_field(m, c, c, B);
        printf("  %d iterations; field at the centre (%.5f, %.5f) T against (0, 0.5) (%+.3f %%)\n", it, B[0], B[1], 100 * (B[1] / 0.5 - 1));
        verdict(it > 0 && fabs(B[1] / 0.5 - 1) < 0.02 && fabs(B[0]) < 0.01, "M3");
        mg_free(m);
    }
    printf("== M4: a magnet embedded in iron\n");
    printf("  recorded (2026-09-26): M4 passed with the first remanence form (+0.18 %%), yet the motor's radially magnetised rotor\n"
           "  showed 6.6 T in its iron; the remanence now rides the faces' harmonic reluctivity (docs/lab/magnet.md)\n");
    {
        MgSpec s;
        mg_spec_defaults(&s);
        const int N = 480, a = 12;
        const double dx = 1e-3, c = N / 2 * dx, mum = 1.05, mu2 = 1000;
        s.nx = s.ny = N, s.dx = dx;
        s.nmaterials = 3;
        s.materials[1] = (MgMaterial){.mu_r = mum, .Br = 1.0};
        s.materials[2] = (MgMaterial){.mu_r = mu2};
        s.regions[s.nregions++] = (MgRegion){.shape = MG_CIRCLE, .a = {c, c, 10 * a * dx}, .material = 2};
        s.regions[s.nregions++] = (MgRegion){.shape = MG_CIRCLE, .a = {c, c, a * dx}, .material = 1, .magnetisation = MG_MAG_PARALLEL, .mag_angle_deg = 90};
        char err[256];
        Magnet *m = mg_create(&s, err, sizeof err);
        int it = mg_solve(m);
        double B[2], e = 1.0 * mu2 / (mum + mu2);
        mg_field(m, c, c, B);
        printf("  %d iterations; field at the centre (%.5f, %.5f) T against (0, %.5f) (%+.3f %%)\n", it, B[0], B[1], e, 100 * (B[1] / e - 1));
        verdict(it > 0 && fabs(B[1] / e - 1) < 0.02 && fabs(B[0]) < 0.01, "M4");
        mg_free(m);
    }
    printf("== M5: a rotor spins up against a fan load\n");
    {
        double A = 1.0, c = 1e-5, J = 3e-4, tau = J / sqrt(A * c), ws = sqrt(A / c), th = 0, w = 0, worst = 0;
        MgLoad L = {J, 0, c};
        for (int k = 1; k <= 3; k += 2) {
            const int n = 5000;
            double h = (k == 1 ? tau : 2 * tau) / n;
            for (int i = 0; i < n; i++) mg_rotor_rk4(&th, &w, h, &L, const_torque, &A);
            double e = ws * tanh(k * tau * sqrt(A * c) / J);
            printf("  t = %d tau: omega %.9f rad/s against %.9f (%+.2e)\n", k, w, e, w / e - 1);
            worst = fmax(worst, fabs(w / e - 1));
        }
        verdict(worst < 1e-6, "M5");
    }
    printf("== M6, M7: a heated disk in a held ring\n");
    {
        const int N = 101, R = 40;
        const double dx = 1e-3, kc = 50, rc = 3e6, q0 = 1e7, T0 = 300;
        size_t n = (size_t)N * N;
        double *k = malloc(n * sizeof(double)), *C = malloc(n * sizeof(double)), *q = malloc(n * sizeof(double));
        unsigned char *held = malloc(n);
        for (int j = 0; j < N; j++)
            for (int i = 0; i < N; i++) {
                size_t c = (size_t)j * N + i;
                double x = i - 50, y = j - 50;
                bool in = x * x + y * y < (double)R * R;
                k[c] = kc, C[c] = rc, held[c] = !in, q[c] = in ? q0 : 0;
            }
        MThermal *t = mt_create(N, N, dx, k, C, held, T0, 1);
        double released = 0, out = 0, dt = 0.5;
        for (int s2 = 0; s2 < 20; s2++) {
            mt_step(t, dt, q);
            out += mt_held_heat(t) * dt;
            for (size_t c = 0; c < n; c++) released += q[c] * dx * dx * dt;
        }
        double stored = mt_stored(t, T0), bal = (released - stored - out) / released;
        printf("  M7: released %.6f J/m, stored %.6f, into the ring %.6f; imbalance %+.2e of the released\n", released, stored, out, bal);
        verdict(fabs(bal) < 1e-6, "M7");
        int it = mt_step(t, 1e9, q);
        it = mt_step(t, 1e9, q);
        double rb = 0;
        int nb = 0;
        for (int j = 1; j < N - 1; j++)
            for (int i = 1; i < N - 1; i++) {
                size_t c = (size_t)j * N + i;
                if (held[c] && (!held[c - 1] || !held[c + 1] || !held[c - N] || !held[c + N])) rb += hypot(i - 50, j - 50), nb++;
            }
        rb /= nb;
        printf("  recorded first run: centre +1.618 %% against the held ring placed at R; FAIL; the ring's radius is now measured\n");
        double Rm = R * dx, Tc = mt_temperature(t)[(size_t)50 * N + 50];
        double e = T0 + q0 * Rm * Rm / (4 * kc) + q0 * Rm * Rm / (2 * kc) * log(rb / R);
        printf("  held ring at a mean radius of %.3f cells\n", rb);
        printf("  M6: %d iterations; centre %.4f K against %.4f (rise %+.3f %%)\n", it, Tc, e, 100 * ((Tc - T0) / (e - T0) - 1));
        verdict(it >= 0 && fabs((Tc - T0) / (e - T0) - 1) < 0.01, "M6");
        mt_free(t);
        free(k), free(C), free(q), free(held);
    }
    printf(failures ? "mgtest: %d FAILED\n" : "mgtest: all passed\n", failures);
    return failures ? 1 : 0;
}
