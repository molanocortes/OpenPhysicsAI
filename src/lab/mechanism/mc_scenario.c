/* mc_scenario.c - the physics lab's "mechanism" domain: elastic parts of a mechanism in contact (src/lab/mechanism).
 *
 * Model "gear_pair": a pinion driving a gear (mgearpair.h), through the positions of the mesh cycle. A scenario names
 * the two gears as a cutter makes them (gear.h), their material with its source, how they are meshed, the torque on
 * the pinion, the positions (pinion turns, or the roll angle of the contact on one gauged tooth of the gear), optional
 * strain gauges on fillets, and whether only one tooth of the pinion is there to carry load (a calibration rig whose
 * other teeth are ground away). Each position is a frame: both gears in place, coloured by von Mises stress, with the
 * contact forces on the flanks; the diagnostics give, per position, the transmission error, the load each tooth pair
 * carries and the gauges' readings.
 *
 * Model "gear_tooth": one tooth of a gear loaded on its flank by a normal force at a given radius (mtooth.h), the
 * single-tooth bending test. A scenario names the gear and its material, how it is meshed, the load radius and the
 * forces, and gauges by position (Hofer's 30 degree tangent point, the computed maximum, an arc length from the tip,
 * or a radius). Each force is a frame coloured by von Mises stress with the surface stress on the nodes; the
 * diagnostics give the largest tensile and compressive surface stresses and where they are, and each gauge's
 * reading as the stress a uniaxial reading of its strain gives (E times the strain), as gauge results are published. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "mgearpair.h"
#include "mtooth.h"
#include "mplanet.h"
#include "mtrain.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static bool gear_from_json(const JsonValue *j, GearSpec *g, double *E, double *nu, char *err, size_t errlen) {
    gear_spec_defaults(g);
    g->teeth = (int)json_get_int(j, "teeth", 0);
    g->module_m = json_get_num(j, "module_m", 0);
    g->pressure_deg = json_get_num(j, "pressure_angle_deg", 20);
    g->shift = json_get_num(j, "profile_shift", 0);
    g->addendum = json_get_num(j, "addendum_coefficient", 1);
    g->rack_addendum = json_get_num(j, "dedendum_coefficient", 1.25);
    g->rack_tip_radius = json_get_num(j, "cutter_tip_radius_coefficient", 0.38);
    g->tip_radius_m = json_get_num(j, "tip_diameter_m", 0) / 2;
    g->thinning_m = json_get_num(j, "flank_thinning_m", 0);
    g->tip_relief_m = json_get_num(j, "tip_relief_m", 0);
    g->face_width_m = json_get_num(j, "face_width_m", 0);
    g->lead_crown_m = json_get_num(j, "lead_crown_m", 0);
    g->bore_radius_m = json_get_num(j, "bore_diameter_m", 0) / 2;
    /* helical: the normal module and pressure angle as given; positive a right-hand helix */
    g->helix_deg = json_get_num(j, "helix_angle_deg", 0);
    /* the relief's start as a roll angle on the gear's own involute: r = r_b sqrt(1 + theta^2), r_b transverse */
    double roll = json_get_num(j, "tip_relief_start_roll_deg", 0);
    if (roll > 0 && g->module_m > 0 && g->teeth > 0) {
        double be = g->helix_deg * M_PI / 180, at = atan(tan(g->pressure_deg * M_PI / 180) / cos(be));
        double rb = g->module_m / cos(be) * g->teeth / 2 * cos(at), th = roll * M_PI / 180;
        g->relief_start_m = rb * sqrt(1 + th * th);
    }
    const JsonValue *mat = json_get(j, "material");
    *E = json_get_num(mat, "youngs_modulus_pa", 0), *nu = json_get_num(mat, "poisson_ratio", 0.3);
    if (!mat || *E <= 0) return snprintf(err, errlen, "mechanism: each gear needs a material with youngs_modulus_pa"), false;
    if (!json_str(json_get(mat, "source"))) return snprintf(err, errlen, "mechanism: a gear's material needs its source"), false;
    if (g->teeth < 6 || g->module_m <= 0 || g->face_width_m <= 0 || g->bore_radius_m <= 0)
        return snprintf(err, errlen, "mechanism: each gear needs teeth, module_m, face_width_m and bore_diameter_m"), false;
    return true;
}

static void mesh_from_json(const JsonValue *j, GearMeshSpec *m) {
    gear_mesh_defaults(m);
    m->teeth_modelled = (int)json_get_int(j, "teeth_modelled", m->teeth_modelled);
    m->n_across = (int)json_get_int(j, "across", m->n_across);
    m->n_flank = (int)json_get_int(j, "flank", m->n_flank);
    m->n_fillet = (int)json_get_int(j, "fillet", m->n_fillet);
    m->n_rim = (int)json_get_int(j, "rim", m->n_rim);
    m->n_face = (int)json_get_int(j, "face", m->n_face);
    m->flank_grading = json_get_num(j, "flank_grading", m->flank_grading);
    m->rim_grading = json_get_num(j, "rim_grading", m->rim_grading);
}

