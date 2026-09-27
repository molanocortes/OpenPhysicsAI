/* sph.c - see sph.h. */
#include "sph.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../threads.h"

/* symmetric tensors: xx, yy, zz, xy, yz, xz (the order of impact.c) */
enum { XX, YY, ZZ, XY, YZ, XZ };

struct Sph {
    SphSpec spec;
    ThreadPool *pool;
    double h, rs, sigma_k, w_dx; /* smoothing length, neighbour search radius, kernel normalisation, W(dx) */
    int n, cap, nbodies;
    int body_mat[64];
    double body_mass[64];
    /* real particles */
    double *x, *v, *rho, *e, *S, *ep, *wp, *m, *rate, *fb; /* fb: Balsara's factor, from the last evaluation */
    int *mat, *body;
    /* Heun: the state at the start of the step and the two derivative sets */
    double *x0, *v0, *rho0, *e0, *S0;
    double *a1, *dr1, *de1, *dS1, *a2, *dr2, *de2, *dS2;
    /* every particle the sums see, real then mirror images: position, velocity, density, pressure, sound speed,
     * stress, artificial stress, mass */
    int ntot, captot;
    double *X, *V, *RHO, *P, *C, *SIG, *R, *M, *FB;
    /* mirror images: source, sign mask per axis (bit a set: coordinate a reflected), offset, across a wall */
    int ng, capg;
    int *gsrc;
    unsigned char *gmask, *gwall;
    double *goff;
    /* neighbour lists of the real particles */
    int *nstart, *nlist;
    size_t nlist_cap;
    /* spatial hash */
    int *cell, *hcount, *horder;
    uint32_t *hkey;
    size_t hsize;
    double t, dt;
    long steps;
    bool unstable;
    double *pout, *Tout; /* pressure and temperature of the real particles, for the output */
};

void sph_spec_defaults(SphSpec *s) {
    memset(s, 0, sizeof *s);
    s->dx = 1e-3;
    s->h_factor = 1.3;
    s->safety = 0.3;
    s->av_alpha = 1.0, s->av_beta = 2.0;
    s->art_stress = 0.3;
}

static void run(Sph *p, int count, int grain, ParallelFn fn, void *ctx) {
    if (count <= 0) return;
    if (p->pool) pool_for(p->pool, count, grain, fn, ctx);
    else fn(ctx, 0, count, 0);
}

Sph *sph_create(const SphSpec *s, char *err, size_t errlen) {
    if (!(s->dx > 0) || s->nmaterials < 1) {
        snprintf(err, errlen, "sph: a particle spacing and at least one material are required");
        return NULL;
    }
    Sph *p = calloc(1, sizeof *p);
    p->spec = *s;
    if (!(p->spec.h_factor > 0)) p->spec.h_factor = 1.3;
    if (!(p->spec.safety > 0)) p->spec.safety = 0.3;
    p->h = p->spec.h_factor * s->dx;
    p->rs = 2.4 * p->h; /* support 2h and a margin for the motion within a step (the step keeps it below 0.2 h) */
    p->sigma_k = 1.0 / (M_PI * p->h * p->h * p->h);
    double q = s->dx / p->h;
    p->w_dx = q < 1 ? p->sigma_k * (1 - 1.5 * q * q + 0.75 * q * q * q) : p->sigma_k * 0.25 * pow(2 - q, 3);
    int nt = s->threads > 0 ? s->threads : cpu_perf_count();
    if (nt > 1) p->pool = pool_create(nt);
    return p;
}

void sph_free(Sph *p) {
    if (!p) return;
    if (p->pool) pool_destroy(p->pool);
    double *d[] = {p->x, p->v, p->rho, p->e, p->S, p->ep, p->wp, p->m, p->rate, p->fb, p->FB, p->x0, p->v0, p->rho0, p->e0, p->S0, p->a1, p->dr1,
                   p->de1, p->dS1, p->a2, p->dr2, p->de2, p->dS2, p->X, p->V, p->RHO, p->P, p->C, p->SIG, p->R, p->M, p->goff,
                   p->pout, p->Tout};
    for (size_t i = 0; i < sizeof d / sizeof d[0]; i++) free(d[i]);
    free(p->mat), free(p->body), free(p->gsrc), free(p->gmask), free(p->gwall), free(p->nstart), free(p->nlist);
    free(p->cell), free(p->hcount), free(p->horder), free(p->hkey);
    free(p);
}

/* ---- bodies -------------------------------------------------------------------------------------------------------- */

static void grow_real(Sph *p, int need) {
    if (need <= p->cap) return;
    int c = p->cap ? p->cap : 4096;
    while (c < need) c *= 2;
#define G(a, k) p->a = realloc(p->a, (size_t)c * (k) * sizeof *p->a)
    G(x, 3), G(v, 3), G(rho, 1), G(e, 1), G(S, 6), G(ep, 1), G(wp, 1), G(m, 1), G(rate, 1), G(fb, 1), G(mat, 1), G(body, 1);
    G(x0, 3), G(v0, 3), G(rho0, 1), G(e0, 1), G(S0, 6), G(a1, 3), G(dr1, 1), G(de1, 1), G(dS1, 6), G(a2, 3), G(dr2, 1), G(de2, 1),
        G(dS2, 6), G(pout, 1), G(Tout, 1);
#undef G
    p->cap = c;
}

