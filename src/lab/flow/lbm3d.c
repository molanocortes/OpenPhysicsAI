/* lbm3d.c - three-dimensional lattice Boltzmann flow with curved bodies (lbm3d.h). */
#include "lbm3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { Q = 19 };
static const double LAMBDA = 3.0 / 16;
static const int CX[Q] = {0, 1, -1, 0, 0, 0, 0, 1, -1, 1, -1, 1, -1, 1, -1, 0, 0, 0, 0};
static const int CY[Q] = {0, 0, 0, 1, -1, 0, 0, 1, -1, -1, 1, 0, 0, 0, 0, 1, -1, 1, -1};
static const int CZ[Q] = {0, 0, 0, 0, 0, 1, -1, 0, 0, 0, 0, 1, -1, -1, 1, 1, -1, -1, 1};
static const int OPP[Q] = {0, 2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11, 14, 13, 16, 15, 18, 17};
static const double W[Q] = {1.0 / 3, 1.0 / 18, 1.0 / 18, 1.0 / 18, 1.0 / 18, 1.0 / 18, 1.0 / 18, 1.0 / 36, 1.0 / 36, 1.0 / 36,
                            1.0 / 36, 1.0 / 36, 1.0 / 36, 1.0 / 36, 1.0 / 36, 1.0 / 36, 1.0 / 36, 1.0 / 36, 1.0 / 36};
static int MY[Q], MZ[Q]; /* the direction with its y (z) component reversed */

struct Lbm3D {
    Lbm3DSpec s;
    size_t n;
    double *f[Q], *g[Q]; /* post-collision populations, now and next */
    unsigned char *solid;
    int *lk_start;        /* first link of a cell, -1 if it has none */
    unsigned char *lk_count;
    unsigned char *lk_dir; /* the incoming direction d (from the solid side) */
    double *lk_q;         /* fraction of the link, from the fluid cell along -c_d, at which the wall lies */
    long nlinks, steps;
    double force[3];
    double *tforce;       /* per thread, 4 each (x, y, z, unstable flag) */
    int nthreads_alloc;
};

static void init_tables(void) {
    for (int d = 0; d < Q; d++)
        for (int e = 0; e < Q; e++) {
            if (CX[e] == CX[d] && CY[e] == -CY[d] && CZ[e] == CZ[d]) MY[d] = e;
            if (CX[e] == CX[d] && CY[e] == CY[d] && CZ[e] == -CZ[d]) MZ[d] = e;
        }
}

Lbm3D *lbm3d_create(const Lbm3DSpec *s, char *err, size_t errlen) {
    init_tables();
    if (s->nx < 4 || s->ny < 3 || s->nz < 3 || !(s->nu > 0) || !(s->smagorinsky >= 0)) {
        snprintf(err, errlen, "lbm3d: need nx >= 4, ny, nz >= 3, nu > 0");
        return NULL;
    }
    double u2 = s->u_in[0] * s->u_in[0] + s->u_in[1] * s->u_in[1] + s->u_in[2] * s->u_in[2];
    if (u2 > 0.2 * 0.2 / 3) {
        snprintf(err, errlen, "lbm3d: inlet speed %.3g is above 0.2 c_s; lower it (a smaller time step)", sqrt(u2));
        return NULL;
    }
    Lbm3D *L = calloc(1, sizeof *L);
    if (!L) return NULL;
    L->s = *s;
    L->n = (size_t)s->nx * s->ny * s->nz;
    bool ok = true;
    for (int d = 0; d < Q && ok && !s->geometry_only; d++) {
        L->f[d] = malloc(L->n * sizeof(double)), L->g[d] = malloc(L->n * sizeof(double));
        ok = L->f[d] && L->g[d];
    }
    L->solid = calloc(L->n, 1), L->lk_start = malloc(L->n * sizeof(int)), L->lk_count = calloc(L->n, 1);
    if (!ok || !L->solid || !L->lk_start || !L->lk_count) {
        snprintf(err, errlen, "lbm3d: out of memory for %zu cells (%.0f MB)", L->n, L->n * (2.0 * Q * 8 + 6) / 1048576);
        lbm3d_free(L);
        return NULL;
    }
    for (size_t i = 0; i < L->n; i++) L->lk_start[i] = -1;
    lbm3d_init(L);
    return L;
}

