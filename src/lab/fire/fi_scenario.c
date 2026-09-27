/* fi_scenario.c - fires and rooms (fire.h), described in SI (docs/lab/fire.md, docs/lab/rooms.md): the box and its
 * cells, its sides (open or walls, at a temperature or adiabatic), obstacles, vents, openings, heat sources and fans,
 * and optionally a burner with its fuel; the run.
 *
 * Written per frame: on the grid, the gas's temperature rise over the reference (dT, K; the ambient unless
 * "reference_temperature_k" is given), its speed and temperature, the age of the air in a room (s), the heat release rate where there is a burner, and the
 * obstacles as "material"; the burner, the vents and the openings as surfaces, the fans' boxes as solid cabinets (a rack is one). The run record gives, for a
 * fire, the averaged heat release and the flame height against Heskestad's correlation; for any room, the averaged heat
 * into the gas from each source, the air through the vents and openings, the mean and hottest temperatures. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/json.h"
#include "../../threads.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "fire.h"

static const char *SIDES[6] = {"x-", "x+", "y-", "y+", "z-", "z+"};
static int side_id(const char *s) {
    for (int i = 0; s && i < 6; i++)
        if (!strcmp(s, SIDES[i])) return i;
    return -1;
}

/* a rectangle on a side as a quad's four corners */
static void side_quad(const double box[6], int side, const double r[4], double q[12]) {
    int ax = side / 2, t0 = ax == 0 ? 1 : 0, t1 = ax == 2 ? 1 : 2;
    double at = (side & 1) ? box[ax + 3] : box[ax];
    const double a[4] = {r[0], r[2], r[2], r[0]}, b[4] = {r[1], r[1], r[3], r[3]};
    for (int c = 0; c < 4; c++) q[3 * c + ax] = at, q[3 * c + t0] = a[c], q[3 * c + t1] = b[c];
}
/* a box's six faces as quads (appended) */
static int box_quads(const double b[6], double *xyz, int *conn, int nq) {
    static const int F[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
    double *p = xyz + 3 * 4 * nq;
    int base = 4 * nq;
    for (int f = 0; f < 6; f++)
        for (int c = 0; c < 4; c++) {
            int v = F[f][c];
            p[3 * (4 * f + c) + 0] = (v & 1) ? b[3] : b[0], p[3 * (4 * f + c) + 1] = (v & 2) ? b[4] : b[1], p[3 * (4 * f + c) + 2] = (v & 4) ? b[5] : b[2];
            conn[base + 4 * f + c] = base + 4 * f + c;
        }
    return nq + 6;
}

bool lab_run_fire(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    const JsonValue *fu = json_get(root, "fuel"), *bu = json_get(root, "burner"), *run = json_get(root, "run");
    FireSpec s;
    fire_spec_defaults(&s);
    double box[6], b[4] = {0};
    s.dx = json_get_num(root, "cell_m", -1);
    if (!json_get_numbers(json_get(root, "box_m"), box, 6) || !(s.dx > 0)) {
        snprintf(err, errlen, "fire: box_m [x0, y0, z0, x1, y1, z1] and cell_m are required");
        return false;
    }
    if (bu) {
        if (!json_get_str(fu, "source", NULL) || !json_get_numbers(json_get(bu, "box_m"), b, 4) || !(json_get_num(bu, "heat_release_rate_w", -1) > 0)) {
            snprintf(err, errlen, "fire: a burner needs fuel {molar_mass_kg_mol, heat_of_combustion_j_kg, air_per_fuel_kg, radiative_fraction, source} "
                                  "and burner {box_m [x0, y0, x1, y1], heat_release_rate_w}");
            return false;
        }
        s.W_fuel = json_get_num(fu, "molar_mass_kg_mol", s.W_fuel), s.dHc = json_get_num(fu, "heat_of_combustion_j_kg", s.dHc);
        s.s_air = json_get_num(fu, "air_per_fuel_kg", s.s_air), s.chi_r = json_get_num(fu, "radiative_fraction", s.chi_r);
        memcpy(s.burner, b, sizeof b), s.hrr = json_get_num(bu, "heat_release_rate_w", 0), s.ramp = json_get_num(bu, "ramp_s", 1.0);
    } else
        s.hrr = 0;
    s.T0 = json_get_num(root, "ambient_temperature_k", s.T0);
    /* the sides: open (the default, with a free-slip floor), wall or slip; walls adiabatic unless given a temperature */
    const JsonValue *sd = json_get(root, "sides"), *wt = json_get(root, "wall_temperature_k");
    for (size_t i = 0; sd && i < json_len(sd); i++) {
        int id = side_id(json_key_at(sd, i));
        const char *k = json_str(json_value_at(sd, i));
        int kind = !k ? -1 : !strcmp(k, "open") ? FIRE_OPEN : !strcmp(k, "wall") ? FIRE_WALL : !strcmp(k, "slip") ? FIRE_SLIP : -1;
        if (id < 0 || kind < 0) {
            snprintf(err, errlen, "fire: sides {\"x-\" ... \"z+\": \"open\" | \"wall\" | \"slip\"}");
            return false;
        }
        s.side[id] = kind;
    }
    for (size_t i = 0; wt && i < json_len(wt); i++) {
        int id = side_id(json_key_at(wt, i));
        const JsonValue *tv = json_value_at(wt, i);
        double T = tv && tv->type == JSON_NUMBER ? tv->u.number : -1;
        if (id < 0 || !(T > 0) || s.side[id] == FIRE_OPEN) {
            snprintf(err, errlen, "fire: wall_temperature_k {side: K} for walls only");
            return false;
        }
        s.side_T[id] = T;
    }
    bool open_any = false;
    for (int i = 0; i < 6; i++) open_any = open_any || s.side[i] == FIRE_OPEN;
    s.sponge = (int)json_get_int(root, "sponge_cells", open_any ? 4 : 0);
    const char *tm = json_get_str(root, "turbulence", bu ? "smagorinsky" : "vreman");
    s.sgs = !strcmp(tm, "vreman") ? FIRE_VREMAN : !strcmp(tm, "none") ? FIRE_NO_SGS : FIRE_SMAGORINSKY;
    for (int a = 0; a < 3; a++) s.n[a] = (int)lround((box[a + 3] - box[a]) / s.dx), s.origin[a] = box[a];
    Fire *F = fire_create(&s, err, errlen);
    if (!F) return false;
    /* the room's contents */
    const JsonValue *obs = json_get(root, "obstacles"), *ven = json_get(root, "vents"), *opn = json_get(root, "openings"), *hs = json_get(root, "heat_sources"),
                    *fans = json_get(root, "fans");
    int nquads = 0, nobs = (int)json_len(obs), nven = (int)json_len(ven), nopn = (int)json_len(opn), nfan = (int)json_len(fans);
    double *vq = calloc((size_t)(nven + 1) * 12, 8), *oq = calloc((size_t)(nopn + 1) * 12, 8), *fq = calloc((size_t)(6 * nfan + 1) * 12, 8);
    int *fconn = calloc((size_t)(6 * nfan + 1) * 4, sizeof(int));
    bool ok = vq && oq && fq && fconn;
    for (int i = 0; ok && i < nobs; i++) {
        const JsonValue *o = json_at(obs, i);
        double bx[6];
        ok = json_get_numbers(json_get(o, "box_m"), bx, 6) && fire_add_obstacle(F, bx, json_get_num(o, "temperature_k", 0));
        if (!ok) snprintf(err, errlen, "fire: obstacles[%d]: box_m [x0, y0, z0, x1, y1, z1] inside the box, temperature_k >= 0 (0 adiabatic)", i);
    }
    for (int i = 0; ok && i < nven; i++) {
        const JsonValue *v = json_at(ven, i);
        double r[4];
        int side = side_id(json_get_str(v, "side", ""));
        ok = side >= 0 && s.side[side] != FIRE_OPEN && json_get_numbers(json_get(v, "rect_m"), r, 4) &&
             fire_add_vent(F, side, r, json_get_num(v, "speed_m_s", 0), json_get_num(v, "temperature_k", s.T0), 0);
        if (!ok) snprintf(err, errlen, "fire: vents[%d]: side (a wall), rect_m [a0, b0, a1, b1], speed_m_s (into the room; negative: an exhaust), temperature_k", i);
        else side_quad(box, side, r, vq + 12 * i);
    }
    for (int i = 0; ok && i < nopn; i++) {
        const JsonValue *v = json_at(opn, i);
        double r[4];
        int side = side_id(json_get_str(v, "side", ""));
        ok = side >= 0 && s.side[side] != FIRE_OPEN && json_get_numbers(json_get(v, "rect_m"), r, 4) && fire_add_opening(F, side, r);
        if (!ok) snprintf(err, errlen, "fire: openings[%d]: side (a wall), rect_m [a0, b0, a1, b1]", i);
        else side_quad(box, side, r, oq + 12 * i);
    }
    double q_sources = 0;
    for (size_t i = 0; ok && i < json_len(hs); i++) {
        const JsonValue *h = json_at(hs, i);
        double bx[6];
        ok = json_get_numbers(json_get(h, "box_m"), bx, 6) && json_get_num(h, "power_w", -1) >= 0 && fire_add_heat(F, bx, json_get_num(h, "power_w", 0));
        if (!ok) snprintf(err, errlen, "fire: heat_sources[%zu]: box_m [x0, y0, z0, x1, y1, z1], power_w", i);
        q_sources += json_get_num(h, "power_w", 0);
    }
    for (int i = 0; ok && i < nfan; i++) {
        const JsonValue *f = json_at(fans, i);
        double bx[6];
        const char *ax = json_get_str(f, "axis", "");
        int a = !strcmp(ax, "x") ? 0 : !strcmp(ax, "y") ? 1 : !strcmp(ax, "z") ? 2 : -1;
        ok = json_get_numbers(json_get(f, "box_m"), bx, 6) && fire_add_fan(F, bx, a, json_get_num(f, "speed_m_s", 0));
        if (!ok) snprintf(err, errlen, "fire: fans[%d]: box_m, axis \"x\" | \"y\" | \"z\", speed_m_s along it", i);
        else nquads = box_quads(bx, fq, fconn, nquads);
    }
    if (!ok) {
        free(vq), free(oq), free(fq), free(fconn), fire_free(F);
        return false;
    }
    const double Tref = json_get_num(root, "reference_temperature_k", s.T0);
    double end = json_get_num(run, "end_s", 10), avg_from = json_get_num(run, "average_from_s", end / 2);
    int frames = (int)json_get_int(run, "frames", 60);
    JsonValue *meta = json_object();
    bool mok = meta && json_set_string(meta, "domain", "fire") && json_set_string(meta, "title", json_get_str(root, "title", bu ? "fire" : "room")) &&
               json_set_string(meta, "model", bu ? "low-Mach reacting flow (large-eddy simulation, mixing-limited combustion), after FDS"
                                                 : "low-Mach buoyant flow (large-eddy simulation), after FDS") &&
               json_set(meta, "scenario", json_clone(root));
    if (mok && bu) mok = json_set_string(meta, "palette", "fire");
    JsonValue *fields = mok ? json_set_object(meta, "fields") : NULL, *looks = mok ? json_set_object(meta, "looks") : NULL;
    mok = mok && fields && looks;
    if (mok && bu) mok = json_set_string(fields, "T", "K") && json_set_string(fields, "hrr", "W/m^3") && json_set_string(looks, "burner", "steel");
    if (mok && !bu) mok = json_set_string(fields, "dT", "K") && json_set_string(fields, "T", "K") && json_set_string(fields, "age", "s");
    mok = mok && json_set_string(fields, "speed", "m/s") && json_set_string(looks, "vents", "steel") && json_set_string(looks, "openings", "glass") &&
          json_set_string(looks, "fans", json_get_str(root, "fan_look", "aluminium"));
    char *hdr = mok ? json_dump(meta, 0, NULL, NULL) : NULL;
    json_free(meta);
    LabWriter *w = hdr ? lab_create(out, hdr, err, errlen) : NULL;
    free(hdr);
    size_t n = fire_cells(F);
    float *T = malloc(n * sizeof(float)), *q = malloc(n * sizeof(float)), *sp = malloc(n * sizeof(float)), *dT = malloc(n * sizeof(float));
    float *mat = nobs ? malloc(n * sizeof(float)) : NULL, *age = malloc(n * sizeof(float));
    const int nz = s.n[2];
    double *qz = calloc((size_t)nz, 8), tavg = 0, hsum = 0, heat_avg[8] = {0};
    ThreadPool *pool = pool_create(cpu_perf_count());
    ok = w && T && q && sp && dT && qz && pool && (!nobs || mat) && age;
    if (mat)
        for (int k = 0; k < s.n[2]; k++)
            for (int j = 0; j < s.n[1]; j++)
                for (int i = 0; i < s.n[0]; i++) mat[(size_t)i + (size_t)s.n[0] * ((size_t)j + (size_t)s.n[1] * k)] = fire_solid(F, i, j, k) ? 1.0f : 0.0f;
    LabBlock B = {{s.n[0], s.n[1], s.n[2]}, 0, 0, {box[0], box[1], box[2]}, {s.dx, s.dx, s.dx}};
    double bq[12] = {b[0], b[1], box[2], b[2], b[1], box[2], b[2], b[3], box[2], b[0], b[3], box[2]};
    int c4[4] = {0, 1, 2, 3}, *vconn = calloc((size_t)(nven + nopn + 1) * 4, sizeof(int));
    for (int i = 0; vconn && i < 4 * (nven + nopn + 1); i++) vconn[i] = i;
    ok = ok && vconn;
    double f0[4] = {0}, f1[4];
    time_t w0 = time(NULL);
    long steps = 0;
    for (int f = 0; f <= frames && ok; f++) {
        double tf = end * f / frames;
        while (fire_time(F) < tf) {
            double dt = fire_step(F, pool);
            if (dt <= 0) {
                snprintf(err, errlen, "fire: the solver failed at t = %.4g s", fire_time(F));
                ok = false;
                break;
            }
            steps++;
            if (fire_time(F) > avg_from) {
                if (tavg == 0) fire_boundary_flows(F, f0);
                double hrr, hq[8];
                fire_diagnostics(F, NULL, &hrr, NULL, NULL, NULL);
                fire_heat_flows(F, hq);
                tavg += dt, hsum += hrr * dt;
                for (int k = 0; k < 8; k++) heat_avg[k] += hq[k] * dt;
                if (bu)
                    for (int k = 0; k < nz; k++) {
                        double qq = 0;
                        for (int j = 0; j < s.n[1]; j++)
                            for (int i = 0; i < s.n[0]; i++) qq += fire_q(F, i, j, k);
                        qz[k] += qq * dt;
                    }
            }
        }
        if (!ok) break;
        fire_fields(F, T, q, sp, NULL);
        for (size_t c = 0; c < n; c++) dT[c] = (float)(T[c] - Tref), age[c] = (float)fire_age(F)[c];
        lab_frame_begin(w, fire_time(F));
        lab_part_blocks(w, "gas", 1, &B);
        if (!bu) lab_field(w, "dT", LAB_AT_CELL, n, dT);
        lab_field(w, "T", LAB_AT_CELL, n, T);
        if (bu) lab_field(w, "hrr", LAB_AT_CELL, n, q);
        lab_field(w, "speed", LAB_AT_CELL, n, sp);
        if (!bu) lab_field(w, "age", LAB_AT_CELL, n, age); /* the age of the air: how long since it came in */
        if (mat) lab_field(w, "material", LAB_AT_CELL, n, mat);
        if (bu) lab_part_cells(w, "burner", 4, bq, 1, LAB_QUAD, c4);
        if (nven) lab_part_cells(w, "vents", 4 * nven, vq, nven, LAB_QUAD, vconn);
        if (nopn) lab_part_cells(w, "openings", 4 * nopn, oq, nopn, LAB_QUAD, vconn);
        if (nquads) lab_part_cells(w, "fans", 4 * nquads, fq, nquads, LAB_QUAD, fconn);
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write a frame (disk full?)");
            ok = false;
        }
        if (!quiet && f % 10 == 0) {
            double hrr, Tm = 0, Tx = 0;
            long nf = 0;
            fire_diagnostics(F, NULL, &hrr, NULL, NULL, NULL);
            for (size_t c = 0; c < n; c++)
                if (!mat || mat[c] == 0) Tm += T[c], nf++, Tx = fmax(Tx, T[c]);
            char hr[48] = "";
            if (bu) snprintf(hr, sizeof hr, ", heat release %.1f kW", hrr / 1e3);
            fprintf(stderr, "  t %.1f s  mean %.2f K, hottest %.1f K%s  %ld steps  %.0f s\n", fire_time(F), Tm / (nf ? nf : 1), Tx, hr, steps, difftime(time(NULL), w0));
        }
    }
    /* the run record */
    fire_boundary_flows(F, f1);
    double Tm = 0, Tx = 0;
    long nf = 0;
    for (size_t c = 0; ok && c < n; c++)
        if (!mat || mat[c] == 0) Tm += T[c], nf++, Tx = fmax(Tx, T[c]);
    char *d = info->diagnostics;
    size_t dl = sizeof info->diagnostics;
    int used = snprintf(d, dl, "{\"cells\":[%d,%d,%d],\"mean_temperature_k\":%.5g,\"hottest_k\":%.5g,\"background_pressure_pa\":%.7g", s.n[0], s.n[1], s.n[2],
                        Tm / (nf ? nf : 1), Tx, fire_pressure(F));
    if (tavg > 0 && used > 0 && (size_t)used < dl) {
        double ta = tavg, mi = (f1[0] - f0[0]) / ta, mo = (f1[2] - f0[2]) / ta;
        used += snprintf(d + used, dl - used,
                         ",\"averaged_from_s\":%.4g,\"inflow_kg_s\":%.5g,\"outflow_kg_s\":%.5g,\"inflow_temperature_k\":%.5g,\"outflow_temperature_k\":%.5g,"
                         "\"heat_from_walls_w\":%.5g,\"heat_from_obstacles_w\":%.5g,\"heat_sources_w\":%.5g",
                         avg_from, mi, mo, f1[0] > f0[0] ? (f1[1] - f0[1]) / (f1[0] - f0[0]) : 0, f1[2] > f0[2] ? (f1[3] - f0[3]) / (f1[2] - f0[2]) : 0,
                         (heat_avg[0] + heat_avg[1] + heat_avg[2] + heat_avg[3] + heat_avg[4] + heat_avg[5]) / ta, heat_avg[6] / ta, q_sources);
    }
    if (bu && used > 0 && (size_t)used < dl) {
        double tot = 0, cum = 0, L = 0;
        for (int k = 0; k < nz; k++) tot += qz[k];
        for (int k = 0; k < nz && tot > 0; k++) {
            if (cum + qz[k] >= 0.99 * tot) {
                L = (k + (0.99 * tot - cum) / qz[k]) * s.dx;
                break;
            }
            cum += qz[k];
        }
        double Q = s.hrr / 1e3, D = sqrt(4 * (b[2] - b[0]) * (b[3] - b[1]) / M_PI), Lh = 0.235 * pow(Q, 0.4) - 1.02 * D;
        used += snprintf(d + used, dl - used, ",\"averaged_heat_release_w\":%.6g,\"flame_height_m\":%.4g,\"heskestad_flame_height_m\":%.4g,\"mass_capped_kg\":%.4g",
                         tavg > 0 ? hsum / tavg : 0, L, Lh, fire_mass_capped(F));
    }
    if (used > 0 && (size_t)used + 2 < dl) snprintf(d + used, dl - used, "}");
    info->frames = w ? lab_frames_written(w) : 0;
    info->steps = steps;
    if (w && !lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    free(T), free(q), free(sp), free(dT), free(mat), free(age), free(qz), free(vq), free(oq), free(fq), free(fconn), free(vconn);
    pool_destroy(pool), fire_free(F);
    return ok;
}
