/* lab2ply.c - a physics-lab result (src/lab/labio.h) as PLY files for a beauty renderer: per frame, the outer surface
 * of every part with one of its fields on it, for tools/render/beauty.py (Blender's Cycles) or any program that reads
 * PLY. The geometry and the field are the result's own; nothing here invents shape or value (README.md beside it).
 *
 *   build:  cc -O2 -std=c11 -Wall -Wextra -o build/lab2ply tools/render/lab2ply.c src/lab/labio.c -lm
 *   use:    build/lab2ply RESULT.lab OUTDIR [--field NAME] [--frames A B] [--every K] [--film N] [--range LO HI]
 *                         [--crease DEG] [--cut A0 A1] [--cut-parts NAME,NAME] [--check]
 *
 * Written into OUTDIR: frame_NNNN/PART.ply for every frame and part (binary, little endian) and manifest.json, which
 * lists them with their counts, the bounds over all frames, the range and the result's own header.
 *   cells of hexahedra   the faces used by one cell only: the outer surface, bores, and the faces a solver's own cutaway
 *                        leaves open. A hexahedron with repeated nodes (a wedge, a pyramid or a tetrahedron written as a
 *                        collapsed hexahedron) gives triangles. The format has no tetrahedra of its own: a tetrahedral
 *                        solver (the kernel) writes its surface as triangles and quadrilaterals, taken as they are.
 *   cells of triangles or quadrilaterals   the surface as written (cells of lines are left out)
 *   3D blocks            the six sides of every block, where no other block of the part lies beyond and no finer block
 *                        covers them, each corner the mean of the side's cells around it; 2D blocks: their plane
 *   points               a point cloud: vertices only, with the part's "radius" field when it has one
 * A surface vertex carries its position (m), a normal (the area weighted mean of the faces around it that lie within the
 * crease angle of each other: a vertex on a sharp edge is split, so edges stay sharp), the field's value in its unit (the
 * header's "fields"), the value mapped to 0..1 over the range, and its rest position: where the same material point is in
 * the first frame written, so that a texture drawn on a part turns with it. Faces are triangles (a quadrilateral split
 * along its shorter diagonal), oriented away from the cell.
 *
 *   --field NAME    the field to carry (the first one the result has, when not given)
 *   --frames A B    the result's frames A to B (all of them by default); --every K takes every Kth
 *   --range LO HI   the 0..1 map. By default the range labfilm chooses for the same frames (the 0.5 and 99.5 percentiles
 *                   of the field's values, symmetric about zero for signed data), so the colours match its films
 *   --film N        N frames spread evenly over A..B. A frame that falls between two of the result's is the linear blend
 *                   of their node positions and fields, on the cells of the nearer one. That is presentation and adds no
 *                   motion beyond the result's own positions; a part that turns 3 degrees between two of them is drawn
 *                   on the chord, at most 0.034 % of its radius inside the arc
 *   --cut A0 A1     a section view: the wedge between the angles A0 and A1 (degrees, counter-clockwise about the z axis)
 *                   taken out of the parts whose names contain one of --cut-parts (all parts when it is not given). The
 *                   cells are cut along the wedge's two half-planes and the field on the cut faces is interpolated along
 *                   the cells' edges (a cell field: the cell's own value). Unlike a solver's cutaway, which leaves whole
 *                   cells out by their centres, the cut is plane and stands still while the parts turn through it
 *   --crease DEG    the angle between two faces beyond which their edge is drawn sharp (35)
 *   --check         per frame and part: vertices, triangles, edges, open, non-manifold and misoriented edges, the Euler
 *                   characteristic, the bounds against those of the part's nodes, and the field on the surface against
 *                   all of the part's values (the numbers labfilm --info prints for the last frame)
 */
#define _DEFAULT_SOURCE
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../../src/lab/labio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- small helpers ------------------------------------------------------------------------------------------- */

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "lab2ply: out of memory (%zu bytes)\n", n);
        exit(2);
    }
    return q;
}
static void *xcalloc(size_t n, size_t s) {
    void *q = calloc(n ? n : 1, s);
    if (!q) {
        fprintf(stderr, "lab2ply: out of memory\n");
        exit(2);
    }
    return q;
}

typedef struct Text {
    char *p;
    size_t len, cap;
} Text;

static void tprintf(Text *t, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(t->p ? t->p + t->len : NULL, t->p ? t->cap - t->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (t->p && t->len + (size_t)n < t->cap) {
            t->len += (size_t)n;
            return;
        }
        size_t c = t->cap ? t->cap : 1 << 16;
        while (c <= t->len + (size_t)n) c *= 2;
        t->p = xrealloc(t->p, c);
        t->cap = c;
    }
}

static void tjson_str(Text *t, const char *s) {
    tprintf(t, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') tprintf(t, "\\%c", c);
        else if (c < 0x20) tprintf(t, "\\u%04x", c);
        else tprintf(t, "%c", c);
    }
    tprintf(t, "\"");
}

static bool mkdir_p(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *q = buf + 1; *q; q++)
        if (*q == '/') {
            *q = 0;
            mkdir(buf, 0755);
            *q = '/';
        }
    struct stat st;
    return mkdir(buf, 0755) == 0 || (stat(buf, &st) == 0 && S_ISDIR(st.st_mode));
}

/* ---- options ------------------------------------------------------------------------------------------------- */

typedef struct Opts {
    const char *in, *out, *field, *cut_parts;
    int first, last, every, film;
    bool range_given, check, cut;
    double lo, hi, crease_deg, cut0, cut1;
} Opts;

static void usage(void) {
    fprintf(stderr, "usage: lab2ply RESULT.lab OUTDIR [--field NAME] [--frames A B] [--every K] [--film N] [--range LO HI]\n"
                    "                [--crease DEG] [--cut A0 A1] [--cut-parts NAME,NAME] [--check]\n");
}

/* whether a part takes the cut: its name contains one of the comma separated names (every part when none are given) */
static bool cut_this(const Opts *o, const char *name) {
    if (!o->cut) return false;
    if (!o->cut_parts || !o->cut_parts[0]) return true;
    const char *s = o->cut_parts;
    while (*s) {
        const char *e = strchr(s, ',');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char w[64];
        if (n > 0 && n < sizeof w) {
            memcpy(w, s, n), w[n] = 0;
            if (strstr(name, w)) return true;
        }
        if (!e) break;
        s = e + 1;
    }
    return false;
}

/* ---- the output mesh: vertices (x y z nx ny nz value value01) and triangles ------------------------------------ */

typedef struct Mesh {
    float *v; /* 11 per vertex: x y z, normal, value, value01, rest x y z */
    int nv, cap_v;
    int *t; /* 3 per triangle */
    int nt, cap_t;
    bool points; /* a point cloud: no faces, vertices x y z value value01 radius (normals unused) */
    bool has_radius;
    float *radius;
} Mesh;

static void mesh_clear(Mesh *m) { m->nv = m->nt = 0, m->points = m->has_radius = false; }
static void mesh_free(Mesh *m) {
    free(m->v), free(m->t), free(m->radius);
    memset(m, 0, sizeof *m);
}

#define NV 11
static int mesh_vertex(Mesh *m, const double x[3], const double n[3], double val, double v01, const double rest[3]) {
    if (m->nv + 1 > m->cap_v) {
        m->cap_v = m->cap_v ? 2 * m->cap_v : 1 << 14;
        m->v = xrealloc(m->v, (size_t)m->cap_v * NV * sizeof(float));
    }
    float *q = &m->v[NV * (size_t)m->nv];
    q[0] = (float)x[0], q[1] = (float)x[1], q[2] = (float)x[2];
    q[3] = (float)n[0], q[4] = (float)n[1], q[5] = (float)n[2];
    q[6] = (float)val, q[7] = (float)v01;
    q[8] = (float)rest[0], q[9] = (float)rest[1], q[10] = (float)rest[2];
    return m->nv++;
}

static void mesh_tri(Mesh *m, int a, int b, int c) {
    if (m->nt + 1 > m->cap_t) {
        m->cap_t = m->cap_t ? 2 * m->cap_t : 1 << 14;
        m->t = xrealloc(m->t, (size_t)m->cap_t * 3 * sizeof(int));
    }
    int *q = &m->t[3 * (size_t)m->nt++];
    q[0] = a, q[1] = b, q[2] = c;
}

static double map01(double v, double lo, double hi) {
    double u = hi > lo ? (v - lo) / (hi - lo) : 0;
    return u < 0 ? 0 : u > 1 ? 1 : u;
}

/* ---- the surface builder: welded points (nodes, and points on edges where a cut crosses them) and triangles ---- */

