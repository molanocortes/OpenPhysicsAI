/* lbm.c - D3Q19 lattice Boltzmann kernel (fused stream-collide, regularized BGK + Smagorinsky LES) */
#include "lbm.h"
#include "common.h"

/* Velocity set: 0 rest, then opposite pairs (odd, even). */
static const int CX[LBM_Q] = {0, 1, -1, 0, 0, 0, 0, 1, -1, 1, -1, 0, 0, 1, -1, 1, -1, 0, 0};
static const int CY[LBM_Q] = {0, 0, 0, 1, -1, 0, 0, 1, -1, 0, 0, 1, -1, -1, 1, 0, 0, 1, -1};
static const int CZ[LBM_Q] = {0, 0, 0, 0, 0, 1, -1, 0, 0, 1, -1, 1, -1, 0, 0, -1, 1, -1, 1};
static const int OPP[LBM_Q] = {0, 2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11, 14, 13, 16, 15, 18, 17};
static const int MIRY[LBM_Q] = {0, 1, 2, 4, 3, 5, 6, 13, 14, 9, 10, 18, 17, 7, 8, 15, 16, 12, 11};
static const int MIRZ[LBM_Q] = {0, 1, 2, 3, 4, 6, 5, 7, 8, 15, 16, 17, 18, 13, 14, 9, 10, 11, 12};

#define W0 (1.0f / 3.0f)
#define W1 (1.0f / 18.0f)
#define W2 (1.0f / 36.0f)
static const float W[LBM_Q] = {W0, W1, W1, W1, W1, W1, W1, W2, W2, W2, W2, W2, W2, W2, W2, W2, W2, W2, W2};

/* Directions that interior cells pull from each halo plane. */
static const int IN_XP[5] = {1, 7, 9, 13, 15}, IN_XM[5] = {2, 8, 10, 14, 16};
static const int IN_YP[5] = {3, 7, 11, 14, 17}, IN_YM[5] = {4, 8, 12, 13, 18};
static const int IN_ZP[5] = {5, 9, 11, 16, 18}, IN_ZM[5] = {6, 10, 12, 15, 17};

#define MAX_THREADS 64

static inline size_t pidx(const Lbm *L, int i, int j, int k) {
    return (size_t)i + (size_t)L->sx * ((size_t)j + (size_t)L->sy * (size_t)k);
}

static inline ptrdiff_t qoff(const Lbm *L, int q) {
    return (ptrdiff_t)CX[q] + (ptrdiff_t)L->sx * ((ptrdiff_t)CY[q] + (ptrdiff_t)L->sy * (ptrdiff_t)CZ[q]);
}

static inline float feq_shifted(int q, float rho, float ux, float uy, float uz) {
    const float cu = (float)CX[q] * ux + (float)CY[q] * uy + (float)CZ[q] * uz;
    const float usq = ux * ux + uy * uy + uz * uz;
    return W[q] * ((rho - 1.0f) + rho * (3.0f * cu + 4.5f * cu * cu - 1.5f * usq));
}

static void run_for(Lbm *L, int count, int grain, ParallelFn fn, void *ctx) {
    if (count <= 0) return;
    if (L->pool)
        pool_for(L->pool, count, grain, fn, ctx);
    else
        fn(ctx, 0, count, 0);
}

static uint32_t rng_next(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return *s = x;
}
static float rng_unit(uint32_t *s) { return (float)(rng_next(s) >> 8) / 16777216.0f; }

size_t lbm_bytes_estimate(int nx, int ny, int nz) {
    size_t npad = (size_t)(nx + 2) * (size_t)(ny + 2) * (size_t)(nz + 2);
    return npad * (2 * LBM_Q * sizeof(float) + 1 + 6 * sizeof(float));
}

void lbm_destroy(Lbm *L) {
    for (int q = 0; q < LBM_Q; q++) {
        mem_free_aligned(L->f[q]);
        mem_free_aligned(L->g[q]);
    }
    mem_free_aligned(L->flags);
    free(L->links);
    free(L->wall_idx);
    free(L->wall_n);
    free(L->wall_a);
    free(L->bsolid);
    free(L->link_delta);
    free(L->turb_y);
    free(L->turb_z);
    free(L->tau_x);
    free(L->sig_r);
    free(L->sig_u);
    for (int a = 0; a < 3; a++) mem_free_aligned(L->u[a]), mem_free_aligned(L->un[a]);
    memset(L, 0, sizeof *L);
}

bool lbm_create(Lbm *L, int nx, int ny, int nz, ThreadPool *pool) {
    memset(L, 0, sizeof *L);
    if (nx < 4 || ny < 4 || nz < 4) return false;
    L->nx = nx, L->ny = ny, L->nz = nz;
    L->sx = nx + 2, L->sy = ny + 2, L->sz = nz + 2;
    L->ncells = (size_t)nx * (size_t)ny * (size_t)nz;
    L->npad = (size_t)L->sx * (size_t)L->sy * (size_t)L->sz;
    if (L->npad >= ((size_t)1 << 27)) {
        LOGE("grid %dx%dx%d too large (limit 134M cells)", nx, ny, nz);
        return false;
    }
    if (pool && pool_size(pool) > MAX_THREADS) {
        LOGE("thread pool larger than %d threads", MAX_THREADS);
        return false;
    }
    L->pool = pool;
    for (int q = 0; q < LBM_Q; q++) {
        L->f[q] = mem_alloc_aligned(L->npad * sizeof(float));
        L->g[q] = mem_alloc_aligned(L->npad * sizeof(float));
        if (!L->f[q] || !L->g[q]) {
            LOGE("out of memory allocating %.0f MB lattice", (double)lbm_bytes_estimate(nx, ny, nz) / 1048576.0);
            lbm_destroy(L);
            return false;
        }
    }
    L->flags = mem_alloc_aligned(L->npad);
    L->turb_y = calloc((size_t)LBM_TURB_MODES * (size_t)L->sy, sizeof(float));
    L->turb_z = calloc((size_t)LBM_TURB_MODES * (size_t)L->sz, sizeof(float));
    L->tau_x = calloc((size_t)L->sx, sizeof(float));
    L->sig_r = calloc((size_t)L->sx, sizeof(float));
    L->sig_u = calloc((size_t)L->sx, sizeof(float));
    if (!L->flags || !L->turb_y || !L->turb_z || !L->tau_x || !L->sig_r || !L->sig_u) {
        lbm_destroy(L);
        return false;
    }

    /* synthetic inflow turbulence: random separable Fourier modes, normalised to unit RMS per component */
    uint32_t seed = 0x9E3779B9u;
    double sumsq[3] = {0, 0, 0};
    for (int m = 0; m < LBM_TURB_MODES; m++) {
        float ly = 6.0f + 40.0f * rng_unit(&seed), lz = 6.0f + 40.0f * rng_unit(&seed);
        L->turb_k[m][0] = 2.0f * (float)M_PI / ly;
        L->turb_k[m][1] = 2.0f * (float)M_PI / lz;
        L->turb_w[m] = 8.0f + 40.0f * rng_unit(&seed);
        L->turb_phase[m][0] = 2.0f * (float)M_PI * rng_unit(&seed);
        L->turb_phase[m][1] = 2.0f * (float)M_PI * rng_unit(&seed);
        for (int c = 0; c < 3; c++) {
            L->turb_amp[m][c] = 2.0f * rng_unit(&seed) - 1.0f;
            sumsq[c] += (double)(L->turb_amp[m][c] * L->turb_amp[m][c]);
        }
    }
    for (int m = 0; m < LBM_TURB_MODES; m++)
        for (int c = 0; c < 3; c++) L->turb_amp[m][c] *= (float)(2.0 / sqrt(sumsq[c] > 1e-12 ? sumsq[c] : 1.0));
    return true;
}

