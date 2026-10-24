/* fr_scenario.c - a brittle body that falls, hits and breaks (peri.h), described in SI (docs/lab/fracture.md): the
 * material and its source, the body (labshape.h), its velocity, the floor, the lattice spacing and the run.
 *
 * Written per frame: the points of the body with their damage (the share of their bonds broken) and speed, and the
 * floor. The run record gives the bonds broken, the energies, and the number of fragments (groups of points still
 * joined by intact bonds, counted at the end). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/json.h"
#include "../../threads.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "../labshape.h"
#include "peri.h"

static double sdf_m(const double p[3], void *ctx) { return labshape_sdf(ctx, p); }

bool lab_run_fracture(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    const JsonValue *mat = json_get(root, "material"), *run = json_get(root, "run"), *fl = json_get(root, "floor");
    double E = json_get_num(mat, "youngs_modulus_pa", -1), rho = json_get_num(mat, "density_kg_m3", -1), KIc = json_get_num(mat, "fracture_toughness_pa_sqrt_m", -1);
    double h = json_get_num(root, "spacing_m", -1), end = json_get_num(run, "end_s", -1), v[3] = {0, 0, 0}, box[6];
    int frames = (int)json_get_int(run, "frames", 60);
    if (!json_get_str(mat, "source", NULL) || !(E > 0) || !(rho > 0) || !(KIc > 0) || !(h > 0) || !(end > 0) || frames < 1 ||
        !json_get_numbers(json_get(root, "box_m"), box, 6)) {
        snprintf(err, errlen, "fracture: material {youngs_modulus_pa, density_kg_m3, fracture_toughness_pa_sqrt_m, source}, spacing_m, box_m and "
                              "run {end_s, frames} are required");
        return false;
    }
    json_get_numbers(json_get(root, "velocity_m_s"), v, 3);
    LabShapes S;
    if (!labshape_parse(json_get(root, "bodies"), &S, err, errlen)) return false;
    /* bond-based peridynamics has Poisson's ratio 1/4: the bulk modulus from Young's at that ratio; G0 = K_Ic^2 / E */
    PeriSpec sp = {.h = h, .horizon = 3, .density = rho, .bulk_modulus = E / (3 * (1 - 2 * 0.25)), .fracture_energy = KIc * KIc / E,
                   .floor_z = fl ? json_get_num(fl, "z_m", 0) : -1e30, .gravity = json_get_num(root, "gravity_m_s2", 9.80665)};
    Peri *P = peri_create(&sp, sdf_m, &S, box, box + 3, err, errlen);
    if (!P) return false;
    peri_set_velocity(P, v);
    int n = peri_count(P);
    double dt = peri_stable_dt(P);
    JsonValue *meta = json_object();
    bool mok = meta && json_set_string(meta, "domain", "fracture") && json_set_string(meta, "title", json_get_str(root, "title", "fracture")) &&
               json_set_string(meta, "model", "bond-based peridynamics, prototype microelastic brittle material") && json_set(meta, "scenario", json_clone(root));
    JsonValue *fields = mok ? json_set_object(meta, "fields") : NULL, *looks = mok ? json_set_object(meta, "looks") : NULL;
    mok = mok && fields && looks && json_set_string(fields, "damage", "1") && json_set_string(fields, "speed", "m/s") &&
          json_set_number(meta, "particle_spacing_m", h) && json_set_string(looks, "floor", "steel") &&
          json_set_string(meta, "palette", json_get_str(root, "palette", "glass"));
    char *hdr = mok ? json_dump(meta, 0, NULL, NULL) : NULL;
    json_free(meta);
    LabWriter *w = hdr ? lab_create(out, hdr, err, errlen) : NULL;
    free(hdr);
    double *x = malloc(3 * (size_t)n * sizeof *x), *vel = malloc(3 * (size_t)n * sizeof *vel), *dmg = malloc((size_t)n * sizeof *dmg), *spd = malloc((size_t)n * sizeof *spd);
    ThreadPool *pool = pool_create(cpu_perf_count());
    bool ok = w && x && vel && dmg && spd && pool;
    long steps = (long)ceil(end / dt), per = steps / frames > 0 ? steps / frames : 1;
    double fz = sp.floor_z;
    double floorq[12] = {box[0], box[1], fz, box[3], box[1], fz, box[3], box[4], fz, box[0], box[4], fz};
    int fconn[4] = {0, 1, 2, 3};
    clock_t c0 = clock();
    for (long k = 0; ok && k <= steps; k++) {
        if (k % per == 0 || k == steps) {
            peri_state(P, x, vel, dmg);
            for (int i = 0; i < n; i++) spd[i] = sqrt(vel[3 * i] * vel[3 * i] + vel[3 * i + 1] * vel[3 * i + 1] + vel[3 * i + 2] * vel[3 * i + 2]);
            lab_frame_begin(w, k * dt);
            lab_part_points(w, "glass", n, x);
            lab_field_d(w, "damage", LAB_AT_NODE, (size_t)n, dmg), lab_field_d(w, "speed", LAB_AT_NODE, (size_t)n, spd);
            if (fl) lab_part_cells(w, "floor", 4, floorq, 1, LAB_QUAD, fconn);
            if (!lab_frame_end(w)) {
                snprintf(err, errlen, "cannot write a frame (disk full?)");
                ok = false;
            }
            if (!quiet) {
                long br;
                peri_energy(P, NULL, NULL, &br);
                fprintf(stderr, "  t %.4g ms  step %ld/%ld  bonds broken %ld  %.0f s cpu\n", 1e3 * k * dt, k, steps, br, (double)(clock() - c0) / CLOCKS_PER_SEC);
            }
        }
        if (k < steps) peri_step(P, dt, pool);
    }
    double ke, ee;
    long br;
    peri_energy(P, &ke, &ee, &br);
    snprintf(info->diagnostics, sizeof info->diagnostics,
             "{\"points\":%d,\"bonds\":%ld,\"bonds_broken\":%ld,\"time_step_s\":%.6g,\"kinetic_j\":%.6g,\"elastic_j\":%.6g,\"critical_stretch\":%.6g,"
             "\"fracture_energy_j_m2\":%.6g}",
             n, peri_bonds(P), br, dt, ke, ee, 0.0, sp.fracture_energy);
    { /* the critical stretch into the record */
        double c, s0;
        peri_constants(P, &c, &s0);
        char *q = strstr(info->diagnostics, "\"critical_stretch\":0");
        if (q) {
            char tail[256];
            snprintf(tail, sizeof tail, "%s", q + strlen("\"critical_stretch\":0"));
            snprintf(q, sizeof info->diagnostics - (size_t)(q - info->diagnostics), "\"critical_stretch\":%.6g%s", s0, tail);
        }
    }
    info->frames = w ? lab_frames_written(w) : 0;
    info->steps = steps;
    if (w && !lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    free(x), free(vel), free(dmg), free(spd);
    pool_destroy(pool), peri_free(P);
    return ok;
}
