/* geomtest.c - verification of the geometry module: shapes, STL I/O, voxeliser, performance, visuals.
 * Run from the project root:  build/geomtest [--no-perf] [--visual-only] [shape ...] */
#include "../src/common.h"
#include "../src/threads.h"
#include "../src/geom/mesh.h"
#include "../src/geom/shapes.h"
#include "../src/geom/voxel.h"

#include <sys/stat.h>

#define OUT_DIR "build/geomtest_out"

static int g_fail = 0;
#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        if (!(cond)) {                                                                                            \
            g_fail++;                                                                                             \
            printf("  FAIL (%s:%d): ", __FILE__, __LINE__);                                                       \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
        }                                                                                                         \
    } while (0)

static bool g_filter_n = 0;
static char **g_filter = NULL;
static bool wanted(const char *name) {
    if (!g_filter_n) return true;
    for (int i = 0; g_filter[i]; i++)
        if (str_ieq(g_filter[i], name)) return true;
    return false;
}

static double analytic_volume(const char *name) {
    if (!strcmp(name, "sphere")) return M_PI / 6;
    if (!strcmp(name, "cylinder")) return M_PI * 0.25 * 4.0;
    if (!strcmp(name, "cube")) return 1.0;
    if (!strcmp(name, "plate")) return 0.02;
    if (!strcmp(name, "torus")) return 2 * M_PI * M_PI * 0.5 * 0.15 * 0.15;
    if (!strcmp(name, "airfoil")) return 0.68088 * 0.12; /* closed-TE NACA 0012 area x span 1 (0.0822 open TE) */
    return NAN;
}

/* ---------------------------------------------------------------------------------------------- */

static void test_shapes(void) {
    printf("\n== 1+2. built-in shapes: stats, analytic volumes, STL round trip ==\n");
    printf("  %-10s %7s %6s %5s %4s %10s %10s %10s %7s  %s\n", "shape", "tris", "tight", "open", "nm", "area",
           "volume", "analytic", "err%", "roundtrip");
    for (int i = 0; i < shape_count(); i++) {
        const ShapeInfo *si = shape_info(i);
        Mesh m;
        CHECK(shape_generate(si->name, &m), "generate %s", si->name);
        MeshStats st;
        mesh_compute_stats(&m, &st);
        double an = analytic_volume(si->name), err = isnan(an) ? NAN : 100.0 * (st.volume - an) / an;
        char path[256], err_s[256] = "", rt[64] = "ok";
        /* OUT_DIR, never models/: these are the repository's tracked meshes and a test must not rewrite them */
        snprintf(path, sizeof path, OUT_DIR "/%s.stl", si->name);
        bool saved = mesh_save_stl(path, &m, err_s, sizeof err_s);
        CHECK(saved, "save %s: %s", path, err_s);
        Mesh r;
        if (saved && mesh_load_stl(path, &r, err_s, sizeof err_s)) {
            bool same = r.tri_count == m.tri_count && !memcmp(&r.bmin, &m.bmin, sizeof(vec3)) &&
                        !memcmp(&r.bmax, &m.bmax, sizeof(vec3)) && !strcmp(r.name, si->name);
            if (!same) snprintf(rt, sizeof rt, "MISMATCH %u vs %u", r.tri_count, m.tri_count);
            CHECK(same, "round trip %s", si->name);
            mesh_free(&r);
        } else {
            snprintf(rt, sizeof rt, "LOAD FAILED");
            CHECK(false, "load %s: %s", path, err_s);
        }
        printf("  %-10s %7u %6s %5u %4u %10.5f %10.6f %10.6f %7.3f  %s\n", si->name, st.triangles,
               st.watertight ? "yes" : "no", st.open_edges, st.nonmanifold_edges, st.surface_area, st.volume, an, err, rt);
        CHECK(st.open_edges == 0, "%s has %u open edges", si->name, st.open_edges);
        CHECK(st.volume > 0, "%s volume not positive", si->name);
        CHECK(st.triangles < 60000, "%s too many triangles", si->name);
        CHECK(st.degenerate_removed == 0, "%s has %u degenerate triangles", si->name, st.degenerate_removed);
        if (!isnan(an)) CHECK(fabs(err) < 2.0, "%s volume error %.2f%%", si->name, err);
        CHECK(m.nrm != NULL, "%s normals missing", si->name);
        mesh_free(&m);
    }
    Mesh dummy;
    CHECK(!shape_generate("nope", &dummy), "unknown shape accepted");
    CHECK(shape_generate("GLIDER", &dummy), "case-insensitive lookup");
    mesh_free(&dummy);
}

/* ---------------------------------------------------------------------------------------------- */

