/* mmath.c - dense routines for the mechanics layer (see mmath.h for conventions) */
#include "mmath.h"

#include <float.h>
#include <stdlib.h>

void mq_from_mat(double *q, const double *R) {
    /* Shoemake: pick the largest of w, x, y, z to divide by */
    double tr = R[0] + R[4] + R[8];
    if (tr > 0) {
        double s = sqrt(tr + 1.0) * 2.0; /* 4w */
        q[0] = 0.25 * s;
        q[1] = (R[7] - R[5]) / s;
        q[2] = (R[2] - R[6]) / s;
        q[3] = (R[3] - R[1]) / s;
    } else if (R[0] >= R[4] && R[0] >= R[8]) {
        double s = sqrt(1.0 + R[0] - R[4] - R[8]) * 2.0; /* 4x */
        q[0] = (R[7] - R[5]) / s;
        q[1] = 0.25 * s;
        q[2] = (R[3] + R[1]) / s;
        q[3] = (R[2] + R[6]) / s;
    } else if (R[4] >= R[8]) {
        double s = sqrt(1.0 + R[4] - R[0] - R[8]) * 2.0; /* 4y */
        q[0] = (R[2] - R[6]) / s;
        q[1] = (R[3] + R[1]) / s;
        q[2] = 0.25 * s;
        q[3] = (R[7] + R[5]) / s;
    } else {
        double s = sqrt(1.0 + R[8] - R[0] - R[4]) * 2.0; /* 4z */
        q[0] = (R[3] - R[1]) / s;
        q[1] = (R[2] + R[6]) / s;
        q[2] = (R[7] + R[5]) / s;
        q[3] = 0.25 * s;
    }
    if (q[0] < 0)
        for (int i = 0; i < 4; i++) q[i] = -q[i];
    mq_normalize(q);
}

void mrpy_from_rot(double *rpy, const double *R) {
    double cp = sqrt(R[0] * R[0] + R[3] * R[3]);
    rpy[1] = atan2(-R[6], cp);
    if (cp > 1e-9) {
        rpy[0] = atan2(R[7], R[8]);
        rpy[2] = atan2(R[3], R[0]);
    } else { /* gimbal lock: roll and yaw share an axis, report the whole rotation as yaw */
        rpy[0] = 0;
        rpy[2] = atan2(-R[1], R[4]);
    }
}

bool mm_chol_factor(double *A, int n, double reltol) {
    double dmax = 0;
    for (int i = 0; i < n; i++)
        if (fabs(A[i * n + i]) > dmax) dmax = fabs(A[i * n + i]);
    double floor = reltol * dmax;
    if (!(dmax > 0)) return n == 0;
    for (int j = 0; j < n; j++) {
        double d = A[j * n + j];
        for (int k = 0; k < j; k++) d -= A[j * n + k] * A[j * n + k];
        if (!(d > floor)) return false;
        d = sqrt(d);
        A[j * n + j] = d;
        for (int i = j + 1; i < n; i++) {
            double s = A[i * n + j];
            for (int k = 0; k < j; k++) s -= A[i * n + k] * A[j * n + k];
            A[i * n + j] = s / d;
        }
    }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) A[i * n + j] = 0;
    return true;
}

void mm_chol_lower(const double *L, int n, double *b) {
    for (int i = 0; i < n; i++) {
        double s = b[i];
        for (int k = 0; k < i; k++) s -= L[i * n + k] * b[k];
        b[i] = s / L[i * n + i];
    }
}

void mm_chol_upper(const double *L, int n, double *b) {
    for (int i = n - 1; i >= 0; i--) {
        double s = b[i];
        for (int k = i + 1; k < n; k++) s -= L[k * n + i] * b[k];
        b[i] = s / L[i * n + i];
    }
}

void mm_chol_solve(const double *L, int n, double *b) {
    mm_chol_lower(L, n, b);
    mm_chol_upper(L, n, b);
}

