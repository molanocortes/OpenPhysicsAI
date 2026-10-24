/* peri.c - bond-based peridynamics with brittle bonds (peri.h). */
#include "peri.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct Peri {
    PeriSpec s;
    int n;
    double *X, *x, *v, *a;   /* reference, current, velocity, acceleration: 3 per point */
    long *start;             /* bonds of point i: start[i] .. start[i + 1] */
    int *nb;
    float *len, *beta;       /* reference length, volume factor */
    unsigned char *broken;
    double V, c, s0, delta, kfloor;
    unsigned char *held;     /* gripped points: their velocity is prescribed */
    double *hv;
    long nbonds;
};

typedef struct {
    Peri *P;
    double dt;
    int phase;
} Ctx;

static void corr_beta(double r, double delta, double h, float *beta) {
    *beta = r <= delta - 0.5 * h ? 1.f : r < delta + 0.5 * h ? (float)((delta + 0.5 * h - r) / h) : 0.f;
}

Peri *peri_create(const PeriSpec *s, PeriSdf sdf, void *ctx, const double lo[3], const double hi[3], char *err, size_t errlen) {
    if (!(s->h > 0) || !(s->density > 0) || !(s->bulk_modulus > 0) || !(s->fracture_energy > 0)) {
        snprintf(err, errlen, "peri: spacing, density, bulk modulus and fracture energy must be positive");
        return NULL;
    }
    Peri *P = calloc(1, sizeof *P);
    if (!P) return NULL;
    P->s = *s;
    double m = s->horizon > 0 ? s->horizon : 3.0, h = s->h;
    P->delta = m * h, P->V = h * h * h;
    /* the micromodulus from the lattice's own sum: the continuum's c = 18 K / (pi delta^4) comes from the integral of
     * |xi| over the horizon's sphere, pi delta^4; over this lattice (with the partial volumes) that sum is S, and
     * c = 18 K / S makes an interior point's energy exact (a 3-spacing horizon overshot by 6 %: peritest P1) */
    {
        double S = 0, reach = P->delta + 0.5 * h;
        int R = (int)ceil(reach / h);
        for (int a = -R; a <= R; a++)
            for (int b = -R; b <= R; b++)
                for (int cc = -R; cc <= R; cc++) {
                    double r = h * sqrt((double)(a * a + b * b + cc * cc));
                    if (r == 0 || r >= reach) continue;
                    float be;
                    corr_beta(r, P->delta, h, &be);
                    S += r * be * P->V;
                }
        P->c = 18 * s->bulk_modulus / S;
    }
    P->s0 = sqrt(5 * s->fracture_energy / (9 * s->bulk_modulus * P->delta));
    /* the lattice points inside */
    int N[3];
    for (int k = 0; k < 3; k++) N[k] = (int)floor((hi[k] - lo[k]) / h);
    size_t cap = 1024, n = 0;
    double *X = malloc(3 * cap * sizeof *X);
    for (int k = 0; X && k < N[2]; k++)
        for (int j = 0; j < N[1]; j++)
            for (int i = 0; i < N[0]; i++) {
                double p[3] = {lo[0] + (i + 0.5) * h, lo[1] + (j + 0.5) * h, lo[2] + (k + 0.5) * h};
                if (sdf(p, ctx) >= 0) continue;
                if (n == cap) {
                    cap *= 2;
                    double *t = realloc(X, 3 * cap * sizeof *X);
                    if (!t) { free(X), X = NULL; break; }
                    X = t;
                }
                memcpy(&X[3 * n++], p, sizeof p);
            }
    if (!X || !n) {
        snprintf(err, errlen, "peri: %s", X ? "no points inside the body at this spacing" : "out of memory");
        free(X), free(P);
        return NULL;
    }
    P->n = (int)n, P->X = X;
    P->x = malloc(3 * n * sizeof(double)), P->v = calloc(3 * n, sizeof(double)), P->a = calloc(3 * n, sizeof(double));
    memcpy(P->x, X, 3 * n * sizeof(double));
    /* neighbours by a grid of cells one horizon wide */
    double reach = P->delta + 0.5 * h;
    int G[3];
    double gl[3];
    for (int k = 0; k < 3; k++) gl[k] = lo[k], G[k] = (int)ceil((hi[k] - lo[k]) / reach) + 1;
    size_t ncell = (size_t)G[0] * G[1] * G[2];
    int *head = malloc(ncell * sizeof(int)), *next = malloc(n * sizeof(int));
    P->start = malloc((n + 1) * sizeof(long));
    if (!P->x || !P->v || !P->a || !head || !next || !P->start) {
        snprintf(err, errlen, "peri: out of memory");
        free(head), free(next), peri_free(P);
        return NULL;
    }
    for (size_t c = 0; c < ncell; c++) head[c] = -1;
    int *cell_of = malloc(3 * n * sizeof(int));
    for (size_t i = 0; i < n; i++) {
        int ci[3];
        for (int k = 0; k < 3; k++) ci[k] = (int)((X[3 * i + k] - gl[k]) / reach), cell_of[3 * i + k] = ci[k];
        size_t c = (size_t)ci[0] + (size_t)G[0] * ((size_t)ci[1] + (size_t)G[1] * ci[2]);
        next[i] = head[c], head[c] = (int)i;
    }
    for (int pass = 0; pass < 2; pass++) {
        long nb = 0;
        for (size_t i = 0; i < n; i++) {
            P->start[i] = nb;
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dxc = -1; dxc <= 1; dxc++) {
                        int cx = cell_of[3 * i] + dxc, cy = cell_of[3 * i + 1] + dy, cz = cell_of[3 * i + 2] + dz;
                        if (cx < 0 || cy < 0 || cz < 0 || cx >= G[0] || cy >= G[1] || cz >= G[2]) continue;
                        for (int j = head[(size_t)cx + (size_t)G[0] * ((size_t)cy + (size_t)G[1] * cz)]; j >= 0; j = next[j]) {
                            if ((size_t)j == i) continue;
                            double d0 = X[3 * j] - X[3 * i], d1 = X[3 * j + 1] - X[3 * i + 1], d2 = X[3 * j + 2] - X[3 * i + 2];
                            double r = sqrt(d0 * d0 + d1 * d1 + d2 * d2);
                            if (r >= reach) continue;
                            if (pass == 1) {
                                P->nb[nb] = j, P->len[nb] = (float)r;
                                corr_beta(r, P->delta, h, &P->beta[nb]);
                            }
                            nb++;
                        }
                    }
        }
        P->start[n] = nb;
        if (pass == 0) {
            P->nbonds = nb;
            P->nb = malloc((size_t)nb * sizeof(int)), P->len = malloc((size_t)nb * sizeof(float)), P->beta = malloc((size_t)nb * sizeof(float));
            P->broken = calloc((size_t)nb, 1);
            if (!P->nb || !P->len || !P->beta || !P->broken) {
                snprintf(err, errlen, "peri: out of memory for %ld bonds", nb);
                free(head), free(next), free(cell_of), peri_free(P);
                return NULL;
            }
        }
    }
    free(head), free(next), free(cell_of);
    double cp = sqrt(1.8 * s->bulk_modulus / s->density); /* the P-wave speed at Poisson's ratio 1/4 */
    P->kfloor = s->floor_stiffness > 0 ? s->floor_stiffness : (cp / h) * (cp / h); /* an acceleration per metre of overlap */
    return P;
}

