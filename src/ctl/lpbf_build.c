/* lpbf_build.c - the inherent-strain LPBF build as an engine analysis (see lpbf_build.h) */
#include "lpbf_build.h"

#include "../fem/hex8.h"
#include "../geom/hexmesh.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/errors.h"
#include "../core/paths.h"
#include "static_analysis.h"

static void issue(JsonValue *arr, NvErr code, const char *hint, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void issue(JsonValue *arr, NvErr code, const char *hint, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (arr) json_push(arr, nv_error_json(code, hint, "%s", msg));
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static void *dup_mem(const void *p, size_t n) {
    void *q = malloc(n ? n : 1);
    if (q && p && n) memcpy(q, p, n);
    return q;
}

/* ------------------------------------------------------------------------------------------------ build */

/* Supports, the simplest defensible kind: every voxel column below a part element, down to the plate at z = 0, that
 * is not part becomes support (group 1). The part's own voxel grid is extended downward; where the part stands above
 * the plate by a distance that is not a whole number of elements, the lowest support layer ends exactly at z = 0.
 * Returns the number of support elements added, or -1 when memory runs out. The arrays of the case grow in place. */
/* The flat downward faces of the STL, rasterised onto the voxel columns: for each column, the heights at which a face
 * flatter than the critical angle lies over it. A triangle marks every column whose centre it covers, and the column of
 * its centroid, so a triangle smaller than a column is not lost. */
typedef struct Overhangs {
    int *start;  /* ncolumns + 1 */
    double *z;   /* heights (m) */
    int faces, on_part, steep;
} Overhangs;

static bool overhangs_build(const Body *b, const double lo[3], const double h[3], const int dim[3], double crit, Overhangs *ov) {
    memset(ov, 0, sizeof *ov);
    size_t ncol = (size_t)dim[0] * (size_t)dim[1];
    int *cnt = calloc(ncol + 1, sizeof(int));
    if (!cnt) return false;
    double cmin = cos(crit);
    for (int pass = 0; pass < 2; pass++) {
        for (int t = 0; t < b->surf.nt; t++) {
            const double *n = b->build_normal + 3 * (size_t)t;
            if (!(-n[2] > cmin)) continue; /* not a downward face flatter than the angle */
            const double *A = b->build_v + 3 * (size_t)b->surf.tri[3 * t], *B = b->build_v + 3 * (size_t)b->surf.tri[3 * t + 1],
                         *C = b->build_v + 3 * (size_t)b->surf.tri[3 * t + 2];
            double xmin = fmin(A[0], fmin(B[0], C[0])), xmax = fmax(A[0], fmax(B[0], C[0]));
            double ymin = fmin(A[1], fmin(B[1], C[1])), ymax = fmax(A[1], fmax(B[1], C[1]));
            int i0 = (int)floor((xmin - lo[0]) / h[0]), i1 = (int)floor((xmax - lo[0]) / h[0]);
            int j0 = (int)floor((ymin - lo[1]) / h[1]), j1 = (int)floor((ymax - lo[1]) / h[1]);
            double den = (B[1] - C[1]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[1] - C[1]);
            int ic = (int)floor(((A[0] + B[0] + C[0]) / 3 - lo[0]) / h[0]), jc = (int)floor(((A[1] + B[1] + C[1]) / 3 - lo[1]) / h[1]);
            for (int j = j0; j <= j1; j++)
                for (int i = i0; i <= i1; i++) {
                    if (i < 0 || j < 0 || i >= dim[0] || j >= dim[1]) continue;
                    double px = lo[0] + (i + 0.5) * h[0], py = lo[1] + (j + 0.5) * h[1], z;
                    bool in = false;
                    if (fabs(den) > 1e-30) {
                        double l1 = ((B[1] - C[1]) * (px - C[0]) + (C[0] - B[0]) * (py - C[1])) / den;
                        double l2 = ((C[1] - A[1]) * (px - C[0]) + (A[0] - C[0]) * (py - C[1])) / den;
                        in = l1 >= -1e-9 && l2 >= -1e-9 && 1 - l1 - l2 >= -1e-9;
                        z = l1 * A[2] + l2 * B[2] + (1 - l1 - l2) * C[2];
                    }
                    if (!in && (i != ic || j != jc)) continue;
                    if (!in) z = (A[2] + B[2] + C[2]) / 3;
                    size_t col = (size_t)j * (size_t)dim[0] + (size_t)i;
                    if (pass == 0) cnt[col + 1]++;
                    else ov->z[cnt[col]++] = z;
                }
        }
        if (pass == 0) {
            for (size_t k = 0; k < ncol; k++) cnt[k + 1] += cnt[k];
            ov->start = malloc((ncol + 1) * sizeof(int));
            ov->z = malloc((size_t)(cnt[ncol] ? cnt[ncol] : 1) * sizeof(double));
            if (!ov->start || !ov->z) {
                free(cnt), free(ov->start), free(ov->z);
                return false;
            }
            memcpy(ov->start, cnt, (ncol + 1) * sizeof(int));
        }
    }
    free(cnt);
    return true;
}

static int add_supports(ThermalCase *c, const Body *part_body, double crit, bool on_part_too, double offset, int *dropped,
                        Overhangs *ovout) {
    size_t ne = (size_t)c->nelems, nn = (size_t)c->nnodes;
    double h[3] = {c->h[0], c->h[1], c->h[2]}, lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (size_t n = 0; n < nn; n++)
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], c->xyz[3 * n + (size_t)k]), hi[k] = fmax(hi[k], c->xyz[3 * n + (size_t)k]);
    int dim[3];
    for (int k = 0; k < 3; k++) dim[k] = (int)lround((hi[k] - lo[k]) / h[k]);
    /* levels below the part: nb of them, the lowest one ending at z = 0 */
    int nb = lo[2] > 1e-6 * h[2] ? (int)ceil(lo[2] / h[2] - 1e-6) : 0;
    int nzt = dim[2] + nb; /* levels in the extended grid, level q = k + nb */
    size_t ncell = (size_t)dim[0] * (size_t)dim[1] * (size_t)nzt;
    unsigned char *cell = calloc(ncell ? ncell : 1, 1); /* 1 part, 2 support */
    if (!cell) return -1;
    Overhangs ovs, *ov = NULL;
    if (part_body && crit > 0) {
        /* the columns in the overhang table index the part's own grid (without the levels below it) */
        if (!overhangs_build(part_body, lo, h, dim, crit, &ovs)) {
            free(cell);
            return -1;
        }
        ov = &ovs;
    }
#define CELL(i, j, q) cell[((size_t)(q) * (size_t)dim[1] + (size_t)(j)) * (size_t)dim[0] + (size_t)(i)]
    int pattern[8][3]; /* the corner order of the mesher's hex8, read off element 0, so new elements match it */
    {
        double c0[3] = {INFINITY, INFINITY, INFINITY};
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) c0[k] = fmin(c0[k], c->xyz[3 * (size_t)c->conn[(size_t)a] + (size_t)k]);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++)
                pattern[a][k] = (int)lround((c->xyz[3 * (size_t)c->conn[(size_t)a] + (size_t)k] - c0[k]) / h[k]);
    }
    for (size_t e = 0; e < ne; e++) {
        double m[3] = {INFINITY, INFINITY, INFINITY};
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) m[k] = fmin(m[k], c->xyz[3 * (size_t)c->conn[8 * e + (size_t)a] + (size_t)k]);
        int i = (int)lround((m[0] - lo[0]) / h[0]), j = (int)lround((m[1] - lo[1]) / h[1]), q = (int)lround((m[2] - lo[2]) / h[2]) + nb;
        if (i >= 0 && i < dim[0] && j >= 0 && j < dim[1] && q >= 0 && q < nzt) CELL(i, j, q) = 1;
    }
    int nsup = 0;
    if (!ov) { /* wave 4: every column under a part voxel, down to the plate or the part below */
        for (int j = 0; j < dim[1]; j++)
            for (int i = 0; i < dim[0]; i++)
                for (int q = 1; q < nzt; q++)
                    if (CELL(i, j, q) == 1 && CELL(i, j, q - 1) == 0)
                        for (int r = q - 1; r >= 0 && CELL(i, j, r) == 0; r--) CELL(i, j, r) = 2, nsup++;
    } else {
        /* the overhang rule: a downward voxel face gets support only where a face of the STL flatter than the critical
         * angle lies over it (within one and a half voxels in z), and only when the column below reaches the plate
         * through empty space: a column that would land on the part, which is what the inside of a closed channel
         * does, gets none */
        for (int j = 0; j < dim[1]; j++)
            for (int i = 0; i < dim[0]; i++)
                for (int q = 1; q < nzt; q++) {
                    if (CELL(i, j, q) != 1 || CELL(i, j, q - 1) != 0) continue;
                    double zf = q == nb ? lo[2] : lo[2] + (q - nb) * h[2]; /* the face's height */
                    bool flat = false;
                    for (int t = ov->start[(size_t)j * (size_t)dim[0] + (size_t)i]; t < ov->start[(size_t)j * (size_t)dim[0] + (size_t)i + 1] && !flat; t++)
                        flat = fabs(ov->z[t] - zf) <= 1.5 * h[2];
                    if (!flat) {
                        ov->steep++;
                        continue;
                    }
                    int r = q - 1;
                    while (r >= 0 && CELL(i, j, r) == 0) r--;
                    if (r >= 0) { /* it would stand on the part */
                        ov->on_part++;
                        if (!on_part_too) continue;
                    }
                    /* the offset: a column that would come within it of a part wall at any of its levels is not built */
                    int R = offset > 0 ? (int)ceil(offset / h[0] - 1e-9) : 0;
                    bool near = false;
                    for (int w = q - 1; w > r && R > 0 && !near; w--)
                        for (int dj = -R; dj <= R && !near; dj++)
                            for (int di = -R; di <= R && !near; di++) {
                                int ii = i + di, jj = j + dj;
                                near = ii >= 0 && ii < dim[0] && jj >= 0 && jj < dim[1] && CELL(ii, jj, w) == 1;
                            }
                    if (near) {
                        (*dropped)++;
                        continue;
                    }
                    ov->faces++;
                    for (int w = q - 1; w > r; w--) CELL(i, j, w) = 2, nsup++;
                }
    }
    if (ov) {
        *ovout = *ov;
        free(ov->start), free(ov->z);
        ovout->start = NULL, ovout->z = NULL;
    }
    if (!nsup) {
        free(cell);
        return 0;
    }
    /* nodes: the existing ones keyed by their grid point, new ones appended */
    size_t npt = (size_t)(dim[0] + 1) * (size_t)(dim[1] + 1) * (size_t)(nzt + 1);
    int *node_at = malloc(npt * sizeof(int));
    if (!node_at) {
        free(cell);
        return -1;
    }
    for (size_t i = 0; i < npt; i++) node_at[i] = -1;
