/* tettest.c - verification of the conforming tetrahedral mesh and the TET4 / TET10 elements (docs/contracts/tet-mesh.md)
 *
 *  Mesher: M1 cube on lattice planes, M2 sphere (volume convergence), M3 thin plate with a hole (no lost wall), M6
 *  conformity checked on every mesh. M4 / M5 (the owner's STL files) run with --stl, they are too large for make test:
 *     ./build/tettest --stl FILE.stl --size 2 [--order 1|2] [--scale 0.001]
 *  Elements: see the second half of the file.
 *   make build/tettest && ./build/tettest */
#include "../src/fem/dense.h"
#include "../src/fem/solid.h"
#include "../src/geom/mesh.h"
#include "../src/geom/tetmesh.h"
#include "../src/threads.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double now_seconds(void);

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
#define NOTE(...)                                                                                                     \
    do {                                                                                                              \
        printf("  ");                                                                                                 \
        printf(__VA_ARGS__);                                                                                          \
        printf("\n");                                                                                                 \
    } while (0)

static ThreadPool *g_pool;

/* ---- analytic shapes -------------------------------------------------------------------------------------------- */

typedef struct {
    double lo[3], hi[3];
} BoxShape;

static double box_sdf(void *ctx, const double p[3]) {
    const BoxShape *b = ctx;
    double q[3], out = 0, in = -INFINITY;
    for (int k = 0; k < 3; k++) {
        double c = 0.5 * (b->lo[k] + b->hi[k]), hh = 0.5 * (b->hi[k] - b->lo[k]);
        q[k] = fabs(p[k] - c) - hh;
        if (q[k] > 0) out += q[k] * q[k];
        in = fmax(in, q[k]);
    }
    return sqrt(out) + fmin(in, 0);
}

static void box_project(void *ctx, const double p[3], double q[3]) {
    const BoxShape *b = ctx;
    bool inside = true;
    for (int k = 0; k < 3; k++) {
        q[k] = fmin(fmax(p[k], b->lo[k]), b->hi[k]);
        if (p[k] < b->lo[k] || p[k] > b->hi[k]) inside = false;
    }
    if (inside) { /* to the nearest face */
        int best = 0, side = 0;
        double d = INFINITY;
        for (int k = 0; k < 3; k++) {
            if (p[k] - b->lo[k] < d) d = p[k] - b->lo[k], best = k, side = 0;
            if (b->hi[k] - p[k] < d) d = b->hi[k] - p[k], best = k, side = 1;
        }
        q[best] = side ? b->hi[best] : b->lo[best];
    }
}

typedef struct {
    double c[3], r;
} Sphere;

static double sphere_sdf(void *ctx, const double p[3]) {
    const Sphere *s = ctx;
    double d[3] = {p[0] - s->c[0], p[1] - s->c[1], p[2] - s->c[2]};
    return sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - s->r;
}

static void sphere_project(void *ctx, const double p[3], double q[3]) {
    const Sphere *s = ctx;
    double d[3] = {p[0] - s->c[0], p[1] - s->c[1], p[2] - s->c[2]}, l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int k = 0; k < 3; k++) q[k] = s->c[k] + (l > 0 ? d[k] / l * s->r : (k == 0 ? s->r : 0));
}

/* a plate [0,Lx] x [0,Ly] x [0,t] with a circular hole of radius a around (cx, cy), optionally rotated about x */
typedef struct {
    double Lx, Ly, t, cx, cy, a;
    double rot; /* radians about the x axis, through the origin */
} Plate;

static void plate_local(const Plate *pl, const double p[3], double q[3]) {
    double c = cos(-pl->rot), s = sin(-pl->rot);
    q[0] = p[0], q[1] = c * p[1] - s * p[2], q[2] = s * p[1] + c * p[2];
}

static double plate_sdf(void *ctx, const double p[3]) {
    const Plate *pl = ctx;
    double q[3];
    plate_local(pl, p, q);
    BoxShape b = {{0, 0, 0}, {pl->Lx, pl->Ly, pl->t}};
    double fb = box_sdf(&b, q);
    double r = hypot(q[0] - pl->cx, q[1] - pl->cy);
    return fmax(fb, pl->a - r);
}

static void grad_project(TetSdfFn f, void *ctx, const double p[3], double q[3], double eps) {
    memcpy(q, p, 3 * sizeof(double));
    for (int it = 0; it < 8; it++) {
        double v = f(ctx, q), g[3];
        if (fabs(v) < 1e-12) break;
        for (int k = 0; k < 3; k++) {
            double a[3] = {q[0], q[1], q[2]}, b[3] = {q[0], q[1], q[2]};
            a[k] += eps, b[k] -= eps;
            g[k] = (f(ctx, a) - f(ctx, b)) / (2 * eps);
        }
        double gg = g[0] * g[0] + g[1] * g[1] + g[2] * g[2];
        if (!(gg > 0)) break;
        for (int k = 0; k < 3; k++) q[k] -= v * g[k] / gg;
    }
}

static void plate_project(void *ctx, const double p[3], double q[3]) {
    const Plate *pl = ctx;
    grad_project(plate_sdf, ctx, p, q, 1e-6 * pl->t);
}

/* ---- mesh checks -------------------------------------------------------------------------------------------------- */

typedef struct {
    int a, b;
} EdgeRec;

static int edge_cmp(const void *x, const void *y) {
    const EdgeRec *p = x, *q = y;
    if (p->a != q->a) return p->a < q->a ? -1 : 1;
    return (p->b > q->b) - (p->b < q->b);
}

/* M6: no face shared by more than two elements, and the boundary a closed 2-manifold (every boundary edge in two
 * boundary faces) */
static bool conforming(const TetMesh *m, int *bad_edges) {
    EdgeRec *e = malloc(3 * (size_t)(m->nfaces ? m->nfaces : 1) * sizeof *e);
    int n = 0;
    for (int f = 0; f < m->nfaces; f++) {
        int el = m->face_elem[f], k = m->face_local[f];
        for (int q = 0; q < 3; q++) {
            int a = m->conn[m->npe * (size_t)el + TETMESH_FACE[k][q]], b = m->conn[m->npe * (size_t)el + TETMESH_FACE[k][(q + 1) % 3]];
            e[n].a = a < b ? a : b, e[n].b = a < b ? b : a, n++;
        }
    }
    qsort(e, (size_t)n, sizeof *e, edge_cmp);
    int bad = 0, odd = 0;
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && e[j].a == e[i].a && e[j].b == e[i].b) j++;
        if (j - i != 2) bad++;
        if ((j - i) % 2) odd++;
        i = j;
    }
    if (bad) printf("  (boundary edges in other than two faces: %d, of which %d in an odd number: %s)\n", bad, odd,
                    odd ? "a crack, the mesh is not conforming" : "pieces touching along edges, conforming");
    free(e);
    *bad_edges = odd;
    return odd == 0 && m->nonmanifold_faces == 0;
}

static void report(const char *name, const TetMesh *m) {
    double verr = m->ref_volume > 0 ? 100 * (m->volume - m->ref_volume) / m->ref_volume : NAN;
    printf("  %-26s TET%-2d %8d el %8d nodes  dihedral %6.2f..%6.2f  aspect %6.2f  node dist %.2e/%.2e  face dist %.2e/%.2e  vol err %+.4f%%  "
           "regions %d  warped %d  steiner %d  thin %d  curved %d/%d  %.2f s\n",
           name, m->order == 2 ? 10 : 4, m->nelems, m->nnodes, m->min_dihedral, m->max_dihedral, m->worst_aspect, m->surf_node_dist_max,
           m->surf_node_dist_mean, m->surf_face_dist_max, m->surf_face_dist_mean, verr, m->regions, m->warped_vertices, m->steiner_vertices,
           m->thin_splits, m->curved_edges, m->curved_edges + m->straightened_edges, m->seconds);
}

