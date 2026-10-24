/* xr.c - the headset link (see xr.h and docs/xr.md) */
#include "xr.h"
#include "app.h"
#include "colormap.h"
#include "common.h"
#include "core/paths.h"
#include "console.h"
#include "fembridge.h"
#include "labapp.h"
#include "render.h"
#include "core/glb.h"
#include "core/json.h"
#include "core/png.h"
#include "core/xrlink.h"
#include "lab/labview.h"
#include "lab/labxr.h"

#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char xr_dir[600];
static unsigned xr_version; /* bumped each time scene.glb is rewritten */

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* the colour map as a 256 x 1 texture: texels 0 to 247 the map, 248 to 255 the neutral grey of the plate. Each vertex
 * carries its value as a texture coordinate, so every pixel is coloured from the map itself (crisp at any distance)
 * instead of colours blended between vertices. */
enum { CMAP_TEXELS = LABXR_TEXELS };
static unsigned char *colormap_png(int cmap, size_t *len) {
    unsigned char px[256 * 3];
    for (int i = 0; i < 256; i++) {
        float rgb[3] = {0.80f, 0.80f, 0.79f};
        if (i < CMAP_TEXELS) colormap_sample(cmap, i / (float)(CMAP_TEXELS - 1), rgb);
        for (int k = 0; k < 3; k++) px[3 * i + k] = (unsigned char)(rgb[k] * 255 + 0.5f);
    }
    unsigned char *out = NULL;
    return png_encode(px, 256, 1, 3, &out, len) ? out : NULL;
}

/* one vertex of the welded mesh: identical position, normal, palette coordinate and value are stored once */
typedef struct {
    float v[9]; /* position, normal, u, v, value */
} XrVert;
/* "no value" (the plate): a quiet NaN made from its bits, since the app builds with fast-math */
static float no_value(void) {
    uint32_t b = 0x7FC00000u;
    float f;
    memcpy(&f, &b, 4);
    return f;
}
static uint32_t vhash(const float *f) {
    uint32_t h = 2166136261u, w;
    for (int i = 0; i < 9; i++) {
        memcpy(&w, &f[i], 4);
        h = (h ^ w) * 16777619u;
    }
    return h;
}