#define PT(i, j, q) node_at[((size_t)(q) * (size_t)(dim[1] + 1) + (size_t)(j)) * (size_t)(dim[0] + 1) + (size_t)(i)]
    for (size_t n = 0; n < nn; n++) {
        int i = (int)lround((c->xyz[3 * n] - lo[0]) / h[0]), j = (int)lround((c->xyz[3 * n + 1] - lo[1]) / h[1]);
        int q = (int)lround((c->xyz[3 * n + 2] - lo[2]) / h[2]) + nb;
        if (i >= 0 && i <= dim[0] && j >= 0 && j <= dim[1] && q >= 0 && q <= nzt) PT(i, j, q) = (int)n;
    }
    size_t cap_n = nn + (size_t)nsup * 4 + 64, cap_e = ne + (size_t)nsup;
    double *xyz = realloc(c->xyz, 3 * cap_n * sizeof(double));
    if (xyz) c->xyz = xyz;
    int *conn = realloc(c->conn, 8 * cap_e * sizeof(int));
    if (conn) c->conn = conn;
    int *mat = realloc(c->elem_mat, cap_e * sizeof(int));
    if (mat) c->elem_mat = mat;
    signed char *body = realloc(c->elem_body, cap_e);
    if (body) c->elem_body = body;
    double *src = realloc(c->elem_source, cap_e * sizeof(double));
    if (src) c->elem_source = src;
    int *birth = realloc(c->elem_birth, cap_e * sizeof(int));
    if (birth) c->elem_birth = birth;
    int *death = realloc(c->elem_death, cap_e * sizeof(int));
    if (death) c->elem_death = death;
    unsigned char *group = realloc(c->elem_group, cap_e);
    if (group) c->elem_group = group;
    unsigned char *fixed = realloc(c->fixed, cap_n);
    if (fixed) c->fixed = fixed;
    double *fixed_T = realloc(c->fixed_T, cap_n * sizeof(double));
    if (fixed_T) c->fixed_T = fixed_T;
    if (!xyz || !conn || !mat || !body || !src || !birth || !death || !group || !fixed || !fixed_T) {
        free(cell), free(node_at);
        return -1;
    }
    size_t n_now = nn, e_now = ne;
    for (int q = 0; q < nzt; q++)
        for (int j = 0; j < dim[1]; j++)
            for (int i = 0; i < dim[0]; i++) {
                if (CELL(i, j, q) != 2) continue;
                int corner[8];
                for (int a = 0; a < 8; a++) {
                    int pi = i + pattern[a][0], pj = j + pattern[a][1], pq = q + pattern[a][2];
                    int nd = PT(pi, pj, pq);
                    if (nd < 0) {
                        if (n_now >= cap_n) {
                            cap_n *= 2;
                            double *x2 = realloc(c->xyz, 3 * cap_n * sizeof(double));
                            unsigned char *f2 = realloc(c->fixed, cap_n);
                            double *t2 = realloc(c->fixed_T, cap_n * sizeof(double));
                            if (x2) c->xyz = x2;
                            if (f2) c->fixed = f2;
                            if (t2) c->fixed_T = t2;
                            if (!x2 || !f2 || !t2) {
                                free(cell), free(node_at);
                                return -1;
                            }
                        }
                        nd = (int)n_now++;
                        c->xyz[3 * (size_t)nd] = lo[0] + pi * h[0];
                        c->xyz[3 * (size_t)nd + 1] = lo[1] + pj * h[1];
                        c->xyz[3 * (size_t)nd + 2] = pq == 0 ? 0.0 : lo[2] + (pq - nb) * h[2]; /* the lowest plane is the plate */
                        c->fixed[nd] = 0, c->fixed_T[nd] = 0;
                        PT(pi, pj, pq) = nd;
                    }
                    corner[a] = nd;
                }
                memcpy(c->conn + 8 * e_now, corner, sizeof corner);
                c->elem_mat[e_now] = 0, c->elem_body[e_now] = 0, c->elem_source[e_now] = 0;
                c->elem_birth[e_now] = -1, c->elem_death[e_now] = -1, c->elem_group[e_now] = 1;
                e_now++;
            }
#undef PT
#undef CELL
    free(cell), free(node_at);
    c->nnodes = (int)n_now, c->nelems = (int)e_now, c->nmesh_nodes = (int)n_now;
    return nsup;
}

/* Replace the case's uniform voxel mesh by its adaptive coarsening (docs/contracts/adaptive-mesh.md). Parts and supports
 * coarsen among themselves; the cut band and anything else never does. Every per-element array follows the element's
 * parent voxel, the plate constraint follows the nodes, and the boundary faces are recomputed (a face shared with a
 * coarser neighbour counts as boundary here, which only matters for drawing: it is inside the part). */
static int cmp_face4(const void *a, const void *b) {
    const int *x = a, *y = b;
    for (int k = 0; k < 4; k++)
        if (x[k] != y[k]) return x[k] < y[k] ? -1 : 1;
    return 0;
}

static bool coarsen_case(LpbfCase *lc, JsonValue *errors) {
    ThermalCase *c = &lc->c;
    int max_level = 0;
    while (max_level < 6 && ldexp(c->h[0], max_level + 1) <= lc->s.max_element_size * (1 + 1e-9)) max_level++;
    lc->uniform_elements = c->nelems;
    if (max_level == 0) return true;
    size_t ne = (size_t)c->nelems;
    unsigned char *kind = malloc(ne);
    if (!kind) goto oom;
    for (size_t e = 0; e < ne; e++) kind[e] = c->elem_group[e] <= 1 ? c->elem_group[e] : 255;
    HexCoarsened H;
    char err[256];
    bool ok = hexmesh_coarsen_xy(c->xyz, c->nnodes, c->conn, c->nelems, c->h, kind, max_level,
                                 lc->s.fine_band > 0 ? lc->s.fine_band : 2, &H, err, sizeof err);
    free(kind);
    if (!ok) {
        issue(errors, NV_ERR_INTERNAL, NULL, "the adaptive mesh could not be built: %s", err);
        return false;
    }
    int neo = H.nelems, nno = H.nnodes;
    int *mat = malloc((size_t)neo * sizeof(int)), *birth = malloc((size_t)neo * sizeof(int)), *death = malloc((size_t)neo * sizeof(int));
    signed char *body = malloc((size_t)neo);
    unsigned char *group = malloc((size_t)neo), *fixed = calloc((size_t)nno, 1);
    double *src = calloc((size_t)neo, sizeof(double)), *fixed_T = calloc((size_t)nno, sizeof(double));
    int *faces = malloc((size_t)neo * 6 * 6 * sizeof(int)); /* sorted 4 nodes, element, local face */
    if (!mat || !birth || !death || !body || !group || !fixed || !src || !fixed_T || !faces) {
        free(mat), free(birth), free(death), free(body), free(group), free(fixed), free(src), free(fixed_T), free(faces);
        hexmesh_coarsened_free(&H);
        goto oom;
    }
    for (int e = 0; e < neo; e++) {
        int pe = H.parent[e];
        mat[e] = c->elem_mat[pe], body[e] = c->elem_body[pe], group[e] = c->elem_group[pe];
        birth[e] = death[e] = -1;
        lc->coarse_levels[H.level[e] < 8 ? H.level[e] : 7]++;
    }
    for (int n = 0; n < c->nnodes; n++)
        if (H.node_of_input[n] >= 0) fixed[H.node_of_input[n]] = c->fixed[n];
    for (int i = 0; i < c->nsets; i++) { /* node sets follow the nodes; face sets no longer mean anything */
        int k = 0;
        for (int j = 0; j < c->set[i].nnodes; j++)
            if (H.node_of_input[c->set[i].nodes[j]] >= 0) c->set[i].nodes[k++] = H.node_of_input[c->set[i].nodes[j]];
        c->set[i].nnodes = k, c->set[i].nfaces = 0;
    }
    /* boundary faces: the faces that no other element shares */
    int nf = 0;
    for (int e = 0; e < neo; e++)
        for (int f = 0; f < 6; f++) {
            int *r = faces + 6 * (size_t)nf++;
            for (int a = 0; a < 4; a++) r[a] = H.conn[8 * (size_t)e + HEX8_FACE_NODES[f][a]];
            for (int a = 1; a < 4; a++) /* sort the four */
                for (int b = a; b > 0 && r[b - 1] > r[b]; b--) {
                    int t = r[b];
                    r[b] = r[b - 1], r[b - 1] = t;
                }
            r[4] = e, r[5] = f;
        }
    qsort(faces, (size_t)nf, 6 * sizeof(int), cmp_face4);
    int nb = 0;
    for (int i = 0; i < nf;) {
        int j = i + 1;
        while (j < nf && !cmp_face4(faces + 6 * (size_t)i, faces + 6 * (size_t)j)) j++;
        if (j - i == 1) nb++;
        i = j;
    }
    int *face_elem = malloc((size_t)(nb ? nb : 1) * sizeof(int));
    unsigned char *face_local = malloc((size_t)(nb ? nb : 1));
    if (!face_elem || !face_local) {
        free(face_elem), free(face_local), free(mat), free(birth), free(death), free(body), free(group), free(fixed), free(src),
            free(fixed_T), free(faces);
        hexmesh_coarsened_free(&H);
        goto oom;
    }
    nb = 0;
    for (int i = 0; i < nf;) {
        int j = i + 1;
        while (j < nf && !cmp_face4(faces + 6 * (size_t)i, faces + 6 * (size_t)j)) j++;
        if (j - i == 1) face_elem[nb] = faces[6 * (size_t)i + 4], face_local[nb] = (unsigned char)faces[6 * (size_t)i + 5], nb++;
        i = j;
    }
    free(faces);
    free(c->xyz), free(c->conn), free(c->elem_mat), free(c->elem_body), free(c->elem_source), free(c->elem_birth),
        free(c->elem_death), free(c->elem_group), free(c->fixed), free(c->fixed_T), free(c->face_elem), free(c->face_local);
    c->xyz = H.xyz, c->conn = H.conn, H.xyz = NULL, H.conn = NULL;
    c->elem_mat = mat, c->elem_body = body, c->elem_source = src, c->elem_birth = birth, c->elem_death = death;
    c->elem_group = group, c->fixed = fixed, c->fixed_T = fixed_T, c->face_elem = face_elem, c->face_local = face_local;
    c->nnodes = c->nmesh_nodes = nno, c->nelems = neo, c->nfaces = nb;
    c->nhang = H.nhang;
    c->hang_node = H.hang_node, c->hang_nmaster = H.hang_nmaster, c->hang_master = H.hang_master;
    c->hang_owner = H.hang_owner, c->hang_weight = H.hang_weight;
    H.hang_node = H.hang_nmaster = H.hang_master = H.hang_owner = NULL, H.hang_weight = NULL;
    hexmesh_coarsened_free(&H);
    lc->part_elements = lc->support_elements = 0;
    for (int e = 0; e < neo; e++)
        lc->part_elements += c->elem_group[e] != 1 && c->elem_group[e] != 5, lc->support_elements += c->elem_group[e] == 1 || c->elem_group[e] == 5;
    return true;
oom:
    issue(errors, NV_ERR_INTERNAL, NULL, "out of memory building the adaptive mesh");
    return false;
}

