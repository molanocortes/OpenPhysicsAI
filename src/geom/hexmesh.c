/* hexmesh.c - voxel hexahedral mesh generation, boundary faces, surface projection and staircase statistics */
#include "hexmesh.h"
#include "bvh.h"

#include <math.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double wall(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

void hexmesh_free(HexMesh *m) {
    free(m->xyz);
    free(m->conn);
    free(m->region);
    free(m->body);
    free(m->ijk);
    free(m->cell_elem);
    free(m->face_elem);
    free(m->face_local);
    free(m->face_tri);
    free(m->face_dist);
    memset(m, 0, sizeof *m);
}

/* ---- inside test by column ray parity ---------------------------------------------------------- */

typedef struct {
    double *t;
    int n, cap;
    bool oom;
} Hits;

static bool on_hit(void *ctx, const BvhHit *h) {
    Hits *hs = ctx;
    if (hs->n == hs->cap) {
        int cap = hs->cap ? 2 * hs->cap : 64;
        double *nt = realloc(hs->t, (size_t)cap * sizeof(double));
        if (!nt) {
            hs->oom = true;
            return false;
        }
        hs->t = nt;
        hs->cap = cap;
    }
    hs->t[hs->n++] = h->t;
    return true;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y);
}

typedef struct {
    const Bvh *bvh;
    const HexMesh *m;    /* grid description */
    unsigned char *votes; /* cells: bit a set when the ray along axis a found the cell inside */
    int axis;             /* 0 x, 1 y, 2 z: the axis this sweep casts along */
    int u0, u1, v0, v1;   /* column range in the two transverse axes */
    atomic_llong abstained;  /* shared across column chunks */
    atomic_bool oom;
} SweepCtx;

/* The point a cell is judged by: its centre, moved by this fraction of the cell in each direction. All three rays
 * pass through exactly this point, so the three answers are answers to the same question; and because the offset is
 * not a round fraction, the point does not land on a face, an edge or a facet diagonal of a part modelled on the
 * same grid, which is what a cube meshed at its own size would otherwise do. */
static const double SAMPLE_OFFSET[3] = {0.0137, 0.0071, -0.0113};

/* Parity of ray crossings along ONE axis, for every cell of every column of that axis.
 *
 * A single axis is not enough on a surface that is not closed: a ray that passes through a gap flips the parity of
 * everything behind it, and a whole column of cells is lost or invented. Three axes vote (see sweep_commit), so a
 * gap has to line up with two of the three directions before it can change the answer. That is the reason for three
 * sweeps rather than a generalised winding number: the winding number is the more general answer, but it costs a
 * solid angle over every triangle for every cell, which on 631 167 triangles and millions of cells is out of reach
 * on this machine, while three parity sweeps reuse the ray machinery that is already here and cost the same as the
 * three jittered rays this used to cast along z alone. */
static void sweep(void *vctx, int begin, int end, int tid) {
    (void)tid;
    SweepCtx *c = vctx;
    const HexMesh *m = c->m;
    const int a = c->axis, u = (a + 1) % 3, v = (a + 2) % 3;
    const int na = m->dims[a];
    const int stride[3] = {1, m->dims[0], m->dims[0] * m->dims[1]};
    Hits hs = {NULL, 0, 0, false};
    unsigned char *inside = calloc((size_t)na, 1);
    if (!inside) {
        atomic_store_explicit(&c->oom, true, memory_order_relaxed);
        return;
    }
    long long abstained = 0;
    double dir[3] = {0, 0, 0};
    dir[a] = 1;
    const double low = m->origin[a] - 1.0;
    for (int vv = begin; vv < end; vv++) {
        int iv = c->v0 + vv;
        for (int iu = c->u0; iu < c->u1; iu++) {
            memset(inside, 0, (size_t)na);
            double o[3];
            o[a] = low;
            o[u] = m->origin[u] + (iu + 0.5 + SAMPLE_OFFSET[u]) * m->h[u];
            o[v] = m->origin[v] + (iv + 0.5 + SAMPLE_OFFSET[v]) * m->h[v];
            hs.n = 0;
            bvh_ray_all(c->bvh, o, dir, 0.0, on_hit, &hs);
            if (hs.oom) {
                atomic_store_explicit(&c->oom, true, memory_order_relaxed);
                break;
            }
            bool reliable = true;
            if (hs.n) {
                qsort(hs.t, (size_t)hs.n, sizeof(double), cmp_d);
                /* merge coincident hits (a ray through a shared edge reports both triangles) */
                int w = 1;
                double tol = 1e-9 * (m->h[a] > 0 ? m->h[a] : 1);
                for (int k = 1; k < hs.n; k++)
                    if (hs.t[k] - hs.t[w - 1] > tol) hs.t[w++] = hs.t[k];
                hs.n = w;
                /* A ray that starts outside the part and ends outside it must cross the surface an even number of
                 * times. An odd count means this ray went through a hole or through a sheet of zero thickness, so
                 * its parity is worthless for the whole column and this axis abstains rather than voting wrongly. */
                reliable = (hs.n & 1) == 0;
                if (reliable) {
                    int hit = 0;
                    for (int k = 0; k < na; k++) {
                        double ca = m->origin[a] + (k + 0.5 + SAMPLE_OFFSET[a]) * m->h[a] - low;
                        while (hit < hs.n && hs.t[hit] < ca) hit++;
                        if (hit & 1) inside[k] = 1;
                    }
                }
            }
            size_t base = (size_t)iu * stride[u] + (size_t)iv * stride[v];
            if (!reliable) {
                for (int k = 0; k < na; k++) c->votes[base + (size_t)k * stride[a]] |= (unsigned char)(8u << a);
                abstained++;
            } else {
                for (int k = 0; k < na; k++)
                    if (inside[k]) c->votes[base + (size_t)k * stride[a]] |= (unsigned char)(1u << a);
            }
        }
    }
    free(inside);
    free(hs.t);
    atomic_fetch_add_explicit(&c->abstained, abstained, memory_order_relaxed);
}

