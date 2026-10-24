/* mbody.c - see mbody.h */
#include "mbody.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../fem/hex8.h"

/* the element formulation; incompatible modes unless a test asks for the plain element */
static Hex8Formulation formulation = HEX8_INCOMPATIBLE;
void mbody_set_formulation(int full) { formulation = full ? HEX8_FULL : HEX8_INCOMPATIBLE; }

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static void element_X(const MBody *b, int e, double X[8][3]) {
    for (int a = 0; a < 8; a++)
        for (int d = 0; d < 3; d++) X[a][d] = b->xyz[3 * b->conn[8 * e + a] + d];
}

bool mbody_create(MBody *b, int nnodes, const double *xyz, int nelems, const int *conn, double E, double nu, const unsigned char *held,
                  char *err, size_t errlen) {
    return mbody_create_springs(b, nnodes, xyz, nelems, conn, E, nu, held, 0, NULL, NULL, err, errlen);
}

bool mbody_create_springs(MBody *b, int nnodes, const double *xyz, int nelems, const int *conn, double E, double nu, const unsigned char *held,
                          int nspring, const int *spring_node, const double *spring_k, char *err, size_t errlen) {
    memset(b, 0, sizeof *b);
    b->nnodes = nnodes, b->nelems = nelems, b->xyz = xyz, b->conn = conn, b->E = E, b->nu = nu;
    if (E <= 0 || nu <= -1 || nu >= 0.5) return snprintf(err, errlen, "a part needs E > 0 and -1 < nu < 0.5"), false;
    double t0 = now();
    b->eq = malloc((size_t)nnodes * 3 * sizeof(int));
    int *edofs = malloc((size_t)nelems * 24 * sizeof(int));
    double *Ke = malloc(24 * 24 * sizeof(double));
    if (!b->eq || !edofs || !Ke) {
        free(edofs), free(Ke), mbody_free(b);
        return snprintf(err, errlen, "out of memory"), false;
    }
    for (int i = 0; i < nnodes; i++)
        for (int d = 0; d < 3; d++) b->eq[3 * i + d] = held && held[i] ? -1 : b->neq++;
    for (int e = 0; e < nelems; e++)
        for (int a = 0; a < 8; a++)
            for (int d = 0; d < 3; d++) edofs[24 * e + 3 * a + d] = b->eq[3 * conn[8 * e + a] + d];
    CsrMatrix K;
    if (!csr_from_elements(&K, b->neq, nelems, 24, edofs, err, errlen)) {
        free(edofs), free(Ke), mbody_free(b);
        return false;
    }
    double D[6][6];
    isotropic_D(E, nu, D);
    for (int e = 0; e < nelems; e++) {
        double X[8][3], minJ;
        element_X(b, e, X);
        if (!hex8_stiffness(X, D, formulation, Ke, &minJ)) {
            snprintf(err, errlen, "element %d is inverted or degenerate (det J %.3g)", e, minJ);
            csr_free(&K), free(edofs), free(Ke), mbody_free(b);
            return false;
        }
        csr_add_element(&K, 24, &edofs[24 * e], Ke);
    }
    for (int k = 0; k < nspring; k++) { /* a node's own 3 x 3 block is in the pattern: its element couples it */
        int nd = spring_node[k], dofs[3] = {b->eq[3 * nd], b->eq[3 * nd + 1], b->eq[3 * nd + 2]};
        csr_add_element(&K, 3, dofs, &spring_k[9 * k]);
    }
    free(edofs), free(Ke);
    b->assemble_seconds = now() - t0;
    t0 = now();
    b->K = mfactor_create(&K, nnodes, xyz, b->eq, err, errlen);
    csr_free(&K);
    b->factor_seconds = now() - t0;
    if (!b->K) {
        mbody_free(b);
        return false;
    }
    return true;
}

void mbody_free(MBody *b) {
    mfactor_free(b->K);
    free(b->eq), free(b->zone_node), free(b->zone_dir), free(b->G), free(b->zone_of);
    memset(b, 0, sizeof *b);
}

