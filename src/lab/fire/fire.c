/* fire.c - low-Mach reacting flow for fires and rooms (fire.h). */
#include "fire.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../mg/mg3d.h"

static const double RU = 8.314462618, W_AIR = 28.96e-3, PRT = 0.5;

/* specific heats J/(kg K) at 300, 500, 1000, 1500, 2000, 2500 and 3000 K: air (0.232 O2 + 0.768 N2 by mass), the lumped
 * products of methane in air (0.151 CO2, 0.124 H2O, 0.725 N2), methane; from NIST-JANAF values of each gas, rounded
 * (docs/lab/fire.md) */
static const double CP_T[7] = {300, 500, 1000, 1500, 2000, 2500, 3000};
static const double CP_AIR[7] = {1012, 1036, 1149, 1223, 1265, 1289, 1308};
static const double CP_PROD[7] = {1113, 1165, 1316, 1426, 1490, 1526, 1548};
static const double CP_FUEL[7] = {2226, 2838, 4190, 4970, 5400, 5600, 5700};
static inline double tab(const double *y, double T) {
    if (T <= CP_T[0]) return y[0];
    for (int i = 0; i < 6; i++)
        if (T <= CP_T[i + 1]) return y[i] + (y[i + 1] - y[i]) * (T - CP_T[i]) / (CP_T[i + 1] - CP_T[i]);
    return y[6];
}
static inline double cp_mix(double T, double yf, double yp) {
    double ya = 1 - yf - yp;
    return yf * tab(CP_FUEL, T) + yp * tab(CP_PROD, T) + (ya > 0 ? ya : 0) * tab(CP_AIR, T);
}

/* the kinds of face: evolved by the momentum equation, held at a speed (a wall, a vent), open (on the box's side, the
 * pressure perturbation zero), closed by a solid (held at rest, no slip along it), a fan's (forced to the fan's speed
 * before each projection but left open to it: holding them closed cut a rack into slabs the pressure solver's coarse
 * grids could not represent, and it stalled) */
enum { K_FLUID = 0, K_HELD = 1, K_OPEN = 2, K_SOLID = 3, K_FAN = 4 };
#define MAXV 64

typedef struct {
    int side;
    double rect[4], speed, T, fuel, hrr;
} Vent;
typedef struct {
    double box[6], speed;
    int axis;
} Fan;
typedef struct {
    int side;
    double rect[4];
} Opening;

struct Fire {
    FireSpec s;
    int nx, ny, nz;
    size_t nc, nu, nv, nw;
    double *rho, *YF, *YP, *u, *v, *w;       /* the state */
    double *rho0, *YF0, *YP0, *u0, *v0, *w0; /* the step's start */
    double *rho1, *YF1, *YP1, *u1, *v1, *w1; /* a stage's result */
    double *T, *Wm, *nut, *Sm, *D, *q, *H, *rhs, *Hs;
    double *AG, *AG0, *AG1;                  /* the age of the air, s (a passive scalar growing 1 s per s, 0 where air comes in) */
    const double *agi;                       /* a stage's age in and out */
    double *ago;
    double *bx, *by, *bz;                    /* 1 / rho on the faces, for the projection (0 on closed faces) */
    double *qs, *Ts;                         /* heat sources W/m^3; an obstacle cell's temperature (0 adiabatic) */
    double *hu, *hv, *hw;                    /* a held face's speed (a fuel vent's before its ramp) */
    int16_t *eu, *ev, *ew;                   /* a vent face's vent, or -1 */
    uint8_t *solid, *ku, *kv, *kw, *nearo;   /* solid cells; the faces' kinds; cells within two of an open face */
    Vent vent[MAXV];
    Opening open[MAXV];
    Fan fan[MAXV];
    int nvent, nopen, nfan;
    bool ready, sealed, noslip[6];
    long nfluid;
    Mg3D *mg;
    double t, p0, gamma, rho_inf, div_err, hrr_now, mass_in, mass_out, stage_in, stage_out, stage_cap, mass_capped;
    double dp0, heat[8], stage_heat[8], bflow[5], stage_flow[5];
};

static inline size_t C(const Fire *F, int i, int j, int k) { return (size_t)i + (size_t)F->nx * ((size_t)j + (size_t)F->ny * k); }
static inline size_t IU(const Fire *F, int i, int j, int k) { return (size_t)i + (size_t)(F->nx + 1) * ((size_t)j + (size_t)F->ny * k); }
static inline size_t IV(const Fire *F, int i, int j, int k) { return (size_t)i + (size_t)F->nx * ((size_t)j + (size_t)(F->ny + 1) * k); }
static inline size_t IW(const Fire *F, int i, int j, int k) { return (size_t)i + (size_t)F->nx * ((size_t)j + (size_t)F->ny * k); }
static inline int clampi(int a, int lo, int hi) { return a < lo ? lo : a > hi ? hi : a; }
/* values beyond the box: the nearest inside (zero gradient at the open faces, a mirror at the free-slip floor) */
static inline double gu(const Fire *F, const double *u, int i, int j, int k) { return u[IU(F, clampi(i, 0, F->nx), clampi(j, 0, F->ny - 1), clampi(k, 0, F->nz - 1))]; }
static inline double gv(const Fire *F, const double *v, int i, int j, int k) { return v[IV(F, clampi(i, 0, F->nx - 1), clampi(j, 0, F->ny), clampi(k, 0, F->nz - 1))]; }
static inline double gw(const Fire *F, const double *w, int i, int j, int k) { return w[IW(F, clampi(i, 0, F->nx - 1), clampi(j, 0, F->ny - 1), clampi(k, 0, F->nz))]; }
static inline double gc(const Fire *F, const double *a, int i, int j, int k) { return a[C(F, clampi(i, 0, F->nx - 1), clampi(j, 0, F->ny - 1), clampi(k, 0, F->nz - 1))]; }
/* a velocity's neighbour across its own direction (a tangential neighbour), for the face whose value is uc: beyond a
 * no-slip side or on a face closed by a solid, the ghost -uc (the wall half-way); beyond any other side the nearest
 * inside (zero gradient: free slip, or an open side) */
static inline double tu(const Fire *F, const double *u, int i, int j, int k, double uc) {
    if (j < 0 || j >= F->ny || k < 0 || k >= F->nz) return F->noslip[j < 0 ? 2 : j >= F->ny ? 3 : k < 0 ? 4 : 5] ? -uc : gu(F, u, i, j, k);
    size_t f = IU(F, clampi(i, 0, F->nx), j, k);
    return F->ku[f] == K_SOLID ? -uc : u[f];
}
static inline double tv(const Fire *F, const double *v, int i, int j, int k, double vc) {
    if (i < 0 || i >= F->nx || k < 0 || k >= F->nz) return F->noslip[i < 0 ? 0 : i >= F->nx ? 1 : k < 0 ? 4 : 5] ? -vc : gv(F, v, i, j, k);
    size_t f = IV(F, i, clampi(j, 0, F->ny), k);
    return F->kv[f] == K_SOLID ? -vc : v[f];
}
static inline double tw(const Fire *F, const double *w, int i, int j, int k, double wc) {
    if (i < 0 || i >= F->nx || j < 0 || j >= F->ny) return F->noslip[i < 0 ? 0 : i >= F->nx ? 1 : j < 0 ? 2 : 3] ? -wc : gw(F, w, i, j, k);
    size_t f = IW(F, i, j, clampi(k, 0, F->nz));
    return F->kw[f] == K_SOLID ? -wc : w[f];
}
/* a cell's value for a limiter's stencil: beyond the box or inside a solid, the fallback (no slope) */
static inline double gcs(const Fire *F, const double *a, int i, int j, int k, double fb) {
    if (i < 0 || j < 0 || k < 0 || i >= F->nx || j >= F->ny || k >= F->nz) return fb;
    size_t c = C(F, i, j, k);
    return F->solid[c] ? fb : a[c];
}

void fire_spec_defaults(FireSpec *s) {
    memset(s, 0, sizeof *s);
    s->T0 = 293.15, s->p0 = 101325, s->g = 9.80665;
    s->W_fuel = 16.043e-3, s->dHc = 50.0e6, s->s_air = 17.2, s->chi_r = 0.2, s->W_prod = 27.6e-3;
    s->cp = 0, s->nu = 1.5e-5, s->cs = 0.2, s->cfl = 0.4, s->ramp = 1.0;
    s->side[4] = FIRE_SLIP, s->sponge = 4, s->sgs = FIRE_SMAGORINSKY, s->Pr = 0.7;
}

void fire_free(Fire *F) {
    if (!F) return;
    double **arr[] = {&F->rho, &F->YF, &F->YP, &F->u, &F->v, &F->w, &F->rho0, &F->YF0, &F->YP0, &F->u0, &F->v0, &F->w0, &F->rho1, &F->YF1, &F->YP1,
                      &F->u1, &F->v1, &F->w1, &F->T, &F->Wm, &F->nut, &F->Sm, &F->D, &F->q, &F->H, &F->rhs, &F->Hs, &F->bx, &F->by, &F->bz,
                      &F->qs, &F->Ts, &F->hu, &F->hv, &F->hw, &F->AG, &F->AG0, &F->AG1};
    for (size_t a = 0; a < sizeof arr / sizeof arr[0]; a++) free(*arr[a]);
    free(F->eu), free(F->ev), free(F->ew), free(F->solid), free(F->ku), free(F->kv), free(F->kw), free(F->nearo);
    mg3d_free(F->mg);
    free(F);
}

