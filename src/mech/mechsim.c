/* mechsim.c - study runtime: actuators, controllers, sensors, scheduling, histories, envelopes, checkpoints */
#include "mechsim.h"

#include "../core/base64.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const ACT_NAMES[ACT_TYPES] = {"effort", "position_servo", "velocity_servo", "dc_motor"};
static const char *const TRAJ_NAMES[TRAJ_TYPES] = {"constant", "step", "ramp", "sine", "trapezoid", "min_jerk", "waypoints"};
static const char *const SENS_NAMES[SENS_TYPES] = {"joint_position", "joint_velocity", "joint_effort", "motor_current", "force_torque", "imu_accelerometer",
                                                   "imu_gyroscope"};

const char *actuator_type_name(ActuatorType t) { return (unsigned)t < ACT_TYPES ? ACT_NAMES[t] : "?"; }
int actuator_type_from_name(const char *s) {
    for (int i = 0; s && i < ACT_TYPES; i++)
        if (!strcmp(s, ACT_NAMES[i])) return i;
    return -1;
}
const char *trajectory_type_name(TrajType t) { return (unsigned)t < TRAJ_TYPES ? TRAJ_NAMES[t] : "?"; }
int trajectory_type_from_name(const char *s) {
    for (int i = 0; s && i < TRAJ_TYPES; i++)
        if (!strcmp(s, TRAJ_NAMES[i])) return i;
    return -1;
}
const char *sensor_type_name(SensorType t) { return (unsigned)t < SENS_TYPES ? SENS_NAMES[t] : "?"; }
int sensor_type_from_name(const char *s) {
    for (int i = 0; s && i < SENS_TYPES; i++)
        if (!strcmp(s, SENS_NAMES[i])) return i;
    return -1;
}
int sensor_dim(SensorType t) { return t == SENS_FORCE_TORQUE ? 6 : (t == SENS_IMU_ACCEL || t == SENS_IMU_GYRO ? 3 : 1); }

/* ------------------------------------------------------------------------------------------------ trajectories */

/* rest-to-rest minimum-jerk blend s(tau), tau in [0, 1] */
static void min_jerk(double tau, double *s, double *ds, double *dds) {
    if (tau <= 0) {
        *s = *ds = *dds = 0;
        return;
    }
    if (tau >= 1) {
        *s = 1, *ds = *dds = 0;
        return;
    }
    double t2 = tau * tau, t3 = t2 * tau;
    *s = t3 * (10 - 15 * tau + 6 * t2);
    *ds = 30 * t2 * (1 - 2 * tau + t2);
    *dds = 60 * tau * (1 - 3 * tau + 2 * t2);
}

void trajectory_eval(const Trajectory *tr, double t, double *p, double *v, double *a) {
    double pp = 0, vv = 0, aa = 0, dt = t - tr->t0;
    switch (tr->type) {
    case TRAJ_CONSTANT: pp = tr->value; break;
    case TRAJ_STEP: pp = dt >= 0 ? tr->value1 : tr->value; break;
    case TRAJ_RAMP: {
        double span = tr->value1 - tr->value, dur = tr->rate != 0 ? fabs(span / tr->rate) : 0;
        if (dt <= 0) pp = tr->value;
        else if (dt >= dur) pp = tr->value1;
        else pp = tr->value + copysign(fabs(tr->rate), span) * dt, vv = copysign(fabs(tr->rate), span);
        break;
    }
    case TRAJ_SINE: {
        double w = 2 * M_PI * tr->frequency, arg = w * dt + tr->phase;
        if (dt < 0) arg = tr->phase, w = 0;
        pp = tr->value + tr->amplitude * sin(arg);
        vv = tr->amplitude * w * cos(arg);
        aa = -tr->amplitude * w * w * sin(arg);
        break;
    }
    case TRAJ_MIN_JERK: {
        double s, ds, dds, T = tr->duration > 0 ? tr->duration : 1, span = tr->value1 - tr->value;
        min_jerk(dt / T, &s, &ds, &dds);
        pp = tr->value + span * s, vv = span * ds / T, aa = span * dds / (T * T);
        break;
    }
    case TRAJ_TRAPEZOID: {
        double span = tr->value1 - tr->value, D = fabs(span), sg = span >= 0 ? 1 : -1, V = tr->vmax, A = tr->amax;
        if (D == 0 || !(V > 0) || !(A > 0)) {
            pp = tr->value1;
            break;
        }
        double ta = V / A, da = 0.5 * A * ta * ta;
        if (2 * da > D) ta = sqrt(D / A), V = A * ta, da = 0.5 * D; /* triangular profile */
        double tc = (D - 2 * da) / V, T = 2 * ta + tc, x, xv, xa;
        if (dt <= 0) x = 0, xv = 0, xa = 0;
        else if (dt < ta) x = 0.5 * A * dt * dt, xv = A * dt, xa = A;
        else if (dt < ta + tc) x = da + V * (dt - ta), xv = V, xa = 0;
        else if (dt < T) {
            double r = T - dt;
            x = D - 0.5 * A * r * r, xv = A * r, xa = -A;
        } else
            x = D, xv = 0, xa = 0;
        pp = tr->value + sg * x, vv = sg * xv, aa = sg * xa;
        break;
    }
    case TRAJ_WAYPOINTS: {
        int n = tr->npoints;
        if (n <= 0) break;
        if (dt <= tr->wt[0] || n == 1) {
            pp = tr->wp[0];
            break;
        }
        if (dt >= tr->wt[n - 1]) {
            pp = tr->wp[n - 1];
            break;
        }
        int i = 0;
        while (i + 1 < n && dt >= tr->wt[i + 1]) i++;
        double T = tr->wt[i + 1] - tr->wt[i], s, ds, dds, span = tr->wp[i + 1] - tr->wp[i];
        min_jerk((dt - tr->wt[i]) / T, &s, &ds, &dds);
        pp = tr->wp[i] + span * s, vv = span * ds / T, aa = span * dds / (T * T);
        break;
    }
    default: break;
    }
    if (p) *p = pp;
    if (v) *v = vv;
    if (a) *a = aa;
}

/* ------------------------------------------------------------------------------------------------ random numbers */

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t xoshiro_next(uint64_t *s) {
    uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0], s[3] ^= s[1], s[1] ^= s[2], s[0] ^= s[3], s[2] ^= t, s[3] = rotl(s[3], 45);
    return r;
}

static void xoshiro_seed(uint64_t *s, uint64_t seed) {
    for (int i = 0; i < 4; i++) { /* splitmix64 */
        seed += 0x9E3779B97F4A7C15ULL;
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        s[i] = z ^ (z >> 31);
    }
}

/* ------------------------------------------------------------------------------------------------ runtime state */

typedef struct CtlState {
    uint64_t k;
    double integ, prev_meas, dfilt, u_unsat, u, reference, measurement, error;
    bool has_prev;
    double delay[MS_MAX_DELAY + 1];
    int dhead;
    double err_sq, err_max;
    uint64_t samples, saturated;
} CtlState;

typedef struct SenState {
    uint64_t k;
    uint64_t rng[4];
    bool have_spare;
    double spare;
    bool filt_init;
    double filt[6];
    double ring[MS_MAX_LATENCY + 1][6];
    int head, count;
    double out[6], truth[6];
} SenState;

typedef struct ActState {
    double cmd;           /* applied command (after delay, held) */
    ActuatorOutput out;   /* at the last evaluation point */
    double peak_effort, peak_current;
    uint64_t over_rating_steps;
} ActState;

/* copy of the contact rows of one step (for load snapshots) */
typedef struct RowCapture {
    int n, cap;
    double h;
    MbContactRow *rows;
    int *sa, *sb;
} RowCapture;

struct MechSim {
    MbModel *m; /* owned */
    ActuatorDef *act;
    int nact;
    ControllerDef *ctl;
    int nctl;
    SensorDef *sen;
    int nsen;
    StudySettings st;
    MbSim *sim;
    MbState *s;
    MbEval *ev;
    ActState *as;
    CtlState *cs;
    SenState *ss;
    uint64_t rec_k;
    double record_period;
    /* recorder */
    int nch;
    char (*ch_name)[80];
    char (*ch_unit)[16];
    double *data; /* nch * cap_rows, column-major blocks: data[c * cap + r] */
    int rows, cap_rows;
    bool record_full;
    /* envelopes per joint */
    double *env; /* 16 per joint: |F|max, t, F(3), M(3) at that time; |M|max, t, F(3), M(3) */
    double drift_max, vdrift_max;
    uint64_t steps_total, events_total;
    bool initialized;
    double *tau_scratch;
    /* snapshots */
    int nsnap;
    double snap_time[64];
    uint64_t snap_next;
    size_t blob_size;
    uint8_t *snap_blob;   /* nsnap * blob_size */
    bool *snap_have;
    uint8_t *peak_blob;   /* 2 * njoints * blob_size: peak force, peak moment */
    double *peak_blob_t;  /* 2 * njoints, NAN when absent */
    /* contact */
    ContactShape *shapes;
    int nshapes;
    MbContactRow *crows;  /* contact rows of the last step */
    int *row_sa, *row_sb, ncrows;
    double last_h;
    int npairs;
    int pair_a[64], pair_b[64];
    double pair_now[64][6];  /* normal force, friction force, sliding, min gap, impact, friction utilisation */
    double pair_peak_n[64], pair_peak_t[64], pair_peak_f[64], pair_min_gap[64], pair_slide_time[64];
    int pair_impacts[64];
    uint8_t *pair_blob;      /* npairs * blob_size at the peak normal force of persistent contact */
    RowCapture *pair_cap;    /* npairs: contact rows of that step */
    double pair_blob_t[64];  /* time the blob was captured (NAN = none) */
    double pair_peak_impulse[64]; /* N*s: largest normal impulse of one impact (summed over consecutive impact steps) */
    double pair_run_impulse[64];  /* N*s: impulse of the impact in progress */
    double pair_util_peak[64], pair_util_peak_t[64]; /* largest friction utilisation |F_t| / (mu F_n) in persistent contact, and when */
    RowCapture snap_cap[64]; /* contact rows of the step that ended at each requested snapshot */
    RowCapture *peak_cap;    /* 2 * njoints: contact rows of the steps with the joint load peaks */
    MPose *cposes;           /* nbodies scratch */
    ContactWarm warm;        /* solver warm start (state) */
    double *cwrench;         /* 6 * nbodies: contact wrench of the last step on each body averaged over the step (world; moment about the COM, force) */
    int step_impulsive;      /* the last step contained an impact with a nonzero impulse */
    int impulsive_steps;
    double *env_impulse;     /* njoints: largest |joint force| * step in impact steps (N*s) */
    int contact_unconverged, contact_max_iter;
    double contact_max_penetration;
    /* flexible bodies */
    double flex_wmax;        /* highest elastic frequency (rad/s), 0 without flexible bodies */
    double flex_step;        /* 0.5 / flex_wmax: steps are limited to it (INFINITY without flexible bodies) */
    int nflex_itf;           /* interfaces of all flexible bodies */
    int *flex_itf_joint;     /* nflex_itf: first tree joint riding each interface (-1 none) */
    double *flex_env;        /* 4 per interface (peak deflection, time, peak rotation, time), then 2 per flexible body (peak strain energy, time) */
    int flex_eq_iterations;  /* initial static equilibrium (0 = undeformed start) */
    double flex_eq_residual;
    char err[256];
};

static void capture_free(RowCapture *c) {
    free(c->rows), free(c->sa), free(c->sb);
    memset(c, 0, sizeof *c);
}

static bool scalar_def_joint(const MbModelDef *d, int j) {
    return j >= 0 && j < d->njoints && (d->joints[j].type == MB_REVOLUTE || d->joints[j].type == MB_PRISMATIC);
}

