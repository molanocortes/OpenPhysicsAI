/* topotest.c - verification of the topology optimiser (docs/contracts/topology-optimisation.md)
 *
 *  T1 the half MBB beam of Sigmund 2001 on 60 x 20 elements, compared with the compliance published by
 *     Andreassen et al. 2011 for the same problem with sensitivity filtering (c = 216.81), criteria 1 and 2;
 *  T2 the short cantilever of Sigmund 2001 on 32 x 20 elements, criterion 3 (no compliance is published: the
 *     criteria are the drop against the grey design, a settled history and the shape);
 *  T3 three dimensions: a cantilever box at 30 % volume against the same volume left as a box with a centred hole,
 *     criterion 4.
 *  Both two-dimensional problems are built as one layer of unit cubes with every out-of-plane displacement held and
 *  the plane-strain material equivalent to plane stress (nu^ = nu / (1 + nu), E^ = E (1 - nu^^2)), fully
 *  integrated, which under that constraint is the four-node plane element of the published codes.
 *   make build/topotest && ./build/topotest */
#include "../src/fem/solid.h"
#include "../src/fem/topopt.h"
#include "../src/threads.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- a box of unit cubes ---------------------------------------------------------------------------------------- */

typedef struct Grid {
    int nx, ny, nz;
    int nnodes, nelems;
    double *xyz;
    int *conn;
} Grid;

static int gnode(const Grid *g, int i, int j, int k) { return (k * (g->ny + 1) + j) * (g->nx + 1) + i; }
static int gelem(const Grid *g, int i, int j, int k) { return (k * g->ny + j) * g->nx + i; }

static void grid_make(Grid *g, int nx, int ny, int nz) {
    g->nx = nx;
    g->ny = ny;
    g->nz = nz;
    g->nnodes = (nx + 1) * (ny + 1) * (nz + 1);
    g->nelems = nx * ny * nz;
    g->xyz = malloc(sizeof(double) * 3 * (size_t)g->nnodes);
    g->conn = malloc(sizeof(int) * 8 * (size_t)g->nelems);
    for (int k = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) {
                int n = gnode(g, i, j, k);
                g->xyz[3 * n + 0] = i;
                g->xyz[3 * n + 1] = j;
                g->xyz[3 * n + 2] = k;
            }
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                int *c = g->conn + 8 * (size_t)gelem(g, i, j, k);
                c[0] = gnode(g, i, j, k);
                c[1] = gnode(g, i + 1, j, k);
                c[2] = gnode(g, i + 1, j + 1, k);
                c[3] = gnode(g, i, j + 1, k);
                c[4] = gnode(g, i, j, k + 1);
                c[5] = gnode(g, i + 1, j, k + 1);
                c[6] = gnode(g, i + 1, j + 1, k + 1);
                c[7] = gnode(g, i, j + 1, k + 1);
            }
}

static void grid_free(Grid *g) {
    free(g->xyz);
    free(g->conn);
    memset(g, 0, sizeof *g);
}

/* the picture in the log, so the layout can be held against the published figure */
static void print_density(const Grid *g, const double *x, const char *title) {
    static const char *ramp = " .:-=+*#%@";
    printf("  %s (%d x %d, one character per element, @ solid, space void)\n", title, g->nx, g->ny);
    for (int j = g->ny - 1; j >= 0; j--) {
        printf("  |");
        for (int i = 0; i < g->nx; i++) {
            double d = x[gelem(g, i, j, 0)];
            int t = (int)(d * 9.999);
            if (t < 0) t = 0;
            if (t > 9) t = 9;
            putchar(ramp[t]);
        }
        printf("|\n");
    }
}