/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    Lbm *L;
    float ux;
    const float *profile; /* per interior row, or NULL */
} ResetCtx;

static void reset_chunk(void *vc, int kb, int ke, int tid) {
    (void)tid;
    ResetCtx *C = vc;
    Lbm *L = C->L;
    float feq[LBM_Q];
    for (int q = 0; q < LBM_Q; q++) feq[q] = feq_shifted(q, 1.0f, C->ux, 0, 0);
    const size_t plane = (size_t)L->sx * (size_t)L->sy;
    for (int k = kb; k < ke; k++) {
        for (size_t n = (size_t)k * plane; n < ((size_t)k + 1) * plane; n++) {
            const bool solid = L->flags[n] != 0;
            if (C->profile && (n - (size_t)k * plane) % (size_t)L->sx == 0) { /* a new row: its own velocity */
                const int j = (int)((n - (size_t)k * plane) / (size_t)L->sx);
                const float u = C->ux * C->profile[CLAMP(j, 1, L->ny) - 1];
                for (int q = 0; q < LBM_Q; q++) feq[q] = feq_shifted(q, 1.0f, u, 0, 0);
            }
            for (int q = 0; q < LBM_Q; q++) {
                const float v = solid ? 0.0f : feq[q];
                L->f[q][n] = v;
                L->g[q][n] = v;
            }
        }
    }
}

void lbm_reset(Lbm *L, float ux) { lbm_reset_profile(L, ux, NULL); }

void lbm_reset_profile(Lbm *L, float ux, const float *profile) {
    ResetCtx c = {L, ux, profile};
    run_for(L, L->sz, 1, reset_chunk, &c);
    L->step = 0;
    L->have_u = false;
    L->force[0] = L->force[1] = L->force[2] = 0;
    L->unstable = false;
    L->umax = fabs(ux);
    L->rho_min = L->rho_max = 1.0;
}

static void *grow(void *p, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) return p;
    size_t nc = *cap ? *cap : 4096;
    while (nc < need) nc *= 2;
    void *np = realloc(p, nc * elem);
    if (!np) {
        LOGE("out of memory building boundary lists");
        abort();
    }
    *cap = nc;
    return np;
}

static void rebuild_lists(Lbm *L) {
    free(L->links), free(L->wall_idx), free(L->wall_n), free(L->wall_a), free(L->bsolid), free(L->link_delta);
    L->links = NULL, L->wall_idx = NULL, L->wall_n = NULL, L->wall_a = NULL, L->bsolid = NULL, L->link_delta = NULL;
    L->nlinks = L->nwall = L->nbsolid = 0;
    size_t cap_l = 0, cap_w = 0, cap_wn = 0, cap_wa = 0, cap_b = 0;
    const int nx = L->nx, ny = L->ny, nz = L->nz;

    /* rows (j,k) that contain any solid, to skip fluid-only neighbourhoods quickly */
    uint8_t *row_solid = calloc((size_t)(ny + 2) * (size_t)(nz + 2), 1);
    uint64_t nsolid = 0;
    for (int k = 1; k <= nz; k++)
        for (int j = 1; j <= ny; j++) {
            size_t b = pidx(L, 0, j, k);
            for (int i = 1; i <= nx; i++)
                if (L->flags[b + (size_t)i]) {
                    row_solid[(size_t)j + (size_t)(ny + 2) * (size_t)k] = 1;
                    break;
                }
        }

    for (int k = 1; k <= nz; k++) {
        for (int j = 1; j <= ny; j++) {
            bool near = false;
            for (int dk = -1; dk <= 1 && !near; dk++)
                for (int dj = -1; dj <= 1; dj++)
                    if (row_solid[(size_t)(j + dj) + (size_t)(ny + 2) * (size_t)(k + dk)]) {
                        near = true;
                        break;
                    }
            if (!near) continue;
            for (int i = 1; i <= nx; i++) {
                const size_t n = pidx(L, i, j, k);
                if (L->flags[n]) {
                    nsolid++;
                    bool axis_fluid = false;
                    for (int q = 1; q < LBM_Q; q++) {
                        const int ii = i + CX[q], jj = j + CY[q];
                        int kk = k + CZ[q];
                        size_t ns = n;
                        if (L->periodic_z && (kk < 1 || kk > nz)) {
                            /* across the seam of a periodic span: the link belongs to the fluid node on the other
                             * side, and is stored from this solid cell's image in the halo, so that the bounce-back
                             * lands where that node pulls from and its momentum is counted in the force */
                            kk = kk < 1 ? kk + nz : kk - nz;
                            ns = pidx(L, i, j, kk - CZ[q]);
                        }
                        if (ii < 1 || ii > nx || jj < 1 || jj > ny || kk < 1 || kk > nz) continue;
                        if (L->flags[pidx(L, ii, jj, kk)]) continue;
                        L->links = grow(L->links, &cap_l, L->nlinks + 1, sizeof *L->links);
                        L->links[L->nlinks++] = (uint32_t)(ns << 5) | (uint32_t)q;
                        if (q <= 6) axis_fluid = true;
                    }
                    if (axis_fluid) {
                        L->bsolid = grow(L->bsolid, &cap_b, L->nbsolid + 1, sizeof *L->bsolid);
                        L->bsolid[L->nbsolid++] = (uint32_t)((i - 1) + nx * ((j - 1) + ny * (k - 1)));
                    }
                } else {
                    float sw = 0, vx = 0, vy = 0, vz = 0;
                    for (int q = 1; q < LBM_Q; q++) {
                        const int ii = i + CX[q], jj = j + CY[q], kk = k + CZ[q];
                        if (ii < 1 || ii > nx || jj < 1 || jj > ny || kk < 1 || kk > nz) continue;
                        if (!L->flags[pidx(L, ii, jj, kk)]) continue;
                        sw += W[q];
                        vx -= W[q] * (float)CX[q];
                        vy -= W[q] * (float)CY[q];
                        vz -= W[q] * (float)CZ[q];
                    }
                    if (sw > 0) {
                        size_t w = L->nwall;
                        L->wall_idx = grow(L->wall_idx, &cap_w, w + 1, sizeof *L->wall_idx);
                        L->wall_n = grow(L->wall_n, &cap_wn, 3 * (w + 1), sizeof *L->wall_n);
                        L->wall_a = grow(L->wall_a, &cap_wa, w + 1, sizeof *L->wall_a);
                        float len = sqrtf(vx * vx + vy * vy + vz * vz);
                        float inv = len > 1e-6f ? 1.0f / len : 0.0f;
                        L->wall_idx[w] = (uint32_t)n;
                        L->wall_n[3 * w] = vx * inv;
                        L->wall_n[3 * w + 1] = vy * inv;
                        L->wall_n[3 * w + 2] = vz * inv;
                        L->wall_a[w] = MINI(6.0f * sw, 2.0f);
                        L->nwall++;
                    }
                }
            }
        }
    }
    free(row_solid);
    L->solid_cells = nsolid;
}

