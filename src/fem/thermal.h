/* thermal.h - transient heat conduction on hexahedral (hex8) meshes
 *
 * Solves the enthalpy form of the energy balance
 *     d(rho h)/dt = div(K grad T) + q,      q_flux = -K grad T
 * on the active elements of a mesh with
 *   - prescribed nodal temperatures,
 *   - boundary faces with heat flux, convection h (T - Ta) or radiation eps sigma (T^4 - Ta^4),
 *   - volumetric sources per element and a moving Gaussian source (power, radius, absorption depth, timed path).
 * Time integration: theta method (theta = 1 backward Euler, 0.5 Crank-Nicolson; both unconditionally stable for the
 * linear problem), lumped (row-sum) or consistent capacity; dt <= 0 solves the steady state.
 *
 * Constitutive evaluation (THERMAL_PROP_QUADRATURE, the default): the conductivity tensor is evaluated at every Gauss
 * point from the temperature interpolated there, so a temperature-dependent or anisotropic conductivity is integrated
 * rather than averaged. THERMAL_PROP_ELEMENT reproduces the older element-mean evaluation and is kept for comparison.
 * An anisotropic material gives three principal conductivities and the rows of the rotation from the material frame to
 * the global frame; K_global = R^T diag(k1, k2, k3) R is formed once per Gauss point.
 *
 * Capacity: the volumetric enthalpy
 *     H(T) = integral_Tref^T rho(s) cp(s) ds + rho_ref L f_liq(T)
 * carries both sensible heat and, when a latent heat and a solidus/liquidus pair are given, the latent heat of melting
 * through the liquid fraction f_liq (linear in T between solidus and liquidus; the equilibrium assumption). The step
 * uses the secant apparent capacity c_app = (H(T1) - H(T0)) / (T1 - T0), which makes the discrete stored energy of the
 * step equal the enthalpy difference exactly, so latent heat is neither lost nor counted twice. Below a temperature
 * change of THERMAL_DH_MIN the derivative dH/dT at the midpoint is used instead.
 *
 * Radiation is linearised per face with Newton's method about the face mean temperature; every nonlinearity is iterated
 * to convergence each step. The linear systems are solved for the temperature correction of each iteration, so solver
 * tolerances apply to the change within the step.
 *
 * Every step reports its discrete energy balance - the change of stored energy against the heat delivered by sources,
 * boundary faces, interfaces and prescribed-temperature nodes - and, independently, the physical enthalpy change
 * obtained by integrating H over the elements. The two agree to round-off with the secant capacity and diverge when the
 * capacity model is inconsistent, which is what makes the balance evidence of energy conservation rather than of a
 * small linear-system residual.
 *
 * Interfaces: pairs of coincident nodes on two sides of a cut may exchange heat through a finite conductance
 * (thermal contact resistance, thin layers, gap conductance). Perfect contact is the shared node and needs no
 * interface. Nodes that belong to no active element keep their temperature (element activation for additive
 * manufacturing). Temperatures are in K, lengths in m, all other quantities SI. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../threads.h"

#define THERMAL_SIGMA 5.670374419e-8 /* Stefan-Boltzmann constant, W/(m^2 K^4) */
#define THERMAL_DH_MIN 1e-9          /* K: below this temperature change the secant capacity falls back to dH/dT */

/* piecewise-linear table in K, constant outside its range; n = 1 is a constant */
typedef struct ThermalTable {
    int n;
    const double *t, *v;
} ThermalTable;

double thermal_table_eval(const ThermalTable *t, double T);
/* exact integral of the table from a to b (enthalpy from cp, thermal strain from the instantaneous expansion coefficient) */
double thermal_table_integral(const ThermalTable *t, double a, double b);

/* Zero-initialise before filling in: the optional fields below must start at zero for a material to be isotropic
 * and free of phase change (calloc, = {0}, or an initialiser list; assigning only .k/.cp/.rho to an uninitialised
 * struct leaves the rest as garbage). */
