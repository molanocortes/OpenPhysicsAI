/* ops_results.c - queries, probes, images and exports of analysis results (no engine lock: results are immutable) */
#include "../core/sha256.h"
#include "../fem/dense.h"
#include "../fem/hex8.h"
#include "ops_internal.h"
#include "selection.h"
#include "static_analysis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pins the results of a succeeded job, loading them from its run directory when they are not in memory */
static StaticResults *acquire(Engine *e, const char *id, OpResult *out) {
    JobState st = JOB_QUEUED;
    void *data = NULL;
    char dir[NV_PATH_MAX] = "", kind[32] = "";
    bool known = jobs_acquire(e->jobs, id, &st, &data, kind, sizeof kind, dir, sizeof dir);
    if (known && kind[0] && strcmp(kind, "static_structural") != 0) {
        if (data) jobs_release(e->jobs, id);
        op_fail(out, NV_ERR_UNSUPPORTED, "use the transient result operations for this job", "job '%s' is a %s analysis", id, kind);
        return NULL;
    }
    if (data) return data;
    if (known && st != JOB_SUCCEEDED) {
        op_fail(out, NV_ERR_PRECONDITION,
                st == JOB_QUEUED || st == JOB_RUNNING ? "wait with job_status {\"wait_seconds\": 30}" : "job_status shows why it did not finish",
                "job '%s' is %s and has no results", id, job_state_name(st));
        return NULL;
    }
    if (!known && !op_project_run_dir(e, id, dir, sizeof dir)) {
        op_fail(out, NV_ERR_NOT_FOUND, "job_list shows the jobs of this session", "no job '%s' in this session or in the runs folder of the open project", id);
        return NULL;
    }
    char path[NV_PATH_MAX], err[700];
    path_join(path, sizeof path, dir, "results.nvr");
    StaticResults *r = static_results_load(path, err, sizeof err);
    if (!r) {
        op_fail(out, NV_ERR_IO, NULL, "cannot load the results of '%s': %s", id, err);
        return NULL;
    }
    snprintf(r->run_dir, sizeof r->run_dir, "%s", dir);
    if (!r->job_id[0]) snprintf(r->job_id, sizeof r->job_id, "%s", id);
    StaticResults *use = jobs_attach(e->jobs, id, "static_structural", dir, r, static_results_free, r->summary ? json_clone(r->summary) : NULL);
    if (!use) op_fail(out, NV_ERR_BUSY, NULL, "cannot register the results of '%s'", id);
    return use;
}

typedef enum { Q_DISPLACEMENT, Q_VON_MISES, Q_STRESS, Q_PRINCIPAL, Q_STRAIN } Quantity;

typedef struct {
    Quantity q;
    int comp; /* displacement 0-2 or 3 = magnitude; stress/strain 0-5; principal 0 max, 1 mid, 2 min */
    bool gauss;
    bool is_signed;
    const char *unit;
    double scale; /* SI -> display unit */
    char label[96];
} QuantitySpec;

static const char *const TENSOR_COMP[6] = {"xx", "yy", "zz", "xy", "yz", "zx"};

static bool parse_quantity(OpResult *out, const JsonValue *p, QuantitySpec *qs, bool allow_gauss) {
    memset(qs, 0, sizeof *qs);
    const char *q = json_get_str(p, "quantity", "von_mises"), *c = json_get_str(p, "component", NULL);
    qs->gauss = !strcmp(json_get_str(p, "value_kind", "nodal_average"), "gauss_point");
    qs->comp = -1;
    if (!strcmp(q, "displacement")) {
        qs->q = Q_DISPLACEMENT, qs->unit = "mm", qs->scale = 1e3;
        qs->comp = !c || !strcmp(c, "magnitude") ? 3 : (!strcmp(c, "x") ? 0 : (!strcmp(c, "y") ? 1 : (!strcmp(c, "z") ? 2 : -1)));
        qs->is_signed = qs->comp < 3;
        snprintf(qs->label, sizeof qs->label, "displacement %s", qs->comp == 3 ? "magnitude" : (c ? c : ""));
        if (qs->gauss) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "omit value_kind", "displacement is a nodal quantity");
            return false;
        }
    } else if (!strcmp(q, "von_mises")) {
        qs->q = Q_VON_MISES, qs->unit = "MPa", qs->scale = 1e-6, qs->comp = c ? -1 : 0;
        snprintf(qs->label, sizeof qs->label, "von Mises stress");
    } else if (!strcmp(q, "stress") || !strcmp(q, "strain")) {
        bool stress = !strcmp(q, "stress");
        qs->q = stress ? Q_STRESS : Q_STRAIN, qs->unit = stress ? "MPa" : "mm/mm", qs->scale = stress ? 1e-6 : 1, qs->is_signed = true;
        for (int k = 0; c && k < 6; k++)
            if (!strcmp(c, TENSOR_COMP[k])) qs->comp = k;
        if (!c) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "component: xx, yy, zz, xy, yz or zx", "%s needs a component", q);
            return false;
        }
        if (!stress && !qs->gauss) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "pass value_kind: gauss_point", "strain is evaluated at Gauss points (engineering shear strains)");
            return false;
        }
        snprintf(qs->label, sizeof qs->label, "%s %s", q, c);
    } else if (!strcmp(q, "principal_stress")) {
        qs->q = Q_PRINCIPAL, qs->unit = "MPa", qs->scale = 1e-6, qs->is_signed = true;
        qs->comp = !c || !strcmp(c, "max") ? 0 : (!strcmp(c, "mid") ? 1 : (!strcmp(c, "min") ? 2 : -1));
        snprintf(qs->label, sizeof qs->label, "principal stress (%s)", c ? c : "max");
    } else {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "unknown quantity '%s'", q);
        return false;
    }
    if (qs->comp < 0) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "see the component description in the schema", "component '%s' does not apply to %s", c ? c : "", q);
        return false;
    }
    if (qs->gauss && !allow_gauss) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "omit value_kind", "images show nodal averages");
        return false;
    }
    return true;
}