static bool run_gear_tooth(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    double t0 = now();
    GearToothSpec S;
    memset(&S, 0, sizeof S);
    if (!gear_from_json(json_get(root, "gear"), &S.gear, &S.E, &S.nu, err, errlen)) return false;
    mesh_from_json(json_get(root, "mesh"), &S.mesh);
    if (!json_get(json_get(root, "mesh"), "teeth_modelled")) S.mesh.teeth_modelled = 3;
    S.hold_cut_faces = json_get_bool(root, "hold_sector_cuts", true);
    const JsonValue *load = json_get(root, "load");
    S.load_radius_m = json_get_num(load, "radius_m", 0);
    S.contact_halfwidth_m = json_get_num(load, "contact_halfwidth_m", 0);
    S.anvil_friction = json_get_num(load, "anvil_friction_coefficient", 0);
    const JsonValue *forces = json_get(load, "normal_force_n");
    int nf = (int)json_len(forces);
    if (S.load_radius_m <= 0 || nf < 1) return snprintf(err, errlen, "mechanism: gear_tooth needs load.radius_m and a list load.normal_force_n"), false;
    const JsonValue *gl = json_get(root, "gauges");
    for (size_t k = 0; k < json_len(gl) && S.ngauges < 8; k++) {
        const JsonValue *g = json_at(gl, k);
        ToothGauge *G = &S.gauge[S.ngauges++];
        snprintf(G->name, sizeof G->name, "%s", json_get_str(g, "name", "gauge"));
        G->loaded_side = !strcmp(json_get_str(g, "side", "loaded"), "loaded");
        const char *where = json_get_str(g, "position", "30deg");
        G->where = !strcmp(where, "max") ? TG_MAX : !strcmp(where, "arc") ? TG_ARC : !strcmp(where, "radius") ? TG_RADIUS : TG_HOFER30;
        G->arc_m = json_get_num(g, "arc_from_tip_m", 0);
        G->radius_m = json_get_num(g, "radius_m", 0);
        G->z_m = json_get_num(g, "z_m", -1);
        G->length_m = json_get_num(g, "length_m", 0);
        if ((G->where == TG_ARC && G->arc_m <= 0) || (G->where == TG_RADIUS && G->radius_m <= 0))
            return snprintf(err, errlen, "mechanism: gauge %s needs arc_from_tip_m or radius_m for its position", G->name), false;
    }
    double view_scale = json_get_num(root, "view_displacement_scale", 0);
    if (!quiet) fprintf(stderr, "  mechanism: building and factorising the gear sector\n");
    GearTooth *gt = geartooth_create(&S, err, errlen);
    if (!gt) return false;
    if (!quiet) fprintf(stderr, "  mechanism: %s\n", geartooth_summary(gt));
    JsonValue *meta = json_object();
    json_set_string(meta, "domain", "mechanism");
    json_set_string(meta, "title", json_get_str(root, "title", "one gear tooth loaded on its flank"));
    json_set_string(meta, "time_name", "force");
    json_set_string(meta, "time_unit", "N");
    json_set_string(meta, "model",
                    "single-tooth bending by finite elements: hex8 with incompatible modes on the exact generated outline (involute and "
                    "trochoidal fillet), the sector held at its bore, a normal flank load with the Hertzian pressure of a flat anvil");
    JsonValue *fields = json_set_object(meta, "fields");
    json_set_string(fields, "mises", "Pa");
    json_set_string(fields, "stress", "Pa");
    json_set_string(fields, "surface_stress", "Pa");
    json_set(meta, "scenario", json_clone(root));
    char *hdr = json_dump(meta, 0, NULL, NULL);
    json_free(meta);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        geartooth_free(gt);
        return false;
    }
    JsonValue *diag = json_object();
    JsonValue *a_f = json_set_array(diag, "normal_force_n"), *a_t = json_set_array(diag, "max_tension_mpa"), *a_ta = json_set_array(diag, "max_tension_arc_mm");
    JsonValue *a_c = json_set_array(diag, "max_compression_mpa"), *a_ca = json_set_array(diag, "max_compression_arc_mm");
    JsonValue *a_g[8], *a_ga[8];
    for (int k = 0; k < S.ngauges; k++) {
        char key[96];
        snprintf(key, sizeof key, "gauge_%s_mpa", S.gauge[k].name);
        a_g[k] = json_set_array(diag, key);
        snprintf(key, sizeof key, "gauge_%s_arc_mm", S.gauge[k].name);
        a_ga[k] = json_set_array(diag, key);
    }
    bool ok = true;
    for (int i = 0; i < nf && ok; i++) {
        const JsonValue *fv = json_at(forces, i);
        double F = fv && fv->type == JSON_NUMBER ? fv->u.number : 0;
        GearToothResult r;
        if (F <= 0) {
            snprintf(err, errlen, "mechanism: normal forces must be positive");
            ok = false;
            break;
        }
        if (!geartooth_solve(gt, F, &r, err, errlen)) {
            ok = false;
            break;
        }
        json_push(a_f, json_number(F));
        json_push(a_t, json_number(round(r.max_tension_pa / 1e4) / 100));
        json_push(a_ta, json_number(round(r.max_tension_arc_m * 1e5) / 100));
        json_push(a_c, json_number(round(r.max_compression_pa / 1e4) / 100));
        json_push(a_ca, json_number(round(r.max_compression_arc_m * 1e5) / 100));
        for (int k = 0; k < S.ngauges; k++) {
            json_push(a_g[k], json_number(round(S.E * r.gauge_strain[k] / 1e4) / 100));
            json_push(a_ga[k], json_number(round(r.gauge_arc_m[k] * 1e5) / 100));
        }
        const double *xyz;
        const int *conn;
        const float *mises, *surf, *nmises;
        int nn, ne;
        geartooth_frame(gt, view_scale, &xyz, &nn, &conn, &ne, &mises, &surf, &nmises);
        lab_frame_begin(w, F);
        lab_part_cells(w, "gear", nn, xyz, ne, LAB_HEX, conn);
        lab_field(w, "mises", LAB_AT_CELL, ne, mises);
        lab_field(w, "stress", LAB_AT_NODE, nn, nmises);
        lab_field(w, "surface_stress", LAB_AT_NODE, nn, surf);
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", i);
            ok = false;
        }
        if (!quiet) {
            fprintf(stderr, "  force %8.1f N  contact half-width %.3f mm  max tension %8.2f MPa at %.2f mm from the tip, max compression %8.2f MPa at %.2f mm\n", F,
                    1e3 * r.halfwidth_m, r.max_tension_pa / 1e6, 1e3 * r.max_tension_arc_m, r.max_compression_pa / 1e6, 1e3 * r.max_compression_arc_m);
            if (S.anvil_friction > 0)
                fprintf(stderr, "    anvil friction %.3f: the flank slides %.2f um %s under the anvil, so friction acts toward the %s\n", S.anvil_friction,
                        1e6 * fabs(r.surface_slip_m), r.surface_slip_m > 0 ? "toward the tip" : "toward the root", r.friction_sense > 0 ? "tip" : "root");
            for (int k = 0; k < S.ngauges; k++)
                fprintf(stderr, "    gauge %-12s at %8.2f mm from the tip: %9.1f microstrain, E x strain %8.2f MPa\n", S.gauge[k].name, 1e3 * r.gauge_arc_m[k],
                        1e6 * r.gauge_strain[k], S.E * r.gauge_strain[k] / 1e6);
            if (getenv("OPAI_MECH_DEBUG"))
                for (int side = 1; side >= 0; side--) {
                    const double *pa, *ps;
                    int np = geartooth_profile(gt, side, &pa, &ps);
                    fprintf(stderr, "    %s side, mid-face (arc from tip mm, sigma_t MPa):", side ? "loaded" : "unloaded");
                    for (int q = 0; q < np; q++) fprintf(stderr, "%s%.2f %.2f", q % 6 ? "   " : "\n     ", 1e3 * pa[q], ps[q] / 1e6);
                    fprintf(stderr, "\n");
                }
        }
    }
    if (ok) {
        json_set_string(diag, "omitted", "friction at the anvil; the rig's clamping (the sector is held at its bore and cut faces)");
        json_set_number(diag, "wall_s", round((now() - t0) * 10) / 10);
        size_t len;
        char *d = json_dump(diag, 0, &len, NULL);
        if (d && len < sizeof info->diagnostics) snprintf(info->diagnostics, sizeof info->diagnostics, "%s", d);
        else snprintf(info->diagnostics, sizeof info->diagnostics, "{\"note\":\"too many forces for the run record; the frames hold them\",\"wall_s\":%.1f}", now() - t0);
        free(d);
    }
    json_free(diag);
    info->frames = lab_frames_written(w);
    info->steps = nf;
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    geartooth_free(gt);
    return ok;
}

/* Model "planetary": a sun, planets on a carrier and an internal ring (mplanet.h), the ring and the carrier held, a
 * torque on the sun, at carrier angles; frames hold every part with its stress, the diagnostics each position's
 * planet load shares, the sun's wind-up and the reactions. */
