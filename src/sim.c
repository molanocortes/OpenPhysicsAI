/* sim.c - simulation manager: fluids and units, model placement, solver thread, snapshots, forces */
#include "sim.h"
#include "common.h"
#include "geom/bvh.h"
#include "geom/voxel.h"
#include "threads.h"

#include <pthread.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#define SNAP_SLOTS 4
#define FORCE_SAMPLE_EVERY 5
#define ROUGH_ELEMENT_CD 0.6

/* ---- fluids --------------------------------------------------------------------------------------- */

static const struct {
    const char *name, *label;
} FLUIDS[FLUID_COUNT] = {
    {"water", "Water"},  {"seawater", "Sea water"}, {"air", "Air"},         {"glycerin", "Glycerin"},
    {"oil", "Olive oil"}, {"honey", "Honey"},        {"mercury", "Mercury"}, {"custom", "Custom"},
};

const char *fluid_name(int id) { return (id >= 0 && id < FLUID_COUNT) ? FLUIDS[id].name : "?"; }
const char *fluid_label(int id) { return (id >= 0 && id < FLUID_COUNT) ? FLUIDS[id].label : "?"; }

int fluid_find(const char *name) {
    for (int i = 0; i < FLUID_COUNT; i++)
        if (str_ieq(name, FLUIDS[i].name)) return i;
    if (str_ieq(name, "sea")) return FLUID_SEAWATER;
    if (str_ieq(name, "glycerol")) return FLUID_GLYCERIN;
    if (str_ieq(name, "oliveoil") || str_ieq(name, "olive")) return FLUID_OLIVE_OIL;
    return -1;
}

void fluid_properties(int id, double T, double *rho, double *nu) {
    double mu = 1.0e-3, r = 1000.0;
    switch (id) {
    case FLUID_WATER:
    case FLUID_SEAWATER: {
        double t = CLAMP(T, 0.0, 99.0);
        mu = 2.414e-5 * pow(10.0, 247.8 / (t + 273.15 - 140.0));                                    /* Vogel */
        r = 1000.0 * (1.0 - (t + 288.9414) / (508929.2 * (t + 68.12963)) * (t - 3.9863) * (t - 3.9863)); /* Tanaka */
        if (id == FLUID_SEAWATER) mu *= 1.08, r += 26.5;
        break;
    }
    case FLUID_AIR: {
        double t = CLAMP(T, -60.0, 400.0) + 273.15;
        mu = 1.716e-5 * pow(t / 273.15, 1.5) * (273.15 + 110.4) / (t + 110.4); /* Sutherland */
        r = 101325.0 / (287.05 * t);
        break;
    }
    case FLUID_GLYCERIN: mu = 1.412 * exp(-0.0835 * (T - 20.0)), r = 1261.0 - 0.65 * (T - 20.0); break;
    case FLUID_OLIVE_OIL: mu = 0.084 * exp(-0.046 * (T - 20.0)), r = 911.0 - 0.66 * (T - 20.0); break;
    case FLUID_HONEY: mu = 10.0 * exp(-0.12 * (T - 20.0)), r = 1420.0; break;
    case FLUID_MERCURY: mu = 1.526e-3 * exp(-0.0026 * (T - 20.0)), r = 13534.0 - 2.45 * (T - 20.0); break;
    default: break;
    }
    *rho = r;
    *nu = mu / r;
}

void sim_params_default(SimParams *p) {
    memset(p, 0, sizeof *p);
    p->fluid = FLUID_WATER;
    p->temperature = 20.0;
    fluid_properties(FLUID_WATER, 20.0, &p->rho, &p->nu);
    p->speed = 0.5;
    p->ref_length = 0.2;
    p->ref_axis = REF_AXIS_X;
    p->roughness = 0.0;
    p->turbulence = 0.01;
    p->aoa = 4.0;
    p->pos[0] = 0.28, p->pos[1] = 0.5, p->pos[2] = 0.5;
    p->fit = 0.62;
    p->fit_length = 0.3;
    p->shell = 0.5;
    p->curved_walls = true;
    p->nx = 192, p->ny = 64, p->nz = 224;
    p->u_lattice = 0.08;
    p->collision = LBM_RECURSIVE;
    p->tau_bulk = 1.0;
    p->hrr_sigma = 0.0;
    p->smagorinsky = 0.14;
    for (int i = 0; i < 4; i++) p->wall[i] = LBM_WALL_SLIP;
    p->threads = cpu_count();
    p->ramp_steps = 600;
    p->start_with_flow = false;
}

/* ---- geometry placement --------------------------------------------------------------------------- */

mat4 sim_model_transform(const Mesh *m, const SimParams *p, double *ref_cells) {
    mat4 R = m4_mul(m4_rotate_y(DEG2RAD(p->yaw)), m4_mul(m4_rotate_z(DEG2RAD(-p->aoa)), m4_rotate_x(DEG2RAD(-p->roll))));
    vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
    for (uint32_t i = 0; i < 3 * m->tri_count; i++) {
        vec3 v = m4_mul_point(R, m->pos[i]);
        lo = v3_min(lo, v), hi = v3_max(hi, v);
    }
    if (m->tri_count == 0) lo = hi = v3(0, 0, 0);
    vec3 ctr = v3_scale(v3_add(lo, hi), 0.5f);
    vec3 e0 = v3_sub(m->bmax, m->bmin); /* unrotated: scale must not change with the attitude */
    double ex = MAXI(e0.x, 1e-9f), ey = MAXI(e0.y, 1e-9f), ez = MAXI(e0.z, 1e-9f);
    double s = p->fit_length * p->nx / ex;
    s = MINI(s, p->fit * p->ny / ey);
    s = MINI(s, p->fit * p->nz / ez);
    vec3 t = v3((float)(p->pos[0] * p->nx), (float)(p->pos[1] * p->ny), (float)(p->pos[2] * p->nz));
    mat4 M = m4_mul(m4_translate(t), m4_mul(m4_scale(v3((float)s, (float)s, (float)s)), m4_mul(m4_translate(v3_neg(ctr)), R)));
    if (ref_cells) {
        double e = p->ref_axis == REF_AXIS_X ? ex : p->ref_axis == REF_AXIS_Y ? ey : p->ref_axis == REF_AXIS_Z ? ez : MAXI(ex, MAXI(ey, ez));
        *ref_cells = e * s;
    }
    return M;
}

