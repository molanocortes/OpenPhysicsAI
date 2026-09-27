/* vis_stream.c - streamlines through a frozen velocity field (RK2 midpoint, fixed arc-length step) */
#include "vis.h"
#include "common.h"

enum { SAMPLE_OK = 0, SAMPLE_OUTSIDE, SAMPLE_BLOCKED };

typedef struct {
    float *v;      /* 5 floats per vertex */
    size_t n, cap; /* vertices */
    bool oom;
} VertBuf;

typedef struct {
    size_t b; /* index of the (i0,j0,k0) corner */
    float tx, ty, tz;
} Trilin;

typedef struct {
    const VisGrid *g;
    const float *scalar;
    const float *seeds;
    StreamParams sp;
    float min_speed2;
    int nx, ny, nz;
    size_t sx, sxy;
    float ex, ey, ez;     /* domain extents */
    float cmx, cmy, cmz;  /* max continuous cell index (n - 1) */
    int imx, imy, imz;    /* max lower corner index (n - 2, or 0) */
    size_t stx, sty, stz; /* strides to the +1 corner (0 on axes of extent 1) */
    VertBuf *bufs;        /* per thread */
    int *seed_tid;
    size_t *seed_off, *seed_cnt, *dst;
    StreamlineSet *out;
} StreamCtx;

static void run_parallel(ThreadPool *pool, int count, int grain, ParallelFn fn, void *ctx) {
    if (pool) pool_for(pool, count, grain, fn, ctx);
    else fn(ctx, 0, count, 0);
}

static inline void trilin_weights(const StreamCtx *c, float x, float y, float z, Trilin *w) {
    float fx = x - 0.5f, fy = y - 0.5f, fz = z - 0.5f;
    fx = fx < 0.0f ? 0.0f : (fx > c->cmx ? c->cmx : fx);
    fy = fy < 0.0f ? 0.0f : (fy > c->cmy ? c->cmy : fy);
    fz = fz < 0.0f ? 0.0f : (fz > c->cmz ? c->cmz : fz);
    int i = (int)fx, j = (int)fy, k = (int)fz;
    i = i > c->imx ? c->imx : i;
    j = j > c->imy ? c->imy : j;
    k = k > c->imz ? c->imz : k;
    w->b = (size_t)i + c->sx * (size_t)j + c->sxy * (size_t)k;
    w->tx = fx - (float)i;
    w->ty = fy - (float)j;
    w->tz = fz - (float)k;
}

static inline float trilin_eval(const StreamCtx *c, const float *a, const Trilin *w) {
    size_t b = w->b, dx = c->stx, dy = c->sty, dz = c->stz;
    float c00 = a[b] + w->tx * (a[b + dx] - a[b]);
    float c10 = a[b + dy] + w->tx * (a[b + dy + dx] - a[b + dy]);
    float c01 = a[b + dz] + w->tx * (a[b + dz + dx] - a[b + dz]);
    float c11 = a[b + dy + dz] + w->tx * (a[b + dy + dz + dx] - a[b + dy + dz]);
    float c0 = c00 + w->ty * (c10 - c00), c1 = c01 + w->ty * (c11 - c01);
    return c0 + w->tz * (c1 - c0);
}

static inline int sample_u(const StreamCtx *c, const float p[3], float u[3], Trilin *w) {
    if (!(p[0] >= 0.0f && p[1] >= 0.0f && p[2] >= 0.0f && p[0] <= c->ex && p[1] <= c->ey && p[2] <= c->ez))
        return SAMPLE_OUTSIDE;
    if (c->g->solid) {
        int i = (int)p[0], j = (int)p[1], k = (int)p[2];
        i = i < c->nx ? i : c->nx - 1;
        j = j < c->ny ? j : c->ny - 1;
        k = k < c->nz ? k : c->nz - 1;
        if (c->g->solid[(size_t)i + c->sx * (size_t)j + c->sxy * (size_t)k]) return SAMPLE_BLOCKED;
    }
    trilin_weights(c, p[0], p[1], p[2], w);
    u[0] = trilin_eval(c, c->g->ux, w);
    u[1] = trilin_eval(c, c->g->uy, w);
    u[2] = trilin_eval(c, c->g->uz, w);
    return SAMPLE_OK;
}

