/* vis_iso.c - isosurfaces by naive surface nets
 *
 * Samples sit at cell centres (or 2x2x2 block centres). Parallel over z-layers of cubes in two phases:
 *   1. per layer: one vertex per cube with a sign change; a dense per-cube map stores its layer-local index.
 *   2. once all layers are done and global vertex offsets are known: per layer, one quad per sign-changing
 *      grid edge at the cube's min corner. x/y edges reference the previous layer, whose map is complete.
 * Per-layer vertex/index buffers are then concatenated in layer order.
 */
#include "vis.h"
#include "common.h"

typedef struct {
    float *v;      /* 7 floats per vertex */
    size_t n, cap; /* vertices */
} FBuf;

typedef struct {
    uint32_t *v;
    size_t n, cap;
} IBuf;

typedef struct {
    const VisGrid *g;
    const float *field, *color;
    float iso;
    int ds;               /* downsample factor (1 or 2) */
    int sx, sy, sz;       /* sample grid */
    size_t ssx, ssxy;     /* sample strides */
    const float *S;       /* classification samples */
    const float *C;       /* colour samples on the sample grid */
    float *Sbuf, *Cbuf;   /* owned sample buffers (downsampled and/or solid-clamped) */
    const uint8_t *near;  /* full-res mask: within one cell of a solid */
    const uint8_t *dsrc;  /* dilation pass input/output */
    uint8_t *ddst;
    int dpass;
    int cx, cy, cz;       /* cube grid */
    size_t cxy;
    int32_t *map;         /* per cube: layer-local vertex index, -1 = none */
    FBuf *lv;             /* per cube layer */
    IBuf *li;
    size_t *voff, *ioff;  /* per cube layer: global offsets (cz + 1 entries) */
    size_t *ndegen;       /* per cube layer: vertices with a degenerate gradient */
    bool *oom;            /* per cube layer */
    IsoMesh *out;
} IsoCtx;

static void run_parallel(ThreadPool *pool, int count, int grain, ParallelFn fn, void *ctx) {
    if (pool) pool_for(pool, count, grain, fn, ctx);
    else fn(ctx, 0, count, 0);
}

/* One pass of a separable 3x3x3 dilation of a full-resolution byte mask (pass 0/1/2 = x/y/z). */
static void dilate_slab(void *vctx, int k0, int k1, int tid) {
    (void)tid;
    IsoCtx *x = vctx;
    int nx = x->g->nx, ny = x->g->ny, nz = x->g->nz;
    ptrdiff_t sxy = (ptrdiff_t)nx * ny;
    for (int k = k0; k < k1; k++) {
        for (int j = 0; j < ny; j++) {
            ptrdiff_t b = (ptrdiff_t)nx * j + sxy * k;
            const uint8_t *r = x->dsrc + b;
            uint8_t *o = x->ddst + b;
            if (x->dpass == 0) {
                if (nx == 1) {
                    o[0] = r[0];
                    continue;
                }
                o[0] = r[0] | r[1];
                for (int i = 1; i < nx - 1; i++) o[i] = r[i - 1] | r[i] | r[i + 1];
                o[nx - 1] = r[nx - 2] | r[nx - 1];
            } else {
                ptrdiff_t st = x->dpass == 1 ? nx : sxy;
                int q = x->dpass == 1 ? j : k, nq = x->dpass == 1 ? ny : nz;
                const uint8_t *m = q > 0 ? r - st : r, *p = q < nq - 1 ? r + st : r;
                for (int i = 0; i < nx; i++) o[i] = m[i] | r[i] | p[i];
            }
        }
    }
}

