/* labxr.c - a physics-lab result for the headset (src/lab/labxr.h, docs/xr.md).
 *
 *   build/labxr RESULT.lab --info                      title, domain, frames and times, fields and units, the box
 *   build/labxr RESULT.lab --frame N -o OUT.glb        frame N (negative: from the end) as the headset's scene;
 *                                                      -o - writes it to standard output
 *   build/labxr RESULT.lab --print-view                the view the app takes for the result (JSON), and nothing else
 *     --view JSON | --view-file FILE   how the window shows it (the "view" the app writes into the headset's state.json)
 *     --field NAME  --range LO HI      without a view: the field and its colour range (default: the first field that
 *                                      varies, its range over the frames as the app takes it)
 *
 * Without --view the look is taken from the result's header as the app's first look at it (labapp_open): the palette
 * it asks for, a solid volume, vortex surfaces, material looks, a solid's particles and element edges, a room's walls,
 * the surface of water. The headset link (tools/xr/xr_bridge.py) always passes the app's own view. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/core/json.h"
#include "../src/lab/labio.h"
#include "../src/lab/labscene.h"
#include "../src/lab/labview.h"
#include "../src/lab/labxr.h"
#include "../src/lab/style.h"

static int fail(const char *msg) {
    fprintf(stderr, "labxr: %s\n", msg);
    return 1;
}

/* the first field of the last frame that varies, as the app picks it */
static void first_field(LabFile *lf, char *out, size_t cap) {
    LabFrame fr;
    char err[128];
    out[0] = 0;
    if (!lab_read_frame(lf, lab_frame_count(lf) - 1, &fr, err, sizeof err)) return;
    for (int pass = 0; pass < 2 && !out[0]; pass++)
        for (int p = 0; p < fr.nparts && !out[0]; p++)
            for (int k = 0; k < fr.parts[p].nfields && !out[0]; k++) {
                const LabField *F = &fr.parts[p].fields[k];
                if (!strcmp(F->name, "solid") || !strcmp(F->name, "material") || !strcmp(F->name, "body")) continue;
                double lo = 1e300, hi = -1e300;
                for (size_t q = 0; q < F->count; q++)
                    if (F->data[q] > -1e30f && F->data[q] < 1e30f) lo = fmin(lo, F->data[q]), hi = fmax(hi, F->data[q]);
                if (pass == 1 || (hi > lo && hi - lo > 0.01 * fmax(fabs(hi), fabs(lo)))) snprintf(out, cap, "%s", F->name);
            }
    lab_frame_free(&fr);
}

