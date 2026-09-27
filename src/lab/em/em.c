/* em.c - see em.h. Layout: grid and materials, the CPML, the 1D incident grid, the updates, measurements, output. */
#include "em.h"

#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../threads.h"
#include "../labshape.h"

#define EPS0 8.8541878128e-12
#define MU0 1.25663706212e-6
#define C0 299792458.0

typedef struct Slab {
    int P, n;             /* layers, cells along the direction */
    double *bE, *aE;      /* per node along the direction (n + 1), for the E-node positions */
    double *bH, *aH;      /* for the H-node positions (n) */
} Slab;

typedef struct Surf {
    int lo[3], hi[3];
    int nface[6];                /* face cells of each of the six faces */
    double complex *acc[6];      /* per face cell: E1, E2, H1, H2 phasors */
    bool on;
} Surf;

struct Em {
    EmSpec spec;
    int nx, ny, nz;
    long N;
    double dt, t;
    long steps;
    double *E[3], *H[3];
    uint16_t *emat[3];           /* per edge: index into the coefficient table */
    double *Ca, *Cb;             /* table */
    int ntab;
    unsigned char *pec_cell;     /* per cell */
    float *eps_cell;             /* per cell, for drawing */
    Slab slab[3];
    double *psiE[3][2], *psiH[3][2]; /* [component][which derivative] in the slabs */
    /* 1D incident grid */
    int n1, off1;
    double *ez1, *hy1, *be1, *bh1, *ae1, *ah1;
    double complex inc_dft;      /* the incident Ez phasor at x = 0 */
    Surf surf;
    ThreadPool *pool;
};

static inline long I3(const Em *e, int i, int j, int k) { return ((long)k * (e->ny + 1) + j) * (e->nx + 1) + i; }
static inline long C3(const Em *e, int i, int j, int k) { return ((long)k * e->ny + j) * e->nx + i; }

void em_spec_defaults(EmSpec *s) {
    memset(s, 0, sizeof *s);
    s->pml = 10;
    s->amplitude = 1.0;
}

static bool inside(const EmObject *o, double x, double y, double z) {
    switch (o->shape) {
    case EM_BOX: return x >= o->lo[0] && x < o->hi[0] && y >= o->lo[1] && y < o->hi[1] && z >= o->lo[2] && z < o->hi[2];
    case EM_SPHERE: return (x - o->c[0]) * (x - o->c[0]) + (y - o->c[1]) * (y - o->c[1]) + (z - o->c[2]) * (z - o->c[2]) < o->r * o->r;
    case EM_CYLINDER_Z: return (x - o->c[0]) * (x - o->c[0]) + (y - o->c[1]) * (y - o->c[1]) < o->r * o->r && z >= o->lo[2] && z < o->hi[2];
    case EM_SHAPES: {
        double p[3] = {x, y, z};
        return labshape_sdf(o->shapes, p) < 0;
    }
    }
    return false;
}

/* the index of (Ca, Cb) for permittivity eps (absolute) and conductivity sig, added to the table if new */
static int coef_index(Em *e, double eps, double sig, bool pec, int *cap) {
    double ca = pec ? 0 : (1 - sig * e->dt / (2 * eps)) / (1 + sig * e->dt / (2 * eps));
    double cb = pec ? 0 : (e->dt / eps) / (1 + sig * e->dt / (2 * eps));
    for (int q = 0; q < e->ntab; q++)
        if (e->Ca[q] == ca && e->Cb[q] == cb) return q;
    if (e->ntab == *cap) {
        *cap *= 2;
        e->Ca = realloc(e->Ca, (size_t)*cap * sizeof(double)), e->Cb = realloc(e->Cb, (size_t)*cap * sizeof(double));
    }
    e->Ca[e->ntab] = ca, e->Cb[e->ntab] = cb;
    return e->ntab++;
}

static void slab_init(Slab *s, int n, int P, double dt, double dx) {
    s->P = P, s->n = n;
    s->bE = calloc((size_t)n + 1, sizeof(double)), s->aE = calloc((size_t)n + 1, sizeof(double));
    s->bH = calloc((size_t)n + 1, sizeof(double)), s->aH = calloc((size_t)n + 1, sizeof(double));
    if (P <= 0) return;
    const double m = 3, smax = 0.8 * (m + 1) / (sqrt(MU0 / EPS0) * dx), amax = 0.05;
    for (int c = 0; c <= n; c++)
        for (int half = 0; half < 2; half++) {
            double pos = c + 0.5 * half; /* in cells */
            double depth = 0;
            if (pos < P) depth = (P - pos) / P;
            else if (pos > n - P) depth = (pos - (n - P)) / P;
            if (depth > 1) depth = 1;
            double sg = smax * pow(depth, m), al = amax * (1 - depth);
            double b = exp(-(sg + al) * dt / EPS0), a = sg > 0 ? sg / (sg + al) * (b - 1) : 0;
            if (half) s->bH[c] = depth > 0 ? b : 1, s->aH[c] = a;
            else s->bE[c] = depth > 0 ? b : 1, s->aE[c] = a;
        }
}

