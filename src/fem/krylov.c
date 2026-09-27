/* krylov.c - BiCGSTAB with ILU(0) or Jacobi preconditioning for nonsymmetric sparse systems (see krylov.h) */
#include "krylov.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static double dot(const double *a, const double *b, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

/* ---- ILU(0) ------------------------------------------------------------------------------------------------------ */

void ilu0_free(Ilu0 *F) {
    free(F->lu), free(F->diag);
    memset(F, 0, sizeof *F);
}

bool ilu0_factor(const CsrMatrix *A, Ilu0 *F, char *err, size_t errlen) {
    memset(F, 0, sizeof *F);
    int n = A->n;
    F->n = n, F->A = A;
    F->lu = malloc((size_t)(A->nnz > 0 ? A->nnz : 1) * sizeof(double));
    F->diag = malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    int64_t *pos = malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    if (!F->lu || !F->diag || !pos) {
        free(pos), ilu0_free(F);
        snprintf(err, errlen, "out of memory for the ILU(0) factor");
        return false;
    }
    memcpy(F->lu, A->val, (size_t)A->nnz * sizeof(double));
    for (int i = 0; i < n; i++) {
        F->diag[i] = -1;
        pos[i] = -1;
    }
    for (int i = 0; i < n; i++)
        for (int64_t k = A->rowptr[i]; k < A->rowptr[i + 1]; k++)
            if (A->col[k] == i) F->diag[i] = k;
    for (int i = 0; i < n; i++) {
        if (F->diag[i] < 0) {
            free(pos), ilu0_free(F);
            snprintf(err, errlen, "ILU(0): row %d has no diagonal entry in the pattern", i);
            return false;
        }
        for (int64_t k = A->rowptr[i]; k < A->rowptr[i + 1]; k++) pos[A->col[k]] = k;
        /* eliminate with every earlier row k whose column appears in row i (columns are sorted) */
        for (int64_t kk = A->rowptr[i]; kk < A->rowptr[i + 1] && A->col[kk] < i; kk++) {
            int k = A->col[kk];
            double pivot = F->lu[F->diag[k]];
            if (pivot == 0 || !isfinite(pivot)) {
                for (int64_t q = A->rowptr[i]; q < A->rowptr[i + 1]; q++) pos[A->col[q]] = -1;
                free(pos), ilu0_free(F);
                snprintf(err, errlen, "ILU(0): zero or non-finite pivot in row %d", k);
                return false;
            }
            double lik = F->lu[kk] / pivot;
            F->lu[kk] = lik;
            for (int64_t kj = F->diag[k] + 1; kj < A->rowptr[k + 1]; kj++) {
                int64_t ij = pos[A->col[kj]];
                if (ij >= 0) F->lu[ij] -= lik * F->lu[kj]; /* fill outside the pattern is dropped: level 0 */
            }
        }
        for (int64_t k = A->rowptr[i]; k < A->rowptr[i + 1]; k++) pos[A->col[k]] = -1;
        if (F->lu[F->diag[i]] == 0 || !isfinite(F->lu[F->diag[i]])) {
            free(pos), ilu0_free(F);
            snprintf(err, errlen, "ILU(0): zero or non-finite pivot in row %d", i);
            return false;
        }
    }
    free(pos);
    return true;
}

void ilu0_apply(const Ilu0 *F, const double *r, double *z) {
    const CsrMatrix *A = F->A;
    int n = F->n;
    for (int i = 0; i < n; i++) { /* L y = r, unit diagonal */
        double s = r[i];
        for (int64_t k = A->rowptr[i]; k < F->diag[i]; k++) s -= F->lu[k] * z[A->col[k]];
        z[i] = s;
    }
    for (int i = n - 1; i >= 0; i--) { /* U z = y */
        double s = z[i];
        for (int64_t k = F->diag[i] + 1; k < A->rowptr[i + 1]; k++) s -= F->lu[k] * z[A->col[k]];
        z[i] = s / F->lu[F->diag[i]];
    }
}

/* ---- BiCGSTAB ---------------------------------------------------------------------------------------------------- */

typedef struct {
    KrylovPrecond kind;
    Ilu0 ilu;
    double *inv_diag;
} Prec;

