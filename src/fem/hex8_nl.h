/* hex8_nl.h - the 8-node hexahedron at large deformation: total Lagrangian, Green-Lagrange strain and second
 * Piola-Kirchhoff stress, with the consistent tangent (material and geometric parts).
 *
 * The linear element of hex8.h is untouched and keeps its behaviour; this is a separate element used by the nonlinear
 * solver. Voigt order and engineering shear are the same as there: [xx, yy, zz, xy, yz, zx].
 *
 *   F_iJ = delta_iJ + sum_a u_ai dN_a,J        (material gradients, on the undeformed shape)
 *   E    = (F'F - I) / 2                        Green-Lagrange
 *   S    = C : E                                St. Venant-Kirchhoff, C the same matrix the linear path uses
 *   f_e  = integral B_L' S dV0                  internal force on the undeformed volume
 *   K_e  = integral (B_L' C B_L + G' S G) dV0   material plus geometric (initial-stress) stiffness
 *
 * Full 2 x 2 x 2 integration locks in bending. With HEX8_NL_EAS the element carries nine enhanced assumed strain
 * modes, condensed element by element, which is the nonlinear counterpart of the incompatible modes the linear
 * element uses: the enhanced strain is added to the Green-Lagrange strain, its parameters follow from the element's
 * own equilibrium, and for St. Venant-Kirchhoff they solve in one step (the strain is linear in them), so the element
 * keeps no state for them. The plastic branch stays fully integrated for now, and says so in its results.
 *
 * A material point may instead carry J2 plasticity at finite strain by the multiplicative route (F = Fe Fp, the return
 * map in logarithmic strain, the plastic part updated by the exponential map): docs/contracts/dynamics.md section 1.
 * `Hex8NlState` holds what that needs per Gauss point. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* per Gauss point, 8 per element; zero-initialised means "undeformed and elastic" */
typedef struct Hex8NlState {
    double Fp_inv[9]; /* the inverse of the plastic deformation gradient, row-major; zero is read as the identity */
    double alpha;     /* equivalent plastic strain */
} Hex8NlState;

typedef struct Hex8NlMaterial {
    double E, nu;       /* Pa, - */
    double yield;       /* Pa; <= 0 keeps the point elastic (St. Venant-Kirchhoff) */
    double hardening;   /* Pa, linear isotropic */
} Hex8NlMaterial;

/* Internal force and tangent of one element at the displacement `ue` (24, node-major x y z).
 * `Ke` may be NULL (force only). `gp_S` and `gp_E` (48 each) may be NULL. `state` (8 points) may be NULL for a purely
 * elastic material; when it is given and the material yields, it is updated in place only if `commit` is true, so a
 * Newton iteration can probe without changing the state. `min_detF` receives the smallest Jacobian of the deformation
 * over the Gauss points; false is returned when an element is inverted (det F <= 0) or its reference shape is
 * degenerate. */
enum { HEX8_NL_FULL = 0, HEX8_NL_EAS = 1 };

bool hex8_nl_element(const double X[8][3], const double ue[24], const Hex8NlMaterial *m, Hex8NlState *state, bool commit, unsigned flags,
                     double fe[24], double Ke[576], double *gp_S, double *gp_E, double *min_detF);

/* One-point (reduced) evaluation at the element centre, for the explicit path: the internal force and the second
 * Piola-Kirchhoff stress there, with the same material as the full element. Hourglass control is the caller's
 * (explicit.c); without it this element has zero-energy modes. */
bool hex8_nl_one_point(const double X[8][3], const double ue[24], const Hex8NlMaterial *m, Hex8NlState *state, bool commit, double fe[24],
                       double S_out[6], double *detF_out);

/* The Cauchy stress at a Gauss point from the stored second Piola-Kirchhoff stress and the deformation gradient:
 * sigma = F S F' / det F. `S` and `sigma` are Voigt (engineering shear irrelevant for stress). */
void hex8_nl_cauchy(const double F[9], const double S[6], double sigma[6]);

/* the deformation gradient at the element centre (the one-point location) */
bool hex8_nl_centre_gradient(const double X[8][3], const double ue[24], double F[9], double *detJ0);

/* the deformation gradient at one Gauss point (g = 0..7), for tests and post-processing */
bool hex8_nl_gradient(const double X[8][3], const double ue[24], int g, double F[9], double *detJ0);
