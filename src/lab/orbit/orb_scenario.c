/* orb_scenario.c - an orbit run described in JSON (docs/lab/orbit.md has every key), and its run loop. Positions in
 * km, velocities in km/s, GM in km^3/s^2, as JPL gives them (tools/horizons.py writes such scenarios). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "orbit.h"

bool lab_run_orbit(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    static OrbSpec s;
    orb_spec_defaults(&s);
    const JsonValue *bodies = json_get(root, "bodies");
    if (!bodies || json_len(bodies) < 1 || json_len(bodies) > ORB_MAX_BODIES) {
        snprintf(err, errlen, "bodies: 1 to %d", ORB_MAX_BODIES);
        return false;
    }
    for (size_t i = 0; i < json_len(bodies); i++) {
        const JsonValue *b = json_at(bodies, i);
        OrbBody *B = &s.bodies[s.n++];
        snprintf(B->name, sizeof B->name, "%s", json_get_str(b, "name", "body"));
        B->gm = json_get_num(b, "gm_km3_s2", -1) * 1e9;
        B->radius = json_get_num(b, "radius_km", 0) * 1e3;
        double x[3], v[3];
        if (!(B->gm >= 0) || !json_get_numbers(json_get(b, "position_km"), x, 3) || !json_get_numbers(json_get(b, "velocity_km_s"), v, 3)) {
            snprintf(err, errlen, "bodies[%zu]: gm_km3_s2, position_km [x, y, z] and velocity_km_s are required", i);
            return false;
        }
        for (int k = 0; k < 3; k++) B->x[k] = 1e3 * x[k], B->v[k] = 1e3 * v[k];
    }
    s.stages = (int)json_get_int(root, "stages", 6);
    s.relativity = json_get_bool(root, "relativity", true);
    s.adaptive = json_get_bool(root, "adaptive", false);
    s.tolerance = json_get_num(root, "tolerance", 1e-13);
    s.dt = json_get_num(root, "step_days", 0.5) * 86400;
    const JsonValue *run = json_get(root, "run");
    double end = json_get_num(run, "end_days", -1) * 86400;
    int frames = (int)json_get_int(run, "frames", 60);
    bool paths = json_get_bool(run, "paths", true);
    if (!(end > 0) || frames < 1) {
        snprintf(err, errlen, "run: end_days > 0 and frames >= 1 are required");
        return false;
    }
    Orbit *o = orb_create(&s, err, errlen);
    if (!o) return false;
    /* an optional pair whose closest approach is reported */
    int ta = -1, tb = -1;
    const JsonValue *tr = json_get(root, "track");
    if (tr) {
        for (int i = 0; i < s.n; i++) {
            if (!strcmp(s.bodies[i].name, json_get_str(tr, "a", ""))) ta = i;
            if (!strcmp(s.bodies[i].name, json_get_str(tr, "b", ""))) tb = i;
        }
        if (ta < 0 || tb < 0) {
            snprintf(err, errlen, "track: a and b must name two bodies");
            orb_free(o);
            return false;
        }
        orb_track(o, ta, tb);
    }
    const char *fo = json_get_str(run, "frame_origin", NULL);
    for (int i = 0; fo && i < s.n; i++)
        if (!strcmp(s.bodies[i].name, fo)) orb_set_frame_origin(o, i);
    char title[160];
    snprintf(title, sizeof title, "%s", json_get_str(root, "title", "orbit run"));
    char *hdr = orb_header_json(&s, title);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        orb_free(o);
        return false;
    }
    double E0 = orb_energy(o);
    bool ok = true;
    for (int f = 0; f <= frames && ok; f++) {
        orb_advance(o, end * f / frames);
        lab_frame_begin(w, orb_time(o));
        orb_write_frame(o, w, paths);
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d", f);
            ok = false;
        }
        if (!quiet && f % 10 == 0) fprintf(stderr, "  frame %3d/%d  t %.2f days  steps %ld\n", f, frames, orb_time(o) / 86400, orb_steps(o));
    }
    int n = snprintf(info->diagnostics, sizeof info->diagnostics, "{\"end_days\":%.9g,\"energy_drift\":%.3e", orb_time(o) / 86400, (orb_energy(o) - E0) / fabs(E0));
    if (ta >= 0) {
        double when, d = orb_closest(o, ta, tb, &when);
        n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, ",\"closest_km\":%.6f,\"closest_at_days\":%.9f", d / 1e3, when / 86400);
    }
    n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, ",\"final\":[");
    for (int i = 0; i < s.n && n < (int)sizeof info->diagnostics - 200; i++) {
        const OrbBody *b = orb_body(o, i);
        n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "%s{\"name\":\"%s\",\"position_km\":[%.6f,%.6f,%.6f]}", i ? "," : "",
                      b->name, b->x[0] / 1e3, b->x[1] / 1e3, b->x[2] / 1e3);
    }
    snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "]}");
    info->frames = lab_frames_written(w);
    info->steps = orb_steps(o);
    if (!lab_close(w) && ok) ok = false;
    orb_free(o);
    return ok;
}