typedef struct Surf {
    const double *X; /* the part's node positions (blended) */
    const double *R; /* their rest positions (the first frame written) */
    const float *nv; /* its node field, or NULL */
    int nnodes;
    int *node_pt;    /* per node: its point, or -1 */
    double *px, *pv, *pr; /* per point: position, node field value, rest position */
    int npt, cap_pt;
    int *hk, *hp;    /* edge points: open addressing on (a, b, plane) -> point */
    size_t hcap, hcount;
    int *tri;        /* 3 point indices per triangle */
    float *tval;     /* per triangle: its cell's value (a cell field) */
    int ntri, cap_tri;
    double nrm[2][3], dir[2][3]; /* the cut's two planes: normal (pointing into the wedge), radial direction */
} Surf;

static void surf_begin(Surf *s, const double *X, const double *R, const float *nv, int nnodes) {
    s->X = X, s->R = R, s->nv = nv;
    if (nnodes > s->nnodes) s->node_pt = xrealloc(s->node_pt, (size_t)nnodes * sizeof(int));
    s->nnodes = nnodes;
    for (int i = 0; i < nnodes; i++) s->node_pt[i] = -1;
    s->npt = s->ntri = 0;
    if (s->hcap) {
        for (size_t i = 0; i < s->hcap; i++) s->hk[3 * i] = -1;
        s->hcount = 0;
    }
}

static void surf_free(Surf *s) {
    free(s->node_pt), free(s->px), free(s->pv), free(s->pr), free(s->hk), free(s->hp), free(s->tri), free(s->tval);
    memset(s, 0, sizeof *s);
}

static void reserve_points(Surf *s, int need) {
    if (need <= s->cap_pt) return;
    int c = s->cap_pt ? s->cap_pt : 1 << 14;
    while (c < need) c *= 2;
    s->px = xrealloc(s->px, (size_t)c * 3 * sizeof(double));
    s->pv = xrealloc(s->pv, (size_t)c * sizeof(double));
    s->pr = xrealloc(s->pr, (size_t)c * 3 * sizeof(double));
    s->cap_pt = c;
}

static int node_point(Surf *s, int n) {
    if (s->node_pt[n] >= 0) return s->node_pt[n];
    reserve_points(s, s->npt + 1);
    memcpy(&s->px[3 * (size_t)s->npt], &s->X[3 * (size_t)n], 3 * sizeof(double));
    memcpy(&s->pr[3 * (size_t)s->npt], &s->R[3 * (size_t)n], 3 * sizeof(double));
    s->pv[s->npt] = s->nv ? s->nv[n] : 0;
    return s->node_pt[n] = s->npt++;
}

static size_t hash3(int a, int b, int c) {
    uint64_t h = (uint64_t)(uint32_t)a * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)(uint32_t)b * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
    h ^= (uint64_t)(uint32_t)c * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
    return (size_t)(h ^ (h >> 29));
}

static void rehash(Surf *s) {
    size_t oc = s->hcap, nc = oc ? 2 * oc : 1 << 12;
    int *ok = s->hk, *op = s->hp;
    s->hk = xrealloc(NULL, nc * 3 * sizeof(int)), s->hp = xrealloc(NULL, nc * sizeof(int));
    for (size_t i = 0; i < nc; i++) s->hk[3 * i] = -1;
    s->hcap = nc;
    for (size_t i = 0; i < oc; i++)
        if (ok[3 * i] >= 0) {
            size_t h = hash3(ok[3 * i], ok[3 * i + 1], ok[3 * i + 2]) & (nc - 1);
            while (s->hk[3 * h] >= 0) h = (h + 1) & (nc - 1);
            memcpy(&s->hk[3 * h], &ok[3 * i], 3 * sizeof(int));
            s->hp[h] = op[i];
        }
    free(ok), free(op);
}

static double plane_dist(const Surf *s, int p, int plane) {
    const double *x = &s->px[3 * (size_t)p], *n = s->nrm[plane];
    return x[0] * n[0] + x[1] * n[1] + x[2] * n[2];
}

/* the point where the segment between points a and b crosses the plane; one point however many faces ask for it */
static int edge_point(Surf *s, int a, int b, int plane) {
    if (a > b) {
        int t = a;
        a = b, b = t;
    }
    if (2 * (s->hcount + 1) > s->hcap) rehash(s);
    size_t h = hash3(a, b, plane) & (s->hcap - 1);
    while (s->hk[3 * h] >= 0) {
        if (s->hk[3 * h] == a && s->hk[3 * h + 1] == b && s->hk[3 * h + 2] == plane) return s->hp[h];
        h = (h + 1) & (s->hcap - 1);
    }
    double da = plane_dist(s, a, plane), db = plane_dist(s, b, plane);
    double t = da / (da - db);
    int p;
    if (!(t > 0)) p = a; /* on the plane already: no new point */
    else if (!(t < 1)) p = b;
    else {
        reserve_points(s, s->npt + 1);
        p = s->npt++;
        for (int k = 0; k < 3; k++) {
            s->px[3 * (size_t)p + k] = s->px[3 * (size_t)a + k] + t * (s->px[3 * (size_t)b + k] - s->px[3 * (size_t)a + k]);
            s->pr[3 * (size_t)p + k] = s->pr[3 * (size_t)a + k] + t * (s->pr[3 * (size_t)b + k] - s->pr[3 * (size_t)a + k]);
        }
        s->pv[p] = s->pv[a] + t * (s->pv[b] - s->pv[a]);
    }
    s->hk[3 * h] = a, s->hk[3 * h + 1] = b, s->hk[3 * h + 2] = plane, s->hp[h] = p;
    s->hcount++;
    return p;
}

/* the part of a polygon on one side of a plane (keep_le: where the distance is at most 0), returns its size */
static int clip(Surf *s, const int *in, int m, int plane, bool keep_le, int *out) {
    int k = 0;
    for (int i = 0; i < m; i++) {
        int a = in[i], b = in[(i + 1) % m];
        double da = plane_dist(s, a, plane), db = plane_dist(s, b, plane);
        bool ia = keep_le ? da <= 0 : da > 0, ib = keep_le ? db <= 0 : db > 0;
        if (ia) out[k++] = a;
        if (ia != ib) {
            int p = edge_point(s, a, b, plane);
            if (k == 0 || out[k - 1] != p) out[k++] = p;
        }
    }
    while (k > 1 && out[k - 1] == out[0]) k--;
    return k;
}

static void add_tri(Surf *s, int a, int b, int c, float val) {
    if (a == b || b == c || c == a) return;
    if (s->ntri + 1 > s->cap_tri) {
        s->cap_tri = s->cap_tri ? 2 * s->cap_tri : 1 << 14;
        s->tri = xrealloc(s->tri, (size_t)s->cap_tri * 3 * sizeof(int));
        s->tval = xrealloc(s->tval, (size_t)s->cap_tri * sizeof(float));
    }
    int *q = &s->tri[3 * (size_t)s->ntri];
    q[0] = a, q[1] = b, q[2] = c;
    s->tval[s->ntri++] = val;
}

static double dist2(const Surf *s, int a, int b) {
    const double *x = &s->px[3 * (size_t)a], *y = &s->px[3 * (size_t)b];
    return (x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]) + (x[2] - y[2]) * (x[2] - y[2]);
}

/* a convex polygon of points as triangles: a fan, a quadrilateral along its shorter diagonal */
static void add_poly(Surf *s, const int *p, int m, float val) {
    int q[64], k = 0;
    for (int i = 0; i < m && k < 64; i++)
        if (k == 0 || p[i] != q[k - 1]) q[k++] = p[i];
    while (k > 1 && q[k - 1] == q[0]) k--;
    if (k < 3) return;
    if (k == 4 && dist2(s, q[1], q[3]) < dist2(s, q[0], q[2])) {
        add_tri(s, q[1], q[2], q[3], val), add_tri(s, q[1], q[3], q[0], val);
        return;
    }
    for (int i = 1; i + 1 < k; i++) add_tri(s, q[0], q[i], q[i + 1], val);
}

/* ---- outer faces of a cells part ----------------------------------------------------------------------------- */

typedef struct FaceRec {
    int key[4]; /* its distinct nodes sorted, INT_MAX after them */
    int n[4];   /* its nodes in order, oriented away from the cell */
    int m;      /* 3 or 4 */
    int cell;
} FaceRec;

