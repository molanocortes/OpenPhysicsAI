/* flexbody.h - reduced elastic model of a meshed part for the multibody dynamics (Craig-Bampton with rigid interfaces)
 *
 * The part's reference frame is its body frame, attached where the body's own joint holds it: the nodes of the root region
 * are clamped. Each child interface is a set of nodes that moves rigidly with a reference point (the child joint's origin),
 * so it has 6 motions. The basis is:
 *   constraint modes     the static shape for a unit motion of one interface (the other interfaces and the root held), 6 per
 *                        interface: loads that enter through the interfaces are reproduced statically exactly
 *   fixed-interface modes the lowest natural modes with every interface and the root held
 * The reduced mass and stiffness are diagonalised, giving mass-normalised coordinates with frequencies of the clamped part
 * whose interfaces are free but rigid. For each coordinate the multibody invariants are computed from the consistent mass
 * matrix, about the body origin and in body axes: interface translation and rotation, ell = [integral rho x cross phi dV;
 * integral rho phi dV] and the first-order inertia change dJ. The first-order theory needs small deformations; the
 * frequencies, the static interface response and the invariants are reported so the reduction can be judged. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "modal.h"
#include "multibody.h"

typedef struct FlexReduceInput {
    const OrthoModel *om;           /* mesh and material in FE coordinates */
    const double *density;          /* nelems, kg/m^3 */
    const int *root_nodes;          /* clamped nodes (the region held by the body's own joint) */
    int nroot;
    int ninterfaces;
    const int *const *itf_nodes;    /* nodes of each child interface */
    const int *itf_count;
    const double (*itf_point)[3];   /* reference point of each interface, FE coordinates (m) */
    int fixed_modes;                /* fixed-interface normal modes kept */
    double zeta;                    /* modal damping ratio given to every coordinate */
    double R[9], p[3];              /* body -> FE placement: x_fe = R x_body + p */
    double max_frequency_hz;        /* 0 = keep every coordinate; otherwise coordinates above it are dropped (reported) */
    bool keep_shapes;               /* return the displacement shapes of the coordinates (report.shapes) */
} FlexReduceInput;

typedef struct FlexReduceReport {
    int coordinates, constraint_modes, fixed_modes, dropped;
    double fe_mass, fe_com[3];      /* Gauss integration of the density, body coordinates */
    double fe_inertia[9];           /* about the FE mass centre, body axes */
    double max_frequency_hz;        /* highest kept coordinate */
    /* static flexibility of the interfaces lost by dropping coordinates: largest relative loss on the diagonal of the
     * interface compliance (translations and rotations of each interface), 0 when nothing is dropped */
    double compliance_loss;
    double *interface_loss;         /* 6 * ninterfaces: the loss per interface direction (x y z translation, x y z rotation); free() */
    bool fixed_modes_converged;
    double *shapes;                 /* keep_shapes: coordinates * 3 * nnodes displacement per unit coordinate, FE axes (free) */
    char note[256];
} FlexReduceReport;

/* fills out (body index left to the caller) with the kept coordinates of the 6 * ninterfaces + fixed_modes basis, in ascending
 * frequency; free report->shapes and report->interface_loss with free() */
bool flex_reduce(const FlexReduceInput *in, MbFlexDef *out, FlexReduceReport *rep, char *err, size_t errlen); /* free with mbflex_free */
