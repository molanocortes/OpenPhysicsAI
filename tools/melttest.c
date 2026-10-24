/* melttest - verification of laser melting (src/lab/melt/melt.h).
 *
 * Criteria written on 2026-09-26, before the first run:
 *   M1 Rosenthal: a Gaussian beam (1/e^2 radius 30 um, 50 W absorbed) crossing a steel-like block (k 25 W/(m K), rho
 *      7800 kg/m^3, c 500 J/(kg K), no melting) at 0.2 m/s. Once quasi-steady, the temperature rise at four points of
 *      the top cells (on the track 2 radii ahead of the beam, 3 and 5 radii behind, and 3 radii to the side) within 2 %
 *      of Rosenthal's moving point source (1946) convolved with the beam's Gaussian. Half the block (the track on its
 *      symmetry plane), 5 um cells, far faces held at the initial temperature.
 *   M2 Stefan: a bar of solid at its melting point, one end raised to 200 K above it (Stefan number 0.5): the melted
 *      length within 1 % of Neumann's similarity solution s = 2 lambda sqrt(alpha t) after 0.72 s.
 *   M3 energy: a small block melted along two tracks, all faces insulated, the top losing heat by convection and
 *      radiation: the enthalpy gained equals the energy absorbed less the energy lost, to 1e-4 of the absorbed.
 *   First run (2026-09-26): all pass. M1 +0.86, -0.06, -0.04, +0.23 %; M2 -0.001 %; M3 imbalance 4e-12.
 *   Added 2026-09-27 with flow in the melt, before its first run:
 *   M4 a liquid layer 100 um deep and 800 um long under a uniform temperature gradient (its ends held 100 K apart),
 *      the top flat and free with the Marangoni stress, the bottom and ends no slip, the y faces free slip, slow
 *      enough that the flow does not bend the temperatures (Peclet 0.002): at mid-length, once steady, the velocity
 *      profile within 2 % of the surface speed of the closed form u(z) = (tau / mu) (3 z^2 / (4 H) - z / 2), tau =
 *      dsigma/dT dT/dx (the thermocapillary return flow, zero net flux).
 *   M5 energy with flow: a track melted into an insulated block with the Marangoni flow on (liquid 316L's viscosity
 *      6 mPa s and dsigma/dT -4e-4 N/(m K), demonstration values): the enthalpy gained equals the energy absorbed less
 *      the energy lost, to 1e-9 of the absorbed (the heat's advection is conservative).
 *   M6 in that run, every projection leaves |div u| h below 1e-8 of the fastest face speed.
 *   M7 in that run, the pool at the track's end is wider for its depth than the same track without flow, by at least
 *      10 % (an outward surface flow, dsigma/dT < 0, carries heat to the rim and spreads the pool: Heiple and Roper
 *      1982), a qualitative check.
 *   First run: M4 pass (1.9e-3 of the surface speed), M5 pass (3.6e-15), M7 pass (width over depth 2.44 without flow,
 *   6.80 with); M6 FAIL, 1.0e-7: the projection stopped at a residual of 1e-10 of its right side, and with the solid's
 *   coefficients some 1e8 below the liquid's that left 1e-7 of divergence where the flow is fastest. The projection now
 *   solves to 1e-12; the criterion is unchanged. The run took 11 minutes: `melttest flow` runs M4 to M7 alone,
 *   `melttest heat` M1 to M3. Then: M4 to M7 all pass, M6 8.7e-10 (M4, M5 and M7 unchanged).
 *   The projection then worked on the pool alone (faces between two cells with no liquid held at rest, the pressure
 *   solved in a window around the rest: sixty times faster). M6 FAIL, 2.0: in the first steps after the first cell
 *   melts the fastest face moves at 1e-16 to 1e-12 m/s, and round-off over that is of order one (the earlier runs
 *   passed only because the solid's creeping velocities kept the fastest speed finite); the divergence itself is
 *   round-off (3e-12 m/s at worst). AMENDED (2026-09-27, after this run, openly): M6 counts the steps from the first at
 *   which the fastest face exceeds 1 mm/s. Then all pass: M4 1.9e-3, M5 5.7e-15, M6 2.7e-12, M7 2.44 and 6.80.
 *   Added 2026-10-08 with the solid's property table (conductivity and specific heat against temperature, GOALS.md
 *   G20), before its first run (`melttest table`):
 *   M8 steady conduction in a bar 2 mm long (40 cells of 50 um) between faces held at 300 K and 1500 K, the conductivity
 *      rising linearly from 10 to 28 W/(m K) over that range (a nickel alloy's trend, demonstration values), no melting:
 *      after ten conduction times every cell's temperature within 0.2 % of the 1200 K span of Kirchhoff's closed form,
 *      integral of k dT linear along the bar.
 *   M9 a seven-point table of a nickel alloy's trend (demonstration values): temperature to enthalpy and back within
 *      1e-9 K from 100 K to the solidus; and M3's block and two tracks with this table and melting: the enthalpy gained
 *      equals the energy absorbed less the energy lost, to 1e-9 of the absorbed.
 *   M10 a table whose two points hold the constants of a run reproduces that run (a track with melting) to 1e-9 of its
 *      largest temperature rise.
 *   Added 2026-10-08 with melt_track_section (GOALS.md G20 step 2), before its first run (`melttest track`):
 *   M11 the trace of a melt isotherm: a Gaussian beam (1/e^2 radius 20 um, 40 W absorbed) crossing a block with M1's
 *      constant properties at 0.5 m/s, melting at 1700 K with no latent heat and no mushy range (so the conduction is
 *      Rosenthal's exactly), half the block on the track's plane of symmetry, 5 um cells: once steady, the width and
 *      depth that melt_track_section measures within 2 % of the trace of Rosenthal's moving point source convolved
 *      with the beam (the cross-section of the points whose largest temperature reached 1700 K, at the depth and
 *      offset of the first cell centres). melt_track_size's whole cells are reported beside it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lab/melt/melt.h"

static int failures;
static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    failures += !ok;
}

/* Rosenthal's rise for unit absorbed power at (xi, y, z) from the source (xi along the motion, z depth) */
static double rosenthal(double xi, double y, double z, double k, double a, double v) {
    double R = sqrt(xi * xi + y * y + z * z);
    return exp(-v * (xi + R) / (2 * a)) / (2 * M_PI * k * R);
}

