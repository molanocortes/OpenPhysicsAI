/* multibody.c - rigid multibody dynamics (see multibody.h)
 *
 * Implementation notes
 *   - Kinematics, recursive Newton-Euler (RNEA) and the composite-rigid-body algorithm (CRBA) run in world Plücker
 *     coordinates: every body twist, motion subspace column and spatial inertia is referred to the world origin, so
 *     composite inertias add without transforms.
 *   - Bilateral constraints (loop joints, couplings, resting limits) are rows G v = 0. Rank decisions use the scaled
 *     Jacobian Gs = Dr^-1 G Dc (linear rows and linear coordinates scaled by the model length) and its SVD. Multipliers
 *     are restricted to the scaled row space, so redundant constraints give the minimum-norm reaction and are reported.
 *   - Integration: RKMK4 with inputs held over the step; limit events located by safeguarded false position. */
#include "multibody.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------ names, def */

static const char *const JOINT_NAMES[MB_JOINT_TYPES] = {"fixed", "revolute", "prismatic", "spherical", "free"};
static const int JOINT_NQ[MB_JOINT_TYPES] = {0, 1, 1, 4, 7};
static const int JOINT_NV[MB_JOINT_TYPES] = {0, 1, 1, 3, 6};

const char *mb_joint_type_name(MbJointType t) { return (unsigned)t < MB_JOINT_TYPES ? JOINT_NAMES[t] : "?"; }
int mb_joint_type_from_name(const char *s) {
    for (int i = 0; s && i < MB_JOINT_TYPES; i++)
        if (!strcmp(s, JOINT_NAMES[i])) return i;
    return -1;
}
int mb_joint_nq(MbJointType t) { return (unsigned)t < MB_JOINT_TYPES ? JOINT_NQ[t] : 0; }
int mb_joint_nv(MbJointType t) { return (unsigned)t < MB_JOINT_TYPES ? JOINT_NV[t] : 0; }

MbModelDef *mbdef_new(void) {
    MbModelDef *d = calloc(1, sizeof *d);
    if (d) mv3_set(d->gravity, 0, 0, -9.80665);
    return d;
}

void mbflex_free(MbFlexDef *f) {
    if (!f) return;
    free(f->omega2), free(f->zeta), free(f->ell), free(f->dJ);
    for (int i = 0; f->interfaces && i < f->ninterfaces; i++) free(f->interfaces[i].phi), free(f->interfaces[i].psi);
    free(f->interfaces);
    memset(f, 0, sizeof *f);
}

bool mbflex_alloc(MbFlexDef *f, int body, int nmodes, int ninterfaces) {
    memset(f, 0, sizeof *f);
    size_t n = (size_t)(nmodes > 0 ? nmodes : 1);
    f->body = body, f->nmodes = nmodes, f->ninterfaces = ninterfaces;
    f->omega2 = calloc(n, sizeof(double)), f->zeta = calloc(n, sizeof(double)), f->ell = calloc(6 * n, sizeof(double)), f->dJ = calloc(9 * n, sizeof(double));
    f->interfaces = calloc((size_t)(ninterfaces > 0 ? ninterfaces : 1), sizeof *f->interfaces);
    bool ok = f->omega2 && f->zeta && f->ell && f->dJ && f->interfaces;
    for (int i = 0; ok && i < ninterfaces; i++) ok = (f->interfaces[i].phi = calloc(3 * n, sizeof(double))) && (f->interfaces[i].psi = calloc(3 * n, sizeof(double)));
    if (!ok) mbflex_free(f);
    return ok;
}

static bool flex_copy(MbFlexDef *d, const MbFlexDef *s) {
    if (!mbflex_alloc(d, s->body, s->nmodes, s->ninterfaces)) return false;
    size_t n = (size_t)(s->nmodes > 0 ? s->nmodes : 0);
    memcpy(d->omega2, s->omega2, n * sizeof(double)), memcpy(d->zeta, s->zeta, n * sizeof(double));
    memcpy(d->ell, s->ell, 6 * n * sizeof(double)), memcpy(d->dJ, s->dJ, 9 * n * sizeof(double));
    for (int i = 0; i < s->ninterfaces; i++) {
        mv3_copy(d->interfaces[i].point, s->interfaces[i].point);
        memcpy(d->interfaces[i].phi, s->interfaces[i].phi, 3 * n * sizeof(double));
        memcpy(d->interfaces[i].psi, s->interfaces[i].psi, 3 * n * sizeof(double));
    }
    return true;
}

void mbdef_free(MbModelDef *d) {
    if (!d) return;
    free(d->bodies);
    free(d->joints);
    free(d->couplings);
    free(d->transmissions);
    for (int f = 0; d->flex && f < d->nflex; f++) mbflex_free(&d->flex[f]);
    free(d->flex);
    free(d);
}

static bool grow(void **p, int *cap, int need, size_t size) {
    if (need <= *cap) return true;
    int nc = *cap ? *cap * 2 : 8;
    while (nc < need) nc *= 2;
    void *q = realloc(*p, (size_t)nc * size);
    if (!q) return false;
    *p = q, *cap = nc;
    return true;
}

MbModelDef *mbdef_clone(const MbModelDef *d) {
    MbModelDef *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    *c = *d;
    c->bodies = NULL, c->joints = NULL, c->couplings = NULL, c->transmissions = NULL, c->flex = NULL;
    c->cap_bodies = c->nbodies, c->cap_joints = c->njoints, c->cap_couplings = c->ncouplings, c->cap_transmissions = c->ntransmissions;
    c->nflex = 0, c->cap_flex = 0;
    bool ok = true;
#define DUP(field, n)                                                                                                                          \
    if (n) {                                                                                                                                   \
        c->field = malloc((size_t)(n) * sizeof *d->field);                                                                                     \
        if (c->field) memcpy(c->field, d->field, (size_t)(n) * sizeof *d->field);                                                              \
        else ok = false;                                                                                                                       \
    }
    DUP(bodies, d->nbodies)
    DUP(joints, d->njoints)
    DUP(couplings, d->ncouplings)
    DUP(transmissions, d->ntransmissions)
#undef DUP
    if (ok && d->nflex) {
        c->flex = calloc((size_t)d->nflex, sizeof *c->flex);
        ok = c->flex != NULL;
        for (int f = 0; ok && f < d->nflex; f++) {
            ok = flex_copy(&c->flex[f], &d->flex[f]);
            if (ok) c->nflex = f + 1;
        }
        c->cap_flex = d->nflex;
    }
    if (!ok) {
        mbdef_free(c);
        return NULL;
    }
    return c;
}

int mbdef_add_body(MbModelDef *d, const char *name, double mass, const double com[3], const double inertia[9]) {
    if (!grow((void **)&d->bodies, &d->cap_bodies, d->nbodies + 1, sizeof *d->bodies)) return -1;
    MbBodyDef *b = &d->bodies[d->nbodies];
    memset(b, 0, sizeof *b);
    snprintf(b->name, sizeof b->name, "%s", name ? name : "");
    b->mass = mass;
    if (com) mv3_copy(b->com, com);
    if (inertia) memcpy(b->inertia, inertia, sizeof b->inertia);
    return d->nbodies++;
}

int mbdef_add_joint(MbModelDef *d, const char *name, MbJointType type, int parent, int child) {
    if (!grow((void **)&d->joints, &d->cap_joints, d->njoints + 1, sizeof *d->joints)) return -1;
    MbJointDef *j = &d->joints[d->njoints];
    memset(j, 0, sizeof *j);
    snprintf(j->name, sizeof j->name, "%s", name ? name : "");
    j->type = type;
    j->parent = parent;
    j->child = child;
    mpose_identity(&j->parent_frame);
    mpose_identity(&j->child_frame);
    mv3_set(j->axis, 0, 0, 1);
    if (type == MB_SPHERICAL) j->q0[0] = 1;
    if (type == MB_FREE) j->q0[3] = 1;
    j->parent_interface = -1;
    return d->njoints++;
}

int mbdef_add_flex(MbModelDef *d, int body, int nmodes, int ninterfaces) {
    if (nmodes < 1 || ninterfaces < 0) return -1;
    if (!grow((void **)&d->flex, &d->cap_flex, d->nflex + 1, sizeof *d->flex)) return -1;
    if (!mbflex_alloc(&d->flex[d->nflex], body, nmodes, ninterfaces)) return -1;
    return d->nflex++;
}

int mbdef_add_coupling(MbModelDef *d, const char *name, int follower, int driver, double ratio, double offset) {
    if (!grow((void **)&d->couplings, &d->cap_couplings, d->ncouplings + 1, sizeof *d->couplings)) return -1;
    MbCouplingDef *c = &d->couplings[d->ncouplings];
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name ? name : "");
    c->follower = follower, c->driver = driver, c->ratio = ratio, c->offset = offset;
    return d->ncouplings++;
}

int mbdef_add_transmission(MbModelDef *d, const char *name, int input, int output, double ratio, double stiffness, double damping, double backlash) {
    if (!grow((void **)&d->transmissions, &d->cap_transmissions, d->ntransmissions + 1, sizeof *d->transmissions)) return -1;
    MbTransmissionDef *t = &d->transmissions[d->ntransmissions];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name ? name : "");
    t->input = input, t->output = output, t->ratio = ratio, t->stiffness = stiffness, t->damping = damping, t->backlash = backlash;
    return d->ntransmissions++;
}

int mbdef_body_index(const MbModelDef *d, const char *name) {
    for (int i = 0; name && i < d->nbodies; i++)
        if (!strcmp(d->bodies[i].name, name)) return i;
    return -1;
}

int mbdef_joint_index(const MbModelDef *d, const char *name) {
    for (int i = 0; name && i < d->njoints; i++)
        if (!strcmp(d->joints[i].name, name)) return i;
    return -1;
}

void mb_options_default(MbOptions *o) {
    memset(o, 0, sizeof *o);
    o->rank_tol = 1e-9;
    o->singular_warn = 1e-6;
    o->projection_tol = 1e-11;
    o->projection_max_iter = 25;
    o->project_positions = true;
    o->project_velocities = true;
    o->assembly_tol = 1e-9;
    o->event_tol = 1e-10;
    o->rest_velocity = 1e-6;
    o->max_events_per_step = 64;
    o->length_scale = 0;
}

/* ------------------------------------------------------------------------------------------------ compile */

static bool rotation_ok(const double *R) {
    double RtR[9], Rt[9];
    mm3_transpose(Rt, R);
    mm3_mul(RtR, Rt, R);
    double err = 0;
    for (int i = 0; i < 9; i++) err = fmax(err, fabs(RtR[i] - (i % 4 == 0 ? 1 : 0)));
    return err < 1e-9 && mm3_det(R) > 0;
}

static bool finite_all(const double *x, int n) {
    for (int i = 0; i < n; i++)
        if (!isfinite(x[i])) return false;
    return true;
}

/* principal moments of a symmetric 3x3 matrix, descending */
static void sym3_eigenvalues(const double *I, double *w) {
    double a[9], V[9];
    memcpy(a, I, sizeof a);
    mm_sym_eig(a, 3, w, V);
}

void mb_model_free(MbModel *m) {
    if (!m) return;
    free(m->bodies);
    free(m->joints);
    free(m->couplings);
    free(m->transmissions);
    free(m->order);
    free(m->body_parent);
    free(m->body_joint);
    free(m->joint_qadr);
    free(m->joint_vadr);
    free(m->joint_loop);
    free(m->child_frame_inv);
    free(m->dofs);
    free(m->limit_joint);
    for (int f = 0; m->flex && f < m->nflex; f++) mbflex_free(&m->flex[f]);
    free(m->flex), free(m->body_flex), free(m->flex_qadr), free(m->flex_vadr), free(m->flex_gadr), free(m->vcol_start);
    free(m);
}

