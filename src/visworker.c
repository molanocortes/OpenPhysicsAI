/* visworker.c - asynchronous visualisation pipeline */
#include "visworker.h"
#include "common.h"
#include "render.h"
#include "threads.h"

#include <pthread.h>
#include <unistd.h>
#if defined(__aarch64__) || defined(__arm64__)
#include <arm_neon.h>
#endif
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

struct VisWorker {
    Sim *sim;
    ThreadPool *pool;
    pthread_t thread;
    bool quit;

    pthread_mutex_t req_mtx;
    VisRequest req;

    pthread_mutex_t res_mtx;
    VisResults res;

    /* worker-owned scratch */
    float *field, *q, *speed;
    uint16_t *field_half;
    uint16_t *vel;
    uint8_t *solid;
    size_t cap;
    StreamlineSet stream;
    IsoMesh iso;
    float *seeds;
    int seed_cap;
    float *qs;       /* filter scratch for the Q field */
    size_t qs_n;
    bool iso_coarse; /* extract vortex surfaces on 2x2x2 cells: the full-resolution mesh was too dense */
    double iso_every; /* seconds between extractions: at most a quarter of the time goes to vortex surfaces */
    bool warned_iso;
};

static bool ensure(VisWorker *w, size_t n) {
    if (w->cap >= n) return true;
    mem_free_aligned(w->field), mem_free_aligned(w->q), mem_free_aligned(w->speed);
    mem_free_aligned(w->field_half), mem_free_aligned(w->vel), mem_free_aligned(w->solid);
    w->field = mem_alloc_aligned(n * sizeof(float));
    w->q = mem_alloc_aligned(n * sizeof(float));
    w->speed = mem_alloc_aligned(n * sizeof(float));
    w->field_half = mem_alloc_aligned(n * sizeof(uint16_t));
    w->vel = mem_alloc_aligned(3 * n * sizeof(uint16_t));
    w->solid = mem_alloc_aligned(n);
    w->cap = (w->field && w->q && w->speed && w->field_half && w->vel && w->solid) ? n : 0;
    return w->cap > 0;
}

/* If height y0 cuts through the body in spanwise column kz (x range [x0,x1]), return a height just clear of it
 * (above or below, whichever is nearer); otherwise y0 unchanged. */
static float clear_height(const uint8_t *solid, int nx, int ny, int x0, int x1, int kz, float y0) {
    int top = -1, bottom = ny;
    for (int j = 0; j < ny; j++) {
        const size_t row = (size_t)nx * ((size_t)j + (size_t)ny * (size_t)kz);
        for (int i = x0; i <= x1; i++)
            if (solid[row + (size_t)i]) {
                if (j > top) top = j;
                if (j < bottom) bottom = j;
                break;
            }
    }
    if (top < 0 || y0 < (float)bottom - 1.0f || y0 > (float)top + 2.0f) return y0;
    const float above = (float)top + 2.5f, below = (float)bottom - 1.5f;
    return (below < 1.0f || fabsf(above - y0) <= fabsf(y0 - below)) ? above : below;
}

/* deterministic jitter in [-0.5, 0.5) per seed: a recomputed set keeps its seeds, so lines evolve with the flow
 * instead of jumping between random placements */
static float seed_jitter(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15, h *= 0x2C1B3C6Du, h ^= h >> 12, h *= 0x297A2D39u, h ^= h >> 15;
    return (float)(h >> 8) / 16777216.0f - 0.5f;
}

