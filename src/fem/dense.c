/* dense.c - small dense linear algebra */
#include "dense.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool dense_solve(double *A, double *b, int n) {
    for (int k = 0; k < n; k++) {
        int piv = k;
        double best = fabs(A[k * n + k]);
        for (int i = k + 1; i < n; i++)
            if (fabs(A[i * n + k]) > best) best = fabs(A[i * n + k]), piv = i;
        if (!(best > 0)) return false;
        if (piv != k) {
            for (int j = 0; j < n; j++) {
                double t = A[k * n + j];
                A[k * n + j] = A[piv * n + j], A[piv * n + j] = t;
            }
            double t = b[k];
            b[k] = b[piv], b[piv] = t;
        }
        double inv = 1.0 / A[k * n + k];
        for (int i = k + 1; i < n; i++) {
            double f = A[i * n + k] * inv;
            if (f == 0) continue;
            for (int j = k; j < n; j++) A[i * n + j] -= f * A[k * n + j];
            b[i] -= f * b[k];
        }
    }
    for (int i = n - 1; i >= 0; i--) {
        double s = b[i];
        for (int j = i + 1; j < n; j++) s -= A[i * n + j] * b[j];
        b[i] = s / A[i * n + i];
    }
    return true;
}

bool dense_spd_inverse(double *A, int n) {
    /* Cholesky A = L L^T stored in the lower triangle */
    for (int j = 0; j < n; j++) {
        double d = A[j * n + j];
        for (int k = 0; k < j; k++) d -= A[j * n + k] * A[j * n + k];
        if (!(d > 0)) return false;
        d = sqrt(d);
        A[j * n + j] = d;
        for (int i = j + 1; i < n; i++) {
            double s = A[i * n + j];
            for (int k = 0; k < j; k++) s -= A[i * n + k] * A[j * n + k];
            A[i * n + j] = s / d;
        }
    }
    /* invert L in place (lower triangle) */
    double Linv[64 * 64];
    if (n > 64) return false;
    memset(Linv, 0, sizeof(double) * (size_t)(n * n));
    for (int i = 0; i < n; i++) {
        Linv[i * n + i] = 1.0 / A[i * n + i];
        for (int j = 0; j < i; j++) {
            double s = 0;
            for (int k = j; k < i; k++) s -= A[i * n + k] * Linv[k * n + j];
            Linv[i * n + j] = s / A[i * n + i];
        }
    }
    /* A^-1 = L^-T L^-1 */
    for (int i = 0; i < n; i++)
        for (int j = 0; j <= i; j++) {
            double s = 0;
            for (int k = i; k < n; k++) s += Linv[k * n + i] * Linv[k * n + j];
            A[i * n + j] = A[j * n + i] = s;
        }
    return true;
}

bool dense_sym_eigen(double *A, int n, double *w, double *V) {
    if (n > 64) return false;
    if (V) {
        memset(V, 0, sizeof(double) * (size_t)(n * n));
        for (int i = 0; i < n; i++) V[i * n + i] = 1;
    }
    for (int sweep = 0; sweep < 100; sweep++) {
        double off = 0, scale = 0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                if (i != j) off += A[i * n + j] * A[i * n + j];
                else scale += A[i * n + j] * A[i * n + j];
            }
        if (off <= 1e-30 * (scale > 0 ? scale : 1)) break;
        for (int p = 0; p < n - 1; p++)
            for (int q = p + 1; q < n; q++) {
                double apq = A[p * n + q];
                if (fabs(apq) < 1e-300) continue;
                double app = A[p * n + p], aqq = A[q * n + q];
                double theta = (aqq - app) / (2 * apq);
                double t = (theta >= 0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1));
                double c = 1 / sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < n; k++) {
                    double akp = A[k * n + p], akq = A[k * n + q];
                    A[k * n + p] = c * akp - s * akq;
                    A[k * n + q] = s * akp + c * akq;
                }
                for (int k = 0; k < n; k++) {
                    double apk = A[p * n + k], aqk = A[q * n + k];
                    A[p * n + k] = c * apk - s * aqk;
                    A[q * n + k] = s * apk + c * aqk;
                }
                if (V)
                    for (int k = 0; k < n; k++) {
                        double vkp = V[k * n + p], vkq = V[k * n + q];
                        V[k * n + p] = c * vkp - s * vkq;
                        V[k * n + q] = s * vkp + c * vkq;
                    }
            }
    }
    for (int i = 0; i < n; i++) w[i] = A[i * n + i];
    /* sort ascending, carrying eigenvectors */
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && w[j] < w[j - 1]; j--) {
            double t = w[j];
            w[j] = w[j - 1], w[j - 1] = t;
            if (V)
                for (int k = 0; k < n; k++) {
                    double vt = V[k * n + j];
                    V[k * n + j] = V[k * n + j - 1], V[k * n + j - 1] = vt;
                }
        }
    }
    return true;
}

