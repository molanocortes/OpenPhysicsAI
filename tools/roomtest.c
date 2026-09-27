/* roomtest - verification of the fire solver's rooms (src/lab/fire/fire.h): walls, obstacles, vents, openings, heat
 * sources, heated walls and sealed boxes.
 *
 * Criteria written on 2026-09-26, before the first run of this test:
 *   A duct: a square duct 13 cells across (13 mm), made of two no-slip sides of the box and two obstacles 3 cells
 *     thick, fed by a vent over its section at Re 20 (hydraulic diameter), open at the far end, no gravity, resolved
 *     flow (no subgrid model). Fully developed (the section 40 cells from the inlet, 3 diameters, the entrance length
 *     is 1.7):
 *     A1 the centreline speed over the mean within 1.5 % of 2.0962 (Shah and London 1978, the square duct);
 *     A2 the pressure drop, f Re = 2 (-dp/dx) Dh^2 / (rho U nu) between 30 and 50 cells, within 3 % of 56.91 (the same);
 *     A3 mass out of the open end equal to what the vent brings in, to 1e-6, at 20 s.
 *   B the differentially heated cube (Tric, Henry and Le Quere 2000): one vertical wall hot, the opposite one cold, the
 *     other four adiabatic, all no slip, a sealed box of air (Pr 0.71), resolved flow. The mean Nusselt number of the hot
 *     wall, Q / (k dT L):
 *     B1 Ra 1e4 on 32 cells a side: within 2 % of 2.0542, the cold wall's within 1 % of the hot wall's;
 *     B2 Ra 1e5 on 64 cells (ROOM_FULL=1; about 20 minutes, run detached): within 2 % of 4.3370, the same balance;
 *     B3 the sealed box's mass unchanged to 1e-10.
 *   C a ventilated room with an obstacle and a 400 W heat source, a supply vent and an opening in the far wall
 *     (adiabatic walls, Vreman's subgrid model): at the statistically steady state (the average over 100 to 150 s,
 *     six air changes in; amended below), the outflow's mass-weighted temperature rise over the supply within 1 % of Q / (m cp), m
 *     the averaged inflow; inflow and outflow within 0.5 % of each other over that window.
 *   D a sealed box of air heated by 1 kW for 10 s: the background pressure rises by (gamma - 1) Q t / V (the first law
 *     for a rigid adiabatic box), within 1 %; the gas's mass unchanged to 1e-10.
 *   First run: A1 FAIL (-1.73 %), A2 (-2.10 %) and A3 pass, D pass (-0.41 %). The failure is the resolution's: the
 *   same discretization solved directly in 2D (Poisson's equation on 13 cells with the walls half a cell out) gives
 *   2.0609, -1.69 %, and 2.0865 and 2.0939 on 25 and 51 cells (second order); the 3D solver's 2.0599 is that discrete
 *   answer to 0.05 %. AMENDED (2026-09-26, after this run, openly): the duct is 25 cells across (0.5 mm cells, 12.5
 *   mm), the section 64 cells from the inlet (2.6 diameters), the pressure between 44 and 76 cells, run for 10 s (the
 *   slowest transient decays as exp(-2 pi^2 nu t / a^2), 0.5 s); the criteria are unchanged.
 *   C's first run: FAIL, -2.71 %, the room still warming: over 100 to 150 s its gas lost mass (outflow 0.069 % above
 *   inflow), and that difference carries the absolute temperature, 293 K x 0.069 % = 0.2 K against the 8.2 K rise.
 *   AMENDED (2026-09-26, after this run, openly): the window is 250 to 300 s (ten air-change times in); the criteria are
 *   unchanged.
 *   Added 2026-09-26 with fans forced before the projection rather than held (the pressure solver stalled on a rack
 *   of held faces), before E's first run:
 *   E a fan box across a duct open at both ends (a square duct of 16 cells, walls no slip, a fan 4 cells long at 0.5
 *     m/s, no gravity, 10 s): the flow through the duct's mid-section within 2 % of the fan's speed times the section,
 *     since nothing resists it but the walls' friction, which the forcing overcomes.
 *   E's first run: pass (-0.42 %). The whole test rerun after the pressure solver became conjugate gradients with
 *   each boundary face's own ghost in the prolongation, and fans forced rather than held: A1 -0.60 %, A2 -0.17 %,
 *   A3, B1 +1.24 %, B3, C -0.84 % (the room is turbulent: a different trajectory, a different 50 s average), D
 *   -0.41 %, E -0.42 %, all pass.
 *   Added 2026-09-27 with the age of the air, before F's first run:
 *   F in C's room over C's window: the outflow's mass-weighted age of air within 2 % of the room's air mass over the
 *     mass flow, the nominal time constant (at steady state the exhaust's mean age equals it however the room mixes:
 *     Sandberg 1981).
 *   F's first run: pass, 23.963 s against 24.034 s (-0.29 %); C in the same run -0.84 %.
 *   Every case: the projection's error, the largest |div u - D| over the largest |D| (or over the largest |div u| where
 *   D is zero), below 1e-6 at every step. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/lab/fire/fire.h"

static int failures;
static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    fflush(stdout);
    failures += !ok;
}

static double worst_div;
static bool run(Fire *F, ThreadPool *pool, double t_end) {
    while (fire_time(F) < t_end) {
        if (fire_step(F, pool) <= 0) {
            printf("  the solver failed at t = %.3f s\n", fire_time(F));
            return false;
        }
        double de;
        fire_diagnostics(F, &de, NULL, NULL, NULL, NULL);
        worst_div = fmax(worst_div, de);
    }
    return true;
}

static void room_spec(FireSpec *s) {
    fire_spec_defaults(s);
    for (int f = 0; f < 6; f++) s->side[f] = FIRE_WALL;
    s->sponge = 0, s->hrr = 0;
}

/* A: the square duct */
static void duct(ThreadPool *pool) {
    printf("== A: a square duct, Re 20\n");
    const double dx = 0.5e-3, a = 25 * dx, nu = 1.5e-5, U = 20 * nu / a, X = 96 * dx, W = 32 * dx;
    const int na = 25, ix = 64, i1 = 44, i2 = 76;
    FireSpec s;
    room_spec(&s);
    s.n[0] = 96, s.n[1] = 32, s.n[2] = 32, s.dx = dx, s.g = 0, s.nu = nu, s.sgs = FIRE_NO_SGS, s.T0 = 293.15;
    s.side[1] = FIRE_OPEN;
    char err[256];
    Fire *F = fire_create(&s, err, sizeof err);
    const double wy[6] = {0, a, 0, X, W, W}, wz[6] = {0, 0, a, X, W, W}, in[4] = {0, 0, a, a};
    if (!F || !fire_add_obstacle(F, wy, 0) || !fire_add_obstacle(F, wz, 0) || !fire_add_vent(F, 0, in, U, s.T0, 0)) {
        printf("  %s\n", F ? "the room was refused" : err);
        failures++;
        return;
    }
    worst_div = 0;
    time_t w0 = time(NULL);
    bool ok = run(F, pool, 10);
    double umax = 0, usum = 0, p1 = 0, p2 = 0, rho = 0;
    for (int k = 0; k < na; k++)
        for (int j = 0; j < na; j++) {
            double uf = fire_face(F, 0, ix, j, k);
            umax = fmax(umax, uf), usum += uf;
            p1 += fire_cell(F, 1, i1, j, k), p2 += fire_cell(F, 1, i2, j, k), rho += fire_cell(F, 0, ix, j, k);
        }
    const double n2 = na * na;
    double um = usum / n2, dpdx = (p2 - p1) / n2 / ((i2 - i1) * dx), fre = 2 * (-dpdx) * a * a / (rho / n2 * um * nu);
    double m0, mi, mo;
    fire_diagnostics(F, NULL, NULL, &m0, &mi, &mo);
    double bf[4];
    fire_boundary_flows(F, bf);
    printf("  %.0f s on this machine; mean speed %.5f m/s (the vent's %.5f), centreline over mean %.4f against 2.0962 (%+.2f %%)\n", difftime(time(NULL), w0),
           um, U, umax / um, 100 * (umax / um / 2.0962 - 1));
    printf("  f Re %.3f against 56.91 (%+.2f %%)\n", fre, 100 * (fre / 56.91 - 1));
    /* A3: the flows now, from one more step's inflow and outflow */
    double b0[4], b1[4];
    fire_boundary_flows(F, b0);
    double t0 = fire_time(F);
    ok = ok && run(F, pool, t0 + 0.2);
    fire_boundary_flows(F, b1);
    double mdi = b1[0] - b0[0], mdo = b1[2] - b0[2];
    printf("  over the last 0.2 s: in %.6e kg, out %.6e kg (%.2e); projection error %.2e\n", mdi, mdo, fabs(mdo / mdi - 1), worst_div);
    verdict(ok && fabs(umax / um / 2.0962 - 1) < 0.015, "A1");
    verdict(ok && fabs(fre / 56.91 - 1) < 0.03, "A2");
    verdict(ok && fabs(mdo / mdi - 1) < 1e-6 && worst_div < 1e-6, "A3");
    fire_free(F);
}

