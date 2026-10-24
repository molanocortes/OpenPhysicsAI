/* hex8_nl.c - the hex8 at large deformation (hex8_nl.h, docs/contracts/dynamics.md) */
#include "hex8_nl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hex8.h"

/* ---- small 3x3 helpers (row-major) ---- */

static void m3_mul(const double A[9], const double B[9], double C[9]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int k = 0; k < 3; k++) s += A[3 * i + k] * B[3 * k + j];
            C[3 * i + j] = s;
        }
}

static double m3_det(const double A[9]) {
    return A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
}

static bool m3_inv(const double A[9], double B[9]) {
    double d = m3_det(A);
    if (!(fabs(d) > 0)) return false;
    double id = 1.0 / d;
    B[0] = (A[4] * A[8] - A[5] * A[7]) * id, B[1] = (A[2] * A[7] - A[1] * A[8]) * id, B[2] = (A[1] * A[5] - A[2] * A[4]) * id;
    B[3] = (A[5] * A[6] - A[3] * A[8]) * id, B[4] = (A[0] * A[8] - A[2] * A[6]) * id, B[5] = (A[2] * A[3] - A[0] * A[5]) * id;
    B[6] = (A[3] * A[7] - A[4] * A[6]) * id, B[7] = (A[1] * A[6] - A[0] * A[7]) * id, B[8] = (A[0] * A[4] - A[1] * A[3]) * id;
    return true;
}

/* eigenvalues and eigenvectors of a symmetric 3x3 by cyclic Jacobi rotations; v holds the eigenvectors as columns */
static void sym3_eigen(const double A[9], double lambda[3], double v[9]) {
    double a[9];
    memcpy(a, A, sizeof a);
    memset(v, 0, 9 * sizeof(double));
    v[0] = v[4] = v[8] = 1;
    for (int sweep = 0; sweep < 32; sweep++) {
        double off = a[1] * a[1] + a[2] * a[2] + a[5] * a[5];
        if (off < 1e-30) break;
        for (int p = 0; p < 2; p++)
            for (int q = p + 1; q < 3; q++) {
                double apq = a[3 * p + q];
                if (fabs(apq) < 1e-300) continue;
                double theta = (a[3 * q + q] - a[3 * p + p]) / (2 * apq);
                double t = (theta >= 0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1));
                double c = 1 / sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; k++) {
                    double akp = a[3 * k + p], akq = a[3 * k + q];
                    a[3 * k + p] = c * akp - s * akq, a[3 * k + q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; k++) {
                    double apk = a[3 * p + k], aqk = a[3 * q + k];
                    a[3 * p + k] = c * apk - s * aqk, a[3 * q + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; k++) {
                    double vkp = v[3 * k + p], vkq = v[3 * k + q];
                    v[3 * k + p] = c * vkp - s * vkq, v[3 * k + q] = s * vkp + c * vkq;
                }
            }
    }
    for (int k = 0; k < 3; k++) lambda[k] = a[3 * k + k];
}

/* the deformation gradient and the material shape-function gradients at a Gauss point */
static bool gradients_at(const double X[8][3], const double ue[24], const double xi[3], double dNdX[8][3], double F[9], double *detJ0) {
    double N[8], dN[8][3], J[3][3];
    hex8_shape(xi[0], xi[1], xi[2], N, dN);
    double dj = hex8_jacobian(X, dN, J, dNdX);
    if (!(dj > 0)) return false;
    *detJ0 = dj;
    memset(F, 0, 9 * sizeof(double));
    F[0] = F[4] = F[8] = 1;
    for (int a = 0; a < 8; a++)
        for (int i = 0; i < 3; i++)
            for (int Jj = 0; Jj < 3; Jj++) F[3 * i + Jj] += ue[3 * a + i] * dNdX[a][Jj];
    return true;
}

static const double NL_XI[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};

static bool gradients(const double X[8][3], const double ue[24], int g, double dNdX[8][3], double F[9], double *detJ0) {
    const double r = 1.0 / sqrt(3.0);
    double xi[3] = {NL_XI[g][0] * r, NL_XI[g][1] * r, NL_XI[g][2] * r};
    return gradients_at(X, ue, xi, dNdX, F, detJ0);
}

