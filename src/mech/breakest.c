/* breakest.c - the break estimate of a printed part (breakest.h). */
#include "breakest.h"

#include <math.h>

/* the section in units: depth 1 (y from -1/2 to 1/2), tensile strength 1, strain in units of the elastic limit strain
 * s_t/E; the stress s(e) = e clamped to [-c, 1]. At breaking the top fibre is at e = m and the strain falls with the
 * curvature k: e(y) = m - k (1/2 - y). Integrals of s and s (e - m) over e from m - k to m, piece by piece. */
static void moments(double c, double m, double k, double *N, double *M) {
    if (k <= 0) {
        *N = fmin(m, 1.0), *M = 0;
        return;
    }
    const double lo = m - k, hi = m;
    /* S0 = integral of s de; T = integral of s (e - m) de, kept apart from S0 so a large m does not cancel */
    double S0 = 0, T = 0;
    double a = fmax(lo, 1.0), b = hi; /* e >= 1: s = 1 */
    if (b > a) S0 += b - a, T += -0.5 * (a - m) * (a - m);
    a = fmax(lo, -c), b = fmin(hi, 1.0); /* -c <= e <= 1: s = e */
    if (b > a) S0 += 0.5 * (b - a) * (b + a), T += (b * b * b - a * a * a) / 3 - m * 0.5 * (b - a) * (b + a);
    a = lo, b = fmin(hi, -c); /* e <= -c: s = -c */
    if (b > a) S0 += -c * (b - a), T += -c * 0.5 * ((b - m) * (b - m) - (a - m) * (a - m));
    /* y = 1/2 + (e - m)/k, dy = de/k */
    *N = S0 / k;
    *M = (0.5 * S0 + T / k) / k;
}

double breakest_bending_ratio(double c, double m) {
    if (!(c > 0) || !(m >= 1)) return NAN;
    if (isinf(m)) return 3 * c / (1 + c); /* fully plastic: the tension block c/(1+c) deep, the lever arm half the depth */
    /* the curvature at which the axial force vanishes: N falls from 1 (k = 0) toward -c */
    double lo = 0, hi = 1;
    double N, M;
    for (moments(c, m, hi, &N, &M); N > 0 && hi < 1e12; moments(c, m, hi, &N, &M)) hi *= 2;
    for (int it = 0; it < 200 && hi - lo > 1e-14 * hi; it++) {
        double k = 0.5 * (lo + hi);
        moments(c, m, k, &N, &M);
        if (N > 0) lo = k;
        else hi = k;
    }
    moments(c, m, 0.5 * (lo + hi), &N, &M);
    return 6 * M;
}

double breakest_load_factor(double n, double b, double c, double m) {
    if (!(c > 0) || !(m >= 1) || (n == 0 && b == 0)) return NAN;
    if (b < 0) b = -b; /* the section is symmetric in depth: the tension on the other side */
    if (b == 0 && n < 0) return c / -n; /* squashed: no fibre in tension */
    if (b == 0) return 1 / n;           /* pulled: every fibre at the strength */
    if (isinf(m)) m = 1e9;
    /* the breaking states, from pure tension (k = 0: N 1, 6M 0) through pure bending to all but squashed (k large:
     * N -> -c, 6M -> 0); along them the angle of (N, 6M) rises from 0 to pi. The ray at the load's angle meets them once. */
    const double want = atan2(b, n);
    double lo = 0, hi = 1, N, M;
    for (moments(c, m, hi, &N, &M); atan2(6 * M, N) < want && hi < 1e15; moments(c, m, hi, &N, &M)) hi *= 2;
    for (int it = 0; it < 200 && hi - lo > 1e-14 * hi; it++) { /* not below: m - k must stay apart from m */
        double k = 0.5 * (lo + hi);
        moments(c, m, k, &N, &M);
        if (atan2(6 * M, N) < want) lo = k;
        else hi = k;
    }
    moments(c, m, 0.5 * (lo + hi), &N, &M);
    return hypot(N, 6 * M) / hypot(n, b);
}

double breakest_critical_distance(double Kc, double s0) { return Kc * Kc / (M_PI * s0 * s0); }

double breakest_creager_paris(double K, double rho, double d) {
    double r = d + 0.5 * rho;
    return K / sqrt(2 * M_PI * r) * (1 + rho / (2 * r));
}

double breakest_senb_K(double P, double B, double W, double a) {
    double x = a / W;
    double f = 6 * sqrt(x) * (1.99 - x * (1 - x) * (2.15 - 3.93 * x + 2.7 * x * x)) / ((1 + 2 * x) * pow(1 - x, 1.5));
    return P / (B * sqrt(W)) * f;
}
