/* gas.c - see gas.h. Layout of this file: geometry (level sets), the block tree and its hash, ghost cells, the image
 * points of the bodies, the Riemann fluxes, the time step, refinement, output. */
#include "gas.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../threads.h"

#define NB GAS_NB
#define NG 3               /* ghost layers: reconstruction needs two; merging small cut cells across block edges three */
#define S (NB + 2 * NG)    /* stored cells per block side */
#define NV 5               /* rho, rho u, rho v, E, G = 1 / (gamma - 1) */
#define NF 6               /* the fluxes of the five, and the face velocity (for the G equation) */
#define IDX(I, J) ((J) * S + (I))
#define NFX ((NB + 1) * NB) /* x faces per block */

typedef struct Block {
    int level, bi, bj;
    int parent, child[4]; /* child order: (0,0), (1,0), (0,1), (1,1) */
    bool leaf, alive;
    signed char flag; /* +1 refine, -1 may coarsen, 0 keep */
    double dx, ox, oy; /* cell size and lower-left corner of the interior */
    double *U, *U0;    /* NV * S * S */
    double *F, *H;     /* NF * NFX: area-integrated face fluxes, x faces and y faces */
    float *phi;        /* S * S: signed distance to the bodies at cell centres, negative inside */
    double *sens;      /* 2 * S * S: pressure-jump sensors in x and in y, for the flux switch */
    /* cut cells (all zero-free for a block away from bodies: kap 1, full areas) */
    float *kap;        /* S * S: the fluid fraction of each cell's planar area; 0 = covered by a body */
    double *vol;       /* S * S: fluid volume of each cell (per radian when axisymmetric), ghost cells included */
    double *ax, *ay;   /* NFX each: open area of the x faces and of the y faces */
    double *wall;      /* 2 * S * S: the wall's area vector in each cell, pointing from fluid into body */
    double *Uh;        /* NV * S * S: the provisional state of a stage, before small cells are merged */
} Block;

struct Gas {
    GasSpec spec;
    double ylen, dx0;
    Block *b;
    int nb, cap;
    int *free_list, nfree;
    int *leaves, nleaves;
    uint64_t *hkey;
    int *hval, hcap;
    double t, dt_last;
    int steps;
    ThreadPool *pool;
    int stage; /* which RK stage the parallel callbacks are in */
    double dt;
    double *dtmin; /* per thread */
    int nthreads;
};

/* ---- geometry ---------------------------------------------------------------------------------------------- */

static double seg_dist2(double px, double py, double ax, double ay, double bx, double by) {
    double ex = bx - ax, ey = by - ay, wx = px - ax, wy = py - ay;
    double l2 = ex * ex + ey * ey;
    double t = l2 > 0 ? (wx * ex + wy * ey) / l2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double dx = wx - t * ex, dy = wy - t * ey;
    return dx * dx + dy * dy;
}

static double body_phi(const GasBody *bd, double x, double y) {
    if (bd->kind == GAS_BODY_CIRCLE) return hypot(x - bd->cx, y - bd->cy) - bd->r;
    double d2 = INFINITY;
    bool inside = false;
    for (int i = 0, j = bd->npts - 1; i < bd->npts; j = i++) {
        double xi = bd->pts[2 * i], yi = bd->pts[2 * i + 1], xj = bd->pts[2 * j], yj = bd->pts[2 * j + 1];
        d2 = fmin(d2, seg_dist2(x, y, xj, yj, xi, yi));
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) inside = !inside;
    }
    return inside ? -sqrt(d2) : sqrt(d2);
}

/* the level set of all bodies; in an axisymmetric run the body is also mirrored below the axis */
static double phi_at(const Gas *g, double x, double y) {
    double p = INFINITY;
    double yy = g->spec.axisymmetric ? fabs(y) : y;
    for (int k = 0; k < g->spec.nbodies; k++) p = fmin(p, body_phi(&g->spec.bodies[k], x, yy));
    return p;
}

static GasPrim initial_at(const GasSpec *s, double x, double y) {
    GasPrim w = s->initial;
    for (int k = 0; k < s->nregions; k++) {
        const GasRegion *r = &s->regions[k];
        bool in = r->kind == GAS_REGION_BOX ? (x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1) : (hypot(x - r->cx, y - r->cy) < r->r);
        if (in) w = r->state;
    }
    return w;
}

static void prim_to_cons(const GasPrim *w, double *U, size_t stride) {
    double G = 1.0 / (w->gamma - 1.0);
    U[0] = w->rho;
    U[stride] = w->rho * w->u;
    U[2 * stride] = w->rho * w->v;
    U[3 * stride] = G * w->p + 0.5 * w->rho * (w->u * w->u + w->v * w->v);
    U[4 * stride] = G;
}

/* primitive (rho, u, v, p, G) from a conserved cell */
static inline void cons_to_w(const double *U, size_t c, double W[5]) {
    const size_t n = (size_t)S * S;
    double rho = U[c], mx = U[n + c], my = U[2 * n + c], E = U[3 * n + c], G = U[4 * n + c];
    double u = mx / rho, v = my / rho;
    W[0] = rho, W[1] = u, W[2] = v, W[3] = (E - 0.5 * rho * (u * u + v * v)) / G, W[4] = G;
}

static inline void w_to_cons(const double W[5], double *U, size_t c) {
    const size_t n = (size_t)S * S;
    U[c] = W[0];
    U[n + c] = W[0] * W[1];
    U[2 * n + c] = W[0] * W[2];
    U[3 * n + c] = W[4] * W[3] + 0.5 * W[0] * (W[1] * W[1] + W[2] * W[2]);
    U[4 * n + c] = W[4];
}

/* ---- the block tree ---------------------------------------------------------------------------------------- */

static uint64_t hkey(int level, int bi, int bj) { return ((uint64_t)(level + 1) << 48) | ((uint64_t)(uint32_t)bi << 24) | (uint64_t)(uint32_t)bj; }

static void hash_rebuild(Gas *g) {
    int need = 16;
    while (need < 4 * g->nb) need *= 2;
    if (need != g->hcap) {
        free(g->hkey);
        free(g->hval);
        g->hcap = need;
        g->hkey = malloc((size_t)need * sizeof *g->hkey);
        g->hval = malloc((size_t)need * sizeof *g->hval);
    }
    memset(g->hkey, 0, (size_t)g->hcap * sizeof *g->hkey);
    for (int i = 0; i < g->nb; i++) {
        if (!g->b[i].alive) continue;
        uint64_t k = hkey(g->b[i].level, g->b[i].bi, g->b[i].bj);
        uint64_t h = (k * 0x9E3779B97F4A7C15ull) >> 20;
        int m = g->hcap - 1, at = (int)(h & (uint64_t)m);
        while (g->hkey[at]) at = (at + 1) & m;
        g->hkey[at] = k;
        g->hval[at] = i;
    }
}

static int find_block(const Gas *g, int level, int bi, int bj) {
    if (level < 0 || bi < 0 || bj < 0) return -1;
    uint64_t k = hkey(level, bi, bj);
    uint64_t h = (k * 0x9E3779B97F4A7C15ull) >> 20;
    int m = g->hcap - 1, at = (int)(h & (uint64_t)m);
    while (g->hkey[at]) {
        if (g->hkey[at] == k) return g->hval[at];
        at = (at + 1) & m;
    }
    return -1;
}

static bool block_alloc_data(Block *B) {
    size_t n = (size_t)S * S;
    B->U = calloc(NV * n, sizeof(double));
    B->U0 = calloc(NV * n, sizeof(double));
    B->F = calloc((size_t)NF * NFX, sizeof(double));
    B->H = calloc((size_t)NF * NFX, sizeof(double));
    B->phi = malloc(n * sizeof(float));
    B->sens = calloc(2 * n, sizeof(double));
    B->kap = malloc(n * sizeof(float));
    B->vol = malloc(n * sizeof(double));
    B->Uh = malloc(NV * n * sizeof(double));
    B->ax = malloc((size_t)NFX * sizeof(double));
    B->ay = malloc((size_t)NFX * sizeof(double));
    B->wall = calloc(2 * n, sizeof(double));
    return B->U && B->U0 && B->F && B->H && B->phi && B->sens && B->kap && B->vol && B->ax && B->ay && B->wall && B->Uh;
}

static void block_free_data(Block *B) {
    free(B->U), free(B->U0), free(B->F), free(B->H), free(B->phi), free(B->sens);
    free(B->kap), free(B->vol), free(B->ax), free(B->ay), free(B->wall), free(B->Uh);
    B->U = B->U0 = B->F = B->H = NULL;
    B->phi = B->kap = NULL;
    B->sens = B->vol = B->ax = B->ay = B->wall = B->Uh = NULL;
}

/* ---- cut cells ------------------------------------------------------------------------------------------------ */

/* the open part of an edge from corner a to corner b (level set pa, pb, linear between): the interval [t0, t1] of
 * the edge's parameter; empty when t1 <= t0 */
static void edge_open(double pa, double pb, double *t0, double *t1) {
    if (pa >= 0 && pb >= 0) *t0 = 0, *t1 = 1;
    else if (pa < 0 && pb < 0) *t0 = *t1 = 0;
    else {
        double t = pa / (pa - pb);
        if (pa >= 0) *t0 = 0, *t1 = t;
        else *t0 = t, *t1 = 1;
    }
}

