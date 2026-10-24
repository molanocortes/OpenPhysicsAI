/* mtrain.c - see mtrain.h */
#include "mtrain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mfactor.h"
#include "../../threads.h"
#ifdef __linux__
#include <sched.h>
#endif

enum { TR_MAX_VIS = 96 };

typedef struct Visual {
    char name[48];
    int owner;        /* the shaft whose rotation turns it about its axis */
    int nnodes, nelems;
    double *local;    /* 3 * nnodes: x, y relative to the owner's axis at the reference pose, z absolute */
    int *conn;
    double *world;
} Visual;

struct Train {
    TrainSpec spec;
    GearModel *model[TR_MAX_MODELS];
    GearPart part[TR_MAX_GEARS];
    double ratio[TR_MAX_SHAFTS];
    int frame[TR_MAX_MESHES]; /* the carrier whose frame holds both axes of a mesh; -1: the housing */
    int dofx[TR_MAX_SHAFTS];  /* a floating shaft's first translation, else -1 */
    int ndof;
    MContactSet cs;
    NearField nf;
    int *c_mesh, *c_side, *c_ga, *c_gb, cap;
    double *lambda;
    double qline[TR_MAX_MESHES];
    bool have_qline;
    /* q, q0: each rigid degree of freedom's offset from the kinematic pose where the train settled under its loads and
     * under the closure's thousandth of them (have_pose: the last solve's, the next one's first guess); theta: the
     * kinematic turns; reach: how far a turn of each degree of freedom moves its gears' teeth, m/rad (1 for a
     * translation), to measure a pose's change as a length */
    double q[MC_MAX_DOF], q0[MC_MAX_DOF], theta[TR_MAX_SHAFTS], reach[MC_MAX_DOF];
    bool have_pose;
    double *f[TR_MAX_GEARS], *u[TR_MAX_GEARS], *nstrain[TR_MAX_GEARS], *world[TR_MAX_GEARS];
    float *mises[TR_MAX_GEARS], *nmises[TR_MAX_GEARS];
    Visual vis[TR_MAX_VIS];
    int nvis;
    double setup_seconds;
    char summary[8192];
};

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

const TrainSpec *train_spec(const Train *t) { return &t->spec; }
double train_ratio(const Train *t, int shaft) { return t->ratio[shaft]; }
int train_gears(const Train *t) { return t->spec.ngears; }
int train_visuals(const Train *t) { return t->nvis; }
const char *train_summary(const Train *t) { return t->summary; }

void train_gear(Train *t, int i, const char **name, const double **xyz, int *nnodes, const int **conn, int *nelems, const float **mises,
                const float **node_mises) {
    GearPart *p = &t->part[i];
    *name = p->name, *xyz = t->world[i], *nnodes = p->m->mesh.nnodes, *conn = p->m->mesh.conn, *nelems = p->m->mesh.nelems;
    *mises = t->mises[i], *node_mises = t->nmises[i];
}

void train_visual(Train *t, int i, const char **name, const double **xyz, int *nnodes, const int **conn, int *nelems) {
    Visual *v = &t->vis[i];
    *name = v->name, *xyz = v->world, *nnodes = v->nnodes, *conn = v->conn, *nelems = v->nelems;
}

void train_free(Train *t) {
    if (!t) return;
    for (int i = 0; i < t->spec.nmodels; i++) gearmodel_release(t->model[i]);
    for (int i = 0; i < t->spec.ngears; i++) free(t->f[i]), free(t->u[i]), free(t->nstrain[i]), free(t->world[i]), free(t->mises[i]), free(t->nmises[i]);
    for (int i = 0; i < t->nvis; i++) free(t->vis[i].local), free(t->vis[i].conn), free(t->vis[i].world);
    mcontact_free(&t->cs);
    nearfield_free(&t->nf);
    free(t->c_mesh), free(t->c_side), free(t->c_ga), free(t->c_gb), free(t->lambda);
    free(t);
}

/* ---------------------------------------------------------------- geometry helpers */

static double inv_(double a) { return tan(a) - a; }

static double inv_solve(double target) {
    double lo = 1e-6, hi = 1.5;
    for (int i = 0; i < 200; i++) {
        double mid = 0.5 * (lo + hi);
        if (inv_(mid) < target) lo = mid;
        else hi = mid;
    }
    return 0.5 * (lo + hi);
}

/* the operating centre distance of two gears (one may be internal), from their shifts, with no backlash from them */
static double centre_distance(const GearGeometry *A, const GearGeometry *B) {
    if (!A->internal && !B->internal) return gear_centre_distance(&A->spec, &B->spec);
    const GearGeometry *I = A->internal ? A : B, *X = A->internal ? B : A;
    double an = I->spec.pressure_deg * M_PI / 180, be = I->spec.helix_deg * M_PI / 180, at = atan(tan(an) / cos(be)), mt = I->spec.module_m / cos(be);
    int zi = I->spec.teeth, ze = X->spec.teeth;
    double aw = inv_solve(inv_(at) + 2 * tan(an) * (I->spec.shift - X->spec.shift) / (zi - ze));
    return mt * (zi - ze) / 2 * cos(at) / cos(aw);
}

/* ---------------------------------------------------------------- kinematics */

/* least squares A x = b (m x n, row-major, m >= n) by Householder QR; false if the columns are dependent */
static bool lsq(double *A, double *b, int m, int n, double *x, double *resid) {
    double amax = 0;
    for (int i = 0; i < m * n; i++) amax = fmax(amax, fabs(A[i]));
    for (int k = 0; k < n; k++) {
        double norm = 0;
        for (int i = k; i < m; i++) norm += A[i * n + k] * A[i * n + k];
        norm = sqrt(norm);
        if (norm < 1e-10 * amax) return false;
        double alpha = A[k * n + k] > 0 ? -norm : norm, v0 = A[k * n + k] - alpha, vn = v0 * v0;
        for (int i = k + 1; i < m; i++) vn += A[i * n + k] * A[i * n + k];
        for (int j = k + 1; j < n; j++) {
            double s = v0 * A[k * n + j];
            for (int i = k + 1; i < m; i++) s += A[i * n + k] * A[i * n + j];
            s = 2 * s / vn;
            A[k * n + j] -= s * v0;
            for (int i = k + 1; i < m; i++) A[i * n + j] -= s * A[i * n + k];
        }
        double s = v0 * b[k];
        for (int i = k + 1; i < m; i++) s += A[i * n + k] * b[i];
        s = 2 * s / vn;
        b[k] -= s * v0;
        for (int i = k + 1; i < m; i++) b[i] -= s * A[i * n + k];
        A[k * n + k] = alpha;
    }
    for (int k = n - 1; k >= 0; k--) {
        double v = b[k];
        for (int j = k + 1; j < n; j++) v -= A[k * n + j] * x[j];
        x[k] = v / A[k * n + k];
    }
    double r = 0;
    for (int i = n; i < m; i++) r += b[i] * b[i];
    *resid = sqrt(r);
    return true;
}