MbModel *mb_compile(const MbModelDef *def, const MbOptions *opt, MechDiag *diag) {
    MbModel *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    if (opt)
        m->opt = *opt;
    else
        mb_options_default(&m->opt);
    int nb = def->nbodies, nj = def->njoints, nc = def->ncouplings, nt = def->ntransmissions;
    int errors = 0;
    m->nbodies = nb, m->njoints = nj, m->ncouplings = nc, m->ntransmissions = nt;
    m->transmissions = calloc((size_t)(nt ? nt : 1), sizeof *m->transmissions);
    m->bodies = calloc((size_t)(nb ? nb : 1), sizeof *m->bodies);
    m->joints = calloc((size_t)(nj ? nj : 1), sizeof *m->joints);
    m->couplings = calloc((size_t)(nc ? nc : 1), sizeof *m->couplings);
    m->order = calloc((size_t)(nb ? nb : 1), sizeof *m->order);
    m->body_parent = calloc((size_t)(nb ? nb : 1), sizeof *m->body_parent);
    m->body_joint = calloc((size_t)(nb ? nb : 1), sizeof *m->body_joint);
    m->joint_qadr = calloc((size_t)(nj ? nj : 1), sizeof *m->joint_qadr);
    m->joint_vadr = calloc((size_t)(nj ? nj : 1), sizeof *m->joint_vadr);
    m->joint_loop = calloc((size_t)(nj ? nj : 1), sizeof *m->joint_loop);
    m->child_frame_inv = calloc((size_t)(nj ? nj : 1), sizeof *m->child_frame_inv);
    m->limit_joint = calloc((size_t)(nj ? nj : 1), sizeof *m->limit_joint);
    if (!m->bodies || !m->joints || !m->couplings || !m->order || !m->body_parent || !m->body_joint || !m->joint_qadr ||
        !m->joint_vadr || !m->joint_loop || !m->child_frame_inv || !m->limit_joint) {
        mdiag_add(diag, MD_ERROR, "OUT_OF_MEMORY", "", NULL, "out of memory compiling the model");
        mb_model_free(m);
        return NULL;
    }
    if (nb) memcpy(m->bodies, def->bodies, (size_t)nb * sizeof *m->bodies);
    if (nj) memcpy(m->joints, def->joints, (size_t)nj * sizeof *m->joints);
    if (nc) memcpy(m->couplings, def->couplings, (size_t)nc * sizeof *m->couplings);
    if (nt && m->transmissions) memcpy(m->transmissions, def->transmissions, (size_t)nt * sizeof *m->transmissions);
    mv3_copy(m->gravity, def->gravity);
    if (!finite_all(m->gravity, 3)) {
        mdiag_add(diag, MD_ERROR, "INVALID_GRAVITY", "gravity", NULL, "gravity must be a finite vector");
        errors++;
    }

    /* bodies */
    for (int i = 0; i < nb; i++) {
        MbBodyDef *b = &m->bodies[i];
        for (int k = 0; k < i; k++)
            if (!strcmp(m->bodies[k].name, b->name)) {
                mdiag_add(diag, MD_ERROR, "DUPLICATE_NAME", b->name, NULL, "two bodies are named '%s'", b->name);
                errors++;
            }
        if (!(b->mass > 0) || !isfinite(b->mass)) {
            mdiag_add(diag, MD_ERROR, "MASS_NOT_POSITIVE", b->name, "give the measured mass, or mass properties from a closed mesh with a declared fill",
                      "body '%s' has mass %g kg; rigid bodies need a positive mass", b->name, b->mass);
            errors++;
            continue;
        }
        if (!finite_all(b->com, 3) || !finite_all(b->inertia, 9)) {
            mdiag_add(diag, MD_ERROR, "NOT_FINITE", b->name, NULL, "body '%s' has a non-finite centre of mass or inertia", b->name);
            errors++;
            continue;
        }
        double asym = 0, scale = 0;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                asym = fmax(asym, fabs(b->inertia[3 * r + c] - b->inertia[3 * c + r]));
                scale = fmax(scale, fabs(b->inertia[3 * r + c]));
            }
        if (asym > 1e-9 * scale) {
            mdiag_add(diag, MD_ERROR, "INERTIA_NOT_SYMMETRIC", b->name, NULL, "inertia of '%s' is not symmetric (max asymmetry %g kg m^2)", b->name, asym);
            errors++;
            continue;
        }
        double w[3];
        sym3_eigenvalues(b->inertia, w);
        if (!(w[2] > 1e-12 * fmax(w[0], DBL_MIN)) || w[1] + w[2] < w[0] * (1 - 1e-9)) {
            mdiag_add(diag, MD_ERROR, "INERTIA_NOT_ADMISSIBLE", b->name,
                      "principal moments must be positive and satisfy I1 + I2 >= I3 (a physical mass distribution)",
                      "inertia of '%s' about its centre of mass has principal moments %.6g, %.6g, %.6g kg m^2", b->name, w[0], w[1], w[2]);
            errors++;
        }
    }

    /* joints */
    bool *jvalid = calloc((size_t)nj + 1, sizeof *jvalid);
    if (!jvalid) {
        mb_model_free(m);
        return NULL;
    }
    for (int j = 0; j < nj; j++) {
        MbJointDef *J = &m->joints[j];
        for (int k = 0; k < j; k++)
            if (!strcmp(m->joints[k].name, J->name)) {
                mdiag_add(diag, MD_ERROR, "DUPLICATE_NAME", J->name, NULL, "two joints are named '%s'", J->name);
                errors++;
            }
        if ((unsigned)J->type >= MB_JOINT_TYPES) {
            mdiag_add(diag, MD_ERROR, "UNKNOWN_JOINT_TYPE", J->name, NULL, "joint '%s' has an unknown type", J->name);
            errors++;
            continue;
        }
        if (J->child < 0 || J->child >= nb || J->parent < -1 || J->parent >= nb || J->parent == J->child) {
            mdiag_add(diag, MD_ERROR, "INVALID_JOINT_BODIES", J->name, NULL, "joint '%s' must connect a parent (a body or the world) to a different child body",
                      J->name);
            errors++;
            continue;
        }
        jvalid[j] = true;
        if (!rotation_ok(J->parent_frame.R) || !rotation_ok(J->child_frame.R) || !finite_all(J->parent_frame.p, 3) ||
            !finite_all(J->child_frame.p, 3)) {
            mdiag_add(diag, MD_ERROR, "INVALID_JOINT_FRAME", J->name, "frames need an orthonormal right-handed rotation and a finite origin",
                      "joint '%s' has an invalid parent or child frame", J->name);
            errors++;
        }
        mpose_inverse(&m->child_frame_inv[j], &J->child_frame);
        if (J->type == MB_REVOLUTE || J->type == MB_PRISMATIC) {
            double n = mv3_norm(J->axis);
            if (!(n > 0) || !isfinite(n)) {
                mdiag_add(diag, MD_ERROR, "AXIS_ZERO", J->name, "give the axis as a unit vector in the joint frame", "joint '%s' has no axis", J->name);
                errors++;
            } else if (fabs(n - 1) > 1e-9) {
                mv3_scale(J->axis, J->axis, 1 / n);
                mdiag_add(diag, MD_WARNING, "AXIS_NORMALISED", J->name, NULL, "axis of joint '%s' had length %.9g and was normalised", J->name, n);
            }
            if (J->stiffness < 0 || J->damping < 0 || J->coulomb < 0 || !isfinite(J->stiffness) || !isfinite(J->damping) ||
                !isfinite(J->coulomb)) {
                mdiag_add(diag, MD_ERROR, "NEGATIVE_PASSIVE", J->name, NULL, "stiffness, damping and friction of joint '%s' must be finite and >= 0", J->name);
                errors++;
            }
            if (J->coulomb > 0 && !(J->coulomb_vreg > 0)) {
                mdiag_add(diag, MD_MISSING_INPUT, "FRICTION_REGULARISATION", J->name,
                          "set coulomb_vreg: friction is -F tanh(v/vreg); smaller values approach ideal Coulomb friction and need smaller steps",
                          "joint '%s' has Coulomb friction but no regularisation speed", J->name);
                errors++;
            }
            if (J->limited && (!(J->lower < J->upper) || J->restitution < 0 || J->restitution > 1)) {
                mdiag_add(diag, MD_ERROR, "INVALID_LIMITS", J->name, NULL, "joint '%s' needs lower < upper and restitution in [0, 1]", J->name);
                errors++;
            }
            if (!(J->armature >= 0) || !isfinite(J->armature)) {
                mdiag_add(diag, MD_ERROR, "NEGATIVE_PASSIVE", J->name, NULL, "armature (reflected rotor inertia) of joint '%s' must be finite and >= 0", J->name);
                errors++;
            }
        } else if (J->limited || J->stiffness > 0 || J->damping > 0 || J->coulomb > 0 || J->armature != 0) {
            mdiag_add(diag, MD_ERROR, "UNSUPPORTED", J->name, "limits, springs, damping, friction and armature are available on revolute and prismatic joints",
                      "joint '%s' (%s) cannot carry limits or passive elements yet", J->name, JOINT_NAMES[J->type]);
            errors++;
        }
        if (J->motion == MB_PRESCRIBED) {
            mdiag_add(diag, MD_ERROR, "UNSUPPORTED", J->name, "use an actuated joint with a controller, or wait for trajectories (stage 3)",
                      "prescribed motion of joint '%s' needs a trajectory, which this build does not provide yet", J->name);
            errors++;
        }
    }
    /* kinematic tree (checked even when bodies or joints have errors, so every problem is reported at once): the first
     * joint that reaches a body from the world; later joints between connected bodies are loops */
    bool *reached = calloc((size_t)nb + 1, sizeof *reached);
    bool *assigned = calloc((size_t)nj + 1, sizeof *assigned);
    if (!reached || !assigned) {
        free(reached), free(assigned), free(jvalid);
        mb_model_free(m);
        return NULL;
    }
    for (int j = 0; j < nj; j++)
        if (!jvalid[j]) assigned[j] = true; /* invalid joints take no part in the tree */
    int norder = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (int j = 0; j < nj; j++) {
            MbJointDef *J = &m->joints[j];
            if (assigned[j]) continue;
            bool pr = J->parent < 0 || reached[J->parent];
            if (pr && !reached[J->child]) {
                reached[J->child] = true;
                assigned[j] = true;
                m->body_parent[J->child] = J->parent;
                m->body_joint[J->child] = j;
                m->order[norder++] = J->child;
                changed = true;
            }
        }
    }
    for (int j = 0; j < nj; j++) {
        MbJointDef *J = &m->joints[j];
        if (assigned[j] || !jvalid[j]) continue;
        bool pr = J->parent < 0 || reached[J->parent];
        if (pr && reached[J->child]) {
            if (J->type == MB_FREE) {
                mdiag_add(diag, MD_ERROR, "FREE_LOOP_JOINT", J->name, "remove the joint", "joint '%s' is a free joint between already connected bodies: it constrains nothing",
                          J->name);
                errors++;
            } else if (J->limited || J->stiffness > 0 || J->damping > 0 || J->coulomb > 0 || J->armature != 0) {
                mdiag_add(diag, MD_ERROR, "UNSUPPORTED", J->name, "move limits and passive elements to a tree joint of the loop",
                          "loop-closing joint '%s' cannot carry limits, springs, damping or friction yet", J->name);
                errors++;
            } else {
                m->joint_loop[j] = true;
                mdiag_add(diag, MD_INFO, "LOOP_CLOSURE", J->name, NULL,
                          "joint '%s' closes a kinematic loop and is enforced as a bilateral constraint with explicit multipliers", J->name);
            }
        } else {
            mdiag_add(diag, MD_ERROR, "JOINT_NOT_CONNECTED", J->name, "check parent and child: the parent must be connected to the world before the child",
                      "joint '%s' connects bodies that are not connected to the world", J->name);
            errors++;
        }
    }
    for (int b = 0; b < nb; b++)
        if (!reached[b]) {
            mdiag_add(diag, MD_ERROR, "BODY_NOT_CONNECTED", m->bodies[b].name,
                      "connect it with a joint; a body that moves freely needs an explicit free joint to the world",
                      "body '%s' is not connected to the world", m->bodies[b].name);
            errors++;
        }
    free(reached), free(assigned), free(jvalid);
    if (errors) {
        mb_model_free(m);
        return NULL;
    }

    /* flexible bodies */
    int nf = def->nflex;
    m->flex = calloc((size_t)(nf ? nf : 1), sizeof *m->flex);
    m->body_flex = malloc((size_t)(nb ? nb : 1) * sizeof(int));
    m->flex_qadr = calloc((size_t)(nf ? nf : 1), sizeof(int)), m->flex_vadr = calloc((size_t)(nf ? nf : 1), sizeof(int));
    m->flex_gadr = calloc((size_t)(nf ? nf : 1), sizeof(int));
    m->vcol_start = malloc((size_t)(nb ? nb : 1) * sizeof(int));
    if (!m->flex || !m->body_flex || !m->flex_qadr || !m->flex_vadr || !m->flex_gadr || !m->vcol_start) {
        mb_model_free(m);
        return NULL;
    }
    for (int b = 0; b < nb; b++) m->body_flex[b] = -1, m->vcol_start[b] = -1;
    for (int f = 0; f < nf; f++) {
        const MbFlexDef *F = &def->flex[f];
        const char *who = F->body >= 0 && F->body < nb ? m->bodies[F->body].name : "flexible body";
        if (F->body < 0 || F->body >= nb || m->body_flex[F->body] >= 0 || F->nmodes < 1 || F->nmodes > 512 || F->ninterfaces < 0) {
            mdiag_add(diag, MD_ERROR, "INVALID_FLEXIBLE_BODY", who, "one elastic model per body, with 1 to 512 coordinates", "elastic model %d is invalid", f);
            errors++;
            continue;
        }
        if (!flex_copy(&m->flex[f], F)) {
            mb_model_free(m);
            return NULL;
        }
        m->nflex = f + 1;
        m->body_flex[F->body] = f;
        bool bad = false;
        for (int k = 0; k < F->nmodes; k++) {
            bad |= !(F->omega2[k] > 0) || !isfinite(F->omega2[k]) || !(F->zeta[k] >= 0 && F->zeta[k] < 1) || !finite_all(F->ell + 6 * k, 6) || !finite_all(F->dJ + 9 * k, 9);
            const double *dj = F->dJ + 9 * k;
            double asym = fmax(fabs(dj[1] - dj[3]), fmax(fabs(dj[2] - dj[6]), fabs(dj[5] - dj[7]))), sc = 0;
            for (int i = 0; i < 9; i++) sc = fmax(sc, fabs(dj[i]));
            bad |= asym > 1e-9 * sc;
        }
        for (int i = 0; i < F->ninterfaces; i++)
            bad |= !finite_all(F->interfaces[i].point, 3) || !finite_all(F->interfaces[i].phi, 3 * F->nmodes) || !finite_all(F->interfaces[i].psi, 3 * F->nmodes);
        if (bad) {
            mdiag_add(diag, MD_ERROR, "INVALID_FLEXIBLE_BODY", who,
                      "frequencies squared must be positive, damping ratios in [0, 1), inertia changes symmetric and every value finite",
                      "the elastic model of '%s' has invalid data", who);
            errors++;
        }
    }
    for (int j = 0; j < nj; j++) {
        const MbJointDef *J = &m->joints[j];
        int fp = J->parent >= 0 ? m->body_flex[J->parent] : -1, fc = m->body_flex[J->child];
        if (m->joint_loop[j] && (fp >= 0 || fc >= 0)) {
            mdiag_add(diag, MD_ERROR, "FLEXIBLE_LOOP_UNSUPPORTED", J->name, "attach flexible bodies through tree joints only",
                      "loop-closing joint '%s' connects a flexible body, which this build does not support", J->name);
            errors++;
            continue;
        }
        if (m->joint_loop[j] || fp < 0) continue;
        const MbFlexDef *F = &m->flex[fp];
        if (J->parent_interface >= 0) {
            if (J->parent_interface >= F->ninterfaces) {
                mdiag_add(diag, MD_ERROR, "INVALID_INTERFACE", J->name, NULL, "joint '%s' names interface %d of '%s', which has %d interfaces", J->name, J->parent_interface,
                          m->bodies[J->parent].name, F->ninterfaces);
                errors++;
            } else {
                double d3[3];
                mv3_sub(d3, J->parent_frame.p, F->interfaces[J->parent_interface].point);
                if (mv3_norm(d3) > 1e-9 * fmax(1.0, mv3_norm(F->interfaces[J->parent_interface].point))) {
                    mdiag_add(diag, MD_ERROR, "INTERFACE_FRAME_MISMATCH", J->name, "the joint's parent frame origin must be the interface point",
                              "joint '%s' is %.3g m away from interface %d of '%s'", J->name, mv3_norm(d3), J->parent_interface, m->bodies[J->parent].name);
                    errors++;
                }
            }
        } else
            mdiag_add(diag, MD_WARNING, "FLEXIBLE_REFERENCE_ATTACHMENT", J->name, "attach the joint to an interface of the elastic model",
                      "joint '%s' is attached to the reference frame of flexible body '%s': the deformation does not move it", J->name, m->bodies[J->parent].name);
    }
    if (errors) {
        mb_model_free(m);
        return NULL;
    }

    /* coordinates */
    int nq = 0, nv = 0;
    for (int j = 0; j < nj; j++) m->joint_qadr[j] = m->joint_vadr[j] = -1;
    for (int i = 0; i < nb; i++) {
        int b = m->order[i], j = m->body_joint[b];
        m->joint_qadr[j] = nq, m->joint_vadr[j] = nv;
        nq += JOINT_NQ[m->joints[j].type], nv += JOINT_NV[m->joints[j].type];
    }
    m->njq = nq, m->njv = nv;
    for (int f = 0, g = 0; f < m->nflex; f++) { /* elastic coordinates follow the joint coordinates */
        m->flex_qadr[f] = nq, m->flex_vadr[f] = nv, m->flex_gadr[f] = g;
        nq += m->flex[f].nmodes, nv += m->flex[f].nmodes, g += m->flex[f].nmodes;
        m->nmodes_total = g;
    }
    for (int i = 0; i < nb; i++) { /* interface columns of the tree joints that ride on an interface */
        int b = m->order[i], p = m->body_parent[b];
        int fp = p >= 0 ? m->body_flex[p] : -1;
        if (fp >= 0 && m->joints[m->body_joint[b]].parent_interface >= 0) m->vcol_start[b] = m->nvcol, m->nvcol += m->flex[fp].nmodes;
    }
    m->nq = nq, m->nv = nv;
    m->dofs = calloc((size_t)(nv ? nv : 1), sizeof *m->dofs);
    if (!m->dofs) {
        mb_model_free(m);
        return NULL;
    }
    for (int i = 0; i < nb; i++) {
        int b = m->order[i], j = m->body_joint[b];
        const MbJointDef *J = &m->joints[j];
        int nvj = JOINT_NV[J->type];
        for (int k = 0; k < nvj; k++) {
            double SJ[6] = {0};
            bool ang = true;
            switch (J->type) {
            case MB_REVOLUTE: mv3_copy(SJ, J->axis); break;
            case MB_PRISMATIC: mv3_copy(SJ + 3, J->axis), ang = false; break;
            case MB_SPHERICAL: SJ[k] = 1; break;
            case MB_FREE:
                SJ[k] = 1;
                ang = k < 3;
                break;
            default: break;
            }
            MbDof *d = &m->dofs[m->joint_vadr[j] + k];
            d->body = b, d->joint = j, d->angular = ang;
            msv_motion_to_parent(d->S_local, &J->child_frame, SJ); /* child joint frame -> child body coordinates */
        }
    }
    for (int f = 0; f < m->nflex; f++)
        for (int k = 0; k < m->flex[f].nmodes; k++) {
            MbDof *d = &m->dofs[m->flex_vadr[f] + k];
            d->body = m->flex[f].body, d->joint = -1, d->angular = false;
        }

    /* limits */
    for (int j = 0; j < nj; j++)
        if (!m->joint_loop[j] && m->joints[j].limited) m->limit_joint[m->nlimits++] = j;

    /* couplings */
    for (int c = 0; c < nc; c++) {
        MbCouplingDef *C = &m->couplings[c];
        bool ok = C->follower >= 0 && C->follower < nj && C->driver >= 0 && C->driver < nj && C->follower != C->driver;
        if (ok) {
            const MbJointDef *F = &m->joints[C->follower], *D = &m->joints[C->driver];
            ok = !m->joint_loop[C->follower] && !m->joint_loop[C->driver] && (F->type == MB_REVOLUTE || F->type == MB_PRISMATIC) &&
                 (D->type == MB_REVOLUTE || D->type == MB_PRISMATIC);
        }
        if (!ok || !isfinite(C->ratio) || !isfinite(C->offset) || C->ratio == 0) {
            mdiag_add(diag, MD_ERROR, "INVALID_COUPLING", C->name, "couple two different revolute or prismatic tree joints with a finite non-zero ratio",
                      "coupling '%s' is invalid", C->name);
            errors++;
        } else if (m->joints[C->follower].motion == MB_ACTUATED) {
            mdiag_add(diag, MD_WARNING, "COUPLED_FOLLOWER_ACTUATED", C->name, NULL,
                      "the follower of coupling '%s' is also actuated: its input adds to the coupling reaction", C->name);
        }
    }
    for (int t = 0; t < nt; t++) {
        const MbTransmissionDef *T = &m->transmissions[t];
        bool ok = T->input >= 0 && T->input < nj && T->output >= 0 && T->output < nj && T->input != T->output && !m->joint_loop[T->input] &&
                  !m->joint_loop[T->output];
        if (ok) {
            MbJointType ti = m->joints[T->input].type, to = m->joints[T->output].type;
            ok = (ti == MB_REVOLUTE || ti == MB_PRISMATIC) && (to == MB_REVOLUTE || to == MB_PRISMATIC);
        }
        if (!ok || !isfinite(T->ratio) || T->ratio == 0 || !(T->stiffness > 0) || !(T->damping >= 0) || !(T->backlash >= 0) || !isfinite(T->stiffness) ||
            !isfinite(T->damping) || !isfinite(T->backlash)) {
            mdiag_add(diag, MD_ERROR, "INVALID_TRANSMISSION", T->name,
                      "connect two different revolute or prismatic tree joints with a non-zero ratio, stiffness > 0, damping >= 0 and backlash >= 0",
                      "transmission '%s' is invalid", T->name);
            errors++;
        }
    }
    if (errors) {
        mb_model_free(m);
        return NULL;
    }

    /* characteristic length */
    double L = m->opt.length_scale;
    if (!(L > 0)) {
        L = 0;
        for (int j = 0; j < nj; j++) L = fmax(L, fmax(mv3_norm(m->joints[j].parent_frame.p), mv3_norm(m->joints[j].child_frame.p)));
        for (int b = 0; b < nb; b++) L = fmax(L, mv3_norm(m->bodies[b].com));
        if (!(L > 0)) L = 1;
    }
    m->length_scale = L;
    return m;
}

/* ------------------------------------------------------------------------------------------------ state */

MbState *mb_state_new(const MbModel *m) {
    MbState *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->nq = m->nq, s->nv = m->nv, s->nlimits = m->nlimits;
    s->q = calloc((size_t)(m->nq ? m->nq : 1), sizeof *s->q);
    s->v = calloc((size_t)(m->nv ? m->nv : 1), sizeof *s->v);
    s->limit_state = calloc((size_t)(m->nlimits ? m->nlimits : 1), 1);
    if (!s->q || !s->v || !s->limit_state) {
        mb_state_free(s);
        return NULL;
    }
    return s;
}

void mb_state_free(MbState *s) {
    if (!s) return;
    free(s->q);
    free(s->v);
    free(s->limit_state);
    free(s);
}

