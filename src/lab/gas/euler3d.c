/* euler3d.c - compressible flow around bodies in 3D (euler3d.h). */
#include "euler3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { G = 2 }; /* padding: two layers of boundary cells on every face */

typedef struct {
    int cell;
    int nb[8];
    double w[8], n[3];
} Ghost;

struct Euler3D {
    Euler3DSpec s;
    int NX, NY, NZ; /* padded */
    size_t N;
    double *U[5], *U0[5], *W[5], *F[5], *R[5];
    float hll_above; /* the sensor level above which HLL is used */
    float *shock; /* the pressure-jump sensor of each cell: HLL where it is large (the hybrid of euler3d.h) */
    unsigned char *kind; /* padded: 0 fluid, 1 solid, 2 ghost, 3 outside the domain */
    Ghost *ghost;
    int nghost;
    double time;
    double *tmax;        /* per thread: the largest signal speed sum */
    int nthreads;
};

static inline size_t IX(const Euler3D *E, int i, int j, int k) { return (size_t)i + (size_t)E->NX * ((size_t)j + (size_t)E->NY * k); }

static void cons_from_prim(double g, double r, const double v[3], double p, double u[5]) {
    u[0] = r, u[1] = r * v[0], u[2] = r * v[1], u[3] = r * v[2], u[4] = p / (g - 1) + 0.5 * r * (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

Euler3D *euler3d_create(const Euler3DSpec *s, Euler3DSdf sdf, void *ctx, char *err, size_t errlen) {
    if (s->nx < 4 || s->ny < 4 || s->nz < 4 || !(s->dx > 0) || !(s->gamma > 1) || !(s->rho_inf > 0) || !(s->p_inf > 0)) {
        snprintf(err, errlen, "euler3d: need at least 4 cells a side, dx > 0, gamma > 1 and a positive free-stream density and pressure");
        return NULL;
    }
    Euler3D *E = calloc(1, sizeof *E);
    if (!E) return NULL;
    E->s = *s;
    if (!(E->s.cfl > 0)) E->s.cfl = 0.4;
    E->hll_above = getenv("EULER3D_HLL_ALL") ? -1.f : 0.5f; /* diagnostics only: HLL on every face */
    E->NX = s->nx + 2 * G, E->NY = s->ny + 2 * G, E->NZ = s->nz + 2 * G;
    E->N = (size_t)E->NX * E->NY * E->NZ;
    bool ok = true;
    for (int c = 0; c < 5 && ok; c++) {
        E->U[c] = malloc(E->N * sizeof(double)), E->U0[c] = malloc(E->N * sizeof(double)), E->W[c] = malloc(E->N * sizeof(double));
        E->F[c] = malloc(E->N * sizeof(double)), E->R[c] = malloc(E->N * sizeof(double));
        ok = E->U[c] && E->U0[c] && E->W[c] && E->F[c] && E->R[c];
    }
    E->kind = malloc(E->N), E->shock = calloc(E->N, sizeof(float));
    if (!ok || !E->kind || !E->shock) {
        snprintf(err, errlen, "euler3d: out of memory for %zu cells (%.0f MB)", E->N, E->N * 25.0 * 8 / 1048576);
        euler3d_free(E);
        return NULL;
    }
    /* the bodies: solid cells, and the solid cells within two cells of the fluid along an axis are ghosts */
    for (int k = 0; k < E->NZ; k++)
        for (int j = 0; j < E->NY; j++)
            for (int i = 0; i < E->NX; i++) {
                size_t q = IX(E, i, j, k);
                bool in = i >= G && j >= G && k >= G && i < G + s->nx && j < G + s->ny && k < G + s->nz;
                if (!in) {
                    E->kind[q] = 3;
                    continue;
                }
                double p[3] = {s->origin[0] + (i - G + 0.5) * s->dx, s->origin[1] + (j - G + 0.5) * s->dx, s->origin[2] + (k - G + 0.5) * s->dx};
                E->kind[q] = sdf && sdf(p, ctx) < 0 ? 1 : 0;
            }
    int cap = 0;
    for (int pass = 0; pass < 2; pass++) {
        int ng = 0;
        for (int k = G; k < G + s->nz; k++)
            for (int j = G; j < G + s->ny; j++)
                for (int i = G; i < G + s->nx; i++) {
                    size_t q = IX(E, i, j, k);
                    if (E->kind[q] == 0) continue;
                    bool near = false;
                    for (int d = 1; d <= 2 && !near; d++) {
                        size_t nb[6] = {IX(E, i - d, j, k), IX(E, i + d, j, k), IX(E, i, j - d, k), IX(E, i, j + d, k), IX(E, i, j, k - d), IX(E, i, j, k + d)};
                        for (int m = 0; m < 6; m++) near |= E->kind[nb[m]] == 0;
                    }
                    if (!near) continue;
                    if (pass == 1) {
                        Ghost *gh = &E->ghost[ng];
                        gh->cell = (int)q;
                        double c[3] = {s->origin[0] + (i - G + 0.5) * s->dx, s->origin[1] + (j - G + 0.5) * s->dx, s->origin[2] + (k - G + 0.5) * s->dx};
                        double phi = sdf(c, ctx), h = 0.25 * s->dx, gr[3];
                        for (int a = 0; a < 3; a++) {
                            double pa[3] = {c[0], c[1], c[2]}, pb[3] = {c[0], c[1], c[2]};
                            pa[a] += h, pb[a] -= h;
                            gr[a] = (sdf(pa, ctx) - sdf(pb, ctx)) / (2 * h);
                        }
                        double gl = sqrt(gr[0] * gr[0] + gr[1] * gr[1] + gr[2] * gr[2]);
                        for (int a = 0; a < 3; a++) gh->n[a] = gl > 0 ? gr[a] / gl : 0;
                        /* the image point, and trilinear weights over the fluid cells around it */
                        double I[3];
                        for (int a = 0; a < 3; a++) I[a] = c[a] - 2 * phi * gh->n[a];
                        double fx = (I[0] - s->origin[0]) / s->dx - 0.5, fy = (I[1] - s->origin[1]) / s->dx - 0.5, fz = (I[2] - s->origin[2]) / s->dx - 0.5;
                        int i0 = (int)floor(fx), j0 = (int)floor(fy), k0 = (int)floor(fz);
                        double tx = fx - i0, ty = fy - j0, tz = fz - k0, wsum = 0;
                        for (int m = 0; m < 8; m++) {
                            /* the stencil may reach into the boundary layers, which hold the mirrored (symmetry) or boundary
                             * states by the time ghosts are filled; clamping here instead made a false stagnation pressure
                             * where two symmetry planes meet (found by euler3dtest E2) */
                            int ii = i0 + (m & 1), jj = j0 + ((m >> 1) & 1), kk = k0 + ((m >> 2) & 1);
                            ii = ii < -G ? -G : ii >= s->nx + G ? s->nx + G - 1 : ii, jj = jj < -G ? -G : jj >= s->ny + G ? s->ny + G - 1 : jj;
                            kk = kk < -G ? -G : kk >= s->nz + G ? s->nz + G - 1 : kk;
                            size_t cq = IX(E, ii + G, jj + G, kk + G);
                            double w = ((m & 1) ? tx : 1 - tx) * (((m >> 1) & 1) ? ty : 1 - ty) * (((m >> 2) & 1) ? tz : 1 - tz);
                            if (E->kind[cq] == 1 || E->kind[cq] == 2) w = 0;
                            gh->nb[m] = (int)cq, gh->w[m] = w, wsum += w;
                        }
                        if (wsum > 1e-12)
                            for (int m = 0; m < 8; m++) gh->w[m] /= wsum;
                        else { /* no fluid around the image: the nearest fluid neighbour along an axis */
                            for (int m = 0; m < 8; m++) gh->w[m] = 0;
                            for (int d = 1; d <= 2 && gh->w[0] == 0; d++) {
                                size_t nb[6] = {IX(E, i - d, j, k), IX(E, i + d, j, k), IX(E, i, j - d, k), IX(E, i, j + d, k), IX(E, i, j, k - d), IX(E, i, j, k + d)};
                                for (int m = 0; m < 6; m++)
                                    if (E->kind[nb[m]] == 0) {
                                        gh->nb[0] = (int)nb[m], gh->w[0] = 1;
                                        break;
                                    }
                            }
                        }
                    }
                    ng++;
                }
        if (pass == 0) {
            cap = ng;
            E->ghost = malloc(sizeof(Ghost) * (size_t)(cap ? cap : 1));
            if (!E->ghost) {
                snprintf(err, errlen, "euler3d: out of memory for ghost cells");
                euler3d_free(E);
                return NULL;
            }
        } else E->nghost = ng;
    }
    for (int m = 0; m < E->nghost; m++) E->kind[E->ghost[m].cell] = 2;
    /* the free stream everywhere */
    double u[5];
    cons_from_prim(s->gamma, s->rho_inf, s->u_inf, s->p_inf, u);
    for (size_t q = 0; q < E->N; q++)
        for (int c = 0; c < 5; c++) E->U[c][q] = u[c];
    return E;
}

void euler3d_free(Euler3D *E) {
    if (!E) return;
    for (int c = 0; c < 5; c++) free(E->U[c]), free(E->U0[c]), free(E->W[c]), free(E->F[c]), free(E->R[c]);
    free(E->kind), free(E->shock), free(E->ghost), free(E->tmax), free(E);
}

double euler3d_time(const Euler3D *E) { return E->time; }

void euler3d_set(Euler3D *E, int i, int j, int k, double rho, const double v[3], double p) {
    double u[5];
    cons_from_prim(E->s.gamma, rho, v, p, u);
    size_t q = IX(E, i + G, j + G, k + G);
    for (int c = 0; c < 5; c++) E->U[c][q] = u[c];
}
const unsigned char *euler3d_kind(const Euler3D *E) { return E->kind; }

/* ---- one stage: boundary layers, ghosts, primitives, fluxes, residual ---------------------------------------------- */

typedef struct {
    Euler3D *E;
    double **U;
    int dir;
    bool bad;
} Ctx;

static void boundaries(Euler3D *E, double **U) {
    const Euler3DSpec *s = &E->s;
    double fs[5];
    cons_from_prim(s->gamma, s->rho_inf, s->u_inf, s->p_inf, fs);
    int lo[3] = {G, G, G}, hi[3] = {G + s->nx - 1, G + s->ny - 1, G + s->nz - 1}, Nn[3] = {E->NX, E->NY, E->NZ};
    for (int a = 0; a < 3; a++)
        for (int side = 0; side < 2; side++) {
            int type = s->face[2 * a + side];
            for (int l = 1; l <= G; l++) {
                int layer = side ? hi[a] + l : lo[a] - l, src = side ? hi[a] - (l - 1) : lo[a] + (l - 1), edge = side ? hi[a] : lo[a];
                int b = (a + 1) % 3, c = (a + 2) % 3;
                for (int jb = 0; jb < Nn[b]; jb++)
                    for (int jc = 0; jc < Nn[c]; jc++) {
                        int P[3], S[3];
                        P[a] = layer, P[b] = jb, P[c] = jc;
                        S[a] = type == E3_SYMMETRY ? src : edge, S[b] = jb, S[c] = jc;
                        size_t q = IX(E, P[0], P[1], P[2]), r = IX(E, S[0], S[1], S[2]);
                        for (int k = 0; k < 5; k++) U[k][q] = type == E3_INFLOW ? fs[k] : U[k][r];
                        if (type == E3_SYMMETRY) U[1 + a][q] = -U[1 + a][r];
                    }
            }
        }
    /* ghosts inside the bodies: the image's state, normal velocity reversed */
    const double g = s->gamma;
    for (int m = 0; m < E->nghost; m++) {
        const Ghost *gh = &E->ghost[m];
        double r = 0, mx = 0, my = 0, mz = 0, p = 0;
        for (int t = 0; t < 8; t++) {
            double w = gh->w[t];
            if (w == 0) continue;
            size_t q = (size_t)gh->nb[t];
            double rr = U[0][q], vx = U[1][q] / rr, vy = U[2][q] / rr, vz = U[3][q] / rr;
            double pp = (g - 1) * (U[4][q] - 0.5 * rr * (vx * vx + vy * vy + vz * vz));
            r += w * rr, mx += w * vx, my += w * vy, mz += w * vz, p += w * pp;
        }
        double vn = mx * gh->n[0] + my * gh->n[1] + mz * gh->n[2], v[3] = {mx - 2 * vn * gh->n[0], my - 2 * vn * gh->n[1], mz - 2 * vn * gh->n[2]};
        double u[5];
        cons_from_prim(g, r > 0 ? r : s->rho_inf, v, p > 0 ? p : s->p_inf, u);
        for (int k = 0; k < 5; k++) U[k][gh->cell] = u[k];
    }
}

static void prim_slab(void *vc, int k0, int k1, int tid) {
    Ctx *C = vc;
    Euler3D *E = C->E;
    const double g = E->s.gamma;
    double smax = 0;
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < E->NY; j++)
            for (int i = 0; i < E->NX; i++) {
                size_t q = IX(E, i, j, k);
                double r = C->U[0][q], vx = C->U[1][q] / r, vy = C->U[2][q] / r, vz = C->U[3][q] / r;
                double p = (g - 1) * (C->U[4][q] - 0.5 * r * (vx * vx + vy * vy + vz * vz));
                E->W[0][q] = r, E->W[1][q] = vx, E->W[2][q] = vy, E->W[3][q] = vz, E->W[4][q] = p;
                if (E->kind[q] == 0) {
                    if (!(r > 0) || !(p > 0)) C->bad = true;
                    else {
                        double c = sqrt(g * p / r);
                        smax = fmax(smax, fabs(vx) + fabs(vy) + fabs(vz) + 3 * c);
                    }
                }
            }
    E->tmax[tid] = fmax(E->tmax[tid], smax);
}

/* the sensor: the largest relative pressure jump from a cell to its six neighbours */
static void sensor_slab(void *vc, int k0, int k1, int tid) {
    Ctx *C = vc;
    Euler3D *E = C->E;
    const long st[3] = {1, E->NX, (long)E->NX * E->NY};
    for (int k = k0; k < k1; k++) {
        if (k < 1 || k >= E->NZ - 1) continue;
        for (int j = 1; j < E->NY - 1; j++)
            for (int i = 1; i < E->NX - 1; i++) {
                size_t q = IX(E, i, j, k);
                /* over one and two cells each way, so that HLL covers the shock and the cells just behind it, where
                 * HLLC's instability grows (euler3dtest E2: the stagnation pressure drifted 2.5 % above the pitot value
                 * when only the shock's own faces were switched) */
                double p = E->W[4][q], m = 0;
                for (int d = 0; d < 3; d++)
                    for (int sg = -1; sg <= 1; sg += 2)
                        for (int r = 1; r <= 2; r++) {
                            long off = sg * r * st[d];
                            if ((long)q + off < 0 || (size_t)((long)q + off) >= E->N) continue;
                            double pn = E->W[4][(size_t)((long)q + off)];
                            if (pn > 0 && p > 0) m = fmax(m, fabs(pn - p) / fmin(pn, p));
                        }
                E->shock[q] = (float)m;
            }
    }
}

static inline double mc(double a, double b) { /* monotonised central */
    if (a * b <= 0) return 0;
    double m = fmin(fmin(2 * fabs(a), 2 * fabs(b)), 0.5 * fabs(a + b));
    return a > 0 ? m : -m;
}

static void hll(const double L[5], const double R[5], double g, int d, double f[5]) {
    double rl = L[0], rr = R[0], pl = L[4], pr = R[4], ul = L[1 + d], ur = R[1 + d];
    double cl = sqrt(g * pl / rl), cr = sqrt(g * pr / rr);
    double El = pl / (g - 1) + 0.5 * rl * (L[1] * L[1] + L[2] * L[2] + L[3] * L[3]), Er = pr / (g - 1) + 0.5 * rr * (R[1] * R[1] + R[2] * R[2] + R[3] * R[3]);
    double sl = fmin(ul - cl, ur - cr), sr = fmax(ul + cl, ur + cr);
    double ULc[5] = {rl, rl * L[1], rl * L[2], rl * L[3], El}, URc[5] = {rr, rr * R[1], rr * R[2], rr * R[3], Er};
    double FL[5] = {rl * ul, rl * L[1] * ul, rl * L[2] * ul, rl * L[3] * ul, (El + pl) * ul};
    double FR[5] = {rr * ur, rr * R[1] * ur, rr * R[2] * ur, rr * R[3] * ur, (Er + pr) * ur};
    FL[1 + d] += pl, FR[1 + d] += pr;
    if (sl >= 0) { memcpy(f, FL, sizeof FL); return; }
    if (sr <= 0) { memcpy(f, FR, sizeof FR); return; }
    for (int c = 0; c < 5; c++) f[c] = (sr * FL[c] - sl * FR[c] + sl * sr * (URc[c] - ULc[c])) / (sr - sl);
}

static void hllc(const double L[5], const double R[5], double g, int d, double f[5]) {
    /* primitive states: rho, u, v, w, p; d the face normal's axis */
    double rl = L[0], rr = R[0], pl = L[4], pr = R[4], ul = L[1 + d], ur = R[1 + d];
    double cl = sqrt(g * pl / rl), cr = sqrt(g * pr / rr);
    double El = pl / (g - 1) + 0.5 * rl * (L[1] * L[1] + L[2] * L[2] + L[3] * L[3]), Er = pr / (g - 1) + 0.5 * rr * (R[1] * R[1] + R[2] * R[2] + R[3] * R[3]);
    double sl = fmin(ul - cl, ur - cr), sr = fmax(ul + cl, ur + cr);
    double ULc[5] = {rl, rl * L[1], rl * L[2], rl * L[3], El}, URc[5] = {rr, rr * R[1], rr * R[2], rr * R[3], Er};
    double FL[5] = {rl * ul, rl * L[1] * ul, rl * L[2] * ul, rl * L[3] * ul, (El + pl) * ul};
    double FR[5] = {rr * ur, rr * R[1] * ur, rr * R[2] * ur, rr * R[3] * ur, (Er + pr) * ur};
    FL[1 + d] += pl, FR[1 + d] += pr;
    if (sl >= 0) { memcpy(f, FL, sizeof FL); return; }
    if (sr <= 0) { memcpy(f, FR, sizeof FR); return; }
    double sm = (pr - pl + rl * ul * (sl - ul) - rr * ur * (sr - ur)) / (rl * (sl - ul) - rr * (sr - ur));
    const double *Uk = sm >= 0 ? ULc : URc, *Fk = sm >= 0 ? FL : FR, *P = sm >= 0 ? L : R;
    double rk = P[0], uk = P[1 + d], pk = P[4], sk = sm >= 0 ? sl : sr, ek = Uk[4];
    double fac = rk * (sk - uk) / (sk - sm), st[5];
    st[0] = fac;
    for (int a = 0; a < 3; a++) st[1 + a] = fac * (a == d ? sm : P[1 + a]);
    st[4] = fac * (ek / rk + (sm - uk) * (sm + pk / (rk * (sk - uk))));
    for (int c = 0; c < 5; c++) f[c] = Fk[c] + sk * (st[c] - Uk[c]);
}

/* the flux through the face between a cell and its neighbour along dir, for every cell that has one */
static void flux_slab(void *vc, int k0, int k1, int tid) {
    Ctx *C = vc;
    Euler3D *E = C->E;
    const int d = C->dir;
    const double g = E->s.gamma;
    const long st[3] = {1, E->NX, (long)E->NX * E->NY};
    const long o = st[d];
    for (int k = k0; k < k1; k++)
        for (int j = 1; j < E->NY - 2; j++)
            for (int i = 1; i < E->NX - 2; i++) {
                if ((d == 1 && (j < 1 || j >= E->NY - 2)) || (d == 2 && (k < 1 || k >= E->NZ - 2))) continue;
                size_t q = IX(E, i, j, k);
                unsigned char a = E->kind[q], b = E->kind[q + o];
                if ((a == 1 || a == 3) && (b == 1 || b == 3)) continue;
                if (a != 0 && b != 0) continue; /* only faces of the fluid */
                double L[5], R[5];
                for (int c = 0; c < 5; c++) {
                    double wm = E->W[c][q - o], w0 = E->W[c][q], w1 = E->W[c][q + o], w2 = E->W[c][q + 2 * o];
                    L[c] = w0 + 0.5 * mc(w0 - wm, w1 - w0), R[c] = w1 - 0.5 * mc(w1 - w0, w2 - w1);
                }
                if (!(L[0] > 0) || !(L[4] > 0) || !(R[0] > 0) || !(R[4] > 0)) /* first order where the reconstruction fails */
                    for (int c = 0; c < 5; c++) L[c] = E->W[c][q], R[c] = E->W[c][q + o];
                double f[5];
                if (fmaxf(E->shock[q], E->shock[q + o]) > E->hll_above) hll(L, R, g, d, f); /* near a strong shock */
                else hllc(L, R, g, d, f);
                for (int c = 0; c < 5; c++) E->F[c][q] = f[c];
            }
}

static void residual_slab(void *vc, int k0, int k1, int tid) {
    Ctx *C = vc;
    Euler3D *E = C->E;
    const int d = C->dir;
    const long st[3] = {1, E->NX, (long)E->NX * E->NY};
    const long o = st[d];
    const double inv = 1 / E->s.dx;
    for (int k = k0; k < k1; k++)
        for (int j = G; j < E->NY - G; j++)
            for (int i = G; i < E->NX - G; i++) {
                size_t q = IX(E, i, j, k);
                if (E->kind[q] != 0) continue;
                for (int c = 0; c < 5; c++) E->R[c][q] += (E->F[c][q - o] - E->F[c][q]) * inv;
            }
}

static bool rhs(Euler3D *E, double **U, ThreadPool *pool, double *smax) {
    boundaries(E, U);
    Ctx C = {E, U, 0, false};
    int nt = pool ? pool_size(pool) : 1;
    if (E->nthreads < nt) {
        free(E->tmax);
        E->tmax = calloc((size_t)nt, sizeof(double));
        E->nthreads = nt;
    }
    for (int t = 0; t < E->nthreads; t++) E->tmax[t] = 0;
    if (pool) pool_for(pool, E->NZ, 1, prim_slab, &C);
    else prim_slab(&C, 0, E->NZ, 0);
    if (C.bad) return false;
    *smax = 0;
    for (int t = 0; t < E->nthreads; t++) *smax = fmax(*smax, E->tmax[t]);
    if (pool) pool_for(pool, E->NZ, 1, sensor_slab, &C);
    else sensor_slab(&C, 0, E->NZ, 0);
    for (int c = 0; c < 5; c++) memset(E->R[c], 0, E->N * sizeof(double));
    for (int d = 0; d < 3; d++) {
        C.dir = d;
        if (pool) pool_for(pool, E->NZ - 2, 1, flux_slab, &C);
        else flux_slab(&C, 0, E->NZ - 2, 0);
        if (pool) pool_for(pool, E->NZ - G, 1, residual_slab, &C);
        else residual_slab(&C, G, E->NZ - G, 0);
    }
    return true;
}

double euler3d_step(Euler3D *E, ThreadPool *pool) {
    double smax;
    if (!rhs(E, E->U, pool, &smax) || !(smax > 0)) return 0;
    double dt = E->s.cfl * E->s.dx / smax;
    for (int c = 0; c < 5; c++) {
        memcpy(E->U0[c], E->U[c], E->N * sizeof(double));
        for (size_t q = 0; q < E->N; q++)
            if (E->kind[q] == 0) E->U[c][q] += dt * E->R[c][q];
    }
    double s2;
    if (!rhs(E, E->U, pool, &s2)) return 0;
    for (int c = 0; c < 5; c++)
        for (size_t q = 0; q < E->N; q++)
            if (E->kind[q] == 0) E->U[c][q] = 0.5 * E->U0[c][q] + 0.5 * (E->U[c][q] + dt * E->R[c][q]);
    E->time += dt;
    return dt;
}

void euler3d_fields(const Euler3D *E, double *rho, double *u, double *p) {
    const Euler3DSpec *s = &E->s;
    size_t o = 0;
    for (int k = G; k < G + s->nz; k++)
        for (int j = G; j < G + s->ny; j++)
            for (int i = G; i < G + s->nx; i++, o++) {
                size_t q = IX(E, i, j, k);
                bool fl = E->kind[q] == 0 || E->kind[q] == 2; /* ghosts hold the gas mirrored across the wall: continuous */
                double r = E->U[0][q], vx = E->U[1][q] / r, vy = E->U[2][q] / r, vz = E->U[3][q] / r;
                double pp = (s->gamma - 1) * (E->U[4][q] - 0.5 * r * (vx * vx + vy * vy + vz * vz));
                if (rho) rho[o] = fl ? r : s->rho_inf;
                if (u) u[3 * o] = fl ? vx : 0, u[3 * o + 1] = fl ? vy : 0, u[3 * o + 2] = fl ? vz : 0;
                if (p) p[o] = fl ? pp : s->p_inf;
            }
}

double euler3d_probe_p(const Euler3D *E, const double x[3]) {
    const Euler3DSpec *s = &E->s;
    double fx = (x[0] - s->origin[0]) / s->dx - 0.5, fy = (x[1] - s->origin[1]) / s->dx - 0.5, fz = (x[2] - s->origin[2]) / s->dx - 0.5;
    int i0 = (int)floor(fx), j0 = (int)floor(fy), k0 = (int)floor(fz);
    double tx = fx - i0, ty = fy - j0, tz = fz - k0, sum = 0, ws = 0;
    for (int m = 0; m < 8; m++) {
        int ii = i0 + (m & 1), jj = j0 + ((m >> 1) & 1), kk = k0 + ((m >> 2) & 1);
        if (ii < 0 || jj < 0 || kk < 0 || ii >= s->nx || jj >= s->ny || kk >= s->nz) continue;
        size_t q = IX(E, ii + G, jj + G, kk + G);
        if (E->kind[q] != 0) continue;
        double w = ((m & 1) ? tx : 1 - tx) * (((m >> 1) & 1) ? ty : 1 - ty) * (((m >> 2) & 1) ? tz : 1 - tz);
        double r = E->U[0][q], vx = E->U[1][q] / r, vy = E->U[2][q] / r, vz = E->U[3][q] / r;
        sum += w * (s->gamma - 1) * (E->U[4][q] - 0.5 * r * (vx * vx + vy * vy + vz * vz)), ws += w;
    }
    return ws > 0 ? sum / ws : NAN;
}
