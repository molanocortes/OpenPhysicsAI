/* modal.c - consistent mass, subspace iteration, modal and Newmark transient response, frequency response (see modal.h) */
#include "modal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmath.h"

void hex8_consistent_mass(const double X[8][3], double rho, double Me[576]) {
    memset(Me, 0, 576 * sizeof(double));
    const double g = 1 / sqrt(3.0);
    for (int gp = 0; gp < 8; gp++) {
        double N[8], dN[8][3], J[3][3], dNdx[8][3];
        hex8_shape(HEX8_XI[gp][0] * g, HEX8_XI[gp][1] * g, HEX8_XI[gp][2] * g, N, dN);
        double w = rho * hex8_jacobian(X, dN, J, dNdx);
        for (int a = 0; a < 8; a++)
            for (int b = 0; b < 8; b++) {
                double v = w * N[a] * N[b];
                for (int k = 0; k < 3; k++) Me[24 * (3 * a + k) + 3 * b + k] += v;
            }
    }
}

static double dotv(const double *a, const double *b, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

void modal_result_free(ModalResult *r) {
    free(r->eq), free(r->omega2), free(r->phi), free(r->residual), free(r->eff);
    csr_free(&r->K), csr_free(&r->M);
    if (r->have_Kf) chol_free(&r->Kf);
    memset(r, 0, sizeof *r);
}

/* assembles K and M over the equations of the free components */
bool modal_assemble(const StructDyn *s, ModalResult *r, char *err, size_t errlen) {
    const OrthoModel *m = s->om;
    int nn = m->nnodes, ne = m->nelems;
    unsigned char *used = calloc((size_t)(nn ? nn : 1), 1);
    int *dofs = malloc((size_t)(ne ? ne : 1) * 24 * sizeof(int));
    r->eq = malloc(3 * (size_t)(nn ? nn : 1) * sizeof(int));
    if (!used || !dofs || !r->eq) {
        free(used), free(dofs);
        snprintf(err, errlen, "out of memory assembling %d elements", ne);
        return false;
    }
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 8; a++) used[m->conn[8 * (size_t)e + a]] = 1;
    int neq = 0;
    for (int n = 0; n < nn; n++)
        for (int k = 0; k < 3; k++) r->eq[3 * n + k] = used[n] && !(s->fixed && s->fixed[3 * (size_t)n + (size_t)k]) ? neq++ : -1;
    r->neq = neq, r->nnodes = nn;
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) dofs[24 * (size_t)e + 3 * (size_t)a + (size_t)k] = r->eq[3 * m->conn[8 * (size_t)e + a] + k];
    bool ok = csr_from_elements(&r->K, neq, ne, 24, dofs, err, errlen) && csr_from_elements(&r->M, neq, ne, 24, dofs, err, errlen);
    for (int e = 0; ok && e < ne; e++) {
        double X[8][3], D[6][6], Ke[576], Me[576], mdj;
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) X[a][k] = m->xyz[3 * (size_t)m->conn[8 * (size_t)e + a] + (size_t)k];
        ortho_element_D(m, e, D);
        if (!hex8_stiffness(X, D, m->formulation, Ke, &mdj)) {
            snprintf(err, errlen, "element %d is inverted or degenerate (Jacobian determinant %.3g)", e, mdj);
            ok = false;
            break;
        }
        if (!(s->density[e] > 0)) {
            snprintf(err, errlen, "element %d has no positive density", e);
            ok = false;
            break;
        }
        hex8_consistent_mass(X, s->density[e], Me);
        csr_add_element(&r->K, 24, dofs + 24 * (size_t)e, Ke);
        csr_add_element(&r->M, 24, dofs + 24 * (size_t)e, Me);
    }
    free(used), free(dofs);
    return ok;
}