void mb_state_copy(MbState *d, const MbState *s) {
    d->t = s->t;
    memcpy(d->aux, s->aux, sizeof d->aux);
    memcpy(d->q, s->q, (size_t)s->nq * sizeof *d->q);
    memcpy(d->v, s->v, (size_t)s->nv * sizeof *d->v);
    memcpy(d->limit_state, s->limit_state, (size_t)s->nlimits);
    d->ledger = s->ledger;
    d->energy0 = s->energy0;
    d->steps = s->steps, d->events = s->events;
}

bool mb_state_initial(const MbModel *m, MbState *s, MechDiag *diag) {
    bool ok = true;
    s->t = 0;
    memset(s->aux, 0, sizeof s->aux);
    memset(&s->ledger, 0, sizeof s->ledger);
    s->steps = s->events = 0;
    memset(s->limit_state, 0, (size_t)m->nlimits);
    for (int j = 0; j < m->njoints; j++) {
        if (m->joint_loop[j]) continue;
        const MbJointDef *J = &m->joints[j];
        int qa = m->joint_qadr[j], va = m->joint_vadr[j];
        memcpy(s->q + qa, J->q0, (size_t)JOINT_NQ[J->type] * sizeof *s->q);
        memcpy(s->v + va, J->v0, (size_t)JOINT_NV[J->type] * sizeof *s->v);
        int quat = J->type == MB_SPHERICAL ? qa : (J->type == MB_FREE ? qa + 3 : -1);
        if (quat >= 0) {
            double n = mq_normalize(s->q + quat);
            if (!(n > 0) || !isfinite(n)) {
                mdiag_add(diag, MD_ERROR, "INVALID_QUATERNION", J->name, NULL, "initial orientation of joint '%s' is not a valid quaternion", J->name);
                ok = false;
            } else if (fabs(n - 1) > 1e-9)
                mdiag_add(diag, MD_WARNING, "QUATERNION_NORMALISED", J->name, NULL, "initial quaternion of joint '%s' had norm %.9g and was normalised", J->name,
                          n);
        }
    }
    for (int k = m->njq; k < m->nq; k++) s->q[k] = 0; /* undeformed, at rest */
    for (int k = m->njv; k < m->nv; k++) s->v[k] = 0;
    if (!finite_all(s->q, m->nq) || !finite_all(s->v, m->nv)) {
        mdiag_add(diag, MD_ERROR, "NOT_FINITE", "", NULL, "initial state is not finite");
        ok = false;
    }
    return ok;
}

double mb_joint_q(const MbModel *m, const MbState *s, int j) { return m->joint_qadr[j] >= 0 ? s->q[m->joint_qadr[j]] : NAN; }
double mb_joint_v(const MbModel *m, const MbState *s, int j) { return m->joint_vadr[j] >= 0 ? s->v[m->joint_vadr[j]] : NAN; }

/* ------------------------------------------------------------------------------------------------ workspace */

typedef enum { ROW_LOOP = 0, ROW_COUPLING, ROW_LIMIT } RowKind;

struct MbSim {
    const MbModel *m;
    int nb, nv, nq, maxrows;
    MPose *T;       /* world pose per body */
    double *SW;     /* 6 per dof */
    double *SWv;    /* 6 per interface column (tree joints riding on an interface of a flexible parent) */
    double *ellw;   /* 6 per elastic coordinate: elastic momentum coupling ell in world coordinates */
    SpInertia *dIw; /* per elastic coordinate: spatial inertia change in world coordinates */
    double *V;      /* 6 per body: world twist */
    double *Ab;     /* 6 per body: bias acceleration (qacc = 0, no gravity) */
    double *Aw;     /* 6 per body: RNEA accelerations */
    SpInertia *I, *Ic;
    double *f;      /* 6 per body */
    double *ext;    /* 6 per body: external wrenches at the world origin */
    double *M, *L;
    double *C, *rhs, *tau_p, *afree, *delta, *tmpv, *tmpv2;
    double *bvec, *gvec; /* per-row scratch */
    double *tau_in;      /* nv: tau array plus force callback at the last evaluation */
    double auxr[MB_AUX_MAX], aux_stage[4][MB_AUX_MAX];
    double t_eval;       /* time passed to the force callback */
    /* constraint rows */
    int nrows;
    double *G, *beta, *res, *Phi;
    bool *row_linear;
    RowKind *row_kind;
    int *row_owner, *row_a, *row_b;
    signed char *row_sign;
    double *Gs, *sv, *Vs, *H, *Z, *Ar, *y, *lam, *u;
    int rank;
    MbConstraintInfo ci;
    /* integrator */
    double *theta, *rate[4], *acc[4], *qs, *vs;
    double pw[4][4];
    MbState *st_trial, *st_try, *st_base;
    char err[256];
};

MbSim *mb_sim_new(const MbModel *m) {
    MbSim *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->m = m;
    int nb = m->nbodies, nv = m->nv, nq = m->nq;
    s->nb = nb, s->nv = nv, s->nq = nq;
    int loops = 0;
    for (int j = 0; j < m->njoints; j++)
        if (m->joint_loop[j]) loops++;
    s->maxrows = 6 * loops + m->ncouplings + m->nlimits + 1;
    size_t NB = (size_t)(nb ? nb : 1), NV = (size_t)(nv ? nv : 1), NQ = (size_t)(nq ? nq : 1), MR = (size_t)s->maxrows;
    size_t RR = MR > NV ? MR : NV;
    bool ok = (s->T = calloc(NB, sizeof *s->T)) && (s->SW = calloc(6 * NV, sizeof(double))) && (s->V = calloc(6 * NB, sizeof(double))) &&
              (s->Ab = calloc(6 * NB, sizeof(double))) && (s->Aw = calloc(6 * NB, sizeof(double))) && (s->I = calloc(NB, sizeof *s->I)) &&
              (s->Ic = calloc(NB, sizeof *s->Ic)) && (s->f = calloc(6 * NB, sizeof(double))) && (s->ext = calloc(6 * NB, sizeof(double))) &&
              (s->M = calloc(NV * NV, sizeof(double))) && (s->L = calloc(NV * NV, sizeof(double))) && (s->C = calloc(NV, sizeof(double))) &&
              (s->rhs = calloc(NV, sizeof(double))) && (s->tau_p = calloc(NV, sizeof(double))) && (s->afree = calloc(NV, sizeof(double))) &&
              (s->delta = calloc(NV, sizeof(double))) && (s->tmpv = calloc(NV, sizeof(double))) && (s->tmpv2 = calloc(NV, sizeof(double))) &&
              (s->bvec = calloc(MR, sizeof(double))) && (s->gvec = calloc(MR, sizeof(double))) && (s->tau_in = calloc(NV, sizeof(double))) &&
              (s->G = calloc(MR * NV, sizeof(double))) && (s->beta = calloc(MR, sizeof(double))) && (s->res = calloc(MR, sizeof(double))) &&
              (s->Phi = calloc(6 * MR, sizeof(double))) && (s->row_linear = calloc(MR, sizeof(bool))) &&
              (s->row_kind = calloc(MR, sizeof(RowKind))) && (s->row_owner = calloc(MR, sizeof(int))) && (s->row_a = calloc(MR, sizeof(int))) &&
              (s->row_b = calloc(MR, sizeof(int))) && (s->row_sign = calloc(MR, 1)) && (s->Gs = calloc(MR * NV, sizeof(double))) &&
              (s->sv = calloc(RR, sizeof(double))) && (s->Vs = calloc(MR * MR, sizeof(double))) && (s->H = calloc(MR * MR, sizeof(double))) &&
              (s->Z = calloc(MR * NV, sizeof(double))) && (s->Ar = calloc(MR * MR, sizeof(double))) && (s->y = calloc(MR, sizeof(double))) &&
              (s->lam = calloc(MR, sizeof(double))) && (s->u = calloc(MR * (NV > 6 ? NV : 6), sizeof(double))) &&
              (s->theta = calloc(NV, sizeof(double))) && (s->qs = calloc(NQ, sizeof(double))) && (s->vs = calloc(NV, sizeof(double)));
    size_t NVC = (size_t)(m->nvcol ? m->nvcol : 1), NMT = (size_t)(m->nmodes_total ? m->nmodes_total : 1);
    ok = ok && (s->SWv = calloc(6 * NVC, sizeof(double))) && (s->ellw = calloc(6 * NMT, sizeof(double))) && (s->dIw = calloc(NMT, sizeof *s->dIw));
    for (int k = 0; ok && k < 4; k++) ok = (s->rate[k] = calloc(NV, sizeof(double))) && (s->acc[k] = calloc(NV, sizeof(double)));
    if (ok) ok = (s->st_trial = mb_state_new(m)) && (s->st_try = mb_state_new(m)) && (s->st_base = mb_state_new(m));
    if (!ok) {
        mb_sim_free(s);
        return NULL;
    }
    return s;
}

void mb_sim_free(MbSim *s) {
    if (!s) return;
    free(s->T), free(s->SW), free(s->V), free(s->Ab), free(s->Aw), free(s->I), free(s->Ic), free(s->f), free(s->ext);
    free(s->SWv), free(s->ellw), free(s->dIw);
    free(s->M), free(s->L), free(s->C), free(s->rhs), free(s->tau_p), free(s->afree), free(s->delta), free(s->tmpv), free(s->tmpv2);
    free(s->bvec), free(s->gvec), free(s->tau_in);
    free(s->G), free(s->beta), free(s->res), free(s->Phi), free(s->row_linear), free(s->row_kind), free(s->row_owner), free(s->row_a);
    free(s->row_b), free(s->row_sign), free(s->Gs), free(s->sv), free(s->Vs), free(s->H), free(s->Z), free(s->Ar), free(s->y), free(s->lam);
    free(s->u), free(s->theta), free(s->qs), free(s->vs);
    for (int k = 0; k < 4; k++) free(s->rate[k]), free(s->acc[k]);
    mb_state_free(s->st_trial), mb_state_free(s->st_try), mb_state_free(s->st_base);
    free(s);
}

const MbModel *mb_sim_model(const MbSim *sim) { return sim->m; }

/* ------------------------------------------------------------------------------------------------ kinematics */

static void joint_transform(const MbJointDef *J, const double *q, MPose *T) {
    switch (J->type) {
    case MB_REVOLUTE:
        mrot_axis_angle(T->R, J->axis, q[0]);
        mv3_set(T->p, 0, 0, 0);
        break;
    case MB_PRISMATIC:
        mm3_identity(T->R);
        mv3_scale(T->p, J->axis, q[0]);
        break;
    case MB_SPHERICAL:
        mq_to_mat(T->R, q);
        mv3_set(T->p, 0, 0, 0);
        break;
    case MB_FREE:
        mq_to_mat(T->R, q + 3);
        mv3_copy(T->p, q);
        break;
    default: mpose_identity(T); break;
    }
}

/* pose of an interface frame of a flexible body in body coordinates: rotation exp(psi eta) about the interface point, which
 * translates by phi eta */
static void flex_interface_pose(const MbFlexDef *F, int itf, const double *eta, MPose *D) {
    const MbFlexInterface *I = &F->interfaces[itf];
    double th[3] = {0, 0, 0}, u[3] = {0, 0, 0}, dq[4], Rp[3];
    for (int k = 0; k < F->nmodes; k++) mv3_addscaled(th, I->psi + 3 * k, eta[k]), mv3_addscaled(u, I->phi + 3 * k, eta[k]);
    mq_exp(dq, th);
    mq_to_mat(D->R, dq);
    mm3_mulv(Rp, D->R, I->point);
    for (int k = 0; k < 3; k++) D->p[k] = I->point[k] + u[k] - Rp[k];
}

/* spatial inertia of a flexible body at its deformation, and the world-coordinate elastic coupling terms */
static void flex_body_inertia(MbSim *sim, int b, const double *eta) {
    const MbModel *m = sim->m;
    const MbBodyDef *B = &m->bodies[b];
    const MbFlexDef *F = &m->flex[m->body_flex[b]];
    const MPose *T = &sim->T[b];
    int g0 = m->flex_gadr[m->body_flex[b]];
    double h[3], J[9], c0 = mv3_dot(B->com, B->com);
    mv3_scale(h, B->com, B->mass);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) J[3 * r + k] = B->inertia[3 * r + k] + B->mass * ((r == k ? c0 : 0) - B->com[r] * B->com[k]);
    for (int mm = 0; mm < F->nmodes; mm++) {
        mv3_addscaled(h, F->ell + 6 * mm + 3, eta[mm]);
        for (int i = 0; i < 9; i++) J[i] += eta[mm] * F->dJ[9 * mm + i];
        /* coupling ell = [rotational; translational] about the body origin -> world origin; inertia change -> world */
        double *lw = sim->ellw + 6 * (g0 + mm), lt[3], pxl[3];
        mm3_mulv(lt, T->R, F->ell + 6 * mm + 3);
        mm3_mulv(lw, T->R, F->ell + 6 * mm);
        mv3_cross(pxl, T->p, lt);
        mv3_addto(lw, pxl);
        mv3_copy(lw + 3, lt);
        SpInertia *dI = &sim->dIw[g0 + mm];
        dI->m = 0;
        mv3_copy(dI->h, lt);
        mm3_rotate_sym(dI->I, T->R, F->dJ + 9 * mm);
        double ph = mv3_dot(T->p, lt);
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) dI->I[3 * r + k] += (r == k ? 2 * ph : 0) - T->p[r] * lt[k] - lt[r] * T->p[k];
    }
    double c[3], Jc[9];
    mv3_scale(c, h, 1 / B->mass);
    double cc = mv3_dot(c, c);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) Jc[3 * r + k] = J[3 * r + k] - B->mass * ((r == k ? cc : 0) - c[r] * c[k]);
    mspi_from_body(&sim->I[b], B->mass, c, Jc, T);
}

/* poses, motion subspaces, twists, bias accelerations and spatial inertias in world coordinates */
static void kinematics(MbSim *sim, const double *q, const double *v) {
    const MbModel *m = sim->m;
    for (int i = 0; i < m->nbodies; i++) {
        int b = m->order[i], j = m->body_joint[b], p = m->body_parent[b];
        const MbJointDef *J = &m->joints[j];
        MPose Tj, t1, t2;
        joint_transform(J, q + m->joint_qadr[j], &Tj);
        int fp = p >= 0 ? m->body_flex[p] : -1, vc = m->vcol_start[b];
        if (p >= 0 && vc >= 0) { /* the joint rides on an interface of a flexible parent */
            MPose D, tmp;
            flex_interface_pose(&m->flex[fp], J->parent_interface, q + m->flex_qadr[fp], &D);
            mpose_mul(&tmp, &sim->T[p], &D);
            mpose_mul(&t1, &tmp, &J->parent_frame);
        } else if (p >= 0)
            mpose_mul(&t1, &sim->T[p], &J->parent_frame);
        else
            t1 = J->parent_frame;
        mpose_mul(&t2, &t1, &Tj);
        mpose_mul(&sim->T[b], &t2, &m->child_frame_inv[j]);
        double *Vb = sim->V + 6 * b, *Ab = sim->Ab + 6 * b, vj[6] = {0}, vi[6] = {0};
        if (p >= 0) {
            memcpy(Vb, sim->V + 6 * p, 6 * sizeof(double));
            memcpy(Ab, sim->Ab + 6 * p, 6 * sizeof(double));
        } else {
            memset(Vb, 0, 6 * sizeof(double));
            memset(Ab, 0, 6 * sizeof(double));
        }
        if (vc >= 0) { /* interface motion: [R psi; R phi - (R psi) x x_interface] per elastic coordinate of the parent */
            const MbFlexDef *F = &m->flex[fp];
            const MbFlexInterface *I = &F->interfaces[J->parent_interface];
            const double *eta = q + m->flex_qadr[fp];
            double xb[3], xw[3];
            mv3_copy(xb, I->point);
            for (int k = 0; k < F->nmodes; k++) mv3_addscaled(xb, I->phi + 3 * k, eta[k]);
            mpose_apply(xw, &sim->T[p], xb);
            for (int k = 0; k < F->nmodes; k++) {
                double *S = sim->SWv + 6 * (vc + k), lin[3], wxx[3];
                mm3_mulv(S, sim->T[p].R, I->psi + 3 * k);
                mm3_mulv(lin, sim->T[p].R, I->phi + 3 * k);
                mv3_cross(wxx, S, xw);
                mv3_sub(S + 3, lin, wxx);
                if (v)
                    for (int r = 0; r < 6; r++) vi[r] += S[r] * v[m->flex_vadr[fp] + k];
            }
        }
        int va = m->joint_vadr[j], nvj = JOINT_NV[J->type];
        for (int k = 0; k < nvj; k++) {
            double *S = sim->SW + 6 * (va + k);
            msv_motion_to_parent(S, &sim->T[b], m->dofs[va + k].S_local);
            if (v)
                for (int r = 0; r < 6; r++) vj[r] += S[r] * v[va + k];
        }
        double c[6];
        if (vc >= 0) { /* interface subspaces are fixed in the parent: bias V_parent x (S_i eta') */
            msv_cross_motion(c, Vb, vi);
            for (int r = 0; r < 6; r++) Ab[r] += c[r], Vb[r] += vi[r];
        }
        for (int r = 0; r < 6; r++) Vb[r] += vj[r];
        msv_cross_motion(c, Vb, vj);
        for (int r = 0; r < 6; r++) Ab[r] += c[r];
        if (m->body_flex[b] >= 0)
            flex_body_inertia(sim, b, q + m->flex_qadr[m->body_flex[b]]);
        else {
            const MbBodyDef *B = &m->bodies[b];
            mspi_from_body(&sim->I[b], B->mass, B->com, B->inertia, &sim->T[b]);
        }
    }
}

