/* study_resolve.c - findings, logged operations on private engines, and resolution of a study definition */
#include "../core/sha256.h"
#include "../core/units.h"
#include "matlib.h"
#include "selection.h"
#include "study_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

double study_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + 1e-6 * (double)tv.tv_usec;
}

void study_hash_json(const JsonValue *v, char out[65]) {
    size_t n = 0;
    char *t = json_dump(v, JSON_SORTED, &n, NULL);
    sha256_hex_of(t ? t : "", t ? n : 0, out);
    free(t);
}

/* ------------------------------------------------------------------------------------------------ findings */

void si_init(StudyIssues *is, const JsonValue *accept) {
    memset(is, 0, sizeof *is);
    is->questions = json_array();
    is->assumptions = json_array();
    is->unsupported = json_array();
    is->not_evaluated = json_array();
    is->warnings = json_array();
    is->accepted = json_array();
    is->accept = accept;
}

void si_free(StudyIssues *is) {
    json_free(is->questions), json_free(is->assumptions), json_free(is->unsupported);
    json_free(is->not_evaluated), json_free(is->warnings), json_free(is->accepted);
    memset(is, 0, sizeof *is);
}

static const char *accepted_reason(const StudyIssues *is, const char *id) {
    for (size_t i = 0; i < json_len(is->accept); i++) {
        const JsonValue *a = json_at(is->accept, i);
        if (!strcmp(json_get_str(a, "id", ""), id)) return json_get_str(a, "reason", "");
    }
    return NULL;
}

