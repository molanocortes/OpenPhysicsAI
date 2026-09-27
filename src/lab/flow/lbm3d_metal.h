/* lbm3d_metal.h - the 3D lattice Boltzmann step of lbm3d.h on the GPU, by Metal (macOS's GPU interface, a system
 * framework), for speed: the same D3Q19 TRT collision with its Smagorinsky viscosity, the same boundaries and the same
 * interpolated bounce-back, one GPU thread per cell.
 *
 * Precision: populations are stored in single precision shifted by their weights (f - w, so that the stored numbers are
 * small and keep their digits: the "DDF shifting" of Lehmann et al. 2022), arithmetic in single precision. Apple's GPUs
 * have no double precision. The double-precision CPU engine is the reference, and lbm3dtest runs the same cases on
 * both. The force on the bodies is summed on the GPU into a history, one entry per step. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "lbm3d.h"

typedef struct Lbm3DGpu Lbm3DGpu;

/* the geometry (solid cells, links) comes from a CPU engine made with lbm3d_create(..., populations off) */
Lbm3DGpu *lbm3d_gpu_create(const Lbm3D *geometry, char *err, size_t errlen);
void lbm3d_gpu_free(Lbm3DGpu *G);
void lbm3d_gpu_init(Lbm3DGpu *G);
void lbm3d_gpu_set_inlet(Lbm3DGpu *G, const double u[3]);
/* nsteps steps; forces[3 * k] the force of step k of these (lattice units), may be NULL; false if unstable */
bool lbm3d_gpu_steps(Lbm3DGpu *G, int nsteps, double *forces);
void lbm3d_gpu_macro(Lbm3DGpu *G, double *rho, double *u);
long lbm3d_gpu_steps_done(const Lbm3DGpu *G);
const char *lbm3d_gpu_name(const Lbm3DGpu *G);

/* The immersed boundary: npts Lagrangian points that the flow must follow (a sheet, a membrane, a body's surface),
 * by direct forcing iterated `iterations` times with the relaxation `relax` (Uhlmann 2005; Luo et al. 2007), spread
 * and interpolated with Roma, Peskin and Berger's three-point kernel, the force entering the collision by Guo's
 * forcing. Before each lbm3d_gpu_ib_step the caller writes, per point, (x, y, z, dV) in cell coordinates (a cell's
 * centre at integer + 1/2; dV the point's share of the surface times one cell) and (ux, uy, uz, m) in lattice units,
 * m the point's mass (0 for a point moved as prescribed; with a mass the force is found so that point and fluid end
 * the step at one velocity, which keeps light structures stable);
 * after it, the force each point put on the fluid (x, y, z, 0), lattice units: the structure feels the opposite. Once
 * enabled, lbm3d_gpu_macro gives the velocity with the force's half step. */
bool lbm3d_gpu_ib_enable(Lbm3DGpu *G, int npts, int iterations, double relax, char *err, size_t errlen);
float *lbm3d_gpu_ib_points(Lbm3DGpu *G);
float *lbm3d_gpu_ib_velocities(Lbm3DGpu *G);
const float *lbm3d_gpu_ib_forces(const Lbm3DGpu *G);
bool lbm3d_gpu_ib_step(Lbm3DGpu *G, double force[3]);
