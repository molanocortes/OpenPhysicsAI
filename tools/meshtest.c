/* meshtest.c - surface patches and voxel hexahedral meshes against exact answers
 *   make build/meshtest && ./build/meshtest */
#include "../src/geom/hexmesh.h"
#include "../src/geom/mesh.h"
#include "../src/geom/patches.h"
#include "../src/geom/surface.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

static void add_box(Mesh *m, double x0, double y0, double z0, double x1, double y1, double z1) {
    vec3 p[8];
    for (int i = 0; i < 8; i++) p[i] = v3((float)(i & 1 ? x1 : x0), (float)(i & 2 ? y1 : y0), (float)(i & 4 ? z1 : z0));
    static const int q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++) mesh_add_quad(m, p[q[f][0]], p[q[f][1]], p[q[f][2]], p[q[f][3]]);
}

/* closed cylinder around the z axis through (cx, cy); hole = inward-facing lateral surface without caps */
static void add_cylinder(Mesh *m, double cx, double cy, double r, double z0, double z1, int seg, bool caps, bool inward) {
    for (int i = 0; i < seg; i++) {
        double a0 = 2 * M_PI * i / seg, a1 = 2 * M_PI * (i + 1) / seg;
        vec3 b0 = v3((float)(cx + r * cos(a0)), (float)(cy + r * sin(a0)), (float)z0), b1 = v3((float)(cx + r * cos(a1)), (float)(cy + r * sin(a1)), (float)z0);
        vec3 t0 = v3(b0.x, b0.y, (float)z1), t1 = v3(b1.x, b1.y, (float)z1);
        if (inward) mesh_add_quad(m, b0, t0, t1, b1);
        else mesh_add_quad(m, b0, b1, t1, t0);
        if (caps) {
            mesh_add_tri(m, v3((float)cx, (float)cy, (float)z0), b1, b0);
            mesh_add_tri(m, v3((float)cx, (float)cy, (float)z1), t0, t1);
        }
    }
}

/* L profile extruded along z: (0,0) (30,0) (30,5) (5,5) (5,25) (0,25), thickness H */
static void add_lbracket(Mesh *m, double H) {
    const double P[6][2] = {{0, 0}, {30, 0}, {30, 5}, {5, 5}, {5, 25}, {0, 25}};
    const int T[4][3] = {{0, 1, 2}, {0, 2, 3}, {0, 3, 5}, {3, 4, 5}};
    for (int t = 0; t < 4; t++) {
        vec3 a = v3((float)P[T[t][0]][0], (float)P[T[t][0]][1], 0), b = v3((float)P[T[t][1]][0], (float)P[T[t][1]][1], 0), c = v3((float)P[T[t][2]][0], (float)P[T[t][2]][1], 0);
        mesh_add_tri(m, a, c, b); /* bottom, normal -z */
        mesh_add_tri(m, v3(a.x, a.y, (float)H), v3(b.x, b.y, (float)H), v3(c.x, c.y, (float)H));
    }
    for (int i = 0; i < 6; i++) {
        const double *p0 = P[i], *p1 = P[(i + 1) % 6];
        mesh_add_quad(m, v3((float)p0[0], (float)p0[1], 0), v3((float)p1[0], (float)p1[1], 0), v3((float)p1[0], (float)p1[1], (float)H),
                      v3((float)p0[0], (float)p0[1], (float)H));
    }
}

typedef struct {
    Surface s;
    SurfaceComponent *comps;
    SurfaceDiagnostics d;
    double *bv; /* metres */
} Body;

static bool body_make(Mesh *m, Body *b) {
    char err[256];
    if (!surface_build(m, NULL, &b->s, &b->comps, &b->d, err, sizeof err)) {
        printf("  surface: %s\n", err);
        return false;
    }
    b->bv = malloc((size_t)b->s.nv * 3 * sizeof(double));
    for (int i = 0; i < 3 * b->s.nv; i++) b->bv[i] = 1e-3 * b->s.v[i];
    return true;
}

static void body_free(Body *b) {
    surface_free(&b->s);
    free(b->comps);
    free(b->bv);
}

