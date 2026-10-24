/* mfactor.c - see mfactor.h */
#include "mfactor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

struct MFactor {
    int n;
#ifdef __APPLE__
    SparseOpaqueFactorization_Double f;
    long *starts;
    int *rows;
    double *vals;
#endif
    CholFactor chol;
    bool builtin;
    double mb;
};

/* node adjacency from the matrix pattern, for the built-in nested dissection (as src/fem/solid.c builds it) */
static bool adjacency(const CsrMatrix *A, int nn, const int *eq, int64_t **adjp_out, int **adj_out) {
    int neq = A->n;
    int64_t *adjp = calloc((size_t)nn + 1, sizeof(int64_t));
    int *eq_node = malloc((size_t)(neq ? neq : 1) * sizeof(int)), *mark = malloc((size_t)(nn ? nn : 1) * sizeof(int));
    if (!adjp || !eq_node || !mark) {
        free(adjp), free(eq_node), free(mark);
        return false;
    }
    for (int nd = 0; nd < nn; nd++)
        for (int k = 0; k < 3; k++)
            if (eq[3 * nd + k] >= 0) eq_node[eq[3 * nd + k]] = nd;
    for (int i = 0; i < nn; i++) mark[i] = -1;
    for (int q = 0; q < neq; q++) {
        int nd = eq_node[q];
        if (q > 0 && eq_node[q - 1] == nd) continue;
        int64_t cnt = 0;
        for (int64_t k = A->rowptr[q]; k < A->rowptr[q + 1]; k++) {
            int o = eq_node[A->col[k]];
            if (o != nd && mark[o] != nd) mark[o] = nd, cnt++;
        }
        adjp[nd + 1] = cnt;
    }
    for (int i = 0; i < nn; i++) adjp[i + 1] += adjp[i];
    int *adj = malloc((size_t)(adjp[nn] ? adjp[nn] : 1) * sizeof(int));
    if (!adj) {
        free(adjp), free(eq_node), free(mark);
        return false;
    }
    for (int i = 0; i < nn; i++) mark[i] = -1;
    for (int q = 0; q < neq; q++) {
        int nd = eq_node[q];
        if (q > 0 && eq_node[q - 1] == nd) continue;
        int64_t w = adjp[nd];
        for (int64_t k = A->rowptr[q]; k < A->rowptr[q + 1]; k++) {
            int o = eq_node[A->col[k]];
            if (o != nd && mark[o] != nd) mark[o] = nd, adj[w++] = o;
        }
    }
    free(eq_node), free(mark);
    *adjp_out = adjp, *adj_out = adj;
    return true;
}

MFactor *mfactor_create(const CsrMatrix *A, int nnodes, const double *node_xyz, const int *node_eq, char *err, size_t errlen) {
    MFactor *F = calloc(1, sizeof *F);
    if (!F) return snprintf(err, errlen, "out of memory"), NULL;
    F->n = A->n;
#ifdef __APPLE__
    {
        long nnz = 0;
        for (int r = 0; r < A->n; r++)
            for (int64_t k = A->rowptr[r]; k < A->rowptr[r + 1]; k++) nnz += A->col[k] >= r;
        F->starts = malloc(sizeof(long) * (size_t)(A->n + 1));
        F->rows = malloc(sizeof(int) * (size_t)(nnz ? nnz : 1));
        F->vals = malloc(sizeof(double) * (size_t)(nnz ? nnz : 1));
        if (!F->starts || !F->rows || !F->vals) {
            mfactor_free(F);
            return snprintf(err, errlen, "out of memory for the factorisation"), NULL;
        }
        long q = 0;
        for (int r = 0; r < A->n; r++) {
            F->starts[r] = q;
            for (int64_t k = A->rowptr[r]; k < A->rowptr[r + 1]; k++)
                if (A->col[k] >= r) F->rows[q] = A->col[k], F->vals[q] = A->val[k], q++;
        }
        F->starts[A->n] = q;
        SparseMatrix_Double M = {
            .structure = {.rowCount = A->n, .columnCount = A->n, .columnStarts = F->starts, .rowIndices = F->rows,
                          .attributes = {.kind = SparseSymmetric, .triangle = SparseLowerTriangle}, .blockSize = 1},
            .data = F->vals};
        SparseSymbolicFactorOptions sym = {.control = SparseDefaultControl, .orderMethod = SparseOrderMetis, .malloc = malloc, .free = free,
                                           .reportError = NULL};
        SparseNumericFactorOptions num = {.control = SparseDefaultControl, .scalingMethod = SparseScalingDefault, .pivotTolerance = 0,
                                          .zeroTolerance = 0};
        F->f = SparseFactor(SparseFactorizationCholesky, M, sym, num);
        if (F->f.status != SparseStatusOK) {
            snprintf(err, errlen, "the stiffness is not positive definite (Cholesky status %d): a part is not held against rigid motion",
                     (int)F->f.status);
            SparseCleanup(F->f);
            F->f.status = SparseStatusReleased;
            mfactor_free(F);
            return NULL;
        }
        F->mb = (double)F->f.symbolicFactorization.factorSize_Double / 1e6;
        return F;
    }
#else
    {
        int64_t *adjp = NULL;
        int *adj = NULL, *perm = malloc((size_t)(A->n ? A->n : 1) * sizeof(int));
        if (!perm || !adjacency(A, nnodes, node_eq, &adjp, &adj)) {
            free(perm), free(adjp), free(adj), free(F);
            return snprintf(err, errlen, "out of memory for the ordering"), NULL;
        }
        bool ok = nested_dissection_order(nnodes, node_xyz, adjp, adj, node_eq, A->n, perm, err, errlen);
        SolveStats st;
        int bad = -1;
        ok = ok && chol_factor(A, perm, &F->chol, NULL, NULL, &st, &bad, err, errlen);
        free(perm), free(adjp), free(adj);
        if (!ok) {
            free(F);
            return NULL;
        }
        F->builtin = true;
        F->mb = st.factor_mb;
        return F;
    }
#endif
    (void)nnodes, (void)node_xyz, (void)node_eq, (void)adjacency;
}

bool mfactor_solve(MFactor *F, double *x, int k) {
    if (!F || k < 1) return false;
#ifdef __APPLE__
    if (!F->builtin) {
        if (k == 1) {
            DenseVector_Double b = {.count = F->n, .data = x};
            SparseSolve(F->f, b);
        } else {
            DenseMatrix_Double B = {.rowCount = F->n, .columnCount = k, .columnStride = F->n, .attributes = {0}, .data = x};
            SparseSolve(F->f, B);
        }
        return true;
    }
#endif
    double *tmp = malloc((size_t)F->n * sizeof(double));
    if (!tmp) return false;
    for (int c = 0; c < k; c++) {
        chol_solve(&F->chol, x + (size_t)c * F->n, tmp);
        memcpy(x + (size_t)c * F->n, tmp, (size_t)F->n * sizeof(double));
    }
    free(tmp);
    return true;
}

double mfactor_megabytes(const MFactor *F) { return F ? F->mb : 0; }

const char *mfactor_method(const MFactor *F) {
    return F && !F->builtin ? "sparse Cholesky (Accelerate, supernodal, METIS ordering)" : "sparse Cholesky (built-in, nested dissection)";
}

void mfactor_free(MFactor *F) {
    if (!F) return;
#ifdef __APPLE__
    if (!F->builtin && F->f.status == SparseStatusOK) SparseCleanup(F->f);
    free(F->starts), free(F->rows), free(F->vals);
#endif
    if (F->builtin) chol_free(&F->chol);
    free(F);
}