/* ---- manager ------------------------------------------------------------------------------------- */

struct Sim {
    pthread_t thread;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    pthread_cond_t cv_idle;
    bool quit, idle;

    /* requests (mtx) */
    SimParams req;
    bool params_dirty;
    Mesh pending_mesh;
    bool mesh_dirty;
    bool running;
    int steps_requested;
    bool reset_requested;
    bool applying; /* requests taken by the solver thread but not yet reflected in the status */
    double outputs_per_sec;
    double playback; /* fraction of full solver speed; 0 or 1 = unthrottled */
    bool kick_request;
    double kick_amp_req;
    int kick_delay_req;
    double kick_amp; /* solver thread: active start-up perturbation */
    uint64_t kick_from, kick_to;

    /* solver-thread state */
    SimParams active;
    bool have_active;
    Lbm L;
    bool have_lbm;
    ThreadPool *pool;
    int pool_threads;
    Mesh mesh;
    bool has_mesh;
    uint8_t *solid;
    uint64_t grid_id;
    SimUnits units;
    double ref_cells, frontal_cells;
    double curved_fraction;
    Bvh wall_bvh;          /* curved walls: BVH of the mesh in its own coordinates, built once per mesh */
    double wall_build_ms, wall_ray_ms;
    double *wall_v;
    int *wall_tri;
    bool wall_bvh_ok;
    mat4 model_M;
    uint32_t geom_version;
    double sim_time;
    double facc[3];
    int facc_n;

    /* snapshots (mtx) */
    SimSnapshot snaps[SNAP_SLOTS];
    uint32_t snap_geom[SNAP_SLOTS];
    int latest;
    uint64_t serial;

    /* force history + status (mtx) */
    ForceSample hist[SIM_FORCE_HISTORY];
    int hist_head, hist_count;
    SimStatus status;
};

static void set_busy(Sim *s, bool busy, const char *msg) {
    pthread_mutex_lock(&s->mtx);
    s->status.busy = busy;
    str_copy(s->status.busy_msg, sizeof s->status.busy_msg, msg ? msg : "");
    pthread_mutex_unlock(&s->mtx);
}

static void compute_units(Sim *s) {
    const SimParams *p = &s->active;
    SimUnits *u = &s->units;
    double rho, nu;
    if (p->fluid == FLUID_CUSTOM)
        rho = p->rho, nu = p->nu;
    else
        fluid_properties(p->fluid, p->temperature, &rho, &nu);
    u->rho = rho;
    u->nu = MAXI(nu, 1e-12);
    u->U = MAXI(p->speed, 1e-6);
    double L = MAXI(p->ref_length, 1e-6);
    u->ref_cells = s->ref_cells > 0.5 ? s->ref_cells : p->ny * 0.25;
    u->dx = L / u->ref_cells;
    u->re = u->U * L / u->nu;
    /* keep tau0 <= 1.1 for very viscous flows by lowering the lattice velocity */
    double ulb = CLAMP(p->u_lattice, 0.005, 0.2);
    double ulb_visc = (1.1 - 0.5) / 3.0 * u->re / u->ref_cells;
    if (ulb > ulb_visc) ulb = MAXI(ulb_visc, 0.002);
    u->u_lb = ulb;
    u->nu_lb = ulb * u->ref_cells / u->re;
    u->tau0 = 3.0 * u->nu_lb + 0.5;
    u->re_eff = ulb * u->ref_cells / MAXI(u->nu_lb, (0.50001 - 0.5) / 3.0);
    u->dt = ulb * u->dx / u->U;
    u->mach = ulb * sqrt(3.0);
    u->frontal_cells = s->frontal_cells;
    u->frontal_area = s->frontal_cells * u->dx * u->dx;
    u->blockage = s->frontal_cells / ((double)p->ny * (double)p->nz);
    u->ks_cells = p->roughness / u->dx;
    u->rough_k = 0.5 * ROUGH_ELEMENT_CD * MINI(u->ks_cells, 2.0);
}

static void snap_free_arrays(SimSnapshot *sn) {
    mem_free_aligned(sn->rho), mem_free_aligned(sn->ux), mem_free_aligned(sn->uy), mem_free_aligned(sn->uz);
    mem_free_aligned(sn->solid);
    sn->rho = sn->ux = sn->uy = sn->uz = NULL;
    sn->solid = NULL;
    sn->grid_id = 0;
}

