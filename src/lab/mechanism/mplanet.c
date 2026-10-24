/* mplanet.c - see mplanet.h */
#include "mplanet.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* parts: 0 the sun, 1..N the planets, N + 1 the ring; the contact problem's bodies in the same order; its rigid
 * degrees of freedom: 0 the sun's rotation, 1..N the planets' */
struct Planetary {
    PlanetarySpec spec;
    int np;
    GearModel *msun, *mplanet, *mring;  /* the planets share one model */
    GearPart part[PL_MAX_PLANETS + 2];
    double carrier_r;                 /* the pins' radius */
    double gamma0[PL_MAX_PLANETS];    /* each pin's polar angle at carrier angle 0 */
    MContactSet cs;
    NearField nf;
    int *mesh_of;                     /* per contact: the mesh it belongs to (0..N-1 sun-planet k, N..2N-1 planet k-ring) */
    int mesh_cap;
    double *lambda;
    double *f[PL_MAX_PLANETS + 2], *u[PL_MAX_PLANETS + 2], *nstrain[PL_MAX_PLANETS + 2], *world[PL_MAX_PLANETS + 2];
    float *mises[PL_MAX_PLANETS + 2], *nmises[PL_MAX_PLANETS + 2];
    double setup_seconds;
    char summary[2048];
};

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

void planetary_free(Planetary *pl) {
    if (!pl) return;
    gearmodel_release(pl->msun), gearmodel_release(pl->mplanet), gearmodel_release(pl->mring);
    for (int i = 0; i < pl->np + 2; i++) {
        free(pl->f[i]), free(pl->u[i]), free(pl->nstrain[i]), free(pl->world[i]), free(pl->mises[i]), free(pl->nmises[i]);
    }
    mcontact_free(&pl->cs);
    nearfield_free(&pl->nf);
    free(pl->mesh_of), free(pl->lambda);
    free(pl);
}

int planetary_parts(const Planetary *pl) { return pl->np + 2; }
const char *planetary_summary(const Planetary *pl) { return pl->summary; }

void planetary_part(Planetary *pl, int i, const char **name, const double **xyz, int *nnodes, const int **conn, int *nelems, const float **mises,
                    const float **node_mises) {
    GearPart *p = &pl->part[i];
    *name = p->name, *xyz = pl->world[i], *nnodes = p->m->mesh.nnodes, *conn = p->m->mesh.conn, *nelems = p->m->mesh.nelems;
    *mises = pl->mises[i], *node_mises = pl->nmises[i];
}

