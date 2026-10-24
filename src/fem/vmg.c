/* vmg.c - geometric multigrid preconditioned conjugate gradients on voxel lattices (see vmg.h) */
#include "vmg.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double wall(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* the corners of a cell in the HEX8 order of hex8.h */
static const int CORNER[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};

typedef struct Level {
    int nx, ny, nz;          /* cells */
    size_t nnode;            /* lattice nodes, (nx + 1)(ny + 1)(nz + 1) */
    int nel;                 /* cells holding material */
    int *el_cell;            /* nel */
    int *cell_el;            /* nx ny nz: element or -1 */
    double *ke;              /* 576 nel on the coarse levels (the finest reads the problem's) */
    int *eq;                 /* 3 nnode: equation or -1 */
    unsigned char *used;     /* nnode: a corner of an element */
    int neq;
    CsrMatrix A;             /* coarse levels own their matrix */
    const CsrMatrix *Ap;     /* the level's matrix (the finest: the caller's) */
    int nblk;                /* nodal block Jacobi: a node's free components are consecutive equations */
    int *blk_eq0;
    unsigned char *blk_n;
    double *blk_inv;         /* 9 per block */
    double lo, hi;           /* the Chebyshev interval of the block-Jacobi preconditioned operator */
    int64_t *pp;             /* prolongation from the next coarser level, rows: this level's equations */
    int *pc;
    double *pw;
    int64_t *rp;             /* restriction to this level from the finer one (the finer's P transposed), rows: ours */
    int *rc;
    double *rw;
    double *b, *x, *r, *d, *t; /* work vectors */
    CholFactor F;
    bool coarsest, factored;
} Level;

static size_t nid(const Level *L, int i, int j, int k) { return ((size_t)k * (L->ny + 1) + (size_t)j) * (L->nx + 1) + (size_t)i; }
static size_t cid(const Level *L, int i, int j, int k) { return ((size_t)k * L->ny + (size_t)j) * L->nx + (size_t)i; }

static void level_free(Level *L) {
    free(L->el_cell), free(L->cell_el), free(L->ke), free(L->eq), free(L->used);
    csr_free(&L->A);
    free(L->blk_eq0), free(L->blk_n), free(L->blk_inv);
    free(L->pp), free(L->pc), free(L->pw), free(L->rp), free(L->rc), free(L->rw);
    free(L->b), free(L->x), free(L->r), free(L->d), free(L->t);
    if (L->factored) chol_free(&L->F);
    memset(L, 0, sizeof *L);
}

/* ---- threaded vector kernels (fixed blocks, so sums do not depend on the thread count) ----------------------------- */

enum { VBLOCK = 8192 };
typedef enum { V_ZERO, V_COPY, V_AXPY, V_XPBY, V_SUB, V_SCALE, V_DOT } VOp;
typedef struct VecCtx {
    VOp op;
    int n;
    double a;
    const double *u, *v;
    double *y;
    double *part;
} VecCtx;

static void vec_range(void *vc, int b0, int b1, int tid) {
    (void)tid;
    VecCtx *c = vc;
    for (int blk = b0; blk < b1; blk++) {
        int i0 = blk * VBLOCK, i1 = i0 + VBLOCK < c->n ? i0 + VBLOCK : c->n;
        double s = 0;
        switch (c->op) {
        case V_ZERO: for (int i = i0; i < i1; i++) c->y[i] = 0; break;
        case V_COPY: for (int i = i0; i < i1; i++) c->y[i] = c->u[i]; break;
        case V_AXPY: for (int i = i0; i < i1; i++) c->y[i] += c->a * c->u[i]; break;     /* y += a u */
        case V_XPBY: for (int i = i0; i < i1; i++) c->y[i] = c->u[i] + c->a * c->y[i]; break; /* y = u + a y */
        case V_SUB: for (int i = i0; i < i1; i++) c->y[i] = c->u[i] - c->v[i]; break;    /* y = u - v */
        case V_SCALE: for (int i = i0; i < i1; i++) c->y[i] *= c->a; break;
        case V_DOT: for (int i = i0; i < i1; i++) s += c->u[i] * c->v[i]; c->part[blk] = s; break;
        }
    }
}

static double vec(ThreadPool *pool, VOp op, int n, double a, const double *u, const double *v, double *y) {
    int nb = (n + VBLOCK - 1) / VBLOCK;
    double stack[256], *part = op == V_DOT ? (nb <= 256 ? stack : malloc(sizeof(double) * (size_t)nb)) : NULL;
    if (op == V_DOT && !part) { /* out of memory for the partial sums: sum serially */
        double s = 0;
        for (int i = 0; i < n; i++) s += u[i] * v[i];
        return s;
    }
    VecCtx c = {op, n, a, u, v, y, part};
    if (pool && nb > 4) pool_for(pool, nb, 1, vec_range, &c);
    else vec_range(&c, 0, nb, 0);
    double s = 0;
    if (op == V_DOT) {
        for (int b = 0; b < nb; b++) s += part[b];
        if (part != stack) free(part);
    }
    return s;
}

/* y = M x for a compressed row matrix given by its three arrays (prolongation or restriction); add: y += M x */
typedef struct MatCtx {
    const int64_t *rp;
    const int *rc;
    const double *rw;
    const double *x;
    double *y;
    int add;
} MatCtx;

