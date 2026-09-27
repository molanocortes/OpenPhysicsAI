/* matlib.c - material records and the embedded material library */
#include "matlib.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

extern const char NAVIER_MATERIALS[];
extern const size_t NAVIER_MATERIALS_len;

static const struct {
    const char *key, *unit;
    double lo, hi; /* plausible range of the SI value, to catch unit mistakes (GPa given as Pa, percent as fraction) */
} PROPS[MATP_COUNT] = {
    [MATP_DENSITY] = {"density_kg_m3", "kg/m^3", 1, 1e5},
    [MATP_E] = {"youngs_modulus_pa", "Pa", 1e3, 2e12},
    [MATP_NU] = {"poisson_ratio", "1", -0.99, 0.499},
    [MATP_ALPHA] = {"expansion_1_per_k", "1/K", -1e-3, 1e-3},
    [MATP_K] = {"conductivity_w_per_mk", "W/(m*K)", 1e-4, 5e3},
    [MATP_K2] = {"conductivity_w_per_mk_2", "W/(m*K)", 1e-4, 5e3},
    [MATP_K3] = {"conductivity_w_per_mk_3", "W/(m*K)", 1e-4, 5e3},
    [MATP_CP] = {"specific_heat_j_per_kgk", "J/(kg*K)", 1, 1e5},
    [MATP_EMISSIVITY] = {"emissivity", "1", 0, 1},
    [MATP_YIELD] = {"yield_strength_pa", "Pa", 1e3, 1e11},
    [MATP_HARDENING] = {"hardening_modulus_pa", "Pa", 0, 1e12},
    [MATP_VISCOSITY] = {"dynamic_viscosity_pa_s", "Pa*s", 1e-7, 1e3},
};

/* recorded for people and agents, not used by a solver: a polymer's melting temperature, elongation, and the print
 * temperatures its supplier states (nozzle and bed, as {"min", "max"} in degC) */
static const char *const SCALAR_KEYS[] = {"solidus_c", "liquidus_c", "latent_heat_j_per_kg", "anneal_temperature_c", "glass_transition_c", "strength_pa",
                                          "melting_c", "elongation_at_break_pct", "elongation_at_yield_pct", "nozzle_temperature_c",
                                          "bed_temperature_c", NULL};
static const char *const OTHER_KEYS[] = {"$comment",  "id",           "name",  "family",      "processes", "status", "provenance",
                                        "powder",    "not_modelled", "calibration", "notes", "conductivity_axes", NULL};

const char *matprop_key(MatProperty p) { return p >= 0 && p < MATP_COUNT ? PROPS[p].key : "?"; }
const char *matprop_unit(MatProperty p) { return p >= 0 && p < MATP_COUNT ? PROPS[p].unit : "?"; }

static JsonValue *g_lib;
static char g_lib_err[300];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void lib_init(void) {
    JsonError je;
    g_lib = json_parse(NAVIER_MATERIALS, NAVIER_MATERIALS_len, NULL, &je);
    if (!g_lib) {
        snprintf(g_lib_err, sizeof g_lib_err, "materials.json: %s (line %d, column %d)", je.message, je.line, je.column);
        return;
    }
    const JsonValue *mats = json_get(g_lib, "materials");
    if (!mats || mats->type != JSON_ARRAY) {
        snprintf(g_lib_err, sizeof g_lib_err, "materials.json has no \"materials\" array");
        json_free(g_lib), g_lib = NULL;
        return;
    }
    for (size_t i = 0; i < json_len(mats); i++) {
        MaterialRecord m;
        char err[256];
        if (!material_from_json(json_at(mats, i), true, &m, err, sizeof err)) {
            snprintf(g_lib_err, sizeof g_lib_err, "materials.json entry %zu: %s", i, err);
            json_free(g_lib), g_lib = NULL;
            return;
        }
        for (size_t k = 0; k < i; k++)
            if (!strcmp(json_get_str(json_at(mats, k), "id", ""), m.id)) {
                snprintf(g_lib_err, sizeof g_lib_err, "materials.json: duplicate id %s", m.id);
                json_free(g_lib), g_lib = NULL;
                return;
            }
    }
}

const JsonValue *matlib_builtin(char *err, size_t errlen) {
    pthread_once(&g_once, lib_init);
    if (!g_lib && err) snprintf(err, errlen, "%s", g_lib_err);
    return g_lib;
}