static bool write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static void test_stl_io(void) {
    printf("\n== 3. STL parsing: ASCII, binary-with-'solid' header, errors ==\n");
    char err[256];
    const char *ascii = "solid  Messy_Tetra\n"
                        "  FACET NORMAL 0 0 0\n\t OUTER   LOOP\n  VERTEX 0 0 0\r\n vertex\t0 1 0\n  Vertex 1e0 0 0\n"
                        " ENDLOOP\nendfacet\n"
                        "facet normal 1 2 3 outer loop vertex 0 0 0 vertex 1 0 0 vertex 0 0 1 endloop endfacet\n"
                        "   facet normal 0 0 0\n outer loop\n vertex 0 0 0\n vertex 0 0 1.0\n vertex 0 +1 0\n"
                        " endloop\n endfacet\n"
                        "facet normal 0 0 0\nouter loop\nvertex 1 0 0\nvertex 0.0 1 0\nvertex 0 0 1\nendloop\nendfacet\n"
                        "endsolid Messy_Tetra\n";
    const char *p = OUT_DIR "/tetra_ascii.stl";
    CHECK(write_file(p, ascii, strlen(ascii)), "write %s", p);
    Mesh m;
    if (mesh_load_stl(p, &m, err, sizeof err)) {
        MeshStats st;
        mesh_compute_stats(&m, &st);
        printf("  ascii tetra: tris %u, unique verts %u, watertight %d, volume %.6f (expect 1/6), name '%s'\n",
               st.triangles, st.unique_vertices, st.watertight, st.volume, m.name);
        CHECK(st.triangles == 4 && st.unique_vertices == 4 && st.watertight, "ascii tetra topology");
        CHECK(fabs(st.volume - 1.0 / 6) < 1e-6, "ascii tetra volume %.6f", st.volume);
        CHECK(!strcmp(m.name, "tetra_ascii"), "name '%s'", m.name);
        CHECK(m.nrm && fabs(v3_len(m.nrm[0]) - 1) < 1e-4, "normals");
        mesh_free(&m);
    } else {
        CHECK(false, "ascii load: %s", err);
    }

    /* binary file whose header starts with "solid" */
    Mesh t;
    mesh_init(&t);
    mesh_add_tri(&t, v3(0, 0, 0), v3(0, 1, 0), v3(1, 0, 0));
    mesh_add_tri(&t, v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1));
    mesh_add_tri(&t, v3(0, 0, 0), v3(0, 0, 1), v3(0, 1, 0));
    mesh_add_tri(&t, v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1));
    p = OUT_DIR "/tetra_solidhdr.stl";
    CHECK(mesh_save_stl(p, &t, err, sizeof err), "save: %s", err);
    FILE *f = fopen(p, "r+b");
    if (f) {
        fwrite("solid tricky header", 1, 19, f);
        fclose(f);
    }
    if (mesh_load_stl(p, &m, err, sizeof err)) {
        CHECK(m.tri_count == 4, "binary+solid header: %u tris", m.tri_count);
        printf("  binary with 'solid' header: %u triangles ok\n", m.tri_count);
        mesh_free(&m);
    } else {
        CHECK(false, "binary+solid header load: %s", err);
    }

    /* errors */
    CHECK(!mesh_load_stl(OUT_DIR "/does_not_exist.stl", &m, err, sizeof err), "missing file accepted");
    printf("  missing file   -> \"%s\"\n", err);
    const char *bad = "solid x\nfacet normal 0 0 0\nouter loop\nvertex 0 0 0\nvertex 1 zero 0\nvertex 0 0 1\n";
    write_file(OUT_DIR "/bad_ascii.stl", bad, strlen(bad));
    CHECK(!mesh_load_stl(OUT_DIR "/bad_ascii.stl", &m, err, sizeof err), "malformed ascii accepted");
    printf("  malformed      -> \"%s\"\n", err);
    const char *nan_s = "solid x\nfacet normal 0 0 0\nouter loop\nvertex 0 0 0\nvertex 1 nan 0\nvertex 0 0 1\nendloop\n";
    write_file(OUT_DIR "/nan_ascii.stl", nan_s, strlen(nan_s));
    CHECK(!mesh_load_stl(OUT_DIR "/nan_ascii.stl", &m, err, sizeof err), "NaN accepted");
    printf("  NaN coordinate -> \"%s\"\n", err);
    write_file(OUT_DIR "/garbage.stl", "hello world, not an stl", 23);
    CHECK(!mesh_load_stl(OUT_DIR "/garbage.stl", &m, err, sizeof err), "garbage accepted");
    printf("  garbage        -> \"%s\"\n", err);

    /* transform with mirror keeps outward winding; normals follow */
    mesh_compute_normals(&t, 30);
    mesh_transform(&t, m4_mul(m4_scale(v3(-2, 1, 1)), m4_rotate_y(0.3f)));
    MeshStats st;
    mesh_compute_stats(&t, &st);
    printf("  mirrored tetra volume %.6f (expect 1/3)\n", st.volume);
    CHECK(fabs(st.volume - 1.0 / 3) < 1e-5, "mirror transform volume %.6f", st.volume);
    mesh_fix_orientation(&t);
    for (int i = 0; i < 3; i++) { /* flip by hand, fix must restore */
        vec3 tmp = t.pos[1 + 3 * i];
        t.pos[1 + 3 * i] = t.pos[2 + 3 * i], t.pos[2 + 3 * i] = tmp;
    }
    for (uint32_t i = 0; i < 4; i++) {
        vec3 tmp = t.pos[1 + 3 * i];
        t.pos[1 + 3 * i] = t.pos[2 + 3 * i], t.pos[2 + 3 * i] = tmp;
    }
    mesh_fix_orientation(&t);
    mesh_compute_stats(&t, &st);
    CHECK(st.volume > 0, "fix_orientation");
    mesh_free(&t);

    /* crease angle on a cube corner: sharp at 40 deg, smooth at 100 deg */
    Mesh c;
    shape_generate("cube", &c);
    float d40 = v3_dot(c.nrm[0], c.nrm[3]); /* not necessarily same vertex: check per-corner vs face normal */
    (void)d40;
    int sharp = 0, smooth = 0;
    for (uint32_t tt = 0; tt < c.tri_count; tt++) {
        vec3 *q = c.pos + 3 * tt;
        vec3 fnn = v3_norm(v3_cross(v3_sub(q[1], q[0]), v3_sub(q[2], q[0])));
        for (int k = 0; k < 3; k++) sharp += v3_dot(fnn, c.nrm[3 * tt + k]) > 0.9999f;
    }
    mesh_compute_normals(&c, 100);
    for (uint32_t i = 0; i < 3 * c.tri_count; i++) smooth += fabs(fabs(c.nrm[i].x) - 0.57735f) < 1e-3;
    printf("  cube normals: %d/36 corners sharp at 40 deg, %d/36 smooth at 100 deg\n", sharp, smooth);
    CHECK(sharp == 36 && smooth == 36, "crease handling");
    mesh_free(&c);
}