void lbm_set_solids(Lbm *L, const uint8_t *solid) {
    const int nx = L->nx, ny = L->ny, nz = L->nz;
    size_t nfresh = 0, cap = 0;
    uint32_t *fresh = NULL;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++) {
            const size_t row = (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k);
            for (int i = 0; i < nx; i++) {
                const uint8_t s = solid[row + (size_t)i] ? 1 : 0;
                const size_t n = pidx(L, i + 1, j + 1, k + 1);
                if (L->flags[n] == s) continue;
                if (!s && L->step > 0) {
                    fresh = grow(fresh, &cap, nfresh + 1, sizeof *fresh);
                    fresh[nfresh++] = (uint32_t)n;
                }
                L->flags[n] = s;
                for (int q = 0; q < LBM_Q; q++) L->f[q][n] = L->g[q][n] = 0.0f;
                if (L->u[0]) L->u[0][n] = L->u[1][n] = L->u[2][n] = 0.0f;
            }
        }
    /* cells uncovered by a moving model take the mass-weighted mean state of their established fluid neighbours
     * instead of starting at rest, which would launch a pressure pulse from every one of them */
    uint8_t *mark = nfresh ? calloc(L->npad, 1) : NULL;
    if (mark) {
        for (size_t m = 0; m < nfresh; m++) mark[fresh[m]] = 1;
        const size_t sxy = (size_t)L->sx * (size_t)L->sy;
        for (size_t m = 0; m < nfresh; m++) {
            const size_t n = fresh[m];
            const int i = (int)(n % (size_t)L->sx), j = (int)((n / (size_t)L->sx) % (size_t)L->sy), k = (int)(n / sxy);
            double r = 0, jx = 0, jy = 0, jz = 0;
            int cnt = 0;
            for (int q = 1; q < LBM_Q; q++) {
                const int ii = i + CX[q], jj = j + CY[q], kk = k + CZ[q];
                if (ii < 1 || ii > nx || jj < 1 || jj > ny || kk < 1 || kk > nz) continue;
                const size_t nb = pidx(L, ii, jj, kk);
                if (L->flags[nb] || mark[nb]) continue;
                double rr = 1.0;
                for (int p = 0; p < LBM_Q; p++) {
                    const double v = L->f[p][nb];
                    rr += v, jx += CX[p] * v, jy += CY[p] * v, jz += CZ[p] * v;
                }
                r += rr;
                cnt++;
            }
            if (!cnt) continue;
            const float rho = (float)(r / cnt), ux = (float)(jx / r), uy = (float)(jy / r), uz = (float)(jz / r);
            for (int q = 0; q < LBM_Q; q++) L->f[q][n] = L->g[q][n] = feq_shifted(q, rho, ux, uy, uz);
            if (L->u[0]) L->u[0][n] = ux, L->u[1][n] = uy, L->u[2][n] = uz;
        }
    }
    free(mark);
    free(fresh);
    rebuild_lists(L);
}

void lbm_cell_moments(const Lbm *L, int i, int j, int k, float *rho, float *ux, float *uy, float *uz) {
    const size_t n = pidx(L, i + 1, j + 1, k + 1);
    float r = 0, jx = 0, jy = 0, jz = 0;
    for (int q = 0; q < LBM_Q; q++) {
        const float v = L->f[q][n];
        r += v, jx += (float)CX[q] * v, jy += (float)CY[q] * v, jz += (float)CZ[q] * v;
    }
    r += 1.0f;
    *rho = r;
    *ux = jx / r, *uy = jy / r, *uz = jz / r;
}

void lbm_link_segment(const Lbm *L, size_t n, float seg[6]) {
    const uint32_t v = L->links[n];
    const size_t s = v >> 5;
    const int q = (int)(v & 31u);
    const size_t sxy = (size_t)L->sx * (size_t)L->sy;
    /* padded index i is interior cell i - 1, whose centre sits at i - 0.5 */
    const float cx = (float)(s % (size_t)L->sx) - 0.5f, cy = (float)((s / (size_t)L->sx) % (size_t)L->sy) - 0.5f;
    const float cz = (float)(s / sxy) - 0.5f;
    seg[0] = cx + (float)CX[q], seg[1] = cy + (float)CY[q], seg[2] = cz + (float)CZ[q];
    seg[3] = (float)-CX[q], seg[4] = (float)-CY[q], seg[5] = (float)-CZ[q];
}

void lbm_set_link_deltas(Lbm *L, float *delta) {
    free(L->link_delta);
    L->link_delta = delta;
    if (!delta) return;
    const size_t sx = (size_t)L->sx, sxy = sx * (size_t)L->sy;
    for (size_t n = 0; n < L->nlinks; n++) {
        float d = delta[n];
        d = d >= 0.001f ? (d <= 1.0f ? d : 1.0f) : 0.001f;
        if (d < 0.5f) {
            /* the short-wall stencil also reads the next node away from the wall: it must be interior fluid */
            const uint32_t v = L->links[n];
            const size_t s = v >> 5;
            const int q = (int)(v & 31u);
            const int i = (int)(s % sx) + 2 * CX[q], j = (int)((s / sx) % (size_t)L->sy) + 2 * CY[q];
            int k = (int)(s / sxy) + 2 * CZ[q];
            if (L->periodic_z) k = k < 1 ? k + L->nz : (k > L->nz ? k - L->nz : k); /* the span wraps: so does the stencil */
            if (i < 1 || i > L->nx || j < 1 || j > L->ny || k < 1 || k > L->nz || L->flags[pidx(L, i, j, k)]) d = 0.5f;
        }
        delta[n] = d;
    }
}

/* ---- boundary conditions ------------------------------------------------------------------------ */

/* one tunnel wall: each plane writes only its own halo row, so the planes run in parallel */
typedef struct {
    Lbm *L;
    const LbmConfig *c;
    const int *set, *mir;
    int nx, h, a, per, tmax, t0;
    bool yaxis;
    LbmWall type;
    float fs[5];
} WallCtx;

static void fill_wall_chunk(void *vctx, int begin, int end, int tid) {
    (void)tid;
    const WallCtx *w = (const WallCtx *)vctx;
    Lbm *L = w->L;
    for (int idx = begin; idx < end; idx++) {
        const int t = w->t0 + idx;
        for (int i = 1; i <= w->nx; i++) {
            const size_t hn = w->yaxis ? pidx(L, i, w->h, t) : pidx(L, i, t, w->h);
            for (int m = 0; m < 5; m++) {
                const int q = w->set[m];
                float v;
                switch (w->type) {
                case LBM_WALL_SLIP:
                    v = L->f[w->mir[q]][w->yaxis ? pidx(L, i, w->a, t) : pidx(L, i, t, w->a)];
                    break;
                case LBM_WALL_FREESTREAM:
                    v = w->fs[m];
                    break;
                case LBM_WALL_PERIODIC:
                    v = L->f[q][w->yaxis ? pidx(L, i, w->per, t) : pidx(L, i, t, w->per)];
                    break;
                default: {
                    const int ii = i + CX[q];
                    int tt = t + (w->yaxis ? CZ[q] : CY[q]);
                    tt = tt < 0 ? 0 : (tt > w->tmax ? w->tmax : tt);
                    v = L->f[OPP[q]][w->yaxis ? pidx(L, ii, w->a, tt) : pidx(L, ii, tt, w->a)];
                    if (w->type == LBM_WALL_MOVING) v += 6.0f * W[q] * (float)CX[q] * w->c->u_in;
                    break;
                }
                }
                L->f[q][hn] = v;
            }
        }
    }
}

static void fill_wall(Lbm *L, const LbmConfig *c, int side) {
    const bool yaxis = side < 2, lo = (side & 1) == 0;
    const int n_ax = yaxis ? L->ny : L->nz;
    const int t0 = yaxis ? 1 : 0, t1 = yaxis ? L->nz : L->sy - 1;
    WallCtx w = {L,
                 c,
                 yaxis ? (lo ? IN_YP : IN_YM) : (lo ? IN_ZP : IN_ZM),
                 yaxis ? MIRY : MIRZ,
                 L->nx,
                 lo ? 0 : n_ax + 1,
                 lo ? 1 : n_ax,
                 lo ? n_ax : 1,
                 yaxis ? L->sz - 1 : L->sy - 1,
                 t0,
                 yaxis,
                 c->wall[side],
                 {0, 0, 0, 0, 0}};
    for (int m = 0; m < 5; m++) w.fs[m] = feq_shifted(w.set[m], 1.0f, c->u_in, 0, 0);
    run_for(L, t1 - t0 + 1, 8, fill_wall_chunk, &w);
}