bool matlib_is_builtin_id(const char *id) {
    const JsonValue *mats = json_get(matlib_builtin(NULL, 0), "materials");
    for (size_t i = 0; id && i < json_len(mats); i++)
        if (!strcmp(json_get_str(json_at(mats, i), "id", ""), id)) return true;
    return false;
}

static bool parse_table(const JsonValue *o, int p, MatTable *t, char *err, size_t errlen) {
    const char *key = PROPS[p].key;
    t->n = 0;
    if (!o) return true;
    if (o->type != JSON_OBJECT) {
        snprintf(err, errlen, "%s must be {\"value\": number} or {\"t_c\": [...], \"value\": [...]}", key);
        return false;
    }
    const JsonValue *v = json_get(o, "value"), *tc = json_get(o, "t_c");
    if (v && v->type == JSON_NUMBER && !tc) {
        t->n = 1;
        t->t[0] = 293.15;
        t->v[0] = v->u.number;
    } else if (v && v->type == JSON_ARRAY && tc && tc->type == JSON_ARRAY) {
        size_t n = json_len(v);
        if (n < 1 || n > MAT_TABLE_MAX || json_len(tc) != n) {
            snprintf(err, errlen, "%s: t_c and value must be arrays of equal length (1 to %d entries)", key, MAT_TABLE_MAX);
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            const JsonValue *ti = json_at(tc, i), *vi = json_at(v, i);
            if (!ti || ti->type != JSON_NUMBER || !vi || vi->type != JSON_NUMBER) {
                snprintf(err, errlen, "%s: entry %zu is not a number", key, i);
                return false;
            }
            if (ti->u.number < -273.15) {
                snprintf(err, errlen, "%s: temperature %g degC is below absolute zero", key, ti->u.number);
                return false;
            }
            t->t[i] = ti->u.number + 273.15;
            t->v[i] = vi->u.number;
            if (i && !(t->t[i] > t->t[i - 1])) {
                snprintf(err, errlen, "%s: t_c must increase strictly", key);
                return false;
            }
        }
        t->n = (int)n;
    } else {
        snprintf(err, errlen, "%s must be {\"value\": number} or {\"t_c\": [...], \"value\": [...]}", key);
        return false;
    }
    for (int i = 0; i < t->n; i++)
        if (!(t->v[i] >= PROPS[p].lo && t->v[i] <= PROPS[p].hi)) {
            snprintf(err, errlen, "%s = %g is outside the plausible range %g to %g %s (check the unit: values are SI)", key, t->v[i], PROPS[p].lo,
                     PROPS[p].hi, PROPS[p].unit);
            t->n = 0;
            return false;
        }
    return true;
}

static double scalar(const JsonValue *rec, const char *key, double offset) {
    const JsonValue *v = json_get(json_get(rec, key), "value");
    return v && v->type == JSON_NUMBER ? v->u.number + offset : NAN;
}

static bool one_of(const char *s, const char *const *list) {
    for (int i = 0; s && list[i]; i++)
        if (!strcmp(s, list[i])) return true;
    return false;
}

