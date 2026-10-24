/* labxrtest.c - the headset's lab scene (src/lab/labxr.h) is the scene the app draws (src/lab/labscene.h, labgpu.c).
 *
 * Criteria, written before the first run (2026-10-07), on a synthetic result of three frames holding every kind of part
 * the lab writes (hexahedra with a cell and a node field, a closed triangle surface, particles with their own radius,
 * line segments, a quad with a material look, a 6 x 5 x 4 volume with a signed field holding a NaN, a body's material
 * and an iso field), for six views (field choice, section, element edges, absolute and log values, iso and solid
 * volumes):
 *   1. the GLB is valid: header, chunk types and lengths, every view inside the binary chunk and 4-byte aligned, every
 *      accessor filling its view, every index naming a vertex, triangle and line index counts multiples of 3 and 2;
 *   2. its triangles, mapped back to lab axes by the to_source matrix it carries, are labscene_build's triangles: the
 *      same number, the same multiset of corner positions with the same stored value (exact float equality), the same
 *      total area (relative 1e-6); faces with a smooth normal keep it (exact), flat faces carry no normals;
 *   3. each value's palette coordinate is the centre of its texel, (0.5 + 247 t) / 256, t the clamped position in the
 *      range (1e-6); a corner without a value reads the neutral texel 252/256;
 *   4. particles and line segments are labscene_build's (counts and multisets of position, value and radius, exact);
 *   5. a part with a material look is drawn in that look and carries no value (when a section keeps it: a section can
 *      cut a whole part away, and then it is drawn in no look at all, as in the app; noted after the first run);
 *   6. the volume's samples decode to (value - lo) / (hi - lo) within half precision (2^-11 relative, 2^-25 absolute),
 *      a NaN sample to the strength-free value; its mask bytes are round(255 m) of labscene_volume_mask exactly; mode
 *      and walls are as the app sets them (fog, iso, solid);
 *   7. the extras name the frame, its time, field, unit, range and parts; the box is labscene_build's bounds;
 *   8. a frame outside the result is refused with a reason; the info lists the frames, times, fields and parts and a
 *      box holding every node and block of the sampled frames;
 *      (amended 2026-10-07, when planar results became pictures in the headset instead of being refused, written before
 *      that code's first test run) a planar result is drawn as one upright quad of its plane's extent and proportions,
 *      in glTF y up, with a grid of its values whose samples are the plane's field, here 2 x + 3 y + 1 on a 16 x 12 block,
 *      reproduced inside the plane within half precision (bilinear sampling is exact for a linear field); the info gives
 *      the same box;
 *   9. the view survives its JSON (every member), and wrong members are refused;
 *  10. half floats: every one of the 65536 patterns survives half -> float -> half; float -> half rounds to nearest
 *      even as an independent reference does (and the hardware's __fp16 where the compiler has it), over 2 million floats.
 * Results in ~/OpenPhysicsAI-Projects/lab, when present, are converted at their first, middle and last frames (each under 2 s,
 * criterion 1 for each); a result that is not 3D is drawn as a picture of its plane (amended with criterion 8). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

#include "../src/core/glb.h"
#include "../src/core/json.h"
#include "../src/core/paths.h"
#include "../src/lab/labio.h"
#include "../src/lab/labscene.h"
#include "../src/lab/labview.h"
#include "../src/lab/labxr.h"
#include "../src/lab/style.h"

static int failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL line %d: %s: ", __LINE__, #c);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* ---- a GLB read back --------------------------------------------------------------------------------------------- */

typedef struct Gl {
    const unsigned char *bin;
    size_t binlen;
    JsonValue *j;
} Gl;

static bool gl_open(Gl *g, const unsigned char *b, size_t n) {
    uint32_t magic, ver, len, jl, jt, bl, bt;
    memset(g, 0, sizeof *g);
    if (n < 28) return false;
    memcpy(&magic, b, 4), memcpy(&ver, b + 4, 4), memcpy(&len, b + 8, 4), memcpy(&jl, b + 12, 4), memcpy(&jt, b + 16, 4);
    CHECK(magic == 0x46546C67u && ver == 2 && len == n, "header");
    CHECK(jt == 0x4E4F534Au && jl % 4 == 0 && 20 + (size_t)jl + 8 <= n, "JSON chunk");
    if (!(jt == 0x4E4F534Au && 20 + (size_t)jl + 8 <= n)) return false;
    memcpy(&bl, b + 20 + jl, 4), memcpy(&bt, b + 24 + jl, 4);
    CHECK(bt == 0x004E4942u && 28 + (size_t)jl + bl == n, "BIN chunk");
    g->j = json_parse((const char *)b + 20, jl, NULL, NULL);
    g->bin = b + 28 + jl, g->binlen = bl;
    CHECK(g->j != NULL, "JSON parses");
    return g->j != NULL;
}

