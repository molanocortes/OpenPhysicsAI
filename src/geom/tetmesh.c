/* tetmesh.c - isosurface stuffing on a graded BCC lattice over a 2:1 octree (docs/contracts/tet-mesh.md) */
#include "tetmesh.h"
#include "bvh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

double now_seconds(void);

const int TETMESH_EDGE[6][2] = {{0, 1}, {1, 2}, {2, 0}, {0, 3}, {1, 3}, {2, 3}};
/* face k is opposite corner k; winding is outward for a positively oriented tetrahedron (0,1,2,3) */
const int TETMESH_FACE[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};

#define ALPHA_LONG 0.24999
#define ALPHA_SHORT 0.41189

/* ---- small geometry ------------------------------------------------------------------------------------------- */

static void sub3(const double *a, const double *b, double *c) { c[0] = a[0] - b[0], c[1] = a[1] - b[1], c[2] = a[2] - b[2]; }
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const double *a, const double *b, double *c) {
    c[0] = a[1] * b[2] - a[2] * b[1], c[1] = a[2] * b[0] - a[0] * b[2], c[2] = a[0] * b[1] - a[1] * b[0];
}
static double norm3(const double *a) { return sqrt(dot3(a, a)); }

double tet_signed_volume(const double X[4][3]) {
    double a[3], b[3], c[3], n[3];
    sub3(X[1], X[0], a), sub3(X[2], X[0], b), sub3(X[3], X[0], c);
    cross3(a, b, n);
    return dot3(n, c) / 6.0;
}

void tet_dihedral_angles(const double X[4][3], double deg[6]) {
    /* outward face normals; the dihedral angle at the edge shared by faces i and j is pi minus their angle */
    double n[4][3];
    for (int k = 0; k < 4; k++) {
        const int *f = TETMESH_FACE[k];
        double a[3], b[3];
        sub3(X[f[1]], X[f[0]], a), sub3(X[f[2]], X[f[0]], b);
        cross3(a, b, n[k]);
        double l = norm3(n[k]);
        if (l > 0)
            for (int q = 0; q < 3; q++) n[k][q] /= l;
    }
    for (int e = 0; e < 6; e++) {
        /* edge (a,b) is shared by the faces opposite the two other corners */
        int a = TETMESH_EDGE[e][0], b = TETMESH_EDGE[e][1], o[2], no = 0;
        for (int k = 0; k < 4; k++)
            if (k != a && k != b) o[no++] = k;
        double c = -dot3(n[o[0]], n[o[1]]);
        if (c > 1) c = 1;
        if (c < -1) c = -1;
        deg[e] = acos(c) * 180.0 / M_PI;
    }
}

double tet_aspect_ratio(const double X[4][3]) {
    double V = fabs(tet_signed_volume(X));
    if (!(V > 0)) return INFINITY;
    double area = 0;
    for (int k = 0; k < 4; k++) {
        const int *f = TETMESH_FACE[k];
        double a[3], b[3], n[3];
        sub3(X[f[1]], X[f[0]], a), sub3(X[f[2]], X[f[0]], b);
        cross3(a, b, n);
        area += 0.5 * norm3(n);
    }
    double r = 3 * V / area;
    /* circumradius: |a^2 (b x c) + b^2 (c x a) + c^2 (a x b)| / (12 V) with edges from corner 0 */
    double a[3], b[3], c[3], bc[3], ca[3], ab[3], s[3];
    sub3(X[1], X[0], a), sub3(X[2], X[0], b), sub3(X[3], X[0], c);
    cross3(b, c, bc), cross3(c, a, ca), cross3(a, b, ab);
    double la = dot3(a, a), lb = dot3(b, b), lc = dot3(c, c);
    for (int q = 0; q < 3; q++) s[q] = la * bc[q] + lb * ca[q] + lc * ab[q];
    double R = norm3(s) / (12 * V);
    return R / (3 * r);
}

/* ---- hash maps (open addressing, uint64 key -> int) --------------------------------------------------------- */

typedef struct {
    uint64_t *key;
    int *val;
    size_t cap, n;
} HMap;

#define HEMPTY UINT64_MAX

static uint64_t hmix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static bool hm_init(HMap *m, size_t cap) {
    size_t c = 64;
    while (c < 2 * cap) c <<= 1;
    m->key = malloc(c * sizeof(uint64_t));
    m->val = malloc(c * sizeof(int));
    if (!m->key || !m->val) return false;
    for (size_t i = 0; i < c; i++) m->key[i] = HEMPTY;
    m->cap = c, m->n = 0;
    return true;
}

static void hm_free(HMap *m) {
    free(m->key), free(m->val);
    memset(m, 0, sizeof *m);
}

static int hm_get(const HMap *m, uint64_t k) {
    size_t i = hmix(k) & (m->cap - 1);
    while (m->key[i] != HEMPTY) {
        if (m->key[i] == k) return m->val[i];
        i = (i + 1) & (m->cap - 1);
    }
    return -1;
}

static bool hm_grow(HMap *m) {
    HMap n;
    if (!hm_init(&n, m->cap)) return false; /* doubles: init takes 2 x cap */
    for (size_t i = 0; i < m->cap; i++)
        if (m->key[i] != HEMPTY) {
            size_t j = hmix(m->key[i]) & (n.cap - 1);
            while (n.key[j] != HEMPTY) j = (j + 1) & (n.cap - 1);
            n.key[j] = m->key[i], n.val[j] = m->val[i], n.n++;
        }
    hm_free(m);
    *m = n;
    return true;
}

/* sets key -> val (overwrites); returns false on OOM */
static bool hm_put(HMap *m, uint64_t k, int val) {
    if (2 * (m->n + 1) > m->cap && !hm_grow(m)) return false;
    size_t i = hmix(k) & (m->cap - 1);
    while (m->key[i] != HEMPTY) {
        if (m->key[i] == k) {
            m->val[i] = val;
            return true;
        }
        i = (i + 1) & (m->cap - 1);
    }
    m->key[i] = k, m->val[i] = val, m->n++;
    return true;
}

/* ---- dynamic arrays ------------------------------------------------------------------------------------------- */

#define GROW(ptr, n, cap, fail)                                                                                          \
    do {                                                                                                                 \
        if ((n) >= (cap)) {                                                                                              \
            size_t nc_ = (cap) ? 2 * (size_t)(cap) : 1024;                                                               \
            void *np_ = realloc((ptr), nc_ * sizeof *(ptr));                                                             \
            if (!np_) goto fail;                                                                                         \
            (ptr) = np_, (cap) = nc_;                                                                                    \
        }                                                                                                                \
    } while (0)

/* ---- the octree ----------------------------------------------------------------------------------------------- */

typedef struct {
    int x, y, z, s; /* finest-cell units; s = 0 for a leaf that was split */
} Leaf;

typedef struct {
    Leaf *leaf;
    size_t n, cap;
    HMap map;  /* leaf key -> index */
    int root;  /* root size in finest cells (a power of two) */
    int depth;
    double origin[3], h;
} Octree;

static int ilog2(int s) {
    int l = 0;
    while ((1 << l) < s) l++;
    return l;
}

static uint64_t leaf_key(int x, int y, int z, int s) {
    return ((uint64_t)x << 44) | ((uint64_t)y << 24) | ((uint64_t)z << 4) | (uint64_t)ilog2(s);
}

static int find_leaf(const Octree *o, int x, int y, int z) {
    if (x < 0 || y < 0 || z < 0 || x >= o->root || y >= o->root || z >= o->root) return -1;
    for (int s = 1; s <= o->root; s <<= 1) {
        int i = hm_get(&o->map, leaf_key(x & ~(s - 1), y & ~(s - 1), z & ~(s - 1), s));
        if (i >= 0 && o->leaf[i].s == s) return i;
    }
    return -1;
}

static bool add_leaf(Octree *o, int x, int y, int z, int s) {
    GROW(o->leaf, o->n, o->cap, oom);
    o->leaf[o->n] = (Leaf){x, y, z, s};
    if (!hm_put(&o->map, leaf_key(x, y, z, s), (int)o->n)) return false;
    o->n++;
    return true;
oom:
    return false;
}

static bool split_leaf(Octree *o, size_t i) {
    Leaf L = o->leaf[i];
    if (L.s <= 1) return true;
    o->leaf[i].s = 0;
    hm_put(&o->map, leaf_key(L.x, L.y, L.z, L.s), -1);
    int h = L.s / 2;
    for (int c = 0; c < 8; c++)
        if (!add_leaf(o, L.x + (c & 1 ? h : 0), L.y + (c & 2 ? h : 0), L.z + (c & 4 ? h : 0), h)) return false;
    return true;
}

static void leaf_centre(const Octree *o, const Leaf *L, double p[3]) {
    p[0] = o->origin[0] + (L->x + 0.5 * L->s) * o->h;
    p[1] = o->origin[1] + (L->y + 0.5 * L->s) * o->h;
    p[2] = o->origin[2] + (L->z + 0.5 * L->s) * o->h;
}

typedef struct {
    const TetMeshSettings *s;
    const double *pts; /* 3 per point */
    double *f;
} SdfBatch;

static void sdf_range(void *vctx, int begin, int end, int tid) {
    (void)tid;
    SdfBatch *b = vctx;
    for (int i = begin; i < end; i++) b->f[i] = b->s->sdf(b->s->ctx, b->pts + 3 * (size_t)i);
}

static void sdf_many(const TetMeshSettings *s, const double *pts, double *f, int n) {
    SdfBatch b = {s, pts, f};
    if (s->pool && n > 64) pool_for(s->pool, n, 32, sdf_range, &b);
    else sdf_range(&b, 0, n, 0);
}

