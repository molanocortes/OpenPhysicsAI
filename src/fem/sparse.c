/* sparse.c - CSR assembly, PCG and sparse Cholesky */
#include "sparse.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double wall(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : (x > y);
}

bool csr_from_elements(CsrMatrix *A, int n, int nelem, int npe, const int *ed, char *err, size_t errlen) {
    memset(A, 0, sizeof *A);
    A->n = n;
    /* equation -> element incidence */
    int64_t *ip = calloc((size_t)n + 1, sizeof(int64_t));
    if (!ip) goto oom;
    for (int e = 0; e < nelem; e++)
        for (int a = 0; a < npe; a++) {
            int d = ed[(int64_t)e * npe + a];
            if (d >= 0) {
                if (d >= n) {
                    snprintf(err, errlen, "element %d refers to equation %d beyond %d", e, d, n);
                    free(ip);
                    return false;
                }
                ip[d + 1]++;
            }
        }
    for (int i = 0; i < n; i++) ip[i + 1] += ip[i];
    int *inc = malloc((size_t)(ip[n] ? ip[n] : 1) * sizeof(int));
    int64_t *fill = malloc((size_t)(n ? n : 1) * sizeof(int64_t));
    int *mark = malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!inc || !fill || !mark) {
        free(ip), free(inc), free(fill), free(mark);
        goto oom;
    }
    memcpy(fill, ip, (size_t)n * sizeof(int64_t));
    for (int e = 0; e < nelem; e++) {
        int seen[64];
        int ns = 0;
        for (int a = 0; a < npe; a++) {
            int d = ed[(int64_t)e * npe + a];
            if (d < 0) continue;
            bool dup = false;
            for (int k = 0; k < ns && !dup; k++) dup = seen[k] == d;
            if (dup) continue; /* an element listing an equation twice adds one incidence */
            if (ns < 64) seen[ns++] = d;
            inc[fill[d]++] = e;
        }
    }
    /* the duplicate filter may leave unused slots: compact the incidence lists */
    {
        int64_t w = 0;
        int64_t start = 0;
        for (int i = 0; i < n; i++) {
            int64_t end = fill[i];
            int64_t ns = w;
            for (int64_t k = start; k < end; k++) inc[w++] = inc[k];
            start = ip[i + 1];
            ip[i] = ns;
        }
        ip[n] = w;
    }
    for (int i = 0; i < n; i++) mark[i] = -1;
    A->rowptr = calloc((size_t)n + 1, sizeof(int64_t));
    if (!A->rowptr) {
        free(ip), free(inc), free(fill), free(mark);
        goto oom;
    }
    for (int i = 0; i < n; i++) {
        int64_t cnt = 0;
        for (int64_t k = ip[i]; k < ip[i + 1]; k++) {
            int e = inc[k];
            for (int a = 0; a < npe; a++) {
                int d = ed[(int64_t)e * npe + a];
                if (d >= 0 && mark[d] != i) mark[d] = i, cnt++;
            }
        }
        A->rowptr[i + 1] = A->rowptr[i] + cnt;
    }
    A->nnz = A->rowptr[n];
    A->col = malloc((size_t)(A->nnz ? A->nnz : 1) * sizeof(int));
    A->val = calloc((size_t)(A->nnz ? A->nnz : 1), sizeof(double));
    if (!A->col || !A->val) {
        free(ip), free(inc), free(fill), free(mark);
        csr_free(A);
        goto oom;
    }
    for (int i = 0; i < n; i++) mark[i] = -1;
    for (int i = 0; i < n; i++) {
        int64_t w = A->rowptr[i];
        for (int64_t k = ip[i]; k < ip[i + 1]; k++) {
            int e = inc[k];
            for (int a = 0; a < npe; a++) {
                int d = ed[(int64_t)e * npe + a];
                if (d >= 0 && mark[d] != i) mark[d] = i, A->col[w++] = d;
            }
        }
        qsort(A->col + A->rowptr[i], (size_t)(A->rowptr[i + 1] - A->rowptr[i]), sizeof(int), cmp_int);
    }
    free(ip), free(inc), free(fill), free(mark);
    return true;
