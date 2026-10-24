/* heat.h - heat and flow together in two dimensions: air or water moving through channels, rooms, racks and cushions,
 * carrying heat into and out of solids that conduct it. The physics behind chip cold plates, heat exchangers, data
 * centre aisles, ventilated rooms, ventilated seats and microfluidic mixers.
 *
 * Flow: lattice Boltzmann, D2Q9, two-relaxation-time collision (Ginzburg's magic parameter 1/4, which puts
 * bounce-back walls where they belong whatever the viscosity) with Guo's forcing for buoyancy (Boussinesq), fans
 * (a body force in a region) and porous media (Brinkman's Darcy drag, implicit). Walls and solids by half-way
 * bounce-back; inlets by bounce-back from a moving wall at the inlet velocity; outlets at fixed pressure with the
 * non-equilibrium part extrapolated.
 *
 * Heat: a finite-volume energy equation on the same cells, rho cp dT/dt + rho cp u.grad T = div(k grad T) + q, in
 * fluid and solid alike, with each cell's own conductivity and heat capacity (harmonic means at faces: conjugate heat
 * transfer with continuous temperature and flux), advection second order with the van Leer limiter, explicit with
 * sub-steps where conduction needs them. A second passive scalar (a species, or the age of the air) rides on the same
 * machinery with its own diffusivity; it does not enter solids.
 *
 * Turbulence, for rooms and halls: an optional Smagorinsky eddy viscosity, and an eddy diffusivity for heat and the scalar
 * through a turbulent Prandtl number.
 *
 * Units: SI throughout the interface; the lattice is internal. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../labio.h"

#define HT_MAX_MATERIALS 16
#define HT_MAX_PATCHES 32
#define HT_MAX_REGIONS 64

enum { HT_FLUID = 0, HT_SOLID = 1, HT_POROUS = 2 };
enum { HT_WALL = 0, HT_INLET = 1, HT_OUTLET = 2, HT_PERIODIC = 3 };
enum { HT_ADIABATIC = 0, HT_FIXED_T = 1, HT_FLUX = 2 };

typedef struct HtMaterial {
    char name[32];
    int kind;          /* HT_SOLID or HT_POROUS (index 0 is always the fluid) */
    double k;          /* W/(m K): the solid's, or the porous bed's effective conductivity */
    double rho_cp;     /* J/(m^3 K) */
    double permeability; /* porous: m^2 */
} HtMaterial;

/* a stretch of one side of the domain: side 0 x-, 1 x+, 2 y-, 3 y+; from/to along it, m */
typedef struct HtPatch {
    int side;
    double from, to;
    int flow;          /* HT_WALL, HT_INLET, HT_OUTLET */
    double velocity;   /* inlet: speed into the domain, m/s */
    bool parabolic;    /* inlet: a parabola over the patch with that mean, else uniform */
    int thermal;       /* walls: HT_ADIABATIC, HT_FIXED_T, HT_FLUX */
    double T;          /* fixed wall temperature or inlet temperature, K */
    double flux;       /* HT_FLUX: heat into the domain, W/m^2 */
    double c;          /* inlet value of the passive scalar */
} HtPatch;

/* a rectangle of the domain: a material, a heat source, a fan, or a source of the passive scalar */
typedef struct HtRegion {
    double lo[2], hi[2]; /* m */
    int material;        /* -1: leave the material as it is */
    double q;            /* W/m^3 */
    double force[2];     /* fan: acceleration of the fluid, m/s^2 */
    double c_source;     /* passive scalar per second (1 for the age of air) */
    bool circle;         /* lo = centre, hi[0] = radius */
} HtRegion;

/* a rotor: a hub and blades that turn about a centre at a fixed angular speed; its cells are solid, its surface moves
 * with it (the flow sees the wall velocity omega x r) and the torque the fluid exerts on it is measured */