Em *em_create(const EmSpec *s, char *err, size_t errlen) {
    if (s->n[0] < 4 || s->n[1] < 4 || s->n[2] < 4 || !(s->dx > 0) || s->pml < 0 || 2 * s->pml >= s->n[0] || 2 * s->pml >= s->n[1] || 2 * s->pml >= s->n[2]) {
        snprintf(err, errlen, "em: at least 4 cells per direction, dx > 0, and room for the PML on both sides");
        return NULL;
    }
    Em *e = calloc(1, sizeof *e);
    e->spec = *s;
    e->nx = s->n[0], e->ny = s->n[1], e->nz = s->n[2];
    e->N = (long)(e->nx + 1) * (e->ny + 1) * (e->nz + 1);
    e->dt = 0.99 * s->dx / (C0 * sqrt(3.0));
    for (int c = 0; c < 3; c++) {
        e->E[c] = calloc((size_t)e->N, sizeof(double)), e->H[c] = calloc((size_t)e->N, sizeof(double));
        e->emat[c] = calloc((size_t)e->N, sizeof(uint16_t));
        if (!e->E[c] || !e->H[c] || !e->emat[c]) {
            snprintf(err, errlen, "em: out of memory");
            em_free(e);
            return NULL;
        }
    }
    long nc = (long)e->nx * e->ny * e->nz;
    e->pec_cell = calloc((size_t)nc, 1);
    e->eps_cell = malloc((size_t)nc * sizeof(float));
    float *sig_cell = calloc((size_t)nc, sizeof(float));
    for (int k = 0; k < e->nz; k++)
        for (int j = 0; j < e->ny; j++)
            for (int i = 0; i < e->nx; i++) {
                double x = (i + 0.5) * s->dx, y = (j + 0.5) * s->dx, z = (k + 0.5) * s->dx;
                float er = 1, sg = 0;
                unsigned char pec = 0;
                for (int q = 0; q < s->nobj; q++)
                    if (inside(&s->obj[q], x, y, z)) {
                        if (s->obj[q].pec) pec = 1;
                        else er = (float)s->obj[q].eps_r, sg = (float)s->obj[q].sigma;
                    }
                long c = C3(e, i, j, k);
                e->eps_cell[c] = pec ? -1 : er, sig_cell[c] = sg, e->pec_cell[c] = pec;
            }
    /* edge coefficients from the edge's own dual cell (a cube of side dx centred on the edge's midpoint), sampled at
     * 4 x 4 x 4 points: the permittivity and conductivity are the volume averages, and the edge is a conductor when at
     * least half of its dual cell is metal. Assigning from the voxels around the edge instead (the first form) moved
     * the effective surface outwards by up to half a cell: a sphere of 8 cells scattered 18 per cent too much (E3). */
    int cap = 64;
    e->Ca = malloc((size_t)cap * sizeof(double)), e->Cb = malloc((size_t)cap * sizeof(double));
    coef_index(e, EPS0, 0, false, &cap); /* index 0: vacuum */
    for (int comp = 0; comp < 3; comp++)
        for (int k = 0; k <= e->nz; k++)
            for (int j = 0; j <= e->ny; j++)
                for (int i = 0; i <= e->nx; i++) {
                    double p[3] = {i * s->dx, j * s->dx, k * s->dx};
                    p[comp] += 0.5 * s->dx; /* the edge's midpoint */
                    /* a quick test: is any object near? */
                    bool near = false;
                    for (int q = 0; q < s->nobj && !near; q++) {
                        const EmObject *o = &s->obj[q];
                        double lo[3], hi[3];
                        if (o->shape == EM_BOX) memcpy(lo, o->lo, sizeof lo), memcpy(hi, o->hi, sizeof hi);
                        else if (o->shape == EM_SHAPES)
                            for (int d = 0; d < 3; d++) {
                                lo[d] = 1e300, hi[d] = -1e300;
                                for (int b = 0; b < o->shapes->n; b++) lo[d] = fmin(lo[d], o->shapes->s[b].aabb_lo[d]), hi[d] = fmax(hi[d], o->shapes->s[b].aabb_hi[d]);
                            }
                        else
                            for (int d = 0; d < 3; d++) lo[d] = o->c[d] - o->r, hi[d] = o->c[d] + o->r;
                        if (o->shape == EM_CYLINDER_Z) lo[2] = o->lo[2], hi[2] = o->hi[2];
                        near = true;
                        for (int d = 0; d < 3; d++)
                            if (p[d] + s->dx < lo[d] || p[d] - s->dx > hi[d]) near = false;
                    }
                    if (!near) continue;
                    /* bodies by distance: an edge a cell or more from every surface is wholly inside or outside, and only
                     * the edges near a surface are sampled (else the sampling of an aircraft takes minutes) */
                    bool only_shapes = true, clear = true, metal_all = false;
                    for (int q = 0; q < s->nobj; q++) {
                        if (s->obj[q].shape != EM_SHAPES) { only_shapes = false; break; }
                        double d = labshape_sdf(s->obj[q].shapes, p);
                        if (fabs(d) < 0.9 * s->dx) clear = false;
                        if (d < 0 && s->obj[q].pec) metal_all = true;
                    }
                    if (only_shapes && clear) {
                        if (metal_all) e->emat[comp][I3(e, i, j, k)] = (uint16_t)coef_index(e, EPS0, 0, true, &cap);
                        continue;
                    }
                    double fe = 0, fs = 0, fm = 0;
                    for (int c1 = 0; c1 < 4; c1++)
                        for (int c2 = 0; c2 < 4; c2++)
                            for (int c3 = 0; c3 < 4; c3++) {
                                double x = p[0] + ((c1 + 0.5) / 4 - 0.5) * s->dx, y = p[1] + ((c2 + 0.5) / 4 - 0.5) * s->dx,
                                       z = p[2] + ((c3 + 0.5) / 4 - 0.5) * s->dx;
                                double er = 1, sg = 0;
                                bool metal = false;
                                for (int q = 0; q < s->nobj; q++)
                                    if (inside(&s->obj[q], x, y, z)) {
                                        if (s->obj[q].pec) metal = true;
                                        else er = s->obj[q].eps_r, sg = s->obj[q].sigma;
                                    }
                                fe += er, fs += sg, fm += metal;
                            }
                    fe /= 64, fs /= 64, fm /= 64;
                    bool pec = fm >= 0.5;
                    if (!pec && fe == 1 && fs == 0) continue;
                    int ci = coef_index(e, EPS0 * fe, fs, pec, &cap);
                    if (ci > 65535) ci = 0;
                    e->emat[comp][I3(e, i, j, k)] = (uint16_t)ci;
                }
    free(sig_cell);
    /* CPML */
    int P = s->pml;
    slab_init(&e->slab[0], e->nx, P, e->dt, s->dx), slab_init(&e->slab[1], e->ny, P, e->dt, s->dx), slab_init(&e->slab[2], e->nz, P, e->dt, s->dx);
    if (P > 0)
        for (int c = 0; c < 3; c++)
            for (int w = 0; w < 2; w++) e->psiE[c][w] = calloc((size_t)e->N, sizeof(double)), e->psiH[c][w] = calloc((size_t)e->N, sizeof(double));
    /* the 1D incident grid: 60 cells of absorbing layer on both ends, the source 10 cells into the free part */
    if (s->plane_wave) {
        e->off1 = 80;
        e->n1 = e->nx + 2 * e->off1;
        e->ez1 = calloc((size_t)e->n1 + 1, sizeof(double)), e->hy1 = calloc((size_t)e->n1 + 1, sizeof(double));
        e->be1 = calloc((size_t)e->n1 + 1, sizeof(double)), e->bh1 = calloc((size_t)e->n1 + 1, sizeof(double));
        e->ae1 = calloc((size_t)e->n1 + 1, sizeof(double)), e->ah1 = calloc((size_t)e->n1 + 1, sizeof(double));
        /* graded electric and matched magnetic loss (sigma* / mu0 = sigma / eps0): no reflection in the continuum */
        int L = 60;
        for (int g = 0; g <= e->n1; g++)
            for (int half = 0; half < 2; half++) {
                double pos = g + 0.5 * half, d = 0;
                if (pos < L) d = (L - pos) / L;
                else if (pos > e->n1 - L) d = (pos - (e->n1 - L)) / L;
                double sg = 0.8 * 4 / (sqrt(MU0 / EPS0) * s->dx) * d * d * d;
                double f = sg * e->dt / (2 * EPS0);
                if (half) e->bh1[g] = (1 - f) / (1 + f), e->ah1[g] = e->dt / (MU0 * s->dx) / (1 + f);
                else e->be1[g] = (1 - f) / (1 + f), e->ae1[g] = e->dt / (EPS0 * s->dx) / (1 + f);
            }
    }
    int nt = s->threads > 0 ? s->threads : cpu_perf_count();
    e->pool = pool_create(nt < 1 ? 1 : nt);
    return e;
}

