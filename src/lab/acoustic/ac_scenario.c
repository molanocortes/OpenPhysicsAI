/* ac_scenario.c - an acoustic run described in JSON (docs/lab/acoustic.md has every key), and its run loop.
 *
 * Materials are given by their normal-incidence absorption coefficient (alpha: R = sqrt(1 - alpha), xi = (1 + R) /
 * (1 - R)) or directly by their normalised impedance; "rigid" is built in. The run writes frames and, per receiver, the
 * reverberation time T30 from the Schroeder decay of what it recorded. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "acoustic.h"
#include "speaker.h"

static int material_index(const AcSpec *s, const char *name) {
    if (!name) return -1;
    for (int m = 0; m < s->nmaterials; m++)
        if (!strcmp(s->material_name[m], name)) return m;
    return -1;
}

/* a loudspeaker in a wall, driven by a voltage burst: sin(2 pi f t) under a Hann window of the given cycles */
typedef struct Speaker {
    bool on;
    SpkDriver d;
    double volt, freq, cycles;
} Speaker;

static double burst(double t, void *ctx) {
    const Speaker *k = ctx;
    double T = k->cycles / k->freq;
    if (t < 0 || t > T) return 0;
    return k->volt * sin(2 * M_PI * k->freq * t) * 0.5 * (1 - cos(2 * M_PI * t / T));
}

typedef struct AcRun {
    Speaker spk;
    double end_time;
    int frames;
    bool volume;
    double slice[3]; /* z, y, x; < 0 leaves a slice out */
    double threshold;
    int stride;
} AcRun;