bool mbody_zone(MBody *b, int n, const int *nodes, const double *dirs, char *err, size_t errlen) {
    free(b->zone_node), free(b->zone_dir), free(b->G), free(b->zone_of);
    b->zone_node = NULL, b->zone_dir = NULL, b->G = NULL, b->zone_of = NULL, b->nzone = 0, b->zone_cyc = b->zone_per = 0;
    double t0 = now();
    int nb = 48; /* right-hand sides per block solve */
    b->zone_node = malloc((size_t)n * sizeof(int));
    b->zone_dir = malloc((size_t)n * 3 * sizeof(double));
    b->G = malloc((size_t)n * n * sizeof(double));
    b->zone_of = malloc((size_t)b->nnodes * sizeof(int));
    double *X = malloc((size_t)b->neq * nb * sizeof(double));
    if (!b->zone_node || !b->zone_dir || !b->G || !b->zone_of || !X) {
        free(X);
        return snprintf(err, errlen, "out of memory for a zone of %d nodes (%.0f MB)", n, (double)n * n * 8 / 1e6), false;
    }
    for (int i = 0; i < b->nnodes; i++) b->zone_of[i] = -1;
    for (int i = 0; i < n; i++) {
        b->zone_node[i] = nodes[i];
        double L = sqrt(dirs[3 * i] * dirs[3 * i] + dirs[3 * i + 1] * dirs[3 * i + 1] + dirs[3 * i + 2] * dirs[3 * i + 2]);
        for (int d = 0; d < 3; d++) b->zone_dir[3 * i + d] = dirs[3 * i + d] / L;
        b->zone_of[nodes[i]] = i;
    }
    b->nzone = n;
    for (int c0 = 0; c0 < n; c0 += nb) {
        int k = c0 + nb <= n ? nb : n - c0;
        memset(X, 0, (size_t)b->neq * k * sizeof(double));
        for (int c = 0; c < k; c++) {
            int nd = nodes[c0 + c];
            for (int d = 0; d < 3; d++) {
                int q = b->eq[3 * nd + d];
                if (q >= 0) X[(size_t)c * b->neq + q] = b->zone_dir[3 * (c0 + c) + d];
            }
        }
        if (!mfactor_solve(b->K, X, k)) {
            free(X);
            return snprintf(err, errlen, "the zone's solves failed"), false;
        }
        for (int c = 0; c < k; c++) {
            const double *x = X + (size_t)c * b->neq;
            for (int i = 0; i < n; i++) {
                double v = 0;
                for (int d = 0; d < 3; d++) {
                    int q = b->eq[3 * nodes[i] + d];
                    if (q >= 0) v += b->zone_dir[3 * i + d] * x[q];
                }
                b->G[(size_t)i * n + c0 + c] = v;
            }
        }
    }
    free(X);
    for (int i = 0; i < n; i++) /* round-off makes it slightly unsymmetric */
        for (int j = i + 1; j < n; j++) {
            double v = 0.5 * (b->G[(size_t)i * n + j] + b->G[(size_t)j * n + i]);
            b->G[(size_t)i * n + j] = b->G[(size_t)j * n + i] = v;
        }
    b->zone_seconds = now() - t0;
    return true;
}

