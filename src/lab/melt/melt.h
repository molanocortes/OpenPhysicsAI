/* melt.h - metal melted by a moving laser and solidifying again: 3D heat conduction with phase change (the enthalpy
 * method) on a uniform grid, and a Gaussian beam crossing the top surface.
 *
 * The state is the volumetric enthalpy H (J/m^3, zero at 0 K). Temperature and liquid fraction follow from it:
 *
 *   H <= Hs:        T = H / (rho c_s)                                   solid,  Hs = rho c_s T_s
 *   Hs < H < Hl:    T = T_s + (T_l - T_s) f, f = (H - Hs) / (Hl - Hs)   mushy,  Hl = Hs + rho (c_m (T_l - T_s) + L)
 *   H >= Hl:        T = T_l + (H - Hl) / (rho c_l)                      liquid
 *
 * with c_m the mean of c_s and c_l over the mushy range (T_l = T_s is an isothermal change: f from H alone). The
 * conductivity is k_s + f (k_l - k_s); faces take the harmonic mean of their two cells. Explicit in time (forward
 * Euler at the stable step), threaded over slabs. The beam deposits A P 2 / (pi w^2) exp(-2 r^2 / w^2) W/m^2 (w the
 * 1/e^2 radius, A the absorptivity) into the top cells; the top surface may lose heat by convection and radiation.
 * Each face of the box is adiabatic or held at a temperature; the energy crossing held faces is counted.
 * Flow in the melt (optional, `flow`): incompressible Navier-Stokes on the cell faces (staggered), constant density,
 * the solid held still by a Carman-Kozeny drag C (1 - f)^2 / (f^3 + 1e-3) u (the enthalpy-porosity method of Voller and
 * Prakash 1987, implicit), the top surface flat (no deformation) with the Marangoni stress mu du/dz = dsigma/dT dT/dx
 * along it, the other faces walls (no slip, or free slip on the y faces); a projection whose face coefficients are 1 /
 * (rho + dt A), A the drag's coefficient, so that pressure cannot push the solid; the heat carried by the flow with a
 * limited upwind flux. No evaporation, no recoil pressure (no keyhole), no powder: limits in docs/lab/melt.md. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "../../threads.h"

typedef struct MeltSpec {
    int n[3];                  /* cells in x, y, z (z up; the top face is z = origin + n h) */
    double h;                  /* cell size, m */
    double origin[3];          /* lower corner, m */
    double rho, c_s, c_l;      /* density kg/m^3, specific heats J/(kg K) of solid and liquid */
    double k_s, k_l;           /* conductivities W/(m K) */
    double T_s, T_l, L;        /* solidus, liquidus (K), latent heat J/kg */
    double T0;                 /* initial temperature, K */
    double held_T[6];          /* per face (x-, x+, y-, y+, z-, z+): < 0 adiabatic, else held at this temperature */
    double h_conv, T_amb, emissivity; /* top-surface losses: W/(m^2 K), K, 0..1 */
    /* the beam: power W, absorptivity, 1/e^2 radius m; its path: from start (x, y) at velocity (vx, vy) m/s between
     * t_on and t_off; up to 16 straight tracks run one after another */
    double power, absorptivity, radius;
    int ntracks;
    double track[16][6];       /* x0, y0, x1, y1 (m), speed (m/s), pause before it (s) */
    /* flow in the melt: on or off, the liquid's viscosity Pa s, its surface tension's change with temperature N/(m K),
     * the mushy zone's drag constant kg/(m^3 s) (0: 1e10), free slip on the y faces (a symmetry plane or a layer) */
    int flow;
    double mu, dsigma_dT, mushy_C;
    int slip_y;
    /* the solid's conductivity and specific heat against temperature (optional, G20): nprop points, prop_T rising (K),
     * prop_k W/(m K), prop_c J/(kg K), linear between points and constant beyond the ends; without them k_s and c_s
     * hold. The enthalpy is the exact integral of the piecewise-linear specific heat. The liquid keeps k_l and c_l; the
     * mushy range blends the solid's values at the solidus with the liquid's. */
    int nprop;
    double prop_T[32], prop_k[32], prop_c[32];
} MeltSpec;

typedef struct Melt Melt;

Melt *melt_create(const MeltSpec *s, char *err, size_t errlen);
void melt_free(Melt *M);
double melt_stable_dt(const Melt *M);
/* one step of dt; the beam where the path puts it at the step's middle */
void melt_step(Melt *M, double dt, ThreadPool *pool);
double melt_time(const Melt *M);
size_t melt_cells(const Melt *M);
/* per cell: temperature K, liquid fraction, and the largest liquid fraction reached so far (melted and solidified) */
void melt_fields(const Melt *M, float *T, float *f, float *fmx);
const double *melt_enthalpy(const Melt *M);
double *melt_enthalpy_rw(Melt *M);
double melt_T_of_H(const Melt *M, double H);
double melt_H_of_T(const Melt *M, double T);
/* energy: absorbed from the beam, lost from the top surface, crossing held faces (out positive), J since the start */
void melt_energy(const Melt *M, double *absorbed, double *surface_loss, double *held_out);
/* where the beam is at time t (false when it is off) */
bool melt_beam_at(const Melt *M, double t, double *x, double *y);
/* the flow: the velocity at a cell centre (m/s), the largest face speed, and the largest |div u| h / u_max of the last
 * projection */
void melt_velocity(const Melt *M, int i, int j, int k, double v[3]);
double melt_max_speed(const Melt *M);
double melt_divergence_error(const Melt *M);
/* the melt pool now: its length along the track, width, depth (m) from the cells with liquid fraction >= 1/2 */
void melt_pool_size(const Melt *M, double *length, double *width, double *depth);
/* the track the pool left between x0 and x1 (m, along x): the largest width (in y) and depth of the cross-sections of
 * cells that reached a liquid fraction of 1/2, as a cut through the solidified track measures them; a cell counts its
 * whole size, so each is within one cell of the truth. False when nothing melted there. */
bool melt_track_size(const Melt *M, double x0, double x1, double *width, double *depth);
/* the same to a fraction of a cell (GOALS.md G20): in each cross-section between x0 and x1 the largest enthalpy each
 * cell reached is interpolated linearly between cell centres to the enthalpy of liquid fraction 1/2, sideways for the
 * width and downward for the depth. With mirror_y the y- face is the track's plane of symmetry (half the block) and the
 * width is twice the boundary's distance from it. The mean over the sections that melted, and the largest; touches
 * when the melted metal reached a face of the block other than the top (and the mirror): the block was too small. */
bool melt_track_section(const Melt *M, double x0, double x1, bool mirror_y, double *width, double *depth, double *width_max, double *depth_max,
                        bool *touches);