bool xr_export_glb(const char *path, char *err, size_t cap) {
    const IsoMesh *m = fem_surface();
    float lo = 0, hi = 1;
    mat4 to_world, to_build;
    if (!m || !m->nverts || !m->nindices) {
        snprintf(err, cap, "no finite-element result is on screen");
        return false;
    }
    if (!fem_surface_range(&lo, &hi) || !fem_world_transform(&to_world, &to_build)) {
        snprintf(err, cap, "the result on screen has no range or no placement in the build");
        return false;
    }
    float s = to_world.m[0] > 0 ? to_world.m[0] : 1.0f; /* world units per metre of the part */
    uint32_t nv = m->nverts, ni = m->nindices;
    float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < nv; i++)
        for (int k = 0; k < 3; k++) {
            float x = m->verts[7 * i + k] / s;
            bmin[k] = fminf(bmin[k], x), bmax[k] = fmaxf(bmax[k], x);
        }
    /* the part rests on its lowest point, centred over the origin, so it can be set down on a table */
    float off[3] = {0.5f * (bmin[0] + bmax[0]), bmin[1], 0.5f * (bmin[2] + bmax[2])};
    float span = hi > lo ? hi - lo : 1.0f;
    size_t tcap = 1;
    while (tcap < 2 * (size_t)nv) tcap <<= 1;
    uint32_t *table = malloc(tcap * sizeof *table), *remap = malloc((size_t)nv * sizeof *remap);
    XrVert *out = malloc((size_t)nv * sizeof *out);
    uint32_t *idx = malloc((size_t)ni * sizeof *idx);
    float *buf = malloc((size_t)nv * 3 * sizeof *buf);
    size_t pnglen = 0;
    unsigned char *png = colormap_png(render_result_colormap(app.renderer), &pnglen);
    bool ok = table && remap && out && idx && buf && png;
    uint32_t nw = 0;
    if (ok) {
        memset(table, 0xFF, tcap * sizeof *table);
        for (uint32_t i = 0; i < nv; i++) {
            const float *v = m->verts + 7 * i;
            XrVert x;
            for (int k = 0; k < 3; k++) x.v[k] = v[k] / s - off[k];
            float nl = sqrtf(v[3] * v[3] + v[4] * v[4] + v[5] * v[5]);
            for (int k = 0; k < 3; k++) x.v[3 + k] = nl > 0 ? v[3 + k] / nl : (float)(k == 1);
            if (v[6] < -1e20f) x.v[6] = 252.0f / 256, x.v[8] = no_value(); /* the plate: neutral grey, no value */
            else {
                float t = (v[6] - lo) / span;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                x.v[6] = (0.5f + t * (CMAP_TEXELS - 1)) / 256; /* the centre of a texel of the map */
                x.v[8] = v[6];
            }
            x.v[7] = 0.5f;
            uint32_t h = vhash(x.v) & (uint32_t)(tcap - 1);
            while (table[h] != 0xFFFFFFFFu && memcmp(out[table[h]].v, x.v, sizeof x.v)) h = (h + 1) & (uint32_t)(tcap - 1);
            if (table[h] == 0xFFFFFFFFu) table[h] = nw, out[nw++] = x;
            remap[i] = table[h];
        }
        for (uint32_t i = 0; i < ni; i++) idx[i] = remap[m->indices[i]];
    }
    unsigned char *glb = NULL;
    size_t len = 0;
    Glb g;
    if (ok && glb_init(&g, "OpenPhysicsAI")) {
        int tex = glb_texture_png(&g, png, pnglen), mesh;
        JsonValue *mat = json_object(), *pbr = mat ? json_set_object(mat, "pbrMetallicRoughness") : NULL, *t = pbr ? json_set_object(pbr, "baseColorTexture") : NULL;
        if (t) json_set_int(t, "index", tex), json_set_number(pbr, "metallicFactor", 0), json_set_number(pbr, "roughnessFactor", 0.5);
        if (mat) json_set_string(mat, "name", "field"), json_set_bool(mat, "doubleSided", true);
        int material = glb_material(&g, mat);
        JsonValue *prims = glb_mesh(&g, "result", &mesh), *attr = glb_primitive(prims, GLB_TRIANGLES, glb_indices(&g, idx, ni), material);
        for (int a = 0; a < 4 && attr; a++) { /* position, normal, palette coordinate, value */
            static const int comps[4] = {3, 3, 2, 1}, first[4] = {0, 3, 6, 8};
            static const char *name[4] = {"POSITION", "NORMAL", "TEXCOORD_0", "_VALUE"};
            for (uint32_t i = 0; i < nw; i++) memcpy(buf + (size_t)comps[a] * i, out[i].v + first[a], (size_t)comps[a] * sizeof *buf);
            json_set_int(attr, name[a], glb_floats(&g, buf, nw, comps[a], a == 0));
        }
        JsonValue *pex = attr ? json_set_object(json_at(prims, 0), "extras") : NULL;
        if (pex) json_set_string(pex, "key", "field"), json_set_int(pex, "part", 0); /* one part: the headset names it */
        /* the facts of the result, and the way back from these metres to the part's own build coordinates, in mm */
        mat4 back = m4_mul(m4_scale(v3(1000, 1000, 1000)), m4_mul(to_build, m4_mul(m4_scale(v3(s, s, s)), m4_translate(v3(off[0], off[1], off[2])))));
        double to_source[16];
        for (int k = 0; k < 16; k++) to_source[k] = back.m[k];
        const FemState *fs = fem_state();
        int f = fem_field();
        JsonValue *ex = json_object(), *tk = ex ? json_set_object(ex, "result") : NULL;
        if (tk) {
            json_set_string(tk, "kind", "fem");
            json_set_string(tk, "field", fem_field_label(f)), json_set_string(tk, "field_name", fem_field_name(f));
            json_set_string(tk, "unit", fem_field_unit(f));
            json_set_number(tk, "lo", lo), json_set_number(tk, "hi", hi);
            json_set_int(tk, "step", fem_step() + 1), json_set_int(tk, "nsteps", fs->nsteps);
            json_set_number(tk, "deform", fem_deform_applied());
            JsonValue *pp = json_set_object(tk, "palette");
            if (pp) json_set_int(pp, "texture", tex), json_set_int(pp, "texels", CMAP_TEXELS), json_set_int(pp, "width", 256);
            json_set(tk, "to_source", json_numbers(to_source, 16));
            json_set_string(tk, "source_unit", "mm");
            JsonValue *parts = json_set_array(tk, "parts");
            if (parts) json_push(parts, json_string(fs->body[0] ? fs->body : "part"));
        }
        glb_node(&g, "result", mesh, ex);
        ok = glb_finish(&g, &glb, &len);
        glb_free(&g);
    } else ok = false;
    free(table), free(remap), free(out), free(idx), free(buf), free(png);
    if (!ok) {
        snprintf(err, cap, "out of memory");
        return false;
    }
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *fp = fopen(tmp, "wb");
    ok = fp && fwrite(glb, 1, len, fp) == len;
    if (fp) ok = (fclose(fp) == 0) && ok;
    free(glb);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        snprintf(err, cap, "cannot write %s", path);
        return false;
    }
    return true;
}

