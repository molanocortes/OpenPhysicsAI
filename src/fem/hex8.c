/* hex8.c - 8-node hexahedron: shape functions, stiffness (full and incompatible modes), loads, recovery */
#include "hex8.h"
#include "dense.h"

#include <math.h>
#include <string.h>

const double HEX8_XI[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
const int HEX8_FACE_NODES[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};

static const double GP = 0.5773502691896257645; /* 1/sqrt(3) */

void hex8_shape(double xi, double eta, double zeta, double N[8], double dN[8][3]) {
    for (int a = 0; a < 8; a++) {
        double sx = HEX8_XI[a][0], sy = HEX8_XI[a][1], sz = HEX8_XI[a][2];
        double fx = 1 + sx * xi, fy = 1 + sy * eta, fz = 1 + sz * zeta;
        if (N) N[a] = 0.125 * fx * fy * fz;
        if (dN) {
            dN[a][0] = 0.125 * sx * fy * fz;
            dN[a][1] = 0.125 * sy * fx * fz;
            dN[a][2] = 0.125 * sz * fx * fy;
        }
    }
}

double hex8_jacobian(const double X[8][3], const double dN[8][3], double J[3][3], double dNdx[8][3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int a = 0; a < 8; a++) s += dN[a][j] * X[a][i];
            J[i][j] = s;
        }
    double m[9] = {J[0][0], J[0][1], J[0][2], J[1][0], J[1][1], J[1][2], J[2][0], J[2][1], J[2][2]}, inv[9], det;
    if (!inv3(m, inv, &det)) {
        if (dNdx) memset(dNdx, 0, sizeof(double) * 24);
        return det;
    }
    if (dNdx)
        for (int a = 0; a < 8; a++)
            for (int i = 0; i < 3; i++) dNdx[a][i] = dN[a][0] * inv[0 * 3 + i] + dN[a][1] * inv[1 * 3 + i] + dN[a][2] * inv[2 * 3 + i];
    return det;
}

void isotropic_D(double E, double nu, double D[6][6]) {
    memset(D, 0, 36 * sizeof(double));
    double lam = E * nu / ((1 + nu) * (1 - 2 * nu)), mu = E / (2 * (1 + nu));
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) D[i][j] = lam + (i == j ? 2 * mu : 0);
    D[3][3] = D[4][4] = D[5][5] = mu;
}

/* strain-displacement rows for a node with physical derivatives d (dx, dy, dz): 6 x 3 block */
static void b_block(const double d[3], double b[6][3]) {
    memset(b, 0, 18 * sizeof(double));
    b[0][0] = d[0];
    b[1][1] = d[1];
    b[2][2] = d[2];
    b[3][0] = d[1], b[3][1] = d[0];
    b[4][1] = d[2], b[4][2] = d[1];
    b[5][0] = d[2], b[5][2] = d[0];
}

typedef struct {
    double B[6][24];
    double G[6][9]; /* incompatible modes */
    double w;       /* detJ * weight */
} GaussData;

