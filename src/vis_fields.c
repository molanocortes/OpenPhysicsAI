/* vis_fields.c - derived scalar fields with range statistics, and trilinear sampling */
#include "vis.h"
#include "common.h"

#define HIST_BINS 4096

typedef struct {
    double min, max;
    uint64_t n;
} FieldStats;

typedef struct {
    const VisGrid *g;
    VisField field;
    float *out;
    const uint8_t *zero_row; /* nx zero bytes, stands in for g->solid when it is NULL */
    float *scratch;          /* 9 * nx floats per thread (velocity gradient rows) */
    FieldStats *stats;       /* per thread */
    uint32_t *hist;          /* HIST_BINS per thread */
    uint64_t *under;         /* per thread: finite fluid values below the histogram range */
    double hlo, hscale;
    float hlo_f, hhi_f;
} FieldCtx;

static void run_parallel(ThreadPool *pool, int count, int grain, ParallelFn fn, void *ctx) {
    if (pool) pool_for(pool, count, grain, fn, ctx);
    else fn(ctx, 0, count, 0);
}

/* Derivatives of one scalar component along row (j,k): central differences inside the domain,
 * one-sided at its faces, zero along axes of extent 1. */
static void row_derivs(const float *a, int nx, int ny, int nz, int j, int k, float *dx, float *dy, float *dz) {
    ptrdiff_t sxy = (ptrdiff_t)nx * ny;
    const float *r = a + (ptrdiff_t)nx * j + sxy * k;
    if (nx > 1) {
        dx[0] = r[1] - r[0];
        for (int i = 1; i < nx - 1; i++) dx[i] = 0.5f * (r[i + 1] - r[i - 1]);
        dx[nx - 1] = r[nx - 1] - r[nx - 2];
    } else {
        dx[0] = 0.0f;
    }
    int jm = j > 0 ? j - 1 : j, jp = j < ny - 1 ? j + 1 : j;
    float sy = jp > jm ? 1.0f / (float)(jp - jm) : 0.0f;
    const float *ym = r + (ptrdiff_t)nx * (jm - j), *yp = r + (ptrdiff_t)nx * (jp - j);
    for (int i = 0; i < nx; i++) dy[i] = sy * (yp[i] - ym[i]);
    int km = k > 0 ? k - 1 : k, kp = k < nz - 1 ? k + 1 : k;
    float sz = kp > km ? 1.0f / (float)(kp - km) : 0.0f;
    const float *zm = r + sxy * (km - k), *zp = r + sxy * (kp - k);
    for (int i = 0; i < nx; i++) dz[i] = sz * (zp[i] - zm[i]);
}

static void field_slab(void *vctx, int k0, int k1, int tid) {
    FieldCtx *c = vctx;
    const VisGrid *g = c->g;
    int nx = g->nx, ny = g->ny, nz = g->nz;
    float *t = c->scratch + (size_t)tid * 9 * (size_t)nx;
    /* d[3*comp + axis] = d u_comp / d x_axis */
    float *d[9];
    for (int q = 0; q < 9; q++) d[q] = t + (size_t)q * (size_t)nx;
    FieldStats st = c->stats[tid];
    for (int k = k0; k < k1; k++) {
        for (int j = 0; j < ny; j++) {
            size_t b = (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k);
            const uint8_t *s = g->solid ? g->solid + b : c->zero_row;
            const float *ux = g->ux + b, *uy = g->uy + b, *uz = g->uz + b;
            float *o = c->out + b;
            switch (c->field) {
            case VIS_SPEED:
                for (int i = 0; i < nx; i++) {
                    float v = sqrtf(ux[i] * ux[i] + uy[i] * uy[i] + uz[i] * uz[i]);
                    o[i] = s[i] ? 0.0f : v;
                }
                break;
            case VIS_UX:
                for (int i = 0; i < nx; i++) o[i] = s[i] ? 0.0f : ux[i];
                break;
            case VIS_UY:
                for (int i = 0; i < nx; i++) o[i] = s[i] ? 0.0f : uy[i];
                break;
            case VIS_UZ:
                for (int i = 0; i < nx; i++) o[i] = s[i] ? 0.0f : uz[i];
                break;
            case VIS_PRESSURE:
                if (g->rho) {
                    const float *r = g->rho + b;
                    for (int i = 0; i < nx; i++) o[i] = (r[i] - 1.0f) * (1.0f / 3.0f);
                } else {
                    memset(o, 0, (size_t)nx * sizeof(float));
                }
                break;
            case VIS_VORTICITY:
            case VIS_QCRITERION:
                row_derivs(g->ux, nx, ny, nz, j, k, d[0], d[1], d[2]);
                row_derivs(g->uy, nx, ny, nz, j, k, d[3], d[4], d[5]);
                row_derivs(g->uz, nx, ny, nz, j, k, d[6], d[7], d[8]);
                if (c->field == VIS_VORTICITY) {
                    for (int i = 0; i < nx; i++) {
                        float wx = d[7][i] - d[5][i], wy = d[2][i] - d[6][i], wz = d[3][i] - d[1][i];
                        float v = sqrtf(wx * wx + wy * wy + wz * wz);
                        o[i] = s[i] ? 0.0f : v;
                    }
                } else {
                    /* Q = 0.5 (|Omega|^2 - |S|^2) = -0.5 tr(A^2), A_ij = du_i/dx_j */
                    for (int i = 0; i < nx; i++) {
                        float a11 = d[0][i], a22 = d[4][i], a33 = d[8][i];
                        float v = -0.5f * (a11 * a11 + a22 * a22 + a33 * a33) -
                                  (d[1][i] * d[3][i] + d[2][i] * d[6][i] + d[5][i] * d[7][i]);
                        o[i] = s[i] ? 0.0f : v;
                    }
                }
                break;
            default:
                memset(o, 0, (size_t)nx * sizeof(float));
                break;
            }
            for (int i = 0; i < nx; i++) {
                float v = o[i];
                if (!is_finite_f32(v)) { /* e.g. a diverged solver: keep NaN/Inf out of textures and statistics */
                    o[i] = 0.0f;
                    continue;
                }
                if (s[i]) continue;
                if (v < st.min) st.min = v;
                if (v > st.max) st.max = v;
                st.n++;
            }
        }
    }
    c->stats[tid] = st;
}