void em_free(Em *e) {
    if (!e) return;
    for (int c = 0; c < 3; c++) {
        free(e->E[c]), free(e->H[c]), free(e->emat[c]);
        for (int w = 0; w < 2; w++) free(e->psiE[c][w]), free(e->psiH[c][w]);
        free(e->slab[c].bE), free(e->slab[c].aE), free(e->slab[c].bH), free(e->slab[c].aH);
    }
    free(e->Ca), free(e->Cb), free(e->pec_cell), free(e->eps_cell);
    free(e->ez1), free(e->hy1), free(e->be1), free(e->bh1), free(e->ae1), free(e->ah1);
    for (int f = 0; f < 6; f++) free(e->surf.acc[f]);
    if (e->pool) pool_destroy(e->pool);
    free(e);
}

static double pulse(const Em *e, double t) {
    double bw = e->spec.bandwidth > 0 ? e->spec.bandwidth : e->spec.f0 > 0 ? 0.5 * e->spec.f0 : 1e9;
    double tau = 1 / (M_PI * bw), t0 = 4 * tau;
    double g = exp(-((t - t0) / tau) * ((t - t0) / tau));
    return e->spec.f0 > 0 ? g * sin(2 * M_PI * e->spec.f0 * (t - t0)) : g;
}

/* ---- updates ------------------------------------------------------------------------------------------------------- */

