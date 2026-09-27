/* support_cell.c - support patterns and their homogenised properties (support_cell.h, docs/contracts/supports.md) */
#include "support_cell.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fem/hex8.h"
#include "../fem/solid.h"

static const char *NAMES[] = {"homogeneous", "block", "thin_wall", "cone", "tree", "lattice", "explicit"};

const char *support_type_name(SupportType t) { return (unsigned)t < 7 ? NAMES[t] : "unknown"; }

bool support_type_from_name(const char *name, SupportType *t) {
    for (int i = 0; i < 7; i++)
        if (name && !strcmp(name, NAMES[i])) {
            *t = (SupportType)i;
            return true;
        }
    return false;
}

double support_pitch(const SupportSpec *s) {
    switch (s->type) {
    case SUPPORT_BLOCK:
    case SUPPORT_THIN_WALL:
    case SUPPORT_CONE: return s->spacing;
    case SUPPORT_TREE: return s->trunk_spacing;
    case SUPPORT_LATTICE: return s->cell_size;
    default: return 1.0;
    }
}

bool support_prismatic(const SupportSpec *s) {
    return s->type == SUPPORT_BLOCK || s->type == SUPPORT_THIN_WALL || s->type == SUPPORT_LATTICE || s->type == SUPPORT_HOMOGENEOUS ||
           s->type == SUPPORT_EXPLICIT;
}

static double wrap(double a, double p) {
    a = fmod(a, p);
    return a < 0 ? a + p : a;
}

static bool tooth(const SupportSpec *s, double along) {
    return wrap(along, s->tooth_pitch) < s->contact_fraction * s->tooth_pitch;
}

/* distance from p to the segment a-b */
static double seg_dist(const double p[3], const double a[3], const double b[3]) {
    double ab[3], ap[3], L2 = 0, t = 0;
    for (int k = 0; k < 3; k++) ab[k] = b[k] - a[k], ap[k] = p[k] - a[k], L2 += ab[k] * ab[k], t += ap[k] * ab[k];
    t = L2 > 0 ? fmin(1, fmax(0, t / L2)) : 0;
    double d2 = 0;
    for (int k = 0; k < 3; k++) {
        double q = ap[k] - t * ab[k];
        d2 += q * q;
    }
    return sqrt(d2);
}

bool support_occupied(const SupportSpec *s, double x, double y, double d) {
    double P = support_pitch(s), c = 0.5 * P;
    x = wrap(x, P), y = wrap(y, P);
    bool teeth = s->tooth_height > 0 && s->contact_fraction < 1 && d < s->tooth_height;
    switch (s->type) {
    case SUPPORT_HOMOGENEOUS:
    case SUPPORT_EXPLICIT: return true;
    case SUPPORT_BLOCK: {
        bool wx = fabs(x - c) < 0.5 * s->wall_thickness; /* a wall along y (normal x) */
        bool wy = fabs(y - c) < 0.5 * s->wall_thickness; /* a wall along x (normal y) */
        if (!teeth) return wx || wy;
        return (wx && tooth(s, y)) || (wy && tooth(s, x));
    }
    case SUPPORT_THIN_WALL: {
        bool along_x = s->direction == 0;
        bool w = along_x ? fabs(y - c) < 0.5 * s->wall_thickness : fabs(x - c) < 0.5 * s->wall_thickness;
        return w && (!teeth || tooth(s, along_x ? x : y));
    }
    case SUPPORT_CONE: {
        double t = s->height > 0 ? fmin(1, fmax(0, d / s->height)) : 0;
        double r = s->top_radius + (s->base_radius - s->top_radius) * t;
        return (x - c) * (x - c) + (y - c) * (y - c) < r * r;
    }
    case SUPPORT_TREE: {
        if (d >= s->branch_height) return (x - c) * (x - c) + (y - c) * (y - c) < s->trunk_radius * s->trunk_radius;
        int n = (int)lround(P / s->spacing);
        if (n < 1) n = 1;
        double t = s->branch_height > 0 ? d / s->branch_height : 0; /* 0 at the tips, 1 at the trunk */
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                double tx = (i + 0.5) * P / n, ty = (j + 0.5) * P / n;
                double px = tx + (c - tx) * t, py = ty + (c - ty) * t;
                if ((x - px) * (x - px) + (y - py) * (y - py) < s->branch_radius * s->branch_radius) return true;
            }
        return false;
    }
    case SUPPORT_LATTICE: {
        double z = wrap(d, P), p[3] = {x, y, z}, ctr[3] = {c, c, c};
        for (int k = 0; k < 8; k++) {
            double a[3] = {(k & 1) ? P : 0, (k & 2) ? P : 0, (k & 4) ? P : 0};
            if (seg_dist(p, a, ctr) < 0.5 * s->strut_diameter) return true;
        }
        return false;
    }
    }
    return false;
}

