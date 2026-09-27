/* surftest.c - tests for STL surface topology, repair reporting, shells, self-intersections and thickness.
 *   make build/surftest && ./build/surftest */
#include "../src/geom/bvh.h"
#include "../src/geom/mesh.h"
#include "../src/geom/surface.h"

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

static bool near(double a, double b, double tol) { return fabs(a - b) <= tol; }

/* axis-aligned box, outward counter-clockwise winding (inward when `inward`) */
static void add_box(Mesh *m, double x0, double y0, double z0, double x1, double y1, double z1, bool inward) {
    vec3 p[8];
    for (int i = 0; i < 8; i++) p[i] = v3((float)(i & 1 ? x1 : x0), (float)(i & 2 ? y1 : y0), (float)(i & 4 ? z1 : z0));
    static const int q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++) {
        if (inward) mesh_add_quad(m, p[q[f][0]], p[q[f][3]], p[q[f][2]], p[q[f][1]]);
        else mesh_add_quad(m, p[q[f][0]], p[q[f][1]], p[q[f][2]], p[q[f][3]]);
    }
}

typedef struct {
    Surface s;
    SurfaceComponent *comps;
    SurfaceDiagnostics d;
    bool ok;
} Built;

static Built build(Mesh *m, const SurfaceRepairOptions *o) {
    Built b;
    char err[256];
    b.ok = surface_build(m, o, &b.s, &b.comps, &b.d, err, sizeof err);
    if (!b.ok) printf("  surface_build: %s\n", err);
    return b;
}

static void done(Built *b, Mesh *m) {
    surface_free(&b->s);
    free(b->comps);
    mesh_free(m);
}

