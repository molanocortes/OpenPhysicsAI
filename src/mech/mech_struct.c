/* mech_struct.c - printed-material records and orthotropic structural assessment jobs (see mech_struct.h) */
#include "mech_struct.h"
#include "breakest.h"
#include "breakest_fe.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/sha256.h"
#include "../ctl/matlib.h"
#include "../fem/dense.h"
#include "../threads.h"
#include "infill.h"
#include "mechunits.h"

static const char *const SOURCES[] = {"measured", "datasheet", "literature", "calibrated", "user", "assumed", NULL};

static bool source_ok(const char *s) {
    for (int i = 0; s && SOURCES[i]; i++)
        if (!strcmp(s, SOURCES[i])) return true;
    return false;
}

static void materials_path(const Project *p, char *out, size_t cap) {
    char dir[NV_PATH_MAX];
    path_join(dir, sizeof dir, p->dir, "mechanics");
    path_join(out, cap, dir, "materials.json");
}

/* ------------------------------------------------------------------------------------------------ material records */

typedef struct Parse {
    const JsonValue *units;
    bool require_units;
    char err[400], hint[300];
    bool missing; /* a missing input rather than an invalid one */
} Parse;

static const char *unit_of(const Parse *ps, MechQty q) {
    const char *u = json_get_str(ps->units, mech_qty_name(q), NULL);
    return u;
}

static bool pq(Parse *ps, const JsonValue *o, const char *key, MechQty q, bool required, double *out, JsonValue *canon, const char *where) {
    const JsonValue *v = json_get(o, key);
    if (!v) {
        if (required) {
            snprintf(ps->err, sizeof ps->err, "%s.%s is missing", where, key);
            ps->missing = true;
        }
        return !required;
    }
    char e[200];
    if (!mech_qty_from_json_ex(v, q, unit_of(ps, q), ps->require_units, out, e, sizeof e)) {
        snprintf(ps->err, sizeof ps->err, "%s.%s: %s", where, key, e);
        return false;
    }
    if (canon) {
        if (q == MQ_DIMENSIONLESS)
            json_set_number(canon, key, *out);
        else
            json_set(canon, key, json_stringf("%.17g %s", *out, mech_qty_si_unit(q)));
    }
    return true;
}

/* parses a record (inline definition or stored canonical form); canonical receives the stored form */
static bool material_parse(const JsonValue *m, const JsonValue *units, bool require_units, MechMaterial *out, JsonValue **canonical, Parse *ps) {
    memset(out, 0, sizeof *out);
    memset(ps, 0, sizeof *ps);
    ps->units = units, ps->require_units = require_units;
    const char *id = json_get_str(m, "id", NULL), *model = json_get_str(m, "model", "orthotropic");
    if (!id || !*id || strlen(id) >= sizeof out->id) {
        snprintf(ps->err, sizeof ps->err, "material.id is missing or longer than 63 characters");
        return false;
    }
    snprintf(out->id, sizeof out->id, "%s", id);
    if (strcmp(model, "orthotropic")) {
        snprintf(ps->err, sizeof ps->err, "material model '%s' is not supported: use orthotropic (give equal constants for an isotropic part)", model);
        return false;
    }
    JsonValue *c = json_object();
    json_set_string(c, "id", id);
    json_set_string(c, "model", "orthotropic");
    const JsonValue *el = json_get(m, "elastic");
    if (!el) {
        snprintf(ps->err, sizeof ps->err, "material.elastic is missing");
        snprintf(ps->hint, sizeof ps->hint, "give E1, E2, E3, nu12, nu13, nu23, G12, G23, G13 and their source; axis 3 is the build direction");
        ps->missing = true;
        json_free(c);
        return false;
    }
    JsonValue *ce = json_set_object(c, "elastic");
    OrthoConstants *k = &out->k;
    bool ok = pq(ps, el, "E1", MQ_PRESSURE, true, &k->E[0], ce, "elastic") && pq(ps, el, "E2", MQ_PRESSURE, true, &k->E[1], ce, "elastic") &&
              pq(ps, el, "E3", MQ_PRESSURE, true, &k->E[2], ce, "elastic") && pq(ps, el, "nu12", MQ_DIMENSIONLESS, true, &k->nu12, ce, "elastic") &&
              pq(ps, el, "nu13", MQ_DIMENSIONLESS, true, &k->nu13, ce, "elastic") && pq(ps, el, "nu23", MQ_DIMENSIONLESS, true, &k->nu23, ce, "elastic") &&
              pq(ps, el, "G12", MQ_PRESSURE, true, &k->G12, ce, "elastic") && pq(ps, el, "G23", MQ_PRESSURE, true, &k->G23, ce, "elastic") &&
              pq(ps, el, "G13", MQ_PRESSURE, true, &k->G13, ce, "elastic");
    const char *src = json_get_str(el, "source", NULL);
    if (ok && !source_ok(src)) {
        snprintf(ps->err, sizeof ps->err, "elastic.source is %s", src ? "not one of measured, datasheet, literature, calibrated, user, assumed" : "missing");
        snprintf(ps->hint, sizeof ps->hint, "say where the constants come from: printed-part constants depend on the printer, material, orientation and infill");
        ps->missing = !src;
        ok = false;
    }
    if (ok) {
        char why[300];
        snprintf(out->elastic_source, sizeof out->elastic_source, "%s", src);
        json_set_string(ce, "source", src);
        if (json_get_str(el, "reference", NULL)) json_set_string(ce, "reference", json_get_str(el, "reference", ""));
        double C[6][6];
        if (!ortho_stiffness(k, C, why, sizeof why)) {
            snprintf(ps->err, sizeof ps->err, "elastic constants: %s", why);
            ok = false;
        }
    }
    if (ok && json_get(m, "density")) {
        ok = pq(ps, m, "density", MQ_DENSITY, false, &out->density, c, "material");
        if (ok && !(out->density > 0)) {
            snprintf(ps->err, sizeof ps->err, "material.density must be positive");
            ok = false;
        }
    }
    const JsonValue *sj = json_get(m, "strength");
    if (ok && sj) {
        JsonValue *cs = json_set_object(c, "strength");
        OrthoStrength *st = &out->strength;
        static const char *const K[9] = {"Xt", "Xc", "Yt", "Yc", "Zt", "Zc", "S12", "S23", "S31"};
        double *dst[9] = {&st->Xt, &st->Xc, &st->Yt, &st->Yc, &st->Zt, &st->Zc, &st->S12, &st->S23, &st->S31};
        for (int i = 0; ok && i < 9; i++) ok = pq(ps, sj, K[i], MQ_PRESSURE, true, dst[i], cs, "strength");
        st->f12 = st->f13 = st->f23 = -0.5;
        bool given_f = json_get(sj, "f12") || json_get(sj, "f13") || json_get(sj, "f23");
        if (ok && given_f)
            ok = pq(ps, sj, "f12", MQ_DIMENSIONLESS, true, &st->f12, cs, "strength") && pq(ps, sj, "f13", MQ_DIMENSIONLESS, true, &st->f13, cs, "strength") &&
                 pq(ps, sj, "f23", MQ_DIMENSIONLESS, true, &st->f23, cs, "strength");
        else if (ok) {
            json_set_number(cs, "f12", -0.5), json_set_number(cs, "f13", -0.5), json_set_number(cs, "f23", -0.5);
            json_set_string(cs, "interaction_note", "Tsai-Wu interaction coefficients not given: -1/2 (Tsai-Hahn), an assumption");
        }
        const char *ss = json_get_str(sj, "source", NULL);
        if (ok && !source_ok(ss)) {
            snprintf(ps->err, sizeof ps->err, "strength.source is %s", ss ? "not one of measured, datasheet, literature, calibrated, user, assumed" : "missing");
            snprintf(ps->hint, sizeof ps->hint, "say where the strengths come from; the interlayer tensile strength Zt depends strongly on the print settings");
            ps->missing = !ss;
            ok = false;
        }
        if (ok) {
            char why[300];
            snprintf(out->strength_source, sizeof out->strength_source, "%s", ss);
            json_set_string(cs, "source", ss);
            if (json_get_str(sj, "reference", NULL)) json_set_string(cs, "reference", json_get_str(sj, "reference", ""));
            if (!strength_valid(st, STRENGTH_TSAI_WU, why, sizeof why)) {
                snprintf(ps->err, sizeof ps->err, "strength: %s", why);
                ok = false;
            }
            out->has_strength = ok;
        }
    }
    if (ok && json_get(m, "print")) json_set(c, "print", json_clone(json_get(m, "print")));
    if (ok && json_get_str(m, "note", NULL)) json_set_string(c, "note", json_get_str(m, "note", ""));
    if (!ok) {
        json_free(c);
        return false;
    }
    if (canonical) *canonical = c;
    else json_free(c);
    return true;
}

bool mech_material_find(const Project *p, const char *id, MechMaterial *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    char path[NV_PATH_MAX];
    materials_path(p, path, sizeof path);
    JsonError je;
    JsonValue *doc = path_is_file(path) ? json_read_file(path, 16 << 20, &je) : NULL;
    const JsonValue *arr = json_get(doc, "materials");
    for (size_t i = 0; i < json_len(arr); i++) {
        if (strcmp(json_get_str(json_at(arr, i), "id", ""), id)) continue;
        Parse ps;
        JsonValue *canon = NULL;
        bool ok = material_parse(json_at(arr, i), NULL, false, out, &canon, &ps);
        if (!ok) snprintf(err, errlen, "stored material '%s' is invalid: %s", id, ps.err);
        else out->record = canon;
        json_free(doc);
        return ok;
    }
    json_free(doc);
    snprintf(err, errlen, "no mechanics material '%s' (define it with mech_material_define)", id);
    return false;
}

void mech_material_free(MechMaterial *m) {
    json_free(m->record);
    m->record = NULL;
}

void op_mech_material_define(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    MechMaterial mat;
    Parse ps;
    JsonValue *canon = NULL;
    if (!material_parse(json_get(p, "material"), json_get(p, "units"), true, &mat, &canon, &ps)) {
        op_fail(out, ps.missing ? NV_ERR_PRECONDITION : NV_ERR_INVALID_PARAMS, ps.hint[0] ? ps.hint : NULL, "%s", ps.err);
        return;
    }
    char dir[NV_PATH_MAX], path[NV_PATH_MAX], err[512];
    path_join(dir, sizeof dir, e->proj->dir, "mechanics");
    if (!engine_resolve_write_path(e, NULL, dir, true, true, dir, sizeof dir, err, sizeof err)) {
        json_free(canon);
        op_fail(out, NV_ERR_PERMISSION, "the project folder must be writable", "cannot store the material: %s", err);
        return;
    }
    path_join(path, sizeof path, dir, "materials.json");
    JsonError je;
    JsonValue *doc = path_is_file(path) ? json_read_file(path, 16 << 20, &je) : NULL;
    if (!doc) {
        doc = json_object();
        json_set_string(doc, "format", "navier-mech-materials");
        json_set_int(doc, "version", 1);
    }
    JsonValue *arr = json_get(doc, "materials");
    if (!arr) arr = json_set_array(doc, "materials");
    bool replaced = false;
    for (size_t i = 0; i < json_len(arr) && !replaced; i++)
        if (!strcmp(json_get_str(json_at(arr, i), "id", ""), mat.id)) {
            json_free(arr->u.array.items[i]);
            arr->u.array.items[i] = json_clone(canon);
            replaced = true;
        }
    if (!replaced) json_push(arr, json_clone(canon));
    if (!json_write_file(path, doc, JSON_PRETTY)) {
        json_free(doc), json_free(canon);
        op_fail(out, NV_ERR_IO, NULL, "cannot write %s", path);
        return;
    }
    json_free(doc);
    engine_touch(e, "mech_material_define", "material '%s'", mat.id);
    JsonValue *v = json_object();
    json_set(v, "material", canon);
    json_set_bool(v, "replaced", replaced);
    JsonValue *chk = json_set_object(v, "checks");
    json_set_string(chk, "elastic", "compliance positive definite");
    json_set_number(chk, "in_plane_to_build_direction_stiffness_ratio", mat.k.E[0] / mat.k.E[2]);
    if (mat.has_strength) {
        json_set_string(chk, "strength", "positive strengths; Tsai-Wu quadratic form positive semidefinite");
        json_set_number(chk, "interlayer_to_in_plane_tensile_strength_ratio", mat.strength.Zt / mat.strength.Xt);
    }
    JsonValue *w = json_set_array(v, "warnings");
    if (!strcmp(mat.elastic_source, "assumed")) json_push(w, json_string("elastic constants are assumed values: stiffness and deflection results inherit that uncertainty"));
    if (mat.has_strength && !strcmp(mat.strength_source, "assumed"))
        json_push(w, json_string("strengths are assumed values: failure indices and strength ratios are not a design margin"));
    if (!mat.has_strength) json_push(w, json_string("no strengths: assessments report stresses and deformation, not failure indices"));
    json_set_string(v, "stored_in", path);
    op_succeed(out, v);
}

/* ---- a printed-material record built from a library material (GOALS.md G17 step 2) ----
 * A library or project material that carries Young's modulus and tensile strength along and across the layers, both
 * pairs from one document (materials.json, tools/matcheck.py), becomes an orthotropic record. What no document gives
 * follows from the rules written into the record, which every result repeats in its assumptions. */
static const char *value_source(const JsonValue *rec, const char *key) {
    const JsonValue *v = json_get(rec, key);
    const char *prov = json_get_str(v, "provenance", ""), *src = json_get_str(v, "source", "");
    if (!strcmp(prov, "measured") || !strcmp(prov, "calibrated")) return prov;
    if (!strcmp(prov, "demonstration")) return "assumed";
    if (!strcmp(prov, "user_supplied")) return "user";
    return strstr(src, "data sheet") || strstr(src, "datasheet") || strstr(src, "TDS") ? "datasheet" : "literature";
}