static void hist_slab(void *vctx, int k0, int k1, int tid) {
    FieldCtx *c = vctx;
    const VisGrid *g = c->g;
    int nx = g->nx, ny = g->ny;
    uint32_t *h = c->hist + (size_t)tid * HIST_BINS;
    uint64_t under = 0;
    float lo = c->hlo_f, hi = c->hhi_f;
    double hlo = c->hlo, scale = c->hscale;
    for (int k = k0; k < k1; k++) {
        for (int j = 0; j < ny; j++) {
            size_t b = (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k);
            const uint8_t *s = g->solid ? g->solid + b : c->zero_row;
            const float *o = c->out + b;
            for (int i = 0; i < nx; i++) {
                float v = o[i];
                if (s[i] || !is_finite_f32(v)) continue;
                if (v < lo) {
                    under++;
                    continue;
                }
                if (v > hi) continue;
                int bin = (int)(((double)v - hlo) * scale);
                bin = bin < 0 ? 0 : (bin >= HIST_BINS ? HIST_BINS - 1 : bin);
                h[bin]++;
            }
        }
    }
    c->under[tid] += under;
}

/* Histogram of the finite fluid values in [lo, hi], merged over threads. */
static void build_hist(FieldCtx *c, ThreadPool *pool, int nt, double lo, double hi, uint32_t *merged,
                       uint64_t *under) {
    memset(c->hist, 0, (size_t)nt * HIST_BINS * sizeof(uint32_t));
    memset(c->under, 0, (size_t)nt * sizeof(uint64_t));
    c->hlo = lo;
    c->hscale = HIST_BINS / (hi - lo);
    c->hlo_f = (float)lo;
    c->hhi_f = (float)hi;
    run_parallel(pool, c->g->nz, 1, hist_slab, c);
    memset(merged, 0, HIST_BINS * sizeof(uint32_t));
    *under = 0;
    for (int t = 0; t < nt; t++) {
        const uint32_t *h = c->hist + (size_t)t * HIST_BINS;
        for (int b = 0; b < HIST_BINS; b++) merged[b] += h[b];
        *under += c->under[t];
    }
}

/* Value of rank q*(total-1) (0-based) from a histogram over [lo, hi]; *bin receives the bin used
 * (-1 below the range, HIST_BINS above). Linear interpolation inside the bin. */
static double hist_percentile(const uint32_t *h, uint64_t under, uint64_t total, double q, double lo, double hi,
                              int *bin) {
    double rank = q * (double)(total - 1), cum = (double)under;
    if (rank < cum) {
        *bin = -1;
        return lo;
    }
    for (int b = 0; b < HIST_BINS; b++) {
        if (h[b] && rank < cum + h[b]) {
            *bin = b;
            return lo + ((double)b + (rank - cum + 0.5) / h[b]) * (hi - lo) / HIST_BINS;
        }
        cum += h[b];
    }
    *bin = HIST_BINS;
    return hi;
}

