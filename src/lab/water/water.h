/* water.h - water with a free surface: it sloshes in a tank, a column collapses and a wave breaks over an obstacle.
 * Weakly compressible smoothed particle hydrodynamics (WCSPH), the method of Monaghan (J. Comput. Phys. 110, 1994) as it
 * is used today for violent free-surface flow (the SPHERIC benchmarks, DualSPHysics):
 *
 *   kernel     Wendland C2 in 3D, W = 21 / (16 pi h^3) (1 - q/2)^4 (2q + 1), q = r / h < 2
 *   pressure   Tait's equation of state p = B ((rho / rho0)^7 - 1), B = rho0 c0^2 / 7, with a numerical speed of sound c0
 *              about ten times the fastest flow, so that density varies by about one per cent
 *   density    the continuity equation, d rho_i / dt = sum_j m_j (v_i - v_j) . grad W_ij, with the delta-SPH diffusion
 *              term of Molteni and Colagrossi (Comput. Phys. Commun. 180, 2009) that keeps the pressure field smooth,
 *              less the hydrostatic part of the density difference (Fourtakas et al., Comput. Fluids 190, 2019) so
 *              that it does not act against gravity
 *   momentum   a_i = - sum_j m_j (p_i / rho_i^2 + p_j / rho_j^2 + Pi_ij) grad W_ij + g, with Monaghan's artificial
 *              viscosity Pi_ij (alpha about 0.02) as the only viscosity: the flows here are at Reynolds numbers of 1e5 and
 *              more, where water's own viscosity does not act on the scale of a particle
 *   walls      fixed boundary particles in three layers (Adami, Hu and Adams, J. Comput. Phys. 231, 2012): each takes the
 *              pressure the fluid next to it implies, p_w = (sum_f p_f W + g . sum_f rho_f (x_w - x_f) W) / sum_f W, so
 *              that a tank, a floor or an obstacle of any shape holds water up with the right hydrostatic pressure
 *   time       symplectic Euler: the velocity from the forces, then position and density with the new velocity (a
 *              second pass over the neighbours), the step from the speed of sound and the acceleration
 *
 * Units: SI. Gravity along -z. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct WtSpec {
    double dx;          /* particle spacing, m */
    double h_factor;    /* smoothing length over spacing, default 1.7 (at 1.3 the Wendland kernel's gradient errors
                           stir water at rest to 5 % of sqrt(g H): measured on 2026-09-26, docs/lab/water.md) */
    double rho0;        /* kg/m^3 */
    double c0;          /* numerical speed of sound, m/s; 0: 10 sqrt(g H) with H the deepest water at the start */
    double alpha;       /* artificial viscosity, default 0.02 */
    double delta;       /* density diffusion, default 0.1 */
    double g;           /* m/s^2, along -z */
    double cfl;         /* of the acoustic step, default 0.25 */
    double lo[3], hi[3]; /* the region particles live in; outside it they stop and are marked out */
    bool periodic_y;    /* the y direction wraps from lo to hi (a thin slab stands for a flow the same along y) */
    int threads;
} WtSpec;

typedef struct Wt Wt;

void wt_spec_defaults(WtSpec *s);
Wt *wt_create(const WtSpec *s, char *err, size_t errlen);
void wt_free(Wt *w);

/* water: lattice points (i + 1/2) dx inside the box and below the surface z = surface(x, y) (the box's top if NULL),
 * at rest with the hydrostatic pressure of that surface; returns the particles added */
typedef double (*WtSurfaceFn)(double x, double y, void *ctx);
int wt_add_water(Wt *w, const double lo[3], const double hi[3], WtSurfaceFn surface, void *ctx);
/* walls: boundary particles on the same lattice inside the box, only those within `layers` spacings of its faces
 * (a skin at least as thick as the kernel's support 2h: four layers at the default h); `open` is a bit mask of faces with no skin (1 -x, 2 +x, 4 -y, 8 +y, 16 -z, 32 +z) */
int wt_add_solid_box(Wt *w, const double lo[3], const double hi[3], int layers, int open);
/* an open-topped tank whose inside is the box: floor and four sides of `layers` particles outside it (with periodic y,
 * no sides in y) */
int wt_add_tank(Wt *w, const double lo[3], const double hi[3], int layers);
/* solids of any shape, given by a signed distance (negative inside): boundary particles on the lattice within the box
 * whose distance lies in (-layers dx, 0]; and water that stays clear of them (distance above half a spacing) */
typedef double (*WtSolidFn)(const double p[3], void *ctx);
int wt_add_solid_fn(Wt *w, const double lo[3], const double hi[3], WtSolidFn sdf, void *ctx, int layers);
int wt_add_water_fn(Wt *w, const double lo[3], const double hi[3], WtSurfaceFn surface, void *sctx, WtSolidFn solid, void *ctx);

double wt_step(Wt *w); /* one step; returns the step taken */
double wt_time(const Wt *w);
long wt_steps(const Wt *w);
int wt_count(const Wt *w);       /* water particles */
int wt_wall_count(const Wt *w);
double wt_c0(const Wt *w);
bool wt_unstable(const Wt *w);

/* the water particles' state (arrays of wt_count entries; positions and velocities 3 per particle) */
const double *wt_x(const Wt *w);
const double *wt_v(const Wt *w);
const double *wt_rho(const Wt *w);
double wt_pressure(const Wt *w, int i);
const double *wt_wall_x(const Wt *w);

/* totals: kinetic, potential (from z = 0) and elastic energy (the work of compressing the water, from the equation of
 * state), J; and the centre of mass. The elastic energy is set once the first step has fixed the speed of sound. */
void wt_energy(const Wt *w, double *kinetic, double *potential, double *elastic);
void wt_centre(const Wt *w, double c[3]);

/* for the GPU engine (water_metal.h): the solver's constants once fixed, and its state to read and write */
typedef struct WtParams {
    double h, c0, B, mass, rho0, g, alpha, delta, cfl, Ly;
    double lo[3], hi[3], cell[3];
    int grid[3];
    bool periodic_y;
} WtParams;
void wt_prepare(Wt *w);                     /* fixes the speed of sound and the starting densities (the first step does it too) */
void wt_params(const Wt *w, WtParams *p);
double *wt_x_rw(Wt *w);                     /* positions, velocities, densities of the water particles, to overwrite */
double *wt_v_rw(Wt *w);
double *wt_rho_rw(Wt *w);
unsigned char *wt_out_rw(Wt *w);            /* 1: left the region */
void wt_set_clock(Wt *w, double t, long steps);