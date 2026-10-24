/* mfactor.h - one sparse symmetric positive-definite factorisation, solved for many right-hand sides.
 *
 * On macOS it is Accelerate's supernodal Cholesky (a system framework: SparseFactor with a METIS ordering, solves with a
 * block of right-hand sides at once, which is what a condensed compliance needs); elsewhere it is the repository's own
 * sparse Cholesky (src/fem/sparse.h, nested-dissection ordering), one right-hand side at a time. Both are exact to
 * round-off, so a result does not depend on which one ran, only how long it took. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../../fem/sparse.h"

typedef struct MFactor MFactor;

/* Factorises A (both triangles present, as finite-element assembly makes it). node_xyz and node_eq are for the
 * built-in ordering (3 coordinates per node, the equation of each node's three components or -1); Accelerate orders
 * by itself. NULL with a message when A is not positive definite (an unheld rigid-body motion) or memory runs out. */
MFactor *mfactor_create(const CsrMatrix *A, int nnodes, const double *node_xyz, const int *node_eq, char *err, size_t errlen);
/* Solves A X = B in place for k right-hand sides stored column after column (n each). */
bool mfactor_solve(MFactor *f, double *x, int k);
double mfactor_megabytes(const MFactor *f);
const char *mfactor_method(const MFactor *f);
void mfactor_free(MFactor *f);
