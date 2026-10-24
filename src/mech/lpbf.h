/* lpbf.h - inherent-strain build of a laser powder-bed part on a voxel hex mesh
 *
 * The part distorts because every layer solidifies stress-free on an already deformed, already stressed body and only
 * then contracts. This module steps that history on the shared solid solver: elements are activated (stress-free in the
 * configuration they are born into), the activated layer receives an orthotropic eigenstrain, and the whole active part
 * re-equilibrates. Removing elements (the cut) releases the internal forces they carried onto the rest.
 *
 * Every step solves for an increment du with the accumulated displacement u += du and the accumulated stress
 * sigma_e += D (B du - eps0_e), so an element activated in step k carries no stress from before k: its reference
 * configuration is the deformed configuration at its birth. That is the whole method (docs/contracts/lpbf-build.md).
 *
 * Default mechanics is elastic; optional J2 plasticity is rate independent and isothermal. Not modelled here:
 * temperature, powder conduction or melt flow, thermal laser tracks, or contact after the cut. Supports may carry
 * stated isotropic fractions or homogenised anisotropic matrices. The eigenstrain is a calibrated input, not a material
 * property. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../fem/hex8.h"
#include "../fem/solid.h"

typedef struct LpbfMesh {
    int nnodes, nelems;
    const double *xyz; /* 3 * nnodes, m; z is the build direction and the plate is at the lowest node plane */
    const int *conn;   /* 8 * nelems, hex8 */
} LpbfMesh;

typedef struct LpbfModel LpbfModel;

/* fixed_dof: 3 * nnodes, 1 for a held component. The build plate sets all three components of every node at z = 0;
 * a verification case can hold single components to remove rigid-body motion without restraining the strain. */
LpbfModel *lpbf_new(const LpbfMesh *mesh, double E, double nu, const unsigned char *fixed_dof, Hex8Formulation form, SolidSolver solver, double pcg_tol,
                    char *err, size_t errlen);
void lpbf_free(LpbfModel *m);

/* activates elements stress-free: they take the current (deformed) configuration as their reference */
void lpbf_activate(LpbfModel *m, const int *elems, int n);
/* one equilibrium step: the listed active elements receive the eigenstrain eps (xx, yy, zz), everything active
 * re-equilibrates. elems may be NULL with n = 0 to re-equilibrate without new strain. */
/* Give the model a yield stress and the plasticity is on: J2 (von Mises) with isotropic linear hardening, radial
 * return at the eight Gauss points, the equivalent plastic strain carried between layers and through the cut. Without
 * this call, or with sigma_y <= 0, every step takes the same single linear solve it always did. `hardening` is the
 * linear hardening modulus H (Pa); 0 is elastic-perfectly-plastic. See docs/contracts/lpbf-build.md section 9. */
void lpbf_set_plasticity(LpbfModel *m, double sigma_y, double hardening, int max_newton, double newton_tol);
bool lpbf_is_plastic(const LpbfModel *m);
/* A stiffness fraction per element (nelems; copied), for homogenised material such as lattice supports: the element's
 * D, and therefore the force its eigenstrain exerts, its stress, and with plasticity its yield stress and hardening,
 * are all scaled by it. NULL, or never called, means 1 everywhere and the build is unchanged. */
bool lpbf_set_scale(LpbfModel *m, const double *scale);
/* Replace the held components (3 * nnodes). Used when the part leaves the plate: the next step holds only what the
 * new mask holds. Holding a component constrains its increment from now on; what it moved before stays. */
void lpbf_set_fixed(LpbfModel *m, const unsigned char *fixed_dof);
/* The hanging nodes of an adaptive mesh (copied). Entry i ties node[i] to its nmaster[i] masters; it applies from the
 * step in which its owner element is active, to the increments from then on (a layer is activated on the deformed part,
 * so the constraint cannot and need not hold for what the node moved before). */
bool lpbf_set_hanging(LpbfModel *m, int n, const int *node, const int *nmaster, const int *master, const double *weight,
                      const int *owner);
/* the equivalent plastic strain per element (Gauss-point mean) and the largest of it; nelems, zero when elastic */
void lpbf_plastic_strain(const LpbfModel *m, double *alpha);
double lpbf_peak_plastic_strain(const LpbfModel *m);
int lpbf_newton_iterations(const LpbfModel *m);   /* total over the build */
double lpbf_last_newton_residual(const LpbfModel *m);

bool lpbf_strain(LpbfModel *m, const int *elems, int n, const double eps[3], char *err, size_t errlen);
/* removes elements (the cut): their internal forces are released onto the rest, which re-equilibrates */
bool lpbf_remove(LpbfModel *m, const int *elems, int n, char *err, size_t errlen);
/* the nodal forces (3 * nnodes, N, added into f) that the listed active elements exert through their stress: what
 * removing them would release onto the rest (the interface tear-off of supports) */
void lpbf_element_forces(const LpbfModel *m, const int *elems, int n, double *f);
/* anisotropic elements (homogenised supports): D_of[e] indexes the 36-entry matrices D (Pa, Voigt, engineering shear),
 * -1 for the isotropic material. They are elastic (no yield) and ignore the stiffness scale. NULL clears it. */
bool lpbf_set_elem_D(LpbfModel *m, const int *D_of, int nD, const double *D);

const double *lpbf_u(const LpbfModel *m);      /* 3 * nnodes, m, accumulated */
const double *lpbf_stress(const LpbfModel *m); /* 48 * nelems: 8 Gauss points x [xx yy zz xy yz zx] */
const unsigned char *lpbf_active(const LpbfModel *m);
void lpbf_von_mises(const LpbfModel *m, double *vm); /* nelems, Gauss-point mean */
int lpbf_solves(const LpbfModel *m);
double lpbf_seconds(const LpbfModel *m);
/* largest |reaction| at the plate nodes after the last step, and the residual of the last solve */
double lpbf_plate_reaction(const LpbfModel *m, double resultant[3]);
double lpbf_equilibrium_error(const LpbfModel *m);