/* ---------------------------------------------------------------------------------------------- */

static void fit_grid(const Mesh *m, float cells, int margin, mat4 *M, int n[3], float *scale) {
    vec3 ext = v3_sub(m->bmax, m->bmin), ctr = v3_scale(v3_add(m->bmin, m->bmax), 0.5f);
    float big = MAXI(ext.x, MAXI(ext.y, ext.z)), s = cells / big;
    n[0] = (int)ceil(ext.x * s) + 2 * margin, n[1] = (int)ceil(ext.y * s) + 2 * margin;
    n[2] = (int)ceil(ext.z * s) + 2 * margin;
    *M = m4_mul(m4_translate(v3(n[0] * 0.5f, n[1] * 0.5f, n[2] * 0.5f)), m4_mul(m4_scale(v3(s, s, s)), m4_translate(v3_neg(ctr))));
    *scale = s;
}

static void test_voxel_basic(ThreadPool *pool) {
    printf("\n== 4a. voxeliser exactness / robustness ==\n");
    VoxelizeOptions o = {0, 2};
    VoxelizeResult r;
    int n = 48;
    uint8_t *g = malloc((size_t)n * n * n);
    Mesh c;
    shape_generate("cube", &c);
    /* cube faces exactly on cell boundaries 8..40 */
    mat4 M = m4_mul(m4_translate(v3(24, 24, 24)), m4_scale(v3(32, 32, 32)));
    voxelize_mesh(&c, M, n, n, n, g, &o, pool, &r);
    printf("  aligned cube 32^3: solid %llu (expect 32768), bbox [%d %d %d]-[%d %d %d], frontal %llu\n",
           (unsigned long long)r.solid_cells, r.bbox_min[0], r.bbox_min[1], r.bbox_min[2], r.bbox_max[0],
           r.bbox_max[1], r.bbox_max[2], (unsigned long long)r.frontal_cells);
    CHECK(r.solid_cells == 32768 && r.frontal_cells == 1024 && r.bbox_min[0] == 8 && r.bbox_max[2] == 39, "aligned cube");
    for (int mv = 1; mv <= 3; mv++) {
        VoxelizeOptions o2 = {0, mv};
        voxelize_mesh(&c, M, n, n, n, g, &o2, NULL, &r);
        CHECK(r.solid_cells == 32768, "aligned cube min_votes %d single-thread: %llu", mv, (unsigned long long)r.solid_cells);
    }
    /* inside-out cube: nonzero rule still fills it */
    Mesh f;
    mesh_init(&f);
    for (uint32_t t = 0; t < c.tri_count; t++) mesh_add_tri(&f, c.pos[3 * t], c.pos[3 * t + 2], c.pos[3 * t + 1]);
    voxelize_mesh(&f, M, n, n, n, g, &o, pool, &r);
    printf("  inside-out cube: solid %llu\n", (unsigned long long)r.solid_cells);
    CHECK(r.solid_cells == 32768, "inside-out cube");
    mesh_free(&f);
    /* open cube (one face missing): majority vote + even-odd fallback keep it mostly solid, no streaks */
    mesh_init(&f);
    for (uint32_t t = 2; t < c.tri_count; t++) mesh_add_tri(&f, c.pos[3 * t], c.pos[3 * t + 1], c.pos[3 * t + 2]);
    voxelize_mesh(&f, M, n, n, n, g, &o, pool, &r);
    printf("  open cube (1 face removed): solid %llu, bbox x %d..%d\n", (unsigned long long)r.solid_cells, r.bbox_min[0],
           r.bbox_max[0]);
    CHECK(r.solid_cells == 32768 && r.bbox_max[0] == 39, "open cube");
    mesh_free(&f);
    /* rotated cube, off-grid: compare with volume */
    M = m4_mul(m4_translate(v3(24.3f, 23.7f, 24.1f)), m4_mul(m4_rotate(v3(1, 2, 3), 0.7f), m4_scale(v3(26, 26, 26))));
    voxelize_mesh(&c, M, n, n, n, g, &o, pool, &r);
    printf("  rotated cube 26^3=17576: solid %llu (%.2f%%)\n", (unsigned long long)r.solid_cells,
           100.0 * ((double)r.solid_cells - 17576) / 17576);
    CHECK(fabs((double)r.solid_cells - 17576) / 17576 < 0.01, "rotated cube");
    /* two overlapping cubes: union volume, not sum */
    Mesh u;
    mesh_init(&u);
    mesh_append(&u, &c);
    mesh_transform(&c, m4_translate(v3(0.5f, 0.25f, 0)));
    mesh_append(&u, &c);
    M = m4_mul(m4_translate(v3(16, 16, 20)), m4_scale(v3(16, 16, 16)));
    voxelize_mesh(&u, M, n, n, n, g, &o, pool, &r);
    printf("  union of 2 overlapping cubes 16^3: solid %llu (expect 16^3*(2-0.5*0.75)=%d)\n",
           (unsigned long long)r.solid_cells, (int)(4096 * (2 - 0.375)));
    CHECK(r.solid_cells == (uint64_t)(4096 * (2 - 0.375)), "union");
    /* clipping: mesh partially outside the grid */
    mesh_free(&c);
    shape_generate("cube", &c);
    M = m4_mul(m4_translate(v3(0, 24, 24)), m4_scale(v3(32, 32, 32)));
    voxelize_mesh(&c, M, n, n, n, g, &o, pool, &r);
    printf("  cube clipped by grid (x<0 half outside): solid %llu (expect %d), bbox x %d..%d\n",
           (unsigned long long)r.solid_cells, 16 * 32 * 32, r.bbox_min[0], r.bbox_max[0]);
    CHECK(r.solid_cells == 16 * 32 * 32 && r.bbox_min[0] == 0 && r.bbox_max[0] == 15, "clipped cube");
    mesh_free(&u);
    mesh_free(&c);
    free(g);
}

