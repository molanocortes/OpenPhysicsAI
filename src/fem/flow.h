/* flow.h - steady laminar incompressible flow in a voxel box by the lattice Boltzmann solver, in SI units
 *
 * The momentum solver is the application's D3Q19 lattice Boltzmann kernel (src/lbm.c), used unchanged: regularized
 * collision with third-order recursive terms, halfway bounce-back at solids and no-slip walls, a velocity inlet at x = 0
 * and a pressure outlet at x = L. It is run from a uniform start (the inlet velocity ramped smoothly over the first
 * acoustic crossings) until the flow is steady.
 *
 * Units. With cell size dx, kinematic viscosity nu and inlet velocity U, the cell Reynolds number Re_c = U dx / nu fixes
 * the ratio of lattice velocity to lattice viscosity: u_lb = Re_c nu_lb, nu_lb = (tau - 1/2) / 3. The relaxation time
 * starts at 0.8 and is lowered when the lattice velocity would exceed 0.06 (Mach 0.10) or when the viscous pressure drop
 * of a confined flow, estimated as 36 nu_lb^2 Re_c L / D^2 in density for a domain L cells long and D cells across its
 * narrowest walled direction, would exceed 1 %. A relaxation time below 0.52 is refused rather than run near the
 * stability limit, and a run whose measured density varies by more than 3 % is refused afterwards. The physical time
 * step is dt = u_lb dx / U.
 *
 * What is returned. The lattice is weakly compressible: density varies with pressure (by up to a few percent along a
 * channel), and in a steady state it is the MOMENTUM density rho u that is divergence-free, not u. The field returned
 * for advection is therefore rho u / rho_ref, in m/s, and the density range is reported so that the compressibility of
 * the run is visible. flow_nodal_velocity maps the cell field to the (nx+1)(ny+1)(nz+1) cell corners used by a hex8
 * mesh of the same cells and scales each cross-section so that its finite-element volume flux equals U times the open
 * inlet area.
 *
 * The lattice kernel keeps process-wide scratch state, so flow_solve runs one lattice at a time per process. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../threads.h"

typedef enum { FLOW_WALL_NOSLIP = 0, FLOW_WALL_SLIP = 1, FLOW_WALL_PERIODIC = 2 } FlowWall;

typedef struct FlowSpec {
    int nx, ny, nz;             /* cells along x (streamwise), y, z; each >= 4 */
    double dx;                  /* m */
    const unsigned char *solid; /* nx*ny*nz, index i + nx*(j + ny*k), non-zero = solid; NULL: none */
    double inlet_velocity;      /* m/s along +x, uniform over the open part of the inlet plane */
    double viscosity;           /* kinematic, m^2/s */
    FlowWall wall[4];           /* the sides y-, y+, z-, z+ */
    double steady_tolerance;    /* largest change of the momentum field over 200 steps, relative to the inlet velocity;
                                   default 2e-6 */
    long max_steps;             /* default 400000 */
    ThreadPool *pool;
} FlowSpec;

typedef struct FlowField {
    int nx, ny, nz;
    double dx;
    double *u;   /* 3 per cell: momentum density over the reference density, m/s (0 in solids) */
    double *rho; /* per cell: lattice density, 1 = reference (0 in solids) */
    double tau, u_lattice, mach, reynolds_cell, dt; /* dt: physical time of one lattice step, s */
    long steps;
    double physical_time; /* s simulated until steady */
    double change;        /* the last relative change */
    bool converged;
    double density_min, density_max;
    double inlet_flux, outlet_flux; /* m^3/s through the first and last cell layers (cell sums of u_x dx^2) */
} FlowField;

/* chooses the lattice parameters and reports why a flow cannot be run; no lattice is allocated */
bool flow_parameters(const FlowSpec *spec, double *tau, double *u_lattice, double *reynolds_cell, char *err, size_t errlen);
bool flow_solve(const FlowSpec *spec, FlowField *out, char *err, size_t errlen);
void flow_field_free(FlowField *f);

/* Nodal velocity on the corner grid, index i + (nx+1)*(j + (ny+1)*k), 3 per node:
 *   - zero at every node that touches a solid cell or lies on a no-slip side;
 *   - on a slip side, the mean of the adjacent fluid cells with the wall-normal component removed;
 *   - on a periodic side, the mean over the cells across the period;
 *   - elsewhere the mean of the adjacent fluid cells.
 * Then the nodes of each cross-section x = i are scaled so that its finite-element volume flux equals the inlet
 * velocity times the open inlet area (mass conservation per cross-section, which the averaging alone does not give).
 * The result is not exactly divergence-free in the finite-element sense (integral N_a div u_h dV = 0 at every node):
 * the energy equation's advection term therefore conserves energy exactly in its own discrete form, and the enthalpy
 * carried through the open boundaries differs from it by integral rho cp T div u_h dV, which a conjugate study reports.
 * (A least-squares projection onto the nodal constraint was tried and rejected: with no-slip and inlet values held,
 * part of the constraint is reachable only through the outlet velocities, which it distorted.) */
typedef struct FlowMapStats {
    double scale_min, scale_max; /* per-section factors: the size of the correction */
} FlowMapStats;

bool flow_nodal_velocity(const FlowSpec *spec, const FlowField *f, double *vel, FlowMapStats *stats, char *err, size_t errlen);
