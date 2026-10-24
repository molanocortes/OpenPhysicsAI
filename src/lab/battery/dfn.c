/* dfn.c - the Doyle-Fuller-Newman model of a lithium-ion cell (dfn.h). */
#include "dfn.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const double FA = 96485.33212, RG = 8.314462618;

/* ---- the LG M50 (Chen et al. 2020, via PyBaMM's Chen2020 parameter set, BSD-3) --------------------------------- */

static double ocp_graphite_lgm50(double x) {
    return 1.9793 * exp(-39.3631 * x) + 0.2482 - 0.0909 * tanh(29.8538 * (x - 0.1234)) - 0.04478 * tanh(14.9159 * (x - 0.2769)) -
           0.0205 * tanh(30.4444 * (x - 0.6103));
}
static double ocp_nmc_lgm50(double x) {
    return -0.8090 * x + 4.4875 - 0.0428 * tanh(18.5138 * (x - 0.5542)) - 17.7326 * tanh(15.7890 * (x - 0.3117)) + 17.5842 * tanh(15.9308 * (x - 0.3120));
}
/* LiPF6 in EC:EMC 3:7, Nyman, Behm and Lindbergh (2008); no temperature dependence given */
static double de_nyman(double c, double T) {
    double x = c / 1000;
    return 8.794e-11 * x * x - 3.972e-10 * x + 4.862e-10;
}
static double kappa_nyman(double c, double T) {
    double x = c / 1000;
    return 0.1297 * x * x * x - 2.51 * pow(x, 1.5) + 3.329 * x;
}

void dfn_spec_lgm50(DfnSpec *s, int n, int nr) {
    memset(s, 0, sizeof *s);
    s->neg = (DfnElectrode){85.2e-6, 0.25, 0.75, 5.86e-6, 33133, 29866, 3.3e-14, 215, 6.48e-7, 35000, ocp_graphite_lgm50, 0, n};
    s->pos = (DfnElectrode){75.6e-6, 0.335, 0.665, 5.22e-6, 63104, 17038, 4e-15, 0.18, 3.42e-6, 17800, ocp_nmc_lgm50, 0, n};
    s->Lsep = 12e-6, s->eps_sep = 0.47, s->nsep = n > 4 ? n / 2 : 2;
    s->brug = 1.5, s->ce0 = 1000, s->tplus = 0.2594, s->De = de_nyman, s->kappa = kappa_nyman;
    s->area = 0.065 * 1.58, s->Tref = 298.15, s->nr = nr;
}

/* ---- the state ------------------------------------------------------------------------------------------------- */

struct Dfn {
    DfnSpec s;
    int N, Ne, nn, ns, np, nr, nu;
    double *w, *eps, *a;         /* per volume: width, porosity, specific area (0 in the separator) */
    int *el;                     /* per volume: its electrode volume index, or -1 */
    double *ce, *ce_old;         /* electrolyte */
    double *cs, *cs_old;         /* particles, nr shells per electrode volume */
    double *u, *r, *r1, *J, *du; /* Newton */
    int *piv;
    /* the particle's response in the current step, per electrode (0 negative, 1 positive): the surface concentration's
     * value with no flux, per volume, and its change per unit reaction current */
    double *c0surf, gsurf[2], *c0shell, gshell[2][64];
    double T, dt, I, V, heat, ueff, t, charge, q[4];
};

static const DfnElectrode *elec(const Dfn *D, int e) { return e ? &D->s.pos : &D->s.neg; }
static int side_of(const Dfn *D, int k) { return k >= D->nn; } /* electrode volume k: 0 negative, 1 positive */

void dfn_free(Dfn *D) {
    if (!D) return;
    free(D->w), free(D->eps), free(D->a), free(D->el), free(D->ce), free(D->ce_old), free(D->cs), free(D->cs_old);
    free(D->u), free(D->r), free(D->r1), free(D->J), free(D->du), free(D->piv), free(D->c0surf), free(D->c0shell);
    free(D);
}

