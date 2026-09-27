/* water.c - weakly compressible SPH for water with a free surface (the model is described in water.h). */
#include "water.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../threads.h"

struct Wt {
    WtSpec s;
    double h, aD, B, c0, m, Ly;
    int n, cap;        /* water */
    double *x, *v, *rho, *drho, *a, *depth, *p; /* p: the pressure of each, from its density, once a step */
    unsigned char *out;
    int nw, wcap;      /* walls */
    double *wx, *wp, *wrho;
    /* cells: the region cut into boxes at least h wide; particles sorted by box every step */
    int g[3];
    double cs[3];
    int *cstart, *order, *pcell, *rank;
    int *nbr, *nnb, K, nbr_cap;
    unsigned char *wet; /* each water particle's neighbours (sorted indices), K at most, found in the force pass */
    struct Sorted *sorted;
    double t;
    long steps;
    bool unstable, ready;
    double hmax;       /* the deepest water at the start, for the speed of sound */
    ThreadPool *pool;
    double amax_t[64], vmax_t[64];
};

void wt_spec_defaults(WtSpec *s) {
    memset(s, 0, sizeof *s);
    s->dx = 0.01, s->h_factor = 1.7, s->rho0 = 1000, s->alpha = 0.02, s->delta = 0.1, s->g = 9.81, s->cfl = 0.25;
    for (int a = 0; a < 3; a++) s->lo[a] = 0, s->hi[a] = 1;
}

Wt *wt_create(const WtSpec *s, char *err, size_t errlen) {
    if (!(s->dx > 0) || !(s->rho0 > 0) || !(s->hi[0] > s->lo[0] && s->hi[1] > s->lo[1] && s->hi[2] > s->lo[2])) {
        snprintf(err, errlen, "water: a particle spacing, a density and a region are required");
        return NULL;
    }
    Wt *w = calloc(1, sizeof *w);
    w->s = *s;
    if (!(w->s.h_factor > 0)) w->s.h_factor = 1.7;
    if (!(w->s.cfl > 0)) w->s.cfl = 0.25;
    w->h = w->s.h_factor * s->dx;
    w->aD = 21.0 / (16 * M_PI * w->h * w->h * w->h);
    w->m = s->rho0 * s->dx * s->dx * s->dx;
    w->Ly = s->hi[1] - s->lo[1];
    if (s->periodic_y && w->Ly < 4.2 * w->h) {
        snprintf(err, errlen, "water: a periodic y needs a width of at least 4.2 smoothing lengths (%.4g m)", 4.2 * w->h);
        free(w);
        return NULL;
    }
    size_t nc = 1;
    for (int a = 0; a < 3; a++) {
        double L = s->hi[a] - s->lo[a];
        w->g[a] = (int)fmax(1, floor(L / w->h)); /* cells at least h wide: the support 2h spans two of them */
        w->cs[a] = L / w->g[a];
        nc *= (size_t)w->g[a];
    }
    if (nc > 50000000) {
        snprintf(err, errlen, "water: the region is too large for its particle spacing (%zu cells)", nc);
        free(w);
        return NULL;
    }
    w->cstart = calloc(nc + 1, sizeof(int));
    int nt = s->threads > 0 ? s->threads : cpu_perf_count();
    if (nt > 64) nt = 64;
    if (nt > 1) w->pool = pool_create(nt);
    return w;
}

void wt_free(Wt *w) {
    if (!w) return;
    if (w->pool) pool_destroy(w->pool);
    free(w->x), free(w->v), free(w->rho), free(w->drho), free(w->a), free(w->depth), free(w->p), free(w->out);
    free(w->wx), free(w->wp), free(w->wrho);
    free(w->cstart), free(w->order), free(w->pcell), free(w->rank), free(w->sorted), free(w->nbr), free(w->nnb), free(w->wet);
    free(w);
}

static void run(Wt *w, int count, int grain, ParallelFn fn, void *ctx) {
    if (count <= 0) return;
    if (w->pool) pool_for(w->pool, count, grain, fn, ctx);
    else fn(ctx, 0, count, 0);
}

/* ---- particles ----------------------------------------------------------------------------------------------------- */