/* external wrenches given at the centres of mass (world) -> world origin */
static void external_wrenches(MbSim *sim, const MbInputs *in) {
    const MbModel *m = sim->m;
    memset(sim->ext, 0, (size_t)(6 * m->nbodies) * sizeof(double));
    if (!in || !in->body_wrench) return;
    for (int b = 0; b < m->nbodies; b++) {
        const double *w = in->body_wrench + 6 * b;
        double c[3], cf[3];
        mpose_apply(c, &sim->T[b], m->bodies[b].com);
        mv3_cross(cf, c, w + 3);
        mv3_add(sim->ext + 6 * b, w, cf);
        mv3_copy(sim->ext + 6 * b + 3, w + 3);
    }
}

/* recursive Newton-Euler over the tree after kinematics(): f[b] = wrench from parent on b (world, origin); tau per dof.
 * qacc NULL = zero. extra: additional external wrenches (6 per body, world origin), may be NULL. */
static void rnea(MbSim *sim, const double *v, const double *qacc, bool gravity, const double *extra, double *tau) {
    const MbModel *m = sim->m;
    for (int i = 0; i < m->nbodies; i++) {
        int b = m->order[i], j = m->body_joint[b], p = m->body_parent[b];
        double *a = sim->Aw + 6 * b;
        if (p >= 0)
            memcpy(a, sim->Aw + 6 * p, 6 * sizeof(double));
        else {
            memset(a, 0, 6 * sizeof(double));
            if (gravity) mv3_scale(a + 3, m->gravity, -1);
        }
        int va = m->joint_vadr[j], nvj = JOINT_NV[m->joints[j].type], vc = m->vcol_start[b];
        double vj[6] = {0}, c[6];
        if (vc >= 0) { /* interface columns of a flexible parent */
            int fp = m->body_flex[p], fv = m->flex_vadr[fp];
            double vi[6] = {0};
            for (int k = 0; k < m->flex[fp].nmodes; k++) {
                const double *S = sim->SWv + 6 * (vc + k);
                for (int r = 0; r < 6; r++) {
                    if (v) vi[r] += S[r] * v[fv + k];
                    if (qacc) a[r] += S[r] * qacc[fv + k];
                }
            }
            msv_cross_motion(c, sim->V + 6 * p, vi);
            for (int r = 0; r < 6; r++) a[r] += c[r];
        }
        for (int k = 0; k < nvj; k++) {
            const double *S = sim->SW + 6 * (va + k);
            for (int r = 0; r < 6; r++) {
                if (v) vj[r] += S[r] * v[va + k];
                if (qacc) a[r] += S[r] * qacc[va + k];
            }
        }
        msv_cross_motion(c, sim->V + 6 * b, vj);
        for (int r = 0; r < 6; r++) a[r] += c[r];
        double Ia[6], IV[6], vxIV[6];
        mspi_mul(Ia, &sim->I[b], a);
        mspi_mul(IV, &sim->I[b], sim->V + 6 * b);
        msv_cross_force(vxIV, sim->V + 6 * b, IV);
        for (int r = 0; r < 6; r++) sim->f[6 * b + r] = Ia[r] + vxIV[r] - sim->ext[6 * b + r] - (extra ? extra[6 * b + r] : 0);
        int fb = m->body_flex[b];
        if (fb >= 0) { /* rate of the elastic momentum ell eta' and of the inertia change */
            const MbFlexDef *F = &m->flex[fb];
            int fv = m->flex_vadr[fb], g0 = m->flex_gadr[fb];
            for (int k = 0; k < F->nmodes; k++) {
                const double *lw = sim->ellw + 6 * (g0 + k);
                double ed = v ? v[fv + k] : 0, ea = qacc ? qacc[fv + k] : 0, vxl[6], dIV[6];
                msv_cross_force(vxl, sim->V + 6 * b, lw);
                mspi_mul(dIV, &sim->dIw[g0 + k], sim->V + 6 * b);
                for (int r = 0; r < 6; r++) sim->f[6 * b + r] += lw[r] * ea + (vxl[r] + dIV[r]) * ed;
            }
        }
    }
    if (tau && m->nflex) memset(tau + m->njv, 0, (size_t)(m->nv - m->njv) * sizeof *tau);
    for (int i = m->nbodies - 1; i >= 0; i--) {
        int b = m->order[i], j = m->body_joint[b], p = m->body_parent[b];
        int va = m->joint_vadr[j], nvj = JOINT_NV[m->joints[j].type], vc = m->vcol_start[b];
        if (tau) {
            for (int k = 0; k < nvj; k++) tau[va + k] = msv_dot(sim->SW + 6 * (va + k), sim->f + 6 * b);
            int fb = m->body_flex[b];
            if (fb >= 0) { /* own elastic terms: eta'' + ell . a - 1/2 V dI V (first pass through this body) */
                const MbFlexDef *F = &m->flex[fb];
                int fv = m->flex_vadr[fb], g0 = m->flex_gadr[fb];
                for (int k = 0; k < F->nmodes; k++) {
                    double dIV[6];
                    mspi_mul(dIV, &sim->dIw[g0 + k], sim->V + 6 * b);
                    tau[fv + k] += (qacc ? qacc[fv + k] : 0) + msv_dot(sim->ellw + 6 * (g0 + k), sim->Aw + 6 * b) - 0.5 * msv_dot(sim->V + 6 * b, dIV);
                }
            }
            if (vc >= 0) {
                int fp = m->body_flex[p], fv = m->flex_vadr[fp];
                for (int k = 0; k < m->flex[fp].nmodes; k++) tau[fv + k] += msv_dot(sim->SWv + 6 * (vc + k), sim->f + 6 * b);
            }
        }
        if (p >= 0)
            for (int r = 0; r < 6; r++) sim->f[6 * p + r] += sim->f[6 * b + r];
    }
}

/* columns of a body: the motion subspaces of its tree joint, then its interface columns (elastic coordinates of the parent) */
static int body_columns(const MbSim *sim, int b, const double **S, int *idx) {
    const MbModel *m = sim->m;
    int j = m->body_joint[b], va = m->joint_vadr[j], nvj = JOINT_NV[m->joints[j].type], n = 0;
    for (int k = 0; k < nvj; k++) S[n] = sim->SW + 6 * (va + k), idx[n++] = va + k;
    int vc = m->vcol_start[b];
    if (vc >= 0) {
        int fp = m->body_flex[m->body_parent[b]];
        for (int k = 0; k < m->flex[fp].nmodes; k++) S[n] = sim->SWv + 6 * (vc + k), idx[n++] = m->flex_vadr[fp] + k;
    }
    return n;
}

static void crba(MbSim *sim) {
    const MbModel *m = sim->m;
    int nv = m->nv;
    for (int b = 0; b < m->nbodies; b++) sim->Ic[b] = sim->I[b];
    for (int i = m->nbodies - 1; i >= 0; i--) {
        int b = m->order[i], p = m->body_parent[b];
        if (p >= 0) mspi_add(&sim->Ic[p], &sim->Ic[b]);
    }
    memset(sim->M, 0, (size_t)nv * (size_t)nv * sizeof(double));
    int maxcol = 6;
    for (int f = 0; f < m->nflex; f++) maxcol = m->flex[f].nmodes + 6 > maxcol ? m->flex[f].nmodes + 6 : maxcol;
    const double **Sx = malloc((size_t)maxcol * sizeof *Sx), **Sy = malloc((size_t)maxcol * sizeof *Sy);
    int *ix = malloc((size_t)maxcol * sizeof(int)), *iy = malloc((size_t)maxcol * sizeof(int));
    if (!Sx || !Sy || !ix || !iy) {
        free(Sx), free(Sy), free(ix), free(iy);
        for (int i = 0; i < nv; i++) sim->M[i * nv + i] = NAN; /* reported by the factorisation */
        return;
    }
    for (int b = 0; b < m->nbodies; b++) {
        int nx = body_columns(sim, b, Sx, ix);
        for (int x = 0; x < nx; x++) {
            double F[6];
            mspi_mul(F, &sim->Ic[b], Sx[x]);
            for (int body = b; body >= 0; body = m->body_parent[body]) {
                int ny = body == b ? x + 1 : body_columns(sim, body, Sy, iy);
                for (int y = 0; y < ny; y++) {
                    const double *S = body == b ? Sx[y] : Sy[y];
                    int c = body == b ? ix[y] : iy[y];
                    double val = msv_dot(S, F);
                    sim->M[ix[x] * nv + c] += val;
                    if (c != ix[x]) sim->M[c * nv + ix[x]] += val;
                }
            }
        }
    }
    /* own elastic terms: unit modal mass and the coupling ell with the motion of the body's reference frame */
    for (int f = 0; f < m->nflex; f++) {
        const MbFlexDef *Fl = &m->flex[f];
        int fv = m->flex_vadr[f], g0 = m->flex_gadr[f];
        for (int k = 0; k < Fl->nmodes; k++) {
            int a = fv + k;
            sim->M[a * nv + a] += 1;
            const double *lw = sim->ellw + 6 * (g0 + k);
            for (int body = Fl->body; body >= 0; body = m->body_parent[body]) {
                int ny = body_columns(sim, body, Sy, iy);
                for (int y = 0; y < ny; y++) {
                    double val = msv_dot(Sy[y], lw);
                    sim->M[a * nv + iy[y]] += val;
                    sim->M[iy[y] * nv + a] += val;
                }
            }
        }
    }
    free(Sx), free(Sy), free(ix), free(iy);
    /* armature: reflected rotor inertia on the diagonal */
    for (int j = 0; j < m->njoints; j++)
        if (!m->joint_loop[j] && m->joints[j].armature > 0) sim->M[m->joint_vadr[j] * nv + m->joint_vadr[j]] += m->joints[j].armature;
}

static void passive_forces(const MbModel *m, const double *q, const double *v, double *tau, double *p_damp, double *p_fric) {
    memset(tau, 0, (size_t)m->nv * sizeof *tau);
    double pd = 0, pf = 0;
    for (int j = 0; j < m->njoints; j++) {
        const MbJointDef *J = &m->joints[j];
        if (m->joint_loop[j] || (J->type != MB_REVOLUTE && J->type != MB_PRISMATIC)) continue;
        int qa = m->joint_qadr[j], va = m->joint_vadr[j];
        double t = 0;
        if (J->stiffness > 0) t -= J->stiffness * (q[qa] - J->spring_reference);
        if (J->damping > 0) {
            double d = J->damping * v[va];
            t -= d;
            pd -= d * v[va];
        }
        if (J->coulomb > 0) {
            double fr = J->coulomb * tanh(v[va] / J->coulomb_vreg);
            t -= fr;
            pf -= fr * v[va];
        }
        tau[va] = t;
    }
    for (int f = 0; f < m->nflex; f++) { /* modal stiffness and damping */
        const MbFlexDef *F = &m->flex[f];
        int qa = m->flex_qadr[f], va = m->flex_vadr[f];
        for (int k = 0; k < F->nmodes; k++) {
            double c = 2 * F->zeta[k] * sqrt(F->omega2[k]), d = c * v[va + k];
            tau[va + k] = -F->omega2[k] * q[qa + k] - d;
            pd -= d * v[va + k];
        }
    }
    for (int k = 0; k < m->ntransmissions; k++) {
        const MbTransmissionDef *T = &m->transmissions[k];
        int qi = m->joint_qadr[T->input], qo = m->joint_qadr[T->output], vi = m->joint_vadr[T->input], vo = m->joint_vadr[T->output];
        double x = q[qi] / T->ratio - q[qo], half = 0.5 * T->backlash, dz = x > half ? x - half : (x < -half ? x + half : 0);
        double rel = v[vi] / T->ratio - v[vo];
        double tau_out = T->stiffness * dz + T->damping * rel;
        tau[vo] += tau_out;
        tau[vi] -= tau_out / T->ratio;
        pd -= T->damping * rel * rel;
    }
    if (p_damp) *p_damp = pd;
    if (p_fric) *p_fric = pf;
}

/* ------------------------------------------------------------------------------------------------ constraints */

static void loop_frames(MbSim *sim, const MbJointDef *J, MPose *Fa, MPose *Fb) {
    if (J->parent >= 0)
        mpose_mul(Fa, &sim->T[J->parent], &J->parent_frame);
    else
        *Fa = J->parent_frame;
    mpose_mul(Fb, &sim->T[J->child], &J->child_frame);
}

/* two unit vectors completing u to a right-handed orthonormal basis (u, p1, p2) */
static void perp_basis(const double *u, double *p1, double *p2) {
    double t[3] = {1, 0, 0};
    if (fabs(u[0]) > 0.6) mv3_set(t, 0, 1, 0);
    mv3_cross(p1, u, t);
    mv3_normalize(p1);
    mv3_cross(p2, u, p1);
}

static void body_twist(MbSim *sim, int b, double *out) {
    if (b >= 0)
        memcpy(out, sim->V + 6 * b, 6 * sizeof(double));
    else
        memset(out, 0, 6 * sizeof(double));
}

static void body_bias(MbSim *sim, int b, double *out) {
    if (b >= 0)
        memcpy(out, sim->Ab + 6 * b, 6 * sizeof(double));
    else
        memset(out, 0, 6 * sizeof(double));
}

/* adds sign * Phi . S over the dofs on the path from body b to the root */
static void add_path(MbSim *sim, int b, const double *Phi, double sign, double *Grow) {
    const MbModel *m = sim->m;
    for (int body = b; body >= 0; body = m->body_parent[body]) {
        int j = m->body_joint[body], va = m->joint_vadr[j], nvj = JOINT_NV[m->joints[j].type], vc = m->vcol_start[body];
        for (int k = 0; k < nvj; k++) Grow[va + k] += sign * msv_dot(Phi, sim->SW + 6 * (va + k));
        if (vc >= 0) {
            int fp = m->body_flex[m->body_parent[body]];
            for (int k = 0; k < m->flex[fp].nmodes; k++) Grow[m->flex_vadr[fp] + k] += sign * msv_dot(Phi, sim->SWv + 6 * (vc + k));
        }
    }
}

/* Builds the bilateral rows at the configuration of the last kinematics() call. limit_state selects resting limits
 * (NULL: none); extra_limits (NULL or per limit: 0 none, +-1 add a row) adds impacting limits. */
static int build_rows(MbSim *sim, const double *q, const signed char *limit_state, const signed char *extra_limits) {
    const MbModel *m = sim->m;
    int nv = m->nv, n = 0;
    memset(sim->G, 0, (size_t)sim->maxrows * (size_t)(nv ? nv : 1) * sizeof(double));
    for (int j = 0; j < m->njoints; j++) {
        if (!m->joint_loop[j]) continue;
        const MbJointDef *J = &m->joints[j];
        MPose Fa, Fb;
        loop_frames(sim, J, &Fa, &Fb);
        double Va[6], Vb[6], Aa[6], Abb[6], Vrel[6], Arel[6];
        body_twist(sim, J->parent, Va), body_twist(sim, J->child, Vb);
        body_bias(sim, J->parent, Aa), body_bias(sim, J->child, Abb);
        for (int r = 0; r < 6; r++) Vrel[r] = Vb[r] - Va[r], Arel[r] = Abb[r] - Aa[r];
        double Rrel[9], RaT[9], dp[3], dpa[3];
        mm3_transpose(RaT, Fa.R);
        mm3_mul(Rrel, RaT, Fb.R);
        mv3_sub(dp, Fb.p, Fa.p);
        mm3_mulv(dpa, RaT, dp);
        const double *o = Fb.p;
        double odot[3], wo[3];
        mv3_cross(wo, Vb, o);
        mv3_add(odot, Vb + 3, wo);
        /* constrained directions in F_a coordinates and their residuals */
        double dirs[6][3], resid[6];
        bool lin[6];
        int nr = 0;
        double logR[3];
        {
            double qrel[4];
            mq_from_mat(qrel, Rrel);
            mq_log(logR, qrel);
        }
        switch (J->type) {
        case MB_FIXED:
            for (int k = 0; k < 3; k++) {
                mv3_set(dirs[nr], k == 0, k == 1, k == 2), lin[nr] = false, resid[nr] = logR[k], nr++;
            }
            for (int k = 0; k < 3; k++) {
                mv3_set(dirs[nr], k == 0, k == 1, k == 2), lin[nr] = true, resid[nr] = dpa[k], nr++;
            }
            break;
        case MB_REVOLUTE: {
            double p1[3], p2[3], ub[3], cr[3];
            perp_basis(J->axis, p1, p2);
            mm3_mulv(ub, Rrel, J->axis);
            mv3_cross(cr, J->axis, ub);
            mv3_copy(dirs[nr], p1), lin[nr] = false, resid[nr] = mv3_dot(p1, cr), nr++;
            mv3_copy(dirs[nr], p2), lin[nr] = false, resid[nr] = mv3_dot(p2, cr), nr++;
            for (int k = 0; k < 3; k++) {
                mv3_set(dirs[nr], k == 0, k == 1, k == 2), lin[nr] = true, resid[nr] = dpa[k], nr++;
            }
            break;
        }
        case MB_PRISMATIC: {
            double p1[3], p2[3];
            perp_basis(J->axis, p1, p2);
            for (int k = 0; k < 3; k++) {
                mv3_set(dirs[nr], k == 0, k == 1, k == 2), lin[nr] = false, resid[nr] = logR[k], nr++;
            }
            mv3_copy(dirs[nr], p1), lin[nr] = true, resid[nr] = mv3_dot(p1, dpa), nr++;
            mv3_copy(dirs[nr], p2), lin[nr] = true, resid[nr] = mv3_dot(p2, dpa), nr++;
            break;
        }
        case MB_SPHERICAL:
            for (int k = 0; k < 3; k++) {
                mv3_set(dirs[nr], k == 0, k == 1, k == 2), lin[nr] = true, resid[nr] = dpa[k], nr++;
            }
            break;
        default: break;
        }
        for (int r = 0; r < nr; r++) {
            double e[3], ed[3], Phi[6], Phid[6];
            mm3_mulv(e, Fa.R, dirs[r]);
            mv3_cross(ed, Va, e); /* e rotates with body a */
            if (lin[r]) {
                double oxe[3], odxe[3], oxed[3];
                mv3_cross(oxe, o, e);
                mv3_copy(Phi, oxe), mv3_copy(Phi + 3, e);
                mv3_cross(odxe, odot, e);
                mv3_cross(oxed, o, ed);
                mv3_add(Phid, odxe, oxed), mv3_copy(Phid + 3, ed);
            } else {
                mv3_copy(Phi, e), mv3_set(Phi + 3, 0, 0, 0);
                mv3_copy(Phid, ed), mv3_set(Phid + 3, 0, 0, 0);
            }
            double *Grow = sim->G + n * nv;
            add_path(sim, J->child, Phi, 1, Grow);
            add_path(sim, J->parent, Phi, -1, Grow);
            sim->beta[n] = msv_dot(Phid, Vrel) + msv_dot(Phi, Arel);
            sim->res[n] = resid[r];
            memcpy(sim->Phi + 6 * n, Phi, sizeof Phi);
            sim->row_linear[n] = lin[r];
            sim->row_kind[n] = ROW_LOOP;
            sim->row_owner[n] = j;
            sim->row_a[n] = J->parent, sim->row_b[n] = J->child;
            sim->row_sign[n] = 0;
            n++;
        }
    }
    for (int c = 0; c < m->ncouplings; c++) {
        const MbCouplingDef *C = &m->couplings[c];
        double *Grow = sim->G + n * nv;
        Grow[m->joint_vadr[C->follower]] += 1;
        Grow[m->joint_vadr[C->driver]] -= C->ratio;
        sim->beta[n] = 0;
        sim->res[n] = q[m->joint_qadr[C->follower]] - C->ratio * q[m->joint_qadr[C->driver]] - C->offset;
        memset(sim->Phi + 6 * n, 0, 6 * sizeof(double));
        sim->row_linear[n] = m->joints[C->follower].type == MB_PRISMATIC;
        sim->row_kind[n] = ROW_COUPLING;
        sim->row_owner[n] = c;
        sim->row_sign[n] = 0;
        n++;
    }
    for (int l = 0; l < m->nlimits; l++) {
        int side = limit_state ? limit_state[l] : 0;
        if (!side && extra_limits) side = extra_limits[l];
        if (!side) continue;
        int j = m->limit_joint[l];
        const MbJointDef *J = &m->joints[j];
        double sgn = side < 0 ? 1.0 : -1.0; /* lower: q - lower >= 0; upper: upper - q >= 0 */
        double lim = side < 0 ? J->lower : J->upper;
        double *Grow = sim->G + n * nv;
        Grow[m->joint_vadr[j]] = sgn;
        sim->beta[n] = 0;
        sim->res[n] = sgn * (q[m->joint_qadr[j]] - lim);
        memset(sim->Phi + 6 * n, 0, 6 * sizeof(double));
        sim->row_linear[n] = J->type == MB_PRISMATIC;
        sim->row_kind[n] = ROW_LIMIT;
        sim->row_owner[n] = l;
        sim->row_sign[n] = (signed char)side;
        n++;
    }
    sim->nrows = n;
    return n;
}

