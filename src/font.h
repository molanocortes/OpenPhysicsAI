/* font.h - glyph atlas rasterised with CoreText */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct Glyph {
    float x0, y0, x1, y1; /* quad relative to pen on the baseline, pixels, y grows downward */
    float u0, v0, u1, v1;
    float advance;
} Glyph;

#define FONT_MAX_EXTRA 160

typedef struct Font {
    char name[64];
    float px;
    float ascent, descent, line_height;
    int atlas_w, atlas_h;
    uint8_t *atlas; /* coverage, atlas_w * atlas_h */
    Glyph ascii[128];
    uint32_t extra_cp[FONT_MAX_EXTRA];
    Glyph extra[FONT_MAX_EXTRA];
    int n_extra;
    unsigned int texture; /* GL texture, owned by the renderer */
} Font;

/* Tries each PostScript name in the comma-separated list until one exists (e.g. "SFMono-Regular,Menlo-Regular"). */
bool font_build(Font *f, const char *names, float px);
void font_free(Font *f);
const Glyph *font_glyph(const Font *f, uint32_t cp);
float font_text_width(const Font *f, const char *utf8);
float font_text_width_n(const Font *f, const char *utf8, int nbytes);
uint32_t utf8_decode(const char **s);
