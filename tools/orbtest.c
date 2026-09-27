/* orbtest.c - verification of the N-body solver (src/lab/orbit). Criteria written before the first run:
 *
 *   O1 Kepler: a test particle on an orbit of eccentricity 0.6 about the Sun, 100 revolutions, 6 stages, 200 steps a
 *      revolution: position within 1e-9 of the closed form (Kepler's equation) relative to the semi-major axis, energy
 *      drift below 1e-12.
 *   O2 relativity: Mercury's orbit about the Sun alone, 100 revolutions with the 1PN term: the perihelion advances by
 *      6 pi GM / (c^2 a (1 - e^2)) a revolution, within 1 per cent (42.98 arcseconds a century).
 *   O3 Sun, Jupiter and Saturn for 10 000 years, 100-day steps: energy within 1e-11 and angular momentum within 1e-12
 *      (relative), as a symplectic method with converged stages must keep them.
 *   O4 a hyperbolic flyby of the Earth (2 km/s at infinity, 10 000 km impact parameter), adaptive steps: the closest
 *      distance within 1e-7 of the two-body value, found between steps by the tracker. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../src/lab/orbit/orbit.h"

static int failures;
static void verdict(bool ok, const char *w) {
    printf("  %s: %s\n", w, ok ? "pass" : "FAIL");
    if (!ok) failures++;
}
static const double GMS = 1.32712440018e20, AU = 1.495978707e11, C = 299792458.0;

int main(void) {
    printf("== O1: Kepler orbit, e = 0.6, 100 revolutions\n");
    {
        double a = AU, e = 0.6, P = 2 * M_PI * sqrt(a * a * a / GMS);
        OrbSpec s;
        orb_spec_defaults(&s);
        s.n = 2;
        s.bodies[0] = (OrbBody){"sun", GMS, 0, {0, 0, 0}, {0, 0, 0}};
        double rp = a * (1 - e), vp = sqrt(GMS / a * (1 + e) / (1 - e));
        s.bodies[1] = (OrbBody){"particle", 0, 0, {rp, 0, 0}, {0, vp, 0}};
        s.dt = P / 200;
        char err[256];
        Orbit *o = orb_create(&s, err, sizeof err);
        double e0 = 0.5 * vp * vp - GMS / rp;
        orb_advance(o, 100 * P);
        const OrbBody *b = orb_body(o, 1);
        /* the closed form at t = 100 P is the start again (the mean anomaly is a multiple of 2 pi) */
        double dx = hypot(b->x[0] - rp, b->x[1]) / a;
        double v2 = b->v[0] * b->v[0] + b->v[1] * b->v[1], e1 = 0.5 * v2 - GMS / hypot(b->x[0], b->x[1]);
        printf("  position error %.2e of a, energy drift %.2e, %ld steps\n", dx, fabs((e1 - e0) / e0), orb_steps(o));
        verdict(dx < 1e-9 && fabs((e1 - e0) / e0) < 1e-12, "O1");
        orb_free(o);
    }
    printf("== O2: Mercury's perihelion, 1PN\n");
    {
        double a = 0.38709893 * AU, e = 0.20563069, P = 2 * M_PI * sqrt(a * a * a / GMS);
        OrbSpec s;
        orb_spec_defaults(&s);
        s.n = 2, s.relativity = true;
        s.bodies[0] = (OrbBody){"sun", GMS, 0, {0, 0, 0}, {0, 0, 0}};
        double rp = a * (1 - e), vp = sqrt(GMS / a * (1 + e) / (1 - e));
        s.bodies[1] = (OrbBody){"mercury", 0, 0, {rp, 0, 0}, {0, vp, 0}};
        s.dt = P / 400;
        char err[256];
        Orbit *o = orb_create(&s, err, sizeof err);
        int N = 100;
        orb_advance(o, N * P);
        /* the direction of the Laplace-Runge-Lenz vector (Newtonian part) points to the perihelion */
        const OrbBody *b = orb_body(o, 1);
        double r = hypot(b->x[0], b->x[1]), L = b->x[0] * b->v[1] - b->x[1] * b->v[0];
        double Ax = b->v[1] * L - GMS * b->x[0] / r, Ay = -b->v[0] * L - GMS * b->x[1] / r;
        double adv = atan2(Ay, Ax); /* radians over N revolutions (it started along +x) */
        double theory = N * 6 * M_PI * GMS / (C * C * a * (1 - e * e));
        double per_century = adv / N * (100 * 365.25 * 86400 / P) * 180 / M_PI * 3600;
        printf("  advance %.4e rad against %.4e (%+.3f %%): %.2f arcseconds a century\n", adv, theory, 100 * (adv - theory) / theory, per_century);
        verdict(fabs(adv - theory) / theory < 0.01, "O2");
        orb_free(o);
    }
    printf("== O3: Sun, Jupiter, Saturn, 10 000 years\n");
    {
        OrbSpec s;
        orb_spec_defaults(&s);
        s.n = 3;
        double gJ = 1.26686534e17, gS = 3.7931187e16;
        s.bodies[0] = (OrbBody){"sun", GMS, 0, {0, 0, 0}, {0, 0, 0}};
        double aJ = 5.2044 * AU, aS = 9.5826 * AU;
        s.bodies[1] = (OrbBody){"jupiter", gJ, 0, {aJ, 0, 0}, {0, sqrt(GMS / aJ), 0}};
        s.bodies[2] = (OrbBody){"saturn", gS, 0, {0, aS, 0}, {-sqrt(GMS / aS), 0, 0.02 * sqrt(GMS / aS)}};
        s.dt = 100 * 86400.0;
        char err[256];
        Orbit *o = orb_create(&s, err, sizeof err);
        double E0 = orb_energy(o), p0[3], L0[3];
        orb_momentum(o, p0, L0);
        clock_t c0 = clock();
        orb_advance(o, 10000 * 365.25 * 86400);
        double E1 = orb_energy(o), p1[3], L1[3];
        orb_momentum(o, p1, L1);
        double dL = sqrt(pow(L1[0] - L0[0], 2) + pow(L1[1] - L0[1], 2) + pow(L1[2] - L0[2], 2)) / sqrt(L0[0] * L0[0] + L0[1] * L0[1] + L0[2] * L0[2]);
        printf("  energy %.2e, angular momentum %.2e relative; %ld steps, %.1f s\n", fabs((E1 - E0) / E0), dL, orb_steps(o), (double)(clock() - c0) / CLOCKS_PER_SEC);
        verdict(fabs((E1 - E0) / E0) < 1e-11 && dL < 1e-12, "O3");
        orb_free(o);
    }
    printf("== O4: a hyperbolic flyby of the Earth, adaptive\n");
    {
        double gE = 3.986004418e14, vinf = 2000, bimp = 1e7;
        OrbSpec s;
        orb_spec_defaults(&s);
        s.n = 2, s.adaptive = true, s.tolerance = 1e-12;
        s.bodies[0] = (OrbBody){"earth", gE, 0, {0, 0, 0}, {0, 0, 0}};
        double d0 = 2e9; /* start far away, on the incoming asymptote */
        s.bodies[1] = (OrbBody){"body", 0, 0, {-d0, bimp, 0}, {vinf, 0, 0}};
        s.dt = 3600;
        char err[256];
        Orbit *o = orb_create(&s, err, sizeof err);
        orb_track(o, 0, 1);
        /* the exact state far away is not on the asymptote; use the exact conic through the starting state */
        double r = hypot(d0, bimp), v2 = vinf * vinf, eps = 0.5 * v2 - gE / r, a = -gE / (2 * eps);
        double h = d0 * 0 + (-d0) * 0; /* h = x vy - y vx */
        h = (-d0) * 0 - bimp * vinf;
        double ecc = sqrt(1 + 2 * eps * h * h / (gE * gE));
        double q = -a * (ecc - 1) * -1; /* for a hyperbola a < 0: q = a (1 - e) */
        q = a * (1 - ecc);
        orb_advance(o, 2 * d0 / vinf);
        double when, dmin = orb_closest(o, 0, 1, &when);
        printf("  closest %.6f km against %.6f km (%.2e), at t %.1f h, %ld steps\n", dmin / 1e3, q / 1e3, fabs(dmin - q) / q, when / 3600, orb_steps(o));
        verdict(fabs(dmin - q) / q < 1e-7, "O4");
        orb_free(o);
    }
    printf(failures ? "orbtest: %d FAILED\n" : "orbtest: all passed\n", failures);
    return failures ? 1 : 0;
}