static bool run_planetary(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    double t0 = now();
    PlanetarySpec S;
    memset(&S, 0, sizeof S);
    double E, nu;
    if (!gear_from_json(json_get(root, "sun"), &S.sun, &S.E[0], &S.nu[0], err, errlen)) return false;
    if (!gear_from_json(json_get(root, "planet"), &S.planet, &S.E[1], &S.nu[1], err, errlen)) return false;
    const JsonValue *rj = json_get(root, "ring");
    /* the ring's "bore_diameter_m" is the outer diameter of its rim, where it is held */
    if (!gear_from_json(rj, &S.ring, &S.E[2], &S.nu[2], err, errlen)) return false;
    S.ring.rack_addendum = 0, S.ring.rack_tip_radius = 0;
    const JsonValue *cj = json_get(rj, "cutter");
    if (!cj) return snprintf(err, errlen, "mechanism: the ring needs its shaper \"cutter\" (teeth, addendum_coefficient, ...)"), false;
    gear_spec_defaults(&S.cutter);
    S.cutter.teeth = (int)json_get_int(cj, "teeth", 0);
    S.cutter.module_m = S.ring.module_m, S.cutter.pressure_deg = S.ring.pressure_deg;
    S.cutter.addendum = json_get_num(cj, "addendum_coefficient", 1.25);
    S.cutter.shift = json_get_num(cj, "profile_shift", 0);
    S.cutter.rack_addendum = json_get_num(cj, "dedendum_coefficient", 1.25);
    S.cutter.rack_tip_radius = json_get_num(cj, "cutter_tip_radius_coefficient", 0.38);
    S.cutter.face_width_m = S.ring.face_width_m;
    (void)E, (void)nu;
    const JsonValue *mj = json_get(root, "mesh");
    mesh_from_json(json_get(mj, "sun"), &S.mesh_sun);
    mesh_from_json(json_get(mj, "planet"), &S.mesh_planet);
    mesh_from_json(json_get(mj, "ring"), &S.mesh_ring);
    S.mesh_sun.teeth_modelled = S.sun.teeth, S.mesh_planet.teeth_modelled = S.planet.teeth, S.mesh_ring.teeth_modelled = S.ring.teeth;
    S.nplanets = (int)json_get_int(root, "planets", 3);
    S.sun_torque_nm = json_get_num(json_get(root, "load"), "sun_torque_nm", 0);
    if (!(S.sun_torque_nm > 0)) return snprintf(err, errlen, "mechanism: load.sun_torque_nm (> 0) is needed"), false;
    S.gap_cut_m = json_get_num(root, "candidate_gap_m", 0);
    S.sun_floating = json_get_bool(root, "sun_floating", false);
    const JsonValue *pe = json_get(root, "pin_errors_m");
    for (size_t k = 0; k < json_len(pe) && k < PL_MAX_PLANETS; k++) {
        const JsonValue *v = json_at(pe, k);
        S.pin_error_m[k][0] = json_len(v) > 0 && json_at(v, 0)->type == JSON_NUMBER ? json_at(v, 0)->u.number : 0;
        S.pin_error_m[k][1] = json_len(v) > 1 && json_at(v, 1)->type == JSON_NUMBER ? json_at(v, 1)->u.number : 0;
    }
    const JsonValue *run = json_get(root, "run");
    int n = (int)json_get_int(run, "positions", 1);
    double c0 = json_get_num(run, "carrier_from_deg", 0), c1 = json_get_num(run, "carrier_to_deg", 0);
    if (n < 1) return snprintf(err, errlen, "mechanism: run.positions >= 1"), false;
    if (!quiet) fprintf(stderr, "  mechanism: building and factorising the stage's %d parts\n", S.nplanets + 2);
    Planetary *pl = planetary_create(&S, err, errlen);
    if (!pl) return false;
    if (!quiet) fprintf(stderr, "  mechanism: %s\n", planetary_summary(pl));
    JsonValue *meta = json_object();
    json_set_string(meta, "domain", "mechanism");
    json_set_string(meta, "title", json_get_str(root, "title", "a planetary gear stage under load"));
    json_set_string(meta, "time_name", "carrier");
    json_set_string(meta, "time_unit", "deg");
    json_set_string(meta, "model",
                    "a planetary stage by finite elements: every gear elastic (hex8 with incompatible modes) on its exact generated outline, the ring "
                    "cut by a shaper, frictionless contact with the sun's and each planet's rotation as rigid degrees of freedom, the ring and the "
                    "carrier held");
    JsonValue *fields = json_set_object(meta, "fields");
    json_set_string(fields, "mises", "Pa");
    json_set_string(fields, "stress", "Pa");
    json_set(meta, "scenario", json_clone(root));
    char *hdr = json_dump(meta, 0, NULL, NULL);
    json_free(meta);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        planetary_free(pl);
        return false;
    }
    JsonValue *diag = json_object();
    JsonValue *a_c = json_set_array(diag, "carrier_deg"), *a_wu = json_set_array(diag, "sun_windup_urad"), *a_tc = json_set_array(diag, "carrier_torque_nm");
    JsonValue *a_tr = json_set_array(diag, "ring_torque_nm"), *a_sh = json_set_array(diag, "load_share"), *a_vm = json_set_array(diag, "max_mises_mpa");
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        double cdeg = n > 1 ? c0 + (c1 - c0) * i / (n - 1) : c0;
        PlanetaryResult r;
        if (!planetary_solve(pl, cdeg, &r, true, err, errlen)) {
            ok = false;
            break;
        }
        json_push(a_c, json_number(round(cdeg * 1e4) / 1e4));
        json_push(a_wu, json_number(round(r.sun_windup_rad * 1e9) / 1e3));
        json_push(a_tc, json_number(round(r.carrier_torque_nm * 1e4) / 1e4));
        json_push(a_tr, json_number(round(r.ring_torque_nm * 1e4) / 1e4));
        JsonValue *shares = json_array();
        for (int k = 0; k < S.nplanets; k++) json_push(shares, json_number(round(r.load_share[k] * 1e5) / 1e5));
        json_push(a_sh, shares);
        json_push(a_vm, json_number(round(r.max_mises_pa / 1e5) / 10));
        lab_frame_begin(w, cdeg);
        for (int p = 0; p < planetary_parts(pl); p++) {
            const char *name;
            const double *xyz;
            const int *conn;
            const float *mises, *nmises;
            int nn, ne;
            planetary_part(pl, p, &name, &xyz, &nn, &conn, &ne, &mises, &nmises);
            lab_part_cells(w, name, nn, xyz, ne, LAB_HEX, conn);
            lab_field(w, "mises", LAB_AT_CELL, ne, mises);
            lab_field(w, "stress", LAB_AT_NODE, nn, nmises);
        }
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", i);
            ok = false;
        }
        if (!quiet) {
            fprintf(stderr, "  carrier %7.3f deg  wind-up %8.2f urad  contacts %d (%d closed, %d pivots)  torques: sun %.4f, carrier %.4f, ring %.4f N m  shares",
                    cdeg, 1e6 * r.sun_windup_rad, r.contacts, r.active, r.pivots, r.sun_torque_check_nm, r.carrier_torque_nm, r.ring_torque_nm);
            for (int k = 0; k < S.nplanets; k++) fprintf(stderr, " %.4f", r.load_share[k]);
            if (S.sun_floating) fprintf(stderr, "  sun floats %.3f, %.3f um", 1e6 * r.sun_float_m[0], 1e6 * r.sun_float_m[1]);
            fprintf(stderr, "\n");
        }
    }
    if (ok) {
        json_set_string(diag, "omitted", "friction; the pins' and bearings' compliance; a floating sun; dynamics");
        json_set_number(diag, "wall_s", round((now() - t0) * 10) / 10);
        size_t len;
        char *d = json_dump(diag, 0, &len, NULL);
        if (d && len < sizeof info->diagnostics) snprintf(info->diagnostics, sizeof info->diagnostics, "%s", d);
        else snprintf(info->diagnostics, sizeof info->diagnostics, "{\"note\":\"too many positions for the run record; the frames hold them\",\"wall_s\":%.1f}", now() - t0);
        free(d);
    }
    json_free(diag);
    info->frames = lab_frames_written(w);
    info->steps = n;
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    planetary_free(pl);
    return ok;
}

