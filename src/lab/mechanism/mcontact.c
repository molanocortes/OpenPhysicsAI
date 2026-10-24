/* mcontact.c - see mcontact.h */
#include "mcontact.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mcontact_clear(MContactSet *cs) { cs->n = 0; }

void mcontact_free(MContactSet *cs) {
    free(cs->nterm), free(cs->term_body), free(cs->term_zone), free(cs->term_w), free(cs->g0), free(cs->m);
    free(cs->slave_body), free(cs->slave_node), free(cs->where), free(cs->normal), free(cs->cdiag), free(cs->mq);
    memset(cs, 0, sizeof *cs);
}

static bool grow(MContactSet *cs) {
    int cap = cs->cap ? 2 * cs->cap : 256;
#define G(p, k) do { void *q = realloc(cs->p, (size_t)cap * (k) * sizeof *cs->p); if (!q) return false; cs->p = q; } while (0)
    G(nterm, 1);
    G(term_body, MC_MAX_TERMS);
    G(term_zone, MC_MAX_TERMS);
    G(term_w, MC_MAX_TERMS);
    G(g0, 1);
    G(m, 1);
    G(slave_body, 1);
    G(slave_node, 1);
    G(where, 3);
    G(normal, 3);
    G(cdiag, 1);
    G(mq, MC_MAX_DOF);
#undef G
    cs->cap = cap;
    return true;
}

int mcontact_add(MContactSet *cs, double g0, double m, int nterm, const int *body, const int *zone, const double *w, int slave_body,
                 int slave_node, const double where[3], const double normal[3]) {
    if (nterm < 1 || nterm > MC_MAX_TERMS) return -1;
    if (cs->n == cs->cap && !grow(cs)) return -1;
    int j = cs->n++;
    cs->nterm[j] = nterm;
    for (int t = 0; t < nterm; t++)
        cs->term_body[MC_MAX_TERMS * j + t] = body[t], cs->term_zone[MC_MAX_TERMS * j + t] = zone[t], cs->term_w[MC_MAX_TERMS * j + t] = w[t];
    cs->g0[j] = g0, cs->m[j] = m, cs->slave_body[j] = slave_body, cs->slave_node[j] = slave_node, cs->cdiag[j] = 0;
    for (int d = 0; d < 3; d++) cs->where[3 * j + d] = where[d], cs->normal[3 * j + d] = normal[d];
    return j;
}

/* Cholesky of a dense SPD matrix in place (lower triangle); false if a pivot is not positive */
static bool chol(double *A, int n) {
    for (int j = 0; j < n; j++) {
        double s = A[(size_t)j * n + j];
        for (int k = 0; k < j; k++) s -= A[(size_t)j * n + k] * A[(size_t)j * n + k];
        if (!(s > 0)) return false;
        double d = sqrt(s);
        A[(size_t)j * n + j] = d;
        for (int i = j + 1; i < n; i++) {
            double v = A[(size_t)i * n + j];
            for (int k = 0; k < j; k++) v -= A[(size_t)i * n + k] * A[(size_t)j * n + k];
            A[(size_t)i * n + j] = v / d;
        }
    }
    return true;
}

static void chol_solve_dense(const double *L, int n, double *x) {
    for (int i = 0; i < n; i++) {
        double v = x[i];
        for (int k = 0; k < i; k++) v -= L[(size_t)i * n + k] * x[k];
        x[i] = v / L[(size_t)i * n + i];
    }
    for (int i = n - 1; i >= 0; i--) {
        double v = x[i];
        for (int k = i + 1; k < n; k++) v -= L[(size_t)k * n + i] * x[k];
        x[i] = v / L[(size_t)i * n + i];
    }
}

