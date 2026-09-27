/* wt_scenario.c - water in a tank described in JSON (docs/lab/water.md has every key): columns of water that collapse,
 * obstacles they run into, an initial sloshing wave. The water's density must say where it comes from ("source"). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "../labshape.h"
#include "water.h"
#include "water_metal.h"

typedef struct Slosh {
    double depth, amp, length;
} Slosh;

static double slosh_surface(double x, double y, void *ctx) {
    (void)y;
    const Slosh *s = ctx;
    return s->depth + s->amp * cos(M_PI * x / s->length);
}

/* solids besides the tank: a sloping beach (a plane rising from its toe) and bodies from the shape library */
typedef struct Solids {
    bool beach;
    double toe, slope;
    LabShapes shapes;
} Solids;
static double solids_sdf(const double p[3], void *ctx) {
    const Solids *S = ctx;
    double d = S->shapes.n ? labshape_sdf(&S->shapes, p) : 1e30;
    if (S->beach) d = fmin(d, (p[2] - S->slope * (p[0] - S->toe)) / sqrt(1 + S->slope * S->slope));
    return d;
}

/* a solitary wave on still water of depth d: eta = H sech^2(k (x - x0)), k = sqrt(3 H / (4 d^3)) (Boussinesq), the
 * water under it moving at c eta / (d + eta), c = sqrt(g (d + H)) */
typedef struct Solitary {
    double H, d, x0, k, c;
} Solitary;
static double solitary_surface(double x, double y, void *ctx) {
    (void)y;
    const Solitary *w = ctx;
    double s = 1 / cosh(w->k * (x - w->x0));
    return w->d + w->H * s * s;
}

/* a box as 8 nodes and 6 quads, appended */
static void box_mesh(const double *b, double *xyz, int *nn, int *conn, int *nq) {
    int base = *nn;
    for (int k = 0; k < 8; k++) {
        xyz[3 * (base + k)] = b[(k & 1) ? 3 : 0], xyz[3 * (base + k) + 1] = b[(k & 2) ? 4 : 1], xyz[3 * (base + k) + 2] = b[(k & 4) ? 5 : 2];
    }
    static const int F[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 4; k++) conn[4 * (*nq + f) + k] = base + F[f][k];
    *nn += 8, *nq += 6;
}