static bool material_from_library(const Project *p, const char *id, MechMaterial *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    MaterialRecord rec;
    if (!material_lookup(p->user_materials, id, &rec, err, errlen)) return false;
    const MatTable *ea = &rec.prop[MATP_E_ALONG], *ec = &rec.prop[MATP_E_ACROSS], *sa = &rec.prop[MATP_TS_ALONG], *sc = &rec.prop[MATP_TS_ACROSS];
    if (!ea->n || !ec->n) {
        snprintf(err, errlen, "material '%s' gives no Young's modulus along and across the layers (youngs_modulus_along_layers_pa and "
                              "youngs_modulus_across_layers_pa); materials_list shows the records that do", id);
        return false;
    }
    const double T = 293.15;
    double E1 = mat_eval(ea, T), E3 = mat_eval(ec, T);
    bool nu_given = rec.prop[MATP_NU].n > 0;
    double nu = nu_given ? mat_eval(&rec.prop[MATP_NU], T) : 0.35;
    OrthoConstants *k = &out->k;
    k->E[0] = k->E[1] = E1, k->E[2] = E3;
    k->nu12 = k->nu13 = k->nu23 = nu;
    k->G12 = E1 / (2 * (1 + nu));
    k->G13 = k->G23 = E1 * E3 / (E1 + E3 + 2 * nu * E3);
    char why[300];
    double C[6][6];
    if (!ortho_stiffness(k, C, why, sizeof why)) {
        snprintf(err, errlen, "the constants built from material '%s' are not admissible: %s", id, why);
        return false;
    }
    snprintf(out->id, sizeof out->id, "%s", id);
    if (rec.prop[MATP_DENSITY].n) out->density = mat_eval(&rec.prop[MATP_DENSITY], T);
    snprintf(out->elastic_source, sizeof out->elastic_source, "%s", value_source(rec.json, "youngs_modulus_along_layers_pa"));
    JsonValue *c = json_object(), *rules = json_array();
    json_set_string(c, "id", id);
    json_set_string(c, "model", "orthotropic");
    json_set_string(c, "built_from", rec.builtin ? "library" : "project material");
    json_set_string(c, "name", rec.name);
    JsonValue *el = json_set_object(c, "elastic");
    json_set(el, "E1", json_stringf("%.17g Pa", k->E[0])), json_set(el, "E2", json_stringf("%.17g Pa", k->E[1])), json_set(el, "E3", json_stringf("%.17g Pa", k->E[2]));
    json_set_number(el, "nu12", nu), json_set_number(el, "nu13", nu), json_set_number(el, "nu23", nu);
    json_set(el, "G12", json_stringf("%.17g Pa", k->G12)), json_set(el, "G23", json_stringf("%.17g Pa", k->G23)), json_set(el, "G13", json_stringf("%.17g Pa", k->G13));
    json_set_string(el, "source", out->elastic_source);
    json_set_string(el, "E1_source", json_get_str(json_get(rec.json, "youngs_modulus_along_layers_pa"), "source", ""));
    json_set_string(el, "E3_source", json_get_str(json_get(rec.json, "youngs_modulus_across_layers_pa"), "source", ""));
    json_push(rules, json_stringf("E1 = E2 = %.4g GPa along the layers and E3 = %.4g GPa across them, from material '%s'; the layer plane is taken as "
                                  "isotropic, as for a +-45 degree raster", E1 / 1e9, E3 / 1e9, id));
    json_push(rules, nu_given ? json_stringf("Poisson's ratios nu12 = nu13 = nu23 = %.3g, the record's", nu)
                              : json_string("Poisson's ratios nu12 = nu13 = nu23 = 0.35, assumed: the record gives none (typical of thermoplastics)"));
    json_push(rules, json_string("G12 = E1 / (2 (1 + nu12)) for the isotropic layer plane; G13 = G23 = E1 E3 / (E1 + E3 + 2 nu13 E3), Saint-Venant's "
                                 "approximation, because no document gives an interlayer shear modulus"));
    if (sa->n && sc->n) {
        double Xt = mat_eval(sa, T), Zt = mat_eval(sc, T);
        OrthoStrength *st = &out->strength;
        st->Xt = st->Xc = st->Yt = st->Yc = Xt;
        st->Zt = st->Zc = Zt;
        st->S12 = Xt / sqrt(3), st->S23 = st->S31 = Zt / sqrt(3);
        st->f12 = st->f13 = st->f23 = -0.5;
        if (!strength_valid(st, STRENGTH_TSAI_WU, why, sizeof why)) {
            json_free(c), json_free(rules);
            snprintf(err, errlen, "the strengths built from material '%s' are not admissible: %s", id, why);
            return false;
        }
        out->has_strength = true;
        snprintf(out->strength_source, sizeof out->strength_source, "%s", value_source(rec.json, "tensile_strength_along_layers_pa"));
        JsonValue *cs = json_set_object(c, "strength");
        static const char *const K[9] = {"Xt", "Xc", "Yt", "Yc", "Zt", "Zc", "S12", "S23", "S31"};
        const double *v[9] = {&st->Xt, &st->Xc, &st->Yt, &st->Yc, &st->Zt, &st->Zc, &st->S12, &st->S23, &st->S31};
        for (int i = 0; i < 9; i++) json_set(cs, K[i], json_stringf("%.17g Pa", *v[i]));
        json_set_number(cs, "f12", -0.5), json_set_number(cs, "f13", -0.5), json_set_number(cs, "f23", -0.5);
        json_set_string(cs, "source", out->strength_source);
        json_set_string(cs, "Xt_source", json_get_str(json_get(rec.json, "tensile_strength_along_layers_pa"), "source", ""));
        json_set_string(cs, "Zt_source", json_get_str(json_get(rec.json, "tensile_strength_across_layers_pa"), "source", ""));
        json_push(rules, json_stringf("tensile strengths Xt = Yt = %.4g MPa along the layers and Zt = %.4g MPa across them, from material '%s'", Xt / 1e6,
                                      Zt / 1e6, id));
        json_push(rules, json_string("compressive strengths taken equal to the tensile ones: no document gives them, and polymers are generally "
                                     "stronger in compression than in tension"));
        json_push(rules, json_string("shear strengths S12 = Xt / sqrt(3) and S23 = S31 = Zt / sqrt(3), the von Mises relation: no document gives them"));
        json_push(rules, json_string("Tsai-Wu interaction coefficients -1/2 (Tsai-Hahn), an assumption"));
    }
    json_set(c, "rules", rules);
    out->record = c;
    return true;
}

/* ------------------------------------------------------------------------------------------------ assessment setup */

static bool axis_json(const JsonValue *v, const char *def, double out[3]) {
    const char *s = v && v->type == JSON_STRING ? v->u.string.ptr : (v ? NULL : def);
    if (s) {
        static const char *const N[6] = {"x", "y", "z", "-x", "-y", "-z"};
        for (int i = 0; i < 6; i++)
            if (!strcmp(s, N[i])) {
                out[0] = out[1] = out[2] = 0;
                out[i % 3] = i < 3 ? 1 : -1;
                return true;
            }
        return false;
    }
    return json_get_numbers(v, out, 3) && (out[0] != 0 || out[1] != 0 || out[2] != 0);
}

/* walls {perimeters, line_width, top_layers, bottom_layers, layer_height} and infill {density, pattern}, as the slicer
 * states them; the core follows from the infill model (infill.h) */
static bool count_of(const JsonValue *o, const char *key, int *n, char *err, size_t errlen) {
    const JsonValue *v = json_get(o, key);
    if (!v || !json_is_integer(v) || v->u.number < 0 || v->u.number > 1000) {
        snprintf(err, errlen, "walls.%s must be a whole number from 0 to 1000", key);
        return false;
    }
    *n = (int)v->u.number;
    return true;
}

static bool layout_parse(const JsonValue *mm, MechStructSetup *s, OpResult *out) {
    PrintLayout *L = &s->layout;
    const JsonValue *wj = json_get(mm, "walls"), *ij = json_get(mm, "infill");
    char err[300];
    const char *hint = "as the slicer states them: walls {perimeters, line_width, top_layers, bottom_layers, layer_height} and infill {density, "
                       "pattern}, for example {\"perimeters\": 2, \"line_width\": \"0.45 mm\", \"top_layers\": 4, \"bottom_layers\": 3, "
                       "\"layer_height\": \"0.2 mm\"} and {\"density\": \"15 %\", \"pattern\": \"grid\"}";
    if (!wj || !ij) {
        op_fail(out, NV_ERR_INVALID_PARAMS, hint, "material_model needs walls and infill together");
        return false;
    }
    int axis = 0;
    for (int k = 1; k < 3; k++)
        if (fabs(s->R[3 * k + 2]) > fabs(s->R[3 * axis + 2])) axis = k;
    if (fabs(s->R[3 * axis + 2]) < 1 - 1e-9) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "turn the part so that it is printed along x, y or z of the build frame (usually z)",
                "walls and infill need a build direction along an axis of the build frame");
        return false;
    }
    bool ok = count_of(wj, "perimeters", &L->perimeters, err, sizeof err) && count_of(wj, "top_layers", &L->top_layers, err, sizeof err) &&
              count_of(wj, "bottom_layers", &L->bottom_layers, err, sizeof err);
    char e2[200];
    if (ok && !mech_qty_from_json_ex(json_get(wj, "line_width"), MQ_LENGTH, NULL, true, &L->line_width, e2, sizeof e2))
        snprintf(err, sizeof err, "walls.line_width: %s", e2), ok = false;
    if (ok && !mech_qty_from_json_ex(json_get(wj, "layer_height"), MQ_LENGTH, NULL, true, &L->layer_height, e2, sizeof e2))
        snprintf(err, sizeof err, "walls.layer_height: %s", e2), ok = false;
    if (ok && !(L->line_width > 0 && L->line_width < 0.05 && L->layer_height > 0 && L->layer_height < 0.05))
        snprintf(err, sizeof err, "walls.line_width and walls.layer_height must be positive lengths below 50 mm"), ok = false;
    const JsonValue *dj = json_get(ij, "density");
    if (ok && dj && dj->type == JSON_NUMBER) L->density = dj->u.number;
    else if (ok && dj && dj->type == JSON_STRING) {
        char *end;
        L->density = strtod(dj->u.string.ptr, &end) / 100;
        while (*end == ' ') end++;
        if (strcmp(end, "%")) L->density = NAN;
    } else if (ok)
        L->density = NAN;
    if (ok && !(L->density > 0 && L->density <= 1))
        snprintf(err, sizeof err, "infill.density must be a fraction in (0, 1] or a percentage such as \"15 %%\""), ok = false;
    const char *pat = json_get_str(ij, "pattern", NULL);
    int pi = pat ? infill_pattern_from_name(pat) : -1;
    if (ok && pi < 0) snprintf(err, sizeof err, "infill.pattern '%s' is not modelled: %s", pat ? pat : "(missing)", infill_pattern_list()), ok = false;
    if (ok) {
        snprintf(L->pattern, sizeof L->pattern, "%s", infill_pattern_name(pi));
        L->rules = json_array();
        ok = infill_core(pi, L->density, &s->mat.k, &s->mat.strength, s->mat.has_strength, &L->core, &L->core_strength, L->rules, err, sizeof err);
    }
    if (!ok) {
        json_free(L->rules);
        L->rules = NULL;
        op_fail(out, NV_ERR_INVALID_PARAMS, hint, "%s", err);
        return false;
    }
    L->on = true;
    return true;
}

bool mech_struct_setup(const Project *p, const JsonValue *mm, MechStructSetup *s, OpResult *out) {
    memset(s, 0, sizeof *s);
    char err[400];
    const char *id = json_get_str(mm, "material", NULL), *lib = json_get_str(mm, "library", NULL);
    if (!id == !lib) {
        op_fail(out, NV_ERR_INVALID_PARAMS,
                "material: the id of a mech_material_define record; library: a library or project material with values along and across the layers",
                id ? "give material_model.material or material_model.library, not both" : "material_model.material or material_model.library is missing");
        return false;
    }
    if (lib) {
        if (!material_from_library(p, lib, &s->mat, err, sizeof err)) {
            op_fail(out, strstr(err, "no material with id") ? NV_ERR_NOT_FOUND : NV_ERR_INVALID_PARAMS, NULL, "%s", err);
            return false;
        }
        id = lib;
    } else if (!mech_material_find(p, id, &s->mat, err, sizeof err)) {
        op_fail(out, NV_ERR_NOT_FOUND, "define the material with mech_material_define", "%s", err);
        return false;
    }
    double b[3], xr[3], ang = 0;
    if (!axis_json(json_get(mm, "build_direction"), "z", b) || !axis_json(json_get(mm, "raster_reference"), "x", xr)) {
        mech_material_free(&s->mat);
        op_fail(out, NV_ERR_INVALID_PARAMS, "axes are x, y, z, -x, -y, -z or [x, y, z] in the build frame of the placed part",
                "material_model.build_direction or raster_reference is not a direction");
        return false;
    }
    if (json_get(mm, "raster_angle")) {
        char e2[200];
        if (!mech_qty_from_json_ex(json_get(mm, "raster_angle"), MQ_ANGLE, NULL, true, &ang, e2, sizeof e2)) {
            mech_material_free(&s->mat);
            op_fail(out, NV_ERR_INVALID_PARAMS, "for example \"45 deg\"", "material_model.raster_angle: %s", e2);
            return false;
        }
    }
    if (!ortho_print_axes(b, xr, ang, s->R)) {
        mech_material_free(&s->mat);
        op_fail(out, NV_ERR_INVALID_PARAMS, "choose a raster reference that is not parallel to the build direction", "the raster reference lies along the build direction");
        return false;
    }
    const char *crit = json_get_str(mm, "criterion", "tsai_wu");
    int ci = strength_criterion_from_name(crit);
    if (ci < 0) {
        mech_material_free(&s->mat);
        op_fail(out, NV_ERR_INVALID_PARAMS, "criterion is max_stress, tsai_wu or tsai_hill", "unknown strength criterion '%s'", crit);
        return false;
    }
    s->criterion = (StrengthCriterion)ci;
    memcpy(s->raster_ref, xr, sizeof s->raster_ref);
    s->raster_angle = ang;
    s->compare_orientations = json_get_bool(mm, "compare_orientations", false);
    if ((json_get(mm, "walls") || json_get(mm, "infill")) && !layout_parse(mm, s, out)) {
        mech_material_free(&s->mat);
        return false;
    }
    s->description = json_object();
    json_set_string(s->description, "type", "orthotropic");
    json_set_string(s->description, lib ? "library" : "material", id);
    json_set(s->description, "record", json_clone(s->mat.record));
    JsonValue *ax = json_set_object(s->description, "material_axes_build_frame");
    json_set(ax, "axis1", json_vec3(s->R[0], s->R[3], s->R[6]));
    json_set(ax, "axis2", json_vec3(s->R[1], s->R[4], s->R[7]));
    json_set(ax, "axis3_build_direction", json_vec3(s->R[2], s->R[5], s->R[8]));
    json_set_number(ax, "raster_angle_deg", ang * 180 / M_PI);
    json_set_string(s->description, "criterion", s->mat.has_strength ? strength_criterion_name(s->criterion) : "none (no strengths in the record)");
    if (s->compare_orientations) json_set_bool(s->description, "compare_orientations", true);
    if (s->layout.on) {
        json_set(s->description, "walls", json_clone(json_get(mm, "walls")));
        json_set(s->description, "infill", json_clone(json_get(mm, "infill")));
        json_set(s->description, "infill_rules", json_clone(s->layout.rules));
    }
    return true;
}

