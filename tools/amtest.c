/* amtest.c - end-to-end verification of the control-to-solver path through the typed operation layer (the path
 * shared by the terminal, the UI, the control socket and MCP):
 *   STL import -> selections -> material -> voxel mesh -> boundary conditions -> validation -> static solve job ->
 *   result queries, probes, images and exports,
 * compared with beam theory, plus conflicts, staleness, gravity, formulation, idempotent retries, duplicate detection,
 * cancellation, and reloading results from the run directory in a fresh engine.
 *   make build/amtest && ./build/amtest */
#include "../src/ctl/engine.h"
#include "../src/ctl/ops.h"
#include "../src/geom/mesh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static const OpCaller CALLER = {"test", "amtest"};

static OpResult run(Engine *e, const char *op, const char *json) {
    OpResult r;
    JsonError jerr;
    JsonValue *params = json ? json_parse(json, strlen(json), NULL, &jerr) : NULL;
    if (json && !params) printf("  (bad test JSON for %s: %s)\n", op, jerr.message);
    ops_invoke(e, op, params, &CALLER, &r);
    json_free(params);
    return r;
}

static const char *err_code(const OpResult *r) { return r->ok ? "OK" : json_get_str(r->error, "code", "?"); }
static const char *err_msg(const OpResult *r) { return r->ok ? "" : json_get_str(r->error, "message", ""); }

static const JsonValue *at(const JsonValue *v, const char *dotted) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", dotted);
    for (char *tok = strtok(buf, "."); tok && v; tok = strtok(NULL, ".")) {
        char *end;
        long idx = strtol(tok, &end, 10);
        v = (*end == 0 && v->type == JSON_ARRAY) ? json_at(v, (size_t)idx) : json_get(v, tok);
    }
    return v;
}

static double num(const JsonValue *v, const char *dotted) {
    const JsonValue *x = at(v, dotted);
    return x && x->type == JSON_NUMBER ? x->u.number : NAN;
}

static bool has_code(const JsonValue *arr, const char *code) {
    for (size_t i = 0; i < json_len(arr); i++)
        if (!strcmp(json_get_str(json_at(arr, i), "code", ""), code)) return true;
    return false;
}

static void add_box(Mesh *m, double x0, double y0, double z0, double x1, double y1, double z1) {
    vec3 p[8];
    for (int i = 0; i < 8; i++) p[i] = v3((float)(i & 1 ? x1 : x0), (float)(i & 2 ? y1 : y0), (float)(i & 4 ? z1 : z0));
    static const int q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++) mesh_add_quad(m, p[q[f][0]], p[q[f][1]], p[q[f][2]], p[q[f][3]]);
}

/* waits for a job to end; returns its final status (caller frees) */
static JsonValue *wait_job(Engine *e, const char *id) {
    char json[256];
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"wait_seconds\": 60}", id);
    for (int i = 0; i < 10; i++) {
        OpResult r = run(e, "job_status", json);
        if (!r.ok) {
            printf("  (job_status failed: %s)\n", err_msg(&r));
            op_result_free(&r);
            return NULL;
        }
        const char *st = json_get_str(r.value, "state", "");
        if (strcmp(st, "queued") && strcmp(st, "running")) {
            JsonValue *v = json_clone(r.value);
            op_result_free(&r);
            return v;
        }
        op_result_free(&r);
    }
    return NULL;
}

static bool submit(Engine *e, const char *params, char *id, size_t cap) {
    OpResult r = run(e, "analysis_run", params);
    bool ok = r.ok;
    if (ok) snprintf(id, cap, "%s", json_get_str(r.value, "job_id", ""));
    else printf("  (analysis_run failed: %s %s)\n", err_code(&r), err_msg(&r));
    op_result_free(&r);
    return ok;
}

static double probe_uz(Engine *e, const char *id, double x, double y, double z) {
    char json[256];
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"points_mm\": [[%g, %g, %g]]}", id, x, y, z);
    OpResult r = run(e, "results_probe", json);
    double v = r.ok ? num(r.value, "probes.0.displacement_mm.z") : NAN;
    op_result_free(&r);
    return v;
}

