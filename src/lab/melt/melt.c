/* melt.c - the enthalpy method for a laser melting metal (melt.h). */
#include "melt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mg/mg3d.h"

enum { MAXT = 64 };

struct Melt {
    MeltSpec s;
    int nx, ny, nz;
    size_t n;
    double *H, *Hn, *T, *f, *Hmax, *k; /* Hmax: the largest enthalpy each cell has reached (its liquid fraction is the largest reached) */
    double Hs, Hl, t;
    double Hp[32]; /* the solid's enthalpy at the property table's points (with nprop) */
    double absorbed, lost, held_out;
    double lost_t[MAXT], held_t[MAXT], abs_t[MAXT];
    double bx, by, dt; /* the beam this step */
    bool on;
    /* the flow: face velocities, the predictor, projection coefficients, pressure */
    double *u, *v, *w, *us, *vs, *ws, *cx, *cy, *cz, *p, *rhs;
    Mg3D *mg;          /* the pressure solver of the window around the pool */
    int wn[3];         /* its size in cells */
    double *wcx, *wcy, *wcz, *wp, *wr;
    double umax, div_err;
};

static inline size_t IU(const Melt *M, int i, int j, int k) { return (size_t)i + (size_t)(M->nx + 1) * ((size_t)j + (size_t)M->ny * k); }
static inline size_t IV(const Melt *M, int i, int j, int k) { return (size_t)i + (size_t)M->nx * ((size_t)j + (size_t)(M->ny + 1) * k); }
static inline size_t IW(const Melt *M, int i, int j, int k) { return (size_t)i + (size_t)M->nx * ((size_t)j + (size_t)M->ny * k); }
static inline size_t CC(const Melt *M, int i, int j, int k) { return (size_t)i + (size_t)M->nx * ((size_t)j + (size_t)M->ny * k); }

static const double SIGMA = 5.670374419e-8;

/* the solid's properties at T: the table's linear interpolation, constant beyond its ends, or the constants */
static double prop_at(const MeltSpec *s, const double *v, double T, double constant) {
    int n = s->nprop;
    if (n <= 0) return constant;
    if (T <= s->prop_T[0]) return v[0];
    if (T >= s->prop_T[n - 1]) return v[n - 1];
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        int m = (lo + hi) / 2;
        if (s->prop_T[m] <= T) lo = m;
        else hi = m;
    }
    double u = (T - s->prop_T[lo]) / (s->prop_T[hi] - s->prop_T[lo]);
    return v[lo] + u * (v[hi] - v[lo]);
}
static inline double k_solid(const MeltSpec *s, double T) { return prop_at(s, s->prop_k, T, s->k_s); }
static inline double c_solid(const MeltSpec *s, double T) { return prop_at(s, s->prop_c, T, s->c_s); }

/* the solid's enthalpy, rho times the integral of c from 0 K to T (c constant below the table's first point) */
static double H_solid(const Melt *M, double T) {
    const MeltSpec *s = &M->s;
    int n = s->nprop;
    if (n <= 0) return s->rho * s->c_s * T;
    if (T <= s->prop_T[0]) return s->rho * s->prop_c[0] * T;
    if (T >= s->prop_T[n - 1]) return M->Hp[n - 1] + s->rho * s->prop_c[n - 1] * (T - s->prop_T[n - 1]);
    int i = 0;
    while (i + 2 < n && s->prop_T[i + 1] <= T) i++;
    double d = T - s->prop_T[i], m = (s->prop_c[i + 1] - s->prop_c[i]) / (s->prop_T[i + 1] - s->prop_T[i]);
    return M->Hp[i] + s->rho * (s->prop_c[i] * d + 0.5 * m * d * d);
}

/* its inverse: within a segment the enthalpy is quadratic in T */
static double T_solid(const Melt *M, double H) {
    const MeltSpec *s = &M->s;
    int n = s->nprop;
    if (n <= 0) return H / (s->rho * s->c_s);
    if (H <= M->Hp[0]) return H / (s->rho * s->prop_c[0]);
    if (H >= M->Hp[n - 1]) return s->prop_T[n - 1] + (H - M->Hp[n - 1]) / (s->rho * s->prop_c[n - 1]);
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (M->Hp[mid] <= H) lo = mid;
        else hi = mid;
    }
    double c0 = s->prop_c[lo], m = (s->prop_c[hi] - c0) / (s->prop_T[hi] - s->prop_T[lo]), e = (H - M->Hp[lo]) / s->rho;
    double d = fabs(m) < 1e-15 ? e / c0 : 2 * e / (c0 + sqrt(c0 * c0 + 2 * m * e)); /* the root of m d^2 / 2 + c0 d = e, stable */
    return s->prop_T[lo] + d;
}

double melt_T_of_H(const Melt *M, double H) {
    const MeltSpec *s = &M->s;
    if (H <= M->Hs) return T_solid(M, H);
    if (H >= M->Hl) return s->T_l + (H - M->Hl) / (s->rho * s->c_l);
    return s->T_s + (s->T_l - s->T_s) * (H - M->Hs) / (M->Hl - M->Hs);
}

