/* euler3d.h - compressible, inviscid flow in three dimensions around bodies: supersonic and hypersonic streams, bow
 * shocks, capsules at an angle of attack.
 *
 * Ideal gas, the Euler equations in conservative form on a Cartesian grid of nx x ny x nz cells (SI units), finite
 * volumes: MUSCL reconstruction of the primitive variables with the monotonised-central limiter, the HLLC Riemann flux
 * (Toro), switched to HLL on faces next to a strong pressure jump (a relative jump above one half to any neighbour),
 * the usual cure for HLLC's instability along the stagnation line of a bow shock (the carbuncle family; found here by
 * euler3dtest E2 and E3), two-stage strong-stability-preserving Runge-Kutta in time at a Courant number of 0.4. Double precision,
 * threaded over z-slabs.
 *
 * Faces: each of the six is an inflow (the free stream held), an outflow (zero gradient) or a symmetry plane (mirror).
 * Bodies come as a signed distance (labshape.h). Solid cells next to the fluid are ghost cells: each takes the state at
 * its image point, reflected through the surface along the distance's gradient, interpolated trilinearly from the fluid
 * cells there, with the normal velocity reversed (a slip wall, second order; Tseng and Ferziger 2003). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "../../threads.h"

enum { E3_INFLOW = 0, E3_OUTFLOW = 1, E3_SYMMETRY = 2 };

typedef struct Euler3DSpec {
    int nx, ny, nz;
    double dx, origin[3];
    double gamma;
    double rho_inf, u_inf[3], p_inf; /* the free stream */
    int face[6];                     /* x low, x high, y low, y high, z low, z high */
    double cfl;
} Euler3DSpec;

typedef struct Euler3D Euler3D;
typedef double (*Euler3DSdf)(const double p[3], void *ctx); /* metres; negative inside */

Euler3D *euler3d_create(const Euler3DSpec *s, Euler3DSdf sdf, void *ctx, char *err, size_t errlen);
void euler3d_free(Euler3D *E);
/* sets cell (i, j, k) (0-based, inside the domain) to a primitive state; for initial conditions */
void euler3d_set(Euler3D *E, int i, int j, int k, double rho, const double v[3], double p);
/* one step (two stages); returns the step taken, 0 if the state went invalid */
double euler3d_step(Euler3D *E, ThreadPool *pool);
double euler3d_time(const Euler3D *E);
/* primitive fields on the grid (NULL to skip); ghost cells give their mirrored state (so that a picture's texture is
 * continuous up to the wall), deeper solid cells the free stream's density and pressure, zero velocity */
void euler3d_fields(const Euler3D *E, double *rho, double *u, double *p);
/* 0 fluid, 1 solid, 2 ghost */
const unsigned char *euler3d_kind(const Euler3D *E);
/* the pressure at a point (trilinear over the fluid cells nearby), Pa; NAN if none */
double euler3d_probe_p(const Euler3D *E, const double x[3]);