int main(void) {
    printf("== closed cube\n");
    {
        Mesh m;
        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, false);
        Built b = build(&m, NULL);
        CHECK(b.ok && b.d.closed_solid && b.d.watertight, "closed solid");
        CHECK(near(b.d.volume, 1000, 1e-9) && near(b.d.area, 600, 1e-9), "volume %g area %g", b.d.volume, b.d.area);
        CHECK(b.s.nv == 8 && b.s.nt == 12 && b.d.components == 1 && b.d.boundary_loops == 0, "nv %d nt %d", b.s.nv, b.s.nt);
        CHECK(b.d.flipped_components == 0 && b.d.flipped_triangles == 0 && b.d.inconsistent_edges == 0, "no repair needed");
        Bvh bvh;
        bvh_build(&bvh, b.s.v, b.s.tri, b.s.nt);
        CHECK(surface_point_inside(&b.s, &bvh, (double[]){5, 5, 5}) && !surface_point_inside(&b.s, &bvh, (double[]){15, 5, 5}), "inside test");
        CHECK(surface_check_self_intersections(&b.s, &bvh, 100, &b.d) == 0, "no self-intersections");
        surface_check_thickness(&b.s, &bvh, 600, &b.d);
        CHECK(b.d.thickness_samples > 100 && near(b.d.min_thickness, 10, 1e-6), "thickness %g (%d samples)", b.d.min_thickness, b.d.thickness_samples);
        bvh_free(&bvh);
        done(&b, &m);
    }
    printf("== inverted cube is reoriented and reported\n");
    {
        Mesh m;
        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, true);
        Built b = build(&m, NULL);
        CHECK(b.ok && b.d.closed_solid && near(b.d.volume, 1000, 1e-9) && b.d.flipped_components == 1, "flipped %d volume %g", b.d.flipped_components, b.d.volume);
        done(&b, &m);
        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, true);
        SurfaceRepairOptions o;
        surface_repair_defaults(&o);
        o.orient_outward = false;
        b = build(&m, &o);
        CHECK(b.ok && !b.d.closed_solid && near(b.d.volume, -1000, 1e-9), "without repair the negative volume is kept (%g)", b.d.volume);
        done(&b, &m);
    }
    printf("== open box (missing top) lists the hole\n");
    {
        Mesh m;
        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, false);
        m.tri_count -= 10; /* keep only the bottom face (2 triangles) */
        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, false);
        /* remove the +Z face: triangles 2 and 3 */
        memmove(m.pos + 6, m.pos + 12, (size_t)(m.tri_count - 4) * 3 * sizeof(vec3));
        m.tri_count -= 2;
        Built b = build(&m, NULL);
        CHECK(b.ok && !b.d.watertight && !b.d.closed_solid && b.d.open_edges == 4, "open edges %u", b.d.open_edges);
        CHECK(b.d.boundary_loops == 1 && b.d.nholes == 1 && near(b.d.holes[0].perimeter, 40, 1e-9) && b.d.holes[0].edges == 4,
              "hole: loops %u perimeter %g edges %d", b.d.boundary_loops, b.d.nholes ? b.d.holes[0].perimeter : 0, b.d.nholes ? b.d.holes[0].edges : 0);
        CHECK(near(b.d.holes[0].centroid[2], 10, 1e-9), "hole centroid z %g", b.d.holes[0].centroid[2]);
        done(&b, &m);
    }
    printf("== cavity (nested shell)\n");
    {
        for (int wrong = 0; wrong < 2; wrong++) {
            Mesh m;
            mesh_init(&m);
            add_box(&m, 0, 0, 0, 10, 10, 10, false);
            add_box(&m, 3, 3, 3, 7, 7, 7, !wrong); /* a correct cavity faces inward */
            Built b = build(&m, NULL);
            CHECK(b.ok && b.d.closed_solid && near(b.d.volume, 936, 1e-9) && b.d.nested_shells == 1, "cavity (%s inner shell): volume %g nested %d",
                  wrong ? "outward" : "inward", b.d.volume, b.d.nested_shells);
            CHECK(b.d.flipped_components == wrong, "flipped components %d", b.d.flipped_components);
            int depth1 = 0;
            for (int c = 0; c < b.s.ncomp; c++) depth1 += b.comps[c].depth == 1 && b.comps[c].parent >= 0;
            CHECK(depth1 == 1, "one shell at depth 1");
            Bvh bvh;
            bvh_build(&bvh, b.s.v, b.s.tri, b.s.nt);
            CHECK(!surface_point_inside(&b.s, &bvh, (double[]){5, 5, 5}) && surface_point_inside(&b.s, &bvh, (double[]){1.5, 5, 5}), "cavity is outside the material");
            surface_check_thickness(&b.s, &bvh, 2000, &b.d);
            CHECK(near(b.d.min_thickness, 3, 1e-6), "wall thickness around the cavity %g", b.d.min_thickness);
            bvh_free(&bvh);
            done(&b, &m);
        }
    }
    printf("== separate bodies, duplicates, non-manifold, self-intersection\n");
    {
        Mesh m;
        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, false);
        add_box(&m, 20, 0, 0, 30, 10, 10, false);
        Built b = build(&m, NULL);
        CHECK(b.ok && b.d.components == 2 && b.d.closed_components == 2 && near(b.d.volume, 2000, 1e-9) && b.d.nested_shells == 0, "two bodies");
        done(&b, &m);

        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, false);
        mesh_add_tri(&m, m.pos[0], m.pos[1], m.pos[2]);
        b = build(&m, NULL);
        CHECK(b.ok && b.d.duplicate_faces == 1 && b.d.duplicate_removed == 1 && b.d.closed_solid, "duplicate removed and reported");
        done(&b, &m);

        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, false);
        add_box(&m, 10, 10, 0, 20, 20, 10, false); /* shares the vertical edge x=10,y=10 */
        b = build(&m, NULL);
        CHECK(b.ok && b.d.nonmanifold_edges >= 1 && !b.d.watertight, "edge-touching boxes: non-manifold edges %u", b.d.nonmanifold_edges);
        done(&b, &m);

        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, false);
        add_box(&m, 5, 5, 5, 15, 15, 15, false);
        b = build(&m, NULL);
        Bvh bvh;
        bvh_build(&bvh, b.s.v, b.s.tri, b.s.nt);
        int n = surface_check_self_intersections(&b.s, &bvh, 1000, &b.d);
        CHECK(n > 0 && b.d.self_intersection_samples > 0, "overlapping boxes intersect (%d pairs)", n);
        bvh_free(&bvh);
        done(&b, &m);
    }
    printf("== inconsistent winding, degenerate faces, welding\n");
    {
        Mesh m;
        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, false);
        vec3 t = m.pos[4];
        m.pos[4] = m.pos[5], m.pos[5] = t; /* flip triangle 1 */
        Built b = build(&m, NULL);
        CHECK(b.ok && b.d.inconsistent_edges == 3 && b.d.flipped_triangles == 1 && b.d.consistently_oriented && b.d.closed_solid,
              "inconsistent %u flipped %d", b.d.inconsistent_edges, b.d.flipped_triangles);
        done(&b, &m);

        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, false);
        mesh_add_tri(&m, v3(0, 0, 0), v3(5, 0, 0), v3(10, 0, 0));
        b = build(&m, NULL);
        CHECK(b.ok && b.d.degenerate == 1 && b.d.degenerate_removed == 1 && b.d.closed_solid, "degenerate %u", b.d.degenerate);
        done(&b, &m);

        mesh_init(&m);
        add_box(&m, 0, 0, 0, 10, 10, 10, false);
        for (uint32_t i = 0; i < 3 * m.tri_count; i += 5) m.pos[i].x += 2e-6f; /* below 1e-6 of the 17.3 diagonal? no: tolerance ~1.7e-5 */
        b = build(&m, NULL);
        CHECK(b.ok && b.s.nv == 8 && b.d.closed_solid, "jittered vertices welded (nv %d, tolerance %g)", b.s.nv, b.d.weld_tolerance);
        done(&b, &m);
    }
    printf("== thin plate thickness\n");
    {
        Mesh m;
        mesh_init(&m);
        add_box(&m, 0, 0, 0, 40, 40, 1.5, false);
        Built b = build(&m, NULL);
        Bvh bvh;
        bvh_build(&bvh, b.s.v, b.s.tri, b.s.nt);
        surface_check_thickness(&b.s, &bvh, 1000, &b.d);
        CHECK(near(b.d.min_thickness, 1.5, 1e-6) && near(b.d.thickness_median, 1.5, 1e-6), "plate: min %g median %g p05 %g", b.d.min_thickness,
              b.d.thickness_median, b.d.thickness_p05);
        bvh_free(&bvh);
        done(&b, &m);
    }
    printf("\n%s: %d passed, %d failed\n", g_fail ? "SURFACE TESTS FAILED" : "ALL SURFACE TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
