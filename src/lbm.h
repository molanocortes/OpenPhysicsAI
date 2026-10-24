/* lbm.h - D3Q19 lattice Boltzmann solver for the incompressible Navier-Stokes equations
 *
 * CPU, multithreaded, SIMD-vectorised fused stream-collide. Populations are stored shifted
 * (f - w) in float32 for precision. Collision: BGK, regularized, or regularized with third-order recursive
 * terms (default), with a separate bulk viscosity, an optional finite-difference hybrid, and Smagorinsky-Lilly LES
 * on the deviatoric stress. Obstacles: halfway bounce-back with momentum-exchange forces.
 * Near-wall roughness: discrete-element roughness drag in the first fluid layer.
 *
 * Lattice axes: +X streamwise (inlet at x=0, outlet at x=nx), +Y up, +Z spanwise.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "threads.h"

#define LBM_Q 19
#define LBM_TURB_MODES 8

/* LBM_RECURSIVE: regularized with third-order recursive Hermite terms (equilibrium and non-equilibrium) */
typedef enum { LBM_BGK = 0, LBM_REGULARIZED = 1, LBM_RECURSIVE = 2 } LbmCollision;

typedef enum {
    LBM_WALL_SLIP = 0,   /* free-slip (specular) - tunnel wall without boundary layer */
    LBM_WALL_FREESTREAM, /* far-field: equilibrium at inlet velocity */
    LBM_WALL_NOSLIP,     /* stationary no-slip wall */
    LBM_WALL_MOVING,     /* no-slip wall moving at inlet speed (rolling road) */
    LBM_WALL_PERIODIC,   /* periodic with the opposite side */
    LBM_WALL_COUNT
} LbmWall;

enum { LBM_SIDE_FLOOR = 0, LBM_SIDE_CEILING = 1, LBM_SIDE_LEFT = 2, LBM_SIDE_RIGHT = 3 };

typedef struct LbmConfig {
    float tau0;          /* molecular relaxation time; nu_lb = (tau0 - 0.5) / 3 */
    float u_in;          /* inlet velocity along +X in lattice units (already ramped) */
    float v_in;          /* inlet cross-flow velocity along +Y (start-up perturbation), usually 0 */
    LbmCollision collision;
    float smagorinsky;   /* Smagorinsky constant Cs; 0 disables LES */
    LbmWall wall[4];     /* floor (y-), ceiling (y+), left (z-), right (z+) */
    float rough_k;       /* 0.5 * Cd_rough * ks/dx ; 0 = hydraulically smooth */
    float turbulence;    /* inlet turbulence intensity (fraction of u_in) */
    int sponge_in;       /* cells of absorbing (high-viscosity) layer after the inlet, 0 = none */
    int sponge_out;      /* cells of absorbing layer before the outlet, 0 = none */
    float sponge_tau;    /* relaxation time reached at the domain ends */
    float sponge_sigma;  /* absorbing strength: equilibrium density (both ends) and velocity (outlet) relaxed
                            toward the free stream, 0 = off, typical 0.2 */
    float hrr_sigma;     /* hybrid recursive regularisation: weight of the population stress against the
                            finite-difference strain (0.98..0.999); 0 or >= 1 = plain regularisation */
    float tau_bulk;      /* relaxation time of the stress trace (bulk viscosity, acoustic damping);
                            <= tau = same as the shear rate */
    const float *in_profile; /* ny factors multiplying u_in row by row (a channel's parabola); NULL = uniform */
} LbmConfig;

typedef struct LbmOutput {
    float *rho, *ux, *uy, *uz; /* nx*ny*nz arrays, index i + nx*(j + ny*k) (all required) */
} LbmOutput;

typedef struct Lbm {
    int nx, ny, nz;
    int sx, sy, sz;     /* padded dimensions (n + 2) */
    size_t ncells;      /* interior cells */
    size_t npad;        /* padded cells */
    float *f[LBM_Q];    /* current post-collision populations (shifted) */
    float *g[LBM_Q];    /* scratch */
    uint8_t *flags;     /* padded, 1 = solid */

    uint32_t *links;    /* bounce-back links: (padded solid index << 5) | q, fluid at solid + c_q */
    size_t nlinks;
    float *link_delta;  /* per link: wall position from the fluid node as a fraction of the link (interpolated
                           bounce-back); NULL = halfway bounce-back (staircase walls) */
    uint32_t *wall_idx; /* padded index of fluid cells adjacent to solids */
    float *wall_n;      /* 3 per wall cell: unit normal pointing into the fluid */
    float *wall_a;      /* wall area weight (1 for a flat wall) */
    size_t nwall;
    uint32_t *bsolid;   /* interior index of solid cells with fluid axis neighbours */
    size_t nbsolid;
    uint64_t solid_cells;

    float turb_k[LBM_TURB_MODES][2];   /* wavenumbers (y, z) */
    float turb_w[LBM_TURB_MODES];      /* spatial wavelength along x for frequency */
    float turb_phase[LBM_TURB_MODES][2];
    float turb_amp[LBM_TURB_MODES][3];
    float *turb_y, *turb_z;            /* per-step mode tables */
    float *tau_x;                      /* per-column molecular relaxation time (sponge layers), sx entries */
    float *sig_r, *sig_u;              /* per-column sponge strength for density / velocity */
    float *u[3], *un[3];               /* padded velocity of the last step and scratch (hybrid collision only) */
    bool have_u;

    ThreadPool *pool;
    uint64_t step;
    double force[3];    /* force on all solids during the last step, lattice units */
    double umax;        /* statistics from the last output step */
    double rho_min, rho_max;
    bool unstable;
    bool periodic_z;    /* the span wraps (set before lbm_set_link_deltas): wall stencils read across it */
} Lbm;

size_t lbm_bytes_estimate(int nx, int ny, int nz);
bool lbm_create(Lbm *L, int nx, int ny, int nz, ThreadPool *pool);
void lbm_destroy(Lbm *L);

/* Equilibrium everywhere: rho = 1, u = (ux, 0, 0) in fluid cells, rest in solids. */
void lbm_reset(Lbm *L, float ux);
/* the same with the velocity ux * profile[j - 1] on row j (ny factors), e.g. a channel's Poiseuille profile */
void lbm_reset_profile(Lbm *L, float ux, const float *profile);

/* solid: nx*ny*nz bytes (index i + nx*(j + ny*k)), non-zero = solid. Cells that change state are
 * re-initialised at rest; boundary link lists are rebuilt. */
void lbm_set_solids(Lbm *L, const uint8_t *solid);

/* Boundary link n as a lattice-space segment from its fluid node to its solid node (cell centres at i + 0.5):
 * seg[0..2] fluid centre, seg[3..5] solid centre minus fluid centre. */
void lbm_link_segment(const Lbm *L, size_t n, float seg[6]);

/* Curved walls (Bouzidi, Firdaouss & Lallemand 2001, linear): delta[n] is the wall's distance from the fluid node of
 * link n as a fraction of the link, clamped to [0.001, 1]. Takes ownership of delta (malloc'ed, nlinks entries);
 * NULL restores halfway bounce-back. Links whose interpolation stencil leaves the fluid fall back to halfway.
 * lbm_set_solids discards the distances. */
void lbm_set_link_deltas(Lbm *L, float *delta);

/* Advance one time step. If out != NULL, macroscopic fields of the new state are written. */
void lbm_step(Lbm *L, const LbmConfig *cfg, const LbmOutput *out);

/* Sample macroscopic values at an interior cell from the current state. */
void lbm_cell_moments(const Lbm *L, int i, int j, int k, float *rho, float *ux, float *uy, float *uz);