static bool gauss_data(const double X[8][3], Hex8Formulation f, GaussData gd[8], double *min_detJ) {
    double dN0[8][3], J0[3][3], J0inv[9], detJ0 = 0;
    if (f == HEX8_INCOMPATIBLE) {
        hex8_shape(0, 0, 0, NULL, dN0);
        detJ0 = hex8_jacobian(X, dN0, J0, NULL);
        double m[9] = {J0[0][0], J0[0][1], J0[0][2], J0[1][0], J0[1][1], J0[1][2], J0[2][0], J0[2][1], J0[2][2]};
        if (!inv3(m, J0inv, NULL)) return false;
    }
    double mind = INFINITY;
    for (int g = 0; g < 8; g++) {
        double xi = HEX8_XI[g][0] * GP, eta = HEX8_XI[g][1] * GP, zeta = HEX8_XI[g][2] * GP;
        double dN[8][3], J[3][3], dNdx[8][3];
        hex8_shape(xi, eta, zeta, NULL, dN);
        double detJ = hex8_jacobian(X, dN, J, dNdx);
        if (detJ < mind) mind = detJ;
        if (!(detJ > 0)) {
            if (min_detJ) *min_detJ = detJ;
            return false;
        }
        memset(gd[g].B, 0, sizeof gd[g].B);
        for (int a = 0; a < 8; a++) {
            double b[6][3];
            b_block(dNdx[a], b);
            for (int r = 0; r < 6; r++)
                for (int c = 0; c < 3; c++) gd[g].B[r][3 * a + c] = b[r][c];
        }
        gd[g].w = detJ; /* Gauss weights are 1 */
        if (f == HEX8_INCOMPATIBLE) {
            /* bubble modes P_m = 1 - xi_m^2: natural gradient (-2 xi, 0, 0), (0, -2 eta, 0), (0, 0, -2 zeta),
             * mapped with the centre Jacobian and scaled by detJ0/detJ (Taylor) */
            double nat[3] = {xi, eta, zeta};
            memset(gd[g].G, 0, sizeof gd[g].G);
            for (int mode = 0; mode < 3; mode++) {
                double dnat[3] = {0, 0, 0};
                dnat[mode] = -2 * nat[mode];
                double d[3];
                for (int i = 0; i < 3; i++) d[i] = (detJ0 / detJ) * (dnat[0] * J0inv[0 * 3 + i] + dnat[1] * J0inv[1 * 3 + i] + dnat[2] * J0inv[2 * 3 + i]);
                double b[6][3];
                b_block(d, b);
                for (int r = 0; r < 6; r++)
                    for (int c = 0; c < 3; c++) gd[g].G[r][3 * mode + c] = b[r][c];
            }
        }
    }
    if (min_detJ) *min_detJ = mind;
    return true;
}

/* element matrices: Kuu (24x24), Kua (24x9), Kaa (9x9) */
static void integrate(const GaussData gd[8], const double D[6][6], Hex8Formulation f, double *Kuu, double *Kua, double *Kaa) {
    memset(Kuu, 0, 576 * sizeof(double));
    if (f == HEX8_INCOMPATIBLE) {
        memset(Kua, 0, 216 * sizeof(double));
        memset(Kaa, 0, 81 * sizeof(double));
    }
    for (int g = 0; g < 8; g++) {
        double DB[6][24], DG[6][9];
        for (int r = 0; r < 6; r++)
            for (int c = 0; c < 24; c++) {
                double s = 0;
                for (int k = 0; k < 6; k++) s += D[r][k] * gd[g].B[k][c];
                DB[r][c] = s;
            }
        double w = gd[g].w;
        for (int i = 0; i < 24; i++)
            for (int j = i; j < 24; j++) {
                double s = 0;
                for (int k = 0; k < 6; k++) s += gd[g].B[k][i] * DB[k][j];
                Kuu[i * 24 + j] += s * w;
            }
        if (f == HEX8_INCOMPATIBLE) {
            for (int r = 0; r < 6; r++)
                for (int c = 0; c < 9; c++) {
                    double s = 0;
                    for (int k = 0; k < 6; k++) s += D[r][k] * gd[g].G[k][c];
                    DG[r][c] = s;
                }
            for (int i = 0; i < 24; i++)
                for (int j = 0; j < 9; j++) {
                    double s = 0;
                    for (int k = 0; k < 6; k++) s += gd[g].B[k][i] * DG[k][j];
                    Kua[i * 9 + j] += s * w;
                }
            for (int i = 0; i < 9; i++)
                for (int j = 0; j < 9; j++) {
                    double s = 0;
                    for (int k = 0; k < 6; k++) s += gd[g].G[k][i] * DG[k][j];
                    Kaa[i * 9 + j] += s * w;
                }
        }
    }
    for (int i = 0; i < 24; i++)
        for (int j = 0; j < i; j++) Kuu[i * 24 + j] = Kuu[j * 24 + i];
}

bool hex8_stiffness(const double X[8][3], const double D[6][6], Hex8Formulation f, double Ke[24 * 24], double *min_detJ) {
    GaussData gd[8];
    if (!gauss_data(X, f, gd, min_detJ)) return false;
    double Kua[216], Kaa[81];
    integrate(gd, D, f, Ke, Kua, Kaa);
    if (f == HEX8_INCOMPATIBLE) {
        if (!dense_spd_inverse(Kaa, 9)) return false;
        /* Ke -= Kua Kaa^-1 Kau */
        double T[216]; /* Kua Kaa^-1 */
        for (int i = 0; i < 24; i++)
            for (int j = 0; j < 9; j++) {
                double s = 0;
                for (int k = 0; k < 9; k++) s += Kua[i * 9 + k] * Kaa[k * 9 + j];
                T[i * 9 + j] = s;
            }
        for (int i = 0; i < 24; i++)
            for (int j = 0; j < 24; j++) {
                double s = 0;
                for (int k = 0; k < 9; k++) s += T[i * 9 + k] * Kua[j * 9 + k];
                Ke[i * 24 + j] -= s;
            }
        /* restore exact symmetry after round-off */
        for (int i = 0; i < 24; i++)
            for (int j = 0; j < i; j++) Ke[i * 24 + j] = Ke[j * 24 + i] = 0.5 * (Ke[i * 24 + j] + Ke[j * 24 + i]);
    }
    return true;
}

