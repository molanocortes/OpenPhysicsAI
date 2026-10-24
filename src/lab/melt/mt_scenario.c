/* mt_scenario.c - a laser melting tracks into a metal plate and the metal solidifying behind it (melt.h), described
 * in SI (docs/lab/melt.md): the metal and its sources, the block of cells, the beam and its tracks, the run.
 *
 * Written per frame: the block (cells averaged over output_every^3) with the temperature and "melted" (the largest
 * liquid fraction each cell has reached: the track left behind), and the beam as a line. The run record gives the
 * energy balance and the melt pool's length, width and depth at the end of the first track. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/json.h"
#include "../../threads.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "melt.h"
#include "mt_scenario.h"

bool melt_scenario_spec(const JsonValue *root, MeltSpec *sp, double *end_s, char *err, size_t errlen) {
    const JsonValue *mat = json_get(root, "metal"), *blk = json_get(root, "block"), *beam = json_get(root, "beam"), *run = json_get(root, "run");
    MeltSpec s = {0};
    double size[3], org[3] = {0, 0, 0};
    s.rho = json_get_num(mat, "density_kg_m3", -1), s.c_s = json_get_num(mat, "specific_heat_solid_j_kgk", -1);
    s.c_l = json_get_num(mat, "specific_heat_liquid_j_kgk", -1), s.k_s = json_get_num(mat, "conductivity_solid_w_mk", -1);
    s.k_l = json_get_num(mat, "conductivity_liquid_w_mk", -1), s.T_s = json_get_num(mat, "solidus_k", -1);
    s.T_l = json_get_num(mat, "liquidus_k", -1), s.L = json_get_num(mat, "latent_heat_j_kg", -1);
    s.emissivity = json_get_num(mat, "emissivity", 0);
    const JsonValue *tab = json_get(mat, "solid_table"); /* [{temperature_k, conductivity_w_mk, specific_heat_j_kgk}], rising */
    if (tab) {
        s.nprop = (int)json_len(tab);
        if (s.nprop > 32) s.nprop = -1; /* refused by melt_create, with its reason */
        for (int i = 0; i < s.nprop; i++) {
            const JsonValue *e = json_at(tab, (size_t)i);
            s.prop_T[i] = json_get_num(e, "temperature_k", -1), s.prop_k[i] = json_get_num(e, "conductivity_w_mk", -1);
            s.prop_c[i] = json_get_num(e, "specific_heat_j_kgk", -1);
        }
    }
    s.h = json_get_num(blk, "cell_m", -1), s.T0 = json_get_num(root, "initial_temperature_k", 293.15);
    s.T_amb = json_get_num(root, "ambient_temperature_k", s.T0), s.h_conv = json_get_num(root, "convection_w_m2k", 0);
    s.power = json_get_num(beam, "power_w", -1), s.absorptivity = json_get_num(beam, "absorptivity", -1), s.radius = json_get_num(beam, "radius_m", -1);
    double end = json_get_num(run, "end_s", -1);
    const JsonValue *tracks = json_get(beam, "tracks");
    if (!json_get_str(mat, "source", NULL) || !json_get_numbers(json_get(blk, "size_m"), size, 3) || !(s.h > 0) || !(s.power > 0) ||
        !(s.absorptivity > 0) || !(s.radius > 0) || !tracks || !json_len(tracks) || !(end > 0)) {
        snprintf(err, errlen, "melt: metal {density_kg_m3, specific_heat_solid_j_kgk, specific_heat_liquid_j_kgk, conductivity_solid_w_mk, "
                              "conductivity_liquid_w_mk, solidus_k, liquidus_k, latent_heat_j_kg, source}, block {size_m, cell_m}, beam {power_w, "
                              "absorptivity, radius_m, tracks [{from_m, to_m, speed_m_s}]} and run {end_s, frames} are required");
        return false;
    }
    json_get_numbers(json_get(blk, "origin_m"), org, 3);
    for (int a = 0; a < 3; a++) s.n[a] = (int)lround(size[a] / s.h), s.origin[a] = org[a];
    for (int d = 0; d < 6; d++) s.held_T[d] = -1;
    s.held_T[4] = json_get_num(blk, "bottom_held_k", -1); /* the plate on a thick base plate: its bottom may be held */
    const JsonValue *hf = json_get(blk, "held_faces"); /* faces held at the initial temperature: "x-", "x+", "y-", "y+", "z-" */
    for (size_t i = 0; i < json_len(hf); i++) {
        static const char *const FN[5] = {"x-", "x+", "y-", "y+", "z-"};
        const char *f = json_str(json_at(hf, i));
        int d = -1;
        for (int q = 0; q < 5; q++)
            if (f && !strcmp(f, FN[q])) d = q;
        if (d < 0) {
            snprintf(err, errlen, "melt: block.held_faces takes x-, x+, y-, y+ and z- (the top carries the beam)");
            return false;
        }
        s.held_T[d] = s.T0;
    }
    s.ntracks = (int)json_len(tracks);
    if (s.ntracks > 16) {
        snprintf(err, errlen, "melt: at most 16 tracks");
        return false;
    }
    for (int i = 0; i < s.ntracks; i++) {
        const JsonValue *t = json_at(tracks, i);
        double a[2], b[2];
        if (!json_get_numbers(json_get(t, "from_m"), a, 2) || !json_get_numbers(json_get(t, "to_m"), b, 2) || !(json_get_num(t, "speed_m_s", -1) > 0)) {
            snprintf(err, errlen, "melt: track %d needs from_m [x, y], to_m [x, y] and speed_m_s", i);
            return false;
        }
        s.track[i][0] = a[0], s.track[i][1] = a[1], s.track[i][2] = b[0], s.track[i][3] = b[1];
        s.track[i][4] = json_get_num(t, "speed_m_s", -1), s.track[i][5] = json_get_num(t, "pause_s", 0);
    }
    const JsonValue *fl = json_get(root, "melt_flow"); /* flow in the melt: the liquid's viscosity and dsigma/dT, sourced */
    if (fl) {
        s.flow = 1, s.mu = json_get_num(fl, "viscosity_pa_s", -1), s.dsigma_dT = json_get_num(fl, "dsigma_dT_n_mk", 0);
        s.mushy_C = json_get_num(fl, "mushy_constant_kg_m3s", 0);
        s.slip_y = json_get_bool(blk, "mirror_y", false); /* the y- face a plane of symmetry: free slip there */
        if (!(s.mu > 0) || !json_get_str(fl, "source", NULL)) {
            snprintf(err, errlen, "melt: melt_flow {viscosity_pa_s, dsigma_dT_n_mk, source} (mushy_constant_kg_m3s optional)");
            return false;
        }
    }
    *sp = s;
    if (end_s) *end_s = end;
    return true;
}

