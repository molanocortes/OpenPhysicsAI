/* solid.c - static linear elasticity: constraint analysis, coloured parallel assembly, solve, recovery */
#include "solid.h"
#include "dense.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- face-connected regions -------------------------------------------------------------------- */

typedef struct {
    int n[4]; /* sorted node ids */
    int elem;
} FaceKey;

static int cmp_face(const void *a, const void *b) {
    const FaceKey *x = a, *y = b;
    for (int k = 0; k < 4; k++)
        if (x->n[k] != y->n[k]) return x->n[k] < y->n[k] ? -1 : 1;
    return x->elem - y->elem;
}

static int uf_find(int *p, int x) {
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

/* region id per element (face adjacency); returns the number of regions or -1 on OOM */
static int face_regions(const HexModel *m, int *region) {
    int ne = m->nelems;
    FaceKey *f = malloc((size_t)(ne ? ne : 1) * 6 * sizeof(FaceKey));
    int *parent = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    if (!f || !parent) {
        free(f), free(parent);
        return -1;
    }
    for (int e = 0; e < ne; e++) {
        parent[e] = e;
        for (int k = 0; k < 6; k++) {
            FaceKey *fk = &f[6 * (size_t)e + k];
            for (int q = 0; q < 4; q++) fk->n[q] = m->conn[8 * (size_t)e + HEX8_FACE_NODES[k][q]];
            for (int i = 1; i < 4; i++)
                for (int j = i; j > 0 && fk->n[j] < fk->n[j - 1]; j--) {
                    int t = fk->n[j];
                    fk->n[j] = fk->n[j - 1], fk->n[j - 1] = t;
                }
            fk->elem = e;
        }
    }
    qsort(f, 6 * (size_t)ne, sizeof(FaceKey), cmp_face);
    for (size_t i = 1; i < 6 * (size_t)ne; i++)
        if (!memcmp(f[i].n, f[i - 1].n, sizeof f[i].n)) {
            int a = uf_find(parent, f[i].elem), b = uf_find(parent, f[i - 1].elem);
            if (a != b) parent[a < b ? b : a] = a < b ? a : b;
        }
    free(f);
    int nr = 0;
    int *label = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    if (!label) {
        free(parent);
        return -1;
    }
    for (int e = 0; e < ne; e++) label[e] = -1;
    for (int e = 0; e < ne; e++) {
        int r = uf_find(parent, e);
        if (label[r] < 0) label[r] = nr++;
        region[e] = label[r];
    }
    free(label);
    free(parent);
    return nr;
}

bool solid_check_constraints(const HexModel *m, const unsigned char *fixed, ConstraintReport *rep) {
    if (m->elem_type != SOLID_ELEM_HEX8) return tet_check_constraints(m, fixed, rep);
    memset(rep, 0, sizeof *rep);
    int ne = m->nelems, nn = m->nnodes;
    int *region = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    int nr = region ? face_regions(m, region) : -1;
    if (nr < 0) {
        free(region);
        rep->nissues = -1;
        return false;
    }
    rep->regions = nr;
    /* node -> region (first seen), and nodes touched by more than one region */
    int *node_region = malloc((size_t)(nn ? nn : 1) * sizeof(int));
    unsigned char *multi = calloc((size_t)(nn ? nn : 1), 1);
    double *G = calloc((size_t)(nr ? nr : 1) * 36, sizeof(double));
    double *cen = calloc((size_t)(nr ? nr : 1) * 4, sizeof(double)); /* x y z count */
    double *lo = malloc((size_t)(nr ? nr : 1) * 3 * sizeof(double)), *hi = malloc((size_t)(nr ? nr : 1) * 3 * sizeof(double));
    int *relems = calloc((size_t)(nr ? nr : 1), sizeof(int)), *weak = calloc((size_t)(nr ? nr : 1), sizeof(int));
    bool ok = node_region && multi && G && cen && lo && hi && relems && weak;
    if (!ok) {
        rep->nissues = -1;
        goto done;
    }
    for (int i = 0; i < nn; i++) node_region[i] = -1;
    for (int r = 0; r < nr; r++)
        for (int k = 0; k < 3; k++) lo[3 * r + k] = INFINITY, hi[3 * r + k] = -INFINITY;
    for (int e = 0; e < ne; e++) {
        int r = region[e];
        relems[r]++;
        for (int a = 0; a < 8; a++) {
            int nd = m->conn[8 * (size_t)e + a];
            if (node_region[nd] < 0) node_region[nd] = r;
            else if (node_region[nd] != r) multi[nd] = 1;
        }
    }
    /* centroid and extent per region from its nodes (a shared node counts for its first region only) */
    for (int nd = 0; nd < nn; nd++) {
        int r = node_region[nd];
        if (r < 0) continue;
        for (int k = 0; k < 3; k++) {
            double x = m->xyz[3 * (size_t)nd + k];
            cen[4 * r + k] += x;
            lo[3 * r + k] = fmin(lo[3 * r + k], x);
            hi[3 * r + k] = fmax(hi[3 * r + k], x);
        }
        cen[4 * r + 3] += 1;
    }
    for (int r = 0; r < nr; r++)
        for (int k = 0; k < 3; k++) cen[4 * r + k] /= cen[4 * r + 3] > 0 ? cen[4 * r + 3] : 1;
    /* shared nodes: count them as weak links for every region that uses them */
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 8; a++) {
            int nd = m->conn[8 * (size_t)e + a];
            if (multi[nd]) weak[region[e]]++;
        }
    /* Gram matrix of the rigid-body modes restricted to the prescribed components of each region */
    for (int e = 0; e < ne; e++) {
        int r = region[e];
        double L = 0;
        for (int k = 0; k < 3; k++) L = fmax(L, hi[3 * r + k] - lo[3 * r + k]);
        if (!(L > 0)) L = 1;
        for (int a = 0; a < 8; a++) {
            int nd = m->conn[8 * (size_t)e + a];
            if (node_region[nd] != r && !multi[nd]) continue;
            double d[3];
            for (int k = 0; k < 3; k++) d[k] = (m->xyz[3 * (size_t)nd + k] - cen[4 * r + k]) / L;
            for (int c = 0; c < 3; c++) {
                if (!fixed[3 * (size_t)nd + c]) continue;
                /* row: displacement component c of the 6 unit modes: tx ty tz, rot about x, y, z (e_k x d) */
                double row[6] = {c == 0, c == 1, c == 2, 0, 0, 0};
                double rx[3] = {0, -d[2], d[1]}, ry[3] = {d[2], 0, -d[0]}, rz[3] = {-d[1], d[0], 0};
                row[3] = rx[c], row[4] = ry[c], row[5] = rz[c];
                for (int i = 0; i < 6; i++)
                    for (int j = 0; j < 6; j++) G[36 * (size_t)r + i * 6 + j] += row[i] * row[j] / 8.0; /* a node counts once per element */
            }
        }
    }
    for (int r = 0; r < nr; r++) {
        double A[36], w[6], V[36];
        memcpy(A, G + 36 * (size_t)r, sizeof A);
        dense_sym_eigen(A, 6, w, V);
        double scale = w[5] > 1 ? w[5] : 1;
        int nfree = 0;
        double modes[6][6];
        for (int i = 0; i < 6; i++)
            if (w[i] <= 1e-12 * scale) {
                for (int k = 0; k < 6; k++) modes[nfree][k] = V[k * 6 + i];
                nfree++;
            }
        if (nfree == 0) continue;
        ok = false;
        if (rep->nissues < SOLID_MAX_ISSUES) {
            ConstraintIssue *is = &rep->issue[rep->nissues++];
            is->region = r;
            is->elements = relems[r];
            for (int k = 0; k < 3; k++) is->centroid[k] = cen[4 * r + k];
            is->free_modes = nfree;
            memcpy(is->modes, modes, sizeof modes);
            is->weak_links = weak[r];
        }
    }
done:
    free(region), free(node_region), free(multi), free(G), free(cen), free(lo), free(hi), free(relems), free(weak);
    return ok;
}

