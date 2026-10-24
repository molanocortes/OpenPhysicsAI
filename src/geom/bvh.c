/* bvh.c - triangle BVH (median split on the longest centroid axis, 4 triangles per leaf) */
#include "bvh.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { LEAF_SIZE = 4, STACK_MAX = 128 };

typedef struct {
    int node, lo, hi;
} BuildItem;

static inline double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void sub3(const double *a, const double *b, double *r) { r[0] = a[0] - b[0], r[1] = a[1] - b[1], r[2] = a[2] - b[2]; }
static inline void cross3(const double *a, const double *b, double *r) {
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}

typedef struct {
    double *cent;
    int axis;
} SortCtx;

/* quickselect: order[lo..hi) partitioned so that element k is in place by centroid[axis] */
static void select_kth(int *order, const double *cent, int axis, int lo, int hi, int k) {
    while (hi - lo > 1) {
        int mid = lo + (hi - lo) / 2;
        double pivot = cent[3 * order[mid] + axis];
        int i = lo, j = hi - 1;
        while (i <= j) {
            while (cent[3 * order[i] + axis] < pivot) i++;
            while (cent[3 * order[j] + axis] > pivot) j--;
            if (i <= j) {
                int t = order[i];
                order[i] = order[j], order[j] = t;
                i++, j--;
            }
        }
        if (k <= j) hi = j + 1;
        else if (k >= i) lo = i;
        else return;
    }
}