static bool make_mesh(TetSdfFn f, TetProjectFn pr, void *ctx, const double lo[3], const double hi[3], double h, double isize, int order,
                      double refv, TetMesh *m, char *err, size_t errlen) {
    TetMeshSettings s = {0};
    s.surface_size = h, s.interior_size = isize, s.order = order;
    memcpy(s.lo, lo, sizeof s.lo), memcpy(s.hi, hi, sizeof s.hi);
    s.sdf = f, s.project = pr, s.ctx = ctx, s.ref_volume = refv, s.pool = g_pool;
    s.min_angle_floor = 1e-6; /* the tests read the angles themselves */
    return tetmesh_generate(&s, m, err, errlen);
}

static void test_cube(void) {
    printf("== M1. cube on lattice planes\n");
    BoxShape b = {{0, 0, 0}, {1, 1, 1}};
    for (int order = 1; order <= 2; order++) {
        TetMesh m;
        char err[256];
        bool ok = make_mesh(box_sdf, box_project, &b, b.lo, b.hi, 1.0 / 8, 0.5, order, 1.0, &m, err, sizeof err);
        CHECK(ok, "cube TET%d: %s", order == 2 ? 10 : 4, err);
        if (!ok) continue;
        report("cube h=1/8", &m);
        int bad;
        CHECK(fabs(m.volume - 1) < 1e-12, "volume exact: %.15g", m.volume);
        CHECK(m.min_dihedral >= 10 && m.max_dihedral <= 165, "angles %.2f..%.2f within 10..165", m.min_dihedral, m.max_dihedral);
        CHECK(m.regions == 1, "one region (%d)", m.regions);
        CHECK(conforming(&m, &bad), "M6 conforming: %d non-manifold faces, %d boundary edges not in two faces", m.nonmanifold_faces, bad);
        tetmesh_free(&m);
    }
}

static void test_sphere(void) {
    printf("== M2. sphere, analytic distance: volume convergence\n");
    Sphere sp = {{0.0123, -0.0071, 0.0049}, 1}; /* off-lattice centre */
    double lo[3], hi[3], V = 4.0 / 3.0 * M_PI;
    for (int k = 0; k < 3; k++) lo[k] = sp.c[k] - 1, hi[k] = sp.c[k] + 1;
    double err1[3], err2[3];
    int nh[3] = {4, 8, 16};
    for (int order = 1; order <= 2; order++) {
        for (int i = 0; i < 3; i++) {
            TetMesh m;
            char err[256], name[64];
            bool ok = make_mesh(sphere_sdf, sphere_project, &sp, lo, hi, 1.0 / nh[i], 4.0 / nh[i], order, V, &m, err, sizeof err);
            CHECK(ok, "sphere h=r/%d: %s", nh[i], err);
            if (!ok) continue;
            snprintf(name, sizeof name, "sphere h=r/%d", nh[i]);
            report(name, &m);
            double e = fabs(m.volume - V) / V;
            (order == 1 ? err1 : err2)[i] = e;
            int bad;
            CHECK(conforming(&m, &bad), "M6 conforming: %d non-manifold faces, %d open boundary edges", m.nonmanifold_faces, bad);
            CHECK(m.regions == 1, "one region (%d)", m.regions);
            CHECK(m.surf_node_dist_max < 1e-9, "surface nodes on the sphere (%.2e)", m.surf_node_dist_max);
            CHECK(m.min_dihedral >= 10 && m.max_dihedral <= 165, "angles %.2f..%.2f within the 10..165 target", m.min_dihedral, m.max_dihedral);
            tetmesh_free(&m);
        }
    }
    double p1 = log2(err1[0] / err1[1]), p2 = log2(err1[1] / err1[2]);
    printf("  TET4 volume error %.3e %.3e %.3e, observed order %.2f %.2f\n", err1[0], err1[1], err1[2], p1, p2);
    CHECK(p1 > 1.5 && p1 < 2.5 && p2 > 1.5 && p2 < 2.5, "TET4 volume error falls at order 2 (%.2f, %.2f)", p1, p2);
    printf("  TET10 (curved) volume error %.3e %.3e %.3e, observed order %.2f %.2f (reported)\n", err2[0], err2[1], err2[2],
           log2(err2[0] / err2[1]), log2(err2[1] / err2[2]));
}

/* point in tetrahedron by barycentric coordinates */
static bool in_tet(const double X[4][3], const double p[3], double tol) {
    double V = tet_signed_volume(X);
    for (int k = 0; k < 4; k++) {
        double Y[4][3];
        memcpy(Y, X, sizeof Y);
        memcpy(Y[k], p, sizeof Y[k]);
        if (tet_signed_volume(Y) < -tol * fabs(V)) return false;
    }
    return true;
}

static int covered(const TetMesh *m, const double *pts, int np) {
    int hit = 0;
    for (int i = 0; i < np; i++) {
        const double *p = pts + 3 * (size_t)i;
        for (int e = 0; e < m->nelems; e++) {
            double X[4][3];
            bool near = true;
            for (int k = 0; k < 4; k++) memcpy(X[k], m->xyz + 3 * (size_t)m->conn[m->npe * (size_t)e + k], sizeof X[k]);
            for (int d = 0; d < 3 && near; d++) {
                double lo = fmin(fmin(X[0][d], X[1][d]), fmin(X[2][d], X[3][d])), hi = fmax(fmax(X[0][d], X[1][d]), fmax(X[2][d], X[3][d]));
                if (p[d] < lo - 1e-12 || p[d] > hi + 1e-12) near = false;
            }
            if (near && in_tet(X, p, 1e-9)) {
                hit++;
                break;
            }
        }
    }
    return hit;
}

static void test_plate(void) {
    printf("== M3. thin plate with a hole, wall thickness = surface cell\n");
    for (int variant = 0; variant < 2; variant++) {
        Plate pl = {20e-3, 12e-3, 1e-3, 10e-3, 6e-3, 3e-3, variant ? 30 * M_PI / 180 : 0};
        double h = pl.t;
        /* bounding box of the (possibly rotated) plate, shifted so its faces do not fall on lattice planes */
        double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int c = 0; c < 8; c++) {
            double q[3] = {c & 1 ? pl.Lx : 0, c & 2 ? pl.Ly : 0, c & 4 ? pl.t : 0}, p[3];
            double co = cos(pl.rot), si = sin(pl.rot);
            p[0] = q[0], p[1] = co * q[1] - si * q[2], p[2] = si * q[1] + co * q[2];
            for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], p[k]), hi[k] = fmax(hi[k], p[k]);
        }
        for (int k = 0; k < 3; k++) lo[k] -= 0.37 * h;
        double V = pl.Lx * pl.Ly * pl.t - M_PI * pl.a * pl.a * pl.t;
        TetMesh m;
        char err[256];
        bool ok = make_mesh(plate_sdf, plate_project, &pl, lo, hi, h, 4 * h, 1, V, &m, err, sizeof err);
        CHECK(ok, "plate: %s", err);
        if (!ok) continue;
        report(variant ? "plate tilted 30 deg" : "plate axis-aligned", &m);
        int bad;
        CHECK(conforming(&m, &bad), "M6 conforming: %d non-manifold faces, %d open boundary edges", m.nonmanifold_faces, bad);
        CHECK(m.regions == 1, "one region (%d)", m.regions);
        /* the mid-surface, away from the rounded outline by half a cell */
        int np = 0;
        double *pts = malloc(3 * 4000 * sizeof(double));
        for (double x = 0.5 * h; x <= pl.Lx - 0.5 * h + 1e-12; x += 0.5 * h)
            for (double y = 0.5 * h; y <= pl.Ly - 0.5 * h + 1e-12; y += 0.5 * h) {
                if (hypot(x - pl.cx, y - pl.cy) < pl.a + 0.5 * h) continue;
                double q[3] = {x, y, 0.5 * pl.t}, co = cos(pl.rot), si = sin(pl.rot);
                double *p = pts + 3 * (size_t)np++;
                p[0] = q[0], p[1] = co * q[1] - si * q[2], p[2] = si * q[1] + co * q[2];
            }
        int hit = covered(&m, pts, np);
        CHECK(hit == np, "no lost wall: %d of %d mid-surface points inside the mesh", hit, np);
        NOTE("angles %.2f..%.2f (target 10..165; reported)", m.min_dihedral, m.max_dihedral);
        free(pts);
        tetmesh_free(&m);
    }
}