typedef struct Ctx {
    Em *e;
} Ctx;

static void h_fn(void *ctx, int k0, int k1, int tid) {
    Em *e = ctx;
    const double f = e->dt / (MU0 * e->spec.dx);
    const long sx = 1, sy = e->nx + 1, sz = (long)(e->nx + 1) * (e->ny + 1);
    double *Ex = e->E[0], *Ey = e->E[1], *Ez = e->E[2], *Hx = e->H[0], *Hy = e->H[1], *Hz = e->H[2];
    for (int k = k0; k < k1; k++)
        for (int j = 0; j <= e->ny; j++)
            for (int i = 0; i <= e->nx; i++) {
                long c = I3(e, i, j, k);
                if (j < e->ny && k < e->nz) Hx[c] -= f * ((Ez[c + sy] - Ez[c]) - (Ey[c + sz] - Ey[c]));
                if (i < e->nx && k < e->nz) Hy[c] -= f * ((Ex[c + sz] - Ex[c]) - (Ez[c + sx] - Ez[c]));
                if (i < e->nx && j < e->ny) Hz[c] -= f * ((Ey[c + sx] - Ey[c]) - (Ex[c + sy] - Ex[c]));
            }
}

static void e_fn(void *ctx, int k0, int k1, int tid) {
    Em *e = ctx;
    const double idx = 1 / e->spec.dx;
    const long sx = 1, sy = e->nx + 1, sz = (long)(e->nx + 1) * (e->ny + 1);
    double *Ex = e->E[0], *Ey = e->E[1], *Ez = e->E[2], *Hx = e->H[0], *Hy = e->H[1], *Hz = e->H[2];
    for (int k = k0; k < k1; k++)
        for (int j = 0; j <= e->ny; j++)
            for (int i = 0; i <= e->nx; i++) {
                long c = I3(e, i, j, k);
                /* tangential E on the outer faces stays zero */
                if (i < e->nx && j > 0 && j < e->ny && k > 0 && k < e->nz) {
                    int q = e->emat[0][c];
                    Ex[c] = e->Ca[q] * Ex[c] + e->Cb[q] * idx * ((Hz[c] - Hz[c - sy]) - (Hy[c] - Hy[c - sz]));
                }
                if (j < e->ny && i > 0 && i < e->nx && k > 0 && k < e->nz) {
                    int q = e->emat[1][c];
                    Ey[c] = e->Ca[q] * Ey[c] + e->Cb[q] * idx * ((Hx[c] - Hx[c - sz]) - (Hz[c] - Hz[c - sx]));
                }
                if (k < e->nz && i > 0 && i < e->nx && j > 0 && j < e->ny) {
                    int q = e->emat[2][c];
                    Ez[c] = e->Ca[q] * Ez[c] + e->Cb[q] * idx * ((Hy[c] - Hy[c - sx]) - (Hx[c] - Hx[c - sy]));
                }
            }
}

/* the CPML corrections, for nodes inside a slab: psi = b psi + a d/dn(field); field += coefficient * psi */
static void cpml_h(Em *e) {
    const int P = e->spec.pml;
    if (P <= 0) return;
    const double f = e->dt / MU0, idx = 1 / e->spec.dx;
    const long sx = 1, sy = e->nx + 1, sz = (long)(e->nx + 1) * (e->ny + 1);
    double *Ex = e->E[0], *Ey = e->E[1], *Ez = e->E[2];
    for (int k = 0; k <= e->nz; k++)
        for (int j = 0; j <= e->ny; j++)
            for (int i = 0; i <= e->nx; i++) {
                bool px = i < P || i >= e->nx - P, py = j < P || j >= e->ny - P, pz = k < P || k >= e->nz - P;
                if (!px && !py && !pz) continue;
                long c = I3(e, i, j, k);
                if (j < e->ny && k < e->nz) { /* Hx: d Ez / dy (H node at j + 1/2), d Ey / dz */
                    if (py) {
                        double *p = &e->psiH[0][0][c];
                        *p = e->slab[1].bH[j] * *p + e->slab[1].aH[j] * (Ez[c + sy] - Ez[c]) * idx;
                        e->H[0][c] -= f * *p;
                    }
                    if (pz) {
                        double *p = &e->psiH[0][1][c];
                        *p = e->slab[2].bH[k] * *p + e->slab[2].aH[k] * (Ey[c + sz] - Ey[c]) * idx;
                        e->H[0][c] += f * *p;
                    }
                }
                if (i < e->nx && k < e->nz) { /* Hy: d Ex / dz, d Ez / dx */
                    if (pz) {
                        double *p = &e->psiH[1][0][c];
                        *p = e->slab[2].bH[k] * *p + e->slab[2].aH[k] * (Ex[c + sz] - Ex[c]) * idx;
                        e->H[1][c] -= f * *p;
                    }
                    if (px) {
                        double *p = &e->psiH[1][1][c];
                        *p = e->slab[0].bH[i] * *p + e->slab[0].aH[i] * (Ez[c + sx] - Ez[c]) * idx;
                        e->H[1][c] += f * *p;
                    }
                }
                if (i < e->nx && j < e->ny) { /* Hz: d Ey / dx, d Ex / dy */
                    if (px) {
                        double *p = &e->psiH[2][0][c];
                        *p = e->slab[0].bH[i] * *p + e->slab[0].aH[i] * (Ey[c + sx] - Ey[c]) * idx;
                        e->H[2][c] -= f * *p;
                    }
                    if (py) {
                        double *p = &e->psiH[2][1][c];
                        *p = e->slab[1].bH[j] * *p + e->slab[1].aH[j] * (Ex[c + sy] - Ex[c]) * idx;
                        e->H[2][c] += f * *p;
                    }
                }
            }
}