bool mbody_zone_cyclic(MBody *b, int ncyc, int nper, const int *nodes, const double *dirs, char *err, size_t errlen) {
    free(b->zone_node), free(b->zone_dir), free(b->G), free(b->zone_of);
    b->zone_node = NULL, b->zone_dir = NULL, b->G = NULL, b->zone_of = NULL, b->nzone = 0, b->zone_cyc = b->zone_per = 0;
    if (ncyc < 1 || nper < 1) return snprintf(err, errlen, "a cyclic zone needs sectors and nodes"), false;
    double t0 = now();
    int n = ncyc * nper, nb = 48;
    /* the symmetry the blocks rely on: node (s, a) is node (0, a) turned by s 2 pi / ncyc, its direction too, and held
     * alike */
    double size = 0;
    for (int i = 0; i < b->nnodes; i++) size = fmax(size, sqrt(b->xyz[3 * i] * b->xyz[3 * i] + b->xyz[3 * i + 1] * b->xyz[3 * i + 1]));
    for (int s = 1; s < ncyc; s++) {
        double c = cos(2 * M_PI * s / ncyc), sn = sin(2 * M_PI * s / ncyc);
        for (int a = 0; a < nper; a++) {
            int i0 = nodes[a], i1 = nodes[s * nper + a];
            const double *p0 = &b->xyz[3 * i0], *p1 = &b->xyz[3 * i1], *d0 = &dirs[3 * a], *d1 = &dirs[3 * (s * nper + a)];
            double L0 = sqrt(d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2]), L1 = sqrt(d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2]);
            double ex = c * p0[0] - sn * p0[1] - p1[0], ey = sn * p0[0] + c * p0[1] - p1[1], ez = p0[2] - p1[2];
            double fx = (c * d0[0] - sn * d0[1]) / L0 - d1[0] / L1, fy = (sn * d0[0] + c * d0[1]) / L0 - d1[1] / L1, fz = d0[2] / L0 - d1[2] / L1;
            if (sqrt(ex * ex + ey * ey + ez * ez) > 1e-9 * size || sqrt(fx * fx + fy * fy + fz * fz) > 1e-9 || (b->eq[3 * i0] < 0) != (b->eq[3 * i1] < 0))
                return snprintf(err, errlen, "the zone is not cyclic: node %d of sector %d is not the image of sector 0's", a, s), false;
        }
    }
    b->zone_node = malloc((size_t)n * sizeof(int));
    b->zone_dir = malloc((size_t)n * 3 * sizeof(double));
    b->G = malloc((size_t)ncyc * nper * nper * sizeof(double));
    b->zone_of = malloc((size_t)b->nnodes * sizeof(int));
    double *X = malloc((size_t)b->neq * nb * sizeof(double)), *T = malloc((size_t)ncyc * nper * nper * sizeof(double));
    if (!b->zone_node || !b->zone_dir || !b->G || !b->zone_of || !X || !T) {
        free(X), free(T);
        return snprintf(err, errlen, "out of memory for a cyclic zone of %d x %d nodes", ncyc, nper), false;
    }
    for (int i = 0; i < b->nnodes; i++) b->zone_of[i] = -1;
    for (int i = 0; i < n; i++) {
        b->zone_node[i] = nodes[i];
        double L = sqrt(dirs[3 * i] * dirs[3 * i] + dirs[3 * i + 1] * dirs[3 * i + 1] + dirs[3 * i + 2] * dirs[3 * i + 2]);
        for (int d = 0; d < 3; d++) b->zone_dir[3 * i + d] = dirs[3 * i + d] / L;
        b->zone_of[nodes[i]] = i;
    }
    b->nzone = n, b->zone_cyc = ncyc, b->zone_per = nper;
    /* unit loads on sector 0's nodes; the response at node a' of sector t is block (0 - t) mod ncyc */
    for (int c0 = 0; c0 < nper; c0 += nb) {
        int k = c0 + nb <= nper ? nb : nper - c0;
        memset(X, 0, (size_t)b->neq * k * sizeof(double));
        for (int c = 0; c < k; c++) {
            int nd = nodes[c0 + c];
            for (int d = 0; d < 3; d++) {
                int q = b->eq[3 * nd + d];
                if (q >= 0) X[(size_t)c * b->neq + q] = b->zone_dir[3 * (c0 + c) + d];
            }
        }
        if (!mfactor_solve(b->K, X, k)) {
            free(X), free(T);
            return snprintf(err, errlen, "the zone's solves failed"), false;
        }
        for (int c = 0; c < k; c++) {
            const double *x = X + (size_t)c * b->neq;
            for (int i = 0; i < n; i++) {
                double v = 0;
                for (int d = 0; d < 3; d++) {
                    int q = b->eq[3 * nodes[i] + d];
                    if (q >= 0) v += b->zone_dir[3 * i + d] * x[q];
                }
                int t = i / nper, blk = (ncyc - t) % ncyc;
                T[((size_t)blk * nper + (i - t * nper)) * nper + c0 + c] = v;
            }
        }
    }
    free(X);
    /* G(i, k) = G(k, i): block d entry (a, c) and block -d entry (c, a) are one value; round-off splits them */
    for (int d = 0; d < ncyc; d++) {
        int e = (ncyc - d) % ncyc;
        for (int a = 0; a < nper; a++)
            for (int c = 0; c < nper; c++)
                b->G[((size_t)d * nper + a) * nper + c] = 0.5 * (T[((size_t)d * nper + a) * nper + c] + T[((size_t)e * nper + c) * nper + a]);
    }
    free(T);
    b->zone_seconds = now() - t0;
    return true;
}

