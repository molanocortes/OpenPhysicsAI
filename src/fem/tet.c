/* tet.c - TET4 and TET10 elements, and the static solve on a tetrahedral model (docs/contracts/tet-mesh.md) */
#include "tet.h"
#include "dense.h"
#include "solid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double now_seconds(void);

const int TET_EDGE[6][2] = {{0, 1}, {1, 2}, {2, 0}, {0, 3}, {1, 3}, {2, 3}};
const int TET_FACE[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
const int TET10_FACE_MID[4][3] = {{5, 9, 8}, {7, 9, 6}, {4, 8, 7}, {6, 5, 4}};

int tet_nodes(int type) { return type == SOLID_ELEM_TET10 ? 10 : 4; }
int tet_gauss_count(int type) { return type == SOLID_ELEM_TET10 ? 4 : 1; }

void tet_gauss_points(int type, double L[][4], double *w) {
    if (type != SOLID_ELEM_TET10) {
        for (int k = 0; k < 4; k++) L[0][k] = 0.25;
        w[0] = 1.0 / 6.0;
        return;
    }
    const double a = 0.5854101966249685, b = 0.1381966011250105;
    for (int g = 0; g < 4; g++) {
        for (int k = 0; k < 4; k++) L[g][k] = k == g ? a : b;
        w[g] = 1.0 / 24.0;
    }
}

static const double DL[4][3] = {{-1, -1, -1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

void tet_shape(int type, const double L[4], double *N, double (*dN)[3]) {
    if (type != SOLID_ELEM_TET10) {
        for (int i = 0; i < 4; i++) {
            if (N) N[i] = L[i];
            if (dN)
                for (int k = 0; k < 3; k++) dN[i][k] = DL[i][k];
        }
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (N) N[i] = L[i] * (2 * L[i] - 1);
        if (dN)
            for (int k = 0; k < 3; k++) dN[i][k] = (4 * L[i] - 1) * DL[i][k];
    }
    for (int e = 0; e < 6; e++) {
        int a = TET_EDGE[e][0], b = TET_EDGE[e][1];
        if (N) N[4 + e] = 4 * L[a] * L[b];
        if (dN)
            for (int k = 0; k < 3; k++) dN[4 + e][k] = 4 * (L[a] * DL[b][k] + L[b] * DL[a][k]);
    }
}

double tet_jacobian(int type, const double (*X)[3], const double (*dN)[3], double J[3][3], double (*dNdx)[3]) {
    int n = tet_nodes(type);
    memset(J, 0, 9 * sizeof(double));
    for (int a = 0; a < n; a++)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) J[i][j] += X[a][i] * dN[a][j];
    double det = J[0][0] * (J[1][1] * J[2][2] - J[1][2] * J[2][1]) - J[0][1] * (J[1][0] * J[2][2] - J[1][2] * J[2][0]) +
                 J[0][2] * (J[1][0] * J[2][1] - J[1][1] * J[2][0]);
    if (dNdx && det != 0) {
        /* dN/dx = dN/dxi J^-1 */
        double inv[3][3];
        inv[0][0] = (J[1][1] * J[2][2] - J[1][2] * J[2][1]) / det;
        inv[0][1] = (J[0][2] * J[2][1] - J[0][1] * J[2][2]) / det;
        inv[0][2] = (J[0][1] * J[1][2] - J[0][2] * J[1][1]) / det;
        inv[1][0] = (J[1][2] * J[2][0] - J[1][0] * J[2][2]) / det;
        inv[1][1] = (J[0][0] * J[2][2] - J[0][2] * J[2][0]) / det;
        inv[1][2] = (J[0][2] * J[1][0] - J[0][0] * J[1][2]) / det;
        inv[2][0] = (J[1][0] * J[2][1] - J[1][1] * J[2][0]) / det;
        inv[2][1] = (J[0][1] * J[2][0] - J[0][0] * J[2][1]) / det;
        inv[2][2] = (J[0][0] * J[1][1] - J[0][1] * J[1][0]) / det;
        for (int a = 0; a < n; a++)
            for (int j = 0; j < 3; j++) dNdx[a][j] = dN[a][0] * inv[0][j] + dN[a][1] * inv[1][j] + dN[a][2] * inv[2][j];
    }
    return det;
}

/* B (6 x 3n) from dN/dx */
static void bmat(int n, const double (*d)[3], double *B) {
    memset(B, 0, 6 * 3 * (size_t)n * sizeof(double));
    int c = 3 * n;
    for (int a = 0; a < n; a++) {
        double x = d[a][0], y = d[a][1], z = d[a][2];
        B[0 * c + 3 * a + 0] = x;
        B[1 * c + 3 * a + 1] = y;
        B[2 * c + 3 * a + 2] = z;
        B[3 * c + 3 * a + 0] = y, B[3 * c + 3 * a + 1] = x;
        B[4 * c + 3 * a + 1] = z, B[4 * c + 3 * a + 2] = y;
        B[5 * c + 3 * a + 0] = z, B[5 * c + 3 * a + 2] = x;
    }
}

bool tet_stiffness(int type, const double (*X)[3], const double D[6][6], double *Ke, double *min_detJ) {
    int n = tet_nodes(type), ng = tet_gauss_count(type), nd = 3 * n;
    double L[TET_MAX_GP][4], w[TET_MAX_GP], dN[TET_MAX_NODES][3], dNdx[TET_MAX_NODES][3], J[3][3], B[6 * 30], DB[6 * 30];
    tet_gauss_points(type, L, w);
    memset(Ke, 0, (size_t)nd * nd * sizeof(double));
    double mind = INFINITY;
    for (int g = 0; g < ng; g++) {
        tet_shape(type, L[g], NULL, dN);
        double det = tet_jacobian(type, X, (const double (*)[3])dN, J, dNdx);
        if (det < mind) mind = det;
        if (!(det > 0)) {
            if (min_detJ) *min_detJ = det;
            return false;
        }
        bmat(n, (const double (*)[3])dNdx, B);
        for (int i = 0; i < 6; i++)
            for (int j = 0; j < nd; j++) {
                double s = 0;
                for (int k = 0; k < 6; k++) s += D[i][k] * B[k * nd + j];
                DB[i * nd + j] = s;
            }
        double f = det * w[g];
        for (int i = 0; i < nd; i++)
            for (int j = i; j < nd; j++) {
                double s = 0;
                for (int k = 0; k < 6; k++) s += B[k * nd + i] * DB[k * nd + j];
                Ke[i * nd + j] += f * s;
            }
    }
    for (int i = 0; i < nd; i++)
        for (int j = 0; j < i; j++) Ke[i * nd + j] = Ke[j * nd + i];
    if (min_detJ) *min_detJ = mind;
    return true;
}

void tet_gauss_strain_stress(int type, const double (*X)[3], const double D[6][6], const double *ue, const double *eps0, double (*eps)[6],
                             double (*sig)[6]) {
    int n = tet_nodes(type), ng = tet_gauss_count(type), nd = 3 * n;
    double L[TET_MAX_GP][4], w[TET_MAX_GP], dN[TET_MAX_NODES][3], dNdx[TET_MAX_NODES][3], J[3][3], B[6 * 30];
    tet_gauss_points(type, L, w);
    for (int g = 0; g < ng; g++) {
        tet_shape(type, L[g], NULL, dN);
        tet_jacobian(type, X, (const double (*)[3])dN, J, dNdx);
        bmat(n, (const double (*)[3])dNdx, B);
        for (int i = 0; i < 6; i++) {
            double s = 0;
            for (int j = 0; j < nd; j++) s += B[i * nd + j] * ue[j];
            eps[g][i] = s;
        }
        for (int i = 0; i < 6; i++) {
            double s = 0;
            for (int k = 0; k < 6; k++) s += D[i][k] * (eps[g][k] - (eps0 ? eps0[k] : 0));
            sig[g][i] = s;
        }
    }
}

void tet_initial_strain_load(int type, const double (*X)[3], const double D[6][6], const double eps0[6], double *fe) {
    int n = tet_nodes(type), ng = tet_gauss_count(type), nd = 3 * n;
    double L[TET_MAX_GP][4], w[TET_MAX_GP], dN[TET_MAX_NODES][3], dNdx[TET_MAX_NODES][3], J[3][3], B[6 * 30], s0[6];
    tet_gauss_points(type, L, w);
    memset(fe, 0, (size_t)nd * sizeof(double));
    for (int i = 0; i < 6; i++) {
        s0[i] = 0;
        for (int k = 0; k < 6; k++) s0[i] += D[i][k] * eps0[k];
    }
    for (int g = 0; g < ng; g++) {
        tet_shape(type, L[g], NULL, dN);
        double det = tet_jacobian(type, X, (const double (*)[3])dN, J, dNdx);
        bmat(n, (const double (*)[3])dNdx, B);
        for (int j = 0; j < nd; j++) {
            double s = 0;
            for (int i = 0; i < 6; i++) s += B[i * nd + j] * s0[i];
            fe[j] += s * det * w[g];
        }
    }
}

void tet_body_load(int type, const double (*X)[3], const double b[3], double *fe) {
    int n = tet_nodes(type), ng = tet_gauss_count(type);
    double L[TET_MAX_GP][4], w[TET_MAX_GP], N[TET_MAX_NODES], dN[TET_MAX_NODES][3], J[3][3];
    /* TET4: one point integrates N exactly; TET10: the four-point rule integrates the quadratic N exactly */
    tet_gauss_points(type, L, w);
    memset(fe, 0, 3 * (size_t)n * sizeof(double));
    for (int g = 0; g < ng; g++) {
        tet_shape(type, L[g], N, dN);
        double det = tet_jacobian(type, X, (const double (*)[3])dN, J, NULL);
        for (int a = 0; a < n; a++)
            for (int k = 0; k < 3; k++) fe[3 * a + k] += N[a] * b[k] * det * w[g];
    }
}

/* the face as a 3- or 6-node triangle: its node indices in the element (corners, then mid-edge nodes) */
static int face_nodes(int type, int face, int *idx) {
    for (int q = 0; q < 3; q++) idx[q] = TET_FACE[face][q];
    if (type != SOLID_ELEM_TET10) return 3;
    for (int q = 0; q < 3; q++) idx[3 + q] = TET10_FACE_MID[face][q];
    return 6;
}

/* degree-4 triangle rule (6 points; D. A. Dunavant, IJNME 21, 1985), barycentrics and weights summing to 1 */
static const double TRI6[6][4] = {
    {0.445948490915965, 0.445948490915965, 0.108103018168070, 0.223381589678011},
    {0.445948490915965, 0.108103018168070, 0.445948490915965, 0.223381589678011},
    {0.108103018168070, 0.445948490915965, 0.445948490915965, 0.223381589678011},
    {0.091576213509771, 0.091576213509771, 0.816847572980459, 0.109951743655322},
    {0.091576213509771, 0.816847572980459, 0.091576213509771, 0.109951743655322},
    {0.816847572980459, 0.091576213509771, 0.091576213509771, 0.109951743655322},
};

/* shape functions of the face triangle and the area vector (x_s1 x x_s2) at barycentrics s */
static void face_eval(int nfn, const double (*F)[3], const double s[3], double *N, double nda[3]) {
    double d1[6], d2[6];
    if (nfn == 3) {
        N[0] = s[0], N[1] = s[1], N[2] = s[2];
        d1[0] = -1, d1[1] = 1, d1[2] = 0;
        d2[0] = -1, d2[1] = 0, d2[2] = 1;
    } else {
        for (int i = 0; i < 3; i++) N[i] = s[i] * (2 * s[i] - 1);
        N[3] = 4 * s[0] * s[1], N[4] = 4 * s[1] * s[2], N[5] = 4 * s[2] * s[0];
        d1[0] = -(4 * s[0] - 1), d1[1] = 4 * s[1] - 1, d1[2] = 0, d1[3] = 4 * (s[0] - s[1]), d1[4] = 4 * s[2], d1[5] = -4 * s[2];
        d2[0] = -(4 * s[0] - 1), d2[1] = 0, d2[2] = 4 * s[2] - 1, d2[3] = -4 * s[1], d2[4] = 4 * s[1], d2[5] = 4 * (s[0] - s[2]);
    }
    double t1[3] = {0, 0, 0}, t2[3] = {0, 0, 0};
    for (int a = 0; a < nfn; a++)
        for (int k = 0; k < 3; k++) t1[k] += d1[a] * F[a][k], t2[k] += d2[a] * F[a][k];
    nda[0] = t1[1] * t2[2] - t1[2] * t2[1];
    nda[1] = t1[2] * t2[0] - t1[0] * t2[2];
    nda[2] = t1[0] * t2[1] - t1[1] * t2[0];
}

static void face_integrate(int type, const double (*X)[3], int face, const double *t, double p, double *fe, double *area, double normal[3]) {
    int idx[6], nfn = face_nodes(type, face, idx), n = tet_nodes(type);
    double F[6][3];
    for (int a = 0; a < nfn; a++) memcpy(F[a], X[idx[a]], sizeof F[a]);
    memset(fe, 0, 3 * (size_t)n * sizeof(double));
    double A = 0, nsum[3] = {0, 0, 0};
    for (int g = 0; g < 6; g++) {
        double N[6], nda[3];
        face_eval(nfn, (const double (*)[3])F, TRI6[g], N, nda);
        double w = 0.5 * TRI6[g][3], dA = sqrt(nda[0] * nda[0] + nda[1] * nda[1] + nda[2] * nda[2]);
        A += w * dA;
        for (int k = 0; k < 3; k++) nsum[k] += w * nda[k];
        for (int a = 0; a < nfn; a++)
            for (int k = 0; k < 3; k++) fe[3 * idx[a] + k] += N[a] * w * (t ? t[k] * dA : -p * nda[k]);
    }
    if (area) *area = A;
    if (normal) {
        double l = sqrt(nsum[0] * nsum[0] + nsum[1] * nsum[1] + nsum[2] * nsum[2]);
        for (int k = 0; k < 3; k++) normal[k] = l > 0 ? nsum[k] / l : 0;
    }
}

void tet_face_load(int type, const double (*X)[3], int face, const double t[3], double *fe, double *area, double normal[3]) {
    face_integrate(type, X, face, t, 0, fe, area, normal);
}

void tet_face_pressure(int type, const double (*X)[3], int face, double p, double *fe, double *area) {
    face_integrate(type, X, face, NULL, p, fe, area, NULL);
}

void tet_face_load_fn(int type, const double (*X)[3], int face, TetTractionFn fn, void *ctx, double *fe) {
    int idx[6], nfn = face_nodes(type, face, idx), n = tet_nodes(type);
    double F[6][3];
    for (int a = 0; a < nfn; a++) memcpy(F[a], X[idx[a]], sizeof F[a]);
    memset(fe, 0, 3 * (size_t)n * sizeof(double));
    for (int g = 0; g < 6; g++) {
        double N[6], nda[3], x[3] = {0, 0, 0}, t[3];
        face_eval(nfn, (const double (*)[3])F, TRI6[g], N, nda);
        double dA = sqrt(nda[0] * nda[0] + nda[1] * nda[1] + nda[2] * nda[2]), w = 0.5 * TRI6[g][3], nu[3];
        for (int k = 0; k < 3; k++) nu[k] = dA > 0 ? nda[k] / dA : 0;
        for (int a = 0; a < nfn; a++)
            for (int k = 0; k < 3; k++) x[k] += N[a] * F[a][k];
        fn(ctx, x, nu, t);
        for (int a = 0; a < nfn; a++)
            for (int k = 0; k < 3; k++) fe[3 * idx[a] + k] += N[a] * w * dA * t[k];
    }
}

void tet_gauss_dv(int type, const double (*X)[3], double *dv) {
    int ng = tet_gauss_count(type);
    double L[TET_MAX_GP][4], w[TET_MAX_GP], dN[TET_MAX_NODES][3], J[3][3];
    tet_gauss_points(type, L, w);
    for (int g = 0; g < ng; g++) {
        tet_shape(type, L[g], NULL, dN);
        dv[g] = tet_jacobian(type, X, (const double (*)[3])dN, J, NULL) * w[g];
    }
}

double tet_volume(int type, const double (*X)[3]) {
    /* degree 3 (curved TET10 has a cubic Jacobian): centroid -4/5, (1/2, 1/6, 1/6, 1/6) and permutations 9/20 */
    double dN[TET_MAX_NODES][3], J[3][3], v;
    double c[4] = {0.25, 0.25, 0.25, 0.25};
    tet_shape(type, c, NULL, dN);
    v = -0.8 * tet_jacobian(type, X, (const double (*)[3])dN, J, NULL);
    for (int k = 0; k < 4; k++) {
        double L[4] = {1.0 / 6, 1.0 / 6, 1.0 / 6, 1.0 / 6};
        L[k] = 0.5;
        tet_shape(type, L, NULL, dN);
        v += 0.45 * tet_jacobian(type, X, (const double (*)[3])dN, J, NULL);
    }
    return v / 6.0;
}

void tet_extrapolate(int type, const double *gp, int ncomp, double *nodal) {
    if (type != SOLID_ELEM_TET10) {
        for (int a = 0; a < 4; a++) memcpy(nodal + (size_t)a * ncomp, gp, (size_t)ncomp * sizeof(double));
        return;
    }
    /* a linear field sum c_i L_i through the four points: f_g = (a - b) c_g + b sum c, and sum f = sum c */
    const double a = 0.5854101966249685, b = 0.1381966011250105;
    for (int k = 0; k < ncomp; k++) {
        double sum = 0;
        for (int g = 0; g < 4; g++) sum += gp[g * ncomp + k];
        for (int g = 0; g < 4; g++) nodal[g * ncomp + k] = (gp[g * ncomp + k] - b * sum) / (a - b);
        for (int e = 0; e < 6; e++) nodal[(4 + e) * ncomp + k] = 0.5 * (nodal[TET_EDGE[e][0] * ncomp + k] + nodal[TET_EDGE[e][1] * ncomp + k]);
    }
}

void tet_interpolate(int type, const double *nodal, int ncomp, const double L[4], double *out) {
    double N[TET_MAX_NODES];
    tet_shape(type, L, N, NULL);
    int n = tet_nodes(type);
    for (int k = 0; k < ncomp; k++) {
        double s = 0;
        for (int a = 0; a < n; a++) s += N[a] * nodal[(size_t)a * ncomp + k];
        out[k] = s;
    }
}

/* ---- the solver on a tetrahedral model ------------------------------------------------------------------------ */

static int ufind(int *p, int x) {
    while (p[x] != x) p[x] = p[p[x]], x = p[x];
    return x;
}

typedef struct {
    int n[3], e;
} TFace;

static int tface_cmp(const void *a, const void *b) {
    const TFace *x = a, *y = b;
    for (int k = 0; k < 3; k++)
        if (x->n[k] != y->n[k]) return x->n[k] < y->n[k] ? -1 : 1;
    return x->e - y->e;
}

static int tet_regions(const HexModel *m, int *region) {
    int ne = m->nelems, npe = tet_nodes(m->elem_type);
    TFace *f = malloc((size_t)(ne ? ne : 1) * 4 * sizeof *f);
    int *par = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    if (!f || !par) {
        free(f), free(par);
        return -1;
    }
    for (int e = 0; e < ne; e++) {
        par[e] = e;
        for (int k = 0; k < 4; k++) {
            TFace *q = &f[4 * (size_t)e + k];
            for (int j = 0; j < 3; j++) q->n[j] = m->conn[(size_t)npe * e + TET_FACE[k][j]];
            for (int i = 1; i < 3; i++)
                for (int j = i; j > 0 && q->n[j] < q->n[j - 1]; j--) {
                    int t = q->n[j];
                    q->n[j] = q->n[j - 1], q->n[j - 1] = t;
                }
            q->e = e;
        }
    }
    qsort(f, 4 * (size_t)ne, sizeof *f, tface_cmp);
    for (size_t i = 1; i < 4 * (size_t)ne; i++)
        if (!memcmp(f[i].n, f[i - 1].n, sizeof f[i].n)) {
            int a = ufind(par, f[i].e), b = ufind(par, f[i - 1].e);
            if (a != b) par[a < b ? b : a] = a < b ? a : b;
        }
    free(f);
    int nr = 0;
    int *label = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    if (!label) {
        free(par);
        return -1;
    }
    for (int e = 0; e < ne; e++) label[e] = -1;
    for (int e = 0; e < ne; e++) {
        int r = ufind(par, e);
        if (label[r] < 0) label[r] = nr++;
        region[e] = label[r];
    }
    free(label), free(par);
    return nr;
}

bool tet_check_constraints(const HexModel *m, const unsigned char *fixed, ConstraintReport *rep) {
    memset(rep, 0, sizeof *rep);
    int ne = m->nelems, nn = m->nnodes, npe = tet_nodes(m->elem_type);
    int *region = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    int nr = region ? tet_regions(m, region) : -1;
    if (nr < 0) {
        free(region);
        rep->nissues = -1;
        return false;
    }
    rep->regions = nr;
    int *node_region = malloc((size_t)(nn ? nn : 1) * sizeof(int));
    unsigned char *multi = calloc((size_t)(nn ? nn : 1), 1);
    double *G = calloc((size_t)(nr ? nr : 1) * 36, sizeof(double)), *cen = calloc((size_t)(nr ? nr : 1) * 4, sizeof(double));
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
        for (int a = 0; a < npe; a++) {
            int nd = m->conn[(size_t)npe * e + a];
            if (node_region[nd] < 0) node_region[nd] = r;
            else if (node_region[nd] != r) multi[nd] = 1;
        }
    }
    for (int nd = 0; nd < nn; nd++) {
        int r = node_region[nd];
        if (r < 0) continue;
        for (int k = 0; k < 3; k++) {
            double x = m->xyz[3 * (size_t)nd + k];
            cen[4 * r + k] += x;
            lo[3 * r + k] = fmin(lo[3 * r + k], x), hi[3 * r + k] = fmax(hi[3 * r + k], x);
        }
        cen[4 * r + 3] += 1;
    }
    for (int r = 0; r < nr; r++)
        for (int k = 0; k < 3; k++) cen[4 * r + k] /= cen[4 * r + 3] > 0 ? cen[4 * r + 3] : 1;
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < npe; a++)
            if (multi[m->conn[(size_t)npe * e + a]]) weak[region[e]]++;
    for (int e = 0; e < ne; e++) {
        int r = region[e];
        double L = 0;
        for (int k = 0; k < 3; k++) L = fmax(L, hi[3 * r + k] - lo[3 * r + k]);
        if (!(L > 0)) L = 1;
        for (int a = 0; a < npe; a++) {
            int nd = m->conn[(size_t)npe * e + a];
            if (node_region[nd] != r && !multi[nd]) continue;
            double d[3];
            for (int k = 0; k < 3; k++) d[k] = (m->xyz[3 * (size_t)nd + k] - cen[4 * r + k]) / L;
            for (int c = 0; c < 3; c++) {
                if (!fixed[3 * (size_t)nd + c]) continue;
                double row[6] = {c == 0, c == 1, c == 2, 0, 0, 0};
                double rx[3] = {0, -d[2], d[1]}, ry[3] = {d[2], 0, -d[0]}, rz[3] = {-d[1], d[0], 0};
                row[3] = rx[c], row[4] = ry[c], row[5] = rz[c];
                for (int i = 0; i < 6; i++)
                    for (int j = 0; j < 6; j++) G[36 * (size_t)r + i * 6 + j] += row[i] * row[j] / npe;
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
        if (!nfree) continue;
        ok = false;
        if (rep->nissues < SOLID_MAX_ISSUES) {
            ConstraintIssue *is = &rep->issue[rep->nissues++];
            is->region = r, is->elements = relems[r];
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

typedef struct {
    const HexModel *m;
    const SolidLoads *L;
    const int *eq, *order;
    double (*D)[6][6];
    CsrMatrix *K;
    double *f, *fext_full;
    bool failed;
    int bad_elem;
    double *mindet; /* per thread */
} TAsm;

static void elem_x(const HexModel *m, int e, double (*X)[3]) {
    int n = tet_nodes(m->elem_type);
    for (int a = 0; a < n; a++) memcpy(X[a], m->xyz + 3 * (size_t)m->conn[(size_t)n * e + a], 3 * sizeof(double));
}

static void tasm_range(void *vctx, int begin, int end, int tid) {
    TAsm *c = vctx;
    const HexModel *m = c->m;
    int type = m->elem_type, n = tet_nodes(type), nd = 3 * n;
    double Ke[900], X[TET_MAX_NODES][3], fe[30], fl[30], fb[30];
    int dofs[30];
    for (int idx = begin; idx < end; idx++) {
        int e = c->order[idx], mi = m->elem_mat ? m->elem_mat[e] : 0;
        elem_x(m, e, X);
        double mdj;
        if (!tet_stiffness(type, (const double (*)[3])X, c->D[mi], Ke, &mdj)) {
            c->failed = true, c->bad_elem = e;
            continue;
        }
        if (mdj < c->mindet[tid]) c->mindet[tid] = mdj;
        double sc = m->elem_scale ? m->elem_scale[e] : 1.0;
        if (sc != 1.0)
            for (int i = 0; i < nd * nd; i++) Ke[i] *= sc;
        for (int a = 0; a < n; a++)
            for (int k = 0; k < 3; k++) dofs[3 * a + k] = c->eq[3 * m->conn[(size_t)n * e + a] + k];
        csr_add_element(c->K, nd, dofs, Ke);
        memset(fe, 0, sizeof fe), memset(fl, 0, sizeof fl);
        if (c->L->fixed_value) {
            double ub[30];
            bool any = false;
            for (int a = 0; a < n; a++)
                for (int k = 0; k < 3; k++) {
                    int node = m->conn[(size_t)n * e + a];
                    ub[3 * a + k] = c->L->fixed[3 * (size_t)node + k] ? c->L->fixed_value[3 * (size_t)node + k] : 0;
                    any |= ub[3 * a + k] != 0;
                }
            if (any)
                for (int i = 0; i < nd; i++) {
                    if (dofs[i] < 0) continue;
                    double s = 0;
                    for (int j = 0; j < nd; j++) s += Ke[i * nd + j] * ub[j];
                    fe[i] -= s;
                }
        }
        if (c->L->eps0) {
            tet_initial_strain_load(type, (const double (*)[3])X, c->D[mi], c->L->eps0 + 6 * (size_t)e, fl);
            if (sc != 1.0)
                for (int i = 0; i < nd; i++) fl[i] *= sc;
        }
        if (c->L->gravity[0] != 0 || c->L->gravity[1] != 0 || c->L->gravity[2] != 0) {
            double rho = m->mat[mi].density, b[3] = {rho * c->L->gravity[0], rho * c->L->gravity[1], rho * c->L->gravity[2]};
            tet_body_load(type, (const double (*)[3])X, b, fb);
            for (int a = 0; a < n; a++)
                for (int k = 0; k < 3; k++) c->fext_full[3 * (size_t)m->conn[(size_t)n * e + a] + k] += fb[3 * a + k];
            for (int i = 0; i < nd; i++) fl[i] += fb[i];
        }
        for (int i = 0; i < nd; i++)
            if (dofs[i] >= 0) c->f[dofs[i]] += fe[i] + fl[i];
    }
}

typedef struct {
    const HexModel *m;
    const SolidLoads *L;
    const int *order;
    double (*D)[6][6];
    SolidResult *res;
    double *fint, *nsum, *ncount, *energy;
} TRec;

static void trec_range(void *vctx, int begin, int end, int tid) {
    TRec *c = vctx;
    const HexModel *m = c->m;
    int type = m->elem_type, n = tet_nodes(type), ng = tet_gauss_count(type), nd = 3 * n;
    double X[TET_MAX_NODES][3], ue[30], eps[TET_MAX_GP][6], sig[TET_MAX_GP][6], Ke[900], f0[30], dv[TET_MAX_GP], nodal[60];
    for (int idx = begin; idx < end; idx++) {
        int e = c->order[idx], mi = m->elem_mat ? m->elem_mat[e] : 0;
        elem_x(m, e, X);
        for (int a = 0; a < n; a++)
            for (int k = 0; k < 3; k++) ue[3 * a + k] = c->res->u[3 * m->conn[(size_t)n * e + a] + k];
        const double *e0 = c->L->eps0 ? c->L->eps0 + 6 * (size_t)e : NULL;
        tet_gauss_strain_stress(type, (const double (*)[3])X, c->D[mi], ue, e0, eps, sig);
        double sc = m->elem_scale ? m->elem_scale[e] : 1.0;
        if (sc != 1.0)
            for (int g = 0; g < ng; g++)
                for (int k = 0; k < 6; k++) sig[g][k] *= sc;
        memcpy(c->res->gp_strain + 6 * (size_t)ng * e, eps, 6 * (size_t)ng * sizeof(double));
        memcpy(c->res->gp_stress + 6 * (size_t)ng * e, sig, 6 * (size_t)ng * sizeof(double));
        if (!tet_stiffness(type, (const double (*)[3])X, c->D[mi], Ke, NULL)) continue;
        memset(f0, 0, sizeof f0);
        if (e0) tet_initial_strain_load(type, (const double (*)[3])X, c->D[mi], e0, f0);
        tet_gauss_dv(type, (const double (*)[3])X, dv);
        double w = 0;
        for (int g = 0; g < ng; g++) {
            double s = 0;
            for (int k = 0; k < 6; k++) s += sig[g][k] * (eps[g][k] - (e0 ? e0[k] : 0));
            w += 0.5 * s * dv[g];
        }
        c->energy[tid] += w;
        tet_extrapolate(type, (const double *)sig, 6, nodal);
        for (int a = 0; a < n; a++) {
            int node = m->conn[(size_t)n * e + a];
            for (int k = 0; k < 3; k++) {
                double s = 0;
                for (int j = 0; j < nd; j++) s += Ke[(3 * a + k) * nd + j] * ue[j];
                c->fint[3 * (size_t)node + k] += sc * (s - f0[3 * a + k]);
            }
            for (int k = 0; k < 6; k++) c->nsum[6 * (size_t)node + k] += nodal[6 * a + k];
            c->ncount[node] += 1;
        }
    }
}

/* ---- two-level preconditioner for TET10: the TET4 problem on the corner nodes as the coarse space ----------------
 * Block-Jacobi PCG converges too slowly on quadratic tetrahedra (a 126 000-element assembly did not converge in
 * 20 000 iterations). The linear displacement field on the corner nodes is contained in the quadratic one, so the TET4
 * stiffness of the same corners is a good coarse operator: it is factorised once (sparse Cholesky, nested dissection)
 * and every application of the preconditioner is a symmetric V-cycle: damped block-Jacobi smoothing, the coarse
 * correction through P (mid-edge nodes take the mean of their corners), smoothing again. */

typedef struct {
    const CsrMatrix *K;
    int neq, nc;         /* fine and coarse equations */
    int *peq;            /* fine eq -> coarse eqs (up to 2) and weights: pa[2*q], pw[2*q] */
    int *pa;
    double *pw;
    double *binv;        /* block inverses, 9 per fine eq block start (blocks of consecutive eqs of a node) */
    int *blk;            /* fine eq -> first eq of its block */
    int *bsz;            /* size of the block at its first eq */
    double omega;
    CholFactor F;
    double *t1, *t2, *rc, *xc;
    ThreadPool *pool;
} Pmg;

static void pmg_smooth(const Pmg *g, const double *r, double *z) {
    for (int q = 0; q < g->neq; q++) {
        if (g->blk[q] != q) continue;
        int n = g->bsz[q];
        const double *B = g->binv + 9 * (size_t)q;
        for (int i = 0; i < n; i++) {
            double s = 0;
            for (int j = 0; j < n; j++) s += B[3 * i + j] * r[q + j];
            z[q + i] = g->omega * s;
        }
    }
}

/* z = M^-1 r */
static void pmg_apply(Pmg *g, const double *r, double *z) {
    int n = g->neq;
    double *t = g->t1, *Az = g->t2;
    pmg_smooth(g, r, z);                                         /* pre-smoothing */
    csr_spmv(g->K, z, Az, g->pool);
    for (int q = 0; q < n; q++) t[q] = r[q] - Az[q];
    memset(g->rc, 0, (size_t)g->nc * sizeof(double));           /* restriction P^T */
    for (int q = 0; q < n; q++)
        for (int k = 0; k < 2; k++)
            if (g->pa[2 * q + k] >= 0) g->rc[g->pa[2 * q + k]] += g->pw[2 * q + k] * t[q];
    chol_solve(&g->F, g->rc, g->xc);                             /* coarse solve */
    for (int q = 0; q < n; q++)                                  /* prolongation */
        for (int k = 0; k < 2; k++)
            if (g->pa[2 * q + k] >= 0) z[q] += g->pw[2 * q + k] * g->xc[g->pa[2 * q + k]];
    csr_spmv(g->K, z, Az, g->pool);                              /* post-smoothing */
    for (int q = 0; q < n; q++) t[q] = r[q] - Az[q];
    double *dz = Az;
    pmg_smooth(g, t, dz);
    for (int q = 0; q < n; q++) z[q] += dz[q];
}

static void inv3x3(const double *a, int n, double *out) {
    memset(out, 0, 9 * sizeof(double));
    if (n == 1) {
        out[0] = 1 / a[0];
        return;
    }
    if (n == 2) {
        double d = a[0] * a[4] - a[1] * a[3];
        out[0] = a[4] / d, out[1] = -a[1] / d, out[3] = -a[3] / d, out[4] = a[0] / d;
        return;
    }
    double d = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    out[0] = (a[4] * a[8] - a[5] * a[7]) / d, out[1] = (a[2] * a[7] - a[1] * a[8]) / d, out[2] = (a[1] * a[5] - a[2] * a[4]) / d;
    out[3] = (a[5] * a[6] - a[3] * a[8]) / d, out[4] = (a[0] * a[8] - a[2] * a[6]) / d, out[5] = (a[2] * a[3] - a[0] * a[5]) / d;
    out[6] = (a[3] * a[7] - a[4] * a[6]) / d, out[7] = (a[1] * a[6] - a[0] * a[7]) / d, out[8] = (a[0] * a[4] - a[1] * a[3]) / d;
}

/* PCG with the two-level preconditioner. x starts at zero. */
static bool pmg_pcg(const HexModel *m, const CsrMatrix *K, const int *eq, const double (*D)[6][6], const double *b, double *x, const SolidOptions *opt,
                    SolveStats *st, char *err, size_t errlen) {
    int nn = m->nnodes, ne = m->nelems, neq = K->n;
    double t0 = now_seconds();
    Pmg g = {0};
    g.K = K, g.neq = neq, g.pool = opt->pool;
    bool ok = false;
    int *ceq = malloc((size_t)(nn ? nn : 1) * 3 * sizeof(int)), *corner = calloc((size_t)(nn ? nn : 1), sizeof(int));
    int *cdofs = malloc((size_t)(ne ? ne : 1) * 12 * sizeof(int)), *perm = NULL, *adj = NULL, *nstart = NULL, *nel = NULL, *mark = NULL;
    int64_t *adjp = NULL;
    double *r = NULL, *z = NULL, *p = NULL, *Ap = NULL;
    CsrMatrix Kc = {0};
    g.pa = malloc(2 * (size_t)(neq ? neq : 1) * sizeof(int));
    g.pw = malloc(2 * (size_t)(neq ? neq : 1) * sizeof(double));
    g.binv = calloc(9 * (size_t)(neq ? neq : 1), sizeof(double));
    g.blk = malloc((size_t)(neq ? neq : 1) * sizeof(int));
    g.bsz = calloc((size_t)(neq ? neq : 1), sizeof(int));
    if (!ceq || !corner || !cdofs || !g.pa || !g.pw || !g.binv || !g.blk || !g.bsz) goto oom;
    /* coarse equations: the unconstrained components of corner nodes */
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 4; a++) corner[m->conn[10 * (size_t)e + a]] = 1;
    int nc = 0;
    for (int n = 0; n < nn; n++)
        for (int k = 0; k < 3; k++) ceq[3 * n + k] = corner[n] && eq[3 * n + k] >= 0 ? nc++ : -1;
    g.nc = nc;
    /* the coarse operator: TET4 stiffness of the corner tetrahedra */
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 4; a++)
            for (int k = 0; k < 3; k++) cdofs[12 * (size_t)e + 3 * a + k] = ceq[3 * m->conn[10 * (size_t)e + a] + k];
    if (!csr_from_elements(&Kc, nc, ne, 12, cdofs, err, errlen)) goto fail;
    for (int e = 0; e < ne; e++) {
        double X[4][3], Ke[144], mdj;
        for (int a = 0; a < 4; a++) memcpy(X[a], m->xyz + 3 * (size_t)m->conn[10 * (size_t)e + a], sizeof X[a]);
        int mi = m->elem_mat ? m->elem_mat[e] : 0;
        if (!tet_stiffness(SOLID_ELEM_TET4, (const double (*)[3])X, D[mi], Ke, &mdj)) continue;
        double sc = m->elem_scale ? m->elem_scale[e] : 1.0;
        if (sc != 1.0)
            for (int i = 0; i < 144; i++) Ke[i] *= sc;
        csr_add_element(&Kc, 12, cdofs + 12 * (size_t)e, Ke);
    }
    /* nested dissection over the corner nodes */
    adjp = calloc((size_t)nn + 1, sizeof(int64_t));
    perm = malloc((size_t)(nc ? nc : 1) * sizeof(int));
    mark = malloc((size_t)(nn ? nn : 1) * sizeof(int));
    nstart = calloc((size_t)nn + 1, sizeof(int));
    if (!adjp || !perm || !mark || !nstart) goto oom;
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 4; a++) nstart[m->conn[10 * (size_t)e + a] + 1]++;
    for (int i = 0; i < nn; i++) nstart[i + 1] += nstart[i];
    nel = malloc((size_t)(nstart[nn] ? nstart[nn] : 1) * sizeof(int));
    if (!nel) goto oom;
    {
        int *fill = malloc((size_t)(nn ? nn : 1) * sizeof(int));
        if (!fill) goto oom;
        memcpy(fill, nstart, (size_t)nn * sizeof(int));
        for (int e = 0; e < ne; e++)
            for (int a = 0; a < 4; a++) nel[fill[m->conn[10 * (size_t)e + a]]++] = e;
        free(fill);
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nn; i++) mark[i] = -1;
        for (int i = 0; i < nn; i++) {
            int64_t cnt = 0, w = pass ? adjp[i] : 0;
            for (int j = nstart[i]; j < nstart[i + 1]; j++)
                for (int a = 0; a < 4; a++) {
                    int o = m->conn[10 * (size_t)nel[j] + a];
                    if (o == i || mark[o] == i) continue;
                    mark[o] = i;
                    if (pass) adj[w++] = o;
                    else cnt++;
                }
            if (!pass) adjp[i + 1] = cnt;
        }
        if (!pass) {
            for (int i = 0; i < nn; i++) adjp[i + 1] += adjp[i];
            adj = malloc((size_t)(adjp[nn] ? adjp[nn] : 1) * sizeof(int));
            if (!adj) goto oom;
        }
    }
    if (!nested_dissection_order(nn, m->xyz, adjp, adj, ceq, nc, perm, err, errlen)) goto fail;
    {
        SolveStats cs = {0};
        int bad = -1;
        if (!chol_factor(&Kc, perm, &g.F, NULL, NULL, &cs, &bad, err, errlen)) goto fail;
        st->factor_nnz = cs.factor_nnz, st->factor_mb = cs.factor_mb;
    }
    csr_free(&Kc);
    /* prolongation: a corner component maps to itself, a mid-edge component to the mean of its edge's corners */
    for (int q = 0; q < neq; q++) g.pa[2 * q] = g.pa[2 * q + 1] = -1, g.pw[2 * q] = g.pw[2 * q + 1] = 0;
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 10; a++) {
            int n = m->conn[10 * (size_t)e + a];
            for (int k = 0; k < 3; k++) {
                int q = eq[3 * n + k];
                if (q < 0 || g.pa[2 * q] >= 0 || g.pa[2 * q + 1] >= 0) continue;
                if (a < 4) g.pa[2 * q] = ceq[3 * n + k], g.pw[2 * q] = 1;
                else {
                    int c0 = m->conn[10 * (size_t)e + TET_EDGE[a - 4][0]], c1 = m->conn[10 * (size_t)e + TET_EDGE[a - 4][1]];
                    g.pa[2 * q] = ceq[3 * c0 + k], g.pw[2 * q] = 0.5;
                    g.pa[2 * q + 1] = ceq[3 * c1 + k], g.pw[2 * q + 1] = 0.5;
                }
            }
        }
    /* block Jacobi: the 3 x 3 (or smaller) diagonal block of each node's equations */
    for (int n = 0; n < nn; n++) {
        int q0 = -1, cnt = 0, qs[3];
        for (int k = 0; k < 3; k++)
            if (eq[3 * n + k] >= 0) qs[cnt++] = eq[3 * n + k];
        if (!cnt) continue;
        q0 = qs[0];
        double a[9] = {0};
        for (int i = 0; i < cnt; i++)
            for (int j = 0; j < cnt; j++) {
                int64_t k = csr_find(K, qs[i], qs[j]);
                a[3 * i + j] = k >= 0 ? K->val[k] : 0;
            }
        inv3x3(a, cnt, g.binv + 9 * (size_t)q0);
        g.bsz[q0] = cnt;
        for (int i = 0; i < cnt; i++) g.blk[qs[i]] = q0;
    }
    g.t1 = malloc((size_t)neq * sizeof(double));
    g.t2 = malloc((size_t)neq * sizeof(double));
    g.rc = malloc((size_t)(nc ? nc : 1) * sizeof(double));
    g.xc = malloc((size_t)(nc ? nc : 1) * sizeof(double));
    r = malloc((size_t)neq * sizeof(double)), z = malloc((size_t)neq * sizeof(double));
    p = malloc((size_t)neq * sizeof(double)), Ap = malloc((size_t)neq * sizeof(double));
    if (!g.t1 || !g.t2 || !g.rc || !g.xc || !r || !z || !p || !Ap) goto oom;
    /* damping: omega = 1 / lambda_max(B^-1 K), from a few power iterations */
    g.omega = 1;
    for (int q = 0; q < neq; q++) p[q] = 1.0 + 0.01 * (q % 7);
    double lam = 1;
    for (int it = 0; it < 12; it++) {
        csr_spmv(K, p, Ap, opt->pool);
        pmg_smooth(&g, Ap, z);
        double nz = 0, np = 0;
        for (int q = 0; q < neq; q++) nz += z[q] * z[q], np += p[q] * p[q];
        lam = sqrt(nz / (np > 0 ? np : 1));
        double s = 1 / sqrt(nz > 0 ? nz : 1);
        for (int q = 0; q < neq; q++) p[q] = z[q] * s;
    }
    g.omega = 1.0 / (1.1 * lam);
    /* conjugate gradients */
    double bb = 0;
    for (int q = 0; q < neq; q++) x[q] = 0, r[q] = b[q], bb += b[q] * b[q];
    double bn = sqrt(bb), tol = opt->pcg_tol > 0 ? opt->pcg_tol : 1e-10, rel = 1;
    if (!(bn > 0)) {
        ok = true, st->converged = true;
        goto done;
    }
    pmg_apply(&g, r, z);
    memcpy(p, z, (size_t)neq * sizeof(double));
    double rz = 0;
    for (int q = 0; q < neq; q++) rz += r[q] * z[q];
    int it;
    int maxit = opt->pcg_max_iter > 0 ? opt->pcg_max_iter : 20000;
    for (it = 1; it <= maxit; it++) {
        csr_spmv(K, p, Ap, opt->pool);
        double pAp = 0;
        for (int q = 0; q < neq; q++) pAp += p[q] * Ap[q];
        if (!(pAp > 0)) {
            snprintf(err, errlen, "the stiffness matrix is not positive definite (an unconstrained rigid-body motion)");
            goto fail;
        }
        double alpha = rz / pAp, rr = 0;
        for (int q = 0; q < neq; q++) x[q] += alpha * p[q], r[q] -= alpha * Ap[q], rr += r[q] * r[q];
        rel = sqrt(rr) / bn;
        if (opt->progress && it % 10 == 0 && !opt->progress(opt->ctx, it, rel)) {
            st->cancelled = true;
            snprintf(err, errlen, "cancelled");
            goto fail;
        }
        if (rel <= tol) break;
        pmg_apply(&g, r, z);
        double rz2 = 0;
        for (int q = 0; q < neq; q++) rz2 += r[q] * z[q];
        double beta = rz2 / rz;
        rz = rz2;
        for (int q = 0; q < neq; q++) p[q] = z[q] + beta * p[q];
    }
    st->iterations = it > maxit ? maxit : it;
    st->rel_residual = rel;
    st->converged = rel <= tol;
    if (!st->converged) {
        snprintf(err, errlen, "conjugate gradients with the two-level preconditioner did not converge in %d iterations (relative residual %.3g, target %.3g)",
                 maxit, rel, tol);
        goto fail;
    }
    ok = true;
    goto done;
oom:
    snprintf(err, errlen, "out of memory building the two-level preconditioner");
fail:
    ok = false;
done:
    snprintf(st->method, sizeof st->method, "pcg-two-level-tet4");
    st->seconds = now_seconds() - t0;
    if (g.F.Lx || g.F.perm) chol_free(&g.F);
    csr_free(&Kc);
    free(ceq), free(corner), free(cdofs), free(perm), free(adj), free(nstart), free(nel), free(mark), free(adjp);
    free(g.pa), free(g.pw), free(g.binv), free(g.blk), free(g.bsz), free(g.t1), free(g.t2), free(g.rc), free(g.xc);
    free(r), free(z), free(p), free(Ap);
    return ok;
}