static bool vb_grow(VertBuf *vb) {
    size_t cap = vb->cap ? vb->cap * 2 : 16384;
    float *v = realloc(vb->v, cap * 5 * sizeof(float));
    if (!v) {
        vb->oom = true;
        return false;
    }
    vb->v = v;
    vb->cap = cap;
    return true;
}

static inline bool vb_push(VertBuf *vb, const float p[3], float s, float t) {
    if (vb->n == vb->cap && !vb_grow(vb)) return false;
    float *v = vb->v + vb->n * 5;
    v[0] = p[0], v[1] = p[1], v[2] = p[2], v[3] = s, v[4] = t;
    vb->n++;
    return true;
}

/* Last partial step: from p (inside) along unit direction d up to the domain face, at most len. */
static void clip_to_domain(const StreamCtx *c, VertBuf *vb, const float p[3], const float d[3], float len,
                           float speed, float t, float dir) {
    float ext[3] = {c->ex, c->ey, c->ez}, s = len;
    for (int a = 0; a < 3; a++) {
        if (d[a] > 1e-6f) s = MINI(s, (ext[a] - p[a]) / d[a]);
        else if (d[a] < -1e-6f) s = MINI(s, -p[a] / d[a]);
    }
    if (!(s > 1e-3f)) return;
    float q[3];
    for (int a = 0; a < 3; a++) q[a] = CLAMP(p[a] + s * d[a], 0.0f, ext[a]);
    Trilin w;
    trilin_weights(c, q[0], q[1], q[2], &w);
    float sc;
    if (c->scalar) {
        sc = trilin_eval(c, c->scalar, &w);
    } else {
        float ux = trilin_eval(c, c->g->ux, &w), uy = trilin_eval(c, c->g->uy, &w), uz = trilin_eval(c, c->g->uz, &w);
        sc = sqrtf(ux * ux + uy * uy + uz * uz);
    }
    vb_push(vb, q, sc, dir * (t + s / speed));
}

/* Integrate from p0 (velocity u0, |u0|^2 = sp2, already stored as a vertex) along dir = +1/-1 and append
 * the new vertices with flight time dir * t. */
static void integrate(const StreamCtx *c, VertBuf *vb, const float p0[3], const float u0[3], float sp2, float dir) {
    const float h = c->sp.step;
    float p[3] = {p0[0], p0[1], p0[2]}, u[3] = {u0[0], u0[1], u0[2]}, t = 0.0f;
    Trilin w;
    for (int n = 0; n < c->sp.max_steps; n++) {
        float sp = sqrtf(sp2), inv = dir / sp;
        float d1[3] = {u[0] * inv, u[1] * inv, u[2] * inv};
        float pm[3] = {p[0] + 0.5f * h * d1[0], p[1] + 0.5f * h * d1[1], p[2] + 0.5f * h * d1[2]}, um[3];
        int r = sample_u(c, pm, um, &w);
        if (r != SAMPLE_OK) {
            if (r == SAMPLE_OUTSIDE) clip_to_domain(c, vb, p, d1, 0.5f * h, sp, t, dir);
            return;
        }
        float spm2 = um[0] * um[0] + um[1] * um[1] + um[2] * um[2];
        if (!is_finite_f32(spm2) || spm2 < c->min_speed2) return;
        float spm = sqrtf(spm2), invm = dir / spm;
        float d2[3] = {um[0] * invm, um[1] * invm, um[2] * invm};
        float pn[3] = {p[0] + h * d2[0], p[1] + h * d2[1], p[2] + h * d2[2]}, un[3];
        r = sample_u(c, pn, un, &w);
        if (r != SAMPLE_OK) {
            if (r == SAMPLE_OUTSIDE) clip_to_domain(c, vb, p, d2, h, spm, t, dir);
            return;
        }
        float spn2 = un[0] * un[0] + un[1] * un[1] + un[2] * un[2];
        if (!is_finite_f32(spn2)) return;
        t += h / spm;
        float sc = c->scalar ? trilin_eval(c, c->scalar, &w) : sqrtf(spn2);
        if (!vb_push(vb, pn, sc, dir * t)) return;
        if (spn2 < c->min_speed2) return;
        memcpy(p, pn, sizeof p);
        memcpy(u, un, sizeof u);
        sp2 = spn2;
    }
}