static bool positive(double v, const char *name, char *err, size_t errlen) {
    if (v > 0 && isfinite(v)) return true;
    snprintf(err, errlen, "%s must be a positive length", name);
    return false;
}

bool support_spec_check(const SupportSpec *s, char *err, size_t errlen) {
    if (!(s->relative_density > 0 && s->relative_density <= 1)) {
        snprintf(err, errlen, "relative_density must lie in (0, 1]");
        return false;
    }
    if (s->tooth_height > 0 && !(s->contact_fraction > 0 && s->contact_fraction <= 1 && s->tooth_pitch > 0)) {
        snprintf(err, errlen, "a tooth interface needs tooth_pitch > 0 and contact_fraction in (0, 1]");
        return false;
    }
    switch (s->type) {
    case SUPPORT_EXPLICIT:
        if (!(s->region[3] > s->region[0] && s->region[4] > s->region[1] && s->region[5] > s->region[2])) {
            snprintf(err, errlen, "an explicit support needs region_mm [x0, y0, z0, x1, y1, z1] with x1 > x0, y1 > y0, z1 > z0");
            return false;
        }
        return true;
    case SUPPORT_HOMOGENEOUS:
        if (!(s->fraction > 0 && s->fraction <= 1)) {
            snprintf(err, errlen, "a homogeneous support needs a stiffness fraction in (0, 1]");
            return false;
        }
        return true;
    case SUPPORT_BLOCK:
    case SUPPORT_THIN_WALL:
        if (!positive(s->wall_thickness, "wall_thickness", err, errlen) || !positive(s->spacing, "spacing", err, errlen)) return false;
        if (s->wall_thickness >= s->spacing) {
            snprintf(err, errlen, "wall_thickness must be smaller than spacing (a wall as thick as its pitch is solid)");
            return false;
        }
        return true;
    case SUPPORT_CONE:
        if (!positive(s->base_radius, "base_radius", err, errlen) || !positive(s->top_radius, "top_radius", err, errlen) ||
            !positive(s->spacing, "spacing", err, errlen))
            return false;
        if (2 * fmax(s->base_radius, s->top_radius) > s->spacing) {
            snprintf(err, errlen, "cones wider than their spacing overlap: 2 x the larger radius must not exceed spacing");
            return false;
        }
        return true;
    case SUPPORT_TREE:
        if (!positive(s->trunk_radius, "trunk_radius", err, errlen) || !positive(s->branch_radius, "branch_radius", err, errlen) ||
            !positive(s->spacing, "spacing", err, errlen) || !positive(s->trunk_spacing, "trunk_spacing", err, errlen) ||
            !positive(s->branch_height, "branch_height", err, errlen))
            return false;
        if (s->spacing > s->trunk_spacing || 2 * s->trunk_radius > s->trunk_spacing) {
            snprintf(err, errlen, "tree: the tip spacing must not exceed trunk_spacing, and a trunk must fit in its pitch");
            return false;
        }
        return true;
    case SUPPORT_LATTICE:
        if (!positive(s->cell_size, "cell_size", err, errlen) || !positive(s->strut_diameter, "strut_diameter", err, errlen)) return false;
        if (s->strut_diameter >= 0.5 * s->cell_size) {
            snprintf(err, errlen, "lattice: strut_diameter must be below half the cell size");
            return false;
        }
        return true;
    }
    snprintf(err, errlen, "unknown support type");
    return false;
}