MechSim *mechsim_new(const MbModelDef *def, const MbOptions *opt, const ActuatorDef *act, int nact, const ControllerDef *ctl, int nctl,
                     const SensorDef *sen, int nsen, const StudySettings *st, MechDiag *diag) {
    /* reflected rotor inertia goes into a private copy of the definition */
    MbModelDef copy = *def;
    copy.joints = malloc((size_t)(def->njoints ? def->njoints : 1) * sizeof *copy.joints);
    if (!copy.joints) return NULL;
    if (def->njoints) memcpy(copy.joints, def->joints, (size_t)def->njoints * sizeof *copy.joints);
    for (int a = 0; a < nact; a++)
        if (act[a].type == ACT_DC_MOTOR && act[a].rotor_inertia > 0 && scalar_def_joint(def, act[a].joint))
            copy.joints[act[a].joint].armature += act[a].gear_ratio * act[a].gear_ratio * act[a].rotor_inertia;
    MbModel *m = mb_compile(&copy, opt, diag);
    free(copy.joints);
    if (!m) return NULL;
    MechSim *ms = calloc(1, sizeof *ms);
    if (!ms) {
        mb_model_free(m);
        return NULL;
    }
    ms->m = m;
    ms->nact = nact, ms->nctl = nctl, ms->nsen = nsen;
    ms->act = calloc((size_t)(nact ? nact : 1), sizeof *ms->act);
    ms->ctl = calloc((size_t)(nctl ? nctl : 1), sizeof *ms->ctl);
    ms->sen = calloc((size_t)(nsen ? nsen : 1), sizeof *ms->sen);
    ms->as = calloc((size_t)(nact ? nact : 1), sizeof *ms->as);
    ms->cs = calloc((size_t)(nctl ? nctl : 1), sizeof *ms->cs);
    ms->ss = calloc((size_t)(nsen ? nsen : 1), sizeof *ms->ss);
    ms->env = calloc((size_t)(16 * (m->njoints ? m->njoints : 1)), sizeof *ms->env);
    ms->tau_scratch = calloc((size_t)(m->nv ? m->nv : 1), sizeof(double));
    ms->sim = mb_sim_new(m);
    ms->s = mb_state_new(m);
    ms->ev = mb_eval_new(m);
    if (!ms->act || !ms->ctl || !ms->sen || !ms->as || !ms->cs || !ms->ss || !ms->env || !ms->tau_scratch || !ms->sim || !ms->s || !ms->ev) {
        mechsim_free(ms);
        return NULL;
    }
    if (nact) memcpy(ms->act, act, (size_t)nact * sizeof *act);
    if (nctl) memcpy(ms->ctl, ctl, (size_t)nctl * sizeof *ctl);
    if (nsen) memcpy(ms->sen, sen, (size_t)nsen * sizeof *sen);
    ms->st = *st;
    if (!(ms->st.max_records > 0)) ms->st.max_records = 200000;
    /* elastic coordinates: the explicit integrators need h w <= 2.8 (RKMK4) and h w <= 2 (contact time stepping); steps are kept
     * at h w_max <= 0.5 so the highest coordinates stay stable with little numerical damping */
    ms->flex_step = INFINITY;
    for (int f = 0; f < m->nflex; f++) {
        for (int k = 0; k < m->flex[f].nmodes; k++) ms->flex_wmax = fmax(ms->flex_wmax, sqrt(m->flex[f].omega2[k]));
        ms->nflex_itf += m->flex[f].ninterfaces;
    }
    if (ms->flex_wmax > 0) ms->flex_step = 0.5 / ms->flex_wmax;
    ms->flex_itf_joint = malloc((size_t)(ms->nflex_itf ? ms->nflex_itf : 1) * sizeof(int));
    ms->flex_env = calloc((size_t)(4 * ms->nflex_itf + 2 * m->nflex + 1), sizeof(double));
    if (!ms->flex_itf_joint || !ms->flex_env) {
        mechsim_free(ms);
        return NULL;
    }
    for (int f = 0, it = 0; f < m->nflex; f++)
        for (int i = 0; i < m->flex[f].ninterfaces; i++, it++) {
            ms->flex_itf_joint[it] = -1;
            for (int j = 0; j < m->njoints && ms->flex_itf_joint[it] < 0; j++)
                if (!m->joint_loop[j] && m->joints[j].parent == m->flex[f].body && m->joints[j].parent_interface == i) ms->flex_itf_joint[it] = j;
        }
    (void)diag;
    return ms;
}

/* translation and rotation vector of interface i in body axes (first order: sum over coordinates of phi eta and psi eta) */
static void flex_motion(const MbFlexDef *F, int i, const double *eta, double d[3], double r[3]) {
    d[0] = d[1] = d[2] = r[0] = r[1] = r[2] = 0;
    for (int k = 0; k < F->nmodes; k++)
        for (int c = 0; c < 3; c++) d[c] += F->interfaces[i].phi[3 * k + c] * eta[k], r[c] += F->interfaces[i].psi[3 * k + c] * eta[k];
}

static double flex_strain_energy(const MbFlexDef *F, const double *eta) {
    double u = 0;
    for (int k = 0; k < F->nmodes; k++) u += 0.5 * F->omega2[k] * eta[k] * eta[k];
    return u;
}

void mechsim_free(MechSim *ms) {
    if (!ms) return;
    free(ms->act), free(ms->ctl), free(ms->sen), free(ms->as), free(ms->cs), free(ms->ss), free(ms->env), free(ms->tau_scratch);
    mb_sim_free(ms->sim);
    mb_state_free(ms->s);
    mb_eval_free(ms->ev);
    free(ms->ch_name), free(ms->ch_unit), free(ms->data);
    free(ms->snap_blob), free(ms->snap_have), free(ms->peak_blob), free(ms->peak_blob_t);
    free(ms->shapes), free(ms->crows), free(ms->row_sa), free(ms->row_sb), free(ms->pair_blob);
    for (int i = 0; ms->pair_cap && i < ms->npairs; i++) capture_free(&ms->pair_cap[i]);
    for (int i = 0; i < 64; i++) capture_free(&ms->snap_cap[i]);
    for (int i = 0; ms->peak_cap && i < 2 * ms->m->njoints; i++) capture_free(&ms->peak_cap[i]);
    free(ms->pair_cap), free(ms->peak_cap), free(ms->cwrench), free(ms->cposes), free(ms->env_impulse);
    free(ms->flex_itf_joint), free(ms->flex_env);
    contact_warm_free(&ms->warm);
    mb_model_free(ms->m);
    free(ms);
}

const MbModel *mechsim_model(const MechSim *ms) { return ms->m; }

bool mechsim_set_contact_shapes(MechSim *ms, const ContactShape *shapes, int n) {
    if (ms->initialized || n < 0) return false;
    free(ms->shapes);
    ms->shapes = malloc((size_t)(n ? n : 1) * sizeof *ms->shapes);
    if (!ms->shapes) return false;
    if (n) memcpy(ms->shapes, shapes, (size_t)n * sizeof *shapes);
    ms->nshapes = n;
    return true;
}

static bool pair_jointed(const MbModel *m, int a, int b) {
    for (int j = 0; j < m->njoints; j++) {
        if (m->joints[j].type == MB_FREE) continue;
        if ((m->joints[j].parent == a && m->joints[j].child == b) || (m->joints[j].parent == b && m->joints[j].child == a)) return true;
    }
    return false;
}

static int pair_index(const MechSim *ms, int a, int b) {
    for (int i = 0; i < ms->npairs; i++)
        if ((ms->pair_a[i] == a && ms->pair_b[i] == b) || (ms->pair_a[i] == b && ms->pair_b[i] == a)) return i;
    return -1;
}

static bool capture_rows(const MechSim *ms, RowCapture *c) {
    if (ms->ncrows > c->cap) {
        size_t n = (size_t)ms->ncrows;
        MbContactRow *r = realloc(c->rows, n * sizeof *r);
        if (!r) return false;
        c->rows = r;
        int *a = realloc(c->sa, n * sizeof *a);
        if (!a) return false;
        c->sa = a;
        int *b = realloc(c->sb, n * sizeof *b);
        if (!b) return false;
        c->sb = b;
        c->cap = ms->ncrows;
    }
    if (ms->ncrows) {
        memcpy(c->rows, ms->crows, (size_t)ms->ncrows * sizeof *c->rows);
        memcpy(c->sa, ms->row_sa, (size_t)ms->ncrows * sizeof *c->sa);
        memcpy(c->sb, ms->row_sb, (size_t)ms->ncrows * sizeof *c->sb);
    }
    c->n = ms->ncrows, c->h = ms->last_h;
    return true;
}

/* rows with a nonzero impulse, in world coordinates */
static JsonValue *capture_json(const MechSim *ms, const RowCapture *c) {
    JsonValue *arr = json_array();
    for (int r = 0; r < c->n; r++) {
        const MbContactRow *C = &c->rows[r];
        double ft[3];
        for (int k = 0; k < 3; k++) ft[k] = C->impulse_tangent[0] * C->tangent[0][k] + C->impulse_tangent[1] * C->tangent[1][k];
        if (!(C->impulse_normal > 0) && !(mv3_norm(ft) > 0)) continue;
        JsonValue *o = json_object();
        json_set_string(o, "shape_a", ms->shapes[c->sa[r]].name);
        json_set_string(o, "shape_b", ms->shapes[c->sb[r]].name);
        json_set(o, "point_a", json_numbers(C->point_a, 3));
        json_set(o, "point_b", json_numbers(C->point_b, 3));
        json_set(o, "normal", json_numbers(C->normal, 3));
        json_set_number(o, "impulse_normal", C->impulse_normal);
        json_set(o, "impulse_tangent", json_numbers(ft, 3));
        json_set_number(o, "gap", C->gap);
        json_set_string(o, "state", C->state == 2 ? "sliding" : "sticking");
        json_set_bool(o, "impact", C->impact);
        json_push(arr, o);
    }
    return arr;
}

static void capture_to_snapshot(const MechSim *ms, const RowCapture *c, JsonValue *o) {
    json_set(o, "contact_rows", capture_json(ms, c));
    json_set_number(o, "contact_step_s", c->h);
    bool impulsive = false;
    for (int r = 0; r < c->n; r++) impulsive |= c->rows[r].impact && c->rows[r].impulse_normal > 0;
    json_set_bool(o, "contact_impulsive", impulsive);
}

/* contact wrenches of the last step on the bodies (impulse / step, at the contact points, about the centres of mass at the
 * end of the step) and whether the step had an impact */
static void contact_wrenches(MechSim *ms, double h) {
    const MbModel *m = ms->m;
    memset(ms->cwrench, 0, (size_t)(6 * m->nbodies) * sizeof(double));
    ms->step_impulsive = 0;
    if (!ms->ncrows) return;
    mb_body_poses(ms->sim, ms->s, ms->cposes, NULL);
    for (int r = 0; r < ms->ncrows; r++) {
        const MbContactRow *C = &ms->crows[r];
        double f[3];
        for (int k = 0; k < 3; k++) f[k] = (C->impulse_normal * C->normal[k] + C->impulse_tangent[0] * C->tangent[0][k] + C->impulse_tangent[1] * C->tangent[1][k]) / h;
        if (C->impact && C->impulse_normal > 0) ms->step_impulsive = 1;
        for (int side = 0; side < 2; side++) {
            int b = side ? C->body_b : C->body_a;
            if (b < 0) continue;
            double sg = side ? -1 : 1, c[3], d3[3], mom[3], fs[3];
            mpose_apply(c, &ms->cposes[b], m->bodies[b].com);
            mv3_sub(d3, side ? C->point_b : C->point_a, c);
            mv3_scale(fs, f, sg);
            mv3_cross(mom, d3, fs);
            mv3_addto(ms->cwrench + 6 * b, mom);
            mv3_addto(ms->cwrench + 6 * b + 3, fs);
        }
    }
}

double mechsim_step_limit(const MechSim *ms) { return fmin(ms->st.max_step, ms->flex_step); }
double mechsim_time(const MechSim *ms) { return ms->s->t; }
const MbState *mechsim_state(const MechSim *ms) { return ms->s; }
MbSim *mechsim_mb(MechSim *ms) { return ms->sim; }

/* ------------------------------------------------------------------------------------------------ actuators */

static bool scalar_joint(const MbModel *m, int j) {
    return j >= 0 && j < m->njoints && !m->joint_loop[j] && (m->joints[j].type == MB_REVOLUTE || m->joints[j].type == MB_PRISMATIC);
}

/* joint effort from command and state; fills the output record */
static double actuator_effort(const ActuatorDef *A, double cmd, double q, double v, ActuatorOutput *o) {
    memset(o, 0, sizeof *o);
    o->command = cmd;
    double tau = 0;
    bool sat = false;
    switch (A->type) {
    case ACT_EFFORT: tau = cmd; break;
    case ACT_POSITION_SERVO: tau = A->kp * (cmd - q) - A->kd * v; break;
    case ACT_VELOCITY_SERVO: tau = A->kv * (cmd - v); break;
    case ACT_DC_MOTOR: {
        double N = A->gear_ratio, w = N * v, V = cmd;
        if (A->voltage_limit > 0 && fabs(V) > A->voltage_limit) V = copysign(A->voltage_limit, V), sat = true;
        double i = (V - A->back_emf_constant * w) / A->resistance;
        if (A->current_limit > 0 && fabs(i) > A->current_limit) {
            i = copysign(A->current_limit, i);
            sat = true;
            V = i * A->resistance + A->back_emf_constant * w; /* terminal voltage the current-limiting driver applies */
        }
        double tm_em = A->torque_constant * i;
        double tm = tm_em - A->motor_viscous * w - (A->motor_coulomb > 0 ? A->motor_coulomb * tanh(w / A->motor_coulomb_vreg) : 0);
        double eta = A->efficiency > 0 ? A->efficiency : 1;
        tau = tm * w >= 0 ? eta * N * tm : N * tm / eta;
        o->voltage = V;
        o->current = i;
        o->electrical_power = V * i;
        o->copper_loss = i * i * A->resistance;
        o->friction_loss = (tm_em - tm) * w;
        o->gear_loss = tm * w - tau * v;
        break;
    }
    default: break;
    }
    if (A->type != ACT_DC_MOTOR && A->effort_limit > 0) {
        double lim = A->effort_limit;
        if (A->velocity_limit > 0 && tau * v > 0) lim *= fmax(0.0, 1 - fabs(v) / A->velocity_limit);
        if (fabs(tau) > lim) tau = copysign(lim, tau), sat = true;
    }
    o->effort = tau;
    o->saturated = sat;
    o->over_rating = A->rated_effort > 0 && fabs(tau) > A->rated_effort;
    return tau;
}

