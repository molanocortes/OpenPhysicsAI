/* mg3d.c - geometric multigrid for Poisson's equation (mg3d.h). */
#include "mg3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXL 16

typedef struct {
    int nx, ny, nz;
    double h;
    double *x, *f, *r;
    double *bx, *by, *bz; /* face coefficients, or NULL (all one) */
    double *id;           /* each cell's inverse diagonal (for the fast interior sweep) */
} Level;

struct Mg3D {
    int nl, bc[6];
    Level L[MAXL];
    bool singular;
    double last;
    double *kx, *kr, *kz, *kp; /* the conjugate gradients' vectors on the finest grid */
};

static inline size_t id3(const Level *l, int i, int j, int k) { return (size_t)i + (size_t)l->nx * ((size_t)j + (size_t)l->ny * k); }
static void diag_all(Mg3D *M, Level *l);

Mg3D *mg3d_create(int nx, int ny, int nz, double h, const int bc[6], char *err, size_t errlen) {
    if (nx < 1 || ny < 1 || nz < 1 || !(h > 0)) {
        snprintf(err, errlen, "mg3d: a grid and a spacing are required");
        return NULL;
    }
    Mg3D *M = calloc(1, sizeof *M);
    if (!M) return NULL;
    memcpy(M->bc, bc, sizeof M->bc);
    M->singular = true;
    for (int f = 0; f < 6; f++)
        if (bc[f] == MG_DIRICHLET) M->singular = false;
    int n[3] = {nx, ny, nz};
    double hh = h;
    for (;;) {
        Level *l = &M->L[M->nl++];
        l->nx = n[0], l->ny = n[1], l->nz = n[2], l->h = hh;
        size_t c = (size_t)n[0] * n[1] * n[2];
        l->x = calloc(c, sizeof(double)), l->f = calloc(c, sizeof(double)), l->r = calloc(c, sizeof(double));
        if (!l->x || !l->f || !l->r) {
            mg3d_free(M);
            snprintf(err, errlen, "mg3d: out of memory");
            return NULL;
        }
        diag_all(M, l);
        if (M->nl == MAXL || n[0] % 2 || n[1] % 2 || n[2] % 2 || n[0] < 4 || n[1] < 4 || n[2] < 4) break;
        n[0] /= 2, n[1] /= 2, n[2] /= 2, hh *= 2;
    }
    return M;
}

void mg3d_free(Mg3D *M) {
    if (!M) return;
    for (int k = 0; k < M->nl; k++)
        free(M->L[k].x), free(M->L[k].f), free(M->L[k].r), free(M->L[k].bx), free(M->L[k].by), free(M->L[k].bz), free(M->L[k].id);
    free(M->kx), free(M->kr), free(M->kz), free(M->kp);
    free(M);
}

bool mg3d_set_coefficients(Mg3D *M, const double *bx, const double *by, const double *bz) {
    if (!bx) {
        for (int lv = 0; lv < M->nl; lv++) {
            Level *l = &M->L[lv];
            free(l->bx), free(l->by), free(l->bz), l->bx = l->by = l->bz = NULL;
            diag_all(M, l);
        }
        return true;
    }
    for (int lv = 0; lv < M->nl; lv++) {
        Level *l = &M->L[lv];
        size_t nxf = (size_t)(l->nx + 1) * l->ny * l->nz, nyf = (size_t)l->nx * (l->ny + 1) * l->nz, nzf = (size_t)l->nx * l->ny * (l->nz + 1);
        if (!l->bx) l->bx = malloc(nxf * 8), l->by = malloc(nyf * 8), l->bz = malloc(nzf * 8); /* kept for the next call */
        if (!l->bx || !l->by || !l->bz) return false;
        if (lv == 0) {
            memcpy(l->bx, bx, nxf * 8), memcpy(l->by, by, nyf * 8), memcpy(l->bz, bz, nzf * 8);
            diag_all(M, l);
            continue;
        }
        const Level *f = &M->L[lv - 1];
        /* a coarse face covers four fine faces: their mean */
        for (int k = 0; k < l->nz; k++)
            for (int j = 0; j < l->ny; j++)
                for (int i = 0; i <= l->nx; i++) {
                    double s = 0;
                    for (int a = 0; a < 2; a++)
                        for (int b = 0; b < 2; b++) s += f->bx[(size_t)(2 * i) + (size_t)(f->nx + 1) * ((size_t)(2 * j + a) + (size_t)f->ny * (2 * k + b))];
                    l->bx[(size_t)i + (size_t)(l->nx + 1) * ((size_t)j + (size_t)l->ny * k)] = s / 4;
                }
        for (int k = 0; k < l->nz; k++)
            for (int j = 0; j <= l->ny; j++)
                for (int i = 0; i < l->nx; i++) {
                    double s = 0;
                    for (int a = 0; a < 2; a++)
                        for (int b = 0; b < 2; b++) s += f->by[(size_t)(2 * i + a) + (size_t)f->nx * ((size_t)(2 * j) + (size_t)(f->ny + 1) * (2 * k + b))];
                    l->by[(size_t)i + (size_t)l->nx * ((size_t)j + (size_t)(l->ny + 1) * k)] = s / 4;
                }
        for (int k = 0; k <= l->nz; k++)
            for (int j = 0; j < l->ny; j++)
                for (int i = 0; i < l->nx; i++) {
                    double s = 0;
                    for (int a = 0; a < 2; a++)
                        for (int b = 0; b < 2; b++) s += f->bz[(size_t)(2 * i + a) + (size_t)f->nx * ((size_t)(2 * j + b) + (size_t)f->ny * (2 * k))];
                    l->bz[(size_t)i + (size_t)l->nx * ((size_t)j + (size_t)l->ny * k)] = s / 4;
                }
        diag_all(M, l);
    }
    return true;
}