/* criterion 1 over the whole file */
static void gl_structure(const Gl *g) {
    const JsonValue *views = json_get(g->j, "bufferViews"), *acc = json_get(g->j, "accessors");
    CHECK(json_get_num(json_at(json_get(g->j, "buffers"), 0), "byteLength", -1) == (double)g->binlen, "buffer length");
    for (size_t i = 0; i < json_len(views); i++) {
        const JsonValue *v = json_at(views, i);
        double off = json_get_num(v, "byteOffset", -1), len = json_get_num(v, "byteLength", -1);
        CHECK(off >= 0 && len >= 0 && off + len <= (double)g->binlen && fmod(off, 4) == 0, "view %zu inside and aligned", i);
    }
    for (size_t i = 0; i < json_len(acc); i++) {
        const JsonValue *a = json_at(acc, i);
        const char *t = json_get_str(a, "type", "");
        int comps = !strcmp(t, "VEC3") ? 3 : !strcmp(t, "VEC2") ? 2 : 1;
        const JsonValue *v = json_at(views, (size_t)json_get_int(a, "bufferView", -1));
        CHECK(v && json_get_num(a, "count", -1) * comps * 4 == json_get_num(v, "byteLength", -2), "accessor %zu fills its view", i);
    }
    const JsonValue *meshes = json_get(g->j, "meshes");
    for (size_t m = 0; m < json_len(meshes); m++) {
        const JsonValue *prims = json_get(json_at(meshes, m), "primitives");
        CHECK(json_len(prims) > 0, "a mesh has primitives");
        for (size_t p = 0; p < json_len(prims); p++) {
            const JsonValue *pr = json_at(prims, p), *at = json_get(pr, "attributes");
            int mode = (int)json_get_int(pr, "mode", 4);
            size_t nv = (size_t)json_get_num(json_at(acc, (size_t)json_get_int(at, "POSITION", -1)), "count", 0);
            for (size_t k = 0; k < json_len(at); k++) {
                const JsonValue *a = json_at(acc, (size_t)json_value_at(at, k)->u.number);
                CHECK(a && json_get_num(a, "count", -1) == (double)nv, "attribute %s has one entry per vertex", json_key_at(at, k));
            }
            if (json_get(pr, "indices")) {
                const JsonValue *a = json_at(acc, (size_t)json_get_int(pr, "indices", -1));
                const JsonValue *v = json_at(views, (size_t)json_get_int(a, "bufferView", -1));
                size_t ni = (size_t)json_get_num(a, "count", 0);
                const uint32_t *idx = (const uint32_t *)(g->bin + (size_t)json_get_num(v, "byteOffset", 0));
                uint32_t mx = 0;
                for (size_t i = 0; i < ni; i++) mx = idx[i] > mx ? idx[i] : mx;
                CHECK(ni == 0 || mx < nv, "every index names a vertex");
                CHECK(ni % (mode == 4 ? 3 : mode == 1 ? 2 : 1) == 0, "index count fits the mode");
            }
        }
    }
}

static const float *gl_floats(const Gl *g, int accessor, size_t *count) {
    const JsonValue *a = json_at(json_get(g->j, "accessors"), (size_t)accessor);
    const JsonValue *v = json_at(json_get(g->j, "bufferViews"), (size_t)json_get_int(a, "bufferView", -1));
    *count = (size_t)json_get_num(a, "count", 0);
    return (const float *)(g->bin + (size_t)json_get_num(v, "byteOffset", 0));
}
static const unsigned char *gl_view(const Gl *g, int view, size_t *len) {
    const JsonValue *v = json_at(json_get(g->j, "bufferViews"), (size_t)view);
    *len = (size_t)json_get_num(v, "byteLength", 0);
    return g->bin + (size_t)json_get_num(v, "byteOffset", 0);
}

/* ---- records compared as multisets ------------------------------------------------------------------------------- */

typedef struct Corner {
    float p[3], value, radius, key;
} Corner;
static int corner_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(Corner)); }
typedef struct Tri {
    Corner c[3];
} Tri;
static int tri_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(Tri)); }
static void tri_sort(Tri *t) { qsort(t->c, 3, sizeof(Corner), corner_cmp); }

static float key_of(const LabVertex *v) { /* as the converter groups: 0 field, 1 plain, 2 edges, 3 + look */
    if (v->field > 0) return 0;
    if (v->field > -0.5f) return 1;
    if (v->field > -1.5f) return 2;
    return 3 + (float)((int)(-v->field + .5f) - 1);
}
static float key_name(const char *key, const char *look) {
    return !strcmp(key, "field") ? 0 : !strcmp(key, "plain") ? 1 : !strcmp(key, "edges") ? 2 : 3 + (float)labscene_look_id(look);
}
static double tri_area(const float *a, const float *b, const float *c) {
    double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    double x = e1[1] * e2[2] - e1[2] * e2[1], y = e1[2] * e2[0] - e1[0] * e2[2], z = e1[0] * e2[1] - e1[1] * e2[0];
    return 0.5 * sqrt(x * x + y * y + z * z);
}
static void to_lab(const double *M, const float *g, float *out) { /* column-major 4 x 4 */
    for (int r = 0; r < 3; r++) out[r] = (float)(M[r] * g[0] + M[4 + r] * g[1] + M[8 + r] * g[2] + M[12 + r]);
}

/* ---- the synthetic result ---------------------------------------------------------------------------------------- */