/* W, full: W_jk = sum over parts of a_j' G_part a_k, plus the contacts' own compliance on the diagonal */
static double *assemble_w(MBody *const *bodies, const MContactSet *cs) {
    int n = cs->n;
    double *W = malloc((size_t)n * n * sizeof(double));
    if (!W) return NULL;
    for (int j = 0; j < n; j++)
        for (int k = j; k < n; k++) {
            double v = 0;
            for (int a = 0; a < cs->nterm[j]; a++) {
                int ba = cs->term_body[MC_MAX_TERMS * j + a], za = cs->term_zone[MC_MAX_TERMS * j + a];
                double wa = cs->term_w[MC_MAX_TERMS * j + a];
                const MBody *B = bodies[ba];
                for (int b = 0; b < cs->nterm[k]; b++) {
                    if (cs->term_body[MC_MAX_TERMS * k + b] != ba) continue;
                    v += wa * cs->term_w[MC_MAX_TERMS * k + b] * mbody_g(B, za, cs->term_zone[MC_MAX_TERMS * k + b]);
                }
            }
            if (k == j) v += cs->cdiag[j];
            W[(size_t)j * n + k] = W[(size_t)k * n + j] = v;
        }
    return W;
}

bool mcontact_solve(MBody *const *bodies, int nbodies, const MContactSet *cs, double torque, double tol, double *lambda, MContactResult *res,
                    char *err, size_t errlen) {
    memset(res, 0, sizeof *res);
    int n = cs->n;
    for (int j = 0; j < n; j++) lambda[j] = 0;
    if (n == 0) return snprintf(err, errlen, "no candidate contacts: the parts do not face each other"), false;
    double *W = assemble_w(bodies, cs);
    if (!W) return snprintf(err, errlen, "out of memory for %d contacts", n), false;
    (void)nbodies;
    /* the first contact to close as the driver turns */
    int first = -1;
    double best = INFINITY, mmax = 0;
    for (int j = 0; j < n; j++) {
        mmax = fmax(mmax, fabs(cs->m[j]));
        if (cs->m[j] < 0) {
            double t = cs->g0[j] / -cs->m[j];
            if (t < best) best = t, first = j;
        }
    }
    if (first < 0) {
        free(W);
        return snprintf(err, errlen, "no contact closes when the driver turns: the torque cannot be carried"), false;
    }
    double ftol = 1e-12 * fabs(torque) / fmax(mmax, 1e-30);
    char *in = calloc((size_t)n, 1);
    int *A = malloc((size_t)n * sizeof(int)), na = 0;
    double *L = malloc((size_t)n * n * sizeof(double)), *x1 = malloc((size_t)n * sizeof(double)), *x2 = malloc((size_t)n * sizeof(double));
    double *g = malloc((size_t)n * sizeof(double));
    if (!in || !A || !L || !x1 || !x2 || !g) {
        free(W), free(in), free(A), free(L), free(x1), free(x2), free(g);
        return snprintf(err, errlen, "out of memory"), false;
    }
    A[na++] = first, in[first] = 1;
    double theta = 0;
    bool ok = false;
    int max_pivots = 50 * n + 100, last_removed = -1;
    for (int it = 0; it < max_pivots; it++) {
        res->pivots = it + 1;
        /* lambda_A = x1 - theta x2 with W_AA x1 = -g0_A, W_AA x2 = m_A; theta from sum m_A lambda_A = -T */
        for (int a = 0; a < na; a++)
            for (int b = 0; b <= a; b++) L[(size_t)a * na + b] = W[(size_t)A[a] * n + A[b]];
        bool spd = chol(L, na);
        if (!spd) { /* redundant contacts: a tiny shift on the diagonal, reported through the residual check below */
            double tr = 0;
            for (int a = 0; a < na; a++) tr += W[(size_t)A[a] * n + A[a]];
            for (int a = 0; a < na; a++)
                for (int b = 0; b <= a; b++) L[(size_t)a * na + b] = W[(size_t)A[a] * n + A[b]] + (a == b ? 1e-10 * tr / na : 0);
            if (!chol(L, na)) {
                snprintf(err, errlen, "the contact compliance of the closed set is singular");
                break;
            }
        }
        for (int a = 0; a < na; a++) x1[a] = -cs->g0[A[a]], x2[a] = cs->m[A[a]];
        chol_solve_dense(L, na, x1);
        chol_solve_dense(L, na, x2);
        double mx1 = 0, mx2 = 0;
        for (int a = 0; a < na; a++) mx1 += cs->m[A[a]] * x1[a], mx2 += cs->m[A[a]] * x2[a];
        theta = (mx1 + torque) / mx2;
        /* a negative force leaves the set */
        int worst = -1;
        double wv = -ftol;
        for (int a = 0; a < na; a++) {
            double l = x1[a] - theta * x2[a];
            lambda[A[a]] = l;
            if (l < wv) wv = l, worst = a;
        }
        if (worst >= 0 && na > 1) {
            last_removed = A[worst];
            in[A[worst]] = 0;
            A[worst] = A[--na];
            continue;
        }
        /* the most penetrated open contact joins it */
        int add = -1;
        double gv = -tol;
        for (int j = 0; j < n; j++) {
            if (in[j]) continue;
            double v = cs->g0[j] + cs->m[j] * theta;
            for (int a = 0; a < na; a++) v += W[(size_t)j * n + A[a]] * lambda[A[a]];
            g[j] = v;
            if (v < gv && j != last_removed) gv = v, add = j;
        }
        if (add < 0) {
            ok = true;
            break;
        }
        last_removed = -1;
        in[add] = 1, A[na++] = add;
    }
    if (ok) {
        double pen = 0, fmin_ = INFINITY, tot = 0;
        for (int j = 0; j < n; j++) {
            if (!in[j]) lambda[j] = 0;
            double v = cs->g0[j] + cs->m[j] * theta;
            for (int a = 0; a < na; a++) v += W[(size_t)j * n + A[a]] * lambda[A[a]];
            pen = fmin(pen, v);
            if (in[j]) fmin_ = fmin(fmin_, lambda[j]);
            tot += lambda[j];
        }
        res->max_penetration = -pen, res->min_force = fmin_, res->total_force = tot;
        res->converged = pen > -10 * tol && fmin_ > -10 * ftol;
        res->active = na, res->theta = theta;
        if (!res->converged) snprintf(err, errlen, "the contact solution left a penetration of %.3g m or a force of %.3g N", -pen, fmin_);
    } else if (!err[0]) {
        snprintf(err, errlen, "the contact set did not settle in %d pivots", max_pivots);
    }
    free(W), free(in), free(A), free(L), free(x1), free(x2), free(g);
    return ok && res->converged;
}

