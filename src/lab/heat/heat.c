/* heat.c - see heat.h. */
#include "heat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../threads.h"

/* D2Q9: rest, the four axes, the four diagonals */
static const int CX[9] = {0, 1, 0, -1, 0, 1, -1, -1, 1};
static const int CY[9] = {0, 0, 1, 0, -1, 1, 1, -1, -1};
static const int OPP[9] = {0, 3, 4, 1, 2, 7, 8, 5, 6};
static const double W[9] = {4.0 / 9, 1.0 / 9, 1.0 / 9, 1.0 / 9, 1.0 / 9, 1.0 / 36, 1.0 / 36, 1.0 / 36, 1.0 / 36};

struct Heat {
    HtSpec s;
    ThreadPool *pool;
    int nx, ny;
    size_t n;
    double dt, vel;            /* time step, s; lattice velocity to m/s */
    double nu_lb, om_p, om_m;  /* TRT rates */
    double *f[9], *fc[9];      /* populations and their post-collision values */
    double *rho, *ux, *uy;     /* lattice units */
    double *nut;               /* eddy viscosity, lattice units (NULL when laminar) */
    double *ax, *ay;           /* fans, lattice acceleration */
    double *T, *Tn, *c, *cn;   /* temperature, K; passive scalar */
    double *q, *csrc;          /* W/m^3, scalar per second */
    unsigned char *mat, *mat0;  /* each cell's material now, and without the rotors */
    unsigned char *rot;         /* 1 + the rotor covering the cell, 0 if none */
    double tq[64][HT_MAX_ROTORS], torque[HT_MAX_ROTORS];
    int *patch_of;             /* per boundary face: side * max + position -> patch or -1 (wall) */
    int maxside;
    double t;
    long steps;
    bool unstable;
};

void ht_spec_defaults(HtSpec *s) {
    memset(s, 0, sizeof *s);
    s->nx = s->ny = 64;
    s->dx = 1e-3;
    s->nu = 1.5e-5, s->alpha = 2.1e-5, s->rho_cp = 1200, s->beta = 1.0 / 300;
    s->T_ref = s->T_init = 293.15;
    s->u_ref = 1.0, s->u_lb = 0.05, s->prandtl_t = 0.85, s->rho = 1.0;
    s->nmaterials = 1;
    snprintf(s->materials[0].name, sizeof s->materials[0].name, "fluid");
}

static void run(Heat *h, int count, ParallelFn fn, void *ctx) {
    if (h->pool) pool_for(h->pool, count, 1, fn, ctx);
    else fn(ctx, 0, count, 0);
}

static inline size_t at(const Heat *h, int i, int j) { return (size_t)j * h->nx + i; }
static inline int kind_of(const Heat *h, size_t c) { return h->mat[c] ? h->s.materials[h->mat[c]].kind : HT_FLUID; }
static inline bool is_solid(const Heat *h, size_t c) { return (h->rot && h->rot[c]) || kind_of(h, c) == HT_SOLID; }

/* the patch a boundary face belongs to: side, and the cell index along it */
static int face_patch(const Heat *h, int side, int pos) { return h->patch_of[side * h->maxside + pos]; }

static double feq(int q, double rho, double ux, double uy) {
    double cu = CX[q] * ux + CY[q] * uy;
    return W[q] * rho * (1 + 3 * cu + 4.5 * cu * cu - 1.5 * (ux * ux + uy * uy));
}

/* the inlet velocity (lattice) of patch p at the cell with index pos along its side */
static void inlet_velocity(const Heat *h, const HtPatch *P, int pos, double u[2]) {
    double s = (pos + 0.5) * h->s.dx, a = (s - P->from) / (P->to - P->from), v = P->velocity / h->vel;
    if (P->parabolic) v *= 6 * a * (1 - a);
    static const double N[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}; /* into the domain from each side */
    u[0] = v * N[P->side][0], u[1] = v * N[P->side][1];
}

