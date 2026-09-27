/* sheet.c - neo-Hookean membranes with mid-edge shell bending and contact (sheet.h). */
#include "sheet.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int a, b, c, d;         /* the hinge's edge (a, b) and the far corners c (of triangle a b c) and d (of b a d) */
    int t1, k1, t2, k2;     /* its two triangles and the edge's place in each */
    double theta0;          /* the rest angle */
} Hinge;

struct Sheet {
    SheetSpec s;
    int n, nt, nh;
    double *X, *x, *v, *m, *f;   /* reference and current positions, velocities, masses, forces */
    int *tri;
    double *Dinv, *A0;           /* per triangle: the inverse reference shape (2 x 2) and area */
    Hinge *h;
    double *tf, *hf;             /* per-triangle and per-hinge force contributions */
    int *adj_start, *adj;        /* per node: (triangle * 3 + corner) and (hinge * 4 + corner + BIG) entries */
    unsigned char *pinned;
    SheetSdf sdf;
    void *sdf_ctx;
    double D;                    /* bending rigidity */
    int *te;                     /* per triangle edge k (nodes k, k + 1): its hinge, or -1 on a free edge */
    double *Q, *tg;              /* per triangle: the bending form in its three edges' angles (3 x 3), dE/dtheta (3) */
    double *th, *thg;            /* per hinge: the angle less the rest angle, and its gradient (12) */
    bool forces_ready;           /* S->f holds the forces of the current positions */
    bool forms_stale;
    double *fext, *narea;        /* external forces (3 per node) when set; node areas */
    int *hstart, *hfill, *hlist, hcap; /* self contact: a spatial hash of the triangles */
    unsigned hmask;
    double hc, dt_s;             /* its cell, the stable step (for the contact's stiffness) */            /* a pin changed which triangles are held: rebuild the bending forms */
};

enum { HOFF = 1 << 29 };

typedef struct {
    long long key;
    int who;
} EdgeRef;
static int cmp_edge(const void *a, const void *b) {
    long long x = ((const EdgeRef *)a)->key, y = ((const EdgeRef *)b)->key;
    return (x > y) - (x < y);
}

static void sub(const double *a, const double *b, double *o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static void cross(const double *a, const double *b, double *o) { o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0]; }
static double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* the dihedral angle at a hinge: the signed angle from the first face's normal to the second's, about the edge */
static double dihedral(const double *xa, const double *xb, const double *xc, const double *xd) {
    double e[3], u[3], w[3], n1[3], n2[3], m[3];
    sub(xb, xa, e), sub(xc, xa, u), sub(xd, xa, w);
    cross(e, u, n1), cross(w, e, n2);
    cross(n1, n2, m);
    double el = sqrt(dot(e, e));
    return atan2(dot(m, e) / (el > 0 ? el : 1), dot(n1, n2));
}

/* Bending: each triangle's shape operator from its edges' dihedral angles (the mid-edge normal model: the normal
 * interpolated between edge midpoints, where it is tilted by half the dihedral angle),
 *
 *   M = sum_k (theta_k - theta0_k) |e_k| / (2 A) t_k t_k^T        (t_k the edge's in-plane unit normal)
 *
 * and the plate's energy A D / 2 (nu (tr M)^2 + (1 - nu) tr M^2) with nu = 1/2, a quadratic form in the three angles.
 * It is isotropic on any mesh and exact for a cylinder on a regular grid in every direction; it sees saddles (which
 * the squared-Laplacian energy does not). A free edge's angle is not defined: its mid-edge normal is free, and the form
 * is minimised over it (a Schur complement per triangle), which leaves the anticlastic freedom of a plate's free edge.
 * An edge against a held (fully pinned) triangle is clamped (below). The first model, a hinge energy per edge with the weights 3 |e|^2 / (A1 + A2), measured 1.46 times stiff along a
 * right-triangle grid and 4.4 times across it (2026-09-26), and was replaced. */
static bool held(const Sheet *S, int t) { return S->pinned[S->tri[3 * t]] && S->pinned[S->tri[3 * t + 1]] && S->pinned[S->tri[3 * t + 2]]; }

