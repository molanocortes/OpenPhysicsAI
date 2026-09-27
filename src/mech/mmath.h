/* mmath.h - fixed-size linear algebra for the mechanics layer: 3-vectors, 3x3 matrices, unit quaternions, rigid poses,
 * 6-D spatial vectors and spatial inertias (inline), plus small dense solvers (mmath.c).
 *
 * Conventions used throughout src/mech:
 *   matrices     row-major double[9]: m[3*r + c]
 *   quaternion   (w, x, y, z), Hamilton product, unit length. R(q) maps vectors from the rotated (body/child) frame to
 *                the reference (world/parent) frame: v_ref = R(q) v_body
 *   pose         (R, p): x_parent = R x_local + p; R maps local to parent axes, p is the local origin in parent coordinates
 *   rpy          URDF convention, fixed axes: R = Rz(yaw) Ry(pitch) Rx(roll)
 *   spatial      motion vectors [angular; linear] = [w; v]; force vectors [moment; force] = [n; f]. In world coordinates
 *                both are referred to the world origin (Plücker coordinates): v is the velocity of the body point that is
 *                momentarily at the origin, n is the moment about the origin
 *   inertia      SpInertia {m, h = m c, I about the reference origin}; spatial momentum = [I w + h x v; m v - h x w]
 * Build without -ffast-math and without FP contraction (CORE_CFLAGS) so results are bitwise repeatable. */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ---- 3-vectors ---- */