Heat *ht_create(const HtSpec *s, char *err, size_t errlen) {
    if (s->nx < 4 || s->ny < 4 || !(s->dx > 0) || !(s->nu > 0) || !(s->u_ref > 0)) {
        snprintf(err, errlen, "heat: nx, ny >= 4, dx > 0, nu > 0 and u_ref > 0 are required");
        return NULL;
    }
    Heat *h = calloc(1, sizeof *h);
    h->s = *s;
    if (!(h->s.u_lb > 0)) h->s.u_lb = 0.05;
    h->nx = s->nx, h->ny = s->ny, h->n = (size_t)s->nx * s->ny;
    h->dt = h->s.u_lb * s->dx / s->u_ref;
    h->vel = s->dx / h->dt;
    h->nu_lb = s->nu * h->dt / (s->dx * s->dx);
    double tau = 3 * h->nu_lb + 0.5;
    if (tau < (s->smagorinsky > 0 ? 0.5 + 1e-7 : 0.502)) {
        snprintf(err, errlen, "heat: the lattice viscosity %.2e is too small (relaxation time %.4f); use smaller cells or a larger u_ref", h->nu_lb, tau);
        free(h);
        return NULL;
    }
    h->om_p = 1 / tau;
    h->om_m = 1 / (0.5 + 0.25 / (tau - 0.5)); /* the magic parameter 1/4 */
    size_t n = h->n;
    for (int q = 0; q < 9; q++) h->f[q] = malloc(n * sizeof(double)), h->fc[q] = malloc(n * sizeof(double));
    h->rho = malloc(n * sizeof(double)), h->ux = calloc(n, sizeof(double)), h->uy = calloc(n, sizeof(double));
    h->ax = calloc(n, sizeof(double)), h->ay = calloc(n, sizeof(double));
    if (s->smagorinsky > 0) h->nut = calloc(n, sizeof(double));
    if (!(h->s.prandtl_t > 0)) h->s.prandtl_t = 0.85;
    h->T = malloc(n * sizeof(double)), h->Tn = malloc(n * sizeof(double)), h->q = calloc(n, sizeof(double));
    h->mat = calloc(n, 1);
    if (s->c_diffusivity > 0) h->c = malloc(n * sizeof(double)), h->cn = malloc(n * sizeof(double)), h->csrc = calloc(n, sizeof(double));
    /* regions, in order */
    const double acc = h->dt * h->dt / s->dx; /* m/s^2 to lattice */
    for (int r = 0; r < s->nregions; r++) {
        const HtRegion *R = &s->regions[r];
        for (int j = 0; j < h->ny; j++)
            for (int i = 0; i < h->nx; i++) {
                double x = (i + 0.5) * s->dx, y = (j + 0.5) * s->dx;
                bool in = R->circle ? (x - R->lo[0]) * (x - R->lo[0]) + (y - R->lo[1]) * (y - R->lo[1]) < R->hi[0] * R->hi[0]
                                    : (x >= R->lo[0] && x < R->hi[0] && y >= R->lo[1] && y < R->hi[1]);
                if (!in) continue;
                size_t c = at(h, i, j);
                if (R->material >= 0 && R->material < s->nmaterials) h->mat[c] = (unsigned char)R->material;
                h->q[c] += R->q;
                h->ax[c] += R->force[0] * acc, h->ay[c] += R->force[1] * acc;
                if (h->csrc) h->csrc[c] += R->c_source;
            }
    }
    h->mat0 = malloc(n);
    memcpy(h->mat0, h->mat, n);
    if (s->nrotors > 0) h->rot = calloc(n, 1);
    /* boundary patches */
    h->maxside = h->nx > h->ny ? h->nx : h->ny;
    h->patch_of = malloc(4 * (size_t)h->maxside * sizeof(int));
    for (int k = 0; k < 4 * h->maxside; k++) h->patch_of[k] = -1;
    for (int p = 0; p < s->npatches; p++) {
        const HtPatch *P = &s->patches[p];
        int len = P->side < 2 ? h->ny : h->nx;
        for (int k = 0; k < len; k++) {
            double x = (k + 0.5) * s->dx;
            if (x >= P->from && x < P->to) h->patch_of[P->side * h->maxside + k] = p;
        }
    }
    for (size_t c = 0; c < n; c++) {
        h->rho[c] = 1;
        for (int q = 0; q < 9; q++) h->f[q][c] = h->fc[q][c] = W[q];
        h->T[c] = s->T_init;
        if (h->c) h->c[c] = s->c_init;
    }
    int nt = s->threads > 0 ? s->threads : cpu_perf_count();
    if (nt > 1) h->pool = pool_create(nt);
    return h;
}

void ht_free(Heat *h) {
    if (!h) return;
    if (h->pool) pool_destroy(h->pool);
    for (int q = 0; q < 9; q++) free(h->f[q]), free(h->fc[q]);
    free(h->rho), free(h->ux), free(h->uy), free(h->ax), free(h->ay), free(h->T), free(h->Tn), free(h->q), free(h->mat);
    free(h->c), free(h->cn), free(h->csrc), free(h->patch_of), free(h->nut), free(h->mat0), free(h->rot);
    free(h);
}

