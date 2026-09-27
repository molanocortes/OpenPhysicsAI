/* study_evidence.c - refinement statistics, the comparison outcome, the numerical-evidence table and the Engineering
 * Evidence Record of a comparison study */
#include "../core/sha256.h"
#include "study_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

static JsonValue *str_list_push(JsonValue *arr, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static JsonValue *str_list_push(JsonValue *arr, const char *fmt, ...) {
    char t[4000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    json_push(arr, json_string(t));
    return arr;
}

/* Refinement of one quantity over a design's valid meshes (coarse to fine). Three conclusions are kept apart:
 *  - the changes between meshes, with their definition;
 *  - whether the last change meets the convergence criterion (reported as met or not met, never dropped);
 *  - whether a discretisation-error estimate can be offered at all, and the conditions checked for it.
 * The estimate is a grid convergence index (Roache): E = Fs |f3 - f2| / (r^p - 1) with the observed order p of the three
 * finest meshes. It is offered only when those meshes have a constant refinement ratio of at least 1.3, change
 * monotonically, give an observed order in [0.5, 4], and represent the geometry identically (voxel volume equal to the
 * geometry volume within 1e-6): a staircase boundary that changes with the mesh makes the meshes discretise different
 * shapes, and Richardson extrapolation does not apply. The safety factor is 3, or 1.25 when four meshes give observed orders
 * that agree within 10 % (asymptotic behaviour indicated rather than assumed). An estimate is not a bound and not a
 * confidence interval. verr_percent may be NULL (geometry representation unknown: no estimate). */
JsonValue *study_refinement(const double *h, const double *q, const double *verr_percent, int n, double criterion) {
    JsonValue *o = json_object();
    json_set(o, "element_sizes_mm", json_numbers(h, (size_t)n));
    json_set(o, "values", json_numbers(q, (size_t)n));
    JsonValue *ch = json_set_array(o, "relative_changes");
    for (int i = 1; i < n; i++) json_push(ch, json_number(fabs(q[i]) > 0 ? (q[i] - q[i - 1]) / fabs(q[i]) : NAN));
    json_set_string(o, "change_definition",
                    "change between consecutive valid meshes relative to the finer mesh's value: (q_fine - q_coarse) / |q_fine|");
    json_set_number(o, "convergence_criterion", criterion);
    json_set_bool(o, "estimate_available", false);
    json_set_string(o, "estimate_nature",
                    "a grid convergence index: an estimate of the discretisation error of the finest value, not a bound and not a confidence interval");
    if (n <= 0) {
        json_set_bool(o, "convergence_criterion_met", false);
        json_set_string(o, "reading", "no valid mesh");
        return o;
    }
    double fine = q[n - 1];
    json_set_number(o, "finest_value", fine);
    char reading[900], cbuf[240];
    JsonValue *why = json_set_array(o, "estimate_unavailable_reasons");
    if (n == 1) {
        json_set_bool(o, "convergence_criterion_met", false);
        json_push(why, json_string("one valid mesh: nothing is known about its discretisation error"));
        json_set_string(o, "reading", "one valid mesh: convergence not assessed and no discretisation-error estimate");
        return o;
    }
    double last = fabs(fine) > 0 ? (fine - q[n - 2]) / fabs(fine) : NAN;
    bool met = isfinite(last) && fabs(last) <= criterion;
    json_set_number(o, "last_relative_change", last);
    json_set_bool(o, "convergence_criterion_met", met);
    snprintf(cbuf, sizeof cbuf, "last change %+.2f%% (relative to the finer mesh) %s the %.3g%% criterion", 100 * last, met ? "meets" : "does not meet",
             100 * criterion);

    /* observed orders of every triplet of consecutive meshes with a constant ratio and a monotone change */
    JsonValue *orders = json_set_array(o, "observed_orders");
    double p_last = NAN, p_prev = NAN;
    for (int k = 2; k < n; k++) {
        double r1 = h[k - 2] / h[k - 1], r2 = h[k - 1] / h[k], e1 = q[k - 1] - q[k - 2], e2 = q[k] - q[k - 1];
        if (!(fabs(r1 - r2) <= 0.01 * r2) || r2 <= 1 || !(e1 * e2 > 0)) continue;
        double p = log(fabs(e1 / e2)) / log(r2);
        JsonValue *po = json_object();
        json_set(po, "element_sizes_mm", json_numbers(h + k - 2, 3));
        json_set_number(po, "order", p);
        json_push(orders, po);
        if (k == n - 1) p_last = p;
        if (k == n - 2) p_prev = p;
    }

    if (isfinite(p_last)) json_set_number(o, "observed_order", p_last);
    JsonValue *conds = json_set_array(o, "estimate_conditions");
/* a condition for the estimate: its text, whether it holds, what was observed; a failed one adds `reason` (a sentence
 * saying what is not the case) to estimate_unavailable_reasons */
#define COND(text, ok, reason, fmt, ...)                                                                                        \
    do {                                                                                                                        \
        JsonValue *c_ = json_object();                                                                                          \
        json_set_string(c_, "condition", text);                                                                                 \
        json_set_bool(c_, "met", ok);                                                                                           \
        JsonValue *s_ = json_stringf(fmt, __VA_ARGS__);                                                                         \
        json_set(c_, "observed", s_);                                                                                           \
        json_push(conds, c_);                                                                                                   \
        if (!(ok)) json_push(why, json_stringf("%s (%s)", reason, json_str(s_)));                                             \
    } while (0)
    bool three = n >= 3;
    COND("at least three valid meshes", three, "fewer than three valid meshes", "%d valid meshes", n);
    bool ok = three;
    if (three) {
        double r1 = h[n - 3] / h[n - 2], r2 = h[n - 2] / h[n - 1], e21 = q[n - 2] - q[n - 3], e32 = q[n - 1] - q[n - 2];
        bool ratio = fabs(r1 - r2) <= 0.01 * r2 && r2 >= 1.3;
        COND("constant refinement ratio of at least 1.3 over the three finest meshes", ratio, "the refinement ratio is not constant or is below 1.3",
             "ratios %.4g and %.4g", r1, r2);
        bool mono = e21 * e32 > 0;
        COND("monotone change over the three finest meshes", mono, "the change is not monotone over the three finest meshes", "changes %+.4g and %+.4g", e21,
             e32);
        bool geo = verr_percent != NULL;
        double vmax = 0;
        for (int k = n - 3; geo && k < n; k++) {
            geo &= isfinite(verr_percent[k]);
            if (isfinite(verr_percent[k])) vmax = fmax(vmax, fabs(verr_percent[k]));
        }
        geo = geo && vmax <= 1e-4;
        if (verr_percent && isfinite(verr_percent[n - 3]) && isfinite(verr_percent[n - 1]))
            COND("the geometry is represented identically on the three finest meshes (volume error within 1e-6)", geo,
                 "the voxel representation of the geometry changes with the mesh, so the meshes discretise different shapes", "volume errors %.3g%%, %.3g%%, %.3g%%",
                 verr_percent[n - 3], verr_percent[n - 2], verr_percent[n - 1]);
        else
            COND("the geometry is represented identically on the three finest meshes (volume error within 1e-6)", false,
                 "whether the meshes represent the same geometry is unknown", "%s", "volume error not recorded");
        bool order_ok = isfinite(p_last) && p_last >= 0.5 && p_last <= 4;
        if (isfinite(p_last))
            COND("observed order of the three finest meshes within [0.5, 4]", order_ok, "the observed order is outside [0.5, 4]", "p = %.3g", p_last);
        else
            COND("observed order of the three finest meshes within [0.5, 4]", false, "no observed order can be computed", "%s",
                 "the ratio is not constant or the change is not monotone");
        ok = ratio && mono && geo && order_ok;
        if (ok) {
            double r = r2, fs = 3;
            const char *basis = "safety factor 3: asymptotic behaviour not demonstrated by three meshes";
            bool four = n >= 4 && isfinite(p_prev) && fabs(h[n - 4] / h[n - 3] - r) <= 0.01 * r && isfinite(verr_percent[n - 4]) && fabs(verr_percent[n - 4]) <= 1e-4 &&
                        fabs(p_last - p_prev) <= 0.1 * fmax(fabs(p_last), fabs(p_prev));
            if (four) {
                fs = 1.25;
                basis = "safety factor 1.25: four meshes give observed orders that agree within 10%";
            }
            double E = fs * fabs(q[n - 1] - q[n - 2]) / (pow(r, p_last) - 1);
            json_set_number(o, "safety_factor", fs);
            json_set_string(o, "safety_factor_basis", basis);
            json_set_number(o, "discretisation_error_estimate", E);
            json_set_number(o, "relative_discretisation_error_estimate", fabs(fine) > 0 ? E / fabs(fine) : NAN);
            json_set_number(o, "richardson_extrapolated_value", q[n - 1] + (q[n - 1] - q[n - 2]) / (pow(r, p_last) - 1));
            json_set_string(o, "estimate_method", "grid convergence index (Roache) over the three finest valid meshes with their observed order");
            json_set_bool(o, "estimate_available", true);
            snprintf(reading, sizeof reading, "%s; discretisation-error estimate %.2g (%.2g%%; observed order %.3g, %s)", cbuf, E,
                     fabs(fine) > 0 ? 100 * E / fabs(fine) : NAN, p_last, basis);
        }
    }
#undef COND
    if (!ok) {
        snprintf(reading, sizeof reading, "%s; no discretisation-error estimate: %s", cbuf,
                 json_len(why) ? json_str(json_at(why, 0)) : "conditions not met");
    }
    json_set_string(o, "reading", reading);
    return o;
}

static const JsonValue *qget(const JsonValue *level, const char *key) { return json_get(json_get(level, "quantities"), key); }
static double qnum(const JsonValue *level, const char *key) { return json_get_num(json_get(level, "quantities"), key, NAN); }

static int design_index(const JsonValue *res, const char *name) {
    for (size_t i = 0; i < json_len(json_get(res, "designs")); i++)
        if (!strcmp(json_get_str(json_at(json_get(res, "designs"), i), "name", ""), name)) return (int)i;
    return -1;
}

/* valid completed levels of one design, up to (and including) level index upto, with each mesh's volume error (percent) */
static int valid_series(const JsonValue *levels, int upto, const char *key, double *h, double *q, double *verr) {
    int n = 0;
    for (int i = 0; i <= upto && i < (int)json_len(levels); i++) {
        const JsonValue *lv = json_at(levels, (size_t)i);
        if (!json_get_bool(lv, "valid", false)) continue;
        double v = qnum(lv, key);
        if (!isfinite(v)) continue;
        h[n] = json_get_num(lv, "element_size_mm", NAN), q[n] = v;
        verr[n] = json_get_num(json_get(lv, "mesh"), "volume_error_percent", NAN);
        n++;
    }
    return n;
}

static const JsonValue *level_at_size(const JsonValue *levels, double h) {
    for (size_t i = 0; i < json_len(levels); i++)
        if (fabs(json_get_num(json_at(levels, i), "element_size_mm", -1) - h) < 1e-9) return json_at(levels, i);
    return NULL;
}

static JsonValue *check_row(JsonValue *rows, const char *check, const char *applies, const char *criterion, const char *observed, bool pass) {
    JsonValue *o = json_object();
    json_set_string(o, "check", check);
    json_set_string(o, "applies_to", applies);
    json_set_string(o, "criterion", criterion);
    json_set_string(o, "observed", observed);
    json_set_string(o, "outcome", pass ? "pass" : "fail");
    json_push(rows, o);
    return o;
}

/* appends to a NUL-terminated buffer, truncating at its capacity */
static void appendf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void appendf(char *buf, size_t cap, const char *fmt, ...) {
    size_t len = strlen(buf);
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
}

/* order[0] is the best design: lowest displacement or highest stiffness (ties keep the definition order) */
static void rank_designs(const double *v, int nd, bool higher_better, int *order) {
    for (int i = 0; i < nd; i++) order[i] = i;
    for (int i = 0; i < nd; i++)
        for (int k = i + 1; k < nd; k++)
            if (higher_better ? v[order[k]] > v[order[i]] : v[order[k]] < v[order[i]]) {
                int t = order[i];
                order[i] = order[k], order[k] = t;
            }
}

static JsonValue *comparison(const StudyJob *sj, StudyDesign *d, int nd, JsonValue **levels, JsonValue **sens, JsonValue *failures,
                             JsonValue *interpretation, JsonValue *uncertainties) {
    const JsonValue *res = sj->resolved;
    const char *primary = json_get_str(json_get(res, "quantities"), "primary", "load_region_displacement");
    bool stiff = !strcmp(primary, "stiffness");
    const char *key = stiff ? "stiffness_n_per_mm" : "load_region_displacement_mm";
    const char *what = stiff ? "stiffness" : "load-region displacement";
    double crit = json_get_num(json_get(res, "refinement"), "convergence_criterion", 0.02);
    int nl = (int)json_len(json_get(json_get(res, "refinement"), "element_sizes_mm"));
    JsonValue *o = json_object();
    json_set_string(o, "primary_quantity", primary);
    json_set_string(o, "better", stiff ? "higher" : "lower");
    /* the finest level every design completed validly */
    int common = -1;
    for (int li = 0; li < nl; li++) {
        bool all = true;
        for (int i = 0; i < nd; i++) all &= json_get_bool(json_at(levels[i], (size_t)li), "valid", false);
        if (all) common = li;
    }
    if (common < 0) {
        char st[2400] = "The comparison cannot be established: no mesh level completed validly for every design.";
        for (int i = 0; i < nd; i++) {
            char reasons[900] = "";
            for (size_t k = 0; k < json_len(levels[i]); k++) {
                const JsonValue *lv = json_at(levels[i], k), *rs = json_get(lv, "invalid_reasons");
                if (!json_get_bool(lv, "completed", false) && !strstr(reasons, "did not complete"))
                    snprintf(reasons + strlen(reasons), sizeof reasons - strlen(reasons), "%sa level did not complete (see failures)", reasons[0] ? "; " : "");
                for (size_t r = 0; r < json_len(rs); r++) {
                    const char *t = json_str(json_at(rs, r));
                    if (t && !strstr(reasons, t)) snprintf(reasons + strlen(reasons), sizeof reasons - strlen(reasons), "%s%s", reasons[0] ? "; " : "", t);
                }
            }
            if (reasons[0]) snprintf(st + strlen(st), sizeof st - strlen(st), " Design %s: %s.", d[i].name, reasons);
        }
        json_set_string(o, "outcome", "cannot_establish");
        json_set_string(o, "statement", st);
        str_list_push(interpretation, "%s", st);
        return o;
    }
    double hc = json_get_num(json_at(levels[0], (size_t)common), "element_size_mm", NAN);
    json_set_number(o, "element_size_mm", hc);
    JsonValue *vals = json_set_array(o, "values");
    double v[STUDY_MAX_DESIGNS], E[STUDY_MAX_DESIGNS], dlast[STUDY_MAX_DESIGNS], chg[STUDY_MAX_DESIGNS];
    bool avail[STUDY_MAX_DESIGNS], met[STUDY_MAX_DESIGNS];
    char nores[STUDY_MAX_DESIGNS][400];
    int one_mesh = -1;
    bool outside = false, nonlin = false, conv_all = true;
    for (int i = 0; i < nd; i++) {
        double hs[STUDY_MAX_LEVELS], qs[STUDY_MAX_LEVELS], vs[STUDY_MAX_LEVELS];
        int n = valid_series(levels[i], common, key, hs, qs, vs);
        JsonValue *rf = study_refinement(hs, qs, vs, n, crit);
        const JsonValue *lv = json_at(levels[i], (size_t)common);
        v[i] = qnum(lv, key);
        if (n < 2 && one_mesh < 0) one_mesh = i;
        avail[i] = json_get_bool(rf, "estimate_available", false);
        E[i] = avail[i] ? json_get_num(rf, "discretisation_error_estimate", NAN) : NAN;
        met[i] = json_get_bool(rf, "convergence_criterion_met", false);
        chg[i] = json_get_num(rf, "last_relative_change", NAN);
        dlast[i] = n >= 2 ? fabs(qs[n - 1] - qs[n - 2]) : NAN;
        conv_all &= met[i];
        const JsonValue *why = json_get(rf, "estimate_unavailable_reasons");
        snprintf(nores[i], sizeof nores[i], "%s", json_len(why) ? json_str(json_at(why, 0)) : "");
        const char *cls = json_get_str(json_get(lv, "checks"), "deformation_classification", "");
        outside |= !strcmp(cls, "outside_small_deformation_assumption");
        nonlin |= !strcmp(cls, "geometric_nonlinearity_possible");
        JsonValue *e = json_object();
        json_set_string(e, "design", d[i].name);
        json_set_number(e, "value", v[i]);
        json_set_int(e, "valid_meshes", n);
        json_set_number(e, "last_relative_change", chg[i]);
        json_set_bool(e, "convergence_criterion_met", met[i]);
        json_set_bool(e, "estimate_available", avail[i]);
        json_set_number(e, "discretisation_error_estimate", E[i]);
        json_set_number(e, "relative_discretisation_error_estimate", avail[i] ? json_get_num(rf, "relative_discretisation_error_estimate", NAN) : NAN);
        if (avail[i]) {
            json_set_number(e, "safety_factor", json_get_num(rf, "safety_factor", NAN));
            json_set_number(e, "observed_order", json_get_num(rf, "observed_order", NAN));
            json_set_string(e, "estimate_method", json_get_str(rf, "estimate_method", ""));
        } else {
            json_set(e, "estimate_unavailable_reasons", json_clone(why));
        }
        json_set_number(e, "stiffness_n_per_mm", qnum(lv, "stiffness_n_per_mm"));
        json_set_number(e, "load_region_displacement_mm", qnum(lv, "load_region_displacement_mm"));
        double m = qnum(lv, "mass_geometry_kg");
        json_set_number(e, "mass_geometry_kg", m);
        if (m > 0) json_set_number(e, "stiffness_per_mass_n_per_mm_kg", qnum(lv, "stiffness_n_per_mm") / m);
        json_set_number(e, "peak_displacement_mm", qnum(lv, "peak_displacement_mm"));
        json_free(rf);
        json_push(vals, e);
    }
    /* rank: best first */
    int order[STUDY_MAX_DESIGNS];
    rank_designs(v, nd, stiff, order);
    JsonValue *rank = json_set_array(o, "ranking");
    for (int i = 0; i < nd; i++) json_push(rank, json_string(d[order[i]].name));
    int b = order[0], w = order[1];
    double D = fabs(v[b] - v[w]), rel = fabs(v[w]) > 0 ? D / fabs(v[w]) : NAN;
    double Esum = E[b] + E[w], Dsum = dlast[b] + dlast[w];
    json_set_number(o, "difference", D);
    json_set_number(o, "relative_difference", rel);

    /* conclusion 1: the ranking on every mesh that all designs completed validly (a fact about the tested meshes) */
    JsonValue *bymesh = json_set_array(o, "ranking_by_mesh");
    bool consistent = true;
    int nmesh = 0;
    double rel_lo = INFINITY, rel_hi = -INFINITY, h_coarse = NAN;
    for (int li = 0; li <= common; li++) {
        double vv[STUDY_MAX_DESIGNS];
        bool all = true;
        for (int i = 0; i < nd; i++) {
            const JsonValue *lv = json_at(levels[i], (size_t)li);
            vv[i] = qnum(lv, key);
            all &= json_get_bool(lv, "valid", false) && isfinite(vv[i]);
        }
        if (!all) continue;
        int ord[STUDY_MAX_DESIGNS];
        rank_designs(vv, nd, stiff, ord);
        JsonValue *m = json_object();
        double hm = json_get_num(json_at(levels[0], (size_t)li), "element_size_mm", NAN), rr = fabs(vv[ord[1]]) > 0 ? fabs(vv[ord[0]] - vv[ord[1]]) / fabs(vv[ord[1]]) : NAN;
        json_set_number(m, "element_size_mm", hm);
        JsonValue *rk = json_set_array(m, "ranking");
        bool same = true;
        for (int i = 0; i < nd; i++) {
            json_push(rk, json_string(d[ord[i]].name));
            same &= ord[i] == order[i];
        }
        json_set_number(m, "relative_difference", rr);
        json_push(bymesh, m);
        consistent &= same;
        if (!nmesh) h_coarse = hm;
        rel_lo = fmin(rel_lo, rr), rel_hi = fmax(rel_hi, rr);
        nmesh++;
    }
    json_set_int(o, "meshes_compared", nmesh);
    json_set_bool(o, "ranking_consistent_on_tested_meshes", consistent);
    /* conclusion 2: the convergence criterion, per design (values[]) and for all */
    json_set_number(o, "convergence_criterion", crit);
    json_set_bool(o, "convergence_criterion_met_by_all", conv_all);
    /* conclusion 3: discretisation-error estimates, when available */
    bool est = avail[b] && avail[w];
    json_set_bool(o, "estimates_available", est);
    json_set_number(o, "combined_discretisation_error_estimate", est ? Esum : NAN);
    json_set_number(o, "sum_of_last_changes", Dsum);
    bool planned_left = common < nl - 1;
    for (size_t i = 0; i < json_len(failures); i++) {
        const char *c = json_get_str(json_at(failures, i), "code", "");
        planned_left |= !strcmp(c, "LEVEL_OVER_ELEMENT_LIMIT") || !strcmp(c, "TIME_BUDGET") || !strcmp(c, "INSUFFICIENT_STORAGE");
    }
    const char *outcome;
    char st[3600];
    const char *unit = stiff ? "N/mm" : "mm";
    int nx = !avail[b] ? b : w; /* a design without an estimate, for the statements */
    const char *finer = planned_left ? " The planned refinement did not complete; finer meshes are needed." : " Add a finer element size to resolve it.";
    if (outside) {
        outcome = "cannot_establish";
        snprintf(st, sizeof st,
                 "The comparison cannot be established: the predicted deformation is outside the small-deformation assumption of the linear model, "
                 "so the linear results do not describe the loaded parts.");
    } else if (one_mesh >= 0) {
        outcome = "not_resolved";
        snprintf(st, sizeof st, "Difference not resolved: design %s has only one valid mesh, so nothing is known about its discretisation error.%s",
                 d[one_mesh].name, finer);
    } else if (est && D > Esum) {
        outcome = "resolved";
        snprintf(st, sizeof st,
                 "%s predicted %s under the modeled conditions: design %s (%.5g %s) against design %s (%.5g %s) at %.4g mm elements, a %.1f%% difference. "
                 "The difference is larger than the sum of the two values' estimated discretisation errors (%.2g %s and %.2g %s; grid convergence "
                 "index, which is an estimate, not a bound).",
                 stiff ? "Higher" : "Lower", what, d[b].name, v[b], unit, d[w].name, v[w], unit, hc, 100 * rel, E[b], unit, E[w], unit);
        if (!consistent) appendf(st, sizeof st, " On at least one coarser mesh the designs ranked differently (see ranking_by_mesh).");
    } else if (est) {
        outcome = conv_all ? "too_small_to_distinguish" : (planned_left ? "more_refinement_required" : "not_resolved");
        snprintf(st, sizeof st,
                 "%s: designs %s and %s differ by %.2f%% in %s at %.4g mm elements, not more than the sum of their estimated discretisation errors "
                 "(%.2g %s).%s",
                 conv_all ? "Difference too small to distinguish" : "Difference not resolved by the current refinement study", d[b].name, d[w].name,
                 100 * rel, what, hc, Esum, unit, conv_all ? "" : finer);
    } else if (consistent && D > Dsum) {
        outcome = "ranking_consistent_on_tested_meshes";
        snprintf(st, sizeof st,
                 "%s predicted %s under the modeled conditions on every tested mesh: design %s against design %s, a difference of %.1f%% to %.1f%% over "
                 "%d meshes (%.4g to %.4g mm); at %.4g mm %.5g %s against %.5g %s. No discretisation-error estimate is available for design %s (%s), "
                 "so the size of the difference is not estimated beyond the tested meshes. The difference at the finest mesh (%.2g %s) is larger "
                 "than the sum of the designs' last changes between meshes (%.2g %s).",
                 stiff ? "Higher" : "Lower", what, d[b].name, d[w].name, 100 * rel_lo, 100 * rel_hi, nmesh, h_coarse, hc, hc, v[b], unit, v[w], unit,
                 d[nx].name, nores[nx], D, unit, Dsum, unit);
    } else if (!consistent) {
        outcome = planned_left ? "more_refinement_required" : "not_resolved";
        snprintf(st, sizeof st,
                 "Difference not resolved: the ranking of the designs changed between the tested meshes (see ranking_by_mesh), and no "
                 "discretisation-error estimate is available for design %s (%s).%s",
                 d[nx].name, nores[nx], finer);
    } else {
        outcome = conv_all ? "too_small_to_distinguish" : (planned_left ? "more_refinement_required" : "not_resolved");
        snprintf(st, sizeof st,
                 "%s: designs %s and %s differ by %.2f%% in %s at %.4g mm elements, not more than the sum of their last changes between meshes "
                 "(%.2g %s); no discretisation-error estimate is available for design %s (%s).%s",
                 conv_all ? "Difference too small to distinguish" : "Difference not resolved by the current refinement study", d[b].name, d[w].name,
                 100 * rel, what, hc, Dsum, unit, d[nx].name, nores[nx], conv_all ? "" : finer);
    }
    if (!outside && one_mesh < 0) {
        if (conv_all) {
            appendf(st, sizeof st, " Every design met the %.3g%% convergence criterion (change between its two finest meshes, relative to the finer).", 100 * crit);
        } else {
            appendf(st, sizeof st, " Convergence criterion (%.3g%% change between the two finest meshes, relative to the finer) not met by", 100 * crit);
            for (int i = 0, k = 0; i < nd; i++)
                if (!met[i]) appendf(st, sizeof st, "%s design %s (%+.2f%%)", k++ ? "," : "", d[i].name, 100 * chg[i]);
            bool ranked = !strcmp(outcome, "resolved") || !strcmp(outcome, "ranking_consistent_on_tested_meshes");
            appendf(st, sizeof st, "%s", ranked ? ", so the size of the difference is less certain than the ranking." : ".");
        }
    }
    if (nonlin && !outside)
        appendf(st, sizeof st, " Conditional: the deformation is large enough that geometric nonlinearity may change the result by more than about 0.1%%.");
    json_set_string(o, "outcome", outcome);
    json_set_string(o, "statement", st);
    str_list_push(interpretation, "%s", st);

    /* robustness of the ranking to the declared sensitivities */
    JsonValue *rob = json_set_array(o, "ranking_robustness");
    bool same_material = true;
    for (int i = 0; i < nd; i++) {
        const JsonValue *dm = json_get(json_at(json_get(res, "designs"), (size_t)design_index(res, d[i].name)), "material");
        same_material &= !dm || dm->type != JSON_OBJECT;
    }
    JsonValue *er = json_object();
    json_set_string(er, "sensitivity", "youngs_modulus");
    if (same_material) {
        json_set_bool(er, "ranking_unchanged", true);
        json_set_string(er, "reason",
                        "every design uses the same material and the loads are forces: all displacements scale with 1/E, so the ranking and the ratio "
                        "between designs do not depend on E; absolute values scale with it");
    } else {
        json_set_string(er, "reason", "the designs use different materials: their moduli could change the ranking; not assessed");
    }
    json_push(rob, er);
    const JsonValue *s0 = json_len(sens[0]) ? sens[0] : NULL;
    for (size_t k = 0; s0 && k < json_len(s0); k++) {
        const JsonValue *ref = json_at(s0, k);
        const char *kind = json_get_str(ref, "kind", ""), *par = json_get_str(ref, "parameter", "");
        double hs = json_get_num(ref, "element_size_mm", NAN), sv[STUDY_MAX_DESIGNS], base[STUDY_MAX_DESIGNS];
        bool complete = true;
        for (int i = 0; i < nd; i++) {
            const JsonValue *match = NULL;
            for (size_t j = 0; j < json_len(sens[i]); j++)
                if (!strcmp(json_get_str(json_at(sens[i], j), "kind", ""), kind) && !strcmp(json_get_str(json_at(sens[i], j), "parameter", ""), par))
                    match = json_at(sens[i], j);
            const JsonValue *bl = level_at_size(levels[i], hs);
            sv[i] = match && json_get_bool(match, "valid", false) ? json_get_num(json_get(match, "quantities"), key, NAN) : NAN;
            base[i] = qnum(bl, key);
            complete &= isfinite(sv[i]) && isfinite(base[i]);
        }
        JsonValue *so = json_object();
        json_set_string(so, "sensitivity", kind);
        json_set_string(so, "parameter", par);
        json_set_number(so, "element_size_mm", hs);
        if (!complete) {
            json_set_string(so, "reason", "not every design completed this sensitivity run");
            json_push(rob, so);
            continue;
        }
        JsonValue *changes = json_set_array(so, "relative_change");
        double maxc = 0;
        for (int i = 0; i < nd; i++) {
            double c = (sv[i] - base[i]) / fabs(base[i]);
            maxc = fmax(maxc, fabs(c));
            JsonValue *ce = json_object();
            json_set_string(ce, "design", d[i].name);
            json_set_number(ce, "baseline", base[i]);
            json_set_number(ce, "value", sv[i]);
            json_set_number(ce, "relative_change", c);
            json_push(changes, ce);
        }
        int bi = 0;
        for (int i = 1; i < nd; i++)
            if (stiff ? sv[i] > sv[bi] : sv[i] < sv[bi]) bi = i;
        json_set_bool(so, "ranking_unchanged", bi == b);
        json_set_number(so, "largest_relative_change", maxc);
        if (!strcmp(kind, "youngs_modulus")) {
            double s = json_get_num(ref, "value", 1), worst = 0;
            for (int i = 0; i < nd; i++) worst = fmax(worst, fabs((stiff ? sv[i] / (base[i] * s) : sv[i] * s / base[i]) - 1));
            json_set_number(so, "scaling_error", worst);
            json_set_bool(so, "scaling_verified", worst <= 1e-6);
        }
        json_push(rob, so);
        if (!strcmp(kind, "mounting_alternative"))
            str_list_push(uncertainties,
                          "Mounting: with the alternative '%s' the %s changes by up to %.1f%%; the ranking %s. A real attachment can lie between "
                          "these idealisations or outside them (for example a flexible fastener), so this indicates the effect of the mounting "
                          "without bounding it.",
                          par, what, 100 * maxc, bi == b ? "is unchanged" : "CHANGES");
        else if (!strcmp(kind, "poisson_ratio"))
            str_list_push(uncertainties, "%s: the %s changes by up to %.2f%%; the ranking %s.", par, what, 100 * maxc,
                          bi == b ? "is unchanged" : "CHANGES");
        else if (!strcmp(kind, "self_weight"))
            str_list_push(uncertainties, "Self-weight: adding each bracket's own weight changes the %s by up to %.2f%%; the ranking %s.", what, 100 * maxc,
                          bi == b ? "is unchanged" : "CHANGES");
    }
    return o;
}

static const char *fmt_g(char *buf, size_t cap, double v) {
    if (isfinite(v)) snprintf(buf, cap, "%.3g", v);
    else snprintf(buf, cap, "n/a");
    return buf;
}

static JsonValue *numerical_evidence(const StudyJob *sj, StudyDesign *d, int nd, JsonValue **levels, JsonValue **sens, JsonValue *uncertainties) {
    JsonValue *rows = json_array();
    double crit = json_get_num(json_get(sj->resolved, "refinement"), "convergence_criterion", 0.02);
    for (int i = 0; i < nd; i++) {
        double eq = 0, mb = 0, en = 0, cons = 0, shift_ratio = 0, verr_fine = NAN;
        bool conv = true, mount = true, topo = true, any = false;
        int worst_cls = 0, nvalid = 0, ncomplete = 0;
        const char *CLS[3] = {"within_small_deformation_assumption", "geometric_nonlinearity_possible", "outside_small_deformation_assumption"};
        for (size_t k = 0; k < json_len(levels[i]); k++) {
            const JsonValue *lv = json_at(levels[i], k), *ck = json_get(lv, "checks");
            if (!json_get_bool(lv, "completed", false)) continue;
            any = true, ncomplete++;
            nvalid += json_get_bool(lv, "valid", false);
            eq = fmax(eq, json_get_num(ck, "equilibrium_error", INFINITY));
            mb = fmax(mb, json_get_num(ck, "moment_balance_error", INFINITY));
            en = fmax(en, fabs(json_get_num(ck, "energy_ratio", INFINITY) - 1));
            cons = fmax(cons, json_get_num(ck, "conjugate_vs_region_mean", INFINITY));
            conv &= json_get_bool(ck, "solver_converged", false);
            const JsonValue *lm = json_get(ck, "load_region_mapping");
            shift_ratio = fmax(shift_ratio, json_get_num(lm, "line_of_action_shift_mm", INFINITY) / json_get_num(lm, "criterion_mm", 1));
            mount &= json_get_bool(json_get(ck, "mounting_region_mapping"), "ok", false);
            topo &= json_get_bool(ck, "mesh_topology_ok", false);
            const char *cls = json_get_str(ck, "deformation_classification", "");
            for (int c = 0; c < 3; c++)
                if (!strcmp(cls, CLS[c]) && c > worst_cls) worst_cls = c;
            verr_fine = json_get_num(json_get(lv, "mesh"), "volume_error_percent", NAN);
        }
        if (!any) continue;
        char a[128], o[300], g1[32];
        snprintf(a, sizeof a, "design %s, %d completed levels (%d valid)", d[i].name, ncomplete, nvalid);
        snprintf(o, sizeof o, "largest %s", fmt_g(g1, sizeof g1, eq));
        check_row(rows, "force equilibrium", a, "|sum of forces| / force scale <= 1e-6", o, eq <= 1e-6);
        snprintf(o, sizeof o, "largest %s", fmt_g(g1, sizeof g1, mb));
        check_row(rows, "moment balance", a, "|sum of moments| / moment scale <= 1e-6", o, mb <= 1e-6);
        snprintf(o, sizeof o, "largest |2U/W - 1| %s", fmt_g(g1, sizeof g1, en));
        check_row(rows, "energy (Clapeyron)", a, "|2 U / W - 1| <= 1e-6", o, en <= 1e-6);
        check_row(rows, "linear solver", a, "converged, true residual recomputed", conv ? "converged at every level" : "not converged at some level", conv);
        check_row(rows, "rigid-body modes", a, "setup_validate finds none before each solve", "none (every solve passed validation)", true);
        snprintf(o, sizeof o, "largest shift / criterion %s", fmt_g(g1, sizeof g1, shift_ratio));
        check_row(rows, "load line of action on the mesh", a, "shift <= max(0.5 h, 1% of the lever arm) and force preserved to 1e-6", o, shift_ratio <= 1);
        snprintf(o, sizeof o, "largest %s", fmt_g(g1, sizeof g1, cons));
        check_row(rows, "work-conjugate displacement = region mean", a, "relative difference <= 1e-9 (a uniform traction does work with the area mean)", o,
                  cons <= 1e-9);
        check_row(rows, "mounting region on the mesh", a, "faces present, mesh/geometry area ratio in (0.5, 2)", mount ? "represented at every level" : "lost at some level", mount);
        check_row(rows, "mesh topology", a, "one face-connected region", topo ? "one region at every level" : "split at some level (a thin connection was lost)", topo);
        check_row(rows, "small deformation", a, "displacement <= 1% of size and rotation error <= 1e-3 (questionable up to 5% / 1e-2)", CLS[worst_cls], worst_cls == 0);
        snprintf(o, sizeof o, "%s%% at the finest completed level", fmt_g(g1, sizeof g1, verr_fine));
        check_row(rows, "geometry approximation (volume)", a, "reported; > 3% changes stiffness and mass noticeably", o, !(fabs(verr_fine) > 3));
        double hs[STUDY_MAX_LEVELS], qs[STUDY_MAX_LEVELS], vs[STUDY_MAX_LEVELS];
        int n = valid_series(levels[i], STUDY_MAX_LEVELS, "load_region_displacement_mm", hs, qs, vs);
        JsonValue *rf = study_refinement(hs, qs, vs, n, crit);
        char c2[200];
        snprintf(c2, sizeof c2, "convergence criterion: change between the two finest valid meshes, relative to the finer, within %.3g%%", 100 * crit);
        check_row(rows, "discretisation (load-region displacement)", a, c2, json_get_str(rf, "reading", ""), json_get_bool(rf, "convergence_criterion_met", false));
        str_list_push(uncertainties, "Discretisation, design %s: %s.", d[i].name, json_get_str(rf, "reading", ""));
        if (isfinite(verr_fine))
            str_list_push(uncertainties, "Geometry approximation, design %s: the finest voxel mesh has %.2f%% volume error against the STL.", d[i].name, verr_fine);
        json_free(rf);
        for (size_t k = 0; k < json_len(sens[i]); k++) {
            const JsonValue *s = json_at(sens[i], k);
            if (strcmp(json_get_str(s, "kind", ""), "youngs_modulus") != 0 || !json_get_bool(s, "valid", false)) continue;
            const JsonValue *bl = level_at_size(levels[i], json_get_num(s, "element_size_mm", NAN));
            double sc = json_get_num(s, "value", 1), ratio = json_get_num(json_get(s, "quantities"), "load_region_displacement_mm", NAN) * sc / qnum(bl, "load_region_displacement_mm");
            snprintf(o, sizeof o, "|u(E x %.3g) x %.3g / u(E) - 1| = %s", sc, sc, fmt_g(g1, sizeof g1, fabs(ratio - 1)));
            check_row(rows, "Young's modulus scaling", a, "linear elasticity: displacement proportional to 1/E, to 1e-6", o, fabs(ratio - 1) <= 1e-6);
        }
    }
    return rows;
}

JsonValue *study_build_evidence(const StudyJob *sj, StudyDesign *d, int nd, JsonValue **levels, JsonValue **sensitivity, JsonValue *failures,
                                const char *status, double wall_seconds) {
    const JsonValue *res = sj->resolved, *chk = sj->check;
    JsonValue *ev = json_object();
    json_set_string(ev, "format", "navier-engineering-evidence-record");
    json_set_int(ev, "format_version", 2);
    char now[32];
    iso_time_now(now, sizeof now);
    json_set_string(ev, "generated", now);
    json_set_string(ev, "status", status);
    json_set_number(ev, "wall_seconds", wall_seconds);
    json_set_string(ev, "nature",
                    "a record of what a linear-elastic model predicts under stated conditions, with its numerical checks; not a certificate, not a "
                    "measurement, and not a statement that a part is safe");
    JsonValue *q = json_set_object(ev, "question");
    json_set_string(q, "text", json_get_str(res, "question", ""));
    if (json_get(res, "decision")) json_set_string(q, "decision", json_get_str(res, "decision", ""));
    JsonValue *st = json_set_object(ev, "study");
    json_set_string(st, "name", json_get_str(res, "name", ""));
    json_set_string(st, "directory", sj->dir);
    json_set_string(st, "study_file", "study.json");
    json_set_string(st, "request_file", "request.json");
    json_set_string(st, "study_hash", sj->hash);

    JsonValue *mc = json_set_object(ev, "modeled_conditions");
    json_set_string(mc, "frames",
                    "design frame: the STL file's axes and origin, lengths in mm; each design's build frame differs by the translation recorded with it");
    JsonValue *dg = json_set_array(mc, "designs");
    for (int i = 0; i < nd; i++) json_push(dg, json_clone(d[i].inspection));
    json_set(mc, "material", json_clone(json_get(res, "material")));
    JsonValue *dm = json_set_array(mc, "design_materials");
    for (size_t i = 0; i < json_len(json_get(res, "designs")); i++) {
        const JsonValue *m = json_get(json_at(json_get(res, "designs"), i), "material");
        if (m && m->type == JSON_OBJECT) json_push(dm, json_clone(m));
    }
    json_set(mc, "manufacturing", json_clone(json_get(res, "manufacturing")));
    json_set(mc, "mounting", json_clone(json_get(res, "mounting")));
    json_set(mc, "load", json_clone(json_get(res, "load")));
    JsonValue *an = json_clone(json_get(res, "analysis"));
    json_set_string(an, "elements", "8-node hexahedra on voxel meshes (staircase boundary)");
    json_set(mc, "analysis", an);
    json_set(mc, "refinement", json_clone(json_get(res, "refinement")));
    json_set(mc, "equivalence", json_clone(json_get(chk, "equivalence")));

    json_set(ev, "assumptions", json_clone(json_get(chk, "assumptions")));
    JsonValue *un = json_set_array(ev, "unresolved_inputs");
    for (size_t i = 0; i < json_len(json_get(chk, "questions")); i++) json_push(un, json_clone(json_at(json_get(chk, "questions"), i)));
    json_set(ev, "accepted_questions", json_clone(json_get(chk, "accepted")));
    json_set(ev, "warnings_at_submission", json_clone(json_get(chk, "warnings")));

    JsonValue *interp = json_array(), *unc = json_array();
    JsonValue *ds = json_set_array(ev, "designs");
    double crit = json_get_num(json_get(res, "refinement"), "convergence_criterion", 0.02);
    for (int i = 0; i < nd; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "name", d[i].name);
        json_set(o, "levels", json_clone(levels[i]));
        JsonValue *rf = json_set_object(o, "refinement");
        static const char *const KEYS[] = {"load_region_displacement_mm", "stiffness_n_per_mm", "mass_mesh_kg", NULL};
        for (int k = 0; KEYS[k]; k++) {
            double hs[STUDY_MAX_LEVELS], qs[STUDY_MAX_LEVELS], vs[STUDY_MAX_LEVELS];
            int n = valid_series(levels[i], STUDY_MAX_LEVELS, KEYS[k], hs, qs, vs);
            json_set(rf, KEYS[k], study_refinement(hs, qs, vs, n, crit));
        }
        json_set(o, "sensitivity", json_clone(sensitivity[i]));
        json_push(ds, o);
    }
    json_set(ev, "comparison", comparison(sj, d, nd, levels, sensitivity, failures, interp, unc));
    json_set(ev, "numerical_evidence", numerical_evidence(sj, d, nd, levels, sensitivity, unc));

    const JsonValue *mat = json_get(res, "material"), *ld = json_get(res, "load"), *mt = json_get(res, "mounting");
    str_list_push(interp,
                  "These are predictions of a static, small-strain, linear-elastic model for: material '%s' (%s values, source %s); mounting region %s; "
                  "%s. They are conditional on those inputs and are not measurements.",
                  json_get_str(mat, "id", "?"), json_get_str(mat, "status", "?"), json_get_str(mat, "source", "?"),
                  !strcmp(json_get_str(mt, "idealization", ""), "fixed") ? "rigidly fixed" : "on a frictionless support",
                  json_get_str(ld, "kind", "") && !strcmp(json_get_str(ld, "kind", ""), "payload_mass") ? "a static payload weight spread uniformly over the load region"
                                                                                                        : "a static force spread uniformly over the load region");
    if (!strcmp(json_get_str(mat, "status", ""), "demonstration"))
        str_list_push(interp, "The material holds demonstration values: absolute displacements, stiffnesses and masses are indicative only.");
    str_list_push(interp, "Stress values are supporting information only. No strength margin or safety statement is made (see not evaluated).");
    str_list_push(unc, "Load distribution and location: a uniform traction over the load region is assumed; a payload bearing on part of it or with an offset centre of mass is not assessed.");
    const char *proc = json_get_str(json_get(res, "manufacturing"), "process", "unspecified");
    if (!strcmp(proc, "unspecified"))
        str_list_push(unc, "Manufacturing: not specified; the material is treated as homogeneous and isotropic.");
    else
        str_list_push(unc, "Manufacturing (%s): its effect on stiffness is represented only through the material values given.", proc);
    json_set(ev, "interpretation", interp);
    json_set(ev, "important_uncertainties", unc);
    json_set(ev, "not_evaluated", json_clone(json_get(chk, "not_evaluated")));
    json_set(ev, "unsupported_requests", json_clone(json_get(chk, "unsupported")));
    json_set(ev, "failures", json_clone(failures));

    JsonValue *rp = json_set_object(ev, "reproduction");
    json_set_string(rp, "study_file", "study.json");
    json_set_string(rp, "study_hash", sj->hash);
    json_set_string(rp, "request_file", "request.json");
    json_set_string(rp, "operations_log", "operations.jsonl");
    JsonValue *geo = json_set_array(rp, "geometry");
    for (int i = 0; i < nd; i++) {
        const JsonValue *g = json_get(d[i].inspection, "geometry");
        JsonValue *go = json_object();
        json_set_string(go, "design", d[i].name);
        json_set_string(go, "original_file", json_get_str(g, "file", ""));
        char stored[512];
        snprintf(stored, sizeof stored, "designs/%s/%s", d[i].name, json_get_str(g, "stored_copy", ""));
        json_set_string(go, "stored_copy", stored);
        json_set_string(go, "sha256", json_get_str(g, "sha256", ""));
        json_push(geo, go);
    }
    JsonValue *sw = json_set_object(rp, "software");
    json_set_string(sw, "name", "NAVIER");
    json_set_string(sw, "version", NAVIER_AM_VERSION);
    json_set_string(sw, "contract_version", ops_contract_version());
    json_set_string(sw, "compiler", __VERSION__);
    json_set_string(sw, "floating_point", "IEEE 754 double precision; no -ffast-math; no floating-point contraction");
    struct utsname u;
    if (uname(&u) == 0) {
        JsonValue *pl = json_set_object(rp, "platform");
        json_set_string(pl, "system", u.sysname);
        json_set_string(pl, "release", u.release);
        json_set_string(pl, "machine", u.machine);
        json_set_int(pl, "online_cpus", sysconf(_SC_NPROCESSORS_ONLN));
    }
    JsonValue *cmds = json_set_array(rp, "commands");
    str_list_push(cmds, "MCP: study_replay {\"directory\": \"%s\"} reruns the study from study.json and the stored geometry copies and compares every quantity", sj->dir);
    str_list_push(cmds, "CLI: navier-ctl --embedded study replay \"%s\"", sj->dir);
    str_list_push(cmds, "Inspect a design: project_open {\"path\": \"%s/designs/<design>\"}, then results_query / results_quantities with a run's job_id", sj->dir);
    json_set_string(rp, "retain_results", json_get_str(res, "retain_results", "refinement"));
    JsonValue *art = json_set_object(rp, "artifacts");
    JsonValue *orig = json_set_array(art, "retained_originals");
    str_list_push(orig, "request.json: the definition as submitted");
    str_list_push(orig, "study.json: the resolved specification and its hash");
    str_list_push(orig, "operations.jsonl: every executed operation with parameters and outcome");
    str_list_push(orig, "designs/<design>/: the projects with the stored geometry copies (hashed) and, per run, spec.json and summary.json");
    str_list_push(orig, "results.nvr of the runs marked results_retained in /designs/i/levels/k/run (the full displacement and stress fields)");
    JsonValue *extr = json_set_array(art, "extracted_summaries");
    str_list_push(extr, "evidence.json: quantities, checks and statistics extracted from the runs, with the SHA-256 of every field file");
    str_list_push(extr, "report.md: generated only from evidence.json");
    str_list_push(extr, "previews/: region images generated at setup");
    JsonValue *regen = json_set_array(art, "regenerated_on_replay");
    str_list_push(regen, "results.nvr of runs whose field was not retained: study_replay recomputes them; the stored SHA-256 identifies the original");
    json_set_string(rp, "retained_results_note",
                    "each run keeps spec.json and summary.json; results.nvr (the full field) is kept for the refinement levels by default and removed for "
                    "sensitivity runs after their quantities and SHA-256 were recorded; study_replay regenerates any of them");
    json_set_string(rp, "reproducibility",
                    "replays on the same build and platform are expected to agree bitwise; the replay record compares quantities with a relative "
                    "tolerance of 1e-9. Other compilers, CPUs or thread counts may differ in the last digits.");
    return ev;
}

JsonValue *study_read_evidence(const char *directory, char *err, size_t errlen) {
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, directory, "evidence.json");
    JsonError je;
    JsonValue *v = json_read_file(path, (size_t)256 << 20, &je);
    if (!v) snprintf(err, errlen, "cannot read %s: %s", path, je.message);
    return v;
}