static void bending_form(Sheet *S, const double *xyz, int t) {
    const int *T = &S->tri[3 * t];
    double e[3][3], n[3], tk[3][3], c[3], A = S->A0[t], Q[9];
    if (held(S, t)) { /* part of a clamp: no energy of its own */
        memset(&S->Q[9 * t], 0, sizeof Q);
        return;
    }
    for (int k = 0; k < 3; k++) sub(&xyz[3 * T[(k + 1) % 3]], &xyz[3 * T[k]], e[k]);
    cross(e[0], e[1], n);
    double nl = sqrt(dot(n, n));
    for (int k = 0; k < 3; k++) n[k] /= nl;
    for (int k = 0; k < 3; k++) {
        double l = sqrt(dot(e[k], e[k]));
        cross(e[k], n, tk[k]);
        for (int j = 0; j < 3; j++) tk[k][j] /= l;
        c[k] = l / (2 * A);
        /* an edge against a held triangle is clamped: its mid-edge normal is the held triangle's, tilted from this one's
         * by the whole angle, not half (without this a clamp sits half a cell out: first-order error, sheettest C2) */
        int h = S->te[3 * t + k];
        if (h >= 0 && held(S, S->h[h].t1 == t ? S->h[h].t2 : S->h[h].t1)) c[k] *= 2;
    }
    const double nu = 0.5;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double tt = dot(tk[i], tk[j]);
            Q[3 * i + j] = A * S->D * c[i] * c[j] * (nu + (1 - nu) * tt * tt);
        }
    /* eliminate the free edges: Q_II - Q_IB Q_BB^-1 Q_BI */
    int B[3], nb = 0;
    for (int k = 0; k < 3; k++)
        if (S->te[3 * t + k] < 0) B[nb++] = k;
    if (nb == 1) {
        int b = B[0];
        double qbb = Q[3 * b + b], R[9];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) R[3 * i + j] = Q[3 * i + j] - Q[3 * i + b] * Q[3 * b + j] / qbb;
        memcpy(Q, R, sizeof R);
    } else if (nb == 2) {
        int b0 = B[0], b1 = B[1];
        double a = Q[3 * b0 + b0], bb = Q[3 * b0 + b1], d = Q[3 * b1 + b1], det = a * d - bb * bb, R[9];
        double i00 = d / det, i01 = -bb / det, i11 = a / det;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                double qi0 = Q[3 * i + b0], qi1 = Q[3 * i + b1], q0j = Q[3 * b0 + j], q1j = Q[3 * b1 + j];
                R[3 * i + j] = Q[3 * i + j] - (qi0 * (i00 * q0j + i01 * q1j) + qi1 * (i01 * q0j + i11 * q1j));
            }
        memcpy(Q, R, sizeof R);
    } else if (nb == 3)
        memset(Q, 0, sizeof Q);
    for (int k = 0; k < nb; k++)
        for (int j = 0; j < 3; j++) Q[3 * B[k] + j] = Q[3 * j + B[k]] = 0;
    memcpy(&S->Q[9 * t], Q, sizeof Q);
}