/* ---- what the headset reads: state.json ---------------------------------------------------------------------------- */

/* the AI's last answer, numbered, so the bridge speaks each one once in the headset: the agent's own words after the
 * last question or operation, or the error it stopped with */
static unsigned answer_id;
static char answer_text[2400];
static void track_answer(void) {
    static bool was_running;
    bool running = agent_running();
    if (was_running && !running) {
        int i = agent_line_count() - 1, kind = 0;
        const char *l = "";
        for (; i >= 0; i--) { /* skip the app's closing lines ("the agent finished", "run recorded") */
            l = agent_line_at(i, &kind);
            if (!(kind == 2 && strncmp(l, "asked: ", 7))) break;
        }
        answer_text[0] = 0;
        if (i >= 0 && kind == 3) str_copy(answer_text, sizeof answer_text, l);
        else {
            int end = i;
            while (i >= 0 && (agent_line_at(i, &kind), kind == 0)) i--;
            for (int k = i + 1; k <= end; k++) {
                size_t n = strlen(answer_text);
                snprintf(answer_text + n, sizeof answer_text - n, "%s%s", n ? "\n" : "", agent_line_at(k, &kind));
            }
        }
        if (answer_text[0]) answer_id++;
    }
    was_running = running;
}

static void colormap_hex(JsonValue *o, const char *key, int cmap, bool lab) {
    JsonValue *a = json_set_array(o, key);
    for (int i = 0; a && i < 9; i++) {
        float rgb[3];
        if (lab) labview_cmap_rgb(cmap, i / 8.0, rgb);
        else colormap_sample(cmap, i / 8.0f, rgb);
        json_push(a, json_stringf("#%02x%02x%02x", (int)(rgb[0] * 255 + 0.5f), (int)(rgb[1] * 255 + 0.5f), (int)(rgb[2] * 255 + 0.5f)));
    }
}

static uint32_t fnv(const char *s, uint32_t h) {
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

/* the simulations the AI started that are still computing (openphysics-mcp lab_run: agent-runs/NAME.pid and NAME.log in the
 * lab's results folder), each with how far along it is: the last "N/M" its solver printed (frame 3/40, step 1200/5000);
 * looked at once a second */
static JsonValue *computing(void) {
    static double at;
    static JsonValue *last;
    if (last && now_s() - at < 1.0) return json_clone(last);
    at = now_s();
    json_free(last);
    last = json_array();
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/agent-runs", labapp_results_dir());
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && json_len(last) < 4) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".pid")) continue;
        char p[1500], buf[2049];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        FILE *f = fopen(p, "r");
        long pid = 0;
        if (f) {
            if (fscanf(f, "%ld", &pid) != 1) pid = 0;
            fclose(f);
        }
        struct stat st;
        if (stat(p, &st) != 0 || !labapp_run_alive((pid_t)pid, st.st_mtime)) continue;
        double since = difftime(time(NULL), st.st_mtime);
        snprintf(p, sizeof p, "%s/%.*s.log", dir, (int)(n - 4), e->d_name);
        double frac = -1;
        if ((f = fopen(p, "r"))) { /* the end of its log: the last N/M in it */
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, size > 2048 ? size - 2048 : 0, SEEK_SET);
            size_t got = fread(buf, 1, sizeof buf - 1, f);
            buf[got] = 0;
            fclose(f);
            for (char *q = buf; *q; q++) {
                long a, b;
                if (*q >= '0' && *q <= '9' && (q == buf || q[-1] < '0' || q[-1] > '9') && sscanf(q, "%ld/%ld", &a, &b) == 2 && b > 0 && a >= 0 && a <= b)
                    frac = (double)a / (double)b;
            }
        }
        JsonValue *o = json_object();
        json_set(o, "name", json_stringn(e->d_name, n - 4));
        if (frac >= 0) json_set_number(o, "done", round(frac * 100) / 100);
        json_set_number(o, "seconds", since);
        json_push(last, o);
    }
    if (d) closedir(d);
    return json_clone(last);
}