void constraint_issue_text(const ConstraintIssue *is, char *out, size_t cap) {
    size_t used = 0;
    int n = snprintf(out, cap, "region %d (%d elements, centroid %.4g %.4g %.4g m) can move freely: ", is->region, is->elements,
                     is->centroid[0], is->centroid[1], is->centroid[2]);
    if (n > 0) used = (size_t)n;
    if (is->free_modes == 6) {
        n = snprintf(out + used, cap - used, "no displacement is prescribed on it at all");
        if (n > 0) used += (size_t)n;
    } else {
        for (int i = 0; i < is->free_modes && used < cap; i++) {
            const double *v = is->modes[i];
            double t = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), rr = sqrt(v[3] * v[3] + v[4] * v[4] + v[5] * v[5]);
            if (t >= rr)
                n = snprintf(out + used, cap - used, "%stranslation along (%.2f, %.2f, %.2f)", i ? "; " : "", v[0] / t, v[1] / t, v[2] / t);
            else
                n = snprintf(out + used, cap - used, "%srotation about axis (%.2f, %.2f, %.2f) through the centroid", i ? "; " : "", v[3] / rr, v[4] / rr,
                             v[5] / rr);
            if (n > 0) used += (size_t)n;
        }
    }
    if (is->weak_links && used < cap)
        snprintf(out + used, cap - used, "; it touches other regions only at %d edge/vertex nodes, which transmit no moment in this mesh", is->weak_links);
}

/* ---- assembly ---------------------------------------------------------------------------------- */

enum { KE_CACHE_MAX = 4096 };

/* ---- hanging nodes (adaptive meshes) ------------------------------------------------------------------------- */

enum { HANG_MAX_RESOLVED = 16, HANG_MAX_DEPTH = 8 };

/* Every hanging node resolved to non-hanging nodes with weights: a mid-face node of a coarse face whose corner is itself
 * a mid-edge node of a coarser element ends as five or six weighted masters. Built once per solve. */
typedef struct HangTable {
    int n;          /* hanging nodes */
    int *hang_of;   /* nnodes: row of the table, or -1 */
    int *rstart;    /* n + 1 */
    int *rnode;     /* resolved masters */
    double *rw;     /* their weights */
    int maxlen;     /* longest resolved list */
} HangTable;

static void hang_free(HangTable *h) {
    free(h->hang_of), free(h->rstart), free(h->rnode), free(h->rw);
    memset(h, 0, sizeof *h);
}

/* resolve one hanging node into non-hanging masters, depth first; false on a cycle or an over-long chain */
static bool hang_resolve(const HexModel *m, const int *row_of, int i, double w, int depth, int *node, double *wt, int *cnt) {
    if (depth > HANG_MAX_DEPTH) return false;
    for (int k = 0; k < m->hang_nmaster[i]; k++) {
        int mn = m->hang_master[4 * i + k];
        double mw = w * m->hang_weight[4 * i + k];
        if (row_of[mn] >= 0) {
            if (!hang_resolve(m, row_of, row_of[mn], mw, depth + 1, node, wt, cnt)) return false;
            continue;
        }
        int j = 0;
        while (j < *cnt && node[j] != mn) j++;
        if (j == *cnt) {
            if (*cnt >= HANG_MAX_RESOLVED) return false;
            node[j] = mn, wt[j] = 0, (*cnt)++;
        }
        wt[j] += mw;
    }
    return true;
}