/* refine where the surface passes (to one finest cell) and inside (to interior units); returns false on OOM */
static bool octree_refine(Octree *o, const TetMeshSettings *s, int surface_units, int interior_units) {
    size_t *work = NULL, nwork = 0, capw = 0;
    double *pts = NULL, *f = NULL;
    GROW(work, nwork, capw, oom);
    work[nwork++] = 0;
    while (nwork > 0) {
        pts = realloc(pts, 3 * nwork * sizeof(double));
        f = realloc(f, nwork * sizeof(double));
        if (!pts || !f) goto oom;
        for (size_t i = 0; i < nwork; i++) leaf_centre(o, &o->leaf[work[i]], pts + 3 * i);
        sdf_many(s, pts, f, (int)nwork);
        size_t *next = NULL, nnext = 0, capn = 0;
        for (size_t i = 0; i < nwork; i++) {
            size_t li = work[i];
            int sz = o->leaf[li].s;
            if (sz <= surface_units) continue;
            double half_diag = 0.5 * sqrt(3.0) * sz * o->h;
            bool surface = fabs(f[i]) <= half_diag * 1.0001;
            bool interior = f[i] < 0 && sz > interior_units;
            if (!surface && !interior) continue;
            size_t first = o->n;
            if (!split_leaf(o, li)) {
                free(next);
                goto oom;
            }
            for (size_t c = first; c < o->n; c++) {
                GROW(next, nnext, capn, oom_next);
                next[nnext++] = c;
            }
            continue;
        oom_next:
            free(next);
            goto oom;
        }
        free(work);
        work = next, nwork = nnext, capw = capn;
    }
    free(work), free(pts), free(f);
    return true;
oom:
    free(work), free(pts), free(f);
    return false;
}

/* thin walls: a surface leaf whose deepest corner is shallower than half its size may sit in a wall too thin for its
 * lattice (stuffing then warps every inside vertex onto the surface and the wall vanishes). The wall's thickness is
 * measured along the surface normal from the leaf centre; below 1.5 leaf sizes the leaf is split, down to the finest
 * cell (thin_levels below surface_size). */
typedef struct {
    const Octree *o;
    const TetMeshSettings *s;
    const size_t *cand;
    unsigned char *thin;
} ThinCtx;

static void thin_range(void *vctx, int begin, int end, int tid) {
    (void)tid;
    ThinCtx *c = vctx;
    const TetMeshSettings *s = c->s;
    for (int i = begin; i < end; i++) {
        const Leaf *L = &c->o->leaf[c->cand[i]];
        double size = L->s * c->o->h, ctr[3];
        leaf_centre(c->o, L, ctr);
        c->thin[i] = 0;
        double fc = s->sdf(s->ctx, ctr);
        if (fabs(fc) > 0.5 * sqrt(3.0) * size * 1.0001) continue;
        double fmin = fc;
        for (int k = 0; k < 8; k++) {
            double p[3] = {c->o->origin[0] + (L->x + (k & 1 ? L->s : 0)) * c->o->h, c->o->origin[1] + (L->y + (k & 2 ? L->s : 0)) * c->o->h,
                           c->o->origin[2] + (L->z + (k & 4 ? L->s : 0)) * c->o->h};
            double f = s->sdf(s->ctx, p);
            if (f < fmin) fmin = f;
        }
        if (fmin <= -0.5 * size) continue; /* a vertex deep enough inside: the wall survives */
        double g[3], eps = 0.05 * size;
        for (int k = 0; k < 3; k++) {
            double a[3] = {ctr[0], ctr[1], ctr[2]}, b[3] = {ctr[0], ctr[1], ctr[2]};
            a[k] += eps, b[k] -= eps;
            g[k] = (s->sdf(s->ctx, a) - s->sdf(s->ctx, b)) / (2 * eps);
        }
        double gl = norm3(g);
        if (!(gl > 0)) continue;
        for (int k = 0; k < 3; k++) g[k] /= gl;
        double q[3] = {ctr[0] - fc * g[0], ctr[1] - fc * g[1], ctr[2] - fc * g[2]}, step = size / 8;
        bool entered = false;
        for (int k = 1; k <= 16; k++) {
            double p[3] = {q[0] - k * step * g[0], q[1] - k * step * g[1], q[2] - k * step * g[2]};
            double f = s->sdf(s->ctx, p);
            if (f < 0) entered = true;
            else if (entered) {
                if (k * step < 1.5 * size) c->thin[i] = 1;
                break;
            }
        }
    }
}

static bool octree_thin(Octree *o, const TetMeshSettings *s, int surface_units, int *nthin, int *unresolved) {
    size_t *cand = NULL, ncand = 0, capc = 0;
    unsigned char *thin = NULL;
    for (size_t i = 0; i < o->n; i++)
        if (o->leaf[i].s >= 1 && o->leaf[i].s <= surface_units) {
            GROW(cand, ncand, capc, oom);
            cand[ncand++] = i;
        }
    while (ncand) {
        thin = realloc(thin, ncand);
        if (!thin) goto oom;
        ThinCtx c = {o, s, cand, thin};
        if (s->pool && ncand > 64) pool_for(s->pool, (int)ncand, 16, thin_range, &c);
        else thin_range(&c, 0, (int)ncand, 0);
        size_t *next = NULL, nnext = 0, capn = 0;
        for (size_t i = 0; i < ncand; i++) {
            if (!thin[i]) continue;
            if (o->leaf[cand[i]].s <= 1) { /* already the finest cell: this wall cannot be resolved at this surface_size */
                (*unresolved)++;
                continue;
            }
            (*nthin)++;
            size_t first = o->n;
            if (!split_leaf(o, cand[i])) {
                free(next);
                goto oom;
            }
            for (size_t k = first; k < o->n; k++)
                if (o->leaf[k].s >= 1) { /* the finest children are tested too, to count what cannot be resolved */
                    if (nnext >= capn) {
                        size_t nc = capn ? 2 * capn : 1024;
                        void *np = realloc(next, nc * sizeof *next);
                        if (!np) {
                            free(next);
                            goto oom;
                        }
                        next = np, capn = nc;
                    }
                    next[nnext++] = k;
                }
        }
        free(cand);
        cand = next, ncand = nnext, capc = capn;
    }
    free(cand), free(thin);
    return true;
oom:
    free(cand), free(thin);
    return false;
}

/* strong 2:1 balance: across faces, edges and vertices neighbouring leaves differ by at most one level */
static bool octree_balance(Octree *o) {
    size_t *mark = NULL, nmark = 0, capm = 0;
    for (;;) {
        nmark = 0;
        for (size_t i = 0; i < o->n; i++) {
            Leaf L = o->leaf[i];
            if (!L.s) continue;
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy && !dz) continue;
                        int x = dx > 0 ? L.x + L.s : (dx < 0 ? L.x - 1 : L.x);
                        int y = dy > 0 ? L.y + L.s : (dy < 0 ? L.y - 1 : L.y);
                        int z = dz > 0 ? L.z + L.s : (dz < 0 ? L.z - 1 : L.z);
                        int n = find_leaf(o, x, y, z);
                        if (n >= 0 && o->leaf[n].s > 2 * L.s) {
                            GROW(mark, nmark, capm, oom);
                            mark[nmark++] = (size_t)n;
                        }
                    }
        }
        if (!nmark) break;
        for (size_t k = 0; k < nmark; k++)
            if (o->leaf[mark[k]].s && !split_leaf(o, mark[k])) goto oom;
    }
    free(mark);
    return true;
oom:
    free(mark);
    return false;
}

/* ---- the background mesh: lattice vertices in doubled finest units ------------------------------------------ */

static uint64_t vkey(const int v[3]) { return ((uint64_t)v[0] << 42) | ((uint64_t)v[1] << 21) | (uint64_t)v[2]; }

typedef void (*BgTetFn)(void *ctx, const int v[4][3]);

typedef struct {
    const Octree *o;
    const HMap *corners; /* doubled corner coordinates of every leaf */
} Bg;

static bool is_corner(const Bg *bg, const int p[3]) { return hm_get(bg->corners, vkey(p)) >= 0; }

static void emit4(BgTetFn fn, void *ctx, const int a[3], const int b[3], const int c[3], const int d[3]) {
    int v[4][3];
    memcpy(v[0], a, sizeof v[0]), memcpy(v[1], b, sizeof v[1]), memcpy(v[2], c, sizeof v[2]), memcpy(v[3], d, sizeof v[3]);
    fn(ctx, v);
}

/* the background tetrahedra that belong to leaf li */
static void leaf_tets(const Bg *bg, size_t li, BgTetFn fn, void *ctx) {
    const Octree *o = bg->o;
    Leaf L = o->leaf[li];
    int S = 2 * L.s, s = L.s;
    int base[3] = {2 * L.x, 2 * L.y, 2 * L.z};
    int C[3] = {base[0] + s, base[1] + s, base[2] + s};
    int lo[3] = {L.x, L.y, L.z};
    for (int a = 0; a < 3; a++)
        for (int side = -1; side <= 1; side += 2) {
            int b = (a + 1) % 3, c = (a + 2) % 3;
            int q[4][3];
            static const int UV[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int k = 0; k < 4; k++) {
                q[k][a] = base[a] + (side > 0 ? S : 0);
                q[k][b] = base[b] + UV[k][0] * S;
                q[k][c] = base[c] + UV[k][1] * S;
            }
            int F[3] = {C[0], C[1], C[2]};
            F[a] += side * s;
            int nc[3] = {lo[0], lo[1], lo[2]};
            nc[a] = side > 0 ? lo[a] + L.s : lo[a] - 1;
            int N = find_leaf(o, nc[0], nc[1], nc[2]);
            int ns = N >= 0 ? o->leaf[N].s : 0;
            if (N >= 0 && ns > s) {
                /* the larger leaf's face centre is one corner of this face; split along the diagonal from it */
                Leaf B = o->leaf[N];
                int BF[3] = {2 * B.x + B.s, 2 * B.y + B.s, 2 * B.z + B.s};
                BF[a] = q[0][a];
                int k0 = -1;
                for (int k = 0; k < 4; k++)
                    if (q[k][0] == BF[0] && q[k][1] == BF[1] && q[k][2] == BF[2]) k0 = k;
                if (k0 < 0) k0 = 0; /* cannot happen in a 2:1 tree */
                emit4(fn, ctx, C, q[k0], q[(k0 + 1) % 4], q[(k0 + 2) % 4]);
                emit4(fn, ctx, C, q[k0], q[(k0 + 2) % 4], q[(k0 + 3) % 4]);
                continue;
            }
            if (N >= 0 && ns == s && side < 0) continue; /* the BCC tetrahedra of this face belong to the other leaf */
            int CN[3] = {C[0], C[1], C[2]};
            CN[a] += side * S;
            for (int k = 0; k < 4; k++) {
                const int *p = q[k], *r = q[(k + 1) % 4];
                int m[3] = {(p[0] + r[0]) / 2, (p[1] + r[1]) / 2, (p[2] + r[2]) / 2};
                int segs[2][2][3], nseg = 0;
                if (S >= 4 && is_corner(bg, m)) {
                    memcpy(segs[0][0], p, sizeof(int) * 3), memcpy(segs[0][1], m, sizeof(int) * 3);
                    memcpy(segs[1][0], m, sizeof(int) * 3), memcpy(segs[1][1], r, sizeof(int) * 3);
                    nseg = 2;
                } else {
                    memcpy(segs[0][0], p, sizeof(int) * 3), memcpy(segs[0][1], r, sizeof(int) * 3);
                    nseg = 1;
                }
                for (int g = 0; g < nseg; g++) {
                    if (N >= 0 && ns == s) emit4(fn, ctx, C, CN, segs[g][0], segs[g][1]);
                    else emit4(fn, ctx, C, F, segs[g][0], segs[g][1]);
                }
            }
        }
}

