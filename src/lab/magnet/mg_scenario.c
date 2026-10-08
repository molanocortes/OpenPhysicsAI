/* mg_scenario.c - a magnetostatic run described in JSON (docs/lab/magnet.md has every key): one field, or a sweep of
 * the rotor through a range of angles with three-phase currents that turn with it, which gives torque against angle.
 * Every material must say where its properties come from ("source"). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "magnet.h"

bool mg_run_drive(const JsonValue *root, const MgSpec *s, Magnet *m, const int *phase, const double *sign, int pole_pairs, double Jpk,
                  double e0, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen);

/* the 3D motor (motor3d.c) links only where MFEM is built (src/lab/lab.mk) */
#if NAVIER_HAS_MFEM
bool lab_run_magnet3d(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen);
bool lab_run_induction(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen);
#endif

bool lab_run_magnet(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    if (json_get(root, "induction")) {
#if NAVIER_HAS_MFEM
        return lab_run_induction(root, out, quiet, info, err, errlen);
#endif
        snprintf(err, errlen, "this build has no 3D magnetics: build MFEM first (make mfem), then rebuild");
        return false;
    }
    if (json_get(root, "three_d")) {
#if NAVIER_HAS_MFEM
        return lab_run_magnet3d(root, out, quiet, info, err, errlen);
#endif
        snprintf(err, errlen, "this build has no 3D magnetics: build MFEM first (make mfem), then rebuild");
        return false;
    }
    static MgSpec s;
    mg_spec_defaults(&s);
    const JsonValue *g = json_get(root, "grid");
    s.nx = (int)json_get_int(g, "nx", 0), s.ny = (int)json_get_int(g, "ny", 0), s.dx = json_get_num(g, "cell_m", -1);
    const JsonValue *mats = json_get(root, "materials");
    for (size_t k = 0; mats && k < json_len(mats) && s.nmaterials < MG_MAX_MATERIALS; k++) {
        const JsonValue *M = json_at(mats, k);
        if (!json_get_str(M, "source", NULL)) {
            snprintf(err, errlen, "materials[%zu]: every material needs a source for its properties", k);
            return false;
        }
        MgMaterial *H = &s.materials[s.nmaterials++];
        snprintf(H->name, sizeof H->name, "%s", json_get_str(M, "name", "material"));
        H->mu_r = json_get_num(M, "relative_permeability", 1), H->Br = json_get_num(M, "remanence_t", 0);
        H->conductivity = json_get_num(M, "conductivity_s_m", 0);
    }
    /* windings: phase (A, B or C) and sign; their current density follows the rotor */
    int phase[MG_MAX_REGIONS];
    double sign[MG_MAX_REGIONS];
    const JsonValue *regs = json_get(root, "regions");
    for (size_t k = 0; regs && k < json_len(regs) && s.nregions < MG_MAX_REGIONS; k++) {
        const JsonValue *R = json_at(regs, k);
        MgRegion *H = &s.regions[s.nregions];
        double a[6] = {0};
        if (json_get_numbers(json_get(R, "box_m"), a, 4)) H->shape = MG_BOX;
        else if (json_get_numbers(json_get(R, "circle_m"), a, 3)) H->shape = MG_CIRCLE;
        else if (json_get_numbers(json_get(R, "sector_m_deg"), a, 6)) H->shape = MG_SECTOR;
        else {
            snprintf(err, errlen, "regions[%zu]: box_m [x0,y0,x1,y1], circle_m [x,y,r] or sector_m_deg [x,y,r_in,r_out,angle0,angle1]", k);
            return false;
        }
        memcpy(H->a, a, sizeof a);
        const char *mn = json_get_str(R, "material", "air");
        H->material = -1;
        for (int m = 0; m < s.nmaterials; m++)
            if (!strcmp(s.materials[m].name, mn)) H->material = m;
        if (H->material < 0) {
            snprintf(err, errlen, "regions[%zu]: unknown material \"%s\"", k, mn);
            return false;
        }
        H->J = json_get_num(R, "current_density_a_m2", 0);
        const char *mg = json_get_str(R, "magnetisation", "none");
        H->magnetisation = !strcmp(mg, "parallel") ? MG_MAG_PARALLEL : !strcmp(mg, "radial") ? MG_MAG_RADIAL : MG_MAG_NONE;
        H->mag_angle_deg = json_get_num(R, "magnetisation_angle_deg", !strcmp(mg, "radial") ? 1 : 0);
        H->rotor = json_get_bool(R, "rotor", false);
        const char *ph = json_get_str(R, "phase", "");
        phase[s.nregions] = ph[0] == 'A' ? 0 : ph[0] == 'B' ? 1 : ph[0] == 'C' ? 2 : -1;
        sign[s.nregions] = json_get_num(R, "phase_sign", 1);
        s.nregions++;
    }
    json_get_numbers(json_get(root, "applied_field_t"), s.applied_B, 2);
    const JsonValue *rot = json_get(root, "rotor");
    json_get_numbers(json_get(rot, "centre_m"), s.rotor_centre, 2);
    s.gap_radius = json_get_num(rot, "torque_radius_m", 0);
    int pole_pairs = (int)json_get_int(rot, "pole_pairs", 1);
    const JsonValue *cur = json_get(root, "currents");
    double Jpk = json_get_num(cur, "peak_density_a_m2", 0), e0 = json_get_num(cur, "electrical_angle_at_zero_deg", 0);
    const JsonValue *run = json_get(root, "run");
    double a0 = json_get_num(run, "angle_from_deg", 0), a1 = json_get_num(run, "angle_to_deg", 0);
    int frames = (int)json_get_int(run, "frames", 1);
    if (frames < 1) frames = 1;
    Magnet *m = mg_create(&s, err, errlen);
    if (!m) return false;
    if (json_get(root, "drive")) /* a motor that spins up and heats: mg_drive.c */
        return mg_run_drive(root, &s, m, phase, sign, pole_pairs, Jpk, e0, out, quiet, info, err, errlen);
    char title[160];
    snprintf(title, sizeof title, "%s", json_get_str(root, "title", "magnet run"));
    char *hdr = mg_header_json(&s, title);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        mg_free(m);
        return false;
    }
    bool ok = true;
    double tsum = 0, tmin = INFINITY, tmax = -INFINITY, loss = 0;
    int n = snprintf(info->diagnostics, sizeof info->diagnostics, "{\"torque_n_m_per_m\":[");
    for (int f = 0; f < frames && ok; f++) {
        double ang = frames > 1 ? a0 + (a1 - a0) * f / (frames - 1) : a0;
        double el = (e0 + pole_pairs * ang) * M_PI / 180; /* the currents turn with the rotor */
        for (int k = 0; k < s.nregions; k++)
            if (phase[k] >= 0) mg_set_region_current(m, k, sign[k] * Jpk * cos(el - phase[k] * 2 * M_PI / 3));
        mg_set_rotor_angle(m, ang);
        int it = mg_solve(m);
        if (it < 0) {
            snprintf(err, errlen, "the field did not converge at %.2f degrees", ang);
            ok = false;
            break;
        }
        double T = mg_torque(m);
        tsum += T, tmin = fmin(tmin, T), tmax = fmax(tmax, T), loss = mg_copper_loss(m);
        if (n < (int)sizeof info->diagnostics - 64) n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "%s%.6g", f ? "," : "", T);
        lab_frame_begin(w, ang);
        mg_write_frame(m, w);
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", f);
            ok = false;
        }
        if (!quiet) fprintf(stderr, "  angle %7.2f deg  %d iterations  torque %.5g N m/m\n", ang, it, T);
    }
    snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n,
             "],\"mean_torque_n_m_per_m\":%.6g,\"ripple_n_m_per_m\":%.6g,\"copper_loss_w_per_m\":%.6g}", tsum / frames, tmax - tmin, loss);
    info->frames = lab_frames_written(w);
    info->steps = frames;
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    mg_free(m);
    return ok;
}