/* auxiliary integrals per actuator (MbState.aux): electrical energy, copper, friction and gearbox losses, effort^2, current^2,
 * saturated time */
enum { AUX_PER_ACT = 7 };

static double act_force(void *ctx, double t, const double *q, const double *v, double *tau, double *aux) {
    MechSim *ms = ctx;
    const MbModel *m = ms->m;
    double p = 0;
    (void)t;
    for (int a = 0; a < ms->nact; a++) {
        const ActuatorDef *A = &ms->act[a];
        int qa = m->joint_qadr[A->joint], va = m->joint_vadr[A->joint];
        ActuatorOutput o;
        double e = actuator_effort(A, ms->as[a].cmd, q[qa], v[va], &o);
        tau[va] += e;
        p += e * v[va];
        double *x = aux + AUX_PER_ACT * a;
        x[0] = o.electrical_power, x[1] = o.copper_loss, x[2] = o.friction_loss, x[3] = o.gear_loss;
        x[4] = o.effort * o.effort, x[5] = o.current * o.current, x[6] = o.saturated ? 1 : 0;
    }
    return p;
}

static void actuators_evaluate(MechSim *ms) {
    for (int a = 0; a < ms->nact; a++) {
        const ActuatorDef *A = &ms->act[a];
        int j = A->joint;
        actuator_effort(A, ms->as[a].cmd, ms->s->q[ms->m->joint_qadr[j]], ms->s->v[ms->m->joint_vadr[j]], &ms->as[a].out);
    }
}

/* ------------------------------------------------------------------------------------------------ sensors */

static double gaussian(SenState *st) {
    if (st->have_spare) {
        st->have_spare = false;
        return st->spare;
    }
    double u1, u2;
    do u1 = (xoshiro_next(st->rng) >> 11) * (1.0 / 9007199254740992.0);
    while (u1 <= 0);
    u2 = (xoshiro_next(st->rng) >> 11) * (1.0 / 9007199254740992.0);
    double r = sqrt(-2 * log(u1));
    st->spare = r * sin(2 * M_PI * u2);
    st->have_spare = true;
    return r * cos(2 * M_PI * u2);
}

/* ground truth of a sensor at the current state; ms->ev must be evaluated */
static void sensor_truth(MechSim *ms, const SensorDef *S, double *x) {
    const MbModel *m = ms->m;
    switch (S->type) {
    case SENS_JOINT_POSITION: x[0] = ms->s->q[m->joint_qadr[S->joint]]; break;
    case SENS_JOINT_VELOCITY: x[0] = ms->s->v[m->joint_vadr[S->joint]]; break;
    case SENS_JOINT_EFFORT: x[0] = ms->as[S->actuator].out.effort; break;
    case SENS_MOTOR_CURRENT: x[0] = ms->as[S->actuator].out.current; break;
    case SENS_FORCE_TORQUE: /* [force; moment] in the child joint frame */
        for (int k = 0; k < 3; k++) x[k] = ms->ev->joint_wrench[6 * S->joint + 3 + k], x[3 + k] = ms->ev->joint_wrench[6 * S->joint + k];
        break;
    case SENS_IMU_ACCEL:
    case SENS_IMU_GYRO: {
        double sf[3], w[3], al[3], Rb[9], Rs[9];
        mb_point_motion(ms->sim, S->body, S->site.p, sf, w, al, Rb); /* from the last evaluation, no recomputation */
        mm3_mul(Rs, Rb, S->site.R);                                   /* site axes in world */
        mm3_tmulv(x, Rs, S->type == SENS_IMU_ACCEL ? sf : w);
        break;
    }
    default: break;
    }
}

static void sensor_sample(MechSim *ms, int i) {
    const SensorDef *S = &ms->sen[i];
    SenState *st = &ms->ss[i];
    int dim = sensor_dim(S->type);
    double truth[6] = {0}, y[6];
    sensor_truth(ms, S, truth);
    for (int k = 0; k < dim; k++) {
        double xk = truth[k] + S->bias + (S->noise_std > 0 ? S->noise_std * gaussian(st) : 0);
        if (S->filter_tau > 0) {
            if (!st->filt_init) st->filt[k] = xk;
            else {
                double al = exp(-S->period / S->filter_tau);
                st->filt[k] = al * st->filt[k] + (1 - al) * xk;
            }
            xk = st->filt[k];
        }
        if (S->resolution > 0) xk = S->resolution * nearbyint(xk / S->resolution);
        y[k] = xk;
    }
    st->filt_init = true;
    int L = S->latency_samples, cap = L + 1;
    st->head = (st->head + 1) % cap;
    memcpy(st->ring[st->head], y, sizeof y);
    if (st->count < cap) st->count++;
    int idx = st->count < cap ? (st->head - (st->count - 1) + cap) % cap : (st->head + 1) % cap; /* oldest kept sample */
    memcpy(st->out, st->ring[idx], sizeof st->out);
    memcpy(st->truth, truth, sizeof truth);
    st->k++;
}

/* ------------------------------------------------------------------------------------------------ controllers */

static void controller_sample(MechSim *ms, int c) {
    const ControllerDef *C = &ms->ctl[c];
    CtlState *st = &ms->cs[c];
    const MbModel *m = ms->m;
    int j = ms->act[C->actuator].joint;
    double t = (double)st->k * C->period, T = C->period;
    double r, rd, rdd, y;
    trajectory_eval(&C->reference, t, &r, &rd, &rdd);
    if (C->feedback_sensor >= 0)
        y = ms->ss[C->feedback_sensor].out[0];
    else
        y = C->control_velocity ? ms->s->v[m->joint_vadr[j]] : ms->s->q[m->joint_qadr[j]];
    double e = C->type == CTRL_OPEN_LOOP ? 0 : r - y, u_unsat = 0; /* an open-loop command has no tracking error */
    if (C->type == CTRL_OPEN_LOOP)
        u_unsat = r;
    else {
        double D = 0;
        if (C->kd != 0 && st->has_prev) {
            double raw = -(y - st->prev_meas) / T;
            if (C->derivative_filter > 0) {
                double al = C->derivative_filter / (C->derivative_filter + T);
                st->dfilt = al * st->dfilt + (1 - al) * raw;
            } else
                st->dfilt = raw;
            D = C->kd * st->dfilt;
        }
        u_unsat = C->kp * e + st->integ + D + C->kff_velocity * rd + C->kff_acceleration * rdd;
    }
    double u = u_unsat;
    bool sat = false;
    if (C->output_max > C->output_min) {
        if (u > C->output_max) u = C->output_max, sat = true;
        if (u < C->output_min) u = C->output_min, sat = true;
    }
    if (C->type == CTRL_PID) {
        switch (C->antiwindup) {
        case AW_NONE: st->integ += C->ki * T * e; break;
        case AW_CLAMP:
            if (!((u_unsat > C->output_max && e * C->ki > 0) || (u_unsat < C->output_min && e * C->ki < 0))) st->integ += C->ki * T * e;
            break;
        case AW_BACK_CALCULATION:
            st->integ += C->ki * T * e + (C->tracking_time > 0 ? T / C->tracking_time : 0) * (u - u_unsat);
            break;
        }
    }
    st->prev_meas = y, st->has_prev = true;
    st->u_unsat = u_unsat, st->u = u, st->reference = r, st->measurement = y, st->error = e;
    st->err_sq += e * e, st->err_max = fmax(st->err_max, fabs(e));
    st->samples++;
    st->saturated += sat;
    /* output delay line: the applied command is the output computed delay_samples periods earlier */
    int cap = C->delay_samples + 1;
    st->dhead = (st->dhead + 1) % cap;
    st->delay[st->dhead] = u;
    double applied = st->k >= (uint64_t)C->delay_samples ? st->delay[(st->dhead + 1) % cap] : ms->act[C->actuator].initial_command;
    if (C->delay_samples == 0) applied = u;
    ms->as[C->actuator].cmd = applied;
    st->k++;
}

/* ------------------------------------------------------------------------------------------------ recorder */

static int add_channel(MechSim *ms, const char *name, const char *unit) {
    char (*nn)[80] = realloc(ms->ch_name, (size_t)(ms->nch + 1) * sizeof *nn);
    if (!nn) return -1;
    ms->ch_name = nn;
    char (*nu)[16] = realloc(ms->ch_unit, (size_t)(ms->nch + 1) * sizeof *nu);
    if (!nu) return -1;
    ms->ch_unit = nu;
    snprintf(ms->ch_name[ms->nch], 80, "%s", name);
    snprintf(ms->ch_unit[ms->nch], 16, "%s", unit);
    return ms->nch++;
}

static void build_channels(MechSim *ms) {
    const MbModel *m = ms->m;
    char nm[80];
    add_channel(ms, "time", "s");
    for (int j = 0; j < m->njoints; j++) {
        const MbJointDef *J = &m->joints[j];
        if (!m->joint_loop[j] && (J->type == MB_REVOLUTE || J->type == MB_PRISMATIC)) {
            bool rot = J->type == MB_REVOLUTE;
            snprintf(nm, sizeof nm, "%s.q", J->name), add_channel(ms, nm, rot ? "rad" : "m");
            snprintf(nm, sizeof nm, "%s.v", J->name), add_channel(ms, nm, rot ? "rad/s" : "m/s");
            snprintf(nm, sizeof nm, "%s.a", J->name), add_channel(ms, nm, rot ? "rad/s^2" : "m/s^2");
            snprintf(nm, sizeof nm, "%s.effort_input", J->name), add_channel(ms, nm, rot ? "N*m" : "N");
            snprintf(nm, sizeof nm, "%s.effort_passive", J->name), add_channel(ms, nm, rot ? "N*m" : "N");
            snprintf(nm, sizeof nm, "%s.effort_constraint", J->name), add_channel(ms, nm, rot ? "N*m" : "N");
        }
        if (!m->joint_loop[j] && J->type == MB_FREE) { /* position of the child joint frame in the parent joint frame */
            snprintf(nm, sizeof nm, "%s.x", J->name), add_channel(ms, nm, "m");
            snprintf(nm, sizeof nm, "%s.y", J->name), add_channel(ms, nm, "m");
            snprintf(nm, sizeof nm, "%s.z", J->name), add_channel(ms, nm, "m");
        }
        static const char *const W[6] = {"Mx", "My", "Mz", "Fx", "Fy", "Fz"};
        for (int k = 0; k < 6; k++) snprintf(nm, sizeof nm, "%s.%s", J->name, W[k]), add_channel(ms, nm, k < 3 ? "N*m" : "N");
    }
    for (int a = 0; a < ms->nact; a++) {
        const char *n = ms->act[a].name;
        snprintf(nm, sizeof nm, "%s.command", n), add_channel(ms, nm, "command");
        snprintf(nm, sizeof nm, "%s.effort", n), add_channel(ms, nm, "N*m|N");
        if (ms->act[a].type == ACT_DC_MOTOR) {
            snprintf(nm, sizeof nm, "%s.voltage", n), add_channel(ms, nm, "V");
            snprintf(nm, sizeof nm, "%s.current", n), add_channel(ms, nm, "A");
            snprintf(nm, sizeof nm, "%s.electrical_power", n), add_channel(ms, nm, "W");
            snprintf(nm, sizeof nm, "%s.losses", n), add_channel(ms, nm, "W");
        }
        snprintf(nm, sizeof nm, "%s.saturated", n), add_channel(ms, nm, "1");
    }
    for (int c = 0; c < ms->nctl; c++) {
        const char *n = ms->ctl[c].name;
        snprintf(nm, sizeof nm, "%s.reference", n), add_channel(ms, nm, "rad|m");
        snprintf(nm, sizeof nm, "%s.measurement", n), add_channel(ms, nm, "rad|m");
        snprintf(nm, sizeof nm, "%s.error", n), add_channel(ms, nm, "rad|m");
        snprintf(nm, sizeof nm, "%s.output", n), add_channel(ms, nm, "command");
    }
    for (int i = 0; i < ms->nsen; i++) {
        int d = sensor_dim(ms->sen[i].type);
        for (int k = 0; k < d; k++) {
            snprintf(nm, sizeof nm, d > 1 ? "%s.measured[%d]" : "%s.measured", ms->sen[i].name, k), add_channel(ms, nm, "SI");
            snprintf(nm, sizeof nm, d > 1 ? "%s.true[%d]" : "%s.true", ms->sen[i].name, k), add_channel(ms, nm, "SI");
        }
    }
    for (int i = 0; i < ms->npairs; i++) {
        const char *a = ms->shapes[ms->pair_a[i]].name, *b = ms->shapes[ms->pair_b[i]].name;
        snprintf(nm, sizeof nm, "contact.%s.%s.normal_force", a, b), add_channel(ms, nm, "N");
        snprintf(nm, sizeof nm, "contact.%s.%s.friction_force", a, b), add_channel(ms, nm, "N");
        snprintf(nm, sizeof nm, "contact.%s.%s.sliding", a, b), add_channel(ms, nm, "1");
        snprintf(nm, sizeof nm, "contact.%s.%s.min_gap", a, b), add_channel(ms, nm, "m");
        snprintf(nm, sizeof nm, "contact.%s.%s.impact", a, b), add_channel(ms, nm, "1");
        snprintf(nm, sizeof nm, "contact.%s.%s.friction_utilization", a, b), add_channel(ms, nm, "1");
    }
    for (int f = 0, it = 0; f < m->nflex; f++) { /* elastic coordinates (mass-normalised), strain energy, interface motion in body axes */
        const MbFlexDef *F = &m->flex[f];
        const char *bn = m->bodies[F->body].name;
        for (int k = 0; k < F->nmodes; k++) snprintf(nm, sizeof nm, "%s.elastic[%d]", bn, k), add_channel(ms, nm, "kg^0.5*m");
        snprintf(nm, sizeof nm, "%s.strain_energy", bn), add_channel(ms, nm, "J");
        for (int i = 0; i < F->ninterfaces; i++, it++) {
            static const char *const D[8] = {"dx", "dy", "dz", "deflection", "rx", "ry", "rz", "rotation"};
            char in[MB_NAME];
            if (ms->flex_itf_joint[it] >= 0) snprintf(in, sizeof in, "%s", m->joints[ms->flex_itf_joint[it]].name);
            else snprintf(in, sizeof in, "interface%d", i);
            for (int k = 0; k < 8; k++) snprintf(nm, sizeof nm, "%s.%s.%s", bn, in, D[k]), add_channel(ms, nm, k < 4 ? "m" : "rad");
        }
    }
    add_channel(ms, "energy.kinetic", "J");
    add_channel(ms, "energy.potential", "J");
    add_channel(ms, "energy.input_work", "J");
    add_channel(ms, "energy.dissipated", "J");
    add_channel(ms, "energy.balance_residual", "J");
    add_channel(ms, "constraint.residual", "1");
}

