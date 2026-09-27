/* ortho.h - orthotropic linear elasticity for printed parts
 *
 * Material model. A printed part is represented as a homogeneous orthotropic solid whose material axes follow the print:
 * axis 3 is the build direction (normal to the layers), axes 1 and 2 lie in the layer plane (axis 1 along the raster
 * reference direction, rotated by the raster angle about the build direction). A +/-45 degree raster is usually given
 * as transversely isotropic constants (E1 = E2, G12 = E1 / (2 (1 + nu12))), for which the raster angle has no effect.
 * The constants are the user's data with their provenance; nothing here supplies default printed-material values.
 *
 * Conventions (those of the shared hex8 element, fem/hex8.h): Voigt order [11, 22, 33, 12, 23, 31] with engineering
 * shear strains. Major Poisson ratios: nu_ij = -eps_j / eps_i under uniaxial stress along i, so nu_ij / E_i = nu_ji / E_j.
 * Material axes are given as a rotation R whose columns are the material axes in global coordinates.
 *
 * Solver. The shared static solver (fem/solid.h) is isotropic; this module assembles the shared hex8 element with a
 * per-element constitutive matrix D = T^T C T and solves with the shared sparse Cholesky or PCG. Results carry stresses
 * in global and in material axes, reactions, energy and equilibrium checks.
 *
 * Strength. Failure criteria are named, never implied: maximum stress (governing component reported), Tsai-Wu (3D, with
 * stated interaction coefficients) and Tsai-Hill (3D Hill form with tension or compression strengths chosen by the sign
 * of each normal stress). Each gives a failure index (1 = on the envelope) and a strength ratio (the factor on the stress
 * state that reaches the envelope). Strength values of printed parts depend on the process and must come with their
 * source; the interlayer (axis 3) tensile strength usually governs. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../fem/hex8.h"
#include "../fem/solid.h"
#include "../fem/sparse.h"

typedef struct OrthoConstants {
    double E[3];              /* Pa, along material axes 1, 2, 3 */
    double nu12, nu13, nu23;  /* major Poisson ratios */
    double G12, G23, G13;     /* Pa */
} OrthoConstants;

/* compliance S and stiffness C (Voigt, engineering shear); false with a reason when the constants are not positive
 * definite (E and G must be positive and the normal block of S positive definite, e.g. nu12^2 < E1 / E2) */
bool ortho_compliance(const OrthoConstants *k, double S[6][6], char *why, size_t n);
bool ortho_stiffness(const OrthoConstants *k, double C[6][6], char *why, size_t n);
/* isotropic constants in orthotropic form (E, nu; G = E / (2 (1 + nu))) */
void ortho_isotropic(double E, double nu, OrthoConstants *k);

/* strain transformation T (eps_material = T eps_global) for material axes R (columns = axes in global coordinates) */
void voigt_strain_transform(const double R[9], double T[6][6]);
/* global constitutive matrix D = T^T C T */
void ortho_global_D(const double C[6][6], const double R[9], double D[6][6]);
/* stress components in material axes: sigma_m = R^T sigma_g R */
void voigt_stress_to_material(const double R[9], const double sig_g[6], double sig_m[6]);
/* material axes of a print: axis 3 = build direction b (unit), axis 1 = the in-plane reference direction x_ref (projected
 * into the layer plane) rotated by raster_angle about b; false when x_ref is parallel to b */
bool ortho_print_axes(const double build_dir[3], const double x_ref[3], double raster_angle, double R[9]);

/* ------------------------------------------------------------------------------------------------ strength */

typedef enum { STRENGTH_MAX_STRESS = 0, STRENGTH_TSAI_WU, STRENGTH_TSAI_HILL, STRENGTH_CRITERIA } StrengthCriterion;
const char *strength_criterion_name(StrengthCriterion c);
int strength_criterion_from_name(const char *s);

typedef struct OrthoStrength {
    double Xt, Xc, Yt, Yc, Zt, Zc; /* Pa, positive magnitudes along axes 1, 2, 3 (Z = build direction: interlayer) */
    double S12, S23, S31;          /* Pa, shear strengths in the material planes */
    double f12, f13, f23;          /* Tsai-Wu normalised interaction coefficients F_ij / sqrt(F_ii F_jj) */
} OrthoStrength;