static const int HF[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
static const int HE[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

static int cmp_face(const void *a, const void *b) {
    const FaceRec *x = a, *y = b;
    for (int k = 0; k < 4; k++)
        if (x->key[k] != y->key[k]) return x->key[k] < y->key[k] ? -1 : 1;
    return 0;
}

static void newell(const double *X, const int *n, int m, double out[3]) {
    out[0] = out[1] = out[2] = 0;
    for (int i = 0; i < m; i++) {
        const double *a = &X[3 * (size_t)n[i]], *b = &X[3 * (size_t)n[(i + 1) % m]];
        out[0] += (a[1] - b[1]) * (a[2] + b[2]);
        out[1] += (a[2] - b[2]) * (a[0] + b[0]);
        out[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
}

/* the faces used by one cell only, oriented away from their cell; triangles and quadrilaterals as they are */
static FaceRec *outer_faces(const LabPart *p, const double *X, int *nout) {
    FaceRec *f;
    int nf = 0;
    if (p->cell_type == LAB_HEX) {
        f = xrealloc(NULL, (size_t)p->ncells * 6 * sizeof *f);
        for (int c = 0; c < p->ncells; c++) {
            const int *e = &p->conn[8 * (size_t)c];
            for (int k = 0; k < 6; k++) {
                FaceRec *F = &f[nf];
                int m = 0;
                for (int j = 0; j < 4; j++) {
                    int v = e[HF[k][j]];
                    bool dup = false;
                    for (int i = 0; i < m; i++) dup |= F->n[i] == v;
                    if (!dup) F->n[m++] = v;
                }
                if (m < 3) continue; /* a face collapsed to an edge or a point */
                F->m = m, F->cell = c;
                for (int j = 0; j < 4; j++) F->key[j] = j < m ? F->n[j] : INT_MAX;
                for (int i = 1; i < 4; i++) /* sort the key */
                    for (int j = i; j > 0 && F->key[j - 1] > F->key[j]; j--) {
                        int t = F->key[j];
                        F->key[j] = F->key[j - 1], F->key[j - 1] = t;
                    }
                nf++;
            }
        }
        qsort(f, (size_t)nf, sizeof *f, cmp_face);
        int out = 0;
        for (int i = 0; i < nf;) {
            int j = i + 1;
            while (j < nf && cmp_face(&f[i], &f[j]) == 0) j++;
            if (j - i == 1) f[out++] = f[i];
            i = j;
        }
        nf = out;
        for (int i = 0; i < nf; i++) { /* away from the cell's centre */
            FaceRec *F = &f[i];
            const int *e = &p->conn[8 * (size_t)F->cell];
            double cc[3] = {0, 0, 0}, fc[3] = {0, 0, 0}, nn[3];
            for (int k = 0; k < 8; k++)
                for (int d = 0; d < 3; d++) cc[d] += X[3 * (size_t)e[k] + d] / 8;
            for (int k = 0; k < F->m; k++)
                for (int d = 0; d < 3; d++) fc[d] += X[3 * (size_t)F->n[k] + d] / F->m;
            newell(X, F->n, F->m, nn);
            if (nn[0] * (fc[0] - cc[0]) + nn[1] * (fc[1] - cc[1]) + nn[2] * (fc[2] - cc[2]) < 0)
                for (int a = 0, b = F->m - 1; a < b; a++, b--) {
                    int t = F->n[a];
                    F->n[a] = F->n[b], F->n[b] = t;
                }
        }
    } else { /* triangles and quadrilaterals: the surface itself */
        int per = p->cell_type;
        f = xrealloc(NULL, (size_t)p->ncells * sizeof *f);
        for (int c = 0; c < p->ncells; c++) {
            FaceRec *F = &f[nf];
            int m = 0;
            for (int j = 0; j < per; j++) {
                int v = p->conn[per * (size_t)c + j];
                bool dup = false;
                for (int i = 0; i < m; i++) dup |= F->n[i] == v;
                if (!dup) F->n[m++] = v;
            }
            if (m < 3) continue;
            F->m = m, F->cell = c;
            nf++;
        }
    }
    *nout = nf;
    return f;
}

/* the cross-section of every hexahedron the plane crosses, on the half of it that bounds the wedge */
static void section(Surf *s, const LabPart *p, int plane, bool other_keep_le, const float *cellv) {
    const double *n = s->nrm[plane], *e1 = s->dir[plane];
    int other = 1 - plane;
    for (int c = 0; c < p->ncells; c++) {
        const int *e = &p->conn[8 * (size_t)c];
        double d[8];
        bool lo = false, hi = false;
        for (int k = 0; k < 8; k++) {
            const double *x = &s->X[3 * (size_t)e[k]];
            d[k] = x[0] * n[0] + x[1] * n[1] + x[2] * n[2];
            if (d[k] <= 0) lo = true;
            else hi = true;
        }
        if (!lo || !hi) continue;
        int pts[12], m = 0;
        for (int k = 0; k < 12; k++) {
            int a = e[HE[k][0]], b = e[HE[k][1]];
            if (a == b || (d[HE[k][0]] <= 0) == (d[HE[k][1]] <= 0)) continue;
            int q = edge_point(s, node_point(s, a), node_point(s, b), plane);
            bool dup = false;
            for (int i = 0; i < m; i++) dup |= pts[i] == q;
            if (!dup) pts[m++] = q;
        }
        if (m < 3) continue;
        /* in order around their centre, in the plane's own coordinates (radial, z) */
        double uc = 0, wc = 0, ang[12];
        for (int i = 0; i < m; i++) {
            const double *x = &s->px[3 * (size_t)pts[i]];
            uc += (x[0] * e1[0] + x[1] * e1[1]) / m, wc += x[2] / m;
        }
        for (int i = 0; i < m; i++) {
            const double *x = &s->px[3 * (size_t)pts[i]];
            ang[i] = atan2(x[2] - wc, x[0] * e1[0] + x[1] * e1[1] - uc);
        }
        for (int i = 1; i < m; i++)
            for (int j = i; j > 0 && ang[j - 1] > ang[j]; j--) {
                double t = ang[j];
                ang[j] = ang[j - 1], ang[j - 1] = t;
                int u = pts[j];
                pts[j] = pts[j - 1], pts[j - 1] = u;
            }
        double nn[3];
        newell(s->px, pts, m, nn);
        if (nn[0] * n[0] + nn[1] * n[1] + nn[2] * n[2] < 0) /* facing into the wedge, out of the material */
            for (int a = 0, b = m - 1; a < b; a++, b--) {
                int t = pts[a];
                pts[a] = pts[b], pts[b] = t;
            }
        int out[32];
        int k = clip(s, pts, m, other, other_keep_le, out);
        add_poly(s, out, k, cellv ? cellv[c] : 0.0f);
    }
}

/* ---- from welded triangles to the output mesh: normals split at creases, values ------------------------------- */

static void finish(Surf *s, double cos_crease, bool cellfield, double lo, double hi, Mesh *m) {
    int nt = s->ntri, np = s->npt;
    double *N = xrealloc(NULL, (size_t)nt * 3 * sizeof(double)), *U = xrealloc(NULL, (size_t)nt * 3 * sizeof(double));
    for (int t = 0; t < nt; t++) {
        const double *a = &s->px[3 * (size_t)s->tri[3 * t]], *b = &s->px[3 * (size_t)s->tri[3 * t + 1]], *c = &s->px[3 * (size_t)s->tri[3 * t + 2]];
        double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        double *q = &N[3 * (size_t)t];
        q[0] = u[1] * v[2] - u[2] * v[1], q[1] = u[2] * v[0] - u[0] * v[2], q[2] = u[0] * v[1] - u[1] * v[0];
        double l = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        for (int k = 0; k < 3; k++) U[3 * (size_t)t + k] = l > 0 ? q[k] / l : 0;
    }
    int *start = xcalloc((size_t)np + 1, sizeof(int)), *inc = xrealloc(NULL, (size_t)nt * 3 * sizeof(int));
    for (int i = 0; i < 3 * nt; i++) start[s->tri[i] + 1]++;
    for (int i = 0; i < np; i++) start[i + 1] += start[i];
    int *fill = xrealloc(NULL, (size_t)np * sizeof(int));
    memcpy(fill, start, (size_t)np * sizeof(int));
    for (int t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++) inc[fill[s->tri[3 * t + k]]++] = t;
    free(fill);
    if (cellfield) /* a cell field at a point: the mean of the faces around it */
        for (int p = 0; p < np; p++) {
            double sum = 0;
            for (int i = start[p]; i < start[p + 1]; i++) sum += s->tval[inc[i]];
            s->pv[p] = start[p + 1] > start[p] ? sum / (start[p + 1] - start[p]) : 0;
        }
    int *corner = xrealloc(NULL, (size_t)nt * 3 * sizeof(int));
    int cap = 64, *vid = xrealloc(NULL, (size_t)cap * sizeof(int));
    double *vn = xrealloc(NULL, (size_t)cap * 3 * sizeof(double));
    mesh_clear(m);
    for (int p = 0; p < np; p++) {
        int k = start[p + 1] - start[p];
        if (k == 0) continue; /* a node inside the part, or one a cut left out */
        if (k > cap) {
            cap = k;
            vid = xrealloc(vid, (size_t)cap * sizeof(int)), vn = xrealloc(vn, (size_t)cap * 3 * sizeof(double));
        }
        int nout = 0;
        double v = s->pv[p], v01 = map01(v, lo, hi);
        for (int i = 0; i < k; i++) {
            int t = inc[start[p] + i];
            const double *ut = &U[3 * (size_t)t];
            double n[3] = {0, 0, 0};
            for (int j = 0; j < k; j++) {
                int t2 = inc[start[p] + j];
                const double *u2 = &U[3 * (size_t)t2];
                if (ut[0] * u2[0] + ut[1] * u2[1] + ut[2] * u2[2] >= cos_crease)
                    for (int d = 0; d < 3; d++) n[d] += N[3 * (size_t)t2 + d];
            }
            double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int d = 0; d < 3; d++) n[d] = l > 0 ? n[d] / l : ut[d];
            int o = -1;
            for (int j = 0; j < nout && o < 0; j++)
                if (n[0] * vn[3 * j] + n[1] * vn[3 * j + 1] + n[2] * vn[3 * j + 2] > 0.99995) o = vid[j];
            if (o < 0) {
                o = mesh_vertex(m, &s->px[3 * (size_t)p], n, v, v01, &s->pr[3 * (size_t)p]);
                vid[nout] = o, memcpy(&vn[3 * nout], n, sizeof n), nout++;
            }
            for (int c = 0; c < 3; c++)
                if (s->tri[3 * t + c] == p) corner[3 * t + c] = o;
        }
    }
    for (int t = 0; t < nt; t++) mesh_tri(m, corner[3 * t], corner[3 * t + 1], corner[3 * t + 2]);
    free(N), free(U), free(start), free(inc), free(corner), free(vid), free(vn);
}

/* ---- a cells part as a surface ------------------------------------------------------------------------------- */

typedef struct FaceCache { /* a part's outer faces, kept while its cells stay the same */
    char name[48];
    int ncells, cell_type;
    int *conn;
    FaceRec *faces;
    int nfaces;
    double *rest; /* the part's node positions in the first frame written */
    int nrest;
} FaceCache;

typedef struct Check {
    int points, edges, open, nonmanifold, misoriented, euler;
    double volume; /* enclosed by the surface (the divergence theorem over its outward triangles), m^3 */
} Check;

static int cmp_edge(const void *a, const void *b) {
    const int *x = a, *y = b;
    if (x[0] != y[0]) return x[0] < y[0] ? -1 : 1;
    if (x[1] != y[1]) return x[1] < y[1] ? -1 : 1;
    return 0;
}

/* the welded surface's edges: open (one face), non-manifold (more than two) and misoriented (two faces, same way) */
static Check check_surface(const Surf *s) {
    Check c = {0};
    int nt = s->ntri;
    int *e = xrealloc(NULL, (size_t)nt * 9 * sizeof(int));
    char *used = xcalloc((size_t)s->npt, 1);
    for (int t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++) {
            int a = s->tri[3 * t + k], b = s->tri[3 * t + (k + 1) % 3];
            int *q = &e[9 * (size_t)t + 3 * k];
            q[0] = a < b ? a : b, q[1] = a < b ? b : a, q[2] = a < b ? 1 : -1;
            used[a] = 1;
        }
    qsort(e, (size_t)nt * 3, 3 * sizeof(int), cmp_edge);
    for (int i = 0; i < 3 * nt;) {
        int j = i, dir = 0;
        while (j < 3 * nt && e[3 * j] == e[3 * i] && e[3 * j + 1] == e[3 * i + 1]) dir += e[3 * j + 2], j++;
        c.edges++;
        if (j - i == 1) c.open++;
        else if (j - i > 2) c.nonmanifold++;
        else if (dir != 0) c.misoriented++;
        i = j;
    }
    for (int p = 0; p < s->npt; p++) c.points += used[p];
    c.euler = c.points - c.edges + nt;
    for (int t = 0; t < nt; t++) {
        const double *a = &s->px[3 * (size_t)s->tri[3 * t]], *b = &s->px[3 * (size_t)s->tri[3 * t + 1]], *d = &s->px[3 * (size_t)s->tri[3 * t + 2]];
        c.volume += (a[0] * (b[1] * d[2] - b[2] * d[1]) + a[1] * (b[2] * d[0] - b[0] * d[2]) + a[2] * (b[0] * d[1] - b[1] * d[0])) / 6;
    }
    free(e), free(used);
    return c;
}

/* the volume of a part's hexahedra, each by the divergence theorem over its faces split as the surface splits them (along
 * the shorter diagonal), so that it equals the volume inside the outer surface when the outer faces are the right ones */
static double cells_volume(const LabPart *p, const double *X) {
    double v = 0;
    for (int c = 0; c < p->ncells; c++) {
        const int *e = &p->conn[8 * (size_t)c];
        double cc[3] = {0, 0, 0};
        for (int k = 0; k < 8; k++)
            for (int d = 0; d < 3; d++) cc[d] += X[3 * (size_t)e[k] + d] / 8;
        for (int f = 0; f < 6; f++) {
            int q[4], m = 0;
            for (int j = 0; j < 4; j++) {
                int n = e[HF[f][j]];
                bool dup = false;
                for (int i = 0; i < m; i++) dup |= q[i] == n;
                if (!dup) q[m++] = n;
            }
            if (m < 3) continue;
            double fc[3] = {0, 0, 0}, nn[3];
            for (int k = 0; k < m; k++)
                for (int d = 0; d < 3; d++) fc[d] += X[3 * (size_t)q[k] + d] / m;
            newell(X, q, m, nn);
            if (nn[0] * (fc[0] - cc[0]) + nn[1] * (fc[1] - cc[1]) + nn[2] * (fc[2] - cc[2]) < 0)
                for (int a = 0, b = m - 1; a < b; a++, b--) {
                    int t = q[a];
                    q[a] = q[b], q[b] = t;
                }
            int t[2][3], nt = 1;
            if (m == 3) memcpy(t[0], q, sizeof t[0]);
            else {
                const double *x0 = &X[3 * (size_t)q[0]], *x1 = &X[3 * (size_t)q[1]], *x2 = &X[3 * (size_t)q[2]], *x3 = &X[3 * (size_t)q[3]];
                double d02 = 0, d13 = 0;
                for (int d = 0; d < 3; d++) d02 += (x0[d] - x2[d]) * (x0[d] - x2[d]), d13 += (x1[d] - x3[d]) * (x1[d] - x3[d]);
                nt = 2;
                if (d13 < d02) t[0][0] = q[1], t[0][1] = q[2], t[0][2] = q[3], t[1][0] = q[1], t[1][1] = q[3], t[1][2] = q[0];
                else t[0][0] = q[0], t[0][1] = q[1], t[0][2] = q[2], t[1][0] = q[0], t[1][1] = q[2], t[1][2] = q[3];
            }
            for (int k = 0; k < nt; k++) {
                const double *a = &X[3 * (size_t)t[k][0]], *b = &X[3 * (size_t)t[k][1]], *d = &X[3 * (size_t)t[k][2]];
                v += (a[0] * (b[1] * d[2] - b[2] * d[1]) + a[1] * (b[2] * d[0] - b[0] * d[2]) + a[2] * (b[0] * d[1] - b[1] * d[0])) / 6;
            }
        }
    }
    return v;
}

static void cells_surface(Surf *s, FaceCache *fc, const LabPart *p, const double *X, const float *nodev, const float *cellv,
                          const Opts *o, double lo, double hi, Mesh *m, Check *chk) {
    if (fc->ncells != p->ncells || fc->cell_type != p->cell_type || !fc->conn ||
        memcmp(fc->conn, p->conn, (size_t)p->ncells * (size_t)p->cell_type * sizeof(int)) != 0) {
        free(fc->conn), free(fc->faces);
        fc->ncells = p->ncells, fc->cell_type = p->cell_type;
        fc->conn = xrealloc(NULL, (size_t)p->ncells * (size_t)p->cell_type * sizeof(int));
        memcpy(fc->conn, p->conn, (size_t)p->ncells * (size_t)p->cell_type * sizeof(int));
        fc->faces = outer_faces(p, X, &fc->nfaces);
    }
    if (fc->nrest != p->npoints) { /* first seen, or its nodes changed: rest where it is now */
        free(fc->rest);
        fc->rest = xrealloc(NULL, (size_t)p->npoints * 3 * sizeof(double));
        memcpy(fc->rest, X, (size_t)p->npoints * 3 * sizeof(double));
        fc->nrest = p->npoints;
    }
    surf_begin(s, X, fc->rest, nodev, p->npoints);
    bool cut = cut_this(o, p->name);
    double span = fmod(o->cut1 - o->cut0, 360.0);
    if (span <= 0) span += 360;
    bool convex = span <= 180; /* the wedge taken out is convex: it is where both planes' sides meet */
    if (cut) {
        double a0 = o->cut0 * M_PI / 180, a1 = o->cut1 * M_PI / 180;
        s->nrm[0][0] = -sin(a0), s->nrm[0][1] = cos(a0), s->nrm[0][2] = 0; /* positive: counter-clockwise of a0 */
        s->nrm[1][0] = sin(a1), s->nrm[1][1] = -cos(a1), s->nrm[1][2] = 0; /* positive: clockwise of a1 */
        s->dir[0][0] = cos(a0), s->dir[0][1] = sin(a0), s->dir[0][2] = 0;
        s->dir[1][0] = cos(a1), s->dir[1][1] = sin(a1), s->dir[1][2] = 0;
    }
    for (int i = 0; i < fc->nfaces; i++) {
        const FaceRec *F = &fc->faces[i];
        int q[4], a[16], b[16], c[16];
        for (int k = 0; k < F->m; k++) q[k] = node_point(s, F->n[k]);
        float val = cellv ? cellv[F->cell] : 0.0f;
        if (!cut) {
            add_poly(s, q, F->m, val);
        } else if (convex) { /* kept: behind the first plane, or in front of it and behind the second */
            int na = clip(s, q, F->m, 0, true, a);
            add_poly(s, a, na, val);
            int nb = clip(s, q, F->m, 0, false, b);
            int nc = clip(s, b, nb, 1, true, c);
            add_poly(s, c, nc, val);
        } else { /* kept: behind both planes */
            int na = clip(s, q, F->m, 0, true, a);
            int nc = clip(s, a, na, 1, true, c);
            add_poly(s, c, nc, val);
        }
    }
    if (cut && p->cell_type == LAB_HEX) {
        section(s, p, 0, !convex, cellv);
        section(s, p, 1, !convex, cellv);
    }
    if (chk) *chk = check_surface(s);
    finish(s, cos(o->crease_deg * M_PI / 180), cellv != NULL, lo, hi, m);
}

/* ---- blocks parts -------------------------------------------------------------------------------------------- */

typedef struct Bins {
    double lo[3], h[3];
    int g;
    int *start, *list;
} Bins;

static bool in_block(const LabBlock *b, const double x[3]) {
    for (int k = 0; k < 3; k++) {
        double lo = b->origin[k], hi = b->origin[k] + b->n[k] * b->dx[k], eps = 1e-9 * b->dx[k];
        if (!(x[k] > lo + eps && x[k] < hi - eps)) return false;
    }
    return true;
}

static void bins_build(Bins *B, const LabPart *p) {
    memset(B, 0, sizeof *B);
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    int n3 = 0;
    for (int b = 0; b < p->nblocks; b++) {
        const LabBlock *bl = &p->blocks[b];
        if (bl->n[2] <= 1) continue;
        n3++;
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], bl->origin[k]), hi[k] = fmax(hi[k], bl->origin[k] + bl->n[k] * bl->dx[k]);
    }
    if (n3 < 2) return;
    int g = (int)ceil(2 * cbrt((double)n3));
    B->g = g < 1 ? 1 : g > 64 ? 64 : g;
    for (int k = 0; k < 3; k++) B->lo[k] = lo[k], B->h[k] = (hi[k] - lo[k]) / B->g;
    size_t nb = (size_t)B->g * B->g * B->g;
    B->start = xcalloc(nb + 1, sizeof(int));
    for (int pass = 0; pass < 2; pass++) {
        int *fill = pass ? xrealloc(NULL, nb * sizeof(int)) : NULL;
        if (pass) memcpy(fill, B->start, nb * sizeof(int)), B->list = xrealloc(NULL, (size_t)B->start[nb] * sizeof(int) + 4);
        for (int b = 0; b < p->nblocks; b++) {
            const LabBlock *bl = &p->blocks[b];
            if (bl->n[2] <= 1) continue;
            int r[3][2];
            for (int k = 0; k < 3; k++) {
                r[k][0] = (int)floor((bl->origin[k] - B->lo[k]) / B->h[k]), r[k][1] = (int)floor((bl->origin[k] + bl->n[k] * bl->dx[k] - B->lo[k]) / B->h[k]);
                r[k][0] = r[k][0] < 0 ? 0 : r[k][0] >= B->g ? B->g - 1 : r[k][0];
                r[k][1] = r[k][1] < 0 ? 0 : r[k][1] >= B->g ? B->g - 1 : r[k][1];
            }
            for (int z = r[2][0]; z <= r[2][1]; z++)
                for (int y = r[1][0]; y <= r[1][1]; y++)
                    for (int x = r[0][0]; x <= r[0][1]; x++) {
                        size_t i = ((size_t)z * B->g + y) * B->g + x;
                        if (pass) B->list[fill[i]++] = b;
                        else B->start[i + 1]++;
                    }
        }
        if (!pass)
            for (size_t i = 0; i < nb; i++) B->start[i + 1] += B->start[i];
        free(fill);
    }
}

/* another block holds the point (min_level: only blocks finer than that level, -1 for any) */
static bool held(const Bins *B, const LabPart *p, const double x[3], int self, int finer_than) {
    if (!B->g) return false;
    int c[3];
    for (int k = 0; k < 3; k++) {
        c[k] = (int)floor((x[k] - B->lo[k]) / B->h[k]);
        if (c[k] < 0 || c[k] >= B->g) return false;
    }
    size_t i = ((size_t)c[2] * B->g + c[1]) * B->g + c[0];
    for (int j = B->start[i]; j < B->start[i + 1]; j++) {
        int b = B->list[j];
        if (b == self || (finer_than >= 0 && p->blocks[b].level <= finer_than)) continue;
        if (in_block(&p->blocks[b], x)) return true;
    }
    return false;
}

static void blocks_surface(const LabPart *p, const float *val, double lo, double hi, Mesh *m) {
    Bins B;
    bins_build(&B, p);
    mesh_clear(m);
    size_t at = 0;
    for (int b = 0; b < p->nblocks; b++) {
        const LabBlock *bl = &p->blocks[b];
        const float *v = val ? val + at : NULL;
        at += lab_block_cells(bl);
        if (bl->n[2] <= 1) { /* a 2D block: its plane, one side */
            int ni = bl->n[0], nj = bl->n[1];
            double nrm[3] = {0, 0, 0};
            nrm[bl->plane == LAB_PLANE_XY ? 2 : bl->plane == LAB_PLANE_XZ ? 1 : 0] = bl->plane == LAB_PLANE_XZ ? -1 : 1;
            size_t nc = (size_t)(ni + 1) * (nj + 1);
            double *cv = xcalloc(nc, sizeof(double));
            int *cn = xcalloc(nc, sizeof(int)), *id = xrealloc(NULL, nc * sizeof(int));
            for (int j = 0; j < nj; j++)
                for (int i = 0; i < ni; i++)
                    for (int dj = 0; dj < 2; dj++)
                        for (int di = 0; di < 2; di++) {
                            size_t k = (size_t)(j + dj) * (ni + 1) + (i + di);
                            cv[k] += v ? v[(size_t)j * ni + i] : 0, cn[k]++;
                        }
            for (int j = 0; j <= nj; j++)
                for (int i = 0; i <= ni; i++) {
                    size_t k = (size_t)j * (ni + 1) + i;
                    double x[3] = {bl->origin[0], bl->origin[1], bl->origin[2]}, s = cn[k] ? cv[k] / cn[k] : 0;
                    if (bl->plane == LAB_PLANE_XY) x[0] += i * bl->dx[0], x[1] += j * bl->dx[1];
                    else if (bl->plane == LAB_PLANE_XZ) x[0] += i * bl->dx[0], x[2] += j * bl->dx[1];
                    else x[1] += i * bl->dx[0], x[2] += j * bl->dx[1];
                    id[k] = mesh_vertex(m, x, nrm, s, map01(s, lo, hi), x);
                }
            for (int j = 0; j < nj; j++)
                for (int i = 0; i < ni; i++) {
                    int a = id[(size_t)j * (ni + 1) + i], bb = id[(size_t)j * (ni + 1) + i + 1];
                    int c = id[(size_t)(j + 1) * (ni + 1) + i + 1], d = id[(size_t)(j + 1) * (ni + 1) + i];
                    mesh_tri(m, a, bb, c), mesh_tri(m, a, c, d);
                }
            free(cv), free(cn), free(id);
            continue;
        }
        const int n[3] = {bl->n[0], bl->n[1], bl->n[2]};
        for (int ax = 0; ax < 3; ax++)
            for (int side = 0; side < 2; side++) {
                int u = (ax + 1) % 3, w = (ax + 2) % 3, nu = n[u], nw = n[w];
                size_t nc = (size_t)(nu + 1) * (nw + 1);
                double *cv = xcalloc(nc, sizeof(double));
                int *cn = xcalloc(nc, sizeof(int)), *id = xrealloc(NULL, nc * sizeof(int));
                char *kept = xcalloc((size_t)nu * nw, 1);
                double nrm[3] = {0, 0, 0};
                nrm[ax] = side ? 1 : -1;
                for (int bb = 0; bb < nw; bb++)
                    for (int a = 0; a < nu; a++) {
                        int idx[3];
                        idx[ax] = side ? n[ax] - 1 : 0, idx[u] = a, idx[w] = bb;
                        double c[3], out[3];
                        for (int k = 0; k < 3; k++) c[k] = out[k] = bl->origin[k] + (idx[k] + 0.5) * bl->dx[k];
                        out[ax] = bl->origin[ax] + (side ? n[ax] + 0.5 : -0.5) * bl->dx[ax];
                        if (held(&B, p, out, b, -1) || held(&B, p, c, b, bl->level)) continue; /* inside, or drawn finer */
                        kept[(size_t)bb * nu + a] = 1;
                        size_t cell = ((size_t)idx[2] * n[1] + idx[1]) * n[0] + idx[0];
                        double sv = v ? v[cell] : 0;
                        for (int db = 0; db < 2; db++)
                            for (int da = 0; da < 2; da++) {
                                size_t k = (size_t)(bb + db) * (nu + 1) + (a + da);
                                cv[k] += sv, cn[k]++;
                            }
                    }
                for (size_t k = 0; k < nc; k++) id[k] = -1;
                double plane = bl->origin[ax] + (side ? n[ax] : 0) * bl->dx[ax];
                for (int bb = 0; bb < nw; bb++)
                    for (int a = 0; a < nu; a++) {
                        if (!kept[(size_t)bb * nu + a]) continue;
                        const int ca[4] = {a, a + 1, a + 1, a}, cb[4] = {bb, bb, bb + 1, bb + 1};
                        int q[4];
                        for (int r = 0; r < 4; r++) {
                            size_t k = (size_t)cb[r] * (nu + 1) + ca[r];
                            if (id[k] < 0) {
                                double x[3], s = cn[k] ? cv[k] / cn[k] : 0;
                                x[ax] = plane, x[u] = bl->origin[u] + ca[r] * bl->dx[u], x[w] = bl->origin[w] + cb[r] * bl->dx[w];
                                id[k] = mesh_vertex(m, x, nrm, s, map01(s, lo, hi), x);
                            }
                            q[r] = id[k];
                        }
                        if (side) mesh_tri(m, q[0], q[1], q[2]), mesh_tri(m, q[0], q[2], q[3]); /* u x w is the axis */
                        else mesh_tri(m, q[0], q[2], q[1]), mesh_tri(m, q[0], q[3], q[2]);
                    }
                free(cv), free(cn), free(id), free(kept);
            }
    }
    free(B.start), free(B.list);
}

/* ---- writing ------------------------------------------------------------------------------------------------- */

static bool write_ply(const char *path, const Mesh *m, const char *comment) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    fprintf(fp, "ply\nformat binary_little_endian 1.0\ncomment %s\nelement vertex %d\n", comment, m->nv);
    fprintf(fp, "property float x\nproperty float y\nproperty float z\n");
    if (!m->points) fprintf(fp, "property float nx\nproperty float ny\nproperty float nz\n");
    fprintf(fp, "property float value\nproperty float value01\n");
    if (m->points && m->has_radius) fprintf(fp, "property float radius\n");
    if (!m->points) fprintf(fp, "property float rest_x\nproperty float rest_y\nproperty float rest_z\n");
    if (!m->points) fprintf(fp, "element face %d\nproperty list uchar int vertex_indices\n", m->nt);
    fprintf(fp, "end_header\n");
    bool ok = true;
    if (!m->points) {
        ok &= fwrite(m->v, sizeof(float), NV * (size_t)m->nv, fp) == NV * (size_t)m->nv;
        unsigned char buf[13 * 4096];
        size_t k = 0;
        for (int t = 0; t < m->nt; t++) {
            buf[13 * k] = 3;
            memcpy(&buf[13 * k + 1], &m->t[3 * (size_t)t], 12);
            if (++k == 4096) ok &= fwrite(buf, 13, k, fp) == k, k = 0;
        }
        if (k) ok &= fwrite(buf, 13, k, fp) == k;
    } else {
        for (int i = 0; i < m->nv && ok; i++) {
            const float *q = &m->v[NV * (size_t)i];
            float r[6] = {q[0], q[1], q[2], q[6], q[7], m->has_radius ? m->radius[i] : 0};
            ok &= fwrite(r, sizeof(float), m->has_radius ? 6 : 5, fp) == (size_t)(m->has_radius ? 6 : 5);
        }
    }
    ok &= fclose(fp) == 0;
    return ok;
}

