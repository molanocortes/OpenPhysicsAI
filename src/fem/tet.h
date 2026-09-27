/* tet.h - 4-node and 10-node tetrahedra for small-strain solid mechanics (docs/contracts/tet-mesh.md)
 *
 * Natural coordinates are volume coordinates L0..L3 (L0 = 1 - xi - eta - zeta, L1 = xi, L2 = eta, L3 = zeta).
 * TET4: linear, constant strain, one quadrature point. Locks in bending; kept for comparison and for quick meshes.
 * TET10: quadratic, node order corners 0-3 then mid-edge nodes 4 (0,1), 5 (1,2), 6 (2,0), 7 (0,3), 8 (1,3), 9 (2,3)
 * (the order of src/geom/tetmesh.h), four quadrature points (degree 2, exact for a straight-sided element).
 * Voigt order and engineering shear strains as in hex8.h: [xx, yy, zz, xy, yz, zx]. Local face k is opposite corner k.
 *
 * The solver entry points are reached through solid_solve and solid_check_constraints when HexModel.elem_type is
 * SOLID_ELEM_TET4 or SOLID_ELEM_TET10; the hexahedral path is not touched. */
#pragma once

#include <stdbool.h>

typedef enum { SOLID_ELEM_HEX8 = 0, SOLID_ELEM_TET4 = 1, SOLID_ELEM_TET10 = 2 } SolidElemType;

enum { TET_MAX_NODES = 10, TET_MAX_GP = 4 };

extern const int TET_EDGE[6][2];  /* corner pairs of the mid-edge nodes 4..9 */
extern const int TET_FACE[4][3];  /* corners of local face k, outward */
extern const int TET10_FACE_MID[4][3]; /* mid-edge nodes of face k, following its corners (between corner q and q+1) */

int tet_nodes(int type); /* 4 or 10 */
int tet_gauss_count(int type); /* 1 or 4 */
/* quadrature points as volume coordinates and weights (the weights sum to 1/6, the reference volume) */
void tet_gauss_points(int type, double L[][4], double *w);
/* shape functions and their derivatives with respect to (xi, eta, zeta) */
void tet_shape(int type, const double L[4], double *N, double (*dN)[3]);
/* J (row i = d x_i / d xi_j) and derivatives with respect to x; returns det J */
double tet_jacobian(int type, const double (*X)[3], const double (*dN)[3], double J[3][3], double (*dNdx)[3]);

/* element stiffness (3n x 3n, row-major, dof order x0 y0 z0 x1 ...); false for a non-positive Jacobian */
bool tet_stiffness(int type, const double (*X)[3], const double D[6][6], double *Ke, double *min_detJ);
/* strains and stresses at the quadrature points; sig = D (eps - eps0) */
void tet_gauss_strain_stress(int type, const double (*X)[3], const double D[6][6], const double *ue, const double *eps0, double (*eps)[6],
                             double (*sig)[6]);
void tet_initial_strain_load(int type, const double (*X)[3], const double D[6][6], const double eps0[6], double *fe);
void tet_body_load(int type, const double (*X)[3], const double b[3], double *fe);
/* uniform traction t (Pa) on local face `face` (curved faces of TET10 integrated with a degree-4 rule) */
void tet_face_load(int type, const double (*X)[3], int face, const double t[3], double *fe, double *area, double normal[3]);
/* uniform pressure p (Pa) acting against the outward normal of local face `face`, following the curved face */
void tet_face_pressure(int type, const double (*X)[3], int face, double p, double *fe, double *area);
/* a traction that varies over the face: fn gives t (Pa) at a point x with outward unit normal n */
typedef void (*TetTractionFn)(void *ctx, const double x[3], const double n[3], double t[3]);
void tet_face_load_fn(int type, const double (*X)[3], int face, TetTractionFn fn, void *ctx, double *fe);
/* Jacobian determinant times weight at each quadrature point (their sum is the volume) */
void tet_gauss_dv(int type, const double (*X)[3], double *dv);
double tet_volume(int type, const double (*X)[3]);
/* quadrature-point values (ngp x ncomp) extrapolated to the nodes (n x ncomp): linear through the four points,
 * mid-edge nodes the mean of their corners */
void tet_extrapolate(int type, const double *gp, int ncomp, double *nodal);
/* interpolation at a point given by volume coordinates */
void tet_interpolate(int type, const double *nodal, int ncomp, const double L[4], double *out);
