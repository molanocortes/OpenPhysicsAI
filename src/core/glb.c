/* glb.c - see glb.h */
#include "glb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static JsonValue *arr(Glb *g, const char *key) {
    JsonValue *a = json_get(g->root, key);
    if (!a) a = json_set_array(g->root, key);
    if (!a) g->failed = true;
    return a;
}

/* append to an array of the root (v is taken over); the index of the new element, or -1 */
static int add(Glb *g, const char *key, JsonValue *v) {
    JsonValue *a = arr(g, key);
    if (!a) {
        json_free(v);
        return g->failed = true, -1;
    }
    if (!json_push(a, v)) return g->failed = true, -1; /* json_push frees v when it fails */
    return (int)json_len(a) - 1;
}

bool glb_init(Glb *g, const char *generator) {
    memset(g, 0, sizeof *g);
    g->root = json_object();
    JsonValue *asset = g->root ? json_set_object(g->root, "asset") : NULL;
    if (!asset) {
        json_free(g->root);
        g->root = NULL;
        return false;
    }
    json_set_string(asset, "version", "2.0");
    json_set_string(asset, "generator", generator ? generator : "OpenPhysicsAI");
    json_set_int(g->root, "scene", 0);
    JsonValue *scenes = json_set_array(g->root, "scenes"), *scene = json_object();
    if (!scenes || !scene || !json_push(scenes, scene) || !json_set_array(scene, "nodes")) g->failed = true;
    return true;
}

void glb_free(Glb *g) {
    json_free(g->root);
    free(g->bin);
    memset(g, 0, sizeof *g);
}

static bool reserve(Glb *g, size_t extra) {
    if (g->len + extra <= g->cap) return true;
    size_t cap = g->cap ? g->cap : 1 << 16;
    while (cap < g->len + extra) cap *= 2;
    unsigned char *b = realloc(g->bin, cap);
    if (!b) return g->failed = true, false;
    g->bin = b, g->cap = cap;
    return true;
}

static int view(Glb *g, const void *data, size_t bytes, int target) {
    if (g->failed || !reserve(g, bytes + 3)) return -1;
    size_t at = g->len;
    if (bytes) memcpy(g->bin + at, data, bytes);
    g->len += bytes;
    while (g->len % 4) g->bin[g->len++] = 0; /* every view starts 4-byte aligned */
    JsonValue *v = json_object();
    if (!v) return g->failed = true, -1;
    json_set_int(v, "buffer", 0);
    json_set_int(v, "byteOffset", (long long)at);
    json_set_int(v, "byteLength", (long long)bytes);
    if (target) json_set_int(v, "target", target);
    return add(g, "bufferViews", v);
}

int glb_bytes(Glb *g, const void *data, size_t bytes) { return view(g, data, bytes, 0); }

static int accessor(Glb *g, int bv, int component, size_t count, int comps, const double *lo, const double *hi) {
    static const char *type[] = {"", "SCALAR", "VEC2", "VEC3", "VEC4"};
    if (bv < 0 || comps < 1 || comps > 4) return g->failed = true, -1;
    JsonValue *a = json_object();
    if (!a) return g->failed = true, -1;
    json_set_int(a, "bufferView", bv);
    json_set_int(a, "componentType", component);
    json_set_int(a, "count", (long long)count);
    json_set_string(a, "type", type[comps]);
    if (lo && hi) json_set(a, "min", json_numbers(lo, (size_t)comps)), json_set(a, "max", json_numbers(hi, (size_t)comps));
    return add(g, "accessors", a);
}

int glb_floats(Glb *g, const float *v, size_t count, int comps, bool minmax) {
    double lo[4] = {0}, hi[4] = {0};
    if (minmax) {
        for (int k = 0; k < comps; k++) lo[k] = INFINITY, hi[k] = -INFINITY;
        for (size_t i = 0; i < count; i++)
            for (int k = 0; k < comps; k++) {
                double x = v[i * (size_t)comps + (size_t)k];
                if (x < lo[k]) lo[k] = x;
                if (x > hi[k]) hi[k] = x;
            }
        if (!count)
            for (int k = 0; k < comps; k++) lo[k] = hi[k] = 0;
    }
    return accessor(g, view(g, v, count * (size_t)comps * sizeof(float), 34962), GLB_FLOAT, count, comps,
                    minmax ? lo : NULL, minmax ? hi : NULL);
}

int glb_indices(Glb *g, const uint32_t *idx, size_t count) {
    return accessor(g, view(g, idx, count * sizeof(uint32_t), 34963), GLB_UINT, count, 1, NULL, NULL);
}

