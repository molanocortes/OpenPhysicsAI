/* sim.h - simulation manager: physical units, parameters, geometry placement, solver thread,
 * field snapshots for visualisation and force coefficients. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "geom/mesh.h"
#include "lbm.h"
#include "math3d.h"

typedef enum {
    FLUID_WATER = 0,
    FLUID_SEAWATER,
    FLUID_AIR,
    FLUID_GLYCERIN,
    FLUID_OLIVE_OIL,
    FLUID_HONEY,
    FLUID_MERCURY,
    FLUID_CUSTOM,
    FLUID_COUNT
} FluidId;

const char *fluid_name(int id);
const char *fluid_label(int id);
int fluid_find(const char *name); /* -1 if unknown */
/* density [kg/m^3] and kinematic viscosity [m^2/s] at temperature T [deg C] */
void fluid_properties(int id, double temp_c, double *rho, double *nu);

typedef enum { REF_AXIS_X = 0, REF_AXIS_Y, REF_AXIS_Z, REF_AXIS_MAX } RefAxis;

typedef struct SimParams {
    /* fluid and flow - applied live */
    int fluid;
    double temperature; /* deg C */
    double rho;         /* kg/m^3 (only used directly for FLUID_CUSTOM) */
    double nu;          /* m^2/s  (only used directly for FLUID_CUSTOM) */
    double speed;       /* free-stream speed, m/s */
    double ref_length;  /* model reference length, m (the extent along ref_axis) */
    int ref_axis;
    double roughness;   /* equivalent sand-grain roughness ks, m */
    double turbulence;  /* inlet turbulence intensity, fraction of speed */
    /* model placement - re-voxelised live */
    double aoa, yaw, roll; /* degrees: pitch nose-up, yaw nose-left, roll right-wing-down */
    double pos[3];         /* bounding-box centre as a fraction of the domain */
    double fit;            /* model cross-section extent as a fraction of the tunnel cross-section */
    double fit_length;     /* model streamwise extent as a fraction of the tunnel length */
    double shell;          /* thin-surface voxel shell radius in cells (0 = off) */
    bool curved_walls;     /* interpolated bounce-back at the exact mesh surface; false = halfway (staircase) */
    /* numerics */
    int nx, ny, nz;        /* lattice size - rebuild */
    double u_lattice;      /* target lattice inlet velocity (Mach ~ 1.73 u) */
    int collision;         /* LbmCollision - live */
    double tau_bulk;       /* bulk relaxation time: damps acoustic noise (<= tau0 = same as shear) - live */
    double hrr_sigma;      /* finite-difference hybrid weight (0 = off, 0.98..0.999) - live */
    double smagorinsky;    /* Cs - live */
    int wall[4];           /* LbmWall per side - live */
    int threads;           /* solver threads - rebuild */
    int ramp_steps;        /* smooth inflow start-up */
    bool start_with_flow;  /* reset to uniform flow instead of fluid at rest */
} SimParams;

void sim_params_default(SimParams *p);

typedef struct SimUnits {
    double dx, dt;          /* lattice spacing [m], time step [s] */
    double U, rho, nu;      /* physical free stream */
    double re;              /* Reynolds number U L / nu */
    double u_lb, nu_lb, tau0, mach;
    double re_eff;          /* Reynolds number the lattice runs at: the solver floors tau0 at 0.50001 */
    double ref_cells;       /* reference length in lattice cells */
    double frontal_cells;   /* projected area on the YZ plane, cells^2 */
    double frontal_area;    /* m^2 */
    double blockage;        /* frontal area / tunnel cross-section */
    double rough_k;         /* lattice roughness drag coefficient */
    double ks_cells;        /* roughness height in cells */
} SimUnits;