static void cpml_e(Em *e) {
    const int P = e->spec.pml;
    if (P <= 0) return;
    const double idx = 1 / e->spec.dx;
    const long sx = 1, sy = e->nx + 1, sz = (long)(e->nx + 1) * (e->ny + 1);
    double *Hx = e->H[0], *Hy = e->H[1], *Hz = e->H[2];
    /* the same edges the main update covers (tangential E on the outer faces stays zero) */
    for (int k = 0; k <= e->nz; k++)
        for (int j = 0; j <= e->ny; j++)
            for (int i = 0; i <= e->nx; i++) {
                bool px = i <= P || i >= e->nx - P, py = j <= P || j >= e->ny - P, pz = k <= P || k >= e->nz - P;
                if (!px && !py && !pz) continue;
                long c = I3(e, i, j, k);
                if (i < e->nx && j > 0 && j < e->ny && k > 0 && k < e->nz) { /* Ex (i + 1/2, j, k): d Hz / dy, d Hy / dz */
                    double cb = e->Cb[e->emat[0][c]];
                    if (py) {
                        double *p = &e->psiE[0][0][c];
                        *p = e->slab[1].bE[j] * *p + e->slab[1].aE[j] * (Hz[c] - Hz[c - sy]) * idx;
                        e->E[0][c] += cb * *p;
                    }
                    if (pz) {
                        double *p = &e->psiE[0][1][c];
                        *p = e->slab[2].bE[k] * *p + e->slab[2].aE[k] * (Hy[c] - Hy[c - sz]) * idx;
                        e->E[0][c] -= cb * *p;
                    }
                }
                if (j < e->ny && i > 0 && i < e->nx && k > 0 && k < e->nz) { /* Ey: d Hx / dz, d Hz / dx */
                    double cb = e->Cb[e->emat[1][c]];
                    if (pz) {
                        double *p = &e->psiE[1][0][c];
                        *p = e->slab[2].bE[k] * *p + e->slab[2].aE[k] * (Hx[c] - Hx[c - sz]) * idx;
                        e->E[1][c] += cb * *p;
                    }
                    if (px) {
                        double *p = &e->psiE[1][1][c];
                        *p = e->slab[0].bE[i] * *p + e->slab[0].aE[i] * (Hz[c] - Hz[c - sx]) * idx;
                        e->E[1][c] -= cb * *p;
                    }
                }
                if (k < e->nz && i > 0 && i < e->nx && j > 0 && j < e->ny) { /* Ez: d Hy / dx, d Hx / dy */
                    double cb = e->Cb[e->emat[2][c]];
                    if (px) {
                        double *p = &e->psiE[2][0][c];
                        *p = e->slab[0].bE[i] * *p + e->slab[0].aE[i] * (Hy[c] - Hy[c - sx]) * idx;
                        e->E[2][c] += cb * *p;
                    }
                    if (py) {
                        double *p = &e->psiE[2][1][c];
                        *p = e->slab[1].bE[j] * *p + e->slab[1].aE[j] * (Hx[c] - Hx[c - sy]) * idx;
                        e->E[2][c] -= cb * *p;
                    }
                }
            }
}

/* the incident plane wave: Ez1 at x = (g - off) dx, Hy1 at (g - off + 1/2) dx */
static inline double ez_inc(const Em *e, int i) { return e->ez1[i + e->off1]; }
static inline double hy_inc(const Em *e, int i) { return e->hy1[i + e->off1]; } /* at i + 1/2 */

static void tfsf_h(Em *e) {
    if (!e->spec.plane_wave) return;
    const int *lo = e->spec.tfsf_lo, *hi = e->spec.tfsf_hi;
    const double f = e->dt / (MU0 * e->spec.dx);
    for (int k = lo[2]; k < hi[2]; k++)
        for (int j = lo[1]; j <= hi[1]; j++) {
            /* Hy just outside the front and back faces sees the total Ez inside */
            e->H[1][I3(e, lo[0] - 1, j, k)] -= f * ez_inc(e, lo[0]);
            e->H[1][I3(e, hi[0], j, k)] += f * ez_inc(e, hi[0]);
        }
    for (int k = lo[2]; k < hi[2]; k++)
        for (int i = lo[0]; i <= hi[0]; i++) {
            /* Hx just outside the side faces y = lo, hi */
            e->H[0][I3(e, i, lo[1] - 1, k)] += f * ez_inc(e, i);
            e->H[0][I3(e, i, hi[1], k)] -= f * ez_inc(e, i);
        }
}

