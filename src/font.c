/* font.c - CoreText glyph atlas builder (pure C APIs: CoreText + CoreGraphics) */
#include "font.h"
#include "common.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

static const uint32_t EXTRA_CODEPOINTS[] = {
    0x00B0, 0x00B5, 0x00B2, 0x00B3, 0x00B7, 0x00D7, 0x00B1, 0x00BB, 0x00AB, 0x00B9, 0x2070, 0x2074, 0x2075,
    0x2076, 0x2077, 0x2078, 0x2079, 0x207B, 0x2080, 0x2081, 0x2082, 0x2083, 0x2084, 0x0394, 0x03A9, 0x03B1,
    0x03B2, 0x03B4, 0x03B5, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03C0, 0x03C1, 0x03C3, 0x03C4, 0x03C6, 0x03C9,
    0x2190, 0x2191, 0x2192, 0x2193, 0x2194, 0x21BA, 0x21BB, 0x21E7, 0x2318, 0x2325, 0x2303, 0x23CE, 0x232B,
    0x25B6, 0x25C0, 0x25B2, 0x25BC, 0x25B8, 0x25BE, 0x25A0, 0x25A1, 0x25CF, 0x25CB, 0x25C6, 0x25C7, 0x2022,
    0x2026, 0x2014, 0x2013, 0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534,
    0x253C, 0x2550, 0x2551, 0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588, 0x2591, 0x2592,
    0x2593, 0x258F, 0x258E, 0x258D, 0x258C, 0x258B, 0x258A, 0x2589, 0x2713, 0x2717, 0x26A0, 0x221E, 0x2248,
    0x2264, 0x2265, 0x221A, 0x2211, 0x2202, 0x2207, 0x2016, 0x2039, 0x203A, 0x2023, 0x2261, 0x2260, 0x00F7,
    0x2219, 0x25E6, 0x2B1D, 0x29BF, 0x23F5, 0x23F8, 0x23F9, 0x2699, 0x2316, 0x2295, 0x2297, 0x2609,
};

typedef struct {
    uint32_t cp;
    CTFontRef font; /* retained */
    CGGlyph glyph;
    int ox, oy, gw, gh; /* bitmap cell offsets relative to pen (CG coords, y up) */
    float adv;
    int ax, ay;         /* atlas position (top-left, y down) */
} Pending;

static CTFontRef create_font(const char *names, float px, char *used, size_t used_cap) {
    char buf[256];
    str_copy(buf, sizeof buf, names);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        CFStringRef cf = CFStringCreateWithCString(NULL, tok, kCFStringEncodingUTF8);
        CTFontRef font = CTFontCreateWithName(cf, px, NULL);
        CFRelease(cf);
        if (!font) continue;
        CFStringRef ps = CTFontCopyPostScriptName(font);
        char got[128] = {0};
        CFStringGetCString(ps, got, sizeof got, kCFStringEncodingUTF8);
        CFRelease(ps);
        if (strcmp(got, tok) == 0) {
            str_copy(used, used_cap, got);
            return font;
        }
        CFRelease(font);
    }
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, px, NULL);
    if (font) str_copy(used, used_cap, "system-monospace");
    return font;
}

static bool prepare_glyph(CTFontRef base, uint32_t cp, Pending *p) {
    UniChar u[2];
    CFIndex n = 1;
    if (cp > 0xFFFF) {
        uint32_t v = cp - 0x10000;
        u[0] = (UniChar)(0xD800 + (v >> 10));
        u[1] = (UniChar)(0xDC00 + (v & 0x3FF));
        n = 2;
    } else {
        u[0] = (UniChar)cp;
    }
    CGGlyph g[2] = {0, 0};
    CTFontRef use = base;
    CFRetain(use);
    if (!CTFontGetGlyphsForCharacters(use, u, g, n)) {
        CFRelease(use);
        CFStringRef s = CFStringCreateWithCharacters(NULL, u, n);
        use = CTFontCreateForString(base, s, CFRangeMake(0, n));
        CFRelease(s);
        if (!use || !CTFontGetGlyphsForCharacters(use, u, g, n)) {
            if (use) CFRelease(use);
            return false;
        }
    }
    CGRect bb = CTFontGetBoundingRectsForGlyphs(use, kCTFontOrientationHorizontal, g, NULL, 1);
    CGSize adv;
    CTFontGetAdvancesForGlyphs(use, kCTFontOrientationHorizontal, g, &adv, 1);
    const int pad = 2;
    p->cp = cp;
    p->font = use;
    p->glyph = g[0];
    p->adv = (float)adv.width;
    if (bb.size.width <= 0 || bb.size.height <= 0) {
        p->ox = p->oy = 0;
        p->gw = p->gh = 0;
    } else {
        int x0 = (int)floor(bb.origin.x), y0 = (int)floor(bb.origin.y);
        int x1 = (int)ceil(bb.origin.x + bb.size.width), y1 = (int)ceil(bb.origin.y + bb.size.height);
        p->ox = x0 - pad;
        p->oy = y0 - pad;
        p->gw = x1 - x0 + 2 * pad;
        p->gh = y1 - y0 + 2 * pad;
    }
    return true;
}