bool hex8_nl_centre_gradient(const double X[8][3], const double ue[24], double F[9], double *detJ0) {
    double dNdX[8][3], xi[3] = {0, 0, 0};
    return gradients_at(X, ue, xi, dNdX, F, detJ0);
}

bool hex8_nl_gradient(const double X[8][3], const double ue[24], int g, double F[9], double *detJ0) {
    double dNdX[8][3];
    return gradients(X, ue, g, dNdX, F, detJ0);
}

/* The nine enhanced modes at a Gauss point, mapped from the natural frame to the global one:
 *   E~ = (j0 / j(xi)) * T0 * M(xi) * alpha,   M = diag(xi, eta, zeta) on the normal components and the pairs on the
 * shears. T0 is the congruence by the inverse Jacobian at the element centre, built column by column so that the
 * transformation is exact for a distorted element as well. */
static void eas_modes(const double T0[36], double j0_over_j, const double xi[3], double Mt[6][9]) {
    double M[6][9] = {{0}};
    M[0][0] = xi[0];
    M[1][1] = xi[1];
    M[2][2] = xi[2];
    M[3][3] = xi[0], M[3][4] = xi[1];
    M[4][5] = xi[1], M[4][6] = xi[2];
    M[5][7] = xi[2], M[5][8] = xi[0];
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 9; k++) {
            double s = 0;
            for (int j = 0; j < 6; j++) s += T0[6 * i + j] * M[j][k];
            Mt[i][k] = j0_over_j * s;
        }
}

/* the 6 x 6 congruence E_global = A' E_natural A in Voigt with engineering shear, built by transforming the six basis
 * strains one by one (no hand-written formula to get wrong) */
static void voigt_congruence(const double A[9], double T[36]) {
    for (int b = 0; b < 6; b++) {
        double e[9] = {0};
        if (b < 3) e[3 * b + b] = 1;
        else {
            int i = b == 3 ? 0 : (b == 4 ? 1 : 2), j = b == 3 ? 1 : (b == 4 ? 2 : 0);
            e[3 * i + j] = e[3 * j + i] = 0.5; /* engineering shear: the tensor component is half */
        }
        double At[9], tmp[9], G[9];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) At[3 * i + j] = A[3 * j + i];
        m3_mul(At, e, tmp);
        m3_mul(tmp, A, G);
        T[6 * 0 + b] = G[0], T[6 * 1 + b] = G[4], T[6 * 2 + b] = G[8];
        T[6 * 3 + b] = G[1] + G[3], T[6 * 4 + b] = G[5] + G[7], T[6 * 5 + b] = G[2] + G[6];
    }
}

/* solve a small dense symmetric system by Gauss elimination with partial pivoting (n <= 9) */
static bool dense_solve(int n, double *A, double *b) {
    for (int c = 0; c < n; c++) {
        int p = c;
        for (int r = c + 1; r < n; r++)
            if (fabs(A[n * r + c]) > fabs(A[n * p + c])) p = r;
        if (fabs(A[n * p + c]) < 1e-300) return false;
        if (p != c) {
            for (int k = 0; k < n; k++) {
                double t = A[n * c + k];
                A[n * c + k] = A[n * p + k], A[n * p + k] = t;
            }
            double t = b[c];
            b[c] = b[p], b[p] = t;
        }
        for (int r = c + 1; r < n; r++) {
            double f = A[n * r + c] / A[n * c + c];
            if (f == 0) continue;
            for (int k = c; k < n; k++) A[n * r + k] -= f * A[n * c + k];
            b[r] -= f * b[c];
        }
    }
    for (int r = n - 1; r >= 0; r--) {
        double s = b[r];
        for (int k = r + 1; k < n; k++) s -= A[n * r + k] * b[k];
        b[r] = s / A[n * r + r];
    }
    return true;
}

