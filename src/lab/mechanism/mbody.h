/* mbody.h - one elastic part of a mechanism: a hexahedral mesh, a linear isotropic material, the nodes where it is held,
 * its stiffness factorised once in its own frame, and the condensed compliance of the nodes where it may touch
 * another part.
 *
 * Small strain, linear elasticity: the 8-node hexahedron with incompatible modes (src/fem/hex8.h), whose bending is
 * right on a few elements through a thickness. A part that moves as a rigid body in the mechanism (a gear turning)
 * keeps the same stiffness in its own frame, so it is factorised once for every position the mechanism takes.
 *
 * Condensed compliance: for zone nodes i, j with unit directions d_i, d_j (in the part's frame), G_ij = d_i' K^-1 d_j is
 * the displacement of node i along d_i under a unit force at node j along d_j, with the held nodes fixed. Contact
 * between parts is solved on these matrices (mcontact.h), and the full displacement field afterwards by one more
 * solve with the contact forces found. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "mfactor.h"

typedef struct MBody {
    int nnodes, nelems;
    const double *xyz; /* 3 * nnodes, m, the part's own frame (borrowed) */
    const int *conn;   /* 8 * nelems (borrowed) */
    double E, nu;      /* Pa, - */
    int *eq;           /* 3 * nnodes: the equation of each component, -1 where held */
    int neq;
    MFactor *K;
    double factor_seconds, assemble_seconds;
    /* the zone: condensed compliance along each zone node's direction */
    int nzone;
    int *zone_node;   /* nzone */
    double *zone_dir; /* 3 * nzone, unit */
    double *G;        /* nzone * nzone, m/N, symmetric; with cyclic symmetry zone_cyc blocks of zone_per^2 (below) */
    int *zone_of;     /* nnodes: the zone index of a node, or -1 */
    int zone_cyc, zone_per; /* cyclic zone: zone_cyc sectors of zone_per nodes each; 0 when it is not cyclic */
    double zone_seconds;
} MBody;

/* the element: incompatible modes (the default) or, with full = 1, the plain trilinear element (for comparisons) */
void mbody_set_formulation(int full);
/* held: per node, nonzero where all three components are fixed */
bool mbody_create(MBody *b, int nnodes, const double *xyz, int nelems, const int *conn, double E, double nu, const unsigned char *held,
                  char *err, size_t errlen);
/* the same with springs to ground at nodes: nspring nodes, each with a symmetric 3 x 3 stiffness (N/m, row-major) added
 * to its block, for supports that hold some directions and not others */
bool mbody_create_springs(MBody *b, int nnodes, const double *xyz, int nelems, const int *conn, double E, double nu, const unsigned char *held,
                          int nspring, const int *spring_node, const double *spring_k, char *err, size_t errlen);
void mbody_free(MBody *b);

/* the zone: nodes and their unit directions (3 each, in the part's frame); computes G with blocks of solves */
bool mbody_zone(MBody *b, int n, const int *nodes, const double *dirs, char *err, size_t errlen);

/* A zone with cyclic symmetry, for a whole gear: ncyc sectors (its teeth) of nper nodes each, ordered sector by sector,
 * sector s the image of sector 0 turned by s 2 pi / ncyc about the z axis with its nodes in the same order and its
 * directions turned with them, on a body whose mesh and held nodes have the same symmetry. Then G between node a of
 * sector s and node c of sector t depends only on (t - s) mod ncyc: it is stored as ncyc blocks of nper x nper and
 * computed from nper solves (the loads on sector 0) instead of ncyc nper. The symmetry is checked: each node must be
 * the image of its sector-0 counterpart within 1e-9 of the body's size, or the call fails. */
bool mbody_zone_cyclic(MBody *b, int ncyc, int nper, const int *nodes, const double *dirs, char *err, size_t errlen);

/* G_ik of a zone, plain or cyclic */
static inline double mbody_g(const MBody *b, int i, int k) {
    if (b->zone_cyc <= 0) return b->G[(size_t)i * b->nzone + k];
    int n = b->zone_per, s = i / n, t = k / n, d = t - s;
    if (d < 0) d += b->zone_cyc;
    return b->G[((size_t)d * n + (i - s * n)) * n + (k - t * n)];
}

/* displacement u (3 * nnodes, m) under nodal forces f (3 * nnodes, N; components at held nodes are ignored) */
bool mbody_solve(MBody *b, const double *f, double *u);

/* Nodal strain and stress (6 components each, Voigt [xx yy zz xy yz zx], engineering shear strain), extrapolated from
 * the Gauss points and averaged over the elements sharing a node; and per element the von Mises stress at its centre
 * (mean of its Gauss points). Any output may be NULL. */
void mbody_fields(const MBody *b, const double *u, double *node_strain, double *node_stress, float *elem_mises);

/* von Mises stress at the nodes from their strains (mbody_fields' node_strain), Pa */
void mbody_node_mises(const MBody *b, const double *node_strain, float *node_mises);

/* strain at a point given by element and natural coordinates (xi, eta, zeta in [-1, 1]), interpolated from the nodal
 * strains of mbody_fields */
void mbody_strain_at(const MBody *b, const double *node_strain, int elem, const double xi[3], double eps[6]);