oom:
    snprintf(err, errlen, "out of memory building the sparse matrix pattern (%d equations)", n);
    return false;
}

void csr_free(CsrMatrix *A) {
    free(A->rowptr);
    free(A->col);
    free(A->val);
    memset(A, 0, sizeof *A);
}

void csr_zero(CsrMatrix *A) { memset(A->val, 0, (size_t)A->nnz * sizeof(double)); }

double csr_memory_mb(const CsrMatrix *A) { return ((double)A->nnz * 12.0 + (double)(A->n + 1) * 8.0) / 1048576.0; }

int64_t csr_find(const CsrMatrix *A, int row, int col) {
    int64_t lo = A->rowptr[row], hi = A->rowptr[row + 1] - 1;
    while (lo <= hi) {
        int64_t mid = (lo + hi) / 2;
        if (A->col[mid] < col) lo = mid + 1;
        else if (A->col[mid] > col) hi = mid - 1;
        else return mid;
    }
    return -1;
}

void csr_add_element(CsrMatrix *A, int npe, const int *dofs, const double *Ke) {
    for (int a = 0; a < npe; a++) {
        int i = dofs[a];
        if (i < 0) continue;
        for (int b = 0; b < npe; b++) {
            int j = dofs[b];
            if (j < 0) continue;
            int64_t k = csr_find(A, i, j);
            if (k >= 0) A->val[k] += Ke[a * npe + b];
        }
    }
}

typedef struct {
    const CsrMatrix *A;
    const double *x;
    double *y;
} SpmvCtx;

static void spmv_range(void *ctx, int begin, int end, int tid) {
    SpmvCtx *c = ctx;
    const CsrMatrix *A = c->A;
    for (int i = begin; i < end; i++) {
        double s = 0;
        for (int64_t k = A->rowptr[i]; k < A->rowptr[i + 1]; k++) s += A->val[k] * c->x[A->col[k]];
        c->y[i] = s;
    }
}

void csr_spmv(const CsrMatrix *A, const double *x, double *y, ThreadPool *pool) {
    SpmvCtx c = {A, x, y};
    if (pool && A->n > 20000) pool_for(pool, A->n, 2048, spmv_range, &c);
    else spmv_range(&c, 0, A->n, 0);
}

/* ---- PCG --------------------------------------------------------------------------------------- */

