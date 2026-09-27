/* multibody.h - rigid multibody dynamics in generalised coordinates (fidelity level 2: rigid dynamics)
 *
 * Model
 *   Bodies are rigid, with mass, centre of mass and inertia about the centre of mass in body axes. Joints connect a parent
 *   (a body or the world) to a child. The first joint that reaches a body from the world forms the kinematic tree; any
 *   further joint between already connected bodies closes a loop and becomes a bilateral constraint (never a hidden
 *   stabilisation). Couplings (gear ratios) and joint limits in contact are constraints too.
 *
 * Frames, units and signs (SI throughout)
 *   world        right-handed, metres; gravity is a model vector (the NAVIER build frame uses +Z up, g = (0, 0, -9.80665))
 *   joint frames parent_frame is the joint frame on the parent (parent body coordinates), child_frame the joint frame on
 *                the child (child body coordinates). At q = 0 the two coincide.
 *   revolute     rotation of the child joint frame about axis (joint frame), right-hand rule, q in rad
 *   prismatic    translation of the child joint frame along axis, q in m
 *   spherical    q = unit quaternion (w, x, y, z) of the child joint frame in the parent joint frame; v = angular velocity
 *                of the child relative to the parent in child joint frame coordinates (rad/s)
 *   free         q = position (m) of the child joint frame origin in parent joint frame coordinates, then its quaternion;
 *                v = [w; v] relative angular and linear velocity in child joint frame coordinates
 *   wrenches     joint_wrench = [moment; force] exerted BY the parent ON the child, expressed in the child joint frame and
 *                taken about its origin. Loop joints: exerted by the parent-side body on the child-side body.
 *   inputs       tau: generalised forces on the degrees of freedom (N m or N, positive along the joint axis);
 *                body_wrench: world-frame [moment; force] applied at the body's centre of mass
 *
 * Integration
 *   Explicit Runge-Kutta-Munthe-Kaas of order 4 on the configuration manifold (exact SO(3) exponential and dexp^-1),
 *   inputs held constant over a step. Limit contacts are located as events and resolved with a Newton impact law.
 *   Loop, coupling and resting-limit constraints are enforced at acceleration level; the drift that integration leaves is
 *   measured, removed by an explicit mass-weighted projection and reported, together with the energy that projection
 *   changed. No Baumgarte terms, no constraint regularisation. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mechdiag.h"
#include "mmath.h"

enum { MB_NAME = 64, MB_AUX_MAX = 128 };

typedef enum { MB_FIXED = 0, MB_REVOLUTE, MB_PRISMATIC, MB_SPHERICAL, MB_FREE, MB_JOINT_TYPES } MbJointType;
typedef enum { MB_PASSIVE = 0, MB_ACTUATED, MB_PRESCRIBED } MbMotion;

const char *mb_joint_type_name(MbJointType t);
int mb_joint_type_from_name(const char *s); /* -1 if unknown */
int mb_joint_nq(MbJointType t);
int mb_joint_nv(MbJointType t);

typedef struct MbBodyDef {
    char name[MB_NAME];
    double mass;       /* kg */
    double com[3];     /* body frame, m */
    double inertia[9]; /* about the centre of mass, body axes, kg m^2 */
} MbBodyDef;

typedef struct MbJointDef {
    char name[MB_NAME];
    MbJointType type;
    int parent; /* body index, -1 = world */
    int child;
    MPose parent_frame;
    MPose child_frame;
    double axis[3]; /* revolute and prismatic: unit vector in the joint frame */
    MbMotion motion;
    /* revolute and prismatic */
    double stiffness, spring_reference; /* N m/rad or N/m; rad or m */
    double damping;                     /* N m s/rad or N s/m */
    double coulomb, coulomb_vreg;       /* friction torque/force magnitude and regularisation speed: -coulomb*tanh(v/vreg) */
    bool limited;
    double lower, upper; /* rad or m */
    double restitution;  /* 0 (plastic) .. 1 (elastic) */
    /* reflected rotor inertia of a rigidly geared motor (kg m^2, or kg for prismatic joints): N^2 J_rotor. It adds to the
     * generalised inertia of the coordinate; its reaction is carried by the parent body through the joint torque. */
    double armature;
    double q0[7], v0[6]; /* initial configuration and velocity, see the header comment */
    /* tree joints whose parent is a flexible body: the parent interface the joint rides on (-1 = the reference frame of
     * the flexible body); parent_frame's origin must be the interface point */
    int parent_interface;
} MbJointDef;

