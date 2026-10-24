/* gendesign.c - generative design with the topology optimiser: a titanium engine-mount bracket optimised on the voxel
 * mesh, every iteration kept, and each kept design written as a closed, smoothed surface for the film
 * (tools/render/README.md, "Generative design"; the optimiser: docs/contracts/topology-optimisation.md).
 *
 *   build/gendesign run OUTDIR [options]          build the case, optimise it, keep the iterations, check the criteria
 *   build/gendesign ply OUTDIR PLYDIR [options]   the kept designs as closed, smoothed isosurfaces of the density (PLY)
 *
 * The case was drawn for this film, not taken from a file: a bracket that hangs a load from a clevis and is bolted
 * down with four bolts, the kind of part an aircraft engine mount uses.
 *   design space   160 x 100 x 72 mm (x along the bolt rows, y across, z up from the mounting face at z = 0)
 *   bolts          four, at x = +-62, y = +-34 mm: holes of 11 mm, bosses of 24 mm diameter and 10 mm height held solid,
 *                  their bottom faces clamped (the bolted joint), and above each boss a cylinder of 24 mm kept empty to the
 *                  top of the space for the bolt head and the socket
 *   clevis         a pin of 12 mm along y through (x, z) = (0, 54) mm; two lugs held solid, rings to 15 mm radius at
 *                  7 <= |y| <= 19 mm; between them room for the rod end (a ring to 19 mm and the rod's slot along the
 *                  load), outside them room for the pin's head and nut (to 10 mm)
 *   load           25 kN through the pin, 30 degrees from vertical towards +x in the x-z plane, spread over the half of
 *                  each lug's bore that the pin presses on, as cos(angle) from the load's line (at 45 degrees the bolts
 *                  on the far side carry almost no axial load, and the optimiser left their bosses unconnected)
 *   material       Ti-6Al-4V as built by laser powder bed fusion, from the library (src/ctl/materials.json,
 *                  ti6al4v_lpbf: E 110 GPa from the EOS Ti64 data sheet, Poisson's ratio 0.342 from TIMET)
 * The case and its load are symmetric about y = 0, so by default half of it is computed (y >= 0, the plane held in y)
 * and the other half is its mirror image; --full computes the whole.
 *
 * run writes OUTDIR/case.txt and cells.u8 (the grid), snap/it_NNNN.f32 (the density of every kept field, one float per
 * element), history.csv, final.f32 and final_vm.f32 (the von Mises stress of the final design), summary.json (every
 * number and the criteria), and setup.png and progress.png for the owner's phone (~/bin/story, when it exists). */
#include "../src/ctl/matlib.h"
#include "../src/fem/solid.h"
#include "../src/fem/topopt.h"
#include "../src/render/swrender.h"
#include "../src/threads.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

double now_seconds(void);

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "gendesign: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}
static void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) die("out of memory (%zu x %zu bytes)", n, s);
    return p;
}

static void path(char *out, size_t cap, const char *dir, const char *name) { snprintf(out, cap, "%s/%s", dir, name); }

/* ---- the case ----------------------------------------------------------------------------------------------------- */

typedef struct Bracket {
    double len, wid, hgt;                  /* the design space, mm: x in +-len/2, y in +-wid/2, z in 0..hgt */
    double bolt_x, bolt_y;                 /* four bolts at (+-bolt_x, +-bolt_y) */
    double bolt_r, boss_r, boss_h, head_r; /* hole, boss (held solid), its height, the room for head and socket above */
    double pin_z, pin_r;                   /* the clevis pin along y through (0, pin_z) */
    double lug_r, lug_y0, lug_y1;          /* the lugs (held solid): rings to lug_r at lug_y0 <= |y| <= lug_y1 */
    double gap_r;                          /* between the lugs: a ring to gap_r and the rod's slot along the load */
    double end_r;                          /* outside the lugs: the pin's head and nut */
    double force, angle;                   /* N through the pin; degrees from vertical towards +x, in the x-z plane */
} Bracket;

static const Bracket BRACKET = {160, 100, 72, 62, 34, 5.5, 12, 10, 12, 54, 6, 15, 7, 19, 19, 10, 25000, 30};

enum { C_VOID = 0, C_DESIGN = 1, C_SOLID = 2 };           /* not meshed (holes, room for other parts), free, held solid */
enum { T_NONE = 0, T_BOSS = 1, T_LUG = 5, T_PIN = 6 };    /* T_BOSS + 0..3: the four bosses; T_PIN: the pin's hole */

/* the class of the point (x, y, z) in mm, and what it belongs to */
static int classify(const Bracket *b, double x, double y, double z, int *tag) {
    *tag = T_NONE;
    for (int s = 0; s < 4; s++) {
        double bx = (s & 1) ? b->bolt_x : -b->bolt_x, by = (s & 2) ? b->bolt_y : -b->bolt_y;
        double r = hypot(x - bx, y - by);
        if (r < b->bolt_r) return C_VOID;
        if (z < b->boss_h && r < b->boss_r) { *tag = T_BOSS + s; return C_SOLID; }
        if (z >= b->boss_h && r < b->head_r) return C_VOID;
    }
    double a = b->angle * M_PI / 180.0, dx = sin(a), dz = cos(a);
    double px = x, pz = z - b->pin_z, rp = hypot(px, pz), ay = fabs(y);
    if (rp < b->pin_r) { *tag = T_PIN; return C_VOID; }
    if (ay < b->lug_y0) {
        if (rp < b->gap_r) return C_VOID;
        double along = px * dx + pz * dz, across = fabs(px * dz - pz * dx); /* the rod, from the pin along the load */
        if (along > 0 && across < b->gap_r) return C_VOID;
    }
    if (ay > b->lug_y1 && rp < b->end_r) return C_VOID;
    if (ay >= b->lug_y0 && ay <= b->lug_y1 && rp < b->lug_r) { *tag = T_LUG; return C_SOLID; }
    return C_DESIGN;
}

typedef struct Grid {
    int nx, ny, nz;            /* cells */
    double h;                  /* m */
    double x0, y0, z0;         /* m: the low corner of cell (0, 0, 0) */
    int half;                  /* 1: y >= 0 only, mirror image about y = 0 */
    unsigned char *cls, *tag;  /* per cell */
    int *elem_of, *cell_of;    /* cell -> element (-1: not meshed), element -> cell */
    int nelems, nnodes;
    int *node_of;              /* grid node -> model node (-1: unused) */
    double *xyz;               /* 3 nnodes, m */
    int *conn;                 /* 8 nelems */
} Grid;

static size_t gcell(const Grid *g, int i, int j, int k) { return ((size_t)k * g->ny + j) * g->nx + i; }
static size_t gnode(const Grid *g, int i, int j, int k) { return ((size_t)k * (g->ny + 1) + j) * (g->nx + 1) + i; }

static void grid_classify(Grid *g, const Bracket *b, double h_mm, int half) {
    memset(g, 0, sizeof *g);
    g->half = half;
    g->nx = (int)lround(b->len / h_mm);
    g->ny = (int)lround((half ? 0.5 * b->wid : b->wid) / h_mm);
    g->nz = (int)lround(b->hgt / h_mm);
    if (g->nx < 4 || g->ny < 2 || g->nz < 4) die("voxel size %g mm is too coarse for the bracket", h_mm);
    double x0 = -0.5 * g->nx * h_mm, y0 = half ? 0.0 : -0.5 * g->ny * h_mm, z0 = 0.0;
    g->h = h_mm * 1e-3;
    g->x0 = x0 * 1e-3, g->y0 = y0 * 1e-3, g->z0 = z0;
    size_t nc = (size_t)g->nx * g->ny * g->nz;
    g->cls = xmalloc(nc);
    g->tag = xmalloc(nc);
    for (int k = 0; k < g->nz; k++)
        for (int j = 0; j < g->ny; j++)
            for (int i = 0; i < g->nx; i++) {
                int t;
                size_t c = gcell(g, i, j, k);
                g->cls[c] = (unsigned char)classify(b, x0 + (i + 0.5) * h_mm, y0 + (j + 0.5) * h_mm, z0 + (k + 0.5) * h_mm, &t);
                g->tag[c] = (unsigned char)t;
            }
}

/* elements of the meshed cells in cell order, and the nodes they use */
static void grid_mesh(Grid *g) {
    size_t nc = (size_t)g->nx * g->ny * g->nz, nn = (size_t)(g->nx + 1) * (g->ny + 1) * (g->nz + 1);
    g->elem_of = xmalloc(sizeof(int) * nc);
    g->nelems = 0;
    for (size_t c = 0; c < nc; c++) g->elem_of[c] = g->cls[c] != C_VOID ? g->nelems++ : -1;
    g->cell_of = xmalloc(sizeof(int) * (size_t)g->nelems);
    for (size_t c = 0; c < nc; c++)
        if (g->elem_of[c] >= 0) g->cell_of[g->elem_of[c]] = (int)c;
    g->node_of = xmalloc(sizeof(int) * nn);
    for (size_t n = 0; n < nn; n++) g->node_of[n] = -1;
    static const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    for (int k = 0; k < g->nz; k++)
        for (int j = 0; j < g->ny; j++)
            for (int i = 0; i < g->nx; i++)
                if (g->cls[gcell(g, i, j, k)] != C_VOID)
                    for (int a = 0; a < 8; a++) g->node_of[gnode(g, i + corner[a][0], j + corner[a][1], k + corner[a][2])] = 0;
    g->nnodes = 0;
    for (size_t n = 0; n < nn; n++)
        if (g->node_of[n] == 0) g->node_of[n] = g->nnodes++;
    g->xyz = xmalloc(sizeof(double) * 3 * (size_t)g->nnodes);
    for (int k = 0; k <= g->nz; k++)
        for (int j = 0; j <= g->ny; j++)
            for (int i = 0; i <= g->nx; i++) {
                int n = g->node_of[gnode(g, i, j, k)];
                if (n < 0) continue;
                g->xyz[3 * (size_t)n + 0] = g->x0 + i * g->h;
                g->xyz[3 * (size_t)n + 1] = g->y0 + j * g->h;
                g->xyz[3 * (size_t)n + 2] = g->z0 + k * g->h;
            }
    g->conn = xmalloc(sizeof(int) * 8 * (size_t)g->nelems);
    for (int e = 0; e < g->nelems; e++) {
        size_t c = (size_t)g->cell_of[e];
        int i = (int)(c % g->nx), j = (int)((c / g->nx) % g->ny), k = (int)(c / ((size_t)g->nx * g->ny));
        for (int a = 0; a < 8; a++) g->conn[8 * (size_t)e + a] = g->node_of[gnode(g, i + corner[a][0], j + corner[a][1], k + corner[a][2])];
    }
}

/* ---- options ------------------------------------------------------------------------------------------------------ */

typedef struct Opts {
    double h, vf, rmin, vstart, vstep, tol, emin, change_tol, move, angle, force;
    int iters, threads, warm, snap, half, story, story_minutes;
    SolidSolver solver;
} Opts;

static void opts_default(Opts *o) {
    memset(o, 0, sizeof *o);
    o->h = 2.0;
    o->vf = 0.12;
    o->rmin = -1;      /* 2.5 voxels */
    o->vstart = 1.0;   /* from the whole design space, solid */
    o->vstep = 0.01;
    o->tol = 1e-10;
    o->emin = 1e-9;
    o->change_tol = 0.01;
    o->move = 0.2;
    o->angle = -1; /* the case's own */
    o->force = 0;
    o->iters = 300;
    o->threads = 4;
    o->warm = 1;
    o->snap = 1;
    o->half = 1;
    o->story = 0;
    o->story_minutes = 10;
    o->solver = SOLID_SOLVER_MG; /* multigrid conjugate gradients on the voxel lattice (src/fem/vmg.h) */
}

