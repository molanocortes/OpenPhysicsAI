/* labxr.c - see labxr.h */
#include "labxr.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/glb.h"
#include "../core/png.h"
#include "labscene.h"
#include "labview.h"
#include "labwater.h"
#include "style.h"

/* ---- the view ---------------------------------------------------------------------------------------------------- */

void labxr_view_defaults(LabXrView *v) {
    memset(v, 0, sizeof *v);
    v->hi = 1;
    v->cmap = STYLE_AUTO;
    v->section_axis = -1;
    v->section_fraction = 0.5;
    v->radius = 2.5;
}

JsonValue *labxr_view_json(const LabXrView *v) {
    JsonValue *j = json_object(), *s, *iso, *w, *looks;
    if (!j) return NULL;
    json_set_string(j, "field", v->field);
    json_set_string(j, "unit", v->unit);
    json_set_number(j, "lo", isfinite(v->lo) ? v->lo : 0);
    json_set_number(j, "hi", isfinite(v->hi) ? v->hi : 1);
    json_set_int(j, "cmap", v->cmap);
    if ((s = json_set_object(j, "section")))
        json_set_int(s, "axis", v->section_axis), json_set_number(s, "fraction", v->section_fraction), json_set_bool(s, "flip", v->section_flip);
    if ((iso = json_set_object(j, "iso"))) json_set_string(iso, "field", v->iso_field), json_set_number(iso, "level", v->iso_level);
    if ((w = json_set_object(j, "water"))) json_set_bool(w, "surface", v->water_surface), json_set_number(w, "spacing_m", v->water_spacing);
    json_set_bool(j, "mesh", v->mesh), json_set_bool(j, "abs", v->absval), json_set_bool(j, "log", v->logscale);
    json_set_bool(j, "solid_points", v->solid_points), json_set_bool(j, "true_size", v->true_size);
    json_set_bool(j, "solid_volume", v->solid_volume), json_set_bool(j, "walls", v->walls);
    json_set_number(j, "radius_px", v->radius), json_set_number(j, "point_radius_m", v->point_radius_m);
    if ((looks = json_set_object(j, "looks")))
        for (int i = 0; i < v->nlooks; i++) json_set_string(looks, v->looks[i].part, labscene_look_name(v->looks[i].look));
    return j;
}

static bool bad(char *err, size_t cap, const char *what) {
    snprintf(err, cap, "view: %s", what);
    return false;
}
static bool num(const JsonValue *o, const char *k, double *out, double lo, double hi, char *err, size_t cap) {
    const JsonValue *x = json_get(o, k);
    if (!x) return true;
    if (x->type != JSON_NUMBER || !isfinite(x->u.number) || x->u.number < lo || x->u.number > hi) {
        snprintf(err, cap, "view: %s must be a number from %g to %g", k, lo, hi);
        return false;
    }
    *out = x->u.number;
    return true;
}
static bool flag(const JsonValue *o, const char *k, bool *out, char *err, size_t cap) {
    const JsonValue *x = json_get(o, k);
    if (!x) return true;
    if (x->type != JSON_BOOL) {
        snprintf(err, cap, "view: %s must be true or false", k);
        return false;
    }
    *out = x->u.boolean;
    return true;
}
static bool text(const JsonValue *o, const char *k, char *out, size_t n, char *err, size_t cap) {
    const JsonValue *x = json_get(o, k);
    if (!x) return true;
    if (x->type != JSON_STRING || x->u.string.len >= n) {
        snprintf(err, cap, "view: %s must be a text shorter than %zu bytes", k, n);
        return false;
    }
    memcpy(out, x->u.string.ptr, x->u.string.len + 1);
    return true;
}

bool labxr_view_from_json(const JsonValue *j, LabXrView *v, char *err, size_t cap) {
    if (!j || j->type != JSON_OBJECT) return bad(err, cap, "a JSON object is expected");
    double cmap = v->cmap, axis = v->section_axis;
    const double BIG = 1e300;
    if (!text(j, "field", v->field, sizeof v->field, err, cap) || !text(j, "unit", v->unit, sizeof v->unit, err, cap) ||
        !num(j, "lo", &v->lo, -BIG, BIG, err, cap) || !num(j, "hi", &v->hi, -BIG, BIG, err, cap) ||
        !num(j, "cmap", &cmap, 0, STYLE_AUTO, err, cap) || !flag(j, "mesh", &v->mesh, err, cap) ||
        !flag(j, "abs", &v->absval, err, cap) || !flag(j, "log", &v->logscale, err, cap) ||
        !flag(j, "solid_points", &v->solid_points, err, cap) || !flag(j, "true_size", &v->true_size, err, cap) ||
        !flag(j, "solid_volume", &v->solid_volume, err, cap) || !flag(j, "walls", &v->walls, err, cap) ||
        !num(j, "radius_px", &v->radius, 0, 100, err, cap) || !num(j, "point_radius_m", &v->point_radius_m, 0, 1e3, err, cap))
        return false;
    int c = (int)cmap;
    if (c != cmap || !((c >= 0 && c < SW_CMAP_COUNT) || (c >= 100 && c <= 104) || c == STYLE_AUTO))
        return bad(err, cap, "cmap must name a palette of the lab (labview.h)");
    v->cmap = c;
    if (v->hi < v->lo) return bad(err, cap, "hi must not be below lo");
    const JsonValue *s = json_get(j, "section");
    if (s) {
        if (s->type != JSON_OBJECT) return bad(err, cap, "section must be an object");
        if (!num(s, "axis", &axis, -1, 2, err, cap) || !num(s, "fraction", &v->section_fraction, 0, 1, err, cap) ||
            !flag(s, "flip", &v->section_flip, err, cap))
            return false;
        if (axis != (int)axis) return bad(err, cap, "section.axis must be -1, 0, 1 or 2");
        v->section_axis = (int)axis;
    }
    const JsonValue *iso = json_get(j, "iso");
    if (iso && (iso->type != JSON_OBJECT || !text(iso, "field", v->iso_field, sizeof v->iso_field, err, cap) ||
                !num(iso, "level", &v->iso_level, -BIG, BIG, err, cap)))
        return iso->type != JSON_OBJECT ? bad(err, cap, "iso must be an object") : false;
    const JsonValue *w = json_get(j, "water");
    if (w && (w->type != JSON_OBJECT || !flag(w, "surface", &v->water_surface, err, cap) ||
              !num(w, "spacing_m", &v->water_spacing, 0, 10, err, cap)))
        return w->type != JSON_OBJECT ? bad(err, cap, "water must be an object") : false;
    const JsonValue *looks = json_get(j, "looks");
    if (looks) {
        if (looks->type != JSON_OBJECT || json_len(looks) > 12) return bad(err, cap, "looks must be an object of at most 12 parts");
        v->nlooks = 0;
        for (size_t i = 0; i < json_len(looks); i++) {
            const char *part = json_key_at(looks, i), *name = json_str(json_value_at(looks, i));
            int id = labscene_look_id(name);
            if (!id || strlen(part) >= sizeof v->looks[0].part) return bad(err, cap, "each look must be a known material (labscene.h)");
            snprintf(v->looks[v->nlooks].part, sizeof v->looks[0].part, "%s", part);
            v->looks[v->nlooks++].look = id;
        }
    }
    return true;
}