static void mat_range(void *vc, int r0, int r1, int tid) {
    (void)tid;
    MatCtx *c = vc;
    for (int i = r0; i < r1; i++) {
        double s = 0;
        for (int64_t k = c->rp[i]; k < c->rp[i + 1]; k++) s += c->rw[k] * c->x[c->rc[k]];
        c->y[i] = c->add ? c->y[i] + s : s;
    }
}

static void mat_apply(ThreadPool *pool, int rows, const int64_t *rp, const int *rc, const double *rw, const double *x, double *y, int add) {
    MatCtx c = {rp, rc, rw, x, y, add};
    if (pool && rows > 20000) pool_for(pool, rows, 2048, mat_range, &c);
    else mat_range(&c, 0, rows, 0);
}

/* z = D^-1 r, D the nodal diagonal blocks */
typedef struct BjCtx {
    const Level *L;
    const double *r;
    double *z;
} BjCtx;

static void bj_range(void *vc, int b0, int b1, int tid) {
    (void)tid;
    BjCtx *c = vc;
    const Level *L = c->L;
    for (int b = b0; b < b1; b++) {
        int e0 = L->blk_eq0[b], m = L->blk_n[b];
        const double *Inv = L->blk_inv + 9 * (size_t)b;
        double rr[3] = {0, 0, 0};
        for (int q = 0; q < m; q++) rr[q] = c->r[e0 + q];
        for (int q = 0; q < m; q++) {
            double s = 0;
            for (int w = 0; w < m; w++) s += Inv[3 * q + w] * rr[w];
            c->z[e0 + q] = s;
        }
    }
}

static void bj_apply(ThreadPool *pool, const Level *L, const double *r, double *z) {
    BjCtx c = {L, r, z};
    if (pool && L->nblk > 8000) pool_for(pool, L->nblk, 1024, bj_range, &c);
    else bj_range(&c, 0, L->nblk, 0);
}

/* ---- the levels --------------------------------------------------------------------------------------------------- */

static bool fail(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return false;
}

/* the finest level: the problem's elements as cells of one lattice, its equations as they are */
static bool level_fine(const VmgProblem *p, const CsrMatrix *K, Level *L, char *err, size_t errlen) {
    memset(L, 0, sizeof *L);
    if (p->nelems <= 0) return fail(err, errlen, "the multigrid solver got no elements");
    const double *x0 = p->xyz + 3 * (size_t)p->conn[0], *x1 = p->xyz + 3 * (size_t)p->conn[1];
    double h = x1[0] - x0[0];
    if (!(h > 0)) return fail(err, errlen, "the multigrid solver needs a voxel mesh: element 0 is not an axis-aligned cube");
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int e = 0; e < p->nelems; e++)
        for (int a = 0; a < 8; a++)
            for (int d = 0; d < 3; d++) {
                double v = p->xyz[3 * (size_t)p->conn[8 * (size_t)e + a] + d];
                if (v < lo[d]) lo[d] = v;
                if (v > hi[d]) hi[d] = v;
            }
    long long dims[3];
    for (int d = 0; d < 3; d++) {
        dims[d] = llround((hi[d] - lo[d]) / h);
        if (dims[d] < 1 || dims[d] > 100000) return fail(err, errlen, "the multigrid solver needs a voxel mesh (span of %lld cells in direction %d)", dims[d], d);
    }
    L->nx = (int)dims[0], L->ny = (int)dims[1], L->nz = (int)dims[2];
    L->nnode = (size_t)(L->nx + 1) * (L->ny + 1) * (L->nz + 1);
    size_t ncell = (size_t)L->nx * L->ny * L->nz;
    int *node_at = malloc(sizeof(int) * L->nnode);
    L->cell_el = malloc(sizeof(int) * ncell);
    L->el_cell = malloc(sizeof(int) * (size_t)p->nelems);
    L->eq = malloc(sizeof(int) * 3 * L->nnode);
    L->used = calloc(L->nnode, 1);
    if (!node_at || !L->cell_el || !L->el_cell || !L->eq || !L->used) {
        free(node_at);
        return fail(err, errlen, "out of memory for the multigrid lattice (%lld nodes)", (long long)L->nnode);
    }
    for (size_t q = 0; q < L->nnode; q++) node_at[q] = -1;
    for (size_t q = 0; q < ncell; q++) L->cell_el[q] = -1;
    double tol = 1e-6 * h;
    bool ok = true;
    for (int e = 0; e < p->nelems && ok; e++) {
        if (!p->ke_index || p->ke_index[e] < 0) {
            ok = fail(err, errlen, "the multigrid solver needs every element's matrix cached (element %d)", e);
            break;
        }
        int li[3];
        const double *c0 = p->xyz + 3 * (size_t)p->conn[8 * (size_t)e];
        for (int d = 0; d < 3; d++) li[d] = (int)llround((c0[d] - lo[d]) / h);
        for (int a = 0; a < 8 && ok; a++) {
            int nd = p->conn[8 * (size_t)e + a], q[3];
            for (int d = 0; d < 3; d++) {
                q[d] = li[d] + CORNER[a][d];
                if (fabs(p->xyz[3 * (size_t)nd + d] - (lo[d] + q[d] * h)) > tol) {
                    ok = fail(err, errlen, "the multigrid solver needs a voxel mesh: element %d is not a cube of the lattice (corner %d)", e, a);
                    break;
                }
            }
            if (!ok) break;
            if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] > L->nx || q[1] > L->ny || q[2] > L->nz) {
                ok = fail(err, errlen, "the multigrid solver needs a voxel mesh: element %d leaves the lattice", e);
                break;
            }
            size_t n = nid(L, q[0], q[1], q[2]);
            if (node_at[n] < 0) node_at[n] = nd;
            else if (node_at[n] != nd) {
                ok = fail(err, errlen, "the multigrid solver needs a conforming voxel mesh: nodes %d and %d share a place", node_at[n], nd);
                break;
            }
        }
        if (!ok) break;
        size_t c = cid(L, li[0], li[1], li[2]);
        if (L->cell_el[c] >= 0) {
            ok = fail(err, errlen, "the multigrid solver needs a voxel mesh: elements %d and %d fill one cell", L->cell_el[c], e);
            break;
        }
        L->cell_el[c] = e;
        L->el_cell[e] = (int)c;
    }
    if (ok) {
        L->nel = p->nelems;
        for (size_t n = 0; n < L->nnode; n++) {
            L->used[n] = node_at[n] >= 0;
            for (int k = 0; k < 3; k++) {
                int q = node_at[n] >= 0 ? p->eq[3 * (size_t)node_at[n] + k] : -1;
                if (q >= K->n) {
                    ok = fail(err, errlen, "the multigrid solver got equation %d beyond the matrix's %d", q, K->n);
                    break;
                }
                L->eq[3 * n + k] = q;
            }
            if (!ok) break;
        }
    }
    free(node_at);
    if (!ok) return false;
    L->neq = K->n;
    L->Ap = K;
    return true;
}