static double node_value(const StaticResults *r, const QuantitySpec *qs, int nd) {
    const double *u = r->sol.u + 3 * (size_t)nd, *s = r->sol.node_stress + 6 * (size_t)nd;
    double ev[3];
    switch (qs->q) {
    case Q_DISPLACEMENT: return qs->comp == 3 ? sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) : u[qs->comp];
    case Q_VON_MISES: return r->node_vm[nd];
    case Q_STRESS: return s[qs->comp];
    case Q_PRINCIPAL: sym3_eigenvalues(s, ev); return ev[qs->comp];
    default: return NAN;
    }
}

static double gauss_value(const StaticResults *r, const QuantitySpec *qs, int e, int g) {
    const double *s = r->sol.gp_stress + 6 * ((size_t)sa_ngp(&r->model) * e + (size_t)g);
    double ev[3], eps[6];
    switch (qs->q) {
    case Q_VON_MISES: return von_mises(s);
    case Q_STRESS: return s[qs->comp];
    case Q_PRINCIPAL: sym3_eigenvalues(s, ev); return ev[qs->comp];
    case Q_STRAIN: static_strain_from_stress(&r->model.mat[r->model.elem_mat[e]], s, eps); return eps[qs->comp];
    default: return NAN;
    }
}

static bool node_supported(const StaticModel *m, int nd) {
    size_t q = 3 * (size_t)nd;
    return m->fixed[q] || m->fixed[q + 1] || m->fixed[q + 2];
}

static bool elem_supported(const StaticModel *m, int e) {
    int npe = sa_npe(m);
    for (int a = 0; a < npe; a++)
        if (node_supported(m, m->conn[(size_t)npe * e + a])) return true;
    return false;
}

static void gauss_point_position(const StaticModel *m, int e, int g, double x[3]) {
    if (m->elem_type) {
        double L[4][4], w[4], N[10], X[10][3];
        tet_gauss_points(m->elem_type, L, w);
        tet_shape(m->elem_type, L[g], N, NULL);
        sa_elem_coords(m, e, X);
        x[0] = x[1] = x[2] = 0;
        for (int a = 0; a < sa_npe(m); a++)
            for (int k = 0; k < 3; k++) x[k] += N[a] * X[a][k];
        return;
    }
    double N[8], dN[8][3];
    const double r3 = 1 / sqrt(3.0);
    hex8_shape(HEX8_XI[g][0] * r3, HEX8_XI[g][1] * r3, HEX8_XI[g][2] * r3, N, dN);
    x[0] = x[1] = x[2] = 0;
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) x[k] += N[a] * m->xyz[3 * (size_t)m->conn[8 * (size_t)e + a] + k];
}

typedef struct {
    unsigned char *node_mask, *elem_mask;
    char desc[160];
} Scope;

static void scope_free(Scope *s) { free(s->node_mask), free(s->elem_mask); }

static bool parse_scope(OpResult *out, const StaticResults *r, const JsonValue *p, Scope *sc) {
    memset(sc, 0, sizeof *sc);
    const StaticModel *m = &r->model;
    const char *sel = json_get_str(p, "selection", NULL), *body = json_get_str(p, "body", NULL);
    if (sel && body) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "give selection or body, not both");
        return false;
    }
    if (!sel && !body) {
        snprintf(sc->desc, sizeof sc->desc, "whole model");
        return true;
    }
    sc->node_mask = calloc((size_t)m->nnodes, 1);
    sc->elem_mask = calloc((size_t)m->nelems, 1);
    if (!sc->node_mask || !sc->elem_mask) {
        scope_free(sc);
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return false;
    }
    if (sel) {
        const SaSet *set = NULL;
        for (int i = 0; i < m->nsets; i++)
            if (!strcmp(m->set[i].name, sel)) set = &m->set[i];
        if (!set) {
            scope_free(sc);
            op_fail(out, NV_ERR_NOT_FOUND, "use a selection that existed on the meshed geometry when the analysis ran",
                    "selection '%s' was not part of this analysis (created later, stale or not on the mesh)", sel);
            return false;
        }
        for (int i = 0; i < set->nnodes; i++) sc->node_mask[set->nodes[i]] = 1;
        for (int i = 0; i < set->nfaces; i++) sc->elem_mask[m->face_elem[set->faces[i]]] = 1;
        snprintf(sc->desc, sizeof sc->desc, "selection '%s' (%d surface nodes; Gauss points of the %d adjacent elements)", sel, set->nnodes, set->nfaces);
    } else {
        int bi = -1;
        for (int i = 0; i < m->nbodies; i++)
            if (!strcmp(m->body_name[i], body)) bi = i;
        if (bi < 0) {
            scope_free(sc);
            op_fail(out, NV_ERR_NOT_FOUND, NULL, "body '%s' is not part of this analysis", body);
            return false;
        }
        for (int e = 0; e < m->nelems; e++) {
            if (m->elem_body[e] != bi) continue;
            sc->elem_mask[e] = 1;
            for (int a = 0; a < sa_npe(m); a++) sc->node_mask[m->conn[(size_t)sa_npe(m) * e + a]] = 1;
        }
        snprintf(sc->desc, sizeof sc->desc, "body '%s'", body);
    }
    return true;
}

