/* orbit.c - see orbit.h. */
#include "orbit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_LIGHT 299792458.0
#define MAXS 10

struct Orbit {
    OrbSpec spec;
    int n, s;
    double c[MAXS], b[MAXS], A[MAXS][MAXS];
    double *y, *comp;          /* state (6 n) and its compensated-summation remainder */
    double t, h;
    long steps;
    int ta, tb;                /* the tracked pair */
    double closest, closest_t;
    int origin;                /* the body the pictures are centred on, -1 none */
    /* paths for drawing */
    double *path;
    int npath, cappath;
};

void orb_spec_defaults(OrbSpec *s) {
    memset(s, 0, sizeof *s);
    s->stages = 6;
    s->tolerance = 1e-13;
}

/* Legendre polynomial P_n(x) and its derivative */
static void legendre(int n, double x, double *p, double *dp) {
    double p0 = 1, p1 = x;
    if (n == 0) {
        *p = 1, *dp = 0;
        return;
    }
    for (int k = 2; k <= n; k++) {
        double p2 = ((2 * k - 1) * x * p1 - (k - 1) * p0) / k;
        p0 = p1, p1 = p2;
    }
    *p = p1;
    *dp = n * (x * p1 - p0) / (x * x - 1);
}

/* Gauss-Legendre nodes and weights on [0, 1] */
static void gauss01(int s, double *c, double *w) {
    for (int i = 0; i < s; i++) {
        double x = cos(M_PI * (i + 0.75) / (s + 0.5));
        for (int it = 0; it < 100; it++) {
            double p, dp;
            legendre(s, x, &p, &dp);
            double dx = p / dp;
            x -= dx;
            if (fabs(dx) < 1e-17) break;
        }
        double p, dp;
        legendre(s, x, &p, &dp);
        c[s - 1 - i] = 0.5 * (x + 1);
        w[s - 1 - i] = 1.0 / ((1 - x * x) * dp * dp); /* half of 2 / ((1 - x^2) P'^2) */
    }
}

static double lagrange(const double *c, int s, int j, double t) {
    double v = 1;
    for (int m = 0; m < s; m++)
        if (m != j) v *= (t - c[m]) / (c[j] - c[m]);
    return v;
}

static void accel(const Orbit *o, const double *y, double *f) {
    int n = o->n;
    for (int i = 0; i < n; i++) {
        f[6 * i + 0] = y[6 * i + 3], f[6 * i + 1] = y[6 * i + 4], f[6 * i + 2] = y[6 * i + 5];
        f[6 * i + 3] = f[6 * i + 4] = f[6 * i + 5] = 0;
    }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            double gi = o->spec.bodies[i].gm, gj = o->spec.bodies[j].gm;
            if (gi == 0 && gj == 0) continue;
            double d[3] = {y[6 * j] - y[6 * i], y[6 * j + 1] - y[6 * i + 1], y[6 * j + 2] - y[6 * i + 2]};
            double r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], r = sqrt(r2), inv = 1 / (r2 * r);
            for (int k = 0; k < 3; k++) {
                f[6 * i + 3 + k] += gj * d[k] * inv;
                f[6 * j + 3 + k] -= gi * d[k] * inv;
            }
        }
    if (o->spec.relativity && n > 1) {
        double g0 = o->spec.bodies[0].gm;
        for (int i = 1; i < n; i++) {
            double r[3], v[3];
            for (int k = 0; k < 3; k++) r[k] = y[6 * i + k] - y[k], v[k] = y[6 * i + 3 + k] - y[3 + k];
            double rr = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]), v2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            double rv = r[0] * v[0] + r[1] * v[1] + r[2] * v[2];
            double pre = g0 / (C_LIGHT * C_LIGHT * rr * rr * rr);
            for (int k = 0; k < 3; k++) f[6 * i + 3 + k] += pre * ((4 * g0 / rr - v2) * r[k] + 4 * rv * v[k]);
        }
    }
}