/* the fluid polygon of a square cell from its corner level sets (linear along the edges): planar area and centroid */
static double fluid_polygon(const double ph[4], const double X[4], const double Y[4], double *cy) {
    double px[8], py[8];
    int n = 0;
    for (int k = 0; k < 4; k++) {
        int m = (k + 1) & 3;
        if (ph[k] >= 0) px[n] = X[k], py[n++] = Y[k];
        if ((ph[k] >= 0) != (ph[m] >= 0)) {
            double t = ph[k] / (ph[k] - ph[m]);
            px[n] = X[k] + t * (X[m] - X[k]), py[n++] = Y[k] + t * (Y[m] - Y[k]);
        }
    }
    double a = 0, sy = 0;
    for (int k = 0; k < n; k++) {
        int m = (k + 1) % n;
        double cr = px[k] * py[m] - px[m] * py[k];
        a += cr;
        sy += (py[k] + py[m]) * cr;
    }
    a *= 0.5;
    *cy = fabs(a) > 0 ? sy / (6 * a) : 0;
    return fabs(a);
}

/* volume fractions of all cells, and for the interior: fluid volumes, open face areas and the wall's area vector
 * (by closure: the open faces and the wall bound the fluid part, so a uniform pressure exerts no net force) */
static void block_geometry(Gas *g, Block *B) {
    const double dx = B->dx, h = dx * dx;
    const bool axi = g->spec.axisymmetric;
    double *pc = malloc((size_t)(S + 1) * (S + 1) * sizeof(double));
    if (!pc) return;
    bool any_cut = false;
    for (int J = 0; J <= S; J++)
        for (int I = 0; I <= S; I++) {
            double v = g->spec.nbodies ? phi_at(g, B->ox + (I - NG) * dx, B->oy + (J - NG) * dx) : INFINITY;
            pc[J * (S + 1) + I] = v;
            if (v < 0) any_cut = true;
        }
#define PC(I, J) pc[(J) * (S + 1) + (I)]
    for (int J = 0; J < S; J++)
        for (int I = 0; I < S; I++) {
            if (!any_cut) {
                B->kap[IDX(I, J)] = 1;
                continue;
            }
            double ph[4] = {PC(I, J), PC(I + 1, J), PC(I + 1, J + 1), PC(I, J + 1)};
            double X[4] = {0, dx, dx, 0}, Y[4] = {0, 0, dx, dx}, cy;
            B->kap[IDX(I, J)] = (float)(fluid_polygon(ph, X, Y, &cy) / h);
        }
    /* x faces: face f of row j at x = ox + f dx, from y = oy + j dx to + dx */
    for (int j = 0; j < NB; j++)
        for (int f = 0; f <= NB; f++) {
            double t0, t1;
            edge_open(PC(f + NG, j + NG), PC(f + NG, j + NG + 1), &t0, &t1);
            double ya = B->oy + (j + t0) * dx, yb = B->oy + (j + t1) * dx;
            B->ax[(size_t)j * (NB + 1) + f] = t1 <= t0 ? 0 : (axi ? 0.5 * (yb * yb - ya * ya) : yb - ya);
        }
    for (int i = 0; i < NB; i++)
        for (int f = 0; f <= NB; f++) {
            double t0, t1;
            edge_open(PC(i + NG, f + NG), PC(i + NG + 1, f + NG), &t0, &t1);
            double yf = B->oy + f * dx;
            B->ay[(size_t)i * (NB + 1) + f] = t1 <= t0 ? 0 : (t1 - t0) * dx * (axi ? fabs(yf) : 1.0);
        }
    /* every cell of the window, ghost cells included (merging reads them): fluid volume and the wall's area vector,
     * from the cell's own four faces (for interior faces the same numbers as ax, ay above) */
    for (int J = 0; J < S; J++)
        for (int I = 0; I < S; I++) {
            double x0 = B->ox + (I - NG) * dx, y0 = B->oy + (J - NG) * dx;
            double ph[4] = {PC(I, J), PC(I + 1, J), PC(I + 1, J + 1), PC(I, J + 1)};
            double X[4] = {x0, x0 + dx, x0 + dx, x0}, Y[4] = {y0, y0, y0 + dx, y0 + dx};
            double cy, area = any_cut ? fluid_polygon(ph, X, Y, &cy) : h;
            if (!any_cut) cy = y0 + 0.5 * dx;
            size_t c = IDX(I, J);
            B->vol[c] = axi ? area * fabs(cy) : area;
            double fa[4]; /* left, right, bottom, top */
            for (int e = 0; e < 4; e++) {
                double t0, t1, pa, pb;
                if (e == 0) pa = ph[0], pb = ph[3];
                else if (e == 1) pa = ph[1], pb = ph[2];
                else if (e == 2) pa = ph[0], pb = ph[1];
                else pa = ph[3], pb = ph[2];
                edge_open(pa, pb, &t0, &t1);
                if (t1 <= t0) fa[e] = 0;
                else if (e < 2) {
                    double ya = y0 + t0 * dx, yb = y0 + t1 * dx;
                    fa[e] = axi ? 0.5 * (yb * yb - ya * ya) : yb - ya;
                } else {
                    double yf = e == 2 ? y0 : y0 + dx;
                    fa[e] = (t1 - t0) * dx * (axi ? fabs(yf) : 1.0);
                }
            }
            B->wall[2 * c] = -(fa[1] - fa[0]);
            B->wall[2 * c + 1] = (axi ? area : 0) - (fa[3] - fa[2]);
            /* round-off in a full cell is not a wall */
            if (fabs(B->wall[2 * c]) + fabs(B->wall[2 * c + 1]) < 1e-12 * dx * (axi ? fabs(cy) + dx : 1)) B->wall[2 * c] = B->wall[2 * c + 1] = 0;
        }
#undef PC
    free(pc);
}

static int new_block(Gas *g, int level, int bi, int bj, int parent) {
    int id;
    if (g->nfree > 0) {
        id = g->free_list[--g->nfree];
    } else {
        if (g->nb == g->cap) {
            int cap = g->cap ? 2 * g->cap : 256;
            Block *nbk = realloc(g->b, (size_t)cap * sizeof *nbk);
            int *fl = realloc(g->free_list, (size_t)cap * sizeof *fl);
            if (!nbk || !fl) return -1;
            g->b = nbk;
            g->free_list = fl;
            g->cap = cap;
        }
        id = g->nb++;
    }
    Block *B = &g->b[id];
    memset(B, 0, sizeof *B);
    B->level = level, B->bi = bi, B->bj = bj, B->parent = parent;
    for (int k = 0; k < 4; k++) B->child[k] = -1;
    B->leaf = B->alive = true;
    B->dx = g->dx0 / (double)(1 << level);
    B->ox = g->spec.x0 + bi * NB * B->dx;
    B->oy = g->spec.y0 + bj * NB * B->dx;
    if (!block_alloc_data(B)) return -1;
    for (int J = 0; J < S; J++)
        for (int I = 0; I < S; I++) B->phi[IDX(I, J)] = (float)phi_at(g, B->ox + (I - NG + 0.5) * B->dx, B->oy + (J - NG + 0.5) * B->dx);
    block_geometry(g, B);
    return id;
}

static void rebuild_leaves(Gas *g) {
    free(g->leaves);
    g->leaves = malloc((size_t)(g->nb ? g->nb : 1) * sizeof(int));
    g->nleaves = 0;
    for (int i = 0; i < g->nb; i++)
        if (g->b[i].alive && g->b[i].leaf) g->leaves[g->nleaves++] = i;
    hash_rebuild(g);
}

static inline double cell_x(const Block *B, int I) { return B->ox + (I - NG + 0.5) * B->dx; }
static inline double cell_y(const Block *B, int J) { return B->oy + (J - NG + 0.5) * B->dx; }

/* the volume weight of a cell: r in an axisymmetric run (per radian, times dx dy), 1 in a planar one */
static inline double rweight(const Gas *g, double y) { return g->spec.axisymmetric ? fabs(y) : 1.0; }

/* ---- ghost cells --------------------------------------------------------------------------------------------- */

static inline double minmod(double a, double b) { return (a * b <= 0) ? 0 : (fabs(a) < fabs(b) ? a : b); }

/* the conserved state of the level-L cell (ci, cj), from whatever leaf holds that region: the leaf itself, the
 * average of the finer cells that cover it, or a limited linear interpolation in the coarser leaf */