Dfn *dfn_create(const DfnSpec *s, char *err, size_t errlen) {
    if (s->neg.n < 2 || s->pos.n < 2 || s->nsep < 1 || s->nr < 3 || s->nr > 64 || !s->neg.U || !s->pos.U || !s->De || !s->kappa || !(s->area > 0)) {
        snprintf(err, errlen, "dfn: volumes (2 or more per electrode, 1 or more in the separator), 3 to 64 shells, the open-circuit "
                              "potentials, the electrolyte's properties and an area are required");
        return NULL;
    }
    Dfn *D = calloc(1, sizeof *D);
    if (!D) return NULL;
    D->s = *s;
    D->nn = s->neg.n, D->ns = s->nsep, D->np = s->pos.n, D->nr = s->nr;
    D->N = D->nn + D->ns + D->np, D->Ne = D->nn + D->np;
    D->nu = 2 * D->N + 2 * D->Ne;
    int N = D->N, Ne = D->Ne, nu = D->nu;
    D->w = malloc(N * 8), D->eps = malloc(N * 8), D->a = malloc(N * 8), D->el = malloc(N * sizeof(int));
    D->ce = malloc(N * 8), D->ce_old = malloc(N * 8), D->cs = malloc((size_t)Ne * D->nr * 8), D->cs_old = malloc((size_t)Ne * D->nr * 8);
    D->u = malloc(nu * 8), D->r = malloc(nu * 8), D->r1 = malloc(nu * 8), D->J = malloc((size_t)nu * nu * 8), D->du = malloc(nu * 8);
    D->piv = malloc(nu * sizeof(int)), D->c0surf = malloc(Ne * 8), D->c0shell = malloc((size_t)Ne * D->nr * 8);
    if (!D->w || !D->eps || !D->a || !D->el || !D->ce || !D->ce_old || !D->cs || !D->cs_old || !D->u || !D->r || !D->r1 || !D->J || !D->du || !D->piv ||
        !D->c0surf || !D->c0shell) {
        snprintf(err, errlen, "dfn: out of memory");
        dfn_free(D);
        return NULL;
    }
    for (int i = 0; i < N; i++) {
        if (i < D->nn) D->w[i] = s->neg.L / D->nn, D->eps[i] = s->neg.eps_e, D->a[i] = 3 * s->neg.eps_s / s->neg.R, D->el[i] = i;
        else if (i < D->nn + D->ns) D->w[i] = s->Lsep / D->ns, D->eps[i] = s->eps_sep, D->a[i] = 0, D->el[i] = -1;
        else D->w[i] = s->pos.L / D->np, D->eps[i] = s->pos.eps_e, D->a[i] = 3 * s->pos.eps_s / s->pos.R, D->el[i] = D->nn + (i - D->nn - D->ns);
        D->ce[i] = s->ce0;
    }
    for (int k = 0; k < Ne; k++)
        for (int m = 0; m < D->nr; m++) D->cs[(size_t)k * D->nr + m] = elec(D, side_of(D, k))->c0;
    /* the first guess: at rest */
    double Un = s->neg.U(s->neg.c0 / s->neg.cmax), Up = s->pos.U(s->pos.c0 / s->pos.cmax);
    for (int i = 0; i < N; i++) D->u[i] = s->ce0, D->u[N + i] = -Un;
    for (int k = 0; k < Ne; k++) D->u[2 * N + k] = side_of(D, k) ? Up - Un : 0, D->u[2 * N + Ne + k] = 0;
    D->V = D->ueff = Up - Un, D->T = s->Tref;
    return D;
}

/* ---- the particles --------------------------------------------------------------------------------------------- */