/* The homogenised properties of every support element, from its depth below the part it carries
 * (docs/contracts/supports.md section 2). Elements in the same depth band (and, for cones, under the same support
 * height) share one unit-cell solve. Fills the elements' stiffness scale, the support volume and contact area, and the
 * bands for the result. Runs on the final mesh (after coarsening, which never merges supports). */
static bool support_properties(ThermalCase *c, const LpbfSettings *s, LpbfCase *lc, JsonValue *errors, JsonValue *warnings) {
    const SupportSpec *sp = &s->support;
    size_t ne = (size_t)c->nelems, nn = (size_t)c->nnodes;
    double h[3] = {c->h[0], c->h[1], c->h[2]}, lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (size_t n = 0; n < nn; n++)
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], c->xyz[3 * n + (size_t)k]), hi[k] = fmax(hi[k], c->xyz[3 * n + (size_t)k]);
    int dim[3];
    for (int k = 0; k < 3; k++) dim[k] = (int)lround((hi[k] - lo[k]) / h[k]);
    size_t ncell = (size_t)dim[0] * (size_t)dim[1] * (size_t)dim[2];
    unsigned char *part = calloc(ncell ? ncell : 1, 1);
    int *band_of = malloc(ne * sizeof(int));
    enum { MAXB = 8192 };
    struct Band { double d0, d1, height; int n; SupportCellProps p; } *band = calloc(MAXB, sizeof *band);
    c->elem_stiff_scale = malloc(ne * sizeof(double));
    if (!part || !band_of || !band || !c->elem_stiff_scale) {
        free(part), free(band_of), free(band);
        issue(errors, NV_ERR_INTERNAL, NULL, "out of memory for the support properties");
        return false;
    }
#define PCELL(i, j, k) part[((size_t)(k) * (size_t)dim[1] + (size_t)(j)) * (size_t)dim[0] + (size_t)(i)]
    double bmin[3], bmax[3];
    for (size_t e = 0; e < ne; e++) {
        c->elem_stiff_scale[e] = 1.0, band_of[e] = -1;
        if (c->elem_group[e] == 1 || c->elem_group[e] == 5) continue;
        for (int k = 0; k < 3; k++) bmin[k] = INFINITY, bmax[k] = -INFINITY;
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) {
                double x = c->xyz[3 * (size_t)c->conn[8 * e + (size_t)a] + (size_t)k];
                bmin[k] = fmin(bmin[k], x), bmax[k] = fmax(bmax[k], x);
            }
        int i0 = (int)lround((bmin[0] - lo[0]) / h[0]), i1 = (int)lround((bmax[0] - lo[0]) / h[0]);
        int j0 = (int)lround((bmin[1] - lo[1]) / h[1]), j1 = (int)lround((bmax[1] - lo[1]) / h[1]);
        int k0 = (int)lround((bmin[2] - lo[2]) / h[2]), k1 = (int)lround((bmax[2] - lo[2]) / h[2]);
        for (int k = k0; k < k1; k++)
            for (int j = j0; j < j1; j++)
                for (int i = i0; i < i1; i++) PCELL(i, j, k) = 1;
    }
    /* the fraction of the supported face the support touches: its section just below the part */
    double contact = 0, P = support_pitch(sp);
    if (sp->type == SUPPORT_HOMOGENEOUS) contact = s->support_fraction;
    else {
        int m = 256, hit = 0;
        for (int b = 0; b < m; b++)
            for (int a = 0; a < m; a++) hit += support_occupied(sp, (a + 0.5) * P / m, (b + 0.5) * P / m, 1e-9 * P);
        contact = hit / (double)(m * m);
    }
    int nb = 0;
    double tolz = 1e-6 * h[2], footprint = h[0] * h[1], faces_area = 0;
    for (size_t e = 0; e < ne; e++) {
        if (c->elem_group[e] != 1 && c->elem_group[e] != 5) continue;
        for (int k = 0; k < 3; k++) bmin[k] = INFINITY, bmax[k] = -INFINITY;
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) {
                double x = c->xyz[3 * (size_t)c->conn[8 * e + (size_t)a] + (size_t)k];
                bmin[k] = fmin(bmin[k], x), bmax[k] = fmax(bmax[k], x);
            }
        int i = (int)floor((0.5 * (bmin[0] + bmax[0]) - lo[0]) / h[0]), j = (int)floor((0.5 * (bmin[1] + bmax[1]) - lo[1]) / h[1]);
        int k = (int)lround((bmax[2] - lo[2]) / h[2]);
        while (k < dim[2] && !PCELL(i, j, k)) k++;
        double zf = k < dim[2] ? lo[2] + k * h[2] : bmax[2]; /* the underside of the part this column carries */
        double d0 = fmax(0, zf - bmax[2]), d1 = zf - bmin[2];
        bool bulk = sp->type == SUPPORT_HOMOGENEOUS || sp->type == SUPPORT_LATTICE ||
                    (support_prismatic(sp) && d0 >= sp->tooth_height - tolz);
        double height = sp->type == SUPPORT_CONE ? zf : 0;
        double key0 = bulk ? -1 : d0, key1 = bulk ? -1 : d1;
        int b = 0;
        while (b < nb && !(fabs(band[b].d0 - key0) < tolz && fabs(band[b].d1 - key1) < tolz && fabs(band[b].height - height) < tolz)) b++;
        if (b == nb) {
            if (nb == MAXB) {
                free(part), free(band_of), free(band);
                issue(errors, NV_ERR_UNSUPPORTED, "use fewer layers or a prismatic support type",
                      "the supports need more than %d distinct unit-cell solves", MAXB);
                return false;
            }
            band[nb].d0 = key0, band[nb].d1 = key1, band[nb].height = height, nb++;
        }
        band[b].n++;
        band_of[e] = b;
        if (d0 < tolz) faces_area += footprint;
    }
#undef PCELL
    char err[256];
    for (int b = 0; b < nb; b++) {
        SupportSpec one = *sp;
        if (sp->type == SUPPORT_CONE) one.height = band[b].height;
        /* a bulk band is solved below the tooth band, where its section no longer changes */
        double below = sp->tooth_height > 0 ? sp->tooth_height : 0;
        if (!support_cell_solve(&one, band[b].d0 < 0 ? below : band[b].d0, band[b].d1 < 0 ? below + h[2] : band[b].d1, s->nu, 0, &band[b].p, err,
                                sizeof err)) {
            free(part), free(band_of), free(band);
            issue(errors, NV_ERR_PRECONDITION, "check the support parameters", "the %s support's unit cell cannot be solved: %s",
                  support_type_name(sp->type), err);
            return false;
        }
    }
    double rho = sp->type == SUPPORT_HOMOGENEOUS ? 1.0 : sp->relative_density, vol = 0;
    int weak = 0;
    /* the heat model's view of every band: conductivity fractions x y z, capacity and stiffness fraction */
    free(c->support_band_props);
    c->support_band_props = malloc(6 * (size_t)nb * sizeof(double));
    if (!c->support_band_props) {
        free(part), free(band_of), free(band);
        issue(errors, NV_ERR_INTERNAL, NULL, "out of memory for the support bands");
        return false;
    }
    c->support_nbands = nb;
    for (int b = 0; b < nb; b++) {
        const SupportCellProps *pr = &band[b].p;
        double *q = c->support_band_props + 6 * (size_t)b;
        q[0] = fmax(pr->cond_x * rho, 1e-6), q[1] = fmax(pr->cond_y * rho, 1e-6), q[2] = fmax(pr->cond_z * rho, 1e-6);
        q[3] = fmax(pr->solid_fraction * rho, 1e-6), q[4] = fmax(pr->stiff_z * rho, 1e-6), q[5] = pr->surface_per_volume;
    }
    /* typed supports carry their full orthotropic matrix, one per band: E x the cell's matrix x the relative density */
    bool aniso = sp->type != SUPPORT_HOMOGENEOUS;
    if (aniso) {
        c->elem_D_of = malloc(ne * sizeof(int)), c->support_D = malloc(36 * (size_t)nb * sizeof(double));
        if (!c->elem_D_of || !c->support_D) {
            free(part), free(band_of), free(band);
            issue(errors, NV_ERR_INTERNAL, NULL, "out of memory for the support matrices");
            return false;
        }
        c->support_nD = nb;
        for (int b = 0; b < nb; b++) {
            double Diso[6][6];
            isotropic_D(s->E, s->nu, Diso);
            /* the pattern's own matrix plus a millionth of the solid's: parallel walls carry nothing across their gaps
             * and nothing in shear across them, and a singular element matrix has no stiffness to condense */
            for (int a = 0; a < 36; a++)
                c->support_D[36 * (size_t)b + (size_t)a] = s->E * band[b].p.C[a] * rho + 1e-6 * Diso[a / 6][a % 6];
        }
        for (size_t e = 0; e < ne; e++) c->elem_D_of[e] = band_of[e];
    }
    free(c->elem_band);
    c->elem_band = malloc(ne * sizeof(int));
    if (c->elem_band) memcpy(c->elem_band, band_of, ne * sizeof(int));
    for (size_t e = 0; e < ne; e++) {
        if (band_of[e] < 0) continue;
        const SupportCellProps *pr = &band[band_of[e]].p;
        double f = pr->stiff_z * rho;
        if (f < 1e-6) f = 1e-6, weak++; /* a slab whose pieces carry nothing vertically keeps a trace, so no node floats */
        c->elem_stiff_scale[e] = f;
        vol += footprint * h[2] * pr->solid_fraction * rho;
    }
    if (weak)
        json_push(warnings, nv_error_json(NV_ERR_PRECONDITION, NULL,
                                          "%d support elements lie in a band whose unit cell carries no vertical load; they keep "
                                          "1e-6 of the solid's stiffness", weak));
    lc->support_volume = vol;
    lc->support_contact_fraction = contact;
    lc->support_contact_area = faces_area * contact;
    lc->support_bands_total = nb;
    /* the most populated bands for the result */
    lc->support_nband = 0;
    for (int pick = 0; pick < 24 && pick < nb; pick++) {
        int best = -1;
        for (int b = 0; b < nb; b++)
            if (band[b].n > 0 && (best < 0 || band[b].n > band[best].n)) best = b;
        if (best < 0) break;
        lc->support_band[pick].d0 = band[best].d0, lc->support_band[pick].d1 = band[best].d1;
        lc->support_band[pick].height = band[best].height, lc->support_band[pick].elements = band[best].n;
        lc->support_band[pick].p = band[best].p;
        lc->support_nband++;
        band[best].n = -band[best].n; /* taken */
    }
    free(part), free(band_of), free(band);
    return true;
}