static void test_voxel_shapes(ThreadPool *pool) {
    printf("\n== 4b. voxelised shapes (largest dimension 96 cells, min_votes 2) ==\n");
    printf("  %-10s %13s %14s %11s %7s %11s %7s %8s %6s %6s\n", "shape", "grid", "mesh vol c^3", "solid r=0", "err%",
           "solid r=.5", "err%", "frontal", "ms", "ms.5");
    for (int i = 0; i < shape_count(); i++) {
        const ShapeInfo *si = shape_info(i);
        Mesh m;
        shape_generate(si->name, &m);
        MeshStats st;
        mesh_compute_stats(&m, &st);
        mat4 M;
        int n[3];
        float s;
        fit_grid(&m, 96, 6, &M, n, &s);
        uint8_t *g = malloc((size_t)n[0] * n[1] * n[2]);
        VoxelizeOptions o0 = {0, 2}, o5 = {0.5f, 2};
        VoxelizeResult r0, r5;
        voxelize_mesh(&m, M, n[0], n[1], n[2], g, &o0, pool, &r0);
        voxelize_mesh(&m, M, n[0], n[1], n[2], g, &o5, pool, &r5);
        double vc = st.volume * s * s * s;
        double e0 = 100 * ((double)r0.solid_cells - vc) / vc, e5 = 100 * ((double)r5.solid_cells - vc) / vc;
        char gs[32];
        snprintf(gs, sizeof gs, "%dx%dx%d", n[0], n[1], n[2]);
        printf("  %-10s %13s %14.0f %11llu %7.2f %11llu %7.2f %8llu %6.1f %6.1f\n", si->name, gs, vc,
               (unsigned long long)r0.solid_cells, e0, (unsigned long long)r5.solid_cells, e5,
               (unsigned long long)r5.frontal_cells, r0.seconds * 1e3, r5.seconds * 1e3);
        bool smooth = !strcmp(si->name, "sphere") || !strcmp(si->name, "cylinder") || !strcmp(si->name, "cube") ||
                      !strcmp(si->name, "torus") || !strcmp(si->name, "ahmed") || !strcmp(si->name, "submarine");
        if (smooth) CHECK(fabs(e0) < 3.0, "%s voxel volume error %.2f%%", si->name, e0);
        CHECK(r5.solid_cells >= r0.solid_cells, "%s shell reduces solids", si->name);
        free(g);
        mesh_free(&m);
    }
}

/* ---------------------------------------------------------------------------------------------- */