double melt_H_of_T(const Melt *M, double T) {
    const MeltSpec *s = &M->s;
    if (T <= s->T_s) return H_solid(M, T);
    if (T >= s->T_l && (T > s->T_l || s->T_l > s->T_s)) return M->Hl + s->rho * s->c_l * (T - s->T_l);
    return M->Hs + (M->Hl - M->Hs) * (s->T_l > s->T_s ? (T - s->T_s) / (s->T_l - s->T_s) : 0);
}

Melt *melt_create(const MeltSpec *s, char *err, size_t errlen) {
    if (s->n[0] < 1 || s->n[1] < 1 || s->n[2] < 1 || !(s->h > 0) || !(s->rho > 0) || !(s->c_s > 0) || !(s->c_l > 0) || !(s->k_s > 0) ||
        !(s->k_l > 0) || !(s->T_l >= s->T_s) || !(s->L >= 0) || !(s->T0 > 0) || s->ntracks < 0 || s->ntracks > 16) {
        snprintf(err, errlen, "melt: need cells, a cell size, positive properties, liquidus at or above solidus, at most 16 tracks");
        return NULL;
    }
    if (s->nprop < 0 || s->nprop > 32 || s->nprop == 1) {
        snprintf(err, errlen, "melt: a property table needs 2 to 32 points");
        return NULL;
    }
    for (int i = 0; i < s->nprop; i++)
        if (!(s->prop_k[i] > 0) || !(s->prop_c[i] > 0) || (i > 0 && !(s->prop_T[i] > s->prop_T[i - 1])) || !(s->prop_T[i] > 0)) {
            snprintf(err, errlen, "melt: the property table needs rising temperatures and positive conductivities and specific heats");
            return NULL;
        }
    double cells = (double)s->n[0] * s->n[1] * s->n[2];
    if (cells > 3e7) {
        snprintf(err, errlen, "melt: %.0f cells is more than this build runs (30 million)", cells);
        return NULL;
    }
    Melt *M = calloc(1, sizeof *M);
    if (!M) return NULL;
    M->s = *s, M->nx = s->n[0], M->ny = s->n[1], M->nz = s->n[2], M->n = (size_t)cells;
    if (s->nprop > 0) { /* the enthalpy at each point of the table: the exact integral of the linear pieces */
        M->Hp[0] = s->rho * s->prop_c[0] * s->prop_T[0];
        for (int i = 1; i < s->nprop; i++)
            M->Hp[i] = M->Hp[i - 1] + s->rho * 0.5 * (s->prop_c[i - 1] + s->prop_c[i]) * (s->prop_T[i] - s->prop_T[i - 1]);
    }
    M->Hs = H_solid(M, s->T_s);
    M->Hl = M->Hs + s->rho * (0.5 * (c_solid(s, s->T_s) + s->c_l) * (s->T_l - s->T_s) + s->L);
    M->H = malloc(M->n * sizeof(double)), M->Hn = malloc(M->n * sizeof(double)), M->T = malloc(M->n * sizeof(double));
    M->f = malloc(M->n * sizeof(double)), M->Hmax = malloc(M->n * sizeof(double)), M->k = malloc(M->n * sizeof(double));
    if (!M->H || !M->Hn || !M->T || !M->f || !M->Hmax || !M->k) {
        melt_free(M);
        snprintf(err, errlen, "melt: out of memory");
        return NULL;
    }
    double H0 = melt_H_of_T(M, s->T0);
    for (size_t i = 0; i < M->n; i++) M->H[i] = M->Hmax[i] = H0;
    if (s->flow) {
        if (!(s->mu > 0) || M->nx < 4 || M->ny < 1 || M->nz < 4) {
            melt_free(M);
            snprintf(err, errlen, "melt: flow needs a viscosity and at least 4 cells along x and z");
            return NULL;
        }
        if (!(M->s.mushy_C > 0)) M->s.mushy_C = 1e10;
        size_t nu = (size_t)(M->nx + 1) * M->ny * M->nz, nv = (size_t)M->nx * (M->ny + 1) * M->nz, nw = (size_t)M->nx * M->ny * (M->nz + 1);
        M->u = calloc(nu, 8), M->us = calloc(nu, 8), M->cx = calloc(nu, 8);
        M->v = calloc(nv, 8), M->vs = calloc(nv, 8), M->cy = calloc(nv, 8);
        M->w = calloc(nw, 8), M->ws = calloc(nw, 8), M->cz = calloc(nw, 8);
        M->p = calloc(M->n, 8), M->rhs = calloc(M->n, 8);
        if (!M->u || !M->us || !M->cx || !M->v || !M->vs || !M->cy || !M->w || !M->ws || !M->cz || !M->p || !M->rhs) {
            melt_free(M);
            if (!err[0]) snprintf(err, errlen, "melt: out of memory");
            return NULL;
        }
    }
    return M;
}

void melt_free(Melt *M) {
    if (!M) return;
    free(M->H), free(M->Hn), free(M->T), free(M->f), free(M->Hmax), free(M->k);
    free(M->u), free(M->v), free(M->w), free(M->us), free(M->vs), free(M->ws), free(M->cx), free(M->cy), free(M->cz), free(M->p), free(M->rhs);
    mg3d_free(M->mg);
    free(M->wcx), free(M->wcy), free(M->wcz), free(M->wp), free(M->wr);
    free(M);
}

