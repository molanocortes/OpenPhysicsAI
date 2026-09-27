/* solid.h - static small-strain linear elasticity on hexahedral meshes (and, through tet.h, tetrahedral ones)
 *
 * Unknowns are nodal displacements (m). Constraints are prescribed displacement components; they are eliminated from
 * the system (no penalty springs, no artificial stiffness). Before solving, every face-connected region is checked
 * for unconstrained rigid-body motion; a region that is free, or attached to the rest only through edges or
 * vertices, is reported instead of being solved. Results: displacements, reaction forces, Gauss-point strain and
 * stress, nodal averages of stress, and equilibrium and energy checks. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../threads.h"
#include "hex8.h"
#include "sparse.h"
#include "tet.h"

typedef struct SolidMaterial {
    double E;       /* Pa */
    double nu;
    double density; /* kg/m^3 (gravity loads) */
} SolidMaterial;

typedef struct HexModel {
    int nnodes, nelems;
    const double *xyz;     /* 3*nnodes, m */
    const int *conn;       /* 8*nelems */
    const int *elem_mat;   /* material index per element (NULL: material 0) */
    int nmat;
    const SolidMaterial *mat;
    Hex8Formulation formulation;
    /* nelems: stiffness multiplier per element (temperature-dependent modulus with a temperature-independent Poisson
     * ratio: D(T) = E(T)/E * D); NULL = 1 */
    const double *elem_scale;
    /* Hanging nodes of an adaptive (2:1) mesh: node hang_node[i] follows hang_nmaster[i] (2 or 4) masters,
     * u = sum w u_master, in all three components. A master may itself be hanging; chains are resolved. The node carries
     * no equation; element matrices and forces touching it go to its masters with the same weights, and its value is
     * interpolated after the solve. 0 hanging nodes (the default) is a conforming mesh, solved exactly as before.
     * Prescribed non-zero displacements (SolidLoads.fixed_value) are refused together with hanging nodes. */
    int nhang;
    const int *hang_node;      /* nhang */
    const int *hang_nmaster;   /* nhang */
    const int *hang_master;    /* 4 * nhang */
    const double *hang_weight; /* 4 * nhang */
    /* nmat * 36, optional: a full 6x6 elasticity matrix (Pa, Voigt, engineering shear) per material, row-major. A
     * material whose first entry is 0 keeps the isotropic matrix of its E and nu; NULL (the default) keeps them all. */
    const double *mat_D;
    /* Trailing on purpose (positional initialisers elsewhere stay valid). Element type (tet.h): SOLID_ELEM_HEX8 (0, the voxel meshes), SOLID_ELEM_TET4 or SOLID_ELEM_TET10. A tetrahedral
     * model has 4 or 10 nodes per element in conn and is solved by tet.c; formulation does not apply to it. */
    int elem_type;
} HexModel;

typedef struct SolidLoads {
    const unsigned char *fixed; /* 3*nnodes: 1 when the displacement component is prescribed */
    const double *fixed_value;  /* 3*nnodes: prescribed value (m), NULL = zero */
    const double *nodal_force;  /* 3*nnodes: applied nodal forces (N), NULL = none */
    const double *eps0;         /* 6*nelems: stress-free initial strain (thermal, eigenstrain), NULL = none */
    double gravity[3];          /* m/s^2, multiplied by the material density */
} SolidLoads;

typedef enum { SOLID_SOLVER_AUTO = 0, SOLID_SOLVER_DIRECT, SOLID_SOLVER_PCG } SolidSolver;

typedef struct SolidOptions {
    SolidSolver solver;
    double pcg_tol;          /* relative residual (default 1e-10) */
    int pcg_max_iter;        /* default 20000 */
    int64_t max_factor_nnz;  /* direct solver memory guard (default 60 million entries, ~0.7 GB) */
    ThreadPool *pool;
    SolveProgressFn progress;
    void *ctx;
} SolidOptions;

enum { SOLID_MAX_ISSUES = 16 };

typedef struct ConstraintIssue {
    int region;          /* face-connected region id */
    int elements;
    double centroid[3];  /* m */
    int free_modes;      /* number of unconstrained rigid-body modes */
    double modes[6][6];  /* free mode vectors: translation x y z, rotation about x y z (through the centroid) */
    int weak_links;      /* nodes shared with other regions only through edges or vertices */
} ConstraintIssue;

typedef struct ConstraintReport {
    int regions;
    int nissues;
    ConstraintIssue issue[SOLID_MAX_ISSUES];
} ConstraintReport;

/* face-connected regions and their unconstrained rigid-body modes; returns true when every region is fixed */
bool solid_check_constraints(const HexModel *m, const unsigned char *fixed, ConstraintReport *rep);
void constraint_issue_text(const ConstraintIssue *is, char *out, size_t cap);

typedef struct SolidResult {
    int neq;
    double *u;            /* 3*nnodes */
    double *reaction;     /* 3*nnodes, nonzero only at prescribed components */
    double *gp_strain;    /* 48*nelems (8 Gauss points x 6) */
    double *gp_stress;    /* 48*nelems */
    double *node_stress;  /* 6*nnodes, average of the element extrapolations sharing the node */
    SolveStats stats;
    double strain_energy;       /* J */
    double external_work;       /* J: applied forces and prescribed displacements times reactions */
    double equilibrium_error;   /* ||residual at free equations|| / (||applied|| + ||reactions||) */
    double load_total[3], reaction_total[3];
    double min_detJ;
    int colors;
    int stiffness_cache_hits;
    int gp_per_elem; /* quadrature points per element in gp_strain / gp_stress: 1 (TET4), 4 (TET10); 0 means 8 (HEX8) */
} SolidResult;

bool solid_solve(const HexModel *m, const SolidLoads *loads, const SolidOptions *opt, SolidResult *res, char *err, size_t errlen);
void solid_result_free(SolidResult *r);

/* the tetrahedral path (tet.c), reached through solid_solve and solid_check_constraints */
bool tet_solid_solve(const HexModel *m, const SolidLoads *loads, const SolidOptions *opt, SolidResult *res, char *err, size_t errlen);
bool tet_check_constraints(const HexModel *m, const unsigned char *fixed, ConstraintReport *rep);