Sheet *sheet_create(const SheetSpec *s, int nn, const double *xyz, int nt, const int *tri, char *err, size_t errlen) {
    if (nn < 3 || nt < 1 || !(s->shear_modulus > 0) || !(s->thickness > 0) || !(s->density > 0)) {
        snprintf(err, errlen, "sheet: need nodes, triangles and a positive shear modulus, thickness and density");
        return NULL;
    }
    Sheet *S = calloc(1, sizeof *S);
    if (!S) return NULL;
    S->s = *s, S->n = nn, S->nt = nt;
    S->X = malloc(3 * (size_t)nn * sizeof(double)), S->x = malloc(3 * (size_t)nn * sizeof(double)), S->v = calloc(3 * (size_t)nn, sizeof(double));
    S->m = calloc((size_t)nn, sizeof(double)), S->f = calloc(3 * (size_t)nn, sizeof(double)), S->pinned = calloc((size_t)nn, 1);
    S->tri = malloc(3 * (size_t)nt * sizeof(int)), S->Dinv = malloc(4 * (size_t)nt * sizeof(double)), S->A0 = malloc((size_t)nt * sizeof(double));
    S->tf = malloc(9 * (size_t)nt * sizeof(double));
    if (!S->X || !S->x || !S->v || !S->m || !S->f || !S->pinned || !S->tri || !S->Dinv || !S->A0 || !S->tf) goto oom;
    memcpy(S->X, xyz, 3 * (size_t)nn * sizeof(double)), memcpy(S->x, xyz, 3 * (size_t)nn * sizeof(double)), memcpy(S->tri, tri, 3 * (size_t)nt * sizeof(int));
    const double mu = s->shear_modulus, H = s->thickness;
    S->D = (s->bending_scale > 0 ? s->bending_scale : 1) * mu * H * H * H / 3; /* E H^3 / (12 (1 - nu^2)), E = 3 mu, nu = 1/2 */
    for (int t = 0; t < nt; t++) {
        const double *p0 = &xyz[3 * tri[3 * t]], *p1 = &xyz[3 * tri[3 * t + 1]], *p2 = &xyz[3 * tri[3 * t + 2]];
        double u[3], w[3], nrm[3], e1[3], e2[3];
        sub(p1, p0, u), sub(p2, p0, w), cross(u, w, nrm);
        double area = 0.5 * sqrt(dot(nrm, nrm)), ul = sqrt(dot(u, u));
        if (!(area > 0)) {
            snprintf(err, errlen, "sheet: triangle %d has no area", t);
            sheet_free(S);
            return NULL;
        }
        for (int k = 0; k < 3; k++) e1[k] = u[k] / ul, nrm[k] /= 2 * area;
        cross(nrm, e1, e2);
        double a = dot(u, e1), b = dot(w, e1), c = dot(u, e2), d = dot(w, e2), det = a * d - b * c; /* Dm = [[a, b], [c, d]] */
        double *Di = &S->Dinv[4 * t];
        Di[0] = d / det, Di[1] = -b / det, Di[2] = -c / det, Di[3] = a / det;
        S->A0[t] = area;
        for (int k = 0; k < 3; k++) S->m[tri[3 * t + k]] += s->density * H * area / 3;
    }
    /* hinges: interior edges, found by sorting the triangles' edges */
    {
        long ne = 3L * nt;
        EdgeRef *E = malloc((size_t)ne * sizeof *E);
        S->h = malloc(sizeof(Hinge) * (size_t)(ne / 2 + 1));
        S->te = malloc(3 * (size_t)nt * sizeof(int)), S->Q = malloc(9 * (size_t)nt * sizeof(double)), S->tg = calloc(3 * (size_t)nt, sizeof(double));
        if (!E || !S->h || !S->te || !S->Q || !S->tg) { free(E); goto oom; }
        for (long i = 0; i < ne; i++) S->te[i] = -1;
        for (int t = 0; t < nt; t++)
            for (int k = 0; k < 3; k++) {
                int a = tri[3 * t + k], b = tri[3 * t + (k + 1) % 3];
                E[3 * t + k].key = (long long)(a < b ? a : b) * nn + (a < b ? b : a), E[3 * t + k].who = 3 * t + k;
            }
        qsort(E, (size_t)ne, sizeof *E, cmp_edge);
        for (long i = 0; i + 1 < ne; i++) {
            if (E[i].key != E[i + 1].key) continue;
            int w1 = E[i].who, w2 = E[i + 1].who, t1 = w1 / 3, k1 = w1 % 3, t2 = w2 / 3, k2 = w2 % 3;
            Hinge *hg = &S->h[S->nh++];
            hg->a = tri[3 * t1 + k1], hg->b = tri[3 * t1 + (k1 + 1) % 3], hg->c = tri[3 * t1 + (k1 + 2) % 3];
            hg->d = tri[3 * t2 + (k2 + 2) % 3];
            hg->t1 = t1, hg->k1 = k1, hg->t2 = t2, hg->k2 = k2;
            S->te[3 * t1 + k1] = S->te[3 * t2 + k2] = S->nh - 1;
            hg->theta0 = dihedral(&xyz[3 * hg->a], &xyz[3 * hg->b], &xyz[3 * hg->c], &xyz[3 * hg->d]);
            i++;
        }
        free(E);
        for (int t = 0; t < nt; t++) bending_form(S, xyz, t);
    }
    S->hf = malloc(12 * (size_t)(S->nh ? S->nh : 1) * sizeof(double));
    S->th = calloc((size_t)(S->nh ? S->nh : 1), sizeof(double)), S->thg = calloc(12 * (size_t)(S->nh ? S->nh : 1), sizeof(double));
    if (!S->th || !S->thg) goto oom;
    /* each node's contributions */
    S->adj_start = calloc((size_t)nn + 1, sizeof(int));
    if (!S->hf || !S->adj_start) goto oom;
    for (int t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++) S->adj_start[tri[3 * t + k] + 1]++;
    for (int h = 0; h < S->nh; h++) {
        int q[4] = {S->h[h].a, S->h[h].b, S->h[h].c, S->h[h].d};
        for (int k = 0; k < 4; k++) S->adj_start[q[k] + 1]++;
    }
    for (int i = 0; i < nn; i++) S->adj_start[i + 1] += S->adj_start[i];
    S->adj = malloc((size_t)S->adj_start[nn] * sizeof(int));
    int *fill = calloc((size_t)nn, sizeof(int));
    if (!S->adj || !fill) { free(fill); goto oom; }
    for (int t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++) {
            int i = tri[3 * t + k];
            S->adj[S->adj_start[i] + fill[i]++] = 3 * t + k;
        }
    for (int h = 0; h < S->nh; h++) {
        int q[4] = {S->h[h].a, S->h[h].b, S->h[h].c, S->h[h].d};
        for (int k = 0; k < 4; k++) S->adj[S->adj_start[q[k]] + fill[q[k]]++] = HOFF + 4 * h + k;
    }
    free(fill);
    S->dt_s = sheet_stable_dt(S);
    if (s->self_contact > 0) { /* the hash's cell: the larger of twice the distance and the mean edge */
        double le = 0;
        for (int t = 0; t < nt; t++) le += sqrt(2 * S->A0[t]);
        S->hc = fmax(2 * s->self_contact, le / nt);
        unsigned size = 1;
        while (size < 4u * (unsigned)nt) size <<= 1;
        S->hmask = size - 1, S->hcap = 8 * nt;
        S->hstart = calloc((size_t)size + 1, sizeof(int)), S->hfill = calloc(size, sizeof(int)), S->hlist = malloc((size_t)S->hcap * sizeof(int));
        if (!S->hstart || !S->hfill || !S->hlist) goto oom;
    }
    return S;
oom:
    snprintf(err, errlen, "sheet: out of memory");
    sheet_free(S);
    return NULL;
}