typedef struct ThermalMaterial {
    ThermalTable k;   /* W/(m K): the isotropic conductivity, or the first principal value when anisotropic */
    ThermalTable cp;  /* J/(kg K) */
    ThermalTable rho; /* kg/m^3 */
    /* --- optional, zero means "not used" (older positional initialisers stay valid) --- */
    ThermalTable k2, k3; /* W/(m K): principal values 2 and 3; n == 0 keeps the material isotropic */
    double axes[9];      /* rows = the material principal directions in global coordinates; all zero = identity */
    double latent_heat;  /* J/kg of melting; 0 = no phase change */
    double solidus;      /* K, required with a latent heat */
    double liquidus;     /* K, must exceed the solidus */
} ThermalMaterial;

/* volumetric enthalpy H(T) (J/m^3) measured from THERMAL_H_REF, and its derivative dH/dT (J/(m^3 K)) */
#define THERMAL_H_REF 0.0
double thermal_enthalpy(const ThermalMaterial *m, double T);
double thermal_capacity(const ThermalMaterial *m, double T);
/* secant capacity (H(T1) - H(T0)) / (T1 - T0), falling back to dH/dT at the midpoint for a vanishing change */
double thermal_secant_capacity(const ThermalMaterial *m, double T0, double T1);
/* liquid fraction in 0..1 (0 when the material has no latent heat) */
double thermal_liquid_fraction(const ThermalMaterial *m, double T);
/* conductivity tensor in global coordinates at temperature T, row-major 3x3 */
void thermal_conductivity(const ThermalMaterial *m, double T, double K[9]);
bool thermal_is_anisotropic(const ThermalMaterial *m);

/* THERMAL_OPEN: an open boundary of an advected region. Where the velocity leaves (u.n > 0) nothing is imposed: no
 * diffusive flux, and the advected enthalpy leaves with the local temperature (outflow). Where it enters (u.n < 0) the
 * fluid brings the face's ambient temperature through the weak inflow term rho cp |u.n| (T_ambient - T), which makes an
 * inlet and a backflow region of an outlet the same condition. */
typedef enum { THERMAL_FLUX = 0, THERMAL_CONVECTION = 1, THERMAL_RADIATION = 2, THERMAL_OPEN = 3 } ThermalFaceKind;

typedef struct ThermalFace {
    int elem;
    unsigned char face; /* hex8 local face 0..5 */
    unsigned char kind; /* ThermalFaceKind */
    double value;       /* flux W/m^2 into the body | h W/(m^2 K) | emissivity | unused (open) */
    double ambient;     /* K, for convection and radiation; the temperature of entering fluid (open) */
} ThermalFace;

/* Gaussian surface source with exponential absorption:
 *   q = 2 P / (pi r^2 d) exp(-2 rho^2 / r^2) exp(-depth / d)
 * where rho is the in-plane distance to the path point and depth the distance below it (-z). It integrates to P over a
 * half-space. The path point moves linearly between timed waypoints and the source is off outside their time range. */
typedef struct ThermalMovingSource {
    double power;  /* W absorbed */
    double radius; /* m, 1/e^2 radius */
    double depth;  /* m, absorption depth */
    int npoints;
    const double *path; /* 4 per point: x, y, z of the surface point (m), time (s), times non-decreasing */
} ThermalMovingSource;

/* finite-conductance interface between two coincident nodes on opposite sides of a cut. The pair exchanges
 *   Q = conductance * area * (T_b - T_a)     [W, positive into node a]
 * so `conductance` is W/(m^2 K) and `area` the nodal share of the interface area (m^2). A thin layer of thickness d
 * and conductivity k_l is the conductance k_l / d; a contact resistance R'' is 1 / R''. */
typedef struct ThermalInterfaceNode {
    int node_a, node_b;
    double conductance; /* W/(m^2 K) */
    double area;        /* m^2 */
} ThermalInterfaceNode;