void mcontact_forces(MBody *const *bodies, int nbodies, const MContactSet *cs, const double *lambda, double **f) {
    for (int b = 0; b < nbodies; b++) memset(f[b], 0, (size_t)bodies[b]->nnodes * 3 * sizeof(double));
    for (int j = 0; j < cs->n; j++) {
        if (lambda[j] == 0) continue;
        for (int t = 0; t < cs->nterm[j]; t++) {
            int b = cs->term_body[MC_MAX_TERMS * j + t], z = cs->term_zone[MC_MAX_TERMS * j + t];
            double w = cs->term_w[MC_MAX_TERMS * j + t];
            const MBody *B = bodies[b];
            int nd = B->zone_node[z];
            for (int d = 0; d < 3; d++) f[b][3 * nd + d] += lambda[j] * w * B->zone_dir[3 * z + d];
        }
    }
}

int mcontact_add_q(MContactSet *cs, double g0, const double *mrow, int nterm, const int *body, const int *zone, const double *w, int slave_body,
                   int slave_node, const double where[3], const double normal[3]) {
    if (cs->ndof < 1 || cs->ndof > MC_MAX_DOF) return -1;
    int j = mcontact_add(cs, g0, mrow[0], nterm, body, zone, w, slave_body, slave_node, where, normal);
    if (j < 0) return -1;
    for (int d = 0; d < MC_MAX_DOF; d++) cs->mq[MC_MAX_DOF * j + d] = d < cs->ndof ? mrow[d] : 0;
    return j;
}

/* dense Cholesky solve of an SPD system of order n with k right-hand sides, columns of X (n x k, column-major) */
static void chol_solve_cols(const double *L, int n, double *X, int k) {
    for (int c = 0; c < k; c++) chol_solve_dense(L, n, X + (size_t)c * n);
}