void sheet_free(Sheet *S) {
    if (!S) return;
    free(S->X), free(S->x), free(S->v), free(S->m), free(S->f), free(S->pinned), free(S->tri), free(S->Dinv), free(S->A0), free(S->tf), free(S->h);
    free(S->hf), free(S->adj_start), free(S->adj);
    free(S->te), free(S->Q), free(S->tg), free(S->th), free(S->thg), free(S->fext), free(S->narea);
    free(S->hstart), free(S->hfill), free(S->hlist), free(S);
}

void sheet_set_bodies(Sheet *S, SheetSdf sdf, void *ctx) { S->sdf = sdf, S->sdf_ctx = ctx; }
void sheet_pin(Sheet *S, int node) {
    if (node < 0 || node >= S->n) return;
    S->pinned[node] = 1, S->forms_stale = true, S->forces_ready = false;
    memset(&S->v[3 * node], 0, 3 * sizeof(double));
}
void sheet_set_pressure(Sheet *S, double p) { S->s.pressure = p, S->forces_ready = false; }
void sheet_set_external(Sheet *S, const double *f) {
    if (!f) {
        free(S->fext), S->fext = NULL;
    } else {
        if (!S->fext) S->fext = malloc(3 * (size_t)S->n * sizeof(double));
        if (S->fext) memcpy(S->fext, f, 3 * (size_t)S->n * sizeof(double));
    }
    S->forces_ready = false;
}
const double *sheet_node_areas(const Sheet *S) {
    Sheet *W = (Sheet *)S;
    if (!W->narea && (W->narea = calloc((size_t)S->n, sizeof(double))))
        for (int t = 0; t < S->nt; t++)
            for (int k = 0; k < 3; k++) W->narea[S->tri[3 * t + k]] += S->A0[t] / 3;
    return W->narea;
}
void sheet_save(const Sheet *S, double *st) {
    memcpy(st, S->x, 3 * (size_t)S->n * sizeof(double)), memcpy(st + 3 * (size_t)S->n, S->v, 3 * (size_t)S->n * sizeof(double));
}
void sheet_restore(Sheet *S, const double *st) {
    memcpy(S->x, st, 3 * (size_t)S->n * sizeof(double)), memcpy(S->v, st + 3 * (size_t)S->n, 3 * (size_t)S->n * sizeof(double));
    S->forces_ready = false;
}
bool sheet_pinned(const Sheet *S, int node) { return node >= 0 && node < S->n && S->pinned[node]; }
double sheet_node_mass(const Sheet *S, int node) { return node >= 0 && node < S->n ? S->m[node] : 0; }
int sheet_nodes(const Sheet *S) { return S->n; }
int sheet_triangles(const Sheet *S) { return S->nt; }
const int *sheet_triangle_nodes(const Sheet *S) { return S->tri; }
const double *sheet_positions(const Sheet *S) { return S->x; }
const double *sheet_velocities(const Sheet *S) { return S->v; }

double sheet_stable_dt(const Sheet *S) {
    /* the shortest altitude of the triangles that can move (a sliver's is far below the square root of its area) */
    double lmin = 1e300;
    for (int t = 0; t < S->nt; t++) {
        const int *T = &S->tri[3 * t];
        if (S->pinned[T[0]] && S->pinned[T[1]] && S->pinned[T[2]]) continue;
        double lmax = 0;
        for (int k = 0; k < 3; k++) {
            double e[3];
            sub(&S->X[3 * T[(k + 1) % 3]], &S->X[3 * T[k]], e);
            lmax = fmax(lmax, sqrt(dot(e, e)));
        }
        lmin = fmin(lmin, 2 * S->A0[t] / lmax);
    }
    if (lmin > 1e299) lmin = 1;
    double c = sqrt(4 * S->s.shear_modulus / S->s.density), dm = 0.3 * lmin / c;
    double db = 0.3 * lmin * lmin * sqrt(S->s.density * S->s.thickness / (S->D > 0 ? S->D : 1e-30)) / 2;
    return fmin(dm, db);
}

typedef struct {
    Sheet *S;
    double dt;
    int phase;
} Ctx;