static bool snap_alloc(SimSnapshot *sn, int nx, int ny, int nz, uint64_t grid_id) {
    snap_free_arrays(sn);
    size_t n = (size_t)nx * (size_t)ny * (size_t)nz;
    sn->rho = mem_alloc_aligned(n * 4), sn->ux = mem_alloc_aligned(n * 4);
    sn->uy = mem_alloc_aligned(n * 4), sn->uz = mem_alloc_aligned(n * 4);
    sn->solid = mem_alloc_aligned(n);
    if (!sn->rho || !sn->ux || !sn->uy || !sn->uz || !sn->solid) {
        snap_free_arrays(sn);
        return false;
    }
    sn->nx = nx, sn->ny = ny, sn->nz = nz;
    sn->grid_id = grid_id;
    return true;
}

static SimSnapshot *claim_slot(Sim *s, int *slot_out) {
    int slot = -1;
    pthread_mutex_lock(&s->mtx);
    for (int i = 0; i < SNAP_SLOTS; i++)
        if (s->snaps[i].refcount == 0 && i != s->latest) {
            slot = i;
            s->snaps[i].refcount = 1;
            break;
        }
    pthread_mutex_unlock(&s->mtx);
    if (slot < 0) return NULL;
    SimSnapshot *sn = &s->snaps[slot];
    if (sn->grid_id != s->grid_id || !sn->rho) {
        if (!snap_alloc(sn, s->L.nx, s->L.ny, s->L.nz, s->grid_id)) {
            pthread_mutex_lock(&s->mtx);
            sn->refcount = 0;
            pthread_mutex_unlock(&s->mtx);
            return NULL;
        }
        s->snap_geom[slot] = 0;
    }
    if (s->snap_geom[slot] != s->geom_version) {
        memcpy(sn->solid, s->solid, s->L.ncells);
        s->snap_geom[slot] = s->geom_version;
    }
    *slot_out = slot;
    return sn;
}

static void publish(Sim *s, int slot) {
    pthread_mutex_lock(&s->mtx);
    if (s->latest >= 0) {
        SimSnapshot *old = &s->snaps[s->latest];
        old->refcount--;
        if (old->refcount == 0 && old->grid_id != s->grid_id) snap_free_arrays(old);
    }
    s->latest = slot;
    s->snaps[slot].serial = ++s->serial;
    pthread_mutex_unlock(&s->mtx);
}

SimSnapshot *sim_acquire_snapshot(Sim *s, uint64_t newer_than) {
    SimSnapshot *r = NULL;
    pthread_mutex_lock(&s->mtx);
    if (s->latest >= 0 && s->snaps[s->latest].serial > newer_than) {
        r = &s->snaps[s->latest];
        r->refcount++;
    }
    pthread_mutex_unlock(&s->mtx);
    return r;
}

void sim_release_snapshot(Sim *s, SimSnapshot *sn) {
    if (!sn) return;
    pthread_mutex_lock(&s->mtx);
    sn->refcount--;
    if (sn->refcount == 0 && (int)(sn - s->snaps) != s->latest && sn->grid_id != s->grid_id) snap_free_arrays(sn);
    pthread_mutex_unlock(&s->mtx);
}

static void clear_forces(Sim *s) {
    pthread_mutex_lock(&s->mtx);
    s->hist_head = s->hist_count = 0;
    pthread_mutex_unlock(&s->mtx);
    s->facc[0] = s->facc[1] = s->facc[2] = 0;
    s->facc_n = 0;
}

/* ---- curved walls -------------------------------------------------------------------------------- */

typedef struct {
    const Lbm *L;
    const Bvh *bvh;
    float *delta;
    mat4 inv; /* lattice -> mesh coordinates */
    uint64_t hits[64];
} DeltaCtx;

static void delta_chunk(void *vc, int b, int e, int tid) {
    DeltaCtx *c = vc;
    uint64_t hits = 0;
    for (int n = b; n < e; n++) {
        float seg[6];
        lbm_link_segment(c->L, (size_t)n, seg);
        /* the placement is affine, so the ray parameter is the same in mesh coordinates */
        const vec3 po = m4_mul_point(c->inv, v3(seg[0], seg[1], seg[2])), pd = m4_mul_dir(c->inv, v3(seg[3], seg[4], seg[5]));
        const double o[3] = {po.x, po.y, po.z}, d[3] = {pd.x, pd.y, pd.z};
        BvhHit h;
        /* search up to twice the link length: a surface just beyond the solid node (a body thickened by the voxel
         * shell) still moves the wall out to that node instead of leaving it halfway */
        if (bvh_ray_nearest(c->bvh, o, d, 0.0, 2.0, -1, &h)) {
            c->delta[n] = (float)h.t;
            hits++;
        } else {
            c->delta[n] = 0.5f;
        }
    }
    c->hits[tid] += hits;
}

static void drop_wall_bvh(Sim *s) {
    if (s->wall_bvh_ok) bvh_free(&s->wall_bvh);
    memset(&s->wall_bvh, 0, sizeof s->wall_bvh);
    free(s->wall_v), free(s->wall_tri);
    s->wall_v = NULL, s->wall_tri = NULL;
    s->wall_bvh_ok = false;
}