double melt_stable_dt(const Melt *M) {
    const MeltSpec *s = &M->s;
    double a = fmax(s->nprop > 0 ? 0 : s->k_s / (s->rho * s->c_s), s->k_l / (s->rho * s->c_l));
    for (int i = 0; i < s->nprop; i++) a = fmax(a, s->prop_k[i] / (s->rho * s->prop_c[i])); /* the fastest diffusion anywhere */
    int dims = (M->nx > 1) + (M->ny > 1) + (M->nz > 1);
    double dt = 0.9 * s->h * s->h / (2 * (dims ? dims : 1) * a);
    if (s->flow) { /* the viscous limit and the last step's fastest face (advection, first order, forward Euler) */
        dt = fmin(dt, 0.9 * s->h * s->h / (2 * (dims ? dims : 1) * s->mu / s->rho));
        if (M->umax > 0) dt = fmin(dt, 0.4 * s->h / M->umax);
    }
    return dt;
}

double melt_time(const Melt *M) { return M->t; }
size_t melt_cells(const Melt *M) { return M->n; }
const double *melt_enthalpy(const Melt *M) { return M->H; }
double *melt_enthalpy_rw(Melt *M) { return M->H; }

bool melt_beam_at(const Melt *M, double t, double *x, double *y) {
    const MeltSpec *s = &M->s;
    double t0 = 0;
    for (int i = 0; i < s->ntracks; i++) {
        const double *tr = s->track[i];
        double len = hypot(tr[2] - tr[0], tr[3] - tr[1]), on = t0 + tr[5], off = on + (tr[4] > 0 ? len / tr[4] : 0);
        if (t >= on && t < off) {
            double u = (t - on) / (off - on);
            *x = tr[0] + u * (tr[2] - tr[0]), *y = tr[1] + u * (tr[3] - tr[1]);
            return true;
        }
        t0 = off;
    }
    return false;
}

/* pass 1: temperature, liquid fraction and conductivity from the enthalpy */
static void state(void *vc, int r0, int r1, int tid) {
    Melt *M = vc;
    const MeltSpec *s = &M->s;
    for (int r = r0; r < r1; r++)
        for (int i = 0; i < M->nx; i++) {
            size_t c = (size_t)r * M->nx + i;
            double H = M->H[c], f = H <= M->Hs ? 0 : H >= M->Hl ? 1 : (H - M->Hs) / (M->Hl - M->Hs);
            double T = melt_T_of_H(M, H), ks = k_solid(s, f > 0 ? s->T_s : T); /* the solid's own k(T); at the solidus in the mushy range */
            M->T[c] = T, M->f[c] = f, M->k[c] = ks + f * (s->k_l - ks);
            if (H > M->Hmax[c]) M->Hmax[c] = H;
        }
}

static double erfbox(double a, double b, double w) { return 0.5 * (erf(M_SQRT2 * b / w) - erf(M_SQRT2 * a / w)); }