void mech_struct_setup_free(MechStructSetup *s) {
    mech_material_free(&s->mat);
    json_free(s->layout.rules);
    s->layout.rules = NULL;
    json_free(s->description);
    s->description = NULL;
    json_free(s->inputs);
    s->inputs = NULL;
}

void mech_struct_assumptions(JsonValue *a, const MechStructSetup *s) {
    const JsonValue *rules = json_get(s->mat.record, "rules"); /* a record built from a library material: its rules, in every result */
    for (size_t i = 0; i < json_len(rules); i++) json_push(a, json_clone(json_at(rules, i)));
    if (s->layout.on) {
        for (size_t i = 0; i < json_len(s->layout.rules); i++) json_push(a, json_clone(json_at(s->layout.rules, i)));
        json_push(a, json_string("walls measured along the grid axes from the part's boundary: exact for faces along them, thinner than printed on "
                                 "slanted faces; the walls carry the part's printed material and axes"));
        json_push(a, json_string("an element cut by the wall boundary mixes wall and core at equal strain (Voigt: exact for stress along the wall, an "
                                 "upper bound across it); wall and core are each checked against their own strengths at that strain"));
    }
    json_push(a, json_stringf("orthotropic linear elastic material '%s' (elastic constants: %s) with axis 3 along the build direction: a homogenised printed "
                              "solid, without layer-scale stress concentrations, voids or local bond defects",
                              s->mat.id, s->mat.elastic_source));
    if (s->mat.has_strength)
        json_push(a, json_stringf("strength criterion %s with strengths from %s; a failure index is a comparison with those data, not a fatigue or "
                                  "damage prediction",
                                  strength_criterion_name(s->criterion), s->mat.strength_source));
}

/* ------------------------------------------------------------------------------------------------ job */

typedef struct Prog {
    Job *job;
} Prog;

static bool on_progress(void *ctx, int it, double value) { /* same reporting as the shared structural job */
    Prog *pr = ctx;
    if (job_cancel_requested(pr->job)) return false;
    char stage[160];
    if (it % 10 == 0) {
        double f = value > 0 && value < 1 ? log(value) / log(1e-10) : 0;
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        snprintf(stage, sizeof stage, "iterative solve: iteration %d, relative residual %.2e", it, value);
        job_progress(pr->job, 0.10 + 0.80 * f, stage);
    } else {
        snprintf(stage, sizeof stage, "direct solve: factorisation %.0f%%", 100 * value);
        job_progress(pr->job, 0.10 + 0.80 * value, stage);
    }
    return true;
}

/* the build-frame position of Gauss point g of element e: 8 points in a voxel, 4 in a TET10 */
static void gp_position(const StaticModel *m, int e, int g, double x[3]) {
    x[0] = x[1] = x[2] = 0;
    if (m->elem_type != SOLID_ELEM_HEX8) {
        double L[TET_MAX_GP][4], w[TET_MAX_GP], N[TET_MAX_NODES], X[TET_MAX_NODES][3];
        tet_gauss_points(m->elem_type, L, w);
        tet_shape(m->elem_type, L[g], N, NULL);
        sa_elem_coords(m, e, X);
        for (int a = 0; a < sa_npe(m); a++)
            for (int k = 0; k < 3; k++) x[k] += N[a] * X[a][k];
        return;
    }
    double N[8], dN[8][3];
    hex8_shape(HEX8_XI[g][0] / sqrt(3), HEX8_XI[g][1] / sqrt(3), HEX8_XI[g][2] / sqrt(3), N, dN);
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) x[k] += N[a] * m->xyz[3 * (size_t)m->conn[8 * (size_t)e + a] + (size_t)k];
}

static bool wr(FILE *f, const void *p, size_t size, size_t n) { return n == 0 || fwrite(p, size, n, f) == n; }

/* ---- where a linear model has no finite stress (GOALS.md G17 step 4) ------------------------------------------
 * A perfectly sharp inside corner and the edge of a rigid support are singular: there the largest stress grows each
 * time the mesh is refined (the sample bracket's strength ratio fell by a quarter per halving of the element size). A
 * peak within a zone of max(1 mm, 1.5 element sizes) of either is flagged and not stated. The supported nodes are kept
 * in a grid of cells at least one zone wide, so that a point needs to look into 27 cells only. */
typedef struct Zones {
    double zone, cell, lo[3];
    int dim[3], *start, *node;
    const double *xyz;
} Zones;

static void zones_free(Zones *z) { free(z->start), free(z->node), z->start = z->node = NULL; }

/* supports that can concentrate stress: a clamp (every component held) or a prescribed displacement; a frictionless
 * support holds only the normal of a flat face, which is how a symmetry plane is modelled, and is left out */
static bool singular_support(const StaticModel *m, int i) {
    size_t q = 3 * (size_t)i;
    int held = (m->fixed[q] != 0) + (m->fixed[q + 1] != 0) + (m->fixed[q + 2] != 0);
    if (held == 0) return false;
    if (held == 3) return true;
    int k = m->node_bc ? m->node_bc[i] : -1;
    return k >= 0 && k < m->nbc && m->bc[k].kind != BC_FRICTIONLESS;
}

static bool zones_init(Zones *z, const StaticModel *m, double zone) {
    memset(z, 0, sizeof *z);
    z->zone = zone, z->xyz = m->xyz;
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    int ns = 0;
    for (int i = 0; i < m->nnodes; i++) {
        size_t q = 3 * (size_t)i;
        if (!singular_support(m, i)) continue;
        ns++;
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], m->xyz[q + k]), hi[k] = fmax(hi[k], m->xyz[q + k]);
    }
    if (!ns) return true; /* no supported node: nothing to look up */
    double ext = fmax(hi[0] - lo[0], fmax(hi[1] - lo[1], hi[2] - lo[2]));
    z->cell = fmax(zone, ext / 128); /* at most 129 cells a side */
    size_t cells = 1;
    for (int k = 0; k < 3; k++) {
        z->lo[k] = lo[k];
        z->dim[k] = (int)floor((hi[k] - lo[k]) / z->cell) + 1;
        cells *= (size_t)z->dim[k];
    }
    z->start = calloc(cells + 1, sizeof(int));
    z->node = malloc((size_t)ns * sizeof(int));
    if (!z->start || !z->node) {
        zones_free(z);
        return false;
    }
    for (int pass = 0; pass < 2; pass++) { /* count, then place */
        for (int i = 0; i < m->nnodes; i++) {
            size_t q = 3 * (size_t)i;
            if (!singular_support(m, i)) continue;
            size_t c = 0;
            for (int k = 2; k >= 0; k--) {
                int ck = (int)floor((m->xyz[q + (size_t)k] - z->lo[k]) / z->cell);
                ck = ck < 0 ? 0 : (ck >= z->dim[k] ? z->dim[k] - 1 : ck);
                c = c * (size_t)z->dim[k] + (size_t)ck;
            }
            if (pass == 0) z->start[c + 1]++;
            else z->node[z->start[c]++] = i;
        }
        if (pass == 0)
            for (size_t c = 0; c < cells; c++) z->start[c + 1] += z->start[c];
    }
    for (size_t c = cells; c > 0; c--) z->start[c] = z->start[c - 1]; /* the placing pass moved every start one cell on */
    z->start[0] = 0;
    return true;
}

/* the distance to the nearest supported node when it is within one cell, else INFINITY */
static double zones_support_distance(const Zones *z, const double p[3]) {
    if (!z->start) return INFINITY;
    int c[3];
    for (int k = 0; k < 3; k++) c[k] = (int)floor((p[k] - z->lo[k]) / z->cell);
    double best = INFINITY;
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int q[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
                if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= z->dim[0] || q[1] >= z->dim[1] || q[2] >= z->dim[2]) continue;
                size_t cell = ((size_t)q[2] * (size_t)z->dim[1] + (size_t)q[1]) * (size_t)z->dim[0] + (size_t)q[0];
                for (int j = z->start[cell]; j < z->start[cell + 1]; j++) {
                    const double *x = z->xyz + 3 * (size_t)z->node[j];
                    double d = sqrt((x[0] - p[0]) * (x[0] - p[0]) + (x[1] - p[1]) * (x[1] - p[1]) + (x[2] - p[2]) * (x[2] - p[2]));
                    best = fmin(best, d);
                }
            }
    return best;
}

typedef struct GpRank {
    double index;
    int i;
} GpRank;

static int rank_desc(const void *a, const void *b) {
    double x = ((const GpRank *)a)->index, y = ((const GpRank *)b)->index;
    return (y > x) - (y < x);
}

/* The strength ratio step 4 states, read at the surface: at every node, from the stress the elements extrapolate there and
 * average (Gauss points lie inside, 3.6 to 9 % under the surface stress of a TET10 beam in bending at 2 mm cells), the
 * smallest outside the zones, beside the flag of the peak when it lies inside one. */
typedef struct Stated {
    bool found, has_sm, core;    /* has_sm: sm holds the stress there; core: walls and infill, the point lies in the infill */
    double ratio, index, x[3], sm[6];
    int mode, node, elem;        /* node (read at the nodes) or elem (at the Gauss points), the other -1 */
    const char *flag;            /* of the peak: none, sharp_inside_corner or support */
    double peak_ratio, peak_x[3], peak_dc, peak_ds;
} Stated;

static void stated_compute(const StaticModel *m, const MechStructSetup *s, const double Rax[9], const double *node_stress, const Zones *z,
                           double zone, Stated *st) {
    memset(st, 0, sizeof *st);
    st->flag = "none", st->peak_dc = st->peak_ds = INFINITY, st->node = st->elem = -1;
    int nn = m->nnodes;
    GpRank *rank = node_stress ? malloc((size_t)(nn > 0 ? nn : 1) * sizeof *rank) : NULL;
    if (!rank) {
        st->flag = "not evaluated";
        return;
    }
    size_t n = 0;
    for (int i = 0; i < nn; i++) {
        double sm[6];
        voigt_stress_to_material(Rax, node_stress + 6 * (size_t)i, sm);
        StrengthResult sr;
        strength_evaluate(s->criterion, &s->mat.strength, sm, &sr);
        if (sr.index > 0 && isfinite(sr.index)) rank[n].index = sr.index, rank[n].i = i, n++;
    }
    qsort(rank, n, sizeof *rank, rank_desc);
    for (size_t k = 0; k < n && !st->found; k++) {
        const double *x = m->xyz + 3 * (size_t)rank[k].i;
        double dc = static_distance_to_reentrant_edge(m, x), ds = zones_support_distance(z, x);
        if (k == 0) { /* the peak itself */
            st->flag = dc < zone ? "sharp_inside_corner" : (ds < zone ? "support" : "none");
            st->peak_ratio = 1 / rank[0].index, st->peak_dc = dc, st->peak_ds = ds;
            memcpy(st->peak_x, x, sizeof st->peak_x);
        }
        if (dc < zone || ds < zone) continue;
        StrengthResult sr;
        voigt_stress_to_material(Rax, node_stress + 6 * (size_t)rank[k].i, st->sm);
        strength_evaluate(s->criterion, &s->mat.strength, st->sm, &sr);
        st->found = st->has_sm = true, st->ratio = sr.ratio, st->index = sr.index, st->mode = sr.mode, st->node = rank[k].i;
        memcpy(st->x, x, sizeof st->x);
    }
    free(rank);
}

/* With walls and infill (GOALS.md G17 step 3) wall and core share the strain at a Gauss point and each is checked against
 * its own strengths, which nodal averages would blur: the stated ratio is then read at the Gauss points (gp_mode carries
 * a core point's mode plus 32), outside the same zones. */
