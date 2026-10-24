/* voxel.c - parallel mesh voxelisation: 3-axis nonzero-winding ray votes plus optional distance shell */
#include "voxel.h"
#include "../common.h"

#include <stdatomic.h>

/* Fixed irrational-ish offsets of ray origins (in cells) against edge/vertex hits on regular meshes. */
#define JIT_C 1.37e-4
#define JIT_R 2.71e-4

typedef struct { float d; int32_t col; int32_t s; } Hit; /* s = +1 exiting, -1 entering */
typedef struct { Hit *hit; size_t cap; } Scratch;

static void run_for(ThreadPool *pool, int count, int grain, ParallelFn fn, void *ctx) {
    if (count <= 0) return;
    if (pool) pool_for(pool, count, grain, fn, ctx);
    else fn(ctx, 0, count, 0);
}

static inline float comp(vec3 v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }

/* Row/column index ranges of rays (at idx + 0.5 + jitter) inside [lo, hi], clipped to [0, n-1]. */
static inline bool ray_range(double lo, double hi, double jit, int n, int *i0, int *i1) {
    double a = ceil(lo - 0.5 - jit), b = floor(hi - 0.5 - jit);
    if (!(a <= b) || b < 0 || a > n - 1) return false;
    *i0 = a < 0 ? 0 : (int)a;
    *i1 = b > n - 1 ? n - 1 : (int)b;
    return true;
}

/* Bin triangles into the rows (along axis rx) of rays they may hit (CSR). */
static bool bin_rows(const vec3 *v, uint32_t nt, int cx, int rx, int nc, int nr, double jc, double jr, double margin,
                     uint32_t **off_out, uint32_t **lst_out) {
    uint32_t *off = calloc((size_t)nr + 1, sizeof(uint32_t));
    if (!off) return false;
    uint64_t total = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t *lst = pass ? *lst_out : NULL;
        for (uint32_t t = 0; t < nt; t++) {
            const vec3 *p = v + 3 * (size_t)t;
            float c0 = comp(p[0], cx), c1 = comp(p[1], cx), c2 = comp(p[2], cx);
            float r0 = comp(p[0], rx), r1 = comp(p[1], rx), r2 = comp(p[2], rx);
            int a0, a1, b0, b1;
            if (!ray_range(MINI(c0, MINI(c1, c2)) - margin, MAXI(c0, MAXI(c1, c2)) + margin, jc, nc, &a0, &a1) ||
                !ray_range(MINI(r0, MINI(r1, r2)) - margin, MAXI(r0, MAXI(r1, r2)) + margin, jr, nr, &b0, &b1))
                continue;
            for (int r = b0; r <= b1; r++) {
                if (pass) lst[off[r]++] = t;
                else off[r + 1]++, total++;
            }
        }
        if (!pass) {
            if (total > 0xFFFFFFFFu || !(*lst_out = malloc((size_t)(total ? total : 1) * sizeof(uint32_t)))) {
                free(off);
                return false;
            }
            for (int r = 0; r < nr; r++) off[r + 1] += off[r]; /* off[r] = start of row r = write cursor */
        }
    }
    for (int r = nr; r > 0; r--) off[r] = off[r - 1]; /* cursors advanced to row ends: shift back */
    off[0] = 0;
    *off_out = off;
    return true;
}

typedef struct {
    const vec3 *v;
    const uint32_t *off, *lst;
    int ax, cx, rx, na, nc, hand;
    size_t sa, sc, sr;
    uint8_t *votes;
    Scratch *scr;
    atomic_int failed;
} AxisJob;

/* Edge function with bit-identical magnitude for (p,q) and (q,p); exact zeros resolved by a consistent
 * symbolic perturbation of the sample point so shared edges are counted exactly once. */
static inline int edge_sign(double px, double py, double qx, double qy, double x, double y, double *w) {
    bool swap = px > qx || (px == qx && py > qy);
    if (swap) {
        double tx = px, ty = py;
        px = qx, py = qy, qx = tx, qy = ty;
    }
    double e = (qx - px) * (y - py) - (qy - py) * (x - px);
    int s = e > 0 ? 1 : e < 0 ? -1 : (qy != py ? (qy > py ? -1 : 1) : (qx != px ? 1 : 0));
    *w = swap ? -e : e;
    return swap ? -s : s;
}

static int cmp_hit(const void *a, const void *b) {
    const Hit *x = a, *y = b;
    if (x->col != y->col) return x->col < y->col ? -1 : 1;
    return (x->d > y->d) - (x->d < y->d);
}