/* pass 2: the new enthalpy of each cell of rows r0..r1 (a row: fixed j, k) */
static void update(void *vc, int r0, int r1, int tid) {
    Melt *M = vc;
    const MeltSpec *s = &M->s;
    const int nx = M->nx, ny = M->ny, nz = M->nz;
    const double h = s->h, ih2 = 1 / (h * h), dt = M->dt;
    const size_t sy = (size_t)nx, sz = (size_t)nx * ny;
    double held = 0, lost = 0, absd = 0;
    for (int r = r0; r < r1; r++) {
        int j = r % ny, k = r / ny;
        for (int i = 0; i < nx; i++) {
            size_t c = (size_t)r * nx + i;
            double Tc = M->T[c], kc = M->k[c], q = 0;
            /* the six faces: a neighbour, a held face (the wall half a cell away) or nothing */
            const int at[6] = {i > 0, i < nx - 1, j > 0, j < ny - 1, k > 0, k < nz - 1};
            const size_t nb[6] = {c - 1, c + 1, c - sy, c + sy, c - sz, c + sz};
            for (int d = 0; d < 6; d++) {
                if (at[d]) {
                    double kn = M->k[nb[d]];
                    q += 2 * kc * kn / (kc + kn) * (M->T[nb[d]] - Tc) * ih2;
                } else if (s->held_T[d] >= 0) {
                    double g = 2 * kc * (s->held_T[d] - Tc) * ih2;
                    q += g, held -= g * h * h * h * dt;
                }
            }
            if (M->u) { /* the heat carried by the flow: each face's velocity times the upwind enthalpy, limited */
                const double *vel[3] = {M->u, M->v, M->w};
                const size_t fl[3] = {IU(M, i, j, k), IV(M, i, j, k), IW(M, i, j, k)}, fr[3] = {IU(M, i + 1, j, k), IV(M, i, j + 1, k), IW(M, i, j, k + 1)};
                const int ix[3] = {i, j, k}, nn[3] = {nx, ny, nz};
                const size_t st[3] = {1, sy, sz};
                for (int a = 0; a < 3; a++)
                    for (int side = 0; side < 2; side++) {
                        double vf = vel[a][side ? fr[a] : fl[a]];
                        if (vf == 0) continue;
                        /* the cells behind (lo) and ahead (hi) of the face along a */
                        int ilo = side ? ix[a] : ix[a] - 1;
                        if (ilo < 0 || ilo + 1 >= nn[a]) continue;
                        size_t clo = side ? c : c - st[a], chi = clo + st[a];
                        double Hlo = M->H[clo], Hhi = M->H[chi];
                        double Hm = ilo > 0 ? M->H[clo - st[a]] : Hlo, Hp = ilo + 2 < nn[a] ? M->H[chi + st[a]] : Hhi;
                        double x0, s1, s2;
                        if (vf > 0) x0 = Hlo, s1 = Hlo - Hm, s2 = Hhi - Hlo;
                        else x0 = Hhi, s1 = Hhi - Hlo, s2 = Hp - Hhi;
                        double lim = s1 * s2 <= 0 ? 0 : (fabs(s1) < fabs(s2) ? s1 : s2);
                        double Hf = x0 + (vf > 0 ? 0.5 : -0.5) * lim;
                        q += (side ? -1 : 1) * vf * Hf / h; /* in through the low face, out through the high */
                    }
            }
            if (k == nz - 1 && s->held_T[5] < 0) { /* the top surface: losses and the beam */
                double loss = s->h_conv * (Tc - s->T_amb) + s->emissivity * SIGMA * (Tc * Tc * Tc * Tc - s->T_amb * s->T_amb * s->T_amb * s->T_amb);
                q -= loss / h, lost += loss * h * h * dt;
                if (M->on) {
                    double x0 = s->origin[0] + i * h - M->bx, y0 = s->origin[1] + j * h - M->by, w = s->radius;
                    if (fabs(x0 + 0.5 * h) < 4 * w + h && fabs(y0 + 0.5 * h) < 4 * w + h) {
                        double P = s->absorptivity * s->power * erfbox(x0, x0 + h, w) * erfbox(y0, y0 + h, w); /* W into this cell */
                        q += P / (h * h * h), absd += P * dt;
                    }
                }
            }
            M->Hn[c] = M->H[c] + dt * q;
        }
    }
    M->held_t[tid] += held, M->lost_t[tid] += lost, M->abs_t[tid] += absd;
}

/* ---- the flow ------------------------------------------------------------------------------------------------- */

/* Carman-Kozeny, kg/(m^3 s). A face between two cells with no liquid at all is held at rest and closed to the
 * projection: the solid drops out of the pressure problem, which is then the pool's alone (the whole block's took a
 * second a step) */
static inline double drag(const Melt *M, double f) {
    double s = 1 - f;
    return M->s.mushy_C * s * s / (f * f * f + 1e-3);
}