static void stated_from_gp(const StaticModel *m, const double *gp_index, const unsigned char *gp_mode, const Zones *z, double zone, Stated *st) {
    memset(st, 0, sizeof *st);
    st->flag = "none", st->peak_dc = st->peak_ds = INFINITY, st->node = st->elem = -1;
    int ngp = sa_ngp(m);
    size_t npts = (size_t)ngp * (size_t)m->nelems, n = 0;
    GpRank *rank = malloc((npts ? npts : 1) * sizeof *rank);
    if (!rank) {
        st->flag = "not evaluated";
        return;
    }
    for (size_t i = 0; i < npts; i++)
        if (gp_index[i] > 0 && isfinite(gp_index[i])) rank[n].index = gp_index[i], rank[n].i = (int)i, n++;
    qsort(rank, n, sizeof *rank, rank_desc);
    for (size_t k = 0; k < n && !st->found; k++) {
        double x[3];
        gp_position(m, rank[k].i / ngp, rank[k].i % ngp, x);
        double dc = static_distance_to_reentrant_edge(m, x), ds = zones_support_distance(z, x);
        if (k == 0) {
            st->flag = dc < zone ? "sharp_inside_corner" : (ds < zone ? "support" : "none");
            st->peak_ratio = 1 / rank[0].index, st->peak_dc = dc, st->peak_ds = ds;
            memcpy(st->peak_x, x, sizeof st->peak_x);
        }
        if (dc < zone || ds < zone) continue;
        int md = gp_mode[rank[k].i];
        st->found = true, st->index = rank[k].index, st->ratio = 1 / rank[k].index, st->elem = rank[k].i / ngp;
        st->core = md != 255 && md >= 32, st->mode = md == 255 ? -1 : md % 32;
        memcpy(st->x, x, sizeof st->x);
    }
    free(rank);
}

static double zone_of(const StaticModel *m) { return fmax(1e-3, 1.5 * fmax(m->h[0], fmax(m->h[1], m->h[2]))); }

static void stated_json(const Stated *st, double zone, bool at_gauss_points, JsonValue *S) {
    char rule[800];
    snprintf(rule, sizeof rule,
             "the stated strength ratio is the smallest at the part's nodes (the surface stress extrapolated from the Gauss points and "
             "averaged) outside %.3g mm of its sharp inside corners (STL edges folding inward by more than 30 degrees) and of the nodes a "
             "fixed support or a prescribed displacement holds (a frictionless support, how a symmetry plane is modelled, is no singularity); "
             "a peak inside those zones is flagged and not stated, because a perfectly sharp corner or a rigid support has no finite stress "
             "in a linear model, and the real part's radius or fixture decides",
             1e3 * zone);
    if (at_gauss_points) /* walls and infill */
        snprintf(rule + strlen(rule), sizeof rule - strlen(rule), "; with walls and infill it is read at the Gauss points, where each is checked "
                                                                  "against its own strengths");
    json_set_string(S, "stated_rule", rule);
    json_set_number(S, "singularity_zone_mm", 1e3 * zone);
    json_set_string(S, "peak_flag", st->flag);
    if (st->peak_ratio > 0) {
        json_set_number(S, "peak_surface_strength_ratio", st->peak_ratio);
        json_set(S, "peak_surface_location_mm", json_vec3(st->peak_x[0] * 1e3, st->peak_x[1] * 1e3, st->peak_x[2] * 1e3));
        if (isfinite(st->peak_dc)) json_set_number(S, "peak_distance_to_inside_corner_mm", 1e3 * st->peak_dc);
        if (st->peak_ds < zone) json_set_number(S, "peak_distance_to_support_mm", 1e3 * st->peak_ds);
    }
    if (st->found) {
        json_set_number(S, "stated_strength_ratio", st->ratio);
        json_set_number(S, "stated_failure_index", st->index);
        json_set_string(S, "stated_governing_mode", strength_mode_name(st->mode));
        if (st->node >= 0) json_set_int(S, "stated_node", st->node);
        if (st->elem >= 0) json_set_int(S, "stated_element", st->elem), json_set_string(S, "stated_part", st->core ? "infill" : "walls");
        json_set(S, "stated_location_mm", json_vec3(st->x[0] * 1e3, st->x[1] * 1e3, st->x[2] * 1e3));
        if (st->has_sm)
            json_set(S, "stated_material_axes_stress_mpa",
                     json_numbers((double[6]){st->sm[0] / 1e6, st->sm[1] / 1e6, st->sm[2] / 1e6, st->sm[3] / 1e6, st->sm[4] / 1e6, st->sm[5] / 1e6}, 6));
    } else
        json_set_string(S, "stated_strength_ratio_note", "every loaded node lies within a zone of a sharp inside corner or a support: no ratio is stated");
}

double *mech_wall_fractions(const StaticModel *m, int axis, int sign, double t_side, double t_top, double t_bottom, char *err, size_t errlen) {
    if (m->elem_type != SOLID_ELEM_HEX8 || !(m->h[0] > 0 && m->h[1] > 0 && m->h[2] > 0) || axis < 0 || axis > 2) {
        snprintf(err, errlen, "walls and infill need the voxel (hex8) mesh");
        return NULL;
    }
    size_t ne = (size_t)m->nelems;
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int n = 0; n < m->nnodes; n++)
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], m->xyz[3 * (size_t)n + k]), hi[k] = fmax(hi[k], m->xyz[3 * (size_t)n + k]);
    size_t dims[3], ncell = 1;
    for (int k = 0; k < 3; k++) {
        dims[k] = (size_t)llround((hi[k] - lo[k]) / m->h[k]);
        if (dims[k] < 1 || dims[k] > 100000) {
            snprintf(err, errlen, "the voxel grid has an unexpected size along axis %d", k);
            return NULL;
        }
        ncell *= dims[k];
    }
    if (ncell > ((size_t)1 << 26)) { /* 64 million cells, 256 MB: beyond that the mesh itself is out of reach here */
        snprintf(err, errlen, "the voxel grid of the part is too large for the wall layout (%zu cells)", ncell);
        return NULL;
    }
    int *cell = malloc(ncell * sizeof *cell), *kd = malloc(6 * ne * sizeof *kd); /* kd: cells to the first empty one, -x +x -y +y -z +z */
    size_t *at = malloc(ne * sizeof *at);
    double *f = malloc((ne ? ne : 1) * sizeof *f);
    if (!cell || !kd || !at || !f) {
        free(cell), free(kd), free(at), free(f);
        snprintf(err, errlen, "out of memory for the wall layout");
        return NULL;
    }
    for (size_t c = 0; c < ncell; c++) cell[c] = -1;
    for (size_t e = 0; e < ne; e++) {
        double c[3] = {0, 0, 0};
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) c[k] += m->xyz[3 * (size_t)m->conn[8 * e + (size_t)a] + (size_t)k] / 8;
        size_t ijk[3];
        for (int k = 0; k < 3; k++) {
            double q = floor((c[k] - lo[k]) / m->h[k]);
            ijk[k] = q < 0 ? 0 : (size_t)q >= dims[k] ? dims[k] - 1 : (size_t)q;
        }
        at[e] = ijk[0] + dims[0] * (ijk[1] + dims[1] * ijk[2]);
        cell[at[e]] = (int)e;
    }
    /* along every grid line, each run of occupied cells: the distance in cells from each cell to the run's two ends */
    size_t stride[3] = {1, dims[0], dims[0] * dims[1]};
    for (int a = 0; a < 3; a++) {
        int b = (a + 1) % 3, c2 = (a + 2) % 3;
        for (size_t jb = 0; jb < dims[b]; jb++)
            for (size_t jc = 0; jc < dims[c2]; jc++) {
                size_t base = jb * stride[b] + jc * stride[c2];
                size_t i = 0;
                while (i < dims[a]) {
                    if (cell[base + i * stride[a]] < 0) {
                        i++;
                        continue;
                    }
                    size_t i0 = i;
                    while (i < dims[a] && cell[base + i * stride[a]] >= 0) i++;
                    for (size_t q = i0; q < i; q++) {
                        int e = cell[base + q * stride[a]];
                        kd[6 * (size_t)e + 2 * a] = (int)(q - i0 + 1), kd[6 * (size_t)e + 2 * a + 1] = (int)(i - q);
                    }
                }
            }
    }
    for (size_t e = 0; e < ne; e++) {
        double keep = 1; /* the share of the element in no wall */
        for (int a = 0; a < 3; a++) {
            double tm = a == axis ? (sign > 0 ? t_bottom : t_top) : t_side, tp = a == axis ? (sign > 0 ? t_top : t_bottom) : t_side;
            double fm = (tm - (kd[6 * e + 2 * (size_t)a] - 1) * m->h[a]) / m->h[a], fp = (tp - (kd[6 * e + 2 * (size_t)a + 1] - 1) * m->h[a]) / m->h[a];
            fm = fm < 0 ? 0 : fm > 1 ? 1 : fm, fp = fp < 0 ? 0 : fp > 1 ? 1 : fp;
            double fa = fm + fp > 1 ? 1 : fm + fp;
            keep *= 1 - fa;
        }
        f[e] = 1 - keep;
    }
    free(cell), free(kd), free(at);
    return f;
}

/* ---- the printed part in a static analysis (analysis_run with material_model "orthotropic", GOALS.md G17) ----
 * The project's own supports and loads (the static model built by static_model_build), the part solved as a printed
 * orthotropic solid with the static settings' element formulation and solver, the strength criterion evaluated at
 * every Gauss point. Gravity becomes nodal forces: on a voxel element (a box) the consistent share of a constant body
 * force is one eighth of the element's weight at each node, exactly. */
static void printed_free(void *p) {
    MechStructSetup *s = p;
    if (!s) return;
    mech_struct_setup_free(s);
    free(s);
}

/* engineering constants of an orthotropic stiffness (material axes, engineering shear): the inverse of its normal block
 * gives E and the major Poisson ratios, its shear diagonal the shear moduli */
static void ortho_from_C(const double C[6][6], OrthoConstants *k) {
    double a = C[0][0], b = C[0][1], c = C[0][2], d = C[1][1], e = C[1][2], f = C[2][2];
    double det = a * (d * f - e * e) - b * (b * f - c * e) + c * (b * e - c * d);
    double S11 = (d * f - e * e) / det, S22 = (a * f - c * c) / det, S33 = (a * d - b * b) / det;
    double S12 = -(b * f - c * e) / det, S13 = (b * e - c * d) / det, S23 = -(a * e - b * c) / det;
    k->E[0] = 1 / S11, k->E[1] = 1 / S22, k->E[2] = 1 / S33;
    k->nu12 = -S12 / S11, k->nu13 = -S13 / S11, k->nu23 = -S23 / S22;
    k->G12 = C[3][3], k->G23 = C[4][4], k->G13 = C[5][5];
}

/* walls and infill: a material per distinct wall fraction, each the stiffness of wall and core mixed at equal strain
 * (Voigt: exact for stress along a wall, an upper bound across it); emat receives each element's material */
static OrthoConstants *layout_materials(const MechStructSetup *s, const double *wf, size_t ne, int *emat, double **fval, int *nmat, char *err, size_t errlen) {
    double Cw[6][6], Cc[6][6];
    char why[200];
    if (!ortho_stiffness(&s->mat.k, Cw, why, sizeof why) || !ortho_stiffness(&s->layout.core, Cc, why, sizeof why)) {
        snprintf(err, errlen, "walls and infill: %s", why);
        return NULL;
    }
    int cap = 64, n = 0;
    OrthoConstants *mats = malloc((size_t)cap * sizeof *mats);
    double *fv = malloc((size_t)cap * sizeof *fv);
    for (size_t e = 0; mats && fv && e < ne; e++) {
        double f = round(wf[e] * 4096) / 4096; /* distinct fractions; 1/4096 of an element is far below the mesh's own error */
        int i = 0;
        while (i < n && fv[i] != f) i++;
        if (i == n) {
            if (n == cap) {
                cap *= 2;
                OrthoConstants *m2 = realloc(mats, (size_t)cap * sizeof *mats);
                double *f2 = realloc(fv, (size_t)cap * sizeof *fv);
                if (m2) mats = m2;
                if (f2) fv = f2;
                if (!m2 || !f2) break;
            }
            double C[6][6] = {{0}};
            for (int p = 0; p < 6; p++)
                for (int q = 0; q < 6; q++) C[p][q] = f * Cw[p][q] + (1 - f) * Cc[p][q];
            if (f == 1) mats[n] = s->mat.k; /* exactly the wall material, and exactly the core at f = 0 */
            else if (f == 0) mats[n] = s->layout.core;
            else ortho_from_C(C, &mats[n]);
            fv[n++] = f;
        }
        emat[e] = i;
    }
    if (!mats || !fv) {
        free(mats), free(fv);
        snprintf(err, errlen, "out of memory for the wall and infill materials");
        return NULL;
    }
    *fval = fv, *nmat = n;
    return mats;
}

static void solve_failure_code(const char *err, char *code, size_t codelen) {
    snprintf(code, codelen, "%s", strstr(err, "out of memory") ? "RESOURCE_LIMIT"
                                  : (strstr(err, "rigid") || strstr(err, "singular") ? "INSUFFICIENT_CONSTRAINTS" : "SOLVER_FAILED"));
}