/* the membrane's forces per triangle, and the pressure's */
static void tri_forces(void *vc, int t0, int t1, int tid) {
    Sheet *S = ((Ctx *)vc)->S;
    const double mu = S->s.shear_modulus, H = S->s.thickness, p = S->s.pressure, eta = S->s.viscosity;
    for (int t = t0; t < t1; t++) {
        const int *T = &S->tri[3 * t];
        const double *x0 = &S->x[3 * T[0]], *x1 = &S->x[3 * T[1]], *x2 = &S->x[3 * T[2]], *Di = &S->Dinv[4 * t];
        double d1[3], d2[3], F[3][2];
        sub(x1, x0, d1), sub(x2, x0, d2);
        for (int k = 0; k < 3; k++) F[k][0] = d1[k] * Di[0] + d2[k] * Di[2], F[k][1] = d1[k] * Di[1] + d2[k] * Di[3];
        double C00 = 0, C01 = 0, C11 = 0;
        for (int k = 0; k < 3; k++) C00 += F[k][0] * F[k][0], C01 += F[k][0] * F[k][1], C11 += F[k][1] * F[k][1];
        double J = C00 * C11 - C01 * C01;
        if (!(J > 1e-12)) J = 1e-12;
        /* dW/dC = mu / 2 (I - C^-1 / det C); C^-1 = [[C11, -C01], [-C01, C00]] / J */
        double S00 = 0.5 * mu * (1 - C11 / (J * J)), S01 = 0.5 * mu * (C01 / (J * J)), S11 = 0.5 * mu * (1 - C00 / (J * J));
        if (eta > 0) { /* Kelvin-Voigt: S += 2 eta dE/dt, dE/dt = sym(F^T dF/dt) */
            const double *v0 = &S->v[3 * T[0]], *v1 = &S->v[3 * T[1]], *v2 = &S->v[3 * T[2]];
            double w1[3], w2[3], G[3][2], E00 = 0, E01 = 0, E11 = 0;
            sub(v1, v0, w1), sub(v2, v0, w2);
            for (int k = 0; k < 3; k++) G[k][0] = w1[k] * Di[0] + w2[k] * Di[2], G[k][1] = w1[k] * Di[1] + w2[k] * Di[3];
            for (int k = 0; k < 3; k++) E00 += F[k][0] * G[k][0], E11 += F[k][1] * G[k][1], E01 += 0.5 * (F[k][0] * G[k][1] + F[k][1] * G[k][0]);
            S00 += 2 * eta * E00, S01 += 2 * eta * E01, S11 += 2 * eta * E11;
        }
        double P[3][2];
        for (int k = 0; k < 3; k++) P[k][0] = 2 * (F[k][0] * S00 + F[k][1] * S01), P[k][1] = 2 * (F[k][0] * S01 + F[k][1] * S11);
        double V = S->A0[t] * H, *out = &S->tf[9 * t];
        for (int k = 0; k < 3; k++) {
            double f1 = -V * (P[k][0] * Di[0] + P[k][1] * Di[1]), f2 = -V * (P[k][0] * Di[2] + P[k][1] * Di[3]);
            out[3 + k] = f1, out[6 + k] = f2, out[k] = -(f1 + f2);
        }
        if (p != 0) {
            double nrm[3];
            cross(d1, d2, nrm); /* 2 A n */
            for (int c = 0; c < 3; c++)
                for (int k = 0; k < 3; k++) out[3 * c + k] += p * nrm[k] / 6;
        }
    }
}

/* the angle at a hinge (less its rest angle) and the angle's gradient on a, b, c, d (Bridson et al. 2003) */
static double hinge_angle(const double *xa, const double *xb, const double *xc, const double *xd, double theta0, double *g) {
    double e[3], u[3], w[3], n1[3], n2[3];
    sub(xb, xa, e), sub(xc, xa, u), sub(xd, xa, w);
    cross(e, u, n1), cross(w, e, n2);
    double ee = dot(e, e), el = sqrt(ee), q1 = dot(n1, n1), q2 = dot(n2, n2);
    if (g) {
        double gc[3], gd[3], cb[3], db[3], ca[3], da[3];
        for (int k = 0; k < 3; k++) gc[k] = -el * n1[k] / q1, gd[k] = -el * n2[k] / q2;
        sub(xc, xb, cb), sub(xd, xb, db), sub(xc, xa, ca), sub(xd, xa, da);
        double sca = dot(cb, e) / ee, sda = dot(db, e) / ee, scb = dot(ca, e) / ee, sdb = dot(da, e) / ee;
        for (int k = 0; k < 3; k++) {
            g[0 + k] = sca * gc[k] + sda * gd[k];
            g[3 + k] = -scb * gc[k] - sdb * gd[k];
            g[6 + k] = gc[k], g[9 + k] = gd[k];
        }
    }
    double m[3];
    cross(n1, n2, m);
    double th = atan2(dot(m, e) / (el > 0 ? el : 1), dot(n1, n2)) - theta0;
    return th > M_PI ? th - 2 * M_PI : th < -M_PI ? th + 2 * M_PI : th;
}

static void hinge_angles(void *vc, int h0, int h1, int tid) {
    Sheet *S = ((Ctx *)vc)->S;
    for (int h = h0; h < h1; h++) {
        const Hinge *hg = &S->h[h];
        S->th[h] = hinge_angle(&S->x[3 * hg->a], &S->x[3 * hg->b], &S->x[3 * hg->c], &S->x[3 * hg->d], hg->theta0, &S->thg[12 * h]);
    }
}

/* per triangle, dE/dtheta of its edges: Q times the angles */
static void tri_bending(void *vc, int t0, int t1, int tid) {
    Sheet *S = ((Ctx *)vc)->S;
    for (int t = t0; t < t1; t++) {
        double a[3];
        for (int k = 0; k < 3; k++) a[k] = S->te[3 * t + k] >= 0 ? S->th[S->te[3 * t + k]] : 0;
        const double *Q = &S->Q[9 * t];
        for (int k = 0; k < 3; k++) S->tg[3 * t + k] = Q[3 * k] * a[0] + Q[3 * k + 1] * a[1] + Q[3 * k + 2] * a[2];
    }
}

