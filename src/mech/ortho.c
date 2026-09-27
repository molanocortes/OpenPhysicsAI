/* ortho.c - orthotropic elasticity, print axes, strength criteria and the hex8 solver (see ortho.h) */
#include "ortho.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fem/dense.h"

/* Voigt index pairs: 11 22 33 12 23 31 */
static const int VI[6] = {0, 1, 2, 0, 1, 2}, VJ[6] = {0, 1, 2, 1, 2, 0};

/* ------------------------------------------------------------------------------------------------ constitutive */

static bool chol3_ok(const double A[9]) {
    double l00 = A[0];
    if (!(l00 > 1e-12)) return false;
    double a = A[1] / sqrt(l00), b = A[2] / sqrt(l00);
    double l11 = A[4] - a * a;
    if (!(l11 > 1e-12)) return false;
    double c = (A[5] - a * b) / sqrt(l11);
    double l22 = A[8] - b * b - c * c;
    return l22 > 1e-12;
}

bool ortho_compliance(const OrthoConstants *k, double S[6][6], char *why, size_t n) {
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++) S[i][j] = 0;
    for (int i = 0; i < 3; i++)
        if (!(k->E[i] > 0) || !isfinite(k->E[i])) {
            if (why) snprintf(why, n, "E%d must be positive and finite (%g Pa)", i + 1, k->E[i]);
            return false;
        }
    if (!(k->G12 > 0) || !(k->G23 > 0) || !(k->G13 > 0) || !isfinite(k->G12) || !isfinite(k->G23) || !isfinite(k->G13)) {
        if (why) snprintf(why, n, "shear moduli G12, G23, G13 must be positive and finite");
        return false;
    }
    if (!isfinite(k->nu12) || !isfinite(k->nu13) || !isfinite(k->nu23)) {
        if (why) snprintf(why, n, "Poisson ratios must be finite");
        return false;
    }
    S[0][0] = 1 / k->E[0], S[1][1] = 1 / k->E[1], S[2][2] = 1 / k->E[2];
    S[0][1] = S[1][0] = -k->nu12 / k->E[0];
    S[0][2] = S[2][0] = -k->nu13 / k->E[0];
    S[1][2] = S[2][1] = -k->nu23 / k->E[1];
    S[3][3] = 1 / k->G12, S[4][4] = 1 / k->G23, S[5][5] = 1 / k->G13;
    double A[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) A[3 * i + j] = S[i][j] / sqrt(S[i][i] * S[j][j]);
    if (!chol3_ok(A)) {
        if (why)
            snprintf(why, n, "the Poisson ratios are not admissible: the compliance is not positive definite (it needs nu12^2 < E1/E2, nu13^2 < E1/E3, "
                             "nu23^2 < E2/E3 and a positive determinant of the normal block)");
        return false;
    }
    return true;
}

bool ortho_stiffness(const OrthoConstants *k, double C[6][6], char *why, size_t n) {
    double S[6][6];
    if (!ortho_compliance(k, S, why, n)) return false;
    double A[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) A[3 * i + j] = S[i][j];
    if (!dense_spd_inverse(A, 3)) {
        if (why) snprintf(why, n, "the normal compliance block cannot be inverted");
        return false;
    }
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++) C[i][j] = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) C[i][j] = A[3 * i + j];
    for (int i = 3; i < 6; i++) C[i][i] = 1 / S[i][i];
    return true;
}

void ortho_isotropic(double E, double nu, OrthoConstants *k) {
    k->E[0] = k->E[1] = k->E[2] = E;
    k->nu12 = k->nu13 = k->nu23 = nu;
    k->G12 = k->G23 = k->G13 = E / (2 * (1 + nu));
}

void voigt_strain_transform(const double R[9], double T[6][6]) {
#define Q(i, k) R[3 * (k) + (i)] /* component k of material axis i */
    for (int a = 0; a < 6; a++) {
        int i = VI[a], j = VJ[a];
        for (int b = 0; b < 6; b++) {
            int k = VI[b], l = VJ[b];
            if (a < 3)
                T[a][b] = b < 3 ? Q(i, k) * Q(i, k) : Q(i, k) * Q(i, l);
            else
                T[a][b] = b < 3 ? 2 * Q(i, k) * Q(j, k) : Q(i, k) * Q(j, l) + Q(i, l) * Q(j, k);
        }
    }
#undef Q
}

