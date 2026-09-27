/* ui.c - immediate-mode UI renderer and widgets */
#include "ui.h"
#include "common.h"
#include "font.h"

#include <stdarg.h>

typedef struct {
    float x, y, u, v;
    float r0, r1, r2, r3; /* rect centre + half size, or segment endpoints */
    float radius, mode;
    float cr, cg, cb, ca;
} UiVert;

typedef struct {
    int start, count;
    GLuint font_tex, cmap_tex;
    int clip[4];
    bool clip_on;
} UiCmd;

#define MAX_BLOCKS 128
#define MAX_CLIP 16
#define MAX_WIDGETS 128

struct Ui {
    Font fonts[FONT_COUNT];
    GLuint font_tex[FONT_COUNT];
    float font_scale;

    GLuint prog, vao, vbo;
    UiVert *verts;
    int nverts, cap;
    UiCmd *cmds;
    int ncmds, cmd_cap;
    GLuint cur_font, cur_cmap;

    int fb_w, fb_h;
    float scale;
    UiInput in;
    uint32_t hot, active;
    bool active_seen;

    float blocks[MAX_BLOCKS][4], prev_blocks[MAX_BLOCKS][4];
    int nblocks, nprev;
    float clip[MAX_CLIP][4];
    int nclip;

    UiWidgetInfo widgets[MAX_WIDGETS], prev_widgets[MAX_WIDGETS];
    int nwidgets, nprev_widgets;
};

static void record_widget(Ui *ui, const char *id, const char *label, float x, float y, float w, float h, int kind) {
    if (ui->nwidgets >= MAX_WIDGETS) return;
    UiWidgetInfo *wi = &ui->widgets[ui->nwidgets++];
    str_copy(wi->id, sizeof wi->id, id);
    str_copy(wi->label, sizeof wi->label, label && label[0] ? label : id);
    wi->x = x, wi->y = y, wi->w = w, wi->h = h;
    wi->kind = kind;
}

static const char *UI_VS = "#version 410 core\n"
    "layout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv; layout(location=2) in vec4 a_rect;\n"
    "layout(location=3) in vec2 a_rm; layout(location=4) in vec4 a_col;\n"
    "uniform vec2 u_res; out vec2 v_pos; out vec2 v_uv; out vec4 v_rect; out vec2 v_rm; out vec4 v_col;\n"
    "void main() { v_pos = a_pos; v_uv = a_uv; v_rect = a_rect; v_rm = a_rm; v_col = a_col;\n"
    "  gl_Position = vec4(a_pos.x / u_res.x * 2.0 - 1.0, 1.0 - a_pos.y / u_res.y * 2.0, 0.0, 1.0); }\n";

static const char *UI_FS = "#version 410 core\n"
    "in vec2 v_pos; in vec2 v_uv; in vec4 v_rect; in vec2 v_rm; in vec4 v_col; out vec4 o;\n"
    "uniform sampler2D u_font, u_cmap;\n"
    "float rbox(vec2 p, vec2 c, vec2 hs, float r) { vec2 q = abs(p - c) - (hs - vec2(r));\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }\n"
    "void main() {\n"
    "  vec4 c = v_col; float m = v_rm.y;\n"
    "  if (m < 0.5) { c.a *= clamp(0.5 - rbox(v_pos, v_rect.xy, v_rect.zw, v_rm.x), 0.0, 1.0); }\n"
    "  else if (m < 1.5) { float a = texture(u_font, v_uv).r; c.a *= pow(a, 0.85); }\n"
    "  else if (m < 2.5) { c.rgb = texture(u_cmap, vec2(v_uv.x, 0.5)).rgb;\n"
    "    c.a *= clamp(0.5 - rbox(v_pos, v_rect.xy, v_rect.zw, v_rm.x), 0.0, 1.0); }\n"
    "  else if (m < 3.5) { float d = rbox(v_pos, v_rect.xy, v_rect.zw, v_rm.x);\n"
    "    c.a *= clamp(0.5 - d, 0.0, 1.0) * clamp(0.5 + d + v_uv.x, 0.0, 1.0); }\n"
    "  else { vec2 a = v_rect.xy, b = v_rect.zw, pa = v_pos - a, ba = b - a;\n"
    "    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);\n"
    "    c.a *= clamp(0.5 - (length(pa - ba * h) - v_rm.x), 0.0, 1.0); }\n"
    "  o = vec4(c.rgb * c.a, c.a); }\n";