/* Model "gear_train": gears on shafts and carriers in any layout (mtrain.h).
 *   "gears":  {name: a gear as above, with "internal": true and its shaper "cutter" for a ring, "helix_angle_deg"
 *              (normal module and pressure angle; positive a right-hand helix) and its "mesh" (the whole gear unless
 *              "teeth_modelled" says otherwise)}; gears of one name share one model (a stage's planets)
 *   "shafts": [{"name", "axis_m": [x, y], "carrier": a shaft's name, "fixed", "locked", "floating", "torque_nm",
 *              "copies": N (a carried shaft repeated evenly round its carrier, named "name 1".."name N") or
 *              "angles_deg": [...] (the copies' angles from the first), "pin_errors_m": [[along, outward], ...] per copy}]
 *   "parts":  [{"name", "gear", "shaft", "z_m": where its face starts along the axis, "phase_deg" (optional)}]; a part
 *              on a repeated shaft is repeated with it
 *   "meshes": [[part, part], ...]; a mesh with a repeated part is repeated with it
 *   "input":  the shaft whose turn places the train; "run": {"positions", "input_from_deg", "input_to_deg"}
 * Frames hold every gear with its stress, and the carriers' plates and pins and the shafts' hubs (rigid, grey); the
 * diagnostics give every shaft's ratio, and per position its wind-up and settled offset, the torque and axial force
 * the contacts put on it, every mesh's force on each flank, the elastic energy stored, and the contact searches the
 * pose took to settle (mtrain.h). */
typedef struct NamedCopies {
    char name[32];
    int first, n; /* indices of the copies (n = 1: not repeated) */
} NamedCopies;

static int find_named(const NamedCopies *v, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(v[i].name, name)) return i;
    return -1;
}

static bool train_model_from_json(const char *name, const JsonValue *j, TrainModelSpec *M, char *err, size_t errlen) {
    memset(M, 0, sizeof *M);
    snprintf(M->name, sizeof M->name, "%s", name);
    char e2[200];
    if (!gear_from_json(j, &M->gear, &M->E, &M->nu, e2, sizeof e2)) return snprintf(err, errlen, "gear %s: %s", name, e2), false;
    M->internal = json_get_bool(j, "internal", false);
    if (M->internal) {
        M->gear.rack_addendum = 0, M->gear.rack_tip_radius = 0;
        const JsonValue *cj = json_get(j, "cutter");
        if (!cj) return snprintf(err, errlen, "mechanism: the ring %s needs its shaper \"cutter\" (teeth, addendum_coefficient, ...)", name), false;
        gear_spec_defaults(&M->cutter);
        M->cutter.teeth = (int)json_get_int(cj, "teeth", 0);
        M->cutter.module_m = M->gear.module_m, M->cutter.pressure_deg = M->gear.pressure_deg, M->cutter.helix_deg = M->gear.helix_deg;
        M->cutter.addendum = json_get_num(cj, "addendum_coefficient", 1.25);
        M->cutter.shift = json_get_num(cj, "profile_shift", 0);
        M->cutter.rack_addendum = json_get_num(cj, "dedendum_coefficient", 1.25);
        M->cutter.rack_tip_radius = json_get_num(cj, "cutter_tip_radius_coefficient", 0.38);
        M->cutter.face_width_m = M->gear.face_width_m;
    }
    const JsonValue *mj = json_get(j, "mesh");
    mesh_from_json(mj, &M->mesh);
    if (!json_get(mj, "teeth_modelled")) M->mesh.teeth_modelled = M->gear.teeth;
    const JsonValue *hj = json_get(j, "hold"); /* "lugs", "lug_width_deg", "phase_deg", "radial_free" */
    M->hold.lugs = (int)json_get_int(hj, "lugs", 0);
    M->hold.lug_deg = json_get_num(hj, "lug_width_deg", 0), M->hold.phase_deg = json_get_num(hj, "phase_deg", 0);
    M->hold.radial_free = json_get_bool(hj, "radial_free", false);
    if (M->hold.lugs < 0 || (M->hold.lugs > 0 && !(M->hold.lug_deg > 0 && M->hold.lug_deg * M->hold.lugs < 360)))
        return snprintf(err, errlen, "mechanism: gear %s: hold.lugs needs lug_width_deg between 0 and 360 / lugs", name), false;
    return true;
}