static void grow_water(Wt *w, int need) {
    if (need <= w->cap) return;
    int c = w->cap ? w->cap : 4096;
    while (c < need) c *= 2;
    w->x = realloc(w->x, (size_t)c * 3 * sizeof(double)), w->v = realloc(w->v, (size_t)c * 3 * sizeof(double));
    w->a = realloc(w->a, (size_t)c * 3 * sizeof(double));
    w->rho = realloc(w->rho, (size_t)c * sizeof(double)), w->drho = realloc(w->drho, (size_t)c * sizeof(double));
    w->depth = realloc(w->depth, (size_t)c * sizeof(double)), w->p = realloc(w->p, (size_t)c * sizeof(double)), w->out = realloc(w->out, (size_t)c);
    w->cap = c;
}

static void grow_walls(Wt *w, int need) {
    if (need <= w->wcap) return;
    int c = w->wcap ? w->wcap : 4096;
    while (c < need) c *= 2;
    w->wx = realloc(w->wx, (size_t)c * 3 * sizeof(double));
    w->wp = realloc(w->wp, (size_t)c * sizeof(double)), w->wrho = realloc(w->wrho, (size_t)c * sizeof(double));
    w->wcap = c;
}

int wt_add_water(Wt *w, const double lo[3], const double hi[3], WtSurfaceFn surface, void *ctx) {
    const double dx = w->s.dx;
    int i0[3], i1[3], added = 0;
    for (int a = 0; a < 3; a++) i0[a] = (int)floor(lo[a] / dx), i1[a] = (int)ceil(hi[a] / dx);
    for (int k = i0[2]; k <= i1[2]; k++)
        for (int j = i0[1]; j <= i1[1]; j++)
            for (int i = i0[0]; i <= i1[0]; i++) {
                double x[3] = {(i + 0.5) * dx, (j + 0.5) * dx, (k + 0.5) * dx};
                if (x[0] < lo[0] || x[0] > hi[0] || x[1] < lo[1] || x[1] > hi[1] || x[2] < lo[2] || x[2] > hi[2]) continue;
                double top = surface ? surface(x[0], x[1], ctx) : hi[2];
                if (x[2] > top) continue;
                grow_water(w, w->n + 1);
                int n = w->n++;
                for (int a = 0; a < 3; a++) w->x[3 * n + a] = x[a], w->v[3 * n + a] = 0, w->a[3 * n + a] = 0;
                w->rho[n] = w->s.rho0, w->drho[n] = 0, w->depth[n] = top - x[2], w->out[n] = 0;
                w->hmax = fmax(w->hmax, top - lo[2]);
                added++;
            }
    return added;
}

int wt_add_solid_fn(Wt *w, const double lo[3], const double hi[3], WtSolidFn sdf, void *ctx, int layers) {
    const double dx = w->s.dx;
    int i0[3], i1[3], added = 0;
    for (int a = 0; a < 3; a++) i0[a] = (int)floor(lo[a] / dx), i1[a] = (int)ceil(hi[a] / dx);
    for (int k = i0[2]; k <= i1[2]; k++)
        for (int j = i0[1]; j <= i1[1]; j++)
            for (int i = i0[0]; i <= i1[0]; i++) {
                double x[3] = {(i + 0.5) * dx, (j + 0.5) * dx, (k + 0.5) * dx};
                if (x[0] < lo[0] || x[0] > hi[0] || x[1] < lo[1] || x[1] > hi[1] || x[2] < lo[2] || x[2] > hi[2]) continue;
                double d = sdf(x, ctx);
                if (d > 0 || d <= -layers * dx) continue;
                grow_walls(w, w->nw + 1);
                int n = w->nw++;
                for (int a = 0; a < 3; a++) w->wx[3 * n + a] = x[a];
                w->wp[n] = 0, w->wrho[n] = w->s.rho0;
                added++;
            }
    return added;
}

int wt_add_water_fn(Wt *w, const double lo[3], const double hi[3], WtSurfaceFn surface, void *sctx, WtSolidFn solid, void *ctx) {
    const double dx = w->s.dx;
    int i0[3], i1[3], added = 0;
    for (int a = 0; a < 3; a++) i0[a] = (int)floor(lo[a] / dx), i1[a] = (int)ceil(hi[a] / dx);
    for (int k = i0[2]; k <= i1[2]; k++)
        for (int j = i0[1]; j <= i1[1]; j++)
            for (int i = i0[0]; i <= i1[0]; i++) {
                double x[3] = {(i + 0.5) * dx, (j + 0.5) * dx, (k + 0.5) * dx};
                if (x[0] < lo[0] || x[0] > hi[0] || x[1] < lo[1] || x[1] > hi[1] || x[2] < lo[2] || x[2] > hi[2]) continue;
                double top = surface ? surface(x[0], x[1], sctx) : hi[2];
                if (x[2] > top || (solid && solid(x, ctx) < 0.5 * dx)) continue;
                grow_water(w, w->n + 1);
                int n = w->n++;
                for (int a = 0; a < 3; a++) w->x[3 * n + a] = x[a], w->v[3 * n + a] = 0, w->a[3 * n + a] = 0;
                w->rho[n] = w->s.rho0, w->drho[n] = 0, w->depth[n] = top - x[2], w->out[n] = 0;
                w->hmax = fmax(w->hmax, top - lo[2]);
                added++;
            }
    return added;
}