static bool ensure_wall_bvh(Sim *s) {
    if (s->wall_bvh_ok) return true;
    const double t_build = now_seconds();
    const size_t nt = s->mesh.tri_count;
    if (!nt || nt >= (size_t)INT32_MAX / 3) return false;
    drop_wall_bvh(s);
    s->wall_v = malloc(9 * nt * sizeof(double));
    s->wall_tri = malloc(3 * nt * sizeof(int));
    if (!s->wall_v || !s->wall_tri) return false;
    for (size_t i = 0; i < 3 * nt; i++) {
        s->wall_v[3 * i] = s->mesh.pos[i].x, s->wall_v[3 * i + 1] = s->mesh.pos[i].y, s->wall_v[3 * i + 2] = s->mesh.pos[i].z;
        s->wall_tri[i] = (int)i;
    }
    s->wall_bvh_ok = bvh_build(&s->wall_bvh, s->wall_v, s->wall_tri, (int)nt);
    s->wall_build_ms = (now_seconds() - t_build) * 1e3;
    return s->wall_bvh_ok;
}

/* Wall distance of every boundary link from the mesh as placed in the lattice. Returns the fraction of links whose
 * segment crosses the surface; the others keep halfway bounce-back. Only the rays change when the model moves: they
 * are mapped into mesh coordinates, where the BVH was built once. */
static double place_curved_walls(Sim *s) {
    const size_t nl = s->L.nlinks;
    mat4 inv;
    if (!ensure_wall_bvh(s) || !m4_invert(s->model_M, &inv)) return 0.0;
    float *delta = malloc(nl * sizeof(float));
    if (!delta) return 0.0;
    DeltaCtx c;
    memset(&c, 0, sizeof c);
    c.L = &s->L, c.bvh = &s->wall_bvh, c.delta = delta, c.inv = inv;
    const double t_rays = now_seconds();
    pool_for(s->pool, (int)nl, 256, delta_chunk, &c);
    s->wall_ray_ms = (now_seconds() - t_rays) * 1e3;
    uint64_t hits = 0;
    for (int t = 0; t < 64; t++) hits += c.hits[t];
    lbm_set_link_deltas(&s->L, delta);
    return (double)hits / (double)nl;
}

static void revoxelize(Sim *s) {
    const int nx = s->L.nx, ny = s->L.ny, nz = s->L.nz;
    double t0 = now_seconds();
    if (!s->has_mesh) {
        memset(s->solid, 0, s->L.ncells);
        s->frontal_cells = 0;
        s->ref_cells = ny * 0.25;
        s->model_M = m4_identity();
    } else {
        char msg[96];
        snprintf(msg, sizeof msg, "voxelising %u triangles", s->mesh.tri_count);
        set_busy(s, true, msg);
        s->model_M = sim_model_transform(&s->mesh, &s->active, &s->ref_cells);
        VoxelizeOptions o = {(float)s->active.shell, 2};
        VoxelizeResult r;
        voxelize_mesh(&s->mesh, s->model_M, nx, ny, nz, s->solid, &o, s->pool, &r);
        s->frontal_cells = (double)r.frontal_cells;
        /* spanning a periodic direction is intended (e.g. quasi-2D cylinder), so only warn on other axes */
        bool per_y = s->active.wall[0] == LBM_WALL_PERIODIC && s->active.wall[1] == LBM_WALL_PERIODIC;
        bool per_z = s->active.wall[2] == LBM_WALL_PERIODIC && s->active.wall[3] == LBM_WALL_PERIODIC;
        bool touch = r.bbox_min[0] <= 1 || r.bbox_max[0] >= nx - 2 ||
                     (!per_y && (r.bbox_min[1] <= 1 || r.bbox_max[1] >= ny - 2)) ||
                     (!per_z && (r.bbox_min[2] <= 1 || r.bbox_max[2] >= nz - 2));
        if (r.solid_cells > 0 && touch) LOGW("model touches the tunnel boundary - reduce 'fit' or move it");
        if (r.solid_cells == 0) LOGW("model produced no solid cells (too small or outside the tunnel)");
    }
    s->L.periodic_z = s->active.wall[2] == LBM_WALL_PERIODIC && s->active.wall[3] == LBM_WALL_PERIODIC;
    lbm_set_solids(&s->L, s->solid);
    const double t_walls = now_seconds();
    s->wall_build_ms = s->wall_ray_ms = 0;
    s->curved_fraction = s->has_mesh && s->active.curved_walls && s->L.nlinks ? place_curved_walls(s) : 0.0;
    const double walls_ms = (now_seconds() - t_walls) * 1e3;
    s->geom_version++;
    if (s->has_mesh) {
        /* dragging the model re-voxelises several times a second: report at most once per quiet second */
        static double last_log;
        double tnow = now_seconds();
        char walls[48];
        if (s->curved_fraction > 0) snprintf(walls, sizeof walls, "curved, %.0f%% on the surface", 100 * s->curved_fraction);
        else snprintf(walls, sizeof walls, "staircase");
        if (tnow - last_log >= 1.0)
            LOGOK("geometry: %llu solid cells, %zu boundary links (%s), frontal %.0f cells  (%.0f ms, walls %.0f ms)",
                  (unsigned long long)s->L.solid_cells, s->L.nlinks, walls, s->frontal_cells, (tnow - t0) * 1e3, walls_ms);
        if (walls_ms > 0) LOGD("  walls: bvh %.1f ms, %zu rays %.1f ms, rest %.1f ms", s->wall_build_ms, s->L.nlinks,
                               s->wall_ray_ms, walls_ms - s->wall_build_ms - s->wall_ray_ms);
        last_log = tnow;
    }
    set_busy(s, false, NULL);
}

static void do_reset(Sim *s) {
    lbm_reset(&s->L, s->active.start_with_flow ? (float)s->units.u_lb : 0.0f);
    s->kick_amp = 0;
    s->sim_time = 0;
    clear_forces(s);
    pthread_mutex_lock(&s->mtx);
    s->status.diverged = false;
    pthread_mutex_unlock(&s->mtx);
}

