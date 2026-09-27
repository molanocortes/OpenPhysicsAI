/* pack.c - a module of cylindrical cells in parallel on a cold plate (pack.h). */
#include "pack.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { M_GAP = -2, M_PLATE = -1 }; /* a cube's material; 0.. a cell */

struct Pack {
    PackSpec s;
    int n[3], ncell;
    size_t nc;
    double o[3], dx;
    int *mat;                          /* per cube */
    double *T, *Told, *rc, *gx, *gy, *gz, *gcool, *gamb, *src, *diag, *r, *z, *p, *q;
    double *Tc;                        /* the coolant under each column of cubes along x, K */
    int *top;                          /* per cell: its top layer's z index */
    int *ncubes;                       /* per cell: how many cubes */
    Dfn **cells;
    double *I, *Rx, *slope, *Vc, *Tm, *Tx, *Q;
    double t, V, made, to_cool, to_air, dV, dI;
};

static inline size_t id(const Pack *P, int i, int j, int k) { return (size_t)i + (size_t)P->n[0] * ((size_t)j + (size_t)P->n[1] * k); }

void pack_free(Pack *P) {
    if (!P) return;
    for (int c = 0; P->cells && c < P->ncell; c++) dfn_free(P->cells[c]);
    free(P->cells);
    double **arr[] = {&P->T, &P->Told, &P->rc, &P->gx, &P->gy, &P->gz, &P->gcool, &P->gamb, &P->src, &P->diag, &P->r, &P->z, &P->p, &P->q, &P->Tc,
                      &P->I, &P->Rx, &P->slope, &P->Vc, &P->Tm, &P->Tx, &P->Q};
    for (size_t a = 0; a < sizeof arr / sizeof arr[0]; a++) free(*arr[a]);
    free(P->mat), free(P->top), free(P->ncubes);
    free(P);
}