void ortho_global_D(const double C[6][6], const double R[9], double D[6][6]) {
    double T[6][6], CT[6][6];
    voigt_strain_transform(R, T);
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++) {
            double s = 0;
            for (int k = 0; k < 6; k++) s += C[i][k] * T[k][j];
            CT[i][j] = s;
        }
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++) {
            double s = 0;
            for (int k = 0; k < 6; k++) s += T[k][i] * CT[k][j];
            D[i][j] = s;
        }
    for (int i = 0; i < 6; i++) /* exact symmetry */
        for (int j = i + 1; j < 6; j++) D[i][j] = D[j][i] = 0.5 * (D[i][j] + D[j][i]);
}

void voigt_stress_to_material(const double R[9], const double s[6], double out[6]) {
    double g[3][3] = {{s[0], s[3], s[5]}, {s[3], s[1], s[4]}, {s[5], s[4], s[2]}}, tmp[3][3], m[3][3];
    for (int i = 0; i < 3; i++) /* tmp = sigma R */
        for (int j = 0; j < 3; j++) tmp[i][j] = g[i][0] * R[3 * 0 + j] + g[i][1] * R[3 * 1 + j] + g[i][2] * R[3 * 2 + j];
    for (int i = 0; i < 3; i++) /* m = R^T tmp */
        for (int j = 0; j < 3; j++) m[i][j] = R[3 * 0 + i] * tmp[0][j] + R[3 * 1 + i] * tmp[1][j] + R[3 * 2 + i] * tmp[2][j];
    out[0] = m[0][0], out[1] = m[1][1], out[2] = m[2][2];
    out[3] = 0.5 * (m[0][1] + m[1][0]), out[4] = 0.5 * (m[1][2] + m[2][1]), out[5] = 0.5 * (m[2][0] + m[0][2]);
}

bool ortho_print_axes(const double b[3], const double xref[3], double raster, double R[9]) {
    double nb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (!(nb > 0)) return false;
    double a3[3] = {b[0] / nb, b[1] / nb, b[2] / nb}, d = xref[0] * a3[0] + xref[1] * a3[1] + xref[2] * a3[2];
    double p[3] = {xref[0] - d * a3[0], xref[1] - d * a3[1], xref[2] - d * a3[2]};
    double np = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (!(np > 1e-9 * (sqrt(xref[0] * xref[0] + xref[1] * xref[1] + xref[2] * xref[2]) + 1e-300))) return false;
    double e1[3] = {p[0] / np, p[1] / np, p[2] / np};
    double e2[3] = {a3[1] * e1[2] - a3[2] * e1[1], a3[2] * e1[0] - a3[0] * e1[2], a3[0] * e1[1] - a3[1] * e1[0]};
    double c = cos(raster), s = sin(raster), a1[3], a2[3];
    for (int k = 0; k < 3; k++) a1[k] = c * e1[k] + s * e2[k];
    a2[0] = a3[1] * a1[2] - a3[2] * a1[1], a2[1] = a3[2] * a1[0] - a3[0] * a1[2], a2[2] = a3[0] * a1[1] - a3[1] * a1[0];
    for (int r = 0; r < 3; r++) R[3 * r + 0] = a1[r], R[3 * r + 1] = a2[r], R[3 * r + 2] = a3[r];
    return true;
}

/* ------------------------------------------------------------------------------------------------ strength */

static const char *const CRIT_NAMES[STRENGTH_CRITERIA] = {"max_stress", "tsai_wu", "tsai_hill"};
const char *strength_criterion_name(StrengthCriterion c) { return c >= 0 && c < STRENGTH_CRITERIA ? CRIT_NAMES[c] : "?"; }
int strength_criterion_from_name(const char *s) {
    for (int i = 0; s && i < STRENGTH_CRITERIA; i++)
        if (!strcmp(s, CRIT_NAMES[i])) return i;
    return -1;
}

static const char *const MODE_NAMES[9] = {"1t (tension along axis 1)",       "1c (compression along axis 1)",  "2t (tension along axis 2)",
                                          "2c (compression along axis 2)",   "3t (interlayer tension)",        "3c (compression across layers)",
                                          "s12 (in-plane shear)",            "s23 (interlayer shear, plane 23)", "s31 (interlayer shear, plane 31)"};
const char *strength_mode_name(int mode) { return mode >= 0 && mode < 9 ? MODE_NAMES[mode] : "none"; }