/* a TET10 mesh follows the part's surface: the tetrahedral solver with the printed solid's matrix for material axes Rax */
static bool printed_solve_tet(const StaticModel *m, const MechStructSetup *s, const double Rax[9], SolidResult *res, char *code, size_t codelen,
                              char *err, size_t errlen) {
    if (s->layout.on) {
        snprintf(code, codelen, "INVALID_PARAMS");
        snprintf(err, errlen, "walls and infill are laid out on the voxel mesh: mesh the part with voxels (mesh_generate without the tetrahedral method)");
        return false;
    }
    double C[6][6], D[6][6];
    if (!ortho_stiffness(&s->mat.k, C, err, errlen)) {
        snprintf(code, codelen, "INVALID_PARAMS");
        return false;
    }
    ortho_global_D(C, Rax, D);
    int nmat = m->nmat > 0 ? m->nmat : 1;
    double *matD = malloc(36 * (size_t)nmat * sizeof *matD);
    SolidMaterial *mats = malloc((size_t)nmat * sizeof *mats);
    if (!matD || !mats) {
        free(matD), free(mats);
        snprintf(code, codelen, "RESOURCE_LIMIT");
        snprintf(err, errlen, "out of memory for the printed materials");
        return false;
    }
    for (int i = 0; i < nmat; i++) { /* every element is the printed solid, as on the voxel mesh */
        memcpy(matD + 36 * (size_t)i, D, sizeof D);
        mats[i] = m->nmat > 0 ? m->mat[i] : (SolidMaterial){s->mat.k.E[0], s->mat.k.nu12, s->mat.density};
        if (s->mat.density > 0) mats[i].density = s->mat.density;
    }
    HexModel hm = {m->nnodes, m->nelems, m->xyz, m->conn, m->elem_mat, nmat, mats, m->settings.formulation, NULL, .mat_D = matD,
                   .elem_type = m->elem_type};
    SolidLoads L = {m->fixed, m->fixed_value, m->nodal_force, NULL, {m->gravity[0], m->gravity[1], m->gravity[2]}};
    int threads = cpu_perf_count();
    ThreadPool *pool = pool_create(threads > 0 ? threads : 1);
    SolidOptions opt = {m->settings.solver, m->settings.pcg_tol > 0 ? m->settings.pcg_tol : 1e-10, 0, 0, pool, NULL, NULL};
    bool ok = solid_solve(&hm, &L, &opt, res, err, errlen);
    if (pool) pool_destroy(pool);
    free(matD), free(mats);
    if (!ok) solve_failure_code(err, code, codelen);
    return ok;
}

/* The voxel solve for material axes Rax. Walls and infill are laid out for that build direction (the grid axis nearest
 * axis 3): *wf_out receives each element's wall fraction, NULL for a solid part, freed by the caller. Gravity becomes
 * nodal forces, one eighth of a voxel's weight at each node, the infill holding its solid fraction of the plastic. */
static bool printed_solve_hex(const StaticModel *m, const MechStructSetup *s, const double Rax[9], OrthoResult *o, double **wf_out, char *code,
                              size_t codelen, char *err, size_t errlen) {
    size_t nn = (size_t)m->nnodes, ne = (size_t)m->nelems;
    *wf_out = NULL;
    double *force = malloc(3 * (nn ? nn : 1) * sizeof *force);
    if (!force) {
        snprintf(code, codelen, "RESOURCE_LIMIT");
        snprintf(err, errlen, "out of memory for the nodal forces");
        return false;
    }
    memcpy(force, m->nodal_force, 3 * nn * sizeof *force);
    double *wf = NULL, *fval = NULL;
    int *emat = NULL, nmat = 1;
    OrthoConstants *mats = NULL;
    const PrintLayout *L = &s->layout;
    if (L->on) {
        int axis = 0;
        for (int k = 1; k < 3; k++)
            if (fabs(Rax[3 * k + 2]) > fabs(Rax[3 * axis + 2])) axis = k;
        wf = mech_wall_fractions(m, axis, Rax[3 * axis + 2] > 0 ? 1 : -1, L->perimeters * L->line_width, L->top_layers * L->layer_height,
                                 L->bottom_layers * L->layer_height, err, errlen);
        emat = wf ? malloc((ne ? ne : 1) * sizeof *emat) : NULL;
        mats = emat ? layout_materials(s, wf, ne, emat, &fval, &nmat, err, errlen) : NULL;
        if (!mats) {
            if (wf && !emat) snprintf(err, errlen, "out of memory for the wall layout");
            free(wf), free(emat), free(force);
            snprintf(code, codelen, "%s", strstr(err, "out of memory") ? "RESOURCE_LIMIT" : "INVALID_PARAMS");
            return false;
        }
    }
    bool gravity = m->gravity[0] != 0 || m->gravity[1] != 0 || m->gravity[2] != 0;
    for (size_t e = 0; gravity && e < ne; e++) {
        const int *en = &m->conn[8 * e];
        double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], m->xyz[3 * (size_t)en[a] + k]), hi[k] = fmax(hi[k], m->xyz[3 * (size_t)en[a] + k]);
        int mi = m->elem_mat ? m->elem_mat[e] : 0;
        double rho = s->mat.density > 0 ? s->mat.density : m->mat[mi].density;
        if (wf) rho *= wf[e] + (1 - wf[e]) * L->density; /* the infill holds its solid fraction of the plastic */
        double w = rho * (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]) / 8;
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) force[3 * (size_t)en[a] + k] += w * m->gravity[k];
    }
    int threads = cpu_perf_count();
    ThreadPool *pool = pool_create(threads > 0 ? threads : 1);
    OrthoModel om = {m->nnodes, m->nelems, m->xyz, m->conn, nmat, mats ? mats : &s->mat.k, emat, Rax, 1, m->settings.formulation};
    OrthoLoads Ld = {m->fixed, m->fixed_value, force};
    SolidOptions opt = {m->settings.solver, m->settings.pcg_tol > 0 ? m->settings.pcg_tol : 1e-10, 0, 0, pool, NULL, NULL};
    bool ok = ortho_solve(&om, &Ld, &opt, o, err, errlen);
    if (pool) pool_destroy(pool);
    free(force), free(mats), free(emat), free(fval);
    if (!ok) {
        free(wf);
        solve_failure_code(err, code, codelen);
        return false;
    }
    *wf_out = wf;
    return true;
}

/* walls and infill: at each Gauss point wall and core share the strain, each checked against its own strengths
 * (GOALS.md G17 step 3); a core point's mode is kept plus 32 */
static void layout_strength(const MechStructSetup *s, const double Rax[9], const OrthoResult *o, const double *wf, size_t ne, double *gp_index,
                            unsigned char *gp_mode, StrengthSummary *sum, bool *core_governs) {
    double Cw[6][6], Cc[6][6], T[6][6];
    char why[200];
    ortho_stiffness(&s->mat.k, Cw, why, sizeof why), ortho_stiffness(&s->layout.core, Cc, why, sizeof why);
    voigt_strain_transform(Rax, T);
    sum->min_ratio = INFINITY;
    for (size_t e = 0; e < ne; e++)
        for (int g = 0; g < 8; g++) {
            const double *eg = o->gp_strain + 48 * e + 6 * (size_t)g;
            double em[6] = {0}, sw[6] = {0}, sc[6] = {0};
            for (int p = 0; p < 6; p++)
                for (int q = 0; q < 6; q++) em[p] += T[p][q] * eg[q];
            for (int p = 0; p < 6; p++)
                for (int q = 0; q < 6; q++) sw[p] += Cw[p][q] * em[q], sc[p] += Cc[p][q] * em[q];
            StrengthResult rw = {0, INFINITY, -1}, rc = {0, INFINITY, -1};
            if (wf[e] > 0) strength_evaluate(s->criterion, &s->mat.strength, sw, &rw);
            if (wf[e] < 1) strength_evaluate(s->criterion, &s->layout.core_strength, sc, &rc);
            bool core = rc.index > rw.index;
            const StrengthResult *rr = core ? &rc : &rw;
            size_t i = 8 * e + (size_t)g;
            gp_index[i] = rr->index;
            gp_mode[i] = rr->mode < 0 ? 255 : (unsigned char)(rr->mode + (core ? 32 : 0));
            if (rr->index > sum->max_index || sum->elem < 0) {
                sum->max_index = rr->index, sum->min_ratio = rr->ratio, sum->elem = (int)e, sum->gp = g, sum->mode = rr->mode;
                memcpy(sum->sig_m, core ? sc : sw, sizeof sum->sig_m);
                *core_governs = core;
            }
        }
}

/* Step 4's comparison: the part printed with each principal axis of the build frame as its build direction, the raster
 * reference kept unless it lies along that axis (then the next axis). Loads, supports and mesh are the part's own; walls
 * and infill are laid out again for each direction. */
static JsonValue *compare_orientations(const StaticModel *m, const MechStructSetup *s, const Stated *placed, const Zones *z, double zone,
                                       int *strongest) {
    static const char *const AXIS[3] = {"x", "y", "z"};
    JsonValue *arr = json_array();
    double best = -1;
    *strongest = -1;
    size_t ne = (size_t)m->nelems;
    for (int a = 0; a < 3; a++) {
        double b[3] = {0, 0, 0}, xr[3], Ra[9];
        b[a] = 1;
        double along = fabs(s->R[2] * b[0] + s->R[5] * b[1] + s->R[8] * b[2]); /* the placed build direction is column 3 of R */
        Stated st;
        memset(&st, 0, sizeof st);
        char code[64] = "", err[400] = "";
        bool ok = true;
        if (fabs(along - 1) < 1e-9) st = *placed;
        else {
            memcpy(xr, s->raster_ref, sizeof xr);
            double c = fabs(xr[0] * b[0] + xr[1] * b[1] + xr[2] * b[2]) / sqrt(xr[0] * xr[0] + xr[1] * xr[1] + xr[2] * xr[2]);
            if (c > 1 - 1e-9) xr[0] = xr[1] = xr[2] = 0, xr[(a + 1) % 3] = 1;
            ok = ortho_print_axes(b, xr, s->raster_angle, Ra);
            if (ok && m->elem_type == SOLID_ELEM_HEX8) {
                OrthoResult o;
                memset(&o, 0, sizeof o);
                double *wf = NULL;
                ok = printed_solve_hex(m, s, Ra, &o, &wf, code, sizeof code, err, sizeof err);
                if (ok && wf) {
                    double *gi = malloc((8 * ne ? 8 * ne : 1) * sizeof *gi);
                    unsigned char *gm = malloc(8 * ne ? 8 * ne : 1);
                    StrengthSummary sum = {0};
                    sum.elem = -1;
                    bool core = false;
                    if (gi && gm) {
                        layout_strength(s, Ra, &o, wf, ne, gi, gm, &sum, &core);
                        stated_from_gp(m, gi, gm, z, zone, &st);
                    } else
                        ok = false, snprintf(err, sizeof err, "out of memory for the comparison");
                    free(gi), free(gm);
                } else if (ok)
                    stated_compute(m, s, Ra, o.node_stress, z, zone, &st);
                free(wf);
                ortho_result_free(&o);
            } else if (ok) {
                SolidResult res;
                memset(&res, 0, sizeof res);
                ok = printed_solve_tet(m, s, Ra, &res, code, sizeof code, err, sizeof err);
                if (ok) stated_compute(m, s, Ra, res.node_stress, z, zone, &st);
                solid_result_free(&res);
            }
        }
        JsonValue *o = json_object();
        json_set_string(o, "build_direction", AXIS[a]);
        json_set_bool(o, "as_placed", fabs(along - 1) < 1e-9);
        if (ok && st.found) {
            json_set_number(o, "stated_strength_ratio", st.ratio);
            json_set_string(o, "governing_mode", strength_mode_name(st.mode));
            json_set_string(o, "peak_flag", st.flag);
            if (st.ratio > best) best = st.ratio, *strongest = a;
        } else
            json_set_string(o, "note", ok ? "no ratio stated: every loaded point lies within a zone" : err);
        json_push(arr, o);
    }
    return arr;
}

/* GOALS.md G17 step 8: whether the stated point stands at a voxel staircase, where the steps distort the stress (the ratio
 * read 5 to 24 % low on that step's ring and hole, and higher than TET10's on a small fillet, step 9): a node of an
 * element that touches a face mapped to an STL triangle inclined to the grid, or with walls and infill a point in such
 * an element */
static bool stated_near_staircase(const StaticModel *m, const Stated *st) {
    if (!st->found || m->elem_type != SOLID_ELEM_HEX8 || !m->face_weight) return false;
    size_t nn = (size_t)m->nnodes, ne = (size_t)m->nelems;
    unsigned char *stair = calloc(nn ? nn : 1, 1);
    if (!stair) return false;
    bool any = false, near = false;
    for (int f = 0; f < m->nfaces; f++) {
        if (!(m->face_weight[f] < 1 - 1e-9)) continue;
        int e = m->face_elem[f], lf = m->face_local[f];
        for (int a = 0; a < 4; a++) stair[m->conn[8 * (size_t)e + (size_t)HEX8_FACE_NODES[lf][a]]] = 1;
        any = true;
    }
    for (size_t e = 0; any && e < ne && !near; e++) {
        const int *c = m->conn + 8 * e;
        bool touches = false, holds = st->elem == (int)e;
        for (int a = 0; a < 8; a++) touches |= stair[c[a]] != 0, holds |= c[a] == st->node;
        near = touches && holds;
    }
    free(stair);
    return near;
}

/* The answer of step 4, in words: how many times the entered load the part holds (and that in kilograms when one force
 * was entered), where and how it would first fail, what the peak flag means, which principal build direction is
 * strongest, and where the strengths come from. */
/* the break estimate at the stated point (GOALS.md G20 step 5): the section through the wall there, for a governing
 * normal stress in tension on a solid part; false when it does not apply */
static bool break_estimate(const StaticModel *m, const MechStructSetup *s, const Stated *st, const double *node_stress, BreakSection *bs,
                           double *c_out) {
    if (!st->found || !node_stress || s->layout.on || !s->mat.has_strength) return false;
    int comp = st->mode == 0 ? 0 : st->mode == 2 ? 1 : st->mode == 4 ? 2 : -1; /* 1t, 2t, 3t */
    if (comp < 0) return false;
    const OrthoStrength *k = &s->mat.strength;
    double s_t = comp == 0 ? k->Xt : comp == 1 ? k->Yt : k->Zt;
    const char *id = s->mat.id;
    double c = strstr(id, "pla") ? BREAKEST_C_PLA : (strstr(id, "abs") || strstr(id, "asa")) ? BREAKEST_C_STYRENIC : BREAKEST_C_OTHER;
    *c_out = c;
    return breakest_fe_section(m, node_stress, s->R, comp, st->x, s_t, c, BREAKEST_M, bs) && bs->ok && bs->load_factor > 0 &&
           isfinite(bs->load_factor);
}