static void view_opts(const LabXrView *v, LabViewOpts *o) {
    labview_defaults(o);
    o->field = v->field, o->unit = v->unit, o->cmap = v->cmap;
    o->mesh = v->mesh, o->absval = v->absval, o->logscale = v->logscale, o->solid_points = v->solid_points;
    o->true_size = v->true_size, o->solid_volume = v->solid_volume, o->walls = v->walls;
    o->radius = v->radius, o->point_radius_m = v->point_radius_m;
    o->nlooks = v->nlooks;
    for (int i = 0; i < v->nlooks; i++) {
        snprintf(o->looks[i].part, sizeof o->looks[i].part, "%s", v->looks[i].part);
        o->looks[i].look = v->looks[i].look;
    }
}

/* ---- palette and materials --------------------------------------------------------------------------------------- */

unsigned char *labxr_palette_png(int cmap, size_t *len) {
    unsigned char px[256 * 3], *out = NULL;
    for (int i = 0; i < 256; i++) {
        float rgb[3] = {0.80f, 0.80f, 0.79f};
        if (i < LABXR_TEXELS) labview_cmap_rgb(cmap, i / (double)(LABXR_TEXELS - 1), rgb);
        for (int k = 0; k < 3; k++) {
            float c = rgb[k] < 0 ? 0 : rgb[k] > 1 ? 1 : rgb[k];
            px[3 * i + k] = (unsigned char)(c * 255 + 0.5f);
        }
    }
    return png_encode(px, 256, 1, 3, &out, len) ? out : NULL;
}

/* the palette coordinate of a value: the centre of its texel */
static float palette_u(double value, double lo, double hi) {
    double t = hi > lo ? (value - lo) / (hi - lo) : 0.5;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return (float)((0.5 + t * (LABXR_TEXELS - 1)) / 256);
}
static const float NO_VALUE_U = 252.0f / 256; /* the neutral grey after the map */

/* the colours labgpu.c gives what is not coloured by the field: a body without the field, element edges, and the
 * material looks (sRGB) with their gloss */
enum { KEY_FIELD, KEY_PLAIN, KEY_EDGES, KEY_LOOK0, NKEYS = KEY_LOOK0 + LOOK_COUNT };
static void key_colour(int key, float rgb[3], double *gloss) {
    static const float LOOK[LOOK_COUNT][3] = {{.90f, .90f, .88f}, {.86f, .50f, .30f}, {.80f, .22f, .20f}, {.22f, .38f, .80f},
                                              {.64f, .66f, .69f}, {.82f, .83f, .85f}, {.72f, .86f, .92f}, {.72f, .09f, .11f},
                                              {.93f, .74f, .66f}};
    static const double GLOSS[LOOK_COUNT] = {32, 90, 60, 60, 60, 60, 160, 10, 40};
    int look = key >= KEY_LOOK0 ? key - KEY_LOOK0 : 0;
    const float *c = key == KEY_EDGES ? (const float[3]){.80f, .80f, .79f} : LOOK[look];
    memcpy(rgb, c, 3 * sizeof *c);
    *gloss = key >= KEY_LOOK0 ? GLOSS[look] : 32;
}
static int vertex_key(const LabVertex *v) {
    if (v->field > 0) return KEY_FIELD;
    if (v->field > -0.5f) return KEY_PLAIN;
    if (v->field > -1.5f) return KEY_EDGES;
    int look = (int)(-v->field + .5f) - 1;
    return look > 0 && look < LOOK_COUNT ? KEY_LOOK0 + look : KEY_PLAIN;
}

/* a triangle with any value of the field is drawn with the field */
static int tri_key(const LabVertex *a) {
    int k = vertex_key(a);
    for (int q = 1; q < 3; q++)
        if (vertex_key(a + q) == KEY_FIELD) k = KEY_FIELD;
    return k;
}

static float lin(float c) { return powf(c, 2.2f); } /* labgpu.c's toLinear */