bool strength_valid(const OrthoStrength *s, StrengthCriterion c, char *why, size_t n) {
    const double v[9] = {s->Xt, s->Xc, s->Yt, s->Yc, s->Zt, s->Zc, s->S12, s->S23, s->S31};
    static const char *const nm[9] = {"Xt", "Xc", "Yt", "Yc", "Zt", "Zc", "S12", "S23", "S31"};
    for (int i = 0; i < 9; i++)
        if (!(v[i] > 0) || !isfinite(v[i])) {
            if (why) snprintf(why, n, "strength %s must be positive and finite", nm[i]);
            return false;
        }
    if (c == STRENGTH_TSAI_WU) { /* the normalised quadratic form must be positive semidefinite (f = -1/2 for all pairs is the semidefinite
                                   * Tsai-Hahn choice: no curvature along one stress direction, where only the linear terms act) */
        double f12 = s->f12, f13 = s->f13, f23 = s->f23;
        double det = 1 + 2 * f12 * f13 * f23 - f12 * f12 - f13 * f13 - f23 * f23;
        if (!isfinite(f12) || !isfinite(f13) || !isfinite(f23) || fabs(f12) > 1 || fabs(f13) > 1 || fabs(f23) > 1 || det < -1e-12) {
            if (why)
                snprintf(why, n, "Tsai-Wu interaction coefficients f12, f13, f23 must keep the quadratic form positive semidefinite (|f| <= 1 and "
                                 "1 + 2 f12 f13 f23 - f12^2 - f13^2 - f23^2 >= 0)");
            return false;
        }
    }
    return true;
}

void strength_evaluate(StrengthCriterion c, const OrthoStrength *st, const double s[6], StrengthResult *r) {
    r->index = 0, r->ratio = INFINITY, r->mode = -1;
    double term[9] = {0};
    if (c == STRENGTH_MAX_STRESS) {
        term[0] = s[0] > 0 ? s[0] / st->Xt : 0, term[1] = s[0] < 0 ? -s[0] / st->Xc : 0;
        term[2] = s[1] > 0 ? s[1] / st->Yt : 0, term[3] = s[1] < 0 ? -s[1] / st->Yc : 0;
        term[4] = s[2] > 0 ? s[2] / st->Zt : 0, term[5] = s[2] < 0 ? -s[2] / st->Zc : 0;
        term[6] = fabs(s[3]) / st->S12, term[7] = fabs(s[4]) / st->S23, term[8] = fabs(s[5]) / st->S31;
        for (int i = 0; i < 9; i++)
            if (term[i] > r->index) r->index = term[i], r->mode = i;
        r->ratio = r->index > 0 ? 1 / r->index : INFINITY;
        return;
    }
    if (c == STRENGTH_TSAI_WU) {
        double F1 = 1 / st->Xt - 1 / st->Xc, F2 = 1 / st->Yt - 1 / st->Yc, F3 = 1 / st->Zt - 1 / st->Zc;
        double F11 = 1 / (st->Xt * st->Xc), F22 = 1 / (st->Yt * st->Yc), F33 = 1 / (st->Zt * st->Zc);
        double F44 = 1 / (st->S12 * st->S12), F55 = 1 / (st->S23 * st->S23), F66 = 1 / (st->S31 * st->S31);
        double F12 = st->f12 * sqrt(F11 * F22), F13 = st->f13 * sqrt(F11 * F33), F23 = st->f23 * sqrt(F22 * F33);
        double L = F1 * s[0] + F2 * s[1] + F3 * s[2];
        double Qd = F11 * s[0] * s[0] + F22 * s[1] * s[1] + F33 * s[2] * s[2] + F44 * s[3] * s[3] + F55 * s[4] * s[4] + F66 * s[5] * s[5] +
                    2 * (F12 * s[0] * s[1] + F13 * s[0] * s[2] + F23 * s[1] * s[2]);
        double R;
        if (Qd > 0) {
            double disc = sqrt(L * L + 4 * Qd);
            R = L >= 0 ? 2 / (L + disc) : (-L + disc) / (2 * Qd);
        } else
            R = L > 0 ? 1 / L : INFINITY;
        r->ratio = R, r->index = isfinite(R) && R > 0 ? 1 / R : 0;
        if (!isfinite(R)) return;
        /* governing term: the largest single-axis contribution on the envelope */
        double a[3] = {R * s[0], R * s[1], R * s[2]};
        double axis[3] = {F1 * a[0] + F11 * a[0] * a[0], F2 * a[1] + F22 * a[1] * a[1], F3 * a[2] + F33 * a[2] * a[2]};
        double best = -INFINITY;
        for (int i = 0; i < 3; i++)
            if (axis[i] > best) best = axis[i], r->mode = 2 * i + (s[i] < 0);
        double sh[3] = {F44 * R * R * s[3] * s[3], F55 * R * R * s[4] * s[4], F66 * R * R * s[5] * s[5]};
        for (int i = 0; i < 3; i++)
            if (sh[i] > best) best = sh[i], r->mode = 6 + i;
        return;
    }
    /* Tsai-Hill (3D Hill form, strengths chosen by the sign of each normal stress) */
    double X = s[0] >= 0 ? st->Xt : st->Xc, Y = s[1] >= 0 ? st->Yt : st->Yc, Z = s[2] >= 0 ? st->Zt : st->Zc;
    double iX = 1 / (X * X), iY = 1 / (Y * Y), iZ = 1 / (Z * Z);
    term[s[0] < 0] = s[0] * s[0] * iX;
    term[2 + (s[1] < 0)] = s[1] * s[1] * iY;
    term[4 + (s[2] < 0)] = s[2] * s[2] * iZ;
    term[6] = s[3] * s[3] / (st->S12 * st->S12), term[7] = s[4] * s[4] / (st->S23 * st->S23), term[8] = s[5] * s[5] / (st->S31 * st->S31);
    double FI = -s[0] * s[1] * (iX + iY - iZ) - s[1] * s[2] * (iY + iZ - iX) - s[2] * s[0] * (iZ + iX - iY);
    double best = 0;
    for (int i = 0; i < 9; i++) {
        FI += term[i];
        if (term[i] > best) best = term[i], r->mode = i;
    }
    if (FI > 0) r->index = sqrt(FI), r->ratio = 1 / r->index;
}