typedef struct ThermalModel {
    int nnodes, nelems;
    const double *xyz;           /* 3*nnodes (m) */
    const int *conn;             /* 8*nelems */
    const int *elem_mat;         /* nelems, NULL: material 0 */
    int nmat;
    const ThermalMaterial *mat;
    const unsigned char *active; /* nelems, NULL: all active */
    const unsigned char *fixed;  /* nnodes, prescribed temperature, NULL: none */
    const double *fixed_T;       /* nnodes (K) */
    const double *elem_source;   /* nelems, W/m^3 held over the step, NULL: none */
    int nfaces;
    const ThermalFace *faces;
    const ThermalMovingSource *moving; /* NULL: none */
    int ninterface;
    const ThermalInterfaceNode *interfaces; /* NULL: none */
    /* optional labels for energy accounting (all NULL: none). Per step the solver reports, in joules,
     *   element groups (e.g. bodies):        stored energy and source energy
     *   face groups (e.g. conditions):       heat into the body through those faces
     *   node groups (e.g. conditions):       heat supplied through those prescribed-temperature nodes
     *   interface groups (e.g. interfaces):  heat from side b into side a
     * so that a body's own balance (stored - sources - boundary - prescribed) can be compared with the heat its
     * interfaces report: two independent computations of the same energy. Group ids lie in 0..THERMAL_MAX_GROUPS-1,
     * or -1 for "not counted". */
    const int *elem_group, *face_group, *node_group, *interface_group;
    /* optional heat loads applied directly at nodes (W, held over the step; positive into the body). Used by partitioned
     * coupling to deliver the heat a neighbouring participant computed at shared interface nodes. NULL: none. */
    const double *node_power;
    /* optional advection by a prescribed velocity field (an incompressible fluid with constant properties):
     *     rho cp (dT/dt + u . grad T) = div(k grad T) + q         (convective form)
     * velocity: 3*nnodes (m/s), interpolated trilinearly; advect: nelems, non-zero for elements whose energy is
     * transported (the fluid), NULL: every element when a velocity is given. Advected elements need a material with
     * constant density and specific heat and no latent heat. The system becomes nonsymmetric and is solved with
     * BiCGSTAB/ILU(0) instead of conjugate gradients. The velocity may change between steps. */
    const double *velocity;
    const unsigned char *advect;
} ThermalModel;

enum { THERMAL_MAX_GROUPS = 32 };

typedef enum {
    THERMAL_PROP_QUADRATURE = 0, /* conductivity at every Gauss point (default) */
    THERMAL_PROP_ELEMENT = 1     /* one conductivity per element at its mean temperature (for comparison) */
} ThermalPropertyEval;

typedef struct ThermalOptions {
    double theta;             /* 0.5..1, default 1 (backward Euler) */
    bool consistent_capacity; /* default: lumped */
    int max_picard;           /* default 50 */
    double picard_tol;        /* K: largest temperature change between iterations, default 1e-6 */
    double pcg_tol;           /* relative residual, default 1e-12 */
    int pcg_max_iter;         /* default 20000 */
    bool raw_source;          /* false (default): scale the moving source so the mesh absorbs exactly its power */
    ThermalPropertyEval property_eval; /* default THERMAL_PROP_QUADRATURE */
    bool element_capacity;    /* true: rho cp at the element mean temperature instead of the secant enthalpy capacity
                               * (the pre-enthalpy behaviour; latent heat is then ignored). Default false. */
    /* advection: THERMAL_STAB_SUPG (default) streamline-upwind Petrov-Galerkin with
     *     tau = [ (2 |u| / h_u)^2 + 9 (4 alpha / h_u^2)^2 ]^(-1/2),   h_u = 2 |u| / sum_a |u . grad N_a|
     * evaluated at the element centre; tau deliberately does not depend on the time step, so the spatial discretisation
     * is the same for every step size and step doubling measures temporal error only. The weighting (N_a + tau u.grad N_a)
     * is applied to the capacity, advection and source terms (consistent SUPG); the diffusion term's second derivatives
     * vanish on parallelepiped hex8 elements with constant isotropic conductivity and are dropped otherwise.
     * THERMAL_STAB_NONE: plain Galerkin (oscillates when the cell Peclet number u h / (2 alpha) exceeds 1; for comparison). */
    int advection_stabilization;
    ThreadPool *pool;
} ThermalOptions;