Fire *fire_create(const FireSpec *s, char *err, size_t errlen) {
    if (s->n[0] < 4 || s->n[1] < 4 || s->n[2] < 4 || !(s->dx > 0) || !(s->hrr >= 0) || !(s->dHc > 0) || !(s->W_fuel > 0) || s->cp < 0) {
        snprintf(err, errlen, "fire: cells, a cell size, a fuel and a heat release rate (0 for none) are required");
        return NULL;
    }
    for (int f = 0; f < 6; f++)
        if (s->side[f] < FIRE_OPEN || s->side[f] > FIRE_SLIP || s->side_T[f] < 0) {
            snprintf(err, errlen, "fire: each side is open, a wall or a free-slip wall, at a temperature >= 0 K (0 adiabatic)");
            return NULL;
        }
    Fire *F = calloc(1, sizeof *F);
    if (!F) return NULL;
    F->s = *s;
    if (!(F->s.cfl > 0)) F->s.cfl = 0.4;
    if (!(F->s.cs > 0)) F->s.cs = 0.2;
    if (!(F->s.Pr > 0)) F->s.Pr = 0.7;
    F->nx = s->n[0], F->ny = s->n[1], F->nz = s->n[2];
    F->nc = (size_t)F->nx * F->ny * F->nz, F->nu = (size_t)(F->nx + 1) * F->ny * F->nz, F->nv = (size_t)F->nx * (F->ny + 1) * F->nz;
    F->nw = (size_t)F->nx * F->ny * (F->nz + 1);
    double **cells[] = {&F->rho, &F->YF, &F->YP, &F->rho0, &F->YF0, &F->YP0, &F->rho1, &F->YF1, &F->YP1, &F->T, &F->Wm, &F->nut, &F->Sm, &F->D, &F->q,
                        &F->H, &F->rhs, &F->Hs, &F->qs, &F->Ts, &F->AG, &F->AG0, &F->AG1};
    for (size_t a = 0; a < sizeof cells / sizeof cells[0]; a++) *cells[a] = calloc(F->nc, sizeof(double));
    F->u = calloc(F->nu, 8), F->u0 = calloc(F->nu, 8), F->u1 = calloc(F->nu, 8), F->bx = calloc(F->nu, 8), F->hu = calloc(F->nu, 8);
    F->v = calloc(F->nv, 8), F->v0 = calloc(F->nv, 8), F->v1 = calloc(F->nv, 8), F->by = calloc(F->nv, 8), F->hv = calloc(F->nv, 8);
    F->w = calloc(F->nw, 8), F->w0 = calloc(F->nw, 8), F->w1 = calloc(F->nw, 8), F->bz = calloc(F->nw, 8), F->hw = calloc(F->nw, 8);
    F->eu = malloc(F->nu * 2), F->ev = malloc(F->nv * 2), F->ew = malloc(F->nw * 2);
    F->ku = calloc(F->nu, 1), F->kv = calloc(F->nv, 1), F->kw = calloc(F->nw, 1), F->solid = calloc(F->nc, 1), F->nearo = calloc(F->nc, 1);
    bool ok = F->u && F->u0 && F->u1 && F->bx && F->hu && F->v && F->v0 && F->v1 && F->by && F->hv && F->w && F->w0 && F->w1 && F->bz && F->hw &&
              F->eu && F->ev && F->ew && F->ku && F->kv && F->kw && F->solid && F->nearo;
    for (size_t a = 0; a < sizeof cells / sizeof cells[0]; a++) ok = ok && *cells[a];
    if (!ok) {
        snprintf(err, errlen, "fire: out of memory");
        fire_free(F);
        return NULL;
    }
    memset(F->eu, 0xff, F->nu * 2), memset(F->ev, 0xff, F->nv * 2), memset(F->ew, 0xff, F->nw * 2);
    F->p0 = s->p0;
    double cpa = tab(CP_AIR, s->T0);
    F->gamma = cpa / (cpa - RU / W_AIR);
    F->rho_inf = s->p0 * W_AIR / (RU * s->T0);
    for (size_t c = 0; c < F->nc; c++) F->rho[c] = F->rho_inf, F->T[c] = s->T0, F->Wm[c] = W_AIR;
    /* the burner: a fuel vent on the floor, its speed set by the heat release rate once its area is known */
    if (s->hrr > 0) {
        Vent *b = &F->vent[F->nvent++];
        b->side = 4, memcpy(b->rect, s->burner, sizeof b->rect), b->T = s->T0, b->fuel = 1, b->hrr = s->hrr;
    }
    return F;
}

double fire_time(const Fire *F) { return F->t; }
size_t fire_cells(const Fire *F) { return F->nc; }

/* ---- the room --------------------------------------------------------------------------------------------------------- */

static bool box_ok(const double b[6]) { return b[3] > b[0] && b[4] > b[1] && b[5] > b[2]; }
static bool rect_ok(int side, const double r[4]) { return side >= 0 && side < 6 && r[2] > r[0] && r[3] > r[1]; }
/* the cells whose centres lie in a box */
static void box_cells(const Fire *F, const double b[6], int lo[3], int hi[3]) {
    const int n[3] = {F->nx, F->ny, F->nz};
    for (int a = 0; a < 3; a++) {
        lo[a] = clampi((int)ceil((b[a] - F->s.origin[a]) / F->s.dx - 0.5), 0, n[a]);
        hi[a] = clampi((int)floor((b[a + 3] - F->s.origin[a]) / F->s.dx - 0.5) + 1, 0, n[a]); /* exclusive */
    }
}

bool fire_add_obstacle(Fire *F, const double box[6], double T) {
    if (F->ready || !box_ok(box) || T < 0) return false;
    int lo[3], hi[3];
    box_cells(F, box, lo, hi);
    for (int k = lo[2]; k < hi[2]; k++)
        for (int j = lo[1]; j < hi[1]; j++)
            for (int i = lo[0]; i < hi[0]; i++) F->solid[C(F, i, j, k)] = 1, F->Ts[C(F, i, j, k)] = T;
    return true;
}
bool fire_add_vent(Fire *F, int side, const double rect[4], double speed, double T, double fuel) {
    if (F->ready || F->nvent >= MAXV || !rect_ok(side, rect) || !(T > 0) || fuel < 0 || fuel > 1) return false;
    Vent *v = &F->vent[F->nvent++];
    v->side = side, memcpy(v->rect, rect, sizeof v->rect), v->speed = speed, v->T = T, v->fuel = fuel, v->hrr = 0;
    return true;
}
bool fire_add_opening(Fire *F, int side, const double rect[4]) {
    if (F->ready || F->nopen >= MAXV || !rect_ok(side, rect)) return false;
    F->open[F->nopen].side = side, memcpy(F->open[F->nopen].rect, rect, sizeof F->open[0].rect), F->nopen++;
    return true;
}
bool fire_add_heat(Fire *F, const double b[6], double watts) {
    if (F->ready || !box_ok(b)) return false;
    /* each cell's share of the box's volume */
    const double dx = F->s.dx, vol = (b[3] - b[0]) * (b[4] - b[1]) * (b[5] - b[2]);
    for (int k = 0; k < F->nz; k++)
        for (int j = 0; j < F->ny; j++)
            for (int i = 0; i < F->nx; i++) {
                double o = 1;
                const int id[3] = {i, j, k};
                for (int a = 0; a < 3; a++) {
                    double x0 = F->s.origin[a] + id[a] * dx;
                    o *= fmax(0, fmin(x0 + dx, b[a + 3]) - fmax(x0, b[a]));
                }
                if (o > 0) F->qs[C(F, i, j, k)] += watts * o / vol / (dx * dx * dx);
            }
    return true;
}
bool fire_add_fan(Fire *F, const double box[6], int axis, double speed) {
    if (F->ready || F->nfan >= MAXV || !box_ok(box) || axis < 0 || axis > 2) return false;
    Fan *f = &F->fan[F->nfan++];
    memcpy(f->box, box, sizeof f->box), f->axis = axis, f->speed = speed;
    return true;
}