typedef struct {
    double v;
    int idx, gp;
} Sample;

static int cmp_sample_desc(const void *a, const void *b) {
    const Sample *x = a, *y = b;
    if (x->v != y->v) return x->v < y->v ? 1 : -1;
    if (x->idx != y->idx) return x->idx - y->idx;
    return x->gp - y->gp;
}

/* nodes of elements that touch a supported node: where support singularities dominate nodal averages */
static unsigned char *support_neighbourhood(const StaticModel *m) {
    unsigned char *near = calloc((size_t)(m->nnodes ? m->nnodes : 1), 1);
    for (int el = 0; near && el < m->nelems; el++)
        if (elem_supported(m, el))
            for (int a = 0; a < sa_npe(m); a++) near[m->conn[(size_t)sa_npe(m) * el + a]] = 1;
    return near;
}

static JsonValue *sample_json(const StaticResults *r, const QuantitySpec *qs, const Sample *s, const unsigned char *near) {
    const StaticModel *m = &r->model;
    JsonValue *o = json_object();
    json_set_number(o, "value", s->v);
    double x[3];
    int e;
    if (s->gp < 0) {
        memcpy(x, m->xyz + 3 * (size_t)s->idx, sizeof x);
        json_set_int(o, "node", s->idx);
        json_set_bool(o, "at_supported_node", node_supported(m, s->idx));
        if (near) json_set_bool(o, "next_to_support", near[s->idx] != 0);
        e = -1;
    } else {
        gauss_point_position(m, s->idx, s->gp, x);
        json_set_int(o, "element", s->idx);
        json_set_int(o, "gauss_point", s->gp);
        json_set_bool(o, "element_touches_support", elem_supported(m, s->idx));
        e = s->idx;
    }
    json_set(o, "location_mm", json_vec3(1e3 * x[0], 1e3 * x[1], 1e3 * x[2]));
    if (e >= 0 && m->elem_body[e] >= 0 && m->elem_body[e] < m->nbodies) json_set_string(o, "body", m->body_name[(int)m->elem_body[e]]);
    (void)qs;
    return o;
}