static void level_cell(const Gas *g, int L, int ci, int cj, double out[NV]) {
    const size_t n = (size_t)S * S;
    int id = find_block(g, L, ci / NB, cj / NB);
    if (id >= 0 && g->b[id].leaf) {
        const Block *B = &g->b[id];
        size_t c = IDX(ci % NB + NG, cj % NB + NG);
        for (int v = 0; v < NV; v++) out[v] = B->U[v * n + c];
        return;
    }
    if (id >= 0) { /* finer: average the four cells of the child that covers this cell */
        int fi = 2 * ci, fj = 2 * cj;
        int cid = find_block(g, L + 1, fi / NB, fj / NB);
        if (cid >= 0 && g->b[cid].leaf) {
            const Block *C = &g->b[cid];
            double wsum = 0, acc[NV] = {0};
            for (int dj = 0; dj < 2; dj++)
                for (int di = 0; di < 2; di++) {
                    int I = (fi + di) % NB + NG, J = (fj + dj) % NB + NG;
                    double w = rweight(g, cell_y(C, J)) * C->kap[IDX(I, J)];
                    if (!(w > 0)) w = 1e-300; /* a covered cell counts for nothing unless all four are covered */
                    for (int v = 0; v < NV; v++) acc[v] += w * C->U[v * n + IDX(I, J)];
                    wsum += w;
                }
            for (int v = 0; v < NV; v++) out[v] = acc[v] / wsum;
            return;
        }
    }
    /* coarser: the parent level's leaf */
    int pid = find_block(g, L - 1, (ci >> 1) / NB, (cj >> 1) / NB);
    if (pid >= 0 && g->b[pid].leaf) {
        const Block *P = &g->b[pid];
        int I = (ci >> 1) % NB + NG, J = (cj >> 1) % NB + NG;
        double sx = (ci & 1) ? 0.25 : -0.25, sy = (cj & 1) ? 0.25 : -0.25;
        /* slopes from the coarse leaf's interior only (its ghost cells may be written by another thread now):
         * at the leaf's edge the slope is zero */
        bool xl = I > NG, xr = I < NB + NG - 1, yl = J > NG, yr = J < NB + NG - 1;
        for (int v = 0; v < NV; v++) {
            const double *u = P->U + v * n;
            double c = u[IDX(I, J)];
            double gx = (xl && xr) ? minmod(u[IDX(I + 1, J)] - c, c - u[IDX(I - 1, J)]) : 0;
            double gy = (yl && yr) ? minmod(u[IDX(I, J + 1)] - c, c - u[IDX(I, J - 1)]) : 0;
            out[v] = c + sx * gx + sy * gy;
        }
        return;
    }
    /* not reachable with a balanced tree; fall back to the root leaf covering the point */
    double x = g->spec.x0 + (ci + 0.5) * g->dx0 / (1 << L), y = g->spec.y0 + (cj + 0.5) * g->dx0 / (1 << L);
    int r = find_block(g, 0, (int)((x - g->spec.x0) / (NB * g->dx0)), (int)((y - g->spec.y0) / (NB * g->dx0)));
    while (r >= 0 && !g->b[r].leaf) {
        const Block *B = &g->b[r];
        int ix = x >= B->ox + NB * B->dx * 0.5, iy = y >= B->oy + NB * B->dx * 0.5;
        r = B->child[ix + 2 * iy];
    }
    if (r < 0) {
        for (int v = 0; v < NV; v++) out[v] = 0;
        return;
    }
    const Block *B = &g->b[r];
    int I = (int)((x - B->ox) / B->dx) + NG, J = (int)((y - B->oy) / B->dx) + NG;
    if (I < NG) I = NG;
    if (J < NG) J = NG;
    if (I > NB + NG - 1) I = NB + NG - 1;
    if (J > NB + NG - 1) J = NB + NG - 1;
    for (int v = 0; v < NV; v++) out[v] = B->U[v * n + IDX(I, J)];
}

static void fill_ghosts_block(Gas *g, Block *B) {
    const size_t n = (size_t)S * S;
    const GasSpec *s = &g->spec;
    int L = B->level;
    int ncx = s->nbx * NB << L, ncy = s->nby * NB << L; /* cells across the domain at this level */
    int ci0 = B->bi * NB - NG, cj0 = B->bj * NB - NG;
    for (int J = 0; J < S; J++)
        for (int I = 0; I < S; I++) {
            if (I >= NG && I < NB + NG && J >= NG && J < NB + NG) continue;
            int ci = ci0 + I, cj = cj0 + J;
            int flipx = 0, flipy = 0; /* +1: reflect the velocity component; 2: the inflow state */
            if (ci < 0 || ci >= ncx) {
                int side = ci < 0 ? 0 : 1, bc = s->boundary[side];
                if (bc == GAS_INFLOW) flipx = 2;
                else if (bc == GAS_OUTFLOW) ci = ci < 0 ? 0 : ncx - 1;
                else ci = ci < 0 ? -1 - ci : 2 * ncx - 1 - ci, flipx = 1;
            }
            if (cj < 0 || cj >= ncy) {
                int side = cj < 0 ? 2 : 3, bc = s->boundary[side];
                if (bc == GAS_INFLOW) flipy = 2;
                else if (bc == GAS_OUTFLOW) cj = cj < 0 ? 0 : ncy - 1;
                else cj = cj < 0 ? -1 - cj : 2 * ncy - 1 - cj, flipy = 1;
            }
            size_t c = IDX(I, J);
            if (flipx == 2 || flipy == 2) {
                prim_to_cons(&s->inflow, B->U + c, n);
                continue;
            }
            double u[NV];
            level_cell(g, L, ci, cj, u);
            if (flipx == 1) u[1] = -u[1];
            if (flipy == 1) u[2] = -u[2];
            for (int v = 0; v < NV; v++) B->U[v * n + c] = u[v];
        }
}

/* ---- bodies: image points ------------------------------------------------------------------------------------ */

/* the leaf that contains (x, y), or -1 */
static int leaf_at(const Gas *g, double x, double y) {
    double bw = NB * g->dx0;
    int bi = (int)floor((x - g->spec.x0) / bw), bj = (int)floor((y - g->spec.y0) / bw);
    if (bi < 0 || bj < 0 || bi >= g->spec.nbx || bj >= g->spec.nby) return -1;
    int r = find_block(g, 0, bi, bj);
    while (r >= 0 && !g->b[r].leaf) {
        const Block *B = &g->b[r];
        int ix = x >= B->ox + NB * B->dx * 0.5, iy = y >= B->oy + NB * B->dx * 0.5;
        r = B->child[ix + 2 * iy];
    }
    return r;
}

/* bilinear primitive state at (x, y) from fluid cells only; false if no fluid cell is near */
static bool sample_fluid(const Gas *g, double x, double y, double W[5]) {
    const GasSpec *s = &g->spec;
    double ylo = s->y0, yhi = s->y0 + g->ylen;
    /* an image point beyond a symmetry side is folded back */
    if (s->axisymmetric && y < 0) y = -y;
    if (y < ylo) y = ylo + 1e-12;
    if (y > yhi) y = yhi - 1e-12;
    double xlo = s->x0, xhi = s->x0 + s->length;
    if (x < xlo) x = xlo + 1e-12;
    if (x > xhi) x = xhi - 1e-12;
    int id = leaf_at(g, x, y);
    if (id < 0) return false;
    const Block *B = &g->b[id];
    double a = (x - B->ox) / B->dx - 0.5 + NG, b = (y - B->oy) / B->dx - 0.5 + NG;
    int I0 = (int)floor(a), J0 = (int)floor(b);
    double tx = a - I0, ty = b - J0;
    double acc[5] = {0}, wsum = 0;
    for (int dj = 0; dj < 2; dj++)
        for (int di = 0; di < 2; di++) {
            int I = I0 + di, J = J0 + dj;
            if (I < 0 || J < 0 || I >= S || J >= S) continue;
            if (!(B->kap[IDX(I, J)] > 0)) continue;
            double w = (di ? tx : 1 - tx) * (dj ? ty : 1 - ty) + 1e-9;
            double Wc[5];
            cons_to_w(B->U, IDX(I, J), Wc);
            for (int k = 0; k < 5; k++) acc[k] += w * Wc[k];
            wsum += w;
        }
    if (wsum <= 0) {
        /* the nearest fluid cell in a 5 x 5 neighbourhood */
        int Ic = (int)lround(a), Jc = (int)lround(b);
        double best = INFINITY;
        for (int J = Jc - 2; J <= Jc + 2; J++)
            for (int I = Ic - 2; I <= Ic + 2; I++) {
                if (I < 0 || J < 0 || I >= S || J >= S || !(B->kap[IDX(I, J)] > 0)) continue;
                double d = (I - a) * (I - a) + (J - b) * (J - b);
                if (d < best) best = d, cons_to_w(B->U, IDX(I, J), acc), wsum = 1;
            }
        if (wsum <= 0) return false;
    }
    for (int k = 0; k < 5; k++) W[k] = acc[k] / wsum;
    return true;
}