int wt_add_solid_box(Wt *w, const double lo[3], const double hi[3], int layers, int open) {
    const double dx = w->s.dx, skin = layers * dx;
    int i0[3], i1[3], added = 0;
    for (int a = 0; a < 3; a++) i0[a] = (int)floor(lo[a] / dx), i1[a] = (int)ceil(hi[a] / dx);
    for (int k = i0[2]; k <= i1[2]; k++)
        for (int j = i0[1]; j <= i1[1]; j++)
            for (int i = i0[0]; i <= i1[0]; i++) {
                double x[3] = {(i + 0.5) * dx, (j + 0.5) * dx, (k + 0.5) * dx};
                if (x[0] < lo[0] || x[0] > hi[0] || x[1] < lo[1] || x[1] > hi[1] || x[2] < lo[2] || x[2] > hi[2]) continue;
                bool near = false;
                for (int a = 0; a < 3 && !near; a++) {
                    if (!(open & (1 << (2 * a))) && x[a] - lo[a] < skin) near = true;
                    if (!(open & (2 << (2 * a))) && hi[a] - x[a] < skin) near = true;
                }
                if (!near) continue;
                grow_walls(w, w->nw + 1);
                int n = w->nw++;
                for (int a = 0; a < 3; a++) w->wx[3 * n + a] = x[a];
                w->wp[n] = 0, w->wrho[n] = w->s.rho0;
                added++;
            }
    return added;
}

int wt_add_tank(Wt *w, const double lo[3], const double hi[3], int layers) {
    const double t = layers * w->s.dx;
    int added = 0;
    double ylo = w->s.periodic_y ? w->s.lo[1] : lo[1] - t, yhi = w->s.periodic_y ? w->s.hi[1] : hi[1] + t;
    /* the floor, then the two ends in x, then (without a periodic y) the two sides */
    added += wt_add_solid_box(w, (double[3]){lo[0] - t, ylo, lo[2] - t}, (double[3]){hi[0] + t, yhi, lo[2]}, layers, 0);
    added += wt_add_solid_box(w, (double[3]){lo[0] - t, ylo, lo[2]}, (double[3]){lo[0], yhi, hi[2]}, layers, 0);
    added += wt_add_solid_box(w, (double[3]){hi[0], ylo, lo[2]}, (double[3]){hi[0] + t, yhi, hi[2]}, layers, 0);
    if (!w->s.periodic_y) {
        added += wt_add_solid_box(w, (double[3]){lo[0], lo[1] - t, lo[2]}, (double[3]){hi[0], lo[1], hi[2]}, layers, 0);
        added += wt_add_solid_box(w, (double[3]){lo[0], hi[1], lo[2]}, (double[3]){hi[0], hi[1] + t, hi[2]}, layers, 0);
    }
    return added;
}

/* ---- the kernel ---------------------------------------------------------------------------------------------------- */

/* grad_i W_ij = x_ij * F(r); F < 0 inside the support */
static inline double kernel_f(const Wt *w, double r) {
    double q = r / w->h;
    if (q >= 2) return 0;
    double u = 1 - 0.5 * q;
    return -5 * w->aD / (w->h * w->h) * u * u * u;
}
static inline double kernel_w(const Wt *w, double r) {
    double q = r / w->h;
    if (q >= 2) return 0;
    double u = 1 - 0.5 * q;
    return w->aD * u * u * u * u * (2 * q + 1);
}

/* ---- cells --------------------------------------------------------------------------------------------------------- */