/* ---- rotors -------------------------------------------------------------------------------------------------------- */

/* is the point (x, y), in the rotor's own frame relative to its centre, inside rotor R? */
static bool in_rotor(const HtRotor *R, double x, double y) {
    double r = hypot(x, y);
    if (r < R->hub_r) return true;
    if (R->blades <= 0 || r < R->r_in || r > R->r_out) return false;
    double beta = R->angle_deg * M_PI / 180, phi = atan2(y, x);
    double sweep = (fabs(beta - M_PI / 2) < 1e-9) ? 0 : -(R->omega >= 0 ? 1 : -1) * log(r / R->r_in) / tan(beta);
    for (int k = 0; k < R->blades; k++) {
        double d = phi - (2 * M_PI * k / R->blades + sweep);
        d = remainder(d, 2 * M_PI);
        if (fabs(r * d) * sin(beta) < 0.5 * R->thickness) return true;
    }
    return false;
}

/* the rotors at the current time: cells they cover turn solid, cells they uncover take the local wall velocity */
static void update_rotors(Heat *h) {
    const HtSpec *s = &h->s;
    for (int k = 0; k < s->nrotors; k++) {
        const HtRotor *R = &s->rotors[k];
        double th = R->omega * h->t, ct = cos(th), st = sin(th), rmax = fmax(R->hub_r, R->r_out) + 2 * s->dx;
        int i0 = (int)floor((R->c[0] - rmax) / s->dx), i1 = (int)ceil((R->c[0] + rmax) / s->dx);
        int j0 = (int)floor((R->c[1] - rmax) / s->dx), j1 = (int)ceil((R->c[1] + rmax) / s->dx);
        for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < h->ny; j++)
            for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < h->nx; i++) {
                size_t c = at(h, i, j);
                double x = (i + 0.5) * s->dx - R->c[0], y = (j + 0.5) * s->dx - R->c[1];
                bool in = in_rotor(R, ct * x + st * y, -st * x + ct * y);
                if (in && h->rot[c] != k + 1) {
                    h->rot[c] = (unsigned char)(k + 1), h->mat[c] = (unsigned char)R->material;
                } else if (!in && h->rot[c] == k + 1) {
                    h->rot[c] = 0, h->mat[c] = h->mat0[c];
                    double ux = -R->omega * y / h->vel, uy = R->omega * x / h->vel; /* the wall's velocity where it was */
                    for (int q = 0; q < 9; q++) h->f[q][c] = feq(q, 1.0, ux, uy);
                    h->rho[c] = 1, h->ux[c] = ux, h->uy[c] = uy;
                }
            }
    }
}

double ht_rotor_torque(const Heat *h, int k) { return (k >= 0 && k < h->s.nrotors) ? h->torque[k] : 0; }

/* ---- flow ---------------------------------------------------------------------------------------------------------- */