Planetary *planetary_create(const PlanetarySpec *spec, char *err, size_t errlen) {
    double t0 = now();
    Planetary *pl = calloc(1, sizeof *pl);
    if (!pl) return snprintf(err, errlen, "out of memory"), NULL;
    pl->spec = *spec;
    const PlanetarySpec *S = &pl->spec;
    int N = S->nplanets, zs = S->sun.teeth, zp = S->planet.teeth, zr = S->ring.teeth;
    pl->np = N;
    if (N < 1 || N > PL_MAX_PLANETS) {
        planetary_free(pl);
        return snprintf(err, errlen, "planetary: 1 to %d planets", PL_MAX_PLANETS), NULL;
    }
    /* evenly spaced planets mesh with the sun and the ring together only if (z_s + z_r) / N is whole */
    if ((zs + zr) % N) {
        planetary_free(pl);
        return snprintf(err, errlen, "planetary: %d planets cannot be spaced evenly: (z_sun + z_ring) / N = (%d + %d) / %d is not whole", N, zs, zr, N), NULL;
    }
    /* the pins' radius: the sun-planet operating centre distance, which the planet-ring mesh must share */
    double a_sp = gear_centre_distance(&S->sun, &S->planet);
    double alpha = S->ring.pressure_deg * M_PI / 180, target = gear_inv(alpha) + 2 * tan(alpha) * (S->ring.shift - S->planet.shift) / (zr - zp);
    double lo = 0.01, hi = 1.5;
    for (int i = 0; i < 200; i++) {
        double mid = 0.5 * (lo + hi);
        if (gear_inv(mid) < target) lo = mid;
        else hi = mid;
    }
    double a_pr = S->ring.module_m * (zr - zp) / 2 * cos(alpha) / cos(0.5 * (lo + hi));
    if (fabs(a_sp - a_pr) > 0.05 * S->sun.module_m) {
        planetary_free(pl);
        return snprintf(err, errlen, "planetary: the sun-planet and planet-ring centre distances differ (%.6g and %.6g m); match the shifts", a_sp, a_pr), NULL;
    }
    pl->carrier_r = a_sp;
    /* phases at carrier angle 0: planet 0 on +y, a ring space centred on +y; each planet's tooth points at the ring's
     * space by its own ray (the ring's phase there, rolled onto the planet's pitch circle); the sun's from planet 0 */
    double ang_ring = M_PI / 2 + M_PI / zr;
    double beta[PL_MAX_PLANETS];
    for (int k = 0; k < N; k++) {
        pl->gamma0[k] = M_PI / 2 + 2 * M_PI * k / N;
        double d = remainder(pl->gamma0[k] - M_PI / 2, 2 * M_PI / zr);
        beta[k] = pl->gamma0[k] - (double)zr / zp * d;
    }
    double eps = remainder(M_PI, 2 * M_PI / zp); /* the planet's tooth (0) or space (pi / z_p) facing the sun */
    double ang_sun = fabs(eps) < 1e-9 ? M_PI / 2 + M_PI / zs : M_PI / 2;
    char e2[256];
    if (!(pl->msun = gearmodel_build(&S->sun, NULL, &S->mesh_sun, S->E[0], S->nu[0], true, GM_LEFT, e2, sizeof e2))) {
        snprintf(err, errlen, "sun: %s", e2);
        planetary_free(pl);
        return NULL;
    }
    if (!(pl->mplanet = gearmodel_build(&S->planet, NULL, &S->mesh_planet, S->E[1], S->nu[1], true, GM_LEFT | GM_RIGHT, e2, sizeof e2))) {
        snprintf(err, errlen, "planet: %s", e2);
        planetary_free(pl);
        return NULL;
    }
    if (!(pl->mring = gearmodel_build(&S->ring, &S->cutter, &S->mesh_ring, S->E[2], S->nu[2], true, GM_LEFT, e2, sizeof e2))) {
        snprintf(err, errlen, "ring: %s", e2);
        planetary_free(pl);
        return NULL;
    }
    gearpart_init(&pl->part[0], "sun", pl->msun, 0, 0, 0, ang_sun);
    for (int k = 0; k < N; k++) {
        char name[32];
        snprintf(name, sizeof name, "planet %d", k + 1);
        gearpart_init(&pl->part[1 + k], name, pl->mplanet, pl->carrier_r * cos(pl->gamma0[k]), pl->carrier_r * sin(pl->gamma0[k]), 0, beta[k]);
    }
    gearpart_init(&pl->part[N + 1], "ring", pl->mring, 0, 0, 0, ang_ring);
    pl->part[0].dof = 0;
    for (int k = 0; k < N; k++) pl->part[1 + k].dof = 1 + k;
    if (S->sun_floating) pl->part[0].dofx = N + 1, pl->part[0].dofy = N + 2;
    for (int i = 0; i < N + 2; i++) {
        int nn = pl->part[i].m->mesh.nnodes;
        pl->f[i] = calloc((size_t)nn * 3, sizeof(double)), pl->u[i] = calloc((size_t)nn * 3, sizeof(double));
        pl->nstrain[i] = calloc((size_t)nn * 6, sizeof(double)), pl->world[i] = calloc((size_t)nn * 3, sizeof(double));
        pl->mises[i] = calloc((size_t)pl->part[i].m->mesh.nelems, sizeof(float)), pl->nmises[i] = calloc((size_t)nn, sizeof(float));
        if (!pl->f[i] || !pl->u[i] || !pl->nstrain[i] || !pl->world[i] || !pl->mises[i] || !pl->nmises[i]) {
            planetary_free(pl);
            return snprintf(err, errlen, "out of memory"), NULL;
        }
    }
    pl->setup_seconds = now() - t0;
    int len = snprintf(pl->summary, sizeof pl->summary, "{\"ratio\":%.6g,\"carrier_radius_m\":%.6g,\"parts\":[", 1 + (double)zr / zs, pl->carrier_r);
    for (int i = 0; i < N + 2 && len < (int)sizeof pl->summary - 200; i++) {
        GearPart *p = &pl->part[i];
        len += snprintf(pl->summary + len, sizeof pl->summary - len, "%s{\"name\":\"%s\",\"teeth\":%d,\"nodes\":%d,\"equations\":%d,\"zone_nodes\":%d,\"min_jacobian_ratio\":%.3f}",
                        i ? "," : "", p->name, p->m->geo.spec.teeth, p->m->mesh.nnodes, p->m->body.neq, p->m->body.nzone, p->m->mesh.min_jacobian_ratio);
    }
    snprintf(pl->summary + len, sizeof pl->summary - len, "],\"solver\":\"%s\",\"setup_s\":%.1f}", mfactor_method(pl->msun->body.K), pl->setup_seconds);
    return pl;
}