static void axis_rows(void *ctx, int begin, int end, int tid) {
    AxisJob *J = ctx;
    Scratch *S = &J->scr[tid];
    for (int row = begin; row < end; row++) {
        uint32_t b = J->off[row], e = J->off[row + 1];
        if (b == e) continue;
        double y = row + 0.5 + JIT_R;
        size_t nh = 0;
        for (uint32_t k = b; k < e; k++) {
            const vec3 *p = J->v + 3 * (size_t)J->lst[k];
            double px[3], py[3], pd[3];
            for (int i = 0; i < 3; i++) px[i] = comp(p[i], J->cx), py[i] = comp(p[i], J->rx), pd[i] = comp(p[i], J->ax);
            int c0, c1;
            if (!ray_range(MINI(px[0], MINI(px[1], px[2])), MAXI(px[0], MAXI(px[1], px[2])), JIT_C, J->nc, &c0, &c1))
                continue;
            double dmin = MINI(pd[0], MINI(pd[1], pd[2])), dmax = MAXI(pd[0], MAXI(pd[1], pd[2]));
            for (int col = c0; col <= c1; col++) {
                double x = col + 0.5 + JIT_C, w0, w1, w2;
                int s0 = edge_sign(px[1], py[1], px[2], py[2], x, y, &w0);
                if (!s0 || edge_sign(px[2], py[2], px[0], py[0], x, y, &w1) != s0 ||
                    edge_sign(px[0], py[0], px[1], py[1], x, y, &w2) != s0)
                    continue;
                double sum = w0 + w1 + w2, d = sum != 0 ? (w0 * pd[0] + w1 * pd[1] + w2 * pd[2]) / sum : pd[0];
                d = CLAMP(d, dmin, dmax);
                if (nh == S->cap) {
                    size_t cap = S->cap ? S->cap * 2 : 1024;
                    Hit *h = realloc(S->hit, cap * sizeof(Hit));
                    if (!h) {
                        atomic_store(&J->failed, 1);
                        return;
                    }
                    S->hit = h, S->cap = cap;
                }
                S->hit[nh++] = (Hit){(float)d, col, s0 * J->hand};
            }
        }
        if (!nh) continue;
        qsort(S->hit, nh, sizeof(Hit), cmp_hit);
        size_t base_r = (size_t)row * J->sr;
        for (size_t g0 = 0, g1; g0 < nh; g0 = g1) {
            int col = S->hit[g0].col, sum = 0;
            for (g1 = g0; g1 < nh && S->hit[g1].col == col; g1++) sum += S->hit[g1].s;
            bool winding = sum == 0; /* else: inconsistent ray (hole / bad winding) -> even-odd */
            size_t base = base_r + (size_t)col * J->sc;
            int state = 0;
            for (size_t q = g0; q + 1 < g1; q++) {
                state = winding ? state - S->hit[q].s : state ^ 1;
                if (!state) continue; /* nonzero rule (also accepts inside-out meshes) */
                double i0 = floor(S->hit[q].d - 0.5) + 1, i1 = floor(S->hit[q + 1].d - 0.5);
                if (i0 < 0) i0 = 0;
                if (i1 > J->na - 1) i1 = J->na - 1;
                if (!(i0 <= i1)) continue;
                for (int i = (int)i0; i <= (int)i1; i++) J->votes[base + (size_t)i * J->sa]++;
            }
        }
    }
}

/* ---------------------------------------------------------------------------------------------- */

static double seg_dist2(const double *p, const double *a, const double *b) {
    double ab[3], ap[3], l2 = 0, t = 0, d2 = 0;
    for (int i = 0; i < 3; i++) ab[i] = b[i] - a[i], ap[i] = p[i] - a[i], l2 += ab[i] * ab[i], t += ab[i] * ap[i];
    t = l2 > 0 ? CLAMP(t / l2, 0.0, 1.0) : 0.0;
    for (int i = 0; i < 3; i++) d2 += (ap[i] - t * ab[i]) * (ap[i] - t * ab[i]);
    return d2;
}

/* Exact squared distance from p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5). */
static double tri_dist2(const double *p, const double *a, const double *b, const double *c) {
    double ab[3], ac[3], ap[3], bp[3], cp[3];
    for (int i = 0; i < 3; i++)
        ab[i] = b[i] - a[i], ac[i] = c[i] - a[i], ap[i] = p[i] - a[i], bp[i] = p[i] - b[i], cp[i] = p[i] - c[i];
#define DOT(u, v) (u[0] * v[0] + u[1] * v[1] + u[2] * v[2])
    double d1 = DOT(ab, ap), d2 = DOT(ac, ap);
    if (d1 <= 0 && d2 <= 0) return DOT(ap, ap);
    double d3 = DOT(ab, bp), d4 = DOT(ac, bp);
    if (d3 >= 0 && d4 <= d3) return DOT(bp, bp);
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return seg_dist2(p, a, b);
    double d5 = DOT(ab, cp), d6 = DOT(ac, cp);
    if (d6 >= 0 && d5 <= d6) return DOT(cp, cp);
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return seg_dist2(p, a, c);
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) return seg_dist2(p, b, c);
    double s = va + vb + vc;
    if (!(s > 0)) return MINI(seg_dist2(p, a, b), MINI(seg_dist2(p, a, c), seg_dist2(p, b, c)));
    double v = vb / s, w = vc / s, q[3];
    for (int i = 0; i < 3; i++) q[i] = ap[i] - ab[i] * v - ac[i] * w;
    return DOT(q, q);
