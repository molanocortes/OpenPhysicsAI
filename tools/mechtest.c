/* mechtest.c - numerical verification of the mechanics layer against analytic references.
 * Every case prints the measured error next to its tolerance; convergence orders are measured, not assumed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/fem/dense.h"
#include "../src/fem/hex8.h"
#include "../src/fem/solid.h"
#include "../src/geom/mesh.h"
#include "../src/mech/loads.h"
#include "../src/mech/assembly.h"
#include "../src/mech/contact.h"
#include "../src/mech/massprops.h"
#include "../src/mech/mechsim.h"
#include "../src/mech/mechspec.h"
#include "../src/core/base64.h"
#include "../src/mech/xml.h"
#include "../src/mech/mmath.h"
#include "../src/mech/multibody.h"
#include "../src/mech/ortho.h"
#include "../src/mech/modal.h"
#include "../src/mech/flexbody.h"
#include "../src/mech/fffprint.h"
#include "../src/mech/lpbf.h"
#include "../src/mech/support_cell.h"
#include "../src/ctl/matlib.h"
#include "../src/ctl/transient_analysis.h"

static int g_pass, g_fail;
#define CHECK(cond, ...)                                                                                                                   \
    do {                                                                                                                                   \
        if (cond)                                                                                                                          \
            g_pass++;                                                                                                                      \
        else {                                                                                                                             \
            g_fail++;                                                                                                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                                                 \
            printf(__VA_ARGS__);                                                                                                           \
            printf("\n");                                                                                                                  \
        }                                                                                                                                  \
    } while (0)
#define REPORT(...)                                                                                                                        \
    do {                                                                                                                                   \
        printf("  ");                                                                                                                      \
        printf(__VA_ARGS__);                                                                                                               \
        printf("\n");                                                                                                                      \
    } while (0)

/* ---------------------------------------------------------------------------------------------- references */

/* complete elliptic integral of the first kind K(m), m = k^2 */
static double ellipk(double m) {
    double a = 1, b = sqrt(1 - m);
    for (int i = 0; i < 40 && fabs(a - b) > 1e-16 * a; i++) {
        double an = 0.5 * (a + b);
        b = sqrt(a * b);
        a = an;
    }
    return M_PI / (2 * a);
}

/* Jacobi elliptic functions by the descending Landen (AGM) scale, Abramowitz & Stegun 16.4 */
static void sncndn(double u, double m, double *sn, double *cn, double *dn) {
    if (m < 1e-15) {
        *sn = sin(u), *cn = cos(u), *dn = 1;
        return;
    }
    double a[32], c[32], b = sqrt(1 - m);
    a[0] = 1, c[0] = sqrt(m);
    int n = 0;
    while (fabs(c[n]) > 1e-17 && n < 30) {
        a[n + 1] = 0.5 * (a[n] + b);
        c[n + 1] = 0.5 * (a[n] - b);
        b = sqrt(a[n] * b);
        n++;
    }
    double phi = ldexp(1.0, n) * a[n] * u, prev = phi;
    for (int i = n; i >= 1; i--) {
        prev = phi;
        phi = 0.5 * (phi + asin(c[i] / a[i] * sin(phi)));
    }
    *sn = sin(phi), *cn = cos(phi), *dn = cos(phi) / cos(prev - phi);
}

static double diag3(double *I, double a, double b, double c) {
    memset(I, 0, 9 * sizeof *I);
    I[0] = a, I[4] = b, I[8] = c;
    return 0;
}

/* ---------------------------------------------------------------------------------------------- math */

static void test_math(void) {
    printf("== math: quaternions, rotations, SO(3) exponential, dense solvers\n");
    double th[3] = {0.3, -1.1, 0.7}, q[4], R[9], R2[9], q2[4], th2[3];
    mq_exp(q, th);
    mq_to_mat(R, q);
    double u[3] = {th[0], th[1], th[2]};
    double ang = mv3_normalize(u);
    mrot_axis_angle(R2, u, ang);
    double e = 0;
    for (int i = 0; i < 9; i++) e = fmax(e, fabs(R[i] - R2[i]));
    CHECK(e < 1e-15, "exp map matches axis-angle rotation (%.2e)", e);
    mq_from_mat(q2, R);
    mq_log(th2, q2);
    e = fmax(fabs(th2[0] - th[0]), fmax(fabs(th2[1] - th[1]), fabs(th2[2] - th[2])));
    CHECK(e < 1e-14, "log(exp(theta)) = theta (%.2e)", e);
    double rpy[3], R3[9];
    mrot_from_rpy(R3, 0.4, -0.9, 2.2);
    mrpy_from_rot(rpy, R3);
    CHECK(fabs(rpy[0] - 0.4) < 1e-14 && fabs(rpy[1] + 0.9) < 1e-14 && fabs(rpy[2] - 2.2) < 1e-14, "rpy round trip");
    /* URDF convention: rpy = Rz(yaw) Ry(pitch) Rx(roll) */
    double Rx[9], Ry[9], Rz[9], t[9], ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    mrot_axis_angle(Rx, ex, 0.4), mrot_axis_angle(Ry, ey, -0.9), mrot_axis_angle(Rz, ez, 2.2);
    mm3_mul(t, Rz, Ry), mm3_mul(t, t, Rx);
    e = 0;
    for (int i = 0; i < 9; i++) e = fmax(e, fabs(t[i] - R3[i]));
    CHECK(e < 1e-15, "rpy is Rz*Ry*Rx (%.2e)", e);
    /* dexp^-1: d/dt of theta(t) for R(t) = R0 exp(theta(t)) with body rate w, checked by finite differences */
    double w[3] = {0.8, -0.3, 1.9}, rate[3], thA[3] = {1.2, 0.4, -2.0};
    mso3_dexpinv(rate, thA, w);
    double h = 1e-6, qa[4], dq[4], qb[4], thp[3], thm[3], rp[9], rm[9];
    /* R(t+h) = exp(thA) exp(h w) -> theta(t+h) = log(...) */
    mq_exp(qa, thA);
    double hw[3] = {h * w[0], h * w[1], h * w[2]};
    mq_exp(dq, hw), mq_mul(qb, qa, dq), mq_log(thp, qb);
    double mhw[3] = {-h * w[0], -h * w[1], -h * w[2]};
    mq_exp(dq, mhw), mq_mul(qb, qa, dq), mq_log(thm, qb);
    (void)rp, (void)rm;
    e = 0;
    for (int k = 0; k < 3; k++) e = fmax(e, fabs((thp[k] - thm[k]) / (2 * h) - rate[k]));
    CHECK(e < 1e-8, "dexp^-1 matches the finite-difference rate of the log coordinates (%.2e)", e);
    double small[3] = {1e-3, -2e-3, 5e-4}, r1[3], r2[3];
    mso3_dexpinv(r1, small, w);
    /* series and closed-form branches agree at the switch |theta| = 1e-2 (theta differs by 2e-12) */
    double b1[3] = {0.009999999999, 0, 0}, b2[3] = {0.010000000001, 0, 0};
    mso3_dexpinv(r1, b1, w), mso3_dexpinv(r2, b2, w);
    double jump = fmax(fabs(r1[1] - r2[1]), fabs(r1[2] - r2[2]));
    CHECK(jump < 1e-10, "dexp^-1 series and closed form agree at the switch (%.2e; theta itself changes by 2e-12)", jump);
    /* Cholesky, symmetric eigen, SVD */
    double A[16] = {4, 1, 0.5, 0.2, 1, 3, 0.3, 0.1, 0.5, 0.3, 2, 0.4, 0.2, 0.1, 0.4, 1.5}, L[16], x[4] = {1, 2, 3, 4}, b[4];
    for (int i = 0; i < 4; i++) {
        b[i] = 0;
        for (int k = 0; k < 4; k++) b[i] += A[4 * i + k] * x[k];
    }
    memcpy(L, A, sizeof L);
    CHECK(mm_chol_factor(L, 4, 1e-14), "Cholesky of an SPD matrix");
    mm_chol_solve(L, 4, b);
    e = 0;
    for (int i = 0; i < 4; i++) e = fmax(e, fabs(b[i] - x[i]));
    CHECK(e < 1e-13, "Cholesky solve (%.2e)", e);
    double Ae[16], wv[4], V[16];
    memcpy(Ae, A, sizeof Ae);
    int sw = mm_sym_eig(Ae, 4, wv, V);
    double rec = 0;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) {
            double s = 0;
            for (int j = 0; j < 4; j++) s += V[4 * i + j] * wv[j] * V[4 * k + j];
            rec = fmax(rec, fabs(s - A[4 * i + k]));
        }
    CHECK(sw > 0 && rec < 1e-13 && wv[0] >= wv[1] && wv[2] >= wv[3], "Jacobi eigendecomposition reconstructs A (%.2e, %d sweeps)", rec, sw);
    /* rank-2 3x4 matrix */
    double B[12] = {1, 2, 3, 4, 2, 4, 6, 8, 1, 0, 1, 0}, s[4], VV[16];
    mm_svd_jacobi(B, 3, 4, s, VV);
    CHECK(s[0] > 1 && s[1] > 0.1 && s[2] < 1e-12 && s[3] < 1e-12, "SVD rank of a rank-2 matrix (%.2e, %.2e)", s[2], s[3]);
}

/* ---------------------------------------------------------------------------------------------- dynamics helpers */

typedef struct Run {
    MbModel *m;
    MbSim *sim;
    MbState *s;
    MbEval *ev;
    MechDiag diag;
} Run;

static bool run_open(Run *r, MbModelDef *def, const MbOptions *opt) {
    memset(r, 0, sizeof *r);
    mdiag_init(&r->diag);
    r->m = mb_compile(def, opt, &r->diag);
    if (!r->m) return false;
    r->sim = mb_sim_new(r->m);
    r->s = mb_state_new(r->m);
    r->ev = mb_eval_new(r->m);
    if (!r->sim || !r->s || !r->ev) return false;
    if (!mb_state_initial(r->m, r->s, &r->diag)) return false;
    return mb_assemble(r->sim, r->s, &r->diag);
}

static void run_close(Run *r) {
    mb_eval_free(r->ev);
    mb_state_free(r->s);
    mb_sim_free(r->sim);
    mb_model_free(r->m);
    mdiag_free(&r->diag);
}

static bool advance(Run *r, double h, double t_end, const MbInputs *in) {
    MbStepReport rep;
    if (!r->sim || !r->s) return false;
    while (r->s->t < t_end - 1e-12 * t_end) {
        double hh = fmin(h, t_end - r->s->t);
        if (!mb_step(r->sim, r->s, hh, in, &rep)) {
            printf("  step failed at t=%g: %s\n", r->s->t, rep.error);
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------------------------- free body */

/* torque-free asymmetric body (Landau & Lifshitz, section 37) with the centre of mass off the body origin, under gravity */
typedef struct FreeCase {
    double I1, I2, I3, w0[3], c[3], mass, vc0[3], p0[3];
    double q0[4];
} FreeCase;

static void free_reference(const FreeCase *fc, double t, double w[3]) {
    double I1 = fc->I1, I2 = fc->I2, I3 = fc->I3;
    double E2 = I1 * fc->w0[0] * fc->w0[0] + I2 * fc->w0[1] * fc->w0[1] + I3 * fc->w0[2] * fc->w0[2];
    double L2 = I1 * I1 * fc->w0[0] * fc->w0[0] + I2 * I2 * fc->w0[1] * fc->w0[1] + I3 * I3 * fc->w0[2] * fc->w0[2];
    double tau = t * sqrt((I3 - I2) * (L2 - E2 * I1) / (I1 * I2 * I3));
    double k2 = (I2 - I1) * (E2 * I3 - L2) / ((I3 - I2) * (L2 - E2 * I1));
    double sn, cn, dn;
    sncndn(tau, k2, &sn, &cn, &dn);
    w[0] = sqrt((E2 * I3 - L2) / (I1 * (I3 - I1))) * cn;
    w[1] = sqrt((E2 * I3 - L2) / (I2 * (I3 - I2))) * sn;
    w[2] = sqrt((L2 - E2 * I1) / (I3 * (I3 - I1))) * dn;
}

/* independent check of the reference: Euler's equations by classical RK4 with a tiny step */
static void euler_rk4(const FreeCase *fc, double t, double w[3]) {
    double y[3] = {fc->w0[0], fc->w0[1], fc->w0[2]}, h = 1e-5;
    int n = (int)llround(t / h);
    h = t / n;
    for (int i = 0; i < n; i++) {
        double k[4][3], yt[3];
        for (int s = 0; s < 4; s++) {
            const double cs[4] = {0, 0.5, 0.5, 1};
            for (int d = 0; d < 3; d++) yt[d] = y[d] + (s ? cs[s] * h * k[s - 1][d] : 0);
            k[s][0] = (fc->I2 - fc->I3) * yt[1] * yt[2] / fc->I1;
            k[s][1] = (fc->I3 - fc->I1) * yt[2] * yt[0] / fc->I2;
            k[s][2] = (fc->I1 - fc->I2) * yt[0] * yt[1] / fc->I3;
        }
        for (int d = 0; d < 3; d++) y[d] += h / 6 * (k[0][d] + 2 * k[1][d] + 2 * k[2][d] + k[3][d]);
    }
    memcpy(w, y, sizeof y);
}

static double free_body_error(const FreeCase *fc, double h, double T, double *com_err, double *L_err, double *E_err) {
    MbModelDef *def = mbdef_new();
    double I[9];
    diag3(I, fc->I1, fc->I2, fc->I3);
    int b = mbdef_add_body(def, "tumbler", fc->mass, fc->c, I);
    int j = mbdef_add_joint(def, "float", MB_FREE, -1, b);
    mv3_set(def->gravity, 0, 0, -9.81);
    MbJointDef *J = &def->joints[j];
    double R[9], Rc[3], wW[3], wxc[3], vO[3], vJ[3];
    memcpy(J->q0, fc->p0, 3 * sizeof(double));
    memcpy(J->q0 + 3, fc->q0, 4 * sizeof(double));
    mq_to_mat(R, fc->q0);
    mm3_mulv(Rc, R, fc->c);
    mm3_mulv(wW, R, fc->w0);
    mv3_cross(wxc, wW, Rc);
    mv3_sub(vO, fc->vc0, wxc);
    mm3_tmulv(vJ, R, vO);
    memcpy(J->v0, fc->w0, 3 * sizeof(double));
    memcpy(J->v0 + 3, vJ, 3 * sizeof(double));
    Run r;
    bool ok = run_open(&r, def, NULL);
    mbdef_free(def);
    if (!ok) {
        mdiag_print(&r.diag, "  ");
        run_close(&r);
        return INFINITY;
    }
    mb_evaluate(r.sim, r.s, NULL, r.ev);
    double L0[3], c0[3];
    /* angular momentum about the centre of mass = L_origin - c x P */
    double *mom = r.ev->momentum;
    MPose Tb;
    mb_body_pose(r.sim, r.s, 0, &Tb);
    mpose_apply(c0, &Tb, fc->c);
    double cxP[3];
    mv3_cross(cxP, c0, mom + 3);
    mv3_sub(L0, mom, cxP);
    ok = advance(&r, h, T, NULL);
    double err = INFINITY;
    if (ok) {
        mb_evaluate(r.sim, r.s, NULL, r.ev);
        mb_body_pose(r.sim, r.s, 0, &Tb);
        double wb[3], wref[3], c[3];
        mm3_tmulv(wb, Tb.R, r.ev->body_twist);
        free_reference(fc, T, wref);
        err = fmax(fabs(wb[0] - wref[0]), fmax(fabs(wb[1] - wref[1]), fabs(wb[2] - wref[2])));
        mpose_apply(c, &Tb, fc->c);
        double cref[3] = {fc->p0[0] + Rc[0] + fc->vc0[0] * T, fc->p0[1] + Rc[1] + fc->vc0[1] * T, fc->p0[2] + Rc[2] + fc->vc0[2] * T - 0.5 * 9.81 * T * T};
        *com_err = fmax(fabs(c[0] - cref[0]), fmax(fabs(c[1] - cref[1]), fabs(c[2] - cref[2])));
        mv3_cross(cxP, c, r.ev->momentum + 3);
        double Lc[3];
        mv3_sub(Lc, r.ev->momentum, cxP);
        *L_err = fmax(fabs(Lc[0] - L0[0]), fmax(fabs(Lc[1] - L0[1]), fabs(Lc[2] - L0[2])));
        *E_err = fabs(r.ev->energy_residual);
    }
    run_close(&r);
    return err;
}

static void test_free_body(void) {
    printf("== free body: parabolic flight of the centre of mass and torque-free rotation (Jacobi elliptic solution)\n");
    FreeCase fc = {.I1 = 0.01, .I2 = 0.02, .I3 = 0.03, .w0 = {4, 0, 3}, .c = {0.1, -0.05, 0.02}, .mass = 2, .vc0 = {1.5, -0.5, 4}, .p0 = {0, 0, 1}};
    double ax[3] = {0.3, 0.8, -0.52};
    mv3_normalize(ax);
    double th[3] = {ax[0] * 0.9, ax[1] * 0.9, ax[2] * 0.9};
    mq_exp(fc.q0, th);
    double wr[3], we[3];
    free_reference(&fc, 2.0, wr);
    euler_rk4(&fc, 2.0, we);
    double eref = fmax(fabs(wr[0] - we[0]), fmax(fabs(wr[1] - we[1]), fabs(wr[2] - we[2])));
    CHECK(eref < 1e-11, "elliptic reference agrees with a fine RK4 solution of Euler's equations (%.2e rad/s)", eref);
    double hs[3] = {4e-3, 2e-3, 1e-3}, errs[3], com = 0, L = 0, E = 0;
    for (int i = 0; i < 3; i++) {
        errs[i] = free_body_error(&fc, hs[i], 2.0, &com, &L, &E);
        REPORT("h = %.0e s: body rate error %.3e rad/s, centre-of-mass error %.2e m, |dL_com| %.2e kg m^2/s, energy residual %.2e J", hs[i], errs[i],
               com, L, E);
    }
    double p1 = log2(errs[0] / errs[1]), p2 = log2(errs[1] / errs[2]);
    REPORT("observed order %.2f, %.2f (RKMK4: 4)", p1, p2);
    CHECK(errs[2] < 1e-7, "rate error at h = 1e-3 s over 2 s (%.2e < 1e-7)", errs[2]);
    CHECK(p2 > 3.7 && p2 < 4.3, "fourth-order convergence of the rotation (%.2f)", p2);
    CHECK(com < 1e-9, "centre of mass follows the parabola; its offset from the body origin carries the O(h^4) attitude error (%.2e m)", com);
    CHECK(L < 1e-7 && E < 1e-7, "angular momentum about the centre of mass and energy conserved (%.2e, %.2e)", L, E);
}

/* ---------------------------------------------------------------------------------------------- pendulum */

typedef struct Pend {
    double m, d, Iyy, theta0;
} Pend;

static MbModelDef *pendulum_def(const Pend *p) {
    MbModelDef *def = mbdef_new();
    double I[9], com[3] = {0, 0, -p->d};
    diag3(I, 0.021, p->Iyy, 0.004); /* slender rod along z: admissible (0.02 + 0.004 >= 0.021) */
    int b = mbdef_add_body(def, "rod", p->m, com, I);
    int j = mbdef_add_joint(def, "pivot", MB_REVOLUTE, -1, b);
    mv3_set(def->joints[j].axis, 0, 1, 0);
    def->joints[j].q0[0] = p->theta0;
    mv3_set(def->gravity, 0, 0, -9.81);
    return def;
}

static double pendulum_theta(const Pend *p, double t) {
    double Ip = p->Iyy + p->m * p->d * p->d, w0 = sqrt(p->m * 9.81 * p->d / Ip), k = sin(p->theta0 / 2);
    double sn, cn, dn;
    sncndn(ellipk(k * k) - w0 * t, k * k, &sn, &cn, &dn);
    return 2 * asin(k * sn);
}

/* independent reference: classical RK4 on theta'' = -w0^2 sin(theta) with a tiny step */
static double pendulum_rk4(const Pend *p, double T) {
    double Ip = p->Iyy + p->m * p->d * p->d, w2 = p->m * 9.81 * p->d / Ip, y[2] = {p->theta0, 0};
    int n = 400000;
    double h = T / n;
    for (int i = 0; i < n; i++) {
        double k1[2] = {y[1], -w2 * sin(y[0])};
        double k2[2] = {y[1] + 0.5 * h * k1[1], -w2 * sin(y[0] + 0.5 * h * k1[0])};
        double k3[2] = {y[1] + 0.5 * h * k2[1], -w2 * sin(y[0] + 0.5 * h * k2[0])};
        double k4[2] = {y[1] + h * k3[1], -w2 * sin(y[0] + h * k3[0])};
        y[0] += h / 6 * (k1[0] + 2 * k2[0] + 2 * k3[0] + k4[0]);
        y[1] += h / 6 * (k1[1] + 2 * k2[1] + 2 * k3[1] + k4[1]);
    }
    return y[0];
}

static double pendulum_error(const Pend *p, double h, double T, double *E_err, double *period_err) {
    MbModelDef *def = pendulum_def(p);
    Run r;
    bool ok = run_open(&r, def, NULL);
    mbdef_free(def);
    double err = INFINITY;
    if (ok) {
        double Ip = p->Iyy + p->m * p->d * p->d, w0 = sqrt(p->m * 9.81 * p->d / Ip), k = sin(p->theta0 / 2);
        double period = 4 * ellipk(k * k) / w0;
        /* measure the time of the second downward zero crossing (t = 3/4 period) by linear interpolation */
        double tprev = 0, qprev = r.s->q[0], tcross = NAN;
        int crossings = 0;
        MbStepReport rep;
        err = 0;
        while (r.s->t < T - 1e-12) {
            if (!mb_step(r.sim, r.s, fmin(h, T - r.s->t), NULL, &rep)) {
                err = INFINITY;
                break;
            }
            double q = r.s->q[0];
            err = fmax(err, fabs(q - pendulum_theta(p, r.s->t)));
            if ((qprev > 0) != (q > 0)) {
                crossings++;
                if (crossings == 2) tcross = tprev + (r.s->t - tprev) * qprev / (qprev - q);
            }
            tprev = r.s->t, qprev = q;
        }
        (void)0;
        mb_evaluate(r.sim, r.s, NULL, r.ev);
        *E_err = fabs(r.ev->energy_residual);
        *period_err = fabs(tcross - 0.75 * period) / period;
    }
    run_close(&r);
    return err;
}

static void test_pendulum(void) {
    printf("== compound pendulum, 143 deg amplitude: elliptic-function solution and period 4K(k)/w0\n");
    Pend p = {1.5, 0.3, 0.02, 2.5};
    double ref_err = fabs(pendulum_rk4(&p, 3.0) - pendulum_theta(&p, 3.0));
    CHECK(ref_err < 1e-11, "elliptic reference agrees with a fine RK4 solution (%.2e rad)", ref_err);
    double hs[5] = {8e-3, 4e-3, 2e-3, 1e-3, 5e-4}, errs[5], E = 0, per = 0, per_fine = 0;
    for (int i = 0; i < 5; i++) {
        errs[i] = pendulum_error(&p, hs[i], 3.0, &E, &per);
        REPORT("h = %.0e s: max angle error over 3 s %.3e rad, energy residual %.2e J, period error (interpolated crossing) %.2e%s", hs[i], errs[i], E,
               per, i ? "" : "");
        if (i == 3) per_fine = per;
    }
    REPORT("observed orders %.2f, %.2f, %.2f, %.2f (pre-asymptotic at coarse steps: the swing nearly stops at 143 deg)", log2(errs[0] / errs[1]),
           log2(errs[1] / errs[2]), log2(errs[2] / errs[3]), log2(errs[3] / errs[4]));
    double order = log2(errs[3] / errs[4]);
    CHECK(errs[3] < 1e-9, "angle error at h = 1 ms (%.2e rad)", errs[3]);
    CHECK(order > 3.8 && order < 4.2, "fourth-order convergence in the asymptotic range (%.2f)", order);
    CHECK(per_fine < 1e-9, "period 4K(k)/w0 (%.2e relative)", per_fine);
}

/* ---------------------------------------------------------------------------------------------- damped oscillator */

static void test_damped_oscillator(void) {
    printf("== damped spring-mass on a prismatic joint: analytic response, energy ledger, support reaction\n");
    double m = 0.8, k = 50, c = 0.6, x0 = 0.05, g = 9.81;
    MbModelDef *def = mbdef_new();
    double I[9];
    diag3(I, 1e-3, 1e-3, 1e-3);
    int b = mbdef_add_body(def, "slider", m, (double[3]){0, 0, 0}, I);
    int j = mbdef_add_joint(def, "rail", MB_PRISMATIC, -1, b);
    MbJointDef *J = &def->joints[j];
    mv3_set(J->axis, 1, 0, 0);
    J->stiffness = k, J->damping = c, J->q0[0] = x0;
    mv3_set(def->gravity, 0, 0, -g);
    Run r;
    bool ok = run_open(&r, def, NULL);
    mbdef_free(def);
    CHECK(ok, "model assembles");
    if (!ok) {
        mdiag_print(&r.diag, "  ");
        run_close(&r);
        return;
    }
    double wn = sqrt(k / m), z = c / (2 * sqrt(k * m)), wd = wn * sqrt(1 - z * z);
    double emax = 0, T = 3;
    for (int i = 1; i <= 300; i++) {
        advance(&r, 1e-3, i * 0.01, NULL);
        double t = r.s->t, x = exp(-z * wn * t) * (x0 * cos(wd * t) + z * wn * x0 / wd * sin(wd * t));
        emax = fmax(emax, fabs(r.s->q[0] - x));
    }
    mb_evaluate(r.sim, r.s, NULL, r.ev);
    double Ediss = r.s->ledger.dissipated_damping, E0 = 0.5 * k * x0 * x0;
    double Enow = r.ev->kinetic + r.ev->potential_spring;
    REPORT("max displacement error over %.0f s: %.2e m; dissipated %.6f J of %.6f J; balance residual %.2e J", T, emax, Ediss, E0,
           r.ev->energy_residual);
    CHECK(emax < 1e-9, "displacement matches the underdamped solution (%.2e m)", emax);
    CHECK(fabs(E0 - Enow - Ediss) < 1e-9, "stored + dissipated energy = initial energy (%.2e J)", fabs(E0 - Enow - Ediss));
    /* reaction in the joint frame: the rail carries the weight (+z) and nothing else; the axial force is the spring+damper */
    const double *w = r.ev->joint_wrench;
    double axial = -k * r.s->q[0] - c * r.s->v[0];
    CHECK(fabs(w[5] - m * g) < 1e-12 && fabs(w[4]) < 1e-12 && fabs(w[3] - axial) < 1e-9, "joint wrench: weight %.6f N, axial %.6f N (spring+damper %.6f)",
          w[5], w[3], axial);
    run_close(&r);
}

/* ---------------------------------------------------------------------------------------------- two-link arm */

typedef struct TwoLink {
    double m1, m2, l1, r1, r2, I1, I2, g;
} TwoLink;

static void two_link_closed_form(const TwoLink *a, const double *q, const double *qd, const double *qdd, double tau[2], double M[4]) {
    double c2 = cos(q[1]), s2 = sin(q[1]);
    double M11 = a->I1 + a->I2 + a->m1 * a->r1 * a->r1 + a->m2 * (a->l1 * a->l1 + a->r2 * a->r2 + 2 * a->l1 * a->r2 * c2);
    double M12 = a->I2 + a->m2 * (a->r2 * a->r2 + a->l1 * a->r2 * c2);
    double M22 = a->I2 + a->m2 * a->r2 * a->r2;
    double h = a->m2 * a->l1 * a->r2 * s2;
    double G1 = (a->m1 * a->r1 + a->m2 * a->l1) * a->g * cos(q[0]) + a->m2 * a->r2 * a->g * cos(q[0] + q[1]);
    double G2 = a->m2 * a->r2 * a->g * cos(q[0] + q[1]);
    tau[0] = M11 * qdd[0] + M12 * qdd[1] - h * (2 * qd[0] * qd[1] + qd[1] * qd[1]) + G1;
    tau[1] = M12 * qdd[0] + M22 * qdd[1] + h * qd[0] * qd[0] + G2;
    if (M) M[0] = M11, M[1] = M12, M[2] = M12, M[3] = M22;
}

static MbModelDef *two_link_def(const TwoLink *a) {
    MbModelDef *def = mbdef_new();
    double I[9];
    diag3(I, 0.2 * a->I1, 1.1 * a->I1, a->I1);
    int b1 = mbdef_add_body(def, "upper", a->m1, (double[3]){a->r1, 0, 0}, I);
    diag3(I, 0.3 * a->I2, 1.2 * a->I2, a->I2);
    int b2 = mbdef_add_body(def, "fore", a->m2, (double[3]){a->r2, 0, 0}, I);
    mbdef_add_joint(def, "shoulder", MB_REVOLUTE, -1, b1);
    int j2 = mbdef_add_joint(def, "elbow", MB_REVOLUTE, b1, b2);
    mv3_set(def->joints[j2].parent_frame.p, a->l1, 0, 0);
    mv3_set(def->gravity, 0, -a->g, 0);
    return def;
}

static void test_two_link(void) {
    printf("== two-link planar arm: recursive Newton-Euler and CRBA against the closed-form Lagrange equations\n");
    TwoLink a = {1.2, 0.8, 0.45, 0.2, 0.17, 0.021, 0.011, 9.81};
    MbModelDef *def = two_link_def(&a);
    Run r;
    bool ok = run_open(&r, def, NULL);
    mbdef_free(def);
    CHECK(ok, "model assembles");
    if (!ok) {
        run_close(&r);
        return;
    }
    uint64_t seed = 12345;
    double etau = 0, eM = 0, efd = 0, ewr = 0, eres = 0;
    for (int trial = 0; trial < 50; trial++) {
        double q[2], qd[2], qdd[2], tau[2], Mref[4], tsim[2], M[4];
        for (int i = 0; i < 2; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            q[i] = ((seed >> 11) / 9007199254740992.0 * 2 - 1) * M_PI;
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            qd[i] = ((seed >> 11) / 9007199254740992.0 * 2 - 1) * 5;
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            qdd[i] = ((seed >> 11) / 9007199254740992.0 * 2 - 1) * 20;
        }
        two_link_closed_form(&a, q, qd, qdd, tau, Mref);
        double wr[12];
        mb_inverse_dynamics(r.sim, q, qd, qdd, NULL, tsim, wr);
        etau = fmax(etau, fmax(fabs(tsim[0] - tau[0]), fabs(tsim[1] - tau[1])));
        mb_mass_matrix(r.sim, q, M);
        for (int i = 0; i < 4; i++) eM = fmax(eM, fabs(M[i] - Mref[i]));
        /* forward dynamics recovers the accelerations */
        memcpy(r.s->q, q, sizeof q), memcpy(r.s->v, qd, sizeof qd);
        MbInputs in = {tau, NULL};
        mb_evaluate(r.sim, r.s, &in, r.ev);
        efd = fmax(efd, fmax(fabs(r.ev->qacc[0] - qdd[0]), fabs(r.ev->qacc[1] - qdd[1])));
        eres = fmax(eres, r.ev->joint_torque_residual);
        /* shoulder reaction = total rate of momentum minus gravity (Newton for the whole arm), in world axes */
        double s1 = sin(q[0]), c1 = cos(q[0]), s12 = sin(q[0] + q[1]), c12 = cos(q[0] + q[1]), w12 = qd[0] + qd[1], a12 = qdd[0] + qdd[1];
        double ac1[2] = {a.r1 * (-s1 * qdd[0] - c1 * qd[0] * qd[0]), a.r1 * (c1 * qdd[0] - s1 * qd[0] * qd[0])};
        double ac2[2] = {a.l1 * (-s1 * qdd[0] - c1 * qd[0] * qd[0]) + a.r2 * (-s12 * a12 - c12 * w12 * w12),
                         a.l1 * (c1 * qdd[0] - s1 * qd[0] * qd[0]) + a.r2 * (c12 * a12 - s12 * w12 * w12)};
        double F[2] = {a.m1 * ac1[0] + a.m2 * ac2[0], a.m1 * ac1[1] + a.m2 * ac2[1] + (a.m1 + a.m2) * a.g};
        /* reported in the child joint frame of the shoulder, which is rotated by q1 about z */
        double Fw[2] = {c1 * wr[3] - s1 * wr[4], s1 * wr[3] + c1 * wr[4]};
        ewr = fmax(ewr, fmax(fabs(Fw[0] - F[0]), fabs(Fw[1] - F[1])));
    }
    REPORT("50 random states: max |tau - tau_ref| %.2e N m, |M - M_ref| %.2e kg m^2, |qdd_FD - qdd| %.2e rad/s^2, shoulder force %.2e N", etau, eM,
           efd, ewr);
    CHECK(etau < 1e-11, "inverse dynamics torques (%.2e N m)", etau);
    CHECK(eM < 1e-14, "mass matrix (%.2e)", eM);
    CHECK(efd < 1e-10, "forward dynamics inverts inverse dynamics (%.2e)", efd);
    CHECK(ewr < 1e-11, "shoulder reaction force equals Newton's law for the whole arm (%.2e N)", ewr);
    CHECK(eres < 1e-11, "joint wrenches project onto the applied joint torques (%.2e)", eres);
    /* passive double pendulum: energy conservation and convergence */
    double hs[2] = {2e-3, 1e-3}, errE[2];
    for (int i = 0; i < 2; i++) {
        def = two_link_def(&a);
        def->joints[0].q0[0] = 0.3, def->joints[1].q0[0] = 1.2;
        def->joints[0].v0[0] = 2.0;
        Run p;
        run_open(&p, def, NULL);
        mbdef_free(def);
        advance(&p, hs[i], 5.0, NULL);
        mb_evaluate(p.sim, p.s, NULL, p.ev);
        errE[i] = fabs(p.ev->energy_residual);
        run_close(&p);
    }
    REPORT("chaotic double pendulum, 5 s: energy residual %.2e J (h = 2 ms), %.2e J (h = 1 ms), ratio %.1f", errE[0], errE[1], errE[0] / errE[1]);
    CHECK(errE[1] < 1e-6 && errE[0] / errE[1] > 10, "energy error small and converging (%.2e J)", errE[1]);
}

/* ---------------------------------------------------------------------------------------------- joint limits */

static void test_limits(void) {
    printf("== joint limits: event location, Newton restitution, persistent contact and its reaction, release\n");
    Pend p = {1.5, 0.3, 0.02, 1.0};
    double Ip = p.Iyy + p.m * p.d * p.d, w02 = p.m * 9.81 * p.d / Ip;
    /* elastic-ish rebound at the lower limit */
    MbModelDef *def = pendulum_def(&p);
    MbJointDef *J = &def->joints[0];
    J->limited = true, J->lower = -0.5, J->upper = 1.5, J->restitution = 0.6;
    Run r;
    bool ok = run_open(&r, def, NULL);
    mbdef_free(def);
    CHECK(ok, "limited pendulum assembles");
    if (!ok) {
        mdiag_print(&r.diag, "  ");
        run_close(&r);
        return;
    }
    double vbefore = -sqrt(2 * w02 * (cos(-0.5) - cos(p.theta0)));
    double qmin = 10;
    bool first = true;
    MbStepReport rep;
    for (int i = 0; i < 2000 && ok; i++) {
        if (!mb_step(r.sim, r.s, 1e-3, NULL, &rep)) break;
        qmin = fmin(qmin, r.s->q[0]);
        if (rep.events && first) {
            first = false;
            /* the step continues after the impact, so the rebound speed comes from the kinetic energy the impact removed */
            double lost = r.s->ledger.dissipated_impacts;
            double Tbefore = 0.5 * Ip * vbefore * vbefore;
            double ratio = sqrt((Tbefore - lost) / Tbefore);
            REPORT("impact located: kinetic energy before %.6f J, dissipated %.6f J, rebound speed ratio %.9f (restitution 0.6)", Tbefore, lost, ratio);
            CHECK(fabs(ratio - 0.6) < 1e-8, "rebound speed ratio equals the restitution coefficient (%.3e)", fabs(ratio - 0.6));
        }
    }
    CHECK(qmin >= -0.5 - 1e-9, "limit never penetrated beyond tolerance (min %.12f rad)", qmin);
    mb_evaluate(r.sim, r.s, NULL, r.ev);
    CHECK(fabs(r.ev->energy_residual) < 1e-8, "energy balance including impact dissipation (%.2e J)", r.ev->energy_residual);
    run_close(&r);

    /* plastic contact held by gravity: the limit reaction equals the static gravity torque */
    def = pendulum_def(&p);
    J = &def->joints[0];
    J->limited = true, J->lower = 0.4, J->upper = 2.0, J->restitution = 0;
    ok = run_open(&r, def, NULL);
    mbdef_free(def);
    CHECK(ok, "plastic-limit pendulum assembles");
    if (!ok) {
        run_close(&r);
        return;
    }
    advance(&r, 1e-3, 2.0, NULL);
    mb_evaluate(r.sim, r.s, NULL, r.ev);
    double hold = p.m * 9.81 * p.d * sin(0.4);
    REPORT("resting on the lower limit: q = %.12f rad, limit torque %.9f N m (static %.9f), state %d", r.s->q[0], r.ev->tau_constraint[0], hold,
           r.s->limit_state[0]);
    CHECK(r.s->limit_state[0] == -1 && fabs(r.s->q[0] - 0.4) < 1e-10, "plastic impact leaves the joint resting on the limit");
    CHECK(fabs(r.ev->tau_constraint[0] - hold) < 1e-9 && fabs(r.ev->qacc[0]) < 1e-9, "limit reaction equals m g d sin(q) (%.2e N m)",
          fabs(r.ev->tau_constraint[0] - hold));
    /* a torque larger than the gravity torque lifts the joint off the limit */
    double tau[1] = {hold * 1.5};
    MbInputs in = {tau, NULL};
    advance(&r, 1e-3, 2.5, &in);
    CHECK(r.s->limit_state[0] == 0 && r.s->q[0] > 0.41, "contact released when the input pulls away (q = %.4f rad)", r.s->q[0]);
    run_close(&r);

    /* an initial configuration outside the limits is an error, not a clamp */
    def = pendulum_def(&p);
    J = &def->joints[0];
    J->limited = true, J->lower = -0.2, J->upper = 0.2;
    ok = run_open(&r, def, NULL);
    mbdef_free(def);
    CHECK(!ok && mdiag_has(&r.diag, "IMPOSSIBLE_INITIAL_CONFIGURATION"), "start outside the limits is rejected");
    run_close(&r);
}

/* ---------------------------------------------------------------------------------------------- four-bar */

typedef struct FourBar {
    double a, b, c, d; /* crank, coupler, rocker, ground */
    double m[3];
} FourBar;

/* closed-form position analysis (branch with C above the ground line) */
static bool fourbar_solve(const FourBar *f, double th2, double *th3rel, double *th4, double C[2]) {
    double Bx = f->a * cos(th2), By = f->a * sin(th2);
    double dx = f->d - Bx, dy = -By, dist = hypot(dx, dy);
    if (dist > f->b + f->c || dist < fabs(f->b - f->c)) return false;
    double along = (f->b * f->b - f->c * f->c + dist * dist) / (2 * dist);
    double h = sqrt(fmax(0, f->b * f->b - along * along));
    double ux = dx / dist, uy = dy / dist;
    double Cx1 = Bx + along * ux - h * uy, Cy1 = By + along * uy + h * ux;
    double Cx2 = Bx + along * ux + h * uy, Cy2 = By + along * uy - h * ux;
    double Cx = Cy1 >= Cy2 ? Cx1 : Cx2, Cy = Cy1 >= Cy2 ? Cy1 : Cy2;
    *th3rel = atan2(Cy - By, Cx - Bx) - th2;
    *th4 = atan2(Cy, Cx - f->d);
    if (C) C[0] = Cx, C[1] = Cy;
    return true;
}

static MbModelDef *fourbar_def(const FourBar *f, double th2, MbJointType loop_type) {
    MbModelDef *def = mbdef_new();
    double len[3] = {f->a, f->b, f->c};
    const char *names[3] = {"crank", "coupler", "rocker"};
    int body[3];
    for (int i = 0; i < 3; i++) {
        double I[9], Izz = f->m[i] * len[i] * len[i] / 12;
        diag3(I, 1e-4 * Izz, Izz, Izz);
        body[i] = mbdef_add_body(def, names[i], f->m[i], (double[3]){len[i] / 2, 0, 0}, I);
    }
    double th3, th4;
    fourbar_solve(f, th2, &th3, &th4, NULL);
    int jA = mbdef_add_joint(def, "A", MB_REVOLUTE, -1, body[0]);
    int jB = mbdef_add_joint(def, "B", MB_REVOLUTE, body[0], body[1]);
    int jD = mbdef_add_joint(def, "D", MB_REVOLUTE, -1, body[2]);
    int jC = mbdef_add_joint(def, "C", loop_type, body[1], body[2]);
    mv3_set(def->joints[jB].parent_frame.p, f->a, 0, 0);
    mv3_set(def->joints[jD].parent_frame.p, f->d, 0, 0);
    mv3_set(def->joints[jC].parent_frame.p, f->b, 0, 0);
    mv3_set(def->joints[jC].child_frame.p, f->c, 0, 0);
    def->joints[jA].q0[0] = th2;
    def->joints[jB].q0[0] = th3;
    def->joints[jD].q0[0] = th4;
    if (loop_type == MB_SPHERICAL) def->joints[jC].q0[0] = 1;
    mv3_set(def->gravity, 0, 0, 0);
    return def;
}

static void test_fourbar(void) {
    printf("== four-bar linkage (closed loop): assembly, redundancy, Freudenstein kinematics, energy, static reactions\n");
    FourBar f = {0.1, 0.35, 0.2, 0.3, {0.2, 0.5, 0.3}};
    double th2 = M_PI / 3;
    MbModelDef *def = fourbar_def(&f, th2, MB_REVOLUTE);
    def->joints[2].q0[0] += 0.05; /* rocker 2.9 deg off: assembly must close the loop and say so */
    def->joints[0].v0[0] = 10;    /* crank rate only: velocities made consistent and reported */
    Run r;
    bool ok = run_open(&r, def, NULL);
    mbdef_free(def);
    CHECK(ok, "four-bar assembles");
    if (!ok) {
        mdiag_print(&r.diag, "  ");
        run_close(&r);
        return;
    }
    mdiag_print(&r.diag, "  diag: ");
    CHECK(mdiag_has(&r.diag, "LOOP_CLOSURE"), "loop-closing joint identified");
    CHECK(mdiag_has(&r.diag, "INITIAL_CONFIGURATION_ADJUSTED") && mdiag_has(&r.diag, "INITIAL_VELOCITY_ADJUSTED"), "adjustments reported");
    CHECK(mdiag_has(&r.diag, "REDUNDANT_CONSTRAINTS") && r.sim && 1, "planar loop of revolute joints reported as over-constrained");
    MbStepReport rep;
    mb_step(r.sim, r.s, 1e-4, NULL, &rep);
    CHECK(rep.cinfo.redundant == 3 && rep.cinfo.mobility == 1, "3 redundant rows, mobility 1 (got %d, %d)", rep.cinfo.redundant, rep.cinfo.mobility);
    mb_evaluate(r.sim, r.s, NULL, r.ev);
    double E0 = r.ev->kinetic;
    double kin_err = 0, drift = 0;
    for (int i = 1; i <= 100; i++) {
        if (!advance(&r, 1e-3, 0.01 * i, NULL)) break;
        double t3, t4;
        fourbar_solve(&f, r.s->q[0], &t3, &t4, NULL);
        double d3 = remainder(r.s->q[1] - t3, 2 * M_PI), d4 = remainder(r.s->q[2] - t4, 2 * M_PI);
        kin_err = fmax(kin_err, fmax(fabs(d3), fabs(d4)));
        drift = fmax(drift, mb_constraint_residual(r.sim, r.s));
    }
    mb_evaluate(r.sim, r.s, NULL, r.ev);
    REPORT("1 s of motion (crank turned %.1f rev): max angle error vs closed form %.2e rad, max residual %.2e, energy change %.2e J of %.4f J", r.s->q[0] / (2 * M_PI),
           kin_err, drift, r.ev->kinetic - E0, E0);
    CHECK(kin_err < 1e-9, "coupler and rocker angles follow the closed-form position analysis (%.2e rad)", kin_err);
    CHECK(drift < 1e-10, "loop residual after projection (%.2e)", drift);
    CHECK(fabs(r.ev->energy_residual) < 1e-6 * E0, "passive linkage conserves energy (%.2e J)", r.ev->energy_residual);
    run_close(&r);

    /* static equilibrium under gravity: crank torque from virtual work, and reactions balancing the weight */
    def = fourbar_def(&f, th2, MB_REVOLUTE);
    mv3_set(def->gravity, 0, -9.81, 0);
    ok = run_open(&r, def, NULL);
    mbdef_free(def);
    double h = 1e-6, U[2];
    for (int s = 0; s < 2; s++) {
        double t2 = th2 + (s ? h : -h), t3, t4, C[2];
        fourbar_solve(&f, t2, &t3, &t4, C);
        double y1 = f.a / 2 * sin(t2), y2 = f.a * sin(t2) + f.b / 2 * sin(t2 + t3), y3 = f.c / 2 * sin(t4);
        U[s] = 9.81 * (f.m[0] * y1 + f.m[1] * y2 + f.m[2] * y3);
    }
    double tau_ref = (U[1] - U[0]) / (2 * h);
    double tau[3] = {tau_ref, 0, 0};
    MbInputs in = {tau, NULL};
    mb_evaluate(r.sim, r.s, &in, r.ev);
    double amax = fmax(fabs(r.ev->qacc[0]), fmax(fabs(r.ev->qacc[1]), fabs(r.ev->qacc[2])));
    /* ground reactions (joints A and D, reported in their child frames) + weight = 0 */
    MPose TA, TD;
    mb_body_pose(r.sim, r.s, 0, &TA);
    mb_body_pose(r.sim, r.s, 2, &TD);
    double FA[3], FD[3];
    mm3_mulv(FA, TA.R, r.ev->joint_wrench + 3);
    mm3_mulv(FD, TD.R, r.ev->joint_wrench + 6 * 2 + 3);
    double W = 9.81 * (f.m[0] + f.m[1] + f.m[2]);
    double sum[3] = {FA[0] + FD[0], FA[1] + FD[1] - W, FA[2] + FD[2]};
    REPORT("holding torque from virtual work %.9f N m: accelerations %.2e rad/s^2, ground reactions + weight = (%.2e, %.2e, %.2e) N", tau_ref, amax,
           sum[0], sum[1], sum[2]);
    CHECK(amax < 1e-6, "virtual-work torque holds the linkage static (%.2e)", amax);
    CHECK(fabs(sum[0]) < 1e-9 && fabs(sum[1]) < 1e-9 && fabs(sum[2]) < 1e-9, "ground reactions balance the weight");
    CHECK(r.ev->joint_torque_residual < 1e-10, "tree joint wrenches consistent with the loop multipliers (%.2e)", r.ev->joint_torque_residual);
    run_close(&r);

    /* spherical loop joint: one redundant row (the out-of-plane force) instead of three */
    def = fourbar_def(&f, th2, MB_SPHERICAL);
    ok = run_open(&r, def, NULL);
    mbdef_free(def);
    mb_step(r.sim, r.s, 1e-4, NULL, &rep);
    CHECK(ok && rep.cinfo.redundant == 1 && rep.cinfo.mobility == 1, "spherical loop joint: 1 redundant row, mobility 1 (got %d, %d)",
          rep.cinfo.redundant, rep.cinfo.mobility);
    run_close(&r);

    /* parallelogram at the change point (all joints collinear): singular configuration */
    FourBar pg = {0.1, 0.3, 0.1, 0.3, {0.2, 0.5, 0.2}};
    def = mbdef_new();
    {
        MbModelDef *tmp = fourbar_def(&pg, 0.3, MB_REVOLUTE);
        mbdef_free(def);
        def = tmp;
        def->joints[0].q0[0] = 0, def->joints[1].q0[0] = 0, def->joints[2].q0[0] = 0;
    }
    ok = run_open(&r, def, NULL);
    mbdef_free(def);
    CHECK(mdiag_has(&r.diag, "SINGULAR_CONFIGURATION"), "change-point configuration of a parallelogram flagged as singular");
    run_close(&r);

    /* link lengths that cannot close */
    FourBar bad = {0.1, 0.1, 0.1, 0.5, {0.2, 0.2, 0.2}};
    def = fourbar_def(&f, th2, MB_REVOLUTE);
    def->joints[1].parent_frame.p[0] = bad.a;
    def->joints[2].parent_frame.p[0] = bad.d;
    def->joints[3].parent_frame.p[0] = bad.b;
    def->joints[3].child_frame.p[0] = bad.c;
    ok = run_open(&r, def, NULL);
    mbdef_free(def);
    CHECK(!ok && mdiag_has(&r.diag, "LOOP_CLOSURE_FAILED"), "impossible loop reported as LOOP_CLOSURE_FAILED");
    run_close(&r);
}

/* ---------------------------------------------------------------------------------------------- gear coupling */

static void test_coupling(void) {
    printf("== gear coupling: reflected inertia and ratio constraint\n");
    MbModelDef *def = mbdef_new();
    double I1[9], I2[9];
    diag3(I1, 2e-5, 2e-5, 4e-5); /* rotor about z: 4e-5 */
    diag3(I2, 3e-4, 3e-4, 6e-4);
    int b1 = mbdef_add_body(def, "pinion", 0.05, (double[3]){0, 0, 0}, I1);
    int b2 = mbdef_add_body(def, "gear", 0.3, (double[3]){0, 0, 0}, I2);
    int j1 = mbdef_add_joint(def, "motor", MB_REVOLUTE, -1, b1);
    int j2 = mbdef_add_joint(def, "output", MB_REVOLUTE, -1, b2);
    mv3_set(def->joints[j2].parent_frame.p, 0.05, 0, 0);
    def->joints[j1].motion = MB_ACTUATED;
    mbdef_add_coupling(def, "mesh", j2, j1, -0.25, 0);
    Run r;
    bool ok = run_open(&r, def, NULL);
    mbdef_free(def);
    CHECK(ok, "geared pair assembles");
    double tau[2] = {0.01, 0};
    MbInputs in = {tau, NULL};
    mb_evaluate(r.sim, r.s, &in, r.ev);
    double Jeff = 4e-5 + 0.25 * 0.25 * 6e-4, alpha = 0.01 / Jeff;
    CHECK(fabs(r.ev->qacc[0] - alpha) < 1e-9 * alpha && fabs(r.ev->qacc[1] + 0.25 * alpha) < 1e-9 * alpha,
          "motor acceleration tau/(J1 + r^2 J2) = %.6f rad/s^2 (got %.6f), output -r times it", alpha, r.ev->qacc[0]);
    advance(&r, 1e-3, 1.0, &in);
    mb_evaluate(r.sim, r.s, &in, r.ev);
    CHECK(fabs(r.s->q[1] + 0.25 * r.s->q[0]) < 1e-10, "ratio held over 1 s (%.2e rad)", fabs(r.s->q[1] + 0.25 * r.s->q[0]));
    CHECK(fabs(r.ev->energy_residual) < 1e-10 * r.s->ledger.work_inputs, "input work equals kinetic energy (%.2e J of %.4f J)", r.ev->energy_residual,
          r.s->ledger.work_inputs);
    run_close(&r);
}

/* ---------------------------------------------------------------------------------------------- determinism */

static void test_determinism(void) {
    printf("== determinism and rollback: identical runs are bitwise equal; a restored state replays exactly\n");
    TwoLink a = {1.2, 0.8, 0.45, 0.2, 0.17, 0.021, 0.011, 9.81};
    Run r1, r2;
    MbModelDef *def = two_link_def(&a);
    def->joints[0].q0[0] = 0.3, def->joints[1].q0[0] = 1.2, def->joints[0].v0[0] = 2;
    run_open(&r1, def, NULL);
    run_open(&r2, def, NULL);
    mbdef_free(def);
    advance(&r1, 1e-3, 1.0, NULL);
    advance(&r2, 1e-3, 1.0, NULL);
    CHECK(!memcmp(r1.s->q, r2.s->q, 2 * sizeof(double)) && !memcmp(r1.s->v, r2.s->v, 2 * sizeof(double)), "two runs are bitwise identical");
    MbState *saved = mb_state_new(r1.m);
    mb_state_copy(saved, r1.s);
    advance(&r1, 1e-3, 2.0, NULL);
    mb_state_copy(r2.s, saved);
    advance(&r2, 1e-3, 2.0, NULL);
    CHECK(!memcmp(r1.s->q, r2.s->q, 2 * sizeof(double)) && r1.s->ledger.projection_change == r2.s->ledger.projection_change,
          "rollback to a saved state reproduces the continuation bitwise");
    mb_state_free(saved);
    run_close(&r1);
    run_close(&r2);
}

/* ---------------------------------------------------------------------------------------------- fixed joint */

static void test_fixed_and_validation(void) {
    printf("== fixed joint reaction and model validation\n");
    MbModelDef *def = mbdef_new();
    double I[9];
    diag3(I, 0.01, 0.02, 0.025);
    int b = mbdef_add_body(def, "bracket", 3, (double[3]){0.2, 0.1, 0}, I);
    int base = mbdef_add_body(def, "arm", 1, (double[3]){0, 0, 0}, I);
    int j0 = mbdef_add_joint(def, "hinge", MB_REVOLUTE, -1, base);
    mv3_set(def->joints[j0].axis, 0, 0, 1);
    int jf = mbdef_add_joint(def, "bolted", MB_FIXED, base, b);
    mv3_set(def->joints[jf].parent_frame.p, 0.5, 0, 0);
    mv3_set(def->gravity, 0, 0, -9.81);
    Run r;
    bool ok = run_open(&r, def, NULL);
    CHECK(ok, "fixed joint assembles");
    mb_evaluate(r.sim, r.s, NULL, r.ev);
    const double *w = r.ev->joint_wrench + 6 * jf;
    /* static: the bolt carries the bracket weight 3 g at the centre of mass (0.2, 0.1, 0) relative to the joint frame */
    double Fz = 3 * 9.81, Mx = 0.1 * Fz, My = -0.2 * Fz;
    CHECK(fabs(w[5] - Fz) < 1e-12 && fabs(w[3]) < 1e-12 && fabs(w[4]) < 1e-12, "bolt force equals the weight (%.9f N)", w[5]);
    CHECK(fabs(w[0] - Mx) < 1e-12 && fabs(w[1] - My) < 1e-12 && fabs(w[2]) < 1e-12, "bolt moment r x W = (%.6f, %.6f) N m", w[0], w[1]);
    run_close(&r);
    /* validation: inadmissible inertia, zero mass, disconnected body */
    double bad[9];
    diag3(bad, 0.01, 0.01, 0.05);
    def->bodies[0].inertia[0] = bad[0], def->bodies[0].inertia[4] = bad[4], def->bodies[0].inertia[8] = bad[8];
    def->bodies[1].mass = 0;
    mbdef_add_body(def, "loose", 1, NULL, I);
    MechDiag d;
    mdiag_init(&d);
    MbModel *m = mb_compile(def, NULL, &d);
    CHECK(!m && mdiag_has(&d, "INERTIA_NOT_ADMISSIBLE") && mdiag_has(&d, "MASS_NOT_POSITIVE"), "inadmissible inertia and zero mass rejected");
    mb_model_free(m);
    mdiag_free(&d);
    def->bodies[0].inertia[0] = 0.01, def->bodies[0].inertia[4] = 0.02, def->bodies[0].inertia[8] = 0.025, def->bodies[1].mass = 1;
    mdiag_init(&d);
    m = mb_compile(def, NULL, &d);
    CHECK(!m && mdiag_has(&d, "BODY_NOT_CONNECTED"), "body without a joint to the world is not silently freed");
    mb_model_free(m);
    mdiag_free(&d);
    mbdef_free(def);
}


/* ---------------------------------------------------------------------------------------------- mass properties */

static const int BOX_TRI[36] = {0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4, 2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5};

/* box [0,a]x[0,b]x[0,c] transformed by T; vertex i has bits x = i&1, y = i&2, z = i&4 */
static void box_mesh(double a, double b, double c, const MPose *T, double *v) {
    for (int i = 0; i < 8; i++) {
        double p[3] = {(i & 1) ? a : 0, (i & 2) ? b : 0, (i & 4) ? c : 0};
        mpose_apply(v + 3 * i, T, p);
    }
}

typedef struct IcoMesh {
    double *v;
    int *t;
    int nv, nt;
} IcoMesh;

static int ico_midpoint(IcoMesh *m, int a, int b, int *keys, int *vals, int cap, double r) {
    int lo = a < b ? a : b, hi = a < b ? b : a;
    unsigned h = (unsigned)(lo * 73856093u) ^ (unsigned)(hi * 19349663u);
    for (int i = 0; i < cap; i++) {
        int slot = (int)((h + (unsigned)i) % (unsigned)cap);
        if (keys[2 * slot] < 0) {
            double *p = m->v + 3 * m->nv;
            for (int k = 0; k < 3; k++) p[k] = 0.5 * (m->v[3 * a + k] + m->v[3 * b + k]);
            double n = mv3_norm(p);
            for (int k = 0; k < 3; k++) p[k] *= r / n;
            keys[2 * slot] = lo, keys[2 * slot + 1] = hi, vals[slot] = m->nv;
            return m->nv++;
        }
        if (keys[2 * slot] == lo && keys[2 * slot + 1] == hi) return vals[slot];
    }
    return -1;
}

static IcoMesh icosphere(double r, int levels) {
    IcoMesh m = {0};
    int nt = 20;
    for (int l = 0; l < levels; l++) nt *= 4;
    int nvmax = nt / 2 + 2;
    m.v = malloc((size_t)(3 * nvmax) * sizeof(double));
    m.t = malloc((size_t)(3 * nt) * sizeof(int));
    double g = (1 + sqrt(5)) / 2;
    double base[12][3] = {{-1, g, 0}, {1, g, 0}, {-1, -g, 0}, {1, -g, 0}, {0, -1, g}, {0, 1, g}, {0, -1, -g}, {0, 1, -g}, {g, 0, -1}, {g, 0, 1}, {-g, 0, -1}, {-g, 0, 1}};
    static const int F[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                 {3, 9, 4}, {3, 4, 2},  {3, 2, 6}, {3, 6, 8}, {3, 8, 9},   {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int i = 0; i < 12; i++) {
        double n = mv3_norm(base[i]);
        for (int k = 0; k < 3; k++) m.v[3 * i + k] = base[i][k] * r / n;
    }
    m.nv = 12, m.nt = 20;
    for (int i = 0; i < 20; i++)
        for (int k = 0; k < 3; k++) m.t[3 * i + k] = F[i][k];
    for (int l = 0; l < levels; l++) {
        int cap = 4 * m.nt + 7;
        int *keys = malloc((size_t)(2 * cap) * sizeof(int)), *vals = malloc((size_t)cap * sizeof(int));
        for (int i = 0; i < 2 * cap; i++) keys[i] = -1;
        int *nt2 = malloc((size_t)(12 * m.nt) * sizeof(int));
        int out = 0;
        for (int i = 0; i < m.nt; i++) {
            int a = m.t[3 * i], b = m.t[3 * i + 1], c = m.t[3 * i + 2];
            int ab = ico_midpoint(&m, a, b, keys, vals, cap, r), bc = ico_midpoint(&m, b, c, keys, vals, cap, r), ca = ico_midpoint(&m, c, a, keys, vals, cap, r);
            int T[4][3] = {{a, ab, ca}, {b, bc, ab}, {c, ca, bc}, {ab, bc, ca}};
            for (int q = 0; q < 4; q++)
                for (int k = 0; k < 3; k++) nt2[out++] = T[q][k];
        }
        memcpy(m.t, nt2, (size_t)out * sizeof(int));
        m.nt = out / 3;
        free(keys), free(vals), free(nt2);
    }
    return m;
}

static void test_mass_properties(void) {
    printf("== mass properties: closed-mesh checks, polyhedral integrals, parallel axis, cavities, printed fill models\n");
    MPose I;
    mpose_identity(&I);
    double v[8 * 3];
    MassProperties mp;
    MechDiag d;
    mdiag_init(&d);
    /* unit cube, unit density */
    box_mesh(1, 1, 1, &I, v);
    FillSpec solid = {.model = FILL_SOLID, .density = 1};
    bool ok = mass_properties_from_mesh(v, 8, BOX_TRI, 12, &solid, &mp, &d, "cube");
    double e = fmax(fabs(mp.mass - 1), fmax(fabs(mp.com[0] - 0.5), fmax(fabs(mp.inertia_com[0] - 1.0 / 6), fabs(mp.inertia_com[1]))));
    CHECK(ok && e < 1e-15, "unit cube: V = 1, centre (0.5, 0.5, 0.5), I = 1/6 (%.2e)", e);
    /* rotated, translated PLA box */
    double a = 0.08, b = 0.05, c = 0.012, rho = 1240;
    MPose T;
    double th[3] = {0.4, -0.7, 1.1}, q[4];
    mq_exp(q, th);
    mq_to_mat(T.R, q);
    mv3_set(T.p, 0.3, -0.2, 0.15);
    box_mesh(a, b, c, &T, v);
    solid.density = rho;
    mdiag_clear(&d);
    ok = mass_properties_from_mesh(v, 8, BOX_TRI, 12, &solid, &mp, &d, "plate");
    double m = rho * a * b * c, loc[3] = {a / 2, b / 2, c / 2}, cref[3];
    double Iloc[9] = {m / 12 * (b * b + c * c), 0, 0, 0, m / 12 * (a * a + c * c), 0, 0, 0, m / 12 * (a * a + b * b)}, Iref[9];
    mpose_apply(cref, &T, loc);
    inertia_rotate(T.R, Iloc, Iref);
    double eI = 0, ec = 0;
    for (int k = 0; k < 9; k++) eI = fmax(eI, fabs(mp.inertia_com[k] - Iref[k]));
    for (int k = 0; k < 3; k++) ec = fmax(ec, fabs(mp.com[k] - cref[k]));
    CHECK(ok && fabs(mp.mass - m) < 1e-15 && ec < 1e-15 && eI < 1e-15 * m, "rotated box: mass %.9f kg, centre error %.1e m, inertia error %.1e kg m^2",
          mp.mass, ec, eI);
    /* parallel axis round trip about an arbitrary point */
    double P[3] = {1.0, 2.0, -0.5}, Ip[9], Ic[9];
    inertia_about_point(mp.inertia_com, mp.mass, mp.com, P, Ip);
    inertia_to_com(Ip, mp.mass, mp.com, P, Ic);
    e = 0;
    for (int k = 0; k < 9; k++) e = fmax(e, fabs(Ic[k] - mp.inertia_com[k]));
    CHECK(e < 1e-15, "parallel-axis theorem round trip (%.1e)", e);
    /* principal axes reproduce the rotation of the box axes */
    double lam[3], ax[9];
    inertia_principal(mp.inertia_com, lam, ax);
    CHECK(fabs(lam[0] - Iloc[8]) < 1e-12 * m && fabs(lam[2] - Iloc[0]) < 1e-12 * m && mm3_det(ax) > 0, "principal moments and right-handed axes");
    /* tetrahedron moment integrals */
    double tv[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    int tt[12] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
    MomentIntegrals mi;
    mesh_volume_integrals(tv, tt, 4, &mi);
    CHECK(fabs(mi.zeroth - 1.0 / 6) < 1e-16 && fabs(mi.first[0] - 1.0 / 24) < 1e-16 && fabs(mi.second[0] - 1.0 / 60) < 1e-16 &&
              fabs(mi.second[3] - 1.0 / 120) < 1e-16,
          "tetrahedron: V = 1/6, int x = 1/24, int x^2 = 1/60, int xy = 1/120");
    /* icosphere: inertia of the polyhedron converges to 2/5 m r^2 */
    double prev = 0, ratio = 0, err = 0;
    for (int lv = 2; lv <= 5; lv++) {
        IcoMesh s = icosphere(0.05, lv);
        mdiag_clear(&d);
        mass_properties_from_mesh(s.v, s.nv, s.t, s.nt, &solid, &mp, &d, "sphere");
        err = fabs(mp.principal[0] / (0.4 * mp.mass * 0.05 * 0.05) - 1);
        double spread = (mp.principal[0] - mp.principal[2]) / mp.principal[0];
        REPORT("icosphere %6d triangles: I/(2/5 m r^2) - 1 = %.3e, anisotropy %.1e, |com| %.1e m", s.nt, err, spread, mv3_norm(mp.com));
        if (prev > 0) ratio = prev / err;
        prev = err;
        free(s.v), free(s.t);
    }
    CHECK(err < 5e-4 && ratio > 3.8 && ratio < 4.2, "second-order convergence of the discretised sphere (polyhedron error %.2e, ratio %.2f)", err, ratio);
    /* hollow box: outer 100 mm cube with an inward-facing 60 mm cavity */
    double hv[16 * 3];
    int ht[24 * 3];
    MPose To = I, Ti = I;
    mv3_set(Ti.p, 0.02, 0.02, 0.02);
    box_mesh(0.1, 0.1, 0.1, &To, hv);
    box_mesh(0.06, 0.06, 0.06, &Ti, hv + 24);
    for (int k = 0; k < 36; k++) ht[k] = BOX_TRI[k];
    for (int t = 0; t < 12; t++) /* inner shell reversed */
        ht[36 + 3 * t] = BOX_TRI[3 * t] + 8, ht[36 + 3 * t + 1] = BOX_TRI[3 * t + 2] + 8, ht[36 + 3 * t + 2] = BOX_TRI[3 * t + 1] + 8;
    solid.density = 1000;
    mdiag_clear(&d);
    ok = mass_properties_from_mesh(hv, 16, ht, 24, &solid, &mp, &d, "hollow");
    double mo = 1000 * 1e-3, mc = 1000 * 0.06 * 0.06 * 0.06, Ihol = mo / 6 * 0.01 - mc / 6 * 0.0036;
    CHECK(ok && fabs(mp.mass - (mo - mc)) < 1e-15 && fabs(mp.inertia_com[0] - Ihol) < 1e-16 && mdiag_has(&d, "MESH_CAVITIES"),
          "cavity subtracts mass %.6f kg and inertia %.6e kg m^2 (reported as explicit internal geometry)", mp.mass, mp.inertia_com[0]);
    /* invalid meshes */
    box_mesh(1, 1, 1, &I, v);
    mdiag_clear(&d);
    CHECK(!mass_properties_from_mesh(v, 8, BOX_TRI, 10, &solid, &mp, &d, "open") && mdiag_has(&d, "MESH_NOT_CLOSED"), "open box rejected");
    int flipped[36];
    for (int t = 0; t < 12; t++) flipped[3 * t] = BOX_TRI[3 * t], flipped[3 * t + 1] = BOX_TRI[3 * t + 2], flipped[3 * t + 2] = BOX_TRI[3 * t + 1];
    mdiag_clear(&d);
    CHECK(!mass_properties_from_mesh(v, 8, flipped, 12, &solid, &mp, &d, "inverted") && mdiag_has(&d, "MESH_INVERTED"), "inward-facing box rejected");
    int onebad[36];
    memcpy(onebad, BOX_TRI, sizeof onebad);
    onebad[1] = BOX_TRI[2], onebad[2] = BOX_TRI[1];
    mdiag_clear(&d);
    CHECK(!mass_properties_from_mesh(v, 8, onebad, 12, &solid, &mp, &d, "mixed") && mdiag_has(&d, "MESH_INCONSISTENT_ORIENTATION"),
          "inconsistent winding rejected");
    mdiag_clear(&d);
    CHECK(!mass_properties_from_mesh(v, 8, BOX_TRI, 12, NULL, &mp, &d, "undeclared") && mdiag_has(&d, "FILL_MODEL_REQUIRED"),
          "no fill model: missing input, not a silent solid");
    /* shell + infill against the exact hollow-shell-plus-infill box */
    double A = 0.1, B = 0.06, C = 0.04;
    box_mesh(A, B, C, &I, v);
    for (int pass = 0; pass < 2; pass++) {
        double t = pass ? 0.6e-3 : 1.2e-3, phi = 0.2;
        FillSpec shell = {.model = FILL_SHELL_INFILL, .density = 1240, .shell_thickness = t, .infill_fraction = phi};
        mdiag_clear(&d);
        ok = mass_properties_from_mesh(v, 8, BOX_TRI, 12, &shell, &mp, &d, "printed box");
        double Vo = A * B * C, a2 = A - 2 * t, b2 = B - 2 * t, c2 = C - 2 * t, Vi = a2 * b2 * c2;
        double mex = 1240 * ((Vo - Vi) + phi * Vi);
        double Ixo = 1240 * Vo / 12 * (B * B + C * C), Ixi = 1240 * Vi / 12 * (b2 * b2 + c2 * c2);
        double Ixex = (Ixo - Ixi) + phi * Ixi;
        double em = fabs(mp.mass / mex - 1), eIx = fabs(mp.inertia_com[0] / Ixex - 1);
        REPORT("shell %.1f mm + 20%% infill: mass %.4f g (exact box shell %.4f g, error %.2f%%), Ixx error %.2f%%, walls %.1f%% of volume", t * 1e3,
               mp.mass * 1e3, mex * 1e3, 100 * em, 100 * eIx, 100 * mp.wall_fraction);
        CHECK(ok && em < 3 * t / C && eIx < 3 * t / C, "thin-shell approximation error below 3 t / thickness (%.2f%%, %.2f%%)", 100 * em, 100 * eIx);
    }
    FillSpec meas = {.model = FILL_MEASURED_MASS, .measured_mass = 0.1, .density = 1240};
    mdiag_clear(&d);
    ok = mass_properties_from_mesh(v, 8, BOX_TRI, 12, &meas, &mp, &d, "weighed box");
    CHECK(ok && fabs(mp.mass - 0.1) < 1e-15 && mdiag_has(&d, "UNIFORM_DISTRIBUTION_ASSUMED") && mdiag_has(&d, "IMPLIED_FILL"),
          "measured mass: uniform-distribution assumption and implied fill reported");
    mdiag_free(&d);
}


/* ---------------------------------------------------------------------------------------------- assemblies */

static char g_tmp[512];

static const char *tmpdir_path(const char *name, char *out, size_t cap) {
    if (!g_tmp[0]) {
        const char *t = getenv("TMPDIR");
        snprintf(g_tmp, sizeof g_tmp, "%s/nv_mechtest_XXXXXX", t && *t ? t : "/tmp");
        for (char *q = g_tmp; *q; q++)
            if (q[0] == '/' && q[1] == '/') memmove(q, q + 1, strlen(q));
        if (!mkdtemp(g_tmp)) g_tmp[0] = 0;
    }
    snprintf(out, cap, "%s/%s", g_tmp, name);
    return out;
}

static bool write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static const char *TWO_LINK_JSON =
    "{\"format\": \"navier-assembly\", \"version\": 1, \"name\": \"two_link\",\n"
    " \"units\": {\"length\": \"mm\", \"angle\": \"deg\", \"mass\": \"g\", \"inertia\": \"kg*m^2\"},\n"
    " \"gravity\": [\"0 m/s^2\", \"-9.81 m/s^2\", \"0 m/s^2\"],\n"
    " \"bodies\": [\n"
    "  {\"name\": \"upper\", \"frames\": [{\"name\": \"elbow\", \"position\": [450, 0, 0]}],\n"
    "   \"mass_properties\": {\"provenance\": \"cad\", \"mass\": 1200, \"com\": [200, 0, 0],\n"
    "                         \"inertia\": {\"ixx\": 0.0042, \"iyy\": 0.0231, \"izz\": 0.021, \"ixy\": 0, \"ixz\": 0, \"iyz\": 0}},\n"
    "   \"material\": {\"id\": \"PLA_generic\", \"source\": \"user\"},\n"
    "   \"manufacturing\": {\"process\": \"FFF\", \"layer_height\": \"0.2 mm\", \"infill\": 0.2}},\n"
    "  {\"name\": \"fore\", \"mass_properties\": {\"provenance\": \"measured\", \"mass\": \"0.8 kg\", \"com\": [\"17 cm\", 0, 0],\n"
    "   \"inertia\": {\"ixx\": \"3300 kg*mm^2\", \"iyy\": \"13200 kg*mm^2\", \"izz\": \"11000 kg*mm^2\", \"ixy\": 0, \"ixz\": 0, \"iyz\": 0}}}\n"
    " ],\n"
    " \"joints\": [\n"
    "  {\"name\": \"shoulder\", \"type\": \"revolute\", \"parent\": \"world\", \"child\": \"upper\", \"parent_frame\": {\"position\": [0, 0, 0]},\n"
    "   \"child_frame\": {\"position\": [0, 0, 0]}, \"axis\": [0, 0, 1], \"motion\": \"actuated\", \"initial\": {\"position\": 30}},\n"
    "  {\"name\": \"elbow\", \"type\": \"revolute\", \"parent\": \"upper\", \"child\": \"fore\", \"parent_frame\": \"upper.elbow\",\n"
    "   \"child_frame\": {\"position\": [0, 0, 0], \"rpy\": [0, 0, 0]}, \"axis\": [0, 0, 1],\n"
    "   \"limits\": {\"lower\": -150, \"upper\": 150, \"restitution\": 0.2, \"effort\": \"2.5 N*m\", \"velocity\": \"180 deg/s\"},\n"
    "   \"damping\": \"0.01 N*m*s/rad\", \"friction\": {\"coulomb\": \"0.02 N*m\", \"regularization_velocity\": \"1 deg/s\"}}\n"
    " ]}\n";

static void SP_DUMP(const char *text, const char *name) {
    char p[700];
    tmpdir_path(name, p, sizeof p);
    write_text(p, text);
    printf("  wrote %s\n", p);
}

static double id_difference(MbModelDef *d1, MbModelDef *d2) {
    MechDiag dg;
    mdiag_init(&dg);
    MbModel *m1 = mb_compile(d1, NULL, &dg), *m2 = mb_compile(d2, NULL, &dg);
    double diff = INFINITY;
    if (m1 && m2 && m1->nv == m2->nv && m1->nv == 2) {
        MbSim *s1 = mb_sim_new(m1), *s2 = mb_sim_new(m2);
        double q[2] = {0.3, -1.1}, v[2] = {1.5, -2}, a[2] = {3, 7}, t1[2], t2[2];
        mb_inverse_dynamics(s1, q, v, a, NULL, t1, NULL);
        mb_inverse_dynamics(s2, q, v, a, NULL, t2, NULL);
        diff = fmax(fabs(t1[0] - t2[0]), fabs(t1[1] - t2[1]));
        mb_sim_free(s1), mb_sim_free(s2);
    } else
        mdiag_print(&dg, "  ");
    mb_model_free(m1), mb_model_free(m2);
    mdiag_free(&dg);
    return diff;
}

static void test_assembly_native(void) {
    printf("== assembly: native format with units, named frames, provenance; canonical round trip; validation\n");
    MechDiag d;
    mdiag_init(&d);
    JsonError je;
    JsonValue *doc = json_parse(TWO_LINK_JSON, strlen(TWO_LINK_JSON), NULL, &je);
    CHECK(doc != NULL, "document parses (%s)", doc ? "" : je.message);
    Assembly *a = asm_from_json(doc, ".", NULL, &d);
    CHECK(a != NULL, "two-link assembly loads");
    if (!a) {
        mdiag_print(&d, "  ");
        json_free(doc);
        mdiag_free(&d);
        return;
    }
    CHECK(fabs(a->bodies[1].com[0] - 0.17) < 1e-15 && fabs(a->bodies[1].inertia[0] - 0.0033) < 1e-15 && fabs(a->joints[0].def.q0[0] - M_PI / 6) < 1e-15,
          "mixed units: 17 cm, 3300 kg*mm^2 and 30 deg converted to SI");
    CHECK(fabs(a->joints[1].def.parent_frame.p[0] - 0.45) < 1e-15, "joint frame from the named frame upper.elbow");
    CHECK(a->bodies[0].inertial_source == SRC_CAD && a->bodies[1].inertial_source == SRC_MEASURED, "mass-property provenance kept");
    CHECK(fabs(a->joints[1].velocity_limit - M_PI) < 1e-15 && fabs(a->joints[1].def.coulomb_vreg - M_PI / 180) < 1e-15, "limits and friction in SI");
    MbModelDef *md = asm_to_model(a, &d);
    TwoLink tl = {1.2, 0.8, 0.45, 0.2, 0.17, 0.021, 0.011, 9.81};
    MbModelDef *ref = two_link_def(&tl);
    /* the JSON arm has damping and friction; inverse dynamics ignores passive joint forces, so the comparison holds */
    double diff = id_difference(md, ref);
    CHECK(diff < 1e-12, "inverse dynamics of the loaded assembly equals the programmatic model (%.2e N m)", diff);
    /* canonical round trip */
    JsonValue *canon = asm_to_json(a);
    char *text = json_dump(canon, JSON_PRETTY, NULL, NULL);
    JsonValue *doc2 = json_parse(text, strlen(text), NULL, &je);
    MechDiag d2;
    mdiag_init(&d2);
    Assembly *b = asm_from_json(doc2, ".", NULL, &d2);
    CHECK(b != NULL && d2.nwarnings == 0, "canonical document reloads without warnings");
    if (b) {
        MbModelDef *md2 = asm_to_model(b, &d2);
        double e = 0;
        for (int i = 0; i < md->nbodies; i++) {
            e = fmax(e, fabs(md->bodies[i].mass - md2->bodies[i].mass));
            for (int k = 0; k < 9; k++) e = fmax(e, fabs(md->bodies[i].inertia[k] - md2->bodies[i].inertia[k]));
        }
        for (int j = 0; j < md->njoints; j++)
            for (int k = 0; k < 9; k++) e = fmax(e, fabs(md->joints[j].parent_frame.R[k] - md2->joints[j].parent_frame.R[k]));
        CHECK(e < 1e-15, "round trip preserves the model (%.1e)", e);
        JsonValue *canon2 = asm_to_json(b);
        char *text2 = json_dump(canon2, JSON_PRETTY, NULL, NULL);
        if (strcmp(text, text2)) {
            SP_DUMP(text, "canon1.json");
            SP_DUMP(text2, "canon2.json");
        }
        CHECK(!strcmp(text, text2), "second canonical dump is identical to the first");
        free(text2);
        json_free(canon2);
        mbdef_free(md2);
        asm_free(b);
    }
    free(text);
    json_free(canon), json_free(doc2);
    mbdef_free(md), mbdef_free(ref);
    asm_free(a);
    json_free(doc);
    mdiag_free(&d2);

    /* validation: missing frames, mass properties and a wrong unit are reported together */
    static const char *BAD =
        "{\"format\": \"navier-assembly\", \"version\": 1, \"name\": \"bad\", \"colour\": \"red\",\n"
        " \"bodies\": [{\"name\": \"a\"},\n"
        "  {\"name\": \"b\", \"mass_properties\": {\"mass\": 1, \"com\": [0, 0, 0], \"inertia\": {\"ixx\": 1, \"iyy\": 1, \"izz\": 1}}}],\n"
        " \"joints\": [{\"name\": \"j\", \"type\": \"revolute\", \"parent\": \"world\", \"child\": \"b\", \"axis\": [0, 0, 1]},\n"
        "  {\"name\": \"k\", \"type\": \"revolute\", \"parent\": \"world\", \"child\": \"a\", \"parent_frame\": {}, \"child_frame\": {},\n"
        "   \"axis\": [0, 0, 1], \"spring\": {\"stiffness\": \"2 N*m\"}}]}";
    mdiag_clear(&d);
    doc = json_parse(BAD, strlen(BAD), NULL, &je);
    a = asm_from_json(doc, ".", NULL, &d);
    CHECK(!a && mdiag_has(&d, "MASS_PROPERTIES_REQUIRED") && mdiag_has(&d, "JOINT_FRAMES_REQUIRED") && mdiag_has(&d, "INVALID_QUANTITY") &&
              mdiag_has(&d, "UNKNOWN_KEY"),
          "missing mass properties, missing joint frames, N*m as a rotational stiffness and an unknown key all reported");
    json_free(doc);
    mdiag_free(&d);
}

static void write_box_stl(const char *path, double ax, double by, double cz) {
    Mesh m;
    mesh_init(&m);
    double v[24];
    MPose I;
    mpose_identity(&I);
    box_mesh(ax, by, cz, &I, v);
    for (int t = 0; t < 12; t++) {
        vec3 p[3];
        for (int k = 0; k < 3; k++) {
            const double *x = v + 3 * BOX_TRI[3 * t + k];
            p[k].x = (float)x[0], p[k].y = (float)x[1], p[k].z = (float)x[2];
        }
        mesh_add_tri(&m, p[0], p[1], p[2]);
    }
    char err[128];
    mesh_save_stl(path, &m, err, sizeof err);
    mesh_free(&m);
}

static void test_assembly_stl(void) {
    printf("== assembly: STL parts with printed fill, mesh-sourced mass properties in the native format\n");
    char path[700];
    tmpdir_path("box40.stl", path, sizeof path);
    write_box_stl(path, 40, 20, 10); /* millimetres */
    Assembly *a = asm_new("printed");
    MechDiag d;
    mdiag_init(&d);
    FillSpec fill = {.model = FILL_SHELL_INFILL, .density = 1240, .shell_thickness = 0.8e-3, .infill_fraction = 0.15};
    AsmBody *B = asm_add_stl_body(a, "bracket", path, "mm", &fill, SRC_USER, NULL, &d);
    CHECK(B != NULL, "STL part imported");
    double v[24];
    MPose I;
    mpose_identity(&I);
    box_mesh(0.04, 0.02, 0.01, &I, v);
    MassProperties ref;
    MechDiag d2;
    mdiag_init(&d2);
    mass_properties_from_mesh(v, 8, BOX_TRI, 12, &fill, &ref, &d2, "ref");
    if (B) {
        double e = fabs(B->mass / ref.mass - 1), ec = mv3_norm((double[3]){B->com[0] - ref.com[0], B->com[1] - ref.com[1], B->com[2] - ref.com[2]});
        CHECK(e < 1e-6 && ec < 1e-8, "mass from the STL file matches the analytic box (%.1e relative, centre %.1e m; STL stores single precision)", e, ec);
        CHECK(B->inertial_source == SRC_COMPUTED && strstr(B->inertial_note, "shell_infill"), "computed provenance with the fill model");
    }
    /* the same part referenced from a native document, in another unit for the density */
    char json[1800], jpath[700];
    snprintf(json, sizeof json,
             "{\"format\": \"navier-assembly\", \"version\": 1, \"name\": \"plate\", \"bodies\": [{\"name\": \"p\", \"mass_properties\": "
             "{\"source\": \"mesh\", \"file\": \"box40.stl\", \"units\": \"mm\", \"fill\": {\"model\": \"solid\", \"density\": \"1.24 g/cm^3\"}}}],"
             " \"joints\": [{\"name\": \"hold\", \"type\": \"fixed\", \"parent\": \"world\", \"child\": \"p\", \"parent_frame\": {}, \"child_frame\": {}}]}");
    tmpdir_path("plate.json", jpath, sizeof jpath);
    write_text(jpath, json);
    mdiag_clear(&d);
    Assembly *p = asm_load_file(jpath, NULL, &d);
    CHECK(p && fabs(p->bodies[0].mass - 1240 * 0.04 * 0.02 * 0.01) < 1e-6 * 0.00992, "mesh-sourced solid part: %.6f g", p ? p->bodies[0].mass * 1e3 : 0);
    CHECK(p && strlen(p->source_sha256) == 64, "source document hash recorded");
    /* missing units and missing fill are inputs to ask for */
    mdiag_clear(&d);
    Assembly *q = asm_new("q");
    CHECK(!asm_add_stl_body(q, "x", path, NULL, &fill, SRC_USER, NULL, &d) && mdiag_has(&d, "MESH_UNITS_REQUIRED"), "STL without units: missing input");
    mdiag_clear(&d);
    FillSpec nofill = {.model = FILL_MODEL_COUNT};
    CHECK(!asm_add_stl_body(q, "x", path, "mm", &nofill, SRC_USER, NULL, &d) && mdiag_has(&d, "FILL_MODEL_REQUIRED"), "STL without a fill model: missing input");
    asm_free(q);
    asm_free(p);
    asm_free(a);
    unlink(jpath);
    unlink(path);
    mdiag_free(&d), mdiag_free(&d2);
}

static const char *URDF_TEXT =
    "<?xml version=\"1.0\"?>\n<!-- test gripper -->\n<robot name=\"gripper test\">\n"
    "  <material name=\"grey\"><color rgba=\"0.5 0.5 0.5 1\"/></material>\n"
    "  <link name=\"base_link\">\n"
    "    <inertial><origin xyz=\"0 0 0.05\" rpy=\"0 0 0\"/><mass value=\"2.0\"/>"
    "<inertia ixx=\"0.01\" ixy=\"0\" ixz=\"0\" iyy=\"0.01\" iyz=\"0\" izz=\"0.005\"/></inertial>\n"
    "    <visual><geometry><box size=\"0.1 0.1 0.1\"/></geometry><material name=\"grey\"/></visual>\n"
    "    <collision><origin xyz=\"0 0 0.05\"/><geometry><cylinder radius=\"0.05\" length=\"0.1\"/></geometry></collision>\n"
    "  </link>\n"
    "  <link name=\"finger_l\"><inertial><origin xyz=\"0 0.01 0.03\" rpy=\"0 0 1.5707963267948966\"/><mass value=\"0.05\"/>"
    "<inertia ixx=\"2e-5\" ixy=\"0\" ixz=\"0\" iyy=\"1e-5\" iyz=\"0\" izz=\"1.5e-5\"/></inertial></link>\n"
    "  <link name=\"finger_r\"><inertial><origin xyz=\"0 -0.01 0.03\"/><mass value=\"0.05\"/>"
    "<inertia ixx=\"1e-5\" ixy=\"0\" ixz=\"0\" iyy=\"2e-5\" iyz=\"0\" izz=\"1.5e-5\"/></inertial></link>\n"
    "  <link name=\"tool0\"/>\n"
    "  <joint name=\"slide_l\" type=\"prismatic\"><parent link=\"base_link\"/><child link=\"finger_l\"/>"
    "<origin xyz=\"0 0.02 0.1\" rpy=\"0 0 0\"/><axis xyz=\"0 2 0\"/><limit lower=\"0\" upper=\"0.03\" effort=\"20\" velocity=\"0.1\"/>"
    "<dynamics damping=\"0.5\" friction=\"0.2\"/></joint>\n"
    "  <joint name=\"slide_r\" type=\"prismatic\"><parent link=\"base_link\"/><child link=\"finger_r\"/>"
    "<origin xyz=\"0 -0.02 0.1\"/><axis xyz=\"0 -1 0\"/><limit lower=\"0\" upper=\"0.03\" effort=\"20\" velocity=\"0.1\"/>"
    "<mimic joint=\"slide_l\" multiplier=\"1\" offset=\"0\"/><safety_controller k_velocity=\"10\"/></joint>\n"
    "  <joint name=\"tool_joint\" type=\"fixed\"><parent link=\"base_link\"/><child link=\"tool0\"/>"
    "<origin xyz=\"0 0 0.15\" rpy=\"3.141592653589793 0 0\"/></joint>\n"
    "  <transmission name=\"t1\"><type>transmission_interface/SimpleTransmission</type></transmission>\n"
    "  <gazebo reference=\"base_link\"><material>Gazebo/Grey</material></gazebo>\n"
    "</robot>\n";

static void test_urdf(void) {
    printf("== URDF subset: links, joints, mimic, massless frames, unsupported elements, explicit root connection\n");
    MechDiag d;
    mdiag_init(&d);
    Assembly *a = asm_from_urdf_text(URDF_TEXT, strlen(URDF_TEXT), ".", NULL, &d);
    CHECK(!a && mdiag_has(&d, "ROOT_CONNECTION"), "root connection to the world is asked for, not assumed");
    AsmLoadOptions opt = {.root_joint = "fixed"};
    mdiag_clear(&d);
    a = asm_from_urdf_text(URDF_TEXT, strlen(URDF_TEXT), ".", &opt, &d);
    CHECK(a != NULL, "URDF imports with root_joint fixed");
    if (!a) {
        mdiag_print(&d, "  ");
        mdiag_free(&d);
        return;
    }
    bool trans = false, gaz = false, safety = false;
    for (int i = 0; i < a->nunsupported; i++) {
        trans |= strstr(a->unsupported[i].text, "transmission") != NULL;
        gaz |= strstr(a->unsupported[i].text, "gazebo") != NULL;
        safety |= strstr(a->unsupported[i].text, "safety_controller") != NULL;
    }
    CHECK(trans && gaz && safety, "transmission, gazebo and safety_controller listed as not imported");
    CHECK(a->ncouplings == 1 && !strcmp(a->couplings[0].follower, "slide_r") && a->couplings[0].ratio == 1, "mimic becomes a coupling");
    int fl = asm_body_index(a, "finger_l");
    const double *Il = a->bodies[fl].inertia;
    CHECK(fabs(Il[0] - 1e-5) < 1e-18 && fabs(Il[4] - 2e-5) < 1e-18 && fabs(Il[1]) < 1e-18, "inertial rpy rotates the inertia into link axes");
    CHECK(fabs(a->joints[asm_joint_index(a, "slide_l")].def.axis[1] - 1) < 1e-15, "axis normalised");
    CHECK(fabs(a->joints[asm_joint_index(a, "slide_l")].effort_limit - 20) < 1e-15, "effort rating kept");
    MbModelDef *md = asm_to_model(a, &d);
    CHECK(md && md->nbodies == 3 && mdiag_has(&d, "MASSLESS_LINK_MERGED"), "massless tool0 merged into base_link as a frame");
    MechDiag dc;
    mdiag_init(&dc);
    MbModel *m = md ? mb_compile(md, NULL, &dc) : NULL;
    CHECK(!m && mdiag_has(&dc, "FRICTION_REGULARISATION"), "URDF friction without a regularisation speed is a missing input");
    mb_model_free(m);
    a->joints[asm_joint_index(a, "slide_l")].def.coulomb_vreg = 1e-3;
    mbdef_free(md);
    md = asm_to_model(a, &d);
    mdiag_clear(&dc);
    Run r;
    bool ok = run_open(&r, md, NULL);
    CHECK(ok && r.m->nv == 2 && r.m->ncouplings == 1, "gripper model assembles: 2 coordinates, 1 coupling");
    if (ok) {
        MbStepReport rep;
        double tau[2] = {1.0, 0};
        MbInputs in = {tau, NULL};
        ok = mb_step(r.sim, r.s, 1e-3, &in, &rep);
        CHECK(ok && fabs(r.s->q[1] - r.s->q[0]) < 1e-12, "mimic keeps both fingers together under a one-sided input (%.2e m)", fabs(r.s->q[1] - r.s->q[0]));
    }
    run_close(&r);
    mbdef_free(md);
    asm_free(a);
    mdiag_free(&d), mdiag_free(&dc);
    /* XML reader */
    char err[200];
    const char *ent = "<a b=\"x &amp; y &#65;&#x42;\"><c>text &lt;1&gt;</c><![CDATA[<raw>]]></a>";
    XmlNode *x = xml_parse(ent, strlen(ent), err, sizeof err);
    CHECK(x && !strcmp(xml_attr(x, "b"), "x & y AB") && !strcmp(xml_child(x, "c")->text, "text <1>") && !strcmp(x->text, "<raw>"), "entities and CDATA");
    xml_free(x);
    x = xml_parse("<robot><link></robot>", 21, err, sizeof err);
    CHECK(!x && strstr(err, "mismatched"), "mismatched tags rejected (%s)", err);
    const char *dt = "<!DOCTYPE r [<!ENTITY e \"boom\">]><r>&e;</r>";
    x = xml_parse(dt, strlen(dt), err, sizeof err);
    CHECK(!x && strstr(err, "DOCTYPE"), "DOCTYPE and entity declarations rejected");
}


/* ---------------------------------------------------------------------------------------------- runtime */

/* a rotor (disk) on a vertical revolute joint: gravity exerts no torque */
static MbModelDef *rotor_def(double J, double damping) {
    MbModelDef *def = mbdef_new();
    double I[9];
    diag3(I, J / 2, J / 2, J);
    int b = mbdef_add_body(def, "disk", 0.2, (double[3]){0, 0, 0}, I);
    int j = mbdef_add_joint(def, "spindle", MB_REVOLUTE, -1, b);
    def->joints[j].motion = MB_ACTUATED;
    def->joints[j].damping = damping;
    return def;
}

static MechSim *runtime(MbModelDef *def, const ActuatorDef *a, int na, const ControllerDef *c, int nc, const SensorDef *se, int ns, double T, double h,
                        double rec, MechDiag *d) {
    StudySettings st = {.end_time = T, .max_step = h, .record_period = rec};
    MechSim *ms = mechsim_new(def, NULL, a, na, c, nc, se, ns, &st, d);
    if (!ms) return NULL;
    if (!mechsim_init(ms, d)) {
        mechsim_free(ms);
        return NULL;
    }
    return ms;
}

static void test_actuators(void) {
    printf("== actuators: DC motor spin-up (analytic), gearbox efficiency, effort saturation and torque-speed envelope\n");
    MechDiag d;
    mdiag_init(&d);
    double J = 2e-4, R = 2, k = 0.05, N = 10, Jr = 1e-6, V = 6;
    MbModelDef *def = rotor_def(J, 0);
    ActuatorDef a = {.name = "motor", .type = ACT_DC_MOTOR, .joint = 0, .gear_ratio = N, .efficiency = 1, .rotor_inertia = Jr, .resistance = R,
                     .torque_constant = k, .back_emf_constant = k, .voltage_limit = 12, .initial_command = V};
    MechSim *ms = runtime(def, &a, 1, NULL, 0, NULL, 0, 0.5, 1e-4, 0.01, &d);
    CHECK(ms != NULL, "motor study initialises");
    if (!ms) {
        mdiag_print(&d, "  ");
        mbdef_free(def);
        return;
    }
    char err[256];
    CHECK(mechsim_run_until(ms, 0.5, err, sizeof err), "spin-up runs (%s)", err);
    double Jt = J + N * N * Jr, wss = V / (k * N), tau = Jt * R / (k * k * N * N);
    double wref = wss * (1 - exp(-0.5 / tau)), w = mechsim_state(ms)->v[0];
    ActuatorOutput o;
    mechsim_actuator_output(ms, 0, &o);
    double iref = (V - k * N * w) / R;
    REPORT("speed %.9f rad/s (analytic %.9f, time constant %.4f s), current %.6f A (from speed %.6f A)", w, wref, tau, o.current, iref);
    CHECK(fabs(w - wref) < 1e-9 * wss, "joint speed follows w_ss (1 - exp(-t/tau)) with reflected rotor inertia (%.2e rad/s)", fabs(w - wref));
    CHECK(fabs(o.current - iref) < 1e-12, "current (V - ke N w) / R");
    JsonValue *sum = mechsim_summary_json(ms);
    const JsonValue *act = json_at(json_get(sum, "actuators"), 0);
    double Eel = json_get_num(act, "electrical_energy_J", 0), Ecu = json_get_num(act, "copper_loss_J", 0);
    double KE = 0.5 * Jt * w * w;
    REPORT("electrical %.6f J = copper %.6f J + kinetic %.6f J (mismatch %.2e J; losses integrated with the dynamics)", Eel, Ecu, KE, Eel - Ecu - KE);
    CHECK(fabs(Eel - Ecu - KE) < 1e-7 * Eel, "electrical energy = copper loss + kinetic energy (%.2e relative)", fabs(Eel - Ecu - KE) / Eel);
    json_free(sum);
    mechsim_free(ms);
    mbdef_free(def);

    /* gearbox efficiency at a steady operating point against a viscous load */
    double c = 1e-3, eta = 0.8;
    def = rotor_def(J, c);
    a.efficiency = eta, a.rotor_inertia = 0;
    ms = runtime(def, &a, 1, NULL, 0, NULL, 0, 3.0, 5e-4, 0.1, &d);
    mechsim_run_until(ms, 3.0, err, sizeof err);
    double vss = N * eta * k * V / (R * c + N * N * eta * k * k);
    w = mechsim_state(ms)->v[0];
    mechsim_actuator_output(ms, 0, &o);
    double tm = k * o.current, Pm = tm * N * w;
    REPORT("steady speed %.6f rad/s (analytic %.6f), gearbox loss %.6f W = (1 - eta) P_motor %.6f W", w, vss, o.gear_loss, (1 - eta) * Pm);
    CHECK(fabs(w - vss) < 1e-6 * vss && fabs(o.gear_loss - (1 - eta) * Pm) < 1e-9, "steady state and gearbox loss with efficiency 0.8");
    mechsim_free(ms);
    mbdef_free(def);

    /* effort actuator: clamp and linear torque-speed envelope */
    def = rotor_def(J, c);
    ActuatorDef e = {.name = "drive", .type = ACT_EFFORT, .joint = 0, .effort_limit = 0.02, .velocity_limit = 30, .initial_command = 5.0, .rated_effort = 0.015};
    ms = runtime(def, &e, 1, NULL, 0, NULL, 0, 3.0, 5e-4, 0.1, &d);
    mechsim_run_until(ms, 3.0, err, sizeof err);
    double venv = e.effort_limit / (c + e.effort_limit / e.velocity_limit);
    w = mechsim_state(ms)->v[0];
    sum = mechsim_summary_json(ms);
    act = json_at(json_get(sum, "actuators"), 0);
    REPORT("envelope speed %.6f rad/s (analytic %.6f), saturated %.0f%% of the time, peak/rated %.2f", w, venv, 100 * json_get_num(act, "saturated_time_fraction", 0),
           json_get_num(act, "peak_over_rated", 0));
    CHECK(fabs(w - venv) < 1e-6 * venv && json_get_num(act, "peak_over_rated", 0) > 1.3, "torque-speed envelope and over-rating flag");
    json_free(sum);
    mechsim_free(ms);
    mbdef_free(def);
    mdiag_free(&d);
}

static void test_controllers_sensors(void) {
    printf("== controllers and sensors: sample-and-hold with delay, noise statistics, seeds, latency, quantisation, IMU, rollback\n");
    MechDiag d;
    mdiag_init(&d);
    char err[256];
    /* open-loop step through a 3-sample delay: the effort changes exactly at the delayed sample */
    MbModelDef *def = rotor_def(2e-4, 1e-3);
    ActuatorDef e = {.name = "drive", .type = ACT_EFFORT, .joint = 0, .effort_limit = 1};
    ControllerDef c = {.name = "open", .type = CTRL_OPEN_LOOP, .actuator = 0, .feedback_sensor = -1, .period = 1e-3, .delay_samples = 3,
                       .reference = {.type = TRAJ_STEP, .t0 = 0.0105, .value = 0, .value1 = 0.01}};
    MechSim *ms = runtime(def, &e, 1, &c, 1, NULL, 0, 0.03, 2.5e-4, 2.5e-4, &d);
    CHECK(ms && mechsim_run_until(ms, 0.03, err, sizeof err), "open-loop study runs");
    if (ms) {
        const double *tcol = mechsim_column(ms, mechsim_channel_index(ms, "time"));
        const double *eff = mechsim_column(ms, mechsim_channel_index(ms, "drive.effort"));
        double t_change = -1;
        for (int r = 0; r < mechsim_rows(ms); r++)
            if (eff[r] > 0.005) {
                t_change = tcol[r];
                break;
            }
        REPORT("reference steps at 10.5 ms, first sample sees it at 11 ms, effort applied at %.4f ms (3 samples later)", t_change * 1e3);
        CHECK(fabs(t_change - 0.014) < 1e-12, "sample-and-hold with a 3-sample output delay");
        mechsim_free(ms);
    }
    mbdef_free(def);

    /* encoder noise, bias, determinism and quantisation on a joint at rest */
    def = rotor_def(2e-4, 1e-3);
    SensorDef enc = {.name = "enc", .type = SENS_JOINT_POSITION, .joint = 0, .period = 1e-3, .noise_std = 0.01, .bias = 0.002, .seed = 42};
    double col_sum = 0, col_sq = 0;
    int n = 0;
    double *first = NULL;
    for (int run = 0; run < 3; run++) {
        SensorDef se = enc;
        if (run == 2) se.seed = 43;
        ms = runtime(def, NULL, 0, NULL, 0, &se, 1, 5.0, 1e-3, 1e-3, &d);
        if (!ms) break;
        mechsim_run_until(ms, 5.0, err, sizeof err);
        const double *m = mechsim_column(ms, mechsim_channel_index(ms, "enc.measured"));
        int rows = mechsim_rows(ms);
        if (run == 0) {
            first = malloc((size_t)rows * sizeof *first);
            memcpy(first, m, (size_t)rows * sizeof *first);
            for (int r = 0; r < rows; r++) col_sum += m[r], col_sq += m[r] * m[r];
            n = rows;
        } else if (run == 1)
            CHECK(!memcmp(first, m, (size_t)rows * sizeof *first), "same seed: identical measurement sequence");
        else
            CHECK(memcmp(first, m, (size_t)rows * sizeof *first) != 0, "different seed: different sequence");
        mechsim_free(ms);
    }
    free(first);
    double mean = col_sum / n, sd = sqrt(col_sq / n - mean * mean);
    REPORT("%d samples: mean %.5f (bias 0.002, standard error %.5f), standard deviation %.5f (0.01)", n, mean, 0.01 / sqrt(n), sd);
    CHECK(fabs(mean - 0.002) < 4 * 0.01 / sqrt(n) && fabs(sd - 0.01) < 0.05 * 0.01, "noise statistics");
    mbdef_free(def);

    /* latency and quantisation on a joint turning at constant speed (velocity servo) */
    def = rotor_def(2e-4, 0);
    ActuatorDef vs = {.name = "spin", .type = ACT_VELOCITY_SERVO, .joint = 0, .kv = 5, .effort_limit = 10, .initial_command = 2.0};
    def->joints[0].v0[0] = 2.0;
    SensorDef lat = {.name = "enc", .type = SENS_JOINT_POSITION, .joint = 0, .period = 2e-3, .latency_samples = 5, .resolution = 2 * M_PI / 4096, .seed = 1};
    ms = runtime(def, &vs, 1, NULL, 0, &lat, 1, 0.2, 5e-4, 2e-3, &d);
    CHECK(ms && mechsim_run_until(ms, 0.2, err, sizeof err), "latency study runs");
    if (ms) {
        const double *m = mechsim_column(ms, mechsim_channel_index(ms, "enc.measured")), *tr = mechsim_column(ms, mechsim_channel_index(ms, "enc.true"));
        double emax = 0, qerr = 0, res = 2 * M_PI / 4096;
        for (int r = 10; r < mechsim_rows(ms); r++) {
            emax = fmax(emax, fabs(m[r] - res * nearbyint(tr[r - 5] / res)));
            qerr = fmax(qerr, fabs(m[r] / res - nearbyint(m[r] / res)));
        }
        CHECK(emax < 1e-12 && qerr < 1e-9, "measured(t) = quantised truth(t - 5 samples) (%.1e rad)", emax);
        mechsim_free(ms);
    }
    mbdef_free(def);

    /* IMU at rest reads +g; spinning body reads its rate and centripetal acceleration */
    def = rotor_def(2e-4, 0);
    def->joints[0].v0[0] = 3.0;
    SensorDef acc = {.name = "acc", .type = SENS_IMU_ACCEL, .body = 0, .period = 1e-3};
    SensorDef gyr = {.name = "gyr", .type = SENS_IMU_GYRO, .body = 0, .period = 1e-3};
    mpose_identity(&acc.site), mpose_identity(&gyr.site);
    mv3_set(acc.site.p, 0.05, 0, 0);
    SensorDef two[2] = {acc, gyr};
    ms = runtime(def, NULL, 0, NULL, 0, two, 2, 0.1, 1e-3, 1e-3, &d);
    if (ms) {
        mechsim_run_until(ms, 0.1, err, sizeof err);
        int last = mechsim_rows(ms) - 1;
        double ax = mechsim_column(ms, mechsim_channel_index(ms, "acc.measured[0]"))[last];
        double az = mechsim_column(ms, mechsim_channel_index(ms, "acc.measured[2]"))[last];
        double gz = mechsim_column(ms, mechsim_channel_index(ms, "gyr.measured[2]"))[last];
        REPORT("IMU 50 mm off the spin axis at 3 rad/s: accel x %.9f (-w^2 r = -0.45), z %.9f (+g), gyro z %.9f", ax, az, gz);
        CHECK(fabs(ax + 0.45) < 1e-9 && fabs(az - 9.80665) < 1e-9 && fabs(gz - 3) < 1e-12, "accelerometer (specific force) and gyroscope in the sensor frame");
        mechsim_free(ms);
    }
    mbdef_free(def);

    /* PID position loop on a geared DC motor lifting an arm, with and without anti-windup */
    double overshoot[2] = {0};
    for (int aw = 0; aw < 2; aw++) {
        def = mbdef_new();
        double I[9];
        diag3(I, 1e-4, 1.2e-3, 1.2e-3);
        int b = mbdef_add_body(def, "arm", 0.3, (double[3]){0.1, 0, 0}, I);
        int j = mbdef_add_joint(def, "shoulder", MB_REVOLUTE, -1, b);
        mv3_set(def->joints[j].axis, 0, -1, 0);
        def->joints[j].motion = MB_ACTUATED;
        ActuatorDef mot = {.name = "motor", .type = ACT_DC_MOTOR, .joint = 0, .gear_ratio = 50, .efficiency = 0.7, .rotor_inertia = 5e-7, .resistance = 3,
                           .torque_constant = 0.02, .back_emf_constant = 0.02, .voltage_limit = 6, .current_limit = 1.5};
        SensorDef en = {.name = "encoder", .type = SENS_JOINT_POSITION, .joint = 0, .period = 1e-3, .resolution = 2 * M_PI / 4096, .seed = 7};
        ControllerDef pid = {.name = "pid", .type = CTRL_PID, .actuator = 0, .feedback_sensor = 0, .period = 1e-3, .delay_samples = 1, .kp = 40, .ki = 60,
                             .kd = 1.5, .derivative_filter = 5e-3, .output_min = -6, .output_max = 6, .antiwindup = aw ? AW_CLAMP : AW_NONE,
                             .reference = {.type = TRAJ_STEP, .t0 = 0.0, .value = 0, .value1 = 1.2}};
        ms = runtime(def, &mot, 1, &pid, 1, &en, 1, 3.0, 5e-4, 1e-3, &d);
        if (!ms) {
            mdiag_print(&d, "  ");
            mbdef_free(def);
            break;
        }
        mechsim_run_until(ms, 3.0, err, sizeof err);
        const double *q = mechsim_column(ms, mechsim_channel_index(ms, "shoulder.q"));
        double qmax = 0;
        for (int r = 0; r < mechsim_rows(ms); r++) qmax = fmax(qmax, q[r]);
        overshoot[aw] = qmax - 1.2;
        JsonValue *sum = mechsim_summary_json(ms);
        const JsonValue *cj = json_at(json_get(sum, "controllers"), 0);
        REPORT("%s: overshoot %.4f rad, final error %.5f rad, saturated %.0f%% of samples", aw ? "clamping anti-windup" : "no anti-windup", overshoot[aw],
               json_get_num(cj, "final_error", 0), 100 * json_get_num(cj, "saturated_sample_fraction", 0));
        if (aw) CHECK(fabs(json_get_num(cj, "final_error", 1)) < 2 * 2 * M_PI / 4096, "settles within two encoder counts");
        json_free(sum);
        mechsim_free(ms);
        mbdef_free(def);
    }
    CHECK(overshoot[1] < overshoot[0], "anti-windup reduces the overshoot after saturation (%.3f < %.3f rad)", overshoot[1], overshoot[0]);

    /* rollback: a noisy closed loop restored from a checkpoint replays bitwise, including the random samples */
    def = rotor_def(2e-4, 1e-3);
    ActuatorDef ps = {.name = "servo", .type = ACT_POSITION_SERVO, .joint = 0, .kp = 0.5, .kd = 0.01, .effort_limit = 0.05};
    SensorDef noisy = {.name = "enc", .type = SENS_JOINT_POSITION, .joint = 0, .period = 1e-3, .noise_std = 0.02, .latency_samples = 2, .filter_tau = 3e-3,
                       .seed = 99};
    ControllerDef outer = {.name = "outer", .type = CTRL_PID, .actuator = 0, .feedback_sensor = 0, .period = 2e-3, .kp = 1, .ki = 0.5, .output_min = -3,
                           .output_max = 3, .antiwindup = AW_CLAMP, .reference = {.type = TRAJ_SINE, .value = 0, .amplitude = 1, .frequency = 2}};
    ms = runtime(def, &ps, 1, &outer, 1, &noisy, 1, 1.0, 5e-4, 1e-3, &d);
    if (ms) {
        mechsim_run_until(ms, 0.4, err, sizeof err);
        size_t sz = mechsim_state_size(ms);
        uint8_t *ck = malloc(sz);
        CHECK(mechsim_save_state(ms, ck, sz), "checkpoint written (%zu bytes)", sz);
        mechsim_run_until(ms, 1.0, err, sizeof err);
        int rows1 = mechsim_rows(ms);
        double q1 = mechsim_state(ms)->q[0];
        const double *m1 = mechsim_column(ms, mechsim_channel_index(ms, "enc.measured"));
        double last1 = m1[rows1 - 1];
        CHECK(mechsim_restore_state(ms, ck, sz) && mechsim_time(ms) == 0.4 && mechsim_rows(ms) < rows1, "restored to 0.4 s with later rows discarded");
        mechsim_run_until(ms, 1.0, err, sizeof err);
        const double *m2 = mechsim_column(ms, mechsim_channel_index(ms, "enc.measured"));
        CHECK(mechsim_rows(ms) == rows1 && mechsim_state(ms)->q[0] == q1 && m2[rows1 - 1] == last1, "retried interval replays bitwise (q and noisy measurement)");
        free(ck);
        mechsim_free(ms);
    }
    mbdef_free(def);
    mdiag_free(&d);
}


/* ---------------------------------------------------------------------------------------------- load transfer to FEM */

typedef struct BoxMesh {
    int nx, ny, nz, nn, ne;
    double *xyz, *rho;
    int *conn;
} BoxMesh;

static int bnode(const BoxMesh *b, int i, int j, int k) { return i + (b->nx + 1) * (j + (b->ny + 1) * k); }

/* x in [0, L], y in [-W/2, W/2], z in [-H/2, H/2] */
static BoxMesh box_hex(int nx, int ny, int nz, double L, double W, double H, double rho) {
    BoxMesh b = {nx, ny, nz, (nx + 1) * (ny + 1) * (nz + 1), nx * ny * nz, NULL, NULL, NULL};
    b.xyz = malloc((size_t)(3 * b.nn) * sizeof(double));
    b.rho = malloc((size_t)b.ne * sizeof(double));
    b.conn = malloc((size_t)(8 * b.ne) * sizeof(int));
    for (int k = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) {
                double *x = b.xyz + 3 * bnode(&b, i, j, k);
                x[0] = L * i / nx, x[1] = -W / 2 + W * j / ny, x[2] = -H / 2 + H * k / nz;
            }
    int e = 0;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++, e++) {
                int n[8] = {bnode(&b, i, j, k),     bnode(&b, i + 1, j, k),     bnode(&b, i + 1, j + 1, k),     bnode(&b, i, j + 1, k),
                            bnode(&b, i, j, k + 1), bnode(&b, i + 1, j, k + 1), bnode(&b, i + 1, j + 1, k + 1), bnode(&b, i, j + 1, k + 1)};
                memcpy(b.conn + 8 * e, n, sizeof n);
                b.rho[e] = rho;
            }
    return b;
}

/* faces of the x = 0 (face 5) or x = L (face 3) end */
static int end_faces(const BoxMesh *b, bool tip, int *elem, unsigned char *local) {
    int n = 0;
    for (int k = 0; k < b->nz; k++)
        for (int j = 0; j < b->ny; j++) {
            int i = tip ? b->nx - 1 : 0;
            elem[n] = i + b->nx * (j + b->ny * k);
            local[n] = tip ? 3 : 5;
            n++;
        }
    return n;
}

/* mean xx stress over Gauss points of elements whose centre is within the slice |x - xs| < dx/2, and slope d(sxx)/dz */
static void slice_stress(const BoxMesh *b, const SolidResult *res, double xs, double *mean, double *slope) {
    double s = 0, sz = 0, szz = 0, sxz = 0, n = 0;
    static const double G = 0.5773502691896257645;
    for (int e = 0; e < b->ne; e++) {
        double X[8][3], xc = 0;
        for (int a = 0; a < 8; a++) {
            const double *p = b->xyz + 3 * b->conn[8 * e + a];
            X[a][0] = p[0], X[a][1] = p[1], X[a][2] = p[2];
            xc += p[0] / 8;
        }
        double dx = b->xyz[3 * bnode(b, 1, 0, 0)];
        if (fabs(xc - xs) > 0.5 * dx) continue;
        for (int g = 0; g < 8; g++) {
            double N[8], dN[8][3], z = 0;
            hex8_shape(HEX8_XI[g][0] * G, HEX8_XI[g][1] * G, HEX8_XI[g][2] * G, N, dN);
            for (int a = 0; a < 8; a++) z += N[a] * X[a][2];
            double sxx = res->gp_stress[48 * e + 6 * g];
            s += sxx, sz += z, szz += z * z, sxz += sxx * z, n++;
        }
    }
    *mean = s / n;
    *slope = (sxz - sz * s / n) / (szz - sz * sz / n);
}

static void test_load_transfer(void) {
    printf("== load transfer to FEM: exact wrench distribution, spinning bar (centrifugal stress), cantilever (bending), isostatic supports\n");
    /* exactness of the distributing coupling */
    BoxMesh b = box_hex(6, 3, 3, 0.12, 0.03, 0.03, 1240);
    FemMeshView mv = {b.nn, b.ne, b.xyz, b.conn, b.rho};
    int fe[64];
    unsigned char fl[64];
    int nf = end_faces(&b, true, fe, fl);
    FaceSet fs = {nf, fe, fl};
    double *f = calloc((size_t)(3 * b.nn), sizeof(double));
    double F[3] = {3.1, -7.2, 12.5}, M[3] = {0.21, -0.05, 0.33}, P[3] = {0.05, 0.004, -0.002}, R[3], Mr[3];
    char err[256];
    WrenchTransfer wt;
    bool ok = loads_distribute_wrench(&mv, &fs, F, M, P, f, &wt, err, sizeof err);
    loads_resultant(&mv, f, P, R, Mr);
    double eF = mv3_norm((double[3]){R[0] - F[0], R[1] - F[1], R[2] - F[2]}), eM = mv3_norm((double[3]){Mr[0] - M[0], Mr[1] - M[1], Mr[2] - M[2]});
    CHECK(ok && eF < 1e-13 * mv3_norm(F) && eM < 1e-13 * (mv3_norm(M) + 0.1 * mv3_norm(F)) && wt.force_error < 1e-12 && wt.moment_error < 1e-12,
          "nodal forces reproduce the force and the moment about an arbitrary point (%.1e N, %.1e N m)", eF, eM);
    /* a line of faces cannot carry a twisting moment about the line */
    BoxMesh line = box_hex(2, 1, 4, 0.02, 0.005, 0.04, 1240);
    FemMeshView lv = {line.nn, line.ne, line.xyz, line.conn, line.rho};
    int le[8];
    unsigned char ll[8];
    int ln = end_faces(&line, false, le, ll);
    FaceSet lfs = {ln, le, ll};
    double *lf = calloc((size_t)(3 * line.nn), sizeof(double));
    double twist[3] = {0, 0, 0.1}, zero[3] = {0, 0, 0}, bendy[3] = {0, 0.1, 0};
    ok = loads_distribute_wrench(&lv, &lfs, zero, twist, zero, lf, NULL, err, sizeof err);
    bool ok2 = loads_distribute_wrench(&lv, &lfs, zero, bendy, zero, lf, NULL, err + 100, sizeof err - 100);
    CHECK(!ok && strstr(err, "cannot carry") && ok2, "a straight line of faces carries bending about y but rejects twist about its own axis");
    free(lf);
    free(line.xyz), free(line.rho), free(line.conn);
    free(f);
    free(b.xyz), free(b.rho), free(b.conn);

    /* spinning bar: body force rho w^2 x balanced by the hub wrench; sxx(x) = rho w^2 (L^2 - x^2) / 2 */
    double L = 0.2, W = 0.02, H = 0.02, rho = 1240, w = 50;
    for (int refine = 0; refine < 2; refine++) {
        int nx = refine ? 40 : 20;
        b = box_hex(nx, 2, 2, L, W, H, rho);
        mv = (FemMeshView){b.nn, b.ne, b.xyz, b.conn, b.rho};
        f = calloc((size_t)(3 * b.nn), sizeof(double));
        double omega[3] = {0, 0, w}, s0[3] = {0, 0, 0}, alpha[3] = {0, 0, 0};
        InertialLoad il;
        loads_inertial(&mv, NULL, 0, s0, omega, alpha, f, &il);
        double Fhub[3] = {-il.resultant[0], -il.resultant[1], -il.resultant[2]}, Mhub[3] = {-il.moment[0], -il.moment[1], -il.moment[2]}, O[3] = {0, 0, 0};
        nf = end_faces(&b, false, fe, fl);
        fs = (FaceSet){nf, fe, fl};
        ok = loads_distribute_wrench(&mv, &fs, Fhub, Mhub, O, f, &wt, err, sizeof err);
        double Fanal = rho * W * H * w * w * L * L / 2;
        if (!refine) CHECK(fabs(il.resultant[0] - Fanal) < 1e-9 * Fanal, "inertial resultant rho A w^2 L^2 / 2 = %.4f N (%.2e)", Fanal, fabs(il.resultant[0] - Fanal));
        IsostaticSupport sup;
        unsigned char *fixed = calloc((size_t)(3 * b.nn), 1);
        bool chosen = loads_isostatic_choose(&mv, NULL, 0, &sup);
        loads_isostatic_fixed(&sup, fixed);
        SolidMaterial mat = {3.5e9, 0.35, rho};
        HexModel hm = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mat, HEX8_INCOMPATIBLE, NULL};
        SolidLoads sl = {fixed, NULL, f, NULL, {0, 0, 0}};
        SolidOptions so = {SOLID_SOLVER_DIRECT, 1e-12, 0, 0, NULL, NULL, NULL};
        SolidResult res;
        memset(&res, 0, sizeof res);
        bool solved = chosen && ok && solid_solve(&hm, &sl, &so, &res, err, sizeof err);
        CHECK(solved, "spinning bar solves with isostatic supports (%s)", solved ? "" : err);
        if (solved) {
            double rmax = 0;
            for (int n = 0; n < 3 * b.nn; n++) rmax = fmax(rmax, fabs(res.reaction[n]));
            double mean, slope;
            slice_stress(&b, &res, L / 2 + (nx % 2 ? 0 : 0.5 * L / nx), &mean, &slope);
            double xs = L / 2 + 0.5 * L / nx, ref = rho * w * w * (L * L - xs * xs) / 2;
            REPORT("%d elements along: support reactions %.2e N (load %.3f N); mean sxx at x = %.1f mm %.1f Pa, analytic %.1f Pa (%.2f%%)", nx, rmax, Fanal,
                   xs * 1e3, mean, ref, 100 * (mean / ref - 1));
            CHECK(rmax < 1e-8 * Fanal, "isostatic reactions vanish for self-equilibrated loads");
            CHECK(fabs(mean / ref - 1) < 0.01, "centrifugal axial stress within 1%% of rho w^2 (L^2 - x^2) / 2");
            solid_result_free(&res);
        }
        free(fixed), free(f);
        free(b.xyz), free(b.rho), free(b.conn);
    }

    /* cantilever loaded only through transferred wrenches: tip force and the root reaction wrench */
    double P0 = 50, Lc = 0.12, Wc = 0.012, Hc = 0.012;
    b = box_hex(24, 3, 4, Lc, Wc, Hc, 1240);
    mv = (FemMeshView){b.nn, b.ne, b.xyz, b.conn, b.rho};
    f = calloc((size_t)(3 * b.nn), sizeof(double));
    double Ftip[3] = {0, 0, -P0}, tipc[3] = {Lc, 0, 0}, Froot[3] = {0, 0, P0}, rootc[3] = {0, 0, 0}, Mroot[3] = {0, -P0 * Lc, 0}, zero3[3] = {0, 0, 0};
    nf = end_faces(&b, true, fe, fl);
    fs = (FaceSet){nf, fe, fl};
    ok = loads_distribute_wrench(&mv, &fs, Ftip, zero3, tipc, f, NULL, err, sizeof err);
    nf = end_faces(&b, false, fe, fl);
    fs = (FaceSet){nf, fe, fl};
    ok &= loads_distribute_wrench(&mv, &fs, Froot, Mroot, rootc, f, NULL, err, sizeof err);
    IsostaticSupport sup;
    unsigned char *fixed = calloc((size_t)(3 * b.nn), 1);
    ok &= loads_isostatic_choose(&mv, NULL, 0, &sup);
    loads_isostatic_fixed(&sup, fixed);
    SolidMaterial mat = {3.5e9, 0.35, 1240};
    HexModel hm = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mat, HEX8_INCOMPATIBLE, NULL};
    SolidLoads sl = {fixed, NULL, f, NULL, {0, 0, 0}};
    SolidOptions so = {SOLID_SOLVER_DIRECT, 1e-12, 0, 0, NULL, NULL, NULL};
    SolidResult res;
    memset(&res, 0, sizeof res);
    bool solved = ok && solid_solve(&hm, &sl, &so, &res, err, sizeof err);
    CHECK(solved, "cantilever solves (%s)", solved ? "" : err);
    if (solved) {
        double mean, slope;
        double xs = Lc / 2 + 0.5 * Lc / 24;
        slice_stress(&b, &res, xs, &mean, &slope);
        double I = Wc * Hc * Hc * Hc / 12, ref = P0 * (Lc - xs) / I; /* d(sxx)/dz = M / I with M = P (L - x) */
        double rmax = 0;
        for (int n = 0; n < 3 * b.nn; n++) rmax = fmax(rmax, fabs(res.reaction[n]));
        REPORT("mid-span bending gradient %.4e Pa/m, beam theory %.4e Pa/m (%.2f%%), mean sxx %.2e Pa, support reactions %.1e N", slope, ref,
               100 * (slope / ref - 1), mean, rmax);
        CHECK(fabs(slope / ref - 1) < 0.02 && fabs(mean) < 1e-3 * ref * Hc && rmax < 1e-8 * P0, "bending stress gradient M/I within 2%% with zero support reactions");
        solid_result_free(&res);
    }
    free(fixed), free(f);
    free(b.xyz), free(b.rho), free(b.conn);
}


/* ---------------------------------------------------------------------------------------------- contact */

typedef struct CRun {
    MbModel *m;
    MbSim *sim;
    MbState *s;
    MechDiag d;
    MbContactRow rows[CT_MAX_POINTS];
    int sa[CT_MAX_POINTS], sb[CT_MAX_POINTS];
    int nrows;
    ContactReport rep;
    ContactWarm warm;
    bool cold; /* no warm start */
} CRun;

static bool crun_open(CRun *r, MbModelDef *def) {
    memset(r, 0, sizeof *r);
    mdiag_init(&r->d);
    MbOptions o;
    mb_options_default(&o);
    r->m = mb_compile(def, &o, &r->d);
    if (!r->m) return false;
    r->sim = mb_sim_new(r->m);
    r->s = mb_state_new(r->m);
    return contact_warm_init(&r->warm, CT_MAX_POINTS) && mb_state_initial(r->m, r->s, &r->d) && mb_assemble(r->sim, r->s, &r->d);
}

static void crun_close(CRun *r) {
    contact_warm_free(&r->warm);
    mb_state_free(r->s), mb_sim_free(r->sim), mb_model_free(r->m);
    mdiag_free(&r->d);
}

static bool cstep(CRun *r, double h, const MbInputs *in, const ContactShape *sh, int ns) {
    ContactOptions o;
    contact_options_default(&o);
    MbStepReport sr;
    bool ok = contact_step(r->sim, r->s, h, in, sh, ns, &o, r->rows, r->sa, r->sb, &r->nrows, &r->rep, &sr, r->cold ? NULL : &r->warm);
    if (!ok) printf("  contact step failed: %s\n", sr.error);
    return ok;
}

static MbModelDef *free_body_def(const char *name, double mass, const double I[9], double pos[3], double vel[3]) {
    MbModelDef *def = mbdef_new();
    int b = mbdef_add_body(def, name, mass, (double[3]){0, 0, 0}, I);
    int j = mbdef_add_joint(def, "float", MB_FREE, -1, b);
    mv3_copy(def->joints[j].q0, pos);
    if (vel) mv3_copy(def->joints[j].v0 + 3, vel); /* linear velocity in body axes (identity orientation) */
    mv3_set(def->gravity, 0, 0, -9.81);
    return def;
}

static void test_contact(void) {
    printf("== contact: resting, slip threshold, sliding, restitution, grasp, tunnelling (Moreau-Jean time stepping)\n");
    double g = 9.81;
    ContactShape floor = {.name = "floor", .type = SHAPE_PLANE, .body = -1, .friction = 0.5, .restitution = 0};
    mpose_identity(&floor.pose);
    /* sphere at rest on the floor */
    {
        double I[9];
        diag3(I, 1e-3, 1e-3, 1e-3);
        MbModelDef *def = free_body_def("ball", 1.0, I, (double[3]){0, 0, 0.05}, NULL);
        CRun r;
        bool ok = crun_open(&r, def);
        mbdef_free(def);
        ContactShape ball = {.name = "ball", .type = SHAPE_SPHERE, .body = 0, .radius = 0.05, .friction = 0.5};
        mpose_identity(&ball.pose);
        ContactShape sh[2] = {floor, ball};
        double ferr = 0;
        for (int k = 0; ok && k < 1000; k++) {
            ok = cstep(&r, 1e-3, NULL, sh, 2);
            double pn = 0;
            for (int i = 0; i < r.nrows; i++) pn += r.rows[i].impulse_normal;
            ferr = fmax(ferr, fabs(pn / 1e-3 - g));
        }
        CHECK(ok && fabs(r.s->q[2] - 0.05) < 1e-9 && fabs(r.s->v[5]) < 1e-9 && ferr < 1e-9,
              "sphere at rest: height kept (%.1e m), contact force = m g every step (%.1e N)", fabs(r.s->q[2] - 0.05), ferr);
        crun_close(&r);
    }
    /* box at rest: four corners carry m g in total */
    {
        double I[9];
        diag3(I, 2e-3, 2e-3, 3e-3);
        MbModelDef *def = free_body_def("block", 2.0, I, (double[3]){0, 0, 0.02}, NULL);
        CRun r;
        bool ok = crun_open(&r, def);
        mbdef_free(def);
        ContactShape box = {.name = "block", .type = SHAPE_BOX, .body = 0, .half = {0.05, 0.03, 0.02}, .friction = 0.5};
        mpose_identity(&box.pose);
        ContactShape sh[2] = {floor, box};
        for (int k = 0; ok && k < 500; k++) ok = cstep(&r, 1e-3, NULL, sh, 2);
        double pn = 0;
        for (int i = 0; i < r.nrows; i++) pn += r.rows[i].impulse_normal;
        REPORT("box at rest: %d contact points, total force %.9f N (m g = %.5f N), height drift %.1e m, penetration %.1e m", r.nrows, pn / 1e-3, 2 * g,
               r.s->q[2] - 0.02, r.rep.max_penetration);
        CHECK(ok && r.nrows == 4 && fabs(pn / 1e-3 - 2 * g) < 1e-8 && fabs(r.s->q[2] - 0.02) < 1e-9, "box at rest on four corners (force distribution among them is not unique)");
        crun_close(&r);
    }
    /* slip threshold on an incline (gravity tilted by theta, mu = 0.5, threshold atan 0.5 = 26.565 deg) */
    for (int k = 0; k < 2; k++) {
        double th = (k ? 28.0 : 25.0) * M_PI / 180, mu = 0.5;
        double I[9];
        diag3(I, 2e-3, 2e-3, 3e-3);
        MbModelDef *def = free_body_def("block", 1.0, I, (double[3]){0, 0, 0.02}, NULL);
        mv3_set(def->gravity, g * sin(th), 0, -g * cos(th));
        CRun r;
        bool ok = crun_open(&r, def);
        mbdef_free(def);
        ContactShape box = {.name = "block", .type = SHAPE_BOX, .body = 0, .half = {0.05, 0.03, 0.02}, .friction = mu};
        mpose_identity(&box.pose);
        ContactShape pl = floor;
        pl.friction = mu;
        ContactShape sh[2] = {pl, box};
        int maxit = 0, unconv = 0;
        for (int n = 0; ok && n < 1000; n++) {
            ok = cstep(&r, 1e-3, NULL, sh, 2);
            maxit = r.rep.solve.iterations > maxit ? r.rep.solve.iterations : maxit;
            unconv += !r.rep.solve.converged;
        }
        double x = r.s->q[0], aref = g * (sin(th) - mu * cos(th)), xref = 0.5 * aref * 1.0;
        if (!k) {
            REPORT("25 deg (below the threshold): displacement after 1 s %.2e m, contacts sticking %d", x, r.rows[0].state == 1);
            CHECK(ok && fabs(x) < 1e-9 && unconv == 0, "sticks below tan(theta) = mu (solver converged in every step, at most %d sweeps)", maxit);
        } else {
            REPORT("28 deg (above): displacement %.6f m, analytic 0.5 g (sin - mu cos) t^2 = %.6f m (%.2f%%), sliding %d", x, xref, 100 * (x / xref - 1),
                   r.rows[0].state == 2);
            CHECK(ok && fabs(x / xref - 1) < 0.01, "slides above the threshold with a = g (sin theta - mu cos theta)");
        }
        crun_close(&r);
    }
    /* sliding to rest: distance v0^2 / (2 mu g) */
    {
        double I[9], mu = 0.3, v0 = 2.0;
        diag3(I, 2e-3, 2e-3, 3e-3);
        MbModelDef *def = free_body_def("block", 1.0, I, (double[3]){0, 0, 0.02}, (double[3]){v0, 0, 0});
        CRun r;
        bool ok = crun_open(&r, def);
        mbdef_free(def);
        ContactShape box = {.name = "block", .type = SHAPE_BOX, .body = 0, .half = {0.05, 0.03, 0.02}, .friction = mu};
        mpose_identity(&box.pose);
        ContactShape pl = floor;
        pl.friction = mu;
        ContactShape sh[2] = {pl, box};
        double tstop = -1;
        for (int n = 0; ok && n < 12000; n++) {
            ok = cstep(&r, 1e-4, NULL, sh, 2);
            if (tstop < 0 && fabs(r.s->v[3]) < 1e-9) tstop = r.s->t;
        }
        double dref = v0 * v0 / (2 * mu * g), tref = v0 / (mu * g);
        MbEval *ev = mb_eval_new(r.m);
        mb_evaluate(r.sim, r.s, NULL, ev);
        REPORT("sliding stop: distance %.6f m (analytic %.6f, %.3f%%), stop time %.4f s (%.4f), friction dissipated %.6f J of %.6f J", r.s->q[0], dref,
               100 * (r.s->q[0] / dref - 1), tstop, tref, r.s->ledger.contact_friction, 0.5 * v0 * v0);
        CHECK(ok && fabs(r.s->q[0] / dref - 1) < 2e-3 && fabs(tstop - tref) < 2e-4, "stopping distance and time of Coulomb sliding");
        CHECK(fabs(r.s->ledger.contact_friction - 0.5 * v0 * v0) < 1e-3 * 0.5 * v0 * v0, "friction dissipation equals the initial kinetic energy");
        mb_eval_free(ev);
        crun_close(&r);
    }
    /* restitution: rebound apex e^2 h0; impulse (1 + e) m v; average force depends on h, impulse does not */
    {
        double e = 0.8, h0 = 0.5, rad = 0.02;
        double apex[2] = {0}, imp[2] = {0}, fav[2] = {0};
        for (int k = 0; k < 2; k++) {
            double h = k ? 1e-4 : 1e-3;
            double I[9];
            diag3(I, 1e-4, 1e-4, 1e-4);
            MbModelDef *def = free_body_def("ball", 0.1, I, (double[3]){0, 0, rad + h0}, NULL);
            CRun r;
            bool ok = crun_open(&r, def);
            mbdef_free(def);
            ContactShape ball = {.name = "ball", .type = SHAPE_SPHERE, .body = 0, .radius = rad, .friction = 0.5, .restitution = e};
            mpose_identity(&ball.pose);
            ContactShape sh[2] = {floor, ball};
            bool bounced = false;
            for (int n = 0; ok && n < (int)(1.2 / h); n++) {
                double vz = r.s->v[5];
                ok = cstep(&r, h, NULL, sh, 2);
                for (int i = 0; i < r.nrows; i++)
                    if (r.rows[i].impulse_normal > imp[k]) imp[k] = r.rows[i].impulse_normal, fav[k] = r.rows[i].impulse_normal / h;
                if (vz < 0 && r.s->v[5] > 0) bounced = true;
                if (bounced) apex[k] = fmax(apex[k], r.s->q[2] - rad);
            }
            crun_close(&r);
        }
        double vimp = sqrt(2 * g * h0), iref = 0.1 * (1 + e) * vimp;
        REPORT("bounce e = 0.8 from 0.5 m: apex %.5f m (h = 1 ms), %.5f m (h = 0.1 ms), analytic %.5f m; impulse %.5f / %.5f N s (analytic %.5f); "
               "average force %.0f / %.0f N (depends on h)", apex[0], apex[1], e * e * h0, imp[0], imp[1], iref, fav[0], fav[1]);
        CHECK(fabs(apex[1] / (e * e * h0) - 1) < 0.01 && fabs(apex[1] - e * e * h0) < fabs(apex[0] - e * e * h0) + 1e-12,
              "rebound height within 1%% at 0.1 ms and converging with the step");
        CHECK(fabs(imp[1] / iref - 1) < 0.01 && fav[1] > 5 * fav[0], "impulse independent of the step, average force not");
    }
    /* grasp: two fingers pressed on a block against gravity; holds when 2 mu F > m g */
    {
        double mu = 0.6, mass = 0.2, disp[2] = {0}, acc[2] = {0};
        double forces[2] = {2.5, 1.2};
        for (int k = 0; k < 2; k++) {
            MbModelDef *def = mbdef_new();
            double Io[9], If[9];
            diag3(Io, 1e-4, 1e-4, 1e-4);
            diag3(If, 5e-5, 5e-5, 5e-5);
            int obj = mbdef_add_body(def, "object", mass, (double[3]){0, 0, 0}, Io);
            int fl = mbdef_add_body(def, "finger_l", 0.05, (double[3]){0, 0, 0}, If);
            int fr = mbdef_add_body(def, "finger_r", 0.05, (double[3]){0, 0, 0}, If);
            mbdef_add_joint(def, "float", MB_FREE, -1, obj);
            int jl = mbdef_add_joint(def, "slide_l", MB_PRISMATIC, -1, fl);
            int jr = mbdef_add_joint(def, "slide_r", MB_PRISMATIC, -1, fr);
            mv3_set(def->joints[jl].parent_frame.p, 0, 0.03, 0), mv3_set(def->joints[jl].axis, 0, -1, 0);
            mv3_set(def->joints[jr].parent_frame.p, 0, -0.03, 0), mv3_set(def->joints[jr].axis, 0, 1, 0);
            mv3_set(def->gravity, 0, 0, -g);
            CRun r;
            bool ok = crun_open(&r, def);
            mbdef_free(def);
            ContactShape so = {.name = "object", .type = SHAPE_BOX, .body = obj, .half = {0.02, 0.02, 0.02}, .friction = mu};
            ContactShape sl = {.name = "pad_l", .type = SHAPE_BOX, .body = fl, .half = {0.01, 0.01, 0.03}, .friction = mu};
            ContactShape sr = {.name = "pad_r", .type = SHAPE_BOX, .body = fr, .half = {0.01, 0.01, 0.03}, .friction = mu};
            mpose_identity(&so.pose), mpose_identity(&sl.pose), mpose_identity(&sr.pose);
            ContactShape sh[3] = {so, sl, sr};
            double tau[8] = {0};
            tau[6] = forces[k], tau[7] = forces[k]; /* object has 6 velocity coordinates, then the two fingers */
            MbInputs in = {tau, NULL, NULL, NULL};
            double z0 = r.s->q[2];
            for (int n = 0; ok && n < 1000; n++) ok = cstep(&r, 1e-4, &in, sh, 3); /* 0.1 s: the block stays between the pads */
            disp[k] = r.s->q[2] - z0;
            acc[k] = 2 * disp[k] / (0.1 * 0.1);
            crun_close(&r);
        }
        double aslip = -(g - 2 * mu * forces[1] / mass);
        REPORT("grasp mu = 0.6, m = 0.2 kg: F = 2.5 N (2 mu F = 3.0 N > m g = 1.96 N) moves %.2e m in 0.1 s; F = 1.2 N slides with %.4f m/s^2 "
               "(analytic %.4f)", disp[0], acc[1], aslip);
        CHECK(fabs(disp[0]) < 1e-6, "held when 2 mu F exceeds the weight");
        CHECK(fabs(acc[1] / aslip - 1) < 0.02, "slips with a = g - 2 mu F / m otherwise");
    }
    /* tunnelling: 30 m/s towards a 1 mm plate with 1 ms steps (30 mm per step) */
    {
        double I[9];
        diag3(I, 1e-6, 1e-6, 1e-6);
        MbModelDef *def = free_body_def("pellet", 0.01, I, (double[3]){-0.5, 0, 0}, (double[3]){30, 0, 0});
        mv3_set(def->gravity, 0, 0, 0);
        CRun r;
        bool ok = crun_open(&r, def);
        mbdef_free(def);
        ContactShape pel = {.name = "pellet", .type = SHAPE_SPHERE, .body = 0, .radius = 0.005, .friction = 0};
        ContactShape plate = {.name = "plate", .type = SHAPE_BOX, .body = -1, .half = {0.0005, 0.1, 0.1}, .friction = 0};
        mpose_identity(&pel.pose), mpose_identity(&plate.pose);
        ContactShape sh[2] = {plate, pel};
        double xmax = -1;
        for (int n = 0; ok && n < 100; n++) {
            ok = cstep(&r, 1e-3, NULL, sh, 2);
            xmax = fmax(xmax, r.s->q[0]);
        }
        REPORT("pellet at 30 m/s against a 1 mm plate: furthest centre position %.6f m (plate face at -0.0005 m, radius 0.005 m)", xmax);
        CHECK(ok && xmax <= -0.0005 - 0.005 + 1e-9, "no tunnelling: speculative contacts widened by the distance travelled per step");
    }
    /* stack of four boxes: warm-started solver against a cold start */
    {
        double drift[2] = {0}, pen[2] = {0};
        long sweeps[2] = {0};
        int unconv[2] = {0}, late_unconv[2] = {0};
        for (int w = 0; w < 2; w++) {
            MbModelDef *def = mbdef_new();
            double I[9];
            diag3(I, 0.1 * 0.05 * 0.05 / 6, 0.1 * 0.05 * 0.05 / 6, 0.1 * 0.05 * 0.05 / 6);
            ContactShape sh[5];
            memset(sh, 0, sizeof sh);
            sh[0] = floor;
            for (int i = 0; i < 4; i++) {
                char nm[16];
                snprintf(nm, sizeof nm, "b%d", i);
                int b = mbdef_add_body(def, nm, 0.1, (double[3]){0, 0, 0}, I);
                snprintf(nm, sizeof nm, "f%d", i);
                int j = mbdef_add_joint(def, nm, MB_FREE, -1, b);
                def->joints[j].q0[2] = 0.025 + 0.05 * i;
                snprintf(sh[i + 1].name, sizeof sh[i + 1].name, "box%d", i);
                sh[i + 1].type = SHAPE_BOX, sh[i + 1].body = b, sh[i + 1].friction = 0.5;
                sh[i + 1].half[0] = sh[i + 1].half[1] = sh[i + 1].half[2] = 0.025;
                mpose_identity(&sh[i + 1].pose);
            }
            mv3_set(def->gravity, 0, 0, -9.81);
            CRun *r = malloc(sizeof *r);
            bool ok = r && crun_open(r, def);
            mbdef_free(def);
            if (!ok) {
                free(r);
                continue;
            }
            r->cold = w == 0;
            for (int n = 0; ok && n < 1000; n++) {
                ok = cstep(r, 1e-3, NULL, sh, 5);
                sweeps[w] += r->rep.solve.iterations;
                unconv[w] += !r->rep.solve.converged;
                late_unconv[w] += n >= 20 && !r->rep.solve.converged;
                pen[w] = fmax(pen[w], r->rep.max_penetration);
            }
            drift[w] = r->s->q[r->m->joint_qadr[3] + 2] - 0.175;
            crun_close(r);
            free(r);
        }
        REPORT("stack of 4 boxes for 1 s at h = 1 ms: cold start %ld sweeps, %d unconverged steps, top drift %.2e m, penetration %.2e m; "
               "warm start %ld sweeps, %d unconverged steps (%d after the first 20 ms), top drift %.2e m, penetration %.2e m",
               sweeps[0], unconv[0], drift[0], pen[0], sweeps[1], unconv[1], late_unconv[1], drift[1], pen[1]);
        CHECK(late_unconv[1] == 0 && fabs(drift[1]) < 1e-8 && sweeps[1] < sweeps[0] / 5,
              "warm start: the resting stack converges every step after settling, holds its height, and needs a fraction of the sweeps");
    }
}


/* ------------------------------------------------------------------------------------------------ contact in the study runtime */

static const char *GRASP_ASM =
    "{\"format\": \"navier-assembly\", \"version\": 1, \"name\": \"grasp\", \"units\": {\"length\": \"mm\", \"mass\": \"g\", \"inertia\": \"kg*m^2\"},\n"
    " \"gravity\": [0, 0, \"-9.81 m/s^2\"],\n"
    " \"bodies\": [\n"
    "  {\"name\": \"object\", \"mass_properties\": {\"provenance\": \"user\", \"mass\": 200, \"com\": [0, 0, 0],\n"
    "    \"inertia\": {\"ixx\": 5.3333e-5, \"iyy\": 5.3333e-5, \"izz\": 5.3333e-5, \"ixy\": 0, \"ixz\": 0, \"iyz\": 0}},\n"
    "   \"geometry\": {\"collision\": [{\"name\": \"object\", \"type\": \"box\", \"size\": [40, 40, 40], \"friction\": 0.6, \"friction_source\": \"user\"}]}},\n"
    "  {\"name\": \"finger_l\", \"mass_properties\": {\"provenance\": \"user\", \"mass\": 50, \"com\": [0, 0, 0],\n"
    "    \"inertia\": {\"ixx\": 5e-5, \"iyy\": 5e-5, \"izz\": 5e-5, \"ixy\": 0, \"ixz\": 0, \"iyz\": 0}},\n"
    "   \"geometry\": {\"collision\": [{\"name\": \"pad_l\", \"type\": \"box\", \"size\": [20, 20, 60], \"friction\": 0.6, \"friction_source\": \"user\"}]}},\n"
    "  {\"name\": \"finger_r\", \"mass_properties\": {\"provenance\": \"user\", \"mass\": 50, \"com\": [0, 0, 0],\n"
    "    \"inertia\": {\"ixx\": 5e-5, \"iyy\": 5e-5, \"izz\": 5e-5, \"ixy\": 0, \"ixz\": 0, \"iyz\": 0}},\n"
    "   \"geometry\": {\"collision\": [{\"name\": \"pad_r\", \"type\": \"box\", \"size\": [20, 20, 60], \"friction\": 0.6, \"friction_source\": \"user\"}]}}\n"
    " ],\n"
    " \"joints\": [\n"
    "  {\"name\": \"float\", \"type\": \"free\", \"parent\": \"world\", \"child\": \"object\", \"parent_frame\": {\"position\": [0, 0, 100]}, \"child_frame\": {}},\n"
    "  {\"name\": \"slide_l\", \"type\": \"prismatic\", \"parent\": \"world\", \"child\": \"finger_l\", \"parent_frame\": {\"position\": [0, 31, 100]},\n"
    "   \"child_frame\": {}, \"axis\": [0, -1, 0], \"motion\": \"actuated\"},\n"
    "  {\"name\": \"slide_r\", \"type\": \"prismatic\", \"parent\": \"world\", \"child\": \"finger_r\", \"parent_frame\": {\"position\": [0, -31, 100]},\n"
    "   \"child_frame\": {}, \"axis\": [0, 1, 0], \"motion\": \"actuated\"}\n"
    " ],\n"
    " \"environment\": [{\"name\": \"floor\", \"type\": \"plane\", \"friction\": 0.5, \"friction_source\": \"default\"}]}\n";

static const char *GRASP_STUDY =
    "{\"format\": \"navier-mech-study\", \"version\": 1, \"name\": \"hold\",\n"
    " \"settings\": {\"end_time\": \"0.3 s\", \"max_step\": \"0.2 ms\", \"record_period\": \"1 ms\", \"contact\": {\"enabled\": true}},\n"
    " \"snapshots\": [\"0.25 s\"],\n"
    " \"actuators\": [{\"name\": \"act_l\", \"type\": \"effort\", \"joint\": \"slide_l\", \"effort_limit\": \"5 N\"},\n"
    "               {\"name\": \"act_r\", \"type\": \"effort\", \"joint\": \"slide_r\", \"effort_limit\": \"5 N\"}],\n"
    " \"controllers\": [{\"name\": \"squeeze_l\", \"type\": \"open_loop\", \"actuator\": \"act_l\", \"period\": \"1 ms\", \"reference\": {\"type\": \"constant\", \"value\": \"2.5 N\"}},\n"
    "                 {\"name\": \"squeeze_r\", \"type\": \"open_loop\", \"actuator\": \"act_r\", \"period\": \"1 ms\", \"reference\": {\"type\": \"constant\", \"value\": \"2.5 N\"}}]}\n";

typedef struct GraspRun {
    Assembly *a;
    MbModelDef *def;
    MechStudy *study;
    ContactShape shapes[16];
    int nshapes;
    MechSim *ms;
} GraspRun;

static bool grasp_open(GraspRun *g, const char *asm_text, const char *study_text, MechDiag *d) {
    memset(g, 0, sizeof *g);
    JsonError je;
    JsonValue *ad = json_parse(asm_text, strlen(asm_text), NULL, &je), *sd = json_parse(study_text, strlen(study_text), NULL, &je);
    g->a = ad ? asm_from_json(ad, ".", NULL, d) : NULL;
    g->def = g->a ? asm_to_model(g->a, d) : NULL;
    g->study = sd ? mspec_from_json(sd, d) : NULL;
    json_free(ad), json_free(sd);
    if (!g->def || !g->study || !mspec_resolve(g->study, g->def, d)) return false;
    g->nshapes = asm_contact_shapes(g->a, g->def, g->shapes, 16, d);
    if (g->nshapes < 0) return false;
    MechStudy *s = g->study;
    g->ms = mechsim_new(g->def, NULL, s->act, s->nact, s->ctl, s->nctl, s->sen, s->nsen, &s->st, d);
    return g->ms && mechsim_set_contact_shapes(g->ms, g->shapes, g->nshapes) && mechsim_init(g->ms, d) &&
           mechsim_set_snapshot_times(g->ms, s->snapshot_s, s->nsnapshots);
}

static void grasp_close(GraspRun *g) {
    mechsim_free(g->ms), mspec_free(g->study), mbdef_free(g->def), asm_free(g->a);
    memset(g, 0, sizeof *g);
}

static double max_abs(const double *x, int n) {
    double m = 0;
    for (int i = 0; i < n; i++) m = fmax(m, fabs(x[i]));
    return m;
}

static void test_contact_runtime(void) {
    printf("== contact in the study runtime: grasp from an assembly document, evaluation with contact forces, impacts, snapshots, rollback\n");
    MechDiag d;
    mdiag_init(&d);
    GraspRun g;
    bool ok = grasp_open(&g, GRASP_ASM, GRASP_STUDY, &d);
    if (!ok) mdiag_print(&d, "  grasp");
    CHECK(ok && g.nshapes == 4, "assembly collision geometry and environment become 4 contact shapes (%d)", g.nshapes);
    CHECK(mdiag_has(&d, "CONTACT_PARAMETER_ASSUMED") && mdiag_has(&d, "RESTITUTION_ASSUMED"), "default friction and missing restitution are reported");
    mdiag_free(&d);
    if (!ok) {
        grasp_close(&g);
        return;
    }
    char err[256];
    MechSim *ms = g.ms;
    const MbModel *m = mechsim_model(ms);
    /* checkpoint at 0.1 s */
    ok = mechsim_run_until(ms, 0.1, err, sizeof err);
    size_t bs = mechsim_state_size(ms);
    uint8_t *ck = malloc(bs), *end1 = malloc(bs), *end2 = malloc(bs);
    ok = ok && ck && end1 && end2 && mechsim_save_state(ms, ck, bs);
    ok = ok && mechsim_run_until(ms, 0.3, err, sizeof err);
    if (!ok) printf("  run failed: %s\n", err);
    CHECK(ok, "grasp study runs to the end with contact");
    const MbState *st = mechsim_state(ms);
    const MbEval *ev = ok ? mechsim_evaluate(ms) : NULL;
    double dz = st->q[2], amax = ev ? max_abs(ev->qacc, m->nv) : 1e9; /* free-joint position relative to its frame at z = 100 mm */
    JsonValue *sum = mechsim_summary_json(ms);
    const JsonValue *co = json_get(sum, "contact"), *pairs = json_get(co, "pairs");
    double fl_end = 0, fr_end = 0, ft_end = 0, peak = 0, pimp = 0, cap_used = 0;
    int impacts = (int)json_get_int(co, "impact_steps", 0);
    for (size_t i = 0; i < json_len(pairs); i++) {
        const JsonValue *x = json_at(pairs, i);
        if (strcmp(json_get_str(x, "shape_a", ""), "object")) continue;
        bool left = !strcmp(json_get_str(x, "shape_b", ""), "pad_l"), right = !strcmp(json_get_str(x, "shape_b", ""), "pad_r");
        if (!left && !right) continue;
        if (left) fl_end = json_get_num(x, "normal_force_at_end_N", 0), peak = json_get_num(x, "peak_normal_force_N", 0), pimp = json_get_num(x, "peak_impact_impulse_Ns", 0);
        else fr_end = json_get_num(x, "normal_force_at_end_N", 0);
        ft_end += json_get_num(x, "friction_force_at_end_N", 0);
        cap_used += json_get_num(x, "friction_utilization_at_end", 9) * json_get_num(x, "friction_coefficient", 0) * json_get_num(x, "normal_force_at_end_N", 0);
    }
    REPORT("object moved %.3f mm while the pads closed and then held; end: normal forces %.9f / %.9f N (actuators 2.5 N), friction on both pads %.9f N "
           "(m g = %.9f N; the split between the pads is statically indeterminate); max |generalised acceleration| of the evaluation with contact forces %.2e; "
           "%d impact steps, peak impact impulse %.4f N s, peak persistent normal force %.4f N",
           dz * 1e3, fl_end, fr_end, ft_end, 0.2 * 9.81, amax, impacts, pimp, peak);
    CHECK(ok && fabs(dz) < 1e-3 && fabs(st->v[5]) < 1e-6, "the grasp holds the object (2 mu F = 3 N > m g = 1.96 N)");
    CHECK(fabs(fl_end - 2.5) < 1e-6 && fabs(fr_end - 2.5) < 1e-6 && fabs(ft_end - 0.2 * 9.81) < 1e-6,
          "static grasp: normal force = actuator force, friction on the pads carries the weight");
    CHECK(fabs(cap_used - 0.2 * 9.81) < 1e-6, "friction utilisation per pad x mu x normal force adds up to the weight (grasp utilisation %.4f)", 0.2 * 9.81 / (0.6 * 5));
    CHECK(amax < 1e-6, "the evaluation includes the contact forces: a held grasp has zero accelerations");
    double vtouch = sqrt(2 * (2.5 / 0.05) * 0.001), pref = 0.05 * vtouch; /* pad momentum after closing the 1 mm gap */
    CHECK(impacts >= 1 && fabs(pimp / pref - 1) < 0.1,
          "pads closing at speed are impact steps, reported as impulses: %.4f N s vs pad momentum %.4f N s (the squeeze force adds F h per impact step)", pimp, pref);
    /* the joint load envelope and peak snapshots exclude impact steps */
    JsonValue *snaps = mechsim_snapshots_json(ms);
    bool any_impulsive = false, have_peak_contact = false, have_requested = false;
    const JsonValue *requested = NULL;
    for (size_t i = 0; i < json_len(snaps); i++) {
        const JsonValue *x = json_at(snaps, i);
        any_impulsive |= json_get_bool(x, "contact_impulsive", false);
        if (!strcmp(json_get_str(x, "label", ""), "peak_contact:object.pad_l")) have_peak_contact = true;
        if (!strcmp(json_get_str(x, "kind", ""), "requested")) have_requested = true, requested = x;
    }
    CHECK(have_peak_contact && have_requested && !any_impulsive, "peak-contact and requested snapshots exist, none of them at an impact step");
    JsonValue *envj = mechsim_load_envelope_json(ms);
    bool env_note = false;
    for (size_t i = 0; i < json_len(envj); i++) env_note |= json_get(json_at(envj, i), "impact_force_impulse_Ns") != NULL;
    CHECK(env_note, "joint load envelope reports impact impulses separately");
    CHECK(mechsim_channel_index(ms, "contact.object.pad_l.normal_force") >= 0 && mechsim_channel_index(ms, "contact.object.floor.min_gap") >= 0 &&
              mechsim_channel_index(ms, "contact.pad_l.floor.normal_force") < 0,
          "contact histories per pair; pads jointed to the world never touch the world's floor");
    /* requested snapshot restored in a fresh runtime: contact loads + gravity balance the object's motion */
    double bal = 1e9, amax2 = 1e9, tsnap = -1;
    if (requested) {
        MechDiag d2;
        mdiag_init(&d2);
        GraspRun g2;
        bool ok2 = grasp_open(&g2, GRASP_ASM, GRASP_STUDY, &d2);
        const char *b64 = json_get_str(requested, "state_base64", "");
        size_t blen = 0;
        unsigned char *blob = base64_decode(b64, strlen(b64), &blen);
        ok2 = ok2 && blob && mechsim_restore_snapshot(g2.ms, blob, blen);
        const MbEval *ev2 = ok2 ? mechsim_evaluate(g2.ms) : NULL;
        if (ev2) {
            amax2 = max_abs(ev2->qacc, m->nv), tsnap = mechsim_time(g2.ms);
            const JsonValue *rows = json_get(requested, "contact_rows");
            double h = json_get_num(requested, "contact_step_s", 0), F[3] = {0, 0, -0.2 * 9.81};
            for (size_t r = 0; r < json_len(rows); r++) {
                const JsonValue *cr = json_at(rows, r);
                double n[3], pt[3];
                json_get_numbers(json_get(cr, "normal"), n, 3), json_get_numbers(json_get(cr, "impulse_tangent"), pt, 3);
                double sg = !strcmp(json_get_str(cr, "shape_a", ""), "object") ? 1 : (!strcmp(json_get_str(cr, "shape_b", ""), "object") ? -1 : 0);
                for (int k = 0; k < 3; k++) F[k] += sg * (json_get_num(cr, "impulse_normal", 0) * n[k] + pt[k]) / h;
            }
            bal = mv3_norm(F); /* m a = 0 for the held object */
        }
        free(blob);
        grasp_close(&g2);
        mdiag_free(&d2);
    }
    REPORT("snapshot at t = %.6f s restored in a new runtime: max |acceleration| %.2e, object force balance from the stored contact rows %.2e N", tsnap, amax2, bal);
    CHECK(fabs(tsnap - 0.25) < 1e-12 && amax2 < 1e-6 && bal < 1e-6, "stored contact rows and the restored state give balanced loads");
    /* rollback: restore the 0.1 s checkpoint and replay */
    ok = mechsim_save_state(ms, end1, bs);
    char *sum1 = json_dump(sum, JSON_SORTED, NULL, NULL);
    bool replay = ok && mechsim_restore_state(ms, ck, bs) && mechsim_run_until(ms, 0.3, err, sizeof err) && mechsim_save_state(ms, end2, bs);
    JsonValue *sumb = mechsim_summary_json(ms);
    char *sum2 = json_dump(sumb, JSON_SORTED, NULL, NULL);
    CHECK(replay && !memcmp(end1, end2, bs) && sum1 && sum2 && !strcmp(sum1, sum2), "restored checkpoint replays the contact run bitwise (state and summary)");
    free(sum1), free(sum2), json_free(sumb), json_free(sum), json_free(snaps), json_free(envj);
    free(ck), free(end1), free(end2);
    grasp_close(&g);
    /* a weak grasp (2 mu F = 1.44 N < m g) slips: the block slides out, falls and comes to rest on the floor */
    {
        char study[4096];
        const char *at = strstr(GRASP_STUDY, "\"0.3 s\"");
        snprintf(study, sizeof study, "%.*s\"1.0 s\"%s", (int)(at - GRASP_STUDY), GRASP_STUDY, at + strlen("\"0.3 s\""));
        for (char *q = strstr(study, "\"2.5 N\""); q; q = strstr(q, "\"2.5 N\"")) memcpy(q, "\"1.2 N\"", 7);
        MechDiag d4;
        mdiag_init(&d4);
        GraspRun gw;
        bool okw = grasp_open(&gw, GRASP_ASM, study, &d4) && mechsim_run_until(gw.ms, 1.0, err, sizeof err);
        double z = okw ? mechsim_state(gw.ms)->q[2] : 1, ul = -1, slide = 0, floor_n = 0;
        JsonValue *sw = okw ? mechsim_summary_json(gw.ms) : NULL;
        const JsonValue *pw = json_get(json_get(sw, "contact"), "pairs");
        for (size_t i = 0; i < json_len(pw); i++) {
            const JsonValue *x = json_at(pw, i);
            if (!strcmp(json_get_str(x, "shape_b", ""), "pad_l")) ul = json_get_num(x, "peak_friction_utilization", -1), slide = json_get_num(x, "sliding_time_s", 0);
            if (!strcmp(json_get_str(x, "shape_b", ""), "floor")) floor_n = json_get_num(x, "normal_force_at_end_N", 0);
        }
        REPORT("weak grasp F = 1.2 N: pad friction utilisation peaks at %.6f, sliding for %.3f s; the block ends %.6f mm below its start (on the floor: -80 mm) "
               "with floor force %.6f N (m g = 1.962 N)", ul, slide, z * 1e3, floor_n);
        CHECK(okw && fabs(ul - 1) < 1e-9 && slide > 0.05 && fabs(z + 0.080) < 1e-6 && fabs(floor_n - 0.2 * 9.81) < 1e-6,
              "a grasp below the friction limit slips (utilisation 1), the block falls and rests on the environment floor");
        json_free(sw);
        grasp_close(&gw);
        mdiag_free(&d4);
    }
    /* refusals: mesh collision geometry, missing friction, duplicate names */
    static const struct {
        const char *from, *to, *code;
    } BAD[] = {
        {"{\"name\": \"object\", \"type\": \"box\", \"size\": [40, 40, 40], \"friction\": 0.6, \"friction_source\": \"user\"}",
         "{\"name\": \"object\", \"type\": \"mesh\", \"file\": \"object.stl\", \"units\": \"mm\", \"friction\": 0.6, \"friction_source\": \"user\"}", "UNSUPPORTED_CONTACT_GEOMETRY"},
        {"{\"name\": \"pad_l\", \"type\": \"box\", \"size\": [20, 20, 60], \"friction\": 0.6, \"friction_source\": \"user\"}",
         "{\"name\": \"pad_l\", \"type\": \"box\", \"size\": [20, 20, 60]}", "CONTACT_FRICTION_REQUIRED"},
        {"\"name\": \"pad_r\"", "\"name\": \"pad_l\"", "DUPLICATE_CONTACT_SHAPE"},
        {"\"friction\": 0.5, \"friction_source\": \"default\"", "\"friction\": 0.5", "CONTACT_PROVENANCE_REQUIRED"},
    };
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        char text[8192];
        const char *at = strstr(GRASP_ASM, BAD[i].from);
        if (!at) {
            CHECK(false, "test text for %s", BAD[i].code);
            continue;
        }
        snprintf(text, sizeof text, "%.*s%s%s", (int)(at - GRASP_ASM), GRASP_ASM, BAD[i].to, at + strlen(BAD[i].from));
        MechDiag d3;
        mdiag_init(&d3);
        GraspRun g3;
        bool ok3 = grasp_open(&g3, text, GRASP_STUDY, &d3);
        CHECK(!ok3 && mdiag_has(&d3, BAD[i].code), "refused with %s", BAD[i].code);
        grasp_close(&g3);
        mdiag_free(&d3);
    }
}

/* ---------------------------------------------------------------------------------------------- orthotropic printed parts */

/* illustrative constants for tests only (not measured data): axis 3 is the build direction */
static const OrthoConstants ORTHO_TEST = {{3.2e9, 3.0e9, 2.4e9}, 0.35, 0.30, 0.32, 1.15e9, 0.85e9, 0.90e9};

static void rot_from_quat(double w, double x, double y, double z, double R[9]) {
    double n = sqrt(w * w + x * x + y * y + z * z);
    w /= n, x /= n, y /= n, z /= n;
    double r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),
                   2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                   2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y)};
    memcpy(R, r, sizeof r);
}

static double mat6_rel_diff(const double A[6][6], const double B[6][6]) {
    double d = 0, s = 0;
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++) d = fmax(d, fabs(A[i][j] - B[i][j])), s = fmax(s, fabs(B[i][j]));
    return s > 0 ? d / s : d;
}

/* consistent nodal loads of a uniform traction on the x = 0 or x = L end */
static void end_traction(const BoxMesh *b, bool tip, const double t[3], double *F) {
    int elem[4096];
    unsigned char local[4096];
    int n = end_faces(b, tip, elem, local);
    for (int i = 0; i < n; i++) {
        double X[8][3], fe[24], area, nrm[3];
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) X[a][k] = b->xyz[3 * b->conn[8 * elem[i] + a] + k];
        hex8_face_load(X, local[i], t, fe, &area, nrm);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) F[3 * b->conn[8 * elem[i] + a] + k] += fe[3 * a + k];
    }
}

static void test_orthotropic(void) {
    printf("== orthotropic printed parts: constitutive matrices, material axes, patch tests, off-axis coupon, beams, strength criteria\n");
    char why[300];
    double S[6][6], C[6][6];
    bool ok = ortho_compliance(&ORTHO_TEST, S, why, sizeof why) && ortho_stiffness(&ORTHO_TEST, C, why, sizeof why);
    double cs = 0;
    for (int i = 0; ok && i < 6; i++)
        for (int j = 0; j < 6; j++) {
            double v = 0;
            for (int k = 0; k < 6; k++) v += C[i][k] * S[k][j];
            cs = fmax(cs, fabs(v - (i == j)));
        }
    CHECK(ok && cs < 1e-12, "C S = I (%.1e)", cs);
    CHECK(fabs(1 / S[2][2] / 2.4e9 - 1) < 1e-14 && fabs(-S[0][1] * 3.2e9 - 0.35) < 1e-14 && fabs(-S[1][2] * 3.0e9 - 0.32) < 1e-14 && fabs(1 / S[4][4] / 0.85e9 - 1) < 1e-14,
          "engineering constants recovered from the compliance");
    OrthoConstants bad = ORTHO_TEST;
    bad.E[1] = 3.2e9, bad.nu12 = 1.05;
    CHECK(!ortho_stiffness(&bad, C, why, sizeof why), "inadmissible Poisson ratio refused: %s", why);
    OrthoConstants iso;
    ortho_isotropic(2.0e9, 0.36, &iso);
    double Ci[6][6], Di[6][6];
    ortho_stiffness(&iso, Ci, NULL, 0);
    isotropic_D(2.0e9, 0.36, Di);
    CHECK(mat6_rel_diff(Ci, Di) < 1e-12, "isotropic constants reproduce the shared isotropic matrix (%.1e)", mat6_rel_diff(Ci, Di));
    /* rotations: energy invariance and the stress rotation agree with the strain transformation */
    ortho_stiffness(&ORTHO_TEST, C, NULL, 0);
    double R[9], T[6][6], D[6][6];
    rot_from_quat(0.83, -0.21, 0.37, 0.29, R);
    voigt_strain_transform(R, T);
    ortho_global_D(C, R, D);
    const double eg[6] = {1.3e-3, -0.4e-3, 0.7e-3, 2.1e-3, -0.9e-3, 0.5e-3};
    double em[6], sg[6], sm[6], sm_ref[6], wg = 0, wm = 0;
    for (int i = 0; i < 6; i++) {
        em[i] = 0, sg[i] = 0;
        for (int k = 0; k < 6; k++) em[i] += T[i][k] * eg[k], sg[i] += D[i][k] * eg[k];
    }
    for (int i = 0; i < 6; i++) {
        sm_ref[i] = 0;
        for (int k = 0; k < 6; k++) sm_ref[i] += C[i][k] * em[k];
        wg += eg[i] * sg[i], wm += em[i] * sm_ref[i];
    }
    voigt_stress_to_material(R, sg, sm);
    double dsm = 0, ssm = 0;
    for (int i = 0; i < 6; i++) dsm = fmax(dsm, fabs(sm[i] - sm_ref[i])), ssm = fmax(ssm, fabs(sm_ref[i]));
    CHECK(fabs(wg / wm - 1) < 1e-12 && dsm < 1e-12 * ssm, "rotated material: strain energy invariant (%.1e), stress in material axes = C T eps (%.1e)", fabs(wg / wm - 1),
          dsm / ssm);
    OrthoConstants ti = {{3.1e9, 3.1e9, 2.3e9}, 0.34, 0.31, 0.31, 3.1e9 / (2 * 1.34), 0.8e9, 0.8e9};
    double Ct[6][6], Dt[6][6], Rz[9];
    ortho_stiffness(&ti, Ct, NULL, 0);
    rot_from_quat(cos(0.37), 0, 0, sin(0.37), Rz);
    ortho_global_D(Ct, Rz, Dt);
    CHECK(mat6_rel_diff(Dt, Ct) < 1e-12, "transversely isotropic material is invariant under rotation about the build direction (%.1e)", mat6_rel_diff(Dt, Ct));
    double Rp[9];
    bool pa = ortho_print_axes((double[3]){0, 0, 2}, (double[3]){1, 0, 0.5}, 30 * M_PI / 180, Rp);
    double det = Rp[0] * (Rp[4] * Rp[8] - Rp[5] * Rp[7]) - Rp[1] * (Rp[3] * Rp[8] - Rp[5] * Rp[6]) + Rp[2] * (Rp[3] * Rp[7] - Rp[4] * Rp[6]);
    CHECK(pa && fabs(Rp[0] - cos(M_PI / 6)) < 1e-15 && fabs(Rp[3] - 0.5) < 1e-15 && fabs(Rp[8] - 1) < 1e-15 && fabs(det - 1) < 1e-14,
          "print axes: axis 3 along the build direction, axis 1 at the raster angle in the layer plane, right-handed");
    CHECK(!ortho_print_axes((double[3]){0, 0, 1}, (double[3]){0, 0, 3}, 0, Rp), "a raster reference parallel to the build direction is refused");

    /* patch test: homogeneous strain on a distorted 2x2x2 mesh with a rotated orthotropic material */
    for (int form = 0; form < 2; form++) {
        BoxMesh b = box_hex(2, 2, 2, 0.02, 0.02, 0.02, 1000);
        double *xyz = b.xyz;
        int mid = bnode(&b, 1, 1, 1);
        xyz[3 * mid] += 0.0017, xyz[3 * mid + 1] -= 0.0011, xyz[3 * mid + 2] += 0.0013;
        const double G[9] = {1.1e-3, 0.4e-3, -0.3e-3, -0.2e-3, 0.8e-3, 0.5e-3, 0.6e-3, -0.7e-3, -0.9e-3};
        unsigned char *fixed = calloc(3 * (size_t)b.nn, 1);
        double *fv = calloc(3 * (size_t)b.nn, sizeof(double));
        for (int nd = 0; nd < b.nn; nd++) {
            if (nd == mid) continue;
            for (int k = 0; k < 3; k++) {
                fixed[3 * nd + k] = 1;
                fv[3 * nd + k] = G[3 * k] * xyz[3 * nd] + G[3 * k + 1] * xyz[3 * nd + 1] + G[3 * k + 2] * xyz[3 * nd + 2];
            }
        }
        OrthoModel om = {b.nn, b.ne, xyz, b.conn, 1, &ORTHO_TEST, NULL, R, 1, form ? HEX8_INCOMPATIBLE : HEX8_FULL};
        OrthoLoads ol = {fixed, fv, NULL};
        OrthoResult res;
        char err[300];
        bool sok = ortho_solve(&om, &ol, NULL, &res, err, sizeof err);
        double du = 0, umax = 0, dsig = 0, smax = 0;
        const double e0[6] = {G[0], G[4], G[8], G[1] + G[3], G[5] + G[7], G[6] + G[2]};
        double s0[6];
        for (int i = 0; i < 6; i++) {
            s0[i] = 0;
            for (int k = 0; k < 6; k++) s0[i] += D[i][k] * e0[k];
        }
        for (int k = 0; sok && k < 3; k++) {
            double ex = G[3 * k] * xyz[3 * mid] + G[3 * k + 1] * xyz[3 * mid + 1] + G[3 * k + 2] * xyz[3 * mid + 2];
            du = fmax(du, fabs(res.u[3 * mid + k] - ex)), umax = fmax(umax, fabs(ex));
        }
        for (int e = 0; sok && e < b.ne; e++)
            for (int g = 0; g < 48; g++) dsig = fmax(dsig, fabs(res.gp_stress[48 * e + g] - s0[g % 6])), smax = fmax(smax, fabs(s0[g % 6]));
        if (!sok) printf("  solve failed: %s\n", err);
        CHECK(sok && du < 1e-12 * umax + 1e-18 && dsig < 1e-9 * smax, "patch test (%s): interior node exact (%.1e m), stress = D eps everywhere (%.1e relative)",
              form ? "incompatible modes" : "full integration", du, dsig / smax);
        if (sok) ortho_result_free(&res);
        free(fixed), free(fv), free(b.xyz), free(b.rho), free(b.conn);
    }

    /* off-axis tensile coupon: raster at theta in the layer plane, uniform end tractions, isostatic support */
    {
        OrthoConstants ua = {{3.2e9, 2.2e9, 2.0e9}, 0.35, 0.33, 0.34, 0.9e9, 0.7e9, 0.75e9};
        double worst = 0;
        char line[400] = "";
        size_t used = 0;
        for (int ti2 = 0; ti2 < 4; ti2++) {
            double th = (double[4]){0, 30, 45, 90}[ti2] * M_PI / 180, Ra[9];
            ortho_print_axes((double[3]){0, 0, 1}, (double[3]){1, 0, 0}, th, Ra);
            BoxMesh b = box_hex(8, 2, 2, 0.04, 0.01, 0.01, 1000);
            unsigned char *fixed = calloc(3 * (size_t)b.nn, 1);
            double *F = calloc(3 * (size_t)b.nn, sizeof(double)), sigma = 1e6;
            end_traction(&b, true, (double[3]){sigma, 0, 0}, F);
            end_traction(&b, false, (double[3]){-sigma, 0, 0}, F);
            int A = bnode(&b, 0, 0, 0), B = bnode(&b, 8, 0, 0), Cn = bnode(&b, 0, 2, 0);
            fixed[3 * A] = fixed[3 * A + 1] = fixed[3 * A + 2] = 1;
            fixed[3 * B + 1] = fixed[3 * B + 2] = 1;
            fixed[3 * Cn + 2] = 1;
            OrthoModel om = {b.nn, b.ne, b.xyz, b.conn, 1, &ua, NULL, Ra, 1, HEX8_INCOMPATIBLE};
            OrthoLoads ol = {fixed, NULL, F};
            OrthoResult res;
            char err[300];
            if (!ortho_solve(&om, &ol, NULL, &res, err, sizeof err)) {
                printf("  solve failed: %s\n", err);
                worst = 1;
            } else {
                double ux0 = 0, uxL = 0;
                int cnt = 0;
                for (int k = 0; k <= 2; k++)
                    for (int j = 0; j <= 2; j++, cnt++) ux0 += res.u[3 * bnode(&b, 0, j, k)], uxL += res.u[3 * bnode(&b, 8, j, k)];
                double Ex = sigma / ((uxL - ux0) / cnt / 0.04), c = cos(th), s2 = sin(th);
                double Eref = 1 / (pow(c, 4) / ua.E[0] + (1 / ua.G12 - 2 * ua.nu12 / ua.E[0]) * s2 * s2 * c * c + pow(s2, 4) / ua.E[1]);
                worst = fmax(worst, fabs(Ex / Eref - 1));
                double rmax = 0;
                for (int i = 0; i < 3 * b.nn; i++) rmax = fmax(rmax, fabs(res.reaction[i]));
                worst = fmax(worst, rmax / (sigma * 1e-4));
                if (used < sizeof line - 80) used += (size_t)snprintf(line + used, sizeof line - used, "%s%.0f deg %.4f GPa (%.4f)", used ? ", " : "", th * 180 / M_PI, Ex / 1e9, Eref / 1e9);
                ortho_result_free(&res);
            }
            free(fixed), free(F), free(b.xyz), free(b.rho), free(b.conn);
        }
        REPORT("off-axis coupon modulus E_x(theta) from the displacements (closed form in brackets): %s", line);
        CHECK(worst < 1e-9, "off-axis modulus exact and support reactions vanish (worst %.1e)", worst);
    }

    /* isotropic limit against the shared isotropic solver on a cantilever */
    {
        BoxMesh b = box_hex(10, 2, 2, 0.1, 0.01, 0.01, 1000);
        unsigned char *fixed = calloc(3 * (size_t)b.nn, 1);
        double *F = calloc(3 * (size_t)b.nn, sizeof(double));
        for (int k = 0; k <= 2; k++)
            for (int j = 0; j <= 2; j++)
                for (int c = 0; c < 3; c++) fixed[3 * bnode(&b, 0, j, k) + c] = 1;
        end_traction(&b, true, (double[3]){0, 2e4, -1e4}, F);
        OrthoConstants k;
        ortho_isotropic(2.1e9, 0.37, &k);
        OrthoModel om = {b.nn, b.ne, b.xyz, b.conn, 1, &k, NULL, NULL, 1, HEX8_INCOMPATIBLE};
        OrthoLoads ol = {fixed, NULL, F};
        OrthoResult ro;
        SolidMaterial sm = {2.1e9, 0.37, 1000};
        HexModel hm = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &sm, HEX8_INCOMPATIBLE, NULL};
        SolidLoads sl = {fixed, NULL, F, NULL, {0, 0, 0}};
        SolidResult rs;
        char err[300];
        bool a = ortho_solve(&om, &ol, NULL, &ro, err, sizeof err), bb = solid_solve(&hm, &sl, NULL, &rs, err, sizeof err);
        double du = 0, umax = 0, ds = 0, smx = 0;
        for (int i = 0; a && bb && i < 3 * b.nn; i++) du = fmax(du, fabs(ro.u[i] - rs.u[i])), umax = fmax(umax, fabs(rs.u[i]));
        for (int i = 0; a && bb && i < 48 * b.ne; i++) ds = fmax(ds, fabs(ro.gp_stress[i] - rs.gp_stress[i])), smx = fmax(smx, fabs(rs.gp_stress[i]));
        CHECK(a && bb && du < 1e-9 * umax && ds < 1e-8 * smx, "isotropic limit equals the shared solver: displacements %.1e, stresses %.1e relative", du / umax, ds / smx);
        CHECK(a && ro.equilibrium_error < 1e-9 && fabs(ro.strain_energy / (0.5 * ro.external_work) - 1) < 1e-9,
              "equilibrium %.1e; strain energy = half the external work (%.1e)", ro.equilibrium_error, fabs(ro.strain_energy / (0.5 * ro.external_work) - 1));
        if (a) ortho_result_free(&ro);
        if (bb) solid_result_free(&rs);
        free(fixed), free(F), free(b.xyz), free(b.rho), free(b.conn);
    }

    /* printed cantilever: flat (axis 1 along the length) and upright (build direction along the length) against Timoshenko */
    {
        OrthoStrength st = {50e6, 60e6, 45e6, 55e6, 25e6, 50e6, 25e6, 15e6, 15e6, -0.5, -0.5, -0.5};
        double tip[2] = {0}, ref[2] = {0}, ratio[2] = {0};
        int mode[2] = {-1, -1};
        for (int orient = 0; orient < 2; orient++) {
            double L = 0.2, h = 0.01, P = 1.0, Ro[9];
            if (orient == 0)
                ortho_print_axes((double[3]){0, 0, 1}, (double[3]){1, 0, 0}, 0, Ro); /* layers in xy, axis 1 along x */
            else
                ortho_print_axes((double[3]){1, 0, 0}, (double[3]){0, 1, 0}, 0, Ro); /* built along x: axis 3 along the length */
            BoxMesh b = box_hex(80, 4, 4, L, h, h, 1000);
            unsigned char *fixed = calloc(3 * (size_t)b.nn, 1);
            double *F = calloc(3 * (size_t)b.nn, sizeof(double));
            for (int k = 0; k <= 4; k++)
                for (int j = 0; j <= 4; j++)
                    for (int c = 0; c < 3; c++) fixed[3 * bnode(&b, 0, j, k) + c] = 1;
            end_traction(&b, true, (double[3]){0, 0, -P / (h * h)}, F);
            OrthoModel om = {b.nn, b.ne, b.xyz, b.conn, 1, &ORTHO_TEST, NULL, Ro, 1, HEX8_INCOMPATIBLE};
            OrthoLoads ol = {fixed, NULL, F};
            OrthoResult res;
            char err[300];
            if (ortho_solve(&om, &ol, NULL, &res, err, sizeof err)) {
                double w = 0;
                for (int k = 0; k <= 4; k++)
                    for (int j = 0; j <= 4; j++) w += res.u[3 * bnode(&b, 80, j, k) + 2];
                tip[orient] = -w / 25;
                double Ealong = orient == 0 ? ORTHO_TEST.E[0] : ORTHO_TEST.E[2], Gxz = orient == 0 ? ORTHO_TEST.G13 : ORTHO_TEST.G13;
                if (orient == 1) Gxz = ORTHO_TEST.G23; /* axis 3 along x, axis 2 along z (x_ref = y gives axis 1 = y) */
                double I = h * h * h * h / 12, A = h * h;
                ref[orient] = P * L * L * L / (3 * Ealong * I) + P * L / (5.0 / 6.0 * Gxz * A);
                StrengthSummary sum;
                ortho_strength_field(&res, b.ne, STRENGTH_TSAI_WU, &st, NULL, &sum);
                ratio[orient] = sum.min_ratio, mode[orient] = sum.mode;
                ortho_result_free(&res);
            } else
                printf("  solve failed: %s\n", err);
            free(fixed), free(F), free(b.xyz), free(b.rho), free(b.conn);
        }
        REPORT("200 mm printed cantilever, 10 mm square, 1 N tip load: flat tip deflection %.4f mm (Timoshenko %.4f, %+.2f%%), upright %.4f mm (%.4f, %+.2f%%); "
               "Tsai-Wu strength ratio flat %.1f (%s), upright %.1f (%s)",
               tip[0] * 1e3, ref[0] * 1e3, 100 * (tip[0] / ref[0] - 1), tip[1] * 1e3, ref[1] * 1e3, 100 * (tip[1] / ref[1] - 1), ratio[0], strength_mode_name(mode[0]),
               ratio[1], strength_mode_name(mode[1]));
        CHECK(fabs(tip[0] / ref[0] - 1) < 0.02 && fabs(tip[1] / ref[1] - 1) < 0.02, "tip deflections within 2%% of Timoshenko beam theory with the modulus along the length");
        CHECK(mode[0] == 0 && mode[1] == 4 && ratio[1] < ratio[0],
              "flat print is governed by tension along the raster, upright print by interlayer tension at a lower strength ratio");
    }

    /* strength criteria against closed forms */
    {
        OrthoStrength st = {50e6, 60e6, 45e6, 55e6, 25e6, 50e6, 25e6, 15e6, 15e6, -0.5, -0.5, -0.5};
        StrengthResult r;
        strength_evaluate(STRENGTH_MAX_STRESS, &st, (double[6]){25e6, 0, 0, 0, 0, 0}, &r);
        bool ms = fabs(r.index - 0.5) < 1e-15 && r.mode == 0;
        strength_evaluate(STRENGTH_MAX_STRESS, &st, (double[6]){0, 0, -10e6, 0, 15e6, 0}, &r);
        ms &= fabs(r.index - 1) < 1e-15 && r.mode == 7;
        CHECK(ms, "maximum stress: ratios and governing components");
        double tw = 0;
        const double uni[6][6] = {{50e6, 0, 0, 0, 0, 0}, {-60e6, 0, 0, 0, 0, 0}, {0, 0, 25e6, 0, 0, 0}, {0, 0, -50e6, 0, 0, 0}, {0, 0, 0, 25e6, 0, 0}, {0, 0, 0, 0, 0, -15e6}};
        for (int i = 0; i < 6; i++) {
            strength_evaluate(STRENGTH_TSAI_WU, &st, uni[i], &r);
            tw = fmax(tw, fabs(r.index - 1));
        }
        const double mix[6] = {20e6, -12e6, 8e6, 6e6, -4e6, 3e6};
        strength_evaluate(STRENGTH_TSAI_WU, &st, mix, &r);
        double F1 = 1 / st.Xt - 1 / st.Xc, F2 = 1 / st.Yt - 1 / st.Yc, F3 = 1 / st.Zt - 1 / st.Zc, F11 = 1 / (st.Xt * st.Xc), F22 = 1 / (st.Yt * st.Yc),
               F33 = 1 / (st.Zt * st.Zc);
        double sc[6];
        for (int i = 0; i < 6; i++) sc[i] = r.ratio * mix[i];
        double val = F1 * sc[0] + F2 * sc[1] + F3 * sc[2] + F11 * sc[0] * sc[0] + F22 * sc[1] * sc[1] + F33 * sc[2] * sc[2] + sc[3] * sc[3] / (st.S12 * st.S12) +
                     sc[4] * sc[4] / (st.S23 * st.S23) + sc[5] * sc[5] / (st.S31 * st.S31) - (sqrt(F11 * F22) * sc[0] * sc[1] + sqrt(F11 * F33) * sc[0] * sc[2] + sqrt(F22 * F33) * sc[1] * sc[2]);
        CHECK(tw < 1e-12 && fabs(val - 1) < 1e-12, "Tsai-Wu: index 1 at every uniaxial and shear strength (%.1e); the strength ratio of a mixed state lands on the envelope (%.1e)", tw,
              fabs(val - 1));
        OrthoStrength sh = {50e6, 50e6, 30e6, 30e6, 30e6, 30e6, 20e6, 20e6, 20e6, -0.5, -0.5, -0.5};
        double th_worst = 0;
        for (int i = 0; i <= 6; i++) {
            double th = 15.0 * i * M_PI / 180, c = cos(th), s1 = sin(th), Ra[9], sg[6] = {1, 0, 0, 0, 0, 0}, smat[6];
            ortho_print_axes((double[3]){0, 0, 1}, (double[3]){1, 0, 0}, th, Ra);
            voigt_stress_to_material(Ra, sg, smat);
            strength_evaluate(STRENGTH_TSAI_HILL, &sh, smat, &r);
            double closed = 1 / sqrt(pow(c, 4) / (50e6 * 50e6) + (1 / (20e6 * 20e6) - 1 / (50e6 * 50e6)) * c * c * s1 * s1 + pow(s1, 4) / (30e6 * 30e6));
            th_worst = fmax(th_worst, fabs(r.ratio / closed - 1));
        }
        CHECK(th_worst < 1e-12, "Tsai-Hill: off-axis uniaxial strength equals the closed form at 0-90 degrees (%.1e)", th_worst);
        OrthoStrength badw = st;
        badw.f12 = 1.2;
        CHECK(!strength_valid(&badw, STRENGTH_TSAI_WU, why, sizeof why) && strength_valid(&st, STRENGTH_TSAI_WU, NULL, 0), "non-convex Tsai-Wu interaction refused: %s", why);
    }
}

/* ---------------------------------------------------------------------------------------------- modal and transient */

typedef struct BeamDyn {
    BoxMesh b;
    OrthoConstants k;
    OrthoModel om;
    double *rho;
    unsigned char *fixed;
    StructDyn sd;
} BeamDyn;

/* x in [0, L] along the beam; clamped at x = 0 when clamped */
static void beamdyn_init(BeamDyn *d, int nx, int ny, int nz, double L, double h, double E, double nu, double rho, bool clamped, const double *R) {
    memset(d, 0, sizeof *d);
    d->b = box_hex(nx, ny, nz, L, h, h, rho);
    ortho_isotropic(E, nu, &d->k);
    d->om = (OrthoModel){d->b.nn, d->b.ne, d->b.xyz, d->b.conn, 1, &d->k, NULL, R, 1, HEX8_INCOMPATIBLE};
    d->rho = d->b.rho;
    if (clamped) {
        d->fixed = calloc(3 * (size_t)d->b.nn, 1);
        for (int k = 0; k <= nz; k++)
            for (int j = 0; j <= ny; j++)
                for (int c = 0; c < 3; c++) d->fixed[3 * bnode(&d->b, 0, j, k) + c] = 1;
    }
    d->sd = (StructDyn){&d->om, d->rho, d->fixed};
}

static void beamdyn_free(BeamDyn *d) { free(d->b.xyz), free(d->b.rho), free(d->b.conn), free(d->fixed); }

/* mode with the largest effective mass (d < 3) or inertia (d >= 3) in a direction among elastic modes */
static int dominant_mode(const ModalResult *r, int d, int after) {
    int best = -1;
    for (int i = after; i < r->nmodes; i++)
        if (best < 0 || r->eff[6 * i + d] > r->eff[6 * best + d]) best = i;
    return best;
}

static void test_modal(void) {
    printf("== structural dynamics: consistent mass, cantilever and free-free modes, exact modal integration, Newmark, frequency response\n");
    char err[300];
    /* linear body-force patterns reproduce the d'Alembert load of an arbitrary rigid motion */
    {
        BoxMesh b = box_hex(4, 2, 3, 0.06, 0.02, 0.03, 1150);
        size_t n3 = 3 * (size_t)b.nn;
        double *f1 = calloc(n3, sizeof(double)), *f2 = calloc(n3, sizeof(double)), *pat = calloc(12 * n3, sizeof(double));
        FemMeshView mv = {b.nn, b.ne, b.xyz, b.conn, b.rho};
        const double s0[3] = {0.7, -9.2, 1.3}, w[3] = {3.1, -1.7, 2.2}, al[3] = {-40, 25, 12};
        InertialLoad il;
        loads_inertial(&mv, NULL, 0, s0, w, al, f1, &il);
        for (int u = 0; u < 12; u++) {
            double B[9] = {0}, cc[3] = {0, 0, 0};
            if (u < 3) cc[u] = 1;
            else if (u < 6) {
                int i = u - 3, j = (i + 1) % 3, l = (i + 2) % 3;
                B[3 * j + l] = -1, B[3 * l + j] = 1;
            } else if (u < 9) {
                int i = u - 6;
                B[0] = B[4] = B[8] = -1;
                B[3 * i + i] += 1;
            } else {
                int i = u - 9, j = (i + 1) % 3;
                B[3 * i + j] = B[3 * j + i] = 1;
            }
            loads_body_linear(&mv, B, cc, pat + (size_t)u * n3);
        }
        const double coef[12] = {s0[0], s0[1], s0[2], al[0], al[1], al[2], w[0] * w[0], w[1] * w[1], w[2] * w[2], w[0] * w[1], w[1] * w[2], w[2] * w[0]};
        double dd = 0, ff = 0;
        for (size_t k = 0; k < n3; k++) {
            for (int u = 0; u < 12; u++) f2[k] += coef[u] * pat[(size_t)u * n3 + k];
            dd += (f2[k] - f1[k]) * (f2[k] - f1[k]), ff += f1[k] * f1[k];
        }
        CHECK(sqrt(dd / ff) < 1e-13, "12 linear body-force patterns reproduce the d'Alembert load of a rigid motion (%.1e)", sqrt(dd / ff));
        free(f1), free(f2), free(pat), free(b.xyz), free(b.rho), free(b.conn);
    }
    /* mass matrix of a distorted element */
    {
        double X[8][3], Me[576], tot[3] = {0, 0, 0};
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) X[a][k] = 0.01 * (HEX8_XI[a][k] > 0) + 0.0013 * sin(1.7 * a + k);
        hex8_consistent_mass(X, 1250, Me);
        for (int i = 0; i < 24; i++)
            for (int j = 0; j < 24; j++)
                if (i % 3 == j % 3) tot[i % 3] += Me[24 * i + j];
        double V = hex8_volume(X);
        CHECK(fabs(tot[0] / (1250 * V) - 1) < 1e-12 && fabs(tot[1] / (1250 * V) - 1) < 1e-12 && fabs(tot[2] / (1250 * V) - 1) < 1e-12,
              "consistent mass of a distorted element: rigid translation carries rho V in every direction");
    }
    const double E = 2.0e9, rho = 1200, L = 0.4, h = 0.01;
    const double c_EB = sqrt(E * h * h / (12 * rho)); /* sqrt(EI / (rho A)) */
    double f1_cant = 0, zeta_dummy = 0;
    (void)zeta_dummy;
    /* cantilever: bending pairs, torsion and the first axial mode among the lowest 16 modes */
    {
        BeamDyn d;
        beamdyn_init(&d, 40, 1, 1, L, h, E, 0.0, rho, true, NULL);
        ModalOptions mo = {16, 0, 1e-12, 400};
        ModalResult r;
        bool ok = modal_solve(&d.sd, &mo, &r, err, sizeof err);
        if (!ok) printf("  modal solve failed: %s\n", err);
        double f[16] = {0}, resmax = 0;
        for (int i = 0; ok && i < 16; i++) f[i] = sqrt(r.omega2[i]) / (2 * M_PI), resmax = fmax(resmax, r.residual[i]);
        int ax = ok ? dominant_mode(&r, 0, 0) : -1, by = ok ? dominant_mode(&r, 1, 0) : -1;
        double fax = ax >= 0 ? f[ax] : 0, fax_ref = sqrt(E / rho) / (4 * L);
        double f1_ref = 1.87510407 * 1.87510407 / (2 * M_PI * L * L) * c_EB, f2_ref = 4.69409113 * 4.69409113 / (2 * M_PI * L * L) * c_EB;
        double m = rho * L * h * h;
        f1_cant = f[0];
        /* bending in y and z is a degenerate pair: any rotation of the pair is a valid basis, so effective masses are summed over it */
        int second_y = -1;
        for (int i = 0; ok && i < 16; i++)
            if (f[i] > 1.5 * f[0] && r.eff[6 * i + 1] + r.eff[6 * i + 2] > 0.05 * m && (second_y < 0 || f[i] < f[second_y])) second_y = i;
        double f2y = second_y >= 0 ? f[second_y] : 0, eff1 = 0, eff2 = 0;
        for (int i = 0; ok && i < 16; i++) {
            if (fabs(f[i] / f[0] - 1) < 1e-4) eff1 += r.eff[6 * i + 1] / m;
            if (second_y >= 0 && fabs(f[i] / f2y - 1) < 1e-4) eff2 += r.eff[6 * i + 1] / m;
        }
        (void)by;
        REPORT("40-element cantilever (L/h = 40, nu = 0): bending %.4f / %.4f Hz (Euler-Bernoulli %.4f / %.4f), effective mass fractions %.4f / %.4f (0.6131 / 0.1883); "
               "axial %.2f Hz (%.2f); %d iterations, largest residual %.1e, M-orthogonality %.1e",
               f[0], f2y, f1_ref, f2_ref, eff1, eff2, fax, fax_ref, ok ? r.iterations : 0, resmax, ok ? r.orthogonality : 1.0);
        CHECK(ok && r.converged && r.nrigid == 0 && fabs(f[0] / f1_ref - 1) < 0.005 && fabs(f[1] / f1_ref - 1) < 0.005 && fabs(f2y / f2_ref - 1) < 0.01,
              "cantilever bending frequencies within 0.5%% / 1%% of Euler-Bernoulli (degenerate pair found)");
        CHECK(fabs(eff1 / 0.6131 - 1) < 0.01 && fabs(eff2 / 0.1883 - 1) < 0.02, "effective modal masses of the first two bending modes");
        CHECK(fabs(fax / fax_ref - 1) < 5e-4, "axial mode within 0.05%% of c / 4L");
        CHECK(ok && resmax < 1e-6 && r.orthogonality < 1e-10, "mode residuals (Ritz vectors converge like the square root of the eigenvalue tolerance) and mass orthogonality");
        if (ok) {
            /* exact modal integration: step and ramp loads on the tip, compared with closed forms of the first mode */
            size_t N = (size_t)r.neq;
            double *F = calloc(N, sizeof(double));
            for (int k = 0; k <= 1; k++)
                for (int j = 0; j <= 1; j++) F[r.eq[3 * bnode(&d.b, 40, j, k) + 1]] = 0.25;
            int i0 = 0;
            double w = sqrt(r.omega2[i0]), G = 0;
            for (size_t k = 0; k < N; k++) G += r.phi[(size_t)i0 * N + k] * F[k];
            int ns = 200;
            double dt = (M_PI / w) / 50, *hist = malloc((size_t)(ns + 1) * sizeof(double)), *q = malloc(16 * (size_t)(ns + 1) * sizeof(double)),
                   *qd = malloc(16 * (size_t)(ns + 1) * sizeof(double));
            double zeta[16], worst = 0;
            for (int i = 0; i < 16; i++) zeta[i] = 0.05;
            for (int n = 0; n <= ns; n++) hist[n] = 1;
            ModalLoad ml = {1, F, ns, dt, hist};
            bool mt = modal_transient(&r, &ml, NULL, q, qd, NULL, err, sizeof err);
            worst = fmax(worst, fabs(q[50] / (2 * G / (w * w)) - 1)); /* undamped peak at t = pi / w */
            bool mt2 = modal_transient(&r, &ml, zeta, q, qd, NULL, err, sizeof err);
            double t = 137 * dt, z = 0.05, wd = w * sqrt(1 - z * z);
            double exact = G / (w * w) * (1 - exp(-z * w * t) * (cos(wd * t) + z / sqrt(1 - z * z) * sin(wd * t)));
            double exact_v = G / (w * w) * exp(-z * w * t) * (w * w / wd) * sin(wd * t);
            worst = fmax(worst, fmax(fabs(q[137] / exact - 1), fabs(qd[137] / exact_v - 1)));
            double T = ns * dt;
            for (int n = 0; n <= ns; n++) hist[n] = n * dt / T;
            bool mt3 = modal_transient(&r, &ml, NULL, q, qd, NULL, err, sizeof err);
            t = 173 * dt;
            exact = G / (w * w) * (t / T - sin(w * t) / (w * T));
            worst = fmax(worst, fabs(q[173] / exact - 1));
            CHECK(mt && mt2 && mt3 && worst < 1e-10, "exact modal integration: undamped step peak 2 F / k, damped step and ramp responses match closed forms (%.1e)", worst);
            /* modal superposition against Newmark for the tip displacement under the step load */
            for (int n = 0; n <= ns; n++) hist[n] = 1;
            int nn2 = 400;
            double dt2 = (2 * M_PI / w) / nn2, *h2 = malloc((size_t)(nn2 + 1) * sizeof(double)), *q2 = malloc(16 * (size_t)(nn2 + 1) * sizeof(double)),
                   *qd2 = malloc(16 * (size_t)(nn2 + 1) * sizeof(double));
            double *u = malloc((size_t)(nn2 + 1) * N * sizeof(double)), *v = malloc((size_t)(nn2 + 1) * N * sizeof(double)), *um = malloc(N * sizeof(double));
            for (int n = 0; n <= nn2; n++) h2[n] = 1;
            ModalLoad ml2 = {1, F, nn2, dt2, h2};
            int tipdof = r.eq[3 * bnode(&d.b, 40, 1, 1) + 1];
            bool a1 = modal_transient(&r, &ml2, NULL, q2, qd2, NULL, err, sizeof err), a2 = newmark_transient(&r, d.b.xyz, &ml2, 0, 0, u, v, err, sizeof err);
            double dmax = 0, umax = 0;
            for (int n = 0; a1 && a2 && n <= nn2; n++) {
                modal_expand(&r, q2, nn2, n, um);
                dmax = fmax(dmax, fabs(um[tipdof] - u[(size_t)n * N + (size_t)tipdof]));
                umax = fmax(umax, fabs(u[(size_t)n * N + (size_t)tipdof]));
            }
            /* static tip deflection for the dynamic amplification */
            double *xs = malloc(N * sizeof(double));
            chol_solve(&r.Kf, F, xs);
            double daf = umax / xs[tipdof];
            REPORT("step load on the tip over one period of mode 1: largest difference modal (16 modes) vs Newmark (T1/400) %.2f%% of the peak; dynamic amplification %.3f",
                   100 * dmax / umax, daf);
            CHECK(a1 && a2 && dmax < 0.01 * umax && daf > 1.9 && daf < 2.05, "modal superposition agrees with Newmark within 1%%; an undamped step roughly doubles the static deflection");
            /* frequency response: static limit with residual flexibility, and the first resonance */
            double fr[2] = {1e-6, f[0]}, re[2], im[2], z16[16];
            for (int i = 0; i < 16; i++) z16[i] = 0.02;
            bool fok = modal_frf(&r, F, tipdof, z16, fr, 2, true, re, im, err, sizeof err);
            double res_ref = 0; /* the degenerate pair resonates together */
            for (int i = 0; i < 16; i++) {
                if (fabs(f[i] / f[0] - 1) >= 1e-4) continue;
                double gi = 0;
                for (size_t k = 0; k < N; k++) gi += r.phi[(size_t)i * N + k] * F[k];
                res_ref += r.phi[(size_t)i * N + (size_t)tipdof] * gi / (2 * 0.02 * r.omega2[i]);
            }
            double pk = hypot(re[1], im[1]);
            REPORT("tip FRF: static limit %.6e m/N (static solve %.6e), |H| at f1 %.4e (single-mode estimate %.4e)", re[0], xs[tipdof], pk, fabs(res_ref));
            CHECK(fok && fabs(re[0] / xs[tipdof] - 1) < 1e-8 && fabs(pk / fabs(res_ref) - 1) < 0.05, "FRF: residual flexibility gives the static limit; resonance peak ~ phi phi^T F / (2 zeta w^2)");
            free(xs), free(F), free(hist), free(q), free(qd), free(h2), free(q2), free(qd2), free(u), free(v), free(um);
            modal_result_free(&r);
        }
        beamdyn_free(&d);
    }
    /* free-free beam: six rigid-body modes found, then the first free-free bending pair */
    {
        BeamDyn d;
        beamdyn_init(&d, 40, 1, 1, L, h, E, 0.0, rho, false, NULL);
        ModalOptions mo = {10, 0, 1e-12, 400};
        ModalResult r;
        bool ok = modal_solve(&d.sd, &mo, &r, err, sizeof err);
        if (!ok) printf("  modal solve failed: %s\n", err);
        double fref = 4.73004074 * 4.73004074 / (2 * M_PI * L * L) * c_EB, f6 = ok ? sqrt(r.omega2[6]) / (2 * M_PI) : 0, f7 = ok ? sqrt(r.omega2[7]) / (2 * M_PI) : 0;
        double m = rho * L * h * h, sumx = 0, sumr = 0, Iz = m * (L * L + h * h) / 12;
        for (int i = 0; ok && i < r.nrigid; i++) sumx += r.eff[6 * i + 0], sumr += r.eff[6 * i + 5];
        REPORT("free-free beam: %d rigid-body modes (largest |w^2| %.1e vs first elastic %.1e), bending %.4f / %.4f Hz (Euler-Bernoulli %.4f); rigid modes carry "
               "%.9f of the mass and %.9f of the inertia about z",
               ok ? r.nrigid : -1, ok ? fabs(r.omega2[5]) : 0, ok ? r.omega2[6] : 0, f6, f7, fref, sumx / m, sumr / Iz);
        CHECK(ok && r.nrigid == 6 && fabs(f6 / fref - 1) < 0.005 && fabs(f7 / fref - 1) < 0.005, "free-free: exactly six rigid-body modes, first bending within 0.5%%");
        CHECK(ok && fabs(sumx / m - 1) < 1e-9 && fabs(sumr / Iz - 1) < 1e-6 && fabs(r.mass / m - 1) < 1e-12, "rigid-body modes carry the whole mass and rotational inertia");
        if (ok) modal_result_free(&r);
        beamdyn_free(&d);
    }
    /* printed beam: flat and upright orientation change the bending frequency by sqrt(E_along) */
    {
        double fr[2] = {0};
        for (int o = 0; o < 2; o++) {
            double Ro[9];
            if (o == 0) ortho_print_axes((double[3]){0, 0, 1}, (double[3]){1, 0, 0}, 0, Ro);
            else ortho_print_axes((double[3]){1, 0, 0}, (double[3]){0, 1, 0}, 0, Ro);
            BeamDyn d;
            beamdyn_init(&d, 40, 1, 1, L, h, E, 0.0, rho, true, Ro);
            d.k = ORTHO_TEST;
            ModalOptions mo = {2, 0, 1e-12, 400};
            ModalResult r;
            if (modal_solve(&d.sd, &mo, &r, err, sizeof err)) {
                fr[o] = sqrt(r.omega2[0]) / (2 * M_PI);
                modal_result_free(&r);
            } else
                printf("  modal solve failed: %s\n", err);
            beamdyn_free(&d);
        }
        double ratio = fr[1] / fr[0], ref = sqrt(ORTHO_TEST.E[2] / ORTHO_TEST.E[0]);
        REPORT("printed cantilever first bending: flat %.4f Hz, upright %.4f Hz; ratio %.5f (sqrt(E3/E1) = %.5f)", fr[0], fr[1], ratio, ref);
        CHECK(fabs(ratio / ref - 1) < 0.01, "print orientation scales the bending frequency with the square root of the modulus along the length");
    }
    (void)f1_cant;
}

/* ---------------------------------------------------------------------------------------------- flexible bodies (core) */

/* runs a model for T seconds with RKMK4 steps h; records a probe every step */
typedef double (*ProbeFn)(const MbModel *m, MbSim *sim, const MbState *s, void *ctx);

static bool flex_run_from(MbModelDef *def, double T, double h, ProbeFn probe, void *ctx, double *hist, int nhist, double *energy_drift, bool equilibrium) {
    MechDiag d;
    mdiag_init(&d);
    MbModel *m = mb_compile(def, NULL, &d);
    if (!m) {
        mdiag_print(&d, "  flex");
        mdiag_free(&d);
        return false;
    }
    MbSim *sim = mb_sim_new(m);
    MbState *s = mb_state_new(m);
    bool ok = sim && s && mb_state_initial(m, s, &d) && mb_assemble(sim, s, &d) && (!equilibrium || mb_flex_equilibrium(sim, s, 1e-13, 100, NULL) > 0);
    MbEval *ev = mb_eval_new(m);
    double e0 = NAN, emax = 0;
    int steps = (int)llround(T / h);
    MbStepReport rep;
    for (int n = 0; ok && n <= steps; n++) {
        if (n > 0) ok = mb_step(sim, s, h, NULL, &rep);
        if (!ok) break;
        int k = (int)((long long)n * (nhist - 1) / steps);
        hist[k] = probe(m, sim, s, ctx);
        if (energy_drift && ev && mb_evaluate(sim, s, NULL, ev)) {
            double e = ev->kinetic + ev->potential_gravity + ev->potential_spring;
            if (isnan(e0)) e0 = e;
            emax = fmax(emax, fabs(e - e0));
        }
    }
    if (energy_drift) *energy_drift = emax;
    mb_eval_free(ev), mb_state_free(s), mb_sim_free(sim), mb_model_free(m);
    mdiag_free(&d);
    return ok;
}

static bool flex_run(MbModelDef *def, double T, double h, ProbeFn probe, void *ctx, double *hist, int nhist, double *energy_drift) {
    return flex_run_from(def, T, h, probe, ctx, hist, nhist, energy_drift, false);
}

static double probe_q(const MbModel *m, MbSim *sim, const MbState *s, void *ctx) {
    (void)sim;
    return s->q[*(int *)ctx];
}

typedef struct ComProbe {
    int body, axis;
} ComProbe;

static double probe_body_com(const MbModel *m, MbSim *sim, const MbState *s, void *ctx) { /* one coordinate of the body's centre of mass */
    const ComProbe *pr = ctx;
    MPose T;
    mb_body_pose(sim, s, pr->body, &T);
    double c[3];
    mpose_apply(c, &T, m->bodies[pr->body].com);
    return c[pr->axis];
}

static void test_flexible_core(void) {
    printf("== flexible bodies in the multibody core: exact rigid equivalents of one elastic coordinate (moving mass, translating and rotating interfaces)\n");
    const double g = 9.81;
    /* A: a spinning hub whose body carries a 0.1 kg mass on a spring along body x at 0.2 m; the elastic coordinate is the
     * mass-normalised slider displacement. Reference: the slider as its own body on a prismatic spring joint. */
    {
        double mb = 1.0, ms = 0.1, x0 = 0.2, k = ms * pow(2 * M_PI * 10, 2), w0 = 3.0, Ib[9], Is[9];
        diag3(Ib, 0.01, 0.01, 0.02);
        diag3(Is, 1e-7, 1e-7, 1e-7);
        /* reference */
        MbModelDef *r = mbdef_new();
        mv3_set(r->gravity, 0, 0, -g);
        int hub = mbdef_add_body(r, "hub", mb, (double[3]){0, 0, 0}, Ib);
        int sl = mbdef_add_body(r, "slider", ms, (double[3]){0, 0, 0}, Is);
        int jh = mbdef_add_joint(r, "spin", MB_REVOLUTE, -1, hub);
        r->joints[jh].v0[0] = w0;
        int js = mbdef_add_joint(r, "spring", MB_PRISMATIC, hub, sl);
        mv3_set(r->joints[js].parent_frame.p, x0, 0, 0), mv3_set(r->joints[js].axis, 1, 0, 0);
        r->joints[js].stiffness = k;
        /* flexible: one body, mass and inertia of hub + slider, elastic coordinate eta = sqrt(ms) u */
        MbModelDef *f = mbdef_new();
        mv3_set(f->gravity, 0, 0, -g);
        double I[9], c[3] = {ms * x0 / (mb + ms), 0, 0};
        diag3(I, 0.01 + 1e-7, 0.01 + 1e-7 + ms * x0 * x0, 0.02 + 1e-7 + ms * x0 * x0); /* about the body origin */
        I[4] -= (mb + ms) * c[0] * c[0], I[8] -= (mb + ms) * c[0] * c[0];      /* about the centre of mass */
        int fb = mbdef_add_body(f, "hub", mb + ms, c, I);
        int fj = mbdef_add_joint(f, "spin", MB_REVOLUTE, -1, fb);
        f->joints[fj].v0[0] = w0;
        int fx = mbdef_add_flex(f, fb, 1, 0);
        MbFlexDef *F = &f->flex[fx];
        double phi[3] = {1 / sqrt(ms), 0, 0}, rm[3];
        F->omega2[0] = k / ms;
        mv3_scale(F->ell + 3, phi, ms);             /* integral rho phi */
        mv3_cross(rm, (double[3]){x0, 0, 0}, F->ell + 3); /* integral rho x cross phi */
        mv3_copy(F->ell, rm);
        for (int i = 0; i < 3; i++) /* ms (2 (x0 . phi) I - x0 phi^T - phi x0^T) */
            for (int j = 0; j < 3; j++) F->dJ[3 * i + j] = ms * ((i == j ? 2 * x0 * phi[0] : 0) - (i == 0 ? x0 : 0) * phi[j] - phi[i] * (j == 0 ? x0 : 0));
        int nh = 401;
        double *hr = malloc(nh * sizeof(double)), *hf = malloc(nh * sizeof(double)), *ar = malloc(nh * sizeof(double)), *af = malloc(nh * sizeof(double));
        int iq_r = 1, iq_f = 1, ia = 0; /* slider coordinate (reference q[1]), elastic coordinate (flexible q[1]), hub angle q[0] */
        double drift_r, drift_f;
        bool ok = flex_run(r, 1.0, 2e-4, probe_q, &iq_r, hr, nh, &drift_r) && flex_run(f, 1.0, 2e-4, probe_q, &iq_f, hf, nh, &drift_f) &&
                  flex_run(r, 1.0, 2e-4, probe_q, &ia, ar, nh, NULL) && flex_run(f, 1.0, 2e-4, probe_q, &ia, af, nh, NULL);
        double dmax = 0, umax = 0, amax = 0;
        for (int i = 0; ok && i < nh; i++) {
            double uf = hf[i] / sqrt(ms);
            dmax = fmax(dmax, fabs(uf - hr[i])), umax = fmax(umax, fabs(hr[i])), amax = fmax(amax, fabs(af[i] - ar[i]));
        }
        /* the first-order model omits the centrifugal softening -m w^2 u: relative frequency error m w^2 / (2 k), a phase drift of
         * 2 pi f T times that, on an oscillation of amplitude umax / 2 */
        double expected = 0.5 * umax * (2 * M_PI * 10 * 1.0) * (0.5 * ms * w0 * w0 / k);
        REPORT("moving mass on a spinning hub (m w^2 / k = %.1e): slider displacement flexible vs rigid equivalent %.2e of %.3e m (second-order phase drift "
               "expected %.2e); hub angle %.1e rad; energy drift flexible %.1e J, rigid %.1e J",
               ms * w0 * w0 / k, dmax, umax, expected, amax, drift_f, drift_r);
        CHECK(ok && dmax < 1.5 * expected && amax < 1e-4, "one elastic coordinate reproduces a mass on a spring inside a spinning body up to the omitted second-order term");
        CHECK(ok && drift_f < 1e-9, "the flexible body conserves energy (T = 1/2 V^T I(eta) V + V^T ell eta' + 1/2 eta'^2)");
        free(hr), free(hf), free(ar), free(af);
        mbdef_free(r), mbdef_free(f);
    }
    /* B and C: a child riding on an interface that translates (B) or rotates (C) with the elastic coordinate. Reference: an
     * intermediate body on a prismatic (B) or revolute (C) spring joint at the interface, carrying the child. */
    for (int kind = 0; kind < 2; kind++) {
        double mb = 1.0, x0 = 0.3, mc = 0.2, Ib[9], Ic[9], Is[9];
        diag3(Ib, 0.01, 0.01, 0.02);
        diag3(Ic, 2e-4, 2e-4, 2e-4);
        double ms = 0.05, Js = 4e-5; /* interface segment: mass (B), or a disk of inertia Js about z centred on the interface (C) */
        diag3(Is, kind ? 2e-5 : 1e-7, kind ? Js : 1e-7, kind ? 2e-5 : 1e-7); /* C rotates about y: out of the plane of the spin */
        double k = kind ? Js * pow(2 * M_PI * 8, 2) : ms * pow(2 * M_PI * 8, 2), w0 = 2.0;
        MbModelDef *r = mbdef_new();
        if (kind) mv3_set(r->gravity, 0, 0, -g); /* C: along the spin axis, bending the child out of the plane */
        else mv3_set(r->gravity, 0, -g, 0);      /* B: in the plane of the spin */
        int hub = mbdef_add_body(r, "hub", mb, (double[3]){0, 0, 0}, Ib);
        int seg = mbdef_add_body(r, "segment", ms, (double[3]){0, 0, 0}, Is);
        int ch = mbdef_add_body(r, "child", mc, (double[3]){0.05, 0, 0}, Ic);
        int jh = mbdef_add_joint(r, "spin", MB_REVOLUTE, -1, hub);
        r->joints[jh].v0[0] = w0;
        int js = mbdef_add_joint(r, "elastic", kind ? MB_REVOLUTE : MB_PRISMATIC, hub, seg);
        mv3_set(r->joints[js].parent_frame.p, x0, 0, 0);
        mv3_set(r->joints[js].axis, 0, 1, 0);
        r->joints[js].stiffness = k;
        mbdef_add_joint(r, "mount", MB_FIXED, seg, ch);
        MbModelDef *f = mbdef_new();
        mv3_copy(f->gravity, r->gravity);
        double I[9], com[3] = {ms * x0 / (mb + ms), 0, 0};
        diag3(I, Ib[0] + Is[0], Ib[4] + Is[4] + ms * x0 * x0, Ib[8] + Is[8] + ms * x0 * x0);
        I[4] -= (mb + ms) * com[0] * com[0], I[8] -= (mb + ms) * com[0] * com[0];
        int fb = mbdef_add_body(f, "hub", mb + ms, com, I);
        int fc = mbdef_add_body(f, "child", mc, (double[3]){0.05, 0, 0}, Ic);
        int fj = mbdef_add_joint(f, "spin", MB_REVOLUTE, -1, fb);
        f->joints[fj].v0[0] = w0;
        int fm = mbdef_add_joint(f, "mount", MB_FIXED, fb, fc);
        mv3_set(f->joints[fm].parent_frame.p, x0, 0, 0);
        f->joints[fm].parent_interface = 0;
        int fx = mbdef_add_flex(f, fb, 1, 1);
        MbFlexDef *F = &f->flex[fx];
        mv3_set(F->interfaces[0].point, x0, 0, 0);
        if (kind) { /* rotation about y of a disk centred on the interface: ell_r = Js psi; the disk's inertia tensor rotates:
                     * dJ = [psi]x J - J [psi]x per unit coordinate (zero first-moment change) */
            double psi = 1 / sqrt(Js), P[9] = {0, 0, psi, 0, 0, 0, -psi, 0, 0}; /* [psi e_y]x */
            F->omega2[0] = k / Js;
            F->interfaces[0].psi[1] = psi;
            F->ell[1] = Js * psi;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) F->dJ[3 * i + j] = P[3 * i + 0] * Is[3 * 0 + j] + P[3 * i + 1] * Is[3 * 1 + j] + P[3 * i + 2] * Is[3 * 2 + j] -
                                                                (Is[3 * i + 0] * P[3 * 0 + j] + Is[3 * i + 1] * P[3 * 1 + j] + Is[3 * i + 2] * P[3 * 2 + j]);
        } else { /* translation along y of the segment mass at x0 */
            double phi = 1 / sqrt(ms);
            F->omega2[0] = k / ms;
            F->interfaces[0].phi[1] = phi;
            F->ell[4] = ms * phi;
            F->ell[2] = x0 * ms * phi;                   /* x0 e_x cross ms phi e_y */
            F->dJ[1] = F->dJ[3] = -ms * x0 * phi;        /* ms (2 (x . u) I - x u^T - u x^T), x . u = 0 */
        }
        int nh = 401;
        ComProbe pr_r = {2, kind ? 2 : 1}, pr_f = {1, kind ? 2 : 1};
        double *yr = malloc(nh * sizeof(double)), *yf = malloc(nh * sizeof(double)), drift_f = 0;
        bool ok = flex_run(r, 1.0, 2e-4, probe_body_com, &pr_r, yr, nh, NULL) && flex_run(f, 1.0, 2e-4, probe_body_com, &pr_f, yf, nh, &drift_f);
        double dmax = 0, ymax = 0, ymin = 0;
        for (int i = 0; ok && i < nh; i++) dmax = fmax(dmax, fabs(yf[i] - yr[i])), ymax = fmax(ymax, yr[i]), ymin = fmin(ymin, yr[i]);
        REPORT("child on a %s interface of a spinning hub: child centre of mass %s flexible vs rigid equivalent %.2e m over a %.4f m range; energy drift %.1e J",
               kind ? "rotating (out of plane)" : "translating", kind ? "z" : "y", dmax, ymax - ymin, drift_f);
        CHECK(ok && dmax < 0.02 * (ymax - ymin) && drift_f < 1e-9, "interface %s: the child follows the rigid equivalent to first order, energy conserved",
              kind ? "rotation" : "translation");
        free(yr), free(yf);
        mbdef_free(r), mbdef_free(f);
    }
}

/* ---------------------------------------------------------------------------------------------- flexible bodies (reduction) */

typedef struct FlexBeam {
    BeamDyn d;
    int *root, *tip, nface;
} FlexBeam;

/* beam along x in [0, L]; the root face x = 0 is the clamped region, the x = L face the tip interface */
static void flexbeam_init(FlexBeam *f, int nx, int ny, int nz, double L, double h, double E, double rho) {
    beamdyn_init(&f->d, nx, ny, nz, L, h, E, 0.0, rho, false, NULL);
    f->nface = (ny + 1) * (nz + 1);
    f->root = malloc((size_t)f->nface * sizeof(int)), f->tip = malloc((size_t)f->nface * sizeof(int));
    for (int k = 0, n = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++, n++) f->root[n] = bnode(&f->d.b, 0, j, k), f->tip[n] = bnode(&f->d.b, nx, j, k);
}

static void flexbeam_free(FlexBeam *f) { beamdyn_free(&f->d), free(f->root), free(f->tip); }

static bool flexbeam_reduce(FlexBeam *f, int ninterfaces, int fixed_modes, double zeta, double fmax, MbFlexDef *out, FlexReduceReport *rep, char *err, size_t errlen) {
    const int *nodes[1] = {f->tip};
    int count[1] = {f->nface};
    double L = f->d.b.xyz[3 * bnode(&f->d.b, f->d.b.nx, 0, 0)];
    const double point[1][3] = {{L, 0, 0}};
    FlexReduceInput in = {&f->d.om, f->d.rho, f->root, f->nface, ninterfaces, nodes, count, point, fixed_modes, zeta, {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, fmax, true};
    return flex_reduce(&in, out, rep, err, errlen);
}

/* Gauss integration over the mesh of a displacement field u (3 nn): integral rho u, integral rho x cross u, and the inertia tensor
 * about the origin of the configuration x + s u (the density is per reference volume) */
static void gauss_field(const OrthoModel *om, const double *rho, const double *u, double s, double t[3], double r[3], double J[9]) {
    const double G = 1 / sqrt(3.0);
    memset(t, 0, 3 * sizeof(double)), memset(r, 0, 3 * sizeof(double)), memset(J, 0, 9 * sizeof(double));
    for (int e = 0; e < om->nelems; e++) {
        double X[8][3], U[8][3];
        for (int a = 0; a < 8; a++)
            for (int c = 0; c < 3; c++) X[a][c] = om->xyz[3 * om->conn[8 * e + a] + c], U[a][c] = u[3 * om->conn[8 * e + a] + c];
        for (int g = 0; g < 8; g++) {
            double N[8], dN[8][3], Jm[3][3], dNdx[8][3], x[3] = {0, 0, 0}, uu[3] = {0, 0, 0}, xr[3];
            hex8_shape(HEX8_XI[g][0] * G, HEX8_XI[g][1] * G, HEX8_XI[g][2] * G, N, dN);
            double dm = rho[e] * hex8_jacobian(X, dN, Jm, dNdx);
            for (int a = 0; a < 8; a++)
                for (int c = 0; c < 3; c++) x[c] += N[a] * X[a][c], uu[c] += N[a] * U[a][c];
            mv3_cross(xr, x, uu);
            for (int c = 0; c < 3; c++) t[c] += dm * uu[c], r[c] += dm * xr[c], x[c] += s * uu[c];
            double xx = mv3_dot(x, x);
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) J[3 * i + j] += dm * ((i == j ? xx : 0) - x[i] * x[j]);
        }
    }
}

static double gauss_dot(const OrthoModel *om, const double *rho, const double *u, const double *v) {
    const double G = 1 / sqrt(3.0);
    double sum = 0;
    for (int e = 0; e < om->nelems; e++) {
        double X[8][3];
        for (int a = 0; a < 8; a++)
            for (int c = 0; c < 3; c++) X[a][c] = om->xyz[3 * om->conn[8 * e + a] + c];
        for (int g = 0; g < 8; g++) {
            double N[8], dN[8][3], Jm[3][3], dNdx[8][3], uu[3] = {0, 0, 0}, vv[3] = {0, 0, 0};
            hex8_shape(HEX8_XI[g][0] * G, HEX8_XI[g][1] * G, HEX8_XI[g][2] * G, N, dN);
            double dm = rho[e] * hex8_jacobian(X, dN, Jm, dNdx);
            for (int a = 0; a < 8; a++)
                for (int c = 0; c < 3; c++) uu[c] += N[a] * u[3 * om->conn[8 * e + a] + c], vv[c] += N[a] * v[3 * om->conn[8 * e + a] + c];
            sum += dm * mv3_dot(uu, vv);
        }
    }
    return sum;
}

/* interface compliance of a reduced body: sum over coordinates of u u^T / w^2, u = [phi; psi] of interface i */
static void flex_compliance(const MbFlexDef *F, int i, double C[36]) {
    memset(C, 0, 36 * sizeof(double));
    for (int m = 0; m < F->nmodes; m++) {
        double u[6];
        mv3_copy(u, F->interfaces[i].phi + 3 * m), mv3_copy(u + 3, F->interfaces[i].psi + 3 * m);
        for (int r = 0; r < 6; r++)
            for (int c = 0; c < 6; c++) C[6 * r + c] += u[r] * u[c] / F->omega2[m];
    }
}

/* the reduced body as the only body of a model, held by `joint` at its root; with a tip body of mass mt on the interface */
static MbModelDef *flexbeam_model(const MbFlexDef *F, const FlexReduceReport *rep, MbJointType joint, double mt, bool flexible) {
    MbModelDef *d = mbdef_new();
    mv3_set(d->gravity, 0, 0, -9.81);
    int b = mbdef_add_body(d, "beam", rep->fe_mass, rep->fe_com, rep->fe_inertia);
    int j = mbdef_add_joint(d, "root", joint, -1, b);
    if (joint == MB_REVOLUTE) mv3_set(d->joints[j].axis, 0, 1, 0);
    if (mt > 0) {
        double It[9];
        diag3(It, 1e-10, 1e-10, 1e-10);
        int c = mbdef_add_body(d, "tip", mt, (double[3]){0, 0, 0}, It);
        int jt = mbdef_add_joint(d, "mount", MB_FIXED, b, c);
        mv3_copy(d->joints[jt].parent_frame.p, F->interfaces[0].point);
        if (flexible) d->joints[jt].parent_interface = 0;
    }
    if (flexible) {
        int k = mbdef_add_flex(d, b, F->nmodes, F->ninterfaces);
        MbFlexDef *G = &d->flex[k];
        memcpy(G->omega2, F->omega2, (size_t)F->nmodes * sizeof(double)), memcpy(G->zeta, F->zeta, (size_t)F->nmodes * sizeof(double));
        memcpy(G->ell, F->ell, 6 * (size_t)F->nmodes * sizeof(double)), memcpy(G->dJ, F->dJ, 9 * (size_t)F->nmodes * sizeof(double));
        for (int i = 0; i < F->ninterfaces; i++) {
            mv3_copy(G->interfaces[i].point, F->interfaces[i].point);
            memcpy(G->interfaces[i].phi, F->interfaces[i].phi, 3 * (size_t)F->nmodes * sizeof(double));
            memcpy(G->interfaces[i].psi, F->interfaces[i].psi, 3 * (size_t)F->nmodes * sizeof(double));
        }
    }
    return d;
}

static void test_flexible_reduction(void) {
    printf("== flexible bodies from meshed parts: Craig-Bampton reduction (invariants, interface compliance, frequencies), static sag, tip-mass frequency, flexible pendulum\n");
    char err[300];
    const double E = 2.0e9, rho = 1200, L = 0.4, h = 0.01, I = h * h * h * h / 12, A = h * h, m_beam = rho * L * h * h;
    const double c_EB = sqrt(E * I / (rho * A));
    /* no interfaces: the coordinates are the clamped modes; invariants against independent Gauss integration */
    {
        FlexBeam fb;
        flexbeam_init(&fb, 40, 1, 1, L, h, E, rho);
        MbFlexDef F;
        FlexReduceReport rep;
        bool ok = flexbeam_reduce(&fb, 0, 8, 0.0, 0, &F, &rep, err, sizeof err);
        if (!ok) printf("  reduction failed: %s\n", err);
        BeamDyn cl;
        beamdyn_init(&cl, 40, 1, 1, L, h, E, 0.0, rho, true, NULL);
        ModalOptions mo = {8, 0, 1e-12, 400};
        ModalResult r;
        bool mok = modal_solve(&cl.sd, &mo, &r, err, sizeof err);
        double fdiff = 0, ell_err = 0, ellr_err = 0, dj_err = 0, orth = 0, scale_t = 0, scale_r = 0, scale_j = 0;
        size_t N3 = 3 * (size_t)fb.d.b.nn;
        for (int m = 0; ok && mok && m < 8; m++) fdiff = fmax(fdiff, fabs(F.omega2[m] / r.omega2[m] - 1));
        for (int m = 0; ok && m < 8; m++) {
            const double *u = rep.shapes + (size_t)m * N3;
            double t[3], rr[3], Jp[9], Jm[9], t2[3], r2[3], s = 1e-3 / sqrt(m_beam);
            gauss_field(&fb.d.om, fb.d.rho, u, s, t, rr, Jp);
            gauss_field(&fb.d.om, fb.d.rho, u, -s, t2, r2, Jm);
            for (int c = 0; c < 3; c++) {
                ell_err = fmax(ell_err, fabs(F.ell[6 * m + 3 + c] - t[c])), scale_t = fmax(scale_t, fabs(t[c]));
                ellr_err = fmax(ellr_err, fabs(F.ell[6 * m + c] - rr[c])), scale_r = fmax(scale_r, fabs(rr[c]));
            }
            for (int k = 0; k < 9; k++) {
                double dj = (Jp[k] - Jm[k]) / (2 * s);
                dj_err = fmax(dj_err, fabs(F.dJ[9 * m + k] - dj)), scale_j = fmax(scale_j, fabs(dj));
            }
            for (int n = 0; n <= m; n++) orth = fmax(orth, fabs(gauss_dot(&fb.d.om, fb.d.rho, u, rep.shapes + (size_t)n * N3) - (m == n)));
        }
        double f1 = ok ? sqrt(F.omega2[0]) / (2 * M_PI) : 0, f1_ref = 1.87510407 * 1.87510407 / (2 * M_PI * L * L) * c_EB;
        REPORT("clamped beam, no interfaces, 8 coordinates: frequencies vs full clamped modal solve max rel. diff %.1e; f1 %.4f Hz (Euler-Bernoulli %.4f); "
               "invariants vs Gauss integration: integral rho phi %.1e (of %.2e), integral rho x cross phi %.1e (of %.2e), dJ %.1e (of %.2e, central difference); "
               "mass-normalisation %.1e; FE mass %.9f kg (%.9f)",
               fdiff, f1, f1_ref, ell_err, scale_t, ellr_err, scale_r, dj_err, scale_j, orth, rep.fe_mass, m_beam);
        CHECK(ok && mok && fdiff < 1e-8 && fabs(f1 / f1_ref - 1) < 0.005, "without interfaces the reduced coordinates are the clamped natural modes");
        CHECK(ok && ell_err < 1e-10 * scale_t && ellr_err < 1e-10 * scale_r && dj_err < 1e-7 * scale_j && orth < 1e-9,
              "modal invariants (integral rho phi, integral rho x cross phi, first-order inertia change) and unit modal mass agree with direct integration");
        CHECK(ok && fabs(rep.fe_mass / m_beam - 1) < 1e-12 && fabs(rep.fe_com[0] - L / 2) < 1e-12 * L, "FE mass properties of the reduced part");
        if (mok) modal_result_free(&r);
        beamdyn_free(&cl);
        if (ok) mbflex_free(&F), free(rep.shapes), free(rep.interface_loss);
        flexbeam_free(&fb);
    }
    /* tip interface: compliance against beam theory; constraint and fixed-interface modes decouple statically; frequencies */
    FlexBeam fb;
    flexbeam_init(&fb, 40, 1, 1, L, h, E, rho);
    MbFlexDef F, F0;
    FlexReduceReport rep, rep0;
    bool ok = flexbeam_reduce(&fb, 1, 6, 0.0, 0, &F, &rep, err, sizeof err);
    if (!ok) printf("  reduction failed: %s\n", err);
    bool ok0 = flexbeam_reduce(&fb, 1, 0, 0.0, 0, &F0, &rep0, err, sizeof err);
    if (ok && ok0) {
        double C[36], C0[36], G = E / 2, kappa = 5.0 / 6, dec = 0;
        flex_compliance(&F, 0, C), flex_compliance(&F0, 0, C0);
        for (int k = 0; k < 36; k++) dec = fmax(dec, fabs(C[k] - C0[k]) / sqrt(fabs(C[7 * (k / 6)] * C[7 * (k % 6)])));
        double dy = L * L * L / (3 * E * I) + L / (kappa * G * A), th = L / (E * I), cr = L * L / (2 * E * I), ax = L / (E * A);
        REPORT("tip interface compliance vs beam theory: force y %.5e m/N (Timoshenko %.5e), moment z %.5e rad/N m (%.5e), coupling %.5e (%.5e), axial %.5e m/N (%.5e); "
               "adding 6 fixed-interface modes changes the compliance by %.1e",
               C[7], dy, C[35], th, C[11], cr, C[0], ax, dec);
        CHECK(fabs(C[7] / dy - 1) < 0.01 && fabs(C[14] / dy - 1) < 0.01 && fabs(C[35] / th - 1) < 1e-3 && fabs(C[11] / cr - 1) < 0.005 && fabs(C[0] / ax - 1) < 1e-3,
              "static interface compliance of the reduced cantilever: bending 1%%, pure moment and axial 0.1%%");
        CHECK(dec < 1e-8, "fixed-interface modes carry no static interface flexibility (constraint and fixed-interface modes are stiffness-orthogonal)");
        BeamDyn cf;
        beamdyn_init(&cf, 40, 1, 1, L, h, E, 0.0, rho, true, NULL);
        ModalOptions mo = {4, 0, 1e-12, 400};
        ModalResult r;
        bool mok = modal_solve(&cf.sd, &mo, &r, err, sizeof err);
        double f1 = sqrt(F.omega2[0]) / (2 * M_PI), f2 = sqrt(F.omega2[1]) / (2 * M_PI), f1_full = mok ? sqrt(r.omega2[0]) / (2 * M_PI) : 0;
        double f1_ref = 1.87510407 * 1.87510407 / (2 * M_PI * L * L) * c_EB;
        REPORT("reduced cantilever (6 constraint + 6 fixed-interface modes = %d coordinates, highest %.0f Hz): f1 %.4f / %.4f Hz, full-order clamped-free %.4f Hz, "
               "Euler-Bernoulli %.4f Hz",
               rep.coordinates, rep.max_frequency_hz, f1, f2, f1_full, f1_ref);
        CHECK(mok && fabs(f1 / f1_full - 1) < 2e-3 && fabs(f2 / f1_full - 1) < 2e-3 && fabs(f1 / f1_ref - 1) < 0.005,
              "reduced first bending pair within 0.2%% of the full-order model (rigid tip face, truncation) and 0.5%% of Euler-Bernoulli");
        if (mok) modal_result_free(&r);
        beamdyn_free(&cf);
        /* frequency cutoff: dropped coordinates are reported with their loss of interface flexibility */
        MbFlexDef Fc;
        FlexReduceReport repc;
        bool okc = flexbeam_reduce(&fb, 1, 6, 0.0, 1000, &Fc, &repc, err, sizeof err);
        REPORT("cutoff 1 kHz: %d coordinates kept, %d dropped, largest loss of interface compliance %.2e", okc ? repc.coordinates : 0, okc ? repc.dropped : 0,
               okc ? repc.compliance_loss : 0);
        CHECK(okc && repc.dropped > 0 && repc.dropped + repc.coordinates == 12 && repc.compliance_loss > 0 && repc.compliance_loss < 1 && rep.compliance_loss == 0,
              "a frequency cutoff drops coordinates and reports the static interface flexibility lost");
        if (okc) mbflex_free(&Fc), free(repc.shapes), free(repc.interface_loss);
    }
    /* multibody: static sag of the clamped reduced beam (damped run to rest) against the full-order static FE solution */
    if (ok) {
        MbFlexDef Fd;
        FlexReduceReport repd;
        bool okd = flexbeam_reduce(&fb, 1, 6, 0.6, 0, &Fd, &repd, err, sizeof err);
        double wmax = 0;
        for (int m = 0; okd && m < Fd.nmodes; m++) wmax = fmax(wmax, sqrt(Fd.omega2[m]));
        double hstep = 1.0 / wmax;
        MbModelDef *md = okd ? flexbeam_model(&Fd, &repd, MB_FIXED, 1e-9, true) : NULL;
        int nh = 11;
        double hist[11];
        ComProbe pr = {1, 2};
        bool run = md && flex_run(md, 0.4, hstep, probe_body_com, &pr, hist, nh, NULL);
        /* full order: root clamped, gravity body force, tip face centroid */
        size_t N3 = 3 * (size_t)fb.d.b.nn;
        unsigned char *fixed = calloc(N3, 1);
        for (int k = 0; k < fb.nface; k++) fixed[3 * fb.root[k]] = fixed[3 * fb.root[k] + 1] = fixed[3 * fb.root[k] + 2] = 1;
        double *f = calloc(N3, sizeof(double)), *rhs = NULL, *x = NULL, Bz[9] = {0};
        FemMeshView mv = {fb.d.b.nn, fb.d.b.ne, fb.d.b.xyz, fb.d.b.conn, fb.d.rho};
        loads_body_linear(&mv, Bz, (double[3]){0, 0, 9.81}, f); /* b = -rho (0 + c) = rho g */
        int *eq = malloc(N3 * sizeof(int)), neq = 0;
        CholFactor Kf;
        SolveStats st;
        bool fok = ortho_assemble_factor(&fb.d.om, fixed, eq, &neq, &Kf, &st, err, sizeof err);
        double tip_full = 0;
        if (fok) {
            rhs = calloc((size_t)neq, sizeof(double)), x = calloc((size_t)neq, sizeof(double));
            for (size_t k = 0; k < N3; k++)
                if (eq[k] >= 0) rhs[eq[k]] = f[k];
            chol_solve(&Kf, rhs, x);
            for (int k = 0; k < fb.nface; k++) tip_full += x[eq[3 * fb.tip[k] + 2]] / fb.nface;
            chol_free(&Kf);
        }
        double w = m_beam * 9.81 / L, beam = -w * L * L * L * L / (8 * E * I);
        REPORT("gravity sag at the tip: multibody with the reduced beam (zeta 0.6, h = 1/w_max = %.2e s) %.6e m, full-order static FE %.6e m (relative difference %.1e), "
               "Euler-Bernoulli %.6e m",
               hstep, run ? hist[nh - 1] : 0, tip_full, run && fok ? fabs(hist[nh - 1] / tip_full - 1) : 0, beam);
        CHECK(run && fok && fabs(hist[nh - 1] / tip_full - 1) < 1e-3 && fabs(hist[nh - 1] - hist[nh - 2]) < 1e-4 * fabs(tip_full),
              "the multibody settles to the full-order static deflection under gravity (reduced basis, rigid tip face)");
        /* the static initial state gives the same deflection at once, and an undamped run from it stays at rest */
        if (md) {
            for (int m = 0; m < md->flex[0].nmodes; m++) md->flex[0].zeta[m] = 0;
            double heq[11];
            bool r2 = flex_run_from(md, 0.1, hstep, probe_body_com, &pr, heq, nh, NULL, true);
            double dev = 0;
            for (int i = 0; r2 && i < nh; i++) dev = fmax(dev, fabs(heq[i] - heq[0]));
            REPORT("static initial state: tip %.9e m (settled damped run %.9e m); largest motion over 0.1 s undamped %.1e m", r2 ? heq[0] : 0, run ? hist[nh - 1] : 0, dev);
            CHECK(r2 && fabs(heq[0] / hist[nh - 1] - 1) < 1e-6 && dev < 1e-9 * fabs(heq[0]),
                  "the flexible body can start in static equilibrium: the settled deflection without transient, and no motion without damping");
        }
        /* stresses from the elastic coordinates: sum eta_k sigma_k against the full-order static stresses and beam theory, for two
         * numbers of fixed-interface modes (distributed loads converge with the modes kept) */
        double dvm_p[2] = {1, 1}, vmax = 0, droot = 0, rootmax = 0;
        static const int PMODES[2] = {6, 24};
        for (int pi = 0; fok && pi < 2; pi++) {
            MbFlexDef Fs;
            FlexReduceReport rs;
            if (!flexbeam_reduce(&fb, 1, PMODES[pi], 0.0, 0, &Fs, &rs, err, sizeof err)) continue;
            MbModelDef *ms = flexbeam_model(&Fs, &rs, MB_FIXED, 1e-9, true);
            MechDiag dg;
            mdiag_init(&dg);
            MbModel *mm = mb_compile(ms, NULL, &dg);
            MbSim *sim = mm ? mb_sim_new(mm) : NULL;
            MbState *stt = mm ? mb_state_new(mm) : NULL;
            bool eqok = sim && stt && mb_state_initial(mm, stt, &dg) && mb_assemble(sim, stt, &dg) && mb_flex_equilibrium(sim, stt, 1e-13, 100, NULL) > 0;
            size_t NE48 = 48 * (size_t)fb.d.b.ne;
            double *ufull = calloc(N3, sizeof(double)), *sfull = malloc(NE48 * sizeof(double)), *srec = calloc(NE48, sizeof(double)),
                   *sk = malloc(NE48 * sizeof(double));
            for (size_t k = 0; k < N3; k++) ufull[k] = eq[k] >= 0 ? x[eq[k]] : 0;
            ortho_gauss_stress(&fb.d.om, ufull, sfull);
            for (int m = 0; eqok && m < Fs.nmodes; m++) {
                ortho_gauss_stress(&fb.d.om, rs.shapes + (size_t)m * N3, sk);
                double eta = stt->q[mm->flex_qadr[0] + m];
                for (size_t k = 0; k < NE48; k++) srec[k] += eta * sk[k];
            }
            double dvm = 0;
            for (int e = 0; eqok && e < fb.d.b.ne; e++)
                for (int g = 0; g < 8; g++) {
                    double vr = von_mises(srec + 48 * e + 6 * g), vf = von_mises(sfull + 48 * e + 6 * g);
                    dvm = fmax(dvm, fabs(vr - vf)), vmax = fmax(vmax, vf);
                }
            dvm_p[pi] = eqok ? dvm : 1;
            /* sigma_xx at the Gauss points of the second element from the root: M(x) z / I with M = w (L - x)^2 / 2 */
            const double Gp = 1 / sqrt(3.0);
            for (int g = 0; eqok && pi == 1 && g < 8; g++) {
                int e = 1;
                double N[8], dN[8][3], xg[3] = {0, 0, 0};
                hex8_shape(HEX8_XI[g][0] * Gp, HEX8_XI[g][1] * Gp, HEX8_XI[g][2] * Gp, N, dN);
                for (int a = 0; a < 8; a++)
                    for (int c = 0; c < 3; c++) xg[c] += N[a] * fb.d.b.xyz[3 * fb.d.b.conn[8 * e + a] + c];
                double ref = w * (L - xg[0]) * (L - xg[0]) / 2 * xg[2] / I; /* sagging under -z gravity: tension at the top */
                droot = fmax(droot, fabs(srec[48 * e + 6 * g] - ref)), rootmax = fmax(rootmax, fabs(ref));
            }
            free(ufull), free(sfull), free(srec), free(sk);
            mb_state_free(stt), mb_sim_free(sim), mb_model_free(mm), mbdef_free(ms);
            mdiag_free(&dg);
            mbflex_free(&Fs), free(rs.shapes), free(rs.interface_loss);
        }
        REPORT("stresses from the elastic coordinates at static equilibrium under gravity: von Mises vs full-order static FE, largest difference %.2e (6 "
               "fixed-interface modes) and %.2e (24) of %.4e Pa; bending stress next to the root vs M z / I %.2e of %.4e Pa",
               dvm_p[0], dvm_p[1], vmax, droot, rootmax);
        CHECK(dvm_p[1] < dvm_p[0] && dvm_p[0] < 0.01 * vmax && dvm_p[1] < 2e-3 * vmax && droot < 0.02 * rootmax,
              "stresses recovered from the coupled coordinates converge to the full-order static stresses as modes are added, and follow beam bending "
              "near the root (2%%)");
        free(fixed), free(f), free(rhs), free(x), free(eq);
        if (md) mbdef_free(md);
        if (okd) mbflex_free(&Fd), free(repd.shapes), free(repd.interface_loss);
    }
    /* multibody: first frequency of the cantilever with a tip mass equal to the beam mass, released under gravity */
    if (ok) {
        double mt = m_beam, wmax = 0;
        for (int m = 0; m < F.nmodes; m++) wmax = fmax(wmax, sqrt(F.omega2[m]));
        double hstep = 0.5 / wmax, T = 1.0;
        int steps = (int)llround(T / hstep), nh = steps + 1;
        hstep = T / steps;
        MbModelDef *md = flexbeam_model(&F, &rep, MB_FIXED, mt, true);
        double *hist = malloc((size_t)nh * sizeof(double));
        ComProbe pr = {1, 2};
        bool run = flex_run(md, T, hstep, probe_body_com, &pr, hist, nh, NULL);
        /* mean crossings of the tip history: the first mode dominates the tip motion */
        double mean = 0, first = -1, last = -1;
        int crossings = 0;
        for (int i = 0; run && i < nh; i++) mean += hist[i] / nh;
        for (int i = 1; run && i < nh; i++)
            if (hist[i - 1] < mean && hist[i] >= mean) {
                double tc = (i - 1 + (mean - hist[i - 1]) / (hist[i] - hist[i - 1])) * hstep;
                if (first < 0) first = tc;
                last = tc, crossings++;
            }
        double f_mb = crossings > 1 ? (crossings - 1) / (last - first) : 0;
        /* Euler-Bernoulli with a tip mass: 1 + cos l cosh l + mu l (cos l sinh l - sin l cosh l) = 0 */
        double mu = mt / m_beam, lo = 0.5, hi = 1.875;
        for (int it = 0; it < 200; it++) {
            double l = 0.5 * (lo + hi), g = 1 + cos(l) * cosh(l) + mu * l * (cos(l) * sinh(l) - sin(l) * cosh(l));
            double glo = 1 + cos(lo) * cosh(lo) + mu * lo * (cos(lo) * sinh(lo) - sin(lo) * cosh(lo));
            if ((g > 0) == (glo > 0)) lo = l;
            else hi = l;
        }
        double lam = 0.5 * (lo + hi), f_ref = lam * lam / (2 * M_PI * L * L) * c_EB;
        REPORT("cantilever with a tip mass (mu = 1) in the multibody: first frequency %.4f Hz from %d oscillations; Euler-Bernoulli with tip mass %.4f Hz (lambda %.6f)",
               f_mb, crossings - 1, f_ref, lam);
        CHECK(run && crossings > 5 && fabs(f_mb / f_ref - 1) < 2e-3, "a body on the interface of the reduced beam lowers the first frequency as beam theory predicts (0.2%%)");
        free(hist);
        mbdef_free(md);
    }
    /* flexible pendulum with a tip mass: energy conservation, and the rigid pendulum approached as the stiffness grows */
    if (ok) {
        double dev[2] = {0}, drift[2] = {0}, span = 0;
        bool run = true;
        for (int s = 0; s < 2 && run; s++) {
            FlexBeam sb;
            flexbeam_init(&sb, 40, 1, 1, L, h, s ? 16 * E : E, rho);
            MbFlexDef Fs;
            FlexReduceReport rs;
            run = flexbeam_reduce(&sb, 1, 6, 0.0, 0, &Fs, &rs, err, sizeof err);
            double wmax = 0;
            for (int m = 0; run && m < Fs.nmodes; m++) wmax = fmax(wmax, sqrt(Fs.omega2[m]));
            double T = 0.3, hstep = 0.25 / wmax;
            int steps = (int)ceil(T / hstep), nh = 301;
            hstep = T / steps;
            MbModelDef *mf = run ? flexbeam_model(&Fs, &rs, MB_REVOLUTE, 0.02, true) : NULL, *mr = run ? flexbeam_model(&Fs, &rs, MB_REVOLUTE, 0.02, false) : NULL;
            double *af = malloc(nh * sizeof(double)), *ar = malloc(nh * sizeof(double));
            int iq = 0;
            run = run && flex_run(mf, T, hstep, probe_q, &iq, af, nh, &drift[s]) && flex_run(mr, T, hstep, probe_q, &iq, ar, nh, NULL);
            for (int i = 0; run && i < nh; i++) dev[s] = fmax(dev[s], fabs(af[i] - ar[i])), span = fmax(span, fabs(ar[i]));
            REPORT("flexible pendulum (E x %d, %d coordinates, h = %.2e s): swing %.4f rad in %.1f s, largest angle difference to the rigid pendulum %.3e rad, energy drift %.2e J "
                   "(m g L = %.3e J)",
                   s ? 16 : 1, run ? Fs.nmodes : 0, hstep, span, T, dev[s], drift[s], (m_beam + 0.02) * 9.81 * L);
            free(af), free(ar);
            if (mf) mbdef_free(mf);
            if (mr) mbdef_free(mr);
            mbflex_free(&Fs), free(rs.shapes), free(rs.interface_loss);
            flexbeam_free(&sb);
        }
        double ratio = dev[1] > 0 ? dev[0] / dev[1] : 0, egy = (m_beam + 0.02) * 9.81 * L;
        REPORT("elastic deviation ratio E / 16E: %.2f (compliance ratio 16)", ratio);
        CHECK(run && drift[0] < 1e-6 * egy && drift[1] < 1e-6 * egy, "the flexible pendulum conserves energy through large rotations");
        CHECK(run && ratio > 12 && ratio < 20, "the elastic deviation from the rigid pendulum scales with the compliance: the rigid model is the stiff limit");
    }
    if (ok) mbflex_free(&F), free(rep.shapes), free(rep.interface_loss);
    if (ok0) mbflex_free(&F0), free(rep0.shapes), free(rep0.interface_loss);
    flexbeam_free(&fb);
}

static JsonValue *mass_props(double m, const double c[3], const double I[9]) {
    JsonValue *mp = json_object();
    json_set_string(mp, "provenance", "computed");
    json_set_number(mp, "mass", m);
    json_set(mp, "com", json_numbers(c, 3));
    JsonValue *in = json_set_object(mp, "inertia");
    json_set_number(in, "ixx", I[0]), json_set_number(in, "iyy", I[4]), json_set_number(in, "izz", I[8]);
    json_set_number(in, "ixy", I[1]), json_set_number(in, "ixz", I[2]), json_set_number(in, "iyz", I[5]);
    return mp;
}

static JsonValue *joint_json(const char *name, const char *type, const char *parent, const char *child, const double pp[3], const double *axis) {
    JsonValue *j = json_object();
    json_set_string(j, "name", name), json_set_string(j, "type", type), json_set_string(j, "parent", parent), json_set_string(j, "child", child);
    JsonValue *pf = json_set_object(j, "parent_frame");
    json_set(pf, "position", json_numbers(pp, 3));
    JsonValue *cf = json_set_object(j, "child_frame");
    json_set(cf, "position", json_vec3(0, 0, 0));
    if (axis) json_set(j, "axis", json_numbers(axis, 3));
    return j;
}

static void test_flexible_assembly(void) {
    printf("== flexible bodies in assemblies and the runtime: exact round trip, interface binding, stale models refused, channels, step limit, rollback\n");
    char err[300];
    const double E = 2.0e9, rho = 1200, L = 0.4, h = 0.01, mt = 0.02;
    FlexBeam fb;
    flexbeam_init(&fb, 40, 1, 1, L, h, E, rho);
    AsmFlexible *X = calloc(1, sizeof *X);
    FlexReduceReport rep;
    bool ok = X && flexbeam_reduce(&fb, 1, 6, 0.0, 0, &X->model, &rep, err, sizeof err);
    flexbeam_free(&fb);
    if (!ok) {
        printf("  reduction failed: %s\n", err);
        free(X);
        g_fail++;
        return;
    }
    free(rep.shapes), free(rep.interface_loss);
    X->interface_joint = calloc(1, sizeof *X->interface_joint);
    snprintf(X->interface_joint[0], MB_NAME, "tip_mount");
    X->fe_mass = rep.fe_mass;
    memcpy(X->fe_com, rep.fe_com, sizeof X->fe_com), memcpy(X->fe_inertia, rep.fe_inertia, sizeof X->fe_inertia);
    X->provenance = json_object();
    json_set_string(X->provenance, "made_by", "mechtest");
    /* document: a beam hinged at the world, flexible, with a tip mass on its tip interface */
    JsonValue *doc = json_object();
    json_set_string(doc, "format", "navier-assembly"), json_set_int(doc, "version", 1), json_set_string(doc, "name", "flexible_pendulum");
    JsonValue *u = json_set_object(doc, "units");
    json_set_string(u, "length", "m"), json_set_string(u, "mass", "kg"), json_set_string(u, "inertia", "kg*m^2");
    json_set(doc, "gravity", json_vec3(0, 0, -9.81));
    JsonValue *bodies = json_set_array(doc, "bodies"), *joints = json_set_array(doc, "joints");
    JsonValue *beam = json_object();
    json_set_string(beam, "name", "beam");
    json_set(beam, "mass_properties", mass_props(X->fe_mass, X->fe_com, X->fe_inertia));
    json_set(beam, "flexible", asm_flexible_to_json(X));
    json_push(bodies, beam);
    JsonValue *tip = json_object();
    double It[9];
    diag3(It, 1e-10, 1e-10, 1e-10);
    json_set_string(tip, "name", "tip");
    json_set(tip, "mass_properties", mass_props(mt, (double[3]){0, 0, 0}, It));
    json_push(bodies, tip);
    json_push(joints, joint_json("root", "revolute", "world", "beam", (double[3]){0, 0, 0}, (double[3]){0, 1, 0}));
    json_push(joints, joint_json("tip_mount", "fixed", "beam", "tip", (double[3]){L, 0, 0}, NULL));
    MechDiag d;
    mdiag_init(&d);
    Assembly *a = asm_from_json(doc, ".", NULL, &d);
    JsonValue *canon = a ? asm_to_json(a) : NULL;
    char *text = canon ? json_dump(canon, 0, NULL, NULL) : NULL;
    JsonValue *doc2 = text ? json_parse(text, strlen(text), NULL, NULL) : NULL;
    Assembly *b = doc2 ? asm_from_json(doc2, ".", NULL, &d) : NULL;
    JsonValue *canon2 = b ? asm_to_json(b) : NULL;
    CHECK(a && b && d.nerrors == 0 && d.nwarnings == 0 && json_equal(canon, canon2), "an assembly with a flexible body loads and its canonical form round-trips exactly");
    if (!a || !b) mdiag_print(&d, "  ");
    MbModelDef *def = b ? asm_to_model(b, &d) : NULL;
    int root = def ? mbdef_joint_index(def, "root") : -1, mount = def ? mbdef_joint_index(def, "tip_mount") : -1;
    CHECK(def && def->nflex == 1 && def->flex[0].nmodes == X->model.nmodes && root >= 0 && mount >= 0 && def->joints[root].parent_interface == -1 &&
              def->joints[mount].parent_interface == 0,
          "the multibody model gets the elastic model; the tip joint rides interface 0 and the root joint the reference frame");
    /* the same model built directly: identical motion */
    FlexReduceReport rr = rep;
    MbModelDef *direct = flexbeam_model(&X->model, &rr, MB_REVOLUTE, mt, true);
    int nh = 101, iq = 0;
    double *h1 = malloc(nh * sizeof(double)), *h2 = malloc(nh * sizeof(double)), wmax = sqrt(X->model.omega2[X->model.nmodes - 1]);
    if (def) mv3_set(def->gravity, 0, 0, -9.81);
    bool run = def && flex_run(def, 0.05, 0.5 / wmax, probe_q, &iq, h1, nh, NULL) && flex_run(direct, 0.05, 0.5 / wmax, probe_q, &iq, h2, nh, NULL);
    double dd = 0;
    for (int i = 0; run && i < nh; i++) dd = fmax(dd, fabs(h1[i] - h2[i]));
    CHECK(run && dd == 0, "the assembly's flexible pendulum moves exactly as the directly built model (largest difference %.1e rad)", dd);
    free(h1), free(h2);
    mbdef_free(direct);
    /* stale or inconsistent models are refused with their codes */
    {
        MechDiag d2;
        mdiag_init(&d2);
        Assembly *c = asm_clone(b);
        c->bodies[0].mass *= 1.01;
        MbModelDef *bad = asm_to_model(c, &d2);
        bool mass_refused = !bad && mdiag_has(&d2, "FLEXIBLE_MASS_PROPERTIES");
        mbdef_free(bad), asm_free(c);
        mdiag_clear(&d2);
        c = asm_clone(b);
        snprintf(c->joints[1].def.name, MB_NAME, "renamed");
        bad = asm_to_model(c, &d2);
        bool joint_refused = !bad && mdiag_has(&d2, "FLEXIBLE_INTERFACE_JOINT");
        mbdef_free(bad), asm_free(c);
        mdiag_clear(&d2);
        c = asm_clone(b);
        c->joints[1].def.parent_frame.p[0] += 1e-3;
        bad = asm_to_model(c, &d2);
        MbModel *mm = bad ? mb_compile(bad, NULL, &d2) : NULL;
        bool frame_refused = bad && !mm && mdiag_has(&d2, "INTERFACE_FRAME_MISMATCH");
        mb_model_free(mm), mbdef_free(bad), asm_free(c);
        CHECK(mass_refused && joint_refused && frame_refused,
              "refused: body mass properties that differ from the reduced mesh, a renamed interface joint, an interface joint moved away from its point");
        mdiag_free(&d2);
    }
    /* runtime: channels, peak outputs, step limit, rollback and contact geometry on the flexible body */
    if (def) {
        MechDiag d3;
        mdiag_init(&d3);
        StudySettings st = {.end_time = 0.2, .max_step = 1e-3, .record_period = 1e-3};
        MechSim *ms = mechsim_new(def, NULL, NULL, 0, NULL, 0, NULL, 0, &st, &d3);
        bool init = ms && mechsim_init(ms, &d3);
        int ce = init ? mechsim_channel_index(ms, "beam.elastic[0]") : -1, cs = init ? mechsim_channel_index(ms, "beam.strain_energy") : -1;
        int cd = init ? mechsim_channel_index(ms, "beam.tip_mount.deflection") : -1, cz = init ? mechsim_channel_index(ms, "beam.tip_mount.dz") : -1;
        CHECK(init && ce >= 0 && cs >= 0 && cd >= 0 && cz >= 0 && mdiag_has(&d3, "FLEXIBLE_STEP_LIMIT"),
              "flexible channels (elastic coordinates, strain energy, interface deflection) and the step-limit notice");
        size_t bs = init ? mechsim_state_size(ms) : 0;
        uint8_t *ck = bs ? malloc(bs) : NULL, *e1 = bs ? malloc(bs) : NULL, *e2 = bs ? malloc(bs) : NULL;
        bool r1 = init && ck && e1 && e2 && mechsim_run_until(ms, 0.1, err, sizeof err) && mechsim_save_state(ms, ck, bs) && mechsim_run_until(ms, 0.2, err, sizeof err) &&
                  mechsim_save_state(ms, e1, bs);
        JsonValue *s1 = r1 ? mechsim_summary_json(ms) : NULL;
        bool r2 = r1 && mechsim_restore_state(ms, ck, bs) && mechsim_run_until(ms, 0.2, err, sizeof err) && mechsim_save_state(ms, e2, bs);
        JsonValue *s2 = r2 ? mechsim_summary_json(ms) : NULL;
        CHECK(r2 && !memcmp(e1, e2, bs) && json_equal(s1, s2), "rollback and replay reproduce the flexible state and its peak outputs bit for bit");
        const JsonValue *fbj = json_at(json_get(s1, "flexible_bodies"), 0), *itj = json_at(json_get(fbj, "interfaces"), 0);
        double peak = json_get_num(itj, "peak_deflection_m", -1), hist_max = 0, us_max = 0, lim = json_get_num(s1, "flexible_step_limit_s", 0);
        const double *col = r2 ? mechsim_column(ms, cd) : NULL, *cu = r2 ? mechsim_column(ms, cs) : NULL;
        for (int i = 0; col && i < mechsim_rows(ms); i++) hist_max = fmax(hist_max, col[i]), us_max = fmax(us_max, cu[i]);
        long long steps = (long long)json_get_num(s1, "steps", 0);
        REPORT("flexible pendulum in the runtime: step limit %.3e s (max_step 1e-3 s), %lld steps to 0.2 s; peak tip deflection %.4e m (recorded history max %.4e m), "
               "peak strain energy %.3e J (history max %.3e J)",
               lim, steps, peak, hist_max, json_get_num(fbj, "peak_strain_energy_j", -1), us_max);
        CHECK(r2 && fabs(lim - 0.5 / wmax) < 1e-15 && steps >= (long long)(0.2 / lim) && peak >= hist_max && peak < 1.2 * hist_max && hist_max > 0,
              "steps limited to 0.5 / w_max; the peak interface deflection is tracked at every step (at least the recorded maximum)");
        json_free(s1), json_free(s2);
        free(ck), free(e1), free(e2);
        mechsim_free(ms);
        /* contact geometry on the flexible body is refused when contact is on */
        mdiag_clear(&d3);
        StudySettings sc = st;
        sc.contact = true;
        ms = mechsim_new(def, NULL, NULL, 0, NULL, 0, NULL, 0, &sc, &d3);
        ContactShape sh;
        memset(&sh, 0, sizeof sh);
        snprintf(sh.name, sizeof sh.name, "beam_pad");
        sh.body = 0, sh.type = SHAPE_SPHERE, sh.radius = 0.01, sh.friction = 0.5;
        mpose_identity(&sh.pose);
        bool refused = ms && mechsim_set_contact_shapes(ms, &sh, 1) && !mechsim_init(ms, &d3) && mdiag_has(&d3, "CONTACT_ON_FLEXIBLE_BODY");
        CHECK(refused, "contact geometry on a flexible body is refused (it would ignore the deformation)");
        mechsim_free(ms);
        mdiag_free(&d3);
    }
    mbdef_free(def);
    asm_free(a), asm_free(b);
    json_free(doc), json_free(doc2), json_free(canon), json_free(canon2);
    free(text);
    asm_flexible_free(X);
    mdiag_free(&d);
}

/* ---------------------------------------------------------------------------------------------- printing (FFF) */

static MatTable const_table(double v) {
    MatTable t;
    memset(&t, 0, sizeof t);
    t.n = 1, t.t[0] = 293.15, t.v[0] = v;
    return t;
}

static FffMaterial fff_const_material(double E, double nu, double alpha) {
    FffMaterial m;
    memset(&m, 0, sizeof m);
    m.rho = const_table(1240), m.cp = const_table(1800), m.k = const_table(0.13), m.E = const_table(E), m.alpha = const_table(alpha);
    m.emissivity = const_table(0.9), m.nu = nu, m.T_relax = 1e9, m.E_floor = 1e6;
    return m;
}

/* Independent scalar recovery from the stored tensor: the invariant precedes spatial averaging. */
static double print_vm_error(const double *stress, const double *vm, int ne, double *cancellation) {
    double err = 0;
    *cancellation = 0;
    for (int e = 0; e < ne; e++) {
        double mean = 0, tensor[6] = {0};
        for (int g = 0; g < 8; g++) {
            const double *s = stress + 48 * (size_t)e + 6 * (size_t)g;
            mean += sqrt(0.5 * ((s[0] - s[1]) * (s[0] - s[1]) + (s[1] - s[2]) * (s[1] - s[2]) +
                              (s[2] - s[0]) * (s[2] - s[0])) + 3 * (s[3] * s[3] + s[4] * s[4] + s[5] * s[5])) / 8;
            for (int k = 0; k < 6; k++) tensor[k] += s[k] / 8;
        }
        err = fmax(err, fabs(vm[e] - mean) / fmax(mean, 1e-30));
        if (mean > 1) *cancellation = fmax(*cancellation, 1 - von_mises(tensor) / mean);
    }
    return err;
}

/* the print's element activation in the time-indexed result file: it round-trips, and files without it still load */
/* ---------------------------------------------------------------- the inherent-strain LPBF build (docs/contracts/lpbf-build.md) */

/* tip curvature of a beam built layer by layer, each layer strained by eps on activation:
 * adding layer k+1 of thickness t on a beam of height k t gives d kappa = 6 eps t (k t) / ((k+1) t)^3 */
static double lpbf_layer_curvature(double eps, double t, int layers) {
    double k = 0;
    for (int i = 1; i < layers; i++) k += 6 * eps * i / (pow(i + 1, 3) * t);
    return k;
}

/* M1, M2: the hanging-node patch test (docs/contracts/adaptive-mesh.md). Two layers of a 4 x 2 block: in the lower one a
 * coarse 2 x 2 element beside four fine ones, in the upper one the reverse, so that coarse edges and faces carry mid-edge
 * and mid-face hanging nodes across both the vertical interface and the layer boundary. A free body under a uniform
 * eigenstrain, held 3-2-1, must displace exactly eps . x at every node and carry no stress. */
static int pgrid(int i, int j, int k) { return i + 5 * (j + 3 * k); }

static void patch_hanging(bool chain, bool fine_only, double *err_u, double *max_sig, int *nhang_out, double *reac, double *eqerr) {
    double xyz[3 * 45];
    for (int k = 0; k <= 2; k++)
        for (int j = 0; j <= 2; j++)
            for (int i = 0; i <= 4; i++) {
                double *x = xyz + 3 * pgrid(i, j, k);
                x[0] = 1e-3 * i, x[1] = 1e-3 * j, x[2] = 1e-3 * k;
            }
    int conn[10 * 8], ne = 0;
#define HEX(i0, j0, k0, s)                                                                                                         \
    do {                                                                                                                           \
        int n_[8] = {pgrid(i0, j0, k0), pgrid(i0 + s, j0, k0), pgrid(i0 + s, j0 + s, k0), pgrid(i0, j0 + s, k0),               \
                     pgrid(i0, j0, k0 + 1), pgrid(i0 + s, j0, k0 + 1), pgrid(i0 + s, j0 + s, k0 + 1), pgrid(i0, j0 + s, k0 + 1)}; \
        memcpy(conn + 8 * ne++, n_, sizeof n_);                                                                                    \
    } while (0)
    HEX(0, 0, 0, 2); /* C: coarse, lower layer */
    for (int j = 0; j < 2; j++)
        for (int i = 2; i < 4; i++) HEX(i, j, 0, 1);
    HEX(2, 0, 1, 2); /* D: coarse, upper layer */
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) HEX(i, j, 1, 1);
#undef HEX
    int hn[16], nm[16], ma[64];
    double wt[64];
    int nh = 0;
#define EDGE(n, a, b) (hn[nh] = (n), nm[nh] = 2, ma[4 * nh] = (a), ma[4 * nh + 1] = (b), wt[4 * nh] = wt[4 * nh + 1] = 0.5, nh++)
#define FACE(n, a, b, c, d)                                                                                                     \
    (hn[nh] = (n), nm[nh] = 4, ma[4 * nh] = (a), ma[4 * nh + 1] = (b), ma[4 * nh + 2] = (c), ma[4 * nh + 3] = (d),            \
     wt[4 * nh] = wt[4 * nh + 1] = wt[4 * nh + 2] = wt[4 * nh + 3] = 0.25, nh++)
    /* owned by C: its top face under the fine upper elements, its x = 2 face beside the fine lower ones */
    EDGE(pgrid(1, 0, 1), pgrid(0, 0, 1), pgrid(2, 0, 1));
    EDGE(pgrid(0, 1, 1), pgrid(0, 0, 1), pgrid(0, 2, 1));
    EDGE(pgrid(2, 1, 1), pgrid(2, 0, 1), pgrid(2, 2, 1));
    EDGE(pgrid(1, 2, 1), pgrid(0, 2, 1), pgrid(2, 2, 1));
    if (chain) /* the mid-face node as the middle of two hanging mid-edge nodes: a chain that must resolve to the same */
        EDGE(pgrid(1, 1, 1), pgrid(1, 0, 1), pgrid(1, 2, 1));
    else
        FACE(pgrid(1, 1, 1), pgrid(0, 0, 1), pgrid(2, 0, 1), pgrid(2, 2, 1), pgrid(0, 2, 1));
    EDGE(pgrid(2, 1, 0), pgrid(2, 0, 0), pgrid(2, 2, 0));
    /* owned by D: its bottom face over the fine lower elements, its x = 2 face beside the fine upper ones */
    EDGE(pgrid(3, 0, 1), pgrid(2, 0, 1), pgrid(4, 0, 1));
    EDGE(pgrid(4, 1, 1), pgrid(4, 0, 1), pgrid(4, 2, 1));
    EDGE(pgrid(3, 2, 1), pgrid(2, 2, 1), pgrid(4, 2, 1));
    FACE(pgrid(3, 1, 1), pgrid(2, 0, 1), pgrid(4, 0, 1), pgrid(4, 2, 1), pgrid(2, 2, 1));
    EDGE(pgrid(2, 1, 2), pgrid(2, 0, 2), pgrid(2, 2, 2));
#undef EDGE
#undef FACE
    const double E = 70e9, nu = 0.33, eps[6] = {-1e-3, -2e-3, -3e-3, 0, 0, 0};
    SolidMaterial mat = {E, nu, 2680};
    HexModel hm = {45, ne, xyz, conn, NULL, 1, &mat, HEX8_INCOMPATIBLE, NULL, nh, hn, nm, ma, wt};
    unsigned char fixed[3 * 45] = {0};
    fixed[3 * pgrid(0, 0, 0)] = fixed[3 * pgrid(0, 0, 0) + 1] = fixed[3 * pgrid(0, 0, 0) + 2] = 1;
    fixed[3 * pgrid(4, 0, 0) + 1] = fixed[3 * pgrid(4, 0, 0) + 2] = 1;
    fixed[3 * pgrid(0, 2, 0) + 2] = 1;
    double eps0[6 * 10] = {0};
    for (int e = 0; e < ne; e++)
        if (!fine_only || e >= 6) memcpy(eps0 + 6 * e, eps, sizeof eps); /* elements 6..9: the fine upper ones */
    SolidLoads L = {fixed, NULL, NULL, eps0, {0, 0, 0}};
    SolidOptions opt = {SOLID_SOLVER_DIRECT, 1e-14, 20000, 0, NULL, NULL, NULL};
    SolidResult res;
    char err[256];
    *err_u = INFINITY, *max_sig = INFINITY, *nhang_out = nh;
    if (!solid_solve(&hm, &L, &opt, &res, err, sizeof err)) {
        printf("  patch solve failed: %s\n", err);
        return;
    }
    double eu = 0, sg = 0;
    unsigned char used[45] = {0}; /* the grid points inside a coarse face belong to no element */
    for (int i = 0; i < 8 * ne; i++) used[conn[i]] = 1;
    for (int n = 0; n < 45; n++)
        if (used[n])
            for (int k = 0; k < 3; k++) eu = fmax(eu, fabs(res.u[3 * n + k] - eps[k] * xyz[3 * n + k]));
    for (int i = 0; i < 48 * ne; i++) sg = fmax(sg, fabs(res.gp_stress[i]));
    *err_u = eu / (3e-3 * 4e-3), *max_sig = sg / (E * 3e-3);
    double r = 0; /* a free body with no external load: every reaction of the 3-2-1 support must vanish */
    for (int i = 0; i < 3 * 45; i++) r = fmax(r, fabs(res.reaction[i]));
    *reac = r / (E * 3e-3 * 1e-6), *eqerr = res.equilibrium_error;
    solid_result_free(&res);
}

static void test_adaptive_mesh(void) {
    printf("== adaptive mesh: hanging-node constraints\n");
    double eu, sg, rc, ee;
    int nh;
    patch_hanging(false, false, &eu, &sg, &nh, &rc, &ee);
    REPORT("M1 patch test, 2:1 interface in x and between layers, %d hanging nodes (mid-edge and mid-face): largest "
           "displacement error %.1e of eps L, largest stress %.1e of E eps", nh, eu, sg);
    CHECK(eu < 1e-12 && sg < 1e-6, "M1: a free body under a uniform eigenstrain on a 2:1 mesh displaces exactly eps . x, stress free");
    double eu2, sg2;
    patch_hanging(true, false, &eu2, &sg2, &nh, &rc, &ee);
    REPORT("M2 the same with the mid-face node written as a chain through two hanging mid-edge nodes: displacement error "
           "%.1e, stress %.1e", eu2, sg2);
    CHECK(eu2 < 1e-12 && sg2 < 1e-6, "M2: a hanging node whose masters hang in turn resolves to the same exact field");
    double eu3, sg3;
    patch_hanging(false, true, &eu3, &sg3, &nh, &rc, &ee);
    REPORT("M1b eigenstrain in the fine upper elements only, so the coarse and fine elements must push on each other through "
           "the constraints: largest support reaction %.1e of E eps dA, equilibrium error %.1e, largest stress %.2f of E eps",
           rc, ee, sg3);
    CHECK(rc < 1e-9 && ee < 1e-10 && sg3 > 0.05,
          "M1b: forces cross a 2:1 interface in balance: a free body keeps zero support reactions while carrying stress");
}

static void test_support_cells(void) {
    printf("== supports: homogenised properties from an explicitly solved unit cell (docs/contracts/supports.md)\n");
    char err[300] = "";
    const double nu = 0.33;
    SupportCellProps pr;
    /* S1: a wall as thick as almost its pitch is not allowed, so solid is the homogeneous type's identity and a lattice
     * of struts is not; the explicit solid cell is a thin wall whose walls fill the cell: build it from walls of pitch
     * p and thickness p (checked separately, the check refuses it), so solve a block whose voxels are all solid */
    SupportSpec solid = {.type = SUPPORT_THIN_WALL, .wall_thickness = 0.999e-3, .spacing = 1e-3, .relative_density = 1};
    bool ok = support_cell_solve(&solid, 0, 0, nu, 8, &pr, err, sizeof err);
    REPORT("S1 a cell whose voxels are all solid (walls of 0.999 on a 1 mm pitch at 8 voxels): solid fraction %.6f, "
           "stiffness %.9f vertical and %.9f lateral, conduction %.9f %.9f %.9f", pr.solid_fraction, pr.stiff_z, pr.stiff_x,
           pr.cond_z, pr.cond_x, pr.cond_y);
    CHECK(ok && fabs(pr.solid_fraction - 1) < 1e-12 && fabs(pr.stiff_z - 1) < 1e-9 && fabs(pr.stiff_x - 1) < 1e-9 &&
              fabs(pr.cond_z - 1) < 1e-9 && fabs(pr.cond_x - 1) < 1e-9 && fabs(pr.cond_y - 1) < 1e-9,
          "S1: an all-solid cell has every fraction 1 (%s)", err);
    /* S2: parallel walls along x, t on pitch s: conduction t/s vertically, stiffness (t/s) E/(1-nu^2) / C33 */
    SupportSpec wall = {.type = SUPPORT_THIN_WALL, .wall_thickness = 0.5e-3, .spacing = 2e-3, .direction = 0, .relative_density = 1};
    ok = support_cell_solve(&wall, 0, 0, nu, 16, &pr, err, sizeof err);
    double ts = 0.25, want = ts * (1 / (1 - nu * nu)) / ((1 - nu) / ((1 + nu) * (1 - 2 * nu)));
    REPORT("S2 walls 0.5 mm on a 2 mm pitch: vertical conduction %.9f (t/s %.2f), vertical stiffness %.5f against "
           "(t/s) E/(1-nu^2)/C33 = %.5f (%+.2f %%), lateral stiffness across the walls %.2e, conduction across %.2e",
           pr.cond_z, ts, pr.stiff_z, want, 100 * (pr.stiff_z / want - 1), pr.stiff_x, pr.cond_y);
    CHECK(ok && fabs(pr.cond_z - ts) < 1e-9, "S2: vertical conduction of walls is their area fraction (%s)", err);
    CHECK(ok && fabs(pr.stiff_z / want - 1) < 0.01, "S2: vertical stiffness of free walls confined along their length within 1 %%");
    CHECK(ok && pr.cond_y < 1e-12, "S2: no conduction across parallel walls that do not touch");
    /* S3: every type, voxels halved: fractions move less than 5 % */
    SupportSpec types[5] = {
        {.type = SUPPORT_BLOCK, .wall_thickness = 0.5e-3, .spacing = 2e-3, .relative_density = 1},
        {.type = SUPPORT_THIN_WALL, .wall_thickness = 0.5e-3, .spacing = 2e-3, .relative_density = 1},
        {.type = SUPPORT_CONE, .base_radius = 0.8e-3, .top_radius = 0.4e-3, .spacing = 2e-3, .height = 4e-3, .relative_density = 1},
        {.type = SUPPORT_TREE, .trunk_radius = 0.6e-3, .branch_radius = 0.3e-3, .spacing = 1.5e-3, .trunk_spacing = 3e-3,
         .branch_height = 2e-3, .relative_density = 1},
        {.type = SUPPORT_LATTICE, .cell_size = 2e-3, .strut_diameter = 0.5e-3, .relative_density = 1},
    };
    for (int t = 0; t < 5; t++) {
        SupportCellProps a, b;
        double d0 = types[t].type == SUPPORT_TREE ? 0.5e-3 : 1e-3, d1 = d0 + 1e-3;
        bool oka = support_cell_solve(&types[t], d0, d1, nu, 0, &a, err, sizeof err); /* the default resolution */
        int n = a.voxels_per_pitch;
        bool okb = oka && support_cell_solve(&types[t], d0, d1, nu, 2 * n, &b, err, sizeof err);
        double ds = okb ? fabs(b.stiff_z / a.stiff_z - 1) : 1, dk = okb ? fabs(b.cond_z / a.cond_z - 1) : 1;
        REPORT("S3 %-9s at the default %d and at %d voxels per pitch: solid %.3f -> %.3f, stiffness %.4f -> %.4f (%.1f %%), conduction "
               "%.4f -> %.4f (%.1f %%), lateral stiffness %.4f, lateral conduction %.4f", support_type_name(types[t].type), n,
               2 * n, a.solid_fraction, b.solid_fraction, a.stiff_z, b.stiff_z, 100 * ds, a.cond_z, b.cond_z, 100 * dk,
               b.stiff_x, b.cond_x);
        CHECK(okb && a.stiff_z > 0 && ds < 0.05 && dk < 0.05, "S3: %s fractions converge (within 5 %% on halving the voxels) (%s)",
              support_type_name(types[t].type), err);
    }
    /* S4: the tooth band conducts c times the band below it (thin walls, teeth resolved by the voxels) */
    SupportSpec tw = wall;
    tw.tooth_height = 1e-3, tw.tooth_pitch = 1e-3, tw.contact_fraction = 0.5;
    SupportCellProps below, band;
    ok = support_cell_solve(&tw, 2e-3, 3e-3, nu, 16, &below, err, sizeof err) &&
         support_cell_solve(&tw, 0, 1e-3, nu, 16, &band, err, sizeof err);
    REPORT("S4 tooth band, contact fraction 0.5: conduction %.9f against %.9f below it (ratio %.9f); stiffness ratio %.4f",
           band.cond_z, below.cond_z, band.cond_z / below.cond_z, band.stiff_z / below.stiff_z);
    CHECK(ok && fabs(band.cond_z / below.cond_z - 0.5) < 1e-9, "S4: the tooth band conducts the contact fraction of the wall (%s)", err);
    SupportSpec bad = wall;
    bad.wall_thickness = 3e-3;
    CHECK(!support_cell_solve(&bad, 0, 0, nu, 0, &pr, err, sizeof err) && strstr(err, "wall_thickness"),
          "a wall thicker than its pitch is refused with the parameter named (%s)", err);
}

static void test_lpbf_build(void) {
    printf("== LPBF: inherent-strain build (eigenstrain, strain-free activation, bimetal, layer-by-layer beam)\n");
    char err[300];
    const double E = 70000e6, nu = 0.33;
    /* V1/V2: one element, free (isostatic support) and fully fixed */
    {
        BoxMesh b = box_hex(1, 1, 1, 0.001, 0.001, 0.001, 2680);
        LpbfMesh M = {b.nn, b.ne, b.xyz, b.conn};
        const double eps[3] = {-1e-3, -2e-3, -3e-3};
        double free_u[3] = {0, 0, 0}, free_s = 0, fixed_s[3] = {0, 0, 0};
        unsigned char *iso = calloc(3 * (size_t)b.nn, 1), *all = malloc(3 * (size_t)b.nn);
        memset(all, 1, 3 * (size_t)b.nn);
        int a = bnode(&b, 0, 0, 0), bb = bnode(&b, 1, 0, 0), cc = bnode(&b, 0, 1, 0);
        iso[3 * a] = iso[3 * a + 1] = iso[3 * a + 2] = 1; /* 3-2-1: free contraction, no rigid-body motion */
        iso[3 * bb + 1] = iso[3 * bb + 2] = 1;
        iso[3 * cc + 2] = 1;
        int one = 0;
        LpbfModel *m = lpbf_new(&M, E, nu, iso, HEX8_INCOMPATIBLE, SOLID_SOLVER_DIRECT, 1e-12, err, sizeof err);
        bool ok = m != NULL;
        if (ok) {
            lpbf_activate(m, &one, 1);
            ok = lpbf_strain(m, &one, 1, eps, err, sizeof err);
        }
        if (ok) {
            int n = bnode(&b, 1, 1, 1); /* the far corner moves by eps . x */
            for (int k = 0; k < 3; k++) free_u[k] = lpbf_u(m)[3 * n + k];
            for (int i = 0; i < 48; i++) free_s = fmax(free_s, fabs(lpbf_stress(m)[i]));
        }
        lpbf_free(m);
        double want[3] = {eps[0] * 0.001, eps[1] * 0.001, eps[2] * 0.001}, du = 0;
        for (int k = 0; k < 3; k++) du = fmax(du, fabs(free_u[k] - want[k]) / fabs(want[k]));
        m = lpbf_new(&M, E, nu, all, HEX8_INCOMPATIBLE, SOLID_SOLVER_DIRECT, 1e-12, err, sizeof err);
        bool ok2 = m != NULL;
        if (ok2) {
            lpbf_activate(m, &one, 1);
            ok2 = lpbf_strain(m, &one, 1, eps, err, sizeof err);
        }
        if (ok2)
            for (int k = 0; k < 3; k++) fixed_s[k] = lpbf_stress(m)[k];
        lpbf_free(m);
        double D[6][6];
        isotropic_D(E, nu, D);
        double want_s[3] = {0, 0, 0}, ds = 0;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) want_s[r] -= D[r][c] * eps[c];
            ds = fmax(ds, fabs(fixed_s[r] - want_s[r]) / fabs(want_s[r]));
        }
        REPORT("V1/V2 one element with eigenstrain (%.0e, %.0e, %.0e): free corner %.6e %.6e %.6e m (eps.x, relative error %.1e), largest stress %.1e Pa; "
               "fully fixed sigma %.6e %.6e %.6e Pa against -C:eps (relative error %.1e)",
               eps[0], eps[1], eps[2], free_u[0], free_u[1], free_u[2], du, free_s, fixed_s[0], fixed_s[1], fixed_s[2], ds);
        CHECK(ok && du < 1e-9 && free_s < 1e-9 * E * 3e-3, "V1: a free element takes the eigenstrain exactly and carries no stress");
        CHECK(ok2 && ds < 1e-12, "V2: a fully constrained element carries exactly -C : eps");
        free(iso), free(all), free(b.xyz), free(b.rho), free(b.conn);
    }
    /* V3: strain-free activation. Activating a layer on a deformed base must change nothing; and because every layer is
     * laid at its programmed height, the top of a stack ends at exactly one layer's contraction, whatever N */
    {
        const int N = 5;
        const double h = 0.001, e = -2e-3;
        BoxMesh b = box_hex(1, 1, N, 0.001, 0.001, N * h, 2680);
        LpbfMesh M = {b.nn, b.ne, b.xyz, b.conn};
        unsigned char *sup = calloc(3 * (size_t)b.nn, 1);
        int a = bnode(&b, 0, 0, 0), bb = bnode(&b, 1, 0, 0), cc = bnode(&b, 0, 1, 0); /* 3-2-1 on the base */
        sup[3 * a] = sup[3 * a + 1] = sup[3 * a + 2] = 1;
        sup[3 * bb + 1] = sup[3 * bb + 2] = 1;
        sup[3 * cc + 2] = 1;
        LpbfModel *m = lpbf_new(&M, E, nu, sup, HEX8_INCOMPATIBLE, SOLID_SOLVER_DIRECT, 1e-12, err, sizeof err);
        bool ok = m != NULL;
        const double eps[3] = {e, e, e}, none[3] = {0, 0, 0};
        double activation_du = 0, activation_ds = 0;
        for (int k = 0; ok && k < N; k++) {
            int elem = k;
            lpbf_activate(m, &elem, 1);
            if (k > 0) { /* activation alone: no motion, no stress */
                double *before_u = malloc(3 * (size_t)b.nn * sizeof(double));
                double *before_s = malloc(48 * (size_t)b.ne * sizeof(double));
                memcpy(before_u, lpbf_u(m), 3 * (size_t)b.nn * sizeof(double));
                memcpy(before_s, lpbf_stress(m), 48 * (size_t)b.ne * sizeof(double));
                ok = lpbf_strain(m, &elem, 1, none, err, sizeof err);
                for (int i = 0; ok && i < 3 * b.nn; i++) activation_du = fmax(activation_du, fabs(lpbf_u(m)[i] - before_u[i]));
                for (int i = 0; ok && i < 48 * b.ne; i++) activation_ds = fmax(activation_ds, fabs(lpbf_stress(m)[i] - before_s[i]));
                free(before_u), free(before_s);
            }
            if (ok) ok = lpbf_strain(m, &elem, 1, eps, err, sizeof err);
        }
        double top = ok ? lpbf_u(m)[3 * bnode(&b, 0, 0, N) + 2] : 0;
        lpbf_free(m);
        double want = e * h; /* every layer is laid at its programmed height: only its own contraction remains on top */
        REPORT("V3 strain-free activation, %d layers laid one by one: activating a layer moved the part by %.1e m and changed the stress by %.1e Pa; "
               "top %.9e m against one layer's contraction eps h = %.9e m (relative error %.1e)",
               N, activation_du, activation_ds, top, want, fabs(top - want) / fabs(want));
        CHECK(ok && activation_du < 1e-15 && activation_ds < 1e-6, "V3a: activating a layer on the deformed part moves nothing and adds no stress");
        CHECK(ok && fabs(top - want) < 1e-9 * fabs(want), "V3b: every layer is laid at its programmed height, so the top ends at one layer's contraction");
        free(sup), free(b.xyz), free(b.rho), free(b.conn);
    }
    /* V4: bilayer strip, eigenstrain in the upper layer only: kappa = 3 d_eps / (2 h) */
    {
        const int nx = 40, nz = 2;
        const double L = 0.04, W = 0.002, H = 0.002, e = -1e-3;
        BoxMesh b = box_hex(nx, 1, nz, L, W, H, 2680);
        for (int n = 0; n < b.nn; n++) b.xyz[3 * n + 2] += H / 2; /* the plate plane is not used here */
        LpbfMesh M = {b.nn, b.ne, b.xyz, b.conn};
        unsigned char *sup = calloc(3 * (size_t)b.nn, 1);
        for (int k = 0; k <= nz; k++)
            for (int j = 0; j <= 1; j++)
                for (int c = 0; c < 3; c++) sup[3 * bnode(&b, 0, j, k) + c] = 1; /* clamped at x = 0 */
        LpbfModel *m = lpbf_new(&M, E, nu, sup, HEX8_INCOMPATIBLE, SOLID_SOLVER_DIRECT, 1e-12, err, sizeof err);
        bool ok = m != NULL;
        int *all = malloc((size_t)b.ne * sizeof(int)), *upper = malloc((size_t)b.ne * sizeof(int));
        int nup = 0;
        for (int e2 = 0; e2 < b.ne; e2++) {
            all[e2] = e2;
            if (e2 >= nx) upper[nup++] = e2; /* the second layer in z */
        }
        if (ok) lpbf_activate(m, all, b.ne);
        const double eps[3] = {e, 0, 0};
        if (ok) ok = lpbf_strain(m, upper, nup, eps, err, sizeof err);
        double tip = 0, mid = 0;
        if (ok) {
            tip = lpbf_u(m)[3 * bnode(&b, nx, 0, nz) + 2];
            mid = lpbf_u(m)[3 * bnode(&b, nx / 2, 0, nz) + 2];
            double *vm = calloc((size_t)b.ne, sizeof(double)), cancellation = 0;
            lpbf_von_mises(m, vm);
            double verr = print_vm_error(lpbf_stress(m), vm, b.ne, &cancellation);
            REPORT("V9 LPBF bending scalar: mean integration-point von Mises relative error %.2e; tensor-before-invariant cancellation %.2f%%",
                   verr, 100 * cancellation);
            CHECK(verr < 1e-12 && cancellation > 0.01, "V9: bending stress is averaged after the invariant, with nonuniform stress exercised");
            free(vm);
        }
        lpbf_free(m);
        /* a clamped strip under a constant curvature: w(x) = kappa x^2 / 2 */
        double kappa = -3 * e / (2 * H), want_tip = kappa * L * L / 2; /* w'' = -3 d_eps / (2 h): a contracting top curls the tip up */
        REPORT("V4 bilayer strip (eigenstrain %.0e in the upper half): tip %.6e m, mid %.6e m; Timoshenko 3 d_eps / (2 h) = %.6f 1/m gives %.6e m "
               "(%.2f %%)",
               e, tip, mid, kappa, want_tip, 100 * (tip - want_tip) / want_tip);
        CHECK(ok && fabs(tip - want_tip) < 0.01 * fabs(want_tip), "V4: a strip strained on one side curves by the bimetal formula");
        free(all), free(upper), free(sup), free(b.xyz), free(b.rho), free(b.conn);
    }
    /* P1, P2 and P4: plasticity in a uniaxial bar. The bar is held in x at both ends and free to contract sideways,
     * so an eigenstrain increment -d drives it exactly uniaxially: the elastic trial stress is E times the eigenstrain
     * driven so far, and what the material carries is what the return mapping leaves. */
    {
        const int nx = 6;
        const double L = 0.006, W = 0.001, H = 0.001;
        const double sy = 230e6;
        for (int hcase = 0; hcase < 2; hcase++) {
            const double Hh = hcase ? 2.0e9 : 0.0; /* perfectly plastic, then linear hardening */
            BoxMesh b = box_hex(nx, 1, 1, L, W, H, 2680);
            LpbfMesh M = {b.nn, b.ne, b.xyz, b.conn};
            unsigned char *ends = calloc(3 * (size_t)b.nn, 1);
            for (int j = 0; j <= 1; j++)
                for (int k = 0; k <= 1; k++) ends[3 * bnode(&b, 0, j, k)] = ends[3 * bnode(&b, nx, j, k)] = 1;
            for (int i = 0; i <= nx; i += nx) {
                ends[3 * bnode(&b, i, 0, 0) + 1] = ends[3 * bnode(&b, i, 0, 0) + 2] = 1;
                ends[3 * bnode(&b, i, 1, 0) + 2] = 1;
                ends[3 * bnode(&b, i, 0, 1) + 1] = 1;
            }
            LpbfModel *m = lpbf_new(&M, E, nu, ends, HEX8_INCOMPATIBLE, SOLID_SOLVER_DIRECT, 1e-12, err, sizeof err);
            bool ok = m != NULL;
            if (ok) lpbf_set_plasticity(m, sy, Hh, 40, 1e-12);
            int *all = malloc((size_t)b.ne * sizeof(int));
            for (int e2 = 0; e2 < b.ne; e2++) all[e2] = e2;
            if (ok) lpbf_activate(m, all, b.ne);
            /* twelve increments: yield is reached at the fourth (E eps_y = sy needs drive 3.29e-3) */
            const double d = 1e-3;
            double worst = 0, worst_drive = 0, got_at_peak = 0, want_at_peak = 0;
            int steps = 12;
            for (int i = 0; ok && i < steps; i++) {
                const double eps[3] = {-d, 0, 0};
                ok = lpbf_strain(m, all, b.ne, eps, err, sizeof err);
                double drive = (i + 1) * d, trial = E * drive;
                double want = trial <= sy ? trial : (sy * E + Hh * trial) / (E + Hh);
                double got = 0;
                for (int g = 0; g < 8; g++) got += lpbf_stress(m)[48 * (nx / 2) + 6 * g] / 8;
                double rel = fabs(got - want) / fabs(want);
                if (rel > worst) worst = rel, worst_drive = drive;
                got_at_peak = got, want_at_peak = want;
            }
            double alpha_peak = lpbf_peak_plastic_strain(m);
            double want_alpha = (E * steps * d - got_at_peak) / E; /* P2: the residual strain of the closed form */
            /* P2: unload by driving the eigenstrain back; the stress must return with slope E and no new yielding */
            double unload_got = 0, unload_want = 0;
            if (ok) {
                const double back[3] = {2 * d, 0, 0};
                ok = lpbf_strain(m, all, b.ne, back, err, sizeof err);
                for (int g = 0; g < 8; g++) unload_got += lpbf_stress(m)[48 * (nx / 2) + 6 * g] / 8;
                unload_want = got_at_peak - E * 2 * d;
            }
            double alpha_after = lpbf_peak_plastic_strain(m);
            lpbf_free(m);
            REPORT("P1/P2/P4 uniaxial bar, %s: worst error %.2e over 12 increments (at drive %.4f), peak %.6e Pa "
                   "against %.6e Pa; unloaded to %.6e Pa against %.6e Pa (slope E), residual plastic strain %.6e "
                   "against %.6e, unchanged by unloading (%.6e)",
                   hcase ? "linear hardening H = 2 GPa" : "elastic-perfectly-plastic", worst, worst_drive,
                   got_at_peak, want_at_peak, unload_got, unload_want, alpha_peak, want_alpha, alpha_after);
            CHECK(ok && worst < 1e-9, hcase ? "P1b: a hardening bar follows sigma_y + H_eff (eps - eps_y) exactly"
                                            : "P1a: a perfectly plastic bar follows E eps then sigma_y exactly");
            CHECK(ok && fabs(alpha_peak - want_alpha) < 1e-9 * fabs(want_alpha),
                  "P2a: the residual plastic strain is eps_max - sigma_max / E");
            CHECK(ok && fabs(unload_got - unload_want) < 1e-9 * fabs(unload_want) &&
                      fabs(alpha_after - alpha_peak) <= 1e-15,
                  "P2b: unloading is elastic, with no further yielding");
            if (!hcase)
                CHECK(ok && fabs(got_at_peak - sy) < 1e-9 * sy,
                      "P4: an eigenstrain driven far past yield leaves the stress capped at exactly sigma_y");
            free(all), free(ends), free(b.xyz), free(b.rho), free(b.conn);
        }
    }
    /* P3: the fully plastic moment. The same restrained bar, but the eigenstrain is applied in a linear gradient
     * through the depth, which is what bending does to a section. The internal moment of the section is integrated
     * from the Gauss-point stresses: at first yield it is the elastic sigma_y b h^2 / 6, and once every fibre has
     * yielded it saturates at the fully plastic sigma_y b h^2 / 4, which is 1.5 times as much. */
    {
        const int nx = 2, nz = 24;
        const double L = 0.004, W = 0.001, Hd = 0.006, sy = 230e6;
        BoxMesh b = box_hex(nx, 1, nz, L, W, Hd, 2680);
        LpbfMesh M = {b.nn, b.ne, b.xyz, b.conn};
        unsigned char *ends = calloc(3 * (size_t)b.nn, 1);
        for (int j = 0; j <= 1; j++)
            for (int k = 0; k <= nz; k++) ends[3 * bnode(&b, 0, j, k)] = ends[3 * bnode(&b, nx, j, k)] = 1;
        ends[3 * bnode(&b, 0, 0, 0) + 1] = ends[3 * bnode(&b, 0, 0, 0) + 2] = 1;
        ends[3 * bnode(&b, 0, 1, 0) + 2] = 1;
        ends[3 * bnode(&b, 0, 0, nz) + 1] = 1;
        LpbfModel *m = lpbf_new(&M, E, nu, ends, HEX8_INCOMPATIBLE, SOLID_SOLVER_DIRECT, 1e-12, err, sizeof err);
        bool ok = m != NULL;
        if (ok) lpbf_set_plasticity(m, sy, 0.0, 40, 1e-12);
        int *all = malloc((size_t)b.ne * sizeof(int)), *row = malloc((size_t)b.ne * sizeof(int));
        for (int e2 = 0; e2 < b.ne; e2++) all[e2] = e2;
        if (ok) lpbf_activate(m, all, b.ne);
        const double dz = Hd / nz, kappa_step = 0.4 * sy / E / (Hd / 2); /* each step adds 40 % of first yield */
        double my = 0, mp = 0;
        for (int step = 0; ok && step < 40; step++) {
            for (int k = 0; ok && k < nz; k++) {
                int n = 0;
                for (int e2 = 0; e2 < b.ne; e2++)
                    if (e2 / (nx * 1) == k) row[n++] = e2;
                double z = (k + 0.5) * dz - Hd / 2;
                const double eps[3] = {-kappa_step * z, 0, 0}; /* a curvature increment as an eigenstrain gradient */
                ok = lpbf_strain(m, row, n, eps, err, sizeof err);
            }
            double moment = 0;
            for (int e2 = 0; e2 < b.ne; e2++) {
                if (e2 % nx != nx / 2) continue; /* one section, away from the ends */
                double z = (e2 / nx + 0.5) * dz - Hd / 2, sxx = 0;
                for (int g = 0; g < 8; g++) sxx += lpbf_stress(m)[48 * e2 + 6 * g] / 8;
                moment += sxx * z * W * dz;
            }
            if (step == 0) my = moment / 0.4; /* the first step is 40 % of first yield and still fully elastic */
            mp = moment;
        }
        lpbf_free(m);
        double want_my = sy * W * Hd * Hd / 6, want_mp = sy * W * Hd * Hd / 4;
        REPORT("P3 fully plastic moment (%d elements through the depth): first yield %.6e N m against %.6e (%.2f %%), "
               "saturated %.6e N m against %.6e (%.2f %%), ratio %.4f against 1.5 (%.2f %%)",
               nz, my, want_my, 100 * (my - want_my) / want_my, mp, want_mp, 100 * (mp - want_mp) / want_mp,
               mp / my, 100 * (mp / my - 1.5) / 1.5);
        CHECK(ok && fabs(mp / my - 1.5) < 0.02 * 1.5,
              "P3: the fully plastic moment of a rectangular section is 1.5 times the first-yield moment");
        free(all), free(row), free(ends), free(b.xyz), free(b.rho), free(b.conn);
    }
    /* V8: the cut. A bar held between two fixed ends and strained everywhere carries sigma = -C : eps; removing the
     * middle element must release it completely: both pieces end stress-free, each contracted by eps times its length */
    {
        const int nx = 9;
        const double L = 0.009, W = 0.001, H = 0.001, e = -1e-3;
        BoxMesh b = box_hex(nx, 1, 1, L, W, H, 2680);
        LpbfMesh M = {b.nn, b.ne, b.xyz, b.conn};
        /* both end faces held in x only, with a 3-2-1 support on each piece: the bar is then exactly uniaxial, and each
         * piece still stands after the middle element is removed */
        unsigned char *ends = calloc(3 * (size_t)b.nn, 1);
        for (int j = 0; j <= 1; j++)
            for (int k = 0; k <= 1; k++) ends[3 * bnode(&b, 0, j, k)] = ends[3 * bnode(&b, nx, j, k)] = 1;
        for (int i = 0; i <= nx; i += nx) {
            ends[3 * bnode(&b, i, 0, 0) + 1] = ends[3 * bnode(&b, i, 0, 0) + 2] = 1;
            ends[3 * bnode(&b, i, 1, 0) + 2] = 1;
            ends[3 * bnode(&b, i, 0, 1) + 1] = 1;
        }
        LpbfModel *m = lpbf_new(&M, E, nu, ends, HEX8_INCOMPATIBLE, SOLID_SOLVER_DIRECT, 1e-12, err, sizeof err);
        bool ok = m != NULL;
        int *all = malloc((size_t)b.ne * sizeof(int));
        for (int e2 = 0; e2 < b.ne; e2++) all[e2] = e2;
        const double eps[3] = {e, 0, 0};
        if (ok) lpbf_activate(m, all, b.ne);
        if (ok) ok = lpbf_strain(m, all, b.ne, eps, err, sizeof err);
        int middle = nx / 2;
        double sxx_before = 0; /* the middle element is uniaxial; take its Gauss-point mean */
        if (ok)
            for (int g = 0; g < 8; g++) sxx_before += lpbf_stress(m)[48 * middle + 6 * g] / 8;
        if (ok) ok = lpbf_remove(m, &middle, 1, err, sizeof err);
        double smax = 0, left_tip = 0, right_tip = 0;
        if (ok) {
            for (int e2 = 0; e2 < b.ne; e2++) {
                if (e2 == middle) continue;
                for (int i = 0; i < 48; i++) smax = fmax(smax, fabs(lpbf_stress(m)[48 * e2 + i]));
            }
            left_tip = lpbf_u(m)[3 * bnode(&b, middle, 0, 0)];      /* the new free face of the left piece */
            right_tip = lpbf_u(m)[3 * bnode(&b, middle + 1, 0, 0)]; /* the new free face of the right piece */
        }
        lpbf_free(m);
        double want_before = -E * e, want_left = e * middle * (L / nx), want_right = -e * (nx - middle - 1) * (L / nx);
        REPORT("V8 cut: bar between two fixed ends, middle sigma_xx %.6e Pa against -E eps %.6e Pa; after removing the middle element the largest stress is "
               "%.1e Pa and the new faces sit at %.6e m and %.6e m (eps L of each piece: %.6e m, %.6e m)",
               sxx_before, want_before, smax, left_tip, right_tip, want_left, want_right);
        CHECK(ok && fabs(sxx_before - want_before) < 1e-9 * fabs(want_before), "V8a: the middle of a bar held at both ends carries -E eps");
        CHECK(ok && smax < 1e-6 * fabs(want_before) && fabs(left_tip - want_left) < 1e-6 * fabs(want_left) &&
                  fabs(right_tip - want_right) < 1e-6 * fabs(want_right),
              "V8b: removing an element releases exactly what it carried: both pieces end stress-free at eps L");
        free(all), free(ends), free(b.xyz), free(b.rho), free(b.conn);
    }
    /* V5/V6: a cantilever built layer by layer, each simulation layer strained in x on activation. V6 halves the element
     * size at the same layer thickness (protocol A3), so the physics is unchanged and only the discretisation moves */
    {
        double tip[2] = {-INFINITY, -INFINITY}, want = 0;
        bool ok = true;
        const double L = 0.04, W = 0.002, H = 0.006, e = -1e-3, t_sim = 0.001;
        const int layers = 6;
        for (int run = 0; run < 2 && ok; run++) {
            const int per = run ? 2 : 1;            /* elements through the thickness of one simulation layer */
            const int nx = run ? 80 : 40, nz = layers * per;
            BoxMesh b = box_hex(nx, 1, nz, L, W, H, 2680);
            LpbfMesh M = {b.nn, b.ne, b.xyz, b.conn};
            unsigned char *sup = calloc(3 * (size_t)b.nn, 1);
            for (int k = 0; k <= nz; k++)
                for (int j = 0; j <= 1; j++)
                    for (int c = 0; c < 3; c++) sup[3 * bnode(&b, 0, j, k) + c] = 1; /* clamped end, as a beam */
            LpbfModel *m = lpbf_new(&M, E, nu, sup, HEX8_INCOMPATIBLE, SOLID_SOLVER_DIRECT, 1e-12, err, sizeof err);
            ok = m != NULL;
            int *layer = malloc((size_t)b.ne * sizeof(int));
            const double eps[3] = {e, 0, 0};
            for (int k = 0; ok && k < layers; k++) {
                int n = 0;
                for (int e2 = 0; e2 < b.ne; e2++)
                    if (e2 / nx / per == k) layer[n++] = e2;
                lpbf_activate(m, layer, n);
                ok = lpbf_strain(m, layer, n, eps, err, sizeof err);
            }
            if (ok) /* the protocol's quantity: the largest u_z over the free-end face */
                for (int k = 0; k <= nz; k++)
                    for (int j = 0; j <= 1; j++) tip[run] = fmax(tip[run], lpbf_u(m)[3 * bnode(&b, nx, j, k) + 2]);
            lpbf_free(m);
            free(layer), free(sup), free(b.xyz), free(b.rho), free(b.conn);
        }
        want = -lpbf_layer_curvature(e, t_sim, layers) * L * L / 2; /* a contracting layer curls the free end up */
        double d0 = 100 * (tip[0] - want) / want, mesh = 100 * (tip[1] - tip[0]) / tip[0];
        REPORT("V5/V6 cantilever built in %d layers of %.1f mm: tip %.6e m at 1.0 mm elements against the summed layer moments %.6e m (%.2f %%); "
               "%.6e m at 0.5 mm elements, %.2f %% from the coarse mesh",
               layers, 1e3 * t_sim, tip[0], want, d0, tip[1], mesh);
        CHECK(ok && fabs(d0) < 3.0, "V5: the layer-by-layer beam follows the closed form for the summed layer moments");
        CHECK(ok && fabs(mesh) < 5.0, "V6: halving the element size at the same layer thickness moves the tip by less than 5 percent");
    }
}

static void test_print_results(void) {
    printf("== printing: the stored times of a print in results.nvt (element birth, format versions)\n");
    char err[300], path[256];
    snprintf(path, sizeof path, "%s/nvprintres_%d.nvt", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    BoxMesh b = box_hex(1, 1, 2, 0.01, 0.01, 0.02, 1240); /* two elements, one above the other */
    const int no = 3;
    ThermalCase *c = calloc(1, sizeof *c);
    c->nnodes = b.nn, c->nelems = b.ne, c->nfaces = 0, c->nmesh_nodes = b.nn, c->noutputs = no;
    c->xyz = malloc(3 * (size_t)b.nn * sizeof(double)), c->conn = malloc(8 * (size_t)b.ne * sizeof(int));
    memcpy(c->xyz, b.xyz, 3 * (size_t)b.nn * sizeof(double)), memcpy(c->conn, b.conn, 8 * (size_t)b.ne * sizeof(int));
    c->elem_mat = calloc((size_t)b.ne, sizeof(int)), c->elem_body = calloc((size_t)b.ne, 1);
    c->fixed = calloc((size_t)b.nn, 1), c->fixed_T = calloc((size_t)b.nn, sizeof(double));
    c->elem_source = calloc((size_t)b.ne, sizeof(double));
    c->times = malloc((size_t)no * sizeof(double)), c->T = malloc((size_t)no * (size_t)b.nn * sizeof(double));
    c->tmin = calloc((size_t)no, sizeof(double)), c->tmax = calloc((size_t)no, sizeof(double));
    c->mech_u = malloc(3 * (size_t)no * (size_t)b.nn * sizeof(double)), c->mech_vm = malloc((size_t)no * (size_t)b.nn * sizeof(double));
    c->mech_peak = calloc((size_t)no, sizeof(double)), c->mech_umax = calloc((size_t)no, sizeof(double));
    c->elem_birth = malloc((size_t)b.ne * sizeof(int));
    c->has_mech = true, c->settings.mechanical = true, c->nbodies = 1;
    snprintf(c->body_name[0], sizeof c->body_name[0], "wall");
    for (int i = 0; i < no; i++) {
        c->times[i] = 10.0 * i, c->tmin[i] = 300 + i, c->tmax[i] = 480 - i;
        c->mech_peak[i] = 1e6 * (i + 1), c->mech_umax[i] = 1e-5 * (i + 1);
        for (int n = 0; n < b.nn; n++) {
            c->T[(size_t)i * (size_t)b.nn + (size_t)n] = 300.0 + i * 7 + n;
            c->mech_vm[(size_t)i * (size_t)b.nn + (size_t)n] = 1e5 * (i + 1) + n;
            for (int k = 0; k < 3; k++) c->mech_u[3 * ((size_t)i * (size_t)b.nn + (size_t)n) + (size_t)k] = 1e-6 * (i + 1) * (k + 1) + 1e-9 * n;
        }
    }
    c->elem_birth[0] = 0, c->elem_birth[1] = 2; /* the lower element exists from the start, the upper one from time 2 */
    c->elem_death = malloc((size_t)b.ne * sizeof(int));
    c->elem_group = malloc((size_t)b.ne);
    c->elem_death[0] = 2, c->elem_death[1] = -1; /* the lower element is cut away at time 2, the upper one never */
    c->elem_group[0] = 1, c->elem_group[1] = 0;  /* support and part */
    bool saved = thermal_results_save(c, path, err, sizeof err);
    ThermalCase *r = saved ? thermal_results_load(path, err, sizeof err) : NULL;
    bool same = r && r->noutputs == no && r->nelems == b.ne && r->nnodes == b.nn && r->elem_birth && r->elem_death && r->elem_group;
    if (same)
        for (int e = 0; e < b.ne; e++)
            same &= r->elem_birth[e] == c->elem_birth[e] && r->elem_death[e] == c->elem_death[e] && r->elem_group[e] == c->elem_group[e];
    if (same)
        for (int i = 0; i < no && same; i++) {
            same &= r->times[i] == c->times[i] && r->mech_peak[i] == c->mech_peak[i];
            for (int n = 0; n < b.nn && same; n++)
                same &= r->T[(size_t)i * (size_t)b.nn + (size_t)n] == c->T[(size_t)i * (size_t)b.nn + (size_t)n] &&
                        r->mech_vm[(size_t)i * (size_t)b.nn + (size_t)n] == c->mech_vm[(size_t)i * (size_t)b.nn + (size_t)n] &&
                        r->mech_u[3 * ((size_t)i * (size_t)b.nn + (size_t)n)] == c->mech_u[3 * ((size_t)i * (size_t)b.nn + (size_t)n)];
        }
    REPORT("results.nvt with element birth, death and group: saved %s, reloaded %s, %d stored times of %d nodes, birth %d and %d, death %d and %d, "
           "group %d and %d, fields identical: %s",
           saved ? "yes" : "no", r ? "yes" : "no", r ? r->noutputs : 0, r ? r->nnodes : 0, r && r->elem_birth ? r->elem_birth[0] : -9,
           r && r->elem_birth ? r->elem_birth[1] : -9, r && r->elem_death ? r->elem_death[0] : -9, r && r->elem_death ? r->elem_death[1] : -9,
           r && r->elem_group ? r->elem_group[0] : 9, r && r->elem_group ? r->elem_group[1] : 9, same ? "yes" : "no");
    CHECK(same, "the stored times, element birth, death and group round-trip through the results file");
    if (r) thermal_case_free(r);
    /* a build without a temperature field: the file must carry none and still load */
    free(c->T), c->T = NULL, free(c->tmin), c->tmin = NULL, free(c->tmax), c->tmax = NULL;
    c->no_temperature = true;
    bool saved_nt = thermal_results_save(c, path, err, sizeof err);
    ThermalCase *rn = saved_nt ? thermal_results_load(path, err, sizeof err) : NULL;
    bool nt_ok = rn && !rn->T && rn->no_temperature && rn->noutputs == no && rn->mech_u && rn->elem_death;
    REPORT("a build without temperatures: saved %s, reloaded %s, no temperature array (%s), displacements kept (%s)", saved_nt ? "yes" : "no",
           rn ? "yes" : "no", rn && !rn->T ? "yes" : "no", rn && rn->mech_u ? "yes" : "no");
    CHECK(nt_ok, "a results file written without a temperature field loads without one, keeping the mechanical fields");
    if (rn) thermal_case_free(rn);
    c->no_temperature = false;
    c->T = calloc((size_t)no * (size_t)b.nn, sizeof(double));
    c->tmin = calloc((size_t)no, sizeof(double)), c->tmax = calloc((size_t)no, sizeof(double));
    /* the same file without the element fields, and an older file that never had them */
    free(c->elem_birth), c->elem_birth = NULL;
    free(c->elem_death), c->elem_death = NULL;
    free(c->elem_group), c->elem_group = NULL;
    bool saved2 = thermal_results_save(c, path, err, sizeof err);
    ThermalCase *r2 = saved2 ? thermal_results_load(path, err, sizeof err) : NULL;
    bool v3_ok = r2 && !r2->elem_birth && !r2->elem_death && !r2->elem_group && r2->T && r2->noutputs == no;
    if (r2) thermal_case_free(r2);
    /* rewrite the version as 2 (the digits have the same width) to prove older files still load */
    bool patched = false;
    FILE *f = fopen(path, "r+b");
    if (f) {
        char head[4096];
        const long base = 16; /* the 8-byte magic and the header length hold zero bytes: read the JSON header itself */
        fseek(f, base, SEEK_SET);
        size_t n = fread(head, 1, sizeof head - 1, f);
        head[n] = 0;
        char *v = strstr(head, "\"version\"");
        if (v) {
            char *digit = v + strlen("\"version\"");
            while (*digit == ':' || *digit == ' ') digit++;
            if (*digit == '4') {
                fseek(f, base + (long)(digit - head), SEEK_SET);
                patched = fputc('2', f) == '2';
            }
        }
        fclose(f);
    }
    ThermalCase *r3 = patched ? thermal_results_load(path, err, sizeof err) : NULL;
    bool v2_ok = r3 && r3->noutputs == no && !r3->elem_birth && !r3->elem_death;
    REPORT("a file written without the element fields loads as version 4 (%s) and as version 2 (%s: %s)", v3_ok ? "yes" : "no",
           patched ? "patched" : "not patched", v2_ok ? "yes" : err);
    CHECK(v3_ok && v2_ok, "a results file without the element fields still loads, in the new format version and in the old one");
    if (r3) thermal_case_free(r3);
    remove(path);
    thermal_case_free(c);
    free(b.xyz), free(b.rho), free(b.conn);
}

static void test_fff_print(void) {
    printf("== printing: layer-by-layer thermo-elastic stress history (incremental consistency, release, bimetal warp, relaxation, full print)\n");
    char err[300];
    const double E = 3.0e9, alpha = 7.0e-5;
    /* a bed-bonded block cooled by 100 K in one increment and in four */
    {
        BoxMesh b = box_hex(8, 4, 3, 0.04, 0.02, 0.006, 1240);
        FffMesh M = {b.nn, b.ne, b.xyz, b.conn};
        unsigned char *bed = calloc((size_t)b.nn, 1);
        for (int j = 0; j <= 4; j++)
            for (int i = 0; i <= 8; i++) bed[bnode(&b, i, j, 0)] = 1;
        FffMaterial mat = fff_const_material(E, 0.3, alpha);
        double *T = malloc((size_t)b.nn * sizeof(double)), smax[2] = {0, 0}, dmax = 0;
        double *s1 = malloc(48 * (size_t)b.ne * sizeof(double));
        bool ok = true;
        for (int run = 0; run < 2 && ok; run++) {
            FffMech *m = fff_mech_new(&M, &mat, bed, err, sizeof err);
            ok = m != NULL;
            for (int e = 0; ok && e < b.ne; e++) fff_mech_activate(m, e, 400.0);
            int steps = run ? 4 : 1;
            for (int s = 1; ok && s <= steps; s++) {
                for (int n = 0; n < b.nn; n++) T[n] = 400.0 - 100.0 * s / steps;
                ok = fff_mech_increment(m, T, err, sizeof err);
            }
            const double *st = ok ? fff_mech_stress(m) : NULL;
            for (int i = 0; st && i < 48 * b.ne; i++) {
                smax[run] = fmax(smax[run], fabs(st[i]));
                if (run == 0) s1[i] = st[i];
                else dmax = fmax(dmax, fabs(st[i] - s1[i]));
            }
            if (ok && run == 1) { /* release the cooled block: a uniform contraction leaves it stress-free */
                double rr[3], rb = fff_mech_bed_reaction(m, rr);
                ok = fff_mech_release(m, T, err, sizeof err);
                double after = 0;
                for (int i = 0; ok && i < 48 * b.ne; i++) after = fmax(after, fabs(fff_mech_stress(m)[i]));
                REPORT("bed-bonded block cooled 100 K: largest stress %.4e Pa (1 increment) and %.4e Pa (4), difference %.1e; after release %.1e Pa, "
                       "support reaction %.1e N (bed reaction resultant %.1e N)",
                       smax[0], smax[1], dmax, after, fff_mech_release_reaction(m), rb);
                CHECK(ok && dmax < 1e-9 * smax[0] && smax[0] > 0.5 * E * alpha * 100, "stress increments add up exactly: one or four cooling increments give the same stresses");
                CHECK(ok && after < 1e-7 * smax[0] && fff_mech_release_reaction(m) < 1e-6 * (1 + rb), "a uniformly cooled part comes off the bed stress-free on a load-free support");
            }
            if (!ok) printf("  fff: %s\n", err);
            fff_mech_free(m);
        }
        free(T), free(s1), free(bed), free(b.xyz), free(b.rho), free(b.conn);
    }
    /* warping: a free first layer cools, a second layer is deposited at nominal size on it and cools: Timoshenko bimetal */
    {
        int nx = 50, ny = 2, nz = 4;
        double L = 0.1, W = 0.004, H = 0.004;
        BoxMesh b = box_hex(nx, ny, nz, L, W, H, 1240);
        FffMesh M = {b.nn, b.ne, b.xyz, b.conn};
        unsigned char *bed = calloc((size_t)b.nn, 1);
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) bed[bnode(&b, i, j, 0)] = 1;
        FffMaterial mat = fff_const_material(E, 0.0, alpha);
        double *T = malloc((size_t)b.nn * sizeof(double));
        FffMech *m = fff_mech_new(&M, &mat, bed, err, sizeof err);
        bool ok = m != NULL;
        for (int n = 0; n < b.nn; n++) T[n] = 400.0;
        for (int e = 0; ok && e < b.ne; e++)
            if (e / (nx * ny) < nz / 2) fff_mech_activate(m, e, 400.0);
        ok = ok && fff_mech_release(m, T, err, sizeof err); /* off the bed before anything cools */
        for (int k = 0; ok && k <= nz / 2; k++)
            for (int j = 0; j <= ny; j++)
                for (int i = 0; i <= nx; i++) T[bnode(&b, i, j, k)] = 300.0;
        ok = ok && fff_mech_increment(m, T, err, sizeof err); /* free contraction of layer 1 */
        double s_free = 0;
        for (int i = 0; ok && i < 48 * b.ne; i++) s_free = fmax(s_free, fabs(fff_mech_stress(m)[i]));
        for (int e = 0; ok && e < b.ne; e++)
            if (e / (nx * ny) >= nz / 2) fff_mech_activate(m, e, 400.0);
        for (int n = 0; n < b.nn; n++) T[n] = 300.0;
        ok = ok && fff_mech_increment(m, T, err, sizeof err); /* layer 2 cools on the contracted layer 1 */
        /* curvature of the centre line: least-squares parabola through the bottom nodes at y = 0 */
        double A[3][3] = {{0}}, rhs[3] = {0};
        for (int i = 0; ok && i <= nx; i++) {
            int n = bnode(&b, i, ny / 2, 0);
            double x = b.xyz[3 * n], w = fff_mech_u(m)[3 * n + 2], ph[3] = {1, x, x * x};
            for (int r = 0; r < 3; r++) {
                rhs[r] += ph[r] * w;
                for (int c = 0; c < 3; c++) A[r][c] += ph[r] * ph[c];
            }
        }
        double c2 = 0;
        if (ok) { /* Cramer's rule for the quadratic coefficient */
            double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
            double d2 = A[0][0] * (A[1][1] * rhs[2] - rhs[1] * A[2][1]) - A[0][1] * (A[1][0] * rhs[2] - rhs[1] * A[2][0]) + rhs[0] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
            c2 = d2 / det;
        }
        double kappa = 2 * c2, ref = 1.5 * alpha * 100.0 / H;
        REPORT("two-layer strip (layer 1 cooled free, layer 2 deposited at nominal size and cooled): curvature %.5f 1/m, Timoshenko bimetal 1.5 alpha dT / h = %.5f "
               "1/m (%+.2f%%); layer 1 after its free contraction %.1e Pa",
               kappa, ref, 100 * (kappa / ref - 1), s_free);
        CHECK(ok && s_free < 1e-6 * E * alpha * 100 && kappa > 0 && fabs(kappa / ref - 1) < 0.02,
              "warping from layer-wise deposition: the strip curls up with the bimetal curvature (2%%)");
        if (ok) {
            double *vm = calloc((size_t)b.ne, sizeof(double)), cancellation = 0;
            fff_mech_von_mises(m, vm);
            double verr = print_vm_error(fff_mech_stress(m), vm, b.ne, &cancellation);
            REPORT("F11 FDM bending scalar: mean integration-point von Mises relative error %.2e; tensor-before-invariant cancellation %.2f%%",
                   verr, 100 * cancellation);
            CHECK(verr < 1e-12 && cancellation > 0.01, "F11: FDM bending stress is averaged after the invariant, with nonuniform stress exercised");
            free(vm);
        }
        if (!ok) printf("  fff: %s\n", err);
        fff_mech_free(m);
        free(T), free(bed), free(b.xyz), free(b.rho), free(b.conn);
    }
    /* relaxation: a stressed bed-bonded block reheated above the relaxation temperature loses its stress and the bed lets go */
    {
        BoxMesh b = box_hex(6, 3, 2, 0.03, 0.015, 0.004, 1240);
        FffMesh M = {b.nn, b.ne, b.xyz, b.conn};
        unsigned char *bed = calloc((size_t)b.nn, 1);
        for (int j = 0; j <= 3; j++)
            for (int i = 0; i <= 6; i++) bed[bnode(&b, i, j, 0)] = 1;
        FffMaterial mat = fff_const_material(E, 0.3, alpha);
        mat.T_relax = 350.0;
        double *T = malloc((size_t)b.nn * sizeof(double));
        FffMech *m = fff_mech_new(&M, &mat, bed, err, sizeof err);
        bool ok = m != NULL;
        for (int e = 0; ok && e < b.ne; e++) fff_mech_activate(m, e, 340.0);
        for (int n = 0; n < b.nn; n++) T[n] = 300.0;
        ok = ok && fff_mech_increment(m, T, err, sizeof err);
        double before = 0, rr[3], rb0 = ok ? fff_mech_bed_reaction(m, rr) : 0;
        for (int i = 0; ok && i < 48 * b.ne; i++) before = fmax(before, fabs(fff_mech_stress(m)[i]));
        for (int n = 0; n < b.nn; n++) T[n] = 380.0;
        ok = ok && fff_mech_increment(m, T, err, sizeof err) && fff_mech_increment(m, T, err, sizeof err);
        double after = 0;
        for (int i = 0; ok && i < 48 * b.ne; i++) after = fmax(after, fabs(fff_mech_stress(m)[i]));
        double rb1 = ok ? fff_mech_bed_reaction(m, rr) : 0;
        REPORT("relaxation above %.0f K: stress %.3e Pa -> %.1e Pa; bed reaction resultant %.1e N -> %.1e N", mat.T_relax, before, after, rb0, rb1);
        CHECK(ok && before > 1e6 && after < 1e-6 * before && rb1 < 1e-6 * (1 + rb0), "material reheated above the relaxation temperature loses its stress and stays in equilibrium");
        if (!ok) printf("  fff: %s\n", err);
        fff_mech_free(m);
        free(T), free(bed), free(b.xyz), free(b.rho), free(b.conn);
    }
    /* a modulus that collapses fivefold over 40 K (a glass transition): heating and cooling a free bar is reversible, and a
     * restrained bar gains the stress integral E alpha dT whatever the number of increments */
    {
        int nx = 20;
        double L = 0.04, Wd = 0.002, T_cold = 300, T_hot = 340;
        BoxMesh b = box_hex(nx, 1, 1, L, Wd, Wd, 1240);
        FffMesh M = {b.nn, b.ne, b.xyz, b.conn};
        FffMaterial mat = fff_const_material(E, 0.0, alpha);
        mat.E.n = 2, mat.E.t[0] = 300.0, mat.E.t[1] = 340.0, mat.E.v[0] = 3.0e9, mat.E.v[1] = 0.6e9;
        unsigned char *free_end = calloc((size_t)b.nn, 1), *both_ends = calloc((size_t)b.nn, 1);
        for (int j = 0; j <= 1; j++)
            for (int k = 0; k <= 1; k++) free_end[bnode(&b, 0, j, k)] = both_ends[bnode(&b, 0, j, k)] = both_ends[bnode(&b, nx, j, k)] = 1;
        double *T = malloc((size_t)b.nn * sizeof(double)), tip_hot = 0, tip_back = 0, s_free = 0, s_restr[2] = {0, 0};
        bool ok = true;
        /* free bar (one end held): 300 K -> 340 K in one increment, back in three */
        FffMech *m = fff_mech_new(&M, &mat, free_end, err, sizeof err);
        ok = m != NULL;
        for (int e = 0; ok && e < b.ne; e++) fff_mech_activate(m, e, T_cold);
        for (int n = 0; ok && n < b.nn; n++) T[n] = T_hot;
        ok = ok && fff_mech_increment(m, T, err, sizeof err);
        if (ok) tip_hot = fff_mech_u(m)[3 * bnode(&b, nx, 0, 0)];
        for (int st = 1; ok && st <= 3; st++) {
            for (int n = 0; n < b.nn; n++) T[n] = T_hot - (T_hot - T_cold) * st / 3;
            ok = fff_mech_increment(m, T, err, sizeof err);
        }
        if (ok) {
            tip_back = fff_mech_u(m)[3 * bnode(&b, nx, 0, 0)];
            for (int i = 0; i < 48 * b.ne; i++) s_free = fmax(s_free, fabs(fff_mech_stress(m)[i]));
        }
        fff_mech_free(m);
        /* restrained bar (both ends held) cooled 340 K -> 300 K in one increment and in five: mid-bar axial stress */
        for (int run = 0; ok && run < 2; run++) {
            m = fff_mech_new(&M, &mat, both_ends, err, sizeof err);
            ok = m != NULL;
            for (int e = 0; ok && e < b.ne; e++) fff_mech_activate(m, e, T_hot);
            int steps = run ? 5 : 1;
            for (int st = 1; ok && st <= steps; st++) {
                for (int n = 0; n < b.nn; n++) T[n] = T_hot - (T_hot - T_cold) * st / steps;
                ok = fff_mech_increment(m, T, err, sizeof err);
            }
            if (ok) {
                const double *sg = fff_mech_stress(m) + 48 * (size_t)(nx / 2);
                for (int g = 0; g < 8; g++) s_restr[run] += sg[6 * g] / 8;
            }
            fff_mech_free(m);
        }
        const double exact_tip = alpha * (T_hot - T_cold) * L, exact_s = alpha * 0.5 * (3.0e9 + 0.6e9) * (T_hot - T_cold);
        REPORT("glass-transition bar (E 3.0 -> 0.6 GPa over 40 K): free tip %.4e m heated (alpha dT L = %.4e), %.1e m after cooling back, largest "
               "stress %.1e Pa; restrained, cooled in 1 and 5 increments: %.5e and %.5e Pa (integral E alpha dT = %.5e)",
               tip_hot, exact_tip, tip_back, s_free, s_restr[0], s_restr[1], exact_s);
        CHECK(ok && fabs(tip_hot - exact_tip) < 1e-3 * exact_tip && fabs(tip_back) < 1e-6 * exact_tip && s_free < 1e-6 * exact_s,
              "a free bar heated through a modulus collapse takes exactly its thermal strain and returns stress-free to its length on cooling");
        CHECK(ok && fabs(s_restr[0] - exact_s) < 5e-3 * exact_s && fabs(s_restr[1] - exact_s) < 5e-3 * exact_s,
              "a restrained bar cooled through a modulus collapse gains the stress integral E alpha dT in one increment or five");
        if (!ok) printf("  fff: %s\n", err);
        free(T), free(free_end), free(both_ends), free(b.xyz), free(b.rho), free(b.conn);
    }
    /* deposition plan: a bridge over two pillars, a block hanging under the span and a floating block */
    {
        BoxMesh b = box_hex(6, 1, 4, 0.006, 0.001, 0.004, 1240);
        const int keep[][2] = {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {5, 0}, {5, 1}, {5, 2}, {5, 3}, {1, 3}, {2, 3}, {3, 3}, {4, 3}, {2, 2}, {3, 1}};
        int nk = (int)(sizeof keep / sizeof keep[0]), *conn = malloc(8 * (size_t)nk * sizeof(int));
        for (int q = 0; q < nk; q++) memcpy(conn + 8 * q, b.conn + 8 * (keep[q][0] + 6 * keep[q][1]), 8 * sizeof(int));
        FffMesh M = {b.nn, nk, b.xyz, conn};
        FffPlanStats st;
        int *dep = fff_plan(&M, 0.001, &st);
        bool ok = dep && st.nlayers == 4 && st.unprintable == 1 && st.late == 1 && dep[13] == -1 && dep[12] == 3;
        for (int q = 0; ok && q < 4; q++) ok = dep[q] == q && dep[4 + q] == q && dep[8 + q] == 3;
        REPORT("deposition plan: %d layers, %d elements never deposited, %d deposited late; span %d %d %d %d, hanging block %d, floating block %d", st.nlayers,
               st.unprintable, st.late, dep ? dep[8] : -9, dep ? dep[9] : -9, dep ? dep[10] : -9, dep ? dep[11] : -9, dep ? dep[12] : -9, dep ? dep[13] : -9);
        CHECK(ok, "deposition plan: a span is laid across its pillars in its own layer, material under it waits for it, a floating block is never printed");
        free(dep), free(conn), free(b.xyz), free(b.rho), free(b.conn);
    }
    /* a whole print of a small PLA wall with the library material */
    {
        MaterialRecord rec;
        bool have = material_lookup(NULL, "pla_generic_demo", &rec, err, sizeof err);
        BoxMesh b = box_hex(20, 2, 8, 0.06, 0.006, 0.024, 1240);
        for (int n = 0; n < b.nn; n++) b.xyz[3 * n + 2] += 0.012; /* the bed at z = 0 */
        FffMesh M = {b.nn, b.ne, b.xyz, b.conn};
        FffMaterial mat;
        memset(&mat, 0, sizeof mat);
        if (have) {
            mat.rho = rec.prop[MATP_DENSITY], mat.cp = rec.prop[MATP_CP], mat.k = rec.prop[MATP_K], mat.E = rec.prop[MATP_E];
            mat.alpha = rec.prop[MATP_ALPHA], mat.emissivity = rec.prop[MATP_EMISSIVITY];
            mat.nu = mat_eval(&rec.prop[MATP_NU], 293.15), mat.T_relax = rec.glass_transition_k + 10, mat.E_floor = 1e6;
        }
        FffProcess P = {0.003, 483.15, 333.15, 303.15, 15.0, true, 4.8e-9, 20.0, 1200.0, 3600.0, 5};
        FffSummary S;
        int frames = 0;
        bool ok = have && fff_simulate(&M, &mat, &P, NULL, &S, err, sizeof err);
        (void)frames;
        REPORT("small PLA wall, %d layers: printed in %.2f h, %d thermal steps, %d stress increments; worst thermal energy balance %.1e; peak von Mises "
               "on the bed %.2f MPa, released %.2f MPa; vertical warp %.3f to %.3f mm; release support reaction %.1e N of bed reactions %.1e N; %.1f s",
               S.nlayers, S.print_time / 3600, S.thermal_steps, S.mech_solves, S.worst_energy_balance, S.peak_vm_bed / 1e6, S.peak_vm_released / 1e6,
               1e3 * S.warp_z_min, 1e3 * S.warp_z_max, S.release_support_reaction, S.bed_reaction_total, S.seconds_thermal + S.seconds_mech);
        REPORT("F14 PLA wall heat ledger: supplied %.9g J, corrected %.9g J, stored %.9g J, bed %.9g J, air %.9g J; "
               "deposition identity %.2e, whole-print error %.2e", S.deposition_heat, S.deposition_correction,
               S.stored_heat, S.bed_heat, S.air_heat, S.deposition_balance, S.global_heat_balance);
        CHECK(ok && S.global_heat_balance < 1e-6, "F14: complete wall conserves nozzle enthalpy through convection, radiation and bed cool-down");
        CHECK(ok && S.nlayers == 8 && S.frames == 2 * 8 + 3 && S.worst_energy_balance < 1e-6 && S.release_support_reaction < 1e-6 * (1 + S.bed_reaction_total) &&
                  S.peak_vm_released > 0,
              "a whole print: every step conserves energy, the bed release is self-equilibrated and residual stresses remain");
        if (!ok) printf("  fff: %s\n", err);
        free(b.xyz), free(b.rho), free(b.conn);
    }
}

typedef struct PrintHeatCapture {
    double T[16], low, high;
    const FffMesh *mesh;
    double remaining_warp_min, remaining_warp_max, extrema_error;
} PrintHeatCapture;

static bool capture_print_heat(const FffFrame *frame, void *ctx) {
    PrintHeatCapture *c = ctx;
    memcpy(c->T, frame->T, sizeof c->T);
    for (int n = 0; n < 16; n++) c->low = fmin(c->low, frame->T[n]), c->high = fmax(c->high, frame->T[n]);
    if (!strcmp(frame->stage, "supports removed")) {
        unsigned char shown[16] = {0};
        for (int e = 0; e < c->mesh->nelems; e++)
            if (frame->active[e])
                for (int a = 0; a < 8; a++) shown[c->mesh->conn[8 * e + a]] = 1;
        double maximum_u = 0, maximum_T = 0;
        c->remaining_warp_min = INFINITY, c->remaining_warp_max = -INFINITY;
        for (int n = 0; n < 16; n++)
            if (shown[n]) {
                const double *u = frame->u + 3 * n;
                c->remaining_warp_min = fmin(c->remaining_warp_min, u[2]);
                c->remaining_warp_max = fmax(c->remaining_warp_max, u[2]);
                maximum_u = fmax(maximum_u, sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]));
                maximum_T = fmax(maximum_T, frame->T[n]);
            }
        c->extrema_error = fmax(fabs(frame->T_max - maximum_T) / 1e-10, fabs(frame->u_max - maximum_u) / 1e-12);
    }
    return true;
}

/* Criteria F10, F12 and V8 are recorded in the process contracts before their first run. */
static void test_printing_numerics(void) {
    char err[300] = {0};
    printf("== printing numerical integrity: exact table integration and accumulated plate reactions\n");
    {
        BoxMesh b = box_hex(1, 1, 1, 1, 1, 1, 1);
        FffMesh mesh = {b.nn, b.ne, b.xyz, b.conn};
        unsigned char *held = malloc((size_t)b.nn);
        memset(held, 1, (size_t)b.nn);
        FffMaterial mat = fff_const_material(1e9, 0, 1e-4);
        mat.E_floor = 2e9;
        mat.E.n = 4;
        double knots[4] = {300, 300.005, 300.015, 301}, values[4] = {1e9, 1e9, 3e9, 3e9};
        memcpy(mat.E.t, knots, sizeof knots), memcpy(mat.E.v, values, sizeof values);
        mat.alpha.n = 2, mat.alpha.t[0] = 300, mat.alpha.t[1] = 301;
        mat.alpha.v[0] = 1e-4, mat.alpha.v[1] = 2e-4;
        /* Independent polynomial integral, including the interior floor crossing at 300.010 K. */
        const double cuts[5] = {0, 0.005, 0.010, 0.015, 1};
        const double mods[5] = {2e9, 2e9, 2e9, 3e9, 3e9};
        double want = 0;
        for (int j = 0; j < 4; j++) {
            double width = cuts[j + 1] - cuts[j], a = 1e-4 * (1 + cuts[j]), da = 1e-4 * width;
            double Em = mods[j], dE = mods[j + 1] - Em;
            want += width * (Em * a + 0.5 * (Em * da + a * dE) + dE * da / 3);
        }
        double got[2] = {0, 0}, errors[2] = {INFINITY, INFINITY};
        bool ok = true;
        for (int run = 0; run < 2; run++) {
            FffMech *m = fff_mech_new(&mesh, &mat, held, err, sizeof err);
            if (!m) { ok = false; break; }
            fff_mech_activate(m, 0, 300);
            const double increments[5] = {300.005, 300.010, 300.015, 300.375, 301};
            for (int s = 0; s < (run ? 5 : 1); s++) {
                double T[8];
                for (int n = 0; n < 8; n++) T[n] = run ? increments[s] : 301;
                if (!fff_mech_increment(m, T, err, sizeof err)) { ok = false; break; }
            }
            got[run] = -fff_mech_stress(m)[0];
            errors[run] = fabs(got[run] - want) / want;
            fff_mech_free(m);
        }
        REPORT("F10 0.01 K transition and modulus-floor crossing: restrained stress %.12g Pa, exact %.12g Pa; one/multiple increments errors %.2e / %.2e",
               got[0], want, errors[0], errors[1]);
        CHECK(ok && errors[0] < 1e-10 && errors[1] < 1e-10, "F10: exact thermoelastic path across sharp table knots and the modulus floor: %s", err);
        free(held), free(b.xyz), free(b.rho), free(b.conn);
    }
    {
        BoxMesh b = box_hex(1, 1, 3, 0.001, 0.001, 0.003, 1000);
        FffMesh mesh = {b.nn, b.ne, b.xyz, b.conn};
        FffProcess process = {0.001, 400, 300, 300, 0, false, 1e-5, 0.002, 0, 0, 3};
        for (int scenario = 0; scenario < 3; scenario++) {
            bool variable = scenario == 1, support = scenario == 2;
            int support_band[3] = {0, -1, -1};
            double band_props[6] = {0.5, 0.5, 0.5, 0.25, 0.25, 0};
            mesh.support_band = support ? support_band : NULL;
            mesh.nbands = support ? 1 : 0, mesh.band_props = support ? band_props : NULL;
            FffMaterial mat = fff_const_material(2e9, 0, 1e-4);
            mat.rho = const_table(1000), mat.cp = const_table(1000);
            if (variable) {
                mat.cp.n = 2, mat.cp.t[0] = 300, mat.cp.t[1] = 400;
                mat.cp.v[0] = 1000, mat.cp.v[1] = 2000;
            }
            PrintHeatCapture capture = {{0}, INFINITY, -INFINITY, &mesh, 0, 0, 0};
            FffCallbacks cb = {capture_print_heat, NULL, &capture};
            FffSummary sum;
            bool ok = fff_simulate(&mesh, &mat, &process, &cb, &sum, err, sizeof err);
            double stored = 0, removed = 0, slope = variable ? 10 : 0, volume = 1e-9;
            /* Independent closed-form enthalpy integral: rho [cp(300) dT + cp' dT^2 / 2]. */
            for (int e = 0; e < 3; e++)
                for (int g = 0; g < 8; g++) {
                    double N[8], Tg = 0, gp = 1 / sqrt(3.0);
                    hex8_shape(HEX8_XI[g][0] * gp, HEX8_XI[g][1] * gp, HEX8_XI[g][2] * gp, N, NULL);
                    for (int a = 0; a < 8; a++) Tg += N[a] * capture.T[mesh.conn[8 * e + a]];
                    double dT = Tg - 300;
                    double energy = volume / 8 * 1000 * (1000 * dT + 0.5 * slope * dT * dT);
                    if (support && e == 0) removed += 0.25 * energy;
                    else stored += energy;
                }
            double supplied = (support ? 2.25 : 3) * volume * 1000 * (1000 * 100 + 0.5 * slope * 100 * 100);
            double relative = fabs(stored + removed + sum.bed_heat - supplied) / supplied;
            REPORT("%s %s cp%s, three-layer deposition: supplied %.12g J, stored %.12g J, bed %.12g J, correction %.12g J; "
                   "independent whole-print error %.2e, deposition identity %.2e; nodal range %.6f..%.6f K",
                   support ? "F13" : "F12", variable ? "linear" : "constant", support ? " with removable support" : "", supplied, stored, sum.bed_heat, sum.deposition_correction, relative,
                   sum.deposition_balance, capture.low, capture.high);
            CHECK(ok && relative < 1e-7 && fabs(sum.deposition_heat / supplied - 1) < 1e-12 &&
                  sum.deposition_balance < 1e-12 && sum.deposition_correction > 0 &&
                  fabs(sum.stored_heat - stored) / supplied < 1e-10 && sum.global_heat_balance < 1e-7 &&
                  fabs(sum.removed_heat - removed) / supplied < 1e-10 && (!support || (sum.support_elements == 1 && removed > 0)) &&
                  capture.low >= 300 - 1e-6 && capture.high <= 400 + 1e-6,
                  "F12/F13: deposition and support removal conserve physical nozzle enthalpy and respect thermal bounds: %s", err);
            if (support) {
                CHECK(ok && capture.extrema_error <= 1 && fabs(sum.warp_z_min - capture.remaining_warp_min) <= 1e-12 &&
                      fabs(sum.warp_z_max - capture.remaining_warp_max) <= 1e-12,
                      "F15: removed supports cannot control shown-part extrema");
            }
        }
        free(b.xyz), free(b.rho), free(b.conn);
    }
    {
        BoxMesh b = box_hex(1, 1, 1, 1, 1, 1, 1);
        LpbfMesh mesh = {b.nn, b.ne, b.xyz, b.conn};
        unsigned char *held = malloc(3 * (size_t)b.nn);
        memset(held, 1, 3 * (size_t)b.nn);
        const double E = 100e9, nu = 0.3, eigen[3] = {-1e-3, 0, 0}, zero[3] = {0, 0, 0};
        double r[3], r1 = 0, r2 = 0, rzero = 0;
        LpbfModel *m = lpbf_new(&mesh, E, nu, held, HEX8_INCOMPATIBLE, SOLID_SOLVER_DIRECT, 1e-12, err, sizeof err);
        bool ok = m != NULL;
        int one = 0;
        if (ok) lpbf_activate(m, &one, 1);
        if (ok) ok = lpbf_strain(m, &one, 1, eigen, err, sizeof err);
        if (ok) r1 = lpbf_plate_reaction(m, r);
        if (ok) ok = lpbf_strain(m, &one, 1, eigen, err, sizeof err);
        if (ok) r2 = lpbf_plate_reaction(m, r);
        if (ok) ok = lpbf_strain(m, NULL, 0, zero, err, sizeof err);
        if (ok) rzero = lpbf_plate_reaction(m, r);
        double want = E * (1 - nu) / ((1 + nu) * (1 - 2 * nu)) * 1e-3 / 4;
        REPORT("V8 total plate nodal reaction: one increment %.12g N, two %.12g N, zero-strain equilibration %.12g N; exact first %.12g N",
               r1, r2, rzero, want);
        CHECK(ok && fabs(r1 / want - 1) < 1e-10 && fabs(r2 / (2 * want) - 1) < 1e-10 && fabs(rzero / r2 - 1) < 1e-10,
              "V8: total accumulated elastic plate reactions equal analytical traction and persist: %s", err);
        lpbf_free(m);
        memset(held, 0, 3 * (size_t)b.nn);
        int A = bnode(&b, 0, 0, 0), B = bnode(&b, 1, 0, 0), C = bnode(&b, 0, 1, 0);
        held[3 * A] = held[3 * A + 1] = held[3 * A + 2] = 1;
        held[3 * B + 1] = held[3 * B + 2] = held[3 * C + 2] = 1;
        m = lpbf_new(&mesh, E, nu, held, HEX8_INCOMPATIBLE, SOLID_SOLVER_DIRECT, 1e-12, err, sizeof err);
        ok = m != NULL;
        const double free_eigen[3] = {-0.001, -0.001, -0.001};
        if (ok) lpbf_activate(m, &one, 1);
        if (ok) ok = lpbf_strain(m, &one, 1, free_eigen, err, sizeof err);
        double equilibrium = ok ? lpbf_equilibrium_error(m) : INFINITY;
        REPORT("V10 freely contracted hex: last elastic solve equilibrium error %.2e", equilibrium);
        CHECK(ok && equilibrium < 1e-9, "V10: vanishing recovered stress does not amplify the equilibrium roundoff: %s", err);
        lpbf_free(m);
        free(held), free(b.xyz), free(b.rho), free(b.conn);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--printing")) {
        test_printing_numerics();
        test_fff_print();
        test_lpbf_build();
        printf("\nPRINTING VERIFICATION: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    test_math();
    test_mass_properties();
    test_free_body();
    test_pendulum();
    test_damped_oscillator();
    test_two_link();
    test_limits();
    test_fourbar();
    test_coupling();
    test_determinism();
    test_fixed_and_validation();
    test_assembly_native();
    test_assembly_stl();
    test_urdf();
    test_actuators();
    test_controllers_sensors();
    test_load_transfer();
    test_contact();
    test_contact_runtime();
    test_orthotropic();
    test_modal();
    test_flexible_core();
    test_flexible_reduction();
    test_flexible_assembly();
    test_printing_numerics();
    test_fff_print();
    test_print_results();
    test_adaptive_mesh();
    test_support_cells();
    test_lpbf_build();
    if (g_tmp[0]) rmdir(g_tmp);
    if (g_fail) {
        printf("\nMECH VERIFICATION TESTS FAILED: %d passed, %d failed\n", g_pass, g_fail);
        return 1;
    }
    printf("\nALL MECH VERIFICATION TESTS PASSED: %d passed, 0 failed\n", g_pass);
    return 0;
}