static int build_seeds(VisWorker *w, const VisRequest *rq, const uint8_t *solid, int nx, int ny, int nz, bool *both) {
    const int n = CLAMP(rq->seed_count, 1, 20000);
    if (w->seed_cap < n) {
        free(w->seeds);
        w->seeds = malloc((size_t)n * 3 * sizeof(float));
        w->seed_cap = w->seeds ? n : 0;
        if (!w->seeds) return 0;
    }
    const float cx = rq->rake_pos[0], cy = rq->rake_pos[1], cz = rq->rake_pos[2];
    const float sa = MAXI(rq->rake_size[0], 0.0f), sb = MAXI(rq->rake_size[1], 0.0f);
    float *s = w->seeds;
    int k = 0;
    *both = rq->seed_mode == SEED_WAKE || rq->seed_mode == SEED_RANDOM || rq->seed_mode == SEED_POINT;
    #define PUT(X, Y, Z) (s[3 * k] = CLAMP(X, 0.6f, nx - 0.6f), s[3 * k + 1] = CLAMP(Y, 0.6f, ny - 0.6f), \
                          s[3 * k + 2] = CLAMP(Z, 0.6f, nz - 0.6f), k++)
    switch (rq->seed_mode) {
    case SEED_GRID:
    case SEED_WAKE: {
        /* jittered (stratified) grid: even coverage without the moire of a regular lattice of lines */
        const int ax = CLAMP(rq->rake_axis, 0, 2);
        const float asp = MAXI(sa, 1e-3f) / MAXI(sb, 1e-3f);
        const int rows = MAXI(1, (int)lroundf(sqrtf(n * asp))), cols = MAXI(1, n / rows);
        for (int r = 0; r < rows; r++)
            for (int q = 0; q < cols && k < n; q++) {
                const float u = ((r + 0.5f + 0.8f * seed_jitter((uint32_t)r, (uint32_t)q)) / rows - 0.5f) * sa;
                const float v = ((q + 0.5f + 0.8f * seed_jitter((uint32_t)q + 7919u, (uint32_t)r)) / cols - 0.5f) * sb;
                if (ax == 0) PUT(cx, cy + u, cz + v);
                else if (ax == 1) PUT(cx + u, cy, cz + v);
                else PUT(cx + u, cy + v, cz);
            }
        break;
    }
    case SEED_LINE:
        for (int i = 0; i < n; i++) PUT(cx, cy + ((i + 0.5f) / n - 0.5f) * sa, cz);
        break;
    case SEED_RANDOM:
        for (int i = 0; i < n; i++)
            PUT(cx + MAXI(sa, sb) * seed_jitter((uint32_t)i, 11u), cy + sa * seed_jitter((uint32_t)i, 12u),
                cz + sb * seed_jitter((uint32_t)i, 13u));
        break;
    case SEED_POINT:
        for (int i = 0; i < n; i++) { /* uniform in a ball of diameter sa */
            const float r = 0.5f * sa * cbrtf(seed_jitter((uint32_t)i, 1u) + 0.5f);
            const float th = 6.2831853f * (seed_jitter((uint32_t)i, 2u) + 0.5f), cu = 2.0f * seed_jitter((uint32_t)i, 3u);
            const float sn = sqrtf(MAXI(0.0f, 1.0f - cu * cu));
            PUT(cx + r * sn * cosf(th), cy + r * sn * sinf(th), cz + r * cu);
        }
        break;
    default: { /* SEED_PLANE: smoke-wire sheet across the span at the rake height */
        int x0 = 0, x1 = nx - 1;
        if (rq->has_model)
            x0 = CLAMP((int)floorf(rq->box_lo[0]) - 1, 0, nx - 1), x1 = CLAMP((int)ceilf(rq->box_hi[0]) + 1, 0, nx - 1);
        for (int i = 0; i < n; i++) {
            const float z = CLAMP(cz + ((i + 0.5f) / n - 0.5f) * sb, 0.6f, nz - 0.6f);
            /* where the sheet would run inside the body (e.g. a wing with dihedral crossing it at a shallow angle),
             * start the line just clear of the surface so it flows over or under instead of stopping on it */
            const float y = (solid && rq->has_model) ? clear_height(solid, nx, ny, x0, x1, (int)z, cy) : cy;
            PUT(cx, y, z);
        }
        break;
    }
    }
    #undef PUT
    return k;
}

static inline uint16_t half_of(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = (int32_t)((x >> 23) & 0xFF) - 112;
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)e << 10) | ((x & 0x007FFFFFu) >> 13));
}

typedef struct {
    const float *src;
    uint16_t *dst;
    float inv;
    size_t n;
} PackCtx;