/* a material per (lines or not, key): the field's palette texture, or a colour; lines are unlit, as in labgpu.c */
static int material(Glb *g, int *cache, bool lines, int key, int texture) {
    int slot = (lines ? NKEYS : 0) + key;
    if (cache[slot] >= 0) return cache[slot];
    JsonValue *m = json_object(), *pbr = m ? json_set_object(m, "pbrMetallicRoughness") : NULL;
    if (!pbr) return json_free(m), -1;
    float rgb[3];
    double gloss;
    key_colour(key, rgb, &gloss);
    char name[64];
    snprintf(name, sizeof name, "%s%s%s", lines ? "lines:" : "", key == KEY_FIELD ? "field" : key == KEY_PLAIN ? "plain" : key == KEY_EDGES ? "edges" : "look:",
             key >= KEY_LOOK0 ? labscene_look_name(key - KEY_LOOK0) : "");
    json_set_string(m, "name", name);
    if (key == KEY_FIELD) {
        JsonValue *t = json_set_object(pbr, "baseColorTexture");
        if (t) json_set_int(t, "index", texture);
    } else {
        double c[4] = {1, 1, 1, 1};
        for (int k = 0; k < 3; k++) c[k] = lin(lines ? rgb[k] * .9f + .05f : rgb[k]);
        json_set(pbr, "baseColorFactor", json_numbers(c, 4));
    }
    json_set_number(pbr, "metallicFactor", 0);
    json_set_number(pbr, "roughnessFactor", fmax(0.25, sqrt(2.0 / (gloss + 2)))); /* a Blinn exponent as roughness */
    json_set_bool(m, "doubleSided", true);
    return cache[slot] = glb_material(g, m);
}

/* ---- welded primitives ------------------------------------------------------------------------------------------- */

/* one vertex of the headset's scene: glTF position and normal, palette u, value, radius, part */
typedef struct Rec {
    float f[10];
} Rec;
typedef struct Prim {
    Rec *v;
    uint32_t *idx, *table;
    size_t n, cap, ni, icap, tcap;
    bool fail;
} Prim;

static uint32_t rec_hash(const Rec *r) {
    uint32_t h = 2166136261u, w;
    for (int i = 0; i < 10; i++) {
        memcpy(&w, &r->f[i], 4);
        h = (h ^ w) * 16777619u;
    }
    return h;
}
static bool grow(void **p, size_t *cap, size_t need, size_t size) {
    if (need <= *cap) return true;
    size_t c = *cap ? *cap : 1024;
    while (c < need) c *= 2;
    void *q = realloc(*p, c * size);
    if (!q) return false;
    *p = q, *cap = c;
    return true;
}
/* add a vertex: identical records are stored once (weld) when weld is set */
static void prim_add(Prim *p, const Rec *r, bool weld) {
    if (p->fail) return;
    if (!grow((void **)&p->idx, &p->icap, p->ni + 1, sizeof *p->idx)) return (void)(p->fail = true);
    if (weld && 2 * (p->n + 1) > p->tcap) { /* rehash into a table at least twice the vertices */
        size_t cap = p->tcap ? 2 * p->tcap : 4096;
        uint32_t *t = malloc(cap * sizeof *t);
        if (!t) return (void)(p->fail = true);
        memset(t, 0xFF, cap * sizeof *t);
        for (size_t i = 0; i < p->n; i++) {
            size_t h = rec_hash(&p->v[i]) & (cap - 1);
            while (t[h] != 0xFFFFFFFFu) h = (h + 1) & (cap - 1);
            t[h] = (uint32_t)i;
        }
        free(p->table);
        p->table = t, p->tcap = cap;
    }
    if (weld) {
        size_t h = rec_hash(r) & (p->tcap - 1);
        while (p->table[h] != 0xFFFFFFFFu) {
            if (!memcmp(p->v[p->table[h]].f, r->f, sizeof r->f)) {
                p->idx[p->ni++] = p->table[h];
                return;
            }
            h = (h + 1) & (p->tcap - 1);
        }
        p->table[h] = (uint32_t)p->n;
    }
    if (!grow((void **)&p->v, &p->cap, p->n + 1, sizeof *p->v)) return (void)(p->fail = true);
    p->v[p->n] = *r;
    p->idx[p->ni++] = (uint32_t)p->n++;
}
static void prim_free(Prim *p) {
    free(p->v), free(p->idx), free(p->table);
    memset(p, 0, sizeof *p);
}

/* a lab vertex as a record: glTF (x, y, z) = lab (x, z, -y) */
static Rec record(const LabVertex *v, const float n[3], double lo, double hi, bool field) {
    Rec r;
    r.f[0] = v->p[0], r.f[1] = v->p[2], r.f[2] = -v->p[1];
    r.f[3] = n[0], r.f[4] = n[2], r.f[5] = -n[1];
    bool has = field && v->field > 0;
    r.f[6] = has ? palette_u(v->value, lo, hi) : NO_VALUE_U;
    r.f[7] = has ? v->value : (field ? NAN : 0);
    r.f[8] = isfinite(v->radius) ? v->radius : 0;
    r.f[9] = (float)v->part;
    return r;
}