static void usage(void) {
    fprintf(stderr,
            "usage: build/gendesign run OUTDIR [--h MM] [--vf F] [--rmin MM] [--vstart F] [--vstep F] [--iters N]\n"
            "                      [--tol T] [--emin R] [--change T] [--move M] [--threads N] [--warm 0|1] [--snap K]\n"
            "                      [--angle DEG] [--force N]\n"
            "                      [--solver mg|pcg|direct|auto] [--full] [--story] [--story-minutes M]\n"
            "       build/gendesign ply OUTDIR PLYDIR [--every K] [--from A] [--to B] [--dense-to D --sparse S] [--taubin N]\n"
            "                      [--domain] [--final] [--check-only] [--only NAME] [--plan-to N]\n"
            "       build/gendesign pic OUTDIR PICTURE.png [--iter K]   a quick look at the final design or iteration K\n"
            "       build/gendesign solve OUTDIR --iter K [--solver mg|pcg|direct] [--threads N] [--tol T] [--emin R]\n");
    exit(2);
}

/* ---- isosurfaces: marching tetrahedra on the node-averaged density ------------------------------------------------ */

typedef struct Mesh {
    int nv, nt, capv, capt;
    double *v;  /* 3 nv */
    float *f;   /* nv: a field at the vertices */
    int *t;     /* 3 nt */
    double *n;  /* 3 nv after mesh_normals */
} Mesh;

static void mesh_free(Mesh *m) {
    free(m->v), free(m->f), free(m->t), free(m->n);
    memset(m, 0, sizeof *m);
}

static int mesh_vertex(Mesh *m, const double p[3], float f) {
    if (m->nv == m->capv) {
        m->capv = m->capv ? 2 * m->capv : 65536;
        m->v = realloc(m->v, sizeof(double) * 3 * (size_t)m->capv);
        m->f = realloc(m->f, sizeof(float) * (size_t)m->capv);
        if (!m->v || !m->f) die("out of memory for the surface");
    }
    memcpy(m->v + 3 * (size_t)m->nv, p, sizeof(double) * 3);
    m->f[m->nv] = f;
    return m->nv++;
}

static void mesh_tri(Mesh *m, int a, int b, int c) {
    if (m->nt == m->capt) {
        m->capt = m->capt ? 2 * m->capt : 131072;
        m->t = realloc(m->t, sizeof(int) * 3 * (size_t)m->capt);
        if (!m->t) die("out of memory for the surface");
    }
    m->t[3 * (size_t)m->nt] = a, m->t[3 * (size_t)m->nt + 1] = b, m->t[3 * (size_t)m->nt + 2] = c;
    m->nt++;
}

/* A field on cells (nx ny nz, cell (i, j, k) spanning [x0 + i h, x0 + (i + 1) h] and so on) and its isosurface at `iso`:
 * every node takes the mean of the eight cells around it, cells outside the grid at `floor`, and one layer of nodes
 * beyond the grid closes the surface; each cube is cut into the six tetrahedra around its main diagonal (Kuhn), so
 * neighbouring cubes agree on their shared faces and the surface is closed and consistently oriented by construction.
 * `vfield` (optional, on cells) is averaged to the nodes the same way, over the cells where `weight` (cells) is
 * positive, and interpolated to the vertices. */
typedef struct CellField {
    int nx, ny, nz;
    double h, x0, y0, z0;
    const float *val;    /* nx ny nz */
    const float *vfield; /* optional */
    double floor;
} CellField;

static void isosurface(const CellField *cf, double iso, Mesh *m) {
    memset(m, 0, sizeof *m);
    int PX = cf->nx + 3, PY = cf->ny + 3, PZ = cf->nz + 3; /* nodes, with one layer outside on each side */
    size_t NN = (size_t)PX * PY * PZ;
    double *nv = xmalloc(sizeof(double) * NN);
    float *nf = cf->vfield ? xmalloc(sizeof(float) * NN) : NULL;
    for (int K = 0; K < PZ; K++)
        for (int J = 0; J < PY; J++)
            for (int I = 0; I < PX; I++) {
                double s = 0, fs = 0, fw = 0;
                for (int c = 0; c < 8; c++) {
                    int i = I - 2 + (c & 1), j = J - 2 + ((c >> 1) & 1), k = K - 2 + ((c >> 2) & 1);
                    double v = cf->floor;
                    if (i >= 0 && j >= 0 && k >= 0 && i < cf->nx && j < cf->ny && k < cf->nz) {
                        size_t q = ((size_t)k * cf->ny + j) * cf->nx + i;
                        v = cf->val[q];
                        if (nf && v > 0.5) fs += cf->vfield[q] * v, fw += v;
                    }
                    s += v;
                }
                double a = s / 8.0;
                if (a == iso) a = iso + 1e-9; /* no vertex exactly on the level: every crossing is strict */
                nv[((size_t)K * PY + J) * PX + I] = a;
                if (nf) nf[((size_t)K * PY + J) * PX + I] = fw > 0 ? (float)(fs / fw) : 0.0f;
            }
    /* a vertex per crossed lattice edge: the edge from node n towards +x, +y, +z, +x+y, +x+z, +y+z, +x+y+z */
    int *ev = xmalloc(sizeof(int) * NN * 7);
    for (size_t q = 0; q < NN * 7; q++) ev[q] = -1;
    static const int dir_of_bits[8] = {-1, 0, 1, 3, 2, 4, 5, 6}; /* bits x=1 y=2 z=4 -> edge direction index */
    static const int perm[6][3] = {{1, 2, 4}, {1, 4, 2}, {2, 1, 4}, {2, 4, 1}, {4, 1, 2}, {4, 2, 1}};
    for (int K = 0; K + 1 < PZ; K++)
        for (int J = 0; J + 1 < PY; J++)
            for (int I = 0; I + 1 < PX; I++) {
                size_t base = ((size_t)K * PY + J) * PX + I;
                double cv[8];
                int any_in = 0, any_out = 0;
                for (int b = 0; b < 8; b++) {
                    cv[b] = nv[base + (size_t)(b & 1) + (size_t)((b >> 1) & 1) * PX + (size_t)((b >> 2) & 1) * PX * PY];
                    if (cv[b] > iso) any_in = 1;
                    else any_out = 1;
                }
                if (!any_in || !any_out) continue;
                for (int t = 0; t < 6; t++) {
                    int tv[4] = {0, perm[t][0], perm[t][0] | perm[t][1], 7};
                    int in[4], nin = 0;
                    for (int q = 0; q < 4; q++) nin += in[q] = cv[tv[q]] > iso;
                    if (nin == 0 || nin == 4) continue;
                    /* the vertex on the tetrahedron's edge between corners a and b (one contains the other's bits) */
                    int vid[4][4];
                    for (int q = 0; q < 4; q++)
                        for (int r = q + 1; r < 4; r++) {
                            vid[q][r] = vid[r][q] = -1;
                            if (in[q] == in[r]) continue;
                            int lo = tv[q], hi = tv[r]; /* tv is increasing along the Kuhn path: lo's bits are in hi's */
                            size_t nlo = base + (size_t)(lo & 1) + (size_t)((lo >> 1) & 1) * PX + (size_t)((lo >> 2) & 1) * PX * PY;
                            size_t key = nlo * 7 + (size_t)dir_of_bits[hi ^ lo];
                            if (ev[key] < 0) {
                                double va = cv[lo], vb = cv[hi], s = (iso - va) / (vb - va), p[3];
                                int ci[3] = {I + (lo & 1), J + ((lo >> 1) & 1), K + ((lo >> 2) & 1)};
                                int cj[3] = {I + (hi & 1), J + ((hi >> 1) & 1), K + ((hi >> 2) & 1)};
                                double org[3] = {cf->x0, cf->y0, cf->z0};
                                for (int d = 0; d < 3; d++) p[d] = org[d] + ((ci[d] - 1) + s * (cj[d] - ci[d])) * cf->h;
                                float fv = 0;
                                if (nf) {
                                    size_t nhi = base + (size_t)(hi & 1) + (size_t)((hi >> 1) & 1) * PX + (size_t)((hi >> 2) & 1) * PX * PY;
                                    fv = (float)(nf[nlo] + s * (nf[nhi] - nf[nlo]));
                                }
                                ev[key] = mesh_vertex(m, p, fv);
                            }
                            vid[q][r] = vid[r][q] = ev[key];
                        }
                    /* outward: from the inside corners towards the outside ones */
                    double gin[3] = {0, 0, 0}, gout[3] = {0, 0, 0};
                    for (int q = 0; q < 4; q++) {
                        double pc[3] = {(double)(tv[q] & 1), (double)((tv[q] >> 1) & 1), (double)((tv[q] >> 2) & 1)};
                        for (int d = 0; d < 3; d++) {
                            if (in[q]) gin[d] += pc[d] / nin;
                            else gout[d] += pc[d] / (4 - nin);
                        }
                    }
                    double g[3] = {gout[0] - gin[0], gout[1] - gin[1], gout[2] - gin[2]};
                    int tri[2][3], ntri = 0;
                    if (nin == 1 || nin == 3) {
                        int lone = -1;
                        for (int q = 0; q < 4; q++)
                            if ((nin == 1) == (in[q] == 1)) lone = q;
                        int k3 = 0;
                        for (int q = 0; q < 4; q++)
                            if (q != lone) tri[0][k3++] = vid[lone][q];
                        ntri = 1;
                    } else {
                        int i1 = -1, i2 = -1, o1 = -1, o2 = -1;
                        for (int q = 0; q < 4; q++) {
                            if (in[q]) { if (i1 < 0) i1 = q; else i2 = q; }
                            else { if (o1 < 0) o1 = q; else o2 = q; }
                        }
                        int a = vid[i1][o1], b = vid[i1][o2], c = vid[i2][o2], d = vid[i2][o1];
                        tri[0][0] = a, tri[0][1] = b, tri[0][2] = c;
                        tri[1][0] = a, tri[1][1] = c, tri[1][2] = d;
                        ntri = 2;
                    }
                    for (int q = 0; q < ntri; q++) {
                        const double *p0 = m->v + 3 * (size_t)tri[q][0], *p1 = m->v + 3 * (size_t)tri[q][1], *p2 = m->v + 3 * (size_t)tri[q][2];
                        double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
                        double nn3[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                        if (nn3[0] * g[0] + nn3[1] * g[1] + nn3[2] * g[2] < 0) mesh_tri(m, tri[q][0], tri[q][2], tri[q][1]);
                        else mesh_tri(m, tri[q][0], tri[q][1], tri[q][2]);
                    }
                }
            }
    free(ev);
    free(nv);
    free(nf);
}

/* vertex neighbours as a compressed row structure */
typedef struct Adj { int *start, *idx; } Adj;

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y);
}

