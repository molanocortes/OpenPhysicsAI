/* mg_drive.c - a motor that spins up under its own torque against a load, then runs and heats (drive.h). The run
 * writes two stretches of frames into one result: the spin-up in real time, then the heating as a time lapse. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../labio.h"
#include "drive.h"
#include "magnet.h"
#include "mthermal.h"

bool mg_run_drive(const JsonValue *root, const MgSpec *s, Magnet *m, const int *phase, const double *sign, int pole_pairs, double Jpk,
                  double e0, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen);

void mg_rotor_rk4(double *theta, double *omega, double dt, const MgLoad *L, MgTorqueFn T, void *ctx) {
#define ACC(th, w) ((T(th, ctx) - L->torque * (w) / (fabs(w) + 1e-3) - L->fan * (w) * fabs(w)) / L->inertia)
    double t0 = *theta, w0 = *omega;
    double k1t = w0, k1w = ACC(t0, w0);
    double k2t = w0 + 0.5 * dt * k1w, k2w = ACC(t0 + 0.5 * dt * k1t, k2t);
    double k3t = w0 + 0.5 * dt * k2w, k3w = ACC(t0 + 0.5 * dt * k2t, k3t);
    double k4t = w0 + dt * k3w, k4w = ACC(t0 + dt * k3t, k4t);
#undef ACC
    *theta = t0 + dt / 6 * (k1t + 2 * k2t + 2 * k3t + k4t);
    *omega = w0 + dt / 6 * (k1w + 2 * k2w + 2 * k3w + k4w);
}

/* the torque table: N m over one period of the angle, linear between points */
typedef struct Table {
    int n;
    double period, *T;
} Table;

static double table_torque(double theta, void *ctx) {
    const Table *t = ctx;
    double u = fmod(theta, t->period);
    if (u < 0) u += t->period;
    double x = u / t->period * (t->n - 1);
    int i = (int)floor(x);
    if (i >= t->n - 1) i = t->n - 2;
    double f = x - i;
    return (1 - f) * t->T[i] + f * t->T[i + 1];
}

/* the currents locked to the rotor, then the field at that angle */
static bool solve_at(Magnet *m, const MgSpec *s, const int *phase, const double *sign, int pp, double Jpk, double e0, double deg) {
    double el = (e0 + pp * deg) * M_PI / 180;
    for (int k = 0; k < s->nregions; k++)
        if (phase[k] >= 0) mg_set_region_current(m, k, sign[k] * Jpk * cos(el - phase[k] * 2 * M_PI / 3));
    mg_set_rotor_angle(m, deg);
    return mg_solve(m) >= 0;
}