/* Conversions from lattice to physical units. */
static inline double units_speed(const SimUnits *u) { return u->dx / u->dt; }                     /* m/s per lattice u */
static inline double units_pressure(const SimUnits *u) { return u->rho * (u->dx / u->dt) * (u->dx / u->dt); } /* Pa per lattice p */
static inline double units_rate(const SimUnits *u) { return 1.0 / u->dt; }                         /* 1/s per lattice 1/step */
static inline double units_force(const SimUnits *u) {
    return u->rho * u->dx * u->dx * u->dx * u->dx / (u->dt * u->dt);
}

typedef struct SimSnapshot {
    int nx, ny, nz;
    float *rho, *ux, *uy, *uz; /* lattice units, nx*ny*nz */
    uint8_t *solid;
    uint64_t step;
    double sim_time;           /* s */
    SimUnits units;
    uint32_t geom_version;
    uint64_t serial;
    /* internal */
    int refcount;
    uint64_t grid_id;
} SimSnapshot;

#define SIM_FORCE_HISTORY 2048

typedef struct ForceSample {
    uint64_t step;
    double time; /* s */
    float cd, cl, cs;
} ForceSample;

typedef struct SimStatus {
    bool running;
    bool diverged;
    bool busy;              /* rebuilding / voxelising */
    char busy_msg[96];
    uint64_t step;
    double sim_time;        /* s */
    double mlups;
    double steps_per_sec;
    double outputs_per_sec; /* field snapshots published per second */
    double realtime_factor; /* simulated seconds per wall second */
    SimUnits units;
    int nx, ny, nz;
    uint64_t cells, solid_cells;
    double mem_mb;
    int threads;
    double umax_lb, rho_min, rho_max;
    double ramp;            /* 0..1 inflow ramp progress */
    /* forces on the model (averaged over the recent window) */
    double fx, fy, fz;      /* N */
    double cd, cl, cs;
    double cd_inst, cl_inst;
    double strouhal;        /* from lift oscillation, 0 if not detected */
    uint32_t geom_version;
    double curved_fraction; /* boundary links placed on the exact mesh surface (0 = staircase walls) */
    mat4 model_to_lattice;
    bool has_model;
} SimStatus;

typedef struct Sim Sim;

Sim *sim_create(const SimParams *p);
void sim_destroy(Sim *s);

void sim_set_params(Sim *s, const SimParams *p); /* live values apply next step; geometry/grid changes are scheduled */
void sim_get_params(Sim *s, SimParams *p);
void sim_set_mesh(Sim *s, const Mesh *m);        /* deep copy; triggers voxelisation. NULL removes the model */
void sim_run(Sim *s, bool running);
void sim_step(Sim *s, int nsteps);               /* advance while paused */
/* Playback speed as a fraction of full solver speed (1 = as fast as possible). Below 1 the solver thread idles
 * between steps, so slow motion also frees the CPU. */
void sim_set_playback(Sim *s, double fraction);
void sim_reset(Sim *s);                          /* restart the flow (keeps geometry) */
/* Smooth cross-flow pulse at the inlet (amplitude x free-stream speed, over ~3 convective times) starting
 * delay_steps after the request is applied: breaks the symmetry of bluff-body wakes so shedding starts quickly. */
void sim_kick(Sim *s, double amplitude, int delay_steps);
void sim_status(Sim *s, SimStatus *st);
int sim_force_history(Sim *s, ForceSample *out, int max); /* oldest first, returns count */
bool sim_wait_idle(Sim *s, double timeout_s);    /* waits until no rebuild/voxelise/steps are pending */
bool sim_requests_pending(Sim *s);               /* parameter/mesh/reset requests not yet applied and reported */

/* Newest snapshot with serial > newer_than, or NULL. Must be released. */
SimSnapshot *sim_acquire_snapshot(Sim *s, uint64_t newer_than);
void sim_release_snapshot(Sim *s, SimSnapshot *snap);

/* Model transform used for voxelisation (mesh space -> lattice space) and the reference length in cells. */
mat4 sim_model_transform(const Mesh *m, const SimParams *p, double *ref_cells);
