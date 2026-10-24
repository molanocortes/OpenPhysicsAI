/* contact.h - collision shapes and contact detection between bodies of a multibody model
 *
 * Shapes (in body coordinates, or world when body = -1): sphere, capsule (segment along the shape z axis), box (half
 * extents), half-space plane (outward normal +z of its frame) and convex vertex sets (hulls, contact against planes only
 * in this build). Nothing is approximated silently: a part used for contact is represented by the shapes the user
 * declares (a concave part needs explicit convex pieces).
 *
 * Detection returns contact rows (multibody.h, MbContactRow) with the unit normal pointing from shape B to shape A, a
 * point on each surface in world coordinates and the signed gap (negative = penetration). Box-plane, box-box, capsule-
 * plane and hull-plane contacts produce manifolds of several points. Pairs within `margin` (plus the distance both bodies
 * can travel in the step, so fast bodies cannot tunnel through thin obstacles) become speculative contacts.
 *
 * Pair parameters: friction mu = min(mu_A, mu_B), restitution e = max(e_A, e_B). Friction and restitution coefficients are
 * model parameters with their stated provenance, not measured material properties. */
#pragma once

#include <stdbool.h>

#include "mechdiag.h"
#include "mmath.h"
#include "multibody.h"

enum { CT_MAX_POINTS = 1024 };

typedef enum { SHAPE_SPHERE = 0, SHAPE_CAPSULE, SHAPE_BOX, SHAPE_PLANE, SHAPE_HULL, SHAPE_TYPES } ShapeType;
const char *shape_type_name(ShapeType t);
int shape_type_from_name(const char *s);

typedef struct ContactShape {
    char name[MB_NAME];
    ShapeType type;
    int body;           /* model body index, -1 = world (static) */
    MPose pose;         /* shape frame in body (or world) coordinates */
    double radius;      /* sphere, capsule */
    double half_length; /* capsule: half of the segment along z */
    double half[3];     /* box half extents */
    int nverts;         /* hull */
    const double *verts; /* 3*nverts, shape frame (not owned) */
    double friction;    /* Coulomb coefficient */
    double restitution; /* Newton coefficient */
    int group;          /* shapes with the same non-zero group never collide */
} ContactShape;

typedef struct ContactOptions {
    double margin;            /* speculative distance (m, default 1e-3) */
    double restitution_speed; /* approach speed below which restitution is not applied (m/s, default 1e-3) */
    int max_iterations;       /* projected Gauss-Seidel sweeps (default 500) */
    double tolerance;         /* relative impulse change that ends the sweeps (default 1e-10) */
    bool exclude_jointed;     /* no contact between a parent and a child of a joint, the world included (default true) */
} ContactOptions;
void contact_options_default(ContactOptions *o);

typedef struct ContactReport {
    int points;
    int unsupported_pairs;    /* shape pairs this build cannot evaluate (reported, never ignored silently) */
    char unsupported[160];
    MbContactStats solve;
    double max_penetration;   /* m, after the step (0 when none) */
} ContactReport;

/* narrow phase for one shape pair at world poses TA, TB; appends up to cap rows (bodies, points, normal, gap, pair
 * coefficients); returns the number added or -1 when the pair type is unsupported */
int contact_pair(const ContactShape *A, const MPose *TA, const ContactShape *B, const MPose *TB, double margin, MbContactRow *out, int cap);

/* all contacts at the given body poses; speed[b] (m/s, may be NULL) widens the margin by the distance travelled in h */
int contact_detect(const MbModel *m, const ContactShape *shapes, int nshapes, const MPose *body_pose, const double *speed, double h, const ContactOptions *opt,
                   MbContactRow *rows, int *shape_a, int *shape_b, int cap, ContactReport *rep);

/* contact shapes of an assembly: collision geometry of its bodies (resolved against the model definition) and its static
 * environment. Only box, sphere, capsule and plane take part; mesh and cylinder collision geometry is reported, never
 * approximated silently. A missing friction coefficient is a missing input. Returns the number of shapes (-1 on errors). */
struct Assembly;
int asm_contact_shapes(const struct Assembly *a, const MbModelDef *def, ContactShape *out, int cap, MechDiag *d);

/* Warm start: the forces (impulse / step) of the persistent contacts of the previous step, matched to the new contacts by
 * shape pair and nearest point, start the solver. Resting contact then converges in a few sweeps and stacks converge at
 * all. The store is part of the simulation state: runtimes checkpoint it, so replays stay bitwise identical. Impact rows
 * are not stored. */
typedef struct ContactWarm {
    int n, cap;
    int *sa, *sb;   /* shape indices */
    double *point;  /* 3 per entry: contact point on shape A, world */
    double *force;  /* 4 per entry: normal force and world tangential force, N */
} ContactWarm;
bool contact_warm_init(ContactWarm *w, int cap);
void contact_warm_free(ContactWarm *w);
/* upper bound of simultaneous contact rows (the largest manifold of every pair that can touch), at most CT_MAX_POINTS */
int contact_warm_capacity(const MbModel *m, const ContactShape *shapes, int nshapes, const ContactOptions *opt);

/* one time step with contacts: detection at the start of the step, then mb_step_contacts; afterwards the penetration at
 * the new positions is measured and reported. rows receives the solved contacts (cap CT_MAX_POINTS). warm (may be NULL)
 * needs shape_a and shape_b; it is read before the solve and replaced after it. */
bool contact_step(MbSim *sim, MbState *s, double h, const MbInputs *in, const ContactShape *shapes, int nshapes, const ContactOptions *opt, MbContactRow *rows,
                  int *shape_a, int *shape_b, int *nrows, ContactReport *rep, MbStepReport *step, ContactWarm *warm);