int mg3d_levels(const Mg3D *M) { return M->nl; }
double mg3d_last_residual(const Mg3D *M) { return M->last; }

/* the weighted sum of a cell's neighbours and the diagonal: sum over faces beta (x_nb - x), a Neumann face adding
 * nothing and a Dirichlet face beta (ghost - x) = -2 beta x */
static inline double nbsum(const Mg3D *M, const Level *l, const double *x, int i, int j, int k, double *diag) {
    const size_t c = id3(l, i, j, k), sy = (size_t)l->nx, sz = (size_t)l->nx * l->ny;
    if (!l->bx) {
        double s = 0, d = 6;
        if (i > 0) s += x[c - 1];
        else d += M->bc[0] == MG_DIRICHLET ? 1 : -1;
        if (i < l->nx - 1) s += x[c + 1];
        else d += M->bc[1] == MG_DIRICHLET ? 1 : -1;
        if (j > 0) s += x[c - sy];
        else d += M->bc[2] == MG_DIRICHLET ? 1 : -1;
        if (j < l->ny - 1) s += x[c + sy];
        else d += M->bc[3] == MG_DIRICHLET ? 1 : -1;
        if (k > 0) s += x[c - sz];
        else d += M->bc[4] == MG_DIRICHLET ? 1 : -1;
        if (k < l->nz - 1) s += x[c + sz];
        else d += M->bc[5] == MG_DIRICHLET ? 1 : -1;
        *diag = d;
        return s;
    }
    const size_t fx = (size_t)i + (size_t)(l->nx + 1) * ((size_t)j + (size_t)l->ny * k), fy = (size_t)i + (size_t)l->nx * ((size_t)j + (size_t)(l->ny + 1) * k);
    const size_t fz = c;
    double b[6] = {l->bx[fx], l->bx[fx + 1], l->by[fy], l->by[fy + l->nx], l->bz[fz], l->bz[fz + sz]};
    const bool in[6] = {i > 0, i < l->nx - 1, j > 0, j < l->ny - 1, k > 0, k < l->nz - 1};
    const long off[6] = {-1, 1, -(long)sy, (long)sy, -(long)sz, (long)sz};
    double s = 0, d = 0;
    for (int q = 0; q < 6; q++) {
        if (in[q]) s += b[q] * x[(long)c + off[q]], d += b[q];
        else if (M->bc[q] == MG_DIRICHLET) d += 2 * b[q];
    }
    *diag = d;
    return s;
}

typedef struct {
    Mg3D *M;
    Level *l, *c;
    int color;
} Job;

/* small grids on the calling thread: a pool's dispatch costs more than their work */
static void run(ThreadPool *pool, const Level *l, ParallelFn fn, void *ctx) {
    if ((size_t)l->nx * l->ny * l->nz < 16384) fn(ctx, 0, l->nz, 0);
    else pool_for(pool, l->nz, 1, fn, ctx);
}