/* ---- the field's range, as labfilm chooses it (src/lab/labview.c, labview_range) ------------------------------ */

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void field_range(LabFile *lf, const char *field, int first, int last, int every, double *lo, double *hi, double *mn, double *mx) {
    size_t cap = 1 << 16, n = 0;
    double *v = xrealloc(NULL, cap * sizeof(double));
    *mn = INFINITY, *mx = -INFINITY;
    for (int i = first; i <= last; i += every) {
        LabFrame fr;
        char err[128];
        if (!lab_read_frame(lf, i, &fr, err, sizeof err)) continue;
        for (int p = 0; p < fr.nparts; p++) {
            const LabField *f = lab_find_field(&fr.parts[p], field);
            if (!f) continue;
            size_t stride = f->count > 200000 ? f->count / 200000 : 1;
            for (size_t k = 0; k < f->count; k++) {
                double x = f->data[k];
                if (!isfinite(x)) continue;
                *mn = fmin(*mn, x), *mx = fmax(*mx, x);
                if (k % stride) continue;
                if (n == cap) cap *= 2, v = xrealloc(v, cap * sizeof(double));
                v[n++] = x;
            }
        }
        lab_frame_free(&fr);
    }
    if (n == 0) {
        *lo = 0, *hi = 1, free(v);
        return;
    }
    qsort(v, n, sizeof(double), cmp_double);
    *lo = v[(size_t)(0.005 * (n - 1))], *hi = v[(size_t)(0.995 * (n - 1))];
    if (!(*hi > *lo)) *lo = v[0], *hi = v[n - 1];
    free(v);
    double m = fmax(fabs(*lo), fabs(*hi)); /* signed data: symmetric about zero, as style_pick decides */
    if (*lo < -0.05 * m && *hi > 0.05 * m) *lo = -m, *hi = m;
}