static void tfsf_e(Em *e) {
    if (!e->spec.plane_wave) return;
    const int *lo = e->spec.tfsf_lo, *hi = e->spec.tfsf_hi;
    const double idx = 1 / e->spec.dx;
    for (int k = lo[2]; k < hi[2]; k++)
        for (int j = lo[1]; j <= hi[1]; j++) {
            long c0 = I3(e, lo[0], j, k), c1 = I3(e, hi[0], j, k);
            e->E[2][c0] -= e->Cb[e->emat[2][c0]] * idx * hy_inc(e, lo[0] - 1);
            e->E[2][c1] += e->Cb[e->emat[2][c1]] * idx * hy_inc(e, hi[0]);
        }
    /* the incident Hy is tangential to the faces z = lo and z = hi too: Ex there sees it across the face */
    for (int j = lo[1]; j <= hi[1]; j++)
        for (int i = lo[0]; i < hi[0]; i++) {
            long c0 = I3(e, i, j, lo[2]), c1 = I3(e, i, j, hi[2]);
            e->E[0][c0] += e->Cb[e->emat[0][c0]] * idx * hy_inc(e, i);
            e->E[0][c1] -= e->Cb[e->emat[0][c1]] * idx * hy_inc(e, i);
        }
}

static void inc_step_h(Em *e) {
    for (int g = 0; g < e->n1; g++) e->hy1[g] = e->bh1[g] * e->hy1[g] + e->ah1[g] * (e->ez1[g + 1] - e->ez1[g]);
}

static void inc_step_e(Em *e, double t) {
    for (int g = 1; g < e->n1; g++) e->ez1[g] = e->be1[g] * e->ez1[g] + e->ae1[g] * (e->hy1[g] - e->hy1[g - 1]);
    e->ez1[e->off1 - 10] += e->spec.amplitude * pulse(e, t); /* soft source, 10 cells before x = 0 */
}

static void surf_accumulate(Em *e);

void em_step(Em *e) {
    /* H at n + 1/2 */
    pool_for(e->pool, e->nz + 1, 1, h_fn, e);
    cpml_h(e);
    tfsf_h(e);
    if (e->spec.plane_wave) inc_step_h(e);
    /* E at n + 1 */
    pool_for(e->pool, e->nz + 1, 1, e_fn, e);
    cpml_e(e);
    tfsf_e(e);
    if (e->spec.plane_wave) inc_step_e(e, e->t + e->dt);
    for (int d = 0; d < e->spec.ndip; d++) {
        int i = (int)lround(e->spec.dip_pos[d][0] / e->spec.dx), j = (int)lround(e->spec.dip_pos[d][1] / e->spec.dx),
            k = (int)floor(e->spec.dip_pos[d][2] / e->spec.dx);
        e->E[2][I3(e, i, j, k)] += e->spec.amplitude * pulse(e, e->t + e->dt);
    }
    e->t += e->dt;
    e->steps++;
    if (e->spec.plane_wave && e->spec.f_dft > 0) e->inc_dft += ez_inc(e, 0) * cexp(-I * 2 * M_PI * e->spec.f_dft * e->t) * e->dt;
    if (e->surf.on) surf_accumulate(e);
}

double em_time(const Em *e) { return e->t; }
double em_dt(const Em *e) { return e->dt; }
long em_steps(const Em *e) { return e->steps; }

double em_probe(const Em *e, int comp, const double x[3]) {
    int i = (int)lround(x[0] / e->spec.dx), j = (int)lround(x[1] / e->spec.dx), k = (int)lround(x[2] / e->spec.dx);
    if (i < 0 || j < 0 || k < 0 || i > e->nx || j > e->ny || k > e->nz) return NAN;
    return comp < 3 ? e->E[comp][I3(e, i, j, k)] : e->H[comp - 3][I3(e, i, j, k)];
}

double em_incident_ez(const Em *e, double x) {
    if (!e->spec.plane_wave) return 0;
    int g = (int)lround(x / e->spec.dx) + e->off1;
    return (g >= 0 && g <= e->n1) ? e->ez1[g] : 0;
}

double em_max_scattered_ez(const Em *e, int margin) {
    const int *lo = e->spec.tfsf_lo, *hi = e->spec.tfsf_hi;
    int P = e->spec.pml;
    double m = 0;
    for (int k = P + margin; k < e->nz - P - margin; k++)
        for (int j = P + margin; j <= e->ny - P - margin; j++)
            for (int i = P + margin; i <= e->nx - P - margin; i++) {
                bool in = i >= lo[0] - 1 && i <= hi[0] + 1 && j >= lo[1] - 1 && j <= hi[1] + 1 && k >= lo[2] - 1 && k <= hi[2];
                if (in) continue;
                m = fmax(m, fabs(e->E[2][I3(e, i, j, k)]));
            }
    return m;
}

/* ---- scattered power through a box ------------------------------------------------------------------------------------ */