/* a tangential neighbour of a face value: beyond a no-slip wall its negative; beyond the top the Marangoni ghost */
static void momentum(void *vc, int k0, int k1, int tid) {
    Melt *M = vc;
    const MeltSpec *s = &M->s;
    const int nx = M->nx, ny = M->ny, nz = M->nz;
    const double h = s->h, dt = M->dt, nu = s->mu / s->rho, rho = s->rho, ih = 1 / h, ih2 = 1 / (h * h), tau_c = s->dsigma_dT / s->mu * h;
    const double *u = M->u, *v = M->v, *w = M->w, *T = M->T, *f = M->f;
    const double ysl = s->slip_y ? 1 : -1; /* the y walls' ghost sign */
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < ny; j++) {
            /* u faces */
            for (int i = 0; i <= nx; i++) {
                size_t q = IU(M, i, j, k);
                if (i == 0 || i == nx) {
                    M->us[q] = 0, M->cx[q] = 0;
                    continue;
                }
                double uc = u[q];
                double xm = u[q - 1], xp = u[q + 1];
                double ym = j > 0 ? u[IU(M, i, j - 1, k)] : ysl * uc, yp = j < ny - 1 ? u[IU(M, i, j + 1, k)] : ysl * uc;
                double zm = k > 0 ? u[IU(M, i, j, k - 1)] : -uc;
                double zp = k < nz - 1 ? u[IU(M, i, j, k + 1)] : uc + tau_c * (T[CC(M, i, j, k)] - T[CC(M, i - 1, j, k)]) * ih;
                double vv = 0.25 * (v[IV(M, i - 1, j, k)] + v[IV(M, i, j, k)] + v[IV(M, i - 1, j + 1, k)] + v[IV(M, i, j + 1, k)]);
                double ww = 0.25 * (w[IW(M, i - 1, j, k)] + w[IW(M, i, j, k)] + w[IW(M, i - 1, j, k + 1)] + w[IW(M, i, j, k + 1)]);
                double adv = (uc > 0 ? uc * (uc - xm) : uc * (xp - uc)) * ih + (vv > 0 ? vv * (uc - ym) : vv * (yp - uc)) * ih +
                             (ww > 0 ? ww * (uc - zm) : ww * (zp - uc)) * ih;
                double lap = (xm + xp + ym + yp + zm + zp - 6 * uc) * ih2;
                double A = drag(M, 0.5 * (f[CC(M, i - 1, j, k)] + f[CC(M, i, j, k)]));
                M->us[q] = (uc + dt * (-adv + nu * lap)) / (1 + dt * A / rho), M->cx[q] = 1 / (rho + dt * A);
                    if (f[CC(M, i - 1, j, k)] <= 0 && f[CC(M, i, j, k)] <= 0) M->us[q] = 0, M->cx[q] = 0; /* solid on both sides */
            }
            /* v faces */
            for (int jj = j; jj <= (j == ny - 1 ? ny : j); jj++)
                for (int i = 0; i < nx; i++) {
                    size_t q = IV(M, i, jj, k);
                    if (jj == 0 || jj == ny) {
                        M->vs[q] = 0, M->cy[q] = 0;
                        continue;
                    }
                    double vc = v[q];
                    double ym = v[IV(M, i, jj - 1, k)], yp = v[IV(M, i, jj + 1, k)];
                    double xm = i > 0 ? v[q - 1] : -vc, xp = i < nx - 1 ? v[q + 1] : -vc;
                    double zm = k > 0 ? v[IV(M, i, jj, k - 1)] : -vc;
                    double zp = k < nz - 1 ? v[IV(M, i, jj, k + 1)] : vc + tau_c * (T[CC(M, i, jj, k)] - T[CC(M, i, jj - 1, k)]) * ih;
                    double uu = 0.25 * (u[IU(M, i, jj - 1, k)] + u[IU(M, i + 1, jj - 1, k)] + u[IU(M, i, jj, k)] + u[IU(M, i + 1, jj, k)]);
                    double ww = 0.25 * (w[IW(M, i, jj - 1, k)] + w[IW(M, i, jj, k)] + w[IW(M, i, jj - 1, k + 1)] + w[IW(M, i, jj, k + 1)]);
                    double adv = (uu > 0 ? uu * (vc - xm) : uu * (xp - vc)) * ih + (vc > 0 ? vc * (vc - ym) : vc * (yp - vc)) * ih +
                                 (ww > 0 ? ww * (vc - zm) : ww * (zp - vc)) * ih;
                    double lap = (xm + xp + ym + yp + zm + zp - 6 * vc) * ih2;
                    double A = drag(M, 0.5 * (f[CC(M, i, jj - 1, k)] + f[CC(M, i, jj, k)]));
                    M->vs[q] = (vc + dt * (-adv + nu * lap)) / (1 + dt * A / rho), M->cy[q] = 1 / (rho + dt * A);
                    if (f[CC(M, i, jj - 1, k)] <= 0 && f[CC(M, i, jj, k)] <= 0) M->vs[q] = 0, M->cy[q] = 0; /* solid on both sides */
                }
            /* w faces (the bottom and the flat top hold w = 0) */
            for (int kk = k; kk <= (k == nz - 1 ? nz : k); kk++)
                for (int i = 0; i < nx; i++) {
                    size_t q = IW(M, i, j, kk);
                    if (kk == 0 || kk == nz) {
                        M->ws[q] = 0, M->cz[q] = 0;
                        continue;
                    }
                    double wc = w[q];
                    double zm = w[IW(M, i, j, kk - 1)], zp = w[IW(M, i, j, kk + 1)];
                    double xm = i > 0 ? w[q - 1] : -wc, xp = i < nx - 1 ? w[q + 1] : -wc;
                    double ym = j > 0 ? w[IW(M, i, j - 1, kk)] : ysl * wc, yp = j < ny - 1 ? w[IW(M, i, j + 1, kk)] : ysl * wc;
                    double uu = 0.25 * (u[IU(M, i, j, kk - 1)] + u[IU(M, i + 1, j, kk - 1)] + u[IU(M, i, j, kk)] + u[IU(M, i + 1, j, kk)]);
                    double vv = 0.25 * (v[IV(M, i, j, kk - 1)] + v[IV(M, i, j + 1, kk - 1)] + v[IV(M, i, j, kk)] + v[IV(M, i, j + 1, kk)]);
                    double adv = (uu > 0 ? uu * (wc - xm) : uu * (xp - wc)) * ih + (vv > 0 ? vv * (wc - ym) : vv * (yp - wc)) * ih +
                                 (wc > 0 ? wc * (wc - zm) : wc * (zp - wc)) * ih;
                    double lap = (xm + xp + ym + yp + zm + zp - 6 * wc) * ih2;
                    double A = drag(M, 0.5 * (f[CC(M, i, j, kk - 1)] + f[CC(M, i, j, kk)]));
                    M->ws[q] = (wc + dt * (-adv + nu * lap)) / (1 + dt * A / rho), M->cz[q] = 1 / (rho + dt * A);
                    if (f[CC(M, i, j, kk - 1)] <= 0 && f[CC(M, i, j, kk)] <= 0) M->ws[q] = 0, M->cz[q] = 0; /* solid on both sides */
                }
        }
}

/* the pressure in a window around the cells the projection can move (any face open): the rest of the block is rigid
 * and drops out. The window is padded to a multiple of 8 cells where the block allows (the multigrid's levels), its
 * solver kept until the window's size changes. The coefficients are scaled by rho (the liquid's is then 1) and the
 * pressure solved for is rho times the true one. */