bool mbody_solve(MBody *b, const double *f, double *u) {
    double *x = calloc((size_t)(b->neq ? b->neq : 1), sizeof(double));
    if (!x) return false;
    for (int i = 0; i < b->nnodes; i++)
        for (int d = 0; d < 3; d++)
            if (b->eq[3 * i + d] >= 0) x[b->eq[3 * i + d]] = f[3 * i + d];
    bool ok = mfactor_solve(b->K, x, 1);
    for (int i = 0; i < b->nnodes; i++)
        for (int d = 0; d < 3; d++) u[3 * i + d] = b->eq[3 * i + d] >= 0 ? x[b->eq[3 * i + d]] : 0;
    free(x);
    return ok;
}

void mbody_fields(const MBody *b, const double *u, double *node_strain, double *node_stress, float *elem_mises) {
    double D[6][6];
    isotropic_D(b->E, b->nu, D);
    int *count = (node_strain || node_stress) ? calloc((size_t)b->nnodes, sizeof(int)) : NULL;
    if (node_strain) memset(node_strain, 0, (size_t)b->nnodes * 6 * sizeof(double));
    if (node_stress) memset(node_stress, 0, (size_t)b->nnodes * 6 * sizeof(double));
    for (int e = 0; e < b->nelems; e++) {
        double X[8][3], ue[24], eps[8][6], sig[8][6];
        element_X(b, e, X);
        for (int a = 0; a < 8; a++)
            for (int d = 0; d < 3; d++) ue[3 * a + d] = u[3 * b->conn[8 * e + a] + d];
        hex8_gauss_strain_stress(X, D, formulation, ue, NULL, eps, sig);
        if (elem_mises) {
            double vm = 0;
            for (int g = 0; g < 8; g++) {
                const double *s = sig[g];
                double v = 0.5 * ((s[0] - s[1]) * (s[0] - s[1]) + (s[1] - s[2]) * (s[1] - s[2]) + (s[2] - s[0]) * (s[2] - s[0])) +
                           3 * (s[3] * s[3] + s[4] * s[4] + s[5] * s[5]);
                vm += sqrt(v) / 8;
            }
            elem_mises[e] = (float)vm;
        }
        if (count) {
            double ne[8][6], ns[8][6];
            hex8_extrapolate(&eps[0][0], 6, &ne[0][0]);
            hex8_extrapolate(&sig[0][0], 6, &ns[0][0]);
            for (int a = 0; a < 8; a++) {
                int nd = b->conn[8 * e + a];
                count[nd]++;
                for (int k = 0; k < 6; k++) {
                    if (node_strain) node_strain[6 * nd + k] += ne[a][k];
                    if (node_stress) node_stress[6 * nd + k] += ns[a][k];
                }
            }
        }
    }
    if (count) {
        for (int i = 0; i < b->nnodes; i++)
            if (count[i])
                for (int k = 0; k < 6; k++) {
                    if (node_strain) node_strain[6 * i + k] /= count[i];
                    if (node_stress) node_stress[6 * i + k] /= count[i];
                }
        free(count);
    }
}

void mbody_strain_at(const MBody *b, const double *node_strain, int elem, const double xi[3], double eps[6]) {
    double N[8], dN[8][3];
    hex8_shape(xi[0], xi[1], xi[2], N, dN);
    for (int k = 0; k < 6; k++) eps[k] = 0;
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 6; k++) eps[k] += N[a] * node_strain[6 * b->conn[8 * elem + a] + k];
}

void mbody_node_mises(const MBody *b, const double *node_strain, float *node_mises) {
    double lam = b->E * b->nu / ((1 + b->nu) * (1 - 2 * b->nu)), mu = b->E / (2 * (1 + b->nu));
    for (int i = 0; i < b->nnodes; i++) {
        const double *e = &node_strain[6 * i];
        double tr = e[0] + e[1] + e[2], s0 = lam * tr + 2 * mu * e[0], s1 = lam * tr + 2 * mu * e[1], s2 = lam * tr + 2 * mu * e[2];
        double t0 = mu * e[3], t1 = mu * e[4], t2 = mu * e[5]; /* engineering shear strains */
        double v = 0.5 * ((s0 - s1) * (s0 - s1) + (s1 - s2) * (s1 - s2) + (s2 - s0) * (s2 - s0)) + 3 * (t0 * t0 + t1 * t1 + t2 * t2);
        node_mises[i] = (float)sqrt(v);
    }
}