static void op_results_query(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    if (op_thermal_job(e, id)) {
        op_thermal_query(e, p, out);
        return;
    }
    QuantitySpec qs;
    if (!parse_quantity(out, p, &qs, true)) return;
    StaticResults *r = acquire(e, id, out);
    if (!r) return;
    const StaticModel *m = &r->model;
    Scope sc;
    if (!parse_scope(out, r, p, &sc)) {
        jobs_release(e->jobs, id);
        return;
    }
    int ngp = sa_ngp(m);
    size_t cap = qs.gauss ? (size_t)ngp * m->nelems : (size_t)m->nnodes, n = 0;
    Sample *smp = malloc((cap ? cap : 1) * sizeof *smp);
    if (!smp) {
        scope_free(&sc);
        jobs_release(e->jobs, id);
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    if (qs.gauss) {
        for (int el = 0; el < m->nelems; el++)
            if (!sc.elem_mask || sc.elem_mask[el])
                for (int g = 0; g < ngp; g++) smp[n++] = (Sample){qs.scale * gauss_value(r, &qs, el, g), el, g};
    } else {
        for (int nd = 0; nd < m->nnodes; nd++)
            if (!sc.node_mask || sc.node_mask[nd]) smp[n++] = (Sample){qs.scale * node_value(r, &qs, nd), nd, -1};
    }
    scope_free(&sc);
    if (!n) {
        free(smp);
        jobs_release(e->jobs, id);
        op_fail(out, NV_ERR_NOT_FOUND, NULL, "the scope contains no values");
        return;
    }
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += smp[i].v;
    qsort(smp, n, sizeof *smp, cmp_sample_desc);
    int top = (int)json_get_int(p, "top", 5);
    if ((size_t)top > n) top = (int)n;
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "quantity", json_get_str(p, "quantity", "von_mises"));
    json_set_string(v, "label", qs.label);
    json_set_string(v, "value_kind", qs.gauss ? "gauss_point" : "nodal_average");
    json_set_string(v, "unit", qs.unit);
    json_set_string(v, "scope", sc.desc);
    json_set_int(v, "count", (long long)n);
    JsonValue *st = json_set_object(v, "statistics");
    json_set_number(st, "min", smp[n - 1].v);
    json_set_number(st, "max", smp[0].v);
    json_set_number(st, "mean", sum / (double)n);
    static const double Q[5] = {0.01, 0.05, 0.50, 0.95, 0.99};
    static const char *const QN[5] = {"p01", "p05", "p50", "p95", "p99"};
    for (int k = 0; k < 5; k++) {
        double pos = (1 - Q[k]) * (double)(n - 1); /* samples are sorted in descending order */
        size_t i = (size_t)pos;
        double f = pos - (double)i, val = i + 1 < n ? smp[i].v * (1 - f) + smp[i + 1].v * f : smp[n - 1].v;
        json_set_number(st, QN[k], val);
    }
    unsigned char *near = qs.q != Q_DISPLACEMENT && !qs.gauss ? support_neighbourhood(m) : NULL;
    JsonValue *largest = json_set_array(v, "largest");
    for (int i = 0; i < top; i++) json_push(largest, sample_json(r, &qs, &smp[i], near));
    if (qs.is_signed) {
        JsonValue *smallest = json_set_array(v, "smallest");
        for (int i = 0; i < top; i++) json_push(smallest, sample_json(r, &qs, &smp[n - 1 - (size_t)i], near));
    }
    JsonValue *notes = json_set_array(v, "notes");
    if (qs.q != Q_DISPLACEMENT) {
        json_push(notes, json_string(qs.gauss ? "Gauss-point values are the element solution at its integration points, without smoothing."
                                              : "Nodal averages combine the Gauss-point stresses of all elements around a node, extrapolated to the node."));
        bool at_support = smp[0].gp < 0 ? (near ? near[smp[0].idx] != 0 : node_supported(m, smp[0].idx)) : elem_supported(m, smp[0].idx);
        if (at_support)
            json_push(notes, json_string("The largest value lies at or next to a support. Ideal supports make stresses singular there: the value grows "
                                         "with mesh refinement. Compare p99, or query a selection away from supports."));
        double loc[3];
        if (smp[0].gp < 0) memcpy(loc, m->xyz + 3 * (size_t)smp[0].idx, sizeof loc);
        else gauss_point_position(m, smp[0].idx, smp[0].gp, loc);
        if (static_distance_to_reentrant_edge(m, loc) < 1.5 * fmax(m->h[0], fmax(m->h[1], m->h[2])))
            json_push(notes, json_string("The largest value lies at a sharp inside corner of the geometry, a stress singularity in linear elasticity: it "
                                         "grows with mesh refinement, and real parts have fillets. Judge stresses away from the corner."));
    }
    free(near);
    free(smp);
    jobs_release(e->jobs, id);
    op_succeed(out, v);
}

static bool natural_coords(const double X[8][3], const double x[3], double xi[3]) {
    xi[0] = xi[1] = xi[2] = 0;
    for (int it = 0; it < 25; it++) {
        double N[8], dN[8][3], J[3][3], dNdx[8][3], f[3] = {0, 0, 0};
        hex8_shape(xi[0], xi[1], xi[2], N, dN);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) f[k] += N[a] * X[a][k];
        for (int k = 0; k < 3; k++) f[k] -= x[k];
        hex8_jacobian(X, dN, J, dNdx);
        double Jm[9], Ji[9], det;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) Jm[3 * i + j] = J[i][j];
        if (!inv3(Jm, Ji, &det)) return false;
        double step = 0;
        for (int i = 0; i < 3; i++) {
            double d = -(Ji[3 * i] * f[0] + Ji[3 * i + 1] * f[1] + Ji[3 * i + 2] * f[2]);
            xi[i] += d;
            step = fmax(step, fabs(d));
        }
        if (step < 1e-12) break;
    }
    return fabs(xi[0]) <= 1 + 1e-9 && fabs(xi[1]) <= 1 + 1e-9 && fabs(xi[2]) <= 1 + 1e-9;
}

/* volume coordinates of x in a (possibly curved) tetrahedron, by Newton on (xi, eta, zeta); true when inside */
static bool tet_natural(int type, const double (*X)[3], const double x[3], double L[4]) {
    double xi[3] = {0.25, 0.25, 0.25};
    int n = tet_nodes(type);
    for (int it = 0; it < 25; it++) {
        double N[10], dN[10][3], J[3][3], f[3] = {0, 0, 0};
        L[0] = 1 - xi[0] - xi[1] - xi[2], L[1] = xi[0], L[2] = xi[1], L[3] = xi[2];
        tet_shape(type, L, N, dN);
        for (int a = 0; a < n; a++)
            for (int k = 0; k < 3; k++) f[k] += N[a] * X[a][k];
        for (int k = 0; k < 3; k++) f[k] -= x[k];
        tet_jacobian(type, X, (const double (*)[3])dN, J, NULL);
        double Jm[9], Ji[9], det;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) Jm[3 * i + j] = J[i][j];
        if (!inv3(Jm, Ji, &det)) return false;
        double step = 0;
        for (int i = 0; i < 3; i++) {
            double d = -(Ji[3 * i] * f[0] + Ji[3 * i + 1] * f[1] + Ji[3 * i + 2] * f[2]);
            xi[i] += d;
            step = fmax(step, fabs(d));
        }
        if (step < 1e-13) break;
    }
    L[0] = 1 - xi[0] - xi[1] - xi[2], L[1] = xi[0], L[2] = xi[1], L[3] = xi[2];
    return L[0] >= -1e-9 && L[1] >= -1e-9 && L[2] >= -1e-9 && L[3] >= -1e-9;
}