/* ---- faces ------------------------------------------------------------------------------------- */

/* neighbour offsets of the hex8 local faces: 0 -z, 1 +z, 2 -y, 3 +x, 4 +y, 5 -x */
static const int FACE_DIR[6][3] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}};

typedef struct {
    const HexMesh *m;
    const HexMeshBody *bodies;
    const Bvh *bvhs;
    double radius;
} ProjCtx;

typedef struct {
    const HexMeshBody *b;
    const double *c, *n;
    double best;
    int tri;
} NearCtx;

static bool near_tri(void *vctx, int t) {
    NearCtx *nc = vctx;
    const double *nt = nc->b->normal + 3 * (size_t)t;
    if (nt[0] * nc->n[0] + nt[1] * nc->n[1] + nt[2] * nc->n[2] <= 0) return true; /* faces the other way */
    double q[3];
    closest_point_triangle(nc->c, nc->b->v + 3 * (size_t)nc->b->tri[3 * t], nc->b->v + 3 * (size_t)nc->b->tri[3 * t + 1],
                           nc->b->v + 3 * (size_t)nc->b->tri[3 * t + 2], q);
    double d = sqrt((q[0] - nc->c[0]) * (q[0] - nc->c[0]) + (q[1] - nc->c[1]) * (q[1] - nc->c[1]) + (q[2] - nc->c[2]) * (q[2] - nc->c[2]));
    if (d < nc->best || (d == nc->best && t < nc->tri)) nc->best = d, nc->tri = t;
    return true;
}

static void project_faces(void *vctx, int begin, int end, int tid) {
    ProjCtx *p = vctx;
    const HexMesh *m = p->m;
    for (int f = begin; f < end; f++) {
        m->face_tri[f] = -1;
        m->face_dist[f] = -1;
        int e = m->face_elem[f], b = m->body[e];
        if (b < 0) continue;
        int lf = m->face_local[f];
        double c[3], n[3] = {FACE_DIR[lf][0], FACE_DIR[lf][1], FACE_DIR[lf][2]};
        for (int k = 0; k < 3; k++) c[k] = m->origin[k] + (m->ijk[3 * e + k] + 0.5 + 0.5 * FACE_DIR[lf][k]) * m->h[k];
        NearCtx nc = {&p->bodies[b], c, n, INFINITY, -1};
        double lo[3], hi[3];
        for (int k = 0; k < 3; k++) lo[k] = c[k] - p->radius, hi[k] = c[k] + p->radius;
        bvh_query_box(&p->bvhs[b], lo, hi, near_tri, &nc);
        if (nc.tri < 0) {
            /* nothing with a compatible normal nearby: fall back to the nearest triangle */
            int t;
            double q[3], d;
            if (bvh_closest(&p->bvhs[b], c, 4 * p->radius, &t, q, &d)) nc.tri = t, nc.best = d;
        }
        m->face_tri[f] = nc.tri;
        m->face_dist[f] = nc.tri >= 0 ? (float)nc.best : -1;
    }
}

/* ---- regions ----------------------------------------------------------------------------------- */