static void fill_bodies_block(Gas *g, Block *B) {
    if (g->spec.nbodies == 0) return;
    const double band = 3.5 * B->dx;
    for (int J = 0; J < S; J++)
        for (int I = 0; I < S; I++) {
            size_t c = IDX(I, J);
            double ph = B->phi[c];
            if (B->kap[c] > 0) continue; /* fluid, cut or not: only covered cells are filled */
            if (ph >= 0) ph = -1e-3 * B->dx;
            double x = cell_x(B, I), y = cell_y(B, J);
            double W[5];
            if (ph < -band) { /* deep inside: a quiet state that keeps reconstruction finite */
                W[0] = g->spec.initial.rho, W[1] = W[2] = 0, W[3] = g->spec.initial.p, W[4] = 1.0 / (g->spec.initial.gamma - 1.0);
                w_to_cons(W, B->U, c);
                continue;
            }
            double h = 0.5 * B->dx;
            double nx = phi_at(g, x + h, y) - phi_at(g, x - h, y), ny = phi_at(g, x, y + h) - phi_at(g, x, y - h);
            double nl = hypot(nx, ny);
            if (nl <= 0) nx = 1, ny = 0, nl = 1;
            nx /= nl, ny /= nl;
            double d = -ph;
            double xi = x + 2 * d * nx, yi = y + 2 * d * ny;
            /* the image point must land in fluid; push it out if the surface is curved */
            for (int k = 0; k < 4 && phi_at(g, xi, yi) < 0.5 * B->dx; k++) xi += 0.5 * B->dx * nx, yi += 0.5 * B->dx * ny;
            if (!sample_fluid(g, xi, yi, W)) {
                W[0] = g->spec.initial.rho, W[1] = W[2] = 0, W[3] = g->spec.initial.p, W[4] = 1.0 / (g->spec.initial.gamma - 1.0);
            } else {
                double un = W[1] * nx + W[2] * ny;
                W[1] -= 2 * un * nx, W[2] -= 2 * un * ny; /* slip wall: the normal velocity reflected */
            }
            w_to_cons(W, B->U, c);
        }
}

/* ---- fluxes -------------------------------------------------------------------------------------------------- */

/* one face: left and right states (rho, un, ut, p, G) in the face frame. Writes the flux of (rho, rho un, rho ut, E),
 * the upwind G and the volume-flux velocity uf (mass flux over upwind density). */
static void riemann(const double *L, const double *R, bool hlle, double f[4], double *Gup, double *uf, double *pstar) {
    double rl = L[0], ul = L[1], vl = L[2], pl = L[3], Gl = L[4];
    double rr = R[0], ur = R[1], vr = R[2], pr = R[3], Gr = R[4];
    double gl = 1 + 1 / Gl, gr = 1 + 1 / Gr;
    double cl = sqrt(gl * pl / rl), cr = sqrt(gr * pr / rr);
    double El = Gl * pl + 0.5 * rl * (ul * ul + vl * vl), Er = Gr * pr + 0.5 * rr * (ur * ur + vr * vr);
    /* Einfeldt wave speed estimates */
    double sl_ = sqrt(rl), sr_ = sqrt(rr), den = sl_ + sr_;
    double ut = (sl_ * ul + sr_ * ur) / den;
    double c2 = (sl_ * cl * cl + sr_ * cr * cr) / den + 0.5 * sl_ * sr_ / (den * den) * (ur - ul) * (ur - ul);
    double ct = sqrt(c2);
    double SL = fmin(ul - cl, ut - ct), SR = fmax(ur + cr, ut + ct);
    if (pstar) { /* the HLLC star pressure, whatever branch the flux takes: the wall pressure of a body face */
        double Ssw = (pr - pl + rl * ul * (SL - ul) - rr * ur * (SR - ur)) / (rl * (SL - ul) - rr * (SR - ur));
        *pstar = pl + rl * (SL - ul) * (Ssw - ul);
    }
    double FL[4] = {rl * ul, rl * ul * ul + pl, rl * ul * vl, ul * (El + pl)};
    double FR[4] = {rr * ur, rr * ur * ur + pr, rr * ur * vr, ur * (Er + pr)};
    if (SL >= 0) {
        memcpy(f, FL, sizeof FL);
        *Gup = Gl, *uf = ul;
        return;
    }
    if (SR <= 0) {
        memcpy(f, FR, sizeof FR);
        *Gup = Gr, *uf = ur;
        return;
    }
    if (hlle) {
        double UL[4] = {rl, rl * ul, rl * vl, El}, UR[4] = {rr, rr * ur, rr * vr, Er};
        for (int k = 0; k < 4; k++) f[k] = (SR * FL[k] - SL * FR[k] + SL * SR * (UR[k] - UL[k])) / (SR - SL);
        if (f[0] >= 0) *Gup = Gl, *uf = f[0] / rl;
        else *Gup = Gr, *uf = f[0] / rr;
        return;
    }
    double Ss = (pr - pl + rl * ul * (SL - ul) - rr * ur * (SR - ur)) / (rl * (SL - ul) - rr * (SR - ur));
    if (Ss >= 0) {
        double chi = (SL - ul) / (SL - Ss);
        double Us[4] = {rl * chi, rl * chi * Ss, rl * chi * vl, rl * chi * (El / rl + (Ss - ul) * (Ss + pl / (rl * (SL - ul))))};
        double UL[4] = {rl, rl * ul, rl * vl, El};
        for (int k = 0; k < 4; k++) f[k] = FL[k] + SL * (Us[k] - UL[k]);
        *Gup = Gl;
        *uf = f[0] / rl;
    } else {
        double chi = (SR - ur) / (SR - Ss);
        double Us[4] = {rr * chi, rr * chi * Ss, rr * chi * vr, rr * chi * (Er / rr + (Ss - ur) * (Ss + pr / (rr * (SR - ur))))};
        double UR[4] = {rr, rr * ur, rr * vr, Er};
        for (int k = 0; k < 4; k++) f[k] = FR[k] + SR * (Us[k] - UR[k]);
        *Gup = Gr;
        *uf = f[0] / rr;
    }
}

static inline double mc_limiter(double a, double b) {
    if (a * b <= 0) return 0;
    double c = 0.5 * (a + b);
    double m = fmin(fabs(c), 2 * fmin(fabs(a), fabs(b)));
    return c > 0 ? m : -m;
}

/* face fluxes of one block; W holds the primitives of all S x S cells */
static void block_fluxes(Gas *g, Block *B, double *W) {
    const size_t n = (size_t)S * S;
    for (size_t c = 0; c < n; c++) {
        double w[5];
        cons_to_w(B->U, c, w);
        for (int k = 0; k < 5; k++) W[k * n + c] = w[k];
    }
    /* pressure-jump sensors in x and in y */
    double *sx = B->sens, *sy = B->sens + n;
    const double *P = W + 3 * n;
    for (int J = 1; J < S - 1; J++)
        for (int I = 1; I < S - 1; I++) {
            size_t c = IDX(I, J);
            double p = P[c];
            double ax = fmax(fabs(P[c + 1] - p) / fmin(P[c + 1], p), fabs(P[c - 1] - p) / fmin(P[c - 1], p));
            double ay = fmax(fabs(P[c + S] - p) / fmin(P[c + S], p), fabs(P[c - S] - p) / fmin(P[c - S], p));
            sx[c] = ax, sy[c] = ay;
        }
    const bool first = g->spec.first_order;
    const double SW = 0.5; /* a pressure jump of 50 per cent across a neighbour: a strong shock */
    for (int dir = 0; dir < 2; dir++) {
        double *Fo = dir == 0 ? B->F : B->H;
        size_t st = dir == 0 ? 1 : S; /* the stride along the face normal */
        for (int a = 0; a < NB; a++)      /* along the face */
            for (int f = 0; f <= NB; f++) { /* the face index along the normal */
                int IL, JL;
                if (dir == 0) IL = f + NG - 1, JL = a + NG;
                else IL = a + NG, JL = f + NG - 1;
                size_t cl = IDX(IL, JL), cr = cl + st;
                size_t fid = dir == 0 ? (size_t)a * (NB + 1) + f : (size_t)a * (NB + 1) + f;
                const double area = (dir == 0 ? B->ax : B->ay)[fid]; /* the open part of the face */
                if (area <= 0) {
                    for (int k = 0; k < NF; k++) Fo[k * NFX + fid] = 0;
                    continue;
                }
                double wl[5], wr[5];
                for (int k = 0; k < 5; k++) {
                    const double *q = W + k * n;
                    double sL = first ? 0 : mc_limiter(q[cl] - q[cl - st], q[cr] - q[cl]);
                    double sR = first ? 0 : mc_limiter(q[cr] - q[cl], q[cr + st] - q[cr]);
                    wl[k] = q[cl] + 0.5 * sL;
                    wr[k] = q[cr] - 0.5 * sR;
                }
                if (wl[0] <= 0 || wl[3] <= 0 || wr[0] <= 0 || wr[3] <= 0 || wl[4] <= 0 || wr[4] <= 0) {
                    for (int k = 0; k < 5; k++) wl[k] = W[k * n + cl], wr[k] = W[k * n + cr];
                }
                /* rotate into the face frame: un along the normal, ut along the face */
                double L[5] = {wl[0], dir == 0 ? wl[1] : wl[2], dir == 0 ? wl[2] : wl[1], wl[3], wl[4]};
                double R[5] = {wr[0], dir == 0 ? wr[1] : wr[2], dir == 0 ? wr[2] : wr[1], wr[3], wr[4]};
                /* a face between fluid and body is a wall: the fluid side against its own reflection across the face.
                 * The Riemann problem is then symmetric, its contact speed is zero, and the mass and energy fluxes vanish
                 * exactly, so the body neither creates nor swallows gas (the ghost cells still give the reconstruction
                 * next to the wall its smooth neighbours) */
                const double *tr = dir == 0 ? sy : sx; /* a jump across the face's own direction */
                bool hlle = fmax(tr[cl], tr[cr]) > SW;
                double fl[4], Gup, uf;
                riemann(L, R, hlle, fl, &Gup, &uf, NULL);
                Fo[0 * NFX + fid] = area * fl[0];
                Fo[1 * NFX + fid] = area * (dir == 0 ? fl[1] : fl[2]);
                Fo[2 * NFX + fid] = area * (dir == 0 ? fl[2] : fl[1]);
                Fo[3 * NFX + fid] = area * fl[3];
                Fo[4 * NFX + fid] = area * Gup * uf;
                Fo[5 * NFX + fid] = area * uf;
            }
    }
}