bool modal_solve(const StructDyn *s, const ModalOptions *optp, ModalResult *r, char *err, size_t errlen) {
    memset(r, 0, sizeof *r);
    ModalOptions opt = *optp;
    if (opt.tol <= 0) opt.tol = 1e-10;
    if (opt.max_iter <= 0) opt.max_iter = 300;
    const OrthoModel *m = s->om;
    for (int i = 0; i < m->nmat; i++) {
        char why[300];
        double C[6][6];
        if (!ortho_stiffness(&m->mat[i], C, why, sizeof why)) {
            snprintf(err, errlen, "material %d: %s", i, why);
            return false;
        }
    }
    if (!modal_assemble(s, r, err, errlen)) {
        modal_result_free(r);
        return false;
    }
    int n = r->neq, p = opt.nmodes;
    if (p < 1 || p > n) {
        snprintf(err, errlen, "modes wanted (%d) must be between 1 and the number of free equations (%d)", p, n);
        modal_result_free(r);
        return false;
    }
    int q = opt.subspace > 0 ? opt.subspace : (2 * p > p + 8 ? 2 * p : p + 8);
    if (q > n) q = n;
    if (q < p) q = p;
    bool supported = false;
    for (int i = 0; s->fixed && i < 3 * m->nnodes && !supported; i++) supported = s->fixed[i] != 0;
    double trK = 0, trM = 0;
    for (int i = 0; i < n; i++) {
        int64_t k = csr_find(&r->K, i, i), l = csr_find(&r->M, i, i);
        if (k >= 0) trK += r->K.val[k];
        if (l >= 0) trM += r->M.val[l];
    }
    double lam_avg = trM > 0 ? trK / trM : 1;
    /* free-free: a moderate negative shift keeps K - sigma M well conditioned; the rigid-body directions it amplifies are
     * separated from the elastic ones by M-orthonormalisation of the iteration vectors */
    r->shift = supported ? 0 : -1e-4 * lam_avg;
    /* shifted matrix with the pattern of K */
    CsrMatrix A = {0};
    bool ok = false;
    double *X = NULL, *Y = NULL, *MX = NULL, *MY = NULL, *Kr = NULL, *Mr = NULL, *Qm = NULL, *w = NULL, *lam = NULL, *lam_old = NULL, *V = NULL, *tmp = NULL;
    int *order = NULL;
    CholFactor F = {0};
    bool haveF = false;
    A.n = r->K.n, A.nnz = r->K.nnz;
    A.rowptr = malloc(((size_t)n + 1) * sizeof(int64_t));
    A.col = malloc((size_t)(A.nnz ? A.nnz : 1) * sizeof(int));
    A.val = malloc((size_t)(A.nnz ? A.nnz : 1) * sizeof(double));
    if (!A.rowptr || !A.col || !A.val) goto oom;
    memcpy(A.rowptr, r->K.rowptr, ((size_t)n + 1) * sizeof(int64_t));
    memcpy(A.col, r->K.col, (size_t)A.nnz * sizeof(int));
    for (int64_t k = 0; k < A.nnz; k++) A.val[k] = r->K.val[k] - r->shift * r->M.val[k];
    bool too_big = false;
    if (!ortho_factor(m->nnodes, m->xyz, &A, r->eq, n, 0, NULL, NULL, &F, &r->factor, &too_big, err, errlen)) {
        if (supported && !too_big) {
            size_t l = strlen(err);
            snprintf(err + l, errlen - l, "; the supports may leave rigid-body motion free");
        }
        goto fail;
    }
    haveF = true;
    size_t N = (size_t)n, Q = (size_t)q;
    X = calloc(Q * N, sizeof(double)), Y = calloc(Q * N, sizeof(double)), MX = calloc(Q * N, sizeof(double)), MY = calloc(Q * N, sizeof(double));
    Kr = malloc(Q * Q * sizeof(double)), Mr = malloc(Q * Q * sizeof(double)), Qm = malloc(Q * Q * sizeof(double)), w = malloc(Q * sizeof(double));
    lam = malloc(Q * sizeof(double)), lam_old = malloc(Q * sizeof(double)), V = malloc(Q * Q * sizeof(double)), tmp = malloc(N * sizeof(double));
    order = malloc(Q * sizeof(int));
    if (!X || !Y || !MX || !MY || !Kr || !Mr || !Qm || !w || !lam || !lam_old || !V || !tmp || !order) goto oom;
    /* starting vectors: the mass diagonal, unit vectors at the largest M_ii / K_ii, and deterministic pseudo-random vectors */
    {
        double *ratio = malloc(N * sizeof(double));
        char *taken = calloc(N, 1);
        if (!ratio || !taken) {
            free(ratio), free(taken);
            goto oom;
        }
        for (int i = 0; i < n; i++) {
            int64_t k = csr_find(&r->K, i, i), l = csr_find(&r->M, i, i);
            double kk = k >= 0 ? r->K.val[k] : 0, mm = l >= 0 ? r->M.val[l] : 0;
            X[i] = mm;
            ratio[i] = kk > 0 ? mm / kk : 0;
        }
        int col = 1;
        for (; col < q - 1; col++) {
            int best = -1;
            for (int i = 0; i < n; i++)
                if (!taken[i] && (best < 0 || ratio[i] > ratio[best])) best = i;
            if (best < 0) break;
            taken[best] = 1;
            X[(size_t)col * N + (size_t)best] = 1;
        }
        uint64_t st = 0x9E3779B97F4A7C15ull;
        for (; col < q; col++)
            for (int i = 0; i < n; i++) {
                st ^= st << 13, st ^= st >> 7, st ^= st << 17;
                X[(size_t)col * N + (size_t)i] = (double)(st >> 11) / 9007199254740992.0 - 0.5;
            }
        free(ratio), free(taken);
    }
    for (int j = 0; j < q; j++) lam_old[j] = NAN;
    uint64_t rng_state = 0xD1B54A32D192ED03ull;
    for (int it = 1; it <= opt.max_iter; it++) {
        for (int j = 0; j < q; j++) {
            double *xj = X + (size_t)j * N, *mxj = MX + (size_t)j * N, *yj = Y + (size_t)j * N, *myj = MY + (size_t)j * N;
            csr_spmv(&r->M, xj, mxj, NULL);
            chol_solve(&F, mxj, yj);
            csr_spmv(&r->M, yj, myj, NULL);
            /* modified Gram-Schmidt in the M inner product, twice; a column that collapses onto the previous ones (rigid-body
             * directions amplified by the shift) is replaced by a fresh pseudo-random vector */
            /* the same column operations keep (K - sigma M) y = M x in MX without multiplying by K (whose large norm would amplify
             * round-off in the high-frequency components into the low Ritz values) */
            for (int attempt = 0; attempt < 4; attempt++) {
                double before = sqrt(fmax(dotv(yj, myj, n), 0));
                for (int pass = 0; pass < 2; pass++)
                    for (int i = 0; i < j; i++) {
                        const double *yi = Y + (size_t)i * N, *myi = MY + (size_t)i * N, *ayi = MX + (size_t)i * N;
                        double c = dotv(yi, myj, n);
                        for (int k = 0; k < n; k++) yj[k] -= c * yi[k], myj[k] -= c * myi[k], mxj[k] -= c * ayi[k];
                    }
                double nm = sqrt(fmax(dotv(yj, myj, n), 0));
                if (nm > 1e-8 * before && nm > 0) {
                    for (int k = 0; k < n; k++) yj[k] /= nm, myj[k] /= nm, mxj[k] /= nm;
                    break;
                }
                for (int k = 0; k < n; k++) {
                    rng_state ^= rng_state << 13, rng_state ^= rng_state >> 7, rng_state ^= rng_state << 17;
                    yj[k] = (double)(rng_state >> 11) / 9007199254740992.0 - 0.5;
                }
                csr_spmv(&r->M, yj, myj, NULL);
                csr_spmv(&r->K, yj, mxj, NULL);
                for (int k = 0; k < n; k++) mxj[k] -= r->shift * myj[k];
            }
        }
        for (int i = 0; i < q; i++)
            for (int j = 0; j <= i; j++) {
                double kr = 0.5 * (dotv(Y + (size_t)i * N, MX + (size_t)j * N, n) + dotv(Y + (size_t)j * N, MX + (size_t)i * N, n));
                double mr = 0.5 * (dotv(Y + (size_t)i * N, MY + (size_t)j * N, n) + dotv(Y + (size_t)j * N, MY + (size_t)i * N, n));
                Kr[(size_t)i * Q + (size_t)j] = Kr[(size_t)j * Q + (size_t)i] = kr;
                Mr[(size_t)i * Q + (size_t)j] = Mr[(size_t)j * Q + (size_t)i] = mr;
            }
        /* Kr z = mu Mr z: Mr = L L^T, C = L^-1 Kr L^-T */
        if (!mm_chol_factor(Mr, q, 1e-14)) {
            snprintf(err, errlen, "the iteration vectors became linearly dependent (subspace %d larger than the independent modes)", q);
            goto fail;
        }
        for (int j = 0; j < q; j++) { /* columns of L^-1 Kr */
            for (int i = 0; i < q; i++) tmp[i] = Kr[(size_t)i * Q + (size_t)j];
            mm_chol_lower(Mr, q, tmp);
            for (int i = 0; i < q; i++) Qm[(size_t)i * Q + (size_t)j] = tmp[i];
        }
        for (int i = 0; i < q; i++) { /* rows: (L^-1 Kr) L^-T = (L^-1 (L^-1 Kr)^T)^T */
            for (int j = 0; j < q; j++) tmp[j] = Qm[(size_t)i * Q + (size_t)j];
            mm_chol_lower(Mr, q, tmp);
            for (int j = 0; j < q; j++) Kr[(size_t)i * Q + (size_t)j] = tmp[j];
        }
        for (int i = 0; i < q; i++)
            for (int j = 0; j < i; j++) Kr[(size_t)i * Q + (size_t)j] = Kr[(size_t)j * Q + (size_t)i] = 0.5 * (Kr[(size_t)i * Q + (size_t)j] + Kr[(size_t)j * Q + (size_t)i]);
        if (mm_sym_eig(Kr, q, w, V) < 0) {
            snprintf(err, errlen, "the reduced eigenproblem did not converge");
            goto fail;
        }
        /* ascending order; z_k = L^-T v_k */
        for (int k = 0; k < q; k++) order[k] = q - 1 - k; /* mm_sym_eig sorts descending */
        memset(X, 0, Q * N * sizeof(double));
        for (int k = 0; k < q; k++) {
            int src = order[k];
            for (int i = 0; i < q; i++) tmp[i] = V[(size_t)i * Q + (size_t)src];
            mm_chol_upper(Mr, q, tmp);
            lam[k] = w[src] + r->shift;
            for (int j = 0; j < q; j++) {
                if (tmp[j] == 0) continue;
                double *xk = X + (size_t)k * N;
                const double *yj = Y + (size_t)j * N;
                for (int i = 0; i < n; i++) xk[i] += tmp[j] * yj[i];
            }
        }
        double scale = fabs(lam[p - 1]) > 0 ? fabs(lam[p - 1]) : 1;
        bool conv = true;
        /* relative change, with a floor at the shift: eigenvalues near zero (rigid-body modes) are only resolved to round-off
         * of the shifted problem */
        double floor_abs = fmax(fabs(r->shift), 1e-6 * scale);
        for (int k = 0; k < p; k++) conv &= isfinite(lam_old[k]) && fabs(lam[k] - lam_old[k]) <= opt.tol * fmax(fabs(lam[k]), floor_abs);
        memcpy(lam_old, lam, Q * sizeof(double));
        r->iterations = it;
        if (conv) {
            r->converged = true;
            break;
        }
    }
    /* results */
    r->nmodes = p;
    r->omega2 = malloc((size_t)p * sizeof(double));
    r->phi = malloc((size_t)p * N * sizeof(double));
    r->residual = malloc((size_t)p * sizeof(double));
    r->eff = calloc((size_t)p * 6, sizeof(double));
    if (!r->omega2 || !r->phi || !r->residual || !r->eff) goto oom;
    memcpy(r->omega2, lam, (size_t)p * sizeof(double));
    memcpy(r->phi, X, (size_t)p * N * sizeof(double));
    double top = 0;
    for (int k = 0; k < p; k++) top = fmax(top, fabs(lam[k]));
    for (int k = 0; k < p; k++) {
        const double *ph = r->phi + (size_t)k * N;
        double mn = 0;
        csr_spmv(&r->M, ph, MX, NULL);
        mn = dotv(ph, MX, n);
        double sc = mn > 0 ? 1 / sqrt(mn) : 1;
        for (int i = 0; i < n; i++) r->phi[(size_t)k * N + (size_t)i] *= sc;
        csr_spmv(&r->K, ph, Y, NULL);
        csr_spmv(&r->M, ph, MY, NULL);
        double rr = 0, kn = 0, mnorm = 0;
        for (int i = 0; i < n; i++) {
            double d = Y[i] - r->omega2[k] * MY[i];
            rr += d * d, kn += Y[i] * Y[i], mnorm += MY[i] * MY[i];
        }
        double den = sqrt(kn) + fabs(r->omega2[k]) * sqrt(mnorm);
        if (!(den > 0)) den = 1e-12 * top * sqrt(mnorm) + 1e-300;
        r->residual[k] = sqrt(rr) / den;
        if (fabs(r->omega2[k]) <= 1e-9 * lam_avg) r->nrigid++;
    }
    if (r->nrigid > 0 && !supported) { /* rigid-body modes: distance from the analytic rigid-body subspace instead of the residual */
        double *Rb = calloc(6 * N, sizeof(double)), *MRb = calloc(6 * N, sizeof(double)), com[3] = {0, 0, 0}, mtot = 0;
        if (!Rb || !MRb) {
            free(Rb), free(MRb);
            goto oom;
        }
        for (int nd = 0; nd < m->nnodes; nd++) /* mass centre from the lumped row sums of M (translation along x) */
            if (r->eq[3 * nd] >= 0) Rb[r->eq[3 * nd]] = 1;
        csr_spmv(&r->M, Rb, MRb, NULL);
        for (int nd = 0; nd < m->nnodes; nd++)
            if (r->eq[3 * nd] >= 0) {
                double w = MRb[r->eq[3 * nd]];
                mtot += w;
                for (int k = 0; k < 3; k++) com[k] += w * m->xyz[3 * (size_t)nd + (size_t)k];
            }
        for (int k = 0; mtot > 0 && k < 3; k++) com[k] /= mtot;
        memset(Rb, 0, 6 * N * sizeof(double));
        for (int d = 0; d < 6; d++)
            for (int nd = 0; nd < m->nnodes; nd++) {
                const double *x = m->xyz + 3 * (size_t)nd;
                double rel[3] = {x[0] - com[0], x[1] - com[1], x[2] - com[2]}, f[3] = {0, 0, 0};
                if (d < 3) f[d] = 1;
                else {
                    int a = d - 3, b2 = (a + 1) % 3, c2 = (a + 2) % 3;
                    f[b2] = -rel[c2], f[c2] = rel[b2];
                }
                for (int k = 0; k < 3; k++)
                    if (r->eq[3 * nd + k] >= 0) Rb[(size_t)d * N + (size_t)r->eq[3 * nd + k]] = f[k];
            }
        for (int d = 0; d < 6; d++) { /* M-orthonormal basis of the rigid-body subspace */
            double *rd = Rb + (size_t)d * N, *mrd = MRb + (size_t)d * N;
            csr_spmv(&r->M, rd, mrd, NULL);
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < d; i++) {
                    double c2 = dotv(Rb + (size_t)i * N, mrd, n);
                    for (int k = 0; k < n; k++) rd[k] -= c2 * Rb[(size_t)i * N + (size_t)k], mrd[k] -= c2 * MRb[(size_t)i * N + (size_t)k];
                }
            double nm = sqrt(fmax(dotv(rd, mrd, n), 0));
            for (int k = 0; nm > 0 && k < n; k++) rd[k] /= nm, mrd[k] /= nm;
        }
        for (int k = 0; k < r->nrigid; k++) {
            double inside = 0;
            for (int d = 0; d < 6; d++) {
                double c2 = dotv(r->phi + (size_t)k * N, MRb + (size_t)d * N, n);
                inside += c2 * c2;
            }
            r->residual[k] = sqrt(fmax(0, 1 - inside));
        }
        free(Rb), free(MRb);
    }
    /* M-orthogonality */
    for (int i = 0; i < p; i++) {
        csr_spmv(&r->M, r->phi + (size_t)i * N, MX, NULL);
        for (int j = 0; j <= i; j++) r->orthogonality = fmax(r->orthogonality, fabs(dotv(r->phi + (size_t)j * N, MX, n) - (i == j)));
    }
    /* mass, mass centre, effective masses and inertias */
    {
        double *rv = calloc(N, sizeof(double)), *Mr1 = malloc(N * sizeof(double));
        if (!rv || !Mr1) {
            free(rv), free(Mr1);
            goto oom;
        }
        for (int nd = 0; nd < m->nnodes; nd++)
            if (r->eq[3 * nd] >= 0) rv[r->eq[3 * nd]] = 1;
        csr_spmv(&r->M, rv, Mr1, NULL);
        r->mass = dotv(rv, Mr1, n);
        for (int nd = 0; nd < m->nnodes; nd++)
            if (r->eq[3 * nd] >= 0)
                for (int k = 0; k < 3; k++) r->com[k] += m->xyz[3 * (size_t)nd + (size_t)k] * Mr1[r->eq[3 * nd]];
        for (int k = 0; r->mass > 0 && k < 3; k++) r->com[k] /= r->mass;
        for (int d = 0; d < 6; d++) {
            memset(rv, 0, N * sizeof(double));
            for (int nd = 0; nd < m->nnodes; nd++) {
                const double *x = m->xyz + 3 * (size_t)nd;
                double rel[3] = {x[0] - r->com[0], x[1] - r->com[1], x[2] - r->com[2]}, f[3] = {0, 0, 0};
                if (d < 3)
                    f[d] = 1;
                else { /* e_d x (x - c) */
                    int a = d - 3, b = (a + 1) % 3, c = (a + 2) % 3;
                    f[b] = -rel[c], f[c] = rel[b];
                }
                for (int k = 0; k < 3; k++)
                    if (r->eq[3 * nd + k] >= 0) rv[r->eq[3 * nd + k]] = f[k];
            }
            csr_spmv(&r->M, rv, Mr1, NULL);
            if (d >= 3) r->inertia[d - 3] = dotv(rv, Mr1, n);
            for (int k = 0; k < p; k++) {
                double L = dotv(r->phi + (size_t)k * N, Mr1, n);
                r->eff[6 * (size_t)k + (size_t)d] = L * L;
            }
        }
        free(rv), free(Mr1);
    }
    if (supported) { /* K itself was factorised (shift 0): keep it for residual flexibility */
        r->Kf = F;
        r->have_Kf = true;
        haveF = false;
    }
    ok = true;
    goto done;