static void collide_rows(void *ctx, int j0, int j1, int tid) {
    (void)tid;
    Heat *h = ctx;
    const HtSpec *s = &h->s;
    const double gx = -s->beta * s->g[0] * h->dt * h->dt / s->dx, gy = -s->beta * s->g[1] * h->dt * h->dt / s->dx;
    for (int j = j0; j < j1; j++)
        for (int i = 0; i < h->nx; i++) {
            size_t c = at(h, i, j);
            if (is_solid(h, c)) continue;
            double r = 0, mx = 0, my = 0;
            for (int q = 0; q < 9; q++) r += h->f[q][c], mx += CX[q] * h->f[q][c], my += CY[q] * h->f[q][c];
            double dT = h->T[c] - s->T_ref;
            double Fx = r * (h->ax[c] + gx * dT), Fy = r * (h->ay[c] + gy * dT);
            double ux, uy;
            if (kind_of(h, c) == HT_POROUS) { /* Darcy drag -nu/K u, taken implicitly */
                double K = s->materials[h->mat[c]].permeability / (s->dx * s->dx), d = h->nu_lb / K;
                ux = (mx + 0.5 * Fx) / (r * (1 + 0.5 * d)), uy = (my + 0.5 * Fy) / (r * (1 + 0.5 * d));
                Fx -= d * r * ux, Fy -= d * r * uy;
            } else {
                ux = (mx + 0.5 * Fx) / r, uy = (my + 0.5 * Fy) / r;
            }
            h->rho[c] = r, h->ux[c] = ux, h->uy[c] = uy;
            double fe[9], S[9];
            for (int q = 0; q < 9; q++) {
                fe[q] = feq(q, r, ux, uy);
                double cu = CX[q] * ux + CY[q] * uy, cF = CX[q] * Fx + CY[q] * Fy, uF = ux * Fx + uy * Fy;
                S[q] = W[q] * (9 * cu * cF - 3 * uF); /* the even part of Guo's term; the odd part is 3 w c.F */
            }
            double omp = h->om_p, omm = h->om_m;
            if (h->nut) { /* Smagorinsky: the relaxation time from the magnitude of the non-equilibrium stress */
                double pxx = 0, pyy = 0, pxy = 0;
                for (int q = 0; q < 9; q++) {
                    double d = h->f[q][c] - fe[q];
                    pxx += CX[q] * CX[q] * d, pyy += CY[q] * CY[q] * d, pxy += CX[q] * CY[q] * d;
                }
                double Pn = sqrt(pxx * pxx + pyy * pyy + 2 * pxy * pxy), t0 = 1 / h->om_p, cs = s->smagorinsky;
                double tt = 0.5 * (t0 + sqrt(t0 * t0 + 18 * M_SQRT2 * cs * cs * Pn / r));
                h->nut[c] = (tt - t0) / 3;
                omp = 1 / tt, omm = 1 / (0.5 + 0.25 / (tt - 0.5));
            }
            if (h->nut) {
                /* turbulent runs: regularised collision (Latt and Chopard, Math. Comput. Simul. 72, 2006), the
                 * non-equilibrium part projected on its second moment (with the forcing's share of it removed), far more
                 * robust than two relaxation times when the molecular relaxation time is close to 1/2 */
                double pxx = 0, pyy = 0, pxy = 0;
                for (int q = 0; q < 9; q++) {
                    double d = h->f[q][c] - fe[q];
                    pxx += CX[q] * CX[q] * d, pyy += CY[q] * CY[q] * d, pxy += CX[q] * CY[q] * d;
                }
                pxx += ux * Fx, pyy += uy * Fy, pxy += 0.5 * (ux * Fy + uy * Fx);
                for (int q = 0; q < 9; q++) {
                    double Qxx = CX[q] * CX[q] - 1.0 / 3, Qyy = CY[q] * CY[q] - 1.0 / 3, Qxy = CX[q] * CY[q];
                    double fneq = W[q] * 4.5 * (Qxx * pxx + Qyy * pyy + 2 * Qxy * pxy);
                    double Sm = 3 * W[q] * (CX[q] * Fx + CY[q] * Fy);
                    /* with the forcing's share added to the stress above, the source enters with 1/2: the moments are
                     * then those of Guo's scheme (momentum change F, stress (1 - w) Pneq + (1 - w/2)(uF + Fu)) */
                    h->fc[q][c] = fe[q] + (1 - omp) * fneq + 0.5 * (S[q] + Sm);
                }
                continue;
            }
            for (int q = 0; q < 9; q++) {
                int o = OPP[q];
                double fp = 0.5 * (h->f[q][c] + h->f[o][c]), fm = 0.5 * (h->f[q][c] - h->f[o][c]);
                double ep = 0.5 * (fe[q] + fe[o]), em = 0.5 * (fe[q] - fe[o]);
                double Sm = 3 * W[q] * (CX[q] * Fx + CY[q] * Fy);
                h->fc[q][c] = h->f[q][c] - omp * (fp - ep) - omm * (fm - em) + (1 - 0.5 * omp) * S[q] + (1 - 0.5 * omm) * Sm;
            }
        }
}

