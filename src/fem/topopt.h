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

/* One design of the run and its analysis, handed to TopOptSettings.snapshot (contract amendment 3, 2026-10-22). Every
 * pointer is read-only and valid during the call only; the optimiser goes on exactly as it would without the call. */
typedef struct TopOptSnapshot {
    int iter;                      /* 0: the starting field; k: the field after k updates */
    int final;                     /* 1 for the field the run returns (always the last snapshot) */
    int nelems;
    const double *density;         /* nelems */
    const unsigned char *passive;  /* nelems: 1 where the density is held at 1 */
    double compliance;             /* f^T u of this field (J) */
    double volume_fraction;        /* of this field, of the design volume */
    double target_volume_fraction; /* what the update into this field aimed at (the starting field: its own volume) */
    double change;                 /* the largest density change of the update into this field (0 for the start) */
    const SolidResult *solve;      /* this field's solve: displacements, Gauss-point strains and stresses, statistics */
} TopOptSnapshot;
typedef void (*TopOptSnapshotFn)(void *ctx, const TopOptSnapshot *snap);

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
    /* Additions of 2026-10-22 (contract amendment 3), trailing so that existing initialisers stay valid. Each is off
     * when zero, and with all of them off a run is the one above, number for number. */
    int snapshot_every;     /* k > 0: call `snapshot` with the starting field, every k-th field and the final one */
    TopOptSnapshotFn snapshot;
    void *snapshot_ctx;
    int warm_start;         /* 1: every solve after the first starts the iterative solver from the previous
                             * displacement (SolidOptions.u_guess); a direct solve is not affected */
    const unsigned char *passive_solid; /* nelems, optional: 1 where the element is held solid (bosses, lugs), in
                             * addition to the shell that passive_layers marks */
    double volume_start;    /* in (volume_fraction, 1]: a volume continuation. The run starts from the uniform field of
                             * this volume (1: the whole design space solid), and update k (1, 2, ...) aims at
                             * max(volume_fraction, volume_start - k volume_step); the change criterion applies only
                             * once the aim has reached volume_fraction. 0: off */
    double volume_step;     /* the fall of the aim per update, as a volume fraction (with volume_start) */
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
    /* additions of 2026-10-22 */
    long long solver_iterations; /* iterations of the iterative solver over all solves (0 when every solve was direct) */
    double solve_seconds;        /* time spent in solid_solve */
    int target_reached_at;       /* the first update that aimed at volume_fraction (1 without a continuation) */
} TopOptResult;

void topopt_settings_default(TopOptSettings *s);
bool topopt_run(const HexModel *m, const SolidLoads *loads, const SolidOptions *opt, const TopOptSettings *s,
                TopOptResult *r, char *err, size_t errlen);
void topopt_result_free(TopOptResult *r);