/* backward Euler in shells of equal thickness: solves the tridiagonal system for right side b (in place) */
static void particle_solve(double R, double Ds, int nr, double dt, double *b) {
    double dr = R / nr, cp[64], dp[64];
    for (int m = 0; m < nr; m++) {
        double vol = 4.0 / 3 * M_PI * (pow((m + 1) * dr, 3) - pow(m * dr, 3));
        double al = m > 0 ? 4 * M_PI * pow(m * dr, 2) * Ds / dr : 0, ar = m < nr - 1 ? 4 * M_PI * pow((m + 1) * dr, 2) * Ds / dr : 0;
        double diag = vol / dt + al + ar, lower = -al, upper = -ar;
        /* Thomas */
        double den = diag - (m > 0 ? lower * cp[m - 1] : 0);
        cp[m] = upper / den;
        dp[m] = (b[m] - (m > 0 ? lower * dp[m - 1] : 0)) / den;
    }
    for (int m = nr - 1; m >= 0; m--) b[m] = dp[m] - (m < nr - 1 ? cp[m] * b[m + 1] : 0);
}
static double shell_volume(double R, int nr, int m) {
    double dr = R / nr;
    return 4.0 / 3 * M_PI * (pow((m + 1) * dr, 3) - pow(m * dr, 3));
}
/* the particles' responses for this step: with no flux, and to a unit reaction current (A/m^2) leaving the surface */
static void particle_prepare(Dfn *D, double dt) {
    const int nr = D->nr;
    for (int e = 0; e < 2; e++) {
        const DfnElectrode *E = elec(D, e);
        double b[64] = {0};
        b[nr - 1] = -4 * M_PI * E->R * E->R / FA; /* mol/s leaving the outer shell per unit current density */
        particle_solve(E->R, E->Ds, nr, dt, b);
        for (int m = 0; m < nr; m++) D->gshell[e][m] = b[m];
        D->gsurf[e] = b[nr - 1] - (E->R / nr / 2) / (FA * E->Ds); /* the surface, half a shell out: its gradient is -j / (F Ds) */
    }
    for (int k = 0; k < D->Ne; k++) {
        const DfnElectrode *E = elec(D, side_of(D, k));
        double *c = D->c0shell + (size_t)k * nr;
        for (int m = 0; m < nr; m++) c[m] = shell_volume(E->R, nr, m) / dt * D->cs_old[(size_t)k * nr + m];
        particle_solve(E->R, E->Ds, nr, dt, c);
        D->c0surf[k] = c[nr - 1];
    }
}

/* ---- the residual ---------------------------------------------------------------------------------------------- */

typedef struct {
    double *Ge, *Gk;  /* face conductances of the electrolyte (diffusion m/s, ionic S/m^2), N + 1 faces */
} Faces;

static double eff(const Dfn *D, double v, int i) { return v * pow(D->eps[i], D->s.brug); }

/* face i sits between volumes i - 1 and i; the two outer faces carry nothing */
static double face_g(const Dfn *D, double vl, int il, double vr, int ir) {
    return 1 / (D->w[il] / (2 * vl) + D->w[ir] / (2 * vr));
}

static void residual(Dfn *D, const double *u, double *r, double *heat_parts, double *ueff) {
    const int N = D->N, Ne = D->Ne, nn = D->nn;
    const double T = D->T, dt = D->dt, I = D->I, tp = D->s.tplus, RT = RG * T;
    const double *ce = u, *pe = u + N, *ps = u + 2 * N, *jj = u + 2 * N + Ne;
    double *rce = r, *rpe = r + N, *rps = r + 2 * N, *rj = r + 2 * N + Ne;
    double qr = 0, qs = 0, qe = 0, qv = 0, sumU = 0;
    for (int i = 0; i < N; i++) {
        rce[i] = D->eps[i] * D->w[i] * (ce[i] - D->ce_old[i]) / dt;
        rpe[i] = 0;
    }
    /* the electrolyte's faces */
    for (int f = 1; f < N; f++) {
        int l = f - 1, rr = f;
        double cl = fmax(ce[l], 1e-6), cr = fmax(ce[rr], 1e-6), cf = 0.5 * (cl + cr);
        double Gd = face_g(D, eff(D, D->s.De(cf, T), l), l, eff(D, D->s.De(cf, T), rr), rr);
        double Gk = face_g(D, eff(D, D->s.kappa(cl, T), l), l, eff(D, D->s.kappa(cr, T), rr), rr);
        double Nf = -Gd * (cr - cl); /* diffusion flux, mol/m^2/s */
        double ie = -Gk * (pe[rr] - pe[l]) + Gk * 2 * RT / FA * (1 - tp) * (log(cr) - log(cl));
        rce[l] += Nf, rce[rr] -= Nf;
        rpe[l] += ie, rpe[rr] -= ie;
        qe += ie * (pe[l] - pe[rr]);
    }
    /* the solid: the negative's collector at zero on its outer face, the positive's collector drawing I */
    for (int k = 0; k < Ne; k++) rps[k] = 0;
    for (int e = 0; e < 2; e++) {
        const DfnElectrode *E = elec(D, e);
        int k0 = e ? nn : 0, n = e ? D->np : nn, i0 = e ? nn + D->ns : 0;
        double h = E->L / n, G = E->sigma / h;
        for (int q = 0; q < n - 1; q++) {
            double is = -G * (ps[k0 + q + 1] - ps[k0 + q]);
            rps[k0 + q] += is, rps[k0 + q + 1] -= is; /* (is_right - is_left) per volume */
            qs += is * (ps[k0 + q] - ps[k0 + q + 1]);
        }
        if (!e) { /* x = 0: the collector at zero, half a volume away */
            double is0 = -E->sigma / (h / 2) * (ps[0] - 0);
            rps[0] -= is0;
            qs += is0 * (0 - ps[0]);
        } else { /* x = L: the current I leaves, the terminal half a volume beyond the last */
            rps[k0 + n - 1] += I;
            qs += I * I * (h / 2) / E->sigma;
        }
        for (int q = 0; q < n; q++) {
            int k = k0 + q, i = i0 + q;
            double ai = D->a[i], w = D->w[i], j = jj[k];
            rce[i] -= (1 - tp) * ai * j * w / FA;
            rpe[i] -= ai * j * w;
            rps[k] += ai * j * w;
            /* the kinetics */
            double csurf = D->c0surf[k] + D->gsurf[e] * j, sto = csurf / E->cmax;
            double U = E->U(sto), eta = ps[k] - pe[i] - U;
            double cs = fmin(fmax(csurf, 1e-9 * E->cmax), E->cmax * (1 - 1e-9));
            double i0c = E->k0 * exp(E->Ea / RG * (1 / D->s.Tref - 1 / T)) * sqrt(fmax(ce[i], 1e-9) * cs * (E->cmax - cs));
            rj[k] = j - 2 * i0c * sinh(FA * eta / (2 * RT));
            qr += ai * j * eta * w, qv += ai * j * T * E->dUdT * w, sumU += ai * j * U * w;
        }
    }
    if (heat_parts) heat_parts[0] = qr, heat_parts[1] = qs, heat_parts[2] = qe, heat_parts[3] = qv;
    if (ueff) *ueff = I != 0 ? -sumU / I : NAN;
}