#define HT_MAX_ROTORS 4
typedef struct HtRotor {
    double c[2];        /* centre, m */
    double omega;       /* rad/s, counter-clockwise positive */
    int material;       /* its solid material */
    double hub_r;       /* m */
    int blades;
    double r_in, r_out; /* the blades' radial extent, m */
    double thickness;   /* m */
    double angle_deg;   /* the blade angle to the tangent (90: straight radial blades); curved blades follow a logarithmic
                           spiral at that angle, swept back against the rotation */
} HtRotor;

typedef struct HtSpec {
    int nx, ny;           /* cells */
    double dx;            /* m */
    double nu, alpha;     /* the fluid's kinematic viscosity and thermal diffusivity, m^2/s */
    double rho_cp;        /* the fluid's, J/(m^3 K) */
    double rho;           /* the fluid's density, kg/m^3 (forces and torques only) */
    double beta;          /* the fluid's thermal expansion coefficient, 1/K (buoyancy, Boussinesq) */
    double g[2];          /* gravity, m/s^2 */
    double T_ref, T_init; /* K */
    double u_ref;         /* the largest speed expected, m/s: it sets the time step (lattice speed u_lb there) */
    double u_lb;          /* default 0.05 */
    double smagorinsky;   /* Smagorinsky constant for turbulent flows (0.1 to 0.17), 0 = laminar: the eddy viscosity from the
                             local non-equilibrium stress (Hou, Sterling, Chen and Doolen, "A lattice Boltzmann
                             subgrid model for high Reynolds number flows", 1996) */
    double prandtl_t;     /* turbulent Prandtl (and Schmidt) number for the eddy diffusivity, default 0.85 */
    double c_diffusivity; /* the passive scalar's, m^2/s; <= 0: no passive scalar */
    double c_init;
    int nmaterials;       /* materials[0] is the fluid (its entries ignored) */
    HtMaterial materials[HT_MAX_MATERIALS];
    int npatches;
    HtPatch patches[HT_MAX_PATCHES]; /* the rest of every side is an adiabatic no-slip wall */
    bool periodic_x;      /* x- and x+ wrap (patches on them ignored) */
    int nregions;
    HtRegion regions[HT_MAX_REGIONS];
    int nrotors;
    HtRotor rotors[HT_MAX_ROTORS];
    int threads;
} HtSpec;

typedef struct Heat Heat;

void ht_spec_defaults(HtSpec *s);
Heat *ht_create(const HtSpec *s, char *err, size_t errlen);
void ht_free(Heat *h);
bool ht_advance(Heat *h, long nsteps); /* false once unstable */
double ht_time(const Heat *h);
double ht_dt(const Heat *h);
long ht_steps(const Heat *h);
bool ht_unstable(const Heat *h);

/* fields in SI, one value per cell, row by row from the lower left */
const double *ht_temperature(const Heat *h);
const double *ht_scalar(const Heat *h); /* NULL without a passive scalar */
void ht_velocity(const Heat *h, int i, int j, double u[2]); /* m/s */
int ht_material_at(const Heat *h, int i, int j);
/* the heat that crosses a patch into the domain, W per metre of depth (conduction; advection at inlets and outlets) */
double ht_patch_heat(const Heat *h, int patch);
/* the flow through a patch into the domain, m^2/s per metre of depth */
double ht_patch_flow(const Heat *h, int patch);
/* the mean temperature and the mean passive scalar over a patch (flow-weighted at inlets and outlets) */
double ht_patch_mean_T(const Heat *h, int patch);
double ht_patch_mean_c(const Heat *h, int patch);
/* the torque of the fluid on a rotor, N m per metre of depth (counter-clockwise positive), averaged over the last step */
double ht_rotor_torque(const Heat *h, int rotor);
/* extreme temperature over the cells of a material, K */
double ht_material_max_T(const Heat *h, int material);

void ht_write_frame(Heat *h, LabWriter *w);
char *ht_header_json(const HtSpec *s, const char *title);