bool font_build(Font *f, const char *names, float px) {
    memset(f, 0, sizeof *f);
    CTFontRef font = create_font(names, px, f->name, sizeof f->name);
    if (!font) return false;
    f->px = px;
    f->ascent = (float)CTFontGetAscent(font);
    f->descent = (float)CTFontGetDescent(font);
    f->line_height = (float)ceil(CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font));

    int nextra = (int)ARRAY_LEN(EXTRA_CODEPOINTS);
    if (nextra > FONT_MAX_EXTRA) nextra = FONT_MAX_EXTRA;
    int total = 95 + nextra;
    Pending *pend = calloc((size_t)total, sizeof *pend);
    int np = 0;
    for (uint32_t c = 32; c < 127; c++)
        if (prepare_glyph(font, c, &pend[np])) np++;
    for (int i = 0; i < nextra; i++)
        if (prepare_glyph(font, EXTRA_CODEPOINTS[i], &pend[np])) np++;

    /* shelf packing; grow the atlas until everything fits */
    int W = 256;
    for (;;) {
        int x = 1, y = 1, shelf = 0;
        bool fit = true;
        for (int i = 0; i < np; i++) {
            Pending *p = &pend[i];
            if (x + p->gw + 1 > W) x = 1, y += shelf + 1, shelf = 0;
            if (y + p->gh + 1 > W) { fit = false; break; }
            p->ax = x, p->ay = y;
            x += p->gw + 1;
            if (p->gh > shelf) shelf = p->gh;
        }
        if (fit) break;
        W *= 2;
    }
    f->atlas_w = f->atlas_h = W;
    f->atlas = calloc((size_t)W * (size_t)W, 1);

    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(f->atlas, (size_t)W, (size_t)W, 8, (size_t)W, gray, kCGImageAlphaNone);
    CGContextSetGrayFillColor(ctx, 1.0, 1.0);
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetShouldSmoothFonts(ctx, false);
    CGContextSetShouldSubpixelPositionFonts(ctx, false);
    CGContextSetShouldSubpixelQuantizeFonts(ctx, false);

    for (int i = 0; i < np; i++) {
        Pending *p = &pend[i];
        Glyph gl = {0};
        gl.advance = p->adv;
        if (p->gw > 0) {
            int cell_bottom = W - (p->ay + p->gh); /* CG origin is bottom-left */
            CGPoint pos = {(CGFloat)(p->ax - p->ox), (CGFloat)(cell_bottom - p->oy)};
            CTFontDrawGlyphs(p->font, &p->glyph, &pos, 1, ctx);
            gl.x0 = (float)p->ox;
            gl.x1 = (float)(p->ox + p->gw);
            gl.y0 = -(float)(p->oy + p->gh);
            gl.y1 = -(float)p->oy;
            gl.u0 = (float)p->ax / (float)W;
            gl.u1 = (float)(p->ax + p->gw) / (float)W;
            gl.v0 = (float)p->ay / (float)W;
            gl.v1 = (float)(p->ay + p->gh) / (float)W;
        }
        if (p->cp < 128) {
            f->ascii[p->cp] = gl;
        } else if (f->n_extra < FONT_MAX_EXTRA) {
            f->extra_cp[f->n_extra] = p->cp;
            f->extra[f->n_extra++] = gl;
        }
        CFRelease(p->font);
    }
    CGContextRelease(ctx);
    CGColorSpaceRelease(gray);
    CFRelease(font);
    free(pend);
    return true;
}

void font_free(Font *f) {
    free(f->atlas);
    f->atlas = NULL;
}

const Glyph *font_glyph(const Font *f, uint32_t cp) {
    if (cp < 128) return &f->ascii[cp];
    for (int i = 0; i < f->n_extra; i++)
        if (f->extra_cp[i] == cp) return &f->extra[i];
    return &f->ascii['?'];
}

uint32_t utf8_decode(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = *s++;
    if (c >= 0xF0 && s[0] && s[1] && s[2]) {
        c = ((c & 7) << 18) | ((s[0] & 63u) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u);
        s += 3;
    } else if (c >= 0xE0 && s[0] && s[1]) {
        c = ((c & 15) << 12) | ((s[0] & 63u) << 6) | (s[1] & 63u);
        s += 2;
    } else if (c >= 0xC0 && s[0]) {
        c = ((c & 31) << 6) | (s[0] & 63u);
        s += 1;
    }
    *ps = (const char *)s;
    return c;
}

float font_text_width_n(const Font *f, const char *s, int nbytes) {
    const char *end = nbytes < 0 ? NULL : s + nbytes;
    float w = 0;
    while (*s && (!end || s < end)) w += font_glyph(f, utf8_decode(&s))->advance;
    return w;
}

float font_text_width(const Font *f, const char *s) { return font_text_width_n(f, s, -1); }