static const char *PATH = "build/labxrtest.lab";
static void write_result(void); /* labxrtest --write FILE: the synthetic result alone, for tools/xr/xr_test.py */
static void write_result(void) {
    char err[256];
    LabWriter *w = lab_create(PATH,
                              "{\"domain\":\"test\",\"title\":\"Every kind of part\",\"fields\":{\"stress\":\"Pa\",\"T\":\"K\",\"Q\":\"1/s^2\"},"
                              "\"looks\":{\"frame\":\"steel\"}}",
                              err, sizeof err);
    if (!w) {
        printf("FAIL: cannot write %s: %s\n", PATH, err);
        exit(1);
    }
    for (int k = 0; k < 3; k++) {
        lab_frame_begin(w, 0.25 * k);
        double hx[36];
        int at = 0;
        for (int x = 0; x < 3; x++)
            for (int z = 0; z < 2; z++)
                for (int y = 0; y < 2; y++) hx[at++] = x + 0.1 * k, hx[at++] = y, hx[at++] = z;
        int conn[16] = {0, 4, 5, 1, 2, 6, 7, 3, 4, 8, 9, 5, 6, 10, 11, 7};
        float cell[2] = {7.0f + k, 19.0f + k}, node[12];
        for (int i = 0; i < 12; i++) node[i] = 1.5f * i + k;
        lab_part_cells(w, "block", 12, hx, 2, LAB_HEX, conn);
        lab_field(w, "stress", LAB_AT_CELL, 2, cell);
        lab_field(w, "T", LAB_AT_NODE, 12, node);
        double tx[12] = {4, 0, 0, 5, 0, 0, 4, 1, 0, 4, 0, 1.0 + 0.2 * k}; /* a closed tetrahedron */
        int tc[12] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
        float tv[4] = {-5, 3, NAN, 12};
        lab_part_cells(w, "skin", 4, tx, 4, LAB_TRI, tc);
        lab_field(w, "stress", LAB_AT_NODE, 4, tv);
        double px[15];
        float pv[5], pr[5];
        for (int i = 0; i < 5; i++) px[3 * i] = 6 + 0.3 * i, px[3 * i + 1] = 0.1 * k, px[3 * i + 2] = 0.5, pv[i] = -8.0f + 6.0f * i, pr[i] = 0.01f * (i + 1);
        lab_part_points(w, "dots", 5, px);
        lab_field(w, "stress", LAB_AT_NODE, 5, pv);
        lab_field(w, "radius", LAB_AT_NODE, 5, pr);
        double lx[12] = {0, 2, 0, 1, 2, 0, 1, 2, 0, 2, 2, 1};
        int lc[4] = {0, 1, 2, 3};
        float lv[4] = {1, 2, 3, 40};
        lab_part_cells(w, "rods", 4, lx, 2, LAB_LINE, lc);
        lab_field(w, "stress", LAB_AT_NODE, 4, lv);
        double qx[12] = {-1, -1, -0.2, 8, -1, -0.2, 8, 3, -0.2, -1, 3, -0.2};
        int qc[4] = {0, 1, 2, 3};
        lab_part_cells(w, "frame", 4, qx, 1, LAB_QUAD, qc);
        LabBlock b = {{6, 5, 4}, 0, 0, {-0.5, -0.5, -0.1}, {1.5, 0.8, 0.5}};
        float sv[120], mv[120], qv[120];
        for (int i = 0; i < 120; i++) sv[i] = (float)(-25 + 0.47 * i + k), mv[i] = (i % 7 == 0) ? 1.0f : 0.0f, qv[i] = (float)(4.0 * sin(0.3 * i));
        sv[17] = NAN;
        lab_part_blocks(w, "air", 1, &b);
        lab_field(w, "stress", LAB_AT_CELL, 120, sv);
        lab_field(w, "material", LAB_AT_CELL, 120, mv);
        lab_field(w, "Q", LAB_AT_CELL, 120, qv);
        if (!lab_frame_end(w)) printf("FAIL: frame %d not written\n", k), exit(1);
    }
    if (!lab_close(w)) printf("FAIL: result not closed\n"), exit(1);
}

static void opts_of(const LabXrView *v, LabViewOpts *o) { /* the options the app's window draws the view with */
    labview_defaults(o);
    o->field = v->field, o->cmap = v->cmap, o->mesh = v->mesh, o->absval = v->absval, o->logscale = v->logscale;
    o->solid_points = v->solid_points, o->true_size = v->true_size, o->radius = v->radius, o->point_radius_m = v->point_radius_m;
    o->nlooks = v->nlooks;
    for (int i = 0; i < v->nlooks; i++) strcpy(o->looks[i].part, v->looks[i].part), o->looks[i].look = v->looks[i].look;
}