/* the thinnest feature of the type, for the default resolution */
static double min_feature(const SupportSpec *s, bool tooth_band) {
    double f = INFINITY;
    switch (s->type) {
    case SUPPORT_BLOCK:
    case SUPPORT_THIN_WALL: f = s->wall_thickness; break;
    case SUPPORT_CONE: f = 2 * fmin(s->base_radius, s->top_radius); break;
    case SUPPORT_TREE: f = 2 * fmin(s->branch_radius, s->trunk_radius); break;
    case SUPPORT_LATTICE: f = s->strut_diameter; break;
    default: f = support_pitch(s); break;
    }
    if (tooth_band) f = fmin(f, fmin(s->contact_fraction, 1 - s->contact_fraction > 0 ? 1 - s->contact_fraction : 1) * s->tooth_pitch);
    return f;
}

/* ---- the voxel cell ---- */

typedef struct Cell {
    int n, nz;          /* voxels per pitch, layers */
    double h, hz, P, H; /* voxel size in x y and z, pitch, slab height */
    unsigned char *occ; /* n * n * nz: the voxel holds material */
    double *phi;        /* n * n * nz: its solid volume fraction, 4 x 4 x 4 samples */
    int *region;        /* per voxel: root of its face-connected piece, -1 empty */
} Cell;

#define VOX(C, i, j, k) ((size_t)(k) * (size_t)(C)->n * (size_t)(C)->n + (size_t)(j) * (size_t)(C)->n + (size_t)(i))

static int uf_find(int *p, int a) {
    while (p[a] != a) a = p[a] = p[p[a]];
    return a;
}

static bool cell_build(Cell *C, const SupportSpec *s, double d0, double d1, int voxels) {
    bool tooth_band = s->tooth_height > 0 && s->contact_fraction < 1 && d0 < s->tooth_height;
    C->P = support_pitch(s);
    /* the default: four voxels across a wall or a cone, eight across an inclined strut (tree, lattice), where the
     * volume-fraction voxels converge within the 5 % of criterion S3 */
    double across = s->type == SUPPORT_TREE || s->type == SUPPORT_LATTICE ? 8 : 4;
    C->n = voxels > 0 ? voxels : (int)ceil(across * C->P / min_feature(s, tooth_band) - 1e-9);
    if (voxels <= 0) C->n = C->n < 8 ? 8 : (C->n > 64 ? 64 : C->n);
    C->h = C->P / C->n;
    if (s->type == SUPPORT_LATTICE) C->H = C->P, d0 = 0; /* one vertical period of the lattice */
    else if (support_prismatic(s) && !tooth_band) C->H = 2 * C->P, d0 = s->tooth_height + C->P; /* away from the teeth */
    else C->H = d1 - d0;
    C->nz = (int)lround(C->H / C->h);
    if (C->nz < 2) C->nz = 2;
    while ((size_t)C->n * C->n * C->nz > 600000 && C->nz > 2) C->nz--; /* memory guard */
    C->hz = C->H / C->nz;
    size_t nv = (size_t)C->n * C->n * C->nz;
    C->occ = calloc(nv, 1), C->region = malloc(nv * sizeof(int)), C->phi = calloc(nv, sizeof(double));
    if (!C->occ || !C->region || !C->phi) return false;
    /* the solid volume fraction of every voxel from 4 x 4 x 4 samples: a voxel's stiffness and conductance are scaled by
     * it, as Simufact scales its voxels (tutorial p. 72), so an inclined strut is not reduced to a staircase */
    enum { SS = 4 };
    for (int k = 0; k < C->nz; k++)
        for (int j = 0; j < C->n; j++)
            for (int i = 0; i < C->n; i++) {
                int hit = 0;
                for (int c = 0; c < SS; c++) {
                    double d = d0 + (C->nz - k - (c + 0.5) / SS) * C->hz; /* k = 0 is the lowest layer, deepest */
                    for (int b = 0; b < SS; b++)
                        for (int a = 0; a < SS; a++)
                            hit += support_occupied(s, (i + (a + 0.5) / SS) * C->h, (j + (b + 0.5) / SS) * C->h, d);
                }
                size_t v = VOX(C, i, j, k);
                C->phi[v] = hit / (double)(SS * SS * SS);
                C->occ[v] = hit > 0;
            }
    for (size_t v = 0; v < nv; v++) C->region[v] = C->occ[v] ? (int)v : -1;
    for (int k = 0; k < C->nz; k++)
        for (int j = 0; j < C->n; j++)
            for (int i = 0; i < C->n; i++) {
                size_t v = VOX(C, i, j, k);
                if (!C->occ[v]) continue;
                size_t nb[3] = {i + 1 < C->n ? VOX(C, i + 1, j, k) : SIZE_MAX, j + 1 < C->n ? VOX(C, i, j + 1, k) : SIZE_MAX,
                                k + 1 < C->nz ? VOX(C, i, j, k + 1) : SIZE_MAX};
                for (int q = 0; q < 3; q++)
                    if (nb[q] != SIZE_MAX && C->occ[nb[q]]) {
                        int a = uf_find(C->region, (int)v), b = uf_find(C->region, (int)nb[q]);
                        if (a != b) C->region[a] = b;
                    }
            }
    for (size_t v = 0; v < nv; v++)
        if (C->occ[v]) C->region[v] = uf_find(C->region, (int)v);
    return true;
}