static void stream_rows(void *ctx, int j0, int j1, int tid) {
    Heat *h = ctx;
    for (int j = j0; j < j1; j++)
        for (int i = 0; i < h->nx; i++) {
            size_t c = at(h, i, j);
            if (is_solid(h, c)) continue;
            for (int q = 0; q < 9; q++) {
                int si = i - CX[q], sj = j - CY[q];
                if (h->s.periodic_x) si = (si + h->nx) % h->nx;
                bool out_x = si < 0 || si >= h->nx, out_y = sj < 0 || sj >= h->ny;
                if (!out_x && !out_y) {
                    size_t sc = at(h, si, sj);
                    if (!is_solid(h, sc)) {
                        h->f[q][c] = h->fc[q][sc];
                    } else if (h->rot && h->rot[sc]) { /* a moving rotor wall: bounce-back with its velocity at the link */
                        const HtRotor *R = &h->s.rotors[h->rot[sc] - 1];
                        double xm = (i + 0.5 - 0.5 * CX[q]) * h->s.dx - R->c[0], ym = (j + 0.5 - 0.5 * CY[q]) * h->s.dx - R->c[1];
                        double wx = -R->omega * ym / h->vel, wy = R->omega * xm / h->vel;
                        double fo = h->fc[OPP[q]][c], fn = fo + 6 * W[q] * h->rho[c] * (CX[q] * wx + CY[q] * wy);
                        h->f[q][c] = fn;
                        double Fx = -CX[q] * (fo + fn), Fy = -CY[q] * (fo + fn); /* on the rotor, lattice units */
                        h->tq[tid][h->rot[sc] - 1] += (xm * Fy - ym * Fx) / h->s.dx;
                    } else {
                        h->f[q][c] = h->fc[OPP[q]][c];
                    }
                    continue;
                }
                /* the population comes from outside: which side, and which patch there */
                int side, pos;
                if (out_x && out_y) side = -1, pos = 0; /* a corner: a wall */
                else if (out_x) side = si < 0 ? 0 : 1, pos = j;
                else side = sj < 0 ? 2 : 3, pos = i;
                int p = side >= 0 ? face_patch(h, side, pos) : -1;
                int type = p >= 0 ? h->s.patches[p].flow : HT_WALL;
                if (type == HT_INLET) {
                    double u[2];
                    inlet_velocity(h, &h->s.patches[p], pos, u);
                    h->f[q][c] = h->fc[OPP[q]][c] + 6 * W[q] * h->rho[c] * (CX[q] * u[0] + CY[q] * u[1]);
                } else if (type == HT_OUTLET) {
                    /* fixed pressure; no backflow: an inward velocity at the outlet is not carried into the boundary */
                    double vx = h->ux[c], vy = h->uy[c];
                    if (side == 0 && vx > 0) vx = 0;
                    if (side == 1 && vx < 0) vx = 0;
                    if (side == 2 && vy > 0) vy = 0;
                    if (side == 3 && vy < 0) vy = 0;
                    h->f[q][c] = feq(q, 1.0, vx, vy) + (h->fc[q][c] - feq(q, h->rho[c], h->ux[c], h->uy[c]));
                } else {
                    h->f[q][c] = h->fc[OPP[q]][c];
                }
            }
        }
}

/* ---- heat and the passive scalar ----------------------------------------------------------------------------------- */

typedef struct {
    Heat *h;
    const double *X;   /* the field being advanced */
    double *Xn;
    double dts;        /* the sub-step, s */
    bool scalar;       /* false: temperature; true: the passive scalar */
} FvCtx;

/* the eddy viscosity of a cell in m^2/s (0 when laminar) */
static inline double cell_nut(const Heat *h, size_t c) { return h->nut ? h->nut[c] * h->s.dx * h->s.dx / h->dt : 0; }
static inline double cell_k(const Heat *h, size_t c) {
    int k = kind_of(h, c);
    return k == HT_FLUID ? (h->s.alpha + cell_nut(h, c) / h->s.prandtl_t) * h->s.rho_cp : h->s.materials[h->mat[c]].k;
}
static inline double cell_rcp(const Heat *h, size_t c) { return kind_of(h, c) == HT_FLUID ? h->s.rho_cp : h->s.materials[h->mat[c]].rho_cp; }

/* van Leer's limited face value, upwind cell u, downwind cell d, the cell behind the upwind one uu (or -1) */
static inline double tvd(const double *X, long uu, size_t u, size_t d) {
    if (uu < 0) return X[u];
    double num = X[u] - X[uu], den = X[d] - X[u];
    if (fabs(den) < 1e-300) return X[u];
    double r = num / den, phi = (r + fabs(r)) / (1 + fabs(r));
    return X[u] + 0.5 * phi * den;
}

/* the physical velocity component a across the face between cells c and n (both fluid or porous) */
static inline double face_u(const Heat *h, size_t c, size_t nb, int a) {
    const double *u = a == 0 ? h->ux : h->uy;
    return 0.5 * (u[c] + u[nb]) * h->vel;
}