void peri_free(Peri *P) {
    if (!P) return;
    free(P->held), free(P->hv);
    free(P->X), free(P->x), free(P->v), free(P->a), free(P->start), free(P->nb), free(P->len), free(P->beta), free(P->broken), free(P);
}

int peri_count(const Peri *P) { return P->n; }
long peri_bonds(const Peri *P) { return P->nbonds; }
const double *peri_reference(const Peri *P) { return P->X; }
void peri_constants(const Peri *P, double *c, double *s0) { *c = P->c, *s0 = P->s0; }

void peri_set_velocity(Peri *P, const double v[3]) {
    for (int i = 0; i < P->n; i++) memcpy(&P->v[3 * i], v, 3 * sizeof(double));
}

double peri_stable_dt(const Peri *P) {
    /* Silling and Askari 2005: dt < sqrt(2 rho / sum_j c V_j / |xi|), for the most bonded point */
    double worst = 0;
    for (int i = 0; i < P->n; i++) {
        double sum = 0;
        for (long b = P->start[i]; b < P->start[i + 1]; b++) sum += P->c * P->beta[b] * P->V / P->len[b];
        worst = fmax(worst, sum);
    }
    return worst > 0 ? 0.8 * sqrt(2 * P->s.density / worst) : 0;
}

static void forces(void *vc, int i0, int i1, int tid) {
    Ctx *C = vc;
    Peri *P = C->P;
    const double c = P->c, V = P->V, s0 = P->s0, rho = P->s.density, fz = P->s.floor_z, h = P->s.h;
    for (int i = i0; i < i1; i++) {
        double f[3] = {0, 0, -P->s.gravity * rho};
        const double *xi = &P->x[3 * i];
        for (long b = P->start[i]; b < P->start[i + 1]; b++) {
            if (P->broken[b]) continue;
            const double *xj = &P->x[3 * P->nb[b]];
            double d0 = xj[0] - xi[0], d1 = xj[1] - xi[1], d2 = xj[2] - xi[2], r = sqrt(d0 * d0 + d1 * d1 + d2 * d2), L = P->len[b];
            double st = (r - L) / L;
            if (st > s0) { /* breaks for good; the pair's other copy sees the same stretch and breaks too */
                P->broken[b] = 1;
                continue;
            }
            double k = c * st * V * P->beta[b] / r;
            f[0] += k * d0, f[1] += k * d1, f[2] += k * d2;
        }
        double *a = &P->a[3 * i];
        a[0] = f[0] / rho, a[1] = f[1] / rho, a[2] = f[2] / rho;
        double pen = fz + 0.5 * h - xi[2];
        if (pen > 0) a[2] += P->kfloor * pen;
    }
}