static inline void mv3_set(double *o, double x, double y, double z) { o[0] = x, o[1] = y, o[2] = z; }
static inline void mv3_copy(double *o, const double *a) { o[0] = a[0], o[1] = a[1], o[2] = a[2]; }
static inline void mv3_add(double *o, const double *a, const double *b) { o[0] = a[0] + b[0], o[1] = a[1] + b[1], o[2] = a[2] + b[2]; }
static inline void mv3_sub(double *o, const double *a, const double *b) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static inline void mv3_scale(double *o, const double *a, double s) { o[0] = a[0] * s, o[1] = a[1] * s, o[2] = a[2] * s; }
static inline void mv3_addto(double *o, const double *a) { o[0] += a[0], o[1] += a[1], o[2] += a[2]; }
static inline void mv3_addscaled(double *o, const double *a, double s) { o[0] += a[0] * s, o[1] += a[1] * s, o[2] += a[2] * s; }
static inline double mv3_dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline double mv3_norm(const double *a) { return sqrt(mv3_dot(a, a)); }
static inline void mv3_cross(double *o, const double *a, const double *b) {
    double x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
static inline double mv3_normalize(double *a) {
    double n = mv3_norm(a);
    if (n > 0) a[0] /= n, a[1] /= n, a[2] /= n;
    return n;
}

/* ---- 3x3 matrices (row-major) ---- */
static inline void mm3_identity(double *m) {
    memset(m, 0, 9 * sizeof *m);
    m[0] = m[4] = m[8] = 1;
}
static inline void mm3_copy(double *o, const double *a) { memcpy(o, a, 9 * sizeof *o); }
static inline void mm3_mul(double *o, const double *a, const double *b) {
    double t[9];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) t[3 * r + c] = a[3 * r] * b[c] + a[3 * r + 1] * b[3 + c] + a[3 * r + 2] * b[6 + c];
    memcpy(o, t, sizeof t);
}
static inline void mm3_transpose(double *o, const double *a) {
    double t[9] = {a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]};
    memcpy(o, t, sizeof t);
}
static inline void mm3_mulv(double *o, const double *m, const double *v) {
    double x = m[0] * v[0] + m[1] * v[1] + m[2] * v[2], y = m[3] * v[0] + m[4] * v[1] + m[5] * v[2], z = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
    o[0] = x, o[1] = y, o[2] = z;
}
static inline void mm3_tmulv(double *o, const double *m, const double *v) { /* m^T v */
    double x = m[0] * v[0] + m[3] * v[1] + m[6] * v[2], y = m[1] * v[0] + m[4] * v[1] + m[7] * v[2], z = m[2] * v[0] + m[5] * v[1] + m[8] * v[2];
    o[0] = x, o[1] = y, o[2] = z;
}
static inline void mm3_skew(double *o, const double *v) {
    double t[9] = {0, -v[2], v[1], v[2], 0, -v[0], -v[1], v[0], 0};
    memcpy(o, t, sizeof t);
}
/* o = R A R^T */
static inline void mm3_rotate_sym(double *o, const double *R, const double *A) {
    double t[9], Rt[9];
    mm3_mul(t, R, A);
    mm3_transpose(Rt, R);
    mm3_mul(o, t, Rt);
}
static inline double mm3_det(const double *m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

/* ---- quaternions (w, x, y, z) ---- */
static inline void mq_identity(double *q) { q[0] = 1, q[1] = q[2] = q[3] = 0; }
static inline void mq_mul(double *o, const double *a, const double *b) {
    double w = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    double x = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    double y = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    double z = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
    o[0] = w, o[1] = x, o[2] = y, o[3] = z;
}
static inline double mq_normalize(double *q) {
    double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0)
        for (int i = 0; i < 4; i++) q[i] /= n;
    else
        mq_identity(q);
    return n;
}
static inline void mq_to_mat(double *R, const double *q) {
    double w = q[0], x = q[1], y = q[2], z = q[3];
    R[0] = 1 - 2 * (y * y + z * z), R[1] = 2 * (x * y - w * z), R[2] = 2 * (x * z + w * y);
    R[3] = 2 * (x * y + w * z), R[4] = 1 - 2 * (x * x + z * z), R[5] = 2 * (y * z - w * x);
    R[6] = 2 * (x * z - w * y), R[7] = 2 * (y * z + w * x), R[8] = 1 - 2 * (x * x + y * y);
}
/* exp of a rotation vector (axis * angle) */
static inline void mq_exp(double *q, const double *th) {
    double a = mv3_norm(th), h = 0.5 * a, s;
    if (a < 1e-6) /* sin(h)/a = 1/2 - a^2/48 + ... */
        s = 0.5 - a * a / 48.0;
    else
        s = sin(h) / a;
    q[0] = cos(h), q[1] = th[0] * s, q[2] = th[1] * s, q[3] = th[2] * s;
}
/* rotation vector of a unit quaternion (angle in [0, pi]) */
static inline void mq_log(double *th, const double *qin) {
    double q[4] = {qin[0], qin[1], qin[2], qin[3]};
    if (q[0] < 0)
        for (int i = 0; i < 4; i++) q[i] = -q[i];
    double s = sqrt(q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    double a = 2 * atan2(s, q[0]);
    double k = s < 1e-12 ? 2.0 : a / s;
    th[0] = q[1] * k, th[1] = q[2] * k, th[2] = q[3] * k;
}
void mq_from_mat(double *q, const double *R);

/* URDF rpy: R = Rz(yaw) * Ry(pitch) * Rx(roll) */
static inline void mrot_from_rpy(double *R, double roll, double pitch, double yaw) {
    double cr = cos(roll), sr = sin(roll), cp = cos(pitch), sp = sin(pitch), cy = cos(yaw), sy = sin(yaw);
    R[0] = cy * cp, R[1] = cy * sp * sr - sy * cr, R[2] = cy * sp * cr + sy * sr;
    R[3] = sy * cp, R[4] = sy * sp * sr + cy * cr, R[5] = sy * sp * cr - cy * sr;
    R[6] = -sp, R[7] = cp * sr, R[8] = cp * cr;
}
void mrpy_from_rot(double *rpy, const double *R);
/* rotation by angle about a unit axis */
static inline void mrot_axis_angle(double *R, const double *u, double a) {
    double c = cos(a), s = sin(a), t = 1 - c;
    R[0] = t * u[0] * u[0] + c, R[1] = t * u[0] * u[1] - s * u[2], R[2] = t * u[0] * u[2] + s * u[1];
    R[3] = t * u[0] * u[1] + s * u[2], R[4] = t * u[1] * u[1] + c, R[5] = t * u[1] * u[2] - s * u[0];
    R[6] = t * u[0] * u[2] - s * u[1], R[7] = t * u[1] * u[2] + s * u[0], R[8] = t * u[2] * u[2] + c;
}
/* inverse of the right Jacobian of SO(3): for R(t) = R0 exp([th(t)]x) with body angular velocity w,
 * d th/dt = Jr^-1(th) w = w + th x w / 2 + beta(|th|) th x (th x w), beta = 1/a^2 - (1 + cos a) / (2 a sin a) */
static inline void mso3_dexpinv(double *o, const double *th, const double *w) {
    double a = mv3_norm(th), beta, c1[3], c2[3];
    if (a < 1e-2) /* series: 1/12 + a^2/720 + a^4/30240 (next term ~a^6/1.2e6) */
        beta = 1.0 / 12.0 + a * a / 720.0 + a * a * a * a / 30240.0;
    else
        beta = 1.0 / (a * a) - (1.0 + cos(a)) / (2.0 * a * sin(a));
    mv3_cross(c1, th, w);
    mv3_cross(c2, th, c1);
    o[0] = w[0] + 0.5 * c1[0] + beta * c2[0];
    o[1] = w[1] + 0.5 * c1[1] + beta * c2[1];
    o[2] = w[2] + 0.5 * c1[2] + beta * c2[2];
}

/* ---- rigid poses ---- */
typedef struct MPose {
    double R[9];
    double p[3];
} MPose;

static inline void mpose_identity(MPose *T) {
    mm3_identity(T->R);
    mv3_set(T->p, 0, 0, 0);
}
/* o = a * b (b expressed in a's frame) */
static inline void mpose_mul(MPose *o, const MPose *a, const MPose *b) {
    MPose t;
    mm3_mul(t.R, a->R, b->R);
    mm3_mulv(t.p, a->R, b->p);
    mv3_addto(t.p, a->p);
    *o = t;
}
static inline void mpose_inverse(MPose *o, const MPose *a) {
    MPose t;
    mm3_transpose(t.R, a->R);
    mm3_mulv(t.p, t.R, a->p);
    mv3_scale(t.p, t.p, -1);
    *o = t;
}
static inline void mpose_apply(double *o, const MPose *T, const double *x) {
    double t[3];
    mm3_mulv(t, T->R, x);
    mv3_add(o, t, T->p);
}

/* ---- spatial vectors ---- */
/* motion vector in local coordinates -> parent coordinates, T = pose of local in parent: w' = R w, v' = R v + p x w' */
static inline void msv_motion_to_parent(double *o, const MPose *T, const double *m) {
    double w[3], v[3], c[3];
    mm3_mulv(w, T->R, m);
    mm3_mulv(v, T->R, m + 3);
    mv3_cross(c, T->p, w);
    mv3_add(o + 3, v, c);
    mv3_copy(o, w);
}
/* force vector local -> parent: f' = R f, n' = R n + p x f' */
static inline void msv_force_to_parent(double *o, const MPose *T, const double *f) {
    double n[3], ff[3], c[3];
    mm3_mulv(n, T->R, f);
    mm3_mulv(ff, T->R, f + 3);
    mv3_cross(c, T->p, ff);
    mv3_add(o, n, c);
    mv3_copy(o + 3, ff);
}
/* force vector parent -> local: f = R^T f', n = R^T (n' - p x f') */
static inline void msv_force_to_local(double *o, const MPose *T, const double *f) {
    double c[3], t[3], ff[3];
    mv3_cross(c, T->p, f + 3);
    mv3_sub(t, f, c);
    mm3_tmulv(ff, T->R, f + 3);
    mm3_tmulv(o, T->R, t);
    mv3_copy(o + 3, ff);
}
/* motion vector parent -> local: w = R^T w', v = R^T (v' - p x w') */
static inline void msv_motion_to_local(double *o, const MPose *T, const double *m) {
    double c[3], t[3], w[3];
    mv3_cross(c, T->p, m);
    mv3_sub(t, m + 3, c);
    mm3_tmulv(w, T->R, m);
    mm3_tmulv(o + 3, T->R, t);
    mv3_copy(o, w);
}
static inline double msv_dot(const double *a, const double *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] + a[4] * b[4] + a[5] * b[5];
}
/* motion cross product: a x m = [aw x mw; aw x mv + av x mw] */
static inline void msv_cross_motion(double *o, const double *a, const double *m) {
    double w[3], v1[3], v2[3];
    mv3_cross(w, a, m);
    mv3_cross(v1, a, m + 3);
    mv3_cross(v2, a + 3, m);
    mv3_copy(o, w);
    mv3_add(o + 3, v1, v2);
}
/* force cross product: a x* f = [aw x fn + av x ff; aw x ff] */
static inline void msv_cross_force(double *o, const double *a, const double *f) {
    double n1[3], n2[3], ff[3];
    mv3_cross(n1, a, f);
    mv3_cross(n2, a + 3, f + 3);
    mv3_cross(ff, a, f + 3);
    mv3_add(o, n1, n2);
    mv3_copy(o + 3, ff);
}

