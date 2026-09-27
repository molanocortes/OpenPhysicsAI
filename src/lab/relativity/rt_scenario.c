/* rt_scenario.c - a black-hole picture or film described in JSON (docs/lab/relativity.md has every key): the camera
 * may circle the hole between two azimuths or rise between two inclinations over the frames. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "blackhole.h"

bool lab_run_relativity(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    BhSpec s;
    bh_spec_defaults(&s);
    const JsonValue *c = json_get(root, "camera"), *im = json_get(root, "image"), *d = json_get(root, "disk");
    s.cam_r = json_get_num(c, "distance_M", s.cam_r);
    s.incl_deg = json_get_num(c, "inclination_deg", s.incl_deg);
    s.azim_deg = json_get_num(c, "azimuth_deg", s.azim_deg);
    s.fov_deg = json_get_num(c, "fov_deg", s.fov_deg);
    s.w = (int)json_get_int(im, "width", s.w), s.h = (int)json_get_int(im, "height", s.h);
    s.disk = d != NULL;
    if (d) s.r_in = json_get_num(d, "inner_M", 6), s.r_out = json_get_num(d, "outer_M", 22), s.t_in_k = json_get_num(d, "temperature_k", 9000);
    s.stars = json_get_bool(root, "stars", true);
    s.aa = (int)json_get_int(im, "samples_per_axis", 2);
    s.exposure_ref = json_get_num(im, "exposure_ref", 0);
    if (!(s.cam_r > 3) || s.w < 8 || s.h < 8 || s.w * s.h > 8000000 || s.aa < 1 || s.aa > 4) {
        snprintf(err, errlen, "relativity: camera.distance_M > 3 (in units of the hole's mass) an image of 8 to 8e6 pixels and 1 to 4 samples per axis");
        return false;
    }
    const JsonValue *run = json_get(root, "run");
    int frames = (int)json_get_int(run, "frames", 1);
    double a0 = json_get_num(run, "azimuth_from_deg", s.azim_deg), a1 = json_get_num(run, "azimuth_to_deg", s.azim_deg);
    double i0 = json_get_num(run, "inclination_from_deg", s.incl_deg), i1 = json_get_num(run, "inclination_to_deg", s.incl_deg);
    double t0 = json_get_num(run, "time_from_M", 0), t1 = json_get_num(run, "time_to_M", 0);
    char title[160];
    snprintf(title, sizeof title, "%s", json_get_str(root, "title", "black hole"));
    char *hdr = bh_header_json(&s, title);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) return false;
    float *rgb = malloc((size_t)s.w * s.h * 3 * sizeof(float));
    bool ok = rgb != NULL;
    for (int f = 0; f < frames && ok; f++) {
        double t = frames > 1 ? (double)f / (frames - 1) : 0;
        s.azim_deg = a0 + t * (a1 - a0), s.incl_deg = i0 + t * (i1 - i0), s.time_M = t0 + t * (t1 - t0);
        double ref = bh_render(&s, rgb);
        if (!(s.exposure_ref > 0)) s.exposure_ref = ref; /* the first frame sets the exposure of the whole film */
        lab_frame_begin(w, s.time_M);
        bh_write_frame(&s, rgb, w);
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", f);
            ok = false;
        }
        if (!quiet) fprintf(stderr, "  frame %3d/%d  inclination %.1f azimuth %.1f\n", f + 1, frames, s.incl_deg, s.azim_deg);
    }
    free(rgb);
    snprintf(info->diagnostics, sizeof info->diagnostics, "{\"frames\":%d,\"photon_sphere_M\":3,\"shadow_radius_M\":5.196152}", frames);
    info->frames = lab_frames_written(w);
    info->steps = frames;
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    return ok;
}