static int ufind(int *p, int x) {
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

static double tri_area(const double *v, const int *tri, int t) {
    const double *a = v + 3 * (size_t)tri[3 * t], *b = v + 3 * (size_t)tri[3 * t + 1], *c = v + 3 * (size_t)tri[3 * t + 2];
    double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    double x = u[1] * w[2] - u[2] * w[1], y = u[2] * w[0] - u[0] * w[2], z = u[0] * w[1] - u[1] * w[0];
    return 0.5 * sqrt(x * x + y * y + z * z);
}

static double signed_volume(const double *v, const int *tri, int nt) {
    /* Translate before the triple product: a millimetre-sized body must not lose volume precision merely because
     * its build-frame coordinates are far from the origin. Translation preserves volume on a closed surface. */
    const double *ref = v + 3 * (size_t)tri[0];
    double vol = 0, correction = 0;
    for (int t = 0; t < nt; t++) {
        const double *a = v + 3 * (size_t)tri[3 * t], *b = v + 3 * (size_t)tri[3 * t + 1], *c = v + 3 * (size_t)tri[3 * t + 2];
        double A[3], B[3], C[3];
        for (int k = 0; k < 3; k++) A[k] = a[k] - ref[k], B[k] = b[k] - ref[k], C[k] = c[k] - ref[k];
        double term = A[0] * (B[1] * C[2] - B[2] * C[1]) + A[1] * (B[2] * C[0] - B[0] * C[2]) +
                      A[2] * (B[0] * C[1] - B[1] * C[0]);
        double y = term - correction, sum = vol + y;
        correction = (sum - vol) - y;
        vol = sum;
    }
    return vol / 6.0;
}

bool hexmesh_generate(const HexMeshBody *bodies, int nbodies, const HexMeshSettings *s, HexMesh *m, char *err, size_t errlen) {
    double t0 = wall();
    memset(m, 0, sizeof *m);
    for (int k = 0; k < 3; k++) m->uncertain_min[k] = INFINITY, m->uncertain_max[k] = -INFINITY;
    if (nbodies <= 0 || nbodies > 16) {
        snprintf(err, errlen, "between 1 and 16 bodies can be meshed (got %d)", nbodies);
        return false;
    }
    if (!bodies || !s) {
        snprintf(err, errlen, "mesh bodies and settings are required");
        return false;
    }
    for (int k = 0; k < 3; k++)
        if (!(s->h[k] > 0) || !isfinite(s->h[k])) {
            snprintf(err, errlen, "element size must be finite and positive");
            return false;
        }
    double cell_volume = s->h[0] * s->h[1] * s->h[2];
    if (!(cell_volume > 0) || !isfinite(cell_volume)) {
        snprintf(err, errlen, "the grid cell volume is not representable; change the element sizes");
        return false;
    }
    if (s->include_plate && (!isfinite(s->plate_thickness) || !isfinite(s->plate_margin) ||
                            s->plate_thickness < 0 || s->plate_margin < 0)) {
        snprintf(err, errlen, "plate thickness and margin must be finite and nonnegative");
        return false;
    }
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int b = 0; b < nbodies; b++) {
        if (bodies[b].nt <= 0 || !bodies[b].v || !bodies[b].tri || !bodies[b].normal) {
            snprintf(err, errlen, "body %d requires triangles, vertices and normals", b);
            return false;
        }
        if (bodies[b].region != HEX_REGION_PART && bodies[b].region != HEX_REGION_SUPPORT) {
            snprintf(err, errlen, "body %d must be a part or support region", b);
            return false;
        }
        for (int t = 0; t < bodies[b].nt; t++)
            for (int q = 0; q < 3; q++) {
                if (bodies[b].tri[3 * (size_t)t + q] < 0) {
                    snprintf(err, errlen, "body %d has a negative triangle vertex index", b);
                    return false;
                }
                const double *p = bodies[b].v + 3 * (size_t)bodies[b].tri[3 * t + q];
                for (int k = 0; k < 3; k++) {
                    if (!isfinite(p[k]) || !isfinite(bodies[b].normal[3 * (size_t)t + k])) {
                        snprintf(err, errlen, "body %d geometry and normals must be finite", b);
                        return false;
                    }
                    lo[k] = fmin(lo[k], p[k]), hi[k] = fmax(hi[k], p[k]);
                }
            }
    }
    if (lo[2] < -1e-9) {
        snprintf(err, errlen, "geometry extends %.4g mm below the build plate top (z = 0); place it with geometry_place", -1e3 * lo[2]);
        return false;
    }
    memcpy(m->h, s->h, sizeof m->h);
    double margin = s->include_plate ? fmax(s->plate_margin, 0) : 0;
    const size_t grid_limit = 300000000;
    /* x/y cells snap to multiples of h so repeated meshing of the same placement is identical */
    for (int k = 0; k < 2; k++) {
        double o = floor((lo[k] - margin) / s->h[k]) * s->h[k];
        double dim = ceil((hi[k] + margin - o) / s->h[k] - 1e-9);
        if (!isfinite(o) || !isfinite(dim) || dim > (double)grid_limit) {
            snprintf(err, errlen, "the grid axis %d exceeds the cell limit; increase the element size", k);
            return false;
        }
        m->origin[k] = o;
        m->dims[k] = (int)dim;
        if (m->dims[k] < 1) m->dims[k] = 1;
    }
    double layers = s->include_plate ? ceil(s->plate_thickness / s->h[2] - 1e-9) : 0;
    double top_layers = ceil(hi[2] / s->h[2] - 1e-9);
    if (!isfinite(layers) || !isfinite(top_layers) || layers < 0 || top_layers < 0 || layers + top_layers > (double)grid_limit) {
        snprintf(err, errlen, "the grid z axis exceeds the cell limit; increase the element size");
        return false;
    }
    m->plate_layers = (int)layers;
    m->origin[2] = -m->plate_layers * s->h[2];
    m->dims[2] = m->plate_layers + (int)top_layers;
    if (m->dims[2] < 1) m->dims[2] = 1;
    size_t ncells = 1, npts = 1;
    for (int k = 0; k < 3; k++) {
        /* Bound each multiplication before doing it, including node dimensions, so a tiny requested spacing never
         * wraps size_t or invokes an out-of-range floating-to-integer conversion. */
        if (ncells > grid_limit / (size_t)m->dims[k] || npts > grid_limit / (size_t)(m->dims[k] + 1)) {
            snprintf(err, errlen, "the grid exceeds the cell limit (%d x %d x %d); increase the element size", m->dims[0], m->dims[1], m->dims[2]);
            return false;
        }
        ncells *= (size_t)m->dims[k];
        npts *= (size_t)(m->dims[k] + 1);
    }
    unsigned char *mask = calloc(ncells, 1);
    unsigned char *votes = calloc(ncells, 1); /* one body at a time: bits 0..2, one per axis */
    /* Preserve the actual majority vote of each body, including abstentions. Recasting rays from element centres
     * both cost O(elements * bodies) ray queries and could disagree with the original outside-to-outside vote. */
    uint16_t *membership = nbodies > 1 ? calloc(ncells, sizeof(uint16_t)) : NULL;
    Bvh *bvhs = calloc((size_t)nbodies, sizeof(Bvh));
    if (!mask || !votes || !bvhs || (nbodies > 1 && !membership)) {
        free(mask), free(votes), free(bvhs), free(membership);
        snprintf(err, errlen, "out of memory for a %zu-cell grid", ncells);
        return false;
    }
    bool ok = true;
    for (int b = 0; b < nbodies && ok; b++) {
        if (!bvh_build(&bvhs[b], bodies[b].v, bodies[b].tri, bodies[b].nt)) {
            snprintf(err, errlen, "out of memory building the search tree");
            ok = false;
            break;
        }
        double blo3[3] = {INFINITY, INFINITY, INFINITY}, bhi3[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int t = 0; t < bodies[b].nt; t++)
            for (int q = 0; q < 3; q++)
                for (int k = 0; k < 3; k++) {
                    double x = bodies[b].v[3 * (size_t)bodies[b].tri[3 * t + q] + k];
                    blo3[k] = fmin(blo3[k], x), bhi3[k] = fmax(bhi3[k], x);
                }
        /* the body's own box, in cells, with one cell of margin on every side */
        int c0[3], c1[3];
        for (int k = 0; k < 3; k++) {
            c0[k] = (int)floor((blo3[k] - m->origin[k]) / s->h[k]) - 1;
            c1[k] = (int)ceil((bhi3[k] - m->origin[k]) / s->h[k]) + 1;
            if (c0[k] < 0) c0[k] = 0;
            if (c1[k] > m->dims[k]) c1[k] = m->dims[k];
        }
        memset(votes, 0, ncells);
        for (int a = 0; a < 3 && ok; a++) {
            int u = (a + 1) % 3, v = (a + 2) % 3;
            SweepCtx sc = {&bvhs[b], m, votes, a, c0[u], c1[u], c0[v], c1[v], 0, false};
            int nv = c1[v] - c0[v];
            if (nv <= 0) continue;
            if (s->pool && nv > 4) pool_for(s->pool, nv, 1, sweep, &sc);
            else sweep(&sc, 0, nv, 0);
            m->abstained_rays += atomic_load_explicit(&sc.abstained, memory_order_relaxed);
            if (atomic_load_explicit(&sc.oom, memory_order_relaxed)) {
                snprintf(err, errlen, "out of memory while classifying cells");
                ok = false;
            }
        }
        if (!ok) break;
        /* the vote: a cell belongs to the body when at least two of the three axes put it inside. A cell the axes
         * disagreed about is counted and its box recorded, so the caller can say how sure the mesh is. */
        for (size_t cix = 0; cix < ncells; cix++) {
            unsigned char v8 = votes[cix];
            if (!v8) continue;
            int yes = (v8 & 1) + ((v8 >> 1) & 1) + ((v8 >> 2) & 1);
            int out = ((v8 >> 3) & 1) + ((v8 >> 4) & 1) + ((v8 >> 5) & 1);
            int reliable = 3 - out;
            if (reliable > 0 && 2 * yes > reliable) {
                mask[cix] = (unsigned char)(bodies[b].region + 1);
                if (membership) membership[cix] |= (uint16_t)(1u << b);
            }
            /* uncertain: the rays that could vote did not agree, or none of them could vote at all */
            if (reliable == 0 || (yes > 0 && yes < reliable)) {
                m->uncertain_cells++;
                size_t rest = cix;
                int ijk[3];
                ijk[0] = (int)(rest % (size_t)m->dims[0]);
                rest /= (size_t)m->dims[0];
                ijk[1] = (int)(rest % (size_t)m->dims[1]);
                ijk[2] = (int)(rest / (size_t)m->dims[1]);
                for (int k = 0; k < 3; k++) {
                    double x = m->origin[k] + (ijk[k] + 0.5) * m->h[k];
                    if (x < m->uncertain_min[k]) m->uncertain_min[k] = x;
                    if (x > m->uncertain_max[k]) m->uncertain_max[k] = x;
                }
            }
        }
    }
    /* plate cells below z = 0 */
    for (int k = 0; k < m->plate_layers && ok; k++)
        for (int j = 0; j < m->dims[1]; j++)
            for (int i = 0; i < m->dims[0]; i++) mask[(size_t)i + (size_t)m->dims[0] * ((size_t)j + (size_t)m->dims[1] * (size_t)k)] = HEX_REGION_PLATE + 1;
    if (!ok) goto fail;

    size_t nel = 0;
    for (size_t c = 0; c < ncells; c++) nel += mask[c] != 0;
    /* The vote has to be decisive. One cell in twenty being a majority opinion rather than a fact is as far as this
     * will go before it says the surface is too broken to mesh honestly. */
    if (nel > 0 && m->uncertain_cells > (long long)(HEXMESH_UNCERTAIN_LIMIT * (double)nel)) {
        snprintf(err, errlen,
                 "the three inside tests disagreed on %lld of %zu cells (%.1f %%, the limit is %.0f %%): the surface "
                 "has gaps or doubled facets large enough to flip a ray, so this would not be a mesh of this part",
                 m->uncertain_cells, nel, 100.0 * (double)m->uncertain_cells / (double)nel,
                 100.0 * HEXMESH_UNCERTAIN_LIMIT);
        goto fail;
    }
    if (nel == 0) {
        snprintf(err, errlen, "no element centre lies inside the geometry: the element size (%.4g mm) is larger than the part features", 1e3 * s->h[0]);
        goto fail;
    }
    if (s->max_elements && nel > s->max_elements) {
        snprintf(err, errlen, "the mesh would have %zu elements, above the limit of %llu; increase the element size", nel, (unsigned long long)s->max_elements);
        goto fail;
    }
    m->nelems = (int)nel;
    m->cell_elem = malloc(ncells * sizeof(int));
    int *pt = malloc(npts * sizeof(int));
    m->conn = malloc(nel * 8 * sizeof(int));
    m->region = malloc(nel);
    m->body = malloc(nel);
    m->ijk = malloc(nel * 3 * sizeof(int));
    if (!m->cell_elem || !pt || !m->conn || !m->region || !m->body || !m->ijk) {
        free(pt);
        snprintf(err, errlen, "out of memory for %zu elements", nel);
        goto fail;
    }
    for (size_t p = 0; p < npts; p++) pt[p] = -1;
    int nx = m->dims[0], ny = m->dims[1], nz = m->dims[2], nn = 0, e = 0;
    #define PT(i, j, k) ((size_t)(i) + (size_t)(nx + 1) * ((size_t)(j) + (size_t)(ny + 1) * (size_t)(k)))
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                size_t c = (size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k);
                if (!mask[c]) {
                    m->cell_elem[c] = -1;
                    continue;
                }
                m->cell_elem[c] = e;
                static const int CORNER[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
                for (int a = 0; a < 8; a++) {
                    size_t p = PT(i + CORNER[a][0], j + CORNER[a][1], k + CORNER[a][2]);
                    if (pt[p] < 0) pt[p] = nn++;
                    m->conn[8 * (size_t)e + a] = pt[p];
                }
                m->region[e] = (unsigned char)(mask[c] - 1);
                m->body[e] = -1;
                if (m->region[e] != HEX_REGION_PLATE) {
                    int owner = -1;
                    bool overlap = false;
                    for (int b = nbodies - 1; b >= 0; b--) {
                        if ((int)bodies[b].region != m->region[e] || (membership && !(membership[c] & (1u << b)))) continue;
                        if (owner < 0) owner = b;
                        else {
                            m->overlap_pairs[owner][b]++;
                            overlap = true;
                        }
                    }
                    m->body[e] = (signed char)owner;
                    m->overlap_cells += overlap;
                }
                m->ijk[3 * e] = i, m->ijk[3 * e + 1] = j, m->ijk[3 * e + 2] = k;
                e++;
            }
    m->nnodes = nn;
    m->xyz = malloc((size_t)nn * 3 * sizeof(double));
    if (!m->xyz) {
        free(pt);
        snprintf(err, errlen, "out of memory for %d nodes", nn);
        goto fail;
    }
    for (int k = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) {
                int p = pt[PT(i, j, k)];
                if (p < 0) continue;
                m->xyz[3 * p] = m->origin[0] + i * s->h[0];
                m->xyz[3 * p + 1] = m->origin[1] + j * s->h[1];
                m->xyz[3 * p + 2] = m->origin[2] + k * s->h[2];
            }
    free(pt);
    #undef PT
    /* boundary faces */
    int cap = 1024;
    m->face_elem = malloc((size_t)cap * sizeof(int));
    m->face_local = malloc((size_t)cap);
    if (!m->face_elem || !m->face_local) goto oom;
    for (int ee = 0; ee < m->nelems; ee++)
        for (int lf = 0; lf < 6; lf++) {
            int i = m->ijk[3 * ee] + FACE_DIR[lf][0], j = m->ijk[3 * ee + 1] + FACE_DIR[lf][1], k = m->ijk[3 * ee + 2] + FACE_DIR[lf][2];
            bool inside = i >= 0 && j >= 0 && k >= 0 && i < nx && j < ny && k < nz;
            if (inside && m->cell_elem[(size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)] >= 0) continue;
            if (m->nfaces == cap) {
                cap *= 2;
                int *nf = realloc(m->face_elem, (size_t)cap * sizeof(int));
                unsigned char *nl = realloc(m->face_local, (size_t)cap);
                if (nf) m->face_elem = nf;
                if (nl) m->face_local = nl;
                if (!nf || !nl) goto oom;
            }
            m->face_elem[m->nfaces] = ee;
            m->face_local[m->nfaces++] = (unsigned char)lf;
        }
    m->face_tri = malloc((size_t)(m->nfaces ? m->nfaces : 1) * sizeof(int));
    m->face_dist = malloc((size_t)(m->nfaces ? m->nfaces : 1) * sizeof(float));
    if (!m->face_tri || !m->face_dist) goto oom;
    double hmax = fmax(s->h[0], fmax(s->h[1], s->h[2]));
    ProjCtx pc = {m, bodies, bvhs, 1.5 * hmax};
    if (s->pool && m->nfaces > 2048) pool_for(s->pool, m->nfaces, 512, project_faces, &pc);
    else project_faces(&pc, 0, m->nfaces, 0);

    /* staircase statistics */
    m->nbodies = nbodies;
    double cellv = cell_volume;
    for (int ee = 0; ee < m->nelems; ee++)
        if (m->body[ee] >= 0 && m->region[ee] != HEX_REGION_PLATE) m->body_volume_mesh[(int)m->body[ee]] += cellv;
    double dsum = 0, distance_area = 0;
    for (int f = 0; f < m->nfaces; f++) {
        int ee = m->face_elem[f];
        if (m->region[ee] == HEX_REGION_PLATE) continue;
        int lf = m->face_local[f];
        double area = FACE_DIR[lf][0] ? s->h[1] * s->h[2] : (FACE_DIR[lf][1] ? s->h[0] * s->h[2] : s->h[0] * s->h[1]);
        /* the bottom of a part resting on the plate is not a free surface when the plate is meshed */
        m->body_area_mesh[(int)m->body[ee]] += area;
        if (m->face_dist[f] >= 0) {
            dsum += area * m->face_dist[f];
            distance_area += area;
            if (m->face_dist[f] > m->max_face_distance) m->max_face_distance = m->face_dist[f];
        }
    }
    m->mean_face_distance = distance_area > 0 ? dsum / distance_area : 0;
    for (int b = 0; b < nbodies; b++) {
        m->body_volume_stl[b] = signed_volume(bodies[b].v, bodies[b].tri, bodies[b].nt);
        for (int t = 0; t < bodies[b].nt; t++) m->body_area_stl[b] += tri_area(bodies[b].v, bodies[b].tri, t);
    }

    /* face-connected regions and edge/vertex-only contacts */
    {
        int *parent = malloc((size_t)m->nelems * sizeof(int));
        if (!parent) goto oom;
        for (int ee = 0; ee < m->nelems; ee++) parent[ee] = ee;
        for (int ee = 0; ee < m->nelems; ee++)
            for (int lf = 1; lf <= 4; lf += 2) { /* +z (1), +x (3); +y handled below */
                int i = m->ijk[3 * ee] + FACE_DIR[lf][0], j = m->ijk[3 * ee + 1] + FACE_DIR[lf][1], k = m->ijk[3 * ee + 2] + FACE_DIR[lf][2];
                if (i >= nx || j >= ny || k >= nz) continue;
                int o = m->cell_elem[(size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)];
                if (o >= 0) {
                    int a = ufind(parent, ee), b = ufind(parent, o);
                    if (a != b) parent[a] = b;
                }
            }
        for (int ee = 0; ee < m->nelems; ee++) {
            int i = m->ijk[3 * ee], j = m->ijk[3 * ee + 1] + 1, k = m->ijk[3 * ee + 2];
            if (j >= ny) continue;
            int o = m->cell_elem[(size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)];
            if (o >= 0) {
                int a = ufind(parent, ee), b = ufind(parent, o);
                if (a != b) parent[a] = b;
            }
        }
        int *node_root = malloc((size_t)m->nnodes * sizeof(int));
        unsigned char *counted = calloc((size_t)m->nnodes, 1);
        if (!node_root || !counted) {
            free(parent), free(node_root), free(counted);
            goto oom;
        }
        for (int nd = 0; nd < m->nnodes; nd++) node_root[nd] = -1;
        for (int ee = 0; ee < m->nelems; ee++) {
            int r = ufind(parent, ee);
            if (r == ee) m->regions++;
            for (int a = 0; a < 8; a++) {
                int nd = m->conn[8 * (size_t)ee + a];
                if (node_root[nd] < 0) node_root[nd] = r;
                else if (node_root[nd] != r && !counted[nd]) counted[nd] = 1, m->edge_contacts++;
            }
        }
        free(parent), free(node_root), free(counted);
    }

    for (int b = 0; b < nbodies; b++) bvh_free(&bvhs[b]);
    free(bvhs);
    free(mask), free(votes), free(membership);
    m->seconds = wall() - t0;
    return true;
oom:
    snprintf(err, errlen, "out of memory while building mesh faces");
fail:
    for (int b = 0; b < nbodies; b++) bvh_free(&bvhs[b]);
    free(bvhs);
    free(mask), free(votes), free(membership);
    hexmesh_free(m);
    return false;
}