/* ---- frames: a cache of two, and the blend of two of them ---------------------------------------------------- */

typedef struct Cache {
    int idx[2];
    LabFrame fr[2];
    int age[2], clock;
} Cache;

static const LabFrame *get_frame(LabFile *lf, Cache *c, int i) {
    for (int k = 0; k < 2; k++)
        if (c->idx[k] == i) return c->age[k] = ++c->clock, &c->fr[k];
    int k = c->age[0] <= c->age[1] ? 0 : 1;
    if (c->idx[k] >= 0) lab_frame_free(&c->fr[k]);
    char err[160];
    if (!lab_read_frame(lf, i, &c->fr[k], err, sizeof err)) {
        fprintf(stderr, "lab2ply: %s\n", err);
        exit(1);
    }
    c->idx[k] = i, c->age[k] = ++c->clock;
    return &c->fr[k];
}

static const LabPart *match_part(const LabFrame *f, int k, const char *name) {
    if (k < f->nparts && !strcmp(f->parts[k].name, name)) return &f->parts[k];
    return lab_find_part(f, name);
}

static void sanitize(const char *in, char *out, size_t n) {
    size_t k = 0;
    for (; *in && k + 1 < n; in++) {
        char c = *in;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out[k++] = ok ? c : '_';
    }
    out[k] = 0;
    if (!k) snprintf(out, n, "part");
}

