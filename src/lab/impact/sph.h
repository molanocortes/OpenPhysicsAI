/* sph.h - solids hit so fast that they break into clouds of fragments: smoothed particle hydrodynamics for solid dynamics
 * (Libersky and Petschek 1991, Libersky et al., J. Comput. Phys. 109, 1993), the method used for hypervelocity impact on
 * spacecraft shields and for cratering, where a mesh would tangle. Same material laws as the Lagrangian solver
 * (impact.h): Mie-Gruneisen pressure, Johnson-Cook strength with adiabatic heating; the two solvers can be run on the
 * same problem and compared.
 *
 * Model (docs/lab/impact.md):
 *   kernel     cubic spline (M4), smoothing length h = h_factor * spacing, support 2h
 *   density    continuity equation, d rho_i / dt = sum_j m_j (v_i - v_j) . grad W_ij
 *   momentum   the symmetric stress form, a_i = sum_j m_j (sigma_i / rho_i^2 + sigma_j / rho_j^2 - Pi_ij I
 *              + (R_i + R_j) f_ij^4) . grad_i W_ij, which conserves momentum pair by pair
 *   energy     the matching form, de_i/dt = -1/2 sum_j m_j v_ij . (the same tensor) . grad_i W_ij, so that kinetic plus
 *              internal energy is conserved by the spatial discretisation exactly
 *   shocks     Monaghan's artificial viscosity (alpha, beta) with Balsara's switch against it acting in shear
 *   tension    the artificial stress of Gray, Monaghan and Swift (Comput. Methods Appl. Mech. Eng. 190, 2001) against the
 *              tensile instability
 *   deviator   Jaumann rate from the SPH velocity gradient, radial return to the Johnson-Cook flow stress
 *   planes     symmetry planes by mirror particles; a rigid frictionless wall is a mirror that does not pull
 *   time       Heun's predictor-corrector (second order), the step from the signal speed and the acceleration
 *
 * Units: SI. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../labio.h"
#include "impact.h"

#define SPH_MAX_PLANES 6

typedef struct SphPlane {
    int axis;    /* 0 x, 1 y, 2 z */
    double pos;  /* m */
    int side;    /* the material lies at coordinates above pos (+1) or below it (-1) */
    bool wall;   /* false: a symmetry plane; true: a rigid frictionless wall that pushes but does not pull */
} SphPlane;

typedef struct SphSpec {
    int nmaterials;
    ImMaterial materials[IM_MAX_MATERIALS];
    double dx;          /* particle spacing, m */
    double h_factor;    /* smoothing length over spacing, default 1.3 */
    double safety;      /* of the stable step, default 0.3 */
    double av_alpha, av_beta; /* default 1 and 2 */
    double art_stress;  /* epsilon of the artificial stress, default 0.3; 0 turns it off */
    bool no_balsara;    /* true: the viscosity acts on every approaching pair; false (default): Balsara's switch (J. Comput.
                           Phys. 121, 1995) scales it by |div v| / (|div v| + |curl v|), so it acts in compression and shocks
                           and fades in shear, where it would only take energy from the plastic flow */
    bool hydro;         /* true: pressure only, no strength */
    bool correct_gradient; /* normalise the velocity gradient (Randles and Libersky 1996) so that it is exact for a linear
                              field even where the kernel is cut by a free surface; momentum and energy keep their
                              conservative form */
    int nplanes;
    SphPlane planes[SPH_MAX_PLANES];
    int threads;
} SphSpec;

typedef struct Sph Sph;

void sph_spec_defaults(SphSpec *s);
Sph *sph_create(const SphSpec *s, char *err, size_t errlen);
void sph_free(Sph *p);

/* bodies: particles at the centres of a cubic lattice of the spec's spacing (lattice points at (i + 1/2) dx), kept where
 * they fall inside the shape and on the material side of every plane; each call is one body, returns its index or -1 */
int sph_add_box(Sph *p, const double lo[3], const double hi[3], int material, const double v[3]);
int sph_add_sphere(Sph *p, const double c[3], double r, int material, const double v[3]);
int sph_add_cylinder(Sph *p, const double base[3], double r, double length, int material, const double v[3]); /* along +z */

double sph_step(Sph *p); /* one step; returns the step taken */
double sph_time(const Sph *p);
long sph_steps(const Sph *p);
int sph_count(const Sph *p);
int sph_bodies(const Sph *p);
bool sph_unstable(const Sph *p);

/* totals over the real particles (the modelled part only, not its mirror images) */
void sph_energy(const Sph *p, double *kinetic, double *internal);
void sph_momentum(const Sph *p, int body, double mom[3]); /* body -1: all */
double sph_body_mass(const Sph *p, int body);

const double *sph_positions(const Sph *p);  /* 3 per particle, m */
const double *sph_velocities(const Sph *p); /* 3 per particle, m/s */
const double *sph_density(const Sph *p);
const double *sph_pressure(const Sph *p);
const double *sph_plastic_strain(const Sph *p);
const double *sph_temperature(const Sph *p);
const int *sph_body(const Sph *p);

/* a frame: part "particles" (points) with fields speed m/s, p Pa, rho kg/m^3, ep 1, T K, body 1; mirror: also the
 * mirror images across the symmetry planes, so a quarter model is drawn whole */
void sph_write_frame(Sph *p, LabWriter *w, bool mirror);
char *sph_header_json(const SphSpec *s, const char *title);
