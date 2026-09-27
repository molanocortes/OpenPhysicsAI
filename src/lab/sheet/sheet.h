/* sheet.h - thin sheets that stretch, bend and touch: cloth, rubber membranes, balloons.
 *
 * A sheet is a triangle mesh with a thickness H0. In its plane it is an incompressible neo-Hookean membrane (rubber):
 * per triangle, with C = F^T F the in-plane Cauchy-Green tensor of the 3 x 2 deformation gradient,
 *
 *   W = mu / 2 (tr C + 1 / det C - 3) per unit reference volume      (the third stretch from incompressibility)
 *
 * so that a sheet of reference area A0 stores A0 H0 W.
 * Out of its plane it bends as a Kirchhoff plate of rigidity D = E H0^3 / (12 (1 - nu^2)), E = 3 mu, nu = 1/2: each
 * triangle's shape operator comes from the dihedral angles of its three edges (the mid-edge normal model), and it
 * stores A D / 2 (nu (tr M)^2 + (1 - nu) tr M^2) of the change from its rest shape; free edges relax, edges against
 * fully pinned triangles are clamped (sheet.c says how, sheettest checks it). Nodes carry the sheet's mass; gravity,
 * an inside pressure (for balloons), velocity damping; contact with
 * rigid bodies given as a signed distance (labshape.h) pushes nodes out and applies Coulomb friction; with self_contact
 * set, parts of the sheet (a fold, or separate pieces in one mesh) push each other apart when closer than that distance
 * (a damped penalty between nodes and triangles, found through a spatial hash). Time: velocity Verlet, threaded. SI. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "../../threads.h"

typedef struct SheetSpec {
    double shear_modulus, thickness, density; /* mu (Pa), H0 (m), kg/m^3 */
    double bending_scale;   /* multiplies the plate's bending rigidity (1: the sheet's own) */
    double gravity;         /* m/s^2 along -z */
    double damping;         /* velocity damping rate, 1/s */
    double pressure;        /* inside pressure, Pa (a closed sheet's outward normal side is outside) */
    double friction;        /* Coulomb coefficient against the bodies */
    double contact_distance; /* how far from a body's surface a node is held, m (0: half the thickness) */
    double viscosity;       /* Kelvin-Voigt viscosity of the membrane, Pa s: a stress 2 eta dE/dt (0: purely elastic) */
    double self_contact;    /* > 0: the sheet's parts keep this distance from each other, m (folds, leaflets meeting);
                               a penalty between each node and the triangles near it that are not its neighbours at rest */
} SheetSpec;

typedef struct Sheet Sheet;
typedef double (*SheetSdf)(const double p[3], void *ctx);

/* nodes (3 per), triangles (3 indices per); a triangle's node order sets its normal (right hand) */
Sheet *sheet_create(const SheetSpec *s, int nnodes, const double *xyz, int ntri, const int *tri, char *err, size_t errlen);
void sheet_free(Sheet *S);
void sheet_set_bodies(Sheet *S, SheetSdf sdf, void *ctx);
void sheet_pin(Sheet *S, int node);              /* held in place from now on */
void sheet_set_pressure(Sheet *S, double p);
/* forces from outside (a fluid), N per node (3 each), held until set again; NULL clears them */
void sheet_set_external(Sheet *S, const double *f);
/* each node's share of the sheet's rest area, m^2 (a third of its triangles') */
const double *sheet_node_areas(const Sheet *S);
double sheet_node_mass(const Sheet *S, int node);
bool sheet_pinned(const Sheet *S, int node);
/* positions and velocities (6 per node) out and back in: to try a step and take it back */
void sheet_save(const Sheet *S, double *state);
void sheet_restore(Sheet *S, const double *state);
double sheet_stable_dt(const Sheet *S);
void sheet_step(Sheet *S, double dt, ThreadPool *pool);
const double *sheet_positions(const Sheet *S);
const double *sheet_velocities(const Sheet *S);
/* elastic energies (membrane, bending) and the kinetic energy, J */
void sheet_energy(const Sheet *S, double *membrane, double *bending, double *kinetic);
/* the largest principal stretch of each triangle (for colour) */
void sheet_stretch(const Sheet *S, double *lambda);
int sheet_nodes(const Sheet *S);
int sheet_triangles(const Sheet *S);
const int *sheet_triangle_nodes(const Sheet *S);
double sheet_volume(const Sheet *S); /* the volume a closed sheet encloses (divergence theorem), m^3 */
