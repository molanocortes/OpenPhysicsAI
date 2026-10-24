/* breaktest - verification of the break estimate (src/mech/breakest.h, GOALS.md G20 step 5).
 *
 * Criteria written on 2026-10-08, before the first run:
 *   B1 the symmetric section (c = 1): g(1, m) within 1e-9 of the closed form 3/2 - 1/(2 m^2) at m = 1, 1.5, 2, 5, 50.
 *   B2 fully plastic: g(c, 1e8) within 1e-6 of 3c / (1 + c) at c = 0.7, 1, 1.3, 2 (and g(c, inf) is that formula).
 *   B3 the asymmetric section at finite m: g(c, m) within 1e-6 of an independent sum over a million fibres, its neutral
 *      axis found by bisection on the axial force, at (c, m) = (1.3, 1.5), (1.76, 3), (0.8, 2).
 *   B4 the load factor: pure bending equals g to 1e-9; pure tension 1/n; a squash load c/|n|; with c = 1 and m = 1e8,
 *      the fully plastic interaction b = 3/2 (1 - n^2) is reached at lambda = 1 within 1e-6 for n = 0.25, 0.5, 0.75;
 *      a load and its mirror (b of the other sign) give the same factor.
 *   B5 Creager and Paris: at rho = 0 the crack's field K / sqrt(2 pi d) to 1e-12; at the root (d = 0) the classic
 *      largest stress 2 K / sqrt(pi rho) to 1e-12.
 *   B6 the bend bar's stress intensity: f(1/2) of ASTM D5045 within 1e-3 of the standard's tabulated 10.65; and the
 *      critical distance of K_c = 3 MPa sqrt(m), s0 = 60 MPa is (1/pi)(1/20)^2 m to 1e-12.
 *
 *   First run (2026-10-08): B1, B2, B3, B5, B6 pass (B3 4.3e-12); B4 FAIL twice. The pure-bending check compared 1/lambda
 *   with g, a slip of this test (the criterion is lambda = g; it now compares that), and the pure-tension factor came out
 *   0: the bisection halved the curvature until m - k equalled m in floating point. It now stops at 1e-14 of its
 *   bracket and answers pure tension directly. The criteria are unchanged.
 *
 *   make build/breaktest && ./build/breaktest */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/mech/breakest.h"

static int fails, passes;
static void check(int ok, const char *what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    ok ? passes++ : fails++;
}

/* an independent section: N fibres of equal depth, the neutral axis by bisection on the axial force, breaking when the
 * top fibre's strain is m */
static double fibres(double c, double m, int n) {
    double lo = -0.5, hi = 0.5; /* the strain's zero */
    double M = 0;
    for (int it = 0; it < 100; it++) {
        double y0 = 0.5 * (lo + hi), k = m / (0.5 - y0), F = 0;
        M = 0;
        for (int i = 0; i < n; i++) {
            double y = -0.5 + (i + 0.5) / n, e = k * (y - y0), s = e > 1 ? 1 : e < -c ? -c : e;
            F += s / n, M += s * y / n;
        }
        if (F > 0) lo = y0; /* too much tension: the zero moves up */
        else hi = y0;
    }
    return 6 * M;
}

