/* opstest.c - integration tests of the typed operation layer (the path shared by terminal, UI, socket and MCP):
 * schema validation, error codes, revisions, idempotent retries, access roots, STL import, placement,
 * diagnostics and project save/open with input hash verification.
 *   make build/opstest && ./build/opstest */
#include "../src/core/jschema.h"
#include "../src/ctl/engine.h"
#include "../src/ctl/ops.h"
#include "../src/geom/mesh.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

static const OpCaller CALLER = {"test", "opstest"};

/* A query about a job this session never started must come back, refusal or not, straight away. An operation that
 * holds the engine lock and then asks where the run directory is takes that lock a second time, and the process
 * stops there: without this watchdog the suite would hang instead of failing. */
static const char *g_watch_op;
static void on_stuck(int sig) {
    (void)sig;
    const char *op = g_watch_op ? g_watch_op : "(none)";
    ssize_t ignored = write(2, "  FAIL: ", 8);
    ignored = write(2, op, strlen(op));
    ignored = write(2, " did not answer a query about an unknown job within five seconds (the engine lock)\n", 84);
    (void)ignored;
    _exit(1);
}

/* runs an operation from a JSON literal; returns the result (caller frees) */
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

static void add_box(Mesh *m, double x0, double y0, double z0, double x1, double y1, double z1) {
    vec3 p[8];
    for (int i = 0; i < 8; i++) p[i] = v3((float)(i & 1 ? x1 : x0), (float)(i & 2 ? y1 : y0), (float)(i & 4 ? z1 : z0));
    static const int q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++) mesh_add_quad(m, p[q[f][0]], p[q[f][1]], p[q[f][2]], p[q[f][3]]);
}

static double num_at(const JsonValue *arr, size_t i) {
    const JsonValue *v = json_at(arr, i);
    return v && v->type == JSON_NUMBER ? v->u.number : NAN;
}