void lbm3d_free(Lbm3D *L) {
    if (!L) return;
    for (int d = 0; d < Q; d++) free(L->f[d]), free(L->g[d]);
    free(L->solid), free(L->lk_start), free(L->lk_count), free(L->lk_dir), free(L->lk_q), free(L->tforce), free(L);
}

static inline double feq(int d, double rho, double ux, double uy, double uz, double u2) {
    double cu = CX[d] * ux + CY[d] * uy + CZ[d] * uz;
    return W[d] * rho * (1 + 3 * cu + 4.5 * cu * cu - 1.5 * u2);
}

/* a seeded random number in [-1, 1] for cell i and component c (PCG hash); lbm3d_metal.m has the same */
double lbm3d_noise(unsigned i, unsigned c) {
    unsigned h = i * 747796405u + c * 2891336453u + 12345u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return h / 4294967295.0 * 2 - 1;
}

void lbm3d_init(Lbm3D *L) {
    if (L->s.geometry_only) return;
    const double *u = L->s.u_in, a = L->s.noise * sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (size_t i = 0; i < L->n; i++) {
        double v[3];
        for (int c = 0; c < 3; c++) v[c] = u[c] + a * lbm3d_noise((unsigned)i, (unsigned)c);
        double u2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        for (int d = 0; d < Q; d++) L->f[d][i] = L->solid[i] ? W[d] : feq(d, 1.0, v[0], v[1], v[2], u2);
    }
    L->steps = 0;
}

bool lbm3d_set_bodies(Lbm3D *L, Lbm3DSdf sdf, void *ctx) {
    const int nx = L->s.nx, ny = L->s.ny, nz = L->s.nz;
    for (int z = 0; z < nz; z++)
        for (int y = 0; y < ny; y++)
            for (int x = 0; x < nx; x++) {
                double p[3] = {x + 0.5, y + 0.5, z + 0.5};
                L->solid[(size_t)x + (size_t)nx * ((size_t)y + (size_t)ny * z)] = sdf && sdf(p, ctx) < 0;
            }
    /* links: from each fluid cell, every direction whose upstream neighbour (x - c_d) is solid */
    long cap = 0, nl = 0;
    for (size_t i = 0; i < L->n; i++) L->lk_start[i] = -1, L->lk_count[i] = 0;
    for (int pass = 0; pass < 2; pass++) {
        nl = 0;
        for (int z = 0; z < nz; z++)
            for (int y = 0; y < ny; y++)
                for (int x = 0; x < nx; x++) {
                    size_t i = (size_t)x + (size_t)nx * ((size_t)y + (size_t)ny * z);
                    if (L->solid[i]) continue;
                    int cnt = 0;
                    for (int d = 1; d < Q; d++) {
                        int sx = x - CX[d], sy = y - CY[d], sz = z - CZ[d];
                        /* across a periodic face the neighbour is on the other side (found by lbm3dtest L1: without
                         * this, cells on the seam lost their wall links and pulled rest populations from the solid) */
                        if (L->s.bc_x == LBM3D_PERIODIC) sx = (sx + nx) % nx;
                        if (L->s.bc_y == LBM3D_PERIODIC) sy = (sy + ny) % ny;
                        if (L->s.bc_z == LBM3D_PERIODIC) sz = (sz + nz) % nz;
                        if (sx < 0 || sy < 0 || sz < 0 || sx >= nx || sy >= ny || sz >= nz) continue;
                        size_t j = (size_t)sx + (size_t)nx * ((size_t)sy + (size_t)ny * sz);
                        if (!L->solid[j]) continue;
                        if (pass == 1) {
                            /* bisection for the wall along the link, from the fluid centre (outside) to the solid one */
                            double a = 0, b = 1, c0[3] = {x + 0.5, y + 0.5, z + 0.5};
                            for (int it = 0; it < 40; it++) {
                                double m = 0.5 * (a + b), p[3] = {c0[0] - m * CX[d], c0[1] - m * CY[d], c0[2] - m * CZ[d]};
                                if (sdf(p, ctx) < 0) b = m;
                                else a = m;
                            }
                            if (!cnt) L->lk_start[i] = (int)nl;
                            L->lk_dir[nl] = (unsigned char)d, L->lk_q[nl] = 0.5 * (a + b);
                        }
                        cnt++, nl++;
                    }
                    if (pass == 1) L->lk_count[i] = (unsigned char)cnt;
                }
        if (pass == 0) {
            cap = nl;
            free(L->lk_dir), free(L->lk_q);
            L->lk_dir = malloc((size_t)(cap ? cap : 1)), L->lk_q = malloc((size_t)(cap ? cap : 1) * sizeof(double));
            if (!L->lk_dir || !L->lk_q) return false;
        }
    }
    L->nlinks = nl;
    for (int d = 0; d < Q && !L->s.geometry_only; d++) /* solid cells hold rest populations (never read by fluid except through links) */
        for (size_t i = 0; i < L->n; i++)
            if (L->solid[i]) L->f[d][i] = W[d];
    return true;
}