/* ---- spatial inertia ---- */
typedef struct SpInertia {
    double m;
    double h[3]; /* first mass moment m*c about the reference origin */
    double I[9]; /* rotational inertia about the reference origin */
} SpInertia;

static inline void mspi_zero(SpInertia *o) { memset(o, 0, sizeof *o); }
/* body with mass m, centre of mass c and inertia Ic about the centre of mass (all in local coordinates), expressed in the
 * parent frame of pose T and referred to the parent origin */
static inline void mspi_from_body(SpInertia *o, double m, const double *c, const double *Ic, const MPose *T) {
    double cw[3];
    mpose_apply(cw, T, c);
    o->m = m;
    mv3_scale(o->h, cw, m);
    mm3_rotate_sym(o->I, T->R, Ic);
    double cc = mv3_dot(cw, cw);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) o->I[3 * r + k] += m * ((r == k ? cc : 0) - cw[r] * cw[k]);
}
static inline void mspi_add(SpInertia *o, const SpInertia *a) {
    o->m += a->m;
    mv3_addto(o->h, a->h);
    for (int i = 0; i < 9; i++) o->I[i] += a->I[i];
}
/* spatial momentum (force vector) of motion v */
static inline void mspi_mul(double *o, const SpInertia *I, const double *v) {
    double Iw[3], hv[3], hw[3], n[3], f[3];
    mm3_mulv(Iw, I->I, v);
    mv3_cross(hv, I->h, v + 3);
    mv3_cross(hw, I->h, v);
    mv3_add(n, Iw, hv);
    mv3_scale(f, v + 3, I->m);
    mv3_sub(f, f, hw);
    mv3_copy(o, n);
    mv3_copy(o + 3, f);
}

