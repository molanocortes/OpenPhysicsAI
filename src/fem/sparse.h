/* sparse.h - sparse symmetric matrices for FEM
 *
 * CsrMatrix stores the full symmetric pattern (both triangles) in compressed sparse row form with sorted columns.
 * Solvers:
 *   - preconditioned conjugate gradients (Jacobi or nodal block-Jacobi), threaded matrix-vector products
 *   - sparse Cholesky (up-looking, elimination tree) with a nested-dissection ordering built from node
 *     coordinates; exact to round-off, used for small and medium systems and as the reference solver
 * All routines use double precision and report convergence diagnostics. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../threads.h"

typedef struct CsrMatrix {
    int n;
    int64_t nnz;
    int64_t *rowptr; /* n + 1 */
    int *col;        /* nnz, sorted within each row */
    double *val;     /* nnz */
} CsrMatrix;

/* Symbolic pattern from element connectivity: elem_dofs holds nelem*npe equation numbers, -1 for entries that are
 * not unknowns (constrained or inactive). */
bool csr_from_elements(CsrMatrix *A, int n, int nelem, int npe, const int *elem_dofs, char *err, size_t errlen);
void csr_free(CsrMatrix *A);
void csr_zero(CsrMatrix *A);
int64_t csr_find(const CsrMatrix *A, int row, int col); /* entry index or -1 */
/* adds a dense npe x npe element matrix (row-major); dofs < 0 are skipped. Not thread-safe for elements that share
 * equations: assemble colours in parallel instead. */
void csr_add_element(CsrMatrix *A, int npe, const int *dofs, const double *Ke);
void csr_spmv(const CsrMatrix *A, const double *x, double *y, ThreadPool *pool); /* y = A x */
double csr_memory_mb(const CsrMatrix *A);

typedef bool (*SolveProgressFn)(void *ctx, int iteration, double relative_residual); /* return false to cancel */

typedef enum { PRECOND_JACOBI = 0, PRECOND_BLOCK_JACOBI = 1 } PrecondType;

typedef struct PcgOptions {
    PrecondType precond;
    const int *block_of; /* block-Jacobi: block id per equation (blocks of 1-3 consecutive equations), NULL = Jacobi */
    double tol;          /* stop when ||b - A x|| <= tol * ||b|| */
    int max_iter;
    ThreadPool *pool;
    SolveProgressFn progress;
    void *ctx;
} PcgOptions;

typedef struct SolveStats {
    char method[48];
    int iterations;
    double rel_residual;   /* reported by the iteration */
    double true_residual;  /* ||b - A x|| / ||b|| recomputed after the solve */
    bool converged;
    bool cancelled;
    double seconds;
    int64_t factor_nnz;    /* direct solver: nonzeros in L */
    double factor_mb;
    double min_pivot, max_pivot; /* direct solver: range of the diagonal of L (squared = pivots) */
} SolveStats;

bool pcg_solve(const CsrMatrix *A, const double *b, double *x, const PcgOptions *opt, SolveStats *st, char *err, size_t errlen);

typedef struct CholFactor {
    int n;
    int *perm;  /* perm[new] = old */
    int *iperm; /* iperm[old] = new */
    int64_t *Lp;
    int *Li;
    double *Lx;
    int64_t nnz;
} CholFactor;

/* Equation ordering by nested dissection over nodes: node_xyz (3*nnodes), node adjacency in CSR (adjp, adj), and
 * the equation numbers of each node (node_eq: 3*nnodes, -1 for no equation). perm[new] = old equation. */
bool nested_dissection_order(int nnodes, const double *node_xyz, const int64_t *adjp, const int *adj, const int *node_eq,
                             int neq, int *perm, char *err, size_t errlen);
/* Symbolic analysis only: nonzeros of L for the ordering (to decide whether a direct solve fits in memory). */
int64_t chol_symbolic_nnz(const CsrMatrix *A, const int *perm, char *err, size_t errlen);
/* Factorises P A P^T = L L^T. perm may be NULL (identity). Fails with the offending equation when A is not positive
 * definite (e.g. an unconstrained rigid-body mode). */
bool chol_factor(const CsrMatrix *A, const int *perm, CholFactor *F, SolveProgressFn progress, void *ctx, SolveStats *st,
                 int *bad_equation, char *err, size_t errlen);
void chol_solve(const CholFactor *F, const double *b, double *x);
void chol_free(CholFactor *F);