int main(void) {
    char base[1024], ws[1200], stl[1300], json[4096], cmd[2600];
    const char *tmpdir = getenv("TMPDIR");
    snprintf(base, sizeof base, "%s/nv_amtest_%ld", tmpdir ? tmpdir : "/tmp", (long)getpid());
    snprintf(ws, sizeof ws, "%s/workspace", base);
    snprintf(stl, sizeof stl, "%s/beam.stl", base);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", base);
    if (system(cmd) != 0) return 2;
    Mesh m;
    mesh_init(&m);
    add_box(&m, 0, 0, 0, 100, 10, 10); /* cantilever 100 x 10 x 10 mm */
    char err[512];
    if (!mesh_save_stl(stl, &m, err, sizeof err)) return 2;
    mesh_free(&m);

    EngineConfig cfg;
    engine_config_default(&cfg);
    CHECK(engine_config_set_workspace(&cfg, ws, err, sizeof err), "workspace: %s", err);
    CHECK(engine_config_add_root(&cfg, false, base, err, sizeof err), "read root");
    Engine *e = engine_create(&cfg, err, sizeof err);
    CHECK(e != NULL, "engine_create: %s", err);
    if (!e) return 1;

    printf("== geometry and selections\n");
    OpResult r = run(e, "project_create", "{\"name\": \"beam_case\"}");
    CHECK(r.ok, "project_create: %s", err_msg(&r));
    char project_dir[1400];
    snprintf(project_dir, sizeof project_dir, "%s", r.ok ? json_get_str(json_get(r.value, "project"), "directory", "") : "");
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"path\": \"%s\", \"units\": \"mm\", \"name\": \"beam\"}", stl);
    r = run(e, "geometry_import", json);
    CHECK(r.ok, "import: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "surfaces_list", "{\"body\": \"beam\"}");
    CHECK(r.ok && json_get_int(r.value, "patches_total", 0) == 6, "box has 6 planar patches (%lld)", r.ok ? json_get_int(r.value, "patches_total", 0) : -1LL);
    op_result_free(&r);
    r = run(e, "selection_create", "{\"name\": \"root\", \"query\": {\"extreme\": {\"direction\": \"-x\"}}, \"description\": \"clamped end\", \"source\": \"user\"}");
    CHECK(r.ok && fabs(num(r.value, "selection.area_mm2") - 100) < 1e-6, "root face 100 mm2: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "selection_create", "{\"name\": \"tip\", \"query\": {\"extreme\": {\"direction\": \"+x\"}}, \"source\": \"user\"}");
    CHECK(r.ok && fabs(num(r.value, "selection.centroid_mm.0") - 50) < 1e-9, "tip face at x = +50 mm: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "selection_create", "{\"name\": \"top\", \"query\": {\"facing\": {\"direction\": \"up\"}}, \"source\": \"user\"}");
    CHECK(r.ok && fabs(num(r.value, "selection.area_mm2") - 1000) < 1e-6, "top face 1000 mm2");
    op_result_free(&r);
    r = run(e, "view_render", "{\"highlight\": [\"root\", \"tip\"], \"width\": 480, \"height\": 320}");
    CHECK(r.ok && r.nimages == 1 && r.images[0].len > 8 && !memcmp(r.images[0].data, "\x89PNG", 4), "view_render returns a PNG");
    char view_id[32];
    snprintf(view_id, sizeof view_id, "%s", r.ok ? json_get_str(r.value, "view_id", "") : "");
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"view_id\": \"%s\", \"pixels\": [[0, 0]]}", view_id);
    r = run(e, "view_pick", json);
    CHECK(r.ok && !json_get_bool(json_at(json_get(r.value, "hits"), 0), "hit", true), "corner pixel is background");
    op_result_free(&r);
    /* a known top-down camera: the image centre looks at the middle of the top face */
    r = run(e, "view_render", "{\"camera\": {\"eye_mm\": [0, 0, 200], \"target_mm\": [0, 0, 0], \"up\": [0, 1, 0], \"fov_deg\": 30}, \"width\": 480, \"height\": 320}");
    snprintf(view_id, sizeof view_id, "%s", r.ok ? json_get_str(r.value, "view_id", "") : "");
    CHECK(r.ok && fabs(num(r.value, "camera.eye_mm.2") - 200) < 1e-9, "explicit camera echoed: %s", err_msg(&r));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"view_id\": \"%s\", \"pixels\": [[240, 160], [2, 2]]}", view_id);
    r = run(e, "view_pick", json);
    const JsonValue *hit = at(r.value, "hits.0");
    CHECK(r.ok && json_get_bool(hit, "hit", false) && fabs(num(hit, "point_mm.2") - 10) < 1e-6 && fabs(num(hit, "normal.2") - 1) < 1e-9,
          "centre pixel hits the top face at z = 10 mm (%g)", num(hit, "point_mm.2"));
    CHECK(r.ok && !json_get_bool(at(r.value, "hits.1"), "hit", true), "border pixel misses the part");
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"name\": \"picked\", \"query\": {\"pick\": {\"view_id\": \"%s\", \"pixel\": [240, 160]}}, \"description\": \"face the user clicked\", \"source\": \"user\"}",
             view_id);
    r = run(e, "selection_create", json);
    CHECK(r.ok && fabs(num(r.value, "selection.area_mm2") - 1000) < 1e-6, "picked selection is the whole top face: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "selection_list", "{\"detailed\": true}");
    const JsonValue *sels = json_get(r.value, "selections");
    const JsonValue *picked = NULL;
    for (size_t i = 0; i < json_len(sels); i++)
        if (!strcmp(json_get_str(json_at(sels, i), "name", ""), "picked")) picked = json_at(sels, i);
    CHECK(picked && json_get(json_get(picked, "query"), "patches"), "the pick is stored as patch ids, reproducible without the view");
    op_result_free(&r);
    r = run(e, "selection_delete", "{\"name\": \"picked\"}");
    CHECK(r.ok, "selection_delete: %s", err_msg(&r));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"name\": \"stale_pick\", \"query\": {\"pick\": {\"view_id\": \"view-999\", \"pixel\": [1, 1]}}, \"source\": \"user\"}");
    r = run(e, "selection_create", json);
    CHECK(!r.ok, "pick on an unknown view fails: %s", err_msg(&r));
    op_result_free(&r);

    printf("== materials and mesh\n");
    r = run(e, "setup_validate", "{}");
    CHECK(r.ok && !json_get_bool(r.value, "ready", true) && has_code(json_get(r.value, "errors"), "PRECONDITION_FAILED"), "no mesh: not ready");
    op_result_free(&r);
    r = run(e, "materials_list", "{}");
    size_t nlib = r.ok ? json_len(json_get(r.value, "materials")) : 0;
    /* Every library record is demonstration data, or labelled measured or published, which the loader accepts only
     * when each of its values names its source (matlib.c; the first such record arrived on 2026-09-18). A record that
     * breaks that rule stops the whole library from loading, so nlib would be 0 here. */
    bool labelled = nlib > 0;
    int ndemo = 0;
    for (size_t i = 0; i < nlib; i++) {
        const char *st = json_get_str(json_at(json_get(r.value, "materials"), i), "status", "");
        ndemo += !strcmp(st, "demonstration");
        labelled &= !strcmp(st, "demonstration") || !strcmp(st, "measured") || !strcmp(st, "published");
    }
    /* The number of demonstration records falls by design: each batch of the library replaces them with sourced records
     * (materials/README.md), and one is kept only where a test assigns it by id. What must hold is the labelling. */
    CHECK(nlib >= 5 && labelled && ndemo >= 1, "library lists %zu materials, %d demonstration and the rest measured or "
          "published with a source per value", nlib, ndemo);
    op_result_free(&r);
    r = run(e, "material_assign", "{\"body\": \"beam\", \"material\": \"unobtainium\", \"source\": \"user\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "NOT_FOUND") && json_get(json_get(r.error, "details"), "available_ids"), "unknown material rejected with ids");
    op_result_free(&r);
    r = run(e, "material_define",
            "{\"material\": {\"id\": \"steel_test\", \"name\": \"steel, test values\", \"family\": \"metal\", \"status\": \"user_supplied\", \"provenance\": "
            "\"amtest: E = 200 GPa, nu = 0.3, rho = 7850 kg/m3\", \"youngs_modulus_pa\": {\"value\": 200}, \"poisson_ratio\": {\"value\": 0.3}}}");
    CHECK(!r.ok && !strcmp(err_code(&r), "INVALID_PARAMS") && strstr(err_msg(&r), "plausible"), "E = 200 Pa (GPa typed as Pa) rejected: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "material_define",
            "{\"material\": {\"id\": \"steel_test\", \"name\": \"steel, test values\", \"family\": \"metal\", \"status\": \"user_supplied\", \"provenance\": "
            "\"amtest: E = 200 GPa, nu = 0.3, rho = 7850 kg/m3\", \"youngs_modulus_pa\": {\"value\": 200e9}, \"poisson_ratio\": {\"value\": 0.3}, "
            "\"density_kg_m3\": {\"value\": 7850}}}");
    CHECK(r.ok, "material_define: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "material_assign", "{\"body\": \"beam\", \"material\": \"steel_test\", \"source\": \"user\"}");
    CHECK(r.ok, "material_assign: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "mesh_generate", "{\"element_size\": 2.5}");
    CHECK(r.ok && num(r.value, "mesh.elements") == 640 && num(r.value, "mesh.nodes") == 1025, "40 x 4 x 4 elements, 1025 nodes: %s", err_msg(&r));
    CHECK(r.ok && fabs(num(r.value, "mesh.bodies.0.volume_error_percent")) < 1e-9, "box volume exact on an aligned grid");
    op_result_free(&r);
    r = run(e, "setup_validate", "{}");
    CHECK(r.ok && !json_get_bool(r.value, "ready", true) && has_code(json_get(r.value, "errors"), "INSUFFICIENT_CONSTRAINTS"), "no supports: not ready");
    op_result_free(&r);

    printf("== boundary conditions\n");
    r = run(e, "boundary_apply", "{\"name\": \"clamp\", \"kind\": \"fixed\", \"selection\": \"root\", \"source\": \"user\"}");
    CHECK(r.ok, "fixed support: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "boundary_apply", "{\"name\": \"load\", \"kind\": \"force\", \"selection\": \"tip\", \"force\": [0, 0, \"-0.1 kN\"], \"source\": \"user\"}");
    CHECK(r.ok && num(r.value, "boundary_condition.mesh_faces") == 16 && fabs(num(r.value, "boundary_condition.force_n.2") + 100) < 1e-9,
          "100 N tip load on 16 mesh faces: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "boundary_apply", "{\"name\": \"lift\", \"kind\": \"displacement\", \"selection\": \"root\", \"displacement\": {\"z\": 1}, \"source\": \"user\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "CONFLICTING_BOUNDARY_CONDITIONS"), "conflicting support on the clamped face: %s", err_code(&r));
    op_result_free(&r);
    r = run(e, "boundary_apply", "{\"name\": \"x\", \"kind\": \"force\", \"selection\": \"tip\", \"source\": \"user\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "INVALID_PARAMS") && strstr(err_msg(&r), "force"), "force value required: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "boundary_apply", "{\"name\": \"x\", \"kind\": \"pressure\", \"selection\": \"tip\", \"pressure\": \"2 kg\", \"source\": \"user\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "INVALID_PARAMS") && strstr(err_msg(&r), "pressure"), "pressure with a mass unit rejected by the schema: %s",
          err_msg(&r));
    op_result_free(&r);
    r = run(e, "setup_validate", "{}");
    CHECK(r.ok && json_get_bool(r.value, "ready", false) && strlen(json_get_str(r.value, "spec_hash", "")) == 64, "setup ready with a specification hash");
    op_result_free(&r);

    printf("== static solve and beam theory\n");
    char id[64] = "", id2[64] = "";
    CHECK(submit(e, "{\"idempotency_key\": \"run-1\"}", id, sizeof id), "analysis_run");
    r = run(e, "analysis_run", "{\"idempotency_key\": \"run-1\"}");
    CHECK(r.ok && r.replayed && !strcmp(json_get_str(r.value, "job_id", ""), id), "retry with the same key returns the same job");
    op_result_free(&r);
    JsonValue *st = wait_job(e, id);
    CHECK(st && !strcmp(json_get_str(st, "state", ""), "succeeded"), "job succeeded: %s", st ? json_get_str(at(st, "error"), "message", "") : "no status");
    const JsonValue *checks = at(st, "summary.checks");
    CHECK(json_get_bool(checks, "equilibrium_ok", false) && json_get_bool(checks, "energy_ok", false), "equilibrium %g, energy ratio %g",
          num(checks, "equilibrium_error"), num(checks, "energy_ratio"));
    CHECK(fabs(num(checks, "reaction_total_n.2") - 100) < 1e-6 && fabs(num(checks, "applied_load_total_n.2") + 100) < 1e-9, "support reaction +100 N (%.9g)",
          num(checks, "reaction_total_n.2"));
    json_free(st);
    const double E = 200e9, nu = 0.3, F = 100, L = 0.1, b = 0.01, hgt = 0.01, I = b * hgt * hgt * hgt / 12, G = E / (2 * (1 + nu));
    double timoshenko = 1e3 * (F * L * L * L / (3 * E * I) + F * L / (5.0 / 6.0 * G * b * hgt));
    double uz = probe_uz(e, id, 50, 0, 5);
    CHECK(fabs(-uz - timoshenko) / timoshenko < 0.03, "tip deflection %.5f mm vs Timoshenko %.5f mm (%.2f%%)", -uz, timoshenko, 100 * (-uz - timoshenko) / timoshenko);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"points_mm\": [[0, 0, 10], [0, 0, 0], [500, 0, 0]]}", id);
    r = run(e, "results_probe", json);
    double sig_top = r.ok ? num(r.value, "probes.0.stress_mpa_interpolated_nodal_average.xx") : NAN;
    double sig_bot = r.ok ? num(r.value, "probes.1.stress_mpa_interpolated_nodal_average.xx") : NAN;
    double sig_theory = 1e-6 * F * (L / 2) * (hgt / 2) / I;
    CHECK(fabs(sig_top - sig_theory) / sig_theory < 0.05 && fabs(sig_bot + sig_theory) / sig_theory < 0.05, "mid-span bending stress %+.3f / %+.3f MPa vs +-%.3f MPa",
          sig_top, sig_bot, sig_theory);
    CHECK(r.ok && !json_get_bool(at(r.value, "probes.2"), "inside", true), "point outside the part reported as outside");
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"quantity\": \"von_mises\"}", id);
    r = run(e, "results_query", json);
    const JsonValue *peak = at(r.value, "largest.0");
    CHECK(r.ok && (json_get_bool(peak, "at_supported_node", false) || json_get_bool(peak, "next_to_support", false)) &&
              json_len(json_get(r.value, "notes")) == 2 && num(peak, "location_mm.0") < -45,
          "peak von Mises (%.4g MPa at x = %.4g mm) flagged at the clamp", num(peak, "value"), num(peak, "location_mm.0"));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"quantity\": \"displacement\", \"component\": \"z\"}", id);
    r = run(e, "results_query", json);
    CHECK(r.ok && fabs(num(r.value, "smallest.0.value") - uz) < 0.02 * fabs(uz), "most negative u_z %.5f mm at the tip", num(r.value, "smallest.0.value"));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"quantity\": \"stress\", \"component\": \"xx\", \"selection\": \"top\"}", id);
    r = run(e, "results_query", json);
    CHECK(r.ok && num(r.value, "count") == 41 * 5 && num(r.value, "statistics.max") > 0, "top-surface scope: %s", err_msg(&r));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"quantity\": \"strain\", \"component\": \"xx\", \"value_kind\": \"gauss_point\"}", id);
    r = run(e, "results_query", json);
    CHECK(r.ok && num(r.value, "statistics.max") > 1e-4 && num(r.value, "statistics.max") < 1e-3, "Gauss-point strain range: %g", num(r.value, "statistics.max"));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"quantity\": \"strain\", \"component\": \"xx\"}", id);
    r = run(e, "results_query", json);
    CHECK(!r.ok && !strcmp(err_code(&r), "INVALID_PARAMS"), "strain needs Gauss points");
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"width\": 480, \"height\": 320}", id);
    r = run(e, "results_render", json);
    CHECK(r.ok && r.nimages == 1 && !memcmp(r.images[0].data, "\x89PNG", 4) && num(r.value, "deformation.scale") > 1, "result image with magnified deformation");
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\"}", id);
    r = run(e, "results_export", json);
    CHECK(r.ok && json_len(json_get(r.value, "files")) == 3, "exports: %s", err_msg(&r));
    for (size_t i = 0; r.ok && i < json_len(json_get(r.value, "files")); i++) {
        const JsonValue *f = json_at(json_get(r.value, "files"), i);
        CHECK(access(json_get_str(f, "path", ""), R_OK) == 0 && strlen(json_get_str(f, "sha256", "")) == 64, "%s written with hash", json_get_str(f, "format", ""));
        if (!strcmp(json_get_str(f, "format", ""), "csv")) {
            FILE *fp = fopen(json_get_str(f, "path", ""), "r");
            int lines = 0, c;
            while (fp && (c = fgetc(fp)) != EOF) lines += c == '\n';
            if (fp) fclose(fp);
            CHECK(lines == 1026, "CSV has a header and 1025 node rows (%d lines)", lines);
        }
    }
    op_result_free(&r);

    printf("== conforming tetrahedral mesh (TET10), the same beam\n");
    r = run(e, "mesh_generate", "{\"method\": \"tet\", \"surface_size\": 2.5}");
    CHECK(r.ok && !strcmp(json_get_str(at(r.value, "mesh"), "method", ""), "tet10") && num(r.value, "mesh.quality.min_dihedral_deg") >= 10 &&
              num(r.value, "mesh.quality.face_connected_regions") == 1,
          "TET10 mesh, one region, smallest dihedral %.1f degrees: %s", r.ok ? num(r.value, "mesh.quality.min_dihedral_deg") : 0, err_msg(&r));
    CHECK(r.ok && fabs(num(r.value, "mesh.bodies.0.volume_error_percent")) < 1e-9, "box volume exact: its faces lie on lattice planes");
    op_result_free(&r);
    char jtet[64] = "";
    CHECK(submit(e, "{\"label\": \"tet10\"}", jtet, sizeof jtet), "static analysis on the tetrahedral mesh");
    st = wait_job(e, jtet);
    CHECK(st && !strcmp(json_get_str(st, "state", ""), "succeeded") && json_get_bool(at(st, "summary.checks"), "equilibrium_ok", false),
          "TET10 job succeeded with equilibrium: %s", st ? json_get_str(at(st, "error"), "message", "") : "no status");
    json_free(st);
    double uzt = probe_uz(e, jtet, 50, 0, 5);
    printf("  TET10 tip deflection %.5f mm, Timoshenko %.5f mm (%+.2f%%)\n", -uzt, timoshenko, 100 * (-uzt - timoshenko) / timoshenko);
    CHECK(fabs(-uzt - timoshenko) / timoshenko < 0.01, "TET10 tip deflection %.5f mm vs Timoshenko %.5f mm (%.2f%%)", -uzt, timoshenko,
          100 * (-uzt - timoshenko) / timoshenko);
    r = run(e, "analysis_run", "{\"analysis\": \"transient_thermal\", \"end_time\": 1, \"time_step\": 1}");
    {
        char why[600];
        snprintf(why, sizeof why, "%s", err_msg(&r));
        const JsonValue *errs = json_get(json_get(r.error, "details"), "errors");
        for (size_t k = 0; k < json_len(errs); k++)
            if (strstr(json_get_str(json_at(errs, k), "message", ""), "tetrahedral")) snprintf(why, sizeof why, "%s", json_get_str(json_at(errs, k), "message", ""));
        CHECK(!r.ok && strstr(why, "tetrahedral"), "a thermal analysis refuses the tetrahedral mesh: %s", why);
    }
    op_result_free(&r);
    r = run(e, "mesh_generate", "{\"method\": \"voxel\", \"element_size\": 2.5}");
    CHECK(r.ok && num(r.value, "mesh.elements") == 640, "back to the voxel mesh for the process analyses: %s", err_msg(&r));
    op_result_free(&r);

    printf("== formulation, gravity\n");
    CHECK(submit(e, "{\"formulation\": \"full_integration\"}", id2, sizeof id2), "full integration run");
    st = wait_job(e, id2);
    json_free(st);
    double uz_full = probe_uz(e, id2, 50, 0, 5);
    /* cubic elements, four through the depth: full integration locks mildly (a few percent), incompatible modes do not */
    CHECK(-uz_full < 0.99 * -uz && -uz_full > 0.8 * -uz, "full integration is stiffer in bending (shear locking): %.5f vs %.5f mm", -uz_full, -uz);
    r = run(e, "boundary_apply", "{\"name\": \"gravity\", \"kind\": \"gravity\", \"acceleration\": [0, 0, -9.81], \"source\": \"user\"}");
    CHECK(r.ok, "gravity: %s", err_msg(&r));
    op_result_free(&r);
    char id3[64] = "";
    CHECK(submit(e, "{}", id3, sizeof id3), "run with gravity");
    st = wait_job(e, id3);
    double weight = 7850 * 100e-3 * 10e-3 * 10e-3 * 9.81;
    CHECK(st && fabs(num(st, "summary.checks.reaction_total_n.2") - (100 + weight)) < 1e-6 * 100, "reaction includes the weight %.6f N (%.9f)", weight,
          num(st, "summary.checks.reaction_total_n.2"));
    json_free(st);

    printf("== transient thermal through the operation layer\n");
    r = run(e, "material_define",
            "{\"material\": {\"id\": \"steel_thermal\", \"name\": \"steel with thermal properties (test)\", \"family\": \"metal\", \"status\": "
            "\"user_supplied\", \"provenance\": \"amtest: k = 15 W/mK, cp = 500 J/kgK, alpha = 1.2e-5 1/K\", \"youngs_modulus_pa\": {\"value\": 200e9}, "
            "\"poisson_ratio\": {\"value\": 0.3}, \"density_kg_m3\": {\"value\": 7850}, \"conductivity_w_per_mk\": {\"value\": 15}, "
            "\"specific_heat_j_per_kgk\": {\"value\": 500}, \"expansion_1_per_k\": {\"value\": 1.2e-5}}}");
    CHECK(r.ok, "material with thermal properties: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "material_assign", "{\"body\": \"beam\", \"material\": \"steel_thermal\", \"source\": \"user\"}");
    CHECK(r.ok, "assign it: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "setup_validate", "{\"analysis\": \"transient_thermal\", \"end_time\": 300, \"time_step\": 10}");
    bool warned = false;
    for (size_t i = 0; r.ok && i < json_len(json_get(r.value, "warnings")); i++)
        warned |= !strcmp(json_get_str(json_at(json_get(r.value, "warnings"), i), "code", ""), "NO_THERMAL_CONDITIONS");
    CHECK(r.ok && warned, "a thermal setup with no conditions is flagged: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "boundary_apply", "{\"name\": \"hot_end\", \"kind\": \"temperature\", \"selection\": \"root\", \"temperature\": 300, \"source\": \"user\"}");
    CHECK(r.ok, "prescribed temperature: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "setup_validate", "{\"analysis\": \"transient_thermal\", \"end_time\": 300, \"time_step\": 10}");
    CHECK(r.ok && json_get_bool(r.value, "ready", false), "thermal setup ready: %s", err_msg(&r));
    op_result_free(&r);
    char jt[64] = "";
    CHECK(submit(e, "{\"analysis\": \"transient_thermal\", \"end_time\": 300, \"time_step\": 10, \"output_every\": 5, \"initial_temperature\": 20}", jt, sizeof jt),
          "transient thermal run");
    st = wait_job(e, jt);
    CHECK(st && !strcmp(json_get_str(st, "state", ""), "succeeded"), "transient job succeeded: %s", st ? json_get_str(at(st, "error"), "message", "") : "no status");
    const JsonValue *en = at(st, "summary.energy");
    double absorbed = num(st, "summary.energy.prescribed_nodes_j");
    double alpha = 15.0 / (7850 * 500), area = 1e-4;
    double analytic = 2 * 15 * 280 * sqrt(300 / (M_PI * alpha)) * area;
    CHECK(num(en, "closure_error") < 1e-6, "energy closes over the whole run (%.3g)", num(en, "closure_error"));
    CHECK(fabs(absorbed - analytic) / analytic < 0.12, "absorbed heat %.1f J vs the semi-infinite solution %.1f J (%.1f%%)", absorbed, analytic,
          100 * (absorbed - analytic) / analytic);
    CHECK(json_len(at(st, "summary.history")) == 7, "7 stored times (30 steps, every 5th)");
    CHECK(fabs(num(st, "summary.temperature.final_max_c") - 300) < 1e-6 && num(st, "summary.temperature.final_min_c") > 20 &&
              num(st, "summary.temperature.final_min_c") < 300,
          "the hot end stays at 300 degC and the cold end warms to %.1f degC", num(st, "summary.temperature.final_min_c"));
    json_free(st);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"quantity\": \"temperature\", \"time_index\": 0}", jt);
    r = run(e, "results_query", json);
    CHECK(r.ok && fabs(num(r.value, "statistics.min") - 20) < 1e-9 && !strcmp(json_get_str(r.value, "unit", ""), "degC"),
          "at t = 0 the part is at its initial temperature: %s", err_msg(&r));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"quantity\": \"temperature\", \"history\": true}", jt);
    r = run(e, "results_query", json);
    CHECK(r.ok && json_len(json_get(r.value, "history")) == 7 && num(r.value, "statistics.max") > 299, "temperature history returned: %s", err_msg(&r));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"points_mm\": [[0, 0, 5]], \"history\": true}", jt);
    r = run(e, "results_probe", json);
    const JsonValue *hist = at(r.value, "probes.0.history");
    double first = num(json_at(hist, 0), "temperature_c"), last = num(json_at(hist, json_len(hist) - 1), "temperature_c");
    CHECK(r.ok && json_get_bool(at(r.value, "probes.0"), "inside", false) && last > first && first >= 20,
          "the mid-span probe warms from %.2f to %.2f degC", first, last);
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"width\": 400, \"height\": 300}", jt);
    r = run(e, "results_render", json);
    CHECK(r.ok && r.nimages == 1 && !memcmp(r.images[0].data, "\x89PNG", 4) && !strcmp(json_get_str(at(r.value, "legend"), "unit", ""), "degC"),
          "temperature image: %s", err_msg(&r));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\"}", jt);
    r = run(e, "results_export", json);
    CHECK(r.ok && json_len(json_get(r.value, "files")) == 3, "transient exports: %s", err_msg(&r));
    for (size_t i = 0; r.ok && i < json_len(json_get(r.value, "files")); i++) {
        const JsonValue *f = json_at(json_get(r.value, "files"), i);
        CHECK(access(json_get_str(f, "path", ""), R_OK) == 0, "%s written", json_get_str(f, "format", ""));
    }
    op_result_free(&r);

    printf("== thermomechanical coupling through the operation layer\n");
    r = run(e, "boundary_remove", "{\"name\": \"load\"}");
    op_result_free(&r);
    char jm[64] = "";
    CHECK(submit(e,
                 "{\"analysis\": \"thermomechanical\", \"end_time\": 100, \"time_step\": 25, \"initial_temperature\": 20, \"stress_free_temperature\": 20}",
                 jm, sizeof jm),
          "thermomechanical run");
    st = wait_job(e, jm);
    CHECK(st && !strcmp(json_get_str(st, "state", ""), "succeeded"), "thermomechanical job succeeded: %s",
          st ? json_get_str(at(st, "error"), "message", "") : "no status");
    const JsonValue *last_row = json_at(at(st, "summary.history"), json_len(at(st, "summary.history")) - 1);
    CHECK(num(last_row, "peak_von_mises_mpa") > 1 && num(last_row, "max_displacement_mm") > 1e-4,
          "heating the clamped end builds %.1f MPa and %.4f mm of movement", num(last_row, "peak_von_mises_mpa"), num(last_row, "max_displacement_mm"));
    json_free(st);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"quantity\": \"von_mises\"}", jm);
    r = run(e, "results_query", json);
    CHECK(r.ok && !strcmp(json_get_str(r.value, "unit", ""), "MPa") && num(r.value, "statistics.max") > 1, "thermal stress query: %s", err_msg(&r));
    op_result_free(&r);

    printf("== duplicates and cancellation\n");
    r = run(e, "mesh_generate", "{\"element_size\": 1}");
    CHECK(r.ok && num(r.value, "mesh.elements") == 10000, "finer mesh for timing-sensitive checks: %s", err_msg(&r));
    op_result_free(&r);
    char ja[64] = "", jb[64] = "", jc[64] = "";
    CHECK(submit(e, "{\"label\": \"a\"}", ja, sizeof ja), "job a");
    r = run(e, "analysis_run", "{\"label\": \"again\"}");
    CHECK(r.ok && json_get_bool(r.value, "deduplicated", false) && !strcmp(json_get_str(r.value, "job_id", ""), ja), "identical active analysis is not started twice");
    op_result_free(&r);
    CHECK(submit(e, "{\"label\": \"b\", \"allow_duplicate\": true}", jb, sizeof jb) && strcmp(ja, jb) != 0, "allow_duplicate starts a second job");
    snprintf(json, sizeof json, "{\"job_id\": \"%s\"}", jb);
    r = run(e, "job_cancel", json);
    CHECK(r.ok && !strcmp(json_get_str(r.value, "state", ""), "cancelled"), "queued job cancelled: %s", r.ok ? json_get_str(r.value, "state", "") : err_msg(&r));
    op_result_free(&r);
    st = wait_job(e, ja);
    CHECK(st && !strcmp(json_get_str(st, "state", ""), "succeeded"), "job a unaffected");
    json_free(st);
    st = wait_job(e, jb);
    CHECK(st && !strcmp(json_get_str(st, "state", ""), "cancelled") && !strcmp(json_get_str(at(st, "error"), "code", ""), "CANCELLED"), "job b reports CANCELLED");
    json_free(st);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"quantity\": \"von_mises\"}", jb);
    r = run(e, "results_query", json);
    CHECK(!r.ok && !strcmp(err_code(&r), "PRECONDITION_FAILED"), "cancelled job has no results");
    op_result_free(&r);
    CHECK(submit(e, "{\"label\": \"c\", \"solver\": \"iterative\", \"tolerance\": 1e-14}", jc, sizeof jc), "job c");
    snprintf(json, sizeof json, "{\"job_id\": \"%s\"}", jc);
    r = run(e, "job_cancel", json);
    CHECK(r.ok, "cancel request for job c");
    op_result_free(&r);
    st = wait_job(e, jc);
    const char *cst = st ? json_get_str(st, "state", "") : "";
    CHECK(!strcmp(cst, "cancelled") || !strcmp(cst, "succeeded"), "job c ends cancelled or, if it finished first, succeeded (%s)", cst);
    json_free(st);

    printf("== staleness after a geometry change\n");
    r = run(e, "geometry_place", "{\"body\": \"beam\", \"rotate_z\": 90}");
    CHECK(r.ok && json_len(json_get(r.value, "stale_selections")) >= 2, "rotation makes the end selections stale: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "setup_validate", "{}");
    CHECK(r.ok && !json_get_bool(r.value, "ready", true) && has_code(json_get(r.value, "errors"), "STALE_REFERENCE"), "validation reports stale references");
    op_result_free(&r);
    r = run(e, "analysis_run", "{}");
    CHECK(!r.ok && !strcmp(err_code(&r), "STALE_REFERENCE"), "analysis refused on a stale setup: %s", err_code(&r));
    op_result_free(&r);
    r = run(e, "project_save", "{}");
    CHECK(r.ok, "save: %s", err_msg(&r));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"quantity\": \"displacement\"}", id);
    r = run(e, "results_query", json);
    double umax_before = r.ok ? num(r.value, "statistics.max") : NAN;
    op_result_free(&r);
    engine_destroy(e);

    printf("== results reload in a new engine\n");
    e = engine_create(&cfg, err, sizeof err);
    CHECK(e != NULL, "second engine");
    if (!e) return 1;
    snprintf(json, sizeof json, "{\"path\": \"%s\"}", project_dir);
    r = run(e, "project_open", json);
    CHECK(r.ok, "reopen: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "boundary_list", "{}");
    CHECK(r.ok && json_len(json_get(r.value, "boundary_conditions")) == 3, "boundary conditions restored");
    op_result_free(&r);
    r = run(e, "materials_list", "{\"id\": \"steel_test\"}");
    CHECK(r.ok && !strcmp(json_get_str(json_get(r.value, "material"), "defined_in", ""), "project"), "project material restored");
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\", \"quantity\": \"displacement\"}", id);
    r = run(e, "results_query", json);
    CHECK(r.ok && num(r.value, "statistics.max") == umax_before, "results loaded from the run directory match (%.9g)", r.ok ? num(r.value, "statistics.max") : NAN);
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"job_id\": \"%s\"}", id);
    r = run(e, "job_status", json);
    CHECK(r.ok && !strcmp(json_get_str(r.value, "state", ""), "succeeded") && json_get(r.value, "summary"), "job status from the run directory");
    op_result_free(&r);
    r = run(e, "results_query", "{\"job_id\": \"job-../../etc\", \"quantity\": \"von_mises\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "NOT_FOUND"), "path-like job ids are rejected");
    op_result_free(&r);

    engine_destroy(e);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) != 0) printf("  (cleanup failed)\n");
    printf("\n%s: %d passed, %d failed\n", g_fail ? "AM PATH TESTS FAILED" : "ALL AM PATH TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