static bool train_from_json(const JsonValue *root, TrainSpec *S, NamedCopies *shafts, int *nsh_named, NamedCopies *parts, int *npt_named,
                            char *err, size_t errlen) {
    memset(S, 0, sizeof *S);
    const JsonValue *gj = json_get(root, "gears");
    if (!gj || gj->type != JSON_OBJECT || gj->u.object.len == 0) return snprintf(err, errlen, "mechanism: gear_train needs \"gears\": {name: gear}"), false;
    if (gj->u.object.len > TR_MAX_MODELS) return snprintf(err, errlen, "mechanism: at most %d kinds of gear", TR_MAX_MODELS), false;
    for (size_t i = 0; i < gj->u.object.len; i++)
        if (!train_model_from_json(gj->u.object.keys[i], gj->u.object.values[i], &S->model[S->nmodels++], err, errlen)) return false;
    /* shafts: carriers first by name, then the copies */
    const JsonValue *sj = json_get(root, "shafts");
    int nn = 0;
    for (size_t i = 0; i < json_len(sj); i++) {
        const JsonValue *v = json_at(sj, i);
        const char *name = json_get_str(v, "name", NULL);
        if (!name || strlen(name) > 24 || find_named(shafts, nn, name) >= 0) return snprintf(err, errlen, "mechanism: each shaft needs a unique short name"), false;
        const JsonValue *ax = json_get(v, "axis_m"), *ang = json_get(v, "angles_deg"), *pe = json_get(v, "pin_errors_m");
        int copies = ang ? (int)json_len(ang) : (int)json_get_int(v, "copies", 1);
        const char *cname = json_get_str(v, "carrier", NULL);
        int carrier = -1;
        if (cname) {
            int c = find_named(shafts, nn, cname);
            if (c < 0 || shafts[c].n != 1) return snprintf(err, errlen, "mechanism: shaft %s: its carrier %s must be a single shaft listed before it", name, cname), false;
            carrier = shafts[c].first;
        }
        if (copies < 1 || (copies > 1 && carrier < 0)) return snprintf(err, errlen, "mechanism: shaft %s: only a carried shaft can be repeated", name), false;
        if (S->nshafts + copies > TR_MAX_SHAFTS) return snprintf(err, errlen, "mechanism: at most %d shafts", TR_MAX_SHAFTS), false;
        double x = json_len(ax) > 0 ? json_at(ax, 0)->u.number : 0, y = json_len(ax) > 1 ? json_at(ax, 1)->u.number : 0;
        snprintf(shafts[nn].name, sizeof shafts[nn].name, "%s", name);
        shafts[nn].first = S->nshafts, shafts[nn].n = copies;
        nn++;
        for (int k = 0; k < copies; k++) {
            TrainShaftSpec *sh = &S->shaft[S->nshafts++];
            if (copies > 1) snprintf(sh->name, sizeof sh->name, "%s %d", name, k + 1);
            else snprintf(sh->name, sizeof sh->name, "%s", name);
            double a = ang ? json_at(ang, k)->u.number * M_PI / 180 : 2 * M_PI * k / copies, cx = 0, cy = 0;
            if (carrier >= 0) cx = S->shaft[carrier].x, cy = S->shaft[carrier].y;
            sh->x = cx + cos(a) * (x - cx) - sin(a) * (y - cy), sh->y = cy + sin(a) * (x - cx) + cos(a) * (y - cy);
            sh->carrier = carrier;
            sh->fixed = json_get_bool(v, "fixed", false), sh->locked = json_get_bool(v, "locked", false), sh->floating = json_get_bool(v, "floating", false);
            sh->torque_nm = json_get_num(v, "torque_nm", 0);
            sh->support_n_per_m = json_get_num(v, "support_n_per_m", 0);
            sh->plate_front = !strcmp(json_get_str(v, "plate", "back"), "front");
            const JsonValue *e = json_at(pe, k);
            for (int d = 0; d < 2; d++) sh->pin_error_m[d] = json_len(e) > (size_t)d && json_at(e, d)->type == JSON_NUMBER ? json_at(e, d)->u.number : 0;
        }
    }
    *nsh_named = nn;
    /* parts, repeated with their shafts */
    const JsonValue *pj = json_get(root, "parts");
    int np = 0;
    for (size_t i = 0; i < json_len(pj); i++) {
        const JsonValue *v = json_at(pj, i);
        const char *name = json_get_str(v, "name", NULL), *gname = json_get_str(v, "gear", NULL), *sname = json_get_str(v, "shaft", NULL);
        if (!name || strlen(name) > 24 || !gname || !sname || find_named(parts, np, name) >= 0)
            return snprintf(err, errlen, "mechanism: each part needs a unique short name, its gear and its shaft"), false;
        int model = -1, sh = find_named(shafts, nn, sname);
        for (int m = 0; m < S->nmodels; m++)
            if (!strcmp(S->model[m].name, gname)) model = m;
        if (model < 0 || sh < 0) return snprintf(err, errlen, "mechanism: part %s: no gear %s or shaft %s", name, gname, sname), false;
        int copies = shafts[sh].n;
        if (S->ngears + copies > TR_MAX_GEARS) return snprintf(err, errlen, "mechanism: at most %d gears", TR_MAX_GEARS), false;
        snprintf(parts[np].name, sizeof parts[np].name, "%s", name);
        parts[np].first = S->ngears, parts[np].n = copies;
        np++;
        for (int k = 0; k < copies; k++) {
            TrainGearSpec *g = &S->gear[S->ngears++];
            if (copies > 1) snprintf(g->name, sizeof g->name, "%s %d", name, k + 1);
            else snprintf(g->name, sizeof g->name, "%s", name);
            g->model = model, g->shaft = shafts[sh].first + k, g->z0_m = json_get_num(v, "z_m", 0);
            g->phase_deg = json_get(v, "phase_deg") ? json_get_num(v, "phase_deg", 0) : NAN;
        }
    }
    *npt_named = np;
    const JsonValue *mj = json_get(root, "meshes");
    for (size_t i = 0; i < json_len(mj); i++) {
        const JsonValue *v = json_at(mj, i);
        const char *a = json_str(json_at(v, 0)), *b = json_str(json_at(v, 1));
        int ia = a ? find_named(parts, np, a) : -1, ib = b ? find_named(parts, np, b) : -1;
        if (json_len(v) != 2 || ia < 0 || ib < 0) return snprintf(err, errlen, "mechanism: mesh %zu: two part names", i + 1), false;
        int na = parts[ia].n, nb = parts[ib].n, n = na > nb ? na : nb;
        if (na > 1 && nb > 1 && na != nb) return snprintf(err, errlen, "mechanism: mesh %s - %s: repeated parts of different counts", a, b), false;
        if (S->nmeshes + n > TR_MAX_MESHES) return snprintf(err, errlen, "mechanism: at most %d meshes", TR_MAX_MESHES), false;
        for (int k = 0; k < n; k++) {
            S->mesh[S->nmeshes].a = parts[ia].first + (na > 1 ? k : 0), S->mesh[S->nmeshes].b = parts[ib].first + (nb > 1 ? k : 0);
            S->nmeshes++;
        }
    }
    const char *in = json_get_str(root, "input", NULL);
    int si = in ? find_named(shafts, nn, in) : -1;
    if (si < 0 || shafts[si].n != 1) return snprintf(err, errlen, "mechanism: \"input\" names the (single) shaft whose turn places the train"), false;
    S->input = shafts[si].first;
    S->gap_cut_m = json_get_num(root, "candidate_gap_m", 0);
    return true;
}

/* the cells of a part outside a wedge of polar angles [a0, a1] about the z axis (by their centres), with their field */
static int cut_cells(const double *xyz, const int *conn, int ne, const float *field, double a0, double a1, int *oconn, float *ofield) {
    int m = 0;
    for (int e = 0; e < ne; e++) {
        double cx = 0, cy = 0;
        for (int k = 0; k < 8; k++) cx += xyz[3 * conn[8 * e + k]] / 8, cy += xyz[3 * conn[8 * e + k] + 1] / 8;
        double d = fmod(atan2(cy, cx) - a0, 2 * M_PI);
        if (d < 0) d += 2 * M_PI;
        if (a1 > a0 && d <= a1 - a0) continue;
        memcpy(&oconn[8 * m], &conn[8 * e], 8 * sizeof(int));
        if (field && ofield) ofield[m] = field[e];
        m++;
    }
    return m;
}