static bool material_side(const Sph *p, const double x[3]) {
    for (int k = 0; k < p->spec.nplanes; k++) {
        const SphPlane *P = &p->spec.planes[k];
        if ((x[P->axis] - P->pos) * P->side <= 0) return false;
    }
    return true;
}

typedef bool (*ShapeFn)(const double x[3], const double *a);

static int add_body(Sph *p, const double lo[3], const double hi[3], ShapeFn in, const double *args, int material, const double vel[3]) {
    if (material < 0 || material >= p->spec.nmaterials || p->nbodies >= 64) return -1;
    const ImMaterial *M = &p->spec.materials[material];
    const double dx = p->spec.dx, mass = M->rho0 * dx * dx * dx;
    int i0[3], i1[3];
    for (int a = 0; a < 3; a++) i0[a] = (int)floor(lo[a] / dx) - 1, i1[a] = (int)ceil(hi[a] / dx) + 1;
    int b = p->nbodies, added = 0;
    for (int k = i0[2]; k <= i1[2]; k++)
        for (int j = i0[1]; j <= i1[1]; j++)
            for (int i = i0[0]; i <= i1[0]; i++) {
                double x[3] = {(i + 0.5) * dx, (j + 0.5) * dx, (k + 0.5) * dx};
                if (x[0] < lo[0] || x[0] > hi[0] || x[1] < lo[1] || x[1] > hi[1] || x[2] < lo[2] || x[2] > hi[2]) continue;
                if (!in(x, args) || !material_side(p, x)) continue;
                grow_real(p, p->n + 1);
                int n = p->n++;
                for (int a = 0; a < 3; a++) p->x[3 * n + a] = x[a], p->v[3 * n + a] = vel[a];
                p->rho[n] = M->rho0, p->e[n] = 0, p->ep[n] = 0, p->wp[n] = 0, p->m[n] = mass, p->rate[n] = 0, p->fb[n] = 1;
                for (int c = 0; c < 6; c++) p->S[6 * n + c] = 0;
                p->mat[n] = material, p->body[n] = b;
                added++;
            }
    if (!added) return -1;
    p->body_mat[b] = material, p->body_mass[b] = added * mass;
    return p->nbodies++;
}

static bool in_box(const double x[3], const double *a) {
    (void)x, (void)a;
    return true;
}
static bool in_sphere(const double x[3], const double *a) {
    double d0 = x[0] - a[0], d1 = x[1] - a[1], d2 = x[2] - a[2];
    return d0 * d0 + d1 * d1 + d2 * d2 < a[3] * a[3];
}
static bool in_cylinder(const double x[3], const double *a) {
    double d0 = x[0] - a[0], d1 = x[1] - a[1];
    return d0 * d0 + d1 * d1 < a[3] * a[3];
}

int sph_add_box(Sph *p, const double lo[3], const double hi[3], int material, const double v[3]) {
    return add_body(p, lo, hi, in_box, NULL, material, v);
}
int sph_add_sphere(Sph *p, const double c[3], double r, int material, const double v[3]) {
    double lo[3] = {c[0] - r, c[1] - r, c[2] - r}, hi[3] = {c[0] + r, c[1] + r, c[2] + r}, a[4] = {c[0], c[1], c[2], r};
    return add_body(p, lo, hi, in_sphere, a, material, v);
}
int sph_add_cylinder(Sph *p, const double base[3], double r, double length, int material, const double v[3]) {
    double lo[3] = {base[0] - r, base[1] - r, base[2]}, hi[3] = {base[0] + r, base[1] + r, base[2] + length},
           a[4] = {base[0], base[1], base[2], r};
    return add_body(p, lo, hi, in_cylinder, a, material, v);
}

/* ---- mirror images and neighbours ---------------------------------------------------------------------------------- */

static void grow_tot(Sph *p, int need) {
    if (need <= p->captot) return;
    int c = p->captot ? p->captot : 8192;
    while (c < need) c *= 2;
#define G(a, k) p->a = realloc(p->a, (size_t)c * (k) * sizeof *p->a)
    G(X, 3), G(V, 3), G(RHO, 1), G(P, 1), G(C, 1), G(SIG, 6), G(R, 6), G(M, 1), G(FB, 1), G(cell, 3), G(hkey, 1), G(horder, 1);
#undef G
    p->captot = c;
}

static void grow_ghost(Sph *p, int need) {
    if (need <= p->capg) return;
    int c = p->capg ? p->capg : 4096;
    while (c < need) c *= 2;
    p->gsrc = realloc(p->gsrc, (size_t)c * sizeof *p->gsrc);
    p->gmask = realloc(p->gmask, (size_t)c);
    p->gwall = realloc(p->gwall, (size_t)c);
    p->goff = realloc(p->goff, (size_t)c * 3 * sizeof *p->goff);
    p->capg = c;
}

/* the image of a vector (velocity) and of a symmetric tensor under a reflection mask */
static inline double sgn(unsigned mask, int a) { return (mask >> a) & 1u ? -1.0 : 1.0; }
static inline void mirror_tensor(unsigned mask, const double *s, double *o) {
    double s0 = sgn(mask, 0), s1 = sgn(mask, 1), s2 = sgn(mask, 2);
    o[XX] = s[XX], o[YY] = s[YY], o[ZZ] = s[ZZ], o[XY] = s0 * s1 * s[XY], o[YZ] = s1 * s2 * s[YZ], o[XZ] = s0 * s2 * s[XZ];
}