enum { THERMAL_STAB_SUPG = 0, THERMAL_STAB_NONE = 1 };

/* why a step failed: decides whether a smaller step can help */
typedef enum {
    THERMAL_FAIL_NONE = 0,
    THERMAL_FAIL_INPUT,     /* malformed model or arguments: no step size can fix it */
    THERMAL_FAIL_NONLINEAR, /* the Picard iteration did not converge: usually fixed by a smaller step */
    THERMAL_FAIL_LINEAR,    /* conjugate gradients broke down or did not converge */
    THERMAL_FAIL_STATE,     /* the iterate became non-finite or fell below 0 K */
    THERMAL_FAIL_RESOURCE   /* out of memory */
} ThermalFailure;

typedef struct ThermalStepStats {
    ThermalFailure failure;   /* set whenever thermal_step returns false */
    int picard_iterations, linear_iterations;
    int assemblies;           /* global matrix assemblies (one per Picard iteration) */
    double relaxation;        /* smallest Aitken relaxation factor used in the step; 1 when the step stayed linear */
    double linear_residual;
    /* energies over the step in J (powers in W for a steady solve) */
    double stored_energy;     /* sum of C (T1 - T0) with the capacity of the final iteration */
    double source_energy;
    double boundary_energy;   /* into the body through flux, convection and radiation faces */
    double prescribed_energy; /* into the body through prescribed-temperature nodes */
    double balance_error;     /* |stored - source - boundary - prescribed| / largest term */
    double interface_energy;  /* net heat crossing finite-conductance interfaces (0 for a closed set of pairs) */
    double enthalpy_change;   /* integral of H(T1) - H(T0) over the active elements, evaluated at the Gauss points:
                               * an independent physical measure of the stored energy */
    double enthalpy_mismatch; /* |stored - enthalpy| / largest term: 0 to round-off with the secant capacity */
    double liquid_volume;     /* m^3 of molten material at the end of the step (0 without a latent heat) */
    /* advection (J over the step; W for a steady solve). With advection the balance is
     *     stored = source + boundary + prescribed + advection + inflow
     * advection_energy: -integral rho cp u . grad T dV over the advected elements, the net enthalpy the velocity
     *                   carries into them; SUPG terms sum to zero over each element and do not enter
     * inflow_energy:    open faces, integral rho cp |u.n| (T_ambient - T) dA where u.n < 0
     * enthalpy_inflow:  open faces, integral rho cp |u.n| T_ambient dA where u.n < 0
     * enthalpy_outflow: open faces, integral rho cp (u.n) T dA where u.n > 0
     * divergence_energy: advection + inflow - (enthalpy_inflow - enthalpy_outflow): zero for a solenoidal velocity
     *                   that does not cross closed walls, so it measures the mass-conservation error of the field */
    double advection_energy, inflow_energy, enthalpy_inflow, enthalpy_outflow, divergence_energy;
    const char *linear_method; /* "pcg/jacobi" or "bicgstab/ilu0" */
    /* per-group energies of the step (J; W for a steady solve), when the model carries group labels */
    double group_stored[THERMAL_MAX_GROUPS], group_source[THERMAL_MAX_GROUPS];
    double group_boundary[THERMAL_MAX_GROUPS], group_prescribed[THERMAL_MAX_GROUPS];
    double interface_heat[THERMAL_MAX_GROUPS];
    double group_advection[THERMAL_MAX_GROUPS]; /* element groups: advection_energy of their elements */
    double group_inflow[THERMAL_MAX_GROUPS];    /* face groups: inflow_energy of their open faces */
    double tmin, tmax;        /* over active nodes */
    bool source_on, source_fallback, source_missed;
    double source_position[3];
    double source_power_mesh; /* power of the source integrated on the mesh before scaling (W) */
    double source_scale;
    double seconds;
    double seconds_assembly, seconds_solve, seconds_energy;
} ThermalStepStats;