static const char *FONT_NAMES[FONT_COUNT] = {"SFMono-Regular,Menlo-Regular", "SFMono-Regular,Menlo-Regular",
                                             "SFMono-Semibold,Menlo-Bold", "SFMono-Bold,Menlo-Bold"};
static const float FONT_PT[FONT_COUNT] = {12.0f, 10.5f, 12.0f, 15.0f};

static void build_fonts(Ui *ui, float scale) {
    for (int i = 0; i < FONT_COUNT; i++) {
        if (ui->font_tex[i]) glDeleteTextures(1, &ui->font_tex[i]);
        font_free(&ui->fonts[i]);
        font_build(&ui->fonts[i], FONT_NAMES[i], FONT_PT[i] * scale);
        ui->font_tex[i] = gl_texture_2d(ui->fonts[i].atlas_w, ui->fonts[i].atlas_h, GL_R8, GL_RED, GL_UNSIGNED_BYTE,
                                        ui->fonts[i].atlas, true);
    }
    ui->font_scale = scale;
}

Ui *ui_create(float scale) {
    Ui *ui = calloc(1, sizeof *ui);
    ui->prog = gl_program("ui", UI_VS, NULL, UI_FS);
    glGenVertexArrays(1, &ui->vao);
    glGenBuffers(1, &ui->vbo);
    glBindVertexArray(ui->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui->vbo);
    GLsizei st = sizeof(UiVert);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, st, (void *)offsetof(UiVert, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, (void *)offsetof(UiVert, u));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, (void *)offsetof(UiVert, r0));
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, (void *)offsetof(UiVert, radius));
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, st, (void *)offsetof(UiVert, cr));
    for (int i = 0; i < 5; i++) glEnableVertexAttribArray((GLuint)i);
    glBindVertexArray(0);
    build_fonts(ui, scale);
    return ui;
}

void ui_destroy(Ui *ui) {
    if (!ui) return;
    for (int i = 0; i < FONT_COUNT; i++) {
        if (ui->font_tex[i]) glDeleteTextures(1, &ui->font_tex[i]);
        font_free(&ui->fonts[i]);
    }
    glDeleteProgram(ui->prog);
    glDeleteBuffers(1, &ui->vbo);
    glDeleteVertexArrays(1, &ui->vao);
    free(ui->verts);
    free(ui->cmds);
    free(ui);
}

void ui_begin(Ui *ui, int fb_w, int fb_h, float scale, const UiInput *in) {
    if (fabsf(scale - ui->font_scale) > 0.01f) build_fonts(ui, scale);
    ui->fb_w = fb_w, ui->fb_h = fb_h, ui->scale = scale;
    ui->in = *in;
    ui->nverts = 0;
    ui->ncmds = 0;
    ui->cur_font = ui->font_tex[FONT_MONO];
    ui->cur_cmap = 0;
    memcpy(ui->prev_blocks, ui->blocks, sizeof ui->blocks);
    ui->nprev = ui->nblocks;
    ui->nblocks = 0;
    memcpy(ui->prev_widgets, ui->widgets, (size_t)ui->nwidgets * sizeof(UiWidgetInfo));
    ui->nprev_widgets = ui->nwidgets;
    ui->nwidgets = 0;
    ui->nclip = 0;
    if (!ui->active_seen) ui->active = 0;
    ui->active_seen = false;
    /* note: 'active' must survive into this frame even when the button is already up, so the widget that owns
     * the press can see the release; it is cleared in ui_end once widgets have run */
    ui->hot = 0;
}