/* ------------------------------------------------------------------------------------------------ solver */

enum { ORTHO_KE_CACHE = 256 };

static void elem_X(const OrthoModel *m, int e, double X[8][3]) {
    for (int a = 0; a < 8; a++) {
        const double *p = m->xyz + 3 * (size_t)m->conn[8 * (size_t)e + a];
        X[a][0] = p[0], X[a][1] = p[1], X[a][2] = p[2];
    }
}

static const double *elem_R(const OrthoModel *m, int e) {
    static const double I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (!m->axes) return I;
    return m->axes + 9 * (size_t)(m->axes_count > 1 ? e : 0);
}

bool ortho_factor(int nn, const double *xyz, const CsrMatrix *K, const int *eq, int neq, int64_t max_factor_nnz, SolveProgressFn progress, void *ctx,
                  CholFactor *F, SolveStats *st, bool *too_big, char *err, size_t errlen) {
    *too_big = false;
    memset(F, 0, sizeof *F);
    bool ok = false;
    int64_t *adjp = calloc((size_t)nn + 1, sizeof(int64_t));
    int *perm = malloc((size_t)(neq ? neq : 1) * sizeof(int)), *eq_node = malloc((size_t)(neq ? neq : 1) * sizeof(int));
    int *mark = malloc((size_t)(nn ? nn : 1) * sizeof(int)), *adj = NULL;
    if (!adjp || !perm || !eq_node || !mark) {
        snprintf(err, errlen, "out of memory ordering %d equations", neq);
        goto done;
    }
    for (int nd = 0; nd < nn; nd++)
        for (int k = 0; k < 3; k++)
            if (eq[3 * nd + k] >= 0) eq_node[eq[3 * nd + k]] = nd;
    for (int i = 0; i < nn; i++) mark[i] = -1;
    for (int q = 0; q < neq; q++) {
        int nd = eq_node[q];
        if (q > 0 && eq_node[q - 1] == nd) continue;
        int64_t cnt = 0;
        for (int64_t k = K->rowptr[q]; k < K->rowptr[q + 1]; k++) {
            int o = eq_node[K->col[k]];
            if (o != nd && mark[o] != nd) mark[o] = nd, cnt++;
        }
        adjp[nd + 1] = cnt;
    }
    for (int i = 0; i < nn; i++) adjp[i + 1] += adjp[i];
    adj = malloc((size_t)(adjp[nn] ? adjp[nn] : 1) * sizeof(int));
    if (!adj) {
        snprintf(err, errlen, "out of memory ordering %d equations", neq);
        goto done;
    }
    for (int i = 0; i < nn; i++) mark[i] = -1;
    for (int q = 0; q < neq; q++) {
        int nd = eq_node[q];
        if (q > 0 && eq_node[q - 1] == nd) continue;
        int64_t w = adjp[nd];
        for (int64_t k = K->rowptr[q]; k < K->rowptr[q + 1]; k++) {
            int o = eq_node[K->col[k]];
            if (o != nd && mark[o] != nd) mark[o] = nd, adj[w++] = o;
        }
    }
    if (!nested_dissection_order(nn, xyz, adjp, adj, eq, neq, perm, err, errlen)) goto done;
    int64_t fnnz = chol_symbolic_nnz(K, perm, err, errlen);
    int64_t limit = max_factor_nnz > 0 ? max_factor_nnz : 60000000;
    if (fnnz < 0 || fnnz > limit) {
        *too_big = true;
        snprintf(err, errlen, "the direct solver needs %lld factor entries (limit %lld)", (long long)fnnz, (long long)limit);
        goto done;
    }
    int bad = -1;
    if (!chol_factor(K, perm, F, progress, ctx, st, &bad, err, errlen)) {
        for (int nd = 0; bad >= 0 && nd < nn; nd++)
            for (int k = 0; k < 3; k++)
                if (eq[3 * nd + k] == bad) {
                    size_t l = strlen(err);
                    snprintf(err + l, errlen - l, " [node %d, component %c, at %.4g %.4g %.4g m]", nd, "xyz"[k], xyz[3 * nd], xyz[3 * nd + 1], xyz[3 * nd + 2]);
                }
        goto done;
    }
    ok = true;
done:
    free(adjp), free(perm), free(eq_node), free(mark), free(adj);
    return ok;
}

