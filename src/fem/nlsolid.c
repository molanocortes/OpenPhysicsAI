/* nlsolid.c - Newton with load stepping on the total-Lagrangian hex8 (nlsolid.h, docs/contracts/dynamics.md) */
#include "nlsolid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sparse.h"

static void elem_coords(const NlModel *m, int e, double X[8][3]) {
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) X[a][k] = m->xyz[3 * (size_t)m->conn[8 * (size_t)e + (size_t)a] + (size_t)k];
}

static void elem_u(const NlModel *m, int e, const double *u, double ue[24]) {
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) ue[3 * a + k] = u[3 * (size_t)m->conn[8 * (size_t)e + (size_t)a] + (size_t)k];
}

/* internal force (and the tangent when K is given) of the whole model at u; false when an element is inverted */
static bool assemble(const NlModel *m, const double *u, Hex8NlState *state, bool commit, double *fint, CsrMatrix *K,
                     const int *eq, double *min_detF) {
    memset(fint, 0, 3 * (size_t)m->nnodes * sizeof(double));
    if (K) csr_zero(K);
    if (min_detF) *min_detF = INFINITY;
    bool ok = true;
    for (int e = 0; e < m->nelems; e++) {
        double X[8][3], ue[24], fe[24], Ke[576], dF;
        elem_coords(m, e, X);
        elem_u(m, e, u, ue);
        int mi = m->elem_mat ? m->elem_mat[e] : 0;
        Hex8NlState *st = state ? state + 8 * (size_t)e : NULL;
        if (!hex8_nl_element(X, ue, &m->mat[mi], st, commit, HEX8_NL_EAS, fe, K ? Ke : NULL, NULL, NULL, &dF)) {
            ok = false;
            continue;
        }
        if (min_detF) *min_detF = fmin(*min_detF, dF);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) fint[3 * (size_t)m->conn[8 * (size_t)e + (size_t)a] + (size_t)k] += fe[3 * a + k];
        if (!K) continue;
        int dofs[24];
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) dofs[3 * a + k] = eq[3 * m->conn[8 * (size_t)e + (size_t)a] + k];
        csr_add_element(K, 24, dofs, Ke);
    }
    return ok;
}

void nlsolid_result_free(NlResult *r) {
    if (!r) return;
    free(r->u), free(r->reaction), free(r->state);
    memset(r, 0, sizeof *r);
}