static bool record_row(MechSim *ms) {
    if (ms->rows >= ms->st.max_records) {
        ms->record_full = true;
        return true;
    }
    if (ms->rows == ms->cap_rows) {
        int nc = ms->cap_rows ? 2 * ms->cap_rows : 1024;
        if (nc > ms->st.max_records) nc = ms->st.max_records;
        double *d = calloc((size_t)ms->nch * (size_t)nc, sizeof *d);
        if (!d) return false;
        for (int c = 0; c < ms->nch && ms->data; c++) memcpy(d + (size_t)c * (size_t)nc, ms->data + (size_t)c * (size_t)ms->cap_rows, (size_t)ms->rows * sizeof *d);
        free(ms->data);
        ms->data = d, ms->cap_rows = nc;
    }
    const MbModel *m = ms->m;
    const MbEval *ev = ms->ev;
    int col = 0, r = ms->rows;
#define PUT(x) ms->data[(size_t)(col++) * (size_t)ms->cap_rows + (size_t)r] = (x)
    PUT(ms->s->t);
    for (int j = 0; j < m->njoints; j++) {
        const MbJointDef *J = &m->joints[j];
        if (!m->joint_loop[j] && (J->type == MB_REVOLUTE || J->type == MB_PRISMATIC)) {
            int qa = m->joint_qadr[j], va = m->joint_vadr[j];
            PUT(ms->s->q[qa]);
            PUT(ms->s->v[va]);
            PUT(ev->qacc[va]);
            PUT(ev->tau_input[va]);
            PUT(ev->tau_passive[va]);
            PUT(ev->tau_constraint[va]);
        }
        if (!m->joint_loop[j] && J->type == MB_FREE)
            for (int k = 0; k < 3; k++) PUT(ms->s->q[m->joint_qadr[j] + k]);
        for (int k = 0; k < 6; k++) PUT(ev->joint_wrench[6 * j + k]);
    }
    for (int a = 0; a < ms->nact; a++) {
        const ActuatorOutput *o = &ms->as[a].out;
        PUT(ms->as[a].cmd);
        PUT(o->effort);
        if (ms->act[a].type == ACT_DC_MOTOR) {
            PUT(o->voltage);
            PUT(o->current);
            PUT(o->electrical_power);
            PUT(o->copper_loss + o->friction_loss + o->gear_loss);
        }
        PUT(o->saturated ? 1.0 : 0.0);
    }
    for (int c = 0; c < ms->nctl; c++) {
        PUT(ms->cs[c].reference);
        PUT(ms->cs[c].measurement);
        PUT(ms->cs[c].error);
        PUT(ms->cs[c].u);
    }
    for (int i = 0; i < ms->nsen; i++) {
        int d = sensor_dim(ms->sen[i].type);
        for (int k = 0; k < d; k++) {
            PUT(ms->ss[i].out[k]);
            PUT(ms->ss[i].truth[k]);
        }
    }
    for (int i = 0; i < ms->npairs; i++)
        for (int k = 0; k < 6; k++) PUT(k == 3 && isinf(ms->pair_now[i][3]) ? NAN : ms->pair_now[i][k]);
    for (int f = 0; f < m->nflex; f++) {
        const MbFlexDef *F = &m->flex[f];
        const double *eta = ms->s->q + m->flex_qadr[f];
        for (int k = 0; k < F->nmodes; k++) PUT(eta[k]);
        PUT(flex_strain_energy(F, eta));
        for (int i = 0; i < F->ninterfaces; i++) {
            double dd[3], rr[3];
            flex_motion(F, i, eta, dd, rr);
            PUT(dd[0]), PUT(dd[1]), PUT(dd[2]), PUT(mv3_norm(dd)), PUT(rr[0]), PUT(rr[1]), PUT(rr[2]), PUT(mv3_norm(rr));
        }
    }
    const MbLedger *lg = &ms->s->ledger;
    PUT(ev->kinetic);
    PUT(ev->potential_gravity + ev->potential_spring);
    PUT(lg->work_inputs + lg->work_external);
    PUT(lg->dissipated_damping + lg->dissipated_friction + lg->dissipated_impacts + lg->contact_friction + lg->contact_impacts);
    PUT(ev->energy_residual);
    PUT(ev->cinfo.residual);
#undef PUT
    ms->rows++;
    return true;
}

int mechsim_channels(const MechSim *ms) { return ms->nch; }
const char *mechsim_channel_name(const MechSim *ms, int c) { return c >= 0 && c < ms->nch ? ms->ch_name[c] : NULL; }
const char *mechsim_channel_unit(const MechSim *ms, int c) { return c >= 0 && c < ms->nch ? ms->ch_unit[c] : NULL; }
int mechsim_channel_index(const MechSim *ms, const char *name) {
    for (int c = 0; name && c < ms->nch; c++)
        if (!strcmp(ms->ch_name[c], name)) return c;
    return -1;
}
int mechsim_rows(const MechSim *ms) { return ms->rows; }
const double *mechsim_column(const MechSim *ms, int c) { return c >= 0 && c < ms->nch && ms->data ? ms->data + (size_t)c * (size_t)ms->cap_rows : NULL; }

/* ------------------------------------------------------------------------------------------------ setup */

static bool check_positive(MechDiag *d, const char *who, const char *what, double v, bool allow_zero) {
    if (isfinite(v) && (allow_zero ? v >= 0 : v > 0)) return true;
    mdiag_add(d, MD_MISSING_INPUT, "INVALID_PARAMETER", who, NULL, "%s must be %s (got %g)", what, allow_zero ? ">= 0" : "> 0", v);
    return false;
}