static bool solve_window(Melt *M, ThreadPool *pool) {
    const int n[3] = {M->nx, M->ny, M->nz};
    const double h = M->s.h, dt = M->dt, rho = M->s.rho;
    int lo[3] = {n[0], n[1], n[2]}, hi[3] = {-1, -1, -1};
    for (int k = 0; k < n[2]; k++)
        for (int j = 0; j < n[1]; j++)
            for (int i = 0; i < n[0]; i++) {
                bool open = M->cx[IU(M, i, j, k)] > 0 || M->cx[IU(M, i + 1, j, k)] > 0 || M->cy[IV(M, i, j, k)] > 0 || M->cy[IV(M, i, j + 1, k)] > 0 ||
                            M->cz[IW(M, i, j, k)] > 0 || M->cz[IW(M, i, j, k + 1)] > 0;
                if (!open) continue;
                const int ix[3] = {i, j, k};
                for (int a = 0; a < 3; a++) lo[a] = ix[a] < lo[a] ? ix[a] : lo[a], hi[a] = ix[a] > hi[a] ? ix[a] : hi[a];
            }
    if (hi[0] < 0) return true; /* nothing can move */
    int wn[3], o[3];
    for (int a = 0; a < 3; a++) {
        int want = hi[a] - lo[a] + 1;
        want = ((want + 7) / 8) * 8;
        if (want > n[a]) want = n[a];
        int start = (lo[a] + hi[a] + 1 - want) / 2;
        if (start < 0) start = 0;
        if (start + want > n[a]) start = n[a] - want;
        wn[a] = want, o[a] = start;
    }
    if (!M->mg || wn[0] != M->wn[0] || wn[1] != M->wn[1] || wn[2] != M->wn[2]) {
        mg3d_free(M->mg);
        free(M->wcx), free(M->wcy), free(M->wcz), free(M->wp), free(M->wr);
        const int bc[6] = {MG_NEUMANN, MG_NEUMANN, MG_NEUMANN, MG_NEUMANN, MG_NEUMANN, MG_NEUMANN};
        char err[160];
        M->mg = mg3d_create(wn[0], wn[1], wn[2], h, bc, err, sizeof err);
        size_t c = (size_t)wn[0] * wn[1] * wn[2];
        M->wcx = malloc((size_t)(wn[0] + 1) * wn[1] * wn[2] * 8), M->wcy = malloc((size_t)wn[0] * (wn[1] + 1) * wn[2] * 8);
        M->wcz = malloc((size_t)wn[0] * wn[1] * (wn[2] + 1) * 8), M->wp = calloc(c, 8), M->wr = malloc(c * 8);
        memcpy(M->wn, wn, sizeof wn);
        if (!M->mg || !M->wcx || !M->wcy || !M->wcz || !M->wp || !M->wr) return false;
    }
    const int X = wn[0], Y = wn[1], Z = wn[2];
    for (int k = 0; k < Z; k++)
        for (int j = 0; j < Y; j++) {
            for (int i = 0; i <= X; i++) /* the window's own boundary faces: closed unless the block's face is open (it never is) */
                M->wcx[i + (size_t)(X + 1) * (j + (size_t)Y * k)] = (i == 0 || i == X) ? 0 : rho * M->cx[IU(M, o[0] + i, o[1] + j, o[2] + k)];
            for (int i = 0; i < X; i++) {
                size_t wc = i + (size_t)X * (j + (size_t)Y * k), c = CC(M, o[0] + i, o[1] + j, o[2] + k);
                M->wr[wc] = rho * M->rhs[c], M->wp[wc] = M->p[c];
            }
        }
    for (int k = 0; k < Z; k++)
        for (int j = 0; j <= Y; j++)
            for (int i = 0; i < X; i++)
                M->wcy[i + (size_t)X * (j + (size_t)(Y + 1) * k)] = (j == 0 || j == Y) ? 0 : rho * M->cy[IV(M, o[0] + i, o[1] + j, o[2] + k)];
    for (int k = 0; k <= Z; k++)
        for (int j = 0; j < Y; j++)
            for (int i = 0; i < X; i++)
                M->wcz[i + (size_t)X * (j + (size_t)Y * k)] = (k == 0 || k == Z) ? 0 : rho * M->cz[IW(M, o[0] + i, o[1] + j, o[2] + k)];
    if (!mg3d_set_coefficients(M->mg, M->wcx, M->wcy, M->wcz)) return false;
    /* to 1e-12 of the right side: at 1e-10 the fastest liquid kept a divergence of 1e-7 of its speed (melttest M6's
     * first run) */
    int it = mg3d_solve(M->mg, M->wp, M->wr, 1e-12, 80, pool);
    if (it < 0 && mg3d_last_residual(M->mg) > 1e-6) return false;
    for (int k = 0; k < Z; k++)
        for (int j = 0; j < Y; j++)
            for (int i = 0; i < X; i++) M->p[CC(M, o[0] + i, o[1] + j, o[2] + k)] = M->wp[i + (size_t)X * (j + (size_t)Y * k)];
    /* the correction on the open faces (all inside the window): u = u* - dt c grad(p_true) with p = rho p_true */
    for (int k = 0; k < Z; k++)
        for (int j = 0; j < Y; j++)
            for (int i = 0; i < X; i++) {
                int I = o[0] + i, J = o[1] + j, K = o[2] + k;
                size_t c = CC(M, I, J, K);
                if (i > 0) {
                    size_t q = IU(M, I, J, K);
                    if (M->cx[q] > 0) M->us[q] -= dt * M->cx[q] * (M->p[c] - M->p[c - 1]) / h;
                }
                if (j > 0) {
                    size_t q = IV(M, I, J, K);
                    if (M->cy[q] > 0) M->vs[q] -= dt * M->cy[q] * (M->p[c] - M->p[c - (size_t)M->nx]) / h;
                }
                if (k > 0) {
                    size_t q = IW(M, I, J, K);
                    if (M->cz[q] > 0) M->ws[q] -= dt * M->cz[q] * (M->p[c] - M->p[c - (size_t)M->nx * M->ny]) / h;
                }
            }
    return true;
}

