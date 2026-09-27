/* battest - verification of the lithium-ion cell model (src/lab/battery/dfn.h), the LG M50 of Chen et al. (2020).
 *
 * Criteria written on 2026-09-26, before the first run:
 *   B1 a particle losing lithium at a constant flux from a uniform start, against Crank's closed form for the sphere
 *      (The Mathematics of Diffusion, 2nd ed., eq. 6.60, the flux reversed): the surface concentration at D t / R^2 = 0.2
 *      within 0.5 % of its drop on 20 shells, the error falling at least threefold from 10 to 20 shells.
 *   B2 lithium in the particles and the electrolyte together unchanged, to 1e-10 of it, over a 1C discharge.
 *   B3 the lithium leaving the negative electrode equals the charge passed over Faraday's constant, to 1e-9.
 *   B4 the heat: the sum of its local parts (reaction overpotentials, Joule heating in solid and electrolyte) equals
 *      I (U_eff - V) at every step, to 1e-8 of it (the identity follows from charge conservation; the discretization
 *      keeps it exactly).
 *   B5 the capacity at C/10 (0.5 A) down to 2.5 V within 5 % of the parameter set's nominal 5.0 Ah.
 *   B6 the voltage at 1C after 1800 s changes by less than 2 mV from 10 to 20 volumes per region and by less than
 *      0.5 mV from 20 to 40.
 *   The module (src/lab/battery/pack.h), criteria written on 2026-09-26 before its first run:
 *   P1 conduction: a homogeneous block, a uniform heat source, cooled through its underside only (the coolant's
 *      temperature held): at steady state the cubes' temperatures equal the closed form T_c + q H / h + q (H z - z^2
 *      / 2) / k plus the half-cube offset the discretization makes, q dx^2 / (8 k), to 1e-9 of the rise; the error
 *      without the offset below 0.5 % of the rise on 20 cubes.
 *   P2 energy: over a module's discharge, the heat the cells and interconnects made equals what the module stored plus
 *      what the coolant and the air took, to 1e-9 of it.
 *   P3 the split: at every step the cells' currents add up to the module's to 1e-9 A, and their terminal voltages
 *      (each cell's less its interconnect's drop) agree to 1e-6 V.
 *   P4 symmetry: two identical cells in an insulated module share the current equally, to 1e-9 of it.
 *   The module's first run: P1 (3.6e-13), P2 (1.5e-13), P3 pass; P4 FAIL (7.5e-4): the grid ran from the module's
 *   corner and its last cube overhung one side by half a millimetre, so the two cells were cut into different cubes.
 *   The grid is now centred on the module across; the criterion is unchanged.
 *   First run: B1 pass (error ratio 4.01); the cell's Newton iteration did not converge, from two defects in the code
 *   (the solid's current balance had the wrong sign on its interior faces; the LU solve applied the row interchanges
 *   interleaved with the substitution, while the factors had moved whole rows). Fixed, before any criterion could be
 *   measured; then all pass: B2 2.6e-14, B3 4.5e-14, B4 1.3e-14, B5 5.075 Ah (+1.5 %), B6 0.21 and 0.05 mV. A 1C
 *   discharge gives 4.93 Ah to 2.5 V. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/lab/battery/dfn.h"
#include "../src/lab/battery/pack.h"

static int failures;
static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    fflush(stdout);
    failures += !ok;
}

/* Crank: the surface of a sphere losing flux F0 (mol/m^2/s) from c0 */
static double crank_surface(double R, double D, double c0, double F0, double t) {
    static const double alpha[] = {4.493409457909064, 7.725251836937707, 10.90412165942890, 14.06619391283147, 17.22075527193077, 20.37130295928756,
                                   23.51945249868900, 26.66605425881267, 29.81159879089296, 32.95638903982248};
    double tau = D * t / (R * R), sum = 0;
    for (int n = 0; n < 10; n++) sum += exp(-alpha[n] * alpha[n] * tau) / (alpha[n] * alpha[n]);
    for (int n = 10; n < 2000; n++) { /* the higher roots: alpha_n close to (n + 3/2) pi - 1 / ((n + 3/2) pi) */
        double a = (n + 1.5) * M_PI, al = a - 1 / a;
        sum += exp(-al * al * tau) / (al * al);
    }
    return c0 - F0 * R / D * (3 * tau + 0.5 - 0.3 - 2 * sum);
}