static bool prec_build(const CsrMatrix *A, KrylovPrecond kind, Prec *P, char *err, size_t errlen) {
    memset(P, 0, sizeof *P);
    P->kind = kind;
    if (kind == KRYLOV_PRECOND_ILU0) return ilu0_factor(A, &P->ilu, err, errlen);
    P->inv_diag = malloc((size_t)(A->n > 0 ? A->n : 1) * sizeof(double));
    if (!P->inv_diag) {
        snprintf(err, errlen, "out of memory for the Jacobi preconditioner");
        return false;
    }
    for (int i = 0; i < A->n; i++) {
        double d = 0;
        for (int64_t k = A->rowptr[i]; k < A->rowptr[i + 1]; k++)
            if (A->col[k] == i) d = A->val[k];
        if (d == 0 || !isfinite(d)) {
            free(P->inv_diag);
            P->inv_diag = NULL;
            snprintf(err, errlen, "Jacobi preconditioner: zero or non-finite diagonal in row %d", i);
            return false;
        }
        P->inv_diag[i] = 1.0 / d;
    }
    return true;
}

static void prec_apply(const Prec *P, int n, const double *r, double *z) {
    if (P->kind == KRYLOV_PRECOND_ILU0) ilu0_apply(&P->ilu, r, z);
    else
        for (int i = 0; i < n; i++) z[i] = P->inv_diag[i] * r[i];
}

static void prec_free(Prec *P) {
    if (P->kind == KRYLOV_PRECOND_ILU0) ilu0_free(&P->ilu);
    free(P->inv_diag);
}