void sym3_eigenvalues(const double v[6], double s[3]) {
    /* trigonometric solution of the characteristic cubic (Smith 1961), robust for repeated roots */
    double a11 = v[0], a22 = v[1], a33 = v[2], a12 = v[3], a23 = v[4], a13 = v[5];
    double p1 = a12 * a12 + a13 * a13 + a23 * a23;
    double q = (a11 + a22 + a33) / 3.0;
    if (p1 <= 1e-30 * (a11 * a11 + a22 * a22 + a33 * a33 + 1e-300)) {
        s[0] = a11, s[1] = a22, s[2] = a33;
    } else {
        double p2 = (a11 - q) * (a11 - q) + (a22 - q) * (a22 - q) + (a33 - q) * (a33 - q) + 2 * p1;
        double p = sqrt(p2 / 6.0);
        double b11 = (a11 - q) / p, b22 = (a22 - q) / p, b33 = (a33 - q) / p, b12 = a12 / p, b23 = a23 / p, b13 = a13 / p;
        double r = 0.5 * (b11 * (b22 * b33 - b23 * b23) - b12 * (b12 * b33 - b23 * b13) + b13 * (b12 * b23 - b22 * b13));
        double phi = r <= -1 ? M_PI / 3.0 : (r >= 1 ? 0 : acos(r) / 3.0);
        s[0] = q + 2 * p * cos(phi);
        s[2] = q + 2 * p * cos(phi + 2.0 * M_PI / 3.0);
        s[1] = 3 * q - s[0] - s[2];
    }
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2 - i; j++)
            if (s[j] < s[j + 1]) {
                double t = s[j];
                s[j] = s[j + 1], s[j + 1] = t;
            }
}

double von_mises(const double s[6]) {
    double a = s[0] - s[1], b = s[1] - s[2], c = s[2] - s[0];
    return sqrt(0.5 * (a * a + b * b + c * c) + 3.0 * (s[3] * s[3] + s[4] * s[4] + s[5] * s[5]));
}

double det3(const double m[9]) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

bool inv3(const double m[9], double out[9], double *det) {
    double d = det3(m);
    if (det) *det = d;
    if (!(fabs(d) > 0)) return false;
    double id = 1.0 / d;
    out[0] = (m[4] * m[8] - m[5] * m[7]) * id;
    out[1] = (m[2] * m[7] - m[1] * m[8]) * id;
    out[2] = (m[1] * m[5] - m[2] * m[4]) * id;
    out[3] = (m[5] * m[6] - m[3] * m[8]) * id;
    out[4] = (m[0] * m[8] - m[2] * m[6]) * id;
    out[5] = (m[2] * m[3] - m[0] * m[5]) * id;
    out[6] = (m[3] * m[7] - m[4] * m[6]) * id;
    out[7] = (m[1] * m[6] - m[0] * m[7]) * id;
    out[8] = (m[0] * m[4] - m[1] * m[3]) * id;
    return true;
}
