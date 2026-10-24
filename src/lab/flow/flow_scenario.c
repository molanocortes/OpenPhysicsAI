/* flow_scenario.c - a two-dimensional tunnel run described in JSON (docs/lab/flow.md has every key). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "flow.h"

bool lab_run_flow3d(const JsonValue *,const char *,bool,LabRunInfo *,char *,size_t);
bool lab_run_lbm3d(const JsonValue *, const char *, bool, LabRunInfo *, char *, size_t);

bool lab_run_flow(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    if (!strcmp(json_get_str(root, "model", ""), "lbm3d")) return lab_run_lbm3d(root, out, quiet, info, err, errlen);
    if (!strcmp(json_get_str(root,"model",""),"periodic3d"))
        return lab_run_flow3d(root,out,quiet,info,err,errlen);
    static FlowSpec s;
    flow_spec_defaults(&s);
    s.Re = json_get_num(root, "reynolds", -1);
    const char *shape = json_get_str(root, "shape", "cylinder");
    if (!strcmp(shape, "cylinder")) s.shape = FLOW_CYLINDER;
    else if (!strcmp(shape, "naca")) {
        s.shape = FLOW_NACA4;
        const char *code = json_get_str(root, "naca", "0012");
        if (strlen(code) != 4) {
            snprintf(err, errlen, "naca: four digits");
            return false;
        }
        snprintf(s.naca, sizeof s.naca, "%s", code);
        s.aoa_deg = json_get_num(root, "aoa_deg", 0);
    } else {
        snprintf(err, errlen, "shape is cylinder or naca");
        return false;
    }
    s.D = (int)json_get_int(root, "body_cells", 16);
    const JsonValue *t = json_get(root, "tunnel");
    s.width_D = json_get_num(t, "width_d", 16), s.length_D = json_get_num(t, "length_d", 30), s.upstream_D = json_get_num(t, "upstream_d", 8);
    s.channel = json_get_bool(t, "channel", false), s.body_y_D = json_get_num(t, "body_y_d", 0);
    s.u_lb = json_get_num(root, "lattice_velocity", 0.08);
    s.nu_m2_s = json_get_num(root, "fluid_kinematic_viscosity_m2_s", 1.0e-6);
    s.D_m = json_get_num(root, "body_size_m", 0.01);
    const JsonValue *rake = json_get(root, "dye_rake");
    if (rake) {
        int n = (int)json_get_int(rake, "count", 0);
        double x = json_get_num(rake, "x_d", -2), y0 = json_get_num(rake, "y_from_d", -1), y1 = json_get_num(rake, "y_to_d", 1);
        for (int i = 0; i < n && s.ndye < 64; i++, s.ndye++) s.dye_xy[s.ndye][0] = x, s.dye_xy[s.ndye][1] = n > 1 ? y0 + (y1 - y0) * i / (n - 1) : y0;
    }
    s.dye_every = (int)json_get_int(root, "dye_every_steps", 4);
    const JsonValue *run = json_get(root, "run");
    double conv = json_get_num(run, "convective_times", -1);
    int frames = (int)json_get_int(run, "frames", 60);
    double film_from = json_get_num(run, "film_from_convective_time", 0);
    if (!(s.Re > 0) || !(conv > 0)) {
        snprintf(err, errlen, "reynolds > 0 and run.convective_times > 0 are required");
        return false;
    }
    Flow *f = flow_create(&s, err, errlen);
    if (!f) return false;
    char title[160];
    snprintf(title, sizeof title, "%s", json_get_str(root, "title", "flow run"));
    char *hdr = flow_header_json(&s, title);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        flow_free(f);
        return false;
    }
    long total = (long)ceil(conv * s.D / s.u_lb), from = (long)ceil(film_from * s.D / s.u_lb);
    if (from > total) from = total;
    bool ok = flow_advance(f, from);
    for (int k = 0; k <= frames && ok; k++) {
        long target = from + (total - from) * k / frames;
        ok = flow_advance(f, target - flow_steps(f));
        lab_frame_begin(w, flow_time_s(f));
        flow_write_frame(f, w);
        if (!lab_frame_end(w)) ok = false;
        if (!quiet && k % 10 == 0) fprintf(stderr, "  frame %3d/%d  t U/D %.1f  steps %ld\n", k, frames, flow_convective_time(f), flow_steps(f));
    }
    if (flow_unstable(f)) snprintf(err, errlen, "the lattice became unstable at step %ld", flow_steps(f));
    long half = flow_steps(f) / 2;
    double cd, cl, cdm, clr;
    flow_coefficients(f, half, &cd, &cl, &cdm, &clr);
    int periods = 0;
    double st = flow_strouhal(f, half, &periods);
    snprintf(info->diagnostics, sizeof info->diagnostics,
             "{\"convective_time\":%.4g,\"cd_mean\":%.5f,\"cl_rms\":%.5f,\"strouhal\":%.5f,\"periods\":%d,\"wake_length_d\":%.4f}", flow_convective_time(f), cdm,
             clr, st, periods, flow_wake_length(f));
    info->frames = lab_frames_written(w);
    info->steps = flow_steps(f);
    if (!lab_close(w)) ok = false;
    flow_free(f);
    return ok;
}