/* normalised half-float packing: half the upload of float32 with no loss for colour mapping */
static void pack_field_job(void *vc, int b, int e, int tid) {
    (void)tid;
    PackCtx *C = vc;
    const size_t chunk = 65536;
    size_t i = (size_t)b * chunk, i1 = MINI((size_t)e * chunk, C->n);
#if defined(__aarch64__) || defined(__arm64__)
    for (; i + 4 <= i1; i += 4) {
        float32x4_t v = vmulq_n_f32(vld1q_f32(C->src + i), C->inv);
        vst1_u16(C->dst + i, vreinterpret_u16_f16(vcvt_f16_f32(v)));
    }
#endif
    for (; i < i1; i++) C->dst[i] = half_of(C->src[i] * C->inv);
}

typedef struct {
    const float *ux, *uy, *uz;
    uint16_t *dst;
    int nx, ny, nz, vx, vy, vz;
} VelDownCtx;

/* 2x2x2 block average of the velocity into a half-resolution half-float volume: tracer particles only need a
 * smooth field (solid collisions use the full-resolution mask), and the upload is 8x smaller */
static void vel_down_job(void *vc, int kb, int ke, int tid) {
    (void)tid;
    VelDownCtx *C = vc;
    const int nx = C->nx, ny = C->ny, nz = C->nz, vx = C->vx, vy = C->vy;
    for (int k = kb; k < ke; k++) {
        const int k0 = 2 * k, k1 = MINI(k0 + 1, nz - 1);
        for (int j = 0; j < vy; j++) {
            const int j0 = 2 * j, j1 = MINI(j0 + 1, ny - 1);
            const size_t r00 = (size_t)nx * ((size_t)j0 + (size_t)ny * (size_t)k0);
            const size_t r10 = (size_t)nx * ((size_t)j1 + (size_t)ny * (size_t)k0);
            const size_t r01 = (size_t)nx * ((size_t)j0 + (size_t)ny * (size_t)k1);
            const size_t r11 = (size_t)nx * ((size_t)j1 + (size_t)ny * (size_t)k1);
            uint16_t *o = C->dst + 3 * (size_t)vx * ((size_t)j + (size_t)vy * (size_t)k);
            for (int i = 0; i < vx; i++, o += 3) {
                const size_t i0 = (size_t)(2 * i), i1 = (size_t)MINI(2 * i + 1, nx - 1);
                const size_t id[8] = {r00 + i0, r00 + i1, r10 + i0, r10 + i1, r01 + i0, r01 + i1, r11 + i0, r11 + i1};
                float sx = 0, sy = 0, sz = 0;
                for (int m = 0; m < 8; m++) sx += C->ux[id[m]], sy += C->uy[id[m]], sz += C->uz[id[m]];
                o[0] = half_of(sx * 0.125f), o[1] = half_of(sy * 0.125f), o[2] = half_of(sz * 0.125f);
            }
        }
    }
}