static void icosphere(Mesh *out, int level, float radius) {
    const float g = (1.0f + sqrtf(5.0f)) / 2;
    const vec3 V[12] = {{-1, g, 0}, {1, g, 0}, {-1, -g, 0}, {1, -g, 0}, {0, -1, g}, {0, 1, g},
                        {0, -1, -g}, {0, 1, -g}, {g, 0, -1}, {g, 0, 1}, {-g, 0, -1}, {-g, 0, 1}};
    static const int F[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9},  {5, 11, 4},
                                 {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6},  {3, 6, 8},
                                 {3, 8, 9},  {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    Mesh a, b;
    mesh_init(&a);
    for (int f = 0; f < 20; f++) mesh_add_tri(&a, v3_norm(V[F[f][0]]), v3_norm(V[F[f][1]]), v3_norm(V[F[f][2]]));
    for (int l = 0; l < level; l++) {
        mesh_init(&b);
        mesh_reserve(&b, a.tri_count * 4);
        for (uint32_t t = 0; t < a.tri_count; t++) {
            vec3 p0 = a.pos[3 * t], p1 = a.pos[3 * t + 1], p2 = a.pos[3 * t + 2];
            vec3 m01 = v3_norm(v3_add(p0, p1)), m12 = v3_norm(v3_add(p1, p2)), m20 = v3_norm(v3_add(p2, p0));
            mesh_add_tri(&b, p0, m01, m20);
            mesh_add_tri(&b, m01, p1, m12);
            mesh_add_tri(&b, m20, m12, p2);
            mesh_add_tri(&b, m01, m12, m20);
        }
        mesh_free(&a);
        a = b;
    }
    for (uint32_t i = 0; i < 3 * a.tri_count; i++) a.pos[i] = v3_scale(a.pos[i], radius);
    mesh_compute_bounds(&a);
    *out = a;
}

static void test_perf(void) {
    printf("\n== 5. performance: icosphere level 8 into 256^3 ==\n");
    Mesh m;
    double t0 = now_seconds();
    icosphere(&m, 8, 1.0f);
    double t1 = now_seconds();
    MeshStats st;
    mesh_compute_stats(&m, &st);
    double t2 = now_seconds();
    mesh_compute_normals(&m, 35);
    double t3 = now_seconds();
    printf("  %u triangles: build %.2fs, stats %.2fs (watertight %d, %u verts), normals %.2fs\n", m.tri_count, t1 - t0,
           t2 - t1, st.watertight, st.unique_vertices, t3 - t2);
    CHECK(st.watertight, "big icosphere watertight");
    char err[256];
    double ts = now_seconds();
    bool saved = mesh_save_stl(OUT_DIR "/big_sphere.stl", &m, err, sizeof err);
    double tl = now_seconds();
    Mesh r;
    if (saved && mesh_load_stl(OUT_DIR "/big_sphere.stl", &r, err, sizeof err)) {
        printf("  binary STL save %.2fs, load (incl. normals) %.2fs, %u tris\n", tl - ts, now_seconds() - tl, r.tri_count);
        CHECK(r.tri_count == m.tri_count, "big STL round trip");
        mesh_free(&r);
    } else {
        CHECK(false, "big STL: %s", err);
    }
    remove(OUT_DIR "/big_sphere.stl");

    /* ASCII parse speed: level-7 icosphere (328k facets, ~80 MB of text) */
    Mesh a7;
    icosphere(&a7, 7, 1.0f);
    FILE *fa = fopen(OUT_DIR "/big_ascii.stl", "w");
    if (fa) {
        fprintf(fa, "solid big\n");
        for (uint32_t t = 0; t < a7.tri_count; t++) {
            const vec3 *q = a7.pos + 3 * (size_t)t;
            fprintf(fa, " facet normal 0 0 0\n  outer loop\n");
            for (int k = 0; k < 3; k++) fprintf(fa, "   vertex %.8e %.8e %.8e\n", q[k].x, q[k].y, q[k].z);
            fprintf(fa, "  endloop\n endfacet\n");
        }
        fprintf(fa, "endsolid big\n");
        fclose(fa);
        double ta = now_seconds();
        Mesh ra;
        if (mesh_load_stl(OUT_DIR "/big_ascii.stl", &ra, err, sizeof err)) {
            double tb = now_seconds();
            MeshStats sa;
            mesh_compute_stats(&ra, &sa);
            printf("  ASCII STL load (incl. normals) %.2fs for %u facets, watertight %d\n", tb - ta, ra.tri_count,
                   sa.watertight);
            CHECK(ra.tri_count == a7.tri_count && sa.watertight, "big ASCII STL");
            mesh_free(&ra);
        } else {
            CHECK(false, "big ASCII STL: %s", err);
        }
        remove(OUT_DIR "/big_ascii.stl");
    }
    mesh_free(&a7);

    const int N = 256;
    uint8_t *g = malloc((size_t)N * N * N);
    mat4 M = m4_mul(m4_translate(v3(128.2f, 127.9f, 128.1f)), m4_scale(v3(110, 110, 110)));
    double exact = 4.0 / 3.0 * M_PI * 110 * 110 * 110;
    uint64_t ref = 0;
    int threads[] = {1, 4, 8};
    for (int ti = 0; ti < 3; ti++) {
        ThreadPool *pool = pool_create(threads[ti]);
        VoxelizeOptions o = {0, 2}, o5 = {0.5f, 2};
        VoxelizeResult res, res5;
        voxelize_mesh(&m, M, N, N, N, g, &o, pool, &res);
        voxelize_mesh(&m, M, N, N, N, g, &o5, pool, &res5);
        printf("  %d thread%s: inside %.3fs (solid %llu, %.3f%% vs exact sphere), with shell 0.5 %.3fs (solid %llu)\n",
               threads[ti], threads[ti] > 1 ? "s" : " ", res.seconds, (unsigned long long)res.solid_cells,
               100 * ((double)res.solid_cells - exact) / exact, res5.seconds, (unsigned long long)res5.solid_cells);
        if (!ref) ref = res.solid_cells;
        CHECK(res.solid_cells == ref, "thread count changes result");
        if (threads[ti] == 8) CHECK(res5.seconds < 3.0, "8-thread voxelisation too slow");
        pool_destroy(pool);
    }
    free(g);
    mesh_free(&m);
}

static void test_visual(ThreadPool *pool);

int main(int argc, char **argv) {
    bool perf = true, only_visual = false;
    static char *filt[64];
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-perf")) perf = false;
        else if (!strcmp(argv[i], "--visual-only")) only_visual = true;
        else if (nf < 63) filt[nf++] = argv[i];
    }
    g_filter = filt, g_filter_n = nf > 0;
    mkdir("build", 0755);
    mkdir(OUT_DIR, 0755);
    ThreadPool *pool = pool_create(8);
    if (!only_visual) {
        test_shapes();
        test_stl_io();
        test_voxel_basic(pool);
        test_voxel_shapes(pool);
        if (perf) test_perf();
    }
    test_visual(pool);
    pool_destroy(pool);
    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL CHECKS PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* 6. visual checks: software z-buffer renders and voxel slices (BMP, converted to PNG with sips) */

static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v, b[1] = (uint8_t)(v >> 8), b[2] = (uint8_t)(v >> 16), b[3] = (uint8_t)(v >> 24); }

static bool write_bmp(const char *path, const uint8_t *rgb, int w, int h) {
    int pad = (4 - (w * 3) % 4) % 4;
    uint32_t data = (uint32_t)((w * 3 + pad) * h);
    uint8_t hdr[54] = {'B', 'M'};
    put32(hdr + 2, 54 + data), put32(hdr + 10, 54), put32(hdr + 14, 40), put32(hdr + 18, (uint32_t)w);
    put32(hdr + 22, (uint32_t)h), hdr[26] = 1, hdr[28] = 24, put32(hdr + 34, data);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(hdr, 1, 54, f) == 54;
    uint8_t *row = malloc((size_t)(w * 3 + pad));
    for (int y = h - 1; ok && y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            const uint8_t *p = rgb + 3 * ((size_t)y * w + x);
            row[3 * x] = p[2], row[3 * x + 1] = p[1], row[3 * x + 2] = p[0];
        }
        memset(row + 3 * w, 0, (size_t)pad);
        ok = fwrite(row, 1, (size_t)(w * 3 + pad), f) == (size_t)(w * 3 + pad);
    }
    free(row);
    return fclose(f) == 0 && ok;
}