static double dot(const double *a, const double *b, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

typedef struct {
    int n;
    double *inv_diag;    /* Jacobi */
    int nblocks;
    int *block_start;    /* nblocks + 1 over a permutation of equations */
    int *block_eq;       /* equations grouped by block */
    double *block_inv;   /* 9 per block (row-major, size^2 used) */
} Precond;

static void precond_free(Precond *P) {
    free(P->inv_diag);
    free(P->block_start);
    free(P->block_eq);
    free(P->block_inv);
}

static bool precond_build(const CsrMatrix *A, const PcgOptions *o, Precond *P, char *err, size_t errlen) {
    memset(P, 0, sizeof *P);
    P->n = A->n;
    if (o->precond == PRECOND_BLOCK_JACOBI && o->block_of) {
        int nb = 0;
        for (int i = 0; i < A->n; i++)
            if (o->block_of[i] + 1 > nb) nb = o->block_of[i] + 1;
        P->nblocks = nb;
        P->block_start = calloc((size_t)nb + 1, sizeof(int));
        P->block_eq = malloc((size_t)(A->n ? A->n : 1) * sizeof(int));
        P->block_inv = calloc((size_t)(nb ? nb : 1) * 9, sizeof(double));
        if (!P->block_start || !P->block_eq || !P->block_inv) goto oom;
        for (int i = 0; i < A->n; i++) P->block_start[o->block_of[i] + 1]++;
        for (int b = 0; b < nb; b++) {
            if (P->block_start[b + 1] > 3) {
                snprintf(err, errlen, "block-Jacobi blocks may hold at most 3 equations");
                precond_free(P);
                return false;
            }
            P->block_start[b + 1] += P->block_start[b];
        }
        int *fill = malloc((size_t)(nb ? nb : 1) * sizeof(int));
        if (!fill) goto oom;
        memcpy(fill, P->block_start, (size_t)nb * sizeof(int));
        for (int i = 0; i < A->n; i++) P->block_eq[fill[o->block_of[i]]++] = i;
        free(fill);
        for (int b = 0; b < nb; b++) {
            int s = P->block_start[b], m = P->block_start[b + 1] - s;
            double M[9] = {0};
            for (int r = 0; r < m; r++)
                for (int c = 0; c < m; c++) {
                    int64_t k = csr_find(A, P->block_eq[s + r], P->block_eq[s + c]);
                    M[r * 3 + c] = k >= 0 ? A->val[k] : 0;
                }
            /* invert the m x m block (m <= 3) */
            double Inv[9] = {0};
            if (m == 1) {
                if (!(M[0] > 0)) goto notspd;
                Inv[0] = 1.0 / M[0];
            } else if (m == 2) {
                double det = M[0] * M[4] - M[1] * M[3];
                if (!(M[0] > 0) || !(det > 0)) goto notspd;
                Inv[0] = M[4] / det, Inv[1] = -M[1] / det, Inv[3] = -M[3] / det, Inv[4] = M[0] / det;
            } else {
                double det = M[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (M[3] * M[8] - M[5] * M[6]) + M[2] * (M[3] * M[7] - M[4] * M[6]);
                if (!(M[0] > 0) || !(det > 0)) goto notspd;
                Inv[0] = (M[4] * M[8] - M[5] * M[7]) / det, Inv[1] = (M[2] * M[7] - M[1] * M[8]) / det, Inv[2] = (M[1] * M[5] - M[2] * M[4]) / det;
                Inv[3] = (M[5] * M[6] - M[3] * M[8]) / det, Inv[4] = (M[0] * M[8] - M[2] * M[6]) / det, Inv[5] = (M[2] * M[3] - M[0] * M[5]) / det;
                Inv[6] = (M[3] * M[7] - M[4] * M[6]) / det, Inv[7] = (M[1] * M[6] - M[0] * M[7]) / det, Inv[8] = (M[0] * M[4] - M[1] * M[3]) / det;
            }
            memcpy(P->block_inv + 9 * (size_t)b, Inv, sizeof Inv);
            continue;
        notspd:
            snprintf(err, errlen, "matrix block at equation %d is not positive definite (missing stiffness or constraint)", P->block_eq[s]);
            precond_free(P);
            return false;
        }
        return true;
    }
    P->inv_diag = malloc((size_t)(A->n ? A->n : 1) * sizeof(double));
    if (!P->inv_diag) goto oom;
    for (int i = 0; i < A->n; i++) {
        int64_t k = csr_find(A, i, i);
        double d = k >= 0 ? A->val[k] : 0;
        if (!(d > 0)) {
            snprintf(err, errlen, "diagonal entry %d is not positive (%g): missing stiffness or constraint", i, d);
            precond_free(P);
            return false;
        }
        P->inv_diag[i] = 1.0 / d;
    }
    return true;
oom:
    snprintf(err, errlen, "out of memory building the preconditioner");
    precond_free(P);
    return false;
}

static void precond_apply(const Precond *P, const double *r, double *z) {
    if (P->inv_diag) {
        for (int i = 0; i < P->n; i++) z[i] = P->inv_diag[i] * r[i];
        return;
    }
    for (int b = 0; b < P->nblocks; b++) {
        int s = P->block_start[b], m = P->block_start[b + 1] - s;
        const double *Inv = P->block_inv + 9 * (size_t)b;
        for (int rr = 0; rr < m; rr++) {
            double v = 0;
            for (int c = 0; c < m; c++) v += Inv[rr * 3 + c] * r[P->block_eq[s + c]];
            z[P->block_eq[s + rr]] = v;
        }
    }
}

bool pcg_solve(const CsrMatrix *A, const double *b, double *x, const PcgOptions *o, SolveStats *st, char *err, size_t errlen) {
    memset(st, 0, sizeof *st);
    double t0 = wall();
    int n = A->n;
    snprintf(st->method, sizeof st->method, "pcg/%s", o->precond == PRECOND_BLOCK_JACOBI && o->block_of ? "block-jacobi" : "jacobi");
    double bnorm = sqrt(dot(b, b, n));
    if (bnorm == 0) {
        memset(x, 0, (size_t)n * sizeof(double));
        st->converged = true;
        st->seconds = wall() - t0;
        return true;
    }
    Precond P;
    if (!precond_build(A, o, &P, err, errlen)) return false;
    double *r = malloc((size_t)n * sizeof(double)), *z = malloc((size_t)n * sizeof(double));
    double *p = malloc((size_t)n * sizeof(double)), *Ap = malloc((size_t)n * sizeof(double));
    if (!r || !z || !p || !Ap) {
        free(r), free(z), free(p), free(Ap);
        precond_free(&P);
        snprintf(err, errlen, "out of memory for PCG vectors");
        return false;
    }
    csr_spmv(A, x, Ap, o->pool);
    for (int i = 0; i < n; i++) r[i] = b[i] - Ap[i];
    precond_apply(&P, r, z);
    memcpy(p, z, (size_t)n * sizeof(double));
    double rz = dot(r, z, n);
    double rel = sqrt(dot(r, r, n)) / bnorm;
    int it = 0;
    bool ok = true;
    while (rel > o->tol && it < o->max_iter) {
        csr_spmv(A, p, Ap, o->pool);
        double pAp = dot(p, Ap, n);
        if (!(pAp > 0)) {
            snprintf(err, errlen, "conjugate gradients broke down at iteration %d (p.Ap = %g): the matrix is not positive definite", it, pAp);
            ok = false;
            break;
        }
        double alpha = rz / pAp;
        for (int i = 0; i < n; i++) {
            x[i] += alpha * p[i];
            r[i] -= alpha * Ap[i];
        }
        it++;
        rel = sqrt(dot(r, r, n)) / bnorm;
        if (o->progress && (it % 10 == 0) && !o->progress(o->ctx, it, rel)) {
            st->cancelled = true;
            ok = false;
            snprintf(err, errlen, "cancelled");
            break;
        }
        precond_apply(&P, r, z);
        double rz_new = dot(r, z, n);
        double beta = rz_new / rz;
        rz = rz_new;
        for (int i = 0; i < n; i++) p[i] = z[i] + beta * p[i];
    }
    st->iterations = it;
    st->rel_residual = rel;
    st->converged = ok && rel <= o->tol;
    /* independent residual check */
    csr_spmv(A, x, Ap, o->pool);
    double rr = 0;
    for (int i = 0; i < n; i++) rr += (b[i] - Ap[i]) * (b[i] - Ap[i]);
    st->true_residual = sqrt(rr) / bnorm;
    if (ok && !st->converged) snprintf(err, errlen, "conjugate gradients did not converge in %d iterations (relative residual %.3g, target %.3g)", it, rel, o->tol);
    free(r), free(z), free(p), free(Ap);
    precond_free(&P);
    st->seconds = wall() - t0;
    return st->converged;
}

/* ---- nested dissection ------------------------------------------------------------------------- */

typedef struct {
    int start, count, out_end;
} NdTask;

static void nd_select(int *nodes, const double *xyz, int axis, int lo, int hi, int k) {
    while (hi - lo > 1) {
        double pivot = xyz[3 * nodes[lo + (hi - lo) / 2] + axis];
        int i = lo, j = hi - 1;
        while (i <= j) {
            while (xyz[3 * nodes[i] + axis] < pivot) i++;
            while (xyz[3 * nodes[j] + axis] > pivot) j--;
            if (i <= j) {
                int t = nodes[i];
                nodes[i] = nodes[j], nodes[j] = t;
                i++, j--;
            }
        }
        if (k <= j) hi = j + 1;
        else if (k >= i) lo = i;
        else return;
    }
}

bool nested_dissection_order(int nnodes, const double *xyz, const int64_t *adjp, const int *adj, const int *node_eq, int neq,
                             int *perm, char *err, size_t errlen) {
    int *nodes = malloc((size_t)(nnodes ? nnodes : 1) * sizeof(int));
    int *order = malloc((size_t)(nnodes ? nnodes : 1) * sizeof(int));
    int *label = malloc((size_t)(nnodes ? nnodes : 1) * sizeof(int));
    int cap = 1024;
    NdTask *stack = malloc((size_t)cap * sizeof(NdTask));
    if (!nodes || !order || !label || !stack) {
        free(nodes), free(order), free(label), free(stack);
        snprintf(err, errlen, "out of memory for the ordering");
        return false;
    }
    /* only nodes that carry equations take part */
    int m = 0;
    for (int v = 0; v < nnodes; v++) {
        label[v] = -1;
        if (node_eq[3 * v] >= 0 || node_eq[3 * v + 1] >= 0 || node_eq[3 * v + 2] >= 0) nodes[m++] = v;
    }
    int sp = 0, next_label = 0;
    stack[sp++] = (NdTask){0, m, m};
    enum { LEAF = 48 };
    while (sp) {
        NdTask t = stack[--sp];
        if (t.count <= LEAF) {
            memcpy(order + t.out_end - t.count, nodes + t.start, (size_t)t.count * sizeof(int));
            continue;
        }
        double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int i = t.start; i < t.start + t.count; i++)
            for (int k = 0; k < 3; k++) {
                double c = xyz[3 * nodes[i] + k];
                if (c < lo[k]) lo[k] = c;
                if (c > hi[k]) hi[k] = c;
            }
        int axis = 0;
        for (int k = 1; k < 3; k++)
            if (hi[k] - lo[k] > hi[axis] - lo[axis]) axis = k;
        if (!(hi[axis] > lo[axis])) {
            memcpy(order + t.out_end - t.count, nodes + t.start, (size_t)t.count * sizeof(int));
            continue;
        }
        int half = t.count / 2;
        nd_select(nodes, xyz, axis, t.start, t.start + t.count, t.start + half);
        int labL = next_label++, labR = next_label++;
        for (int i = t.start; i < t.start + half; i++) label[nodes[i]] = labL;
        for (int i = t.start + half; i < t.start + t.count; i++) label[nodes[i]] = labR;
        /* separator: left nodes with a neighbour on the right; move them to the end of the left segment */
        int wl = t.start, nsep = 0;
        int *sepbuf = order + t.out_end - t.count; /* scratch inside this task's output range */
        for (int i = t.start; i < t.start + half; i++) {
            int v = nodes[i];
            bool sep = false;
            for (int64_t k = adjp[v]; k < adjp[v + 1] && !sep; k++) sep = label[adj[k]] == labR;
            if (sep) sepbuf[nsep++] = v;
            else nodes[wl++] = v;
        }
        int nL = wl - t.start, nR = t.count - half;
        /* separator numbered last */
        memmove(order + t.out_end - nsep, sepbuf, (size_t)nsep * sizeof(int));
        if (sp + 2 > cap) {
            cap *= 2;
            NdTask *ns = realloc(stack, (size_t)cap * sizeof(NdTask));
            if (!ns) {
                free(nodes), free(order), free(label), free(stack);
                snprintf(err, errlen, "out of memory for the ordering");
                return false;
            }
            stack = ns;
        }
        /* the right part keeps its position in nodes[]; the left part was compacted to the front of its segment */
        stack[sp++] = (NdTask){t.start + half, nR, t.out_end - nsep};
        stack[sp++] = (NdTask){t.start, nL, t.out_end - nsep - nR};
    }
    int w = 0;
    for (int i = 0; i < m; i++)
        for (int k = 0; k < 3; k++) {
            int eq = node_eq[3 * order[i] + k];
            if (eq >= 0) perm[w++] = eq;
        }
    free(nodes), free(order), free(label), free(stack);
    if (w != neq) {
        snprintf(err, errlen, "ordering covers %d of %d equations", w, neq);
        return false;
    }
    return true;
}

/* ---- sparse Cholesky --------------------------------------------------------------------------- */

/* Upper triangle of C = P A P^T stored by columns (Cp, Ci, Cx). */
static bool permute_upper(const CsrMatrix *A, const int *iperm, int64_t **Cp_out, int **Ci_out, double **Cx_out) {
    int n = A->n;
    int64_t *Cp = calloc((size_t)n + 1, sizeof(int64_t));
    if (!Cp) return false;
    for (int i = 0; i < n; i++) {
        int pi = iperm ? iperm[i] : i;
        for (int64_t k = A->rowptr[i]; k < A->rowptr[i + 1]; k++) {
            int pj = iperm ? iperm[A->col[k]] : A->col[k];
            if (pi <= pj) Cp[pj + 1]++;
        }
    }
    for (int j = 0; j < n; j++) Cp[j + 1] += Cp[j];
    int *Ci = malloc((size_t)(Cp[n] ? Cp[n] : 1) * sizeof(int));
    double *Cx = Cx_out ? malloc((size_t)(Cp[n] ? Cp[n] : 1) * sizeof(double)) : NULL;
    int64_t *fill = malloc((size_t)(n ? n : 1) * sizeof(int64_t));
    if (!Ci || (Cx_out && !Cx) || !fill) {
        free(Cp), free(Ci), free(Cx), free(fill);
        return false;
    }
    memcpy(fill, Cp, (size_t)n * sizeof(int64_t));
    for (int i = 0; i < n; i++) {
        int pi = iperm ? iperm[i] : i;
        for (int64_t k = A->rowptr[i]; k < A->rowptr[i + 1]; k++) {
            int pj = iperm ? iperm[A->col[k]] : A->col[k];
            if (pi <= pj) {
                int64_t q = fill[pj]++;
                Ci[q] = pi;
                if (Cx) Cx[q] = A->val[k];
            }
        }
    }
    free(fill);
    *Cp_out = Cp;
    *Ci_out = Ci;
    if (Cx_out) *Cx_out = Cx;
    return true;
}

static void etree_upper(int n, const int64_t *Cp, const int *Ci, int *parent, int *ancestor) {
    for (int k = 0; k < n; k++) {
        parent[k] = -1;
        ancestor[k] = -1;
        for (int64_t p = Cp[k]; p < Cp[k + 1]; p++) {
            int i = Ci[p];
            while (i != -1 && i < k) {
                int inext = ancestor[i];
                ancestor[i] = k;
                if (inext == -1) parent[i] = k;
                i = inext;
            }
        }
    }
}

/* nonzero pattern of row k of L (excluding the diagonal) in s[top..n-1] */
static int ereach(int k, const int64_t *Cp, const int *Ci, const int *parent, int *s, int *w, int n) {
    int top = n;
    w[k] = k;
    for (int64_t p = Cp[k]; p < Cp[k + 1]; p++) {
        int i = Ci[p];
        if (i > k) continue;
        int len = 0;
        for (; w[i] != k; i = parent[i]) {
            s[len++] = i;
            w[i] = k;
        }
        while (len > 0) s[--top] = s[--len];
    }
    return top;
}

static bool symbolic_counts(const CsrMatrix *A, const int *iperm, int64_t **Cp, int **Ci, double **Cx, int **parent, int64_t **colcount) {
    int n = A->n;
    if (!permute_upper(A, iperm, Cp, Ci, Cx)) return false;
    *parent = malloc((size_t)(n ? n : 1) * sizeof(int));
    int *anc = malloc((size_t)(n ? n : 1) * sizeof(int));
    int *s = malloc((size_t)(n ? n : 1) * sizeof(int));
    int *w = malloc((size_t)(n ? n : 1) * sizeof(int));
    *colcount = calloc((size_t)n + 1, sizeof(int64_t));
    if (!*parent || !anc || !s || !w || !*colcount) {
        free(anc), free(s), free(w);
        return false;
    }
    etree_upper(n, *Cp, *Ci, *parent, anc);
    for (int i = 0; i < n; i++) w[i] = -1;
    for (int k = 0; k < n; k++) {
        (*colcount)[k]++; /* diagonal */
        int top = ereach(k, *Cp, *Ci, *parent, s, w, n);
        for (; top < n; top++) (*colcount)[s[top]]++;
    }
    free(anc), free(s), free(w);
    return true;
}

int64_t chol_symbolic_nnz(const CsrMatrix *A, const int *perm, char *err, size_t errlen) {
    int n = A->n;
    int *iperm = NULL;
    if (perm) {
        iperm = malloc((size_t)(n ? n : 1) * sizeof(int));
        if (!iperm) return -1;
        for (int i = 0; i < n; i++) iperm[perm[i]] = i;
    }
    int64_t *Cp = NULL, *cc = NULL;
    int *Ci = NULL, *parent = NULL;
    int64_t nnz = -1;
    if (symbolic_counts(A, iperm, &Cp, &Ci, NULL, &parent, &cc)) {
        nnz = 0;
        for (int k = 0; k < n; k++) nnz += cc[k];
    } else {
        snprintf(err, errlen, "out of memory in the symbolic factorisation");
    }
    free(iperm), free(Cp), free(Ci), free(parent), free(cc);
    return nnz;
}

bool chol_factor(const CsrMatrix *A, const int *perm, CholFactor *F, SolveProgressFn progress, void *ctx, SolveStats *st, int *bad_eq,
                 char *err, size_t errlen) {
    double t0 = wall();
    memset(F, 0, sizeof *F);
    int n = A->n;
    F->n = n;
    if (bad_eq) *bad_eq = -1;
    F->perm = malloc((size_t)(n ? n : 1) * sizeof(int));
    F->iperm = malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!F->perm || !F->iperm) goto oom;
    for (int i = 0; i < n; i++) F->perm[i] = perm ? perm[i] : i;
    for (int i = 0; i < n; i++) F->iperm[F->perm[i]] = i;
    int64_t *Cp = NULL, *cc = NULL;
    int *Ci = NULL, *parent = NULL;
    double *Cx = NULL;
    if (!symbolic_counts(A, F->iperm, &Cp, &Ci, &Cx, &parent, &cc)) {
        free(Cp), free(Ci), free(Cx), free(parent), free(cc);
        goto oom;
    }
    F->Lp = malloc(((size_t)n + 1) * sizeof(int64_t));
    if (!F->Lp) {
        free(Cp), free(Ci), free(Cx), free(parent), free(cc);
        goto oom;
    }
    F->Lp[0] = 0;
    for (int k = 0; k < n; k++) F->Lp[k + 1] = F->Lp[k] + cc[k];
    F->nnz = F->Lp[n];
    free(cc);
    F->Li = malloc((size_t)(F->nnz ? F->nnz : 1) * sizeof(int));
    F->Lx = malloc((size_t)(F->nnz ? F->nnz : 1) * sizeof(double));
    int64_t *c = malloc((size_t)(n ? n : 1) * sizeof(int64_t));
    double *x = calloc((size_t)(n ? n : 1), sizeof(double));
    int *s = malloc((size_t)(n ? n : 1) * sizeof(int)), *w = malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!F->Li || !F->Lx || !c || !x || !s || !w) {
        free(Cp), free(Ci), free(Cx), free(parent), free(c), free(x), free(s), free(w);
        goto oom;
    }
    for (int k = 0; k < n; k++) c[k] = F->Lp[k], w[k] = -1;
    double dmin = INFINITY, dmax = 0;
    bool ok = true;
    for (int k = 0; k < n; k++) {
        int top = ereach(k, Cp, Ci, parent, s, w, n);
        x[k] = 0;
        for (int64_t p = Cp[k]; p < Cp[k + 1]; p++)
            if (Ci[p] <= k) x[Ci[p]] = Cx[p];
        double d = x[k];
        x[k] = 0;
        for (; top < n; top++) {
            int i = s[top];
            double lki = x[i] / F->Lx[F->Lp[i]];
            x[i] = 0;
            for (int64_t p = F->Lp[i] + 1; p < c[i]; p++) x[F->Li[p]] -= F->Lx[p] * lki;
            d -= lki * lki;
            int64_t p = c[i]++;
            F->Li[p] = k;
            F->Lx[p] = lki;
        }
        /* relative pivot test: a pivot that collapses by many orders of magnitude signals a singular matrix */
        double scale = 0;
        for (int64_t p = Cp[k]; p < Cp[k + 1]; p++)
            if (Ci[p] == k) scale = fabs(Cx[p]);
        if (!(d > 1e-13 * scale) || !isfinite(d)) {
            if (bad_eq) *bad_eq = F->perm[k];
            snprintf(err, errlen, "matrix is singular or not positive definite at equation %d (pivot %.3g, diagonal %.3g): "
                                  "typically an unconstrained rigid-body motion or a disconnected region",
                     F->perm[k], d, scale);
            ok = false;
            break;
        }
        double lkk = sqrt(d);
        if (lkk < dmin) dmin = lkk;
        if (lkk > dmax) dmax = lkk;
        int64_t p = c[k]++;
        F->Li[p] = k;
        F->Lx[p] = lkk;
        if (progress && (k & 4095) == 4095 && !progress(ctx, k, (double)k / n)) {
            snprintf(err, errlen, "cancelled");
            if (st) st->cancelled = true;
            ok = false;
            break;
        }
    }
    free(Cp), free(Ci), free(Cx), free(parent), free(c), free(x), free(s), free(w);
    if (st) {
        snprintf(st->method, sizeof st->method, "sparse Cholesky (nested dissection)");
        st->factor_nnz = F->nnz;
        st->factor_mb = (double)F->nnz * 12.0 / 1048576.0;
        st->min_pivot = dmin * dmin;
        st->max_pivot = dmax * dmax;
        st->seconds = wall() - t0;
    }
    if (!ok) chol_free(F);
    return ok;