static int cell_of(const Wt *w, const double *x) {
    int c[3];
    for (int a = 0; a < 3; a++) {
        c[a] = (int)floor((x[a] - w->s.lo[a]) / w->cs[a]);
        if (c[a] < 0 || c[a] >= w->g[a]) return -1;
    }
    return (c[2] * w->g[1] + c[1]) * w->g[0] + c[0];
}

/* every particle, water first (index i) then walls (index n + k), sorted by cell */
static void build_cells(Wt *w) {
    int N = w->n + w->nw;
    size_t nc = (size_t)w->g[0] * w->g[1] * w->g[2];
    w->order = realloc(w->order, (size_t)(N > 0 ? N : 1) * sizeof(int));
    w->pcell = realloc(w->pcell, (size_t)(N > 0 ? N : 1) * sizeof(int));
    memset(w->cstart, 0, (nc + 1) * sizeof(int));
    for (int i = 0; i < N; i++) {
        int c = i < w->n ? (w->out[i] ? -1 : cell_of(w, &w->x[3 * i])) : cell_of(w, &w->wx[3 * (i - w->n)]);
        w->pcell[i] = c;
        if (c >= 0) w->cstart[c + 1]++;
    }
    for (size_t c = 0; c < nc; c++) w->cstart[c + 1] += w->cstart[c];
    int *fill = malloc((nc + 1) * sizeof(int));
    memcpy(fill, w->cstart, (nc + 1) * sizeof(int));
    for (int i = 0; i < N; i++)
        if (w->pcell[i] >= 0) w->order[fill[w->pcell[i]]++] = i;
    free(fill);
    /* which cells hold water: the walls' pass skips the rest (most of a tank's wall is dry) */
    w->wet = realloc(w->wet, nc);
    memset(w->wet, 0, nc);
    for (int i = 0; i < w->n; i++)
        if (w->pcell[i] >= 0) w->wet[w->pcell[i]] = 1;
}

/* the cells around a point (two on each side, as cells are at least h wide), each once: a periodic y of five cells or
 * fewer would otherwise be visited twice */
static int neighbour_cells(const Wt *w, const double *x, int *out) {
    int c[3];
    for (int a = 0; a < 3; a++) c[a] = (int)floor((x[a] - w->s.lo[a]) / w->cs[a]);
    int ys[5], ny = 0;
    if (w->s.periodic_y && w->g[1] <= 5) {
        for (int j = 0; j < w->g[1]; j++) ys[ny++] = j;
    } else
        for (int d = -2; d <= 2; d++) {
            int j = c[1] + d;
            if (w->s.periodic_y) j = (j + w->g[1]) % w->g[1];
            else if (j < 0 || j >= w->g[1]) continue;
            ys[ny++] = j;
        }
    int m = 0;
    for (int dk = -2; dk <= 2; dk++) {
        int k = c[2] + dk;
        if (k < 0 || k >= w->g[2]) continue;
        for (int y = 0; y < ny; y++)
            for (int di = -2; di <= 2; di++) {
                int i = c[0] + di;
                if (i < 0 || i >= w->g[0]) continue;
                out[m++] = (k * w->g[1] + ys[y]) * w->g[0] + i;
            }
    }
    return m;
}

static inline void separation(const Wt *w, const double *xi, const double *xj, double *d) {
    d[0] = xi[0] - xj[0], d[1] = xi[1] - xj[1], d[2] = xi[2] - xj[2];
    if (w->s.periodic_y) d[1] -= w->Ly * round(d[1] / w->Ly);
}

static inline double pressure_of(const Wt *w, double rho) {
    double r = rho / w->s.rho0, r2 = r * r;
    return w->B * (r2 * r2 * r2 * r - 1);
}

/* ---- the step ------------------------------------------------------------------------------------------------------ */

/* every particle in cell order, so that the loops over neighbours read memory in sequence */
typedef struct Sorted {
    double x[3], v[3], rho, p;
    int id; /* water: its index; wall k: -1 - k */
} Sorted;