/* a coarse face next to finer cells takes the sum of the fine fluxes, so what leaves one side enters the other */
static void reflux_block(Gas *g, Block *B) {
    for (int side = 0; side < 4; side++) {
        int di = side == 0 ? -1 : side == 1 ? 1 : 0, dj = side == 2 ? -1 : side == 3 ? 1 : 0;
        int nid = find_block(g, B->level, B->bi + di, B->bj + dj);
        if (nid < 0 || g->b[nid].leaf) continue;
        const Block *N = &g->b[nid];
        /* the two children of N that touch B */
        for (int half = 0; half < 2; half++) {
            int cx, cy;
            if (side <= 1) cx = side == 0 ? 1 : 0, cy = half;
            else cx = half, cy = side == 2 ? 1 : 0;
            int cid = N->child[cx + 2 * cy];
            if (cid < 0 || !g->b[cid].leaf) continue;
            const Block *C = &g->b[cid];
            for (int k = 0; k < NB / 2; k++) { /* coarse face k along B's side within this half */
                int a = half * (NB / 2) + k;    /* coarse index along the side */
                int fa = 2 * k;                 /* fine index along the child's side */
                for (int v = 0; v < NF; v++) {
                    double sum;
                    size_t fidC;
                    if (side <= 1) {
                        int ff = side == 0 ? NB : 0; /* the child's face on the shared side */
                        sum = C->F[v * NFX + (size_t)fa * (NB + 1) + ff] + C->F[v * NFX + (size_t)(fa + 1) * (NB + 1) + ff];
                        fidC = (size_t)a * (NB + 1) + (side == 0 ? 0 : NB);
                        B->F[v * NFX + fidC] = sum;
                    } else {
                        int ff = side == 2 ? NB : 0;
                        sum = C->H[v * NFX + (size_t)fa * (NB + 1) + ff] + C->H[v * NFX + (size_t)(fa + 1) * (NB + 1) + ff];
                        fidC = (size_t)a * (NB + 1) + (side == 2 ? 0 : NB);
                        B->H[v * NFX + fidC] = sum;
                    }
                }
            }
        }
    }
}

/* the pressure on a wall that a gas of state (rho, p, gamma) meets with normal velocity un (positive into the wall):
 * the exact solution of the Riemann problem between the state and its reflection, f(p*) = un with Toro's pressure
 * function (a shock if un > 0, a rarefaction if un < 0, vacuum when the gas leaves faster than it can expand) */
static double wall_pressure(double rho, double p, double gamma, double un) {
    double c = sqrt(gamma * p / rho);
    if (un <= -2 * c / (gamma - 1)) return 0;
    double A = 2 / ((gamma + 1) * rho), Bc = (gamma - 1) / (gamma + 1) * p;
    double ps = un > 0 ? p + rho * c * un : p * pow(1 + 0.5 * (gamma - 1) * un / c, 2 * gamma / (gamma - 1));
    for (int it = 0; it < 30; it++) {
        double f, fd;
        if (ps > p) {
            double sq = sqrt(A / (ps + Bc));
            f = (ps - p) * sq, fd = sq * (1 - 0.5 * (ps - p) / (Bc + ps));
        } else {
            double q = ps / p;
            f = 2 * c / (gamma - 1) * (pow(q, (gamma - 1) / (2 * gamma)) - 1), fd = 1 / (rho * c) * pow(q, -(gamma + 1) / (2 * gamma));
        }
        double d = (f - un) / fd;
        ps -= d;
        if (ps < 1e-12 * p) ps = 1e-12 * p;
        if (fabs(d) < 1e-12 * ps) break;
    }
    return ps;
}

/* One stage, in three passes. Provisional: fluxes through the open faces, the wall pressure on the cut face and the
 * axisymmetric pressure on the side faces give every fluid cell its new state with the full time step, which a small
 * cut cell alone could not bear. Exchange: the provisional states of the ghost cells are copied from the neighbouring
 * blocks of the same level. Merge (Quirk 1994): every cell of less than half a volume joins one large neighbour away
 * from the wall, and each group takes the volume-weighted mean of its provisional states, which is exactly the update
 * of the merged volume, because the fluxes between members cancel. Groups are stars around their large cell, so a
 * window of three ghost layers holds every member of a group that touches the block and every member's neighbours:
 * the blocks on both sides of an edge find the same groups and the same means, and mass is conserved across them.
 * Tried first and rejected, on record: flux redistribution (Chern and Colella 1987) drove a sliver at a body's corner
 * to negative density; state redistribution with overlapping neighbourhoods (Berger and Giuliani 2021, first-order
 * form) and merging confined to one block both failed in the first step of an impulsive Mach 3 start behind a
 * cylinder, where the slivers lay in the last column of their block. */
static void block_provisional(Gas *g, Block *B, double dt) {
    const size_t n = (size_t)S * S;
    const double dx = B->dx;
    const bool axi = g->spec.axisymmetric;
    for (int j = 0; j < NB; j++)
        for (int i = 0; i < NB; i++) {
            size_t c = IDX(i + NG, j + NG);
            double vol = B->vol[c];
            if (!(vol > 0)) {
                for (int v = 0; v < NV; v++) B->Uh[v * n + c] = B->U[v * n + c];
                continue;
            }
            size_t fx0 = (size_t)j * (NB + 1) + i, fx1 = fx0 + 1;
            size_t fy0 = (size_t)i * (NB + 1) + j, fy1 = fy0 + 1;
            double d[NF];
            for (int v = 0; v < NF; v++) d[v] = (B->F[v * NFX + fx1] - B->F[v * NFX + fx0]) + (B->H[v * NFX + fy1] - B->H[v * NFX + fy0]);
            double W[5];
            cons_to_w(B->U, c, W);
            double wx = B->wall[2 * c], wr = B->wall[2 * c + 1];
            double wa = hypot(wx, wr);
            if (wa > 0) {
                double un = (W[1] * wx + W[2] * wr) / wa;
                double pw = wall_pressure(W[0], W[3], 1 + 1 / W[4], un);
                d[1] += pw * wx, d[2] += pw * wr;
            }
            for (int v = 0; v < 4; v++) B->Uh[v * n + c] = B->U[v * n + c] - dt * d[v] / vol;
            B->Uh[4 * n + c] = W[4] - dt * (d[4] - W[4] * d[5]) / vol;
            if (axi) B->Uh[2 * n + c] += dt * W[3] * B->kap[c] * dx * dx / vol; /* the pressure on the side faces */
        }
}

/* the provisional states of the ghost cells, from the neighbours of the same level (bodies lie at the finest level,
 * so merging never crosses a change of level); NaN where there is none */
static void block_uh_ghost(Gas *g, Block *B) {
    const size_t n = (size_t)S * S;
    int ci0 = B->bi * NB - NG, cj0 = B->bj * NB - NG;
    for (int J = 0; J < S; J++)
        for (int I = 0; I < S; I++) {
            if (I >= NG && I < NB + NG && J >= NG && J < NB + NG) continue;
            size_t c = IDX(I, J);
            int ci = ci0 + I, cj = cj0 + J;
            int id = (ci < 0 || cj < 0) ? -1 : find_block(g, B->level, ci / NB, cj / NB);
            if (id < 0 || !g->b[id].leaf) {
                for (int v = 0; v < NV; v++) B->Uh[v * n + c] = NAN;
                continue;
            }
            const Block *N = &g->b[id];
            size_t cn = IDX(ci % NB + NG, cj % NB + NG);
            for (int v = 0; v < NV; v++) B->Uh[v * n + c] = N->Uh[v * n + cn];
        }
}

static inline bool is_small(const Gas *g, const Block *B, int I, int J) {
    double v = B->vol[IDX(I, J)];
    double full = (g->spec.axisymmetric ? fabs(cell_y(B, J)) : 1.0) * B->dx * B->dx;
    return v > 0 && v < 0.5 * full;
}

static inline bool is_large(const Gas *g, const Block *B, int I, int J) {
    if (I < 0 || J < 0 || I >= S || J >= S) return false;
    size_t c = IDX(I, J);
    double v = B->vol[c];
    double full = (g->spec.axisymmetric ? fabs(cell_y(B, J)) : 1.0) * B->dx * B->dx;
    return v >= 0.5 * full && !isnan(B->Uh[c]);
}