/* the same convolved with a Gaussian beam of 1/e^2 radius w carrying power P, by a fine midpoint sum over +-3 w */
static double rosenthal_beam(double xi, double y, double z, double k, double a, double v, double w, double P) {
    const int N = 240;
    double s = 0, d = 6 * w / N;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            double u = -3 * w + (i + 0.5) * d, q = -3 * w + (j + 0.5) * d;
            double I = 2 * P / (M_PI * w * w) * exp(-2 * (u * u + q * q) / (w * w));
            s += I * rosenthal(xi - u, y - q, z, k, a, v) * d * d;
        }
    return s;
}

int main(int argc, char **argv) {
    ThreadPool *pool = pool_create(cpu_perf_count());
    char err[256];
    const bool heat = argc < 2 || !strcmp(argv[1], "heat"), flow = argc < 2 || !strcmp(argv[1], "flow"), table = argc < 2 || !strcmp(argv[1], "table");
    const bool track = argc < 2 || !strcmp(argv[1], "track");
    if (heat) {
    printf("== M1: a beam crossing a block, against Rosenthal\n");
    {
        const double h = 5e-6, w = 30e-6, k = 25, rho = 7800, c = 500, v = 0.2, P = 50, a = k / (rho * c), T0 = 300;
        MeltSpec s = {.n = {360, 80, 80}, .h = h, .origin = {-1.5e-3, 0, -0.4e-3}, .rho = rho, .c_s = c, .c_l = c, .k_s = k, .k_l = k,
                      .T_s = 1e9, .T_l = 1e9, .L = 0, .T0 = T0, .held_T = {T0, T0, -1, T0, T0, -1}, .power = P, .absorptivity = 1, .radius = w,
                      .ntracks = 1, .track = {{-1.4e-3, 0, 0, 0, v, 0}}};
        Melt *M = melt_create(&s, err, sizeof err);
        if (!M) {
            printf("  %s\n", err);
            verdict(false, "M1");
        } else {
            double dt = melt_stable_dt(M), tend = 1.4e-3 / v * 0.9999;
            long steps = (long)ceil(tend / dt);
            dt = tend / steps;
            for (long i = 0; i < steps; i++) melt_step(M, dt, pool);
            double bx, by;
            melt_beam_at(M, melt_time(M) - 0.5 * dt, &bx, &by);
            float *T = malloc(melt_cells(M) * sizeof(float));
            melt_fields(M, T, NULL, NULL);
            /* probes: (xi, y) in beam radii; the top cell, depth h / 2 */
            const double pr[4][2] = {{2, 0}, {-3, 0}, {-5, 0}, {0, 3}};
            double worst = 0;
            for (int p = 0; p < 4; p++) {
                int i = (int)floor((bx + pr[p][0] * w - s.origin[0]) / h), j = (int)floor(pr[p][1] * w / h);
                double xc = s.origin[0] + (i + 0.5) * h, yc = (j + 0.5) * h;
                double num = T[((size_t)(s.n[2] - 1) * s.n[1] + j) * s.n[0] + i] - T0, ref = rosenthal_beam(xc - bx, yc, 0.5 * h, k, a, v, w, P);
                printf("  xi %+.0f um, y %.0f um: rise %.2f K against %.2f K (%+.2f %%)\n", 1e6 * (xc - bx), 1e6 * yc, num, ref, 100 * (num / ref - 1));
                worst = fmax(worst, fabs(num / ref - 1));
            }
            double ab, lo, ho;
            melt_energy(M, &ab, &lo, &ho);
            printf("  %ld steps of %.3g s; %zu cells; absorbed %.4g J (half the beam), out through held faces %.4g J\n", steps, dt, melt_cells(M), ab, ho);
            verdict(worst < 0.02, "M1");
            free(T);
            melt_free(M);
        }
    }
    printf("== M2: melting from a hot wall, against Neumann\n");
    {
        const double k = 100, rho = 2500, c = 1000, L = 4e5, Tm = 1000, Tw = 1200, a = k / (rho * c), h = 25e-6, tend = 0.72;
        const double St = c * (Tw - Tm) / L;
        double lo = 0.01, hi = 2; /* lambda exp(lambda^2) erf(lambda) = St / sqrt(pi) */
        for (int it = 0; it < 200; it++) {
            double m = 0.5 * (lo + hi);
            *(m * exp(m * m) * erf(m) < St / sqrt(M_PI) ? &lo : &hi) = m;
        }
        double lam = 0.5 * (lo + hi), exact = 2 * lam * sqrt(a * tend);
        MeltSpec s = {.n = {400, 1, 1}, .h = h, .rho = rho, .c_s = c, .c_l = c, .k_s = k, .k_l = k, .T_s = Tm, .T_l = Tm, .L = L, .T0 = Tm,
                      .held_T = {Tw, -1, -1, -1, -1, -1}};
        Melt *M = melt_create(&s, err, sizeof err);
        if (!M) {
            printf("  %s\n", err);
            verdict(false, "M2");
        } else {
            double dt = melt_stable_dt(M);
            long steps = (long)ceil(tend / dt);
            dt = tend / steps;
            for (long i = 0; i < steps; i++) melt_step(M, dt, pool);
            float *f = malloc(melt_cells(M) * sizeof(float));
            melt_fields(M, NULL, f, NULL);
            double sl = 0;
            for (size_t i = 0; i < melt_cells(M); i++) sl += f[i] * h;
            printf("  Stefan %.2f, lambda %.5f; melted %.5f mm against %.5f mm (%+.3f %%), %ld steps\n", St, lam, 1e3 * sl, 1e3 * exact, 100 * (sl / exact - 1), steps);
            verdict(fabs(sl / exact - 1) < 0.01, "M2");
            free(f);
            melt_free(M);
        }
    }
    printf("== M3: energy with melting, two tracks and surface losses\n");
    {
        MeltSpec s = {.n = {80, 50, 30}, .h = 10e-6, .origin = {0, 0, -0.3e-3}, .rho = 7950, .c_s = 500, .c_l = 800, .k_s = 20, .k_l = 30,
                      .T_s = 1658, .T_l = 1723, .L = 2.7e5, .T0 = 300, .held_T = {-1, -1, -1, -1, -1, -1}, .h_conv = 50, .T_amb = 300,
                      .emissivity = 0.4, .power = 150, .absorptivity = 0.35, .radius = 40e-6, .ntracks = 2,
                      .track = {{0.1e-3, 0.18e-3, 0.7e-3, 0.18e-3, 0.8, 0}, {0.7e-3, 0.30e-3, 0.1e-3, 0.30e-3, 0.8, 1e-4}}};
        Melt *M = melt_create(&s, err, sizeof err);
        if (!M) {
            printf("  %s\n", err);
            verdict(false, "M3");
        } else {
            double H0 = 0, H1 = 0, dt = melt_stable_dt(M), V = pow(s.h, 3);
            for (size_t i = 0; i < melt_cells(M); i++) H0 += melt_enthalpy(M)[i] * V;
            long steps = (long)ceil(2.4e-3 / dt);
            float *f = malloc(melt_cells(M) * sizeof(float)), *fm = malloc(melt_cells(M) * sizeof(float));
            double fpeak = 0;
            for (long i = 0; i < steps; i++) {
                melt_step(M, dt, pool);
                if (i % 50 == 0) {
                    melt_fields(M, NULL, f, NULL);
                    for (size_t c = 0; c < melt_cells(M); c++) fpeak = fmax(fpeak, f[c]);
                }
            }
            for (size_t i = 0; i < melt_cells(M); i++) H1 += melt_enthalpy(M)[i] * V;
            double ab, lo, ho;
            melt_energy(M, &ab, &lo, &ho);
            melt_fields(M, NULL, f, fm);
            size_t melted = 0;
            for (size_t c = 0; c < melt_cells(M); c++) melted += fm[c] >= 0.5;
            double bal = (H1 - H0) - (ab - lo - ho);
            printf("  absorbed %.6g J, lost %.4g J, stored %.6g J; imbalance %.3g J (%.2g of absorbed); %zu cells melted and resolidified, "
                   "largest liquid fraction %.2f\n", ab, lo, H1 - H0, bal, fabs(bal) / ab, melted, fpeak);
            verdict(fabs(bal) / ab < 1e-4 && melted > 0, "M3");
            free(f), free(fm);
            melt_free(M);
        }
    }
    }
    if (table) {
    printf("== M8: steady conduction with k(T), against Kirchhoff\n");
    {
        const double T1 = 300, T2 = 1500, h = 50e-6, L = 40 * h, kA = 10, b = (28.0 - 10.0) / (T2 - T1);
        MeltSpec s = {.n = {40, 1, 1}, .h = h, .rho = 8440, .c_s = 500, .c_l = 700, .k_s = 15, .k_l = 30, .T_s = 1e6, .T_l = 1e6, .L = 0, .T0 = 900,
                      .held_T = {T1, T2, -1, -1, -1, -1}, .nprop = 2, .prop_T = {T1, T2}, .prop_k = {10, 28}, .prop_c = {400, 700}};
        Melt *M = melt_create(&s, err, sizeof err);
        if (!M) {
            printf("  %s\n", err);
            verdict(false, "M8");
        } else {
            double tc = L * L / (kA / (8440.0 * 400)), tend = 10 * tc, dt = melt_stable_dt(M);
            long steps = (long)ceil(tend / dt);
            dt = tend / steps;
            float *T = malloc(40 * sizeof(float)), *Tp = malloc(40 * sizeof(float));
            for (long i = 0; i < steps; i++) {
                melt_step(M, dt, pool);
                if (i == steps - steps / 10) melt_fields(M, Tp, NULL, NULL);
            }
            melt_fields(M, T, NULL, NULL);
            double U2 = kA * (T2 - T1) + 0.5 * b * (T2 - T1) * (T2 - T1), worst = 0, drift = 0;
            for (int i = 0; i < 40; i++) {
                double x = (i + 0.5) * h, U = U2 * x / L, d = (-kA + sqrt(kA * kA + 2 * b * U)) / b, ex = T1 + d;
                worst = fmax(worst, fabs(T[i] - ex)), drift = fmax(drift, fabs(T[i] - Tp[i]));
                if (i % 8 == 0 || i == 39) printf("  x %6.0f um: %.3f K against %.3f K\n", x * 1e6, T[i], ex);
            }
            printf("  %ld steps over %.2f s (10 conduction times); largest error %.4f K (%.4f %% of the span); change over the last tenth %.2e K\n",
                   steps, tend, worst, 100 * worst / (T2 - T1), drift);
            verdict(worst <= 0.002 * (T2 - T1), "M8");
            free(T), free(Tp);
            melt_free(M);
        }
    }
    /* a nickel alloy's trend, demonstration values: conductivity and specific heat rise with temperature */
    const double tT[7] = {300, 500, 700, 900, 1100, 1300, 1563}, tk[7] = {9.8, 12.5, 15.5, 18.5, 21.5, 24.5, 28}, tc7[7] = {410, 450, 490, 530, 570, 610, 650};
    printf("== M9: enthalpy with a property table: round trips and the energy of two tracks\n");
    {
        MeltSpec s = {.n = {80, 50, 30}, .h = 10e-6, .origin = {0, 0, -0.3e-3}, .rho = 8440, .c_s = 500, .c_l = 720, .k_s = 20, .k_l = 30,
                      .T_s = 1563, .T_l = 1623, .L = 2.9e5, .T0 = 300, .held_T = {-1, -1, -1, -1, -1, -1}, .h_conv = 50, .T_amb = 300,
                      .emissivity = 0.4, .power = 150, .absorptivity = 0.35, .radius = 40e-6, .ntracks = 2,
                      .track = {{0.1e-3, 0.18e-3, 0.7e-3, 0.18e-3, 0.8, 0}, {0.7e-3, 0.30e-3, 0.1e-3, 0.30e-3, 0.8, 1e-4}}, .nprop = 7};
        memcpy(s.prop_T, tT, sizeof tT), memcpy(s.prop_k, tk, sizeof tk), memcpy(s.prop_c, tc7, sizeof tc7);
        Melt *M = melt_create(&s, err, sizeof err);
        if (!M) {
            printf("  %s\n", err);
            verdict(false, "M9");
        } else {
            double rt = 0;
            for (int i = 0; i <= 2000; i++) {
                double T = 100 + (s.T_s - 100) * i / 2000.0;
                rt = fmax(rt, fabs(melt_T_of_H(M, melt_H_of_T(M, T)) - T));
            }
            double H0 = 0, H1 = 0, dt = melt_stable_dt(M), V = pow(s.h, 3);
            for (size_t i = 0; i < melt_cells(M); i++) H0 += melt_enthalpy(M)[i] * V;
            long steps = (long)ceil(2.4e-3 / dt);
            for (long i = 0; i < steps; i++) melt_step(M, dt, pool);
            for (size_t i = 0; i < melt_cells(M); i++) H1 += melt_enthalpy(M)[i] * V;
            double ab, lo, ho;
            melt_energy(M, &ab, &lo, &ho);
            float *fm = malloc(melt_cells(M) * sizeof(float));
            melt_fields(M, NULL, NULL, fm);
            size_t melted = 0;
            for (size_t c = 0; c < melt_cells(M); c++) melted += fm[c] >= 0.5;
            double bal = (H1 - H0) - (ab - lo - ho);
            printf("  round trip from 100 K to the solidus: worst %.2e K; absorbed %.6g J, lost %.4g J, stored %.6g J, imbalance %.2e of absorbed; %zu cells "
                   "melted\n", rt, ab, lo, H1 - H0, fabs(bal) / ab, melted);
            verdict(rt <= 1e-9 && fabs(bal) / ab <= 1e-9 && melted > 0, "M9");
            free(fm);
            melt_free(M);
        }
    }
    printf("== M10: a table of constants reproduces the constants\n");
    {
        MeltSpec s = {.n = {80, 40, 30}, .h = 10e-6, .origin = {0, 0, -0.3e-3}, .rho = 7950, .c_s = 500, .c_l = 800, .k_s = 20, .k_l = 30,
                      .T_s = 1658, .T_l = 1723, .L = 2.7e5, .T0 = 300, .held_T = {-1, -1, -1, -1, -1, -1}, .h_conv = 50, .T_amb = 300,
                      .emissivity = 0.4, .power = 150, .absorptivity = 0.35, .radius = 40e-6, .ntracks = 1,
                      .track = {{0.1e-3, 0.2e-3, 0.7e-3, 0.2e-3, 0.8, 0}}};
        MeltSpec t = s;
        t.nprop = 2, t.prop_T[0] = 200, t.prop_T[1] = 2500, t.prop_k[0] = t.prop_k[1] = s.k_s, t.prop_c[0] = t.prop_c[1] = s.c_s;
        Melt *A = melt_create(&s, err, sizeof err), *B = melt_create(&t, err, sizeof err);
        if (!A || !B) {
            printf("  %s\n", err);
            verdict(false, "M10");
        } else {
            double dt = melt_stable_dt(A);
            long steps = (long)ceil(0.9e-3 / dt);
            for (long i = 0; i < steps; i++) melt_step(A, dt, pool), melt_step(B, dt, pool);
            float *Ta = malloc(melt_cells(A) * sizeof(float)), *Tb = malloc(melt_cells(B) * sizeof(float));
            melt_fields(A, Ta, NULL, NULL), melt_fields(B, Tb, NULL, NULL);
            double diff = 0, rise = 0;
            for (size_t c = 0; c < melt_cells(A); c++) diff = fmax(diff, fabs((double)Ta[c] - Tb[c])), rise = fmax(rise, Ta[c] - 300.0);
            printf("  %ld steps; largest difference %.3e K against a rise of %.1f K (%.1e)\n", steps, diff, rise, diff / rise);
            verdict(diff <= 1e-9 * rise, "M10");
            free(Ta), free(Tb);
        }
        melt_free(A), melt_free(B);
    }
    }
    if (flow) {
    printf("== M4: the thermocapillary return flow in a liquid layer\n");
    {
        const double h = 5e-6, H = 100e-6, L = 800e-6, rho = 7000, c = 800, k = 30, mu = 0.05, dsdT = -1.6e-6, T1 = 2000, T2 = 2100;
        MeltSpec s = {.n = {160, 2, 20}, .h = h, .origin = {0, 0, 0}, .rho = rho, .c_s = c, .c_l = c, .k_s = k, .k_l = k, .T_s = 1000, .T_l = 1000,
                      .L = 2e5, .T0 = 2050, .held_T = {T1, T2, -1, -1, -1, -1}, .flow = 1, .mu = mu, .dsigma_dT = dsdT, .slip_y = 1};
        Melt *M = melt_create(&s, err, sizeof err);
        if (!M) {
            printf("  %s\n", err);
            return 1;
        }
        double *Hm = melt_enthalpy_rw(M); /* the conduction's steady state: linear between the held ends */
        for (int kk = 0; kk < 20; kk++)
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 160; i++) Hm[i + 160 * (j + 2 * kk)] = melt_H_of_T(M, T1 + (T2 - T1) * (i + 0.5) / 160);
        const double tau = dsdT * (T2 - T1) / L, us = tau * H / (4 * mu), tv = H * H / (mu / rho);
        while (melt_time(M) < 6 * tv) melt_step(M, melt_stable_dt(M), pool);
        double worst = 0;
        for (int kk = 0; kk < 20; kk++) {
            double z = (kk + 0.5) * h, v[3], v2[3];
            melt_velocity(M, 79, 0, kk, v), melt_velocity(M, 80, 0, kk, v2);
            double ux = 0.5 * (v[0] + v2[0]), ex = tau / mu * (3 * z * z / (4 * H) - z / 2);
            worst = fmax(worst, fabs(ux - ex) / fabs(us));
            if (kk % 4 == 0 || kk == 19) printf("  z %5.1f um: u %+.4e m/s against %+.4e\n", z * 1e6, ux, ex);
        }
        printf("  surface speed %.4e m/s; after %.4f s (6 viscous times); largest error %.2e of it\n", us, melt_time(M), worst);
        verdict(worst < 0.02, "M4");
        melt_free(M);
    }
    printf("== M5, M6, M7: a track with Marangoni flow\n");
    {
        double ratio[2] = {0}, bal = 1, ab = 1, worst_div = 0;
        for (int flow = 0; flow < 2; flow++) {
            MeltSpec s = {.n = {120, 60, 30}, .h = 5e-6, .origin = {0, 0, -150e-6}, .rho = 7904, .c_s = 580, .c_l = 800, .k_s = 22, .k_l = 30,
                          .T_s = 1675, .T_l = 1708, .L = 2.9e5, .T0 = 293.15, .held_T = {-1, -1, -1, -1, -1, -1}, .h_conv = 10, .T_amb = 293.15,
                          .emissivity = 0.4, .power = 200, .absorptivity = 0.35, .radius = 40e-6, .ntracks = 1,
                          .track = {{100e-6, 150e-6, 500e-6, 150e-6, 0.8, 0}}, .flow = flow, .mu = 6e-3, .dsigma_dT = -4e-4};
            Melt *M = melt_create(&s, err, sizeof err);
            if (!M) {
                printf("  %s\n", err);
                return 1;
            }
            double E0 = 0;
            for (size_t c = 0; c < melt_cells(M); c++) E0 += melt_enthalpy(M)[c];
            while (melt_time(M) < 0.5e-3) {
                melt_step(M, melt_stable_dt(M), pool);
                if (flow && melt_max_speed(M) > 1e-3) worst_div = fmax(worst_div, melt_divergence_error(M));
            }
            double len, wid, dep, a, l, hd, E = 0;
            melt_pool_size(M, &len, &wid, &dep);
            melt_energy(M, &a, &l, &hd);
            for (size_t c = 0; c < melt_cells(M); c++) E += melt_enthalpy(M)[c];
            E = (E - E0) * pow(5e-6, 3);
            ratio[flow] = dep > 0 ? wid / dep : 0;
            printf("  %s: pool %.0f um long, %.0f wide, %.0f deep; fastest %.3f m/s; energy: absorbed %.6e J, lost %.3e, stored %.6e (%.1e)\n",
                   flow ? "with flow" : "conduction", len * 1e6, wid * 1e6, dep * 1e6, melt_max_speed(M), a, l, E, fabs(E - (a - l)) / a);
            if (flow) bal = fabs(E - (a - l)), ab = a;
            melt_free(M);
        }
        printf("  width over depth: %.3f without flow, %.3f with (%+.1f %%); the largest |div u| h / u_max %.2e\n", ratio[0], ratio[1],
               100 * (ratio[1] / ratio[0] - 1), worst_div);
        verdict(bal / ab < 1e-9, "M5");
        verdict(worst_div < 1e-8, "M6");
        verdict(ratio[1] > 1.1 * ratio[0], "M7");
    }
    }
    if (track) {
    printf("== M11: the trace a melt isotherm leaves, against Rosenthal\n");
    {
        const double h = 5e-6, w = 20e-6, k = 25, rho = 7800, c = 500, v = 0.5, P = 40, a = k / (rho * c), T0 = 300, Tm = 1700;
        /* the reference: the steady field's largest temperature over the passage, at the first cell centres' depth (for
         * the width) and offset from the plane of symmetry (for the depth), found by bisection on the boundary and a
         * golden-section search for the peak along the track */
        double zc = 0.5 * h, yc = 0.5 * h;
        double ref[2];
        for (int q = 0; q < 2; q++) {
            double lo = 1e-6, hi = 400e-6;
            for (int it = 0; it < 40; it++) {
                double m = 0.5 * (lo + hi), y = q == 0 ? m : yc, z = q == 0 ? zc : m;
                double xa = -600e-6, xb = 60e-6, g = 0.5 * (sqrt(5.0) - 1);
                double x1 = xb - g * (xb - xa), x2 = xa + g * (xb - xa);
                double f1 = rosenthal_beam(x1, y, z, k, a, v, w, P), f2 = rosenthal_beam(x2, y, z, k, a, v, w, P);
                for (int s2 = 0; s2 < 40; s2++) {
                    if (f1 > f2) xb = x2, x2 = x1, f2 = f1, x1 = xb - g * (xb - xa), f1 = rosenthal_beam(x1, y, z, k, a, v, w, P);
                    else xa = x1, x1 = x2, f1 = f2, x2 = xa + g * (xb - xa), f2 = rosenthal_beam(x2, y, z, k, a, v, w, P);
                }
                *(T0 + fmax(f1, f2) >= Tm ? &lo : &hi) = m;
            }
            ref[q] = 0.5 * (lo + hi);
        }
        double ref_w = 2 * ref[0], ref_d = ref[1];
        MeltSpec s = {.n = {330, 60, 60}, .h = h, .origin = {-1.5e-3, 0, -0.3e-3}, .rho = rho, .c_s = c, .c_l = c, .k_s = k, .k_l = k,
                      .T_s = Tm, .T_l = Tm, .L = 0, .T0 = T0, .held_T = {T0, T0, -1, T0, T0, -1}, .power = P, .absorptivity = 1, .radius = w,
                      .ntracks = 1, .track = {{-1.4e-3, 0, 0, 0, v, 0}}};
        Melt *M = melt_create(&s, err, sizeof err);
        if (!M) {
            printf("  %s\n", err);
            verdict(false, "M11");
        } else {
            double dt = melt_stable_dt(M), tend = 1.4e-3 / v * 0.9999;
            long steps = (long)ceil(tend / dt);
            dt = tend / steps;
            for (long i = 0; i < steps; i++) melt_step(M, dt, pool);
            double W, D, Wm, Dm, Wc, Dc;
            bool touch = false, got = melt_track_section(M, -0.6e-3, -0.5e-3, true, &W, &D, &Wm, &Dm, &touch);
            melt_track_size(M, -0.6e-3, -0.5e-3, &Wc, &Dc);
            /* melt_track_size counts the whole half block's cells from y = 0: twice that is the full width */
            printf("  Rosenthal's trace: width %.2f um, depth %.2f um\n", 1e6 * ref_w, 1e6 * ref_d);
            printf("  melt_track_section: width %.2f um (%+.2f %%), depth %.2f um (%+.2f %%); the largest of the sections %.2f and %.2f%s\n",
                   1e6 * W, 100 * (W / ref_w - 1), 1e6 * D, 100 * (D / ref_d - 1), 1e6 * Wm, 1e6 * Dm, touch ? "; the melt touched a face" : "");
            printf("  melt_track_size's whole cells: width %.1f um (%+.1f %%), depth %.1f um (%+.1f %%)\n", 2e6 * Wc, 100 * (2 * Wc / ref_w - 1), 1e6 * Dc,
                   100 * (Dc / ref_d - 1));
            printf("  %ld steps of %.3g s, %zu cells\n", steps, dt, melt_cells(M));
            verdict(got && !touch && fabs(W / ref_w - 1) < 0.02 && fabs(D / ref_d - 1) < 0.02, "M11");
            melt_free(M);
        }
    }
    }
    pool_destroy(pool); /* at the end: the flow cases used to run after the heat cases had destroyed it */
    printf(failures ? "melttest: %d FAILED\n" : "melttest: all passed\n", failures);
    return failures ? 1 : 0;
}