Pack *pack_create(const PackSpec *s, char *err, size_t errlen) {
    if (s->rows < 1 || s->cols < 1 || !(s->pitch >= 2 * s->radius) || !(s->radius > 0) || !(s->height > 0) || !(s->plate > 0) || !(s->dx > 0) ||
        !(s->kr > 0) || !(s->kz > 0) || !(s->rc_cell > 0) || !(s->k_plate > 0) || !(s->rc_plate > 0) || !(s->k_gap > 0) || !(s->rc_gap > 0) ||
        s->h_cool < 0 || s->h_amb < 0 || !(s->mcp_cool > 0) || s->R_ext < 0) {
        snprintf(err, errlen, "pack: rows, columns, a pitch at least a diameter, the cells' size, a plate, a grid, the materials and the "
                              "cooling are required");
        return NULL;
    }
    Pack *P = calloc(1, sizeof *P);
    if (!P) return NULL;
    P->s = *s, P->dx = s->dx, P->ncell = s->rows * s->cols;
    double L[3] = {s->cols * s->pitch + 2 * s->margin, s->rows * s->pitch + 2 * s->margin, s->plate + s->height};
    /* the grid centred on the module across (a cell's cubes then mirror its twin's), from the plate's underside up */
    for (int a = 0; a < 3; a++) P->n[a] = (int)ceil(L[a] / s->dx - 1e-9), P->o[a] = a < 2 ? -0.5 * (P->n[a] * s->dx - L[a]) : 0;
    P->nc = (size_t)P->n[0] * P->n[1] * P->n[2];
    size_t nc = P->nc;
    double **cube[] = {&P->T, &P->Told, &P->rc, &P->gx, &P->gy, &P->gz, &P->gcool, &P->gamb, &P->src, &P->diag, &P->r, &P->z, &P->p, &P->q};
    for (size_t a = 0; a < sizeof cube / sizeof cube[0]; a++) *cube[a] = calloc(nc, 8);
    P->mat = malloc(nc * sizeof(int)), P->Tc = calloc(P->n[0], 8);
    int nk = P->ncell;
    P->top = calloc(nk, sizeof(int)), P->ncubes = calloc(nk, sizeof(int)), P->cells = calloc(nk, sizeof(Dfn *));
    double **cellv[] = {&P->I, &P->Rx, &P->slope, &P->Vc, &P->Tm, &P->Tx, &P->Q};
    for (size_t a = 0; a < sizeof cellv / sizeof cellv[0]; a++) *cellv[a] = calloc(nk, 8);
    bool ok = P->mat && P->Tc && P->top && P->ncubes && P->cells;
    for (size_t a = 0; a < sizeof cube / sizeof cube[0]; a++) ok = ok && *cube[a];
    for (size_t a = 0; a < sizeof cellv / sizeof cellv[0]; a++) ok = ok && *cellv[a];
    if (!ok) {
        snprintf(err, errlen, "pack: out of memory");
        pack_free(P);
        return NULL;
    }
    const double dx = s->dx;
    /* the materials: plate below, cells standing on it, gap between */
    for (int k = 0; k < P->n[2]; k++)
        for (int j = 0; j < P->n[1]; j++)
            for (int i = 0; i < P->n[0]; i++) {
                double x = P->o[0] + (i + 0.5) * dx, y = P->o[1] + (j + 0.5) * dx, z = (k + 0.5) * dx;
                int m = M_GAP;
                if (z < s->plate) m = M_PLATE;
                else if (z < s->plate + s->height) {
                    int c = (int)floor((x - s->margin) / s->pitch), r = (int)floor((y - s->margin) / s->pitch);
                    if (c >= 0 && c < s->cols && r >= 0 && r < s->rows) {
                        double cx = s->margin + (c + 0.5) * s->pitch, cy = s->margin + (r + 0.5) * s->pitch;
                        if (hypot(x - cx, y - cy) <= s->radius) m = r * s->cols + c;
                    }
                }
                size_t q = id(P, i, j, k);
                P->mat[q] = m, P->T[q] = s->T0;
                if (m >= 0) P->ncubes[m]++, P->top[m] = k > P->top[m] ? k : P->top[m];
            }
    for (int c = 0; c < nk; c++)
        if (!P->ncubes[c]) {
            snprintf(err, errlen, "pack: the grid (%g m) is too coarse for the cells", dx);
            pack_free(P);
            return NULL;
        }
    /* conductances: each face's two half-cubes in series, along the face's normal */
    for (int k = 0; k < P->n[2]; k++)
        for (int j = 0; j < P->n[1]; j++)
            for (int i = 0; i < P->n[0]; i++) {
                size_t q = id(P, i, j, k);
                int m = P->mat[q];
                double kxy = m >= 0 ? s->kr : m == M_PLATE ? s->k_plate : s->k_gap, kzz = m >= 0 ? s->kz : kxy;
                P->rc[q] = (m >= 0 ? s->rc_cell : m == M_PLATE ? s->rc_plate : s->rc_gap) * dx * dx * dx;
                double kn[3] = {kxy, kxy, kzz};
                for (int a = 0; a < 3; a++) {
                    int ii = i + (a == 0), jj = j + (a == 1), kk = k + (a == 2);
                    double *g = a == 0 ? P->gx : a == 1 ? P->gy : P->gz;
                    if (ii >= P->n[0] || jj >= P->n[1] || kk >= P->n[2]) continue;
                    size_t q2 = id(P, ii, jj, kk);
                    int m2 = P->mat[q2];
                    double k2xy = m2 >= 0 ? s->kr : m2 == M_PLATE ? s->k_plate : s->k_gap, k2 = a == 2 && m2 >= 0 ? s->kz : k2xy;
                    g[q] = 2 * dx / (1 / kn[a] + 1 / k2);
                }
                /* the outer faces: the plate's underside to the coolant, the rest to the air */
                double ga = 0;
                const bool side[6] = {i == 0, i == P->n[0] - 1, j == 0, j == P->n[1] - 1, k == 0, k == P->n[2] - 1};
                for (int f = 0; f < 6; f++) {
                    if (!side[f]) continue;
                    double kk = f < 4 ? kxy : kzz;
                    if (f == 4) P->gcool[q] = s->h_cool > 0 ? dx * dx / (dx / (2 * kk) + 1 / s->h_cool) : 0;
                    else if (s->h_amb > 0) ga += dx * dx / (dx / (2 * kk) + 1 / s->h_amb);
                }
                P->gamb[q] = ga;
            }
    for (int i = 0; i < P->n[0]; i++) P->Tc[i] = s->T_cool_in;
    for (int c = 0; c < nk; c++) {
        P->cells[c] = dfn_create(&s->cell, err, errlen);
        if (!P->cells[c]) {
            pack_free(P);
            return NULL;
        }
        P->Rx[c] = s->R_ext, P->Tm[c] = P->Tx[c] = s->T0, P->Vc[c] = dfn_voltage(P->cells[c]);
    }
    P->V = P->Vc[0];
    return P;
}