/* LU with partial pivoting, in place; false if singular */
static bool lu(double *A, int n, int *piv) {
    for (int k = 0; k < n; k++) {
        int p = k;
        double m = fabs(A[(size_t)k * n + k]);
        for (int i = k + 1; i < n; i++)
            if (fabs(A[(size_t)i * n + k]) > m) m = fabs(A[(size_t)i * n + k]), p = i;
        if (m == 0) return false;
        piv[k] = p;
        if (p != k)
            for (int j = 0; j < n; j++) {
                double t = A[(size_t)k * n + j];
                A[(size_t)k * n + j] = A[(size_t)p * n + j], A[(size_t)p * n + j] = t;
            }
        double d = A[(size_t)k * n + k];
        for (int i = k + 1; i < n; i++) {
            double f = A[(size_t)i * n + k] / d;
            if (f == 0) continue;
            A[(size_t)i * n + k] = f;
            for (int j = k + 1; j < n; j++) A[(size_t)i * n + j] -= f * A[(size_t)k * n + j];
        }
    }
    return true;
}
/* the factors hold whole-row interchanges (their multipliers moved with the rows), so the right side takes every
 * interchange first, then the forward substitution */
static void lu_solve(const double *A, int n, const int *piv, double *b) {
    for (int k = 0; k < n; k++)
        if (piv[k] != k) {
            double t = b[k];
            b[k] = b[piv[k]], b[piv[k]] = t;
        }
    for (int k = 0; k < n; k++)
        for (int i = k + 1; i < n; i++) b[i] -= A[(size_t)i * n + k] * b[k];
    for (int i = n - 1; i >= 0; i--) {
        double s = b[i];
        for (int j = i + 1; j < n; j++) s -= A[(size_t)i * n + j] * b[j];
        b[i] = s / A[(size_t)i * n + i];
    }
}