static void apply_changes(Sim *s, const SimParams *np, Mesh *newmesh, bool *reset) {
    SimParams old = s->active;
    bool first = !s->have_active;
    s->active = *np;
    s->have_active = true;
    s->active.nx = CLAMP(s->active.nx, 16, 2048);
    s->active.ny = CLAMP(s->active.ny, 16, 2048);
    s->active.nz = CLAMP(s->active.nz, 16, 2048);
    s->active.threads = CLAMP(s->active.threads, 1, 64);

    if (!s->pool || s->active.threads != s->pool_threads) {
        if (s->pool) pool_destroy(s->pool);
        s->pool = pool_create(s->active.threads);
        s->pool_threads = s->active.threads;
        if (s->have_lbm) s->L.pool = s->pool;
    }

    bool grid_changed = first || !s->have_lbm || s->active.nx != old.nx || s->active.ny != old.ny || s->active.nz != old.nz;
    bool geom_changed = newmesh != NULL || grid_changed || np->aoa != old.aoa || np->yaw != old.yaw || np->roll != old.roll ||
                        np->pos[0] != old.pos[0] || np->pos[1] != old.pos[1] || np->pos[2] != old.pos[2] ||
                        np->fit != old.fit || np->fit_length != old.fit_length || np->shell != old.shell ||
                        np->ref_axis != old.ref_axis || np->curved_walls != old.curved_walls;
    if (newmesh) {
        drop_wall_bvh(s);
        mesh_free(&s->mesh);
        s->mesh = *newmesh;
        s->has_mesh = s->mesh.tri_count > 0;
    }
    if (grid_changed) {
        char msg[96];
        snprintf(msg, sizeof msg, "allocating %dx%dx%d lattice", s->active.nx, s->active.ny, s->active.nz);
        set_busy(s, true, msg);
        if (s->have_lbm) lbm_destroy(&s->L);
        s->have_lbm = lbm_create(&s->L, s->active.nx, s->active.ny, s->active.nz, s->pool);
        if (!s->have_lbm && !first) {
            LOGW("falling back to previous grid %dx%dx%d", old.nx, old.ny, old.nz);
            s->active.nx = old.nx, s->active.ny = old.ny, s->active.nz = old.nz;
            s->have_lbm = lbm_create(&s->L, old.nx, old.ny, old.nz, s->pool);
        }
        if (!s->have_lbm) {
            LOGE("could not allocate any lattice");
            set_busy(s, false, NULL);
            return;
        }
        free(s->solid);
        s->solid = calloc(s->L.ncells, 1);
        s->grid_id++;
        *reset = true;
        pthread_mutex_lock(&s->mtx);
        for (int i = 0; i < SNAP_SLOTS; i++)
            if (s->snaps[i].refcount == 0 && i != s->latest) snap_free_arrays(&s->snaps[i]);
        pthread_mutex_unlock(&s->mtx);
        LOGOK("lattice %dx%dx%d  (%.2fM cells, %.0f MB)", s->L.nx, s->L.ny, s->L.nz, s->L.ncells / 1e6,
              lbm_bytes_estimate(s->L.nx, s->L.ny, s->L.nz) / 1048576.0);
    }
    if (geom_changed) revoxelize(s);
    compute_units(s);
    if (s->units.tau0 < 0.5005 && s->active.smagorinsky <= 0.0)
        LOGW("tau0 = %.5f is very close to 0.5 without LES - the run may diverge (enable 'les')", s->units.tau0);
    set_busy(s, false, NULL);
}

static LbmConfig make_config(Sim *s, double *ramp_out) {
    LbmConfig c;
    memset(&c, 0, sizeof c);
    const SimParams *p = &s->active;
    double ramp = 1.0;
    if (!p->start_with_flow && p->ramp_steps > 0) {
        double r = CLAMP((double)s->L.step / (double)p->ramp_steps, 0.0, 1.0);
        ramp = r * r * (3.0 - 2.0 * r);
    }
    *ramp_out = ramp;
    c.tau0 = (float)s->units.tau0;
    c.u_in = (float)(s->units.u_lb * ramp);
    if (s->kick_amp > 0 && s->L.step >= s->kick_from && s->L.step < s->kick_to) {
        double ph = (double)(s->L.step - s->kick_from) / (double)(s->kick_to - s->kick_from);
        c.v_in = (float)(s->kick_amp * s->units.u_lb * ramp * sin(M_PI * ph));
    }
    c.collision = (LbmCollision)p->collision;
    c.tau_bulk = (float)p->tau_bulk;
    c.hrr_sigma = (float)p->hrr_sigma;
    c.smagorinsky = (float)MAXI(p->smagorinsky, 0.0);
    for (int i = 0; i < 4; i++) c.wall[i] = (LbmWall)p->wall[i];
    c.rough_k = (float)s->units.rough_k;
    c.turbulence = (float)CLAMP(p->turbulence, 0.0, 0.5);
    c.sponge_in = MAXI(3, s->L.nx / 40);
    c.sponge_out = MAXI(6, s->L.nx / 12);
    c.sponge_tau = (float)MAXI(0.8, s->units.tau0);
    c.sponge_sigma = 0.2f;
    return c;
}