static void save_png(const char *base, const uint8_t *rgb, int w, int h) {
    char bmp[256], png[256], cmd[700];
    snprintf(bmp, sizeof bmp, OUT_DIR "/%s.bmp", base);
    snprintf(png, sizeof png, OUT_DIR "/%s.png", base);
    if (!write_bmp(bmp, rgb, w, h)) {
        CHECK(false, "write %s", bmp);
        return;
    }
    snprintf(cmd, sizeof cmd, "sips -s format png '%s' --out '%s' >/dev/null 2>&1", bmp, png);
    CHECK(system(cmd) == 0, "sips failed for %s", bmp);
    remove(bmp);
    printf("  wrote %s\n", png);
}

/* Orthographic Lambert+Blinn render of m seen from direction `dir` (object -> camera) into W x H RGB.
 * Back-facing visible pixels are painted red (would reveal inverted winding). */
static void render_panel(const Mesh *m, vec3 dir, const vec3 *fmin, const vec3 *fmax, uint8_t *rgb, int W, int H) {
    enum { SS = 3 };
    int w = W * SS, h = H * SS;
    vec3 f = v3_norm(v3_neg(dir));
    vec3 upw = fabsf(f.y) > 0.99f ? v3(0, 0, -1) : v3(0, 1, 0);
    vec3 rt = v3_norm(v3_cross(f, upw)), up = v3_cross(rt, f);
    size_t nc = 3 * (size_t)m->tri_count;
    float *P = malloc(nc * 3 * sizeof(float)), *zb = malloc((size_t)w * h * sizeof(float));
    float *img = malloc((size_t)w * h * 3 * sizeof(float));
    float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
    for (size_t i = 0; i < nc; i++) {
        P[3 * i] = v3_dot(m->pos[i], rt), P[3 * i + 1] = v3_dot(m->pos[i], up), P[3 * i + 2] = v3_dot(m->pos[i], f);
        xmin = MINI(xmin, P[3 * i]), xmax = MAXI(xmax, P[3 * i]), ymin = MINI(ymin, P[3 * i + 1]), ymax = MAXI(ymax, P[3 * i + 1]);
    }
    if (fmin && fmax) { /* fit a focus box instead of the whole mesh */
        xmin = ymin = 1e30f, xmax = ymax = -1e30f;
        for (int c = 0; c < 8; c++) {
            vec3 q = v3(c & 1 ? fmax->x : fmin->x, c & 2 ? fmax->y : fmin->y, c & 4 ? fmax->z : fmin->z);
            float qx = v3_dot(q, rt), qy = v3_dot(q, up);
            xmin = MINI(xmin, qx), xmax = MAXI(xmax, qx), ymin = MINI(ymin, qy), ymax = MAXI(ymax, qy);
        }
    }
    float sc = 0.92f * MINI(w / MAXI(xmax - xmin, 1e-9f), h / MAXI(ymax - ymin, 1e-9f));
    float cx = 0.5f * (xmin + xmax), cy = 0.5f * (ymin + ymax);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float t = (float)y / h, *c = img + 3 * ((size_t)y * w + x);
            c[0] = 0.93f - 0.12f * t, c[1] = 0.95f - 0.10f * t, c[2] = 0.97f - 0.06f * t;
            zb[(size_t)y * w + x] = 1e30f;
        }
    vec3 L = v3_norm(v3(-0.45f, 0.6f, 0.65f)), Hh = v3_norm(v3_add(L, v3(0, 0, 1)));
    for (uint32_t t = 0; t < m->tri_count; t++) {
        float X[3], Y[3], Z[3];
        for (int k = 0; k < 3; k++) {
            const float *q = P + 3 * (3 * (size_t)t + k);
            X[k] = (q[0] - cx) * sc + w * 0.5f, Y[k] = h * 0.5f - (q[1] - cy) * sc, Z[k] = q[2];
        }
        float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
        if (area == 0) continue;
        bool front = area < 0; /* CCW in camera space = CW with y down */
        int x0 = MAXI(0, (int)floor(MINI(X[0], MINI(X[1], X[2])))), x1 = MINI(w - 1, (int)ceil(MAXI(X[0], MAXI(X[1], X[2]))));
        int y0 = MAXI(0, (int)floor(MINI(Y[0], MINI(Y[1], Y[2])))), y1 = MINI(h - 1, (int)ceil(MAXI(Y[0], MAXI(Y[1], Y[2]))));
        vec3 *nn = m->nrm ? m->nrm + 3 * (size_t)t : NULL;
        vec3 fn = v3_norm(v3_cross(v3_sub(m->pos[3 * t + 1], m->pos[3 * t]), v3_sub(m->pos[3 * t + 2], m->pos[3 * t])));
        for (int py = y0; py <= y1; py++)
            for (int px = x0; px <= x1; px++) {
                float sx = px + 0.5f, sy = py + 0.5f;
                float b0 = ((X[2] - X[1]) * (sy - Y[1]) - (Y[2] - Y[1]) * (sx - X[1])) / area;
                float b1 = ((X[0] - X[2]) * (sy - Y[2]) - (Y[0] - Y[2]) * (sx - X[2])) / area;
                float b2 = 1 - b0 - b1;
                if (b0 < -1e-5f || b1 < -1e-5f || b2 < -1e-5f) continue;
                float z = b0 * Z[0] + b1 * Z[1] + b2 * Z[2];
                size_t idx = (size_t)py * w + px;
                if (z >= zb[idx]) continue;
                zb[idx] = z;
                float *c = img + 3 * idx;
                if (!front) {
                    c[0] = 0.9f, c[1] = 0.1f, c[2] = 0.1f;
                    continue;
                }
                vec3 n = nn ? v3_norm(v3_add(v3_add(v3_scale(nn[0], b0), v3_scale(nn[1], b1)), v3_scale(nn[2], b2))) : fn;
                vec3 ncam = v3(v3_dot(n, rt), v3_dot(n, up), -v3_dot(n, f));
                float dif = MAXI(0.0f, v3_dot(ncam, L)), spec = powf(MAXI(0.0f, v3_dot(ncam, Hh)), 48.0f);
                float sky = 0.5f + 0.5f * n.y, head = MAXI(0.0f, ncam.z);
                for (int k = 0; k < 3; k++) {
                    static const float base[3] = {0.80f, 0.82f, 0.86f};
                    c[k] = MINI(1.0f, base[k] * (0.14f + 0.16f * sky + 0.22f * head + 0.55f * dif) + 0.25f * spec);
                }
            }
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int k = 0; k < 3; k++) {
                float s = 0;
                for (int j = 0; j < SS; j++)
                    for (int i = 0; i < SS; i++) s += img[3 * ((size_t)(y * SS + j) * w + x * SS + i) + k];
                rgb[3 * ((size_t)y * W + x) + k] = (uint8_t)CLAMP(s / (SS * SS) * 255.0f + 0.5f, 0.0f, 255.0f);
            }
    free(P), free(zb), free(img);
}