/* a discharge at constant current I (A) with dt, until the voltage falls to vmin or t_end; returns the capacity in Ah */
static double discharge(Dfn *D, double I, double dt, double vmin, double t_end, double *worst_heat, double *v_at, double t_at) {
    double ah = 0;
    while (dfn_time(D) < t_end - 1e-9) {
        if (!dfn_step(D, I, 298.15, dt)) {
            printf("  the step failed at t = %.1f s\n", dfn_time(D));
            return -1;
        }
        double q[4];
        dfn_heat_parts(D, q);
        double lhs = q[0] + q[1] + q[2], rhs = I * (dfn_ocv(D) - dfn_voltage(D));
        if (worst_heat && fabs(rhs) > 0) *worst_heat = fmax(*worst_heat, fabs(lhs - rhs) / fabs(rhs));
        if (v_at && fabs(dfn_time(D) - t_at) < 0.5 * dt) *v_at = dfn_voltage(D);
        if (dfn_voltage(D) < vmin) break;
        ah = I * dfn_time(D) / 3600;
    }
    return ah;
}

int main(void) {
    time_t w0 = time(NULL);
    printf("== B1: a particle losing lithium at a constant flux, against Crank\n");
    {
        const double R = 5.86e-6, Ds = 3.3e-14, c0 = 29866, F0 = 2e-5, t = 0.2 * R * R / Ds, dt = t / 4000;
        double ex = crank_surface(R, Ds, c0, F0, t), e10 = 0, e20 = 0;
        for (int nr = 10; nr <= 40; nr *= 2) {
            double c = dfn_particle_surface(R, Ds, c0, F0, t, dt, nr), err = fabs(c - ex) / (c0 - ex);
            printf("  %2d shells: surface %.3f mol/m^3 against %.3f (error %.3e of the drop)\n", nr, c, ex, err);
            if (nr == 10) e10 = err;
            if (nr == 20) e20 = err;
        }
        printf("  error ratio 10 -> 20 shells: %.2f\n", e10 / e20);
        verdict(e20 < 5e-3 && e10 / e20 >= 3, "B1");
    }
    char err[200];
    DfnSpec s;
    printf("== B2, B3, B4: a 1C discharge (5 A) of the LG M50\n");
    {
        dfn_spec_lgm50(&s, 20, 20);
        Dfn *D = dfn_create(&s, err, sizeof err);
        if (!D) {
            printf("  %s\n", err);
            return 1;
        }
        double ls0, le0, ln0, q0, ls, le, ln, q, worst_heat = 0;
        dfn_inventory(D, &ls0, &le0, &ln0, &q0);
        printf("  at rest: %.4f V\n", dfn_voltage(D));
        double ah = discharge(D, 5.0, 10, 2.5, 4000, &worst_heat, NULL, 0);
        dfn_inventory(D, &ls, &le, &ln, &q);
        double tot0 = ls0 + le0, tot = ls + le, dneg = ln0 - ln;
        printf("  %.3f Ah to 2.5 V in %.0f s, %.4f V at the end; lithium %.12e -> %.12e mol (%.2e); left the negative %.9e mol, charge / F %.9e mol (%.2e)\n", ah,
               dfn_time(D), dfn_voltage(D), tot0, tot, fabs(tot / tot0 - 1), dneg, q / 96485.33212, fabs(dneg / (q / 96485.33212) - 1));
        printf("  heat identity, worst over the steps: %.2e\n", worst_heat);
        verdict(fabs(tot / tot0 - 1) < 1e-10, "B2");
        verdict(fabs(dneg / (q / 96485.33212) - 1) < 1e-9, "B3");
        verdict(worst_heat < 1e-8, "B4");
        dfn_free(D);
    }
    printf("== B5: the capacity at C/10\n");
    {
        dfn_spec_lgm50(&s, 10, 12);
        Dfn *D = dfn_create(&s, err, sizeof err);
        double ah = discharge(D, 0.5, 60, 2.5, 40000, NULL, NULL, 0);
        printf("  %.3f Ah (nominal 5.0 Ah, %+.2f %%)\n", ah, 100 * (ah / 5 - 1));
        verdict(fabs(ah / 5 - 1) < 0.05, "B5");
        dfn_free(D);
    }
    printf("== B6: the voltage at 1C after 1800 s against the volumes across the cell\n");
    {
        double v[3] = {0};
        for (int q = 0; q < 3; q++) {
            int n = 10 << q;
            dfn_spec_lgm50(&s, n, 20);
            Dfn *D = dfn_create(&s, err, sizeof err);
            discharge(D, 5.0, 10, 0, 1800, NULL, &v[q], 1800);
            printf("  %2d volumes per electrode: %.5f V\n", n, v[q]);
            dfn_free(D);
        }
        printf("  10 -> 20: %.3f mV, 20 -> 40: %.3f mV\n", 1e3 * fabs(v[1] - v[0]), 1e3 * fabs(v[2] - v[1]));
        verdict(fabs(v[1] - v[0]) < 2e-3 && fabs(v[2] - v[1]) < 0.5e-3, "B6");
    }
    printf("== P1: steady conduction in a block cooled from below\n");
    {
        PackSpec ps = {0};
        dfn_spec_lgm50(&ps.cell, 4, 6);
        ps.rows = 1, ps.cols = 1, ps.pitch = 0.02, ps.radius = 0.008, ps.height = 0.038, ps.plate = 0.002, ps.margin = 0, ps.dx = 0.002;
        ps.kr = ps.kz = ps.k_plate = ps.k_gap = 5.0, ps.rc_cell = ps.rc_plate = ps.rc_gap = 2e6;
        ps.h_cool = 500, ps.T_cool_in = 300, ps.mcp_cool = 1e12, ps.h_amb = 0, ps.T_amb = 300, ps.T0 = 300;
        Pack *P = pack_create(&ps, err, sizeof err);
        if (!P) {
            printf("  %s\n", err);
            return 1;
        }
        int n[3];
        double o[3], dx;
        pack_grid(P, n, o, &dx);
        size_t nc = (size_t)n[0] * n[1] * n[2];
        double *src = malloc(nc * 8), qv = 2e5, H = n[2] * dx, k = 5.0, h = 500;
        for (size_t c = 0; c < nc; c++) src[c] = qv * dx * dx * dx;
        for (int st = 0; st < 3; st++) pack_thermal_step(P, src, 1e9); /* steady */
        const double *T = pack_T(P);
        double rise = qv * H / h + qv * H * H / (2 * k), e_off = 0, e_raw = 0;
        for (int kk = 0; kk < n[2]; kk++) {
            double z = (kk + 0.5) * dx, ex = 300 + qv * H / h + qv * (H * z - z * z / 2) / k, Tv = T[(size_t)n[0] * n[1] * kk + 1];
            e_raw = fmax(e_raw, fabs(Tv - ex) / rise), e_off = fmax(e_off, fabs(Tv - ex - qv * dx * dx / (8 * k)) / rise);
        }
        printf("  %d cubes over %.3f m: rise %.4f K; error %.2e of it with the half-cube offset, %.2e without\n", n[2], H, rise, e_off, e_raw);
        verdict(e_off < 1e-9 && e_raw < 5e-3, "P1");
        free(src);
        pack_free(P);
    }
    printf("== P2, P3: a module of 2 x 3 LG M50 cells on a cold plate, 2C\n");
    {
        PackSpec ps = {0};
        dfn_spec_lgm50(&ps.cell, 8, 8);
        ps.rows = 2, ps.cols = 3, ps.pitch = 0.023, ps.radius = 0.0105, ps.height = 0.070, ps.plate = 0.003, ps.margin = 0.003, ps.dx = 0.0015;
        ps.kr = 1.16, ps.kz = 24.7, ps.rc_cell = 1.69e6, ps.k_plate = 237, ps.rc_plate = 2.42e6, ps.k_gap = 0.026, ps.rc_gap = 1.2e3;
        ps.h_cool = 1000, ps.T_cool_in = 298.15, ps.mcp_cool = 20, ps.h_amb = 5, ps.T_amb = 298.15, ps.T0 = 298.15, ps.R_ext = 1e-3;
        Pack *P = pack_create(&ps, err, sizeof err);
        if (!P) {
            printf("  %s\n", err);
            return 1;
        }
        pack_set_resistance(P, 4, 3e-3);
        double worst_dV = 0, worst_dI = 0;
        time_t t0 = time(NULL);
        while (pack_time(P) < 1500) {
            if (!pack_step(P, 60.0, 10)) {
                printf("  the module failed at t = %.0f s\n", pack_time(P));
                break;
            }
            double dV, dI;
            pack_split_error(P, &dV, &dI);
            worst_dV = fmax(worst_dV, dV), worst_dI = fmax(worst_dI, fabs(dI));
            if (pack_voltage(P) < 2.5) break;
        }
        double made, stored, tc, ta;
        pack_energy(P, &made, &stored, &tc, &ta);
        printf("  %.0f s (%.0f s on this machine), terminal %.4f V; heat made %.3f J, stored %.3f, coolant %.3f, air %.3f (%.2e)\n", pack_time(P),
               difftime(time(NULL), t0), pack_voltage(P), made, stored, tc, ta, fabs(stored + tc + ta - made) / made);
        for (int c = 0; c < pack_cells(P); c++) {
            double I, Tm, Tx, q, V;
            pack_cell_state(P, c, &I, &Tm, &Tx, &q, &V);
            printf("  cell %d: %.3f A, mean %.2f K, hottest %.2f K, %.3f W\n", c, I, Tm, Tx, q);
        }
        printf("  worst over the steps: voltages apart %.2e V, currents off %.2e A\n", worst_dV, worst_dI);
        verdict(fabs(stored + tc + ta - made) < 1e-9 * made, "P2");
        verdict(worst_dI < 1e-9 && worst_dV < 1e-6, "P3");
        pack_free(P);
    }
    printf("== P4: two identical cells, insulated\n");
    {
        PackSpec ps = {0};
        dfn_spec_lgm50(&ps.cell, 6, 6);
        ps.rows = 1, ps.cols = 2, ps.pitch = 0.023, ps.radius = 0.0105, ps.height = 0.070, ps.plate = 0.003, ps.margin = 0.003, ps.dx = 0.0025;
        ps.kr = 1.16, ps.kz = 24.7, ps.rc_cell = 1.69e6, ps.k_plate = 237, ps.rc_plate = 2.42e6, ps.k_gap = 0.026, ps.rc_gap = 1.2e3;
        ps.h_cool = 0, ps.T_cool_in = 298.15, ps.mcp_cool = 20, ps.h_amb = 0, ps.T_amb = 298.15, ps.T0 = 298.15, ps.R_ext = 1e-3;
        Pack *P = pack_create(&ps, err, sizeof err);
        double worst = 0;
        for (int st = 0; P && st < 30; st++) {
            if (!pack_step(P, 10.0, 10)) break;
            double I0, I1;
            pack_cell_state(P, 0, &I0, NULL, NULL, NULL, NULL), pack_cell_state(P, 1, &I1, NULL, NULL, NULL, NULL);
            worst = fmax(worst, fabs(I0 - I1) / 5.0);
        }
        printf("  largest difference of the two currents, over 30 steps: %.2e of each\n", worst);
        verdict(P && worst < 1e-9, "P4");
        pack_free(P);
    }
    printf("== %.0f s on this machine\n", difftime(time(NULL), w0));
    printf(failures ? "battest: %d FAILED\n" : "battest: all passed\n", failures);
    return failures ? 1 : 0;
}
