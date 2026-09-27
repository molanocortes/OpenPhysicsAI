/* hftest.c - verification of the heat-and-flow solver (src/lab/heat).
 *
 * Criteria, written on 2026-09-26 before the first run and not to be changed without a dated note:
 *   H1  natural convection in a square cavity, hot and cold side walls, adiabatic top and bottom, Pr 0.71: the mean
 *       Nusselt number of the hot wall within 2 % of de Vahl Davis's benchmark (Int. J. Numer. Meth. Fluids 3, 1983;
 *       2.243 at Ra 1e4, 4.519 at Ra 1e5, as tabulated by Wasberg et al., arXiv physics/0305049, table 1), on 64 and
 *       128 cells across; the hot and cold walls' Nusselt numbers within 1 % of each other (the energy balance).
 *   H2  conduction through a wall of three layers (1, 5 and 0.5 W/(m K)) between fixed temperatures: the steady heat
 *       flow within 0.1 % of dT / sum(L / k), and each interface temperature within 0.1 % of dT of its closed form.
 *   H3  a channel driven by a uniform body force between no-slip walls: the centre-line speed within 1 % of
 *       a H^2 / (8 nu) (plane Poiseuille flow).
 *   H4  forced convection between parallel isothermal plates with a parabolic inflow: far downstream, the local
 *       Nusselt number on the hydraulic diameter within 3 % of the fully developed 7.541 (Shah and London, Laminar Flow
 *       Forced Convection in Ducts, 1978).
 *   H5  (added 2026-09-26 with the rotors, before their first run) Taylor-Couette flow: a disk of radius R1 = 30 cells
 *       turning inside a fixed ring of radius R2 = 60 cells at Re 2.5: the torque per unit length within 3 % of
 *       4 pi mu omega R1^2 R2^2 / (R2^2 - R1^2), and the speed at mid-gap within 2 % of A r + B / r. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/lab/heat/heat.h"

static int failures;

static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    fflush(stdout);
    if (!ok) failures++;
}

static double wall(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

/* the cavity at a Rayleigh number on N cells: returns the hot and cold walls' Nusselt numbers */
static bool cavity(double Ra, int N, double *nu_h, double *nu_c, double *tsec) {
    HtSpec s;
    ht_spec_defaults(&s);
    const double nu = 1.5e-5, Pr = 0.71, beta = 1.0 / 300, g = 9.81, dT = 1.0;
    s.nu = nu, s.alpha = nu / Pr, s.rho_cp = 1200, s.beta = beta, s.g[1] = -g;
    double H = cbrt(Ra * nu * s.alpha / (g * beta * dT));
    s.nx = s.ny = N, s.dx = H / N;
    s.T_ref = s.T_init = 300;
    s.u_ref = sqrt(g * beta * dT * H); /* the free-fall speed */
    s.npatches = 2;
    s.patches[0] = (HtPatch){.side = 0, .from = 0, .to = H, .flow = HT_WALL, .thermal = HT_FIXED_T, .T = 300.5};
    s.patches[1] = (HtPatch){.side = 1, .from = 0, .to = H, .flow = HT_WALL, .thermal = HT_FIXED_T, .T = 299.5};
    char err[256];
    Heat *h = ht_create(&s, err, sizeof err);
    if (!h) {
        printf("  %s\n", err);
        return false;
    }
    double k = s.alpha * s.rho_cp, t_conv = H / s.u_ref, prev = 0;
    long chunk = (long)(2 * t_conv / ht_dt(h));
    bool ok = true;
    for (int it = 0; it < 400 && ok; it++) { /* until the Nusselt number stops changing: at most 800 free-fall times */
        ok = ht_advance(h, chunk);
        *nu_h = ht_patch_heat(h, 0) / (k * dT), *nu_c = -ht_patch_heat(h, 1) / (k * dT);
        if (it > 5 && fabs(*nu_h - prev) < 1e-5 * *nu_h && fabs(*nu_h - *nu_c) < 2e-3 * *nu_h) break;
        prev = *nu_h;
    }
    *tsec = ht_time(h) / t_conv;
    ht_free(h);
    return ok;
}

