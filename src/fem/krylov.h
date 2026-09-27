/* krylov.h - iterative solvers for NONSYMMETRIC sparse systems (advection)
 *
 * Conjugate gradients (sparse.h) requires a symmetric positive-definite matrix. An advection term makes the matrix
 * nonsymmetric, and CG applied to it has no convergence guarantee and can stop at a wrong answer, so these systems use
 * BiCGSTAB (van der Vorst 1992) with right preconditioning, which keeps the monitored residual equal to the true
 * residual b - A x.
 *
 * Preconditioners:
 *   ILU(0)  incomplete LU on the matrix's own pattern (row-wise IKJ elimination; exact LU for a tridiagonal matrix)
 *   Jacobi  the diagonal
 *
 * Breakdowns are handled explicitly: when the shadow residual becomes orthogonal to the residual the iteration restarts
 * from the current iterate (up to a bounded number of restarts), and a zero pivot in ILU(0) or a vanishing stabilisation
 * factor fails with a message rather than returning an unconverged vector as a solution. The true residual is
 * recomputed after every solve and decides convergence: it must meet the relative tolerance, or - when the tolerance lies
 * below what floating point can reach on a large nonsymmetric system - the backward error |b - A x| / (|A| |x| + |b|)
 * (infinity norms) must be at the rounding floor, 64 machine epsilons. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sparse.h"

typedef enum { KRYLOV_PRECOND_ILU0 = 0, KRYLOV_PRECOND_JACOBI = 1 } KrylovPrecond;

typedef struct KrylovOptions {
    KrylovPrecond precond;
    double tol;       /* stop when ||b - A x|| <= tol * ||b|| */
    int max_iter;     /* default 5000 */
    int max_restarts; /* after a breakdown, default 10 */
    ThreadPool *pool; /* matrix-vector products */
} KrylovOptions;

/* An ILU(0) factor on the pattern of A: L (unit lower) and U share the pattern of A; diag[i] is the entry of (i, i). */
typedef struct Ilu0 {
    int n;
    const CsrMatrix *A; /* pattern */
    double *lu;         /* nnz values */
    int64_t *diag;
} Ilu0;

bool ilu0_factor(const CsrMatrix *A, Ilu0 *F, char *err, size_t errlen);
void ilu0_apply(const Ilu0 *F, const double *r, double *z); /* z = (LU)^-1 r */
void ilu0_free(Ilu0 *F);

/* x holds the initial guess on entry and the solution on return. st->method names the method and preconditioner. */
bool bicgstab_solve(const CsrMatrix *A, const double *b, double *x, const KrylovOptions *opt, SolveStats *st, char *err, size_t errlen);