static void hinge_forces(void *vc, int h0, int h1, int tid) {
    Sheet *S = ((Ctx *)vc)->S;
    for (int h = h0; h < h1; h++) {
        const Hinge *hg = &S->h[h];
        double G = S->tg[3 * hg->t1 + hg->k1] + S->tg[3 * hg->t2 + hg->k2];
        for (int k = 0; k < 12; k++) S->hf[12 * h + k] = -G * S->thg[12 * h + k];
    }
}

/* the forces on each node from its triangles and hinges, gravity; into S->f */
static void gather(void *vc, int i0, int i1, int tid) {
    Sheet *S = ((Ctx *)vc)->S;
    for (int i = i0; i < i1; i++) {
        double f[3] = {0, 0, -S->s.gravity * S->m[i]};
        if (S->fext) f[0] += S->fext[3 * i], f[1] += S->fext[3 * i + 1], f[2] += S->fext[3 * i + 2];
        for (int k = S->adj_start[i]; k < S->adj_start[i + 1]; k++) {
            int a = S->adj[k];
            const double *src = a >= HOFF ? &S->hf[3 * (a - HOFF)] : &S->tf[3 * a];
            f[0] += src[0], f[1] += src[1], f[2] += src[2];
        }
        memcpy(&S->f[3 * i], f, sizeof f);
    }
}

/* velocity Verlet: phase 0 a half kick and the drift (then contact), phase 1 the second half kick (then damping) */
static void node_update(void *vc, int i0, int i1, int tid) {
    Ctx *C = vc;
    Sheet *S = C->S;
    const double dt = C->dt, damp = S->s.damping, mu_f = S->s.friction, r = S->s.contact_distance > 0 ? S->s.contact_distance : 0.5 * S->s.thickness;
    for (int i = i0; i < i1; i++) {
        if (S->pinned[i]) continue;
        double *v = &S->v[3 * i], *x = &S->x[3 * i], *f = &S->f[3 * i];
        for (int k = 0; k < 3; k++) v[k] += 0.5 * dt * f[k] / S->m[i];
        if (C->phase == 1) {
            if (damp > 0)
                for (int k = 0; k < 3; k++) v[k] *= 1 - damp * dt;
            continue;
        }
        for (int k = 0; k < 3; k++) x[k] += dt * v[k];
        if (S->sdf) { /* contact: out of the body along its distance's gradient, the inward speed removed, friction */
            double d = S->sdf(x, S->sdf_ctx);
            if (d < r) {
                double gr[3], hh = 1e-5;
                for (int k = 0; k < 3; k++) {
                    double pa[3] = {x[0], x[1], x[2]}, pb[3] = {x[0], x[1], x[2]};
                    pa[k] += hh, pb[k] -= hh;
                    gr[k] = (S->sdf(pa, S->sdf_ctx) - S->sdf(pb, S->sdf_ctx)) / (2 * hh);
                }
                double gl = sqrt(dot(gr, gr));
                if (gl > 0) {
                    for (int k = 0; k < 3; k++) gr[k] /= gl, x[k] += (r - d) * gr[k];
                    double vn = dot(v, gr);
                    if (vn < 0) {
                        double vt[3], vtl;
                        for (int k = 0; k < 3; k++) vt[k] = v[k] - vn * gr[k];
                        vtl = sqrt(dot(vt, vt));
                        double keep = vtl > 0 ? fmax(0, 1 - mu_f * (-vn) / vtl) : 0;
                        for (int k = 0; k < 3; k++) v[k] = keep * vt[k];
                    }
                }
            }
        }
    }
}

/* the closest point of triangle abc to p (Ericson, Real-Time Collision Detection, 5.1.5), its barycentric weights */
static void closest_tri(const double *p, const double *a, const double *b, const double *c, double *q, double *w) {
    double ab[3], ac[3], ap[3], bp[3], cp[3];
    sub(b, a, ab), sub(c, a, ac), sub(p, a, ap);
    double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { w[0] = 1, w[1] = w[2] = 0; goto done; }
    sub(p, b, bp);
    double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { w[1] = 1, w[0] = w[2] = 0; goto done; }
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { double v = d1 / (d1 - d3); w[0] = 1 - v, w[1] = v, w[2] = 0; goto done; }
    sub(p, c, cp);
    double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { w[2] = 1, w[0] = w[1] = 0; goto done; }
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { double v = d2 / (d2 - d6); w[0] = 1 - v, w[2] = v, w[1] = 0; goto done; }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { double v = (d4 - d3) / ((d4 - d3) + (d5 - d6)); w[1] = 1 - v, w[2] = v, w[0] = 0; goto done; }
    {
        double den = 1 / (va + vb + vc), v = vb * den, u = vc * den;
        w[0] = 1 - v - u, w[1] = v, w[2] = u;
    }
done:
    for (int k = 0; k < 3; k++) q[k] = w[0] * a[k] + w[1] * b[k] + w[2] * c[k];
}