oom:
    snprintf(err, errlen, "out of memory in the modal solver (%d equations, subspace %d)", n, q);
fail:
    ok = false;
done:
    if (haveF) chol_free(&F);
    csr_free(&A);
    free(X), free(Y), free(MX), free(MY), free(Kr), free(Mr), free(Qm), free(w), free(lam), free(lam_old), free(V), free(tmp), free(order);
    if (!ok) modal_result_free(r);
    return ok;
}

/* ------------------------------------------------------------------------------------------------ transient */

/* exact recurrence for x'' + 2 z w x' + w^2 x = p(t), p linear over an interval of length h (Nigam & Jennings) */
typedef struct Recurrence {
    double A, B, C, D, Ad, Bd, Cd, Dd;
} Recurrence;

static void recurrence(double w, double z, double h, Recurrence *R) {
    double k = w * w, s1 = sqrt(1 - z * z), wd = w * s1, e = exp(-z * w * h), s = sin(wd * h), c = cos(wd * h);
    R->A = e * (z / s1 * s + c);
    R->B = e * (s / wd);
    R->C = (1 / k) * (2 * z / (w * h) + e * (((1 - 2 * z * z) / (wd * h) - z / s1) * s - (1 + 2 * z / (w * h)) * c));
    R->D = (1 / k) * (1 - 2 * z / (w * h) + e * ((2 * z * z - 1) / (wd * h) * s + 2 * z / (w * h) * c));
    R->Ad = -e * (w / s1 * s);
    R->Bd = e * (c - z / s1 * s);
    R->Cd = (1 / k) * (-1 / h + e * ((w / s1 + z / (h * s1)) * s + c / h));
    R->Dd = (1 / (k * h)) * (1 - e * (z / s1 * s + c));
}