/* face f: 0 x-lo, 1 x-hi, 2 y-lo, 3 y-hi, 4 z-lo, 5 z-hi. Face cells span the other two directions. */
void em_scatter_surface(Em *e, const int lo[3], const int hi[3]) {
    Surf *s = &e->surf;
    memcpy(s->lo, lo, sizeof s->lo), memcpy(s->hi, hi, sizeof s->hi);
    for (int f = 0; f < 6; f++) {
        int ax = f / 2, a = (ax + 1) % 3, b = (ax + 2) % 3;
        s->nface[f] = (hi[a] - lo[a]) * (hi[b] - lo[b]);
        s->acc[f] = calloc((size_t)s->nface[f] * 4, sizeof(double complex));
    }
    s->on = true;
}

/* E tangential (two components) and H tangential at the centre of a face cell; H is averaged across the face */
static void face_fields(const Em *e, int ax, int pos, int u, int v, double Et[2], double Ht[2]) {
    int a = (ax + 1) % 3, b = (ax + 2) % 3;
    int idx[3];
    idx[ax] = pos, idx[a] = u, idx[b] = v;
    /* E_a at (ax = pos, a = u + 1/2, b = v): average over b, v and v + 1 */
    int q1[3] = {idx[0], idx[1], idx[2]}, q2[3] = {idx[0], idx[1], idx[2]};
    q2[b] += 1;
    Et[0] = 0.5 * (e->E[a][I3(e, q1[0], q1[1], q1[2])] + e->E[a][I3(e, q2[0], q2[1], q2[2])]);
    int r1[3] = {idx[0], idx[1], idx[2]}, r2[3] = {idx[0], idx[1], idx[2]};
    r2[a] += 1;
    Et[1] = 0.5 * (e->E[b][I3(e, r1[0], r1[1], r1[2])] + e->E[b][I3(e, r2[0], r2[1], r2[2])]);
    /* H_a at (ax = pos + 1/2, a = u, b = v + 1/2): averaged over ax (pos - 1, pos) and over a (u, u + 1) */
    double h = 0;
    for (int s1 = -1; s1 <= 0; s1++)
        for (int s2 = 0; s2 <= 1; s2++) {
            int q[3] = {idx[0], idx[1], idx[2]};
            q[ax] += s1, q[a] += s2;
            h += e->H[a][I3(e, q[0], q[1], q[2])];
        }
    Ht[0] = 0.25 * h;
    h = 0;
    for (int s1 = -1; s1 <= 0; s1++)
        for (int s2 = 0; s2 <= 1; s2++) {
            int q[3] = {idx[0], idx[1], idx[2]};
            q[ax] += s1, q[b] += s2;
            h += e->H[b][I3(e, q[0], q[1], q[2])];
        }
    Ht[1] = 0.25 * h;
}

static void surf_accumulate(Em *e) {
    Surf *s = &e->surf;
    double w = 2 * M_PI * e->spec.f_dft;
    double complex pe = cexp(-I * w * e->t) * e->dt, ph = cexp(-I * w * (e->t - 0.5 * e->dt)) * e->dt;
    for (int f = 0; f < 6; f++) {
        int ax = f / 2, a = (ax + 1) % 3, b = (ax + 2) % 3;
        int pos = (f & 1) ? s->hi[ax] : s->lo[ax];
        int nu = s->hi[a] - s->lo[a];
        for (int v = s->lo[b]; v < s->hi[b]; v++)
            for (int u = s->lo[a]; u < s->hi[a]; u++) {
                double Et[2], Ht[2];
                face_fields(e, ax, pos, u, v, Et, Ht);
                double complex *acc = s->acc[f] + 4 * ((size_t)(v - s->lo[b]) * nu + (u - s->lo[a]));
                acc[0] += Et[0] * pe, acc[1] += Et[1] * pe, acc[2] += Ht[0] * ph, acc[3] += Ht[1] * ph;
            }
    }
}

double em_scattered_power(const Em *e, double *incident_intensity) {
    const Surf *s = &e->surf;
    double P = 0, dA = e->spec.dx * e->spec.dx;
    for (int f = 0; f < 6; f++) {
        int ax = f / 2, a = (ax + 1) % 3, b = (ax + 2) % 3;
        double sgn = (f & 1) ? 1 : -1;
        int nu = s->hi[a] - s->lo[a];
        for (int v = s->lo[b]; v < s->hi[b]; v++)
            for (int u = s->lo[a]; u < s->hi[a]; u++) {
                const double complex *acc = s->acc[f] + 4 * ((size_t)(v - s->lo[b]) * nu + (u - s->lo[a]));
                /* (E x H*) along ax: Ea Hb* - Eb Ha* */
                P += sgn * 0.5 * creal(acc[0] * conj(acc[3]) - acc[1] * conj(acc[2])) * dA;
            }
    }
    if (incident_intensity) *incident_intensity = cabs(e->inc_dft) * cabs(e->inc_dft) / (2 * sqrt(MU0 / EPS0));
    return P;
}

/* ---- output -------------------------------------------------------------------------------------------------------- */