static void trace_seed(StreamCtx *c, int s, int tid) {
    VertBuf *vb = &c->bufs[tid];
    const float *sd = c->seeds + 3 * (size_t)s;
    c->seed_cnt[s] = 0;
    if (!is_finite_f32(sd[0]) || !is_finite_f32(sd[1]) || !is_finite_f32(sd[2])) return;
    float p[3] = {sd[0], sd[1], sd[2]}, u[3];
    Trilin w;
    if (sample_u(c, p, u, &w) != SAMPLE_OK) return;
    float sp2 = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
    if (!is_finite_f32(sp2) || sp2 < c->min_speed2) return;
    size_t start = vb->n;
    if (!vb_push(vb, p, c->scalar ? trilin_eval(c, c->scalar, &w) : sqrtf(sp2), 0.0f)) return;
    if (c->sp.both_directions) {
        integrate(c, vb, p, u, sp2, -1.0f);
        for (size_t a = start, b = vb->n - 1; a < b; a++, b--) { /* upstream part: reverse in place */
            float tmp[5];
            memcpy(tmp, vb->v + a * 5, sizeof tmp);
            memcpy(vb->v + a * 5, vb->v + b * 5, sizeof tmp);
            memcpy(vb->v + b * 5, tmp, sizeof tmp);
        }
    }
    integrate(c, vb, p, u, sp2, 1.0f);
    size_t cnt = vb->n - start;
    if (cnt < 2) {
        vb->n = start;
        return;
    }
    float t0 = vb->v[start * 5 + 4]; /* <= 0: shift so the first vertex has flight time 0 */
    if (t0 != 0.0f)
        for (size_t i = start; i < vb->n; i++) vb->v[i * 5 + 4] -= t0;
    c->seed_tid[s] = tid;
    c->seed_off[s] = start;
    c->seed_cnt[s] = cnt;
}

static void trace_range(void *vctx, int b, int e, int tid) {
    for (int s = b; s < e; s++) trace_seed((StreamCtx *)vctx, s, tid);
}

static void copy_range(void *vctx, int b, int e, int tid) {
    (void)tid;
    StreamCtx *c = vctx;
    for (int s = b; s < e; s++) {
        if (c->seed_cnt[s] < 2) continue;
        memcpy(c->out->verts + c->dst[s] * 5, c->bufs[c->seed_tid[s]].v + c->seed_off[s] * 5,
               c->seed_cnt[s] * 5 * sizeof(float));
    }
}