bool modal_transient(const ModalResult *r, const ModalLoad *L, const double *zeta, double *q, double *qd, double *rigid_share, char *err, size_t errlen) {
    int p = r->nmodes, ns = L->nsteps;
    size_t N = (size_t)r->neq, S = (size_t)ns + 1;
    if (!(L->dt > 0) || ns < 1) {
        snprintf(err, errlen, "the load needs a positive step and at least one interval");
        return false;
    }
    double *gen = calloc((size_t)(p ? p : 1) * (size_t)(L->npatterns ? L->npatterns : 1), sizeof(double));
    if (!gen) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    for (int i = 0; i < p; i++)
        for (int j = 0; j < L->npatterns; j++) gen[(size_t)i * (size_t)L->npatterns + (size_t)j] = dotv(r->phi + (size_t)i * N, L->patterns + (size_t)j * N, r->neq);
    double top = 0;
    for (int i = 0; i < p; i++) top = fmax(top, fabs(r->omega2[i]));
    if (rigid_share) *rigid_share = 0;
    for (int i = 0; i < p; i++) {
        bool rigid = i < r->nrigid; /* rigid-body modes come first (ascending, |w^2| ~ 0) */
        double z = zeta ? zeta[i] : 0;
        if (!rigid && !(z >= 0 && z < 1)) {
            free(gen);
            snprintf(err, errlen, "damping ratio of mode %d must be in [0, 1)", i + 1);
            return false;
        }
        Recurrence R;
        if (!rigid) recurrence(sqrt(r->omega2[i]), z, L->dt, &R);
        q[(size_t)i * S] = 0, qd[(size_t)i * S] = 0;
        for (size_t n = 0; n < S; n++) {
            double f = 0;
            for (int j = 0; j < L->npatterns; j++) f += gen[(size_t)i * (size_t)L->npatterns + (size_t)j] * L->histories[(size_t)j * S + n];
            if (rigid) {
                q[(size_t)i * S + n] = 0, qd[(size_t)i * S + n] = 0;
                continue;
            }
            if (n + 1 >= S) break;
            double f1 = 0;
            for (int j = 0; j < L->npatterns; j++) f1 += gen[(size_t)i * (size_t)L->npatterns + (size_t)j] * L->histories[(size_t)j * S + n + 1];
            double x = q[(size_t)i * S + n], v = qd[(size_t)i * S + n];
            q[(size_t)i * S + n + 1] = R.A * x + R.B * v + R.C * f + R.D * f1;
            qd[(size_t)i * S + n + 1] = R.Ad * x + R.Bd * v + R.Cd * f + R.Dd * f1;
        }
    }
    if (rigid_share && r->nrigid > 0) { /* generalised rigid-body force relative to the elastic ones, over the samples */
        for (size_t n = 0; n < S; n++) {
            double fr = 0, fe = 0;
            for (int i = 0; i < p; i++) {
                double f = 0;
                for (int j = 0; j < L->npatterns; j++) f += gen[(size_t)i * (size_t)L->npatterns + (size_t)j] * L->histories[(size_t)j * S + n];
                if (i < r->nrigid) fr += f * f;
                else fe += f * f;
            }
            if (fr + fe > 0) *rigid_share = fmax(*rigid_share, sqrt(fr / (fr + fe)));
        }
    }
    free(gen);
    return true;
}