static void compute_range(FieldCtx *c, ThreadPool *pool, int nt, double mn, double mx, uint64_t n, VisRange *r) {
    r->min = (float)mn;
    r->max = (float)mx;
    r->lo = r->min;
    r->hi = r->max;
    if (!(mx > mn)) return;
    uint32_t merged[HIST_BINS];
    uint64_t under;
    int b1, b99;
    build_hist(c, pool, nt, mn, mx, merged, &under);
    double p1 = hist_percentile(merged, under, n, 0.01, mn, mx, &b1);
    double p99 = hist_percentile(merged, under, n, 0.99, mn, mx, &b99);
    /* Outliers squeeze the bulk into few bins: refine over the bins that hold the robust range. */
    if (b1 >= 0 && b99 < HIST_BINS && b99 - b1 < HIST_BINS / 16) {
        double w = (mx - mn) / HIST_BINS, rlo = mn + b1 * w, rhi = mn + (b99 + 1) * w;
        if (rhi > rlo) {
            build_hist(c, pool, nt, rlo, rhi, merged, &under);
            p1 = hist_percentile(merged, under, n, 0.01, rlo, rhi, &b1);
            p99 = hist_percentile(merged, under, n, 0.99, rlo, rhi, &b99);
        }
    }
    r->lo = (float)CLAMP(p1, mn, mx);
    r->hi = (float)CLAMP(p99, mn, mx);
    if (r->hi < r->lo) r->hi = r->lo;
}

void vis_compute_field(const VisGrid *g, VisField field, float *out, VisRange *range, ThreadPool *pool) {
    if (range) *range = (VisRange){0, 0, 0, 0};
    if (!g || !out || g->nx <= 0 || g->ny <= 0 || g->nz <= 0) return;
    int nx = g->nx;
    size_t ncells = (size_t)nx * (size_t)g->ny * (size_t)g->nz;
    bool need_u = field != VIS_PRESSURE;
    if ((int)field < 0 || field >= VIS_FIELD_COUNT || (need_u && (!g->ux || !g->uy || !g->uz))) {
        memset(out, 0, ncells * sizeof(float));
        return;
    }
    int nt = pool ? pool_size(pool) : 1;
    FieldCtx c = {.g = g, .field = field, .out = out};
    c.scratch = malloc((size_t)nt * 9 * (size_t)nx * sizeof(float));
    c.stats = malloc((size_t)nt * sizeof(FieldStats));
    c.hist = range ? malloc((size_t)nt * HIST_BINS * sizeof(uint32_t)) : NULL;
    c.under = range ? malloc((size_t)nt * sizeof(uint64_t)) : NULL;
    uint8_t *zero_row = g->solid ? NULL : calloc((size_t)nx, 1);
    c.zero_row = zero_row;
    if (!c.scratch || !c.stats || (range && (!c.hist || !c.under)) || (!g->solid && !zero_row)) {
        LOGE("vis_compute_field: out of memory");
        memset(out, 0, ncells * sizeof(float));
        goto done;
    }
    for (int t = 0; t < nt; t++) c.stats[t] = (FieldStats){1e300, -1e300, 0};

    run_parallel(pool, g->nz, 1, field_slab, &c);

    if (range) {
        double mn = 1e300, mx = -1e300;
        uint64_t n = 0;
        for (int t = 0; t < nt; t++) {
            if (!c.stats[t].n) continue;
            mn = c.stats[t].min < mn ? c.stats[t].min : mn;
            mx = c.stats[t].max > mx ? c.stats[t].max : mx;
            n += c.stats[t].n;
        }
        if (n) compute_range(&c, pool, nt, mn, mx, n, range);
    }
done:
    free(c.scratch);
    free(c.stats);
    free(c.hist);
    free(c.under);
    free(zero_row);
}

/* Continuous index along one axis for trilinear interpolation between cell centres, clamped. */
static inline void lerp_axis(float x, int n, int *i0, int *i1, float *t) {
    float f = x - 0.5f, fmax = (float)(n - 1);
    f = f < 0.0f ? 0.0f : (f > fmax ? fmax : f);
    int i = (int)f;
    if (i > n - 2) i = n > 1 ? n - 2 : 0;
    *i0 = i;
    *i1 = n > 1 ? i + 1 : i;
    *t = f - (float)i;
}

static inline float trilerp(const float *a, size_t nx, size_t nxy, int i0, int i1, int j0, int j1, int k0, int k1,
                            float tx, float ty, float tz) {
    size_t r00 = nx * (size_t)j0 + nxy * (size_t)k0, r10 = nx * (size_t)j1 + nxy * (size_t)k0;
    size_t r01 = nx * (size_t)j0 + nxy * (size_t)k1, r11 = nx * (size_t)j1 + nxy * (size_t)k1;
    float c00 = a[r00 + i0] + tx * (a[r00 + i1] - a[r00 + i0]);
    float c10 = a[r10 + i0] + tx * (a[r10 + i1] - a[r10 + i0]);
    float c01 = a[r01 + i0] + tx * (a[r01 + i1] - a[r01 + i0]);
    float c11 = a[r11 + i0] + tx * (a[r11 + i1] - a[r11 + i0]);
    float c0 = c00 + ty * (c10 - c00), c1 = c01 + ty * (c11 - c01);
    return c0 + tz * (c1 - c0);
}