int pack_cells(const Pack *P) { return P->ncell; }
bool pack_set_resistance(Pack *P, int c, double R) {
    if (c < 0 || c >= P->ncell || R < 0) return false;
    P->Rx[c] = R;
    return true;
}

/* ---- conduction -------------------------------------------------------------------------------------------------- */

/* y = A x: the implicit operator (heat capacity over dt, the faces, the outer faces) */
static void apply(const Pack *P, const double *x, double *y, double dt) {
    const int nx = P->n[0], ny = P->n[1], nz = P->n[2];
    const size_t sy = nx, sz = (size_t)nx * ny;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                size_t q = id(P, i, j, k);
                double v = (P->rc[q] / dt + P->gcool[q] + P->gamb[q]) * x[q];
                if (i < nx - 1) v += P->gx[q] * (x[q] - x[q + 1]);
                if (i > 0) v += P->gx[q - 1] * (x[q] - x[q - 1]);
                if (j < ny - 1) v += P->gy[q] * (x[q] - x[q + sy]);
                if (j > 0) v += P->gy[q - sy] * (x[q] - x[q - sy]);
                if (k < nz - 1) v += P->gz[q] * (x[q] - x[q + sz]);
                if (k > 0) v += P->gz[q - sz] * (x[q] - x[q - sz]);
                y[q] = v;
            }
}

static bool conduct(Pack *P, double dt) {
    const size_t nc = P->nc;
    const int nx = P->n[0], ny = P->n[1], nz = P->n[2];
    const size_t sy = nx, sz = (size_t)nx * ny;
    memcpy(P->Told, P->T, nc * 8);
    /* the right side, and the diagonal for the preconditioner */
    double *b = P->q;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                size_t q = id(P, i, j, k);
                b[q] = P->rc[q] / dt * P->Told[q] + P->src[q] + P->gcool[q] * P->Tc[i] + P->gamb[q] * P->s.T_amb;
                double d = P->rc[q] / dt + P->gcool[q] + P->gamb[q];
                if (i < nx - 1) d += P->gx[q];
                if (i > 0) d += P->gx[q - 1];
                if (j < ny - 1) d += P->gy[q];
                if (j > 0) d += P->gy[q - sy];
                if (k < nz - 1) d += P->gz[q];
                if (k > 0) d += P->gz[q - sz];
                P->diag[q] = d;
            }
    /* conjugate gradients from the last temperatures */
    double *r = P->r, *z = P->z, *p = P->p, *Ap = malloc(nc * 8), *w = Ap;
    if (!Ap) return false;
    apply(P, P->T, Ap, dt);
    double bn = 0, rz = 0;
    for (size_t q = 0; q < nc; q++) r[q] = b[q] - Ap[q], z[q] = r[q] / P->diag[q], p[q] = z[q], rz += r[q] * z[q], bn += b[q] * b[q];
    bn = sqrt(bn);
    bool ok = false;
    for (int it = 0; it < 5000; it++) {
        double rn = 0;
        for (size_t q = 0; q < nc; q++) rn += r[q] * r[q];
        if (sqrt(rn) <= 1e-12 * bn) {
            ok = true;
            break;
        }
        apply(P, p, Ap, dt);
        double pAp = 0;
        for (size_t q = 0; q < nc; q++) pAp += p[q] * Ap[q];
        double alpha = rz / pAp, rz2 = 0;
        for (size_t q = 0; q < nc; q++) P->T[q] += alpha * p[q], r[q] -= alpha * Ap[q], z[q] = r[q] / P->diag[q], rz2 += r[q] * z[q];
        double beta = rz2 / rz;
        rz = rz2;
        for (size_t q = 0; q < nc; q++) p[q] = z[q] + beta * p[q];
    }
    free(w);
    if (!ok) return false;
    /* what left: to the coolant (which warms along x as it goes) and to the air */
    double tc = 0, ta = 0, s0 = 0;
    for (size_t q = 0; q < nc; q++) s0 += P->src[q];
    double upstream = 0;
    for (int i = 0; i < nx; i++) {
        double col = 0;
        for (int j = 0; j < ny; j++) {
            size_t q = id(P, i, j, 0);
            col += P->gcool[q] * (P->T[q] - P->Tc[i]);
        }
        tc += col;
        /* the coolant under this column for the next step: the inlet's plus what the columns before took, half of this one */
        double next = P->s.T_cool_in + (upstream + 0.5 * col) / P->s.mcp_cool;
        upstream += col;
        P->Tc[i] = next;
    }
    for (size_t q = 0; q < nc; q++) ta += P->gamb[q] * (P->T[q] - P->s.T_amb);
    P->made += s0 * dt, P->to_cool += tc * dt, P->to_air += ta * dt;
    return true;
}

