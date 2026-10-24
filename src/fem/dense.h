/* dense.h - small dense linear algebra for element and material computations (double precision) */
#pragma once

#include <stdbool.h>

/* A x = b for a small general matrix (n <= 64) by LU with partial pivoting; A and b are overwritten. */
bool dense_solve(double *A, double *b, int n);
/* inverse of a small symmetric positive definite matrix via Cholesky (in place); false if not SPD */
bool dense_spd_inverse(double *A, int n);
/* all eigenvalues/vectors of a symmetric matrix by cyclic Jacobi rotations (n <= 64).
 * A is destroyed; w receives eigenvalues in ascending order, V (optional, n*n) the eigenvectors as columns. */
bool dense_sym_eigen(double *A, int n, double *w, double *V);
/* eigenvalues of a symmetric 3x3 tensor in Voigt order [xx yy zz xy yz zx], descending (s1 >= s2 >= s3) */
void sym3_eigenvalues(const double v[6], double s[3]);
double von_mises(const double s[6]);
double det3(const double m[9]);
bool inv3(const double m[9], double out[9], double *det);