/* The supports of the LPBF generator applied to another voxel case (the FFF print): generation under the overhangs
 * of body with the support fields of s, then homogenisation. stats receives the counts, volume, contact and bands. */
int lpbf_supports_apply(Project *p, ThermalCase *c, const LpbfSettings *s, LpbfCase *stats, JsonValue *errors, JsonValue *warnings) {
    Overhangs ov = {0};
    const Body *body = s->support_rule != 1 ? project_body(p, s->body[0] ? s->body : NULL) : NULL;
    int nsup = add_supports(c, body, s->support_rule != 1 ? s->support_angle : 0, s->support_rule == 2, s->support_offset,
                            &stats->support_columns_offset, &ov);
    stats->support_faces = ov.faces, stats->support_faces_on_part = ov.on_part, stats->support_faces_steep = ov.steep;
    if (nsup < 0) {
        issue(errors, NV_ERR_INTERNAL, NULL, "out of memory generating the supports");
        return -1;
    }
    stats->support_elements = nsup;
    if (nsup > 0 && !support_properties(c, s, stats, errors, warnings)) return -1;
    return nsup;
}

bool lpbf_case_build(Project *p, const LpbfSettings *s, LpbfCase **out, JsonValue *errors, JsonValue *warnings) {
    *out = NULL;
    char why[256];
    if (!project_mesh_current(p, why, sizeof why)) {
        issue(errors, NV_ERR_PRECONDITION, "run mesh_generate again, then start the build", "the mesh is stale: %s", why);
        return false;
    }
    if (s->strain_provenance == PROV_COUNT) {
        issue(errors, NV_ERR_PRECONDITION, "give inherent_strain.provenance (user, inferred or calibrated)",
              "the inherent strain carries no provenance: it is a calibrated quantity, not a material property");
        return false;
    }
    if (s->material_provenance == PROV_COUNT) {
        issue(errors, NV_ERR_PRECONDITION, "give material.provenance", "the elastic constants carry no provenance");
        return false;
    }
    if (s->has_cut && s->cut_provenance == PROV_COUNT) {
        issue(errors, NV_ERR_PRECONDITION, "give cut.provenance; a cut height and kerf that no source states are assumed",
              "the cut carries no provenance");
        return false;
    }
    if (!(s->layer_thickness > 0)) {
        issue(errors, NV_ERR_INVALID_PARAMS, NULL, "layer_thickness_sim must be positive");
        return false;
    }
    LpbfCase *lc = calloc(1, sizeof *lc);
    if (!lc) {
        issue(errors, NV_ERR_INTERNAL, NULL, "out of memory");
        return false;
    }
    lc->s = *s;
    ThermalCase *c = &lc->c;
    StaticSettings sset = {s->formulation, s->solver, s->pcg_tol, 293.15, 0};
    JsonValue *build_errors = json_array();
    if (!static_model_build(p, &sset, &c->mech, build_errors, warnings)) {
        bool only_supports = json_len(build_errors) > 0; /* the plate holds the part; the builder cannot know that */
        for (size_t i = 0; i < json_len(build_errors); i++)
            only_supports &= !strcmp(json_get_str(json_at(build_errors, i), "code", ""), "INSUFFICIENT_CONSTRAINTS");
        if (!only_supports) {
            for (size_t i = 0; i < json_len(build_errors); i++) json_push(errors, json_clone(json_at(build_errors, i)));
            json_free(build_errors);
            static_model_free(&c->mech);
            free(lc);
            return false;
        }
    }
    json_free(build_errors);
    c->has_mech = true;
    StaticModel *m = &c->mech;
    bool ok = true;
    if (m->nbodies != 1) {
        issue(errors, NV_ERR_UNSUPPORTED, "build one part at a time", "the mesh holds %d bodies; the build model builds one part on the plate", m->nbodies);
        ok = false;
    }
    /* the part must rest on the plate (z = 0) */
    double zmin = INFINITY, zmax = -INFINITY, xmax = -INFINITY, xmin = INFINITY;
    for (int n = 0; n < m->nnodes; n++) {
        zmin = fmin(zmin, m->xyz[3 * n + 2]), zmax = fmax(zmax, m->xyz[3 * n + 2]);
        xmax = fmax(xmax, m->xyz[3 * n]), xmin = fmin(xmin, m->xyz[3 * n]);
    }
    double tol = 1e-6 * fmax(zmax - zmin, 1e-3);
    if (ok && !s->supports && fabs(zmin) > 0.6 * m->h[2] + tol) {
        issue(errors, NV_ERR_PRECONDITION, "place the part on the plate with geometry_place (z_offset 0)",
              "the meshed part starts %.2f mm above the build plate (z = 0): it is not attached to it", 1e3 * zmin);
        ok = false;
    }
    if (!ok) {
        static_model_free(&c->mech);
        free(lc);
        return false;
    }
    size_t nn = (size_t)m->nnodes, ne = (size_t)m->nelems, nf = (size_t)m->nfaces;
    c->nnodes = m->nnodes, c->nelems = m->nelems, c->nfaces = m->nfaces, c->nmesh_nodes = m->nnodes;
    c->no_temperature = true;
    c->xyz = dup_mem(m->xyz, 3 * nn * sizeof(double));
    c->conn = dup_mem(m->conn, 8 * ne * sizeof(int));
    c->elem_mat = dup_mem(m->elem_mat, ne * sizeof(int));
    c->elem_body = dup_mem(m->elem_body, ne);
    c->face_elem = dup_mem(m->face_elem, nf * sizeof(int));
    c->face_local = dup_mem(m->face_local, nf);
    c->fixed = calloc(nn, 1);
    c->fixed_T = calloc(nn, sizeof(double));
    c->elem_source = calloc(ne, sizeof(double));
    c->elem_birth = malloc(ne * sizeof(int));
    c->elem_death = malloc(ne * sizeof(int));
    c->elem_group = calloc(ne, 1);
    if (!c->xyz || !c->conn || !c->elem_mat || !c->elem_body || !c->face_elem || !c->face_local || !c->fixed || !c->fixed_T || !c->elem_source ||
        !c->elem_birth || !c->elem_death || !c->elem_group) {
        issue(errors, NV_ERR_INTERNAL, NULL, "out of memory building the LPBF case");
        thermal_case_free(lc);
        return false;
    }
    for (size_t e = 0; e < ne; e++) c->elem_birth[e] = -1, c->elem_death[e] = -1;
    memcpy(c->h, m->h, sizeof c->h);
    lc->part_elements = (int)ne;
    if (s->supports) {
        if (zmin < -tol) {
            issue(errors, NV_ERR_PRECONDITION, "place the part at or above the plate with geometry_place",
                  "the part reaches %.2f mm below the build plate", -1e3 * zmin);
            thermal_case_free(lc);
            return false;
        }
        if (s->support_height >= 0 && fabs(zmin - s->support_height) > fmax(tol, 1e-3 * c->h[2])) {
            issue(errors, NV_ERR_PRECONDITION, "place the part with geometry_place (z_offset) and run mesh_generate again",
                  "supports.height_above_plate is %.3f mm, and the meshed part stands %.3f mm above the plate",
                  1e3 * s->support_height, 1e3 * zmin);
            thermal_case_free(lc);
            return false;
        }
        Overhangs ov = {0};
        int nsup = 0;
        if (s->support.type == SUPPORT_EXPLICIT) { /* meshed with the part: its elements inside the region */
            const double *R = s->support.region;
            for (size_t e = 0; e < ne; e++) {
                double x[3] = {0, 0, 0};
                for (int a = 0; a < 8; a++)
                    for (int k = 0; k < 3; k++) x[k] += c->xyz[3 * (size_t)c->conn[8 * e + (size_t)a] + (size_t)k] / 8;
                if (x[0] > R[0] && x[0] < R[3] && x[1] > R[1] && x[1] < R[4] && x[2] > R[2] && x[2] < R[5]) c->elem_group[e] = 1, nsup++;
            }
            lc->part_elements = (int)ne - nsup;
        } else {
            const Body *body = s->support_rule != 1 ? project_body(p, s->body[0] ? s->body : NULL) : NULL;
            nsup = add_supports(c, body, s->support_rule != 1 ? s->support_angle : 0, s->support_rule == 2, s->support_offset,
                                &lc->support_columns_offset, &ov);
        }
        lc->support_faces = ov.faces, lc->support_faces_on_part = ov.on_part, lc->support_faces_steep = ov.steep;
        if (nsup < 0) {
            issue(errors, NV_ERR_INTERNAL, NULL, "out of memory generating the supports");
            thermal_case_free(lc);
            return false;
        }
        lc->support_elements = nsup;
        nn = (size_t)c->nnodes, ne = (size_t)c->nelems;
        if (!nsup) json_push(warnings, nv_error_json(NV_ERR_PRECONDITION, NULL,
            "supports were asked for, and the part has no downward-facing surface above empty space: none were generated"));
    }
    /* the plate: z = 0 with supports, the part's own lowest plane without them */
    double zplate = s->supports ? 0.0 : zmin;
    for (size_t n = 0; n < nn; n++) c->fixed[n] = fabs(c->xyz[3 * n + 2] - zplate) < tol;
    c->nbodies = m->nbodies;
    for (int i = 0; i < m->nbodies; i++) snprintf(c->body_name[i], sizeof c->body_name[i], "%s", m->body_name[i]);
    snprintf(c->mesh_hash, sizeof c->mesh_hash, "%s", m->mesh_hash);
    c->nmat = 1;
    snprintf(c->mat_id[0], sizeof c->mat_id[0], "%s", m->nmat > 0 ? m->mat_id[0] : "elastic");
    snprintf(c->mat_status[0], sizeof c->mat_status[0], "%s", m->nmat > 0 ? m->mat_status[0] : "user");
    c->nsets = m->nsets;
    for (int i = 0; i < m->nsets; i++) {
        snprintf(c->set[i].name, sizeof c->set[i].name, "%s", m->set[i].name);
        c->set[i].nnodes = m->set[i].nnodes, c->set[i].nfaces = m->set[i].nfaces;
        c->set[i].nodes = dup_mem(m->set[i].nodes, (size_t)m->set[i].nnodes * sizeof(int));
        c->set[i].faces = dup_mem(m->set[i].faces, (size_t)m->set[i].nfaces * sizeof(int));
    }
    c->settings.mechanical = true;
    c->settings.formulation = s->formulation, c->settings.solver = s->solver, c->settings.pcg_tol = s->pcg_tol;
    c->settings.stepping = THERMAL_STEPPING_FIXED;
    lc->tip_x = xmax;
    /* the strain in the model frame: the mesh always has the part's long axis along x */
    lc->eps_model[0] = s->orientation ? s->eps[1] : s->eps[0];
    lc->eps_model[1] = s->orientation ? s->eps[0] : s->eps[1];
    lc->eps_model[2] = s->eps[2];
    /* the cut: every element whose z extent crosses the kerf band, away from the solid block region (x below the first
     * element column is never cut because the block carries the part; the geometry decides, not this code: the band is
     * horizontal and the block's elements are cut too only if they lie in it) */
    if (s->has_cut) {
        int ncut = 0;
        for (size_t e = 0; e < ne; e++) {
            double z0 = INFINITY, z1 = -INFINITY, xc = 0;
            for (int a = 0; a < 8; a++) {
                double z = c->xyz[3 * (size_t)c->conn[8 * e + (size_t)a] + 2];
                z0 = fmin(z0, z), z1 = fmax(z1, z);
                xc += c->xyz[3 * (size_t)c->conn[8 * e + (size_t)a]] / 8;
            }
            /* the element must overlap the kerf band, not merely touch it: a band whose edges fall exactly on element
             * boundaries (kerf 1 mm on 1 mm elements) otherwise takes the two neighbours as well */
            bool crosses = z1 > s->cut_height - s->cut_kerf / 2 + tol && z0 < s->cut_height + s->cut_kerf / 2 - tol;
            if (crosses && xc > xmin + s->cut_from_x && c->elem_group[e] == 0)
                c->elem_group[e] = 3, ncut++; /* marked here (3 while building), removed by the run, shown as part */
            else if (crosses && xc > xmin + s->cut_from_x && c->elem_group[e] == 1)
                c->elem_group[e] = 5, ncut++; /* a support in the kerf (5): a support while it stands, removed by the cut */
        }
        lc->elements_cut = ncut;
        if (!ncut) {
            issue(errors, NV_ERR_PRECONDITION, "move the cut height into the part, or mesh finer",
                  "the cut at %.2f mm with a kerf of %.2f mm crosses no element (elements are %.2f mm tall)", 1e3 * s->cut_height, 1e3 * s->cut_kerf,
                  1e3 * c->h[2]);
            thermal_case_free(lc);
            return false;
        }
    }
    lc->uniform_elements = c->nelems;
    if (s->max_element_size > c->h[0] * (1 + 1e-9)) {
        if (fabs(c->h[0] - c->h[1]) > 1e-9 * c->h[0]) {
            issue(errors, NV_ERR_PRECONDITION, "mesh with the same element size in x and y",
                  "the adaptive mesh merges square blocks, so it needs square voxels in x and y");
            thermal_case_free(lc);
            return false;
        }
        if (!coarsen_case(lc, errors)) {
            thermal_case_free(lc);
            return false;
        }
    }
    if (s->supports && lc->support_elements > 0 && !support_properties(&lc->c, &lc->s, lc, errors, warnings)) {
        thermal_case_free(lc);
        return false;
    }
    *out = lc;
    return true;
}