static void render_shape(const Mesh *m, const char *suffix, const vec3 dirs[4], const vec3 *fbox[4][2]) {
    enum { PW = 512 };
    int W = 2 * PW + 4, H = 2 * PW + 4;
    uint8_t *canvas = malloc((size_t)W * H * 3), *panel = malloc((size_t)PW * PW * 3);
    memset(canvas, 60, (size_t)W * H * 3);
    for (int v = 0; v < 4; v++) {
        render_panel(m, dirs[v], fbox ? fbox[v][0] : NULL, fbox ? fbox[v][1] : NULL, panel, PW, PW);
        int ox = (v % 2) * (PW + 4), oy = (v / 2) * (PW + 4);
        for (int y = 0; y < PW; y++) memcpy(canvas + 3 * ((size_t)(y + oy) * W + ox), panel + 3 * (size_t)y * PW, 3 * (size_t)PW);
    }
    char base[160];
    snprintf(base, sizeof base, "render_%s%s", m->name, suffix);
    save_png(base, canvas, W, H);
    free(canvas), free(panel);
}

/* 26-connected solid components and 6-connected fluid cells not reachable from the grid boundary. */
static void connectivity(const uint8_t *g, const int n[3], int *components, uint64_t *enclosed) {
    size_t cells = (size_t)n[0] * n[1] * n[2];
    uint8_t *seen = calloc(cells, 1);
    uint32_t *stack = malloc(cells * sizeof(uint32_t));
    size_t sp = 0;
    *components = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t s0 = 0; s0 < cells; s0++) {
            int i0 = (int)(s0 % n[0]), j0 = (int)((s0 / n[0]) % n[1]), k0 = (int)(s0 / ((size_t)n[0] * n[1]));
            bool boundary = !i0 || !j0 || !k0 || i0 == n[0] - 1 || j0 == n[1] - 1 || k0 == n[2] - 1;
            if (seen[s0] || (pass == 0 ? !g[s0] : (g[s0] || !boundary))) continue;
            if (pass == 0) (*components)++;
            seen[s0] = 1, stack[sp++] = (uint32_t)s0;
            while (sp) {
                size_t c = stack[--sp];
                int i = (int)(c % n[0]), j = (int)((c / n[0]) % n[1]), k = (int)(c / ((size_t)n[0] * n[1]));
                for (int dk = -1; dk <= 1; dk++)
                    for (int dj = -1; dj <= 1; dj++)
                        for (int di = -1; di <= 1; di++) {
                            int a = abs(di) + abs(dj) + abs(dk);
                            if (!a || (pass == 1 && a != 1)) continue;
                            int ii = i + di, jj = j + dj, kk = k + dk;
                            if (ii < 0 || jj < 0 || kk < 0 || ii >= n[0] || jj >= n[1] || kk >= n[2]) continue;
                            size_t q = (size_t)ii + (size_t)n[0] * ((size_t)jj + (size_t)n[1] * kk);
                            if (seen[q] || (pass == 0 ? !g[q] : g[q])) continue;
                            seen[q] = 1, stack[sp++] = (uint32_t)q;
                        }
            }
        }
    }
    *enclosed = 0;
    for (size_t c = 0; c < cells; c++) *enclosed += !g[c] && !seen[c];
    free(seen), free(stack);
}

/* plane: 2 = constant k (image x = i, y = j up), 1 = constant j (image x = i, y = k down) */
static void slice_png(const char *base, const uint8_t *g0, const uint8_t *g5, const int n[3], int plane, int idx) {
    int iw = n[0], ih = plane == 2 ? n[1] : n[2], s = CLAMP(1100 / MAXI(iw, ih), 1, 8);
    int W = iw * s, H = ih * s;
    uint8_t *rgb = malloc((size_t)W * H * 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int ci = x / s, cy = y / s;
            size_t c = plane == 2 ? (size_t)ci + (size_t)n[0] * ((size_t)(ih - 1 - cy) + (size_t)n[1] * idx)
                                  : (size_t)ci + (size_t)n[0] * ((size_t)idx + (size_t)n[1] * cy);
            uint8_t col[3] = {255, 255, 255};
            if (g0[c]) col[0] = 25, col[1] = 55, col[2] = 110;
            else if (g5[c]) col[0] = 110, col[1] = 170, col[2] = 235;
            if (s >= 4 && (x % s == 0 || y % s == 0)) for (int k = 0; k < 3; k++) col[k] = (uint8_t)(col[k] * 0.88f);
            memcpy(rgb + 3 * ((size_t)y * W + x), col, 3);
        }
    save_png(base, rgb, W, H);
    free(rgb);
}

