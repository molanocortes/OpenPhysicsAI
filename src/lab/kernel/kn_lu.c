/* kn_lu.c - see kn_lu.h. Accelerate takes compressed columns: the rows MFEM assembles are turned into columns first. */
#include "kn_lu.h"

#include <Accelerate/Accelerate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct KnLU {
    int n;
    bool lu;
    SparseOpaqueFactorization_Double f;
    long *starts;
    int *rows;
    double *vals;
};

KnLU *kn_lu_factor(int n, const int *row_ptr, const int *col, const double *val, char *err, size_t errlen) {
    KnLU *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->n = n;
    long nnz = row_ptr[n];
    s->starts = calloc((size_t)n + 1, sizeof(long));
    s->rows = malloc(sizeof(int) * (size_t)(nnz ? nnz : 1));
    s->vals = malloc(sizeof(double) * (size_t)(nnz ? nnz : 1));
    long *next = malloc(sizeof(long) * (size_t)(n + 1));
    if (!s->starts || !s->rows || !s->vals || !next) {
        snprintf(err, errlen, "kn_lu: out of memory for %ld entries", nnz);
        free(next), kn_lu_free(s);
        return NULL;
    }
    for (long k = 0; k < nnz; k++) s->starts[col[k] + 1]++; /* entries per column */
    for (int j = 0; j < n; j++) s->starts[j + 1] += s->starts[j];
    memcpy(next, s->starts, sizeof(long) * (size_t)(n + 1));
    for (int r = 0; r < n; r++)
        for (int k = row_ptr[r]; k < row_ptr[r + 1]; k++) {
            long q = next[col[k]]++;
            s->rows[q] = r, s->vals[q] = val[k];
        }
    free(next);
    SparseMatrix_Double A = {
        .structure = {.rowCount = n, .columnCount = n, .columnStarts = s->starts, .rowIndices = s->rows,
                      .attributes = {.kind = SparseOrdinary}, .blockSize = 1},
        .data = s->vals};
    SparseSymbolicFactorOptions sym = {.control = SparseDefaultControl, .orderMethod = SparseOrderDefault,
                                       .malloc = malloc, .free = free, .reportError = NULL};
    SparseNumericFactorOptions num = {.control = SparseDefaultControl, .scalingMethod = SparseScalingDefault,
                                      .pivotTolerance = 0.1, .zeroTolerance = 0};
    if (__builtin_available(macOS 15.5, *)) {
        s->f = SparseFactor(SparseFactorizationLU, A, sym, num);
        s->lu = true;
    } else {
        s->f = SparseFactor(SparseFactorizationQR, A, sym, num);
    }
    if (s->f.status != SparseStatusOK) {
        snprintf(err, errlen, "kn_lu: the %s factorisation failed (status %d: singular or out of memory)", s->lu ? "LU" : "QR", (int)s->f.status);
        SparseCleanup(s->f);
        s->f.status = SparseStatusReleased;
        kn_lu_free(s);
        return NULL;
    }
    return s;
}

bool kn_lu_solve(KnLU *s, double *x) {
    if (!s || s->f.status != SparseStatusOK) return false;
    DenseVector_Double b = {.count = s->n, .data = x};
    SparseSolve(s->f, b);
    return true;
}

double kn_lu_bytes(const KnLU *s) {
    return s && s->f.status == SparseStatusOK ? (double)s->f.symbolicFactorization.factorSize_Double : 0;
}

const char *kn_lu_method(const KnLU *s) { return s && s->lu ? "LU" : "QR"; }

void kn_lu_free(KnLU *s) {
    if (!s) return;
    if (s->f.status == SparseStatusOK) SparseCleanup(s->f);
    free(s->starts), free(s->rows), free(s->vals), free(s);
}