bool bicgstab_solve(const CsrMatrix *A, const double *b, double *x, const KrylovOptions *o, SolveStats *st, char *err, size_t errlen) {
    memset(st, 0, sizeof *st);
    double t0 = now_s();
    int n = A->n;
    KrylovOptions opt = *o;
    if (opt.max_iter <= 0) opt.max_iter = 5000;
    if (opt.max_restarts <= 0) opt.max_restarts = 10;
    snprintf(st->method, sizeof st->method, "bicgstab/%s", opt.precond == KRYLOV_PRECOND_ILU0 ? "ilu0" : "jacobi");
    double bnorm = sqrt(dot(b, b, n));
    if (bnorm == 0) {
        memset(x, 0, (size_t)n * sizeof(double));
        st->converged = true;
        st->seconds = now_s() - t0;
        return true;
    }
    Prec P;
    if (!prec_build(A, opt.precond, &P, err, errlen)) {
        st->seconds = now_s() - t0;
        return false;
    }
    size_t bytes = (size_t)(n > 0 ? n : 1) * sizeof(double);
    double *r = malloc(bytes), *rh = malloc(bytes), *p = calloc(1, bytes), *v = calloc(1, bytes), *ph = malloc(bytes), *s = malloc(bytes),
           *sh = malloc(bytes), *t = malloc(bytes);
    if (!r || !rh || !p || !v || !ph || !s || !sh || !t) {
        free(r), free(rh), free(p), free(v), free(ph), free(s), free(sh), free(t);
        prec_free(&P);
        snprintf(err, errlen, "out of memory for BiCGSTAB vectors");
        return false;
    }
    double anorm = 0, binf = 0; /* infinity norms for the backward error */
    for (int i = 0; i < n; i++) {
        double row = 0;
        for (int64_t k = A->rowptr[i]; k < A->rowptr[i + 1]; k++) row += fabs(A->val[k]);
        anorm = fmax(anorm, row), binf = fmax(binf, fabs(b[i]));
    }
    double rho_prev = 1, alpha = 1, omega = 1, rel = 1, true_rel = INFINITY;
    int it = 0, restarts = 0, replacements = 0;
    bool ok = true;
    const char *why = NULL;
    /* residual replacement (van der Vorst and Ye 2000): the recursively updated residual drifts from b - A x by rounding
     * errors proportional to the largest intermediate iterate, which on strongly nonsymmetric (advection-dominated)
     * systems can be many orders above the tolerance. When the recursive residual has converged but the true one has
     * not, the iteration restarts from the current iterate with the true residual. */
cycle:
    csr_spmv(A, x, t, opt.pool);
    for (int i = 0; i < n; i++) r[i] = b[i] - t[i];
    memcpy(rh, r, bytes);
    memset(p, 0, bytes), memset(v, 0, bytes);
    rho_prev = alpha = omega = 1;
    rel = sqrt(dot(r, r, n)) / bnorm;
    while (rel > opt.tol && it < opt.max_iter) {
        double rho = dot(rh, r, n);
        double rn = sqrt(dot(r, r, n)), rhn = sqrt(dot(rh, rh, n));
        if (fabs(rho) <= 1e-30 * rn * rhn || !isfinite(rho)) {
            /* the shadow residual became orthogonal to the residual: restart from the current iterate */
            if (++restarts > opt.max_restarts || !isfinite(rho)) {
                why = "the shadow residual stayed orthogonal to the residual after the permitted restarts";
                ok = false;
                break;
            }
            memcpy(rh, r, bytes);
            memset(p, 0, bytes), memset(v, 0, bytes);
            rho_prev = alpha = omega = 1;
            continue;
        }
        double beta = (rho / rho_prev) * (alpha / omega);
        for (int i = 0; i < n; i++) p[i] = r[i] + beta * (p[i] - omega * v[i]);
        prec_apply(&P, n, p, ph);
        csr_spmv(A, ph, v, opt.pool);
        double rhv = dot(rh, v, n);
        if (rhv == 0 || !isfinite(rhv)) {
            if (++restarts > opt.max_restarts) {
                why = "breakdown: the shadow residual is orthogonal to A p";
                ok = false;
                break;
            }
            memcpy(rh, r, bytes);
            memset(p, 0, bytes), memset(v, 0, bytes);
            rho_prev = alpha = omega = 1;
            continue;
        }
        alpha = rho / rhv;
        for (int i = 0; i < n; i++) s[i] = r[i] - alpha * v[i];
        it++;
        double snorm = sqrt(dot(s, s, n)) / bnorm;
        if (snorm <= opt.tol) {
            for (int i = 0; i < n; i++) x[i] += alpha * ph[i];
            memcpy(r, s, bytes);
            rel = snorm;
            break;
        }
        prec_apply(&P, n, s, sh);
        csr_spmv(A, sh, t, opt.pool);
        double tt = dot(t, t, n);
        if (!(tt > 0) || !isfinite(tt)) {
            why = "breakdown: A M^-1 s vanished";
            ok = false;
            break;
        }
        omega = dot(t, s, n) / tt;
        for (int i = 0; i < n; i++) {
            x[i] += alpha * ph[i] + omega * sh[i];
            r[i] = s[i] - omega * t[i];
        }
        rel = sqrt(dot(r, r, n)) / bnorm;
        if (!isfinite(rel)) {
            why = "the residual became non-finite";
            ok = false;
            break;
        }
        if (omega == 0) {
            if (rel <= opt.tol) break;
            why = "breakdown: the stabilisation factor omega vanished";
            ok = false;
            break;
        }
        rho_prev = rho;
    }
    csr_spmv(A, x, t, opt.pool);
    double rr = 0, rinf = 0, xinf = 0;
    for (int i = 0; i < n; i++) {
        double ri = b[i] - t[i];
        rr += ri * ri;
        rinf = fmax(rinf, fabs(ri)), xinf = fmax(xinf, fabs(x[i]));
    }
    true_rel = sqrt(rr) / bnorm;
    /* the rounding floor: when the backward error |r| / (|A| |x| + |b|) (infinity norms) is at machine precision, x solves a
     * problem perturbed only by rounding and no iteration can improve it; a relative tolerance below that floor (large,
     * strongly nonsymmetric systems) is then met as far as floating point allows */
    double backward = rinf / (anorm * xinf + binf);
    bool at_floor = ok && isfinite(backward) && backward <= 64 * DBL_EPSILON;
    if (ok && rel <= opt.tol && true_rel > opt.tol && !at_floor && isfinite(true_rel) && it < opt.max_iter && replacements < opt.max_restarts) {
        replacements++;
        goto cycle;
    }
    st->iterations = it;
    st->rel_residual = rel;
    st->true_residual = true_rel;
    /* the true residual decides, down to the rounding floor */
    st->converged = ok && isfinite(true_rel) && (true_rel <= opt.tol || at_floor);
    if (!st->converged) {
        if (why) snprintf(err, errlen, "BiCGSTAB failed after %d iterations (%d restarts, %d residual replacements): %s", it, restarts, replacements, why);
        else
            snprintf(err, errlen,
                     "BiCGSTAB did not converge in %d iterations (%d restarts, %d residual replacements): relative residual %.3g, true residual %.3g, "
                     "target %.3g; backward error %.3g against the rounding floor %.3g",
                     it, restarts, replacements, rel, true_rel, opt.tol, backward, 64 * DBL_EPSILON);
    }
    free(r), free(rh), free(p), free(v), free(ph), free(s), free(sh), free(t);
    prec_free(&P);
    st->seconds = now_s() - t0;
    return st->converged;
}