oom:
    snprintf(err, errlen, "out of memory in the sparse Cholesky factorisation (%d equations)", n);
    chol_free(F);
    return false;
}

void chol_solve(const CholFactor *F, const double *b, double *x) {
    int n = F->n;
    double *y = malloc((size_t)(n ? n : 1) * sizeof(double));
    if (!y) return;
    for (int i = 0; i < n; i++) y[i] = b[F->perm[i]];
    for (int j = 0; j < n; j++) {
        y[j] /= F->Lx[F->Lp[j]];
        for (int64_t p = F->Lp[j] + 1; p < F->Lp[j + 1]; p++) y[F->Li[p]] -= F->Lx[p] * y[j];
    }
    for (int j = n - 1; j >= 0; j--) {
        for (int64_t p = F->Lp[j] + 1; p < F->Lp[j + 1]; p++) y[j] -= F->Lx[p] * y[F->Li[p]];
        y[j] /= F->Lx[F->Lp[j]];
    }
    for (int i = 0; i < n; i++) x[F->perm[i]] = y[i];
    free(y);
}

void chol_free(CholFactor *F) {
    free(F->perm);
    free(F->iperm);
    free(F->Lp);
    free(F->Li);
    free(F->Lx);
    memset(F, 0, sizeof *F);
}