bool bvh_build(Bvh *b, const double *v, const int *tri, int nt) {
    memset(b, 0, sizeof *b);
    b->v = v, b->tri = tri, b->nt = nt;
    if (nt <= 0) return true;
    b->order = malloc((size_t)nt * sizeof(int));
    double *cent = malloc((size_t)nt * 3 * sizeof(double));
    double *tlo = malloc((size_t)nt * 3 * sizeof(double)), *thi = malloc((size_t)nt * 3 * sizeof(double));
    int cap = 2 * (nt / LEAF_SIZE + 1) + 1;
    b->nodes = malloc((size_t)cap * sizeof(BvhNode));
    if (!b->order || !cent || !tlo || !thi || !b->nodes) {
        free(cent), free(tlo), free(thi);
        bvh_free(b);
        return false;
    }
    for (int t = 0; t < nt; t++) {
        b->order[t] = t;
        for (int k = 0; k < 3; k++) {
            double x0 = v[3 * tri[3 * t] + k], x1 = v[3 * tri[3 * t + 1] + k], x2 = v[3 * tri[3 * t + 2] + k];
            tlo[3 * t + k] = fmin(x0, fmin(x1, x2));
            thi[3 * t + k] = fmax(x0, fmax(x1, x2));
            cent[3 * t + k] = (x0 + x1 + x2) / 3.0;
        }
    }
    BuildItem stack[STACK_MAX];
    int sp = 0;
    b->nnodes = 1;
    stack[sp++] = (BuildItem){0, 0, nt};
    while (sp) {
        int node = stack[sp - 1].node, lo = stack[sp - 1].lo, hi = stack[sp - 1].hi;
        sp--;
        BvhNode *n = &b->nodes[node];
        double clo[3] = {DBL_MAX, DBL_MAX, DBL_MAX}, chi[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
        for (int k = 0; k < 3; k++) n->lo[k] = DBL_MAX, n->hi[k] = -DBL_MAX;
        for (int i = lo; i < hi; i++) {
            int t = b->order[i];
            for (int k = 0; k < 3; k++) {
                n->lo[k] = fmin(n->lo[k], tlo[3 * t + k]);
                n->hi[k] = fmax(n->hi[k], thi[3 * t + k]);
                clo[k] = fmin(clo[k], cent[3 * t + k]);
                chi[k] = fmax(chi[k], cent[3 * t + k]);
            }
        }
        int axis = 0;
        for (int k = 1; k < 3; k++)
            if (chi[k] - clo[k] > chi[axis] - clo[axis]) axis = k;
        /* a median split leaves some leaves smaller than LEAF_SIZE, so the node count can pass the estimate for a
         * perfectly filled tree. Grow rather than collapse the rest of the subtree into one huge leaf, which would
         * make every query scan thousands of triangles. */
        if (b->nnodes + 2 > cap && hi - lo > LEAF_SIZE && chi[axis] - clo[axis] > 0 && sp + 2 <= STACK_MAX) {
            int ncap = cap + cap / 2 + 16;
            BvhNode *grown = realloc(b->nodes, (size_t)ncap * sizeof(BvhNode));
            if (grown) b->nodes = grown, cap = ncap, n = &b->nodes[node];
        }
        if (hi - lo <= LEAF_SIZE || chi[axis] - clo[axis] <= 0 || sp + 2 > STACK_MAX || b->nnodes + 2 > cap) {
            n->first = lo;
            n->count = hi - lo;
            continue;
        }
        int mid = lo + (hi - lo) / 2;
        select_kth(b->order, cent, axis, lo, hi, mid);
        int left = b->nnodes;
        b->nnodes += 2;
        n = &b->nodes[node];
        n->first = left;
        n->count = 0;
        stack[sp++] = (BuildItem){left, lo, mid};
        stack[sp++] = (BuildItem){left + 1, mid, hi};
    }
    free(cent), free(tlo), free(thi);
    return true;
}

void bvh_free(Bvh *b) {
    free(b->nodes);
    free(b->order);
    memset(b, 0, sizeof *b);
}

bool ray_triangle(const double o[3], const double d[3], const double a[3], const double b[3], const double c[3], double *t,
                  double *u, double *v) {
    double e1[3], e2[3], p[3], s[3], q[3];
    sub3(b, a, e1);
    sub3(c, a, e2);
    cross3(d, e2, p);
    double det = dot3(e1, p);
    double scale = sqrt(dot3(e1, e1) * dot3(e2, e2) * dot3(d, d));
    if (!(fabs(det) > 1e-14 * scale)) return false;
    double inv = 1.0 / det;
    sub3(o, a, s);
    double uu = dot3(s, p) * inv;
    if (uu < 0 || uu > 1) return false;
    cross3(s, e1, q);
    double vv = dot3(d, q) * inv;
    if (vv < 0 || uu + vv > 1) return false;
    *t = dot3(e2, q) * inv;
    *u = uu, *v = vv;
    return true;
}

static bool ray_box(const double o[3], const double inv[3], const double lo[3], const double hi[3], double tmin, double tmax, double *entry) {
    for (int k = 0; k < 3; k++) {
        double t0 = (lo[k] - o[k]) * inv[k], t1 = (hi[k] - o[k]) * inv[k];
        if (isnan(t0) || isnan(t1)) { /* ray parallel to the slab and on its boundary */
            if (o[k] < lo[k] || o[k] > hi[k]) return false;
            continue;
        }
        if (t0 > t1) {
            double tt = t0;
            t0 = t1, t1 = tt;
        }
        if (t0 > tmin) tmin = t0;
        if (t1 < tmax) tmax = t1;
        if (tmin > tmax) return false;
    }
    *entry = tmin;
    return true;
}

static void tri_verts(const Bvh *b, int t, const double **a, const double **bb, const double **c) {
    *a = b->v + 3 * b->tri[3 * t];
    *bb = b->v + 3 * b->tri[3 * t + 1];
    *c = b->v + 3 * b->tri[3 * t + 2];
}

bool bvh_ray_nearest(const Bvh *b, const double o[3], const double d[3], double tmin, double tmax, int skip_tri, BvhHit *hit) {
    if (!b->nnodes) return false;
    double inv[3] = {1.0 / d[0], 1.0 / d[1], 1.0 / d[2]};
    int stack[STACK_MAX];
    int sp = 0;
    stack[sp++] = 0;
    bool found = false;
    double best = tmax;
    while (sp) {
        const BvhNode *n = &b->nodes[stack[--sp]];
        double entry;
        if (!ray_box(o, inv, n->lo, n->hi, tmin, best, &entry)) continue;
        if (n->count) {
            for (int i = n->first; i < n->first + n->count; i++) {
                int t = b->order[i];
                if (t == skip_tri) continue;
                const double *pa, *pb, *pc;
                tri_verts(b, t, &pa, &pb, &pc);
                double tt, uu, vv;
                if (ray_triangle(o, d, pa, pb, pc, &tt, &uu, &vv) && tt > tmin && tt < best) {
                    best = tt;
                    hit->tri = t, hit->t = tt, hit->u = uu, hit->v = vv;
                    found = true;
                }
            }
        } else if (sp + 2 <= STACK_MAX) {
            const BvhNode *l = &b->nodes[n->first], *r = &b->nodes[n->first + 1];
            double el, er;
            bool hl = ray_box(o, inv, l->lo, l->hi, tmin, best, &el), hr = ray_box(o, inv, r->lo, r->hi, tmin, best, &er);
            if (hl && hr) {
                /* push the farther child first so the nearer one is visited first */
                stack[sp++] = el <= er ? n->first + 1 : n->first;
                stack[sp++] = el <= er ? n->first : n->first + 1;
            } else if (hl) {
                stack[sp++] = n->first;
            } else if (hr) {
                stack[sp++] = n->first + 1;
            }
        }
    }
    return found;
}

void bvh_ray_all(const Bvh *b, const double o[3], const double d[3], double tmin, BvhHitFn fn, void *ctx) {
    if (!b->nnodes) return;
    double inv[3] = {1.0 / d[0], 1.0 / d[1], 1.0 / d[2]};
    int stack[STACK_MAX];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const BvhNode *n = &b->nodes[stack[--sp]];
        double entry;
        if (!ray_box(o, inv, n->lo, n->hi, tmin, DBL_MAX, &entry)) continue;
        if (n->count) {
            for (int i = n->first; i < n->first + n->count; i++) {
                int t = b->order[i];
                const double *pa, *pb, *pc;
                tri_verts(b, t, &pa, &pb, &pc);
                BvhHit h;
                if (ray_triangle(o, d, pa, pb, pc, &h.t, &h.u, &h.v) && h.t > tmin) {
                    h.tri = t;
                    if (!fn(ctx, &h)) return;
                }
            }
        } else if (sp + 2 <= STACK_MAX) {
            stack[sp++] = n->first;
            stack[sp++] = n->first + 1;
        }
    }
}