/* the projection: div(c grad p) = div u* / dt, then u = u* - dt c grad p */
static bool project(Melt *M, ThreadPool *pool) {
    const int nx = M->nx, ny = M->ny, nz = M->nz;
    const double h = M->s.h, dt = M->dt;
    double rmax = 0;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                double d = (M->us[IU(M, i + 1, j, k)] - M->us[IU(M, i, j, k)] + M->vs[IV(M, i, j + 1, k)] - M->vs[IV(M, i, j, k)] + M->ws[IW(M, i, j, k + 1)] -
                            M->ws[IW(M, i, j, k)]) / h;
                M->rhs[CC(M, i, j, k)] = d / dt;
                rmax = fmax(rmax, fabs(d));
            }
    if (rmax > 0 && !solve_window(M, pool)) return false;
    double *t;
    t = M->u, M->u = M->us, M->us = t;
    t = M->v, M->v = M->vs, M->vs = t;
    t = M->w, M->w = M->ws, M->ws = t;
    /* the check, and the fastest face */
    double dmax = 0, um = 0;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                double d = (M->u[IU(M, i + 1, j, k)] - M->u[IU(M, i, j, k)] + M->v[IV(M, i, j + 1, k)] - M->v[IV(M, i, j, k)] + M->w[IW(M, i, j, k + 1)] -
                            M->w[IW(M, i, j, k)]);
                dmax = fmax(dmax, fabs(d));
                um = fmax(um, fmax(fabs(M->u[IU(M, i, j, k)]), fmax(fabs(M->v[IV(M, i, j, k)]), fabs(M->w[IW(M, i, j, k)]))));
            }
    M->umax = um, M->div_err = um > 0 ? dmax / um : dmax;
    return true;
}

void melt_velocity(const Melt *M, int i, int j, int k, double vel[3]) {
    if (!M->u) {
        vel[0] = vel[1] = vel[2] = 0;
        return;
    }
    vel[0] = 0.5 * (M->u[IU(M, i, j, k)] + M->u[IU(M, i + 1, j, k)]), vel[1] = 0.5 * (M->v[IV(M, i, j, k)] + M->v[IV(M, i, j + 1, k)]);
    vel[2] = 0.5 * (M->w[IW(M, i, j, k)] + M->w[IW(M, i, j, k + 1)]);
}
double melt_max_speed(const Melt *M) { return M->umax; }
double melt_divergence_error(const Melt *M) { return M->div_err; }

void melt_step(Melt *M, double dt, ThreadPool *pool) {
    int rows = M->ny * M->nz;
    M->dt = dt;
    M->on = melt_beam_at(M, M->t + 0.5 * dt, &M->bx, &M->by);
    pool_for(pool, rows, 16, state, M);
    if (M->u) {
        pool_for(pool, M->nz, 1, momentum, M);
        if (!project(M, pool)) fprintf(stderr, "melt: the pressure did not converge at t = %.4g s\n", M->t);
    }
    memset(M->held_t, 0, sizeof M->held_t), memset(M->lost_t, 0, sizeof M->lost_t), memset(M->abs_t, 0, sizeof M->abs_t);
    pool_for(pool, rows, 16, update, M);
    for (int t = 0; t < MAXT; t++) M->held_out += M->held_t[t], M->lost += M->lost_t[t], M->absorbed += M->abs_t[t];
    double *x = M->H;
    M->H = M->Hn, M->Hn = x;
    M->t += dt;
}

void melt_fields(const Melt *M, float *T, float *f, float *fmx) {
    for (size_t c = 0; c < M->n; c++) {
        double H = M->H[c], fr = H <= M->Hs ? 0 : H >= M->Hl ? 1 : (H - M->Hs) / (M->Hl - M->Hs);
        if (T) T[c] = (float)melt_T_of_H(M, H);
        if (f) f[c] = (float)fr;
        if (fmx) {
            double Hm = fmax(M->Hmax[c], H);
            fmx[c] = (float)(Hm <= M->Hs ? 0 : Hm >= M->Hl ? 1 : (Hm - M->Hs) / (M->Hl - M->Hs));
        }
    }
}

void melt_energy(const Melt *M, double *absorbed, double *surface_loss, double *held_out) {
    if (absorbed) *absorbed = M->absorbed;
    if (surface_loss) *surface_loss = M->lost;
    if (held_out) *held_out = M->held_out;
}