typedef struct {
    Lbm3D *L;
    int nthreads;
} StepCtx;

static void step_slab(void *vctx, int z0, int z1, int tid) {
    StepCtx *C = vctx;
    Lbm3D *L = C->L;
    const Lbm3DSpec *s = &L->s;
    const int nx = s->nx, ny = s->ny, nz = s->nz;
    const size_t sxy = (size_t)nx * ny;
    const double tau0 = 3 * s->nu + 0.5, cs2 = s->smagorinsky * s->smagorinsky;
    const double ax = s->accel[0], ay = s->accel[1], az = s->accel[2];
    const bool forced = ax != 0 || ay != 0 || az != 0, inout = s->bc_x == LBM3D_INOUT;
    double *fo[Q], *gn[Q];
    for (int d = 0; d < Q; d++) fo[d] = L->f[d], gn[d] = L->g[d];
    double Fx = 0, Fy = 0, Fz = 0, bad = 0;
    for (int z = z0; z < z1; z++)
        for (int y = 0; y < ny; y++) {
            /* per row: where each direction's population comes from (row offset and direction, after wrap or mirror) */
            long rowoff[Q];
            int rdir[Q];
            bool bounce[Q];
            for (int d = 0; d < Q; d++) {
                int sy = y - CY[d], sz = z - CZ[d], dd = d;
                bounce[d] = s->floor && sz < 0; /* from below the floor: what left for it comes back */
                if (sy < 0 || sy >= ny) {
                    if (s->bc_y == LBM3D_PERIODIC) sy = (sy + ny) % ny;
                    else sy = y, dd = MY[dd];
                }
                if (sz < 0 || sz >= nz) {
                    if (s->bc_z == LBM3D_PERIODIC) sz = (sz + nz) % nz;
                    else sz = z, dd = MZ[dd];
                }
                rowoff[d] = (long)((size_t)sy * nx + (size_t)sz * sxy), rdir[d] = dd;
            }
            const size_t row = (size_t)y * nx + (size_t)z * sxy;
            for (int x = 0; x < nx; x++) {
                const size_t i = row + (size_t)x;
                if (L->solid[i]) {
                    for (int d = 0; d < Q; d++) gn[d][i] = W[d];
                    continue;
                }
                if (inout && x == 0) { /* inlet: equilibrium at the inlet velocity, density from the next cell */
                    double rho = 0;
                    for (int d = 0; d < Q; d++) rho += fo[d][i + 1];
                    double u[3] = {s->u_in[0], s->u_in[1], s->u_in[2]};
                    if (s->inlet_noise > 0) {
                        double a = s->inlet_noise * sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                        for (int c = 0; c < 3; c++) u[c] += a * lbm3d_noise((unsigned)i ^ ((unsigned)L->steps * 2654435761u), (unsigned)c + 3);
                    }
                    double u2 = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
                    for (int d = 0; d < Q; d++) gn[d][i] = feq(d, rho, u[0], u[1], u[2], u2);
                    continue;
                }
                double fi[Q], ou[3] = {0, 0, 0}, ou2 = 0;
                if (inout && x == nx - 1 && s->outlet == 1) { /* a pressure outlet: its own velocity of the step before */
                    double r = 0, j[3] = {0, 0, 0};
                    for (int d = 0; d < Q; d++) r += fo[d][i], j[0] += CX[d] * fo[d][i], j[1] += CY[d] * fo[d][i], j[2] += CZ[d] * fo[d][i];
                    for (int c = 0; c < 3; c++) ou[c] = j[c] / r;
                    ou2 = ou[0] * ou[0] + ou[1] * ou[1] + ou[2] * ou[2];
                }
                for (int d = 0; d < Q; d++) {
                    int sx = x - CX[d];
                    if (sx >= nx && inout) { /* outlet: what would come from beyond it is its own from the step before */
                        fi[d] = s->outlet == 1 ? feq(d, 1.0, ou[0], ou[1], ou[2], ou2) : fo[d][i];
                        continue;
                    }
                    if (bounce[d]) {
                        fi[d] = fo[OPP[d]][i];
                        continue;
                    }
                    if (sx < 0) sx += nx;
                    else if (sx >= nx) sx -= nx;
                    fi[d] = fo[rdir[d]][(size_t)rowoff[d] + (size_t)sx];
                }
                if (L->lk_start[i] >= 0) { /* curved walls: Bouzidi's interpolated bounce-back, and the momentum exchanged */
                    for (int k = L->lk_start[i], e = k + L->lk_count[i]; k < e; k++) {
                        int d = L->lk_dir[k], o = OPP[d];
                        double q = L->lk_q[k], fin;
                        long xn = x + CX[d], yn = y + CY[d], zn = z + CZ[d];
                        if (s->bc_x == LBM3D_PERIODIC) xn = (xn + nx) % nx;
                        if (s->bc_y == LBM3D_PERIODIC) yn = (yn + ny) % ny;
                        if (s->bc_z == LBM3D_PERIODIC) zn = (zn + nz) % nz;
                        bool second = xn >= 0 && yn >= 0 && zn >= 0 && xn < nx && yn < ny && zn < nz &&
                                      !L->solid[(size_t)xn + (size_t)nx * ((size_t)yn + (size_t)ny * zn)];
                        size_t j = second ? (size_t)xn + (size_t)nx * ((size_t)yn + (size_t)ny * zn) : i;
                        if (q < 0.5 && second) fin = 2 * q * fo[o][i] + (1 - 2 * q) * fo[o][j];
                        else if (q >= 0.5) fin = fo[o][i] / (2 * q) + (2 * q - 1) / (2 * q) * fo[d][i];
                        else fin = fo[o][i];
                        fi[d] = fin;
                        /* the body takes c_o (f_o out + f_d back), less the uniform ambient part 2 w: it cancels over a closed
                         * body and would be a false force on one standing on the floor */
                        double m = fo[o][i] + fin - 2 * W[o];
                        Fx += CX[o] * m, Fy += CY[o] * m, Fz += CZ[o] * m;
                    }
                }
                /* moments */
                double rho = 0, jx = 0, jy = 0, jz = 0;
                for (int d = 0; d < Q; d++) rho += fi[d], jx += CX[d] * fi[d], jy += CY[d] * fi[d], jz += CZ[d] * fi[d];
                /* the equilibrium at j / rho; the force enters whole through the antisymmetric part (Ginzburg's TRT forcing),
                 * and the fluid's velocity is j / rho + a / 2 */
                double ux = jx / rho, uy = jy / rho, uz = jz / rho, u2 = ux * ux + uy * uy + uz * uz;
                if (!(rho > 0) || !(u2 < 0.16)) bad = 1;
                double fe[Q], pxx = 0, pyy = 0, pzz = 0, pxy = 0, pxz = 0, pyz = 0;
                for (int d = 0; d < Q; d++) {
                    fe[d] = feq(d, rho, ux, uy, uz, u2);
                    if (cs2 > 0) {
                        double ne = fi[d] - fe[d];
                        pxx += CX[d] * CX[d] * ne, pyy += CY[d] * CY[d] * ne, pzz += CZ[d] * CZ[d] * ne;
                        pxy += CX[d] * CY[d] * ne, pxz += CX[d] * CZ[d] * ne, pyz += CY[d] * CZ[d] * ne;
                    }
                }
                double tau = tau0;
                if (s->sponge > 0 && (x > nx - 1 - s->sponge || x < s->sponge)) { /* sponges at both ends */
                    double r = x < s->sponge ? (double)(s->sponge - x) / s->sponge : (double)(x - (nx - 1 - s->sponge)) / s->sponge;
                    tau = 3 * (s->nu + (s->sponge_nu - s->nu) * r * r) + 0.5;
                }
                const double tau_mol = tau;
                if (cs2 > 0) {
                    double Qm = sqrt(2 * (pxx * pxx + pyy * pyy + pzz * pzz + 2 * (pxy * pxy + pxz * pxz + pyz * pyz)));
                    tau = 0.5 * (tau_mol + sqrt(tau_mol * tau_mol + 18 * sqrt(2.0) * cs2 * Qm / rho));
                }
                const double wp = 1 / tau;
                if (s->collision == LBM3D_REGULARIZED) {
                    /* the non-equilibrium part projected on its second moment: 4.5 w (c c - I / 3) : Pi. The equilibrium is at
                     * j / rho, so the first moment has no non-equilibrium part; the force enters whole, antisymmetric */
                    if (cs2 == 0) {
                        pxx = pyy = pzz = pxy = pxz = pyz = 0;
                        for (int d = 0; d < Q; d++) {
                            double ne = fi[d] - fe[d];
                            pxx += CX[d] * CX[d] * ne, pyy += CY[d] * CY[d] * ne, pzz += CZ[d] * CZ[d] * ne;
                            pxy += CX[d] * CY[d] * ne, pxz += CX[d] * CZ[d] * ne, pyz += CY[d] * CZ[d] * ne;
                        }
                    }
                    const double keep = 1 - wp;
                    for (int d = 0; d < Q; d++) {
                        const double cx = CX[d], cy = CY[d], cz = CZ[d];
                        double reg = 4.5 * W[d] * ((cx * cx - 1.0 / 3) * pxx + (cy * cy - 1.0 / 3) * pyy + (cz * cz - 1.0 / 3) * pzz +
                                                   2 * (cx * cy * pxy + cx * cz * pxz + cy * cz * pyz));
                        gn[d][i] = fe[d] + keep * reg + (forced ? 3 * W[d] * rho * (cx * ax + cy * ay + cz * az) : 0);
                    }
                    continue;
                }
                /* two relaxation times: the symmetric part at the viscous rate, the antisymmetric at the rate that keeps
                 * Lambda = 3/16, for which bounce-back walls lie exactly halfway (Ginzburg) */
                const double wm = 1 / (LAMBDA / (tau - 0.5) + 0.5);
                gn[0][i] = fi[0] - wp * (fi[0] - fe[0]);
                for (int d = 1; d < Q; d += 2) {
                    const int o = d + 1;
                    const double sp = 0.5 * (fi[d] + fi[o]), sm = 0.5 * (fi[d] - fi[o]), ep = 0.5 * (fe[d] + fe[o]), em = 0.5 * (fe[d] - fe[o]);
                    double dp = -wp * (sp - ep), dm = -wm * (sm - em);
                    if (forced) dm += W[d] * rho * 3 * (CX[d] * ax + CY[d] * ay + CZ[d] * az);
                    gn[d][i] = fi[d] + dp + dm, gn[o][i] = fi[o] + dp - dm;
                }
            }
        }
    double *tf = &L->tforce[4 * tid];
    tf[0] += Fx, tf[1] += Fy, tf[2] += Fz, tf[3] += bad;
}

