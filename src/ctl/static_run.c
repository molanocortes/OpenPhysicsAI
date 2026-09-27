/* static_run.c - the static analysis job: solve, derived results, and a summary with equilibrium, energy and
 * stress-singularity checks */
#include "../core/sha256.h"
#include "../fem/dense.h"
#include "../fem/hex8.h"
#include "../threads.h"
#include "static_analysis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Job *job;
    double tol;
} Progress;

static bool on_progress(void *ctx, int it, double value) {
    Progress *pr = ctx;
    if (job_cancel_requested(pr->job)) return false;
    char stage[160];
    if (it % 10 == 0) { /* conjugate gradients report every 10 iterations with the relative residual */
        double f = value > 0 && value < 1 ? log(value) / log(pr->tol) : 0;
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        snprintf(stage, sizeof stage, "iterative solve: iteration %d, relative residual %.2e", it, value);
        job_progress(pr->job, 0.10 + 0.80 * f, stage);
    } else { /* the sparse Cholesky factorisation reports the factorised fraction */
        snprintf(stage, sizeof stage, "direct solve: factorisation %.0f%%", 100 * value);
        job_progress(pr->job, 0.10 + 0.80 * value, stage);
    }
    return true;
}

void static_results_derive(StaticResults *r) {
    const StaticModel *m = &r->model;
    free(r->node_vm), free(r->elem_vm);
    r->node_vm = malloc((size_t)(m->nnodes ? m->nnodes : 1) * sizeof(double));
    r->elem_vm = malloc((size_t)(m->nelems ? m->nelems : 1) * sizeof(double));
    if (!r->node_vm || !r->elem_vm || !r->sol.node_stress || !r->sol.gp_stress) return;
    for (int nd = 0; nd < m->nnodes; nd++) r->node_vm[nd] = von_mises(r->sol.node_stress + 6 * (size_t)nd);
    int ng = sa_ngp(m);
    for (int e = 0; e < m->nelems; e++) {
        double mx = 0;
        for (int g = 0; g < ng; g++) mx = fmax(mx, von_mises(r->sol.gp_stress + 6 * ((size_t)ng * e + (size_t)g)));
        r->elem_vm[e] = mx;
    }
}

void static_results_free(void *data) {
    StaticResults *r = data;
    if (!r) return;
    static_model_free(&r->model);
    solid_result_free(&r->sol);
    free(r->node_vm), free(r->elem_vm);
    json_free(r->build_warnings);
    json_free(r->summary);
    free(r);
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double percentile_sorted(const double *v, size_t n, double q) {
    if (!n) return NAN;
    double pos = q * (double)(n - 1);
    size_t i = (size_t)pos;
    double f = pos - (double)i;
    return i + 1 < n ? v[i] * (1 - f) + v[i + 1] * f : v[n - 1];
}

static JsonValue *mm3(const double *x) { return json_vec3(1e3 * x[0], 1e3 * x[1], 1e3 * x[2]); }

static bool node_supported(const StaticModel *m, int nd) {
    size_t q = 3 * (size_t)nd;
    return m->fixed[q] || m->fixed[q + 1] || m->fixed[q + 2];
}

double static_distance_to_reentrant_edge(const StaticModel *m, const double p[3]) {
    double best = INFINITY;
    for (int i = 0; m->concave_seg && i < m->nconcave; i++) {
        const double *s = m->concave_seg + 6 * (size_t)i;
        double d[3], w[3], q[3];
        for (int k = 0; k < 3; k++) d[k] = s[3 + k] - s[k], w[k] = p[k] - s[k];
        double l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], t = l2 > 0 ? (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / l2 : 0;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        for (int k = 0; k < 3; k++) q[k] = s[k] + t * d[k] - p[k];
        best = fmin(best, sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]));
    }
    return best;
}

