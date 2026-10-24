/* kn_lu.h - a sparse direct solve of a general (non-symmetric) square system, for the kernel's equations that carry a
 * convection term: Accelerate's sparse LU with threshold partial pivoting (macOS 15.5 and later), its QR factorisation
 * on older systems. The matrix comes in compressed rows as finite-element assembly makes it. Built with the MFEM
 * backends only (src/lab/lab.mk). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KnLU KnLU;

/* n rows; row_ptr[n + 1], col[row_ptr[n]], val[row_ptr[n]]. NULL with a message (singular, out of memory). */
KnLU *kn_lu_factor(int n, const int *row_ptr, const int *col, const double *val, char *err, size_t errlen);
/* solves in place: x holds the right-hand side on entry and the solution on return */
bool kn_lu_solve(KnLU *s, double *x);
double kn_lu_bytes(const KnLU *s);
const char *kn_lu_method(const KnLU *s); /* "LU" or "QR" */
void kn_lu_free(KnLU *s);

#ifdef __cplusplus
}
#endif
