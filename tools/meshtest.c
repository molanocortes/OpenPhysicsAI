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

/* Printing-mesh criteria, declared 2026-10-03 before the first run:
 * PM1: 20 x 10 x 5 mm box, h=(0.4,0.5,0.2) mm: exactly 25,000 cells, positive box Jacobians,
 *      25 complete z layers, volume within 1e-12 relative, and face/centroid positions within 1e-12 m.
 * PM2: three analytically intersecting boxes with part/support ownership: the last classified region wins;
 *      only overlaps within that final region count, and all per-body volumes match exact cell counts.
 *      Serial and 4-thread meshes, owners and uncertainty counters must be identical; a box missing its +x face
 *      must report exactly 50 abstaining x columns at 1 mm, with the other two axes preserving its 1,000 cells.
 * PM3: anisotropic boundary mean is AREA-weighted, against the exact distances of a rectangular solid,
 *      within 1e-8 m (input STL helper stores floats). This is a diagnostic, not a convergence claim.
 * PM4: nonfinite sizes/geometry/normals, empty bodies, negative indices and unrepresentable grids are refused
 *      before allocation or conversion to integer grid dimensions.
 * PM5: the source volume of the closed PM1 box is translation-invariant to 1e-9 relative after a 1 km x/y
 *      translation in double precision (a numerical robustness fixture, not a physical build).
 * --bench-ownership reports an eight-body 64,000-cell fixture: geometry/ownership checks are exact;
 * timing is reported, with no acceptance threshold because the laptop is shared. */

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