void hex8_gauss_strain_stress(const double X[8][3], const double D[6][6], Hex8Formulation f, const double ue[24], const double *eps0,
                              double eps[8][6], double sig[8][6]) {
    GaussData gd[8];
    double dummy;
    if (!gauss_data(X, f, gd, &dummy)) {
        memset(eps, 0, 48 * sizeof(double));
        memset(sig, 0, 48 * sizeof(double));
        return;
    }
    double alpha[9] = {0};
    if (f == HEX8_INCOMPATIBLE) {
        double Kuu[576], Kua[216], Kaa[81];
        integrate(gd, D, f, Kuu, Kua, Kaa);
        /* internal modes minimise the element energy: Kaa alpha = -(Kau u - fa0), with fa0 = integral G^T D eps0 */
        double rhs[9];
        for (int i = 0; i < 9; i++) {
            double s = 0;
            for (int j = 0; j < 24; j++) s += Kua[j * 9 + i] * ue[j];
            rhs[i] = -s;
        }
        if (eps0) {
            for (int g = 0; g < 8; g++) {
                double De0[6];
                for (int r = 0; r < 6; r++) {
                    double s = 0;
                    for (int k = 0; k < 6; k++) s += D[r][k] * eps0[k];
                    De0[r] = s;
                }
                for (int i = 0; i < 9; i++) {
                    double s = 0;
                    for (int k = 0; k < 6; k++) s += gd[g].G[k][i] * De0[k];
                    rhs[i] += s * gd[g].w;
                }
            }
        }
        if (dense_solve(Kaa, rhs, 9)) memcpy(alpha, rhs, sizeof alpha);
    }
    for (int g = 0; g < 8; g++) {
        for (int r = 0; r < 6; r++) {
            double s = 0;
            for (int c = 0; c < 24; c++) s += gd[g].B[r][c] * ue[c];
            if (f == HEX8_INCOMPATIBLE)
                for (int c = 0; c < 9; c++) s += gd[g].G[r][c] * alpha[c];
            eps[g][r] = s;
        }
        for (int r = 0; r < 6; r++) {
            double s = 0;
            for (int k = 0; k < 6; k++) s += D[r][k] * (eps[g][k] - (eps0 ? eps0[k] : 0));
            sig[g][r] = s;
        }
    }
}

void hex8_initial_strain_load(const double X[8][3], const double D[6][6], Hex8Formulation f, const double eps0[6], double fe[24]) {
    GaussData gd[8];
    double dummy;
    memset(fe, 0, 24 * sizeof(double));
    if (!gauss_data(X, f, gd, &dummy)) return;
    double De0[6];
    for (int r = 0; r < 6; r++) {
        double s = 0;
        for (int k = 0; k < 6; k++) s += D[r][k] * eps0[k];
        De0[r] = s;
    }
    double fa[9] = {0};
    for (int g = 0; g < 8; g++) {
        for (int i = 0; i < 24; i++) {
            double s = 0;
            for (int k = 0; k < 6; k++) s += gd[g].B[k][i] * De0[k];
            fe[i] += s * gd[g].w;
        }
        if (f == HEX8_INCOMPATIBLE)
            for (int i = 0; i < 9; i++) {
                double s = 0;
                for (int k = 0; k < 6; k++) s += gd[g].G[k][i] * De0[k];
                fa[i] += s * gd[g].w;
            }
    }
    if (f == HEX8_INCOMPATIBLE) {
        /* condensed load: f_u - Kua Kaa^-1 f_a */
        double Kuu[576], Kua[216], Kaa[81];
        integrate(gd, D, f, Kuu, Kua, Kaa);
        if (dense_solve(Kaa, fa, 9))
            for (int i = 0; i < 24; i++) {
                double s = 0;
                for (int k = 0; k < 9; k++) s += Kua[i * 9 + k] * fa[k];
                fe[i] -= s;
            }
    }
}

