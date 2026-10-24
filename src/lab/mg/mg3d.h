/* mg3d.h - Poisson's equation on a uniform 3D grid by geometric multigrid: lap(x) = f, the seven-point Laplacian on
 * cell centres (spacing h in every direction), each face of the box either a Dirichlet face (x = 0 on the face itself:
 * the ghost cell is the negative of the cell inside) or a Neumann face (no gradient: the ghost equals the cell inside).
 * V-cycles with red-black Gauss-Seidel smoothing, restriction by the average of the eight children, prolongation by
 * trilinear interpolation, the coarsest grid smoothed to convergence. Optionally div(beta grad x) = f with beta given
 * on the cell faces (a density's inverse, for a projection with variable density). Threaded over z-slabs. With every face Neumann the
 * problem is singular: the right side's mean is removed and so is the solution's. A face with beta = 0 is closed (on the
 * box's side, a Dirichlet face so closed acts as a Neumann one, which lets a side be partly open); a cell with every face
 * closed (inside a solid) drops out of the problem and out of the means. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "../../threads.h"

enum { MG_NEUMANN = 0, MG_DIRICHLET = 1 };

typedef struct Mg3D Mg3D;

/* faces in the order x-, x+, y-, y+, z-, z+; the sizes are halved while all three stay even and at least 4 */
Mg3D *mg3d_create(int nx, int ny, int nz, double h, const int bc[6], char *err, size_t errlen);
void mg3d_free(Mg3D *M);
/* variable coefficients: div(beta grad x) = f, beta on the faces: bx for the x faces ((nx + 1) ny nz, i + (nx + 1) (j + ny k)),
 * by (nx (ny + 1) nz) and bz (nx ny (nz + 1)) in the same way; the coarse grids' faces take the mean of the four fine faces
 * they cover. NULL restores beta = 1. The arrays are copied. */
bool mg3d_set_coefficients(Mg3D *M, const double *bx, const double *by, const double *bz);
/* solves in place (x holds the first guess) by conjugate gradients preconditioned with one V-cycle an iteration;
 * returns the iterations used, or -1 if the residual did not fall below tol times the right side's norm within
 * maxcycles */
int mg3d_solve(Mg3D *M, double *x, const double *f, double tol, int maxcycles, ThreadPool *pool);
int mg3d_levels(const Mg3D *M);
double mg3d_last_residual(const Mg3D *M); /* relative, after the last solve */