static unsigned hcell(long ix, long iy, long iz, unsigned mask) {
    return (unsigned)((ix * 73856093L) ^ (iy * 19349663L) ^ (iz * 83492791L)) & mask;
}

/* the sheet against itself: every node against the triangles in its hash cell that are not near it at rest */
static void self_contact(Sheet *S) {
    const double d = S->s.self_contact, hc = S->hc;
    const unsigned mask = S->hmask;
    memset(S->hstart, 0, (size_t)(mask + 2) * sizeof(int));
    for (int pass = 0; pass < 2; pass++) { /* count, then fill: each triangle in every cell its box (grown by d) meets */
        for (int t = 0; t < S->nt; t++) {
            const int *T = &S->tri[3 * t];
            double lo[3], hi[3];
            for (int k = 0; k < 3; k++) {
                lo[k] = fmin(S->x[3 * T[0] + k], fmin(S->x[3 * T[1] + k], S->x[3 * T[2] + k])) - d;
                hi[k] = fmax(S->x[3 * T[0] + k], fmax(S->x[3 * T[1] + k], S->x[3 * T[2] + k])) + d;
            }
            for (long iz = (long)floor(lo[2] / hc); iz <= (long)floor(hi[2] / hc); iz++)
                for (long iy = (long)floor(lo[1] / hc); iy <= (long)floor(hi[1] / hc); iy++)
                    for (long ix = (long)floor(lo[0] / hc); ix <= (long)floor(hi[0] / hc); ix++) {
                        unsigned h = hcell(ix, iy, iz, mask);
                        if (pass == 0) S->hstart[h + 1]++;
                        else if (S->hfill[h] < S->hstart[h + 1] - S->hstart[h] && S->hstart[h] + S->hfill[h] < S->hcap)
                            S->hlist[S->hstart[h] + S->hfill[h]++] = t;
                    }
        }
        if (pass == 0) {
            for (unsigned h = 0; h <= mask; h++) S->hstart[h + 1] += S->hstart[h];
            if (S->hstart[mask + 1] > S->hcap) {
                int cap = S->hstart[mask + 1] + S->hstart[mask + 1] / 2;
                int *nl = realloc(S->hlist, (size_t)cap * sizeof(int));
                if (!nl) return;
                S->hlist = nl, S->hcap = cap;
            }
            memset(S->hfill, 0, (size_t)(mask + 1) * sizeof(int));
        }
    }
    for (int i = 0; i < S->n; i++) {
        const double *p = &S->x[3 * i], *vp = &S->v[3 * i], *P0 = &S->X[3 * i];
        unsigned h = hcell((long)floor(p[0] / hc), (long)floor(p[1] / hc), (long)floor(p[2] / hc), mask);
        for (int e = S->hstart[h]; e < S->hstart[h] + S->hfill[h]; e++) {
            int t = S->hlist[e];
            const int *T = &S->tri[3 * t];
            if (T[0] == i || T[1] == i || T[2] == i) continue;
            double q[3], w[3], r[3];
            closest_tri(p, &S->x[3 * T[0]], &S->x[3 * T[1]], &S->x[3 * T[2]], q, w);
            sub(p, q, r);
            double dist = sqrt(dot(r, r));
            if (dist >= d) continue;
            double q0[3], w0[3], r0[3]; /* at rest: neighbours in the sheet are not contacts */
            closest_tri(P0, &S->X[3 * T[0]], &S->X[3 * T[1]], &S->X[3 * T[2]], q0, w0);
            sub(P0, q0, r0);
            if (dot(r0, r0) < 2.25 * d * d) continue;
            double nrm[3];
            if (dist > 1e-12 * d) for (int k = 0; k < 3; k++) nrm[k] = r[k] / dist;
            else { /* on the triangle: push along its normal, to the side the node moves from */
                double e1[3], e2[3];
                sub(&S->x[3 * T[1]], &S->x[3 * T[0]], e1), sub(&S->x[3 * T[2]], &S->x[3 * T[0]], e2), cross(e1, e2, nrm);
                double l = sqrt(dot(nrm, nrm)), sgn = dot(vp, nrm) > 0 ? -1 : 1;
                for (int k = 0; k < 3; k++) nrm[k] *= sgn / (l > 0 ? l : 1);
            }
            double mt = w[0] * S->m[T[0]] + w[1] * S->m[T[1]] + w[2] * S->m[T[2]], mi = S->m[i], mr = mi * mt / (mi + mt);
            double vt[3] = {0, 0, 0};
            for (int c = 0; c < 3; c++)
                for (int k = 0; k < 3; k++) vt[k] += w[c] * S->v[3 * T[c] + k];
            double vn = (vp[0] - vt[0]) * nrm[0] + (vp[1] - vt[1]) * nrm[1] + (vp[2] - vt[2]) * nrm[2];
            /* a spring stiff enough to stop the pair within the gap at the stable step, damped near critically */
            double k = 0.2 * mr / (S->dt_s * S->dt_s), fmag = k * (d - dist) - 2 * 0.5 * sqrt(k * mr) * vn;
            if (fmag <= 0) continue;
            for (int k2 = 0; k2 < 3; k2++) {
                S->f[3 * i + k2] += fmag * nrm[k2];
                for (int c = 0; c < 3; c++) S->f[3 * T[c] + k2] -= w[c] * fmag * nrm[k2];
            }
        }
    }
}