static void answer_json(const StaticModel *m, const MechStructSetup *s, const Stated *st, double zone, const JsonValue *orient, int strongest,
                        const double *node_stress, JsonValue *S) {
    JsonValue *A = json_set_object(S, "answer");
    double F[3] = {0, 0, 0};
    int nforce = 0;
    bool gravity = false;
    for (int k = 0; k < m->nbc; k++) {
        const SaBcInfo *b = &m->bc[k];
        if (b->kind == BC_GRAVITY) gravity = true;
        if (b->kind != BC_FORCE && b->kind != BC_TRACTION && b->kind != BC_PRESSURE) continue;
        nforce++;
        for (int a = 0; a < 3; a++) F[a] += b->applied[a];
    }
    double load = sqrt(F[0] * F[0] + F[1] * F[1] + F[2] * F[2]);
    char text[2400];
    size_t n = 0;
    if (!strcmp(st->flag, "sharp_inside_corner"))
        n += (size_t)snprintf(text + n, sizeof text - n,
                              "The highest stress sits at a sharp inside corner at (%.3g, %.3g, %.3g) mm, which a model of a perfectly sharp corner "
                              "cannot rate, and the corner will hold less than what follows: round it (a fillet) and run again. Read %.2g mm away "
                              "from it, ",
                              st->peak_x[0] * 1e3, st->peak_x[1] * 1e3, st->peak_x[2] * 1e3, zone * 1e3);
    else if (!strcmp(st->flag, "support"))
        n += (size_t)snprintf(text + n, sizeof text - n,
                              "The highest stress sits where a support holds the part, which an ideal rigid support exaggerates. Read %.2g mm away "
                              "from it, ", zone * 1e3);
    if (st->found) {
        bool between = st->mode == 4 || st->mode == 5 || st->mode == 7 || st->mode == 8; /* 3t, 3c, s23, s31 */
        char kg[64] = "";
        if (nforce == 1 && load > 0) snprintf(kg, sizeof kg, " (about %.2g kg)", st->ratio * load / 9.80665);
        const char *what = nforce == 0 ? (gravity ? "its own weight" : "the loads you entered")
                                       : (nforce == 1 ? "the load you entered" : "every load you entered");
        n += (size_t)snprintf(text + n, sizeof text - n, "%s %.2g times %s%s before it would first fail, %s at (%.3g, %.3g, %.3g) mm%s, by %s.",
                              n ? "it holds about" : "Holds about", st->ratio, what, kg, between ? "between the layers" : "along the layers",
                              st->x[0] * 1e3, st->x[1] * 1e3, st->x[2] * 1e3, s->layout.on ? (st->core ? " in the infill" : " in the walls") : "",
                              strength_mode_name(st->mode));
        json_set_number(A, "strength_ratio", st->ratio);
        json_set_number(A, "load_n", load);
        if (nforce == 1 && load > 0) {
            json_set_number(A, "failure_load_n", st->ratio * load);
            json_set_number(A, "failure_load_kg", st->ratio * load / 9.80665);
        }
        json_set_bool(A, "between_layers", between);
        json_set_string(A, "governing_mode", strength_mode_name(st->mode));
        json_set(A, "where_mm", json_vec3(st->x[0] * 1e3, st->x[1] * 1e3, st->x[2] * 1e3));
        BreakSection bs;
        double cc = 0;
        JsonValue *B = json_set_object(A, "break_estimate");
        if (break_estimate(m, s, st, node_stress, &bs, &cc)) {
            /* the factor on the stated loads: the section's factor on the stresses at the point, which first failure reads
             * as st->ratio; both scale with the loads */
            double lf = bs.load_factor;
            json_set_number(B, "load_factor", lf);
            if (nforce == 1 && load > 0) json_set_number(B, "break_load_n", lf * load), json_set_number(B, "break_load_kg", lf * load / 9.80665);
            json_set_number(B, "membrane_mpa", bs.membrane / 1e6);
            json_set_number(B, "bending_mpa", bs.bending / 1e6);
            json_set_number(B, "wall_mm", bs.wall * 1e3);
            json_set(B, "through_wall_direction", json_vec3(bs.dir[0], bs.dir[1], bs.dir[2]));
            json_set_number(B, "compression_over_tension", cc);
            json_set_number(B, "outer_fibre_strain_over_elastic_limit", BREAKEST_M);
            json_set_string(B, "method", "the governing stress through the wall at the stated point, split into membrane and bending (ASME VIII Div. 2 "
                                         "Annex 5-A), on a section that flows at its tensile strength in tension and at c times it in compression "
                                         "and breaks when its outer tensile fibre reaches m times the elastic limit strain (src/mech/breakest.c); c "
                                         "and m fixed on 9 calibration studies, GOALS.md G20 step 5");
            char kgb[64] = "";
            if (nforce == 1 && load > 0) snprintf(kgb, sizeof kgb, " (about %.2g kg)", lf * load / 9.80665);
            n += (size_t)snprintf(text + n, sizeof text - n, " The break estimate, with the plastic flowing before it breaks: about %.2g times%s.",
                                  lf, kgb);
        } else
            json_set_string(B, "not_evaluated", s->layout.on ? "walls and infill: the break estimate is for solid sections"
                                                              : "the governing stress there is not a tension along a material axis");
        bool stair = stated_near_staircase(m, st);
        if (m->elem_type == SOLID_ELEM_HEX8 && m->face_weight) json_set_bool(S, "stated_near_staircase", stair);
        if (stair && n < sizeof text)
            n += (size_t)snprintf(text + n, sizeof text - n,
                                  " That point stands where a curved or inclined surface meets the voxel grid in steps, which distort the "
                                  "stress there: the ratio read 5 to 24 %% low on a ring and a hole and 18 %% above TET10's on a small fillet "
                                  "(GOALS.md G17 steps 8 and 9)%s.",
                                  s->layout.on ? "; walls and infill are laid out on voxels only"
                                               : "; a TET10 mesh (mesh_generate with method tet and order 2) follows the surface and reads such "
                                                 "surfaces within about 1 %");
    } else
        n += (size_t)snprintf(text + n, sizeof text - n, "%sno strength ratio can be stated: every loaded point lies within a zone of a sharp "
                                                         "inside corner or a support.", n ? "" : "No strength ratio can be stated: ");
    json_set_string(A, "flag", st->flag);
    if (orient) {
        json_set(A, "orientations", json_clone(orient));
        const JsonValue *best = strongest >= 0 ? json_at(orient, (size_t)strongest) : NULL;
        if (best && st->found) {
            double rb = json_get_num(best, "stated_strength_ratio", 0);
            if (json_get_bool(best, "as_placed", false))
                n += (size_t)snprintf(text + n, sizeof text - n, " As placed is the strongest of the three principal build directions.");
            else if (rb <= st->ratio * 1.005)
                n += (size_t)snprintf(text + n, sizeof text - n, " As placed is about as strong as the strongest of the three principal build "
                                                                 "directions (%s).", json_get_str(best, "build_direction", "?"));
            else
                n += (size_t)snprintf(text + n, sizeof text - n, " Printed with %s as the build direction it would hold about %.2g times as much: the "
                                                                 "strongest of the three principal build directions.",
                                      json_get_str(best, "build_direction", "?"), rb / st->ratio);
            json_set_string(A, "strongest_build_direction", json_get_str(best, "build_direction", "?"));
        }
    }
    /* the inputs' provenance: who set each support and load, and where the material's values come from */
    int nuser = 0, nother = 0;
    char others[320] = "";
    size_t no = 0;
    for (size_t i = 0; i < json_len(s->inputs); i++) {
        const JsonValue *o = json_at(s->inputs, i);
        if (!strcmp(json_get_str(o, "source", ""), "user"))
            nuser++;
        else if (no < sizeof others) {
            nother++;
            no += (size_t)snprintf(others + no, sizeof others - no, "%s%s (%s)", nother > 1 ? ", " : "", json_get_str(o, "name", "?"),
                                   json_get_str(o, "source", "?"));
        } else
            nother++;
    }
    JsonValue *src = json_set_object(A, "sources");
    json_set_string(src, "material", s->mat.id);
    json_set_string(src, "elastic_constants", s->mat.elastic_source);
    json_set_string(src, "strengths", s->mat.strength_source);
    if (s->layout.on) json_set(src, "walls_and_infill", json_clone(s->layout.rules));
    json_set(src, "supports_and_loads", json_clone(s->inputs));
    if (n < sizeof text) {
        if (nother == 0)
            n += (size_t)snprintf(text + n, sizeof text - n, " Supports and loads: %s set by you.", nuser == 1 ? "the one" : "all");
        else
            n += (size_t)snprintf(text + n, sizeof text - n, " Supports and loads: %d set by you, %d not: %s.", nuser, nother, others);
    }
    bool has_break = json_get(json_get(A, "break_estimate"), "load_factor") != NULL;
    if (n < sizeof text)
        snprintf(text + n, sizeof text - n, " Strengths of %s: %s. %s", s->mat.id, s->mat.strength_source,
                 has_break ? "Both are estimates for non-critical parts, not a guarantee."
                           : "A linear-elastic first-failure estimate for non-critical parts, not a guarantee.");
    json_set_string(A, "text", text);
    json_set_string(A, "accuracy_note",
                    "where a curved or inclined surface meets the voxel grid in steps, the steps distort the stress (the ratio 5 to 24 % low "
                    "on a ring and a hole, 18 % above TET10's on a small fillet), so a printed part without walls and infill is solved on "
                    "TET10, made at the voxel size (GOALS.md G17 steps 8 and 9; stated_near_staircase marks a stated point at such steps when "
                    "voxels are kept); on TET10, which keeps sharp edges since 2026-10-08, a ring and a hole read within 1.2 % of their closed "
                    "forms, and a fillet, hole or edge only a few cells across moves when the surface size is halved. In 26 published tests "
                    "of printed plastics the parts held a median 1.7 times the first-failure estimate before breaking (GOALS.md G17 steps 4 "
                    "and 5); the break estimate met them blind within a median 17 %, and 21 groups of six further studies within 13 % "
                    "(GOALS.md G20 step 5)");
}