static void image_position(const Sph *p, int g, const double *x, double *o) {
    unsigned mk = p->gmask[g];
    const double *src = x + 3 * (size_t)p->gsrc[g];
    for (int a = 0; a < 3; a++) o[a] = sgn(mk, a) * src[a] + p->goff[3 * (size_t)g + a];
}

/* images of the real particles within the search radius of each plane, and images of images near the next plane */
static void build_ghosts(Sph *p) {
    p->ng = 0;
    for (int k = 0; k < p->spec.nplanes; k++) {
        const SphPlane *P = &p->spec.planes[k];
        int nimg = p->n + p->ng; /* the images so far (real and mirror), each checked against this plane */
        for (int i = 0; i < nimg; i++) {
            double xi[3];
            unsigned mk = 0;
            double off[3] = {0, 0, 0};
            int src = i;
            bool wall = P->wall;
            if (i < p->n) {
                for (int a = 0; a < 3; a++) xi[a] = p->x[3 * (size_t)i + a];
            } else {
                int g = i - p->n;
                image_position(p, g, p->x, xi);
                mk = p->gmask[g], src = p->gsrc[g], wall = wall || p->gwall[g];
                for (int a = 0; a < 3; a++) off[a] = p->goff[3 * (size_t)g + a];
            }
            double d = (xi[P->axis] - P->pos) * P->side;
            if (d < 0 || d >= p->rs) continue;
            grow_ghost(p, p->ng + 1);
            int g = p->ng++;
            p->gsrc[g] = src, p->gmask[g] = (unsigned char)(mk ^ (1u << P->axis)), p->gwall[g] = wall;
            for (int a = 0; a < 3; a++) p->goff[3 * (size_t)g + a] = off[a];
            p->goff[3 * (size_t)g + P->axis] = 2 * P->pos - off[P->axis];
        }
    }
    p->ntot = p->n + p->ng;
    grow_tot(p, p->ntot);
}

static inline uint32_t hash3(int i, int j, int k) {
    return ((uint32_t)i * 73856093u) ^ ((uint32_t)j * 19349663u) ^ ((uint32_t)k * 83492791u);
}

typedef struct {
    Sph *p;
    int pass; /* 0 count, 1 fill */
} NbrCtx;

static void nbr_chunk(void *vc, int b, int e, int tid) {
    (void)tid;
    NbrCtx *C = vc;
    Sph *p = C->p;
    const double rs2 = p->rs * p->rs;
    const uint32_t mask = (uint32_t)p->hsize - 1;
    for (int i = b; i < e; i++) {
        const double *xi = p->X + 3 * (size_t)i;
        const int *ci = p->cell + 3 * (size_t)i;
        int cnt = 0;
        int *out = C->pass ? p->nlist + p->nstart[i] : NULL;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int cx = ci[0] + dx, cy = ci[1] + dy, cz = ci[2] + dz;
                    uint32_t hk = hash3(cx, cy, cz) & mask;
                    for (int s = p->hcount[hk]; s < p->hcount[hk + 1]; s++) {
                        int j = p->horder[s];
                        if (j == i) continue;
                        const int *cj = p->cell + 3 * (size_t)j;
                        if (cj[0] != cx || cj[1] != cy || cj[2] != cz) continue; /* another cell in the same bucket */
                        const double *xj = p->X + 3 * (size_t)j;
                        double r0 = xi[0] - xj[0], r1 = xi[1] - xj[1], r2 = xi[2] - xj[2];
                        if (r0 * r0 + r1 * r1 + r2 * r2 >= rs2) continue;
                        if (out) out[cnt] = j;
                        cnt++;
                    }
                }
        if (!C->pass) p->nstart[i + 1] = cnt;
    }
}

static void build_neighbours(Sph *p) {
    int N = p->ntot;
    for (int i = 0; i < p->n; i++)
        for (int a = 0; a < 3; a++) p->X[3 * (size_t)i + a] = p->x[3 * (size_t)i + a];
    for (int g = 0; g < p->ng; g++) image_position(p, g, p->x, p->X + 3 * (size_t)(p->n + g));
    size_t hs = 1;
    while (hs < 2 * (size_t)N) hs <<= 1;
    if (hs != p->hsize) {
        free(p->hcount);
        p->hcount = malloc((hs + 1) * sizeof *p->hcount);
        p->hsize = hs;
    }
    memset(p->hcount, 0, (hs + 1) * sizeof *p->hcount);
    const double inv = 1.0 / p->rs;
    for (int i = 0; i < N; i++) {
        int *c = p->cell + 3 * (size_t)i;
        for (int a = 0; a < 3; a++) c[a] = (int)floor(p->X[3 * (size_t)i + a] * inv);
        p->hkey[i] = hash3(c[0], c[1], c[2]) & (uint32_t)(hs - 1);
        p->hcount[p->hkey[i] + 1]++;
    }
    for (size_t k = 0; k < hs; k++) p->hcount[k + 1] += p->hcount[k];
    /* counting sort into buckets; a scratch copy of the starts places each particle */
    int *pos = malloc(hs * sizeof *pos);
    memcpy(pos, p->hcount, hs * sizeof *pos);
    for (int i = 0; i < N; i++) p->horder[pos[p->hkey[i]]++] = i;
    free(pos);
    p->nstart = realloc(p->nstart, ((size_t)p->n + 1) * sizeof *p->nstart);
    p->nstart[0] = 0;
    NbrCtx C = {p, 0};
    run(p, p->n, 256, nbr_chunk, &C);
    for (int i = 0; i < p->n; i++) p->nstart[i + 1] += p->nstart[i];
    size_t tot = (size_t)p->nstart[p->n];
    if (tot > p->nlist_cap) {
        p->nlist_cap = tot + tot / 4 + 1024;
        free(p->nlist);
        p->nlist = malloc(p->nlist_cap * sizeof *p->nlist);
    }
    C.pass = 1;
    run(p, p->n, 256, nbr_chunk, &C);
}