/* ---- the generator --------------------------------------------------------------------------------------------- */

typedef struct {
    int a, b;         /* lattice vertex ids: a inside, b outside */
    double t;         /* crossing parameter from a */
    double p[3];
    int out;          /* output node id, -1 until used */
} Cut;

typedef struct {
    int v, cut;
    double dist;
} Cand;

typedef struct {
    /* lattice */
    HMap vmap;
    int (*vc)[3];
    size_t nv, capv;
    double *vp, *vf, *vf0; /* positions, values, values before warping */
    unsigned char *warped;
    int *vout;
    /* cuts */
    HMap emap; /* (a,b) -> cut index */
    Cut *cut;
    size_t ncut, capc;
    Cand *cand;
    size_t ncand, capd;
    /* output */
    double *xyz;
    size_t nout, capo;
    int *tet;
    size_t ntet, capt;
    int steiner, bad_stencils;
    double h, unit, origin[3];
    const TetMeshSettings *s;
    double zero_tol;
    bool oom;
    long long bg;
} Gen;

static int sgn(const Gen *g, int v) {
    double f = g->vf[v];
    return f < 0 ? -1 : (f > 0 ? 1 : 0);
}

static uint64_t ekey(int a, int b) {
    if (a > b) {
        int t = a;
        a = b, b = t;
    }
    return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b;
}

static int lattice_id(Gen *g, const int c[3]) {
    uint64_t k = vkey(c);
    int id = hm_get(&g->vmap, k);
    if (id >= 0) return id;
    if (g->nv >= g->capv) {
        size_t nc = g->capv ? 2 * g->capv : 4096;
        void *p = realloc(g->vc, nc * sizeof *g->vc);
        if (!p) {
            g->oom = true;
            return -1;
        }
        g->vc = p, g->capv = nc;
    }
    memcpy(g->vc[g->nv], c, sizeof g->vc[0]);
    if (!hm_put(&g->vmap, k, (int)g->nv)) {
        g->oom = true;
        return -1;
    }
    return (int)g->nv++;
}

static void pass_collect(void *ctx, const int v[4][3]) {
    Gen *g = ctx;
    g->bg++;
    for (int k = 0; k < 4; k++) lattice_id(g, v[k]);
}

static bool axis_aligned(const int *a, const int *b) {
    int same = (a[0] == b[0]) + (a[1] == b[1]) + (a[2] == b[2]);
    return same >= 2;
}

static void pass_edges(void *ctx, const int v[4][3]) {
    Gen *g = ctx;
    int id[4];
    for (int k = 0; k < 4; k++) id[k] = hm_get(&g->vmap, vkey(v[k]));
    for (int e = 0; e < 6; e++) {
        int a = id[TETMESH_EDGE[e][0]], b = id[TETMESH_EDGE[e][1]];
        int sa = sgn(g, a), sb = sgn(g, b);
        if (!(sa * sb < 0)) continue;
        uint64_t k = ekey(a, b);
        if (hm_get(&g->emap, k) >= 0) continue;
        if (g->ncut >= g->capc) {
            size_t nc = g->capc ? 2 * g->capc : 4096;
            void *p = realloc(g->cut, nc * sizeof *g->cut);
            if (!p) {
                g->oom = true;
                return;
            }
            g->cut = p, g->capc = nc;
        }
        Cut *c = &g->cut[g->ncut];
        c->a = sa < 0 ? a : b, c->b = sa < 0 ? b : a, c->t = 0.5, c->out = -1;
        if (!hm_put(&g->emap, k, (int)g->ncut)) {
            g->oom = true;
            return;
        }
        g->ncut++;
    }
}

static void cut_range(void *vctx, int begin, int end, int tid) {
    (void)tid;
    Gen *g = vctx;
    const TetMeshSettings *s = g->s;
    for (int i = begin; i < end; i++) {
        Cut *c = &g->cut[i];
        const double *pa = g->vp + 3 * (size_t)c->a, *pb = g->vp + 3 * (size_t)c->b;
        double t = -1;
        if (s->cross && s->cross(s->ctx, pa, pb, &t) && t > 0 && t < 1) {
            /* exact crossing */
        } else {
            /* Illinois regula falsi on f along the edge, bracketed by the two signs */
            double t0 = 0, t1 = 1, f0 = g->vf[c->a], f1 = g->vf[c->b];
            int side = 0;
            t = 0.5;
            for (int it = 0; it < 60; it++) {
                t = (t0 * f1 - t1 * f0) / (f1 - f0);
                if (!(t > t0 && t < t1)) t = 0.5 * (t0 + t1);
                double p[3] = {pa[0] + t * (pb[0] - pa[0]), pa[1] + t * (pb[1] - pa[1]), pa[2] + t * (pb[2] - pa[2])};
                double ft = s->sdf(s->ctx, p);
                if (fabs(ft) <= 1e-4 * g->zero_tol || t1 - t0 < 1e-15) break;
                if (ft < 0) {
                    t0 = t, f0 = ft;
                    if (side == -1) f1 *= 0.5;
                    side = -1;
                } else {
                    t1 = t, f1 = ft;
                    if (side == 1) f0 *= 0.5;
                    side = 1;
                }
            }
        }
        c->t = t;
        for (int q = 0; q < 3; q++) c->p[q] = pa[q] + t * (pb[q] - pa[q]);
    }
}

static int cand_cmp(const void *x, const void *y) {
    const Cand *a = x, *b = y;
    if (a->v != b->v) return a->v < b->v ? -1 : 1;
    return a->dist < b->dist ? -1 : (a->dist > b->dist);
}

/* ---- output ------------------------------------------------------------------------------------------------- */

static int out_lattice(Gen *g, int v) {
    if (g->vout[v] >= 0) return g->vout[v];
    if (g->nout >= g->capo) {
        size_t nc = g->capo ? 2 * g->capo : 4096;
        void *p = realloc(g->xyz, 3 * nc * sizeof(double));
        if (!p) {
            g->oom = true;
            return -1;
        }
        g->xyz = p, g->capo = nc;
    }
    memcpy(g->xyz + 3 * g->nout, g->vp + 3 * (size_t)v, 3 * sizeof(double));
    return g->vout[v] = (int)g->nout++;
}

static int out_point(Gen *g, const double p[3]) {
    if (g->nout >= g->capo) {
        size_t nc = g->capo ? 2 * g->capo : 4096;
        void *q = realloc(g->xyz, 3 * nc * sizeof(double));
        if (!q) {
            g->oom = true;
            return -1;
        }
        g->xyz = q, g->capo = nc;
    }
    memcpy(g->xyz + 3 * g->nout, p, 3 * sizeof(double));
    return (int)g->nout++;
}

static int out_cut(Gen *g, int a, int b) {
    int ci = hm_get(&g->emap, ekey(a, b));
    if (ci < 0) {
        g->oom = true; /* a sign change without a cut point cannot happen; treat as a failure */
        return -1;
    }
    Cut *c = &g->cut[ci];
    if (c->out < 0) c->out = out_point(g, c->p);
    return c->out;
}

static void out_tet(Gen *g, int a, int b, int c, int d) {
    if (a < 0 || b < 0 || c < 0 || d < 0) {
        g->oom = true;
        return;
    }
    if (g->ntet >= g->capt) {
        size_t nc = g->capt ? 2 * g->capt : 4096;
        void *p = realloc(g->tet, 4 * nc * sizeof(int));
        if (!p) {
            g->oom = true;
            return;
        }
        g->tet = p, g->capt = nc;
    }
    int *t = g->tet + 4 * g->ntet++;
    double X[4][3];
    int id[4] = {a, b, c, d};
    for (int k = 0; k < 4; k++) memcpy(X[k], g->xyz + 3 * (size_t)id[k], sizeof X[k]);
    if (tet_signed_volume(X) < 0) {
        int tmp = id[2];
        id[2] = id[3], id[3] = tmp;
    }
    memcpy(t, id, sizeof id);
}

static double pd(const Gen *g, int a, int b) {
    double d[3];
    sub3(g->xyz + 3 * (size_t)a, g->xyz + 3 * (size_t)b, d);
    return norm3(d);
}

/* the diagonal of a planar quadrilateral (w0 w1 w2 w3, cyclic): 0 joins w0-w2, 1 joins w1-w3. Shorter diagonal, ties
 * by the smallest node id; the rule sees only the face, so the two elements sharing it agree. */
