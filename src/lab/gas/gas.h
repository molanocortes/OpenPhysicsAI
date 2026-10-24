/* gas.h - compressible flow on a block-structured adaptive mesh: supersonic and hypersonic flow, shocks, blast
 * waves, nozzles. Two dimensions, planar or axisymmetric (x along the axis, y the radius).
 *
 * Equations: the Euler equations of an ideal gas, with a second gas carried by the field G = 1 / (gamma - 1), which
 * is advected in the quasi-conservative form of Johnsen and Colonius (J. Comput. Phys. 219, 2006) so that a material
 * interface between gases of different gamma (helium in air) does not produce spurious pressure oscillations.
 *
 * Discretisation (docs/lab/gas.md):
 *   finite volume, primitive variables reconstructed piecewise linearly with the monotonised-central limiter,
 *   HLLC Riemann flux (Toro, Spruce and Speares 1994) with HLLE on faces that lie along a strong shock (the
 *   carbuncle cure of Quirk 1994), SSP Runge-Kutta 2 in time, one time step for all levels.
 *   Adaptive mesh: a quadtree of 16 x 16 cell blocks, refinement ratio 2, 2:1 balance including diagonal
 *   neighbours, refinement where the Loehner indicator (normalised second difference of density and pressure,
 *   as in FLASH) exceeds 0.8 and next to bodies, coarsening below 0.2; optionally only where the jumps between
 *   neighbours are large (refine_jump), which keeps smooth regions coarse. Fluxes on a coarse face next to finer cells
 *   are replaced by the sum of the fine fluxes, so mass, momentum and energy are conserved to round-off.
 *   Axisymmetric: cell volumes r dr dx and face areas per radian, and the pressure on the side faces as a source,
 *   so a uniform state is preserved exactly.
 *   Solid bodies: cut cells. A level set (circles, polygons) gives each cut cell its fluid polygon, hence its fluid
 *   volume, the open part of its faces and its wall; fluxes pass through the open faces, the wall carries the
 *   pressure of the exact reflecting Riemann problem, and cells under half a volume are merged with a large
 *   neighbour away from the wall (Quirk 1994), across block edges too, so small cells do not limit the time step.
 *   Mass, momentum and energy are conserved exactly; covered cells hold mirrored states for the reconstruction.
 *
 * Units: SI throughout. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../labio.h"

#define GAS_NB 16 /* cells per block side */
#define GAS_MAX_BODIES 8
#define GAS_MAX_REGIONS 8

typedef enum { GAS_OUTFLOW = 0, GAS_INFLOW, GAS_WALL, GAS_AXIS } GasBoundary;
typedef enum { GAS_BODY_CIRCLE = 0, GAS_BODY_POLYGON } GasBodyKind;
typedef enum { GAS_REGION_BOX = 0, GAS_REGION_CIRCLE } GasRegionKind;

/* a primitive state: density kg/m^3, velocity m/s, pressure Pa, gamma */
typedef struct GasPrim {
    double rho, u, v, p, gamma;
} GasPrim;

typedef struct GasBody {
    int kind;
    double cx, cy, r;   /* circle */
    int npts;           /* polygon, counter-clockwise, m; an axisymmetric body is its half profile closed on the axis */
    double *pts;        /* 2 * npts, owned by the caller until gas_create copies it */
} GasBody;

typedef struct GasRegion {
    int kind;
    double x0, y0, x1, y1; /* box */
    double cx, cy, r;      /* circle */
    GasPrim state;
} GasRegion;

typedef struct GasSpec {
    bool axisymmetric;
    double x0, y0;       /* lower-left corner, m */
    double length;       /* extent in x, m */
    int nbx, nby;        /* root blocks; the extent in y is length * nby / nbx */
    int max_level;       /* 0 = no refinement */
    int body_level;      /* level forced within a band around bodies (default max_level) */
    double cfl;          /* default 0.4 */
    int regrid_every;    /* steps between regrids, default 4 */
    double refine_above, coarsen_below; /* Loehner thresholds, default 0.8 and 0.2 */
    double refine_jump;  /* 0 (default): the Loehner indicator alone, as in FLASH. > 0: refine only where a neighbour
                          * also differs by more than this fraction, coarsen where no neighbour differs by half of it
                          * (the coarser grid would see the jumps doubled). It spares smooth regions (a hypersonic
                          * shock layer) and costs accuracy at weak features (the ends of a rarefaction: G2 of
                          * tools/gastest.c goes from 1.2 to 1.4 times the uniform error with 0.06) */
    int boundary[4];     /* x low, x high, y low, y high (GasBoundary) */
    GasPrim inflow;      /* the state of INFLOW sides */
    GasPrim initial;     /* everywhere, before regions */
    int nregions;
    GasRegion regions[GAS_MAX_REGIONS];
    int nbodies;
    GasBody bodies[GAS_MAX_BODIES];
    double gas_constant; /* J/(kg K), for the temperature field (287.05 air); 0 leaves temperature out */
    int threads;         /* 0: all performance cores */
    bool first_order;    /* no reconstruction (for tests) */
} GasSpec;

typedef struct Gas Gas;

void gas_spec_defaults(GasSpec *s);
Gas *gas_create(const GasSpec *s, char *err, size_t errlen);
void gas_free(Gas *g);

/* one time step (both Runge-Kutta stages), dt from the CFL condition, limited so that time does not pass t_stop
 * (t_stop <= 0: no limit); regrids every regrid_every steps. Returns the step taken, 0 when the state failed. */
double gas_step(Gas *g, double t_stop);
double gas_time(const Gas *g);
int gas_steps(const Gas *g);
void gas_regrid(Gas *g);

typedef struct GasStats {
    int leaves, levels_used;
    long cells;
    double mass, xmom, energy; /* integrals over the fluid, per radian when axisymmetric */
    double min_rho, min_p;
} GasStats;
void gas_stats(const Gas *g, GasStats *st);

/* the state at a point, from the finest cell there (bilinear between cell centres); false inside a body or outside */
bool gas_sample(const Gas *g, double x, double y, GasPrim *out);
/* the finest cell size at a point */
double gas_cell_size(const Gas *g, double x, double y);

/* one frame: part "gas" (the leaf blocks) with the fields named in `fields` (comma separated from rho, p, u, v, mach,
 * T, gamma, vorticity, solid; NULL means "rho,p,mach,solid"), and part "body" (polygon outlines as lines) */
void gas_write_frame(Gas *g, LabWriter *w, const char *fields);

/* the header JSON of a run (domain, units of every field, the spec in words); caller frees */
char *gas_header_json(const GasSpec *s, const char *title);
