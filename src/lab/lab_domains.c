/* lab_domains.c - see lab_domains.h */
#include "lab_domains.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gas/gas.h"
#include "gas/gas_scenario.h"
#include "labio.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

const char *lab_domain_list(void) { return "gas, acoustic, impact, orbit, em, flow, heat, magnet, relativity, water, fracture, sheet, melt, fire, battery"; }

bool lab_run_gas3d(const JsonValue *,const char *,bool,LabRunInfo *,char *,size_t);
bool lab_run_compressible3d(const JsonValue *, const char *, bool, LabRunInfo *, char *, size_t);

static bool run_gas(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    if (!strcmp(json_get_str(root, "model", ""), "compressible3d")) return lab_run_compressible3d(root, out, quiet, info, err, errlen);
    if (!strcmp(json_get_str(root,"model",""),"euler3d"))
        return lab_run_gas3d(root,out,quiet,info,err,errlen);
    GasScenario sc;
    if (!gas_scenario_parse(root, &sc, err, errlen)) {
        gas_scenario_free(&sc);
        return false;
    }
    Gas *g = gas_create(&sc.spec, err, errlen);
    if (!g) {
        gas_scenario_free(&sc);
        return false;
    }
    char *hdr = gas_header_json(&sc.spec, sc.title);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        gas_free(g);
        gas_scenario_free(&sc);
        return false;
    }
    bool ok = true;
    double t0 = now();
    for (int f = 0; f <= sc.frames && ok; f++) {
        double target = sc.end_time * f / sc.frames;
        while (gas_time(g) < target * (1 - 1e-12)) {
            if (gas_step(g, target) <= 0) {
                snprintf(err, errlen, "gas: the step failed at t = %g s", gas_time(g));
                ok = false;
                break;
            }
        }
        if (!ok) break;
        lab_frame_begin(w, gas_time(g));
        gas_write_frame(g, w, sc.fields);
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", f);
            ok = false;
        }
        if (!quiet) {
            GasStats st;
            gas_stats(g, &st);
            fprintf(stderr, "  frame %3d/%d  t %.4g s  steps %d  leaves %d  cells %ld  levels %d  %.1f s\n", f, sc.frames, gas_time(g), gas_steps(g),
                    st.leaves, st.cells, st.levels_used, now() - t0);
        }
    }
    GasStats st;
    gas_stats(g, &st);
    info->frames = lab_frames_written(w);
    info->steps = gas_steps(g);
    snprintf(info->diagnostics, sizeof info->diagnostics,
             "{\"end_time_s\":%.9g,\"leaves\":%d,\"cells\":%ld,\"levels\":%d,\"min_density_kg_m3\":%.6g,\"min_pressure_pa\":%.6g}", gas_time(g),
             st.leaves, st.cells, st.levels_used, st.min_rho, st.min_p);
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    gas_free(g);
    gas_scenario_free(&sc);
    return ok;
}

bool lab_run_domain(const char *domain, const JsonValue *scenario, const char *out_path, bool quiet, LabRunInfo *info, char *err,
                    size_t errlen) {
    memset(info, 0, sizeof *info);
    if (!strcmp(domain, "gas")) return run_gas(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "acoustic")) return lab_run_acoustic(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "impact")) return lab_run_impact(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "orbit")) return lab_run_orbit(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "em")) return lab_run_em(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "flow")) return lab_run_flow(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "heat")) return lab_run_heat(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "magnet")) return lab_run_magnet(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "fracture")) return lab_run_fracture(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "sheet")) return lab_run_sheet(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "melt")) return lab_run_melt(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "fire")) return lab_run_fire(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "battery")) return lab_run_battery(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "relativity")) return lab_run_relativity(scenario, out_path, quiet, info, err, errlen);
    if (!strcmp(domain, "water")) return lab_run_water(scenario, out_path, quiet, info, err, errlen);
    snprintf(err, errlen, "unknown domain \"%s\"; this build knows: %s", domain, lab_domain_list());
    return false;
}