/* ------------------------------------------------------------------------------------------------ the run */

static void store_time(LpbfCase *lc, LpbfModel *model, double t, int *cap, float *scratch_unused) {
    (void)scratch_unused;
    ThermalCase *c = &lc->c;
    size_t nn = (size_t)c->nnodes, ne = (size_t)c->nelems;
    int i = c->noutputs;
    if (i >= *cap) {
        int grow = *cap ? 2 * *cap : 32;
        c->times = realloc(c->times, (size_t)grow * sizeof(double));
        c->mech_u = realloc(c->mech_u, 3 * (size_t)grow * nn * sizeof(double));
        c->mech_vm = realloc(c->mech_vm, (size_t)grow * nn * sizeof(double));
        c->mech_peak = realloc(c->mech_peak, (size_t)grow * sizeof(double));
        c->mech_umax = realloc(c->mech_umax, (size_t)grow * sizeof(double));
        *cap = grow;
    }
    const double *u = lpbf_u(model);
    const unsigned char *act = lpbf_active(model);
    double *evm = malloc(ne * sizeof(double));
    double *nvm = calloc(nn, sizeof(double));
    int *cnt = calloc(nn, sizeof(int));
    lpbf_von_mises(model, evm);
    for (size_t e = 0; e < ne; e++) {
        if (!act[e]) continue;
        if (c->elem_birth[e] < 0) c->elem_birth[e] = i;
        for (int a = 0; a < 8; a++) {
            int n = c->conn[8 * e + (size_t)a];
            nvm[n] += evm[e], cnt[n]++;
        }
    }
    for (size_t e = 0; e < ne; e++) /* an element that was active and is not any more died at this stored time */
        if (!act[e] && c->elem_birth[e] >= 0 && c->elem_death[e] < 0) c->elem_death[e] = i;
    double umax = 0, peak = 0;
    for (size_t n = 0; n < nn; n++) {
        c->mech_vm[(size_t)i * nn + n] = cnt[n] ? nvm[n] / cnt[n] : 0.0;
        peak = fmax(peak, c->mech_vm[(size_t)i * nn + n]);
        double mag = sqrt(u[3 * n] * u[3 * n] + u[3 * n + 1] * u[3 * n + 1] + u[3 * n + 2] * u[3 * n + 2]);
        umax = fmax(umax, mag);
        for (int k = 0; k < 3; k++) c->mech_u[3 * ((size_t)i * nn + n) + (size_t)k] = u[3 * n + k];
    }
    c->times[i] = t;
    c->mech_umax[i] = umax, c->mech_peak[i] = peak;
    c->noutputs = i + 1;
    free(evm), free(nvm), free(cnt);
}

/* the protocol's quantity: the largest u_z over the nodes at the free end (x = xmax), in mm */
static double tip_uz(const LpbfCase *lc, const double *u) {
    const ThermalCase *c = &lc->c;
    double tol = 1e-6 * fmax(lc->tip_x, 1e-3), best = -INFINITY;
    for (int n = 0; n < c->nnodes; n++)
        if (fabs(c->xyz[3 * n] - lc->tip_x) < tol) best = fmax(best, u[3 * n + 2]);
    return isfinite(best) ? best : 0;
}

/* Mark (group 4) the part elements that are not face-connected, through the part, to its largest piece. A voxel mesh of
 * a complex, not watertight part can hold small islands that only the supports tie to the rest; once the supports are
 * gone they would float, and the stiffness matrix would be singular. They are removed with the supports and counted.
 * Returns the number of island elements, or -1 when memory runs out. */
static int uf_root(int *p, int x) {
    while (p[x] != x) p[x] = p[p[x]], x = p[x];
    return x;
}

static int mark_islands(ThermalCase *c) {
    size_t ne = (size_t)c->nelems;
    double h[3] = {c->h[0], c->h[1], c->h[2]}, lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int n = 0; n < c->nnodes; n++)
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], c->xyz[3 * (size_t)n + (size_t)k]), hi[k] = fmax(hi[k], c->xyz[3 * (size_t)n + (size_t)k]);
    int dim[3];
    for (int k = 0; k < 3; k++) dim[k] = (int)lround((hi[k] - lo[k]) / h[k]) + 1;
    size_t ncell = (size_t)dim[0] * (size_t)dim[1] * (size_t)dim[2];
    int *at = malloc(ncell * sizeof(int)), *par = malloc(ne * sizeof(int)), *size = calloc(ne, sizeof(int));
    if (!at || !par || !size) {
        free(at), free(par), free(size);
        return -1;
    }
    for (size_t i = 0; i < ncell; i++) at[i] = -1;
    /* every voxel an element covers (an adaptive element covers several), for the part only: supports, the cut and
     * islands already found are not part of it */
    for (size_t e = 0; e < ne; e++) {
        par[e] = (int)e;
        if (c->elem_group[e] != 0) continue;
        double m[3] = {INFINITY, INFINITY, INFINITY}, M[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) {
                double x = c->xyz[3 * (size_t)c->conn[8 * e + (size_t)a] + (size_t)k];
                m[k] = fmin(m[k], x), M[k] = fmax(M[k], x);
            }
        int i0 = (int)lround((m[0] - lo[0]) / h[0]), i1 = (int)lround((M[0] - lo[0]) / h[0]);
        int j0 = (int)lround((m[1] - lo[1]) / h[1]), j1 = (int)lround((M[1] - lo[1]) / h[1]);
        int k0 = (int)lround((m[2] - lo[2]) / h[2]), k1 = (int)lround((M[2] - lo[2]) / h[2]);
        for (int k = k0; k < k1; k++)
            for (int j = j0; j < j1; j++)
                for (int i = i0; i < i1; i++) at[((size_t)k * (size_t)dim[1] + (size_t)j) * (size_t)dim[0] + (size_t)i] = (int)e;
    }
    /* face-neighbouring voxels of two part elements join their pieces */
    for (int k = 0; k < dim[2]; k++)
        for (int j = 0; j < dim[1]; j++)
            for (int i = 0; i < dim[0]; i++) {
                int e = at[((size_t)k * (size_t)dim[1] + (size_t)j) * (size_t)dim[0] + (size_t)i];
                if (e < 0) continue;
                int nb[3] = {i + 1 < dim[0] ? at[((size_t)k * (size_t)dim[1] + (size_t)j) * (size_t)dim[0] + (size_t)i + 1] : -1,
                             j + 1 < dim[1] ? at[((size_t)k * (size_t)dim[1] + (size_t)j + 1) * (size_t)dim[0] + (size_t)i] : -1,
                             k + 1 < dim[2] ? at[((size_t)(k + 1) * (size_t)dim[1] + (size_t)j) * (size_t)dim[0] + (size_t)i] : -1};
                for (int d = 0; d < 3; d++)
                    if (nb[d] >= 0) {
                        int a = uf_root(par, e), b = uf_root(par, nb[d]);
                        if (a != b) par[a] = b;
                    }
            }
    int best = -1;
    for (size_t e = 0; e < ne; e++)
        if (c->elem_group[e] == 0) {
            int r = uf_root(par, (int)e);
            size[r]++;
            if (best < 0 || size[r] > size[best]) best = r;
        }
    int islands = 0;
    for (size_t e = 0; e < ne; e++)
        if (c->elem_group[e] == 0 && uf_root(par, (int)e) != best) c->elem_group[e] = 4, islands++;
    free(at), free(par), free(size);
    return islands;
}