bool lab_run_water(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    WtSpec s;
    wt_spec_defaults(&s);
    const JsonValue *wat = json_get(root, "water"), *num = json_get(root, "numerics"), *run = json_get(root, "run");
    if (!wat || !json_get_str(wat, "source", NULL) || !(json_get_num(wat, "density_kg_m3", -1) > 0)) {
        snprintf(err, errlen, "water: \"water\" needs density_kg_m3 and the source it comes from");
        return false;
    }
    s.rho0 = json_get_num(wat, "density_kg_m3", 1000);
    s.dx = json_get_num(root, "particle_spacing_m", -1);
    s.g = json_get_num(root, "gravity_m_s2", 9.80665);
    s.h_factor = json_get_num(num, "h_factor", s.h_factor), s.alpha = json_get_num(num, "alpha", s.alpha);
    s.delta = json_get_num(num, "delta", s.delta), s.cfl = json_get_num(num, "cfl", s.cfl), s.c0 = json_get_num(num, "c0_m_s", 0);
    double T[3];
    if (!(s.dx > 0) || !json_get_numbers(json_get(json_get(root, "tank"), "size_m"), T, 3) || !(T[0] > 0 && T[1] > 0 && T[2] > 0)) {
        snprintf(err, errlen, "water: particle_spacing_m and tank.size_m [x, y, z] (the inside, from the origin) are required");
        return false;
    }
    int layers = (int)ceil(2 * (s.h_factor > 0 ? s.h_factor : 1.7));
    double t = layers * s.dx;
    for (int a = 0; a < 3; a++) s.lo[a] = -t - s.dx, s.hi[a] = T[a] + t + s.dx;
    s.hi[2] = T[2] * 1.5 + t; /* splashes may rise above the walls; they fall back */
    Wt *w = wt_create(&s, err, errlen);
    if (!w) return false;
    double lo[3] = {0, 0, 0};
    wt_add_tank(w, lo, T, layers);
    const JsonValue *obs = json_get(root, "obstacles");
    int nobs = 0;
    double ob[16][6];
    for (size_t k = 0; obs && k < json_len(obs) && nobs < 16; k++)
        if (json_get_numbers(json_get(json_at(obs, k), "box_m"), ob[nobs], 6)) {
            wt_add_solid_box(w, ob[nobs], &ob[nobs][3], layers, 0);
            nobs++;
        }
    const JsonValue *cols = json_get(root, "columns");
    for (size_t k = 0; cols && k < json_len(cols); k++) {
        double b[6];
        if (json_get_numbers(json_get(json_at(cols, k), "box_m"), b, 6)) wt_add_water(w, b, &b[3], NULL, NULL);
    }
    /* a beach and bodies, then still water with a solitary wave on it, clear of them */
    Solids sol = {0};
    const JsonValue *bj = json_get(root, "beach");
    if (bj) sol.beach = true, sol.toe = json_get_num(bj, "toe_x_m", 0), sol.slope = json_get_num(bj, "slope", 0.1);
    if (!labshape_parse(json_get(root, "bodies"), &sol.shapes, err, errlen)) {
        wt_free(w);
        return false;
    }
    if (sol.beach || sol.shapes.n) wt_add_solid_fn(w, lo, T, solids_sdf, &sol, layers);
    const JsonValue *swj = json_get(root, "solitary_wave"), *dj = json_get(root, "still_water");
    Solitary sw = {0};
    if (swj || dj) {
        sw.d = json_get_num(swj ? swj : dj, "depth_m", 0), sw.H = swj ? json_get_num(swj, "height_m", 0) : 0;
        sw.x0 = json_get_num(swj, "crest_x_m", 0), sw.k = sw.H > 0 ? sqrt(3 * sw.H / (4 * sw.d * sw.d * sw.d)) : 0;
        sw.c = sqrt(s.g * (sw.d + sw.H));
        double hi[3] = {T[0], T[1], sw.d + sw.H};
        int first = wt_count(w);
        wt_add_water_fn(w, lo, hi, solitary_surface, &sw, (sol.beach || sol.shapes.n) ? solids_sdf : NULL, &sol);
        double *vv = wt_v_rw(w);
        const double *xx = wt_x(w);
        for (int i = first; sw.H > 0 && i < wt_count(w); i++) {
            double eta = solitary_surface(xx[3 * i], 0, &sw) - sw.d;
            vv[3 * i] = sw.c * eta / (sw.d + eta);
        }
    }
    Slosh sl = {0};
    const JsonValue *sj = json_get(root, "sloshing");
    if (sj) {
        sl.depth = json_get_num(sj, "depth_m", 0), sl.amp = json_get_num(sj, "amplitude_m", 0), sl.length = T[0];
        double hi[3] = {T[0], T[1], sl.depth + fabs(sl.amp)};
        wt_add_water(w, lo, hi, slosh_surface, &sl);
    }
    if (wt_count(w) == 0) {
        snprintf(err, errlen, "water: no water (give columns, sloshing, still_water or solitary_wave)");
        wt_free(w);
        return false;
    }
    double end = json_get_num(run, "end_s", 2.0);
    int frames = (int)json_get_int(run, "frames", 60);
    char title[200], hdr[1400];
    snprintf(title, sizeof title, "%s", json_get_str(root, "title", "water"));
    snprintf(hdr, sizeof hdr,
             "{\"domain\":\"water\",\"title\":\"%s\",\"solver\":\"src/lab/water: weakly compressible SPH, Wendland C2 kernel, Tait "
             "equation of state, delta-SPH density diffusion, boundary particles after Adami et al. 2012\",\"particle_spacing_m\":%g,"
             "\"particles\":%d,\"fields\":{\"speed\":\"m/s\",\"p\":\"Pa\"}}",
             title, s.dx, wt_count(w));
    LabWriter *lw = lab_create(out, hdr, err, errlen);
    if (!lw) {
        wt_free(w);
        return false;
    }
    /* the tank's edges and the obstacles, the same in every frame */
    double txyz[8 * 3];
    int tconn[12 * 2];
    for (int k = 0; k < 8; k++) txyz[3 * k] = (k & 1) ? T[0] : 0, txyz[3 * k + 1] = (k & 2) ? T[1] : 0, txyz[3 * k + 2] = (k & 4) ? T[2] : 0;
    static const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int e = 0; e < 12; e++) tconn[2 * e] = E[e][0], tconn[2 * e + 1] = E[e][1];
    double oxyz[16 * 8 * 3];
    int oconn[16 * 6 * 4], onn = 0, onq = 0;
    for (int k = 0; k < nobs; k++) box_mesh(ob[k], oxyz, &onn, oconn, &onq);
    double bq[12];
    int bconn4[4] = {0, 1, 2, 3};
    if (sol.beach) {
        double xe = fmin(T[0], sol.toe + T[2] / sol.slope), ze = sol.slope * (xe - sol.toe);
        double q[12] = {sol.toe, 0, 0, xe, 0, ze, xe, T[1], ze, sol.toe, T[1], 0};
        memcpy(bq, q, sizeof q);
    }
    int sbt = 0, sbn = 0, *sconn = NULL;
    double *sxyz = NULL;
    if (sol.shapes.n) {
        double mlo[3] = {0, 0, 0};
        double *soup = labshape_mesh(&sol.shapes, mlo, T, 0.5 * s.dx, &sbt);
        if (sbt && (sbn = labshape_weld(soup, sbt, &sxyz, &sconn)) < 0) sbt = 0;
        free(soup);
    }
    int n = wt_count(w);
    float *spd = malloc((size_t)n * sizeof(float)), *pr = malloc((size_t)n * sizeof(float));
    double *xyz = malloc((size_t)n * 3 * sizeof(double));
    bool ok = spd && pr && xyz;
    double front0 = INFINITY, front = 0, next = 0, pmax = 0;
    /* the GPU engine (water_metal.h, verified against this CPU engine by wtest) unless "engine": "cpu" */
    WtGpu *G = NULL;
    bool gpu_bad = false;
    if (strcmp(json_get_str(root, "engine", "gpu"), "cpu")) {
        char gerr[256];
        G = wt_gpu_create(w, gerr, sizeof gerr);
        if (!G && !quiet) fprintf(stderr, "  water: %s; running on the CPU\n", gerr);
    }
    for (int f = 0; f < frames && ok; f++) {
        double tf = frames > 1 ? end * f / (frames - 1) : 0;
        if (G) {
            if (!wt_gpu_run_to(G, tf)) gpu_bad = true;
            wt_gpu_sync(G, w);
        } else
            while (wt_time(w) < tf && !wt_unstable(w)) wt_step(w);
        if (wt_unstable(w) || gpu_bad) {
            snprintf(err, errlen, "water: unstable at t = %.4g s", wt_time(w));
            ok = false;
            break;
        }
        const double *x = wt_x(w), *v = wt_v(w);
        int m = 0;
        double fx = INFINITY;
        for (int i = 0; i < n; i++) {
            if (x[3 * i] < -0.5 * s.dx || x[3 * i] > T[0] + 0.5 * s.dx || x[3 * i + 2] < -s.dx) continue; /* escaped */
            memcpy(&xyz[3 * m], &x[3 * i], 3 * sizeof(double));
            spd[m] = (float)sqrt(v[3 * i] * v[3 * i] + v[3 * i + 1] * v[3 * i + 1] + v[3 * i + 2] * v[3 * i + 2]);
            pr[m] = (float)wt_pressure(w, i);
            pmax = fmax(pmax, pr[m]);
            if (x[3 * i + 2] < 2 * s.dx) fx = fmin(fx, x[3 * i]); /* the surge's front along the floor */
            m++;
        }
        if (f == 0) front0 = fx;
        front = fx;
        lab_frame_begin(lw, wt_time(w));
        lab_part_points(lw, "water", m, xyz);
        lab_field(lw, "speed", LAB_AT_NODE, (size_t)m, spd);
        lab_field(lw, "p", LAB_AT_NODE, (size_t)m, pr);
        lab_part_cells(lw, "tank", 8, txyz, 12, LAB_LINE, tconn);
        if (nobs) lab_part_cells(lw, "obstacle", onn, oxyz, onq, LAB_QUAD, oconn);
        if (sol.beach) lab_part_cells(lw, "beach", 4, bq, 1, LAB_QUAD, bconn4);
        if (sbt) lab_part_cells(lw, "body", sbn, sxyz, sbt, LAB_TRI, sconn);
        if (!lab_frame_end(lw)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", f);
            ok = false;
        }
        if (!quiet && wt_time(w) >= next) {
            fprintf(stderr, "  t = %.3f s  frame %d/%d  %ld steps\n", wt_time(w), f + 1, frames, wt_steps(w));
            next += end / 10;
        }
    }
    snprintf(info->diagnostics, sizeof info->diagnostics,
             "{\"particles\":%d,\"walls\":%d,\"c0_m_s\":%.4g,\"front_start_m\":%.4g,\"front_end_m\":%.4g,\"max_pressure_pa\":%.4g,"
             "\"engine\":\"%s\"}",
             n, wt_wall_count(w), wt_c0(w), front0, front, pmax, G ? wt_gpu_name(G) : "CPU, double");
    wt_gpu_free(G);
    info->steps = wt_steps(w);
    info->frames = lab_frames_written(lw);
    free(spd), free(pr), free(xyz), free(sxyz), free(sconn);
    if (!lab_close(lw) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    wt_free(w);
    return ok;
}
