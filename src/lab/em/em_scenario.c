/* em_scenario.c - an electromagnetic run described in JSON (docs/lab/em.md has every key), and its run loop. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "../labshape.h"
#include "em.h"

bool lab_run_em(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    static EmSpec s;
    em_spec_defaults(&s);
    double cells[3];
    s.dx = json_get_num(root, "dx_m", -1);
    if (!json_get_numbers(json_get(root, "cells"), cells, 3) || !(s.dx > 0)) {
        snprintf(err, errlen, "cells [nx, ny, nz] and dx_m are required");
        return false;
    }
    for (int k = 0; k < 3; k++) s.n[k] = (int)cells[k];
    s.pml = (int)json_get_int(root, "pml_cells", 10);
    const JsonValue *pw = json_get(root, "plane_wave");
    if (pw) {
        double lo[3], hi[3];
        if (!json_get_numbers(json_get(pw, "tfsf_lo_cells"), lo, 3) || !json_get_numbers(json_get(pw, "tfsf_hi_cells"), hi, 3)) {
            snprintf(err, errlen, "plane_wave: tfsf_lo_cells and tfsf_hi_cells");
            return false;
        }
        s.plane_wave = true;
        for (int k = 0; k < 3; k++) s.tfsf_lo[k] = (int)lo[k], s.tfsf_hi[k] = (int)hi[k];
        s.amplitude = json_get_num(pw, "amplitude_v_m", 1);
        s.f0 = json_get_num(pw, "frequency_hz", 0);
        s.bandwidth = json_get_num(pw, "bandwidth_hz", 0);
    }
    const JsonValue *objs = json_get(root, "objects");
    for (size_t i = 0; objs && i < json_len(objs) && s.nobj < EM_MAX_OBJECTS; i++) {
        const JsonValue *o = json_at(objs, i);
        EmObject *O = &s.obj[s.nobj];
        const char *shape = json_get_str(o, "shape", "");
        double v[6];
        if (!strcmp(shape, "sphere") && json_get_numbers(json_get(o, "center_m"), O->c, 3)) O->shape = EM_SPHERE, O->r = json_get_num(o, "radius_m", 0);
        else if (!strcmp(shape, "box") && json_get_numbers(json_get(o, "box_m"), v, 6)) {
            O->shape = EM_BOX;
            for (int k = 0; k < 3; k++) O->lo[k] = v[k], O->hi[k] = v[3 + k];
        } else if (!strcmp(shape, "cylinder_z") && json_get_numbers(json_get(o, "center_m"), O->c, 3)) {
            O->shape = EM_CYLINDER_Z, O->r = json_get_num(o, "radius_m", 0);
            O->lo[2] = json_get_num(o, "z0_m", 0), O->hi[2] = json_get_num(o, "z1_m", 0);
        } else {
            snprintf(err, errlen, "objects[%zu]: sphere {center_m, radius_m}, box {box_m}, cylinder_z {center_m, radius_m, z0_m, z1_m}", i);
            return false;
        }
        O->pec = json_get_bool(o, "conductor", false);
        O->eps_r = json_get_num(o, "relative_permittivity", 1);
        O->sigma = json_get_num(o, "conductivity_s_m", 0);
        s.nobj++;
    }
    /* conducting bodies from the shape library (an aircraft, a ship): "bodies": [...] */
    static LabShapes shapes;
    if (!labshape_parse(json_get(root, "bodies"), &shapes, err, errlen)) return false;
    if (shapes.n && s.nobj < EM_MAX_OBJECTS) {
        EmObject *O = &s.obj[s.nobj++];
        memset(O, 0, sizeof *O);
        O->shape = EM_SHAPES, O->shapes = &shapes, O->pec = true, O->eps_r = 1;
    }
    /* a radar: a pulsed dipole antenna that also listens; the echo's delay gives the range */
    const JsonValue *radar = json_get(root, "radar");
    double ant[3] = {0, 0, 0};
    if (radar) {
        if (!json_get_numbers(json_get(radar, "antenna_m"), ant, 3) || s.ndip >= 8) {
            snprintf(err, errlen, "radar: antenna_m [x, y, z]");
            return false;
        }
        memcpy(s.dip_pos[s.ndip++], ant, sizeof ant);
        s.amplitude = json_get_num(radar, "amplitude_v_m", 1);
        s.f0 = json_get_num(radar, "frequency_hz", 0);
        s.bandwidth = json_get_num(radar, "bandwidth_hz", 0);
    }
    const JsonValue *run = json_get(root, "run");
    double end = json_get_num(run, "end_time_s", -1), zs = json_get_num(run, "slice_z_m", 0.5 * s.n[2] * s.dx);
    int frames = (int)json_get_int(run, "frames", 60);
    if (!(end > 0)) {
        snprintf(err, errlen, "run: end_time_s > 0");
        return false;
    }
    Em *e = em_create(&s, err, errlen);
    if (!e) return false;
    /* the scattered field: the same source without the bodies, stepped alongside; their difference is the echo alone */
    Em *free_run = NULL;
    if (radar && shapes.n && json_get_bool(run, "scattered", true)) {
        static EmSpec s0;
        s0 = s;
        s0.nobj = 0;
        free_run = em_create(&s0, err, errlen);
        if (!free_run) {
            em_free(e);
            return false;
        }
    }
    char title[160];
    snprintf(title, sizeof title, "%s", json_get_str(root, "title", "em run"));
    char *hdr = em_header_json(&s, title);
    if (hdr && shapes.n) { /* the bodies drawn as aluminium */
        size_t L = strlen(hdr);
        char *h2 = realloc(hdr, L + 64);
        if (h2) hdr = h2, snprintf(hdr + L - 1, 64, ",\"looks\":{\"aircraft\":\"aluminium\"}}");
    }
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        em_free(e);
        return false;
    }
    bool ok = true;
    int ntri = 0;
    double lo[3] = {0, 0, 0}, hi[3] = {s.n[0] * s.dx, s.n[1] * s.dx, s.n[2] * s.dx};
    double *soup = shapes.n ? labshape_mesh(&shapes, lo, hi, 0.5 * s.dx, &ntri) : NULL, *tri = NULL;
    int *conn = NULL, nnode = ntri ? labshape_weld(soup, ntri, &tri, &conn) : 0;
    free(soup);
    if (nnode < 0) ntri = 0;
    /* the antenna's own record: |Ez| after the transmitted pulse has left, and the strongest echo */
    size_t tcap = 0, tn = 0;
    double *trace = NULL;
    for (int f = 0; f <= frames && ok; f++) {
        while (em_time(e) < end * f / frames) {
            em_step(e);
            if (free_run) em_step(free_run);
            if (radar) {
                if (tn == tcap) {
                    tcap = tcap ? 2 * tcap : 4096;
                    double *t2 = realloc(trace, tcap * sizeof *t2);
                    if (!t2) { ok = false; break; }
                    trace = t2;
                }
                double pr[3] = {ant[0], ant[1], ant[2] + 2 * s.dx}; /* two cells above the feed, off the source edge */
                trace[tn++] = em_probe(e, 2, pr) - (free_run ? em_probe(free_run, 2, pr) : 0); /* the echo alone */
            }
        }
        lab_frame_begin(w, em_time(e));
        int vstride = (int)json_get_int(run, "volume_stride", 1);
        if (json_get_bool(run, "volume", false) && free_run) {
            int d[3];
            em_volume_ez(e, vstride, NULL, d);
            size_t nv = (size_t)d[0] * d[1] * d[2];
            float *a = malloc(nv * sizeof *a), *b = malloc(nv * sizeof *b);
            if (!a || !b) ok = false;
            else {
                em_volume_ez(e, vstride, a, d), em_volume_ez(free_run, vstride, b, d);
                for (size_t q = 0; q < nv; q++) b[q] = a[q] - b[q];
                double hh = s.dx * vstride, o = 0.5 * (vstride - 1) * s.dx;
                LabBlock blk = {{d[0], d[1], d[2]}, 0, LAB_PLANE_XY, {o, o, o}, {hh, hh, hh}};
                lab_part_blocks(w, "volume", 1, &blk);
                lab_field(w, "ez_scattered", LAB_AT_CELL, nv, b);
                lab_field(w, "ez", LAB_AT_CELL, nv, a);
            }
            free(a), free(b);
        } else if (json_get_bool(run, "volume", false)) ok = vstride > 1 || shapes.n ? em_write_volume_ez(e, w, vstride) : em_write_volume(e, w);
        else em_write_frame(e, w, zs);
        if (radar) lab_part_points(w, "antenna", 1, ant);
        if (ntri && conn) lab_part_cells(w, "aircraft", nnode, tri, ntri, LAB_TRI, conn);
        if (!lab_frame_end(w)) ok = false;
        if (!quiet && f % 10 == 0) fprintf(stderr, "  frame %3d/%d  t %.4g ns  steps %ld\n", f, frames, 1e9 * em_time(e), em_steps(e));
    }
    int nd = snprintf(info->diagnostics, sizeof info->diagnostics, "{\"end_time_s\":%.9g,\"dt_s\":%.6g", em_time(e), em_dt(e));
    if (radar && tn > 10) {
        /* the pulse's centre leaves the antenna at t0 = 4 tau (em.c); listen from when it has gone (t0 + 4 tau) */
        double bw = s.bandwidth > 0 ? s.bandwidth : s.f0 > 0 ? 0.5 * s.f0 : 1e9, tau = 1 / (M_PI * bw), t0 = 4 * tau, dt = em_dt(e);
        /* a range gate, as a radar has: listen from the earliest time an echo can return, the round trip to the
         * nearest point of any body, less the pulse's half-width (else the feed's own ringing is taken for the echo) */
        const double c = 299792458.0;
        double dmin = shapes.n ? labshape_sdf(&shapes, ant) : 0, gate = t0 + 2 * dmin / c - 3 * tau;
        double best = 0, tbest = 0, direct = 0;
        for (size_t k = 0; k < tn; k++) {
            double t = (k + 1) * dt, a = fabs(trace[k]);
            if (t < t0 + 4 * tau) direct = fmax(direct, a);
            else if (t >= gate && a > best) best = a, tbest = t;
        }
        nd += snprintf(info->diagnostics + nd, sizeof info->diagnostics - (size_t)nd, ",\"nearest_body_m\":%.5g", dmin);
        nd += snprintf(info->diagnostics + nd, sizeof info->diagnostics - (size_t)nd,
                       ",\"radar_echo_delay_s\":%.6g,\"radar_range_m\":%.5g,\"radar_echo_to_direct\":%.4g", tbest - t0, 0.5 * c * (tbest - t0),
                       direct > 0 ? best / direct : 0);
    }
    snprintf(info->diagnostics + nd, sizeof info->diagnostics - (size_t)nd, "}");
    free(trace), free(tri), free(conn);
    info->frames = lab_frames_written(w);
    info->steps = em_steps(e);
    if (!lab_close(w)) ok = false;
    em_free(e), em_free(free_run);
    return ok;
}