static void op_results_probe(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    if (op_thermal_job(e, id)) {
        op_thermal_probe(e, p, out);
        return;
    }
    StaticResults *r = acquire(e, id, out);
    if (!r) return;
    const StaticModel *m = &r->model;
    const JsonValue *pts = json_get(p, "points_mm");
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    JsonValue *arr = json_set_array(v, "probes");
    for (size_t i = 0; i < json_len(pts); i++) {
        double x[3];
        json_get_numbers(json_at(pts, i), x, 3);
        for (int k = 0; k < 3; k++) x[k] *= 1e-3;
        JsonValue *o = json_object();
        json_set(o, "point_mm", json_vec3(1e3 * x[0], 1e3 * x[1], 1e3 * x[2]));
        int found = -1, npe = sa_npe(m);
        double xi[3] = {0, 0, 0}, X[10][3], Lv[4] = {0, 0, 0, 0};
        for (int el = 0; el < m->nelems && found < 0; el++) {
            double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
            for (int a = 0; a < npe; a++)
                for (int k = 0; k < 3; k++) {
                    X[a][k] = m->xyz[3 * (size_t)m->conn[(size_t)npe * el + a] + k];
                    lo[k] = fmin(lo[k], X[a][k]), hi[k] = fmax(hi[k], X[a][k]);
                }
            double tol = 1e-9 * fmax(hi[0] - lo[0], 1e-12);
            if (x[0] < lo[0] - tol || x[0] > hi[0] + tol || x[1] < lo[1] - tol || x[1] > hi[1] + tol || x[2] < lo[2] - tol || x[2] > hi[2] + tol) continue;
            if (m->elem_type ? tet_natural(m->elem_type, (const double (*)[3])X, x, Lv) : natural_coords(X, x, xi)) found = el;
        }
        if (found < 0) {
            int best = 0;
            double bd = INFINITY;
            for (int nd = 0; nd < m->nnodes; nd++) {
                const double *q = m->xyz + 3 * (size_t)nd;
                double d = (q[0] - x[0]) * (q[0] - x[0]) + (q[1] - x[1]) * (q[1] - x[1]) + (q[2] - x[2]) * (q[2] - x[2]);
                if (d < bd) bd = d, best = nd;
            }
            json_set_bool(o, "inside", false);
            json_set(o, "nearest_node_mm", json_vec3(1e3 * m->xyz[3 * (size_t)best], 1e3 * m->xyz[3 * (size_t)best + 1], 1e3 * m->xyz[3 * (size_t)best + 2]));
            json_set_number(o, "distance_mm", 1e3 * sqrt(bd));
            json_set_string(o, "note", m->elem_type ? "the point is outside the meshed volume (the faceted mesh surface differs slightly from the STL "
                                                      "surface, and sharp edges are rounded at the cell scale)"
                                                    : "the point is outside the meshed volume (the voxel boundary differs from the STL surface)");
            json_push(arr, o);
            continue;
        }
        double N[10], dN[10][3], u[3] = {0, 0, 0}, s[6] = {0, 0, 0, 0, 0, 0};
        if (m->elem_type) tet_shape(m->elem_type, Lv, N, NULL);
        else hex8_shape(xi[0], xi[1], xi[2], N, dN);
        for (int a = 0; a < npe; a++) {
            int nd = m->conn[(size_t)npe * found + a];
            for (int k = 0; k < 3; k++) u[k] += N[a] * r->sol.u[3 * (size_t)nd + k];
            for (int k = 0; k < 6; k++) s[k] += N[a] * r->sol.node_stress[6 * (size_t)nd + k];
        }
        json_set_bool(o, "inside", true);
        json_set_int(o, "element", found);
        if (m->elem_body[found] >= 0 && m->elem_body[found] < m->nbodies) json_set_string(o, "body", m->body_name[(int)m->elem_body[found]]);
        if (m->elem_type) json_set(o, "volume_coordinates", json_numbers(Lv, 4));
        else json_set(o, "natural_coordinates", json_vec3(xi[0], xi[1], xi[2]));
        JsonValue *d = json_set_object(o, "displacement_mm");
        json_set_number(d, "x", 1e3 * u[0]);
        json_set_number(d, "y", 1e3 * u[1]);
        json_set_number(d, "z", 1e3 * u[2]);
        json_set_number(d, "magnitude", 1e3 * sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]));
        JsonValue *so = json_set_object(o, "stress_mpa_interpolated_nodal_average");
        for (int k = 0; k < 6; k++) json_set_number(so, TENSOR_COMP[k], 1e-6 * s[k]);
        double ev[3];
        sym3_eigenvalues(s, ev);
        json_set(o, "principal_stress_mpa", json_vec3(1e-6 * ev[0], 1e-6 * ev[1], 1e-6 * ev[2]));
        json_set_number(o, "von_mises_mpa", 1e-6 * von_mises(s));
        double gmin = INFINITY, gmax = 0;
        int ngp = sa_ngp(m);
        for (int g = 0; g < ngp; g++) {
            double vm = von_mises(r->sol.gp_stress + 6 * ((size_t)ngp * found + (size_t)g));
            gmin = fmin(gmin, vm), gmax = fmax(gmax, vm);
        }
        JsonValue *gp = json_set_object(o, "element_gauss_point_von_mises_mpa");
        json_set_number(gp, "min", 1e-6 * gmin);
        json_set_number(gp, "max", 1e-6 * gmax);
        json_set_bool(o, "element_touches_support", elem_supported(m, found));
        json_push(arr, o);
    }
    jobs_release(e->jobs, id);
    op_succeed(out, v);
}

