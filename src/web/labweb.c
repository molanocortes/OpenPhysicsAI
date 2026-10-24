/* labweb.c - the physics lab inside a web page: the same solvers as build/labrun and the same renderer as build/labfilm,
 * compiled to WebAssembly (wasm32-wasip1, a reactor) by tools/web_build.py. One thread (threads_serial.c), and the
 * native core's arithmetic: IEEE, no fast-math, no contraction.
 *
 * The page keeps its files in memory (site/lab/wasi.js). It writes a scenario to /lab/scenario.json and calls lw_run,
 * which leaves the result in /lab/result.lab and the JSON record build/labrun would print in lw_record. lw_open then
 * opens the result with the first look the app gives it (src/labapp.c, labapp_open), lw_view changes that look, and
 * lw_render draws a frame at any size and camera angle, as often as the viewer turns the view. Strings go in through
 * lw_buffer and come out as NUL-terminated text in the module's memory. */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/json.h"
#include "../core/sha256.h"
#include "../lab/lab_domains.h"
#include "../lab/labio.h"
#include "../lab/labscene.h"
#include "../lab/labview.h"
#include "../lab/style.h"

#define EXPORT(name) __attribute__((export_name(name)))

#define SCENARIO_PATH "/lab/scenario.json"
#define RESULT_PATH "/lab/result.lab"

static char record[4096];
static LabFile *result;
static LabFrame frame;
static int frame_index = -1, nframes;
static LabViewOpts opt;              /* the look: field, range, materials; the camera comes with each lw_render */
static char field[48], unit[32], solid[16], lines[48], view_name[16], domain[32], time_unit[16];
static double range_lo, range_hi;
static bool range_given;
static unsigned char *rgba, legend[3 * 256];
static size_t rgba_size;
static char *buffer, *state;
static size_t buffer_size;