static bool mesh_reserve(Planetary *pl, int n) {
    if (n <= pl->mesh_cap) return true;
    int cap = pl->mesh_cap ? 2 * pl->mesh_cap : 256;
    while (cap < n) cap *= 2;
    int *m = realloc(pl->mesh_of, (size_t)cap * sizeof(int));
    if (!m) return false;
    pl->mesh_of = m, pl->mesh_cap = cap;
    return true;
}

/* the contact problem at the current placement, for a sun torque */
static bool contact_solve(Planetary *pl, double torque, double *q, MContactResult *res, double q_est, double Estar, char *err, size_t errlen) {
    int N = pl->np, nd = pl->cs.ndof;
    double force[MC_MAX_DOF] = {0}, ksoft[MC_MAX_DOF];
    force[0] = torque;
    for (int d = 0; d <= N; d++) {
        const GearPart *p = &pl->part[d];
        ksoft[d] = 1e-9 * p->m->E * p->m->geo.spec.face_width_m * p->m->geo.r * p->m->geo.r;
    }
    for (int d = N + 1; d < nd; d++) ksoft[d] = 1e-9 * pl->msun->E * pl->msun->geo.spec.face_width_m; /* the sun's float */
    MBody *bodies[PL_MAX_PLANETS + 2];
    for (int i = 0; i < N + 2; i++) bodies[i] = &pl->part[i].m->body;
    gearpart_near_field(&pl->cs, &pl->nf, NULL, q_est, Estar);
    for (int pass = 0; pass < 3; pass++) {
        if (!mcontact_solve_q(bodies, N + 2, &pl->cs, force, ksoft, 1e-13, pl->lambda, q, res, err, errlen)) return false;
        if (pass < 2) gearpart_near_field(&pl->cs, &pl->nf, pl->lambda, q_est, Estar);
    }
    return true;
}