static void fv_rows(void *vctx, int j0, int j1, int tid) {
    (void)tid;
    FvCtx *F = vctx;
    Heat *h = F->h;
    const HtSpec *s = &h->s;
    const double dx = s->dx;
    for (int j = j0; j < j1; j++)
        for (int i = 0; i < h->nx; i++) {
            size_t c = at(h, i, j);
            bool cs = is_solid(h, c);
            if (F->scalar && cs) {
                F->Xn[c] = F->X[c];
                continue;
            }
            double kc = F->scalar ? s->c_diffusivity : cell_k(h, c);
            double cap = F->scalar ? 1.0 : cell_rcp(h, c), adv_cap = F->scalar ? 1.0 : s->rho_cp;
            double net = 0; /* into the cell, per unit depth, per second: W/m for heat */
            static const int DI[4] = {-1, 1, 0, 0}, DJ[4] = {0, 0, -1, 1};
            for (int d = 0; d < 4; d++) {
                int ii = i + DI[d], jj = j + DJ[d];
                if (s->periodic_x) ii = (ii + h->nx) % h->nx;
                int a = d < 2 ? 0 : 1;
                double sgn = (d & 1) ? 1.0 : -1.0; /* the face's outward normal along axis a */
                if (ii >= 0 && ii < h->nx && jj >= 0 && jj < h->ny) {
                    size_t nb = at(h, ii, jj);
                    bool ns = is_solid(h, nb);
                    if (F->scalar) {
                        if (ns) continue; /* the scalar does not enter solids */
                        double Dt = s->c_diffusivity + 0.5 * (cell_nut(h, c) + cell_nut(h, nb)) / s->prandtl_t;
                        net += Dt * (F->X[nb] - F->X[c]);
                    } else {
                        double kn = cell_k(h, nb), kf = (kc > 0 && kn > 0) ? 2 * kc * kn / (kc + kn) : 0;
                        net += kf * (F->X[nb] - F->X[c]);
                    }
                    if (!cs && !ns) { /* advection across a fluid face */
                        double un = face_u(h, c, nb, a) * sgn; /* outward */
                        size_t up = un > 0 ? c : nb, dn = un > 0 ? nb : c;
                        int ui = un > 0 ? i : ii, uj = un > 0 ? j : jj;
                        int bi = ui - (un > 0 ? DI[d] : -DI[d]), bj = uj - (un > 0 ? DJ[d] : -DJ[d]);
                        if (s->periodic_x) bi = (bi + h->nx) % h->nx;
                        long uu = (bi >= 0 && bi < h->nx && bj >= 0 && bj < h->ny && !is_solid(h, at(h, bi, bj))) ? (long)at(h, bi, bj) : -1;
                        /* advective form: the face carries the difference to this cell's value, so that a lattice
                         * velocity that is only nearly free of divergence does not create heat (T div u, with T near
                         * 300 K, would); the scheme then keeps every value between its neighbours' */
                        net -= adv_cap * un * (tvd(F->X, uu, up, dn) - F->X[c]) * dx;
                    }
                    continue;
                }
                /* a face on the domain's boundary */
                int side = d, pos = d < 2 ? j : i;
                int p = face_patch(h, side, pos);
                if (p < 0) continue; /* an adiabatic, impermeable wall */
                const HtPatch *P = &s->patches[p];
                if (P->flow == HT_INLET) {
                    double u[2];
                    inlet_velocity(h, P, pos, u);
                    double uin = fabs(u[a]) * h->vel, Xin = F->scalar ? P->c : P->T;
                    net += adv_cap * uin * (Xin - F->X[c]) * dx + kc * (Xin - F->X[c]) * 2;
                } else if (P->flow == HT_OUTLET) {
                    /* outflow carries the cell's own value: nothing to add in the advective form */
                } else if (!F->scalar) {
                    if (P->thermal == HT_FIXED_T) net += kc * (P->T - F->X[c]) * 2;
                    else if (P->thermal == HT_FLUX) net += P->flux * dx;
                }
            }
            double src = F->scalar ? (h->csrc ? h->csrc[c] : 0) * dx * dx : h->q[c] * dx * dx;
            F->Xn[c] = F->X[c] + F->dts * (net + src) / (cap * dx * dx);
        }
}