/* B: the differentially heated cube */
static double cavity(ThreadPool *pool, double Ra, int n, double nu_ref, const char *name, double tol) {
    printf("== %s: the differentially heated cube, Ra %.0e, %d cells a side\n", name, Ra, n);
    const double L = 0.1, T0 = 300, nu = 1.5e-5, Pr = 0.71, g = 9.80665;
    const double dT = Ra * nu * nu * T0 / (g * L * L * L * Pr); /* Ra = g (1 / T0) dT L^3 Pr / nu^2 */
    FireSpec s;
    room_spec(&s);
    s.n[0] = s.n[1] = s.n[2] = n, s.dx = L / n, s.g = g, s.nu = nu, s.Pr = Pr, s.sgs = FIRE_NO_SGS, s.T0 = T0;
    s.side_T[0] = T0 + dT / 2, s.side_T[1] = T0 - dT / 2;
    char err[256];
    Fire *F = fire_create(&s, err, sizeof err);
    if (!F) {
        printf("  %s\n", err);
        failures++;
        return 0;
    }
    worst_div = 0;
    double m0;
    fire_diagnostics(F, NULL, NULL, &m0, NULL, NULL);
    /* the conductivity at the mean state: rho cp nu / Pr, cp of air at 300 K from the solver's own table */
    const double rho = s.p0 * 28.96e-3 / (8.314462618 * T0), k = rho * 1012.0 * nu / Pr;
    time_t w0 = time(NULL);
    double nh = 0, nc = 0, last = -1;
    bool ok = true;
    const double t_max = getenv("ROOM_TMAX") ? atof(getenv("ROOM_TMAX")) : 600;
    for (double t = 10; t <= t_max && ok; t += 10) {
        ok = run(F, pool, t);
        double q[8];
        fire_heat_flows(F, q);
        nh = q[0] / (k * dT * L), nc = -q[1] / (k * dT * L);
        printf("  t %4.0f s  Nu hot %.4f cold %.4f  (%.0f s)\n", t, nh, nc, difftime(time(NULL), w0));
        fflush(stdout);
        if (last > 0 && fabs(nh / last - 1) < 2e-4 && fabs(nc / nh - 1) < 2e-3) break; /* steady */
        last = nh;
    }
    double m1;
    fire_diagnostics(F, NULL, NULL, &m1, NULL, NULL);
    printf("  Nu %.4f against %.4f (%+.2f %%); cold wall %+.2f %% of the hot; mass change %.2e; projection error %.2e; p0 %.2f Pa\n", nh, nu_ref,
           100 * (nh / nu_ref - 1), 100 * (nc / nh - 1), fabs(m1 / m0 - 1), worst_div, fire_pressure(F));
    verdict(ok && fabs(nh / nu_ref - 1) < tol && fabs(nc / nh - 1) < 0.01 && worst_div < 1e-6, name);
    fire_free(F);
    return fabs(m1 / m0 - 1);
}