/* direct solve; *fallback when the factor is too big for the automatic choice */
static bool direct_solve(const OrthoModel *m, CsrMatrix *K, const int *eq, int neq, const double *f, double *x, const SolidOptions *opt, SolveStats *st,
                         bool *fallback, char *err, size_t errlen) {
    CholFactor F;
    bool too_big = false;
    *fallback = false;
    if (!ortho_factor(m->nnodes, m->xyz, K, eq, neq, opt->max_factor_nnz, opt->progress, opt->ctx, &F, st, &too_big, err, errlen)) {
        if (too_big && opt->solver != SOLID_SOLVER_DIRECT) {
            *fallback = true;
            return true;
        }
        if (too_big) {
            size_t l = strlen(err);
            snprintf(err + l, errlen - l, "; use the iterative solver");
        }
        return false;
    }
    chol_solve(&F, f, x);
    chol_free(&F);
    st->converged = true;
    return true;
}

bool ortho_solve(const OrthoModel *m, const OrthoLoads *L, const SolidOptions *optp, OrthoResult *res, char *err, size_t errlen) {
    memset(res, 0, sizeof *res);
    SolidOptions opt = optp ? *optp : (SolidOptions){0};
    if (opt.pcg_tol <= 0) opt.pcg_tol = 1e-10;
    if (opt.pcg_max_iter <= 0) opt.pcg_max_iter = 20000;
    int nn = m->nnodes, ne = m->nelems;
    size_t NN = (size_t)(nn ? nn : 1), NE = (size_t)(ne ? ne : 1);
    bool ok = false;
    int *eq = malloc(3 * NN * sizeof(int)), *elem_dofs = NULL, *ke_index = malloc(NE * sizeof(int)), *block_of = NULL;
    unsigned char *used = calloc(NN, 1);
    int nD = m->axes && m->axes_count > 1 ? ne : (m->nmat ? m->nmat : 1);
    double (*C)[6][6] = malloc((size_t)(m->nmat ? m->nmat : 1) * sizeof *C), (*D)[6][6] = malloc((size_t)(nD ? nD : 1) * sizeof *D);
    double *ke_cache = malloc(576 * sizeof(double) * ORTHO_KE_CACHE), *f = NULL, *x = NULL, *fext = NULL, *fint = NULL, *nsum = NULL, *ncount = NULL;
    int64_t (*keys)[25] = malloc(sizeof *keys * ORTHO_KE_CACHE);
    CsrMatrix K = {0};
    if (!eq || !ke_index || !used || !C || !D || !ke_cache || !keys) goto oom;
    for (int i = 0; i < m->nmat; i++) {
        char why[300];
        if (!ortho_stiffness(&m->mat[i], C[i], why, sizeof why)) {
            snprintf(err, errlen, "material %d: %s", i, why);
            goto fail;
        }
    }
    /* constitutive matrix per material (common axes) or per element (element axes) */
    for (int d = 0; d < nD; d++) {
        int e = nD == ne && m->axes && m->axes_count > 1 ? d : -1;
        int mi = e >= 0 ? (m->elem_mat ? m->elem_mat[e] : 0) : d;
        ortho_global_D(C[mi], elem_R(m, e >= 0 ? e : 0), D[d]);
    }
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 8; a++) used[m->conn[8 * (size_t)e + a]] = 1;
    int neq = 0;
    for (int nd = 0; nd < nn; nd++)
        for (int k = 0; k < 3; k++) eq[3 * nd + k] = (used[nd] && !L->fixed[3 * (size_t)nd + k]) ? neq++ : -1;
    res->neq = neq;
    /* element stiffness with a cache for translated copies (voxel meshes) sharing a constitutive matrix */
    int ncache = 0;
    double mindet = INFINITY;
    for (int e = 0; e < ne; e++) {
        ke_index[e] = -1;
        double X[8][3];
        elem_X(m, e, X);
        int di = nD == ne && m->axes && m->axes_count > 1 ? e : (m->elem_mat ? m->elem_mat[e] : 0);
        double size = 0;
        for (int k = 0; k < 3; k++) size = fmax(size, fabs(X[6][k] - X[0][k]));
        int64_t key[25];
        bool keyed = size > 0;
        for (int a = 0; keyed && a < 8; a++)
            for (int k = 0; k < 3; k++) {
                double q = (X[a][k] - X[0][k]) / (size * 1e-9);
                if (fabs(q) > 9e15) keyed = false;
                key[3 * a + k] = llround(q);
            }
        key[24] = di;
        int found = -1;
        for (int c = ncache - 1; keyed && c >= 0 && found < 0; c--)
            if (!memcmp(keys[c], key, sizeof key)) found = c;
        if (found >= 0) {
            ke_index[e] = found;
            res->stiffness_cache_hits++;
            continue;
        }
        if (!keyed || ncache == ORTHO_KE_CACHE) continue;
        double mdj;
        if (!hex8_stiffness(X, D[di], m->formulation, ke_cache + 576 * (size_t)ncache, &mdj)) {
            snprintf(err, errlen, "element %d is inverted or degenerate (Jacobian determinant %.3g)", e, mdj);
            goto fail;
        }
        mindet = fmin(mindet, mdj);
        memcpy(keys[ncache], key, sizeof key);
        ke_index[e] = ncache++;
    }
    elem_dofs = malloc(NE * 24 * sizeof(int));
    if (!elem_dofs) goto oom;
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) elem_dofs[24 * (size_t)e + 3 * (size_t)a + (size_t)k] = eq[3 * m->conn[8 * (size_t)e + a] + k];
    if (!csr_from_elements(&K, neq, ne, 24, elem_dofs, err, errlen)) goto fail;
    f = calloc((size_t)(neq ? neq : 1), sizeof(double));
    fext = calloc(3 * NN, sizeof(double));
    if (!f || !fext) goto oom;
    double Ke[576];
    for (int e = 0; e < ne; e++) {
        const double *ke = ke_index[e] >= 0 ? ke_cache + 576 * (size_t)ke_index[e] : NULL;
        if (!ke) {
            double X[8][3], mdj;
            elem_X(m, e, X);
            int di = nD == ne && m->axes && m->axes_count > 1 ? e : (m->elem_mat ? m->elem_mat[e] : 0);
            if (!hex8_stiffness(X, D[di], m->formulation, Ke, &mdj)) {
                snprintf(err, errlen, "element %d is inverted or degenerate (Jacobian determinant %.3g)", e, mdj);
                goto fail;
            }
            mindet = fmin(mindet, mdj);
            ke = Ke;
        }
        const int *dofs = elem_dofs + 24 * (size_t)e;
        csr_add_element(&K, 24, dofs, ke);
        if (L->fixed_value) /* prescribed displacements move to the right-hand side */
            for (int a = 0; a < 24; a++) {
                if (dofs[a] < 0) continue;
                double s = 0;
                for (int b = 0; b < 24; b++) {
                    if (dofs[b] >= 0) continue;
                    int nd = m->conn[8 * (size_t)e + b / 3];
                    s += ke[24 * a + b] * L->fixed_value[3 * (size_t)nd + (size_t)(b % 3)];
                }
                f[dofs[a]] -= s;
            }
    }
    res->min_detJ = mindet;
    if (L->nodal_force)
        for (int nd = 0; nd < nn; nd++)
            for (int k = 0; k < 3; k++) {
                double v = L->nodal_force[3 * (size_t)nd + (size_t)k];
                fext[3 * (size_t)nd + (size_t)k] += v;
                int q = eq[3 * nd + k];
                if (q >= 0) f[q] += v;
            }
    res->u = calloc(3 * NN, sizeof(double));
    x = calloc((size_t)(neq ? neq : 1), sizeof(double));
    if (!res->u || !x) goto oom;
    if (neq > 0) {
        bool direct = opt.solver == SOLID_SOLVER_DIRECT || (opt.solver == SOLID_SOLVER_AUTO && neq <= 250000), fallback = false;
        if (direct && !direct_solve(m, &K, eq, neq, f, x, &opt, &res->stats, &fallback, err, errlen)) goto fail;
        if (direct && !fallback)
            snprintf(res->stats.method, sizeof res->stats.method, "sparse Cholesky (nested dissection)");
        else {
            block_of = malloc((size_t)neq * sizeof(int));
            if (!block_of) goto oom;
            int nb = 0;
            for (int nd = 0; nd < nn; nd++) {
                bool any = false;
                for (int k = 0; k < 3; k++)
                    if (eq[3 * nd + k] >= 0) block_of[eq[3 * nd + k]] = nb, any = true;
                nb += any;
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
            if (q >= 0) res->u[3 * (size_t)nd + (size_t)k] = x[q];
            else if (L->fixed[3 * (size_t)nd + (size_t)k] && L->fixed_value) res->u[3 * (size_t)nd + (size_t)k] = L->fixed_value[3 * (size_t)nd + (size_t)k];
        }
    csr_free(&K);
    /* recovery */
    res->gp_stress = malloc(NE * 48 * sizeof(double));
    res->gp_stress_mat = malloc(NE * 48 * sizeof(double));
    res->gp_strain = malloc(NE * 48 * sizeof(double));
    res->node_stress = calloc(6 * NN, sizeof(double));
    res->reaction = calloc(3 * NN, sizeof(double));
    fint = calloc(3 * NN, sizeof(double));
    nsum = calloc(6 * NN, sizeof(double));
    ncount = calloc(NN, sizeof(double));
    if (!res->gp_stress || !res->gp_stress_mat || !res->gp_strain || !res->node_stress || !res->reaction || !fint || !nsum || !ncount) goto oom;
    for (int e = 0; e < ne; e++) {
        double X[8][3], ue[24], fe[24], eps[8][6], sig[8][6], nodal[48];
        elem_X(m, e, X);
        int di = nD == ne && m->axes && m->axes_count > 1 ? e : (m->elem_mat ? m->elem_mat[e] : 0);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) ue[3 * a + k] = res->u[3 * (size_t)m->conn[8 * (size_t)e + a] + (size_t)k];
        const double *ke = ke_index[e] >= 0 ? ke_cache + 576 * (size_t)ke_index[e] : NULL;
        if (!ke) {
            double mdj;
            hex8_stiffness(X, D[di], m->formulation, Ke, &mdj);
            ke = Ke;
        }
        double w = 0;
        for (int a = 0; a < 24; a++) {
            double s = 0;
            for (int b = 0; b < 24; b++) s += ke[24 * a + b] * ue[b];
            fe[a] = s, w += s * ue[a];
        }
        res->strain_energy += 0.5 * w;
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) fint[3 * (size_t)m->conn[8 * (size_t)e + a] + (size_t)k] += fe[3 * a + k];
        hex8_gauss_strain_stress(X, D[di], m->formulation, ue, NULL, eps, sig);
        const double *R = elem_R(m, e);
        double flat[48];
        for (int g = 0; g < 8; g++) {
            memcpy(res->gp_stress + 48 * (size_t)e + 6 * (size_t)g, sig[g], 6 * sizeof(double));
            memcpy(res->gp_strain + 48 * (size_t)e + 6 * (size_t)g, eps[g], 6 * sizeof(double));
            voigt_stress_to_material(R, sig[g], res->gp_stress_mat + 48 * (size_t)e + 6 * (size_t)g);
            memcpy(flat + 6 * g, sig[g], 6 * sizeof(double));
        }
        hex8_extrapolate(flat, 6, nodal);
        for (int a = 0; a < 8; a++) {
            int nd = m->conn[8 * (size_t)e + a];
            for (int k = 0; k < 6; k++) nsum[6 * (size_t)nd + (size_t)k] += nodal[6 * a + k];
            ncount[nd] += 1;
        }
    }
    double rfree = 0, aload = 0, rsum2 = 0;
    for (int nd = 0; nd < nn; nd++) {
        if (ncount[nd] > 0)
            for (int k = 0; k < 6; k++) res->node_stress[6 * (size_t)nd + (size_t)k] = nsum[6 * (size_t)nd + (size_t)k] / ncount[nd];
        for (int k = 0; k < 3; k++) {
            size_t i = 3 * (size_t)nd + (size_t)k;
            double r = fint[i] - fext[i];
            res->load_total[k] += fext[i];
            aload += fext[i] * fext[i];
            if (used[nd] && L->fixed[i]) {
                res->reaction[i] = r;
                res->reaction_total[k] += r;
                rsum2 += r * r;
                res->external_work += r * res->u[i];
            } else if (used[nd])
                rfree += r * r;
            res->external_work += fext[i] * res->u[i];
        }
    }
    double denom = sqrt(aload) + sqrt(rsum2);
    res->equilibrium_error = denom > 0 ? sqrt(rfree) / denom : sqrt(rfree);
    ok = true;
    goto done;
