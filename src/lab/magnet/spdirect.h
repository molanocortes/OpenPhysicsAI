/* spdirect.h - a sparse symmetric positive-definite direct solve, by Apple's Accelerate framework (a system framework
 * of macOS: SparseFactor with a Cholesky factorisation and METIS-style nested-dissection ordering, in double precision).
 *
 * The matrix comes as compressed rows with both triangles present (as finite-element assembly makes it); only its lower
 * triangle is read. One factorisation serves any number of right-hand sides. When Cholesky breaks down on a nearly
 * semidefinite matrix (rounding makes a pivot non-positive), a supernodal Bunch-Kaufman LDL^T is used instead. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SpDirect SpDirect;

/* n rows; row_ptr[n + 1], col[row_ptr[n]], val[row_ptr[n]] in compressed-row form. NULL with a message on failure
 * (not positive definite, out of memory). */
SpDirect *spd_factor(int n, const int *row_ptr, const int *col, const double *val, char *err, size_t errlen);
/* the same for a symmetric indefinite matrix (a pivoted LDL^T straight away) */
SpDirect *spd_factor_indefinite(int n, const int *row_ptr, const int *col, const double *val, char *err, size_t errlen);
/* solves in place: x holds the right-hand side on entry and the solution on return */
bool spd_solve(SpDirect *s, double *x);
/* bytes held by the factor (for the run record) */
double spd_factor_bytes(const SpDirect *s);
void spd_free(SpDirect *s);

#ifdef __cplusplus
}
#endif
