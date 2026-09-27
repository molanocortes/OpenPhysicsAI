/* colormap.c - colour map tables (piecewise-linear stops in sRGB) */
#include "colormap.h"
#include "common.h"

#include <ctype.h>

#define MAX_STOPS 16

typedef struct {
    const char *name;
    int n;
    float pos[MAX_STOPS];
    uint32_t rgb[MAX_STOPS];
} Gradient;

static Gradient G[CMAP_COUNT] = {
    [CMAP_TURBO] = {"turbo", 0, {0}, {0}},
    [CMAP_CFD] = {"cfd", 7, {0, 0.17f, 0.34f, 0.5f, 0.66f, 0.83f, 1}, {0x1027a6, 0x1f78ff, 0x14e0ff, 0x3cf07a, 0xf5ee2a, 0xff8a14, 0xe8141c}},
    [CMAP_JET] = {"jet", 6, {0, 0.11f, 0.375f, 0.625f, 0.89f, 1}, {0x000080, 0x0000ff, 0x00ffff, 0xffff00, 0xff0000, 0x800000}},
    [CMAP_VIRIDIS] = {"viridis", 10, {0}, {0x440154, 0x482878, 0x3e4989, 0x31688e, 0x26828e, 0x1f9e89, 0x35b779, 0x6ece58, 0xb5de2b, 0xfde725}},
    [CMAP_PLASMA] = {"plasma", 10, {0}, {0x0d0887, 0x46039f, 0x7201a8, 0x9c179e, 0xbd3786, 0xd8576b, 0xed7953, 0xfb9f3a, 0xfdca26, 0xf0f921}},
    [CMAP_INFERNO] = {"inferno", 10, {0}, {0x000004, 0x1b0c41, 0x4a0c6b, 0x781c6d, 0xa52c60, 0xcf4446, 0xed6925, 0xfb9b06, 0xf7d13d, 0xfcffa4}},
    [CMAP_MAGMA] = {"magma", 10, {0}, {0x000004, 0x180f3d, 0x440f76, 0x721f81, 0x9e2f7f, 0xcd4071, 0xf1605d, 0xfd9668, 0xfeca8d, 0xfcfdbf}},
    [CMAP_COOLWARM] = {"coolwarm", 9, {0}, {0x3b4cc0, 0x5977e3, 0x7b9ff9, 0x9ebeff, 0xdddddd, 0xf5c4ad, 0xf49a7b, 0xde604d, 0xb40426}},
    [CMAP_HYDRO] = {"hydro", 7, {0}, {0x020617, 0x03045e, 0x0077b6, 0x00b4d8, 0x48cae4, 0x90e0ef, 0xe8fbff}},
    [CMAP_ICEFIRE] = {"icefire", 9, {0}, {0xbde7db, 0x5fb8d2, 0x3a73c2, 0x2d3a78, 0x161616, 0x6b2440, 0xc23c3c, 0xef8a4c, 0xfbe0a8}},
    [CMAP_GRAY] = {"gray", 2, {0, 1}, {0x101010, 0xf0f0f0}},
    [CMAP_CUSTOM] = {"custom", 3, {0, 0.5f, 1}, {0x001030, 0x00ffcc, 0xff4000}},
};

static void fix_positions(Gradient *g) {
    bool zero = true;
    for (int i = 1; i < g->n; i++)
        if (g->pos[i] != 0) zero = false;
    if (zero)
        for (int i = 0; i < g->n; i++) g->pos[i] = g->n > 1 ? (float)i / (float)(g->n - 1) : 0;
}

const char *colormap_name(int id) { return (id >= 0 && id < CMAP_COUNT) ? G[id].name : "?"; }

int colormap_find(const char *name) {
    for (int i = 0; i < CMAP_COUNT; i++)
        if (str_ieq(name, G[i].name)) return i;
    if (str_ieq(name, "rainbow")) return CMAP_CFD;
    if (str_ieq(name, "grey")) return CMAP_GRAY;
    return -1;
}

static float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float lin_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }

static void turbo(float x, float rgb[3]) {
    x = CLAMP(x, 0.0f, 1.0f);
    const float r4[4] = {0.13572138f, 4.61539260f, -42.66032258f, 132.13108234f};
    const float g4[4] = {0.09140261f, 2.19418839f, 4.84296658f, -14.18503333f};
    const float b4[4] = {0.10667330f, 12.64194608f, -60.58204836f, 110.36276771f};
    const float r2[2] = {-152.94239396f, 59.28637943f};
    const float g2[2] = {4.27729857f, 2.82956604f};
    const float b2[2] = {-89.90310912f, 27.34824973f};
    float v4[4] = {1, x, x * x, x * x * x};
    float v2[2] = {v4[2] * v4[2], v4[3] * v4[2]};
    rgb[0] = r4[0] * v4[0] + r4[1] * v4[1] + r4[2] * v4[2] + r4[3] * v4[3] + r2[0] * v2[0] + r2[1] * v2[1];
    rgb[1] = g4[0] * v4[0] + g4[1] * v4[1] + g4[2] * v4[2] + g4[3] * v4[3] + g2[0] * v2[0] + g2[1] * v2[1];
    rgb[2] = b4[0] * v4[0] + b4[1] * v4[1] + b4[2] * v4[2] + b4[3] * v4[3] + b2[0] * v2[0] + b2[1] * v2[1];
    for (int i = 0; i < 3; i++) rgb[i] = CLAMP(rgb[i], 0.0f, 1.0f);
}

void colormap_sample(int id, float t, float rgb[3]) {
    if (id < 0 || id >= CMAP_COUNT) id = CMAP_TURBO;
    if (!(t >= 0)) t = 0;
    if (t > 1) t = 1;
    if (id == CMAP_TURBO) {
        turbo(t, rgb);
        return;
    }
    Gradient *g = &G[id];
    fix_positions(g);
    int i = 0;
    while (i < g->n - 2 && t > g->pos[i + 1]) i++;
    float span = g->pos[i + 1] - g->pos[i];
    float s = span > 1e-6f ? (t - g->pos[i]) / span : 0;
    s = CLAMP(s, 0.0f, 1.0f);
    for (int c = 0; c < 3; c++) {
        float a = srgb_to_lin((float)((g->rgb[i] >> (16 - 8 * c)) & 0xFF) / 255.0f);
        float b = srgb_to_lin((float)((g->rgb[i + 1] >> (16 - 8 * c)) & 0xFF) / 255.0f);
        rgb[c] = lin_to_srgb(a + (b - a) * s);
    }
}

void colormap_fill_rgba(int id, uint8_t *rgba, int n) {
    for (int i = 0; i < n; i++) {
        float rgb[3];
        colormap_sample(id, n > 1 ? (float)i / (float)(n - 1) : 0, rgb);
        rgba[4 * i] = (uint8_t)lrintf(rgb[0] * 255);
        rgba[4 * i + 1] = (uint8_t)lrintf(rgb[1] * 255);
        rgba[4 * i + 2] = (uint8_t)lrintf(rgb[2] * 255);
        rgba[4 * i + 3] = 255;
    }
}

static bool parse_hex(const char *s, uint32_t *out) {
    if (*s == '#') s++;
    size_t n = 0;
    while (isxdigit((unsigned char)s[n])) n++;
    if (n != 6 && n != 3) return false;
    unsigned v = (unsigned)strtoul(s, NULL, 16);
    if (n == 3) v = ((v >> 8) & 0xF) * 0x110000u + ((v >> 4) & 0xF) * 0x1100u + (v & 0xF) * 0x11u;
    *out = v;
    return true;
}

bool colormap_set_custom(const char *spec, char *err, size_t errlen) {
    Gradient g = {"custom", 0, {0}, {0}};
    char buf[512];
    str_copy(buf, sizeof buf, spec);
    bool any_pos = false;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " ,;", &save); tok; tok = strtok_r(NULL, " ,;", &save)) {
        if (g.n >= MAX_STOPS) {
            snprintf(err, errlen, "at most %d colour stops", MAX_STOPS);
            return false;
        }
        char *colon = strchr(tok, ':');
        float pos = 0;
        char *col = tok;
        if (colon) {
            *colon = 0;
            pos = strtof(tok, NULL);
            col = colon + 1;
            any_pos = true;
        }
        uint32_t rgb;
        if (!parse_hex(col, &rgb)) {
            snprintf(err, errlen, "bad colour '%s' (use #rrggbb)", col);
            return false;
        }
        g.pos[g.n] = pos;
        g.rgb[g.n++] = rgb;
    }
    if (g.n < 2) {
        snprintf(err, errlen, "need at least two colours");
        return false;
    }
    if (!any_pos) {
        for (int i = 0; i < g.n; i++) g.pos[i] = (float)i / (float)(g.n - 1);
    } else {
        for (int i = 1; i < g.n; i++)
            if (g.pos[i] < g.pos[i - 1]) {
                snprintf(err, errlen, "stop positions must increase");
                return false;
            }
        if (g.pos[0] == 0 && g.pos[g.n - 1] == 0) g.pos[g.n - 1] = 1;
    }
    G[CMAP_CUSTOM] = g;
    return true;
}