/* scaled SVD of the rows: rank, H = Dr^-1 Vr, conditioning */
static void reduce_rows(MbSim *sim) {
    const MbModel *m = sim->m;
    int n = sim->nrows, nv = m->nv;
    double L = m->length_scale;
    MbConstraintInfo *ci = &sim->ci;
    memset(ci, 0, sizeof *ci);
    ci->rows = n;
    ci->sigma_min_ratio = 1;
    sim->rank = 0;
    if (n == 0) {
        ci->mobility = nv;
        return;
    }
    /* B = Gs^T (nv x n) */
    double *B = sim->Gs;
    for (int r = 0; r < n; r++) {
        double rs = sim->row_linear[r] ? L : 1;
        for (int c = 0; c < nv; c++) {
            double cs = m->dofs[c].angular ? 1 : L;
            B[c * n + r] = sim->G[r * nv + c] * cs / rs;
        }
    }
    if (nv == 0) {
        ci->redundant = n;
        return;
    }
    mm_svd_jacobi(B, nv, n, sim->sv, sim->Vs);
    double smax = sim->sv[0];
    int rank = 0;
    for (int i = 0; i < n && i < nv; i++)
        if (smax > 0 && sim->sv[i] > m->opt.rank_tol * smax) rank++;
    sim->rank = rank;
    ci->rank = rank;
    ci->redundant = n - rank;
    ci->mobility = nv - rank;
    ci->sigma_min_ratio = rank > 0 ? sim->sv[rank - 1] / smax : 1;
    for (int c = 0; c < rank; c++)
        for (int r = 0; r < n; r++) sim->H[r * rank + c] = sim->Vs[r * n + c] / (sim->row_linear[r] ? L : 1);
}

/* after crba()+chol and reduce_rows(): delta = M^-1 G^T H y with H^T G delta = H^T b; lambda = H y (rows). */
static bool solve_rows(MbSim *sim, const double *b, double *delta, double *lambda) {
    const MbModel *m = sim->m;
    int n = sim->nrows, nv = m->nv, r = sim->rank;
    memset(delta, 0, (size_t)nv * sizeof *delta);
    if (lambda) memset(lambda, 0, (size_t)n * sizeof *lambda);
    if (r == 0) return true;
    for (int i = 0; i < r; i++) {
        double *z = sim->Z + i * nv;
        memset(z, 0, (size_t)nv * sizeof *z);
        for (int row = 0; row < n; row++) {
            double h = sim->H[row * r + i];
            if (h == 0) continue;
            for (int c = 0; c < nv; c++) z[c] += sim->G[row * nv + c] * h;
        }
        memcpy(sim->u + i * nv, z, (size_t)nv * sizeof *z); /* w_i = G^T H_i */
        mm_chol_lower(sim->L, nv, z);                      /* z_i = L^-1 w_i */
    }
    for (int i = 0; i < r; i++) {
        double s = 0;
        for (int row = 0; row < n; row++) s += sim->H[row * r + i] * b[row];
        sim->y[i] = s;
        for (int k = 0; k <= i; k++) {
            double d = 0;
            for (int c = 0; c < nv; c++) d += sim->Z[i * nv + c] * sim->Z[k * nv + c];
            sim->Ar[i * r + k] = sim->Ar[k * r + i] = d;
        }
    }
    if (!mm_chol_factor(sim->Ar, r, 1e-14)) {
        snprintf(sim->err, sizeof sim->err, "reduced constraint matrix is not positive definite (rank %d of %d rows)", r, n);
        return false;
    }
    mm_chol_solve(sim->Ar, r, sim->y);
    for (int i = 0; i < r; i++)
        for (int c = 0; c < nv; c++) delta[c] += sim->u[i * nv + c] * sim->y[i];
    mm_chol_solve(sim->L, nv, delta);
    if (lambda)
        for (int row = 0; row < n; row++) {
            double s = 0;
            for (int i = 0; i < r; i++) s += sim->H[row * r + i] * sim->y[i];
            lambda[row] = s;
        }
    return true;
}

/* scaled norm of a row vector (positions or velocities) */
static double scaled_norm(MbSim *sim, const double *x) {
    double s = 0, L = sim->m->length_scale;
    for (int r = 0; r < sim->nrows; r++) {
        double v = x[r] / (sim->row_linear[r] ? L : 1);
        s += v * v;
    }
    return sqrt(s);
}

/* component of Dr^-1 beta outside the scaled row space */
static double inconsistency(MbSim *sim) {
    int n = sim->nrows, r = sim->rank;
    double L = sim->m->length_scale, tot = 0;
    for (int row = 0; row < n; row++) {
        double ur = sim->beta[row] / (sim->row_linear[row] ? L : 1);
        double proj = 0;
        for (int c = 0; c < r; c++) {
            double dot = 0;
            for (int k = 0; k < n; k++) dot += sim->Vs[k * n + c] * sim->beta[k] / (sim->row_linear[k] ? L : 1);
            proj += sim->Vs[row * n + c] * dot;
        }
        tot += (ur - proj) * (ur - proj);
    }
    return sqrt(tot);
}

