/* surface.c - welding, topology, orientation, shells and diagnostics for STL surfaces */
#include "surface.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void sub3(const double *a, const double *b, double *r) { r[0] = a[0] - b[0], r[1] = a[1] - b[1], r[2] = a[2] - b[2]; }
static inline void cross3(const double *a, const double *b, double *r) {
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}
static inline double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void surface_repair_defaults(SurfaceRepairOptions *o) {
    o->remove_degenerate = true;
    o->remove_duplicate_faces = true;
    o->orient_outward = true;
    o->weld_tolerance_rel = 1e-6;
}

void surface_free(Surface *s) {
    free(s->v);
    free(s->tri);
    free(s->src);
    free(s->nbr);
    free(s->comp);
    free(s->normal);
    free(s->area);
    memset(s, 0, sizeof *s);
}

/* ---- welding ----------------------------------------------------------------------------------- */

static uint64_t hash3(int64_t x, int64_t y, int64_t z) {
    uint64_t h = (uint64_t)x * 0x9E3779B97F4A7C15ull ^ (uint64_t)y * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)z * 0x165667B19E3779F9ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return h;
}

/* pos: n points (3n doubles). ids[i] receives a unique vertex index. Returns the unique count, -1 on OOM. */
static int weld_points(const double *pos, int n, double tol, const double lo[3], int *ids, double **uniq) {
    size_t tsize = 64;
    while (tsize < 2 * (size_t)n) tsize <<= 1;
    size_t mask = tsize - 1;
    int *table = malloc(tsize * sizeof(int));
    int *first = malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!table || !first) {
        free(table), free(first);
        return -1;
    }
    memset(table, 0xFF, tsize * sizeof(int));
    double cell = 4.0 * tol, inv = 1.0 / cell, tol2 = tol * tol;
    int nu = 0;
    for (int i = 0; i < n; i++) {
        const double *p = pos + 3 * (size_t)i;
        int64_t c[3];
        int d0[3], d1[3];
        for (int k = 0; k < 3; k++) {
            double f = (p[k] - lo[k]) * inv;
            c[k] = (int64_t)floor(f);
            double fr = f - (double)c[k];
            d0[k] = fr < 0.25 ? -1 : 0;
            d1[k] = fr > 0.75 ? 1 : 0;
        }
        int found = -1;
        for (int pass = 0; pass < 2 && found < 0; pass++)
            for (int oz = d0[2]; oz <= d1[2] && found < 0; oz++)
                for (int oy = d0[1]; oy <= d1[1] && found < 0; oy++)
                    for (int ox = d0[0]; ox <= d1[0] && found < 0; ox++) {
                        bool own = !ox && !oy && !oz;
                        if (own != (pass == 0)) continue;
                        for (size_t h = hash3(c[0] + ox, c[1] + oy, c[2] + oz) & mask; table[h] >= 0; h = (h + 1) & mask) {
                            const double *q = pos + 3 * (size_t)first[table[h]];
                            double dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
                            if (dx * dx + dy * dy + dz * dz <= tol2) {
                                found = table[h];
                                break;
                            }
                        }
                    }
        if (found < 0) {
            found = nu++;
            first[found] = i;
            size_t h = hash3(c[0], c[1], c[2]) & mask;
            while (table[h] >= 0) h = (h + 1) & mask;
            table[h] = found;
        }
        ids[i] = found;
    }
    *uniq = malloc((size_t)(nu ? nu : 1) * 3 * sizeof(double));
    if (!*uniq) {
        free(table), free(first);
        return -1;
    }
    for (int u = 0; u < nu; u++) memcpy(*uniq + 3 * (size_t)u, pos + 3 * (size_t)first[u], 3 * sizeof(double));
    free(table);
    free(first);
    return nu;
}

/* ---- edges ------------------------------------------------------------------------------------- */

typedef struct {
    int lo, hi, tri, k;
} EdgeRec;

static int cmp_edge(const void *a, const void *b) {
    const EdgeRec *x = a, *y = b;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    if (x->hi != y->hi) return x->hi < y->hi ? -1 : 1;
    if (x->tri != y->tri) return x->tri < y->tri ? -1 : 1;
    return x->k - y->k;
}

typedef struct {
    int a, b, c, tri;
} FaceKey;