static void mesh_adjacency(const Mesh *m, Adj *A) {
    size_t ne = 6 * (size_t)m->nt;
    uint64_t *e = xmalloc(sizeof(uint64_t) * ne);
    for (int t = 0; t < m->nt; t++)
        for (int q = 0; q < 3; q++) {
            uint64_t a = (uint64_t)m->t[3 * (size_t)t + q], b = (uint64_t)m->t[3 * (size_t)t + (q + 1) % 3];
            e[6 * (size_t)t + 2 * q] = a << 32 | b;
            e[6 * (size_t)t + 2 * q + 1] = b << 32 | a;
        }
    qsort(e, ne, sizeof(uint64_t), cmp_u64);
    size_t w = 0;
    for (size_t i = 0; i < ne; i++)
        if (i == 0 || e[i] != e[i - 1]) e[w++] = e[i];
    A->start = xcalloc((size_t)m->nv + 1, sizeof(int));
    A->idx = xmalloc(sizeof(int) * (w ? w : 1));
    for (size_t i = 0; i < w; i++) A->start[(e[i] >> 32) + 1]++;
    for (int v = 0; v < m->nv; v++) A->start[v + 1] += A->start[v];
    for (size_t i = 0; i < w; i++) A->idx[i] = (int)(e[i] & 0xffffffffu); /* sorted by the first vertex, so in order */
    free(e);
}

/* Taubin's lambda|mu smoothing (Taubin 1995): a shrinking step and an inflating one, so the shape keeps its volume;
 * the umbrella operator with equal weights */
static void mesh_taubin(Mesh *m, int iterations, double lambda, double mu) {
    if (iterations <= 0 || m->nv == 0) return;
    Adj A;
    mesh_adjacency(m, &A);
    double *d = xmalloc(sizeof(double) * 3 * (size_t)m->nv);
    for (int it = 0; it < 2 * iterations; it++) {
        double f = (it & 1) ? mu : lambda;
        for (int v = 0; v < m->nv; v++) {
            int s = A.start[v], e = A.start[v + 1];
            double c[3] = {0, 0, 0};
            for (int q = s; q < e; q++)
                for (int k = 0; k < 3; k++) c[k] += m->v[3 * (size_t)A.idx[q] + k];
            for (int k = 0; k < 3; k++) d[3 * (size_t)v + k] = e > s ? c[k] / (e - s) - m->v[3 * (size_t)v + k] : 0;
        }
        for (size_t q = 0; q < 3 * (size_t)m->nv; q++) m->v[q] += f * d[q];
    }
    free(d);
    free(A.start);
    free(A.idx);
}

static void mesh_normals(Mesh *m) {
    free(m->n);
    m->n = xcalloc(3 * (size_t)m->nv, sizeof(double));
    for (int t = 0; t < m->nt; t++) {
        const int *q = m->t + 3 * (size_t)t;
        const double *p0 = m->v + 3 * (size_t)q[0], *p1 = m->v + 3 * (size_t)q[1], *p2 = m->v + 3 * (size_t)q[2];
        double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (int a = 0; a < 3; a++)
            for (int k = 0; k < 3; k++) m->n[3 * (size_t)q[a] + k] += n[k]; /* area-weighted */
    }
    for (int v = 0; v < m->nv; v++) {
        double *n = m->n + 3 * (size_t)v, l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0) n[0] /= l, n[1] /= l, n[2] /= l;
        else n[2] = 1;
    }
}

/* the surface's soundness: every edge in exactly two triangles, in opposite directions; Euler characteristic, pieces,
 * the enclosed volume (divergence theorem) and its area */
typedef struct MeshCheck {
    long long edges, open, nonmanifold, misoriented;
    int components;
    long long euler;
    double volume, area;
} MeshCheck;

static int uf_find(int *p, int x) {
    while (p[x] != x) p[x] = p[p[x]], x = p[x];
    return x;
}

static void mesh_check(const Mesh *m, MeshCheck *c) {
    memset(c, 0, sizeof *c);
    size_t ne = 3 * (size_t)m->nt;
    uint64_t *e = xmalloc(sizeof(uint64_t) * (ne ? ne : 1));
    for (int t = 0; t < m->nt; t++)
        for (int q = 0; q < 3; q++) {
            uint64_t a = (uint64_t)m->t[3 * (size_t)t + q], b = (uint64_t)m->t[3 * (size_t)t + (q + 1) % 3];
            /* undirected key in the high bits, the direction in the lowest */
            e[3 * (size_t)t + q] = a < b ? (a << 33 | b << 1) : (b << 33 | a << 1 | 1);
        }
    qsort(e, ne, sizeof(uint64_t), cmp_u64);
    for (size_t i = 0; i < ne;) {
        size_t j = i;
        while (j < ne && (e[j] >> 1) == (e[i] >> 1)) j++;
        c->edges++;
        if (j - i == 1) c->open++;
        else if (j - i > 2) c->nonmanifold++;
        else if ((e[i] & 1) == (e[i + 1] & 1)) c->misoriented++;
        i = j;
    }
    free(e);
    c->euler = (long long)m->nv - c->edges + m->nt;
    int *p = xmalloc(sizeof(int) * (size_t)(m->nv ? m->nv : 1));
    for (int v = 0; v < m->nv; v++) p[v] = v;
    for (int t = 0; t < m->nt; t++) {
        int a = uf_find(p, m->t[3 * (size_t)t]), b = uf_find(p, m->t[3 * (size_t)t + 1]), d = uf_find(p, m->t[3 * (size_t)t + 2]);
        if (a != b) p[b] = a;
        a = uf_find(p, a);
        if (a != d) p[d] = a;
    }
    unsigned char *used = xcalloc((size_t)(m->nv ? m->nv : 1), 1);
    for (int t = 0; t < m->nt; t++) used[m->t[3 * (size_t)t]] = 1;
    for (int v = 0; v < m->nv; v++)
        if (used[v] && uf_find(p, v) == v) c->components++;
    free(used);
    free(p);
    for (int t = 0; t < m->nt; t++) {
        const int *q = m->t + 3 * (size_t)t;
        const double *a = m->v + 3 * (size_t)q[0], *b = m->v + 3 * (size_t)q[1], *d = m->v + 3 * (size_t)q[2];
        c->volume += (a[0] * (b[1] * d[2] - b[2] * d[1]) - a[1] * (b[0] * d[2] - b[2] * d[0]) + a[2] * (b[0] * d[1] - b[1] * d[0])) / 6.0;
        double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
        double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        c->area += 0.5 * sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    }
}