/* a side's faces: the tangential cell counts, and the face at tangential cells (a, b) with its component */
static void side_dims(const Fire *F, int s, int *na, int *nb) {
    int ax = s / 2;
    *na = ax == 0 ? F->ny : F->nx, *nb = ax == 2 ? F->ny : F->nz;
}
static size_t side_face(const Fire *F, int s, int a, int b, int *comp) {
    int ax = s / 2, hi = s & 1;
    *comp = ax;
    if (ax == 0) return IU(F, hi ? F->nx : 0, a, b);
    if (ax == 1) return IV(F, a, hi ? F->ny : 0, b);
    return IW(F, a, b, hi ? F->nz : 0);
}
static size_t side_cell(const Fire *F, int s, int a, int b) {
    int ax = s / 2, hi = s & 1;
    if (ax == 0) return C(F, hi ? F->nx - 1 : 0, a, b);
    if (ax == 1) return C(F, a, hi ? F->ny - 1 : 0, b);
    return C(F, a, b, hi ? F->nz - 1 : 0);
}
/* the share of a side's face (a, b) a rectangle covers */
static double cover(const Fire *F, int s, int a, int b, const double r[4]) {
    int ax = s / 2, t0 = ax == 0 ? 1 : 0, t1 = ax == 2 ? 1 : 2;
    const double dx = F->s.dx, x0 = F->s.origin[t0] + a * dx, y0 = F->s.origin[t1] + b * dx;
    double o0 = fmax(0, fmin(x0 + dx, r[2]) - fmax(x0, r[0])), o1 = fmax(0, fmin(y0 + dx, r[3]) - fmax(y0, r[1]));
    return o0 * o1 / (dx * dx);
}
static double vent_density(const Fire *F, const Vent *v) {
    double W = 1 / (v->fuel / F->s.W_fuel + (1 - v->fuel) / W_AIR);
    return F->p0 * W / (RU * v->T);
}

/* the faces' kinds and held speeds, the Poisson solver's sides, once the room is complete */
static bool prepare(Fire *F, char *err, size_t errlen) {
    uint8_t *K[3] = {F->ku, F->kv, F->kw};
    double *Hh[3] = {F->hu, F->hv, F->hw}, *V[3] = {F->u, F->v, F->w};
    int16_t *E[3] = {F->eu, F->ev, F->ew};
    int comp;
    for (int s = 0; s < 6; s++) {
        int na, nb;
        side_dims(F, s, &na, &nb);
        for (int b = 0; b < nb; b++)
            for (int a = 0; a < na; a++) {
                size_t f = side_face(F, s, a, b, &comp);
                K[comp][f] = F->s.side[s] == FIRE_OPEN ? K_OPEN : K_HELD, Hh[comp][f] = 0;
            }
    }
    for (int o = 0; o < F->nopen; o++) {
        int s = F->open[o].side, na, nb;
        if (F->s.side[s] == FIRE_OPEN) continue;
        side_dims(F, s, &na, &nb);
        for (int b = 0; b < nb; b++)
            for (int a = 0; a < na; a++)
                if (cover(F, s, a, b, F->open[o].rect) >= 0.5) K[s / 2][side_face(F, s, a, b, &comp)] = K_OPEN;
    }
    for (int e = 0; e < F->nvent; e++) {
        Vent *v = &F->vent[e];
        int s = v->side, na, nb;
        side_dims(F, s, &na, &nb);
        double area = 0;
        for (int b = 0; b < nb; b++)
            for (int a = 0; a < na; a++) area += cover(F, s, a, b, v->rect) * F->s.dx * F->s.dx;
        if (v->hrr > 0) v->speed = area > 0 ? v->hrr / (F->s.dHc * area * vent_density(F, v) * v->fuel) : 0;
        for (int b = 0; b < nb; b++)
            for (int a = 0; a < na; a++) {
                double cv = cover(F, s, a, b, v->rect);
                if (cv <= 0) continue;
                size_t f = side_face(F, s, a, b, &comp);
                K[comp][f] = K_HELD, Hh[comp][f] = ((s & 1) ? -1 : 1) * cv * v->speed, E[comp][f] = (int16_t)e;
            }
    }
    const double dx = F->s.dx;
    for (int e = 0; e < F->nfan; e++) {
        const Fan *fn = &F->fan[e];
        const int ax = fn->axis, n[3] = {F->nx, F->ny, F->nz};
        for (int k = 0; k < n[2] + (ax == 2); k++)
            for (int j = 0; j < n[1] + (ax == 1); j++)
                for (int i = 0; i < n[0] + (ax == 0); i++) {
                    const int id[3] = {i, j, k};
                    bool in = id[ax] > 0 && id[ax] < n[ax];
                    for (int a = 0; a < 3 && in; a++) {
                        double x = F->s.origin[a] + (a == ax ? id[a] : id[a] + 0.5) * dx;
                        in = x >= fn->box[a] - 1e-9 * dx && x <= fn->box[a + 3] + 1e-9 * dx;
                    }
                    if (!in) continue;
                    size_t f = ax == 0 ? IU(F, i, j, k) : ax == 1 ? IV(F, i, j, k) : IW(F, i, j, k);
                    K[ax][f] = K_FAN, Hh[ax][f] = fn->speed;
                }
    }
    /* a solid cell closes its six faces */
    F->nfluid = 0;
    for (int k = 0; k < F->nz; k++)
        for (int j = 0; j < F->ny; j++)
            for (int i = 0; i < F->nx; i++) {
                size_t c = C(F, i, j, k);
                if (!F->solid[c]) {
                    F->nfluid++;
                    continue;
                }
                size_t fs[6] = {IU(F, i, j, k), IU(F, i + 1, j, k), IV(F, i, j, k), IV(F, i, j + 1, k), IW(F, i, j, k), IW(F, i, j, k + 1)};
                for (int q = 0; q < 6; q++) K[q / 2][fs[q]] = K_SOLID, Hh[q / 2][fs[q]] = 0, E[q / 2][fs[q]] = -1;
            }
    if (!F->nfluid) {
        snprintf(err, errlen, "fire: the obstacles fill the box");
        return false;
    }
    /* the Poisson solver: a side with an open face holds the pressure perturbation (closed faces then carry no
     * coefficient), a side with none is a Neumann side; with no open face anywhere the box is sealed */
    int bc[6];
    F->sealed = true;
    for (int s = 0; s < 6; s++) {
        int na, nb;
        bool any = false;
        side_dims(F, s, &na, &nb);
        for (int b = 0; b < nb && !any; b++)
            for (int a = 0; a < na && !any; a++) any = K[s / 2][side_face(F, s, a, b, &comp)] == K_OPEN;
        bc[s] = any ? MG_DIRICHLET : MG_NEUMANN;
        if (any) F->sealed = false;
        F->noslip[s] = F->s.side[s] == FIRE_WALL;
        /* cells within two of an open face, where momentum is advected upwind */
        if (!any) continue;
        for (int b = 0; b < nb; b++)
            for (int a = 0; a < na; a++) {
                if (K[s / 2][side_face(F, s, a, b, &comp)] != K_OPEN) continue;
                size_t c0 = side_cell(F, s, a, b);
                int ci = (int)(c0 % F->nx), cj = (int)(c0 / F->nx % F->ny), ck = (int)(c0 / ((size_t)F->nx * F->ny));
                int ax = s / 2, dir = (s & 1) ? -1 : 1;
                for (int d = 0; d < 2; d++)
                    for (int p = -1; p <= 1; p++)
                        for (int r = -1; r <= 1; r++) {
                            int q[3] = {ci, cj, ck};
                            q[ax] += dir * d, q[ax == 0 ? 1 : 0] += p, q[ax == 2 ? 1 : 2] += r;
                            if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= F->nx || q[1] >= F->ny || q[2] >= F->nz) continue;
                            F->nearo[C(F, q[0], q[1], q[2])] = 1;
                        }
            }
    }
    F->mg = mg3d_create(F->nx, F->ny, F->nz, dx, bc, err, errlen);
    if (!F->mg) return false;
    size_t nf[3] = {F->nu, F->nv, F->nw};
    for (int a = 0; a < 3; a++)
        for (size_t f = 0; f < nf[a]; f++)
            if (K[a][f] == K_HELD || K[a][f] == K_SOLID || K[a][f] == K_FAN) V[a][f] = Hh[a][f];
    F->heat[7] = 0;
    for (size_t c = 0; c < F->nc; c++) F->heat[7] += F->qs[c] * dx * dx * dx;
    F->ready = true;
    return true;
}

/* ---- the pieces of a stage ------------------------------------------------------------------------------------------- */

typedef struct {
    Fire *F;
    const double *rho, *YF, *YP, *u, *v, *w;
    double *rho_o, *YF_o, *YP_o, *u_o, *v_o, *w_o;
    double dt;
    double in[64], out[64], capped[64], flow[64][5], heat[64][7];
} Stage;