/* C: a ventilated room */
static void room(ThreadPool *pool) {
    printf("== C: a ventilated room with a 400 W heat source\n");
    FireSpec s;
    room_spec(&s);
    s.n[0] = 32, s.n[1] = 16, s.n[2] = 16, s.dx = 0.05, s.sgs = FIRE_VREMAN, s.T0 = 293.15;
    char err[256];
    Fire *F = fire_create(&s, err, sizeof err);
    const double supply[4] = {0.3, 0.3, 0.5, 0.5}, door[4] = {0.3, 0.5, 0.5, 0.7}, block[6] = {1.0, 0.2, 0, 1.2, 0.6, 0.4}, heater[6] = {0.6, 0.3, 0, 0.8, 0.5, 0.2};
    const double Q = 400;
    if (!F || !fire_add_vent(F, 0, supply, 1.0, s.T0, 0) || !fire_add_opening(F, 1, door) || !fire_add_obstacle(F, block, 0) || !fire_add_heat(F, heater, Q)) {
        printf("  %s\n", F ? "the room was refused" : err);
        failures++;
        return;
    }
    worst_div = 0;
    time_t w0 = time(NULL);
    const double ta = getenv("ROOM_C_FROM") ? atof(getenv("ROOM_C_FROM")) : 250, tb = ta + 50; /* the window */
    bool ok = run(F, pool, ta);
    double b0[4], b1[4];
    fire_boundary_flows(F, b0);
    double t0 = fire_time(F), a0 = fire_age_out(F), mroom = 0;
    ok = ok && run(F, pool, tb);
    fire_boundary_flows(F, b1);
    fire_diagnostics(F, NULL, NULL, &mroom, NULL, NULL);
    double age_out = (fire_age_out(F) - a0) / (b1[2] - b0[2]);
    double tw = fire_time(F) - t0, mi = (b1[0] - b0[0]) / tw, mo = (b1[2] - b0[2]) / tw;
    double Tin = (b1[1] - b0[1]) / (b1[0] - b0[0]), Tout = (b1[3] - b0[3]) / (b1[2] - b0[2]);
    double dTe = Q / (mi * 1012.0); /* cp of air below 300 K in the solver's table */
    printf("  %.0f s on this machine; inflow %.5f kg/s, outflow %.5f kg/s (%+.3f %%); outflow %.3f K above the supply against Q / (m cp) = %.3f K (%+.2f %%); "
           "projection error %.2e\n",
           difftime(time(NULL), w0), mi, mo, 100 * (mo / mi - 1), Tout - Tin, dTe, 100 * ((Tout - Tin) / dTe - 1), worst_div);
    verdict(ok && fabs((Tout - Tin) / dTe - 1) < 0.01 && fabs(mo / mi - 1) < 0.005 && worst_div < 1e-6, "C");
    printf("  the exhaust's mean age of air %.3f s against the room's mass over the flow %.3f s (%+.2f %%)\n", age_out, mroom / mi, 100 * (age_out / (mroom / mi) - 1));
    verdict(ok && fabs(age_out / (mroom / mi) - 1) < 0.02, "F");
    fire_free(F);
}

