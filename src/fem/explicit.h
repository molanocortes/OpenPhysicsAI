/* explicit.h - explicit dynamics on the hex8: central difference, lumped mass, one-point integration with hourglass
 * control (docs/contracts/dynamics.md, step 2).
 *
 * Nothing here touches the implicit paths. The element is the large-deformation element of hex8_nl.h evaluated at one
 * point, so the same material (St. Venant-Kirchhoff, or J2 at finite strain) is used in both.
 *
 *   a_n      = M^-1 (f_ext - f_int(u_n) + f_hourglass)
 *   v_{n+1/2} = v_{n-1/2} + a_n dt,      u_{n+1} = u_n + v_{n+1/2} dt
 *
 * The stable step is dt = safety * L_c / c with c = sqrt((lambda + 2 mu) / rho) the dilatational wave speed and L_c the
 * element's volume over its largest face (the edge, for a cube; far less for a distorted element, where the shortest
 * edge is not a safe measure); the element that sets it is reported. Mass scaling raises the step by adding
 * mass to the elements below a target step, and always reports how much mass it added.
 *
 * Hourglass control is the Flanagan-Belytschko stiffness form: the four hourglass shape vectors are made orthogonal to
 * the linear field, and each mode is resisted by a stiffness Q * mu * V0 * sum |grad N|^2. Q is an input; its default
 * (0.05) is assumed, not taken from a source, and the result says so. The work the hourglass forces do is accounted
 * separately, so the energy balance shows what the stabilisation cost.
 *
 * Energy: kinetic, internal, hourglass, contact and external work are accumulated every step; `ex_energy` returns them
 * and the drift of their balance.
 *
 * LIMIT, stated because a number that cannot be read is worse than no number: `internal` is 1/2 S : E at the element
 * centre, which is the stored elastic energy only while the point is elastic. Once the material yields, E is the total
 * strain, so the term over-counts, and the plastic work is not accumulated anywhere. The balance therefore closes only
 * for elastic runs (D9: 0.002 per cent; the elastic form of D14: -2.4 per cent) and must not be read for a plastic one
 * (the plastic form of D14 reports +103 per cent). Accounting the plastic work, and taking the elastic energy from the
 * elastic Hencky strain, is registered in docs/contracts/dynamics.md as open. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "excontact.h"
#include "hex8_nl.h"

typedef struct ExMesh {
    int nnodes, nelems;
    const double *xyz; /* 3 * nnodes, m */
    const int *conn;   /* 8 * nelems */
} ExMesh;

typedef struct ExMaterial {
    Hex8NlMaterial mech;
    double density; /* kg/m^3 */
} ExMaterial;

typedef struct ExOptions {
    double safety;        /* of the stable step, default 0.9 */
    double hourglass;     /* Q, default 0.05 (assumed) */
    double mass_scale_dt; /* target step; 0 (default) means no mass scaling */
    double damping;       /* mass-proportional damping coefficient, default 0 */
} ExOptions;

typedef struct ExEnergy {
    double kinetic, internal, hourglass, external;
    double contact;  /* stored in the contact springs */
    double friction; /* dissipated by Coulomb friction */
    double contact_damping; /* dissipated by the contact dashpots */
    double total;    /* kinetic + internal + hourglass */
    double drift;    /* (total - external - total_at_start) / the largest of them */
} ExEnergy;

typedef struct ExDyn ExDyn;

/* `fixed` (3 * nnodes, may be NULL) holds a component; `fixed_velocity` (3 * nnodes, may be NULL) is the velocity
 * given to the held components (zero when absent), so an end can be driven at constant speed. */
ExDyn *ex_create(const ExMesh *mesh, const ExMaterial *mat, const unsigned char *fixed, const double *fixed_velocity, const ExOptions *opt,
                 char *err, size_t errlen);
void ex_free(ExDyn *d);

double ex_dt(const ExDyn *d);                 /* the step in use */
int ex_critical_element(const ExDyn *d);      /* the element that sets it */
double ex_mass_added(const ExDyn *d);         /* the ratio of added mass to real mass (0 without mass scaling) */
double ex_time(const ExDyn *d);
int ex_steps(const ExDyn *d);

void ex_set_velocity(ExDyn *d, const double *v);     /* 3 * nnodes, the initial velocity */
void ex_set_displacement(ExDyn *d, const double *u); /* 3 * nnodes, the initial displacement (a released shape) */
void ex_set_force(ExDyn *d, const double *f);    /* 3 * nnodes, a constant external force */
/* Mass-proportional damping (1/s), which may be changed during a run: a case that has to start from rest settles
 * with it and then continues without. It lowers the stable step, so a damped phase wants a smaller safety factor;
 * the result reports the damping used. */
void ex_set_damping(ExDyn *d, double damping);
const double *ex_u(const ExDyn *d);
const double *ex_v(const ExDyn *d);
const double *ex_stress(const ExDyn *d);
const double *ex_mass(const ExDyn *d);   /* nnodes, the lumped mass actually used (mass scaling included) */ /* 6 * nelems, the second Piola-Kirchhoff stress at the element centre */

/* Contact (excontact.h). The planes and the options are copied; the body's own free surface is used for
 * self-contact when the options ask for it. The penalty adds its own stability limit and the step is cut to
 * `safety * 2 / sqrt(k/m)` when contact is stiffer than the elements; `ex_dt` then reports the smaller step and
 * `ex_contact_cut_step` says whether contact is what set it. Call before advancing. */
bool ex_set_contact(ExDyn *d, const ExPlane *planes, int nplanes, const ExContactOptions *opt, char *err, size_t errlen);
const ExContact *ex_contact(const ExDyn *d);
/* moves a rigid surface (its position, velocity or friction) during a run; the index is the order given above */
bool ex_set_contact_plane(ExDyn *d, int index, const ExPlane *plane);
bool ex_contact_cut_step(const ExDyn *d);

/* advances to `t_end`; false when an element inverts (the state then holds the last good step) */
bool ex_advance(ExDyn *d, double t_end, char *err, size_t errlen);
void ex_energy(const ExDyn *d, ExEnergy *e);