bool mechsim_init(MechSim *ms, MechDiag *d) {
    const MbModel *m = ms->m;
    bool ok = true;
    if (ms->nact * AUX_PER_ACT > MB_AUX_MAX) {
        mdiag_add(d, MD_ERROR, "RESOURCE_LIMIT", "actuators", NULL, "at most %d actuators per study", MB_AUX_MAX / AUX_PER_ACT);
        return false;
    }
    if (!(ms->st.end_time > 0) || !(ms->st.max_step > 0)) {
        mdiag_add(d, MD_MISSING_INPUT, "STUDY_TIME", "study", "give end_time and max_step", "the study needs a positive end time and maximum step");
        ok = false;
    }
    const MbJointDef *joints = m->joints;
    for (int a = 0; a < ms->nact; a++) {
        ActuatorDef *A = &ms->act[a];
        if (!scalar_joint(m, A->joint)) {
            mdiag_add(d, MD_ERROR, "ACTUATOR_JOINT", A->name, "actuate a revolute or prismatic tree joint", "actuator '%s' is not on a scalar joint", A->name);
            ok = false;
            continue;
        }
        if ((unsigned)A->type >= ACT_TYPES) {
            mdiag_add(d, MD_ERROR, "ACTUATOR_TYPE", A->name, NULL, "unknown actuator type");
            ok = false;
            continue;
        }
        for (int b = 0; b < a; b++)
            if (ms->act[b].joint == A->joint)
                mdiag_add(d, MD_WARNING, "ACTUATORS_SHARE_JOINT", A->name, NULL, "actuators '%s' and '%s' both drive joint '%s': their efforts add", ms->act[b].name,
                          A->name, joints[A->joint].name);
        switch (A->type) {
        case ACT_EFFORT:
            if (!(A->effort_limit > 0))
                mdiag_add(d, MD_WARNING, "UNLIMITED_EFFORT", A->name, "set effort_limit from the actuator rating", "effort actuator '%s' has no effort limit", A->name);
            break;
        case ACT_POSITION_SERVO:
        case ACT_VELOCITY_SERVO:
            ok &= check_positive(d, A->name, "effort_limit (servo stall effort)", A->effort_limit, false);
            ok &= A->type == ACT_POSITION_SERVO ? check_positive(d, A->name, "kp", A->kp, false) : check_positive(d, A->name, "kv", A->kv, false);
            break;
        case ACT_DC_MOTOR:
            ok &= check_positive(d, A->name, "resistance", A->resistance, false);
            ok &= check_positive(d, A->name, "torque_constant", A->torque_constant, false);
            ok &= check_positive(d, A->name, "back_emf_constant", A->back_emf_constant, false);
            ok &= check_positive(d, A->name, "gear_ratio", A->gear_ratio, false);
            ok &= check_positive(d, A->name, "voltage_limit", A->voltage_limit, false);
            ok &= check_positive(d, A->name, "rotor_inertia", A->rotor_inertia, true);
            if (!(A->efficiency > 0 && A->efficiency <= 1)) {
                mdiag_add(d, MD_MISSING_INPUT, "INVALID_PARAMETER", A->name, "give the gearbox efficiency from the datasheet (0 < eta <= 1)", "efficiency must be in (0, 1]");
                ok = false;
            }
            if (A->motor_coulomb > 0 && !(A->motor_coulomb_vreg > 0)) {
                mdiag_add(d, MD_MISSING_INPUT, "FRICTION_REGULARISATION", A->name, NULL, "motor Coulomb friction needs motor_coulomb_vreg");
                ok = false;
            }
            if (A->torque_constant > 0 && fabs(A->torque_constant - A->back_emf_constant) > 0.02 * A->torque_constant)
                mdiag_add(d, MD_WARNING, "MOTOR_CONSTANTS_INCONSISTENT", A->name, "in SI units kt (N m/A) equals ke (V s/rad) for an ideal DC motor",
                          "torque constant %.6g N m/A and back-EMF constant %.6g V s/rad differ by more than 2%%: the energy balance will not close",
                          A->torque_constant, A->back_emf_constant);
            break;
        default: break;
        }
        ms->as[a].cmd = A->initial_command;
    }
    for (int c = 0; c < ms->nctl; c++) {
        ControllerDef *C = &ms->ctl[c];
        if (C->actuator < 0 || C->actuator >= ms->nact) {
            mdiag_add(d, MD_ERROR, "CONTROLLER_ACTUATOR", C->name, NULL, "controller '%s' drives no actuator", C->name);
            ok = false;
            continue;
        }
        ok &= check_positive(d, C->name, "period", C->period, false);
        if (C->delay_samples < 0 || C->delay_samples > MS_MAX_DELAY) {
            mdiag_add(d, MD_ERROR, "CONTROLLER_DELAY", C->name, NULL, "delay_samples must be 0..%d", MS_MAX_DELAY);
            ok = false;
        }
        if (C->feedback_sensor >= ms->nsen) {
            mdiag_add(d, MD_ERROR, "CONTROLLER_SENSOR", C->name, NULL, "feedback sensor index out of range");
            ok = false;
        } else if (C->feedback_sensor < 0 && C->type == CTRL_PID)
            mdiag_add(d, MD_WARNING, "IDEAL_FEEDBACK", C->name, "attach a sensor to include noise, resolution, filtering and latency",
                      "controller '%s' uses ideal state feedback (exact joint state without sensor effects)", C->name);
        if (C->antiwindup == AW_BACK_CALCULATION && !(C->tracking_time > 0)) {
            mdiag_add(d, MD_MISSING_INPUT, "INVALID_PARAMETER", C->name, "a common choice is sqrt(Ti * Td) or Ti", "back-calculation anti-windup needs tracking_time > 0");
            ok = false;
        }
        for (int e = 0; e < c; e++)
            if (ms->ctl[e].actuator == C->actuator)
                mdiag_add(d, MD_ERROR, "ACTUATOR_DOUBLY_CONTROLLED", C->name, NULL, "controllers '%s' and '%s' command the same actuator", ms->ctl[e].name, C->name);
    }
    for (int i = 0; i < ms->nsen; i++) {
        SensorDef *S = &ms->sen[i];
        ok &= check_positive(d, S->name, "period", S->period, false);
        if (S->latency_samples < 0 || S->latency_samples > MS_MAX_LATENCY) {
            mdiag_add(d, MD_ERROR, "SENSOR_LATENCY", S->name, NULL, "latency_samples must be 0..%d", MS_MAX_LATENCY);
            ok = false;
        }
        bool target_ok = true;
        switch (S->type) {
        case SENS_JOINT_POSITION:
        case SENS_JOINT_VELOCITY: target_ok = scalar_joint(m, S->joint); break;
        case SENS_JOINT_EFFORT: target_ok = S->actuator >= 0 && S->actuator < ms->nact; break;
        case SENS_MOTOR_CURRENT: target_ok = S->actuator >= 0 && S->actuator < ms->nact && ms->act[S->actuator].type == ACT_DC_MOTOR; break;
        case SENS_FORCE_TORQUE: target_ok = S->joint >= 0 && S->joint < m->njoints; break;
        case SENS_IMU_ACCEL:
        case SENS_IMU_GYRO: target_ok = S->body >= 0 && S->body < m->nbodies; break;
        default: target_ok = false;
        }
        if (!target_ok) {
            mdiag_add(d, MD_ERROR, "SENSOR_TARGET", S->name, NULL, "sensor '%s' (%s) has no valid joint, actuator or body", S->name, sensor_type_name(S->type));
            ok = false;
        }
        if (S->noise_std < 0 || S->resolution < 0 || S->filter_tau < 0) {
            mdiag_add(d, MD_ERROR, "INVALID_PARAMETER", S->name, NULL, "noise_std, resolution and filter_tau must be >= 0");
            ok = false;
        }
        xoshiro_seed(ms->ss[i].rng, S->seed);
    }
    for (int f = 0; f < m->nflex; f++) {
        const char *bn = m->bodies[m->flex[f].body].name;
        for (int i = 0; ms->st.contact && i < ms->nshapes; i++)
            if (ms->shapes[i].body == m->flex[f].body) {
                mdiag_add(d, MD_ERROR, "CONTACT_ON_FLEXIBLE_BODY", ms->shapes[i].name,
                          "mount the contact geometry on a body attached to an interface of the flexible body (the interface carries the deformation)",
                          "contact shape '%s' is on flexible body '%s': contact geometry following only its reference frame would ignore the deformation",
                          ms->shapes[i].name, bn);
                ok = false;
            }
        for (int i = 0; i < ms->nsen; i++)
            if ((ms->sen[i].type == SENS_IMU_ACCEL || ms->sen[i].type == SENS_IMU_GYRO) && ms->sen[i].body == m->flex[f].body)
                mdiag_add(d, MD_WARNING, "SENSOR_ON_FLEXIBLE_BODY", ms->sen[i].name, "mount the IMU on a body attached to an interface to measure the deformation",
                          "IMU '%s' on flexible body '%s' follows the reference frame held at the root region, not the deformed part", ms->sen[i].name, bn);
    }
    if (ms->flex_wmax > 0 && ms->flex_step < ms->st.max_step)
        mdiag_add(d, MD_INFO, "FLEXIBLE_STEP_LIMIT", "study", "reduce the flexible bodies with max_frequency_hz to allow longer steps (the loss of interface flexibility is reported)",
                  "steps are limited to %.3g s (0.5 / w_max, highest elastic frequency %.4g Hz) below max_step %.3g s", ms->flex_step, ms->flex_wmax / (2 * M_PI),
                  ms->st.max_step);
    if (!ok) return false;
    if (!mb_state_initial(m, ms->s, d) || !mb_assemble(ms->sim, ms->s, d)) return false;
    if (m->nflex && !ms->st.flexible_undeformed) {
        double res;
        int it = mb_flex_equilibrium(ms->sim, ms->s, 1e-12, 100, &res);
        if (it < 0) {
            mdiag_add(d, MD_ERROR, "FLEXIBLE_EQUILIBRIUM", "study", "check the flexible bodies' stiffness against their loads, or start them undeformed",
                      "the elastic coordinates found no static equilibrium under the initial loads (relative residual %.3g)", res);
            return false;
        }
        ms->flex_eq_iterations = it, ms->flex_eq_residual = res;
        mdiag_add(d, MD_INFO, "FLEXIBLE_INITIAL_EQUILIBRIUM", "study", NULL,
                  "flexible bodies start in static equilibrium under gravity and the initial motion with the joints held (%d iterations, relative residual %.2g)",
                  it, res);
    }
    ms->record_period = ms->st.record_period > 0 ? ms->st.record_period : fmax(ms->st.max_step, ms->st.end_time / 1000);
    if (ms->st.contact) {
        if (!ms->nshapes)
            mdiag_add(d, MD_WARNING, "CONTACT_WITHOUT_SHAPES", "contact", "add collision geometry to the bodies and environment shapes",
                      "contact is enabled but no contact shapes are defined");
        ms->crows = malloc(CT_MAX_POINTS * sizeof *ms->crows);
        ms->row_sa = malloc(CT_MAX_POINTS * sizeof(int));
        ms->row_sb = malloc(CT_MAX_POINTS * sizeof(int));
        ms->cwrench = calloc((size_t)(6 * (m->nbodies ? m->nbodies : 1)), sizeof(double));
        ms->cposes = calloc((size_t)(m->nbodies ? m->nbodies : 1), sizeof *ms->cposes);
        ms->env_impulse = calloc((size_t)(m->njoints ? m->njoints : 1), sizeof(double));
        ms->peak_cap = calloc((size_t)(2 * (m->njoints ? m->njoints : 1)), sizeof *ms->peak_cap);
        if (!ms->crows || !ms->row_sa || !ms->row_sb || !ms->cwrench || !ms->cposes || !ms->env_impulse || !ms->peak_cap) return false;
        for (int i = 0; i < ms->nshapes; i++)
            for (int j = i + 1; j < ms->nshapes; j++) {
                const ContactShape *A = &ms->shapes[i], *B = &ms->shapes[j];
                if (A->body == B->body || (A->group && A->group == B->group) || (A->body < 0 && B->body < 0) || pair_jointed(m, A->body, B->body)) continue;
                if (ms->npairs == 64) {
                    mdiag_add(d, MD_WARNING, "CONTACT_PAIRS_LIMIT", "contact", NULL, "more than 64 shape pairs can touch: only the first 64 get their own histories");
                    goto pairs_done;
                }
                ms->pair_a[ms->npairs] = i, ms->pair_b[ms->npairs] = j;
                ms->pair_min_gap[ms->npairs] = INFINITY;
                ms->npairs++;
            }
    pairs_done:
        ms->pair_cap = calloc((size_t)(ms->npairs ? ms->npairs : 1), sizeof *ms->pair_cap);
        if (!ms->pair_cap) return false;
        {
            ContactOptions co;
            contact_options_default(&co);
            if (!contact_warm_init(&ms->warm, contact_warm_capacity(m, ms->shapes, ms->nshapes, &co))) return false;
        }
        for (int i = 0; i < 64; i++) ms->pair_blob_t[i] = NAN;
        mdiag_add(d, MD_INFO, "CONTACT_TIME_STEPPING", "contact", NULL,
                  "contact is on: the study integrates with first-order Moreau-Jean time stepping (impulses, Coulomb friction, restitution); report the "
                  "sensitivity to the time step");
    }
    build_channels(ms);
    ms->blob_size = mechsim_state_size(ms);
    size_t nj = (size_t)(m->njoints ? m->njoints : 1);
    ms->peak_blob = calloc(2 * nj, ms->blob_size);
    ms->peak_blob_t = malloc(2 * nj * sizeof(double));
    if (!ms->peak_blob || !ms->peak_blob_t) {
        mdiag_add(d, MD_ERROR, "OUT_OF_MEMORY", "study", NULL, "out of memory for load snapshots");
        return false;
    }
    for (size_t i = 0; i < 2 * nj; i++) ms->peak_blob_t[i] = NAN;
    ms->initialized = true;
    return true;
}

/* ------------------------------------------------------------------------------------------------ stepping */

static bool due(double t, double tk) { return tk <= t + 1e-12 * fmax(1.0, fabs(t)); }

static bool evaluate(MechSim *ms) {
    MbInputs in = {NULL, ms->cwrench, act_force, ms}; /* contact forces of the last step enter as body wrenches (NULL without contact) */
    if (!mb_evaluate(ms->sim, ms->s, &in, ms->ev)) {
        snprintf(ms->err, sizeof ms->err, "evaluation failed at t = %.9g s", ms->s->t);
        return false;
    }
    actuators_evaluate(ms);
    return true;
}

static void capture(const MechSim *ms, uint8_t *dst) { mechsim_save_state(ms, dst, ms->blob_size); }

static void flex_envelopes(MechSim *ms) {
    const MbModel *m = ms->m;
    double t = ms->s->t;
    bool first = ms->steps_total == 0;
    for (int f = 0, it = 0; f < m->nflex; f++) {
        const MbFlexDef *F = &m->flex[f];
        const double *eta = ms->s->q + m->flex_qadr[f];
        for (int i = 0; i < F->ninterfaces; i++, it++) {
            double dd[3], rr[3], *e = ms->flex_env + 4 * it;
            flex_motion(F, i, eta, dd, rr);
            double dn = mv3_norm(dd), rn = mv3_norm(rr);
            if (dn > e[0] || first) e[0] = dn, e[1] = t;
            if (rn > e[2] || first) e[2] = rn, e[3] = t;
        }
        double us = flex_strain_energy(F, eta), *e = ms->flex_env + 4 * ms->nflex_itf + 2 * f;
        if (us > e[0] || first) e[0] = us, e[1] = t;
    }
}

static void update_envelopes(MechSim *ms) {
    const MbModel *m = ms->m;
    flex_envelopes(ms); /* state quantities: valid on impact steps too */
    if (ms->step_impulsive) { /* impact: the step-average forces scale with 1/step; keep the impulse instead */
        for (int j = 0; j < m->njoints; j++) {
            const double *w = ms->ev->joint_wrench + 6 * j;
            ms->env_impulse[j] = fmax(ms->env_impulse[j], sqrt(w[3] * w[3] + w[4] * w[4] + w[5] * w[5]) * ms->last_h);
        }
        return;
    }
    for (int j = 0; j < m->njoints; j++) {
        const double *w = ms->ev->joint_wrench + 6 * j;
        double F = sqrt(w[3] * w[3] + w[4] * w[4] + w[5] * w[5]), M = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        double *e = ms->env + 16 * j;
        if (F > e[0] || ms->steps_total == 0) {
            e[0] = F, e[1] = ms->s->t;
            memcpy(e + 2, w + 3, 3 * sizeof(double));
            memcpy(e + 5, w, 3 * sizeof(double));
            if (ms->peak_blob) capture(ms, ms->peak_blob + (size_t)(2 * j) * ms->blob_size), ms->peak_blob_t[2 * j] = ms->s->t;
            if (ms->peak_cap && !capture_rows(ms, &ms->peak_cap[2 * j])) ms->peak_blob_t[2 * j] = NAN;
        }
        if (M > e[8] || ms->steps_total == 0) {
            e[8] = M, e[9] = ms->s->t;
            memcpy(e + 10, w + 3, 3 * sizeof(double));
            memcpy(e + 13, w, 3 * sizeof(double));
            if (ms->peak_blob) capture(ms, ms->peak_blob + (size_t)(2 * j + 1) * ms->blob_size), ms->peak_blob_t[2 * j + 1] = ms->s->t;
            if (ms->peak_cap && !capture_rows(ms, &ms->peak_cap[2 * j + 1])) ms->peak_blob_t[2 * j + 1] = NAN;
        }
    }
}