int glb_texture_png(Glb *g, const unsigned char *png, size_t len) {
    int bv = view(g, png, len, 0);
    JsonValue *img = json_object(), *smp = json_object(), *tex = json_object();
    if (bv < 0 || !img || !smp || !tex) {
        json_free(img), json_free(smp), json_free(tex);
        return g->failed = true, -1;
    }
    json_set_int(img, "bufferView", bv);
    json_set_string(img, "mimeType", "image/png");
    json_set_int(smp, "magFilter", 9729), json_set_int(smp, "minFilter", 9729); /* linear */
    json_set_int(smp, "wrapS", 33071), json_set_int(smp, "wrapT", 33071);       /* clamp to edge */
    json_set_int(tex, "source", add(g, "images", img));
    json_set_int(tex, "sampler", add(g, "samplers", smp));
    return add(g, "textures", tex);
}

int glb_material(Glb *g, JsonValue *material) { return add(g, "materials", material); }

JsonValue *glb_mesh(Glb *g, const char *name, int *mesh) {
    JsonValue *m = json_object(), *prims = m ? json_set_array(m, "primitives") : NULL;
    if (!prims) {
        json_free(m);
        g->failed = true;
        *mesh = -1;
        return NULL;
    }
    if (name) json_set_string(m, "name", name);
    *mesh = add(g, "meshes", m);
    return prims;
}

JsonValue *glb_primitive(JsonValue *primitives, int mode, int indices, int material) {
    JsonValue *p = json_object(), *attr = p ? json_set_object(p, "attributes") : NULL;
    if (!attr || !primitives || !json_push(primitives, p)) {
        if (p && (!attr || !primitives)) json_free(p);
        return NULL; /* glb_finish then finds a primitive without attributes and fails */
    }
    json_set_int(p, "mode", mode);
    if (indices >= 0) json_set_int(p, "indices", indices);
    if (material >= 0) json_set_int(p, "material", material);
    return attr;
}

int glb_node(Glb *g, const char *name, int mesh, JsonValue *extras) {
    JsonValue *n = json_object();
    if (!n) {
        json_free(extras);
        return g->failed = true, -1;
    }
    if (name) json_set_string(n, "name", name);
    if (mesh >= 0) json_set_int(n, "mesh", mesh);
    if (extras && !json_set(n, "extras", extras)) g->failed = true;
    int i = add(g, "nodes", n);
    JsonValue *nodes = json_get(json_at(json_get(g->root, "scenes"), 0), "nodes");
    if (i < 0 || !nodes || !json_push(nodes, json_number(i))) g->failed = true;
    return i;
}

static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); } /* little endian: the hosts this builds on */

bool glb_finish(Glb *g, unsigned char **out, size_t *len) {
    *out = NULL, *len = 0;
    if (!g->root || g->failed) return false;
    JsonValue *b = json_object();
    if (!b || !json_set_int(b, "byteLength", (long long)g->len)) return json_free(b), false;
    if (add(g, "buffers", b) != 0) return false;
    size_t jn = 0;
    bool nonfinite = false;
    char *js = json_dump(g->root, 0, &jn, &nonfinite);
    if (!js || nonfinite) return free(js), false; /* a NaN in a min, max or extra would be written as null */
    size_t jpad = (jn + 3) & ~(size_t)3, total = 12 + 8 + jpad + 8 + g->len;
    unsigned char *f = malloc(total);
    if (!f) return free(js), false;
    put32(f, 0x46546C67u), put32(f + 4, 2), put32(f + 8, (uint32_t)total); /* "glTF", version 2, length */
    put32(f + 12, (uint32_t)jpad), put32(f + 16, 0x4E4F534Au);             /* JSON chunk */
    memcpy(f + 20, js, jn);
    memset(f + 20 + jn, ' ', jpad - jn);
    put32(f + 20 + jpad, (uint32_t)g->len), put32(f + 24 + jpad, 0x004E4942u); /* BIN chunk */
    if (g->len) memcpy(f + 28 + jpad, g->bin, g->len);
    free(js);
    *out = f, *len = total;
    return true;
}

uint16_t glb_half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u, mant = x & 0x7FFFFFu;
    int exp = (int)((x >> 23) & 0xFF);
    if (exp == 0xFF) return (uint16_t)(sign | 0x7C00u | (mant ? 0x200u : 0)); /* Inf, NaN */
    exp = exp - 127 + 15;
    if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u); /* too large: infinity */
    if (exp <= 0) {                                    /* a subnormal half, or zero */
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        int shift = 14 - exp;
        uint32_t h = mant >> shift, rem = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1))) h++;
        return (uint16_t)(sign | h);
    }
    uint32_t h = ((uint32_t)exp << 10) | (mant >> 13), rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) h++; /* a carry into the exponent is the right rounding */
    return (uint16_t)(sign | h);
}

float glb_half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu, x;
    if (exp == 0x1F) x = sign | 0x7F800000u | (mant << 13);
    else if (exp) x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    else if (!mant) x = sign;
    else { /* subnormal: normalise */
        int e = -1;
        do mant <<= 1, e++;
        while (!(mant & 0x400u));
        x = sign | ((uint32_t)(127 - 15 - e) << 23) | ((mant & 0x3FFu) << 13);
    }
    float f;
    memcpy(&f, &x, 4);
    return f;
}