static void cell_free(Cell *C) { free(C->occ), free(C->region), free(C->phi); }

/* the conductance between two partly filled voxels: the series (harmonic) mean of their fractions */
#define HM(a, b) (2.0 * (a) * (b) / ((a) + (b)))

/* finite-volume conduction across the cell along axis (0 x, 1 y, 2 z): the effective conductivity over the solid's */
static double conduction(const Cell *C, int axis) {
    unsigned char *ok = NULL;
    size_t nv = (size_t)C->n * C->n * C->nz;
    /* region roots touching both faces normal to axis */
    unsigned char *lo = calloc(nv, 1), *hi = calloc(nv, 1);
    ok = calloc(nv, 1);
    double *x = calloc(nv, sizeof(double)), *r = calloc(nv, sizeof(double)), *p = calloc(nv, sizeof(double)),
           *q = calloc(nv, sizeof(double)), *dg = calloc(nv, sizeof(double));
    int *id = malloc(nv * sizeof(int));
    double result = 0;
    if (!lo || !hi || !ok || !x || !r || !p || !q || !dg || !id) goto done;
    int dim[3] = {C->n, C->n, C->nz};
    double hh[3] = {C->h, C->h, C->hz};
    for (int k = 0; k < C->nz; k++)
        for (int j = 0; j < C->n; j++)
            for (int i = 0; i < C->n; i++) {
                size_t v = VOX(C, i, j, k);
                if (!C->occ[v]) continue;
                int c[3] = {i, j, k};
                if (c[axis] == 0) lo[C->region[v]] = 1;
                if (c[axis] == dim[axis] - 1) hi[C->region[v]] = 1;
            }
    for (size_t v = 0; v < nv; v++) ok[v] = lo[v] && hi[v];
    /* conductances (k = 1): between neighbours along a, area / distance; to a loaded face, area / half a voxel */
    double g[3], gb;
    for (int a = 0; a < 3; a++) g[a] = hh[(a + 1) % 3] * hh[(a + 2) % 3] / hh[a];
    gb = 2 * g[axis];
    /* unknowns: voxels of spanning pieces; the lower face at 0, the upper at 1: A x = b */
    for (size_t v = 0; v < nv; v++) id[v] = C->occ[v] && ok[C->region[v]] ? 1 : 0;
    double *b = r; /* b first, then the residual */
    for (int k = 0; k < C->nz; k++)
        for (int j = 0; j < C->n; j++)
            for (int i = 0; i < C->n; i++) {
                size_t v = VOX(C, i, j, k);
                if (!id[v]) continue;
                int c[3] = {i, j, k};
                double d = 0;
                for (int a = 0; a < 3; a++)
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        int cc[3] = {c[0], c[1], c[2]};
                        cc[a] += sgn;
                        if (cc[a] < 0 || cc[a] >= dim[a]) {
                            if (a == axis) {
                                d += gb * C->phi[v];
                                if (sgn > 0) b[v] += gb * C->phi[v];
                            }
                            continue; /* the other side faces are insulated */
                        }
                        size_t w = VOX(C, cc[0], cc[1], cc[2]);
                        if (id[w]) d += g[a] * HM(C->phi[v], C->phi[w]);
                    }
                dg[v] = d;
            }
    /* Jacobi-preconditioned conjugate gradients, matrix-free */
    double rz = 0, bnorm = 0;
    for (size_t v = 0; v < nv; v++)
        if (id[v]) p[v] = r[v] / dg[v], rz += r[v] * p[v], bnorm += r[v] * r[v];
    bnorm = sqrt(bnorm);
    for (int it = 0; it < 20000 && bnorm > 0; it++) {
        double pq = 0;
        for (int k = 0; k < C->nz; k++)
            for (int j = 0; j < C->n; j++)
                for (int i = 0; i < C->n; i++) {
                    size_t v = VOX(C, i, j, k);
                    if (!id[v]) continue;
                    int c[3] = {i, j, k};
                    double s = dg[v] * p[v];
                    for (int a = 0; a < 3; a++)
                        for (int sgn = -1; sgn <= 1; sgn += 2) {
                            int cc[3] = {c[0], c[1], c[2]};
                            cc[a] += sgn;
                            if (cc[a] < 0 || cc[a] >= dim[a]) continue;
                            size_t w = VOX(C, cc[0], cc[1], cc[2]);
                            if (id[w]) s -= g[a] * HM(C->phi[v], C->phi[w]) * p[w];
                        }
                    q[v] = s, pq += p[v] * s;
                }
        double alpha = rz / pq, rn = 0, rz1 = 0;
        for (size_t v = 0; v < nv; v++)
            if (id[v]) x[v] += alpha * p[v], r[v] -= alpha * q[v], rn += r[v] * r[v];
        if (sqrt(rn) < 1e-13 * bnorm) break;
        for (size_t v = 0; v < nv; v++)
            if (id[v]) rz1 += r[v] * r[v] / dg[v];
        double beta = rz1 / rz;
        rz = rz1;
        for (size_t v = 0; v < nv; v++)
            if (id[v]) p[v] = r[v] / dg[v] + beta * p[v];
    }
    /* heat through the upper face: gb (1 - T) summed over the voxels touching it */
    double Q = 0;
    for (int k = 0; k < C->nz; k++)
        for (int j = 0; j < C->n; j++)
            for (int i = 0; i < C->n; i++) {
                size_t v = VOX(C, i, j, k);
                int c[3] = {i, j, k};
                if (id[v] && c[axis] == dim[axis] - 1) Q += gb * C->phi[v] * (1.0 - x[v]);
            }
    double L = dim[axis] * hh[axis], A = 1;
    for (int a = 0; a < 3; a++)
        if (a != axis) A *= dim[a] * hh[a];
    result = Q * L / A; /* k_eff / k with a unit temperature difference */