typedef struct {
    Lbm *L;
    const LbmConfig *c;
    bool turb;
} IoCtx;

static void inout_chunk(void *vc, int kb, int ke, int tid) {
    (void)tid;
    IoCtx *C = vc;
    Lbm *L = C->L;
    const LbmConfig *c = C->c;
    const int nx = L->nx, ny = L->ny, nz = L->nz, sy = L->sy, sz = L->sz;
    for (int k = kb; k < ke; k++) {
        const int kc = CLAMP(k, 1, nz);
        for (int j = 0; j < sy; j++) {
            const int jc = CLAMP(j, 1, ny);
            /* velocity inlet, density extrapolated from the first interior layer */
            const size_t nin = pidx(L, 1, jc, kc);
            float rho = 1.0f;
            if (!L->flags[nin]) {
                float r = 0;
                for (int q = 0; q < LBM_Q; q++) r += L->f[q][nin];
                rho = CLAMP(1.0f + r, 0.8f, 1.2f);
            }
            float ux = c->in_profile ? c->u_in * c->in_profile[jc - 1] : c->u_in, uy = c->v_in, uz = 0;
            if (C->turb) {
                float px = 0, py = 0, pz = 0;
                for (int m = 0; m < LBM_TURB_MODES; m++) {
                    const float s = L->turb_y[m * sy + j] * L->turb_z[m * sz + k];
                    px += L->turb_amp[m][0] * s;
                    py += L->turb_amp[m][1] * s;
                    pz += L->turb_amp[m][2] * s;
                }
                const float amp = c->turbulence * c->u_in;
                ux += amp * px, uy += amp * py, uz += amp * pz;
            }
            const size_t hin = pidx(L, 0, j, k);
            for (int m = 0; m < 5; m++) L->f[IN_XP[m]][hin] = feq_shifted(IN_XP[m], rho, ux, uy, uz);

            /* pressure outlet (rho = 1), velocity extrapolated, no backflow */
            const size_t nout = pidx(L, nx, jc, kc);
            float vx = 0, vy = 0, vz = 0;
            if (!L->flags[nout]) {
                float r = 0, jx = 0, jy = 0, jz = 0;
                for (int q = 0; q < LBM_Q; q++) {
                    const float v = L->f[q][nout];
                    r += v, jx += (float)CX[q] * v, jy += (float)CY[q] * v, jz += (float)CZ[q] * v;
                }
                r += 1.0f;
                vx = jx / r, vy = jy / r, vz = jz / r;
            }
            vx = CLAMP(vx, 0.0f, 0.4f);
            vy = CLAMP(vy, -0.4f, 0.4f);
            vz = CLAMP(vz, -0.4f, 0.4f);
            const size_t hout = pidx(L, nx + 1, j, k);
            for (int m = 0; m < 5; m++) L->f[IN_XM[m]][hout] = feq_shifted(IN_XM[m], 1.0f, vx, vy, vz);
        }
    }
}

typedef struct {
    Lbm *L;
    ptrdiff_t off[LBM_Q];
    double fs[MAX_THREADS][3];
} LinkCtx;

/* Bounce-back: write the reflected population into the solid cell so that the uniform pull stream delivers it to
 * the fluid node, and accumulate the momentum exchanged with the body (Mei et al. 2002 for interpolated walls).
 * Halfway without wall distances; with them, Bouzidi linear interpolation places the wall at fraction d of the link:
 *   d < 1/2:  f_q(x) = 2d f*_oq(x) + (1 - 2d) f*_oq(x + c_q)      (x + c_q: next fluid node away from the wall)
 *   d >= 1/2: f_q(x) = f*_oq(x) / 2d + (1 - 1/2d) f*_q(x)
 * Weights sum to one, so the formulas hold for the shifted populations as well. */
static void links_chunk(void *vc, int b, int e, int tid) {
    LinkCtx *C = vc;
    Lbm *L = C->L;
    const float *dl = L->link_delta;
    double fx = 0, fy = 0, fz = 0;
    for (int n = b; n < e; n++) {
        const uint32_t v = L->links[n];
        const size_t s = v >> 5;
        const int q = (int)(v & 31u), oq = OPP[q];
        const size_t x = (size_t)((ptrdiff_t)s + C->off[q]);
        const float fo = L->f[oq][x];
        float fn = fo;
        if (dl) {
            const float d = dl[n];
            if (d < 0.5f)
                fn = 2.0f * d * fo + (1.0f - 2.0f * d) * L->f[oq][(size_t)((ptrdiff_t)x + C->off[q])];
            else
                fn = (0.5f / d) * fo + (1.0f - 0.5f / d) * L->f[q][x];
        }
        L->f[q][s] = fn;
        const double mom = (double)fo + (double)fn + 2.0 * (double)W[oq];
        fx -= CX[q] * mom, fy -= CY[q] * mom, fz -= CZ[q] * mom;
    }
    C->fs[tid][0] += fx, C->fs[tid][1] += fy, C->fs[tid][2] += fz;
}

typedef struct {
    Lbm *L;
    float k;
    double fs[MAX_THREADS][3];
} RoughCtx;

/* Discrete-element roughness: drag of sub-grid roughness elements in the first fluid layer,
 * F = -K |u_t| u_t applied implicitly through an exact-difference (equilibrium shift) update. */
static void rough_chunk(void *vc, int b, int e, int tid) {
    RoughCtx *C = vc;
    Lbm *L = C->L;
    double fx = 0, fy = 0, fz = 0;
    for (int w = b; w < e; w++) {
        const size_t n = L->wall_idx[w];
        const float K = C->k * L->wall_a[w];
        float r = 0, jx = 0, jy = 0, jz = 0;
        for (int q = 0; q < LBM_Q; q++) {
            const float v = L->g[q][n];
            r += v, jx += (float)CX[q] * v, jy += (float)CY[q] * v, jz += (float)CZ[q] * v;
        }
        const float rho = 1.0f + r;
        const float ux = jx / rho, uy = jy / rho, uz = jz / rho;
        const float *nn = &L->wall_n[3 * w];
        const float un = ux * nn[0] + uy * nn[1] + uz * nn[2];
        const float tx = ux - un * nn[0], ty = uy - un * nn[1], tz = uz - un * nn[2];
        const float tm = sqrtf(tx * tx + ty * ty + tz * tz);
        const float s = K * tm;
        if (!(s > 1e-7f)) continue;
        const float fac = 1.0f / (1.0f + s) - 1.0f;
        const float dux = fac * tx, duy = fac * ty, duz = fac * tz;
        const float vx = ux + dux, vy = uy + duy, vz = uz + duz;
        for (int q = 0; q < LBM_Q; q++)
            L->g[q][n] += feq_shifted(q, rho, vx, vy, vz) - feq_shifted(q, rho, ux, uy, uz);
        fx -= (double)(rho * dux), fy -= (double)(rho * duy), fz -= (double)(rho * duz);
    }
    C->fs[tid][0] += fx, C->fs[tid][1] += fy, C->fs[tid][2] += fz;
}

/* ---- stream + collide --------------------------------------------------------------------------- */

typedef struct {
    Lbm *L;
    float cles, uin;
    float sigma, tau_bulk; /* hybrid weight of the population stress; bulk relaxation time floor */
    const LbmOutput *out;
    float umax2[MAX_THREADS], rmin[MAX_THREADS], rmax[MAX_THREADS];
    int bad[MAX_THREADS];
} SweepCtx;

