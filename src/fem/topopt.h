/* topopt.h - compliance topology optimisation on the voxel hexahedral mesh (docs/contracts/topology-optimisation.md)
 *
 * A loop around solid_solve: modified SIMP densities enter the solver as the per-element stiffness multiplier it
 * already accepts (HexModel.elem_scale), the sensitivities come from the Gauss-point strains and stresses it
 * already returns, the sensitivity filter is the one of the published 99-line code, and the update is optimality
 * criteria with a move limit. One linear solve per iteration; nothing in the solver changes.
 *
 * Compliance is c = f^T u with zero prescribed displacements, which is twice the strain energy; both are computed
 * and their difference is reported, so a mismatch shows up instead of being averaged away. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "solid.h"

typedef struct TopOptSettings {
    double volume_fraction; /* target volume / design volume, (0, 1] */
    double penalty;         /* SIMP p (default 3) */
    double filter_radius;   /* m; 0 or less: 1.5 element widths. A radius below one element width filters nothing */
    double move_limit;      /* default 0.2 */
    double x_min;           /* lower bound on the density (default 1e-3) */
    double e_min_ratio;     /* E_min / E_0 (default 1e-9) */
    int max_iter;           /* default 100 */
    double change_tol;      /* stop when the largest density change is at or below this (default 0.01) */
    int passive_layers;     /* 0 none; 1 the elements at supports and loads stay solid; 2 one layer more */
    SolveProgressFn progress; /* optional: called once per iteration with the iteration fraction */
    void *ctx;
} TopOptSettings;

typedef struct TopOptStep {
    int iter;
    double compliance;      /* J (f^T u) */
    double volume_fraction; /* of the design volume */
    double change;          /* max density change of this update */
} TopOptStep;

typedef struct TopOptResult {
    double *density;         /* nelems, x_min .. 1 */
    unsigned char *passive;  /* nelems: 1 where the density was held at 1 */
    TopOptStep *history;     /* `iterations` entries */
    int iterations;          /* updates performed */
    int solves;              /* linear solves (iterations + the final one) */
    int passive_elements;
    double compliance;          /* of the final density field */
    double compliance_initial;  /* of the starting uniform field */
    double volume_fraction;     /* reached */
    double change;              /* last density change */
    double energy_mismatch;     /* largest |f^T u - 2 U| / |f^T u| over the run */
    double seconds;
    char stop_reason[64];    /* "converged" or "iteration limit" */
} TopOptResult;

void topopt_settings_default(TopOptSettings *s);
bool topopt_run(const HexModel *m, const SolidLoads *loads, const SolidOptions *opt, const TopOptSettings *s,
                TopOptResult *r, char *err, size_t errlen);
void topopt_result_free(TopOptResult *r);