typedef struct MbCouplingDef {
    char name[MB_NAME];
    int follower, driver; /* revolute or prismatic tree joints */
    double ratio, offset; /* q_follower = ratio * q_driver + offset */
} MbCouplingDef;

/* compliant transmission between an input coordinate (motor side) and an output coordinate: the output receives
 * tau = k * dz(q_in / ratio - q_out) + c * (v_in / ratio - v_out), the input receives -tau / ratio, where dz removes a
 * backlash gap (total free play, centred). An explicit element, not a hidden stabilisation. */
typedef struct MbTransmissionDef {
    char name[MB_NAME];
    int input, output;  /* revolute or prismatic tree joints */
    double ratio;       /* input displacement per output displacement (gear reduction N) */
    double stiffness;   /* N m/rad or N/m at the output */
    double damping;     /* N m s/rad or N s/m at the output */
    double backlash;    /* rad or m at the output (total free play) */
} MbTransmissionDef;

/* Flexible bodies (fidelity level 4): the linear elastic deformation of a body in nmodes mass-normalised coordinates eta,
 * relative to a body-attached reference frame (the body frame, to which the body's own tree joint attaches, so reduced
 * models built with the parent attachment clamped fit this convention). First order in the deformation:
 *   kinetic energy  T = 1/2 V^T I(eta) V + V^T ell eta' + 1/2 eta'^T eta'
 *   strain energy   U = 1/2 sum omega2_m eta_m^2; modal damping -2 zeta_m sqrt(omega2_m) eta'_m
 *   I(eta)          first moment m c + sum eta_m ell_t,m and rotational inertia J_O + sum eta_m dJ_m (so centrifugal and
 *                   gravity loads on the deformation follow from the energy)
 *   interfaces      frames at body points that translate by phi eta and rotate by psi eta; tree joints to children ride on
 *                   them
 * Not included: second-order terms (geometric stiffening by centrifugal or axial stress, the change of the interface motion
 * with the deformation), loop joints on flexible bodies, contact on the deforming surface (contact shapes follow the
 * reference frame). */
typedef struct MbFlexInterface {
    double point[3]; /* body coordinates, m */
    double *phi;     /* 3 * nmodes: translation per unit coordinate, body axes */
    double *psi;     /* 3 * nmodes: small rotation per unit coordinate, body axes */
} MbFlexInterface;

typedef struct MbFlexDef {
    int body;
    int nmodes;
    double *omega2;  /* nmodes, (rad/s)^2, > 0 */
    double *zeta;    /* nmodes, modal damping ratios in [0, 1) */
    double *ell;     /* 6 * nmodes: [integral rho x cross phi dV; integral rho phi dV] about the body origin, body axes */
    double *dJ;      /* 9 * nmodes: change of the rotational inertia about the body origin per unit coordinate, body axes */
    int ninterfaces;
    MbFlexInterface *interfaces;
} MbFlexDef;

typedef struct MbModelDef {
    MbBodyDef *bodies;
    int nbodies, cap_bodies;
    MbJointDef *joints;
    int njoints, cap_joints;
    MbCouplingDef *couplings;
    int ncouplings, cap_couplings;
    MbTransmissionDef *transmissions;
    int ntransmissions, cap_transmissions;
    double gravity[3];
    MbFlexDef *flex;
    int nflex, cap_flex;
} MbModelDef;

MbModelDef *mbdef_new(void);
void mbdef_free(MbModelDef *d);
MbModelDef *mbdef_clone(const MbModelDef *d);
int mbdef_add_body(MbModelDef *d, const char *name, double mass, const double com[3], const double inertia[9]);
/* adds a joint with identity frames, zero initial state and an identity quaternion; returns its index (or -1) */
int mbdef_add_joint(MbModelDef *d, const char *name, MbJointType type, int parent, int child);
int mbdef_add_coupling(MbModelDef *d, const char *name, int follower, int driver, double ratio, double offset);
int mbdef_add_transmission(MbModelDef *d, const char *name, int input, int output, double ratio, double stiffness, double damping, double backlash);
/* adds zeroed elastic data (arrays allocated) for a body; returns its index or -1 */
int mbdef_add_flex(MbModelDef *d, int body, int nmodes, int ninterfaces);
bool mbflex_alloc(MbFlexDef *f, int body, int nmodes, int ninterfaces); /* zeroed arrays */
void mbflex_free(MbFlexDef *f);
int mbdef_body_index(const MbModelDef *d, const char *name);
int mbdef_joint_index(const MbModelDef *d, const char *name);