/* temperature, molar mass, the strain rate and the subgrid viscosity */
static void props(void *vs, int k0, int k1, int tid) {
    Stage *S = vs;
    Fire *F = S->F;
    const double dx = F->s.dx, cs2 = F->s.cs * F->s.cs * dx * dx, p0 = F->p0;
    const double *u = S->u, *v = S->v, *w = S->w;
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < F->ny; j++)
            for (int i = 0; i < F->nx; i++) {
                size_t c = C(F, i, j, k);
                if (F->solid[c]) {
                    F->nut[c] = F->Sm[c] = 0, F->T[c] = F->s.T0, F->Wm[c] = W_AIR;
                    continue;
                }
                double yf = S->YF[c], yp = S->YP[c], ya = fmax(0, 1 - yf - yp);
                double W = 1 / (yf / F->s.W_fuel + yp / F->s.W_prod + ya / W_AIR);
                F->Wm[c] = W, F->T[c] = p0 * W / (S->rho[c] * RU);
                /* the velocity gradients at the centre (the wall's ghost where a neighbour is beyond one) */
                double u0 = u[IU(F, i, j, k)], u1 = u[IU(F, i + 1, j, k)], v0 = v[IV(F, i, j, k)], v1 = v[IV(F, i, j + 1, k)], w0 = w[IW(F, i, j, k)],
                       w1 = w[IW(F, i, j, k + 1)];
                double dudx = (u1 - u0) / dx, dvdy = (v1 - v0) / dx, dwdz = (w1 - w0) / dx;
                double dudy = 0.25 * (tu(F, u, i, j + 1, k, u0) + tu(F, u, i + 1, j + 1, k, u1) - tu(F, u, i, j - 1, k, u0) - tu(F, u, i + 1, j - 1, k, u1)) / dx;
                double dudz = 0.25 * (tu(F, u, i, j, k + 1, u0) + tu(F, u, i + 1, j, k + 1, u1) - tu(F, u, i, j, k - 1, u0) - tu(F, u, i + 1, j, k - 1, u1)) / dx;
                double dvdx = 0.25 * (tv(F, v, i + 1, j, k, v0) + tv(F, v, i + 1, j + 1, k, v1) - tv(F, v, i - 1, j, k, v0) - tv(F, v, i - 1, j + 1, k, v1)) / dx;
                double dvdz = 0.25 * (tv(F, v, i, j, k + 1, v0) + tv(F, v, i, j + 1, k + 1, v1) - tv(F, v, i, j, k - 1, v0) - tv(F, v, i, j + 1, k - 1, v1)) / dx;
                double dwdx = 0.25 * (tw(F, w, i + 1, j, k, w0) + tw(F, w, i + 1, j, k + 1, w1) - tw(F, w, i - 1, j, k, w0) - tw(F, w, i - 1, j, k + 1, w1)) / dx;
                double dwdy = 0.25 * (tw(F, w, i, j + 1, k, w0) + tw(F, w, i, j + 1, k + 1, w1) - tw(F, w, i, j - 1, k, w0) - tw(F, w, i, j - 1, k + 1, w1)) / dx;
                double sxy = 0.5 * (dudy + dvdx), sxz = 0.5 * (dudz + dwdx), syz = 0.5 * (dvdz + dwdy);
                double S2 = 2 * (dudx * dudx + dvdy * dvdy + dwdz * dwdz + 2 * (sxy * sxy + sxz * sxz + syz * syz));
                F->Sm[c] = sqrt(S2);
                if (F->s.sgs == FIRE_SMAGORINSKY) F->nut[c] = cs2 * F->Sm[c];
                else if (F->s.sgs == FIRE_VREMAN) { /* Vreman (2004): vanishes in pure shear and at walls, c = 0.07 */
                    const double a[3][3] = {{dudx, dvdx, dwdx}, {dudy, dvdy, dwdy}, {dudz, dvdz, dwdz}}; /* a[i][j] = du_j / dx_i */
                    double b[3][3], aa = 0;
                    for (int p = 0; p < 3; p++)
                        for (int q = 0; q < 3; q++) {
                            b[p][q] = dx * dx * (a[0][p] * a[0][q] + a[1][p] * a[1][q] + a[2][p] * a[2][q]);
                            aa += a[p][q] * a[p][q];
                        }
                    double B = b[0][0] * b[1][1] - b[0][1] * b[0][1] + b[0][0] * b[2][2] - b[0][2] * b[0][2] + b[1][1] * b[2][2] - b[1][2] * b[1][2];
                    F->nut[c] = aa > 1e-30 && B > 0 ? 0.07 * sqrt(B / aa) : 0;
                } else
                    F->nut[c] = 0;
            }
}

/* a face value by the upwind cell and a minmod-limited slope: the cells along the face's normal are a (behind) and b
 * (ahead), with am behind a and bp ahead of b */
static inline double face_val(double am, double a, double b, double bp, double vel) {
    double x, s1, s2;
    if (vel >= 0) x = a, s1 = a - am, s2 = b - a;
    else x = b, s1 = b - a, s2 = bp - b;
    double lim = s1 * s2 <= 0 ? 0 : (fabs(s1) < fabs(s2) ? s1 : s2);
    return x + (vel >= 0 ? 0.5 : -0.5) * lim;
}

/* continuity and the species: the fluxes through the six faces of every cell */
static void scalars(void *vs, int k0, int k1, int tid) {
    Stage *S = vs;
    Fire *F = S->F;
    const double dx = F->s.dx, dt = S->dt, ia = dt / dx, a2 = dx * dx * dt;
    const uint8_t *K[3] = {F->ku, F->kv, F->kw};
    const int16_t *E[3] = {F->eu, F->ev, F->ew};
    double in = 0, out = 0, inT = 0, outT = 0, outA = 0;
    const double *ag = F->agi;
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < F->ny; j++)
            for (int i = 0; i < F->nx; i++) {
                size_t c = C(F, i, j, k);
                if (F->solid[c]) {
                    S->rho_o[c] = S->rho[c], S->YF_o[c] = S->YF[c], S->YP_o[c] = S->YP[c], F->ago[c] = F->agi[c];
                    continue;
                }
                double dm = 0, dF = 0, dP = 0, dA = 0; /* the net inflow of mass, fuel, products and age per unit area */
                for (int dir = 0; dir < 3; dir++)
                    for (int side = 0; side < 2; side++) {
                        /* the face and the cell beyond it */
                        int di = dir == 0 ? (side ? 1 : -1) : 0, dj = dir == 1 ? (side ? 1 : -1) : 0, dk = dir == 2 ? (side ? 1 : -1) : 0;
                        int ni = i + di, nj = j + dj, nk = k + dk;
                        size_t fi = dir == 0 ? IU(F, i + side, j, k) : dir == 1 ? IV(F, i, j + side, k) : IW(F, i, j, k + side);
                        double vel = dir == 0 ? S->u[fi] : dir == 1 ? S->v[fi] : S->w[fi];
                        double sgn = side ? -1 : 1; /* the face's velocity counted into the cell */
                        bool outside = ni < 0 || nj < 0 || nk < 0 || ni >= F->nx || nj >= F->ny || nk >= F->nz;
                        double fr, ff, fp, fa = 0;
                        if (outside) {
                            int kind = K[dir][fi];
                            if (kind == K_SOLID || vel == 0) continue;
                            double Tin = F->s.T0;
                            if (sgn * vel > 0) {
                                if (kind == K_OPEN) fr = F->rho_inf, ff = 0, fp = 0; /* air coming in */
                                else {
                                    int e = E[dir][fi];
                                    if (e < 0) continue;
                                    const Vent *vt = &F->vent[e];
                                    fr = vent_density(F, vt), ff = vt->fuel, fp = 0, Tin = vt->T;
                                }
                            } else
                                fr = S->rho[c], ff = S->YF[c], fp = S->YP[c], Tin = F->T[c], fa = ag[c];
                            double m = sgn * vel * fr;
                            dm += m, dF += m * ff, dP += m * fp, dA += m * fa;
                            if (m > 0) in += m * a2, inT += m * a2 * Tin;
                            else out -= m * a2, outT -= m * a2 * Tin, outA -= m * a2 * fa;
                            continue;
                        }
                        size_t n2 = C(F, ni, nj, nk);
                        if (F->solid[n2]) continue;
                        /* along the normal: the cell behind the face (a) and ahead (b), from low index to high */
                        int ai = side ? i : ni, aj = side ? j : nj, ak = side ? k : nk, bi = side ? ni : i, bj = side ? nj : j, bk = side ? nk : k;
                        size_t ca = C(F, ai, aj, ak), cb = C(F, bi, bj, bk);
                        int mi = ai - (dir == 0), mj = aj - (dir == 1), mk = ak - (dir == 2), pi = bi + (dir == 0), pj = bj + (dir == 1), pk = bk + (dir == 2);
                        fr = face_val(gcs(F, S->rho, mi, mj, mk, S->rho[ca]), S->rho[ca], S->rho[cb], gcs(F, S->rho, pi, pj, pk, S->rho[cb]), vel);
                        ff = face_val(gcs(F, S->YF, mi, mj, mk, S->YF[ca]), S->YF[ca], S->YF[cb], gcs(F, S->YF, pi, pj, pk, S->YF[cb]), vel);
                        fp = face_val(gcs(F, S->YP, mi, mj, mk, S->YP[ca]), S->YP[ca], S->YP[cb], gcs(F, S->YP, pi, pj, pk, S->YP[cb]), vel);
                        /* turbulent diffusion of the species across the face */
                        double Dt = 0.5 * (F->nut[c] + F->nut[n2]) / PRT + F->s.nu, rf = 0.5 * (S->rho[c] + S->rho[n2]);
                        dF += rf * Dt * (S->YF[n2] - S->YF[c]) / dx, dP += rf * Dt * (S->YP[n2] - S->YP[c]) / dx;
                        fa = face_val(gcs(F, ag, mi, mj, mk, ag[ca]), ag[ca], ag[cb], gcs(F, ag, pi, pj, pk, ag[cb]), vel);
                        dA += rf * Dt * (ag[n2] - ag[c]) / dx;
                        double m = sgn * vel * fr; /* mass per area per time into the cell */
                        dm += m, dF += m * ff, dP += m * fp, dA += m * fa;
                    }
                double r0 = S->rho[c], rn = r0 + ia * dm;
                double rmin = F->p0 * W_AIR / (RU * 3000.0); /* the gas no hotter than 3000 K */
                if (rn < rmin) S->capped[tid] += (rmin - rn) * dx * dx * dx, rn = rmin;
                S->rho_o[c] = rn;
                S->YF_o[c] = fmin(1, fmax(0, (r0 * S->YF[c] + ia * dF) / rn));
                S->YP_o[c] = fmin(1 - S->YF_o[c], fmax(0, (r0 * S->YP[c] + ia * dP) / rn));
                F->ago[c] = fmax(0, (r0 * ag[c] + ia * dA + dt * r0) / rn); /* ageing: rho times 1 s per s */
            }
    S->in[tid] += in, S->out[tid] += out, S->flow[tid][0] += in, S->flow[tid][1] += inT, S->flow[tid][2] += out, S->flow[tid][3] += outT;
    S->flow[tid][4] += outA;
}