static bool parse(const JsonValue *root, AcSpec *s, AcRun *run, char *title, size_t tl, char *err, size_t errlen) {
    ac_spec_defaults(s);
    snprintf(title, tl, "%s", json_get_str(root, "title", "acoustic run"));
    if (!json_get_numbers(json_get(root, "room_m"), s->size, 3)) {
        snprintf(err, errlen, "room_m [Lx, Ly, Lz] is required");
        return false;
    }
    s->dx = json_get_num(root, "dx_m", -1);
    s->c = json_get_num(root, "speed_of_sound_m_s", 343.2);
    s->rho = json_get_num(root, "density_kg_m3", 1.204);
    if (!(s->dx > 0)) {
        snprintf(err, errlen, "dx_m > 0 is required");
        return false;
    }
    const JsonValue *mats = json_get(root, "materials");
    for (size_t i = 0; mats && i < json_len(mats); i++) {
        if (s->nmaterials == AC_MAX_MATERIALS) {
            snprintf(err, errlen, "at most %d materials", AC_MAX_MATERIALS - 1);
            return false;
        }
        const JsonValue *m = json_at(mats, i);
        const char *name = json_get_str(m, "name", NULL);
        if (!name) {
            snprintf(err, errlen, "materials[%zu].name is required", i);
            return false;
        }
        double xi;
        if (json_get(m, "absorption_normal")) {
            double a = json_get_num(m, "absorption_normal", -1);
            if (!(a >= 0 && a < 1)) {
                snprintf(err, errlen, "materials[%zu].absorption_normal lies in [0, 1)", i);
                return false;
            }
            double R = sqrt(1 - a);
            xi = (1 + R) / (1 - R);
        } else if (json_get(m, "impedance_rho_c")) {
            xi = json_get_num(m, "impedance_rho_c", -1);
        } else {
            snprintf(err, errlen, "materials[%zu]: absorption_normal or impedance_rho_c", i);
            return false;
        }
        int k = s->nmaterials++;
        s->xi[k] = xi;
        snprintf(s->material_name[k], sizeof s->material_name[k], "%s", name);
    }
    const JsonValue *walls = json_get(root, "walls");
    static const char *SIDES[6] = {"x_low", "x_high", "y_low", "y_high", "z_low", "z_high"};
    for (int d = 0; d < 6; d++) {
        const char *w = json_get_str(walls, SIDES[d], json_get_str(walls, "all", "rigid"));
        int m = material_index(s, w);
        if (m < 0) {
            snprintf(err, errlen, "walls.%s: unknown material %s", SIDES[d], w);
            return false;
        }
        s->wall_material[d] = m;
    }
    const JsonValue *boxes = json_get(root, "boxes");
    for (size_t i = 0; boxes && i < json_len(boxes); i++) {
        if (s->nboxes == AC_MAX_BOXES) {
            snprintf(err, errlen, "at most %d boxes", AC_MAX_BOXES);
            return false;
        }
        const JsonValue *b = json_at(boxes, i);
        double v[6];
        int m = material_index(s, json_get_str(b, "material", "rigid"));
        if (!json_get_numbers(json_get(b, "box_m"), v, 6) || m < 0) {
            snprintf(err, errlen, "boxes[%zu]: box_m [x0, y0, z0, x1, y1, z1] and a known material", i);
            return false;
        }
        AcBox *B = &s->boxes[s->nboxes++];
        for (int k = 0; k < 3; k++) B->lo[k] = v[k], B->hi[k] = v[3 + k];
        B->material = m;
    }
    const JsonValue *src = json_get(root, "sources");
    for (size_t i = 0; src && i < json_len(src); i++) {
        if (s->nsources == AC_MAX_SOURCES) break;
        const JsonValue *q = json_at(src, i);
        AcSource *S = &s->sources[s->nsources];
        if (!json_get_numbers(json_get(q, "position_m"), S->pos, 3)) {
            snprintf(err, errlen, "sources[%zu].position_m is required", i);
            return false;
        }
        S->amplitude = json_get_num(q, "amplitude_pa_at_1m", 1.0);
        S->width_s = json_get_num(q, "pulse_width_s", 2e-4);
        S->delay_s = json_get_num(q, "delay_s", 5 * S->width_s);
        s->nsources++;
    }
    const JsonValue *sp = json_get(root, "speaker");
    if (sp) { /* a driver by its Thiele-Small parameters, each from its maker's sheet */
        const JsonValue *d = json_get(sp, "driver"), *dr = json_get(sp, "drive");
        static const char *FACES[6] = {"x_low", "x_high", "y_low", "y_high", "z_low", "z_high"};
        const char *face = json_get_str(sp, "face", "z_low");
        s->piston_face = -1;
        for (int f = 0; f < 6; f++)
            if (!strcmp(face, FACES[f])) s->piston_face = f;
        SpkDriver *D = &run->spk.d;
        D->Re = json_get_num(d, "re_ohm", -1), D->Le = json_get_num(d, "le_h", -1), D->Bl = json_get_num(d, "bl_t_m", -1);
        D->Mms = json_get_num(d, "mms_kg", -1), D->Cms = json_get_num(d, "cms_m_n", -1), D->Sd = json_get_num(d, "sd_m2", -1);
        double fs = json_get_num(d, "fs_hz", -1), qms = json_get_num(d, "qms", -1);
        D->Rms = json_get_num(d, "rms_kg_s", fs > 0 && qms > 0 ? 2 * M_PI * fs * D->Mms / qms : -1);
        if (s->piston_face < 0 || !json_get_str(d, "source", NULL) || !(D->Re > 0 && D->Le > 0 && D->Bl > 0 && D->Mms > 0 && D->Cms > 0 &&
                                                                    D->Sd > 0 && D->Rms > 0) ||
            !json_get_numbers(json_get(sp, "centre_m"), s->piston_centre, 2)) {
            snprintf(err, errlen, "speaker: face, centre_m [u, v] in that face, and driver {re_ohm, le_h, bl_t_m, mms_kg, cms_m_n, sd_m2, "
                                  "rms_kg_s or fs_hz and qms, source}");
            return false;
        }
        s->piston = true, s->piston_radius = sqrt(D->Sd / M_PI);
        run->spk.on = true;
        run->spk.volt = json_get_num(dr, "voltage_peak_v", 4.0), run->spk.freq = json_get_num(dr, "frequency_hz", 200);
        run->spk.cycles = json_get_num(dr, "cycles", 6);
    }
    const JsonValue *rec = json_get(root, "receivers");
    for (size_t i = 0; rec && i < json_len(rec) && s->nreceivers < AC_MAX_RECEIVERS; i++)
        if (json_get_numbers(json_get(json_at(rec, i), "position_m"), s->receivers[s->nreceivers], 3)) s->nreceivers++;
    const JsonValue *r = json_get(root, "run");
    run->end_time = json_get_num(r, "end_time_s", -1);
    run->volume = json_get_bool(r, "volume", true);
    run->frames = (int)json_get_int(r, "frames", 40);
    run->slice[0] = json_get_num(r, "slice_z_m", -1);
    run->slice[1] = json_get_num(r, "slice_y_m", -1);
    run->slice[2] = json_get_num(r, "slice_x_m", -1);
    run->threshold = json_get_num(r, "wavefront_threshold_pa", 0);
    run->stride = (int)json_get_int(r, "wavefront_stride", 2);
    if (!(run->end_time > 0) || run->frames < 1) {
        snprintf(err, errlen, "run: end_time_s > 0 and frames >= 1 are required");
        return false;
    }
    return true;
}