static bool kinematics(Train *t, char *err, size_t errlen) {
    const TrainSpec *S = &t->spec;
    int n = S->nshafts, m = 0, cap = n + 1 + S->nmeshes;
    double *A = calloc((size_t)cap * n, sizeof(double)), *b = calloc((size_t)cap, sizeof(double)), x[TR_MAX_SHAFTS], resid;
    if (!A || !b) {
        free(A), free(b);
        return snprintf(err, errlen, "out of memory"), false;
    }
    for (int s = 0; s < n; s++)
        if (S->shaft[s].fixed) A[m * n + s] = 1, b[m++] = 0;
    A[m * n + S->input] = 1, b[m++] = 1;
    for (int k = 0; k < S->nmeshes; k++) {
        const GearPart *ga = &t->part[S->mesh[k].a], *gb = &t->part[S->mesh[k].b];
        int a = S->gear[S->mesh[k].a].shaft, bs = S->gear[S->mesh[k].b].shaft, c = t->frame[k];
        double za = ga->m->geo.spec.teeth, zb = gb->m->geo.spec.teeth, sg = (ga->m->geo.internal || gb->m->geo.internal) ? -1 : 1;
        A[m * n + a] += za, A[m * n + bs] += sg * zb;
        if (c >= 0) A[m * n + c] -= za + sg * zb;
        b[m++] = 0;
    }
    bool ok = m >= n && lsq(A, b, m, n, x, &resid);
    free(A), free(b);
    if (!ok) return snprintf(err, errlen, "train: the meshes and fixed shafts leave the train with more than one degree of freedom (a shaft that nothing turns?)"), false;
    if (resid > 1e-9) return snprintf(err, errlen, "train: the meshes and fixed shafts cannot all hold: the train would be locked (ties disagree by %.3g)", resid), false;
    for (int s = 0; s < n; s++) t->ratio[s] = fabs(x[s]) < 1e-14 ? 0 : x[s];
    return true;
}

/* ---------------------------------------------------------------- phasing */

/* the tooth angle that gear B needs at the reference pose to mesh with gear A as phased: along the line of centres one
 * gear's tooth space centred on the other's tooth */
static double mesh_phase(const GearPart *A, const GearPart *B) {
    bool ia = A->m->geo.internal, ib = B->m->geo.internal;
    int zA = A->m->geo.spec.teeth, zB = B->m->geo.spec.teeth;
    double pA = 2 * M_PI / zA, pB = 2 * M_PI / zB, ta = gearpart_tooth_angle(A, 0);
    if (!ia && !ib) {
        double psi = atan2(B->ay - A->ay, B->ax - A->ax), dA = remainder(ta - psi, pA);
        return psi + M_PI + pB / 2 - (double)zA / zB * dA;
    }
    if (!ia) { /* A inside the ring B */
        double psi = atan2(A->ay - B->ay, A->ax - B->ax), dA = remainder(ta - psi, pA);
        return psi + pB / 2 + (double)zA / zB * dA;
    }
    double psi = atan2(B->ay - A->ay, B->ax - A->ax), dR = remainder(ta - psi, pA); /* B inside the ring A */
    return psi + (double)zA / zB * (dR - pA / 2);
}

/* the polar angle about B's axis of B's teeth that face A */
static double facing(const GearPart *B, const GearPart *A) {
    double dx = A->ax - B->ax, dy = A->ay - B->ay;
    if (!B->m->geo.internal && A->m->geo.internal) return atan2(-dy, -dx);
    return atan2(dy, dx);
}