static inline __attribute__((always_inline)) void sweep_impl(SweepCtx *C, int kb, int ke, int tid, const bool reg,
                                                             const bool out, const bool hrr, const bool rr3) {
    Lbm *const L = C->L;
    const int nx = L->nx, ny = L->ny;
    const ptrdiff_t sx = L->sx, sxy = (ptrdiff_t)L->sx * (ptrdiff_t)L->sy;
    const float *restrict tx = L->tau_x;
    const float *restrict sgr = L->sig_r;
    const float *restrict sgu = L->sig_u;
    const float cles = C->cles, uin = C->uin;
    const float sig = C->sigma, sig1 = 1.0f - C->sigma, taub_min = C->tau_bulk;
    float umax2 = 0.0f, rmin = 1e30f, rmax = -1e30f;
    int bad = 0;

    for (int k = kb + 1; k <= ke; k++) {
        for (int j = 1; j <= ny; j++) {
            const ptrdiff_t b = sx * j + sxy * k;
            const float *restrict s0 = L->f[0] + b;
            const float *restrict s1 = L->f[1] + b - 1;
            const float *restrict s2 = L->f[2] + b + 1;
            const float *restrict s3 = L->f[3] + b - sx;
            const float *restrict s4 = L->f[4] + b + sx;
            const float *restrict s5 = L->f[5] + b - sxy;
            const float *restrict s6 = L->f[6] + b + sxy;
            const float *restrict s7 = L->f[7] + b - 1 - sx;
            const float *restrict s8 = L->f[8] + b + 1 + sx;
            const float *restrict s9 = L->f[9] + b - 1 - sxy;
            const float *restrict s10 = L->f[10] + b + 1 + sxy;
            const float *restrict s11 = L->f[11] + b - sx - sxy;
            const float *restrict s12 = L->f[12] + b + sx + sxy;
            const float *restrict s13 = L->f[13] + b - 1 + sx;
            const float *restrict s14 = L->f[14] + b + 1 - sx;
            const float *restrict s15 = L->f[15] + b - 1 + sxy;
            const float *restrict s16 = L->f[16] + b + 1 - sxy;
            const float *restrict s17 = L->f[17] + b - sx + sxy;
            const float *restrict s18 = L->f[18] + b + sx - sxy;
            float *restrict d0 = L->g[0] + b, *restrict d1 = L->g[1] + b, *restrict d2 = L->g[2] + b;
            float *restrict d3 = L->g[3] + b, *restrict d4 = L->g[4] + b, *restrict d5 = L->g[5] + b;
            float *restrict d6 = L->g[6] + b, *restrict d7 = L->g[7] + b, *restrict d8 = L->g[8] + b;
            float *restrict d9 = L->g[9] + b, *restrict d10 = L->g[10] + b, *restrict d11 = L->g[11] + b;
            float *restrict d12 = L->g[12] + b, *restrict d13 = L->g[13] + b, *restrict d14 = L->g[14] + b;
            float *restrict d15 = L->g[15] + b, *restrict d16 = L->g[16] + b, *restrict d17 = L->g[17] + b;
            float *restrict d18 = L->g[18] + b;
            const uint8_t *restrict fl = L->flags + b;
            /* hybrid collision: rows of the previous step's velocity around this row, and this step's output */
            const float *restrict vxc = hrr ? L->u[0] + b : NULL, *restrict vyc = hrr ? L->u[1] + b : NULL;
            const float *restrict vzc = hrr ? L->u[2] + b : NULL;
            const float *restrict vxym = hrr ? L->u[0] + b - sx : NULL, *restrict vxyp = hrr ? L->u[0] + b + sx : NULL;
            const float *restrict vxzm = hrr ? L->u[0] + b - sxy : NULL, *restrict vxzp = hrr ? L->u[0] + b + sxy : NULL;
            const float *restrict vyym = hrr ? L->u[1] + b - sx : NULL, *restrict vyyp = hrr ? L->u[1] + b + sx : NULL;
            const float *restrict vyzm = hrr ? L->u[1] + b - sxy : NULL, *restrict vyzp = hrr ? L->u[1] + b + sxy : NULL;
            const float *restrict vzym = hrr ? L->u[2] + b - sx : NULL, *restrict vzyp = hrr ? L->u[2] + b + sx : NULL;
            const float *restrict vzzm = hrr ? L->u[2] + b - sxy : NULL, *restrict vzzp = hrr ? L->u[2] + b + sxy : NULL;
            float *restrict wux = hrr ? L->un[0] + b : NULL, *restrict wuy = hrr ? L->un[1] + b : NULL;
            float *restrict wuz = hrr ? L->un[2] + b : NULL;
            const ptrdiff_t ob = (ptrdiff_t)nx * ((ptrdiff_t)(j - 1) + (ptrdiff_t)ny * (ptrdiff_t)(k - 1)) - 1;
            float *restrict orho = out ? C->out->rho : NULL;
            float *restrict oux = out ? C->out->ux : NULL;
            float *restrict ouy = out ? C->out->uy : NULL;
            float *restrict ouz = out ? C->out->uz : NULL;
            float rsum = 0.0f;

            /* streams are disjoint (f/g/flags/output are separate allocations) */
            #pragma clang loop vectorize(assume_safety)
            for (int i = 1; i <= nx; i++) {
                const float f0 = s0[i], f1 = s1[i], f2 = s2[i], f3 = s3[i], f4 = s4[i], f5 = s5[i], f6 = s6[i];
                const float f7 = s7[i], f8 = s8[i], f9 = s9[i], f10 = s10[i], f11 = s11[i], f12 = s12[i];
                const float f13 = s13[i], f14 = s14[i], f15 = s15[i], f16 = s16[i], f17 = s17[i], f18 = s18[i];

                const float sxx = f1 + f2, syy = f3 + f4, szz = f5 + f6;
                const float a78 = f7 + f8, a910 = f9 + f10, a1112 = f11 + f12;
                const float a1314 = f13 + f14, a1516 = f15 + f16, a1718 = f17 + f18;
                const float rho1 = f0 + sxx + syy + szz + a78 + a910 + a1112 + a1314 + a1516 + a1718;
                const float rho = 1.0f + rho1;
                const float jx = (f1 - f2) + (f7 - f8) + (f9 - f10) + (f13 - f14) + (f15 - f16);
                const float jy = (f3 - f4) + (f7 - f8) + (f11 - f12) + (f14 - f13) + (f17 - f18);
                const float jz = (f5 - f6) + (f9 - f10) + (f11 - f12) + (f16 - f15) + (f18 - f17);
                const float inv = 1.0f / rho;
                const float ux = jx * inv, uy = jy * inv, uz = jz * inv;
                const float uxx = ux * ux, uyy = uy * uy, uzz = uz * uz;

                /* non-equilibrium stress Pi_neq = sum c c f - rho cs^2 I - rho u u  (shifted form) */
                const float r3 = rho1 * (1.0f / 3.0f);
                const float pxx = sxx + a78 + a910 + a1314 + a1516 - r3 - rho * uxx;
                const float pyy = syy + a78 + a1112 + a1314 + a1718 - r3 - rho * uyy;
                const float pzz = szz + a910 + a1112 + a1516 + a1718 - r3 - rho * uzz;
                const float pxy = a78 - a1314 - rho * ux * uy;
                const float pxz = a910 - a1516 - rho * ux * uz;
                const float pyz = a1112 - a1718 - rho * uy * uz;

                /* Smagorinsky on the deviatoric stress, so pressure (acoustic) noise doesn't raise the eddy viscosity:
                 * tau = (tau0 + sqrt(tau0^2 + 18 sqrt2 Cs^2 |Pi_dev| / rho)) / 2 */
                const float tr3 = (pxx + pyy + pzz) * (1.0f / 3.0f);
                const float qxx = pxx - tr3, qyy = pyy - tr3, qzz = pzz - tr3;
                const float pn = sqrtf(qxx * qxx + qyy * qyy + qzz * qzz + 2.0f * (pxy * pxy + pxz * pxz + pyz * pyz));
                const float t0 = tx[i];
                const float tau = 0.5f * (t0 + sqrtf(t0 * t0 + cles * pn * inv));
                const float om = 1.0f / tau;

                /* equilibrium moments; inside sponge layers they are relaxed toward the free stream */
                const float sr = sgr[i], su = sgu[i];
                const float er1 = rho1 * (1.0f - sr), er = 1.0f + er1;
                const float vx = ux + su * (uin - ux), vy = uy - su * uy, vz = uz - su * uz;
                const float vxx = vx * vx, vyy = vy * vy, vzz = vz * vz;
                const float vsq = 1.5f * (vxx + vyy + vzz);
                const float rw1 = er * W1, rw2 = er * W2;
                const float e0 = W0 * er1 - er * W0 * vsq;
                const float b1 = W1 * er1 - rw1 * vsq;
                const float b2 = W2 * er1 - rw2 * vsq;
                const float e1s = b1 + rw1 * 4.5f * vxx, e1a = rw1 * 3.0f * vx;
                const float e3s = b1 + rw1 * 4.5f * vyy, e3a = rw1 * 3.0f * vy;
                const float e5s = b1 + rw1 * 4.5f * vzz, e5a = rw1 * 3.0f * vz;
                const float cxy = vx + vy, cxz = vx + vz, cyz = vy + vz;
                const float dxy = vx - vy, dxz = vx - vz, dyz = vy - vz;
                const float e7s = b2 + rw2 * 4.5f * cxy * cxy, e7a = rw2 * 3.0f * cxy;
                const float e9s = b2 + rw2 * 4.5f * cxz * cxz, e9a = rw2 * 3.0f * cxz;
                const float e11s = b2 + rw2 * 4.5f * cyz * cyz, e11a = rw2 * 3.0f * cyz;
                const float e13s = b2 + rw2 * 4.5f * dxy * dxy, e13a = rw2 * 3.0f * dxy;
                const float e15s = b2 + rw2 * 4.5f * dxz * dxz, e15a = rw2 * 3.0f * dxz;
                const float e17s = b2 + rw2 * 4.5f * dyz * dyz, e17a = rw2 * 3.0f * dyz;

                float o0, o1, o2, o3, o4, o5, o6, o7, o8, o9, o10, o11, o12, o13, o14, o15, o16, o17, o18;
                if (reg) {
                    /* regularized: f = feq + relaxed f_neq^(1)(Pi), second-order Hermite projection.
                     * Hybrid (HRR, Jacob et al. 2018): Pi = sigma Pi_neq + (1 - sigma) Pi_FD, Pi_FD = -2 rho cs^2 tau S
                     * from centred differences of the previous step's velocity; it damps grid-scale modes that the
                     * population stress alone lets grow as tau -> 1/2. The deviatoric part relaxes at the shear
                     * rate and the trace at the bulk rate, which damps acoustic waves without touching shear. */
                    float mxx = pxx, myy = pyy, mzz = pzz, mxy = pxy, mxz = pxz, myz = pyz;
                    if (hrr) {
                        const float axx = 0.5f * (vxc[i + 1] - vxc[i - 1]), axy = 0.5f * (vxyp[i] - vxym[i]);
                        const float axz = 0.5f * (vxzp[i] - vxzm[i]), ayx = 0.5f * (vyc[i + 1] - vyc[i - 1]);
                        const float ayy = 0.5f * (vyyp[i] - vyym[i]), ayz = 0.5f * (vyzp[i] - vyzm[i]);
                        const float azx = 0.5f * (vzc[i + 1] - vzc[i - 1]), azy = 0.5f * (vzyp[i] - vzym[i]);
                        const float azz = 0.5f * (vzzp[i] - vzzm[i]);
                        const float kf = -(2.0f / 3.0f) * rho * tau * sig1;
                        mxx = sig * pxx + kf * axx;
                        myy = sig * pyy + kf * ayy;
                        mzz = sig * pzz + kf * azz;
                        mxy = sig * pxy + 0.5f * kf * (axy + ayx);
                        mxz = sig * pxz + 0.5f * kf * (axz + azx);
                        myz = sig * pyz + 0.5f * kf * (ayz + azy);
                    }
                    const float om1 = 1.0f - om;
                    const float taub = tau > taub_min ? tau : taub_min;
                    const float bt = (1.0f - 1.0f / taub) * (mxx + myy + mzz) * (1.0f / 3.0f);
                    const float t3 = (mxx + myy + mzz) * (1.0f / 3.0f);
                    const float dxx = mxx - t3, dyy = myy - t3, dzz = mzz - t3;
                    const float n0 = -1.5f * bt;
                    const float qx = 0.25f * om1 * dxx, qy = 0.25f * om1 * dyy, qz = 0.25f * om1 * dzz;
                    const float hxy = 0.125f * (om1 * (dxx + dyy) + bt), gxy = 0.25f * om1 * mxy;
                    const float hxz = 0.125f * (om1 * (dxx + dzz) + bt), gxz = 0.25f * om1 * mxz;
                    const float hyz = 0.125f * (om1 * (dyy + dzz) + bt), gyz = 0.25f * om1 * myz;
                    o0 = e0 + n0;
                    o1 = e1s + e1a + qx, o2 = e1s - e1a + qx;
                    o3 = e3s + e3a + qy, o4 = e3s - e3a + qy;
                    o5 = e5s + e5a + qz, o6 = e5s - e5a + qz;
                    o7 = e7s + e7a + hxy + gxy, o8 = e7s - e7a + hxy + gxy;
                    o9 = e9s + e9a + hxz + gxz, o10 = e9s - e9a + hxz + gxz;
                    o11 = e11s + e11a + hyz + gyz, o12 = e11s - e11a + hyz + gyz;
                    o13 = e13s + e13a + hxy - gxy, o14 = e13s - e13a + hxy - gxy;
                    o15 = e15s + e15a + hxz - gxz, o16 = e15s - e15a + hxz - gxz;
                    o17 = e17s + e17a + hyz - gyz, o18 = e17s - e17a + hyz - gyz;
                    if (rr3) {
                        /* third-order recursive terms (Malaspinas 2015). D3Q19 carries the Hermite pairs
                         * (xxy +- yzz), (xzz +- xyy), (yyz +- xxz); their lattice norms 2 cs^6 and 6 cs^6 give the
                         * weights below. Moments: equilibrium rho u u u plus the relaxed non-equilibrium
                         * a3_ijk = u_i a2_jk + u_j a2_ik + u_k a2_ij built recursively from the stress. */
                        const float nxxy = 2.0f * ux * mxy + uy * mxx, nyzz = 2.0f * uz * myz + uy * mzz;
                        const float nxzz = 2.0f * uz * mxz + ux * mzz, nxyy = 2.0f * uy * mxy + ux * myy;
                        const float nyyz = 2.0f * uy * myz + uz * myy, nxxz = 2.0f * ux * mxz + uz * mxx;
                        const float exxy = er * vxx * vy, eyzz = er * vy * vzz, exzz = er * vx * vzz;
                        const float exyy = er * vx * vyy, eyyz = er * vyy * vz, exxz = er * vxx * vz;
                        const float A1p = exxy + eyzz + om1 * (nxxy + nyzz), A1m = exxy - eyzz + om1 * (nxxy - nyzz);
                        const float A2p = exzz + exyy + om1 * (nxzz + nxyy), A2m = exzz - exyy + om1 * (nxzz - nxyy);
                        const float A3p = eyyz + exxz + om1 * (nyyz + nxxz), A3m = eyyz - exxz + om1 * (nyyz - nxxz);
                        const float h1 = 0.5f * A2p, h3 = 0.5f * A1p, h5 = 0.5f * A3p;
                        const float h7 = 0.125f * (A1p + A2p + A1m - A2m), h13 = 0.125f * (A2p - A1p - A1m - A2m);
                        const float h9 = 0.125f * (A2p + A3p + A2m - A3m), h15 = 0.125f * (A2p - A3p + A2m + A3m);
                        const float h11 = 0.125f * (A1p + A3p - A1m + A3m), h17 = 0.125f * (A1p - A3p - A1m - A3m);
                        o1 -= h1, o2 += h1, o3 -= h3, o4 += h3, o5 -= h5, o6 += h5;
                        o7 += h7, o8 -= h7, o13 += h13, o14 -= h13;
                        o9 += h9, o10 -= h9, o15 += h15, o16 -= h15;
                        o11 += h11, o12 -= h11, o17 += h17, o18 -= h17;
                    }
                } else {
                    o0 = f0 + om * (e0 - f0);
                    o1 = f1 + om * (e1s + e1a - f1), o2 = f2 + om * (e1s - e1a - f2);
                    o3 = f3 + om * (e3s + e3a - f3), o4 = f4 + om * (e3s - e3a - f4);
                    o5 = f5 + om * (e5s + e5a - f5), o6 = f6 + om * (e5s - e5a - f6);
                    o7 = f7 + om * (e7s + e7a - f7), o8 = f8 + om * (e7s - e7a - f8);
                    o9 = f9 + om * (e9s + e9a - f9), o10 = f10 + om * (e9s - e9a - f10);
                    o11 = f11 + om * (e11s + e11a - f11), o12 = f12 + om * (e11s - e11a - f12);
                    o13 = f13 + om * (e13s + e13a - f13), o14 = f14 + om * (e13s - e13a - f14);
                    o15 = f15 + om * (e15s + e15a - f15), o16 = f16 + om * (e15s - e15a - f16);
                    o17 = f17 + om * (e17s + e17a - f17), o18 = f18 + om * (e17s - e17a - f18);
                }

                const float keep = fl[i] ? 0.0f : 1.0f;
                d0[i] = keep * o0, d1[i] = keep * o1, d2[i] = keep * o2, d3[i] = keep * o3;
                d4[i] = keep * o4, d5[i] = keep * o5, d6[i] = keep * o6, d7[i] = keep * o7;
                d8[i] = keep * o8, d9[i] = keep * o9, d10[i] = keep * o10, d11[i] = keep * o11;
                d12[i] = keep * o12, d13[i] = keep * o13, d14[i] = keep * o14, d15[i] = keep * o15;
                d16[i] = keep * o16, d17[i] = keep * o17, d18[i] = keep * o18;
                if (hrr) wux[i] = keep * ux, wuy[i] = keep * uy, wuz[i] = keep * uz;

                if (out) {
                    const float rr = 1.0f + keep * rho1;
                    orho[ob + i] = rr;
                    oux[ob + i] = keep * ux;
                    ouy[ob + i] = keep * uy;
                    ouz[ob + i] = keep * uz;
                    const float u2 = keep * (uxx + uyy + uzz);
                    umax2 = u2 > umax2 ? u2 : umax2;
                    rmin = rr < rmin ? rr : rmin;
                    rmax = rr > rmax ? rr : rmax;
                    rsum += keep * rho1;
                }
            }
            if (out && !is_finite_f32(rsum)) bad++;
        }
    }
    if (out) {
        if (umax2 > C->umax2[tid]) C->umax2[tid] = umax2;
        if (rmin < C->rmin[tid]) C->rmin[tid] = rmin;
        if (rmax > C->rmax[tid]) C->rmax[tid] = rmax;
        C->bad[tid] += bad;
    }
}