/* the runs the AI started that ended in the last two minutes without their record (a value the solver refused after
 * the AI had answered, or a crash), each with why: the headset says so instead of leaving the person waiting. A run
 * the AI followed to its end with lab_run_status has no pid file left, and the AI said what happened itself. */
static JsonValue *stopped(void) {
    static double at;
    static JsonValue *last;
    if (last && now_s() - at < 1.0) return json_clone(last);
    at = now_s();
    json_free(last);
    last = json_array();
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/agent-runs", labapp_results_dir());
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && json_len(last) < 4) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".pid")) continue;
        char p[1500], buf[2049];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        FILE *f = fopen(p, "r");
        long pid = 0;
        if (f) {
            if (fscanf(f, "%ld", &pid) != 1) pid = 0;
            fclose(f);
        }
        struct stat st;
        if (pid <= 0 || (stat(p, &st) == 0 && labapp_run_alive((pid_t)pid, st.st_mtime))) continue;
        snprintf(p, sizeof p, "%s/%.*s.log", dir, (int)(n - 4), e->d_name);
        if (stat(p, &st) != 0 || difftime(time(NULL), st.st_mtime) > 120 || !(f = fopen(p, "r"))) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, size > 2048 ? size - 2048 : 0, SEEK_SET);
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        buf[got] = 0;
        fclose(f);
        while (got && (buf[got - 1] == '\n' || buf[got - 1] == '\r' || buf[got - 1] == ' ')) buf[--got] = 0;
        char *line = strrchr(buf, '\n');
        line = line ? line + 1 : buf;
        if (line[0] == '{') continue; /* labrun's record: it finished, and the app opens its result */
        JsonValue *o = json_object();
        json_set(o, "name", json_stringn(e->d_name, n - 4));
        json_set_int(o, "ended", (long long)st.st_mtime);
        if (!strncmp(line, "labrun: ", 8))
            json_set(o, "why", json_stringn(line + 8, strlen(line + 8) > 200 ? 200 : strlen(line + 8)));
        else json_set_string(o, "why", "it ended before its result was complete");
        json_push(last, o);
    }
    if (d) closedir(d);
    return json_clone(last);
}