done:
    free(lo), free(hi), free(ok), free(x), free(r), free(p), free(q), free(dg), free(id);
    return result;
}

/* The full stiffness of the cell by periodic homogenisation: u = E x + w with w periodic in x and y (and in z for a
 * prismatic slab; a slab whose section changes with depth has w = 0 on its lower and upper faces instead). Each macro
 * strain E is applied as the initial strain -E, the periodic w follows from constraints that tie every node of an upper
 * face to its partner on the lower face, and the volume average of the stress is a column of the effective matrix.
 * Voigt order xx yy zz xy yz zx, engineering shear. Cn is that matrix over the solid's Young's modulus. */
static bool stiffness_matrix(const Cell *C, bool zper, double nu, double Cn[36], int *nelem, char *err, size_t errlen) {
    int n = C->n, nz = C->nz, N1 = n + 1, NZ1 = nz + 1;
    size_t nv = (size_t)n * n * nz, ngrid = (size_t)N1 * N1 * NZ1;
    int *par = malloc(nv * sizeof(int)), *node_of = malloc(ngrid * sizeof(int)), *conn = NULL, *hn = NULL, *hm = NULL, *hma = NULL;
    unsigned char *keep = calloc(nv, 1), *fixed = NULL;
    double *xyz = NULL, *escale = NULL, *eps0 = NULL, *hw = NULL;
    bool success = false;
    SolidResult res = {0};
    memset(Cn, 0, 36 * sizeof(double));
    if (!par || !node_of || !keep) goto done;
#define V3(i, j, k) ((size_t)(k) * (size_t)n * (size_t)n + (size_t)(j) * (size_t)n + (size_t)(i))
#define G3(i, j, k) ((size_t)(k) * (size_t)N1 * (size_t)N1 + (size_t)(j) * (size_t)N1 + (size_t)(i))
    /* pieces, joined across the periodic faces too */
    for (size_t v = 0; v < nv; v++) par[v] = (int)v;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++) {
                size_t v = V3(i, j, k);
                if (!C->occ[v]) continue;
                size_t nb[3] = {V3((i + 1) % n, j, k), V3(i, (j + 1) % n, k), k + 1 < nz ? V3(i, j, k + 1) : (zper ? V3(i, j, 0) : SIZE_MAX)};
                for (int q = 0; q < 3; q++)
                    if (nb[q] != SIZE_MAX && C->occ[nb[q]]) {
                        int a = uf_find(par, (int)v), b = uf_find(par, (int)nb[q]);
                        if (a != b) par[a] = b;
                    }
            }
    /* what carries load: with z periodic the largest piece; otherwise every piece that reaches the lower or upper face */
    int *size = calloc(nv, sizeof(int));
    unsigned char *anchored = calloc(nv, 1);
    if (!size || !anchored) {
        free(size), free(anchored);
        goto done;
    }
    int best = -1;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++) {
                size_t v = V3(i, j, k);
                if (!C->occ[v]) continue;
                int r = uf_find(par, (int)v);
                size[r]++;
                if (k == 0 || k == nz - 1) anchored[r] = 1;
                if (best < 0 || size[r] > size[best]) best = r;
            }
    int ne = 0;
    for (size_t v = 0; v < nv; v++)
        if (C->occ[v]) {
            int r = uf_find(par, (int)v);
            keep[v] = zper ? r == best : anchored[r];
            ne += keep[v];
        }
    free(size), free(anchored);
    *nelem = ne;
    if (!ne) { /* nothing carries: the cell has no stiffness */
        success = true;
        goto done;
    }
    for (size_t g = 0; g < ngrid; g++) node_of[g] = -1;
    conn = malloc(8 * (size_t)ne * sizeof(int)), xyz = malloc(3 * ngrid * sizeof(double)), escale = malloc((size_t)ne * sizeof(double));
    eps0 = malloc(6 * (size_t)ne * sizeof(double));
    int *gidx = malloc(ngrid * sizeof(int)); /* grid index of each node */
    if (!conn || !xyz || !escale || !eps0 || !gidx) {
        free(gidx);
        goto done;
    }
    static const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    int nn = 0, e = 0;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++) {
                size_t v = V3(i, j, k);
                if (!keep[v]) continue;
                for (int a = 0; a < 8; a++) {
                    int ii = i + corner[a][0], jj = j + corner[a][1], kk = k + corner[a][2];
                    size_t g = G3(ii, jj, kk);
                    if (node_of[g] < 0) {
                        node_of[g] = nn, gidx[nn] = (int)g;
                        xyz[3 * nn] = ii * C->h, xyz[3 * nn + 1] = jj * C->h, xyz[3 * nn + 2] = kk * C->hz;
                        nn++;
                    }
                    conn[8 * e + a] = node_of[g];
                }
                escale[e++] = C->phi[v];
            }
    /* constraints: a node on an upper face follows its partner on the lower face (w periodic), written as two equal
     * masters of weight 1/2, the form the solver's constraint table takes */
    fixed = calloc(3 * (size_t)nn, 1);
    hn = malloc((size_t)nn * sizeof(int)), hm = malloc((size_t)nn * sizeof(int)), hma = malloc(4 * (size_t)nn * sizeof(int));
    hw = malloc(4 * (size_t)nn * sizeof(double));
    if (!fixed || !hn || !hm || !hma || !hw) {
        free(gidx);
        goto done;
    }
    int nh = 0, pin = -1;
    for (int q = 0; q < nn; q++) {
        int g = gidx[q], ii = g % N1, jj = (g / N1) % N1, kk = g / (N1 * N1);
        if (!zper && (kk == 0 || kk == nz)) { /* w = 0 on the loaded faces of a non-periodic slab */
            fixed[3 * q] = fixed[3 * q + 1] = fixed[3 * q + 2] = 1;
            continue;
        }
        int mi = ii, mj = jj, mk = kk;
        if (ii == n) mi = 0;
        else if (jj == n) mj = 0;
        else if (zper && kk == nz) mk = 0;
        else {
            if (pin < 0) pin = q;
            continue;
        }
        int master = node_of[G3(mi, mj, mk)];
        if (master < 0) continue; /* no material across this face here: a free surface */
        hn[nh] = q, hm[nh] = 2, hma[4 * nh] = hma[4 * nh + 1] = master, hma[4 * nh + 2] = hma[4 * nh + 3] = master;
        hw[4 * nh] = hw[4 * nh + 1] = 0.5, hw[4 * nh + 2] = hw[4 * nh + 3] = 0;
        nh++;
    }
    free(gidx);
    if (zper) {
        if (pin < 0) {
            snprintf(err, errlen, "the periodic cell has no interior node to hold");
            goto done;
        }
        fixed[3 * pin] = fixed[3 * pin + 1] = fixed[3 * pin + 2] = 1;
    }
    SolidMaterial mat = {1.0, nu, 0};
    HexModel hmod = {.nnodes = nn, .nelems = ne, .xyz = xyz, .conn = conn, .nmat = 1, .mat = &mat, .formulation = HEX8_INCOMPATIBLE,
                     .elem_scale = escale, .nhang = nh, .hang_node = hn, .hang_nmaster = hm, .hang_master = hma, .hang_weight = hw};
    SolidOptions opt = {.solver = SOLID_SOLVER_PCG, .pcg_tol = 1e-11};
    double V = C->P * C->P * C->H;
    for (int col = 0; col < 6; col++) {
        for (int q = 0; q < ne; q++)
            for (int k = 0; k < 6; k++) eps0[6 * q + k] = k == col ? -1.0 : 0.0;
        SolidLoads ld = {.fixed = fixed, .eps0 = eps0};
        solid_result_free(&res);
        memset(&res, 0, sizeof res);
        if (!solid_solve(&hmod, &ld, &opt, &res, err, errlen)) goto done;
        for (int q = 0; q < ne; q++) {
            double w = C->h * C->h * C->hz / 8.0;
            for (int g = 0; g < 8; g++)
                for (int k = 0; k < 6; k++) Cn[6 * k + col] += w * res.gp_stress[48 * (size_t)q + 6 * g + (size_t)k] / V;
        }
    }
    for (int a = 0; a < 6; a++) /* symmetric up to the solver's tolerance: take the mean */
        for (int b = a + 1; b < 6; b++) Cn[6 * a + b] = Cn[6 * b + a] = 0.5 * (Cn[6 * a + b] + Cn[6 * b + a]);
    success = true;