Orbit *orb_create(const OrbSpec *s, char *err, size_t errlen) {
    if (s->n < 1 || s->n > ORB_MAX_BODIES || !(s->dt > 0) || s->stages < 1 || s->stages > MAXS) {
        snprintf(err, errlen, "orbit: 1 to %d bodies, dt > 0, 1 to %d stages", ORB_MAX_BODIES, MAXS);
        return NULL;
    }
    Orbit *o = calloc(1, sizeof *o);
    o->spec = *s;
    o->n = s->n, o->s = s->stages;
    if (!(o->spec.tolerance > 0)) o->spec.tolerance = 1e-13;
    gauss01(o->s, o->c, o->b);
    /* A_ij = integral from 0 to c_i of the j-th Lagrange polynomial, exactly by an s-point Gauss rule on [0, c_i] */
    double gc[MAXS], gw[MAXS];
    gauss01(o->s, gc, gw);
    for (int i = 0; i < o->s; i++)
        for (int j = 0; j < o->s; j++) {
            double acc = 0;
            for (int q = 0; q < o->s; q++) acc += gw[q] * lagrange(o->c, o->s, j, o->c[i] * gc[q]);
            o->A[i][j] = acc * o->c[i];
        }
    o->y = malloc((size_t)6 * o->n * sizeof(double));
    o->comp = calloc((size_t)6 * o->n, sizeof(double));
    for (int i = 0; i < o->n; i++)
        for (int k = 0; k < 3; k++) o->y[6 * i + k] = s->bodies[i].x[k], o->y[6 * i + 3 + k] = s->bodies[i].v[k];
    o->h = s->dt;
    o->ta = o->tb = -1;
    o->closest = INFINITY;
    o->origin = -1;
    return o;
}

void orb_free(Orbit *o) {
    if (!o) return;
    free(o->y), free(o->comp), free(o->path);
    free(o);
}

/* one Gauss-Legendre step of size h from (y, comp) into (yo, co) */
static void gl_step(const Orbit *o, const double *y, const double *comp, double h, double *yo, double *co) {
    int m = 6 * o->n, s = o->s;
    double *K = malloc((size_t)s * m * sizeof(double)), *Kn = malloc((size_t)s * m * sizeof(double)), *Y = malloc((size_t)m * sizeof(double));
    accel(o, y, K);
    for (int i = 1; i < s; i++) memcpy(K + (size_t)i * m, K, (size_t)m * sizeof(double));
    for (int it = 0; it < 200; it++) {
        double dmax = 0, kmax = 0;
        for (int i = 0; i < s; i++) {
            for (int q = 0; q < m; q++) {
                double acc = 0;
                for (int j = 0; j < s; j++) acc += o->A[i][j] * K[(size_t)j * m + q];
                Y[q] = y[q] + h * acc;
            }
            accel(o, Y, Kn + (size_t)i * m);
        }
        for (size_t q = 0; q < (size_t)s * m; q++) {
            dmax = fmax(dmax, fabs(Kn[q] - K[q]));
            kmax = fmax(kmax, fabs(Kn[q]));
        }
        memcpy(K, Kn, (size_t)s * m * sizeof(double));
        if (dmax <= 1e-16 * kmax) break;
    }
    for (int q = 0; q < m; q++) {
        double inc = 0;
        for (int i = 0; i < s; i++) inc += o->b[i] * K[(size_t)i * m + q];
        /* compensated: y + (h inc + comp) */
        double add = h * inc + comp[q];
        double t = y[q] + add;
        co[q] = add - (t - y[q]);
        yo[q] = t;
    }
    free(K), free(Kn), free(Y);
}

static void hermite(const double *x0, const double *v0, const double *x1, const double *v1, double h, double u, double *out) {
    double h00 = 2 * u * u * u - 3 * u * u + 1, h10 = u * u * u - 2 * u * u + u, h01 = -2 * u * u * u + 3 * u * u, h11 = u * u * u - u * u;
    for (int k = 0; k < 3; k++) out[k] = h00 * x0[k] + h10 * h * v0[k] + h01 * x1[k] + h11 * h * v1[k];
}

static void gl_step(const Orbit *o, const double *y, const double *comp, double h, double *yo, double *co);