typedef struct MbOptions {
    double rank_tol;         /* singular-value ratio below which a scaled constraint direction is dependent (1e-9) */
    double singular_warn;    /* ratio flagging a near-singular configuration (1e-6) */
    double projection_tol;   /* accepted scaled position residual after projection (1e-11) */
    int projection_max_iter; /* 25 */
    bool project_positions;  /* true */
    bool project_velocities; /* true */
    double assembly_tol;     /* initial residual accepted without adjusting the configuration (1e-9, scaled) */
    double event_tol;        /* limit crossing location tolerance (rad, or m / length_scale) (1e-10) */
    double rest_velocity;    /* rebound speed below which a limit contact persists (rad/s, or m/s / length_scale) (1e-6) */
    int max_events_per_step; /* 64 */
    double length_scale;     /* m; 0 = derived from the model size. Mixes angular and linear rows in rank decisions */
} MbOptions;
void mb_options_default(MbOptions *o);

typedef struct MbDof {
    int body, joint; /* joint = -1 for an elastic coordinate of the flexible body `body` */
    double S_local[6]; /* motion subspace column in child body coordinates */
    bool angular;
} MbDof;

typedef struct MbModel {
    int nbodies, njoints, ncouplings, ntransmissions;
    MbBodyDef *bodies;
    MbJointDef *joints;
    MbCouplingDef *couplings;
    MbTransmissionDef *transmissions;
    int *order;       /* bodies, parents before children */
    int *body_parent; /* tree parent (-1 world) */
    int *body_joint;  /* tree joint */
    int *joint_qadr, *joint_vadr; /* -1 for loop joints */
    bool *joint_loop;
    MPose *child_frame_inv;
    int nq, nv;
    MbDof *dofs;
    int nlimits; /* limited joints */
    int *limit_joint;
    double gravity[3];
    double length_scale;
    MbOptions opt;
    /* flexible bodies: elastic coordinates follow the joint coordinates in q and v */
    int nflex, njq, njv;
    MbFlexDef *flex;        /* deep copies */
    int *body_flex;         /* nbodies: flex index or -1 */
    int *flex_qadr, *flex_vadr, *flex_gadr; /* nflex: first coordinate in q, in v, and in the list of all modes */
    int nmodes_total;
    int *vcol_start;        /* nbodies: first interface column of the body's tree joint, -1 without */
    int nvcol;
} MbModel;

MbModel *mb_compile(const MbModelDef *def, const MbOptions *opt, MechDiag *diag); /* NULL on errors (listed in diag) */
void mb_model_free(MbModel *m);

typedef struct MbLedger { /* J, accumulated from the initial state */
    double work_inputs;          /* generalised input forces (tau array and force callback) */
    double work_external;        /* applied body wrenches */
    double dissipated_damping;   /* viscous joint damping and transmission damping */
    double dissipated_friction;  /* regularised Coulomb joint friction */
    double dissipated_impacts;   /* kinetic energy removed by limit impacts (restitution < 1) */
    double contact_friction;     /* dissipated by friction between bodies in contact */
    double contact_impacts;      /* kinetic energy removed by normal contact impulses */
    double projection_change;    /* change of T + U caused by drift projection (numerical, should be small) */
} MbLedger;

typedef struct MbState {
    double t;
    /* auxiliary integrals (e.g. actuator energies) whose rates the force callback returns: integrated with the same
     * Runge-Kutta weights and event splitting as the mechanical state */
    double aux[MB_AUX_MAX];
    int nq, nv, nlimits;
    double *q, *v;
    signed char *limit_state; /* per limited joint (model order): 0 free, -1 resting on lower, +1 resting on upper */
    MbLedger ledger;
    double energy0; /* T + U of the assembled initial state */
    uint64_t steps, events;
} MbState;