bool mcontact_solve_q(MBody *const *bodies, int nbodies, const MContactSet *cs, const double *force, const double *ksoft, double tol,
                      double *lambda, double *q, MContactResult *res, char *err, size_t errlen) {
    memset(res, 0, sizeof *res);
    int n = cs->n, nd = cs->ndof;
    for (int j = 0; j < n; j++) lambda[j] = 0;
    for (int d = 0; d < nd; d++) q[d] = 0;
    if (nd < 1 || nd > MC_MAX_DOF) return snprintf(err, errlen, "contact: 1 to %d rigid degrees of freedom", MC_MAX_DOF), false;
    double *W = n ? assemble_w(bodies, cs) : NULL;
    if (n && !W) return snprintf(err, errlen, "out of memory for %d contacts", n), false;
    (void)nbodies;
    double fmax_ = 0, mmax = 1e-30;
    for (int d = 0; d < nd; d++)
        if (ksoft[d] >= 0) fmax_ = fmax(fmax_, fabs(force[d]));
    for (int j = 0; j < n; j++)
        for (int d = 0; d < nd; d++) mmax = fmax(mmax, fabs(cs->mq[MC_MAX_DOF * j + d]));
    double ftol = 1e-12 * fmax_ / mmax;
    char *in = calloc((size_t)(n ? n : 1), 1);
    int *A = malloc((size_t)(n ? n : 1) * sizeof(int)), na = 0;
    double *L = malloc((size_t)(n ? n : 1) * (n ? n : 1) * sizeof(double)), *X = malloc((size_t)(n ? n : 1) * (nd + 1) * sizeof(double));
    double S[MC_MAX_DOF * MC_MAX_DOF], b[MC_MAX_DOF];
    if (!in || !A || !L || !X) {
        free(W), free(in), free(A), free(L), free(X);
        return snprintf(err, errlen, "out of memory"), false;
    }
    bool ok = false;
    int max_pivots = 50 * n + 100, last_removed = -1;
    for (int it = 0; it < max_pivots; it++) {
        res->pivots = it + 1;
        /* X = W_AA^-1 [-g0_A | M_A] */
        if (na) {
            for (int a = 0; a < na; a++)
                for (int c = 0; c <= a; c++) L[(size_t)a * na + c] = W[(size_t)A[a] * n + A[c]];
            if (!chol(L, na)) {
                double tr = 0;
                for (int a = 0; a < na; a++) tr += W[(size_t)A[a] * n + A[a]];
                for (int a = 0; a < na; a++)
                    for (int c = 0; c <= a; c++) L[(size_t)a * na + c] = W[(size_t)A[a] * n + A[c]] + (a == c ? 1e-10 * tr / na : 0);
                if (!chol(L, na)) {
                    snprintf(err, errlen, "the contact compliance of the closed set is singular");
                    break;
                }
            }
            for (int a = 0; a < na; a++) {
                X[a] = -cs->g0[A[a]];
                for (int d = 0; d < nd; d++) X[(size_t)(d + 1) * na + a] = cs->mq[MC_MAX_DOF * A[a] + d];
            }
            chol_solve_cols(L, na, X, nd + 1);
        }
        /* S q = b: S = M_A' W^-1 M_A + K, b = M_A' x1 + f; a held degree of freedom's row and column are the identity's */
        for (int d = 0; d < nd; d++) {
            b[d] = ksoft[d] < 0 ? 0 : force[d];
            for (int e = 0; e < nd; e++) S[d * nd + e] = d == e ? (ksoft[d] < 0 ? 1 : ksoft[d]) : 0;
            if (ksoft[d] < 0) continue;
            for (int a = 0; a < na; a++) {
                double mad = cs->mq[MC_MAX_DOF * A[a] + d];
                b[d] += mad * X[a];
                for (int e = 0; e < nd; e++)
                    if (ksoft[e] >= 0) S[d * nd + e] += mad * X[(size_t)(e + 1) * na + a];
            }
        }
        for (int d = 0; d < nd; d++) /* symmetric up to round-off */
            for (int e = d + 1; e < nd; e++) S[d * nd + e] = S[e * nd + d] = 0.5 * (S[d * nd + e] + S[e * nd + d]);
        if (!chol(S, nd)) {
            snprintf(err, errlen, "the rigid degrees of freedom are not held (a soft spring of zero?)");
            break;
        }
        for (int d = 0; d < nd; d++) q[d] = b[d];
        chol_solve_dense(S, nd, q);
        /* lambda_A = x1 - X2 q; a negative force leaves the set */
        int worst = -1;
        double wv = -ftol;
        for (int a = 0; a < na; a++) {
            double l = X[a];
            for (int d = 0; d < nd; d++) l -= X[(size_t)(d + 1) * na + a] * q[d];
            lambda[A[a]] = l;
            if (l < wv) wv = l, worst = a;
        }
        /* The greedy rules below can cycle on a degenerate set (contacts nearly dependent). Past 4 n + 100 pivots the
         * least index rule takes over: the first contact, by index, whose force is negative leaves or whose gap is
         * negative joins. With the rigid degrees of freedom eliminated the problem's matrix, W + M K^-1 M', is positive
         * definite, and for such matrices this rule (Murty's) cannot cycle. */
        if (it > 4 * n + 100) {
            int flip = -1;
            for (int j = 0; j < n && flip < 0; j++) {
                if (in[j]) {
                    if (lambda[j] < -ftol) flip = j;
                    continue;
                }
                double v = cs->g0[j];
                for (int d = 0; d < nd; d++) v += cs->mq[MC_MAX_DOF * j + d] * q[d];
                for (int a = 0; a < na; a++) v += W[(size_t)j * n + A[a]] * lambda[A[a]];
                if (v < -tol) flip = j;
            }
            if (flip < 0) {
                ok = true;
                break;
            }
            if (in[flip]) {
                int a = 0;
                while (A[a] != flip) a++;
                in[flip] = 0, lambda[flip] = 0, A[a] = A[--na];
            } else {
                in[flip] = 1, A[na++] = flip;
            }
            continue;
        }
        if (worst >= 0) {
            last_removed = A[worst];
            in[A[worst]] = 0, lambda[A[worst]] = 0;
            A[worst] = A[--na];
            continue;
        }
        /* the most penetrated open contact joins it */
        int add = -1;
        double gv = -tol;
        for (int j = 0; j < n; j++) {
            if (in[j]) continue;
            double v = cs->g0[j];
            for (int d = 0; d < nd; d++) v += cs->mq[MC_MAX_DOF * j + d] * q[d];
            for (int a = 0; a < na; a++) v += W[(size_t)j * n + A[a]] * lambda[A[a]];
            if (v < gv && j != last_removed) gv = v, add = j;
        }
        if (add < 0) {
            ok = true;
            break;
        }
        last_removed = -1;
        in[add] = 1, A[na++] = add;
    }
    if (ok) {
        double pen = 0, fmin_ = INFINITY, tot = 0;
        for (int j = 0; j < n; j++) {
            if (!in[j]) lambda[j] = 0;
            double v = cs->g0[j];
            for (int d = 0; d < nd; d++) v += cs->mq[MC_MAX_DOF * j + d] * q[d];
            for (int a = 0; a < na; a++) v += W[(size_t)j * n + A[a]] * lambda[A[a]];
            pen = fmin(pen, v);
            if (in[j]) fmin_ = fmin(fmin_, lambda[j]);
            tot += lambda[j];
        }
        res->max_penetration = -pen, res->min_force = na ? fmin_ : 0, res->total_force = tot;
        res->converged = pen > -10 * tol && (na == 0 || fmin_ > -10 * ftol);
        res->active = na, res->theta = q[0];
        if (!res->converged) snprintf(err, errlen, "the contact solution left a penetration of %.3g m or a force of %.3g N", -pen, fmin_);
    } else if (!err[0]) {
        snprintf(err, errlen, "the contact set did not settle in %d pivots", max_pivots);
    }
    free(W), free(in), free(A), free(L), free(X);
    return ok && res->converged;
}
