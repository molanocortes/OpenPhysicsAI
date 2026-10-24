/* flowtest.c - verification of the lab's flow domain (src/lab/flow) against an independent solution: the benchmark of
 * laminar flow around a cylinder in a channel, Schaefer and Turek, "Benchmark computations of laminar flow around a
 * cylinder", Notes on Numerical Fluid Mechanics 52 (1996) 547-566. Its reference intervals are the spread of the
 * best solutions of the groups that took part (finite elements, finite volumes, lattice Boltzmann), at resolutions far
 * finer than a test can afford, so they stand in for the exact answer.
 *
 * The set-up, in the benchmark's units: channel 2.2 m long and 0.41 m high, no-slip floor and ceiling, a cylinder of
 * D = 0.1 m centred at (0.2, 0.2) m, 0.05 m below the channel's middle; parabolic inflow with mean speed U, Re = U D / nu,
 * coefficients on U and D. Here the lengths are in D: channel 22 D x 4.1 D, cylinder 2 D from the inlet, 0.05 D below
 * the middle.
 *
 * Criteria, written on 2026-09-25 before the first run and not to be changed without a dated note:
 *   F1  2D-1, steady, Re 20, D = 20 cells: drag coefficient within 3 % of 5.58 (the interval is 5.57 to 5.59) and the
 *       recirculation length behind the cylinder within 5 % of 0.847 D (interval 0.842 to 0.852 D).
 *   F2  2D-2, periodic, Re 100, D = 20 cells: Strouhal number f D / U within 3 % of 0.300 (interval 0.295 to 0.305),
 *       largest drag coefficient within 3 % of 3.23 (3.22 to 3.24), largest lift coefficient within 10 % of 1.00
 *       (0.99 to 1.01).
 * The lift at Re 20 (0.0104 to 0.0110) and the pressure difference are recorded, not judged: the lift is a difference
 * of two large wall sums and at 20 cells across it is below the resolution of this test. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/lab/flow/flow.h"

static int failures;

static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    if (!ok) failures++;
}

static double wall(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

static int g_collision;

static Flow *channel(double Re, int D, double u_lb) {
    FlowSpec s;
    flow_spec_defaults(&s);
    s.Re = Re, s.D = D, s.u_lb = u_lb;
    s.channel = true;
    s.collision = g_collision;
    s.width_D = 4.1, s.length_D = 22, s.upstream_D = 2, s.body_y_D = -0.05;
    s.D_m = 0.1, s.nu_m2_s = 0.1 * 1.0 / Re; /* the benchmark's units: U = 1 m/s */
    char err[256];
    Flow *f = flow_create(&s, err, sizeof err);
    if (!f) printf("  %s\n", err);
    return f;
}

/* exploration outside the verdicts: flowtest F1|F2 D CONVECTIVE_TIMES [LATTICE_U [COLLISION]] */
static int explore(const char *which, int D, double conv, double u) {
    long per = (long)(D / u);
    bool f1 = which[1] == '1';
    Flow *f = channel(f1 ? 20 : 100, D, u);
    if (!f) return 1;
    double c0 = wall();
    if (f1) {
        double cd, cl, cdm, clr, prev = 0;
        for (int k = 1; k <= (int)conv; k++) {
            flow_advance(f, per);
            flow_coefficients(f, flow_steps(f) - per, &cd, &cl, &cdm, &clr);
            if (k % 10 == 0) printf("  D %d  t %d D/U  drag %.4f (%+.1e)  lift %.5f  La %.4f  %.0f s\n", D, k, cdm, cdm - prev, cl, flow_wake_length(f), wall() - c0), fflush(stdout);
            prev = cdm;
        }
    } else {
        flow_advance(f, (long)((conv - 20) * per));
        long from = flow_steps(f);
        flow_advance(f, 20 * per);
        int periods;
        double St = flow_strouhal(f, from, &periods), a, b;
        flow_extrema(f, from, &a, &b);
        printf("  D %d  Strouhal %.4f (%d periods)  largest drag %.4f  largest lift %.4f  %.0f s\n", D, St, periods, a, b, wall() - c0);
    }
    flow_free(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4) {
        if (argc >= 6) g_collision = atoi(argv[5]);
        return explore(argv[1], atoi(argv[2]), atof(argv[3]), argc >= 5 ? atof(argv[4]) : 0.05);
    }
    printf("== F1: Schaefer-Turek 2D-1, cylinder in a channel, Re 20, steady\n");
    {
        double c0 = wall();
        const int D = 20;
        const double u = 0.05;
        Flow *f = channel(20, D, u);
        bool ok = f != NULL;
        double cd = 0, cl = 0, La = 0, cdm, clr;
        if (ok) {
            long per = (long)(D / u); /* steps per convective time D / U */
            ok = flow_advance(f, 40 * per);
            flow_coefficients(f, flow_steps(f) - per, &cd, &cl, &cdm, &clr);
            La = flow_wake_length(f);
            double cd_prev = cdm;
            ok = ok && flow_advance(f, per);
            flow_coefficients(f, flow_steps(f) - per, &cd, &cl, &cdm, &clr);
            printf("  drag %.4f (steady to %.1e over the last convective time), lift %.5f (recorded)\n", cdm, fabs(cdm - cd_prev) / cdm, cl);
            cd = cdm;
            La = flow_wake_length(f);
            printf("  recirculation length %.4f D; %ld steps, %.1f s\n", La, flow_steps(f), wall() - c0);
            printf("  recorded first runs (docs/lab/flow.md): drag 5.24 from a channel started at rest, 5.39 with the periodic seam's wall links missing\n");
        }
        verdict(ok && fabs(cd / 5.58 - 1) < 0.03 && fabs(La / 0.847 - 1) < 0.05, "F1");
        flow_free(f);
    }
    printf("== F2: Schaefer-Turek 2D-2, cylinder in a channel, Re 100, periodic shedding\n");
    {
        double c0 = wall();
        const int D = 20;
        const double u = 0.05;
        Flow *f = channel(100, D, u);
        bool ok = f != NULL;
        double St = 0, cdmax = 0, clmax = 0;
        int periods = 0;
        if (ok) {
            long per = (long)(D / u);
            ok = flow_advance(f, 60 * per);
            long from = flow_steps(f);
            ok = ok && flow_advance(f, 20 * per);
            St = flow_strouhal(f, from, &periods);
            flow_extrema(f, from, &cdmax, &clmax);
            printf("  Strouhal %.4f over %d periods, largest drag %.4f, largest lift %.4f; %ld steps, %.1f s\n", St, periods, cdmax, clmax,
                   flow_steps(f), wall() - c0);
        }
        verdict(ok && periods >= 4 && fabs(St / 0.300 - 1) < 0.03 && fabs(cdmax / 3.23 - 1) < 0.03 && fabs(clmax / 1.00 - 1) < 0.10, "F2");
        flow_free(f);
    }
    printf(failures ? "flowtest: %d FAILED\n" : "flowtest: all passed\n", failures);
    return failures ? 1 : 0;
}