static void sweep_bgk(void *c, int b, int e, int t) { sweep_impl(c, b, e, t, false, false, false, false); }
static void sweep_bgk_out(void *c, int b, int e, int t) { sweep_impl(c, b, e, t, false, true, false, false); }
static void sweep_reg(void *c, int b, int e, int t) { sweep_impl(c, b, e, t, true, false, false, false); }
static void sweep_reg_out(void *c, int b, int e, int t) { sweep_impl(c, b, e, t, true, true, false, false); }
static void sweep_hrr(void *c, int b, int e, int t) { sweep_impl(c, b, e, t, true, false, true, false); }
static void sweep_hrr_out(void *c, int b, int e, int t) { sweep_impl(c, b, e, t, true, true, true, false); }
static void sweep_rr(void *c, int b, int e, int t) { sweep_impl(c, b, e, t, true, false, false, true); }
static void sweep_rr_out(void *c, int b, int e, int t) { sweep_impl(c, b, e, t, true, true, false, true); }
static void sweep_rrh(void *c, int b, int e, int t) { sweep_impl(c, b, e, t, true, false, true, true); }
static void sweep_rrh_out(void *c, int b, int e, int t) { sweep_impl(c, b, e, t, true, true, true, true); }

typedef struct {
    Lbm *L;
} VelCtx;