/* criteria 2 to 7 for one frame in one view */
static void check_frame(LabFile *lf, int frame, const LabXrView *v, const char *name) {
    unsigned char *glb = NULL;
    size_t len = 0;
    char err[256];
    if (!labxr_frame_glb(lf, frame, v, &glb, &len, err, sizeof err)) {
        CHECK(0, "%s frame %d: %s", name, frame, err);
        return;
    }
    Gl g;
    if (!gl_open(&g, glb, len)) {
        free(glb);
        return;
    }
    gl_structure(&g);
    LabFrame fr;
    lab_read_frame(lf, frame, &fr, err, sizeof err);
    LabViewOpts o;
    opts_of(v, &o);
    LabScene s;
    CHECK(labscene_build(&fr, &o, v->section_axis, v->section_fraction, v->section_flip, &s), "%s: reference scene", name);
    const JsonValue *tk = json_get(json_get(json_at(json_get(g.j, "nodes"), 0), "extras"), "result");
    double M[16];
    CHECK(json_get_numbers(json_get(tk, "to_source"), M, 16), "%s: to_source", name);
    double lo = v->lo, hi = v->hi;

    /* triangles, particles and lines of the GLB, in lab axes */
    size_t ntri = 0, cap = 1024, npt = 0, nln = 0;
    Tri *tris = malloc(cap * sizeof *tris);
    Corner *pts = malloc((s.npoints + 1) * sizeof *pts), *lns = malloc((s.nlines + 1) * sizeof *lns);
    double area = 0, area_ref = 0;
    const JsonValue *prims = json_get(json_at(json_get(g.j, "meshes"), 0), "primitives");
    for (size_t p = 0; p < json_len(prims); p++) {
        const JsonValue *pr = json_at(prims, p), *at = json_get(pr, "attributes"), *ex = json_get(pr, "extras");
        int mode = (int)json_get_int(pr, "mode", 4);
        float key = key_name(json_get_str(ex, "key", ""), json_get_str(ex, "look", ""));
        size_t nv, nu, nval = 0, nr = 0, nn = 0;
        const float *pos = gl_floats(&g, (int)json_get_int(at, "POSITION", -1), &nv), *uv = gl_floats(&g, (int)json_get_int(at, "TEXCOORD_0", -1), &nu);
        const float *val = json_get(at, "_VALUE") ? gl_floats(&g, (int)json_get_int(at, "_VALUE", -1), &nval) : NULL;
        const float *rad = json_get(at, "_RADIUS") ? gl_floats(&g, (int)json_get_int(at, "_RADIUS", -1), &nr) : NULL;
        const float *nrm = json_get(at, "NORMAL") ? gl_floats(&g, (int)json_get_int(at, "NORMAL", -1), &nn) : NULL;
        CHECK((key == 0) == (val != NULL), "%s: a value exactly where the field colours", name);
        if (key >= 3) CHECK(!val, "%s: a look carries no value", name);
        if (mode == 4) CHECK(nrm || json_get_bool(ex, "flat", false), "%s: triangles are smooth or flat", name);
        size_t ni = nv;
        const uint32_t *idx = NULL;
        if (json_get(pr, "indices")) {
            size_t l;
            const JsonValue *a = json_at(json_get(g.j, "accessors"), (size_t)json_get_int(pr, "indices", -1));
            idx = (const uint32_t *)gl_view(&g, (int)json_get_int(a, "bufferView", -1), &l);
            ni = (size_t)json_get_num(a, "count", 0);
        }
        if (mode == 4 && ntri + ni / 3 + 1 > cap) {
            while (ntri + ni / 3 + 1 > cap) cap *= 2;
            tris = realloc(tris, cap * sizeof *tris);
        }
        for (size_t i = 0; i < nv; i++) { /* criterion 3 */
            float want = val && isfinite(val[i]) ? (float)((0.5 + 247 * fmin(1, fmax(0, hi > lo ? (val[i] - lo) / (hi - lo) : .5))) / 256) : 252.0f / 256;
            if (key == 0) CHECK(fabsf(uv[2 * i] - want) < 1e-6f, "%s: palette u %g for value %g, want %g", name, uv[2 * i], val[i], want);
            if (nrm) CHECK(fabsf(nrm[3 * i] * nrm[3 * i] + nrm[3 * i + 1] * nrm[3 * i + 1] + nrm[3 * i + 2] * nrm[3 * i + 2] - 1) < 1e-5f, "%s: unit normal", name);
        }
        for (size_t i = 0; i < ni; i++) {
            uint32_t q = idx ? idx[i] : (uint32_t)i;
            Corner c = {{0}, val ? val[q] : 0, rad ? rad[q] : (float)json_get_num(ex, "radius_m", 0), key};
            to_lab(M, pos + 3 * q, c.p);
            if (mode == 4) {
                tris[i / 3 + ntri].c[i % 3] = c;
                if (i % 3 == 2) area += tri_area(tris[i / 3 + ntri].c[0].p, tris[i / 3 + ntri].c[1].p, c.p);
            } else if (mode == 0 && npt < s.npoints + 1) pts[npt++] = c;
            else if (mode == 1 && nln < s.nlines + 1) lns[nln++] = c;
        }
        if (mode == 4) ntri += ni / 3;
    }
    /* criterion 2 */
    CHECK(ntri * 3 == s.ntri, "%s frame %d: %zu triangles, the app draws %zu", name, frame, ntri, s.ntri / 3);
    if (ntri * 3 == s.ntri) {
        Tri *ref = malloc((ntri + 1) * sizeof *ref);
        for (size_t t = 0; t < ntri; t++) {
            const LabVertex *a = s.tri + 3 * t;
            float key = key_of(a);
            for (int q = 1; q < 3; q++)
                if (key_of(a + q) == 0) key = 0;
            for (int q = 0; q < 3; q++) {
                Corner c = {{a[q].p[0], a[q].p[1], a[q].p[2]}, key == 0 ? (a[q].field > 0 ? a[q].value : NAN) : 0, 0, key};
                ref[t].c[q] = c;
            }
            area_ref += tri_area(a[0].p, a[1].p, a[2].p);
            tri_sort(ref + t), tri_sort(tris + t);
        }
        for (size_t t = 0; t < ntri; t++)
            for (int q = 0; q < 3; q++) /* NaN compares by its bits; the converter writes the default quiet NaN */
                if (isnan(ref[t].c[q].value)) ref[t].c[q].value = NAN;
        for (size_t t = 0; t < ntri; t++)
            for (int q = 0; q < 3; q++)
                if (isnan(tris[t].c[q].value)) tris[t].c[q].value = NAN;
        qsort(ref, ntri, sizeof *ref, tri_cmp), qsort(tris, ntri, sizeof *tris, tri_cmp);
        CHECK(!memcmp(ref, tris, ntri * sizeof *ref), "%s frame %d: the triangles' corners and values are the app's", name, frame);
        CHECK(fabs(area - area_ref) <= 1e-6 * fmax(area_ref, 1e-12), "%s: area %.9g against %.9g", name, area, area_ref);
        free(ref);
    }
    /* criterion 4 */
    CHECK(npt == s.npoints, "%s: %zu particles, the app draws %zu", name, npt, s.npoints);
    CHECK(nln == s.nlines, "%s: %zu line corners, the app draws %zu", name, nln, s.nlines);
    if (npt == s.npoints) {
        Corner *ref = malloc((npt + 1) * sizeof *ref);
        for (size_t i = 0; i < npt; i++) {
            const LabVertex *a = s.points + i;
            Corner c = {{a->p[0], a->p[1], a->p[2]}, key_of(a) == 0 ? a->value : 0, a->radius, key_of(a)};
            ref[i] = c;
        }
        qsort(ref, npt, sizeof *ref, corner_cmp), qsort(pts, npt, sizeof *pts, corner_cmp);
        CHECK(!memcmp(ref, pts, npt * sizeof *ref), "%s: particles' positions, values and radii are the app's", name);
        free(ref);
    }
    if (nln == s.nlines) {
        Corner *ref = malloc((nln + 1) * sizeof *ref);
        for (size_t i = 0; i < nln; i++) {
            const LabVertex *a = s.lines + i;
            float key = key_of(a);
            if (key_of(s.lines + (i ^ 1)) == 0) key = 0; /* a segment with a value is coloured by the field */
            Corner c = {{a->p[0], a->p[1], a->p[2]}, key == 0 ? (a->field > 0 ? a->value : NAN) : 0, 0, key};
            ref[i] = c;
        }
        for (size_t i = 0; i < nln; i++) lns[i].radius = 0;
        qsort(ref, nln, sizeof *ref, corner_cmp), qsort(lns, nln, sizeof *lns, corner_cmp);
        CHECK(!memcmp(ref, lns, nln * sizeof *ref), "%s: line corners are the app's", name);
        free(ref);
    }
    /* criterion 5: the frame quad wears steel whenever the app draws it */
    bool steel = false, drawn = false;
    for (size_t p = 0; p < json_len(prims); p++) {
        const JsonValue *ex = json_get(json_at(prims, p), "extras");
        steel = steel || (!strcmp(json_get_str(ex, "look", ""), "steel") && json_get_int(ex, "part", -1) == 4);
    }
    for (size_t t = 0; t < s.ntri; t++) drawn = drawn || s.tri[t].part == 4;
    CHECK(steel == drawn, "%s: the frame is drawn in steel (%d) exactly when the app draws it (%d)", name, steel, drawn);
    /* criterion 7 */
    CHECK(json_get_int(tk, "frame", -1) == frame && json_get_num(tk, "time", -1) == lab_frame_time(lf, frame), "%s: frame and time", name);
    CHECK(!strcmp(json_get_str(tk, "field", ""), v->field) && json_get_num(tk, "lo", 0) == lo && json_get_num(tk, "hi", 0) == hi, "%s: field and range", name);
    CHECK(json_len(json_get(tk, "parts")) == 6 && !strcmp(json_str(json_at(json_get(tk, "parts"), 3)), "rods"), "%s: parts", name);
    double blo[3], bhi[3];
    CHECK(json_get_numbers(json_get(json_get(tk, "box"), "lo"), blo, 3) && json_get_numbers(json_get(json_get(tk, "box"), "hi"), bhi, 3) &&
              !memcmp(blo, s.lo, sizeof blo) && !memcmp(bhi, s.hi, sizeof bhi),
          "%s: box", name);
    /* criterion 6 */
    const JsonValue *vo = json_get(tk, "volume");
    const LabPart *vol = labscene_volume(&fr);
    const LabField *vf = vol ? lab_find_field(vol, v->field) : NULL;
    CHECK((vo && vo->type == JSON_OBJECT) == (vf != NULL), "%s: a volume exactly when it holds the field", name);
    if (vf && vo && vo->type == JSON_OBJECT) {
        size_t nc = lab_block_cells(vol->blocks), lv, lm = 0;
        const uint16_t *h = (const uint16_t *)gl_view(&g, (int)json_get_int(vo, "values", -1), &lv);
        int mview = (int)json_get_int(vo, "mask", -1);
        const unsigned char *m8 = mview >= 0 ? gl_view(&g, mview, &lm) : NULL;
        double dims[3], org[3], cell[3];
        CHECK(json_get_numbers(json_get(vo, "dims"), dims, 3) && json_get_numbers(json_get(vo, "origin"), org, 3) &&
                  json_get_numbers(json_get(vo, "cell"), cell, 3) && dims[0] == 6 && dims[1] == 5 && dims[2] == 4 &&
                  !memcmp(org, vol->blocks->origin, sizeof org) && !memcmp(cell, vol->blocks->dx, sizeof cell),
              "%s: volume grid", name);
        CHECK(lv == nc * 2, "%s: one half float per cell", name);
        double span = hi - lo, zero = lo < 0 && hi > 0 ? -lo / span : 0;
        int bad = 0;
        for (size_t i = 0; i < nc && lv == nc * 2; i++) {
            double x = vf->data[i], want;
            if (!isfinite(x)) want = zero;
            else {
                if (v->absval) x = fabs(x);
                if (v->logscale) x = log10(fmax(fabs(x), 1e-30));
                want = (x - lo) / span;
            }
            double got = glb_half_to_float(h[i]);
            if (!(fabs(got - want) <= fabs(want) * 0x1p-11 + 0x1p-25)) bad++;
        }
        CHECK(bad == 0, "%s: %d volume samples off by more than half precision", name, bad);
        float *mask = malloc(nc * sizeof *mask);
        int mk = labscene_volume_mask(vol, v->iso_field[0] ? v->iso_field : NULL, v->iso_level, mask);
        const char *want_mode = mk == 'i' ? "iso" : v->solid_volume ? "solid" : "fog";
        CHECK(!strcmp(json_get_str(vo, "mode", ""), want_mode), "%s: volume mode %s, want %s", name, json_get_str(vo, "mode", ""), want_mode);
        CHECK(json_get_bool(vo, "walls", true) == (v->walls && mk != 'i'), "%s: walls", name);
        CHECK((m8 != NULL) == (mk != 0) && (!m8 || lm == nc), "%s: a mask exactly when the app has one", name);
        int mbad = 0;
        for (size_t i = 0; m8 && i < nc; i++)
            if (m8[i] != (unsigned char)lrintf(fminf(1, fmaxf(0, mask[i])) * 255)) mbad++;
        CHECK(mbad == 0, "%s: %d mask bytes differ", name, mbad);
        free(mask);
    }
    free(tris), free(pts), free(lns);
    labscene_free(&s);
    lab_frame_free(&fr);
    json_free(g.j);
    free(glb);
}