static bool run_gear_train(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    double t0 = now();
    TrainSpec *S = malloc(sizeof *S);
    NamedCopies shafts[TR_MAX_SHAFTS], parts[TR_MAX_GEARS];
    int nsh = 0, npt = 0;
    if (!S) return snprintf(err, errlen, "out of memory"), false;
    if (!train_from_json(root, S, shafts, &nsh, parts, &npt, err, errlen)) {
        free(S);
        return false;
    }
    S->range_deg[0] = json_get_num(json_get(root, "run"), "input_from_deg", 0), S->range_deg[1] = json_get_num(json_get(root, "run"), "input_to_deg", 0);
    const JsonValue *run = json_get(root, "run"), *tq = json_get(run, "torques_nm");
    bool frames = json_get_bool(run, "frames", true); /* false: a study, its numbers only */
    int n = (int)json_get_int(run, "positions", 1), ntq = json_len(tq) > 0 ? (int)json_len(tq) : 1;
    double a0 = json_get_num(run, "input_from_deg", 0), a1 = json_get_num(run, "input_to_deg", 0);
    if (n < 1) {
        free(S);
        return snprintf(err, errlen, "mechanism: run.positions >= 1"), false;
    }
    for (int k = 0; k < ntq && json_len(tq); k++)
        if (json_at(tq, k)->type != JSON_NUMBER) {
            free(S);
            return snprintf(err, errlen, "mechanism: run.torques_nm is a list of the input shaft's torques, N m"), false;
        }
    if (!quiet) fprintf(stderr, "  mechanism: building and factorising %d kinds of gear for %d gears on %d shafts, %d meshes\n", S->nmodels, S->ngears, S->nshafts, S->nmeshes);
    Train *t = train_create(S, err, errlen);
    free(S);
    if (!t) return false;
    S = (TrainSpec *)train_spec(t);
    if (!quiet) fprintf(stderr, "  mechanism: %s\n", train_summary(t));
    JsonValue *meta = json_object();
    json_set_string(meta, "domain", "mechanism");
    json_set_string(meta, "title", json_get_str(root, "title", "a gear train under load"));
    json_set_string(meta, "time_name", "input");
    json_set_string(meta, "time_unit", "deg");
    json_set_string(meta, "model",
                    "a gear train by finite elements: every gear elastic (hex8 with incompatible modes) on its exact generated outline (rings cut by a "
                    "shaper, helical gears as involute helicoids), frictionless contact on both flanks of every mesh, every shaft's rotation (and a carrier's) "
                    "a rigid degree of freedom; shafts, pins, bearings and carriers rigid");
    JsonValue *fields = json_set_object(meta, "fields");
    json_set_string(fields, "mises", "Pa");
    json_set_string(fields, "stress", "Pa");
    json_set(meta, "scenario", json_clone(root));
    char *hdr = json_dump(meta, 0, NULL, NULL);
    json_free(meta);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        train_free(t);
        return false;
    }
    JsonValue *diag = json_object();
    JsonValue *names = json_set_array(diag, "shafts"), *ratios = json_set_array(diag, "ratio"), *mnames = json_set_array(diag, "meshes");
    for (int s = 0; s < S->nshafts; s++) json_push(names, json_string(S->shaft[s].name)), json_push(ratios, json_number(train_ratio(t, s)));
    for (int k = 0; k < S->nmeshes; k++) json_push(mnames, json_stringf("%s - %s", S->gear[S->mesh[k].a].name, S->gear[S->mesh[k].b].name));
    JsonValue *a_in = json_set_array(diag, "input_deg"), *a_wu = json_set_array(diag, "windup_urad"), *a_tq = json_set_array(diag, "torque_nm");
    JsonValue *a_ti = json_set_array(diag, "input_torque_nm");
    JsonValue *a_ax = json_set_array(diag, "axial_n"), *a_mf = json_set_array(diag, "mesh_force_n"), *a_vm = json_set_array(diag, "max_mises_mpa");
    JsonValue *a_fl = json_set_array(diag, "float_um"), *a_ms = json_set_array(diag, "mesh_side_force_n"), *a_po = json_set_array(diag, "poses");
    JsonValue *a_of = json_set_array(diag, "offset_urad"), *a_en = json_set_array(diag, "energy_uj"), *a_se = json_set_array(diag, "settle_nm");
    /* pictures: a wedge cut out of the rings and carrier plates about the train's axis, so the gears inside show */
    const JsonValue *cut = json_get(json_get(root, "picture"), "cutaway_deg");
    bool cutaway = json_len(cut) == 2;
    double cut0 = cutaway ? json_at(cut, 0)->u.number * M_PI / 180 : 0, cut1 = cutaway ? json_at(cut, 1)->u.number * M_PI / 180 : 0;
    int *cconn = NULL;
    float *cfield = NULL;
    if (cutaway) {
        int cap = 0;
        for (int g = 0; g < train_gears(t); g++) {
            const char *nm;
            const double *xyz;
            const int *conn;
            const float *mi, *nmi;
            int nn, ne;
            train_gear(t, g, &nm, &xyz, &nn, &conn, &ne, &mi, &nmi);
            cap = ne > cap ? ne : cap;
        }
        for (int v = 0; v < train_visuals(t); v++) {
            const char *nm;
            const double *xyz;
            const int *conn;
            int nn, ne;
            train_visual(t, v, &nm, &xyz, &nn, &conn, &ne);
            cap = ne > cap ? ne : cap;
        }
        cconn = malloc((size_t)cap * 8 * sizeof(int)), cfield = malloc((size_t)cap * sizeof(float));
        if (!cconn || !cfield) {
            free(cconn), free(cfield), json_free(diag), lab_close(w), train_free(t);
            return snprintf(err, errlen, "out of memory"), false;
        }
    }
    bool ok = true;
    for (int f = 0; f < n * ntq && ok; f++) {
        int i = f % n;
        double deg = n > 1 ? a0 + (a1 - a0) * i / (n - 1) : a0;
        if (json_len(tq)) ((TrainSpec *)S)->shaft[S->input].torque_nm = json_at(tq, f / n)->u.number; /* the input's torque for this frame */
        TrainResult r;
        if (!train_solve(t, deg, &r, frames, err, errlen)) {
            ok = false;
            break;
        }
        json_push(a_in, json_number(round(deg * 1e4) / 1e4));
        json_push(a_ti, json_number(S->shaft[S->input].torque_nm));
        JsonValue *wu = json_array(), *tq = json_array(), *ax = json_array(), *mf = json_array(), *fl = json_array(), *of = json_array();
        for (int s = 0; s < S->nshafts; s++) {
            json_push(wu, json_number(round(r.windup_rad[s] * 1e9) / 1e3));
            json_push(of, json_number(round(r.offset_rad[s] * 1e12) / 1e6));
            json_push(tq, json_number(round(r.torque_nm[s] * 1e5) / 1e5));
            json_push(ax, json_number(round(r.axial_n[s] * 1e3) / 1e3));
            if (S->shaft[s].floating) json_push(fl, json_stringf("%s: %.4f, %.4f", S->shaft[s].name, 1e6 * r.float_m[s][0], 1e6 * r.float_m[s][1]));
        }
        JsonValue *ms2 = json_array();
        for (int k = 0; k < S->nmeshes; k++) {
            json_push(mf, json_number(round(r.mesh_force_n[k] * 1e3) / 1e3));
            JsonValue *pr = json_array();
            json_push(pr, json_number(round(r.mesh_side_force_n[k][0] * 1e3) / 1e3)), json_push(pr, json_number(round(r.mesh_side_force_n[k][1] * 1e3) / 1e3));
            json_push(ms2, pr);
        }
        json_push(a_wu, wu), json_push(a_tq, tq), json_push(a_ax, ax), json_push(a_mf, mf), json_push(a_fl, fl), json_push(a_ms, ms2);
        json_push(a_po, json_number(r.poses)), json_push(a_of, of), json_push(a_en, json_number(r.energy_j * 1e6));
        json_push(a_se, json_number(round(r.settle_m * 1e12) / 1e3));
        json_push(a_vm, json_number(round(r.max_mises_pa / 1e5) / 10));
        if (frames) lab_frame_begin(w, ntq > 1 ? f : deg);
        for (int g = 0; g < train_gears(t) && frames; g++) {
            const char *name;
            const double *xyz;
            const int *conn;
            const float *mises, *nmises;
            int nn, ne;
            train_gear(t, g, &name, &xyz, &nn, &conn, &ne, &mises, &nmises);
            if (cutaway && S->model[S->gear[g].model].internal) {
                int m = cut_cells(xyz, conn, ne, mises, cut0, cut1, cconn, cfield);
                lab_part_cells(w, name, nn, xyz, m, LAB_HEX, cconn);
                lab_field(w, "mises", LAB_AT_CELL, m, cfield);
            } else {
                lab_part_cells(w, name, nn, xyz, ne, LAB_HEX, conn);
                lab_field(w, "mises", LAB_AT_CELL, ne, mises);
            }
            lab_field(w, "stress", LAB_AT_NODE, nn, nmises);
        }
        for (int v = 0; v < train_visuals(t) && frames; v++) {
            const char *name;
            const double *xyz;
            const int *conn;
            int nn, ne;
            train_visual(t, v, &name, &xyz, &nn, &conn, &ne);
            if (cutaway && strstr(name, "plate")) {
                int m = cut_cells(xyz, conn, ne, NULL, cut0, cut1, cconn, NULL);
                lab_part_cells(w, name, nn, xyz, m, LAB_HEX, cconn);
            } else {
                lab_part_cells(w, name, nn, xyz, ne, LAB_HEX, conn);
            }
        }
        if (frames && !lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", f);
            ok = false;
        }
        if (!quiet) {
            if (ntq > 1) fprintf(stderr, "  %.4g N m", S->shaft[S->input].torque_nm);
            fprintf(stderr, "  input %8.3f deg  contacts %d (%d closed, %d pivots; %d poses, search %.1f s, solve %.1f s)  wind-up %.2f urad  torques:", deg,
                    r.contacts, r.active, r.pivots, r.poses, r.search_seconds, r.closure_seconds + r.solve_seconds, 1e6 * r.windup_rad[S->input]);
            for (int s = 0; s < S->nshafts; s++) fprintf(stderr, " %s %.4f", S->shaft[s].name, r.torque_nm[s]);
            fprintf(stderr, " N m\n");
        }
    }
    if (ok) {
        json_set_string(diag, "omitted", "friction; the compliance of shafts, pins and carriers (bearings only where a support stiffness is given); dynamics");
        json_set_number(diag, "wall_s", round((now() - t0) * 10) / 10);
        size_t len;
        char *d = json_dump(diag, 0, &len, NULL);
        /* the whole record beside the result, where the run record's room is too small for it */
        char path[1024];
        snprintf(path, sizeof path, "%s.train.json", out);
        FILE *fp = d ? fopen(path, "w") : NULL;
        if (fp) fputs(d, fp), fputc('\n', fp), fclose(fp);
        if (d && len < sizeof info->diagnostics) snprintf(info->diagnostics, sizeof info->diagnostics, "%s", d);
        else snprintf(info->diagnostics, sizeof info->diagnostics, "{\"note\":\"too long for the run record: the whole record is %s\",\"wall_s\":%.1f}", path, now() - t0);
        free(d);
    }
    json_free(diag);
    free(cconn), free(cfield);
    info->frames = lab_frames_written(w);
    info->steps = (long)n * ntq;
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    train_free(t);
    return ok;
}