/* the primitive's attributes into the GLB; constant radius and part go into its extras instead */
static bool emit(Glb *g, JsonValue *prims, const Prim *p, int mode, int mat, int key, bool normals, bool points) {
    if (!p->n) return true;
    size_t n = p->n;
    float *buf = malloc(n * 3 * sizeof *buf);
    if (!buf) return false;
    bool same_r = true, same_p = true;
    for (size_t i = 1; i < n; i++) same_r = same_r && p->v[i].f[8] == p->v[0].f[8], same_p = same_p && p->v[i].f[9] == p->v[0].f[9];
    JsonValue *attr = glb_primitive(prims, mode, points ? -1 : glb_indices(g, p->idx, p->ni), mat);
    if (!attr) return free(buf), false;
    for (size_t i = 0; i < n; i++) memcpy(buf + 3 * i, p->v[i].f, 3 * sizeof *buf);
    json_set_int(attr, "POSITION", glb_floats(g, buf, n, 3, true));
    if (normals) {
        for (size_t i = 0; i < n; i++) memcpy(buf + 3 * i, p->v[i].f + 3, 3 * sizeof *buf);
        json_set_int(attr, "NORMAL", glb_floats(g, buf, n, 3, false));
    }
    for (size_t i = 0; i < n; i++) buf[2 * i] = p->v[i].f[6], buf[2 * i + 1] = 0.5f;
    json_set_int(attr, "TEXCOORD_0", glb_floats(g, buf, n, 2, false));
    if (key == KEY_FIELD) {
        for (size_t i = 0; i < n; i++) buf[i] = p->v[i].f[7];
        json_set_int(attr, "_VALUE", glb_floats(g, buf, n, 1, false));
    }
    if (points && !same_r) {
        for (size_t i = 0; i < n; i++) buf[i] = p->v[i].f[8];
        json_set_int(attr, "_RADIUS", glb_floats(g, buf, n, 1, false));
    }
    if (!same_p) {
        for (size_t i = 0; i < n; i++) buf[i] = p->v[i].f[9];
        json_set_int(attr, "_PART", glb_floats(g, buf, n, 1, false));
    }
    free(buf);
    /* the primitive's own facts, for the page: what colours it, and the radius and part when they do not vary */
    JsonValue *prim = json_at(prims, json_len(prims) - 1), *ex = json_set_object(prim, "extras");
    if (!ex) return false;
    json_set_string(ex, "key", key == KEY_FIELD ? "field" : key == KEY_PLAIN ? "plain" : key == KEY_EDGES ? "edges" : "look");
    if (key >= KEY_LOOK0) json_set_string(ex, "look", labscene_look_name(key - KEY_LOOK0));
    if (points && same_r) json_set_number(ex, "radius_m", p->v[0].f[8]);
    if (mode == GLB_TRIANGLES && !normals) json_set_bool(ex, "flat", true);
    if (same_p) json_set_int(ex, "part", (long long)p->v[0].f[9]);
    return true;
}

/* ---- the frame --------------------------------------------------------------------------------------------------- */

static void box_add(double lo[3], double hi[3], const double p[3]) {
    for (int k = 0; k < 3; k++)
        if (isfinite(p[k])) lo[k] = fmin(lo[k], p[k]), hi[k] = fmax(hi[k], p[k]);
}
/* every node and every block corner of a frame */
static void frame_box(const LabFrame *fr, double lo[3], double hi[3]) {
    for (int i = 0; i < fr->nparts; i++) {
        const LabPart *p = fr->parts + i;
        for (int k = 0; k < p->npoints; k++) box_add(lo, hi, p->xyz + 3 * k);
        for (int b = 0; b < p->nblocks; b++) {
            const LabBlock *bl = p->blocks + b;
            double far[3];
            for (int k = 0; k < 3; k++) far[k] = bl->origin[k] + bl->dx[k] * bl->n[k];
            box_add(lo, hi, bl->origin), box_add(lo, hi, far);
        }
    }
}

static JsonValue *vec(const double *v, int n) { return json_numbers(v, (size_t)n); }

/* ---- a planar result, or a picture: an upright picture of its plane ---------------------------------------------- */

enum { PICTURE_PX = 1024, PLANE_GRID = 256 };
static const double PICTURE_WIDTH_M = 0.4; /* a picture whose blocks are not in metres (r, g, b fields) */

/* the part that is a plane: blocks in the xy plane, one cell thick; -1 if none */
static int plane_part(const LabFrame *fr) {
    for (int i = 0; i < fr->nparts; i++) {
        const LabPart *p = fr->parts + i;
        bool ok = p->kind == LAB_BLOCKS && p->nblocks > 0;
        for (int b = 0; ok && b < p->nblocks; b++) ok = p->blocks[b].n[2] <= 1 && p->blocks[b].plane == LAB_PLANE_XY;
        if (ok) return i;
    }
    return -1;
}
static bool is_picture(const LabPart *p) {
    return p->nblocks == 1 && lab_find_field(p, "r") && lab_find_field(p, "g") && lab_find_field(p, "b");
}
static void plane_extent(const LabPart *p, double lo[2], double hi[2]) {
    lo[0] = lo[1] = INFINITY, hi[0] = hi[1] = -INFINITY;
    for (int b = 0; b < p->nblocks; b++)
        for (int k = 0; k < 2; k++) {
            const LabBlock *bl = p->blocks + b;
            lo[k] = fmin(lo[k], bl->origin[k]), hi[k] = fmax(hi[k], bl->origin[k] + bl->dx[k] * bl->n[k]);
        }
}
/* the value at (x, y): bilinear between cell centres of the finest block covering it (as labprobe and labfilm read a
 * plane); NAN outside every block */
static double plane_sample(const LabPart *p, const float *f, double x, double y) {
    int best = -1, lev = -1;
    size_t first = 0, best_first = 0;
    double bu = 0, bv = 0;
    for (int b = 0; b < p->nblocks; b++) {
        const LabBlock *bl = p->blocks + b;
        double u = (x - bl->origin[0]) / bl->dx[0], v = (y - bl->origin[1]) / bl->dx[1];
        if (u >= 0 && v >= 0 && u < bl->n[0] && v < bl->n[1] && bl->level > lev) best = b, lev = bl->level, bu = u, bv = v, best_first = first;
        first += lab_block_cells(bl);
    }
    if (best < 0) return NAN;
    const LabBlock *bl = p->blocks + best;
    const float *c = f + best_first;
    int nx = bl->n[0], ny = bl->n[1];
    double u = bu - 0.5, v = bv - 0.5;
    int i0 = (int)floor(u), j0 = (int)floor(v);
    double tx = u - i0, ty = v - j0;
    int i1 = i0 + 1 < nx ? i0 + 1 : nx - 1, j1 = j0 + 1 < ny ? j0 + 1 : ny - 1;
    i0 = i0 < 0 ? 0 : i0, j0 = j0 < 0 ? 0 : j0;
    return (1 - ty) * ((1 - tx) * c[(size_t)j0 * nx + i0] + tx * c[(size_t)j0 * nx + i1]) +
           ty * ((1 - tx) * c[(size_t)j1 * nx + i0] + tx * c[(size_t)j1 * nx + i1]);
}