static int quad_diag(const Gen *g, const int w[4]) {
    double l0 = pd(g, w[0], w[2]), l1 = pd(g, w[1], w[3]);
    if (fabs(l0 - l1) > 1e-9 * (l0 + l1)) return l0 < l1 ? 0 : 1;
    int m0 = w[0] < w[2] ? w[0] : w[2], m1 = w[1] < w[3] ? w[1] : w[3];
    return m0 < m1 ? 0 : 1;
}

static void quad_tris(const int w[4], int d, int t[2][3]) {
    if (d == 0) {
        t[0][0] = w[0], t[0][1] = w[1], t[0][2] = w[2];
        t[1][0] = w[0], t[1][1] = w[2], t[1][2] = w[3];
    } else {
        t[0][0] = w[1], t[0][1] = w[2], t[0][2] = w[3];
        t[1][0] = w[1], t[1][1] = w[3], t[1][2] = w[0];
    }
}

static double tets_min_angle(const Gen *g, int (*t)[4], int n) {
    double worst = 180;
    for (int i = 0; i < n; i++) {
        double X[4][3], deg[6];
        for (int k = 0; k < 4; k++) memcpy(X[k], g->xyz + 3 * (size_t)t[i][k], sizeof X[k]);
        if (fabs(tet_signed_volume(X)) <= 0) return -1;
        tet_dihedral_angles(X, deg);
        for (int e = 0; e < 6; e++) worst = fmin(worst, deg[e]);
    }
    return worst;
}

/* a triangular prism: bottom P0 P1 P2, top P3 P4 P5 (Pi over Pi+3). Its boundary, consistently oriented: bottom
 * (P0 P1 P2), top (P3 P5 P4), lateral quadrilaterals (P1 P0 P3 P4), (P2 P1 P4 P5), (P0 P2 P5 P3). fixed[q] = 1 when
 * quadrilateral q lies on a face shared with a neighbour and takes its diagonal from quad_diag. A decomposition is a
 * cone from one vertex over the boundary triangles that do not contain it; it is accepted only if every cone
 * tetrahedron has the same orientation (the vertex sees every face from inside), so nothing overlaps. */
static bool cone_ok(const Gen *g, int (*tt)[4], int n, double *minang) {
    int sign = 0;
    for (int i = 0; i < n; i++) {
        double X[4][3];
        for (int k = 0; k < 4; k++) memcpy(X[k], g->xyz + 3 * (size_t)tt[i][k], sizeof X[k]);
        double V = tet_signed_volume(X), L = pd(g, tt[i][0], tt[i][1]) + pd(g, tt[i][2], tt[i][3]);
        if (!(fabs(V) > 1e-12 * L * L * L)) return false;
        int sg = V > 0 ? 1 : -1;
        if (sign && sg != sign) return false;
        sign = sg;
    }
    *minang = tets_min_angle(g, tt, n);
    return *minang > 0;
}

static int prism_tris(const int P[6], const int d[3], int tris[8][3]) {
    int Q[3][4] = {{P[1], P[0], P[3], P[4]}, {P[2], P[1], P[4], P[5]}, {P[0], P[2], P[5], P[3]}};
    int nt = 0;
    tris[nt][0] = P[0], tris[nt][1] = P[1], tris[nt][2] = P[2], nt++;
    tris[nt][0] = P[3], tris[nt][1] = P[5], tris[nt][2] = P[4], nt++;
    for (int q = 0; q < 3; q++) {
        int t2[2][3];
        quad_tris(Q[q], d[q], t2);
        memcpy(tris[nt++], t2[0], sizeof t2[0]);
        memcpy(tris[nt++], t2[1], sizeof t2[1]);
    }
    return nt;
}

static void prism(Gen *g, const int P[6], const bool fixed[3]) {
    int Q[3][4] = {{P[1], P[0], P[3], P[4]}, {P[2], P[1], P[4], P[5]}, {P[0], P[2], P[5], P[3]}};
    int dfix[3];
    for (int q = 0; q < 3; q++) dfix[q] = fixed[q] ? quad_diag(g, Q[q]) : -1;
    int best[8][4], nbest = 0, bestd[3] = {0, 0, 0};
    double bestang = -1;
    for (int combo = 0; combo < 8; combo++) {
        int d[3];
        bool okc = true;
        for (int q = 0; q < 3; q++) {
            d[q] = (combo >> q) & 1;
            if (dfix[q] >= 0 && d[q] != dfix[q]) okc = false;
        }
        if (!okc) continue;
        int tris[8][3];
        int nt = prism_tris(P, d, tris);
        for (int v = 0; v < 6; v++) {
            int pv = P[v], tt[8][4], n = 0;
            for (int i = 0; i < nt; i++) {
                if (tris[i][0] == pv || tris[i][1] == pv || tris[i][2] == pv) continue;
                tt[n][0] = pv, tt[n][1] = tris[i][0], tt[n][2] = tris[i][1], tt[n][3] = tris[i][2];
                n++;
            }
            if (n != 3) continue; /* a quadrilateral at pv is not split through pv: a flat tetrahedron would appear */
            double ang;
            if (cone_ok(g, tt, n, &ang) && ang > bestang) {
                bestang = ang, nbest = n;
                memcpy(best, tt, sizeof(int) * 4 * (size_t)n);
                memcpy(bestd, d, sizeof bestd);
            }
        }
    }
    (void)bestd;
    if (nbest > 0) {
        for (int i = 0; i < nbest; i++) out_tet(g, best[i][0], best[i][1], best[i][2], best[i][3]);
        return;
    }
    /* no vertex sees the whole boundary (the constrained diagonals form a cycle, or warping bent the prism): a Steiner
     * vertex at the centroid, eight tetrahedra */
    double c[3] = {0, 0, 0};
    for (int k = 0; k < 6; k++)
        for (int q = 0; q < 3; q++) c[q] += g->xyz[3 * (size_t)P[k] + q] / 6;
    int sc = out_point(g, c);
    if (sc < 0) return;
    g->steiner++;
    int d[3];
    for (int q = 0; q < 3; q++) d[q] = dfix[q] >= 0 ? dfix[q] : quad_diag(g, Q[q]);
    int tris[8][3], tt[8][4];
    int nt = prism_tris(P, d, tris);
    for (int i = 0; i < nt; i++) tt[i][0] = sc, tt[i][1] = tris[i][0], tt[i][2] = tris[i][1], tt[i][3] = tris[i][2];
    double ang;
    if (!cone_ok(g, tt, nt, &ang)) g->bad_stencils++;
    for (int i = 0; i < nt; i++) out_tet(g, tt[i][0], tt[i][1], tt[i][2], tt[i][3]);
}

/* after warping: a background tetrahedron that warping flattened (below 5 % of its lattice volume) or turned over gets
 * its warped vertices back. The paper's no-inversion argument covers the pure BCC lattice; the transition cones here
 * are not covered by it, so this is checked instead of assumed. */
static void pass_unwarp(void *ctx, const int v[4][3]) {
    Gen *g = ctx;
    int id[4];
    double X[4][3], X0[4][3];
    bool any = false;
    for (int k = 0; k < 4; k++) {
        id[k] = hm_get(&g->vmap, vkey(v[k]));
        memcpy(X[k], g->vp + 3 * (size_t)id[k], sizeof X[k]);
        for (int q = 0; q < 3; q++) X0[k][q] = g->origin[q] + 0.5 * g->unit * v[k][q];
        any |= g->warped[id[k]] == 1;
    }
    if (!any) return;
    double V0 = tet_signed_volume(X0), V = tet_signed_volume(X);
    if (V / V0 >= 0.05) return;
    for (int k = 0; k < 4; k++)
        if (g->warped[id[k]] == 1) g->warped[id[k]] = 2; /* marked: restored after the sweep */
}

static void pass_stencil(void *ctx, const int v[4][3]) {
    Gen *g = ctx;
    int id[4], in[4], zr[4], ou[4], ni = 0, nz = 0, no = 0;
    for (int k = 0; k < 4; k++) {
        id[k] = hm_get(&g->vmap, vkey(v[k]));
        int sg = sgn(g, id[k]);
        if (sg < 0) in[ni++] = id[k];
        else if (sg == 0) zr[nz++] = id[k];
        else ou[no++] = id[k];
    }
    if (ni == 0) return;
    if (no == 0) {
        out_tet(g, out_lattice(g, id[0]), out_lattice(g, id[1]), out_lattice(g, id[2]), out_lattice(g, id[3]));
        return;
    }
    if (ni == 1) {
        int i = out_lattice(g, in[0]);
        if (nz == 0) out_tet(g, i, out_cut(g, in[0], ou[0]), out_cut(g, in[0], ou[1]), out_cut(g, in[0], ou[2]));
        else if (nz == 1) out_tet(g, i, out_lattice(g, zr[0]), out_cut(g, in[0], ou[0]), out_cut(g, in[0], ou[1]));
        else out_tet(g, i, out_lattice(g, zr[0]), out_lattice(g, zr[1]), out_cut(g, in[0], ou[0]));
        return;
    }
    if (ni == 2 && no == 2) {
        int i1 = out_lattice(g, in[0]), i2 = out_lattice(g, in[1]);
        int c11 = out_cut(g, in[0], ou[0]), c12 = out_cut(g, in[0], ou[1]);
        int c21 = out_cut(g, in[1], ou[0]), c22 = out_cut(g, in[1], ou[1]);
        /* bottom (i1 c11 c12), top (i2 c21 c22): quad 0 lies on face (i1 i2 o1), quad 2 on (i1 i2 o2), quad 1 inside */
        int P[6] = {i1, c11, c12, i2, c21, c22};
        bool fx[3] = {true, false, true};
        prism(g, P, fx);
        return;
    }
    if (ni == 2 && nz == 1) {
        int i1 = out_lattice(g, in[0]), i2 = out_lattice(g, in[1]), z = out_lattice(g, zr[0]);
        int c1 = out_cut(g, in[0], ou[0]), c2 = out_cut(g, in[1], ou[0]);
        int w[4] = {i1, i2, c2, c1}, t2[2][3];
        quad_tris(w, quad_diag(g, w), t2);
        int tt[2][4] = {{z, t2[0][0], t2[0][1], t2[0][2]}, {z, t2[1][0], t2[1][1], t2[1][2]}};
        double ang;
        if (!cone_ok(g, tt, 2, &ang)) g->bad_stencils++;
        out_tet(g, tt[0][0], tt[0][1], tt[0][2], tt[0][3]);
        out_tet(g, tt[1][0], tt[1][1], tt[1][2], tt[1][3]);
        return;
    }
    if (ni == 3) {
        int i1 = out_lattice(g, in[0]), i2 = out_lattice(g, in[1]), i3 = out_lattice(g, in[2]);
        int c1 = out_cut(g, in[0], ou[0]), c2 = out_cut(g, in[1], ou[0]), c3 = out_cut(g, in[2], ou[0]);
        int P[6] = {i1, i2, i3, c1, c2, c3};
        bool fx[3] = {true, true, true};
        prism(g, P, fx);
        return;
    }
}