/* binary PLY as tools/render/beauty.py reads it: float x y z nx ny nz value value01, triangles */
static bool mesh_write_ply(const Mesh *m, const char *file, const char *comment, double lo, double hi) {
    FILE *f = fopen(file, "wb");
    if (!f) return false;
    fprintf(f, "ply\nformat binary_little_endian 1.0\ncomment %s\nelement vertex %d\n", comment, m->nv);
    fprintf(f, "property float x\nproperty float y\nproperty float z\nproperty float nx\nproperty float ny\nproperty float nz\n");
    fprintf(f, "property float value\nproperty float value01\nelement face %d\nproperty list uchar int vertex_indices\nend_header\n", m->nt);
    for (int v = 0; v < m->nv; v++) {
        float r[8];
        for (int k = 0; k < 3; k++) r[k] = (float)m->v[3 * (size_t)v + k], r[3 + k] = (float)(m->n ? m->n[3 * (size_t)v + k] : 0);
        r[6] = m->f ? m->f[v] : 0.0f;
        double u = hi > lo ? (r[6] - lo) / (hi - lo) : 0;
        r[7] = (float)(u < 0 ? 0 : (u > 1 ? 1 : u));
        fwrite(r, sizeof r, 1, f);
    }
    for (int t = 0; t < m->nt; t++) {
        unsigned char k = 3;
        fwrite(&k, 1, 1, f);
        fwrite(m->t + 3 * (size_t)t, sizeof(int), 3, f);
    }
    bool ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

/* the cell field of the whole bracket from an element field of the computed grid (mirrored when it is half); cells
 * not meshed take `vacant` */
static float *whole_cells(const Grid *g, const float *elem, float vacant, int *ny_out, double *y0_out) {
    int ny = g->half ? 2 * g->ny : g->ny;
    float *c = xmalloc(sizeof(float) * (size_t)g->nx * ny * g->nz);
    for (int k = 0; k < g->nz; k++)
        for (int jj = 0; jj < ny; jj++)
            for (int i = 0; i < g->nx; i++) {
                int j = g->half ? (jj < g->ny ? g->ny - 1 - jj : jj - g->ny) : jj;
                int e = g->elem_of[gcell(g, i, j, k)];
                c[((size_t)k * ny + jj) * g->nx + i] = e >= 0 ? elem[e] : vacant;
            }
    *ny_out = ny;
    *y0_out = g->half ? g->y0 - g->ny * g->h : g->y0;
    return c;
}

static void design_surface(const Grid *g, const float *density, const float *vfield, double floor, int taubin, Mesh *m) {
    int ny;
    double y0;
    float *cells = whole_cells(g, density, (float)floor, &ny, &y0);
    float *vcells = vfield ? whole_cells(g, vfield, 0.0f, &ny, &y0) : NULL;
    CellField cf = {g->nx, ny, g->nz, g->h, g->x0, y0, g->z0, cells, vcells, floor};
    isosurface(&cf, 0.5, m);
    free(cells);
    free(vcells);
    mesh_taubin(m, taubin, 0.5, -0.53);
    mesh_normals(m);
}

/* ---- pictures for the owner's phone (the project's own software renderer) ------------------------------------------ */

static void story(const char *args) {
    const char *home = getenv("HOME");
    if (!home) return;
    char exe[1024], cmd[4096];
    snprintf(exe, sizeof exe, "%s/bin/story", home);
    if (access(exe, X_OK) != 0) return;
    snprintf(cmd, sizeof cmd, "'%s' %s > /dev/null 2>&1", exe, args);
    int rc = system(cmd);
    (void)rc;
}

typedef struct View {
    SwCamera cam;
    double light[3];
} View;

static void view_make_az(View *v, const Grid *g, int w, int h, double az_deg);
static void view_make(View *v, const Grid *g, int w, int h) { view_make_az(v, g, w, h, 0.0); }

static void view_make_az(View *v, const Grid *g, int w, int h, double az_deg) {
    double lo[3] = {g->x0, g->half ? g->y0 - g->ny * g->h : g->y0, g->z0};
    double hi[3] = {g->x0 + g->nx * g->h, g->y0 + g->ny * g->h, g->z0 + g->nz * g->h};
    double c[3] = {0.5 * (lo[0] + hi[0]), 0.5 * (lo[1] + hi[1]), 0.5 * (lo[2] + hi[2]) - 0.08 * (hi[2] - lo[2])};
    double r = 0.5 * sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
    double ca = cos(az_deg * M_PI / 180.0), sa = sin(az_deg * M_PI / 180.0);
    double dir[3] = {-0.62 * ca + 1.0 * sa, -0.62 * sa - 1.0 * ca, 0.62}, l = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    double eye[3] = {c[0] + 3.4 * r * dir[0] / l, c[1] + 3.4 * r * dir[1] / l, c[2] + 3.4 * r * dir[2] / l}, up[3] = {0, 0, 1};
    sw_camera_look(&v->cam, eye, c, up, 34.0, w, h);
    double li[3] = {-0.3 * ca + 0.55 * sa, -0.3 * sa - 0.55 * ca, 0.78}, ll = sqrt(li[0] * li[0] + li[1] * li[1] + li[2] * li[2]);
    for (int k = 0; k < 3; k++) v->light[k] = li[k] / ll;
}

/* a surface drawn as brushed titanium: a soft key light, a sky term and a rim, per vertex */
static void draw_mesh(SwImage *im, const View *v, const Mesh *m, const float base[3], int id, double alpha) {
    float *col = xmalloc(sizeof(float) * 3 * (size_t)(m->nv ? m->nv : 1));
    for (int q = 0; q < m->nv; q++) {
        const double *n = m->n + 3 * (size_t)q, *p = m->v + 3 * (size_t)q;
        double vd[3] = {v->cam.eye[0] - p[0], v->cam.eye[1] - p[1], v->cam.eye[2] - p[2]};
        double vl = sqrt(vd[0] * vd[0] + vd[1] * vd[1] + vd[2] * vd[2]);
        double nl = n[0] * v->light[0] + n[1] * v->light[1] + n[2] * v->light[2];
        double nv = (n[0] * vd[0] + n[1] * vd[1] + n[2] * vd[2]) / vl;
        if (nv < 0) nl = -nl, nv = -nv; /* two-sided */
        double hx = v->light[0] + vd[0] / vl, hy = v->light[1] + vd[1] / vl, hz = v->light[2] + vd[2] / vl;
        double hl = sqrt(hx * hx + hy * hy + hz * hz), nh = (n[0] * hx + n[1] * hy + n[2] * hz) / hl;
        double diff = 0.18 + 0.62 * fmax(0, nl) + 0.22 * (0.5 + 0.5 * n[2]), spec = 0.35 * pow(fmax(0, nh), 40), rim = 0.25 * pow(1 - nv, 3);
        for (int k = 0; k < 3; k++) {
            double c = base[k] * diff + spec + rim;
            col[3 * (size_t)q + k] = (float)(c > 1 ? 1 : c);
        }
    }
    for (int t = 0; t < m->nt; t++) {
        const int *q = m->t + 3 * (size_t)t;
        if (alpha < 1) {
            /* the design space as glass: a pale shade blended over what is behind (drawn after the solid parts) */
            double s = (col[3 * (size_t)q[0]] + col[3 * (size_t)q[1]] + col[3 * (size_t)q[2]]) / 3.0;
            sw_triangle_scalar_alpha(im, &v->cam, m->v + 3 * (size_t)q[0], m->v + 3 * (size_t)q[1], m->v + 3 * (size_t)q[2], s, s, s, 0.0,
                                     60.0, SW_CMAP_LAB_SOLID, false, id, alpha); /* the map's light metal end: neutral glass */
        } else {
            sw_triangle(im, &v->cam, m->v + 3 * (size_t)q[0], m->v + 3 * (size_t)q[1], m->v + 3 * (size_t)q[2], col + 3 * (size_t)q[0],
                        col + 3 * (size_t)q[1], col + 3 * (size_t)q[2], false, id);
        }
    }
    free(col);
}

static bool write_png(const SwImage *im, const char *file) {
    unsigned char *png = NULL;
    size_t len = 0;
    if (!sw_png(im, &png, &len)) return false;
    FILE *f = fopen(file, "wb");
    bool ok = f && fwrite(png, 1, len, f) == len;
    if (f) ok = (fclose(f) == 0) && ok;
    free(png);
    return ok;
}

static void picture_begin(SwImage *big) {
    sw_image_init(big, 1920, 1080);
    unsigned char top[3] = {226, 228, 232}, bottom[3] = {196, 199, 205};
    sw_clear(big, top, bottom);
}

static void picture_end(SwImage *big, const char *file, const char *line1, const char *line2) {
    SwImage im;
    sw_downsample(big, &im);
    unsigned char ink[3] = {43, 47, 54};
    sw_text(&im, 24, 20, 2, line1, ink);
    if (line2) sw_text(&im, 24, 48, 2, line2, ink);
    write_png(&im, file);
    sw_image_free(&im);
    sw_image_free(big);
}

/* supports (the bosses' faces on the mounting surface, clamped), the symmetry plane of a half model (held in y), the
 * load on the bores (cos(angle) from the load's line over the half the pin presses on), and the regions held solid;
 * returns the force on the computed model */
static double case_loads(const Grid *gp, const Bracket *b, unsigned char **fixed_out, double **force_out, unsigned char **held_out,
                         int *nclamped_out, int *nsym_out, int *nload_out) {
    const Grid g = *gp;
    unsigned char *fixed = xcalloc(3 * (size_t)g.nnodes, 1);
    double *force = xcalloc(3 * (size_t)g.nnodes, sizeof(double));
    int nclamped = 0, nsym = 0, nload = 0;
    double a = b->angle * M_PI / 180.0, d[3] = {sin(a), 0, cos(a)};
    double wsum = 0;
    double *w = xcalloc((size_t)g.nnodes, sizeof(double));
    for (int k = 0; k <= g.nz; k++)
        for (int j = 0; j <= g.ny; j++)
            for (int i = 0; i <= g.nx; i++) {
                int n = g.node_of[gnode(&g, i, j, k)];
                if (n < 0) continue;
                int boss = 0, lug = 0, pin = 0;
                for (int c = 0; c < 8; c++) {
                    int ci = i - 1 + (c & 1), cj = j - 1 + ((c >> 1) & 1), ck = k - 1 + ((c >> 2) & 1);
                    if (ci < 0 || cj < 0 || ck < 0 || ci >= g.nx || cj >= g.ny || ck >= g.nz) continue;
                    int t = g.tag[gcell(&g, ci, cj, ck)];
                    boss |= t >= T_BOSS && t < T_BOSS + 4;
                    lug |= t == T_LUG;
                    pin |= t == T_PIN;
                }
                if (k == 0 && boss) { /* the boss's face on the mounting surface, clamped by the bolt */
                    fixed[3 * (size_t)n] = fixed[3 * (size_t)n + 1] = fixed[3 * (size_t)n + 2] = 1;
                    nclamped++;
                }
                if (g.half && j == 0 && !fixed[3 * (size_t)n + 1]) fixed[3 * (size_t)n + 1] = 1, nsym++;
                if (lug && pin) { /* the bore: the pin presses on the half that faces the load, as cos(angle) */
                    double rx = g.x0 + i * g.h, rz = g.z0 + k * g.h - b->pin_z * 1e-3, rl = hypot(rx, rz);
                    double c = rl > 0 ? (rx * d[0] + rz * d[2]) / rl : 0;
                    if (c > 0) w[n] = c, wsum += c, nload++;
                }
            }
    if (!(wsum > 0) || nclamped == 0) die("the case has no loaded or no clamped nodes");
    double fcase = g.half ? 0.5 * b->force : b->force;
    for (int n = 0; n < g.nnodes; n++)
        if (w[n] > 0)
            for (int k = 0; k < 3; k++) force[3 * (size_t)n + k] = fcase * w[n] / wsum * d[k];
    free(w);
    unsigned char *held = xcalloc((size_t)g.nelems, 1);
    for (int e = 0; e < g.nelems; e++) held[e] = g.cls[g.cell_of[e]] == C_SOLID;
    *fixed_out = fixed, *force_out = force, *held_out = held;
    *nclamped_out = nclamped, *nsym_out = nsym, *nload_out = nload;
    return fcase;
}

/* ---- run ---------------------------------------------------------------------------------------------------------- */

typedef struct RunCtx {
    const Grid *g;
    const Opts *o;
    const char *out;
    FILE *hist;
    double t0, last_story, emin, penalty;
    double max_true_res, max_eq_err, solve_s;
    long long pcg_total;
    int nsnap, max_pcg;
    double c_target;   /* compliance of the first field at the target volume */
    int it_target;
    float *fbuf;
    double vm_max;
} RunCtx;

static double von_mises(const double s[6]) {
    double a = s[0] - s[1], b = s[1] - s[2], c = s[2] - s[0];
    return sqrt(0.5 * (a * a + b * b + c * c) + 3.0 * (s[3] * s[3] + s[4] * s[4] + s[5] * s[5]));
}

static void progress_picture(RunCtx *rc, const double *x, int iter, double vf, double comp) {
    const Grid *g = rc->g;
    for (int e = 0; e < g->nelems; e++) rc->fbuf[e] = (float)x[e];
    Mesh m;
    design_surface(g, rc->fbuf, NULL, 1e-3, 8, &m);
    SwImage big;
    picture_begin(&big);
    View v;
    view_make(&v, g, 1920, 1080);
    float ti[3] = {0.66f, 0.67f, 0.70f};
    draw_mesh(&big, &v, &m, ti, 1, 1.0);
    char file[1024], l1[160], l2[200], args[1400];
    path(file, sizeof file, rc->out, "progress.png");
    snprintf(l1, sizeof l1, "Iteration %d: %.1f %% of the design space, compliance %.4g J", iter, 100 * vf, comp);
    snprintf(l2, sizeof l2, "Ti-6Al-4V engine-mount bracket, %d voxels%s", g->half ? 2 * g->nelems : g->nelems,
             g->half ? " (half computed, mirrored)" : "");
    picture_end(&big, file, l1, l2);
    mesh_free(&m);
    snprintf(args, sizeof args, "frame '%s' --caption 'iteration %d, %.1f %% of the design space'", file, iter, 100 * vf);
    story(args);
    /* the carving takes (1 - vf) / step updates; then the run settles, usually within a few dozen, at most --iters */
    int est = rc->o->vstart > 0 ? (int)ceil((rc->o->vstart - rc->o->vf) / rc->o->vstep) + 40 : 120;
    if (est > rc->o->iters) est = rc->o->iters;
    if (est < iter + 1) est = iter + 1;
    snprintf(args, sizeof args, "progress %d %d --unit iterations --note '%.1f %% of the design space, compliance %.4g J'", iter, est,
             100 * vf, comp);
    story(args);
}

static void on_snapshot(void *ctx, const TopOptSnapshot *sn) {
    RunCtx *rc = ctx;
    const Grid *g = rc->g;
    char file[1024], name[64];
    snprintf(name, sizeof name, "snap/it_%04d.f32", sn->iter);
    path(file, sizeof file, rc->out, name);
    for (int e = 0; e < sn->nelems; e++) rc->fbuf[e] = (float)sn->density[e];
    FILE *f = fopen(file, "wb");
    if (!f || fwrite(rc->fbuf, sizeof(float), (size_t)sn->nelems, f) != (size_t)sn->nelems) die("cannot write %s", file);
    fclose(f);
    const SolveStats *st = &sn->solve->stats;
    rc->nsnap++;
    rc->pcg_total += st->iterations;
    if (st->iterations > rc->max_pcg) rc->max_pcg = st->iterations;
    if (st->true_residual > rc->max_true_res) rc->max_true_res = st->true_residual;
    if (sn->solve->equilibrium_error > rc->max_eq_err) rc->max_eq_err = sn->solve->equilibrium_error;
    rc->solve_s += st->seconds;
    if (rc->it_target < 0 && sn->target_volume_fraction <= rc->o->vf + 1e-12 && sn->iter > 0) {
        rc->it_target = sn->iter;
        rc->c_target = sn->compliance;
    }
    double wall = now_seconds() - rc->t0;
    fprintf(rc->hist, "%d,%d,%.10g,%.8f,%.8f,%.6f,%d,%.3e,%.3e,%.2f,%.1f\n", sn->iter, sn->final, sn->compliance, sn->volume_fraction,
            sn->target_volume_fraction, sn->change, st->iterations, st->true_residual, sn->solve->equilibrium_error, st->seconds, wall);
    fflush(rc->hist);
    printf("it %4d%s  c %.6g J  vol %.4f (aim %.4f)  change %.4f  %s %d it, residual %.1e, %.1f s  [%.0f s]\n", sn->iter,
           sn->final ? " final" : "", sn->compliance, sn->volume_fraction, sn->target_volume_fraction, sn->change, st->method,
           st->iterations, st->true_residual, st->seconds, wall);
    fflush(stdout);
    if (sn->final) { /* the stress of the final design: D0 eps (the solid's stress), the element mean of its von Mises */
        float *vm = xmalloc(sizeof(float) * (size_t)sn->nelems);
        rc->vm_max = 0;
        for (int e = 0; e < sn->nelems; e++) {
            double s = rc->emin + pow(sn->density[e], rc->penalty) * (1 - rc->emin), acc = 0;
            for (int q = 0; q < 8; q++) acc += von_mises(sn->solve->gp_stress + 48 * (size_t)e + 6 * q);
            vm[e] = (float)(acc / 8.0 / s);
            if (sn->density[e] > 0.5 && vm[e] > rc->vm_max) rc->vm_max = vm[e];
        }
        path(file, sizeof file, rc->out, "final_vm.f32");
        f = fopen(file, "wb");
        if (!f || fwrite(vm, sizeof(float), (size_t)sn->nelems, f) != (size_t)sn->nelems) die("cannot write %s", file);
        fclose(f);
        path(file, sizeof file, rc->out, "final.f32");
        f = fopen(file, "wb");
        if (!f || fwrite(rc->fbuf, sizeof(float), (size_t)sn->nelems, f) != (size_t)sn->nelems) die("cannot write %s", file);
        fclose(f);
        free(vm);
    }
    if (rc->o->story && (sn->final || now_seconds() - rc->last_story > 60.0 * rc->o->story_minutes)) {
        rc->last_story = now_seconds();
        progress_picture(rc, sn->density, sn->iter, sn->volume_fraction, sn->compliance);
    }
    (void)g;
}

/* face-connected pieces of the elements above half density; for the half model a piece reaches its mirror image only
 * through the plane y = 0 */
typedef struct Pieces {
    int count, largest, touches_plane, holds_all;
    int sizes[8];
} Pieces;

static void pieces(const Grid *g, const double *x, Pieces *pc) {
    memset(pc, 0, sizeof *pc);
    size_t nc = (size_t)g->nx * g->ny * g->nz;
    int *p = xmalloc(sizeof(int) * nc);
    for (size_t c = 0; c < nc; c++) p[c] = (int)c;
    unsigned char *on = xcalloc(nc, 1);
    for (int e = 0; e < g->nelems; e++) on[g->cell_of[e]] = x[e] > 0.5;
    for (int k = 0; k < g->nz; k++)
        for (int j = 0; j < g->ny; j++)
            for (int i = 0; i < g->nx; i++) {
                size_t c = gcell(g, i, j, k);
                if (!on[c]) continue;
                size_t nb[3] = {i + 1 < g->nx ? gcell(g, i + 1, j, k) : c, j + 1 < g->ny ? gcell(g, i, j + 1, k) : c,
                                k + 1 < g->nz ? gcell(g, i, j, k + 1) : c};
                for (int q = 0; q < 3; q++)
                    if (nb[q] != c && on[nb[q]]) {
                        int a = uf_find(p, (int)c), b = uf_find(p, (int)nb[q]);
                        if (a != b) p[b] = a;
                    }
            }
    int *size = xcalloc(nc, sizeof(int));
    for (size_t c = 0; c < nc; c++)
        if (on[c]) size[uf_find(p, (int)c)]++;
    int best = -1;
    for (size_t c = 0; c < nc; c++)
        if (on[c] && p[c] == (int)c) {
            if (pc->count < 8) pc->sizes[pc->count] = size[c];
            pc->count++;
            if (best < 0 || size[c] > size[best]) best = (int)c;
        }
    pc->largest = best >= 0 ? size[best] : 0;
    /* the largest piece: does it hold every boss and the lug, and (half model) reach the plane? */
    int has[8] = {0};
    for (int k = 0; k < g->nz; k++)
        for (int j = 0; j < g->ny; j++)
            for (int i = 0; i < g->nx; i++) {
                size_t c = gcell(g, i, j, k);
                if (!on[c] || uf_find(p, (int)c) != best) continue;
                if (g->tag[c] < 8) has[g->tag[c]] = 1;
                if (j == 0) pc->touches_plane = 1;
            }
    /* which bosses this grid holds at all */
    int present[8] = {0};
    for (size_t c = 0; c < nc; c++)
        if (g->cls[c] == C_SOLID && g->tag[c] < 8) present[g->tag[c]] = 1;
    pc->holds_all = 1;
    for (int t = T_BOSS; t <= T_LUG; t++)
        if (present[t] && !has[t]) pc->holds_all = 0;
    free(size);
    free(on);
    free(p);
}

static void setup_picture(const Grid *g, const Bracket *b, const char *file) {
    size_t nc = (size_t)g->nx * g->ny * g->nz;
    float *dom = xmalloc(sizeof(float) * (size_t)g->nelems), *sol = xmalloc(sizeof(float) * (size_t)g->nelems);
    for (int e = 0; e < g->nelems; e++) {
        dom[e] = 1.0f;
        sol[e] = g->cls[g->cell_of[e]] == C_SOLID ? 1.0f : 1e-3f;
    }
    (void)nc;
    Mesh md, ms;
    design_surface(g, dom, NULL, 1e-3, 4, &md);
    design_surface(g, sol, NULL, 1e-3, 4, &ms);
    SwImage big;
    picture_begin(&big);
    View v;
    view_make(&v, g, 1920, 1080);
    float ti[3] = {0.55f, 0.57f, 0.61f};
    draw_mesh(&big, &v, &ms, ti, 1, 1.0);
    /* the load: an arrow from the pin along its line; the clamps: a short stroke under each boss */
    double a = b->angle * M_PI / 180.0, L = 0.045;
    double p0[3] = {0, 0, b->pin_z * 1e-3}, p1[3] = {L * sin(a), 0, b->pin_z * 1e-3 + L * cos(a)};
    unsigned char red[3] = {200, 40, 30}, blue[3] = {40, 80, 170};
    sw_line(&big, &v.cam, p0, p1, red, 0.05, 7);
    double side[3] = {cos(a), 0, -sin(a)};
    for (int s = -1; s <= 1; s += 2) {
        double q[3] = {p1[0] - 0.012 * sin(a) + s * 0.006 * side[0], 0, p1[2] - 0.012 * cos(a) + s * 0.006 * side[2]};
        sw_line(&big, &v.cam, p1, q, red, 0.05, 7);
    }
    for (int s = 0; s < 4; s++) {
        double bx = ((s & 1) ? b->bolt_x : -b->bolt_x) * 1e-3, by = ((s & 2) ? b->bolt_y : -b->bolt_y) * 1e-3;
        for (int k = 0; k < 12; k++) {
            double t0 = 2 * M_PI * k / 12, r = b->boss_r * 1e-3;
            double q0[3] = {bx + r * cos(t0), by + r * sin(t0), -0.0005}, q1[3] = {bx + 0.8 * r * cos(t0), by + 0.8 * r * sin(t0), -0.006};
            sw_line(&big, &v.cam, q0, q1, blue, 0.05, 4);
        }
    }
    draw_mesh(&big, &v, &md, ti, 2, 0.22);
    char l1[200], l2[240];
    snprintf(l1, sizeof l1, "Design space %.0f x %.0f x %.0f mm, Ti-6Al-4V, %.2f million voxels of %.2f mm", b->len, b->wid, b->hgt,
             (g->half ? 2 * g->nelems : g->nelems) / 1e6, g->h * 1e3);
    snprintf(l2, sizeof l2, "%.0f kN at %.0f deg on the clevis pin (red), four bolts clamped (blue)", b->force / 1e3, b->angle);
    picture_end(&big, file, l1, l2);
    mesh_free(&md);
    mesh_free(&ms);
    free(dom);
    free(sol);
}

static int cmd_run(int argc, char **argv) {
    if (argc < 1) usage();
    const char *out = argv[0];
    Opts o;
    opts_default(&o);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--h") && v) o.h = atof(v), i++;
        else if (!strcmp(a, "--vf") && v) o.vf = atof(v), i++;
        else if (!strcmp(a, "--rmin") && v) o.rmin = atof(v), i++;
        else if (!strcmp(a, "--vstart") && v) o.vstart = atof(v), i++;
        else if (!strcmp(a, "--vstep") && v) o.vstep = atof(v), i++;
        else if (!strcmp(a, "--iters") && v) o.iters = atoi(v), i++;
        else if (!strcmp(a, "--tol") && v) o.tol = atof(v), i++;
        else if (!strcmp(a, "--emin") && v) o.emin = atof(v), i++;
        else if (!strcmp(a, "--change") && v) o.change_tol = atof(v), i++;
        else if (!strcmp(a, "--move") && v) o.move = atof(v), i++;
        else if (!strcmp(a, "--angle") && v) o.angle = atof(v), i++;
        else if (!strcmp(a, "--force") && v) o.force = atof(v), i++;
        else if (!strcmp(a, "--threads") && v) o.threads = atoi(v), i++;
        else if (!strcmp(a, "--warm") && v) o.warm = atoi(v), i++;
        else if (!strcmp(a, "--snap") && v) o.snap = atoi(v), i++;
        else if (!strcmp(a, "--story-minutes") && v) o.story_minutes = atoi(v), i++;
        else if (!strcmp(a, "--solver") && v) {
            if (!strcmp(v, "mg")) o.solver = SOLID_SOLVER_MG;
            else if (!strcmp(v, "pcg")) o.solver = SOLID_SOLVER_PCG;
            else if (!strcmp(v, "direct")) o.solver = SOLID_SOLVER_DIRECT;
            else if (!strcmp(v, "auto")) o.solver = SOLID_SOLVER_AUTO;
            else usage();
            i++;
        } else if (!strcmp(a, "--full")) o.half = 0;
        else if (!strcmp(a, "--story")) o.story = 1;
        else usage();
    }
    if (o.rmin <= 0) o.rmin = 2.5 * o.h;
    Bracket bk = BRACKET;
    if (o.angle >= 0) bk.angle = o.angle;
    if (o.force > 0) bk.force = o.force;
    const Bracket *b = &bk;
    double t_start = now_seconds();
    char file[1024];
    mkdir(out, 0755);
    path(file, sizeof file, out, "snap");
    mkdir(file, 0755);

    /* the material, from the library: never a number from memory */
    MaterialRecord mr;
    char err[512] = {0};
    if (!material_lookup(NULL, "ti6al4v_lpbf", &mr, err, sizeof err)) die("%s", err);
    SolidMaterial mat = {mat_eval(&mr.prop[MATP_E], 293.15), mat_eval(&mr.prop[MATP_NU], 293.15), mat_eval(&mr.prop[MATP_DENSITY], 293.15)};
    if (!(mat.E > 0) || !(mat.nu > 0)) die("the library's ti6al4v_lpbf has no modulus or Poisson's ratio");

    Grid g;
    grid_classify(&g, b, o.h, o.half);
    grid_mesh(&g);
    size_t nc = (size_t)g.nx * g.ny * g.nz;
    int nsolid = 0, ndesign = 0;
    for (size_t c = 0; c < nc; c++) nsolid += g.cls[c] == C_SOLID, ndesign += g.cls[c] == C_DESIGN;

    /* supports, symmetry and the load */
    unsigned char *fixed, *held;
    double *force;
    int nclamped, nsym, nload;
    double fcase = case_loads(&g, b, &fixed, &force, &held, &nclamped, &nsym, &nload);
    HexModel m = {0};
    m.nnodes = g.nnodes;
    m.nelems = g.nelems;
    m.xyz = g.xyz;
    m.conn = g.conn;
    m.nmat = 1;
    m.mat = &mat;
    m.formulation = HEX8_INCOMPATIBLE; /* the default of the static analysis: right in bending */
    m.elem_type = SOLID_ELEM_HEX8;
    SolidLoads loads = {0};
    loads.fixed = fixed;
    loads.nodal_force = force;

    printf("gendesign: the bracket at %.3g mm voxels: %d x %d x %d cells%s, %d elements (%d free, %d held solid), %d nodes\n", o.h,
           g.nx, g.ny, g.nz, g.half ? " (half, mirrored about y = 0)" : "", g.nelems, ndesign, nsolid, g.nnodes);
    printf("  material %s: E %.4g Pa, nu %.3g; %d clamped nodes, %d on the symmetry plane, %d loaded (%.4g N at %.0f deg)\n",
           mr.id, mat.E, mat.nu, nclamped, nsym, nload, fcase, b->angle);
    printf("  volume fraction %.3f, filter radius %.3g mm, continuation %s (start %.2f, step %.4f), warm start %d, tol %.0e, %d threads\n",
           o.vf, o.rmin, o.vstart > 0 ? "on" : "off", o.vstart, o.vstep, o.warm, o.tol, o.threads);
    fflush(stdout);

    /* the grid for the surfaces: case.txt and the class of every cell */
    path(file, sizeof file, out, "case.txt");
    FILE *f = fopen(file, "w");
    if (!f) die("cannot write %s", file);
    fprintf(f, "gendesign-grid 1\n%d %d %d\n%.17g %.17g %.17g %.17g\n%d\n%d\n", g.nx, g.ny, g.nz, g.h, g.x0, g.y0, g.z0, g.half, g.nelems);
    fprintf(f, "%.9g %.9g %.9g %.9g\n", b->pin_z * 1e-3, b->angle, b->lug_r * 1e-3, b->force); /* for the film's load arrow */
    fclose(f);
    path(file, sizeof file, out, "cells.u8");
    f = fopen(file, "wb");
    if (!f || fwrite(g.cls, 1, nc, f) != nc) die("cannot write %s", file);
    fclose(f);
    path(file, sizeof file, out, "tags.u8");
    f = fopen(file, "wb");
    if (!f || fwrite(g.tag, 1, nc, f) != nc) die("cannot write %s", file);
    fclose(f);
    path(file, sizeof file, out, "setup.png");
    setup_picture(&g, b, file);

    ThreadPool *pool = pool_create(o.threads);
    SolidOptions so = {0};
    so.solver = o.solver;
    so.pcg_tol = o.tol;
    so.pcg_max_iter = 200000;
    so.pool = pool;
    TopOptSettings s;
    topopt_settings_default(&s);
    s.volume_fraction = o.vf;
    s.penalty = 3.0;
    s.filter_radius = o.rmin * 1e-3;
    s.move_limit = o.move;
    s.x_min = 1e-3;
    s.e_min_ratio = o.emin;
    s.max_iter = o.iters;
    s.change_tol = o.change_tol;
    s.passive_layers = 0; /* the symmetry plane is held in y: a shell at supports would make it solid */
    s.passive_solid = held;
    s.warm_start = o.warm;
    if (o.vstart > 0) s.volume_start = o.vstart, s.volume_step = o.vstep;
    RunCtx rc = {0};
    rc.g = &g;
    rc.o = &o;
    rc.out = out;
    rc.t0 = now_seconds();
    rc.last_story = rc.t0;
    rc.emin = o.emin;
    rc.penalty = 3.0;
    rc.it_target = -1;
    rc.fbuf = xmalloc(sizeof(float) * (size_t)g.nelems);
    path(file, sizeof file, out, "history.csv");
    rc.hist = fopen(file, "w");
    if (!rc.hist) die("cannot write %s", file);
    fprintf(rc.hist, "iteration,final,compliance_j,volume_fraction,aim,change,solver_iterations,true_residual,equilibrium_error,solve_s,wall_s\n");
    if (o.snap > 0) {
        s.snapshot = on_snapshot;
        s.snapshot_ctx = &rc;
        s.snapshot_every = o.snap;
    }
    TopOptResult r;
    if (!topopt_run(&m, &loads, &so, &s, &r, err, sizeof err)) die("the optimisation failed: %s", err);
    fclose(rc.hist);

    /* the reference of criterion 6: the same volume spread evenly through the design space, each free element at the
     * stiffness its share of material would give unpenalised (the held regions solid) */
    double *scale = xmalloc(sizeof(double) * (size_t)g.nelems);
    double vtot = (double)g.nelems, vheld = 0;
    for (int e = 0; e < g.nelems; e++) vheld += held[e];
    double xg = (o.vf * vtot - vheld) / (vtot - vheld);
    for (int e = 0; e < g.nelems; e++) scale[e] = held[e] ? 1.0 : xg;
    HexModel mg = m;
    mg.elem_scale = scale;
    SolidResult rg;
    double c_grey = NAN;
    if (solid_solve(&mg, &loads, &so, &rg, err, sizeof err)) {
        c_grey = rg.external_work;
        solid_result_free(&rg);
    } else {
        printf("gendesign: the uniform reference did not solve: %s\n", err);
    }
    free(scale);

    Pieces pc;
    pieces(&g, r.density, &pc);
    int above = 0;
    for (int e = 0; e < g.nelems; e++) above += r.density[e] > 0.5;
    double grey_share = 0;
    for (int e = 0; e < g.nelems; e++) grey_share += (r.density[e] > 0.1 && r.density[e] < 0.9);
    grey_share /= g.nelems;
    /* the history after the target volume was reached */
    double c_spread = 0;
    int last = r.iterations, nlast = 0;
    double cmax = -1, cmin = 1e300;
    for (int k = last - 10; k < last; k++)
        if (k >= 0 && k + 1 >= r.target_reached_at) {
            cmax = fmax(cmax, r.history[k].compliance), cmin = fmin(cmin, r.history[k].compliance), nlast++;
        }
    if (nlast > 1) c_spread = (cmax - cmin) / cmin;
    double vf_err = fabs(r.volume_fraction - o.vf) / o.vf;
    double total = now_seconds() - t_start;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double peak_gb = ru.ru_maxrss / 1048576.0; /* kilobytes on Linux */
    /* the criteria of tools/render/README.md, "Generative design" (7, the surfaces, is judged by `gendesign ply`) */
    int crit[9] = {0};
    crit[1] = rc.it_target >= 0 && r.compliance < rc.c_target && nlast >= 10 && c_spread < 1e-3;
    crit[2] = vf_err <= 0.005;
    crit[3] = r.energy_mismatch < 1e-6;
    crit[4] = pc.count == 1 && pc.holds_all && (!g.half || pc.touches_plane);
    crit[5] = rc.max_true_res <= 1e-9 && rc.nsnap == r.solves;
    crit[6] = r.compliance <= 0.5 * c_grey;
    crit[8] = total <= 12 * 3600.0 && peak_gb < 20.0;

    printf("\ngendesign: %d iterations, stopped: %s (largest change %.4f); target volume reached at update %d\n", r.iterations,
           r.stop_reason, r.change, r.target_reached_at);
    printf("  compliance: solid design space %.6g J, at the target %.6g J (iteration %d), final %.6g J; the same volume spread evenly %.6g J\n",
           r.compliance_initial, rc.c_target, rc.it_target, r.compliance, c_grey);
    printf("  stiffness: %.2f times that of the same volume spread evenly, %.1f %% of the solid block's with %.1f %% of its material\n",
           c_grey / r.compliance, 100 * r.compliance_initial / r.compliance, 100 * r.volume_fraction);
    printf("  volume %.5f (target %.3f, %.3f %% off); f^T u against 2U at most %.2e; last 10 compliances within %.3f %%\n",
           r.volume_fraction, o.vf, 100 * vf_err, r.energy_mismatch, 100 * c_spread);
    printf("  above half density: %d elements in %d piece(s) (largest %d)%s%s; grey (0.1..0.9) %.2f %% of the elements\n", above,
           pc.count, pc.largest, pc.holds_all ? ", holding every boss and the lug" : ", NOT holding every boss and the lug",
           g.half ? (pc.touches_plane ? ", reaching the symmetry plane" : ", NOT reaching the symmetry plane") : "", 100 * grey_share);
    printf("  solves %d: %lld conjugate-gradient iterations (at most %d in one), %.0f s in the solver; worst true residual %.1e, "
           "worst equilibrium error %.1e\n",
           r.solves, r.solver_iterations, rc.max_pcg, r.solve_seconds, rc.max_true_res, rc.max_eq_err);
    printf("  largest von Mises stress in the final design %.4g MPa; run %.0f s (%.2f h), peak memory %.2f GB\n", rc.vm_max / 1e6, total,
           total / 3600, peak_gb);
    static const char *cname[9] = {"", "compliance falls and converges", "volume within 0.5 %", "energy check below 1e-6",
                                   "one part holding every boss and lug", "every solve converged", "twice as stiff as the even spread",
                                   "surfaces (gendesign ply)", "within 12 h and 20 GB"};
    for (int k = 1; k <= 8; k++)
        if (k != 7) printf("  criterion %d (%s): %s\n", k, cname[k], crit[k] ? "PASS" : "FAIL");

    path(file, sizeof file, out, "summary.json");
    f = fopen(file, "w");
    if (!f) die("cannot write %s", file);
    fprintf(f, "{\n  \"case\": \"engine-mount bracket, Ti-6Al-4V (ti6al4v_lpbf), %.0f kN at %.0f deg through a clevis, four bolts\",\n",
            b->force / 1e3, b->angle);
    fprintf(f, "  \"load_n\": %.6g, \"load_angle_deg\": %.6g,\n", b->force, b->angle);
    fprintf(f, "  \"voxel_mm\": %.6g, \"cells\": [%d, %d, %d], \"half_model\": %d, \"elements\": %d, \"elements_whole\": %d, \"nodes\": %d,\n", o.h,
            g.nx, g.ny, g.nz, g.half, g.nelems, g.half ? 2 * g.nelems : g.nelems, g.nnodes);
    fprintf(f, "  \"held_solid\": %d, \"free\": %d, \"clamped_nodes\": %d, \"loaded_nodes\": %d,\n", nsolid, ndesign, nclamped, nload);
    fprintf(f, "  \"material\": {\"id\": \"%s\", \"E_pa\": %.6g, \"nu\": %.6g},\n", mr.id, mat.E, mat.nu);
    fprintf(f, "  \"settings\": {\"volume_fraction\": %.6g, \"penalty\": 3, \"filter_radius_mm\": %.6g, \"move_limit\": %.3g, \"x_min\": 0.001, "
               "\"e_min_ratio\": %.3g, \"max_iter\": %d, \"change_tol\": %.4g, \"volume_start\": %.4g, \"volume_step\": %.6g, "
               "\"warm_start\": %d, \"pcg_tol\": %.3g, \"threads\": %d},\n",
            o.vf, o.rmin, o.move, o.emin, o.iters, o.change_tol, o.vstart, o.vstep, o.warm, o.tol, o.threads);
    fprintf(f, "  \"iterations\": %d, \"solves\": %d, \"stop_reason\": \"%s\", \"last_change\": %.6g, \"target_reached_at\": %d,\n",
            r.iterations, r.solves, r.stop_reason, r.change, r.target_reached_at);
    fprintf(f, "  \"compliance_solid_design_space_j\": %.10g, \"compliance_at_target_j\": %.10g, \"iteration_at_target\": %d, "
               "\"compliance_final_j\": %.10g, \"compliance_uniform_j\": %.10g,\n",
            r.compliance_initial, rc.c_target, rc.it_target, r.compliance, c_grey);
    fprintf(f, "  \"compliance_spread_last10\": %.6g, \"volume_fraction\": %.8f, \"volume_error_relative\": %.6g, \"energy_mismatch\": %.6g,\n",
            c_spread, r.volume_fraction, vf_err, r.energy_mismatch);
    fprintf(f, "  \"elements_above_half\": %d, \"pieces\": %d, \"largest_piece\": %d, \"holds_every_boss_and_lug\": %d, "
               "\"touches_symmetry_plane\": %d, \"grey_share\": %.6g,\n",
            above, pc.count, pc.largest, pc.holds_all, pc.touches_plane, grey_share);
    fprintf(f, "  \"solver_iterations\": %lld, \"max_solver_iterations\": %d, \"solve_seconds\": %.1f, \"max_true_residual\": %.3g, "
               "\"max_equilibrium_error\": %.3g,\n",
            r.solver_iterations, rc.max_pcg, r.solve_seconds, rc.max_true_res, rc.max_eq_err);
    fprintf(f, "  \"von_mises_max_mpa\": %.6g, \"seconds\": %.1f, \"peak_memory_gb\": %.3f,\n", rc.vm_max / 1e6, total, peak_gb);
    fprintf(f, "  \"criteria\": {\"1_compliance_falls_and_converges\": %d, \"2_volume\": %d, \"3_energy\": %d, \"4_one_part\": %d, "
               "\"5_solves_converged\": %d, \"6_stiffer_than_even_spread\": %d, \"8_budget\": %d}\n}\n",
            crit[1], crit[2], crit[3], crit[4], crit[5], crit[6], crit[8]);
    fclose(f);
    pool_destroy(pool);
    topopt_result_free(&r);
    return 0;
}