static void advance_fields(Heat *h) {
    const HtSpec *s = &h->s;
    /* explicit conduction: the sub-step keeps k dt / (rho cp dx^2) below 0.2 in every cell */
    double amax = s->alpha, numax = 0;
    if (h->nut)
        for (size_t c = 0; c < h->n; c++) numax = fmax(numax, cell_nut(h, c));
    amax += numax / s->prandtl_t;
    for (int m = 1; m < s->nmaterials; m++)
        if (s->materials[m].rho_cp > 0) amax = fmax(amax, s->materials[m].k / s->materials[m].rho_cp);
    int nsub = (int)ceil(h->dt * amax / (0.2 * s->dx * s->dx));
    if (nsub < 1) nsub = 1;
    FvCtx F = {h, h->T, h->Tn, h->dt / nsub, false};
    for (int k = 0; k < nsub; k++) {
        F.X = h->T, F.Xn = h->Tn;
        run(h, h->ny, fv_rows, &F);
        double *t = h->T;
        h->T = h->Tn, h->Tn = t;
    }
    if (h->c) {
        int ns = (int)ceil(h->dt * (s->c_diffusivity + numax / s->prandtl_t) / (0.2 * s->dx * s->dx));
        if (ns < 1) ns = 1;
        FvCtx G = {h, h->c, h->cn, h->dt / ns, true};
        for (int k = 0; k < ns; k++) {
            G.X = h->c, G.Xn = h->cn;
            run(h, h->ny, fv_rows, &G);
            double *t = h->c;
            h->c = h->cn, h->cn = t;
        }
    }
}

bool ht_advance(Heat *h, long nsteps) {
    for (long k = 0; k < nsteps && !h->unstable; k++) {
        if (h->rot) update_rotors(h);
        run(h, h->ny, collide_rows, h);
        memset(h->tq, 0, sizeof h->tq);
        run(h, h->ny, stream_rows, h);
        for (int r = 0; r < h->s.nrotors; r++) { /* lattice torque (r in cells) to N m per metre of depth */
            double t = 0;
            for (int th = 0; th < 64; th++) t += h->tq[th][r];
            h->torque[r] = t * h->s.rho * pow(h->s.dx, 4) / (h->dt * h->dt);
        }
        advance_fields(h);
        h->t += h->dt, h->steps++;
        if ((h->steps & 255) == 0) {
            size_t c = at(h, h->nx / 2, h->ny / 2);
            if (!isfinite(h->rho[c]) || !isfinite(h->T[c])) h->unstable = true;
        }
    }
    return !h->unstable;
}

double ht_time(const Heat *h) { return h->t; }
double ht_dt(const Heat *h) { return h->dt; }
long ht_steps(const Heat *h) { return h->steps; }
bool ht_unstable(const Heat *h) { return h->unstable; }
const double *ht_temperature(const Heat *h) { return h->T; }
const double *ht_scalar(const Heat *h) { return h->c; }
int ht_material_at(const Heat *h, int i, int j) { return h->mat[at(h, i, j)]; }
void ht_velocity(const Heat *h, int i, int j, double u[2]) {
    size_t c = at(h, i, j);
    u[0] = is_solid(h, c) ? 0 : h->ux[c] * h->vel, u[1] = is_solid(h, c) ? 0 : h->uy[c] * h->vel;
}

/* heat, flow, and flow-weighted means over a patch */
static void patch_sums(const Heat *h, int p, double *heat, double *flow, double *mT, double *mc) {
    const HtSpec *s = &h->s;
    const HtPatch *P = &s->patches[p];
    int len = P->side < 2 ? h->ny : h->nx, a = P->side < 2 ? 0 : 1;
    double sgn_in = (P->side & 1) ? -1.0 : 1.0; /* the inward normal along axis a */
    double Q = 0, V = 0, sT = 0, sc = 0, sw = 0;
    for (int k = 0; k < len; k++) {
        if (face_patch(h, P->side, k) != p) continue;
        int i = P->side == 0 ? 0 : P->side == 1 ? h->nx - 1 : k, j = P->side == 2 ? 0 : P->side == 3 ? h->ny - 1 : k;
        size_t c = at(h, i, j);
        double kc = cell_k(h, c), un = 0, Tf = h->T[c], cf = h->c ? h->c[c] : 0;
        if (P->flow == HT_INLET) {
            double u[2];
            inlet_velocity(h, P, k, u);
            un = fabs(u[a]) * h->vel, Tf = P->T, cf = P->c;
            Q += s->rho_cp * un * P->T * s->dx + kc * (P->T - h->T[c]) * 2;
        } else if (P->flow == HT_OUTLET) {
            un = (a == 0 ? h->ux[c] : h->uy[c]) * h->vel * sgn_in; /* negative when leaving */
            if (un < 0) Q += s->rho_cp * un * h->T[c] * s->dx;
        } else if (P->thermal == HT_FIXED_T) {
            Q += kc * (P->T - h->T[c]) * 2;
        } else if (P->thermal == HT_FLUX) {
            Q += P->flux * s->dx;
        }
        V += un * s->dx;
        double wgt = fabs(un) > 0 ? fabs(un) : 0;
        sT += wgt * Tf, sc += wgt * cf, sw += wgt;
    }
    *heat = Q, *flow = V;
    *mT = sw > 0 ? sT / sw : NAN, *mc = sw > 0 ? sc / sw : NAN;
}