void closest_point_triangle(const double p[3], const double a[3], const double b[3], const double c[3], double q[3]) {
    /* Ericson, Real-Time Collision Detection, 5.1.5 */
    double ab[3], ac[3], ap[3], bp[3], cp[3];
    sub3(b, a, ab);
    sub3(c, a, ac);
    sub3(p, a, ap);
    double d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) {
        memcpy(q, a, 3 * sizeof(double));
        return;
    }
    sub3(p, b, bp);
    double d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) {
        memcpy(q, b, 3 * sizeof(double));
        return;
    }
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        double w = d1 / (d1 - d3);
        for (int k = 0; k < 3; k++) q[k] = a[k] + w * ab[k];
        return;
    }
    sub3(p, c, cp);
    double d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) {
        memcpy(q, c, 3 * sizeof(double));
        return;
    }
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        double w = d2 / (d2 - d6);
        for (int k = 0; k < 3; k++) q[k] = a[k] + w * ac[k];
        return;
    }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; k++) q[k] = b[k] + w * (c[k] - b[k]);
        return;
    }
    double denom = 1.0 / (va + vb + vc);
    double vv = vb * denom, ww = vc * denom;
    for (int k = 0; k < 3; k++) q[k] = a[k] + ab[k] * vv + ac[k] * ww;
}

static double box_dist2(const double p[3], const double lo[3], const double hi[3]) {
    double s = 0;
    for (int k = 0; k < 3; k++) {
        double d = p[k] < lo[k] ? lo[k] - p[k] : (p[k] > hi[k] ? p[k] - hi[k] : 0);
        s += d * d;
    }
    return s;
}

bool bvh_closest(const Bvh *b, const double p[3], double max_dist, int *tri, double q[3], double *dist) {
    if (!b->nnodes) return false;
    double best2 = max_dist * max_dist;
    bool found = false;
    int stack[STACK_MAX];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const BvhNode *n = &b->nodes[stack[--sp]];
        if (box_dist2(p, n->lo, n->hi) > best2) continue;
        if (n->count) {
            for (int i = n->first; i < n->first + n->count; i++) {
                int t = b->order[i];
                const double *pa, *pb, *pc;
                tri_verts(b, t, &pa, &pb, &pc);
                double c[3];
                closest_point_triangle(p, pa, pb, pc, c);
                double dx = c[0] - p[0], dy = c[1] - p[1], dz = c[2] - p[2];
                double d2 = dx * dx + dy * dy + dz * dz;
                /* ties go to the lowest triangle index so results are deterministic */
                if (d2 < best2 || (found && d2 == best2 && t < *tri)) {
                    best2 = d2;
                    *tri = t;
                    memcpy(q, c, sizeof c);
                    found = true;
                }
            }
        } else if (sp + 2 <= STACK_MAX) {
            const BvhNode *l = &b->nodes[n->first], *r = &b->nodes[n->first + 1];
            double dl = box_dist2(p, l->lo, l->hi), dr = box_dist2(p, r->lo, r->hi);
            stack[sp++] = dl <= dr ? n->first + 1 : n->first;
            stack[sp++] = dl <= dr ? n->first : n->first + 1;
        }
    }
    if (found) *dist = sqrt(best2);
    return found;
}

void bvh_query_box(const Bvh *b, const double lo[3], const double hi[3], BvhTriFn fn, void *ctx) {
    if (!b->nnodes) return;
    int stack[STACK_MAX];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const BvhNode *n = &b->nodes[stack[--sp]];
        if (n->lo[0] > hi[0] || n->hi[0] < lo[0] || n->lo[1] > hi[1] || n->hi[1] < lo[1] || n->lo[2] > hi[2] || n->hi[2] < lo[2])
            continue;
        if (n->count) {
            for (int i = n->first; i < n->first + n->count; i++)
                if (!fn(ctx, b->order[i])) return;
        } else if (sp + 2 <= STACK_MAX) {
            stack[sp++] = n->first;
            stack[sp++] = n->first + 1;
        }
    }
}