void hex8_body_load(const double X[8][3], const double b[3], double fe[24]) {
    memset(fe, 0, 24 * sizeof(double));
    for (int g = 0; g < 8; g++) {
        double N[8], dN[8][3], J[3][3];
        hex8_shape(HEX8_XI[g][0] * GP, HEX8_XI[g][1] * GP, HEX8_XI[g][2] * GP, N, dN);
        double detJ = hex8_jacobian(X, dN, J, NULL);
        for (int a = 0; a < 8; a++)
            for (int c = 0; c < 3; c++) fe[3 * a + c] += N[a] * b[c] * detJ;
    }
}

void hex8_face_load(const double X[8][3], int face, const double t[3], double fe[24], double *area, double normal[3]) {
    memset(fe, 0, 24 * sizeof(double));
    const int *fn = HEX8_FACE_NODES[face];
    static const double S[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    double A = 0, nsum[3] = {0, 0, 0};
    for (int gs = 0; gs < 2; gs++)
        for (int gt = 0; gt < 2; gt++) {
            double s = (gs ? 1 : -1) * GP, tt = (gt ? 1 : -1) * GP;
            double Nq[4], ds[3] = {0, 0, 0}, dt[3] = {0, 0, 0};
            for (int q = 0; q < 4; q++) {
                Nq[q] = 0.25 * (1 + S[q][0] * s) * (1 + S[q][1] * tt);
                double dNs = 0.25 * S[q][0] * (1 + S[q][1] * tt), dNt = 0.25 * S[q][1] * (1 + S[q][0] * s);
                for (int k = 0; k < 3; k++) {
                    ds[k] += dNs * X[fn[q]][k];
                    dt[k] += dNt * X[fn[q]][k];
                }
            }
            double n[3] = {ds[1] * dt[2] - ds[2] * dt[1], ds[2] * dt[0] - ds[0] * dt[2], ds[0] * dt[1] - ds[1] * dt[0]};
            double jac = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            A += jac;
            for (int k = 0; k < 3; k++) nsum[k] += n[k];
            for (int q = 0; q < 4; q++)
                for (int c = 0; c < 3; c++) fe[3 * fn[q] + c] += Nq[q] * t[c] * jac;
        }
    if (area) *area = A;
    if (normal) {
        double l = sqrt(nsum[0] * nsum[0] + nsum[1] * nsum[1] + nsum[2] * nsum[2]);
        for (int k = 0; k < 3; k++) normal[k] = l > 0 ? nsum[k] / l : 0;
    }
}

double hex8_volume(const double X[8][3]) {
    double V = 0;
    for (int g = 0; g < 8; g++) {
        double dN[8][3], J[3][3];
        hex8_shape(HEX8_XI[g][0] * GP, HEX8_XI[g][1] * GP, HEX8_XI[g][2] * GP, NULL, dN);
        V += hex8_jacobian(X, dN, J, NULL);
    }
    return V;
}

void hex8_gauss_detj(const double X[8][3], double detj[8]) {
    for (int g = 0; g < 8; g++) {
        double dN[8][3], J[3][3];
        hex8_shape(HEX8_XI[g][0] * GP, HEX8_XI[g][1] * GP, HEX8_XI[g][2] * GP, NULL, dN);
        detj[g] = hex8_jacobian(X, dN, J, NULL);
    }
}

void hex8_extrapolate(const double *gp, int ncomp, double *nodal) {
    /* the trilinear interpolant through the Gauss point values, evaluated at the nodes (natural coordinate
     * sqrt(3) in the Gauss-point frame) */
    static double E[8][8];
    static int ready = 0;
    if (!ready) {
        for (int a = 0; a < 8; a++) {
            double N[8];
            double s = 1.0 / GP;
            hex8_shape(HEX8_XI[a][0] * s, HEX8_XI[a][1] * s, HEX8_XI[a][2] * s, N, NULL);
            for (int g = 0; g < 8; g++) E[a][g] = N[g];
        }
        ready = 1;
    }
    for (int a = 0; a < 8; a++)
        for (int c = 0; c < ncomp; c++) {
            double v = 0;
            for (int g = 0; g < 8; g++) v += E[a][g] * gp[g * ncomp + c];
            nodal[a * ncomp + c] = v;
        }
}