/* ---- material state ------------------------------------------------------------------------------------------------ */

static double temperature(const ImMaterial *M, double wp) { return M->T_room + M->chi * wp / M->cp; }

/* the principal decomposition of a symmetric 3x3 tensor by Jacobi rotations: s = Q diag(l) Q^T */
static void eigen_sym(const double *t, double l[3], double Q[3][3]) {
    double A[3][3] = {{t[XX], t[XY], t[XZ]}, {t[XY], t[YY], t[YZ]}, {t[XZ], t[YZ], t[ZZ]}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) Q[i][j] = i == j;
    for (int sweep = 0; sweep < 12; sweep++) {
        double off = A[0][1] * A[0][1] + A[0][2] * A[0][2] + A[1][2] * A[1][2];
        double scale = A[0][0] * A[0][0] + A[1][1] * A[1][1] + A[2][2] * A[2][2];
        if (off <= 1e-24 * scale || off == 0) break;
        for (int a = 0; a < 2; a++)
            for (int b = a + 1; b < 3; b++) {
                if (A[a][b] == 0) continue;
                double th = 0.5 * (A[b][b] - A[a][a]) / A[a][b];
                double tt = (th >= 0 ? 1 : -1) / (fabs(th) + sqrt(th * th + 1));
                double c = 1 / sqrt(tt * tt + 1), s = tt * c;
                for (int k = 0; k < 3; k++) { /* A = J^T A J, columns then rows */
                    double ka = A[k][a], kb = A[k][b];
                    A[k][a] = c * ka - s * kb, A[k][b] = s * ka + c * kb;
                }
                for (int k = 0; k < 3; k++) {
                    double ak = A[a][k], bk = A[b][k];
                    A[a][k] = c * ak - s * bk, A[b][k] = s * ak + c * bk;
                }
                for (int k = 0; k < 3; k++) {
                    double ka = Q[k][a], kb = Q[k][b];
                    Q[k][a] = c * ka - s * kb, Q[k][b] = s * ka + c * kb;
                }
            }
    }
    for (int i = 0; i < 3; i++) l[i] = A[i][i];
}

typedef struct {
    Sph *p;
    const double *x, *v, *rho, *e, *S; /* the state being differentiated */
    double *a, *drho, *de, *dS;
    double dtmin[64];
} EvalCtx;

/* pressure, sound speed, stress and artificial stress of every real particle */
static void state_chunk(void *vc, int b, int e, int tid) {
    (void)tid;
    EvalCtx *E = vc;
    Sph *p = E->p;
    for (int i = b; i < e; i++) {
        const ImMaterial *M = &p->spec.materials[p->mat[i]];
        double rho = E->rho[i] > 1e-3 * M->rho0 ? E->rho[i] : 1e-3 * M->rho0;
        double mu = rho / M->rho0 - 1, Ev = M->rho0 * E->e[i];
        double pr = im_mg_pressure(M, mu, Ev);
        const double dm = 1e-6;
        double dpdr = (im_mg_pressure(M, mu + dm, Ev) - im_mg_pressure(M, mu - dm, Ev)) / (2 * dm * M->rho0);
        double c2 = dpdr + pr / (rho * rho) * M->gamma0 * M->rho0;
        if (c2 < 0.25 * M->c0 * M->c0) c2 = 0.25 * M->c0 * M->c0;
        bool solid = !p->spec.hydro && temperature(M, p->wp[i]) < M->T_melt;
        if (solid) c2 += 4 * M->shear_modulus / (3 * rho);
        p->P[i] = pr, p->C[i] = sqrt(c2), p->RHO[i] = rho, p->M[i] = p->m[i], p->FB[i] = p->spec.no_balsara ? 1 : p->fb[i];
        p->pout[i] = pr;
        for (int a = 0; a < 3; a++) p->V[3 * (size_t)i + a] = E->v[3 * (size_t)i + a], p->X[3 * (size_t)i + a] = E->x[3 * (size_t)i + a];
        double *sg = p->SIG + 6 * (size_t)i, *R = p->R + 6 * (size_t)i;
        for (int c = 0; c < 6; c++) sg[c] = solid ? E->S[6 * (size_t)i + c] : 0;
        sg[XX] -= pr, sg[YY] -= pr, sg[ZZ] -= pr;
        for (int c = 0; c < 6; c++) R[c] = 0;
        if (p->spec.art_stress > 0) {
            double l[3], Q[3][3];
            eigen_sym(sg, l, Q);
            double r[3];
            bool any = false;
            for (int k = 0; k < 3; k++) r[k] = l[k] > 0 ? -p->spec.art_stress * l[k] / (rho * rho) : 0, any = any || l[k] > 0;
            if (any) {
                double T[3][3];
                for (int a = 0; a < 3; a++)
                    for (int c = 0; c < 3; c++) T[a][c] = Q[a][0] * r[0] * Q[c][0] + Q[a][1] * r[1] * Q[c][1] + Q[a][2] * r[2] * Q[c][2];
                R[XX] = T[0][0], R[YY] = T[1][1], R[ZZ] = T[2][2], R[XY] = T[0][1], R[YZ] = T[1][2], R[XZ] = T[0][2];
            }
        }
    }
}