static void sort_particles(Wt *w) {
    int N = w->cstart[(size_t)w->g[0] * w->g[1] * w->g[2]];
    w->sorted = realloc(w->sorted, (size_t)(N > 0 ? N : 1) * sizeof(Sorted));
    w->rank = realloc(w->rank, (size_t)(w->n > 0 ? w->n : 1) * sizeof(int));
    Sorted *S = w->sorted;
    for (int q = 0; q < N; q++) {
        int i = w->order[q];
        if (i < w->n) {
            memcpy(S[q].x, &w->x[3 * i], sizeof S[q].x), memcpy(S[q].v, &w->v[3 * i], sizeof S[q].v);
            S[q].rho = w->rho[i], S[q].p = w->p[i], S[q].id = i;
            w->rank[i] = q;
        } else {
            int k = i - w->n;
            memcpy(S[q].x, &w->wx[3 * k], sizeof S[q].x);
            S[q].v[0] = S[q].v[1] = S[q].v[2] = 0, S[q].rho = w->wrho[k], S[q].p = w->wp[k], S[q].id = -1 - k;
        }
    }
}

/* the walls' pressure from the water next to them (Adami et al. 2012), clamped at zero so that water does not stick */
static void wall_pressure(void *ctx, int k0, int k1, int tid) {
    (void)tid;
    Wt *w = ctx;
    const Sorted *S = w->sorted;
    int cells[125];
    for (int k = k0; k < k1; k++) {
        const double *xw = &w->wx[3 * k];
        int nc = neighbour_cells(w, xw, cells);
        double sw = 0, sp = 0, sr = 0;
        for (int c = 0; c < nc; c++)
            for (int q = w->wet[cells[c]] ? w->cstart[cells[c]] : 0; w->wet[cells[c]] && q < w->cstart[cells[c] + 1]; q++) {
                if (S[q].id < 0) continue;
                double d[3];
                separation(w, xw, S[q].x, d);
                double W = kernel_w(w, sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
                if (W <= 0) continue;
                sw += W, sp += S[q].p * W, sr += S[q].rho * d[2] * W;
            }
        double p = sw > 0 ? (sp - w->s.g * sr) / sw : 0; /* g . (x_w - x_f) with g along -z */
        if (p < 0) p = 0;
        w->wp[k] = p, w->wrho[k] = w->s.rho0 * pow(p / w->B + 1, 1.0 / 7);
    }
}

static void water_rates(void *ctx, int i0, int i1, int tid) {
    Wt *w = ctx;
    const Sorted *S = w->sorted;
    const double h = w->h, h4 = 4 * h * h, c0 = w->c0, m = w->m, alpha = w->s.alpha, g = w->s.g;
    int cells[125];
    double amax = w->amax_t[tid], vmax = w->vmax_t[tid];
    for (int i = i0; i < i1; i++) {
        if (w->out[i]) {
            w->a[3 * i] = w->a[3 * i + 1] = w->a[3 * i + 2] = 0;
            continue;
        }
        const Sorted *P = &S[w->rank[i]];
        double ri = P->rho, pri = P->p / (ri * ri);
        double acc[3] = {0, 0, -g};
        int *list = &w->nbr[(size_t)i * w->K], nn = 0;
        int nc = neighbour_cells(w, P->x, cells);
        for (int c = 0; c < nc; c++)
            for (int q = w->cstart[cells[c]]; q < w->cstart[cells[c] + 1]; q++) {
                const Sorted *Q = &S[q];
                if (Q == P) continue;
                double d[3];
                separation(w, P->x, Q->x, d);
                double r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                if (r2 >= h4 || r2 <= 0) continue;
                if (nn < w->K) list[nn] = q;
                nn++;
                double F = kernel_f(w, sqrt(r2)), rj = Q->rho;
                double vr = (P->v[0] - Q->v[0]) * d[0] + (P->v[1] - Q->v[1]) * d[1] + (P->v[2] - Q->v[2]) * d[2];
                double Pi = 0;
                if (vr < 0) {
                    double mu = h * vr / (r2 + 0.01 * h * h);
                    Pi = -alpha * c0 * mu / (0.5 * (ri + rj));
                }
                double f = -m * (pri + Q->p / (rj * rj) + Pi) * F;
                acc[0] += f * d[0], acc[1] += f * d[1], acc[2] += f * d[2];
            }
        for (int a = 0; a < 3; a++) w->a[3 * i + a] = acc[a];
        w->nnb[i] = nn <= w->K ? nn : -1; /* -1: the list overflowed, the density pass searches the cells again */
        amax = fmax(amax, sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]));
        vmax = fmax(vmax, sqrt(P->v[0] * P->v[0] + P->v[1] * P->v[1] + P->v[2] * P->v[2]));
    }
    w->amax_t[tid] = amax, w->vmax_t[tid] = vmax;
}