static void all_forces(Sheet *S, ThreadPool *pool) {
    Ctx C = {S, 0, 0};
    if (S->forms_stale) {
        for (int t = 0; t < S->nt; t++) bending_form(S, S->X, t);
        S->forms_stale = false, S->dt_s = sheet_stable_dt(S);
    }
    pool_for(pool, S->nt, 256, tri_forces, &C);
    if (S->nh) {
        pool_for(pool, S->nh, 256, hinge_angles, &C);
        pool_for(pool, S->nt, 256, tri_bending, &C);
        pool_for(pool, S->nh, 256, hinge_forces, &C);
    }
    pool_for(pool, S->n, 256, gather, &C);
    if (S->s.self_contact > 0 && S->hstart) self_contact(S);
    S->forces_ready = true;
}

void sheet_step(Sheet *S, double dt, ThreadPool *pool) {
    if (!S->forces_ready) all_forces(S, pool);
    Ctx C = {S, dt, 0};
    pool_for(pool, S->n, 256, node_update, &C);
    all_forces(S, pool);
    C.phase = 1;
    pool_for(pool, S->n, 256, node_update, &C);
}

void sheet_energy(const Sheet *S, double *membrane, double *bending, double *kinetic) {
    double em = 0, eb = 0, ek = 0;
    for (int t = 0; t < S->nt; t++) {
        const int *T = &S->tri[3 * t];
        const double *x0 = &S->x[3 * T[0]], *x1 = &S->x[3 * T[1]], *x2 = &S->x[3 * T[2]], *Di = &S->Dinv[4 * t];
        double d1[3], d2[3], F[3][2], C00 = 0, C01 = 0, C11 = 0;
        sub(x1, x0, d1), sub(x2, x0, d2);
        for (int k = 0; k < 3; k++) F[k][0] = d1[k] * Di[0] + d2[k] * Di[2], F[k][1] = d1[k] * Di[1] + d2[k] * Di[3];
        for (int k = 0; k < 3; k++) C00 += F[k][0] * F[k][0], C01 += F[k][0] * F[k][1], C11 += F[k][1] * F[k][1];
        double J = C00 * C11 - C01 * C01;
        em += S->A0[t] * S->s.thickness * 0.5 * S->s.shear_modulus * (C00 + C11 + 1 / J - 3);
    }
    for (int t = 0; t < S->nt; t++) {
        double a[3];
        for (int k = 0; k < 3; k++) {
            int h = S->te[3 * t + k];
            a[k] = h < 0 ? 0 : hinge_angle(&S->x[3 * S->h[h].a], &S->x[3 * S->h[h].b], &S->x[3 * S->h[h].c], &S->x[3 * S->h[h].d], S->h[h].theta0, NULL);
        }
        const double *Q = &S->Q[9 * t];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) eb += 0.5 * a[i] * Q[3 * i + j] * a[j];
    }
    for (int i = 0; i < S->n; i++) ek += 0.5 * S->m[i] * dot(&S->v[3 * i], &S->v[3 * i]);
    if (membrane) *membrane = em;
    if (bending) *bending = eb;
    if (kinetic) *kinetic = ek;
}

void sheet_stretch(const Sheet *S, double *lambda) {
    for (int t = 0; t < S->nt; t++) {
        const int *T = &S->tri[3 * t];
        const double *x0 = &S->x[3 * T[0]], *x1 = &S->x[3 * T[1]], *x2 = &S->x[3 * T[2]], *Di = &S->Dinv[4 * t];
        double d1[3], d2[3], F[3][2], C00 = 0, C01 = 0, C11 = 0;
        sub(x1, x0, d1), sub(x2, x0, d2);
        for (int k = 0; k < 3; k++) F[k][0] = d1[k] * Di[0] + d2[k] * Di[2], F[k][1] = d1[k] * Di[1] + d2[k] * Di[3];
        for (int k = 0; k < 3; k++) C00 += F[k][0] * F[k][0], C01 += F[k][0] * F[k][1], C11 += F[k][1] * F[k][1];
        double tr = C00 + C11, det = C00 * C11 - C01 * C01, disc = sqrt(fmax(0, 0.25 * tr * tr - det));
        lambda[t] = sqrt(0.5 * tr + disc);
    }
}

double sheet_volume(const Sheet *S) {
    double V = 0;
    for (int t = 0; t < S->nt; t++) {
        const int *T = &S->tri[3 * t];
        double c[3];
        cross(&S->x[3 * T[1]], &S->x[3 * T[2]], c);
        V += dot(&S->x[3 * T[0]], c) / 6;
    }
    return V;
}