/* Builds the sample layers [k0,k1): 2x2x2 averages (ds 2) and/or clamping near solids to <= iso. */
static void prep_slab(void *vctx, int k0, int k1, int tid) {
    (void)tid;
    IsoCtx *x = vctx;
    size_t nx = (size_t)x->g->nx, sxy = nx * (size_t)x->g->ny;
    const float *f = x->field, *col = x->color ? x->color : x->field, iso = x->iso;
    const uint8_t *nr = x->near;
    for (int k = k0; k < k1; k++) {
        for (int j = 0; j < x->sy; j++) {
            if (x->ds == 1) {
                size_t b = nx * (size_t)j + sxy * (size_t)k;
                float *o = x->Sbuf + b;
                for (size_t i = 0; i < nx; i++) {
                    float v = f[b + i];
                    o[i] = nr[b + i] && v > iso ? iso : v;
                }
                continue;
            }
            for (int i = 0; i < x->sx; i++) {
                size_t c0 = 2 * (size_t)i + nx * 2 * (size_t)j + sxy * 2 * (size_t)k;
                size_t c[8] = {c0, c0 + 1, c0 + nx, c0 + nx + 1, c0 + sxy, c0 + sxy + 1, c0 + sxy + nx, c0 + sxy + nx + 1};
                float s = 0.0f, cc = 0.0f;
                uint8_t near = 0;
                for (int q = 0; q < 8; q++) {
                    s += f[c[q]];
                    cc += col[c[q]];
                    if (nr) near |= nr[c[q]];
                }
                s *= 0.125f;
                size_t o = (size_t)i + x->ssx * (size_t)j + x->ssxy * (size_t)k;
                x->Sbuf[o] = near && s > iso ? iso : s;
                x->Cbuf[o] = cc * 0.125f;
            }
        }
    }
}

static inline float S_at(const IsoCtx *x, int i, int j, int k) {
    return x->S[(size_t)i + x->ssx * (size_t)j + x->ssxy * (size_t)k];
}

/* Central-difference gradient of the samples at sample (i,j,k), one-sided at the borders. */
static inline void sample_grad(const IsoCtx *x, int i, int j, int k, float gr[3]) {
    int im = i > 0 ? i - 1 : i, ip = i < x->sx - 1 ? i + 1 : i;
    int jm = j > 0 ? j - 1 : j, jp = j < x->sy - 1 ? j + 1 : j;
    int km = k > 0 ? k - 1 : k, kp = k < x->sz - 1 ? k + 1 : k;
    gr[0] = (S_at(x, ip, j, k) - S_at(x, im, j, k)) / (float)(ip - im);
    gr[1] = (S_at(x, i, jp, k) - S_at(x, i, jm, k)) / (float)(jp - jm);
    gr[2] = (S_at(x, i, j, kp) - S_at(x, i, j, km)) / (float)(kp - km);
}

static bool fbuf_push(FBuf *b, const float v[7]) {
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        float *nv = realloc(b->v, cap * 7 * sizeof(float));
        if (!nv) return false;
        b->v = nv;
        b->cap = cap;
    }
    memcpy(b->v + b->n * 7, v, 7 * sizeof(float));
    b->n++;
    return true;
}