static void record_forces(Sim *s) {
    s->facc[0] += s->L.force[0], s->facc[1] += s->L.force[1], s->facc[2] += s->L.force[2];
    if (++s->facc_n < FORCE_SAMPLE_EVERY) return;
    double q = 0.5 * s->units.u_lb * s->units.u_lb * MAXI(s->frontal_cells, 1.0) * s->facc_n;
    ForceSample fs = {s->L.step, s->sim_time, (float)(s->facc[0] / q), (float)(s->facc[1] / q), (float)(s->facc[2] / q)};
    s->facc[0] = s->facc[1] = s->facc[2] = 0;
    s->facc_n = 0;
    pthread_mutex_lock(&s->mtx);
    s->hist[s->hist_head] = fs;
    s->hist_head = (s->hist_head + 1) % SIM_FORCE_HISTORY;
    if (s->hist_count < SIM_FORCE_HISTORY) s->hist_count++;
    pthread_mutex_unlock(&s->mtx);
}

/* status is refreshed under the lock by the solver thread; window statistics over recent force samples */
static void update_status(Sim *s, double mlups, double sps, double ramp, bool recompute_strouhal) {
    pthread_mutex_lock(&s->mtx);
    SimStatus *st = &s->status;
    st->running = s->running;
    st->step = s->L.step;
    st->sim_time = s->sim_time;
    st->mlups = mlups;
    st->steps_per_sec = sps;
    st->outputs_per_sec = s->outputs_per_sec;
    st->realtime_factor = sps * s->units.dt;
    st->units = s->units;
    st->nx = s->L.nx, st->ny = s->L.ny, st->nz = s->L.nz;
    st->cells = s->L.ncells;
    st->solid_cells = s->L.solid_cells;
    st->curved_fraction = s->curved_fraction;
    st->mem_mb = lbm_bytes_estimate(s->L.nx, s->L.ny, s->L.nz) / 1048576.0;
    for (int i = 0; i < SNAP_SLOTS; i++)
        if (s->snaps[i].rho) st->mem_mb += s->L.ncells * 17.0 / 1048576.0;
    st->threads = s->pool_threads;
    st->umax_lb = s->L.umax;
    st->rho_min = s->L.rho_min, st->rho_max = s->L.rho_max;
    st->ramp = ramp;
    st->geom_version = s->geom_version;
    st->model_to_lattice = s->model_M;
    st->has_model = s->has_mesh;

    int n = MINI(s->hist_count, 600);
    if (n > 0) {
        double cd = 0, cl = 0, cs = 0;
        for (int i = 0; i < n; i++) {
            const ForceSample *f = &s->hist[(s->hist_head - 1 - i + 2 * SIM_FORCE_HISTORY) % SIM_FORCE_HISTORY];
            cd += f->cd, cl += f->cl, cs += f->cs;
        }
        cd /= n, cl /= n, cs /= n;
        const ForceSample *last = &s->hist[(s->hist_head - 1 + SIM_FORCE_HISTORY) % SIM_FORCE_HISTORY];
        st->cd = cd, st->cl = cl, st->cs = cs;
        st->cd_inst = last->cd, st->cl_inst = last->cl;
        double qA = 0.5 * s->units.rho * s->units.U * s->units.U * s->units.frontal_area;
        st->fx = cd * qA, st->fy = cl * qA, st->fz = cs * qA;
        if (recompute_strouhal) {
            int m = MINI(s->hist_count, 1024);
            st->strouhal = 0;
            if (m >= 200) {
                double mean = 0, amp = 0;
                for (int i = 0; i < m; i++) mean += s->hist[(s->hist_head - m + i + SIM_FORCE_HISTORY) % SIM_FORCE_HISTORY].cl;
                mean /= m;
                int crossings = 0;
                double prev = 0;
                uint64_t first_step = 0, last_step = 0;
                for (int i = 0; i < m; i++) {
                    const ForceSample *f = &s->hist[(s->hist_head - m + i + SIM_FORCE_HISTORY) % SIM_FORCE_HISTORY];
                    double v = f->cl - mean;
                    amp = MAXI(amp, fabs(v));
                    if (i > 0 && ((prev < 0 && v >= 0) || (prev > 0 && v <= 0))) {
                        if (crossings == 0) first_step = f->step;
                        last_step = f->step;
                        crossings++;
                    }
                    prev = v;
                }
                if (crossings >= 5 && amp > 0.01 && last_step > first_step) {
                    double freq = (crossings - 1) * 0.5 / (double)(last_step - first_step); /* per lattice step */
                    st->strouhal = freq * s->units.ref_cells / s->units.u_lb;
                }
            }
        }
    } else {
        st->cd = st->cl = st->cs = st->cd_inst = st->cl_inst = 0;
        st->fx = st->fy = st->fz = 0;
        st->strouhal = 0;
    }
    pthread_mutex_unlock(&s->mtx);
}

typedef struct {
    Sim *s;
    SimSnapshot *sn;
} MomentsCtx;

/* macroscopic fields straight from the populations (used while paused), one z-plane per chunk */
static void snapshot_moments_chunk(void *vc, int kb, int ke, int tid) {
    (void)tid;
    MomentsCtx *C = vc;
    Sim *s = C->s;
    SimSnapshot *sn = C->sn;
    for (int k = kb; k < ke; k++)
        for (int j = 0; j < s->L.ny; j++)
            for (int i = 0; i < s->L.nx; i++) {
                size_t id = (size_t)i + (size_t)s->L.nx * ((size_t)j + (size_t)s->L.ny * (size_t)k);
                float rho, ux, uy, uz;
                lbm_cell_moments(&s->L, i, j, k, &rho, &ux, &uy, &uz);
                bool so = s->solid[id] != 0;
                sn->rho[id] = so ? 1.0f : rho;
                sn->ux[id] = so ? 0.0f : ux;
                sn->uy[id] = so ? 0.0f : uy;
                sn->uz[id] = so ? 0.0f : uz;
            }
}