static void write_state(bool has_fem, const char *note) {
    static char *last;
    static double written;
    const FemState *fs = fem_state();
    bool lab = labapp_active();
    char title[200];
    str_copy(title, sizeof title, lab ? labapp_title() : fs->project[0] ? fs->project : fs->body);
    for (char *c = title; *c; c++)
        if (*c == '_') *c = ' '; /* "Helical_Lantern" is Helical Lantern */
    JsonValue *st = json_object();
    if (!st) return;
    json_set_string(st, "app", "OpenPhysicsAI");
    json_set_string(st, "scene", lab ? "lab" : has_fem ? "fem" : "none");
    json_set_bool(st, "has_scene", lab || has_fem);
    json_set_int(st, "version", xr_version);
    json_set_string(st, "title", title);
    json_set_string(st, "note", note);
    if (has_fem && !lab) { /* the finite-element result: its field, range, stored times, section */
        JsonValue *fe = json_set_object(st, "fem"), *fields = fe ? json_set_array(fe, "fields") : NULL;
        float lo = 0, hi = 0;
        fem_surface_range(&lo, &hi);
        int f = fem_field();
        if (fe) {
            json_set_string(fe, "field", fem_field_label(f)), json_set_string(fe, "field_name", fem_field_name(f));
            json_set_string(fe, "unit", fem_field_unit(f));
            json_set_number(fe, "lo", lo), json_set_number(fe, "hi", hi);
            json_set_int(fe, "step", fem_step() + 1), json_set_int(fe, "nsteps", fem_step_count());
            json_set_number(fe, "time_s", fs->time_s);
            json_set_bool(fe, "playing", fem_playing());
            colormap_hex(fe, "colormap", render_result_colormap(app.renderer), false);
            JsonValue *sec = json_set_object(fe, "section");
            if (sec)
                json_set_bool(sec, "on", fem_section_on()), json_set_int(sec, "axis", fem_section_axis()),
                    json_set_number(sec, "fraction", fem_section_position()), json_set_bool(sec, "flip", fem_section_flipped());
        }
        for (int i = 0; fields && i < FEM_FIELD_COUNT; i++)
            if (fem_field_available(i)) {
                JsonValue *x = json_object();
                if (x) json_set_string(x, "name", fem_field_name(i)), json_set_string(x, "label", fem_field_label(i)), json_set_string(x, "unit", fem_field_unit(i));
                json_push(fields, x);
            }
    }
    if (lab) { /* the lab result: the file, the frame, and how the window shows it; the bridge has build/labxr draw it */
        JsonValue *lb = json_set_object(st, "lab");
        LabXrView v;
        if (lb && labapp_xr_view(&v)) {
            const char *path = labapp_path();
            struct stat sb;
            long long mtime = stat(path, &sb) == 0 ? (long long)sb.st_mtime : 0;
            int frame = labapp_frame();
            json_set_string(lb, "path", path);
            json_set_int(lb, "mtime", mtime);
            /* the result as opened: a run shown while it computes keeps its version as frames are added (they never
             * change once written), so the headset keeps the frames it has; a file opened again is a new version */
            json_set_int(lb, "version", (long long)labapp_open_serial());
            json_set_bool(lb, "growing", labapp_growing());
            json_set_int(lb, "frame", frame), json_set_int(lb, "nframes", labapp_nframes());
            json_set_bool(lb, "playing", labapp_playing()), json_set_number(lb, "fps", labapp_fps());
            json_set_number(lb, "time", labapp_frame_time(frame));
            json_set_string(lb, "time_unit", labapp_time_unit()[0] ? labapp_time_unit() : "s");
            json_set_string(lb, "domain", labapp_domain());
            json_set_bool(lb, "native3d", labapp_native3d());
            char names[16][48];
            int n = labapp_field_names(names, 16);
            JsonValue *fields = json_set_array(lb, "fields");
            for (int i = 0; fields && i < n; i++) json_push(fields, json_string(names[i]));
            JsonValue *view = labxr_view_json(&v);
            char *vt = view ? json_dump(view, JSON_SORTED, NULL, NULL) : NULL;
            char key[24];
            snprintf(key, sizeof key, "%08x", fnv(vt ? vt : "", fnv(path, 2166136261u) ^ (uint32_t)labapp_open_serial()));
            json_set_string(lb, "view_key", key); /* the bridge and the page draw frames again when it changes */
            json_set(lb, "view", view);
            colormap_hex(lb, "colormap", v.cmap == 200 ? 4 : v.cmap, true);
            free(vt);
        }
    }
    { /* the AI: what it is doing, its last lines, a permission prompt waiting, its last answer */
        JsonValue *ag = json_set_object(st, "agent"), *lines = ag ? json_set_array(ag, "lines") : NULL;
        if (ag) json_set_bool(ag, "running", agent_running());
        int count = agent_line_count(), idx[6], k = 0;
        for (int i = count - 1; i >= 0 && k < 6; i--) { /* the last six lines a person reads */
            int kind;
            const char *l = agent_line_at(i, &kind);
            if (kind == 2 && strncmp(l, "asked: ", 7)) continue;
            idx[k++] = i;
        }
        for (int j = k - 1; lines && j >= 0; j--) {
            int kind;
            char text[420];
            str_copy(text, sizeof text, agent_line_at(idx[j], &kind));
            if (kind == 2) { /* only the user's words, not the context the window or the headset adds */
                memmove(text, text + 7, strlen(text + 7) + 1);
                char *ctx = strstr(text, "\n\n(");
                if (ctx) *ctx = 0;
            }
            JsonValue *x = json_object();
            if (x) json_set_int(x, "kind", kind == 2 ? 4 : kind), json_set_string(x, "text", text);
            json_push(lines, x);
        }
        char tool[64], detail[600];
        if (ag && agent_approval_pending(tool, sizeof tool, detail, sizeof detail)) {
            JsonValue *ap = json_set_object(ag, "approval");
            if (ap) json_set_string(ap, "tool", tool), json_set_string(ap, "detail", detail);
        } else if (ag) json_set(ag, "approval", json_null());
        JsonValue *an = ag ? json_set_object(ag, "answer") : NULL;
        if (an) json_set_int(an, "id", answer_id), json_set_string(an, "text", answer_text);
        int turn = 0;
        bool done = true;
        const char *words = agent_turn_words(&turn, &done);
        if (ag && turn > 0) { /* the current answer's words as they come: the bridge speaks them sentence by sentence */
            JsonValue *say = json_set_object(ag, "say");
            if (say) json_set_int(say, "turn", turn), json_set_string(say, "text", words), json_set_bool(say, "done", done);
            char asked[1600]; /* the person's words of this turn, without the context added after them */
            str_copy(asked, sizeof asked, agent_turn_prompt());
            char *cut = strstr(asked, "\n\n(");
            if (cut) *cut = 0;
            if (say) json_set_string(say, "asked", asked);
            double first = agent_first_words_s();
            if (first >= 0) json_set_number(ag, "first_words_s", round(first * 100) / 100);
        }
        if (ag) {
            json_set(ag, "computing", computing());
            json_set(ag, "stopped", stopped());
            json_set_bool(ag, "busy", agent_running());
            json_set_string(ag, "session", agent_session_id());
            json_set_string(ag, "voice_model", hud_agent_voice_model());
        }
    }
    char *text = json_dump(st, 0, NULL, NULL);
    json_free(st);
    if (!text) return;
    if (last && !strcmp(text, last) && now_s() - written < 2.0) { /* unchanged: rewritten every 2 s, so the bridge sees the app */
        free(text);
        return;
    }
    written = now_s();
    free(last);
    last = text;
    char p[700], tmp[720];
    snprintf(p, sizeof p, "%s/state.json", xr_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *fp = fopen(tmp, "w");
    if (!fp) return;
    fputs(text, fp);
    fputc('\n', fp);
    if (fclose(fp) == 0) rename(tmp, p);
    else unlink(tmp);
}

/* ---- what the headset asks: the inbox -------------------------------------------------------------------------------- */

/* the oldest file of the inbox starting with prefix and ending in .txt; false when there is none */
static bool oldest(const char *prefix, char *name, size_t cap) {
    DIR *d = opendir(xr_dir);
    if (!d) return false;
    struct dirent *e;
    size_t pl = strlen(prefix);
    name[0] = 0;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (strncmp(e->d_name, prefix, pl) || n < pl + 5 || strcmp(e->d_name + n - 4, ".txt") || n >= cap) continue;
        if (!name[0] || strcmp(e->d_name, name) < 0) str_copy(name, cap, e->d_name);
    }
    closedir(d);
    return name[0] != 0;
}
static void take(const char *name, char *text, size_t cap) {
    char p[1000];
    snprintf(p, sizeof p, "%s/%s", xr_dir, name);
    text[0] = 0;
    FILE *f = fopen(p, "r");
    if (f) {
        size_t n = fread(text, 1, cap - 1, f);
        text[n] = 0;
        fclose(f);
    }
    unlink(p);
    size_t n = strlen(text);
    while (n && (text[n - 1] == '\n' || text[n - 1] == ' ')) text[--n] = 0;
}