/* ---- half floats: an independent reference ----------------------------------------------------------------------- */

static uint16_t ref_half(float f) {
    uint16_t sign = signbit(f) ? 0x8000 : 0;
    double x = fabs((double)f);
    if (isnan(f)) return sign | 0x7E00;
    if (x >= 65520.0) return sign | 0x7C00; /* past the largest half by half an ulp: infinity */
    if (x < 0x1p-14) {                       /* subnormal halves are multiples of 2^-24 */
        double m = nearbyint(x / 0x1p-24);
        return (uint16_t)(sign | (uint16_t)m); /* m = 1024 is the smallest normal, 0x0400, as it should be */
    }
    int e;
    frexp(x, &e); /* x = f 2^e, f in [0.5, 1) */
    e -= 1;       /* x in [2^e, 2^(e+1)) */
    double m = nearbyint(x / ldexp(1, e - 10));
    if (m == 2048) m = 1024, e++;
    if (e > 15) return sign | 0x7C00;
    return (uint16_t)(sign | (uint16_t)((e + 15) << 10) | (uint16_t)(m - 1024));
}

static void check_halves(void) {
    int bad = 0;
    for (uint32_t h = 0; h < 65536; h++) {
        float f = glb_half_to_float((uint16_t)h);
        uint16_t back = glb_half(f);
        bool nan = ((h >> 10) & 0x1F) == 0x1F && (h & 0x3FF);
        if (nan ? !(isnan(f) && ((back >> 10) & 0x1F) == 0x1F && (back & 0x3FF)) : back != h) bad++;
    }
    CHECK(bad == 0, "%d half patterns do not survive half -> float -> half", bad);
    uint64_t r = 0x9E3779B97F4A7C15ull;
    int off = 0, hw = 0;
    for (int i = 0; i < 2000000; i++) {
        r ^= r << 13, r ^= r >> 7, r ^= r << 17;
        uint32_t bits = (uint32_t)r;
        if (i % 2) bits = (bits & 0x8FFFFFFFu) | 0x30000000u; /* half of them near the half range */
        float f;
        memcpy(&f, &bits, 4);
        uint16_t got = glb_half(f), want = ref_half(f);
        if (isnan(f)) {
            if (((got >> 10) & 0x1F) != 0x1F || !(got & 0x3FF)) off++;
            continue;
        }
        if (got != want) off++;
#if defined(__aarch64__)
        __fp16 hf = (__fp16)f;
        uint16_t hb;
        memcpy(&hb, &hf, 2);
        if (hb != got) hw++;
#endif
    }
    CHECK(off == 0, "%d floats round differently from the reference", off);
    CHECK(hw == 0, "%d floats round differently from the hardware's __fp16", hw);
}