/* face-connected regions with their element counts and boxes (the --stl report) */
typedef struct {
    int n[3], e;
} FKey;
static int fkey_cmp(const void *a, const void *b) {
    const FKey *x = a, *y = b;
    for (int k = 0; k < 3; k++)
        if (x->n[k] != y->n[k]) return x->n[k] < y->n[k] ? -1 : 1;
    return 0;
}
static int ufind(int *p, int x) {
    while (p[x] != x) p[x] = p[p[x]], x = p[x];
    return x;
}
static void region_report(const TetMesh *m) {
    int ne = m->nelems;
    FKey *f = malloc(4 * (size_t)ne * sizeof *f);
    int *par = malloc((size_t)ne * sizeof(int));
    for (int e = 0; e < ne; e++) {
        par[e] = e;
        for (int k = 0; k < 4; k++) {
            FKey *q = &f[4 * (size_t)e + k];
            for (int j = 0; j < 3; j++) q->n[j] = m->conn[m->npe * (size_t)e + TETMESH_FACE[k][j]];
            for (int i = 1; i < 3; i++)
                for (int j = i; j > 0 && q->n[j] < q->n[j - 1]; j--) {
                    int t = q->n[j];
                    q->n[j] = q->n[j - 1], q->n[j - 1] = t;
                }
            q->e = e;
        }
    }
    qsort(f, 4 * (size_t)ne, sizeof *f, fkey_cmp);
    for (size_t i = 1; i < 4 * (size_t)ne; i++)
        if (!fkey_cmp(&f[i], &f[i - 1])) {
            int a = ufind(par, f[i].e), b = ufind(par, f[i - 1].e);
            if (a != b) par[a] = b;
        }
    for (int r = 0, shown = 0; r < ne && shown < 8; r++) {
        if (ufind(par, r) != r) continue;
        int cnt = 0;
        double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int e = 0; e < ne; e++)
            if (ufind(par, e) == r) {
                cnt++;
                for (int k = 0; k < 4; k++)
                    for (int d = 0; d < 3; d++) {
                        double x = m->xyz[3 * (size_t)m->conn[m->npe * (size_t)e + k] + d];
                        lo[d] = fmin(lo[d], x), hi[d] = fmax(hi[d], x);
                    }
            }
        printf("  region: %d elements, box %.1f..%.1f x %.1f..%.1f x %.1f..%.1f mm\n", cnt, 1e3 * lo[0], 1e3 * hi[0], 1e3 * lo[1], 1e3 * hi[1],
               1e3 * lo[2], 1e3 * hi[2]);
        shown++;
    }
    free(f), free(par);
}

/* ---- the owner's STL files (M4, M5) ------------------------------------------------------------------------------- */

static int g_thin = 0;
static int run_stl(const char *path, double size_mm, int order, double scale) {
    Mesh sm;
    char err[512];
    mesh_init(&sm);
    double t0 = now_seconds();
    if (!mesh_load_stl(path, &sm, err, sizeof err)) {
        printf("cannot read %s: %s\n", path, err);
        return 2;
    }
    int nt = (int)sm.tri_count;
    double *v = malloc(9 * (size_t)nt * sizeof(double));
    int *tri = malloc(3 * (size_t)nt * sizeof(int));
    for (int i = 0; i < 3 * nt; i++) {
        v[3 * (size_t)i] = sm.pos[i].x * scale, v[3 * (size_t)i + 1] = sm.pos[i].y * scale, v[3 * (size_t)i + 2] = sm.pos[i].z * scale;
        tri[i] = i;
    }
    mesh_free(&sm);
    TetStlGeometry g;
    if (!tet_stl_init(&g, v, tri, nt, err, sizeof err)) {
        printf("%s\n", err);
        return 2;
    }
    printf("%s: %d triangles, %.1f x %.1f x %.1f mm, STL volume %.1f mm^3, read in %.1f s\n", path, nt, 1e3 * (g.hi[0] - g.lo[0]),
           1e3 * (g.hi[1] - g.lo[1]), 1e3 * (g.hi[2] - g.lo[2]), 1e9 * g.volume, now_seconds() - t0);
    TetMeshSettings s = {0};
    s.surface_size = size_mm * 1e-3, s.order = order;
    memcpy(s.lo, g.lo, sizeof s.lo), memcpy(s.hi, g.hi, sizeof s.hi);
    s.sdf = tet_stl_sdf, s.project = tet_stl_project, s.cross = tet_stl_cross, s.ctx = &g, s.ref_volume = fabs(g.volume);
    s.pool = g_pool, s.thin_levels = g_thin;
    TetMesh m;
    bool ok = tetmesh_generate(&s, &m, err, sizeof err);
    if (!ok) printf("refused: %s\n", err);
    report("stl", &m);
    printf("  octree leaves %d (depth %d), lattice vertices %d, background tetrahedra %d, cut points %d, inside test split on %lld of %lld "
           "evaluations\n",
           m.octree_leaves, m.octree_depth, m.lattice_vertices, m.background_tets, m.cut_points, g.votes_split, g.evaluations);
    int bad = 0;
    if (ok) region_report(&m);
    if (ok) printf("  conforming: %s (%d non-manifold faces, %d open boundary edges)\n", conforming(&m, &bad) ? "yes" : "NO", m.nonmanifold_faces, bad);
    tetmesh_free(&m);
    tet_stl_free(&g);
    free(v), free(tri);
    return ok ? 0 : 1;
}

void tettest_elements(void);