/* the divergence the heat release, the heat sources and conduction ask of the flow */
static void divergence(void *vs, int k0, int k1, int tid) {
    Stage *S = vs;
    Fire *F = S->F;
    const double dx = F->s.dx, nuk = F->s.nu / F->s.Pr;
    const uint8_t *K[3] = {F->ku, F->kv, F->kw};
    const int16_t *E[3] = {F->eu, F->ev, F->ew};
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < F->ny; j++)
            for (int i = 0; i < F->nx; i++) {
                size_t c = C(F, i, j, k);
                if (F->solid[c]) {
                    F->D[c] = 0;
                    continue;
                }
                double Tc = F->T[c], cond = 0, cp = F->s.cp > 0 ? F->s.cp : cp_mix(Tc, S->YF[c], S->YP[c]);
                double kc = cp * S->rho[c] * (F->nut[c] / PRT + nuk); /* the cell's conductivity */
                const int nb[6][3] = {{i - 1, j, k}, {i + 1, j, k}, {i, j - 1, k}, {i, j + 1, k}, {i, j, k - 1}, {i, j, k + 1}};
                for (int d = 0; d < 6; d++) {
                    int a = nb[d][0], b = nb[d][1], e = nb[d][2];
                    if (a < 0 || b < 0 || e < 0 || a >= F->nx || b >= F->ny || e >= F->nz) {
                        /* a wall at a temperature (not its vents or openings); open faces carry ambient heat by the flow */
                        size_t fi = d < 2 ? IU(F, i + (d & 1), j, k) : d < 4 ? IV(F, i, j + (d & 1), k) : IW(F, i, j, k + (d & 1));
                        if (F->s.side_T[d] > 0 && K[d / 2][fi] == K_HELD && E[d / 2][fi] < 0) {
                            double fl = kc * (F->s.side_T[d] - Tc) * 2 / dx; /* W/m^2, the wall half a cell away */
                            cond += fl / dx, S->heat[tid][d] += fl * dx * dx;
                        }
                        continue;
                    }
                    size_t n2 = C(F, a, b, e);
                    if (F->solid[n2]) {
                        if (F->Ts[n2] > 0) {
                            double fl = kc * (F->Ts[n2] - Tc) * 2 / dx;
                            cond += fl / dx, S->heat[tid][6] += fl * dx * dx;
                        }
                        continue;
                    }
                    double kf = cp * 0.5 * (S->rho[c] * (F->nut[c] / PRT + nuk) + S->rho[n2] * (F->nut[n2] / PRT + nuk));
                    cond += kf * (F->T[n2] - Tc) / (dx * dx);
                }
                /* above 2500 K (past methane's adiabatic flame temperature: numerical overshoot) the expansion fades out by
                 * 3000 K, so that the gas does not heat on without mass being added to hold it (as a density floor did) */
                double heat = (1 - F->s.chi_r) * F->q[c] + F->qs[c] + cond;
                if (Tc > 2500 && heat > 0) heat *= fmax(0, (3000 - Tc) / 500);
                F->D[c] = heat / (S->rho[c] * cp * Tc);
            }
}

/* a velocity's derivative along an axis, advected at speed a: central inside, upwind within two cells of an open face
 * (central differences reflect and grow where the flow leaves an open boundary) */
static inline double ddir(double m, double c, double p, double a, bool near_open, double i2) {
    if (!near_open) return (p - m) * i2;
    return a >= 0 ? (c - m) * 2 * i2 : (p - c) * 2 * i2;
}

/* the sponge's factor at a column of cells (i, j): a quarter of the way to rest each stage at the outermost cells next
 * to an open side x-, x+, y- or y+, nothing four cells in */
static inline double sponge(const Fire *F, int i, int j) {
    const int ns = F->s.sponge;
    int d = ns;
    if (F->s.side[0] == FIRE_OPEN && i < d) d = i;
    if (F->s.side[1] == FIRE_OPEN && F->nx - 1 - i < d) d = F->nx - 1 - i;
    if (F->s.side[2] == FIRE_OPEN && j < d) d = j;
    if (F->s.side[3] == FIRE_OPEN && F->ny - 1 - j < d) d = F->ny - 1 - j;
    return d >= ns ? 1 : 1 - 0.25 * (double)(ns - d) / ns;
}

/* the momentum's rates at the faces, then the stage's velocity before the projection: advection (central), buoyancy,
 * the viscous term; the pressure comes from the projection. Held faces keep their speed. */
static void momentum(void *vs, int k0, int k1, int tid) {
    Stage *S = vs;
    Fire *F = S->F;
    const double dx = F->s.dx, dt = S->dt, g = F->s.g, i2 = 1 / (2 * dx), ih2 = 1 / (dx * dx);
    const double *u = S->u, *v = S->v, *w = S->w;
    const int nx = F->nx, ny = F->ny, nz = F->nz;
#define NEAR(a, b, c) F->nearo[C(F, clampi(a, 0, nx - 1), clampi(b, 0, ny - 1), clampi(c, 0, nz - 1))]
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < ny; j++) {
            /* u faces */
            for (int i = 0; i <= nx; i++) {
                size_t f = IU(F, i, j, k);
                if (F->ku[f] != K_FLUID) {
                    S->u_o[f] = F->ku[f] == K_FAN ? F->hu[f] : u[f];
                    continue;
                }
                double uc = u[f];
                double vv = 0.25 * (gv(F, v, i - 1, j, k) + gv(F, v, i, j, k) + gv(F, v, i - 1, j + 1, k) + gv(F, v, i, j + 1, k));
                double ww = 0.25 * (gw(F, w, i - 1, j, k) + gw(F, w, i, j, k) + gw(F, w, i - 1, j, k + 1) + gw(F, w, i, j, k + 1));
                bool nb = NEAR(i - 1, j, k) || NEAR(i, j, k);
                double ym = tu(F, u, i, j - 1, k, uc), yp = tu(F, u, i, j + 1, k, uc), zm = tu(F, u, i, j, k - 1, uc), zp = tu(F, u, i, j, k + 1, uc);
                double xm = gu(F, u, i - 1, j, k), xp = gu(F, u, i + 1, j, k);
                double adv = uc * ddir(xm, uc, xp, uc, nb, i2) + vv * ddir(ym, uc, yp, vv, nb, i2) + ww * ddir(zm, uc, zp, ww, nb, i2);
                double nu = F->s.nu + 0.5 * (gc(F, F->nut, i - 1, j, k) + gc(F, F->nut, i, j, k));
                double lap = (xp + xm + yp + ym + zp + zm - 6 * uc) * ih2;
                S->u_o[f] = uc + dt * (-adv + nu * lap);
                if (F->s.sponge > 0 && i > 0) S->u_o[f] *= sponge(F, i - 1, j);
            }
            /* v faces */
            for (int jj = j; jj <= (j == ny - 1 ? ny : j); jj++)
                for (int i = 0; i < nx; i++) {
                    size_t f = IV(F, i, jj, k);
                    if (F->kv[f] != K_FLUID) {
                        S->v_o[f] = F->kv[f] == K_FAN ? F->hv[f] : v[f];
                        continue;
                    }
                    double vc = v[f];
                    double uu = 0.25 * (gu(F, u, i, jj - 1, k) + gu(F, u, i + 1, jj - 1, k) + gu(F, u, i, jj, k) + gu(F, u, i + 1, jj, k));
                    double ww = 0.25 * (gw(F, w, i, jj - 1, k) + gw(F, w, i, jj, k) + gw(F, w, i, jj - 1, k + 1) + gw(F, w, i, jj, k + 1));
                    bool nb = NEAR(i, jj - 1, k) || NEAR(i, jj, k);
                    double xm = tv(F, v, i - 1, jj, k, vc), xp = tv(F, v, i + 1, jj, k, vc), zm = tv(F, v, i, jj, k - 1, vc), zp = tv(F, v, i, jj, k + 1, vc);
                    double ym = gv(F, v, i, jj - 1, k), yp = gv(F, v, i, jj + 1, k);
                    double adv = uu * ddir(xm, vc, xp, uu, nb, i2) + vc * ddir(ym, vc, yp, vc, nb, i2) + ww * ddir(zm, vc, zp, ww, nb, i2);
                    double nu = F->s.nu + 0.5 * (gc(F, F->nut, i, jj - 1, k) + gc(F, F->nut, i, jj, k));
                    double lap = (xp + xm + yp + ym + zp + zm - 6 * vc) * ih2;
                    S->v_o[f] = vc + dt * (-adv + nu * lap);
                    if (F->s.sponge > 0 && jj > 0) S->v_o[f] *= sponge(F, i, jj - 1);
                }
            /* w faces, with buoyancy */
            for (int kk = k; kk <= (k == nz - 1 ? nz : k); kk++)
                for (int i = 0; i < nx; i++) {
                    size_t f = IW(F, i, j, kk);
                    if (F->kw[f] != K_FLUID) {
                        S->w_o[f] = F->kw[f] == K_FAN ? F->hw[f] : w[f];
                        continue;
                    }
                    double wc = w[f];
                    double uu = 0.25 * (gu(F, u, i, j, kk - 1) + gu(F, u, i + 1, j, kk - 1) + gu(F, u, i, j, kk) + gu(F, u, i + 1, j, kk));
                    double vv = 0.25 * (gv(F, v, i, j, kk - 1) + gv(F, v, i, j + 1, kk - 1) + gv(F, v, i, j, kk) + gv(F, v, i, j + 1, kk));
                    bool nb = NEAR(i, j, kk - 1) || NEAR(i, j, kk);
                    double xm = tw(F, w, i - 1, j, kk, wc), xp = tw(F, w, i + 1, j, kk, wc), ym = tw(F, w, i, j - 1, kk, wc), yp = tw(F, w, i, j + 1, kk, wc);
                    double zm = gw(F, w, i, j, kk - 1), zp = gw(F, w, i, j, kk + 1);
                    double adv = uu * ddir(xm, wc, xp, uu, nb, i2) + vv * ddir(ym, wc, yp, vv, nb, i2) + wc * ddir(zm, wc, zp, wc, nb, i2);
                    double nu = F->s.nu + 0.5 * (gc(F, F->nut, i, j, kk - 1) + gc(F, F->nut, i, j, kk));
                    double lap = (xp + xm + yp + ym + zp + zm - 6 * wc) * ih2;
                    double rf = 0.5 * (gc(F, S->rho, i, j, kk - 1) + gc(F, S->rho, i, j, kk)), buoy = -g * (rf - F->rho_inf) / rf;
                    S->w_o[f] = wc + dt * (-adv + nu * lap + buoy);
                    if (F->s.sponge > 0 && kk > 0) S->w_o[f] *= sponge(F, i, j);
                }
        }