static bool write_grid(Em *e, LabWriter *w, double z_slice, bool volume) {
    int k = (int)lround(z_slice / e->spec.dx);
    if (k < 0) k = 0;
    if (k >= e->nz) k = e->nz - 1;
    int first = volume ? 0 : k, last = volume ? e->nz : k + 1;
    LabBlock b = {{e->nx, e->ny, last - first}, 0, LAB_PLANE_XY, {0, 0, volume ? 0 : (k + 0.5) * e->spec.dx}, {e->spec.dx, e->spec.dx, e->spec.dx}};
    size_t n = (size_t)e->nx * e->ny * (last - first);
    float *ez = malloc(n * sizeof(float)), *mat = malloc(n * sizeof(float));
    float *total=volume?malloc(n*sizeof(float)):NULL,*scatter=volume?malloc(n*sizeof(float)):NULL;
    if (!ez || !mat || (volume && (!total || !scatter))) { free(ez); free(mat);free(total);free(scatter);return false; }
    for (k = first; k < last; k++)
    for (int j = 0; j < e->ny; j++)
        for (int i = 0; i < e->nx; i++) {
            /* Ez at the cell centre of the slice: the edge at (i, j) and its neighbours, averaged */
            double v = 0.25 * (e->E[2][I3(e, i, j, k)] + e->E[2][I3(e, i + 1, j, k)] + e->E[2][I3(e, i, j + 1, k)] + e->E[2][I3(e, i + 1, j + 1, k)]);
            ez[((size_t)(k-first) * e->ny + j) * e->nx + i] = (float)v;
            if(volume){
                double incident_inside=0,incident_outside=0;
                if(e->spec.plane_wave)for(int dj=0;dj<2;dj++)for(int di=0;di<2;di++){
                    const int *lo=e->spec.tfsf_lo,*hi=e->spec.tfsf_hi;
                    bool inside=i+di>=lo[0] && i+di<=hi[0] && j+dj>=lo[1] && j+dj<=hi[1] && k>=lo[2] && k<hi[2];
                    double inc=.25*ez_inc(e,i+di);
                    if(inside)incident_inside+=inc;else incident_outside+=inc;
                }
                size_t q=((size_t)(k-first)*e->ny+j)*e->nx+i;
                total[q]=(float)(v+incident_outside);scatter[q]=(float)(v-incident_inside);
            }
            float er = e->eps_cell[C3(e, i, j, k)];
            mat[((size_t)(k-first) * e->ny + j) * e->nx + i] = er < 0 ? 1.0f : (er > 1 ? 0.3f : 0.0f); /* conductors drawn solid, dielectrics left open */
        }
    lab_part_blocks(w, volume ? "volume" : "slice", 1, &b);
    if(volume){lab_field(w,"ez_scattered",LAB_AT_CELL,n,scatter);lab_field(w,"ez_total",LAB_AT_CELL,n,total);}
    lab_field(w, "ez", LAB_AT_CELL, n, ez);
    lab_field(w, "material", LAB_AT_CELL, n, mat);
    free(ez), free(mat);free(total);free(scatter);
    return true;
}

void em_write_frame(Em *e, LabWriter *w, double z_slice) { (void)write_grid(e,w,z_slice,false); }
bool em_write_volume(Em *e, LabWriter *w) { return write_grid(e,w,0,true); }

void em_volume_ez(const Em *e, int stride, float *out, int dims[3]) {
    if (stride < 1) stride = 1;
    int nx = (e->nx + stride - 1) / stride, ny = (e->ny + stride - 1) / stride, nz = (e->nz + stride - 1) / stride;
    dims[0] = nx, dims[1] = ny, dims[2] = nz;
    if (!out) return;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                int a = i * stride, b2 = j * stride, c = k * stride; /* (not I: complex.h's) */
                double v = 0.25 * (e->E[2][I3(e, a, b2, c)] + e->E[2][I3(e, a + 1, b2, c)] + e->E[2][I3(e, a, b2 + 1, c)] + e->E[2][I3(e, a + 1, b2 + 1, c)]);
                out[((size_t)k * ny + j) * nx + i] = (float)v;
            }
}

bool em_write_volume_ez(Em *e, LabWriter *w, int stride) {
    if (stride < 1) stride = 1;
    int d[3];
    em_volume_ez(e, stride, NULL, d);
    size_t n = (size_t)d[0] * d[1] * d[2];
    float *ez = malloc(n * sizeof(float));
    if (!ez) return false;
    em_volume_ez(e, stride, ez, d);
    double h = e->spec.dx * stride, o = 0.5 * (stride - 1) * e->spec.dx;
    LabBlock b = {{d[0], d[1], d[2]}, 0, LAB_PLANE_XY, {o, o, o}, {h, h, h}};
    lab_part_blocks(w, "volume", 1, &b);
    lab_field(w, "ez", LAB_AT_CELL, n, ez);
    free(ez);
    return true;
}

char *em_header_json(const EmSpec *s, const char *title) {
    char *h = malloc(2048);
    snprintf(h, 2048,
             "{\"domain\":\"em\",\"title\":\"%s\",\"solver\":\"src/lab/em: Yee FDTD, CPML, TF/SF plane wave\",\"cells\":[%d,%d,%d],\"dx_m\":%.6g,"
             "\"fields\":{\"ez\":\"V/m\",\"ez_total\":\"V/m\",\"ez_scattered\":\"V/m\",\"material\":\"1\"}}",
             title ? title : "", s->n[0], s->n[1], s->n[2], s->dx);
    return h;
}