static void test_patches(void) {
    printf("== surface patches\n");
    Mesh m;
    mesh_init(&m);
    add_box(&m, 0, 0, 0, 20, 10, 5);
    Body b;
    body_make(&m, &b);
    PatchSet ps;
    char err[256];
    CHECK(patches_build(&b.s, 2, 30, &ps, err, sizeof err), "box patches: %s", err);
    CHECK(ps.n == 6, "box has 6 patches (%d)", ps.n);
    int planar = 0, adj4 = 0;
    double area = 0;
    for (int p = 0; p < ps.n; p++) {
        PatchGeom g;
        patch_geometry(&b.s, b.s.v, NULL, &ps, p, &g);
        planar += g.type == PATCH_PLANAR && g.normal_spread_deg < 1e-6;
        adj4 += ps.adj_start[p + 1] - ps.adj_start[p] == 4;
        area += g.area;
    }
    CHECK(planar == 6 && adj4 == 6 && fabs(area - 700) < 1e-9, "all planar, each adjacent to four others, total area %g", area);
    patches_free(&ps);
    body_free(&b);
    mesh_free(&m);

    mesh_init(&m);
    add_cylinder(&m, 0, 0, 10, 0, 20, 96, true, false);
    body_make(&m, &b);
    CHECK(patches_build(&b.s, 2, 30, &ps, err, sizeof err) && ps.n == 3, "cylinder: 3 patches (%d)", ps.n);
    int cyl = -1;
    for (int p = 0; p < ps.n; p++)
        if (ps.type[p] == PATCH_CYLINDRICAL) cyl = p;
    CHECK(cyl >= 0, "lateral surface classified as cylindrical");
    if (cyl >= 0) {
        PatchGeom g;
        patch_geometry(&b.s, b.s.v, NULL, &ps, cyl, &g);
        CHECK(fabs(g.radius - 10) < 0.01 && fabs(fabs(g.axis[2]) - 1) < 1e-6 && !g.concave, "radius %.4f, axis (%.3f %.3f %.3f), boss", g.radius, g.axis[0], g.axis[1], g.axis[2]);
    }
    patches_free(&ps);
    body_free(&b);
    mesh_free(&m);

    /* plate with a through hole: build the plate as a closed annular solid is complex; use a box with an inward cylinder
     * shell nested inside (a cavity along z) to exercise concave classification */
    mesh_init(&m);
    add_box(&m, -20, -20, 0, 20, 20, 10);
    add_cylinder(&m, 0, 0, 5, 2, 8, 64, true, false);
    body_make(&m, &b);
    CHECK(patches_build(&b.s, 2, 30, &ps, err, sizeof err), "cavity body");
    int concave = 0;
    for (int p = 0; p < ps.n; p++) {
        if (ps.type[p] != PATCH_CYLINDRICAL) continue;
        PatchGeom g;
        patch_geometry(&b.s, b.s.v, NULL, &ps, p, &g);
        concave += g.concave && fabs(g.radius - 5) < 0.05;
    }
    CHECK(concave == 1, "cylindrical cavity wall recognised as concave (hole-like), %d found", concave);
    patches_free(&ps);
    body_free(&b);
    mesh_free(&m);

    mesh_init(&m);
    add_lbracket(&m, 8);
    body_make(&m, &b);
    CHECK(b.d.closed_solid && fabs(b.d.volume - (30 * 5 + 5 * 20) * 8) < 1e-6, "L-bracket is a closed solid, volume %g", b.d.volume);
    CHECK(patches_build(&b.s, 2, 30, &ps, err, sizeof err) && ps.n == 8, "L-bracket: 8 planar faces (%d)", ps.n);
    patches_free(&ps);
    body_free(&b);
    mesh_free(&m);
}

static bool mesh_body(Mesh *src, Body *b, HexMeshBody *hb) {
    if (!body_make(src, b)) return false;
    hb->v = b->bv;
    hb->tri = b->s.tri;
    hb->nt = b->s.nt;
    hb->normal = b->s.normal;
    hb->region = HEX_REGION_PART;
    return true;
}