/* ---- triangle-triangle intersection ------------------------------------------------------------ */

static bool seg_hits_tri(const double *p, const double *q, const double *a, const double *b, const double *c) {
    double d[3];
    sub3(q, p, d);
    double t, u, v;
    return ray_triangle(p, d, a, b, c, &t, &u, &v) && t >= 0 && t <= 1;
}

static bool seg_seg_2d(const double *p1, const double *p2, const double *q1, const double *q2, int ax, int ay, double eps) {
    double r0 = p2[ax] - p1[ax], r1 = p2[ay] - p1[ay], s0 = q2[ax] - q1[ax], s1 = q2[ay] - q1[ay];
    double den = r0 * s1 - r1 * s0;
    double w0 = q1[ax] - p1[ax], w1 = q1[ay] - p1[ay];
    if (fabs(den) <= eps) return false; /* parallel: overlap handled by the containment tests */
    double t = (w0 * s1 - w1 * s0) / den, u = (w0 * r1 - w1 * r0) / den;
    return t >= 0 && t <= 1 && u >= 0 && u <= 1;
}

static bool point_in_tri_2d(const double *p, const double *a, const double *b, const double *c, int ax, int ay) {
    double d1 = (p[ax] - b[ax]) * (a[ay] - b[ay]) - (a[ax] - b[ax]) * (p[ay] - b[ay]);
    double d2 = (p[ax] - c[ax]) * (b[ay] - c[ay]) - (b[ax] - c[ax]) * (p[ay] - c[ay]);
    double d3 = (p[ax] - a[ax]) * (c[ay] - a[ay]) - (c[ax] - a[ax]) * (p[ay] - a[ay]);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

bool tri_tri_intersect(const double *a0, const double *a1, const double *a2, const double *b0, const double *b1, const double *b2) {
    double e1[3], e2[3], nb[3], na[3];
    sub3(b1, b0, e1);
    sub3(b2, b0, e2);
    cross3(e1, e2, nb);
    sub3(a1, a0, e1);
    sub3(a2, a0, e2);
    cross3(e1, e2, na);
    double la = 0, lb = 0;
    const double *A[3] = {a0, a1, a2}, *B[3] = {b0, b1, b2};
    for (int i = 0; i < 3; i++) {
        double t[3];
        sub3(A[(i + 1) % 3], A[i], t);
        la = fmax(la, sqrt(dot3(t, t)));
        sub3(B[(i + 1) % 3], B[i], t);
        lb = fmax(lb, sqrt(dot3(t, t)));
    }
    double L = fmax(la, lb);
    double nbl = sqrt(dot3(nb, nb)), nal = sqrt(dot3(na, na));
    if (!(nbl > 0) || !(nal > 0)) return false;
    double eps_b = 1e-10 * nbl * L, eps_a = 1e-10 * nal * L;
    double da[3], db[3];
    for (int i = 0; i < 3; i++) {
        double t[3];
        sub3(A[i], b0, t);
        da[i] = dot3(nb, t);
        sub3(B[i], a0, t);
        db[i] = dot3(na, t);
    }
    if ((da[0] > eps_b && da[1] > eps_b && da[2] > eps_b) || (da[0] < -eps_b && da[1] < -eps_b && da[2] < -eps_b)) return false;
    if ((db[0] > eps_a && db[1] > eps_a && db[2] > eps_a) || (db[0] < -eps_a && db[1] < -eps_a && db[2] < -eps_a)) return false;
    bool coplanar = fabs(da[0]) <= eps_b && fabs(da[1]) <= eps_b && fabs(da[2]) <= eps_b;
    if (coplanar) {
        int drop = 0;
        for (int k = 1; k < 3; k++)
            if (fabs(nb[k]) > fabs(nb[drop])) drop = k;
        int ax = (drop + 1) % 3, ay = (drop + 2) % 3;
        double eps2 = 1e-14 * L * L;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                if (seg_seg_2d(A[i], A[(i + 1) % 3], B[j], B[(j + 1) % 3], ax, ay, eps2)) return true;
        return point_in_tri_2d(a0, b0, b1, b2, ax, ay) || point_in_tri_2d(b0, a0, a1, a2, ax, ay);
    }
    for (int i = 0; i < 3; i++) {
        if (seg_hits_tri(A[i], A[(i + 1) % 3], b0, b1, b2)) return true;
        if (seg_hits_tri(B[i], B[(i + 1) % 3], a0, a1, a2)) return true;
    }
    return false;
}
