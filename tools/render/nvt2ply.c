/* nvt2ply.c - an engine result file (results.nvt, src/ctl/thermal_io.c) as PLY surfaces for a beauty renderer: per stored
 * time, or per state between two stored times, the outer faces of the elements alive then, one file per element group
 * (part, support, plate), every vertex where the solver put its node, the displacement multiplied by a stated factor,
 * and the von Mises stress on it. Written for the layer-by-layer LPBF build (docs/contracts/lpbf-build.md), whose file
 * holds one stored time per layer and one after the cut with mech_u, mech_vm, elem_birth, elem_death and elem_group;
 * any result with mech_u and mech_vm reads (the FFF print's too). Nothing here invents shape or value
 * (tools/render/README.md, "The metal printing film").
 *
 *   build:  cc -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra -o build/nvt2ply tools/render/nvt2ply.c src/core/json.c src/core/sbuf.c -lm
 *   use:    build/nvt2ply RESULTS.nvt OUTDIR [--scale F] [--range LO HI] [--times A B] [--every K] [--blend A B K]...
 *
 * Written into OUTDIR: state_NNNN/GROUP.ply for every state and element group that has faces (binary, little endian),
 * and manifest.json, which lists the states with their stored times, the files and their counts, the bounds over all
 * states, the colour range and the result file's own header and summary.
 *   alive       element e is drawn at stored time i unless i < elem_birth[e] or (elem_death[e] >= 0 and i >= elem_death[e])
 *               (lpbf-build.md section 5); a result without those arrays draws every element
 *   outer face  a face of an alive element that no other alive element shares (of any group), oriented away from its
 *               element; so the part's face on a support is not drawn while the support stands, and is once it is gone
 *   vertex      one per node and face direction: the faces of a voxel mesh point along +-x, +-y or +-z before they
 *               deform, and a vertex is shared by the faces of one direction at its node only, so that every edge of
 *               the voxels stays sharp and a bent face stays smooth. Its normal is the area-weighted mean of those faces'
 *               normals, deformed. Per vertex (all float): x y z (m, the rest position plus F times the displacement),
 *               nx ny nz, value (von Mises, Pa), value01 (value mapped over the range), rest_x rest_y rest_z (m, the
 *               undeformed node), ux uy uz (m, the displacement itself, not multiplied), node (the node's index)
 *   faces       triangles, each quadrilateral split along its shorter diagonal as deformed
 *   state       a stored time i, or a blend between stored times A and B at s in (0, 1): u = (1 - s) u_A + s u_B and the
 *               stress likewise, drawn on the elements alive at B. That is presentation; in this linear model the
 *               displacement blend is the equilibrium under the fraction s of the force B released (the cut), the
 *               blended von Mises an interpolation between two computed fields
 *
 *   --scale F       the displacement factor (1): every vertex at rest + F u. A film says F
 *   --range LO HI   the colour map's range in Pa. By default 0 to the 99.5 percentile of the von Mises stress at the
 *                   nodes of the part's alive elements over every stored time
 *   --times A B     the stored times A to B (all by default; negative counts from the end, -1 the last); --every K
 *   --blend A B K   K - 1 states between stored times A and B (s = k / K), after A; repeatable
 * Refused: a mesh whose elements are not all the same size (an adaptive mesh's coarse faces meet four fine ones and would
 * be drawn inside the part), a result without mech_u or mech_vm, more than 2^24 nodes (the node index is a float). */
#define _DEFAULT_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../../src/core/json.h"

static const char MAGIC[8] = {'N', 'V', 'T', 'H', 'R', '0', '0', '1'};
static const int HF[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}}; /* src/fem/hex8.c */

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("nvt2ply: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory (%zu bytes)", n);
    return q;
}
static void *xcalloc(size_t n, size_t s) {
    void *q = calloc(n ? n : 1, s);
    if (!q) die("out of memory (%zu x %zu bytes)", n, s);
    return q;
}
static void *xmalloc(size_t n) { return xrealloc(NULL, n); }

static bool mkdir_p(const char *path) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(buf, 0755) && errno != EEXIST) return false;
            *p = '/';
        }
    return !mkdir(buf, 0755) || errno == EEXIST;
}