static bool hang_build(const HexModel *m, int nn, HangTable *h, char *err, size_t errlen) {
    memset(h, 0, sizeof *h);
    if (m->nhang <= 0) return true;
    int *row_of = malloc((size_t)nn * sizeof(int));
    h->hang_of = malloc((size_t)nn * sizeof(int));
    h->rstart = calloc((size_t)m->nhang + 1, sizeof(int));
    h->rnode = malloc((size_t)m->nhang * HANG_MAX_RESOLVED * sizeof(int));
    h->rw = malloc((size_t)m->nhang * HANG_MAX_RESOLVED * sizeof(double));
    if (!row_of || !h->hang_of || !h->rstart || !h->rnode || !h->rw) {
        free(row_of), hang_free(h);
        snprintf(err, errlen, "out of memory for %d hanging nodes", m->nhang);
        return false;
    }
    for (int i = 0; i < nn; i++) row_of[i] = h->hang_of[i] = -1;
    for (int i = 0; i < m->nhang; i++) {
        int nd = m->hang_node[i];
        if (nd < 0 || nd >= nn || (m->hang_nmaster[i] != 2 && m->hang_nmaster[i] != 4)) {
            free(row_of), hang_free(h);
            snprintf(err, errlen, "hanging node %d is not a mid-edge or mid-face node of the mesh", nd);
            return false;
        }
        row_of[nd] = i;
    }
    h->n = m->nhang;
    for (int i = 0; i < m->nhang; i++) {
        int cnt = 0;
        int *node = h->rnode + h->rstart[i];
        double *wt = h->rw + h->rstart[i];
        if (!hang_resolve(m, row_of, i, 1.0, 0, node, wt, &cnt)) {
            free(row_of), hang_free(h);
            snprintf(err, errlen, "hanging node %d does not resolve to free masters (a cycle, or a chain deeper than %d)",
                     m->hang_node[i], HANG_MAX_DEPTH);
            return false;
        }
        h->rstart[i + 1] = h->rstart[i] + cnt;
        if (cnt > h->maxlen) h->maxlen = cnt;
        h->hang_of[m->hang_node[i]] = i;
    }
    free(row_of);
    return true;
}

/* the equations and weights one node component contributes to: itself, or its resolved masters */
static int hang_expand(const HangTable *h, const int *eq, int nd, int k, int *q, double *w) {
    int i = h->hang_of ? h->hang_of[nd] : -1;
    if (i < 0) {
        if (eq[3 * nd + k] < 0) return 0;
        q[0] = eq[3 * nd + k], w[0] = 1.0;
        return 1;
    }
    int c = 0;
    for (int r = h->rstart[i]; r < h->rstart[i + 1]; r++)
        if (eq[3 * h->rnode[r] + k] >= 0) q[c] = eq[3 * h->rnode[r] + k], w[c] = h->rw[r], c++;
    return c;
}

typedef struct {
    const HexModel *m;
    const SolidLoads *L;
    const int *eq;
    const int *order;       /* elements sorted by colour */
    const int *ke_index;    /* per element: cached matrix index or -1 */
    const double *ke_cache; /* 576 per entry */
    double (*D)[6][6];      /* per material */
    CsrMatrix *K;
    double *f;              /* per equation */
    double *fext_full;      /* 3*nnodes applied loads (body + initial strain), for the residual check */
    bool failed;
    int bad_elem;
    double min_detJ;
    const HangTable *hang; /* NULL or empty: a conforming mesh */
} AsmCtx;

static void elem_coords(const HexModel *m, int e, double X[8][3]) {
    for (int a = 0; a < 8; a++) {
        int nd = m->conn[8 * (size_t)e + a];
        for (int k = 0; k < 3; k++) X[a][k] = m->xyz[3 * (size_t)nd + k];
    }
}

static int elem_matid(const HexModel *m, int e) { return m->elem_mat ? m->elem_mat[e] : 0; }