bool planetary_solve(Planetary *pl, double carrier_deg, PlanetaryResult *R, bool fields, char *err, size_t errlen) {
    memset(R, 0, sizeof *R);
    const PlanetarySpec *S = &pl->spec;
    int N = pl->np, zs = S->sun.teeth, zp = S->planet.teeth, zr = S->ring.teeth;
    double pc = carrier_deg * M_PI / 180, phi[PL_MAX_PLANETS + 2] = {0};
    phi[0] = pc * (1 + (double)zr / zs);
    for (int k = 0; k < N; k++) {
        GearPart *p = &pl->part[1 + k];
        double g = pl->gamma0[k] + pc, et = S->pin_error_m[k][0], er = S->pin_error_m[k][1];
        p->cx = (pl->carrier_r + er) * cos(g) - et * sin(g), p->cy = (pl->carrier_r + er) * sin(g) + et * cos(g);
        phi[1 + k] = pc * (1 - (double)zr / zp);
    }
    /* contacts: the sun's left flanks drive the planets' left flanks; the planets' right flanks bear on the ring's
     * left flanks (a planet's tooth is loaded on one flank by the sun and on the other by the ring) */
    MContactSet *cs = &pl->cs;
    mcontact_clear(cs);
    cs->ndof = N + 1 + (S->sun_floating ? 2 : 0);
    double gap_cut = S->gap_cut_m > 0 ? S->gap_cut_m : 60e-6, bw = S->sun.face_width_m;
    double q_est = fabs(S->sun_torque_nm) / (N * pl->msun->geo.r_b * bw);
    GearPart *sun = &pl->part[0], *ring = &pl->part[N + 1];
    for (int k = 0; k < N; k++) {
        GearPart *p = &pl->part[1 + k];
        int n0 = cs->n;
        if (gearpart_mesh_contacts(cs, &pl->nf, sun, 0, GM_LEFT, phi[0], p, 1 + k, GM_LEFT, phi[1 + k], q_est, gap_cut, err, errlen) < 0) return false;
        int n1 = cs->n;
        if (gearpart_mesh_contacts(cs, &pl->nf, p, 1 + k, GM_RIGHT, phi[1 + k], ring, N + 1, GM_LEFT, 0, q_est, gap_cut, err, errlen) < 0) return false;
        if (!mesh_reserve(pl, cs->n)) return snprintf(err, errlen, "out of memory"), false;
        for (int j = n0; j < n1; j++) pl->mesh_of[j] = k;
        for (int j = n1; j < cs->n; j++) pl->mesh_of[j] = N + k;
    }
    if (getenv("OPAI_MECH_DEBUG")) { /* each mesh's smallest unloaded gap: equal across planets when the phases are right */
        fprintf(stderr, "  debug: smallest gaps (um), sun-planet:");
        for (int m = 0; m < 2 * N; m++) {
            double gmin = INFINITY;
            for (int j = 0; j < cs->n; j++)
                if (pl->mesh_of[j] == m) gmin = fmin(gmin, cs->g0[j]);
            fprintf(stderr, "%s %.3f", m == N ? "; planet-ring:" : "", 1e6 * gmin);
        }
        fprintf(stderr, "\n");
    }
    free(pl->lambda);
    pl->lambda = malloc((size_t)(cs->n ? cs->n : 1) * sizeof(double));
    if (!pl->lambda) return snprintf(err, errlen, "out of memory"), false;
    double Estar = 1 / ((1 - S->nu[0] * S->nu[0]) / S->E[0] + (1 - S->nu[1] * S->nu[1]) / S->E[1]);
    double q[MC_MAX_DOF], q0[MC_MAX_DOF];
    MContactResult res;
    /* the rigid closure of the backlash: the same problem under a torque a millionth as large */
    if (!contact_solve(pl, 1e-6 * S->sun_torque_nm, q0, &res, 1e-6 * q_est, Estar, err, errlen)) return false;
    if (!contact_solve(pl, S->sun_torque_nm, q, &res, q_est, Estar, err, errlen)) return false;
    R->carrier_deg = carrier_deg;
    R->sun_windup_rad = q[0] - q0[0];
    if (S->sun_floating) R->sun_float_m[0] = q[N + 1], R->sun_float_m[1] = q[N + 2];
    for (int k = 0; k < N; k++) R->planet_turn_rad[k] = q[1 + k];
    R->contacts = cs->n, R->active = res.active, R->pivots = res.pivots;
    /* forces and torques: a contact's normal points from its part B toward its part A, and pushes A along it */
    double ts = 0, tr = 0, tc = 0;
    for (int k = 0; k < N; k++) {
        double fx = 0, fy = 0;
        for (int j = 0; j < cs->n; j++) {
            if (pl->mesh_of[j] != k && pl->mesh_of[j] != N + k) continue;
            double l = pl->lambda[j], nx = cs->normal[3 * j], ny = cs->normal[3 * j + 1], wx = cs->where[3 * j], wy = cs->where[3 * j + 1];
            if (pl->mesh_of[j] == k) { /* A the sun, B the planet */
                R->sun_mesh_force_n[k] += l;
                ts += wx * l * ny - wy * l * nx;
                fx -= l * nx, fy -= l * ny;
            } else { /* A the planet, B the ring */
                R->ring_mesh_force_n[k] += l;
                tr += wx * -l * ny - wy * -l * nx;
                fx += l * nx, fy += l * ny;
            }
        }
        GearPart *p = &pl->part[1 + k];
        tc += p->cx * fy - p->cy * fx; /* the gears' forces on a free planet act through its pin */
    }
    double mean = 0;
    for (int k = 0; k < N; k++) mean += R->sun_mesh_force_n[k] / N;
    for (int k = 0; k < N; k++) R->load_share[k] = mean > 0 ? R->sun_mesh_force_n[k] / mean : 0;
    R->sun_torque_check_nm = ts, R->ring_torque_nm = tr, R->carrier_torque_nm = tc;
    if (!fields) return true;
    MBody *bodies[PL_MAX_PLANETS + 2];
    for (int i = 0; i < N + 2; i++) bodies[i] = &pl->part[i].m->body;
    mcontact_forces(bodies, N + 2, cs, pl->lambda, pl->f);
    for (int i = 0; i < N + 2; i++) {
        GearPart *p = &pl->part[i];
        MBody *body = &p->m->body;
        if (!mbody_solve(body, pl->f[i], pl->u[i])) return snprintf(err, errlen, "%s: the solve failed", p->name), false;
        mbody_fields(body, pl->u[i], pl->nstrain[i], NULL, pl->mises[i]);
        mbody_node_mises(body, pl->nstrain[i], pl->nmises[i]);
        for (int e = 0; e < p->m->mesh.nelems; e++) R->max_mises_pa = fmax(R->max_mises_pa, pl->mises[i][e]);
        double a = p->rot0 + phi[i] + (p->dof >= 0 ? q[p->dof] : 0), c = cos(a), s = sin(a);
        double ox = p->cx + (p->dofx >= 0 ? q[p->dofx] : 0), oy = p->cy + (p->dofy >= 0 ? q[p->dofy] : 0);
        for (int n = 0; n < p->m->mesh.nnodes; n++) {
            double x = p->m->mesh.xyz[3 * n], y = p->m->mesh.xyz[3 * n + 1];
            pl->world[i][3 * n] = c * x - s * y + ox, pl->world[i][3 * n + 1] = s * x + c * y + oy, pl->world[i][3 * n + 2] = p->m->mesh.xyz[3 * n + 2] + p->az;
        }
    }
    return true;
}