/* ---- the result file: a JSON header, then the arrays it lists, little endian ------------------------------------ */

typedef struct {
    char name[32];
    char type; /* d float64, i int32, b uint8, c int8 */
    size_t count;
    off_t offset;
} Arr;

typedef struct {
    FILE *f;
    JsonValue *h;
    int nn, ne, no;
    Arr arr[128];
    int narr;
} Nvt;

static size_t tsize(char t) { return t == 'd' ? 8 : (t == 'i' ? 4 : 1); }

static void nvt_open(Nvt *r, const char *path) {
    memset(r, 0, sizeof *r);
    r->f = fopen(path, "rb");
    if (!r->f) die("%s: cannot open", path);
    char m[8];
    uint64_t hl = 0;
    if (fread(m, 1, 8, r->f) != 8 || memcmp(m, MAGIC, 8) || fread(&hl, 8, 1, r->f) != 1 || hl == 0 || hl > (64u << 20))
        die("%s: not an engine result file (results.nvt)", path);
    char *t = xmalloc((size_t)hl);
    if (fread(t, 1, (size_t)hl, r->f) != hl) die("%s: the header is cut short", path);
    JsonError je;
    r->h = json_parse(t, (size_t)hl, NULL, &je);
    free(t);
    if (!r->h) die("%s: unreadable header (%s)", path, je.message);
    long long v = json_get_int(r->h, "version", 0);
    if (strcmp(json_get_str(r->h, "format", ""), "navier-am-thermal-results") || v < 1 || v > 4) die("%s: unsupported format", path);
    long long nn = json_get_int(r->h, "nnodes", -1), ne = json_get_int(r->h, "nelems", -1), no = json_get_int(r->h, "noutputs", -1);
    if (nn <= 0 || ne <= 0 || no <= 0 || nn > (1 << 24) || ne > 200000000LL || no > 100000)
        die("%s: sizes the exporter refuses (%lld nodes, %lld elements, %lld stored times; at most 2^24 nodes)", path, nn, ne, no);
    r->nn = (int)nn, r->ne = (int)ne, r->no = (int)no;
    const JsonValue *al = json_get(r->h, "arrays");
    off_t off = 16 + (off_t)hl;
    if (json_len(al) > sizeof r->arr / sizeof r->arr[0]) die("%s: too many arrays", path);
    for (size_t i = 0; i < json_len(al); i++) {
        const JsonValue *o = json_at(al, i);
        Arr *a = &r->arr[r->narr++];
        snprintf(a->name, sizeof a->name, "%s", json_get_str(o, "name", ""));
        a->type = json_get_str(o, "type", "?")[0];
        a->count = (size_t)json_get_int(o, "count", 0);
        a->offset = off;
        off += (off_t)(a->count * tsize(a->type));
    }
    struct stat st;
    if (fstat(fileno(r->f), &st) || st.st_size < off)
        die("%s: %lld bytes, the header lists %lld", path, (long long)st.st_size, (long long)off);
}

static const Arr *nvt_find(const Nvt *r, const char *name) {
    for (int i = 0; i < r->narr; i++)
        if (!strcmp(r->arr[i].name, name)) return &r->arr[i];
    return NULL;
}

/* n values from element `first` of an array, which must have the type and count given (count 0: any) */
static void *nvt_read(const Nvt *r, const char *name, char type, size_t count, size_t first, size_t n, void *dst) {
    const Arr *a = nvt_find(r, name);
    if (!a) return NULL;
    if (a->type != type || (count && a->count != count) || first + n > a->count)
        die("array %s: type %c count %zu, expected %c %zu (reading %zu at %zu)", name, a->type, a->count, type, count, n, first);
    if (!dst) dst = xmalloc(n * tsize(type));
    if (fseeko(r->f, a->offset + (off_t)(first * tsize(type)), SEEK_SET) || fread(dst, tsize(type), n, r->f) != n)
        die("array %s: cannot read", name);
    return dst;
}

/* ---- the faces of the mesh, matched once: each face with the one or two elements it belongs to ------------------ */

typedef struct {
    int k[4];
    int e, f;
} FaceRec;

