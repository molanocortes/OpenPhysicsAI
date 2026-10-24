/* induct3d.c - induction heating of a gear (eddy3d.h), described in SI (docs/lab/magnet.md, "Induction heating").
 *
 * A steel gear inside a ring coil carrying an alternating current: the eddy currents it induces crowd into the gear's
 * surface within the skin depth, most at the teeth, and their loss heats the steel, which conducts it inwards. One tooth
 * pitch is computed (the gear's rotational symmetry, a periodic sector of the cylindrical grid) and the result written
 * for every tooth. The coil is a current source of rectangular section with no loss of its own (water-cooled copper);
 * the gear's properties are those at the given temperature, constant through the run. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "eddy3d.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { MAXR = 512 };

static int split(double *out, int at, double a, double b, double cell) {
    int n = (int)ceil((b - a) / cell - 1e-9);
    if (n < 1) n = 1;
    for (int i = 1; i <= n; i++) out[at + i] = a + (b - a) * i / n;
    return at + n;
}

bool lab_run_induction(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    const JsonValue *in = json_get(root, "induction"), *gear = json_get(in, "gear"), *coil = json_get(in, "coil"), *steel = json_get(in, "steel");
    const JsonValue *grid = json_get(in, "grid"), *heat = json_get(in, "heat");
    int Z = (int)json_get_int(gear, "teeth", 0), spt = (int)json_get_int(grid, "sectors_per_tooth", 24);
    double Rb = json_get_num(gear, "bore_radius_m", -1), Rr = json_get_num(gear, "root_radius_m", -1), Rt = json_get_num(gear, "tip_radius_m", -1);
    double fr = json_get_num(gear, "tooth_fraction_at_root", 0.55), ft = json_get_num(gear, "tooth_fraction_at_tip", 0.35), th = json_get_num(gear, "thickness_m", -1);
    double ca = json_get_num(coil, "inner_radius_m", -1), cb = json_get_num(coil, "outer_radius_m", -1), ch = json_get_num(coil, "height_m", -1);
    double I = json_get_num(coil, "current_a", -1), f = json_get_num(coil, "frequency_hz", -1);
    double sig = json_get_num(steel, "conductivity_s_m", -1), mur = json_get_num(steel, "relative_permeability", 1), ks = json_get_num(steel, "thermal_conductivity_w_mk", -1);
    double cs = json_get_num(steel, "volumetric_heat_capacity_j_m3k", -1), Rout = json_get_num(grid, "outer_radius_m", -1), zext = json_get_num(grid, "axial_extent_m", -1);
    double hr = json_get_num(grid, "surface_cell_m", 0.0005), dur = json_get_num(heat, "duration_s", -1), Tamb = json_get_num(heat, "ambient_k", 293.15);
    int frames = (int)json_get_int(heat, "frames", 60), sub = (int)json_get_int(heat, "steps_per_frame", 5);
    if (Z < 3 || spt < 4 || !(Rb > 0 && Rr > Rb && Rt > Rr && ca > Rt && cb > ca && Rout > cb) || !(th > 0) || !(ch > 0) || !(I > 0) || !(f > 0) ||
        !(sig > 0) || !(mur > 0) || !(ks > 0) || !(cs > 0) || !(zext > th && zext > ch) || !(dur > 0) || frames < 1 || sub < 1 ||
        !json_get_str(steel, "source", NULL) || !json_get_str(coil, "source", NULL)) {
        snprintf(err, errlen, "induction: gear {teeth, bore/root/tip radius, thickness}, coil {radii, height, current_a, frequency_hz, source}, steel "
                              "{conductivity, permeability, conductivity and heat capacity, source}, grid {outer_radius_m, axial_extent_m}, heat "
                              "{duration_s, frames} are required, with the radii in order bore < root < tip < coil < outer");
        return false;
    }
    /* rings: coarse in the hub, fine through the teeth and just past them, then out to the coil and beyond */
    static double r[MAXR], z[MAXR];
    int nr = 0;
    r[0] = Rb;
    nr = split(r, nr, Rb, Rr - 3 * hr, fmax(4 * hr, (Rr - Rb) / 10));
    nr = split(r, nr, Rr - 3 * hr, Rt + 2 * hr, hr);
    nr = split(r, nr, Rt + 2 * hr, ca, fmax(2 * hr, (ca - Rt) / 6));
    nr = split(r, nr, ca, cb, (cb - ca) / 4);
    nr = split(r, nr, cb, Rout, (Rout - cb) / 5);
    int nz = 0;
    double hz = fmin(th, ch) / 10;
    z[0] = -zext / 2;
    nz = split(z, nz, -zext / 2, -fmax(th, ch) / 2, (zext - fmax(th, ch)) / 8);
    nz = split(z, nz, -fmax(th, ch) / 2, fmax(th, ch) / 2, hz);
    nz = split(z, nz, fmax(th, ch) / 2, zext / 2, (zext - fmax(th, ch)) / 8);
    Mag3DGrid g = {nr, spt, nz, r, z, 0.0, false, Z, true};
    size_t n = mag3d_count(&g);
    double *nu = malloc(n * sizeof *nu), *sg = calloc(n, sizeof *sg), *T = calloc(3 * n, sizeof *T), *q = malloc(n * sizeof *q);
    double *B = malloc(n * sizeof *B), *J = malloc(n * sizeof *J), *k = malloc(n * sizeof *k), *c = malloc(n * sizeof *c), *Te = malloc(n * sizeof *Te);
    unsigned char *kind = calloc(n, 1); /* 1 gear, 2 coil */
    if (!nu || !sg || !T || !q || !B || !J || !k || !c || !Te || !kind) {
        snprintf(err, errlen, "induction: out of memory");
        free(nu), free(sg), free(T), free(q), free(B), free(J), free(k), free(c), free(Te), free(kind);
        return false;
    }
    const double MU0 = 4e-7 * M_PI, pitch = 2 * M_PI / Z, Jc = I / ((cb - ca) * ch);
    for (int iz = 0; iz < nz; iz++)
        for (int it = 0; it < spt; it++)
            for (int ir = 0; ir < nr; ir++) {
                size_t e = mag3d_index(&g, ir, it, iz);
                double rc = 0.5 * (r[ir] + r[ir + 1]), zc = 0.5 * (z[iz] + z[iz + 1]), tc = pitch * (it + 0.5) / spt;
                bool in_z = fabs(zc) < th / 2, steelc = false;
                if (in_z && rc < Rr) steelc = true;
                else if (in_z && rc < Rt) { /* a tooth, centred in the pitch, narrowing from root to tip */
                    double u = (rc - Rr) / (Rt - Rr), half = 0.5 * pitch * (fr + (ft - fr) * u);
                    steelc = fabs(tc - 0.5 * pitch) < half;
                }
                nu[e] = 1 / (MU0 * (steelc ? mur : 1)), sg[e] = steelc ? sig : 0;
                k[e] = steelc ? ks : 0.0263, c[e] = steelc ? cs : 1.1614 * 1007;
                kind[e] = steelc ? 1 : 0;
                if (fabs(zc) < ch / 2 && rc < cb) { /* the coil: T_z = J (b - max(r, a)) makes an azimuthal current in [a, b] */
                    T[3 * e + 2] = Jc * (cb - fmax(rc, ca));
                    if (rc > ca) kind[e] = 2;
                }
            }
    Mag3DStats st;
    clock_t c0 = clock();
    if (!eddy3d_solve(&g, nu, sg, T, 2 * M_PI * f, q, B, J, &st, err, errlen)) {
        free(nu), free(sg), free(T), free(q), free(B), free(J), free(k), free(c), free(Te), free(kind);
        return false;
    }
    double P = 0;
    for (int iz = 0; iz < nz; iz++)
        for (int it = 0; it < spt; it++)
            for (int ir = 0; ir < nr; ir++) P += q[mag3d_index(&g, ir, it, iz)] * mag3d_volume(&g, ir, it, iz);
    P *= Z;
    if (!quiet) fprintf(stderr, "  eddy currents: %d unknowns, factor %.0f MB, %.1f s; %.4g W into the gear\n", st.dofs, st.factor_mb, st.assemble_s + st.solve_s, P);
    double dt = dur / frames / sub;
    Heat3DFem *H = heat3dfem_create(&g, k, c, Tamb, dt, err, errlen);
    if (!H) {
        free(nu), free(sg), free(T), free(q), free(B), free(J), free(k), free(c), free(Te), free(kind);
        return false;
    }
    JsonValue *meta = json_object();
    bool mok = meta && json_set_string(meta, "domain", "magnet") && json_set_string(meta, "title", json_get_str(root, "title", "induction heating")) &&
               json_set_string(meta, "model", "3D eddy currents (MFEM Nedelec, time-harmonic) and conduction (MFEM H1), one tooth by symmetry") &&
               json_set(meta, "scenario", json_clone(root));
    JsonValue *fields = mok ? json_set_object(meta, "fields") : NULL, *looks = mok ? json_set_object(meta, "looks") : NULL;
    mok = mok && fields && looks && json_set_string(fields, "T", "K") && json_set_string(fields, "q", "W/m^3") && json_set_string(fields, "J", "A/m^2") &&
          json_set_string(fields, "B", "T") && json_set_string(looks, "coil", "copper");
    char *hdr = mok ? json_dump(meta, 0, NULL, NULL) : NULL;
    json_free(meta);
    LabWriter *w = hdr ? lab_create(out, hdr, err, errlen) : NULL;
    free(hdr);
    /* the parts, every tooth: the computed sector turned by each pitch */
    size_t ncells[3] = {0, 0, 0};
    for (size_t e = 0; e < n; e++) ncells[kind[e]]++;
    size_t cap = (ncells[1] > ncells[2] ? ncells[1] : ncells[2]) * (size_t)Z;
    double *xyz = malloc(cap * 8 * 3 * sizeof *xyz), *fv[4];
    int *conn = malloc(cap * 8 * sizeof *conn);
    for (int m = 0; m < 4; m++) fv[m] = malloc(cap * sizeof(double));
    bool ok = w && xyz && conn && fv[0] && fv[1] && fv[2] && fv[3];
    double Tmax = Tamb, Ttip = Tamb, Troot = Tamb;
    for (int fr_ = 0; ok && fr_ <= frames; fr_++) {
        if (fr_ > 0)
            for (int s = 0; s < sub && ok; s++) ok = heat3dfem_step(H, q);
        heat3dfem_temperature(H, Te);
        lab_frame_begin(w, fr_ * dur / frames);
        for (int part = 1; part <= 2; part++) {
            size_t nc = 0;
            for (int tooth = 0; tooth < Z; tooth++) {
                double rot = tooth * pitch, cr = cos(rot), sr = sin(rot);
                for (int iz = 0; iz < nz; iz++)
                    for (int it = 0; it < spt; it++)
                        for (int ir = 0; ir < nr; ir++) {
                            size_t e = mag3d_index(&g, ir, it, iz);
                            if (kind[e] != part) continue;
                            static const int C[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
                            for (int v = 0; v < 8; v++) {
                                double t = pitch * (it + C[v][1]) / spt + rot, rr = r[ir + C[v][0]];
                                double *X = &xyz[3 * (8 * nc + v)];
                                X[0] = rr * cos(t), X[1] = rr * sin(t), X[2] = z[iz + C[v][2]];
                                conn[8 * nc + v] = (int)(8 * nc + v);
                            }
                            (void)cr, (void)sr;
                            fv[0][nc] = Te[e], fv[1][nc] = q[e], fv[2][nc] = J[e], fv[3][nc] = B[e];
                            nc++;
                        }
            }
            if (!nc) continue;
            lab_part_cells(w, part == 1 ? "gear" : "coil", (int)(8 * nc), xyz, (int)nc, LAB_HEX, conn);
            lab_field_d(w, "T", LAB_AT_CELL, nc, fv[0]), lab_field_d(w, "q", LAB_AT_CELL, nc, fv[1]);
            lab_field_d(w, "J", LAB_AT_CELL, nc, fv[2]), lab_field_d(w, "B", LAB_AT_CELL, nc, fv[3]);
        }
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write a frame (disk full?)");
            ok = false;
        }
        /* the tooth's tip and root at mid-thickness */
        for (int ir = 0; ir < nr; ir++) {
            size_t e = mag3d_index(&g, ir, spt / 2, nz / 2);
            double rc = 0.5 * (r[ir] + r[ir + 1]);
            if (kind[e] == 1 && rc < Rt && rc > Rt - 2 * hr) Ttip = Te[e];
            if (kind[e] == 1 && rc < Rr - 3 * hr && rc > Rr - 8 * hr) Troot = Te[e];
        }
        for (size_t e = 0; e < n; e++)
            if (kind[e] == 1) Tmax = fmax(Tmax, Te[e]);
        if (!quiet && fr_ % 10 == 0) fprintf(stderr, "  t %.3g s  peak %.0f K  tip %.0f K  below the root %.0f K  %.0f s cpu\n", fr_ * dur / frames, Tmax, Ttip, Troot,
                                             (double)(clock() - c0) / CLOCKS_PER_SEC);
    }
    snprintf(info->diagnostics, sizeof info->diagnostics,
             "{\"unknowns\":%d,\"factor_mb\":%.0f,\"power_w\":%.6g,\"skin_depth_m\":%.4g,\"peak_temperature_k\":%.5g,\"tooth_tip_k\":%.5g,"
             "\"below_root_k\":%.5g,\"elements_per_tooth\":%zu}",
             st.dofs, st.factor_mb, P, sqrt(2 / (2 * M_PI * f * MU0 * mur * sig)), Tmax, Ttip, Troot, n);
    info->frames = w ? lab_frames_written(w) : 0;
    info->steps = (long)frames * sub;
    if (w && !lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    heat3dfem_free(H);
    for (int m = 0; m < 4; m++) free(fv[m]);
    free(xyz), free(conn), free(nu), free(sg), free(T), free(q), free(B), free(J), free(k), free(c), free(Te), free(kind);
    return ok;
}
