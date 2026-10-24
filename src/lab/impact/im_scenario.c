/* im_scenario.c - an impact run described in JSON (docs/lab/impact.md has every key), and its run loop.
 *
 * Bodies are built from shapes (cylinder, box, sphere) with a named material from the library
 * (impact_materials.c); a scenario may override the erosion limits of a body's material, and must then say where
 * the limit comes from ("fail_strain_source"): a limit without a source is refused. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "impact.h"
#include "impact_materials.h"
#include "sph.h"

/* "method": "sph": the same bodies filled with particles of the given spacing (hypervelocity, fragmentation) */
static bool run_sph(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    static SphSpec s;
    sph_spec_defaults(&s);
    s.dx = json_get_num(root, "particle_spacing_m", -1);
    if (!(s.dx > 0)) {
        snprintf(err, errlen, "method sph needs particle_spacing_m");
        return false;
    }
    char title[160];
    snprintf(title, sizeof title, "%s", json_get_str(root, "title", "impact run"));
    const char *sym = json_get_str(root, "symmetry", "none");
    if (!strcmp(sym, "quarter") || !strcmp(sym, "half")) s.planes[s.nplanes++] = (SphPlane){0, 0.0, +1, false};
    if (!strcmp(sym, "quarter")) s.planes[s.nplanes++] = (SphPlane){1, 0.0, +1, false};
    if (json_get(root, "anvil_z_m")) s.planes[s.nplanes++] = (SphPlane){2, json_get_num(root, "anvil_z_m", 0), +1, true};
    const JsonValue *bodies = json_get(root, "bodies");
    if (!bodies || json_len(bodies) < 1 || json_len(bodies) > IM_MAX_MATERIALS) {
        snprintf(err, errlen, "bodies: one to %d", IM_MAX_MATERIALS);
        return false;
    }
    for (size_t i = 0; i < json_len(bodies); i++) {
        const ImMaterial *M = im_material_find(json_get_str(json_at(bodies, i), "material", ""));
        if (!M) {
            snprintf(err, errlen, "bodies[%zu]: unknown material (the library holds ofhc_copper, aluminium_6061_t6)", i);
            return false;
        }
        s.materials[s.nmaterials++] = *M;
    }
    Sph *p = sph_create(&s, err, errlen);
    if (!p) return false;
    for (size_t i = 0; i < json_len(bodies); i++) {
        const JsonValue *b = json_at(bodies, i);
        const char *shape = json_get_str(b, "shape", "");
        double v[3] = {0, 0, 0};
        json_get_numbers(json_get(b, "velocity_m_s"), v, 3);
        int got = -1;
        if (!strcmp(shape, "cylinder")) {
            double base[3] = {0, 0, json_get_num(b, "z0_m", 0)};
            got = sph_add_cylinder(p, base, json_get_num(b, "radius_m", 0), json_get_num(b, "length_m", 0), (int)i, v);
        } else if (!strcmp(shape, "box")) {
            double lo[3], hi[3];
            if (json_get_numbers(json_get(b, "lo_m"), lo, 3) && json_get_numbers(json_get(b, "hi_m"), hi, 3)) got = sph_add_box(p, lo, hi, (int)i, v);
        } else if (!strcmp(shape, "sphere")) {
            double c[3];
            if (json_get_numbers(json_get(b, "center_m"), c, 3)) got = sph_add_sphere(p, c, json_get_num(b, "radius_m", 0), (int)i, v);
        }
        if (got < 0) {
            snprintf(err, errlen, "bodies[%zu]: shape cylinder {radius_m, z0_m, length_m}, box {lo_m, hi_m} or sphere {center_m, radius_m}, "
                                  "large enough to hold particles of the spacing",
                     i);
            sph_free(p);
            return false;
        }
    }
    const JsonValue *run = json_get(root, "run");
    double end = json_get_num(run, "end_time_s", -1);
    int frames = (int)json_get_int(run, "frames", 40);
    bool mirror = json_get_bool(run, "mirror", true);
    if (!(end > 0) || frames < 1) {
        snprintf(err, errlen, "run: end_time_s > 0 and frames >= 1 are required");
        sph_free(p);
        return false;
    }
    char *hdr = sph_header_json(&s, title);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        sph_free(p);
        return false;
    }
    double k0, u0, k1, u1;
    sph_energy(p, &k0, &u0);
    bool ok = true;
    clock_t c0 = clock();
    for (int f = 0; f <= frames && ok; f++) {
        double target = end * f / frames;
        while (sph_time(p) < target && !sph_unstable(p)) sph_step(p);
        if (sph_unstable(p)) {
            snprintf(err, errlen, "the particles became unstable at step %ld", sph_steps(p));
            ok = false;
            break;
        }
        lab_frame_begin(w, sph_time(p));
        sph_write_frame(p, w, mirror);
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", f);
            ok = false;
        }
        if (!quiet) fprintf(stderr, "  frame %3d/%d  t %.4g s  steps %ld  particles %d  %.1f s cpu\n", f, frames, sph_time(p), sph_steps(p), sph_count(p),
                            (double)(clock() - c0) / CLOCKS_PER_SEC);
    }
    sph_energy(p, &k1, &u1);
    int n = snprintf(info->diagnostics, sizeof info->diagnostics, "{\"method\":\"sph\",\"particles\":%d,\"end_time_s\":%.9g,\"energy_balance\":%.5f,\"bodies\":[",
                     sph_count(p), sph_time(p), (k1 + u1 - k0 - u0) / (k0 + u0));
    for (int b = 0; b < sph_bodies(p); b++) {
        double mom[3];
        sph_momentum(p, b, mom);
        n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "%s{\"mass_kg\":%.6g,\"momentum_z\":%.6g}", b ? "," : "",
                      sph_body_mass(p, b), mom[2]);
    }
    snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "]}");
    info->frames = lab_frames_written(w);
    info->steps = sph_steps(p);
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    sph_free(p);
    return ok;
}