/* T30 from the Schroeder backward integral of one receiver's record; -1 if the decay does not reach -35 dB */
static double t30(const double *p, long n, double dt) {
    double *e = malloc((size_t)(n ? n : 1) * sizeof(double));
    if (!e || n < 10) {
        free(e);
        return -1;
    }
    double acc = 0;
    for (long k = n - 1; k >= 0; k--) acc += p[k] * p[k], e[k] = acc;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    long cnt = 0;
    bool reached = false;
    for (long k = 0; k < n; k++) {
        double db = 10 * log10(e[k] / e[0]);
        if (db < -35) reached = true;
        if (db > -5 || db < -35) continue;
        double t = k * dt;
        sx += t, sy += db, sxx += t * t, sxy += t * db, cnt++;
    }
    free(e);
    if (!reached || cnt < 3) return -1;
    double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
    return -60 / slope;
}

bool lab_run_acoustic(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    static AcSpec s; /* large (the box list): not on the stack */
    AcRun run = {0};
    char title[160];
    if (!parse(root, &s, &run, title, sizeof title, err, errlen)) return false;
    Acoustic *a = ac_create(&s, err, errlen);
    if (!a) return false;
    char *hdr = ac_header_json(&s, title);
    LabWriter *w = lab_create(out, hdr, err, errlen);
    free(hdr);
    if (!w) {
        ac_free(a);
        return false;
    }
    bool ok = true;
    clock_t c0 = clock();
    SpkState cone = {0, 0, 0};
    double xmax = 0, imax = 0;
    for (int f = 0; f <= run.frames && ok; f++) {
        double target = run.end_time * f / run.frames;
        while (ac_time(a) < target - 0.5 * ac_dt(a)) {
            if (run.spk.on) { /* the cone's acceleration now drives this step; then the driver moves on by the same dt */
                double t = ac_time(a);
                ac_set_piston_acceleration(a, spk_acceleration(&run.spk.d, &cone));
                spk_step(&run.spk.d, &cone, t, ac_dt(a), burst, &run.spk);
                xmax = fmax(xmax, fabs(cone.x)), imax = fmax(imax, fabs(cone.i));
            }
            ac_step(a);
        }
        lab_frame_begin(w, ac_time(a));
        if (run.volume) ok = ac_write_volume(a, w);
        else ac_write_frame(a, w, run.slice[0], run.slice[1], run.slice[2], run.threshold, run.stride);
        if (run.spk.on) { /* the cone as a disk in its wall, pushed out by its excursion (drawn ten times larger) */
            enum { NS = 32 };
            double xyz[3 * (NS + 1)];
            int conn[3 * NS], ax = s.piston_face / 2, hi = s.piston_face % 2, u = ax == 0 ? 1 : 0, v = ax == 2 ? 1 : 2;
            double base = hi ? s.size[ax] : 0, out = (hi ? -1 : 1) * 10 * cone.x;
            for (int k = 0; k <= NS; k++) {
                double *X = &xyz[3 * k], ang = 2 * M_PI * k / NS, rr = k == NS ? 0 : s.piston_radius;
                X[ax] = base + (k == NS ? out : 0.3 * out), X[u] = s.piston_centre[0] + rr * cos(ang), X[v] = s.piston_centre[1] + rr * sin(ang);
                if (k < NS) conn[3 * k] = NS, conn[3 * k + 1] = k, conn[3 * k + 2] = (k + 1) % NS;
            }
            lab_part_cells(w, "speaker", NS + 1, xyz, NS, LAB_TRI, conn);
        }
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write frame %d (disk full?)", f);
            ok = false;
        }
        if (!quiet) fprintf(stderr, "  frame %3d/%d  t %.4g s  steps %ld  %.1f s cpu\n", f, run.frames, ac_time(a), ac_steps(a), (double)(clock() - c0) / CLOCKS_PER_SEC);
    }
    info->frames = lab_frames_written(w);
    info->steps = ac_steps(a);
    int n = snprintf(info->diagnostics, sizeof info->diagnostics, "{\"end_time_s\":%.9g,\"dt_s\":%.6g,\"receivers\":[", ac_time(a), ac_dt(a));
    for (int r = 0; r < s.nreceivers && n < (int)sizeof info->diagnostics - 80; r++) {
        long ns;
        const double *tr = ac_receiver_trace(a, r, &ns);
        double pk = 0;
        for (long k = 0; k < ns; k++) pk = fmax(pk, fabs(tr[k]));
        double T = t30(tr, ns, ac_dt(a));
        char t[32];
        if (T < 0) snprintf(t, sizeof t, "null"); /* the record did not decay by 35 dB */
        else snprintf(t, sizeof t, "%.4g", T);
        n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "%s{\"peak_pa\":%.6g,\"t30_s\":%s}", r ? "," : "", pk, t);
    }
    if (run.spk.on)
        n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "],\"cone_excursion_max_m\":%.5g,\"coil_current_max_a\":%.5g", xmax, imax);
    else n += snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "]");
    snprintf(info->diagnostics + n, sizeof info->diagnostics - (size_t)n, "}");
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    ac_free(a);
    return ok;
}