static double nice_scale(double s) {
    if (!(s > 0)) return 0;
    double p = pow(10, floor(log10(s))), mant = s / p;
    return (mant >= 5 ? 5 : (mant >= 2 ? 2 : 1)) * p;
}

static void op_results_render(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    if (op_thermal_job(e, id)) {
        op_thermal_render(e, p, out);
        return;
    }
    QuantitySpec qs;
    if (!parse_quantity(out, p, &qs, false)) return;
    ViewOptions o;
    if (!op_parse_view(out, p, &o)) return;
    StaticResults *r = acquire(e, id, out);
    if (!r) return;
    const StaticModel *m = &r->model;
    double *val = malloc((size_t)(m->nnodes ? m->nnodes : 1) * sizeof(double));
    if (!val) {
        jobs_release(e->jobs, id);
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    double lo = INFINITY, hi = -INFINITY, umax = 0, blo[3] = {INFINITY, INFINITY, INFINITY}, bhi[3] = {-INFINITY, -INFINITY, -INFINITY};
    int imax = 0;
    for (int nd = 0; nd < m->nnodes; nd++) {
        val[nd] = qs.scale * node_value(r, &qs, nd);
        if (val[nd] > hi) hi = val[nd], imax = nd;
        lo = fmin(lo, val[nd]);
        const double *u = r->sol.u + 3 * (size_t)nd;
        umax = fmax(umax, sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]));
        for (int k = 0; k < 3; k++) blo[k] = fmin(blo[k], m->xyz[3 * (size_t)nd + k]), bhi[k] = fmax(bhi[k], m->xyz[3 * (size_t)nd + k]);
    }
    const char *cm = json_get_str(p, "colormap", qs.is_signed ? "coolwarm" : "turbo");
    SwColormap cmap = sw_colormap_find(cm) >= 0 ? (SwColormap)sw_colormap_find(cm) : SW_CMAP_TURBO; /* the renderer's own names */
    const JsonValue *range = json_get(p, "range");
    double rg[2];
    if (range && json_get_numbers(range, rg, 2)) {
        if (!(rg[1] > rg[0])) {
            free(val);
            jobs_release(e->jobs, id);
            op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "range must be [min, max] with min < max");
            return;
        }
        lo = rg[0], hi = rg[1];
    } else if (qs.is_signed && cmap == SW_CMAP_COOLWARM) {
        double a = fmax(fabs(lo), fabs(hi));
        lo = -a, hi = a;
    }
    double size = sqrt((bhi[0] - blo[0]) * (bhi[0] - blo[0]) + (bhi[1] - blo[1]) * (bhi[1] - blo[1]) + (bhi[2] - blo[2]) * (bhi[2] - blo[2]));
    const JsonValue *ds = json_get(p, "deformation_scale");
    bool autoscale = !ds || (json_str(ds) && !strcmp(json_str(ds), "auto"));
    double scale = autoscale ? (umax > 0 ? fmax(1, nice_scale(0.08 * size / umax)) : 0) : (ds->type == JSON_NUMBER ? ds->u.number : 0);
    char title[256], legend[96], marker[64];
    if (o.title[0]) snprintf(title, sizeof title, "%s", o.title);
    else if (scale > 0) snprintf(title, sizeof title, "%s (nodal average), deformation x%g", qs.label, scale);
    else snprintf(title, sizeof title, "%s (nodal average), undeformed", qs.label);
    snprintf(legend, sizeof legend, "%s", qs.label);
    snprintf(marker, sizeof marker, "max %.4g %s", val[imax], qs.unit);
    /* a body may be asked for as glass, so a body inside an assembly can be shown through the others */
    float opacity[MESH_MAX_BODIES];
    for (int i = 0; i < MESH_MAX_BODIES; i++) opacity[i] = 1.0f;
    bool any_glass = false;
    const JsonValue *bo = json_get(p, "body_opacity");
    for (size_t i = 0; bo && bo->type == JSON_OBJECT && i < bo->u.object.len; i++) {
        const char *name = bo->u.object.keys[i];
        double a = json_get_num(bo, name, 1.0);
        for (int b = 0; b < m->nbodies; b++)
            if (!strcmp(m->body_name[b], name)) {
                opacity[b] = (float)(a < 0.05 ? 0.05 : (a > 1 ? 1 : a));
                any_glass = any_glass || opacity[b] < 0.999f;
            }
    }
    FieldView fv = {m->xyz, r->sol.u, m->conn, m->face_elem, m->face_local, m->nfaces, val, lo, hi, scale, json_get_bool(p, "show_undeformed", true) && scale > 0, cmap,
                    title, legend, qs.unit, m->xyz + 3 * (size_t)imax, marker, m->elem_type,
                    m->elem_body, any_glass ? opacity : NULL, m->nbodies};
    ViewResult vr;
    char err[512];
    bool ok = view_render_field(&fv, &o, &vr, err, sizeof err);
    double vmax = val[imax];
    free(val);
    if (!ok) {
        jobs_release(e->jobs, id);
        op_fail(out, NV_ERR_PRECONDITION, NULL, "%s", err);
        return;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "quantity", qs.label);
    json_set_string(v, "value_kind", "nodal_average");
    JsonValue *lg = json_set_object(v, "legend");
    json_set_number(lg, "min", lo);
    json_set_number(lg, "max", hi);
    json_set_string(lg, "unit", qs.unit);
    json_set_string(lg, "colormap", cm);
    json_set_number(v, "max_value", vmax);
    json_set(v, "max_value_at_mm", json_vec3(1e3 * m->xyz[3 * (size_t)imax], 1e3 * m->xyz[3 * (size_t)imax + 1], 1e3 * m->xyz[3 * (size_t)imax + 2]));
    JsonValue *df = json_set_object(v, "deformation");
    json_set_number(df, "scale", scale);
    json_set_string(df, "scale_source", autoscale ? "auto" : "user");
    json_set_number(df, "max_displacement_mm", 1e3 * umax);
    json_set_string(df, "note", scale == 1 ? "displacements drawn at true scale"
                                           : (scale > 1 ? "displacements are magnified for visibility; the part does not deform this much" : "undeformed shape"));
    json_set_string(v, "caution", "A colour image is not evidence of correctness: check the summary's equilibrium and energy checks and the warnings.");
    engine_lock(e);
    op_finish_view(e, out, &vr, "result", id, "result.png", v);
    engine_unlock(e);
    jobs_release(e->jobs, id);
    op_succeed(out, v);
}