#undef DOT
}

typedef struct {
    const vec3 *v;
    const uint32_t *off, *lst; /* triangles per k slab */
    int nx, ny;
    double R;
    uint8_t *out;
} ShellJob;

static void shell_slabs(void *ctx, int begin, int end, int tid) {
    (void)tid;
    ShellJob *J = ctx;
    double R = J->R, R2 = R * R;
    for (int k = begin; k < end; k++) {
        double z = k + 0.5;
        for (uint32_t e = J->off[k]; e < J->off[k + 1]; e++) {
            const vec3 *p = J->v + 3 * (size_t)J->lst[e];
            double a[3] = {p[0].x, p[0].y, p[0].z}, b[3] = {p[1].x, p[1].y, p[1].z}, c[3] = {p[2].x, p[2].y, p[2].z};
            int j0, j1, i0, i1;
            if (!ray_range(MINI(a[1], MINI(b[1], c[1])) - R, MAXI(a[1], MAXI(b[1], c[1])) + R, 0, J->ny, &j0, &j1) ||
                !ray_range(MINI(a[0], MINI(b[0], c[0])) - R, MAXI(a[0], MAXI(b[0], c[0])) + R, 0, J->nx, &i0, &i1))
                continue;
            /* plane slab |n.(x-a)| <= R|n| restricts the i-range of each row */
            double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            double n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
            double nl = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int j = j0; j <= j1; j++) {
                double y = j + 0.5;
                int lo = i0, hi = i1;
                if (nl > 0 && fabs(n[0]) > 1e-9 * nl) {
                    double rest = n[1] * (y - a[1]) + n[2] * (z - a[2]);
                    double xa = a[0] + (-R * nl - rest) / n[0], xb = a[0] + (R * nl - rest) / n[0];
                    int r0, r1;
                    if (!ray_range(MINI(xa, xb), MAXI(xa, xb), 0, J->nx, &r0, &r1)) continue;
                    lo = MAXI(lo, r0), hi = MINI(hi, r1);
                }
                size_t row = (size_t)J->nx * ((size_t)j + (size_t)J->ny * (size_t)k);
                for (int i = lo; i <= hi; i++) {
                    if (J->out[row + (size_t)i]) continue;
                    double q[3] = {i + 0.5, y, z};
                    if (tri_dist2(q, a, b, c) <= R2) J->out[row + (size_t)i] = 1;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t *out;
    int nx, ny, min_votes;
    uint64_t *count, *frontal;
    int *bmin, *bmax; /* 2 per slab (i, j) */
} FinishJob;

static void threshold_slabs(void *ctx, int begin, int end, int tid) {
    (void)tid;
    FinishJob *J = ctx;
    size_t plane = (size_t)J->nx * (size_t)J->ny;
    uint8_t mv = (uint8_t)J->min_votes;
    for (int k = begin; k < end; k++) {
        uint8_t *o = J->out + plane * (size_t)k;
        for (size_t i = 0; i < plane; i++) o[i] = o[i] >= mv;
    }
}

static void stats_slabs(void *ctx, int begin, int end, int tid) {
    (void)tid;
    FinishJob *J = ctx;
    for (int k = begin; k < end; k++) {
        uint64_t cnt = 0, fr = 0;
        int imin = J->nx, imax = -1, jmin = J->ny, jmax = -1;
        for (int j = 0; j < J->ny; j++) {
            const uint8_t *o = J->out + (size_t)J->nx * ((size_t)j + (size_t)J->ny * (size_t)k);
            int first = -1, last = -1, c = 0;
            for (int i = 0; i < J->nx; i++)
                if (o[i]) {
                    if (first < 0) first = i;
                    last = i, c++;
                }
            if (!c) continue;
            cnt += (uint64_t)c, fr++;
            imin = MINI(imin, first), imax = MAXI(imax, last), jmin = MINI(jmin, j), jmax = MAXI(jmax, j);
        }
        J->count[k] = cnt, J->frontal[k] = fr;
        J->bmin[2 * k] = imin, J->bmin[2 * k + 1] = jmin, J->bmax[2 * k] = imax, J->bmax[2 * k + 1] = jmax;
    }
}

void voxelize_mesh(const Mesh *m, mat4 M, int nx, int ny, int nz, uint8_t *out, const VoxelizeOptions *opt,
                   ThreadPool *pool, VoxelizeResult *result) {
    double t0 = now_seconds();
    VoxelizeResult res;
    memset(&res, 0, sizeof res);
    res.bbox_min[0] = nx, res.bbox_min[1] = ny, res.bbox_min[2] = nz;
    res.bbox_max[0] = res.bbox_max[1] = res.bbox_max[2] = -1;
    if (nx <= 0 || ny <= 0 || nz <= 0 || !out) {
        res.seconds = now_seconds() - t0;
        if (result) *result = res;
        return;
    }
    int n[3] = {nx, ny, nz};
    size_t cells = (size_t)nx * (size_t)ny * (size_t)nz, stride[3] = {1, (size_t)nx, (size_t)nx * (size_t)ny};
    float shell = opt ? opt->shell_radius : 0.0f;
    int min_votes = CLAMP(opt ? opt->min_votes : 2, 1, 3);
    memset(out, 0, cells);

    uint32_t nt = m ? m->tri_count : 0;
    vec3 *v = nt ? malloc(3 * (size_t)nt * sizeof(vec3)) : NULL;
    int nthr = pool ? pool_size(pool) : 1;
    Scratch *scr = calloc((size_t)nthr, sizeof(Scratch));
    bool ok = scr && (!nt || v);
    for (size_t i = 0; ok && i < 3 * (size_t)nt; i++) v[i] = m4_mul_point(M, m->pos[i]);

    /* inside votes: rays along each axis; rows along axis rx are disjoint slabs of the output */
    static const int perm[3][3] = {{0, 1, 2}, {1, 0, 2}, {2, 0, 1}}; /* ax, cx, rx */
    for (int a = 0; ok && nt && a < 3; a++) {
        AxisJob J = {.v = v, .ax = perm[a][0], .cx = perm[a][1], .rx = perm[a][2], .scr = scr, .votes = out};
        uint32_t *off = NULL, *lst = NULL;
        J.na = n[J.ax], J.nc = n[J.cx], J.hand = a == 1 ? -1 : 1;
        J.sa = stride[J.ax], J.sc = stride[J.cx], J.sr = stride[J.rx];
        atomic_init(&J.failed, 0);
        if (!bin_rows(v, nt, J.cx, J.rx, J.nc, n[J.rx], JIT_C, JIT_R, 0.0, &off, &lst)) {
            ok = false;
            break;
        }
        J.off = off, J.lst = lst;
        run_for(pool, n[J.rx], 1, axis_rows, &J);
        if (atomic_load(&J.failed)) ok = false;
        free(off), free(lst);
    }
    FinishJob F = {.out = out, .nx = nx, .ny = ny, .min_votes = min_votes};
    run_for(pool, nz, 4, threshold_slabs, &F);

    if (ok && nt && shell > 0) {
        /* bin triangles into the k slabs (and x-range) their R-expanded bbox reaches */
        uint32_t *off = NULL, *lst = NULL;
        if (bin_rows(v, nt, 0, 2, nx, nz, 0.0, 0.0, shell, &off, &lst)) {
            ShellJob S = {.v = v, .off = off, .lst = lst, .nx = nx, .ny = ny, .R = shell, .out = out};
            run_for(pool, nz, 1, shell_slabs, &S);
        } else {
            ok = false;
        }
        free(off), free(lst);
    }
    if (!ok) LOGE("voxelize: out of memory (%u triangles, %dx%dx%d)", nt, nx, ny, nz);

    F.count = calloc((size_t)nz, sizeof(uint64_t));
    F.frontal = calloc((size_t)nz, sizeof(uint64_t));
    F.bmin = malloc(2 * (size_t)nz * sizeof(int));
    F.bmax = malloc(2 * (size_t)nz * sizeof(int));
    if (F.count && F.frontal && F.bmin && F.bmax) {
        run_for(pool, nz, 4, stats_slabs, &F);
        for (int k = 0; k < nz; k++) {
            if (!F.count[k]) continue;
            res.solid_cells += F.count[k], res.frontal_cells += F.frontal[k];
            res.bbox_min[0] = MINI(res.bbox_min[0], F.bmin[2 * k]), res.bbox_max[0] = MAXI(res.bbox_max[0], F.bmax[2 * k]);
            res.bbox_min[1] = MINI(res.bbox_min[1], F.bmin[2 * k + 1]);
            res.bbox_max[1] = MAXI(res.bbox_max[1], F.bmax[2 * k + 1]);
            res.bbox_min[2] = MINI(res.bbox_min[2], k), res.bbox_max[2] = MAXI(res.bbox_max[2], k);
        }
    }
    free(F.count), free(F.frontal), free(F.bmin), free(F.bmax);
    for (int i = 0; scr && i < nthr; i++) free(scr[i].hit);
    free(scr), free(v);
    res.seconds = now_seconds() - t0;
    if (result) *result = res;
}