void hex8_nl_cauchy(const double F[9], const double S[6], double sigma[6]) {
    double Sm[9] = {S[0], S[3], S[5], S[3], S[1], S[4], S[5], S[4], S[2]};
    double FS[9], FSFt[9], Ft[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) Ft[3 * i + j] = F[3 * j + i];
    m3_mul(F, Sm, FS);
    m3_mul(FS, Ft, FSFt);
    double j = m3_det(F);
    if (!(fabs(j) > 0)) j = 1;
    sigma[0] = FSFt[0] / j, sigma[1] = FSFt[4] / j, sigma[2] = FSFt[8] / j;
    sigma[3] = FSFt[1] / j, sigma[4] = FSFt[5] / j, sigma[5] = FSFt[2] / j;
}

/* J2 at finite strain, multiplicative: the elastic trial from F and the stored inverse plastic gradient, the return
 * map in principal logarithmic strain, the plastic part updated by the exponential map (dynamics.md section 1).
 * Returns the second Piola-Kirchhoff stress; with commit the state is written back. */
static void j2_finite(const double F[9], const Hex8NlMaterial *m, Hex8NlState *st, bool commit, double S[6]) {
    double mu = m->E / (2 * (1 + m->nu)), K = m->E / (3 * (1 - 2 * m->nu));
    double Fp_inv[9];
    memcpy(Fp_inv, st->Fp_inv, sizeof Fp_inv);
    if (Fp_inv[0] == 0 && Fp_inv[4] == 0 && Fp_inv[8] == 0) { /* zero means the identity: no plastic flow yet */
        memset(Fp_inv, 0, sizeof Fp_inv);
        Fp_inv[0] = Fp_inv[4] = Fp_inv[8] = 1;
    }
    double Fe[9], be[9], Fet[9];
    m3_mul(F, Fp_inv, Fe);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) Fet[3 * i + j] = Fe[3 * j + i];
    m3_mul(Fe, Fet, be); /* the elastic left Cauchy-Green trial */
    double lam2[3], n[9];
    sym3_eigen(be, lam2, n);
    double eps[3], lam[3];
    for (int a = 0; a < 3; a++) {
        if (!(lam2[a] > 1e-300)) lam2[a] = 1e-300;
        lam[a] = sqrt(lam2[a]);
        eps[a] = 0.5 * log(lam2[a]);
    }
    double tr = eps[0] + eps[1] + eps[2], tau[3], s[3];
    for (int a = 0; a < 3; a++) {
        double dev = eps[a] - tr / 3.0;
        tau[a] = 2 * mu * dev + K * tr; /* Kirchhoff stress, principal */
        s[a] = 2 * mu * dev;
    }
    double snorm = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    double q = sqrt(1.5) * snorm, alpha = st->alpha, dgamma = 0;
    if (m->yield > 0) {
        double f = q - (m->yield + m->hardening * alpha);
        if (f > 0) {
            dgamma = f / (3 * mu + m->hardening);
            double scale = snorm > 0 ? (q - 3 * mu * dgamma) / q : 0;
            for (int a = 0; a < 3; a++) {
                s[a] *= scale;
                eps[a] = s[a] / (2 * mu) + tr / 3.0;
                tau[a] = s[a] + K * tr;
            }
            alpha += dgamma;
        }
    }
    /* the Kirchhoff stress in the spatial frame, then the second Piola-Kirchhoff stress S = F^-1 tau F^-T */
    double taum[9] = {0};
    for (int a = 0; a < 3; a++)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) taum[3 * i + j] += tau[a] * n[3 * i + a] * n[3 * j + a];
    double Finv[9];
    if (!m3_inv(F, Finv)) {
        memset(S, 0, 6 * sizeof(double));
        return;
    }
    double FinvT[9], tmp[9], Sm[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) FinvT[3 * i + j] = Finv[3 * j + i];
    m3_mul(Finv, taum, tmp);
    m3_mul(tmp, FinvT, Sm);
    S[0] = Sm[0], S[1] = Sm[4], S[2] = Sm[8], S[3] = 0.5 * (Sm[1] + Sm[3]), S[4] = 0.5 * (Sm[5] + Sm[7]), S[5] = 0.5 * (Sm[2] + Sm[6]);
    if (!commit || dgamma <= 0) return;
    /* Fe_new = V_new V_tr^-1 Fe_tr, and the state keeps Fp^-1 = F^-1 Fe_new */
    double Vnew[9] = {0}, Vtri[9] = {0};
    for (int a = 0; a < 3; a++) {
        double ln = exp(eps[a]); /* the new elastic stretch */
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                Vnew[3 * i + j] += ln * n[3 * i + a] * n[3 * j + a];
                Vtri[3 * i + j] += (1.0 / lam[a]) * n[3 * i + a] * n[3 * j + a];
            }
    }
    double A[9], Fe_new[9], Fp_inv_new[9];
    m3_mul(Vnew, Vtri, A);
    m3_mul(A, Fe, Fe_new);
    m3_mul(Finv, Fe_new, Fp_inv_new);
    memcpy(st->Fp_inv, Fp_inv_new, sizeof Fp_inv_new);
    st->alpha = alpha;
}