bool lbm3d_step(Lbm3D *L, ThreadPool *pool) {
    int nt = pool ? pool_size(pool) : 1;
    if (L->nthreads_alloc < nt) {
        free(L->tforce);
        L->tforce = calloc(4 * (size_t)nt, sizeof(double));
        if (!L->tforce) return false;
        L->nthreads_alloc = nt;
    }
    memset(L->tforce, 0, 4 * (size_t)L->nthreads_alloc * sizeof(double));
    StepCtx C = {L, nt};
    if (pool) pool_for(pool, L->s.nz, 1, step_slab, &C);
    else step_slab(&C, 0, L->s.nz, 0);
    double F[4] = {0, 0, 0, 0};
    for (int t = 0; t < L->nthreads_alloc; t++)
        for (int k = 0; k < 4; k++) F[k] += L->tforce[4 * t + k];
    L->force[0] = F[0], L->force[1] = F[1], L->force[2] = F[2];
    for (int d = 0; d < Q; d++) {
        double *t = L->f[d];
        L->f[d] = L->g[d], L->g[d] = t;
    }
    L->steps++;
    return F[3] == 0;
}

void lbm3d_macro(const Lbm3D *L, double *rho, double *u) {
    for (size_t i = 0; i < L->n; i++) {
        double r = 0, jx = 0, jy = 0, jz = 0;
        for (int d = 0; d < Q; d++) {
            double v = L->f[d][i];
            r += v, jx += CX[d] * v, jy += CY[d] * v, jz += CZ[d] * v;
        }
        bool sol = L->solid[i];
        if (rho) rho[i] = sol ? 1 : r;
        if (u) {
            /* the stored populations are post-collision, the force already in them: the fluid's velocity at the step is
             * j_post / rho - a + a / 2 */
            u[3 * i] = sol ? 0 : jx / r - 0.5 * L->s.accel[0];
            u[3 * i + 1] = sol ? 0 : jy / r - 0.5 * L->s.accel[1];
            u[3 * i + 2] = sol ? 0 : jz / r - 0.5 * L->s.accel[2];
        }
    }
}

void lbm3d_force(const Lbm3D *L, double F[3]) { F[0] = L->force[0], F[1] = L->force[1], F[2] = L->force[2]; }
const unsigned char *lbm3d_solid(const Lbm3D *L) { return L->solid; }
long lbm3d_links(const Lbm3D *L) { return L->nlinks; }
long lbm3d_steps(const Lbm3D *L) { return L->steps; }
const Lbm3DSpec *lbm3d_spec(const Lbm3D *L) { return &L->s; }
void lbm3d_set_inlet(Lbm3D *L, const double u[3]) { L->s.u_in[0] = u[0], L->s.u_in[1] = u[1], L->s.u_in[2] = u[2]; }
void lbm3d_link_arrays(const Lbm3D *L, const int **start, const unsigned char **count, const unsigned char **dir, const double **q) {
    *start = L->lk_start, *count = L->lk_count, *dir = L->lk_dir, *q = L->lk_q;
}
