/* loads.h - transfer of rigid-multibody loads to a hexahedral FEM mesh (fidelity level 3: rigid motion + FEM load assessment)
 *
 * A body that moves as a rigid body is assessed quasi-statically at one instant with d'Alembert's principle: the wrenches
 * its joints exert on it, plus the body force rho * (g - a(x)) of its own motion, are in equilibrium. Nothing is clamped
 * to make the stiffness invertible: rigid-body motion is removed by an isostatic 3-2-1 support whose reactions are computed
 * and must be negligible. When they are not, the rigid model and the FEM model disagree (mass, centre of mass, inertia or
 * load placement) and the imbalance is reported, not hidden.
 *
 * Distribution of a wrench over an attachment region (hexahedron faces). With c the area centroid and r = x - c, the traction
 *   t(x) = F / A + w x r,  w = J^-1 M_c,  J = sum over faces of A_f (|r_f|^2 E - r_f r_f^T)
 * reproduces the force F and the moment M_c about c exactly (the uniformly weighted distributing coupling). Each face
 * receives the traction at its centroid, and consistent nodal forces follow. When J is singular (for example a straight
 * line of faces), a moment about the degenerate direction cannot be carried and the transfer fails with that direction.
 *
 * Inertial body force: b(x) = -(s0 + alpha x x + omega x (omega x x)) with s0 the specific force (acceleration minus
 * gravity) of the body point at the FEM origin; integrated with 2x2x2 Gauss points (exact for this linear field on
 * parallelepiped elements). All quantities in the FEM frame and SI units. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct FemMeshView {
    int nnodes, nelems;
    const double *xyz;     /* 3*nnodes (m) */
    const int *conn;       /* 8*nelems, hex8 node order of fem/hex8.h */
    const double *density; /* nelems (kg/m^3) */
} FemMeshView;

typedef struct FaceSet {
    int nfaces;
    const int *elem;
    const unsigned char *local; /* local face index of fem/hex8.h */
} FaceSet;

typedef struct WrenchTransfer {
    double area;          /* m^2 */
    double centroid[3];   /* m */
    double J[9];          /* area second-moment tensor about the centroid (m^4) */
    double J_eig[3];      /* its eigenvalues, descending */
    double applied_force[3], applied_moment[3]; /* resultant of the nodal forces (moment about the requested point) */
    double force_error, moment_error;           /* relative mismatch against the request */
    double max_traction;  /* Pa, largest face traction magnitude */
} WrenchTransfer;

/* adds nodal forces reproducing force F and moment M (about point p) over the faces; false if the region cannot carry it */
bool loads_distribute_wrench(const FemMeshView *mv, const FaceSet *fs, const double F[3], const double M[3], const double p[3], double *nodal_force,
                             WrenchTransfer *rep, char *err, size_t errlen);

typedef struct InertialLoad {
    double mass;           /* kg of the FEM elements */
    double com[3];         /* m */
    double resultant[3];   /* N */
    double moment[3];      /* N m about the FEM origin */
} InertialLoad;

/* adds the consistent nodal forces of b(x) over the listed elements (all when elems is NULL) */
void loads_inertial(const FemMeshView *mv, const int *elems, int nelems, const double s0[3], const double omega[3], const double alpha[3], double *nodal_force,
                    InertialLoad *rep);

/* adds the consistent nodal forces of the linear body force field b(x) = -rho (B x + c) (B row-major 3x3) over all elements;
 * the d'Alembert force of a rigid motion is B = [alpha]x + w w^T - |w|^2 I, c = s0 */
void loads_body_linear(const FemMeshView *mv, const double B[9], const double c[3], double *nodal_force);

typedef struct IsostaticSupport {
    int node[3];           /* A: x y z fixed; B: two directions normal to AB; C: the direction normal to plane ABC */
    double dir[3][3];      /* constrained directions: B uses dir[0], dir[1]; C uses dir[2] */
} IsostaticSupport;

/* chooses A, B, C among the nodes (all when nodes is NULL): far apart and not collinear; false for degenerate node sets */
bool loads_isostatic_choose(const FemMeshView *mv, const int *nodes, int nnodes, IsostaticSupport *s);
/* fixed flags for a solver that constrains Cartesian components: requires the constrained directions of B and C to be
 * coordinate axes, which loads_isostatic_choose guarantees */
void loads_isostatic_fixed(const IsostaticSupport *s, unsigned char *fixed);

/* resultant force and moment (about p) of nodal forces */
void loads_resultant(const FemMeshView *mv, const double *nodal_force, const double p[3], double F[3], double M[3]);