bool material_from_json(const JsonValue *rec, bool builtin, MaterialRecord *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (!rec || rec->type != JSON_OBJECT) {
        snprintf(err, errlen, "a material record must be an object");
        return false;
    }
    const char *id = json_get_str(rec, "id", NULL), *name = json_get_str(rec, "name", NULL), *family = json_get_str(rec, "family", NULL),
               *status = json_get_str(rec, "status", NULL), *prov = json_get_str(rec, "provenance", NULL);
    if (!id || !*id || strlen(id) > 63) {
        snprintf(err, errlen, "id must be 1-63 characters");
        return false;
    }
    for (const char *c = id; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.')) {
            snprintf(err, errlen, "id '%s' may only contain letters, digits, '_', '-' and '.'", id);
            return false;
        }
    static const char *const FAMILIES[] = {"polymer", "metal", "other", "fluid", NULL}, *const STATUSES[] = {"demonstration", "user_supplied", "calibrated", "measured", "published", NULL};
    if (!name || !*name) {
        snprintf(err, errlen, "material '%s': name is required", id);
        return false;
    }
    if (!one_of(family, FAMILIES)) {
        snprintf(err, errlen, "material '%s': family must be polymer, metal, other or fluid", id);
        return false;
    }
    if (!one_of(status, STATUSES)) {
        snprintf(err, errlen, "material '%s': status must be demonstration, user_supplied, calibrated, measured or "
                              "published", id);
        return false;
    }
    /* A built-in entry was demonstration data only, until 2026-09-18, when the first measured record arrived. A
     * built-in may now be labelled measured or published, and then every one of its property objects must carry its
     * own `source`: the label is a claim about where each number came from, and it is checked, not trusted. */
    bool sourced = !strcmp(status, "measured") || !strcmp(status, "published");
    if (builtin && sourced) {
        static const char *const VALUED[] = {"density_kg_m3", "youngs_modulus_pa", "poisson_ratio", "yield_strength_pa",
                                             "tensile_strength_pa", "hardening_modulus_pa", "expansion_1_per_k",
                                             "conductivity_w_per_mk", "specific_heat_j_per_kgk", "strength_pa", NULL};
        for (int i = 0; VALUED[i]; i++) {
            const JsonValue *v = json_get(rec, VALUED[i]);
            if (!v || v->type != JSON_OBJECT) continue;
            const char *src = json_get_str(v, "source", NULL);
            if (!src || !*src) {
                snprintf(err, errlen, "material '%s' is labelled %s, so every value must name its source: %s does not",
                         id, status, VALUED[i]);
                return false;
            }
        }
    } else if (builtin && strcmp(status, "demonstration") != 0) {
        snprintf(err, errlen, "material '%s': a library entry is demonstration data unless every value names the "
                              "measurement or publication it came from, and is then labelled measured or published", id);
        return false;
    }
    if (!prov || strlen(prov) < 3) {
        snprintf(err, errlen, "material '%s': provenance is required (where the values come from)", id);
        return false;
    }
    for (size_t i = 0; i < json_len(rec); i++) {
        const char *k = json_key_at(rec, i);
        bool known = one_of(k, SCALAR_KEYS) || one_of(k, OTHER_KEYS);
        for (int p = 0; p < MATP_COUNT && !known; p++) known = !strcmp(k, PROPS[p].key);
        if (!known) {
            snprintf(err, errlen, "material '%s': unknown field '%s'", id, k);
            return false;
        }
    }
    snprintf(out->id, sizeof out->id, "%s", id);
    snprintf(out->name, sizeof out->name, "%s", name);
    snprintf(out->family, sizeof out->family, "%s", family);
    snprintf(out->status, sizeof out->status, "%s", status);
    for (int p = 0; p < MATP_COUNT; p++)
        if (!parse_table(json_get(rec, PROPS[p].key), p, &out->prop[p], err, errlen)) return false;
    out->solidus_k = scalar(rec, "solidus_c", 273.15);
    out->liquidus_k = scalar(rec, "liquidus_c", 273.15);
    out->latent_heat = scalar(rec, "latent_heat_j_per_kg", 0);
    out->anneal_k = scalar(rec, "anneal_temperature_c", 273.15);
    out->glass_transition_k = scalar(rec, "glass_transition_c", 273.15);
    if (isfinite(out->solidus_k) && isfinite(out->liquidus_k) && !(out->liquidus_k > out->solidus_k)) {
        snprintf(err, errlen, "material '%s': liquidus must be above solidus", id);
        return false;
    }
    /* anisotropic conductivity: principal directions as three orthonormal rows in the body frame. Checked here so
     * that a non-orthonormal frame is rejected when the material is defined, not when a solve starts. */
    memset(out->k_axes, 0, sizeof out->k_axes);
    out->has_k_axes = false;
    const JsonValue *ax = json_get(rec, "conductivity_axes");
    if (ax) {
        if (ax->type != JSON_ARRAY || json_len(ax) != 3) {
            snprintf(err, errlen, "material '%s': conductivity_axes must be three rows of three numbers", id);
            return false;
        }
        for (int r = 0; r < 3; r++) {
            const JsonValue *row = json_at(ax, r);
            if (!row || row->type != JSON_ARRAY || json_len(row) != 3) {
                snprintf(err, errlen, "material '%s': conductivity_axes row %d must hold three numbers", id, r);
                return false;
            }
            for (int c = 0; c < 3; c++) {
                const JsonValue *v = json_at(row, c);
                if (!v || v->type != JSON_NUMBER || !isfinite(v->u.number)) {
                    snprintf(err, errlen, "material '%s': conductivity_axes[%d][%d] must be a finite number", id, r, c);
                    return false;
                }
                out->k_axes[3 * r + c] = v->u.number;
            }
        }
        for (int a = 0; a < 3; a++)
            for (int b2 = a; b2 < 3; b2++) {
                double d = 0;
                for (int c = 0; c < 3; c++) d += out->k_axes[3 * a + c] * out->k_axes[3 * b2 + c];
                if (fabs(d - (a == b2 ? 1.0 : 0.0)) > 1e-9) {
                    snprintf(err, errlen,
                             "material '%s': conductivity_axes must be orthonormal (row %d . row %d = %.6g); the rows are unit "
                             "vectors of the principal directions in the body frame",
                             id, a, b2, d);
                    return false;
                }
            }
        out->has_k_axes = true;
    }
    if (!out->prop[MATP_K2].n && (out->prop[MATP_K3].n || out->has_k_axes)) {
        snprintf(err, errlen, "material '%s': conductivity_w_per_mk_2 is needed before a third principal value or conductivity_axes", id);
        return false;
    }
    out->builtin = builtin;
    out->json = rec;
    return true;
}