/* one backward-Euler step by Newton's method */
static bool step_once(Dfn *D, double I, double T, double dt) {
    const int nu = D->nu, N = D->N, Ne = D->Ne;
    memcpy(D->ce_old, D->ce, N * 8), memcpy(D->cs_old, D->cs, (size_t)Ne * D->nr * 8);
    D->I = I / D->s.area, D->T = T, D->dt = dt;
    particle_prepare(D, dt);
    double *u = D->u, saved[4096];
    if (nu > 4096) return false;
    memcpy(saved, u, nu * 8);
    /* the scales of the unknowns: concentration, potential, reaction current */
    const double jsc = fmax(1.0, fabs(D->I) / (D->a[0] * D->s.neg.L));
    bool ok = false;
    for (int it = 0; it < 40; it++) {
        residual(D, u, D->r, NULL, NULL);
        for (int c = 0; c < nu; c++) {
            double sc = c < N ? D->s.ce0 : c < 2 * N + Ne ? 1.0 : jsc;
            double h = 1e-7 * fmax(fabs(u[c]), sc), keep = u[c];
            u[c] = keep + h;
            residual(D, u, D->r1, NULL, NULL);
            u[c] = keep;
            for (int rr = 0; rr < nu; rr++) D->J[(size_t)rr * nu + c] = (D->r1[rr] - D->r[rr]) / h;
        }
        if (!lu(D->J, nu, D->piv)) {
            if (getenv("DFN_DEBUG")) fprintf(stderr, "dfn: singular Jacobian\n");
            break;
        }
        for (int c = 0; c < nu; c++) D->du[c] = -D->r[c];
        lu_solve(D->J, nu, D->piv, D->du);
        double worst = 0;
        for (int c = 0; c < nu; c++) {
            double sc = c < N ? D->s.ce0 : c < 2 * N + Ne ? 1.0 : jsc;
            /* damped: no concentration below a tenth of itself in one iteration, no potential by more than 0.2 V */
            double d = D->du[c];
            if (c < N && u[c] + d < 0.1 * u[c]) d = -0.9 * u[c];
            if (c >= N && c < 2 * N + Ne) d = fmax(-0.2, fmin(0.2, d));
            u[c] += d;
            worst = fmax(worst, fabs(d) / sc);
        }
        if (getenv("DFN_DEBUG")) {
            double rn = 0;
            for (int c = 0; c < nu; c++) rn = fmax(rn, fabs(D->r[c]));
            int wc = 0;
            for (int c = 0; c < nu; c++)
                if (fabs(D->du[c]) > fabs(D->du[wc])) wc = c;
            fprintf(stderr, "dfn: iteration %d update %.3e residual %.3e (largest raw update %.3e at unknown %d of %d; N %d Ne %d)\n", it, worst, rn, D->du[wc], wc, nu, N, Ne);
        }
        if (!isfinite(worst)) break;
        if (worst < 1e-11) {
            ok = true;
            break;
        }
    }
    /* the particles' surfaces must stay inside their range */
    for (int k = 0; ok && k < Ne; k++) {
        const DfnElectrode *E = elec(D, side_of(D, k));
        double cs = D->c0surf[k] + D->gsurf[side_of(D, k)] * u[2 * N + Ne + k];
        if (!(cs > 0 && cs < E->cmax)) ok = false;
    }
    if (!ok) {
        memcpy(u, saved, nu * 8), memcpy(D->ce, D->ce_old, N * 8), memcpy(D->cs, D->cs_old, (size_t)Ne * D->nr * 8);
        return false;
    }
    memcpy(D->ce, u, N * 8);
    for (int k = 0; k < Ne; k++) {
        int e = side_of(D, k);
        for (int m = 0; m < D->nr; m++) D->cs[(size_t)k * D->nr + m] = D->c0shell[(size_t)k * D->nr + m] + D->gshell[e][m] * u[2 * N + Ne + k];
    }
    residual(D, u, D->r, D->q, &D->ueff);
    const DfnElectrode *P = &D->s.pos;
    D->V = u[2 * N + Ne - 1] - D->I * (P->L / D->np / 2) / P->sigma;
    for (int k = 0; k < 4; k++) D->q[k] *= D->s.area;
    D->heat = D->q[0] + D->q[1] + D->q[2] + D->q[3];
    if (D->I == 0) D->ueff = D->V;
    D->t += dt, D->charge += I * dt;
    return true;
}

bool dfn_step(Dfn *D, double I, double T, double dt) {
    if (step_once(D, I, T, dt)) return true;
    /* halve the step, up to five times, from the same state */
    const size_t ns = (size_t)D->Ne * D->nr;
    double *ce = malloc(D->N * 8), *cs = malloc(ns * 8), *u = malloc(D->nu * 8), t = D->t, q = D->charge;
    bool ok = false;
    if (ce && cs && u) {
        memcpy(ce, D->ce, D->N * 8), memcpy(cs, D->cs, ns * 8), memcpy(u, D->u, D->nu * 8);
        for (int depth = 1; depth <= 5 && !ok; depth++) {
            int n = 1 << depth;
            ok = true;
            for (int k = 0; k < n && ok; k++) ok = step_once(D, I, T, dt / n);
            if (!ok) memcpy(D->ce, ce, D->N * 8), memcpy(D->cs, cs, ns * 8), memcpy(D->u, u, D->nu * 8), D->t = t, D->charge = q;
        }
    }
    free(ce), free(cs), free(u);
    return ok;
}