#undef NEAR
    /* the open faces: the normal velocity of the face just inside (no gradient); the projection, with the pressure
     * perturbation zero on these faces, then decides what flows in and out. (Evolving them by the momentum equation with
     * stencils reaching outside the box grew without bound on a side face.) The sponge along open sides keeps a wind
     * through the box from growing out of nothing (with the pressure zero on every open face such a flow costs nothing). */
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < ny; j++) {
            if (F->ku[IU(F, 0, j, k)] == K_OPEN) S->u_o[IU(F, 0, j, k)] = S->u_o[IU(F, 1, j, k)];
            if (F->ku[IU(F, nx, j, k)] == K_OPEN) S->u_o[IU(F, nx, j, k)] = S->u_o[IU(F, nx - 1, j, k)];
            for (int i = 0; i < nx; i++) {
                if (k == 0 && F->kw[IW(F, i, j, 0)] == K_OPEN) S->w_o[IW(F, i, j, 0)] = S->w_o[IW(F, i, j, 1)];
                if (k == nz - 1 && F->kw[IW(F, i, j, nz)] == K_OPEN) S->w_o[IW(F, i, j, nz)] = S->w_o[IW(F, i, j, nz - 1)];
            }
        }
    for (int k = k0; k < k1; k++)
        for (int i = 0; i < nx; i++) {
            if (F->kv[IV(F, i, 0, k)] == K_OPEN) S->v_o[IV(F, i, 0, k)] = S->v_o[IV(F, i, 1, k)];
            if (F->kv[IV(F, i, ny, k)] == K_OPEN) S->v_o[IV(F, i, ny, k)] = S->v_o[IV(F, i, ny - 1, k)];
        }
}

/* the projection: div(u - dt grad H / rho) = D */
static void rhs_slab(void *vs, int k0, int k1, int tid) {
    Stage *S = vs;
    Fire *F = S->F;
    const double dx = F->s.dx;
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < F->ny; j++)
            for (int i = 0; i < F->nx; i++) {
                size_t c = C(F, i, j, k);
                if (F->solid[c]) {
                    F->rhs[c] = 0;
                    continue;
                }
                double div = (S->u_o[IU(F, i + 1, j, k)] - S->u_o[IU(F, i, j, k)] + S->v_o[IV(F, i, j + 1, k)] - S->v_o[IV(F, i, j, k)] +
                              S->w_o[IW(F, i, j, k + 1)] - S->w_o[IW(F, i, j, k)]) / dx;
                F->rhs[c] = (div - F->D[c]) / S->dt;
            }
}
/* 1 / rho on every face the projection may change (a boundary face takes its cell's); 0 on held and closed faces */
static void coeff_slab(void *vs, int k0, int k1, int tid) {
    Stage *S = vs;
    Fire *F = S->F;
#define OPENF(kind) ((kind) == K_FLUID || (kind) == K_OPEN || (kind) == K_FAN)
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < F->ny; j++) {
            for (int i = 0; i <= F->nx; i++) {
                size_t f = IU(F, i, j, k);
                F->bx[f] = OPENF(F->ku[f]) ? 2 / (gc(F, S->rho, i - 1, j, k) + gc(F, S->rho, i, j, k)) : 0;
            }
            for (int jj = j; jj <= (j == F->ny - 1 ? F->ny : j); jj++)
                for (int i = 0; i < F->nx; i++) {
                    size_t f = IV(F, i, jj, k);
                    F->by[f] = OPENF(F->kv[f]) ? 2 / (gc(F, S->rho, i, jj - 1, k) + gc(F, S->rho, i, jj, k)) : 0;
                }
            for (int kk = k; kk <= (k == F->nz - 1 ? F->nz : k); kk++)
                for (int i = 0; i < F->nx; i++) {
                    size_t f = IW(F, i, j, kk);
                    F->bz[f] = OPENF(F->kw[f]) ? 2 / (gc(F, S->rho, i, j, kk - 1) + gc(F, S->rho, i, j, kk)) : 0;
                }
        }
#undef OPENF
}
static void correct_slab(void *vs, int k0, int k1, int tid) {
    Stage *S = vs;
    Fire *F = S->F;
    const double dx = F->s.dx, dt = S->dt;
    const double *H = F->Hs; /* the pressure perturbation, Pa; beyond an open face its negative (zero on the face) */
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < F->ny; j++) {
            for (int i = 0; i <= F->nx; i++) {
                size_t f = IU(F, i, j, k);
                if (F->bx[f] == 0) continue;
                double ha = i > 0 ? H[C(F, i - 1, j, k)] : -H[C(F, 0, j, k)], hb = i < F->nx ? H[C(F, i, j, k)] : -H[C(F, F->nx - 1, j, k)];
                S->u_o[f] -= dt * F->bx[f] * (hb - ha) / dx;
            }
            for (int jj = j; jj <= (j == F->ny - 1 ? F->ny : j); jj++)
                for (int i = 0; i < F->nx; i++) {
                    size_t f = IV(F, i, jj, k);
                    if (F->by[f] == 0) continue;
                    double ha = jj > 0 ? H[C(F, i, jj - 1, k)] : -H[C(F, i, 0, k)], hb = jj < F->ny ? H[C(F, i, jj, k)] : -H[C(F, i, F->ny - 1, k)];
                    S->v_o[f] -= dt * F->by[f] * (hb - ha) / dx;
                }
            for (int kk = k; kk <= (k == F->nz - 1 ? F->nz : k); kk++)
                for (int i = 0; i < F->nx; i++) {
                    size_t f = IW(F, i, j, kk);
                    if (F->bz[f] == 0) continue;
                    double ha = kk > 0 ? H[C(F, i, j, kk - 1)] : -H[C(F, i, j, 0)], hb = kk < F->nz ? H[C(F, i, j, kk)] : -H[C(F, i, j, F->nz - 1)];
                    S->w_o[f] -= dt * F->bz[f] * (hb - ha) / dx;
                }
        }
}
static double prof[8], prof_cycles, prof_solves;
static int prof_on = -1;
static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}
static void prof_report(void) {
    const char *nm[8] = {"props", "divergence", "scalars", "momentum", "rhs+coeff", "multigrid", "correct", "check"};
    for (int k = 0; k < 8; k++) fprintf(stderr, "fire profile: %-10s %8.3f s\n", nm[k], prof[k]);
    fprintf(stderr, "fire profile: %.1f V-cycles a solve over %.0f solves\n", prof_cycles / (prof_solves > 0 ? prof_solves : 1), prof_solves);
}
#define TICK(k)                                                                                                        \
    do {                                                                                                               \
        if (prof_on) {                                                                                                 \
            double t1 = now_s();                                                                                       \
            prof[k] += t1 - t0, t0 = t1;                                                                               \
        }                                                                                                              \
    } while (0)