static UiCmd *current_cmd(Ui *ui, GLuint font_tex, GLuint cmap_tex) {
    int clip[4] = {0, 0, 0, 0};
    bool clip_on = ui->nclip > 0;
    if (clip_on) {
        const float *c = ui->clip[ui->nclip - 1];
        clip[0] = (int)floorf(c[0] * ui->scale);
        clip[1] = (int)floorf((float)ui->fb_h - (c[1] + c[3]) * ui->scale);
        clip[2] = (int)ceilf(c[2] * ui->scale);
        clip[3] = (int)ceilf(c[3] * ui->scale);
    }
    UiCmd *last = ui->ncmds ? &ui->cmds[ui->ncmds - 1] : NULL;
    bool same = last && last->clip_on == clip_on && (!clip_on || memcmp(last->clip, clip, sizeof clip) == 0) &&
                (font_tex == 0 || last->font_tex == font_tex) && (cmap_tex == 0 || last->cmap_tex == cmap_tex);
    if (same) {
        if (font_tex) last->font_tex = font_tex;
        if (cmap_tex) last->cmap_tex = cmap_tex;
        return last;
    }
    if (ui->ncmds == ui->cmd_cap) {
        ui->cmd_cap = ui->cmd_cap ? ui->cmd_cap * 2 : 64;
        ui->cmds = realloc(ui->cmds, (size_t)ui->cmd_cap * sizeof(UiCmd));
    }
    UiCmd *c = &ui->cmds[ui->ncmds++];
    c->start = ui->nverts;
    c->count = 0;
    c->font_tex = font_tex ? font_tex : (last ? last->font_tex : ui->font_tex[FONT_MONO]);
    c->cmap_tex = cmap_tex ? cmap_tex : (last ? last->cmap_tex : 0);
    memcpy(c->clip, clip, sizeof clip);
    c->clip_on = clip_on;
    return c;
}

static void push_quad(Ui *ui, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                      float rect[4], float radius, float mode, uint32_t rgba, GLuint font_tex, GLuint cmap_tex) {
    UiCmd *cmd = current_cmd(ui, font_tex, cmap_tex);
    if (ui->nverts + 6 > ui->cap) {
        ui->cap = ui->cap ? ui->cap * 2 : 8192;
        ui->verts = realloc(ui->verts, (size_t)ui->cap * sizeof(UiVert));
    }
    float cr = ((rgba >> 24) & 0xFF) / 255.0f, cg = ((rgba >> 16) & 0xFF) / 255.0f;
    float cb = ((rgba >> 8) & 0xFF) / 255.0f, ca = (rgba & 0xFF) / 255.0f;
    float xs[6] = {x0, x1, x1, x0, x1, x0}, ys[6] = {y0, y0, y1, y0, y1, y1};
    float us[6] = {u0, u1, u1, u0, u1, u0}, vs[6] = {v0, v0, v1, v0, v1, v1};
    for (int i = 0; i < 6; i++) {
        UiVert *v = &ui->verts[ui->nverts++];
        v->x = xs[i], v->y = ys[i], v->u = us[i], v->v = vs[i];
        v->r0 = rect[0], v->r1 = rect[1], v->r2 = rect[2], v->r3 = rect[3];
        v->radius = radius, v->mode = mode;
        v->cr = cr, v->cg = cg, v->cb = cb, v->ca = ca;
    }
    cmd->count += 6;
}

void ui_rect(Ui *ui, float x, float y, float w, float h, uint32_t rgba, float radius) {
    float s = ui->scale;
    float rect[4] = {(x + w * 0.5f) * s, (y + h * 0.5f) * s, w * 0.5f * s, h * 0.5f * s};
    float r = MINI(radius * s, MINI(rect[2], rect[3]));
    push_quad(ui, x * s - 1, y * s - 1, (x + w) * s + 1, (y + h) * s + 1, 0, 0, 0, 0, rect, r, 0, rgba, 0, 0);
}

void ui_rect_grad(Ui *ui, float x, float y, float w, float h, uint32_t top, uint32_t bottom, float radius) {
    ui_rect(ui, x, y, w, h, top, radius);
    /* recolour the bottom vertices of the quad just pushed */
    float cr = ((bottom >> 24) & 0xFF) / 255.0f, cg = ((bottom >> 16) & 0xFF) / 255.0f;
    float cb = ((bottom >> 8) & 0xFF) / 255.0f, ca = (bottom & 0xFF) / 255.0f;
    UiVert *v = ui->verts + ui->nverts - 6;
    int bottom_idx[] = {2, 4, 5};
    for (int i = 0; i < 3; i++) {
        UiVert *q = &v[bottom_idx[i]];
        q->cr = cr, q->cg = cg, q->cb = cb, q->ca = ca;
    }
}

void ui_rect_outline(Ui *ui, float x, float y, float w, float h, uint32_t rgba, float radius, float thickness) {
    float s = ui->scale;
    float rect[4] = {(x + w * 0.5f) * s, (y + h * 0.5f) * s, w * 0.5f * s, h * 0.5f * s};
    float r = MINI(radius * s, MINI(rect[2], rect[3]));
    push_quad(ui, x * s - 1, y * s - 1, (x + w) * s + 1, (y + h) * s + 1, thickness * s, 0, thickness * s, 0, rect, r,
              3, rgba, 0, 0);
}