static bool printed_solve(StaticResults *r, char *code, size_t codelen, char *err, size_t errlen) {
    MechStructSetup *s = r->printed;
    StaticModel *m = &r->model;
    size_t nn = (size_t)m->nnodes, ne = (size_t)m->nelems;
    int ngp = sa_ngp(m), npe = sa_npe(m);
    const PrintLayout *L = &s->layout;
    OrthoResult o;
    memset(&o, 0, sizeof o);
    double *wf = NULL;
    if (m->elem_type != SOLID_ELEM_HEX8) {
        if (!printed_solve_tet(m, s, s->R, &r->sol, code, codelen, err, errlen)) return false;
    } else {
        if (!printed_solve_hex(m, s, s->R, &o, &wf, code, codelen, err, errlen)) return false;
        /* the solution, in the static result's own fields */
        SolidResult *q = &r->sol;
        q->neq = o.neq, q->u = o.u, q->reaction = o.reaction, q->gp_strain = o.gp_strain, q->gp_stress = o.gp_stress;
        q->node_stress = o.node_stress, q->stats = o.stats, q->strain_energy = o.strain_energy, q->external_work = o.external_work;
        q->equilibrium_error = o.equilibrium_error, q->min_detJ = o.min_detJ, q->stiffness_cache_hits = o.stiffness_cache_hits;
        memcpy(q->load_total, o.load_total, sizeof q->load_total), memcpy(q->reaction_total, o.reaction_total, sizeof q->reaction_total);
        q->gp_per_elem = 0;
    }
    JsonValue *S = json_object();
    json_set(S, "material_model", json_clone(s->description));
    json_set_string(S, "elastic_source", s->mat.elastic_source);
    json_set_string(S, "element", sa_element_name(m));
    if (s->mat.has_strength) {
        size_t npts = (size_t)ngp * ne;
        r->gp_index = malloc((npts ? npts : 1) * sizeof *r->gp_index);
        r->node_index = calloc(nn ? nn : 1, sizeof *r->node_index);
        r->gp_mode = malloc((npts ? npts : 1) * sizeof *r->gp_mode);
        if (!r->gp_index || !r->node_index || !r->gp_mode) {
            free(o.gp_stress_mat), free(wf);
            json_free(S);
            snprintf(code, codelen, "RESOURCE_LIMIT");
            snprintf(err, errlen, "out of memory for the failure indices");
            return false;
        }
        StrengthSummary sum = {0};
        sum.elem = -1;
        bool core_governs = false;
        if (wf) /* walls and infill */
            layout_strength(s, s->R, &o, wf, ne, r->gp_index, r->gp_mode, &sum, &core_governs);
        else if (m->elem_type == SOLID_ELEM_HEX8) {
            ortho_strength_field(&o, m->nelems, s->criterion, &s->mat.strength, r->gp_index, &sum);
            for (size_t i = 0; i < npts; i++) { /* the governing term of every point, kept beside its index */
                StrengthResult sr;
                strength_evaluate(s->criterion, &s->mat.strength, o.gp_stress_mat + 6 * i, &sr);
                r->gp_mode[i] = sr.mode < 0 ? 255 : (unsigned char)sr.mode;
            }
        } else { /* the tetrahedral solver's stresses are in the build frame: each point turned into the material axes */
            sum.min_ratio = INFINITY, sum.gp = -1, sum.mode = -1;
            for (size_t i = 0; i < npts; i++) {
                double sm[6];
                voigt_stress_to_material(s->R, r->sol.gp_stress + 6 * i, sm);
                StrengthResult sr;
                strength_evaluate(s->criterion, &s->mat.strength, sm, &sr);
                r->gp_index[i] = sr.index;
                r->gp_mode[i] = sr.mode < 0 ? 255 : (unsigned char)sr.mode;
                if (sr.index > sum.max_index || sum.elem < 0) {
                    sum.max_index = sr.index, sum.min_ratio = sr.ratio, sum.elem = (int)(i / (size_t)ngp), sum.gp = (int)(i % (size_t)ngp);
                    sum.mode = sr.mode;
                    memcpy(sum.sig_m, sm, sizeof sum.sig_m);
                }
            }
        }
        for (size_t e = 0; e < ne; e++) { /* each node shows the largest index around it: a weak spot is never averaged away */
            double emax = 0;
            for (int g = 0; g < ngp; g++) emax = fmax(emax, r->gp_index[(size_t)ngp * e + (size_t)g]);
            for (int a = 0; a < npe; a++) {
                double *v = &r->node_index[m->conn[(size_t)npe * e + (size_t)a]];
                if (emax > *v) *v = emax;
            }
        }
        r->has_strength = true;
        json_set_string(S, "criterion", strength_criterion_name(s->criterion));
        json_set_number(S, "largest_failure_index", sum.max_index);
        json_set_number(S, "smallest_strength_ratio", isfinite(sum.min_ratio) ? sum.min_ratio : 0);
        if (sum.elem >= 0) {
            double xg[3];
            gp_position(m, sum.elem, sum.gp, xg);
            json_set_string(S, "governing_mode", strength_mode_name(sum.mode));
            if (wf) json_set_string(S, "governing_part", core_governs ? "infill" : "walls");
            json_set_int(S, "governing_element", sum.elem);
            json_set(S, "location_mm", json_vec3(xg[0] * 1e3, xg[1] * 1e3, xg[2] * 1e3));
            json_set(S, "material_axes_stress_mpa",
                     json_numbers((double[6]){sum.sig_m[0] / 1e6, sum.sig_m[1] / 1e6, sum.sig_m[2] / 1e6, sum.sig_m[3] / 1e6, sum.sig_m[4] / 1e6, sum.sig_m[5] / 1e6}, 6));
        }
        /* what step 4 states (GOALS.md G17): the ratio outside the singular zones (at the nodes, or at the Gauss points with
         * walls and infill), the comparison of build directions when asked for, and the answer in words */
        double zone = zone_of(m);
        Zones z;
        Stated st;
        if (zones_init(&z, m, zone)) {
            if (wf) stated_from_gp(m, r->gp_index, r->gp_mode, &z, zone, &st);
            else stated_compute(m, s, s->R, r->sol.node_stress, &z, zone, &st);
            stated_json(&st, zone, wf != NULL, S);
            JsonValue *orient = NULL;
            int strongest = -1;
            if (s->compare_orientations) orient = compare_orientations(m, s, &st, &z, zone, &strongest);
            answer_json(m, s, &st, zone, orient, strongest, wf ? NULL : r->sol.node_stress, S);
            json_free(orient);
            zones_free(&z);
        } else
            json_set_string(S, "peak_flag", "not evaluated (out of memory)");
        json_set_string(S, "strength_source", s->mat.strength_source);
        json_set_string(S, "reading",
                        "a linear-elastic first-failure estimate for non-critical parts, not a guarantee: the strength ratio is the factor on all "
                        "the loads at which the criterion is first reached at the governing Gauss point; it inherits the uncertainty of the "
                        "strengths and of the homogenised printed solid (no voids, layer-scale defects or flaws)");
    } else
        json_set_string(S, "criterion", "not evaluated: the material record has no strengths, so only stresses and deformation are reported");
    JsonValue *as = json_set_array(S, "assumptions");
    mech_struct_assumptions(as, s);
    if (wf) { /* what the layout made of the part: the counts a person can check against the slicer's preview */
        JsonValue *lj = json_set_object(S, "walls_and_infill");
        json_set_int(lj, "perimeters", L->perimeters), json_set_number(lj, "line_width_mm", L->line_width * 1e3);
        json_set_int(lj, "top_layers", L->top_layers), json_set_int(lj, "bottom_layers", L->bottom_layers);
        json_set_number(lj, "layer_height_mm", L->layer_height * 1e3);
        json_set_number(lj, "wall_mm", L->perimeters * L->line_width * 1e3), json_set_number(lj, "top_mm", L->top_layers * L->layer_height * 1e3);
        json_set_number(lj, "bottom_mm", L->bottom_layers * L->layer_height * 1e3);
        json_set_number(lj, "infill_density", L->density), json_set_string(lj, "infill_pattern", L->pattern);
        long long nw = 0, nc = 0;
        double vw = 0, vt = 0;
        for (size_t e = 0; e < ne; e++) {
            double v = m->h[0] * m->h[1] * m->h[2];
            nw += wf[e] >= 1, nc += wf[e] <= 0, vw += wf[e] * v, vt += v;
        }
        json_set_int(lj, "elements_in_walls", nw), json_set_int(lj, "elements_in_infill", nc), json_set_int(lj, "elements_mixed", (long long)ne - nw - nc);
        json_set_number(lj, "wall_volume_fraction", vt > 0 ? vw / vt : 0);
        JsonValue *ck = json_set_object(lj, "core");
        json_set_number(ck, "E1_gpa", L->core.E[0] / 1e9), json_set_number(ck, "E2_gpa", L->core.E[1] / 1e9), json_set_number(ck, "E3_gpa", L->core.E[2] / 1e9);
        if (s->mat.has_strength)
            json_set_number(ck, "Xt_mpa", L->core_strength.Xt / 1e6), json_set_number(ck, "Yt_mpa", L->core_strength.Yt / 1e6),
                json_set_number(ck, "Zt_mpa", L->core_strength.Zt / 1e6);
    }
    free(wf);
    free(o.gp_stress_mat);
    json_free(r->strength);
    r->strength = S;
    return true;
}

bool mech_static_printed_attach(const Project *p, const JsonValue *mm, StaticResults *r, OpResult *out) {
    if (r->model.elem_type == SOLID_ELEM_TET4) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "mesh the part with voxels or with second-order tetrahedra (mesh_generate method tet, order 2)",
                "the printed-material model is solved on voxels (hex8) or TET10, not on linear tetrahedra, which lock in bending");
        return false;
    }
    MechStructSetup *s = calloc(1, sizeof *s);
    if (!s) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return false;
    }
    if (!mech_struct_setup(p, mm, s, out)) {
        free(s);
        return false;
    }
    s->inputs = json_array(); /* the supports and loads with who set each (step 4's provenance; the same list as 9c113f0's) */
    for (int i = 0; i < p->nbcs; i++) {
        const BoundaryCondition *b = &p->bcs[i];
        if (b->kind > BC_GRAVITY) continue;
        JsonValue *o = json_object();
        json_set_string(o, "name", b->name);
        json_set_string(o, "kind", bc_kind_name(b->kind));
        json_set_string(o, "source", provenance_name(b->source));
        json_push(s->inputs, o);
    }
    r->printed = s, r->printed_free = printed_free, r->printed_solve = printed_solve;
    return true;
}

const JsonValue *mech_static_printed_description(const StaticResults *r) {
    return r->printed ? ((const MechStructSetup *)r->printed)->description : NULL;
}

bool mech_struct_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    MechStructJob *J = data;
    StaticModel *m = &J->model;
    MechStructSetup *s = &J->setup;
    job_progress(job, 0.02, "assembling the orthotropic stiffness matrix");
    int threads = cpu_perf_count();
    ThreadPool *pool = pool_create(threads > 0 ? threads : 1);
    Prog pr = {job};
    OrthoModel om = {m->nnodes, m->nelems, m->xyz, m->conn, 1, &s->mat.k, NULL, s->R, 1, HEX8_INCOMPATIBLE};
    OrthoLoads L = {m->fixed, m->fixed_value, m->nodal_force};
    SolidOptions opt = {SOLID_SOLVER_AUTO, 1e-10, 0, 0, pool, on_progress, &pr};
    OrthoResult r;
    bool ok = ortho_solve(&om, &L, &opt, &r, err, errlen);
    if (pool) pool_destroy(pool);
    if (!ok) {
        snprintf(code, codelen, "%s", job_cancel_requested(job) ? "CANCELLED" : (strstr(err, "out of memory") ? "RESOURCE_LIMIT" : "SOLVER_FAILED"));
        return false;
    }
    if (!r.stats.converged) {
        snprintf(code, codelen, "SOLVER_FAILED");
        snprintf(err, errlen, "the solver did not converge: relative residual %.3g after %d iterations", r.stats.rel_residual, r.stats.iterations);
        ortho_result_free(&r);
        return false;
    }
    job_progress(job, 0.92, "evaluating stresses and strength");
    size_t ne = (size_t)m->nelems, nn = (size_t)m->nnodes;
    double *gi = s->mat.has_strength ? malloc(8 * ne * sizeof(double)) : NULL;
    StrengthSummary sum = {0};
    if (s->mat.has_strength) {
        if (!gi) {
            ortho_result_free(&r);
            snprintf(code, codelen, "RESOURCE_LIMIT");
            snprintf(err, errlen, "out of memory for failure indices");
            return false;
        }
        ortho_strength_field(&r, m->nelems, s->criterion, &s->mat.strength, gi, &sum);
    }
    /* summary */
    JsonValue *S = json_object();
    json_set_string(S, "analysis", "mech_structural");
    json_set_string(S, "job_id", J->job_id);
    json_set(S, "material_model", json_clone(s->description));
    JsonValue *sol = json_set_object(S, "solver");
    json_set_string(sol, "method", r.stats.method);
    json_set_int(sol, "equations", r.neq);
    json_set_number(sol, "true_residual", r.stats.true_residual);
    json_set_number(sol, "equilibrium_error", r.equilibrium_error);
    json_set_number(sol, "strain_energy_J", r.strain_energy);
    json_set_number(sol, "energy_balance", r.external_work > 0 ? r.strain_energy / (0.5 * r.external_work) - 1 : 0);
    json_set_int(sol, "stiffness_cache_hits", r.stiffness_cache_hits);
    json_set_string(sol, "formulation", "hex8 with incompatible modes");
    double umax = 0, xu[3] = {0};
    for (size_t i = 0; i < nn; i++) {
        double v = sqrt(r.u[3 * i] * r.u[3 * i] + r.u[3 * i + 1] * r.u[3 * i + 1] + r.u[3 * i + 2] * r.u[3 * i + 2]);
        if (v > umax) umax = v, memcpy(xu, m->xyz + 3 * i, sizeof xu);
    }
    JsonValue *dj = json_set_object(S, "largest_displacement");
    json_set_number(dj, "value_mm", umax * 1e3);
    json_set(dj, "location_mm", json_vec3(xu[0] * 1e3, xu[1] * 1e3, xu[2] * 1e3));
    json_set_string(dj, "reference", "relative to the isostatic support nodes (rigid-body motion removed)");
    static const char *const COMP[6] = {"11", "22", "33", "12", "23", "31"};
    JsonValue *ms = json_set_array(S, "largest_material_axes_stress");
    for (int c = 0; c < 6; c++) {
        double best = 0, xb[3] = {0};
        for (size_t e = 0; e < ne; e++)
            for (int g = 0; g < 8; g++) {
                double v = r.gp_stress_mat[48 * e + 6 * (size_t)g + (size_t)c];
                if (fabs(v) > fabs(best)) best = v, gp_position(m, (int)e, g, xb);
            }
        JsonValue *o = json_object();
        json_set_string(o, "component", COMP[c]);
        json_set_number(o, "value_mpa", best / 1e6);
        json_set(o, "location_mm", json_vec3(xb[0] * 1e3, xb[1] * 1e3, xb[2] * 1e3));
        json_push(ms, o);
    }
    if (s->mat.has_strength && sum.elem >= 0) {
        JsonValue *fj = json_set_object(S, "strength");
        double xg[3];
        gp_position(m, sum.elem, sum.gp, xg);
        json_set_string(fj, "criterion", strength_criterion_name(s->criterion));
        json_set_number(fj, "largest_failure_index", sum.max_index);
        json_set_number(fj, "smallest_strength_ratio", sum.min_ratio);
        json_set_string(fj, "governing_mode", strength_mode_name(sum.mode));
        json_set(fj, "location_mm", json_vec3(xg[0] * 1e3, xg[1] * 1e3, xg[2] * 1e3));
        json_set(fj, "material_axes_stress_mpa",
                 json_numbers((double[6]){sum.sig_m[0] / 1e6, sum.sig_m[1] / 1e6, sum.sig_m[2] / 1e6, sum.sig_m[3] / 1e6, sum.sig_m[4] / 1e6, sum.sig_m[5] / 1e6}, 6));
        json_set_string(fj, "data_source", s->mat.strength_source);
        json_set_string(fj, "reading",
                        "strength ratio = factor on this load state that reaches the criterion at the governing Gauss point; linear elasticity scales "
                        "stresses with the loads, and the assessment is quasi-static at one instant");
    } else
        json_set_string(S, "strength", "not evaluated: the material record has no strengths");
    /* reactions of the isostatic support */
    const JsonValue *nodes = json_get(json_get(J->report, "support"), "nodes");
    double rmax = 0;
    for (size_t i = 0; i < json_len(nodes); i++) {
        long long nd = json_get_int(json_at(nodes, i), "node", -1);
        if (nd < 0 || nd >= m->nnodes) continue;
        for (int k = 0; k < 3; k++) rmax = fmax(rmax, fabs(r.reaction[3 * (size_t)nd + (size_t)k]));
    }
    double scale = json_get_num(json_get(J->report, "balance"), "load_scale_N", 0);
    JsonValue *chk = json_set_object(J->report, "support_reactions");
    json_set_number(chk, "largest_component_N", rmax);
    json_set_number(chk, "relative_to_load_scale", scale > 0 ? rmax / scale : 0);
    json_set_string(chk, "reading", rmax <= 1e-6 * scale ? "negligible: the load set is self-equilibrated and the stresses do not depend on the support choice"
                                                         : "not negligible: the rigid-body and FEM models disagree; stresses near the support nodes are not physical");
    json_set(S, "mechanical_assessment", json_clone(J->report));
    /* files */
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, J->run_dir, "mech_structure.bin");
    FILE *f = fopen(path, "wb");
    int32_t hdr[3] = {(int32_t)nn, (int32_t)ne, gi ? 1 : 0};
    bool wok = f && wr(f, "NVMSTR01", 1, 8) && wr(f, hdr, sizeof hdr[0], 3) && wr(f, m->xyz, sizeof(double), 3 * nn) && wr(f, m->conn, sizeof(int), 8 * ne) &&
               wr(f, r.u, sizeof(double), 3 * nn) && wr(f, r.gp_stress, sizeof(double), 48 * ne) && wr(f, r.gp_stress_mat, sizeof(double), 48 * ne) &&
               wr(f, r.gp_strain, sizeof(double), 48 * ne) && (!gi || wr(f, gi, sizeof(double), 8 * ne)) && wr(f, s->R, sizeof(double), 9);
    if (f) wok &= fclose(f) == 0;
    free(gi);
    ortho_result_free(&r);
    if (!wok) {
        json_free(S);
        snprintf(code, codelen, "IO_ERROR");
        snprintf(err, errlen, "cannot write %s", path);
        return false;
    }
    JsonValue *files = json_set_array(S, "files");
    char hex[65];
    uint64_t bytes = 0;
    if (sha256_file(path, hex, &bytes)) {
        JsonValue *fo = json_object();
        json_set_string(fo, "name", "mech_structure.bin");
        json_set_string(fo, "format", "NVMSTR01: counts, node coordinates (m), connectivity, displacements (m), Gauss-point stresses in global and material axes (Pa), strains, failure indices, material axes");
        json_set_int(fo, "bytes", (long long)bytes);
        json_set_string(fo, "sha256", hex);
        json_push(files, fo);
    }
    path_join(path, sizeof path, J->run_dir, "mech_assessment.json");
    json_write_file(path, J->report, JSON_PRETTY);
    path_join(path, sizeof path, J->run_dir, "summary.json");
    if (!json_write_file(path, S, JSON_PRETTY | JSON_SORTED)) {
        json_free(S);
        snprintf(code, codelen, "IO_ERROR");
        snprintf(err, errlen, "cannot write %s", path);
        return false;
    }
    job_progress(job, 1.0, "done");
    job_set_summary(job, S);
    return true;
}