static int cmp_face(const void *pa, const void *pb) {
    const FaceKey *x = pa, *y = pb;
    if (x->a != y->a) return x->a < y->a ? -1 : 1;
    if (x->b != y->b) return x->b < y->b ? -1 : 1;
    if (x->c != y->c) return x->c < y->c ? -1 : 1;
    return x->tri < y->tri ? -1 : (x->tri > y->tri);
}

/* is (a,b,c) an even permutation of the sorted key (same orientation class)? */
static int orientation_class(int a, int b, int c) {
    /* rotate so the smallest index comes first; then the order of the other two decides the class */
    if (a < b && a < c) return b < c ? 0 : 1;
    if (b < a && b < c) return c < a ? 0 : 1;
    return a < b ? 0 : 1;
}

static int find_root(int *parent, int x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

static void flip_triangle(Surface *s, int t) {
    int *T = s->tri + 3 * (size_t)t;
    int tmp = T[1];
    T[1] = T[2], T[2] = tmp;
    int *N = s->nbr + 3 * (size_t)t;
    tmp = N[0];
    N[0] = N[2], N[2] = tmp; /* edge (0,1)->(0,2) reversed is old edge 2; edge (2,1) is old edge 1 */
}

static bool build_edges(Surface *s, SurfaceDiagnostics *d, bool count_inconsistent) {
    size_t ne = 3 * (size_t)s->nt;
    EdgeRec *e = malloc((ne ? ne : 1) * sizeof(EdgeRec));
    if (!e) return false;
    for (int t = 0; t < s->nt; t++)
        for (int k = 0; k < 3; k++) {
            int a = s->tri[3 * t + k], b = s->tri[3 * t + (k + 1) % 3];
            e[3 * (size_t)t + k] = (EdgeRec){a < b ? a : b, a < b ? b : a, t, k};
        }
    qsort(e, ne, sizeof *e, cmp_edge);
    d->open_edges = d->nonmanifold_edges = 0;
    if (count_inconsistent) d->inconsistent_edges = 0;
    for (size_t i = 0; i < ne;) {
        size_t j = i + 1;
        while (j < ne && e[j].lo == e[i].lo && e[j].hi == e[i].hi) j++;
        size_t n = j - i;
        if (n == 1) {
            s->nbr[3 * (size_t)e[i].tri + e[i].k] = -1;
            d->open_edges++;
        } else if (n == 2) {
            s->nbr[3 * (size_t)e[i].tri + e[i].k] = e[i + 1].tri;
            s->nbr[3 * (size_t)e[i + 1].tri + e[i + 1].k] = e[i].tri;
            int a0 = s->tri[3 * e[i].tri + e[i].k], a1 = s->tri[3 * e[i + 1].tri + e[i + 1].k];
            if (count_inconsistent && a0 == a1) d->inconsistent_edges++;
        } else {
            for (size_t k = i; k < j; k++) s->nbr[3 * (size_t)e[k].tri + e[k].k] = -2;
            d->nonmanifold_edges++;
        }
        i = j;
    }
    free(e);
    return true;
}

/* ---- inside tests ------------------------------------------------------------------------------ */

typedef struct {
    const Surface *s;
    int *counts; /* per component crossings */
    int skip_comp;
    bool ambiguous;
} ParityCtx;

static bool parity_hit(void *ctx, const BvhHit *h) {
    ParityCtx *c = ctx;
    int comp = c->s->comp[h->tri];
    if (comp == c->skip_comp) return true;
    const double eps = 1e-9;
    if (h->u < eps || h->v < eps || 1.0 - h->u - h->v < eps) c->ambiguous = true;
    c->counts[comp]++;
    return true;
}

static const double RAY_DIRS[3][3] = {
    {0.5773502691896258, 0.5773502691896258, 0.5773502691896258},
    {-0.3244428422615251, 0.8436867027770211, 0.4276144409112457},
    {0.7071067811865476, -0.1732050807568877, -0.6855654600401044},
};

/* counts crossings of a ray from p with every component; tries other directions on ambiguous hits */
static bool component_crossings(const Surface *s, const Bvh *bvh, const double p[3], int skip_comp, int *counts) {
    for (int attempt = 0; attempt < 3; attempt++) {
        memset(counts, 0, (size_t)s->ncomp * sizeof(int));
        ParityCtx c = {s, counts, skip_comp, false};
        bvh_ray_all(bvh, p, RAY_DIRS[attempt], 0.0, parity_hit, &c);
        if (!c.ambiguous) return true;
    }
    return true; /* accept the last attempt */
}

bool surface_point_inside(const Surface *s, const Bvh *bvh, const double p[3]) {
    int votes = 0;
    for (int r = 0; r < 3; r++) {
        int crossings = 0;
        struct {
            int n;
        } acc = {0};
        (void)acc;
        int *counts = calloc((size_t)(s->ncomp ? s->ncomp : 1), sizeof(int));
        if (!counts) return false;
        ParityCtx c = {s, counts, -1, false};
        bvh_ray_all(bvh, p, RAY_DIRS[r], 0.0, parity_hit, &c);
        for (int k = 0; k < s->ncomp; k++) crossings += counts[k];
        free(counts);
        votes += crossings & 1;
    }
    return votes >= 2;
}

/* ---- build ------------------------------------------------------------------------------------- */

static bool tri_degenerate(const double *a, const double *b, const double *c) {
    double u[3], v[3], n[3];
    sub3(b, a, u);
    sub3(c, a, v);
    cross3(u, v, n);
    double emax2 = fmax(dot3(u, u), dot3(v, v));
    double w[3];
    sub3(c, b, w);
    emax2 = fmax(emax2, dot3(w, w));
    return !(dot3(n, n) > 1e-20 * emax2 * emax2);
}

static void orient_components(Surface *s, SurfaceDiagnostics *d, bool apply) {
    int *flip = malloc((size_t)(s->nt ? s->nt : 1) * sizeof(int));
    int *queue = malloc((size_t)(s->nt ? s->nt : 1) * sizeof(int));
    char *seen = calloc((size_t)(s->nt ? s->nt : 1), 1);
    if (!flip || !queue || !seen) {
        free(flip), free(queue), free(seen);
        return;
    }
    for (int start = 0; start < s->nt; start++) {
        if (seen[start]) continue;
        int head = 0, tail = 0;
        queue[tail++] = start;
        seen[start] = 1;
        flip[start] = 0;
        bool conflict = false;
        while (head < tail) {
            int t = queue[head++];
            for (int k = 0; k < 3; k++) {
                int o = s->nbr[3 * t + k];
                if (o < 0) continue;
                int a = s->tri[3 * t + k], b = s->tri[3 * t + (k + 1) % 3];
                /* direction of the shared edge in o */
                int ko = -1;
                for (int j = 0; j < 3; j++) {
                    int oa = s->tri[3 * o + j], ob = s->tri[3 * o + (j + 1) % 3];
                    if ((oa == a && ob == b) || (oa == b && ob == a)) {
                        ko = j;
                        break;
                    }
                }
                if (ko < 0) continue;
                bool same_dir = s->tri[3 * o + ko] == a;
                int want = flip[t] ^ (same_dir ? 1 : 0);
                if (!seen[o]) {
                    seen[o] = 1;
                    flip[o] = want;
                    queue[tail++] = o;
                } else if (flip[o] != want) {
                    conflict = true;
                }
            }
        }
        if (conflict) d->non_orientable_components++;
        if (apply && !conflict) {
            for (int i = 0; i < tail; i++)
                if (flip[queue[i]]) {
                    flip_triangle(s, queue[i]);
                    d->flipped_triangles++;
                }
        }
    }
    free(flip), free(queue), free(seen);
}

bool surface_build(const Mesh *m, const SurfaceRepairOptions *optp, Surface *s, SurfaceComponent **comps_out,
                   SurfaceDiagnostics *d, char *err, size_t errlen) {
    SurfaceRepairOptions opt;
    if (optp) opt = *optp;
    else surface_repair_defaults(&opt);
    if (!(opt.weld_tolerance_rel > 0)) opt.weld_tolerance_rel = 1e-6;
    memset(s, 0, sizeof *s);
    memset(d, 0, sizeof *d);
    *comps_out = NULL;
    d->self_intersections = -1;
    d->min_thickness = d->thickness_p05 = d->thickness_median = NAN;
    int n0 = (int)m->tri_count;
    d->source_triangles = m->tri_count;
    if (n0 <= 0) {
        snprintf(err, errlen, "surface has no triangles");
        return false;
    }
    size_t nc = 3 * (size_t)n0;
    double *pos = malloc(nc * 3 * sizeof(double));
    int *ids = malloc(nc * sizeof(int));
    if (!pos || !ids) {
        free(pos), free(ids);
        snprintf(err, errlen, "out of memory for %d triangles", n0);
        return false;
    }
    double lo[3] = {DBL_MAX, DBL_MAX, DBL_MAX}, hi[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (size_t i = 0; i < nc; i++) {
        double p[3] = {m->pos[i].x, m->pos[i].y, m->pos[i].z};
        for (int k = 0; k < 3; k++) {
            if (!isfinite(p[k])) {
                free(pos), free(ids);
                snprintf(err, errlen, "triangle %zu has a non-finite coordinate", i / 3);
                return false;
            }
            pos[3 * i + k] = p[k];
            lo[k] = fmin(lo[k], p[k]);
            hi[k] = fmax(hi[k], p[k]);
        }
    }
    double diag = sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
    d->weld_tolerance = fmax(opt.weld_tolerance_rel * diag, 1e-300);
    int nu = weld_points(pos, (int)nc, d->weld_tolerance, lo, ids, &s->v);
    if (nu < 0) {
        free(pos), free(ids);
        snprintf(err, errlen, "out of memory while welding vertices");
        return false;
    }
    s->nv = nu;
    d->unique_vertices = (uint32_t)nu;
    d->merged_vertices = (uint32_t)(nc - (size_t)nu);

    s->tri = malloc(nc * sizeof(int));
    s->src = malloc((size_t)n0 * sizeof(int));
    if (!s->tri || !s->src) goto oom;
    int nt = 0;
    for (int t = 0; t < n0; t++) {
        int a = ids[3 * t], b = ids[3 * t + 1], c = ids[3 * t + 2];
        bool degen = a == b || b == c || a == c || tri_degenerate(pos + 9 * (size_t)t, pos + 9 * (size_t)t + 3, pos + 9 * (size_t)t + 6);
        if (degen) {
            d->degenerate++;
            if (opt.remove_degenerate) {
                d->degenerate_removed++;
                continue;
            }
        }
        s->tri[3 * nt] = a, s->tri[3 * nt + 1] = b, s->tri[3 * nt + 2] = c;
        s->src[nt] = t;
        nt++;
    }
    free(pos);
    free(ids);
    pos = NULL, ids = NULL;
    if (!nt) {
        surface_free(s);
        snprintf(err, errlen, "all %d triangles are degenerate", n0);
        return false;
    }

    /* duplicate faces */
    {
        FaceKey *fk = malloc((size_t)nt * sizeof(FaceKey));
        char *drop = calloc((size_t)nt, 1);
        if (!fk || !drop) {
            free(fk), free(drop);
            goto oom;
        }
        for (int t = 0; t < nt; t++) {
            int v3[3] = {s->tri[3 * t], s->tri[3 * t + 1], s->tri[3 * t + 2]};
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2 - i; j++)
                    if (v3[j] > v3[j + 1]) {
                        int tmp = v3[j];
                        v3[j] = v3[j + 1], v3[j + 1] = tmp;
                    }
            fk[t] = (FaceKey){v3[0], v3[1], v3[2], t};
        }
        qsort(fk, (size_t)nt, sizeof *fk, cmp_face);
        for (int i = 0; i < nt;) {
            int j = i + 1;
            while (j < nt && fk[j].a == fk[i].a && fk[j].b == fk[i].b && fk[j].c == fk[i].c) j++;
            if (j - i > 1) {
                int cls0 = 0, cls1 = 0;
                for (int k = i; k < j; k++) {
                    int t = fk[k].tri;
                    int cls = orientation_class(s->tri[3 * t], s->tri[3 * t + 1], s->tri[3 * t + 2]);
                    if (cls == 0) {
                        if (cls0++ > 0) {
                            d->duplicate_faces++;
                            if (opt.remove_duplicate_faces) drop[t] = 1;
                        }
                    } else {
                        if (cls1++ > 0) {
                            d->duplicate_faces++;
                            if (opt.remove_duplicate_faces) drop[t] = 1;
                        }
                    }
                }
                if (cls0 && cls1) d->opposite_duplicates++;
            }
            i = j;
        }
        free(fk);
        int w = 0;
        for (int t = 0; t < nt; t++) {
            if (drop[t]) {
                d->duplicate_removed++;
                continue;
            }
            if (w != t) {
                memcpy(s->tri + 3 * w, s->tri + 3 * t, 3 * sizeof(int));
                s->src[w] = s->src[t];
            }
            w++;
        }
        free(drop);
        nt = w;
    }
    s->nt = nt;
    s->nbr = malloc(3 * (size_t)nt * sizeof(int));
    s->comp = malloc((size_t)nt * sizeof(int));
    s->normal = malloc(3 * (size_t)nt * sizeof(double));
    s->area = malloc((size_t)nt * sizeof(double));
    if (!s->nbr || !s->comp || !s->normal || !s->area) goto oom;
    if (!build_edges(s, d, true)) goto oom;

    /* components over all shared edges (manifold and non-manifold) */
    {
        int *parent = malloc((size_t)nt * sizeof(int));
        EdgeRec *e = malloc(3 * (size_t)nt * sizeof(EdgeRec));
        if (!parent || !e) {
            free(parent), free(e);
            goto oom;
        }
        for (int t = 0; t < nt; t++) parent[t] = t;
        for (int t = 0; t < nt; t++)
            for (int k = 0; k < 3; k++) {
                int a = s->tri[3 * t + k], b = s->tri[3 * t + (k + 1) % 3];
                e[3 * (size_t)t + k] = (EdgeRec){a < b ? a : b, a < b ? b : a, t, k};
            }
        qsort(e, 3 * (size_t)nt, sizeof *e, cmp_edge);
        for (size_t i = 1; i < 3 * (size_t)nt; i++)
            if (e[i].lo == e[i - 1].lo && e[i].hi == e[i - 1].hi) {
                int ra = find_root(parent, e[i].tri), rb = find_root(parent, e[i - 1].tri);
                if (ra != rb) parent[ra < rb ? rb : ra] = ra < rb ? ra : rb;
            }
        free(e);
        int *label = malloc((size_t)nt * sizeof(int));
        if (!label) {
            free(parent);
            goto oom;
        }
        for (int t = 0; t < nt; t++) label[t] = -1;
        int nc2 = 0;
        for (int t = 0; t < nt; t++) {
            int r = find_root(parent, t);
            if (label[r] < 0) label[r] = nc2++;
            s->comp[t] = label[r];
        }
        free(label);
        free(parent);
        s->ncomp = nc2;
    }

    orient_components(s, d, opt.orient_outward);
    if (opt.orient_outward && d->flipped_triangles) {
        uint32_t incons = d->inconsistent_edges;
        build_edges(s, d, false);
        d->inconsistent_edges = incons;
    }

    SurfaceComponent *comps = calloc((size_t)s->ncomp, sizeof *comps);
    if (!comps) goto oom;
    for (int c = 0; c < s->ncomp; c++) {
        comps[c].closed = true;
        comps[c].parent = -1;
        for (int k = 0; k < 3; k++) comps[c].bmin[k] = DBL_MAX, comps[c].bmax[k] = -DBL_MAX;
    }
    for (int k = 0; k < 3; k++) d->bmin[k] = DBL_MAX, d->bmax[k] = -DBL_MAX;
    for (int t = 0; t < nt; t++) {
        SurfaceComponent *c = &comps[s->comp[t]];
        c->ntri++;
        for (int k = 0; k < 3; k++)
            if (s->nbr[3 * t + k] < 0) c->closed = false;
        for (int j = 0; j < 3; j++) {
            const double *p = s->v + 3 * (size_t)s->tri[3 * t + j];
            for (int k = 0; k < 3; k++) {
                c->bmin[k] = fmin(c->bmin[k], p[k]);
                c->bmax[k] = fmax(c->bmax[k], p[k]);
                d->bmin[k] = fmin(d->bmin[k], p[k]);
                d->bmax[k] = fmax(d->bmax[k], p[k]);
            }
        }
    }
    /* signed volume about each component's box centre (reduces cancellation) */
    for (int t = 0; t < nt; t++) {
        SurfaceComponent *c = &comps[s->comp[t]];
        double o[3], p[3][3];
        for (int k = 0; k < 3; k++) o[k] = 0.5 * (c->bmin[k] + c->bmax[k]);
        for (int j = 0; j < 3; j++) sub3(s->v + 3 * (size_t)s->tri[3 * t + j], o, p[j]);
        double cr[3];
        cross3(p[1], p[2], cr);
        c->volume += dot3(p[0], cr) / 6.0;
    }

    /* nesting of closed shells */
    {
        Bvh bvh;
        int *order = malloc((size_t)s->ncomp * sizeof(int));
        int *counts = malloc((size_t)s->ncomp * sizeof(int));
        if (!order || !counts || !bvh_build(&bvh, s->v, s->tri, nt)) {
            free(order), free(counts);
            free(comps);
            goto oom;
        }
        int nclosed = 0;
        for (int c = 0; c < s->ncomp; c++)
            if (comps[c].closed) order[nclosed++] = c;
        /* larger boxes first so parents are resolved before children */
        for (int i = 1; i < nclosed; i++) {
            int x = order[i];
            double vx = (comps[x].bmax[0] - comps[x].bmin[0]) * (comps[x].bmax[1] - comps[x].bmin[1]) * (comps[x].bmax[2] - comps[x].bmin[2]);
            int j = i - 1;
            while (j >= 0) {
                int y = order[j];
                double vy = (comps[y].bmax[0] - comps[y].bmin[0]) * (comps[y].bmax[1] - comps[y].bmin[1]) * (comps[y].bmax[2] - comps[y].bmin[2]);
                if (vy >= vx) break;
                order[j + 1] = y;
                j--;
            }
            order[j + 1] = x;
        }
        int first_tri_of = 0;
        (void)first_tri_of;
        for (int i = 0; i < nclosed; i++) {
            int c = order[i];
            /* test point: centroid of the component's first triangle */
            int t0 = -1;
            for (int t = 0; t < nt && t0 < 0; t++)
                if (s->comp[t] == c) t0 = t;
            if (t0 < 0) continue;
            double p[3] = {0, 0, 0};
            for (int j = 0; j < 3; j++)
                for (int k = 0; k < 3; k++) p[k] += s->v[3 * s->tri[3 * t0 + j] + k] / 3.0;
            component_crossings(s, &bvh, p, c, counts);
            double best_vol = DBL_MAX;
            for (int a = 0; a < s->ncomp; a++) {
                if (a == c || !comps[a].closed || !(counts[a] & 1)) continue;
                double va = (comps[a].bmax[0] - comps[a].bmin[0]) * (comps[a].bmax[1] - comps[a].bmin[1]) * (comps[a].bmax[2] - comps[a].bmin[2]);
                if (va < best_vol) best_vol = va, comps[c].parent = a;
            }
            if (comps[c].parent >= 0) {
                comps[c].depth = comps[comps[c].parent].depth + 1;
                d->nested_shells++;
            }
        }
        bvh_free(&bvh);
        free(order);
        free(counts);
    }

    if (opt.orient_outward) {
        for (int c = 0; c < s->ncomp; c++) {
            if (!comps[c].closed) continue;
            bool want_positive = (comps[c].depth % 2) == 0;
            if ((comps[c].volume < 0) == want_positive && comps[c].volume != 0) {
                for (int t = 0; t < nt; t++)
                    if (s->comp[t] == c) flip_triangle(s, t);
                comps[c].volume = -comps[c].volume;
                comps[c].flipped = true;
                d->flipped_components++;
            }
        }
    }

    for (int t = 0; t < nt; t++) {
        const double *a = s->v + 3 * (size_t)s->tri[3 * t], *b = s->v + 3 * (size_t)s->tri[3 * t + 1], *c = s->v + 3 * (size_t)s->tri[3 * t + 2];
        double u[3], w[3], n[3];
        sub3(b, a, u);
        sub3(c, a, w);
        cross3(u, w, n);
        double l = sqrt(dot3(n, n));
        s->area[t] = 0.5 * l;
        for (int k = 0; k < 3; k++) s->normal[3 * t + k] = l > 0 ? n[k] / l : 0;
        comps[s->comp[t]].area += s->area[t];
        d->area += s->area[t];
    }
    d->components = s->ncomp;
    for (int c = 0; c < s->ncomp; c++) {
        if (comps[c].closed) {
            d->closed_components++;
            d->volume += comps[c].volume;
        }
    }
    d->watertight = d->open_edges == 0 && d->nonmanifold_edges == 0;

    /* consistency after repair: recount same-direction manifold edges */
    {
        uint32_t after = 0;
        for (int t = 0; t < nt; t++)
            for (int k = 0; k < 3; k++) {
                int o = s->nbr[3 * t + k];
                if (o < 0 || o < t) continue;
                int a = s->tri[3 * t + k], b = s->tri[3 * t + (k + 1) % 3];
                for (int j = 0; j < 3; j++)
                    if (s->tri[3 * o + j] == a && s->tri[3 * o + (j + 1) % 3] == b) after++;
            }
        d->consistently_oriented = after == 0;
    }
    d->closed_solid = d->watertight && d->consistently_oriented && d->volume > 0 && d->non_orientable_components == 0;

    /* boundary loops (holes) */
    if (d->open_edges) {
        int no = (int)d->open_edges;
        int *ea = malloc((size_t)no * sizeof(int)), *eb = malloc((size_t)no * sizeof(int));
        int *head = malloc((size_t)s->nv * sizeof(int)), *next = malloc((size_t)no * sizeof(int));
        char *used = calloc((size_t)no, 1);
        if (ea && eb && head && next && used) {
            int n = 0;
            for (int t = 0; t < nt; t++)
                for (int k = 0; k < 3; k++)
                    if (s->nbr[3 * t + k] == -1 && n < no) ea[n] = s->tri[3 * t + k], eb[n] = s->tri[3 * t + (k + 1) % 3], n++;
            for (int v = 0; v < s->nv; v++) head[v] = -1;
            /* index open edges by their end vertex: a loop continues with the edge that starts where this one ends,
             * which in a consistently oriented surface is an edge whose start equals our end */
            for (int i = 0; i < n; i++) next[i] = head[ea[i]], head[ea[i]] = i;
            for (int i = 0; i < n; i++) {
                if (used[i]) continue;
                SurfaceHole h = {0, 0, {0, 0, 0}};
                int cur = i;
                while (cur >= 0 && !used[cur]) {
                    used[cur] = 1;
                    const double *pa = s->v + 3 * (size_t)ea[cur], *pb = s->v + 3 * (size_t)eb[cur];
                    double dd[3];
                    sub3(pb, pa, dd);
                    h.perimeter += sqrt(dot3(dd, dd));
                    for (int k = 0; k < 3; k++) h.centroid[k] += pa[k];
                    h.edges++;
                    int nxt = -1;
                    /* prefer an edge leaving eb (same winding); fall back to any unused edge touching eb */
                    for (int j = head[eb[cur]]; j >= 0; j = next[j])
                        if (!used[j]) {
                            nxt = j;
                            break;
                        }
                    if (nxt < 0)
                        for (int j = 0; j < n; j++)
                            if (!used[j] && (eb[j] == eb[cur])) {
                                nxt = j;
                                break;
                            }
                    cur = nxt;
                }
                for (int k = 0; k < 3; k++) h.centroid[k] /= h.edges;
                d->boundary_loops++;
                /* keep the largest holes */
                int pos2 = d->nholes < SURFACE_MAX_HOLES ? d->nholes : SURFACE_MAX_HOLES - 1;
                if (d->nholes < SURFACE_MAX_HOLES || h.perimeter > d->holes[SURFACE_MAX_HOLES - 1].perimeter) {
                    d->holes[pos2] = h;
                    if (d->nholes < SURFACE_MAX_HOLES) d->nholes++;
                    for (int q = pos2; q > 0 && d->holes[q].perimeter > d->holes[q - 1].perimeter; q--) {
                        SurfaceHole tmp = d->holes[q];
                        d->holes[q] = d->holes[q - 1], d->holes[q - 1] = tmp;
                    }
                }
            }
        }
        free(ea), free(eb), free(head), free(next), free(used);
    }

    *comps_out = comps;
    return true;
oom:
    free(pos);
    free(ids);
    surface_free(s);
    snprintf(err, errlen, "out of memory building the surface topology (%d triangles)", n0);
    return false;
}

/* ---- self-intersections ------------------------------------------------------------------------ */

typedef struct {
    const Surface *s;
    int t;
    int found;
    int max_pairs;
    SurfaceDiagnostics *d;
} IsectCtx;

static bool isect_candidate(void *ctx, int o) {
    IsectCtx *c = ctx;
    if (o <= c->t) return true;
    const int *A = c->s->tri + 3 * (size_t)c->t, *B = c->s->tri + 3 * (size_t)o;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (A[i] == B[j]) return true; /* neighbours sharing a vertex are not tested */
    const double *v = c->s->v;
    if (tri_tri_intersect(v + 3 * A[0], v + 3 * A[1], v + 3 * A[2], v + 3 * B[0], v + 3 * B[1], v + 3 * B[2])) {
        if (c->d->self_intersection_samples < SURFACE_MAX_INTERSECTIONS) {
            int k = c->d->self_intersection_samples++;
            for (int q = 0; q < 3; q++)
                c->d->intersection_points[3 * k + q] = (v[3 * A[0] + q] + v[3 * A[1] + q] + v[3 * A[2] + q]) / 3.0;
        }
        c->found++;
        if (c->found >= c->max_pairs) return false;
    }
    return true;
}

int surface_check_self_intersections(const Surface *s, const Bvh *bvh, int max_pairs, SurfaceDiagnostics *d) {
    IsectCtx c = {s, 0, 0, max_pairs > 0 ? max_pairs : 1000000, d};
    d->self_intersection_samples = 0;
    for (int t = 0; t < s->nt && c.found < c.max_pairs; t++) {
        double lo[3], hi[3];
        for (int k = 0; k < 3; k++) {
            double a = s->v[3 * s->tri[3 * t] + k], b = s->v[3 * s->tri[3 * t + 1] + k], cc = s->v[3 * s->tri[3 * t + 2] + k];
            lo[k] = fmin(a, fmin(b, cc)) - d->weld_tolerance;
            hi[k] = fmax(a, fmax(b, cc)) + d->weld_tolerance;
        }
        c.t = t;
        bvh_query_box(bvh, lo, hi, isect_candidate, &c);
    }
    d->self_intersections = c.found;
    return c.found;
}

/* ---- thickness --------------------------------------------------------------------------------- */

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y);
}