static void gs_slab(void *vj, int k0, int k1, int tid) {
    Job *J = vj;
    Level *l = J->l;
    const double h2 = l->h * l->h;
    const int nx = l->nx, ny = l->ny, nz = l->nz;
    const size_t sy = (size_t)nx, sz = (size_t)nx * ny;
    double *x = l->x;
    const double *f = l->f, *id = l->id;
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < ny; j++) {
            int i0 = (j + k + J->color) & 1;
            bool inner = id && j > 0 && j < ny - 1 && k > 0 && k < nz - 1;
            for (int i = i0; i < nx; i += 2) {
                size_t c = id3(l, i, j, k);
                if (inner && i > 0 && i < nx - 1) { /* the interior: no face of the box, the diagonal known */
                    double sum;
                    if (l->bx) {
                        const size_t fx = (size_t)i + (size_t)(nx + 1) * ((size_t)j + (size_t)ny * k), fy = (size_t)i + (size_t)nx * ((size_t)j + (size_t)(ny + 1) * k);
                        sum = l->bx[fx] * x[c - 1] + l->bx[fx + 1] * x[c + 1] + l->by[fy] * x[c - sy] + l->by[fy + sy] * x[c + sy] + l->bz[c] * x[c - sz] +
                              l->bz[c + sz] * x[c + sz];
                    } else
                        sum = x[c - 1] + x[c + 1] + x[c - sy] + x[c + sy] + x[c - sz] + x[c + sz];
                    x[c] = (sum - h2 * f[c]) * id[c];
                    continue;
                }
                double d, s = nbsum(J->M, l, x, i, j, k, &d);
                x[c] = d > 0 ? (s - h2 * f[c]) / d : 0; /* a cell with every face closed (inside a solid) drops out */
            }
        }
}

/* every cell's inverse diagonal, after the coefficients change */
static void diag_all(Mg3D *M, Level *l) {
    size_t n = (size_t)l->nx * l->ny * l->nz;
    if (!l->id) l->id = malloc(n * sizeof(double));
    if (!l->id) return;
    for (int k = 0; k < l->nz; k++)
        for (int j = 0; j < l->ny; j++)
            for (int i = 0; i < l->nx; i++) {
                double d;
                nbsum(M, l, l->x, i, j, k, &d);
                l->id[id3(l, i, j, k)] = d > 0 ? 1 / d : 0;
            }
}
static void smooth(Mg3D *M, Level *l, int sweeps, bool reverse, ThreadPool *pool) {
    for (int s = 0; s < sweeps; s++)
        for (int c = 0; c < 2; c++) {
            Job J = {M, l, NULL, reverse ? 1 - c : c};
            run(pool, l, gs_slab, &J);
        }
}

static void residual_slab(void *vj, int k0, int k1, int tid) {
    Job *J = vj;
    Level *l = J->l;
    const double ih2 = 1 / (l->h * l->h);
    const int nx = l->nx, ny = l->ny, nz = l->nz;
    const size_t sy = (size_t)nx, sz = (size_t)nx * ny;
    const double *x = l->x, *f = l->f;
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < ny; j++) {
            bool inner = j > 0 && j < ny - 1 && k > 0 && k < nz - 1;
            for (int i = 0; i < nx; i++) {
                size_t c = id3(l, i, j, k);
                if (inner && i > 0 && i < nx - 1) { /* the interior: no face of the box */
                    double s, d;
                    if (l->bx) {
                        const size_t fx = (size_t)i + (size_t)(nx + 1) * ((size_t)j + (size_t)ny * k), fy = (size_t)i + (size_t)nx * ((size_t)j + (size_t)(ny + 1) * k);
                        const double b0 = l->bx[fx], b1 = l->bx[fx + 1], b2 = l->by[fy], b3 = l->by[fy + sy], b4 = l->bz[c], b5 = l->bz[c + sz];
                        s = b0 * x[c - 1] + b1 * x[c + 1] + b2 * x[c - sy] + b3 * x[c + sy] + b4 * x[c - sz] + b5 * x[c + sz];
                        d = b0 + b1 + b2 + b3 + b4 + b5;
                    } else
                        s = x[c - 1] + x[c + 1] + x[c - sy] + x[c + sy] + x[c - sz] + x[c + sz], d = 6;
                    l->r[c] = f[c] - (s - d * x[c]) * ih2;
                    continue;
                }
                double d, s = nbsum(J->M, l, l->x, i, j, k, &d);
                l->r[c] = f[c] - (s - d * x[c]) * ih2;
            }
        }
}
static void residual(Mg3D *M, Level *l, ThreadPool *pool) {
    Job J = {M, l, NULL, 0};
    run(pool, l, residual_slab, &J);
}

