/* hex8.h - 8-node isoparametric hexahedron for small-strain solid mechanics and heat conduction
 *
 * Node order in natural coordinates (xi, eta, zeta):
 *   0 (-,-,-)  1 (+,-,-)  2 (+,+,-)  3 (-,+,-)  4 (-,-,+)  5 (+,-,+)  6 (+,+,+)  7 (-,+,+)
 * Voigt order for stress and strain: [xx, yy, zz, xy, yz, zx], engineering shear strains (gamma = 2 eps).
 * Integration: 2x2x2 Gauss points ordered like the nodes (gp g at XI[g]/sqrt(3)).
 *
 * Formulations
 *   HEX8_FULL          standard trilinear displacement element, full integration. Locks in bending (shear
 *                      locking) and for nearly incompressible response; converges slowly for thin parts.
 *   HEX8_INCOMPATIBLE  Wilson-Taylor incompatible modes (9 internal bubble modes, condensed per element, with the
 *                      Taylor correction so the constant-strain patch test passes on distorted elements). Much
 *                      better in bending; the default for linear analysis.
 * Faces (outward normals): 0 zeta=-1, 1 zeta=+1, 2 eta=-1, 3 xi=+1, 4 eta=+1, 5 xi=-1. */
#pragma once

#include <stdbool.h>

enum { HEX8_NODES = 8, HEX8_DOFS = 24, HEX8_GP = 8 };

typedef enum { HEX8_FULL = 0, HEX8_INCOMPATIBLE = 1 } Hex8Formulation;

extern const double HEX8_XI[8][3];
extern const int HEX8_FACE_NODES[6][4];

void hex8_shape(double xi, double eta, double zeta, double N[8], double dN[8][3]);
/* J (row i = d x_i / d xi_j), inverse-mapped derivatives dNdx; returns det J */
double hex8_jacobian(const double X[8][3], const double dN[8][3], double J[3][3], double dNdx[8][3]);
void isotropic_D(double E, double nu, double D[6][6]);

/* Element stiffness Ke (24x24, row-major, dof order x0 y0 z0 x1 ...). Returns false for a non-positive Jacobian at any
 * Gauss point (inverted or degenerate element); min_detJ receives the smallest Jacobian determinant. */
bool hex8_stiffness(const double X[8][3], const double D[6][6], Hex8Formulation f, double Ke[24 * 24], double *min_detJ);

/* Strains and stresses at the 8 Gauss points for nodal displacements ue (24). eps0 (optional, 6 per element, e.g.
 * thermal strain) is subtracted before applying D: sig = D (eps - eps0). */
void hex8_gauss_strain_stress(const double X[8][3], const double D[6][6], Hex8Formulation f, const double ue[24],
                              const double *eps0, double eps[8][6], double sig[8][6]);
/* Equivalent nodal forces of a uniform initial strain eps0: f = integral B^T D eps0 dV (for incompatible modes the
 * condensed load, consistent with hex8_stiffness). */
void hex8_initial_strain_load(const double X[8][3], const double D[6][6], Hex8Formulation f, const double eps0[6], double fe[24]);
/* consistent load of a uniform body force b (N/m^3): f = integral N^T b dV */
void hex8_body_load(const double X[8][3], const double b[3], double fe[24]);
/* consistent load of a uniform traction t (Pa) on face `face`; returns the face area and its outward unit normal */
void hex8_face_load(const double X[8][3], int face, const double t[3], double fe[24], double *area, double normal[3]);
double hex8_volume(const double X[8][3]);
/* Jacobian determinant at each of the 8 Gauss points (unit weights) */
void hex8_gauss_detj(const double X[8][3], double detj[8]);
/* extrapolate Gauss point values (8 x ncomp) to the element nodes (8 x ncomp) */
void hex8_extrapolate(const double *gp, int ncomp, double *nodal);