static void *worker_main(void *arg) {
    VisWorker *w = arg;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    uint64_t last_serial = 0;
    uint32_t last_version = 0xFFFFFFFFu, last_geom = 0xFFFFFFFFu;
    double t_vel = 0, t_stream = 0, t_iso = 0;
    uint64_t last_grid = 0;
    while (!w->quit) {
        pthread_mutex_lock(&w->req_mtx);
        VisRequest rq = w->req;
        pthread_mutex_unlock(&w->req_mtx);
        bool changed = rq.version != last_version;
        SimSnapshot *s = sim_acquire_snapshot(w->sim, changed ? 0 : last_serial);
        if (!s) {
            usleep(6000);
            continue;
        }
        last_serial = s->serial;
        last_version = rq.version;
        size_t n = (size_t)s->nx * (size_t)s->ny * (size_t)s->nz;
        if (!ensure(w, n)) {
            sim_release_snapshot(w->sim, s);
            usleep(100000);
            continue;
        }
        bool grid_changed = s->grid_id != last_grid;
        last_grid = s->grid_id;
        VisGrid g = {s->nx, s->ny, s->nz, s->rho, s->ux, s->uy, s->uz, s->solid};
        double now = now_seconds();

        double t0 = now_seconds();
        VisRange range;
        vis_compute_field(&g, (VisField)rq.field, w->field, &range, w->pool);
        float field_scale = MAXI(fabsf(range.min), fabsf(range.max));
        if (!(field_scale > 1e-30f)) field_scale = 1.0f;
        /* publish buffers are swapped with the results side, which starts out empty: re-allocate when a swap
         * handed us NULL, and skip the output rather than write through a missing buffer */
        if (!w->field_half) w->field_half = mem_alloc_aligned(n * sizeof(uint16_t));
        const bool have_half = w->field_half != NULL;
        if (have_half) {
            PackCtx pc = {w->field, w->field_half, 1.0f / field_scale, n};
            pool_for(w->pool, (int)((n + 65535) / 65536), 1, pack_field_job, &pc);
        }
        double field_ms = (now_seconds() - t0) * 1e3;

        bool do_vel = rq.velocity && (now - t_vel > 0.06 || changed || grid_changed);
        if (do_vel && !w->vel) w->vel = mem_alloc_aligned(3 * n * sizeof(uint16_t));
        if (!w->vel) do_vel = false;
        if (do_vel) {
            VelDownCtx vd = {s->ux, s->uy, s->uz, w->vel, s->nx, s->ny, s->nz,
                             (s->nx + 1) / 2, (s->ny + 1) / 2, (s->nz + 1) / 2};
            pool_for(w->pool, vd.vz, 1, vel_down_job, &vd);
            t_vel = now;
        }
        bool do_solid = s->geom_version != last_geom || grid_changed;
        if (do_solid && !w->solid) w->solid = mem_alloc_aligned(n);
        if (!w->solid) do_solid = false;
        if (do_solid) {
            memcpy(w->solid, s->solid, n);
            last_geom = s->geom_version;
        }
        bool do_stream = rq.stream_on && (now - t_stream > 0.2 || changed || grid_changed);
        double stream_ms = 0;
        if (do_stream) {
            t0 = now_seconds();
            bool both = false;
            int ns = build_seeds(w, &rq, s->solid, s->nx, s->ny, s->nz, &both);
            StreamParams sp = {MAXI(64, rq.stream_steps), 0.5f, both, 1e-5f};
            vis_streamlines(&g, w->field, w->seeds, ns, &sp, &w->stream, w->pool);
            stream_ms = (now_seconds() - t0) * 1e3;
            t_stream = now;
        }
        bool do_iso = rq.iso_on && (now - t_iso > MAXI(w->iso_every, 0.1) || changed || grid_changed);
        double iso_ms = 0;
        if (do_iso && w->qs_n != n) {
            mem_free_aligned(w->qs);
            w->qs = mem_alloc_aligned(n * sizeof(float));
            w->qs_n = w->qs ? n : 0;
        }
        if (!w->qs) do_iso = false;
        if (do_iso) {
            t0 = now_seconds();
            /* Q of the 2-cell filtered velocity gradient field: grid-scale content of the resolved (LES) field makes
             * speckles that appear and vanish between extractions, the filter keeps the coherent structures */
            vis_compute_field(&g, VIS_QCRITERION, w->q, NULL, w->pool);
            vis_smooth3(w->q, w->qs, s->nx, s->ny, s->nz, w->pool);
            vis_compute_field(&g, VIS_SPEED, w->speed, NULL, w->pool);
            /* fixed physical threshold Q (L/U)^2 = level: nothing rescales it from frame to frame, so surfaces don't
             * pulse. A mesh too dense to draw is extracted on 2x2x2 cells instead (sticky until settings change). */
            const double lu = s->units.u_lb / MAXI(s->units.ref_cells, 1.0);
            if (changed || grid_changed) w->iso_coarse = false;
            IsoParams ip = {(float)(rq.iso_level * lu * lu), (n > 3000000 || w->iso_coarse) ? 2 : 1, true};
            vis_isosurface(&g, w->qs, w->speed, &ip, &w->iso, w->pool);
            uint32_t tris = w->iso.nindices / 3;
            if (tris > 1200000 && ip.downsample == 1) {
                w->iso_coarse = true;
                ip.downsample = 2;
                vis_isosurface(&g, w->qs, w->speed, &ip, &w->iso, w->pool);
                tris = w->iso.nindices / 3;
            }
            if (tris > 2500000) {
                if (!w->warned_iso)
                    LOGW("vortex surfaces too dense (%u triangles) at Q (L/U)^2 = %.3g - raise it ('vortices level')", tris,
                         rq.iso_level);
                w->warned_iso = true;
                do_iso = false; /* don't ship a mesh that would stall the renderer */
            }
            iso_ms = (now_seconds() - t0) * 1e3;
            w->iso_every = 4.0 * iso_ms * 1e-3;
            t_iso = now;
        }

        pthread_mutex_lock(&w->res_mtx);
        VisResults *R = &w->res;
        R->nx = s->nx, R->ny = s->ny, R->nz = s->nz;
        R->step = s->step;
        R->units = s->units;
        if (have_half) {
            uint16_t *th = R->field_half;
            R->field_half = w->field_half, w->field_half = th;
            R->field_scale = field_scale;
            R->field_new = true;
        }
        R->field_id = rq.field;
        R->range = range;
        R->field_ms = field_ms;
        if (do_vel) {
            uint16_t *tv = R->vel_half;
            R->vel_half = w->vel, w->vel = tv;
            R->vel_new = true;
        }
        if (do_solid) {
            uint8_t *ts = R->solid;
            R->solid = w->solid, w->solid = ts;
            R->solid_new = true;
            R->geom_version = s->geom_version;
        }
        if (do_stream) {
            StreamlineSet tmp = R->stream;
            R->stream = w->stream, w->stream = tmp;
            R->stream_new = true;
            R->stream_ms = stream_ms;
        }
        if (do_iso) {
            IsoMesh tmp = R->iso;
            R->iso = w->iso, w->iso = tmp;
            R->iso_new = true;
            R->iso_ms = iso_ms;
        }
        /* a result buffer that was not replaced on a grid change still has the old size: drop it so it can never
         * be swapped back into the worker and overflowed */
        if (grid_changed && !do_vel) {
            mem_free_aligned(R->vel_half);
            R->vel_half = NULL;
            R->vel_new = false;
        }
        /* buffers swapped back in are stale and may be smaller: force realloc on size change */
        if (grid_changed) {
            mem_free_aligned(w->field_half), mem_free_aligned(w->vel), mem_free_aligned(w->solid);
            w->field_half = mem_alloc_aligned(n * sizeof(uint16_t));
            w->vel = mem_alloc_aligned(3 * n * sizeof(uint16_t));
            w->solid = mem_alloc_aligned(n);
        }
        pthread_mutex_unlock(&w->res_mtx);
        sim_release_snapshot(w->sim, s);
    }
    return NULL;
}