/* false with a reason for non-positive strengths or a Tsai-Wu quadratic form that is not positive semidefinite */
bool strength_valid(const OrthoStrength *s, StrengthCriterion c, char *why, size_t n);

typedef struct StrengthResult {
    double index;   /* failure index: max stress = largest stress/strength ratio; quadratic criteria = 1 / strength ratio */
    double ratio;   /* strength ratio: factor on the stress state that reaches the envelope (INFINITY when unloaded) */
    int mode;       /* governing term: 0 1t, 1 1c, 2 2t, 3 2c, 4 3t, 5 3c, 6 s12, 7 s23, 8 s31 (largest contribution) */
} StrengthResult;

void strength_evaluate(StrengthCriterion c, const OrthoStrength *s, const double sig_m[6], StrengthResult *r);
const char *strength_mode_name(int mode); /* e.g. "3t (interlayer tension)" */

/* ------------------------------------------------------------------------------------------------ solver */

typedef struct OrthoModel {
    int nnodes, nelems;
    const double *xyz;           /* 3 * nnodes, m */
    const int *conn;             /* 8 * nelems */
    int nmat;
    const OrthoConstants *mat;
    const int *elem_mat;         /* nelems, NULL = material 0 */
    const double *axes;          /* 9 per element (elem_axes) or 9 for all (axes_count 1); NULL = global axes */
    int axes_count;              /* 1 or nelems */
    Hex8Formulation formulation;
} OrthoModel;

typedef struct OrthoLoads {
    const unsigned char *fixed;  /* 3 * nnodes */
    const double *fixed_value;   /* 3 * nnodes, NULL = zero */
    const double *nodal_force;   /* 3 * nnodes, NULL = none */
} OrthoLoads;

typedef struct OrthoResult {
    int neq;
    double *u;                   /* 3 * nnodes */
    double *reaction;            /* 3 * nnodes (nonzero at prescribed components) */
    double *gp_stress;           /* 48 * nelems, global axes */
    double *gp_stress_mat;       /* 48 * nelems, material axes */
    double *gp_strain;           /* 48 * nelems, global axes, engineering shear */
    double *node_stress;         /* 6 * nnodes, global axes, average of element extrapolations */
    double strain_energy;        /* J */
    double external_work;        /* J */
    double equilibrium_error;    /* ||residual at free equations|| / (||applied|| + ||reactions||) */
    double load_total[3], reaction_total[3];
    double min_detJ;
    int stiffness_cache_hits;
    SolveStats stats;
} OrthoResult;

bool ortho_solve(const OrthoModel *m, const OrthoLoads *L, const SolidOptions *opt, OrthoResult *res, char *err, size_t errlen);
/* nested-dissection ordering over the nodes and sparse Cholesky of an assembled matrix K (equations eq, 3 per node); false
 * with a reason, and *too_big when the factor would exceed max_factor_nnz (0 = 60 million entries) */
bool ortho_factor(int nnodes, const double *xyz, const CsrMatrix *K, const int *eq, int neq, int64_t max_factor_nnz, SolveProgressFn progress, void *ctx,
                  CholFactor *F, SolveStats *st, bool *too_big, char *err, size_t errlen);
/* per-element constitutive matrix index and matrix of a model: D for element e (global axes) */
void ortho_element_D(const OrthoModel *m, int e, double D[6][6]);
/* stiffness over the free components (eq: 3 * nnodes equation numbers, -1 fixed or unused) and its sparse Cholesky factor */
bool ortho_assemble_factor(const OrthoModel *m, const unsigned char *fixed, int *eq, int *neq, CholFactor *F, SolveStats *st, char *err, size_t errlen);
/* Gauss-point stresses (48 per element, global axes) of a nodal displacement field (3 * nnodes) */
void ortho_gauss_stress(const OrthoModel *m, const double *u, double *gp_stress);
void ortho_result_free(OrthoResult *r);

/* failure index field at the Gauss points (8 per element) and the governing point */
typedef struct StrengthSummary {
    double max_index, min_ratio;
    int elem, gp, mode;
    double sig_m[6];             /* material-axes stress at the governing point */
} StrengthSummary;
void ortho_strength_field(const OrthoResult *r, int nelems, StrengthCriterion c, const OrthoStrength *s, double *gp_index, StrengthSummary *sum);