void surface_check_thickness(const Surface *s, const Bvh *bvh, int samples, SurfaceDiagnostics *d) {
    if (samples <= 0 || s->nt == 0) return;
    double *vals = malloc((size_t)samples * sizeof(double));
    if (!vals) return;
    double total = d->area > 0 ? d->area : 1;
    double step = total / samples, acc = 0;
    int n = 0, stratum = 0;
    double diag = sqrt((d->bmax[0] - d->bmin[0]) * (d->bmax[0] - d->bmin[0]) + (d->bmax[1] - d->bmin[1]) * (d->bmax[1] - d->bmin[1]) +
                       (d->bmax[2] - d->bmin[2]) * (d->bmax[2] - d->bmin[2]));
    d->min_thickness = NAN;
    /* one sample per area stratum: a triangle receives as many samples as strata centres fall inside its share of
     * the cumulative area, placed at low-discrepancy (R2 sequence) points inside the triangle */
    for (int t = 0; t < s->nt && stratum < samples; t++) {
        acc += s->area[t];
        const double *nrm = s->normal + 3 * (size_t)t;
        const double *A = s->v + 3 * (size_t)s->tri[3 * t], *B = s->v + 3 * (size_t)s->tri[3 * t + 1], *C = s->v + 3 * (size_t)s->tri[3 * t + 2];
        for (; stratum < samples && (stratum + 0.5) * step < acc; stratum++) {
            if (dot3(nrm, nrm) < 0.5) continue;
            double r1 = fmod(0.5 + stratum * 0.7548776662466927, 1.0), r2 = fmod(0.5 + stratum * 0.5698402909980532, 1.0);
            double sq = sqrt(r1), wa = 1.0 - sq, wb = sq * (1.0 - r2), wc = sq * r2;
            double o[3], dir[3] = {-nrm[0], -nrm[1], -nrm[2]};
            for (int k = 0; k < 3; k++) o[k] = wa * A[k] + wb * B[k] + wc * C[k];
            BvhHit h;
            if (!bvh_ray_nearest(bvh, o, dir, 1e-9 * diag, 2 * diag, t, &h)) continue;
            /* the ray must leave the material through a face pointing along it */
            if (dot3(s->normal + 3 * (size_t)h.tri, dir) <= 0) continue;
            vals[n] = h.t;
            if (!(d->min_thickness <= h.t)) {
                d->min_thickness = h.t;
                memcpy(d->min_thickness_at, o, sizeof o);
            }
            n++;
        }
    }
    d->thickness_samples = n;
    if (n) {
        qsort(vals, (size_t)n, sizeof(double), cmp_double);
        d->thickness_p05 = vals[(int)floor(0.05 * (n - 1))];
        d->thickness_median = vals[(n - 1) / 2];
    }
    free(vals);
}
