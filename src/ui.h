/* ui.h - immediate-mode 2D UI (panels, text, buttons, sliders, plots) drawn with OpenGL.
 * All coordinates are in points (origin top-left); fonts are rasterised at the backing scale. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "glutil.h"

typedef enum { FONT_MONO = 0, FONT_SMALL, FONT_BOLD, FONT_TITLE, FONT_DISPLAY, FONT_HERO, FONT_COUNT } UiFont;

typedef struct UiInput {
    float mx, my;
    bool down[3], pressed[3], released[3];
    int clicks;
    float wheel;
    uint32_t mods;
} UiInput;

typedef struct Ui Ui;

Ui *ui_create(float scale);
void ui_destroy(Ui *ui);

void ui_begin(Ui *ui, int fb_w, int fb_h, float scale, const UiInput *in);
void ui_end(Ui *ui, GLuint target_fbo);

/* true when the pointer is over a widget/panel registered this frame or a widget is being dragged */
bool ui_wants_mouse(const Ui *ui);
bool ui_wants_mouse_at(const Ui *ui, float x, float y);

/* press-activated region, registered like a widget so automated UI tests can find it */
bool ui_clickable(Ui *ui, const char *label, float x, float y, float w, float h);
/* a thin vertical scrollbar: the thumb is dragged or the track clicked; *pos runs from 0 (the top) to max, view is the
 * visible height in the same units; true when it moved */
bool ui_vscroll(Ui *ui, const char *id, float x, float y, float w, float h, float *pos, float max, float view);

/* Widgets drawn during the previous frame (automated UI tests / debugging). */
typedef struct UiWidgetInfo {
    char label[64]; /* visible label (the id when there is none) */
    char id[64];
    float x, y, w, h;
    int kind; /* 0 button, 1 slider, 2 clickable region */
} UiWidgetInfo;
int ui_widget_count(const Ui *ui);
/* widget under a point in the frame just drawn (cursor feedback) */
const UiWidgetInfo *ui_widget_at(const Ui *ui, float x, float y);
const UiWidgetInfo *ui_widget(const Ui *ui, int index);
/* exact (case-insensitive) label or id match first, then label substring */
const UiWidgetInfo *ui_find_widget(const Ui *ui, const char *label);
void ui_block_mouse(Ui *ui, float x, float y, float w, float h); /* registers an opaque region */

void ui_rect(Ui *ui, float x, float y, float w, float h, uint32_t rgba, float radius);
void ui_rect_grad(Ui *ui, float x, float y, float w, float h, uint32_t top, uint32_t bottom, float radius);
void ui_rect_outline(Ui *ui, float x, float y, float w, float h, uint32_t rgba, float radius, float thickness);
void ui_line(Ui *ui, float x0, float y0, float x1, float y1, float thickness, uint32_t rgba);
void ui_colormap_bar(Ui *ui, float x, float y, float w, float h, GLuint cmap_tex, float radius);
void ui_shadow(Ui *ui, float x, float y, float w, float h, float radius, float blur, uint32_t rgba);
void ui_image(Ui *ui, GLuint tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, float radius,
              uint32_t rgba);
void ui_sentence_case(char *s);
void ui_push_clip(Ui *ui, float x, float y, float w, float h);
void ui_pop_clip(Ui *ui);

/* y is the top of the text line. Returns the advance width. */
float ui_text(Ui *ui, int font, float x, float y, uint32_t rgba, const char *s);
float ui_textf(Ui *ui, int font, float x, float y, uint32_t rgba, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
float ui_text_right(Ui *ui, int font, float right_x, float y, uint32_t rgba, const char *s);
float ui_text_width(Ui *ui, int font, const char *s);
float ui_text_width_n(Ui *ui, int font, const char *s, int nbytes);
float ui_line_height(Ui *ui, int font);
float ui_char_width(Ui *ui, int font);

bool ui_hover(Ui *ui, float x, float y, float w, float h);
bool ui_clicked(Ui *ui, float x, float y, float w, float h);
/* id: any string; text after "##" is not displayed */
bool ui_button(Ui *ui, const char *id_label, float x, float y, float w, float h, bool active);
bool ui_slider(Ui *ui, const char *id, float x, float y, float w, float h, double *value, double lo, double hi,
               bool logarithmic);
void ui_plot(Ui *ui, float x, float y, float w, float h, const float *values, int n, float lo, float hi, uint32_t rgba,
             float thickness);

/* Palette */
#define UI_BG        0xFFFFFFF0u
#define UI_PANEL     0xF7F6F3E6u
#define UI_PANEL2    0xEFEDE9F0u
#define UI_BORDER    0xB4B1AB55u
#define UI_TEXT      0x111111FFu
#define UI_DIM       0x7A7A80FFu
#define UI_FAINT     0xB4B1ABFFu
#define UI_ACCENT    0xE8551CFFu
#define UI_ACCENT2   0xB0700AFFu
#define UI_GOOD      0x1E8E4EFFu
#define UI_BAD       0xD23B3BFFu
#define UI_VIOLET    0x6E56CFFFu
/* studio theme: the page behind panels, black for primary actions and chosen states, a hairline for separators */
#define UI_PAGE      0xF2F1EEFFu
#define UI_INK       0x111111FFu
#define UI_HAIRLINE  0xE3E1DCFFu
#define UI_CARD      0xFFFFFFFFu