oom:
    snprintf(err, errlen, "out of memory in the orthotropic solver (%d nodes, %d elements)", nn, ne);
fail:
    ortho_result_free(res);
done:
    csr_free(&K);
    free(eq), free(elem_dofs), free(ke_index), free(block_of), free(used), free(C), free(D), free(ke_cache), free(keys);
    free(f), free(x), free(fext), free(fint), free(nsum), free(ncount);
    return ok;
}

void ortho_element_D(const OrthoModel *m, int e, double D[6][6]) {
    double C[6][6];
    int mi = m->elem_mat ? m->elem_mat[e] : 0;
    if (!ortho_stiffness(&m->mat[mi], C, NULL, 0)) {
        for (int i = 0; i < 6; i++)
            for (int j = 0; j < 6; j++) D[i][j] = 0;
        return;
    }
    ortho_global_D(C, elem_R(m, e), D);
}

bool ortho_assemble_factor(const OrthoModel *m, const unsigned char *fixed, int *eq, int *neq, CholFactor *F, SolveStats *st, char *err, size_t errlen) {
    int nn = m->nnodes, ne = m->nelems;
    unsigned char *used = calloc((size_t)(nn ? nn : 1), 1);
    int *dofs = malloc((size_t)(ne ? ne : 1) * 24 * sizeof(int));
    CsrMatrix K = {0};
    bool ok = false;
    if (!used || !dofs) {
        snprintf(err, errlen, "out of memory assembling %d elements", ne);
        goto done;
    }
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 8; a++) used[m->conn[8 * (size_t)e + a]] = 1;
    int n = 0;
    for (int nd = 0; nd < nn; nd++)
        for (int k = 0; k < 3; k++) eq[3 * nd + k] = used[nd] && !(fixed && fixed[3 * (size_t)nd + (size_t)k]) ? n++ : -1;
    *neq = n;
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) dofs[24 * (size_t)e + 3 * (size_t)a + (size_t)k] = eq[3 * m->conn[8 * (size_t)e + a] + k];
    if (!csr_from_elements(&K, n, ne, 24, dofs, err, errlen)) goto done;
    for (int e = 0; e < ne; e++) {
        double X[8][3], D[6][6], Ke[576], mdj;
        elem_X(m, e, X);
        ortho_element_D(m, e, D);
        if (!hex8_stiffness(X, D, m->formulation, Ke, &mdj)) {
            snprintf(err, errlen, "element %d is inverted or degenerate (Jacobian determinant %.3g)", e, mdj);
            goto done;
        }
        csr_add_element(&K, 24, dofs + 24 * (size_t)e, Ke);
    }
    bool too_big = false;
    ok = ortho_factor(nn, m->xyz, &K, eq, n, 0, NULL, NULL, F, st, &too_big, err, errlen);