bool material_lookup(const JsonValue *project_records, const char *id, MaterialRecord *out, char *err, size_t errlen) {
    for (size_t i = 0; id && i < json_len(project_records); i++)
        if (!strcmp(json_get_str(json_at(project_records, i), "id", ""), id)) return material_from_json(json_at(project_records, i), false, out, err, errlen);
    const JsonValue *lib = matlib_builtin(err, errlen);
    if (!lib) return false;
    const JsonValue *mats = json_get(lib, "materials");
    for (size_t i = 0; id && i < json_len(mats); i++)
        if (!strcmp(json_get_str(json_at(mats, i), "id", ""), id)) return material_from_json(json_at(mats, i), true, out, err, errlen);
    snprintf(err, errlen, "no material with id '%s' (materials_list shows the library and the project's materials)", id ? id : "");
    return false;
}

double mat_eval(const MatTable *t, double T) {
    if (!t->n) return NAN;
    if (t->n == 1 || T <= t->t[0]) return t->v[0];
    if (T >= t->t[t->n - 1]) return t->v[t->n - 1];
    int lo = 0, hi = t->n - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (t->t[mid] <= T) lo = mid;
        else hi = mid;
    }
    double w = (T - t->t[lo]) / (t->t[hi] - t->t[lo]);
    return t->v[lo] + w * (t->v[hi] - t->v[lo]);
}

bool mat_table_range(const MatTable *t, double *tmin, double *tmax) {
    if (t->n < 2) return false;
    *tmin = t->t[0];
    *tmax = t->t[t->n - 1];
    return true;
}

JsonValue *material_summary_json(const MaterialRecord *m) {
    JsonValue *o = json_object();
    json_set_string(o, "id", m->id);
    json_set_string(o, "name", m->name);
    json_set_string(o, "family", m->family);
    const JsonValue *proc = json_get(m->json, "processes");
    json_set(o, "processes", proc ? json_clone(proc) : json_array());
    json_set_string(o, "status", m->status);
    json_set_string(o, "defined_in", m->builtin ? "library" : "project");
    json_set_string(o, "provenance", json_get_str(m->json, "provenance", ""));
    JsonValue *rt = json_set_object(o, "values_at_20_c");
    JsonValue *tdep = json_set_array(o, "temperature_dependent");
    double tlo = INFINITY, thi = -INFINITY;
    for (int p = 0; p < MATP_COUNT; p++) {
        if (!m->prop[p].n) continue;
        json_set_number(rt, PROPS[p].key, mat_eval(&m->prop[p], 293.15));
        double a, b;
        if (mat_table_range(&m->prop[p], &a, &b)) {
            json_push(tdep, json_string(PROPS[p].key));
            tlo = fmin(tlo, a), thi = fmax(thi, b);
        }
    }
    if (thi >= tlo) {
        JsonValue *r = json_array();
        json_push(r, json_number(tlo - 273.15));
        json_push(r, json_number(thi - 273.15));
        json_set(o, "table_range_c", r);
    }
    if (isfinite(m->glass_transition_k)) json_set_number(o, "glass_transition_c", m->glass_transition_k - 273.15);
    if (isfinite(m->solidus_k)) json_set_number(o, "solidus_c", m->solidus_k - 273.15);
    if (isfinite(m->liquidus_k)) json_set_number(o, "liquidus_c", m->liquidus_k - 273.15);
    const JsonValue *nm = json_get(m->json, "not_modelled");
    if (nm) json_set(o, "not_modelled", json_clone(nm));
    if (!strcmp(m->status, "demonstration"))
        json_set_string(o, "warning", "demonstration values: not traceable to a grade, supplier or test and not calibrated; results show trends only");
    return o;
}