static void restrict_slab(void *vj, int k0, int k1, int tid) {
    Job *J = vj;
    Level *l = J->l, *c = J->c;
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < c->ny; j++)
            for (int i = 0; i < c->nx; i++) {
                double s = 0;
                for (int d = 0; d < 8; d++) s += l->r[id3(l, 2 * i + (d & 1), 2 * j + ((d >> 1) & 1), 2 * k + (d >> 2))];
                c->f[id3(c, i, j, k)] = s / 8, c->x[id3(c, i, j, k)] = 0;
            }
}

/* the coarse correction at a fine cell: along each axis 3/4 of the coarse cell holding it and 1/4 of its neighbour on
 * the fine cell's side (beyond the box, the ghost the face implies) */
/* the coefficient of a boundary face of the coarse grid: the side a (0 x, 1 y, 2 z), high or low, at cell q */
static inline double side_beta(const Level *c, int a, bool high, const int q[3]) {
    if (!c->bx) return 1;
    if (a == 0) return c->bx[(size_t)(high ? c->nx : 0) + (size_t)(c->nx + 1) * ((size_t)q[1] + (size_t)c->ny * q[2])];
    if (a == 1) return c->by[(size_t)q[0] + (size_t)c->nx * ((size_t)(high ? c->ny : 0) + (size_t)(c->ny + 1) * q[2])];
    return c->bz[(size_t)q[0] + (size_t)c->nx * ((size_t)q[1] + (size_t)c->ny * (high ? c->nz : 0))];
}
static inline double coarse_at(const Mg3D *M, const Level *c, int I, int J, int K) {
    int n[3] = {c->nx, c->ny, c->nz}, q[3] = {I, J, K};
    bool lo[3], hi[3];
    for (int a = 0; a < 3; a++) {
        lo[a] = q[a] < 0, hi[a] = q[a] >= n[a];
        if (lo[a]) q[a] = 0;
        if (hi[a]) q[a] = n[a] - 1;
    }
    /* the ghost beyond a side: the negative on a Dirichlet face, the cell itself on a Neumann face or on a Dirichlet
     * side's closed face (beta 0: a wall in a side that is open elsewhere) */
    double sgn = 1;
    for (int a = 0; a < 3; a++) {
        if (lo[a] && M->bc[2 * a] == MG_DIRICHLET && side_beta(c, a, false, q) != 0) sgn = -sgn;
        if (hi[a] && M->bc[2 * a + 1] == MG_DIRICHLET && side_beta(c, a, true, q) != 0) sgn = -sgn;
    }
    return sgn * c->x[id3(c, q[0], q[1], q[2])];
}
static void prolong_slab(void *vj, int k0, int k1, int tid) {
    Job *J = vj;
    Level *l = J->l, *c = J->c;
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < l->ny; j++)
            for (int i = 0; i < l->nx; i++) {
                int I = i / 2, Jj = j / 2, K = k / 2, di = (i & 1) ? 1 : -1, dj = (j & 1) ? 1 : -1, dk = (k & 1) ? 1 : -1;
                double v = 0;
                for (int a = 0; a < 2; a++)
                    for (int b = 0; b < 2; b++)
                        for (int e = 0; e < 2; e++) {
                            double w = (a ? 0.25 : 0.75) * (b ? 0.25 : 0.75) * (e ? 0.25 : 0.75);
                            v += w * coarse_at(J->M, c, I + a * di, Jj + b * dj, K + e * dk);
                        }
                l->x[id3(l, i, j, k)] += v;
            }
}

/* the mean over the cells that take part (a cell with every face closed does not): a singular problem's right side
 * must have none, and its solution is defined without one */
static void remove_mean(const Level *l, double *a) {
    size_t n = (size_t)l->nx * l->ny * l->nz, na = 0;
    double m = 0;
    for (size_t q = 0; q < n; q++)
        if (!l->id || l->id[q] != 0) m += a[q], na++;
    if (!na) return;
    m /= na;
    for (size_t q = 0; q < n; q++)
        if (!l->id || l->id[q] != 0) a[q] -= m;
}