MbState *mb_state_new(const MbModel *m);
void mb_state_free(MbState *s);
void mb_state_copy(MbState *dst, const MbState *src); /* same model */
/* q and v from the joint definitions (quaternions normalised; a zero quaternion is an error in diag) */
bool mb_state_initial(const MbModel *m, MbState *s, MechDiag *diag);

/* state-dependent generalised forces evaluated at every integrator stage (actuators): adds to tau (nv), writes the rates of
 * the auxiliary integrals into aux_rate (MB_AUX_MAX slots, zeroed before the call) and returns the mechanical input power
 * sum(tau * v) it delivered. Inputs other than (t, q, v) must be held constant over the step. */
typedef double (*MbForceFn)(void *ctx, double t, const double *q, const double *v, double *tau, double *aux_rate);

typedef struct MbInputs {
    const double *tau;         /* nv, or NULL */
    const double *body_wrench; /* 6 * nbodies, or NULL */
    MbForceFn force_fn;        /* NULL = none */
    void *force_ctx;
} MbInputs;

typedef struct MbConstraintInfo {
    int rows;               /* bilateral rows (loops, couplings, resting limits) */
    int rank;
    int redundant;          /* rows - rank */
    int mobility;           /* nv - rank */
    double sigma_min_ratio; /* smallest non-zero scaled singular value / largest (1 without constraints) */
    double inconsistency;   /* scaled acceleration-bias component outside the constraint range (0 = consistent) */
    double residual;        /* scaled position residual */
} MbConstraintInfo;

typedef struct MbStepReport {
    int events;
    int projection_iterations;
    double drift_before, drift_after;   /* scaled position residual */
    double vdrift_before, vdrift_after; /* scaled velocity residual */
    MbConstraintInfo cinfo;
    char error[256];
} MbStepReport;

typedef struct MbSim MbSim; /* workspace for one model; not thread-safe, use one per thread */
MbSim *mb_sim_new(const MbModel *m);
void mb_sim_free(MbSim *sim);
const MbModel *mb_sim_model(const MbSim *sim);

/* Checks limits, closes loops by mass-weighted projection (reporting every adjustment), makes the velocity consistent,
 * analyses the constraints (rank, redundancy, mobility, singularity) and records the initial energy. */
bool mb_assemble(MbSim *sim, MbState *s, MechDiag *diag);

/* one step of length h (events inside the step are located); false on failure with rep->error */
bool mb_step(MbSim *sim, MbState *s, double h, const MbInputs *in, MbStepReport *rep);

typedef struct MbEval {
    double *qacc;         /* nv */
    double *tau_passive;  /* nv: springs, damping, friction */
    double *tau_constraint; /* nv: generalised constraint forces G^T lambda */
    double *joint_wrench; /* 6 * njoints */
    double *body_pose;    /* 12 * nbodies: R (9, row-major) then p (3), world */
    double *body_twist;   /* 6 * nbodies: [w; v of the centre of mass], world */
    double *tau_input;    /* nv: tau array plus the force callback */
    double kinetic, potential_gravity, potential_spring; /* potential_spring includes transmission springs */
    double momentum[6];   /* [angular about the world origin; linear], world */
    double energy_residual; /* (T + U) - E0 - inputs + dissipation - projection */
    double joint_torque_residual; /* max |S^T f - applied| over the degrees of freedom (consistency of the reactions) */
    MbConstraintInfo cinfo;
} MbEval;
MbEval *mb_eval_new(const MbModel *m);
void mb_eval_free(MbEval *e);
/* forward dynamics, reactions, energy and momentum at the current state */
bool mb_evaluate(MbSim *sim, const MbState *s, const MbInputs *in, MbEval *out);

/* tree inverse dynamics (recursive Newton-Euler): generalised forces for accelerations qacc, and the joint wrenches of the
 * tree joints. Loop constraints are not included: use mb_evaluate for closed chains. */
void mb_inverse_dynamics(MbSim *sim, const double *q, const double *v, const double *qacc, const double *body_wrench, double *tau,
                         double *joint_wrench);