typedef struct ThermalSolver ThermalSolver;

/* Temporal error estimate of a step, for adaptive time stepping.
 *
 * Given the accepted start state T_start, a coarse solution T_coarse and a fine solution T_fine at the same end time
 * (step doubling: one full step and two half steps), the estimated error of the fine solution is
 *     e = factor * (T_fine - T_coarse),     factor = 1 / (2^p - 1)
 * for a method of order p (Richardson). It is measured twice:
 *   temperature, per active node that is not prescribed:
 *     e_T,i / (tol.temperature + tol.relative * max(|T_start,i - T_ref|, |T_fine,i - T_ref|))
 *   enthalpy, per Gauss point of every active element:
 *     e_H,g / (tol_H + tol.relative * max(|H(T_start,g) - H(T_ref)|, |H(T_fine,g) - H(T_ref)|))
 * The relative parts scale with the change since the reference temperature, never with the absolute temperature, so
 * a small rise above 300 K is held to its own size. Both use the maximum over the mesh, so a local hot spot cannot hide
 * in an average. The enthalpy measure is what catches a phase transition: in a mushy zone a small temperature error is
 * a large latent-energy error. The step is acceptable when err <= 1. */
typedef struct ThermalTimeTolerance {
    double relative;        /* dimensionless */
    double temperature;     /* K, a temperature difference */
    double enthalpy;        /* J/m^3; <= 0: temperature * rho cp of each material at the reference temperature */
    bool temperature_only;  /* ignore the enthalpy measure (comparison studies only) */
} ThermalTimeTolerance;

typedef struct ThermalErrorNorm {
    double err;              /* max(err_T, err_H): the controller's measure, accept when <= 1 */
    double err_T, err_H;     /* weighted maxima of the two measures */
    double rms_T;            /* weighted root mean square of the temperature measure (reported, not used) */
    double max_dT;           /* K: the largest estimated temperature error */
    double max_dH;           /* J/m^3: the largest estimated enthalpy error */
    int worst_node;          /* node of err_T, -1 */
    int worst_elem;          /* element of err_H, -1 */
    int nodes, gauss_points; /* how many entered each measure */
} ThermalErrorNorm;

void thermal_error_norm(const ThermalModel *m, const double *T_start, const double *T_coarse, const double *T_fine, double factor, double T_ref,
                        const ThermalTimeTolerance *tol, ThermalErrorNorm *out);

/* builds the sparse pattern, element colouring and geometry caches; the element and node sets must not change later */
ThermalSolver *thermal_create(const ThermalModel *m, const ThermalOptions *opt, char *err, size_t errlen);
void thermal_free(ThermalSolver *s);
/* after a successful thermal_step: the heat (W, averaged over the step as the theta rule weights it) that flowed INTO the
 * model at node n through its prescribed temperature, i.e. the reaction of the constraint; 0 for free nodes */
double thermal_node_reaction(const ThermalSolver *s, const ThermalModel *m, int n);
/* the theta of the time integrator the solver was created with (1 backward Euler, 0.5 Crank-Nicolson) */
double thermal_theta(const ThermalSolver *s);
/* advances from T0 at time t to T1 at t + dt (T0 and T1 distinct, nnodes each); dt <= 0 solves the steady state
 * with T0 as the initial guess. Loads, activity and prescribed values may change between calls. */
bool thermal_step(ThermalSolver *s, const ThermalModel *m, const double *T0, double *T1, double t, double dt, ThermalStepStats *st, char *err,
                  size_t errlen);