int main(int argc, char **argv) {
    g_pool = pool_create(cpu_perf_count() > 0 ? cpu_perf_count() : 4);
    if (argc > 2 && !strcmp(argv[1], "--stl")) {
        double size = 2, scale = 1e-3;
        int order = 1;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "--size")) size = atof(argv[i + 1]);
            else if (!strcmp(argv[i], "--order")) order = atoi(argv[i + 1]);
            else if (!strcmp(argv[i], "--scale")) scale = atof(argv[i + 1]);
            else if (!strcmp(argv[i], "--thin")) g_thin = atoi(argv[i + 1]);
        }
        int r = run_stl(argv[2], size, order, scale);
        pool_destroy(g_pool);
        return r;
    }
    bool only_mesh = argc > 1 && !strcmp(argv[1], "--mesh");
    test_cube();
    test_sphere();
    test_plate();
    if (!only_mesh) tettest_elements();
    pool_destroy(g_pool);
    printf("\ntettest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ==== elements (T1-T6) ============================================================================================ */

typedef struct {
    int type, npe, nn, ne;
    double *xyz;
    int *conn;
} TMesh;

static void tmesh_free(TMesh *m) {
    free(m->xyz), free(m->conn);
    memset(m, 0, sizeof *m);
}

typedef struct {
    int a, b, e, k;
} EdgeRef;
static int edgeref_cmp(const void *x, const void *y) {
    const EdgeRef *p = x, *q = y;
    if (p->a != q->a) return p->a < q->a ? -1 : 1;
    if (p->b != q->b) return p->b < q->b ? -1 : 1;
    return 0;
}

/* corner tetrahedra (4 per element in c4, positively oriented) to a TMesh of the given type; mid-edge nodes at edge
 * midpoints */
static void tmesh_from_corners(int type, const double *xyz, int nn, const int *c4, int ne, TMesh *m) {
    m->type = type, m->npe = tet_nodes(type), m->ne = ne;
    int nmid = 0;
    EdgeRef *er = NULL;
    if (type == SOLID_ELEM_TET10) {
        er = malloc(6 * (size_t)ne * sizeof *er);
        for (int e = 0; e < ne; e++)
            for (int k = 0; k < 6; k++) {
                int a = c4[4 * e + TET_EDGE[k][0]], b = c4[4 * e + TET_EDGE[k][1]];
                er[6 * e + k] = (EdgeRef){a < b ? a : b, a < b ? b : a, e, k};
            }
        qsort(er, 6 * (size_t)ne, sizeof *er, edgeref_cmp);
        for (int i = 0; i < 6 * ne; i++)
            if (i == 0 || edgeref_cmp(&er[i], &er[i - 1])) nmid++;
    }
    m->nn = nn + nmid;
    m->xyz = malloc(3 * (size_t)m->nn * sizeof(double));
    m->conn = malloc((size_t)m->npe * ne * sizeof(int));
    memcpy(m->xyz, xyz, 3 * (size_t)nn * sizeof(double));
    for (int e = 0; e < ne; e++) memcpy(m->conn + (size_t)m->npe * e, c4 + 4 * e, 4 * sizeof(int));
    if (er) {
        int id = nn - 1;
        for (int i = 0; i < 6 * ne; i++) {
            if (i == 0 || edgeref_cmp(&er[i], &er[i - 1])) {
                id++;
                for (int d = 0; d < 3; d++) m->xyz[3 * (size_t)id + d] = 0.5 * (xyz[3 * (size_t)er[i].a + d] + xyz[3 * (size_t)er[i].b + d]);
            }
            m->conn[10 * (size_t)er[i].e + 4 + er[i].k] = id;
        }
        free(er);
    }
}

/* a box [0,Lx] x [0,Ly] x [0,Lz] of nx x ny x nz cubes, six Kuhn tetrahedra each (a conforming, translation-invariant
 * split along the cube diagonal) */
static void kuhn_box(int nx, int ny, int nz, double Lx, double Ly, double Lz, int type, TMesh *m) {
    int nn = (nx + 1) * (ny + 1) * (nz + 1), ne = 6 * nx * ny * nz;
    double *xyz = malloc(3 * (size_t)nn * sizeof(double));
    int *c4 = malloc(4 * (size_t)ne * sizeof(int));
#define NID(i, j, k) ((i) + (nx + 1) * ((j) + (ny + 1) * (k)))
    for (int k = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) {
                double *p = xyz + 3 * (size_t)NID(i, j, k);
                p[0] = Lx * i / nx, p[1] = Ly * j / ny, p[2] = Lz * k / nz;
            }
    static const int PERM[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    int e = 0;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++)
                for (int pp = 0; pp < 6; pp++) {
                    int c[3] = {i, j, k}, v[4];
                    v[0] = NID(c[0], c[1], c[2]);
                    for (int s2 = 0; s2 < 3; s2++) {
                        c[PERM[pp][s2]]++;
                        v[s2 + 1] = NID(c[0], c[1], c[2]);
                    }
                    double X[4][3];
                    for (int a = 0; a < 4; a++) memcpy(X[a], xyz + 3 * (size_t)v[a], sizeof X[a]);
                    if (tet_signed_volume(X) < 0) {
                        int t = v[2];
                        v[2] = v[3], v[3] = t;
                    }
                    memcpy(c4 + 4 * e++, v, sizeof v);
                }
#undef NID
    tmesh_from_corners(type, xyz, nn, c4, ne, m);
    free(xyz), free(c4);
}

static bool tsolve(const TMesh *m, double E, double nu, double rho, const unsigned char *fixed, const double *fval, const double *force, const double *eps0,
                   const double g[3], SolidSolver solver, SolidResult *r, char *err, size_t errlen) {
    SolidMaterial mat = {E, nu, rho};
    HexModel hm = {m->nn, m->ne, m->xyz, m->conn, NULL, 1, &mat, HEX8_FULL, NULL, .elem_type = m->type};
    SolidLoads L = {fixed, fval, force, eps0, {g ? g[0] : 0, g ? g[1] : 0, g ? g[2] : 0}};
    SolidOptions o = {solver, 1e-12, 100000, 0, g_pool, NULL, NULL};
    return solid_solve(&hm, &L, &o, r, err, errlen);
}

static double lcg(unsigned *s) {
    *s = *s * 1103515245u + 12345u;
    return ((*s >> 8) & 0xffff) / 65535.0 - 0.5;
}

static const char *TNAME(int type) { return type == SOLID_ELEM_TET10 ? "TET10" : "TET4"; }

static void test_t1_element(void) {
    printf("== T1. single distorted element: rigid-body modes and constant strain\n");
    unsigned seed = 777;
    double D[6][6];
    isotropic_D(210e9, 0.3, D);
    for (int type = SOLID_ELEM_TET4; type <= SOLID_ELEM_TET10; type++) {
        int n = tet_nodes(type), nd = 3 * n;
        double X[10][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int a = 0; a < 4; a++)
            for (int k = 0; k < 3; k++) X[a][k] += 0.15 * lcg(&seed);
        for (int e = 0; e < 6; e++)
            for (int k = 0; k < 3; k++) X[4 + e][k] = 0.5 * (X[TET_EDGE[e][0]][k] + X[TET_EDGE[e][1]][k]) + (type == SOLID_ELEM_TET10 ? 0.02 * lcg(&seed) : 0);
        double Ke[900], mdj, A[900], w[30];
        CHECK(tet_stiffness(type, (const double (*)[3])X, D, Ke, &mdj), "%s stiffness", TNAME(type));
        double sym = 0;
        for (int i = 0; i < nd; i++)
            for (int j = 0; j < nd; j++) sym = fmax(sym, fabs(Ke[i * nd + j] - Ke[j * nd + i]));
        CHECK(sym <= 1e-12 * fabs(Ke[0]), "%s symmetric (%g)", TNAME(type), sym);
        memcpy(A, Ke, sizeof(double) * nd * nd);
        dense_sym_eigen(A, nd, w, NULL);
        int zero = 0, neg = 0;
        for (int i = 0; i < nd; i++) {
            if (fabs(w[i]) <= 1e-9 * w[nd - 1]) zero++;
            else if (w[i] < 0) neg++;
        }
        CHECK(zero == 6 && neg == 0, "%s: %d zero eigenvalues (want 6), %d negative", TNAME(type), zero, neg);
        double G[3][3] = {{1e-3, 2e-4, -3e-4}, {5e-4, -2e-3, 1e-4}, {-1e-4, 3e-4, 1.5e-3}}, ue[30];
        for (int a = 0; a < n; a++)
            for (int i = 0; i < 3; i++) ue[3 * a + i] = G[i][0] * X[a][0] + G[i][1] * X[a][1] + G[i][2] * X[a][2] + 1e-3 * (i + 1);
        double exact[6] = {G[0][0], G[1][1], G[2][2], G[0][1] + G[1][0], G[1][2] + G[2][1], G[2][0] + G[0][2]};
        double eps[4][6], sig[4][6], maxe = 0;
        tet_gauss_strain_stress(type, (const double (*)[3])X, D, ue, NULL, eps, sig);
        for (int g = 0; g < tet_gauss_count(type); g++)
            for (int k = 0; k < 6; k++) maxe = fmax(maxe, fabs(eps[g][k] - exact[k]));
        CHECK(maxe < 1e-13, "%s reproduces constant strain (error %.2e)", TNAME(type), maxe);
        printf("  %s: 6 rigid-body modes, constant strain error %.1e, volume %.6f\n", TNAME(type), maxe, tet_volume(type, (const double (*)[3])X));
    }
}

static void test_t2_patch(void) {
    printf("== T2. patch test: distorted interior node, linear displacement on the boundary\n");
    for (int type = SOLID_ELEM_TET4; type <= SOLID_ELEM_TET10; type++) {
        TMesh c;
        kuhn_box(2, 2, 2, 1, 1, 1, SOLID_ELEM_TET4, &c);
        /* move the centre node, then build the mesh of this type from the moved corners (straight edges) */
        int centre = 1 + 3 * (1 + 3 * 1);
        c.xyz[3 * centre] += 0.07, c.xyz[3 * centre + 1] -= 0.05, c.xyz[3 * centre + 2] += 0.04;
        TMesh m;
        tmesh_from_corners(type, c.xyz, c.nn, c.conn, c.ne, &m);
        tmesh_free(&c);
        double G[3][3] = {{1e-3, 2e-4, -3e-4}, {5e-4, -2e-3, 1e-4}, {-1e-4, 3e-4, 1.5e-3}};
        unsigned char *fixed = calloc(3 * (size_t)m.nn, 1);
        double *fv = calloc(3 * (size_t)m.nn, sizeof(double));
        for (int n = 0; n < m.nn; n++) {
            const double *p = m.xyz + 3 * (size_t)n;
            bool b = false;
            for (int k = 0; k < 3; k++) b |= fabs(p[k]) < 1e-12 || fabs(p[k] - 1) < 1e-12;
            for (int i = 0; i < 3; i++) {
                fixed[3 * n + i] = b;
                fv[3 * n + i] = G[i][0] * p[0] + G[i][1] * p[1] + G[i][2] * p[2] + 1e-3 * (i + 1);
            }
        }
        SolidResult r;
        char err[256];
        bool ok = tsolve(&m, 210e9, 0.3, 7800, fixed, fv, NULL, NULL, NULL, SOLID_SOLVER_DIRECT, &r, err, sizeof err);
        CHECK(ok, "%s patch solve: %s", TNAME(type), err);
        if (ok) {
            double du = 0, ds = 0, D[6][6], sx[6];
            isotropic_D(210e9, 0.3, D);
            double ex[6] = {G[0][0], G[1][1], G[2][2], G[0][1] + G[1][0], G[1][2] + G[2][1], G[2][0] + G[0][2]};
            for (int i = 0; i < 6; i++) {
                sx[i] = 0;
                for (int k = 0; k < 6; k++) sx[i] += D[i][k] * ex[k];
            }
            for (int n = 0; n < m.nn; n++)
                for (int i = 0; i < 3; i++) du = fmax(du, fabs(r.u[3 * n + i] - fv[3 * n + i]));
            int ng = r.gp_per_elem;
            for (int e = 0; e < m.ne; e++)
                for (int g = 0; g < ng; g++)
                    for (int k = 0; k < 6; k++) ds = fmax(ds, fabs(r.gp_stress[6 * ((size_t)ng * e + g) + k] - sx[k]));
            CHECK(du < 1e-10 * 3e-3 && ds < 1e-10 * 210e9 * 3e-3, "%s: interior displacement error %.2e m, stress error %.2e Pa", TNAME(type), du, ds);
            printf("  %s: displacement error %.1e m, stress error %.1e Pa (of %.3g Pa)\n", TNAME(type), du, ds, 210e9 * 3e-3);
            solid_result_free(&r);
        }
        free(fixed), free(fv);
        tmesh_free(&m);
    }
}

static double cantilever_tip(int type, int nx, int ny, int nz, SolidSolver solver, double *sec) {
    const double L = 10, h = 1, w = 1, P = 1e-3, E = 1, nu = 0;
    TMesh m;
    kuhn_box(nx, ny, nz, L, w, h, type, &m);
    unsigned char *fixed = calloc(3 * (size_t)m.nn, 1);
    double *force = calloc(3 * (size_t)m.nn, sizeof(double));
    for (int n = 0; n < m.nn; n++)
        if (m.xyz[3 * n] < 1e-12) fixed[3 * n] = fixed[3 * n + 1] = fixed[3 * n + 2] = 1;
    /* shear traction on the end face x = L: every element face whose nodes all lie on it */
    for (int e = 0; e < m.ne; e++)
        for (int f = 0; f < 4; f++) {
            bool on = true;
            for (int q = 0; q < 3; q++) on &= fabs(m.xyz[3 * (size_t)m.conn[(size_t)m.npe * e + TET_FACE[f][q]]] - L) < 1e-12;
            if (!on) continue;
            double X[10][3], fe[30], t[3] = {0, 0, -P / (w * h)}, area, nrm[3];
            for (int a = 0; a < m.npe; a++) memcpy(X[a], m.xyz + 3 * (size_t)m.conn[(size_t)m.npe * e + a], sizeof X[a]);
            tet_face_load(type, (const double (*)[3])X, f, t, fe, &area, nrm);
            for (int a = 0; a < m.npe; a++)
                for (int k = 0; k < 3; k++) force[3 * (size_t)m.conn[(size_t)m.npe * e + a] + k] += fe[3 * a + k];
        }
    SolidResult r;
    char err[256];
    double tip = NAN;
    if (tsolve(&m, E, nu, 1, fixed, NULL, force, NULL, NULL, solver, &r, err, sizeof err)) {
        double s = 0;
        int cnt = 0;
        for (int n = 0; n < m.nn; n++)
            if (fabs(m.xyz[3 * n] - L) < 1e-12) s += r.u[3 * n + 2], cnt++;
        tip = -s / cnt;
        if (sec) *sec = r.stats.seconds;
        solid_result_free(&r);
    } else
        printf("  cantilever failed: %s\n", err);
    free(fixed), free(force);
    tmesh_free(&m);
    return tip;
}

static void test_t3_cantilever(void) {
    printf("== T3. cantilever, end shear load (L/h = 10, nu = 0), Kuhn tetrahedra\n");
    const double L = 10, h = 1, w = 1, P = 1e-3, E = 1;
    double I = w * h * h * h / 12, G = E / 2, A = w * h, kappa = 5.0 / 6.0;
    double timo = P * L * L * L / (3 * E * I) + P * L / (kappa * G * A);
    printf("  Timoshenko %.6g\n  %-10s %-14s %-10s %-14s %-10s\n", timo, "mesh", "TET4", "error", "TET10", "error");
    int meshes[4][3] = {{10, 1, 1}, {20, 2, 2}, {40, 4, 4}, {80, 8, 8}};
    double t4[4], t10[4];
    for (int i = 0; i < 4; i++) {
        t4[i] = i < 3 ? cantilever_tip(SOLID_ELEM_TET4, meshes[i][0], meshes[i][1], meshes[i][2], SOLID_SOLVER_AUTO, NULL) : NAN;
        t10[i] = i < 3 ? cantilever_tip(SOLID_ELEM_TET10, meshes[i][0], meshes[i][1], meshes[i][2], SOLID_SOLVER_AUTO, NULL) : NAN;
        if (i < 3)
            printf("  %dx%dx%-6d %-14.6g %+8.2f%%  %-14.6g %+8.2f%%\n", meshes[i][0], meshes[i][1], meshes[i][2], t4[i], 100 * (t4[i] - timo) / timo, t10[i],
                   100 * (t10[i] - timo) / timo);
    }
    CHECK(t4[0] < 0.8 * timo, "TET4 locks on the coarse mesh (%.1f%% of Timoshenko)", 100 * t4[0] / timo);
    CHECK(fabs(t10[2] - timo) / timo < 0.01, "TET10 within 1%% of Timoshenko at 40x4x4 cubes, 3840 elements (%.3f%%)", 100 * (t10[2] - timo) / timo);
    CHECK(fabs(t10[2] - t10[1]) < fabs(t10[1] - t10[0]), "TET10: successive differences shrink");
    double td, tp;
    double dd = cantilever_tip(SOLID_ELEM_TET10, 20, 2, 2, SOLID_SOLVER_DIRECT, &td), dp = cantilever_tip(SOLID_ELEM_TET10, 20, 2, 2, SOLID_SOLVER_PCG, &tp);
    CHECK(fabs(dd - dp) < 1e-8 * dd, "direct (%.3f s) and PCG (%.3f s) agree: %.10g vs %.10g", td, tp, dd, dp);
}

/* Kirsch: an infinite plate with a hole of radius a under remote tension s0 along x, plane stress */
typedef struct {
    double a, s0;
} Kirsch;

static void kirsch_stress(const Kirsch *k, double x, double y, double s[3]) {
    double r = hypot(x, y), th = atan2(y, x), a2 = k->a * k->a / (r * r), a4 = a2 * a2, c2 = cos(2 * th), s2 = sin(2 * th);
    double srr = 0.5 * k->s0 * (1 - a2) + 0.5 * k->s0 * (1 - 4 * a2 + 3 * a4) * c2;
    double stt = 0.5 * k->s0 * (1 + a2) - 0.5 * k->s0 * (1 + 3 * a4) * c2;
    double srt = -0.5 * k->s0 * (1 + 2 * a2 - 3 * a4) * s2;
    double c = cos(th), sn = sin(th);
    s[0] = srr * c * c + stt * sn * sn - 2 * srt * sn * c;
    s[1] = srr * sn * sn + stt * c * c + 2 * srt * sn * c;
    s[2] = (srr - stt) * sn * c + srt * (c * c - sn * sn);
}

static void kirsch_traction(void *ctx, const double x[3], const double n[3], double t[3]) {
    double s[3];
    kirsch_stress(ctx, x[0], x[1], s);
    t[0] = s[0] * n[0] + s[2] * n[1], t[1] = s[2] * n[0] + s[1] * n[1], t[2] = 0;
}

static void kirsch_run(int type, double nu, double hdiv, double *scf, int *nel, int *nnod, double *secs, double *mind) {
    const double a = 1, W = 4, t = 0.5, s0 = 1;
    Plate pl = {W, W, t, 0, 0, a, 0};
    double lo[3] = {0, 0, 0}, hi[3] = {W, W, t};
    TetMesh tm;
    char err[256];
    *scf = NAN;
    if (!make_mesh(plate_sdf, plate_project, &pl, lo, hi, a / hdiv, 4 * a / hdiv, type == SOLID_ELEM_TET10 ? 2 : 1, 0, &tm, err, sizeof err)) {
        printf("  Kirsch mesh: %s\n", err);
        return;
    }
    *nel = tm.nelems, *nnod = tm.nnodes, *mind = tm.min_dihedral;
    TMesh m = {type, tm.npe, tm.nnodes, tm.nelems, tm.xyz, tm.conn};
    unsigned char *fixed = calloc(3 * (size_t)m.nn, 1);
    double *force = calloc(3 * (size_t)m.nn, sizeof(double));
    Kirsch k = {a, s0};
    double tol = 1e-9 * W;
    for (int f = 0; f < tm.nfaces; f++) {
        int e = tm.face_elem[f], lf = tm.face_local[f], idx[6], nf = 3;
        for (int q = 0; q < 3; q++) idx[q] = TET_FACE[lf][q];
        if (type == SOLID_ELEM_TET10)
            for (int q = 0; q < 3; q++) idx[3 + q] = TET10_FACE_MID[lf][q], nf = 6;
        double c[3] = {0, 0, 0};
        for (int q = 0; q < 3; q++)
            for (int d = 0; d < 3; d++) c[d] += m.xyz[3 * (size_t)m.conn[(size_t)m.npe * e + idx[q]] + d] / 3;
        int fix_dir = -1;
        if (c[0] < tol) fix_dir = 0;
        else if (c[1] < tol) fix_dir = 1;
        else if (c[2] < tol) fix_dir = 2;
        if (fix_dir >= 0) {
            for (int q = 0; q < nf; q++) fixed[3 * (size_t)m.conn[(size_t)m.npe * e + idx[q]] + fix_dir] = 1;
            continue;
        }
        if (fabs(c[0] - W) < tol || fabs(c[1] - W) < tol) {
            double X[10][3], fe[30];
            for (int q = 0; q < m.npe; q++) memcpy(X[q], m.xyz + 3 * (size_t)m.conn[(size_t)m.npe * e + q], sizeof X[q]);
            tet_face_load_fn(type, (const double (*)[3])X, lf, kirsch_traction, &k, fe);
            for (int q = 0; q < m.npe; q++)
                for (int d = 0; d < 3; d++) force[3 * (size_t)m.conn[(size_t)m.npe * e + q] + d] += fe[3 * q + d];
        }
    }
    SolidResult r;
    double t0 = now_seconds();
    if (tsolve(&m, 1, nu, 1, fixed, NULL, force, NULL, NULL, SOLID_SOLVER_AUTO, &r, err, sizeof err)) {
        double s = 0;
        int cnt = 0;
        for (int n = 0; n < m.nn; n++) {
            const double *p = m.xyz + 3 * (size_t)n;
            if (p[0] < tol && fabs(hypot(p[0], p[1]) - a) < 1e-6 * a && p[2] > 0.2 * t && p[2] < 0.8 * t) s += r.node_stress[6 * n], cnt++;
        }
        *scf = cnt ? s / cnt / s0 : NAN;
        *secs = now_seconds() - t0;
        printf("  %-6s nu=%.1f h=a/%-3g %7d el %7d nodes  min dihedral %5.2f  Kt = %.4f  (%+.2f%%, %d nodes on the hole at x = 0)  equilibrium %.1e  %.1f s\n",
               TNAME(type), nu, hdiv, m.ne, m.nn, tm.min_dihedral, *scf, 100 * (*scf - 3) / 3, cnt, r.equilibrium_error, *secs);
        solid_result_free(&r);
    } else
        printf("  Kirsch solve: %s\n", err);
    free(fixed), free(force);
    tetmesh_free(&tm);
}

static void test_t4_kirsch(void) {
    printf("== T4. Kirsch: plate with a circular hole, exact tractions on the outer faces, quarter model\n");
    double scf, secs, mind;
    int ne, nn;
    kirsch_run(SOLID_ELEM_TET10, 0.0, 8, &scf, &ne, &nn, &secs, &mind);
    CHECK(fabs(scf - 3) / 3 < 0.03, "TET10, nu = 0: stress concentration %.4f within 3%% of 3.00", scf);
    kirsch_run(SOLID_ELEM_TET4, 0.0, 8, &scf, &ne, &nn, &secs, &mind);
    NOTE("TET4 on the same mesh: %.4f (reported)", scf);
    kirsch_run(SOLID_ELEM_TET10, 0.3, 8, &scf, &ne, &nn, &secs, &mind);
    NOTE("TET10, nu = 0.3: %.4f (reported; the three-dimensional plate is not the plane-stress solution then)", scf);
}

static void test_t6_balance(void) {
    printf("== T6. equilibrium, energy, gravity, eigenstrain, constraints\n");
    for (int type = SOLID_ELEM_TET4; type <= SOLID_ELEM_TET10; type++) {
        const double Lx = 2, Ly = 1, Lz = 1, E = 200e9, nu = 0.3, sigma = 50e6;
        TMesh m;
        kuhn_box(4, 2, 2, Lx, Ly, Lz, type, &m);
        unsigned char *fixed = calloc(3 * (size_t)m.nn, 1);
        double *force = calloc(3 * (size_t)m.nn, sizeof(double));
        for (int n = 0; n < m.nn; n++)
            for (int k = 0; k < 3; k++)
                if (fabs(m.xyz[3 * n + k]) < 1e-12) fixed[3 * n + k] = 1; /* rollers on the three coordinate planes */
        for (int e = 0; e < m.ne; e++)
            for (int f = 0; f < 4; f++) {
                bool on = true;
                for (int q = 0; q < 3; q++) on &= fabs(m.xyz[3 * (size_t)m.conn[(size_t)m.npe * e + TET_FACE[f][q]]] - Lx) < 1e-12;
                if (!on) continue;
                double X[10][3], fe[30], t[3] = {sigma, 0, 0}, area, nrm[3];
                for (int a = 0; a < m.npe; a++) memcpy(X[a], m.xyz + 3 * (size_t)m.conn[(size_t)m.npe * e + a], sizeof X[a]);
                tet_face_load(type, (const double (*)[3])X, f, t, fe, &area, nrm);
                for (int a = 0; a < m.npe; a++)
                    for (int k = 0; k < 3; k++) force[3 * (size_t)m.conn[(size_t)m.npe * e + a] + k] += fe[3 * a + k];
            }
        SolidResult r;
        char err[256];
        bool ok = tsolve(&m, E, nu, 7800, fixed, NULL, force, NULL, NULL, SOLID_SOLVER_AUTO, &r, err, sizeof err);
        CHECK(ok, "%s tension: %s", TNAME(type), err);
        if (ok) {
            double eu = 0, es = 0;
            for (int n = 0; n < m.nn; n++) {
                const double *p = m.xyz + 3 * (size_t)n;
                double ex[3] = {sigma / E * p[0], -nu * sigma / E * p[1], -nu * sigma / E * p[2]};
                for (int k = 0; k < 3; k++) eu = fmax(eu, fabs(r.u[3 * n + k] - ex[k]));
                es = fmax(es, fabs(r.node_stress[6 * n] - sigma));
            }
            CHECK(eu < 1e-9 * sigma / E * Lx && es < 1e-8 * sigma, "%s tension exact: displacement error %.1e m, stress error %.1e Pa", TNAME(type), eu, es);
            CHECK(fabs(r.reaction_total[0] + sigma * Ly * Lz) < 1e-8 * sigma * Ly * Lz, "%s reaction %.6g N balances %.6g N", TNAME(type), r.reaction_total[0],
                  sigma * Ly * Lz);
            CHECK(r.equilibrium_error < 1e-9, "%s equilibrium error %.1e", TNAME(type), r.equilibrium_error);
            CHECK(fabs(r.external_work - 2 * r.strain_energy) < 1e-8 * r.external_work, "%s work %.6g J = 2 x strain energy %.6g J", TNAME(type),
                  r.external_work, r.strain_energy);
            printf("  %s tension: displacement error %.1e m, equilibrium %.1e, work - 2U = %.1e J\n", TNAME(type), eu, r.equilibrium_error,
                   r.external_work - 2 * r.strain_energy);
            solid_result_free(&r);
        }
        /* gravity: the base reaction is the weight */
        memset(fixed, 0, 3 * (size_t)m.nn);
        for (int n = 0; n < m.nn; n++)
            if (fabs(m.xyz[3 * n + 2]) < 1e-12) fixed[3 * n] = fixed[3 * n + 1] = fixed[3 * n + 2] = 1;
        double gv[3] = {0, 0, -9.81}, weight = 7800 * 9.81 * Lx * Ly * Lz;
        ok = tsolve(&m, E, nu, 7800, fixed, NULL, NULL, NULL, gv, SOLID_SOLVER_AUTO, &r, err, sizeof err);
        CHECK(ok && fabs(r.reaction_total[2] - weight) < 1e-9 * weight, "%s base reaction %.9g N = weight %.9g N", TNAME(type), ok ? r.reaction_total[2] : 0,
              weight);
        if (ok) solid_result_free(&r);
        /* eigenstrain: free expansion is stress-free, full restraint gives the hydrostatic stress */
        double e0 = 1e-3, *eps0 = calloc(6 * (size_t)m.ne, sizeof(double));
        for (int e = 0; e < m.ne; e++) eps0[6 * e] = eps0[6 * e + 1] = eps0[6 * e + 2] = e0;
        memset(fixed, 0, 3 * (size_t)m.nn);
        for (int n = 0; n < m.nn; n++) {
            const double *p = m.xyz + 3 * (size_t)n;
            if (fabs(p[0]) + fabs(p[1]) + fabs(p[2]) < 1e-12) fixed[3 * n] = fixed[3 * n + 1] = fixed[3 * n + 2] = 1;
            if (fabs(p[0] - Lx) + fabs(p[1]) + fabs(p[2]) < 1e-12) fixed[3 * n + 1] = fixed[3 * n + 2] = 1;
            if (fabs(p[0]) + fabs(p[1] - Ly) + fabs(p[2]) < 1e-12) fixed[3 * n + 2] = 1;
        }
        ok = tsolve(&m, E, nu, 7800, fixed, NULL, NULL, eps0, NULL, SOLID_SOLVER_AUTO, &r, err, sizeof err);
        CHECK(ok, "%s free expansion: %s", TNAME(type), err);
        if (ok) {
            double smax = 0;
            for (int i = 0; i < 6 * m.ne * r.gp_per_elem; i++) smax = fmax(smax, fabs(r.gp_stress[i]));
            CHECK(smax < 1e-10 * E * e0, "%s free expansion stress-free (%.1e Pa)", TNAME(type), smax);
            solid_result_free(&r);
        }
        for (int n = 0; n < m.nn; n++) {
            const double *p = m.xyz + 3 * (size_t)n;
            bool b = fabs(p[0]) < 1e-12 || fabs(p[0] - Lx) < 1e-12 || fabs(p[1]) < 1e-12 || fabs(p[1] - Ly) < 1e-12 || fabs(p[2]) < 1e-12 ||
                     fabs(p[2] - Lz) < 1e-12;
            for (int k = 0; k < 3; k++) fixed[3 * n + k] = b;
        }
        ok = tsolve(&m, E, nu, 7800, fixed, NULL, NULL, eps0, NULL, SOLID_SOLVER_AUTO, &r, err, sizeof err);
        if (ok) {
            double K = E / (3 * (1 - 2 * nu)), expect = -3 * K * e0, es = 0, U = 4.5 * K * e0 * e0 * Lx * Ly * Lz;
            for (int i = 0; i < m.ne * r.gp_per_elem; i++)
                for (int k = 0; k < 3; k++) es = fmax(es, fabs(r.gp_stress[6 * i + k] - expect));
            CHECK(es < 1e-8 * fabs(expect), "%s restrained: hydrostatic %.6g Pa (error %.1e)", TNAME(type), expect, es);
            CHECK(fabs(r.strain_energy - U) < 1e-8 * U, "%s stored energy %.9g J = %.9g J", TNAME(type), r.strain_energy, U);
            solid_result_free(&r);
        }
        /* constraint analysis before the solve */
        ConstraintReport rep;
        SolidMaterial mat = {E, nu, 7800};
        HexModel hm = {m.nn, m.ne, m.xyz, m.conn, NULL, 1, &mat, HEX8_FULL, NULL, .elem_type = type};
        memset(fixed, 0, 3 * (size_t)m.nn);
        bool fine = solid_check_constraints(&hm, fixed, &rep);
        CHECK(!fine && rep.nissues == 1 && rep.issue[0].free_modes == 6, "%s unsupported: 6 free modes (%d)", TNAME(type), rep.nissues ? rep.issue[0].free_modes : -1);
        for (int n = 0; n < m.nn; n++)
            if (fabs(m.xyz[3 * n + 2]) < 1e-12) fixed[3 * n + 2] = 1;
        fine = solid_check_constraints(&hm, fixed, &rep);
        CHECK(!fine && rep.nissues == 1 && rep.issue[0].free_modes == 3, "%s base on rollers: 3 free modes (%d)", TNAME(type), rep.nissues ? rep.issue[0].free_modes : -1);
        free(eps0), free(fixed), free(force);
        tmesh_free(&m);
    }
}

/* T5: a stepped flat bar with shoulder fillets in tension (half model about y = 0, extruded along z).
 * Reference: the curve fit for "shoulder fillets in a flat bar, tension" as reproduced by
 * https://amesweb.info/stress-concentration-factor-calculator/shoulder-fillets-in-flat-bar.aspx (opened 2026-09-19),
 * which cites W. D. Pilkey, Formulas for Stress, Strain, and Structural Matrices, 2nd ed., Wiley 2005:
 *   Kt = C1 + C2 (2h/D) + C3 (2h/D)^2 + C4 (2h/D)^3, h = (D - d)/2, sigma_nom = P / (t d); for 0.1 <= h/r <= 2.0
 *   C1 = 1.006 + 1.008 sqrt(h/r) - 0.044 h/r, C2 = -0.115 - 0.584 sqrt(h/r) + 0.315 h/r,
 *   C3 = 0.245 - 1.006 sqrt(h/r) - 0.257 h/r, C4 = -0.135 + 0.582 sqrt(h/r) - 0.017 h/r.
 * The Peterson chart this fit belongs to could not be opened (docs/contracts/tet-mesh.md, amendment 2026-09-19). */
typedef struct {
    double d, D, r, Ln, Lw, t;
    int n;
    double (*P)[2]; /* the half profile, counter-clockwise, arc discretised */
} Step;

static double seg_dist(const double *a, const double *b, double x, double y) {
    double ex = b[0] - a[0], ey = b[1] - a[1], l2 = ex * ex + ey * ey, s = l2 > 0 ? ((x - a[0]) * ex + (y - a[1]) * ey) / l2 : 0;
    s = s < 0 ? 0 : (s > 1 ? 1 : s);
    return hypot(x - a[0] - s * ex, y - a[1] - s * ey);
}

static double step_sdf2(const Step *st, double x, double y) {
    double dm = INFINITY;
    bool in = false;
    for (int i = 0, j = st->n - 1; i < st->n; j = i++) {
        const double *a = st->P[i], *b = st->P[j];
        dm = fmin(dm, seg_dist(a, b, x, y));
        if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
    }
    return in ? -dm : dm;
}

static double step_sdf(void *ctx, const double p[3]) {
    const Step *st = ctx;
    double qx = step_sdf2(st, p[0], p[1]), qy = fabs(p[2] - 0.5 * st->t) - 0.5 * st->t;
    return hypot(fmax(qx, 0), fmax(qy, 0)) + fmin(fmax(qx, qy), 0);
}

static void step_project(void *ctx, const double p[3], double q[3]) { grad_project(step_sdf, ctx, p, q, 1e-7); }

static void test_t5_fillet(void) {
    printf("== T5. stepped flat bar with shoulder fillets in tension, against a published curve fit\n");
    Step st = {1.0, 1.5, 0.25, 2.0, 2.25, 0.2, 0, NULL};
    double hh = 0.5 * (st.D - st.d), hr = hh / st.r, sq = sqrt(hr), x2 = 2 * hh / st.D;
    double C1 = 1.006 + 1.008 * sq - 0.044 * hr, C2 = -0.115 - 0.584 * sq + 0.315 * hr, C3 = 0.245 - 1.006 * sq - 0.257 * hr,
           C4 = -0.135 + 0.582 * sq - 0.017 * hr;
    double kref = C1 + C2 * x2 + C3 * x2 * x2 + C4 * x2 * x2 * x2;
    printf("  D/d = %.2f, r/d = %.2f, h/r = %.2f: Kt (curve fit) = %.4f\n", st.D / st.d, st.r / st.d, hr, kref);
    int narc = 256;
    st.P = malloc((size_t)(6 + narc) * sizeof *st.P);
    double pts[5][2] = {{-st.Ln, 0}, {st.Lw, 0}, {st.Lw, st.D / 2}, {0, st.D / 2}, {0, st.d / 2 + st.r}};
    for (int i = 0; i < 5; i++) st.P[st.n][0] = pts[i][0], st.P[st.n][1] = pts[i][1], st.n++;
    for (int i = 1; i <= narc; i++) {
        double th = -0.5 * M_PI * i / narc; /* from (0, d/2 + r) round to (-r, d/2), material outside the circle */
        st.P[st.n][0] = -st.r + st.r * cos(th), st.P[st.n][1] = st.d / 2 + st.r + st.r * sin(th), st.n++;
    }
    st.P[st.n][0] = -st.Ln, st.P[st.n][1] = st.d / 2, st.n++;
    double lo[3] = {-st.Ln, 0, 0}, hi[3] = {st.Lw, st.D / 2, st.t};
    TetMesh tm;
    char err[256];
    double h = st.r / 5;
    if (!make_mesh(step_sdf, step_project, &st, lo, hi, h, 4 * h, 2, 0, &tm, err, sizeof err)) {
        CHECK(0, "fillet mesh: %s", err);
        free(st.P);
        return;
    }
    TMesh m = {SOLID_ELEM_TET10, 10, tm.nnodes, tm.nelems, tm.xyz, tm.conn};
    unsigned char *fixed = calloc(3 * (size_t)m.nn, 1);
    double *force = calloc(3 * (size_t)m.nn, sizeof(double)), s_end = 1.0, tol = 1e-9;
    for (int f = 0; f < tm.nfaces; f++) {
        int e = tm.face_elem[f], lf = tm.face_local[f], idx[6];
        for (int q = 0; q < 3; q++) idx[q] = TET_FACE[lf][q], idx[3 + q] = TET10_FACE_MID[lf][q];
        double c[3] = {0, 0, 0};
        for (int q = 0; q < 3; q++)
            for (int k = 0; k < 3; k++) c[k] += m.xyz[3 * (size_t)m.conn[10 * (size_t)e + idx[q]] + k] / 3;
        int dir = fabs(c[0] + st.Ln) < tol ? 0 : (c[1] < tol ? 1 : (c[2] < tol ? 2 : -1));
        if (dir >= 0) {
            for (int q = 0; q < 6; q++) fixed[3 * (size_t)m.conn[10 * (size_t)e + idx[q]] + dir] = 1;
        } else if (fabs(c[0] - st.Lw) < tol) {
            double X[10][3], fe[30], tt[3] = {s_end, 0, 0}, area, nrm[3];
            for (int q = 0; q < 10; q++) memcpy(X[q], m.xyz + 3 * (size_t)m.conn[10 * (size_t)e + q], sizeof X[q]);
            tet_face_load(SOLID_ELEM_TET10, (const double (*)[3])X, lf, tt, fe, &area, nrm);
            for (int q = 0; q < 10; q++)
                for (int k = 0; k < 3; k++) force[3 * (size_t)m.conn[10 * (size_t)e + q] + k] += fe[3 * q + k];
        }
    }
    SolidResult r;
    double t0 = now_seconds();
    if (tsolve(&m, 1, 0, 1, fixed, NULL, force, NULL, NULL, SOLID_SOLVER_AUTO, &r, err, sizeof err)) {
        /* the largest sigma_xx over nodes on the fillet surface (between its two tangent points), mid-thickness */
        double smax = 0, snom = s_end * st.D / st.d;
        for (int n = 0; n < m.nn; n++) {
            const double *p = m.xyz + 3 * (size_t)n;
            double rr = hypot(p[0] + st.r, p[1] - st.d / 2 - st.r);
            if (p[0] > -st.r - 1e-9 && p[0] < 1e-9 && p[1] < st.d / 2 + st.r && fabs(rr - st.r) < 1e-3 * st.r && p[2] > 0.25 * st.t && p[2] < 0.75 * st.t)
                smax = fmax(smax, r.node_stress[6 * n]);
        }
        double kt = smax / snom;
        printf("  TET10 h = r/5: %d el %d nodes, min dihedral %.2f, Kt = %.4f (%+.2f%% against %.4f), equilibrium %.1e, %.1f s\n", m.ne, m.nn, tm.min_dihedral,
               kt, 100 * (kt - kref) / kref, kref, r.equilibrium_error, now_seconds() - t0);
        CHECK(fabs(kt - kref) / kref < 0.05, "fillet Kt %.4f within 5%% of the curve fit %.4f", kt, kref);
        solid_result_free(&r);
    } else
        CHECK(0, "fillet solve: %s", err);
    free(fixed), free(force), free(st.P);
    tetmesh_free(&tm);
}

void tettest_elements(void) {
    test_t1_element();
    test_t2_patch();
    test_t3_cantilever();
    test_t4_kirsch();
    test_t5_fillet();
    test_t6_balance();
}