/* ---- ply ---------------------------------------------------------------------------------------------------------- */

static int cmpf(const void *p, const void *q) {
    float a = *(const float *)p, b = *(const float *)q;
    return a < b ? -1 : (a > b);
}

typedef struct CaseInfo { double pin_z, angle, lug_r, force; int target_reached_at; } CaseInfo;

static void grid_load(Grid *g, CaseInfo *ci, const char *out) {
    memset(g, 0, sizeof *g);
    memset(ci, 0, sizeof *ci);
    char file[1024];
    path(file, sizeof file, out, "case.txt");
    FILE *f = fopen(file, "r");
    if (!f) die("cannot read %s", file);
    char magic[64];
    int ver;
    if (fscanf(f, "%63s %d %d %d %d %lf %lf %lf %lf %d %d", magic, &ver, &g->nx, &g->ny, &g->nz, &g->h, &g->x0, &g->y0, &g->z0, &g->half,
               &g->nelems) != 11 || strcmp(magic, "gendesign-grid"))
        die("%s is not a gendesign grid", file);
    if (fscanf(f, "%lf %lf %lf %lf", &ci->pin_z, &ci->angle, &ci->lug_r, &ci->force) != 4) ci->pin_z = ci->angle = ci->lug_r = ci->force = 0;
    fclose(f);
    /* the update at which the run reached its target volume, from its summary (the film slows down after it) */
    path(file, sizeof file, out, "summary.json");
    f = fopen(file, "r");
    if (f) {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        const char *k = strstr(buf, "\"target_reached_at\": ");
        if (k) ci->target_reached_at = atoi(k + strlen("\"target_reached_at\": "));
    }
    size_t nc = (size_t)g->nx * g->ny * g->nz;
    g->cls = xmalloc(nc);
    g->tag = xcalloc(nc, 1);
    path(file, sizeof file, out, "cells.u8");
    f = fopen(file, "rb");
    if (!f || fread(g->cls, 1, nc, f) != nc) die("cannot read %s", file);
    fclose(f);
    path(file, sizeof file, out, "tags.u8");
    f = fopen(file, "rb");
    if (f) {
        if (fread(g->tag, 1, nc, f) != nc) die("cannot read %s", file);
        fclose(f);
    }
    g->elem_of = xmalloc(sizeof(int) * nc);
    int ne = 0;
    for (size_t c = 0; c < nc; c++) g->elem_of[c] = g->cls[c] != C_VOID ? ne++ : -1;
    if (ne != g->nelems) die("%s: %d meshed cells, the case says %d", out, ne, g->nelems);
    g->cell_of = xmalloc(sizeof(int) * (size_t)ne);
    for (size_t c = 0; c < nc; c++)
        if (g->elem_of[c] >= 0) g->cell_of[g->elem_of[c]] = (int)c;
}