static inline double density_pair(const Wt *w, const Sorted *P, const Sorted *Q, double ri, double kd, double kh, double m) {
    double d[3];
    separation(w, P->x, Q->x, d);
    double F = kernel_f(w, sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    double dr = m * ((P->v[0] - Q->v[0]) * d[0] + (P->v[1] - Q->v[1]) * d[1] + (P->v[2] - Q->v[2]) * d[2]) * F;
    if (Q->id >= 0) /* delta-SPH between water particles, less the hydrostatic difference rho0 g (z_i - z_j) / c0^2 */
        dr += kd * 2 * ((Q->rho - ri) - kh * d[2]) * (-F) / Q->rho;
    return dr;
}

/* the density's rate with the velocities of the end of the step: the continuity equation and the delta-SPH diffusion
 * (a separate pass so that density, like position, moves with the new velocity: with the old one the pair of them is
 * forward Euler for sound waves, which grows at every step) */
static void water_density(void *ctx, int i0, int i1, int tid) {
    (void)tid;
    Wt *w = ctx;
    const Sorted *S = w->sorted;
    const double h = w->h, h4 = 4 * h * h, c0 = w->c0, rho0 = w->s.rho0, m = w->m, delta = w->s.delta, g = w->s.g;
    (void)h;
    const double kd = delta * h * c0 * m, kh = rho0 * g / (c0 * c0);
    int cells[125];
    for (int i = i0; i < i1; i++) {
        if (w->out[i]) {
            w->drho[i] = 0;
            continue;
        }
        const Sorted *P = &S[w->rank[i]];
        double ri = P->rho, dr = 0;
        const int *list = &w->nbr[(size_t)i * w->K];
        int nn = w->nnb[i];
        if (nn < 0) { /* the list overflowed: search the cells */
            nn = 0;
            int nc = neighbour_cells(w, P->x, cells);
            for (int c = 0; c < nc; c++)
                for (int q = w->cstart[cells[c]]; q < w->cstart[cells[c] + 1]; q++) {
                    double d[3];
                    separation(w, P->x, S[q].x, d);
                    double r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                    if (&S[q] != P && r2 < h4 && r2 > 0) dr += density_pair(w, P, &S[q], ri, kd, kh, m);
                }
        }
        for (int k = 0; k < nn; k++) dr += density_pair(w, P, &S[list[k]], ri, kd, kh, m);
        w->drho[i] = dr;
    }
}

static void start(Wt *w) {
    if (!(w->s.c0 > 0)) w->c0 = 10 * sqrt(w->s.g * fmax(w->hmax, 10 * w->s.dx));
    else w->c0 = w->s.c0;
    w->B = w->s.rho0 * w->c0 * w->c0 / 7;
    for (int i = 0; i < w->n; i++) w->rho[i] = w->s.rho0 * pow(w->s.rho0 * w->s.g * w->depth[i] / w->B + 1, 1.0 / 7);
    w->ready = true;
}

double wt_step(Wt *w) {
    if (!w->ready) start(w);
    if (w->unstable || w->n == 0) return 0;
    build_cells(w);
    for (int i = 0; i < w->n; i++) w->p[i] = pressure_of(w, w->rho[i]);
    sort_particles(w);
    if (!w->nbr || w->nbr_cap < w->n) {
        double rs = 2 * w->h / w->s.dx;
        w->K = (int)ceil(4.19 * rs * rs * rs * 1.5) + 16;
        w->nbr_cap = w->n;
        free(w->nbr), free(w->nnb);
        w->nbr = malloc((size_t)w->n * w->K * sizeof(int)), w->nnb = malloc((size_t)w->n * sizeof(int));
    }
    run(w, w->nw, 256, wall_pressure, w);
    int N = w->cstart[(size_t)w->g[0] * w->g[1] * w->g[2]];
    for (int q = 0; q < N; q++)
        if (w->sorted[q].id < 0) {
            int k = -1 - w->sorted[q].id;
            w->sorted[q].rho = w->wrho[k], w->sorted[q].p = w->wp[k];
        }
    for (int t = 0; t < 64; t++) w->amax_t[t] = 0, w->vmax_t[t] = 0;
    run(w, w->n, 256, water_rates, w);
    double amax = 0, vmax = 0;
    for (int t = 0; t < 64; t++) amax = fmax(amax, w->amax_t[t]), vmax = fmax(vmax, w->vmax_t[t]);
    double dt = w->s.cfl * fmin(w->h / (w->c0 + vmax), amax > 0 ? sqrt(w->h / amax) : INFINITY);
    const double Ly = w->Ly;
    for (int i = 0; i < w->n; i++)
        if (!w->out[i])
            for (int a = 0; a < 3; a++) w->v[3 * i + a] += dt * w->a[3 * i + a];
    for (int i = 0; i < w->n; i++)
        if (!w->out[i]) memcpy(w->sorted[w->rank[i]].v, &w->v[3 * i], 3 * sizeof(double));
    run(w, w->n, 256, water_density, w);
    for (int i = 0; i < w->n; i++) {
        if (w->out[i]) continue;
        double *x = &w->x[3 * i], *v = &w->v[3 * i];
        for (int a = 0; a < 3; a++) x[a] += dt * v[a];
        w->rho[i] += dt * w->drho[i];
        if (w->s.periodic_y) x[1] -= Ly * floor((x[1] - w->s.lo[1]) / Ly);
        for (int a = 0; a < 3; a++)
            if (x[a] < w->s.lo[a] || x[a] >= w->s.hi[a]) w->out[i] = 1, v[0] = v[1] = v[2] = 0;
        if (!isfinite(x[0] + x[1] + x[2] + w->rho[i]) || w->rho[i] < 0.5 * w->s.rho0 || w->rho[i] > 2 * w->s.rho0) w->unstable = true;
    }
    w->t += dt, w->steps++;
    return dt;
}

/* ---- queries ------------------------------------------------------------------------------------------------------- */

double wt_time(const Wt *w) { return w->t; }
long wt_steps(const Wt *w) { return w->steps; }
int wt_count(const Wt *w) { return w->n; }
int wt_wall_count(const Wt *w) { return w->nw; }
double wt_c0(const Wt *w) { return w->c0; }
bool wt_unstable(const Wt *w) { return w->unstable; }
const double *wt_x(const Wt *w) { return w->x; }
const double *wt_v(const Wt *w) { return w->v; }
const double *wt_rho(const Wt *w) { return w->rho; }
const double *wt_wall_x(const Wt *w) { return w->wx; }
double wt_pressure(const Wt *w, int i) { return pressure_of(w, w->rho[i]); }

void wt_energy(const Wt *w, double *kinetic, double *potential, double *elastic) {
    double k = 0, p = 0, e = 0, r0 = w->s.rho0;
    for (int i = 0; i < w->n; i++) {
        const double *v = &w->v[3 * i];
        k += 0.5 * w->m * (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        p += w->m * w->s.g * w->x[3 * i + 2];
        /* m times the work of compression, the integral of p / rho^2 from rho0 to rho for Tait's equation */
        double r = w->ready ? w->rho[i] : r0;
        e += w->m * w->B * ((pow(r / r0, 6) - 1) / (6 * r0) + 1 / r - 1 / r0);
    }
    if (kinetic) *kinetic = k;
    if (potential) *potential = p;
    if (elastic) *elastic = e;
}

void wt_centre(const Wt *w, double c[3]) {
    c[0] = c[1] = c[2] = 0;
    int n = 0;
    for (int i = 0; i < w->n; i++)
        if (!w->out[i]) c[0] += w->x[3 * i], c[1] += w->x[3 * i + 1], c[2] += w->x[3 * i + 2], n++;
    if (n)
        for (int a = 0; a < 3; a++) c[a] /= n;
}

void wt_prepare(Wt *w) {
    if (!w->ready) start(w);
}
void wt_params(const Wt *w, WtParams *p) {
    p->h = w->h, p->c0 = w->c0, p->B = w->B, p->mass = w->m, p->rho0 = w->s.rho0, p->g = w->s.g, p->alpha = w->s.alpha;
    p->delta = w->s.delta, p->cfl = w->s.cfl, p->Ly = w->Ly, p->periodic_y = w->s.periodic_y;
    for (int a = 0; a < 3; a++) p->lo[a] = w->s.lo[a], p->hi[a] = w->s.hi[a], p->cell[a] = w->cs[a], p->grid[a] = w->g[a];
}
double *wt_x_rw(Wt *w) { return w->x; }
double *wt_v_rw(Wt *w) { return w->v; }
double *wt_rho_rw(Wt *w) { return w->rho; }
unsigned char *wt_out_rw(Wt *w) { return w->out; }
void wt_set_clock(Wt *w, double t, long steps) { w->t = t, w->steps = steps; }