double ht_patch_heat(const Heat *h, int p) {
    double a, b, c, d;
    patch_sums(h, p, &a, &b, &c, &d);
    return a;
}
double ht_patch_flow(const Heat *h, int p) {
    double a, b, c, d;
    patch_sums(h, p, &a, &b, &c, &d);
    return b;
}
double ht_patch_mean_T(const Heat *h, int p) {
    double a, b, c, d;
    patch_sums(h, p, &a, &b, &c, &d);
    return c;
}
double ht_patch_mean_c(const Heat *h, int p) {
    double a, b, c, d;
    patch_sums(h, p, &a, &b, &c, &d);
    return d;
}

double ht_material_max_T(const Heat *h, int m) {
    double t = -INFINITY;
    for (size_t c = 0; c < h->n; c++)
        if (h->mat[c] == m) t = fmax(t, h->T[c]);
    return t;
}

void ht_write_frame(Heat *h, LabWriter *w) {
    LabBlock b = {{h->nx, h->ny, 1}, 0, LAB_PLANE_XY, {0, 0, 0}, {h->s.dx, h->s.dx, h->s.dx}};
    size_t n = h->n;
    float *T = malloc(n * sizeof(float)), *sp = malloc(n * sizeof(float)), *ux = malloc(n * sizeof(float)), *uy = malloc(n * sizeof(float)),
          *vo = malloc(n * sizeof(float)), *m = malloc(n * sizeof(float)), *cc = h->c ? malloc(n * sizeof(float)) : NULL;
    for (int j = 0; j < h->ny; j++)
        for (int i = 0; i < h->nx; i++) {
            size_t c = at(h, i, j);
            double u[2];
            ht_velocity(h, i, j, u);
            T[c] = (float)h->T[c], ux[c] = (float)u[0], uy[c] = (float)u[1], sp[c] = (float)hypot(u[0], u[1]), m[c] = (float)h->mat[c];
            int il = i > 0 ? i - 1 : i, ir = i < h->nx - 1 ? i + 1 : i, jl = j > 0 ? j - 1 : j, jr = j < h->ny - 1 ? j + 1 : j;
            double dvdx = (h->uy[at(h, ir, j)] - h->uy[at(h, il, j)]) / ((ir - il) * h->s.dx), dudy = (h->ux[at(h, i, jr)] - h->ux[at(h, i, jl)]) / ((jr - jl) * h->s.dx);
            vo[c] = is_solid(h, c) ? 0.0f : (float)((dvdx - dudy) * h->vel);
            if (cc) cc[c] = (float)h->c[c];
        }
    lab_part_blocks(w, "heat", 1, &b);
    lab_field(w, "T", LAB_AT_CELL, n, T);
    lab_field(w, "speed", LAB_AT_CELL, n, sp);
    lab_field(w, "ux", LAB_AT_CELL, n, ux);
    lab_field(w, "uy", LAB_AT_CELL, n, uy);
    lab_field(w, "vorticity", LAB_AT_CELL, n, vo);
    lab_field(w, "material", LAB_AT_CELL, n, m);
    if (cc) lab_field(w, "c", LAB_AT_CELL, n, cc);
    free(T), free(sp), free(ux), free(uy), free(vo), free(m), free(cc);
}

char *ht_header_json(const HtSpec *s, const char *title) {
    char *o = malloc(1024);
    snprintf(o, 1024,
             "{\"domain\":\"heat\",\"title\":\"%s\",\"solver\":\"src/lab/heat: D2Q9 TRT lattice Boltzmann with Guo forcing (buoyancy, fans, "
             "Brinkman), finite-volume conjugate heat transfer, van Leer advection\",\"nx\":%d,\"ny\":%d,\"dx_m\":%.6g,"
             "\"fields\":{\"T\":\"K\",\"speed\":\"m/s\",\"ux\":\"m/s\",\"uy\":\"m/s\",\"vorticity\":\"1/s\",\"material\":\"1\",\"c\":\"1\"}}",
             title, s->nx, s->ny, s->dx);
    return o;
}