static void test_print_mesh(void) {
    printf("== PM printing meshes: layer geometry, ownership and anisotropic boundary error\n");
    Mesh src;
    mesh_init(&src);
    add_box(&src, 0, 0, 0, 20, 10, 5);
    Body b;
    HexMeshBody hb;
    if (!mesh_body(&src, &b, &hb)) return;
    HexMeshSettings st = {{0.4e-3, 0.5e-3, 0.2e-3}, false, 0, 0, 0, NULL};
    HexMesh hm;
    char err[256] = {0};
    if (hexmesh_generate(&hb, 1, &st, &hm, err, sizeof err)) {
        CHECK(hm.nelems == 25000 && hm.dims[2] == 25, "PM1 complete printing layers: %d cells, %d z layers", hm.nelems, hm.dims[2]);
        CHECK(fabs(hm.body_volume_mesh[0] / (20e-3 * 10e-3 * 5e-3) - 1) < 1e-12, "PM1 exact box volume %.12g", hm.body_volume_mesh[0]);
        int layer[25] = {0};
        double maxzerr = 0, minjac = INFINITY;
        for (int e = 0; e < hm.nelems; e++) {
            int k = hm.ijk[3 * e + 2];
            if (k >= 0 && k < 25) layer[k]++;
            const double *a = hm.xyz + 3 * (size_t)hm.conn[8 * (size_t)e];
            const double *x = hm.xyz + 3 * (size_t)hm.conn[8 * (size_t)e + 1];
            const double *y = hm.xyz + 3 * (size_t)hm.conn[8 * (size_t)e + 3];
            const double *z = hm.xyz + 3 * (size_t)hm.conn[8 * (size_t)e + 4];
            maxzerr = fmax(maxzerr, fabs(a[2] - k * st.h[2]));
            minjac = fmin(minjac, (x[0] - a[0]) * (y[1] - a[1]) * (z[2] - a[2]) / 8);
        }
        int complete = 0;
        for (int k = 0; k < 25; k++) complete += layer[k] == 1000;
        CHECK(complete == 25 && maxzerr < 1e-12 && minjac > 0, "PM1 %d complete layers, max z error %.3g m, min detJ %.3g m3", complete, maxzerr, minjac);
        printf("  PM1 %d elements, %d nodes, 25 layers, min detJ %.9g mm3, %.4f s\n", hm.nelems, hm.nnodes, minjac * 1e9, hm.seconds);
        hexmesh_free(&hm);
    } else CHECK(false, "PM1 mesh: %s", err);

    for (int n = 0; n < b.s.nv; n++) b.bv[3 * n] += 1000, b.bv[3 * n + 1] += 2000;
    if (hexmesh_generate(&hb, 1, &st, &hm, err, sizeof err)) {
        double rel = fabs(hm.body_volume_stl[0] / (20e-3 * 10e-3 * 5e-3) - 1);
        CHECK(rel < 1e-9 && hm.nelems == 25000, "PM5 translated source volume relative error %.9g, %d cells", rel, hm.nelems);
        printf("  PM5 closed source-volume translation error %.9g relative\n", rel);
        hexmesh_free(&hm);
    } else CHECK(false, "PM5 translated mesh: %s", err);
    for (int n = 0; n < b.s.nv; n++) b.bv[3 * n] -= 1000, b.bv[3 * n + 1] -= 2000;

    /* Invalid numeric inputs must fail without creating a partial mesh. */
    HexMeshSettings bad = st;
    bad.h[0] = NAN;
    CHECK(!hexmesh_generate(&hb, 1, &bad, &hm, err, sizeof err) && strstr(err, "finite"), "PM4 NaN size refused: %s", err);
    bad.h[0] = INFINITY;
    CHECK(!hexmesh_generate(&hb, 1, &bad, &hm, err, sizeof err) && strstr(err, "finite"), "PM4 infinite size refused: %s", err);
    bad = st, bad.h[0] = 1e-300;
    CHECK(!hexmesh_generate(&hb, 1, &bad, &hm, err, sizeof err) && strstr(err, "grid"), "PM4 unrepresentable grid refused: %s", err);
    bad = st;
    bad.h[0] = bad.h[1] = bad.h[2] = 1e110;
    CHECK(!hexmesh_generate(&hb, 1, &bad, &hm, err, sizeof err) && strstr(err, "grid"), "PM4 overflowing cell volume refused: %s", err);
    bad.h[0] = bad.h[1] = bad.h[2] = 1e-110;
    CHECK(!hexmesh_generate(&hb, 1, &bad, &hm, err, sizeof err) && strstr(err, "grid"), "PM4 underflowing cell volume refused: %s", err);
    bad = st, bad.include_plate = true, bad.plate_thickness = INFINITY;
    CHECK(!hexmesh_generate(&hb, 1, &bad, &hm, err, sizeof err) && strstr(err, "finite"), "PM4 infinite plate size refused: %s", err);
    HexMeshBody empty = hb;
    empty.nt = 0;
    CHECK(!hexmesh_generate(&empty, 1, &st, &hm, err, sizeof err) && strstr(err, "triangle"), "PM4 empty body refused: %s", err);
    int oldtri = b.s.tri[0];
    b.s.tri[0] = -1;
    CHECK(!hexmesh_generate(&hb, 1, &st, &hm, err, sizeof err) && strstr(err, "index"), "PM4 negative index refused: %s", err);
    b.s.tri[0] = oldtri;
    double old = b.bv[0];
    b.bv[0] = NAN;
    CHECK(!hexmesh_generate(&hb, 1, &st, &hm, err, sizeof err) && strstr(err, "finite"), "PM4 NaN vertex refused: %s", err);
    b.bv[0] = old;
    old = b.s.normal[0], b.s.normal[0] = INFINITY;
    CHECK(!hexmesh_generate(&hb, 1, &st, &hm, err, sizeof err) && strstr(err, "finite"), "PM4 infinite normal refused: %s", err);
    b.s.normal[0] = old;
    body_free(&b), mesh_free(&src);

    mesh_init(&src);
    add_box(&src, 0.1, 0.2, 0, 3.9, 7.8, 0.8);
    if (!mesh_body(&src, &b, &hb)) return;
    st = (HexMeshSettings){{1e-3, 2e-3, 0.2e-3}, false, 0, 0, 0, NULL};
    if (hexmesh_generate(&hb, 1, &st, &hm, err, sizeof err)) {
        double exact = (2 * 8 * 0.8 * 0.1 + 2 * 4 * 0.8 * 0.2) / (2 * (8 * 0.8 + 4 * 0.8 + 4 * 8)) * 1e-3;
        CHECK(fabs(hm.mean_face_distance - exact) < 1e-8, "PM3 area-weighted boundary mean %.9g mm vs %.9g", hm.mean_face_distance * 1e3, exact * 1e3);
        printf("  PM3 mean surface offset %.9g mm (exact %.9g), maximum %.9g mm\n", hm.mean_face_distance * 1e3, exact * 1e3, hm.max_face_distance * 1e3);
        hexmesh_free(&hm);
    } else CHECK(false, "PM3 mesh: %s", err);
    body_free(&b), mesh_free(&src);

    /* Body 0: part x=[0,4]; 1: support x=[2,6]; 2: part x=[3,5], all y/z=[0,2] mm.
     * Result owner counts are 8,8,8. Only x=[3,4] has a final part/part overlap (4 cells). */
    Mesh ms[3];
    Body bs[3];
    HexMeshBody hbs[3];
    for (int i = 0; i < 3; i++) {
        mesh_init(&ms[i]);
        add_box(&ms[i], i == 0 ? 0 : i == 1 ? 2 : 3, 0, 0, i == 0 ? 4 : i == 1 ? 6 : 5, 2, 2);
        mesh_body(&ms[i], &bs[i], &hbs[i]);
    }
    hbs[1].region = HEX_REGION_SUPPORT;
    st = (HexMeshSettings){{1e-3, 1e-3, 1e-3}, false, 0, 0, 0, NULL};
    HexMesh serial = {0}, parallel = {0};
    bool a = hexmesh_generate(hbs, 3, &st, &serial, err, sizeof err);
    CHECK(a, "PM2 serial mesh: %s", err);
    if (a) {
        int counts[3] = {0}, wrong = 0;
        for (int e = 0; e < serial.nelems; e++) {
            int x = serial.ijk[3 * e];
            int owner = x < 2 ? 0 : x == 2 || x == 5 ? 1 : 2;
            wrong += serial.body[e] != owner || serial.region[e] != hbs[owner].region;
            if (serial.body[e] >= 0 && serial.body[e] < 3) counts[(int)serial.body[e]]++;
        }
        CHECK(serial.nelems == 24 && !wrong && counts[0] == 8 && counts[1] == 8 && counts[2] == 8, "PM2 exact owners %d/%d/%d, %d wrong", counts[0], counts[1], counts[2], wrong);
        CHECK(serial.overlap_cells == 4 && serial.overlap_pairs[2][0] == 4, "PM2 final-region overlaps %d, pair 2/0=%d", serial.overlap_cells, serial.overlap_pairs[2][0]);
        ThreadPool *pool = pool_create(4);
        st.pool = pool;
        bool p = hexmesh_generate(hbs, 3, &st, &parallel, err, sizeof err);
        CHECK(p, "PM2 threaded mesh: %s", err);
        if (p) {
            CHECK(serial.nelems == parallel.nelems && serial.nnodes == parallel.nnodes &&
                  memcmp(serial.conn, parallel.conn, (size_t)serial.nelems * 8 * sizeof(int)) == 0 &&
                  memcmp(serial.body, parallel.body, (size_t)serial.nelems) == 0 &&
                  serial.uncertain_cells == parallel.uncertain_cells && serial.abstained_rays == parallel.abstained_rays,
                  "PM2 serial/threaded mesh and classification identical");
            hexmesh_free(&parallel);
        }
        pool_destroy(pool);
        hexmesh_free(&serial);
    }
    for (int i = 0; i < 3; i++) body_free(&bs[i]), mesh_free(&ms[i]);

    mesh_init(&src);
    add_box(&src, 0, 0, 0, 20, 10, 5);
    if (!mesh_body(&src, &b, &hb)) return;
    int tri[36], nt = 0;
    double normal[36];
    for (int t = 0; t < hb.nt; t++) {
        if (hb.normal[3 * t] > 0.99) continue;
        memcpy(tri + 3 * nt, hb.tri + 3 * t, 3 * sizeof(int));
        memcpy(normal + 3 * nt, hb.normal + 3 * t, 3 * sizeof(double));
        nt++;
    }
    hb.nt = nt, hb.tri = tri, hb.normal = normal;
    st = (HexMeshSettings){{1e-3, 1e-3, 1e-3}, false, 0, 0, 0, NULL};
    if (hexmesh_generate(&hb, 1, &st, &serial, err, sizeof err)) {
        CHECK(serial.nelems == 1000 && serial.abstained_rays == 50, "PM2 open +x face: %d cells, %lld abstained columns", serial.nelems, serial.abstained_rays);
        ThreadPool *pool = pool_create(4);
        st.pool = pool;
        bool p = hexmesh_generate(&hb, 1, &st, &parallel, err, sizeof err);
        CHECK(p, "PM2 threaded abstention mesh: %s", err);
        if (p) {
            CHECK(parallel.nelems == serial.nelems && parallel.abstained_rays == 50 && parallel.uncertain_cells == serial.uncertain_cells &&
                  memcmp(serial.conn, parallel.conn, (size_t)serial.nelems * 8 * sizeof(int)) == 0,
                  "PM2 deterministic threaded abstentions: %lld vs exact 50", parallel.abstained_rays);
            hexmesh_free(&parallel);
        }
        pool_destroy(pool), hexmesh_free(&serial);
    } else CHECK(false, "PM2 open-face classification: %s", err);
    body_free(&b), mesh_free(&src);
}