static bool process_events(MechSim *ms) {
    double t = ms->s->t;
    bool any_sensor = false, any_ctl = false;
    for (int i = 0; i < ms->nsen; i++) any_sensor |= due(t, (double)ms->ss[i].k * ms->sen[i].period);
    for (int c = 0; c < ms->nctl; c++) any_ctl |= due(t, (double)ms->cs[c].k * ms->ctl[c].period);
    bool rec = due(t, (double)ms->rec_k * ms->record_period);
    bool snap = ms->snap_next < (uint64_t)ms->nsnap && due(t, ms->snap_time[ms->snap_next]);
    if (!any_sensor && !any_ctl && !rec && !snap) return true;
    if (!evaluate(ms)) return false;
    for (int i = 0; i < ms->nsen; i++)
        if (due(t, (double)ms->ss[i].k * ms->sen[i].period)) sensor_sample(ms, i);
    bool commands_changed = false;
    for (int c = 0; c < ms->nctl; c++)
        if (due(t, (double)ms->cs[c].k * ms->ctl[c].period)) controller_sample(ms, c), commands_changed = true;
    if (snap) { /* after sensors and controllers of this instant, like the recorded row */
        if (commands_changed && !evaluate(ms)) return false;
        capture(ms, ms->snap_blob + (size_t)ms->snap_next * ms->blob_size);
        ms->snap_have[ms->snap_next] = true;
        if (ms->st.contact && !capture_rows(ms, &ms->snap_cap[ms->snap_next])) {
            snprintf(ms->err, sizeof ms->err, "out of memory capturing contact rows");
            return false;
        }
        ms->snap_next++;
    }
    if (rec) {
        if (commands_changed && !evaluate(ms)) return false; /* record the efforts of the commands now held */
        if (!record_row(ms)) {
            snprintf(ms->err, sizeof ms->err, "out of memory recording histories");
            return false;
        }
        ms->rec_k++;
    }
    return true;
}

static double next_event(const MechSim *ms, double t_target) {
    double t = ms->s->t, n = fmin(t_target, t + fmin(ms->st.max_step, ms->flex_step));
    for (int i = 0; i < ms->nsen; i++) n = fmin(n, (double)ms->ss[i].k * ms->sen[i].period);
    for (int c = 0; c < ms->nctl; c++) n = fmin(n, (double)ms->cs[c].k * ms->ctl[c].period);
    n = fmin(n, (double)ms->rec_k * ms->record_period);
    if (ms->snap_next < (uint64_t)ms->nsnap) n = fmin(n, ms->snap_time[ms->snap_next]);
    return n;
}

bool mechsim_run_until(MechSim *ms, double t_target, char *err, size_t errlen) {
    if (!ms->initialized) {
        snprintf(err, errlen, "runtime not initialised");
        return false;
    }
    if (t_target > ms->st.end_time) t_target = ms->st.end_time;
    if (ms->steps_total == 0 && ms->rows == 0) {
        if (!process_events(ms)) goto fail;
        update_envelopes(ms);
    }
    while (ms->s->t < t_target - 1e-12 * fmax(1.0, t_target)) {
        double tn = next_event(ms, t_target), h = tn - ms->s->t;
        if (!(h > 0)) {
            snprintf(ms->err, sizeof ms->err, "scheduler stalled at t = %.17g s", ms->s->t);
            goto fail;
        }
        MbInputs in = {NULL, NULL, act_force, ms};
        MbStepReport rep;
        if (ms->st.contact) {
            ContactOptions co;
            contact_options_default(&co);
            if (ms->st.contact_margin > 0) co.margin = ms->st.contact_margin;
            if (ms->st.contact_max_iterations > 0) co.max_iterations = ms->st.contact_max_iterations;
            if (ms->st.contact_tolerance > 0) co.tolerance = ms->st.contact_tolerance;
            if (ms->st.contact_restitution_speed > 0) co.restitution_speed = ms->st.contact_restitution_speed;
            ContactReport crep;
            if (!contact_step(ms->sim, ms->s, h, &in, ms->shapes, ms->nshapes, &co, ms->crows, ms->row_sa, ms->row_sb, &ms->ncrows, &crep, &rep, &ms->warm)) {
                snprintf(ms->err, sizeof ms->err, "t = %.9g s: %s", ms->s->t, rep.error);
                goto fail;
            }
            if (crep.unsupported_pairs) {
                snprintf(ms->err, sizeof ms->err, "contact between %s is not supported in this build", crep.unsupported);
                goto fail;
            }
            ms->last_h = h;
            ms->contact_unconverged += !crep.solve.converged;
            ms->contact_max_iter = crep.solve.iterations > ms->contact_max_iter ? crep.solve.iterations : ms->contact_max_iter;
            ms->contact_max_penetration = fmax(ms->contact_max_penetration, crep.max_penetration);
        } else if (!mb_step(ms->sim, ms->s, h, &in, &rep)) {
            snprintf(ms->err, sizeof ms->err, "t = %.9g s: %s", ms->s->t, rep.error);
            goto fail;
        }
        ms->s->t = tn; /* land exactly on the event instant */
        if (ms->st.contact) {
            contact_wrenches(ms, h);
            ms->impulsive_steps += ms->step_impulsive;
            for (int i = 0; i < ms->npairs; i++) {
                ms->pair_now[i][0] = ms->pair_now[i][1] = ms->pair_now[i][2] = ms->pair_now[i][4] = ms->pair_now[i][5] = 0;
                ms->pair_now[i][3] = INFINITY;
            }
            double ft[64][3], pn[64];
            memset(ft, 0, sizeof ft), memset(pn, 0, sizeof pn);
            for (int r = 0; r < ms->ncrows; r++) {
                int pi = pair_index(ms, ms->row_sa[r], ms->row_sb[r]);
                if (pi < 0) continue;
                const MbContactRow *C = &ms->crows[r];
                pn[pi] += C->impulse_normal;
                for (int k = 0; k < 3; k++) ft[pi][k] += (C->impulse_tangent[0] * C->tangent[0][k] + C->impulse_tangent[1] * C->tangent[1][k]) / h;
                if (C->state == 2) ms->pair_now[pi][2] = 1;
                ms->pair_now[pi][3] = fmin(ms->pair_now[pi][3], C->gap);
                if (C->impact && C->impulse_normal > 0) ms->pair_now[pi][4] = 1;
            }
            for (int i = 0; i < ms->npairs; i++) {
                ms->pair_now[i][0] = pn[i] / h;
                ms->pair_now[i][1] = mv3_norm(ft[i]);
                if (pn[i] > 0) { /* friction utilisation of the pair: 1 = at the Coulomb limit (sliding); frictionless contact has no margin */
                    double mu = fmin(ms->shapes[ms->pair_a[i]].friction, ms->shapes[ms->pair_b[i]].friction);
                    ms->pair_now[i][5] = mu > 0 ? fmin(1.0, ms->pair_now[i][1] / (mu * ms->pair_now[i][0])) : 1.0;
                }
                if (ms->pair_now[i][2] > 0) ms->pair_slide_time[i] += h;
                if (ms->pair_now[i][4] > 0) {
                    ms->pair_impacts[i]++;
                    ms->pair_run_impulse[i] += pn[i];
                    ms->pair_peak_impulse[i] = fmax(ms->pair_peak_impulse[i], ms->pair_run_impulse[i]);
                } else
                    ms->pair_run_impulse[i] = 0;
                if (ms->pair_now[i][3] < ms->pair_min_gap[i]) ms->pair_min_gap[i] = ms->pair_now[i][3];
                if (ms->step_impulsive) continue; /* force peaks come from persistent contact only */
                ms->pair_peak_t[i] = fmax(ms->pair_peak_t[i], ms->pair_now[i][1]);
                if (ms->pair_now[i][5] > ms->pair_util_peak[i]) ms->pair_util_peak[i] = ms->pair_now[i][5], ms->pair_util_peak_t[i] = ms->s->t;
                if (ms->pair_now[i][0] > ms->pair_peak_n[i]) {
                    ms->pair_peak_n[i] = ms->pair_now[i][0];
                    ms->pair_peak_f[i] = ms->s->t;
                    if (!ms->pair_blob) ms->pair_blob = calloc((size_t)(ms->npairs ? ms->npairs : 1), ms->blob_size);
                    ms->pair_blob_t[i] = NAN;
                    if (ms->pair_blob && capture_rows(ms, &ms->pair_cap[i]) && mechsim_save_state(ms, ms->pair_blob + (size_t)i * ms->blob_size, ms->blob_size))
                        ms->pair_blob_t[i] = ms->s->t;
                }
            }
        }
        ms->drift_max = fmax(ms->drift_max, rep.drift_before);
        ms->vdrift_max = fmax(ms->vdrift_max, rep.vdrift_before);
        ms->events_total += (uint64_t)rep.events;
        ms->steps_total++;
        if (!evaluate(ms)) goto fail;
        for (int a = 0; a < ms->nact; a++) {
            ActState *A = &ms->as[a];
            A->peak_effort = fmax(A->peak_effort, fabs(A->out.effort));
            A->peak_current = fmax(A->peak_current, fabs(A->out.current));
            A->over_rating_steps += A->out.over_rating;
        }
        update_envelopes(ms);
        if (!process_events(ms)) goto fail;
    }
    return true;
fail:
    snprintf(err, errlen, "%s", ms->err[0] ? ms->err : "run failed");
    return false;
}

bool mechsim_actuator_output(const MechSim *ms, int a, ActuatorOutput *out) {
    if (a < 0 || a >= ms->nact) return false;
    *out = ms->as[a].out;
    return true;
}

const MbEval *mechsim_evaluate(MechSim *ms) { return evaluate(ms) ? ms->ev : NULL; }

JsonValue *mechsim_last_contact_json(const MechSim *ms, double *step, bool *impulsive) {
    if (!ms->st.contact || !ms->crows) return NULL;
    RowCapture view = {ms->ncrows, ms->ncrows, ms->last_h, ms->crows, ms->row_sa, ms->row_sb};
    if (step) *step = ms->last_h;
    if (impulsive) *impulsive = ms->step_impulsive != 0;
    return capture_json(ms, &view);
}

int mechsim_impulsive_steps(const MechSim *ms) { return ms->impulsive_steps; }

bool mechsim_joint_wrenches(MechSim *ms, double *w) {
    if (!evaluate(ms)) return false;
    memcpy(w, ms->ev->joint_wrench, (size_t)(6 * ms->m->njoints) * sizeof(double));
    return true;
}

/* ------------------------------------------------------------------------------------------------ checkpoint */

typedef struct Writer {
    uint8_t *buf;
    size_t cap, len;
    bool ok;
} Writer;

static void wr(Writer *w, const void *p, size_t n) {
    if (w->buf && w->len + n <= w->cap) memcpy(w->buf + w->len, p, n);
    else if (w->buf) w->ok = false;
    w->len += n;
}

