/* rttest.c - verification of light near a black hole (src/lab/relativity).
 *
 * Criteria, written on 2026-09-26 before the first run and not to be changed without a dated note (M = 1):
 *   R1  weak-field bending: a ray of impact parameter b = 1000 is bent by 4/b + 15 pi / (4 b^2) (the second-order
 *       expansion of the exact Schwarzschild deflection); criterion: within 0.01 %.
 *   R2  the photon sphere: rays are captured below the critical impact parameter 3 sqrt(3) = 5.196152 and escape above
 *       it; the boundary found by bisection within 1e-5 of it.
 *   R3  the first integral (du/dphi)^2 + u^2 - 2 u^3 = 1 / b^2 is kept along a strongly bent ray (b = 5.3, bent by
 *       more than a full turn) within 1e-9 relative.
 *       Amended 2026-09-26 after the first run: b = 5.3 bends a ray by 3.56 rad, not a full turn, so the premise was
 *       wrong (the integral was kept to 1.1e-14). The ray is now b = 5.1965, 1e-4 above the photon sphere's value, which
 *       winds more than once; the tolerance is unchanged.
 *   R4  the disk's frequency shift: a photon leaving an orbit at r = 10 straight up the axis (no Doppler part along its
 *       path) reaches a static observer far away with g = sqrt(1 - 3 / r) (the orbit's time dilation, with gravity);
 *       traced from the observer, criterion: within 1e-3. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/lab/relativity/blackhole.h"

static int failures;

static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    fflush(stdout);
    if (!ok) failures++;
}

int main(void) {
    printf("== R1: weak-field bending of light\n");
    {
        double b = 1000, drift, a = bh_deflection(b, &drift), e = 4 / b + 15 * M_PI / (4 * b * b);
        printf("  b = %.0f: bent by %.10e rad, expansion %.10e (%+.5f %%); first integral kept to %.1e\n", b, a, e, 100 * (a / e - 1), drift);
        verdict(isfinite(a) && fabs(a / e - 1) < 1e-4, "R1");
    }
    printf("== R2: the photon sphere, the edge between captured and escaping rays\n");
    {
        double lo = 5.0, hi = 5.4; /* captured at lo, escaping at hi */
        bool ok = bh_captured(lo) && !bh_captured(hi);
        for (int k = 0; k < 40 && ok; k++) {
            double m = 0.5 * (lo + hi);
            if (bh_captured(m)) lo = m;
            else hi = m;
        }
        double bc = 0.5 * (lo + hi), e = 3 * sqrt(3.0);
        printf("  critical impact parameter %.7f, 3 sqrt(3) = %.7f (%+.2e)\n", bc, e, bc - e);
        verdict(ok && fabs(bc - e) < 1e-5, "R2");
    }
    printf("== R3: the first integral along a strongly bent ray\n");
    {
        printf("  recorded first run: b = 5.3 bent by 3.5579 rad (0.57 turns), integral kept to 1.09e-14; FAIL on the premise\n");
        double drift, a = bh_deflection(5.1965, &drift);
        printf("  b = 5.1965: bent by %.4f rad (%.2f turns), first integral kept to %.2e\n", a, a / (2 * M_PI), drift);
        verdict(isfinite(a) && a > 2 * M_PI && drift < 1e-9, "R3");
    }
    printf("== R4: the shift of light from an orbiting disk, seen from far along the axis\n");
    {
        BhSpec s;
        bh_spec_defaults(&s);
        s.r_in = 6, s.r_out = 22; /* hit radius grows smoothly with the aim across the disk: bisect it to r = 10 */
        /* an observer far up the axis; a ray aimed so that it meets the disk at r = 10: bisect its angle (the first
         * version of this set-up bracketed on a thin ring, which a ray misses on both sides: recorded, criterion kept) */
        double R = 1e4, cam[3] = {0, 0, R};
        double lo = 0.0008, hi = 0.0020, g = 0, rh = 0;
        for (int k = 0; k < 60; k++) {
            double m = 0.5 * (lo + hi), d[3] = {sin(m), 0, -cos(m)};
            BhHit h;
            bh_trace(&s, cam, d, &h);
            if (h.fate != 2) break;
            g = h.g, rh = h.r_hit;
            if (fabs(h.r_hit - 10) < 1e-9) break;
            if (h.r_hit < 10) lo = m;
            else hi = m;
        }
        double e = sqrt(1 - 3.0 / 10) / sqrt(1 - 2.0 / R);
        printf("  hit at r = %.6f with g = %.6f; sqrt(1 - 3/r) = %.6f (%+.2e)\n", rh, g, e, g - e);
        verdict(fabs(rh - 10) < 1e-3 && fabs(g - e) < 1e-3, "R4");
    }
    printf(failures ? "rttest: %d FAILED\n" : "rttest: all passed\n", failures);
    return failures ? 1 : 0;
}