static double pair_distance(const Orbit *o, const double *y) {
    double d[3];
    for (int k = 0; k < 3; k++) d[k] = y[6 * o->tb + k] - y[6 * o->ta + k];
    return sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

/* the closest approach inside a step: located on the cubic Hermite interpolant, then refined by integrating from
 * the step's start to each trial time with the method itself (the interpolant alone is only third order) */
static void track(Orbit *o, const double *y0, const double *c0, const double *y1, double t0, double h) {
    if (o->ta < 0) return;
    double x0[3], v0[3], x1[3], v1[3];
    for (int k = 0; k < 3; k++) {
        x0[k] = y0[6 * o->tb + k] - y0[6 * o->ta + k], v0[k] = y0[6 * o->tb + 3 + k] - y0[6 * o->ta + 3 + k];
        x1[k] = y1[6 * o->tb + k] - y1[6 * o->ta + k], v1[k] = y1[6 * o->tb + 3 + k] - y1[6 * o->ta + 3 + k];
    }
    double bu = 0, bd = INFINITY;
    for (int q = 0; q <= 64; q++) {
        double u = q / 64.0, p[3];
        hermite(x0, v0, x1, v1, h, u, p);
        double d = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (d < bd) bd = d, bu = u;
    }
    if (bd > 1.01 * o->closest) return;
    int m = 6 * o->n;
    double *yt = malloc((size_t)m * sizeof(double)), *ct = malloc((size_t)m * sizeof(double));
    double lo = fmax(0, bu - 1.0 / 64), hi = fmin(1, bu + 1.0 / 64);
    for (int it = 0; it < 50 && hi - lo > 1e-12; it++) {
        double m1 = lo + 0.381966 * (hi - lo), m2 = lo + 0.618034 * (hi - lo);
        gl_step(o, y0, c0, m1 * h, yt, ct);
        double d1 = pair_distance(o, yt);
        gl_step(o, y0, c0, m2 * h, yt, ct);
        double d2 = pair_distance(o, yt);
        if (d1 < d2) hi = m2;
        else lo = m1;
    }
    double u = 0.5 * (lo + hi);
    gl_step(o, y0, c0, u * h, yt, ct);
    double d = pair_distance(o, yt);
    if (d < o->closest) o->closest = d, o->closest_t = t0 + u * h;
    free(yt), free(ct);
}

void orb_advance(Orbit *o, double t_end) {
    int m = 6 * o->n;
    double *y1 = malloc((size_t)m * sizeof(double)), *c1 = malloc((size_t)m * sizeof(double));
    double *y2 = malloc((size_t)m * sizeof(double)), *c2 = malloc((size_t)m * sizeof(double));
    double *ym = malloc((size_t)m * sizeof(double)), *cm = malloc((size_t)m * sizeof(double));
    while (o->t < t_end * (1 - 1e-15)) {
        double h = o->h;
        bool last = false;
        if (o->t + h >= t_end) h = t_end - o->t, last = true;
        if (!o->spec.adaptive) {
            gl_step(o, o->y, o->comp, h, y1, c1);
            track(o, o->y, o->comp, y1, o->t, h);
            memcpy(o->y, y1, (size_t)m * sizeof(double)), memcpy(o->comp, c1, (size_t)m * sizeof(double));
            o->t += h;
        } else {
            /* step doubling: one step of h against two of h / 2; the two halves are kept */
            gl_step(o, o->y, o->comp, h, y1, c1);
            gl_step(o, o->y, o->comp, 0.5 * h, ym, cm);
            gl_step(o, ym, cm, 0.5 * h, y2, c2);
            double err = 0;
            for (int i = 0; i < o->n; i++)
                for (int k = 0; k < 3; k++) {
                    double sc = 0;
                    for (int j = 0; j < 3; j++) sc += y2[6 * i + j] * y2[6 * i + j];
                    /* relative to the separation from the central body where there is one */
                    double d0 = 0;
                    for (int j = 0; j < 3; j++) d0 += (y2[6 * i + j] - y2[j]) * (y2[6 * i + j] - y2[j]);
                    double scale = i > 0 ? sqrt(d0) : sqrt(sc) + 1;
                    err = fmax(err, fabs(y1[6 * i + k] - y2[6 * i + k]) / (scale > 0 ? scale : 1));
                }
            double fac = err > 0 ? 0.9 * pow(o->spec.tolerance / err, 1.0 / (2 * o->s + 1)) : 2;
            if (fac > 2) fac = 2;
            if (fac < 0.2) fac = 0.2;
            if (err <= o->spec.tolerance || h < 1e-6) {
                track(o, o->y, o->comp, ym, o->t, 0.5 * h);
                track(o, ym, cm, y2, o->t + 0.5 * h, 0.5 * h);
                memcpy(o->y, y2, (size_t)m * sizeof(double)), memcpy(o->comp, c2, (size_t)m * sizeof(double));
                o->t += h;
                if (!last) o->h = h * fac;
            } else {
                o->h = h * fac;
                continue;
            }
        }
        o->steps++;
    }
    free(y1), free(c1), free(y2), free(c2), free(ym), free(cm);
}

double orb_time(const Orbit *o) { return o->t; }
long orb_steps(const Orbit *o) { return o->steps; }

const OrbBody *orb_body(const Orbit *o, int i) {
    static OrbBody b;
    b = o->spec.bodies[i];
    for (int k = 0; k < 3; k++) b.x[k] = o->y[6 * i + k], b.v[k] = o->y[6 * i + 3 + k];
    return &b;
}

double orb_energy(const Orbit *o) {
    double e = 0;
    for (int i = 0; i < o->n; i++) {
        const double *v = o->y + 6 * i + 3;
        e += 0.5 * o->spec.bodies[i].gm * (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int j = i + 1; j < o->n; j++) {
            double d[3] = {o->y[6 * j] - o->y[6 * i], o->y[6 * j + 1] - o->y[6 * i + 1], o->y[6 * j + 2] - o->y[6 * i + 2]};
            e -= o->spec.bodies[i].gm * o->spec.bodies[j].gm / sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        }
    }
    return e;
}

void orb_momentum(const Orbit *o, double p[3], double L[3]) {
    for (int k = 0; k < 3; k++) p[k] = L[k] = 0;
    for (int i = 0; i < o->n; i++) {
        double g = o->spec.bodies[i].gm;
        const double *x = o->y + 6 * i, *v = o->y + 6 * i + 3;
        for (int k = 0; k < 3; k++) p[k] += g * v[k];
        L[0] += g * (x[1] * v[2] - x[2] * v[1]), L[1] += g * (x[2] * v[0] - x[0] * v[2]), L[2] += g * (x[0] * v[1] - x[1] * v[0]);
    }
}

void orb_track(Orbit *o, int a, int b) {
    o->ta = a, o->tb = b;
    o->closest = INFINITY;
}

double orb_closest(const Orbit *o, int a, int b, double *when) {
    if (when) *when = o->closest_t;
    return (a == o->ta && b == o->tb) ? o->closest : NAN;
}

void orb_write_frame(Orbit *o, LabWriter *w, bool paths) {
    int n = o->n;
    double *X = malloc((size_t)n * 3 * sizeof(double));
    float *gm = malloc((size_t)n * sizeof(float)), *rad = malloc((size_t)n * sizeof(float));
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) X[3 * i + k] = o->y[6 * i + k] - (o->origin >= 0 ? o->y[6 * o->origin + k] : 0);
        gm[i] = (float)o->spec.bodies[i].gm, rad[i] = (float)o->spec.bodies[i].radius;
    }
    lab_part_points(w, "bodies", n, X);
    lab_field(w, "gm", LAB_AT_NODE, (size_t)n, gm);
    lab_field(w, "radius", LAB_AT_NODE, (size_t)n, rad);
    if (paths) {
        if (o->npath + 1 > o->cappath) {
            o->cappath = o->cappath ? 2 * o->cappath : 64;
            o->path = realloc(o->path, (size_t)o->cappath * n * 3 * sizeof(double));
        }
        memcpy(o->path + (size_t)o->npath * n * 3, X, (size_t)n * 3 * sizeof(double));
        o->npath++;
        if (o->npath > 1) {
            int nseg = (o->npath - 1) * n;
            int *conn = malloc((size_t)nseg * 2 * sizeof(int));
            float *who = malloc((size_t)nseg * sizeof(float));
            int k = 0;
            for (int f = 0; f + 1 < o->npath; f++)
                for (int i = 0; i < n; i++) conn[2 * k] = f * n + i, conn[2 * k + 1] = (f + 1) * n + i, who[k++] = (float)i;
            lab_part_cells(w, "paths", o->npath * n, o->path, nseg, LAB_LINE, conn);
            lab_field(w, "body", LAB_AT_CELL, (size_t)nseg, who);
            free(conn), free(who);
        }
    }
    free(X), free(gm), free(rad);
}

void orb_set_frame_origin(Orbit *o, int origin) { o->origin = origin; }

char *orb_header_json(const OrbSpec *s, const char *title) {
    char *h = malloc(4096);
    int n = snprintf(h, 4096,
                     "{\"domain\":\"orbit\",\"title\":\"%s\",\"solver\":\"src/lab/orbit: Gauss-Legendre IRK, %d stages (order %d)%s%s\",\"bodies\":[",
                     title ? title : "", s->stages, 2 * s->stages, s->adaptive ? ", adaptive" : ", fixed step", s->relativity ? ", 1PN" : "");
    for (int i = 0; i < s->n && n < 3900; i++) n += snprintf(h + n, 4096 - (size_t)n, "%s\"%s\"", i ? "," : "", s->bodies[i].name);
    snprintf(h + n, 4096 - (size_t)n, "],\"fields\":{\"gm\":\"m^3/s^2\",\"radius\":\"m\",\"body\":\"1\"}}");
    return h;
}