void vis_streamlines(const VisGrid *g, const float *scalar, const float *seeds, int nseeds, const StreamParams *sp,
                     StreamlineSet *out, ThreadPool *pool) {
    if (!out) return;
    out->nverts = 0;
    out->nlines = 0;
    if (!g || !sp || !seeds || nseeds <= 0 || !g->ux || !g->uy || !g->uz || g->nx <= 0 || g->ny <= 0 || g->nz <= 0 ||
        sp->max_steps <= 0 || !is_finite_f32(sp->step) || !(sp->step > 0.0f))
        return;
    int nt = pool ? pool_size(pool) : 1;
    StreamCtx c = {.g = g, .scalar = scalar, .seeds = seeds, .sp = *sp, .out = out};
    float ms = is_finite_f32(sp->min_speed) ? sp->min_speed : 0.0f;
    c.min_speed2 = MAXI(ms * ms, 1e-20f);
    c.nx = g->nx, c.ny = g->ny, c.nz = g->nz;
    c.sx = (size_t)g->nx;
    c.sxy = c.sx * (size_t)g->ny;
    c.ex = (float)g->nx, c.ey = (float)g->ny, c.ez = (float)g->nz;
    c.cmx = (float)(g->nx - 1), c.cmy = (float)(g->ny - 1), c.cmz = (float)(g->nz - 1);
    c.imx = MAXI(g->nx - 2, 0), c.imy = MAXI(g->ny - 2, 0), c.imz = MAXI(g->nz - 2, 0);
    c.stx = g->nx > 1 ? 1 : 0;
    c.sty = g->ny > 1 ? c.sx : 0;
    c.stz = g->nz > 1 ? c.sxy : 0;
    c.bufs = calloc((size_t)nt, sizeof(VertBuf));
    c.seed_tid = malloc((size_t)nseeds * sizeof(int));
    c.seed_off = malloc((size_t)nseeds * sizeof(size_t));
    c.seed_cnt = malloc((size_t)nseeds * sizeof(size_t));
    c.dst = malloc((size_t)nseeds * sizeof(size_t));
    if (!c.bufs || !c.seed_tid || !c.seed_off || !c.seed_cnt || !c.dst) {
        LOGE("vis_streamlines: out of memory");
        goto done;
    }

    run_parallel(pool, nseeds, nt > 1 ? 4 : nseeds, trace_range, &c);

    size_t total = 0, lines = 0;
    for (int s = 0; s < nseeds; s++) {
        if (c.seed_cnt[s] < 2) continue;
        c.dst[s] = total;
        total += c.seed_cnt[s];
        lines++;
    }
    for (int t = 0; t < nt; t++)
        if (c.bufs[t].oom) {
            LOGW("vis_streamlines: out of memory, some lines truncated");
            break;
        }
    if (total == 0) goto done;
    if (total > UINT32_MAX / 2 || lines > UINT32_MAX / 2) {
        LOGE("vis_streamlines: too many vertices (%zu)", total);
        goto done;
    }
    if (total > out->cap_verts) {
        size_t cap = total + total / 4;
        float *v = realloc(out->verts, cap * 5 * sizeof(float));
        if (!v) {
            LOGE("vis_streamlines: out of memory (%zu vertices)", total);
            goto done;
        }
        out->verts = v;
        out->cap_verts = (uint32_t)cap;
    }
    if (lines > out->cap_lines) {
        size_t cap = lines + lines / 4;
        uint32_t *lf = realloc(out->line_first, cap * sizeof(uint32_t));
        if (lf) out->line_first = lf;
        uint32_t *lc = realloc(out->line_count, cap * sizeof(uint32_t));
        if (lc) out->line_count = lc;
        if (!lf || !lc) {
            LOGE("vis_streamlines: out of memory (%zu lines)", lines);
            goto done;
        }
        out->cap_lines = (uint32_t)cap;
    }
    run_parallel(pool, nseeds, 64, copy_range, &c);
    uint32_t l = 0;
    for (int s = 0; s < nseeds; s++) {
        if (c.seed_cnt[s] < 2) continue;
        out->line_first[l] = (uint32_t)c.dst[s];
        out->line_count[l] = (uint32_t)c.seed_cnt[s];
        l++;
    }
    out->nverts = (uint32_t)total;
    out->nlines = l;
done:
    if (c.bufs)
        for (int t = 0; t < nt; t++) free(c.bufs[t].v);
    free(c.bufs);
    free(c.seed_tid);
    free(c.seed_off);
    free(c.seed_cnt);
    free(c.dst);
}

void streamlines_free(StreamlineSet *s) {
    if (!s) return;
    free(s->verts);
    free(s->line_first);
    free(s->line_count);
    memset(s, 0, sizeof *s);
}