static void ghost_chunk(void *vc, int b, int e, int tid) {
    (void)tid;
    EvalCtx *E = vc;
    Sph *p = E->p;
    for (int g = b; g < e; g++) {
        int j = p->n + g, s = p->gsrc[g];
        unsigned mk = p->gmask[g];
        image_position(p, g, E->x, p->X + 3 * (size_t)j);
        for (int a = 0; a < 3; a++) p->V[3 * (size_t)j + a] = sgn(mk, a) * E->v[3 * (size_t)s + a];
        p->RHO[j] = p->RHO[s], p->P[j] = p->P[s], p->C[j] = p->C[s], p->M[j] = p->M[s], p->FB[j] = p->FB[s];
        mirror_tensor(mk, p->SIG + 6 * (size_t)s, p->SIG + 6 * (size_t)j);
        mirror_tensor(mk, p->R + 6 * (size_t)s, p->R + 6 * (size_t)j);
    }
}

static inline void tdot(const double *T, const double g[3], double o[3]) {
    o[0] = T[XX] * g[0] + T[XY] * g[1] + T[XZ] * g[2];
    o[1] = T[XY] * g[0] + T[YY] * g[1] + T[YZ] * g[2];
    o[2] = T[XZ] * g[0] + T[YZ] * g[1] + T[ZZ] * g[2];
}