bool nlsolid_solve(const NlModel *m, const NlLoads *L, const NlOptions *opt, NlResult *res, char *err, size_t errlen) {
    NlOptions o = {0};
    if (opt) o = *opt;
    if (o.load_steps <= 0) o.load_steps = 10;
    if (o.max_newton <= 0) o.max_newton = 25;
    if (!(o.tol > 0)) o.tol = 1e-8;
    if (!(o.min_step > 0)) o.min_step = 1e-4;
    memset(res, 0, sizeof *res);
    size_t nn = (size_t)m->nnodes, ne = (size_t)m->nelems;
    int *eq = malloc(3 * nn * sizeof(int));
    double *u = calloc(3 * nn, sizeof(double)), *fint = calloc(3 * nn, sizeof(double));
    double *du = NULL, *rhs = NULL, *fext = calloc(3 * nn, sizeof(double));
    Hex8NlState *state = calloc(8 * (ne ? ne : 1), sizeof *state);
    int *elem_dofs = malloc(24 * (ne ? ne : 1) * sizeof(int));
    CsrMatrix K = {0};
    CholFactor F = {0};
    bool ok = false;
    if (!eq || !u || !fint || !fext || !state || !elem_dofs) {
        snprintf(err, errlen, "out of memory for the nonlinear solve");
        goto done;
    }
    int neq = 0;
    for (size_t i = 0; i < 3 * nn; i++) eq[i] = L->fixed && L->fixed[i] ? -1 : neq++;
    res->neq = neq;
    if (!neq) {
        snprintf(err, errlen, "every displacement is prescribed: there is nothing to solve");
        goto done;
    }
    for (int e = 0; e < m->nelems; e++)
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) elem_dofs[24 * (size_t)e + 3 * (size_t)a + (size_t)k] = eq[3 * m->conn[8 * (size_t)e + (size_t)a] + k];
    if (!csr_from_elements(&K, neq, m->nelems, 24, elem_dofs, err, errlen)) goto done;
    du = calloc((size_t)neq, sizeof(double)), rhs = calloc((size_t)neq, sizeof(double));
    if (!du || !rhs) {
        snprintf(err, errlen, "out of memory for the nonlinear solve");
        goto done;
    }
    /* gravity, once: it scales with the load factor like every other load */
    if (m->density && (L->gravity[0] || L->gravity[1] || L->gravity[2])) {
        for (int e = 0; e < m->nelems; e++) {
            double X[8][3], fe[24];
            elem_coords(m, e, X);
            double b[3] = {L->gravity[0], L->gravity[1], L->gravity[2]};
            int mi = m->elem_mat ? m->elem_mat[e] : 0;
            for (int k = 0; k < 3; k++) b[k] *= m->density[mi];
            hex8_body_load(X, b, fe);
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) fext[3 * (size_t)m->conn[8 * (size_t)e + (size_t)a] + (size_t)k] += fe[3 * a + k];
        }
    }
    if (L->nodal_force)
        for (size_t i = 0; i < 3 * nn; i++) fext[i] += L->nodal_force[i];

    double lambda = 0, step = 1.0 / o.load_steps;
    double *u_save = malloc(3 * nn * sizeof(double));
    Hex8NlState *state_save = malloc(8 * (ne ? ne : 1) * sizeof *state_save);
    if (!u_save || !state_save) {
        free(u_save), free(state_save);
        snprintf(err, errlen, "out of memory for the nonlinear solve");
        goto done;
    }
    while (lambda < 1 - 1e-12) {
        double target = fmin(1.0, lambda + step);
        memcpy(u_save, u, 3 * nn * sizeof(double));
        memcpy(state_save, state, 8 * ne * sizeof *state);
        for (size_t i = 0; i < 3 * nn; i++) /* prescribed displacements follow the load factor */
            if (L->fixed && L->fixed[i]) u[i] = L->fixed_value ? target * L->fixed_value[i] : 0.0;
        bool converged = false, indefinite = false;
        double resid = 0;
        for (int it = 0; it < o.max_newton; it++) {
            double dF;
            if (!assemble(m, u, state, false, fint, &K, eq, &dF)) {
                resid = INFINITY;
                break;
            }
            res->min_det_f = res->min_det_f == 0 ? dF : fmin(res->min_det_f, dF);
            double rnorm = 0, fnorm = 0;
            for (size_t i = 0; i < 3 * nn; i++) {
                double r = target * fext[i] - fint[i];
                if (eq[i] >= 0) rhs[eq[i]] = r, rnorm += r * r;
                fnorm += fint[i] * fint[i] + target * target * fext[i] * fext[i];
            }
            rnorm = sqrt(rnorm), fnorm = sqrt(fnorm);
            resid = rnorm / (fnorm > 0 ? fnorm : 1.0);
            res->iterations++;
            if (resid < o.tol) {
                converged = true;
                break;
            }
            chol_free(&F);
            int bad = -1;
            SolveStats st = {0};
            if (!chol_factor(&K, NULL, &F, NULL, NULL, &st, &bad, err, errlen)) {
                indefinite = true;
                break;
            }
            chol_solve(&F, rhs, du);
            for (size_t i = 0; i < 3 * nn; i++)
                if (eq[i] >= 0) u[i] += du[eq[i]];
        }
        if (converged) {
            double dF;
            assemble(m, u, state, true, fint, NULL, eq, &dF); /* commit the plastic state of the increment */
            lambda = target;
            res->increments++;
            res->residual = resid;
            if (step < 1.0 / o.load_steps) step = fmin(1.0 / o.load_steps, 2 * step); /* grow back after a cut-back */
            continue;
        }
        memcpy(u, u_save, 3 * nn * sizeof(double));
        memcpy(state, state_save, 8 * ne * sizeof *state);
        if (indefinite && o.stop_when_indefinite) {
            res->tangent_indefinite = true;
            res->load_factor = lambda;
            free(u_save), free(state_save);
            ok = true;
            goto finish;
        }
        step *= 0.5;
        res->cutbacks++;
        if (step < o.min_step) {
            free(u_save), free(state_save);
            snprintf(err, errlen, "the increment fell below %.3g of the load without converging (residual %.3g%s)", o.min_step, resid,
                     indefinite ? ", tangent not positive definite" : "");
            goto done;
        }
    }
    free(u_save), free(state_save);
    res->load_factor = lambda;
    ok = true;
finish:
    /* reactions: the internal force at the prescribed components */
    {
        double dF;
        assemble(m, u, state, false, fint, NULL, eq, &dF);
        res->u = malloc(3 * nn * sizeof(double));
        res->reaction = calloc(3 * nn, sizeof(double));
        res->state = malloc(8 * (ne ? ne : 1) * sizeof *state);
        if (!res->u || !res->reaction || !res->state) {
            snprintf(err, errlen, "out of memory for the result");
            ok = false;
            goto done;
        }
        memcpy(res->u, u, 3 * nn * sizeof(double));
        memcpy(res->state, state, 8 * ne * sizeof *state);
        for (size_t i = 0; i < 3 * nn; i++)
            if (L->fixed && L->fixed[i]) res->reaction[i] = fint[i] - res->load_factor * fext[i];
    }
done:
    chol_free(&F);
    csr_free(&K);
    free(eq), free(u), free(fint), free(fext), free(du), free(rhs), free(state), free(elem_dofs);
    if (!ok) nlsolid_result_free(res);
    return ok;
}