/* the largest total displacement over the nodes of the part (supports and removed elements excluded) */
static double part_umax(const LpbfCase *lc, const LpbfModel *model) {
    const ThermalCase *c = &lc->c;
    const double *u = lpbf_u(model);
    const unsigned char *act = lpbf_active(model);
    double best = 0;
    for (int e = 0; e < c->nelems; e++) {
        if (!act[e] || c->elem_group[e] == 1 || c->elem_group[e] == 4 || c->elem_group[e] == 5) continue;
        for (int a = 0; a < 8; a++) {
            size_t n = (size_t)c->conn[8 * (size_t)e + (size_t)a];
            best = fmax(best, sqrt(u[3 * n] * u[3 * n] + u[3 * n + 1] * u[3 * n + 1] + u[3 * n + 2] * u[3 * n + 2]));
        }
    }
    return best;
}

bool lpbf_build_once(Job *job, LpbfCase *lc, bool store_times, double plo, double phi,
                     char *code, size_t codelen, char *err, size_t errlen) {
    ThermalCase *c = &lc->c;
    c->noutputs = 0; /* a calibration runs the same case again and again */
    size_t ne = (size_t)c->nelems;
    double t0 = now_s();
    unsigned char *plate = calloc(3 * (size_t)c->nnodes, 1);
    int *layer_of = malloc(ne * sizeof(int)), *elems = malloc(ne * sizeof(int));
    if (!plate || !layer_of || !elems) {
        free(plate), free(layer_of), free(elems);
        snprintf(code, codelen, "INTERNAL");
        snprintf(err, errlen, "out of memory");
        return false;
    }
    for (int n = 0; n < c->nnodes; n++)
        if (c->fixed[n])
            for (int k = 0; k < 3; k++) plate[3 * n + k] = 1;
    double zmin = INFINITY;
    for (int n = 0; n < c->nnodes; n++) zmin = fmin(zmin, c->xyz[3 * n + 2]);
    int layers = 0;
    for (size_t e = 0; e < ne; e++) {
        double zc = 0;
        for (int a = 0; a < 8; a++) zc += c->xyz[3 * (size_t)c->conn[8 * e + (size_t)a] + 2] / 8;
        layer_of[e] = lc->s.whole_part ? 0 : (int)floor((zc - zmin) / lc->s.layer_thickness);
        if (layer_of[e] < 0) layer_of[e] = 0;
        if (layer_of[e] + 1 > layers) layers = layer_of[e] + 1;
    }
    lc->layers = layers;
    LpbfMesh mesh = {c->nnodes, c->nelems, c->xyz, c->conn};
    LpbfModel *model = lpbf_new(&mesh, lc->s.E, lc->s.nu, plate, lc->s.formulation, lc->s.solver, lc->s.pcg_tol, err, errlen);
    if (model && lc->s.yield_stress > 0)
        lpbf_set_plasticity(model, lc->s.yield_stress, lc->s.hardening, lc->s.max_newton, lc->s.newton_tol);
    if (model && c->nhang > 0 &&
        !lpbf_set_hanging(model, c->nhang, c->hang_node, c->hang_nmaster, c->hang_master, c->hang_weight, c->hang_owner)) {
        lpbf_free(model);
        model = NULL;
        snprintf(err, errlen, "out of memory for the hanging nodes");
    }
    if (model && lc->support_elements > 0 && c->elem_stiff_scale) /* the homogenised supports: their own fractions */
        lpbf_set_scale(model, c->elem_stiff_scale);
    if (model && lc->support_elements > 0 && c->elem_D_of && !lpbf_set_elem_D(model, c->elem_D_of, c->support_nD, c->support_D)) {
        lpbf_free(model);
        model = NULL;
        snprintf(err, errlen, "out of memory for the support matrices");
    }
    free(plate);
    if (!model) {
        free(layer_of), free(elems);
        snprintf(code, codelen, "INTERNAL");
        return false;
    }
    int cap = 0;
    bool ok = true;
    for (int k = 0; ok && k < layers; k++) {
        if (job_cancel_requested(job)) {
            snprintf(code, codelen, "CANCELLED");
            snprintf(err, errlen, "cancelled after %d of %d layers", k, layers);
            ok = false;
            break;
        }
        int n = 0;
        for (size_t e = 0; e < ne; e++)
            if (layer_of[e] == k) elems[n++] = (int)e;
        if (!n) continue;
        lpbf_activate(model, elems, n);
        ok = lpbf_strain(model, elems, n, lc->eps_model, err, errlen);
        if (!ok) {
            snprintf(code, codelen, "SOLVER_FAILED");
            break;
        }
        if (store_times) store_time(lc, model, k + 1.0, &cap, NULL);
        char stage[64];
        snprintf(stage, sizeof stage, "layer %d of %d", k + 1, layers);
        job_progress(job, plo + (phi - plo) * (k + 1.0) / (layers + 1.0), stage);
    }
    if (ok) {
        lc->tip_before = tip_uz(lc, lpbf_u(model));
        lc->umax_before_cut = part_umax(lc, model);
        lc->peak_vm_before = c->noutputs ? c->mech_peak[c->noutputs - 1] : 0;
    }
    double t_next = layers + 1.0;
    /* A part that stands on supports and is cut off them is freed by the cut: the kerf, the stub below it (an island
     * once the kerf is gone) and the supports leave in one step, and the freed part is held 3-2-1 like a part whose
     * supports were removed. Cutting first would leave it floating for one solve. */
    bool together = ok && lc->s.has_cut && lc->support_elements > 0 && lc->s.remove_supports;
    lc->cut_with_supports = together;
    if (ok && lc->s.has_cut && !together) {
        job_progress(job, phi, "cutting the part off the plate");
        int n = 0;
        for (size_t e = 0; e < ne; e++)
            if (c->elem_group[e] == 3 || c->elem_group[e] == 5) elems[n++] = (int)e;
        ok = lpbf_remove(model, elems, n, err, errlen);
        if (!ok) snprintf(code, codelen, "SOLVER_FAILED");
        if (ok && store_times) store_time(lc, model, t_next, &cap, NULL);
        t_next += 1.0;
    }
    if (ok) {
        lc->tip_after = lc->s.has_cut ? tip_uz(lc, lpbf_u(model)) : lc->tip_before;
        lc->umax_after_cut = lc->s.has_cut ? part_umax(lc, model) : lc->umax_before_cut;
    }
    lc->tip_after_support_removal = lc->tip_after;
    lc->umax_final = lc->umax_after_cut;
    if (ok && lc->support_elements > 0 && lc->s.remove_supports) {
        job_progress(job, phi, "removing the supports");
        int islands = mark_islands(c);
        lc->island_elements = islands > 0 ? islands : 0;
        int n = 0;
        for (size_t e = 0; e < ne; e++)
            if (c->elem_group[e] == 1 || c->elem_group[e] == 4 || (together && (c->elem_group[e] == 3 || c->elem_group[e] == 5))) elems[n++] = (int)e;
        /* the interface tear-off: the forces the leaving elements exert on the part that stays, at the nodes they
         * share with it (docs/contracts/supports.md section 3) */
        {
            size_t nn = (size_t)c->nnodes;
            double *ft = calloc(3 * nn, sizeof(double));
            unsigned char *stay = calloc(nn, 1), *leave = calloc(nn, 1);
            const unsigned char *act = lpbf_active(model);
            if (ft && stay && leave) {
                lpbf_element_forces(model, elems, n, ft);
                for (int q = 0; q < n; q++)
                    for (int a = 0; a < 8; a++) leave[c->conn[8 * (size_t)elems[q] + (size_t)a]] = 1;
                for (size_t e = 0; e < ne; e++)
                    if (act[e] && c->elem_group[e] == 0)
                        for (int a = 0; a < 8; a++) stay[c->conn[8 * e + (size_t)a]] = 1;
                lc->tearoff_max = lc->tearoff_sum = 0;
                for (int k = 0; k < 3; k++) lc->tearoff_resultant[k] = 0;
                for (size_t q = 0; q < nn; q++) {
                    if (!(stay[q] && leave[q])) continue;
                    double m2 = 0;
                    for (int k = 0; k < 3; k++) m2 += ft[3 * q + (size_t)k] * ft[3 * q + (size_t)k], lc->tearoff_resultant[k] += ft[3 * q + (size_t)k];
                    lc->tearoff_max = fmax(lc->tearoff_max, sqrt(m2)), lc->tearoff_sum += sqrt(m2);
                }
            }
            free(ft), free(stay), free(leave);
        }
        /* A part that stood on its supports only is free once they are gone. It is then held statically determinately
         * (3-2-1 on its lowest plane: one node in x y z, one in y z along x, one in z), which removes the rigid-body
         * motion and does not restrain the springback. A part that still touches the plate stays held by it. */
        bool on_plate = false;
        for (size_t e = 0; e < ne && !on_plate; e++) {
            if (c->elem_group[e] != 0) continue;
            for (int a = 0; a < 8 && !on_plate; a++) on_plate = c->fixed[c->conn[8 * e + (size_t)a]];
        }
        if (!on_plate) {
            unsigned char *hold = calloc(3 * (size_t)c->nnodes, 1);
            double zlow = INFINITY, xlo = INFINITY, xhi = -INFINITY;
            for (size_t e = 0; e < ne; e++) {
                if (c->elem_group[e] != 0) continue;
                for (int a = 0; a < 8; a++) zlow = fmin(zlow, c->xyz[3 * (size_t)c->conn[8 * e + (size_t)a] + 2]);
            }
            int n1 = -1, n2 = -1, n3 = -1;
            double tolz = 1e-3 * c->h[2], best3 = -1;
            /* a hanging node of the adaptive mesh carries no equation of its own: it cannot hold the part */
            unsigned char *hanging = calloc((size_t)c->nnodes, 1);
            if (hanging)
                for (int q = 0; q < c->nhang; q++) hanging[c->hang_node[q]] = 1;
            for (size_t e = 0; e < ne; e++) { /* the nodes of the part's lowest plane */
                if (c->elem_group[e] != 0) continue;
                for (int a = 0; a < 8; a++) {
                    int nd = c->conn[8 * e + (size_t)a];
                    const double *x = c->xyz + 3 * (size_t)nd;
                    if (fabs(x[2] - zlow) > tolz || (hanging && hanging[nd])) continue;
                    if (x[0] < xlo) xlo = x[0], n1 = nd;
                    if (x[0] > xhi) xhi = x[0], n2 = nd;
                }
            }
            if (n1 >= 0 && n2 >= 0) /* the third, farthest from the line n1-n2, so the three are well conditioned */
                for (size_t e = 0; e < ne; e++) {
                    if (c->elem_group[e] != 0) continue;
                    for (int a = 0; a < 8; a++) {
                        int nd = c->conn[8 * e + (size_t)a];
                        const double *x = c->xyz + 3 * (size_t)nd, *p1 = c->xyz + 3 * (size_t)n1, *p2 = c->xyz + 3 * (size_t)n2;
                        if (fabs(x[2] - zlow) > tolz || (hanging && hanging[nd])) continue;
                        double dx = p2[0] - p1[0], dy = p2[1] - p1[1], L = hypot(dx, dy);
                        double d = L > 0 ? fabs((x[0] - p1[0]) * dy - (x[1] - p1[1]) * dx) / L : 0;
                        if (d > best3) best3 = d, n3 = nd;
                    }
                }
            free(hanging);
            if (!hold || n1 < 0 || n2 < 0 || n3 < 0 || n1 == n2 || best3 <= 0) {
                free(hold);
                ok = false;
                snprintf(code, codelen, "SOLVER_FAILED");
                snprintf(err, errlen, "the part stood on its supports only, and no three nodes of its lowest plane could "
                                      "hold it statically determinately once they were removed");
            } else {
                hold[3 * n1] = hold[3 * n1 + 1] = hold[3 * n1 + 2] = 1;
                hold[3 * n2 + 1] = hold[3 * n2 + 2] = 1;
                hold[3 * n3 + 2] = 1;
                lpbf_set_fixed(model, hold);
                free(hold);
                lc->held_321 = true;
            }
        }
        if (ok) ok = lpbf_remove(model, elems, n, err, errlen);
        if (!ok) snprintf(code, codelen, "SOLVER_FAILED");
        if (ok && store_times) store_time(lc, model, t_next, &cap, NULL);
        if (ok) lc->tip_after_support_removal = tip_uz(lc, lpbf_u(model)), lc->umax_final = part_umax(lc, model);
        if (ok && together) lc->tip_after = lc->tip_after_support_removal, lc->umax_after_cut = lc->umax_final;
    }
    if (ok) {
        lc->springback = lc->tip_after - lc->tip_before;
        lc->peak_vm_after = c->noutputs ? c->mech_peak[c->noutputs - 1] : 0;
        double r[3];
        lc->plate_reaction = lpbf_plate_reaction(model, r);
        lc->equilibrium_error = lpbf_equilibrium_error(model);
        lc->solves = lpbf_solves(model);
        lc->peak_plastic_strain = lpbf_peak_plastic_strain(model);
        lc->newton_iterations = lpbf_newton_iterations(model);
        lc->last_newton_residual = lpbf_last_newton_residual(model);
        lc->seconds = now_s() - t0;
    }
    lpbf_free(model);
    free(layer_of), free(elems);
    return ok;
}