static double wall(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

/* A buffer of at least n bytes for the page to write a string into (a scenario path, a view), valid until the next call. */
EXPORT("lw_buffer") char *lw_buffer(int n) {
    if (n < 1) n = 1;
    if ((size_t)n > buffer_size) {
        char *grown = realloc(buffer, (size_t)n);
        if (!grown) return NULL;
        buffer = grown;
        buffer_size = (size_t)n;
    }
    return buffer;
}

/* Run the scenario at SCENARIO_PATH into RESULT_PATH; 0 on success. quiet = 0 prints the solver's progress to stderr,
 * which the page shows as it comes. */
EXPORT("lw_run") int lw_run(int quiet) {
    JsonError je;
    JsonValue *root = json_read_file(SCENARIO_PATH, 16u << 20, &je);
    if (!root) {
        snprintf(record, sizeof record, "{\"error\":\"the scenario is not valid JSON (line %d)\"}", je.line);
        return 1;
    }
    size_t len;
    char *canon = json_dump(root, JSON_SORTED, &len, NULL);
    char hex[65] = {0};
    if (canon) sha256_hex_of(canon, len, hex);
    free(canon);
    char dom[32];
    snprintf(dom, sizeof dom, "%s", json_get_str(root, "domain", ""));
    LabRunInfo info = {0};
    char err[512] = {0};
    double t0 = wall();
    bool ok = lab_run_domain(dom, root, RESULT_PATH, quiet != 0, &info, err, sizeof err);
    double secs = wall() - t0;
    json_free(root);
    JsonValue *rec = json_object();
    if (!ok) {
        json_set_string(rec, "error", err[0] ? err : "the run failed");
    } else {
        json_set_string(rec, "scenario_sha256", hex);
        json_set_string(rec, "domain", dom);
        json_set_int(rec, "frames", info.frames);
        json_set_int(rec, "steps", info.steps);
        json_set_number(rec, "wall_s", round(secs * 100) / 100);
        JsonValue *d = info.diagnostics[0] ? json_parse(info.diagnostics, strlen(info.diagnostics), NULL, NULL) : NULL;
        json_set(rec, "diagnostics", d ? d : json_object());
    }
    char *text = json_dump(rec, 0, &len, NULL);
    snprintf(record, sizeof record, "%s", text && len < sizeof record ? text : "{\"error\":\"the record is too long\"}");
    free(text);
    json_free(rec);
    return ok ? 0 : 1;
}

/* The JSON record of the last run, or {"error": ...}. */
EXPORT("lw_record") const char *lw_record(void) { return record; }

static void compute_range(void) {
    if (range_given || !field[0]) return;
    LabViewOpts o = opt;
    o.first = 0, o.last = nframes - 1, o.every = nframes > 40 ? nframes / 20 : 1;
    labview_range(result, &o, &range_lo, &range_hi);
}

static void set_field(const char *name) {
    if (name != field) snprintf(field, sizeof field, "%s", name);
    opt.field = field;
    unit[0] = 0;
    JsonValue *h = json_parse(lab_header(result), strlen(lab_header(result)), NULL, NULL);
    snprintf(unit, sizeof unit, "%s", h ? json_get_str(json_get(h, "fields"), field, "") : "");
    json_free(h);
    opt.unit = unit;
    range_given = false;
    compute_range();
}

/* The first look, as the app gives it (src/labapp.c, labapp_open): the domain's own needs, the materials the header
 * names, the first field that varies, and the solid drawn as a body. */
static void first_look(void) {
    labview_defaults(&opt);
    opt.light = true; /* the light room, as on the rest of the site */
    opt.supersample = 2;
    opt.no_chrome = true;   /* the page shows the title and the time */
    opt.no_colorbar = true; /* and the legend */
    field[0] = unit[0] = solid[0] = lines[0] = view_name[0] = 0;
    range_given = false;
    JsonValue *h = json_parse(lab_header(result), strlen(lab_header(result)), NULL, NULL);
    snprintf(domain, sizeof domain, "%s", h ? json_get_str(h, "domain", "") : "");
    snprintf(time_unit, sizeof time_unit, "%s", h ? json_get_str(h, "time_unit", "") : "");
    if (!strcmp(domain, "impact")) opt.solid_points = true, opt.mesh = true;
    if (strcmp(domain, "water") && h && json_get_num(h, "particle_spacing_m", 0) > 0)
        opt.point_radius_m = 0.55 * json_get_num(h, "particle_spacing_m", 0);
    if (!strcmp(domain, "water")) opt.point_radius_m = 0.62 * json_get_num(h, "particle_spacing_m", 0);
    if (!strcmp(domain, "gas") && h && strstr(json_get_str(h, "geometry", ""), "axisymmetric")) opt.mesh_below = opt.mirror = true;
    if (!strcmp(domain, "acoustic")) opt.walls = true;
    opt.solid_volume = h && !strcmp(json_get_str(h, "volume_look", ""), "solid");
    if (h && !strcmp(json_get_str(h, "palette", ""), "glass")) opt.cmap = 103;
    if (h && !strcmp(json_get_str(h, "palette", ""), "fire")) opt.cmap = 104;
    const JsonValue *looks = h ? json_get(h, "looks") : NULL;
    for (size_t i = 0; looks && i < json_len(looks) && opt.nlooks < 12; i++) {
        const char *key = json_key_at(looks, i), *val = json_str(json_value_at(looks, i));
        int id = labscene_look_id(val);
        if (key && id) snprintf(opt.looks[opt.nlooks].part, sizeof opt.looks[0].part, "%s", key), opt.looks[opt.nlooks++].look = id;
    }
    if (!strcmp(domain, "orbit")) opt.labels = true, opt.true_size = true, opt.radius = 3;
    json_free(h);
    char err[256];
    LabFrame first, last;
    const char *pick = "";
    static char pickbuf[48];
    if (lab_read_frame(result, 0, &first, err, sizeof err)) {
        bool have_last = nframes > 1 && lab_read_frame(result, nframes - 1, &last, err, sizeof err);
        const LabFrame *scan = have_last ? &last : &first;
        for (int pass = 0; pass < 2 && !pick[0]; pass++)
            for (int p = 0; p < scan->nparts && !pick[0]; p++)
                for (int k = 0; k < scan->parts[p].nfields && !pick[0]; k++) {
                    const LabField *F = &scan->parts[p].fields[k];
                    if (!strcmp(F->name, "solid") || !strcmp(F->name, "material") || !strcmp(F->name, "body")) continue;
                    double lo = INFINITY, hi = -INFINITY;
                    for (size_t q = 0; q < F->count; q++)
                        if (isfinite(F->data[q])) lo = fmin(lo, F->data[q]), hi = fmax(hi, F->data[q]);
                    bool varies = hi > lo && (hi - lo) > 0.01 * fmax(fabs(hi), fabs(lo));
                    if (pass == 1 || varies) snprintf(pickbuf, sizeof pickbuf, "%s", F->name), pick = pickbuf;
                }
        if (have_last) lab_frame_free(&last);
        for (int p = 0; p < first.nparts; p++)
            if (lab_find_field(&first.parts[p], "solid")) snprintf(solid, sizeof solid, "solid"), opt.solid = solid;
            else if (lab_find_field(&first.parts[p], "material") && first.parts[p].kind == LAB_BLOCKS)
                snprintf(solid, sizeof solid, "material"), opt.solid = solid;
        lab_frame_free(&first);
    }
    set_field(pick);
}

/* Open RESULT_PATH for drawing, with its first look; returns the number of frames, or -1. */
EXPORT("lw_open") int lw_open(void) {
    if (frame_index >= 0) lab_frame_free(&frame);
    frame_index = -1;
    if (result) lab_close_file(result);
    char err[256];
    result = lab_open(RESULT_PATH, err, sizeof err);
    nframes = result ? lab_frame_count(result) : 0;
    if (!result || nframes < 1) {
        snprintf(record, sizeof record, "{\"error\":\"the result holds no frames\"}");
        return -1;
    }
    first_look();
    return nframes;
}

/* Change the look (JSON text, every key optional): field, range [lo, hi], mirror, mesh, mesh_below, solid, zoom,
 * center [x, y], view ("flat", "top"), labels, true_size, focus, span, lines, nlines, log, abs. 0 on success. */
EXPORT("lw_view") int lw_view(const char *json) {
    if (!result) return 1;
    JsonValue *v = json_parse(json, strlen(json), NULL, NULL);
    if (!v || v->type != JSON_OBJECT) return json_free(v), 1;
    if (json_get(v, "mirror")) opt.mirror = json_get_bool(v, "mirror", false);
    if (json_get(v, "mesh_below")) opt.mesh_below = json_get_bool(v, "mesh_below", false), opt.mirror |= opt.mesh_below;
    if (json_get(v, "mesh")) opt.mesh = json_get_bool(v, "mesh", false);
    if (json_get(v, "labels")) opt.labels = json_get_bool(v, "labels", false);
    if (json_get(v, "true_size")) opt.true_size = json_get_bool(v, "true_size", false);
    if (json_get(v, "log")) opt.logscale = json_get_bool(v, "log", false);
    if (json_get(v, "abs")) opt.absval = json_get_bool(v, "abs", false);
    if (json_get(v, "zoom") && json_get_num(v, "zoom", 1) > 0) opt.zoom = json_get_num(v, "zoom", 1);
    if (json_get(v, "focus")) opt.focus = (int)json_get_int(v, "focus", -1);
    if (json_get(v, "span")) opt.span = json_get_num(v, "span", 0);
    double c[2];
    if (json_get_numbers(json_get(v, "center"), c, 2)) opt.cx = c[0], opt.cy = c[1], opt.center_given = true;
    if (json_str(json_get(v, "solid"))) snprintf(solid, sizeof solid, "%s", json_get_str(v, "solid", "")), opt.solid = solid[0] ? solid : NULL;
    if (json_str(json_get(v, "view"))) snprintf(view_name, sizeof view_name, "%s", json_get_str(v, "view", "")), opt.view = view_name[0] ? view_name : NULL;
    if (json_str(json_get(v, "lines")))
        snprintf(lines, sizeof lines, "%s", json_get_str(v, "lines", "")), opt.lines = lines[0] ? lines : NULL,
        opt.nlines = (int)json_get_int(v, "nlines", 24);
    if (json_str(json_get(v, "field"))) set_field(json_get_str(v, "field", ""));
    double r[2];
    if (json_get_numbers(json_get(v, "range"), r, 2) && r[1] > r[0]) range_lo = r[0], range_hi = r[1], range_given = true;
    json_free(v);
    if (frame_index >= 0) lab_frame_free(&frame);
    frame_index = -1;
    return 0;
}

/* The result and its look as JSON text: title, domain, time unit, frames and their times, the fields with their
 * units, and the field shown with its range. */
EXPORT("lw_state") const char *lw_state(void) {
    free(state);
    state = NULL;
    if (!result) return "{}";
    JsonValue *s = json_object(), *h = json_parse(lab_header(result), strlen(lab_header(result)), NULL, NULL);
    json_set_string(s, "title", h ? json_get_str(h, "title", "") : "");
    json_set_string(s, "domain", domain);
    json_set_string(s, "time_unit", time_unit);
    json_set_int(s, "frames", nframes);
    JsonValue *times = json_set_array(s, "times");
    for (int i = 0; i < nframes; i++) json_push(times, json_number(lab_frame_time(result, i)));
    JsonValue *fields = json_get(h, "fields");
    json_set(s, "fields", fields && fields->type == JSON_OBJECT ? json_clone(fields) : json_object());
    json_set_string(s, "field", field);
    json_set_string(s, "unit", unit);
    json_set_number(s, "lo", isfinite(range_lo) ? range_lo : 0);
    json_set_number(s, "hi", isfinite(range_hi) ? range_hi : 0);
    json_set_bool(s, "log", opt.logscale);
    json_free(h);
    size_t len;
    state = json_dump(s, 0, &len, NULL);
    json_free(s);
    return state ? state : "{}";
}

/* The colours of the field's map, n samples from low to high (n <= 256), as RGB bytes: the page draws the legend. */
EXPORT("lw_legend") const unsigned char *lw_legend(int n) {
    if (n < 2) n = 2;
    if (n > 256) n = 256;
    int cmap = opt.cmap == STYLE_AUTO ? style_pick(range_lo, range_hi) : opt.cmap;
    for (int i = 0; i < n; i++) {
        float rgb[3];
        labview_cmap_rgb(cmap, i / (double)(n - 1), rgb);
        for (int k = 0; k < 3; k++) {
            float x = rgb[k] < 0 ? 0 : rgb[k] > 1 ? 1 : rgb[k];
            legend[3 * i + k] = (unsigned char)(x * 255 + 0.5f);
        }
    }
    return legend;
}

/* Draw frame i into a w x h RGBA image and return its address in memory (w*h*4 bytes), or 0. The camera looks from
 * azimuth azim and elevation elev (degrees), closer by the factor zoom than the look's own framing; fast = 1 skips
 * the anti-aliasing, for a view being turned. */
EXPORT("lw_render") unsigned char *lw_render(int i, int w, int h, double azim, double elev, double zoom, int fast) {
    if (!result || i < 0 || i >= nframes || w < 16 || h < 16 || w > 4096 || h > 4096) return NULL;
    char err[256];
    if (i != frame_index) {
        if (frame_index >= 0) lab_frame_free(&frame);
        frame_index = -1;
        if (!lab_read_frame(result, i, &frame, err, sizeof err)) return NULL;
        frame_index = i;
    }
    LabViewOpts o = opt;
    o.w = w, o.h = h;
    o.angles = true, o.azim = azim, o.elev = elev;
    if (zoom > 0) o.zoom = opt.zoom * zoom;
    o.supersample = fast ? 1 : 2;
    SwImage img;
    if (!labview_render(result, &frame, &o, range_lo, range_hi, &img)) return NULL;
    size_t need = (size_t)img.w * img.h * 4;
    if (need > rgba_size) {
        unsigned char *grown = realloc(rgba, need);
        if (!grown) {
            sw_image_free(&img);
            return NULL;
        }
        rgba = grown;
        rgba_size = need;
    }
    for (size_t p = 0, n = (size_t)img.w * img.h; p < n; p++) {
        rgba[4 * p] = img.rgb[3 * p];
        rgba[4 * p + 1] = img.rgb[3 * p + 1];
        rgba[4 * p + 2] = img.rgb[3 * p + 2];
        rgba[4 * p + 3] = 255;
    }
    sw_image_free(&img);
    return rgba;
}

/* The domains this build knows, comma separated. */
EXPORT("lw_domains") const char *lw_domains(void) { return lab_domain_list(); }