bool melt_track_size(const Melt *M, double x0, double x1, double *width, double *depth) {
    const double h = M->s.h;
    double wmax = 0, dmax = 0;
    for (int i = 0; i < M->nx; i++) {
        double xc = M->s.origin[0] + (i + 0.5) * h;
        if (xc < x0 || xc > x1) continue;
        int jlo = M->ny, jhi = -1, klo = M->nz;
        for (int k = 0; k < M->nz; k++)
            for (int j = 0; j < M->ny; j++) {
                size_t c = ((size_t)k * M->ny + j) * M->nx + i;
                double H = fmax(M->Hmax[c], M->H[c]), fr = H <= M->Hs ? 0 : H >= M->Hl ? 1 : (H - M->Hs) / (M->Hl - M->Hs);
                if (fr < 0.5) continue;
                if (j < jlo) jlo = j;
                if (j > jhi) jhi = j;
                if (k < klo) klo = k;
            }
        if (jhi < 0) continue;
        wmax = fmax(wmax, (jhi - jlo + 1) * h), dmax = fmax(dmax, (M->nz - klo) * h);
    }
    if (width) *width = wmax;
    if (depth) *depth = dmax;
    return wmax > 0;
}

bool melt_track_section(const Melt *M, double x0, double x1, bool mirror_y, double *width, double *depth, double *width_max, double *depth_max,
                        bool *touches) {
    const double h = M->s.h, Hh = 0.5 * (M->Hs + M->Hl), top = M->s.origin[2] + M->nz * h;
    double wsum = 0, dsum = 0, wmax = 0, dmax = 0;
    int nsec = 0;
    bool edge = false;
    for (int i = 0; i < M->nx; i++) {
        double xc = M->s.origin[0] + (i + 0.5) * h;
        if (xc < x0 || xc > x1) continue;
        double ylo = INFINITY, yhi = -INFINITY, zlo = INFINITY;
        for (int k = 0; k < M->nz; k++)
            for (int j = 0; j < M->ny; j++) {
                double Hc = fmax(M->Hmax[CC(M, i, j, k)], M->H[CC(M, i, j, k)]);
                if (Hc < Hh) continue;
                double yc = M->s.origin[1] + (j + 0.5) * h, zc = M->s.origin[2] + (k + 0.5) * h;
                /* the boundary between this cell's centre and a neighbour's that stayed below: linear in between */
                if (j + 1 < M->ny) {
                    double Hn = fmax(M->Hmax[CC(M, i, j + 1, k)], M->H[CC(M, i, j + 1, k)]);
                    if (Hn < Hh) yhi = fmax(yhi, yc + h * (Hc - Hh) / (Hc - Hn));
                } else yhi = fmax(yhi, yc + 0.5 * h), edge = true;
                if (j > 0) {
                    double Hn = fmax(M->Hmax[CC(M, i, j - 1, k)], M->H[CC(M, i, j - 1, k)]);
                    if (Hn < Hh) ylo = fmin(ylo, yc - h * (Hc - Hh) / (Hc - Hn));
                } else {
                    ylo = fmin(ylo, mirror_y ? M->s.origin[1] : yc - 0.5 * h);
                    if (!mirror_y) edge = true;
                }
                if (k > 0) {
                    double Hn = fmax(M->Hmax[CC(M, i, j, k - 1)], M->H[CC(M, i, j, k - 1)]);
                    if (Hn < Hh) zlo = fmin(zlo, zc - h * (Hc - Hh) / (Hc - Hn));
                } else zlo = fmin(zlo, zc - 0.5 * h), edge = true;
            }
        if (!(yhi > -INFINITY)) continue;
        double w = mirror_y ? 2 * (yhi - M->s.origin[1]) : yhi - ylo, d = top - zlo;
        wsum += w, dsum += d, wmax = fmax(wmax, w), dmax = fmax(dmax, d), nsec++;
    }
    if (width) *width = nsec ? wsum / nsec : 0;
    if (depth) *depth = nsec ? dsum / nsec : 0;
    if (width_max) *width_max = wmax;
    if (depth_max) *depth_max = dmax;
    if (touches) *touches = edge;
    return nsec > 0;
}

void melt_pool_size(const Melt *M, double *length, double *width, double *depth) {
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    const double h = M->s.h;
    for (int k = 0; k < M->nz; k++)
        for (int j = 0; j < M->ny; j++)
            for (int i = 0; i < M->nx; i++) {
                size_t c = ((size_t)k * M->ny + j) * M->nx + i;
                double H = M->H[c];
                if (H < 0.5 * (M->Hs + M->Hl)) continue;
                double p[3] = {i * h, j * h, k * h};
                for (int a = 0; a < 3; a++) lo[a] = fmin(lo[a], p[a]), hi[a] = fmax(hi[a], p[a]);
            }
    bool any = hi[0] >= lo[0];
    if (length) *length = any ? hi[0] - lo[0] + h : 0;
    if (width) *width = any ? hi[1] - lo[1] + h : 0;
    if (depth) *depth = any ? M->nz * h - lo[2] : 0;
}