void ui_line(Ui *ui, float x0, float y0, float x1, float y1, float thickness, uint32_t rgba) {
    float s = ui->scale, ht = thickness * 0.5f * s;
    float rect[4] = {x0 * s, y0 * s, x1 * s, y1 * s};
    float pad = ht + 1.5f;
    push_quad(ui, MINI(x0, x1) * s - pad, MINI(y0, y1) * s - pad, MAXI(x0, x1) * s + pad, MAXI(y0, y1) * s + pad, 0, 0,
              0, 0, rect, ht, 4, rgba, 0, 0);
}

void ui_colormap_bar(Ui *ui, float x, float y, float w, float h, GLuint cmap_tex, float radius) {
    float s = ui->scale;
    float rect[4] = {(x + w * 0.5f) * s, (y + h * 0.5f) * s, w * 0.5f * s, h * 0.5f * s};
    push_quad(ui, x * s, y * s, (x + w) * s, (y + h) * s, 0, 0.5f, 1, 0.5f, rect, MINI(radius * s, rect[3]), 2,
              0xFFFFFFFFu, 0, cmap_tex);
}

void ui_push_clip(Ui *ui, float x, float y, float w, float h) {
    if (ui->nclip >= MAX_CLIP) return;
    if (ui->nclip > 0) {
        const float *p = ui->clip[ui->nclip - 1];
        float x1 = MINI(x + w, p[0] + p[2]), y1 = MINI(y + h, p[1] + p[3]);
        x = MAXI(x, p[0]), y = MAXI(y, p[1]);
        w = MAXI(x1 - x, 0.0f), h = MAXI(y1 - y, 0.0f);
    }
    float *c = ui->clip[ui->nclip++];
    c[0] = x, c[1] = y, c[2] = w, c[3] = h;
}

void ui_pop_clip(Ui *ui) {
    if (ui->nclip > 0) ui->nclip--;
}

float ui_text_width_n(Ui *ui, int font, const char *s, int nbytes) {
    return font_text_width_n(&ui->fonts[font], s, nbytes) / ui->scale;
}
float ui_text_width(Ui *ui, int font, const char *s) { return ui_text_width_n(ui, font, s, -1); }
float ui_line_height(Ui *ui, int font) { return ui->fonts[font].line_height / ui->scale; }
float ui_char_width(Ui *ui, int font) { return ui->fonts[font].ascii['M'].advance / ui->scale; }

float ui_text(Ui *ui, int font, float x, float y, uint32_t rgba, const char *s) {
    const Font *f = &ui->fonts[font];
    float px = roundf(x * ui->scale);
    float base = roundf(y * ui->scale + (f->line_height - (f->ascent + f->descent)) * 0.5f + f->ascent);
    float start = px;
    float rect[4] = {0, 0, 0, 0};
    while (*s) {
        uint32_t cp = utf8_decode(&s);
        const Glyph *g = font_glyph(f, cp);
        if (g->x1 > g->x0) {
            push_quad(ui, px + g->x0, base + g->y0, px + g->x1, base + g->y1, g->u0, g->v0, g->u1, g->v1, rect, 0, 1,
                      rgba, ui->font_tex[font], 0);
        }
        px += roundf(g->advance);
    }
    return (px - start) / ui->scale;
}

float ui_textf(Ui *ui, int font, float x, float y, uint32_t rgba, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return ui_text(ui, font, x, y, rgba, buf);
}

float ui_text_right(Ui *ui, int font, float right_x, float y, uint32_t rgba, const char *s) {
    float w = ui_text_width(ui, font, s);
    return ui_text(ui, font, right_x - w, y, rgba, s);
}

void ui_block_mouse(Ui *ui, float x, float y, float w, float h) {
    if (ui->nblocks >= MAX_BLOCKS) return;
    float *b = ui->blocks[ui->nblocks++];
    b[0] = x, b[1] = y, b[2] = w, b[3] = h;
}

bool ui_wants_mouse(const Ui *ui) {
    if (ui->active) return true;
    for (int i = 0; i < ui->nprev; i++) {
        const float *b = ui->prev_blocks[i];
        if (ui->in.mx >= b[0] && ui->in.mx < b[0] + b[2] && ui->in.my >= b[1] && ui->in.my < b[1] + b[3]) return true;
    }
    return false;
}