bool vis_sample_velocity(const VisGrid *g, float x, float y, float z, float u[3]) {
    u[0] = u[1] = u[2] = 0.0f;
    if (!g || !g->ux || !g->uy || !g->uz || g->nx <= 0 || g->ny <= 0 || g->nz <= 0) return false;
    if (!is_finite_f32(x) || !is_finite_f32(y) || !is_finite_f32(z)) return false;
    if (x < 0.0f || y < 0.0f || z < 0.0f || x > (float)g->nx || y > (float)g->ny || z > (float)g->nz) return false;
    size_t nx = (size_t)g->nx, nxy = nx * (size_t)g->ny;
    if (g->solid) {
        int ci = MINI((int)x, g->nx - 1), cj = MINI((int)y, g->ny - 1), ck = MINI((int)z, g->nz - 1);
        if (g->solid[(size_t)ci + nx * (size_t)cj + nxy * (size_t)ck]) return false;
    }
    int i0, i1, j0, j1, k0, k1;
    float tx, ty, tz;
    lerp_axis(x, g->nx, &i0, &i1, &tx);
    lerp_axis(y, g->ny, &j0, &j1, &ty);
    lerp_axis(z, g->nz, &k0, &k1, &tz);
    u[0] = trilerp(g->ux, nx, nxy, i0, i1, j0, j1, k0, k1, tx, ty, tz);
    u[1] = trilerp(g->uy, nx, nxy, i0, i1, j0, j1, k0, k1, tx, ty, tz);
    u[2] = trilerp(g->uz, nx, nxy, i0, i1, j0, j1, k0, k1, tx, ty, tz);
    return true;
}

float vis_sample_scalar(const float *field, int nx, int ny, int nz, float x, float y, float z) {
    if (!field || nx <= 0 || ny <= 0 || nz <= 0) return 0.0f;
    if (!is_finite_f32(x) || !is_finite_f32(y) || !is_finite_f32(z)) return 0.0f;
    int i0, i1, j0, j1, k0, k1;
    float tx, ty, tz;
    lerp_axis(x, nx, &i0, &i1, &tx);
    lerp_axis(y, ny, &j0, &j1, &ty);
    lerp_axis(z, nz, &k0, &k1, &tz);
    return trilerp(field, (size_t)nx, (size_t)nx * (size_t)ny, i0, i1, j0, j1, k0, k1, tx, ty, tz);
}

typedef struct {
    const float *src;
    float *dst;
    int nx, ny, nz, axis;
} SmoothCtx;

static void smooth_slab(void *vctx, int k0, int k1, int tid) {
    (void)tid;
    const SmoothCtx *c = vctx;
    const int nx = c->nx, ny = c->ny, nz = c->nz;
    const ptrdiff_t sxy = (ptrdiff_t)nx * ny;
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < ny; j++) {
            const ptrdiff_t b = (ptrdiff_t)nx * j + sxy * k;
            const float *s = c->src + b;
            float *d = c->dst + b;
            if (c->axis == 0) {
                d[0] = 0.25f * (3.0f * s[0] + s[nx > 1 ? 1 : 0]);
                for (int i = 1; i < nx - 1; i++) d[i] = 0.25f * (s[i - 1] + 2.0f * s[i] + s[i + 1]);
                if (nx > 1) d[nx - 1] = 0.25f * (3.0f * s[nx - 1] + s[nx - 2]);
            } else {
                const ptrdiff_t st = c->axis == 1 ? nx : sxy;
                const int q = c->axis == 1 ? j : k, nq = c->axis == 1 ? ny : nz;
                const float *m = q > 0 ? s - st : s, *p = q < nq - 1 ? s + st : s;
                for (int i = 0; i < nx; i++) d[i] = 0.25f * (m[i] + 2.0f * s[i] + p[i]);
            }
        }
}

void vis_smooth3(float *field, float *out, int nx, int ny, int nz, ThreadPool *pool) {
    SmoothCtx c = {field, out, nx, ny, nz, 0};
    run_parallel(pool, nz, 1, smooth_slab, &c);
    c.src = out, c.dst = field, c.axis = 1;
    run_parallel(pool, nz, 1, smooth_slab, &c);
    c.src = field, c.dst = out, c.axis = 2;
    run_parallel(pool, nz, 1, smooth_slab, &c);
}