void si_question(StudyIssues *is, const char *id, bool blocking, bool acceptable, const char *why, JsonValue *details, const char *fmt, ...) {
    char text[1600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    const char *reason = acceptable ? accepted_reason(is, id) : NULL;
    if (reason) {
        JsonValue *a = json_object();
        json_set_string(a, "id", id);
        json_set_string(a, "question", text);
        json_set_string(a, "reason", reason);
        if (details) json_set(a, "details", details);
        json_push(is->accepted, a);
        si_assume(is, id, "user", "accepted by the user; not assessed further", "%s Accepted: %s", text, reason);
        return;
    }
    JsonValue *q = json_object();
    json_set_string(q, "id", id);
    json_set_bool(q, "blocking", blocking);
    json_set_bool(q, "acceptable", acceptable);
    json_set_string(q, "question", text);
    if (why) json_set_string(q, "why_it_matters", why);
    if (acceptable)
        json_set_string(q, "how_to_answer", "change the definition, or add {\"id\": \"<this id>\", \"reason\": \"...\"} to accept, which records it as a user assumption");
    else
        json_set_string(q, "how_to_answer", "change the definition; this cannot be accepted as an assumption");
    if (details) json_set(q, "details", details);
    json_push(is->questions, q);
    if (blocking) is->blocking++;
}

void si_assume(StudyIssues *is, const char *subject, const char *source, const char *effect, const char *fmt, ...) {
    char text[1800];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    JsonValue *a = json_object();
    json_set_string(a, "subject", subject);
    json_set_string(a, "text", text);
    json_set_string(a, "source", source);
    json_set_string(a, "effect", effect ? effect : "not assessed");
    json_push(is->assumptions, a);
}

void si_unsupported(StudyIssues *is, const char *id, bool blocking, const char *fmt, ...) {
    char text[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    JsonValue *u = json_object();
    json_set_string(u, "id", id);
    json_set_string(u, "text", text);
    json_set_bool(u, "blocking", blocking);
    json_push(is->unsupported, u);
    if (blocking) is->not_supported = true;
}

void si_not_evaluated(StudyIssues *is, const char *item, const char *reason) {
    for (size_t i = 0; i < json_len(is->not_evaluated); i++)
        if (!strcmp(json_get_str(json_at(is->not_evaluated, i), "item", ""), item)) return;
    JsonValue *o = json_object();
    json_set_string(o, "item", item);
    json_set_string(o, "reason", reason);
    json_push(is->not_evaluated, o);
}

void si_warn(StudyIssues *is, const char *code, JsonValue *details, const char *fmt, ...) {
    char text[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    JsonValue *w = json_object();
    json_set_string(w, "code", code);
    json_set_string(w, "message", text);
    if (details) json_set(w, "details", details);
    json_push(is->warnings, w);
}

/* ------------------------------------------------------------------------------------------------ operations */

static const OpCaller STUDY_CALLER = {"study", "comparison study"};

bool study_op(Engine *eng, StudyLog *lg, const char *op, JsonValue *params, OpResult *r) {
    memset(r, 0, sizeof *r);
    double t0 = study_now();
    ops_invoke(eng, op, params, &STUDY_CALLER, r);
    if (lg && (lg->f || lg->mem)) {
        JsonValue *o = json_object();
        char ts[32];
        iso_time_now(ts, sizeof ts);
        json_set_string(o, "time", ts);
        if (lg->design[0]) json_set_string(o, "design", lg->design);
        json_set_string(o, "operation", op);
        json_set(o, "params", json_clone(params));
        json_set_bool(o, "ok", r->ok);
        json_set_number(o, "seconds", study_now() - t0);
        if (!r->ok && r->error) {
            json_set_string(o, "error_code", json_get_str(r->error, "code", ""));
            json_set_string(o, "error_message", json_get_str(r->error, "message", ""));
        }
        if (lg->f) {
            char *t = json_dump(o, JSON_SORTED, NULL, NULL);
            if (t) fprintf(lg->f, "%s\n", t), fflush(lg->f);
            free(t);
            json_free(o);
        } else {
            json_push(lg->mem, o);
        }
    }
    json_free(params);
    return r->ok;
}

void study_op_error(const OpResult *r, char *out, size_t cap) {
    snprintf(out, cap, "%s: %s", json_get_str(r->error, "code", "ERROR"), json_get_str(r->error, "message", "operation failed"));
}

/* ------------------------------------------------------------------------------------------------ resolution */

const char *study_project_source(const char *s) {
    if (!s || !strcmp(s, "default")) return "default";
    if (!strcmp(s, "inferred")) return "inferred";
    return "user"; /* user, measured and database values are stated inputs; the study keeps the finer distinction */
}

static const char *src(const JsonValue *o, const char *key, const char *def) { return json_get_str(o, key, def); }

static bool contains_word(const char *text, const char *word) {
    size_t n = strlen(word);
    for (const char *p = text; p && *p; p++) {
        if (strncasecmp(p, word, n) != 0) continue;
        bool start = p == text || !isalpha((unsigned char)p[-1]);
        if (start) return true;
    }
    return false;
}

/* the material of the study or of one design: {id, status, source, reference, record, youngs_modulus_pa, poisson_ratio,
 * density_kg_m3}; NULL with a question when it cannot be resolved */
static JsonValue *resolve_material(const JsonValue *m, const char *who, StudyIssues *is) {
    char qid[128];
    const JsonValue *record = json_get(m, "record");
    const char *id = json_get_str(m, "id", NULL);
    snprintf(qid, sizeof qid, "%s.material", who);
    if (!m || (!record && !id)) {
        si_question(is, qid, true, false,
                    "stiffness scales with Young's modulus and mass with density; with no material there is nothing to compare",
                    NULL, "Which material (and grade or supplier) is %s made of? Give a library id (materials_list) or a record with youngs_modulus_pa, "
                          "poisson_ratio and density_kg_m3 and their source.",
                    !strcmp(who, "study") ? "the part" : who);
        return NULL;
    }
    MaterialRecord rec;
    char err[400];
    bool ok = record ? material_from_json(record, false, &rec, err, sizeof err) : material_lookup(NULL, id, &rec, err, sizeof err);
    if (!ok) {
        si_question(is, qid, true, false, "the material record cannot be used as given", json_string(err), "The material of %s is not valid: %s", who, err);
        return NULL;
    }
    double T = 293.15, E = mat_eval(&rec.prop[MATP_E], T), nu = mat_eval(&rec.prop[MATP_NU], T), rho = mat_eval(&rec.prop[MATP_DENSITY], T);
    if (!isfinite(E) || !isfinite(nu)) {
        si_question(is, qid, true, false, "a linear elastic analysis needs Young's modulus and Poisson's ratio", NULL,
                    "Material '%s' defines no elastic properties: what are its Young's modulus and Poisson's ratio?", rec.id);
        return NULL;
    }
    JsonValue *o = json_object();
    json_set_string(o, "id", rec.id);
    json_set_string(o, "name", rec.name);
    json_set_string(o, "status", rec.status);
    json_set_string(o, "source", src(m, "source", "user"));
    json_set_string(o, "reference", json_get_str(m, "reference", json_get_str(rec.json, "provenance", "")));
    json_set_bool(o, "library", !record);
    json_set(o, "record", json_clone(rec.json));
    json_set_number(o, "youngs_modulus_pa", E);
    json_set_number(o, "poisson_ratio", nu);
    if (isfinite(rho)) json_set_number(o, "density_kg_m3", rho);
    if (!strcmp(rec.status, "demonstration"))
        si_assume(is, "demonstration_material", "default",
                  "the ranking of designs made of the same material does not depend on Young's modulus (sensitivity section); absolute values do",
                  "Material '%s' holds demonstration values, not data for a real grade: absolute displacements, stiffnesses and masses are indicative "
                  "only.",
                  rec.id);
    const char *s = src(m, "source", "user");
    if (!strcmp(s, "default") || !strcmp(s, "inferred"))
        si_assume(is, "material_choice", s, "Young's modulus scaling is reported in the sensitivity section when youngs_modulus_relative is given",
                  "The material of %s ('%s') was %s, not stated by the user.", who, rec.id, !strcmp(s, "default") ? "assumed" : "inferred");
    if (!isfinite(rho))
        si_warn(is, "DENSITY_MISSING", NULL, "material '%s' has no density: mass and self-weight cannot be evaluated", rec.id);
    return o;
}

static JsonValue *resolve_region(const JsonValue *r) {
    JsonValue *o = json_object();
    json_set(o, "query", json_clone(json_get(r, "query")));
    json_set_string(o, "mode", json_get_str(r, "mode", "patch"));
    json_set_number(o, "coverage", json_get_num(r, "coverage", 0.5));
    json_set_string(o, "description", json_get_str(r, "description", ""));
    json_set_string(o, "source", json_get_str(r, "source", "user"));
    return o;
}

static bool length_mm(const JsonValue *v, double def_mm, double *mm) {
    if (!v) {
        *mm = def_mm;
        return true;
    }
    double si;
    char err[200];
    if (!quantity_from_json(v, DIM_LENGTH, "mm", &si, err, sizeof err)) return false;
    *mm = 1e3 * si;
    return true;
}

static int cmp_desc(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) - (x > y);
}

JsonValue *study_resolve(Engine *e, const JsonValue *def, StudyIssues *is, NvErr *code, char *err, size_t errlen) {
    JsonValue *res = json_object();
    json_set_string(res, "format", "navier-comparison-study");
    json_set_int(res, "format_version", 1);
    json_set_string(res, "name", json_get_str(def, "name", "study"));
    json_set_string(res, "question", json_get_str(def, "question", ""));
    if (json_get(def, "decision")) json_set_string(res, "decision", json_get_str(def, "decision", ""));

    /* material first: designs may override it */
    JsonValue *material = resolve_material(json_get(def, "material"), "study", is);
    json_set(res, "material", material ? material : json_null());

    /* designs */
    const JsonValue *designs = json_get(def, "designs");
    JsonValue *rd = json_set_array(res, "designs");
    char seen[STUDY_MAX_DESIGNS][64];
    for (size_t i = 0; i < json_len(designs) && i < STUDY_MAX_DESIGNS; i++) {
        const JsonValue *d = json_at(designs, i);
        const char *name = json_get_str(d, "name", "");
        for (size_t k = 0; k < i; k++)
            if (!strcmp(seen[k], name)) {
                *code = NV_ERR_INVALID_PARAMS;
                snprintf(err, errlen, "two designs are named '%s'", name);
                json_free(res);
                return NULL;
            }
        snprintf(seen[i], sizeof seen[i], "%s", name);
        JsonValue *o = json_object();
        json_set_string(o, "name", name);
        json_set_string(o, "description", json_get_str(d, "description", ""));
        const JsonValue *g = json_get(d, "geometry");
        const char *path = json_get_str(g, "path", "");
        char real[NV_PATH_MAX], perr[700];
        if (!path_real(path, real, sizeof real)) {
            *code = NV_ERR_NOT_FOUND;
            snprintf(err, errlen, "design '%s': geometry file not found: %s", name, path);
            json_free(o), json_free(res);
            return NULL;
        }
        if (!engine_resolve_read_path(e, path, real, sizeof real, perr, sizeof perr)) {
            *code = NV_ERR_PERMISSION;
            snprintf(err, errlen, "design '%s': %s", name, perr);
            json_free(o), json_free(res);
            return NULL;
        }
        char sha[65];
        uint64_t bytes = 0;
        if (!sha256_file(real, sha, &bytes)) {
            *code = NV_ERR_IO;
            snprintf(err, errlen, "design '%s': cannot read %s", name, real);
            json_free(o), json_free(res);
            return NULL;
        }
        JsonValue *go = json_set_object(o, "geometry");
        json_set_string(go, "path", real);
        json_set_string(go, "sha256", sha);
        json_set_int(go, "bytes", (long long)bytes);
        const char *units = json_get_str(g, "units", NULL);
        if (!units || !strcmp(units, "unknown")) {
            json_set(go, "units", json_null());
            char qid[128];
            snprintf(qid, sizeof qid, "%s.units", name);
            si_question(is, qid, true, false,
                        "an STL file stores bare numbers: the same file is 80 mm or 80 inches across, which changes stiffness by orders of magnitude",
                        NULL, "In which length unit was the geometry of design '%s' exported (mm, cm, m, in)?", name);
        } else {
            json_set_string(go, "units", units);
            json_set_string(go, "units_source", json_get_str(g, "units_source", "user"));
            const char *us = json_get_str(g, "units_source", "user");
            if (strcmp(us, "user") != 0)
                si_assume(is, "geometry_units", us, "the size plausibility check of the import is reported with the design",
                          "The unit of design '%s' (%s) was %s, not stated by the user.", name, units, us);
        }
        json_set(o, "mounting_region", resolve_region(json_get(d, "mounting_region")));
        json_set(o, "load_region", resolve_region(json_get(d, "load_region")));
        const JsonValue *dm = json_get(d, "material");
        if (dm) {
            JsonValue *mo = resolve_material(dm, name, is);
            json_set(o, "material", mo ? mo : json_null());
        }
        json_push(rd, o);
    }
    if (json_len(designs) < 2) {
        *code = NV_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "a comparison needs at least two designs");
        json_free(res);
        return NULL;
    }

    /* manufacturing */
    const JsonValue *mf = json_get(def, "manufacturing");
    JsonValue *mo = json_set_object(res, "manufacturing");
    const char *process = json_get_str(mf, "process", "unspecified");
    json_set_string(mo, "process", process);
    json_set_string(mo, "source", json_get_str(mf, "source", mf ? "user" : "default"));
    static const char *const MF_KEYS[] = {"orientation", "infill_percent", "layer_height", "effective_properties", "notes", NULL};
    for (int k = 0; MF_KEYS[k]; k++)
        if (json_get(mf, MF_KEYS[k])) json_set(mo, MF_KEYS[k], json_clone(json_get(mf, MF_KEYS[k])));
    bool additive = !strcmp(process, "fff") || !strcmp(process, "sla") || !strcmp(process, "sls") || !strcmp(process, "mjf") || !strcmp(process, "lpbf");
    if (!strcmp(process, "unspecified"))
        si_assume(is, "manufacturing", "default", "not assessed",
                  "The manufacturing process is not specified: the material values are used as given, as for a homogeneous, isotropic, defect-free "
                  "part.");
    if (additive) {
        const char *eff = json_get_str(mf, "effective_properties", "not_available");
        JsonValue *missing = json_array();
        if (!json_get(mf, "orientation")) json_push(missing, json_string("orientation"));
        if (!strcmp(process, "fff") && !json_get(mf, "infill_percent")) json_push(missing, json_string("infill_percent"));
        if (strcmp(eff, "measured_for_this_process") != 0) {
            JsonValue *details = json_object();
            json_set(details, "missing_print_information", missing);
            si_question(is, "manufacturing.printed_properties", true, true,
                        "printed parts are anisotropic and their stiffness depends on build orientation, infill, layer bonding and porosity; bulk values "
                        "can overstate stiffness substantially, and differently for designs loaded across or along the layers",
                        details,
                        "The designs are made by %s, but the material values are not stated to apply to printed parts made that way. Are there "
                        "stiffness values measured for this process, orientation and infill? Otherwise accept that the comparison assumes isotropic "
                        "bulk properties.",
                        process);
        } else {
            json_free(missing);
        }
        si_not_evaluated(is, "interlayer or print-defect failure", "not modelled: the material is homogeneous and isotropic");
        si_not_evaluated(is, "anisotropic stiffness of the printed material", "not modelled: isotropic linear elasticity");
    }

    /* mounting */
    const JsonValue *mt = json_get(def, "mounting");
    JsonValue *mto = json_set_object(res, "mounting");
    const char *ideal = json_get_str(mt, "idealization", "fixed");
    json_set_string(mto, "idealization", ideal);
    json_set_string(mto, "boundary_kind", !strcmp(ideal, "frictionless_normal") ? "frictionless_support" : "fixed");
    json_set_string(mto, "description", json_get_str(mt, "description", ""));
    const char *msrc = json_get_str(mt, "source", "user");
    json_set_string(mto, "source", msrc);
    const JsonValue *sens = json_get(def, "sensitivity");
    size_t nalt = json_len(json_get(sens, "mounting_alternatives"));
    if (strcmp(msrc, "user") != 0 && !nalt)
        si_question(is, "mounting.assumption", true, true,
                    "an ideal clamp is the stiffest possible mounting; bolts, washers and a flexible wall make brackets markedly softer, and not "
                    "equally for different designs",
                    NULL,
                    "The mounting idealisation ('%s') was %s. How is the bracket attached (bolts: number, size, preload; bonded; clamped face)? "
                    "Either describe it, add a mounting alternative to assess the effect, or accept the assumption.",
                    ideal, !strcmp(msrc, "default") ? "assumed" : "inferred");
    si_assume(is, "mounting_idealization", msrc,
              nalt ? "assessed by the mounting alternatives in the sensitivity section" : "not assessed: no mounting alternative was declared",
              "The mounting region is idealised as %s: %s. No contact, slip, fastener or wall flexibility is modelled.",
              !strcmp(ideal, "fixed") ? "rigidly fixed (all displacements zero)" : "a frictionless support (normal displacement zero)",
              json_get_str(mt, "description", "no description given"));

    /* load */
    const JsonValue *ld = json_get(def, "load");
    JsonValue *lo = json_set_object(res, "load");
    const char *kind = json_get_str(ld, "kind", "payload_mass");
    json_set_string(lo, "kind", kind);
    char ierr[300];
    double dir[3] = {0, 0, -1};
    if (!direction_from_json(json_get(ld, "direction"), dir, ierr, sizeof ierr)) {
        *code = NV_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "load.direction: %s", ierr);
        json_free(res);
        return NULL;
    }
    json_set(lo, "direction", json_vec3(dir[0], dir[1], dir[2]));
    json_set_string(lo, "direction_source", json_get_str(ld, "direction_source", json_get_str(ld, "source", "user")));
    double force = NAN, mass = NAN, g = 9.80665;
    bool have = true;
    if (!strcmp(kind, "payload_mass")) {
        const JsonValue *mv = json_get(ld, "mass");
        if (!mv || !quantity_from_json(mv, DIM_MASS, "kg", &mass, ierr, sizeof ierr)) {
            have = false;
            si_question(is, "load.mass", true, false, "displacement is proportional to the load", NULL, "What mass does the bracket carry%s%s?",
                        mv ? " (the given value is not a mass: " : "", mv ? ierr : "");
        }
        const JsonValue *gv = json_get(ld, "gravity");
        if (gv) {
            if (!quantity_from_json(gv, DIM_ACCELERATION, "m/s^2", &g, ierr, sizeof ierr)) {
                *code = NV_ERR_INVALID_UNIT;
                snprintf(err, errlen, "load.gravity: %s", ierr);
                json_free(res);
                return NULL;
            }
            json_set_string(lo, "gravity_source", json_get_str(ld, "gravity_source", "user"));
        } else {
            json_set_string(lo, "gravity_source", "default");
            si_assume(is, "gravitational_acceleration", "default", "a different g scales every displacement equally and does not change the ranking",
                      "The payload weighs mass x standard gravity 9.80665 m/s^2, applied statically: no dynamic amplification from shocks, vibration or "
                      "handling.");
        }
        if (have) {
            force = mass * g;
            json_set_number(lo, "mass_kg", mass);
            json_set_number(lo, "gravity_m_s2", g);
        }
    } else {
        const JsonValue *fv = json_get(ld, "force");
        if (!fv || !quantity_from_json(fv, DIM_FORCE, "N", &force, ierr, sizeof ierr)) {
            have = false;
            si_question(is, "load.force", true, false, "displacement is proportional to the load", NULL, "What force (magnitude) does the bracket carry?");
        }
    }
    if (have) {
        json_set_number(lo, "force_n", force);
        json_set(lo, "force_vector_n", json_vec3(force * dir[0], force * dir[1], force * dir[2]));
    }
    const char *lsrc = json_get_str(ld, "source", "user");
    json_set_string(lo, "source", lsrc);
    json_set_string(lo, "distribution", "uniform_over_region");
    json_set_string(lo, "description", json_get_str(ld, "description", ""));
    bool self_weight = json_get_bool(ld, "self_weight", false);
    json_set_bool(lo, "self_weight", self_weight);
    si_assume(is, "load_distribution", "default", "not assessed",
              "The load acts as a uniform traction over the load region, so its resultant passes through the region's centroid. A payload that "
              "bears on part of the region, or whose centre of mass is offset, loads the bracket differently.");
    if (!self_weight)
        si_assume(is, "self_weight", "default",
                  json_get_bool(sens, "self_weight", false) ? "assessed in the sensitivity section" : "not assessed; the weight of each design is reported",
                  "The bracket's own weight is not applied: only the payload loads it.");
    si_not_evaluated(is, "dynamic loads, impact and vibration", "the load is static");

    /* quantities */
    JsonValue *qo = json_set_object(res, "quantities");
    json_set_string(qo, "primary", json_get_str(def, "primary_quantity", "load_region_displacement"));
    json_set_string(qo, "load_region_displacement",
                    "area-weighted mean displacement of the load region along the load direction (mm); lower is stiffer");
    json_set_string(qo, "stiffness", "load / work-conjugate displacement = |F|^2 / (load work) (N/mm); higher is stiffer");
    json_set_string(qo, "mass", "density x closed STL volume (kg), with the mesh volume reported beside it");

    /* refinement */
    const JsonValue *rf = json_get(def, "refinement");
    JsonValue *ro = json_set_object(res, "refinement");
    double sizes[STUDY_MAX_LEVELS];
    int nsz = 0;
    for (size_t i = 0; i < json_len(json_get(rf, "element_sizes")) && nsz < STUDY_MAX_LEVELS; i++) {
        double mm;
        if (!length_mm(json_at(json_get(rf, "element_sizes"), i), 0, &mm) || !(mm > 0)) {
            *code = NV_ERR_INVALID_UNIT;
            snprintf(err, errlen, "refinement.element_sizes[%zu] is not a positive length", i);
            json_free(res);
            return NULL;
        }
        bool dup = false;
        for (int k = 0; k < nsz; k++) dup |= fabs(sizes[k] - mm) < 1e-9;
        if (!dup) sizes[nsz++] = mm;
    }
    qsort(sizes, (size_t)nsz, sizeof(double), cmp_desc);
    if (nsz < 2) {
        *code = NV_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "refinement.element_sizes needs at least two different sizes: one mesh gives no estimate of the discretisation error");
        json_free(res);
        return NULL;
    }
    json_set(ro, "element_sizes_mm", json_numbers(sizes, (size_t)nsz));
    json_set_number(ro, "convergence_criterion", json_get_num(rf, "convergence_criterion", 0.02));
    bool equal_ratio = true;
    for (int k = 2; k < nsz; k++) equal_ratio &= fabs(sizes[k - 2] / sizes[k - 1] - sizes[k - 1] / sizes[k]) < 0.01 * sizes[k - 1] / sizes[k];
    json_set_bool(ro, "constant_ratio", equal_ratio);
    if (!equal_ratio)
        si_warn(is, "REFINEMENT_RATIO_NOT_CONSTANT", NULL,
                "the element sizes do not shrink by a constant ratio: changes between meshes are reported, but no order or extrapolation is estimated");

    /* sensitivity */
    JsonValue *so = json_set_object(res, "sensitivity");
    if (json_get(sens, "youngs_modulus_relative")) json_set(so, "youngs_modulus_relative", json_clone(json_get(sens, "youngs_modulus_relative")));
    if (json_get(sens, "poisson_ratio")) json_set(so, "poisson_ratio", json_clone(json_get(sens, "poisson_ratio")));
    if (nalt) json_set(so, "mounting_alternatives", json_clone(json_get(sens, "mounting_alternatives")));
    json_set_bool(so, "self_weight", json_get_bool(sens, "self_weight", false) && !self_weight);
    json_set_string(so, "level", json_get_str(sens, "level", "finest"));
    for (size_t i = 0; i < nalt; i++) {
        const JsonValue *alt = json_at(json_get(sens, "mounting_alternatives"), i);
        const JsonValue *regions = json_get(alt, "regions");
        for (size_t k = 0; k < json_len(regions); k++) {
            const char *dn = json_key_at(regions, k);
            bool known = false;
            for (size_t j = 0; j < json_len(designs); j++) known |= !strcmp(json_get_str(json_at(designs, j), "name", ""), dn);
            if (!known) {
                *code = NV_ERR_INVALID_PARAMS;
                snprintf(err, errlen, "sensitivity.mounting_alternatives[%zu].regions names an unknown design '%s'", i, dn);
                json_free(res);
                return NULL;
            }
        }
    }

    /* equivalence tolerances, limits, analysis settings */
    const JsonValue *eq = json_get(def, "equivalence");
    JsonValue *eo = json_set_object(res, "equivalence");
    double ptol;
    if (!length_mm(json_get(eq, "position_tolerance"), 1.0, &ptol)) ptol = 1.0;
    json_set_number(eo, "position_tolerance_mm", ptol);
    json_set_number(eo, "relative_tolerance", json_get_num(eq, "relative_tolerance", 0.02));
    json_set_number(eo, "angle_tolerance_deg", json_get_num(eq, "angle_tolerance_deg", 5));
    const JsonValue *lim = json_get(def, "limits");
    JsonValue *lmo = json_set_object(res, "limits");
    json_set_int(lmo, "max_elements", json_get_int(lim, "max_elements", 400000));
    double wall = 1800;
    const JsonValue *mw = json_get(lim, "max_wall_time");
    if (mw && !quantity_from_json(mw, DIM_TIME, "s", &wall, ierr, sizeof ierr)) wall = 1800;
    json_set_number(lmo, "max_wall_seconds", wall);
    const JsonValue *an = json_get(def, "analysis");
    JsonValue *ao = json_set_object(res, "analysis");
    json_set_string(ao, "physics", "static, small-strain linear elasticity, isotropic material");
    json_set_string(ao, "formulation", json_get_str(an, "formulation", "incompatible_modes"));
    json_set_string(ao, "solver", json_get_str(an, "solver", "auto"));
    double tref = 293.15;
    const JsonValue *tv = json_get(an, "reference_temperature");
    if (tv && !quantity_from_json(tv, DIM_TEMPERATURE, "degC", &tref, ierr, sizeof ierr)) tref = 293.15;
    json_set_number(ao, "reference_temperature_c", tref - 273.15);
    json_set_string(res, "retain_results", json_get_str(def, "retain_results", "refinement"));
    if (json_get(def, "accept")) json_set(res, "accept", json_clone(json_get(def, "accept")));
    const JsonValue *notes = json_get(def, "notes");
    for (size_t i = 0; i < json_len(notes); i++) {
        const JsonValue *n = json_at(notes, i);
        si_assume(is, json_get_str(n, "subject", "note"), json_get_str(n, "source", "user"), "recorded as stated", "%s", json_get_str(n, "text", ""));
    }

    /* what this release does not evaluate, and requests for it */
    const char *qtext = json_get_str(def, "question", "");
    const char *dtext = json_get_str(def, "decision", "");
    static const char *const STRENGTH[] = {"safe", "safety", "strength", "strong enough", "fail", "failure", "break", "yield", "fatigue", "crack", NULL};
    for (int k = 0; STRENGTH[k]; k++)
        if (contains_word(qtext, STRENGTH[k]) || contains_word(dtext, STRENGTH[k])) {
            si_unsupported(is, "strength_or_safety", false,
                           "The question asks about '%s'. This workflow compares stiffness, displacement and mass. It does not decide whether a part is "
                           "safe or strong enough: no failure criterion, fastener check or validated strength data is part of it, and linear-elastic "
                           "stress peaks at supports and corners are singular. A stress plot is not evidence of safety.",
                           STRENGTH[k]);
            break;
        }
    si_not_evaluated(is, "strength margin or safety", "no failure criterion and no validated strength data for the modelled condition; stress peaks at ideal supports and sharp corners are singular");
    si_not_evaluated(is, "fatigue", "cyclic loading is not modelled");
    si_not_evaluated(is, "fastener loads, pull-out and joint slip", "the mounting is an ideal support; fasteners and contact are not modelled");
    si_not_evaluated(is, "buckling and geometric nonlinearity", "linear static analysis; large-deformation indicators are checked after solving");
    si_not_evaluated(is, "creep and temperature effects", "properties at the reference temperature, time-independent");
    return res;
}
