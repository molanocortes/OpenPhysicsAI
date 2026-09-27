/* bt_scenario.c - a battery module under load (pack.h, dfn.h), described in SI (docs/lab/battery.md): the cell and its
 * parameter set, the module's layout, the thermal properties and the cooling, the interconnects, the load, the run.
 *
 * Written per frame: each cell as a cylinder of hexahedra (24 around, 3 rings, layers up its height) and the cold plate
 * as a slab of them, carrying the temperature at their centres (K, interpolated from the thermal grid) and each cell's
 * current (A); the terminal voltage in the run record. The run record gives the capacity delivered, the time to the
 * cut-off voltage, the hottest cell and the spread of the currents, the coolant's outlet, and the energy balance. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "pack.h"

enum { NTH = 24, NRING = 3 };

bool lab_run_battery(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    const JsonValue *cell = json_get(root, "cell"), *mod = json_get(root, "module"), *th = json_get(root, "thermal"), *co = json_get(root, "cooling"),
                    *el = json_get(root, "electrical"), *ld = json_get(root, "load"), *run = json_get(root, "run");
    const JsonValue *jr = json_get(th, "jelly_roll"), *pl = json_get(th, "plate"), *gp = json_get(th, "gap");
    if (!cell || strcmp(json_get_str(cell, "parameters", ""), "lgm50_chen2020") || !mod || !jr || !pl || !gp || !co || !ld ||
        !json_get_str(jr, "source", NULL) || !json_get_str(pl, "source", NULL) || !json_get_str(gp, "source", NULL)) {
        snprintf(err, errlen, "battery: cell {parameters: \"lgm50_chen2020\"}, module, thermal {jelly_roll, plate, gap, each with its source}, "
                              "cooling and load are required");
        return false;
    }
    PackSpec s = {0};
    dfn_spec_lgm50(&s.cell, (int)json_get_int(cell, "volumes_per_region", 8), (int)json_get_int(cell, "shells", 8));
    s.rows = (int)json_get_int(mod, "rows", 0), s.cols = (int)json_get_int(mod, "cols", 0);
    s.pitch = json_get_num(mod, "pitch_m", 0), s.radius = json_get_num(mod, "radius_m", 0), s.height = json_get_num(mod, "height_m", 0);
    s.plate = json_get_num(mod, "plate_m", 0), s.margin = json_get_num(mod, "margin_m", 0), s.dx = json_get_num(mod, "grid_m", 0);
    s.kr = json_get_num(jr, "radial_w_mk", 0), s.kz = json_get_num(jr, "axial_w_mk", 0), s.rc_cell = json_get_num(jr, "heat_capacity_j_m3k", 0);
    s.k_plate = json_get_num(pl, "conductivity_w_mk", 0), s.rc_plate = json_get_num(pl, "heat_capacity_j_m3k", 0);
    s.k_gap = json_get_num(gp, "conductivity_w_mk", 0), s.rc_gap = json_get_num(gp, "heat_capacity_j_m3k", 0);
    s.h_cool = json_get_num(co, "plate_coefficient_w_m2k", 0), s.T_cool_in = json_get_num(co, "coolant_inlet_k", 298.15);
    s.mcp_cool = json_get_num(co, "coolant_capacity_rate_w_k", 0), s.h_amb = json_get_num(co, "air_coefficient_w_m2k", 0);
    s.T_amb = json_get_num(co, "air_k", 298.15), s.T0 = json_get_num(root, "initial_temperature_k", 298.15);
    s.R_ext = json_get_num(el, "interconnect_ohm", 0);
    Pack *P = pack_create(&s, err, errlen);
    if (!P) return false;
    const JsonValue *weak = json_get(el, "weak_cells");
    for (size_t i = 0; i < json_len(weak); i++) {
        const JsonValue *w = json_at(weak, i);
        if (!pack_set_resistance(P, (int)json_get_int(w, "cell", -1), json_get_num(w, "interconnect_ohm", -1))) {
            snprintf(err, errlen, "battery: weak_cells[%zu]: cell (0 to %d, row by row) and interconnect_ohm", i, pack_cells(P) - 1);
            pack_free(P);
            return false;
        }
    }
    const double I = json_get_num(ld, "current_a", 0), vcut = json_get_num(ld, "cutoff_v", 2.5);
    const double dt = json_get_num(run, "dt_s", 10), end = json_get_num(run, "end_s", 3600);
    const int frames = (int)json_get_int(run, "frames", 60), nk = pack_cells(P);
    /* the mesh: the cells' cylinders and the plate's slab */
    const int nz = (int)json_get_int(mod, "layers", 14), npl[2] = {s.cols * 6, s.rows * 6};
    const int nper = (1 + NRING * NTH) * (nz + 1), hper = NRING * NTH * nz;
    const int nn = nk * nper + (npl[0] + 1) * (npl[1] + 1) * 2, nh = nk * hper + npl[0] * npl[1];
    double *xyz = malloc((size_t)nn * 3 * 8), *cen = malloc((size_t)nh * 3 * 8);
    int *conn = malloc((size_t)nh * 8 * sizeof(int)), *owner = malloc((size_t)nh * sizeof(int));
    float *T = malloc((size_t)nh * sizeof(float)), *cur = malloc((size_t)nh * sizeof(float));
    if (!xyz || !cen || !conn || !owner || !T || !cur) {
        free(xyz), free(cen), free(conn), free(owner), free(T), free(cur), pack_free(P);
        snprintf(err, errlen, "battery: out of memory");
        return false;
    }
    int n0 = 0, h0 = 0;
    const double zb = pack_plate_top(P);
    for (int c = 0; c < nk; c++) {
        double cx, cy;
        pack_cell_xy(P, c, &cx, &cy);
        int base = n0;
        for (int l = 0; l <= nz; l++) {
            double z = zb + s.height * l / nz;
            double *p = xyz + 3 * (size_t)(base + l * (1 + NRING * NTH));
            p[0] = cx, p[1] = cy, p[2] = z;
            for (int r = 1; r <= NRING; r++)
                for (int a = 0; a < NTH; a++) {
                    double *q = p + 3 * (1 + (r - 1) * NTH + a), ang = 2 * M_PI * a / NTH, rr = s.radius * r / NRING;
                    q[0] = cx + rr * cos(ang), q[1] = cy + rr * sin(ang), q[2] = z;
                }
        }
        n0 += nper;
#define ND(l, r, a) (base + (l) * (1 + NRING * NTH) + ((r) == 0 ? 0 : 1 + ((r) - 1) * NTH + (((a) % NTH + NTH) % NTH)))
        for (int l = 0; l < nz; l++)
            for (int r = 0; r < NRING; r++)
                for (int a = 0; a < NTH; a++) {
                    int *e = conn + 8 * (size_t)h0;
                    e[0] = ND(l, r, a), e[1] = ND(l, r + 1, a), e[2] = ND(l, r + 1, a + 1), e[3] = ND(l, r, a + 1);
                    for (int m = 0; m < 4; m++) e[4 + m] = e[m] + (1 + NRING * NTH);
                    double ang = 2 * M_PI * (a + 0.5) / NTH, rr = s.radius * (r + 0.5) / NRING;
                    cen[3 * h0] = cx + rr * cos(ang), cen[3 * h0 + 1] = cy + rr * sin(ang), cen[3 * h0 + 2] = zb + s.height * (l + 0.5) / nz;
                    owner[h0++] = c;
                }
#undef ND
    }
    { /* the plate: one layer of hexahedra under the whole module */
        int n[3];
        double o[3], gdx;
        pack_grid(P, n, o, &gdx);
        double X0 = o[0], Y0 = o[1], X1 = o[0] + n[0] * gdx, Y1 = o[1] + n[1] * gdx;
        int base = n0;
        for (int k = 0; k < 2; k++)
            for (int j = 0; j <= npl[1]; j++)
                for (int i = 0; i <= npl[0]; i++) {
                    double *p = xyz + 3 * (size_t)n0++;
                    p[0] = X0 + (X1 - X0) * i / npl[0], p[1] = Y0 + (Y1 - Y0) * j / npl[1], p[2] = k ? zb : 0;
                }
        const int sx = npl[0] + 1, sl = sx * (npl[1] + 1);
        for (int j = 0; j < npl[1]; j++)
            for (int i = 0; i < npl[0]; i++) {
                int *e = conn + 8 * (size_t)h0, b0 = base + j * sx + i;
                e[0] = b0, e[1] = b0 + 1, e[2] = b0 + sx + 1, e[3] = b0 + sx;
                for (int m = 0; m < 4; m++) e[4 + m] = e[m] + sl;
                cen[3 * h0] = X0 + (X1 - X0) * (i + 0.5) / npl[0], cen[3 * h0 + 1] = Y0 + (Y1 - Y0) * (j + 0.5) / npl[1], cen[3 * h0 + 2] = zb / 2;
                owner[h0++] = -1;
            }
    }
    JsonValue *meta = json_object();
    bool ok = meta && json_set_string(meta, "domain", "battery") && json_set_string(meta, "title", json_get_str(root, "title", "battery module")) &&
              json_set_string(meta, "model", "Doyle-Fuller-Newman cells in parallel, 3D conduction, a cooled plate") && json_set(meta, "scenario", json_clone(root));
    JsonValue *fields = ok ? json_set_object(meta, "fields") : NULL;
    ok = ok && fields && json_set_string(fields, "T", "K") && json_set_string(fields, "current", "A");
    char *hdr = ok ? json_dump(meta, 0, NULL, NULL) : NULL;
    json_free(meta);
    LabWriter *w = hdr ? lab_create(out, hdr, err, errlen) : NULL;
    free(hdr);
    ok = w != NULL;
    long steps = 0;
    double ah = 0, t_cut = -1, Tmax = 0;
    time_t w0 = time(NULL);
    for (int f = 0; f <= frames && ok; f++) {
        double tf = end * f / frames;
        while (ok && pack_time(P) < tf - 1e-9 && t_cut < 0) {
            if (!pack_step(P, I, fmin(dt, tf - pack_time(P)))) {
                snprintf(err, errlen, "battery: a cell's model failed at t = %.1f s", pack_time(P));
                ok = false;
                break;
            }
            steps++;
            ah = I * pack_time(P) / 3600;
            for (int c = 0; c < nk; c++) {
                double tx;
                pack_cell_state(P, c, NULL, NULL, &tx, NULL, NULL);
                Tmax = fmax(Tmax, tx);
            }
            if (pack_voltage(P) < vcut) t_cut = pack_time(P);
        }
        if (!ok) break;
        for (int h = 0; h < nh; h++) {
            T[h] = (float)pack_T_at(P, cen[3 * h], cen[3 * h + 1], cen[3 * h + 2]);
            double Ic = 0;
            if (owner[h] >= 0) pack_cell_state(P, owner[h], &Ic, NULL, NULL, NULL, NULL);
            cur[h] = (float)Ic;
        }
        lab_frame_begin(w, pack_time(P));
        lab_part_cells(w, "module", nn, xyz, nh, LAB_HEX, conn);
        lab_field(w, "T", LAB_AT_CELL, nh, T), lab_field(w, "current", LAB_AT_CELL, nh, cur);
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write a frame (disk full?)");
            ok = false;
        }
        if (!quiet && f % 10 == 0)
            fprintf(stderr, "  t %.0f s  %.4f V  %.3f Ah  hottest %.2f K  coolant out %.2f K  %ld steps  %.0f s\n", pack_time(P), pack_voltage(P), ah, Tmax,
                    pack_coolant_out(P), steps, difftime(time(NULL), w0));
        if (t_cut >= 0) break;
    }
    double made, stored, tc, ta, imin = 1e30, imax = -1e30;
    int hot = 0;
    double hotT = 0;
    pack_energy(P, &made, &stored, &tc, &ta);
    for (int c = 0; c < nk; c++) {
        double Ic, Tx;
        pack_cell_state(P, c, &Ic, NULL, &Tx, NULL, NULL);
        imin = fmin(imin, Ic), imax = fmax(imax, Ic);
        if (Tx > hotT) hotT = Tx, hot = c;
    }
    snprintf(info->diagnostics, sizeof info->diagnostics,
             "{\"cells\":%d,\"delivered_ah\":%.5g,\"cutoff_s\":%.5g,\"terminal_v\":%.5g,\"hottest_k\":%.5g,\"hottest_cell\":%d,\"current_spread_a\":[%.5g,%.5g],"
             "\"coolant_outlet_k\":%.5g,\"heat_made_j\":%.6g,\"heat_stored_j\":%.6g,\"heat_to_coolant_j\":%.6g,\"heat_to_air_j\":%.6g}",
             nk, ah, t_cut, pack_voltage(P), Tmax, hot, imin, imax, pack_coolant_out(P), made, stored, tc, ta);
    info->frames = w ? lab_frames_written(w) : 0;
    info->steps = steps;
    if (w && !lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    free(xyz), free(cen), free(conn), free(owner), free(T), free(cur);
    pack_free(P);
    return ok;
}