/* an answer to a permission prompt (answer.txt) goes to the prompt waiting in the window; view commands (cmd-*.txt)
 * run at once, in the order they came; a question (ask-*.txt, oldest first) goes to Ask AI once the agent is free */
static void read_inbox(void) {
    char p[700], name[300], text[2000];
    snprintf(p, sizeof p, "%s/answer.txt", xr_dir);
    FILE *f = fopen(p, "r");
    if (f) {
        char a[16] = "";
        if (!fgets(a, sizeof a, f)) a[0] = 0;
        fclose(f);
        unlink(p);
        agent_approval_answer(!strncmp(a, "allow", 5));
    }
    for (int k = 0; k < 8 && oldest("cmd-", name, sizeof name); k++) {
        take(name, text, sizeof text);
        if (xrlink_command_allowed(text)) {
            LOGI("headset: %s", text);
            console_exec(text, false);
        } else LOGW("headset: refused '%.80s' (not a view command)", text);
    }
    snprintf(p, sizeof p, "%s/stop.txt", xr_dir); /* "stop", or the person talking over the answer */
    if (access(p, F_OK) == 0) {
        unlink(p);
        if (agent_running()) LOGI("headset: stop"), agent_stop();
    }
    if (!oldest("ask-", name, sizeof name)) return;
    if (agent_running()) { /* a question said over an answer: the answer stops, the question goes next */
        agent_stop();
        return;
    }
    take(name, text, sizeof text);
    char *pic = strstr(text, "\n[picture: ");
    if (pic) { /* what the person saw as they asked: a JPEG the bridge left in this folder, sent with the question */
        char path[700];
        snprintf(path, sizeof path, "%.*s", (int)strcspn(pic + 11, "]\n"), pic + 11);
        *pic = 0;
        size_t dl = strlen(xr_dir), pl = strlen(path);
        if (!strncmp(path, xr_dir, dl) && path[dl] == '/' && !strstr(path, "..") && pl > 4 && !strcmp(path + pl - 4, ".jpg"))
            agent_attach_picture(path);
    }
    if (text[0]) hud_agent_ask_voice(text);
}