JsonValue *static_summary_json(const StaticResults *r) {
    const StaticModel *m = &r->model;
    const SolidResult *s = &r->sol;
    int nn = m->nnodes, ne = m->nelems;
    JsonValue *o = json_object();
    json_set_string(o, "analysis", "static_structural");
    json_set(o, "model", static_model_summary_json(m));

    JsonValue *sv = json_set_object(o, "solver");
    json_set_string(sv, "method", s->stats.method);
    json_set_int(sv, "iterations", s->stats.iterations);
    json_set_number(sv, "relative_residual", s->stats.rel_residual);
    json_set_number(sv, "true_relative_residual", s->stats.true_residual);
    json_set_bool(sv, "converged", s->stats.converged);
    json_set_number(sv, "seconds", s->stats.seconds);
    if (s->stats.factor_nnz > 0) {
        json_set_int(sv, "factor_nonzeros", s->stats.factor_nnz);
        json_set_number(sv, "factor_mb", s->stats.factor_mb);
        json_set_number(sv, "pivot_ratio", s->stats.max_pivot > 0 ? s->stats.min_pivot / s->stats.max_pivot : 0);
    }

    double umax = -1, amax[3] = {0, 0, 0};
    int un = -1;
    for (int nd = 0; nd < nn; nd++) {
        const double *u = s->u + 3 * (size_t)nd;
        double mag = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (mag > umax) umax = mag, un = nd;
        for (int k = 0; k < 3; k++) amax[k] = fmax(amax[k], fabs(u[k]));
    }
    JsonValue *d = json_set_object(o, "displacement");
    json_set_number(d, "max_magnitude_mm", 1e3 * (umax > 0 ? umax : 0));
    if (un >= 0) {
        json_set(d, "at_mm", mm3(m->xyz + 3 * (size_t)un));
        json_set_int(d, "node", un);
        json_set(d, "vector_mm", mm3(s->u + 3 * (size_t)un));
    }
    json_set(d, "max_abs_component_mm", mm3(amax));

    if (r->node_vm && r->elem_vm && nn > 0 && ne > 0) {
        int vn = 0, ge = 0;
        for (int nd = 1; nd < nn; nd++)
            if (r->node_vm[nd] > r->node_vm[vn]) vn = nd;
        for (int e = 1; e < ne; e++)
            if (r->elem_vm[e] > r->elem_vm[ge]) ge = e;
        double *sorted = malloc((size_t)nn * sizeof(double));
        double p50 = NAN, p95 = NAN, p99 = NAN;
        if (sorted) {
            memcpy(sorted, r->node_vm, (size_t)nn * sizeof(double));
            qsort(sorted, (size_t)nn, sizeof(double), cmp_double);
            p50 = percentile_sorted(sorted, (size_t)nn, 0.50);
            p95 = percentile_sorted(sorted, (size_t)nn, 0.95);
            p99 = percentile_sorted(sorted, (size_t)nn, 0.99);
            free(sorted);
        }
        bool at_support = node_supported(m, vn), touches = false;
        int npe = sa_npe(m);
        for (int a = 0; a < npe; a++) touches |= node_supported(m, m->conn[(size_t)npe * ge + a]);
        double centroid[3];
        sa_elem_centroid(m, ge, centroid);
        JsonValue *vm = json_set_object(o, "von_mises");
        json_set_number(vm, "nodal_average_max_mpa", 1e-6 * r->node_vm[vn]);
        json_set(vm, "nodal_max_at_mm", mm3(m->xyz + 3 * (size_t)vn));
        json_set_int(vm, "nodal_max_node", vn);
        json_set_number(vm, "gauss_point_max_mpa", 1e-6 * r->elem_vm[ge]);
        json_set_int(vm, "gauss_point_max_element", ge);
        json_set(vm, "gauss_point_max_element_centroid_mm", mm3(centroid));
        json_set_number(vm, "nodal_p50_mpa", 1e-6 * p50);
        json_set_number(vm, "nodal_p95_mpa", 1e-6 * p95);
        json_set_number(vm, "nodal_p99_mpa", 1e-6 * p99);
        json_set_bool(vm, "nodal_max_at_supported_node", at_support);
        json_set_bool(vm, "gauss_point_max_element_touches_support", touches);
        double ratio = p99 > 0 ? r->node_vm[vn] / p99 : 0;
        json_set_number(vm, "max_to_p99_ratio", ratio);
        JsonValue *w = json_set_array(o, "result_warnings");
        double hmax = fmax(m->h[0], fmax(m->h[1], m->h[2]));
        double dcorner = fmin(static_distance_to_reentrant_edge(m, m->xyz + 3 * (size_t)vn), static_distance_to_reentrant_edge(m, centroid));
        bool corner = dcorner < 1.5 * hmax;
        json_set_bool(vm, "peak_near_reentrant_corner", corner);
        if (isfinite(dcorner)) json_set_number(vm, "peak_distance_to_reentrant_edge_mm", 1e3 * dcorner);
        if (corner) {
            JsonValue *wo = json_object();
            json_set_string(wo, "code", "PEAK_AT_REENTRANT_CORNER");
            char msg[900];
            snprintf(msg, sizeof msg,
                     "the largest von Mises stress (%.4g MPa nodal average) lies %.3g mm from a sharp inside corner of the geometry. A sharp re-entrant "
                     "corner is a stress singularity in linear elasticity: the peak grows as the mesh is refined, and real parts have fillets. Judge "
                     "the design by stresses away from the corner (99th percentile %.4g MPa), or model the fillet with a fine mesh",
                     1e-6 * r->node_vm[vn], 1e3 * dcorner, 1e-6 * p99);
            json_set_string(wo, "message", msg);
            json_push(w, wo);
        }
        if (at_support || touches) {
            JsonValue *wo = json_object();
            json_set_string(wo, "code", "PEAK_STRESS_AT_SUPPORT");
            char msg[900];
            snprintf(msg, sizeof msg,
                     "the largest von Mises stress (%.4g MPa nodal average, %.4g MPa at a Gauss point) is at a supported node or in an element touching a "
                     "support; ideal supports create stress singularities, so this peak grows with mesh refinement and is not a design value. The "
                     "99th percentile of nodal values is %.4g MPa; query stresses away from supports (results_query with a selection, results_probe)",
                     1e-6 * r->node_vm[vn], 1e-6 * r->elem_vm[ge], 1e-6 * p99);
            json_set_string(wo, "message", msg);
            json_push(w, wo);
        } else if (ratio > 2.5 && !corner) {
            JsonValue *wo = json_object();
            json_set_string(wo, "code", "ISOLATED_STRESS_PEAK");
            char msg[600];
            snprintf(msg, sizeof msg,
                     "the peak von Mises stress is %.1f times the 99th percentile of nodal values: check whether it is a mesh artefact (staircase step or "
                     "re-entrant corner) by repeating the analysis with a finer mesh",
                     ratio);
            json_set_string(wo, "message", msg);
            json_push(w, wo);
        }
    }

    JsonValue *ck = json_set_object(o, "checks");
    double sum[3];
    for (int k = 0; k < 3; k++) sum[k] = s->load_total[k] + s->reaction_total[k];
    json_set(ck, "applied_load_total_n", json_vec3(s->load_total[0], s->load_total[1], s->load_total[2]));
    json_set(ck, "reaction_total_n", json_vec3(s->reaction_total[0], s->reaction_total[1], s->reaction_total[2]));
    json_set(ck, "load_plus_reaction_n", json_vec3(sum[0], sum[1], sum[2]));
    json_set_number(ck, "equilibrium_error", s->equilibrium_error);
    json_set_bool(ck, "equilibrium_ok", s->equilibrium_error < 1e-6);
    json_set_number(ck, "strain_energy_j", s->strain_energy);
    json_set_number(ck, "external_work_j", s->external_work);
    if (fabs(s->external_work) > 0) {
        double ratio = 2 * s->strain_energy / s->external_work;
        json_set_number(ck, "energy_ratio", ratio);
        json_set_bool(ck, "energy_ok", fabs(ratio - 1) < 1e-6);
    }
    json_set_number(ck, "min_jacobian_determinant", s->min_detJ);
    json_set_string(ck, "notes",
                    "reactions are forces exerted by the supports on the part, so applied loads plus reactions should vanish, and so should their moments "
                    "(balance); for linear statics twice the strain energy equals the external work (Clapeyron), so energy_ratio should be 1");

    JsonValue *rx = json_set_array(o, "reactions");
    for (int i = 0; i < m->nbc; i++) {
        if (!bc_is_constraint(m->bc[i].kind)) continue;
        double f[3] = {0, 0, 0};
        int nodes = 0;
        for (int nd = 0; nd < nn; nd++) {
            if (m->node_bc[nd] != i) continue;
            nodes++;
            for (int k = 0; k < 3; k++) f[k] += s->reaction[3 * (size_t)nd + k];
        }
        JsonValue *ro = json_object();
        json_set_string(ro, "condition", m->bc[i].name);
        json_set(ro, "force_n", json_vec3(f[0], f[1], f[2]));
        json_set_int(ro, "nodes", nodes);
        json_push(rx, ro);
    }
    JsonValue *ld = json_set_array(o, "loads");
    for (int i = 0; i < m->nbc; i++) {
        const SaBcInfo *b = &m->bc[i];
        if (bc_is_constraint(b->kind)) continue;
        JsonValue *lo = json_object();
        json_set_string(lo, "condition", b->name);
        json_set_string(lo, "kind", bc_kind_name(b->kind));
        json_set(lo, "resultant_on_stl_n", json_vec3(b->requested[0], b->requested[1], b->requested[2]));
        json_set(lo, "resultant_applied_n", json_vec3(b->applied[0], b->applied[1], b->applied[2]));
        json_push(ld, lo);
    }
    JsonValue *bal = static_balance_json(r, NULL);
    json_set_number(ck, "moment_balance_error", json_get_num(bal, "moment_balance_error", NAN));
    json_set_bool(ck, "moment_balance_ok", json_get_bool(bal, "moment_balance_ok", false));
    json_set(o, "balance", bal);
    json_set(o, "mass", static_mass_json(m));
    JsonValue *dfm = static_deformation_json(r);
    const char *cls = json_get_str(dfm, "classification", "");
    if (strcmp(cls, "within_small_deformation_assumption") != 0) {
        JsonValue *wo = json_object();
        bool outside = !strcmp(cls, "outside_small_deformation_assumption");
        json_set_string(wo, "code", outside ? "OUTSIDE_SMALL_DEFORMATION" : "GEOMETRIC_NONLINEARITY_POSSIBLE");
        json_set_string(wo, "message",
                        json_str(json_get(dfm, "criteria")) ? (outside ? "the displacements or rotations are too large for small-strain linear elasticity: this "
                                                                         "result does not describe the deformed part (see deformation.criteria); geometric "
                                                                         "nonlinearity is not modelled"
                                                                       : "the displacements or rotations are large enough that geometric nonlinearity may change "
                                                                         "the result by more than about 0.1% (see deformation.criteria); it is not modelled")
                                                              : "");
        JsonValue *w = json_get(o, "result_warnings");
        if (!w) w = json_set_array(o, "result_warnings");
        json_push(w, wo);
    }
    json_set(o, "deformation", dfm);
    json_set(o, "setup_warnings", r->build_warnings ? json_clone(r->build_warnings) : json_array());
    JsonValue *notes = json_set_array(o, "interpretation");
    char t[400];
    snprintf(t, sizeof t,
             "Small-strain linear elasticity with isotropic materials evaluated at %.4g degC. Not included: contact, plasticity, creep, geometric "
             "nonlinearity, thermal strains, residual stresses from the build process, anisotropy of printed material.",
             m->settings.reference_temperature_k - 273.15);
    json_push(notes, json_string(t));
    json_push(notes, json_string("Nodal stresses average the Gauss-point values extrapolated to the nodes of the surrounding elements; Gauss-point "
                                 "values are not smoothed. Reactions shared by nodes of several supports are attributed to the first support."));
    json_push(notes, json_string("The voxel mesh boundary is a staircase: confirm results by repeating the analysis with half the element size."));
    JsonValue *units = json_set_object(o, "units");
    json_set_string(units, "length", "mm");
    json_set_string(units, "stress", "MPa");
    json_set_string(units, "force", "N");
    json_set_string(units, "energy", "J");
    json_set_string(units, "frame", "build frame: +z is the build direction, z = 0 the plate top");
    return o;
}