static bool factor_mass(MbSim *sim) {
    int nv = sim->m->nv;
    crba(sim);
    memcpy(sim->L, sim->M, (size_t)nv * (size_t)nv * sizeof(double));
    if (!mm_chol_factor(sim->L, nv, 1e-15)) {
        snprintf(sim->err, sizeof sim->err, "mass matrix is not positive definite: a degree of freedom moves no mass or inertia");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------------------------------ dynamics */

/* forward dynamics at (q, v): qacc, multipliers in sim->lam, powers {inputs, external, damping, friction} */
static bool fdyn(MbSim *sim, const double *q, const double *v, const signed char *limit_state, const MbInputs *in, double *qacc,
                 double *powers) {
    const MbModel *m = sim->m;
    int nv = m->nv;
    kinematics(sim, q, v);
    external_wrenches(sim, in);
    rnea(sim, v, NULL, true, NULL, sim->C);
    double pd, pf, pcb = 0;
    passive_forces(m, q, v, sim->tau_p, &pd, &pf);
    for (int i = 0; i < nv; i++) sim->tau_in[i] = in && in->tau ? in->tau[i] : 0;
    memset(sim->auxr, 0, sizeof sim->auxr);
    if (in && in->force_fn) {
        memset(sim->tmpv2, 0, (size_t)nv * sizeof(double));
        pcb = in->force_fn(in->force_ctx, sim->t_eval, q, v, sim->tmpv2, sim->auxr);
        for (int i = 0; i < nv; i++) sim->tau_in[i] += sim->tmpv2[i];
    }
    for (int i = 0; i < nv; i++) sim->rhs[i] = sim->tau_in[i] + sim->tau_p[i] - sim->C[i];
    if (!factor_mass(sim)) return false;
    memcpy(sim->afree, sim->rhs, (size_t)nv * sizeof(double));
    mm_chol_solve(sim->L, nv, sim->afree);
    build_rows(sim, q, limit_state, NULL);
    if (sim->nrows) {
        reduce_rows(sim);
        double *bb = sim->bvec;
        for (int r = 0; r < sim->nrows; r++) {
            double g = 0;
            for (int c = 0; c < nv; c++) g += sim->G[r * nv + c] * sim->afree[c];
            bb[r] = -(sim->beta[r] + g);
        }
        sim->ci.inconsistency = inconsistency(sim);
        if (!solve_rows(sim, bb, sim->delta, sim->lam)) return false;
        for (int i = 0; i < nv; i++) qacc[i] = sim->afree[i] + sim->delta[i];
    } else {
        memset(&sim->ci, 0, sizeof sim->ci);
        sim->ci.mobility = nv;
        sim->ci.sigma_min_ratio = 1;
        memcpy(qacc, sim->afree, (size_t)nv * sizeof(double));
    }
    if (powers) {
        double pin = pcb, pext = 0;
        if (in && in->tau)
            for (int i = 0; i < nv; i++) pin += in->tau[i] * v[i];
        for (int b = 0; b < m->nbodies; b++) pext += msv_dot(sim->ext + 6 * b, sim->V + 6 * b);
        powers[0] = pin, powers[1] = pext, powers[2] = pd, powers[3] = pf;
    }
    for (int i = 0; i < nv; i++)
        if (!isfinite(qacc[i])) {
            snprintf(sim->err, sizeof sim->err, "non-finite acceleration");
            return false;
        }
    return true;
}

/* q <- q0 (+) dtheta, dtheta in tangent coordinates (free joints: [rotation; translation in parent joint frame]) */
static void retract(const MbModel *m, const double *q0, const double *d, double *q) {
    if (q != q0) memcpy(q, q0, (size_t)m->nq * sizeof *q);
    for (int j = 0; j < m->njoints; j++) {
        if (m->joint_loop[j]) continue;
        int qa = m->joint_qadr[j], va = m->joint_vadr[j];
        double dq[4], out[4];
        switch (m->joints[j].type) {
        case MB_REVOLUTE:
        case MB_PRISMATIC: q[qa] = q0[qa] + d[va]; break;
        case MB_SPHERICAL:
            mq_exp(dq, d + va);
            mq_mul(out, q0 + qa, dq);
            mq_normalize(out);
            memcpy(q + qa, out, sizeof out);
            break;
        case MB_FREE:
            for (int k = 0; k < 3; k++) q[qa + k] = q0[qa + k] + d[va + 3 + k];
            mq_exp(dq, d + va);
            mq_mul(out, q0 + qa + 3, dq);
            mq_normalize(out);
            memcpy(q + qa + 3, out, sizeof out);
            break;
        default: break;
        }
    }
    for (int k = 0; k < m->nv - m->njv; k++) q[m->njq + k] = q0[m->njq + k] + d[m->njv + k]; /* elastic coordinates */
}

/* d(theta)/dt at the stage configuration q (= q0 (+) theta) with velocity v */
static void tangent_rate(const MbModel *m, const double *q, const double *theta, const double *v, double *rate) {
    for (int j = 0; j < m->njoints; j++) {
        if (m->joint_loop[j]) continue;
        int qa = m->joint_qadr[j], va = m->joint_vadr[j];
        switch (m->joints[j].type) {
        case MB_REVOLUTE:
        case MB_PRISMATIC: rate[va] = v[va]; break;
        case MB_SPHERICAL: mso3_dexpinv(rate + va, theta + va, v + va); break;
        case MB_FREE: {
            double R[9];
            mso3_dexpinv(rate + va, theta + va, v + va);
            mq_to_mat(R, q + qa + 3);
            mm3_mulv(rate + va + 3, R, v + va + 3);
            break;
        }
        default: break;
        }
    }
    for (int k = m->njv; k < m->nv; k++) rate[k] = v[k];
}

/* velocity-space increment (free joints: [w; v] in child joint coordinates) -> tangent coordinates at q */
static void velocity_to_tangent(const MbModel *m, const double *q, const double *dv, double *dt) {
    memcpy(dt, dv, (size_t)m->nv * sizeof *dt);
    for (int j = 0; j < m->njoints; j++) {
        if (m->joint_loop[j] || m->joints[j].type != MB_FREE) continue;
        int qa = m->joint_qadr[j], va = m->joint_vadr[j];
        double R[9];
        mq_to_mat(R, q + qa + 3);
        mm3_mulv(dt + va + 3, R, dv + va + 3);
    }
}

static double transmission_energy(const MbModel *m, const double *q) {
    double e = 0;
    for (int k = 0; k < m->ntransmissions; k++) {
        const MbTransmissionDef *T = &m->transmissions[k];
        double x = q[m->joint_qadr[T->input]] / T->ratio - q[m->joint_qadr[T->output]], half = 0.5 * T->backlash;
        double dz = x > half ? x - half : (x < -half ? x + half : 0);
        e += 0.5 * T->stiffness * dz * dz;
    }
    return e;
}

static void energy(MbSim *sim, const double *q, const double *v, double *T, double *Ug, double *Us) {
    const MbModel *m = sim->m;
    kinematics(sim, q, v);
    double t = 0, ug = 0, us = 0;
    for (int b = 0; b < m->nbodies; b++) {
        double IV[6];
        mspi_mul(IV, &sim->I[b], sim->V + 6 * b);
        t += 0.5 * msv_dot(sim->V + 6 * b, IV);
        ug -= mv3_dot(m->gravity, sim->I[b].h);
    }
    for (int j = 0; j < m->njoints; j++) {
        const MbJointDef *J = &m->joints[j];
        if (m->joint_loop[j]) continue;
        if (J->armature > 0 && v) t += 0.5 * J->armature * v[m->joint_vadr[j]] * v[m->joint_vadr[j]];
        if (!(J->stiffness > 0)) continue;
        double d = q[m->joint_qadr[j]] - J->spring_reference;
        us += 0.5 * J->stiffness * d * d;
    }
    us += transmission_energy(m, q);
    for (int f = 0; f < m->nflex; f++) { /* elastic kinetic energy, coupling and strain energy */
        const MbFlexDef *F = &m->flex[f];
        int qa = m->flex_qadr[f], va = m->flex_vadr[f], g0 = m->flex_gadr[f];
        for (int k = 0; k < F->nmodes; k++) {
            if (v) t += v[va + k] * msv_dot(sim->V + 6 * F->body, sim->ellw + 6 * (g0 + k)) + 0.5 * v[va + k] * v[va + k];
            us += 0.5 * F->omega2[k] * q[qa + k] * q[qa + k];
        }
    }
    if (T) *T = t;
    if (Ug) *Ug = ug;
    if (Us) *Us = us;
}

static double total_energy(MbSim *sim, const double *q, const double *v) {
    double T, Ug, Us;
    energy(sim, q, v, &T, &Ug, &Us);
    return T + Ug + Us;
}

/* one RKMK4 step from s0 to out (no events, no projection) */
static bool rk4(MbSim *sim, const MbState *s0, double h, const MbInputs *in, MbState *out) {
    const MbModel *m = sim->m;
    int nv = m->nv;
    static const double C[4] = {0, 0.5, 0.5, 1}, W[4] = {1, 2, 2, 1};
    for (int k = 0; k < 4; k++) {
        if (k == 0) {
            memset(sim->theta, 0, (size_t)nv * sizeof(double));
            memcpy(sim->qs, s0->q, (size_t)m->nq * sizeof(double));
            memcpy(sim->vs, s0->v, (size_t)nv * sizeof(double));
        } else {
            for (int i = 0; i < nv; i++) {
                sim->theta[i] = C[k] * h * sim->rate[k - 1][i];
                sim->vs[i] = s0->v[i] + C[k] * h * sim->acc[k - 1][i];
            }
            retract(m, s0->q, sim->theta, sim->qs);
        }
        sim->t_eval = s0->t + C[k] * h;
        if (!fdyn(sim, sim->qs, sim->vs, s0->limit_state, in, sim->acc[k], sim->pw[k])) return false;
        memcpy(sim->aux_stage[k], sim->auxr, sizeof sim->auxr);
        tangent_rate(m, sim->qs, sim->theta, sim->vs, sim->rate[k]);
    }
    for (int i = 0; i < nv; i++) {
        double r = 0, a = 0;
        for (int k = 0; k < 4; k++) r += W[k] * sim->rate[k][i], a += W[k] * sim->acc[k][i];
        sim->theta[i] = h / 6 * r;
        out->v[i] = s0->v[i] + h / 6 * a;
    }
    retract(m, s0->q, sim->theta, out->q);
    out->t = s0->t + h;
    memcpy(out->limit_state, s0->limit_state, (size_t)m->nlimits);
    out->ledger = s0->ledger;
    out->energy0 = s0->energy0;
    out->steps = s0->steps, out->events = s0->events;
    double P[4] = {0};
    for (int k = 0; k < 4; k++)
        for (int c = 0; c < 4; c++) P[c] += W[k] * sim->pw[k][c];
    for (int i = 0; i < MB_AUX_MAX; i++)
        out->aux[i] = s0->aux[i] + h / 6 * (sim->aux_stage[0][i] + 2 * sim->aux_stage[1][i] + 2 * sim->aux_stage[2][i] + sim->aux_stage[3][i]);
    out->ledger.work_inputs += h / 6 * P[0];
    out->ledger.work_external += h / 6 * P[1];
    out->ledger.dissipated_damping -= h / 6 * P[2];
    out->ledger.dissipated_friction -= h / 6 * P[3];
    return true;
}

/* smallest signed distance to a limit that is not resting (scaled); limit index in *which */
static double limit_distance(const MbModel *m, const MbState *s, int *which) {
    double dmin = INFINITY;
    int w = -1;
    for (int l = 0; l < m->nlimits; l++) {
        if (s->limit_state[l]) continue;
        const MbJointDef *J = &m->joints[m->limit_joint[l]];
        double q = s->q[m->joint_qadr[m->limit_joint[l]]];
        double sc = J->type == MB_PRISMATIC ? m->length_scale : 1;
        double d = fmin(q - J->lower, J->upper - q) / sc;
        if (d < dmin) dmin = d, w = l;
    }
    if (which) *which = w;
    return dmin;
}

/* releases resting limits whose multiplier pulls (repeats until consistent) */
static bool update_resting(MbSim *sim, MbState *s, const MbInputs *in) {
    const MbModel *m = sim->m;
    for (int iter = 0; iter <= m->nlimits; iter++) {
        bool any = false;
        for (int l = 0; l < m->nlimits; l++) any |= s->limit_state[l] != 0;
        if (!any) return true;
        sim->t_eval = s->t;
        if (!fdyn(sim, s->q, s->v, s->limit_state, in, sim->tmpv, NULL)) return false;
        bool released = false;
        double scale = 0;
        for (int r = 0; r < sim->nrows; r++) scale = fmax(scale, fabs(sim->lam[r]));
        for (int r = 0; r < sim->nrows; r++)
            if (sim->row_kind[r] == ROW_LIMIT && sim->lam[r] < -1e-12 * fmax(scale, 1e-300)) {
                int l = sim->row_owner[r];
                double v = s->v[m->joint_vadr[m->limit_joint[l]]];
                if (s->limit_state[l] * v <= 0) { /* not moving into the limit */
                    s->limit_state[l] = 0;
                    released = true;
                }
            }
        if (!released) return true;
    }
    return true;
}

/* Newton impact law for limits reached at the current state */
static bool resolve_impacts(MbSim *sim, MbState *s, int *nimpacts) {
    const MbModel *m = sim->m;
    int nv = m->nv;
    signed char extra[m->nlimits > 0 ? m->nlimits : 1];
    memset(extra, 0, sizeof extra);
    double target_speed[m->nlimits > 0 ? m->nlimits : 1];
    int count = 0;
    for (int l = 0; l < m->nlimits; l++) {
        if (s->limit_state[l]) continue;
        int j = m->limit_joint[l];
        const MbJointDef *J = &m->joints[j];
        double q = s->q[m->joint_qadr[j]], v = s->v[m->joint_vadr[j]];
        double sc = J->type == MB_PRISMATIC ? m->length_scale : 1;
        double tol = m->opt.event_tol * 10;
        if ((q - J->lower) / sc <= tol && v <= 0) {
            extra[l] = -1;
            target_speed[l] = -J->restitution * v; /* separation speed along +q */
            count++;
        } else if ((J->upper - q) / sc <= tol && v >= 0) {
            extra[l] = 1;
            target_speed[l] = J->restitution * v; /* separation speed along -q */
            count++;
        }
    }
    *nimpacts = count;
    if (!count) return true;
    double E_before = total_energy(sim, s->q, s->v);
    for (int attempt = 0; attempt <= m->nlimits; attempt++) {
        kinematics(sim, s->q, s->v);
        if (!factor_mass(sim)) return false;
        build_rows(sim, s->q, s->limit_state, extra);
        reduce_rows(sim);
        double *b = sim->bvec;
        for (int r = 0; r < sim->nrows; r++) {
            double g = 0;
            for (int c = 0; c < nv; c++) g += sim->G[r * nv + c] * s->v[c];
            double target = 0;
            if (sim->row_kind[r] == ROW_LIMIT && !s->limit_state[sim->row_owner[r]]) {
                int l = sim->row_owner[r];
                double sc = m->joints[m->limit_joint[l]].type == MB_PRISMATIC ? m->length_scale : 1;
                double sep = target_speed[l];
                target = sep / sc < m->opt.rest_velocity ? 0 : sep;
            }
            b[r] = target - g;
        }
        if (!solve_rows(sim, b, sim->delta, sim->lam)) return false;
        bool dropped = false;
        for (int r = 0; r < sim->nrows; r++)
            if (sim->row_kind[r] == ROW_LIMIT && sim->lam[r] < 0) {
                int l = sim->row_owner[r];
                if (extra[l]) extra[l] = 0;
                else s->limit_state[l] = 0;
                dropped = true;
            }
        if (dropped) continue;
        for (int c = 0; c < nv; c++) s->v[c] += sim->delta[c];
        for (int l = 0; l < m->nlimits; l++)
            if (extra[l]) {
                double sc = m->joints[m->limit_joint[l]].type == MB_PRISMATIC ? m->length_scale : 1;
                if (target_speed[l] / sc < m->opt.rest_velocity) s->limit_state[l] = extra[l];
            }
        break;
    }
    double E_after = total_energy(sim, s->q, s->v);
    s->ledger.dissipated_impacts += E_before - E_after;
    return true;
}

/* position and velocity projection onto the bilateral constraints */
static bool project(MbSim *sim, MbState *s, MbStepReport *rep, bool positions, bool velocities) {
    const MbModel *m = sim->m;
    int nv = m->nv;
    kinematics(sim, s->q, s->v);
    if (build_rows(sim, s->q, s->limit_state, NULL) == 0) return true;
    double E_before = total_energy(sim, s->q, s->v);
    double drift = scaled_norm(sim, sim->res);
    if (rep) rep->drift_before = drift, rep->drift_after = drift;
    int it = 0;
    if (positions) {
        while (drift > m->opt.projection_tol && it < m->opt.projection_max_iter) {
            if (!factor_mass(sim)) return false;
            reduce_rows(sim);
            double *b = sim->bvec;
            for (int r = 0; r < sim->nrows; r++) b[r] = -sim->res[r];
            if (!solve_rows(sim, b, sim->delta, NULL)) return false;
            velocity_to_tangent(m, s->q, sim->delta, sim->tmpv);
            retract(m, s->q, sim->tmpv, s->q);
            kinematics(sim, s->q, s->v);
            build_rows(sim, s->q, s->limit_state, NULL);
            drift = scaled_norm(sim, sim->res);
            it++;
        }
        if (rep) rep->drift_after = drift, rep->projection_iterations = it;
        if (drift > m->opt.projection_tol * 1e3) {
            snprintf(sim->err, sizeof sim->err, "constraint projection did not converge: scaled residual %.3g after %d iterations", drift, it);
            return false;
        }
    }
    if (velocities) {
        kinematics(sim, s->q, s->v);
        build_rows(sim, s->q, s->limit_state, NULL);
        double *g = sim->gvec;
        for (int r = 0; r < sim->nrows; r++) {
            double x = 0;
            for (int c = 0; c < nv; c++) x += sim->G[r * nv + c] * s->v[c];
            g[r] = x;
        }
        if (rep) rep->vdrift_before = scaled_norm(sim, g);
        if (!factor_mass(sim)) return false;
        reduce_rows(sim);
        for (int r = 0; r < sim->nrows; r++) g[r] = -g[r];
        if (!solve_rows(sim, g, sim->delta, NULL)) return false;
        for (int c = 0; c < nv; c++) s->v[c] += sim->delta[c];
        for (int r = 0; r < sim->nrows; r++) {
            double x = 0;
            for (int c = 0; c < nv; c++) x += sim->G[r * nv + c] * s->v[c];
            g[r] = x;
        }
        if (rep) rep->vdrift_after = scaled_norm(sim, g);
    }
    s->ledger.projection_change += total_energy(sim, s->q, s->v) - E_before;
    return true;
}

bool mb_step(MbSim *sim, MbState *s, double h, const MbInputs *in, MbStepReport *rep) {
    MbStepReport local;
    if (!rep) rep = &local;
    memset(rep, 0, sizeof *rep);
    if (!sim || !s) {
        snprintf(rep->error, sizeof rep->error, "no simulation");
        return false;
    }
    const MbModel *m = sim->m;
    sim->err[0] = 0;
    if (!(h > 0) || !isfinite(h)) {
        snprintf(rep->error, sizeof rep->error, "time step must be positive and finite");
        return false;
    }
    double remaining = h;
    int events = 0;
    MbState *trial = sim->st_trial, *tryst = sim->st_try, *base = sim->st_base;
    while (remaining > 0) {
        if (!update_resting(sim, s, in)) goto fail;
        if (!rk4(sim, s, remaining, in, trial)) goto fail;
        double dend = limit_distance(m, trial, NULL);
        if (!(dend < -m->opt.event_tol)) {
            mb_state_copy(s, trial);
            remaining = 0;
            break;
        }
        /* locate the first crossing by safeguarded false position on the step fraction */
        mb_state_copy(base, s);
        double lo = 0, hi = 1, dlo = limit_distance(m, s, NULL), dhi = dend, phi = 1;
        if (dlo < 0) dlo = 0;
        int side = 0;
        bool found = false;
        for (int it = 0; it < 100; it++) {
            phi = hi - dhi * (hi - lo) / (dhi - dlo);
            if (!(phi > lo && phi < hi)) phi = 0.5 * (lo + hi);
            if (!rk4(sim, base, phi * remaining, in, tryst)) goto fail;
            double d = limit_distance(m, tryst, NULL);
            if (fabs(d) <= m->opt.event_tol || (hi - lo) * remaining <= 1e-15 * fmax(1.0, fabs(s->t))) {
                found = true;
                break;
            }
            if (d < 0) {
                hi = phi, dhi = d;
                if (side == -1) dlo *= 0.5;
                side = -1;
            } else {
                lo = phi, dlo = d;
                if (side == 1) dhi *= 0.5;
                side = 1;
            }
        }
        if (!found) {
            snprintf(sim->err, sizeof sim->err, "could not locate a joint-limit event within the step");
            goto fail;
        }
        mb_state_copy(s, tryst);
        remaining -= phi * remaining;
        if (remaining < 1e-14 * h) remaining = 0;
        int nimp = 0;
        if (!resolve_impacts(sim, s, &nimp)) goto fail;
        events += nimp ? nimp : 1;
        if (events > m->opt.max_events_per_step) {
            /* chattering: make every limit in contact persistent (a plastic impact) and report it */
            for (int l = 0; l < m->nlimits; l++) {
                int j = m->limit_joint[l];
                const MbJointDef *J = &m->joints[j];
                double q = s->q[m->joint_qadr[j]], sc = J->type == MB_PRISMATIC ? m->length_scale : 1;
                if ((q - J->lower) / sc <= 10 * m->opt.event_tol) s->limit_state[l] = -1;
                if ((J->upper - q) / sc <= 10 * m->opt.event_tol) s->limit_state[l] = 1;
            }
            snprintf(rep->error, sizeof rep->error, "more than %d limit events in one step: contacts made persistent", m->opt.max_events_per_step);
        }
    }
    s->events += (uint64_t)events;
    s->steps++;
    rep->events = events;
    if (!project(sim, s, rep, m->opt.project_positions, m->opt.project_velocities)) goto fail;
    rep->cinfo = sim->ci;
    return true;
fail:
    snprintf(rep->error, sizeof rep->error, "%s", sim->err[0] ? sim->err : "step failed");
    return false;
}

/* ------------------------------------------------------------------------------------------------ assembly */

static uint64_t lcg_next(uint64_t *x) {
    *x = *x * 6364136223846793005ULL + 1442695040888963407ULL;
    return *x >> 11;
}

bool mb_assemble(MbSim *sim, MbState *s, MechDiag *diag) {
    if (!sim || !s) return false;
    const MbModel *m = sim->m;
    int nv = m->nv;
    bool ok = true;
    sim->err[0] = 0;
    /* limits of the given configuration: never clamped silently */
    for (int l = 0; l < m->nlimits; l++) {
        int j = m->limit_joint[l];
        const MbJointDef *J = &m->joints[j];
        double q = s->q[m->joint_qadr[j]], sc = J->type == MB_PRISMATIC ? m->length_scale : 1;
        double unit = J->type == MB_PRISMATIC ? 1e3 : 180 / M_PI;
        const char *un = J->type == MB_PRISMATIC ? "mm" : "deg";
        if ((q - J->lower) / sc < -m->opt.assembly_tol || (J->upper - q) / sc < -m->opt.assembly_tol) {
            mdiag_add(diag, MD_ERROR, "IMPOSSIBLE_INITIAL_CONFIGURATION", J->name, "choose an initial position inside the limits",
                      "joint '%s' starts at %.6g %s, outside its limits [%.6g, %.6g] %s", J->name, q * unit, un, J->lower * unit, J->upper * unit, un);
            ok = false;
        } else if ((q - J->lower) / sc <= m->opt.assembly_tol)
            s->limit_state[l] = -1;
        else if ((J->upper - q) / sc <= m->opt.assembly_tol)
            s->limit_state[l] = 1;
    }
    if (!ok) return false;
    kinematics(sim, s->q, s->v);
    if (!factor_mass(sim)) {
        mdiag_add(diag, MD_ERROR, "MASS_MATRIX_SINGULAR", "", "every degree of freedom must move some mass or rotational inertia",
                  "%s", sim->err);
        return false;
    }
    int rows = build_rows(sim, s->q, s->limit_state, NULL);
    if (rows) {
        double r0 = scaled_norm(sim, sim->res);
        if (r0 > m->opt.assembly_tol) {
            double *q_before = malloc((size_t)m->nq * sizeof(double));
            if (!q_before) return false;
            memcpy(q_before, s->q, (size_t)m->nq * sizeof(double));
            MbStepReport rep;
            memset(&rep, 0, sizeof rep);
            bool pok = project(sim, s, &rep, true, false);
            if (!pok || rep.drift_after > m->opt.projection_tol * 10) {
                mdiag_add(diag, MD_ERROR, "LOOP_CLOSURE_FAILED", "",
                          "check link lengths, joint axes and frames: the loop cannot be closed near the given configuration",
                          "closed loops cannot be satisfied: scaled residual %.3g before and %.3g after %d projection iterations (length scale %.4g m)", r0,
                          rep.drift_after, rep.projection_iterations, m->length_scale);
                free(q_before);
                return false;
            }
            char changes[400] = "";
            size_t used = 0;
            for (int j = 0; j < m->njoints; j++) {
                const MbJointDef *J = &m->joints[j];
                if (m->joint_loop[j] || (J->type != MB_REVOLUTE && J->type != MB_PRISMATIC)) continue;
                int qa = m->joint_qadr[j];
                double dq = s->q[qa] - q_before[qa];
                if (fabs(dq) > 1e-12 && used < sizeof changes - 60)
                    used += (size_t)snprintf(changes + used, sizeof changes - used, "%s%s %+.4g %s", used ? ", " : "", J->name,
                                             J->type == MB_PRISMATIC ? dq * 1e3 : dq * 180 / M_PI, J->type == MB_PRISMATIC ? "mm" : "deg");
            }
            mdiag_add(diag, MD_WARNING, "INITIAL_CONFIGURATION_ADJUSTED", "", "give an initial configuration that closes the loops to avoid the adjustment",
                      "initial configuration did not satisfy the constraints (scaled residual %.3g); moved to the nearest mass-weighted solution: %s", r0,
                      used ? changes : "orientation/position coordinates only");
            free(q_before);
            for (int l = 0; l < m->nlimits; l++) {
                int j = m->limit_joint[l];
                const MbJointDef *J = &m->joints[j];
                double q = s->q[m->joint_qadr[j]];
                if (q < J->lower - 1e-12 || q > J->upper + 1e-12) {
                    mdiag_add(diag, MD_ERROR, "IMPOSSIBLE_INITIAL_CONFIGURATION", J->name, NULL,
                              "closing the loops moves joint '%s' outside its limits", J->name);
                    ok = false;
                }
            }
            if (!ok) return false;
        }
        /* velocities consistent with the constraints */
        kinematics(sim, s->q, s->v);
        build_rows(sim, s->q, s->limit_state, NULL);
        double *g = sim->gvec;
        for (int r = 0; r < sim->nrows; r++) {
            double x = 0;
            for (int c = 0; c < nv; c++) x += sim->G[r * nv + c] * s->v[c];
            g[r] = x;
        }
        double vr = scaled_norm(sim, g);
        if (vr > m->opt.assembly_tol) {
            MbStepReport rep;
            memset(&rep, 0, sizeof rep);
            bool pok = project(sim, s, &rep, false, true);
            if (!pok) {
                mdiag_add(diag, MD_ERROR, "VELOCITY_PROJECTION_FAILED", "", NULL, "%s", sim->err);
                return false;
            }
            mdiag_add(diag, MD_WARNING, "INITIAL_VELOCITY_ADJUSTED", "", NULL,
                      "initial velocities violated the constraints (scaled residual %.3g); the kinetic-energy-minimal consistent velocity is used", vr);
        }
        /* rank analysis at the assembled configuration (bilateral rows without resting limits) */
        kinematics(sim, s->q, s->v);
        factor_mass(sim);
        build_rows(sim, s->q, NULL, NULL);
        if (sim->nrows) {
            reduce_rows(sim);
            MbConstraintInfo ci = sim->ci;
            ci.inconsistency = inconsistency(sim);
            if (ci.redundant > 0) {
                /* rows taking part in the dependent directions */
                char who[400] = "";
                size_t used = 0;
                int n = sim->nrows;
                for (int r = 0; r < n; r++) {
                    double w = 0;
                    for (int c = sim->rank; c < n; c++) w = fmax(w, fabs(sim->Vs[r * n + c]));
                    if (w > 0.1 && used < sizeof who - 80) {
                        const char *owner = sim->row_kind[r] == ROW_LOOP ? m->joints[sim->row_owner[r]].name
                                                                          : (sim->row_kind[r] == ROW_COUPLING ? m->couplings[sim->row_owner[r]].name : "limit");
                        used += (size_t)snprintf(who + used, sizeof who - used, "%s%s %s row %d", used ? ", " : "", owner,
                                                 sim->row_linear[r] ? "force" : "moment", r);
                    }
                }
                mdiag_add(diag, MD_WARNING, "REDUNDANT_CONSTRAINTS", "",
                          "reactions along these directions are statically indeterminate for rigid bodies: the minimum-norm solution is reported. Use "
                          "joints that do not over-constrain the mechanism (for example a spherical joint in a planar loop) or model the compliance",
                          "%d of %d constraint rows are redundant at the assembled configuration (%s)", ci.redundant, ci.rows, who);
            }
            if (ci.mobility == 0)
                mdiag_add(diag, MD_INFO, "NO_MOBILITY", "", NULL, "the constraints leave no degree of freedom: the assembly is a rigid structure");
            if (ci.inconsistency > 1e-8)
                mdiag_add(diag, MD_ERROR, "INCONSISTENT_CONSTRAINTS", "", "check redundant loops and couplings for conflicting requirements",
                          "redundant constraints demand incompatible accelerations (scaled mismatch %.3g)", ci.inconsistency);
            /* singularity: rank here versus nearby assembled configurations */
            int generic = ci.rank;
            if (ci.rank > 0 || ci.rows > 0) {
                MbState *tmp = sim->st_base;
                uint64_t seed = 0x9E3779B97F4A7C15ULL;
                for (int sample = 0; sample < 4; sample++) {
                    mb_state_copy(tmp, s);
                    memset(tmp->limit_state, 0, (size_t)m->nlimits);
                    for (int c = 0; c < nv; c++) {
                        double u = (double)lcg_next(&seed) / 9007199254740992.0 * 2 - 1;
                        sim->tmpv[c] = 1e-2 * u * (m->dofs[c].angular ? 1 : m->length_scale);
                    }
                    velocity_to_tangent(m, tmp->q, sim->tmpv, sim->tmpv2);
                    retract(m, tmp->q, sim->tmpv2, tmp->q);
                    MbStepReport rep;
                    memset(&rep, 0, sizeof rep);
                    bool pok = project(sim, tmp, &rep, true, false);
                    if (!pok || rep.drift_after > m->opt.projection_tol * 10) continue;
                    kinematics(sim, tmp->q, tmp->v);
                    build_rows(sim, tmp->q, NULL, NULL);
                    reduce_rows(sim);
                    if (sim->rank > generic) generic = sim->rank;
                }
            }
            if (generic > ci.rank)
                mdiag_add(diag, MD_WARNING, "SINGULAR_CONFIGURATION", "",
                          "the mechanism is at a singular (for example toggle or change-point) position: reactions can be unbounded and the motion "
                          "branch is ambiguous; move away from it",
                          "constraint rank is %d here but %d at nearby assembled configurations", ci.rank, generic);
            else if (ci.rank > 0 && ci.sigma_min_ratio < m->opt.singular_warn)
                mdiag_add(diag, MD_WARNING, "NEAR_SINGULAR_CONFIGURATION", "", NULL,
                          "smallest scaled constraint singular value ratio is %.3g: close to a singular configuration", ci.sigma_min_ratio);
            mdiag_add(diag, MD_INFO, "CONSTRAINT_SUMMARY", "", NULL,
                      "%d constraint rows, rank %d, %d redundant, mobility %d of %d coordinates, smallest singular value ratio %.3g, length scale %.4g m",
                      ci.rows, ci.rank, ci.redundant, ci.mobility, nv, ci.sigma_min_ratio, m->length_scale);
            sim->ci = ci;
        }
    }
    s->energy0 = total_energy(sim, s->q, s->v);
    memset(&s->ledger, 0, sizeof s->ledger);
    return ok;
}

/* ------------------------------------------------------------------------------------------------ queries */

MbEval *mb_eval_new(const MbModel *m) {
    MbEval *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    size_t NV = (size_t)(m->nv ? m->nv : 1), NB = (size_t)(m->nbodies ? m->nbodies : 1), NJ = (size_t)(m->njoints ? m->njoints : 1);
    if (!(e->qacc = calloc(NV, sizeof(double))) || !(e->tau_passive = calloc(NV, sizeof(double))) || !(e->tau_constraint = calloc(NV, sizeof(double))) ||
        !(e->tau_input = calloc(NV, sizeof(double))) ||
        !(e->joint_wrench = calloc(6 * NJ, sizeof(double))) || !(e->body_pose = calloc(12 * NB, sizeof(double))) ||
        !(e->body_twist = calloc(6 * NB, sizeof(double)))) {
        mb_eval_free(e);
        return NULL;
    }
    return e;
}

void mb_eval_free(MbEval *e) {
    if (!e) return;
    free(e->qacc), free(e->tau_passive), free(e->tau_constraint), free(e->tau_input), free(e->joint_wrench), free(e->body_pose), free(e->body_twist);
    free(e);
}

bool mb_evaluate(MbSim *sim, const MbState *s, const MbInputs *in, MbEval *out) {
    if (!sim || !s || !out) return false;
    const MbModel *m = sim->m;
    int nv = m->nv, nb = m->nbodies;
    sim->t_eval = s->t;
    if (!fdyn(sim, s->q, s->v, s->limit_state, in, out->qacc, NULL)) return false;
    memcpy(out->tau_passive, sim->tau_p, (size_t)nv * sizeof(double));
    memcpy(out->tau_input, sim->tau_in, (size_t)nv * sizeof(double));
    double *tau_applied = malloc((size_t)(nv ? nv : 1) * sizeof(double));
    if (!tau_applied) return false;
    memcpy(tau_applied, sim->tau_in, (size_t)nv * sizeof(double));
    out->cinfo = sim->ci;
    out->cinfo.residual = scaled_norm(sim, sim->res);
    /* generalised constraint forces and loop wrenches */
    memset(out->tau_constraint, 0, (size_t)nv * sizeof(double));
    double *loopw = calloc((size_t)(6 * (nb ? nb : 1)), sizeof(double));
    double *tau_joint_rows = calloc((size_t)(nv ? nv : 1), sizeof(double));
    double *tau_id = calloc((size_t)(nv ? nv : 1), sizeof(double));
    if (!loopw || !tau_joint_rows || !tau_id) {
        free(loopw), free(tau_joint_rows), free(tau_id);
        return false;
    }
    memset(out->joint_wrench, 0, (size_t)(6 * m->njoints) * sizeof(double));
    for (int r = 0; r < sim->nrows; r++) {
        for (int c = 0; c < nv; c++) out->tau_constraint[c] += sim->G[r * nv + c] * sim->lam[r];
        if (sim->row_kind[r] == ROW_LOOP) {
            const double *Phi = sim->Phi + 6 * r;
            for (int k = 0; k < 6; k++) {
                double w = Phi[k] * sim->lam[r];
                if (sim->row_b[r] >= 0) loopw[6 * sim->row_b[r] + k] += w;
                if (sim->row_a[r] >= 0) loopw[6 * sim->row_a[r] + k] -= w;
            }
            /* wrench on the child-side body, accumulated per loop joint in world coordinates for now */
            for (int k = 0; k < 6; k++) out->joint_wrench[6 * sim->row_owner[r] + k] += Phi[k] * sim->lam[r];
        } else
            for (int c = 0; c < nv; c++) tau_joint_rows[c] += sim->G[r * nv + c] * sim->lam[r];
    }
    /* tree joint wrenches: inverse dynamics with the actual accelerations and the loop constraint wrenches */
    kinematics(sim, s->q, s->v);
    external_wrenches(sim, in);
    rnea(sim, s->v, out->qacc, true, loopw, tau_id);
    double resid = 0;
    for (int c = 0; c < nv; c++) {
        double arm = m->dofs[c].joint >= 0 ? m->joints[m->dofs[c].joint].armature : 0;
        double applied = tau_applied[c] + sim->tau_p[c] + tau_joint_rows[c];
        resid = fmax(resid, fabs(tau_id[c] + arm * out->qacc[c] - applied));
    }
    free(tau_applied);
    out->joint_torque_residual = resid;
    for (int j = 0; j < m->njoints; j++) {
        const MbJointDef *J = &m->joints[j];
        MPose F;
        if (m->joint_loop[j]) {
            mpose_mul(&F, &sim->T[J->child], &J->child_frame);
            double w[6];
            memcpy(w, out->joint_wrench + 6 * j, sizeof w);
            msv_force_to_local(out->joint_wrench + 6 * j, &F, w);
        } else {
            mpose_mul(&F, &sim->T[J->child], &J->child_frame);
            msv_force_to_local(out->joint_wrench + 6 * j, &F, sim->f + 6 * J->child);
        }
    }
    free(loopw), free(tau_joint_rows), free(tau_id);
    /* poses, twists, energy, momentum */
    double T = 0, ug = 0, us = 0;
    memset(out->momentum, 0, sizeof out->momentum);
    for (int b = 0; b < nb; b++) {
        memcpy(out->body_pose + 12 * b, sim->T[b].R, 9 * sizeof(double));
        memcpy(out->body_pose + 12 * b + 9, sim->T[b].p, 3 * sizeof(double));
        double c[3], wc[3], IV[6];
        mpose_apply(c, &sim->T[b], m->bodies[b].com);
        const double *V = sim->V + 6 * b;
        mv3_copy(out->body_twist + 6 * b, V);
        mv3_cross(wc, V, c);
        mv3_add(out->body_twist + 6 * b + 3, V + 3, wc);
        mspi_mul(IV, &sim->I[b], V);
        T += 0.5 * msv_dot(V, IV);
        ug -= mv3_dot(m->gravity, sim->I[b].h);
        for (int k = 0; k < 6; k++) out->momentum[k] += IV[k];
    }
    for (int j = 0; j < m->njoints; j++) {
        const MbJointDef *J = &m->joints[j];
        if (m->joint_loop[j]) continue;
        if (J->armature > 0) T += 0.5 * J->armature * s->v[m->joint_vadr[j]] * s->v[m->joint_vadr[j]];
        if (!(J->stiffness > 0)) continue;
        double d = s->q[m->joint_qadr[j]] - J->spring_reference;
        us += 0.5 * J->stiffness * d * d;
    }
    us += transmission_energy(m, s->q);
    for (int f = 0; f < m->nflex; f++) { /* elastic kinetic energy and momentum coupling, strain energy */
        const MbFlexDef *F = &m->flex[f];
        int qa = m->flex_qadr[f], va = m->flex_vadr[f], g0 = m->flex_gadr[f];
        for (int k = 0; k < F->nmodes; k++) {
            const double *lw = sim->ellw + 6 * (g0 + k);
            T += s->v[va + k] * msv_dot(sim->V + 6 * F->body, lw) + 0.5 * s->v[va + k] * s->v[va + k];
            us += 0.5 * F->omega2[k] * s->q[qa + k] * s->q[qa + k];
            for (int r = 0; r < 6; r++) out->momentum[r] += lw[r] * s->v[va + k];
        }
    }
    out->kinetic = T, out->potential_gravity = ug, out->potential_spring = us;
    const MbLedger *lg = &s->ledger;
    out->energy_residual = (T + ug + us) - s->energy0 - lg->work_inputs - lg->work_external + lg->dissipated_damping + lg->dissipated_friction +
                           lg->dissipated_impacts + lg->contact_friction + lg->contact_impacts - lg->projection_change;
    return true;
}

void mb_inverse_dynamics(MbSim *sim, const double *q, const double *v, const double *qacc, const double *body_wrench, double *tau, double *joint_wrench) {
    const MbModel *m = sim->m;
    MbInputs in = {NULL, body_wrench};
    kinematics(sim, q, v);
    external_wrenches(sim, &in);
    rnea(sim, v, qacc, true, NULL, tau);
    if (joint_wrench)
        for (int j = 0; j < m->njoints; j++) {
            memset(joint_wrench + 6 * j, 0, 6 * sizeof(double));
            if (m->joint_loop[j]) continue;
            MPose F;
            mpose_mul(&F, &sim->T[m->joints[j].child], &m->joints[j].child_frame);
            msv_force_to_local(joint_wrench + 6 * j, &F, sim->f + 6 * m->joints[j].child);
        }
}

int mb_flex_equilibrium(MbSim *sim, MbState *s, double tol, int max_iter, double *residual) {
    const MbModel *m = sim->m;
    if (residual) *residual = 0;
    if (!m->nflex) return 0;
    double *tau = malloc((size_t)m->nv * sizeof(double)), *zero = calloc((size_t)m->nv, sizeof(double));
    if (!tau || !zero) {
        free(tau), free(zero);
        return -1;
    }
    int it, done = -1;
    double res = INFINITY;
    for (it = 1; it <= max_iter; it++) {
        mb_inverse_dynamics(sim, s->q, s->v, zero, NULL, tau, NULL);
        double change = 0, scale = 0, hmax = 0;
        res = 0;
        for (int f = 0; f < m->nflex; f++) {
            const MbFlexDef *F = &m->flex[f];
            int qa = m->flex_qadr[f], va = m->flex_vadr[f];
            for (int k = 0; k < F->nmodes; k++) {
                double h = tau[va + k], eta = -h / F->omega2[k];
                res = fmax(res, fabs(F->omega2[k] * s->q[qa + k] + h)), hmax = fmax(hmax, fabs(h));
                change = fmax(change, fabs(eta - s->q[qa + k])), scale = fmax(scale, fabs(eta));
                s->q[qa + k] = eta;
            }
        }
        res = hmax > 0 ? res / hmax : 0;
        if (change <= tol * scale || scale == 0) {
            done = it;
            break;
        }
    }
    if (done > 0) { /* the residual of the accepted state */
        mb_inverse_dynamics(sim, s->q, s->v, zero, NULL, tau, NULL);
        double r = 0, hmax = 0;
        for (int f = 0; f < m->nflex; f++)
            for (int k = 0; k < m->flex[f].nmodes; k++) {
                double h = tau[m->flex_vadr[f] + k];
                r = fmax(r, fabs(m->flex[f].omega2[k] * s->q[m->flex_qadr[f] + k] + h)), hmax = fmax(hmax, fabs(h));
            }
        res = hmax > 0 ? r / hmax : 0;
    }
    if (residual) *residual = res;
    s->energy0 = total_energy(sim, s->q, s->v);
    memset(&s->ledger, 0, sizeof s->ledger);
    free(tau), free(zero);
    return done;
}

void mb_mass_matrix(MbSim *sim, const double *q, double *M) {
    kinematics(sim, q, NULL);
    crba(sim);
    memcpy(M, sim->M, (size_t)sim->m->nv * (size_t)sim->m->nv * sizeof(double));
}

double mb_constraint_residual(MbSim *sim, const MbState *s) {
    kinematics(sim, s->q, s->v);
    build_rows(sim, s->q, s->limit_state, NULL);
    return scaled_norm(sim, sim->res);
}

void mb_body_pose(MbSim *sim, const MbState *s, int body, MPose *T) {
    kinematics(sim, s->q, NULL);
    *T = sim->T[body];
}

void mb_point_motion(MbSim *sim, int body, const double p[3], double sf[3], double omega[3], double alpha[3], double R_body[9]) {
    /* sim->Aw holds the spatial accelerations of the last mb_evaluate, including the uniform -g of the gravity trick, so
     * the classical acceleration derived from it is the specific force */
    const double *V = sim->V + 6 * body, *A = sim->Aw + 6 * body;
    double pw[3], vp[3], wxp[3], axp[3], wxvp[3];
    mpose_apply(pw, &sim->T[body], p);
    mv3_cross(wxp, V, pw);
    mv3_add(vp, V + 3, wxp);
    mv3_cross(axp, A, pw);
    mv3_cross(wxvp, V, vp);
    for (int k = 0; k < 3; k++) sf[k] = A[3 + k] + axp[k] + wxvp[k];
    if (omega) mv3_copy(omega, V);
    if (alpha) mv3_copy(alpha, A);
    if (R_body) memcpy(R_body, sim->T[body].R, 9 * sizeof(double));
}

void mb_body_poses(MbSim *sim, const MbState *s, MPose *poses, double *twist) {
    kinematics(sim, s->q, twist ? s->v : NULL);
    for (int b = 0; b < sim->m->nbodies; b++) {
        poses[b] = sim->T[b];
        if (twist) memcpy(twist + 6 * b, sim->V + 6 * b, 6 * sizeof(double));
    }
}

/* ------------------------------------------------------------------------------------------------ contact time stepping */

typedef enum { CROW_EQ = 0, CROW_UNI, CROW_T1, CROW_T2 } CRowKind;

/* row target for a one-sided row with pre-step velocity vpre and distance dist (restitution within the step) */
static double unilateral_target(double vpre, double dist, double h, double e, double rs) {
    double bounce = -e * vpre;
    if (vpre < -rs && dist + h * vpre < 0 && bounce > rs) return bounce; /* impact inside this step: Newton restitution */
    if (dist > 0) return -dist / h;                                        /* speculative or plastic: close the gap exactly */
    return 0;                                                              /* touching or penetrated: no pull, no push-out */
}

bool mb_step_contacts(MbSim *sim, MbState *s, double h, const MbInputs *in, MbContactRow *rows, int nrows, const MbContactSolve *opt, MbContactStats *st,
                      MbStepReport *rep) {
    MbContactStats lst;
    MbStepReport lrep;
    if (!st) st = &lst;
    if (!rep) rep = &lrep;
    memset(st, 0, sizeof *st);
    memset(rep, 0, sizeof *rep);
    if (!sim || !s || !(h > 0)) {
        snprintf(rep->error, sizeof rep->error, "invalid contact step");
        return false;
    }
    const MbModel *m = sim->m;
    int nv = m->nv;
    int max_it = opt && opt->max_iterations > 0 ? opt->max_iterations : 500;
    double tol = opt && opt->tolerance > 0 ? opt->tolerance : 1e-10, rs = opt && opt->restitution_speed > 0 ? opt->restitution_speed : 1e-3;
    sim->err[0] = 0;
    /* forces at the start of the step (inputs held) */
    sim->t_eval = s->t;
    kinematics(sim, s->q, s->v);
    external_wrenches(sim, in);
    rnea(sim, s->v, NULL, true, NULL, sim->C);
    double pd, pf, pcb = 0, pext = 0;
    passive_forces(m, s->q, s->v, sim->tau_p, &pd, &pf);
    memset(sim->auxr, 0, sizeof sim->auxr);
    for (int i = 0; i < nv; i++) sim->tau_in[i] = in && in->tau ? in->tau[i] : 0;
    if (in && in->force_fn) {
        memset(sim->tmpv2, 0, (size_t)nv * sizeof(double));
        pcb = in->force_fn(in->force_ctx, sim->t_eval, s->q, s->v, sim->tmpv2, sim->auxr);
        for (int i = 0; i < nv; i++) sim->tau_in[i] += sim->tmpv2[i];
    }
    (void)pcb;
    for (int b = 0; b < m->nbodies; b++) pext += msv_dot(sim->ext + 6 * b, sim->V + 6 * b);
    for (int i = 0; i < nv; i++) sim->rhs[i] = sim->tau_in[i] + sim->tau_p[i] - sim->C[i];
    if (!factor_mass(sim)) goto fail;
    double *vfree = sim->afree;
    memcpy(vfree, sim->rhs, (size_t)nv * sizeof(double));
    mm_chol_solve(sim->L, nv, vfree);
    for (int i = 0; i < nv; i++) vfree[i] = s->v[i] + h * vfree[i];
    /* rows: bilateral constraints, joint limits, contact normal and tangents */
    int nb = build_rows(sim, s->q, NULL, NULL);
    int nl = m->nlimits, R = nb + nl + 3 * nrows;
    size_t NVs = (size_t)(nv ? nv : 1), Rs = (size_t)(R ? R : 1);
    double *J = calloc(Rs * NVs, sizeof(double)), *Y = calloc(Rs * NVs, sizeof(double)), *W = calloc(Rs, sizeof(double));
    double *target = calloc(Rs, sizeof(double)), *p = calloc(Rs, sizeof(double)), *vp = malloc(NVs * sizeof(double)), *W12 = calloc(Rs, sizeof(double));
    CRowKind *kind = calloc(Rs, sizeof(CRowKind));
    if (!J || !Y || !W || !target || !p || !vp || !W12 || !kind) {
        free(J), free(Y), free(W), free(target), free(p), free(vp), free(W12), free(kind);
        snprintf(sim->err, sizeof sim->err, "out of memory for %d contact rows", R);
        goto fail;
    }
    for (int r = 0; r < nb; r++) memcpy(J + (size_t)r * NVs, sim->G + (size_t)r * NVs, NVs * sizeof(double)), kind[r] = CROW_EQ;
    for (int l = 0; l < nl; l++) {
        int r = nb + l, j = m->limit_joint[l], va = m->joint_vadr[j], qa = m->joint_qadr[j];
        const MbJointDef *Jd = &m->joints[j];
        double q = s->q[qa], dl = q - Jd->lower, du = Jd->upper - q;
        double sgn = dl <= du ? 1 : -1, dist = dl <= du ? dl : du;
        J[(size_t)r * NVs + (size_t)va] = sgn;
        kind[r] = CROW_UNI;
        target[r] = unilateral_target(sgn * s->v[va], dist, h, Jd->restitution, rs);
    }
    for (int c = 0; c < nrows; c++) {
        MbContactRow *C = &rows[c];
        double t1[3], t2[3];
        perp_basis(C->normal, t1, t2);
        mv3_copy(C->tangent[0], t1), mv3_copy(C->tangent[1], t2);
        const double *dirs[3] = {C->normal, t1, t2};
        for (int k = 0; k < 3; k++) {
            int r = nb + nl + 3 * c + k;
            double PhiA[6], PhiB[6];
            mv3_cross(PhiA, C->point_a, dirs[k]);
            mv3_copy(PhiA + 3, dirs[k]);
            mv3_cross(PhiB, C->point_b, dirs[k]);
            mv3_copy(PhiB + 3, dirs[k]);
            if (C->body_a >= 0) add_path(sim, C->body_a, PhiA, 1, J + (size_t)r * NVs);
            if (C->body_b >= 0) add_path(sim, C->body_b, PhiB, -1, J + (size_t)r * NVs);
            kind[r] = k == 0 ? CROW_UNI : (k == 1 ? CROW_T1 : CROW_T2);
        }
        double vpre = 0;
        for (int i = 0; i < nv; i++) vpre += J[(size_t)(nb + nl + 3 * c) * NVs + (size_t)i] * s->v[i];
        target[nb + nl + 3 * c] = unilateral_target(vpre, C->gap, h, C->restitution, rs);
        C->impact = vpre < -rs && C->gap + h * vpre < 0;
    }
    for (int r = 0; r < R; r++) {
        double *y = Y + (size_t)r * NVs;
        memcpy(y, J + (size_t)r * NVs, NVs * sizeof(double));
        mm_chol_solve(sim->L, nv, y);
        double w = 0;
        for (int i = 0; i < nv; i++) w += J[(size_t)r * NVs + (size_t)i] * y[i];
        W[r] = w;
    }
    for (int c = 0; c < nrows; c++) {
        int r1 = nb + nl + 3 * c + 1;
        double w = 0;
        for (int i = 0; i < nv; i++) w += J[(size_t)r1 * NVs + (size_t)i] * Y[(size_t)(r1 + 1) * NVs + (size_t)i];
        W12[c] = w;
    }
    memcpy(vp, vfree, NVs * sizeof(double));
    if (opt && opt->warm_start) /* impulses of the matched contacts of the previous step, inside the friction cone */
        for (int c = 0; c < nrows; c++) {
            const MbContactRow *C = &rows[c];
            int rn = nb + nl + 3 * c;
            double pn = fmax(0, C->warm[0]), p1 = mv3_dot(C->warm + 1, C->tangent[0]), p2 = mv3_dot(C->warm + 1, C->tangent[1]);
            double pt = hypot(p1, p2), lim = C->friction * pn;
            if (pt > lim) p1 = pt > 0 ? p1 * lim / pt : 0, p2 = pt > 0 ? p2 * lim / pt : 0;
            if (!(pn > 0)) continue;
            p[rn] = pn, p[rn + 1] = p1, p[rn + 2] = p2;
            for (int i = 0; i < nv; i++) vp[i] += pn * Y[(size_t)rn * NVs + (size_t)i] + p1 * Y[(size_t)(rn + 1) * NVs + (size_t)i] + p2 * Y[(size_t)(rn + 2) * NVs + (size_t)i];
        }
    /* manifolds: consecutive contact rows between the same bodies with the same normal (a face contact) */
    int *mstart = malloc((size_t)(nrows + 1) * sizeof(int)), nman = 0;
    for (int c = 0; mstart && c < nrows; c++)
        if (c == 0 || rows[c].body_a != rows[c - 1].body_a || rows[c].body_b != rows[c - 1].body_b || mv3_dot(rows[c].normal, rows[c - 1].normal) < 1 - 1e-9)
            mstart[nman++] = c;
    if (mstart) mstart[nman] = nrows;
    double *jvn = malloc((size_t)(nrows ? nrows : 1) * 3 * sizeof(double));
    int it;
    double maxdp = 0, maxp = 0;
    bool converged = R == 0;
    for (it = 1; it <= max_it && R > 0 && mstart && jvn; it++) {
        maxdp = 0, maxp = 0;
        for (int r = 0; r < nb + nl; r++) {
            if (!(W[r] > 1e-14)) continue;
            double jv = 0;
            for (int i = 0; i < nv; i++) jv += J[(size_t)r * NVs + (size_t)i] * vp[i];
            double np = p[r] + (target[r] - jv) / W[r];
            if (kind[r] == CROW_UNI && np < 0) np = 0;
            double d = np - p[r];
            if (d != 0)
                for (int i = 0; i < nv; i++) vp[i] += d * Y[(size_t)r * NVs + (size_t)i];
            p[r] = np;
            maxdp = fmax(maxdp, fabs(d)), maxp = fmax(maxp, fabs(np));
        }
        /* Rigid flat-on-flat contact leaves the split of the impulse among the points of a face undetermined. Within a
         * manifold the points are updated together (Jacobi step relaxed by 1/k) so the split does not depend on the order
         * of the points; manifolds are updated one after the other (Gauss-Seidel). */
        for (int mi = 0; mi < nman; mi++) {
            int c0 = mstart[mi], c1 = mstart[mi + 1], k = c1 - c0;
            double omega = 1.0 / k;
            for (int pass = 0; pass < 2; pass++) { /* pass 0: normal rows, pass 1: friction */
                for (int c = c0; c < c1; c++) {
                    int rn = nb + nl + 3 * c;
                    double a1 = 0, a2 = 0, a3 = 0;
                    for (int i = 0; i < nv; i++) {
                        a1 += J[(size_t)rn * NVs + (size_t)i] * vp[i];
                        if (pass) a2 += J[(size_t)(rn + 1) * NVs + (size_t)i] * vp[i], a3 += J[(size_t)(rn + 2) * NVs + (size_t)i] * vp[i];
                    }
                    jvn[3 * c] = a1, jvn[3 * c + 1] = a2, jvn[3 * c + 2] = a3;
                }
                for (int c = c0; c < c1; c++) {
                    int rn = nb + nl + 3 * c, r1 = rn + 1, r2 = rn + 2;
                    if (!pass) {
                        if (!(W[rn] > 1e-14)) continue;
                        double np = fmax(0, p[rn] + (k == 1 ? 1.0 : omega) * (target[rn] - jvn[3 * c]) / W[rn]), d = np - p[rn];
                        if (d != 0)
                            for (int i = 0; i < nv; i++) vp[i] += d * Y[(size_t)rn * NVs + (size_t)i];
                        p[rn] = np;
                        maxdp = fmax(maxdp, fabs(d)), maxp = fmax(maxp, np);
                        continue;
                    }
                    double lim = rows[c].friction * p[rn];
                    double a = W[r1], b = W12[c], dd = W[r2], det = a * dd - b * b;
                    if (!(a > 1e-14 && dd > 1e-14)) continue;
                    double w1 = jvn[3 * c + 1], w2 = jvn[3 * c + 2];
                    double u1 = w1 - (a * p[r1] + b * p[r2]), u2 = w2 - (b * p[r1] + dd * p[r2]); /* velocity without this friction */
                    double c1v, c2v;
                    if (det > 1e-14 * a * dd) c1v = -(dd * u1 - b * u2) / det, c2v = -(a * u2 - b * u1) / det;
                    else c1v = -u1 / a, c2v = -u2 / dd;
                    if (hypot(c1v, c2v) > lim) {
                        /* sliding: exact local problem min 1/2 p^T W p + u0^T p over |p| <= lim (see mb_step_contacts) */
                        if (lim <= 0) {
                            c1v = c2v = 0;
                        } else {
                            double lo = 0, hi = hypot(u1, u2) / lim + fabs(a) + fabs(dd), q1 = 0, q2 = 0;
                            for (int bi = 0; bi < 200; bi++) {
                                double sm = 0.5 * (lo + hi), A = a + sm, D = dd + sm, den = A * D - b * b;
                                q1 = -(D * u1 - b * u2) / den, q2 = -(A * u2 - b * u1) / den;
                                if (hypot(q1, q2) > lim) lo = sm;
                                else hi = sm;
                                if (hi - lo <= 1e-15 * hi) break;
                            }
                            double sc = hypot(q1, q2);
                            c1v = sc > 0 ? q1 * lim / sc : 0, c2v = sc > 0 ? q2 * lim / sc : 0;
                        }
                    }
                    double rel = k == 1 ? 1.0 : omega;
                    double d1 = rel * (c1v - p[r1]), d2 = rel * (c2v - p[r2]);
                    /* the relaxed step stays inside the disc (convex combination of two points inside it) */
                    if (d1 != 0 || d2 != 0)
                        for (int i = 0; i < nv; i++) vp[i] += d1 * Y[(size_t)r1 * NVs + (size_t)i] + d2 * Y[(size_t)r2 * NVs + (size_t)i];
                    p[r1] += d1, p[r2] += d2;
                    maxdp = fmax(maxdp, fmax(fabs(d1), fabs(d2))), maxp = fmax(maxp, hypot(p[r1], p[r2]));
                }
            }
        }
        if (maxdp <= tol * fmax(maxp, 1e-300) || maxdp == 0) {
            converged = true;
            break;
        }
    }
    free(mstart), free(jvn);
    st->iterations = R > 0 ? (it > max_it ? max_it : it) : 0;
    st->residual = maxp > 0 ? maxdp / maxp : 0;
    st->converged = converged;
    st->bilateral_rows = nb, st->limit_rows = nl, st->contact_rows = nrows;
    /* ledger (midpoint velocity makes the kinetic-energy decomposition exact for the discrete step) */
    for (int i = 0; i < nv; i++) sim->tmpv[i] = 0.5 * (s->v[i] + vp[i]);
    double pin = 0;
    for (int i = 0; i < nv; i++) pin += sim->tau_in[i] * sim->tmpv[i];
    s->ledger.work_inputs += h * pin;
    s->ledger.work_external += h * pext;
    s->ledger.dissipated_damping -= h * pd;
    s->ledger.dissipated_friction -= h * pf;
    for (int k = 0; k < MB_AUX_MAX; k++) s->aux[k] += h * sim->auxr[k];
    for (int r = 0; r < R; r++) {
        if (p[r] == 0) continue;
        double jv = 0;
        for (int i = 0; i < nv; i++) jv += J[(size_t)r * NVs + (size_t)i] * sim->tmpv[i];
        double work = p[r] * jv;
        if (r < nb)
            st->bilateral_work += work;
        else if (r < nb + nl)
            s->ledger.dissipated_impacts -= work;
        else if (kind[r] == CROW_UNI)
            s->ledger.contact_impacts -= work, st->impact_dissipation -= work;
        else
            s->ledger.contact_friction -= work, st->friction_dissipation -= work;
    }
    for (int c = 0; c < nrows; c++) {
        int rn = nb + nl + 3 * c;
        MbContactRow *C = &rows[c];
        double vn = 0, v1 = 0, v2 = 0;
        for (int i = 0; i < nv; i++)
            vn += J[(size_t)rn * NVs + (size_t)i] * vp[i], v1 += J[(size_t)(rn + 1) * NVs + (size_t)i] * vp[i], v2 += J[(size_t)(rn + 2) * NVs + (size_t)i] * vp[i];
        C->impulse_normal = p[rn];
        C->impulse_tangent[0] = p[rn + 1], C->impulse_tangent[1] = p[rn + 2];
        C->normal_velocity = vn;
        C->tangential_speed = hypot(v1, v2);
        double ft = hypot(p[rn + 1], p[rn + 2]);
        C->state = p[rn] <= 0 ? 0 : (ft >= C->friction * p[rn] * (1 - 1e-9) && C->tangential_speed > 1e-9 ? 2 : 1);
    }
    /* positions with the new velocity */
    for (int i = 0; i < nv; i++) sim->tmpv[i] = h * vp[i];
    velocity_to_tangent(m, s->q, sim->tmpv, sim->tmpv2);
    retract(m, s->q, sim->tmpv2, s->q);
    memcpy(s->v, vp, NVs * sizeof(double));
    s->t += h;
    s->steps++;
    free(J), free(Y), free(W), free(target), free(p), free(vp), free(W12), free(kind);
    if (nb && !project(sim, s, rep, m->opt.project_positions, m->opt.project_velocities)) goto fail;
    rep->cinfo = sim->ci;
    return true;
fail:
    snprintf(rep->error, sizeof rep->error, "%s", sim->err[0] ? sim->err : "contact step failed");
    return false;
}