bool lab_run_impact(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    if (!strcmp(json_get_str(root, "method", "lagrange"), "sph")) return run_sph(root, out, quiet, info, err, errlen);
    static ImSpec s;
    im_spec_defaults(&s);
    char title[160];
    snprintf(title, sizeof title, "%s", json_get_str(root, "title", "impact run"));
    const char *sym = json_get_str(root, "symmetry", "none");
    if (!strcmp(sym, "quarter")) s.sym_x = s.sym_y = true;
    else if (!strcmp(sym, "half")) s.sym_x = true;
    else if (strcmp(sym, "none")) {
        snprintf(err, errlen, "symmetry is none, half (x = 0) or quarter (x = 0 and y = 0)");
        return false;
    }
    if (json_get(root, "anvil_z_m")) s.anvil = true, s.anvil_z = json_get_num(root, "anvil_z_m", 0);
    const JsonValue *bodies = json_get(root, "bodies");
    if (!bodies || json_len(bodies) < 1 || json_len(bodies) > 8) {
        snprintf(err, errlen, "bodies: one to eight");
        return false;
    }
    ImMesh m = {0};
    for (size_t i = 0; i < json_len(bodies); i++) {
        const JsonValue *b = json_at(bodies, i);
        const char *mat = json_get_str(b, "material", "");
        const ImMaterial *M = im_material_find(mat);
        if (!M) {
            snprintf(err, errlen, "bodies[%zu]: unknown material \"%s\" (the library holds ofhc_copper, aluminium_6061_t6)", i, mat);
            im_mesh_free(&m);
            return false;
        }
        ImMaterial mm = *M;
        if (json_get(b, "fail_strain")) {
            if (!json_get_str(b, "fail_strain_source", NULL)) {
                snprintf(err, errlen, "bodies[%zu]: fail_strain needs fail_strain_source (where the limit comes from)", i);
                im_mesh_free(&m);
                return false;
            }
            mm.fail_strain = json_get_num(b, "fail_strain", 0);
        }
        /* one material slot per body keeps overrides apart */
        int mi = s.nmaterials++;
        s.materials[mi] = mm;
        int n0 = m.nnodes, e0 = m.nelems;
        const char *shape = json_get_str(b, "shape", "");
        bool quarter = s.sym_x && s.sym_y;
        bool ok = false;
        if (!strcmp(shape, "cylinder")) {
            double c[3] = {0, 0, 0};
            json_get_numbers(json_get(b, "cells"), c, 3);
            ok = im_mesh_cylinder(&m, json_get_num(b, "radius_m", 0), json_get_num(b, "z0_m", 0), json_get_num(b, "length_m", 0), (int)c[0], (int)c[1],
                                  (int)c[2], quarter);
        } else if (!strcmp(shape, "box")) {
            double lo[3], hi[3], c[3];
            if (json_get_numbers(json_get(b, "lo_m"), lo, 3) && json_get_numbers(json_get(b, "hi_m"), hi, 3) && json_get_numbers(json_get(b, "cells"), c, 3)) {
                int n[3] = {(int)c[0], (int)c[1], (int)c[2]};
                ok = im_mesh_box(&m, lo, hi, n);
            }
        } else if (!strcmp(shape, "sphere")) {
            double c[3];
            if (json_get_numbers(json_get(b, "center_m"), c, 3))
                ok = im_mesh_sphere(&m, c, json_get_num(b, "radius_m", 0), (int)json_get_int(b, "cells", 6), quarter);
        }
        if (!ok || m.nnodes == n0) {
            snprintf(err, errlen, "bodies[%zu]: shape cylinder {radius_m, z0_m, length_m, cells [per quarter, rings, layers]}, box {lo_m, hi_m, cells}, or "
                                  "sphere {center_m, radius_m, cells}",
                     i);
            im_mesh_free(&m);
            return false;
        }
        ImBody *B = &s.bodies[s.nbodies++];
        B->first_node = n0, B->nnodes = m.nnodes - n0, B->first_elem = e0, B->nelems = m.nelems - e0, B->material = mi;
        json_get_numbers(json_get(b, "velocity_m_s"), B->velocity, 3);
    }
    s.mesh = m;
    const JsonValue *run = json_get(root, "run");
    double end = json_get_num(run, "end_time_s", -1);
    int frames = (int)json_get_int(run, "frames", 40);
    bool mirror = json_get_bool(run, "mirror", true);
    if (!(end > 0) || frames < 1) {
        snprintf(err, errlen, "run: end_time_s > 0 and frames >= 1 are required");
        im_mesh_free(&m);
        return false;
    }
    Impact *im = im_create(&s, err, errlen);
    if (!im) {
        im_mesh_free(&m);
        return false;
    }
    char *hdr = im_header_json(&s, title);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        im_free(im);
        im_mesh_free(&m);
        return false;
    }
    ImEnergy e0;
    im_energy(im, &e0);
    bool ok = true;
    clock_t c0 = clock();
    for (int f = 0; f <= frames && ok; f++) {
        double target = end * f / frames;
        while (im_time(im) < target) im_step(im);
        lab_frame_begin(w, im_time(im));
        im_write_frame(im, w, mirror);
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", f);
            ok = false;
        }
        if (!quiet) fprintf(stderr, "  frame %3d/%d  t %.4g s  steps %ld  eroded %d  %.1f s cpu\n", f, frames, im_time(im), im_steps(im), im_eroded_count(im),
                            (double)(clock() - c0) / CLOCKS_PER_SEC);
    }
    ImEnergy e1;
    im_energy(im, &e1);
    /* the extent of each body now: for a Taylor test, the final length and the largest diameter */
    const double *x = im_positions(im);
    int n = snprintf(info->diagnostics, sizeof info->diagnostics, "{\"end_time_s\":%.9g,\"energy_balance\":%.5f,\"eroded\":%d,\"bodies\":[", im_time(im),
                     (e1.total - e0.total) / e0.total, im_eroded_count(im));
    for (int b = 0; b < s.nbodies; b++) {
        double zlo = INFINITY, zhi = -INFINITY, rmax = 0;
        for (int k = s.bodies[b].first_node; k < s.bodies[b].first_node + s.bodies[b].nnodes; k++) {
            zlo = fmin(zlo, x[3 * k + 2]), zhi = fmax(zhi, x[3 * k + 2]);
            rmax = fmax(rmax, hypot(x[3 * k], x[3 * k + 1]));
        }
        double p[3];
        im_body_momentum(im, b, p);
        n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "%s{\"z_min_m\":%.6g,\"z_max_m\":%.6g,\"r_max_m\":%.6g,\"momentum_z\":%.6g}",
                      b ? "," : "", zlo, zhi, rmax, p[2]);
    }
    snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "]}");
    info->frames = lab_frames_written(w);
    info->steps = im_steps(im);
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    im_free(im);
    im_mesh_free(&m);
    return ok;
}