bool lab_run_mechanism(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    const char *model = json_get_str(root, "model", "gear_pair");
    if (!strcmp(model, "gear_tooth")) return run_gear_tooth(root, out, quiet, info, err, errlen);
    if (!strcmp(model, "planetary")) return run_planetary(root, out, quiet, info, err, errlen);
    if (!strcmp(model, "gear_train")) return run_gear_train(root, out, quiet, info, err, errlen);
    if (strcmp(model, "gear_pair")) return snprintf(err, errlen, "mechanism: the model \"%s\" is not known (gear_pair, gear_tooth, planetary, gear_train)", model), false;
    double t0 = now();
    GearPairSpec S;
    memset(&S, 0, sizeof S);
    const JsonValue *gears = json_get(root, "gears");
    if (json_len(gears) != 2) return snprintf(err, errlen, "mechanism: gears is a list of two, the pinion (driver) first"), false;
    for (int b = 0; b < 2; b++)
        if (!gear_from_json(json_at(gears, b), &S.gear[b], &S.E[b], &S.nu[b], err, errlen)) return false;
    const JsonValue *mesh = json_get(root, "mesh");
    mesh_from_json(mesh, &S.mesh[0]);
    S.mesh[1] = S.mesh[0];
    S.centre_distance_m = json_get_num(root, "centre_distance_m", 0);
    const JsonValue *load = json_get(root, "load");
    /* one torque, or a list: each position is solved at every torque (the parts stay factorised) */
    const JsonValue *tq = json_get(load, "pinion_torque_nm");
    double torques[16];
    int ntq = 0;
    if (tq && tq->type == JSON_NUMBER) torques[ntq++] = tq->u.number;
    for (size_t k = 0; tq && tq->type != JSON_NUMBER && k < json_len(tq) && ntq < 16; k++) {
        const JsonValue *v = json_at(tq, k);
        if (v && v->type == JSON_NUMBER) torques[ntq++] = v->u.number;
    }
    for (int k = 0; k < ntq; k++)
        if (!(torques[k] > 0)) ntq = 0;
    if (ntq == 0) return snprintf(err, errlen, "mechanism: load.pinion_torque_nm (> 0, or a list of them) is needed"), false;
    S.torque_nm = torques[0];
    S.hold_cut_faces = json_get_bool(root, "hold_sector_cuts", true);
    S.single_tooth = json_get_bool(root, "single_pinion_tooth", false);
    S.gap_cut_m = json_get_num(root, "candidate_gap_m", 0);
    const JsonValue *gl = json_get(root, "gauges");
    for (size_t k = 0; k < json_len(gl) && S.ngauges < 8; k++) {
        const JsonValue *g = json_at(gl, k);
        GearGauge *G = &S.gauge[S.ngauges++];
        snprintf(G->name, sizeof G->name, "%s", json_get_str(g, "name", "gauge"));
        G->body = json_get_int(g, "gear", 1) ? 1 : 0;
        G->tooth = (int)json_get_int(g, "tooth", 0);
        G->loaded_side = !strcmp(json_get_str(g, "side", "loaded"), "loaded");
        const char *where = json_get_str(g, "position", "30deg");
        G->radius_m = strcmp(where, "30deg") ? json_get_num(g, "radius_m", 0) : 0;
        G->z_m = json_get_num(g, "z_m", -1);
        G->length_m = json_get_num(g, "length_m", 0);
    }
    const JsonValue *run = json_get(root, "run");
    int npos = (int)json_get_int(run, "positions", 0);
    const JsonValue *rolls = json_get(run, "gauged_roll_deg"); /* a list: roll angles of the contact on the gauged tooth */
    if (!rolls && npos < 1) return snprintf(err, errlen, "mechanism: run.positions or run.gauged_roll_deg is needed"), false;
    if (!quiet) fprintf(stderr, "  mechanism: building and factorising both gears\n");
    GearPair *gp = gearpair_create(&S, err, errlen);
    if (!gp) return false;
    if (!quiet) fprintf(stderr, "  mechanism: %s\n", gearpair_summary(gp));
    /* positions: pinion turns, or the turns that put the contact on the gauged tooth at the asked roll angles */
    int n = rolls ? (int)json_len(rolls) : npos;
    double *turn = malloc((size_t)n * sizeof(double));
    for (int i = 0; i < n; i++) {
        if (rolls) {
            const JsonValue *rv = json_at(rolls, i);
            double roll = rv && rv->type == JSON_NUMBER ? rv->u.number : NAN;
            if (!gearpair_turn_for_roll(gp, roll, &turn[i], err, errlen)) {
                free(turn), gearpair_free(gp);
                return false;
            }
        } else {
            double a = json_get_num(run, "pinion_turn_from_deg", -10), b = json_get_num(run, "pinion_turn_to_deg", 10);
            turn[i] = n > 1 ? a + (b - a) * i / (n - 1) : a;
        }
    }
    /* the header */
    JsonValue *meta = json_object();
    json_set_string(meta, "domain", "mechanism");
    json_set_string(meta, "title", json_get_str(root, "title", "a gear pair in mesh"));
    json_set_string(meta, "time_name", ntq > 1 ? "solve" : "pinion turn"); /* several torques: frames are numbered */
    json_set_string(meta, "time_unit", ntq > 1 ? "" : "deg");
    json_set_string(meta, "model",
                    "loaded tooth contact by finite elements: hex8 with incompatible modes, both gears held at their bores, frictionless "
                    "contact on the exact involutes, solved exactly on condensed compliances");
    JsonValue *fields = json_set_object(meta, "fields");
    json_set_string(fields, "mises", "Pa");
    json_set_string(fields, "stress", "Pa");
    json_set_string(fields, "contact", "N");
    json_set(meta, "scenario", json_clone(root));
    char *hdr = json_dump(meta, 0, NULL, NULL);
    json_free(meta);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        free(turn), gearpair_free(gp);
        return false;
    }
    /* positions; the record keeps one array per quantity, so it stays short */
    JsonValue *diag = json_object();
    JsonValue *a_turn = json_set_array(diag, "pinion_turn_deg"), *a_roll = json_set_array(diag, "gauged_roll_deg");
    JsonValue *a_tq = ntq > 1 ? json_set_array(diag, "pinion_torque_nm") : NULL;
    JsonValue *a_te = json_set_array(diag, "te_elastic_um"), *a_vm = json_set_array(diag, "max_mises_mpa");
    JsonValue *a_g[8];
    for (int k = 0; k < S.ngauges; k++) {
        char key[64];
        snprintf(key, sizeof key, "gauge_%s_microstrain", S.gauge[k].name);
        a_g[k] = json_set_array(diag, key);
    }
    bool ok = true;
    double te_min = INFINITY, te_max = -INFINITY;