VisWorker *visworker_create(Sim *sim, int threads) {
    VisWorker *w = calloc(1, sizeof *w);
    w->sim = sim;
    w->pool = pool_create(MAXI(1, threads));
    pthread_mutex_init(&w->req_mtx, NULL);
    pthread_mutex_init(&w->res_mtx, NULL);
    w->req.field = VIS_SPEED;
    w->req.seed_count = 200;
    w->req.stream_steps = 1200;
    w->req.version = 1;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_create(&w->thread, &attr, worker_main, w);
    pthread_attr_destroy(&attr);
    return w;
}

void visworker_destroy(VisWorker *w) {
    if (!w) return;
    w->quit = true;
    pthread_join(w->thread, NULL);
    pool_destroy(w->pool);
    mem_free_aligned(w->field), mem_free_aligned(w->q), mem_free_aligned(w->speed), mem_free_aligned(w->qs);
    mem_free_aligned(w->field_half), mem_free_aligned(w->vel), mem_free_aligned(w->solid);
    mem_free_aligned(w->res.field_half), mem_free_aligned(w->res.vel_half), mem_free_aligned(w->res.solid);
    streamlines_free(&w->stream);
    streamlines_free(&w->res.stream);
    isomesh_free(&w->iso);
    isomesh_free(&w->res.iso);
    free(w->seeds);
    pthread_mutex_destroy(&w->req_mtx);
    pthread_mutex_destroy(&w->res_mtx);
    free(w);
}

void visworker_request(VisWorker *w, const VisRequest *req) {
    pthread_mutex_lock(&w->req_mtx);
    w->req = *req;
    pthread_mutex_unlock(&w->req_mtx);
}

VisResults *visworker_lock(VisWorker *w) {
    pthread_mutex_lock(&w->res_mtx);
    return &w->res;
}

void visworker_unlock(VisWorker *w) { pthread_mutex_unlock(&w->res_mtx); }