int hexmesh_locate(const HexMesh *m, const double p[3]) {
    int idx[3];
    for (int k = 0; k < 3; k++) {
        double f = (p[k] - m->origin[k]) / m->h[k];
        if (!(f >= 0) || f > m->dims[k]) return -1;
        idx[k] = (int)f;
        if (idx[k] == m->dims[k]) idx[k]--;
    }
    return m->cell_elem[(size_t)idx[0] + (size_t)m->dims[0] * ((size_t)idx[1] + (size_t)m->dims[1] * (size_t)idx[2])];
}

/* ---- adaptive coarsening (hexmesh.h) ------------------------------------------------------------------------- */

void hexmesh_coarsened_free(HexCoarsened *c) {
    if (!c) return;
    free(c->xyz), free(c->conn), free(c->parent), free(c->level), free(c->node_of_input);
    free(c->hang_node), free(c->hang_nmaster), free(c->hang_master), free(c->hang_owner), free(c->hang_weight);
    memset(c, 0, sizeof *c);
}

bool hexmesh_coarsen_xy(const double *xyz, int nnodes, const int *conn, int nelems, const double h[3],
                        const unsigned char *kind, int max_level, int fine_band, HexCoarsened *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (nelems <= 0 || max_level < 0 || max_level > 6) {
        snprintf(err, errlen, "coarsening needs elements and a largest level between 0 and 6");
        return false;
    }
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int n = 0; n < nnodes; n++)
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], xyz[3 * (size_t)n + k]), hi[k] = fmax(hi[k], xyz[3 * (size_t)n + k]);
    int dim[3];
    for (int k = 0; k < 3; k++) dim[k] = (int)lround((hi[k] - lo[k]) / h[k]);
    size_t ncell = (size_t)dim[0] * (size_t)dim[1] * (size_t)dim[2];
    size_t npt = (size_t)(dim[0] + 1) * (size_t)(dim[1] + 1) * (size_t)(dim[2] + 1);