int mm_sym_eig(double *A, int n, double *w, double *V) {
    for (int i = 0; i < n * n; i++) V[i] = 0;
    for (int i = 0; i < n; i++) V[i * n + i] = 1;
    int sweep;
    bool converged = false;
    for (sweep = 1; sweep <= 100; sweep++) {
        double off = 0, diag = 0;
        for (int i = 0; i < n; i++) {
            diag += A[i * n + i] * A[i * n + i];
            for (int j = i + 1; j < n; j++) off += A[i * n + j] * A[i * n + j];
        }
        if (off <= 1e-30 * diag || off == 0) {
            converged = true;
            break;
        }
        for (int p = 0; p < n; p++)
            for (int q = p + 1; q < n; q++) {
                double apq = A[p * n + q];
                if (apq == 0) continue;
                double app = A[p * n + p], aqq = A[q * n + q];
                double theta = (aqq - app) / (2 * apq);
                double t = (theta >= 0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1));
                double c = 1 / sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < n; k++) { /* A <- J^T A J, columns p, q */
                    double akp = A[k * n + p], akq = A[k * n + q];
                    A[k * n + p] = c * akp - s * akq;
                    A[k * n + q] = s * akp + c * akq;
                }
                for (int k = 0; k < n; k++) { /* rows p, q */
                    double apk = A[p * n + k], aqk = A[q * n + k];
                    A[p * n + k] = c * apk - s * aqk;
                    A[q * n + k] = s * apk + c * aqk;
                }
                A[p * n + q] = A[q * n + p] = 0;
                for (int k = 0; k < n; k++) {
                    double vkp = V[k * n + p], vkq = V[k * n + q];
                    V[k * n + p] = c * vkp - s * vkq;
                    V[k * n + q] = s * vkp + c * vkq;
                }
            }
    }
    for (int i = 0; i < n; i++) w[i] = A[i * n + i];
    /* selection sort, descending, carrying eigenvector columns */
    for (int i = 0; i < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++)
            if (w[j] > w[best]) best = j;
        if (best != i) {
            double t = w[i];
            w[i] = w[best], w[best] = t;
            for (int k = 0; k < n; k++) {
                double tv = V[k * n + i];
                V[k * n + i] = V[k * n + best], V[k * n + best] = tv;
            }
        }
    }
    return converged ? sweep : -sweep;
}

void mm_sym_pinv_solve(const double *A, int n, const double *b, double *x, double reltol, int *rank, double *inconsistency, double *work) {
    double *a = work, *V = work + n * n, *w = work + 2 * n * n, *c = work + 2 * n * n + n;
    memcpy(a, A, (size_t)n * (size_t)n * sizeof *a);
    mm_sym_eig(a, n, w, V);
    double wmax = n > 0 ? fabs(w[0]) : 0;
    double bn = 0, nul = 0;
    int r = 0;
    for (int i = 0; i < n; i++) bn += b[i] * b[i];
    for (int i = 0; i < n; i++) x[i] = 0;
    for (int j = 0; j < n; j++) {
        double p = 0;
        for (int k = 0; k < n; k++) p += V[k * n + j] * b[k];
        c[j] = p;
        if (w[j] > reltol * wmax && w[j] > 0) {
            r++;
            for (int k = 0; k < n; k++) x[k] += V[k * n + j] * (p / w[j]);
        } else
            nul += p * p;
    }
    if (rank) *rank = r;
    if (inconsistency) *inconsistency = bn > 0 ? sqrt(nul / bn) : 0;
}

int mm_svd_jacobi(double *B, int p, int k, double *s, double *V) {
    for (int i = 0; i < k * k; i++) V[i] = 0;
    for (int i = 0; i < k; i++) V[i * k + i] = 1;
    int sweep;
    for (sweep = 1; sweep <= 80; sweep++) {
        bool rotated = false;
        for (int i = 0; i < k; i++)
            for (int j = i + 1; j < k; j++) {
                double alpha = 0, beta = 0, gamma = 0;
                for (int r = 0; r < p; r++) {
                    double bi = B[r * k + i], bj = B[r * k + j];
                    alpha += bi * bi;
                    beta += bj * bj;
                    gamma += bi * bj;
                }
                if (gamma == 0 || fabs(gamma) <= 1e-15 * sqrt(alpha * beta)) continue;
                rotated = true;
                double zeta = (beta - alpha) / (2 * gamma);
                double t = (zeta >= 0 ? 1.0 : -1.0) / (fabs(zeta) + sqrt(1 + zeta * zeta));
                double c = 1 / sqrt(1 + t * t), sn = c * t;
                for (int r = 0; r < p; r++) {
                    double bi = B[r * k + i], bj = B[r * k + j];
                    B[r * k + i] = c * bi - sn * bj;
                    B[r * k + j] = sn * bi + c * bj;
                }
                for (int r = 0; r < k; r++) {
                    double vi = V[r * k + i], vj = V[r * k + j];
                    V[r * k + i] = c * vi - sn * vj;
                    V[r * k + j] = sn * vi + c * vj;
                }
            }
        if (!rotated) break;
    }
    for (int j = 0; j < k; j++) {
        double n2 = 0;
        for (int r = 0; r < p; r++) n2 += B[r * k + j] * B[r * k + j];
        s[j] = sqrt(n2);
    }
    for (int i = 0; i < k; i++) {
        int best = i;
        for (int j = i + 1; j < k; j++)
            if (s[j] > s[best]) best = j;
        if (best != i) {
            double t = s[i];
            s[i] = s[best], s[best] = t;
            for (int r = 0; r < k; r++) {
                double tv = V[r * k + i];
                V[r * k + i] = V[r * k + best], V[r * k + best] = tv;
            }
            for (int r = 0; r < p; r++) {
                double tb = B[r * k + i];
                B[r * k + i] = B[r * k + best], B[r * k + best] = tb;
            }
        }
    }
    return sweep;
}