/* E: a fan in an open duct */
static void fan(ThreadPool *pool) {
    printf("== E: a fan in a duct open at both ends\n");
    FireSpec s;
    room_spec(&s);
    const double dx = 0.01, U = 0.5;
    s.n[0] = 64, s.n[1] = 16, s.n[2] = 16, s.dx = dx, s.g = 0, s.sgs = FIRE_VREMAN, s.T0 = 293.15;
    s.side[0] = s.side[1] = FIRE_OPEN, s.sponge = 0;
    char err[256];
    Fire *F = fire_create(&s, err, sizeof err);
    const double box[6] = {0.20, 0, 0, 0.24, 0.16, 0.16};
    if (!F || !fire_add_fan(F, box, 0, U)) {
        printf("  %s\n", F ? "the fan was refused" : err);
        failures++;
        return;
    }
    worst_div = 0;
    bool ok = run(F, pool, 10);
    double q = 0;
    for (int k = 0; k < 16; k++)
        for (int j = 0; j < 16; j++) q += fire_face(F, 0, 44, j, k) * dx * dx;
    double qe = U * 0.16 * 0.16;
    printf("  flow %.6f m^3/s against %.6f (%+.3f %%); projection error %.2e\n", q, qe, 100 * (q / qe - 1), worst_div);
    verdict(ok && fabs(q / qe - 1) < 0.02 && worst_div < 1e-6, "E");
    fire_free(F);
}