static void test_visual(ThreadPool *pool) {
    printf("\n== 6. visual checks (renders + voxel slices) ==\n");
    for (int i = 0; i < shape_count(); i++) {
        const ShapeInfo *si = shape_info(i);
        if (!wanted(si->name)) continue;
        Mesh m;
        shape_generate(si->name, &m);
        static const vec3 views[4] = {{0, 0, 1}, {0, 1, 0}, {-1.0f, 0.75f, 1.1f}, {1.3f, 0.55f, -0.9f}};
        render_shape(&m, "", views, NULL);
        if (!strcmp(si->name, "glider")) { /* close-ups: cockpit / wing root, T-tail */
            vec3 a0 = v3(m.bmin.x - 0.01f, m.bmin.y - 0.01f, -0.07f), a1 = v3(m.bmin.x + 0.21f, m.bmin.y + 0.06f, 0.07f);
            vec3 t0 = v3(m.bmax.x - 0.11f, m.bmin.y + 0.02f, -0.1f), t1 = v3(m.bmax.x + 0.01f, m.bmax.y + 0.005f, 0.1f);
            static const vec3 dv[4] = {{0, 0.12f, 1}, {-1.0f, 0.6f, 0.9f}, {0.25f, 0.2f, 1}, {1.0f, 0.7f, 0.8f}};
            const vec3 *fb[4][2] = {{&a0, &a1}, {&a0, &a1}, {&t0, &t1}, {&t0, &t1}};
            render_shape(&m, "_detail", dv, fb);
        }
        static const char *slice_shapes[] = {"glider", "ahmed", "submarine", "wing"};
        bool do_slice = false;
        for (int k = 0; k < 4; k++) do_slice |= !strcmp(si->name, slice_shapes[k]);
        if (!do_slice) {
            mesh_free(&m);
            continue;
        }
        mat4 M;
        int n[3];
        float sc;
        fit_grid(&m, 256, 6, &M, n, &sc);
        size_t cells = (size_t)n[0] * n[1] * n[2];
        uint8_t *g0 = malloc(cells), *g5 = malloc(cells);
        VoxelizeOptions o0 = {0, 2}, o5 = {0.5f, 2};
        VoxelizeResult r0, r5;
        voxelize_mesh(&m, M, n[0], n[1], n[2], g0, &o0, pool, &r0);
        voxelize_mesh(&m, M, n[0], n[1], n[2], g5, &o5, pool, &r5);
        int comp0, comp5;
        uint64_t encl0, encl5, outside = 0;
        connectivity(g0, n, &comp0, &encl0);
        connectivity(g5, n, &comp5, &encl5);
        vec3 lo = m4_mul_point(M, m.bmin), hi = m4_mul_point(M, m.bmax); /* leak check: solids beyond bbox+1 */
        for (int k = 0; k < n[2]; k++)
            for (int j = 0; j < n[1]; j++)
                for (int ii = 0; ii < n[0]; ii++) {
                    if (!g5[(size_t)ii + (size_t)n[0] * ((size_t)j + (size_t)n[1] * k)]) continue;
                    float x = ii + 0.5f, y = j + 0.5f, z = k + 0.5f;
                    outside += x < lo.x - 1 || x > hi.x + 1 || y < lo.y - 1 || y > hi.y + 1 || z < lo.z - 1 || z > hi.z + 1;
                }
        printf("  %s grid %dx%dx%d: solid %llu / %llu (shell), components %d / %d, enclosed fluid %llu / %llu, "
               "outside bbox %llu, %.0f / %.0f ms\n",
               si->name, n[0], n[1], n[2], (unsigned long long)r0.solid_cells, (unsigned long long)r5.solid_cells, comp0,
               comp5, (unsigned long long)encl0, (unsigned long long)encl5, (unsigned long long)outside, r0.seconds * 1e3,
               r5.seconds * 1e3);
        CHECK(comp5 == 1, "%s voxel solid not one connected component (%d)", si->name, comp5);
        CHECK(outside == 0 && encl5 == 0, "%s voxel leaks: %llu outside, %llu enclosed", si->name,
              (unsigned long long)outside, (unsigned long long)encl5);
        /* y plane with the most solid cells (wing planform / hull axis) */
        int best = 0;
        uint64_t bc = 0;
        for (int j = 0; j < n[1]; j++) {
            uint64_t c = 0;
            for (int k = 0; k < n[2]; k++)
                for (int ii = 0; ii < n[0]; ii++) c += g5[(size_t)ii + (size_t)n[0] * ((size_t)j + (size_t)n[1] * k)];
            if (c > bc) bc = c, best = j;
        }
        char base[160];
        snprintf(base, sizeof base, "slice_%s_midZ", si->name);
        slice_png(base, g0, g5, n, 2, n[2] / 2);
        snprintf(base, sizeof base, "slice_%s_Z3q", si->name);
        slice_png(base, g0, g5, n, 2, 3 * n[2] / 4);
        snprintf(base, sizeof base, "slice_%s_midY", si->name);
        slice_png(base, g0, g5, n, 1, n[1] / 2);
        snprintf(base, sizeof base, "slice_%s_maxY%d", si->name, best);
        slice_png(base, g0, g5, n, 1, best);
        free(g0), free(g5);
        mesh_free(&m);
    }
}
