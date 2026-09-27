/* flexbody.c - Craig-Bampton reduction of a meshed part into multibody elastic coordinates (see flexbody.h) */
#include "flexbody.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmath.h"

static double dot(const double *a, const double *b, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

/* node-space field -> full equation space (unused nodes dropped) */
static void to_eq(const int *eq, size_t N3, const double *node, double *eqv) {
    for (size_t k = 0; k < N3; k++)
        if (eq[k] >= 0) eqv[eq[k]] = node[k];
}

bool flex_reduce(const FlexReduceInput *in, MbFlexDef *out, FlexReduceReport *rep, char *err, size_t errlen) {
    const OrthoModel *om = in->om;
    int nn = om->nnodes, ni = in->ninterfaces, p = in->fixed_modes, nr = 6 * ni + p;
    size_t N3 = 3 * (size_t)nn, NR = (size_t)nr;
    memset(rep, 0, sizeof *rep);
    memset(out, 0, sizeof *out);
    if (nr < 1 || p < 0 || ni < 0 || in->nroot < 1) {
        snprintf(err, errlen, "the reduction needs a root region and at least one coordinate");
        return false;
    }
    bool ok = false, haveFI = false, haveR2 = false;
    unsigned char *all_fix = calloc(N3, 1);
    int *itf_of = malloc((size_t)(nn ? nn : 1) * sizeof(int)), *eqI = malloc(N3 * sizeof(int));
    double *B = NULL, *BF = NULL, *KB = NULL, *MB = NULL, *Kr = NULL, *Mr = NULL, *V = NULL, *w = NULL, *Q = NULL, *tmp = NULL, *rhs = NULL, *sol = NULL;
    double *fields = NULL, *Mfields = NULL, *PhiF = NULL;
    ModalResult A, R2;
    memset(&A, 0, sizeof A), memset(&R2, 0, sizeof R2);
    CholFactor FI;
    if (!all_fix || !itf_of || !eqI) goto oom;
    for (int n = 0; n < nn; n++) itf_of[n] = -1;
    for (int k = 0; k < in->nroot; k++) {
        int n = in->root_nodes[k];
        if (n < 0 || n >= nn) {
            snprintf(err, errlen, "root node %d out of range", n);
            goto fail;
        }
        all_fix[3 * (size_t)n] = all_fix[3 * (size_t)n + 1] = all_fix[3 * (size_t)n + 2] = 1;
        itf_of[n] = -2;
    }
    for (int i = 0; i < ni; i++)
        for (int k = 0; k < in->itf_count[i]; k++) {
            int n = in->itf_nodes[i][k];
            if (n < 0 || n >= nn || itf_of[n] != -1) {
                snprintf(err, errlen, "interface %d shares nodes with the root or another interface (node %d)", i, n);
                goto fail;
            }
            itf_of[n] = i;
            all_fix[3 * (size_t)n] = all_fix[3 * (size_t)n + 1] = all_fix[3 * (size_t)n + 2] = 1;
        }
    /* full stiffness and mass, every component free */
    StructDyn sfull = {om, in->density, NULL};
    if (!modal_assemble(&sfull, &A, err, errlen)) goto fail;
    size_t NE = (size_t)A.neq;
    /* interior stiffness with the root and every interface held */
    int neqI = 0;
    SolveStats st;
    memset(&st, 0, sizeof st);
    if (!ortho_assemble_factor(om, all_fix, eqI, &neqI, &FI, &st, err, errlen)) goto fail;
    haveFI = true;
    B = calloc(NR * N3, sizeof(double));
    BF = calloc(NR * NE, sizeof(double));
    tmp = calloc(NE, sizeof(double));
    rhs = calloc((size_t)(neqI ? neqI : 1), sizeof(double));
    sol = calloc((size_t)(neqI ? neqI : 1), sizeof(double));
    if (!B || !BF || !tmp || !rhs || !sol) goto oom;
    /* constraint modes: a unit rigid motion of one interface, statically extended into the interior */
    for (int i = 0; i < ni; i++)
        for (int d = 0; d < 6; d++) {
            double *col = B + (size_t)(6 * i + d) * N3, *colF = BF + (size_t)(6 * i + d) * NE;
            for (int k = 0; k < in->itf_count[i]; k++) {
                int n = in->itf_nodes[i][k];
                const double *x = om->xyz + 3 * (size_t)n;
                double u[3] = {0, 0, 0}, r[3];
                if (d < 3) u[d] = 1;
                else {
                    double e[3] = {0, 0, 0};
                    e[d - 3] = 1;
                    for (int c = 0; c < 3; c++) r[c] = x[c] - in->itf_point[i][c];
                    mv3_cross(u, e, r);
                }
                for (int c = 0; c < 3; c++) col[3 * (size_t)n + (size_t)c] = u[c];
            }
            memset(colF, 0, NE * sizeof(double));
            to_eq(A.eq, N3, col, colF);
            csr_spmv(&A.K, colF, tmp, NULL);
            for (size_t k = 0; k < N3; k++)
                if (eqI[k] >= 0 && A.eq[k] >= 0) rhs[eqI[k]] = -tmp[A.eq[k]];
            chol_solve(&FI, rhs, sol);
            for (size_t k = 0; k < N3; k++)
                if (eqI[k] >= 0) col[k] = sol[eqI[k]];
            memset(colF, 0, NE * sizeof(double));
            to_eq(A.eq, N3, col, colF);
        }
    chol_free(&FI);
    haveFI = false;
    /* fixed-interface normal modes */
    rep->fixed_modes_converged = true;
    if (p > 0) {
        StructDyn sfix = {om, in->density, all_fix};
        ModalOptions mo = {p, 0, 1e-10, 400};
        if (!modal_solve(&sfix, &mo, &R2, err, errlen)) goto fail;
        haveR2 = true;
        rep->fixed_modes_converged = R2.converged;
        for (int m = 0; m < p; m++) {
            double *col = B + (size_t)(6 * ni + m) * N3, *colF = BF + (size_t)(6 * ni + m) * NE;
            for (size_t k = 0; k < N3; k++) col[k] = R2.eq[k] >= 0 ? R2.phi[(size_t)m * (size_t)R2.neq + (size_t)R2.eq[k]] : 0;
            to_eq(A.eq, N3, col, colF);
        }
    }
    free(B), B = NULL; /* the basis lives on in the equation space */
    /* reduced matrices, then mass-normalised coordinates */
    KB = malloc(NE * sizeof(double));
    MB = malloc(NE * sizeof(double));
    Kr = malloc(NR * NR * sizeof(double));
    Mr = malloc(NR * NR * sizeof(double));
    V = malloc(NR * NR * sizeof(double));
    w = malloc(NR * sizeof(double));
    Q = calloc(NR * NR, sizeof(double));
    if (!KB || !MB || !Kr || !Mr || !V || !w || !Q) goto oom;
    for (size_t c = 0; c < NR; c++) {
        csr_spmv(&A.K, BF + c * NE, KB, NULL);
        csr_spmv(&A.M, BF + c * NE, MB, NULL);
        for (size_t r = 0; r < NR; r++) Kr[r * NR + c] = dot(BF + r * NE, KB, NE), Mr[r * NR + c] = dot(BF + r * NE, MB, NE);
    }
    for (size_t r = 0; r < NR; r++)
        for (size_t c = 0; c < r; c++) {
            Kr[r * NR + c] = Kr[c * NR + r] = 0.5 * (Kr[r * NR + c] + Kr[c * NR + r]);
            Mr[r * NR + c] = Mr[c * NR + r] = 0.5 * (Mr[r * NR + c] + Mr[c * NR + r]);
        }
    if (!mm_chol_factor(Mr, nr, 1e-14)) {
        snprintf(err, errlen, "the reduced mass matrix is singular: the basis vectors are dependent");
        goto fail;
    }
    {
        double *col = malloc(NR * sizeof(double));
        if (!col) goto oom;
        for (size_t c = 0; c < NR; c++) { /* L^-1 Kr */
            for (size_t r = 0; r < NR; r++) col[r] = Kr[r * NR + c];
            mm_chol_lower(Mr, nr, col);
            for (size_t r = 0; r < NR; r++) V[r * NR + c] = col[r];
        }
        for (size_t r = 0; r < NR; r++) { /* (L^-1 (L^-1 Kr)^T)^T */
            for (size_t c = 0; c < NR; c++) col[c] = V[r * NR + c];
            mm_chol_lower(Mr, nr, col);
            for (size_t c = 0; c < NR; c++) Kr[r * NR + c] = col[c];
        }
        for (size_t r = 0; r < NR; r++)
            for (size_t c = 0; c < r; c++) Kr[r * NR + c] = Kr[c * NR + r] = 0.5 * (Kr[r * NR + c] + Kr[c * NR + r]);
        if (mm_sym_eig(Kr, nr, w, V) < 0) {
            free(col);
            snprintf(err, errlen, "the reduced eigenproblem did not converge");
            goto fail;
        }
        for (int m = 0; m < nr; m++) { /* ascending: column nr-1-m of V; Q column m = L^-T v */
            int src = nr - 1 - m;
            for (size_t r = 0; r < NR; r++) col[r] = V[r * NR + (size_t)src];
            mm_chol_upper(Mr, nr, col);
            for (size_t r = 0; r < NR; r++) Q[r * NR + (size_t)m] = col[r];
        }
        for (int m = 0; m < nr / 2; m++) {
            double t = w[m];
            w[m] = w[nr - 1 - m], w[nr - 1 - m] = t;
        }
        free(col);
    }
    if (!(w[0] > 0)) {
        snprintf(err, errlen, "the reduced stiffness is not positive definite (lowest eigenvalue %.3g): the root region does not hold the part", w[0]);
        goto fail;
    }
    /* coordinates kept: all, or those up to the cutoff; the loss of interface flexibility is measured on the compliance
     * sum over coordinates of u u^T / w^2 with u the interface motions of the coordinate */
    int nk = nr;
    if (in->max_frequency_hz > 0) {
        double w2cut = pow(2 * M_PI * in->max_frequency_hz, 2);
        nk = 0;
        while (nk < nr && w[nk] <= w2cut) nk++;
        if (nk == 0) {
            snprintf(err, errlen, "no coordinate below %.6g Hz: the lowest is %.6g Hz", in->max_frequency_hz, sqrt(fmax(w[0], 0)) / (2 * M_PI));
            goto fail;
        }
    }
    rep->interface_loss = calloc((size_t)(6 * ni + 1), sizeof(double));
    if (!rep->interface_loss) goto oom;
    for (int i = 0; i < ni && nk < nr; i++)
        for (int blk = 0; blk < 2; blk++) { /* translation and rotation blocks, rotated to body axes: R^T C R */
            double all[9] = {0}, kept[9] = {0};
            for (int m = 0; m < nr; m++)
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 3; c++) {
                        double v = Q[(size_t)(6 * i + 3 * blk + r) * NR + (size_t)m] * Q[(size_t)(6 * i + 3 * blk + c) * NR + (size_t)m] / w[m];
                        all[3 * r + c] += v;
                        if (m < nk) kept[3 * r + c] += v;
                    }
            for (int d = 0; d < 3; d++) {
                double a = 0, k = 0;
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 3; c++) a += in->R[3 * r + d] * all[3 * r + c] * in->R[3 * c + d], k += in->R[3 * r + d] * kept[3 * r + c] * in->R[3 * c + d];
                double loss = a > 0 ? (a - k) / a : 0;
                rep->interface_loss[6 * i + 3 * blk + d] = loss;
                rep->compliance_loss = fmax(rep->compliance_loss, loss);
            }
        }
    rep->dropped = nr - nk;
    rep->max_frequency_hz = sqrt(fmax(w[nk - 1], 0)) / (2 * M_PI);
    if (!mbflex_alloc(out, -1, nk, ni)) goto oom;
    /* coordinate shapes in the full equation space */
    size_t NK = (size_t)nk;
    PhiF = calloc(NK * NE, sizeof(double));
    if (!PhiF) goto oom;
    for (size_t m = 0; m < NK; m++)
        for (size_t c = 0; c < NR; c++) {
            double q = Q[c * NR + m];
            if (q == 0) continue;
            for (size_t k = 0; k < NE; k++) PhiF[m * NE + k] += q * BF[c * NE + k];
        }
    /* fields for the invariants, FE axes, relative to the body origin p: translations (3), rotations (3), g_ij (6) */
    fields = calloc(12 * NE, sizeof(double));
    Mfields = malloc(12 * NE * sizeof(double));
    if (!fields || !Mfields) goto oom;
    for (int n = 0; n < nn; n++) {
        double r[3];
        for (int c = 0; c < 3; c++) r[c] = om->xyz[3 * (size_t)n + (size_t)c] - in->p[c];
        for (int d = 0; d < 12; d++) {
            double u[3] = {0, 0, 0};
            if (d < 3) u[d] = 1;
            else if (d < 6) {
                double e[3] = {0, 0, 0};
                e[d - 3] = 1;
                mv3_cross(u, e, r);
            } else { /* g_ij = 2 delta_ij r - r_i e_j - r_j e_i for (i, j) = xx yy zz xy yz zx */
                static const int GI[6] = {0, 1, 2, 0, 1, 2}, GJ[6] = {0, 1, 2, 1, 2, 0};
                int i = GI[d - 6], j = GJ[d - 6];
                if (i == j) mv3_scale(u, r, 2);
                u[j] -= r[i], u[i] -= r[j];
            }
            for (int c = 0; c < 3; c++) {
                int e = A.eq[3 * n + c];
                if (e >= 0) fields[(size_t)d * NE + (size_t)e] = u[c];
            }
        }
    }
    for (int d = 0; d < 12; d++) csr_spmv(&A.M, fields + (size_t)d * NE, Mfields + (size_t)d * NE, NULL);
    for (int m = 0; m < nk; m++) {
        const double *ph = PhiF + (size_t)m * NE;
        out->omega2[m] = w[m];
        out->zeta[m] = in->zeta;
        double lt[3], lr[3], dj[9], tb[3], rb[3];
        for (int d = 0; d < 3; d++) lt[d] = dot(ph, Mfields + (size_t)d * NE, NE), lr[d] = dot(ph, Mfields + (size_t)(d + 3) * NE, NE);
        static const int GI[6] = {0, 1, 2, 0, 1, 2}, GJ[6] = {0, 1, 2, 1, 2, 0};
        for (int d = 0; d < 6; d++) {
            double v = dot(ph, Mfields + (size_t)(d + 6) * NE, NE);
            dj[3 * GI[d] + GJ[d]] = dj[3 * GJ[d] + GI[d]] = v;
        }
        mm3_tmulv(tb, in->R, lt), mm3_tmulv(rb, in->R, lr);
        mv3_copy(out->ell + 6 * m, rb), mv3_copy(out->ell + 6 * m + 3, tb);
        double tmp9[9]; /* R^T dJ R */
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) tmp9[3 * r + c] = dj[3 * r + 0] * in->R[3 * 0 + c] + dj[3 * r + 1] * in->R[3 * 1 + c] + dj[3 * r + 2] * in->R[3 * 2 + c];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) out->dJ[9 * m + 3 * r + c] = in->R[3 * 0 + r] * tmp9[3 * 0 + c] + in->R[3 * 1 + r] * tmp9[3 * 1 + c] + in->R[3 * 2 + r] * tmp9[3 * 2 + c];
        /* interface motions: the master motions carried by the constraint-mode part of the coordinate */
        for (int i = 0; i < ni; i++) {
            double t[3], th[3];
            for (int d = 0; d < 3; d++) t[d] = Q[(size_t)(6 * i + d) * NR + (size_t)m], th[d] = Q[(size_t)(6 * i + 3 + d) * NR + (size_t)m];
            mm3_tmulv(out->interfaces[i].phi + 3 * m, in->R, t);
            mm3_tmulv(out->interfaces[i].psi + 3 * m, in->R, th);
        }
    }
    for (int i = 0; i < ni; i++) {
        double d3[3];
        for (int c = 0; c < 3; c++) d3[c] = in->itf_point[i][c] - in->p[c];
        mm3_tmulv(out->interfaces[i].point, in->R, d3);
    }
    /* rigid mass properties of the FE model (Gauss integration of the density), body coordinates */
    {
        double mass = 0, h[3] = {0, 0, 0}, S2[9] = {0}; /* second moments about the body origin, FE axes */
        const double G = 1 / sqrt(3.0);
        for (int e = 0; e < om->nelems; e++) {
            double X[8][3];
            for (int a = 0; a < 8; a++)
                for (int c = 0; c < 3; c++) X[a][c] = om->xyz[3 * (size_t)om->conn[8 * (size_t)e + a] + (size_t)c] - in->p[c];
            for (int g = 0; g < 8; g++) {
                double N[8], dN[8][3], J[3][3], dNdx[8][3], x[3] = {0, 0, 0};
                hex8_shape(HEX8_XI[g][0] * G, HEX8_XI[g][1] * G, HEX8_XI[g][2] * G, N, dN);
                double dm = in->density[e] * hex8_jacobian(X, dN, J, dNdx);
                for (int a = 0; a < 8; a++)
                    for (int c = 0; c < 3; c++) x[c] += N[a] * X[a][c];
                mass += dm;
                for (int c = 0; c < 3; c++) h[c] += dm * x[c];
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 3; c++) S2[3 * r + c] += dm * x[r] * x[c];
            }
        }
        double c_fe[3] = {h[0] / mass, h[1] / mass, h[2] / mass}, Ic_fe[9], tr = S2[0] + S2[4] + S2[8];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) Ic_fe[3 * r + c] = (r == c ? tr : 0) - S2[3 * r + c];
        double cc = mv3_dot(c_fe, c_fe);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) Ic_fe[3 * r + c] -= mass * ((r == c ? cc : 0) - c_fe[r] * c_fe[c]);
        rep->fe_mass = mass;
        mm3_tmulv(rep->fe_com, in->R, c_fe);
        double tmp9[9];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) tmp9[3 * r + c] = Ic_fe[3 * r + 0] * in->R[3 * 0 + c] + Ic_fe[3 * r + 1] * in->R[3 * 1 + c] + Ic_fe[3 * r + 2] * in->R[3 * 2 + c];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) rep->fe_inertia[3 * r + c] = in->R[3 * 0 + r] * tmp9[3 * 0 + c] + in->R[3 * 1 + r] * tmp9[3 * 1 + c] + in->R[3 * 2 + r] * tmp9[3 * 2 + c];
    }
    if (in->keep_shapes) {
        rep->shapes = calloc(NK * N3, sizeof(double));
        if (!rep->shapes) goto oom;
        for (size_t m = 0; m < NK; m++)
            for (size_t k = 0; k < N3; k++)
                if (A.eq[k] >= 0) rep->shapes[m * N3 + k] = PhiF[m * NE + (size_t)A.eq[k]];
    }
    rep->coordinates = nk, rep->constraint_modes = 6 * ni, rep->fixed_modes = p;
    ok = true;
    goto done;
oom:
    snprintf(err, errlen, "out of memory in the flexible-body reduction (%d nodes, %d coordinates)", nn, nr);
fail:
    ok = false;
    mbflex_free(out);
    free(rep->shapes), rep->shapes = NULL;
    free(rep->interface_loss), rep->interface_loss = NULL;
done:
    if (haveFI) chol_free(&FI);
    if (haveR2) modal_result_free(&R2);
    modal_result_free(&A);
    free(all_fix), free(itf_of), free(eqI), free(B), free(BF), free(KB), free(MB), free(Kr), free(Mr), free(V), free(w), free(Q), free(tmp), free(rhs), free(sol);
    free(fields), free(Mfields), free(PhiF);
    return ok;
}