bool lpbf_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    LpbfCase *lc = data;
    ThermalCase *c = &lc->c;
    if (!lpbf_build_once(job, lc, true, 0.0, 0.96, code, codelen, err, errlen)) return false;
    /* the groups the result file and the application use: 0 part (the cut is part, it dies at the cut), 1 support */
    for (size_t e = 0; e < (size_t)c->nelems; e++)
        if (c->elem_group[e] == 3 || c->elem_group[e] == 4) c->elem_group[e] = 0;
        else if (c->elem_group[e] == 5) c->elem_group[e] = 1; /* a support in the kerf is shown as support */
    json_free(c->summary);
    c->summary = lpbf_summary_json(lc);
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, c->run_dir, "results.nvt");
    job_progress(job, 0.98, "writing the results");
    if (!thermal_results_save(c, path, err, errlen)) {
        snprintf(code, codelen, "IO");
        return false;
    }
    path_join(path, sizeof path, c->run_dir, "summary.json");
    if (!json_write_file(path, c->summary, JSON_PRETTY)) {
        snprintf(code, codelen, "IO");
        snprintf(err, errlen, "cannot write %s", path);
        return false;
    }
    job_set_summary(job, json_clone(c->summary));
    job_progress(job, 1.0, "done");
    return true;
}

JsonValue *lpbf_model_json(const LpbfCase *lc) {
    const ThermalCase *c = &lc->c;
    JsonValue *o = json_object();
    json_set_string(o, "analysis", "lpbf_build");
    json_set_string(o, "mode", lc->s.whole_part ? "whole_part" : "layer_by_layer");
    json_set_int(o, "nodes", c->nnodes);
    json_set_int(o, "elements", c->nelems);
    if (lc->s.max_element_size > 0) {
        json_set_int(o, "uniform_elements", lc->uniform_elements);
        json_set_int(o, "hanging_nodes", c->nhang);
        json_set_int(o, "part_elements", lc->part_elements);
        json_set_int(o, "support_elements", lc->support_elements);
    }
    json_set_string(o, "mesh_hash", c->mesh_hash);
    json_set(o, "element_size_mm", json_vec3(1e3 * c->h[0], 1e3 * c->h[1], 1e3 * c->h[2]));
    json_set_string(o, "body", c->nbodies > 0 ? c->body_name[0] : "");
    json_set_number(o, "layer_thickness_mm", 1e3 * lc->s.layer_thickness);
    json_set_string(o, "build_orientation", lc->s.orientation ? "Y" : "X");
    JsonValue *st = json_set_object(o, "inherent_strain");
    json_set_number(st, "exx", lc->s.eps[0]), json_set_number(st, "eyy", lc->s.eps[1]), json_set_number(st, "ezz", lc->s.eps[2]);
    json_set_string(st, "frame", "machine frame, as given");
    json_set(st, "applied_to_model_axes", json_vec3(lc->eps_model[0], lc->eps_model[1], lc->eps_model[2]));
    json_set_string(st, "source", lc->s.strain_source);
    json_set_string(st, "provenance", lc->s.prov_text[0][0] ? lc->s.prov_text[0] : provenance_name(lc->s.strain_provenance));
    JsonValue *mt = json_set_object(o, "material");
    json_set_number(mt, "youngs_modulus_mpa", lc->s.E / 1e6);
    json_set_number(mt, "poissons_ratio", lc->s.nu);
    json_set_string(mt, "provenance", lc->s.prov_text[1][0] ? lc->s.prov_text[1] : provenance_name(lc->s.material_provenance));
    json_set_string(mt, "note", "a body loaded only by an eigenstrain has a displacement field that does not depend on the modulus; it sets the stresses");
    if (lc->s.has_cut) {
        JsonValue *cut = json_set_object(o, "cut");
        json_set_number(cut, "height_mm", 1e3 * lc->s.cut_height);
        if (lc->cut_with_supports)
            json_set_string(cut, "with_supports", "the cut freed a part standing on supports: the kerf, the stub below it "
                                                  "and the supports were removed in one step, the part then held 3-2-1");
        json_set_number(cut, "kerf_mm", 1e3 * lc->s.cut_kerf);
        json_set_number(cut, "from_x_mm", 1e3 * lc->s.cut_from_x);
        json_set_string(cut, "from_x_measured_from", "the part's own minimum x (its fixed end), not the build frame origin");
        json_set_int(cut, "elements_removed", lc->elements_cut);
        json_set_string(cut, "rule", "every element beyond from_x whose z extent crosses the kerf band is removed; what lies before it keeps the part on the plate");
        json_set_string(cut, "provenance", lc->s.prov_text[2][0] ? lc->s.prov_text[2] : provenance_name(lc->s.cut_provenance));
    }
    return o;
}

/* the supports as generated: type, parameters with their sources, what was served, volume, contact, the homogenised
 * bands; after the run also the removal, the tear-off and the hold */