bool ui_hover(Ui *ui, float x, float y, float w, float h) {
    if (ui->nclip > 0) {
        const float *c = ui->clip[ui->nclip - 1];
        if (ui->in.mx < c[0] || ui->in.mx >= c[0] + c[2] || ui->in.my < c[1] || ui->in.my >= c[1] + c[3]) return false;
    }
    return ui->in.mx >= x && ui->in.mx < x + w && ui->in.my >= y && ui->in.my < y + h;
}

bool ui_clicked(Ui *ui, float x, float y, float w, float h) { return ui->in.pressed[0] && ui_hover(ui, x, y, w, h); }

static uint32_t hash_id(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h ? h : 1;
}

bool ui_button(Ui *ui, const char *id_label, float x, float y, float w, float h, bool active) {
    uint32_t id = hash_id(id_label);
    bool hover = ui_hover(ui, x, y, w, h);
    bool clicked = false;
    if (hover) ui->hot = id;
    if (hover && ui->in.pressed[0]) ui->active = id;
    if (ui->active == id) {
        ui->active_seen = true;
        if (ui->in.released[0]) {
            clicked = hover;
            ui->active = 0;
        }
    }
    bool held = ui->active == id && ui->in.down[0];
    uint32_t bg = active ? 0x1B4A5EFFu : (held ? 0x1A2A38FFu : (hover ? 0x16222EFFu : 0x0F1822FFu));
    uint32_t border = active ? 0x38E1FFB0u : (hover ? 0x3F6A85A0u : 0x243A4C90u);
    ui_rect(ui, x, y, w, h, bg, 5);
    ui_rect_outline(ui, x, y, w, h, border, 5, 1);
    if (active) ui_rect(ui, x + 6, y + h - 2.5f, w - 12, 1.5f, 0x38E1FFFFu, 1);
    char label[128];
    str_copy(label, sizeof label, id_label);
    char *hh = strstr(label, "##");
    if (hh) *hh = 0;
    record_widget(ui, id_label, label, x, y, w, h, 0);
    float tw = ui_text_width(ui, FONT_SMALL, label);
    float lh = ui_line_height(ui, FONT_SMALL);
    ui_text(ui, FONT_SMALL, roundf(x + (w - tw) * 0.5f), roundf(y + (h - lh) * 0.5f),
            active ? 0xE8FBFFFFu : (hover ? 0xD6E6F2FFu : 0xA9BCCBFFu), label);
    return clicked;
}

bool ui_slider(Ui *ui, const char *id_str, float x, float y, float w, float h, double *value, double lo, double hi,
               bool logarithmic) {
    uint32_t id = hash_id(id_str);
    record_widget(ui, id_str, id_str, x, y, w, h, 1);
    bool hover = ui_hover(ui, x, y - 3, w, h + 6);
    if (hover) ui->hot = id;
    if (hover && ui->in.pressed[0]) ui->active = id;
    bool changed = false;
    double v = CLAMP(*value, lo, hi);
    double llo = logarithmic ? log(MAXI(lo, 1e-30)) : lo, lhi = logarithmic ? log(MAXI(hi, 1e-30)) : hi;
    double t = logarithmic ? (log(MAXI(v, 1e-30)) - llo) / (lhi - llo) : (v - lo) / (hi - lo);
    if (ui->active == id) {
        ui->active_seen = true;
        double nt = CLAMP((ui->in.mx - x) / w, 0.0, 1.0);
        double nv = logarithmic ? exp(llo + nt * (lhi - llo)) : lo + nt * (hi - lo);
        if (nv != *value) {
            *value = nv;
            changed = true;
        }
        t = nt;
        if (ui->in.released[0]) ui->active = 0;
    }
    t = CLAMP(t, 0.0, 1.0);
    float cy = y + h * 0.5f;
    ui_rect(ui, x, cy - 2, w, 4, 0x1A2733FFu, 2);
    ui_rect(ui, x, cy - 2, (float)(w * t), 4, ui->active == id ? 0x5FF0FFFFu : 0x2FB8D8FFu, 2);
    float kx = x + (float)(w * t);
    ui_rect(ui, kx - 5, cy - 5, 10, 10, hover || ui->active == id ? 0xE8FBFFFFu : 0xB8CCD8FFu, 5);
    return changed;
}