/* the headset page asked the bridge in the last 5 s (page.json, written by tools/xr/xr_bridge.py) */
static bool page_connected(void) {
    char p[700];
    snprintf(p, sizeof p, "%s/page.json", xr_dir);
    JsonValue *j = json_read_file(p, 1 << 16, NULL);
    double seen = json_get_num(j, "seen", 0);
    json_free(j);
    return seen > 0 && difftime(time(NULL), (time_t)seen) <= 5;
}

void xr_tick(void) {
    static double next_look, last_export;
    static uint64_t exported_gen = (uint64_t)-1;
    static int exported_cmap = -2;
    static bool active, pending;
    if (app.headless && !op_getenv("XR")) return; /* a test run never writes into the owner's headset folder */
    double t = now_s();
    if (t >= next_look) { /* is the bridge there? once a second */
        next_look = t + 1.0;
        if (!xr_dir[0] && !xrlink_dir(xr_dir, sizeof xr_dir)) snprintf(xr_dir, sizeof xr_dir, "/tmp/openphysics-xr");
        struct stat sb;
        bool now_active = stat(xr_dir, &sb) == 0 && S_ISDIR(sb.st_mode);
        if (now_active && !active) exported_gen = (uint64_t)-1, LOGI("headset link: %s", xr_dir);
        active = now_active;
        static double next_warm;
        if (active && t >= next_warm && page_connected()) { /* someone is in the headset: the conversation is ready */
            next_warm = t + 10.0;
            hud_agent_warm();
        }
    }
    if (!active) return;
    /* the simulations the AI makes open in front of the person, whatever the window is showing (in the headset it
     * cannot be seen): a run shown while it computes, a finished one when it is written (labapp_follow) */
    static double followed_at;
    labapp_follow(followed_at > 0 ? t - followed_at : 0);
    followed_at = t;
    track_answer();
    uint64_t gen = fem_surface_generation();
    int cmap = render_result_colormap(app.renderer);
    bool has = fem_surface() != NULL;
    if (has && !labapp_active() && (gen != exported_gen || cmap != exported_cmap)) pending = true;
    if (pending && t - last_export >= 1.0) { /* at most once a second, so playback does not flood the folder */
        char p[700], err[200];
        snprintf(p, sizeof p, "%s/scene.glb", xr_dir);
        if (xr_export_glb(p, err, sizeof err)) xr_version++;
        exported_gen = gen, exported_cmap = cmap, pending = false, last_export = t;
    }
    static double next_state;
    if (t >= next_state) {
        next_state = t + (agent_running() ? 0.08 : 0.25); /* while the AI answers, its words reach the headset quickly */
        write_state(has, has || labapp_active() ? "" : "open a computed part or a lab result to see it here");
        read_inbox();
    }
}