static void deriv_chunk(void *vc, int b, int e, int tid) {
    EvalCtx *E = vc;
    Sph *p = E->p;
    const double h = p->h, sk = p->sigma_k, alpha = p->spec.av_alpha, beta = p->spec.av_beta, wdx = p->w_dx;
    double dtmin = 1e30;
    for (int i = b; i < e; i++) {
        const double *xi = p->X + 3 * (size_t)i, *vi = p->V + 3 * (size_t)i, *si = p->SIG + 6 * (size_t)i, *Ri = p->R + 6 * (size_t)i;
        const double ri = p->RHO[i], ci = p->C[i];
        double acc[3] = {0, 0, 0}, dr = 0, de = 0, L[3][3] = {{0}}, Mg[3][3] = {{0}};
        double vsig = 0;
        for (int k = p->nstart[i]; k < p->nstart[i + 1]; k++) {
            int j = p->nlist[k];
            const double *xj = p->X + 3 * (size_t)j, *vj = p->V + 3 * (size_t)j;
            double r[3] = {xi[0] - xj[0], xi[1] - xj[1], xi[2] - xj[2]};
            double d2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
            if (d2 >= 4 * h * h || d2 == 0) continue;
            double d = sqrt(d2), q = d / h, dW, W;
            if (q < 1) W = sk * (1 - 1.5 * q * q + 0.75 * q * q * q), dW = sk / h * (-3 * q + 2.25 * q * q);
            else W = sk * 0.25 * (2 - q) * (2 - q) * (2 - q), dW = -sk / h * 0.75 * (2 - q) * (2 - q);
            double gw[3] = {dW * r[0] / d, dW * r[1] / d, dW * r[2] / d};
            double vij[3] = {vi[0] - vj[0], vi[1] - vj[1], vi[2] - vj[2]};
            double mj = p->M[j], rj = p->RHO[j];
            double vg = vij[0] * gw[0] + vij[1] * gw[1] + vij[2] * gw[2];
            dr += mj * vg;
            for (int a = 0; a < 3; a++)
                for (int c = 0; c < 3; c++) L[a][c] -= mj / rj * vij[a] * gw[c], Mg[a][c] -= mj / rj * r[a] * gw[c];
            double vr = vij[0] * r[0] + vij[1] * r[1] + vij[2] * r[2];
            double Pi = 0;
            if (vr < 0) {
                double mu = h * vr / (d2 + 0.01 * h * h), cb = 0.5 * (ci + p->C[j]), rb = 0.5 * (ri + rj);
                Pi = 0.5 * (p->FB[i] + p->FB[j]) * (-alpha * cb * mu + beta * mu * mu) / rb;
                if (-mu > vsig) vsig = -mu;
            }
            double T[6];
            const double *sj = p->SIG + 6 * (size_t)j, *Rj = p->R + 6 * (size_t)j;
            bool pull_wall = j >= p->n && p->gwall[j - p->n];
            double ii = 1 / (ri * ri), jj = 1 / (rj * rj);
            double f = W / wdx, f4 = f * f * f * f;
            for (int c = 0; c < 6; c++) T[c] = si[c] * ii + sj[c] * jj + (Ri[c] + Rj[c]) * f4;
            if (pull_wall) {
                /* a wall pushes but does not pull: the pair's stress acts only while it presses the particle away */
                double t[3];
                tdot(T, gw, t);
                if (t[0] * r[0] + t[1] * r[1] + t[2] * r[2] < 0)
                    for (int c = 0; c < 6; c++) T[c] = 0;
            }
            T[XX] -= Pi, T[YY] -= Pi, T[ZZ] -= Pi;
            double tg[3];
            tdot(T, gw, tg);
            acc[0] += mj * tg[0], acc[1] += mj * tg[1], acc[2] += mj * tg[2];
            de -= 0.5 * mj * (vij[0] * tg[0] + vij[1] * tg[1] + vij[2] * tg[2]);
        }
        for (int a = 0; a < 3; a++) E->a[3 * (size_t)i + a] = acc[a];
        E->drho[i] = dr, E->de[i] = de;
        if (p->spec.correct_gradient) { /* L <- L M^-1, with M the same sum for the position field (the identity if exact) */
            double det = Mg[0][0] * (Mg[1][1] * Mg[2][2] - Mg[1][2] * Mg[2][1]) - Mg[0][1] * (Mg[1][0] * Mg[2][2] - Mg[1][2] * Mg[2][0]) +
                         Mg[0][2] * (Mg[1][0] * Mg[2][1] - Mg[1][1] * Mg[2][0]);
            if (fabs(det) > 1e-3) { /* a particle with too few neighbours keeps the plain estimate */
                double I[3][3], Lc[3][3];
                I[0][0] = (Mg[1][1] * Mg[2][2] - Mg[1][2] * Mg[2][1]) / det, I[0][1] = (Mg[0][2] * Mg[2][1] - Mg[0][1] * Mg[2][2]) / det;
                I[0][2] = (Mg[0][1] * Mg[1][2] - Mg[0][2] * Mg[1][1]) / det, I[1][0] = (Mg[1][2] * Mg[2][0] - Mg[1][0] * Mg[2][2]) / det;
                I[1][1] = (Mg[0][0] * Mg[2][2] - Mg[0][2] * Mg[2][0]) / det, I[1][2] = (Mg[0][2] * Mg[1][0] - Mg[0][0] * Mg[1][2]) / det;
                I[2][0] = (Mg[1][0] * Mg[2][1] - Mg[1][1] * Mg[2][0]) / det, I[2][1] = (Mg[0][1] * Mg[2][0] - Mg[0][0] * Mg[2][1]) / det;
                I[2][2] = (Mg[0][0] * Mg[1][1] - Mg[0][1] * Mg[1][0]) / det;
                for (int a = 0; a < 3; a++)
                    for (int c = 0; c < 3; c++) Lc[a][c] = L[a][0] * I[0][c] + L[a][1] * I[1][c] + L[a][2] * I[2][c];
                memcpy(L, Lc, sizeof L);
            }
        }
        /* deviatoric stress rate: Jaumann, from the SPH velocity gradient L[a][c] = d v_a / d x_c */
        const ImMaterial *M = &p->spec.materials[p->mat[i]];
        double D[3][3], Wr[3][3];
        for (int a = 0; a < 3; a++)
            for (int c = 0; c < 3; c++) D[a][c] = 0.5 * (L[a][c] + L[c][a]), Wr[a][c] = 0.5 * (L[a][c] - L[c][a]);
        double trD = D[0][0] + D[1][1] + D[2][2], dd = 0;
        for (int a = 0; a < 3; a++)
            for (int c = 0; c < 3; c++) {
                double dv = D[a][c] - (a == c ? trD / 3 : 0);
                dd += dv * dv;
            }
        p->rate[i] = sqrt(2.0 / 3.0 * dd);
        { /* Balsara's factor for the next evaluation */
            double dv = fabs(L[0][0] + L[1][1] + L[2][2]);
            double cx = L[2][1] - L[1][2], cy = L[0][2] - L[2][0], cz = L[1][0] - L[0][1], cv = sqrt(cx * cx + cy * cy + cz * cz);
            p->fb[i] = dv / (dv + cv + 1e-4 * ci / h);
        }
        double *dS = E->dS + 6 * (size_t)i;
        if (p->spec.hydro) {
            for (int c = 0; c < 6; c++) dS[c] = 0;
        } else {
            const double *sv = E->S + 6 * (size_t)i;
            double s[3][3] = {{sv[XX], sv[XY], sv[XZ]}, {sv[XY], sv[YY], sv[YZ]}, {sv[XZ], sv[YZ], sv[ZZ]}}, o[3][3];
            double G = M->shear_modulus;
            for (int a = 0; a < 3; a++)
                for (int c = 0; c < 3; c++) {
                    double ws = 0, sw = 0;
                    for (int k = 0; k < 3; k++) ws += Wr[a][k] * s[k][c], sw += s[a][k] * Wr[k][c];
                    o[a][c] = 2 * G * (D[a][c] - (a == c ? trD / 3 : 0)) + ws - sw;
                }
            dS[XX] = o[0][0], dS[YY] = o[1][1], dS[ZZ] = o[2][2], dS[XY] = o[0][1], dS[YZ] = o[1][2], dS[XZ] = o[0][2];
        }
        /* stable step: the signal speed across a smoothing length, the acceleration, and a motion within the step that
         * keeps every pair inside the neighbour search radius */
        double am = sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
        double dti = p->spec.safety * h / (ci + 1.2 * (alpha * ci + beta * vsig));
        if (am > 0) dti = fmin(dti, 0.25 * sqrt(h / am));
        if (!(dti > 0) || !isfinite(acc[0] + acc[1] + acc[2] + dr + de)) dti = 0; /* flagged below */
        if (dti < dtmin) dtmin = dti;
    }
    if (dtmin < E->dtmin[tid]) E->dtmin[tid] = dtmin;
}