static void write_state(const MechSim *ms, Writer *w) {
    const MbState *s = ms->s;
    static const char MAGIC[8] = {'N', 'V', 'M', 'S', 'C', 'K', 'P', '1'};
    wr(w, MAGIC, 8);
    wr(w, &s->t, sizeof s->t);
    wr(w, s->aux, sizeof s->aux);
    wr(w, s->q, (size_t)s->nq * sizeof *s->q);
    wr(w, s->v, (size_t)s->nv * sizeof *s->v);
    wr(w, s->limit_state, (size_t)s->nlimits);
    wr(w, &s->ledger, sizeof s->ledger);
    wr(w, &s->energy0, sizeof s->energy0);
    wr(w, &s->steps, sizeof s->steps);
    wr(w, &s->events, sizeof s->events);
    wr(w, ms->as, (size_t)ms->nact * sizeof *ms->as);
    wr(w, ms->cs, (size_t)ms->nctl * sizeof *ms->cs);
    wr(w, ms->ss, (size_t)ms->nsen * sizeof *ms->ss);
    wr(w, &ms->rec_k, sizeof ms->rec_k);
    wr(w, &ms->rows, sizeof ms->rows);
    wr(w, ms->env, (size_t)(16 * ms->m->njoints) * sizeof *ms->env);
    wr(w, &ms->drift_max, sizeof ms->drift_max);
    wr(w, &ms->vdrift_max, sizeof ms->vdrift_max);
    wr(w, &ms->steps_total, sizeof ms->steps_total);
    wr(w, &ms->events_total, sizeof ms->events_total);
    wr(w, &ms->record_full, sizeof ms->record_full);
    /* contact outputs (per used pair) and the solver warm start */
    size_t np = (size_t)ms->npairs;
    wr(w, &ms->npairs, sizeof ms->npairs);
    wr(w, ms->pair_now, np * sizeof ms->pair_now[0]);
    wr(w, ms->pair_peak_n, np * sizeof(double));
    wr(w, ms->pair_peak_t, np * sizeof(double));
    wr(w, ms->pair_peak_f, np * sizeof(double));
    wr(w, ms->pair_min_gap, np * sizeof(double));
    wr(w, ms->pair_slide_time, np * sizeof(double));
    wr(w, ms->pair_impacts, np * sizeof(int));
    wr(w, ms->pair_peak_impulse, np * sizeof(double));
    wr(w, ms->pair_run_impulse, np * sizeof(double));
    wr(w, ms->pair_util_peak, np * sizeof(double));
    wr(w, ms->pair_util_peak_t, np * sizeof(double));
    wr(w, &ms->contact_unconverged, sizeof ms->contact_unconverged);
    wr(w, &ms->contact_max_iter, sizeof ms->contact_max_iter);
    wr(w, &ms->contact_max_penetration, sizeof ms->contact_max_penetration);
    wr(w, &ms->last_h, sizeof ms->last_h);
    wr(w, &ms->step_impulsive, sizeof ms->step_impulsive);
    wr(w, &ms->impulsive_steps, sizeof ms->impulsive_steps);
    if (ms->cwrench) wr(w, ms->cwrench, (size_t)(6 * ms->m->nbodies) * sizeof(double));
    if (ms->env_impulse) wr(w, ms->env_impulse, (size_t)ms->m->njoints * sizeof(double));
    wr(w, ms->flex_env, (size_t)(4 * ms->nflex_itf + 2 * ms->m->nflex) * sizeof(double));
    size_t wc = (size_t)ms->warm.cap; /* fixed size: the whole capacity */
    wr(w, &ms->warm.n, sizeof ms->warm.n);
    if (wc) {
        wr(w, ms->warm.sa, wc * sizeof(int)), wr(w, ms->warm.sb, wc * sizeof(int));
        wr(w, ms->warm.point, 3 * wc * sizeof(double)), wr(w, ms->warm.force, 4 * wc * sizeof(double));
    }
}

size_t mechsim_state_size(const MechSim *ms) {
    Writer w = {NULL, 0, 0, true};
    write_state(ms, &w);
    return w.len;
}

bool mechsim_save_state(const MechSim *ms, uint8_t *buf, size_t cap) {
    Writer w = {buf, cap, 0, true};
    write_state(ms, &w);
    return w.ok && w.len <= cap;
}

typedef struct Reader {
    const uint8_t *buf;
    size_t len, pos;
    bool ok;
} Reader;

static void rd(Reader *r, void *p, size_t n) {
    if (r->pos + n > r->len) {
        r->ok = false;
        return;
    }
    memcpy(p, r->buf + r->pos, n);
    r->pos += n;
}

bool mechsim_restore_state(MechSim *ms, const uint8_t *buf, size_t len) {
    if (len != mechsim_state_size(ms)) return false;
    Reader r = {buf, len, 0, true};
    char magic[8];
    rd(&r, magic, 8);
    if (memcmp(magic, "NVMSCKP1", 8)) return false;
    MbState *s = ms->s;
    rd(&r, &s->t, sizeof s->t);
    rd(&r, s->aux, sizeof s->aux);
    rd(&r, s->q, (size_t)s->nq * sizeof *s->q);
    rd(&r, s->v, (size_t)s->nv * sizeof *s->v);
    rd(&r, s->limit_state, (size_t)s->nlimits);
    rd(&r, &s->ledger, sizeof s->ledger);
    rd(&r, &s->energy0, sizeof s->energy0);
    rd(&r, &s->steps, sizeof s->steps);
    rd(&r, &s->events, sizeof s->events);
    rd(&r, ms->as, (size_t)ms->nact * sizeof *ms->as);
    rd(&r, ms->cs, (size_t)ms->nctl * sizeof *ms->cs);
    rd(&r, ms->ss, (size_t)ms->nsen * sizeof *ms->ss);
    rd(&r, &ms->rec_k, sizeof ms->rec_k);
    int rows;
    rd(&r, &rows, sizeof rows);
    rd(&r, ms->env, (size_t)(16 * ms->m->njoints) * sizeof *ms->env);
    rd(&r, &ms->drift_max, sizeof ms->drift_max);
    rd(&r, &ms->vdrift_max, sizeof ms->vdrift_max);
    rd(&r, &ms->steps_total, sizeof ms->steps_total);
    rd(&r, &ms->events_total, sizeof ms->events_total);
    rd(&r, &ms->record_full, sizeof ms->record_full);
    int npairs;
    rd(&r, &npairs, sizeof npairs);
    if (npairs != ms->npairs) return false;
    size_t np = (size_t)npairs;
    rd(&r, ms->pair_now, np * sizeof ms->pair_now[0]);
    rd(&r, ms->pair_peak_n, np * sizeof(double));
    rd(&r, ms->pair_peak_t, np * sizeof(double));
    rd(&r, ms->pair_peak_f, np * sizeof(double));
    rd(&r, ms->pair_min_gap, np * sizeof(double));
    rd(&r, ms->pair_slide_time, np * sizeof(double));
    rd(&r, ms->pair_impacts, np * sizeof(int));
    rd(&r, ms->pair_peak_impulse, np * sizeof(double));
    rd(&r, ms->pair_run_impulse, np * sizeof(double));
    rd(&r, ms->pair_util_peak, np * sizeof(double));
    rd(&r, ms->pair_util_peak_t, np * sizeof(double));
    rd(&r, &ms->contact_unconverged, sizeof ms->contact_unconverged);
    rd(&r, &ms->contact_max_iter, sizeof ms->contact_max_iter);
    rd(&r, &ms->contact_max_penetration, sizeof ms->contact_max_penetration);
    rd(&r, &ms->last_h, sizeof ms->last_h);
    rd(&r, &ms->step_impulsive, sizeof ms->step_impulsive);
    rd(&r, &ms->impulsive_steps, sizeof ms->impulsive_steps);
    if (ms->cwrench) rd(&r, ms->cwrench, (size_t)(6 * ms->m->nbodies) * sizeof(double));
    if (ms->env_impulse) rd(&r, ms->env_impulse, (size_t)ms->m->njoints * sizeof(double));
    rd(&r, ms->flex_env, (size_t)(4 * ms->nflex_itf + 2 * ms->m->nflex) * sizeof(double));
    size_t wc = (size_t)ms->warm.cap;
    rd(&r, &ms->warm.n, sizeof ms->warm.n);
    if (ms->warm.n < 0 || ms->warm.n > ms->warm.cap) return false;
    if (wc) {
        rd(&r, ms->warm.sa, wc * sizeof(int)), rd(&r, ms->warm.sb, wc * sizeof(int));
        rd(&r, ms->warm.point, 3 * wc * sizeof(double)), rd(&r, ms->warm.force, 4 * wc * sizeof(double));
    }
    for (int i = 0; i < ms->npairs; i++) /* peak captures made after the checkpoint are no longer the peaks */
        if (!(ms->pair_blob_t[i] == ms->pair_peak_f[i])) ms->pair_blob_t[i] = NAN;
    if (!r.ok || rows > ms->rows) return false;
    ms->rows = rows; /* histories recorded after the checkpoint are discarded */
    /* outputs captured after the checkpoint are discarded too */
    for (int i = 0; ms->peak_blob_t && i < 2 * ms->m->njoints; i++)
        if (ms->peak_blob_t[i] > s->t) ms->peak_blob_t[i] = NAN;
    uint64_t next = 0;
    for (int i = 0; i < ms->nsnap; i++) {
        if (ms->snap_time[i] > s->t + 1e-12 * fmax(1.0, s->t)) ms->snap_have[i] = false;
        else if (ms->snap_have[i]) next = (uint64_t)i + 1;
    }
    ms->snap_next = next;
    return true;
}

bool mechsim_set_snapshot_times(MechSim *ms, const double *times, int n) {
    if (n < 0 || n > 64 || !ms->initialized) return false;
    free(ms->snap_blob), free(ms->snap_have);
    ms->snap_blob = calloc((size_t)(n ? n : 1), ms->blob_size);
    ms->snap_have = calloc((size_t)(n ? n : 1), sizeof(bool));
    if (!ms->snap_blob || !ms->snap_have) return false;
    for (int i = 0; i < n; i++) {
        if (!(times[i] >= 0) || (i && !(times[i] > times[i - 1]))) return false; /* increasing, non-negative */
        ms->snap_time[i] = times[i];
    }
    ms->nsnap = n, ms->snap_next = 0;
    return true;
}

static JsonValue *blob_json(const MechSim *ms, const uint8_t *blob) {
    size_t len = 0;
    char *b64 = base64_encode(blob, ms->blob_size, &len);
    JsonValue *v = json_string(b64 ? b64 : "");
    free(b64);
    return v;
}

JsonValue *mechsim_snapshots_json(const MechSim *ms) {
    JsonValue *arr = json_array();
    for (int i = 0; i < ms->nsnap; i++) {
        if (!ms->snap_have[i]) continue;
        JsonValue *o = json_object();
        char label[64];
        snprintf(label, sizeof label, "t=%.9g", ms->snap_time[i]);
        json_set_string(o, "label", label);
        json_set_string(o, "kind", "requested");
        json_set_number(o, "time_s", ms->snap_time[i]);
        json_set(o, "state_base64", blob_json(ms, ms->snap_blob + (size_t)i * ms->blob_size));
        if (ms->st.contact) capture_to_snapshot(ms, &ms->snap_cap[i], o);
        json_push(arr, o);
    }
    for (int i = 0; ms->pair_blob && i < ms->npairs; i++) {
        if (!(ms->pair_peak_n[i] > 0) || isnan(ms->pair_blob_t[i])) continue;
        JsonValue *o = json_object();
        char label[160];
        snprintf(label, sizeof label, "peak_contact:%s.%s", ms->shapes[ms->pair_a[i]].name, ms->shapes[ms->pair_b[i]].name);
        json_set_string(o, "label", label);
        json_set_string(o, "kind", "peak_contact");
        json_set_number(o, "time_s", ms->pair_peak_f[i]);
        json_set(o, "state_base64", blob_json(ms, ms->pair_blob + (size_t)i * ms->blob_size));
        capture_to_snapshot(ms, &ms->pair_cap[i], o);
        json_push(arr, o);
    }
    for (int j = 0; ms->peak_blob && j < ms->m->njoints; j++)
        for (int k = 0; k < 2; k++) {
            double t = ms->peak_blob_t[2 * j + k];
            if (isnan(t)) continue;
            JsonValue *o = json_object();
            char label[128];
            snprintf(label, sizeof label, "%s:%s", k ? "peak_moment" : "peak_force", ms->m->joints[j].name);
            json_set_string(o, "label", label);
            json_set_string(o, "kind", k ? "peak_moment" : "peak_force");
            json_set_string(o, "joint", ms->m->joints[j].name);
            json_set_number(o, "time_s", t);
            json_set(o, "state_base64", blob_json(ms, ms->peak_blob + (size_t)(2 * j + k) * ms->blob_size));
            if (ms->peak_cap) capture_to_snapshot(ms, &ms->peak_cap[2 * j + k], o);
            json_push(arr, o);
        }
    return arr;
}

bool mechsim_restore_snapshot(MechSim *ms, const uint8_t *buf, size_t len) {
    if (len != ms->blob_size || len < 12) return false;
    /* the recorder row count is the only field that may exceed this runtime's histories: patch a copy */
    uint8_t *copy = malloc(len);
    if (!copy) return false;
    memcpy(copy, buf, len);
    const MbState *s = ms->s;
    size_t off = 8 + sizeof s->t + sizeof s->aux + (size_t)s->nq * sizeof *s->q + (size_t)s->nv * sizeof *s->v + (size_t)s->nlimits + sizeof s->ledger +
                 sizeof s->energy0 + sizeof s->steps + sizeof s->events + (size_t)ms->nact * sizeof *ms->as + (size_t)ms->nctl * sizeof *ms->cs +
                 (size_t)ms->nsen * sizeof *ms->ss + sizeof ms->rec_k;
    int zero = 0;
    memcpy(copy + off, &zero, sizeof zero);
    ms->rows = 0;
    bool ok = mechsim_restore_state(ms, copy, len);
    free(copy);
    return ok;
}