/* the element matrix of element e of level L, and its scale */
static const double *level_ke(const Level *L, const VmgProblem *p, int e, double *scale) {
    if (L->ke) {
        *scale = 1.0;
        return L->ke + 576 * (size_t)e;
    }
    *scale = p->elem_scale ? p->elem_scale[e] : 1.0;
    return p->ke + 576 * (size_t)p->ke_index[e];
}

typedef struct CoarseCtx {
    const Level *F;
    Level *C;
    const VmgProblem *p;
} CoarseCtx;

/* the matrices of coarse cells E0..E1: sum over the fine cells inside of T^T K_e T, the rows of prescribed fine
 * components left out of T */
static void coarse_range(void *vc, int E0, int E1, int tid) {
    (void)tid;
    CoarseCtx *c = vc;
    const Level *F = c->F;
    Level *C = c->C;
    double M[576];
    for (int E = E0; E < E1; E++) {
        double *Kc = C->ke + 576 * (size_t)E;
        memset(Kc, 0, sizeof(double) * 576);
        size_t ce = (size_t)C->el_cell[E];
        int I = (int)(ce % C->nx), J = (int)((ce / C->nx) % C->ny), Kk = (int)(ce / ((size_t)C->nx * C->ny));
        for (int child = 0; child < 8; child++) {
            int px = child & 1, py = (child >> 1) & 1, pz = (child >> 2) & 1;
            int i = 2 * I + px, j = 2 * J + py, k = 2 * Kk + pz;
            if (i >= F->nx || j >= F->ny || k >= F->nz) continue;
            int e = F->cell_el[cid(F, i, j, k)];
            if (e < 0) continue;
            double s;
            const double *Kf = level_ke(F, c->p, e, &s);
            if (s == 0) continue;
            /* W[a][A]: the weight of coarse corner A at fine corner a; m[a][q]: fine component free */
            double W[8][8];
            unsigned char m[8][3];
            for (int a = 0; a < 8; a++) {
                int l[3] = {px + CORNER[a][0], py + CORNER[a][1], pz + CORNER[a][2]};
                for (int A = 0; A < 8; A++) {
                    double w = 1;
                    for (int d = 0; d < 3; d++) {
                        int t = l[d] - 2 * CORNER[A][d];
                        w *= t == 0 ? 1.0 : (t == 1 || t == -1 ? 0.5 : 0.0);
                    }
                    W[a][A] = w;
                }
                size_t n = nid(F, i + CORNER[a][0], j + CORNER[a][1], k + CORNER[a][2]);
                for (int q = 0; q < 3; q++) m[a][q] = F->eq[3 * n + (size_t)q] >= 0;
            }
            /* M = Kf T: M[(a q)][(B r)] = sum_b Kf[(a q)][(b r)] m[b][r] W[b][B] */
            for (int row = 0; row < 24; row++)
                for (int B = 0; B < 8; B++)
                    for (int r = 0; r < 3; r++) {
                        double v = 0;
                        for (int b = 0; b < 8; b++)
                            if (m[b][r] && W[b][B] != 0) v += Kf[row * 24 + 3 * b + r] * W[b][B];
                        M[row * 24 + 3 * B + r] = v;
                    }
            /* Kc += s T^T M */
            for (int A = 0; A < 8; A++)
                for (int q = 0; q < 3; q++)
                    for (int a = 0; a < 8; a++) {
                        if (!m[a][q] || W[a][A] == 0) continue;
                        double w = s * W[a][A];
                        const double *mr = M + (3 * a + q) * 24;
                        double *kr = Kc + (3 * A + q) * 24;
                        for (int col = 0; col < 24; col++) kr[col] += w * mr[col];
                    }
        }
    }
}