/* Corner q of a cube has offset (q & 1, q >> 1 & 1, q >> 2 & 1); edges listed per axis (x, y, z). */
static const uint8_t EDGES[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

static bool make_vertex(const IsoCtx *x, int a, int b, int c, const float f[8], unsigned m, FBuf *vb,
                        size_t *ndeg) {
    float sum[3] = {0.0f, 0.0f, 0.0f};
    int cnt = 0;
    for (int e = 0; e < 12; e++) {
        unsigned q0 = EDGES[e][0], q1 = EDGES[e][1];
        if (!(((m >> q0) ^ (m >> q1)) & 1u)) continue;
        float t = (x->iso - f[q0]) / (f[q1] - f[q0]);
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        sum[0] += (float)(q0 & 1u);
        sum[1] += (float)((q0 >> 1) & 1u);
        sum[2] += (float)((q0 >> 2) & 1u);
        sum[e / 4] += t;
        cnt++;
    }
    float l[3];
    for (int d = 0; d < 3; d++) {
        l[d] = sum[d] / (float)cnt;
        if (!is_finite_f32(l[d])) l[d] = 0.5f;
        l[d] = CLAMP(l[d], 0.0f, 1.0f);
    }
    /* normal: -gradient, trilinearly interpolated from the corner gradients */
    float gs[3] = {0.0f, 0.0f, 0.0f}, col = 0.0f;
    const float *C = x->C;
    for (int q = 0; q < 8; q++) {
        int di = q & 1, dj = (q >> 1) & 1, dk = (q >> 2) & 1;
        float wq = (di ? l[0] : 1.0f - l[0]) * (dj ? l[1] : 1.0f - l[1]) * (dk ? l[2] : 1.0f - l[2]), gq[3];
        sample_grad(x, a + di, b + dj, c + dk, gq);
        gs[0] += wq * gq[0];
        gs[1] += wq * gq[1];
        gs[2] += wq * gq[2];
        col += wq * C[(size_t)(a + di) + x->ssx * (size_t)(b + dj) + x->ssxy * (size_t)(c + dk)];
    }
    float v[7], ds = (float)x->ds;
    v[0] = ds * ((float)a + l[0] + 0.5f);
    v[1] = ds * ((float)b + l[1] + 0.5f);
    v[2] = ds * ((float)c + l[2] + 0.5f);
    float mx = MAXI(fabsf(gs[0]), MAXI(fabsf(gs[1]), fabsf(gs[2])));
    v[3] = v[4] = v[5] = 0.0f;
    if (mx > 0.0f && is_finite_f32(mx)) {
        float g0 = gs[0] / mx, g1 = gs[1] / mx, g2 = gs[2] / mx, s = -1.0f / sqrtf(g0 * g0 + g1 * g1 + g2 * g2);
        v[3] = g0 * s, v[4] = g1 * s, v[5] = g2 * s;
    }
    if (!is_finite_f32(v[3]) || !is_finite_f32(v[4]) || !is_finite_f32(v[5]) ||
        (v[3] == 0.0f && v[4] == 0.0f && v[5] == 0.0f)) {
        v[3] = v[4] = v[5] = 0.0f;
        (*ndeg)++; /* zero normal: replaced by the adjacent face normals after the merge */
    }
    v[6] = is_finite_f32(col) ? col : 0.0f;
    return fbuf_push(vb, v);
}

static void verts_slab(void *vctx, int c0, int c1, int tid) {
    (void)tid;
    IsoCtx *x = vctx;
    const float *S = x->S, iso = x->iso;
    int cx = x->cx, cy = x->cy;
    for (int c = c0; c < c1; c++) {
        FBuf *vb = &x->lv[c];
        vb->n = 0;
        size_t ndeg = 0;
        for (int b = 0; b < cy; b++) {
            const float *r00 = S + x->ssx * (size_t)b + x->ssxy * (size_t)c, *r10 = r00 + x->ssx;
            const float *r01 = r00 + x->ssxy, *r11 = r01 + x->ssx;
            int32_t *mrow = x->map + x->cxy * (size_t)c + (size_t)cx * (size_t)b;
            for (int a = 0; a < cx; a++) { /* quick classification: corners on both sides? */
                float lo = r00[a], hi = r00[a], w;
                w = r00[a + 1], lo = w < lo ? w : lo, hi = w > hi ? w : hi;
                w = r10[a], lo = w < lo ? w : lo, hi = w > hi ? w : hi;
                w = r10[a + 1], lo = w < lo ? w : lo, hi = w > hi ? w : hi;
                w = r01[a], lo = w < lo ? w : lo, hi = w > hi ? w : hi;
                w = r01[a + 1], lo = w < lo ? w : lo, hi = w > hi ? w : hi;
                w = r11[a], lo = w < lo ? w : lo, hi = w > hi ? w : hi;
                w = r11[a + 1], lo = w < lo ? w : lo, hi = w > hi ? w : hi;
                mrow[a] = ((hi > iso) & (lo <= iso)) ? 0 : -1;
            }
            for (int a = 0; a < cx; a++) {
                if (mrow[a] < 0) continue;
                float f[8] = {r00[a], r00[a + 1], r10[a], r10[a + 1], r01[a], r01[a + 1], r11[a], r11[a + 1]};
                unsigned m = 0;
                for (int q = 0; q < 8; q++) m |= (unsigned)(f[q] > iso) << q;
                mrow[a] = -1;
                if (m == 0 || m == 255) continue;
                if (!make_vertex(x, a, b, c, f, m, vb, &ndeg)) {
                    x->oom[c] = true;
                    continue;
                }
                mrow[a] = (int32_t)(vb->n - 1);
            }
        }
        x->ndegen[c] = ndeg;
    }
}

static inline void emit_quad(IBuf *ib, bool *oom, uint32_t q0, uint32_t q1, uint32_t q2, uint32_t q3, bool fwd) {
    if (ib->n + 6 > ib->cap) {
        size_t cap = ib->cap ? ib->cap * 2 : 8192;
        uint32_t *nv = realloc(ib->v, cap * sizeof(uint32_t));
        if (!nv) {
            *oom = true;
            return;
        }
        ib->v = nv;
        ib->cap = cap;
    }
    uint32_t *d = ib->v + ib->n;
    if (fwd) d[0] = q0, d[1] = q1, d[2] = q2, d[3] = q0, d[4] = q2, d[5] = q3;
    else d[0] = q0, d[1] = q2, d[2] = q1, d[3] = q0, d[4] = q3, d[5] = q2;
    ib->n += 6;
}

/* Quads for the sign-changing edges at each cube's min corner. The vertex order (q0..q3) winds
 * counter-clockwise around +axis; it is kept when the field decreases along +axis (normal = +axis). */
static void quads_slab(void *vctx, int c0, int c1, int tid) {
    (void)tid;
    IsoCtx *x = vctx;
    const float iso = x->iso;
    int cx = x->cx, cy = x->cy;
    for (int c = c0; c < c1; c++) {
        IBuf *ib = &x->li[c];
        ib->n = 0;
        const int32_t *m0 = x->map + x->cxy * (size_t)c, *mz = c > 0 ? m0 - x->cxy : m0;
        uint32_t o0 = (uint32_t)x->voff[c], oz = c > 0 ? (uint32_t)x->voff[c - 1] : 0;
        for (int b = 0; b < cy; b++) {
            const float *s0 = x->S + x->ssx * (size_t)b + x->ssxy * (size_t)c;
            const int32_t *r0 = m0 + (size_t)cx * (size_t)b, *rm = b > 0 ? r0 - cx : r0; /* rows b and b-1 */
            const int32_t *z0 = mz + (size_t)cx * (size_t)b, *zm = b > 0 ? z0 - cx : z0;
            for (int a = 0; a < cx; a++) {
                int32_t v = r0[a];
                if (v < 0) continue;
                bool in0 = s0[a] > iso;
                if (b > 0 && c > 0 && (s0[a + 1] > iso) != in0) { /* x edge: cubes (a, b-1..b, c-1..c) */
                    int32_t q0 = zm[a], q1 = z0[a], q3 = rm[a];
                    if ((q0 | q1 | q3) >= 0)
                        emit_quad(ib, &x->oom[c], oz + q0, oz + q1, o0 + v, o0 + q3, in0);
                }
                if (a > 0 && c > 0 && (s0[a + x->ssx] > iso) != in0) { /* y edge: cubes (a-1..a, b, c-1..c) */
                    int32_t q0 = z0[a - 1], q1 = r0[a - 1], q3 = z0[a];
                    if ((q0 | q1 | q3) >= 0)
                        emit_quad(ib, &x->oom[c], oz + q0, o0 + q1, o0 + v, oz + q3, in0);
                }
                if (a > 0 && b > 0 && (s0[a + x->ssxy] > iso) != in0) { /* z edge: cubes (a-1..a, b-1..b, c) */
                    int32_t q0 = rm[a - 1], q1 = rm[a], q3 = r0[a - 1];
                    if ((q0 | q1 | q3) >= 0)
                        emit_quad(ib, &x->oom[c], o0 + q0, o0 + q1, o0 + v, o0 + q3, in0);
                }
            }
        }
    }
}

static void copy_slab(void *vctx, int c0, int c1, int tid) {
    (void)tid;
    IsoCtx *x = vctx;
    for (int c = c0; c < c1; c++) {
        if (x->lv[c].n) memcpy(x->out->verts + x->voff[c] * 7, x->lv[c].v, x->lv[c].n * 7 * sizeof(float));
        if (x->li[c].n) memcpy(x->out->indices + x->ioff[c], x->li[c].v, x->li[c].n * sizeof(uint32_t));
    }
}

/* Vertices whose gradient vanished get the normalised sum of their adjacent face normals. */
static void fix_degenerate_normals(IsoMesh *m) {
    uint8_t *mark = calloc(m->nverts ? m->nverts : 1, 1);
    if (!mark) return;
    for (uint32_t i = 0; i < m->nverts; i++) {
        const float *n = m->verts + (size_t)i * 7 + 3;
        mark[i] = n[0] == 0.0f && n[1] == 0.0f && n[2] == 0.0f;
    }
    for (uint32_t t = 0; t + 2 < m->nindices; t += 3) {
        uint32_t id[3] = {m->indices[t], m->indices[t + 1], m->indices[t + 2]};
        if (!(mark[id[0]] | mark[id[1]] | mark[id[2]])) continue;
        const float *p0 = m->verts + (size_t)id[0] * 7, *p1 = m->verts + (size_t)id[1] * 7, *p2 = m->verts + (size_t)id[2] * 7;
        float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        float fn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (int q = 0; q < 3; q++) {
            if (!mark[id[q]]) continue;
            float *n = m->verts + (size_t)id[q] * 7 + 3;
            n[0] += fn[0], n[1] += fn[1], n[2] += fn[2];
        }
    }
    for (uint32_t i = 0; i < m->nverts; i++) {
        if (!mark[i]) continue;
        float *n = m->verts + (size_t)i * 7 + 3, l2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        if (l2 > 0.0f && is_finite_f32(l2)) {
            float s = 1.0f / sqrtf(l2);
            n[0] *= s, n[1] *= s, n[2] *= s;
        } else {
            n[0] = 0.0f, n[1] = 0.0f, n[2] = 1.0f;
        }
    }
    free(mark);
}

void vis_isosurface(const VisGrid *g, const float *field, const float *color_field, const IsoParams *ip,
                    IsoMesh *out, ThreadPool *pool) {
    if (!out) return;
    out->nverts = 0;
    out->nindices = 0;
    if (!g || !field || !ip || g->nx <= 0 || g->ny <= 0 || g->nz <= 0 || !is_finite_f32(ip->iso)) return;
    IsoCtx x = {.g = g, .field = field, .color = color_field, .iso = ip->iso, .out = out};
    x.ds = ip->downsample >= 2 ? 2 : 1;
    x.sx = g->nx / x.ds, x.sy = g->ny / x.ds, x.sz = g->nz / x.ds;
    if (x.sx < 2 || x.sy < 2 || x.sz < 2) return;
    x.ssx = (size_t)x.sx;
    x.ssxy = x.ssx * (size_t)x.sy;
    x.cx = x.sx - 1, x.cy = x.sy - 1, x.cz = x.sz - 1;
    x.cxy = (size_t)x.cx * (size_t)x.cy;
    size_t ncells = (size_t)g->nx * (size_t)g->ny * (size_t)g->nz, nsamples = x.ssxy * (size_t)x.sz;
    bool exclude = ip->exclude_near_solid && g->solid;

    x.map = malloc(x.cxy * (size_t)x.cz * sizeof(int32_t));
    x.lv = calloc((size_t)x.cz, sizeof(FBuf));
    x.li = calloc((size_t)x.cz, sizeof(IBuf));
    x.voff = calloc((size_t)x.cz + 1, sizeof(size_t));
    x.ioff = calloc((size_t)x.cz + 1, sizeof(size_t));
    x.ndegen = calloc((size_t)x.cz, sizeof(size_t));
    x.oom = calloc((size_t)x.cz, sizeof(bool));
    bool ok = x.map && x.lv && x.li && x.voff && x.ioff && x.ndegen && x.oom;
    uint8_t *mask = NULL;
    if (ok && exclude) { /* dilate the solid mask by one cell (3x3x3) */
        mask = malloc(ncells);
        uint8_t *tmp = malloc(ncells);
        ok = mask && tmp;
        if (ok) {
            x.dpass = 0, x.dsrc = g->solid, x.ddst = mask;
            run_parallel(pool, g->nz, 2, dilate_slab, &x);
            x.dpass = 1, x.dsrc = mask, x.ddst = tmp;
            run_parallel(pool, g->nz, 2, dilate_slab, &x);
            x.dpass = 2, x.dsrc = tmp, x.ddst = mask;
            run_parallel(pool, g->nz, 2, dilate_slab, &x);
            x.near = mask;
        }
        free(tmp);
    }
    if (ok && (x.ds == 2 || exclude)) {
        x.Sbuf = malloc(nsamples * sizeof(float));
        x.Cbuf = x.ds == 2 ? malloc(nsamples * sizeof(float)) : NULL;
        ok = x.Sbuf && (x.ds == 1 || x.Cbuf);
        if (ok) run_parallel(pool, x.sz, 1, prep_slab, &x);
    }
    free(mask);
    x.near = NULL;
    if (!ok) {
        LOGE("vis_isosurface: out of memory");
        goto done;
    }
    x.S = x.Sbuf ? x.Sbuf : field;
    x.C = x.Cbuf ? x.Cbuf : (color_field ? color_field : field);

    run_parallel(pool, x.cz, 1, verts_slab, &x);
    size_t ndeg = 0;
    for (int c = 0; c < x.cz; c++) {
        x.voff[c + 1] = x.voff[c] + x.lv[c].n;
        ndeg += x.ndegen[c];
    }
    size_t nv = x.voff[x.cz];
    if (nv >= UINT32_MAX / 2) {
        LOGE("vis_isosurface: too many vertices (%zu)", nv);
        goto done;
    }
    run_parallel(pool, x.cz, 1, quads_slab, &x);
    bool oom = false;
    for (int c = 0; c < x.cz; c++) {
        x.ioff[c + 1] = x.ioff[c] + x.li[c].n;
        oom |= x.oom[c];
    }
    size_t ni = x.ioff[x.cz];
    if (oom) LOGW("vis_isosurface: out of memory, mesh incomplete");
    if (ni >= UINT32_MAX / 2) {
        LOGE("vis_isosurface: too many triangles (%zu)", ni / 3);
        goto done;
    }
    if (nv == 0 || ni == 0) goto done;
    if (nv > out->cap_verts) {
        size_t cap = nv + nv / 4;
        float *v = realloc(out->verts, cap * 7 * sizeof(float));
        if (!v) {
            LOGE("vis_isosurface: out of memory (%zu vertices)", nv);
            goto done;
        }
        out->verts = v;
        out->cap_verts = (uint32_t)cap;
    }
    if (ni > out->cap_indices) {
        size_t cap = ni + ni / 4;
        uint32_t *v = realloc(out->indices, cap * sizeof(uint32_t));
        if (!v) {
            LOGE("vis_isosurface: out of memory (%zu indices)", ni);
            goto done;
        }
        out->indices = v;
        out->cap_indices = (uint32_t)cap;
    }
    run_parallel(pool, x.cz, 2, copy_slab, &x);
    out->nverts = (uint32_t)nv;
    out->nindices = (uint32_t)ni;
    if (ndeg) fix_degenerate_normals(out);
done:
    for (int c = 0; c < x.cz; c++) {
        if (x.lv) free(x.lv[c].v);
        if (x.li) free(x.li[c].v);
    }
    free(x.map);
    free(x.lv);
    free(x.li);
    free(x.voff);
    free(x.ioff);
    free(x.ndegen);
    free(x.oom);
    free(x.Sbuf);
    free(x.Cbuf);
}

void isomesh_free(IsoMesh *m) {
    if (!m) return;
    free(m->verts);
    free(m->indices);
    memset(m, 0, sizeof *m);
}