JsonValue *study_compare_replay(const JsonValue *reference, const JsonValue *replay) {
    JsonValue *o = json_object();
    static const char *const KEYS[] = {"load_region_displacement_mm", "stiffness_n_per_mm", "mass_mesh_kg", "peak_displacement_mm", NULL};
    JsonValue *rows = json_set_array(o, "quantities");
    double worst = 0;
    int compared = 0, identical = 0, missing = 0;
    const JsonValue *rd = json_get(reference, "designs"), *pd = json_get(replay, "designs");
    for (size_t i = 0; i < json_len(rd); i++) {
        const JsonValue *a = json_at(rd, i), *b = NULL;
        for (size_t k = 0; k < json_len(pd); k++)
            if (!strcmp(json_get_str(json_at(pd, k), "name", ""), json_get_str(a, "name", ""))) b = json_at(pd, k);
        for (size_t l = 0; l < json_len(json_get(a, "levels")); l++) {
            const JsonValue *la = json_at(json_get(a, "levels"), l);
            if (!json_get_bool(la, "completed", false)) continue;
            const JsonValue *lb = b ? level_at_size(json_get(b, "levels"), json_get_num(la, "element_size_mm", NAN)) : NULL;
            for (int k = 0; KEYS[k]; k++) {
                double x = qnum(la, KEYS[k]), y = lb ? qnum(lb, KEYS[k]) : NAN;
                if (!isfinite(x)) continue;
                JsonValue *r = json_object();
                json_set_string(r, "design", json_get_str(a, "name", ""));
                json_set_number(r, "element_size_mm", json_get_num(la, "element_size_mm", NAN));
                json_set_string(r, "quantity", KEYS[k]);
                json_set_number(r, "reference", x);
                if (!isfinite(y)) {
                    json_set_string(r, "replay", "missing");
                    missing++;
                } else {
                    double rel = fabs(y - x) / fmax(fabs(x), 1e-300);
                    json_set_number(r, "replay", y);
                    json_set_number(r, "relative_difference", rel);
                    json_set_bool(r, "bitwise_identical", x == y);
                    worst = fmax(worst, rel);
                    identical += x == y;
                    compared++;
                }
                json_push(rows, r);
            }
        }
    }
    (void)qget;
    json_set_int(o, "compared", compared);
    json_set_int(o, "bitwise_identical", identical);
    json_set_int(o, "missing", missing);
    json_set_number(o, "largest_relative_difference", worst);
    json_set_number(o, "tolerance", 1e-9);
    const char *oa = json_get_str(json_get(reference, "comparison"), "outcome", ""), *ob = json_get_str(json_get(replay, "comparison"), "outcome", "");
    json_set_string(o, "reference_software_version", json_get_str(json_get(json_get(reference, "reproduction"), "software"), "version", "?"));
    json_set_string(o, "replay_software_version", json_get_str(json_get(json_get(replay, "reproduction"), "software"), "version", "?"));
    json_set_string(o, "reference_outcome", oa);
    json_set_string(o, "replay_outcome", ob);
    bool ok = compared > 0 && !missing && worst <= 1e-9 && !strcmp(oa, ob);
    json_set_string(o, "outcome", ok ? "reproduced" : "differs");
    return o;
}