static bool read_floats(const char *file, float *v, int n) {
    FILE *f = fopen(file, "rb");
    if (!f) return false;
    bool ok = fread(v, sizeof(float), (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static void report_mesh(FILE *man, const char *name, int iter, const Mesh *m, const MeshCheck *c, double voxel_volume, bool *first) {
    fprintf(man, "%s    {\"name\": \"%s\", \"iteration\": %d, \"file\": \"%s.ply\", \"vertices\": %d, \"triangles\": %d, \"open_edges\": %lld, "
                 "\"nonmanifold_edges\": %lld, \"misoriented_edges\": %lld, \"pieces\": %d, \"euler\": %lld, \"volume_m3\": %.9g, "
                 "\"voxel_volume_m3\": %.9g, \"area_m2\": %.9g}",
            *first ? "" : ",\n", name, iter, name, m->nv, m->nt, c->open, c->nonmanifold, c->misoriented, c->components, c->euler, c->volume,
            voxel_volume, c->area);
    *first = false;
}

/* the surfaces of a run: the design space and its held regions (iteration -1), the kept iterations the film shows,
 * and the final design with its stress (iteration -2) */
typedef struct SurfJob { char name[32]; int iter; } SurfJob;

static int cmd_ply(int argc, char **argv) {
    if (argc < 2) usage();
    const char *out = argv[0], *dst = argv[1], *only = NULL;
    int every = 1, from = 0, to = 99999, taubin = 12, domain = 0, final = 0, dense_to = -1, sparse = 1, check_only = 0, plan_to = -1;
    for (int i = 2; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--every") && v) every = atoi(v), i++;
        else if (!strcmp(argv[i], "--dense-to") && v) dense_to = atoi(v), i++;
        else if (!strcmp(argv[i], "--sparse") && v) sparse = atoi(v) > 0 ? atoi(v) : 1, i++;
        else if (!strcmp(argv[i], "--from") && v) from = atoi(v), i++;
        else if (!strcmp(argv[i], "--to") && v) to = atoi(v), i++;
        else if (!strcmp(argv[i], "--taubin") && v) taubin = atoi(v), i++;
        else if (!strcmp(argv[i], "--only") && v) only = v, i++;
        else if (!strcmp(argv[i], "--domain")) domain = 1;
        else if (!strcmp(argv[i], "--final")) final = 1;
        else if (!strcmp(argv[i], "--check-only")) check_only = 1;
        else if (!strcmp(argv[i], "--plan-to") && v) plan_to = atoi(v), i++;
        else usage();
    }
    Grid g;
    CaseInfo ci;
    grid_load(&g, &ci, out);
    mkdir(dst, 0755);
    char file[1024];
    if (plan_to >= 0) { /* a film planned before the run has ended: iterations 0..plan_to listed, nothing built or read */
        path(file, sizeof file, dst, "manifest.json");
        FILE *pm = fopen(file, "w");
        if (!pm) die("cannot write %s", file);
        double lo[3] = {g.x0, g.half ? g.y0 - g.ny * g.h : g.y0, g.z0}, hi[3] = {g.x0 + g.nx * g.h, g.y0 + g.ny * g.h, g.z0 + g.nz * g.h};
        fprintf(pm, "{\n  \"source\": \"%s\",\n  \"planned\": true,\n  \"bounds\": [[%.9g, %.9g, %.9g], [%.9g, %.9g, %.9g]],\n"
                    "  \"voxel_m\": %.9g,\n  \"half_model\": %d,\n  \"taubin\": %d,\n  \"target_reached_at\": %d,\n"
                    "  \"case\": {\"pin_z_m\": %.9g, \"angle_deg\": %.9g, \"arrow_start_m\": %.9g, \"force_n\": %.9g},\n"
                    "  \"surfaces\": [\n",
                out, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], g.h, g.half, taubin, ci.target_reached_at, ci.pin_z, ci.angle,
                ci.lug_r + 0.009, ci.force);
        for (int it = 0; it <= plan_to; it++)
            fprintf(pm, "%s    {\"name\": \"it_%04d\", \"iteration\": %d, \"file\": \"it_%04d.ply\", \"planned\": true}", it ? ",\n" : "",
                    it, it, it);
        fprintf(pm, "\n  ]\n}\n");
        fclose(pm);
        printf("gendesign ply: a planned manifest, iterations 0 to %d\n", plan_to);
        return 0;
    }
    /* the list of surfaces */
    int njob = 0, cap = 1024;
    SurfJob *job = xmalloc(sizeof(SurfJob) * (size_t)cap);
    if (domain) {
        snprintf(job[njob].name, sizeof job[0].name, "domain"), job[njob++].iter = -1;
        snprintf(job[njob].name, sizeof job[0].name, "held"), job[njob++].iter = -1;
    }
    int last_kept = -1; /* the run's last kept field: always among them */
    for (int it = from; it <= to; it++) {
        snprintf(file, sizeof file, "%s/snap/it_%04d.f32", out, it);
        if (access(file, R_OK) == 0) last_kept = it;
    }
    for (int it = from; it <= to; it += every) {
        /* the film's choice (tools/render/topofilm.py): every iteration up to dense_to, then every sparse-th, and the last */
        if (dense_to >= 0 && it > dense_to && (it - dense_to) % sparse != 0 && it != last_kept) continue;
        snprintf(file, sizeof file, "%s/snap/it_%04d.f32", out, it);
        if (access(file, R_OK) != 0) continue;
        if (njob == cap) {
            cap *= 2;
            job = realloc(job, sizeof(SurfJob) * (size_t)cap);
            if (!job) die("out of memory");
        }
        snprintf(job[njob].name, sizeof job[0].name, "it_%04d", it), job[njob++].iter = it;
    }
    if (final) snprintf(job[njob].name, sizeof job[0].name, "final_vm"), job[njob++].iter = -2;
    /* the stress colours of the final design: the 0.5 and 99.5 percentiles over the elements above half density */
    float *x = xmalloc(sizeof(float) * (size_t)g.nelems), *vm = NULL;
    double vm_lo = 0, vm_hi = 0;
    if (final) {
        vm = xmalloc(sizeof(float) * (size_t)g.nelems);
        snprintf(file, sizeof file, "%s/final.f32", out);
        if (!read_floats(file, x, g.nelems)) die("cannot read %s", file);
        snprintf(file, sizeof file, "%s/final_vm.f32", out);
        if (!read_floats(file, vm, g.nelems)) die("cannot read %s", file);
        int na = 0;
        float *srt = xmalloc(sizeof(float) * (size_t)g.nelems);
        for (int e = 0; e < g.nelems; e++)
            if (x[e] > 0.5) srt[na++] = vm[e];
        qsort(srt, (size_t)na, sizeof(float), cmpf);
        vm_lo = na ? srt[(size_t)(0.005 * (na - 1))] : 0;
        vm_hi = na ? srt[(size_t)(0.995 * (na - 1))] : 1;
        free(srt);
    }
    FILE *man = NULL;
    if (!only) {
        path(file, sizeof file, dst, "manifest.json");
        man = fopen(file, "w");
        if (!man) die("cannot write %s", file);
        double lo[3] = {g.x0, g.half ? g.y0 - g.ny * g.h : g.y0, g.z0}, hi[3] = {g.x0 + g.nx * g.h, g.y0 + g.ny * g.h, g.z0 + g.nz * g.h};
        fprintf(man, "{\n  \"source\": \"%s\",\n  \"bounds\": [[%.9g, %.9g, %.9g], [%.9g, %.9g, %.9g]],\n  \"voxel_m\": %.9g,\n  \"half_model\": %d,\n"
                     "  \"taubin\": %d,\n  \"target_reached_at\": %d,\n  \"dense_to\": %d,\n  \"sparse\": %d,\n  \"files_written\": %d,\n"
                     "  \"case\": {\"pin_z_m\": %.9g, \"angle_deg\": %.9g, \"arrow_start_m\": %.9g, \"force_n\": %.9g},\n"
                     "  \"stress_range_pa\": [%.6g, %.6g],\n  \"surfaces\": [\n",
                out, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], g.h, g.half, taubin, ci.target_reached_at, dense_to, sparse, !check_only,
                ci.pin_z, ci.angle, ci.lug_r + 0.009, ci.force, vm_lo, vm_hi);
    }
    bool first = true;
    double vox = g.h * g.h * g.h * (g.half ? 2 : 1);
    long long bad = 0;
    int done = 0;
    for (int j = 0; j < njob; j++) {
        if (only && strcmp(job[j].name, only)) continue;
        const char *name = job[j].name;
        const float *field = NULL;
        double floor = 1e-3, lo = 0, hi = 1;
        char comment[160];
        if (job[j].iter == -1) { /* the design space as the engineer gives it, or the regions held solid */
            bool held = !strcmp(name, "held");
            for (int e = 0; e < g.nelems; e++) x[e] = !held || g.cls[g.cell_of[e]] == C_SOLID ? 1.0f : 1e-3f;
            snprintf(comment, sizeof comment, "OpenPhysicsAI gendesign: %s", held ? "regions held solid" : "the design space");
        } else if (job[j].iter == -2) {
            snprintf(file, sizeof file, "%s/final.f32", out);
            if (!read_floats(file, x, g.nelems)) die("cannot read %s", file);
            field = vm, lo = vm_lo, hi = vm_hi;
            snprintf(comment, sizeof comment, "OpenPhysicsAI gendesign: the final design, von Mises stress (Pa)");
        } else {
            snprintf(file, sizeof file, "%s/snap/it_%04d.f32", out, job[j].iter);
            if (!read_floats(file, x, g.nelems)) die("cannot read %s", file);
            snprintf(comment, sizeof comment, "OpenPhysicsAI gendesign: iteration %d, density isosurface at 0.5", job[j].iter);
        }
        double vv = 0;
        for (int e = 0; e < g.nelems; e++) vv += (x[e] > 0.5f) * vox;
        Mesh m;
        design_surface(&g, x, field, floor, taubin, &m);
        MeshCheck c;
        mesh_check(&m, &c);
        if (!check_only) {
            snprintf(file, sizeof file, "%s/%s.ply", dst, name);
            if (!mesh_write_ply(&m, file, comment, lo, hi)) die("cannot write %s", file);
        }
        if (man) report_mesh(man, name, job[j].iter, &m, &c, vv, &first);
        bad += c.open + c.nonmanifold + c.misoriented;
        printf("%s: %d vertices, %d triangles, %lld open, %lld non-manifold, %lld misoriented edges, %d piece(s), Euler %lld, "
               "volume %.6g mm3 (voxels above 0.5: %.6g)\n",
               name, m.nv, m.nt, c.open, c.nonmanifold, c.misoriented, c.components, c.euler, c.volume * 1e9, vv * 1e9);
        fflush(stdout);
        mesh_free(&m);
        done++;
    }
    if (man) {
        fprintf(man, "\n  ]\n}\n");
        fclose(man);
    }
    if (only && !done) die("no surface named %s in this run", only);
    if (vm) printf("stress range %.4g to %.4g MPa (0.5 and 99.5 percentiles over the elements above half density)\n", vm_lo / 1e6, vm_hi / 1e6);
    free(x);
    free(vm);
    free(job);
    printf("gendesign ply: %d surfaces%s, %lld bad edges in all\n", done, check_only ? " checked (no files written)" : "", bad);
    return bad ? 1 : 0;
}

/* ---- pic: a quick look at a kept design from the front and from behind (the project's software renderer) --------- */

static int cmd_pic(int argc, char **argv) {
    if (argc < 2) usage();
    const char *out = argv[0], *png = argv[1];
    int iter = -1;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--iter") && i + 1 < argc) iter = atoi(argv[++i]);
        else usage();
    Grid g;
    CaseInfo ci;
    grid_load(&g, &ci, out);
    float *x = xmalloc(sizeof(float) * (size_t)g.nelems);
    char file[1024];
    if (iter >= 0) snprintf(file, sizeof file, "%s/snap/it_%04d.f32", out, iter);
    else snprintf(file, sizeof file, "%s/final.f32", out);
    if (!read_floats(file, x, g.nelems)) die("cannot read %s", file);
    Mesh m;
    design_surface(&g, x, NULL, 1e-3, 12, &m);
    SwImage big, a, b, both;
    float ti[3] = {0.66f, 0.67f, 0.70f};
    sw_image_init(&both, 1920, 540);
    for (int side = 0; side < 2; side++) {
        picture_begin(&big);
        View v;
        view_make_az(&v, &g, 1920, 1080, side ? 180.0 : 0.0);
        draw_mesh(&big, &v, &m, ti, 1, 1.0);
        sw_downsample(&big, side ? &b : &a);
        sw_image_free(&big);
    }
    for (int y = 0; y < 540; y++) {
        memcpy(both.rgb + 3 * (size_t)y * 1920, a.rgb + 3 * (size_t)y * 960, 3 * 960);
        memcpy(both.rgb + 3 * ((size_t)y * 1920 + 960), b.rgb + 3 * (size_t)y * 960, 3 * 960);
    }
    char l1[200];
    unsigned char ink[3] = {43, 47, 54};
    snprintf(l1, sizeof l1, "%s, %s: front-left and back-right", out, iter >= 0 ? "a kept iteration" : "the final design");
    sw_text(&both, 20, 16, 2, l1, ink);
    if (!write_png(&both, png)) die("cannot write %s", png);
    printf("%s: %d triangles\n", png, m.nt);
    mesh_free(&m);
    sw_image_free(&a);
    sw_image_free(&b);
    sw_image_free(&both);
    free(x);
    return 0;
}

/* ---- solve: one static solve of a kept design, to compare solvers on the real problem ---------------------------- */

static int cmd_solve(int argc, char **argv) {
    if (argc < 1) usage();
    const char *out = argv[0];
    int iter = 0, threads = 4;
    double tol = 1e-10, emin = 1e-9;
    SolidSolver solver = SOLID_SOLVER_MG;
    for (int i = 1; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--iter") && v) iter = atoi(v), i++;
        else if (!strcmp(argv[i], "--threads") && v) threads = atoi(v), i++;
        else if (!strcmp(argv[i], "--tol") && v) tol = atof(v), i++;
        else if (!strcmp(argv[i], "--emin") && v) emin = atof(v), i++;
        else if (!strcmp(argv[i], "--solver") && v) {
            solver = !strcmp(v, "pcg") ? SOLID_SOLVER_PCG : !strcmp(v, "direct") ? SOLID_SOLVER_DIRECT : SOLID_SOLVER_MG;
            i++;
        } else usage();
    }
    Grid gl;
    CaseInfo ci;
    grid_load(&gl, &ci, out);
    Bracket bk = BRACKET;
    if (ci.angle > 0) bk.angle = ci.angle;
    if (ci.force > 0) bk.force = ci.force;
    Grid g;
    grid_classify(&g, &bk, gl.h * 1e3, gl.half);
    grid_mesh(&g);
    if (g.nelems != gl.nelems) die("%s: the case rebuilds with %d elements, the run had %d", out, g.nelems, gl.nelems);
    unsigned char *fixed, *held;
    double *force;
    int nclamped, nsym, nload;
    case_loads(&g, &bk, &fixed, &force, &held, &nclamped, &nsym, &nload);
    MaterialRecord mr;
    char err[512] = {0};
    if (!material_lookup(NULL, "ti6al4v_lpbf", &mr, err, sizeof err)) die("%s", err);
    SolidMaterial mat = {mat_eval(&mr.prop[MATP_E], 293.15), mat_eval(&mr.prop[MATP_NU], 293.15), 0};
    float *x = xmalloc(sizeof(float) * (size_t)g.nelems);
    char file[1024];
    snprintf(file, sizeof file, "%s/snap/it_%04d.f32", out, iter);
    if (!read_floats(file, x, g.nelems)) die("cannot read %s", file);
    double *scale = xmalloc(sizeof(double) * (size_t)g.nelems);
    for (int e = 0; e < g.nelems; e++) scale[e] = emin + pow(x[e], 3.0) * (1 - emin);
    HexModel m = {0};
    m.nnodes = g.nnodes, m.nelems = g.nelems, m.xyz = g.xyz, m.conn = g.conn, m.nmat = 1, m.mat = &mat;
    m.formulation = HEX8_INCOMPATIBLE, m.elem_type = SOLID_ELEM_HEX8, m.elem_scale = scale;
    SolidLoads loads = {0};
    loads.fixed = fixed, loads.nodal_force = force;
    ThreadPool *pool = pool_create(threads);
    SolidOptions so = {0};
    so.solver = solver, so.pcg_tol = tol, so.pcg_max_iter = 200000, so.pool = pool;
    SolidResult res;
    double t0 = now_seconds();
    if (!solid_solve(&m, &loads, &so, &res, err, sizeof err)) die("the solve failed: %s", err);
    printf("solve %s iteration %d: %s, %d iterations, %.1f s in the solver, %.1f s in all; true residual %.2e; compliance %.10g J; "
           "f^T u against 2U %.2e\n",
           out, iter, res.stats.method, res.stats.iterations, res.stats.seconds, now_seconds() - t0, res.stats.true_residual,
           res.external_work, fabs(res.external_work - 2 * res.strain_energy) / res.external_work);
    solid_result_free(&res);
    pool_destroy(pool);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    if (!strcmp(argv[1], "solve")) return cmd_solve(argc - 2, argv + 2);
    if (!strcmp(argv[1], "run")) return cmd_run(argc - 2, argv + 2);
    if (!strcmp(argv[1], "ply")) return cmd_ply(argc - 2, argv + 2);
    if (!strcmp(argv[1], "pic")) return cmd_pic(argc - 2, argv + 2);
    usage();
    return 2;
}