static void op_results_export(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    if (op_thermal_job(e, id)) {
        op_thermal_export(e, p, out);
        return;
    }
    StaticResults *r = acquire(e, id, out);
    if (!r) return;
    char dir[NV_PATH_MAX], err[700];
    const char *dest = json_get_str(p, "directory", NULL);
    if (dest) {
        if (!engine_resolve_write_path(e, NULL, dest, true, true, dir, sizeof dir, err, sizeof err)) {
            jobs_release(e->jobs, id);
            op_fail(out, NV_ERR_PERMISSION, NULL, "%s", err);
            return;
        }
    } else {
        snprintf(dir, sizeof dir, "%s", r->run_dir);
    }
    if (!dir[0]) {
        jobs_release(e->jobs, id);
        op_fail(out, NV_ERR_PRECONDITION, "pass directory", "the job has no run directory");
        return;
    }
    const JsonValue *formats = json_get(p, "formats");
    JsonValue *files = json_array();
    for (size_t i = 0; i < json_len(formats); i++) {
        const char *fmt = json_str(json_at(formats, i));
        const char *fname = !strcmp(fmt, "vtu") ? "results.vtu" : (!strcmp(fmt, "csv") ? "nodes.csv" : "summary.json");
        char path[NV_PATH_MAX];
        path_join(path, sizeof path, dir, fname);
        bool ok;
        err[0] = 0;
        if (!strcmp(fmt, "vtu")) ok = static_export_vtu(r, path, err, sizeof err);
        else if (!strcmp(fmt, "csv")) ok = static_export_csv(r, path, err, sizeof err);
        else ok = r->summary && json_write_file(path, r->summary, JSON_PRETTY | JSON_SORTED);
        if (!ok) {
            json_free(files);
            jobs_release(e->jobs, id);
            op_fail(out, NV_ERR_IO, NULL, "cannot export %s: %s", fname, err[0] ? err : "write failed");
            return;
        }
        char hex[65] = "";
        uint64_t bytes = 0;
        sha256_file(path, hex, &bytes);
        JsonValue *fo = json_object();
        json_set_string(fo, "format", fmt);
        json_set_string(fo, "path", path);
        json_set_int(fo, "bytes", (long long)bytes);
        json_set_string(fo, "sha256", hex);
        json_set_string(fo, "contents",
                        !strcmp(fmt, "vtu") ? "VTK XML unstructured grid, appended raw binary: per node displacement_m, stress_nodal_average_pa, "
                                              "von_mises_nodal_average_pa, reaction_force_n, prescribed_displacement; per element "
                                              "von_mises_gauss_point_max_pa, stress_gauss_point_mean_pa, body, material. SI units, build frame."
                        : !strcmp(fmt, "csv") ? "one row per node: coordinates and displacements in mm, stresses in MPa, reactions in N"
                                              : "analysis summary: model, solver, checks, reactions, loads, warnings, interpretation");
        json_push(files, fo);
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "directory", dir);
    json_set(v, "files", files);
    jobs_release(e->jobs, id);
    op_succeed(out, v);
}