bool tet_solid_solve(const HexModel *m, const SolidLoads *L, const SolidOptions *optp, SolidResult *res, char *err, size_t errlen) {
    memset(res, 0, sizeof *res);
    SolidOptions opt = optp ? *optp : (SolidOptions){0};
    if (opt.pcg_tol <= 0) opt.pcg_tol = 1e-10;
    if (opt.pcg_max_iter <= 0) opt.pcg_max_iter = 20000;
    if (opt.max_factor_nnz <= 0) opt.max_factor_nnz = 60000000;
    int type = m->elem_type, n = tet_nodes(type), ng = tet_gauss_count(type), nn = m->nnodes, ne = m->nelems;
    if (type != SOLID_ELEM_TET4 && type != SOLID_ELEM_TET10) {
        snprintf(err, errlen, "unknown element type %d", type);
        return false;
    }
    res->gp_per_elem = ng;
    bool ok = false;
    int *eq = malloc((size_t)(nn ? nn : 1) * 3 * sizeof(int)), *color = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    int *order = malloc((size_t)(ne ? ne : 1) * sizeof(int)), *cstart = NULL, *elem_dofs = NULL, *perm = NULL, *block_of = NULL, *adj = NULL;
    unsigned char *used = calloc((size_t)(nn ? nn : 1), 1);
    uint64_t *node_colors = calloc((size_t)(nn ? nn : 1) * 4, sizeof(uint64_t));
    double (*D)[6][6] = malloc((size_t)(m->nmat ? m->nmat : 1) * sizeof *D);
    double *f = NULL, *fext_full = NULL, *x = NULL, *fint = NULL, *nsum = NULL, *ncount = NULL, *energy = NULL, *mindet = NULL;
    int64_t *adjp = NULL;
    CsrMatrix K = {0};
    int nthreads = opt.pool ? pool_size(opt.pool) : 1;
    if (!eq || !color || !order || !used || !node_colors || !D) goto oom;
    for (int i = 0; i < m->nmat; i++) {
        if (!(m->mat[i].E > 0) || !(m->mat[i].nu > -1 && m->mat[i].nu < 0.5)) {
            snprintf(err, errlen, "material %d has invalid elastic constants (E = %g Pa, nu = %g)", i, m->mat[i].E, m->mat[i].nu);
            goto fail;
        }
        isotropic_D(m->mat[i].E, m->mat[i].nu, D[i]);
    }
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < n; a++) used[m->conn[(size_t)n * e + a]] = 1;
    int neq = 0;
    for (int nd = 0; nd < nn; nd++)
        for (int k = 0; k < 3; k++) eq[3 * nd + k] = (used[nd] && !L->fixed[3 * (size_t)nd + k]) ? neq++ : -1;
    res->neq = neq;
    /* greedy colouring with up to 256 colours (a tetrahedral node has more neighbours than a hexahedral one) */
    int ncolors = 0;
    for (int e = 0; e < ne; e++) {
        uint64_t mask[4] = {0, 0, 0, 0};
        for (int a = 0; a < n; a++)
            for (int w = 0; w < 4; w++) mask[w] |= node_colors[4 * (size_t)m->conn[(size_t)n * e + a] + w];
        int c = 0;
        while (c < 256 && (mask[c >> 6] >> (c & 63)) & 1) c++;
        if (c == 256) c = 255; /* the last colour is then assembled serially */
        color[e] = c;
        if (c + 1 > ncolors) ncolors = c + 1;
        for (int a = 0; a < n; a++) node_colors[4 * (size_t)m->conn[(size_t)n * e + a] + (c >> 6)] |= (uint64_t)1 << (c & 63);
    }
    res->colors = ncolors;
    cstart = calloc((size_t)ncolors + 1, sizeof(int));
    if (!cstart) goto oom;
    for (int e = 0; e < ne; e++) cstart[color[e] + 1]++;
    for (int c = 0; c < ncolors; c++) cstart[c + 1] += cstart[c];
    {
        int *fill = malloc((size_t)(ncolors ? ncolors : 1) * sizeof(int));
        if (!fill) goto oom;
        memcpy(fill, cstart, (size_t)ncolors * sizeof(int));
        for (int e = 0; e < ne; e++) order[fill[color[e]]++] = e;
        free(fill);
    }
    elem_dofs = malloc((size_t)(ne ? ne : 1) * 3 * n * sizeof(int));
    if (!elem_dofs) goto oom;
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < n; a++)
            for (int k = 0; k < 3; k++) elem_dofs[(size_t)3 * n * e + 3 * a + k] = eq[3 * m->conn[(size_t)n * e + a] + k];
    if (!csr_from_elements(&K, neq, ne, 3 * n, elem_dofs, err, errlen)) goto fail;
    free(elem_dofs);
    elem_dofs = NULL;
    f = calloc((size_t)(neq ? neq : 1), sizeof(double));
    fext_full = calloc((size_t)(nn ? nn : 1) * 3, sizeof(double));
    mindet = malloc((size_t)nthreads * sizeof(double));
    if (!f || !fext_full || !mindet) goto oom;
    for (int t = 0; t < nthreads; t++) mindet[t] = INFINITY;
    TAsm actx = {m, L, eq, order, D, &K, f, fext_full, false, -1, mindet};
    for (int c = 0; c < ncolors; c++) {
        int cnt = cstart[c + 1] - cstart[c];
        TAsm sub = actx;
        sub.order = order + cstart[c];
        if (opt.pool && c < 255 && cnt > 256) pool_for(opt.pool, cnt, 64, tasm_range, &sub);
        else tasm_range(&sub, 0, cnt, nthreads - 1);
        if (sub.failed) {
            snprintf(err, errlen, "element %d is inverted or degenerate", sub.bad_elem);
            goto fail;
        }
    }
    res->min_detJ = INFINITY;
    for (int t = 0; t < nthreads; t++) res->min_detJ = fmin(res->min_detJ, mindet[t]);
    if (L->nodal_force)
        for (int nd = 0; nd < nn; nd++)
            for (int k = 0; k < 3; k++) {
                double v = L->nodal_force[3 * (size_t)nd + k];
                if (v == 0) continue;
                fext_full[3 * (size_t)nd + k] += v;
                int q = eq[3 * nd + k];
                if (q >= 0) f[q] += v;
            }
    res->u = calloc((size_t)(nn ? nn : 1) * 3, sizeof(double));
    x = calloc((size_t)(neq ? neq : 1), sizeof(double));
    if (!res->u || !x) goto oom;
    if (neq > 0) {
        bool direct = opt.solver == SOLID_SOLVER_DIRECT;
        if (opt.solver == SOLID_SOLVER_AUTO) direct = neq <= 250000;
        if (direct) {
            /* node adjacency from the connectivity, for the nested-dissection ordering */
            adjp = calloc((size_t)nn + 1, sizeof(int64_t));
            perm = malloc((size_t)neq * sizeof(int));
            int *mark = malloc((size_t)(nn ? nn : 1) * sizeof(int)), *nstart = calloc((size_t)nn + 1, sizeof(int)), *nel = NULL;
            if (!adjp || !perm || !mark || !nstart) {
                free(mark), free(nstart);
                goto oom;
            }
            for (int e = 0; e < ne; e++)
                for (int a = 0; a < n; a++) nstart[m->conn[(size_t)n * e + a] + 1]++;
            for (int i = 0; i < nn; i++) nstart[i + 1] += nstart[i];
            nel = malloc((size_t)(nstart[nn] ? nstart[nn] : 1) * sizeof(int));
            int *fill = malloc((size_t)(nn ? nn : 1) * sizeof(int));
            if (!nel || !fill) {
                free(mark), free(nstart), free(nel), free(fill);
                goto oom;
            }
            memcpy(fill, nstart, (size_t)nn * sizeof(int));
            for (int e = 0; e < ne; e++)
                for (int a = 0; a < n; a++) nel[fill[m->conn[(size_t)n * e + a]]++] = e;
            free(fill);
            for (int pass = 0; pass < 2; pass++) {
                for (int i = 0; i < nn; i++) mark[i] = -1;
                for (int i = 0; i < nn; i++) {
                    int64_t cnt = 0, w = pass ? adjp[i] : 0;
                    for (int j = nstart[i]; j < nstart[i + 1]; j++)
                        for (int a = 0; a < n; a++) {
                            int o = m->conn[(size_t)n * nel[j] + a];
                            if (o == i || mark[o] == i) continue;
                            mark[o] = i;
                            if (pass) adj[w++] = o;
                            else cnt++;
                        }
                    if (!pass) adjp[i + 1] = cnt;
                }
                if (!pass) {
                    for (int i = 0; i < nn; i++) adjp[i + 1] += adjp[i];
                    adj = malloc((size_t)(adjp[nn] ? adjp[nn] : 1) * sizeof(int));
                    if (!adj) {
                        free(mark), free(nstart), free(nel);
                        goto oom;
                    }
                }
            }
            free(mark), free(nstart), free(nel);
            if (!nested_dissection_order(nn, m->xyz, adjp, adj, eq, neq, perm, err, errlen)) goto fail;
            int64_t fnnz = chol_symbolic_nnz(&K, perm, err, errlen);
            if (fnnz < 0 || fnnz > opt.max_factor_nnz) {
                if (opt.solver == SOLID_SOLVER_DIRECT) {
                    snprintf(err, errlen, "direct solver needs %lld factor entries (limit %lld); use the iterative solver", (long long)fnnz,
                             (long long)opt.max_factor_nnz);
                    goto fail;
                }
                direct = false;
            }
        }
        if (direct) {
            CholFactor F;
            int bad = -1;
            if (!chol_factor(&K, perm, &F, opt.progress, opt.ctx, &res->stats, &bad, err, errlen)) {
                for (int nd = 0; nd < nn && bad >= 0; nd++)
                    for (int k = 0; k < 3; k++)
                        if (eq[3 * nd + k] == bad) {
                            size_t l = strlen(err);
                            snprintf(err + l, errlen - l, " [node %d, component %c, at %.4g %.4g %.4g m]", nd, "xyz"[k], m->xyz[3 * nd],
                                     m->xyz[3 * nd + 1], m->xyz[3 * nd + 2]);
                        }
                goto fail;
            }
            chol_solve(&F, f, x);
            chol_free(&F);
            res->stats.converged = true;
        } else if (type == SOLID_ELEM_TET10) {
            if (!pmg_pcg(m, &K, eq, (const double (*)[6][6])D, f, x, &opt, &res->stats, err, errlen)) goto fail;
        } else {
            block_of = malloc((size_t)neq * sizeof(int));
            if (!block_of) goto oom;
            int nb = 0;
            for (int nd = 0; nd < nn; nd++) {
                bool anyq = false;
                for (int k = 0; k < 3; k++)
                    if (eq[3 * nd + k] >= 0) block_of[eq[3 * nd + k]] = nb, anyq = true;
                if (anyq) nb++;
            }
            PcgOptions po = {PRECOND_BLOCK_JACOBI, block_of, opt.pcg_tol, opt.pcg_max_iter, opt.pool, opt.progress, opt.ctx};
            if (!pcg_solve(&K, f, x, &po, &res->stats, err, errlen)) goto fail;
        }
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
    csr_free(&K);
    res->gp_strain = malloc((size_t)(ne ? ne : 1) * 6 * ng * sizeof(double));
    res->gp_stress = malloc((size_t)(ne ? ne : 1) * 6 * ng * sizeof(double));
    res->node_stress = calloc((size_t)(nn ? nn : 1) * 6, sizeof(double));
    res->reaction = calloc((size_t)(nn ? nn : 1) * 3, sizeof(double));
    fint = calloc((size_t)(nn ? nn : 1) * 3, sizeof(double));
    nsum = calloc((size_t)(nn ? nn : 1) * 6, sizeof(double));
    ncount = calloc((size_t)(nn ? nn : 1), sizeof(double));
    energy = calloc((size_t)nthreads, sizeof(double));
    if (!res->gp_strain || !res->gp_stress || !res->node_stress || !res->reaction || !fint || !nsum || !ncount || !energy) goto oom;
    TRec rctx = {m, L, order, D, res, fint, nsum, ncount, energy};
    for (int c = 0; c < ncolors; c++) {
        int cnt = cstart[c + 1] - cstart[c];
        TRec sub = rctx;
        sub.order = order + cstart[c];
        if (opt.pool && c < 255 && cnt > 256) pool_for(opt.pool, cnt, 64, trec_range, &sub);
        else trec_range(&sub, 0, cnt, nthreads - 1);
    }
    for (int t = 0; t < nthreads; t++) res->strain_energy += energy[t];
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
    double denom = sqrt(aload) + sqrt(rsum2) + sqrt(fnorm);
    res->equilibrium_error = denom > 0 ? sqrt(rfree) / denom : sqrt(rfree);
    ok = true;
    goto done;
oom:
    snprintf(err, errlen, "out of memory in the structural solver (%d nodes, %d tetrahedra)", nn, ne);
fail:
    solid_result_free(res);
done:
    free(eq), free(color), free(order), free(cstart), free(elem_dofs), free(perm), free(block_of), free(adj), free(used), free(node_colors), free(D);
    free(f), free(fext_full), free(x), free(fint), free(nsum), free(ncount), free(energy), free(mindet), free(adjp);
    csr_free(&K);
    return ok;
}