static void benchmark_ownership(void) {
    Mesh src;
    mesh_init(&src);
    add_box(&src, 0, 0, 0, 40, 20, 4);
    Body b;
    HexMeshBody hb[8];
    if (!mesh_body(&src, &b, &hb[0])) return;
    for (int i = 1; i < 8; i++) hb[i] = hb[0];
    HexMeshSettings st = {{0.5e-3, 0.5e-3, 0.2e-3}, false, 0, 0, 0, NULL};
    HexMesh hm;
    char err[256];
    if (hexmesh_generate(hb, 8, &st, &hm, err, sizeof err)) {
        CHECK(hm.nelems == 64000 && hm.overlap_cells == 64000, "eight-body benchmark exact cells and overlaps %d/%d", hm.nelems, hm.overlap_cells);
        int wrong = 0;
        for (int e = 0; e < hm.nelems; e++) wrong += hm.body[e] != 7;
        CHECK(!wrong, "eight-body benchmark: last body owns every cell (%d wrong)", wrong);
        printf("  OWNERSHIP BENCHMARK: 8 bodies, %d elements, %d nodes, %d overlap cells, %.6f s\n", hm.nelems, hm.nnodes, hm.overlap_cells, hm.seconds);
        hexmesh_free(&hm);
    } else CHECK(false, "ownership benchmark: %s", err);
    body_free(&b), mesh_free(&src);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench-ownership")) {
        benchmark_ownership();
        return g_fail ? 1 : 0;
    }
    test_patches();
    test_hexmesh();
    test_print_mesh();
    printf("\n%s: %d passed, %d failed\n", g_fail ? "MESH TESTS FAILED" : "ALL MESH TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