int main(int argc, char **argv) {
    if (argc == 2 && argv[1][0] == 'H') goto h5_only; /* hftest H5: run the last case alone */
    if (argc == 3) { /* exploration outside the verdicts: hftest RA N */
        double nh, nc, tc, c0 = wall();
        bool ok = cavity(atof(argv[1]), atoi(argv[2]), &nh, &nc, &tc);
        printf("Ra %s N %s: Nu hot %.5f cold %.5f after %.0f free-fall times, %s, %.0f s\n", argv[1], argv[2], nh, nc, tc, ok ? "stable" : "UNSTABLE", wall() - c0);
        return 0;
    }
    printf("== H1: natural convection in a square cavity against de Vahl Davis (1983)\n");
    printf("  recorded first run (2026-09-26, heat advected in conservative form): Ra 1e4 on 64 cells Nu 2.172 (-3.2 %%), FAIL; on 32 cells no\n"
           "  convection at all, temperatures outside the walls' range (docs/lab/heat.md)\n");
    {
        const double Ra[2] = {1e4, 1e5}, ref[2] = {2.243, 4.519};
        const int N[2] = {64, 128};
        for (int r = 0; r < 2; r++) {
            double c0 = wall(), nh = 0, nc = 0, tc = 0;
            bool ok = cavity(Ra[r], N[r], &nh, &nc, &tc);
            printf("  Ra %.0e on %d cells: Nu hot %.4f, cold %.4f (benchmark %.3f, %+.2f %%), after %.0f free-fall times, %.1f s\n", Ra[r], N[r], nh, nc, ref[r],
                   100 * (nh / ref[r] - 1), tc, wall() - c0);
            char name[32];
            snprintf(name, sizeof name, "H1 Ra %.0e", Ra[r]);
            verdict(ok && fabs(nh / ref[r] - 1) < 0.02 && fabs(nh / nc - 1) < 0.01, name);
        }
    }
    printf("== H2: conduction through three layers\n");
    {
        HtSpec s;
        ht_spec_defaults(&s);
        const double dx = 1e-3, k[3] = {1.0, 5.0, 0.5}, L = 10 * dx;
        s.nx = 30, s.ny = 4, s.dx = dx, s.u_ref = 5e-5, s.T_init = 300, s.T_ref = 300;
        s.nmaterials = 4;
        for (int m = 0; m < 3; m++) {
            s.materials[m + 1] = (HtMaterial){.kind = HT_SOLID, .k = k[m], .rho_cp = 1e6};
            s.regions[s.nregions++] = (HtRegion){.lo = {m * L, 0}, .hi = {(m + 1) * L, 1}, .material = m + 1};
        }
        s.npatches = 2;
        s.patches[0] = (HtPatch){.side = 0, .from = 0, .to = 1, .flow = HT_WALL, .thermal = HT_FIXED_T, .T = 310};
        s.patches[1] = (HtPatch){.side = 1, .from = 0, .to = 1, .flow = HT_WALL, .thermal = HT_FIXED_T, .T = 300};
        char err[256];
        Heat *h = ht_create(&s, err, sizeof err);
        double R = L / k[0] + L / k[1] + L / k[2], q = 10 / R; /* W/m^2 */
        double Tq1 = 310 - q * L / k[0], Tq2 = Tq1 - q * L / k[1];
        ht_advance(h, 20000);
        double Q = ht_patch_heat(h, 0) / (s.ny * dx), Qout = -ht_patch_heat(h, 1) / (s.ny * dx);
        /* interface temperatures: the harmonic interpolation between the two cells beside the interface */
        const double *T = ht_temperature(h);
        int j = 1;
        double a = T[j * s.nx + 9], b = T[j * s.nx + 10], T1 = (k[0] * a + k[1] * b) / (k[0] + k[1]);
        a = T[j * s.nx + 19], b = T[j * s.nx + 20];
        double T2 = (k[1] * a + k[2] * b) / (k[1] + k[2]);
        printf("  heat flow %.5f W/m^2 in, %.5f out, closed form %.5f (%+.4f %%); interfaces %.4f and %.4f K against %.4f and %.4f\n", Q, Qout, q,
               100 * (Q / q - 1), T1, T2, Tq1, Tq2);
        verdict(fabs(Q / q - 1) < 1e-3 && fabs(Qout / q - 1) < 1e-3 && fabs(T1 - Tq1) < 0.01 && fabs(T2 - Tq2) < 0.01, "H2");
        ht_free(h);
    }
    printf("== H3: plane Poiseuille flow driven by a body force\n");
    {
        HtSpec s;
        ht_spec_defaults(&s);
        const int N = 32;
        const double H = 0.01, nu = 1e-5, a = 1e-3;
        s.nx = 8, s.ny = N, s.dx = H / N, s.nu = nu, s.beta = 0, s.periodic_x = true;
        double umax = a * H * H / (8 * nu);
        s.u_ref = 2 * umax;
        s.regions[s.nregions++] = (HtRegion){.lo = {0, 0}, .hi = {1, 1}, .material = -1, .force = {a, 0}};
        char err[256];
        Heat *h = ht_create(&s, err, sizeof err);
        ht_advance(h, (long)(2 * H * H / nu / ht_dt(h)));
        double u[2];
        ht_velocity(h, 4, N / 2, u);
        double y = (N / 2 + 0.5) * s.dx, exact = a / (2 * nu) * y * (H - y);
        printf("  speed at y = %.5f m: %.6f m/s, closed form %.6f (%+.3f %%)\n", y, u[0], exact, 100 * (u[0] / exact - 1));
        verdict(!ht_unstable(h) && fabs(u[0] / exact - 1) < 0.01, "H3");
        ht_free(h);
    }
    printf("== H4: forced convection between isothermal plates, fully developed Nusselt number\n");
    {
        double c0 = wall();
        HtSpec s;
        ht_spec_defaults(&s);
        const int N = 20, Lc = 40 * N;
        const double H = 0.01, nu = 1.5e-5, alpha = 2.1e-5, U = 0.105; /* Pe on the hydraulic diameter: U 2H / alpha = 100 */
        s.nx = Lc, s.ny = N, s.dx = H / N, s.nu = nu, s.alpha = alpha, s.beta = 0, s.T_init = 300, s.T_ref = 300;
        s.u_ref = 1.5 * U;
        s.npatches = 4;
        s.patches[0] = (HtPatch){.side = 0, .from = 0, .to = H, .flow = HT_INLET, .velocity = U, .parabolic = true, .T = 300};
        s.patches[1] = (HtPatch){.side = 1, .from = 0, .to = H, .flow = HT_OUTLET};
        s.patches[2] = (HtPatch){.side = 2, .from = 0, .to = 1, .flow = HT_WALL, .thermal = HT_FIXED_T, .T = 310};
        s.patches[3] = (HtPatch){.side = 3, .from = 0, .to = 1, .flow = HT_WALL, .thermal = HT_FIXED_T, .T = 310};
        char err[256];
        Heat *h = ht_create(&s, err, sizeof err);
        double Lx = Lc * s.dx;
        ht_advance(h, (long)(3 * Lx / U / ht_dt(h)));
        /* local Nusselt number at x = 10 H from the inlet: wall flux over (T_wall - T_bulk), on D_h = 2 H */
        const double *T = ht_temperature(h);
        double k = alpha * s.rho_cp, Dh = 2 * H;
        int i = 10 * N;
        double num = 0, den = 0;
        for (int j = 0; j < N; j++) {
            double u[2];
            ht_velocity(h, i, j, u);
            num += u[0] * T[j * s.nx + i], den += u[0];
        }
        double Tb = num / den;
        double qw = k * (310 - T[i]) * 2 / s.dx; /* the lower wall's flux, W/m^2 */
        double Nu = qw / (310 - Tb) * Dh / k;
        printf("  x = 10 H: bulk temperature %.4f K, wall flux %.2f W/m^2, Nu %.4f (fully developed 7.541, %+.2f %%); %.1f s\n", Tb, qw, Nu,
               100 * (Nu / 7.541 - 1), wall() - c0);
        verdict(!ht_unstable(h) && fabs(Nu / 7.541 - 1) < 0.03, "H4");
        ht_free(h);
    }
h5_only:
    printf("== H5: Taylor-Couette flow, a turning disk inside a fixed ring\n");
    {
        HtSpec s;
        ht_spec_defaults(&s);
        const int N = 132;
        const double dx = 1e-3, nu = 1e-4, rho = 1000, R1 = 30 * dx, R2 = 60 * dx, w = 0.01 / R1;
        const double cx = N / 2 * dx, cy = N / 2 * dx;
        s.nx = s.ny = N, s.dx = dx, s.nu = nu, s.rho = rho, s.beta = 0, s.u_ref = 0.02;
        s.nmaterials = 2;
        s.materials[1] = (HtMaterial){.kind = HT_SOLID, .k = 1, .rho_cp = 1e6};
        s.regions[s.nregions++] = (HtRegion){.lo = {0, 0}, .hi = {1, 1}, .material = 1};
        s.regions[s.nregions++] = (HtRegion){.lo = {cx, cy}, .hi = {R2, 0}, .material = 0, .circle = true};
        s.nrotors = 1;
        s.rotors[0] = (HtRotor){.c = {cx, cy}, .omega = w, .material = 1, .hub_r = R1};
        char err[256];
        Heat *h = ht_create(&s, err, sizeof err);
        ht_advance(h, (long)(8 * (R2 - R1) * (R2 - R1) / nu / ht_dt(h)));
        double T = ht_rotor_torque(h, 0), Te = 4 * M_PI * rho * nu * w * R1 * R1 * R2 * R2 / (R2 * R2 - R1 * R1);
        /* mean azimuthal speed on the ring of cells at mid-gap */
        double rm = 0.5 * (R1 + R2), A = -w * R1 * R1 / (R2 * R2 - R1 * R1), B = w * R1 * R1 * R2 * R2 / (R2 * R2 - R1 * R1);
        double su = 0, sw = 0;
        for (int j = 0; j < N; j++)
            for (int i = 0; i < N; i++) {
                double x = (i + 0.5) * dx - cx, y = (j + 0.5) * dx - cy, r = hypot(x, y);
                if (fabs(r - rm) > 0.5 * dx) continue;
                double u[2];
                ht_velocity(h, i, j, u);
                su += (-y * u[0] + x * u[1]) / r, sw += 1;
            }
        double ut = su / sw, ute = A * rm + B / rm;
        printf("  torque %.5e N m/m against %.5e (%+.2f %%, opposing the rotation: %s); speed at mid-gap %.5f m/s against %.5f (%+.2f %%)\n", T, -Te,
               100 * (-T / Te - 1), T < 0 ? "yes" : "no", ut, ute, 100 * (ut / ute - 1));
        verdict(!ht_unstable(h) && T < 0 && fabs(-T / Te - 1) < 0.03 && fabs(ut / ute - 1) < 0.02, "H5");
        ht_free(h);
    }
    printf(failures ? "hftest: %d FAILED\n" : "hftest: all passed\n", failures);
    return failures ? 1 : 0;
}