static void velocity_init_chunk(void *vc, int kb, int ke, int tid) {
    (void)tid;
    Lbm *L = ((VelCtx *)vc)->L;
    for (int k = kb + 1; k <= ke; k++)
        for (int j = 1; j <= L->ny; j++)
            for (int i = 1; i <= L->nx; i++) {
                const size_t n = pidx(L, i, j, k);
                float ux = 0, uy = 0, uz = 0;
                if (!L->flags[n]) {
                    float r = 1.0f, jx = 0, jy = 0, jz = 0;
                    for (int q = 0; q < LBM_Q; q++) {
                        const float v = L->f[q][n];
                        r += v, jx += (float)CX[q] * v, jy += (float)CY[q] * v, jz += (float)CZ[q] * v;
                    }
                    ux = jx / r, uy = jy / r, uz = jz / r;
                }
                L->u[0][n] = ux, L->u[1][n] = uy, L->u[2][n] = uz;
            }
}

/* zero-gradient copies into the halo layers, so the centred differences need no boundary cases */
static void fill_velocity_halo(Lbm *L) {
    const int sx = L->sx, sy = L->sy, sz = L->sz;
    const size_t plane = (size_t)sx * (size_t)sy;
    for (int a = 0; a < 3; a++) {
        float *u = L->u[a];
        for (int k = 1; k < sz - 1; k++) {
            for (int j = 1; j < sy - 1; j++) {
                u[pidx(L, 0, j, k)] = u[pidx(L, 1, j, k)];
                u[pidx(L, sx - 1, j, k)] = u[pidx(L, sx - 2, j, k)];
            }
            for (int i = 0; i < sx; i++) {
                u[pidx(L, i, 0, k)] = u[pidx(L, i, 1, k)];
                u[pidx(L, i, sy - 1, k)] = u[pidx(L, i, sy - 2, k)];
            }
        }
        memcpy(u, u + plane, plane * sizeof(float));
        memcpy(u + plane * (size_t)(sz - 1), u + plane * (size_t)(sz - 2), plane * sizeof(float));
    }
}

/* velocity arrays for the hybrid collision, allocated on first use and re-derived from the populations
 * whenever they are stale (reset, or steps taken with another collision model) */