/* ---- boundary, regions, quality ------------------------------------------------------------------------------- */

typedef struct {
    int n[3];
    int elem;
    unsigned char local;
} FaceRec;

static int face_cmp(const void *a, const void *b) {
    const FaceRec *x = a, *y = b;
    for (int k = 0; k < 3; k++)
        if (x->n[k] != y->n[k]) return x->n[k] < y->n[k] ? -1 : 1;
    return x->elem - y->elem;
}

static int pair_cmp(const void *a, const void *b) {
    const int *x = a, *y = b;
    if (x[0] != y[0]) return x[0] < y[0] ? -1 : 1;
    return (x[1] > y[1]) - (x[1] < y[1]);
}

static int uf_find(int *p, int x) {
    while (p[x] != x) p[x] = p[p[x]], x = p[x];
    return x;
}

/* TET10 shape function derivatives with respect to (xi, eta, zeta) at volume coordinates L (L[0] = 1 - xi - eta - zeta) */
static void tet10_dshape(const double L[4], double dN[10][3]) {
    static const double dL[4][3] = {{-1, -1, -1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++) dN[i][k] = (4 * L[i] - 1) * dL[i][k];
    for (int e = 0; e < 6; e++) {
        int a = TETMESH_EDGE[e][0], b = TETMESH_EDGE[e][1];
        for (int k = 0; k < 3; k++) dN[4 + e][k] = 4 * (L[a] * dL[b][k] + L[b] * dL[a][k]);
    }
}

static double tet10_detj(const double X[10][3], const double L[4]) {
    double dN[10][3], J[3][3] = {{0}};
    tet10_dshape(L, dN);
    for (int n = 0; n < 10; n++)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) J[i][j] += X[n][i] * dN[n][j];
    return J[0][0] * (J[1][1] * J[2][2] - J[1][2] * J[2][1]) - J[0][1] * (J[1][0] * J[2][2] - J[1][2] * J[2][0]) +
           J[0][2] * (J[1][0] * J[2][1] - J[1][1] * J[2][0]);
}