bool pack_thermal_step(Pack *P, const double *src_w, double dt) {
    memcpy(P->src, src_w, P->nc * 8);
    if (!conduct(P, dt)) return false;
    P->t += dt;
    return true;
}

/* ---- the module ------------------------------------------------------------------------------------------------ */

bool pack_step(Pack *P, double Itot, double dt) {
    const int n = P->ncell;
    /* each cell's temperature: the mean over its cubes */
    for (int c = 0; c < n; c++) P->Tm[c] = 0, P->Tx[c] = 0;
    for (size_t q = 0; q < P->nc; q++)
        if (P->mat[q] >= 0) P->Tm[P->mat[q]] += P->T[q] / P->ncubes[P->mat[q]], P->Tx[P->mat[q]] = fmax(P->Tx[P->mat[q]], P->T[q]);
    /* the split: every cell's V_k(I_k) - I_k R_k equal, the currents adding up to the module's */
    if (P->t == 0 || Itot == 0)
        for (int c = 0; c < n; c++) P->I[c] = Itot / n;
    else {
        double sum = 0;
        for (int c = 0; c < n; c++) sum += P->I[c];
        for (int c = 0; c < n; c++) P->I[c] = sum != 0 ? P->I[c] * Itot / sum : Itot / n;
    }
    for (int c = 0; c < n; c++) { /* the slope dV/dI by a secant */
        double V0, V1, d = fmax(0.01, 1e-3 * fabs(P->I[c]));
        if (!dfn_try(P->cells[c], P->I[c], P->Tm[c], dt, &V0) || !dfn_try(P->cells[c], P->I[c] + d, P->Tm[c], dt, &V1)) return false;
        P->slope[c] = (V1 - V0) / d;
        P->Vc[c] = V0;
    }
    double V = 0;
    for (int it = 0; it < 12; it++) {
        double s1 = 0, sw = 0, si = 0;
        for (int c = 0; c < n; c++) {
            if (it > 0 && !dfn_try(P->cells[c], P->I[c], P->Tm[c], dt, &P->Vc[c])) return false;
            double e = P->slope[c] - P->Rx[c], W = P->Vc[c] - P->I[c] * P->Rx[c];
            s1 += 1 / e, sw += W / e, si += P->I[c];
        }
        V = (Itot - si + sw) / s1;
        double worst = 0;
        for (int c = 0; c < n; c++) {
            double e = P->slope[c] - P->Rx[c], W = P->Vc[c] - P->I[c] * P->Rx[c], d = (V - W) / e;
            P->I[c] += d, worst = fmax(worst, fabs(d));
        }
        if (worst < 1e-10 * fmax(1.0, fabs(Itot) / n)) break;
    }
    /* take the step in every cell, and its heat into the cubes */
    double vmin = 1e30, vmax = -1e30, sum = 0;
    for (int c = 0; c < n; c++) {
        if (!dfn_step(P->cells[c], P->I[c], P->Tm[c], dt)) return false;
        double v = dfn_voltage(P->cells[c]) - P->I[c] * P->Rx[c];
        vmin = fmin(vmin, v), vmax = fmax(vmax, v), sum += P->I[c];
        P->Vc[c] = dfn_voltage(P->cells[c]);
        P->Q[c] = dfn_heat(P->cells[c]) + P->I[c] * P->I[c] * P->Rx[c];
    }
    P->dV = vmax - vmin, P->dI = sum - Itot, P->V = 0.5 * (vmin + vmax);
    int *ntop = calloc(n, sizeof(int));
    if (!ntop) return false;
    for (size_t q = 0; q < P->nc; q++)
        if (P->mat[q] >= 0 && (int)(q / ((size_t)P->n[0] * P->n[1])) == P->top[P->mat[q]]) ntop[P->mat[q]]++;
    for (size_t q = 0; q < P->nc; q++) {
        int m = P->mat[q];
        P->src[q] = 0;
        if (m < 0) continue;
        P->src[q] = dfn_heat(P->cells[m]) / P->ncubes[m];
        if ((int)(q / ((size_t)P->n[0] * P->n[1])) == P->top[m]) P->src[q] += P->I[m] * P->I[m] * P->Rx[m] / ntop[m];
    }
    free(ntop);
    if (!conduct(P, dt)) return false;
    P->t += dt;
    return true;
}