static bool phase_gears(Train *t, char *err, size_t errlen) {
    const TrainSpec *S = &t->spec;
    int G = S->ngears, queue[TR_MAX_GEARS], qh = 0, qt = 0;
    bool phased[TR_MAX_GEARS] = {false};
    for (int g = 0; g < G; g++)
        if (!isnan(S->gear[g].phase_deg)) {
            GearPart *p = &t->part[g];
            gearpart_init(p, S->gear[g].name, p->m, p->ax, p->ay, p->az, S->gear[g].phase_deg * M_PI / 180);
            phased[g] = true, queue[qt++] = g;
        }
    for (;;) { /* with no phase given, the first gear's middle tooth faces its first partner */
        if (qh == qt) { /* a group of gears tied to the rest only through shafts (the second gears of compound planets and
                           their ring): its phase is free, the gears on a shaft being timed to fit */
            int g = 0;
            while (g < G && phased[g]) g++;
            if (g == G) break;
            for (int k = 0; k < S->nmeshes && isnan(S->gear[g].phase_deg); k++) { /* a sector faces its first partner */
                int o = S->mesh[k].a == g ? S->mesh[k].b : S->mesh[k].b == g ? S->mesh[k].a : -1;
                if (o < 0) continue;
                GearPart *p = &t->part[g];
                gearpart_init(p, S->gear[g].name, p->m, p->ax, p->ay, p->az, facing(p, &t->part[o]));
                break;
            }
            phased[g] = true, queue[qt++] = g;
        }
        int g = queue[qh++];
        for (int k = 0; k < S->nmeshes; k++) {
            int o = S->mesh[k].a == g ? S->mesh[k].b : S->mesh[k].b == g ? S->mesh[k].a : -1;
            if (o < 0) continue;
            GearPart *A = &t->part[g], *B = &t->part[o];
            double need = mesh_phase(A, B), pB = 2 * M_PI / B->m->geo.spec.teeth;
            if (!phased[o]) {
                double fc = facing(B, A);
                gearpart_init(B, S->gear[o].name, B->m, B->ax, B->ay, B->az, fc + remainder(need - fc, pB));
                phased[o] = true, queue[qt++] = o;
                continue;
            }
            double mis = remainder(gearpart_tooth_angle(B, 0) - need, pB) * B->m->geo.r_b;
            if (fabs(mis) > 1e-3 * B->m->geo.spec.module_m)
                return snprintf(err, errlen,
                                "train: %s and %s cannot both mesh where they are: misphased by %.1f um along the line of action (evenly spaced planets "
                                "need (z_sun + z_ring) / N whole; a compound planet's gears need their tooth numbers to agree)",
                                A->name, B->name, 1e6 * fabs(mis)),
                       false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- compliance zones */

/* Where the factorisation is the built-in one (one thread per factorisation and solve; Accelerate threads its own),
 * the models of a train are built, and their compliances computed, side by side, at most one per processor this
 * process may use: each model alone, as it would be built in turn, so the results do not change. */
static int model_threads(int n) {
#ifdef __APPLE__
    (void)n;
    return 1;
#else
    int c = cpu_count();
#ifdef __linux__
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) c = CPU_COUNT(&set);
#endif
    return c < n ? (c > 0 ? c : 1) : n;
#endif
}

typedef struct ModelJobs {
    const TrainSpec *S;
    GearModel **model;
    unsigned char *mask[TR_MAX_MODELS];
    bool ok[TR_MAX_MODELS];
    char err[TR_MAX_MODELS][256];
} ModelJobs;

static void build_models(void *ctx, int begin, int end, int tid) {
    (void)tid;
    ModelJobs *J = ctx;
    for (int i = begin; i < end; i++) {
        const TrainModelSpec *M = &J->S->model[i];
        J->model[i] = gearmodel_create(&M->gear, M->internal ? &M->cutter : NULL, &M->mesh, M->E, M->nu, true, &M->hold, J->err[i], sizeof J->err[i]);
    }
}

static void zone_models(void *ctx, int begin, int end, int tid) {
    (void)tid;
    ModelJobs *J = ctx;
    for (int i = begin; i < end; i++) J->ok[i] = gearmodel_set_zone(J->model[i], GM_LEFT | GM_RIGHT, J->mask[i], J->err[i], sizeof J->err[i]);
}

static void side_by_side(int n, ParallelFn fn, ModelJobs *J) {
    int nt = model_threads(n);
    ThreadPool *pool = nt > 1 ? pool_create(nt) : NULL;
    if (pool) pool_for(pool, n, 1, fn, J), pool_destroy(pool);
    else fn(J, 0, n, 0);
}

/* every model's compliance: all teeth, except a model held at lugs, whose compliance is non-cyclic and costs a solve a
 * node: only the teeth that face a partner somewhere in the input's range, three teeth either side, over the face */
static bool zones(Train *t, char *err, size_t errlen) {
    const TrainSpec *S = &t->spec;
    ModelJobs J;
    memset(&J, 0, sizeof J);
    J.S = S, J.model = t->model;
    for (int i = 0; i < S->nmodels; i++) {
        GearModel *m = t->model[i];
        unsigned char *mask = NULL;
        if (!m->held_all_round && m->mesh.teeth == m->geo.spec.teeth) {
            int z = m->geo.spec.teeth;
            mask = calloc((size_t)z, 1);
            if (!mask) {
                for (int k = 0; k < i; k++) free(J.mask[k]);
                return snprintf(err, errlen, "out of memory"), false;
            }
            double a0 = S->range_deg[0] * M_PI / 180, a1 = S->range_deg[1] * M_PI / 180, pitch = 2 * M_PI / z, b = m->geo.spec.face_width_m;
            enum { NS = 48 };
            for (int g = 0; g < S->ngears; g++) {
                if (S->gear[g].model != i) continue;
                for (int k = 0; k < S->nmeshes; k++) {
                    int o = S->mesh[k].a == g ? S->mesh[k].b : S->mesh[k].b == g ? S->mesh[k].a : -1;
                    if (o < 0) continue;
                    for (int q = 0; q <= NS; q++) {
                        double phi = a0 + (a1 - a0) * q / NS, th[TR_MAX_SHAFTS], xy[2][2];
                        for (int s = 0; s < S->nshafts; s++) th[s] = t->ratio[s] * phi;
                        for (int w = 0; w < 2; w++) { /* the two axes at this turn */
                            const TrainShaftSpec *sh = &S->shaft[S->gear[w ? o : g].shaft];
                            double x = sh->x, y = sh->y;
                            if (sh->carrier >= 0) {
                                const TrainShaftSpec *cr = &S->shaft[sh->carrier];
                                double c = cos(th[sh->carrier]), sn = sin(th[sh->carrier]);
                                x = cr->x + c * (sh->x - cr->x) - sn * (sh->y - cr->y), y = cr->y + sn * (sh->x - cr->x) + c * (sh->y - cr->y);
                            }
                            xy[w][0] = x, xy[w][1] = y;
                        }
                        double dx = xy[1][0] - xy[0][0], dy = xy[1][1] - xy[0][1];
                        double fc = (!m->geo.internal && t->part[o].m->geo.internal) ? atan2(-dy, -dx) : atan2(dy, dx);
                        for (int e = 0; e < 2; e++) { /* both ends of the face */
                            double base = t->part[g].rot0 + th[S->gear[g].shaft] + m->mesh.tooth_angle0 + m->geo.twist * b * e;
                            long c = lround((fc - base) / pitch);
                            for (long d = -3; d <= 3; d++) mask[(((c + d) % z) + z) % z] = 1;
                        }
                    }
                }
            }
        }
        J.mask[i] = mask;
    }
    side_by_side(S->nmodels, zone_models, &J);
    bool ok = true;
    for (int i = 0; i < S->nmodels; i++) {
        if (ok && !J.ok[i]) snprintf(err, errlen, "%s", J.err[i]), ok = false;
        free(J.mask[i]);
    }
    return ok;
}

/* ---------------------------------------------------------------- pictures: carriers, pins, hubs */

/* an annular hex mesh about the origin: radii r0 < r1, z from z0 to z1 */
static bool annulus(Visual *v, const char *name, int owner, double cx, double cy, double r0, double r1, double z0, double z1, int nth) {
    int nr = 2, nz = 2;
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->owner = owner;
    v->nnodes = nth * (nr + 1) * (nz + 1), v->nelems = nth * nr * nz;
    v->local = malloc((size_t)v->nnodes * 3 * sizeof(double)), v->world = malloc((size_t)v->nnodes * 3 * sizeof(double));
    v->conn = malloc((size_t)v->nelems * 8 * sizeof(int));
    if (!v->local || !v->world || !v->conn) return false;
#define VID(i, j, k) (((k) * (nr + 1) + (j)) * nth + ((i) % nth))
    for (int k = 0; k <= nz; k++)
        for (int j = 0; j <= nr; j++)
            for (int i = 0; i < nth; i++) {
                double a = 2 * M_PI * i / nth, r = r0 + (r1 - r0) * j / nr;
                double *X = &v->local[3 * VID(i, j, k)];
                X[0] = cx + r * cos(a), X[1] = cy + r * sin(a), X[2] = z0 + (z1 - z0) * k / nz;
            }
    int e = 0;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < nr; j++)
            for (int i = 0; i < nth; i++) {
                int *c = &v->conn[8 * e++];
                c[0] = VID(i, j, k), c[1] = VID(i, j + 1, k), c[2] = VID(i + 1, j + 1, k), c[3] = VID(i + 1, j, k);
                c[4] = VID(i, j, k + 1), c[5] = VID(i, j + 1, k + 1), c[6] = VID(i + 1, j + 1, k + 1), c[7] = VID(i + 1, j, k + 1);
            }
#undef VID
    return true;
}

static bool build_visuals(Train *t) {
    const TrainSpec *S = &t->spec;
    for (int c = 0; c < S->nshafts; c++) {
        /* a carrier: a plate behind its planets and a pin through each */
        double zmin = INFINITY, zmax = -INFINITY, rmax = 0, pin = INFINITY, boss = 0;
        int carried = 0;
        for (int s = 0; s < S->nshafts; s++) {
            if (S->shaft[s].carrier != c) continue;
            carried++;
            rmax = fmax(rmax, hypot(S->shaft[s].x - S->shaft[c].x, S->shaft[s].y - S->shaft[c].y));
            for (int g = 0; g < S->ngears; g++) {
                if (S->gear[g].shaft != s) continue;
                const GearGeometry *geo = &t->part[g].m->geo;
                zmin = fmin(zmin, S->gear[g].z0_m), zmax = fmax(zmax, S->gear[g].z0_m + geo->spec.face_width_m);
                pin = fmin(pin, geo->spec.bore_radius_m), boss = fmax(boss, geo->spec.bore_radius_m);
            }
        }
        if (!carried || !(zmin < INFINITY)) continue;
        double gap = 0.15 * (zmax - zmin), th = 0.35 * (zmax - zmin), rin = fmax(0.2 * rmax, rmax - 2.2 * boss), rout = rmax + 2.2 * boss;
        double p0 = zmin - gap - th, p1 = zmin - gap, q0 = zmin - gap, q1 = zmax + gap; /* the plate, the pins */
        if (S->shaft[c].plate_front) p0 = zmax + gap, p1 = zmax + gap + th, q0 = zmin - gap, q1 = p1;
        else q0 = p0;
        char name[48];
        snprintf(name, sizeof name, "%s plate", S->shaft[c].name);
        if (t->nvis == TR_MAX_VIS || !annulus(&t->vis[t->nvis++], name, c, 0, 0, rin, rout, p0, p1, 96)) return false;
        for (int s = 0; s < S->nshafts; s++) {
            if (S->shaft[s].carrier != c) continue;
            double px = S->shaft[s].x - S->shaft[c].x, py = S->shaft[s].y - S->shaft[c].y;
            snprintf(name, sizeof name, "%s pin", S->shaft[s].name);
            if (t->nvis == TR_MAX_VIS || !annulus(&t->vis[t->nvis++], name, c, px, py, 0.25 * pin, 0.97 * pin, q0, q1, 24)) return false;
        }
    }
    for (int s = 0; s < S->nshafts; s++) {
        /* a shaft with gears that is not fixed: a hub through its gears' bores, and on to its carrier plate */
        if (S->shaft[s].fixed || S->shaft[s].carrier >= 0) continue;
        double zmin = INFINITY, zmax = -INFINITY, rb = INFINITY;
        int ng = 0;
        for (int g = 0; g < S->ngears; g++) {
            if (S->gear[g].shaft != s || t->part[g].m->geo.internal) continue;
            ng++;
            zmin = fmin(zmin, S->gear[g].z0_m), zmax = fmax(zmax, S->gear[g].z0_m + t->part[g].m->geo.spec.face_width_m);
            rb = fmin(rb, t->part[g].m->geo.spec.bore_radius_m);
        }
        if (!ng) continue;
        double len = zmax - zmin;
        for (int c = 0; c < t->nvis; c++) /* a carrier that is this shaft: reach its plate */
            if (t->vis[c].owner == s && strstr(t->vis[c].name, "plate"))
                for (int n = 0; n < t->vis[c].nnodes; n++) zmin = fmin(zmin, t->vis[c].local[3 * n + 2]), zmax = fmax(zmax, t->vis[c].local[3 * n + 2]);
        char name[48];
        snprintf(name, sizeof name, "%s shaft", S->shaft[s].name);
        if (t->nvis == TR_MAX_VIS || !annulus(&t->vis[t->nvis++], name, s, 0, 0, 0.3 * rb, 0.97 * rb, zmin - 0.3 * len, zmax + 0.3 * len, 32)) return false;
    }
    return true;
}

/* ---------------------------------------------------------------- creation */

Train *train_create(const TrainSpec *spec, char *err, size_t errlen) {
    double t0 = now();
    Train *t = calloc(1, sizeof *t);
    if (!t) return snprintf(err, errlen, "out of memory"), NULL;
    t->spec = *spec;
    TrainSpec *S = &t->spec;
    int nmodels = S->nmodels;
    S->nmodels = 0; /* counts the models built, for train_free */
    if (nmodels < 1 || nmodels > TR_MAX_MODELS || S->nshafts < 1 || S->nshafts > TR_MAX_SHAFTS || S->ngears < 2 || S->ngears > TR_MAX_GEARS ||
        S->nmeshes < 1 || S->nmeshes > TR_MAX_MESHES || S->input < 0 || S->input >= S->nshafts) {
        train_free(t);
        return snprintf(err, errlen, "train: 1-%d gear models, 1-%d shafts, 2-%d gears, 1-%d meshes and an input shaft", TR_MAX_MODELS, TR_MAX_SHAFTS,
                        TR_MAX_GEARS, TR_MAX_MESHES),
               NULL;
    }
    for (int s = 0; s < S->nshafts; s++) {
        int c = S->shaft[s].carrier;
        if (c >= S->nshafts || c == s || (c >= 0 && S->shaft[c].carrier >= 0)) {
            train_free(t);
            return snprintf(err, errlen, "train: %s: a carrier must be another shaft on a fixed axis", S->shaft[s].name), NULL;
        }
        if (c >= 0 && (S->shaft[s].floating || S->shaft[s].fixed)) {
            train_free(t);
            return snprintf(err, errlen, "train: %s: a carried shaft can be neither fixed nor floating (give it a support stiffness)", S->shaft[s].name), NULL;
        }
        if (S->shaft[s].support_n_per_m < 0 || (S->shaft[s].support_n_per_m > 0 && (S->shaft[s].floating || S->shaft[s].fixed))) {
            train_free(t);
            return snprintf(err, errlen, "train: %s: a support stiffness is for a shaft that is neither fixed nor floating", S->shaft[s].name), NULL;
        }
    }
    for (int g = 0; g < S->ngears; g++)
        if (S->gear[g].model < 0 || S->gear[g].model >= nmodels || S->gear[g].shaft < 0 || S->gear[g].shaft >= S->nshafts) {
            train_free(t);
            return snprintf(err, errlen, "train: %s: no such model or shaft", S->gear[g].name), NULL;
        }
    /* the models, each built once (side by side where that is faster) */
    ModelJobs J;
    memset(&J, 0, sizeof J);
    J.S = S, J.model = t->model;
    side_by_side(nmodels, build_models, &J);
    S->nmodels = nmodels;
    for (int i = 0; i < nmodels; i++)
        if (!t->model[i]) {
            snprintf(err, errlen, "train: gear %s: %s", S->model[i].name, J.err[i]);
            train_free(t);
            return NULL;
        }
    /* the gears at the reference pose; models shared by gears count their references */
    int used[TR_MAX_MODELS] = {0};
    for (int g = 0; g < S->ngears; g++) {
        const TrainGearSpec *G = &S->gear[g];
        const TrainShaftSpec *sh = &S->shaft[G->shaft];
        GearModel *m = t->model[G->model]; /* owned by the train, borrowed by its gears */
        used[G->model]++;
        gearpart_init(&t->part[g], G->name, m, sh->x, sh->y, G->z0_m, M_PI / 2);
    }
    for (int i = 0; i < nmodels; i++)
        if (!used[i]) {
            train_free(t);
            return snprintf(err, errlen, "train: the gear model %s is not used", S->model[i].name), NULL;
        }
    /* every mesh: the frame both axes are fixed in, the centre distance its gears need */
    for (int k = 0; k < S->nmeshes; k++) {
        int ga = S->mesh[k].a, gb = S->mesh[k].b;
        if (ga < 0 || gb < 0 || ga >= S->ngears || gb >= S->ngears || ga == gb) {
            train_free(t);
            return snprintf(err, errlen, "train: mesh %d: no such gears", k + 1), NULL;
        }
        const GearPart *A = &t->part[ga], *B = &t->part[gb];
        int a = S->gear[ga].shaft, b = S->gear[gb].shaft, ca = S->shaft[a].carrier, cb = S->shaft[b].carrier, c = -1;
        if (A->m->geo.internal && B->m->geo.internal) {
            train_free(t);
            return snprintf(err, errlen, "train: %s and %s are both internal gears", A->name, B->name), NULL;
        }
        if (ca >= 0 && cb >= 0 && ca != cb) {
            train_free(t);
            return snprintf(err, errlen, "train: %s and %s ride on different carriers", A->name, B->name), NULL;
        }
        c = ca >= 0 ? ca : cb;
        if (c >= 0) { /* the other axis must be the carrier's own */
            int o = ca >= 0 ? (cb >= 0 ? -1 : b) : a;
            if (o >= 0 && hypot(S->shaft[o].x - S->shaft[c].x, S->shaft[o].y - S->shaft[c].y) > 1e-9) {
                train_free(t);
                return snprintf(err, errlen, "train: %s is carried round by %s, but %s's axis is not the carrier's", ca >= 0 ? A->name : B->name,
                                S->shaft[c].name, ca >= 0 ? B->name : A->name),
                       NULL;
            }
        }
        t->frame[k] = c;
        const GearGeometry *X = &A->m->geo, *Y = &B->m->geo;
        double mtA = X->spec.module_m / cos(X->helix), mtB = Y->spec.module_m / cos(Y->helix);
        if (fabs(mtA / mtB - 1) > 1e-9 || fabs(X->spec.pressure_deg - Y->spec.pressure_deg) > 1e-9) {
            train_free(t);
            return snprintf(err, errlen, "train: %s and %s: a mesh needs the same module and pressure angle", A->name, B->name), NULL;
        }
        double sgn = (X->internal || Y->internal) ? 1 : -1;
        if (X->helix != 0 || Y->helix != 0)
            if (fabs(X->helix - sgn * Y->helix) > 1e-9) {
                train_free(t);
                return snprintf(err, errlen, "train: %s and %s: helical gears in mesh need equal helix angles, of opposite hands (external) or the same hand (internal)",
                                A->name, B->name),
                       NULL;
            }
        double need = centre_distance(X, Y), have = hypot(A->ax - B->ax, A->ay - B->ay);
        if (fabs(need - have) > 0.02 * X->spec.module_m) {
            train_free(t);
            return snprintf(err, errlen, "train: %s and %s are %.6g m apart; their shifts need %.6g m", A->name, B->name, have, need), NULL;
        }
    }
    char e3[256] = "";
    if (!kinematics(t, e3, sizeof e3) || !phase_gears(t, e3, sizeof e3) || !zones(t, e3, sizeof e3)) {
        snprintf(err, errlen, "%s", e3);
        train_free(t);
        return NULL;
    }
    /* the rigid degrees of freedom: every shaft's rotation (held ones too, for their reactions), floating translations */
    t->ndof = S->nshafts;
    for (int s = 0; s < S->nshafts; s++) {
        t->dofx[s] = -1;
        if (S->shaft[s].floating || S->shaft[s].support_n_per_m > 0) t->dofx[s] = t->ndof, t->ndof += 2;
    }
    if (t->ndof > MC_MAX_DOF) {
        train_free(t);
        return snprintf(err, errlen, "train: %d rigid degrees of freedom, at most %d", t->ndof, MC_MAX_DOF), NULL;
    }
    for (int d = 0; d < t->ndof; d++) t->reach[d] = 1;
    for (int s = 0; s < S->nshafts; s++) { /* a shaft's gears' outer radius, or a carrier's reach to its planets' teeth */
        t->reach[s] = 0;
        for (int g = 0; g < S->ngears; g++) {
            const GearGeometry *geo = &t->part[g].m->geo;
            int gs = S->gear[g].shaft;
            double ro = fmax(geo->r_a, geo->r_f); /* a ring's teeth reach out to its root */
            if (gs == s) t->reach[s] = fmax(t->reach[s], ro);
            else if (S->shaft[gs].carrier == s) t->reach[s] = fmax(t->reach[s], hypot(S->shaft[gs].x - S->shaft[s].x, S->shaft[gs].y - S->shaft[s].y) + ro);
        }
    }
    for (int g = 0; g < S->ngears; g++) {
        GearPart *p = &t->part[g];
        int s = S->gear[g].shaft, c = S->shaft[s].carrier;
        p->dof = s;
        if (t->dofx[s] >= 0) p->dofx = t->dofx[s], p->dofy = t->dofx[s] + 1;
        if (c >= 0) p->cdof = c, p->kx = S->shaft[c].x, p->ky = S->shaft[c].y;
        int nn = p->m->mesh.nnodes;
        t->f[g] = calloc((size_t)nn * 3, sizeof(double)), t->u[g] = calloc((size_t)nn * 3, sizeof(double));
        t->nstrain[g] = calloc((size_t)nn * 6, sizeof(double)), t->world[g] = calloc((size_t)nn * 3, sizeof(double));
        t->mises[g] = calloc((size_t)p->m->mesh.nelems, sizeof(float)), t->nmises[g] = calloc((size_t)nn, sizeof(float));
        if (!t->f[g] || !t->u[g] || !t->nstrain[g] || !t->world[g] || !t->mises[g] || !t->nmises[g]) {
            train_free(t);
            return snprintf(err, errlen, "out of memory"), NULL;
        }
    }
    if (!build_visuals(t)) {
        train_free(t);
        return snprintf(err, errlen, "out of memory"), NULL;
    }
    t->setup_seconds = now() - t0;
    int len = snprintf(t->summary, sizeof t->summary, "{\"shafts\":[");
    for (int s = 0; s < S->nshafts && len < (int)sizeof t->summary - 300; s++)
        len += snprintf(t->summary + len, sizeof t->summary - len, "%s{\"name\":\"%s\",\"ratio\":%.9g%s%s%s,\"support_n_per_m\":%.6g}", s ? "," : "", S->shaft[s].name,
                        t->ratio[s], S->shaft[s].fixed ? ",\"fixed\":true" : "", S->shaft[s].locked ? ",\"locked\":true" : "",
                        S->shaft[s].floating ? ",\"floating\":true" : "", S->shaft[s].support_n_per_m);
    len += snprintf(t->summary + len, sizeof t->summary - len, "],\"models\":[");
    for (int i = 0; i < S->nmodels && len < (int)sizeof t->summary - 300; i++) {
        const GearModel *m = t->model[i];
        len += snprintf(t->summary + len, sizeof t->summary - len,
                        "%s{\"name\":\"%s\",\"teeth\":%d,\"helix_deg\":%.4g,\"internal\":%s,\"gears\":%d,\"nodes\":%d,\"elements\":%d,\"equations\":%d,"
                        "\"zone_nodes\":%d,\"cyclic\":%s,\"factor_s\":%.2f,\"zone_s\":%.2f,\"min_jacobian_ratio\":%.3f}",
                        i ? "," : "", S->model[i].name, m->geo.spec.teeth, m->geo.spec.helix_deg, m->geo.internal ? "true" : "false", used[i], m->mesh.nnodes,
                        m->mesh.nelems, m->body.neq, m->body.nzone, m->body.zone_cyc > 0 ? "true" : "false", m->body.factor_seconds, m->body.zone_seconds,
                        m->mesh.min_jacobian_ratio);
    }
    snprintf(t->summary + len, sizeof t->summary - len, "],\"gears\":%d,\"meshes\":%d,\"rigid_dofs\":%d,\"solver\":\"%s\",\"setup_s\":%.1f}", S->ngears,
             S->nmeshes, t->ndof, mfactor_method(t->model[0]->body.K), t->setup_seconds);
    return t;
}

/* ---------------------------------------------------------------- solving */

static bool c_reserve(Train *t, int n) {
    if (n <= t->cap) return true;
    int cap = t->cap ? 2 * t->cap : 512;
    while (cap < n) cap *= 2;
    int *a = realloc(t->c_mesh, (size_t)cap * sizeof(int));
    if (!a) return false;
    t->c_mesh = a;
    if (!(a = realloc(t->c_side, (size_t)cap * sizeof(int)))) return false;
    t->c_side = a;
    if (!(a = realloc(t->c_ga, (size_t)cap * sizeof(int)))) return false;
    t->c_ga = a;
    if (!(a = realloc(t->c_gb, (size_t)cap * sizeof(int)))) return false;
    t->c_gb = a, t->cap = cap;
    return true;
}

/* every shaft's axis at the turns theta (with its carrier's, and a pin out of place), moved by its own translation
 * where it floats or sits on bearings (off: the rigid degrees of freedom's offsets from the kinematic pose, or NULL) */
static void place(Train *t, const double *theta, const double *off) {
    const TrainSpec *S = &t->spec;
    for (int g = 0; g < S->ngears; g++) {
        GearPart *p = &t->part[g];
        int s = S->gear[g].shaft;
        const TrainShaftSpec *sh = &S->shaft[s];
        if (sh->carrier < 0) {
            p->cx = sh->x, p->cy = sh->y;
        } else {
            const TrainShaftSpec *cr = &S->shaft[sh->carrier];
            double dx = sh->x - cr->x, dy = sh->y - cr->y, r = hypot(dx, dy), ga = atan2(dy, dx) + theta[sh->carrier];
            double et = sh->pin_error_m[0], er = sh->pin_error_m[1];
            p->cx = cr->x + (r + er) * cos(ga) - et * sin(ga), p->cy = cr->y + (r + er) * sin(ga) + et * cos(ga);
            p->kx = cr->x, p->ky = cr->y;
        }
        if (off && t->dofx[s] >= 0) p->cx += off[t->dofx[s]], p->cy += off[t->dofx[s] + 1];
    }
}

static bool build_contacts(Train *t, const double *theta, double qglob, char *err, size_t errlen) {
    const TrainSpec *S = &t->spec;
    MContactSet *cs = &t->cs;
    mcontact_clear(cs);
    cs->ndof = t->ndof;
    double gap_cut = S->gap_cut_m > 0 ? S->gap_cut_m : 60e-6;
    for (int k = 0; k < S->nmeshes; k++) {
        int ga = S->mesh[k].a, gb = S->mesh[k].b;
        if (t->part[ga].m->geo.internal) { int x = ga; ga = gb, gb = x; } /* A is the external gear */
        GearPart *A = &t->part[ga], *B = &t->part[gb];
        bool internal = B->m->geo.internal;
        double q = t->have_qline && t->qline[k] > 0 ? t->qline[k] : qglob;
        for (int side = 0; side < 2; side++) {
            int sA = side ? GM_RIGHT : GM_LEFT, sB = internal ? (side ? GM_LEFT : GM_RIGHT) : sA, n0 = cs->n;
            if (gearpart_mesh_contacts(cs, &t->nf, A, ga, sA, theta[S->gear[ga].shaft], B, gb, sB, theta[S->gear[gb].shaft], q, gap_cut, err, errlen) < 0)
                return false;
            if (!c_reserve(t, cs->n)) return snprintf(err, errlen, "out of memory"), false;
            /* the side as gear a of the mesh sees it */
            int sa = ga == S->mesh[k].a ? side : (internal ? 1 - side : side);
            for (int j = n0; j < cs->n; j++) t->c_mesh[j] = k, t->c_side[j] = sa, t->c_ga[j] = ga, t->c_gb[j] = gb;
        }
    }
    free(t->lambda);
    t->lambda = malloc((size_t)(cs->n ? cs->n : 1) * sizeof(double));
    if (!t->lambda) return snprintf(err, errlen, "out of memory"), false;
    return true;
}

/* the rigid motions q from the pose the contacts were found at, `off` from the kinematic pose: the soft springs and
 * the bearings act on the whole offset off + q; energy: the elastic energy stored at the solution */
static bool contact_solve(Train *t, double scale, const double *off, double *q, MContactResult *res, double qglob, double *energy, char *err,
                          size_t errlen) {
    const TrainSpec *S = &t->spec;
    double force[MC_MAX_DOF] = {0}, ksoft[MC_MAX_DOF];
    for (int d = 0; d < MC_MAX_DOF; d++) ksoft[d] = -1; /* held unless given a spring below */
    for (int s = 0; s < S->nshafts; s++) {
        const TrainShaftSpec *sh = &S->shaft[s];
        if (sh->fixed || sh->locked) {
            ksoft[s] = -1;
            continue;
        }
        force[s] = scale * sh->torque_nm;
        double k = 0;
        for (int g = 0; g < S->ngears; g++) { /* the gears on it, or carried by it */
            const GearPart *p = &t->part[g];
            int gs = S->gear[g].shaft;
            double r = gs == s ? p->m->geo.r : S->shaft[gs].carrier == s ? hypot(p->ax - sh->x, p->ay - sh->y) + p->m->geo.r : -1;
            if (r > 0) k = fmax(k, 1e-9 * p->m->E * p->m->geo.spec.face_width_m * r * r);
        }
        ksoft[s] = k > 0 ? k : 1;
        if (t->dofx[s] >= 0) { /* a bearing's stiffness, or a floating shaft's soft spring */
            double kt = 0;
            for (int g = 0; g < S->ngears; g++)
                if (S->gear[g].shaft == s) kt = fmax(kt, 1e-9 * t->part[g].m->E * t->part[g].m->geo.spec.face_width_m);
            if (sh->support_n_per_m > 0) kt = sh->support_n_per_m;
            ksoft[t->dofx[s]] = ksoft[t->dofx[s] + 1] = kt;
        }
    }
    for (int d = 0; d < t->ndof; d++)
        if (ksoft[d] >= 0) force[d] -= ksoft[d] * off[d];
    MBody *bodies[TR_MAX_GEARS];
    for (int g = 0; g < S->ngears; g++) bodies[g] = &t->part[g].m->body;
    double Estar = 0;
    for (int g = 0; g < S->ngears; g++) Estar = fmax(Estar, 0.5 * t->part[g].m->E / (1 - t->part[g].m->nu * t->part[g].m->nu));
    gearpart_near_field(&t->cs, &t->nf, NULL, scale * qglob, Estar);
    for (int pass = 0; pass < 3; pass++) {
        if (!mcontact_solve_q(bodies, S->ngears, &t->cs, force, ksoft, 1e-13, t->lambda, q, res, err, errlen)) return false;
        if (pass < 2) gearpart_near_field(&t->cs, &t->nf, t->lambda, scale * qglob, Estar);
    }
    /* the contacts' compliance (the parts' and the near field's, as last set) holds 1/2 lambda' W lambda, and W lambda is
     * minus the rigid part of each closed gap; the soft springs and bearings hold theirs on the whole offset */
    double U = 0;
    for (int j = 0; j < t->cs.n; j++) {
        if (!(t->lambda[j] > 0)) continue;
        double g = t->cs.g0[j];
        for (int d = 0; d < t->ndof; d++) g += t->cs.mq[MC_MAX_DOF * j + d] * q[d];
        U -= 0.5 * t->lambda[j] * g;
    }
    for (int d = 0; d < t->ndof; d++)
        if (ksoft[d] > 0) U += 0.5 * ksoft[d] * (off[d] + q[d]) * (off[d] + q[d]);
    *energy = U;
    return true;
}

/* the line load each mesh carries, from a solution (lambda at `scale` of the load) */
static void mesh_line_loads(Train *t, double scale) {
    const TrainSpec *S = &t->spec;
    double F[TR_MAX_MESHES] = {0};
    for (int j = 0; j < t->cs.n; j++) F[t->c_mesh[j]] += t->lambda[j] / scale;
    for (int k = 0; k < S->nmeshes; k++) {
        const GearGeometry *g = &t->part[S->mesh[k].a].m->geo;
        t->qline[k] = F[k] * cos(g->beta_b) / (1.5 * g->spec.face_width_m);
    }
    t->have_qline = true;
}

enum { TR_MAX_POSES = 60 };
#define TR_POSE_TOL 1e-9    /* m: a pose has settled when the last solve moves no gear's teeth further than this, */
#define TR_POSE_SWITCH 1e-7 /* or, once its steps swing to and fro across a contact that switches, when they move less */

/* The train settled at `scale` of its loads. The contacts are found, and their gaps and their rows of M (each gap's
 * change with the rigid motions) taken, at a pose: the kinematic turns moved by `off`, each rigid degree of freedom's
 * offset from them. A solve gives the rigid motions from there; the pose moves by them and the contacts are found
 * again, until the pose stops moving. A train with a high ratio turns its input degrees beyond its kinematic turn
 * before it carries its load (the backlash and the compliance of its far meshes, seen through the ratio), and over
 * such a turn the gaps taken at the kinematic pose do not hold: other tooth pairs come into mesh, and a tooth's corner,
 * whose gap is linear in the turns only near where it was found, closes where it cannot. Where a contact switches as
 * the pose crosses it (its nearest point jumping from a flank to a tooth's corner, say) the steps can swing to and fro
 * across it; then they are halved, and the pose has settled when a halved step moves the teeth less than TR_POSE_TOL
 * or a whole one less than TR_POSE_SWITCH (the solution sits on the switch; R->settle_m keeps the whole step). */
static bool settle(Train *t, double scale, double *off, MContactResult *res, double qglob, TrainResult *R, char *err, size_t errlen) {
    const TrainSpec *S = &t->spec;
    double th[TR_MAX_SHAFTS], dq[MC_MAX_DOF], last[MC_MAX_DOF] = {0}, move = INFINITY, lastmove = INFINITY, relax = 1;
    for (int it = 0; it < TR_MAX_POSES; it++) {
        for (int s = 0; s < S->nshafts; s++) th[s] = t->theta[s] + off[s];
        place(t, th, off);
        double t0 = now();
        if (!build_contacts(t, th, qglob, err, errlen)) return false;
        R->search_seconds += now() - t0;
        if (!contact_solve(t, scale, off, dq, res, qglob, &R->energy_j, err, errlen)) return false;
        mesh_line_loads(t, scale);
        R->poses++;
        double back = 0;
        move = 0;
        for (int d = 0; d < t->ndof; d++) move = fmax(move, fabs(dq[d]) * t->reach[d]), back += dq[d] * last[d] * t->reach[d] * t->reach[d];
        /* a step about as long as the last and turned back on it: the pose swings across a contact that switches there
         * (from a flank to a tooth's corner, say), and the steps are halved until they close in on it */
        if (back < 0 && move > 0.5 * lastmove) relax = fmax(0.5 * relax, 1.0 / 1024);
        for (int d = 0; d < t->ndof; d++) off[d] += relax * dq[d], last[d] = dq[d];
        lastmove = move;
        if (relax * move <= TR_POSE_TOL || (relax < 1 && move <= TR_POSE_SWITCH)) {
            R->settle_m = fmax(R->settle_m, move);
            return true;
        }
    }
    return snprintf(err, errlen, "train: at %.4g deg of input the pose did not settle in %d contact searches (the last solve moved the teeth %.3g m)", R->input_deg,
                    TR_MAX_POSES, move),
           false;
}

bool train_solve(Train *t, double input_deg, TrainResult *R, bool fields, char *err, size_t errlen) {
    memset(R, 0, sizeof *R);
    const TrainSpec *S = &t->spec;
    R->input_deg = input_deg;
    double phi = input_deg * M_PI / 180;
    for (int s = 0; s < S->nshafts; s++) t->theta[s] = t->ratio[s] * phi, R->turn_rad[s] = t->theta[s];
    /* equilibrium is possible only if a shaft is locked or the torques do no work as the train turns */
    bool locked = false;
    double work = 0, wabs = 0;
    for (int s = 0; s < S->nshafts; s++) {
        locked |= S->shaft[s].locked;
        if (!S->shaft[s].fixed) work += S->shaft[s].torque_nm * t->ratio[s], wabs += fabs(S->shaft[s].torque_nm * t->ratio[s]);
    }
    if (!locked && fabs(work) > 1e-9 * wabs)
        return snprintf(err, errlen, "train: the torques do not balance (their power as the train turns is %.4g W per rad/s): lock a shaft or balance them", work),
               false;
    /* a first guess of each mesh's line load: the largest torque at the smallest base radius on its shaft */
    double qglob = 0;
    for (int s = 0; s < S->nshafts; s++) {
        if (S->shaft[s].torque_nm == 0) continue;
        for (int g = 0; g < S->ngears; g++)
            if (S->gear[g].shaft == s)
                qglob = fmax(qglob, fabs(S->shaft[s].torque_nm) / (t->part[g].m->geo.r_b * t->part[g].m->geo.spec.face_width_m));
    }
    if (qglob <= 0) qglob = 1e4;
    MContactResult res;
    /* the rigid closure of the backlash: the same problem under a thousandth of the loads, whose own elastic share is
     * taken out below. (A millionth is too little where a high ratio amplifies the backlash: the soft springs that
     * hold unloaded parts would stop the closure short.) Each settles (above) from the last solve's pose if there is
     * one (the loaded pose from the closure's by the last solve's elastic share), else from the kinematic pose. */
    const double sc = 1e-3;
    double off[MC_MAX_DOF], el[MC_MAX_DOF];
    for (int d = 0; d < t->ndof; d++) off[d] = t->have_pose ? t->q0[d] : 0, el[d] = t->have_pose ? t->q[d] - t->q0[d] : 0;
    t->have_pose = false;
    double t0 = now(), s0 = R->search_seconds;
    if (!settle(t, sc, off, &res, qglob, R, err, errlen)) return false;
    R->closure_seconds = now() - t0 - (R->search_seconds - s0);
    memcpy(t->q0, off, sizeof(double) * (size_t)t->ndof);
    for (int d = 0; d < t->ndof; d++) off[d] += el[d];
    t0 = now(), s0 = R->search_seconds;
    if (!settle(t, 1, off, &res, qglob, R, err, errlen)) return false;
    R->solve_seconds = now() - t0 - (R->search_seconds - s0);
    memcpy(t->q, off, sizeof(double) * (size_t)t->ndof);
    t->have_pose = true;
    for (int s = 0; s < S->nshafts; s++) R->offset_rad[s] = t->q[s];
    R->contacts = t->cs.n, R->active = res.active, R->pivots = res.pivots;
    const MContactSet *cs = &t->cs;
    double side_force[TR_MAX_MESHES][2] = {{0}};
    for (int j = 0; j < cs->n; j++) {
        double l = t->lambda[j];
        if (l == 0) continue;
        int k = t->c_mesh[j], sa = S->gear[t->c_ga[j]].shaft, sb = S->gear[t->c_gb[j]].shaft;
        double nz = cs->normal[3 * j + 2];
        R->mesh_force_n[k] += l, side_force[k][t->c_side[j]] += l;
        R->mesh_axial_n[k] += t->c_ga[j] == S->mesh[k].a ? l * nz : -l * nz;
        R->axial_n[sa] += l * nz, R->axial_n[sb] -= l * nz;
        for (int d = 0; d < 2; d++) R->force_n[sa][d] += l * cs->normal[3 * j + d], R->force_n[sb][d] -= l * cs->normal[3 * j + d];
        for (int d = 0; d < S->nshafts; d++) R->torque_nm[d] += l * cs->mq[MC_MAX_DOF * j + d];
    }
    for (int k = 0; k < S->nmeshes; k++) {
        R->mesh_side[k] = side_force[k][0] + side_force[k][1] <= 0 ? -1 : side_force[k][1] > side_force[k][0];
        R->mesh_side_force_n[k][0] = side_force[k][0], R->mesh_side_force_n[k][1] = side_force[k][1];
    }
    for (int s = 0; s < S->nshafts; s++) {
        R->windup_rad[s] = (S->shaft[s].fixed || S->shaft[s].locked) ? 0 : (t->q[s] - t->q0[s]) / (1 - sc);
        if (t->dofx[s] >= 0) R->float_m[s][0] = t->q[t->dofx[s]], R->float_m[s][1] = t->q[t->dofx[s] + 1];
    }
    /* the pose with the wind-up, for pictures */
    double th[TR_MAX_SHAFTS];
    for (int s = 0; s < S->nshafts; s++) th[s] = t->theta[s] + t->q[s];
    place(t, th, t->q);
    for (int g = 0; g < S->ngears; g++) {
        GearPart *p = &t->part[g];
        double a = p->rot0 + th[S->gear[g].shaft], c = cos(a), sn = sin(a), ox = p->cx, oy = p->cy;
        for (int n = 0; n < p->m->mesh.nnodes; n++) {
            double x = p->m->mesh.xyz[3 * n], y = p->m->mesh.xyz[3 * n + 1];
            t->world[g][3 * n] = c * x - sn * y + ox, t->world[g][3 * n + 1] = sn * x + c * y + oy, t->world[g][3 * n + 2] = p->m->mesh.xyz[3 * n + 2] + p->az;
        }
    }
    for (int i = 0; i < t->nvis; i++) {
        Visual *v = &t->vis[i];
        const TrainShaftSpec *sh = &S->shaft[v->owner];
        double c = cos(th[v->owner]), sn = sin(th[v->owner]), ox = sh->x, oy = sh->y;
        if (t->dofx[v->owner] >= 0) ox += t->q[t->dofx[v->owner]], oy += t->q[t->dofx[v->owner] + 1];
        for (int n = 0; n < v->nnodes; n++) {
            const double *X = &v->local[3 * n];
            v->world[3 * n] = c * X[0] - sn * X[1] + ox, v->world[3 * n + 1] = sn * X[0] + c * X[1] + oy, v->world[3 * n + 2] = X[2];
        }
    }
    place(t, t->theta, NULL);
    if (!fields) return true;
    MBody *bodies[TR_MAX_GEARS];
    for (int g = 0; g < S->ngears; g++) bodies[g] = &t->part[g].m->body;
    mcontact_forces(bodies, S->ngears, cs, t->lambda, t->f);
    for (int g = 0; g < S->ngears; g++) {
        GearPart *p = &t->part[g];
        MBody *body = &p->m->body;
        if (!mbody_solve(body, t->f[g], t->u[g])) return snprintf(err, errlen, "%s: the solve failed", p->name), false;
        mbody_fields(body, t->u[g], t->nstrain[g], NULL, t->mises[g]);
        mbody_node_mises(body, t->nstrain[g], t->nmises[g]);
        for (int e = 0; e < p->m->mesh.nelems; e++) R->gear_max_mises_pa[g] = fmax(R->gear_max_mises_pa[g], t->mises[g][e]);
        R->max_mises_pa = fmax(R->max_mises_pa, R->gear_max_mises_pa[g]);
    }
    return true;
}
