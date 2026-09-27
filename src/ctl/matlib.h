/* matlib.h - material records: the built-in library (materials.json, embedded at build time) and materials defined in
 * a project. Every record states its status (demonstration, user_supplied, calibrated, measured, published) and
 * provenance. `measured` and `published` mean the values come from a measurement or a publication this repository
 * holds, and each value carries its own source. Properties are
 * constants or temperature tables (degC in the file, K in memory, SI values), interpolated linearly and held
 * constant outside the table. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"

enum { MAT_TABLE_MAX = 64 };

typedef struct MatTable {
    int n;                   /* 0: property absent */
    double t[MAT_TABLE_MAX]; /* K, strictly increasing */
    double v[MAT_TABLE_MAX]; /* SI */
} MatTable;

typedef enum {
    MATP_DENSITY = 0, /* kg/m^3 */
    MATP_E,           /* Pa */
    MATP_NU,
    MATP_ALPHA,       /* instantaneous expansion coefficient, 1/K */
    MATP_K,           /* W/(m K): isotropic, or the first principal value of an anisotropic conductivity */
    MATP_K2,          /* W/(m K): second principal conductivity (absent = isotropic) */
    MATP_K3,          /* W/(m K): third principal conductivity */
    MATP_CP,          /* J/(kg K) */
    MATP_EMISSIVITY,
    MATP_YIELD,       /* Pa */
    MATP_HARDENING,   /* Pa */
    MATP_VISCOSITY,   /* Pa s: dynamic viscosity of a fluid */
    MATP_COUNT
} MatProperty;

typedef struct MaterialRecord {
    char id[64];
    char name[160];
    char family[16]; /* polymer, metal, other */
    char status[24]; /* demonstration, user_supplied, calibrated, measured, published */
    bool builtin;
    MatTable prop[MATP_COUNT];
    double solidus_k, liquidus_k, latent_heat, anneal_k, glass_transition_k; /* NAN when absent */
    /* rows = the conductivity principal directions in the body frame; all zero = aligned with the body axes.
     * Only meaningful when conductivity_w_per_mk_2 (and optionally _3) are given. */
    double k_axes[9];
    bool has_k_axes;
    const JsonValue *json;                                                   /* source record (not owned) */
} MaterialRecord;

const char *matprop_key(MatProperty p);  /* JSON key, e.g. "youngs_modulus_pa" */
const char *matprop_unit(MatProperty p); /* SI unit */
/* the parsed built-in library ({"materials": [...]}); NULL with err when the embedded file is invalid */
const JsonValue *matlib_builtin(char *err, size_t errlen);
bool matlib_is_builtin_id(const char *id);
/* validates a record; err names the offending field */
bool material_from_json(const JsonValue *rec, bool builtin, MaterialRecord *out, char *err, size_t errlen);
/* project records (a JSON array, may be NULL) take precedence over the library */
bool material_lookup(const JsonValue *project_records, const char *id, MaterialRecord *out, char *err, size_t errlen);
double mat_eval(const MatTable *t, double temperature_k); /* NAN when absent */
bool mat_table_range(const MatTable *t, double *tmin_k, double *tmax_k); /* false for constants and absent tables */
/* id, name, family, processes, status, provenance, values at 20 degC, temperature-dependent keys, not_modelled */
JsonValue *material_summary_json(const MaterialRecord *m);