bool mg_run_drive(const JsonValue *root, const MgSpec *s, Magnet *m, const int *phase, const double *sign, int pp, double Jpk, double e0,
                  const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    const JsonValue *d = json_get(root, "drive"), *th = json_get(d, "thermal"), *ld = json_get(d, "load");
    const double Lstk = json_get_num(d, "stack_length_m", -1), kf = json_get_num(d, "fill_factor", -1);
    const double alpha = json_get_num(d, "copper_temperature_coefficient_1_k", -1);
    if (!(Lstk > 0) || !(kf > 0 && kf <= 1) || !(alpha >= 0) || !json_get_str(d, "source", NULL)) {
        snprintf(err, errlen, "drive: stack_length_m, fill_factor (0 to 1), copper_temperature_coefficient_1_k and a source are required");
        mg_free(m);
        return false;
    }
    /* each material's thermal properties, by name, each with its source (air is material 0) */
    const int nm = s->nmaterials;
    double kmat[MG_MAX_MATERIALS], cmat[MG_MAX_MATERIALS], rho[MG_MAX_MATERIALS], held_T[MG_MAX_MATERIALS];
    for (int k = 0; k < nm; k++) {
        const JsonValue *t = json_get(th, s->materials[k].name);
        if (!t || !json_get_str(t, "source", NULL) || !(json_get_num(t, "thermal_conductivity_w_mk", -1) > 0) ||
            !(json_get_num(t, "volumetric_heat_capacity_j_m3k", -1) > 0)) {
            snprintf(err, errlen, "drive.thermal.%s: thermal_conductivity_w_mk, volumetric_heat_capacity_j_m3k and a source are required",
                     s->materials[k].name);
            mg_free(m);
            return false;
        }
        kmat[k] = json_get_num(t, "thermal_conductivity_w_mk", 1), cmat[k] = json_get_num(t, "volumetric_heat_capacity_j_m3k", 1);
        rho[k] = json_get_num(t, "density_kg_m3", 0), held_T[k] = json_get_num(t, "held_temperature_k", -1);
    }
    const double T_amb = json_get_num(d, "initial_temperature_k", 293.15);
    /* the rotor's inertia from its own cells, unless given */
    const int nx = s->nx, ny = s->ny;
    const double dx = s->dx, cx = s->rotor_centre[0], cy = s->rotor_centre[1];
    double J = json_get_num(d, "rotor_inertia_kg_m2", 0);
    if (!(J > 0)) {
        J = 0;
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                double x = (i + 0.5) * dx - cx, y = (j + 0.5) * dx - cy, r2 = x * x + y * y;
                int mt = mg_material_at(m, i, j);
                if (r2 < 0.99 * pow(json_get_num(d, "rotor_radius_m", 0), 2)) J += rho[mt] * r2 * dx * dx;
            }
        J *= Lstk;
    }
    MgLoad load = {J, json_get_num(ld, "torque_n_m", 0), json_get_num(ld, "fan_n_m_s2", 0)};
    /* the torque table over one period of the torque (the currents follow the rotor) */
    const JsonValue *tt = json_get(d, "torque_table");
    Table tab = {(int)json_get_int(tt, "points", 31), json_get_num(tt, "period_deg", 360.0 / pp) * M_PI / 180, NULL};
    if (tab.n < 3) tab.n = 3;
    tab.T = malloc((size_t)tab.n * sizeof(double));
    double Tmean = 0;
    for (int k = 0; k < tab.n; k++) {
        double deg = tab.period * 180 / M_PI * k / (tab.n - 1);
        if (!solve_at(m, s, phase, sign, pp, Jpk, e0, deg)) {
            snprintf(err, errlen, "the field did not converge at %.2f degrees", deg);
            free(tab.T), mg_free(m);
            return false;
        }
        tab.T[k] = mg_torque(m) * Lstk;
        if (k < tab.n - 1) Tmean += tab.T[k] / (tab.n - 1);
        if (!quiet) fprintf(stderr, "  torque table %2d/%d: %.2f deg  %.4f N m\n", k + 1, tab.n, deg, tab.T[k]);
    }
    /* the thermal model on the same cells, the materials as painted at the start */
    size_t nc = (size_t)nx * ny;
    double *k = malloc(nc * sizeof(double)), *C = malloc(nc * sizeof(double)), *q = calloc(nc, sizeof(double));
    unsigned char *held = calloc(nc, 1), *wind = calloc(nc, 1);
    float *Tf = malloc(nc * sizeof(float));
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            size_t c = (size_t)j * nx + i;
            int mt = mg_material_at(m, i, j);
            k[c] = kmat[mt], C[c] = cmat[mt], held[c] = held_T[mt] > 0, wind[c] = s->materials[mt].conductivity > 0;
        }
    MThermal *T = mt_create(nx, ny, dx, k, C, held, T_amb, s->threads);
    double *Tc = mt_temperature(T);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++)
            if (held[(size_t)j * nx + i]) Tc[(size_t)j * nx + i] = held_T[mg_material_at(m, i, j)];
    char title[200], hdr[1600];
    snprintf(title, sizeof title, "%s", json_get_str(root, "title", "a motor runs and heats"));
    char *mh = mg_header_json(s, title);
    /* the magnet header with the drive's own fields and times added */
    size_t ml = strlen(mh);
    if (ml > 1 && mh[ml - 1] == '}') mh[ml - 1] = 0;
    const JsonValue *sp = json_get(d, "spin_up"), *hr = json_get(d, "heat_run");
    const double t_spin = json_get_num(sp, "duration_s", 0.3), t_heat = json_get_num(hr, "duration_s", 3600);
    const int f_spin = (int)json_get_int(sp, "frames", 40), f_heat = (int)json_get_int(hr, "frames", 60);
    const int sub = (int)fmax(1, json_get_int(hr, "steps_per_frame", 2));
    char *fl = strstr(mh, "\"fields\":{");
    if (fl) { /* the temperature joins the field list with its unit */
        char head[1200];
        snprintf(head, sizeof head, "%.*s\"fields\":{\"T\":\"K\",%s", (int)(fl - mh), mh, fl + 10);
        snprintf(hdr, sizeof hdr, "%s,\"drive\":true,\"time_lapse_from_s\":%.6g}", head, t_spin);
    } else snprintf(hdr, sizeof hdr, "%s,\"drive\":true,\"time_lapse_from_s\":%.6g}", mh, t_spin);
    free(mh);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    bool ok = w != NULL;
    double theta = 0, omega = 0, t = 0, t90 = -1;
    const double dtm = json_get_num(sp, "step_s", 2e-5);
    const double w_ss = load.fan > 0 && Tmean > load.torque ? sqrt((Tmean - load.torque) / load.fan) : NAN;
    double copper_W = 0, Tmax = T_amb, Trot = T_amb;
    for (int f = 0; ok && f < f_spin + f_heat; f++) {
        if (f < f_spin) { /* the spin-up in real time */
            double tf = t_spin * f / (f_spin - 1 > 0 ? f_spin - 1 : 1);
            while (t < tf - 1e-12) {
                double h = fmin(dtm, tf - t);
                mg_rotor_rk4(&theta, &omega, h, &load, table_torque, &tab);
                t += h;
                if (t90 < 0 && isfinite(w_ss) && omega >= 0.9 * w_ss) t90 = t;
            }
        } else { /* the heating, a time lapse at the speed reached; losses are the cycle's mean J^2 / (kf sigma(T)) */
            double dth = t_heat / f_heat / sub;
            for (int st = 0; st < sub; st++) {
                copper_W = 0;
                for (size_t c = 0; c < nc; c++) {
                    if (!wind[c]) continue;
                    int mt = mg_material_at(m, (int)(c % nx), (int)(c / nx));
                    double rho_e = (1 + alpha * (Tc[c] - 293.15)) / s->materials[mt].conductivity;
                    q[c] = 0.5 * Jpk * Jpk / kf * rho_e, copper_W += q[c] * dx * dx * Lstk;
                }
                if (mt_step(T, dth, q) < 0) {
                    snprintf(err, errlen, "the thermal solve did not converge");
                    ok = false;
                    break;
                }
                theta += omega * dth, t += dth;
            }
            if (!ok) break;
        }
        double deg = fmod(theta * 180 / M_PI, 360.0);
        if (!solve_at(m, s, phase, sign, pp, Jpk, e0, deg)) {
            snprintf(err, errlen, "the field did not converge at %.2f degrees", deg);
            ok = false;
            break;
        }
        Tmax = T_amb, Trot = 0;
        int nr = 0;
        for (size_t c = 0; c < nc; c++) {
            Tf[c] = (float)Tc[c], Tmax = fmax(Tmax, Tc[c]);
            double x = (c % nx + 0.5) * dx - cx, y = (c / nx + 0.5) * dx - cy;
            if (x * x + y * y < pow(json_get_num(d, "rotor_radius_m", 0), 2)) Trot += Tc[c], nr++;
        }
        Trot = nr ? Trot / nr : T_amb;
        lab_frame_begin(w, t);
        mg_write_frame(m, w);
        lab_field(w, "T", LAB_AT_CELL, nc, Tf);
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", f);
            ok = false;
        }
        if (!quiet)
            fprintf(stderr, "  t %9.4f s  %6.0f rpm  copper %.1f W  hottest %.2f K  rotor %.2f K\n", t, omega * 30 / M_PI, copper_W, Tmax, Trot);
    }
    snprintf(info->diagnostics, sizeof info->diagnostics,
             "{\"rotor_inertia_kg_m2\":%.5g,\"mean_torque_n_m\":%.5g,\"ripple_n_m\":%.5g,\"steady_speed_rpm\":%.5g,\"speed_reached_rpm\":%.5g,"
             "\"time_to_90_percent_s\":%.5g,\"copper_loss_w\":%.5g,\"hottest_k\":%.5g,\"rotor_mean_k\":%.5g}",
             J, Tmean, 0.0, w_ss * 30 / M_PI, omega * 30 / M_PI, t90, copper_W, Tmax, Trot);
    { /* the ripple, from the table */
        double lo = INFINITY, hi = -INFINITY;
        for (int i = 0; i < tab.n; i++) lo = fmin(lo, tab.T[i]), hi = fmax(hi, tab.T[i]);
        char *p = strstr(info->diagnostics, "\"ripple_n_m\":");
        if (p) {
            char rest[1024];
            snprintf(rest, sizeof rest, "%s", strchr(p, ','));
            snprintf(p, sizeof info->diagnostics - (size_t)(p - info->diagnostics), "\"ripple_n_m\":%.5g%s", hi - lo, rest);
        }
    }
    info->frames = w ? lab_frames_written(w) : 0;
    info->steps = f_spin + f_heat;
    if (w && !lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    mt_free(T);
    free(k), free(C), free(q), free(held), free(wind), free(Tf), free(tab.T);
    mg_free(m);
    return ok;
}