int main(void) {
    printf("== B1 the symmetric section against its closed form\n");
    {
        const double ms[5] = {1, 1.5, 2, 5, 50};
        double worst = 0;
        for (int i = 0; i < 5; i++) worst = fmax(worst, fabs(breakest_bending_ratio(1, ms[i]) - (1.5 - 0.5 / (ms[i] * ms[i]))));
        char w[160];
        snprintf(w, sizeof w, "g(1, m) against 3/2 - 1/(2 m^2): worst %.2e (criterion 1e-9)", worst);
        check(worst < 1e-9, w);
    }
    printf("== B2 fully plastic\n");
    {
        const double cs[4] = {0.7, 1, 1.3, 2};
        double worst = 0, inf_worst = 0;
        for (int i = 0; i < 4; i++) {
            worst = fmax(worst, fabs(breakest_bending_ratio(cs[i], 1e8) - 3 * cs[i] / (1 + cs[i])));
            inf_worst = fmax(inf_worst, fabs(breakest_bending_ratio(cs[i], INFINITY) - 3 * cs[i] / (1 + cs[i])));
        }
        char w[160];
        snprintf(w, sizeof w, "g(c, 1e8) against 3c/(1+c): worst %.2e (criterion 1e-6); g(c, inf) %.1e", worst, inf_worst);
        check(worst < 1e-6 && inf_worst < 1e-15, w);
    }
    printf("== B3 the asymmetric section at finite m against a million fibres\n");
    {
        const double cm[3][2] = {{1.3, 1.5}, {1.76, 3}, {0.8, 2}};
        double worst = 0;
        for (int i = 0; i < 3; i++) {
            double g = breakest_bending_ratio(cm[i][0], cm[i][1]), f = fibres(cm[i][0], cm[i][1], 1000000);
            printf("  c %.2f m %.1f: g %.9f, fibres %.9f\n", cm[i][0], cm[i][1], g, f);
            worst = fmax(worst, fabs(g - f));
        }
        char w[160];
        snprintf(w, sizeof w, "worst difference %.2e (criterion 1e-6)", worst);
        check(worst < 1e-6, w);
    }
    printf("== B4 the load factor\n");
    {
        double g = breakest_bending_ratio(1.3, 2.5), lb = breakest_load_factor(0, 1, 1.3, 2.5);
        char w[200];
        snprintf(w, sizeof w, "pure bending: lambda %.12f against g %.12f", lb, g);
        check(fabs(lb - g) < 1e-9, w);
        double lt = breakest_load_factor(0.5, 0, 1.3, 2.5), ls = breakest_load_factor(-0.5, 0, 1.3, 2.5);
        snprintf(w, sizeof w, "pure tension n = 1/2: %.12f (2); squashed n = -1/2: %.12f (2.6)", lt, ls);
        check(fabs(lt - 2) < 1e-9 && fabs(ls - 2.6) < 1e-12, w);
        double worst = 0;
        for (int i = 1; i <= 3; i++) {
            double n = 0.25 * i, b = 1.5 * (1 - n * n);
            worst = fmax(worst, fabs(breakest_load_factor(n, b, 1, 1e8) - 1));
        }
        snprintf(w, sizeof w, "the fully plastic interaction b = 3/2 (1 - n^2): lambda within %.2e of 1 (criterion 1e-6)", worst);
        check(worst < 1e-6, w);
        double a = breakest_load_factor(0.3, 0.8, 1.5, 3), b2 = breakest_load_factor(0.3, -0.8, 1.5, 3);
        snprintf(w, sizeof w, "a load and its mirror: %.12f and %.12f", a, b2);
        check(a == b2, w);
    }
    printf("== B5 Creager and Paris\n");
    {
        double K = 2e6, d = 1e-4, rho = 5e-4;
        double crack = breakest_creager_paris(K, 0, d), root = breakest_creager_paris(K, rho, 0);
        double e1 = fabs(crack / (K / sqrt(2 * M_PI * d)) - 1), e2 = fabs(root / (2 * K / sqrt(M_PI * rho)) - 1);
        char w[160];
        snprintf(w, sizeof w, "the crack's field %.1e, the root's largest stress %.1e (criterion 1e-12)", e1, e2);
        check(e1 < 1e-12 && e2 < 1e-12, w);
    }
    printf("== B6 the bend bar and the critical distance\n");
    {
        double f = breakest_senb_K(1, 1, 1, 0.5);
        double L = breakest_critical_distance(3e6, 60e6), Lx = (1 / M_PI) * (1.0 / 20) * (1.0 / 20);
        char w[160];
        snprintf(w, sizeof w, "f(1/2) %.4f against 10.65 (criterion 1e-3 relative); L %.6e m against %.6e", f, L, Lx);
        check(fabs(f / 10.65 - 1) < 1e-3 && fabs(L / Lx - 1) < 1e-12, w);
    }
    printf("\nbreaktest: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