bool dfn_try(Dfn *D, double I, double T, double dt, double *V) {
    const size_t ns = (size_t)D->Ne * D->nr;
    double *ce = malloc(D->N * 8), *cs = malloc(ns * 8), *u = malloc(D->nu * 8);
    double keep[12] = {D->t, D->charge, D->V, D->heat, D->ueff, D->q[0], D->q[1], D->q[2], D->q[3], D->I, D->T, D->dt};
    bool ok = false;
    if (ce && cs && u) {
        memcpy(ce, D->ce, D->N * 8), memcpy(cs, D->cs, ns * 8), memcpy(u, D->u, D->nu * 8);
        ok = dfn_step(D, I, T, dt);
        if (ok) *V = D->V;
        memcpy(D->ce, ce, D->N * 8), memcpy(D->cs, cs, ns * 8), memcpy(D->u, u, D->nu * 8);
        D->t = keep[0], D->charge = keep[1], D->V = keep[2], D->heat = keep[3], D->ueff = keep[4];
        for (int k = 0; k < 4; k++) D->q[k] = keep[5 + k];
        D->I = keep[9], D->T = keep[10], D->dt = keep[11];
    }
    free(ce), free(cs), free(u);
    return ok;
}

double dfn_voltage(const Dfn *D) { return D->V; }
double dfn_heat(const Dfn *D) { return D->heat; }
double dfn_ocv(const Dfn *D) { return D->ueff; }
double dfn_time(const Dfn *D) { return D->t; }
void dfn_heat_parts(const Dfn *D, double q[4]) { memcpy(q, D->q, sizeof D->q); }

void dfn_inventory(const Dfn *D, double *li_solid, double *li_e, double *li_neg, double *charge) {
    double s = 0, n = 0, e = 0;
    for (int k = 0; k < D->Ne; k++) {
        int side = side_of(D, k), i = side ? D->nn + D->ns + (k - D->nn) : k;
        const DfnElectrode *E = elec(D, side);
        double sum = 0;
        for (int m = 0; m < D->nr; m++) sum += shell_volume(E->R, D->nr, m) * D->cs[(size_t)k * D->nr + m];
        double avg = sum / (4.0 / 3 * M_PI * pow(E->R, 3)), li = E->eps_s * D->w[i] * D->s.area * avg;
        s += li;
        if (!side) n += li;
    }
    for (int i = 0; i < D->N; i++) e += D->eps[i] * D->w[i] * D->s.area * D->ce[i];
    if (li_solid) *li_solid = s;
    if (li_e) *li_e = e;
    if (li_neg) *li_neg = n;
    if (charge) *charge = D->charge;
}

void dfn_stoichiometry(const Dfn *D, double *neg, double *pos) {
    double sn = 0, sp = 0;
    for (int k = 0; k < D->Ne; k++) {
        const DfnElectrode *E = elec(D, side_of(D, k));
        double sum = 0;
        for (int m = 0; m < D->nr; m++) sum += shell_volume(E->R, D->nr, m) * D->cs[(size_t)k * D->nr + m];
        double sto = sum / (4.0 / 3 * M_PI * pow(E->R, 3)) / E->cmax;
        if (side_of(D, k)) sp += sto / D->np;
        else sn += sto / D->nn;
    }
    if (neg) *neg = sn;
    if (pos) *pos = sp;
}

double dfn_particle_surface(double R, double Ds, double c0, double flux, double t, double dt, int nr) {
    double c[64];
    if (nr > 64) return NAN;
    for (int m = 0; m < nr; m++) c[m] = c0;
    int steps = (int)lround(t / dt);
    for (int s = 0; s < steps; s++) {
        for (int m = 0; m < nr; m++) c[m] *= shell_volume(R, nr, m) / dt;
        c[nr - 1] -= 4 * M_PI * R * R * flux;
        particle_solve(R, Ds, nr, dt, c);
    }
    return c[nr - 1] - flux * (R / nr / 2) / Ds;
}
