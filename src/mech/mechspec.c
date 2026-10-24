/* mechspec.c - mechanical study documents (see mechspec.h) */
#include "mechspec.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mechunits.h"

MechStudy *mspec_new(const char *name) {
    MechStudy *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->name, sizeof s->name, "%s", name ? name : "study");
    s->units = json_object();
    s->settings = json_object();
    s->actuators = json_array();
    s->sensors = json_array();
    s->controllers = json_array();
    s->snapshots = json_array();
    if (!s->units || !s->settings || !s->actuators || !s->sensors || !s->controllers || !s->snapshots) {
        mspec_free(s);
        return NULL;
    }
    return s;
}

static void clear_resolved(MechStudy *s) {
    free(s->act), free(s->sen), free(s->ctl);
    s->act = NULL, s->sen = NULL, s->ctl = NULL;
    s->nact = s->nsen = s->nctl = s->nsnapshots = 0;
    s->resolved = false;
}

void mspec_free(MechStudy *s) {
    if (!s) return;
    clear_resolved(s);
    json_free(s->units), json_free(s->settings), json_free(s->actuators), json_free(s->sensors), json_free(s->controllers), json_free(s->snapshots);
    free(s);
}

static bool name_ok(const char *n) {
    size_t len = n ? strlen(n) : 0;
    if (!len || len >= MB_NAME) return false;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static JsonValue *array_for(MechStudy *s, const char *kind) {
    if (!strcmp(kind, "actuator")) return s->actuators;
    if (!strcmp(kind, "sensor")) return s->sensors;
    if (!strcmp(kind, "controller")) return s->controllers;
    return NULL;
}

static void check_keys(MechDiag *d, const JsonValue *o, const char *path, const char *const *allowed) {
    for (size_t i = 0; i < json_len(o); i++) {
        const char *k = json_key_at(o, i);
        bool ok = false;
        for (int j = 0; allowed[j] && !ok; j++) ok = !strcmp(allowed[j], k);
        if (!ok) mdiag_add(d, MD_WARNING, "UNKNOWN_KEY", path, NULL, "unknown key '%s' ignored", k);
    }
}

static const char *const ACT_KEYS[] = {"name", "type", "units", "joint", "effort_limit", "velocity_limit", "rated_effort", "kp", "kd", "kv", "initial_command",
                                       "gear_ratio", "lead", "efficiency", "rotor_inertia", "resistance", "torque_constant", "back_emf_constant",
                                       "voltage_limit", "current_limit", "motor_viscous", "motor_coulomb", "motor_coulomb_vreg", "source", "note", NULL};
static const char *const SEN_KEYS[] = {"name", "type", "units", "joint", "actuator", "body", "site", "period", "latency_samples", "latency", "noise_std", "bias",
                                       "resolution", "filter_tau", "seed", "source", "note", NULL};
static const char *const CTL_KEYS[] = {"name", "type", "units", "actuator", "feedback", "loop", "period", "delay_samples", "delay", "kp", "ki", "kd",
                                       "derivative_filter", "feedforward", "output_limits", "antiwindup", "tracking_time", "reference", "note", NULL};

bool mspec_set_component(MechStudy *s, const char *kind, const JsonValue *def, MechDiag *d) {
    JsonValue *arr = array_for(s, kind ? kind : "");
    if (!arr) {
        mdiag_add(d, MD_ERROR, "INVALID_KIND", kind ? kind : "", NULL, "component kind must be actuator, sensor or controller");
        return false;
    }
    const char *name = json_get_str(def, "name", NULL);
    if (!def || def->type != JSON_OBJECT || !name_ok(name)) {
        mdiag_add(d, MD_ERROR, "INVALID_NAME", kind, NULL, "a %s needs an object with a valid name (letters, digits, '_' and '-')", kind);
        return false;
    }
    const char *type = json_get_str(def, "type", NULL);
    bool type_ok = !strcmp(kind, "actuator") ? actuator_type_from_name(type) >= 0
                                             : (!strcmp(kind, "sensor") ? sensor_type_from_name(type) >= 0 : (type && (!strcmp(type, "pid") || !strcmp(type, "open_loop"))));
    if (!type_ok) {
        mdiag_add(d, MD_ERROR, "INVALID_TYPE", name, NULL, "%s '%s' has a missing or unknown type '%s'", kind, name, type ? type : "");
        return false;
    }
    check_keys(d, def, name, !strcmp(kind, "actuator") ? ACT_KEYS : (!strcmp(kind, "sensor") ? SEN_KEYS : CTL_KEYS));
    clear_resolved(s);
    for (size_t i = 0; i < json_len(arr); i++)
        if (!strcmp(json_get_str(json_at(arr, i), "name", ""), name)) {
            JsonValue *copy = json_clone(def);
            if (!copy) return false;
            json_free(arr->u.array.items[i]);
            arr->u.array.items[i] = copy; /* replaces the previous definition */
            return true;
        }
    return json_push(arr, json_clone(def));
}

bool mspec_remove_component(MechStudy *s, const char *kind, const char *name) {
    JsonValue *arr = array_for(s, kind ? kind : "");
    for (size_t i = 0; arr && i < json_len(arr); i++)
        if (!strcmp(json_get_str(json_at(arr, i), "name", ""), name ? name : "")) {
            json_free(arr->u.array.items[i]);
            memmove(arr->u.array.items + i, arr->u.array.items + i + 1, (arr->u.array.len - i - 1) * sizeof *arr->u.array.items);
            arr->u.array.len--;
            clear_resolved(s);
            return true;
        }
    return false;
}

bool mspec_set_settings(MechStudy *s, const JsonValue *settings, const JsonValue *snapshots, MechDiag *d) {
    if (settings) {
        if (settings->type != JSON_OBJECT) {
            mdiag_add(d, MD_ERROR, "INVALID_SETTINGS", "settings", NULL, "settings must be an object");
            return false;
        }
        static const char *const K[] = {"end_time", "max_step", "record_period", "max_records", "units", "contact", "flexible_initial_state", NULL};
        check_keys(d, settings, "settings", K);
        json_free(s->settings);
        s->settings = json_clone(settings);
    }
    if (snapshots) {
        if (snapshots->type != JSON_ARRAY || json_len(snapshots) > MSPEC_MAX_SNAPSHOTS) {
            mdiag_add(d, MD_ERROR, "INVALID_SNAPSHOTS", "snapshots", NULL, "snapshots must be an array of at most %d times", MSPEC_MAX_SNAPSHOTS);
            return false;
        }
        json_free(s->snapshots);
        s->snapshots = json_clone(snapshots);
    }
    clear_resolved(s);
    return true;
}

void mspec_set_units(MechStudy *s, const JsonValue *units) {
    if (!units || units->type != JSON_OBJECT) return;
    json_free(s->units);
    s->units = json_clone(units);
    clear_resolved(s);
}

MechStudy *mspec_from_json(const JsonValue *doc, MechDiag *d) {
    if (!doc || doc->type != JSON_OBJECT || strcmp(json_get_str(doc, "format", ""), "navier-mech-study") || json_get_int(doc, "version", 0) != 1) {
        mdiag_add(d, MD_ERROR, "INVALID_DOCUMENT", "format", "set \"format\": \"navier-mech-study\" and \"version\": 1", "not a navier-mech-study document");
        return NULL;
    }
    MechStudy *s = mspec_new(json_get_str(doc, "name", "study"));
    if (!s) return NULL;
    static const char *const K[] = {"format", "version", "name", "plain_numbers", "settings", "snapshots", "actuators", "sensors", "controllers", "note", NULL};
    check_keys(d, doc, "", K); /* units live in "settings" and in each component, never globally */
    s->require_units = !strcmp(json_get_str(doc, "plain_numbers", "si"), "require_units");
    bool ok = mspec_set_settings(s, json_get(doc, "settings"), json_get(doc, "snapshots"), d);
    static const char *const KIND[3] = {"actuator", "sensor", "controller"}, *const ARR[3] = {"actuators", "sensors", "controllers"};
    for (int k = 0; k < 3; k++) {
        const JsonValue *a = json_get(doc, ARR[k]);
        for (size_t i = 0; i < json_len(a); i++) ok &= mspec_set_component(s, KIND[k], json_at(a, i), d);
    }
    if (!ok) {
        mspec_free(s);
        return NULL;
    }
    return s;
}

JsonValue *mspec_to_json(const MechStudy *s) {
    JsonValue *o = json_object();
    json_set_string(o, "format", "navier-mech-study");
    json_set_int(o, "version", 1);
    json_set_string(o, "name", s->name);
    json_set_string(o, "plain_numbers", s->require_units ? "require_units" : "si");
    json_set(o, "settings", json_clone(s->settings));
    json_set(o, "snapshots", json_clone(s->snapshots));
    json_set(o, "actuators", json_clone(s->actuators));
    json_set(o, "sensors", json_clone(s->sensors));
    json_set(o, "controllers", json_clone(s->controllers));
    return o;
}

/* ------------------------------------------------------------------------------------------------ resolution */

typedef struct R {
    MechStudy *s;
    const MbModelDef *def;
    MechDiag *d;
    int errors;
    const JsonValue *units; /* units of the object being resolved (component or settings); each keeps its own */
} R;

static const char *unit_for(R *r, MechQty q) { return json_get_str(r->units, mech_qty_name(q), NULL); }

static bool qv(R *r, const JsonValue *o, const char *key, MechQty q, const char *who, double *out, bool required) {
    const JsonValue *v = json_get(o, key);
    if (!v) {
        if (required) {
            mdiag_add(r->d, MD_MISSING_INPUT, "MISSING_PARAMETER", who, NULL, "'%s' needs %s (%s)", who, key, mech_qty_name(q));
            r->errors++;
        }
        return false;
    }
    char e[256];
    if (!mech_qty_from_json_ex(v, q, unit_for(r, q), r->s->require_units, out, e, sizeof e)) {
        mdiag_add(r->d, MD_ERROR, "INVALID_QUANTITY", who, NULL, "%s.%s: %s", who, key, e);
        r->errors++;
        return false;
    }
    return true;
}

static bool gain(R *r, const JsonValue *o, const char *key, const char *who, double *out) {
    const JsonValue *v = json_get(o, key);
    if (!v) return false;
    if (v->type != JSON_NUMBER || !isfinite(v->u.number)) {
        mdiag_add(r->d, MD_ERROR, "INVALID_GAIN", who, "gains are plain SI numbers: command units per unit of error (for example V/rad, N m/rad)",
                  "%s.%s must be a number", who, key);
        r->errors++;
        return false;
    }
    *out = v->u.number;
    return true;
}

static int joint_index(R *r, const char *name, const char *who) {
    int j = -1;
    for (int i = 0; name && i < r->def->njoints; i++)
        if (!strcmp(r->def->joints[i].name, name)) j = i;
    if (j < 0 || (r->def->joints[j].type != MB_REVOLUTE && r->def->joints[j].type != MB_PRISMATIC)) {
        mdiag_add(r->d, MD_ERROR, "UNKNOWN_JOINT", who, NULL, "'%s' refers to '%s', which is not a revolute or prismatic joint of the assembly", who, name ? name : "");
        r->errors++;
        return -1;
    }
    return j;
}

static int named(const JsonValue *arr, const char *name) {
    for (size_t i = 0; name && i < json_len(arr); i++)
        if (!strcmp(json_get_str(json_at(arr, i), "name", ""), name)) return (int)i;
    return -1;
}

static bool resolve_actuator(R *r, const JsonValue *o, ActuatorDef *A) {
    memset(A, 0, sizeof *A);
    const char *nm = json_get_str(o, "name", "");
    snprintf(A->name, sizeof A->name, "%s", nm);
    A->type = (ActuatorType)actuator_type_from_name(json_get_str(o, "type", ""));
    A->joint = joint_index(r, json_get_str(o, "joint", NULL), nm);
    if (A->joint < 0) return false;
    bool pr = r->def->joints[A->joint].type == MB_PRISMATIC;
    MechQty EFF = pr ? MQ_FORCE : MQ_TORQUE, VEL = pr ? MQ_VELOCITY : MQ_ANGULAR_VELOCITY, POS = pr ? MQ_LENGTH : MQ_ANGLE;
    qv(r, o, "effort_limit", EFF, nm, &A->effort_limit, A->type == ACT_POSITION_SERVO || A->type == ACT_VELOCITY_SERVO);
    qv(r, o, "velocity_limit", VEL, nm, &A->velocity_limit, false);
    qv(r, o, "rated_effort", EFF, nm, &A->rated_effort, false);
    qv(r, o, "kp", pr ? MQ_LINEAR_STIFFNESS : MQ_ROTATIONAL_STIFFNESS, nm, &A->kp, A->type == ACT_POSITION_SERVO);
    qv(r, o, "kd", pr ? MQ_LINEAR_DAMPING : MQ_ROTATIONAL_DAMPING, nm, &A->kd, false);
    qv(r, o, "kv", pr ? MQ_LINEAR_DAMPING : MQ_ROTATIONAL_DAMPING, nm, &A->kv, A->type == ACT_VELOCITY_SERVO);
    MechQty cmdq = A->type == ACT_EFFORT ? EFF : (A->type == ACT_POSITION_SERVO ? POS : (A->type == ACT_VELOCITY_SERVO ? VEL : MQ_VOLTAGE));
    qv(r, o, "initial_command", cmdq, nm, &A->initial_command, false);
    if (A->type == ACT_DC_MOTOR) {
        bool dc = true;
        if (json_get(o, "lead")) {
            double lead;
            if (!pr) {
                mdiag_add(r->d, MD_ERROR, "INVALID_PARAMETER", nm, "use gear_ratio on revolute joints", "'lead' applies to prismatic (screw-driven) joints");
                r->errors++;
            } else if (qv(r, o, "lead", MQ_LENGTH, nm, &lead, true))
                A->gear_ratio = 2 * M_PI / lead;
        } else
            dc &= qv(r, o, "gear_ratio", MQ_DIMENSIONLESS, nm, &A->gear_ratio, true);
        dc &= qv(r, o, "efficiency", MQ_DIMENSIONLESS, nm, &A->efficiency, true);
        dc &= qv(r, o, "resistance", MQ_RESISTANCE, nm, &A->resistance, true);
        dc &= qv(r, o, "torque_constant", MQ_TORQUE_CONSTANT, nm, &A->torque_constant, true);
        dc &= qv(r, o, "back_emf_constant", MQ_BACK_EMF_CONSTANT, nm, &A->back_emf_constant, true);
        dc &= qv(r, o, "voltage_limit", MQ_VOLTAGE, nm, &A->voltage_limit, true);
        qv(r, o, "current_limit", MQ_CURRENT, nm, &A->current_limit, false);
        if (!json_get(o, "rotor_inertia"))
            mdiag_add(r->d, MD_MISSING_INPUT, "MISSING_PARAMETER", nm, "rotor inertia is reflected by N^2 and often dominates at high gear ratios; take it from the datasheet",
                      "dc_motor '%s' needs rotor_inertia", nm), r->errors++;
        else
            qv(r, o, "rotor_inertia", MQ_INERTIA, nm, &A->rotor_inertia, true);
        qv(r, o, "motor_viscous", MQ_ROTATIONAL_DAMPING, nm, &A->motor_viscous, false);
        qv(r, o, "motor_coulomb", MQ_TORQUE, nm, &A->motor_coulomb, false);
        qv(r, o, "motor_coulomb_vreg", MQ_ANGULAR_VELOCITY, nm, &A->motor_coulomb_vreg, false);
        (void)dc;
    }
    return true;
}

static bool resolve_sensor(R *r, const JsonValue *o, SensorDef *S) {
    memset(S, 0, sizeof *S);
    const char *nm = json_get_str(o, "name", "");
    snprintf(S->name, sizeof S->name, "%s", nm);
    S->type = (SensorType)sensor_type_from_name(json_get_str(o, "type", ""));
    S->joint = S->actuator = S->body = -1;
    mpose_identity(&S->site);
    MechQty q = MQ_DIMENSIONLESS;
    switch (S->type) {
    case SENS_JOINT_POSITION:
    case SENS_JOINT_VELOCITY: {
        S->joint = joint_index(r, json_get_str(o, "joint", NULL), nm);
        if (S->joint < 0) return false;
        bool pr = r->def->joints[S->joint].type == MB_PRISMATIC;
        q = S->type == SENS_JOINT_POSITION ? (pr ? MQ_LENGTH : MQ_ANGLE) : (pr ? MQ_VELOCITY : MQ_ANGULAR_VELOCITY);
        break;
    }
    case SENS_JOINT_EFFORT:
    case SENS_MOTOR_CURRENT: {
        int a = named(r->s->actuators, json_get_str(o, "actuator", NULL));
        if (a < 0) {
            mdiag_add(r->d, MD_ERROR, "UNKNOWN_ACTUATOR", nm, NULL, "sensor '%s' refers to an unknown actuator", nm);
            r->errors++;
            return false;
        }
        S->actuator = a;
        if (S->type == SENS_MOTOR_CURRENT)
            q = MQ_CURRENT;
        else {
            int j = joint_index(r, json_get_str(json_at(r->s->actuators, (size_t)a), "joint", NULL), nm);
            q = j >= 0 && r->def->joints[j].type == MB_PRISMATIC ? MQ_FORCE : MQ_TORQUE;
        }
        break;
    }
    case SENS_FORCE_TORQUE: {
        const char *jn = json_get_str(o, "joint", NULL);
        for (int i = 0; jn && i < r->def->njoints; i++)
            if (!strcmp(r->def->joints[i].name, jn)) S->joint = i;
        if (S->joint < 0) {
            mdiag_add(r->d, MD_ERROR, "UNKNOWN_JOINT", nm, NULL, "force_torque sensor '%s' needs an existing joint", nm);
            r->errors++;
            return false;
        }
        break; /* noise in SI numbers for both forces (N) and moments (N m) */
    }
    case SENS_IMU_ACCEL:
    case SENS_IMU_GYRO: {
        const char *bn = json_get_str(o, "body", NULL);
        for (int i = 0; bn && i < r->def->nbodies; i++)
            if (!strcmp(r->def->bodies[i].name, bn)) S->body = i;
        if (S->body < 0) {
            mdiag_add(r->d, MD_ERROR, "UNKNOWN_BODY", nm, "a body merged as a frame of its parent cannot carry a sensor yet", "IMU '%s' needs a body of the model", nm);
            r->errors++;
            return false;
        }
        const JsonValue *site = json_get(o, "site");
        if (site) {
            double p[3] = {0, 0, 0}, rpy[3] = {0, 0, 0};
            const JsonValue *pos = json_get(site, "position"), *ang = json_get(site, "rpy");
            char e[200];
            for (int k = 0; k < 3; k++) {
                if (pos && !mech_qty_from_json_ex(json_at(pos, (size_t)k), MQ_LENGTH, unit_for(r, MQ_LENGTH), r->s->require_units, &p[k], e, sizeof e)) {
                    mdiag_add(r->d, MD_ERROR, "INVALID_QUANTITY", nm, NULL, "site.position: %s", e);
                    r->errors++;
                }
                if (ang && !mech_qty_from_json_ex(json_at(ang, (size_t)k), MQ_ANGLE, unit_for(r, MQ_ANGLE), r->s->require_units, &rpy[k], e, sizeof e)) {
                    mdiag_add(r->d, MD_ERROR, "INVALID_QUANTITY", nm, NULL, "site.rpy: %s", e);
                    r->errors++;
                }
            }
            mv3_copy(S->site.p, p);
            mrot_from_rpy(S->site.R, rpy[0], rpy[1], rpy[2]);
        }
        q = S->type == SENS_IMU_ACCEL ? MQ_ACCELERATION : MQ_ANGULAR_VELOCITY;
        break;
    }
    default: return false;
    }
    qv(r, o, "period", MQ_TIME, nm, &S->period, true);
    if (json_get(o, "latency")) {
        double lat;
        if (qv(r, o, "latency", MQ_TIME, nm, &lat, true) && S->period > 0) {
            S->latency_samples = (int)llround(lat / S->period);
            if (fabs(S->latency_samples * S->period - lat) > 1e-9 * fmax(lat, S->period))
                mdiag_add(r->d, MD_WARNING, "LATENCY_ROUNDED", nm, NULL, "latency %.6g s rounded to %d samples of %.6g s", lat, S->latency_samples, S->period);
        }
    } else
        S->latency_samples = (int)json_get_int(o, "latency_samples", 0);
    qv(r, o, "noise_std", q, nm, &S->noise_std, false);
    qv(r, o, "bias", q, nm, &S->bias, false);
    qv(r, o, "resolution", q, nm, &S->resolution, false);
    qv(r, o, "filter_tau", MQ_TIME, nm, &S->filter_tau, false);
    const JsonValue *seed = json_get(o, "seed");
    if (S->noise_std > 0 && !seed) {
        mdiag_add(r->d, MD_MISSING_INPUT, "SEED_REQUIRED", nm, "give an integer seed so the measurement noise is reproducible", "noisy sensor '%s' needs a seed", nm);
        r->errors++;
    }
    S->seed = (uint64_t)json_get_int(o, "seed", 0);
    return true;
}

static bool reference(R *r, const JsonValue *o, MechQty q, const char *who, Trajectory *T) {
    memset(T, 0, sizeof *T);
    if (!o || o->type != JSON_OBJECT) {
        mdiag_add(r->d, MD_MISSING_INPUT, "REFERENCE_REQUIRED", who, "e.g. {\"type\": \"min_jerk\", \"from\": \"0 deg\", \"to\": \"45 deg\", \"start\": \"0.1 s\", \"duration\": \"0.5 s\"}",
                  "controller '%s' needs a reference", who);
        r->errors++;
        return false;
    }
    static const char *const K[] = {"type", "start", "value", "from", "to", "rate", "amplitude", "frequency", "phase", "duration", "vmax", "amax", "waypoints", NULL};
    check_keys(r->d, o, who, K);
    int t = trajectory_type_from_name(json_get_str(o, "type", NULL));
    if (t < 0) {
        mdiag_add(r->d, MD_ERROR, "INVALID_REFERENCE", who, "constant, step, ramp, sine, trapezoid, min_jerk or waypoints", "unknown reference type");
        r->errors++;
        return false;
    }
    T->type = (TrajType)t;
    qv(r, o, "start", MQ_TIME, who, &T->t0, false);
    MechQty rate = q == MQ_ANGLE ? MQ_ANGULAR_VELOCITY : (q == MQ_LENGTH ? MQ_VELOCITY : MQ_DIMENSIONLESS);
    MechQty acc = q == MQ_ANGLE ? MQ_ANGULAR_ACCELERATION : (q == MQ_LENGTH ? MQ_ACCELERATION : MQ_DIMENSIONLESS);
    switch (T->type) {
    case TRAJ_CONSTANT: qv(r, o, "value", q, who, &T->value, true); break;
    case TRAJ_SINE:
        qv(r, o, "value", q, who, &T->value, false);
        qv(r, o, "amplitude", q, who, &T->amplitude, true);
        qv(r, o, "frequency", MQ_FREQUENCY, who, &T->frequency, true);
        qv(r, o, "phase", MQ_ANGLE, who, &T->phase, false);
        break;
    case TRAJ_WAYPOINTS: {
        const JsonValue *w = json_get(o, "waypoints");
        int n = (int)json_len(w);
        if (n < 2 || n > MS_MAX_WAYPOINTS) {
            mdiag_add(r->d, MD_ERROR, "INVALID_REFERENCE", who, NULL, "waypoints needs 2..%d entries {time, value}", MS_MAX_WAYPOINTS);
            r->errors++;
            return false;
        }
        T->npoints = n;
        for (int i = 0; i < n; i++) {
            qv(r, json_at(w, (size_t)i), "time", MQ_TIME, who, &T->wt[i], true);
            qv(r, json_at(w, (size_t)i), "value", q, who, &T->wp[i], true);
            if (i && !(T->wt[i] > T->wt[i - 1])) {
                mdiag_add(r->d, MD_ERROR, "INVALID_REFERENCE", who, NULL, "waypoint times must increase");
                r->errors++;
            }
        }
        break;
    }
    default:
        qv(r, o, "from", q, who, &T->value, true);
        qv(r, o, "to", q, who, &T->value1, true);
        if (T->type == TRAJ_RAMP) qv(r, o, "rate", rate, who, &T->rate, true);
        if (T->type == TRAJ_MIN_JERK) qv(r, o, "duration", MQ_TIME, who, &T->duration, true);
        if (T->type == TRAJ_TRAPEZOID) qv(r, o, "vmax", rate, who, &T->vmax, true), qv(r, o, "amax", acc, who, &T->amax, true);
        break;
    }
    return true;
}

static bool resolve_controller(R *r, const JsonValue *o, ControllerDef *C) {
    memset(C, 0, sizeof *C);
    const char *nm = json_get_str(o, "name", "");
    snprintf(C->name, sizeof C->name, "%s", nm);
    C->type = !strcmp(json_get_str(o, "type", ""), "open_loop") ? CTRL_OPEN_LOOP : CTRL_PID;
    C->actuator = named(r->s->actuators, json_get_str(o, "actuator", NULL));
    if (C->actuator < 0) {
        mdiag_add(r->d, MD_ERROR, "UNKNOWN_ACTUATOR", nm, NULL, "controller '%s' refers to an unknown actuator", nm);
        r->errors++;
        return false;
    }
    const JsonValue *ao = json_at(r->s->actuators, (size_t)C->actuator);
    int j = joint_index(r, json_get_str(ao, "joint", NULL), nm);
    if (j < 0) return false;
    bool pr = r->def->joints[j].type == MB_PRISMATIC;
    const char *loop = json_get_str(o, "loop", "position");
    C->control_velocity = !strcmp(loop, "velocity");
    const char *fb = json_get_str(o, "feedback", NULL);
    if (C->type == CTRL_PID) {
        if (!fb) {
            mdiag_add(r->d, MD_MISSING_INPUT, "FEEDBACK_REQUIRED", nm, "name a sensor, or \"ideal\" to use the exact joint state (an idealisation, reported)",
                      "controller '%s' needs feedback", nm);
            r->errors++;
        } else if (!strcmp(fb, "ideal"))
            C->feedback_sensor = -1;
        else if ((C->feedback_sensor = named(r->s->sensors, fb)) < 0) {
            mdiag_add(r->d, MD_ERROR, "UNKNOWN_SENSOR", nm, NULL, "controller '%s' refers to unknown sensor '%s'", nm, fb);
            r->errors++;
        }
    } else
        C->feedback_sensor = -1;
    qv(r, o, "period", MQ_TIME, nm, &C->period, true);
    if (json_get(o, "delay")) {
        double dl;
        if (qv(r, o, "delay", MQ_TIME, nm, &dl, true) && C->period > 0) C->delay_samples = (int)llround(dl / C->period);
    } else
        C->delay_samples = (int)json_get_int(o, "delay_samples", 0);
    gain(r, o, "kp", nm, &C->kp), gain(r, o, "ki", nm, &C->ki), gain(r, o, "kd", nm, &C->kd);
    qv(r, o, "derivative_filter", MQ_TIME, nm, &C->derivative_filter, false);
    const JsonValue *ff = json_get(o, "feedforward");
    if (ff) gain(r, ff, "velocity", nm, &C->kff_velocity), gain(r, ff, "acceleration", nm, &C->kff_acceleration);
    ActuatorType at = (ActuatorType)actuator_type_from_name(json_get_str(ao, "type", ""));
    MechQty cmdq = at == ACT_EFFORT ? (pr ? MQ_FORCE : MQ_TORQUE)
                                    : (at == ACT_POSITION_SERVO ? (pr ? MQ_LENGTH : MQ_ANGLE) : (at == ACT_VELOCITY_SERVO ? (pr ? MQ_VELOCITY : MQ_ANGULAR_VELOCITY) : MQ_VOLTAGE));
    const JsonValue *lim = json_get(o, "output_limits");
    if (lim) {
        char e[200];
        if (json_len(lim) != 2 || !mech_qty_from_json_ex(json_at(lim, 0), cmdq, unit_for(r, cmdq), r->s->require_units, &C->output_min, e, sizeof e) ||
            !mech_qty_from_json_ex(json_at(lim, 1), cmdq, unit_for(r, cmdq), r->s->require_units, &C->output_max, e, sizeof e) || !(C->output_max > C->output_min)) {
            mdiag_add(r->d, MD_ERROR, "INVALID_LIMITS", nm, NULL, "output_limits must be [min, max] in %s units", mech_qty_name(cmdq));
            r->errors++;
        }
    } else if (C->type == CTRL_PID)
        mdiag_add(r->d, MD_WARNING, "UNLIMITED_OUTPUT", nm, "set output_limits to the command range the hardware accepts", "controller '%s' has no output limits", nm);
    const char *aw = json_get_str(o, "antiwindup", "none");
    C->antiwindup = !strcmp(aw, "clamp") ? AW_CLAMP : (!strcmp(aw, "back_calculation") ? AW_BACK_CALCULATION : AW_NONE);
    qv(r, o, "tracking_time", MQ_TIME, nm, &C->tracking_time, C->antiwindup == AW_BACK_CALCULATION);
    MechQty refq = C->type == CTRL_OPEN_LOOP ? cmdq : (C->control_velocity ? (pr ? MQ_VELOCITY : MQ_ANGULAR_VELOCITY) : (pr ? MQ_LENGTH : MQ_ANGLE));
    reference(r, json_get(o, "reference"), refq, nm, &C->reference);
    return true;
}

bool mspec_resolve(MechStudy *s, const MbModelDef *def, MechDiag *d) {
    clear_resolved(s);
    R r = {s, def, d, 0};
    const JsonValue *st = s->settings;
    memset(&s->st, 0, sizeof s->st);
    r.units = json_get(st, "units");
    if (!json_get(st, "end_time") || !json_get(st, "max_step"))
        mdiag_add(d, MD_MISSING_INPUT, "STUDY_TIME", "settings", "for example {\"end_time\": \"2 s\", \"max_step\": \"0.5 ms\"}", "settings need end_time and max_step"), r.errors++;
    qv(&r, st, "end_time", MQ_TIME, "settings", &s->st.end_time, false);
    qv(&r, st, "max_step", MQ_TIME, "settings", &s->st.max_step, false);
    qv(&r, st, "record_period", MQ_TIME, "settings", &s->st.record_period, false);
    s->st.max_records = (int)json_get_int(st, "max_records", 200000);
    const char *fis = json_get_str(st, "flexible_initial_state", "static_equilibrium");
    if (strcmp(fis, "static_equilibrium") && strcmp(fis, "undeformed"))
        mdiag_add(d, MD_ERROR, "INVALID_SETTINGS", "settings.flexible_initial_state", NULL, "flexible_initial_state is static_equilibrium or undeformed"), r.errors++;
    s->st.flexible_undeformed = !strcmp(fis, "undeformed");
    const JsonValue *ct = json_get(st, "contact");
    if (ct) {
        static const char *const CK[] = {"enabled", "margin", "max_iterations", "tolerance", "restitution_speed", NULL};
        check_keys(d, ct, "settings.contact", CK);
        s->st.contact = json_get_bool(ct, "enabled", true);
        qv(&r, ct, "margin", MQ_LENGTH, "settings.contact", &s->st.contact_margin, false);
        qv(&r, ct, "restitution_speed", MQ_VELOCITY, "settings.contact", &s->st.contact_restitution_speed, false);
        s->st.contact_max_iterations = (int)json_get_int(ct, "max_iterations", 0);
        s->st.contact_tolerance = json_get_num(ct, "tolerance", 0);
    }
    int na = (int)json_len(s->actuators), ns = (int)json_len(s->sensors), nc = (int)json_len(s->controllers);
    s->act = calloc((size_t)(na ? na : 1), sizeof *s->act);
    s->sen = calloc((size_t)(ns ? ns : 1), sizeof *s->sen);
    s->ctl = calloc((size_t)(nc ? nc : 1), sizeof *s->ctl);
    if (!s->act || !s->sen || !s->ctl) {
        clear_resolved(s);
        return false;
    }
    for (int i = 0; i < na; i++) {
        r.units = json_get(json_at(s->actuators, (size_t)i), "units");
        r.errors += !resolve_actuator(&r, json_at(s->actuators, (size_t)i), &s->act[i]);
    }
    for (int i = 0; i < ns; i++) {
        r.units = json_get(json_at(s->sensors, (size_t)i), "units");
        r.errors += !resolve_sensor(&r, json_at(s->sensors, (size_t)i), &s->sen[i]);
    }
    for (int i = 0; i < nc; i++) {
        r.units = json_get(json_at(s->controllers, (size_t)i), "units");
        r.errors += !resolve_controller(&r, json_at(s->controllers, (size_t)i), &s->ctl[i]);
    }
    s->nact = na, s->nsen = ns, s->nctl = nc;
    r.units = json_get(st, "units");
    for (size_t i = 0; i < json_len(s->snapshots) && i < MSPEC_MAX_SNAPSHOTS; i++) {
        char e[200];
        if (!mech_qty_from_json_ex(json_at(s->snapshots, i), MQ_TIME, unit_for(&r, MQ_TIME), s->require_units, &s->snapshot_s[s->nsnapshots], e, sizeof e)) {
            mdiag_add(d, MD_ERROR, "INVALID_QUANTITY", "snapshots", NULL, "%s", e);
            r.errors++;
        } else
            s->nsnapshots++;
    }
    if (r.errors) {
        clear_resolved(s);
        return false;
    }
    s->resolved = true;
    return true;
}

JsonValue *mspec_canonical_json(const MechStudy *s, const MbModelDef *def) {
    if (!s->resolved) return NULL;
    JsonValue *o = json_object();
    json_set_string(o, "format", "navier-mech-study");
    json_set_int(o, "version", 1);
    json_set_string(o, "name", s->name);
    json_set_string(o, "plain_numbers", "si");
    JsonValue *st = json_set_object(o, "settings");
    json_set_number(st, "end_time", s->st.end_time);
    json_set_number(st, "max_step", s->st.max_step);
    json_set_number(st, "record_period", s->st.record_period);
    json_set_int(st, "max_records", s->st.max_records);
    if (s->st.flexible_undeformed) json_set_string(st, "flexible_initial_state", "undeformed");
    if (s->st.contact) {
        JsonValue *ct = json_set_object(st, "contact");
        json_set_bool(ct, "enabled", true);
        json_set_number(ct, "margin", s->st.contact_margin);
        json_set_int(ct, "max_iterations", s->st.contact_max_iterations);
        json_set_number(ct, "tolerance", s->st.contact_tolerance);
        json_set_number(ct, "restitution_speed", s->st.contact_restitution_speed);
    }
    json_set(o, "snapshots", json_numbers(s->snapshot_s, (size_t)s->nsnapshots));
    JsonValue *aa = json_set_array(o, "actuators");
    for (int i = 0; i < s->nact; i++) {
        const ActuatorDef *A = &s->act[i];
        JsonValue *x = json_object();
        json_set_string(x, "name", A->name);
        json_set_string(x, "type", actuator_type_name(A->type));
        json_set_string(x, "joint", def->joints[A->joint].name);
        static const char *const K[] = {"effort_limit", "velocity_limit", "rated_effort", "kp", "kd", "kv", "initial_command", "gear_ratio", "efficiency",
                                        "rotor_inertia", "resistance", "torque_constant", "back_emf_constant", "voltage_limit", "current_limit",
                                        "motor_viscous", "motor_coulomb", "motor_coulomb_vreg"};
        double V[] = {A->effort_limit, A->velocity_limit, A->rated_effort, A->kp, A->kd, A->kv, A->initial_command, A->gear_ratio, A->efficiency,
                      A->rotor_inertia, A->resistance, A->torque_constant, A->back_emf_constant, A->voltage_limit, A->current_limit, A->motor_viscous,
                      A->motor_coulomb, A->motor_coulomb_vreg};
        for (size_t k = 0; k < sizeof V / sizeof *V; k++) json_set_number(x, K[k], V[k]);
        json_push(aa, x);
    }
    JsonValue *sa = json_set_array(o, "sensors");
    for (int i = 0; i < s->nsen; i++) {
        const SensorDef *S = &s->sen[i];
        JsonValue *x = json_object();
        json_set_string(x, "name", S->name);
        json_set_string(x, "type", sensor_type_name(S->type));
        if (S->joint >= 0) json_set_string(x, "joint", def->joints[S->joint].name);
        if (S->actuator >= 0) json_set_string(x, "actuator", s->act[S->actuator].name);
        if (S->body >= 0) {
            json_set_string(x, "body", def->bodies[S->body].name);
            double rpy[3];
            mrpy_from_rot(rpy, S->site.R);
            JsonValue *site = json_set_object(x, "site");
            json_set(site, "position", json_numbers(S->site.p, 3));
            json_set(site, "rpy", json_numbers(rpy, 3));
        }
        json_set_number(x, "period", S->period);
        json_set_int(x, "latency_samples", S->latency_samples);
        json_set_number(x, "noise_std", S->noise_std);
        json_set_number(x, "bias", S->bias);
        json_set_number(x, "resolution", S->resolution);
        json_set_number(x, "filter_tau", S->filter_tau);
        json_set_int(x, "seed", (long long)S->seed);
        json_push(sa, x);
    }
    JsonValue *ca = json_set_array(o, "controllers");
    for (int i = 0; i < s->nctl; i++) {
        const ControllerDef *C = &s->ctl[i];
        JsonValue *x = json_object();
        json_set_string(x, "name", C->name);
        json_set_string(x, "type", C->type == CTRL_OPEN_LOOP ? "open_loop" : "pid");
        json_set_string(x, "actuator", s->act[C->actuator].name);
        json_set_string(x, "feedback", C->feedback_sensor >= 0 ? s->sen[C->feedback_sensor].name : "ideal");
        json_set_string(x, "loop", C->control_velocity ? "velocity" : "position");
        json_set_number(x, "period", C->period);
        json_set_int(x, "delay_samples", C->delay_samples);
        json_set_number(x, "kp", C->kp), json_set_number(x, "ki", C->ki), json_set_number(x, "kd", C->kd);
        json_set_number(x, "derivative_filter", C->derivative_filter);
        JsonValue *ff = json_set_object(x, "feedforward");
        json_set_number(ff, "velocity", C->kff_velocity), json_set_number(ff, "acceleration", C->kff_acceleration);
        double lim[2] = {C->output_min, C->output_max};
        if (lim[1] > lim[0]) json_set(x, "output_limits", json_numbers(lim, 2)); /* no limits (max <= min) are omitted */
        json_set_string(x, "antiwindup", C->antiwindup == AW_CLAMP ? "clamp" : (C->antiwindup == AW_BACK_CALCULATION ? "back_calculation" : "none"));
        json_set_number(x, "tracking_time", C->tracking_time);
        const Trajectory *T = &C->reference;
        JsonValue *rf = json_set_object(x, "reference");
        json_set_string(rf, "type", trajectory_type_name(T->type));
        json_set_number(rf, "start", T->t0);
        switch (T->type) {
        case TRAJ_CONSTANT: json_set_number(rf, "value", T->value); break;
        case TRAJ_SINE:
            json_set_number(rf, "value", T->value), json_set_number(rf, "amplitude", T->amplitude);
            json_set_number(rf, "frequency", T->frequency), json_set_number(rf, "phase", T->phase);
            break;
        case TRAJ_WAYPOINTS: break;
        default:
            json_set_number(rf, "from", T->value), json_set_number(rf, "to", T->value1);
            if (T->type == TRAJ_RAMP) json_set_number(rf, "rate", T->rate);
            if (T->type == TRAJ_MIN_JERK) json_set_number(rf, "duration", T->duration);
            if (T->type == TRAJ_TRAPEZOID) json_set_number(rf, "vmax", T->vmax), json_set_number(rf, "amax", T->amax);
            break;
        }
        if (T->npoints) {
            JsonValue *wp = json_set_array(rf, "waypoints");
            for (int k = 0; k < T->npoints; k++) {
                JsonValue *p = json_object();
                json_set_number(p, "time", T->wt[k]), json_set_number(p, "value", T->wp[k]);
                json_push(wp, p);
            }
        }
        json_push(ca, x);
    }
    return o;
}