void modal_expand(const ModalResult *r, const double *q, int nsteps, int step, double *u) {
    size_t N = (size_t)r->neq, S = (size_t)nsteps + 1;
    memset(u, 0, N * sizeof(double));
    for (int i = 0; i < r->nmodes; i++) {
        double qi = q[(size_t)i * S + (size_t)step];
        if (qi == 0) continue;
        const double *ph = r->phi + (size_t)i * N;
        for (size_t k = 0; k < N; k++) u[k] += qi * ph[k];
    }
}

bool newmark_transient(const ModalResult *r, const double *xyz, const ModalLoad *L, double alpha, double beta, double *u, double *v, char *err, size_t errlen) {
    int n = r->neq, ns = L->nsteps;
    size_t N = (size_t)n, S = (size_t)ns + 1;
    double h = L->dt, cK = 1 + 2 * beta / h, cM = 4 / (h * h) + 2 * alpha / h;
    CsrMatrix E = r->K;
    E.val = malloc((size_t)(E.nnz ? E.nnz : 1) * sizeof(double));
    double *a = calloc(N, sizeof(double)), *f = malloc(N * sizeof(double)), *t1 = malloc(N * sizeof(double)), *t2 = malloc(N * sizeof(double));
    double *t3 = malloc(N * sizeof(double));
    bool ok = false, haveE = false, haveM = false, too_big = false;
    CholFactor FE, FM;
    SolveStats st;
    memset(&st, 0, sizeof st);
    if (!E.val || !a || !f || !t1 || !t2 || !t3) {
        snprintf(err, errlen, "out of memory");
        goto done;
    }
    for (int64_t k = 0; k < E.nnz; k++) E.val[k] = cK * r->K.val[k] + cM * r->M.val[k];
    if (!ortho_factor(r->nnodes, xyz, &E, r->eq, n, 0, NULL, NULL, &FE, &st, &too_big, err, errlen)) goto done;
    haveE = true;
    if (!ortho_factor(r->nnodes, xyz, &r->M, r->eq, n, 0, NULL, NULL, &FM, &st, &too_big, err, errlen)) goto done;
    haveM = true;
    /* force at a sample */
#define FORCE(step, out)                                                                                                                                  \
    do {                                                                                                                                                  \
        memset(out, 0, N * sizeof(double));                                                                                                               \
        for (int j = 0; j < L->npatterns; j++) {                                                                                                          \
            double g = L->histories[(size_t)j * S + (size_t)(step)];                                                                                      \
            if (g != 0)                                                                                                                                   \
                for (size_t k = 0; k < N; k++) out[k] += g * L->patterns[(size_t)j * N + k];                                                              \
        }                                                                                                                                                 \
    } while (0)
    memset(u, 0, N * sizeof(double)), memset(v, 0, N * sizeof(double));
    FORCE(0, f);
    chol_solve(&FM, f, a); /* a0 = M^-1 f0 (starting at rest) */
    for (int s = 0; s < ns; s++) {
        const double *un = u + (size_t)s * N, *vn = v + (size_t)s * N;
        double *u1 = u + (size_t)(s + 1) * N, *v1 = v + (size_t)(s + 1) * N;
        FORCE(s + 1, f);
        for (size_t k = 0; k < N; k++) t1[k] = 4 / (h * h) * un[k] + 4 / h * vn[k] + a[k], t2[k] = 2 / h * un[k] + vn[k];
        csr_spmv(&r->M, t1, t3, NULL);
        for (size_t k = 0; k < N; k++) f[k] += t3[k];
        if (alpha != 0 || beta != 0) { /* C t2 = alpha M t2 + beta K t2 */
            csr_spmv(&r->M, t2, t3, NULL);
            for (size_t k = 0; k < N; k++) f[k] += alpha * t3[k];
            csr_spmv(&r->K, t2, t3, NULL);
            for (size_t k = 0; k < N; k++) f[k] += beta * t3[k];
        }
        chol_solve(&FE, f, u1);
        for (size_t k = 0; k < N; k++) {
            double du = u1[k] - un[k], an = a[k];
            v1[k] = 2 / h * du - vn[k];
            a[k] = 4 / (h * h) * du - 4 / h * vn[k] - an;
        }
    }
#undef FORCE
    ok = true;
done:
    if (haveE) chol_free(&FE);
    if (haveM) chol_free(&FM);
    free(E.val), free(a), free(f), free(t1), free(t2), free(t3);
    return ok;
}