static bool ensure_velocity(Lbm *L) {
    if (!L->u[0]) {
        for (int a = 0; a < 3; a++) {
            L->u[a] = mem_alloc_aligned(L->npad * sizeof(float));
            L->un[a] = mem_alloc_aligned(L->npad * sizeof(float));
        }
        if (!L->u[0] || !L->u[1] || !L->u[2] || !L->un[0] || !L->un[1] || !L->un[2]) {
            LOGE("out of memory for the hybrid collision's velocity field - using plain regularisation");
            for (int a = 0; a < 3; a++) mem_free_aligned(L->u[a]), mem_free_aligned(L->un[a]), L->u[a] = L->un[a] = NULL;
            return false;
        }
        L->have_u = false;
    }
    if (!L->have_u) {
        VelCtx vc = {L};
        run_for(L, L->nz, 1, velocity_init_chunk, &vc);
        fill_velocity_halo(L);
        L->have_u = true;
    }
    return true;
}

static void fix_solid_output(Lbm *L, const LbmOutput *out) {
    const int nx = L->nx, ny = L->ny, nz = L->nz;
    const size_t nxy = (size_t)nx * (size_t)ny;
    for (size_t b = 0; b < L->nbsolid; b++) {
        const size_t id = L->bsolid[b];
        const int i = (int)(id % (size_t)nx), j = (int)((id / (size_t)nx) % (size_t)ny), k = (int)(id / nxy);
        float sum = 0;
        int cnt = 0;
        for (int q = 1; q <= 6; q++) {
            const int ii = i + CX[q], jj = j + CY[q], kk = k + CZ[q];
            if (ii < 0 || ii >= nx || jj < 0 || jj >= ny || kk < 0 || kk >= nz) continue;
            if (L->flags[pidx(L, ii + 1, jj + 1, kk + 1)]) continue;
            sum += out->rho[(size_t)ii + (size_t)nx * ((size_t)jj + (size_t)ny * (size_t)kk)];
            cnt++;
        }
        if (cnt) out->rho[id] = sum / (float)cnt;
    }
}

void lbm_step(Lbm *L, const LbmConfig *c, const LbmOutput *out) {
    for (int s = 0; s < 4; s++) fill_wall(L, c, s);

    const bool turb = c->turbulence > 0.0f && c->u_in > 0.0f;
    if (turb) {
        const double t = (double)L->step;
        for (int m = 0; m < LBM_TURB_MODES; m++) {
            const double om = 2.0 * M_PI * (double)c->u_in / (double)L->turb_w[m];
            const double ph = fmod(om * t + (double)L->turb_phase[m][0], 2.0 * M_PI);
            for (int j = 0; j < L->sy; j++)
                L->turb_y[m * L->sy + j] = (float)sin((double)L->turb_k[m][0] * j + ph);
            for (int k = 0; k < L->sz; k++)
                L->turb_z[m * L->sz + k] = (float)cos((double)L->turb_k[m][1] * k + (double)L->turb_phase[m][1]);
        }
    }
    IoCtx io = {L, c, turb};
    run_for(L, L->sz, 8, inout_chunk, &io);

    static LinkCtx lc; /* large; only touched from the simulation thread */
    memset(&lc, 0, sizeof lc);
    lc.L = L;
    for (int q = 0; q < LBM_Q; q++) lc.off[q] = qoff(L, q);
    run_for(L, (int)L->nlinks, 2048, links_chunk, &lc);
    double F[3] = {0, 0, 0};
    for (int t = 0; t < MAX_THREADS; t++)
        for (int a = 0; a < 3; a++) F[a] += lc.fs[t][a];

    /* per-column relaxation time: molecular tau0 plus quadratic sponge ramps at both domain ends */
    const float tau0 = c->tau0 > 0.50001f ? c->tau0 : 0.50001f;
    const float taus = c->sponge_tau > tau0 ? c->sponge_tau : tau0;
    const float smax = CLAMP(c->sponge_sigma, 0.0f, 1.0f);
    for (int i = 0; i < L->sx; i++) {
        float w_in = 0.0f, w_out = 0.0f;
        if (c->sponge_in > 0 && i <= c->sponge_in) w_in = 1.0f - (float)(i - 1) / (float)c->sponge_in;
        if (c->sponge_out > 0 && i > L->nx - c->sponge_out)
            w_out = (float)(i - (L->nx - c->sponge_out)) / (float)c->sponge_out;
        w_in = CLAMP(w_in, 0.0f, 1.0f);
        w_out = CLAMP(w_out, 0.0f, 1.0f);
        const float w = w_in > w_out ? w_in : w_out;
        L->tau_x[i] = tau0 + (taus - tau0) * w * w;
        L->sig_r[i] = smax * w * w;
        L->sig_u[i] = smax * w_out * w_out;
    }

    SweepCtx sc;
    memset(&sc, 0, sizeof sc);
    sc.L = L;
    sc.uin = c->u_in;
    sc.cles = 18.0f * (float)M_SQRT2 * c->smagorinsky * c->smagorinsky;
    sc.out = out;
    for (int t = 0; t < MAX_THREADS; t++) sc.rmin[t] = 1e30f, sc.rmax[t] = -1e30f;
    const bool reg = c->collision != LBM_BGK;
    const bool hrr = reg && c->hrr_sigma > 0.0f && c->hrr_sigma < 0.9999f && ensure_velocity(L);
    sc.sigma = hrr ? c->hrr_sigma : 1.0f;
    sc.tau_bulk = c->tau_bulk;
    ParallelFn fn;
    if (!reg) fn = out ? sweep_bgk_out : sweep_bgk;
    else if (c->collision == LBM_RECURSIVE) fn = hrr ? (out ? sweep_rrh_out : sweep_rrh) : (out ? sweep_rr_out : sweep_rr);
    else fn = hrr ? (out ? sweep_hrr_out : sweep_hrr) : (out ? sweep_reg_out : sweep_reg);
    run_for(L, L->nz, 1, fn, &sc);

    if (c->rough_k > 0.0f && L->nwall > 0) {
        static RoughCtx rc;
        memset(&rc, 0, sizeof rc);
        rc.L = L;
        rc.k = c->rough_k;
        run_for(L, (int)L->nwall, 4096, rough_chunk, &rc);
        for (int t = 0; t < MAX_THREADS; t++)
            for (int a = 0; a < 3; a++) F[a] += rc.fs[t][a];
    }

    for (int q = 0; q < LBM_Q; q++) {
        float *tmp = L->f[q];
        L->f[q] = L->g[q];
        L->g[q] = tmp;
    }
    if (hrr) {
        for (int a = 0; a < 3; a++) {
            float *tmp = L->u[a];
            L->u[a] = L->un[a];
            L->un[a] = tmp;
        }
        fill_velocity_halo(L);
    } else {
        L->have_u = false;
    }
    L->force[0] = F[0], L->force[1] = F[1], L->force[2] = F[2];

    if (out) {
        fix_solid_output(L, out);
        float umax2 = 0, rmin = 1e30f, rmax = -1e30f;
        int bad = 0;
        for (int t = 0; t < MAX_THREADS; t++) {
            if (sc.umax2[t] > umax2) umax2 = sc.umax2[t];
            if (sc.rmin[t] < rmin) rmin = sc.rmin[t];
            if (sc.rmax[t] > rmax) rmax = sc.rmax[t];
            bad += sc.bad[t];
        }
        L->umax = sqrt((double)umax2);
        L->rho_min = rmin, L->rho_max = rmax;
        L->unstable = bad > 0 || !is_finite_f32(umax2) || rmin < 0.2f || rmax > 5.0f;
    }
    L->step++;
}