/* the large neighbour a small cell joins, away from its wall; -1 if none (it then stays alone) */
static int merge_target(const Gas *g, const Block *B, int I, int J) {
    size_t c = IDX(I, J);
    double mx = -B->wall[2 * c], my = -B->wall[2 * c + 1];
    int sx = mx > 0 ? 1 : -1, sy = my > 0 ? 1 : -1;
    int cand[11][2], nc = 0;
    if (fabs(my) < 0.4142 * fabs(mx)) cand[nc][0] = sx, cand[nc++][1] = 0;
    else if (fabs(mx) < 0.4142 * fabs(my)) cand[nc][0] = 0, cand[nc++][1] = sy;
    else cand[nc][0] = sx, cand[nc++][1] = sy, cand[nc][0] = sx, cand[nc++][1] = 0, cand[nc][0] = 0, cand[nc++][1] = sy;
    static const int R8[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    for (int k = 0; k < 8; k++) cand[nc][0] = R8[k][0], cand[nc++][1] = R8[k][1];
    for (int k = 0; k < nc; k++)
        if (is_large(g, B, I + cand[k][0], J + cand[k][1])) return IDX(I + cand[k][0], J + cand[k][1]);
    return -1;
}

static void block_merge(Gas *g, Block *B, int stage) {
    const size_t n = (size_t)S * S;
    bool any = false;
    for (int J = 1; J < S - 1 && !any; J++)
        for (int I = 1; I < S - 1 && !any; I++) any = B->wall[2 * IDX(I, J)] != 0 || B->wall[2 * IDX(I, J) + 1] != 0;
    for (int j = 0; j < NB; j++)
        for (int i = 0; i < NB; i++) {
            int I = i + NG, J = j + NG;
            size_t c = IDX(I, J);
            if (!(B->vol[c] > 0)) continue;
            double Un[NV];
            for (int v = 0; v < NV; v++) Un[v] = B->Uh[v * n + c];
            if (any) {
                int root = is_small(g, B, I, J) ? merge_target(g, B, I, J) : (int)c;
                if (root >= 0) {
                    int RI = root % S, RJ = root / S;
                    double vs = B->vol[root], acc[NV];
                    for (int v = 0; v < NV; v++) acc[v] = B->vol[root] * B->Uh[v * n + root];
                    int members = 1;
                    for (int dj = -1; dj <= 1; dj++)
                        for (int di = -1; di <= 1; di++) {
                            int SI = RI + di, SJ = RJ + dj;
                            if ((!di && !dj) || SI < 1 || SJ < 1 || SI >= S - 1 || SJ >= S - 1) continue;
                            if (!is_small(g, B, SI, SJ) || merge_target(g, B, SI, SJ) != root) continue;
                            size_t sc = IDX(SI, SJ);
                            if (isnan(B->Uh[sc])) continue;
                            vs += B->vol[sc];
                            for (int v = 0; v < NV; v++) acc[v] += B->vol[sc] * B->Uh[v * n + sc];
                            members++;
                        }
                    if (members > 1)
                        for (int v = 0; v < NV; v++) Un[v] = acc[v] / vs;
                }
            }
            if (stage == 1)
                for (int v = 0; v < NV; v++) Un[v] = 0.5 * (B->U0[v * n + c] + Un[v]);
            /* floors: a state that lost positivity is held, not propagated as NaN */
            if (!(Un[0] > 1e-12)) Un[0] = 1e-12;
            if (!(Un[4] > 0.1)) Un[4] = 0.1;
            double ke = 0.5 * (Un[1] * Un[1] + Un[2] * Un[2]) / Un[0];
            if (!(Un[3] - ke > 1e-12 * Un[4])) Un[3] = ke + 1e-9 * Un[4];
            for (int v = 0; v < NV; v++) B->U[v * n + c] = Un[v];
        }
}

/* ---- parallel passes ----------------------------------------------------------------------------------------- */

enum { PASS_GHOST, PASS_BODY, PASS_FLUX, PASS_REFLUX, PASS_PROVISIONAL, PASS_UH_GHOST, PASS_MERGE, PASS_SAVE, PASS_DT };
typedef struct PassCtx {
    Gas *g;
    int pass;
    double *W; /* per thread scratch, 5 * S * S each */
} PassCtx;

static void pass_fn(void *ctx, int begin, int end, int tid) {
    PassCtx *pc = ctx;
    Gas *g = pc->g;
    const size_t n = (size_t)S * S;
    for (int k = begin; k < end; k++) {
        Block *B = &g->b[g->leaves[k]];
        switch (pc->pass) {
        case PASS_GHOST: fill_ghosts_block(g, B); break;
        case PASS_BODY: fill_bodies_block(g, B); break;
        case PASS_FLUX: block_fluxes(g, B, pc->W + (size_t)tid * 5 * n); break;
        case PASS_REFLUX: reflux_block(g, B); break;
        case PASS_PROVISIONAL: block_provisional(g, B, g->dt); break;
        case PASS_UH_GHOST: block_uh_ghost(g, B); break;
        case PASS_MERGE: block_merge(g, B, g->stage); break;
        case PASS_SAVE: memcpy(B->U0, B->U, NV * n * sizeof(double)); break;
        case PASS_DT: {
            double m = 0;
            for (int j = NG; j < NB + NG; j++)
                for (int i = NG; i < NB + NG; i++) {
                    size_t c = IDX(i, j);
                    if (!(B->kap[c] > 0)) continue;
                    double W[5];
                    cons_to_w(B->U, c, W);
                    double cs = sqrt((1 + 1 / W[4]) * W[3] / W[0]);
                    double r = (fabs(W[1]) + cs + fabs(W[2]) + cs) / B->dx;
                    if (r > m) m = r;
                }
            if (m > g->dtmin[tid]) g->dtmin[tid] = m;
            break;
        }
        }
    }
}

static void run_pass(Gas *g, int pass, double *W) {
    PassCtx pc = {g, pass, W};
    pool_for(g->pool, g->nleaves, 4, pass_fn, &pc);
}

/* ---- refinement ---------------------------------------------------------------------------------------------- */

static double loehner(const double *q, size_t c, size_t st) {
    double a = q[c + st], b = q[c], d = q[c - st];
    double num = fabs(a - 2 * b + d);
    double den = fabs(a - b) + fabs(b - d) + 0.01 * (fabs(a) + 2 * fabs(b) + fabs(d));
    return den > 0 ? num / den : 0;
}

static void mark_block(Gas *g, Block *B, double *W) {
    const size_t n = (size_t)S * S;
    double emax = 0, pmin_abs = INFINITY;
    for (int J = 0; J < S; J++)
        for (int I = 0; I < S; I++) {
            double w[5];
            cons_to_w(B->U, IDX(I, J), w);
            W[IDX(I, J)] = w[0];
            W[n + IDX(I, J)] = w[3];
        }
    bool body_near = false;
    double jmax = 0; /* the largest relative jump between neighbouring cells */
    for (int J = NG; J < NB + NG; J++)
        for (int I = NG; I < NB + NG; I++) {
            size_t c = IDX(I, J);
            pmin_abs = fmin(pmin_abs, fabs(B->phi[c]));
            if (!(B->kap[c] > 0)) continue;
            for (int q = 0; q < 2; q++) {
                const double *f = W + q * n;
                emax = fmax(emax, loehner(f, c, 1));
                emax = fmax(emax, loehner(f, c, S));
                /* both neighbours in each direction, as the indicator sees them: a feature entering the block across
                 * its lower or left edge shows only against the ghost cell behind */
                static const int OFF[4] = {1, -1, S, -S};
                for (int k = 0; k < 4; k++)
                    if (B->kap[c + OFF[k]] > 0) jmax = fmax(jmax, fabs(f[c + OFF[k]] - f[c]) / fmin(fabs(f[c + OFF[k]]), fabs(f[c])));
            }
        }
    if (g->spec.nbodies > 0 && pmin_abs < 3 * B->dx) body_near = true;
    B->flag = 0;
    int L = B->level;
    /* a feature: the Loehner indicator is high and the jump is real (a smooth expansion at a coarse level has a high
     * normalised second difference but small jumps) */
    bool feature = emax > g->spec.refine_above && jmax > g->spec.refine_jump;
    bool smooth = emax < g->spec.coarsen_below || (g->spec.refine_jump > 0 && jmax < 0.5 * g->spec.refine_jump);
    if (L < g->spec.max_level && (feature || (body_near && L < g->spec.body_level))) B->flag = 1;
    else if (L > 0 && smooth && !(body_near && L <= g->spec.body_level)) B->flag = -1;
}

static void refine_block(Gas *g, int id) {
    const size_t n = (size_t)S * S;
    int L = g->b[id].level, bi = g->b[id].bi, bj = g->b[id].bj;
    for (int q = 0; q < 4; q++) {
        int cx = q & 1, cy = q >> 1;
        int cid = new_block(g, L + 1, 2 * bi + cx, 2 * bj + cy, id);
        Block *P = &g->b[id]; /* new_block may move the array */
        if (cid < 0) continue;
        P->child[q] = cid;
        Block *C = &g->b[cid];
        C->flag = 0;
        for (int j = 0; j < NB; j++)
            for (int i = 0; i < NB; i++) {
                int pi = cx * (NB / 2) + (i >> 1) + NG, pj = cy * (NB / 2) + (j >> 1) + NG;
                double sx = (i & 1) ? 0.25 : -0.25, sy = (j & 1) ? 0.25 : -0.25;
                for (int v = 0; v < NV; v++) {
                    const double *u = P->U + v * n;
                    double c = u[IDX(pi, pj)];
                    double gx = minmod(u[IDX(pi + 1, pj)] - c, c - u[IDX(pi - 1, pj)]);
                    double gy = minmod(u[IDX(pi, pj + 1)] - c, c - u[IDX(pi, pj - 1)]);
                    /* axisymmetric: the children's volumes grow with r, so the radial slope would add r h gy / 4 of
                     * the quantity to the four children; that amount is taken back (conservative prolongation) */
                    double corr = 0;
                    if (g->spec.axisymmetric) {
                        double r = fabs(cell_y(P, pj));
                        if (r > 0) corr = 0.25 * (0.25 * P->dx) * gy / r * (cell_y(P, pj) >= 0 ? 1 : -1);
                    }
                    C->U[v * n + IDX(i + NG, j + NG)] = c + sx * gx + sy * gy - corr;
                }
            }
    }
    Block *P = &g->b[id];
    P->leaf = false;
    block_free_data(P);
}

static void coarsen_block(Gas *g, int id) {
    const size_t n = (size_t)S * S;
    Block *P = &g->b[id];
    if (!block_alloc_data(P)) return;
    for (int J = 0; J < S; J++)
        for (int I = 0; I < S; I++) P->phi[IDX(I, J)] = (float)phi_at(g, cell_x(P, I), cell_y(P, J));
    block_geometry(g, P);
    for (int q = 0; q < 4; q++) {
        int cx = q & 1, cy = q >> 1;
        Block *C = &g->b[P->child[q]];
        for (int j = 0; j < NB / 2; j++)
            for (int i = 0; i < NB / 2; i++) {
                double acc[NV] = {0}, ws = 0;
                for (int dj = 0; dj < 2; dj++)
                    for (int di = 0; di < 2; di++) {
                        int I = 2 * i + di + NG, J = 2 * j + dj + NG;
                        double w = C->vol[IDX(I, J)];
                        if (!(w > 0)) w = 1e-300;
                        for (int v = 0; v < NV; v++) acc[v] += w * C->U[v * n + IDX(I, J)];
                        ws += w;
                    }
                size_t c = IDX(cx * (NB / 2) + i + NG, cy * (NB / 2) + j + NG);
                for (int v = 0; v < NV; v++) P->U[v * n + c] = acc[v] / ws;
            }
        block_free_data(C);
        C->alive = false;
        g->free_list[g->nfree++] = P->child[q];
        P->child[q] = -1;
    }
    P->leaf = true;
    P->flag = 0;
}

/* the leaf covering the level-L block position (bi, bj), or -1 when that region is finer than L */
static int covering_leaf(const Gas *g, int L, int bi, int bj) {
    for (int l = L; l >= 0; l--) {
        int id = find_block(g, l, bi >> (L - l), bj >> (L - l));
        if (id >= 0) return g->b[id].leaf ? id : (l == L ? -2 : -1);
    }
    return -1;
}

typedef struct MarkCtx {
    Gas *g;
    double *W;
} MarkCtx;

static void mark_fn(void *ctx, int begin, int end, int tid) {
    MarkCtx *m = ctx;
    for (int k = begin; k < end; k++) mark_block(m->g, &m->g->b[m->g->leaves[k]], m->W + (size_t)tid * 2 * S * S);
}

static void regrid_once(Gas *g, bool mark) {
    const GasSpec *s = &g->spec;
    if (mark) {
        run_pass(g, PASS_GHOST, NULL);
        double *W = malloc((size_t)g->nthreads * 2 * S * S * sizeof(double));
        MarkCtx mc = {g, W};
        pool_for(g->pool, g->nleaves, 4, mark_fn, &mc);
        free(W);
    }
    /* 2:1 balance for refinement: a leaf to be refined forces its coarser neighbours to be refined */
    for (bool changed = true; changed;) {
        changed = false;
        for (int k = 0; k < g->nleaves; k++) {
            Block *B = &g->b[g->leaves[k]];
            if (B->flag != 1) continue;
            for (int dj = -1; dj <= 1; dj++)
                for (int di = -1; di <= 1; di++) {
                    int nbi = B->bi + di, nbj = B->bj + dj;
                    if (nbi < 0 || nbj < 0 || nbi >= s->nbx << B->level || nbj >= s->nby << B->level) continue;
                    int id = covering_leaf(g, B->level, nbi, nbj);
                    if (id >= 0 && g->b[id].level < B->level && g->b[id].flag != 1) g->b[id].flag = 1, changed = true;
                }
        }
    }
    /* coarsening: all four siblings agree, none of the parent's neighbours is finer than the children */
    for (int k = 0; k < g->nleaves; k++) {
        Block *B = &g->b[g->leaves[k]];
        if (B->flag != -1 || B->parent < 0) continue;
        Block *P = &g->b[B->parent];
        bool ok = true;
        for (int q = 0; q < 4 && ok; q++) {
            int c = P->child[q];
            if (c < 0 || !g->b[c].leaf || g->b[c].flag != -1) ok = false;
        }
        for (int dj = -1; dj <= 1 && ok; dj++)
            for (int di = -1; di <= 1 && ok; di++) {
                if (!di && !dj) continue;
                int id = find_block(g, P->level, P->bi + di, P->bj + dj);
                if (id < 0 || g->b[id].leaf) continue;
                for (int q = 0; q < 4; q++) {
                    int c = g->b[id].child[q];
                    if (c >= 0 && (!g->b[c].leaf || g->b[c].flag == 1)) ok = false;
                }
            }
        if (!ok)
            for (int q = 0; q < 4; q++)
                if (P->child[q] >= 0 && g->b[P->child[q]].flag == -1) g->b[P->child[q]].flag = 0;
        if (ok)
            for (int q = 0; q < 4; q++) g->b[P->child[q]].flag = -2; /* agreed */
    }
    int nleaves = g->nleaves;
    int *leaves = malloc((size_t)nleaves * sizeof(int));
    memcpy(leaves, g->leaves, (size_t)nleaves * sizeof(int));
    for (int k = 0; k < nleaves; k++) {
        int id = leaves[k];
        if (!g->b[id].alive || !g->b[id].leaf) continue;
        if (g->b[id].flag == 1) refine_block(g, id);
    }
    for (int k = 0; k < nleaves; k++) {
        int id = leaves[k];
        if (!g->b[id].alive || !g->b[id].leaf || g->b[id].flag != -2) continue;
        int pid = g->b[id].parent;
        if (pid >= 0 && !g->b[pid].leaf) coarsen_block(g, pid);
    }
    free(leaves);
    for (int i = 0; i < g->nb; i++) g->b[i].flag = 0;
    rebuild_leaves(g);
}

void gas_regrid(Gas *g) { regrid_once(g, true); }

/* ---- creation ------------------------------------------------------------------------------------------------ */

void gas_spec_defaults(GasSpec *s) {
    memset(s, 0, sizeof *s);
    s->length = 1;
    s->nbx = 4, s->nby = 2;
    s->max_level = 3;
    s->body_level = -1;
    s->cfl = 0.4;
    s->regrid_every = 4;
    s->refine_above = 0.8, s->coarsen_below = 0.2, s->refine_jump = 0.0;
    GasPrim air = {1.225, 0, 0, 101325, 1.4};
    s->initial = s->inflow = air;
    s->gas_constant = 287.05;
}

static void set_initial(Gas *g, Block *B) {
    const size_t n = (size_t)S * S;
    for (int J = 0; J < S; J++)
        for (int I = 0; I < S; I++) {
            GasPrim w = initial_at(&g->spec, cell_x(B, I), cell_y(B, J));
            prim_to_cons(&w, B->U + IDX(I, J), n);
        }
}

Gas *gas_create(const GasSpec *spec, char *err, size_t errlen) {
    const GasSpec *s = spec;
    if (s->nbx < 1 || s->nby < 1 || s->length <= 0 || s->max_level < 0 || s->max_level > 12 || s->cfl <= 0 || s->cfl > 0.9) {
        snprintf(err, errlen, "gas: invalid grid (root blocks, length, max_level 0..12, cfl 0..0.9)");
        return NULL;
    }
    if (s->axisymmetric && fabs(s->y0) > 0) {
        snprintf(err, errlen, "gas: an axisymmetric run starts at the axis, y0 = 0");
        return NULL;
    }
    Gas *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->spec = *s;
    if (g->spec.body_level < 0 || g->spec.body_level > g->spec.max_level) g->spec.body_level = g->spec.max_level;
    if (g->spec.regrid_every < 1) g->spec.regrid_every = 4;
    for (int k = 0; k < s->nbodies; k++) {
        GasBody *bd = &g->spec.bodies[k];
        if (bd->kind == GAS_BODY_POLYGON) {
            if (bd->npts < 3 || !s->bodies[k].pts) {
                snprintf(err, errlen, "gas: polygon body %d needs at least 3 points", k);
                free(g);
                return NULL;
            }
            bd->pts = malloc((size_t)bd->npts * 2 * sizeof(double));
            memcpy(bd->pts, s->bodies[k].pts, (size_t)bd->npts * 2 * sizeof(double));
        }
    }
    g->dx0 = s->length / (s->nbx * NB);
    g->ylen = g->dx0 * NB * s->nby;
    g->nthreads = s->threads > 0 ? s->threads : cpu_perf_count();
    if (g->nthreads < 1) g->nthreads = 1;
    g->pool = pool_create(g->nthreads);
    g->nthreads = pool_size(g->pool);
    g->dtmin = calloc((size_t)g->nthreads, sizeof(double));
    for (int bj = 0; bj < s->nby; bj++)
        for (int bi = 0; bi < s->nbx; bi++) {
            int id = new_block(g, 0, bi, bj, -1);
            if (id < 0) {
                snprintf(err, errlen, "gas: out of memory");
                gas_free(g);
                return NULL;
            }
            set_initial(g, &g->b[id]);
        }
    rebuild_leaves(g);
    /* initial refinement: mark on the exact initial state, refine, set the initial state on the new blocks again */
    for (int l = 0; l < g->spec.max_level; l++) {
        regrid_once(g, true);
        for (int k = 0; k < g->nleaves; k++) set_initial(g, &g->b[g->leaves[k]]);
    }
    return g;
}

void gas_free(Gas *g) {
    if (!g) return;
    for (int i = 0; i < g->nb; i++)
        if (g->b[i].alive) block_free_data(&g->b[i]);
    for (int k = 0; k < g->spec.nbodies; k++) free(g->spec.bodies[k].pts);
    free(g->b), free(g->free_list), free(g->leaves), free(g->hkey), free(g->hval), free(g->dtmin);
    if (g->pool) pool_destroy(g->pool);
    free(g);
}

/* ---- the step ------------------------------------------------------------------------------------------------ */

double gas_step(Gas *g, double t_stop) {
    if (g->steps > 0 && g->steps % g->spec.regrid_every == 0 && g->spec.max_level > 0) regrid_once(g, true);
    double *W = malloc((size_t)g->nthreads * 5 * S * S * sizeof(double));
    if (!W) return 0;
    run_pass(g, PASS_GHOST, W);
    run_pass(g, PASS_BODY, W);
    for (int i = 0; i < g->nthreads; i++) g->dtmin[i] = 0;
    run_pass(g, PASS_DT, W);
    double rate = 0;
    for (int i = 0; i < g->nthreads; i++) rate = fmax(rate, g->dtmin[i]);
    if (!(rate > 0) || !isfinite(rate)) {
        free(W);
        return 0;
    }
    double dt = g->spec.cfl / rate;
    if (t_stop > 0 && g->t + dt > t_stop) dt = t_stop - g->t;
    if (dt <= 0) {
        free(W);
        return 0;
    }
    g->dt = dt;
    run_pass(g, PASS_SAVE, W);
    for (int stage = 0; stage < 2; stage++) {
        g->stage = stage;
        if (stage == 1) {
            run_pass(g, PASS_GHOST, W);
            run_pass(g, PASS_BODY, W);
        }
        run_pass(g, PASS_FLUX, W);
        run_pass(g, PASS_REFLUX, W);
        run_pass(g, PASS_PROVISIONAL, W);
        run_pass(g, PASS_UH_GHOST, W);
        run_pass(g, PASS_MERGE, W);
    }
    free(W);
    g->t += dt;
    g->steps++;
    g->dt_last = dt;
    return dt;
}

double gas_time(const Gas *g) { return g->t; }
int gas_steps(const Gas *g) { return g->steps; }

void gas_stats(const Gas *g, GasStats *st) {
    const size_t n = (size_t)S * S;
    memset(st, 0, sizeof *st);
    st->min_rho = st->min_p = INFINITY;
    st->leaves = g->nleaves;
    int maxl = 0;
    for (int k = 0; k < g->nleaves; k++) {
        const Block *B = &g->b[g->leaves[k]];
        maxl = B->level > maxl ? B->level : maxl;
        for (int j = NG; j < NB + NG; j++)
            for (int i = NG; i < NB + NG; i++) {
                size_t c = IDX(i, j);
                double vol = B->vol[c];
                if (!(vol > 0)) continue;
                st->mass += vol * B->U[c];
                st->xmom += vol * B->U[n + c];
                st->energy += vol * B->U[3 * n + c];
                double W[5];
                cons_to_w(B->U, c, W);
                st->min_rho = fmin(st->min_rho, W[0]);
                st->min_p = fmin(st->min_p, W[3]);
            }
    }
    st->cells = (long)g->nleaves * NB * NB;
    st->levels_used = maxl + 1;
}

bool gas_sample(const Gas *g, double x, double y, GasPrim *out) {
    double yy = g->spec.axisymmetric ? fabs(y) : y;
    int id = leaf_at(g, x, yy);
    if (id < 0) return false;
    const Block *B = &g->b[id];
    int I = (int)floor((x - B->ox) / B->dx) + NG, J = (int)floor((yy - B->oy) / B->dx) + NG;
    if (!(B->kap[IDX(I, J)] > 0)) return false;
    double W[5];
    if (!sample_fluid(g, x, yy, W)) return false;
    out->rho = W[0], out->u = W[1], out->v = g->spec.axisymmetric && y < 0 ? -W[2] : W[2], out->p = W[3], out->gamma = 1 + 1 / W[4];
    return true;
}

double gas_cell_size(const Gas *g, double x, double y) {
    int id = leaf_at(g, x, g->spec.axisymmetric ? fabs(y) : y);
    return id < 0 ? 0 : g->b[id].dx;
}

/* ---- output -------------------------------------------------------------------------------------------------- */

static bool want(const char *list, const char *name) {
    size_t l = strlen(name);
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += l)
        if ((p == list || p[-1] == ',') && (p[l] == 0 || p[l] == ',')) return true;
    return false;
}

