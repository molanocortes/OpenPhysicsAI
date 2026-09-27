/* spdirect.c - sparse Cholesky by Accelerate (spdirect.h). Built only with the MFEM backends (src/lab/lab.mk). */
#include "spdirect.h"

#include <Accelerate/Accelerate.h>
#include <stdio.h>
#include <stdlib.h>

struct SpDirect {
    int n;
    SparseOpaqueFactorization_Double f;
    long *starts;
    int *rows;
    double *vals;
};

static SpDirect *factor(int n, const int *row_ptr, const int *col, const double *val, bool indefinite, char *err, size_t errlen);
SpDirect *spd_factor(int n, const int *row_ptr, const int *col, const double *val, char *err, size_t errlen) {
    return factor(n, row_ptr, col, val, false, err, errlen);
}
SpDirect *spd_factor_indefinite(int n, const int *row_ptr, const int *col, const double *val, char *err, size_t errlen) {
    return factor(n, row_ptr, col, val, true, err, errlen);
}
static SpDirect *factor(int n, const int *row_ptr, const int *col, const double *val, bool indefinite, char *err, size_t errlen) {
    SpDirect *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->n = n;
    /* the lower triangle in compressed columns: for a symmetric matrix, column j is row j, entries at or below it */
    long nnz = 0;
    for (int r = 0; r < n; r++)
        for (int k = row_ptr[r]; k < row_ptr[r + 1]; k++) nnz += col[k] >= r;
    s->starts = malloc(sizeof(long) * (size_t)(n + 1));
    s->rows = malloc(sizeof(int) * (size_t)nnz);
    s->vals = malloc(sizeof(double) * (size_t)nnz);
    if (!s->starts || !s->rows || !s->vals) {
        snprintf(err, errlen, "spdirect: out of memory for %ld entries", nnz);
        spd_free(s);
        return NULL;
    }
    long q = 0;
    for (int r = 0; r < n; r++) {
        s->starts[r] = q;
        for (int k = row_ptr[r]; k < row_ptr[r + 1]; k++)
            if (col[k] >= r) s->rows[q] = col[k], s->vals[q] = val[k], q++;
    }
    s->starts[n] = q;
    SparseMatrix_Double A = {
        .structure = {.rowCount = n, .columnCount = n, .columnStarts = s->starts, .rowIndices = s->rows,
                      .attributes = {.kind = SparseSymmetric, .triangle = SparseLowerTriangle}, .blockSize = 1},
        .data = s->vals};
    SparseSymbolicFactorOptions sym = {.control = SparseDefaultControl, .orderMethod = SparseOrderMetis,
                                       .malloc = malloc, .free = free, .reportError = NULL};
    SparseNumericFactorOptions num = {.control = SparseDefaultControl, .scalingMethod = SparseScalingDefault,
                                      .pivotTolerance = indefinite ? 0.01 : 0, .zeroTolerance = 0};
    s->f = SparseFactor(indefinite ? SparseFactorizationLDLTSBK : SparseFactorizationCholesky, A, sym, num);
    if (s->f.status != SparseStatusOK && !indefinite) {
        /* nearly semidefinite (a curl-curl system's gradients carry only a tiny regularising mass): pivoted LDL^T */
        SparseCleanup(s->f);
        num.pivotTolerance = 0.01;
        s->f = SparseFactor(SparseFactorizationLDLTSBK, A, sym, num);
    }
    if (s->f.status != SparseStatusOK) {
        snprintf(err, errlen, "spdirect: Cholesky and LDL^T factorisations failed (status %d: singular or out of memory)", (int)s->f.status);
        SparseCleanup(s->f);
        s->f.status = SparseStatusReleased;
        spd_free(s);
        return NULL;
    }
    return s;
}

bool spd_solve(SpDirect *s, double *x) {
    if (!s || s->f.status != SparseStatusOK) return false;
    DenseVector_Double b = {.count = s->n, .data = x};
    SparseSolve(s->f, b);
    return true;
}

double spd_factor_bytes(const SpDirect *s) {
    return s && s->f.status == SparseStatusOK ? (double)s->f.symbolicFactorization.factorSize_Double : 0;
}

void spd_free(SpDirect *s) {
    if (!s) return;
    if (s->f.status == SparseStatusOK) SparseCleanup(s->f);
    free(s->starts), free(s->rows), free(s->vals), free(s);
}