static bool level_coarsen(const Level *F, const VmgProblem *p, Level *C, ThreadPool *pool, char *err, size_t errlen) {
    memset(C, 0, sizeof *C);
    C->nx = (F->nx + 1) / 2, C->ny = (F->ny + 1) / 2, C->nz = (F->nz + 1) / 2;
    C->nnode = (size_t)(C->nx + 1) * (C->ny + 1) * (C->nz + 1);
    size_t ncell = (size_t)C->nx * C->ny * C->nz;
    C->cell_el = malloc(sizeof(int) * ncell);
    C->eq = malloc(sizeof(int) * 3 * C->nnode);
    C->used = calloc(C->nnode, 1);
    double *diag = calloc(3 * C->nnode, sizeof(double));
    if (!C->cell_el || !C->eq || !C->used || !diag) {
        free(diag);
        return fail(err, errlen, "out of memory for a multigrid level (%lld nodes)", (long long)C->nnode);
    }
    for (size_t q = 0; q < ncell; q++) C->cell_el[q] = -1;
    for (int e = 0; e < F->nel; e++) {
        size_t c = (size_t)F->el_cell[e];
        int i = (int)(c % F->nx), j = (int)((c / F->nx) % F->ny), k = (int)(c / ((size_t)F->nx * F->ny));
        C->cell_el[cid(C, i / 2, j / 2, k / 2)] = 0;
    }
    C->nel = 0;
    for (size_t q = 0; q < ncell; q++)
        if (C->cell_el[q] == 0) C->nel++;
    C->el_cell = malloc(sizeof(int) * (size_t)(C->nel ? C->nel : 1));
    C->ke = malloc(sizeof(double) * 576 * (size_t)(C->nel ? C->nel : 1));
    if (!C->el_cell || !C->ke) {
        free(diag);
        return fail(err, errlen, "out of memory for the matrices of a multigrid level (%d cells)", C->nel);
    }
    int ne = 0;
    for (size_t q = 0; q < ncell; q++)
        if (C->cell_el[q] == 0) {
            C->el_cell[ne] = (int)q;
            C->cell_el[q] = ne++;
        }
    CoarseCtx cc = {F, C, p};
    if (pool && C->nel > 64) pool_for(pool, C->nel, 16, coarse_range, &cc);
    else coarse_range(&cc, 0, C->nel, 0);
    /* nodes and their components: a coarse component is prescribed where the fine node at its place is used and
     * prescribed there, and dropped where it carries no stiffness at all */
    for (int E = 0; E < C->nel; E++) {
        size_t ce = (size_t)C->el_cell[E];
        int I = (int)(ce % C->nx), J = (int)((ce / C->nx) % C->ny), Kk = (int)(ce / ((size_t)C->nx * C->ny));
        for (int A = 0; A < 8; A++) {
            size_t n = nid(C, I + CORNER[A][0], J + CORNER[A][1], Kk + CORNER[A][2]);
            C->used[n] = 1;
            for (int q = 0; q < 3; q++) diag[3 * n + (size_t)q] += C->ke[576 * (size_t)E + (size_t)(3 * A + q) * 25];
        }
    }
    C->neq = 0;
    for (int k = 0; k <= C->nz; k++)
        for (int j = 0; j <= C->ny; j++)
            for (int i = 0; i <= C->nx; i++) {
                size_t n = nid(C, i, j, k);
                for (int q = 0; q < 3; q++) {
                    int fr = C->used[n] && diag[3 * n + (size_t)q] > 0;
                    if (fr && 2 * i <= F->nx && 2 * j <= F->ny && 2 * k <= F->nz) {
                        size_t fn = nid(F, 2 * i, 2 * j, 2 * k);
                        if (F->used[fn] && F->eq[3 * fn + (size_t)q] < 0) fr = 0;
                    }
                    C->eq[3 * n + (size_t)q] = fr ? C->neq++ : -1;
                }
            }
    free(diag);
    /* the coarse matrix, assembled from the cells' matrices */
    int *ed = malloc(sizeof(int) * 24 * (size_t)(C->nel ? C->nel : 1));
    if (!ed) return fail(err, errlen, "out of memory assembling a multigrid level");
    for (int E = 0; E < C->nel; E++) {
        size_t ce = (size_t)C->el_cell[E];
        int I = (int)(ce % C->nx), J = (int)((ce / C->nx) % C->ny), Kk = (int)(ce / ((size_t)C->nx * C->ny));
        for (int A = 0; A < 8; A++) {
            size_t n = nid(C, I + CORNER[A][0], J + CORNER[A][1], Kk + CORNER[A][2]);
            for (int q = 0; q < 3; q++) ed[24 * (size_t)E + 3 * A + q] = C->eq[3 * n + (size_t)q];
        }
    }
    if (!csr_from_elements(&C->A, C->neq, C->nel, 24, ed, err, errlen)) {
        free(ed);
        return false;
    }
    for (int E = 0; E < C->nel; E++) csr_add_element(&C->A, 24, ed + 24 * (size_t)E, C->ke + 576 * (size_t)E);
    free(ed);
    C->Ap = &C->A;
    /* the prolongation into the fine level: trilinear weights from the coarse nodes around each fine node */
    Level *Fm = (Level *)F;
    Fm->pp = calloc((size_t)F->neq + 1, sizeof(int64_t));
    if (!Fm->pp) return fail(err, errlen, "out of memory for a prolongation");
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            for (int r = 0; r < F->neq; r++) Fm->pp[r + 1] += Fm->pp[r];
            Fm->pc = malloc(sizeof(int) * (size_t)(Fm->pp[F->neq] ? Fm->pp[F->neq] : 1));
            Fm->pw = malloc(sizeof(double) * (size_t)(Fm->pp[F->neq] ? Fm->pp[F->neq] : 1));
            if (!Fm->pc || !Fm->pw) return fail(err, errlen, "out of memory for a prolongation");
        }
        int64_t *fill = NULL;
        if (pass == 1) {
            fill = malloc(sizeof(int64_t) * (size_t)(F->neq ? F->neq : 1));
            if (!fill) return fail(err, errlen, "out of memory for a prolongation");
            memcpy(fill, Fm->pp, sizeof(int64_t) * (size_t)F->neq);
        }
        for (int k = 0; k <= F->nz; k++)
            for (int j = 0; j <= F->ny; j++)
                for (int i = 0; i <= F->nx; i++) {
                    size_t n = nid(F, i, j, k);
                    if (!F->used[n]) continue;
                    int lo[3] = {i / 2, j / 2, k / 2}, cnt[3] = {(i & 1) + 1, (j & 1) + 1, (k & 1) + 1};
                    double wt[3] = {(i & 1) ? 0.5 : 1.0, (j & 1) ? 0.5 : 1.0, (k & 1) ? 0.5 : 1.0};
                    for (int q = 0; q < 3; q++) {
                        int row = F->eq[3 * n + (size_t)q];
                        if (row < 0) continue;
                        for (int c2 = 0; c2 < cnt[2]; c2++)
                            for (int c1 = 0; c1 < cnt[1]; c1++)
                                for (int c0 = 0; c0 < cnt[0]; c0++) {
                                    int I = lo[0] + c0, J = lo[1] + c1, Kk = lo[2] + c2;
                                    if (I > C->nx || J > C->ny || Kk > C->nz) continue;
                                    int col = C->eq[3 * nid(C, I, J, Kk) + (size_t)q];
                                    if (col < 0) continue;
                                    if (pass == 0) Fm->pp[row + 1]++;
                                    else {
                                        Fm->pc[fill[row]] = col;
                                        Fm->pw[fill[row]] = wt[0] * wt[1] * wt[2];
                                        fill[row]++;
                                    }
                                }
                    }
                }
        free(fill);
    }
    /* the restriction: the prolongation transposed */
    int64_t nnz = Fm->pp[F->neq];
    C->rp = calloc((size_t)C->neq + 1, sizeof(int64_t));
    C->rc = malloc(sizeof(int) * (size_t)(nnz ? nnz : 1));
    C->rw = malloc(sizeof(double) * (size_t)(nnz ? nnz : 1));
    int64_t *fill = malloc(sizeof(int64_t) * (size_t)(C->neq ? C->neq : 1));
    if (!C->rp || !C->rc || !C->rw || !fill) {
        free(fill);
        return fail(err, errlen, "out of memory for a restriction");
    }
    for (int64_t q = 0; q < nnz; q++) C->rp[Fm->pc[q] + 1]++;
    for (int r = 0; r < C->neq; r++) C->rp[r + 1] += C->rp[r];
    memcpy(fill, C->rp, sizeof(int64_t) * (size_t)C->neq);
    for (int r = 0; r < F->neq; r++)
        for (int64_t q = Fm->pp[r]; q < Fm->pp[r + 1]; q++) {
            int col = Fm->pc[q];
            C->rc[fill[col]] = r;
            C->rw[fill[col]] = Fm->pw[q];
            fill[col]++;
        }
    free(fill);
    return true;
}