/* derivatives of the given state; returns the stable step it allows */
static double evaluate(Sph *p, const double *x, const double *v, const double *rho, const double *e, const double *S, double *a, double *drho,
                       double *de, double *dS) {
    EvalCtx E = {p, x, v, rho, e, S, a, drho, de, dS, {0}};
    for (int t = 0; t < 64; t++) E.dtmin[t] = 1e30;
    run(p, p->n, 512, state_chunk, &E);
    run(p, p->ng, 1024, ghost_chunk, &E);
    run(p, p->n, 128, deriv_chunk, &E);
    double dt = 1e30;
    for (int t = 0; t < 64; t++) dt = fmin(dt, E.dtmin[t]);
    return dt;
}

/* Johnson-Cook radial return of the deviator of particle i; commit: keep the plastic strain and work */
static void plastic_return(Sph *p, int i, double *S, double dt, bool commit) {
    if (p->spec.hydro) return;
    const ImMaterial *M = &p->spec.materials[p->mat[i]];
    double *s = S + 6 * (size_t)i;
    double J2 = s[XX] * s[XX] + s[YY] * s[YY] + s[ZZ] * s[ZZ] + 2 * (s[XY] * s[XY] + s[YZ] * s[YZ] + s[XZ] * s[XZ]);
    double seq = sqrt(1.5 * J2), T = temperature(M, p->wp[i]), G = M->shear_modulus;
    if (T >= M->T_melt) {
        for (int c = 0; c < 6; c++) s[c] = 0;
        return;
    }
    double sy0 = im_jc_flow(M, p->ep[i], 0, T);
    if (seq <= sy0) return;
    double lo = 0, hi = seq / (3 * G);
    for (int it = 0; it < 60; it++) {
        double mid = 0.5 * (lo + hi);
        double r = seq - 3 * G * mid - im_jc_flow(M, p->ep[i] + mid, mid / dt, T);
        if (r > 0) lo = mid;
        else hi = mid;
    }
    double dep = 0.5 * (lo + hi), sy = seq - 3 * G * dep;
    double k = sy > 0 ? sy / seq : 0;
    for (int c = 0; c < 6; c++) s[c] *= k;
    if (commit) {
        p->ep[i] += dep;
        p->wp[i] += sy * dep / p->rho[i];
    }
}