static int cmp_face(const void *a, const void *b) {
    const FaceRec *x = a, *y = b;
    for (int i = 0; i < 4; i++)
        if (x->k[i] != y->k[i]) return x->k[i] < y->k[i] ? -1 : 1;
    return 0;
}

typedef struct {
    int n;
    int *e1, *e2;            /* e2 < 0: a face of the mesh's own boundary */
    unsigned char *f1, *f2;
} Faces;

static void faces_build(Faces *F, const int *conn, int ne) {
    size_t nf = 6 * (size_t)ne;
    FaceRec *r = xmalloc(nf * sizeof *r);
    for (int e = 0; e < ne; e++)
        for (int f = 0; f < 6; f++) {
            FaceRec *q = &r[6 * (size_t)e + (size_t)f];
            for (int a = 0; a < 4; a++) q->k[a] = conn[8 * (size_t)e + (size_t)HF[f][a]];
            for (int a = 1; a < 4; a++)
                for (int b = a; b > 0 && q->k[b - 1] > q->k[b]; b--) {
                    int t = q->k[b];
                    q->k[b] = q->k[b - 1], q->k[b - 1] = t;
                }
            q->e = e, q->f = f;
        }
    qsort(r, nf, sizeof *r, cmp_face);
    F->e1 = xmalloc(nf * sizeof(int)), F->e2 = xmalloc(nf * sizeof(int));
    F->f1 = xmalloc(nf), F->f2 = xmalloc(nf);
    F->n = 0;
    for (size_t i = 0; i < nf;) {
        size_t j = i + 1;
        while (j < nf && !cmp_face(&r[i], &r[j])) j++;
        if (j - i > 2) die("a face is shared by %zu elements: the mesh is not a conforming hexahedral mesh", j - i);
        F->e1[F->n] = r[i].e, F->f1[F->n] = (unsigned char)r[i].f;
        F->e2[F->n] = j - i == 2 ? r[i + 1].e : -1, F->f2[F->n] = j - i == 2 ? (unsigned char)r[i + 1].f : 0;
        F->n++;
        i = j;
    }
    free(r);
}

/* ---- a surface being written -------------------------------------------------------------------------------------- */

enum { NV = 15 }; /* x y z nx ny nz value value01 rest_x rest_y rest_z ux uy uz node */

typedef struct {
    float *v;
    int nv, capv;
    int *t;
    int nt, capt;
    double *nacc; /* normal accumulators, 3 per vertex */
    int quads;
} Mesh;

static void newell(const double P[4][3], double n[3]) {
    n[0] = n[1] = n[2] = 0;
    for (int a = 0; a < 4; a++) {
        const double *p = P[a], *q = P[(a + 1) % 4];
        n[0] += (p[1] - q[1]) * (p[2] + q[2]);
        n[1] += (p[2] - q[2]) * (p[0] + q[0]);
        n[2] += (p[0] - q[0]) * (p[1] + q[1]);
    }
    for (int k = 0; k < 3; k++) n[k] *= 0.5; /* twice the area vector halved: the area vector */
}

static bool write_ply(const char *path, const Mesh *m, const char *comment) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    fprintf(fp, "ply\nformat binary_little_endian 1.0\ncomment %s\nelement vertex %d\n", comment, m->nv);
    static const char *P[NV] = {"x", "y", "z", "nx", "ny", "nz", "value", "value01", "rest_x", "rest_y", "rest_z", "ux", "uy", "uz", "node"};
    for (int k = 0; k < NV; k++) fprintf(fp, "property float %s\n", P[k]);
    fprintf(fp, "element face %d\nproperty list uchar int vertex_indices\nend_header\n", m->nt);
    bool ok = fwrite(m->v, sizeof(float), NV * (size_t)m->nv, fp) == NV * (size_t)m->nv;
    unsigned char buf[13 * 4096];
    size_t k = 0;
    for (int t = 0; t < m->nt; t++) {
        buf[13 * k] = 3;
        memcpy(&buf[13 * k + 1], &m->t[3 * (size_t)t], 12);
        if (++k == 4096) ok &= fwrite(buf, 13, k, fp) == k, k = 0;
    }
    if (k) ok &= fwrite(buf, 13, k, fp) == k;
    ok &= fclose(fp) == 0;
    return ok;
}