/* ---- the view's JSON --------------------------------------------------------------------------------------------- */

static void check_view_json(void) {
    LabXrView a, b;
    labxr_view_defaults(&a);
    strcpy(a.field, "stress"), strcpy(a.unit, "Pa");
    a.lo = -3.5, a.hi = 12.25, a.cmap = 102, a.section_axis = 2, a.section_fraction = 0.375, a.section_flip = true;
    strcpy(a.iso_field, "Q"), a.iso_level = 1500;
    a.water_surface = true, a.water_spacing = 0.02, a.mesh = a.absval = a.logscale = a.solid_points = a.true_size = a.solid_volume = a.walls = true;
    a.radius = 4, a.point_radius_m = 0.0124, a.nlooks = 2;
    strcpy(a.looks[0].part, "rotor"), a.looks[0].look = LOOK_STEEL, strcpy(a.looks[1].part, "magnets N"), a.looks[1].look = LOOK_MAGNET_NORTH;
    JsonValue *j = labxr_view_json(&a);
    char *txt = json_dump(j, 0, NULL, NULL);
    JsonValue *k = json_parse(txt, strlen(txt), NULL, NULL);
    char err[200];
    labxr_view_defaults(&b);
    CHECK(labxr_view_from_json(k, &b, err, sizeof err), "view parses: %s", err);
    CHECK(!memcmp(&a, &b, sizeof a), "the view survives its JSON");
    static const char *wrong[] = {"{\"cmap\":7}", "{\"section\":{\"axis\":3}}", "{\"section\":{\"fraction\":2}}", "{\"looks\":{\"x\":\"gold\"}}",
                                  "{\"lo\":\"x\"}", "{\"lo\":5,\"hi\":1}", "{\"field\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}", "[1]",
                                  "{\"mesh\":1}"};
    for (size_t i = 0; i < sizeof wrong / sizeof *wrong; i++) {
        JsonValue *w = json_parse(wrong[i], strlen(wrong[i]), NULL, NULL);
        LabXrView c;
        labxr_view_defaults(&c);
        err[0] = 0;
        CHECK(!labxr_view_from_json(w, &c, err, sizeof err) && err[0], "a wrong view is refused: %s", wrong[i]);
        json_free(w);
    }
    json_free(j), json_free(k), free(txt);
}

/* ---- results on this computer ------------------------------------------------------------------------------------ */

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