static void *sim_main(void *arg) {
    Sim *s = arg;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    double last_pub = 0, win_t0 = now_seconds(), win_work = 0, mlups = 0, sps = 0, ramp = 0;
    uint64_t win_steps = 0, win_outputs = 0;
    for (;;) {
        pthread_mutex_lock(&s->mtx);
        while (!s->quit && !s->params_dirty && !s->mesh_dirty && !s->reset_requested && !s->running &&
               s->steps_requested == 0) {
            s->idle = true;
            s->status.running = false;
            pthread_cond_broadcast(&s->cv_idle);
            pthread_cond_wait(&s->cv, &s->mtx);
        }
        s->idle = false;
        if (s->quit) {
            pthread_mutex_unlock(&s->mtx);
            break;
        }
        bool params_dirty = s->params_dirty, mesh_dirty = s->mesh_dirty, reset = s->reset_requested;
        SimParams np = s->req;
        Mesh newmesh;
        mesh_init(&newmesh);
        if (mesh_dirty) {
            newmesh = s->pending_mesh;
            mesh_init(&s->pending_mesh);
        }
        bool kick = s->kick_request;
        double kick_amp = s->kick_amp_req;
        int kick_delay = s->kick_delay_req;
        s->kick_request = false;
        s->params_dirty = s->mesh_dirty = s->reset_requested = false;
        s->applying = params_dirty || mesh_dirty || reset;
        bool running = s->running;
        int pending_steps = s->steps_requested;
        double playback = s->playback;
        pthread_mutex_unlock(&s->mtx);

        if (params_dirty || mesh_dirty) apply_changes(s, &np, mesh_dirty ? &newmesh : NULL, &reset);
        if (!s->have_lbm) {
            pthread_mutex_lock(&s->mtx);
            s->running = false;
            s->steps_requested = 0;
            s->applying = false;
            pthread_mutex_unlock(&s->mtx);
            continue;
        }
        if (reset) do_reset(s);
        if (kick) {
            /* applied after a reset in the same batch, so the pulse is timed from the fresh start */
            s->kick_amp = kick_amp;
            s->kick_from = s->L.step + (uint64_t)MAXI(kick_delay, 0);
            s->kick_to = s->kick_from + (uint64_t)MAXI(200.0, 3.0 * s->units.ref_cells / MAXI(s->units.u_lb, 1e-3));
        }
        if (params_dirty || mesh_dirty || reset) {
            update_status(s, mlups, 0, ramp, false);
            pthread_mutex_lock(&s->mtx);
            s->applying = false;
            pthread_mutex_unlock(&s->mtx);
        }
        if (!running && pending_steps == 0) {
            /* publish a fresh snapshot after geometry/reset changes even while paused */
            if (params_dirty || mesh_dirty || reset) {
                double r;
                LbmConfig c = make_config(s, &r);
                (void)c;
                int slot;
                SimSnapshot *sn = claim_slot(s, &slot);
                if (sn) {
                    MomentsCtx mc = {s, sn};
                    pool_for(s->pool, s->L.nz, 1, snapshot_moments_chunk, &mc);
                    sn->step = s->L.step, sn->sim_time = s->sim_time, sn->units = s->units;
                    sn->geom_version = s->geom_version;
                    publish(s, slot);
                }
            }
            update_status(s, mlups, 0, ramp, false);
            continue;
        }

        LbmConfig cfg = make_config(s, &ramp);
        double now = now_seconds();
        int slot = -1;
        SimSnapshot *sn = NULL;
        /* 24 snapshots/s is plenty for display; each one costs an output sweep, a vis recompute and an upload */
        if (now - last_pub >= 1.0 / 24.0 || !running) sn = claim_slot(s, &slot);
        LbmOutput out;
        if (sn) out.rho = sn->rho, out.ux = sn->ux, out.uy = sn->uy, out.uz = sn->uz;
        lbm_step(&s->L, &cfg, sn ? &out : NULL);
        s->sim_time += s->units.dt;
        record_forces(s);
        if (sn) {
            sn->step = s->L.step, sn->sim_time = s->sim_time, sn->units = s->units;
            sn->geom_version = s->geom_version;
            publish(s, slot);
            last_pub = now;
            win_outputs++;
        }
        win_steps++;
        double t = now_seconds();
        win_work += t - now;
        if (t - win_t0 >= 0.5) {
            sps = win_steps / (t - win_t0);
            /* throughput from compute time only, so slow-motion idling doesn't read as a slow solver */
            mlups = win_steps / MAXI(win_work, 1e-9) * (double)s->L.ncells / 1e6;
            s->outputs_per_sec = win_outputs / (t - win_t0);
            win_steps = 0, win_outputs = 0, win_t0 = t, win_work = 0;
        }
        bool diverged = sn && s->L.unstable;
        pthread_mutex_lock(&s->mtx);
        if (!running && s->steps_requested > 0) s->steps_requested--;
        if (diverged) {
            s->running = false;
            s->steps_requested = 0;
            s->status.diverged = true;
        }
        pthread_mutex_unlock(&s->mtx);
        update_status(s, mlups, running ? sps : 0, ramp, s->L.step % 50 == 0);
        if (diverged)
            LOGE("simulation diverged at step %llu (umax=%.3f) - lower the speed/lattice velocity, enable LES or "
                 "increase resolution, then 'reset'", (unsigned long long)s->L.step, s->L.umax);
        if (running && playback > 0 && playback < 0.999 && !diverged) {
            /* slow motion: idle for the rest of this step's time slice; new requests wake the thread early */
            double idle = MINI((t - now) * (1.0 / playback - 1.0), 2.0);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (long)(idle * 1e9);
            ts.tv_sec += ts.tv_nsec / 1000000000L;
            ts.tv_nsec %= 1000000000L;
            pthread_mutex_lock(&s->mtx);
            if (s->running && !s->quit && !s->params_dirty && !s->mesh_dirty && !s->reset_requested &&
                s->playback == playback)
                pthread_cond_timedwait(&s->cv, &s->mtx, &ts);
            pthread_mutex_unlock(&s->mtx);
        }
    }
    return NULL;
}

