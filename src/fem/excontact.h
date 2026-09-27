/* excontact.h - node-to-surface penalty contact for the explicit path (docs/contracts/dynamics.md, step 3).
 *
 * A slave node that has crossed a surface is pushed back by a spring: f = k g along the surface normal, with g the
 * penetration. Tangentially the same spring is used with a Coulomb limit: the node sticks (the tangential spring
 * stretches with the relative slip) until |f_t| reaches mu |f_n|, and then slides with |f_t| = mu |f_n| against the
 * relative tangential velocity. The friction law and the pair rule mu = min(mu_A, mu_B) are the mechanics layer's
 * (src/mech/contact.h); the rigid-body solver there is impulse-based and is not duplicated here, because an explicit
 * step is far smaller than a rigid-body step and a penalty is the form that suits it.
 *
 * Surfaces:
 *   - rigid planes (ExPlane), which may translate at a constant velocity: the crush plates, a floor, an incline;
 *   - the deforming free surface of the mesh itself (self-contact), from the faces of the hexes that no second
 *     element shares. Facets that share a node with the slave node are skipped, so a surface never contacts itself
 *     across its own corner.
 *
 * Search: a uniform grid (buckets) over the facet centroids, rebuilt every `rebuild_every` steps; the interval is an
 * input and the result reports it, because a surface that folds faster than the rebuild can miss a contact.
 *
 * Penalty stiffness: k = scale * K * A / h per facet, K the bulk modulus, A the facet area and h the thickness of the
 * element behind it (its volume over that area), the usual explicit form. It adds its own stability limit,
 * dt <= 2 sqrt(m_min / k_max); `excontact_max_stiffness` reports it so the caller can cut the step and say so.
 *
 * Energy: the springs' stored energy and the work friction has dissipated are both accounted, so the balance of an
 * impact stays readable. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct ExPlane {
    double point[3];    /* any point of the plane */
    double normal[3];   /* unit, pointing into the half space the body occupies */
    double velocity[3]; /* the plane's own motion (m/s), constant */
    double friction;    /* Coulomb coefficient of the plane */
} ExPlane;

typedef struct ExContactOptions {
    double scale;        /* the penalty scale factor, default 0.1 (assumed, reported) */
    double friction;     /* the body's own Coulomb coefficient; the pair takes the smaller of the two */
    int rebuild_every;   /* steps between bucket rebuilds, default 10 */
    bool self_contact;   /* node against the body's own free surface */
    double damping;      /* normal dashpot, as a fraction of critical for the contact spring (default 0.1, assumed).
                          * Without it a stiff penalty rings: the static penetration of a resting body is a fraction
                          * of a micrometre, so any oscillation of that size lifts it off and friction comes and goes.
                          * It acts only while the surfaces approach or separate, never in steady sliding, and what it
                          * dissipates is accounted separately. */
    double depth_limit;  /* a node deeper than this is treated as having passed through and is reported, not pushed;
                          * 0 asks for the automatic value (half the representative facet size) */
} ExContactOptions;

typedef struct ExContact ExContact;

/* `mass` is the lumped nodal mass (nnodes). `bulk` is the material's bulk modulus. The planes are copied. */
ExContact *excontact_create(int nnodes, int nelems, const double *xyz, const int *conn, const double *mass, double bulk,
                            const ExPlane *planes, int nplanes, const ExContactOptions *opt, char *err, size_t errlen);
void excontact_free(ExContact *c);

/* replaces a plane during a run (a plate that moves, or a friction coefficient that changes) */
bool excontact_set_plane(ExContact *c, int index, const ExPlane *plane);

int excontact_nfacets(const ExContact *c);
int excontact_nsurface_nodes(const ExContact *c);
double excontact_max_frequency(const ExContact *c); /* the largest sqrt(k/m) of a slave node, for the step limit */

/* Adds the contact forces at the deformed coordinates `x` and velocities `v` into `f` (which the caller has already
 * filled with the other forces), advancing the friction state by `dt`. `step` drives the bucket rebuild. */
void excontact_forces(ExContact *c, const double *x, const double *v, double dt, int step, double *f);

double excontact_energy(const ExContact *c);         /* the springs' stored energy */
double excontact_friction_work(const ExContact *c);  /* what friction has dissipated */
double excontact_plate_work(const ExContact *c);     /* the work a moving plane has fed into the springs */
double excontact_damping_work(const ExContact *c);   /* what the normal dashpots have dissipated */
/* The impulse the rigid surfaces have given the body since the start (N s), normal and friction together: the mean
 * contact force over a window is the difference of two readings over its length. Self-contact is internal and does
 * not enter it. */
void excontact_impulse(const ExContact *c, double out[3]);
/* the force the rigid surfaces exerted in the last call, and the largest radius (about the given axis point) at
 * which a node was in contact with them, which is the contact patch's radius */
void excontact_force(const ExContact *c, double out[3]);
double excontact_patch_radius(const ExContact *c);
double excontact_peak_pressure(const ExContact *c); /* the largest nodal force over that node's share of the surface */
double excontact_max_penetration(const ExContact *c);/* the deepest penetration seen (m) */
int excontact_passed_through(const ExContact *c);    /* nodes seen deeper than the depth limit */
int excontact_active(const ExContact *c);            /* contacts in the last call */