bool modal_frf(const ModalResult *r, const double *F, int out, const double *zeta, const double *freq_hz, int nfreq, bool residual, double *re, double *im,
               char *err, size_t errlen) {
    int p = r->nmodes;
    size_t N = (size_t)r->neq;
    if (out < 0 || out >= r->neq) {
        snprintf(err, errlen, "output equation %d out of range", out);
        return false;
    }
    if (residual && !r->have_Kf) {
        snprintf(err, errlen, "residual flexibility needs a supported part (the free-free stiffness is singular)");
        return false;
    }
    double top = 0, stat = 0;
    for (int i = 0; i < p; i++) top = fmax(top, fabs(r->omega2[i]));
    if (residual) {
        double *x = malloc(N * sizeof(double));
        if (!x) {
            snprintf(err, errlen, "out of memory");
            return false;
        }
        chol_solve(&r->Kf, F, x);
        stat = x[out];
        free(x);
        for (int i = 0; i < p; i++) {
            if (i < r->nrigid) continue;
            const double *ph = r->phi + (size_t)i * N;
            stat -= ph[out] * dotv(ph, F, r->neq) / r->omega2[i];
        }
    }
    for (int k = 0; k < nfreq; k++) {
        double w = 2 * M_PI * freq_hz[k], sr = residual ? stat : 0, si = 0;
        for (int i = 0; i < p; i++) {
            if (i < r->nrigid) continue;
            const double *ph = r->phi + (size_t)i * N;
            double gi = ph[out] * dotv(ph, F, r->neq), wi = sqrt(r->omega2[i]), z = zeta ? zeta[i] : 0;
            double a = r->omega2[i] - w * w, b = 2 * z * wi * w, den = a * a + b * b;
            sr += gi * a / den, si -= gi * b / den;
        }
        re[k] = sr, im[k] = si;
    }
    return true;
}
