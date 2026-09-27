/* evaltest.c - regression tests for the defects found by the independent evaluation of 0.3.0-rc1
 *   D1  the discretisation-error estimate and the comparison outcome keep three conclusions apart: the ranking on the
 *       tested meshes, the convergence criterion (met or not, never dropped) and an estimate offered only when its
 *       conditions hold; no "±" and no "uncertainty" wording for it. Inputs are the stored values of the rc1 bracket
 *       example and of the four-level audit.
 *   D2  the change definition is stated with every refinement reading
 *   D3  access roots: a folder inside a root takes no slot; the limit message names the limit and the roots in use; a
 *       write root that cannot also be readable is refused
 *   D4  storage: the estimate per analysis, the preflight plan and the free-space query
 *   make build/evaltest && ./build/evaltest */
#include "../src/ctl/engine.h"
#include "../src/ctl/study_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (cond) g_pass++;                                                                                           \
        else {                                                                                                        \
            g_fail++;                                                                                                 \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                             \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static JsonValue *parse(const char *s) {
    JsonError je;
    JsonValue *v = json_parse(s, strlen(s), NULL, &je);
    if (!v) printf("  bad test JSON: %s\n", je.message);
    return v;
}

/* one design's level records: sizes, values of the load-region displacement, volume errors (percent); valid[k] = 0
 * marks an invalid level */
static JsonValue *levels_of(int n, const double *h, const double *u, const double *verr, const int *valid) {
    JsonValue *arr = json_array();
    for (int k = 0; k < n; k++) {
        JsonValue *lv = json_object();
        json_set_number(lv, "element_size_mm", h[k]);
        json_set_bool(lv, "completed", true);
        json_set_bool(lv, "valid", valid ? valid[k] != 0 : true);
        JsonValue *m = json_set_object(lv, "mesh");
        json_set_number(m, "volume_error_percent", verr[k]);
        JsonValue *q = json_set_object(lv, "quantities");
        json_set_number(q, "load_region_displacement_mm", u[k]);
        json_set_number(q, "stiffness_n_per_mm", 29.42 / u[k]);
        json_set_number(q, "mass_mesh_kg", 0.1 * (1 + verr[k] / 100));
        json_set_number(q, "mass_geometry_kg", 0.1);
        json_set_number(q, "peak_displacement_mm", 1.2 * u[k]);
        JsonValue *c = json_set_object(lv, "checks");
        json_set_string(c, "deformation_classification", "within_small_deformation_assumption");
        json_set_number(c, "equilibrium_error", 1e-12);
        json_set_number(c, "moment_balance_error", 1e-12);
        json_set_number(c, "energy_ratio", 1);
        json_set_number(c, "conjugate_vs_region_mean", 0);
        json_set_bool(c, "solver_converged", true);
        JsonValue *lm = json_set_object(c, "load_region_mapping");
        json_set_number(lm, "line_of_action_shift_mm", 0);
        json_set_number(lm, "criterion_mm", 1);
        json_set_bool(json_set_object(c, "mounting_region_mapping"), "ok", true);
        json_set_bool(c, "mesh_topology_ok", true);
        json_push(arr, lv);
    }
    return arr;
}

typedef struct Built {
    JsonValue *ev;
    const JsonValue *cmp, *va, *vb; /* comparison, values of the first and second design (by name) */
    char *md;
} Built;

static const JsonValue *value_of(const JsonValue *cmp, const char *name) {
    for (size_t i = 0; i < json_len(json_get(cmp, "values")); i++)
        if (!strcmp(json_get_str(json_at(json_get(cmp, "values"), i), "design", ""), name)) return json_at(json_get(cmp, "values"), i);
    return NULL;
}

static Built build(const char *na, const char *nb, int n, const double *h, const double *ua, const double *ea, const int *valid_a, const double *ub,
                   const double *eb) {
    StudyJob sj;
    memset(&sj, 0, sizeof sj);
    char sizes[200] = "[";
    for (int k = 0; k < n; k++) snprintf(sizes + strlen(sizes), sizeof sizes - strlen(sizes), "%s%.17g", k ? "," : "", h[k]);
    strcat(sizes, "]");
    char res[1600];
    snprintf(res, sizeof res,
             "{\"name\":\"t\",\"question\":\"which displaces less\",\"refinement\":{\"element_sizes_mm\":%s,\"convergence_criterion\":0.02},"
             "\"material\":{\"id\":\"m\",\"status\":\"user_supplied\",\"source\":\"user\"},\"load\":{\"kind\":\"force\",\"force_n\":29.42,\"direction\":[0,0,-1]},"
             "\"mounting\":{\"idealization\":\"fixed\",\"source\":\"user\"},\"manufacturing\":{\"process\":\"unspecified\"},\"analysis\":{},"
             "\"designs\":[{\"name\":\"%s\"},{\"name\":\"%s\"}],\"retain_results\":\"refinement\",\"sensitivity\":{}}",
             sizes, na, nb);
    sj.resolved = parse(res);
    sj.check = parse("{\"equivalence\":{\"status\":\"equivalent\"},\"assumptions\":[],\"questions\":[],\"accepted\":[],\"warnings\":[],\"not_evaluated\":[],\"unsupported\":[]}");
    snprintf(sj.dir, sizeof sj.dir, "/nonexistent/evaltest");
    StudyDesign d[2];
    memset(d, 0, sizeof d);
    snprintf(d[0].name, sizeof d[0].name, "%s", na);
    snprintf(d[1].name, sizeof d[1].name, "%s", nb);
    JsonValue *levels[2] = {levels_of(n, h, ua, ea, valid_a), levels_of(n, h, ub, eb, NULL)}, *sens[2] = {json_array(), json_array()};
    JsonValue *failures = json_array();
    Built b;
    b.ev = study_build_evidence(&sj, d, 2, levels, sens, failures, "complete", 1.0);
    b.cmp = json_get(b.ev, "comparison");
    b.va = value_of(b.cmp, na);
    b.vb = value_of(b.cmp, nb);
    b.md = study_report_markdown(b.ev);
    json_free(levels[0]), json_free(levels[1]), json_free(sens[0]), json_free(sens[1]), json_free(failures);
    json_free(sj.resolved), json_free(sj.check);
    return b;
}

static void built_free(Built *b) {
    json_free(b->ev);
    free(b->md);
}

static bool no_uncertainty_wording(const char *s) { return s && !strstr(s, "\xc2\xb1") && !strstr(s, "uncertainty ±") && !strstr(s, "numerical uncertainty"); }

static const JsonValue *discretisation_row(const JsonValue *ev, const char *design) {
    const JsonValue *rows = json_get(ev, "numerical_evidence");
    char want[80];
    snprintf(want, sizeof want, "design %s,", design);
    for (size_t i = 0; i < json_len(rows); i++) {
        const JsonValue *r = json_at(rows, i);
        if (!strcmp(json_get_str(r, "check", ""), "discretisation (load-region displacement)") && strstr(json_get_str(r, "applies_to", ""), want)) return r;
    }
    return NULL;
}

static void test_refinement_estimates(void) {
    printf("== D1/D2 refinement conditions\n");
    const double h3[] = {4, 2, 1};
    /* design A of the rc1 example: exact geometry at every mesh */
    const double a3[] = {0.0247427, 0.0251846, 0.0253994}, zero3[] = {1e-12, 1e-12, 1e-12};
    JsonValue *r = study_refinement(h3, a3, zero3, 3, 0.02);
    CHECK(json_get_bool(r, "estimate_available", false), "A (3 meshes, exact geometry): estimate available");
    CHECK(json_get_num(r, "safety_factor", 0) == 3, "A: safety factor 3 with three meshes (got %g)", json_get_num(r, "safety_factor", 0));
    CHECK(fabs(json_get_num(r, "observed_order", 0) - 1.0407) < 2e-3, "A: observed order 1.04 (got %g)", json_get_num(r, "observed_order", 0));
    double expect = 3 * (0.0253994 - 0.0251846) / (pow(2, json_get_num(r, "observed_order", 0)) - 1);
    CHECK(fabs(json_get_num(r, "discretisation_error_estimate", 0) / expect - 1) < 1e-12, "A: GCI = 3 |f3 - f2| / (r^p - 1)");
    CHECK(json_get_bool(r, "convergence_criterion_met", false), "A: last change 0.85%% meets 2%%");
    CHECK(strstr(json_get_str(r, "change_definition", ""), "relative to the finer mesh") != NULL, "D2: change definition stated");
    CHECK(!json_get(r, "uncertainty") && !json_get(r, "relative_uncertainty"), "no 'uncertainty' keys");
    json_free(r);

    /* design B: staircase chamfer, volume error changes with the mesh */
    const double b3[] = {0.0097683, 0.0090846, 0.0087746}, eb3[] = {-3.57, -1.79, -0.89};
    r = study_refinement(h3, b3, eb3, 3, 0.02);
    CHECK(!json_get_bool(r, "estimate_available", true), "B (geometry changes with the mesh): no estimate");
    CHECK(json_get_num(r, "discretisation_error_estimate", -1) == -1, "B: no estimate value");
    const char *why = json_str(json_at(json_get(r, "estimate_unavailable_reasons"), 0));
    CHECK(why && strstr(why, "representation of the geometry changes with the mesh") && strstr(why, "-3.57%"), "B: the reason names the geometry representation (%s)",
          why ? why : "none");
    CHECK(!json_get_bool(r, "convergence_criterion_met", true), "B: 3.53%% change fails the 2%% criterion (reported, not dropped)");
    CHECK(fabs(json_get_num(r, "last_relative_change", 0) - (0.0087746 - 0.0090846) / 0.0087746) < 1e-12, "D2: B's change is relative to the finer value (-3.53%%)");
    CHECK(json_get(r, "observed_order") != NULL, "B: the observed order is still reported (%g)", json_get_num(r, "observed_order", NAN));
    json_free(r);

    /* four meshes (audit): A's orders 1.04 and 1.12 agree within 10 %: safety factor 1.25; B still without an estimate */
    const double h4[] = {4, 2, 1, 0.5}, a4[] = {0.0247427, 0.0251846, 0.0253994, 0.0254981}, zero4[] = {0, 0, 0, 0};
    r = study_refinement(h4, a4, zero4, 4, 0.02);
    CHECK(json_get_bool(r, "estimate_available", false) && json_get_num(r, "safety_factor", 0) == 1.25, "A (4 meshes, consistent orders): safety factor 1.25 (got %g)",
          json_get_num(r, "safety_factor", 0));
    CHECK(json_len(json_get(r, "observed_orders")) == 2, "A: two observed orders listed");
    json_free(r);
    const double a4x[] = {0.0247427, 0.0251846, 0.0253994, 0.0254400};
    r = study_refinement(h4, a4x, zero4, 4, 0.02);
    CHECK(json_get_bool(r, "estimate_available", false) && json_get_num(r, "safety_factor", 0) == 3, "orders that disagree (1.04 against 2.40): safety factor 3");
    json_free(r);

    /* each condition on its own */
    const double osc[] = {0.0250, 0.0252, 0.0251};
    r = study_refinement(h3, osc, zero3, 3, 0.02);
    CHECK(!json_get_bool(r, "estimate_available", true) && strstr(json_str(json_at(json_get(r, "estimate_unavailable_reasons"), 0)), "monotone"), "oscillating: no estimate");
    json_free(r);
    const double hvar[] = {4, 2, 1.5};
    r = study_refinement(hvar, a3, zero3, 3, 0.02);
    CHECK(!json_get_bool(r, "estimate_available", true) && strstr(json_str(json_at(json_get(r, "estimate_unavailable_reasons"), 0)), "ratio"), "ratios 2 and 1.33: no estimate");
    json_free(r);
    const double hsmall[] = {1.44, 1.2, 1.0};
    r = study_refinement(hsmall, a3, zero3, 3, 0.02);
    CHECK(!json_get_bool(r, "estimate_available", true), "ratio 1.2 (< 1.3): no estimate");
    json_free(r);
    const double steep[] = {0.020, 0.0252, 0.0253994};
    r = study_refinement(h3, steep, zero3, 3, 0.02);
    CHECK(!json_get_bool(r, "estimate_available", true) && strstr(json_str(json_at(json_get(r, "estimate_unavailable_reasons"), 0)), "observed order is outside"),
          "observed order 4.4 (outside [0.5, 4]): no estimate");
    json_free(r);
    r = study_refinement(h3, a3, NULL, 3, 0.02);
    CHECK(!json_get_bool(r, "estimate_available", true), "volume errors not recorded: no estimate");
    json_free(r);
    r = study_refinement(h3 + 1, a3 + 1, zero3, 2, 0.02);
    CHECK(!json_get_bool(r, "estimate_available", true) && json_get_bool(r, "convergence_criterion_met", false), "two meshes: criterion assessed, no estimate");
    json_free(r);
    r = study_refinement(h3, a3, zero3, 1, 0.02);
    CHECK(!json_get_bool(r, "estimate_available", true) && !json_get_bool(r, "convergence_criterion_met", true), "one mesh: neither");
    json_free(r);
}

static void test_outcomes(void) {
    printf("== D1 comparison outcomes\n");
    const double h3[] = {4, 2, 1}, zero3[] = {0, 0, 0};
    const double a3[] = {0.0247427, 0.0251846, 0.0253994}, b3[] = {0.0097683, 0.0090846, 0.0087746}, eb3[] = {-3.57, -1.79, -0.89};

    /* the rc1 bracket example: rc1 said "resolved" with "numerical uncertainty ±"; B has no defensible estimate */
    Built x = build("A", "B", 3, h3, a3, zero3, NULL, b3, eb3);
    const char *oc = json_get_str(x.cmp, "outcome", "");
    CHECK(!strcmp(oc, "ranking_consistent_on_tested_meshes"), "rc1 example: ranking_consistent_on_tested_meshes (got %s)", oc);
    CHECK(!strcmp(json_str(json_at(json_get(x.cmp, "ranking"), 0)), "B"), "B ranked first");
    CHECK(json_get_bool(x.cmp, "ranking_consistent_on_tested_meshes", false) && json_get_int(x.cmp, "meshes_compared", 0) == 3, "ranking the same on the 3 meshes");
    CHECK(json_get_bool(x.va, "estimate_available", false) && !json_get_bool(x.vb, "estimate_available", true), "estimate for A only");
    CHECK(!json_get_bool(x.vb, "convergence_criterion_met", true) && !json_get_bool(x.cmp, "convergence_criterion_met_by_all", true), "B's failed criterion kept");
    const char *st = json_get_str(x.cmp, "statement", "");
    CHECK(strstr(st, "not met by design B (-3.53%)") != NULL, "statement names B's failed criterion: %s", st);
    CHECK(strstr(st, "No discretisation-error estimate is available for design B") != NULL, "statement says why the size is not estimated");
    CHECK(strstr(st, "60.5% to 65.5%") != NULL, "statement gives the difference over the tested meshes");
    CHECK(no_uncertainty_wording(st), "no ± or uncertainty wording in the statement");
    const JsonValue *row = discretisation_row(x.ev, "B");
    CHECK(row && !strcmp(json_get_str(row, "outcome", ""), "fail"), "numerical evidence: B's discretisation row fails");
    CHECK(x.md && no_uncertainty_wording(x.md), "report: no ± or 'numerical uncertainty'");
    CHECK(x.md && strstr(x.md, "**not met**") && strstr(x.md, "not available: ") && strstr(x.md, "Ranking on the tested meshes"), "report shows the three conclusions");
    CHECK(json_get_int(x.ev, "format_version", 0) == 2, "evidence format version 2");
    built_free(&x);

    /* the four-level audit: B meets the criterion at 0.5 mm, still no estimate; A's safety factor 1.25 */
    const double h4[] = {4, 2, 1, 0.5}, a4[] = {0.0247427, 0.0251846, 0.0253994, 0.0254981}, b4[] = {0.0097683, 0.0090846, 0.0087746, 0.0086276};
    const double zero4[] = {0, 0, 0, 0}, eb4[] = {-3.57, -1.79, -0.89, -0.45};
    x = build("A", "B", 4, h4, a4, zero4, NULL, b4, eb4);
    CHECK(!strcmp(json_get_str(x.cmp, "outcome", ""), "ranking_consistent_on_tested_meshes"), "four levels: ranking_consistent_on_tested_meshes (%s)", json_get_str(x.cmp, "outcome", ""));
    CHECK(json_get_bool(x.cmp, "convergence_criterion_met_by_all", false), "four levels: every design meets the criterion (B -1.70%%)");
    CHECK(json_get_num(x.va, "safety_factor", 0) == 1.25, "four levels: A with safety factor 1.25");
    built_free(&x);

    /* cantilevers of case R1: exact geometry, estimates for both, difference far larger: resolved */
    const double hb[] = {2.5, 1.25, 0.625}, c10[] = {0.0199622, 0.0199973, 0.0200115}, c20[] = {0.1588127, 0.1590618, 0.1591834};
    x = build("C10", "C20", 3, hb, c10, zero3, NULL, c20, zero3);
    CHECK(!strcmp(json_get_str(x.cmp, "outcome", ""), "resolved"), "cantilevers: resolved (%s)", json_get_str(x.cmp, "outcome", ""));
    CHECK(json_get_bool(x.cmp, "estimates_available", false) && json_get_num(x.cmp, "combined_discretisation_error_estimate", 0) > 0, "cantilevers: combined estimate");
    CHECK(no_uncertainty_wording(json_get_str(x.cmp, "statement", "")) && strstr(json_get_str(x.cmp, "statement", ""), "estimate, not a bound"), "resolved statement wording");
    built_free(&x);

    /* a 0.07 % difference (case E9): both meet the criterion, one staircase design: too small to distinguish */
    double a9[3], e9[] = {-0.5, -0.3, -0.1};
    for (int k = 0; k < 3; k++) a9[k] = a3[k] * (1 - 0.0007);
    x = build("A", "A_chamfer", 3, h3, a3, zero3, NULL, a9, e9);
    CHECK(!strcmp(json_get_str(x.cmp, "outcome", ""), "too_small_to_distinguish"), "E9-like: too_small_to_distinguish (%s)", json_get_str(x.cmp, "outcome", ""));
    built_free(&x);

    /* the ranking flips between meshes and one design has no estimate: not resolved, whatever the finest difference */
    const double fa[] = {0.0200, 0.0210, 0.0211}, fb[] = {0.0205, 0.0206, 0.0205}, fe[] = {-1, -0.5, -0.25};
    x = build("A", "B", 3, h3, fa, zero3, NULL, fb, fe);
    CHECK(!strcmp(json_get_str(x.cmp, "outcome", ""), "not_resolved") && !json_get_bool(x.cmp, "ranking_consistent_on_tested_meshes", true),
          "ranking changes between meshes: not_resolved (%s)", json_get_str(x.cmp, "outcome", ""));
    built_free(&x);

    /* only one valid mesh for a design */
    const int valid[] = {0, 0, 1};
    x = build("A", "B", 3, h3, a3, zero3, valid, b3, eb3);
    CHECK(!strcmp(json_get_str(x.cmp, "outcome", ""), "not_resolved") && strstr(json_get_str(x.cmp, "statement", ""), "only one valid mesh"),
          "one valid mesh: not_resolved (%s)", json_get_str(x.cmp, "outcome", ""));
    built_free(&x);
}

static void test_roots(void) {
    printf("== D3 access roots\n");
    EngineConfig c;
    engine_config_default(&c);
    char err[8192];
    int n0 = c.nread_roots;
    CHECK(engine_config_add_root(&c, false, "/private/tmp/navier-evaltest/inside", err, sizeof err) && c.nread_roots == n0,
          "a folder inside /private/tmp takes no read slot (%d -> %d)", n0, c.nread_roots);
    CHECK(engine_config_add_root(&c, false, "/tmp/navier-evaltest/x", err, sizeof err) && c.nread_roots == n0, "/tmp/... is covered by the canonical /private/tmp");
    int w0 = c.nwrite_roots, r0 = c.nread_roots;
    CHECK(engine_config_add_root(&c, true, "/usr/navier-evaltest-write", err, sizeof err) && c.nwrite_roots == w0 + 1 && c.nread_roots == r0 + 1,
          "a new write root is also readable");
    int k = 0;
    while (c.nread_roots < ENGINE_MAX_ROOTS && k < 64) {
        char p[64];
        snprintf(p, sizeof p, "/usr/navier-evaltest-%d", k++);
        engine_config_add_root(&c, false, p, err, sizeof err);
    }
    CHECK(c.nread_roots == ENGINE_MAX_ROOTS && ENGINE_MAX_ROOTS >= 16, "read roots filled to the limit %d", ENGINE_MAX_ROOTS);
    int full = c.nread_roots;
    CHECK(engine_config_add_root(&c, false, "/usr/navier-evaltest-0/sub", err, sizeof err) && c.nread_roots == full, "at the limit, a folder inside a root is still accepted");
    bool ok = engine_config_add_root(&c, false, "/usr/navier-evaltest-over", err, sizeof err);
    CHECK(!ok && strstr(err, "limit of 16 read roots") && strstr(err, "common parent folder"), "over the limit refused with a clear message: %s", err);
    CHECK(strstr(err, "Roots in use") != NULL, "the message lists the roots in use");
    int wb = c.nwrite_roots;
    ok = engine_config_add_root(&c, true, "/usr/navier-evaltest-write-over", err, sizeof err);
    CHECK(!ok && c.nwrite_roots == wb && strstr(err, "implied by a write root"), "a write root that cannot be readable is refused and withdrawn: %s", err);
    CHECK(!engine_config_add_root(&c, false, "", err, sizeof err) && strstr(err, "not a valid path"), "invalid path message");
}

static void test_storage(void) {
    printf("== D4 storage estimates\n");
    double b = study_run_bytes(40960);
    CHECK(b >= 26155116 && b <= 1.2 * 26155116, "40,960 elements: %.0f bytes against 26,155,116 measured", b);
    b = study_run_bytes(6600);
    CHECK(b >= 4406865 && b <= 1.2 * 4406865, "6,600 elements: %.0f bytes against 4,406,865 measured", b);
    char at[4096];
    double f = study_free_bytes("/nonexistent-navier-evaltest/a/b", at, sizeof at);
    CHECK(f > 0 && !strcmp(at, "/"), "free space of a missing folder measured at its nearest existing ancestor (%s)", at);
    JsonValue *res = parse("{\"refinement\":{\"element_sizes_mm\":[4,2,1]},\"retain_results\":\"refinement\",\"limits\":{\"max_elements\":400000},"
                           "\"sensitivity\":{\"poisson_ratio\":[0.3,0.36],\"youngs_modulus_relative\":[0.9,1.1]},\"designs\":[{\"name\":\"A\"},{\"name\":\"B\"}]}");
    const double vol[] = {14528, 18490};
    JsonValue *plan = study_storage_plan(res, vol, 2, "/");
    double per = 0;
    for (int i = 0; i < 2; i++)
        for (double h = 4; h >= 1; h /= 2) per += study_run_bytes(vol[i] / (h * h * h));
    double retained = json_get_num(plan, "estimated_retained_bytes", 0), peak = json_get_num(plan, "estimated_peak_bytes", 0);
    CHECK(fabs(retained - (per + 2 * 1024 * 1024)) < 1, "refinement retention: every level plus 2 MB (%.0f)", retained);
    CHECK(peak > retained && fabs(peak - retained - study_run_bytes(vol[1])) < 1, "peak adds the largest removed field");
    CHECK(json_get(plan, "free_bytes") && json_get_bool(plan, "sufficient", false), "small study fits");
    json_free(plan);
    json_set_string(res, "retain_results", "none");
    plan = study_storage_plan(res, vol, 2, "/");
    CHECK(json_get_num(plan, "estimated_retained_bytes", 0) == 2 * 1024 * 1024, "retain none: only the record");
    json_free(plan);
    JsonValue *big = parse("{\"refinement\":{\"element_sizes_mm\":[0.02,0.01]},\"retain_results\":\"all\",\"limits\":{\"max_elements\":2000000},\"sensitivity\":{},"
                           "\"designs\":[{\"name\":\"A\"},{\"name\":\"B\"}]}");
    const double vbig[] = {1.5, 1.5}; /* 1.5 mm3 at 0.01 mm: 1.5 M elements, about 1 GB per analysis */
    plan = study_storage_plan(big, vbig, 2, "/");
    double need = json_get_num(plan, "estimated_peak_bytes", 0), free_b = json_get_num(plan, "free_bytes", 0);
    CHECK(json_get_bool(plan, "sufficient", true) == (free_b - study_storage_reserve_bytes() >= need), "sufficiency follows free space and reserve");
    json_free(plan);
    json_free(big);
    json_free(res);
}

int main(void) {
    test_refinement_estimates();
    test_outcomes();
    test_roots();
    test_storage();
    printf("evaltest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
