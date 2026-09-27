/* heat_scenario.c - a heat-and-flow run described in JSON (docs/lab/heat.md has every key), and its run loop.
 *
 * Every material and the fluid must say where their properties come from ("source"): a property without a source is
 * refused, the same rule as the impact library's. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "heat.h"

bool lab_run_heat3d(const JsonValue *,const char *,bool,LabRunInfo *,char *,size_t);

static int side_of(const char *s) {
    return !strcmp(s, "x-") ? 0 : !strcmp(s, "x+") ? 1 : !strcmp(s, "y-") ? 2 : !strcmp(s, "y+") ? 3 : -1;
}

bool lab_run_heat(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    if (!strcmp(json_get_str(root,"model",""),"conduction3d"))
        return lab_run_heat3d(root,out,quiet,info,err,errlen);
    static HtSpec s;
    ht_spec_defaults(&s);
    const JsonValue *g = json_get(root, "grid");
    s.nx = (int)json_get_int(g, "nx", 0), s.ny = (int)json_get_int(g, "ny", 0), s.dx = json_get_num(g, "cell_m", -1);
    const JsonValue *fl = json_get(root, "fluid");
    if (!fl || !json_get_str(fl, "source", NULL)) {
        snprintf(err, errlen, "fluid: kinematic_viscosity_m2_s, thermal_diffusivity_m2_s, volumetric_heat_capacity_j_m3k, expansion_1_k and a source");
        return false;
    }
    s.nu = json_get_num(fl, "kinematic_viscosity_m2_s", -1), s.alpha = json_get_num(fl, "thermal_diffusivity_m2_s", -1);
    s.rho_cp = json_get_num(fl, "volumetric_heat_capacity_j_m3k", -1), s.beta = json_get_num(fl, "expansion_1_k", 0);
    s.rho = json_get_num(fl, "density_kg_m3", 0);
    json_get_numbers(json_get(root, "gravity_m_s2"), s.g, 2);
    s.T_ref = json_get_num(root, "reference_temperature_k", 293.15);
    s.T_init = json_get_num(root, "initial_temperature_k", s.T_ref);
    s.u_ref = json_get_num(root, "largest_speed_m_s", -1);
    s.u_lb = json_get_num(root, "lattice_speed", 0.05);
    s.periodic_x = json_get_bool(root, "periodic_x", false);
    const JsonValue *tu = json_get(root, "turbulence");
    if (tu) s.smagorinsky = json_get_num(tu, "smagorinsky", 0), s.prandtl_t = json_get_num(tu, "turbulent_prandtl", 0.85);
    const JsonValue *sc = json_get(root, "scalar");
    if (sc) s.c_diffusivity = json_get_num(sc, "diffusivity_m2_s", -1), s.c_init = json_get_num(sc, "initial", 0);
    if (!(s.nu > 0) || !(s.alpha > 0) || !(s.rho_cp > 0) || !(s.u_ref > 0)) {
        snprintf(err, errlen, "the fluid's viscosity, diffusivity and heat capacity, and largest_speed_m_s, must be positive");
        return false;
    }
    const JsonValue *mats = json_get(root, "materials");
    for (size_t m = 0; mats && m < json_len(mats); m++) {
        const JsonValue *M = json_at(mats, m);
        if (s.nmaterials >= HT_MAX_MATERIALS) break;
        if (!json_get_str(M, "source", NULL)) {
            snprintf(err, errlen, "materials[%zu]: every material needs a source for its properties", m);
            return false;
        }
        HtMaterial *H = &s.materials[s.nmaterials++];
        snprintf(H->name, sizeof H->name, "%s", json_get_str(M, "name", "material"));
        H->kind = !strcmp(json_get_str(M, "kind", "solid"), "porous") ? HT_POROUS : HT_SOLID;
        H->k = json_get_num(M, "conductivity_w_mk", -1), H->rho_cp = json_get_num(M, "volumetric_heat_capacity_j_m3k", -1);
        H->permeability = json_get_num(M, "permeability_m2", -1);
        if (!(H->k > 0) || !(H->rho_cp > 0) || (H->kind == HT_POROUS && !(H->permeability > 0))) {
            snprintf(err, errlen, "materials[%zu]: conductivity_w_mk and volumetric_heat_capacity_j_m3k > 0 (and permeability_m2 if porous)", m);
            return false;
        }
    }
    const JsonValue *regs = json_get(root, "regions");
    for (size_t r = 0; regs && r < json_len(regs) && s.nregions < HT_MAX_REGIONS; r++) {
        const JsonValue *R = json_at(regs, r);
        HtRegion *H = &s.regions[s.nregions++];
        double b[4];
        H->material = -1;
        if (json_get_numbers(json_get(R, "box_m"), b, 4)) H->lo[0] = b[0], H->lo[1] = b[1], H->hi[0] = b[2], H->hi[1] = b[3];
        else if (json_get_numbers(json_get(R, "circle_m"), b, 3)) H->circle = true, H->lo[0] = b[0], H->lo[1] = b[1], H->hi[0] = b[2];
        else {
            snprintf(err, errlen, "regions[%zu]: box_m [x0, y0, x1, y1] or circle_m [x, y, r]", r);
            return false;
        }
        const char *mn = json_get_str(R, "material", NULL);
        if (mn) {
            if (!strcmp(mn, "fluid")) H->material = 0;
            for (int m = 1; m < s.nmaterials; m++)
                if (!strcmp(s.materials[m].name, mn)) H->material = m;
            if (H->material < 0) {
                snprintf(err, errlen, "regions[%zu]: unknown material \"%s\"", r, mn);
                return false;
            }
        }
        H->q = json_get_num(R, "heat_w_m3", 0);
        json_get_numbers(json_get(R, "fan_m_s2"), H->force, 2);
        H->c_source = json_get_num(R, "scalar_source_per_s", 0);
    }
    const JsonValue *rs = json_get(root, "rotors");
    for (size_t k = 0; rs && k < json_len(rs) && s.nrotors < HT_MAX_ROTORS; k++) {
        const JsonValue *R = json_at(rs, k);
        HtRotor *H = &s.rotors[s.nrotors++];
        json_get_numbers(json_get(R, "centre_m"), H->c, 2);
        H->omega = json_get_num(R, "rpm", 0) * 2 * M_PI / 60;
        H->hub_r = json_get_num(R, "hub_radius_m", 0), H->blades = (int)json_get_int(R, "blades", 0);
        H->r_in = json_get_num(R, "blade_inner_radius_m", 0), H->r_out = json_get_num(R, "blade_outer_radius_m", 0);
        H->thickness = json_get_num(R, "blade_thickness_m", 0), H->angle_deg = json_get_num(R, "blade_angle_deg", 90);
        const char *mn = json_get_str(R, "material", "");
        H->material = -1;
        for (int m = 1; m < s.nmaterials; m++)
            if (!strcmp(s.materials[m].name, mn)) H->material = m;
        if (H->material < 0 || !(s.rho > 0)) {
            snprintf(err, errlen, "rotors[%zu]: a solid material from the list, and the fluid's density_kg_m3 (for the torque)", k);
            return false;
        }
    }
    const JsonValue *bs = json_get(root, "boundaries");
    for (size_t p = 0; bs && p < json_len(bs) && s.npatches < HT_MAX_PATCHES; p++) {
        const JsonValue *B = json_at(bs, p);
        HtPatch *P = &s.patches[s.npatches++];
        P->side = side_of(json_get_str(B, "side", ""));
        if (P->side < 0) {
            snprintf(err, errlen, "boundaries[%zu]: side x-, x+, y- or y+", p);
            return false;
        }
        P->from = json_get_num(B, "from_m", 0), P->to = json_get_num(B, "to_m", 1e9);
        const char *t = json_get_str(B, "type", "wall");
        P->flow = !strcmp(t, "inlet") ? HT_INLET : !strcmp(t, "outlet") ? HT_OUTLET : HT_WALL;
        P->velocity = json_get_num(B, "speed_m_s", 0), P->parabolic = json_get_bool(B, "parabolic", false);
        P->T = json_get_num(B, "temperature_k", s.T_ref), P->flux = json_get_num(B, "heat_flux_w_m2", 0), P->c = json_get_num(B, "scalar", 0);
        const char *th = json_get_str(B, "thermal", json_get(B, "temperature_k") ? "fixed" : json_get(B, "heat_flux_w_m2") ? "flux" : "adiabatic");
        P->thermal = !strcmp(th, "fixed") ? HT_FIXED_T : !strcmp(th, "flux") ? HT_FLUX : HT_ADIABATIC;
    }
    const JsonValue *run = json_get(root, "run");
    double end = json_get_num(run, "end_time_s", -1), first = json_get_num(run, "first_frame_s", 0);
    int frames = (int)json_get_int(run, "frames", 40);
    if (!(end > 0) || frames < 1) {
        snprintf(err, errlen, "run: end_time_s > 0 and frames >= 1 are required");
        return false;
    }
    Heat *h = ht_create(&s, err, errlen);
    if (!h) return false;
    char title[160];
    snprintf(title, sizeof title, "%s", json_get_str(root, "title", "heat run"));
    char *hdr = ht_header_json(&s, title);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        ht_free(h);
        return false;
    }
    bool ok = ht_advance(h, (long)(first / ht_dt(h)));
    for (int f = 0; f <= frames && ok; f++) {
        double target = first + (end - first) * f / frames;
        ok = ht_advance(h, (long)ceil((target - ht_time(h)) / ht_dt(h)));
        lab_frame_begin(w, ht_time(h));
        ht_write_frame(h, w);
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", f);
            ok = false;
        }
        if (!quiet && f % 10 == 0) fprintf(stderr, "  frame %3d/%d  t %.4g s  steps %ld\n", f, frames, ht_time(h), ht_steps(h));
    }
    if (ht_unstable(h)) snprintf(err, errlen, "the flow became unstable at step %ld (a finer grid or a smaller lattice_speed)", ht_steps(h));
    int n = snprintf(info->diagnostics, sizeof info->diagnostics, "{\"time_s\":%.6g,\"boundaries\":[", ht_time(h));
    for (int p = 0; p < s.npatches && n < (int)sizeof info->diagnostics - 200; p++)
        n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "%s{\"heat_w_per_m\":%.6g,\"flow_m2_s\":%.6g,\"mean_T_k\":%.6g}", p ? "," : "",
                      ht_patch_heat(h, p), ht_patch_flow(h, p), ht_patch_mean_T(h, p));
    n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "],\"rotors\":[");
    for (int k = 0; k < s.nrotors; k++)
        n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "%s{\"torque_n_m_per_m\":%.6g,\"power_w_per_m\":%.6g}", k ? "," : "",
                      ht_rotor_torque(h, k), -ht_rotor_torque(h, k) * s.rotors[k].omega);
    n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "],\"materials_max_T_k\":{");
    for (int m = 1; m < s.nmaterials && n < (int)sizeof info->diagnostics - 100; m++)
        n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "%s\"%s\":%.6g", m > 1 ? "," : "", s.materials[m].name, ht_material_max_T(h, m));
    snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "}}");
    info->frames = lab_frames_written(w);
    info->steps = ht_steps(h);
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    bool stable = !ht_unstable(h);
    ht_free(h);
    return ok && stable;
}