static void write_pgm(const Grid *g, const double *x, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P2\n%d %d\n255\n", g->nx, g->ny);
    for (int j = g->ny - 1; j >= 0; j--) {
        for (int i = 0; i < g->nx; i++) {
            double d = x[gelem(g, i, j, 0)];
            int v = (int)((1.0 - d) * 255.0 + 0.5);
            fprintf(f, "%d ", v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        fputc('\n', f);
    }
    fclose(f);
}

/* Plane stress (E, nu) as the equivalent plane-strain material of a layer whose out-of-plane motion is held.
 * A held layer is plane strain, so the pair (E^, nu^) is the one that makes the plane-strain matrix equal to the
 * plane-stress matrix of (E, nu): nu^ = nu / (1 + nu), E^ = E (1 - nu^^2). With E = 1 and nu = 0.3 that is
 * E^ = 0.9467456, nu^ = 0.2307692, and the two 3x3 matrices then agree to the last digit. */
static SolidMaterial plane_stress_as_plane_strain(double E, double nu) {
    double nh = nu / (1.0 + nu);
    SolidMaterial m = {E * (1.0 - nh * nh), nh, 0.0};
    return m;
}

typedef struct Case {
    Grid g;
    SolidMaterial mat;
    HexModel model;
    SolidLoads loads;
    unsigned char *fixed;
    double *force;
} Case;

static void case_make(Case *c, int nx, int ny, int nz, double E, double nu, bool plane) {
    memset(c, 0, sizeof *c);
    grid_make(&c->g, nx, ny, nz);
    c->mat = plane ? plane_stress_as_plane_strain(E, nu) : (SolidMaterial){E, nu, 0.0};
    c->fixed = calloc(3 * (size_t)c->g.nnodes, 1);
    c->force = calloc(3 * (size_t)c->g.nnodes, sizeof(double));
    c->model.nnodes = c->g.nnodes;
    c->model.nelems = c->g.nelems;
    c->model.xyz = c->g.xyz;
    c->model.conn = c->g.conn;
    c->model.nmat = 1;
    c->model.mat = &c->mat;
    c->model.formulation = HEX8_FULL; /* the published codes use the fully integrated bilinear element */
    c->model.elem_type = SOLID_ELEM_HEX8;
    c->loads.fixed = c->fixed;
    c->loads.nodal_force = c->force;
    if (plane) /* a plane problem: no out-of-plane motion anywhere */
        for (int n = 0; n < c->g.nnodes; n++) c->fixed[3 * (size_t)n + 2] = 1;
}

static void case_free(Case *c) {
    grid_free(&c->g);
    free(c->fixed);
    free(c->force);
}

static void opts(SolidOptions *o) {
    memset(o, 0, sizeof *o);
    o->solver = SOLID_SOLVER_AUTO;
    o->pcg_tol = 1e-10;
    o->pcg_max_iter = 20000;
    o->pool = g_pool;
}

/* ---- T1: the half MBB beam --------------------------------------------------------------------------------------- */

static void mbb_beam(void) {
    printf("== T1 the half MBB beam of Sigmund 2001 on 60 x 20, against the published compliance\n");
    const int nx = 60, ny = 20;
    Case c;
    case_make(&c, nx, ny, 1, 1.0, 0.3, true);
    /* symmetry along the left edge, a vertical roller under the lower right corner, unit load down at the top left */
    for (int j = 0; j <= ny; j++)
        for (int k = 0; k <= 1; k++) c.fixed[3 * (size_t)gnode(&c.g, 0, j, k) + 0] = 1;
    for (int k = 0; k <= 1; k++) c.fixed[3 * (size_t)gnode(&c.g, nx, 0, k) + 1] = 1;
    for (int k = 0; k <= 1; k++) c.force[3 * (size_t)gnode(&c.g, 0, ny, k) + 1] = -0.5; /* 1 N over the two layers */

    SolidOptions o;
    opts(&o);
    TopOptSettings s;
    topopt_settings_default(&s);
    s.volume_fraction = 0.5;
    s.penalty = 3.0;
    s.filter_radius = 2.4; /* 0.04 times the width of the domain, as in Andreassen et al. 2011 section 3.4 */
    s.passive_layers = 0;  /* the published problem has no passive region */
    s.x_min = 0.0;
    s.max_iter = 200;
    TopOptResult r;
    char err[256] = {0};
    double t0 = now_seconds();
    bool ok = topopt_run(&c.model, &c.loads, &o, &s, &r, err, sizeof err);
    CHECK(ok, "the MBB beam optimised: %s", err);
    if (!ok) { case_free(&c); return; }
    double t = now_seconds() - t0;

    const double published = 216.81; /* Andreassen et al. 2011, figure 3, sensitivity filtering, 60 x 20 */
    double rel = 100.0 * (r.compliance - published) / published;
    NOTE("%d iterations, %d solves, %.1f s (%.2f s per solve); stopped: %s", r.iterations, r.solves, t, t / r.solves,
         r.stop_reason);
    NOTE("compliance %.2f J against the published %.2f (%+.2f %%); volume fraction %.4f; f^T u against 2 U differ by %.1e",
         r.compliance, published, rel, r.volume_fraction, r.energy_mismatch);
    NOTE("compliance of the uniform grey start: %.2f J", r.compliance_initial);
    /* criterion 1: within 3 percent of the published compliance */
    CHECK(fabs(rel) <= 3.0, "the MBB compliance is within 3 %% of the published 216.81 (got %.2f, %+.2f %%)", r.compliance,
          rel);
    CHECK(r.energy_mismatch < 1e-6, "f^T u and twice the strain energy agree (%.2e)", r.energy_mismatch);
    CHECK(fabs(r.volume_fraction - 0.5) < 1e-3, "the volume constraint is met (%.4f)", r.volume_fraction);

    /* criterion 2: the features of the published figure that can be measured */
    print_density(&c.g, r.density, "MBB beam density");
    write_pgm(&c.g, r.density, "build/topopt-mbb.pgm");
    /* The chord that runs the whole span is the one away from the load: the load sits on the top left corner and the
     * roller under the bottom right one, and the material between them closes along the bottom. Amendment 1 of the
     * contract records that the first wording of this criterion named the wrong edge. Both are measured. */
    double top = 0, bottom = 0;
    for (int i = 0; i < nx; i++) {
        top += r.density[gelem(&c.g, i, ny - 1, 0)];
        bottom += r.density[gelem(&c.g, i, 0, 0)];
    }
    top /= nx;
    bottom /= nx;
    int void_cells = 0, interior = 0;
    for (int j = 1; j < ny - 1; j++)
        for (int i = 1; i < nx - 1; i++) {
            interior++;
            if (r.density[gelem(&c.g, i, j, 0)] < 0.1) void_cells++;
        }
    int checker = 0;
    for (int j = 1; j < ny - 1; j++)
        for (int i = 1; i < nx - 1; i++) {
            if (r.density[gelem(&c.g, i, j, 0)] >= 0.3) continue;
            if (r.density[gelem(&c.g, i - 1, j, 0)] > 0.7 && r.density[gelem(&c.g, i + 1, j, 0)] > 0.7 &&
                r.density[gelem(&c.g, i, j - 1, 0)] > 0.7 && r.density[gelem(&c.g, i, j + 1, 0)] > 0.7)
                checker++;
        }
    double under_load = r.density[gelem(&c.g, 0, ny - 1, 0)];
    NOTE("chord mean density: bottom %.3f, top %.3f; %d of %d interior elements below 0.1 (%.1f %%); %d checkerboard "
         "cells; the element under the load is at %.3f",
         bottom, top, void_cells, interior, 100.0 * void_cells / interior, checker, under_load);
    CHECK(bottom > 0.9, "the edge away from the load is a solid chord over the whole span (mean density %.3f)", bottom);
    CHECK(top > 0.5, "the loaded edge carries material over most of the span (mean density %.3f)", top);
    CHECK(void_cells > interior / 12, "the interior is opened up (%d of %d elements below 0.1)", void_cells, interior);
    CHECK(checker == 0, "no checkerboard cells (%d)", checker);
    CHECK(under_load > 0.5, "material stays under the load (%.3f)", under_load);
    CHECK(t < 120.0, "the published grid runs in well under two minutes (%.1f s)", t);
    topopt_result_free(&r);
    case_free(&c);
}

/* ---- T2: the short cantilever ------------------------------------------------------------------------------------ */

static void short_cantilever(void) {
    printf("== T2 the short cantilever of Sigmund 2001 on 32 x 20, top(32, 20, 0.4, 3.0, 1.2)\n");
    const int nx = 32, ny = 20;
    Case c;
    case_make(&c, nx, ny, 1, 1.0, 0.3, true);
    for (int j = 0; j <= ny; j++) /* the left edge fully fixed */
        for (int k = 0; k <= 1; k++) {
            c.fixed[3 * (size_t)gnode(&c.g, 0, j, k) + 0] = 1;
            c.fixed[3 * (size_t)gnode(&c.g, 0, j, k) + 1] = 1;
        }
    for (int k = 0; k <= 1; k++) c.force[3 * (size_t)gnode(&c.g, nx, 0, k) + 1] = -0.5; /* unit load at the lower right corner */

    SolidOptions o;
    opts(&o);
    TopOptSettings s;
    topopt_settings_default(&s);
    s.volume_fraction = 0.4;
    s.penalty = 3.0;
    s.filter_radius = 1.2;
    s.passive_layers = 0;
    s.x_min = 0.0;
    s.max_iter = 200;
    TopOptResult r;
    char err[256] = {0};
    double t0 = now_seconds();
    bool ok = topopt_run(&c.model, &c.loads, &o, &s, &r, err, sizeof err);
    CHECK(ok, "the short cantilever optimised: %s", err);
    if (!ok) { case_free(&c); return; }
    double t = now_seconds() - t0;
    double drop = 100.0 * (1.0 - r.compliance / r.compliance_initial);
    NOTE("%d iterations, %.1f s; compliance %.3f J from the grey start of %.3f J (%.1f %% lower); volume fraction %.4f",
         r.iterations, t, r.compliance, r.compliance_initial, drop, r.volume_fraction);
    NOTE("no compliance is published for this problem: the criteria are the drop, a settled history and the shape");
    print_density(&c.g, r.density, "short cantilever density");
    write_pgm(&c.g, r.density, "build/topopt-cantilever.pgm");

    /* (a) the drop against the uniform grey design of the same volume */
    CHECK(drop > 40.0, "the optimised design is far stiffer than the grey start (%.1f %% lower compliance)", drop);
    /* (b) the history settles: after the first five iterations the compliance stays within a band that shrinks */
    double worst_rise = 0;
    for (int i = 6; i < r.iterations; i++) {
        double rise = (r.history[i].compliance - r.history[i - 1].compliance) / r.history[i - 1].compliance;
        if (rise > worst_rise) worst_rise = rise;
    }
    NOTE("largest rise in compliance after iteration 5: %.2f %%", 100 * worst_rise);
    CHECK(worst_rise < 0.05, "the history settles after the first iterations (largest rise %.2f %%)", 100 * worst_rise);
    /* (c) the published shape: solid at the top and bottom of the fixed edge, open in the middle third, and material
     * at the loaded corner */
    double top_root = 0, bot_root = 0, mid_root = 0;
    int mid = 0;
    for (int j = 0; j < ny; j++) {
        double d = r.density[gelem(&c.g, 0, j, 0)];
        if (j >= ny - 3) top_root += d / 3.0;
        else if (j < 3) bot_root += d / 3.0;
        else if (j > ny / 3 && j < 2 * ny / 3) { mid_root += d; mid++; }
    }
    mid_root /= mid;
    double corner = r.density[gelem(&c.g, nx - 1, 0, 0)];
    NOTE("at the fixed edge: top %.3f, bottom %.3f, middle third %.3f; the loaded corner is at %.3f", top_root, bot_root,
         mid_root, corner);
    CHECK(top_root > 0.6 && bot_root > 0.6, "the fork meets the fixed edge top and bottom (%.3f, %.3f)", top_root, bot_root);
    CHECK(mid_root < 0.4, "the middle of the fixed edge is opened up (%.3f)", mid_root);
    CHECK(corner > 0.5, "material stays at the loaded corner (%.3f)", corner);
    CHECK(t < 120.0, "the cantilever runs in well under two minutes (%.1f s)", t);
    topopt_result_free(&r);
    case_free(&c);
}

/* ---- T3: three dimensions ---------------------------------------------------------------------------------------- */

typedef struct HoleCell { int e; double d; } HoleCell;
static int hole_cmp(const void *a, const void *b) {
    double da = ((const HoleCell *)a)->d, db = ((const HoleCell *)b)->d;
    return da < db ? -1 : (da > db ? 1 : 0);
}

/* the compliance of a given density field, solved once */
static double compliance_of(Case *c, const double *x, double penalty, double emin) {
    SolidOptions o;
    opts(&o);
    double *scale = malloc(sizeof(double) * (size_t)c->g.nelems);
    for (int e = 0; e < c->g.nelems; e++) scale[e] = emin + pow(x[e], penalty) * (1.0 - emin);
    HexModel m = c->model;
    m.elem_scale = scale;
    SolidResult res;
    char err[256] = {0};
    double out = -1;
    if (solid_solve(&m, &c->loads, &o, &res, err, sizeof err)) {
        out = res.external_work;
        solid_result_free(&res);
    } else {
        NOTE("the reference solve failed: %s", err);
    }
    free(scale);
    return out;
}

static void box_3d(void) {
    printf("== T3 a cantilever box in three dimensions at 30 %% volume, against the same volume with a centred hole\n");
    const int nx = 24, ny = 12, nz = 6;
    Case c;
    case_make(&c, nx, ny, nz, 70e9, 0.33, false); /* an aluminium-like modulus; only the ratio of the two matters */
    for (int j = 0; j <= ny; j++)
        for (int k = 0; k <= nz; k++)
            for (int d = 0; d < 3; d++) c.fixed[3 * (size_t)gnode(&c.g, 0, j, k) + d] = 1;
    int tip_nodes = 0;
    for (int k = 0; k <= nz; k++) tip_nodes++;
    for (int k = 0; k <= nz; k++) c.force[3 * (size_t)gnode(&c.g, nx, 0, k) + 1] = -1000.0 / tip_nodes; /* 1 kN down */

    SolidOptions o;
    opts(&o);
    TopOptSettings s;
    topopt_settings_default(&s);
    s.volume_fraction = 0.3;
    s.penalty = 3.0;
    s.filter_radius = 1.5;
    s.passive_layers = 1; /* the app's default: the clamped face and the loaded face stay solid */
    s.max_iter = 60;
    TopOptResult r;
    char err[256] = {0};
    double t0 = now_seconds();
    bool ok = topopt_run(&c.model, &c.loads, &o, &s, &r, err, sizeof err);
    CHECK(ok, "the three-dimensional box optimised: %s", err);
    if (!ok) { case_free(&c); return; }
    double t = now_seconds() - t0;

    /* The same volume of material, arranged without optimisation: exactly the same number of elements taken out of
     * the middle of the box, nearest the centre first, which is one centred hole of the right volume. */
    double *hole = malloc(sizeof(double) * (size_t)c.g.nelems);
    int removed_target = (int)((1.0 - s.volume_fraction) * c.g.nelems + 0.5), removed = removed_target;
    HoleCell *cell = malloc(sizeof(HoleCell) * (size_t)c.g.nelems);
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                int e = gelem(&c.g, i, j, k);
                double dx = (i + 0.5 - nx / 2.0) / nx, dy = (j + 0.5 - ny / 2.0) / ny, dz = (k + 0.5 - nz / 2.0) / nz;
                cell[e].e = e;
                cell[e].d = dx * dx + dy * dy + dz * dz;
            }
    qsort(cell, (size_t)c.g.nelems, sizeof(HoleCell), hole_cmp);
    for (int e = 0; e < c.g.nelems; e++) hole[e] = 1.0;
    for (int i = 0; i < removed_target; i++) hole[cell[i].e] = 1e-3;
    free(cell);
    double hole_fraction = 1.0 - (double)removed / c.g.nelems;
    double c_hole = compliance_of(&c, hole, s.penalty, s.e_min_ratio);
    NOTE("%d iterations, %d solves, %.1f s; optimised compliance %.4g J at volume fraction %.3f (%d passive elements)",
         r.iterations, r.solves, t, r.compliance, r.volume_fraction, r.passive_elements);
    NOTE("the same volume left as a box with a centred hole (%d of %d elements removed, volume fraction %.3f): %.4g J",
         removed, c.g.nelems, hole_fraction, c_hole);
    double factor = c_hole / r.compliance;
    NOTE("the optimised design is %.1f times stiffer than the unoptimised one at the same volume", factor);
    CHECK(c_hole > 0, "the reference design solved");
    CHECK(factor > 2.0, "the optimised design beats the hole by more than a factor of two (%.1f)", factor);
    CHECK(r.volume_fraction < s.volume_fraction + 1e-3, "the volume constraint is met (%.4f)", r.volume_fraction);
    CHECK(t < 120.0, "the three-dimensional case runs in well under two minutes (%.1f s)", t);
    free(hole);
    topopt_result_free(&r);
    case_free(&c);
}

/* ---- refusals ---------------------------------------------------------------------------------------------------- */

static void refusals(void) {
    printf("== T4 what the optimiser refuses instead of guessing\n");
    Case c;
    case_make(&c, 6, 4, 2, 70e9, 0.33, false);
    for (int j = 0; j <= 4; j++)
        for (int k = 0; k <= 2; k++)
            for (int d = 0; d < 3; d++) c.fixed[3 * (size_t)gnode(&c.g, 0, j, k) + d] = 1;
    c.force[3 * (size_t)gnode(&c.g, 6, 0, 0) + 1] = -100.0;
    SolidOptions o;
    opts(&o);
    TopOptSettings s;
    topopt_settings_default(&s);
    TopOptResult r;
    char err[256] = {0};

    s.volume_fraction = 0.01; /* smaller than the passive region */
    bool ok = topopt_run(&c.model, &c.loads, &o, &s, &r, err, sizeof err);
    CHECK(!ok, "a volume fraction below the passive volume is refused");
    NOTE("refusal: %s", err);
    CHECK(strstr(err, "%") != NULL, "the refusal names both percentages");

    s.volume_fraction = 0.5;
    HexModel tet = c.model;
    tet.elem_type = SOLID_ELEM_TET4;
    err[0] = 0;
    ok = topopt_run(&tet, &c.loads, &o, &s, &r, err, sizeof err);
    CHECK(!ok, "a tetrahedral model is refused");
    NOTE("refusal: %s", err);
    case_free(&c);
}

/* ---- T5: the additions of 2026-10-22 (contract amendment 3) ------------------------------------------------------- */

/* Criteria, written before the first run of this test (2026-10-22):
 *  (a) snapshots change nothing: the same run with snapshots of every field and without any gives the identical
 *      final compliance (==), the identical density field (memcmp) and the same iteration count; the snapshots are
 *      the starting field, every field in order and the final one, the k-th carrying the compliance the history has
 *      for it, bit for bit, and the last the result's compliance; every third field gives the same run again;
 *  (b) the warm start agrees with the cold start to the solver's tolerance: final compliances within 1e-6
 *      relative, densities within 1e-4, the same iteration count, and fewer conjugate-gradient iterations in all;
 *  (c) the volume continuation (the MBB beam of T1 from the whole domain solid, 0.02 of volume less per update):
 *      the aim is followed (the volume after update k is max(0.5, 1 - 0.02 k) within 1e-6), the volume constraint
 *      is met at the end (within 1e-3), f^T u and 2 U agree (1e-6), the run does not stop before the target is
 *      reached, and the design is as stiff as the published one: within 5 % of 216.81;
 *  (d) a region held solid by the caller stays at exactly 1 and is counted as passive. */

typedef struct SnapLog {
    int count, iters[400], finals;
    double compliance[400], volume[400];
} SnapLog;

static void snap_record(void *ctx, const TopOptSnapshot *sn) {
    SnapLog *l = ctx;
    if (l->count < 400) {
        l->iters[l->count] = sn->iter;
        l->compliance[l->count] = sn->compliance;
        l->volume[l->count] = sn->volume_fraction;
    }
    l->finals += sn->final;
    l->count++;
}

static void cantilever_box(Case *c, int nx, int ny, int nz) {
    case_make(c, nx, ny, nz, 70e9, 0.33, false);
    for (int j = 0; j <= ny; j++)
        for (int k = 0; k <= nz; k++)
            for (int d = 0; d < 3; d++) c->fixed[3 * (size_t)gnode(&c->g, 0, j, k) + d] = 1;
    for (int k = 0; k <= nz; k++) c->force[3 * (size_t)gnode(&c->g, nx, 0, k) + 1] = -1000.0 / (nz + 1);
}

static void additions(void) {
    printf("== T5 the additions of 2026-10-22: snapshots, warm start, volume continuation, regions held solid\n");
    char err[256] = {0};

    /* (a) snapshots change nothing */
    {
        Case c;
        cantilever_box(&c, 16, 8, 4);
        SolidOptions o;
        opts(&o);
        TopOptSettings s;
        topopt_settings_default(&s);
        s.volume_fraction = 0.3;
        s.filter_radius = 1.5;
        s.max_iter = 40;
        TopOptResult plain, snapped, third;
        bool ok1 = topopt_run(&c.model, &c.loads, &o, &s, &plain, err, sizeof err);
        SnapLog *log = calloc(1, sizeof *log), *log3 = calloc(1, sizeof *log3);
        s.snapshot = snap_record;
        s.snapshot_ctx = log;
        s.snapshot_every = 1;
        bool ok2 = topopt_run(&c.model, &c.loads, &o, &s, &snapped, err, sizeof err);
        s.snapshot_ctx = log3;
        s.snapshot_every = 3;
        bool ok3 = topopt_run(&c.model, &c.loads, &o, &s, &third, err, sizeof err);
        CHECK(ok1 && ok2 && ok3, "the box optimised with and without snapshots: %s", err);
        if (ok1 && ok2 && ok3) {
            int n = c.g.nelems;
            NOTE("(a) %d iterations; compliance %.17g J without snapshots, %.17g J with every field, %.17g J with every "
                 "third; %d and %d snapshots",
                 plain.iterations, plain.compliance, snapped.compliance, third.compliance, log->count, log3->count);
            CHECK(plain.compliance == snapped.compliance && plain.compliance == third.compliance,
                  "the final compliance is identical with snapshots");
            CHECK(!memcmp(plain.density, snapped.density, sizeof(double) * (size_t)n) &&
                      !memcmp(plain.density, third.density, sizeof(double) * (size_t)n),
                  "the density field is identical with snapshots");
            CHECK(plain.iterations == snapped.iterations && plain.iterations == third.iterations,
                  "the iteration count is identical (%d, %d, %d)", plain.iterations, snapped.iterations, third.iterations);
            bool order = log->count == plain.iterations + 1 && log->finals == 1, same = true;
            for (int k = 0; k < log->count && k < 400; k++) {
                if (log->iters[k] != k) order = false;
                double want = k < plain.iterations ? plain.history[k].compliance : plain.compliance;
                if (log->compliance[k] != want) same = false;
            }
            CHECK(order, "every field is handed over once, in order, the final one marked (%d snapshots for %d updates)",
                  log->count, plain.iterations);
            CHECK(same, "each snapshot carries the compliance of its field, bit for bit");
            bool every3 = log3->finals == 1;
            int want3 = (plain.iterations - 1) / 3 + 2; /* 0, 3, 6, ... below the count, and the final field */
            if (log3->count != want3) every3 = false;
            for (int k = 0; k + 1 < log3->count && k < 400; k++)
                if (log3->iters[k] != 3 * k) every3 = false;
            CHECK(every3, "every third field and the final one (%d snapshots, %d expected)", log3->count, want3);
        }
        free(log);
        free(log3);
        if (ok1) topopt_result_free(&plain);
        if (ok2) topopt_result_free(&snapped);
        if (ok3) topopt_result_free(&third);
        case_free(&c);
    }

    /* (b) the warm start, on the conjugate-gradient solver */
    {
        Case c;
        cantilever_box(&c, 24, 12, 6);
        SolidOptions o;
        opts(&o);
        o.solver = SOLID_SOLVER_PCG;
        TopOptSettings s;
        topopt_settings_default(&s);
        s.volume_fraction = 0.3;
        s.filter_radius = 1.5;
        s.max_iter = 30;
        TopOptResult cold, warm;
        double t0 = now_seconds();
        bool ok1 = topopt_run(&c.model, &c.loads, &o, &s, &cold, err, sizeof err);
        double t1 = now_seconds();
        s.warm_start = 1;
        bool ok2 = topopt_run(&c.model, &c.loads, &o, &s, &warm, err, sizeof err);
        double t2 = now_seconds();
        CHECK(ok1 && ok2, "the box optimised cold and warm on the iterative solver: %s", err);
        if (ok1 && ok2) {
            double rel = fabs(warm.compliance - cold.compliance) / cold.compliance, dx = 0;
            for (int e = 0; e < c.g.nelems; e++) dx = fmax(dx, fabs(warm.density[e] - cold.density[e]));
            NOTE("(b) %d iterations; compliance cold %.10g J, warm %.10g J (%.1e apart); densities at most %.1e apart",
                 cold.iterations, cold.compliance, warm.compliance, rel, dx);
            NOTE("    conjugate-gradient iterations cold %lld, warm %lld (%.2f times fewer); solver time %.1f s against "
                 "%.1f s, whole runs %.1f s against %.1f s",
                 cold.solver_iterations, warm.solver_iterations, (double)cold.solver_iterations / (double)(warm.solver_iterations ? warm.solver_iterations : 1),
                 cold.solve_seconds, warm.solve_seconds, t1 - t0, t2 - t1);
            CHECK(rel < 1e-6, "the warm start gives the cold start's compliance to 1e-6 (%.1e)", rel);
            CHECK(dx < 1e-4, "and its densities to 1e-4 (%.1e)", dx);
            CHECK(warm.iterations == cold.iterations, "and the same iteration count (%d, %d)", warm.iterations, cold.iterations);
            CHECK(warm.solver_iterations < cold.solver_iterations, "with fewer conjugate-gradient iterations (%lld < %lld)",
                  warm.solver_iterations, cold.solver_iterations);
        }
        if (ok1) topopt_result_free(&cold);
        if (ok2) topopt_result_free(&warm);
        case_free(&c);
    }

    /* (c) the volume continuation on the MBB beam of T1 */
    {
        const int nx = 60, ny = 20;
        Case c;
        case_make(&c, nx, ny, 1, 1.0, 0.3, true);
        for (int j = 0; j <= ny; j++)
            for (int k = 0; k <= 1; k++) c.fixed[3 * (size_t)gnode(&c.g, 0, j, k) + 0] = 1;
        for (int k = 0; k <= 1; k++) c.fixed[3 * (size_t)gnode(&c.g, nx, 0, k) + 1] = 1;
        for (int k = 0; k <= 1; k++) c.force[3 * (size_t)gnode(&c.g, 0, ny, k) + 1] = -0.5;
        SolidOptions o;
        opts(&o);
        TopOptSettings s;
        topopt_settings_default(&s);
        s.volume_fraction = 0.5;
        s.filter_radius = 2.4;
        s.passive_layers = 0;
        s.x_min = 0.0;
        s.max_iter = 300;
        s.volume_start = 1.0;
        s.volume_step = 0.02;
        TopOptResult r;
        double t0 = now_seconds();
        bool ok = topopt_run(&c.model, &c.loads, &o, &s, &r, err, sizeof err);
        CHECK(ok, "the MBB beam optimised from the whole domain: %s", err);
        if (ok) {
            double worst = 0;
            int carving = 0;
            for (int k = 1; k <= r.iterations; k++) {
                double aim = 1.0 - 0.02 * k;
                if (aim <= 0.5) break;
                carving++;
                worst = fmax(worst, fabs(r.history[k - 1].volume_fraction - aim));
            }
            double rel = 100.0 * (r.compliance - 216.81) / 216.81;
            NOTE("(c) %d iterations (%.1f s), the target reached at update %d, stopped: %s; the solid start %.4g J; final "
                 "compliance %.2f J against the published 216.81 (%+.2f %%); volume %.4f; f^T u against 2 U %.1e",
                 r.iterations, now_seconds() - t0, r.target_reached_at, r.stop_reason, r.compliance_initial, r.compliance,
                 rel, r.volume_fraction, r.energy_mismatch);
            NOTE("    over the %d updates above the target the volume followed the aim to %.1e", carving, worst);
            print_density(&c.g, r.density, "MBB beam density from the whole domain");
            CHECK(carving >= 20 && worst < 1e-6, "the volume follows the continuation's aim (%d updates, %.1e)", carving, worst);
            CHECK(fabs(r.volume_fraction - 0.5) < 1e-3, "the volume constraint is met at the end (%.4f)", r.volume_fraction);
            CHECK(r.energy_mismatch < 1e-6, "f^T u and twice the strain energy agree (%.1e)", r.energy_mismatch);
            CHECK(r.iterations > r.target_reached_at, "the run goes on after the target is reached (%d > %d)", r.iterations,
                  r.target_reached_at);
            CHECK(fabs(rel) <= 5.0, "the continuation's design is within 5 %% of the published compliance (%+.2f %%)", rel);
            topopt_result_free(&r);
        }
        case_free(&c);
    }

    /* (d) a region held solid by the caller */
    {
        const int nx = 32, ny = 20;
        Case c;
        case_make(&c, nx, ny, 1, 1.0, 0.3, true);
        for (int j = 0; j <= ny; j++)
            for (int k = 0; k <= 1; k++) {
                c.fixed[3 * (size_t)gnode(&c.g, 0, j, k) + 0] = 1;
                c.fixed[3 * (size_t)gnode(&c.g, 0, j, k) + 1] = 1;
            }
        for (int k = 0; k <= 1; k++) c.force[3 * (size_t)gnode(&c.g, nx, 0, k) + 1] = -0.5;
        unsigned char *held = calloc((size_t)c.g.nelems, 1);
        int nheld = 0;
        for (int j = 8; j < 12; j++) /* a 4 x 4 block in the middle, where the published fork is open */
            for (int i = 14; i < 18; i++) held[gelem(&c.g, i, j, 0)] = 1, nheld++;
        SolidOptions o;
        opts(&o);
        TopOptSettings s;
        topopt_settings_default(&s);
        s.volume_fraction = 0.4;
        s.filter_radius = 1.2;
        s.passive_layers = 0;
        s.x_min = 0.0;
        s.max_iter = 200;
        s.passive_solid = held;
        TopOptResult r;
        bool ok = topopt_run(&c.model, &c.loads, &o, &s, &r, err, sizeof err);
        CHECK(ok, "the short cantilever with a block held solid optimised: %s", err);
        if (ok) {
            int solid = 0;
            for (int e = 0; e < c.g.nelems; e++)
                if (held[e] && r.density[e] == 1.0 && r.passive[e]) solid++;
            NOTE("(d) %d of %d held elements at exactly 1; %d passive elements reported; volume %.4f; compliance %.3f J",
                 solid, nheld, r.passive_elements, r.volume_fraction, r.compliance);
            CHECK(solid == nheld, "every held element stays at exactly 1 (%d of %d)", solid, nheld);
            CHECK(r.passive_elements == nheld, "and is counted as passive (%d)", r.passive_elements);
            CHECK(fabs(r.volume_fraction - 0.4) < 1e-3, "the volume constraint is met (%.4f)", r.volume_fraction);
            topopt_result_free(&r);
        }
        /* and a continuation that starts below its target is refused, not run */
        s.passive_solid = NULL;
        s.volume_start = 0.3;
        s.volume_step = 0.02;
        err[0] = 0;
        ok = topopt_run(&c.model, &c.loads, &o, &s, &r, err, sizeof err);
        CHECK(!ok, "a volume continuation that starts below the target is refused");
        NOTE("refusal: %s", err);
        free(held);
        case_free(&c);
    }
}

/* ---- T6: the multigrid solver on voxel meshes (contract amendment 4) ---------------------------------------------- */

/* Criteria, written before the first run of this test (2026-10-22): against the direct solver (sparse Cholesky, exact
 * to round-off) on the same assembled problem, with the multigrid conjugate gradients at a relative residual of 1e-10,
 *  (a) a uniform cantilever box (24 x 12 x 6, clamped end, tip load): the largest displacement difference is below
 *      1e-7 of the largest displacement, in at most 40 iterations;
 *  (b) the same box with the densities T3's optimisation found (stiffness x^3 between 1e-9 and 1, the contrast of a
 *      topology optimisation): the same 1e-7, in at most 80 iterations;
 *  (c) an irregular lattice of odd size (25 x 13 x 7 cells) with a hole of cells left out, one face clamped and one
 *      plane held in a single component (a symmetry plane): the same 1e-7, in at most 60 iterations;
 *  (d) a mesh that is not a lattice of cubes (one node moved) is refused with a message;
 *  (e) the topology optimisation of T3 solved by multigrid gives the direct run's design: compliance within 1e-6,
 *      densities within 1e-4, the same iteration count;
 *  (f) on a larger box (40 x 20 x 20 cells, 52,000 equations) the multigrid solve takes less time than the block-Jacobi
 *      conjugate gradients at the same tolerance; both times and iteration counts are printed. */

static double max_abs(const double *v, size_t n) {
    double m = 0;
    for (size_t i = 0; i < n; i++) m = fmax(m, fabs(v[i]));
    return m;
}

/* one static solve of the case, with the given solver and stiffness scales; returns the displacements (3 nnodes) */
static double *solve_case(Case *c, const double *scale, SolidSolver solver, int *iters, double *seconds, char *err, size_t errlen) {
    SolidOptions o;
    opts(&o);
    o.solver = solver;
    o.pcg_max_iter = 100000;
    HexModel m = c->model;
    m.elem_scale = scale;
    SolidResult res;
    double t0 = now_seconds();
    if (!solid_solve(&m, &c->loads, &o, &res, err, errlen)) return NULL;
    if (seconds) *seconds = now_seconds() - t0;
    if (iters) *iters = res.stats.iterations;
    double *u = malloc(sizeof(double) * 3 * (size_t)c->g.nnodes);
    memcpy(u, res.u, sizeof(double) * 3 * (size_t)c->g.nnodes);
    solid_result_free(&res);
    return u;
}

static void compare_mg(Case *c, const double *scale, const char *what, int max_iters) {
    char err[512] = {0};
    int it = 0;
    double tm = 0, td = 0;
    double *ud = solve_case(c, scale, SOLID_SOLVER_DIRECT, NULL, &td, err, sizeof err);
    double *um = ud ? solve_case(c, scale, SOLID_SOLVER_MG, &it, &tm, err, sizeof err) : NULL;
    CHECK(ud && um, "%s: both solvers ran (%s)", what, err);
    if (ud && um) {
        size_t n = 3 * (size_t)c->g.nnodes;
        double d = 0;
        for (size_t i = 0; i < n; i++) d = fmax(d, fabs(um[i] - ud[i]));
        double rel = d / max_abs(ud, n);
        NOTE("%s: multigrid %d iterations (%.2f s), direct %.2f s; largest difference %.2e of the largest displacement", what, it, tm, td, rel);
        CHECK(rel < 1e-7, "%s: the multigrid displacements are the direct ones to 1e-7 (%.2e)", what, rel);
        CHECK(it <= max_iters, "%s: in at most %d iterations (%d)", what, max_iters, it);
    }
    free(ud);
    free(um);
}

static void multigrid(void) {
    printf("== T6 the multigrid solver on voxel meshes, against the direct solver\n");
    char err[512] = {0};
    /* (a) and (b): the cantilever box of T3, uniform and with T3's optimised densities */
    {
        Case c;
        cantilever_box(&c, 24, 12, 6);
        c.model.formulation = HEX8_INCOMPATIBLE; /* the static analysis' element; T3 runs the full one */
        compare_mg(&c, NULL, "(a) uniform box", 40);
        SolidOptions o;
        opts(&o);
        TopOptSettings s;
        topopt_settings_default(&s);
        s.volume_fraction = 0.3;
        s.filter_radius = 1.5;
        s.max_iter = 60;
        TopOptResult r;
        if (topopt_run(&c.model, &c.loads, &o, &s, &r, err, sizeof err)) {
            double *scale = malloc(sizeof(double) * (size_t)c.g.nelems);
            for (int e = 0; e < c.g.nelems; e++) scale[e] = 1e-9 + pow(r.density[e], 3.0) * (1 - 1e-9);
            compare_mg(&c, scale, "(b) optimised densities", 80);
            free(scale);
            topopt_result_free(&r);
        } else {
            CHECK(false, "(b) the densities to test with: %s", err);
        }
        case_free(&c);
    }
    /* (c) an odd lattice with a hole, a clamped face and a plane held in y */
    {
        const int nx = 25, ny = 13, nz = 7;
        Grid g;
        grid_make(&g, nx, ny, nz);
        /* leave out a hole of cells: 6 <= i < 12, 4 <= j < 9, all k; renumber the elements that stay */
        int keep = 0;
        int *conn = malloc(sizeof(int) * 8 * (size_t)g.nelems);
        for (int k = 0; k < nz; k++)
            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx; i++) {
                    if (i >= 6 && i < 12 && j >= 4 && j < 9) continue;
                    memcpy(conn + 8 * (size_t)keep, g.conn + 8 * (size_t)gelem(&g, i, j, k), sizeof(int) * 8);
                    keep++;
                }
        Case c;
        memset(&c, 0, sizeof c);
        c.g = g;
        c.mat = (SolidMaterial){110e9, 0.342, 0.0};
        c.fixed = calloc(3 * (size_t)g.nnodes, 1);
        c.force = calloc(3 * (size_t)g.nnodes, sizeof(double));
        c.model.nnodes = g.nnodes;
        c.model.nelems = keep;
        c.model.xyz = g.xyz;
        c.model.conn = conn;
        c.model.nmat = 1;
        c.model.mat = &c.mat;
        c.model.formulation = HEX8_INCOMPATIBLE;
        c.model.elem_type = SOLID_ELEM_HEX8;
        c.loads.fixed = c.fixed;
        c.loads.nodal_force = c.force;
        for (int j = 0; j <= ny; j++) /* the face x = 0 clamped */
            for (int k = 0; k <= nz; k++)
                for (int d = 0; d < 3; d++) c.fixed[3 * (size_t)gnode(&g, 0, j, k) + d] = 1;
        for (int i = 0; i <= nx; i++) /* the plane y = 0 held in y only */
            for (int k = 0; k <= nz; k++) c.fixed[3 * (size_t)gnode(&g, i, 0, k) + 1] = 1;
        for (int j = 0; j <= ny; j++) c.force[3 * (size_t)gnode(&g, nx, j, nz) + 2] = -1000.0 / (ny + 1);
        compare_mg(&c, NULL, "(c) odd lattice with a hole and a symmetry plane", 60);
        /* (d) one node moved: not a lattice of cubes any more */
        double *xyz = malloc(sizeof(double) * 3 * (size_t)g.nnodes);
        memcpy(xyz, g.xyz, sizeof(double) * 3 * (size_t)g.nnodes);
        xyz[3 * (size_t)gnode(&g, 3, 3, 3) + 0] += 0.2;
        c.model.xyz = xyz;
        SolidOptions o;
        opts(&o);
        o.solver = SOLID_SOLVER_MG;
        SolidResult res;
        err[0] = 0;
        bool ok = solid_solve(&c.model, &c.loads, &o, &res, err, sizeof err);
        if (ok) solid_result_free(&res);
        CHECK(!ok && strstr(err, "voxel"), "(d) a mesh that is not a lattice of cubes is refused (%s)", err);
        NOTE("(d) refusal: %s", err);
        free(xyz);
        free(conn);
        case_free(&c);
    }
    /* (e) the optimisation of T3 with the multigrid solver */
    {
        Case c;
        cantilever_box(&c, 24, 12, 6);
        SolidOptions o;
        opts(&o);
        TopOptSettings s;
        topopt_settings_default(&s);
        s.volume_fraction = 0.3;
        s.filter_radius = 1.5;
        s.max_iter = 60;
        TopOptResult rd, rm;
        o.solver = SOLID_SOLVER_DIRECT;
        bool ok1 = topopt_run(&c.model, &c.loads, &o, &s, &rd, err, sizeof err);
        o.solver = SOLID_SOLVER_MG;
        bool ok2 = ok1 && topopt_run(&c.model, &c.loads, &o, &s, &rm, err, sizeof err);
        CHECK(ok1 && ok2, "(e) the box optimised with the direct and the multigrid solver: %s", err);
        if (ok1 && ok2) {
            double rel = fabs(rm.compliance - rd.compliance) / rd.compliance, dx = 0;
            for (int e = 0; e < c.g.nelems; e++) dx = fmax(dx, fabs(rm.density[e] - rd.density[e]));
            NOTE("(e) %d and %d iterations; compliance direct %.10g J, multigrid %.10g J (%.1e apart); densities at most %.1e apart; "
                 "%lld multigrid iterations in %d solves",
                 rd.iterations, rm.iterations, rd.compliance, rm.compliance, rel, dx, rm.solver_iterations, rm.solves);
            CHECK(rel < 1e-6, "(e) the multigrid run's compliance is the direct run's to 1e-6 (%.1e)", rel);
            CHECK(dx < 1e-4, "(e) and its densities to 1e-4 (%.1e)", dx);
            CHECK(rm.iterations == rd.iterations, "(e) with the same iteration count (%d, %d)", rm.iterations, rd.iterations);
        }
        if (ok1) topopt_result_free(&rd);
        if (ok2) topopt_result_free(&rm);
        case_free(&c);
    }
    /* (f) a larger box: the time against block-Jacobi conjugate gradients */
    {
        Case c;
        cantilever_box(&c, 40, 20, 20);
        c.model.formulation = HEX8_INCOMPATIBLE;
        int itp = 0, itm = 0;
        double tp = 0, tm = 0;
        double *up = solve_case(&c, NULL, SOLID_SOLVER_PCG, &itp, &tp, err, sizeof err);
        double *um = up ? solve_case(&c, NULL, SOLID_SOLVER_MG, &itm, &tm, err, sizeof err) : NULL;
        CHECK(up && um, "(f) both iterative solvers ran (%s)", err);
        if (up && um) {
            size_t n = 3 * (size_t)c.g.nnodes;
            double d = 0;
            for (size_t i = 0; i < n; i++) d = fmax(d, fabs(um[i] - up[i]));
            NOTE("(f) %d elements: block-Jacobi conjugate gradients %d iterations in %.2f s, multigrid %d iterations in %.2f s (%.1f times "
                 "faster); the two answers %.1e apart",
                 c.g.nelems, itp, tp, itm, tm, tp / tm, d / max_abs(up, n));
            CHECK(tm < tp, "(f) the multigrid solve is faster than block-Jacobi conjugate gradients (%.2f s against %.2f s)", tm, tp);
        }
        free(up);
        free(um);
        case_free(&c);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    g_pool = pool_create(cpu_perf_count() > 0 ? cpu_perf_count() : 4);
    double t0 = now_seconds();
    mbb_beam();
    short_cantilever();
    box_3d();
    refusals();
    additions();
    multigrid();
    printf("\n%s: %d passed, %d failed (%.1f s)\n", g_fail ? "TOPOLOGY OPTIMISATION FAILED" : "ALL TOPOLOGY OPTIMISATION TESTS PASSED",
           g_pass, g_fail, now_seconds() - t0);
    pool_destroy(g_pool);
    return g_fail ? 1 : 0;
}