static void assemble_range(void *vctx, int begin, int end, int tid) {
    AsmCtx *c = vctx;
    const HexModel *m = c->m;
    double Ke_local[576], Ke_scaled[576];
    for (int idx = begin; idx < end; idx++) {
        int e = c->order[idx], mi = elem_matid(m, e);
        double X[8][3];
        elem_coords(m, e, X);
        const double *Ke;
        if (c->ke_index[e] >= 0) {
            Ke = c->ke_cache + 576 * (size_t)c->ke_index[e];
        } else {
            double mdj;
            if (!hex8_stiffness(X, c->D[mi], m->formulation, Ke_local, &mdj)) {
                c->failed = true;
                c->bad_elem = e;
                continue;
            }
            Ke = Ke_local;
        }
        double sc = m->elem_scale ? m->elem_scale[e] : 1.0;
        if (sc != 1.0) {
            for (int i = 0; i < 576; i++) Ke_scaled[i] = sc * Ke[i];
            Ke = Ke_scaled;
        }
        int dofs[24];
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) dofs[3 * a + k] = c->eq[3 * m->conn[8 * (size_t)e + a] + k];
        bool hanging = false;
        if (c->hang && c->hang->n)
            for (int a = 0; a < 8; a++) hanging |= c->hang->hang_of[m->conn[8 * (size_t)e + a]] >= 0;
        if (hanging) { /* K += T^T Ke T, entry by entry, over the resolved masters */
            int nq[24], q[24][HANG_MAX_RESOLVED];
            double w[24][HANG_MAX_RESOLVED];
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) nq[3 * a + k] = hang_expand(c->hang, c->eq, m->conn[8 * (size_t)e + a], k, q[3 * a + k], w[3 * a + k]);
            for (int i = 0; i < 24; i++)
                for (int ii = 0; ii < nq[i]; ii++)
                    for (int j = 0; j < 24; j++)
                        for (int jj = 0; jj < nq[j]; jj++) {
                            int64_t at = csr_find(c->K, q[i][ii], q[j][jj]);
                            if (at >= 0) c->K->val[at] += w[i][ii] * w[j][jj] * Ke[i * 24 + j];
                        }
            double fl[24] = {0};
            if (c->L->eps0) {
                hex8_initial_strain_load(X, c->D[mi], m->formulation, c->L->eps0 + 6 * (size_t)e, fl);
                if (sc != 1.0)
                    for (int i = 0; i < 24; i++) fl[i] *= sc;
            }
            if (c->L->gravity[0] != 0 || c->L->gravity[1] != 0 || c->L->gravity[2] != 0) {
                double rho = m->mat[mi].density, b[3] = {rho * c->L->gravity[0], rho * c->L->gravity[1], rho * c->L->gravity[2]}, fb[24];
                hex8_body_load(X, b, fb);
                for (int a = 0; a < 8; a++)
                    for (int k = 0; k < 3; k++) c->fext_full[3 * (size_t)m->conn[8 * (size_t)e + a] + k] += fb[3 * a + k];
                for (int i = 0; i < 24; i++) fl[i] += fb[i];
            }
            for (int i = 0; i < 24; i++)
                for (int ii = 0; ii < nq[i]; ii++) c->f[q[i][ii]] += w[i][ii] * fl[i];
            continue;
        }
        csr_add_element(c->K, 24, dofs, Ke);
        double fe[24] = {0};
        /* prescribed displacements move to the right-hand side */
        if (c->L->fixed_value) {
            double ub[24];
            bool any = false;
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) {
                    int nd = m->conn[8 * (size_t)e + a];
                    ub[3 * a + k] = c->L->fixed[3 * (size_t)nd + k] ? c->L->fixed_value[3 * (size_t)nd + k] : 0;
                    any |= ub[3 * a + k] != 0;
                }
            if (any)
                for (int i = 0; i < 24; i++) {
                    if (dofs[i] < 0) continue;
                    double s = 0;
                    for (int j = 0; j < 24; j++) s += Ke[i * 24 + j] * ub[j];
                    fe[i] -= s;
                }
        }
        double fl[24] = {0};
        if (c->L->eps0) {
            hex8_initial_strain_load(X, c->D[mi], m->formulation, c->L->eps0 + 6 * (size_t)e, fl);
            if (sc != 1.0)
                for (int i = 0; i < 24; i++) fl[i] *= sc;
        }
        if (c->L->gravity[0] != 0 || c->L->gravity[1] != 0 || c->L->gravity[2] != 0) {
            double rho = m->mat[mi].density, b[3] = {rho * c->L->gravity[0], rho * c->L->gravity[1], rho * c->L->gravity[2]}, fb[24];
            hex8_body_load(X, b, fb);
            /* body forces are external loads; the initial-strain term is part of the internal force (sigma = D (eps - eps0))
             * and moves to the right-hand side only for the solve */
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) c->fext_full[3 * (size_t)m->conn[8 * (size_t)e + a] + k] += fb[3 * a + k];
            for (int i = 0; i < 24; i++) fl[i] += fb[i];
        }
        for (int i = 0; i < 24; i++)
            if (dofs[i] >= 0) c->f[dofs[i]] += fe[i] + fl[i];
    }
}

/* ---- recovery ---------------------------------------------------------------------------------- */

typedef struct {
    const HexModel *m;
    const SolidLoads *L;
    const int *order;
    const int *ke_index;
    const double *ke_cache;
    double (*D)[6][6];
    SolidResult *res;
    double *fint;   /* 3*nnodes */
    double *nsum;   /* 6*nnodes */
    double *ncount; /* nnodes */
    double *energy; /* per thread */
} RecCtx;

static void recover_range(void *vctx, int begin, int end, int tid) {
    RecCtx *c = vctx;
    const HexModel *m = c->m;
    for (int idx = begin; idx < end; idx++) {
        int e = c->order[idx], mi = elem_matid(m, e);
        double X[8][3], ue[24];
        elem_coords(m, e, X);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) ue[3 * a + k] = c->res->u[3 * m->conn[8 * (size_t)e + a] + k];
        double eps[8][6], sig[8][6];
        const double *e0 = c->L->eps0 ? c->L->eps0 + 6 * (size_t)e : NULL;
        hex8_gauss_strain_stress(X, c->D[mi], m->formulation, ue, e0, eps, sig);
        double sc = m->elem_scale ? m->elem_scale[e] : 1.0;
        if (sc != 1.0)
            for (int g = 0; g < 8; g++)
                for (int k = 0; k < 6; k++) sig[g][k] *= sc;
        memcpy(c->res->gp_strain + 48 * (size_t)e, eps, sizeof eps);
        memcpy(c->res->gp_stress + 48 * (size_t)e, sig, sizeof sig);
        double Ke_local[576];
        const double *Ke = Ke_local;
        if (c->ke_index[e] >= 0) Ke = c->ke_cache + 576 * (size_t)c->ke_index[e];
        else if (!hex8_stiffness(X, c->D[mi], m->formulation, Ke_local, NULL)) continue;
        double fe[24], f0[24] = {0};
        if (e0) hex8_initial_strain_load(X, c->D[mi], m->formulation, e0, f0);
        for (int i = 0; i < 24; i++) {
            double s = 0;
            for (int j = 0; j < 24; j++) s += Ke[i * 24 + j] * ue[j];
            fe[i] = sc * (s - f0[i]);
        }
        /* stored elastic energy: 1/2 integral sigma . (eps - eps0) dV at the Gauss points */
        double detj[8], w = 0;
        hex8_gauss_detj(X, detj);
        for (int g = 0; g < 8; g++) {
            double s = 0;
            for (int k = 0; k < 6; k++) s += sig[g][k] * (eps[g][k] - (e0 ? e0[k] : 0));
            w += 0.5 * s * detj[g];
        }
        c->energy[tid] += w;
        double nodal[48];
        hex8_extrapolate((const double *)sig, 6, nodal);
        for (int a = 0; a < 8; a++) {
            int nd = m->conn[8 * (size_t)e + a];
            for (int k = 0; k < 3; k++) c->fint[3 * (size_t)nd + k] += fe[3 * a + k];
            for (int k = 0; k < 6; k++) c->nsum[6 * (size_t)nd + k] += nodal[6 * a + k];
            c->ncount[nd] += 1;
        }
    }
}

