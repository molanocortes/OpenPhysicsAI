/* print_analysis.c - the layer-by-layer print as an engine analysis (see print_analysis.h) */
#include "print_analysis.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/errors.h"
#include "../core/paths.h"
#include "../core/units.h"
#include "../mech/fffprint.h"
#include "matlib.h"
#include "static_analysis.h"

static void issue(JsonValue *arr, NvErr code, const char *hint, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void issue(JsonValue *arr, NvErr code, const char *hint, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (arr) json_push(arr, nv_error_json(code, hint, "%s", msg));
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static void *dup_mem(const void *p, size_t n) {
    void *q = malloc(n ? n : 1);
    if (q && p && n) memcpy(q, p, n);
    return q;
}

bool print_parse_volume_rate(const JsonValue *v, double *si, char *err, size_t errlen) {
    if (v && v->type == JSON_NUMBER) {
        *si = v->u.number;
        return true;
    }
    if (!v || v->type != JSON_STRING) {
        snprintf(err, errlen, "expected a number in m^3/s or a string with a unit such as \"15 mm3/s\"");
        return false;
    }
    const char *s = v->u.string.ptr;
    char *end = NULL;
    double value = strtod(s, &end);
    if (end == s) {
        snprintf(err, errlen, "'%s' does not start with a number", s);
        return false;
    }
    while (*end == ' ') end++;
    if (!*end) {
        snprintf(err, errlen, "'%s' has no unit; a deposition rate needs one, for example mm3/s", s);
        return false;
    }
    /* slicers write mm3/s; the unit reader wants mm^3/s, so a digit right after a letter becomes an exponent */
    char unit[64];
    size_t k = 0;
    for (const char *q = end; *q && k + 2 < sizeof unit; q++) {
        if (*q >= '0' && *q <= '9' && k > 0 && ((unit[k - 1] >= 'a' && unit[k - 1] <= 'z') || (unit[k - 1] >= 'A' && unit[k - 1] <= 'Z'))) unit[k++] = '^';
        unit[k++] = *q;
    }
    unit[k] = 0;
    UnitExpr u;
    if (!unit_parse(unit, &u, err, errlen)) return false;
    if (u.exp[0] != 3 || u.exp[2] != -1 || u.exp[1] || u.exp[3] || u.exp[4]) {
        snprintf(err, errlen, "'%s' is not a volume per time", end);
        return false;
    }
    *si = value * u.factor;
    return true;
}

/* ------------------------------------------------------------------------------------------------ build */

static bool required_table(const MaterialRecord *r, int prop, const char *name, const char *id, JsonValue *errors) {
    if (r->prop[prop].n > 0) return true;
    issue(errors, NV_ERR_PRECONDITION, "material_define can add the property, or choose a filament record that has it",
          "material '%s' has no %s, which the print needs", id, name);
    return false;
}

bool print_case_build(Project *p, const PrintSettings *s, PrintCase **out, JsonValue *errors, JsonValue *warnings) {
    *out = NULL;
    char why[256];
    if (!project_mesh_current(p, why, sizeof why)) {
        issue(errors, NV_ERR_PRECONDITION, "run mesh_generate again, then start the print", "the mesh is stale: %s", why);
        return false;
    }
    if (s->provenance == PROV_COUNT) {
        issue(errors, NV_ERR_PRECONDITION, "state provenance: \"user\", \"inferred\" or \"calibrated\"",
              "the print process carries no provenance: the analysis does not guess where printer settings come from");
        return false;
    }
    PrintCase *pc = calloc(1, sizeof *pc);
    if (!pc) {
        issue(errors, NV_ERR_INTERNAL, NULL, "out of memory");
        return false;
    }
    pc->s = *s;
    ThermalCase *c = &pc->c;
    StaticSettings sset = {s->formulation, s->solver, s->pcg_tol, s->ambient_t, 0};
    /* a print carries no structural boundary conditions: the bed holds the part, so the builder's complaint about
     * missing supports is expected and only that one is tolerated */
    JsonValue *build_errors = json_array();
    if (!static_model_build(p, &sset, &c->mech, build_errors, warnings)) {
        bool only_supports = json_len(build_errors) > 0;
        for (size_t i = 0; i < json_len(build_errors); i++)
            only_supports &= !strcmp(json_get_str(json_at(build_errors, i), "code", ""), "INSUFFICIENT_CONSTRAINTS");
        if (!only_supports) {
            for (size_t i = 0; i < json_len(build_errors); i++) json_push(errors, json_clone(json_at(build_errors, i)));
            json_free(build_errors);
            static_model_free(&c->mech);
            free(pc);
            return false;
        }
    }
    json_free(build_errors);
    c->has_mech = true;
    StaticModel *m = &c->mech;
    bool ok = true;
    if (m->nbodies != 1) {
        issue(errors, NV_ERR_UNSUPPORTED, "print one part at a time: remove the other bodies or mesh them separately",
              "the mesh holds %d bodies; the print model prints one part on the plate", m->nbodies);
        ok = false;
    }
    if (m->nmat != 1) {
        issue(errors, NV_ERR_UNSUPPORTED, "assign one material to the printed body", "the mesh uses %d materials; the print model has one filament", m->nmat);
        ok = false;
    }
    const char *body = s->body[0] ? s->body : (m->nbodies == 1 ? m->body_name[0] : "");
    MaterialAssignment *ma = body[0] ? project_material_for(p, body) : NULL;
    if (!ma) {
        issue(errors, NV_ERR_PRECONDITION, "assign a material with material_assign", "body '%s' has no material assignment", body);
        ok = false;
    } else if (ma->source == PROV_DEFAULT) {
        issue(errors, NV_ERR_PRECONDITION, "assign the material with material_assign and state its source (user, inferred or calibrated)",
              "the material of '%s' carries the provenance 'default': the print does not run on a guessed filament", body);
        ok = false;
    }
    MaterialRecord rec;
    char merr[256];
    if (ok && !material_lookup(p->user_materials, m->mat_id[0], &rec, merr, sizeof merr)) {
        issue(errors, NV_ERR_NOT_FOUND, "material_list shows the records", "material '%s': %s", m->mat_id[0], merr);
        ok = false;
    }
    if (ok) {
        ok &= required_table(&rec, MATP_DENSITY, "density", m->mat_id[0], errors);
        ok &= required_table(&rec, MATP_CP, "specific heat", m->mat_id[0], errors);
        ok &= required_table(&rec, MATP_K, "thermal conductivity", m->mat_id[0], errors);
        ok &= required_table(&rec, MATP_E, "Young's modulus", m->mat_id[0], errors);
        ok &= required_table(&rec, MATP_ALPHA, "thermal expansion coefficient", m->mat_id[0], errors);
    }
    /* the part must rest on the build plate, whose top is z = 0: an element with a whole face on the lowest node plane,
     * and that plane within one element of the plate */
    double zmin = INFINITY, zmax = -INFINITY;
    for (int n = 0; ok && n < m->nnodes; n++) zmin = fmin(zmin, m->xyz[3 * n + 2]), zmax = fmax(zmax, m->xyz[3 * n + 2]);
    if (ok) {
        double tol = 1e-6 * fmax(zmax - zmin, 1e-3);
        int on_plate = 0;
        for (int e = 0; e < m->nelems; e++) {
            int nbed = 0;
            for (int a = 0; a < 8; a++) nbed += fabs(m->xyz[3 * (size_t)m->conn[8 * (size_t)e + (size_t)a] + 2] - zmin) < tol;
            on_plate += nbed >= 4;
        }
        if (s->supports) on_plate = 1; /* the supports stand on the plate */
        if (!s->supports && fabs(zmin) > 0.6 * m->h[2] + tol) {
            issue(errors, NV_ERR_PRECONDITION, "place the part on the plate with geometry_place (z_offset 0), or print the supports as part of the geometry",
                  "the meshed part starts %.2f mm above the build plate (z = 0): it has no bed face to be printed on", 1e3 * zmin);
            ok = false;
        } else if (!on_plate) {
            issue(errors, NV_ERR_PRECONDITION, "place a flat face on the plate (geometry_place), or print the supports as part of the geometry",
                  "no element has a whole face on the build plate at z = %.3f mm: the part touches it along an edge or a point only", 1e3 * zmin);
            ok = false;
        }
    }
    if (!ok) {
        static_model_free(&c->mech);
        free(pc);
        return false;
    }
    /* the case: the mesh and the material of the print, with the bed as a prescribed temperature */
    size_t nn = (size_t)m->nnodes, ne = (size_t)m->nelems, nf = (size_t)m->nfaces;
    c->nnodes = m->nnodes, c->nelems = m->nelems, c->nfaces = m->nfaces, c->nmesh_nodes = m->nnodes;
    c->xyz = dup_mem(m->xyz, 3 * nn * sizeof(double));
    c->conn = dup_mem(m->conn, 8 * ne * sizeof(int));
    c->elem_mat = dup_mem(m->elem_mat, ne * sizeof(int));
    c->elem_body = dup_mem(m->elem_body, ne);
    c->face_elem = dup_mem(m->face_elem, nf * sizeof(int));
    c->face_local = dup_mem(m->face_local, nf);
    c->fixed = calloc(nn, 1);
    c->fixed_T = malloc(nn * sizeof(double));
    c->elem_source = calloc(ne, sizeof(double));
    c->elem_birth = malloc(ne * sizeof(int));
    if (!c->xyz || !c->conn || !c->elem_mat || !c->elem_body || !c->face_elem || !c->face_local || !c->fixed || !c->fixed_T || !c->elem_source ||
        !c->elem_birth) {
        issue(errors, NV_ERR_INTERNAL, NULL, "out of memory building the print case");
        thermal_case_free(pc);
        return false;
    }
    double tol = 1e-6 * fmax(zmax - zmin, 1e-3);
    for (size_t e = 0; e < ne; e++) c->elem_birth[e] = -1;
    memcpy(c->h, m->h, sizeof c->h);
    if (s->supports) { /* generated and homogenised like the LPBF build's; the bed is then z = 0 */
        c->elem_group = calloc(ne, 1), c->elem_death = malloc(ne * sizeof(int));
        pc->sup_stats = calloc(1, sizeof *pc->sup_stats);
        if (!c->elem_group || !c->elem_death || !pc->sup_stats) {
            issue(errors, NV_ERR_INTERNAL, NULL, "out of memory for the supports");
            print_case_free(pc);
            return false;
        }
        for (size_t e = 0; e < ne; e++) c->elem_death[e] = -1;
        LpbfSettings ss = s->sup;
        ss.E = mat_eval(&rec.prop[MATP_E], s->ambient_t);
        ss.nu = rec.prop[MATP_NU].n ? mat_eval(&rec.prop[MATP_NU], 293.15) : 0.35;
        pc->sup_stats->s = ss;
        if (zmin < -tol) {
            issue(errors, NV_ERR_PRECONDITION, "place the part at or above the plate with geometry_place", "the part reaches below the plate");
            print_case_free(pc);
            return false;
        }
        int nsup = lpbf_supports_apply(p, c, &ss, pc->sup_stats, errors, warnings);
        if (nsup < 0) {
            print_case_free(pc);
            return false;
        }
        if (!nsup) json_push(warnings, nv_error_json(NV_ERR_PRECONDITION, NULL, "supports were asked for, and the part has no "
                                                                               "downward-facing surface above empty space: none were generated"));
        nn = (size_t)c->nnodes, ne = (size_t)c->nelems;
        zmin = 0;
    }
    for (size_t n = 0; n < nn; n++) {
        c->fixed[n] = fabs(c->xyz[3 * n + 2] - zmin) < tol;
        c->fixed_T[n] = s->bed_t;
    }
    memcpy(c->h, m->h, sizeof c->h);
    c->nbodies = m->nbodies;
    for (int i = 0; i < m->nbodies; i++) snprintf(c->body_name[i], sizeof c->body_name[i], "%s", m->body_name[i]);
    snprintf(c->mesh_hash, sizeof c->mesh_hash, "%s", m->mesh_hash);
    c->nmat = 1;
    c->rec[0] = rec;
    memset(&c->tmat[0], 0, sizeof c->tmat[0]);
    c->tmat[0].k = (ThermalTable){c->rec[0].prop[MATP_K].n, c->rec[0].prop[MATP_K].t, c->rec[0].prop[MATP_K].v};
    c->tmat[0].cp = (ThermalTable){c->rec[0].prop[MATP_CP].n, c->rec[0].prop[MATP_CP].t, c->rec[0].prop[MATP_CP].v};
    c->tmat[0].rho = (ThermalTable){c->rec[0].prop[MATP_DENSITY].n, c->rec[0].prop[MATP_DENSITY].t, c->rec[0].prop[MATP_DENSITY].v};
    snprintf(c->mat_id[0], sizeof c->mat_id[0], "%s", m->mat_id[0]);
    snprintf(c->mat_status[0], sizeof c->mat_status[0], "%s", m->mat_status[0]);
    snprintf(pc->material_id, sizeof pc->material_id, "%s", rec.id);
    snprintf(pc->material_status, sizeof pc->material_status, "%s", rec.status);
    snprintf(pc->material_name, sizeof pc->material_name, "%s", rec.name);
    pc->glass_transition_k = rec.glass_transition_k;
    pc->relaxation_k = (isfinite(rec.glass_transition_k) ? rec.glass_transition_k : 333.15) + s->relaxation_offset;
    pc->strength_pa = rec.prop[MATP_YIELD].n ? mat_eval(&rec.prop[MATP_YIELD], 293.15) : NAN;
    /* selections mapped onto the mesh, so result queries can restrict to a region */
    c->nsets = m->nsets;
    for (int i = 0; i < m->nsets; i++) {
        snprintf(c->set[i].name, sizeof c->set[i].name, "%s", m->set[i].name);
        c->set[i].nnodes = m->set[i].nnodes, c->set[i].nfaces = m->set[i].nfaces;
        c->set[i].nodes = dup_mem(m->set[i].nodes, (size_t)m->set[i].nnodes * sizeof(int));
        c->set[i].faces = dup_mem(m->set[i].faces, (size_t)m->set[i].nfaces * sizeof(int));
    }
    if (s->layer_height < 0.999 * m->h[2])
        json_push(warnings, nv_error_json(NV_ERR_PRECONDITION,
                                          "use a simulation layer of at least one element height, or mesh finer",
                                          "the simulation layer (%.2f mm) is thinner than an element (%.2f mm): layers that hold no element centroid "
                                          "deposit nothing and are skipped",
                                          1e3 * s->layer_height, 1e3 * m->h[2]));
    c->settings.mechanical = true;
    c->settings.initial_temperature = s->ambient_t;
    c->settings.reference_temperature = s->ambient_t;
    c->settings.theta = 1.0;
    c->settings.output_every = 1;
    c->settings.formulation = s->formulation;
    c->settings.solver = s->solver;
    c->settings.pcg_tol = s->pcg_tol;
    c->settings.stepping = THERMAL_STEPPING_FIXED;
    *out = pc;
    return true;
}

void print_case_free(void *vpc) {
    PrintCase *pc = vpc;
    if (!pc) return;
    json_free(pc->probes);
    pc->probes = NULL;
    if (pc->sup_stats) thermal_case_free(&pc->sup_stats->c); /* only its (empty) case: the statistics own nothing else */
    pc->sup_stats = NULL;
    thermal_case_free(&pc->c);
}

/* ------------------------------------------------------------------------------------------------ the run */

typedef struct Store {
    PrintCase *pc;
    Job *job;
    int cap;
    float *node_vm; /* scratch: nodal average of the element von Mises */
    int *node_cnt;
    /* probe histories at every thermal substep */
    double *ht;
    double *hT; /* nprobes per sample */
    int hn, hcap;
    int probe_node[PRINT_MAX_PROBES];
    int probe_elem[PRINT_MAX_PROBES][8], probe_nelem[PRINT_MAX_PROBES];
    char err[256];
    bool failed;
} Store;

static bool grow_outputs(Store *st, int need) {
    ThermalCase *c = &st->pc->c;
    if (need <= st->cap) return true;
    int cap = st->cap ? 2 * st->cap : 64;
    while (cap < need) cap *= 2;
    if (cap > TC_MAX_OUTPUTS) cap = TC_MAX_OUTPUTS;
    if (need > cap) {
        snprintf(st->err, sizeof st->err, "the print would store more than %d times; use store \"key_times\" or fewer layers", TC_MAX_OUTPUTS);
        return false;
    }
    size_t nn = (size_t)c->nnodes;
    double *times = realloc(c->times, (size_t)cap * sizeof(double));
    double *T = realloc(c->T, (size_t)cap * nn * sizeof(double));
    double *tmin = realloc(c->tmin, (size_t)cap * sizeof(double)), *tmax = realloc(c->tmax, (size_t)cap * sizeof(double));
    double *u = realloc(c->mech_u, 3 * (size_t)cap * nn * sizeof(double));
    double *vm = realloc(c->mech_vm, (size_t)cap * nn * sizeof(double));
    double *peak = realloc(c->mech_peak, (size_t)cap * sizeof(double)), *umax = realloc(c->mech_umax, (size_t)cap * sizeof(double));
    if (times) c->times = times;
    if (T) c->T = T;
    if (tmin) c->tmin = tmin;
    if (tmax) c->tmax = tmax;
    if (u) c->mech_u = u;
    if (vm) c->mech_vm = vm;
    if (peak) c->mech_peak = peak;
    if (umax) c->mech_umax = umax;
    if (!times || !T || !tmin || !tmax || !u || !vm || !peak || !umax) {
        snprintf(st->err, sizeof st->err, "out of memory storing %d times of %d nodes", cap, c->nnodes);
        return false;
    }
    st->cap = cap;
    return true;
}

static bool keep_frame(const FffFrame *f, void *ctx) {
    Store *st = ctx;
    PrintCase *pc = st->pc;
    ThermalCase *c = &pc->c;
    if (job_cancel_requested(st->job)) {
        snprintf(st->err, sizeof st->err, "cancelled");
        return false;
    }
    int i = c->noutputs;
    if (!grow_outputs(st, i + 1)) {
        st->failed = true;
        return false;
    }
    size_t nn = (size_t)c->nnodes, ne = (size_t)c->nelems;
    memcpy(c->T + (size_t)i * nn, f->T, nn * sizeof(double));
    memcpy(c->mech_u + 3 * (size_t)i * nn, f->u, 3 * nn * sizeof(double));
    memset(st->node_vm, 0, nn * sizeof(float));
    memset(st->node_cnt, 0, nn * sizeof(int));
    for (size_t e = 0; e < ne; e++) {
        if (!f->active[e]) continue;
        if (c->elem_birth[e] < 0) c->elem_birth[e] = i;
        for (int a = 0; a < 8; a++) {
            int n = c->conn[8 * e + (size_t)a];
            st->node_vm[n] += (float)f->vm[e], st->node_cnt[n]++;
        }
    }
    double tlo = INFINITY, thi = -INFINITY;
    for (size_t n = 0; n < nn; n++) {
        c->mech_vm[(size_t)i * nn + n] = st->node_cnt[n] ? st->node_vm[n] / st->node_cnt[n] : 0.0;
        if (st->node_cnt[n]) tlo = fmin(tlo, f->T[n]), thi = fmax(thi, f->T[n]);
    }
    c->times[i] = f->time;
    c->tmin[i] = isfinite(tlo) ? tlo : f->T[0];
    c->tmax[i] = isfinite(thi) ? thi : f->T[0];
    c->mech_peak[i] = f->vm_max;
    c->mech_umax[i] = f->u_max;
    c->noutputs = i + 1;
    c->settings.end_time = f->time;
    pc->worst_balance = fmax(pc->worst_balance, f->energy_balance);
    if (!strcmp(f->stage, "deposited")) pc->layers_deposited++;
    char stage[96];
    snprintf(stage, sizeof stage, "layer %d of %d: %s", f->layer + 1, f->nlayers, f->stage);
    job_progress(st->job, f->nlayers > 0 ? 0.97 * (f->layer + 1.0) / f->nlayers : 0.5, stage);
    return true;
}

static void keep_step(double time, const double *T, const unsigned char *active, void *ctx) {
    Store *st = ctx;
    PrintCase *pc = st->pc;
    if (pc->s.nprobes <= 0) return;
    if (st->hn == st->hcap) {
        int cap = st->hcap ? 2 * st->hcap : 256;
        double *ht = realloc(st->ht, (size_t)cap * sizeof(double));
        double *hT = realloc(st->hT, (size_t)cap * (size_t)pc->s.nprobes * sizeof(double));
        if (ht) st->ht = ht;
        if (hT) st->hT = hT;
        if (!ht || !hT) return;
        st->hcap = cap;
    }
    st->ht[st->hn] = time;
    for (int i = 0; i < pc->s.nprobes; i++) {
        bool on = false;
        for (int k = 0; k < st->probe_nelem[i] && !on; k++) on = active[st->probe_elem[i][k]] != 0;
        st->hT[(size_t)st->hn * (size_t)pc->s.nprobes + (size_t)i] = on ? T[st->probe_node[i]] : NAN;
    }
    st->hn++;
}

static void locate_probes(Store *st) {
    PrintCase *pc = st->pc;
    ThermalCase *c = &pc->c;
    for (int i = 0; i < pc->s.nprobes; i++) {
        int best = 0;
        double bd = INFINITY;
        for (int n = 0; n < c->nnodes; n++) {
            double d = 0;
            for (int k = 0; k < 3; k++) d += (c->xyz[3 * (size_t)n + (size_t)k] - pc->s.probe[i][k]) * (c->xyz[3 * (size_t)n + (size_t)k] - pc->s.probe[i][k]);
            if (d < bd) bd = d, best = n;
        }
        st->probe_node[i] = best;
        st->probe_nelem[i] = 0;
        for (int e = 0; e < c->nelems && st->probe_nelem[i] < 8; e++)
            for (int a = 0; a < 8; a++)
                if (c->conn[8 * (size_t)e + (size_t)a] == best) {
                    st->probe_elem[i][st->probe_nelem[i]++] = e;
                    break;
                }
    }
}

static JsonValue *probes_json(const Store *st) {
    const PrintCase *pc = st->pc;
    const ThermalCase *c = &pc->c;
    JsonValue *arr = json_array();
    for (int i = 0; i < pc->s.nprobes; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "label", pc->s.probe_label[i]);
        json_set(o, "requested_mm", json_vec3(1e3 * pc->s.probe[i][0], 1e3 * pc->s.probe[i][1], 1e3 * pc->s.probe[i][2]));
        int n = st->probe_node[i];
        json_set(o, "node_mm", json_vec3(1e3 * c->xyz[3 * (size_t)n], 1e3 * c->xyz[3 * (size_t)n + 1], 1e3 * c->xyz[3 * (size_t)n + 2]));
        json_set_int(o, "node", n);
        JsonValue *t = json_set_array(o, "time_s"), *v = json_set_array(o, "temperature_c");
        for (int k = 0; k < st->hn; k++) {
            double x = st->hT[(size_t)k * (size_t)pc->s.nprobes + (size_t)i];
            json_push(t, json_number(st->ht[k]));
            json_push(v, isfinite(x) ? json_number(x - 273.15) : json_null());
        }
        json_push(arr, o);
    }
    return arr;
}

JsonValue *print_model_json(const PrintCase *pc) {
    const ThermalCase *c = &pc->c;
    JsonValue *o = json_object();
    json_set_string(o, "analysis", "fff_print");
    json_set_int(o, "nodes", c->nnodes);
    json_set_int(o, "elements", c->nelems);
    json_set_string(o, "mesh_hash", c->mesh_hash);
    json_set(o, "element_size_mm", json_vec3(1e3 * c->h[0], 1e3 * c->h[1], 1e3 * c->h[2]));
    json_set_string(o, "body", c->nbodies > 0 ? c->body_name[0] : "");
    JsonValue *mat = json_set_object(o, "material");
    json_set_string(mat, "id", pc->material_id);
    json_set_string(mat, "name", pc->material_name);
    json_set_string(mat, "status", pc->material_status);
    if (isfinite(pc->glass_transition_k)) json_set_number(mat, "glass_transition_c", pc->glass_transition_k - 273.15);
    json_set_number(mat, "relaxation_c", pc->relaxation_k - 273.15);
    JsonValue *pr = json_set_object(o, "process");
    json_set_number(pr, "simulation_layer_mm", 1e3 * pc->s.layer_height);
    json_set_number(pr, "nozzle_c", pc->s.nozzle_t - 273.15);
    json_set_number(pr, "bed_c", pc->s.bed_t - 273.15);
    json_set_number(pr, "ambient_c", pc->s.ambient_t - 273.15);
    json_set_number(pr, "convection_w_m2k", pc->s.h_conv);
    json_set_bool(pr, "radiation", pc->s.radiation);
    json_set_number(pr, "deposition_rate_mm3_s", 1e9 * pc->s.deposition_rate);
    json_set_number(pr, "min_layer_time_s", pc->s.min_layer_time);
    json_set_number(pr, "cooldown_bed_on_s", pc->s.cooldown_bed_on);
    json_set_number(pr, "cooldown_bed_off_s", pc->s.cooldown_bed_off);
    json_set_int(pr, "thermal_substeps_per_layer", pc->s.thermal_substeps);
    json_set_string(pr, "provenance", provenance_name(pc->s.provenance));
    json_set_string(pr, "store", pc->s.store_substeps ? "substeps" : "key_times");
    JsonValue *nu = json_set_object(o, "numerics");
    json_set_string(nu, "solver", pc->s.solver == SOLID_SOLVER_DIRECT ? "direct" : (pc->s.solver == SOLID_SOLVER_PCG ? "iterative" : "auto"));
    json_set_number(nu, "tolerance", pc->s.pcg_tol);
    json_set_number(nu, "skip_increment_below_k", pc->s.skip_below_k);
    json_set_string(nu, "note",
                    "auto factorises directly while the factor fits and falls back to the iterative solver when it does not; the iterative solver is "
                    "faster on smaller models (measured on the truss bridge at 4 mm elements: 220 s automatic against 98 s iterative at a tolerance of "
                    "1e-8, peak stress within 2.6e-8 and warp within 3.7e-6 relative)");
    return o;
}

JsonValue *print_summary_json(const PrintCase *pc) {
    const ThermalCase *c = &pc->c;
    JsonValue *o = json_object();
    json_set_string(o, "analysis", "fff_print");
    json_set(o, "model", print_model_json(pc));
    JsonValue *dep = json_set_object(o, "deposition");
    json_set_int(dep, "simulation_layers", pc->layers);
    json_set_int(dep, "layers_deposited", pc->layers_deposited);
    json_set_int(dep, "elements_printed", pc->printed_elements);
    json_set_int(dep, "elements_deposited_late", pc->late_elements);
    json_set_int(dep, "elements_never_deposited", pc->never_deposited);
    json_set_string(dep, "rule", "material is deposited where it has a whole face on the bed or shares a face with printed material; an overhang waits "
                                 "for the layer that carries it, and a fragment that never connects is not printed");
    json_set_number(dep, "printed_volume_cm3", 1e6 * pc->volume);
    JsonValue *t = json_set_object(o, "timing");
    json_set_number(t, "print_time_h", pc->print_time / 3600);
    json_set_number(t, "total_machine_time_h", pc->total_time / 3600);
    json_set_number(t, "compute_seconds", pc->seconds_total);
    json_set_number(t, "compute_seconds_heat", pc->seconds_thermal);
    json_set_number(t, "compute_seconds_stress", pc->seconds_stress);
    json_set_int(t, "thermal_steps", pc->thermal_steps);
    json_set_int(t, "stress_increments", pc->stress_increments);
    json_set_int(t, "increments_skipped", pc->skipped_increments);
    JsonValue *r = json_set_object(o, "results");
    json_set_number(r, "peak_von_mises_on_bed_mpa", pc->peak_bed / 1e6);
    json_set_number(r, "peak_von_mises_released_mpa", pc->peak_released / 1e6);
    json_set_number(r, "warp_z_min_mm", 1e3 * pc->warp_min);
    json_set_number(r, "warp_z_max_mm", 1e3 * pc->warp_max);
    json_set_number(r, "worst_heat_balance_relative", pc->worst_balance);
    json_set_number(r, "largest_bed_reaction_n", pc->bed_reaction);
    json_set_number(r, "support_reaction_after_release_n", pc->release_reaction);
    json_set_number(r, "equilibrium_error_last_solve", pc->equilibrium_error_last_solve);
    json_set_number(r, "equilibrium_error_at_release", pc->equilibrium_error_at_release);
    json_set_string(r, "equilibrium_error_definition", "recovered free-equation residual norm / (applied nodal load norm + individual prescribed-DOF "
                                                      "reaction norm + assembled free RHS norm including eigenstrain); dimensionless for nonzero force "
                                                      "scale; zero scale reports absolute residual in N, zero for an unloaded zero state");
    json_set_string(r, "bed_reaction_definition", "largest absolute component of the NET bed resultant, not the largest individual bed reaction");
    json_set_string(r, "release_check", "release equilibrium uses the normalized free-equation residual; support reactions remain in N and are not "
                                       "divided by the net bed resultant, which can vanish for self-equilibrated thermal loads");
    json_set_number(r, "heat_into_bed_j", pc->bed_heat);
    json_set_number(r, "heat_into_bed_during_print_j", pc->bed_heat_print);
    json_set_number(r, "supplied_nozzle_enthalpy_above_ambient_j", pc->deposition_heat);
    json_set_number(r, "deposition_enthalpy_correction_j", pc->deposition_correction);
    json_set_number(r, "stored_enthalpy_above_ambient_j", pc->stored_heat);
    json_set_number(r, "heat_into_air_j", pc->air_heat);
    json_set_number(r, "enthalpy_removed_with_supports_j", pc->removed_heat);
    json_set_number(r, "worst_deposition_balance_relative", pc->deposition_balance);
    json_set_number(r, "whole_print_heat_balance_relative", pc->global_heat_balance);
    json_set_string(r, "stress_field_definition", "element mean of eight Gauss-point von Mises values, not a Gauss-point maximum");
    if (pc->sup_stats && pc->sup_stats->support_elements > 0) {
        JsonValue *sp = lpbf_supports_json(pc->sup_stats);
        json_set_int(sp, "removed_after_release", pc->supports_removed);
        JsonValue *to = json_set_object(sp, "tear_off");
        json_set_number(to, "largest_nodal_force_n", pc->tearoff_max);
        json_set_number(to, "sum_of_nodal_force_magnitudes_n", pc->tearoff_sum);
        json_set_string(to, "definition", "the forces the supports exerted on the part at the nodes they share with it, just "
                                          "before their removal after the bed release");
        json_set_string(sp, "heat_model", "each support band conducts with its stated conductivity fractions along x, y and "
                                          "z and stores heat with its stated capacity fraction; pattern surface per volume "
                                          "provides homogenised convection and radiation over its element faces");
        json_set(r, "supports", sp);
    }
    json_set_int(r, "stored_times", c->noutputs);
    json_set_string(r, "last_stored_time", "the state after release from the bed");
    if (isfinite(pc->strength_pa) && pc->strength_pa > 0) {
        json_set_number(r, "material_strength_mpa", pc->strength_pa / 1e6);
        json_set_number(r, "peak_on_bed_over_strength", pc->peak_bed / pc->strength_pa);
    }
    if (pc->probes) json_set(o, "probes", json_clone(pc->probes));
    /* the scope, machine-readable: what these numbers are and are not */
    JsonValue *sc = json_set_object(o, "scope");
    json_set_string(sc, "statement", "process simulation on demonstration material data; not a forecast for a particular printer or filament");
    json_set_bool(sc, "is_forecast", false);
    JsonValue *ll = json_set_object(sc, "layer_lumping");
    json_set_number(ll, "simulation_layer_mm", 1e3 * pc->s.layer_height);
    json_set_number(ll, "printed_layer_mm", 1e3 * pc->s.printed_layer_height);
    json_set_number(ll, "printed_layers_per_simulation_layer", pc->s.printed_layer_height > 0 ? pc->s.layer_height / pc->s.printed_layer_height : 0);
    json_set_string(ll, "toolpath_within_a_layer", "not modelled: a simulation layer is activated at once with nozzle enthalpy; "
                                                "missing energy at conforming nodes is delivered in the first thermal substep");
    json_set_string(ll, "deposition_timing", "the enthalpy correction is a finite heat pulse during the first substep; "
                                         "local deposition temperatures require time refinement");
    json_set_string(sc, "creep_below_relaxation_temperature", "not modelled");
    json_set_string(sc, "constitutive_law", "temperature-dependent incremental stress accumulation (hypoelastic approximation); "
                    "old stress is not rescaled when the modulus changes, and stress is reset above the relaxation temperature; "
                    "no time-dependent viscoelastic law");
    json_set_bool(sc, "bed_stresses_are_upper_bound", true);
    json_set_string(sc, "bed_stresses_note",
                    "the part is held on the bed near the glass transition for the whole print; without creep the stress that builds up there is an "
                    "upper bound");
    JsonValue *mj = json_set_object(sc, "material");
    json_set_string(mj, "id", pc->material_id);
    json_set_string(mj, "status", pc->material_status);
    json_set_bool(mj, "measured", !strcmp(pc->material_status, "measured"));
    json_set_bool(sc, "compared_with_measurement", false);
    JsonValue *nm = json_set_array(sc, "not_modelled");
    static const char *const NOT[] = {"toolpath within a layer",  "creep below the relaxation temperature",
                                      "plasticity",               "raster anisotropy and interlayer strength",
                                      "crystallisation shrinkage", "resolved roads and inter-road thermal contact resistance",
                                      "gravity",                  "adhesion failure during the print"};
    for (size_t i = 0; i < sizeof NOT / sizeof NOT[0]; i++) json_push(nm, json_string(NOT[i]));
    json_set_string(sc, "use", "compare designs, process settings and trends; do not certify a part with these numbers");
    return o;
}

bool print_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    PrintCase *pc = data;
    ThermalCase *c = &pc->c;
    Store st;
    memset(&st, 0, sizeof st);
    st.pc = pc, st.job = job;
    st.node_vm = malloc((size_t)c->nnodes * sizeof(float));
    st.node_cnt = malloc((size_t)c->nnodes * sizeof(int));
    if (!st.node_vm || !st.node_cnt) {
        free(st.node_vm), free(st.node_cnt);
        snprintf(code, codelen, "INTERNAL");
        snprintf(err, errlen, "out of memory");
        return false;
    }
    locate_probes(&st);
    const MaterialRecord *rec = &c->rec[0];
    FffMaterial mat;
    memset(&mat, 0, sizeof mat);
    mat.rho = rec->prop[MATP_DENSITY], mat.cp = rec->prop[MATP_CP], mat.k = rec->prop[MATP_K];
    mat.E = rec->prop[MATP_E], mat.alpha = rec->prop[MATP_ALPHA], mat.emissivity = rec->prop[MATP_EMISSIVITY];
    mat.nu = rec->prop[MATP_NU].n ? mat_eval(&rec->prop[MATP_NU], 293.15) : 0.35;
    mat.T_relax = pc->relaxation_k;
    mat.E_floor = 1e6;
    FffProcess proc = {pc->s.layer_height,     pc->s.nozzle_t,         pc->s.bed_t,            pc->s.ambient_t,
                       pc->s.h_conv,           pc->s.radiation,        pc->s.deposition_rate,  pc->s.min_layer_time,
                       pc->s.cooldown_bed_on,  pc->s.cooldown_bed_off, pc->s.thermal_substeps, pc->s.store_substeps,
                       (int)pc->s.solver,      pc->s.pcg_tol,          pc->s.skip_below_k};
    FffMesh mesh = {c->nnodes, c->nelems, c->xyz, c->conn, c->support_nbands > 0 ? c->elem_band : NULL, c->support_nbands,
                    c->support_band_props};
    FffCallbacks cb = {keep_frame, keep_step, &st};
    FffSummary sum;
    double t0 = now_s();
    job_progress(job, 0.01, "planning the deposition");
    bool ok = fff_simulate(&mesh, &mat, &proc, &cb, &sum, err, errlen);
    free(st.node_vm), free(st.node_cnt);
    if (!ok) {
        free(st.ht), free(st.hT);
        if (job_cancel_requested(job)) {
            snprintf(code, codelen, "CANCELLED");
            snprintf(err, errlen, "cancelled after %d of the stored times", c->noutputs);
        } else {
            snprintf(code, codelen, st.failed ? "RESOURCE" : "SOLVER_FAILED");
        }
        return false;
    }
    pc->layers = sum.nlayers, pc->late_elements = sum.late_elements, pc->never_deposited = sum.unprintable_elements;
    pc->printed_elements = c->nelems - sum.unprintable_elements;
    pc->print_time = sum.print_time, pc->total_time = sum.total_time;
    pc->peak_bed = sum.peak_vm_bed, pc->peak_released = sum.peak_vm_released;
    pc->warp_min = sum.warp_z_min, pc->warp_max = sum.warp_z_max;
    pc->bed_reaction = sum.bed_reaction_total, pc->release_reaction = sum.release_support_reaction;
    pc->equilibrium_error_last_solve = sum.equilibrium_error_last_solve;
    pc->equilibrium_error_at_release = sum.equilibrium_error_at_release;
    pc->worst_balance = fmax(pc->worst_balance, sum.worst_energy_balance);
    pc->thermal_steps = sum.thermal_steps, pc->stress_increments = sum.mech_solves, pc->skipped_increments = sum.skipped_increments;
    pc->seconds_thermal = sum.seconds_thermal, pc->seconds_stress = sum.seconds_mech;
    pc->seconds_total = now_s() - t0;
    pc->volume = sum.printed_volume;
    pc->bed_heat = sum.bed_heat, pc->bed_heat_print = sum.bed_heat_print;
    pc->deposition_heat = sum.deposition_heat, pc->deposition_correction = sum.deposition_correction;
    pc->stored_heat = sum.stored_heat, pc->air_heat = sum.air_heat, pc->removed_heat = sum.removed_heat;
    pc->deposition_balance = sum.deposition_balance, pc->global_heat_balance = sum.global_heat_balance;
    pc->tearoff_max = sum.tearoff_max, pc->tearoff_sum = sum.tearoff_sum, pc->supports_removed = sum.support_elements;
    c->nsteps = sum.thermal_steps;
    c->worst_balance = pc->worst_balance;
    pc->probes = st.hn ? probes_json(&st) : NULL;
    free(st.ht), free(st.hT);
    json_free(c->summary);
    c->summary = print_summary_json(pc);
    job_progress(job, 0.99, "writing the results");
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, c->run_dir, "results.nvt");
    if (!thermal_results_save(c, path, err, errlen)) {
        snprintf(code, codelen, "IO");
        return false;
    }
    path_join(path, sizeof path, c->run_dir, "summary.json");
    if (!json_write_file(path, c->summary, JSON_PRETTY)) {
        snprintf(code, codelen, "IO");
        snprintf(err, errlen, "cannot write %s", path);
        return false;
    }
    job_set_summary(job, json_clone(c->summary));
    job_progress(job, 1.0, "done");
    return true;
}