double sph_step(Sph *p) {
    if (p->unstable || p->n == 0) return 0;
    int n = p->n;
    build_ghosts(p);
    build_neighbours(p);
    memcpy(p->x0, p->x, (size_t)n * 3 * sizeof(double));
    memcpy(p->v0, p->v, (size_t)n * 3 * sizeof(double));
    memcpy(p->rho0, p->rho, (size_t)n * sizeof(double));
    memcpy(p->e0, p->e, (size_t)n * sizeof(double));
    memcpy(p->S0, p->S, (size_t)n * 6 * sizeof(double));
    double dt = evaluate(p, p->x, p->v, p->rho, p->e, p->S, p->a1, p->dr1, p->de1, p->dS1);
    /* no pair may move by more than 0.2 h relative to another within the step (the search radius margin) */
    double vmax = 0;
    for (int i = 0; i < n; i++) {
        const double *v = p->v + 3 * (size_t)i;
        vmax = fmax(vmax, sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
    }
    if (vmax > 0) dt = fmin(dt, 0.1 * p->h / vmax);
    if (!(dt > 0) || !isfinite(dt)) {
        p->unstable = true;
        return 0;
    }
    /* predictor */
    for (int i = 0; i < n; i++) {
        for (int a = 0; a < 3; a++) {
            size_t k = 3 * (size_t)i + a;
            p->x[k] = p->x0[k] + dt * p->v0[k];
            p->v[k] = p->v0[k] + dt * p->a1[k];
        }
        p->rho[i] = p->rho0[i] + dt * p->dr1[i];
        p->e[i] = p->e0[i] + dt * p->de1[i];
        for (int c = 0; c < 6; c++) p->S[6 * (size_t)i + c] = p->S0[6 * (size_t)i + c] + dt * p->dS1[6 * (size_t)i + c];
        plastic_return(p, i, p->S, dt, false);
    }
    evaluate(p, p->x, p->v, p->rho, p->e, p->S, p->a2, p->dr2, p->de2, p->dS2);
    /* corrector */
    for (int i = 0; i < n; i++) {
        for (int a = 0; a < 3; a++) {
            size_t k = 3 * (size_t)i + a;
            double vp = p->v[k];
            p->v[k] = p->v0[k] + 0.5 * dt * (p->a1[k] + p->a2[k]);
            p->x[k] = p->x0[k] + 0.5 * dt * (p->v0[k] + vp);
        }
        p->rho[i] = p->rho0[i] + 0.5 * dt * (p->dr1[i] + p->dr2[i]);
        p->e[i] = p->e0[i] + 0.5 * dt * (p->de1[i] + p->de2[i]);
        for (int c = 0; c < 6; c++) {
            size_t k = 6 * (size_t)i + c;
            p->S[k] = p->S0[k] + 0.5 * dt * (p->dS1[k] + p->dS2[k]);
        }
        plastic_return(p, i, p->S, dt, true);
        if (!isfinite(p->x[3 * (size_t)i] + p->v[3 * (size_t)i] + p->rho[i] + p->e[i])) p->unstable = true;
    }
    p->t += dt, p->dt = dt, p->steps++;
    return dt;
}

double sph_time(const Sph *p) { return p->t; }
long sph_steps(const Sph *p) { return p->steps; }
int sph_count(const Sph *p) { return p->n; }
int sph_bodies(const Sph *p) { return p->nbodies; }
bool sph_unstable(const Sph *p) { return p->unstable; }
double sph_body_mass(const Sph *p, int b) { return (b >= 0 && b < p->nbodies) ? p->body_mass[b] : 0; }

void sph_energy(const Sph *p, double *kin, double *inte) {
    double k = 0, u = 0;
    for (int i = 0; i < p->n; i++) {
        const double *v = p->v + 3 * (size_t)i;
        k += 0.5 * p->m[i] * (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        u += p->m[i] * p->e[i];
    }
    *kin = k, *inte = u;
}

void sph_momentum(const Sph *p, int body, double mom[3]) {
    mom[0] = mom[1] = mom[2] = 0;
    for (int i = 0; i < p->n; i++) {
        if (body >= 0 && p->body[i] != body) continue;
        for (int a = 0; a < 3; a++) mom[a] += p->m[i] * p->v[3 * (size_t)i + a];
    }
}

const double *sph_positions(const Sph *p) { return p->x; }
const double *sph_velocities(const Sph *p) { return p->v; }
const double *sph_density(const Sph *p) { return p->rho; }
const double *sph_pressure(const Sph *p) { return p->pout; }
const double *sph_plastic_strain(const Sph *p) { return p->ep; }
const int *sph_body(const Sph *p) { return p->body; }
const double *sph_temperature(const Sph *p) {
    for (int i = 0; i < p->n; i++) ((Sph *)p)->Tout[i] = temperature(&p->spec.materials[p->mat[i]], p->wp[i]);
    return p->Tout;
}

void sph_write_frame(Sph *p, LabWriter *w, bool mirror) {
    int n = p->n, copies = 1;
    unsigned masks[8] = {0};
    double offs[8][3] = {{0}};
    if (mirror)
        for (int k = 0; k < p->spec.nplanes; k++) {
            const SphPlane *P = &p->spec.planes[k];
            if (P->wall || copies >= 8) continue;
            for (int c = 0; c < copies; c++) {
                masks[copies + c] = masks[c] ^ (1u << P->axis);
                for (int a = 0; a < 3; a++) offs[copies + c][a] = offs[c][a];
                offs[copies + c][P->axis] = 2 * P->pos - offs[c][P->axis];
            }
            copies *= 2;
        }
    size_t N = (size_t)n * copies;
    double *xyz = malloc(N * 3 * sizeof(double));
    float *f = malloc(N * sizeof(float));
    for (int c = 0; c < copies; c++)
        for (int i = 0; i < n; i++)
            for (int a = 0; a < 3; a++)
                xyz[3 * ((size_t)c * n + i) + a] = sgn(masks[c], a) * p->x[3 * (size_t)i + a] + offs[c][a];
    lab_part_points(w, "particles", (int)N, xyz);
    const double *T = sph_temperature(p);
    for (int fld = 0; fld < 6; fld++) {
        for (int c = 0; c < copies; c++)
            for (int i = 0; i < n; i++) {
                const double *v = p->v + 3 * (size_t)i;
                double val = fld == 0 ? sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
                           : fld == 1 ? p->pout[i]
                           : fld == 2 ? p->rho[i]
                           : fld == 3 ? p->ep[i]
                           : fld == 4 ? T[i]
                                      : p->body[i];
                f[(size_t)c * n + i] = (float)val;
            }
        static const char *names[] = {"speed", "p", "rho", "ep", "T", "body"};
        lab_field(w, names[fld], LAB_AT_NODE, N, f);
    }
    free(xyz), free(f);
}

char *sph_header_json(const SphSpec *s, const char *title) {
    char *o = malloc(1024);
    snprintf(o, 1024,
             "{\"domain\":\"impact\",\"title\":\"%s\",\"solver\":\"src/lab/impact/sph: smoothed particle hydrodynamics, cubic spline, "
             "Mie-Gruneisen, Johnson-Cook, Monaghan viscosity, Gray-Monaghan-Swift artificial stress, Heun\",\"spacing_m\":%.6g,"
             "\"h_factor\":%.4g,\"fields\":{\"speed\":\"m/s\",\"p\":\"Pa\",\"rho\":\"kg/m^3\",\"ep\":\"1\",\"T\":\"K\",\"body\":\"1\"}}",
             title, s->dx, s->h_factor);
    return o;
}