bool hex8_nl_one_point(const double X[8][3], const double ue[24], const Hex8NlMaterial *m, Hex8NlState *state, bool commit, double fe[24],
                       double S_out[6], double *detF_out) {
    double dNdX[8][3], F[9], detJ0, xi[3] = {0, 0, 0};
    if (!gradients_at(X, ue, xi, dNdX, F, &detJ0)) return false;
    double detF = m3_det(F);
    if (detF_out) *detF_out = detF;
    if (!(detF > 0)) return false;
    double vol = 8 * detJ0, C[9] = {0}, E[6], S[6];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) C[3 * i + j] += F[3 * k + i] * F[3 * k + j];
    E[0] = 0.5 * (C[0] - 1), E[1] = 0.5 * (C[4] - 1), E[2] = 0.5 * (C[8] - 1);
    E[3] = C[1], E[4] = C[5], E[5] = C[2];
    if (m->yield > 0 && state) {
        Hex8NlState local = *state;
        j2_finite(F, m, &local, commit, S);
        if (commit) *state = local;
    } else {
        double D[6][6];
        isotropic_D(m->E, m->nu, D);
        for (int i = 0; i < 6; i++) {
            S[i] = 0;
            for (int j = 0; j < 6; j++) S[i] += D[i][j] * E[j];
        }
    }
    if (S_out) memcpy(S_out, S, 6 * sizeof(double));
    if (fe) {
        memset(fe, 0, 24 * sizeof(double));
        for (int a = 0; a < 8; a++)
            for (int i = 0; i < 3; i++) {
                int c = 3 * a + i;
                double B[6];
                B[0] = F[3 * i + 0] * dNdX[a][0];
                B[1] = F[3 * i + 1] * dNdX[a][1];
                B[2] = F[3 * i + 2] * dNdX[a][2];
                B[3] = F[3 * i + 0] * dNdX[a][1] + F[3 * i + 1] * dNdX[a][0];
                B[4] = F[3 * i + 1] * dNdX[a][2] + F[3 * i + 2] * dNdX[a][1];
                B[5] = F[3 * i + 2] * dNdX[a][0] + F[3 * i + 0] * dNdX[a][2];
                double s = 0;
                for (int k = 0; k < 6; k++) s += B[k] * S[k];
                fe[c] = s * vol;
            }
    }
    return true;
}

