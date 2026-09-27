/* mech_struct.c - printed-material records and orthotropic structural assessment jobs (see mech_struct.h) */
#include "mech_struct.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/sha256.h"
#include "../fem/dense.h"
#include "../threads.h"
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

bool mech_struct_setup(const Project *p, const JsonValue *mm, MechStructSetup *s, OpResult *out) {
    memset(s, 0, sizeof *s);
    char err[400];
    const char *id = json_get_str(mm, "material", NULL);
    if (!id) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "material_model needs material: the id of a mech_material_define record", "material_model.material is missing");
        return false;
    }
    if (!mech_material_find(p, id, &s->mat, err, sizeof err)) {
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
    s->description = json_object();
    json_set_string(s->description, "type", "orthotropic");
    json_set_string(s->description, "material", id);
    json_set(s->description, "record", json_clone(s->mat.record));
    JsonValue *ax = json_set_object(s->description, "material_axes_build_frame");
    json_set(ax, "axis1", json_vec3(s->R[0], s->R[3], s->R[6]));
    json_set(ax, "axis2", json_vec3(s->R[1], s->R[4], s->R[7]));
    json_set(ax, "axis3_build_direction", json_vec3(s->R[2], s->R[5], s->R[8]));
    json_set_number(ax, "raster_angle_deg", ang * 180 / M_PI);
    json_set_string(s->description, "criterion", s->mat.has_strength ? strength_criterion_name(s->criterion) : "none (no strengths in the record)");
    return true;
}

void mech_struct_setup_free(MechStructSetup *s) {
    mech_material_free(&s->mat);
    json_free(s->description);
    s->description = NULL;
}

void mech_struct_assumptions(JsonValue *a, const MechStructSetup *s) {
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

static void gp_position(const StaticModel *m, int e, int g, double x[3]) {
    double N[8], dN[8][3];
    hex8_shape(HEX8_XI[g][0] / sqrt(3), HEX8_XI[g][1] / sqrt(3), HEX8_XI[g][2] / sqrt(3), N, dN);
    x[0] = x[1] = x[2] = 0;
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) x[k] += N[a] * m->xyz[3 * (size_t)m->conn[8 * (size_t)e + a] + (size_t)k];
}

static bool wr(FILE *f, const void *p, size_t size, size_t n) { return n == 0 || fwrite(p, size, n, f) == n; }

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