void mech_struct_job_free(void *data) {
    MechStructJob *J = data;
    if (!J) return;
    static_model_free(&J->model);
    mech_struct_setup_free(&J->setup);
    json_free(J->report);
    free(J);
}

/* ------------------------------------------------------------------------------------------------ query */

typedef struct StructData {
    int nn, ne;
    bool has_index;
    double *xyz, *u, *sg, *sm, *eps, *gi, R[9];
    int *conn;
} StructData;

static void sd_free(StructData *d) { free(d->xyz), free(d->u), free(d->sg), free(d->sm), free(d->eps), free(d->gi), free(d->conn); }

static bool rd(FILE *f, void *p, size_t size, size_t n) { return n == 0 || fread(p, size, n, f) == n; }

static bool sd_load(const char *path, StructData *d, char *err, size_t errlen) {
    memset(d, 0, sizeof *d);
    FILE *f = fopen(path, "rb");
    char magic[8];
    int32_t hdr[3];
    if (!f || !rd(f, magic, 1, 8) || memcmp(magic, "NVMSTR01", 8) || !rd(f, hdr, sizeof hdr[0], 3) || hdr[0] < 0 || hdr[1] < 0) {
        if (f) fclose(f);
        snprintf(err, errlen, "%s is missing or not an orthotropic structure result", path);
        return false;
    }
    size_t nn = (size_t)hdr[0], ne = (size_t)hdr[1];
    d->nn = hdr[0], d->ne = hdr[1], d->has_index = hdr[2] != 0;
    d->xyz = malloc(3 * (nn ? nn : 1) * sizeof(double)), d->u = malloc(3 * (nn ? nn : 1) * sizeof(double)), d->conn = malloc(8 * (ne ? ne : 1) * sizeof(int));
    d->sg = malloc(48 * (ne ? ne : 1) * sizeof(double)), d->sm = malloc(48 * (ne ? ne : 1) * sizeof(double)), d->eps = malloc(48 * (ne ? ne : 1) * sizeof(double));
    d->gi = d->has_index ? malloc(8 * (ne ? ne : 1) * sizeof(double)) : NULL;
    bool ok = d->xyz && d->u && d->conn && d->sg && d->sm && d->eps && (!d->has_index || d->gi) && rd(f, d->xyz, sizeof(double), 3 * nn) &&
              rd(f, d->conn, sizeof(int), 8 * ne) && rd(f, d->u, sizeof(double), 3 * nn) && rd(f, d->sg, sizeof(double), 48 * ne) && rd(f, d->sm, sizeof(double), 48 * ne) &&
              rd(f, d->eps, sizeof(double), 48 * ne) && (!d->has_index || rd(f, d->gi, sizeof(double), 8 * ne)) && rd(f, d->R, sizeof(double), 9);
    fclose(f);
    for (size_t i = 0; ok && i < 8 * ne; i++) ok = d->conn[i] >= 0 && (size_t)d->conn[i] < nn;
    if (!ok) {
        sd_free(d);
        snprintf(err, errlen, "%s is truncated or corrupt", path);
    }
    return ok;
}

typedef struct Hit {
    double v;
    int e, g, node;
} Hit;

static void keep_largest(Hit *h, int cap, int *n, double key, Hit x) {
    if (*n < cap) {
        h[(*n)++] = x;
    } else {
        int wi = 0;
        for (int i = 1; i < cap; i++)
            if (fabs(h[i].v) < fabs(h[wi].v)) wi = i;
        if (fabs(key) <= fabs(h[wi].v)) return;
        h[wi] = x;
    }
    for (int i = *n - 1; i > 0 && fabs(h[i].v) > fabs(h[i - 1].v); i--) { /* keep sorted, largest first */
        Hit t = h[i];
        h[i] = h[i - 1], h[i - 1] = t;
    }
}

void op_mech_structure_query(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", ""), *q = json_get_str(p, "quantity", "summary"), *comp = json_get_str(p, "component", NULL);
    char dir[NV_PATH_MAX], path[NV_PATH_MAX], err[600];
    if (!op_project_run_dir(e, id, dir, sizeof dir)) {
        op_fail(out, NV_ERR_NOT_FOUND, "use job_list for the jobs of the open project", "no run directory for job '%s'", id);
        return;
    }
    path_join(path, sizeof path, dir, "summary.json");
    JsonError je;
    JsonValue *summary = json_read_file(path, 64 << 20, &je);
    if (!summary || strcmp(json_get_str(summary, "analysis", ""), "mech_structural")) {
        json_free(summary);
        path_join(path, sizeof path, dir, "spec.json");
        JsonValue *spec = json_read_file(path, 64 << 20, &je);
        bool ortho = spec && json_get(spec, "material_model");
        json_free(spec);
        if (ortho) op_fail(out, NV_ERR_PRECONDITION, "wait with job_status {\"wait_seconds\": 30}", "job '%s' has no orthotropic results yet", id);
        else op_fail(out, NV_ERR_UNSUPPORTED, "use results_query for isotropic structural analyses", "job '%s' is not an orthotropic structural assessment", id);
        return;
    }
    if (!strcmp(q, "summary")) {
        JsonValue *v = json_object();
        json_set_string(v, "job_id", id);
        json_set(v, "summary", summary);
        op_succeed(out, v);
        return;
    }
    /* strengths and criterion of the assessment, for the governing mode of each reported point */
    MechMaterial rec;
    Parse ps;
    const JsonValue *mmj = json_get(summary, "material_model");
    bool have_rec = material_parse(json_get(mmj, "record"), NULL, false, &rec, NULL, &ps) && rec.has_strength;
    int crit = strength_criterion_from_name(json_get_str(mmj, "criterion", ""));
    json_free(summary);
    int nlarg = (int)json_get_int(p, "largest", 5);
    if (nlarg < 1) nlarg = 1;
    if (nlarg > 100) nlarg = 100;
    StructData d;
    path_join(path, sizeof path, dir, "mech_structure.bin");
    if (!sd_load(path, &d, err, sizeof err)) {
        op_fail(out, NV_ERR_IO, NULL, "%s", err);
        return;
    }
    static const char *const GC[6] = {"xx", "yy", "zz", "xy", "yz", "zx"}, *const MC[6] = {"11", "22", "33", "12", "23", "31"};
    Hit hits[100];
    int nh = 0;
    const char *unit = "MPa";
    double factor = 1e-6;
    bool nodal = false;
    if (!strcmp(q, "displacement")) {
        int c = comp ? (!strcmp(comp, "x") ? 0 : !strcmp(comp, "y") ? 1 : !strcmp(comp, "z") ? 2 : !strcmp(comp, "magnitude") ? 3 : -1) : 3;
        if (c < 0) {
            sd_free(&d);
            op_fail(out, NV_ERR_INVALID_PARAMS, "component is x, y, z or magnitude", "unknown displacement component '%s'", comp);
            return;
        }
        nodal = true, unit = "mm", factor = 1e3;
        for (int i = 0; i < d.nn; i++) {
            const double *u = d.u + 3 * (size_t)i;
            double v = c < 3 ? u[c] : sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            keep_largest(hits, nlarg, &nh, v, (Hit){v, -1, -1, i});
        }
    } else if (!strcmp(q, "stress") || !strcmp(q, "material_stress") || !strcmp(q, "strain") || !strcmp(q, "failure_index")) {
        bool mat = !strcmp(q, "material_stress"), fi = !strcmp(q, "failure_index"), strain = !strcmp(q, "strain");
        int c = -1;
        if (fi) {
            if (!d.has_index) {
                sd_free(&d);
                op_fail(out, NV_ERR_PRECONDITION, "add strengths with their source to the material record and assess again", "this assessment has no failure indices");
                return;
            }
            unit = "1", factor = 1;
        } else {
            const char *const *names = mat ? MC : GC;
            for (int i = 0; comp && i < 6; i++)
                if (!strcmp(comp, names[i])) c = i;
            if (!strcmp(q, "stress") && comp && !strcmp(comp, "von_mises")) c = 6;
            if (c < 0) {
                sd_free(&d);
                op_fail(out, NV_ERR_INVALID_PARAMS, mat ? "component is 11, 22, 33, 12, 23 or 31 (axis 3 = build direction)" : "component is xx, yy, zz, xy, yz, zx (or von_mises for stress)",
                        "missing or unknown component '%s'", comp ? comp : "");
                return;
            }
            if (strain) unit = "1 (engineering shear)", factor = 1;
        }
        for (int e2 = 0; e2 < d.ne; e2++)
            for (int g = 0; g < 8; g++) {
                size_t k = 48 * (size_t)e2 + 6 * (size_t)g;
                double v = fi ? d.gi[8 * (size_t)e2 + (size_t)g] : (c == 6 ? von_mises(d.sg + k) : (strain ? d.eps : mat ? d.sm : d.sg)[k + (size_t)c]);
                keep_largest(hits, nlarg, &nh, v, (Hit){v, e2, g, -1});
            }
    } else {
        sd_free(&d);
        op_fail(out, NV_ERR_INVALID_PARAMS, "quantity is summary, displacement, stress, material_stress, strain or failure_index", "unknown quantity '%s'", q);
        return;
    }
    JsonValue *v = json_object(), *arr = json_array();
    json_set_string(v, "job_id", id);
    json_set_string(v, "quantity", q);
    if (comp) json_set_string(v, "component", comp);
    json_set_string(v, "unit", unit);
    json_set_string(v, "evaluated_at", nodal ? "mesh nodes" : "Gauss points (no averaging across elements)");
    for (int i = 0; i < nh; i++) {
        JsonValue *o = json_object();
        double x[3] = {0, 0, 0};
        if (hits[i].node >= 0) memcpy(x, d.xyz + 3 * (size_t)hits[i].node, sizeof x);
        else {
            double N[8], dN[8][3];
            hex8_shape(HEX8_XI[hits[i].g][0] / sqrt(3), HEX8_XI[hits[i].g][1] / sqrt(3), HEX8_XI[hits[i].g][2] / sqrt(3), N, dN);
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) x[k] += N[a] * d.xyz[3 * (size_t)d.conn[8 * (size_t)hits[i].e + a] + (size_t)k];
            json_set_int(o, "element", hits[i].e);
            json_set_int(o, "gauss_point", hits[i].g);
        }
        json_set_number(o, "value", hits[i].v * factor);
        json_set(o, "location_mm", json_vec3(x[0] * 1e3, x[1] * 1e3, x[2] * 1e3));
        if (!strcmp(q, "failure_index")) {
            const double *sm = d.sm + 48 * (size_t)hits[i].e + 6 * (size_t)hits[i].g;
            json_set(o, "material_axes_stress_mpa", json_numbers((double[6]){sm[0] / 1e6, sm[1] / 1e6, sm[2] / 1e6, sm[3] / 1e6, sm[4] / 1e6, sm[5] / 1e6}, 6));
            if (have_rec && crit >= 0) {
                StrengthResult sr;
                strength_evaluate((StrengthCriterion)crit, &rec.strength, sm, &sr);
                json_set_string(o, "governing_mode", strength_mode_name(sr.mode));
                json_set_number(o, "strength_ratio", sr.ratio);
            }
        }
        json_push(arr, o);
    }
    json_set(v, "largest", arr);
    sd_free(&d);
    op_succeed(out, v);
}