bool hex8_nl_element(const double X[8][3], const double ue[24], const Hex8NlMaterial *m, Hex8NlState *state, bool commit, unsigned flags,
                     double fe[24], double Ke[576], double *gp_S, double *gp_E, double *min_detF) {
    double D[6][6];
    isotropic_D(m->E, m->nu, D);
    bool plastic = m->yield > 0 && state;
    bool eas = (flags & HEX8_NL_EAS) && !plastic; /* the plastic branch stays fully integrated (dynamics.md) */
    if (fe) memset(fe, 0, 24 * sizeof(double));
    if (Ke) memset(Ke, 0, 576 * sizeof(double));
    if (min_detF) *min_detF = INFINITY;
    struct {
        double dNdX[8][3], F[9], detJ0, Ec[6], B[6][24], Mt[6][9];
    } gp[8];
    double T0[36] = {0}, j0 = 1;
    if (eas) { /* the Jacobian at the element centre sets the frame the enhanced modes live in */
        double N[8], dN[8][3], J[3][3], dNdX0[8][3], A[9], J0[9];
        hex8_shape(0, 0, 0, N, dN);
        j0 = hex8_jacobian(X, dN, J, dNdX0);
        if (!(j0 > 0)) return false;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) J0[3 * i + j] = J[i][j];
        if (!m3_inv(J0, A)) return false;
        voigt_congruence(A, T0);
    }
    for (int g = 0; g < 8; g++) {
        if (!gradients(X, ue, g, gp[g].dNdX, gp[g].F, &gp[g].detJ0)) return false;
        double detF = m3_det(gp[g].F);
        if (min_detF) *min_detF = fmin(*min_detF, detF);
        if (!(detF > 0)) return false;
        double C[9] = {0};
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                for (int k = 0; k < 3; k++) C[3 * i + j] += gp[g].F[3 * k + i] * gp[g].F[3 * k + j];
        gp[g].Ec[0] = 0.5 * (C[0] - 1), gp[g].Ec[1] = 0.5 * (C[4] - 1), gp[g].Ec[2] = 0.5 * (C[8] - 1);
        gp[g].Ec[3] = C[1], gp[g].Ec[4] = C[5], gp[g].Ec[5] = C[2]; /* engineering shear */
        for (int a = 0; a < 8; a++)
            for (int i = 0; i < 3; i++) {
                int c = 3 * a + i;
                const double *F = gp[g].F;
                gp[g].B[0][c] = F[3 * i + 0] * gp[g].dNdX[a][0];
                gp[g].B[1][c] = F[3 * i + 1] * gp[g].dNdX[a][1];
                gp[g].B[2][c] = F[3 * i + 2] * gp[g].dNdX[a][2];
                gp[g].B[3][c] = F[3 * i + 0] * gp[g].dNdX[a][1] + F[3 * i + 1] * gp[g].dNdX[a][0];
                gp[g].B[4][c] = F[3 * i + 1] * gp[g].dNdX[a][2] + F[3 * i + 2] * gp[g].dNdX[a][1];
                gp[g].B[5][c] = F[3 * i + 2] * gp[g].dNdX[a][0] + F[3 * i + 0] * gp[g].dNdX[a][2];
            }
        if (eas) {
            const double r = 1.0 / sqrt(3.0);
            double xi[3] = {NL_XI[g][0] * r, NL_XI[g][1] * r, NL_XI[g][2] * r};
            eas_modes(T0, j0 / gp[g].detJ0, xi, gp[g].Mt);
        }
    }
    /* the enhanced parameters follow from the element's own equilibrium; for St. Venant-Kirchhoff the strain is linear
     * in them, so one solve of the 9 x 9 system is exact */
    double alpha[9] = {0};
    if (eas) {
        double Kaa[81] = {0}, ra[9] = {0};
        for (int g = 0; g < 8; g++) {
            double DM[6][9], DE[6];
            for (int i = 0; i < 6; i++) {
                DE[i] = 0;
                for (int j = 0; j < 6; j++) DE[i] += D[i][j] * gp[g].Ec[j];
                for (int k = 0; k < 9; k++) {
                    double s = 0;
                    for (int j = 0; j < 6; j++) s += D[i][j] * gp[g].Mt[j][k];
                    DM[i][k] = s;
                }
            }
            for (int k = 0; k < 9; k++) {
                for (int i = 0; i < 6; i++) ra[k] += gp[g].Mt[i][k] * DE[i] * gp[g].detJ0;
                for (int l = 0; l < 9; l++) {
                    double s = 0;
                    for (int i = 0; i < 6; i++) s += gp[g].Mt[i][k] * DM[i][l];
                    Kaa[9 * k + l] += s * gp[g].detJ0;
                }
            }
        }
        for (int k = 0; k < 9; k++) alpha[k] = -ra[k];
        if (!dense_solve(9, Kaa, alpha)) memset(alpha, 0, sizeof alpha);
    }
    double Kua[24 * 9] = {0}, Kaa2[81] = {0};
    for (int g = 0; g < 8; g++) {
        double E[6], S[6];
        for (int i = 0; i < 6; i++) {
            E[i] = gp[g].Ec[i];
            if (eas)
                for (int k = 0; k < 9; k++) E[i] += gp[g].Mt[i][k] * alpha[k];
        }
        if (plastic) {
            Hex8NlState local = state[g];
            j2_finite(gp[g].F, m, &local, commit, S);
            if (commit) state[g] = local;
        } else {
            for (int i = 0; i < 6; i++) {
                S[i] = 0;
                for (int j = 0; j < 6; j++) S[i] += D[i][j] * E[j];
            }
        }
        if (gp_S) memcpy(gp_S + 6 * g, S, 6 * sizeof(double));
        if (gp_E) memcpy(gp_E + 6 * g, E, 6 * sizeof(double));
        if (fe)
            for (int c = 0; c < 24; c++) {
                double s = 0;
                for (int i = 0; i < 6; i++) s += gp[g].B[i][c] * S[i];
                fe[c] += s * gp[g].detJ0;
            }
        if (!Ke) continue;
        double DB[6][24];
        for (int i = 0; i < 6; i++)
            for (int c = 0; c < 24; c++) {
                double s = 0;
                for (int j = 0; j < 6; j++) s += D[i][j] * gp[g].B[j][c];
                DB[i][c] = s;
            }
        for (int r = 0; r < 24; r++)
            for (int c = 0; c < 24; c++) {
                double s = 0;
                for (int i = 0; i < 6; i++) s += gp[g].B[i][r] * DB[i][c];
                Ke[24 * r + c] += s * gp[g].detJ0;
            }
        double Sm[9] = {S[0], S[3], S[5], S[3], S[1], S[4], S[5], S[4], S[2]};
        for (int a = 0; a < 8; a++)
            for (int b = 0; b < 8; b++) {
                double gab = 0;
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) gab += gp[g].dNdX[a][i] * Sm[3 * i + j] * gp[g].dNdX[b][j];
                for (int i = 0; i < 3; i++) Ke[24 * (3 * a + i) + 3 * b + i] += gab * gp[g].detJ0;
            }
        if (!eas) continue;
        for (int k = 0; k < 9; k++) { /* the coupling and the enhanced block, for the condensation below */
            double DMk[6];
            for (int i = 0; i < 6; i++) {
                double s = 0;
                for (int j = 0; j < 6; j++) s += D[i][j] * gp[g].Mt[j][k];
                DMk[i] = s;
            }
            for (int c = 0; c < 24; c++) {
                double s = 0;
                for (int i = 0; i < 6; i++) s += gp[g].B[i][c] * DMk[i];
                Kua[9 * c + k] += s * gp[g].detJ0;
            }
            for (int l = 0; l < 9; l++) {
                double s = 0;
                for (int i = 0; i < 6; i++) s += gp[g].Mt[i][l] * DMk[i]; /* row l against the column k of D M~ */
                Kaa2[9 * l + k] += s * gp[g].detJ0;
            }
        }
    }
    if (Ke && eas) { /* static condensation: K = Kuu - Kua Kaa^-1 Kau */
        double Z[9 * 24];
        for (int c = 0; c < 24; c++) {
            double A[81], rhs[9];
            memcpy(A, Kaa2, sizeof A);
            for (int k = 0; k < 9; k++) rhs[k] = Kua[9 * c + k];
            if (!dense_solve(9, A, rhs)) memset(rhs, 0, sizeof rhs);
            for (int k = 0; k < 9; k++) Z[9 * c + k] = rhs[k];
        }
        for (int r = 0; r < 24; r++)
            for (int c = 0; c < 24; c++) {
                double s = 0;
                for (int k = 0; k < 9; k++) s += Kua[9 * r + k] * Z[9 * c + k];
                Ke[24 * r + c] -= s;
            }
    }
    return true;
}