static void check_real(void) {
    const char *home = getenv("HOME");
    char dir[1024];
    char projects[900] = ""; /* ~/OpenPhysicsAI-Projects, or ~/NAVIER-Projects while only that exists */
    if (home && op_projects_dir(projects, sizeof projects, NULL)) snprintf(dir, sizeof dir, "%s/lab", projects);
    else snprintf(dir, sizeof dir, "/nonexistent");
    DIR *d = opendir(dir);
    if (!d) {
        puts("labxrtest: no results in ~/OpenPhysicsAI-Projects/lab (the real-result checks are skipped)");
        return;
    }
    struct dirent *e;
    int files = 0, frames = 0, planar = 0;
    double worst = 0;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".lab") || e->d_name[0] == '.') continue;
        char path[1400], err[300];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        LabFile *lf = lab_open(path, err, sizeof err);
        if (!lf) continue;
        JsonValue *info = labxr_info(lf, err, sizeof err);
        CHECK(info != NULL, "%s: info: %s", e->d_name, err);
        LabFrame f0;
        bool native = false; /* drawn in 3D: what the converter can draw, whatever the app's domain choices */
        if (lab_read_frame(lf, 0, &f0, err, sizeof err)) native = labscene_supported(&f0), lab_frame_free(&f0);
        LabXrView v;
        labxr_view_defaults(&v);
        LabFrame fr; /* the first field of the first part, its range from that frame */
        if (lab_read_frame(lf, 0, &fr, err, sizeof err)) {
            for (int p = 0; p < fr.nparts && !v.field[0]; p++)
                for (int k = 0; k < fr.parts[p].nfields && !v.field[0]; k++)
                    if (strcmp(fr.parts[p].fields[k].name, "material")) snprintf(v.field, sizeof v.field, "%s", fr.parts[p].fields[k].name);
            lab_frame_free(&fr);
        }
        v.lo = -1, v.hi = 1;
        int nf = lab_frame_count(lf), pick[3] = {0, nf / 2, nf - 1};
        for (int k = 0; k < 3; k++) {
            unsigned char *glb = NULL;
            size_t len = 0;
            double t0 = now();
            bool ok = labxr_frame_glb(lf, pick[k], &v, &glb, &len, err, sizeof err);
            double dt = now() - t0;
            worst = fmax(worst, dt);
            if (!native) { /* a plane, drawn as a picture of it */
                CHECK(ok, "%s frame %d: a planar result is drawn as a picture (%s)", e->d_name, pick[k], ok ? "" : err);
                Gl g;
                if (ok && gl_open(&g, glb, len)) {
                    gl_structure(&g);
                    const JsonValue *pic = json_get(json_get(json_get(json_at(json_get(g.j, "nodes"), 0), "extras"), "result"), "picture");
                    CHECK(pic && pic->type == JSON_OBJECT, "%s: the frame says it is a picture", e->d_name);
                    json_free(g.j);
                }
                planar += k == 0;
            } else {
                CHECK(ok, "%s frame %d: %s", e->d_name, pick[k], err);
                CHECK(dt < 2.0, "%s frame %d took %.2f s", e->d_name, pick[k], dt);
                Gl g;
                if (ok && gl_open(&g, glb, len)) gl_structure(&g), json_free(g.j), frames++;
            }
            free(glb);
        }
        files++;
        json_free(info);
        lab_close_file(lf);
    }
    closedir(d);
    printf("labxrtest: %d results on this computer: %d 3D frames converted and checked, %d planar results drawn as pictures, slowest %.2f s\n", files,
           frames, planar, worst);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--write")) {
        PATH = argv[2];
        write_result();
        return 0;
    }
    write_result();
    char err[256];
    LabFile *lf = lab_open(PATH, err, sizeof err);
    if (!lf) return printf("FAIL: %s\n", err), 1;
    LabXrView v;
    /* the six views */
    for (int view = 0; view < 6; view++) {
        labxr_view_defaults(&v);
        strcpy(v.field, view == 1 ? "T" : "stress"), strcpy(v.unit, view == 1 ? "K" : "Pa");
        v.lo = -10, v.hi = 30, v.walls = true, v.nlooks = 1;
        strcpy(v.looks[0].part, "frame"), v.looks[0].look = LOOK_STEEL;
        static const char *names[] = {"stress", "node field T", "section, edges, |stress|", "vortex surfaces of Q", "solid volume", "log10 |stress|"};
        if (view == 1) v.lo = 0, v.hi = 20;
        if (view == 2) v.section_axis = 0, v.section_fraction = 0.5, v.mesh = true, v.absval = true, v.lo = 0, v.hi = 30;
        if (view == 3) strcpy(v.iso_field, "Q"), v.iso_level = 2.0;
        if (view == 4) v.solid_volume = true, v.true_size = true, v.walls = false;
        if (view == 5) v.logscale = true, v.lo = -1, v.hi = 1.6, v.section_axis = 2, v.section_fraction = 0.3, v.section_flip = true;
        for (int f = 0; f < 3; f++) check_frame(lf, f, &v, names[view]);
    }
    /* criterion 8 */
    unsigned char *glb = NULL;
    size_t len = 0;
    labxr_view_defaults(&v);
    strcpy(v.field, "stress");
    CHECK(!labxr_frame_glb(lf, 3, &v, &glb, &len, err, sizeof err) && strstr(err, "frames 0 to 2"), "frame 3 refused: %s", err);
    CHECK(!labxr_frame_glb(lf, -1, &v, &glb, &len, err, sizeof err), "frame -1 refused");
    JsonValue *info = labxr_info(lf, err, sizeof err);
    double lo[3], hi[3];
    CHECK(info && json_get_int(info, "frames", 0) == 3, "info frames");
    CHECK(json_len(json_get(info, "times")) == 3 && json_at(json_get(info, "times"), 1)->u.number == lab_frame_time(lf, 1), "info times");
    CHECK(!strcmp(json_get_str(json_get(info, "fields"), "Q", ""), "1/s^2") && json_len(json_get(info, "parts")) == 6 && json_get_bool(info, "native3d", false),
          "info fields, parts, 3D");
    CHECK(json_get_numbers(json_get(json_get(info, "box"), "lo"), lo, 3) && json_get_numbers(json_get(json_get(info, "box"), "hi"), hi, 3) &&
              lo[0] == -1 && lo[1] == -1 && fabs(lo[2] - (-0.2)) < 1e-12 && hi[0] == 8.5 && hi[1] == 3.5 && fabs(hi[2] - 1.9) < 1e-12,
          "info box %g %g %g to %g %g %g", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    json_free(info);
    lab_close_file(lf);
    { /* a planar result: a 16 x 12 block of 0.25 m cells starting at (1, 2), its field 2 x + 3 y + 1 at the cell centres */
        LabWriter *w = lab_create("build/labxrtest_plane.lab", "{\"domain\":\"test\",\"title\":\"plane\",\"fields\":{\"p\":\"Pa\"}}", err, sizeof err);
        LabBlock b = {{16, 12, 1}, 0, 0, {1, 2, 0}, {0.25, 0.25, 1}};
        float p[192];
        for (int j = 0; j < 12; j++)
            for (int i = 0; i < 16; i++) p[j * 16 + i] = (float)(2 * (1 + (i + 0.5) * 0.25) + 3 * (2 + (j + 0.5) * 0.25) + 1);
        lab_frame_begin(w, 0), lab_part_blocks(w, "slice", 1, &b), lab_field(w, "p", LAB_AT_CELL, 192, p), lab_frame_end(w), lab_close(w);
        LabFile *pl = lab_open("build/labxrtest_plane.lab", err, sizeof err);
        labxr_view_defaults(&v);
        strcpy(v.field, "p"), strcpy(v.unit, "Pa"), v.lo = 8, v.hi = 22;
        bool ok = pl && labxr_frame_glb(pl, 0, &v, &glb, &len, err, sizeof err);
        CHECK(ok, "a planar result is drawn: %s", ok ? "" : err);
        Gl g;
        if (ok && gl_open(&g, glb, len)) {
            gl_structure(&g);
            const JsonValue *prims = json_get(json_at(json_get(g.j, "meshes"), 0), "primitives");
            const JsonValue *pr = json_at(prims, 0), *tk = json_get(json_get(json_at(json_get(g.j, "nodes"), 0), "extras"), "result");
            const JsonValue *pic = json_get(tk, "picture");
            size_t nv;
            const float *pos = gl_floats(&g, (int)json_get_int(json_get(pr, "attributes"), "POSITION", -1), &nv);
            double lo3[3], hi3[3], dims[2];
            CHECK(json_len(prims) == 1 && nv == 4 && !strcmp(json_get_str(json_get(pr, "extras"), "key", ""), "picture"), "one quad, a picture");
            CHECK(json_get_numbers(json_get(pic, "gltf_lo"), lo3, 3) && json_get_numbers(json_get(pic, "gltf_hi"), hi3, 3) &&
                      lo3[0] == 1 && lo3[1] == 2 && hi3[0] == 5 && hi3[1] == 5 && lo3[2] == 0 && hi3[2] == 0,
                  "the quad stands upright over the plane's extent, x 1 to 5 m, y 2 to 5 m");
            float plo[3] = {1e9f, 1e9f, 1e9f}, phi[3] = {-1e9f, -1e9f, -1e9f};
            for (size_t i = 0; i < nv; i++)
                for (int k = 0; k < 3; k++) plo[k] = fminf(plo[k], pos[3 * i + k]), phi[k] = fmaxf(phi[k], pos[3 * i + k]);
            CHECK(plo[0] == 1 && phi[0] == 5 && plo[1] == 2 && phi[1] == 5 && plo[2] == 0 && phi[2] == 0, "its corners are the box's");
            CHECK(json_get_numbers(json_get(pic, "dims"), dims, 2) && dims[0] == 256 && dims[1] == 192, "a value grid of the plane's proportions, 256 x 192");
            size_t lv;
            const uint16_t *hv = (const uint16_t *)gl_view(&g, (int)json_get_int(pic, "values", -1), &lv);
            int bad = 0, n = 0;
            for (int y = 0; lv == 256 * 192 * 2 && y < 192; y++)
                for (int x = 0; x < 256; x++) {
                    double X = 1 + (x + 0.5) * 4.0 / 256, Y = 2 + (y + 0.5) * 3.0 / 192; /* rows from the bottom */
                    if (X < 1.125 || X > 4.875 || Y < 2.125 || Y > 4.875) continue; /* between the outer cell centres */
                    double want = (2 * X + 3 * Y + 1 - 8) / 14, got = glb_half_to_float(hv[y * 256 + x]);
                    n++, bad += !(fabs(got - want) <= fabs(want) * 0x1p-10 + 1e-6);
                }
            CHECK(n > 40000 && bad == 0, "the grid is the field 2 x + 3 y + 1 inside the plane: %d of %d samples off", bad, n);
            json_free(g.j);
        }
        free(glb), glb = NULL;
        JsonValue *pinfo = pl ? labxr_info(pl, err, sizeof err) : NULL;
        double ilo[3];
        CHECK(pinfo && !json_get_bool(pinfo, "native3d", true) && json_get_numbers(json_get(json_get(pinfo, "picture"), "gltf_lo"), ilo, 3) && ilo[0] == 1 && ilo[1] == 2,
              "the info gives the picture's box");
        json_free(pinfo);
        if (pl) lab_close_file(pl);
        remove("build/labxrtest_plane.lab");
    }
    check_view_json();
    check_halves();
    remove(PATH);
    check_real();
    if (failures) return printf("labxrtest: %d FAILED\n", failures), 1;
    puts("labxrtest: GLB structure, triangles, values and palette, particles and lines, looks, volume samples and mask, extras, "
         "refusals, view JSON and half floats PASS");
    return 0;
}