int main(int argc, char **argv) {
    Opts o = {.first = 0, .last = -1, .every = 1, .crease_deg = 35};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--field") && i + 1 < argc) o.field = argv[++i];
        else if (!strcmp(a, "--frames") && i + 2 < argc) o.first = atoi(argv[++i]), o.last = atoi(argv[++i]);
        else if (!strcmp(a, "--every") && i + 1 < argc) o.every = atoi(argv[++i]);
        else if (!strcmp(a, "--film") && i + 1 < argc) o.film = atoi(argv[++i]);
        else if (!strcmp(a, "--range") && i + 2 < argc) o.lo = atof(argv[++i]), o.hi = atof(argv[++i]), o.range_given = true;
        else if (!strcmp(a, "--crease") && i + 1 < argc) o.crease_deg = atof(argv[++i]);
        else if (!strcmp(a, "--cut") && i + 2 < argc) o.cut0 = atof(argv[++i]), o.cut1 = atof(argv[++i]), o.cut = true;
        else if (!strcmp(a, "--cut-parts") && i + 1 < argc) o.cut_parts = argv[++i];
        else if (!strcmp(a, "--check")) o.check = true;
        else if (a[0] == '-') return usage(), 1;
        else if (!o.in) o.in = a;
        else if (!o.out) o.out = a;
        else return usage(), 1;
    }
    if (!o.in || !o.out || o.every < 1 || o.film < 0) return usage(), 1;
    if (o.cut && !(fmod(o.cut1 - o.cut0 + 720.0, 360.0) > 0)) return fprintf(stderr, "lab2ply: the cut's two angles are the same\n"), 1;
    char err[256];
    LabFile *lf = lab_open(o.in, err, sizeof err);
    if (!lf) return fprintf(stderr, "lab2ply: %s\n", err), 1;
    int nf = lab_frame_count(lf);
    if (nf == 0) return fprintf(stderr, "lab2ply: %s has no frames\n", o.in), 1;
    if (o.last < 0 || o.last >= nf) o.last = nf - 1;
    if (o.first < 0) o.first = 0;
    if (o.first > o.last) return fprintf(stderr, "lab2ply: frames %d to %d: the result has %d\n", o.first, o.last, nf), 1;
    if (!o.field) { /* the first field the result has */
        LabFrame fr;
        if (lab_read_frame(lf, o.first, &fr, err, sizeof err)) {
            for (int p = 0; p < fr.nparts && !o.field; p++)
                if (fr.parts[p].nfields) {
                    static char name[48];
                    snprintf(name, sizeof name, "%s", fr.parts[p].fields[0].name);
                    o.field = name;
                }
            lab_frame_free(&fr);
        }
        if (!o.field) o.field = "";
    }
    double fmin_all = 0, fmax_all = 0;
    if (o.field[0]) {
        double lo, hi;
        field_range(lf, o.field, o.first, o.last, o.film ? 1 : o.every, &lo, &hi, &fmin_all, &fmax_all);
        if (!o.range_given) o.lo = lo, o.hi = hi;
        printf("lab2ply: field %s, values %.6g to %.6g over frames %d to %d; mapped to 0..1 over %.6g to %.6g (%s)\n", o.field, fmin_all,
               fmax_all, o.first, o.last, o.lo, o.hi, o.range_given ? "given" : "the range labfilm chooses");
    }
    if (!mkdir_p(o.out)) return fprintf(stderr, "lab2ply: cannot make %s\n", o.out), 1;
    int nout = o.film ? o.film : (o.last - o.first) / o.every + 1;
    Text man = {0};
    tprintf(&man, "{\n \"source\": ");
    tjson_str(&man, o.in);
    tprintf(&man, ",\n \"field\": ");
    tjson_str(&man, o.field);
    tprintf(&man, ",\n \"range\": [%.9g, %.9g],\n \"range_rule\": \"%s\",\n \"field_min\": %.9g,\n \"field_max\": %.9g,\n", o.lo, o.hi,
            o.range_given ? "given" : "labfilm: the 0.5 and 99.5 percentiles of the values, symmetric about zero for signed data", fmin_all, fmax_all);
    tprintf(&man, " \"source_frames\": [%d, %d],\n \"every\": %d,\n \"film\": %d,\n \"crease_deg\": %g,\n", o.first, o.last, o.every, o.film, o.crease_deg);
    if (o.cut) {
        tprintf(&man, " \"cut_deg\": [%g, %g],\n \"cut_parts\": ", o.cut0, o.cut1);
        tjson_str(&man, o.cut_parts ? o.cut_parts : "");
        tprintf(&man, ",\n");
    }
    tprintf(&man, " \"frames\": [");
    Cache cache = {.idx = {-1, -1}};
    Surf surf = {0};
    Mesh mesh = {0};
    FaceCache *fcache = NULL;
    int nfc = 0;
    double blo[3] = {INFINITY, INFINITY, INFINITY}, bhi[3] = {-INFINITY, -INFINITY, -INFINITY};
    double *Xb = NULL;
    float *nodeb = NULL, *cellb = NULL;
    size_t capX = 0, capN = 0, capC = 0;
    long total_v = 0, total_t = 0;
    for (int j = 0; j < nout; j++) {
        double s = o.film ? (nout > 1 ? o.first + (double)(o.last - o.first) * j / (nout - 1) : o.first) : o.first + j * o.every;
        int i0 = (int)floor(s);
        double w = s - i0;
        if (i0 >= o.last) i0 = o.last, w = 0;
        int i1 = w > 0 ? i0 + 1 : i0;
        const LabFrame *F0 = get_frame(lf, &cache, i0), *F1 = get_frame(lf, &cache, i1); /* the second keeps the first */
        const LabFrame *near = w < 0.5 ? F0 : F1;
        double time = (1 - w) * F0->time + w * F1->time;
        char dir[1100];
        snprintf(dir, sizeof dir, "%s/frame_%04d", o.out, j);
        mkdir_p(dir);
        tprintf(&man, "%s\n  {\"dir\": \"frame_%04d\", \"time\": %.9g, \"source\": %.6f, \"parts\": [", j ? "," : "", j, time, s);
        char used[256][64];
        int nused = 0, written = 0;
        long fv = 0, ft = 0;
        for (int k = 0; k < near->nparts; k++) {
            const LabPart *P = &near->parts[k];
            const LabPart *Pa = match_part(F0, k, P->name), *Pb = match_part(F1, k, P->name);
            if (P->kind == LAB_CELLS && P->cell_type == LAB_LINE) continue;
            bool blend = w > 0 && Pa && Pb && Pa->kind == Pb->kind && Pa->npoints == Pb->npoints && P->kind != LAB_BLOCKS;
            const double *X = P->xyz;
            if (blend && P->npoints) {
                if ((size_t)P->npoints * 3 > capX) capX = (size_t)P->npoints * 3, Xb = xrealloc(Xb, capX * sizeof(double));
                for (size_t q = 0; q < (size_t)P->npoints * 3; q++) Xb[q] = (1 - w) * Pa->xyz[q] + w * Pb->xyz[q];
                X = Xb;
            }
            const LabField *f = lab_find_field(P, o.field);
            const float *nodev = NULL, *cellv = NULL;
            size_t ncell = P->kind == LAB_BLOCKS ? lab_part_ncells(P) : (size_t)P->ncells;
            bool at_node = f && (P->kind == LAB_POINTS || f->location == LAB_AT_NODE) && f->count == (size_t)P->npoints;
            bool at_cell = f && P->kind != LAB_POINTS && f->location == LAB_AT_CELL && f->count == ncell;
            if (at_node) {
                nodev = f->data;
                const LabField *fa = lab_find_field(Pa, o.field), *fb = lab_find_field(Pb, o.field);
                if (blend && fa && fb && fa->count == f->count && fb->count == f->count) {
                    if (f->count > capN) capN = f->count, nodeb = xrealloc(nodeb, capN * sizeof(float));
                    for (size_t q = 0; q < f->count; q++) nodeb[q] = (float)((1 - w) * fa->data[q] + w * fb->data[q]);
                    nodev = nodeb;
                }
            } else if (at_cell) {
                cellv = f->data;
                const LabField *fa = lab_find_field(Pa, o.field), *fb = lab_find_field(Pb, o.field);
                bool same = w > 0 && Pa && Pb && fa && fb && fa->count == f->count && fb->count == f->count;
                if (same && P->kind == LAB_CELLS)
                    same = Pa->ncells == Pb->ncells && Pa->cell_type == Pb->cell_type &&
                           !memcmp(Pa->conn, Pb->conn, (size_t)Pa->ncells * (size_t)Pa->cell_type * sizeof(int));
                if (same && P->kind == LAB_BLOCKS)
                    same = Pa->nblocks == Pb->nblocks && !memcmp(Pa->blocks, Pb->blocks, (size_t)Pa->nblocks * sizeof(LabBlock));
                if (same) {
                    if (f->count > capC) capC = f->count, cellb = xrealloc(cellb, capC * sizeof(float));
                    for (size_t q = 0; q < f->count; q++) cellb[q] = (float)((1 - w) * fa->data[q] + w * fb->data[q]);
                    cellv = cellb;
                }
            }
            Check chk = {0};
            bool have_chk = false;
            if (P->kind == LAB_CELLS) {
                int c = 0;
                while (c < nfc && strcmp(fcache[c].name, P->name)) c++;
                if (c == nfc) {
                    fcache = xrealloc(fcache, (size_t)(nfc + 1) * sizeof *fcache);
                    memset(&fcache[nfc], 0, sizeof *fcache);
                    snprintf(fcache[nfc].name, sizeof fcache[nfc].name, "%s", P->name);
                    nfc++;
                }
                cells_surface(&surf, &fcache[c], P, X, nodev, cellv, &o, o.lo, o.hi, &mesh, o.check ? &chk : NULL);
                have_chk = o.check;
            } else if (P->kind == LAB_BLOCKS) {
                blocks_surface(P, cellv, o.lo, o.hi, &mesh);
            } else { /* points */
                mesh_clear(&mesh);
                mesh.points = true;
                const LabField *r = lab_find_field(P, "radius");
                mesh.has_radius = r && r->count == (size_t)P->npoints;
                if (mesh.has_radius) mesh.radius = xrealloc(mesh.radius, (size_t)P->npoints * sizeof(float));
                double zero[3] = {0, 0, 0};
                for (int q = 0; q < P->npoints; q++) {
                    double v = nodev ? nodev[q] : 0;
                    mesh_vertex(&mesh, &X[3 * (size_t)q], zero, v, map01(v, o.lo, o.hi), &X[3 * (size_t)q]);
                    if (mesh.has_radius) mesh.radius[q] = r->data[q];
                }
            }
            char base[64], file[64];
            sanitize(P->name, base, sizeof base);
            snprintf(file, sizeof file, "%s", base);
            for (int u = 0, again = 1; again; u++) {
                again = 0;
                for (int q = 0; q < nused; q++)
                    if (!strcmp(used[q], file)) again = 1;
                if (again) snprintf(file, sizeof file, "%.40s_%d", base, u + 2);
            }
            if (nused < 256) snprintf(used[nused++], sizeof used[0], "%s", file);
            char path[1300], comment[200];
            snprintf(path, sizeof path, "%s/%s.ply", dir, file);
            snprintf(comment, sizeof comment, "lab2ply: part %.47s, frame %.4f of the result (time %.6g), field %.47s", P->name, s, time, o.field);
            if (!write_ply(path, &mesh, comment)) return fprintf(stderr, "lab2ply: cannot write %s (disk full?)\n", path), 1;
            double plo[3] = {INFINITY, INFINITY, INFINITY}, phi[3] = {-INFINITY, -INFINITY, -INFINITY}, vlo = INFINITY, vhi = -INFINITY;
            for (int q = 0; q < mesh.nv; q++)
                for (int d = 0; d < 3; d++) {
                    double x = mesh.v[NV * (size_t)q + d];
                    plo[d] = fmin(plo[d], x), phi[d] = fmax(phi[d], x);
                    if (d == 0) vlo = fmin(vlo, mesh.v[NV * (size_t)q + 6]), vhi = fmax(vhi, mesh.v[NV * (size_t)q + 6]);
                }
            for (int d = 0; d < 3; d++) blo[d] = fmin(blo[d], plo[d]), bhi[d] = fmax(bhi[d], phi[d]);
            fv += mesh.nv, ft += mesh.nt;
            tprintf(&man, "%s\n   {\"name\": ", written++ ? "," : "");
            tjson_str(&man, P->name);
            tprintf(&man, ", \"file\": \"frame_%04d/%s.ply\", \"kind\": \"%s\", \"vertices\": %d, \"triangles\": %d, \"field\": %s%s}", j, file,
                    mesh.points ? "points" : "surface", mesh.nv, mesh.nt, (nodev || cellv) ? "true" : "false",
                    cut_this(&o, P->name) && P->kind == LAB_CELLS ? ", \"cut\": true" : "");
            if (o.check) {
                printf("frame %d (source %.4f) part %s: %d vertices, %d triangles", j, s, P->name, mesh.nv, mesh.nt);
                if (P->kind == LAB_CELLS) printf(" from %d nodes and %d cells of %d nodes", P->npoints, P->ncells, P->cell_type);
                if (P->kind == LAB_BLOCKS) printf(" from %d blocks, %zu cells", P->nblocks, lab_part_ncells(P));
                if (P->kind == LAB_POINTS) printf(" (points)");
                printf("\n");
                if (have_chk) {
                    printf("  welded: %d points, %d edges: %d open, %d non-manifold, %d misoriented; Euler characteristic %d\n", chk.points, chk.edges,
                           chk.open, chk.nonmanifold, chk.misoriented, chk.euler);
                    printf("  volume inside the surface %.9g m^3", chk.volume);
                    if (P->cell_type == LAB_HEX) printf(", of its cells %.9g m^3", cells_volume(P, X));
                    printf("\n");
                }
                printf("  bounds of the surface: %.9g %.9g %.9g to %.9g %.9g %.9g m\n", plo[0], plo[1], plo[2], phi[0], phi[1], phi[2]);
                if (P->kind != LAB_BLOCKS && P->npoints) {
                    double nlo[3] = {INFINITY, INFINITY, INFINITY}, nhi[3] = {-INFINITY, -INFINITY, -INFINITY};
                    char *usedn = xcalloc((size_t)P->npoints, 1);
                    if (P->kind == LAB_CELLS)
                        for (size_t q = 0; q < (size_t)P->ncells * P->cell_type; q++) usedn[P->conn[q]] = 1;
                    else memset(usedn, 1, (size_t)P->npoints);
                    for (int q = 0; q < P->npoints; q++)
                        if (usedn[q])
                            for (int d = 0; d < 3; d++) nlo[d] = fmin(nlo[d], X[3 * (size_t)q + d]), nhi[d] = fmax(nhi[d], X[3 * (size_t)q + d]);
                    free(usedn);
                    printf("  bounds of its cells' nodes: %.9g %.9g %.9g to %.9g %.9g %.9g m\n", nlo[0], nlo[1], nlo[2], nhi[0], nhi[1], nhi[2]);
                }
                if (nodev || cellv) {
                    double alo = INFINITY, ahi = -INFINITY;
                    size_t cnt = nodev ? (size_t)P->npoints : ncell;
                    const float *src = nodev ? nodev : cellv;
                    for (size_t q = 0; q < cnt; q++) alo = fmin(alo, src[q]), ahi = fmax(ahi, src[q]);
                    printf("  %s on the surface %.6g to %.6g, at all its %s %.6g to %.6g\n", o.field, vlo, vhi, nodev ? "nodes" : "cells", alo, ahi);
                }
            }
        }
        tprintf(&man, "]}");
        total_v += fv, total_t += ft;
        if (!o.check && (j == 0 || j == nout - 1 || (j + 1) % 25 == 0))
            printf("frame %d of %d (source %.3f, time %.6g): %ld vertices, %ld triangles\n", j + 1, nout, s, time, fv, ft);
    }
    for (int d = 0; d < 3; d++)
        if (!isfinite(blo[d]) || !isfinite(bhi[d])) blo[d] = bhi[d] = 0; /* nothing drawn */
    tprintf(&man, "\n ],\n \"bounds\": [[%.9g, %.9g, %.9g], [%.9g, %.9g, %.9g]],\n \"header\": %s\n}\n", blo[0], blo[1], blo[2], bhi[0], bhi[1], bhi[2],
            lab_header(lf)[0] == '{' ? lab_header(lf) : "{}");
    char mpath[1100];
    snprintf(mpath, sizeof mpath, "%s/manifest.json", o.out);
    FILE *fp = fopen(mpath, "w");
    if (!fp || fwrite(man.p, 1, man.len, fp) != man.len || fclose(fp) != 0) return fprintf(stderr, "lab2ply: cannot write %s\n", mpath), 1;
    printf("lab2ply: %d frames to %s, %ld vertices and %ld triangles in all; bounds %.6g %.6g %.6g to %.6g %.6g %.6g m\n", nout, o.out, total_v,
           total_t, blo[0], blo[1], blo[2], bhi[0], bhi[1], bhi[2]);
    for (int c = 0; c < nfc; c++) free(fcache[c].conn), free(fcache[c].faces), free(fcache[c].rest);
    free(fcache), free(Xb), free(nodeb), free(cellb), free(man.p);
    surf_free(&surf), mesh_free(&mesh);
    for (int k = 0; k < 2; k++)
        if (cache.idx[k] >= 0) lab_frame_free(&cache.fr[k]);
    lab_close_file(lf);
    return 0;
}