/* a sealed box: the background pressure changes so that div u integrates to what the vents bring in,
 * D = D_heat - (dp0/dt) / (gamma p0) */
static void seal(Fire *F, const double *u, const double *v, const double *w) {
    const double dx = F->s.dx;
    double sum = 0, phi = 0;
    for (size_t c = 0; c < F->nc; c++) sum += F->D[c];
    const double *V[3] = {u, v, w};
    const uint8_t *K[3] = {F->ku, F->kv, F->kw};
    for (int sd = 0; sd < 6; sd++) {
        int na, nb, comp;
        side_dims(F, sd, &na, &nb);
        for (int b = 0; b < nb; b++)
            for (int a = 0; a < na; a++) {
                size_t f = side_face(F, sd, a, b, &comp);
                if (K[comp][f] == K_HELD) phi += ((sd & 1) ? -1 : 1) * V[comp][f] * dx * dx;
            }
    }
    double corr = (phi / (dx * dx * dx) - sum) / (double)F->nfluid;
    for (size_t c = 0; c < F->nc; c++)
        if (!F->solid[c]) F->D[c] += corr;
    F->dp0 += 0.5 * (-F->gamma * F->p0 * corr);
}

/* the first state made to meet the divergence it asks for (the vents' held faces are not, in a box at rest) */
static bool project_initial(Fire *F, ThreadPool *pool) {
    Stage *S = calloc(1, sizeof *S);
    if (!S) return false;
    { /* the fuel vents start from nothing (their ramp) */
        double *V[3] = {F->u, F->v, F->w};
        const int16_t *E[3] = {F->eu, F->ev, F->ew};
        for (int sd = 0; sd < 6; sd++) {
            int na, nb, comp;
            side_dims(F, sd, &na, &nb);
            for (int b = 0; b < nb; b++)
                for (int a = 0; a < na; a++) {
                    size_t f = side_face(F, sd, a, b, &comp);
                    if (E[comp][f] >= 0 && F->vent[E[comp][f]].fuel > 0 && F->s.ramp > 0) V[comp][f] = 0;
                }
        }
    }
    *S = (Stage){F, F->rho, F->YF, F->YP, F->u, F->v, F->w, NULL, NULL, NULL, F->u, F->v, F->w, 1.0, {0}, {0}, {0}, {{0}}, {{0}}};
    pool_for(pool, F->nz, 1, props, S);
    pool_for(pool, F->nz, 1, divergence, S);
    if (F->sealed) seal(F, F->u, F->v, F->w);
    F->dp0 = 0;
    pool_for(pool, F->nz, 1, rhs_slab, S);
    pool_for(pool, F->nz, 1, coeff_slab, S);
    bool ok = mg3d_set_coefficients(F->mg, F->bx, F->by, F->bz) && mg3d_solve(F->mg, F->Hs, F->rhs, 1e-10, 40, pool) >= 0;
    if (ok) pool_for(pool, F->nz, 1, correct_slab, S);
    memset(F->Hs, 0, F->nc * sizeof(double));
    free(S);
    return ok;
}

static bool stage(Fire *F, const double *rho, const double *YF, const double *YP, const double *u, const double *v, const double *w, double *rho_o,
                  double *YF_o, double *YP_o, double *u_o, double *v_o, double *w_o, double dt, ThreadPool *pool) {
    Stage *S = calloc(1, sizeof *S);
    if (!S) return false;
    *S = (Stage){F, rho, YF, YP, u, v, w, rho_o, YF_o, YP_o, u_o, v_o, w_o, dt, {0}, {0}, {0}, {{0}}, {{0}}};
    if (prof_on < 0) {
        prof_on = getenv("FIRE_PROFILE") != NULL;
        if (prof_on) atexit(prof_report);
    }
    const double dx = F->s.dx;
    double t0 = prof_on ? now_s() : 0;
    pool_for(pool, F->nz, 1, props, S);
    TICK(0);
    pool_for(pool, F->nz, 1, divergence, S);
    if (F->sealed) seal(F, u, v, w);
    TICK(1);
    pool_for(pool, F->nz, 1, scalars, S);
    TICK(2);
    pool_for(pool, F->nz, 1, momentum, S);
    TICK(3);
    pool_for(pool, F->nz, 1, rhs_slab, S);
    pool_for(pool, F->nz, 1, coeff_slab, S);
    double rmax = 0;
    for (size_t c = 0; c < F->nc; c++) rmax = fmax(rmax, fabs(F->rhs[c]));
    bool ok = mg3d_set_coefficients(F->mg, F->bx, F->by, F->bz);
    memcpy(F->Hs, F->H, F->nc * sizeof(double)); /* the last pressure as the first guess */
    TICK(4);
    int cyc = ok ? mg3d_solve(F->mg, F->Hs, F->rhs, 1e-10, 30, pool) : -1;
    if (prof_on) prof_cycles += cyc < 0 ? 30 : cyc, prof_solves++;
    if (!ok || (cyc < 0 && mg3d_last_residual(F->mg) > 1e-5)) {
        fprintf(stderr, "fire: the pressure did not converge at t = %.4g s (relative residual %.2e)\n", F->t, mg3d_last_residual(F->mg));
        free(S);
        return false;
    }
    TICK(5);
    pool_for(pool, F->nz, 1, correct_slab, S);
    TICK(6);
    memcpy(F->H, F->Hs, F->nc * sizeof(double));
    for (int t = 0; t < 64; t++) {
        F->stage_in += S->in[t], F->stage_out += S->out[t], F->stage_cap += S->capped[t];
        for (int q = 0; q < 5; q++) F->stage_flow[q] += S->flow[t][q];
        for (int q = 0; q < 7; q++) F->stage_heat[q] += S->heat[t][q];
    }
    free(S);
    /* the projection's check: the divergence now against D */
    double emax = 0, dmax = 0;
    for (int k = 0; k < F->nz; k++)
        for (int j = 0; j < F->ny; j++)
            for (int i = 0; i < F->nx; i++) {
                size_t c = C(F, i, j, k);
                if (F->solid[c]) continue;
                double div = (u_o[IU(F, i + 1, j, k)] - u_o[IU(F, i, j, k)] + v_o[IV(F, i, j + 1, k)] - v_o[IV(F, i, j, k)] + w_o[IW(F, i, j, k + 1)] -
                              w_o[IW(F, i, j, k)]) / dx;
                emax = fmax(emax, fabs(div - F->D[c])), dmax = fmax(dmax, fabs(F->D[c]));
            }
    /* relative to the larger of the largest |D| and the largest divergence the projection removed (D is round-off in
     * an isothermal flow) */
    double scale = fmax(dmax, rmax * dt);
    F->div_err = scale > 0 ? emax / scale : emax;
    TICK(7);
    return true;
}

/* fuel and air burn as fast as they mix; the heat released per unit volume */
static void react(void *vs, int k0, int k1, int tid) {
    Stage *S = vs;
    Fire *F = S->F;
    const double dt = S->dt, dx = F->s.dx, sa = F->s.s_air, taug = sqrt(2 * dx / F->s.g);
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < F->ny; j++)
            for (int i = 0; i < F->nx; i++) {
                size_t c = C(F, i, j, k);
                double yf = F->YF[c];
                if (yf <= 0 || F->solid[c]) {
                    F->q[c] = 0;
                    continue;
                }
                double yp = F->YP[c], ya = fmax(0, 1 - yf - yp);
                double Smag = F->Sm[c]; /* |S| */
                double tau = fmin(Smag > 0 ? 1 / Smag : 1e30, taug);
                double burn = fmin(yf, ya / sa) * (1 - exp(-dt / tau));
                F->YF[c] = yf - burn, F->YP[c] = fmin(1 - F->YF[c], yp + (1 + sa) * burn);
                F->q[c] = F->rho[c] * burn * F->s.dHc / dt;
            }
}