/* the plane's place in the scene: glTF (x, y, 0) of the plane's lab (x, y), upright, in metres; a picture without metres
 * 0.4 m wide. The same in the frames and in the info, so every frame stands in one place. */
static void plane_frame(const LabPart *p, double gl_lo[3], double gl_hi[3], double *scale) {
    double lo[2], hi[2];
    plane_extent(p, lo, hi);
    *scale = is_picture(p) && hi[0] > lo[0] ? PICTURE_WIDTH_M / (hi[0] - lo[0]) : 1;
    gl_lo[0] = lo[0] * *scale, gl_lo[1] = lo[1] * *scale, gl_lo[2] = 0;
    gl_hi[0] = hi[0] * *scale, gl_hi[1] = hi[1] * *scale, gl_hi[2] = 0;
}

static bool plane_glb(LabFile *lf, int frame, const LabFrame *fr, int pi, const LabXrView *v, unsigned char **out, size_t *len,
                      char *err, size_t cap) {
    const LabPart *p = fr->parts + pi;
    bool picture = is_picture(p);
    const LabField *f = picture ? NULL : lab_find_field(p, v->field);
    if (!picture && !f) {
        snprintf(err, cap, "the plane does not hold %s", v->field);
        return false;
    }
    double lo2[2], hi2[2], glo[3], ghi[3], scale;
    plane_extent(p, lo2, hi2);
    plane_frame(p, glo, ghi, &scale);
    double w = hi2[0] - lo2[0], h = hi2[1] - lo2[1];
    if (!(w > 0 && h > 0)) {
        snprintf(err, cap, "the plane has no extent");
        return false;
    }
    int W = w >= h ? PICTURE_PX : (int)lround(PICTURE_PX * w / h), H = w >= h ? (int)lround(PICTURE_PX * h / w) : PICTURE_PX;
    W = W < 16 ? 16 : W, H = H < 16 ? 16 : H;
    int GW = W < PLANE_GRID ? W : PLANE_GRID, GH = (int)lround((double)GW * H / W);
    GH = GH < 2 ? 2 : GH;
    double lo = v->lo, hi = v->hi, span = hi > lo ? hi - lo : 1;
    int cm = v->cmap == STYLE_AUTO ? style_pick(lo, hi) : v->cmap;
    unsigned char *px = malloc((size_t)W * H * 4);
    uint16_t *grid = picture ? NULL : malloc((size_t)GW * GH * sizeof *grid);
    bool ok = px && (picture || grid);
    const float *ch[3] = {NULL, NULL, NULL};
    if (picture) ch[0] = lab_find_field(p, "r")->data, ch[1] = lab_find_field(p, "g")->data, ch[2] = lab_find_field(p, "b")->data;
    for (int y = 0; ok && y < H; y++) /* the picture's first row is its top */
        for (int x = 0; x < W; x++) {
            double X = lo2[0] + (x + 0.5) * w / W, Y = hi2[1] - (y + 0.5) * h / H;
            unsigned char *q = px + 4 * ((size_t)y * W + x);
            if (picture) {
                for (int k = 0; k < 3; k++) {
                    double c = plane_sample(p, ch[k], X, Y);
                    q[k] = (unsigned char)lround(255.0 * fmin(1.0, fmax(0.0, isfinite(c) ? c : 0)));
                }
                q[3] = 255;
                continue;
            }
            double val = plane_sample(p, f->data, X, Y);
            if (!isfinite(val)) {
                q[0] = q[1] = q[2] = q[3] = 0; /* outside the blocks: nothing */
                continue;
            }
            if (v->absval) val = fabs(val);
            if (v->logscale) val = log10(fmax(fabs(val), 1e-30));
            float rgb[3];
            labview_cmap_rgb(cm, (val - lo) / span, rgb);
            for (int k = 0; k < 3; k++) q[k] = (unsigned char)lrintf(fminf(1, fmaxf(0, rgb[k])) * 255);
            q[3] = 255;
        }
    for (int y = 0; ok && grid && y < GH; y++) /* the values a probe reads: rows from the bottom, x fastest */
        for (int x = 0; x < GW; x++) {
            double X = lo2[0] + (x + 0.5) * w / GW, Y = lo2[1] + (y + 0.5) * h / GH, val = plane_sample(p, f->data, X, Y);
            if (isfinite(val) && v->absval) val = fabs(val);
            if (isfinite(val) && v->logscale) val = log10(fmax(fabs(val), 1e-30));
            double sv = isfinite(val) ? (val - lo) / span : NAN;
            grid[(size_t)y * GW + x] = glb_half((float)(isfinite(sv) ? fmin(6e4, fmax(-6e4, sv)) : sv));
        }
    unsigned char *png = NULL;
    size_t pl = 0;
    ok = ok && png_encode(px, W, H, 4, &png, &pl);
    free(px);
    Glb g;
    ok = ok && glb_init(&g, "OpenPhysicsAI labxr");
    int tex = ok ? glb_texture_png(&g, png, pl) : -1, mesh = -1;
    free(png);
    JsonValue *m = json_object(), *pbr = m ? json_set_object(m, "pbrMetallicRoughness") : NULL, *t = pbr ? json_set_object(pbr, "baseColorTexture") : NULL;
    if (t) {
        json_set_int(t, "index", tex), json_set_number(pbr, "metallicFactor", 0), json_set_number(pbr, "roughnessFactor", 1);
        json_set_string(m, "name", "picture"), json_set_string(m, "alphaMode", "MASK"), json_set_bool(m, "doubleSided", true);
    }
    int mat = ok ? glb_material(&g, m) : (json_free(m), -1);
    JsonValue *prims = ok ? glb_mesh(&g, "result", &mesh) : NULL;
    const float pos[12] = {(float)glo[0], (float)glo[1], 0, (float)ghi[0], (float)glo[1], 0, (float)ghi[0], (float)ghi[1], 0, (float)glo[0], (float)ghi[1], 0};
    const float nrm[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}, uv[8] = {0, 1, 1, 1, 1, 0, 0, 0};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    JsonValue *attr = prims ? glb_primitive(prims, GLB_TRIANGLES, glb_indices(&g, idx, 6), mat) : NULL;
    if (attr) {
        json_set_int(attr, "POSITION", glb_floats(&g, pos, 4, 3, true));
        json_set_int(attr, "NORMAL", glb_floats(&g, nrm, 4, 3, false));
        json_set_int(attr, "TEXCOORD_0", glb_floats(&g, uv, 4, 2, false));
        JsonValue *pex = json_set_object(json_at(prims, 0), "extras");
        if (pex) json_set_string(pex, "key", "picture"), json_set_int(pex, "part", pi);
    }
    ok = ok && attr;
    JsonValue *ex = ok ? json_object() : NULL, *tk = ex ? json_set_object(ex, "result") : NULL;
    if (tk) {
        json_set_string(tk, "kind", "lab");
        json_set_int(tk, "frame", frame);
        json_set_number(tk, "time", lab_frame_time(lf, frame));
        json_set_string(tk, "field", picture ? "" : v->field), json_set_string(tk, "unit", picture ? "" : v->unit);
        json_set_number(tk, "lo", lo), json_set_number(tk, "hi", hi);
        json_set_int(tk, "cmap", cm), json_set_int(tk, "geometry_cmap", cm);
        double M[16] = {1 / scale, 0, 0, 0, 0, 1 / scale, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; /* lab (x, y, 0) of glTF (x, y) */
        json_set(tk, "to_source", vec(M, 16));
        json_set_string(tk, "source_unit", picture ? "" : "m");
        JsonValue *parts = json_set_array(tk, "parts");
        for (int i = 0; parts && i < fr->nparts; i++) json_push(parts, json_string(fr->parts[i].name));
        double blo[3] = {lo2[0], lo2[1], 0}, bhi[3] = {hi2[0], hi2[1], 0};
        JsonValue *box = json_set_object(tk, "box");
        if (box) json_set(box, "lo", vec(blo, 3)), json_set(box, "hi", vec(bhi, 3));
        JsonValue *pic = json_set_object(tk, "picture");
        if (pic) {
            json_set_bool(pic, "rgb", picture);
            json_set_bool(pic, "true_size", !picture);
            json_set(pic, "gltf_lo", vec(glo, 3)), json_set(pic, "gltf_hi", vec(ghi, 3));
            if (grid) {
                double dims[2] = {GW, GH};
                json_set(pic, "dims", vec(dims, 2));
                json_set_int(pic, "values", glb_bytes(&g, grid, (size_t)GW * GH * sizeof *grid));
                json_set_string(pic, "encoding", "half floats of (value - lo) / (hi - lo), rows from the bottom, x fastest");
            }
        }
        json_set(tk, "volume", json_null()), json_set(tk, "section", json_null());
    }
    free(grid);
    ok = ok && tk && glb_node(&g, "result", mesh, ex) >= 0;
    if (!tk) json_free(ex);
    ok = ok && glb_finish(&g, out, len);
    if (!ok) snprintf(err, cap, "out of memory while drawing the plane");
    glb_free(&g);
    return ok;
}

bool labxr_frame_glb(LabFile *lf, int frame, const LabXrView *v, unsigned char **out, size_t *len, char *err, size_t cap) {
    *out = NULL, *len = 0;
    if (frame < 0 || frame >= lab_frame_count(lf)) {
        snprintf(err, cap, "frame %d: the result holds frames 0 to %d", frame, lab_frame_count(lf) - 1);
        return false;
    }
    LabFrame fr, water = {0};
    if (!lab_read_frame(lf, frame, &fr, err, cap)) return false;
    if (!labscene_supported(&fr)) { /* a plane (or a picture) is shown as an upright picture of it */
        int pi = plane_part(&fr);
        bool ok = pi >= 0 && plane_glb(lf, frame, &fr, pi, v, out, len, err, cap);
        if (pi < 0) snprintf(err, cap, "this result is neither 3D nor a plane: the headset cannot show it");
        lab_frame_free(&fr);
        return ok;
    }
    LabViewOpts o;
    view_opts(v, &o);
    LabScene s;
    if (!labscene_build(&fr, &o, v->section_axis, v->section_fraction, v->section_flip, &s)) {
        lab_frame_free(&fr);
        snprintf(err, cap, "frame %d has no drawable 3D geometry", frame);
        return false;
    }
    double lo = v->lo, hi = v->hi;
    int cm = v->cmap == STYLE_AUTO ? style_pick(lo, hi) : v->cmap;
    char note[200] = "";

    /* the computed volume (or the surface of water particles), as the app uploads it */
    const LabPart *vol = labscene_volume(&fr);
    bool water_mode = false;
    if (!vol && v->water_surface && v->water_spacing > 0 && labwater_volume(&fr, v->water_spacing, v->field, &water) && water.nparts)
        vol = water.parts, water_mode = true;
    const LabField *vf = vol ? lab_find_field(vol, v->field) : NULL;
    if (vol && !vf) snprintf(note, sizeof note, "the volume does not hold %s", v->field), vol = NULL;
    int geo_cm = cm == SW_CMAP_LAB && !vol && v->solid_points ? SW_CMAP_LAB_SOLID : cm; /* as labgpu.c draws particles */

    Glb g;
    bool ok = glb_init(&g, "OpenPhysicsAI labxr");
    size_t pl = 0, vl = 0;
    unsigned char *pal = labxr_palette_png(geo_cm, &pl), *vpal = vol && cm != geo_cm ? labxr_palette_png(cm, &vl) : NULL;
    int tex = pal ? glb_texture_png(&g, pal, pl) : -1, vtex = vpal ? glb_texture_png(&g, vpal, vl) : tex;
    free(pal), free(vpal);
    ok = ok && tex >= 0;
    int mats[2 * NKEYS];
    for (int i = 0; i < 2 * NKEYS; i++) mats[i] = -1;
    int mesh = -1;
    JsonValue *prims = ok ? glb_mesh(&g, "result", &mesh) : NULL;
    ok = ok && prims;

    /* triangles, by key; a triangle with any value of the field is drawn with the field (a vertex without one is grey).
     * Faces of hexahedra and quads have no smooth normal: a primitive made only of them carries no NORMAL, so the
     * headset shades it flat from the screen (as labgpu.c does) and its corners weld across faces of equal value. */
    for (int key = 0; ok && key < NKEYS; key++) {
        Prim p = {0};
        bool any_smooth = false;
        for (size_t t = 0; t + 2 < s.ntri && !any_smooth; t += 3)
            if (tri_key(s.tri + t) == key)
                for (int q = 0; q < 3; q++) {
                    const float *n = s.tri[t + (size_t)q].n;
                    any_smooth = any_smooth || n[0] * n[0] + n[1] * n[1] + n[2] * n[2] > 0.25f;
                }
        for (size_t t = 0; t + 2 < s.ntri; t += 3) {
            const LabVertex *a = s.tri + t;
            if (tri_key(a) != key) continue;
            float fn[3] = {0, 0, 0};
            const float *A = a[0].p, *B = a[1].p, *C = a[2].p;
            float e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
            fn[0] = e1[1] * e2[2] - e1[2] * e2[1], fn[1] = e1[2] * e2[0] - e1[0] * e2[2], fn[2] = e1[0] * e2[1] - e1[1] * e2[0];
            float l = sqrtf(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
            for (int c = 0; c < 3; c++) fn[c] = l > 0 ? fn[c] / l : (c == 2);
            static const float flat[3] = {0, 0, 0};
            for (int q = 0; q < 3; q++) { /* a smooth normal where the scene has one, else the face's */
                const float *n = a[q].n;
                bool smooth = n[0] * n[0] + n[1] * n[1] + n[2] * n[2] > 0.25f;
                Rec r = record(a + q, smooth ? n : any_smooth ? fn : flat, lo, hi, key == KEY_FIELD);
                prim_add(&p, &r, true);
            }
        }
        ok = !p.fail && emit(&g, prims, &p, GLB_TRIANGLES, p.n ? material(&g, mats, false, key, tex) : -1, key, any_smooth, false);
        prim_free(&p);
    }
    /* particles and lines */
    for (int kind = 0; ok && kind < 2; kind++) {
        const LabVertex *vs = kind ? s.lines : s.points;
        size_t n = kind ? s.nlines : s.npoints;
        static const float none[3] = {0, 0, 0};
        for (int key = 0; ok && key < NKEYS; key++) {
            Prim p = {0};
            for (size_t i = 0; i < n; i += kind ? 2 : 1) {
                int k = vertex_key(vs + i);
                if (kind && i + 1 < n && vertex_key(vs + i + 1) == KEY_FIELD) k = KEY_FIELD;
                if (k != key) continue;
                for (int q = 0; q < (kind ? 2 : 1) && i + (size_t)q < n; q++) {
                    Rec r = record(vs + i + q, none, lo, hi, key == KEY_FIELD);
                    prim_add(&p, &r, kind == 1);
                }
            }
            ok = !p.fail && emit(&g, prims, &p, kind ? GLB_LINES : GLB_POINTS, p.n ? material(&g, mats, kind == 1, key, tex) : -1, key, false, kind == 0);
            prim_free(&p);
        }
    }

    /* the frame's facts */
    JsonValue *ex = json_object(), *tk = ex ? json_set_object(ex, "result") : NULL;
    ok = ok && tk;
    if (ok) {
        json_set_string(tk, "kind", "lab");
        json_set_int(tk, "frame", frame);
        json_set_number(tk, "time", lab_frame_time(lf, frame));
        json_set_string(tk, "field", v->field), json_set_string(tk, "unit", v->unit);
        json_set_number(tk, "lo", lo), json_set_number(tk, "hi", hi);
        json_set_int(tk, "cmap", cm), json_set_int(tk, "geometry_cmap", geo_cm);
        json_set_bool(tk, "abs", v->absval), json_set_bool(tk, "log", v->logscale);
        JsonValue *pp = json_set_object(tk, "palette");
        if (pp) json_set_int(pp, "texture", tex), json_set_int(pp, "texels", LABXR_TEXELS), json_set_int(pp, "width", 256);
        static const double TO_LAB[16] = {1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1}; /* column-major: lab = M glTF */
        json_set(tk, "to_source", vec(TO_LAB, 16));
        json_set_string(tk, "source_unit", "m");
        JsonValue *parts = json_set_array(tk, "parts");
        for (int i = 0; parts && i < fr.nparts; i++) json_push(parts, json_string(fr.parts[i].name));
        JsonValue *box = json_set_object(tk, "box");
        if (box) json_set(box, "lo", vec(s.lo, 3)), json_set(box, "hi", vec(s.hi, 3));
        JsonValue *pt = json_set_object(tk, "points");
        if (pt) json_set_number(pt, "pixel_radius", v->radius), json_set_bool(pt, "water_droplets", water_mode);
        if (v->section_axis >= 0) {
            JsonValue *sec = json_set_object(tk, "section");
            int a = v->section_axis;
            if (sec)
                json_set_int(sec, "axis", a), json_set_bool(sec, "flip", v->section_flip),
                    json_set_number(sec, "position", s.lo[a] + v->section_fraction * (s.hi[a] - s.lo[a]));
        } else json_set(tk, "section", json_null());
        if (vol) { /* the field as half floats of (value - lo) / (hi - lo), and its mask as bytes */
            const LabBlock *b = vol->blocks;
            size_t nc = lab_block_cells(b);
            uint16_t *h = malloc(nc * sizeof *h);
            float *mask = malloc(nc * sizeof *mask);
            unsigned char *m8 = malloc(nc);
            ok = h && mask && m8;
            if (ok) {
                double span = hi > lo ? hi - lo : 1, zero = lo < 0 && hi > 0 ? -lo / span : 0;
                for (size_t i = 0; i < nc; i++) {
                    double x = vf->data[i], sv;
                    if (!isfinite(x)) sv = zero; /* no value: no strength, as far as the colours go */
                    else {
                        if (v->absval) x = fabs(x);
                        if (v->logscale) x = log10(fmax(fabs(x), 1e-30));
                        sv = (x - lo) / span;
                        sv = sv < -6e4 ? -6e4 : sv > 6e4 ? 6e4 : sv;
                    }
                    h[i] = glb_half((float)sv);
                }
                int mk = labscene_volume_mask(vol, v->iso_field[0] ? v->iso_field : NULL, v->iso_level, mask);
                for (size_t i = 0; i < nc; i++) {
                    float m = mask[i] < 0 ? 0 : mask[i] > 1 ? 1 : mask[i];
                    m8[i] = (unsigned char)lrintf(m * 255);
                }
                JsonValue *vo = json_set_object(tk, "volume");
                double dims[3] = {b->n[0], b->n[1], b->n[2]};
                ok = vo != NULL;
                if (ok) {
                    json_set(vo, "dims", vec(dims, 3));
                    json_set(vo, "origin", vec(b->origin, 3)), json_set(vo, "cell", vec(b->dx, 3));
                    json_set_int(vo, "values", glb_bytes(&g, h, nc * sizeof *h));
                    json_set_int(vo, "mask", mk ? glb_bytes(&g, m8, nc) : -1);
                    json_set_string(vo, "mask_kind", mk == 'i' ? "iso" : mk == 'c' ? "coverage" : mk == 'm' ? "material" : "none");
                    json_set_string(vo, "mode", water_mode ? "water" : mk == 'i' ? "iso" : v->solid_volume ? "solid" : "fog");
                    json_set_bool(vo, "walls", v->walls && !water_mode && mk != 'i');
                    json_set_int(vo, "palette", vtex);
                    json_set_string(vo, "encoding", "half floats of (value - lo) / (hi - lo), x fastest, lab axes");
                }
            }
            free(h), free(mask), free(m8);
        } else json_set(tk, "volume", json_null());
        if (note[0]) json_set_string(tk, "note", note);
    } else json_free(ex), ex = NULL;
    if (ok && !json_len(prims)) json_remove(g.root, "meshes"), mesh = -1; /* a volume alone: no mesh without primitives */
    if (ok) ok = glb_node(&g, "result", mesh, ex) >= 0, ex = NULL;
    json_free(ex);
    ok = ok && glb_finish(&g, out, len);
    if (!ok) snprintf(err, cap, "out of memory while writing the scene");
    glb_free(&g);
    labscene_free(&s);
    lab_frame_free(&water);
    lab_frame_free(&fr);
    return ok;
}

/* ---- the result -------------------------------------------------------------------------------------------------- */

JsonValue *labxr_info(LabFile *lf, char *err, size_t cap) {
    int n = lab_frame_count(lf);
    if (n < 1) {
        snprintf(err, cap, "the result holds no frames");
        return NULL;
    }
    JsonValue *h = json_parse(lab_header(lf), strlen(lab_header(lf)), NULL, NULL), *j = json_object();
    if (!j) return json_free(h), NULL;
    json_set_string(j, "title", json_get_str(h, "title", ""));
    json_set_string(j, "domain", json_get_str(h, "domain", ""));
    json_set_string(j, "time_unit", json_get_str(h, "time_unit", "s")[0] ? json_get_str(h, "time_unit", "s") : "s");
    json_set_int(j, "frames", n);
    JsonValue *times = json_set_array(j, "times"), *fields = json_set_object(j, "fields");
    for (int i = 0; times && i < n; i++) json_push(times, json_number(lab_frame_time(lf, i)));
    const JsonValue *hf = json_get(h, "fields");
    for (size_t i = 0; fields && i < json_len(hf); i++)
        if (json_str(json_value_at(hf, i))) json_set_string(fields, json_key_at(hf, i), json_str(json_value_at(hf, i)));
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    int pick[3] = {0, n / 2, n - 1};
    bool native = false;
    for (int k = 0; k < 3; k++) {
        LabFrame fr;
        if (k && pick[k] == pick[k - 1]) continue;
        if (!lab_read_frame(lf, pick[k], &fr, err, cap)) {
            json_free(h), json_free(j);
            return NULL;
        }
        if (k == 0) {
            native = labscene_supported(&fr) && strcmp(json_get_str(h, "domain", ""), "orbit");
            int pi = native ? -1 : plane_part(&fr);
            if (pi >= 0) { /* drawn as an upright picture of its plane (plane_glb): its place in glTF axes */
                double glo[3], ghi[3], scale;
                plane_frame(fr.parts + pi, glo, ghi, &scale);
                JsonValue *pic = json_set_object(j, "picture");
                if (pic) json_set_bool(pic, "rgb", is_picture(fr.parts + pi)), json_set(pic, "gltf_lo", vec(glo, 3)), json_set(pic, "gltf_hi", vec(ghi, 3));
            }
            JsonValue *parts = json_set_array(j, "parts");
            for (int i = 0; parts && i < fr.nparts; i++) json_push(parts, json_string(fr.parts[i].name));
        }
        frame_box(&fr, lo, hi);
        lab_frame_free(&fr);
    }
    json_set_bool(j, "native3d", native);
    if (isfinite(lo[0]) && isfinite(lo[1]) && isfinite(lo[2])) {
        JsonValue *box = json_set_object(j, "box");
        if (box) json_set(box, "lo", vec(lo, 3)), json_set(box, "hi", vec(hi, 3));
    }
    json_free(h);
    return j;
}