#define R4(v) (round((v) * 1e4) / 1e4)
    for (int fi = 0; fi < n * ntq && ok; fi++) {
        int i = fi / ntq;
        GearPairPosition p;
        gearpair_set_torque(gp, torques[fi % ntq]);
        if (a_tq) json_push(a_tq, json_number(torques[fi % ntq]));
        if (!gearpair_solve(gp, turn[i], &p, true, err, errlen)) {
            ok = false;
            break;
        }
        te_min = fmin(te_min, p.te_elastic_um), te_max = fmax(te_max, p.te_elastic_um);
        json_push(a_turn, json_number(R4(p.roll_deg)));
        json_push(a_roll, p.gauged_roll_deg == p.gauged_roll_deg ? json_number(round(p.gauged_roll_deg * 1e3) / 1e3) : json_null());
        json_push(a_te, json_number(R4(p.te_elastic_um)));
        json_push(a_vm, json_number(round(fmax(p.max_mises_pa[0], p.max_mises_pa[1]) / 1e5) / 10));
        for (int k = 0; k < S.ngauges; k++) json_push(a_g[k], json_number(round(1e7 * p.gauge_strain[k]) / 10));
        lab_frame_begin(w, ntq > 1 ? fi : p.roll_deg);
        for (int b = 0; b < 2; b++) {
            const double *xyz;
            const int *conn;
            const float *mises, *contact, *nmises;
            int nn, ne;
            gearpair_frame(gp, b, &xyz, &nn, &conn, &ne, &mises, &contact, &nmises);
            lab_part_cells(w, b ? "gear" : "pinion", nn, xyz, ne, LAB_HEX, conn);
            lab_field(w, "mises", LAB_AT_CELL, ne, mises);
            lab_field(w, "stress", LAB_AT_NODE, nn, nmises);
            lab_field(w, "contact", LAB_AT_NODE, nn, contact);
        }
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", i);
            ok = false;
        }
        if (!quiet)
            fprintf(stderr, "  position %2d/%d  turn %8.3f deg  roll %6.2f deg  torque %7.2f N m  TE %7.3f um  F %7.1f N  gauges %.0f %.0f ue\n", i + 1, n,
                    p.roll_deg, p.gauged_roll_deg, torques[fi % ntq], p.te_elastic_um, p.force_total_n, 1e6 * p.gauge_strain[0], 1e6 * p.gauge_strain[1]);
    }
#undef R4
    if (ok) {
        json_set_number(diag, "te_elastic_peak_to_peak_um", round((te_max - te_min) * 1e4) / 1e4);
        json_set_string(diag, "omitted", "friction; dynamics; shaft and bearing compliance (bores held rigidly)");
        json_set_number(diag, "wall_s", round((now() - t0) * 10) / 10);
        size_t len;
        char *d = json_dump(diag, 0, &len, NULL);
        if (d && len < sizeof info->diagnostics) snprintf(info->diagnostics, sizeof info->diagnostics, "%s", d);
        else snprintf(info->diagnostics, sizeof info->diagnostics, "{\"note\":\"too many positions for the run record; the frames hold them\",\"wall_s\":%.1f}", now() - t0);
        free(d);
        if (!quiet) fprintf(stderr, "  mechanism: %s\n", gearpair_summary(gp));
    }
    json_free(diag);
    info->frames = lab_frames_written(w);
    info->steps = n * ntq;
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    free(turn);
    gearpair_free(gp);
    return ok;
}