static void vcycle(Mg3D *M, int lev, ThreadPool *pool) {
    Level *l = &M->L[lev];
    if (lev == M->nl - 1) {
        smooth(M, l, 30, false, pool), smooth(M, l, 30, true, pool);
        return;
    }
    smooth(M, l, 2, false, pool);
    residual(M, l, pool);
    Level *c = &M->L[lev + 1];
    Job J = {M, l, c, 0};
    run(pool, c, restrict_slab, &J);
    if (M->singular) remove_mean(c, c->f); /* the coarse right side's mean is not in the range of a Neumann operator */
    vcycle(M, lev + 1, pool);
    run(pool, l, prolong_slab, &J);
    smooth(M, l, 2, true, pool); /* red-black, then black-red: a symmetric cycle, as the conjugate gradients want */
}

static double dot(const double *a, const double *b, size_t n) {
    double s = 0;
    for (size_t q = 0; q < n; q++) s += a[q] * b[q];
    return s;
}
/* z = the V-cycle's approximation of the operator's inverse applied to r */
static void precondition(Mg3D *M, const double *r, double *z, ThreadPool *pool) {
    Level *l = &M->L[0];
    size_t n = (size_t)l->nx * l->ny * l->nz;
    memcpy(l->f, r, n * sizeof(double)), memset(l->x, 0, n * sizeof(double));
    vcycle(M, 0, pool);
    if (M->singular) remove_mean(l, l->x);
    memcpy(z, l->x, n * sizeof(double));
}

/* Conjugate gradients preconditioned by one V-cycle (flexible: Polak-Ribiere's beta, which tolerates a cycle that is not
 * exactly symmetric). Multigrid alone stalls where its coarse grids misrepresent a few modes (a room joined to the
 * outside by a doorway: thin walls average away on coarse grids); the gradients remove those few in a few iterations. */
int mg3d_solve(Mg3D *M, double *x, const double *f, double tol, int maxcycles, ThreadPool *pool) {
    Level *l = &M->L[0];
    size_t n = (size_t)l->nx * l->ny * l->nz;
    if (!M->kx) M->kx = malloc(n * 8), M->kr = malloc(n * 8), M->kz = malloc(n * 8), M->kp = malloc(n * 8);
    if (!M->kx || !M->kr || !M->kz || !M->kp) return -1;
    double *X = M->kx, *R = M->kr, *Z = M->kz, *P = M->kp;
    memcpy(X, x, n * sizeof(double)), memcpy(l->f, f, n * sizeof(double));
    if (M->singular) remove_mean(l, l->f);
    double fn = sqrt(dot(l->f, l->f, n));
    memcpy(l->x, X, n * sizeof(double));
    residual(M, l, pool);
    memcpy(R, l->r, n * sizeof(double));
    const bool trace = getenv("MG_TRACE") != NULL;
    M->last = fn > 0 ? sqrt(dot(R, R, n)) / fn : sqrt(dot(R, R, n));
    int used = M->last <= tol ? 0 : -1;
    if (used < 0) {
        precondition(M, R, Z, pool);
        memcpy(P, Z, n * sizeof(double));
        double rz = dot(R, Z, n);
        for (int it = 1; it <= maxcycles; it++) {
            /* Q = A P, in the level's residual array: the residual of P with no right side is -A P */
            memcpy(l->x, P, n * sizeof(double)), memset(l->f, 0, n * sizeof(double));
            residual(M, l, pool);
            double *Q = l->r, pq = -dot(P, Q, n);
            if (pq == 0 || rz == 0) break;
            double alpha = rz / pq;
            for (size_t q = 0; q < n; q++) X[q] += alpha * P[q], R[q] += alpha * Q[q];
            double rn = sqrt(dot(R, R, n));
            M->last = fn > 0 ? rn / fn : rn;
            if (trace) fprintf(stderr, "mg3d: iteration %d residual %.3e\n", it, M->last);
            if (M->last <= tol) {
                used = it;
                break;
            }
            double rzo = dot(R, Z, n);
            precondition(M, R, Z, pool);
            double rzn = dot(R, Z, n), beta = (rzn - rzo) / rz;
            rz = rzn;
            for (size_t q = 0; q < n; q++) P[q] = Z[q] + beta * P[q];
        }
    }
    if (M->singular) remove_mean(l, X);
    memcpy(x, X, n * sizeof(double));
    return used;
}