/* ------------------------------------------------------------------------------------------------ reports */

JsonValue *mechsim_summary_json(const MechSim *ms) {
    const MbModel *m = ms->m;
    JsonValue *o = json_object();
    json_set_number(o, "end_time_s", ms->s->t);
    json_set_number(o, "max_step_s", ms->st.max_step);
    if (m->nflex) {
        json_set_number(o, "flexible_step_limit_s", ms->flex_step);
        json_set_string(o, "flexible_initial_state", ms->st.flexible_undeformed ? "undeformed (the loads are applied suddenly at t = 0)"
                                                                               : "static equilibrium under the initial loads with the joints held");
        if (!ms->st.flexible_undeformed) json_set_number(o, "flexible_initial_residual", ms->flex_eq_residual);
        JsonValue *fa = json_set_array(o, "flexible_bodies");
        for (int f = 0, it = 0; f < m->nflex; f++) {
            const MbFlexDef *F = &m->flex[f];
            JsonValue *x = json_object();
            json_set_string(x, "body", m->bodies[F->body].name);
            json_set_int(x, "elastic_coordinates", F->nmodes);
            double wmax = 0;
            for (int k = 0; k < F->nmodes; k++) wmax = fmax(wmax, sqrt(F->omega2[k]));
            json_set_number(x, "highest_frequency_hz", wmax / (2 * M_PI));
            const double *es = ms->flex_env + 4 * ms->nflex_itf + 2 * f;
            json_set_number(x, "peak_strain_energy_j", es[0]);
            json_set_number(x, "peak_strain_energy_time_s", es[1]);
            JsonValue *ia = json_set_array(x, "interfaces");
            for (int i = 0; i < F->ninterfaces; i++, it++) {
                const double *e = ms->flex_env + 4 * it;
                JsonValue *y = json_object();
                if (ms->flex_itf_joint[it] >= 0) json_set_string(y, "joint", m->joints[ms->flex_itf_joint[it]].name);
                json_set_int(y, "interface", i);
                json_set_number(y, "peak_deflection_m", e[0]);
                json_set_number(y, "peak_deflection_time_s", e[1]);
                json_set_number(y, "peak_rotation_rad", e[2]);
                json_set_number(y, "peak_rotation_time_s", e[3]);
                json_push(ia, y);
            }
            json_set_string(x, "model", "first-order floating frame: reference frame held at the root region, small elastic deformation superposed on large "
                                        "motion; interface motion is relative to the reference frame, in body axes");
            json_push(fa, x);
        }
    }
    json_set_int(o, "steps", (long long)ms->steps_total);
    json_set_int(o, "limit_events", (long long)ms->events_total);
    json_set_int(o, "recorded_rows", ms->rows);
    if (ms->record_full) json_set_string(o, "recording", "stopped at max_records; later rows were not kept");
    JsonValue *ca = json_set_array(o, "controllers");
    for (int c = 0; c < ms->nctl; c++) {
        const CtlState *st = &ms->cs[c];
        JsonValue *x = json_object();
        json_set_string(x, "name", ms->ctl[c].name);
        json_set_int(x, "samples", (long long)st->samples);
        if (ms->ctl[c].type == CTRL_OPEN_LOOP) {
            json_set_string(x, "tracking", "open loop: the reference is the command; no feedback and no tracking error");
            json_set_number(x, "saturated_sample_fraction", st->samples ? (double)st->saturated / (double)st->samples : 0);
            json_push(ca, x);
            continue;
        }
        json_set_number(x, "rms_error", st->samples ? sqrt(st->err_sq / (double)st->samples) : 0);
        json_set_number(x, "max_abs_error", st->err_max);
        json_set_number(x, "final_error", st->error);
        json_set_number(x, "saturated_sample_fraction", st->samples ? (double)st->saturated / (double)st->samples : 0);
        json_set_string(x, "error_units", "rad or m (position loops), rad/s or m/s (velocity loops)");
        json_push(ca, x);
    }
    JsonValue *aa = json_set_array(o, "actuators");
    double T = ms->s->t;
    for (int a = 0; a < ms->nact; a++) {
        const ActState *A = &ms->as[a];
        const ActuatorDef *D = &ms->act[a];
        const double *ax = ms->s->aux + AUX_PER_ACT * a;
        JsonValue *x = json_object();
        json_set_string(x, "name", D->name);
        json_set_string(x, "type", actuator_type_name(D->type));
        json_set_string(x, "joint", m->joints[D->joint].name);
        json_set_number(x, "peak_effort", A->peak_effort);
        json_set_number(x, "rms_effort", T > 0 ? sqrt(ax[4] / T) : 0);
        json_set_string(x, "effort_units", m->joints[D->joint].type == MB_PRISMATIC ? "N" : "N*m");
        json_set_number(x, "saturated_time_fraction", T > 0 ? ax[6] / T : 0);
        if (D->rated_effort > 0) {
            json_set_number(x, "rated_effort", D->rated_effort);
            json_set_number(x, "peak_over_rated", A->peak_effort / D->rated_effort);
        }
        if (D->type == ACT_DC_MOTOR) {
            json_set_number(x, "peak_current_A", A->peak_current);
            json_set_number(x, "rms_current_A", T > 0 ? sqrt(ax[5] / T) : 0);
            json_set_number(x, "electrical_energy_J", ax[0]);
            json_set_number(x, "copper_loss_J", ax[1]);
            json_set_number(x, "motor_friction_loss_J", ax[2]);
            json_set_number(x, "gearbox_loss_J", ax[3]);
            json_set_string(x, "loss_integration", "integrated with the dynamics (same Runge-Kutta stages); exported once for the thermal side");
        }
        json_push(aa, x);
    }
    if (ms->st.contact) {
        JsonValue *co = json_set_object(o, "contact");
        json_set_string(co, "integration", "first-order Moreau-Jean time stepping: impulses per step, average forces = impulse / step");
        json_set_int(co, "solver_max_sweeps", ms->contact_max_iter);
        json_set_int(co, "steps_not_converged", ms->contact_unconverged);
        json_set_number(co, "max_penetration_m", ms->contact_max_penetration);
        json_set_int(co, "impact_steps", ms->impulsive_steps);
        if (ms->impulsive_steps)
            json_set_string(co, "impact_steps_note",
                            "steps with an impact are excluded from force peaks, peak snapshots and the joint load envelope: a rigid impact transfers an impulse "
                            "(N*s) whose average force scales with 1/step; impact forces need contact compliance, which this model does not have");
        json_set_string(co, "friction_utilization",
                        "utilisation = |friction force| / (mu x normal force) of a pair in persistent contact; 1 means sliding, slip margin = 1 - utilisation. "
                        "With several contacts on one body (a grasp) the split of friction among them is statically indeterminate for rigid bodies: "
                        "compare the sum of friction forces with the sum of mu x normal forces for the grasp as a whole");
        JsonValue *pa = json_set_array(co, "pairs");
        for (int i = 0; i < ms->npairs; i++) {
            if (!(ms->pair_peak_n[i] > 0) && !(ms->pair_peak_impulse[i] > 0)) continue;
            JsonValue *x = json_object();
            json_set_string(x, "shape_a", ms->shapes[ms->pair_a[i]].name);
            json_set_string(x, "shape_b", ms->shapes[ms->pair_b[i]].name);
            json_set_number(x, "peak_normal_force_N", ms->pair_peak_n[i]);
            json_set_number(x, "peak_normal_force_time_s", ms->pair_peak_f[i]);
            json_set_number(x, "peak_friction_force_N", ms->pair_peak_t[i]);
            json_set_number(x, "sliding_time_s", ms->pair_slide_time[i]);
            json_set_int(x, "impact_steps", ms->pair_impacts[i]);
            json_set_number(x, "peak_impact_impulse_Ns", ms->pair_peak_impulse[i]);
            json_set_number(x, "min_gap_m", isinf(ms->pair_min_gap[i]) ? 0 : ms->pair_min_gap[i]);
            json_set_bool(x, "in_contact_at_end", ms->pair_now[i][0] > 0);
            json_set_number(x, "normal_force_at_end_N", ms->pair_now[i][0]);
            json_set_number(x, "friction_force_at_end_N", ms->pair_now[i][1]);
            json_set_number(x, "friction_coefficient", fmin(ms->shapes[ms->pair_a[i]].friction, ms->shapes[ms->pair_b[i]].friction));
            json_set_number(x, "peak_friction_utilization", ms->pair_util_peak[i]);
            json_set_number(x, "peak_friction_utilization_time_s", ms->pair_util_peak_t[i]);
            if (ms->pair_now[i][0] > 0) {
                json_set_number(x, "friction_utilization_at_end", ms->pair_now[i][5]);
                json_set_number(x, "slip_margin_at_end", 1 - ms->pair_now[i][5]);
            }
            json_push(pa, x);
        }
    }
    const MbLedger *lg = &ms->s->ledger;
    JsonValue *en = json_set_object(o, "energy_J");
    json_set_number(en, "initial", ms->s->energy0);
    json_set_number(en, "kinetic", ms->ev->kinetic);
    json_set_number(en, "potential", ms->ev->potential_gravity + ms->ev->potential_spring);
    json_set_number(en, "input_work", lg->work_inputs);
    json_set_number(en, "external_work", lg->work_external);
    json_set_number(en, "dissipated_damping", lg->dissipated_damping);
    json_set_number(en, "dissipated_friction", lg->dissipated_friction);
    json_set_number(en, "dissipated_impacts", lg->dissipated_impacts);
    json_set_number(en, "dissipated_contact_friction", lg->contact_friction);
    json_set_number(en, "dissipated_contact_impacts", lg->contact_impacts);
    json_set_number(en, "projection_change", lg->projection_change);
    json_set_number(en, "balance_residual", ms->ev->energy_residual);
    JsonValue *cn = json_set_object(o, "constraints");
    json_set_number(cn, "max_position_drift_before_projection", ms->drift_max);
    json_set_number(cn, "max_velocity_drift_before_projection", ms->vdrift_max);
    json_set_int(cn, "rows", ms->ev->cinfo.rows);
    json_set_int(cn, "rank", ms->ev->cinfo.rank);
    json_set_int(cn, "redundant", ms->ev->cinfo.redundant);
    json_set_string(cn, "drift_units", "scaled: rad, or m divided by the model length scale");
    return o;
}

JsonValue *mechsim_load_envelope_json(const MechSim *ms) {
    const MbModel *m = ms->m;
    JsonValue *arr = json_array();
    for (int j = 0; j < m->njoints; j++) {
        const double *e = ms->env + 16 * j;
        JsonValue *x = json_object();
        json_set_string(x, "joint", m->joints[j].name);
        json_set_string(x, "frame", "child joint frame, about its origin; wrench exerted by the parent on the child");
        for (int k = 0; k < 2; k++) {
            const double *b = e + 8 * k;
            JsonValue *pk = json_set_object(x, k ? "peak_moment" : "peak_force");
            json_set_number(pk, k ? "magnitude_Nm" : "magnitude_N", b[0]);
            json_set_number(pk, "time_s", b[1]);
            json_set(pk, "force_N", json_numbers(b + 2, 3));
            json_set(pk, "moment_Nm", json_numbers(b + 5, 3));
        }
        if (ms->env_impulse && ms->impulsive_steps) {
            json_set_number(x, "impact_force_impulse_Ns", ms->env_impulse[j]);
            json_set_string(x, "impact_note", "impact steps are excluded from the peaks; the impulse is |joint force| x step over the impact step (approximate)");
        }
        json_push(arr, x);
    }
    return arr;
}

bool mechsim_write_csv(const MechSim *ms, const char *path, char *err, size_t errlen) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errlen, "cannot write %s", path);
        return false;
    }
    for (int c = 0; c < ms->nch; c++) fprintf(f, "%s%s [%s]", c ? "," : "", ms->ch_name[c], ms->ch_unit[c]);
    fputc('\n', f);
    for (int r = 0; r < ms->rows; r++) {
        for (int c = 0; c < ms->nch; c++) fprintf(f, "%s%.17g", c ? "," : "", ms->data[(size_t)c * (size_t)ms->cap_rows + (size_t)r]);
        fputc('\n', f);
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) snprintf(err, errlen, "write error on %s", path);
    return ok;
}