bool static_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    StaticResults *r = data;
    StaticModel *m = &r->model;
    job_progress(job, 0.02, "assembling the stiffness matrix");
    int threads = cpu_perf_count();
    ThreadPool *pool = pool_create(threads > 0 ? threads : 1);
    Progress pr = {job, m->settings.pcg_tol > 0 ? m->settings.pcg_tol : 1e-10};
    HexModel hmod = {m->nnodes, m->nelems, m->xyz, m->conn, m->elem_mat, m->nmat, m->mat, m->settings.formulation, NULL, .elem_type = m->elem_type};
    SolidLoads L = {m->fixed, m->fixed_value, m->nodal_force, NULL, {m->gravity[0], m->gravity[1], m->gravity[2]}};
    SolidOptions opt = {m->settings.solver, pr.tol, 0, 0, pool, on_progress, &pr};
    bool ok = solid_solve(&hmod, &L, &opt, &r->sol, err, errlen);
    if (pool) pool_destroy(pool);
    if (!ok) {
        const char *c = "SOLVER_FAILED";
        if (job_cancel_requested(job)) c = "CANCELLED";
        else if (strstr(err, "rigid") || strstr(err, "freely") || strstr(err, "singular")) c = "INSUFFICIENT_CONSTRAINTS";
        else if (strstr(err, "out of memory") || strstr(err, "limit")) c = "RESOURCE_LIMIT";
        snprintf(code, codelen, "%s", c);
        return false;
    }
    if (!r->sol.stats.converged) {
        snprintf(code, codelen, "SOLVER_FAILED");
        snprintf(err, errlen, "the iterative solver did not converge: relative residual %.3g after %d iterations (tolerance %.3g); try solver \"direct\" or a coarser mesh",
                 r->sol.stats.rel_residual, r->sol.stats.iterations, pr.tol);
        return false;
    }
    free(r->sol.gp_strain); /* strain follows from the stress for linear elasticity */
    r->sol.gp_strain = NULL;
    job_progress(job, 0.92, "recovering stresses");
    static_results_derive(r);
    if (!r->node_vm || !r->elem_vm) {
        snprintf(code, codelen, "RESOURCE_LIMIT");
        snprintf(err, errlen, "out of memory deriving results");
        return false;
    }
    json_free(r->summary);
    r->summary = static_summary_json(r);
    job_progress(job, 0.95, "writing results");
    if (r->run_dir[0]) {
        char path[NV_PATH_MAX], hex[65];
        uint64_t bytes = 0;
        path_join(path, sizeof path, r->run_dir, "results.nvr");
        if (!static_results_save(r, path, err, errlen)) {
            snprintf(code, codelen, "IO_ERROR");
            return false;
        }
        JsonValue *files = json_set_array(r->summary, "files");
        if (sha256_file(path, hex, &bytes)) {
            JsonValue *fo = json_object();
            json_set_string(fo, "name", "results.nvr");
            json_set_string(fo, "format", "navier-am-results 1: JSON header and little-endian arrays");
            json_set_int(fo, "bytes", (long long)bytes);
            json_set_string(fo, "sha256", hex);
            json_push(files, fo);
        }
        path_join(path, sizeof path, r->run_dir, "summary.json");
        if (!json_write_file(path, r->summary, JSON_PRETTY | JSON_SORTED)) {
            snprintf(code, codelen, "IO_ERROR");
            snprintf(err, errlen, "cannot write %s", path);
            return false;
        }
    }
    job_set_summary(job, json_clone(r->summary));
    return true;
}