void gas_write_frame(Gas *g, LabWriter *w, const char *fields) {
    const char *fl = fields ? fields : "rho,p,mach,solid";
    const size_t n = (size_t)S * S;
    /* ghost cells are needed for the vorticity */
    if (want(fl, "vorticity")) {
        run_pass(g, PASS_GHOST, NULL);
        run_pass(g, PASS_BODY, NULL);
    }
    int nl = g->nleaves;
    LabBlock *bl = malloc((size_t)nl * sizeof *bl);
    size_t nc = (size_t)nl * NB * NB;
    float *buf = malloc(nc * sizeof(float));
    if (!bl || !buf) {
        free(bl);
        free(buf);
        return;
    }
    for (int k = 0; k < nl; k++) {
        const Block *B = &g->b[g->leaves[k]];
        LabBlock *L = &bl[k];
        L->n[0] = L->n[1] = NB, L->n[2] = 1;
        L->level = B->level;
        L->plane = LAB_PLANE_XY;
        L->origin[0] = B->ox, L->origin[1] = B->oy, L->origin[2] = 0;
        L->dx[0] = L->dx[1] = B->dx, L->dx[2] = B->dx;
    }
    lab_part_blocks(w, "gas", nl, bl);
    static const char *NAMES[] = {"rho", "p", "u", "v", "mach", "T", "gamma", "vorticity", "solid"};
    for (int f = 0; f < 9; f++) {
        if (!want(fl, NAMES[f])) continue;
        if (f == 5 && g->spec.gas_constant <= 0) continue;
        size_t at = 0;
        for (int k = 0; k < nl; k++) {
            const Block *B = &g->b[g->leaves[k]];
            for (int j = NG; j < NB + NG; j++)
                for (int i = NG; i < NB + NG; i++) {
                    size_t c = IDX(i, j);
                    double W[5];
                    cons_to_w(B->U, c, W);
                    double v = 0;
                    switch (f) {
                    case 0: v = W[0]; break;
                    case 1: v = W[3]; break;
                    case 2: v = W[1]; break;
                    case 3: v = W[2]; break;
                    case 4: v = hypot(W[1], W[2]) / sqrt((1 + 1 / W[4]) * W[3] / W[0]); break;
                    case 5: v = W[3] / (W[0] * g->spec.gas_constant); break;
                    case 6: v = 1 + 1 / W[4]; break;
                    case 7: {
                        double vr = B->U[2 * n + c + 1] / B->U[c + 1], vl = B->U[2 * n + c - 1] / B->U[c - 1];
                        double uu = B->U[n + c + S] / B->U[c + S], ud = B->U[n + c - S] / B->U[c - S];
                        v = (vr - vl - uu + ud) / (2 * B->dx);
                        break;
                    }
                    case 8: v = 1 - B->kap[c]; break; /* the covered fraction: 1 inside a body */
                    }
                    if (!(B->kap[c] > 0) && f != 8) v = f == 0 ? 0 : v;
                    buf[at++] = (float)v;
                }
        }
        lab_field(w, NAMES[f], LAB_AT_CELL, nc, buf);
    }
    free(buf);
    free(bl);
}

char *gas_header_json(const GasSpec *s, const char *title) {
    char *h = malloc(2048);
    if (!h) return NULL;
    snprintf(h, 2048,
             "{\"domain\":\"gas\",\"title\":\"%s\",\"solver\":\"src/lab/gas: Euler, HLLC/HLLE, MUSCL-MC, SSP-RK2, block AMR\","
             "\"geometry\":\"%s\",\"length_m\":%.9g,\"root_blocks\":[%d,%d],\"block_cells\":%d,\"max_level\":%d,"
             "\"fields\":{\"rho\":\"kg/m^3\",\"p\":\"Pa\",\"u\":\"m/s\",\"v\":\"m/s\",\"mach\":\"1\",\"T\":\"K\",\"gamma\":\"1\","
             "\"vorticity\":\"1/s\",\"solid\":\"1\"}}",
             title ? title : "", s->axisymmetric ? "axisymmetric (x axis, y radius)" : "planar", s->length, s->nbx, s->nby, NB, s->max_level);
    return h;
}
