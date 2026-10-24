/* lbm3d.h - three-dimensional incompressible flow by the lattice Boltzmann method, built to carry bodies in a stream:
 * wings, spheres, cylinders, vehicles.
 *
 * Lattice D3Q19 on a box of nx x ny x nz cells, lattice units inside (cell 1, step 1, speed of sound 1 / sqrt 3); the
 * scenario layer converts to SI. Double precision, structure-of-arrays storage, one fused pull-stream-and-collide pass
 * per step, threaded over z-slabs (src/threads.h).
 *
 * Collision: two relaxation times (Ginzburg's TRT): the symmetric part of each population pair relaxes at the viscous
 * rate, the antisymmetric part at the rate that keeps Lambda = 3/16, for which bounce-back walls sit exactly halfway
 * whatever the viscosity; a Smagorinsky eddy viscosity from the local non-equilibrium stress (Hou et al. 1996) when
 * cs > 0, for large-eddy simulation. A uniform body acceleration by Guo's forcing, split between the two parts.
 * (A regularised BGK was tried first: it lost a share of the force each step, and BGK's walls move with the viscosity;
 * lbm3dtest L1 found both.)
 *
 * Boundaries: the x faces are an inlet (equilibrium at the given velocity, density from the next plane) and an outlet
 * (the populations that would come from beyond it are its own from the step before; an optional sponge before it
 * raises the viscosity to absorb waves), or periodic; the y and z faces are periodic or free-slip (mirror).
 * Bodies are given by a signed distance (negative inside); cells whose centre is inside are solid, and every link from
 * a fluid cell into a solid one carries the fraction q of its length at which it meets the surface, found by bisection.
 * The link then reflects by Bouzidi, Firdaouss and Lallemand's interpolated bounce-back (2001), second order for curved
 * walls; with q unknown it would be the halfway bounce-back. The force on the bodies is the momentum exchanged across
 * those links (Ladd 1994), summed every step. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "../../threads.h"

enum { LBM3D_PERIODIC = 0, LBM3D_SLIP = 1, LBM3D_INOUT = 2 };
enum { LBM3D_TRT = 0, LBM3D_REGULARIZED = 1 };

typedef struct Lbm3DSpec {
    int nx, ny, nz;
    double nu;        /* kinematic viscosity, lattice units */
    double u_in[3];   /* inlet velocity (and the initial one), lattice units */
    double accel[3];  /* body acceleration, lattice units */
    double smagorinsky; /* Smagorinsky constant; 0: none */
    int bc_x;         /* LBM3D_INOUT or LBM3D_PERIODIC */
    int bc_y, bc_z;   /* LBM3D_PERIODIC or LBM3D_SLIP */
    bool floor;       /* the z-low face is a no-slip wall (halfway bounce-back): the ground under a standing body */
    int sponge;       /* cells after the inlet and before the outlet over which the viscosity rises to sponge_nu,
                         absorbing waves (and, at the inlet, the weak instability of an equilibrium inlet at a viscosity
                         near zero); 0: none */
    double sponge_nu;
    bool geometry_only; /* no populations on the CPU: the geometry for the GPU engine (lbm3d_metal.h) */
    double noise;     /* the initial field's random 3D disturbance, a fraction of the inlet speed per component (a
                         seeded hash of the cell, the same on CPU and GPU): real streams are never perfectly smooth, and a
                         perfectly symmetric start stays symmetric far longer than any real flow */
    double inlet_noise; /* the inlet's random disturbance, a fraction of its speed, new every step (free-stream turbulence) */
    int outlet;       /* 0: what would enter from beyond the outlet is its own from the step before (waves leave; no backflow);
                         1: a pressure outlet, the equilibrium at density 1 and the outlet cell's own velocity of the step
                         before, which takes flow coming back in (a pulsing flow, vortices leaving) */
    int collision;    /* LBM3D_TRT (default: exact walls, for resolved flows) or LBM3D_REGULARIZED (the non-equilibrium part
                         projected on its second moment, Latt and Chopard 2006: damps what a coarse grid cannot resolve,
                         for large-eddy simulation at high Reynolds number) */
} Lbm3DSpec;

typedef struct Lbm3D Lbm3D;

/* signed distance to the bodies, in cells, from a point in cell coordinates (a cell's centre is at integer + 0.5) */
typedef double (*Lbm3DSdf)(const double p[3], void *ctx);

Lbm3D *lbm3d_create(const Lbm3DSpec *s, char *err, size_t errlen);
void lbm3d_free(Lbm3D *L);
/* marks the bodies and their links; call before the first step, or again to move them (the flow is kept) */
bool lbm3d_set_bodies(Lbm3D *L, Lbm3DSdf sdf, void *ctx);
/* every fluid cell at equilibrium with density 1 and the inlet velocity */
void lbm3d_init(Lbm3D *L);
/* one step; false if the flow went unstable (a density not positive or a speed above 0.4) */
bool lbm3d_step(Lbm3D *L, ThreadPool *pool);
/* the macroscopic fields of the current state: rho[n] and u[3n] (either may be NULL); solid cells give rho 1, u 0 */
void lbm3d_macro(const Lbm3D *L, double *rho, double *u);
/* the force on the bodies in the last step, lattice units (x, y, z) */
void lbm3d_force(const Lbm3D *L, double F[3]);
const unsigned char *lbm3d_solid(const Lbm3D *L);
long lbm3d_links(const Lbm3D *L);
long lbm3d_steps(const Lbm3D *L);
const Lbm3DSpec *lbm3d_spec(const Lbm3D *L);
double lbm3d_noise(unsigned i, unsigned c); /* the seeded hash in [-1, 1] both engines use */
void lbm3d_set_inlet(Lbm3D *L, const double u[3]);
void lbm3d_link_arrays(const Lbm3D *L, const int **start, const unsigned char **count, const unsigned char **dir, const double **q);