static void kick_drift(void *vc, int i0, int i1, int tid) {
    Ctx *C = vc;
    Peri *P = C->P;
    for (int i = i0; i < i1; i++)
        for (int k = 0; k < 3; k++) {
            if (P->held && P->held[i]) {
                P->v[3 * i + k] = P->hv[3 * i + k];
                if (C->phase == 0) P->x[3 * i + k] += C->dt * P->v[3 * i + k];
                continue;
            }
            P->v[3 * i + k] += 0.5 * C->dt * P->a[3 * i + k];
            if (C->phase == 0) P->x[3 * i + k] += C->dt * P->v[3 * i + k];
        }
}

void peri_step(Peri *P, double dt, ThreadPool *pool) {
    Ctx C = {P, dt, 0};
    int grain = 256;
    pool_for(pool, P->n, grain, kick_drift, &C); /* half kick and drift */
    pool_for(pool, P->n, grain, forces, &C);
    C.phase = 1;
    pool_for(pool, P->n, grain, kick_drift, &C); /* the second half kick */
}

void peri_state(const Peri *P, double *x, double *v, double *damage) {
    if (x) memcpy(x, P->x, 3 * (size_t)P->n * sizeof(double));
    if (v) memcpy(v, P->v, 3 * (size_t)P->n * sizeof(double));
    if (damage)
        for (int i = 0; i < P->n; i++) {
            long tot = P->start[i + 1] - P->start[i], br = 0;
            for (long b = P->start[i]; b < P->start[i + 1]; b++) br += P->broken[b];
            damage[i] = tot ? (double)br / tot : 0;
        }
}

double peri_point_energy(const Peri *P, int i) {
    double w = 0;
    const double *xi = &P->x[3 * i];
    for (long b = P->start[i]; b < P->start[i + 1]; b++) {
        if (P->broken[b]) continue;
        const double *xj = &P->x[3 * P->nb[b]];
        double d0 = xj[0] - xi[0], d1 = xj[1] - xi[1], d2 = xj[2] - xi[2], r = sqrt(d0 * d0 + d1 * d1 + d2 * d2), L = P->len[b];
        double st = (r - L) / L;
        w += 0.25 * P->c * st * st * L * P->V * P->beta[b]; /* half the bond's energy density, a quarter with the pair's */
    }
    return w * P->V;
}

void peri_energy(const Peri *P, double *kinetic, double *elastic, long *broken) {
    double k = 0, e = 0;
    long br = 0;
    for (int i = 0; i < P->n; i++) {
        const double *v = &P->v[3 * i];
        k += 0.5 * P->s.density * P->V * (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        e += peri_point_energy(P, i);
        for (long b = P->start[i]; b < P->start[i + 1]; b++) br += P->broken[b];
    }
    if (kinetic) *kinetic = k;
    if (elastic) *elastic = e;
    if (broken) *broken = br / 2;
}

void peri_displace(Peri *P, void (*u)(const double x[3], double out[3], void *ctx), void *ctx) {
    for (int i = 0; i < P->n; i++) {
        double d[3];
        u(&P->X[3 * i], d, ctx);
        for (int k = 0; k < 3; k++) P->x[3 * i + k] = P->X[3 * i + k] + d[k];
    }
}

long peri_cut(Peri *P, int axis, double c0, int along, double a0, double a1) {
    long cut = 0;
    for (int i = 0; i < P->n; i++)
        for (long b = P->start[i]; b < P->start[i + 1]; b++) {
            const double *a = &P->X[3 * i], *c = &P->X[3 * P->nb[b]];
            if ((a[axis] - c0) * (c[axis] - c0) >= 0) continue;
            double t = (c0 - a[axis]) / (c[axis] - a[axis]), y = a[along] + t * (c[along] - a[along]);
            if (y >= a0 && y <= a1) P->broken[b] = 1, cut++;
        }
    return cut / 2;
}

void peri_set_velocity_field(Peri *P, void (*v)(const double x[3], double out[3], void *ctx), void *ctx) {
    for (int i = 0; i < P->n; i++) v(&P->X[3 * i], &P->v[3 * i], ctx);
}

void peri_grip(Peri *P, const double lo[3], const double hi[3], const double v[3]) {
    if (!P->held) P->held = calloc((size_t)P->n, 1), P->hv = calloc(3 * (size_t)P->n, sizeof(double));
    if (!P->held || !P->hv) return;
    for (int i = 0; i < P->n; i++) {
        const double *x = &P->X[3 * i];
        if (x[0] < lo[0] || x[1] < lo[1] || x[2] < lo[2] || x[0] > hi[0] || x[1] > hi[1] || x[2] > hi[2]) continue;
        P->held[i] = 1;
        memcpy(&P->hv[3 * i], v, 3 * sizeof(double)), memcpy(&P->v[3 * i], v, 3 * sizeof(double));
    }
}