/* the check points of a curved element: the 4 quadrature points, the corners and the edge midpoints */
static bool tet10_valid(const double X[10][3], double straight_detj) {
    static const double a = 0.5854101966249685, b = 0.1381966011250105;
    double pts[14][4] = {{a, b, b, b}, {b, a, b, b}, {b, b, a, b}, {b, b, b, a}, {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    int np = 8;
    for (int e = 0; e < 6; e++) {
        double *p = pts[np++];
        memset(p, 0, 4 * sizeof(double));
        p[TETMESH_EDGE[e][0]] = p[TETMESH_EDGE[e][1]] = 0.5;
    }
    for (int i = 0; i < np; i++)
        if (!(tet10_detj(X, pts[i]) >= 0.2 * straight_detj)) return false;
    return true;
}

static double tet10_volume(const double X[10][3]) {
    /* degree-3 rule: centroid -4/5, (1/2,1/6,1/6,1/6) and permutations 9/20 (weights sum to 1), times 1/6 */
    double v = -0.8 * tet10_detj(X, (double[4]){0.25, 0.25, 0.25, 0.25});
    for (int k = 0; k < 4; k++) {
        double L[4] = {1.0 / 6, 1.0 / 6, 1.0 / 6, 1.0 / 6};
        L[k] = 0.5;
        v += 0.45 * tet10_detj(X, L);
    }
    return v / 6.0;
}

/* ---- quality improvement ---------------------------------------------------------------------------------------
 * Isosurface stuffing as implemented here leaves some slivers the paper's parity rule would have avoided (section 1 of
 * the contract). They are improved by node smoothing: a node incident to an element below 15 degrees is moved to the
 * candidate position (the centroid of its neighbours, or a step along an axis) that most raises the smallest of its
 * elements' dihedral angles and of their supplements (so large angles count too), and only if it raises it; surface nodes are projected back onto the surface after every move.
 * Topology does not change, so conformity is untouched and no element can invert (a move that would is rejected). */

static double tet_min_angle_xyz(const double *xyz, const int *t) {
    double X[4][3], deg[6];
    for (int k = 0; k < 4; k++) memcpy(X[k], xyz + 3 * (size_t)t[k], sizeof X[k]);
    if (!(tet_signed_volume(X) > 0)) return -1;
    tet_dihedral_angles(X, deg);
    double m = 180;
    for (int k = 0; k < 6; k++) m = fmin(m, fmin(deg[k], 180 - deg[k])); /* both ends: slivers and caps */
    return m;
}

static double node_quality(const double *xyz, const int *tet, const int *adj, int n0, int n1) {
    double q = 180;
    for (int i = n0; i < n1; i++) {
        double a = tet_min_angle_xyz(xyz, tet + 4 * (size_t)adj[i]);
        if (a < q) q = a;
        if (q < 0) return -1;
    }
    return q;
}

static bool improve_quality(Gen *g, TetMesh *out, const TetMeshSettings *s) {
    size_t nn = g->nout, ne = g->ntet;
    int *start = calloc(nn + 1, sizeof(int)), *adj = malloc(4 * ne * sizeof(int));
    unsigned char *bnd = calloc(nn, 1);
    if (!start || !adj || !bnd) {
        free(start), free(adj), free(bnd);
        return false;
    }
    for (size_t e = 0; e < ne; e++)
        for (int k = 0; k < 4; k++) start[g->tet[4 * e + k] + 1]++;
    for (size_t i = 0; i < nn; i++) start[i + 1] += start[i];
    int *fill = malloc(nn * sizeof(int));
    if (!fill) {
        free(start), free(adj), free(bnd);
        return false;
    }
    memcpy(fill, start, nn * sizeof(int));
    for (size_t e = 0; e < ne; e++)
        for (int k = 0; k < 4; k++) adj[fill[g->tet[4 * e + k]]++] = (int)e;
    free(fill);
    for (int f = 0; f < out->nfaces; f++)
        for (int q = 0; q < 3; q++) bnd[g->tet[4 * (size_t)out->face_elem[f] + TETMESH_FACE[out->face_local[f]][q]]] = 1;
    const double target = 15.0;
    int moved = 0;
    for (int sweep = 0; sweep < 8; sweep++) {
        int moved_now = 0;
        for (size_t v = 0; v < nn; v++) {
            double q0 = node_quality(g->xyz, g->tet, adj, start[v], start[v + 1]);
            if (q0 >= target || start[v] == start[v + 1]) continue;
            double *p = g->xyz + 3 * v, orig[3] = {p[0], p[1], p[2]}, cen[3] = {0, 0, 0}, len = 0;
            int cnt = 0;
            for (int i = start[v]; i < start[v + 1]; i++)
                for (int k = 0; k < 4; k++) {
                    int w = g->tet[4 * (size_t)adj[i] + k];
                    if ((size_t)w == v) continue;
                    for (int d = 0; d < 3; d++) cen[d] += g->xyz[3 * (size_t)w + d];
                    len += pd(g, (int)v, w);
                    cnt++;
                }
            if (!cnt) continue;
            for (int d = 0; d < 3; d++) cen[d] /= cnt;
            len /= cnt;
            double best[3] = {orig[0], orig[1], orig[2]}, bq = q0;
            for (int it = 0; it < 4; it++) {
                double step = len * 0.2 / (1 << it);
                double cands[8][3];
                int nc = 0;
                for (int d = 0; d < 3; d++) cands[nc][d] = best[d] + 0.5 * (cen[d] - best[d]);
                nc++;
                for (int ax = 0; ax < 3; ax++)
                    for (int sg = -1; sg <= 1; sg += 2) {
                        memcpy(cands[nc], best, sizeof best);
                        cands[nc][ax] += sg * step;
                        nc++;
                    }
                for (int c = 0; c < nc; c++) {
                    double q[3];
                    if (bnd[v]) s->project(s->ctx, cands[c], q);
                    else memcpy(q, cands[c], sizeof q);
                    memcpy(p, q, sizeof q);
                    double nq = node_quality(g->xyz, g->tet, adj, start[v], start[v + 1]);
                    if (nq > bq + 1e-9) {
                        bq = nq;
                        memcpy(best, q, sizeof q);
                    }
                }
                memcpy(p, best, sizeof best);
            }
            if (bq > q0 + 1e-9) moved_now++;
            else memcpy(p, orig, sizeof orig);
        }
        moved += moved_now;
        if (!moved_now) break;
    }
    out->smoothed_nodes = moved;
    free(start), free(adj), free(bnd);
    return true;
}

void tetmesh_free(TetMesh *m) {
    free(m->xyz), free(m->conn), free(m->face_elem), free(m->face_local);
    memset(m, 0, sizeof *m);
}

bool tetmesh_generate(const TetMeshSettings *s, TetMesh *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    double t0 = now_seconds();
    double h = s->surface_size;
    if (!s->sdf || !(h > 0)) {
        snprintf(err, errlen, "a tetrahedral mesh needs a surface and a positive surface_size");
        return false;
    }
    int order = s->order == 2 ? 2 : 1;
    if (s->order != 1 && s->order != 2) {
        snprintf(err, errlen, "order must be 1 (TET4) or 2 (TET10), not %d", s->order);
        return false;
    }
    if (order == 2 && !s->project) {
        snprintf(err, errlen, "TET10 needs a projection onto the surface for its curved edges");
        return false;
    }
    double isize = s->interior_size > 0 ? s->interior_size : 4 * h;
    if (isize < h * (1 - 1e-12)) {
        snprintf(err, errlen, "interior_size (%.4g m) is smaller than surface_size (%.4g m)", isize, h);
        return false;
    }
    int thin_levels = s->thin_levels < 0 ? 0 : (s->thin_levels == 0 ? 2 : s->thin_levels);
    int surface_units = 1 << thin_levels;
    int interior_units = surface_units;
    while (2 * interior_units * h / surface_units <= isize * (1 + 1e-12)) interior_units *= 2;
    double ext = 0;
    for (int k = 0; k < 3; k++) {
        if (!(s->hi[k] >= s->lo[k])) {
            snprintf(err, errlen, "the bounding box is empty");
            return false;
        }
        ext = fmax(ext, s->hi[k] - s->lo[k]);
    }
    Octree o = {0};
    o.h = h / surface_units; /* the finest cell */
    o.root = surface_units;
    while (o.root * o.h < ext + 4 * h) o.root *= 2;
    o.depth = ilog2(o.root);
    if (o.depth > 18) {
        snprintf(err, errlen, "the part is %.4g m across, %d surface cells: too many for the octree (surface_size too small)", ext,
                 (int)ceil(ext / h));
        return false;
    }
    for (int k = 0; k < 3; k++) o.origin[k] = s->lo[k] - 2 * h;
    Gen g = {0};
    g.h = h, g.s = s;
    g.zero_tol = 1e-9 * ext;
    HMap corners = {0};
    bool ok = false;
    int *regp = NULL;
    FaceRec *fr = NULL;
    if (!hm_init(&o.map, 1024) || !add_leaf(&o, 0, 0, 0, o.root)) goto oom;
    int nthin = 0, unresolved = 0;
    if (!octree_refine(&o, s, surface_units, interior_units)) goto oom;
    /* always run: with thin_levels 0 the pass only counts the walls the finest cell cannot resolve */
    if (!octree_thin(&o, s, surface_units, &nthin, &unresolved)) goto oom;
    out->unresolved_thin = unresolved;
    out->finest_cell = o.h;
    if (!octree_balance(&o)) goto oom;
    out->thin_splits = nthin;
    int round = 0;
restuff:;
    size_t nleaves = 0;
    if (!hm_init(&corners, 8 * 1024)) goto oom;
    for (size_t i = 0; i < o.n; i++) {
        Leaf L = o.leaf[i];
        if (!L.s) continue;
        nleaves++;
        for (int c = 0; c < 8; c++) {
            int p[3] = {2 * (L.x + (c & 1 ? L.s : 0)), 2 * (L.y + (c & 2 ? L.s : 0)), 2 * (L.z + (c & 4 ? L.s : 0))};
            if (!hm_put(&corners, vkey(p), 1)) goto oom;
        }
    }
    out->octree_leaves = (int)nleaves;
    out->octree_depth = o.depth;
    Bg bg = {&o, &corners};
    /* pass 0: lattice vertices and their values */
    if (!hm_init(&g.vmap, 16 * nleaves)) goto oom;
    for (size_t i = 0; i < o.n && !g.oom; i++)
        if (o.leaf[i].s) leaf_tets(&bg, i, pass_collect, &g);
    if (g.oom) goto oom;
    out->background_tets = (int)g.bg;
    out->lattice_vertices = (int)g.nv;
    if (s->max_elements && (uint64_t)g.bg > 4 * s->max_elements) {
        snprintf(err, errlen, "the background lattice has %lld tetrahedra; the mesh would exceed the limit of %llu elements: increase surface_size",
                 g.bg, (unsigned long long)s->max_elements);
        goto fail;
    }
    g.vp = malloc(3 * g.nv * sizeof(double));
    g.vf = malloc(g.nv * sizeof(double));
    g.warped = calloc(g.nv ? g.nv : 1, 1);
    g.vout = malloc((g.nv ? g.nv : 1) * sizeof(int));
    if (!g.vp || !g.vf || !g.warped || !g.vout) goto oom;
    for (size_t i = 0; i < g.nv; i++) {
        for (int k = 0; k < 3; k++) g.vp[3 * i + k] = o.origin[k] + 0.5 * o.h * g.vc[i][k];
        g.vout[i] = -1;
    }
    sdf_many(s, g.vp, g.vf, (int)g.nv);
    for (size_t i = 0; i < g.nv; i++)
        if (fabs(g.vf[i]) <= g.zero_tol) g.vf[i] = 0;
    g.vf0 = malloc(g.nv * sizeof(double));
    if (!g.vf0) goto oom;
    memcpy(g.vf0, g.vf, g.nv * sizeof(double));
    /* pass 1: cut points on every edge whose ends have strict opposite signs */
    if (!hm_init(&g.emap, g.nv / 4 + 1024)) goto oom;
    for (size_t i = 0; i < o.n && !g.oom; i++)
        if (o.leaf[i].s) leaf_tets(&bg, i, pass_edges, &g);
    if (g.oom) goto oom;
    if (s->pool && g.ncut > 64) pool_for(s->pool, (int)g.ncut, 32, cut_range, &g);
    else cut_range(&g, 0, (int)g.ncut, 0);
    out->cut_points = (int)g.ncut;
    /* warping: candidates are cut points closer than alpha x edge length to a lattice vertex */
    for (size_t i = 0; i < g.ncut; i++) {
        Cut *c = &g.cut[i];
        double e[3];
        sub3(g.vp + 3 * (size_t)c->b, g.vp + 3 * (size_t)c->a, e);
        double len = norm3(e);
        double alpha = axis_aligned(g.vc[c->a], g.vc[c->b]) ? ALPHA_LONG : ALPHA_SHORT;
        for (int end = 0; end < 2; end++) {
            double frac = end == 0 ? c->t : 1 - c->t;
            if (frac < alpha) {
                if (g.ncand >= g.capd) {
                    size_t nc = g.capd ? 2 * g.capd : 4096;
                    void *p = realloc(g.cand, nc * sizeof *g.cand);
                    if (!p) goto oom;
                    g.cand = p, g.capd = nc;
                }
                g.cand[g.ncand++] = (Cand){end == 0 ? c->a : c->b, (int)i, frac * len};
            }
        }
    }
    qsort(g.cand, g.ncand, sizeof *g.cand, cand_cmp);
    for (size_t i = 0; i < g.ncand;) {
        size_t j = i;
        int v = g.cand[i].v;
        while (j < g.ncand && g.cand[j].v == v) j++;
        if (sgn(&g, v) != 0) {
            for (size_t k = i; k < j; k++) {
                Cut *c = &g.cut[g.cand[k].cut];
                int other = c->a == v ? c->b : c->a;
                if (sgn(&g, other) == 0) continue; /* that edge lost its cut point when its other end was warped */
                memcpy(g.vp + 3 * (size_t)v, c->p, 3 * sizeof(double));
                g.vf[v] = 0;
                g.warped[v] = 1;
                out->warped_vertices++;
                break;
            }
        }
        i = j;
    }
    g.unit = o.h;
    memcpy(g.origin, o.origin, sizeof g.origin);
    for (int it = 0; it < 20; it++) {
        for (size_t i = 0; i < o.n; i++)
            if (o.leaf[i].s) leaf_tets(&bg, i, pass_unwarp, &g);
        int restored = 0;
        for (size_t i = 0; i < g.nv; i++)
            if (g.warped[i] == 2) {
                for (int k = 0; k < 3; k++) g.vp[3 * i + k] = o.origin[k] + 0.5 * o.h * g.vc[i][k];
                g.vf[i] = g.vf0[i];
                g.warped[i] = 0;
                restored++;
            }
        out->unwarped_vertices += restored;
        out->warped_vertices -= restored;
        if (!restored) break;
    }
    /* pass 2: stencils */
    for (size_t i = 0; i < o.n && !g.oom; i++)
        if (o.leaf[i].s) leaf_tets(&bg, i, pass_stencil, &g);
    if (g.oom) goto oom;
    out->steiner_vertices = g.steiner;
    out->bad_stencils = g.bad_stencils;
    if (g.ntet == 0) {
        snprintf(err, errlen, "no tetrahedron lies inside the surface: the part is thinner than the surface cells (%.4g m), or the inside test "
                              "found no inside",
                 h);
        goto fail;
    }
    if (s->max_elements && g.ntet > s->max_elements) {
        snprintf(err, errlen, "the mesh has %zu elements, above the limit of %llu: increase surface_size", g.ntet, (unsigned long long)s->max_elements);
        goto fail;
    }
    size_t ne = g.ntet, nn = g.nout;
    /* faces: boundary, conformity, regions */
    fr = malloc(4 * ne * sizeof *fr);
    regp = malloc(ne * sizeof(int));
    if (!fr || !regp) goto oom;
    for (size_t e = 0; e < ne; e++) {
        regp[e] = (int)e;
        for (int k = 0; k < 4; k++) {
            FaceRec *f = &fr[4 * e + k];
            for (int q = 0; q < 3; q++) f->n[q] = g.tet[4 * e + TETMESH_FACE[k][q]];
            for (int i = 1; i < 3; i++)
                for (int j = i; j > 0 && f->n[j] < f->n[j - 1]; j--) {
                    int t = f->n[j];
                    f->n[j] = f->n[j - 1], f->n[j - 1] = t;
                }
            f->elem = (int)e, f->local = (unsigned char)k;
        }
    }
    qsort(fr, 4 * ne, sizeof *fr, face_cmp);
    size_t nb = 0;
    for (size_t i = 0; i < 4 * ne;) {
        size_t j = i + 1;
        while (j < 4 * ne && !memcmp(fr[j].n, fr[i].n, sizeof fr[i].n)) j++;
        if (j - i == 1) nb++;
        else if (j - i > 2) out->nonmanifold_faces++;
        for (size_t k = i + 1; k < j; k++) {
            int a = uf_find(regp, fr[i].elem), b = uf_find(regp, fr[k].elem);
            if (a != b) regp[a < b ? b : a] = a < b ? a : b;
        }
        i = j;
    }
    out->face_elem = malloc((nb ? nb : 1) * sizeof(int));
    out->face_local = malloc(nb ? nb : 1);
    if (!out->face_elem || !out->face_local) goto oom;
    for (size_t i = 0; i < 4 * ne;) {
        size_t j = i + 1;
        while (j < 4 * ne && !memcmp(fr[j].n, fr[i].n, sizeof fr[i].n)) j++;
        if (j - i == 1) {
            out->face_elem[out->nfaces] = fr[i].elem;
            out->face_local[out->nfaces] = fr[i].local;
            out->nfaces++;
        }
        i = j;
    }
    free(fr);
    fr = NULL;
    for (size_t e = 0; e < ne; e++)
        if (uf_find(regp, (int)e) == (int)e) out->regions++;
    free(regp);
    regp = NULL;
    /* pinches: a boundary edge in four or more boundary faces is a place where the stuffed mesh holds together only
     * along an edge (a wall or a fin thinner than the lattice there). The octree is refined around every such edge,
     * down to the finest cell, and the part stuffed again; three rounds at most. */
    if (round < 3) {
        int nb = out->nfaces, npinch = 0, nsplit = 0;
        int (*be)[2] = malloc(3 * (size_t)(nb ? nb : 1) * sizeof *be);
        if (!be) goto oom;
        for (int f = 0; f < nb; f++)
            for (int q = 0; q < 3; q++) {
                int a = g.tet[4 * (size_t)out->face_elem[f] + TETMESH_FACE[out->face_local[f]][q]];
                int b = g.tet[4 * (size_t)out->face_elem[f] + TETMESH_FACE[out->face_local[f]][(q + 1) % 3]];
                be[3 * f + q][0] = a < b ? a : b, be[3 * f + q][1] = a < b ? b : a;
            }
        qsort(be, 3 * (size_t)nb, sizeof *be, pair_cmp);
        size_t *split = NULL, nsp = 0, capsp = 0;
        for (size_t i = 0; i < 3 * (size_t)nb;) {
            size_t j = i + 1;
            while (j < 3 * (size_t)nb && be[j][0] == be[i][0] && be[j][1] == be[i][1]) j++;
            if (j - i > 2) {
                npinch++;
                double mid[3];
                for (int d = 0; d < 3; d++) mid[d] = 0.5 * (g.xyz[3 * (size_t)be[i][0] + d] + g.xyz[3 * (size_t)be[i][1] + d]);
                /* the leaves around the pinch: the one containing it and its 26 neighbours */
                int c0[3];
                for (int d = 0; d < 3; d++) c0[d] = (int)floor((mid[d] - o.origin[d]) / o.h);
                for (int dz = -1; dz <= 1; dz++)
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int L = find_leaf(&o, c0[0] + dx, c0[1] + dy, c0[2] + dz);
                            if (L < 0 || o.leaf[L].s <= 1) continue;
                            if (nsp >= capsp) {
                                size_t nc = capsp ? 2 * capsp : 256;
                                void *np = realloc(split, nc * sizeof *split);
                                if (!np) {
                                    free(split), free(be);
                                    goto oom;
                                }
                                split = np, capsp = nc;
                            }
                            split[nsp++] = (size_t)L;
                        }
            }
            i = j;
        }
        free(be);
        for (size_t k = 0; k < nsp; k++)
            if (o.leaf[split[k]].s > 1) {
                if (!split_leaf(&o, split[k])) {
                    free(split);
                    goto oom;
                }
                nsplit++;
            }
        free(split);
        out->pinches = npinch;
        if (nsplit > 0) {
            if (!octree_balance(&o)) goto oom;
            out->pinch_splits += nsplit;
            round++;
            /* start the stuffing again on the refined tree */
            hm_free(&corners);
            hm_free(&g.vmap);
            hm_free(&g.emap);
            free(g.vc), free(g.vp), free(g.vf), free(g.vf0), free(g.warped), free(g.vout), free(g.cut), free(g.cand), free(g.xyz), free(g.tet);
            double zt = g.zero_tol;
            memset(&g, 0, sizeof g);
            g.h = h, g.s = s, g.zero_tol = zt;
            free(out->face_elem), free(out->face_local);
            out->face_elem = NULL, out->face_local = NULL;
            out->nfaces = out->regions = out->nonmanifold_faces = 0;
            out->warped_vertices = out->unwarped_vertices = out->cut_points = out->steiner_vertices = out->bad_stencils = 0;
            goto restuff;
        }
    }
    /* quality improvement: smoothing that only accepts moves raising the worst angle around a node */
    if (!improve_quality(&g, out, s)) goto oom;
    /* quality of the corner tetrahedra */
    out->min_dihedral = 180, out->max_dihedral = 0, out->worst_aspect = 1;
    double vol = 0;
    for (size_t e = 0; e < ne; e++) {
        double X[4][3], deg[6];
        for (int k = 0; k < 4; k++) memcpy(X[k], g.xyz + 3 * (size_t)g.tet[4 * e + k], sizeof X[k]);
        double V = tet_signed_volume(X);
        vol += V;
        if (!(V > 0)) {
            double c[3] = {0, 0, 0};
            for (int k = 0; k < 4; k++)
                for (int q = 0; q < 3; q++) c[q] += X[k][q] / 4;
            snprintf(err, errlen, "element %zu has volume %.3g m^3 after warping (at %.4g %.4g %.4g m)", e, V, c[0], c[1], c[2]);
            goto fail;
        }
        tet_dihedral_angles(X, deg);
        for (int k = 0; k < 6; k++) {
            if (deg[k] < out->min_dihedral) {
                out->min_dihedral = deg[k];
                for (int q = 0; q < 3; q++) out->min_dihedral_at[q] = 0.25 * (X[0][q] + X[1][q] + X[2][q] + X[3][q]);
            }
            out->max_dihedral = fmax(out->max_dihedral, deg[k]);
        }
        out->worst_aspect = fmax(out->worst_aspect, tet_aspect_ratio(X));
    }
    /* TET10: a node on every edge, moved onto the surface on boundary edges when the element stays valid */
    int npe = order == 2 ? 10 : 4;
    int *conn = malloc(npe * ne * sizeof(int));
    if (!conn) goto oom;
    for (size_t e = 0; e < ne; e++) memcpy(conn + npe * e, g.tet + 4 * e, 4 * sizeof(int));
    if (order == 2) {
        HMap mid = {0};
        if (!hm_init(&mid, 7 * ne)) {
            free(conn);
            goto oom;
        }
        size_t ncorner = nn;
        for (size_t e = 0; e < ne && !g.oom; e++)
            for (int k = 0; k < 6; k++) {
                int a = g.tet[4 * e + TETMESH_EDGE[k][0]], b = g.tet[4 * e + TETMESH_EDGE[k][1]];
                uint64_t key = ekey(a, b);
                int m = hm_get(&mid, key);
                if (m < 0) {
                    double p[3];
                    for (int q = 0; q < 3; q++) p[q] = 0.5 * (g.xyz[3 * (size_t)a + q] + g.xyz[3 * (size_t)b + q]);
                    m = out_point(&g, p);
                    if (m < 0 || !hm_put(&mid, key, m)) g.oom = true;
                }
                conn[npe * e + 4 + k] = m;
            }
        if (g.oom) {
            hm_free(&mid), free(conn);
            goto oom;
        }
        nn = g.nout;
        /* boundary edges */
        unsigned char *curved = calloc(nn - ncorner + 1, 1);
        double *straight = malloc(3 * (nn - ncorner + 1) * sizeof(double));
        if (!curved || !straight) {
            free(curved), free(straight), hm_free(&mid), free(conn);
            goto oom;
        }
        memcpy(straight, g.xyz + 3 * ncorner, 3 * (nn - ncorner) * sizeof(double));
        for (int f = 0; f < out->nfaces; f++) {
            int e = out->face_elem[f], k = out->face_local[f];
            for (int q = 0; q < 3; q++) {
                int a = TETMESH_FACE[k][q], b = TETMESH_FACE[k][(q + 1) % 3];
                for (int ed = 0; ed < 6; ed++)
                    if ((TETMESH_EDGE[ed][0] == a && TETMESH_EDGE[ed][1] == b) || (TETMESH_EDGE[ed][0] == b && TETMESH_EDGE[ed][1] == a)) {
                        int m = conn[npe * (size_t)e + 4 + ed];
                        if (!curved[m - ncorner]) {
                            double qp[3];
                            s->project(s->ctx, g.xyz + 3 * (size_t)m, qp);
                            memcpy(g.xyz + 3 * (size_t)m, qp, sizeof qp);
                            curved[m - ncorner] = 1;
                        }
                    }
            }
        }
        /* straighten until every element is valid */
        for (int pass = 0; pass < 20; pass++) {
            int changed = 0;
            for (size_t e = 0; e < ne; e++) {
                bool any = false;
                for (int k = 0; k < 6; k++) any |= curved[conn[npe * e + 4 + k] - ncorner] == 1;
                if (!any) continue;
                double X[10][3], C4[4][3];
                for (int k = 0; k < 10; k++) memcpy(X[k], g.xyz + 3 * (size_t)conn[npe * e + k], sizeof X[k]);
                memcpy(C4, X, sizeof C4);
                if (tet10_valid(X, 6 * tet_signed_volume(C4))) continue;
                for (int k = 0; k < 6; k++) {
                    int m = conn[npe * e + 4 + k];
                    if (curved[m - ncorner] == 1) {
                        memcpy(g.xyz + 3 * (size_t)m, straight + 3 * (size_t)(m - ncorner), 3 * sizeof(double));
                        curved[m - ncorner] = 2;
                        changed++;
                    }
                }
            }
            if (!changed) break;
        }
        for (size_t i = 0; i < nn - ncorner; i++) {
            if (curved[i] == 1) out->curved_edges++;
            if (curved[i] == 2) out->straightened_edges++;
        }
        vol = 0;
        for (size_t e = 0; e < ne; e++) {
            double X[10][3];
            for (int k = 0; k < 10; k++) memcpy(X[k], g.xyz + 3 * (size_t)conn[npe * e + k], sizeof X[k]);
            vol += tet10_volume(X);
        }
        free(curved), free(straight);
        hm_free(&mid);
    }
    out->volume = vol;
    out->ref_volume = s->ref_volume;
    /* distance of the surface to the part: boundary corner nodes and face centroids (the quadratic face at its centroid) */
    {
        unsigned char *seen = calloc(nn, 1);
        double *pts = malloc(3 * ((size_t)out->nfaces + nn + 1) * sizeof(double));
        if (!seen || !pts) {
            free(seen), free(pts), free(conn);
            goto oom;
        }
        int np = 0;
        for (int f = 0; f < out->nfaces; f++) {
            int e = out->face_elem[f], k = out->face_local[f];
            for (int q = 0; q < 3; q++) {
                int nd = conn[npe * (size_t)e + TETMESH_FACE[k][q]];
                if (!seen[nd]) {
                    seen[nd] = 1;
                    memcpy(pts + 3 * (size_t)np++, g.xyz + 3 * (size_t)nd, 3 * sizeof(double));
                }
            }
        }
        int nnode_pts = np;
        for (int f = 0; f < out->nfaces; f++) {
            int e = out->face_elem[f], k = out->face_local[f];
            double c[3] = {0, 0, 0};
            if (npe == 4) {
                for (int q = 0; q < 3; q++)
                    for (int d = 0; d < 3; d++) c[d] += g.xyz[3 * (size_t)conn[4 * (size_t)e + TETMESH_FACE[k][q]] + d] / 3;
            } else {
                /* six-node triangle at its centroid: corners -1/9, mid-edge nodes 4/9 */
                for (int q = 0; q < 3; q++) {
                    int a = TETMESH_FACE[k][q], b = TETMESH_FACE[k][(q + 1) % 3], m = -1;
                    for (int ed = 0; ed < 6; ed++)
                        if ((TETMESH_EDGE[ed][0] == a && TETMESH_EDGE[ed][1] == b) || (TETMESH_EDGE[ed][0] == b && TETMESH_EDGE[ed][1] == a)) m = 4 + ed;
                    for (int d = 0; d < 3; d++)
                        c[d] += -g.xyz[3 * (size_t)conn[10 * (size_t)e + a] + d] / 9 + 4 * g.xyz[3 * (size_t)conn[10 * (size_t)e + m] + d] / 9;
                }
            }
            memcpy(pts + 3 * (size_t)np++, c, sizeof c);
        }
        double *fv = malloc((size_t)(np ? np : 1) * sizeof(double));
        if (!fv) {
            free(seen), free(pts), free(conn);
            goto oom;
        }
        sdf_many(s, pts, fv, np);
        for (int i = 0; i < np; i++) {
            double d = fabs(fv[i]);
            if (i < nnode_pts) {
                out->surf_node_dist_max = fmax(out->surf_node_dist_max, d);
                out->surf_node_dist_mean += d / (nnode_pts ? nnode_pts : 1);
            } else {
                out->surf_face_dist_max = fmax(out->surf_face_dist_max, d);
                out->surf_face_dist_mean += d / (out->nfaces ? out->nfaces : 1);
            }
        }
        free(seen), free(pts), free(fv);
    }
    out->order = order, out->npe = npe;
    out->nnodes = (int)nn, out->nelems = (int)ne;
    out->xyz = g.xyz;
    g.xyz = NULL;
    out->conn = conn;
    out->seconds = now_seconds() - t0;
    double floor = s->min_angle_floor > 0 ? s->min_angle_floor : 5.0;
    if (out->min_dihedral < floor) {
        snprintf(err, errlen, "the smallest dihedral angle is %.2f degrees (at %.4g %.4g %.4g m), below the floor of %.1f degrees",
                 out->min_dihedral, out->min_dihedral_at[0], out->min_dihedral_at[1], out->min_dihedral_at[2], floor);
        goto fail_keep_stats;
    }
    ok = true;
    goto done;
oom:
    snprintf(err, errlen, "out of memory in the tetrahedral mesher");
fail:
    tetmesh_free(out);
    goto done;
fail_keep_stats: {
    /* the numbers stay readable for the caller's report; the arrays go */
    TetMesh keep = *out;
    free(out->xyz), free(out->conn), free(out->face_elem), free(out->face_local);
    *out = keep;
    out->xyz = NULL, out->conn = NULL, out->face_elem = NULL, out->face_local = NULL;
    out->nnodes = out->nelems = out->nfaces = 0;
}
done:
    free(o.leaf);
    hm_free(&o.map);
    hm_free(&corners);
    hm_free(&g.vmap);
    hm_free(&g.emap);
    free(g.vc), free(g.vp), free(g.vf), free(g.vf0), free(g.warped), free(g.vout), free(g.cut), free(g.cand), free(g.xyz), free(g.tet);
    free(fr), free(regp);
    return ok;
}

/* ---- an STL as a signed distance ------------------------------------------------------------------------------- */

bool tet_stl_init(TetStlGeometry *gm, const double *v, const int *tri, int nt, char *err, size_t errlen) {
    memset(gm, 0, sizeof *gm);
    Bvh *b = calloc(1, sizeof *b);
    if (!b || !bvh_build(b, v, tri, nt)) {
        free(b);
        snprintf(err, errlen, "could not build the search tree of the surface");
        return false;
    }
    gm->v = v, gm->tri = tri, gm->nt = nt, gm->bvh = b;
    for (int k = 0; k < 3; k++) gm->lo[k] = INFINITY, gm->hi[k] = -INFINITY;
    double vol = 0;
    for (int t = 0; t < nt; t++) {
        const double *a = v + 3 * (size_t)tri[3 * t], *bb = v + 3 * (size_t)tri[3 * t + 1], *c = v + 3 * (size_t)tri[3 * t + 2];
        double n[3];
        cross3(bb, c, n);
        vol += dot3(a, n) / 6;
        for (int k = 0; k < 3; k++) {
            gm->lo[k] = fmin(gm->lo[k], fmin(a[k], fmin(bb[k], c[k])));
            gm->hi[k] = fmax(gm->hi[k], fmax(a[k], fmax(bb[k], c[k])));
        }
    }
    gm->volume = vol;
    double d[3];
    sub3(gm->hi, gm->lo, d);
    gm->scale = norm3(d);
    return true;
}

void tet_stl_free(TetStlGeometry *gm) {
    if (gm->bvh) bvh_free(gm->bvh);
    free(gm->bvh);
    memset(gm, 0, sizeof *gm);
}

static bool count_hit(void *ctx, const BvhHit *hit) {
    (void)hit;
    (*(int *)ctx)++;
    return true;
}

bool tet_stl_inside(TetStlGeometry *gm, const double p[3], bool *unanimous) {
    /* three nearly axial lines through p (a small irrational tilt keeps them off edges and vertices of a grid-like
     * mesh); a line whose total crossing count is odd went through a hole and abstains */
    static const double D[3][3] = {{1, 1.3e-4 * M_PI, 1.1e-4 * M_E}, {0.9e-4 * M_E, 1, 1.2e-4 * M_PI}, {1.4e-4 * M_PI, 0.8e-4 * M_E, 1}};
    int in = 0, votes = 0;
    for (int k = 0; k < 3; k++) {
        int fwd = 0, back = 0;
        double nd[3] = {-D[k][0], -D[k][1], -D[k][2]};
        bvh_ray_all(gm->bvh, p, D[k], 0, count_hit, &fwd);
        bvh_ray_all(gm->bvh, p, nd, 0, count_hit, &back);
        if ((fwd + back) % 2) continue;
        votes++;
        in += fwd % 2;
    }
    bool inside = 2 * in > votes;
    bool u = votes == 3 && (in == 0 || in == 3);
    if (unanimous) *unanimous = u;
    __atomic_fetch_add(&gm->evaluations, 1, __ATOMIC_RELAXED);
    if (!u) __atomic_fetch_add(&gm->votes_split, 1, __ATOMIC_RELAXED);
    return inside;
}

double tet_stl_sdf(void *vg, const double p[3]) {
    TetStlGeometry *gm = vg;
    int tri;
    double q[3], d;
    if (!bvh_closest(gm->bvh, p, INFINITY, &tri, q, &d)) return INFINITY;
    if (d <= 1e-9 * gm->scale) return 0;
    return tet_stl_inside(gm, p, NULL) ? -d : d;
}

void tet_stl_project(void *vg, const double p[3], double q[3]) {
    TetStlGeometry *gm = vg;
    int tri;
    double d;
    if (!bvh_closest(gm->bvh, p, INFINITY, &tri, q, &d)) memcpy(q, p, 3 * sizeof(double));
}

bool tet_stl_cross(void *vg, const double a[3], const double b[3], double *t) {
    TetStlGeometry *gm = vg;
    double d[3];
    sub3(b, a, d);
    BvhHit hit;
    if (!bvh_ray_nearest(gm->bvh, a, d, 0, 1, -1, &hit)) return false;
    *t = hit.t;
    return true;
}