static void supports_fill(const LpbfCase *lc, JsonValue *sp, bool after_run) {
    json_set_int(sp, "elements", lc->support_elements);
    json_set_int(sp, "part_elements", lc->part_elements);
    const SupportSpec *st = &lc->s.support;
    json_set_string(sp, "type", support_type_name(st->type));
    if (st->type == SUPPORT_HOMOGENEOUS) json_set_number(sp, "stiffness_fraction", lc->s.support_fraction);
    JsonValue *pa = json_set_array(sp, "parameters"), *as = json_set_array(sp, "assumed");
    for (int i = 0; i < lc->s.support_nparam; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "name", lc->s.support_param[i].name);
        json_set_number(o, "value", lc->s.support_param[i].value);
        json_set_string(o, "unit", lc->s.support_param[i].unit);
        json_set_string(o, "provenance", lc->s.support_param[i].prov);
        json_set_string(o, "source", lc->s.support_param[i].source);
        json_push(pa, o);
        if (!strcmp(lc->s.support_param[i].prov, "assumed")) json_push(as, json_string(lc->s.support_param[i].name));
    }
    json_set_number(sp, "volume_mm3", 1e9 * lc->support_volume);
    json_set_number(sp, "contact_area_mm2", 1e6 * lc->support_contact_area);
    json_set_number(sp, "contact_fraction", lc->support_contact_fraction);
    json_set_int(sp, "columns_dropped_for_offset", lc->support_columns_offset);
    JsonValue *hm = json_set_object(sp, "homogenised");
    json_set_int(hm, "unit_cells_solved", lc->support_bands_total);
    json_set_string(hm, "applied", lc->s.support.type == SUPPORT_HOMOGENEOUS
        ? "the given fraction scales each support element's isotropic stiffness and, with plasticity, its yield stress"
        : "each support element takes the full orthotropic stiffness matrix of its band, from the periodic unit cell, times "
          "the relative density; supports stay elastic (no yield). The conduction fractions are for the heat model");
    JsonValue *bl = json_set_array(hm, "bands");
    for (int i = 0; i < lc->support_nband; i++) {
        const SupportCellProps *pr = &lc->support_band[i].p;
        JsonValue *o = json_object();
        if (lc->support_band[i].d0 < 0) json_set_string(o, "depth", "the whole column below the tooth band");
        else {
            double dd[2] = {1e3 * lc->support_band[i].d0, 1e3 * lc->support_band[i].d1};
            json_set(o, "depth_below_part_mm", json_numbers(dd, 2));
        }
        if (lc->support_band[i].height > 0) json_set_number(o, "support_height_mm", 1e3 * lc->support_band[i].height);
        json_set_int(o, "elements", lc->support_band[i].elements);
        json_set_number(o, "solid_fraction", pr->solid_fraction);
        json_set_number(o, "stiffness_vertical", pr->stiff_z);
        json_set_number(o, "stiffness_lateral", pr->stiff_x);
        json_set_number(o, "conduction_vertical", pr->cond_z);
        json_set_number(o, "conduction_lateral_x", pr->cond_x);
        json_set_number(o, "conduction_lateral_y", pr->cond_y);
        json_set_number(o, "surface_per_volume_per_mm", 1e-3 * pr->surface_per_volume);
        json_set_int(o, "voxels_per_pitch", pr->voxels_per_pitch);
        json_push(bl, o);
    }
    if (after_run && lc->s.remove_supports) {
        JsonValue *to = json_set_object(sp, "tear_off");
        json_set_number(to, "largest_nodal_force_n", lc->tearoff_max);
        json_set_number(to, "sum_of_nodal_force_magnitudes_n", lc->tearoff_sum);
        json_set(to, "resultant_n", json_vec3(lc->tearoff_resultant[0], lc->tearoff_resultant[1], lc->tearoff_resultant[2]));
        json_set_string(to, "definition", "the forces the removed elements exerted on the part that stays, at the nodes they "
                                          "share with it, just before removal: the load the part releases when they go. The "
                                          "resultant is zero when the part stood on them alone");
    }
    json_set_string(sp, "source", lc->s.support_source);
    json_set_string(sp, "provenance", lc->s.support_prov_text[0] ? lc->s.support_prov_text
                                                                 : provenance_name(lc->s.support_provenance));
    json_set_bool(sp, "removed", lc->s.remove_supports);
    if (!after_run) goto rule;
    json_set_bool(sp, "held_3_2_1_after_removal", lc->held_321);
    json_set_int(sp, "island_elements_removed_with_them", lc->island_elements);
    if (lc->held_321)
        json_set_string(sp, "reference_after_removal", "the part stood on its supports only, so after their removal "
                        "it is held statically determinately on its lowest plane (3-2-1); displacements after the "
                        "removal are measured in that frame, which does not restrain the springback but does decide "
                        "where the zero of the displacement is");
    json_set_number(sp, "tip_uz_after_support_removal_mm", 1e3 * lc->tip_after_support_removal);
    json_set_number(sp, "max_displacement_after_support_removal_mm", 1e3 * lc->umax_final);
rule:
    if (lc->s.support_rule != 1) {
        json_set_string(sp, "rule", lc->s.support_rule == 2 ? "overhang_all" : "overhang");
        json_set_number(sp, "critical_angle_deg", lc->s.support_angle * 180.0 / M_PI);
        json_set_string(sp, "critical_angle_source", lc->s.support_angle_source);
        json_set_int(sp, "supported_faces", lc->support_faces);
        json_set_int(sp, lc->s.support_rule == 2 ? "faces_standing_on_the_part_supported_too" : "faces_that_would_stand_on_the_part",
                     lc->support_faces_on_part);
        json_set_int(sp, "downward_faces_steeper_than_the_angle", lc->support_faces_steep);
        json_set_string(sp, "model", lc->s.support_rule == 2
            ? "a sensitivity, not the rule: a downward voxel face gets support where a face of the STL flatter than the "
              "critical angle lies over it, down to the plate or to the part below it, channels included. A homogenised "
              "lattice at this fraction of the solid's stiffness and, with plasticity, of its yield stress, activated with "
              "its layer and strained like the part"
            : "a downward voxel face gets support where a face of the STL flatter than the "
              "critical angle lies over it and the column below reaches the plate through empty "
              "space; a column that would stand on the part, as inside a closed channel, gets "
              "none. The support is of the given type, homogenised from its explicitly solved unit "
              "cell (homogenised.bands), activated with its layer and strained like the part");
    } else {
        json_set_string(sp, "rule", "every_column");
        json_set_string(sp, "model", "every voxel column below a part element, down to the plate at z = 0, that is not "
                                     "part (the wave-4 rule); a homogenised lattice at this fraction of the solid's "
                                     "stiffness and, with plasticity, of its yield stress");
    }
}

JsonValue *lpbf_supports_json(const LpbfCase *lc) {
    JsonValue *sp = json_object();
    supports_fill(lc, sp, false);
    return sp;
}

JsonValue *lpbf_summary_json(const LpbfCase *lc) {
    const ThermalCase *c = &lc->c;
    JsonValue *o = json_object();
    json_set_string(o, "analysis", "lpbf_build");
    json_set(o, "model", lpbf_model_json(lc));
    json_set_string(o, "temperature", "not modelled");
    JsonValue *r = json_set_object(o, "results");
    json_set_number(r, "tip_uz_before_cut_mm", 1e3 * lc->tip_before);
    json_set_number(r, "tip_uz_after_cut_mm", 1e3 * lc->tip_after);
    json_set_number(r, "springback_mm", 1e3 * (lc->tip_after - lc->tip_before));
    json_set_string(r, "tip_definition", "max u_z over the nodes at the free end (largest x), +z up");
    json_set_number(r, "free_end_x_mm", 1e3 * lc->tip_x);
    json_set_number(r, "peak_von_mises_before_cut_mpa", lc->peak_vm_before / 1e6);
    json_set_number(r, "peak_von_mises_after_cut_mpa", lc->peak_vm_after / 1e6);
    json_set_string(r, "stress_field_definition", "element mean of eight Gauss-point von Mises values, not a Gauss-point maximum");
    json_set_int(r, "layers", lc->layers);
    json_set_int(r, "stored_times", c->noutputs);
    json_set_int(r, "solves", lc->solves);
    json_set_number(r, "seconds", lc->seconds);
    json_set_number(r, "equilibrium_error_last_solve", lc->equilibrium_error);
    if (lc->s.max_element_size > 0) {
        JsonValue *am = json_set_object(r, "adaptive_mesh");
        json_set_number(am, "surface_element_size_mm", 1e3 * lc->c.h[0]);
        json_set_number(am, "max_element_size_mm", 1e3 * lc->s.max_element_size);
        json_set_int(am, "elements", lc->c.nelems);
        json_set_int(am, "uniform_elements", lc->uniform_elements);
        json_set_int(am, "nodes", lc->c.nnodes);
        json_set_int(am, "hanging_nodes", lc->c.nhang);
        json_set_int(am, "fine_band_voxels", lc->s.fine_band > 0 ? lc->s.fine_band : 2);
        JsonValue *lv = json_set_array(am, "elements_per_level");
        for (int L = 0; L < 8; L++)
            if (lc->coarse_levels[L]) {
                JsonValue *o = json_object();
                json_set_number(o, "size_mm", 1e3 * ldexp(lc->c.h[0], L));
                json_set_int(o, "elements", lc->coarse_levels[L]);
                json_push(lv, o);
            }
        json_set_string(am, "method", "2:1 balanced coarsening in x and y only (an element stays one layer tall), fine at "
                                      "the surface, hanging-node constraints eliminated in assembly");
    }
    json_set_number(r, "max_displacement_before_cut_mm", 1e3 * lc->umax_before_cut);
    json_set_number(r, "max_displacement_after_cut_mm", 1e3 * lc->umax_after_cut);
    json_set_string(r, "max_displacement_definition", "largest total displacement over the nodes of the part "
                                                      "(supports excluded), from the plate's reference");
    if (lc->support_elements > 0) supports_fill(lc, json_set_object(r, "supports"), true);
    if (lc->s.yield_stress > 0) {
        JsonValue *pl = json_set_object(r, "plasticity");
        json_set_number(pl, "yield_strength_mpa", lc->s.yield_stress / 1e6);
        json_set_number(pl, "hardening_modulus_mpa", lc->s.hardening / 1e6);
        json_set_string(pl, "model", "J2 (von Mises), isotropic linear hardening, radial return at the Gauss points");
        json_set_number(pl, "peak_equivalent_plastic_strain", lc->peak_plastic_strain);
        json_set_bool(pl, "yielded", lc->peak_plastic_strain > 0);
        json_set_int(pl, "newton_iterations", lc->newton_iterations);
        json_set_number(pl, "last_newton_residual_n", lc->last_newton_residual);
        json_set_string(pl, "source", lc->s.plastic_source);
        json_set_string(pl, "provenance", lc->s.plastic_prov_text[0] ? lc->s.plastic_prov_text
                                                                     : provenance_name(lc->s.plastic_provenance));
        json_set_string(pl, "scope", "rate independent, isothermal, small strain, no kinematic hardening and so no "
                                     "Bauschinger effect, no damage");
    }
    json_set_number(r, "largest_plate_reaction_n", lc->plate_reaction);
    JsonValue *sc = json_set_object(o, "scope");
    json_set_string(sc, "statement",
                    "inherent-strain process simulation: the strain is a calibrated input, not a material property, and the result is only as good as "
                    "that calibration and the geometry it was calibrated on");
    json_set_bool(sc, "is_forecast", false);
    json_set_string(sc, "method", "layer-by-layer activation, each layer stress-free on the deformed part, then its eigenstrain, then equilibrium");
    json_set_bool(sc, "plasticity", false);
    json_set_bool(sc, "temperature_field", false);
    json_set_bool(sc, "supports_modelled", false);
    json_set_bool(sc, "compared_with_measurement", false);
    JsonValue *nm = json_set_array(sc, "not_modelled");
    static const char *const NOT[] = {"the thermal history and the melt pool", "plasticity and annealing", "powder conduction",
                                      "support structures other than the geometry itself", "contact after the cut", "residual stress from the cutting process"};
    for (size_t i = 0; i < sizeof NOT / sizeof NOT[0]; i++) json_push(nm, json_string(NOT[i]));
    json_set_string(sc, "use", "compare strategies, orientations and geometries; a number becomes a claim only against a measurement "
                               "under a protocol written before the run");
    return o;
}