double fire_step(Fire *F, ThreadPool *pool) {
    if (!F->ready) {
        char err[160] = "";
        if (!prepare(F, err, sizeof err) || !project_initial(F, pool)) {
            fprintf(stderr, "%s\n", err[0] ? err : "fire: the first projection failed");
            return 0;
        }
    }
    /* the step: the CFL number on the fastest face and the diffusion limit (Heun's method is stable to dx^2 / (6 k)) */
    double umax = 1e-3, kmax = fmax(F->s.nu, F->s.nu / F->s.Pr);
    for (size_t f = 0; f < F->nu; f++) umax = fmax(umax, fabs(F->u[f]));
    for (size_t f = 0; f < F->nv; f++) umax = fmax(umax, fabs(F->v[f]));
    for (size_t f = 0; f < F->nw; f++) umax = fmax(umax, fabs(F->w[f]));
    double dmax = 0;
    for (size_t c = 0; c < F->nc; c++) kmax = fmax(kmax, F->nut[c] / PRT + F->s.nu / F->s.Pr), dmax = fmax(dmax, fabs(F->D[c]));
    const double dx = F->s.dx;
    double dt = fmin(F->s.cfl * dx / umax, 0.16 * dx * dx / kmax);
    /* and the expansion: no cell's density may fall by more than a fifth in a step (a first burst of burning in still
     * air, at the step's upper limit, once emptied a cell below the density floor) */
    if (dmax > 0) dt = fmin(dt, 0.2 / dmax);
    dt = fmin(dt, 0.05);
    { /* the fuel vents' ramp */
        double r = F->s.ramp > 0 ? fmin(1, (F->t + 0.5 * dt) / F->s.ramp) : 1;
        double *V[3] = {F->u, F->v, F->w}, *Hh[3] = {F->hu, F->hv, F->hw};
        const int16_t *E[3] = {F->eu, F->ev, F->ew};
        for (int sd = 0; sd < 6; sd++) {
            int na, nb, comp;
            side_dims(F, sd, &na, &nb);
            for (int b = 0; b < nb; b++)
                for (int a = 0; a < na; a++) {
                    size_t f = side_face(F, sd, a, b, &comp);
                    int e = E[comp][f];
                    if (e >= 0 && F->vent[e].fuel > 0) V[comp][f] = r * Hh[comp][f];
                }
        }
    }
    memcpy(F->rho0, F->rho, F->nc * 8), memcpy(F->YF0, F->YF, F->nc * 8), memcpy(F->YP0, F->YP, F->nc * 8), memcpy(F->AG0, F->AG, F->nc * 8);
    memcpy(F->u0, F->u, F->nu * 8), memcpy(F->v0, F->v, F->nv * 8), memcpy(F->w0, F->w, F->nw * 8);
    F->stage_in = F->stage_out = F->stage_cap = F->dp0 = 0;
    memset(F->stage_flow, 0, sizeof F->stage_flow), memset(F->stage_heat, 0, sizeof F->stage_heat);
    /* Heun: a stage from the start, a stage from its result, and the average */
    F->agi = F->AG0, F->ago = F->AG1;
    if (!stage(F, F->rho0, F->YF0, F->YP0, F->u0, F->v0, F->w0, F->rho1, F->YF1, F->YP1, F->u1, F->v1, F->w1, dt, pool)) return 0;
    F->agi = F->AG1, F->ago = F->AG;
    if (!stage(F, F->rho1, F->YF1, F->YP1, F->u1, F->v1, F->w1, F->rho, F->YF, F->YP, F->u, F->v, F->w, dt, pool)) return 0;
    for (size_t c = 0; c < F->nc; c++)
        F->rho[c] = 0.5 * (F->rho0[c] + F->rho[c]), F->YF[c] = 0.5 * (F->YF0[c] + F->YF[c]), F->YP[c] = 0.5 * (F->YP0[c] + F->YP[c]),
        F->AG[c] = 0.5 * (F->AG0[c] + F->AG[c]);
    for (size_t f = 0; f < F->nu; f++) F->u[f] = 0.5 * (F->u0[f] + F->u[f]);
    for (size_t f = 0; f < F->nv; f++) F->v[f] = 0.5 * (F->v0[f] + F->v[f]);
    for (size_t f = 0; f < F->nw; f++) F->w[f] = 0.5 * (F->w0[f] + F->w[f]);
    F->mass_in += 0.5 * F->stage_in, F->mass_out += 0.5 * F->stage_out, F->mass_capped += 0.5 * F->stage_cap;
    for (int q = 0; q < 5; q++) F->bflow[q] += 0.5 * F->stage_flow[q];
    for (int q = 0; q < 7; q++) F->heat[q] = 0.5 * F->stage_heat[q]; /* W, the mean of the two stages */
    F->p0 += dt * F->dp0;
    Stage *S = calloc(1, sizeof *S);
    if (!S) return 0;
    S->F = F, S->dt = dt;
    pool_for(pool, F->nz, 1, react, S);
    free(S);
    double hrr = 0;
    for (size_t c = 0; c < F->nc; c++) hrr += F->q[c];
    F->hrr_now = hrr * dx * dx * dx;
    for (size_t c = 0; c < F->nc; c++)
        if (!isfinite(F->rho[c]) || !isfinite(F->w[c])) return 0;
    F->t += dt;
    return dt;
}

void fire_fields(const Fire *F, float *T, float *q, float *speed, float *fuel) {
    for (int k = 0; k < F->nz; k++)
        for (int j = 0; j < F->ny; j++)
            for (int i = 0; i < F->nx; i++) {
                size_t c = C(F, i, j, k);
                bool sol = F->solid[c];
                if (T) T[c] = (float)(sol ? F->s.T0 : F->p0 * F->Wm[c] / (F->rho[c] * RU));
                if (q) q[c] = (float)F->q[c];
                if (fuel) fuel[c] = (float)F->YF[c];
                if (speed) {
                    double a = 0.5 * (F->u[IU(F, i, j, k)] + F->u[IU(F, i + 1, j, k)]), b = 0.5 * (F->v[IV(F, i, j, k)] + F->v[IV(F, i, j + 1, k)]);
                    double e = 0.5 * (F->w[IW(F, i, j, k)] + F->w[IW(F, i, j, k + 1)]);
                    speed[c] = sol ? 0.0f : (float)sqrt(a * a + b * b + e * e);
                }
            }
}

void fire_diagnostics(const Fire *F, double *div_err, double *hrr_now, double *mass, double *mass_in, double *mass_out) {
    if (div_err) *div_err = F->div_err;
    if (hrr_now) *hrr_now = F->hrr_now;
    if (mass) {
        double m = 0;
        for (size_t c = 0; c < F->nc; c++)
            if (!F->solid[c]) m += F->rho[c];
        *mass = m * F->s.dx * F->s.dx * F->s.dx;
    }
    if (mass_in) *mass_in = F->mass_in;
    if (mass_out) *mass_out = F->mass_out;
}
void fire_heat_flows(const Fire *F, double q[8]) { memcpy(q, F->heat, sizeof F->heat); }
void fire_boundary_flows(const Fire *F, double f[4]) { memcpy(f, F->bflow, 4 * sizeof(double)); }
double fire_age_out(const Fire *F) { return F->bflow[4]; }
const double *fire_age(const Fire *F) { return F->AG; }
double fire_pressure(const Fire *F) { return F->p0; }
bool fire_solid(const Fire *F, int i, int j, int k) { return F->solid[C(F, i, j, k)]; }
void fire_velocity(const Fire *F, int i, int j, int k, double vel[3]) {
    vel[0] = 0.5 * (F->u[IU(F, i, j, k)] + F->u[IU(F, i + 1, j, k)]), vel[1] = 0.5 * (F->v[IV(F, i, j, k)] + F->v[IV(F, i, j + 1, k)]);
    vel[2] = 0.5 * (F->w[IW(F, i, j, k)] + F->w[IW(F, i, j, k + 1)]);
}

double fire_T(const Fire *F, int i, int j, int k) { return F->p0 * F->Wm[C(F, i, j, k)] / (F->rho[C(F, i, j, k)] * RU); }
double fire_w(const Fire *F, int i, int j, int k) { return 0.5 * (F->w[IW(F, i, j, k)] + F->w[IW(F, i, j, k + 1)]); }
double fire_q(const Fire *F, int i, int j, int k) { return F->q[C(F, i, j, k)]; }
void fire_fastest(const Fire *F, double *speed, int *comp, int *i, int *j, int *k) {
    double m = 0;
    for (int kk = 0; kk < F->nz; kk++)
        for (int jj = 0; jj < F->ny; jj++)
            for (int ii = 0; ii <= F->nx; ii++) {
                double a = fabs(F->u[IU(F, ii, jj, kk)]);
                if (a > m) m = a, *comp = 0, *i = ii, *j = jj, *k = kk;
            }
    for (int kk = 0; kk < F->nz; kk++)
        for (int jj = 0; jj <= F->ny; jj++)
            for (int ii = 0; ii < F->nx; ii++) {
                double a = fabs(F->v[IV(F, ii, jj, kk)]);
                if (a > m) m = a, *comp = 1, *i = ii, *j = jj, *k = kk;
            }
    for (int kk = 0; kk <= F->nz; kk++)
        for (int jj = 0; jj < F->ny; jj++)
            for (int ii = 0; ii < F->nx; ii++) {
                double a = fabs(F->w[IW(F, ii, jj, kk)]);
                if (a > m) m = a, *comp = 2, *i = ii, *j = jj, *k = kk;
            }
    *speed = m;
}

double fire_mass_capped(const Fire *F) { return F->mass_capped; }
double fire_face(const Fire *F, int comp, int i, int j, int k) {
    return comp == 0 ? F->u[IU(F, i, j, k)] : comp == 1 ? F->v[IV(F, i, j, k)] : F->w[IW(F, i, j, k)];
}
double fire_cell(const Fire *F, int what, int i, int j, int k) { /* 0 rho, 1 pressure perturbation, 2 D, 3 nu_t, 4 fuel */
    size_t c = C(F, i, j, k);
    return what == 0 ? F->rho[c] : what == 1 ? F->H[c] : what == 2 ? F->D[c] : what == 3 ? F->nut[c] : F->YF[c];
}