static double map01(double v, double lo, double hi) {
    double t = hi > lo ? (v - lo) / (hi - lo) : 0.0;
    return t < 0 ? 0 : (t > 1 ? 1 : t);
}

/* ---- one state ---------------------------------------------------------------------------------------------------- */

typedef struct {
    int a, b;
    double s;
} State;

static void alive_at(int i, int ne, const int *birth, const int *death, unsigned char *alive) {
    for (int e = 0; e < ne; e++) {
        bool hide = (birth && i < birth[e]) || (death && death[e] >= 0 && i >= death[e]);
        alive[e] = !hide;
    }
}

static const char *group_name(int g, char *buf, size_t n) {
    static const char *NAMES[3] = {"part", "support", "plate"};
    if (g >= 0 && g < 3) return NAMES[g];
    snprintf(buf, n, "group%d", g);
    return buf;
}

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL;
    double scale = 1, lo = 0, hi = -1;
    bool have_range = false;
    int ta = 0, tb = -1, every = 1, nblend = 0, blend[32][3];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--scale") && i + 1 < argc) scale = atof(argv[++i]);
        else if (!strcmp(a, "--range") && i + 2 < argc) lo = atof(argv[++i]), hi = atof(argv[++i]), have_range = true;
        else if (!strcmp(a, "--times") && i + 2 < argc) ta = atoi(argv[++i]), tb = atoi(argv[++i]);
        else if (!strcmp(a, "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(a, "--blend") && i + 3 < argc && nblend < 32) {
            blend[nblend][0] = atoi(argv[++i]), blend[nblend][1] = atoi(argv[++i]), blend[nblend][2] = atoi(argv[++i]);
            nblend++;
        } else if (a[0] == '-' && a[1] == '-') die("unknown option %s (see the head of tools/render/nvt2ply.c)", a);
        else if (!in) in = a;
        else if (!out) out = a;
        else die("one result file and one output folder");
    }
    if (!in || !out) {
        fprintf(stderr, "usage: nvt2ply RESULTS.nvt OUTDIR [--scale F] [--range LO HI] [--times A B] [--every K] [--blend A B K]...\n");
        return 2;
    }
    if (every < 1) every = 1;
    Nvt r;
    nvt_open(&r, in);
    int nn = r.nn, ne = r.ne, no = r.no;
    if (!json_get_bool(r.h, "has_mechanical", false) || !nvt_find(&r, "mech_u") || !nvt_find(&r, "mech_vm"))
        die("%s holds no displacement and stress (mech_u, mech_vm)", in);
    double *X = nvt_read(&r, "xyz", 'd', 3 * (size_t)nn, 0, 3 * (size_t)nn, NULL);
    int *conn = nvt_read(&r, "conn", 'i', 8 * (size_t)ne, 0, 8 * (size_t)ne, NULL);
    int *birth = nvt_read(&r, "elem_birth", 'i', (size_t)ne, 0, (size_t)ne, NULL);
    int *death = nvt_read(&r, "elem_death", 'i', (size_t)ne, 0, (size_t)ne, NULL);
    unsigned char *group = nvt_read(&r, "elem_group", 'b', (size_t)ne, 0, (size_t)ne, NULL);
    double *times = nvt_read(&r, "times", 'd', (size_t)no, 0, (size_t)no, NULL);
    for (size_t q = 0; q < 8 * (size_t)ne; q++)
        if (conn[q] < 0 || conn[q] >= nn) die("element %zu names node %d of %d", q / 8, conn[q], nn);
    /* one element size: the faces of a uniform voxel mesh match one to one */
    double h0[3] = {0, 0, 0};
    for (int e = 0; e < ne; e++) {
        double mn[3] = {INFINITY, INFINITY, INFINITY}, mx[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) {
                double x = X[3 * (size_t)conn[8 * (size_t)e + (size_t)a] + (size_t)k];
                mn[k] = fmin(mn[k], x), mx[k] = fmax(mx[k], x);
            }
        for (int k = 0; k < 3; k++) {
            double hk = mx[k] - mn[k];
            if (e == 0) h0[k] = hk;
            else if (fabs(hk - h0[k]) > 1e-6 * h0[k])
                die("element %d is %.6g m along axis %d, element 0 %.6g m: an adaptive mesh, which this exporter does not draw", e, hk, k, h0[k]);
        }
    }
    if (ta < 0) ta += no;
    if (tb < 0) tb += no;
    if (ta < 0 || tb >= no || ta > tb) die("--times %d %d: the file has stored times 0 to %d", ta, tb, no - 1);
    /* the states: stored times, with the blends after their first stored time */
    State *st = xmalloc(((size_t)no + 32 * 1024) * sizeof *st);
    int ns = 0;
    for (int i = ta; i <= tb; i++) {
        if ((i - ta) % every && i != tb) continue;
        st[ns++] = (State){i, i, 0.0};
        for (int k = 0; k < nblend; k++) {
            int A = blend[k][0] < 0 ? blend[k][0] + no : blend[k][0], B = blend[k][1] < 0 ? blend[k][1] + no : blend[k][1], K = blend[k][2];
            if (A != i) continue;
            if (B < 0 || B >= no || K < 2 || K > 1024) die("--blend %d %d %d: stored times 0 to %d, 2 to 1024 steps", blend[k][0], blend[k][1], K, no - 1);
            for (int j = 1; j < K; j++) st[ns++] = (State){A, B, (double)j / K};
        }
    }
    printf("nvt2ply: %s: %d nodes, %d elements of %.4g x %.4g x %.4g mm, %d stored times; %d states, displacement x %g\n", in, nn, ne,
           1e3 * h0[0], 1e3 * h0[1], 1e3 * h0[2], no, ns, scale);
    fflush(stdout);
    Faces F;
    faces_build(&F, conn, ne);
    unsigned char *alive = xmalloc((size_t)ne);
    double *ua = xmalloc(3 * (size_t)nn * sizeof(double)), *ub = xmalloc(3 * (size_t)nn * sizeof(double));
    double *va = xmalloc((size_t)nn * sizeof(double)), *vb = xmalloc((size_t)nn * sizeof(double));
    /* the range: 0 to the 99.5 percentile of the part's alive nodes over every stored time (two passes, a fine histogram) */
    if (!have_range) {
        unsigned char *mark = xmalloc((size_t)nn);
        enum { NB = 1 << 20 };
        double vmax = 0;
        uint64_t *hist = xcalloc(NB, sizeof(uint64_t)), total = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < no; i++) {
                nvt_read(&r, "mech_vm", 'd', (size_t)no * (size_t)nn, (size_t)i * (size_t)nn, (size_t)nn, va);
                alive_at(i, ne, birth, death, alive);
                memset(mark, 0, (size_t)nn);
                for (int e = 0; e < ne; e++)
                    if (alive[e] && (!group || group[e] == 0))
                        for (int a = 0; a < 8; a++) mark[conn[8 * (size_t)e + (size_t)a]] = 1;
                for (int n = 0; n < nn; n++) {
                    if (!mark[n]) continue;
                    if (pass == 0) vmax = fmax(vmax, va[n]);
                    else if (vmax > 0) {
                        int bin = (int)(va[n] / vmax * (NB - 1));
                        hist[bin < 0 ? 0 : (bin >= NB ? NB - 1 : bin)]++, total++;
                    }
                }
            }
        uint64_t acc = 0, want = (uint64_t)ceil(0.995 * (double)total);
        int bin = 0;
        while (bin < NB - 1 && acc + hist[bin] < want) acc += hist[bin++];
        lo = 0, hi = vmax > 0 ? (bin + 1.0) / (NB - 1) * vmax : 1.0;
        if (hi > vmax) hi = vmax;
        printf("nvt2ply: colour range 0 to %.6g Pa (the 99.5 percentile; the largest is %.6g Pa)\n", hi, vmax);
        free(hist), free(mark);
    }
    /* the surfaces */
    int *vidx = xmalloc(6 * (size_t)nn * sizeof(int));
    for (size_t q = 0; q < 6 * (size_t)nn; q++) vidx[q] = -1;
    int *touched = NULL, ntouched = 0, captouched = 0;
    int *quad_e = xmalloc((size_t)F.n * sizeof(int));
    unsigned char *quad_f = xmalloc((size_t)F.n), *quad_g = xmalloc((size_t)F.n);
    Mesh m = {0};
    JsonValue *man = json_object();
    json_set_string(man, "format", "nvt2ply");
    json_set_string(man, "source", in);
    json_set_string(man, "field", "von_mises");
    json_set_string(man, "unit", "Pa");
    json_set_number(man, "scale", scale);
    JsonValue *rg = json_set_array(man, "range");
    json_push(rg, json_number(lo)), json_push(rg, json_number(hi));
    json_set_int(man, "stored_times", no);
    json_set(man, "times", json_numbers(times, (size_t)no));
    json_set(man, "element_size_m", json_numbers(h0, 3));
    JsonValue *frames = json_set_array(man, "frames");
    double blo[3] = {INFINITY, INFINITY, INFINITY}, bhi[3] = {-INFINITY, -INFINITY, -INFINITY};
    double rlo[3] = {INFINITY, INFINITY, INFINITY}, rhi[3] = {-INFINITY, -INFINITY, -INFINITY};
    int cached_a = -1, cached_b = -1;
    long total_v = 0, total_t = 0;
    for (int j = 0; j < ns; j++) {
        State S = st[j];
        if (S.a != cached_a) {
            nvt_read(&r, "mech_u", 'd', 3 * (size_t)no * (size_t)nn, 3 * (size_t)S.a * (size_t)nn, 3 * (size_t)nn, ua);
            nvt_read(&r, "mech_vm", 'd', (size_t)no * (size_t)nn, (size_t)S.a * (size_t)nn, (size_t)nn, va);
            cached_a = S.a;
        }
        if (S.b != S.a && S.b != cached_b) {
            nvt_read(&r, "mech_u", 'd', 3 * (size_t)no * (size_t)nn, 3 * (size_t)S.b * (size_t)nn, 3 * (size_t)nn, ub);
            nvt_read(&r, "mech_vm", 'd', (size_t)no * (size_t)nn, (size_t)S.b * (size_t)nn, (size_t)nn, vb);
            cached_b = S.b;
        }
        double s = S.b == S.a ? 0.0 : S.s, w0 = 1 - s;
        alive_at(s > 0 ? S.b : S.a, ne, birth, death, alive);
        /* the outer faces of the alive elements, with their group */
        int nq = 0;
        for (int q = 0; q < F.n; q++) {
            bool a1 = alive[F.e1[q]], a2 = F.e2[q] >= 0 && alive[F.e2[q]];
            if (a1 == a2) continue;
            int e = a1 ? F.e1[q] : F.e2[q];
            quad_e[nq] = e, quad_f[nq] = a1 ? F.f1[q] : F.f2[q], quad_g[nq] = group ? group[e] : 0;
            nq++;
        }
        char dir[4200];
        snprintf(dir, sizeof dir, "%s/state_%04d", out, j);
        if (!mkdir_p(dir)) die("cannot create %s", dir);
        JsonValue *fr = json_object();
        json_set_int(fr, "index", j);
        json_set_int(fr, "a", S.a), json_set_int(fr, "b", S.b), json_set_number(fr, "s", s);
        json_set_number(fr, "time", w0 * times[S.a] + s * times[S.b]);
        JsonValue *parts = json_set_array(fr, "parts");
        double umax_part = 0, vmax_surf = 0;
        long fv = 0, ft = 0;
        bool present[256] = {false};
        for (int q = 0; q < nq; q++) present[quad_g[q]] = true;
        for (int g = 0; g < 256; g++) {
            if (!present[g]) continue;
            m.nv = m.nt = m.quads = 0;
            for (int q = 0; q < nq; q++) {
                if (quad_g[q] != g) continue;
                int e = quad_e[q], f = quad_f[q], nd[4];
                double R[4][3], P[4][3], ec[3] = {0, 0, 0}, fc[3] = {0, 0, 0}, nr[3], nd_[3];
                for (int a = 0; a < 8; a++)
                    for (int k = 0; k < 3; k++) ec[k] += X[3 * (size_t)conn[8 * (size_t)e + (size_t)a] + (size_t)k] / 8;
                for (int a = 0; a < 4; a++) nd[a] = conn[8 * (size_t)e + (size_t)HF[f][a]];
                for (int a = 0; a < 4; a++)
                    for (int k = 0; k < 3; k++) R[a][k] = X[3 * (size_t)nd[a] + (size_t)k], fc[k] += R[a][k] / 4;
                newell(R, nr);
                if (nr[0] * (fc[0] - ec[0]) + nr[1] * (fc[1] - ec[1]) + nr[2] * (fc[2] - ec[2]) < 0) { /* away from the element */
                    int t = nd[1];
                    nd[1] = nd[3], nd[3] = t;
                    for (int k = 0; k < 3; k++) {
                        double x = R[1][k];
                        R[1][k] = R[3][k], R[3][k] = x, nr[k] = -nr[k];
                    }
                }
                int ax = fabs(nr[0]) >= fabs(nr[1]) && fabs(nr[0]) >= fabs(nr[2]) ? 0 : (fabs(nr[1]) >= fabs(nr[2]) ? 1 : 2);
                int d = 2 * ax + (nr[ax] < 0);
                int vi[4];
                for (int a = 0; a < 4; a++) {
                    int n = nd[a];
                    double u[3], val = w0 * va[n] + (s > 0 ? s * vb[n] : 0);
                    for (int k = 0; k < 3; k++) u[k] = w0 * ua[3 * (size_t)n + (size_t)k] + (s > 0 ? s * ub[3 * (size_t)n + (size_t)k] : 0);
                    for (int k = 0; k < 3; k++) P[a][k] = R[a][k] + scale * u[k];
                    size_t key = 6 * (size_t)n + (size_t)d;
                    if (vidx[key] < 0) {
                        if (m.nv == m.capv) {
                            m.capv = m.capv ? 2 * m.capv : 65536;
                            m.v = xrealloc(m.v, (size_t)m.capv * NV * sizeof(float));
                            m.nacc = xrealloc(m.nacc, (size_t)m.capv * 3 * sizeof(double));
                        }
                        float *p = &m.v[(size_t)m.nv * NV];
                        for (int k = 0; k < 3; k++) p[k] = (float)P[a][k], p[8 + k] = (float)R[a][k], p[11 + k] = (float)u[k];
                        p[6] = (float)val, p[7] = (float)map01(val, lo, hi), p[14] = (float)n;
                        m.nacc[3 * (size_t)m.nv] = m.nacc[3 * (size_t)m.nv + 1] = m.nacc[3 * (size_t)m.nv + 2] = 0;
                        vidx[key] = m.nv++;
                        if (ntouched == captouched) captouched = captouched ? 2 * captouched : 65536, touched = xrealloc(touched, (size_t)captouched * sizeof(int));
                        touched[ntouched++] = (int)key;
                        if (g == 0) {
                            umax_part = fmax(umax_part, sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]));
                            vmax_surf = fmax(vmax_surf, val);
                        }
                    }
                    vi[a] = vidx[key];
                }
                newell(P, nd_);
                for (int a = 0; a < 4; a++)
                    for (int k = 0; k < 3; k++) m.nacc[3 * (size_t)vi[a] + (size_t)k] += nd_[k];
                if (m.nt + 2 > m.capt) m.capt = m.capt ? 2 * m.capt : 131072, m.t = xrealloc(m.t, (size_t)m.capt * 3 * sizeof(int));
                double d02 = 0, d13 = 0;
                for (int k = 0; k < 3; k++) d02 += (P[2][k] - P[0][k]) * (P[2][k] - P[0][k]), d13 += (P[3][k] - P[1][k]) * (P[3][k] - P[1][k]);
                int *t = &m.t[3 * (size_t)m.nt];
                if (d02 <= d13) t[0] = vi[0], t[1] = vi[1], t[2] = vi[2], t[3] = vi[0], t[4] = vi[2], t[5] = vi[3];
                else t[0] = vi[1], t[1] = vi[2], t[2] = vi[3], t[3] = vi[1], t[4] = vi[3], t[5] = vi[0];
                m.nt += 2, m.quads++;
            }
            for (int v = 0; v < m.nv; v++) {
                double *n = &m.nacc[3 * (size_t)v], l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                float *p = &m.v[(size_t)v * NV];
                for (int k = 0; k < 3; k++) {
                    p[3 + k] = (float)(l > 0 ? n[k] / l : 0);
                    blo[k] = fmin(blo[k], p[k]), bhi[k] = fmax(bhi[k], p[k]);
                    rlo[k] = fmin(rlo[k], p[8 + k]), rhi[k] = fmax(rhi[k], p[8 + k]);
                }
            }
            for (int q = 0; q < ntouched; q++) vidx[touched[q]] = -1;
            ntouched = 0;
            char gb[16], path[4400], comment[300];
            const char *gname = group_name(g, gb, sizeof gb);
            snprintf(path, sizeof path, "%s/%s.ply", dir, gname);
            snprintf(comment, sizeof comment, "nvt2ply: %s, state %d: stored times %d and %d at s %.6f, displacement x %g, von Mises in Pa", gname, j, S.a,
                     S.b, s, scale);
            if (!write_ply(path, &m, comment)) die("cannot write %s (disk full?)", path);
            JsonValue *p = json_object();
            json_set_string(p, "name", gname);
            char rel[64];
            snprintf(rel, sizeof rel, "state_%04d/%s.ply", j, gname);
            json_set_string(p, "file", rel);
            json_set_string(p, "kind", "surface");
            json_set_int(p, "group", g);
            json_set_int(p, "vertices", m.nv), json_set_int(p, "triangles", m.nt), json_set_int(p, "quads", m.quads);
            json_set_bool(p, "field", true);
            json_push(parts, p);
            fv += m.nv, ft += m.nt;
        }
        json_set_number(fr, "part_surface_max_displacement_m", umax_part);
        json_set_number(fr, "part_surface_max_von_mises_pa", vmax_surf);
        json_push(frames, fr);
        total_v += fv, total_t += ft;
        if (j == 0 || j == ns - 1 || (j + 1) % 10 == 0 || s > 0)
            printf("state %d of %d (stored times %d, %d, s %.3f): %ld vertices, %ld triangles; the part's surface moves up to %.4g mm, "
                   "von Mises up to %.4g MPa\n", j + 1, ns, S.a, S.b, s, fv, ft, 1e3 * umax_part, 1e-6 * vmax_surf);
        fflush(stdout);
    }
    json_set(man, "bounds", json_array());
    JsonValue *bd = json_get(man, "bounds");
    json_push(bd, json_numbers(blo, 3)), json_push(bd, json_numbers(bhi, 3));
    json_set(man, "rest_bounds", json_array());
    bd = json_get(man, "rest_bounds");
    json_push(bd, json_numbers(rlo, 3)), json_push(bd, json_numbers(rhi, 3));
    JsonValue *hd = json_set_object(man, "header");
    json_set_string(hd, "title", "LPBF build, results.nvt");
    JsonValue *fl = json_set_object(hd, "fields");
    json_set_string(fl, "von_mises", "Pa");
    json_set_string(hd, "job_id", json_get_str(r.h, "job_id", ""));
    json_set_string(hd, "kind", json_get_str(r.h, "kind", ""));
    if (json_get(r.h, "summary")) json_set(hd, "summary", json_clone(json_get(r.h, "summary")));
    char mpath[4200];
    snprintf(mpath, sizeof mpath, "%s/manifest.json", out);
    if (!json_write_file(mpath, man, JSON_PRETTY)) die("cannot write %s", mpath);
    printf("nvt2ply: %d states to %s, %ld vertices and %ld triangles in all; bounds %.6g %.6g %.6g to %.6g %.6g %.6g m\n", ns, out, total_v, total_t,
           blo[0], blo[1], blo[2], bhi[0], bhi[1], bhi[2]);
    json_free(man), json_free(r.h);
    fclose(r.f);
    free(X), free(conn), free(birth), free(death), free(group), free(times), free(st), free(alive), free(ua), free(ub), free(va), free(vb);
    free(vidx), free(touched), free(quad_e), free(quad_f), free(quad_g), free(m.v), free(m.t), free(m.nacc);
    free(F.e1), free(F.e2), free(F.f1), free(F.f2);
    return 0;
}
