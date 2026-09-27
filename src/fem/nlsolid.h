/* nlsolid.h - static equilibrium at large deformation: Newton on the total-Lagrangian hex8 of hex8_nl.h.
 *
 * The linear solver of solid.h is untouched and is still what every existing analysis uses. This one exists for the
 * cases the linear path cannot answer: deflections that are not small, buckling, and finite-strain plasticity
 * (docs/contracts/dynamics.md, step 1).
 *
 * The load (nodal forces, gravity and prescribed displacements) is applied over `load_steps` increments. Each
 * increment runs Newton with the consistent tangent; an increment that does not converge, or that inverts an element,
 * is halved and retried down to `min_step`. The result reports the increments, the iterations and the cut-backs, so a
 * number that took a hundred cut-backs is never mistaken for one that took none.
 *
 * Buckling: with `stop_when_indefinite` the solve stops at the first increment whose tangent is no longer positive
 * definite (the factorisation fails), and reports the load factor reached. That factor, times the applied load, is
 * the critical load to within the increment size. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "hex8_nl.h"
#include "solid.h"

typedef struct NlModel {
    int nnodes, nelems;
    const double *xyz;   /* 3 * nnodes, m (the undeformed shape) */
    const int *conn;     /* 8 * nelems */
    const int *elem_mat; /* per element, NULL: material 0 */
    int nmat;
    const Hex8NlMaterial *mat;
    const double *density; /* per material, kg/m^3, for gravity; NULL: no gravity */
} NlModel;

typedef struct NlLoads {
    const unsigned char *fixed; /* 3 * nnodes: 1 where the displacement is prescribed */
    const double *fixed_value;  /* 3 * nnodes, m, reached at the end of the last increment; NULL: zero */
    const double *nodal_force;  /* 3 * nnodes, N, reached at the end of the last increment; NULL: none */
    double gravity[3];          /* m/s^2 */
} NlLoads;

typedef struct NlOptions {
    int load_steps;            /* increments of the full load (default 10) */
    int max_newton;            /* iterations per increment (default 25) */
    double tol;                /* relative residual (default 1e-8) */
    double min_step;           /* smallest increment as a fraction of the full load (default 1e-4) */
    bool stop_when_indefinite; /* buckling: stop at the first tangent that is not positive definite */
    ThreadPool *pool;
} NlOptions;

typedef struct NlResult {
    double *u;           /* 3 * nnodes */
    double *reaction;    /* 3 * nnodes, nonzero at prescribed components */
    Hex8NlState *state;  /* 8 * nelems, the plastic state after the solve */
    int increments, iterations, cutbacks;
    double load_factor;  /* 1 on a complete solve; less when it stopped at an indefinite tangent */
    double residual;     /* the last relative residual */
    double min_det_f;    /* the smallest deformation Jacobian reached */
    bool tangent_indefinite;
    int neq;
} NlResult;

bool nlsolid_solve(const NlModel *m, const NlLoads *loads, const NlOptions *opt, NlResult *res, char *err, size_t errlen);
void nlsolid_result_free(NlResult *r);