done:
    solid_result_free(&res);
    free(par), free(node_of), free(conn), free(hn), free(hm), free(hma), free(keep), free(fixed), free(xyz), free(escale), free(eps0), free(hw);
    return success;
#undef V3
#undef G3
}

bool support_cell_solve(const SupportSpec *s, double d0, double d1, double nu, int voxels, SupportCellProps *out, char *err,
                        size_t errlen) {
    memset(out, 0, sizeof *out);
    if (!support_spec_check(s, err, errlen)) return false;
    if (s->type == SUPPORT_HOMOGENEOUS || s->type == SUPPORT_EXPLICIT) { /* explicit supports are solid voxels */
        double f = s->type == SUPPORT_EXPLICIT ? 1.0 : s->fraction, D[6][6];
        out->solid_fraction = f;
        out->stiff_z = out->stiff_x = out->cond_z = out->cond_x = out->cond_y = f;
        isotropic_D(1.0, nu, D);
        for (int a = 0; a < 36; a++) out->C[a] = f * D[a / 6][a % 6];
        return true;
    }
    if (!support_prismatic(s) && !(d1 > d0)) {
        snprintf(err, errlen, "a %s support needs a slab d0 < d1 below the part", support_type_name(s->type));
        return false;
    }
    Cell C = {0};
    if (!cell_build(&C, s, d0, d1, voxels)) {
        cell_free(&C);
        snprintf(err, errlen, "out of memory for the unit cell");
        return false;
    }
    size_t nv = (size_t)C.n * C.n * C.nz, nocc = 0;
    double vol = 0;
    for (size_t v = 0; v < nv; v++) nocc += C.occ[v], vol += C.phi[v];
    out->solid_fraction = vol / (double)nv;
    out->voxels_per_pitch = C.n, out->layers = C.nz;
    bool tooth_band = s->tooth_height > 0 && s->contact_fraction < 1 && d0 < s->tooth_height;
    bool zper = support_prismatic(s) && !tooth_band;
    bool ok = true;
    if (nocc) {
        ok = stiffness_matrix(&C, zper, nu, out->C, &out->elements, err, errlen);
        double c33 = (1 - nu) / ((1 + nu) * (1 - 2 * nu)); /* C33 of the solid over E */
        out->stiff_z = out->C[14] / c33, out->stiff_x = out->C[0] / c33;
        out->cond_z = conduction(&C, 2), out->cond_x = conduction(&C, 0), out->cond_y = conduction(&C, 1);
        /* the surface: faces between a voxel at least half full and one less than half full, periodic in x and y (and z
         * when the slab repeats); the cut faces of a slab are not surface */
        double area = 0;
        for (int k = 0; k < C.nz; k++)
            for (int j = 0; j < C.n; j++)
                for (int i = 0; i < C.n; i++) {
                    bool a = C.phi[VOX(&C, i, j, k)] >= 0.5;
                    bool bx = C.phi[VOX(&C, (i + 1) % C.n, j, k)] >= 0.5, by = C.phi[VOX(&C, i, (j + 1) % C.n, k)] >= 0.5;
                    if (a != bx) area += C.h * C.hz;
                    if (a != by) area += C.h * C.hz;
                    if (k + 1 < C.nz || zper) {
                        bool bz = C.phi[VOX(&C, i, j, (k + 1) % C.nz)] >= 0.5;
                        if (a != bz) area += C.h * C.h;
                    }
                }
        out->surface_per_volume = area / (C.P * C.P * C.H);
    }
    cell_free(&C);
    return ok;
}