/* ---- dense routines (mmath.c); matrices are row-major n x n unless stated ---- */
/* in-place Cholesky A = L L^T (lower triangle kept); false if not positive definite (pivot <= tol * max diagonal) */
bool mm_chol_factor(double *A, int n, double reltol);
void mm_chol_solve(const double *L, int n, double *b);           /* solves L L^T x = b in place */
void mm_chol_lower(const double *L, int n, double *b);     /* L y = b in place */
void mm_chol_upper(const double *L, int n, double *b);     /* L^T x = y in place */
/* symmetric eigendecomposition by cyclic Jacobi: A (destroyed) = V diag(w) V^T, eigenvalues sorted descending,
 * eigenvectors in the columns of V. Returns the number of sweeps (negative if not converged). */
int mm_sym_eig(double *A, int n, double *w, double *V);
/* minimum-norm solution of the symmetric positive semi-definite system A x = b: eigen-directions with eigenvalue below
 * reltol * max eigenvalue are treated as null. rank receives the numerical rank; *inconsistency the norm of b's component
 * in the null space divided by |b| (0 when consistent). A is not modified. work must hold 2*n*n + 2*n doubles. */
void mm_sym_pinv_solve(const double *A, int n, const double *b, double *x, double reltol, int *rank, double *inconsistency,
                    double *work);
/* one-sided Jacobi SVD of the p x k matrix B (row-major, overwritten by U*diag(s)): B = U diag(s) V^T. Singular values
 * (k of them; zero when p < k) are sorted descending with the columns of V (k x k, row-major). Returns sweeps used. */
int mm_svd_jacobi(double *B, int p, int k, double *s, double *V);
