/* peri.h - brittle solids that crack: bond-based peridynamics (Silling 2000), in three dimensions.
 *
 * The solid is a set of points on a cubic lattice of spacing h, each carrying the mass of its cell. Every pair closer
 * than the horizon delta (3 h here) is a bond, a spring that acts along the line between them:
 *
 *   f = c s w_vol (unit vector),  s = (|y_j - y_i| - |x_j - x_i|) / |x_j - x_i|  (the bond's stretch)
 *
 * the prototype microelastic brittle material (PMB, Silling and Askari 2005): c = 18 K / (pi delta^4) matches the bulk
 * modulus K (Poisson's ratio is then 1/4, the bond-based model's own), with pi delta^4 replaced by the lattice's own
 * sum of |xi| dV so that an interior point is exact, and a bond breaks for good when its stretch
 * passes s0 = sqrt(5 G0 / (9 K delta)), which makes the energy to separate a unit area equal the fracture energy G0.
 * Volumes of partial neighbours are corrected linearly across the horizon's edge (Bobaru). Time: velocity Verlet at a
 * fraction of the stable step. A rigid floor at z = floor pushes back by a penalty. Threaded over points.
 *
 * Damage at a point is the fraction of its bonds that have broken: 0 intact, towards 1 a free fragment's edge. Units SI. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "../../threads.h"

typedef struct PeriSpec {
    double h;            /* lattice spacing, m */
    double horizon;      /* in spacings, default 3 */
    double density, bulk_modulus, fracture_energy; /* kg/m^3, Pa, J/m^2 */
    double floor_z;      /* the rigid floor's height; -inf for none */
    double floor_stiffness; /* penalty, N/m per point; 0: from the material */
    double gravity;      /* m/s^2 along -z */
} PeriSpec;

typedef struct Peri Peri;
typedef double (*PeriSdf)(const double p[3], void *ctx); /* metres, negative inside */

/* points on the lattice inside the body, within the box [lo, hi] */
Peri *peri_create(const PeriSpec *s, PeriSdf sdf, void *ctx, const double lo[3], const double hi[3], char *err, size_t errlen);
void peri_free(Peri *P);
int peri_count(const Peri *P);
long peri_bonds(const Peri *P);
void peri_set_velocity(Peri *P, const double v[3]);
/* the stable step's fraction taken: dt = safety * h / c_p */
double peri_stable_dt(const Peri *P);
void peri_step(Peri *P, double dt, ThreadPool *pool);
/* per point: current position (3), velocity (3), damage; any may be NULL */
void peri_state(const Peri *P, double *x, double *v, double *damage);
/* kinetic and bond (elastic) energy, and bonds broken so far */
void peri_energy(const Peri *P, double *kinetic, double *elastic, long *broken);
/* the model's constants: micromodulus c, critical stretch s0 */
void peri_constants(const Peri *P, double *c, double *s0);
/* the reference positions of the points */
const double *peri_reference(const Peri *P);
/* moves every point by the displacement field u(x) (for tests: a uniform strain) */
void peri_displace(Peri *P, void (*u)(const double x[3], double out[3], void *ctx), void *ctx);
/* breaks every bond that crosses the plane where coordinate `axis` equals c, within a0 <= coordinate `along` <= a1
 * (all of the third): a pre-crack, a notch */
long peri_cut(Peri *P, int axis, double c, int along, double a0, double a1);
/* sets each point's velocity from a field of its reference position */
void peri_set_velocity_field(Peri *P, void (*v)(const double x[3], double out[3], void *ctx), void *ctx);
/* the points inside the box [lo, hi] move at velocity v from now on (a grip) */
void peri_grip(Peri *P, const double lo[3], const double hi[3], const double v[3]);
/* the elastic energy stored in the bonds of point i (half of each bond's), J */
double peri_point_energy(const Peri *P, int i);