#define CI(i, j, k) (((size_t)(k) * (size_t)dim[1] + (size_t)(j)) * (size_t)dim[0] + (size_t)(i))
#define PI_(i, j, k) (((size_t)(k) * (size_t)(dim[1] + 1) + (size_t)(j)) * (size_t)(dim[0] + 1) + (size_t)(i))
    int *cell = malloc(ncell * sizeof(int));
    unsigned char *lvl = malloc(ncell), *act = malloc(ncell);
    int *pt_node = malloc(npt * sizeof(int)), *pt_out = malloc(npt * sizeof(int));
    int pattern[8][3];
    bool ok = false;
    if (!cell || !lvl || !act || !pt_node || !pt_out) {
        snprintf(err, errlen, "out of memory coarsening %d elements", nelems);
        goto done;
    }
    for (size_t c = 0; c < ncell; c++) cell[c] = -1;
    for (size_t p = 0; p < npt; p++) pt_node[p] = pt_out[p] = -1;
    for (int n = 0; n < nnodes; n++) {
        int i = (int)lround((xyz[3 * (size_t)n] - lo[0]) / h[0]), j = (int)lround((xyz[3 * (size_t)n + 1] - lo[1]) / h[1]);
        int k = (int)lround((xyz[3 * (size_t)n + 2] - lo[2]) / h[2]);
        if (i >= 0 && i <= dim[0] && j >= 0 && j <= dim[1] && k >= 0 && k <= dim[2]) pt_node[PI_(i, j, k)] = n;
    }
    { /* the corner order of the input's hex8, read off element 0, so the output matches it */
        double c0[3] = {INFINITY, INFINITY, INFINITY};
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) c0[k] = fmin(c0[k], xyz[3 * (size_t)conn[a] + k]);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) pattern[a][k] = (int)lround((xyz[3 * (size_t)conn[a] + k] - c0[k]) / h[k]);
    }
    for (int e = 0; e < nelems; e++) {
        double m[3] = {INFINITY, INFINITY, INFINITY}, M[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) m[k] = fmin(m[k], xyz[3 * (size_t)conn[8 * (size_t)e + a] + k]), M[k] = fmax(M[k], xyz[3 * (size_t)conn[8 * (size_t)e + a] + k]);
        for (int k = 0; k < 3; k++)
            if (fabs(M[k] - m[k] - h[k]) > 1e-6 * h[k]) {
                snprintf(err, errlen, "element %d is not one voxel of the given size: coarsening needs a uniform voxel mesh", e);
                goto done;
            }
        int i = (int)lround((m[0] - lo[0]) / h[0]), j = (int)lround((m[1] - lo[1]) / h[1]), k = (int)lround((m[2] - lo[2]) / h[2]);
        cell[CI(i, j, k)] = e;
    }
    /* the largest level each voxel's aligned block could take on its own: all voxels present, one mergeable kind, none
     * at the surface (a voxel is at the surface when a face, edge or corner neighbour in its layer or the next is missing) */
    for (int k = 0; k < dim[2]; k++)
        for (int j = 0; j < dim[1]; j++)
            for (int i = 0; i < dim[0]; i++) {
                size_t c = CI(i, j, k);
                lvl[c] = 0;
                if (cell[c] < 0 || kind[cell[c]] == 255) continue;
                for (int L = 1; L <= max_level; L++) {
                    int s = 1 << L, i0 = (i >> L) << L, j0 = (j >> L) << L;
                    if (i0 + s > dim[0] || j0 + s > dim[1]) break;
                    bool can = true;
                    const int g = fine_band > 0 ? fine_band : 1; /* voxels of fine band kept at every surface */
                    for (int b = j0 - g; b <= j0 + s - 1 + g && can; b++)
                        for (int a = i0 - g; a <= i0 + s - 1 + g && can; a++)
                            for (int dk = -g; dk <= g && can; dk++) {
                                int kk = k + dk;
                                bool inside = a >= i0 && a < i0 + s && b >= j0 && b < j0 + s && dk == 0;
                                if (a < 0 || b < 0 || kk < 0 || a >= dim[0] || b >= dim[1] || kk >= dim[2]) {
                                    can = false;
                                    break;
                                }
                                int f = cell[CI(a, b, kk)];
                                if (f < 0) can = false;
                                else if (inside && kind[f] != kind[cell[c]]) can = false;
                            }
                    if (!can) break;
                    lvl[c] = (unsigned char)L;
                }
            }
    /* 2:1 balance over faces, edges and corners: lower a voxel's level until no neighbour is more than one level finer */
    for (int pass = 0; pass < 64; pass++) {
        for (int k = 0; k < dim[2]; k++)
            for (int j = 0; j < dim[1]; j++)
                for (int i = 0; i < dim[0]; i++) {
                    size_t c = CI(i, j, k);
                    act[c] = 0;
                    if (cell[c] < 0) continue;
                    for (int L = lvl[c]; L >= 1; L--) {
                        int s = 1 << L, i0 = (i >> L) << L, j0 = (j >> L) << L;
                        bool all = true;
                        for (int b = j0; b < j0 + s && all; b++)
                            for (int a = i0; a < i0 + s && all; a++) all = lvl[CI(a, b, k)] >= L;
                        if (all) {
                            act[c] = (unsigned char)L;
                            break;
                        }
                    }
                }
        bool changed = false;
        for (int k = 0; k < dim[2]; k++)
            for (int j = 0; j < dim[1]; j++)
                for (int i = 0; i < dim[0]; i++) {
                    size_t c = CI(i, j, k);
                    if (cell[c] < 0 || !act[c]) continue;
                    for (int dk = -1; dk <= 1; dk++)
                        for (int dj = -1; dj <= 1; dj++)
                            for (int di = -1; di <= 1; di++) {
                                int a = i + di, b = j + dj, kk = k + dk;
                                if (a < 0 || b < 0 || kk < 0 || a >= dim[0] || b >= dim[1] || kk >= dim[2]) continue;
                                size_t n = CI(a, b, kk);
                                if (cell[n] < 0 || act[c] <= act[n] + 1) continue;
                                if (lvl[c] > act[n] + 1) lvl[c] = (unsigned char)(act[n] + 1), changed = true;
                            }
                }
        if (!changed) break;
        if (pass == 63) {
            snprintf(err, errlen, "the 2:1 balance did not settle");
            goto done;
        }
    }
    /* the elements: one per block, at its first voxel */
    int ne_out = 0;
    for (size_t c = 0; c < ncell; c++) {
        if (cell[c] < 0) continue;
        int i = (int)(c % (size_t)dim[0]), j = (int)((c / (size_t)dim[0]) % (size_t)dim[1]), L = act[c];
        if (((i >> L) << L) == i && ((j >> L) << L) == j) ne_out++;
    }
    out->conn = malloc((size_t)ne_out * 8 * sizeof(int));
    out->parent = malloc((size_t)ne_out * sizeof(int));
    out->level = malloc((size_t)ne_out);
    out->node_of_input = malloc((size_t)(nnodes ? nnodes : 1) * sizeof(int));
    if (!out->conn || !out->parent || !out->level || !out->node_of_input) {
        snprintf(err, errlen, "out of memory for the coarse mesh");
        goto done;
    }
    int nn_out = 0;
    ne_out = 0;
    for (int k = 0; k < dim[2]; k++)
        for (int j = 0; j < dim[1]; j++)
            for (int i = 0; i < dim[0]; i++) {
                size_t c = CI(i, j, k);
                int L = act[c];
                if (cell[c] < 0 || ((i >> L) << L) != i || ((j >> L) << L) != j) continue;
                int s = 1 << L;
                for (int a = 0; a < 8; a++) {
                    size_t p = PI_(i + pattern[a][0] * s, j + pattern[a][1] * s, k + pattern[a][2]);
                    if (pt_node[p] < 0) {
                        snprintf(err, errlen, "a coarse corner is not a node of the input mesh");
                        goto done;
                    }
                    if (pt_out[p] < 0) pt_out[p] = nn_out++;
                    out->conn[8 * (size_t)ne_out + a] = pt_out[p];
                }
                out->parent[ne_out] = cell[c], out->level[ne_out] = (unsigned char)L;
                ne_out++;
            }
    out->nelems = ne_out, out->nnodes = nn_out;
    out->xyz = malloc((size_t)(nn_out ? nn_out : 1) * 3 * sizeof(double));
    if (!out->xyz) {
        snprintf(err, errlen, "out of memory for the coarse mesh");
        goto done;
    }
    for (int n = 0; n < nnodes; n++) out->node_of_input[n] = -1;
    for (size_t p = 0; p < npt; p++)
        if (pt_out[p] >= 0) {
            memcpy(out->xyz + 3 * (size_t)pt_out[p], xyz + 3 * (size_t)pt_node[p], 3 * sizeof(double));
            out->node_of_input[pt_node[p]] = pt_out[p];
        }
    /* hanging nodes: an output node inside an edge or a face of a coarse element. A 2:1 mesh allows only the mid-edge and
     * mid-face points; any other would mean the balance failed, and is refused. */
    int cap = 1024, nh = 0;
    out->hang_node = malloc((size_t)cap * sizeof(int)), out->hang_nmaster = malloc((size_t)cap * sizeof(int));
    out->hang_owner = malloc((size_t)cap * sizeof(int)), out->hang_master = malloc((size_t)cap * 4 * sizeof(int));
    out->hang_weight = malloc((size_t)cap * 4 * sizeof(double));
    for (int e = 0; e < ne_out; e++) {
        int L = out->level[e];
        if (!L) continue;
        int s = 1 << L, half = s / 2, pe = out->parent[e];
        double m0[3] = {INFINITY, INFINITY, INFINITY};
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) m0[k] = fmin(m0[k], xyz[3 * (size_t)conn[8 * (size_t)pe + a] + k]);
        int i0 = (int)lround((m0[0] - lo[0]) / h[0]), j0 = (int)lround((m0[1] - lo[1]) / h[1]), k0 = (int)lround((m0[2] - lo[2]) / h[2]);
        for (int dk = 0; dk <= 1; dk++)
            for (int b = 0; b <= s; b++)
                for (int a = 0; a <= s; a++) {
                    bool corner = (a == 0 || a == s) && (b == 0 || b == s);
                    if (corner) continue;
                    size_t p = PI_(i0 + a, j0 + b, k0 + dk);
                    if (pt_out[p] < 0) continue;
                    int nm, mi[4][2];
                    if ((a == 0 || a == s) && b == half) nm = 2, mi[0][0] = a, mi[0][1] = 0, mi[1][0] = a, mi[1][1] = s;
                    else if ((b == 0 || b == s) && a == half) nm = 2, mi[0][0] = 0, mi[0][1] = b, mi[1][0] = s, mi[1][1] = b;
                    else if (a == half && b == half)
                        nm = 4, mi[0][0] = 0, mi[0][1] = 0, mi[1][0] = s, mi[1][1] = 0, mi[2][0] = s, mi[2][1] = s, mi[3][0] = 0, mi[3][1] = s;
                    else {
                        snprintf(err, errlen, "a node sits at (%d, %d) of a %d-voxel face: the mesh is not 2:1 balanced", a, b, s);
                        goto done;
                    }
                    if (nh == cap) {
                        cap *= 2;
                        int *a1 = realloc(out->hang_node, (size_t)cap * sizeof(int)), *a2 = realloc(out->hang_nmaster, (size_t)cap * sizeof(int));
                        int *a3 = realloc(out->hang_owner, (size_t)cap * sizeof(int)), *a4 = realloc(out->hang_master, (size_t)cap * 4 * sizeof(int));
                        double *a5 = realloc(out->hang_weight, (size_t)cap * 4 * sizeof(double));
                        if (a1) out->hang_node = a1;
                        if (a2) out->hang_nmaster = a2;
                        if (a3) out->hang_owner = a3;
                        if (a4) out->hang_master = a4;
                        if (a5) out->hang_weight = a5;
                        if (!a1 || !a2 || !a3 || !a4 || !a5) {
                            snprintf(err, errlen, "out of memory for the hanging nodes");
                            goto done;
                        }
                    }
                    out->hang_node[nh] = pt_out[p], out->hang_nmaster[nh] = nm, out->hang_owner[nh] = e;
                    for (int q = 0; q < 4; q++) {
                        out->hang_master[4 * nh + q] = q < nm ? pt_out[PI_(i0 + mi[q][0], j0 + mi[q][1], k0 + dk)] : -1;
                        out->hang_weight[4 * nh + q] = q < nm ? 1.0 / nm : 0;
                    }
                    nh++;
                }
    }
    out->nhang = nh;
    ok = true;
done:
#undef CI
#undef PI_
    free(cell), free(lvl), free(act), free(pt_node), free(pt_out);
    if (!ok) hexmesh_coarsened_free(out);
    return ok;
}