static void test_hexmesh(void) {
    printf("== voxel hexahedral meshes\n");
    Mesh m;
    mesh_init(&m);
    add_box(&m, 0, 0, 0, 20, 10, 5);
    Body b;
    HexMeshBody hb;
    mesh_body(&m, &b, &hb);
    HexMeshSettings st = {{1e-3, 1e-3, 1e-3}, false, 0, 0, 0, NULL};
    HexMesh hm;
    char err[256];
    CHECK(hexmesh_generate(&hb, 1, &st, &hm, err, sizeof err), "box mesh: %s", err);
    CHECK(hm.nelems == 1000 && hm.nnodes == 21 * 11 * 6, "exact box: %d elements, %d nodes", hm.nelems, hm.nnodes);
    CHECK(hm.nfaces == 2 * (200 + 100 + 50), "boundary faces %d", hm.nfaces);
    CHECK(fabs(hm.body_volume_mesh[0] - hm.body_volume_stl[0]) < 1e-15 && fabs(hm.body_area_mesh[0] - hm.body_area_stl[0]) < 1e-12,
          "volume and area exact for an aligned box (%.6g / %.6g m^3)", hm.body_volume_mesh[0], hm.body_volume_stl[0]);
    CHECK(hm.max_face_distance < 1e-9 && hm.regions == 1 && hm.edge_contacts == 0, "faces lie on the surface (max %.2e m), one region", hm.max_face_distance);
    int compatible = 0;
    for (int f = 0; f < hm.nfaces; f++) {
        static const int DIR[6][3] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}};
        const int *d = DIR[hm.face_local[f]];
        int t = hm.face_tri[f];
        if (t >= 0 && b.s.normal[3 * t] * d[0] + b.s.normal[3 * t + 1] * d[1] + b.s.normal[3 * t + 2] * d[2] > 0.99) compatible++;
    }
    CHECK(compatible == hm.nfaces, "every boundary face maps to a triangle of the same face (%d/%d)", compatible, hm.nfaces);
    double p[3] = {0.0105, 0.0055, 0.0025};
    int el = hexmesh_locate(&hm, p);
    CHECK(el >= 0 && hm.ijk[3 * el] == 10 && hm.ijk[3 * el + 1] == 5 && hm.ijk[3 * el + 2] == 2, "point location");
    hexmesh_free(&hm);

    st.include_plate = true;
    st.plate_thickness = 3e-3;
    st.plate_margin = 2e-3;
    CHECK(hexmesh_generate(&hb, 1, &st, &hm, err, sizeof err), "box on plate: %s", err);
    int plate = 0, part = 0;
    for (int e = 0; e < hm.nelems; e++) plate += hm.region[e] == HEX_REGION_PLATE, part += hm.region[e] == HEX_REGION_PART;
    CHECK(part == 1000 && plate == 24 * 14 * 3 && hm.plate_layers == 3 && hm.regions == 1, "part %d, plate %d elements (%d x %d x 3), attached in one region",
          part, plate, hm.dims[0], hm.dims[1]);
    hexmesh_free(&hm);
    body_free(&b);
    mesh_free(&m);

    mesh_init(&m);
    add_cylinder(&m, 0, 0, 10, 0, 20, 128, true, false);
    for (uint32_t i = 0; i < 3 * m.tri_count; i++) m.pos[i].x += 15, m.pos[i].y += 15; /* keep it in positive x, y */
    mesh_body(&m, &b, &hb);
    double exact = M_PI * 100 * 20;
    double prev_err = 1;
    for (int r = 0; r < 3; r++) {
        double h = 2e-3 / (1 << r);
        HexMeshSettings sc = {{h, h, h}, false, 0, 0, 0, NULL};
        if (!hexmesh_generate(&hb, 1, &sc, &hm, err, sizeof err)) {
            CHECK(false, "cylinder mesh h=%g: %s", h, err);
            break;
        }
        double rel = fabs(hm.body_volume_mesh[0] * 1e9 - exact) / exact;
        printf("  cylinder r=10 mm, h=%.2f mm: %d elements, volume error %.2f%%, area ratio %.3f, mean/max face distance %.3f/%.3f mm\n", h * 1e3,
               hm.nelems, 100 * rel, hm.body_area_mesh[0] / hm.body_area_stl[0], hm.mean_face_distance * 1e3, hm.max_face_distance * 1e3);
        CHECK(hm.max_face_distance <= 0.9 * h * sqrt(3), "face distance bounded by the cell size");
        if (r > 0) CHECK(rel < prev_err + 1e-12, "volume error does not grow under refinement (%.4f -> %.4f)", prev_err, rel);
        prev_err = rel;
        hexmesh_free(&hm);
    }
    CHECK(prev_err < 0.02, "fine cylinder volume within 2%% (%.2f%%)", 100 * prev_err);
    body_free(&b);
    mesh_free(&m);

    /* two boxes touching only along an edge of the voxel grid */
    mesh_init(&m);
    add_box(&m, 0, 0, 0, 4, 4, 4);
    add_box(&m, 4, 4, 0, 8, 8, 4);
    mesh_body(&m, &b, &hb);
    HexMeshSettings se = {{1e-3, 1e-3, 1e-3}, false, 0, 0, 0, NULL};
    CHECK(hexmesh_generate(&hb, 1, &se, &hm, err, sizeof err), "edge contact mesh: %s", err);
    CHECK(hm.regions == 2 && hm.edge_contacts == 5, "two face-connected regions sharing an edge of 5 nodes (%d regions, %d shared nodes)", hm.regions,
          hm.edge_contacts);
    hexmesh_free(&hm);
    body_free(&b);
    mesh_free(&m);

    mesh_init(&m);
    add_box(&m, 0, 0, 0, 5, 5, 5);
    mesh_body(&m, &b, &hb);
    HexMeshSettings big = {{20e-3, 20e-3, 20e-3}, false, 0, 0, 0, NULL};
    CHECK(!hexmesh_generate(&hb, 1, &big, &hm, err, sizeof err) && strstr(err, "larger than"), "element larger than the part is refused: %s", err);
    HexMeshSettings lim = {{0.1e-3, 0.1e-3, 0.1e-3}, false, 0, 0, 1000, NULL};
    CHECK(!hexmesh_generate(&hb, 1, &lim, &hm, err, sizeof err) && strstr(err, "limit"), "element limit enforced: %s", err);
    body_free(&b);
    mesh_free(&m);
}

int main(void) {
    test_patches();
    test_hexmesh();
    printf("\n%s: %d passed, %d failed\n", g_fail ? "MESH TESTS FAILED" : "ALL MESH TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