done:
    csr_free(&K);
    free(used), free(dofs);
    return ok;
}

void ortho_gauss_stress(const OrthoModel *m, const double *u, double *gp) {
    for (int e = 0; e < m->nelems; e++) {
        double X[8][3], D[6][6], ue[24], eps[8][6], sig[8][6];
        elem_X(m, e, X);
        ortho_element_D(m, e, D);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) ue[3 * a + k] = u[3 * (size_t)m->conn[8 * (size_t)e + a] + (size_t)k];
        hex8_gauss_strain_stress(X, D, m->formulation, ue, NULL, eps, sig);
        for (int g = 0; g < 8; g++) memcpy(gp + 48 * (size_t)e + 6 * (size_t)g, sig[g], 6 * sizeof(double));
    }
}

void ortho_result_free(OrthoResult *r) {
    free(r->u), free(r->reaction), free(r->gp_stress), free(r->gp_stress_mat), free(r->gp_strain), free(r->node_stress);
    SolveStats st = r->stats;
    int neq = r->neq;
    memset(r, 0, sizeof *r);
    r->stats = st, r->neq = neq;
}

void ortho_strength_field(const OrthoResult *r, int nelems, StrengthCriterion c, const OrthoStrength *s, double *gp_index, StrengthSummary *sum) {
    memset(sum, 0, sizeof *sum);
    sum->min_ratio = INFINITY, sum->elem = -1, sum->gp = -1, sum->mode = -1;
    for (int e = 0; e < nelems; e++)
        for (int g = 0; g < 8; g++) {
            const double *sm = r->gp_stress_mat + 48 * (size_t)e + 6 * (size_t)g;
            StrengthResult sr;
            strength_evaluate(c, s, sm, &sr);
            if (gp_index) gp_index[8 * (size_t)e + (size_t)g] = sr.index;
            if (sr.index > sum->max_index || sum->elem < 0) {
                sum->max_index = sr.index, sum->min_ratio = sr.ratio, sum->elem = e, sum->gp = g, sum->mode = sr.mode;
                memcpy(sum->sig_m, sm, sizeof sum->sig_m);
            }
        }
}