Sim *sim_create(const SimParams *p) {
    Sim *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);
    pthread_cond_init(&s->cv_idle, NULL);
    mesh_init(&s->mesh);
    mesh_init(&s->pending_mesh);
    s->latest = -1;
    s->req = *p;
    s->params_dirty = true;
    s->model_M = m4_identity();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_create(&s->thread, &attr, sim_main, s);
    pthread_attr_destroy(&attr);
    return s;
}

void sim_destroy(Sim *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mtx);
    s->quit = true;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
    pthread_join(s->thread, NULL);
    if (s->have_lbm) lbm_destroy(&s->L);
    if (s->pool) pool_destroy(s->pool);
    for (int i = 0; i < SNAP_SLOTS; i++) snap_free_arrays(&s->snaps[i]);
    drop_wall_bvh(s);
    mesh_free(&s->mesh);
    mesh_free(&s->pending_mesh);
    free(s->solid);
    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->cv);
    pthread_cond_destroy(&s->cv_idle);
    free(s);
}

void sim_set_params(Sim *s, const SimParams *p) {
    pthread_mutex_lock(&s->mtx);
    s->req = *p;
    s->params_dirty = true;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
}

void sim_get_params(Sim *s, SimParams *p) {
    pthread_mutex_lock(&s->mtx);
    *p = s->req;
    pthread_mutex_unlock(&s->mtx);
}

void sim_set_mesh(Sim *s, const Mesh *m) {
    Mesh copy;
    mesh_init(&copy);
    if (m && m->tri_count) {
        mesh_reserve(&copy, m->tri_count);
        memcpy(copy.pos, m->pos, 3 * (size_t)m->tri_count * sizeof(vec3));
        copy.tri_count = m->tri_count;
        copy.bmin = m->bmin, copy.bmax = m->bmax;
        str_copy(copy.name, sizeof copy.name, m->name);
    }
    pthread_mutex_lock(&s->mtx);
    mesh_free(&s->pending_mesh);
    s->pending_mesh = copy;
    s->mesh_dirty = true;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
}

void sim_run(Sim *s, bool running) {
    pthread_mutex_lock(&s->mtx);
    if (running && s->status.diverged) {
        s->reset_requested = true;
        s->status.diverged = false;
    }
    s->running = running;
    s->status.running = running;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
}

void sim_kick(Sim *s, double amplitude, int delay_steps) {
    pthread_mutex_lock(&s->mtx);
    s->kick_request = true;
    s->kick_amp_req = CLAMP(amplitude, 0.0, 0.5);
    s->kick_delay_req = MAXI(delay_steps, 0);
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
}

void sim_step(Sim *s, int n) {
    pthread_mutex_lock(&s->mtx);
    s->steps_requested += MAXI(n, 0);
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
}

void sim_set_playback(Sim *s, double fraction) {
    pthread_mutex_lock(&s->mtx);
    s->playback = CLAMP(fraction, 0.001, 1.0);
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
}

void sim_reset(Sim *s) {
    pthread_mutex_lock(&s->mtx);
    s->reset_requested = true;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
}

void sim_status(Sim *s, SimStatus *st) {
    pthread_mutex_lock(&s->mtx);
    *st = s->status;
    st->running = s->running;
    pthread_mutex_unlock(&s->mtx);
}

int sim_force_history(Sim *s, ForceSample *out, int max) {
    pthread_mutex_lock(&s->mtx);
    int n = MINI(max, s->hist_count);
    for (int i = 0; i < n; i++) out[i] = s->hist[(s->hist_head - n + i + SIM_FORCE_HISTORY) % SIM_FORCE_HISTORY];
    pthread_mutex_unlock(&s->mtx);
    return n;
}

bool sim_requests_pending(Sim *s) {
    pthread_mutex_lock(&s->mtx);
    bool p = s->params_dirty || s->mesh_dirty || s->reset_requested || s->applying;
    pthread_mutex_unlock(&s->mtx);
    return p;
}

bool sim_wait_idle(Sim *s, double timeout_s) {
    double end = now_seconds() + timeout_s;
    pthread_mutex_lock(&s->mtx);
    while (!(s->idle && !s->params_dirty && !s->mesh_dirty && !s->reset_requested && s->steps_requested == 0 &&
             !s->running)) {
        double rem = end - now_seconds();
        if (rem <= 0) {
            pthread_mutex_unlock(&s->mtx);
            return false;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        double w = MINI(rem, 0.05);
        ts.tv_nsec += (long)(w * 1e9);
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&s->cv_idle, &s->mtx, &ts);
    }
    pthread_mutex_unlock(&s->mtx);
    return true;
}