int main(void) {
    char base[1024], ws[1200], box_bin[1300], box_ascii[1300], garbage[1300], cmd[2600];
    const char *tmpdir = getenv("TMPDIR");
    snprintf(base, sizeof base, "%s/nv_opstest_%ld", tmpdir ? tmpdir : "/tmp", (long)getpid());
    snprintf(ws, sizeof ws, "%s/workspace", base);
    snprintf(box_bin, sizeof box_bin, "%s/box.stl", base);
    snprintf(box_ascii, sizeof box_ascii, "%s/box_ascii.stl", base);
    snprintf(garbage, sizeof garbage, "%s/garbage.stl", base);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", base);
    if (system(cmd) != 0) return 2;

    /* test geometry: a 20 x 10 x 5 mm box, binary and ASCII */
    Mesh m;
    mesh_init(&m);
    add_box(&m, 0, 0, 0, 20, 10, 5);
    char err[512];
    if (!mesh_save_stl(box_bin, &m, err, sizeof err)) return 2;
    FILE *f = fopen(box_ascii, "w");
    fprintf(f, "solid box_in_mm\n");
    for (uint32_t t = 0; t < m.tri_count; t++) {
        fprintf(f, " facet normal 0 0 0\n  outer loop\n");
        for (int k = 0; k < 3; k++) fprintf(f, "   vertex %g %g %g\n", m.pos[3 * t + k].x, m.pos[3 * t + k].y, m.pos[3 * t + k].z);
        fprintf(f, "  endloop\n endfacet\n");
    }
    fprintf(f, "endsolid box_in_mm\n");
    fclose(f);
    f = fopen(garbage, "w");
    fprintf(f, "this is not an STL file at all\n");
    fclose(f);
    mesh_free(&m);

    EngineConfig cfg;
    engine_config_default(&cfg);
    CHECK(engine_config_set_workspace(&cfg, ws, err, sizeof err), "workspace: %s", err);
    CHECK(engine_config_add_root(&cfg, false, base, err, sizeof err), "read root");
    Engine *e = engine_create(&cfg, err, sizeof err);
    CHECK(e != NULL, "engine_create: %s", err);
    if (!e) return 1;

    printf("== registry and capabilities\n");
    CHECK(ops_count() >= 8, "operations registered: %d", ops_count());
    for (int i = 0; i < ops_count(); i++) {
        JsonValue *s = ops_input_schema(ops_at(i));
        char *text = json_dump(s, 0, NULL, NULL);
        CHECK(text && !strstr(text, "$ref"), "%s schema is self-contained", ops_at(i)->name);
        CHECK(text && strstr(text, "\"type\":\"object\""), "%s schema is an object schema", ops_at(i)->name);
        free(text);
        json_free(s);
    }
    OpResult r = run(e, "capabilities_get", "{}");
    CHECK(r.ok && json_len(json_get(r.value, "operations")) == (size_t)ops_count(), "capabilities lists operations");
    CHECK(r.ok && json_len(json_get(r.value, "model_limitations")) > 0, "capabilities states model limitations");
    op_result_free(&r);
    r = run(e, "capabilities_get", "{\"verbose\": true}");
    CHECK(!r.ok && !strcmp(err_code(&r), "INVALID_PARAMS") && strstr(err_msg(&r), "unknown property"), "extra parameter rejected: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "geometry_imprt", "{}");
    CHECK(!r.ok && !strcmp(err_code(&r), "UNKNOWN_OPERATION") && strstr(json_get_str(r.error, "hint", ""), "geometry_import"), "unknown op with suggestion");
    op_result_free(&r);

    printf("== the material library at a glance\n");
    r = run(e, "material_list", "{}");
    {
        const JsonValue *mats = json_get(r.value, "materials"), *cnt = json_get(r.value, "counts");
        size_t n = json_len(mats);
        bool shaped = n > 0;
        const JsonValue *pla = NULL, *cocr = NULL, *ss316 = NULL;
        for (size_t i = 0; i < n; i++) {
            const JsonValue *m = json_at(mats, i);
            const char *id = json_get_str(m, "id", "");
            shaped &= id[0] && json_get_str(m, "status", "")[0] && json_get_str(m, "group", "")[0] &&
                      json_get(m, "properties") && json_get(json_get(m, "lacks"), "stress") && json_get(json_get(m, "lacks"), "heat") &&
                      json_get_str(m, "source_line", "")[0];
            if (!strcmp(id, "pla_fff")) pla = m;
            if (!strcmp(id, "cocr_f75_lpbf")) cocr = m;
            if (!strcmp(id, "ss316l_lpbf")) ss316 = m;
        }
        CHECK(r.ok && shaped && json_get_int(cnt, "materials", -1) == (long long)n &&
                  json_get_int(cnt, "with_a_source_on_every_value", 0) + json_get_int(cnt, "demonstration", 0) <= (long long)n,
              "material_list: %zu records, each with status, group, properties, lacks and a source line; counts agree", n);
        CHECK(pla && !strcmp(json_get_str(pla, "group", ""), "plastic filament (FFF)") && json_len(json_get(json_get(pla, "lacks"), "stress")) == 0 &&
                  json_len(json_get(json_get(pla, "lacks"), "heat")) == 0,
              "the sourced PLA is in the filament group and lacks nothing for a stress or a heat analysis");
        bool cocr_nu = false;
        const JsonValue *cs = cocr ? json_get(json_get(cocr, "lacks"), "stress") : NULL;
        for (size_t i = 0; i < json_len(cs); i++) cocr_nu |= !strcmp(json_str(json_at(cs, i)) ? json_str(json_at(cs, i)) : "", "Poisson's ratio");
        CHECK(cocr_nu && ss316 && json_len(json_get(json_get(ss316, "lacks"), "stress")) == 0,
              "a record without Poisson's ratio says so (CoCr F75); 316L, which has one now, lacks nothing for stress");
    }
    op_result_free(&r);
    r = run(e, "material_list", "{\"process\": \"fff\"}");
    {
        const JsonValue *mats = json_get(r.value, "materials");
        bool all = r.ok && json_len(mats) > 0;
        for (size_t i = 0; i < json_len(mats); i++) {
            const JsonValue *pr = json_get(json_at(mats, i), "processes");
            bool fff = false;
            for (size_t k = 0; k < json_len(pr); k++) fff |= json_str(json_at(pr, k)) && !strcmp(json_str(json_at(pr, k)), "fff");
            all &= fff;
        }
        CHECK(all, "material_list filtered to fff lists only records for that process (%zu, the build plate included)", json_len(mats));
    }
    op_result_free(&r);
    r = run(e, "material_list", "{\"process\": \"casting\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "INVALID_PARAMS"), "material_list refuses an unknown process");
    op_result_free(&r);

    printf("== project lifecycle\n");
    char json[4096];
    snprintf(json, sizeof json, "{\"path\": \"%s\", \"units\": \"mm\"}", box_bin);
    r = run(e, "geometry_import", json);
    CHECK(!r.ok && !strcmp(err_code(&r), "NO_PROJECT"), "import without project: %s", err_code(&r));
    op_result_free(&r);
    r = run(e, "project_create", "{\"name\": \"bad name!\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "INVALID_PARAMS"), "invalid project name");
    op_result_free(&r);
    r = run(e, "project_create", "{\"name\": \"x\", \"directory\": \"/usr/nv_forbidden_test\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "PERMISSION_DENIED"), "directory outside write roots: %s %s", err_code(&r), err_msg(&r));
    op_result_free(&r);
    r = run(e, "project_create", "{\"name\": \"demo\", \"description\": \"ops test\"}");
    CHECK(r.ok, "project_create: %s", err_msg(&r));
    const char *pfile = r.ok ? json_get_str(r.value, "project_file", "") : "";
    CHECK(access(pfile, F_OK) == 0, "project.json written at %s", pfile);
    char project_dir[1300];
    snprintf(project_dir, sizeof project_dir, "%s", r.ok ? json_get_str(json_get(r.value, "project"), "directory", "") : "");
    op_result_free(&r);
    r = run(e, "project_create", "{\"name\": \"demo\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "ALREADY_EXISTS"), "second create refuses to overwrite: %s", err_code(&r));
    op_result_free(&r);

    printf("== geometry import\n");
    r = run(e, "geometry_import", "{\"path\": \"x.stl\"}");
    CHECK(!r.ok && strstr(err_msg(&r), "units"), "units are required: %s", err_msg(&r));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"path\": \"%s\", \"units\": \"milimeter\"}", box_bin);
    r = run(e, "geometry_import", json);
    CHECK(!r.ok && strstr(err_msg(&r), "must be one of"), "unit enum: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "geometry_import", "{\"path\": \"/etc/hosts\", \"units\": \"mm\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "PERMISSION_DENIED"), "read outside roots: %s %s", err_code(&r), err_msg(&r));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"path\": \"%s\", \"units\": \"mm\"}", garbage);
    r = run(e, "geometry_import", json);
    CHECK(!r.ok && !strcmp(err_code(&r), "GEOMETRY_INVALID"), "garbage file: %s %s", err_code(&r), err_msg(&r));
    op_result_free(&r);

    snprintf(json, sizeof json, "{\"path\": \"%s\", \"units\": \"mm\", \"idempotency_key\": \"import-1\"}", box_bin);
    r = run(e, "geometry_import", json);
    CHECK(r.ok, "import binary box: %s", err_msg(&r));
    uint64_t rev_after_import = r.revision;
    const JsonValue *body = json_get(r.value, "body");
    CHECK(!strcmp(json_get_str(body, "name", ""), "box") && json_get_bool(body, "closed_solid", false), "body 'box' is a closed solid");
    CHECK(fabs(json_get_num(body, "volume_mm3", 0) - 1000) < 1e-6, "volume 1000 mm^3 (got %g)", json_get_num(body, "volume_mm3", 0));
    const JsonValue *size = json_get(body, "size_mm");
    CHECK(fabs(num_at(size, 0) - 20) < 1e-9 && fabs(num_at(size, 1) - 10) < 1e-9 && fabs(num_at(size, 2) - 5) < 1e-9, "size 20x10x5 mm");
    const JsonValue *bmin = json_get(json_get(body, "bounds_mm"), "min");
    CHECK(fabs(num_at(bmin, 2)) < 1e-12 && fabs(num_at(bmin, 0) + 10) < 1e-9, "placed on the plate, centred");
    CHECK(json_get_bool(json_get(r.value, "units_check"), "plausible", false), "20 mm part is plausible");
    CHECK(json_len(json_get(r.value, "assumptions")) == 1, "default up axis recorded as an assumption");
    op_result_free(&r);

    r = run(e, "geometry_import", json);
    CHECK(r.ok && r.replayed && r.revision == rev_after_import, "retry with the same idempotency key replays (revision %llu)", (unsigned long long)r.revision);
    op_result_free(&r);
    /* geometry_pick: the ray the native 3D view casts under the cursor, answered as a surface patch */
    r = run(e, "geometry_pick", "{\"origin_mm\": [0, 0, 50], \"direction\": [0, 0, -1]}");
    {
        const JsonValue *pi = json_get(r.value, "patch_info");
        CHECK(r.ok && json_get_bool(r.value, "hit", false) && !strcmp(json_get_str(r.value, "body", ""), "box"),
              "a ray from above hits the box: %s", err_msg(&r));
        CHECK(r.ok && fabs(num_at(json_get(r.value, "point_mm"), 2) - 5) < 1e-9, "it hits the top face at z = 5 mm");
        CHECK(pi && !strcmp(json_get_str(pi, "type", ""), "planar") && !strcmp(json_get_str(pi, "facing", ""), "up"),
              "the patch is the planar top face");
        CHECK(pi && fabs(json_get_num(pi, "area_mm2", 0) - 200) < 1e-6, "its area is 20 x 10 mm (got %g)",
              pi ? json_get_num(pi, "area_mm2", 0) : 0);
        CHECK(pi && fabs(num_at(json_get(pi, "normal"), 2) - 1) < 1e-9, "its outward normal points up");
    }
    op_result_free(&r);
    r = run(e, "geometry_pick", "{\"origin_mm\": [500, 0, 50], \"direction\": [0, 0, -1]}");
    CHECK(r.ok && !json_get_bool(r.value, "hit", true) && json_get_str(r.value, "message", NULL),
          "a ray beside the part reports no hit and says so");
    op_result_free(&r);
    r = run(e, "geometry_pick", "{\"origin_mm\": [0, 0, 50], \"direction\": [0, 0, 0]}");
    CHECK(!r.ok && !strcmp(err_code(&r), "INVALID_PARAMS"), "a zero direction is refused: %s", err_code(&r));
    op_result_free(&r);
    r = run(e, "geometry_pick", "{\"direction\": [0, 0, -1]}");
    CHECK(!r.ok, "the origin is required: %s", err_code(&r));
    op_result_free(&r);

    snprintf(json, sizeof json, "{\"path\": \"%s\", \"units\": \"in\", \"idempotency_key\": \"import-1\"}", box_bin);
    r = run(e, "geometry_import", json);
    CHECK(!r.ok && !strcmp(err_code(&r), "IDEMPOTENCY_KEY_REUSED"), "key reuse with different params: %s", err_code(&r));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"path\": \"%s\", \"units\": \"mm\"}", box_bin);
    r = run(e, "geometry_import", json);
    CHECK(!r.ok && !strcmp(err_code(&r), "ALREADY_EXISTS"), "duplicate body name: %s", err_code(&r));
    op_result_free(&r);

    snprintf(json, sizeof json, "{\"path\": \"%s\", \"units\": \"um\", \"units_source\": \"inferred\", \"name\": \"tiny\"}", box_ascii);
    r = run(e, "geometry_import", json);
    CHECK(r.ok, "ascii import: %s", err_msg(&r));
    const JsonValue *uc = json_get(r.value, "units_check");
    CHECK(!json_get_bool(uc, "plausible", true) && strstr(json_get_str(uc, "assessment", ""), "implausible"), "micrometre box flagged: %s",
          json_get_str(uc, "assessment", ""));
    CHECK(json_len(json_get(r.value, "warnings")) == 1, "missing units_note warned");
    CHECK(!strcmp(json_get_str(json_get(json_get(r.value, "body"), "source"), "format", ""), "ascii"), "ascii detected");
    op_result_free(&r);

    printf("== placement, units and revisions\n");
    r = run(e, "geometry_place", "{\"body\": \"box\", \"up_axis\": \"+Y\", \"rotate_z\": 90, \"position\": [\"0.5 in\", 3], \"expected_revision\": 1}");
    CHECK(!r.ok && !strcmp(err_code(&r), "REVISION_CONFLICT") && json_len(json_get(json_get(r.error, "details"), "changes_since")) > 0,
          "stale expected_revision rejected with the change list: %s", err_code(&r));
    op_result_free(&r);
    r = run(e, "project_inspect", "{\"sections\": [\"summary\"]}");
    uint64_t cur = r.revision;
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"body\": \"box\", \"up_axis\": \"+Y\", \"rotate_z\": 90, \"position\": [\"0.5 in\", 3], \"expected_revision\": %llu}",
             (unsigned long long)cur);
    r = run(e, "geometry_place", json);
    CHECK(r.ok && r.revision == cur + 1, "placement with current revision: %s", err_msg(&r));
    body = json_get(r.value, "body");
    size = json_get(body, "size_mm");
    /* file Y up: build extents x=20, y=5, z=10; then 90 deg about Z swaps x and y */
    CHECK(fabs(num_at(size, 0) - 5) < 1e-9 && fabs(num_at(size, 1) - 20) < 1e-9 && fabs(num_at(size, 2) - 10) < 1e-9, "size after placement %g %g %g",
          num_at(size, 0), num_at(size, 1), num_at(size, 2));
    bmin = json_get(json_get(body, "bounds_mm"), "min");
    CHECK(fabs(num_at(bmin, 0) - (12.7 - 2.5)) < 1e-9 && fabs(num_at(bmin, 1) - (3 - 10)) < 1e-9 && fabs(num_at(bmin, 2)) < 1e-12,
          "position [0.5 in, 3 mm] applied: min %g %g %g", num_at(bmin, 0), num_at(bmin, 1), num_at(bmin, 2));
    op_result_free(&r);
    r = run(e, "geometry_place", "{\"body\": \"box\", \"z_offset\": -1}");
    CHECK(!r.ok && !strcmp(err_code(&r), "OUT_OF_RANGE"), "negative z_offset: %s", err_code(&r));
    op_result_free(&r);
    r = run(e, "geometry_place", "{\"body\": \"box\", \"z_offset\": \"3 kg\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "INVALID_PARAMS") && strstr(err_msg(&r), "length"), "wrong dimension: %s", err_msg(&r));
    op_result_free(&r);
    r = run(e, "geometry_place", "{\"body\": \"nope\"}");
    CHECK(!r.ok && !strcmp(err_code(&r), "NOT_FOUND"), "unknown body: %s", err_code(&r));
    op_result_free(&r);

    printf("== diagnostics\n");
    r = run(e, "geometry_diagnostics", "{\"body\": \"box\"}");
    const JsonValue *checks = json_get(json_get(r.value, "diagnostics"), "checks");
    CHECK(r.ok && json_get_int(checks, "self_intersecting_face_pairs", -1) == 0, "no self-intersections");
    CHECK(fabs(json_get_num(json_get(checks, "wall_thickness"), "min_mm", 0) - 5) < 1e-6, "min wall 5 mm (got %g)", json_get_num(json_get(checks, "wall_thickness"), "min_mm", 0));
    op_result_free(&r);

    /* Criterion declared 2026-10-03 before the first run: this measured 5 mm wall with hxy=4 mm and hz=0.5 mm
     * gets a conservative largest-spacing warning despite its fine z layers; a uniform 0.5 mm mesh does not.
     * Both API responses explicitly identify the boundary-distance mean as weighted by face area. */
    printf("== anisotropic printing-mesh diagnostics\n");
    for (int fine = 0; fine < 2; fine++) {
        r = run(e, "mesh_generate", fine ? "{\"element_size\":\"0.5 mm\"}" :
                                          "{\"element_size\":\"4 mm\",\"element_size_z\":\"0.5 mm\"}");
        CHECK(r.ok, "printing mesh diagnostic operation: %s", err_msg(&r));
        if (r.ok) {
            bool warned = false;
            const JsonValue *warnings = json_get(r.value, "warnings");
            for (size_t i = 0; i < json_len(warnings); i++) {
                const JsonValue *w = json_at(warnings, i);
                if (w && w->type == JSON_STRING && strstr(w->u.string.ptr, "walls of 'box'") &&
                    strstr(w->u.string.ptr, "largest element spacing")) warned = true;
            }
            CHECK(warned == !fine, "5 mm walls with %s XY spacing %s conservative warning", fine ? "0.5 mm" : "4 mm", warned ? "carry" : "omit");
            const JsonValue *boundary = json_get(json_get(r.value, "mesh"), "boundary");
            CHECK(!strcmp(json_get_str(boundary, "mean_distance_weighting", ""), "boundary-face area"),
                  "mesh API labels area-weighted boundary distance");
        }
        op_result_free(&r);
    }

    printf("== save, reopen, tamper\n");
    r = run(e, "project_save", "{}");
    CHECK(r.ok, "save: %s", err_msg(&r));
    op_result_free(&r);
    snprintf(json, sizeof json, "{\"path\": \"%s\"}", project_dir);
    r = run(e, "project_open", json);
    CHECK(r.ok && json_len(json_get(r.value, "bodies")) == 2, "reopen: %s", err_msg(&r));
    const JsonValue *bodies = json_get(r.value, "bodies");
    const JsonValue *b0 = json_at(bodies, 0);
    size = json_get(b0, "size_mm");
    CHECK(fabs(num_at(size, 0) - 5) < 1e-9 && fabs(num_at(size, 1) - 20) < 1e-9, "placement restored");
    CHECK(json_len(json_get(r.value, "assumptions")) == 2, "assumptions restored (%zu)", json_len(json_get(r.value, "assumptions")));
    op_result_free(&r);
    snprintf(cmd, sizeof cmd, "for f in '%s'/inputs/box-*.stl; do printf 'x' >> \"$f\"; done", project_dir);
    CHECK(system(cmd) == 0, "tamper");
    r = run(e, "project_open", json);
    CHECK(!r.ok && strstr(err_msg(&r), "SHA-256"), "tampered input detected: %s", err_msg(&r));
    op_result_free(&r);

    printf("== repairing a surface\n");
    /* geometry_repair: a box with a stray sheet inside it and one facet missing, made into something the mesher can
     * trust, with every change counted */
    {
        char rws[1300];
        snprintf(rws, sizeof rws, "%s/repairws", base);
        EngineConfig rcfg;
        engine_config_default(&rcfg);
        CHECK(engine_config_set_workspace(&rcfg, rws, err, sizeof err), "repair workspace: %s", err);
        CHECK(engine_config_add_root(&rcfg, false, base, err, sizeof err), "repair read root");
        Engine *re = engine_create(&rcfg, err, sizeof err);
        CHECK(re != NULL, "engine for the repair: %s", err);
        if (!re) return 1;
        r = run(re, "project_create", "{\"name\": \"repair\"}");
        CHECK(r.ok, "a project for the repair: %s", err_msg(&r));
        op_result_free(&r);
        char broken[1300];
        snprintf(broken, sizeof broken, "%s/broken.stl", base);
        Mesh full, bm;
        mesh_init(&full);
        mesh_init(&bm);
        add_box(&full, 0, 0, 0, 20, 10, 5);
        for (uint32_t t = 0; t + 1 < full.tri_count; t++) /* every facet but the last: a triangular hole */
            mesh_add_tri(&bm, full.pos[3 * t], full.pos[3 * t + 1], full.pos[3 * t + 2]);
        mesh_add_tri(&bm, v3(2, 2, 2), v3(8, 2, 2), v3(8, 8, 2)); /* a sheet of zero thickness inside the box */
        mesh_add_tri(&bm, v3(2, 2, 2), v3(8, 8, 2), v3(2, 8, 2));
        mesh_free(&full);
        if (!mesh_save_stl(broken, &bm, err, sizeof err)) {
            printf("  cannot write %s: %s\n", broken, err);
            return 2;
        }
        mesh_free(&bm);
        snprintf(json, sizeof json, "{\"path\": \"%s\", \"units\": \"mm\", \"name\": \"broken\"}", broken);
        r = run(re, "geometry_import", json);
        CHECK(r.ok, "a box with a hole and a sheet inside imports: %s", err_msg(&r));
        op_result_free(&r);
        r = run(re, "geometry_repair", "{\"body\": \"broken\"}");
        CHECK(!r.ok, "a repair without provenance is refused: %s", err_code(&r));
        op_result_free(&r);
        r = run(re, "geometry_repair", "{\"body\": \"broken\", \"provenance\": \"user\", \"max_hole_edges\": 8}");
        {
            const JsonValue *ch = json_get(r.value, "changes"), *af = json_get(r.value, "after");
            CHECK(r.ok, "the repair runs: %s", err_msg(&r));
            CHECK(r.ok && json_get_int(ch, "shells_dropped", 0) == 1 && json_get_int(ch, "facets_dropped", 0) == 2,
                  "the sheet inside is dropped: %lld shell, %lld facets", json_get_int(ch, "shells_dropped", -1),
                  json_get_int(ch, "facets_dropped", -1));
            CHECK(r.ok && json_get_int(ch, "holes_filled", 0) == 1 && json_get_int(ch, "facets_added", 0) == 1,
                  "the hole is filled with one facet: %lld hole(s), %lld facet(s)",
                  json_get_int(ch, "holes_filled", -1), json_get_int(ch, "facets_added", -1));
            CHECK(r.ok && json_get_bool(af, "closed_solid", false), "the repaired box is a closed solid");
            CHECK(r.ok && fabs(json_get_num(af, "volume", 0) - 1000) < 1e-6,
                  "and it encloses the box's 1000 mm3 (got %g)", json_get_num(af, "volume", 0));
            CHECK(r.ok && !strcmp(json_get_str(ch, "provenance", ""), "user"), "the provenance is carried through");
        }
        op_result_free(&r);
        r = run(re, "geometry_repair", "{\"body\": \"nosuchbody\", \"provenance\": \"user\"}");
        CHECK(!r.ok && !strcmp(err_code(&r), "NOT_FOUND"), "an unknown body is refused: %s", err_code(&r));
        op_result_free(&r);
            engine_destroy(re);
    }

    /* every operation that takes a job id answers a query about a job this session does not know, within a second */
    {
        char pdir[1400], runs[1500], stale[1600], json[1800], jws[1300];
        snprintf(jws, sizeof jws, "%s/jobqueryws", base);
        EngineConfig jcfg;
        engine_config_default(&jcfg);
        CHECK(engine_config_set_workspace(&jcfg, jws, err, sizeof err), "job-query workspace: %s", err);
        Engine *je = engine_create(&jcfg, err, sizeof err);
        CHECK(je != NULL, "engine for the job queries: %s", err);
        if (!je) return 1;
        snprintf(json, sizeof json, "{\"name\": \"jobquery\", \"description\": \"queries about unknown jobs\"}");
        OpResult r = run(je, "project_create", json);
        snprintf(pdir, sizeof pdir, "%s", json_get_str(json_get(r.value, "project"), "directory", ""));
        op_result_free(&r);
        /* a run directory with nothing in it: known to the folder, unknown to this session */
        snprintf(runs, sizeof runs, "%s/runs", pdir);
        snprintf(stale, sizeof stale, "%s/job-20260101-000000-0000-1", runs);
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", stale);
        if (system(cmd) != 0) printf("  (could not make the stale run directory)\n");
        /* a second one that looks like a finished run of an earlier session: this is the path that looks the run
         * directory up while the operation already holds the engine lock */
        snprintf(cmd, sizeof cmd,
                 "mkdir -p '%s/job-20260101-000000-0000-2' && printf '{}' > '%s/job-20260101-000000-0000-2/spec.json' && "
                 "printf 'x' > '%s/job-20260101-000000-0000-2/results.nvt' && "
                 "printf 'x' > '%s/job-20260101-000000-0000-2/results.nvr'",
                 runs, runs, runs, runs);
        if (system(cmd) != 0) printf("  (could not make the finished-looking run directory)\n");
        static const char *JOB_OPS[] = {"job_status", "job_cancel",  "job_pause",     "job_resume",    "job_checkpoint",
                                        "results_query", "results_quantities", "results_probe", "results_export",
                                        "results_render", "results_interface", "topology_result", NULL};
        static const char *IDS[] = {"job-20260101-000000-0000-1", "job-20260101-000000-0000-2",
                                    "job-19700101-000000-0000-9", NULL};
        signal(SIGALRM, on_stuck);
        for (int i = 0; JOB_OPS[i]; i++)
            for (int k = 0; IDS[k]; k++) {
                snprintf(json, sizeof json, "{\"job_id\": \"%s\"}", IDS[k]);
                g_watch_op = JOB_OPS[i];
                alarm(5);
                struct timespec t0, t1;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                r = run(je, JOB_OPS[i], json);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                alarm(0);
                double ms = 1e3 * (t1.tv_sec - t0.tv_sec) + 1e-6 * (t1.tv_nsec - t0.tv_nsec);
                /* the answer may be a refusal or, for a run directory that holds results, an answer: what matters
                 * is that it comes back, and quickly */
                if (k == 2)
                    CHECK(!r.ok, "%s refuses a job that exists nowhere (%s): %s", JOB_OPS[i], IDS[k], err_code(&r));
                CHECK(ms < 1000.0, "%s answers about %s within a second (%.0f ms)", JOB_OPS[i], IDS[k], ms);
                op_result_free(&r);
            }
        engine_destroy(je);
    }

    engine_destroy(e);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) != 0) printf("  (cleanup failed)\n");
    printf("\n%s: %d passed, %d failed\n", g_fail ? "OPS TESTS FAILED" : "ALL OPS TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