void mb_mass_matrix(MbSim *sim, const double *q, double *M); /* nv x nv */
/* elastic coordinates of the flexible bodies in static equilibrium with the joint coordinates and velocities held: they solve
 * w^2 eta = -h_e(q, v, eta) (gravity, and centrifugal and gyroscopic loads of the given velocities, through the tree), by
 * fixed-point iteration (the load depends on eta only through the small deformation). Loop constraint forces are not
 * included. The energy baseline and the ledger are reset. Returns the iterations, or -1 without convergence; residual
 * receives max |w^2 eta + h_e| / max |h_e|. */
int mb_flex_equilibrium(MbSim *sim, MbState *s, double tol, int max_iter, double *residual);
/* scaled position residual of the bilateral constraints (loops, couplings, resting limits) */
double mb_constraint_residual(MbSim *sim, const MbState *s);
/* world pose of a body at the state's configuration */
void mb_body_pose(MbSim *sim, const MbState *s, int body, MPose *T);
/* world poses (and, when twist is not NULL, world twists [w; v at the origin], 6 per body) of all bodies in one pass */
void mb_body_poses(MbSim *sim, const MbState *s, MPose *poses, double *twist);

/* ---- contact time stepping (Moreau-Jean, first order) ----
 * Rows between two bodies (or a body and the world) found by a contact detector (contact.h). Over a step h:
 *   M (v+ - v) = h (tau - C) + G^T p_bilateral + J^T p   (impulses p in N s; average forces p / h)
 *   normal:   0 <= p_n  _|_  v_n+ - target >= 0, target = e |v_n-| when an impact happens within the step (approach faster
 *             than restitution_speed), -gap / h for a speculative contact that is still open, 0 when touching or penetrated
 *   friction: |p_t| <= mu p_n (Coulomb disc), projected Gauss-Seidel
 *   positions: q+ = q (+) h v+  (symplectic Euler on the manifold)
 * Joint limits become one-sided rows of the same kind; loops and couplings are equality rows. Penetration is not pushed
 * out by an artificial velocity: it is measured and reported. */
typedef struct MbContactRow {
    int body_a, body_b;            /* model bodies, -1 = world */
    double point_a[3], point_b[3]; /* world */
    double normal[3];              /* world unit normal from B to A */
    double gap;                    /* m, at the start of the step */
    double friction, restitution;
    /* solution */
    double impulse_normal;         /* N s */
    double impulse_tangent[2];     /* N s along tangent[0], tangent[1] */
    double tangent[2][3];
    double normal_velocity;        /* m/s after the step, A relative to B along the normal */
    double tangential_speed;       /* m/s after the step */
    signed char state;             /* 0 open or separating, 1 sticking, 2 sliding */
    bool impact;                   /* the step resolved an impact (restitution or approach faster than restitution_speed): the
                                    * average force impulse / h then depends on h and is not a load for a stress assessment */
    double warm[4];                /* input with MbContactSolve.warm_start: starting normal impulse and world tangential impulse (N s) */
} MbContactRow;

typedef struct MbContactSolve {
    int max_iterations;       /* projected Gauss-Seidel sweeps */
    double tolerance;         /* relative impulse change that ends the sweeps */
    double restitution_speed; /* m/s */
    bool warm_start;          /* start from rows[].warm (projected onto the friction cone) instead of zero */
} MbContactSolve;

typedef struct MbContactStats {
    int iterations;
    double residual;
    bool converged;
    int bilateral_rows, limit_rows, contact_rows;
    double friction_dissipation, impact_dissipation; /* J in this step */
    double bilateral_work;                           /* J: should vanish (ideal constraints) */
} MbContactStats;

bool mb_step_contacts(MbSim *sim, MbState *s, double h, const MbInputs *in, MbContactRow *rows, int nrows, const MbContactSolve *opt, MbContactStats *st,
                      MbStepReport *rep);
/* after mb_evaluate: specific force (acceleration minus gravity, what an accelerometer measures) and angular velocity of a
 * body-fixed point p (body coordinates), world axes; angular acceleration of the body. */
void mb_point_motion(MbSim *sim, int body, const double p[3], double specific_force[3], double omega[3], double alpha[3], double R_body[9]);
/* joint coordinate (scalar joints) and joint-frame helpers */
double mb_joint_q(const MbModel *m, const MbState *s, int joint);
double mb_joint_v(const MbModel *m, const MbState *s, int joint);