void ui_plot(Ui *ui, float x, float y, float w, float h, const float *values, int n, float lo, float hi, uint32_t rgba,
             float thickness) {
    if (n < 2 || hi <= lo) return;
    int step = n > (int)(w * ui->scale) ? n / (int)(w * ui->scale) : 1;
    float px = 0, py = 0;
    bool first = true;
    for (int i = 0; i < n; i += step) {
        float t = CLAMP((values[i] - lo) / (hi - lo), 0.0f, 1.0f);
        float cx = x + w * (float)i / (float)(n - 1), cy = y + h * (1.0f - t);
        if (!first) ui_line(ui, px, py, cx, cy, thickness, rgba);
        px = cx, py = cy;
        first = false;
    }
}

bool ui_clickable(Ui *ui, const char *label, float x, float y, float w, float h) {
    record_widget(ui, label, label, x, y, w, h, 2);
    return ui->in.pressed[0] && ui_hover(ui, x, y, w, h);
}

bool ui_wants_mouse_at(const Ui *ui, float x, float y) {
    if (ui->active) return true;
    for (int i = 0; i < ui->nprev; i++) {
        const float *b = ui->prev_blocks[i];
        if (x >= b[0] && x < b[0] + b[2] && y >= b[1] && y < b[1] + b[3]) return true;
    }
    return false;
}

int ui_widget_count(const Ui *ui) { return ui->nprev_widgets; }

const UiWidgetInfo *ui_widget_at(const Ui *ui, float x, float y) {
    const UiWidgetInfo *list = ui->nwidgets ? ui->widgets : ui->prev_widgets;
    int n = ui->nwidgets ? ui->nwidgets : ui->nprev_widgets;
    for (int i = n - 1; i >= 0; i--) {
        const UiWidgetInfo *w = &list[i];
        if (x >= w->x && x < w->x + w->w && y >= w->y - 3 && y < w->y + w->h + 3) return w;
    }
    return NULL;
}

const UiWidgetInfo *ui_widget(const Ui *ui, int index) {
    return (index >= 0 && index < ui->nprev_widgets) ? &ui->prev_widgets[index] : NULL;
}

static bool contains_i(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return false;
    for (; *hay; hay++)
        if (str_starts_with_i(hay, needle)) return true;
    return false;
}

/* Exact label or id first; otherwise a partial match, but only when it is unique, so that a script click cannot land on
 * a different control than the one it names (for example "LINE" while STREAMLINES is on screen). */
const UiWidgetInfo *ui_find_widget(const Ui *ui, const char *label) {
    for (int i = 0; i < ui->nprev_widgets; i++)
        if (str_ieq(ui->prev_widgets[i].label, label) || str_ieq(ui->prev_widgets[i].id, label))
            return &ui->prev_widgets[i];
    const UiWidgetInfo *found = NULL;
    for (int i = 0; i < ui->nprev_widgets; i++)
        if (contains_i(ui->prev_widgets[i].label, label)) {
            if (found) return NULL; /* ambiguous */
            found = &ui->prev_widgets[i];
        }
    return found;
}

void ui_end(Ui *ui, GLuint target_fbo) {
    if (!ui->in.down[0]) ui->active = 0;
    if (!ui->prog || ui->nverts == 0) return;
    glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
    glViewport(0, 0, ui->fb_w, ui->fb_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(ui->prog);
    glUniform2f(glGetUniformLocation(ui->prog, "u_res"), (float)ui->fb_w, (float)ui->fb_h);
    glUniform1i(glGetUniformLocation(ui->prog, "u_font"), 0);
    glUniform1i(glGetUniformLocation(ui->prog, "u_cmap"), 1);
    glBindVertexArray(ui->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)ui->nverts * sizeof(UiVert)), ui->verts, GL_STREAM_DRAW);
    for (int i = 0; i < ui->ncmds; i++) {
        const UiCmd *c = &ui->cmds[i];
        if (!c->count) continue;
        if (c->clip_on) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(c->clip[0], c->clip[1], c->clip[2], c->clip[3]);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, c->font_tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, c->cmap_tex ? c->cmap_tex : ui->font_tex[FONT_MONO]);
        glDrawArrays(GL_TRIANGLES, c->start, c->count);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
}