/* the header's look, as labapp_open takes it */
static void header_view(LabFile *lf, LabXrView *v) {
    JsonValue *h = json_parse(lab_header(lf), strlen(lab_header(lf)), NULL, NULL);
    const char *domain = json_get_str(h, "domain", "");
    double spacing = json_get_num(h, "particle_spacing_m", 0);
    if (!strcmp(domain, "impact")) v->solid_points = v->mesh = true;
    if (strcmp(domain, "water") && spacing > 0) v->point_radius_m = 0.55 * spacing;
    if (!strcmp(domain, "water")) v->point_radius_m = 0.62 * spacing, v->water_spacing = spacing, v->water_surface = spacing > 0;
    if (!strcmp(domain, "acoustic")) v->walls = true;
    if (!strcmp(domain, "orbit")) v->true_size = true, v->radius = 3;
    v->solid_volume = !strcmp(json_get_str(h, "volume_look", ""), "solid");
    if (!strcmp(json_get_str(h, "palette", ""), "glass")) v->cmap = 103;
    if (!strcmp(json_get_str(h, "palette", ""), "fire")) v->cmap = 104;
    if (!strcmp(json_get_str(h, "volume_look", ""), "iso"))
        snprintf(v->iso_field, sizeof v->iso_field, "%s", json_get_str(h, "iso_field", "")), v->iso_level = json_get_num(h, "iso_level", 0);
    const JsonValue *looks = json_get(h, "looks");
    for (size_t i = 0; i < json_len(looks) && v->nlooks < 12; i++) {
        int id = labscene_look_id(json_str(json_value_at(looks, i)));
        if (id) snprintf(v->looks[v->nlooks].part, sizeof v->looks[0].part, "%s", json_key_at(looks, i)), v->looks[v->nlooks++].look = id;
    }
    json_free(h);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: labxr RESULT.lab --info | --frame N [--view JSON | --view-file FILE] [--field F] [--range LO HI] -o OUT.glb|-\n");
        return 2;
    }
    const char *in = argv[1], *out = NULL, *view_text = NULL, *view_file = NULL, *field = NULL;
    bool info = false, frame_given = false, range_given = false, print_view = false;
    int frame = 0;
    double lo = 0, hi = 1;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--info")) info = true;
        else if (!strcmp(a, "--frame") && i + 1 < argc) frame = atoi(argv[++i]), frame_given = true;
        else if (!strcmp(a, "--print-view")) print_view = true;
        else if (!strcmp(a, "--view") && i + 1 < argc) view_text = argv[++i];
        else if (!strcmp(a, "--view-file") && i + 1 < argc) view_file = argv[++i];
        else if (!strcmp(a, "--field") && i + 1 < argc) field = argv[++i];
        else if (!strcmp(a, "--range") && i + 2 < argc) lo = atof(argv[i + 1]), hi = atof(argv[i + 2]), i += 2, range_given = true;
        else if (!strcmp(a, "-o") && i + 1 < argc) out = argv[++i];
        else {
            fprintf(stderr, "labxr: unknown argument %s\n", a);
            return 2;
        }
    }
    char err[300];
    LabFile *lf = lab_open(in, err, sizeof err);
    if (!lf) return fail(err);
    if (info) {
        JsonValue *j = labxr_info(lf, err, sizeof err);
        if (!j) return lab_close_file(lf), fail(err);
        char *s = json_dump(j, 0, NULL, NULL);
        if (s) puts(s);
        free(s), json_free(j), lab_close_file(lf);
        return s ? 0 : 1;
    }
    if (!print_view && (!frame_given || !out)) return lab_close_file(lf), fail("--frame N and -o OUT are needed (or --info, --print-view)");
    int n = lab_frame_count(lf);
    if (frame < 0) frame += n;
    LabXrView v;
    labxr_view_defaults(&v);
    JsonValue *vj = NULL;
    if (view_text) vj = json_parse(view_text, strlen(view_text), NULL, NULL);
    else if (view_file) vj = json_read_file(view_file, 1 << 20, NULL);
    if ((view_text || view_file) && !vj) return lab_close_file(lf), fail("the view is not valid JSON");
    if (vj) {
        bool ok = labxr_view_from_json(vj, &v, err, sizeof err);
        json_free(vj);
        if (!ok) return lab_close_file(lf), fail(err);
    } else header_view(lf, &v);
    if (field) snprintf(v.field, sizeof v.field, "%s", field);
    if (!v.field[0]) first_field(lf, v.field, sizeof v.field);
    if (!vj && !v.unit[0]) { /* the header's unit of the field */
        JsonValue *h = json_parse(lab_header(lf), strlen(lab_header(lf)), NULL, NULL);
        snprintf(v.unit, sizeof v.unit, "%s", json_get_str(json_get(h, "fields"), v.field, ""));
        json_free(h);
    }
    if (range_given) v.lo = lo, v.hi = hi;
    else if (!vj) { /* over the frames, as the app takes it */
        LabViewOpts o;
        labview_defaults(&o);
        o.field = v.field, o.absval = v.absval, o.logscale = v.logscale;
        o.first = 0, o.last = n - 1, o.every = n > 40 ? n / 20 : 1;
        labview_range(lf, &o, &v.lo, &v.hi);
    }
    if (print_view) { /* the cmap as the legend resolves it, as the app reports it */
        if (v.cmap == STYLE_AUTO) v.cmap = style_pick(v.lo, v.hi);
        JsonValue *j = labxr_view_json(&v);
        char *t = j ? json_dump(j, 0, NULL, NULL) : NULL;
        if (t) puts(t);
        free(t), json_free(j), lab_close_file(lf);
        return t ? 0 : 1;
    }
    unsigned char *glb = NULL;
    size_t len = 0;
    if (!labxr_frame_glb(lf, frame, &v, &glb, &len, err, sizeof err)) return lab_close_file(lf), fail(err);
    lab_close_file(lf);
    FILE *f = strcmp(out, "-") ? fopen(out, "wb") : stdout;
    bool ok = f && fwrite(glb, 1, len, f) == len;
    if (f && f != stdout) ok = (fclose(f) == 0) && ok;
    else if (f) ok = (fflush(f) == 0) && ok;
    free(glb);
    if (!ok) return fail("cannot write the scene");
    if (f != stdout) fprintf(stderr, "labxr: frame %d of %s, %zu bytes, %s\n", frame, in, len, out);
    return 0;
}