bool lab_run_melt(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    const JsonValue *blk = json_get(root, "block"), *run = json_get(root, "run");
    const JsonValue *fl = json_get(root, "melt_flow");
    MeltSpec s;
    double end;
    if (!melt_scenario_spec(root, &s, &end, err, errlen)) return false;
    double size[3];
    json_get_numbers(json_get(blk, "size_m"), size, 3);
    int frames = (int)json_get_int(run, "frames", 60), every = (int)json_get_int(blk, "output_every", 1);
    if (frames < 1 || every < 1) {
        snprintf(err, errlen, "melt: run.frames and block.output_every must be at least 1");
        return false;
    }
    s.n[0] -= s.n[0] % every, s.n[1] -= s.n[1] % every, s.n[2] -= s.n[2] % every;
    Melt *M = melt_create(&s, err, errlen);
    if (!M) return false;
    const int ox = s.n[0] / every, oy = s.n[1] / every, oz = s.n[2] / every;
    const size_t nout = (size_t)ox * oy * oz, n = melt_cells(M);
    JsonValue *meta = json_object();
    bool mok = meta && json_set_string(meta, "domain", "melt") && json_set_string(meta, "title", json_get_str(root, "title", "melt")) &&
               json_set_string(meta, "model", fl ? "3D conduction with melting and solidification (enthalpy method), Gaussian beam, Marangoni flow in the melt"
                                                 : "3D conduction with melting and solidification (enthalpy method), Gaussian beam; no flow in the melt") &&
               json_set_string(meta, "volume_look", "solid") && json_set(meta, "scenario", json_clone(root)) &&
               /* the solver's own blind record against measured tracks, not a statement about this run */
               json_set_string(meta, "measured_accuracy",
                               "blind against NIST AM-Bench single tracks in IN625 and IN718 (GOALS.md G20 step 2), conduction, the "
                               "absorptivity fixed on one calibration track: the conduction-mode tracks within a median 11 % in width and "
                               "depth; keyhole tracks' width within 12 % but their depth 42 to 76 % short, where NIST measured about twice "
                               "the absorbed power the model uses");
    JsonValue *fields = mok ? json_set_object(meta, "fields") : NULL;
    mok = mok && fields && json_set_string(fields, "T", "K") && json_set_string(fields, "melted", "1");
    if (mok && fl) mok = json_set_string(fields, "speed", "m/s");
    char *hdr = mok ? json_dump(meta, 0, NULL, NULL) : NULL;
    json_free(meta);
    LabWriter *w = hdr ? lab_create(out, hdr, err, errlen) : NULL;
    free(hdr);
    float *T = malloc(n * sizeof(float)), *fm = malloc(n * sizeof(float));
    double *oT = malloc(nout * sizeof(double)), *oM = malloc(nout * sizeof(double)), *oS = fl ? malloc(nout * sizeof(double)) : NULL;
    ThreadPool *pool = pool_create(cpu_perf_count());
    bool ok = w && T && fm && oT && oM && pool && (!fl || oS);
    double dt = melt_stable_dt(M);
    long steps = (long)ceil(end / dt);
    dt = end / steps;
    LabBlock B = {{ox, oy, oz}, 0, 0, {s.origin[0], s.origin[1], s.origin[2]}, {every * s.h, every * s.h, every * s.h}};
    double pool_len = 0, pool_wid = 0, pool_dep = 0, first_end = 0;
    { /* when the first track ends (its pool is measured just before) */
        const double *t0 = s.track[0];
        first_end = t0[5] + hypot(t0[2] - t0[0], t0[3] - t0[1]) / t0[4];
    }
    bool measured = false;
    time_t wall0 = time(NULL);
    long st = 0;
    for (int fr = 0; ok && fr <= frames; fr++) {
        const double tf = end * fr / frames;
        while (melt_time(M) < tf - 1e-15) {
            if (!measured && melt_time(M) >= 0.98 * first_end) melt_pool_size(M, &pool_len, &pool_wid, &pool_dep), measured = true;
            double d = fl ? melt_stable_dt(M) : dt;
            if (melt_time(M) + d > tf) d = tf - melt_time(M);
            melt_step(M, d, pool), st++;
        }
        {
            melt_fields(M, T, NULL, fm);
            const double inv = 1.0 / ((double)every * every * every);
            for (int k = 0; k < oz; k++)
                for (int j = 0; j < oy; j++)
                    for (int i = 0; i < ox; i++) {
                        double a = 0, b = 0, sp = 0;
                        for (int kk = 0; kk < every; kk++)
                            for (int jj = 0; jj < every; jj++)
                                for (int ii = 0; ii < every; ii++) {
                                    size_t c = ((size_t)(k * every + kk) * s.n[1] + (j * every + jj)) * s.n[0] + (i * every + ii);
                                    a += T[c], b = fmax(b, fm[c]);
                                    if (oS) {
                                        double v[3];
                                        melt_velocity(M, i * every + ii, j * every + jj, k * every + kk, v);
                                        sp = fmax(sp, sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
                                    }
                                }
                        size_t o = ((size_t)k * oy + j) * ox + i;
                        oT[o] = a * inv, oM[o] = b;
                        if (oS) oS[o] = sp;
                    }
            lab_frame_begin(w, melt_time(M));
            lab_part_blocks(w, "plate", 1, &B);
            lab_field_d(w, "T", LAB_AT_CELL, nout, oT), lab_field_d(w, "melted", LAB_AT_CELL, nout, oM);
            if (oS) lab_field_d(w, "speed", LAB_AT_CELL, nout, oS);
            double bx, by;
            if (melt_beam_at(M, melt_time(M), &bx, &by)) {
                double top = s.origin[2] + s.n[2] * s.h, line[6] = {bx, by, top, bx, by, top + 0.5 * size[2]};
                int conn[2] = {0, 1};
                lab_part_cells(w, "beam", 2, line, 1, LAB_LINE, conn);
            }
            if (!lab_frame_end(w)) {
                snprintf(err, errlen, "cannot write a frame (disk full?)");
                ok = false;
            }
            if (!quiet) {
                double ab, lo, ho, L, W, D;
                melt_energy(M, &ab, &lo, &ho);
                melt_pool_size(M, &L, &W, &D);
                fprintf(stderr, "  t %.4g ms  step %ld  absorbed %.4g J  pool %.0f x %.0f x %.0f um  fastest %.3g m/s  %.0f s\n", 1e3 * melt_time(M), st, ab, 1e6 * L,
                        1e6 * W, 1e6 * D, melt_max_speed(M), difftime(time(NULL), wall0));
            }
        }
    }
    steps = st;
    double ab, lo, ho;
    melt_energy(M, &ab, &lo, &ho);
    snprintf(info->diagnostics, sizeof info->diagnostics,
             "{\"cells\":%zu,\"time_step_s\":%.6g,\"absorbed_j\":%.6g,\"surface_loss_j\":%.6g,\"held_faces_out_j\":%.6g,\"pool_length_m\":%.6g,"
             "\"pool_width_m\":%.6g,\"pool_depth_m\":%.6g}",
             n, dt, ab, lo, ho, pool_len, pool_wid, pool_dep);
    info->frames = w ? lab_frames_written(w) : 0;
    info->steps = steps;
    if (w && !lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    free(T), free(fm), free(oT), free(oM), free(oS);
    pool_destroy(pool), melt_free(M);
    return ok;
}