double pack_time(const Pack *P) { return P->t; }
double pack_voltage(const Pack *P) { return P->V; }
void pack_cell_state(const Pack *P, int c, double *I, double *Tm, double *Tx, double *heat, double *V) {
    if (I) *I = P->I[c];
    if (Tm) *Tm = P->Tm[c];
    if (Tx) *Tx = P->Tx[c];
    if (heat) *heat = P->Q[c];
    if (V) *V = P->Vc[c];
}
double pack_coolant_out(const Pack *P) { return P->Tc[P->n[0] - 1]; }
void pack_energy(const Pack *P, double *made, double *stored, double *to_cool, double *to_air) {
    double st = 0;
    for (size_t q = 0; q < P->nc; q++) st += P->rc[q] * (P->T[q] - P->s.T0);
    if (made) *made = P->made;
    if (stored) *stored = st;
    if (to_cool) *to_cool = P->to_cool;
    if (to_air) *to_air = P->to_air;
}
void pack_grid(const Pack *P, int n[3], double o[3], double *dx) {
    for (int a = 0; a < 3; a++) n[a] = P->n[a], o[a] = P->o[a];
    *dx = P->dx;
}
const double *pack_T(const Pack *P) { return P->T; }
double pack_T_at(const Pack *P, double x, double y, double z) {
    double f[3] = {(x - P->o[0]) / P->dx - 0.5, (y - P->o[1]) / P->dx - 0.5, (z - P->o[2]) / P->dx - 0.5};
    int i0[3];
    double w[3];
    for (int a = 0; a < 3; a++) {
        if (f[a] < 0) f[a] = 0;
        if (f[a] > P->n[a] - 1) f[a] = P->n[a] - 1;
        i0[a] = (int)floor(f[a]);
        if (i0[a] > P->n[a] - 2) i0[a] = P->n[a] - 2 < 0 ? 0 : P->n[a] - 2;
        w[a] = f[a] - i0[a];
    }
    double v = 0;
    for (int c = 0; c < 8; c++) {
        int ii = i0[0] + (c & 1), jj = i0[1] + ((c >> 1) & 1), kk = i0[2] + (c >> 2);
        if (ii >= P->n[0]) ii = P->n[0] - 1;
        if (jj >= P->n[1]) jj = P->n[1] - 1;
        if (kk >= P->n[2]) kk = P->n[2] - 1;
        v += ((c & 1) ? w[0] : 1 - w[0]) * (((c >> 1) & 1) ? w[1] : 1 - w[1]) * ((c >> 2) ? w[2] : 1 - w[2]) * P->T[id(P, ii, jj, kk)];
    }
    return v;
}
void pack_cell_xy(const Pack *P, int c, double *x, double *y) {
    *x = P->s.margin + (c % P->s.cols + 0.5) * P->s.pitch, *y = P->s.margin + (c / P->s.cols + 0.5) * P->s.pitch;
}
double pack_plate_top(const Pack *P) { return P->s.plate; }
void pack_split_error(const Pack *P, double *dV, double *dI) {
    if (dV) *dV = P->dV;
    if (dI) *dI = P->dI;
}