/* ---- driver ------------------------------------------------------------------------------------ */

static bool quantize_key(const double X[8][3], double tol, int64_t *key) {
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) {
            double r = (X[a][k] - X[0][k]) / tol;
            if (!(fabs(r) < 9e15)) return false;
            key[3 * a + k] = llround(r);
        }
    return true;
}

bool solid_solve(const HexModel *m, const SolidLoads *L, const SolidOptions *optp, SolidResult *res, char *err, size_t errlen) {
    if (m->elem_type != SOLID_ELEM_HEX8) return tet_solid_solve(m, L, optp, res, err, errlen);
    memset(res, 0, sizeof *res);
    SolidOptions opt = optp ? *optp : (SolidOptions){0};
    if (opt.pcg_tol <= 0) opt.pcg_tol = 1e-10;
    if (opt.pcg_max_iter <= 0) opt.pcg_max_iter = 20000;
    if (opt.max_factor_nnz <= 0) opt.max_factor_nnz = 60000000;
    int nn = m->nnodes, ne = m->nelems;
    bool ok = false;
    int *eq = malloc((size_t)(nn ? nn : 1) * 3 * sizeof(int));
    unsigned char *used = calloc((size_t)(nn ? nn : 1), 1);
    double (*D)[6][6] = malloc((size_t)(m->nmat ? m->nmat : 1) * sizeof *D);
    int *color = malloc((size_t)(ne ? ne : 1) * sizeof(int)), *order = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    uint64_t *node_colors = calloc((size_t)(nn ? nn : 1), sizeof(uint64_t));
    int *ke_index = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    double *ke_cache = NULL, *f = NULL, *fext_full = NULL, *fint = NULL, *nsum = NULL, *ncount = NULL;
    int64_t (*keys)[24] = NULL;
    int *key_mat = NULL;
    CsrMatrix K = {0};
    HangTable hang = {0};
    int *elem_dofs = NULL, *block_of = NULL, *perm = NULL;
    int64_t *adjp = NULL;
    int *adj = NULL;
    if (!eq || !used || !D || !color || !order || !node_colors || !ke_index) goto oom;
    for (int i = 0; i < m->nmat; i++) {
        if (m->mat_D && m->mat_D[36 * (size_t)i] != 0) { /* a given anisotropic matrix */
            for (int a = 0; a < 36; a++) D[i][a / 6][a % 6] = m->mat_D[36 * (size_t)i + (size_t)a];
            continue;
        }
        if (!(m->mat[i].E > 0) || !(m->mat[i].nu > -1 && m->mat[i].nu < 0.5)) {
            snprintf(err, errlen, "material %d has invalid elastic constants (E = %g Pa, nu = %g)", i, m->mat[i].E, m->mat[i].nu);
            goto fail;
        }
        isotropic_D(m->mat[i].E, m->mat[i].nu, D[i]);
    }
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 8; a++) used[m->conn[8 * (size_t)e + a]] = 1;
    if (m->nhang > 0) {
        if (L->fixed_value) {
            snprintf(err, errlen, "prescribed displacements are not supported together with hanging nodes");
            goto fail;
        }
        if (!hang_build(m, nn, &hang, err, errlen)) goto fail;
        /* a hanging node carries no equation, so a component held on it holds only if its masters hold it too; one that
         * does not would be dropped without a word, and the model could move where it was meant to be held */
        for (int nd = 0; nd < nn; nd++) {
            int r = hang.hang_of[nd];
            if (r < 0 || !used[nd]) continue;
            for (int k = 0; k < 3; k++) {
                if (!L->fixed[3 * (size_t)nd + (size_t)k]) continue;
                for (int q = hang.rstart[r]; q < hang.rstart[r + 1]; q++)
                    if (!L->fixed[3 * (size_t)hang.rnode[q] + (size_t)k]) {
                        snprintf(err, errlen, "node %d is held in component %d but hangs on node %d, which is free in it: hold a "
                                              "node that is not hanging", nd, k, hang.rnode[q]);
                        goto fail;
                    }
            }
        }
    }
    int neq = 0;
    for (int nd = 0; nd < nn; nd++) /* a hanging node carries no equation: its masters do */
        for (int k = 0; k < 3; k++)
            eq[3 * nd + k] = (used[nd] && !L->fixed[3 * (size_t)nd + k] && !(hang.n && hang.hang_of[nd] >= 0)) ? neq++ : -1;
    res->neq = neq;

    /* greedy colouring: elements of one colour share no node, so they assemble in parallel */
    int ncolors = 0;
    for (int e = 0; e < ne; e++) {
        /* an element writes to its own nodes' rows, and through a hanging node to that node's masters' rows */
        int wn[8 * HANG_MAX_RESOLVED], nw = 0;
        for (int a = 0; a < 8; a++) {
            int nd = m->conn[8 * (size_t)e + a], i = hang.n ? hang.hang_of[nd] : -1;
            if (i < 0) wn[nw++] = nd;
            else
                for (int r = hang.rstart[i]; r < hang.rstart[i + 1] && nw < 8 * HANG_MAX_RESOLVED; r++) wn[nw++] = hang.rnode[r];
        }
        uint64_t mask = 0;
        for (int a = 0; a < nw; a++) mask |= node_colors[wn[a]];
        int c = 0;
        while (c < 64 && (mask >> c) & 1) c++;
        if (c == 64) c = 63; /* practically unreachable for hexahedral meshes; colour 63 is then assembled serially */
        color[e] = c;
        if (c + 1 > ncolors) ncolors = c + 1;
        for (int a = 0; a < nw; a++) node_colors[wn[a]] |= (uint64_t)1 << c;
    }
    res->colors = ncolors;
    int *cstart = calloc((size_t)ncolors + 1, sizeof(int));
    if (!cstart) goto oom;
    for (int e = 0; e < ne; e++) cstart[color[e] + 1]++;
    for (int c = 0; c < ncolors; c++) cstart[c + 1] += cstart[c];
    {
        int *fill = malloc((size_t)(ncolors ? ncolors : 1) * sizeof(int));
        if (!fill) {
            free(cstart);
            goto oom;
        }
        memcpy(fill, cstart, (size_t)ncolors * sizeof(int));
        for (int e = 0; e < ne; e++) order[fill[color[e]]++] = e;
        free(fill);
    }

    /* stiffness cache for elements that are translated copies of each other (voxel meshes) */
    ke_cache = malloc(576 * sizeof(double) * 64);
    keys = malloc(sizeof *keys * 64);
    key_mat = malloc(sizeof(int) * 64);
    int ncache = 0, cache_cap = 64;
    double mindet = INFINITY;
    if (!ke_cache || !keys || !key_mat) {
        free(cstart);
        goto oom;
    }
    for (int e = 0; e < ne; e++) {
        ke_index[e] = -1;
        double X[8][3];
        elem_coords(m, e, X);
        double size = 0;
        for (int k = 0; k < 3; k++) size = fmax(size, fabs(X[6][k] - X[0][k]));
        int64_t key[24];
        if (!(size > 0) || !quantize_key(X, size * 1e-9, key)) continue;
        int mi = elem_matid(m, e), found = -1;
        for (int c = ncache - 1; c >= 0 && found < 0; c--)
            if (key_mat[c] == mi && !memcmp(keys[c], key, sizeof key)) found = c;
        if (found >= 0) {
            ke_index[e] = found;
            res->stiffness_cache_hits++;
            continue;
        }
        if (ncache >= KE_CACHE_MAX) continue;
        if (ncache == cache_cap) {
            cache_cap *= 2;
            double *nc = realloc(ke_cache, 576 * sizeof(double) * (size_t)cache_cap);
            int64_t (*nk)[24] = realloc(keys, sizeof *keys * (size_t)cache_cap);
            int *nm = realloc(key_mat, sizeof(int) * (size_t)cache_cap);
            if (nc) ke_cache = nc;
            if (nk) keys = nk;
            if (nm) key_mat = nm;
            if (!nc || !nk || !nm) {
                free(cstart);
                goto oom;
            }
        }
        double mdj;
        if (!hex8_stiffness(X, D[mi], m->formulation, ke_cache + 576 * (size_t)ncache, &mdj)) {
            snprintf(err, errlen, "element %d is inverted or degenerate (Jacobian determinant %.3g)", e, mdj);
            free(cstart);
            goto fail;
        }
        if (mdj < mindet) mindet = mdj;
        memcpy(keys[ncache], key, sizeof key);
        key_mat[ncache] = mi;
        ke_index[e] = ncache++;
    }
    res->min_detJ = mindet;

    int npe = 24 * (hang.maxlen > 1 ? hang.maxlen : 1); /* a hanging component widens to its resolved masters */
    elem_dofs = malloc((size_t)(ne ? ne : 1) * (size_t)npe * sizeof(int));
    if (!elem_dofs) {
        free(cstart);
        goto oom;
    }
    for (int e = 0; e < ne; e++) {
        int *row = elem_dofs + (size_t)npe * (size_t)e, cnt = 0;
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) {
                int q[HANG_MAX_RESOLVED];
                double w[HANG_MAX_RESOLVED];
                int nq = hang.n ? hang_expand(&hang, eq, m->conn[8 * (size_t)e + a], k, q, w)
                                : (eq[3 * m->conn[8 * (size_t)e + a] + k] >= 0 ? (q[0] = eq[3 * m->conn[8 * (size_t)e + a] + k], 1) : 0);
                if (npe == 24) row[3 * a + k] = nq ? q[0] : -1;
                else
                    for (int i = 0; i < nq; i++) row[cnt++] = q[i];
            }
        if (npe != 24)
            while (cnt < npe) row[cnt++] = -1;
    }
    if (!csr_from_elements(&K, neq, ne, npe, elem_dofs, err, errlen)) {
        free(cstart);
        goto fail;
    }
    free(elem_dofs);
    elem_dofs = NULL;
    f = calloc((size_t)(neq ? neq : 1), sizeof(double));
    fext_full = calloc((size_t)(nn ? nn : 1) * 3, sizeof(double));
    if (!f || !fext_full) {
        free(cstart);
        goto oom;
    }
    AsmCtx actx = {m, L, eq, order, ke_index, ke_cache, D, &K, f, fext_full, false, -1, INFINITY, &hang};
    for (int c = 0; c < ncolors; c++) {
        int cnt = cstart[c + 1] - cstart[c];
        AsmCtx sub = actx;
        sub.order = order + cstart[c];
        if (opt.pool && c < 63 && cnt > 256) pool_for(opt.pool, cnt, 128, assemble_range, &sub);
        else assemble_range(&sub, 0, cnt, 0);
        if (sub.failed) {
            snprintf(err, errlen, "element %d is inverted or degenerate", sub.bad_elem);
            free(cstart);
            goto fail;
        }
    }
    if (L->nodal_force)
        for (int nd = 0; nd < nn; nd++)
            for (int k = 0; k < 3; k++) {
                double v = L->nodal_force[3 * (size_t)nd + k];
                if (v == 0) continue;
                fext_full[3 * (size_t)nd + k] += v;
                if (hang.n && hang.hang_of[nd] >= 0) {
                    int q[HANG_MAX_RESOLVED];
                    double w[HANG_MAX_RESOLVED];
                    int nq = hang_expand(&hang, eq, nd, k, q, w);
                    for (int i = 0; i < nq; i++) f[q[i]] += w[i] * v;
                    continue;
                }
                int q = eq[3 * nd + k];
                if (q >= 0) f[q] += v;
            }

    res->u = calloc((size_t)(nn ? nn : 1) * 3, sizeof(double));
    double *x = calloc((size_t)(neq ? neq : 1), sizeof(double));
    if (!res->u || !x) {
        free(x);
        free(cstart);
        goto oom;
    }
    if (neq > 0) {
        bool direct = opt.solver == SOLID_SOLVER_DIRECT;
        /* with hanging nodes the masters couple across coarse elements and the direct factor fills in far more (a 0.5 mm
         * cantilever: 145 s direct against 22 s iterative), so an adaptive mesh goes to the iterative solver */
        if (opt.solver == SOLID_SOLVER_AUTO) direct = neq <= 250000 && m->nhang == 0;
        if (direct) {
            /* nested dissection over nodes needs node adjacency */
            adjp = calloc((size_t)nn + 1, sizeof(int64_t));
            perm = malloc((size_t)(neq ? neq : 1) * sizeof(int));
            if (!adjp || !perm) {
                free(x);
                free(cstart);
                goto oom;
            }
            /* node adjacency from the equation pattern: node of an equation = eq_node */
            int *eq_node = malloc((size_t)(neq ? neq : 1) * sizeof(int));
            if (!eq_node) {
                free(x);
                free(cstart);
                goto oom;
            }
            for (int nd = 0; nd < nn; nd++)
                for (int k = 0; k < 3; k++)
                    if (eq[3 * nd + k] >= 0) eq_node[eq[3 * nd + k]] = nd;
            int *mark = malloc((size_t)(nn ? nn : 1) * sizeof(int));
            if (!mark) {
                free(eq_node), free(x), free(cstart);
                goto oom;
            }
            for (int i = 0; i < nn; i++) mark[i] = -1;
            for (int q = 0; q < neq; q++) {
                int nd = eq_node[q];
                if (q > 0 && eq_node[q - 1] == nd) continue; /* equations of a node are consecutive */
                int64_t cnt = 0;
                for (int64_t k = K.rowptr[q]; k < K.rowptr[q + 1]; k++) {
                    int o = eq_node[K.col[k]];
                    if (o != nd && mark[o] != nd) mark[o] = nd, cnt++;
                }
                adjp[nd + 1] = cnt;
            }
            for (int i = 0; i < nn; i++) adjp[i + 1] += adjp[i];
            adj = malloc((size_t)(adjp[nn] ? adjp[nn] : 1) * sizeof(int));
            if (!adj) {
                free(mark), free(eq_node), free(x), free(cstart);
                goto oom;
            }
            for (int i = 0; i < nn; i++) mark[i] = -1;
            for (int q = 0; q < neq; q++) {
                int nd = eq_node[q];
                if (q > 0 && eq_node[q - 1] == nd) continue;
                int64_t w = adjp[nd];
                for (int64_t k = K.rowptr[q]; k < K.rowptr[q + 1]; k++) {
                    int o = eq_node[K.col[k]];
                    if (o != nd && mark[o] != nd) mark[o] = nd, adj[w++] = o;
                }
            }
            free(mark);
            free(eq_node);
            if (!nested_dissection_order(nn, m->xyz, adjp, adj, eq, neq, perm, err, errlen)) {
                free(x), free(cstart);
                goto fail;
            }
            int64_t fnnz = chol_symbolic_nnz(&K, perm, err, errlen);
            if (fnnz < 0 || fnnz > opt.max_factor_nnz) {
                if (opt.solver == SOLID_SOLVER_DIRECT) {
                    snprintf(err, errlen, "direct solver needs %lld factor entries (limit %lld); use the iterative solver", (long long)fnnz,
                             (long long)opt.max_factor_nnz);
                    free(x), free(cstart);
                    goto fail;
                }
                direct = false;
            }
        }
        if (direct) {
            CholFactor F;
            int bad = -1;
            if (!chol_factor(&K, perm, &F, opt.progress, opt.ctx, &res->stats, &bad, err, errlen)) {
                if (bad >= 0) {
                    for (int nd = 0; nd < nn; nd++)
                        for (int k = 0; k < 3; k++)
                            if (eq[3 * nd + k] == bad) {
                                size_t l = strlen(err);
                                snprintf(err + l, errlen - l, " [node %d, component %c, at %.4g %.4g %.4g m]", nd, "xyz"[k], m->xyz[3 * nd],
                                         m->xyz[3 * nd + 1], m->xyz[3 * nd + 2]);
                            }
                }
                free(x), free(cstart);
                goto fail;
            }
            chol_solve(&F, f, x);
            chol_free(&F);
            res->stats.converged = true;
        } else {
            block_of = malloc((size_t)neq * sizeof(int));
            if (!block_of) {
                free(x), free(cstart);
                goto oom;
            }
            int nb = 0;
            for (int nd = 0; nd < nn; nd++) {
                bool anyq = false;
                for (int k = 0; k < 3; k++)
                    if (eq[3 * nd + k] >= 0) block_of[eq[3 * nd + k]] = nb, anyq = true;
                if (anyq) nb++;
            }
            PcgOptions po = {PRECOND_BLOCK_JACOBI, block_of, opt.pcg_tol, opt.pcg_max_iter, opt.pool, opt.progress, opt.ctx};
            if (!pcg_solve(&K, f, x, &po, &res->stats, err, errlen)) {
                free(x), free(cstart);
                goto fail;
            }
        }
        /* independent residual of the assembled system */
        double *Ax = malloc((size_t)neq * sizeof(double));
        if (Ax) {
            csr_spmv(&K, x, Ax, opt.pool);
            double rr = 0, bb = 0;
            for (int q = 0; q < neq; q++) rr += (f[q] - Ax[q]) * (f[q] - Ax[q]), bb += f[q] * f[q];
            res->stats.true_residual = bb > 0 ? sqrt(rr / bb) : sqrt(rr);
            free(Ax);
        }
    }
    for (int nd = 0; nd < nn; nd++)
        for (int k = 0; k < 3; k++) {
            int q = eq[3 * nd + k];
            if (q >= 0) res->u[3 * nd + k] = x[q];
            else if (L->fixed[3 * (size_t)nd + k] && L->fixed_value) res->u[3 * nd + k] = L->fixed_value[3 * (size_t)nd + k];
        }
    for (int i = 0; i < hang.n; i++) { /* resolved masters are never hanging, so their values are already final */
        int nd = m->hang_node[i];
        for (int k = 0; k < 3; k++) {
            double v = 0;
            for (int r = hang.rstart[i]; r < hang.rstart[i + 1]; r++) v += hang.rw[r] * res->u[3 * hang.rnode[r] + k];
            res->u[3 * nd + k] = v;
        }
    }
    free(x);
    csr_free(&K);

    /* recovery: stresses, internal forces, reactions, energy */
    res->gp_strain = malloc((size_t)(ne ? ne : 1) * 48 * sizeof(double));
    res->gp_stress = malloc((size_t)(ne ? ne : 1) * 48 * sizeof(double));
    res->node_stress = calloc((size_t)(nn ? nn : 1) * 6, sizeof(double));
    res->reaction = calloc((size_t)(nn ? nn : 1) * 3, sizeof(double));
    fint = calloc((size_t)(nn ? nn : 1) * 3, sizeof(double));
    nsum = calloc((size_t)(nn ? nn : 1) * 6, sizeof(double));
    ncount = calloc((size_t)(nn ? nn : 1), sizeof(double));
    int nthreads = opt.pool ? pool_size(opt.pool) : 1;
    double *energy = calloc((size_t)nthreads, sizeof(double));
    if (!res->gp_strain || !res->gp_stress || !res->node_stress || !res->reaction || !fint || !nsum || !ncount || !energy) {
        free(energy), free(cstart);
        goto oom;
    }
    RecCtx rctx = {m, L, order, ke_index, ke_cache, D, res, fint, nsum, ncount, energy};
    for (int c = 0; c < ncolors; c++) {
        int cnt = cstart[c + 1] - cstart[c];
        RecCtx sub = rctx;
        sub.order = order + cstart[c];
        if (opt.pool && c < 63 && cnt > 256) pool_for(opt.pool, cnt, 128, recover_range, &sub);
        else recover_range(&sub, 0, cnt, nthreads - 1);
    }
    free(cstart);
    for (int t = 0; t < nthreads; t++) res->strain_energy += energy[t];
    free(energy);
    for (int i = 0; i < hang.n; i++) { /* the constraint carries a hanging node's force to its masters */
        int nd = m->hang_node[i];
        for (int k = 0; k < 3; k++) {
            for (int r = hang.rstart[i]; r < hang.rstart[i + 1]; r++) {
                fint[3 * (size_t)hang.rnode[r] + k] += hang.rw[r] * fint[3 * (size_t)nd + k];
                fext_full[3 * (size_t)hang.rnode[r] + k] += hang.rw[r] * fext_full[3 * (size_t)nd + k];
            }
            fint[3 * (size_t)nd + k] = fext_full[3 * (size_t)nd + k] = 0;
        }
    }
    double rfree = 0, aload = 0, rsum2 = 0;
    for (int nd = 0; nd < nn; nd++) {
        if (ncount[nd] > 0)
            for (int k = 0; k < 6; k++) res->node_stress[6 * nd + k] = nsum[6 * nd + k] / ncount[nd];
        for (int k = 0; k < 3; k++) {
            double r = fint[3 * nd + k] - fext_full[3 * nd + k];
            res->load_total[k] += fext_full[3 * nd + k];
            aload += fext_full[3 * nd + k] * fext_full[3 * nd + k];
            if (used[nd] && L->fixed[3 * (size_t)nd + k]) {
                res->reaction[3 * nd + k] = r;
                res->reaction_total[k] += r;
                rsum2 += r * r;
                res->external_work += r * res->u[3 * nd + k];
            } else if (used[nd]) {
                rfree += r * r;
            }
            res->external_work += fext_full[3 * nd + k] * res->u[3 * nd + k];
        }
    }
    double fnorm = 0;
    for (int q = 0; q < neq; q++) fnorm += f[q] * f[q];
    /* force scale: applied loads, reactions and the assembled right-hand side (which carries initial-strain loads) */
    double denom = sqrt(aload) + sqrt(rsum2) + sqrt(fnorm);
    res->equilibrium_error = denom > 0 ? sqrt(rfree) / denom : sqrt(rfree);
    ok = true;
    goto done;
oom:
    snprintf(err, errlen, "out of memory in the structural solver (%d nodes, %d elements)", nn, ne);
fail:
    solid_result_free(res);
done:
    free(eq), free(used), free(D), free(color), free(order), free(node_colors), free(ke_index), free(ke_cache), free(keys), free(key_mat);
    free(f), free(fext_full), free(fint), free(nsum), free(ncount), free(elem_dofs), free(block_of), free(perm), free(adjp), free(adj);
    csr_free(&K);
    hang_free(&hang);
    return ok;
}

void solid_result_free(SolidResult *r) {
    free(r->u);
    free(r->reaction);
    free(r->gp_strain);
    free(r->gp_stress);
    free(r->node_stress);
    SolveStats st = r->stats;
    int neq = r->neq;
    memset(r, 0, sizeof *r);
    r->stats = st;
    r->neq = neq;
}