/* The smoother's constants. Environment variables may change them for experiments (VMG_DEGREE, VMG_SAFETY, VMG_RATIO,
 * VMG_STEPS, VMG_COARSE); unset, every value is the one documented in vmg.h and verified in topotest T6. */
typedef struct VmgKnobs { int degree, steps, coarse; double safety, ratio; } VmgKnobs;

static VmgKnobs vmg_knobs(const VmgOptions *o) {
    VmgKnobs k = {o->smooth_degree > 0 ? o->smooth_degree : 2, 12, o->coarse_max > 0 ? o->coarse_max : 6000, 1.1, 10.0};
    const char *v;
    if ((v = getenv("VMG_DEGREE")) && atoi(v) > 0) k.degree = atoi(v);
    if ((v = getenv("VMG_STEPS")) && atoi(v) > 0 && atoi(v) <= 64) k.steps = atoi(v);
    if ((v = getenv("VMG_COARSE")) && atoi(v) > 0) k.coarse = atoi(v);
    if ((v = getenv("VMG_SAFETY")) && atof(v) >= 1.0) k.safety = atof(v);
    if ((v = getenv("VMG_RATIO")) && atof(v) > 1.0) k.ratio = atof(v);
    return k;
}

/* largest eigenvalue of a symmetric tridiagonal matrix (diagonal a, off-diagonal b) by bisection on Sturm counts */
static double tridiag_max(const double *a, const double *b, int n) {
    double lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < n; i++) {
        double r = (i > 0 ? fabs(b[i - 1]) : 0) + (i + 1 < n ? fabs(b[i]) : 0);
        if (a[i] - r < lo) lo = a[i] - r;
        if (a[i] + r > hi) hi = a[i] + r;
    }
    for (int it = 0; it < 100; it++) {
        double mid = 0.5 * (lo + hi), d = 1;
        int below = 0;
        for (int i = 0; i < n; i++) {
            d = a[i] - mid - (i > 0 ? b[i - 1] * b[i - 1] / d : 0);
            if (d == 0) d = -1e-300;
            if (d < 0) below++;
        }
        if (below == n) hi = mid; /* every eigenvalue lies below mid */
        else lo = mid;
    }
    return hi;
}