/* D: a sealed box heated */
static void sealed(ThreadPool *pool) {
    printf("== D: a sealed box of air heated by 1 kW\n");
    FireSpec s;
    room_spec(&s);
    s.n[0] = s.n[1] = s.n[2] = 16, s.dx = 0.05, s.sgs = FIRE_VREMAN, s.T0 = 293.15;
    char err[256];
    Fire *F = fire_create(&s, err, sizeof err);
    const double heater[6] = {0.3, 0.3, 0, 0.5, 0.5, 0.2}, Q = 1000, V = 0.8 * 0.8 * 0.8;
    if (!F || !fire_add_heat(F, heater, Q)) {
        printf("  %s\n", F ? "the room was refused" : err);
        failures++;
        return;
    }
    worst_div = 0;
    double m0, m1, p0 = fire_pressure(F);
    fire_diagnostics(F, NULL, NULL, &m0, NULL, NULL);
    bool ok = run(F, pool, 10);
    fire_diagnostics(F, NULL, NULL, &m1, NULL, NULL);
    const double cp = 1012.0, R = 8.314462618 / 28.96e-3, gamma = cp / (cp - R);
    double dp = fire_pressure(F) - p0, dpe = (gamma - 1) * Q * fire_time(F) / V;
    printf("  pressure rise %.2f Pa against (gamma - 1) Q t / V = %.2f Pa (%+.3f %%); mass change %.2e; projection error %.2e\n", dp, dpe, 100 * (dp / dpe - 1),
           fabs(m1 / m0 - 1), worst_div);
    verdict(ok && fabs(dp / dpe - 1) < 0.01 && fabs(m1 / m0 - 1) < 1e-10 && worst_div < 1e-6, "D");
    fire_free(F);
}

int main(int argc, char **argv) {
    ThreadPool *pool = pool_create(cpu_perf_count());
    const char *only = argc > 1 ? argv[1] : NULL; /* a single case: A, B, C or D */
    if (!only || only[0] == 'A') duct(pool);
    if (!only || only[0] == 'B') {
        double dm = cavity(pool, 1e4, 32, 2.0542, "B1", 0.02);
        if (getenv("ROOM_FULL")) dm = fmax(dm, cavity(pool, 1e5, 64, 4.3370, "B2", 0.02));
        else printf("  B2 (Ra 1e5, 64 cells) runs with ROOM_FULL=1\n");
        verdict(dm < 1e-10, "B3");
    }
    if (!only || only[0] == 'C') room(pool);
    if (!only || only[0] == 'D') sealed(pool);
    if (!only || only[0] == 'E') fan(pool);
    pool_destroy(pool);
    printf(failures ? "roomtest: %d FAILED\n" : "roomtest: all passed\n", failures);
    return failures ? 1 : 0;
}
