/* modal.h - structural dynamics of a part: consistent mass, natural modes of a supported or free-free part, modal transient
 * response with exact integration of piecewise-linear loads, direct Newmark integration and frequency response
 *
 * Model. The hex8 mesh, elastic constants and material axes of ortho.h, a density per element and optional prescribed
 * (zero) displacement components. A free-free part (no fixed components) has six rigid-body modes; they are found, counted
 * and reported, never assumed.
 *
 * Modes. Shift-invert subspace iteration: (K - sigma M) Y = M X, Rayleigh-Ritz on (K, M) in span(Y), until the wanted
 * eigenvalues stop changing. sigma = 0 with supports; a small negative shift free-free (K - sigma M is then positive
 * definite). Modes are mass-normalised. Checks: residual ||K phi - w^2 M phi|| relative to ||K phi|| + w^2 ||M phi||,
 * M-orthogonality, and effective modal masses (translations) and inertias (rotations about the mass centre).
 *
 * Transient. With modal superposition each elastic mode obeys q'' + 2 zeta w q' + w^2 q = phi^T f(t). Loads are
 * sums of fixed spatial patterns times histories that vary linearly between samples; each interval is integrated exactly
 * (Nigam-Jennings recurrence), so the only approximations are the truncation to the computed modes and the piecewise-
 * linear load. Rigid-body modes are left out: a self-equilibrated load (joint and contact loads plus d'Alembert forces of
 * the rigid motion) does not excite them, and the share of the load that would is reported. The direct alternative is
 * Newmark's average-acceleration method on the full system with Rayleigh damping C = alpha M + beta K (second order,
 * unconditionally stable, no numerical damping). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "ortho.h"

typedef struct StructDyn {
    const OrthoModel *om;          /* geometry, constants, axes, formulation */
    const double *density;         /* nelems, kg/m^3 */
    const unsigned char *fixed;    /* 3 * nnodes, NULL = free-free */
} StructDyn;

typedef struct ModalOptions {
    int nmodes;       /* modes wanted, rigid-body modes included (a free-free part: 6 + the elastic modes) */
    int subspace;     /* vectors iterated (0 = max(2 nmodes, nmodes + 8)) */
    double tol;       /* relative eigenvalue change that ends the iteration (default 1e-10) */
    int max_iter;     /* default 300 */
} ModalOptions;

typedef struct ModalResult {
    int nnodes, neq, nmodes, nrigid, iterations;
    bool converged;
    int *eq;              /* 3 * nnodes: equation numbers, -1 for fixed components and unused nodes */
    double *omega2;       /* nmodes, ascending (rad/s)^2; rigid-body modes are ~0 */
    double *phi;          /* nmodes * neq, mass-normalised */
    double *residual;     /* nmodes: elastic modes ||K phi - w^2 M phi|| / (||K phi|| + w^2 ||M phi||); rigid-body modes of a free part: the
                           * M-norm distance from the analytic rigid-body subspace (translations, rotations about the mass centre) */
    double orthogonality; /* max |phi_i^T M phi_j - delta_ij| */
    double mass, com[3];  /* from the mass matrix */
    double inertia[3];    /* rigid-body moments of inertia about x y z through the mass centre (kg m^2), free components only */
    double *eff;          /* nmodes * 6: effective mass (kg) along x y z, effective inertia (kg m^2) about x y z through the mass centre */
    double shift;
    SolveStats factor;
    CsrMatrix K, M;       /* assembled matrices (equation space), kept for response calculations */
    CholFactor Kf;        /* factor of K when the part is supported (static residual flexibility); empty free-free */
    bool have_Kf;
} ModalResult;

void hex8_consistent_mass(const double X[8][3], double rho, double Me[576]);
/* stiffness and consistent mass over the free components into r->K, r->M and r->eq (the other fields stay empty) */
bool modal_assemble(const StructDyn *s, ModalResult *r, char *err, size_t errlen);
bool modal_solve(const StructDyn *s, const ModalOptions *opt, ModalResult *r, char *err, size_t errlen);
void modal_result_free(ModalResult *r);

typedef struct ModalLoad {
    int npatterns;
    const double *patterns;   /* npatterns * neq: spatial load patterns in equation space (N) */
    int nsteps;               /* intervals */
    double dt;                /* s */
    const double *histories;  /* npatterns * (nsteps + 1): multipliers at the samples, linear in between */
} ModalLoad;

/* q, qd: nmodes * (nsteps + 1) (row = mode); zeta: nmodes damping ratios (< 1). rigid_share (optional) receives the largest
 * ratio |sum over rigid modes of (phi^T f)^2|^(1/2) / |f| over the samples (0 for self-equilibrated loads). */
bool modal_transient(const ModalResult *r, const ModalLoad *L, const double *zeta, double *q, double *qd, double *rigid_share, char *err, size_t errlen);
/* displacement history in equation space from modal coordinates: u = sum phi_i q_i (elastic modes) */
void modal_expand(const ModalResult *r, const double *q, int nsteps, int step, double *u);

/* u, v: (nsteps + 1) * neq; starts at rest. xyz is needed for the ordering of the factorisation. */
bool newmark_transient(const ModalResult *r, const double *xyz, const ModalLoad *L, double alpha, double beta, double *u, double *v, char *err, size_t errlen);

/* complex displacement amplitude at equation `out` for a unit-amplitude harmonic force pattern F (neq) at each frequency (Hz):
 * sum over elastic modes, plus the static residual flexibility of the truncated modes when the part is supported */
bool modal_frf(const ModalResult *r, const double *F, int out, const double *zeta, const double *freq_hz, int nfreq, bool residual, double *re, double *im,
               char *err, size_t errlen);