static void op_results_quantities(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    if (op_thermal_job(e, id)) {
        op_fail(out, NV_ERR_UNSUPPORTED, "use results_query and results_interface for transient runs",
                "results_quantities evaluates static_structural results; job '%s' is a transient analysis", id);
        return;
    }
    StaticResults *r = acquire(e, id, out);
    if (!r) return;
    const StaticModel *m = &r->model;
    char err[600];
    double about[3], *ap = NULL;
    const JsonValue *ab = json_get(p, "about");
    if (ab) {
        if (!json_get_numbers(ab, about, 3)) {
            jobs_release(e->jobs, id);
            op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "about must be [x, y, z] in mm");
            return;
        }
        for (int k = 0; k < 3; k++) about[k] *= 1e-3;
        ap = about;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    const JsonValue *regions = json_get(p, "regions");
    JsonValue *ro = json_set_array(v, "regions");
    for (size_t i = 0; i < json_len(regions); i++) {
        const JsonValue *rq = json_at(regions, i);
        const char *sel = json_get_str(rq, "selection", "");
        double d[3];
        StaticRegionDisplacement rd;
        JsonValue *o = json_object();
        json_set_string(o, "selection", sel);
        json_set(o, "direction", json_clone(json_get(rq, "direction")));
        if (!direction_from_json(json_get(rq, "direction"), d, err, sizeof err) || !static_region_displacement(r, sel, d, &rd, err, sizeof err)) {
            json_set_string(o, "error", err);
        } else {
            json_set(o, "unit_direction", json_vec3(d[0], d[1], d[2]));
            json_set_int(o, "mesh_faces", rd.faces);
            json_set_number(o, "area_mm2", 1e6 * rd.area);
            json_set(o, "centroid_mm", json_vec3(1e3 * rd.centroid[0], 1e3 * rd.centroid[1], 1e3 * rd.centroid[2]));
            json_set(o, "mean_displacement_mm", json_vec3(1e3 * rd.mean[0], 1e3 * rd.mean[1], 1e3 * rd.mean[2]));
            json_set_number(o, "mean_along_direction_mm", 1e3 * rd.along);
            json_set_number(o, "min_along_direction_mm", 1e3 * rd.min_along);
            json_set_number(o, "max_along_direction_mm", 1e3 * rd.max_along);
        }
        json_push(ro, o);
    }
    json_set_string(v, "region_definition",
                    "area-weighted mean over the selection's boundary faces of the displacement component along the unit direction; the displacement is "
                    "bilinear on each face, so each face contributes the mean of its corner values times its area. min/max are nodal values");
    const JsonValue *loads = json_get(p, "loads");
    JsonValue *lo = json_set_array(v, "loads");
    for (int i = 0; i < m->nbc; i++) {
        const SaBcInfo *b = &m->bc[i];
        bool wanted = false;
        if (loads) {
            for (size_t k = 0; k < json_len(loads); k++) wanted |= json_str(json_at(loads, k)) && !strcmp(json_str(json_at(loads, k)), b->name);
        } else {
            wanted = b->kind == BC_FORCE || b->kind == BC_TRACTION;
        }
        if (!wanted) continue;
        JsonValue *o = json_object();
        json_set_string(o, "condition", b->name);
        json_set_string(o, "kind", bc_kind_name(b->kind));
        StaticLoadWork w;
        if (!static_load_work(r, i, &w, err, sizeof err)) {
            json_set_string(o, "error", err);
        } else {
            double fn = sqrt(w.force[0] * w.force[0] + w.force[1] * w.force[1] + w.force[2] * w.force[2]);
            json_set(o, "force_n", json_vec3(w.force[0], w.force[1], w.force[2]));
            json_set_number(o, "force_magnitude_n", fn);
            json_set_number(o, "work_j", w.work);
            json_set_number(o, "conjugate_displacement_mm", 1e3 * w.conjugate_displacement);
            if (isfinite(w.stiffness)) json_set_number(o, "stiffness_n_per_mm", 1e-3 * w.stiffness);
            else json_set_string(o, "stiffness_unavailable", "the load did no positive work (it is resisted without displacement along it)");
        }
        json_push(lo, o);
    }
    for (size_t k = 0; loads && k < json_len(loads); k++) {
        const char *name = json_str(json_at(loads, k));
        if (name && static_find_bc(m, name) < 0) {
            JsonValue *o = json_object();
            json_set_string(o, "condition", name);
            json_set_string(o, "error", "no condition of this name in the result");
            json_push(lo, o);
        }
    }
    json_set_string(v, "load_definition",
                    "work W = sum of the load's consistent nodal forces times the displacements of their nodes; conjugate displacement W/|F| (for a "
                    "uniform traction the area-weighted mean displacement along F); stiffness |F|^2/W = |F| / conjugate displacement");
    json_set(v, "balance", static_balance_json(r, ap));
    json_set(v, "mass", static_mass_json(m));
    json_set(v, "deformation", static_deformation_json(r));
    JsonValue *units = json_set_object(v, "units");
    json_set_string(units, "length", "mm");
    json_set_string(units, "force", "N");
    json_set_string(units, "moment", "N m");
    json_set_string(units, "stiffness", "N/mm");
    json_set_string(units, "mass", "kg");
    jobs_release(e->jobs, id);
    op_succeed(out, v);
}

const OpBinding OPS_RESULTS_BINDINGS[] = {
    {"results_quantities", op_results_quantities},
    {"results_query", op_results_query},
    {"results_probe", op_results_probe},
    {"results_render", op_results_render},
    {"results_export", op_results_export},
    {"results_interface", op_thermal_interface_results},
    {NULL, NULL},
};