/* the nodal block-Jacobi inverses, and the largest eigenvalue of D^-1 A from a few Lanczos steps (conjugate gradients
 * on a fixed pseudo-random right-hand side, the tridiagonal matrix from their coefficients) */
static bool level_smoother(Level *L, const VmgKnobs *kn, ThreadPool *pool, char *err, size_t errlen) {
    const CsrMatrix *A = L->Ap;
    int n = L->neq;
    L->nblk = 0;
    for (size_t nd = 0; nd < L->nnode; nd++) {
        int any = 0;
        for (int q = 0; q < 3; q++) any |= L->eq[3 * nd + (size_t)q] >= 0;
        L->nblk += any;
    }
    L->blk_eq0 = malloc(sizeof(int) * (size_t)(L->nblk ? L->nblk : 1));
    L->blk_n = malloc((size_t)(L->nblk ? L->nblk : 1));
    L->blk_inv = calloc(9 * (size_t)(L->nblk ? L->nblk : 1), sizeof(double));
    if (!L->blk_eq0 || !L->blk_n || !L->blk_inv) return fail(err, errlen, "out of memory for the smoother");
    int b = 0;
    for (size_t nd = 0; nd < L->nnode; nd++) {
        int qs[3], m = 0;
        for (int q = 0; q < 3; q++)
            if (L->eq[3 * nd + (size_t)q] >= 0) qs[m++] = L->eq[3 * nd + (size_t)q];
        if (!m) continue;
        for (int q = 1; q < m; q++)
            if (qs[q] != qs[0] + q) return fail(err, errlen, "the multigrid smoother needs a node's equations together (equation %d)", qs[0]);
        double M[9] = {0}, Inv[9] = {0};
        for (int r = 0; r < m; r++)
            for (int c = 0; c < m; c++) {
                int64_t k = csr_find(A, qs[r], qs[c]);
                M[3 * r + c] = k >= 0 ? A->val[k] : 0;
            }
        if (m == 1) {
            if (!(M[0] > 0)) return fail(err, errlen, "the multigrid level has a non-positive diagonal at equation %d", qs[0]);
            Inv[0] = 1.0 / M[0];
        } else if (m == 2) {
            double det = M[0] * M[4] - M[1] * M[3];
            if (!(M[0] > 0) || !(det > 0)) return fail(err, errlen, "the multigrid level has a block that is not positive definite at equation %d", qs[0]);
            Inv[0] = M[4] / det, Inv[1] = -M[1] / det, Inv[3] = -M[3] / det, Inv[4] = M[0] / det;
        } else {
            double det = M[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (M[3] * M[8] - M[5] * M[6]) + M[2] * (M[3] * M[7] - M[4] * M[6]);
            if (!(M[0] > 0) || !(det > 0)) return fail(err, errlen, "the multigrid level has a block that is not positive definite at equation %d", qs[0]);
            Inv[0] = (M[4] * M[8] - M[5] * M[7]) / det, Inv[1] = (M[2] * M[7] - M[1] * M[8]) / det, Inv[2] = (M[1] * M[5] - M[2] * M[4]) / det;
            Inv[3] = (M[5] * M[6] - M[3] * M[8]) / det, Inv[4] = (M[0] * M[8] - M[2] * M[6]) / det, Inv[5] = (M[2] * M[3] - M[0] * M[5]) / det;
            Inv[6] = (M[3] * M[7] - M[4] * M[6]) / det, Inv[7] = (M[1] * M[6] - M[0] * M[7]) / det, Inv[8] = (M[0] * M[4] - M[1] * M[3]) / det;
        }
        L->blk_eq0[b] = qs[0];
        L->blk_n[b] = (unsigned char)m;
        memcpy(L->blk_inv + 9 * (size_t)b, Inv, sizeof Inv);
        b++;
    }
    /* Lanczos through conjugate gradients: 12 steps on D^-1 A from a fixed pseudo-random start */
    enum { STEPS_MAX = 64 };
    int STEPS = kn->steps;
    double *r = L->r, *z = L->d, *pv = L->t, *q = L->x, al[STEPS_MAX + 1], be[STEPS_MAX + 1], ta[STEPS_MAX + 1], tb[STEPS_MAX + 1];
    for (int i = 0; i < n; i++) {
        uint64_t s = (uint64_t)i * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
        s ^= s >> 31, s *= 0xBF58476D1CE4E5B9ull, s ^= s >> 27;
        r[i] = (double)(s >> 11) / 9007199254740992.0 - 0.5;
    }
    bj_apply(pool, L, r, z);
    vec(pool, V_COPY, n, 0, z, NULL, pv);
    double rz = vec(pool, V_DOT, n, 0, r, z, NULL);
    int steps = 0;
    for (int k = 0; k < STEPS && rz > 0; k++) {
        csr_spmv(A, pv, q, pool);
        double pq = vec(pool, V_DOT, n, 0, pv, q, NULL);
        if (!(pq > 0)) break;
        al[k] = rz / pq;
        vec(pool, V_AXPY, n, -al[k], q, NULL, r);
        bj_apply(pool, L, r, z);
        double rz2 = vec(pool, V_DOT, n, 0, r, z, NULL);
        be[k] = rz2 / rz;
        rz = rz2;
        vec(pool, V_XPBY, n, be[k], z, NULL, pv);
        steps = k + 1;
    }
    if (steps < 1) return fail(err, errlen, "the multigrid level's eigenvalue estimate failed (%d equations)", n);
    for (int k = 0; k < steps; k++) {
        ta[k] = 1.0 / al[k] + (k > 0 ? be[k - 1] / al[k - 1] : 0.0);
        if (k + 1 < steps) tb[k] = sqrt(be[k]) / al[k];
    }
    double lmax = tridiag_max(ta, tb, steps);
    L->hi = kn->safety * lmax;
    L->lo = L->hi / kn->ratio;
    return true;
}

/* Chebyshev smoothing of A x = b on [lo, hi] of D^-1 A (Saad, Iterative Methods for Sparse Linear Systems, algorithm
 * 12.1); zero: x starts at zero */
static void cheb(Level *L, const double *b, double *x, int deg, bool zero, ThreadPool *pool) {
    int n = L->neq;
    double theta = 0.5 * (L->hi + L->lo), delta = 0.5 * (L->hi - L->lo), sigma = theta / delta, rho = 1.0 / sigma;
    double *r = L->r, *d = L->d, *t = L->t;
    if (zero) {
        vec(pool, V_COPY, n, 0, b, NULL, r);
        vec(pool, V_ZERO, n, 0, NULL, NULL, x);
    } else {
        csr_spmv(L->Ap, x, t, pool);
        vec(pool, V_SUB, n, 0, b, t, r);
    }
    bj_apply(pool, L, r, d);
    vec(pool, V_SCALE, n, 1.0 / theta, NULL, NULL, d);
    for (int k = 0; k < deg; k++) {
        vec(pool, V_AXPY, n, 1.0, d, NULL, x);
        if (k == deg - 1) break;
        csr_spmv(L->Ap, d, t, pool);
        vec(pool, V_AXPY, n, -1.0, t, NULL, r);
        double rho2 = 1.0 / (2.0 * sigma - rho);
        bj_apply(pool, L, r, t);
        vec(pool, V_SCALE, n, rho2 * rho, NULL, NULL, d);
        vec(pool, V_AXPY, n, 2.0 * rho2 / delta, t, NULL, d);
        rho = rho2;
    }
}

/* one V-cycle: x = B b, x overwritten */
static void vcycle(Level *lev, int l, const double *b, double *x, int deg, ThreadPool *pool) {
    Level *L = &lev[l];
    if (L->coarsest) {
        chol_solve(&L->F, b, x);
        return;
    }
    cheb(L, b, x, deg, true, pool);
    csr_spmv(L->Ap, x, L->t, pool);
    vec(pool, V_SUB, L->neq, 0, b, L->t, L->r);
    Level *C = &lev[l + 1];
    mat_apply(pool, C->neq, C->rp, C->rc, C->rw, L->r, C->b, 0);
    vcycle(lev, l + 1, C->b, C->x, deg, pool);
    mat_apply(pool, L->neq, L->pp, L->pc, L->pw, C->x, x, 1);
    cheb(L, b, x, deg, false, pool);
}

enum { VMG_MAX_LEVELS = 12 };

bool vmg_solve(const VmgProblem *p, const CsrMatrix *K, const double *bvec, double *x, const VmgOptions *o, SolveStats *st,
               char *err, size_t errlen) {
    memset(st, 0, sizeof *st);
    double t0 = wall();
    VmgKnobs kn = vmg_knobs(o);
    int n = K->n, deg = kn.degree, cmax = kn.coarse;
    ThreadPool *pool = o->pool;
    double bnorm = sqrt(vec(pool, V_DOT, n, 0, bvec, bvec, NULL));
    if (bnorm == 0) {
        memset(x, 0, sizeof(double) * (size_t)n);
        st->converged = true;
        snprintf(st->method, sizeof st->method, "mgcg");
        return true;
    }
    Level lev[VMG_MAX_LEVELS];
    memset(lev, 0, sizeof lev);
    int nl = 0;
    bool ok = level_fine(p, K, &lev[0], err, errlen);
    if (ok) nl = 1;
    while (ok && lev[nl - 1].neq > cmax && nl < VMG_MAX_LEVELS) {
        const Level *F = &lev[nl - 1];
        if (F->nx < 2 && F->ny < 2 && F->nz < 2) break;
        if (!level_coarsen(F, p, &lev[nl], pool, err, errlen)) {
            ok = false;
            nl++;
            break;
        }
        if (lev[nl].neq <= 0 || lev[nl].neq >= F->neq) { /* nothing gained: the finer level is the coarsest */
            level_free(&lev[nl]);
            free(lev[nl - 1].pp), free(lev[nl - 1].pc), free(lev[nl - 1].pw);
            lev[nl - 1].pp = NULL, lev[nl - 1].pc = NULL, lev[nl - 1].pw = NULL;
            break;
        }
        nl++;
    }
    for (int l = 0; ok && l < nl; l++) {
        Level *L = &lev[l];
        size_t bytes = sizeof(double) * (size_t)(L->neq ? L->neq : 1);
        L->b = malloc(bytes), L->x = malloc(bytes), L->r = malloc(bytes), L->d = malloc(bytes), L->t = malloc(bytes);
        if (!L->b || !L->x || !L->r || !L->d || !L->t) ok = fail(err, errlen, "out of memory for the multigrid vectors (%d equations)", L->neq);
    }
    if (ok) {
        Level *Lc = &lev[nl - 1];
        Lc->coarsest = true;
        int bad = -1;
        SolveStats cs;
        if (!chol_factor(Lc->Ap, NULL, &Lc->F, NULL, NULL, &cs, &bad, err, errlen)) ok = false;
        else Lc->factored = true;
    }
    for (int l = 0; ok && l + 1 < nl; l++) ok = level_smoother(&lev[l], &kn, pool, err, errlen);
    double t_setup = wall() - t0;

    /* conjugate gradients preconditioned by one V-cycle */
    int it = 0;
    double rel = 1;
    if (ok) {
        double *r = malloc(sizeof(double) * (size_t)n), *z = malloc(sizeof(double) * (size_t)n), *pv = malloc(sizeof(double) * (size_t)n),
               *q = malloc(sizeof(double) * (size_t)n);
        if (!r || !z || !pv || !q) ok = fail(err, errlen, "out of memory for the conjugate gradients (%d equations)", n);
        if (ok) {
            csr_spmv(K, x, q, pool);
            vec(pool, V_SUB, n, 0, bvec, q, r);
            rel = sqrt(vec(pool, V_DOT, n, 0, r, r, NULL)) / bnorm;
            double rz = 0;
            if (rel > o->tol) {
                vcycle(lev, 0, r, z, deg, pool);
                vec(pool, V_COPY, n, 0, z, NULL, pv);
                rz = vec(pool, V_DOT, n, 0, r, z, NULL);
            }
            while (rel > o->tol && it < o->max_iter) {
                csr_spmv(K, pv, q, pool);
                double pq = vec(pool, V_DOT, n, 0, pv, q, NULL);
                if (!(pq > 0) || !(rz > 0)) {
                    ok = fail(err, errlen, "multigrid conjugate gradients broke down at iteration %d (the preconditioner or the matrix is not positive definite)", it);
                    break;
                }
                double alpha = rz / pq;
                vec(pool, V_AXPY, n, alpha, pv, NULL, x);
                vec(pool, V_AXPY, n, -alpha, q, NULL, r);
                it++;
                rel = sqrt(vec(pool, V_DOT, n, 0, r, r, NULL)) / bnorm;
                if (o->progress && it % 10 == 0 && !o->progress(o->ctx, it, rel)) {
                    st->cancelled = true;
                    ok = fail(err, errlen, "cancelled");
                    break;
                }
                if (rel <= o->tol) break;
                vcycle(lev, 0, r, z, deg, pool);
                double rz2 = vec(pool, V_DOT, n, 0, r, z, NULL);
                vec(pool, V_XPBY, n, rz2 / rz, z, NULL, pv);
                rz = rz2;
            }
            if (ok && rel > o->tol) {
                snprintf(err, errlen, "multigrid conjugate gradients did not converge in %d iterations (relative residual %.3g, target %.3g)", it,
                         rel, o->tol);
                ok = false;
            }
            csr_spmv(K, x, q, pool);
            vec(pool, V_SUB, n, 0, bvec, q, r);
            st->true_residual = sqrt(vec(pool, V_DOT, n, 0, r, r, NULL)) / bnorm;
        }
        free(r), free(z), free(pv), free(q);
    }
    st->iterations = it;
    st->rel_residual = rel;
    st->converged = ok && rel <= o->tol;
    char sizes[24] = "";
    if (nl > 0) snprintf(sizes, sizeof sizes, ", coarsest %d", lev[nl - 1].neq);
    snprintf(st->method, sizeof st->method, "mgcg/%d levels%s, %.1f s setup", nl, sizes, t_setup);
    for (int l = 0; l < nl; l++) level_free(&lev[l]);
    st->seconds = wall() - t0;
    return st->converged;
}
