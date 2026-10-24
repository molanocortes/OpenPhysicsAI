/* vmg.h - geometric multigrid on uniform voxel meshes for static linear elasticity, as the preconditioner of conjugate
 * gradients (SolidOptions.solver = SOLID_SOLVER_MG; docs/contracts/topology-optimisation.md, amendment 4).
 *
 * A voxel mesh is a lattice of equal cubes. Its coarse levels are the lattices of twice the cell size, down to a level
 * small enough to factorise. The prolongation P is trilinear interpolation from the coarse lattice nodes to the fine
 * ones, and each coarse operator is the Galerkin product P^T A P, built cell by cell: the matrix of a coarse cell is the
 * sum over its (up to eight) fine cells of T^T K_e T, T the interpolation from the coarse cell's corners to the fine
 * cell's, with the rows of prescribed fine components left out. For nested trilinear cells that is the Galerkin product
 * exactly, so every coarse level sees the stiffness the fine one has, including the contrast of a topology
 * optimisation's densities (Amir, Aage and Lazarov, Struct. Multidisc. Optim. 49, 2014, use the same construction).
 * Smoothing: a Chebyshev polynomial in the nodal block-Jacobi preconditioned operator on [lambda / 10, lambda], lambda
 * estimated by a power iteration, the same before and after the coarse correction, so that one V-cycle is a symmetric
 * positive definite preconditioner. The coarsest level is solved by sparse Cholesky.
 *
 * Nothing else in the solver changes: the fine matrix and right-hand side are the ones solid_solve assembles, and the
 * conjugate gradients stop on the same test as pcg_solve (relative residual against the right-hand side). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sparse.h"

typedef struct VmgProblem {
    int nnodes, nelems;
    const double *xyz;        /* 3 nnodes (m) */
    const int *conn;          /* 8 nelems, in the HEX8 node order of hex8.h */
    const int *eq;            /* 3 nnodes: the equation of each component, -1 when prescribed or unused */
    const double *ke;         /* element matrices, 576 per entry of ke_index */
    const int *ke_index;      /* nelems: every element's entry (a voxel mesh has one per material) */
    const double *elem_scale; /* nelems: stiffness multiplier, NULL = 1 */
} VmgProblem;

typedef struct VmgOptions {
    double tol;               /* stop when ||b - A x|| <= tol ||b|| */
    int max_iter;
    ThreadPool *pool;
    SolveProgressFn progress; /* every 10 iterations; false cancels */
    void *ctx;
    int smooth_degree;        /* Chebyshev degree before and after the coarse correction (default 2) */
    int coarse_max;           /* coarsen until a level has at most this many equations (default 6000) */
} VmgOptions;

/* Solves K x = b, with K the assembled fine matrix over the free equations of the voxel model p, x holding the first
 * guess. Refuses (false, with the reason) a mesh that is not a lattice of equal axis-aligned cubes. st->iterations
 * counts conjugate-gradient iterations; st->method names the hierarchy. */
bool vmg_solve(const VmgProblem *p, const CsrMatrix *K, const double *b, double *x, const VmgOptions *o, SolveStats *st,
               char *err, size_t errlen);
