#include <dirent.h>
#include <sys/stat.h>
/* hud.c - toolbar, experiment panel, legend, overlays and terminal layout */
#include "app.h"
#include "labapp.h"
#include "lab/labview.h"
#include "colormap.h"
#include <ctype.h>
#include <stdarg.h>

#include "console.h"
#include "platform.h"
#include "core/paths.h"
#include "ctl/matlib.h"
#include "fembridge.h"
#include "voice.h"
#include "glutil.h"
#include "image.h"

#define TOOLBAR_Y 32.0f
#define TOOLBAR_H 26.0f

static void exec_cmd(const char *cmd) { console_exec(cmd, true); }
static void solid_run_now(void);
static void solid_open_run(void);

static void section(Ui *ui, float x, float y, float w, const char *title, const char *right) {
    float tw = ui_text(ui, FONT_BOLD, x, y, UI_TEXT, title);
    float rw = right ? ui_text_width(ui, FONT_SMALL, right) + 8 : 0;
    float lh = ui_line_height(ui, FONT_BOLD);
    ui_rect(ui, x + tw + 8, y + lh * 0.5f, MAXI(w - tw - 8 - rw, 0.0f), 1, UI_HAIRLINE, 0);
    if (right) ui_text_right(ui, FONT_SMALL, x + w, y + 1, UI_DIM, right);
}

/* Every sentence and every labelled number the panel writes is also kept as text, so a test can read what the panel
 * said instead of guessing from pixels ('uitext'). It is cleared at the start of each panel. */
static char panel_text[8192];
static size_t panel_text_len;

static void panel_say(const char *fmt, ...) {
    if (panel_text_len + 2 >= sizeof panel_text) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(panel_text + panel_text_len, sizeof panel_text - panel_text_len, fmt, ap);
    va_end(ap);
    if (n > 0) panel_text_len = MINI(panel_text_len + (size_t)n, sizeof panel_text - 1);
}

const char *hud_panel_text(void) { return panel_text; }

/* Manual keeps the command line within reach while giving the numerical inspector the visual emphasis. */
static bool manual_terminal_open;
static void manual_terminal(Ui *ui, float x, float y, float w, float bottom, double time) {
    const float input_h = ui_line_height(ui, FONT_MONO) + 14;
    if (bottom - y < input_h + 24) {
        if (!console_focused()) return;
        y = MAXI(80.0f, bottom - 180.0f);
        ui_rect(ui, x - 6, y - 6, w + 12, bottom - y + 12, 0xF7F6F3F8u, 7);
    }
    bool expanded = manual_terminal_open || console_focused();
    if (ui_button(ui, "TERMINAL##output", x, y, 94, 22, expanded)) {
        manual_terminal_open = !expanded;
        if (expanded) console_set_focus(false);
    }
    ui_text_right(ui, FONT_SMALL, x + w, y + 5, UI_DIM,
                  console_focused() ? "Esc to leave" : expanded ? "click to collapse" : "click to expand / ` to type");
    panel_say("Terminal output: %s\n", expanded ? "expanded" : "collapsed");
    y += 26;
    float th = expanded ? bottom - y : input_h;
    if (ui_clickable(ui, "terminal", x, y, w, th)) console_set_focus(true);
    console_draw(ui, x, y, w, th, time);
}

static void kv(Ui *ui, float x, float y, float w, const char *k, const char *v, uint32_t vcol) {
    panel_say("%s: %s\n", k, v);
    ui_text(ui, FONT_SMALL, x, y + 1, UI_DIM, k);
    ui_text_right(ui, FONT_MONO, x + w, y, vcol, v);
}

static void fmt_eng(char *buf, size_t cap, double v, const char *unit) { format_si(buf, cap, v, unit); }

/* key/value row that is also a control: commands ending in a space are prefilled in the terminal so the
 * user types the value; complete commands run immediately */
static void kv_cmd(Ui *ui, float x, float y, float w, const char *k, const char *v, uint32_t vcol, const char *cmd) {
    if (ui_hover(ui, x - 4, y - 1, w + 8, 16)) ui_rect(ui, x - 4, y - 1, w + 8, 16, 0xE8551C16u, 3);
    kv(ui, x, y, w, k, v, vcol);
    if (ui_clickable(ui, k, x - 4, y - 1, w + 8, 16)) {
        size_t n = strlen(cmd);
        if (n && cmd[n - 1] == ' ')
            console_set_input(cmd);
        else
            exec_cmd(cmd);
    }
}

static bool slider_row(Ui *ui, const char *id, const char *label, const char *value, float x, float y, float w,
                       double *v, double lo, double hi, bool logscale) {
    ui_text(ui, FONT_SMALL, x, y, UI_DIM, label);
    ui_text_right(ui, FONT_MONO, x + w, y - 1, UI_TEXT, value);
    return ui_slider(ui, id, x, y + 15, w, 10, v, lo, hi, logscale);
}

/* the pill at the right end of the toolbar; computed early so the layer buttons know how much room is left */
static void perf_text(char *buf, size_t cap) {
    if (labapp_active()) {
        snprintf(buf, cap, "%s replay \xC2\xB7 %.0f FPS", labapp_native3d() ? "3D" : "result", app.fps);
    } else if (app.workspace == WS_SOLID) {
        const FemState *fs = fem_state();
        if (fs->job_active) snprintf(buf, cap, "SOLVING %.0f%% \xC2\xB7 %.0f FPS", fs->job_progress * 100, app.fps);
        else if (fs->nelems > 0) snprintf(buf, cap, "%d elements \xC2\xB7 %.0f FPS", fs->nelems, app.fps);
        else snprintf(buf, cap, "%.0f FPS", app.fps);
    } else {
        snprintf(buf, cap, "%.0f MLUPS \xC2\xB7 %.0f FPS", app.status.mlups, app.fps);
    }
}

static bool glossary_open;
static bool lab_library_open;
static bool expert_view(void); /* the full controls are shown (the focus interface is not) */
static bool chat_open = false; /* Ask AI: the conversation sidebar, opened on purpose (owner: hidden by default) */
/* Ask AI: the model and effort, picked per tool from the tool's own lists ([0] Claude Code, [1] Codex; "" is the tool's
 * default) and remembered; the menu that picks them opens from one chip beside the box */
static char agent_model_pick[2][64], agent_effort_pick[2][32], agent_perm_pick[2][24];
static bool model_menu_open;
static char agent_seen[1024], agent_run_key[96]; /* the model each pick ran last time; the pick of the run going now */
static float chat_scroll, chat_max; /* how far back the conversation is scrolled, in points */
static float chat_left = 1e9f;      /* the conversation's left edge in the last frame: the wheel scrolls it there */
static bool round_action(Ui *ui, const char *id, float cx, float cy, float r, const char *glyph, bool accent);
bool hud_simulations_open(void) { return lab_library_open; }
static char solid_field_id[48];
static bool clean_view;
static float inspector_scroll, inspector_max;
static int browser_tab, browser_page;
static char browser_search[128], browser_domain[32];
static bool browser_refresh = true;
static bool browser_return_clean;
void hud_open_simulations(void) {
    browser_return_clean = !app.hud_on && clean_view;
    app.hud_on = true;
    lab_library_open = true;
    browser_refresh = true;
    console_set_focus(false);
    solid_field_id[0] = 0;
}
void hud_panel_scroll(float dy) {
    if (lab_library_open) return;
    if (console_focused() || manual_terminal_open) { console_scroll(dy / 14.0f); return; }
    inspector_scroll = CLAMP(inspector_scroll - dy, 0, inspector_max);
}
/* the wheel over the open conversation scrolls it back through the talk (true: the wheel was used here) */
bool hud_scroll_at(float x, float y, float dy) {
    if (lab_library_open || expert_view() || app.ui_mode != UI_AGENT || !chat_open || x < chat_left || y < 44) return false;
    chat_scroll = CLAMP(chat_scroll + dy, 0, chat_max);
    return true;
}
static void inspector_begin(Ui *ui, float x, float top, float w, float bottom) {
    ui_push_clip(ui, x, top, w, MAXI(0, bottom - top));
}
static void inspector_end(Ui *ui, float x, float top, float w, float bottom, float content_end) {
    ui_pop_clip(ui);
    inspector_max = MAXI(0, content_end + inspector_scroll - bottom);
    inspector_scroll = MINI(inspector_scroll, inspector_max);
    if (inspector_max > 0) {
        float travel = bottom - top, thumb = MAXI(24, travel * travel / (travel + inspector_max));
        ui_rect(ui, x + w - 3, top, 3, travel, 0xDDDAD4FFu, 1);
        ui_rect(ui, x + w - 3, top + (travel - thumb) * inspector_scroll / inspector_max, 3, thumb, UI_DIM, 1);
    }
}
void hud_set_clean_view(bool enabled) { clean_view = enabled; }
/* Manual and Agentic use the same result, camera and playback state. */
static float mode_switch_width(Ui *ui) {
    return ui_text_width(ui, FONT_SMALL, "Manual") + ui_text_width(ui, FONT_SMALL, "Agentic") + 2 * 20 + 2 + 36;
}

static float toolbar(float W) {
    Ui *ui = app.ui;
    float x = 12, y = TOOLBAR_Y, h = TOOLBAR_H;
    const SimStatus *st = &app.status;
    char perf[96];
    perf_text(perf, sizeof perf);

    #define BTN(label, w, active) (ui_block_mouse(ui, x, y, w, h), ui_button(ui, label, x, y, w, h, active))
    ui_block_mouse(ui, 12, 3, 108, 24);
    if (ui_button(ui, "SIMULATIONS##labnav", 12, 3, 108, 24, lab_library_open)) {
        if (lab_library_open) { lab_library_open = false; app.hud_on = !browser_return_clean; solid_field_id[0] = 0; }
        else hud_open_simulations();
    }
    if (lab_library_open) goto perf_pill;
    if (app.ui_mode != UI_ADVANCED) goto perf_pill;
    if (labapp_active() || labapp_prepared()) {
        if (BTN("FLUID##labnav", 66, false)) { labapp_close(); lab_library_open=false; exec_cmd("workspace fluid"); }
        x += 70;
        if (BTN("SOLID##labnav", 66, false)) { labapp_close(); lab_library_open=false; exec_cmd("workspace solid"); }
        goto perf_pill;
    }
    /* The tunnel's own actions belong to the tunnel. In the analysis workspace START, RESET and LOAD STL are not
     * dim, they are absent: the water tunnel is a different instrument and its verbs mean nothing to a part. */
    if (app.workspace == WS_FLUID) {
        bool running = st->running;
        if (BTN(running ? "\xE2\x8F\xB8  PAUSE##run" : "\xE2\x96\xB6  START##run", 86, running)) exec_cmd(running ? "pause" : "start");
        x += 90;
        if (BTN("\xE2\x86\xBA  RESET", 76, false)) exec_cmd("reset");
        x += 80;
        if (BTN("LOAD STL", 76, false)) exec_cmd("open");
        x += 84;
    }

    /* the two halves of the application: the same window, camera and terminal drive either solver */
    ui_rect(ui, x - 6, y + 4, 1, h - 8, 0xB4B1AB80u, 0);
    static const char *const WS_LABEL[WS_COUNT] = {"FLUID", "SOLID"};
    if (W < 1320) {
        /* narrow window: one button that switches, so the layer toggles keep their room */
        int other = app.workspace == WS_FLUID ? WS_SOLID : WS_FLUID;
        char label[32];
        /* U+2194 is in the font atlas (font.c EXTRA_CODEPOINTS); U+21C4 is not and drew as "?" */
        snprintf(label, sizeof label, "\xE2\x86\x94 %s", WS_LABEL[other]);
        float ww = ui_text_width(ui, FONT_SMALL, label) + 14;
        if (BTN(label, ww, false)) {
            char cmd[32];
            snprintf(cmd, sizeof cmd, "workspace %s", workspace_name(other));
            exec_cmd(cmd);
        }
        x += ww + 2;
    } else {
        for (int i = 0; i < WS_COUNT; i++) {
            float ww = ui_text_width(ui, FONT_SMALL, WS_LABEL[i]) + 14;
            if (BTN(WS_LABEL[i], ww, app.workspace == i)) {
                char cmd[32];
                snprintf(cmd, sizeof cmd, "workspace %s", workspace_name(i));
                exec_cmd(cmd);
            }
            x += ww + 2;
        }
    }
    x += 6;

    if (app.workspace == WS_SOLID) {
        const FemState *fs = fem_state();
        ui_rect(ui, x - 6, y + 4, 1, h - 8, 0xB4B1AB80u, 0);
        static const char *const FL[FEM_FIELD_COUNT] = {"STRESS", "DISPLACEMENT", "TEMPERATURE", "FAILURE INDEX"};
        for (int i = 0; i < FEM_FIELD_COUNT; i++) {
            if (fs->have_result && !fem_field_available(i)) continue;
            float fw = MAXI(ui_text_width(ui, FONT_SMALL, FL[i]) + 18, 30.0f);
            if (BTN(FL[i], fw, fem_field() == i)) {
                char cmd[64];
                snprintf(cmd, sizeof cmd, "fem field %s", fem_field_name(i));
                exec_cmd(cmd);
            }
            x += fw + 3;
        }
        x += 10;
        ui_rect(ui, x - 6, y + 4, 1, h - 8, 0xB4B1AB80u, 0);
        float ow = ui_text_width(ui, FONT_SMALL, "OPEN STL") + 18;
        if (BTN("OPEN STL", ow, false)) exec_cmd("open");
        x += ow + 3;
        float rw = ui_text_width(ui, FONT_SMALL, "RESULT") + 18;
        if (BTN("RESULT", rw, fem_visible() && fs->have_result)) exec_cmd(fem_visible() ? "fem hide" : "fem show");
        x += rw + 3;
        const char *run_label = fs->job_active ? "STOP##runsetup" : "RUN SETUP##runsetup";
        float fw = ui_text_width(ui, FONT_SMALL, fs->job_active ? "STOP" : "RUN SETUP") + 18;
        if (BTN(run_label, fw, fs->job_active)) {
            if (fs->job_active) exec_cmd("solid cancel");
            else solid_open_run();
        }
        x += fw + 3;
        goto perf_pill;
    }

    static const struct { const char *label; int field; } tabs[] = {
        {"SPEED", DF_SPEED}, {"Ux", DF_UX}, {"PRESSURE", DF_PRESSURE}, {"Cp", DF_CP},
        {"VORTICITY", DF_VORTICITY}, {"Q", DF_QCRIT}};
    ui_rect(ui, x - 6, y + 4, 1, h - 8, 0xB4B1AB80u, 0);
    for (size_t i = 0; i < ARRAY_LEN(tabs); i++) {
        float w = MAXI(ui_text_width(ui, FONT_SMALL, tabs[i].label) + 18, 30.0f);
        if (BTN(tabs[i].label, w, app.display_field == tabs[i].field)) {
            char cmd[64];
            snprintf(cmd, sizeof cmd, "view %s", display_field_name(tabs[i].field));
            exec_cmd(cmd);
        }
        x += w + 3;
    }
    x += 10;
    ui_rect(ui, x - 6, y + 4, 1, h - 8, 0xB4B1AB80u, 0);
    static const struct { const char *label, *cmd; int which; } layers[] = {
        {"STREAMLINES", "streamlines toggle", 0}, {"PARTICLES", "particles toggle", 1}, {"VORTICES", "vortices toggle", 2},
        {"SLICE", "slice", 3}, {"SURFACE", NULL, 4}, {"VOLUME", "volume toggle", 5}};
    bool states[] = {app.rs.stream_on, app.rs.particles_on, app.rs.vortex_on, app.rs.slice_on,
                     app.rs.surface_mode == SURFACE_FIELD, app.rs.volume_on};
    float limit = W - (ui_text_width(ui, FONT_SMALL, perf) + 20) - 22;
    for (size_t i = 0; i < ARRAY_LEN(layers); i++) {
        float w = ui_text_width(ui, FONT_SMALL, layers[i].label) + 18;
        if (x + w > limit) break;
        if (BTN(layers[i].label, w, states[i])) {
            if (layers[i].which == 4)
                exec_cmd(app.rs.surface_mode == SURFACE_FIELD ? "surface solid" : "surface field");
            else
                exec_cmd(layers[i].cmd);
        }
        x += w + 3;
    }
    #undef BTN

perf_pill:;
    /* performance pill */
    float pw = ui_text_width(ui, FONT_SMALL, perf) + 20;
    /* who the window is for, always one click away and remembered: Simple | Advanced | Agent, and "?". It sits in
     * the title row at the top right, where it never competes with the tunnel's crowded toolbar. */
    {
        float mx = W - 12 - mode_switch_width(ui), y = 3, h = 24;
        static const char *const ML[UI_MODE_COUNT] = {"Simple##mode", "Manual##mode", "Agentic##mode"};
        static const char *const MV[UI_MODE_COUNT] = {"Simple", "Manual", "Agentic"};
        for (int i = UI_ADVANCED; i < UI_MODE_COUNT; i++) {
            float bw = ui_text_width(ui, FONT_SMALL, MV[i]) + 20;
            ui_block_mouse(ui, mx, y, bw, h);
            if (ui_button(ui, ML[i], mx, y, bw, h, app.ui_mode == i) && app.ui_mode != i) {
                char cmd[32];
                snprintf(cmd, sizeof cmd, "mode %s", ui_mode_name(i));
                exec_cmd(cmd);
            }
            mx += bw + 2;
        }
        ui_block_mouse(ui, mx + 4, y, 28, h);
        if (ui_button(ui, "?##glossary", mx + 4, y, 28, h, glossary_open)) glossary_open = !glossary_open;
    }
    y = TOOLBAR_Y, h = TOOLBAR_H;
    ui_rect(ui, W - pw - 12, y + 3, pw, h - 6, 0xEFEDE9E0u, 10);
    ui_rect_outline(ui, W - pw - 12, y + 3, pw, h - 6, 0xB4B1AB90u, 10, 1);
    ui_text(ui, FONT_SMALL, W - pw - 2, y + (h - ui_line_height(ui, FONT_SMALL)) * 0.5f, st->running ? UI_GOOD : UI_DIM, perf);
    ui_block_mouse(ui, W - pw - 12, y + 3, pw, h - 6);
    if (ui_hover(ui, W - pw - 12, y + 3, pw, h - 6)) ui_rect_outline(ui, W - pw - 12, y + 3, pw, h - 6, 0xE8551CA0u, 10, 1);
    if (ui_clickable(ui, "performance", W - pw - 12, y + 3, pw, h - 6)) exec_cmd("perf");
    return y + h;
}

static void title_strip(float W) {
    Ui *ui = app.ui;
    char t[256];
    const char *name, *kind;
    if (lab_library_open) {
        name = "OpenPhysicsAI";
        kind = "  simulation library";
        t[0] = 0;
    } else if (labapp_active()) {
        name = "OpenPhysicsAI";
        kind = "  physics lab \xC2\xB7 ";
        snprintf(t, sizeof t, "%s", labapp_title());
    } else if (labapp_prepared()) {
        name = "OpenPhysicsAI";
        kind = "  physics lab / setup / ";
        snprintf(t, sizeof t, "%s", labapp_scenario_title());
    } else if (app.workspace == WS_SOLID) {
        const FemState *fs = fem_state();
        name = "OpenPhysicsAI";
        kind = app.ui_mode == UI_ADVANCED ? (fs->meshed && fs->mesh_method == 1 ? (fs->tet_order == 2 ? "  finite-element analysis \xC2\xB7 tet10 FEM \xC2\xB7 "
                                                                                                 : "  finite-element analysis \xC2\xB7 tet4 FEM \xC2\xB7 ")
                                                                               : "  finite-element analysis \xC2\xB7 hex8 FEM \xC2\xB7 ")
                                          : "  print simulation \xC2\xB7 ";
        snprintf(t, sizeof t, "%s", fs->have_project ? (fs->body[0] ? fs->body : fs->project) : "");
    } else {
        name = "OpenPhysicsAI";
        kind = "  lattice-Boltzmann water tunnel \xC2\xB7 D3Q19 \xC2\xB7 ";
        snprintf(t, sizeof t, "%s", app.has_model ? app.model_name : "empty tunnel");
    }
    char kbuf[96];
    if (!t[0]) { /* no name to introduce: the separator would trail off into nothing */
        snprintf(kbuf, sizeof kbuf, "%s", kind);
        size_t n = strlen(kbuf);
        while (n > 0 && (kbuf[n - 1] == ' ' || (unsigned char)kbuf[n - 1] == 0xB7 || (unsigned char)kbuf[n - 1] == 0xC2))
            kbuf[--n] = 0;
        kind = kbuf;
    }
    float w1 = ui_text_width(ui, FONT_BOLD, name);
    float w2 = ui_text_width(ui, FONT_SMALL, kind);
    float w3 = ui_text_width(ui, FONT_SMALL, t);
    float right = W - mode_switch_width(ui) - 24;
    float x = MAXI(132, (W - (w1 + w2 + w3)) * 0.5f), y = 7;
    ui_push_clip(ui, 132, 0, MAXI(right - 132, 0), 30);
    x += ui_text(ui, FONT_BOLD, x, y, 0x111111FFu, name);
    x += ui_text(ui, FONT_SMALL, x, y + 1, UI_DIM, kind);
    ui_text(ui, FONT_SMALL, x, y + 1, UI_ACCENT2, t);
    ui_pop_clip(ui);
}

/* CONTROLS header with FLOW / MODEL / TIME tabs on the right */
static void controls_header(Ui *ui, float x, float y, float w) {
    static const char *names[] = {"FLOW", "MODEL", "TIME", "LINES"};
    float right = x + w;
    for (int i = 3; i >= 0; i--) {
        float tw = ui_text_width(ui, FONT_SMALL, names[i]) + 14, tx = right - tw;
        bool on = app.panel_tab == i, hov = ui_hover(ui, tx, y - 2, tw, 18);
        if (on || hov) ui_rect(ui, tx, y - 2, tw, 18, on ? 0xFBE3D6C0u : 0xE8551C16u, 4);
        if (on) ui_rect(ui, tx + 4, y + 14, tw - 8, 1.5f, 0xE8551CFFu, 1);
        ui_text(ui, FONT_SMALL, tx + 7, y + 1, on ? 0x111111FFu : (hov ? UI_TEXT : UI_DIM), names[i]);
        if (ui_clickable(ui, names[i], tx, y - 2, tw, 18)) app.panel_tab = i;
        right = tx - 3;
    }
    float tw = ui_text(ui, FONT_BOLD, x, y, UI_TEXT, "CONTROLS");
    float lh = ui_line_height(ui, FONT_BOLD);
    ui_rect(ui, x + tw + 8, y + lh * 0.5f, MAXI(right - (x + tw + 8) - 6, 0.0f), 1, 0xB4B1AB90u, 0);
}

/* MODEL tab: quarter turns of the mesh, quick fixes, attitude and size */
static float model_controls(Ui *ui, float x, float y, float w) {
    SimParams *p = &app.params;
    char a[64];
    if (!app.has_model) {
        ui_text(ui, FONT_SMALL, x, y + 4, UI_DIM, "no model loaded - LOAD STL, drop a file, or 'scene glider'");
        return y + 32;
    }
    static const struct { const char *label, *cmd; } fixes[] = {
        {"X 90\xC2\xB0##turn", "rotate x 90"}, {"Y 90\xC2\xB0##turn", "rotate y 90"}, {"Z 90\xC2\xB0##turn", "rotate z 90"},
        {"Z-UP##orient", "orient zup"},      {"FLIP##orient", "orient flip"},    {"AUTO##orient", "orient auto"},
        {"RESET##orient", "orient reset"}};
    int nb = (int)ARRAY_LEN(fixes);
    float gap = 4, bw = (w - gap * (nb - 1)) / nb, bh = 22;
    for (int i = 0; i < nb; i++)
        if (ui_button(ui, fixes[i].label, x + i * (bw + gap), y, bw, bh, false)) exec_cmd(fixes[i].cmd);
    y += bh + 10;
    double v = p->aoa;
    snprintf(a, sizeof a, "%+.1f\xC2\xB0", p->aoa);
    if (slider_row(ui, "sl_pitch", "pitch \xC2\xB7 nose up", a, x, y, w, &v, -90.0, 90.0, false)) {
        double nv = round(v * 2.0) / 2.0;
        if (nv != p->aoa) {
            p->aoa = nv;
            app_push_params();
        }
    }
    y += 32;
    v = p->yaw;
    snprintf(a, sizeof a, "%+.1f\xC2\xB0", p->yaw);
    if (slider_row(ui, "sl_yaw", "yaw \xC2\xB7 \xE2\x8C\xA5-drag the model to turn it", a, x, y, w, &v, -180.0, 180.0, false)) {
        double nv = round(v);
        if (nv != p->yaw) {
            p->yaw = nv;
            app_push_params();
        }
    }
    y += 32;
    v = p->roll;
    snprintf(a, sizeof a, "%+.1f\xC2\xB0", p->roll);
    if (slider_row(ui, "sl_roll", "roll \xC2\xB7 \xE2\x87\xA7\xE2\x8C\xA5-drag", a, x, y, w, &v, -180.0, 180.0, false)) {
        double nv = round(v);
        if (nv != p->roll) {
            p->roll = nv;
            app_push_params();
        }
    }
    y += 32;
    v = p->fit;
    snprintf(a, sizeof a, "%.0f%% of the tunnel", p->fit * 100);
    if (slider_row(ui, "sl_fit", "size", a, x, y, w, &v, 0.05, 0.95, false)) {
        double k = v / MAXI(p->fit, 1e-3); /* scale both limits so the size changes whichever one binds */
        p->fit = v;
        p->fit_length = CLAMP(p->fit_length * k, 0.02, 0.95);
        app_push_params();
    }
    return y + 32;
}

/* TIME tab: playback (slow motion), time step, resolution presets and single steps */
static float time_controls(Ui *ui, float x, float y, float w) {
    const SimStatus *st = &app.status;
    const SimUnits *u = &st->units;
    SimParams *p = &app.params;
    char a[96];
    double pb = app.playback > 0 ? app.playback : 1.0;
    double full = st->cells > 0 ? st->mlups * 1e6 / (double)st->cells : 0;
    if (pb >= 1.0) snprintf(a, sizeof a, "full \xC2\xB7 %.0f steps/s", full);
    else snprintf(a, sizeof a, "%.0f%% \xC2\xB7 %.2g steps/s", pb * 100, pb * full);
    double v = pb;
    if (slider_row(ui, "sl_playback", "playback speed \xC2\xB7 keys , .", a, x, y, w, &v, 0.01, 1.0, true)) {
        app.playback = v > 0.97 ? 1.0 : v;
        sim_set_playback(app.sim, app.playback);
    }
    y += 32;
    v = p->u_lattice;
    snprintf(a, sizeof a, "u %.3f \xC2\xB7 Ma %.2f", u->u_lb, u->mach);
    if (slider_row(ui, "sl_ulb", "time step \xC2\xB7 accurate \xE2\x86\x94 fast", a, x, y, w, &v, 0.02, 0.15, false)) {
        double nv = round(v * 200.0) / 200.0;
        if (nv != p->u_lattice) {
            p->u_lattice = nv;
            app_push_params();
        }
    }
    y += 32;
    static const struct { const char *label, *cmd; double cells; } q[] = {
        {"DRAFT##quality", "quality draft", 0.9e6}, {"NORMAL##quality", "quality normal", 2.8e6},
        {"HIGH##quality", "quality high", 5.5e6},   {"ULTRA##quality", "quality ultra", 9.0e6}};
    int cur = 0;
    double best = 1e30;
    for (int i = 0; i < 4; i++) {
        double d = fabs(log(((double)st->cells + 1.0) / q[i].cells));
        if (d < best) best = d, cur = i;
    }
    float lx = x + 78, gap = 4, bh = 22, bw = (w - 78 - gap * 3) / 4;
    ui_text(ui, FONT_SMALL, x, y + 4, UI_DIM, "resolution");
    for (int i = 0; i < 4; i++)
        /* the highlighted preset is only the nearest one: clicking it must still apply its exact resolution */
        if (ui_button(ui, q[i].label, lx + i * (bw + gap), y, bw, bh, i == cur && st->cells > 0)) exec_cmd(q[i].cmd);
    y += bh + 10;
    static const struct { const char *label, *cmd; } steps[] = {{"+1##step", "step 1"}, {"+10##step", "step 10"}, {"+100##step", "step 100"}};
    bw = (w - 78 - gap * 2) / 3;
    ui_text(ui, FONT_SMALL, x, y + 4, UI_DIM, "step \xC2\xB7 key /");
    for (int i = 0; i < 3; i++)
        if (ui_button(ui, steps[i].label, lx + i * (bw + gap), y, bw, bh, false)) exec_cmd(steps[i].cmd);
    y += bh + 10;
    double lap = u->u_lb > 0 && st->steps_per_sec > 0.01 ? u->ref_cells / (u->u_lb * st->steps_per_sec) : 0;
    if (lap >= 120) snprintf(a, sizeof a, "on screen, water moves one model length every %.1f min", lap / 60);
    else if (lap > 0) snprintf(a, sizeof a, "on screen, water moves one model length every %.*f s", lap < 10 ? 1 : 0, lap);
    else snprintf(a, sizeof a, "run the experiment to see the on-screen time scale");
    ui_text(ui, FONT_SMALL, x, y, UI_DIM, a);
    return y + 20;
}

/* LINES tab: rake shape, position (the orange handle in the view drags it too), size and line count */
static float lines_controls(Ui *ui, float x, float y, float w) {
    static const struct { const char *label; int mode; } shapes[] = {
        {"SHEET##rake", SEED_PLANE}, {"GRID##rake", SEED_GRID}, {"LINE##rake", SEED_LINE},
        {"POINT##rake", SEED_POINT}, {"WAKE##rake", SEED_WAKE}, {"RANDOM##rake", SEED_RANDOM}};
    char a[64];
    const int nb = (int)ARRAY_LEN(shapes) + 1;
    const float gap = 4, bh = 22, bw = (w - gap * (nb - 1)) / nb;
    for (int i = 0; i < nb - 1; i++)
        if (ui_button(ui, shapes[i].label, x + i * (bw + gap), y, bw, bh, app.rs.stream_on && app.seed_mode == shapes[i].mode)) {
            char cmd[48];
            snprintf(cmd, sizeof cmd, "streamlines %s", seed_mode_name(shapes[i].mode));
            exec_cmd(cmd);
        }
    if (ui_button(ui, "AUTO##rake", x + (nb - 1) * (bw + gap), y, bw, bh, !app.rake_manual)) exec_cmd("streamlines auto");
    y += bh + 10;
    const float dims[3] = {(float)MAXI(app.nx, 1), (float)MAXI(app.ny, 1), (float)MAXI(app.nz, 1)};
    static const char *ids[3] = {"sl_rake_x", "sl_rake_y", "sl_rake_z"};
    static const char *labels[3] = {"rake along the flow \xC2\xB7 drag the orange handle", "rake height", "rake across the span"};
    float *pos[3] = {&app.rake_pos.x, &app.rake_pos.y, &app.rake_pos.z};
    for (int i = 0; i < 3; i++) {
        double v = *pos[i] / dims[i];
        snprintf(a, sizeof a, "%.2f", v);
        if (slider_row(ui, ids[i], labels[i], a, x, y, w, &v, 0.0, 1.0, false)) {
            *pos[i] = (float)(v * dims[i]);
            app.rake_manual = true, app.rs.stream_on = true;
        }
        y += 32;
    }
    /* size (as a fraction of the tunnel cross-section diagonal) and line count side by side */
    const float hw = (w - 16) * 0.5f;
    const double diag_t = sqrt((double)dims[1] * dims[1] + (double)dims[2] * dims[2]);
    const double diag = sqrt((double)app.rake_size[0] * app.rake_size[0] + (double)app.rake_size[1] * app.rake_size[1]);
    double sv = MAXI(diag / diag_t, 0.02);
    snprintf(a, sizeof a, "%.2f", sv);
    if (slider_row(ui, "sl_rake_size", "size", a, x, y, hw, &sv, 0.02, 1.5, true)) {
        if (diag > 1e-3) {
            const float k = (float)(sv * diag_t / diag);
            app.rake_size[0] *= k, app.rake_size[1] *= k;
        } else {
            app.rake_size[0] = (float)(sv * dims[1]), app.rake_size[1] = (float)(sv * dims[2]);
        }
        app.rake_manual = true, app.rs.stream_on = true;
    }
    double lines = app.seed_count;
    snprintf(a, sizeof a, "%d", app.seed_count);
    if (slider_row(ui, "sl_seeds", "lines", a, x + hw + 16, y, hw, &lines, 20.0, 5000.0, true))
        app.seed_count = (int)lround(lines), app.rs.stream_on = true;
    return y + 32;
}

/* ---------------------------------------------------------------------------------------------------------------
 * SOLID workspace: one path through an analysis, six steps.
 *   1 PART -> 2 MESH -> 3 MATERIAL -> 4 HOLD & LOAD -> 5 SOLVE  -> 6 RESULTS   (what the part carries in service)
 *   1 PART -> 2 MESH -> 3 MATERIAL -> 4 BUILD        -> 5 RUN   -> 6 RESULTS   (what the printer does to the part)
 * The two paths are the same six steps; only step 4 and the name of step 5 differ, and the part chooses which one
 * in step 1. Every step knows its own state, only the step being worked on is open, and nothing is on screen that
 * cannot be used yet: a control that is not usable says why when the pointer rests on it. Every action is one of
 * the typed operations, the same ones an agent calls.
 * ------------------------------------------------------------------------------------------------------------- */

enum { STEP_PART = 0, STEP_MESH, STEP_MATERIAL, STEP_HOLD, STEP_SOLVE, STEP_RESULTS, STEP_COUNT };
typedef enum { ST_TODO = 0, ST_ATTENTION, ST_DONE } StepState;

static int solid_step;          /* the step whose controls are open */
static bool solid_step_pinned;  /* the user chose it; otherwise the panel follows the work */
static bool solid_view_open;    /* the viewer tools inside RESULTS */
static double solid_end_time = 300, solid_time_step = 10;
static int solid_analysis = 0;  /* 0 static, 1 transient thermal, 2 thermomechanical */

/* the build path: what the printer does to the part, for the two job kinds the engine has */
static bool solid_build_path;   /* false: hold and load it. true: print or build it */
static bool solid_box_pick;     /* BOX armed: a drag in the view collects faces instead of turning the camera */
static bool solid_box_front;    /* the box takes only the faces turned towards the camera */
static int build_kind;          /* 0 lpbf_build (metal powder bed), 1 fff_print (plastic filament) */
static int build_orient;        /* 0 the part's long axis lay along machine X, 1 along Y */
static int build_prov = 1;      /* example strain is inferred until the user states otherwise */
static int fdm_prov = 1;        /* example printer settings are inferred, never automatically calibrated */
static bool build_cut = true;   /* the wire cut that releases the part from the plate */

static const char *const BUILD_PROV[3] = {"user", "inferred", "calibrated"};

static const char *const SOLID_ANALYSIS[3] = {"static_structural", "transient_thermal", "thermomechanical"};

static void solid_open_run(void) {
    solid_step = STEP_SOLVE;
    solid_step_pinned = true;
    inspector_scroll = 0;
}

static void solid_run_now(void) {
    char cmd[128];
    if (solid_analysis == 0)
        snprintf(cmd, sizeof cmd, "solid run static");
    else
        snprintf(cmd, sizeof cmd, "solid run %s %.10g %.10g", solid_analysis == 1 ? "thermal" : "thermomechanical",
                 solid_end_time, solid_time_step);
    exec_cmd(cmd);
}

/* small text broken at spaces; returns the y below the last line drawn */
static float wrap_text(Ui *ui, float x, float y, float w, uint32_t col, const char *text, int max_lines) {
    panel_say("%s\n", text);
    float lh = ui_line_height(ui, FONT_SMALL);
    const char *p = text;
    for (int line = 0; line < max_lines && *p; line++) {
        int n = 0, last_space = 0;
        while (p[n] && ui_text_width_n(ui, FONT_SMALL, p, n + 1) <= w) {
            if (p[n] == ' ') last_space = n;
            n++;
        }
        if (p[n] && last_space > 0) n = last_space;
        char buf[256];
        snprintf(buf, sizeof buf, "%.*s", (int)MINI((size_t)n, sizeof buf - 1), p);
        ui_text(ui, FONT_SMALL, x, y, col, buf);
        y += lh;
        p += n;
        while (*p == ' ') p++;
    }
    return y;
}

static void progress_bar(Ui *ui, float x, float y, float w, float h, double frac, uint32_t col) {
    ui_rect(ui, x, y, w, h, 0xF7F6F3FFu, h * 0.5f);
    float f = (float)CLAMP(frac, 0.0, 1.0);
    if (f > 0.002f) ui_rect(ui, x, y, MAXI(w * f, h), h, col, h * 0.5f);
    ui_rect_outline(ui, x, y, w, h, 0xB4B1AB90u, h * 0.5f, 1);
}

/* Fields you type a number into. A build has several inputs at once (layer thickness, cut height, kerf), so each
 * field keeps its own text and only the focused one receives keys. Keys arrive through the same path as every other
 * key (main.c), so a test types into a field the way a person does. */
typedef struct NumField { char id[24], text[32]; } NumField;
static NumField solid_fields[12];


static NumField *field_slot(const char *id) {
    NumField *spare = NULL;
    for (size_t i = 0; i < sizeof solid_fields / sizeof solid_fields[0]; i++) {
        if (!strcmp(solid_fields[i].id, id)) return &solid_fields[i];
        if (!spare && !solid_fields[i].id[0]) spare = &solid_fields[i];
    }
    if (spare) str_copy(spare->id, sizeof spare->id, id);
    return spare; /* NULL only if the panel ever grows past the table, and then the field simply holds nothing */
}

static const char *field_text(const char *id) {
    const NumField *f = field_slot(id);
    return f ? f->text : "";
}

static void field_clear(const char *id) {
    NumField *f = field_slot(id);
    if (f) f->text[0] = 0;
}

/* what the field says, or the given default when it is empty: every build input has a stated default */
static double field_num(const char *id, double fallback) {
    const char *t = field_text(id);
    return t[0] ? atof(t) : fallback;
}

/* fields that take words (the agent's question, its command line): a buffer each, registered by id */
typedef struct TextField { const char *id; char *buf; size_t cap; void (*on_enter)(void); } TextField;
static TextField text_fields[8];
static int ntext_fields;

static TextField *text_slot(const char *id) {
    for (int i = 0; i < ntext_fields; i++)
        if (!strcmp(text_fields[i].id, id)) return &text_fields[i];
    return NULL;
}

bool hud_text_focused(void) { return solid_field_id[0] != 0; }

void hud_text_key(uint32_t key) {
    TextField *tf = solid_field_id[0] ? text_slot(solid_field_id) : NULL;
    if (tf) {
        size_t n = strlen(tf->buf);
        if (key == KEY_ESCAPE) solid_field_id[0] = 0;
        else if (key == KEY_ENTER) {
            solid_field_id[0] = 0;
            if (tf->on_enter) tf->on_enter();
        } else if (key == KEY_BACKSPACE) {
            if (n) tf->buf[n - 1] = 0;
        } else if (key >= 32 && key < 127 && n + 1 < tf->cap) {
            tf->buf[n] = (char)key;
            tf->buf[n + 1] = 0;
        }
        return;
    }
    NumField *f = solid_field_id[0] ? field_slot(solid_field_id) : NULL;
    if (!f) return;
    size_t n = strlen(f->text);
    if (key == KEY_ESCAPE) {
        f->text[0] = 0;
        solid_field_id[0] = 0;
    } else if (key == KEY_ENTER) {
        solid_field_id[0] = 0; /* the value stays in the field for the step to read */
    } else if (key == KEY_BACKSPACE) {
        if (n) f->text[n - 1] = 0;
    } else if (key < 128 && (isdigit((int)key) || key == '.' || key == '-' || key == 'e' || key == '+') &&
               n + 1 < sizeof f->text) {
        f->text[n] = (char)key;
        f->text[n + 1] = 0;
    }
}

/* Cmd+V: the clipboard typed into the focused field, line breaks as spaces, as much as fits */
void hud_text_paste(void) {
    char clip[4096];
    if (!platform_clipboard_get(clip, sizeof clip)) return;
    for (const char *c = clip; *c; c++) {
        uint32_t k = (uint8_t)*c;
        if (k == '\n' || k == '\r' || k == '\t') k = ' ';
        if (k >= 32 && k < 127) hud_text_key(k);
    }
}

void hud_text_copy(void) {
    TextField *tf = solid_field_id[0] ? text_slot(solid_field_id) : NULL;
    platform_clipboard_set(tf ? tf->buf : solid_field_id[0] ? field_text(solid_field_id) : "");
}

const char *hud_text_value(void) {
    TextField *tf = solid_field_id[0] ? text_slot(solid_field_id) : NULL;
    if (tf) return tf->buf;
    return solid_field_id[0] ? field_text(solid_field_id) : "";
}

/* a field that takes words; Enter runs on_enter. Several lines are shown when the text is long. */
static float text_field(Ui *ui, const char *id, float x, float y, float w, int lines, const char *hint, char *buf,
                        size_t cap, void (*on_enter)(void)) {
    TextField *tf = text_slot(id);
    if (!tf && ntext_fields < (int)(sizeof text_fields / sizeof text_fields[0])) {
        tf = &text_fields[ntext_fields++];
        tf->id = id;
    }
    if (tf) tf->buf = buf, tf->cap = cap, tf->on_enter = on_enter;
    bool focused = !strcmp(solid_field_id, id);
    float lh = ui_line_height(ui, FONT_SMALL), h = 12 + lines * (lh + 1);
    float tr = lines <= 1 ? h * 0.5f : 10; /* studio: a one-line field is a white pill */
    ui_rect(ui, x, y, w, h, 0xFFFFFFFFu, tr);
    ui_rect_outline(ui, x, y, w, h, focused ? 0xE8551CC0u : 0xDDDAD4FFu, tr, focused ? 1.5f : 1);
    ui_push_clip(ui, x + 2, y, w - 4, h);
    const char *shown = buf[0] ? buf : (focused ? "" : hint);
    float yy = y + 6, pad = lines <= 1 ? 14 : 8, caret_x = x + pad, caret_y = y + 6;
    const char *p = shown;
    if (lines <= 1 && buf[0]) { /* one line: show the end of the text, so what is typed is always in view */
        while (*p && ui_text_width(ui, FONT_SMALL, p) > w - pad - 16) p++;
    }
    for (int line = 0; line < lines && *p; line++) {
        int n = 0, last_space = 0;
        while (p[n] && ui_text_width_n(ui, FONT_SMALL, p, n + 1) <= w - 18) {
            if (p[n] == ' ') last_space = n;
            n++;
        }
        if (p[n] && last_space > 0) n = last_space;
        char lb[512];
        snprintf(lb, sizeof lb, "%.*s", (int)MINI((size_t)n, sizeof lb - 1), p);
        float tw = ui_text(ui, FONT_SMALL, x + pad, yy, buf[0] ? UI_TEXT : UI_FAINT, lb);
        if (buf[0]) caret_x = x + pad + tw + 1, caret_y = yy;
        yy += lh + 1;
        p += n;
        while (*p == ' ') p++;
    }
    if (focused && ((int)(app.time * 2) & 1)) ui_rect(ui, caret_x, caret_y, 1.5f, lh, UI_ACCENT, 0); /* after the last letter */
    ui_pop_clip(ui);
    ui_block_mouse(ui, x, y, w, h);
    if (ui_clickable(ui, id, x, y, w, h)) {
        str_copy(solid_field_id, sizeof solid_field_id, id);
        console_set_focus(false);
    }
    return y + h;
}

/* returns true while this field holds the keyboard */
static bool number_field(Ui *ui, const char *id, float x, float y, float w, float h, const char *hint) {
    bool focused = !strcmp(solid_field_id, id);
    const char *own = field_text(id);
    ui_rect(ui, x, y, w, h, focused ? 0xEFEDE9FFu : 0xF7F6F3FFu, 4);
    ui_rect_outline(ui, x, y, w, h, focused ? 0xE8551CC0u : 0xDDDAD4FFu, 4, 1);
    const char *text = own[0] ? own : (focused ? "" : hint);
    uint32_t col = own[0] ? UI_TEXT : UI_FAINT;
    ui_text(ui, FONT_MONO, x + 8, y + (h - ui_line_height(ui, FONT_MONO)) * 0.5f, col, text);
    if (focused) {
        float cx = x + 8 + ui_text_width(ui, FONT_MONO, own) + 1;
        ui_rect(ui, cx, y + 4, 1.5f, h - 8, UI_ACCENT, 0);
    }
    ui_block_mouse(ui, x, y, w, h);
    if (ui_clickable(ui, id, x, y, w, h)) {
        /* a click on a number field starts a new number. There is no caret to place, so the alternative is typing
         * that appends to what is already there, which turned a 2 followed by a 6 into 26 mm. */
        NumField *f = field_slot(id);
        if (f) f->text[0] = 0;
        str_copy(solid_field_id, sizeof solid_field_id, id);
        console_set_focus(false);
    }
    return focused;
}

/* A button that may not be usable yet. It stays on screen so the path is visible, but it is dim, it does nothing,
 * and resting on it says why. Tests can still find it by name. A click on it while it is dim sets *refused (when
 * given) to the reason, so the caller can keep that refusal beside the step instead of only in the log. */
static bool refusable_button(Ui *ui, const char *label, float x, float y, float w, float h, bool active, const char *why,
                             const char **refused) {
    if (!why) return ui_button(ui, label, x, y, w, h, active);
    const char *vis = label;
    char clean[96];
    const char *hash = strstr(label, "##");
    if (hash) {
        snprintf(clean, sizeof clean, "%.*s", (int)(hash - label), label);
        vis = clean;
    }
    ui_rect(ui, x, y, w, h, 0xF7F6F3FFu, 5);
    ui_rect_outline(ui, x, y, w, h, 0xDDDAD4FFu, 5, 1);
    float tw = ui_text_width(ui, FONT_SMALL, vis);
    ui_text(ui, FONT_SMALL, x + (w - tw) * 0.5f, y + (h - ui_line_height(ui, FONT_SMALL)) * 0.5f, 0xB4B1ABFFu, vis);
    ui_block_mouse(ui, x, y, w, h);
    if (ui_clickable(ui, vis, x, y, w, h)) { /* a click on a dim control answers instead of doing nothing */
        LOGI("%s", why);
        if (refused) *refused = why;
    }
    if (ui_hover(ui, x, y, w, h)) {
        float tipw = MINI(ui_text_width(ui, FONT_SMALL, why) + 16, 380.0f), tiph = 22;
        float tx = CLAMP(x, 12.0f, (float)app.win_w - tipw - 12), ty = y - tiph - 4;
        ui_rect(ui, tx, ty, tipw, tiph, 0xEFEDE9F4u, 5);
        ui_rect_outline(ui, tx, ty, tipw, tiph, 0xE8551C80u, 5, 1);
        ui_push_clip(ui, tx, ty, tipw, tiph);
        ui_text(ui, FONT_SMALL, tx + 8, ty + 4, UI_TEXT, why);
        ui_pop_clip(ui);
    }
    return false;
}

static bool step_button(Ui *ui, const char *label, float x, float y, float w, float h, bool active, const char *why) {
    return refusable_button(ui, label, x, y, w, h, active, why, NULL);
}

typedef struct SolidMat {
    char id[64], name[160], label[24];
    char status[24];
    int group;      /* 0 metal powder bed (LPBF), 1 plastic filament (FFF), 2 everything else */
} SolidMat;

static const char *const MAT_GROUP_TITLE[3] = {"Metal powder bed (LPBF)", "Plastic filament (FFF)", "Other"};

static bool record_has_process(const JsonValue *rec, const char *proc) {
    const JsonValue *pr = json_get(rec, "processes");
    for (size_t k = 0; k < json_len(pr); k++)
        if (!strcmp(json_str(json_at(pr, k)) ? json_str(json_at(pr, k)) : "", proc)) return true;
    return false;
}

/* the library as the operation material_list reports it, so the interface and an agent see the same records */
static int solid_materials(const SolidMat **out) {
    static SolidMat m[64];
    int n = 0;
    const JsonValue *arr = fem_material_list();
    for (size_t i = 0; i < json_len(arr) && n < 64; i++) {
        const JsonValue *rec = json_at(arr, i);
        str_copy(m[n].id, sizeof m[n].id, json_get_str(rec, "id", ""));
        str_copy(m[n].name, sizeof m[n].name, json_get_str(rec, "name", m[n].id));
        str_copy(m[n].status, sizeof m[n].status, json_get_str(rec, "status", ""));
        const char *grp = json_get_str(rec, "group", "");
        m[n].group = !strcmp(grp, "metal powder bed (LPBF)") ? 0 : !strcmp(grp, "plastic filament (FFF)") ? 1 : 2;
        /* button label: the grade, not the whole id (ss316l_lpbf -> SS316L); a demonstration record says so, so the
         * sourced record and the demonstration one never share a label; the terminal shows the id */
        size_t k = 0;
        for (; k + 1 < 16 && m[n].id[k] && m[n].id[k] != '_'; k++) m[n].label[k] = (char)toupper((unsigned char)m[n].id[k]);
        m[n].label[k] = 0;
        if (!strcmp(m[n].status, "demonstration")) snprintf(m[n].label + k, sizeof m[n].label - k, " DEMO");
        if (m[n].id[0]) n++;
    }
    *out = m;
    return n;
}

/* what a record lacks for the analyses the panel offers, in words, from material_list; "" when it has everything */
static void join_words(const JsonValue *arr, char *out, size_t cap) {
    size_t n = 0;
    out[0] = 0;
    for (size_t i = 0; i < json_len(arr) && n + 2 < cap; i++)
        n += (size_t)snprintf(out + n, cap - n, "%s%s", i ? ", " : "", json_str(json_at(arr, i)) ? json_str(json_at(arr, i)) : "");
}

static void material_gaps(const char *id, char *out, size_t cap) {
    out[0] = 0;
    const JsonValue *arr = fem_material_list(), *rec = NULL;
    for (size_t i = 0; i < json_len(arr) && !rec; i++)
        if (!strcmp(json_get_str(json_at(arr, i), "id", ""), id)) rec = json_at(arr, i);
    if (!rec) return;
    const JsonValue *lacks = json_get(rec, "lacks");
    char stress[200], heat[200];
    join_words(json_get(lacks, "stress"), stress, sizeof stress);
    join_words(json_get(lacks, "heat"), heat, sizeof heat);
    size_t n = 0;
    if (stress[0]) n += (size_t)snprintf(out + n, cap - n, "Not in this record, so a stress analysis refuses it: %s. ", stress);
    if (heat[0] && n < cap) snprintf(out + n, cap - n, "Not in this record, so a heat analysis refuses it: %s.", heat);
}

/* ---- what each step knows about itself --------------------------------------------------------------------- */

typedef struct StepInfo {
    StepState state;
    char line[72];   /* the one line the chip shows when the step is closed */
} StepInfo;

static void solid_steps(StepInfo info[STEP_COUNT]) {
    const FemState *s = fem_state();
    const JsonValue *conds = fem_conditions();
    int supports = 0, loads = 0;
    for (size_t i = 0; i < json_len(conds); i++) {
        const char *k = json_get_str(json_at(conds, i), "kind", "");
        if (!strcmp(k, "fixed") || !strcmp(k, "displacement") || !strcmp(k, "frictionless_support")) supports++;
        else loads++;
    }
    memset(info, 0, sizeof(StepInfo) * STEP_COUNT);

    if (fem_pending_import()) {
        info[STEP_PART].state = ST_ATTENTION;
        str_copy(info[STEP_PART].line, sizeof info[STEP_PART].line, "which unit?");
    } else if (s->nbodies > 0) {
        info[STEP_PART].state = s->closed_solid ? ST_DONE : ST_ATTENTION;
        snprintf(info[STEP_PART].line, sizeof info[STEP_PART].line, "%s \xC2\xB7 %u tri", s->body, s->triangles);
    } else {
        str_copy(info[STEP_PART].line, sizeof info[STEP_PART].line, "open an STL");
    }

    if (s->meshed) {
        info[STEP_MESH].state = s->mesh_current ? ST_DONE : ST_ATTENTION;
        snprintf(info[STEP_MESH].line, sizeof info[STEP_MESH].line, "%d el \xC2\xB7 %.3g mm%s", s->nelems,
                 fem_project_mesh_mm(), s->mesh_current ? "" : " stale");
    } else if (s->nbodies > 0) {
        snprintf(info[STEP_MESH].line, sizeof info[STEP_MESH].line, "suggest %.3g mm", fem_suggested_mesh_mm());
    }

    if (s->material[0]) {
        info[STEP_MATERIAL].state = ST_DONE;
        snprintf(info[STEP_MATERIAL].line, sizeof info[STEP_MATERIAL].line, "%s", s->material_name);
    } else if (s->nbodies > 0) {
        str_copy(info[STEP_MATERIAL].line, sizeof info[STEP_MATERIAL].line, "choose one");
    }

    if (solid_build_path) { /* step 4 is the build: its inputs all have defaults, so it is ready as soon as there is a mesh */
        info[STEP_HOLD].state = s->meshed ? ST_DONE : ST_TODO;
        snprintf(info[STEP_HOLD].line, sizeof info[STEP_HOLD].line, "%s \xC2\xB7 %s", build_kind ? "FFF" : "LPBF",
                 build_kind ? "filament" : "typed strain");
    } else if (supports && loads) {
        info[STEP_HOLD].state = ST_DONE;
        snprintf(info[STEP_HOLD].line, sizeof info[STEP_HOLD].line, "%d held \xC2\xB7 %d loaded", supports, loads);
    } else if (supports || loads) {
        info[STEP_HOLD].state = ST_ATTENTION;
        snprintf(info[STEP_HOLD].line, sizeof info[STEP_HOLD].line, supports ? "%d held, no load" : "%d loads, nothing held",
                 supports ? supports : loads);
    } else if (s->meshed) {
        str_copy(info[STEP_HOLD].line, sizeof info[STEP_HOLD].line, "click a face");
    }

    if (s->job_active) {
        info[STEP_SOLVE].state = ST_ATTENTION;
        snprintf(info[STEP_SOLVE].line, sizeof info[STEP_SOLVE].line, "solving %.0f%%", s->job_progress * 100);
    } else if (s->have_result) {
        info[STEP_SOLVE].state = ST_DONE;
        str_copy(info[STEP_SOLVE].line, sizeof info[STEP_SOLVE].line, "solved");
    } else if (s->job_error[0]) {
        info[STEP_SOLVE].state = ST_ATTENTION;
        str_copy(info[STEP_SOLVE].line, sizeof info[STEP_SOLVE].line, "did not finish");
    }

    if (s->have_result) {
        info[STEP_RESULTS].state = ST_DONE;
        snprintf(info[STEP_RESULTS].line, sizeof info[STEP_RESULTS].line, "%.4g %s peak", s->peak,
                 fem_field_unit(fem_field()));
    }
}

/* the first step that still needs work; the panel follows it unless the user pinned another one */
static int solid_first_open(const StepInfo info[STEP_COUNT]) {
    for (int i = 0; i < STEP_COUNT; i++)
        if (info[i].state != ST_DONE) return i;
    return STEP_RESULTS;
}

static const char *solid_hint(const StepInfo info[STEP_COUNT], int step) {
    const FemState *s = fem_state();
    switch (step) {
    case STEP_PART:
        if (fem_pending_import()) return "Which unit is the file written in? Choose mm, cm or inch.";
        if (s->nbodies == 0 && s->have_result && s->result_body[0])
            return "The result shown was built on this project's part; the project file itself lists no part.";
        if (s->nbodies == 0) return "Press OPEN, or drag an STL onto the window.";
        return "Check the verdict, then go to MESH.";
    case STEP_MESH:
        return s->meshed && s->mesh_current ? "Mesh is ready. Choose a material next."
                                            : "Accept the suggested element size or move the slider, then press GENERATE.";
    case STEP_MATERIAL:
        if (!s->material[0]) return "Choose a material for the part.";
        return solid_build_path ? "Material assigned. Set the build up next." : "Material assigned. Hold a face next.";
    case STEP_HOLD:
        if (solid_build_path)
            return build_kind == 1 ? "Review deposition and cooling settings. Every value shows its unit; RUN PRINT is in step 5."
                                   : "Choose the machine, the inherent strain and the cut. Every value shows its unit; RUN BUILD is in step 5.";
        if (info[STEP_HOLD].state == ST_DONE) return "Held and loaded. Press SOLVE.";
        return "Click a face on the part, then press HOLD; click another face, type a force and press LOAD.";
    case STEP_SOLVE:
        if (s->job_active) return solid_build_path ? "Printing. The window stays live; the part appears layer by layer."
                                                   : "Solving. The window stays live; the result appears by itself.";
        if (solid_build_path) return info[STEP_HOLD].state == ST_DONE ? (build_kind == 1 ? "Press RUN PRINT." : "Press RUN BUILD.") : "Mesh the part first.";
        return info[STEP_HOLD].state == ST_DONE ? "Press SOLVE." : "Hold and load the part first.";
    default:
        if (!s->have_result) return solid_build_path ? "Run the build first." : "Solve first.";
        return solid_build_path ? (build_kind == 1 ? "Press PLAY PRINT to watch it grow; the deflections are at true scale."
                                                  : "Press PLAY BUILD to watch it grow; the deflections are at true scale.")
                                : strcmp(s->result_kind, "static_structural") ? "Explore the stored times below. REPORT saves the result and its setup."
                                                                            : "Read the numbers, then press CHECK MESH and REPORT.";
    }
}

/* ---- 1 PART ------------------------------------------------------------------------------------------------- */

static float step_part(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    const float gap = 4, bh = 22;
    char a[220];
    const char *waiting = fem_pending_import();
    if (waiting) {
        const char *base = strrchr(waiting, '/');
        snprintf(a, sizeof a, "%s", base ? base + 1 : waiting);
        ui_text(ui, FONT_SMALL, x, y, UI_TEXT, a);
        y += 18;
        ui_text(ui, FONT_SMALL, x, y, UI_ACCENT2, "Which unit are its numbers in? It is never guessed.");
        y += 20;
        float bw = (w - 2 * gap) / 3;
        static const struct { const char *label, *unit; } U[3] = {{"MM##unit", "mm"}, {"CM##unit", "cm"}, {"INCH##unit", "in"}};
        for (int i = 0; i < 3; i++) {
            char cmd[48];
            snprintf(cmd, sizeof cmd, "solid import %s", U[i].unit);
            if (ui_button(ui, U[i].label, x + i * (bw + gap), y, bw, bh, false)) exec_cmd(cmd);
        }
        y += bh + 6;
        if (ui_button(ui, "CANCEL##unit", x, y, bw, bh, false)) exec_cmd("solid drop");
        return y + bh + 4;
    }
    if (s->nbodies == 0 && s->have_result && s->result_body[0]) {
        /* a build run through the operations and never saved: project.json lists no body, the run names it */
        snprintf(a, sizeof a, "The result shown was built on '%s'. The project file lists no part, because the project "
                              "was not saved after the part was imported: open the STL again to change anything.", s->result_body);
        y = wrap_text(ui, x, y, w, UI_DIM, a, 4) + 6;
    } else if (s->nbodies == 0) {
        y = wrap_text(ui, x, y, w, UI_DIM, "Open an STL of the part, or drag it onto the window.", 2) + 6;
    } else {
        snprintf(a, sizeof a, "%.4g \xC3\x97 %.4g \xC3\x97 %.4g mm", s->size_mm[0], s->size_mm[1], s->size_mm[2]);
        kv(ui, x, y, w, "size", a, UI_TEXT);
        y += 17;
        /* A volume is the space a closed surface encloses. An open one encloses nothing, and the signed sum comes
         * out as some number, often zero: say that instead of showing it. */
        if (s->closed_solid || s->volume_mm3 > 0)
            snprintf(a, sizeof a, "%.4g mm\xC2\xB3 \xC2\xB7 %u triangles", s->volume_mm3, s->triangles);
        else
            snprintf(a, sizeof a, "not available until repaired \xC2\xB7 %u triangles", s->triangles);
        kv(ui, x, y, w, "volume", a, s->closed_solid || s->volume_mm3 > 0 ? UI_TEXT : UI_ACCENT2);
        y += 19;
        /* the verdict, in words, from the same diagnostics the operations report */
        if (s->closed_solid)
            snprintf(a, sizeof a, "Closed solid: the mesher can fill it.%s",
                     s->degenerate_removed ? " Degenerate triangles were removed on import." : "");
        else
            snprintf(a, sizeof a, "Not watertight: %d open and %d non-manifold edges. It is still meshed: a cell is "
                                  "inside when two of three rays say so, and a ray that crosses the surface an odd "
                                  "number of times does not vote. MESH says how many cells were close calls.",
                     s->open_edges, s->nonmanifold_edges);
        y = wrap_text(ui, x, y, w, s->closed_solid ? UI_GOOD : UI_ACCENT2, a, 5) + 2;
        if (s->repaired) { /* what the repair changed stays on screen: it changed the part */
            snprintf(a, sizeof a, "Repaired: %d facets dropped with %d stray shell(s), %d hole(s) filled with %d "
                                  "facets. %d open and %d non-manifold edges left.",
                     s->repair_facets_dropped, s->repair_shells_dropped, s->repair_holes_filled,
                     s->repair_facets_added, s->open_edges, s->nonmanifold_edges);
            y = wrap_text(ui, x, y, w, UI_ACCENT2, a, 4) + 2;
        }
        if (!s->closed_solid && !s->repaired)
            y = wrap_text(ui, x, y, w, UI_DIM,
                          "REPAIR drops stray shells and fills small holes, and says in numbers what it changed.", 2) + 2;
        if (s->components > 1 || s->nested_shells > 0) {
            snprintf(a, sizeof a, "%d separate components%s: all of them are meshed and analysed together.",
                     s->components, s->nested_shells > 0 ? ", one inside another" : "");
            y = wrap_text(ui, x, y, w, UI_DIM, a, 2) + 2;
        }
        if (s->min_thickness_mm > 0) {
            snprintf(a, sizeof a, "thinnest wall %.3g mm: the element size must be well below it", s->min_thickness_mm);
            y = wrap_text(ui, x, y, w, UI_DIM, a, 2) + 2;
        }
        y += 6;
        /* the two questions this part can be asked. The rest of the strip follows the answer. */
        ui_text(ui, FONT_SMALL, x, y, UI_DIM, "what do you want to know about it?");
        y += 16;
        float qw = (w - gap) / 2;
        if (ui_button(ui, "ANALYSE STRESS##path", x, y, qw, bh, !solid_build_path)) {
            solid_build_path = false;
            solid_step_pinned = false;
        }
        if (ui_button(ui, "SIMULATE THE BUILD##path", x + qw + gap, y, qw, bh, solid_build_path)) {
            solid_build_path = true;
            solid_step_pinned = false;
        }
        y += bh + 4;
        y = wrap_text(ui, x, y, w, UI_FAINT,
                      solid_build_path ? "What the printer does to it: the part grows layer by layer and distorts."
                                       : "What it carries in service: hold faces, load faces, solve.", 2) + 4;
    }
    float bw = (w - 2 * gap) / 3;
    if (s->nbodies && !s->closed_solid) {
        if (step_button(ui, "REPAIR##part", x, y, bw, bh, false, NULL)) exec_cmd("solid repair");
        ui_text(ui, FONT_SMALL, x + bw + gap + 2, y + 5, UI_FAINT, "drop stray shells, fill small holes");
        y += bh + 6;
    }
    if (ui_button(ui, "OPEN##part", x, y, bw, bh, false)) exec_cmd("open");
    if (ui_button(ui, "FROM TUNNEL##part", x + bw + gap, y, bw, bh, false)) exec_cmd("solid import");
    if (step_button(ui, "TO TUNNEL##part", x + 2 * (bw + gap), y, bw, bh, false,
                    s->nbodies ? NULL : "there is no part yet to send to the tunnel"))
        exec_cmd("solid export");
    y += bh + 6;
    /* the project a part belongs to: made for you when you open a file, and still yours to choose */
    ui_text(ui, FONT_SMALL, x, y + 5, UI_FAINT, s->have_project ? s->project : "");
    if (ui_button(ui, "NEW PROJECT##proj", x + w - 2 * bw - gap, y, bw, bh, false)) exec_cmd("solid new");
    if (ui_button(ui, "OPEN...##proj", x + w - bw, y, bw, bh, false)) exec_cmd("solid open");
    return y + bh + 4;
}

static bool g_mesh_tet, g_mesh_tet_touched; /* the MESH step's choice of tetrahedra, and whether the user made it */

/* ---- 2 MESH ------------------------------------------------------------------------------------------------- */

static float step_mesh(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    const float bh = 22;
    char a[220];
    if (s->nbodies == 0) {
        y = wrap_text(ui, x, y, w, UI_DIM, "A part is needed before it can be meshed.", 1) + 6;
        return y;
    }
    /* which mesh: the voxel grid every print analysis runs on, or tetrahedra that follow the surface */
    if (s->meshed && !g_mesh_tet_touched) g_mesh_tet = s->mesh_method == 1;
    {
        float bw = (w - 4) / 2;
        if (ui_button(ui, "VOXELS##meshm", x, y, bw, bh, !g_mesh_tet)) g_mesh_tet = false, g_mesh_tet_touched = true;
        if (ui_button(ui, "TETRAHEDRA##meshm", x + bw + 4, y, bw, bh, g_mesh_tet)) g_mesh_tet = true, g_mesh_tet_touched = true;
        y += bh + 4;
        y = wrap_text(ui, x, y, w, UI_DIM,
                      g_mesh_tet ? "Tetrahedra follow the part's surface: holes stay round and fillets keep their shape, which is where parts "
                                   "break. 10-node elements, for the structural analysis."
                                 : "Voxels: boxes on the build layers. The print analyses need them; curved faces become steps.",
                      3) + 4;
    }
    double mm = fem_mesh_size_mm();
    double v = mm;
    snprintf(a, sizeof a, "%.3g mm", mm);
    if (slider_row(ui, "sl_elem", g_mesh_tet ? "surface cell" : "element size", a, x, y, w - 92, &v, 0.2, 20.0, true)) {
        fem_set_mesh_size_mm(v);
        field_clear("element size"); /* the slider is now what the size says */
    }
    double typed = field_num("element size", 0);
    if (typed > 0) mm = typed;      /* a typed size wins: a mesh study wants an exact number, not a slider position */
    if (ui_button(ui, "GENERATE##mesh", x + w - 86, y + 4, 86, bh, false)) {
        fem_set_mesh_size_mm(mm);
        /* one halving where a wall is thinner than 1.5 cells keeps thin walls without the element count of two */
        if (g_mesh_tet) {
            if (fem_op("mesh_generate", "{\"method\": \"tet\", \"surface_size\": %.6g, \"order\": 2, \"thin_wall_levels\": 1}", mm)) {
                const JsonValue *me = json_get(fem_last_value(), "mesh");
                LOGOK("tetrahedral %s mesh: %lld elements, %lld nodes, surface %.3g mm, dihedral %.1f to %.1f degrees",
                      !strcmp(json_get_str(me, "method", ""), "tet10") ? "TET10" : "TET4", json_get_int(me, "elements", 0),
                      json_get_int(me, "nodes", 0), json_get_num(me, "surface_size_mm", 0),
                      json_get_num(json_get(me, "quality"), "min_dihedral_deg", 0), json_get_num(json_get(me, "quality"), "max_dihedral_deg", 0));
            }
        }
        else fem_op("mesh_generate", "{\"element_size\": %.6g}", mm);
    }
    y += 32;
    snprintf(a, sizeof a, "suggested %.3g mm", fem_suggested_mesh_mm());
    ui_text(ui, FONT_SMALL, x, y + 5, UI_DIM, a);
    ui_text(ui, FONT_SMALL, x + 120, y + 5, UI_FAINT, "or type it");
    number_field(ui, "element size", x + 180, y, 70, bh, "");
    ui_text(ui, FONT_SMALL, x + 256, y + 5, UI_FAINT, "mm");
    y += bh + 6;
    if (s->meshed && s->mesh_method == 1) {
        snprintf(a, sizeof a, "%d elements \xC2\xB7 %d nodes", s->nelems, s->nnodes);
        kv(ui, x, y, w, s->tet_order == 2 ? "10-node tetrahedra" : "4-node tetrahedra", a, s->mesh_current ? UI_TEXT : UI_ACCENT2);
        y += 17;
        snprintf(a, sizeof a, "%.0f to %.0f degrees", s->tet_min_dihedral, s->tet_max_dihedral);
        kv(ui, x, y, w, "angles between element faces", a, s->tet_min_dihedral >= 10 ? UI_GOOD : UI_ACCENT2);
        y += 17;
        snprintf(a, sizeof a, "%+.2f %%", s->mesh_volume_error_pct);
        kv(ui, x, y, w, "volume against the STL", a, fabs(s->mesh_volume_error_pct) < 2 ? UI_GOOD : UI_ACCENT2);
        y += 19;
        char words[400];
        if (s->tet_unresolved_thin > 0) {
            char thin[320];
            snprintf(thin, sizeof thin,
                     "Walls thinner than about %.3g mm show holes: the finest cell here is %.3g mm and %d places are still thinner than 1.5 of it. "
                     "The volume is %+.1f %% against the STL. Mesh again with a surface cell of %.3g mm to resolve them.",
                     1.5 * s->tet_finest_mm, s->tet_finest_mm, s->tet_unresolved_thin, s->mesh_volume_error_pct, 0.5 * s->tet_finest_mm);
            y = wrap_text(ui, x, y, w, UI_ACCENT2, thin, 4) + 2;
        }
        char edges[96];
        if (s->tet_sharp_kept) snprintf(edges, sizeof edges, "its sharp edges and corners are kept");
        else snprintf(edges, sizeof edges, "sharp edges are rounded over about %.2g mm", fem_project_mesh_mm());
        snprintf(words, sizeof words, "%s The surface nodes lie on the STL; %s.%s",
                 s->tet_min_dihedral >= 10 ? "Every element is well shaped (a regular one has 70.5 degrees between faces)."
                                           : "Some elements are flat, and stresses in them are less accurate.",
                 edges, s->tet_regions > 1 ? " The mesh is in more than one piece: a feature is thinner than the finest cell." : "");
        y = wrap_text(ui, x, y, w, s->tet_min_dihedral >= 10 && s->tet_regions <= 1 ? UI_DIM : UI_ACCENT2, words, 4) + 2;
        if (!s->mesh_current)
            y = wrap_text(ui, x, y, w, UI_ACCENT2, "The part changed after this mesh was made: press GENERATE again.", 2) + 2;
    } else if (s->meshed) {
        snprintf(a, sizeof a, "%d elements \xC2\xB7 %d nodes", s->nelems, s->nnodes);
        kv(ui, x, y, w, "hex mesh", a, s->mesh_current ? UI_TEXT : UI_ACCENT2);
        y += 17;
        snprintf(a, sizeof a, "%+.2f %%", s->mesh_volume_error_pct);
        kv(ui, x, y, w, "volume against the STL", a, fabs(s->mesh_volume_error_pct) < 2 ? UI_GOOD : UI_ACCENT2);
        y += 19;
        if (fabs(s->mesh_volume_error_pct) >= 2)
            y = wrap_text(ui, x, y, w, UI_ACCENT2,
                          "The staircase boundary costs volume: finer elements bring it down. CHECK MESH after the "
                          "solve tells you whether it changed the answer.", 3) + 2;
        if (!s->mesh_current)
            y = wrap_text(ui, x, y, w, UI_ACCENT2, "The part changed after this mesh was made: press GENERATE again.", 2) + 2;
    }
    return y + 4;
}

/* ---- 3 MATERIAL --------------------------------------------------------------------------------------------- */

static void source_sentence(char *out, size_t cap, const char *src);

static float step_material(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    const float gap = 4, bh = 22;
    const SolidMat *mats;
    int nm = solid_materials(&mats);
    if (s->material_name[0]) {
        char line[300];
        y = wrap_text(ui, x, y, w, UI_ACCENT, s->material_name, 2);
        /* the record's own words about where its numbers come from, not a sentence the panel made up */
        snprintf(line, sizeof line, "status: %s", s->material_status[0] ? s->material_status : "not stated");
        y = wrap_text(ui, x, y, w, UI_DIM, line, 1);
        if (s->material_source[0]) {
            /* the first sentence names the documents; every value's own source is one command away */
            char first[400], src[700];
            source_sentence(first, sizeof first, s->material_source);
            snprintf(src, sizeof src, "source: %s. Each value's own source: am materials_list id=%s", first, s->material);
            y = wrap_text(ui, x, y, w, UI_DIM, src, 4);
        }
        if (!strcmp(s->material_status, "demonstration"))
            y = wrap_text(ui, x, y, w, UI_ACCENT2,
                          "Demonstration values: not traceable to a grade or a test, not for design. Define your own "
                          "with 'am material_define' to get numbers you can rely on.", 3) + 4;
        else
            y += 4;
    }
    if (s->material[0]) {
        char gaps[400];
        material_gaps(s->material, gaps, sizeof gaps);
        if (gaps[0]) y = wrap_text(ui, x, y, w, UI_ACCENT2, gaps, 4) + 4;
    }
    /* grouped by the process the record is for; a dot says how far to trust it: green measured or published with a
     * source on every value, amber demonstration */
    int cols = 3;
    float bw = (w - gap * (cols - 1)) / cols;
    for (int g = 0; g < 3; g++) {
        int col = 0, any = 0;
        for (int i = 0; i < nm; i++) {
            if (mats[i].group != g) continue;
            if (!any) {
                y = wrap_text(ui, x, y, w, UI_DIM, MAT_GROUP_TITLE[g], 1) + 2; /* in the panel text too, for uitext */
                any = 1;
            }
            char label[80];
            snprintf(label, sizeof label, "%s##mat%d", mats[i].label, i);
            float bx = x + col * (bw + gap);
            if (step_button(ui, label, bx, y, bw, bh, !strcmp(s->material, mats[i].id),
                            s->nbodies ? NULL : "there is no part to give a material to"))
                fem_op("material_assign", "{\"body\": \"%s\", \"material\": \"%s\", \"source\": \"user\"}",
                       s->body[0] ? s->body : "part", mats[i].id);
            bool sourced = !strcmp(mats[i].status, "measured") || !strcmp(mats[i].status, "published");
            ui_rect(ui, bx + bw - 9, y + 4, 5, 5, sourced ? UI_GOOD : UI_ACCENT2, 2.5f);
            if (++col == cols) col = 0, y += bh + gap;
        }
        if (any && col) y += bh + gap;
        if (any) y += 4;
    }
    y = wrap_text(ui, x, y, w, UI_FAINT, "Green dot: every value names its source. Amber: demonstration values.", 2);
    return y + 4;
}

/* ---- 4 HOLD & LOAD ------------------------------------------------------------------------------------------ */

static int solid_load_dir;  /* 0 normal, 1 -normal, 2 +x, 3 +y, 4 +z */

static float step_hold(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    const float gap = 4, bh = 22;
    char a[220];
    if (!s->meshed)
        y = wrap_text(ui, x, y, w, UI_ACCENT2, "Mesh the part first: conditions are put on its faces and carried to "
                                               "the mesh.", 2) + 4;

    /* what is picked right now */
    int np = fem_picked_count();
    if (np > 0) {
        const double *n = fem_picked_normal();
        snprintf(a, sizeof a, "%d face%s \xC2\xB7 %.4g mm\xC2\xB2", np, np == 1 ? "" : "s", fem_picked_area_mm2());
        kv(ui, x, y, w, "picked", a, UI_ACCENT);
        y += 17;
        snprintf(a, sizeof a, "%.3f, %.3f, %.3f", n[0], n[1], n[2]);
        kv(ui, x, y, w, "outward normal", a, UI_DIM);
        y += 19;
    } else {
        y = wrap_text(ui, x, y, w, UI_DIM,
                      "Click a face on the part to pick it; click it again or shift-click to drop it.", 2) + 4;
    }

    const char *why = s->nbodies == 0 ? "there is no part yet" : np == 0 ? "click a face on the part first" : NULL;
    float bw = (w - 2 * gap) / 3;
    if (step_button(ui, "HOLD##bc", x, y, bw, bh, false, why)) fem_apply_hold();
    if (step_button(ui, "CLEAR##bc", x + bw + gap, y, bw, bh, false, np ? NULL : "nothing is picked")) fem_clear_pick();
    if (step_button(ui, "GRAVITY##bc", x + 2 * (bw + gap), y, bw, bh, false,
                    s->nbodies ? NULL : "there is no part yet"))
        fem_apply_gravity();
    y += bh + 6;
    /* a rectangle takes many faces at once, which is the only sane way to hold a row of bolt holes */
    if (ui_button(ui, "BOX##pickbox", x, y, bw, bh, solid_box_pick)) solid_box_pick = !solid_box_pick;
    if (ui_button(ui, "FRONT ONLY##pickfront", x + bw + gap, y, bw, bh, solid_box_front))
        solid_box_front = !solid_box_front;
    ui_text(ui, FONT_SMALL, x + 2 * (bw + gap) + 2, y + 5, UI_FAINT,
            solid_box_pick ? "drag; shift-drag drops" : "many faces at once");
    y += bh + 4;
    y = wrap_text(ui, x, y, w, UI_FAINT,
                  solid_box_front ? "The box takes only the faces turned towards you."
                                  : "The box takes every face whose centre is inside it, the ones behind the part "
                                    "as well.", 2) + 4;

    /* the load: a number, a unit, a direction */
    ui_text(ui, FONT_SMALL, x, y + 5, UI_DIM, "force");
    number_field(ui, "load magnitude", x + 44, y, 92, bh, "0");
    ui_text(ui, FONT_SMALL, x + 142, y + 5, UI_DIM, "N");
    static const char *const DIR[5] = {"NORMAL##dir", "-NORMAL##dir", "X##dir", "Y##dir", "Z##dir"};
    float dw = 44;
    for (int i = 0; i < 5; i++)
        if (ui_button(ui, DIR[i], x + 160 + i * (dw + 2), y, dw, bh, solid_load_dir == i)) solid_load_dir = i;
    y += bh + 6;
    double newtons = field_num("load magnitude", 0);
    const char *lwhy = np == 0 ? "click a face on the part first" : !(fabs(newtons) > 0) ? "type the force in newtons" : NULL;
    if (step_button(ui, "LOAD##bc", x, y, bw, bh, false, lwhy)) {
        if (fem_apply_load(newtons, solid_load_dir)) field_clear("load magnitude");
    }
    if (step_button(ui, "VALIDATE##bc", x + bw + gap, y, bw, bh, false, s->meshed ? NULL : "mesh the part first"))
        fem_op("setup_validate", "{\"analysis\": \"%s\"}", SOLID_ANALYSIS[solid_analysis]);
    y += bh + 10;

    /* what is already on the part */
    const JsonValue *list = fem_conditions();
    size_t nc = json_len(list);
    if (nc == 0) {
        y = wrap_text(ui, x, y, w, UI_DIM, "Nothing is held and nothing is loaded yet.", 1) + 4;
    }
    for (size_t i = 0; i < nc && i < 8; i++) {
        const JsonValue *c = json_at(list, i);
        const char *name = json_get_str(c, "name", "");
        const char *kind = json_get_str(c, "kind", "");
        const char *sel = json_get_str(c, "selection", "");
        snprintf(a, sizeof a, "%s%s%s", kind, sel[0] ? " \xC2\xB7 " : "", sel);
        ui_text(ui, FONT_SMALL, x, y + 1, UI_DIM, name);
        ui_text_right(ui, FONT_SMALL, x + w - 22, y + 1, UI_TEXT, a);
        char id[96];
        snprintf(id, sizeof id, "\xC3\x97##bc%zu", i);
        if (ui_button(ui, id, x + w - 18, y - 1, 18, 16, false))
            fem_op("boundary_remove", "{\"name\": \"%s\"}", name);
        y += 18;
    }
    return y + 4;
}

/* ---- 4 BUILD: what the printer does to the part ------------------------------------------------------------- */

/* Every input the two build operations need, as the interface holds it. The operations refuse a value without a
 * provenance, so the panel never sends one without: the typed strain carries the word the user picked, and the cut
 * carries "assumed" because no source states a cut height and kerf for a general part. */
/* the window follows the job it just started itself, the way pressing SOLVE does */
static bool build_follow(bool started) {
    if (!started) return false;
    const char *id = json_get_str(fem_last_value(), "job_id", NULL);
    if (id) {
        fem_follow_job(id);
        LOGOK("build submitted as %s", id);
    }
    return true;
}

static bool build_run_now(double layer_mm) {
    const FemState *s = fem_state();
    char params[1400];
    if (build_kind == 0) {
        char strain[300];
        snprintf(strain, sizeof strain,
                 "\"exx\": %.10g, \"eyy\": %.10g, \"ezz\": %.10g, \"provenance\": \"%s\", "
                 "\"source\": \"manual inputs; provenance explicitly selected in the interface\"",
                 field_num("exx", 0), field_num("eyy", 0), field_num("ezz", -0.03), BUILD_PROV[build_prov]);
        char cut[220] = "";
        if (build_cut)
            snprintf(cut, sizeof cut,
                     ", \"cut\": {\"height\": \"%.10g mm\", \"kerf\": \"%.10g mm\", \"from_x\": \"%.10g mm\", "
                     "\"provenance\": \"assumed\"}",
                     field_num("cut height", 2.5), field_num("kerf", 0.3), field_num("cut from", 10.0));
        snprintf(params, sizeof params,
                 "{\"body\": \"%s\", \"build_orientation\": \"%c\", \"layer_thickness_sim\": \"%.10g mm\", "
                 "\"inherent_strain\": {%s}, "
                 "\"material\": {\"youngs_modulus\": \"%.10g MPa\", \"poissons_ratio\": %.10g, \"provenance\": \"user\"}%s, "
                 "\"label\": \"build from the OpenPhysicsAI interface\"}",
                 s->body, build_orient ? 'Y' : 'X', layer_mm, strain,
                 field_num("youngs", (s->youngs_gpa > 0 ? s->youngs_gpa : 70.0) * 1000.0),
                 field_num("poisson", s->poisson > 0 ? s->poisson : 0.33), cut);
        return build_follow(fem_op("lpbf_build_run", "%s", params));
    }
    snprintf(params, sizeof params,
             "{\"body\": \"%s\", \"process\": {\"layer_height\": \"%.10g mm\", \"printed_layer_height\": \"%.10g mm\", "
             "\"nozzle_temperature\": \"%.10g degC\", \"bed_temperature\": \"%.10g degC\", "
             "\"ambient_temperature\": \"%.10g degC\", \"deposition_rate\": \"%.10g mm^3/s\", "
             "\"provenance\": \"%s\"}, \"label\": \"print from the OpenPhysicsAI interface\"}",
             s->body, layer_mm, field_num("printed layer", 0.2), field_num("nozzle", 210.0),
             field_num("bed", 60.0), field_num("ambient", 30.0), field_num("rate", 8.0), BUILD_PROV[fdm_prov]);
    return build_follow(fem_op("mech_print_run", "%s", params));
}

/* one labelled number with its unit, laid out so a row of them lines up */
static void build_field(Ui *ui, float x, float y, float lw, float fw, const char *label, const char *id,
                        const char *deflt, const char *unit) {
    ui_text(ui, FONT_SMALL, x, y + 5, UI_DIM, label);
    number_field(ui, id, x + lw, y, fw, 22, deflt);
    ui_text(ui, FONT_SMALL, x + lw + fw + 6, y + 5, UI_FAINT, unit);
}

static float step_build(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    const float gap = 4, bh = 22;
    char a[260];

    /* the two job kinds the engine has, named by what the machine is */
    float bw = (w - gap) / 2;
    if (ui_button(ui, "LPBF METAL##bk", x, y, bw, bh, build_kind == 0)) build_kind = 0;
    if (ui_button(ui, "FDM / FFF PLASTIC##bk", x + bw + gap, y, bw, bh, build_kind == 1)) build_kind = 1;
    y += bh + 8;

    if (build_kind == 0) {
        panel_say("Strain provenance: %s\n", BUILD_PROV[build_prov]);
        y = wrap_text(ui, x, y, w, UI_DIM,
                      "Every simulation layer is laid stress-free on the part that has already distorted below it, "
                      "then contracts by the inherent strain. This is a declared process input; predictive use "
                      "requires calibration for the process and geometry.", 4) + 4;
        y = wrap_text(ui, x, y, w, UI_ACCENT2, "The starting strain is an example, not a fit to this printer. Review it before running.", 2) + 4;
        ui_text(ui, FONT_SMALL, x, y + 5, UI_DIM, "orientation");
        if (ui_button(ui, "X##ori", x + 80, y, 44, bh, build_orient == 0)) build_orient = 0;
        if (ui_button(ui, "Y##ori", x + 80 + 48, y, 44, bh, build_orient == 1)) build_orient = 1;
        ui_text(ui, FONT_SMALL, x + 180, y + 5, UI_FAINT, "machine axis of the long side");
        y += bh + 6;
        build_field(ui, x, y, 80, 70, "layer", "layer", "1", "mm (simulation layer)");
        y += bh + 8;

        /* the inherent strain: three numbers with a provenance. No named strain set ships with the engine. */
        ui_text(ui, FONT_SMALL, x, y, UI_DIM, "inherent strain");
        y += 16;
        {
            float fw = (w - 2 * gap) / 3 - 30;
            build_field(ui, x, y, 30, fw, "exx", "exx", "0", "");
            build_field(ui, x + (fw + 30 + gap), y, 30, fw, "eyy", "eyy", "0", "");
            build_field(ui, x + 2 * (fw + 30 + gap), y, 30, fw, "ezz", "ezz", "-0.03", "");
            y += bh + 6;
            ui_text(ui, FONT_SMALL, x, y + 5, UI_DIM, "provenance");
            float pw = (w - 80 - 2 * gap) / 3;
            for (int i = 0; i < 3; i++) {
                char label[40];
                snprintf(label, sizeof label, "%c%s##prov", (char)toupper((unsigned char)BUILD_PROV[i][0]), BUILD_PROV[i] + 1);
                if (ui_button(ui, label, x + 80 + i * (pw + gap), y, pw, bh, build_prov == i)) build_prov = i;
            }
            y += bh + 6;
        }

        /* the cut that releases the part from the plate */
        if (ui_button(ui, build_cut ? "CUT ON##cut" : "CUT OFF##cut", x, y, bw, bh, build_cut)) build_cut = !build_cut;
        ui_text(ui, FONT_SMALL, x + bw + gap + 2, y + 5, UI_FAINT, "the wire that frees the part");
        y += bh + 6;
        if (build_cut) {
            float lw = 64, fw = (w - 3 * gap) / 3 - lw;
            build_field(ui, x, y, lw, fw, "height", "cut height", "2.5", "mm");
            y += bh + 4;
            build_field(ui, x, y, lw, fw, "kerf", "kerf", "0.3", "mm");
            y += bh + 4;
            build_field(ui, x, y, lw, fw, "from", "cut from", "10", "mm from the fixed end");
            y += bh + 6;
            y = wrap_text(ui, x, y, w, UI_DIM,
                          "The cut height, kerf and start are assumed, not published: the summary and the report say so.", 2) + 2;
        }
        /* the elastic constants: the assigned material's, or the ones this build is actually for. A body loaded only
         * by an eigenstrain has a displacement that does not depend on the modulus; the stresses do. */
        char edef[24], nudef[24];
        snprintf(edef, sizeof edef, "%.6g", s->youngs_gpa > 0 ? s->youngs_gpa * 1000.0 : 70000.0);
        snprintf(nudef, sizeof nudef, "%.4g", s->poisson > 0 ? s->poisson : 0.33);
        build_field(ui, x, y, 22, 92, "E", "youngs", edef, "MPa");
        build_field(ui, x + 190, y, 20, 80, "\xCE\xBD", "poisson", nudef, "");
        y += bh + 6;
    } else {
        panel_say("Process provenance: %s\n", BUILD_PROV[fdm_prov]);
        y = wrap_text(ui, x, y, w, UI_DIM,
                      "Each layer is deposited hot on the layers below and cools; the part distorts as it cools. "
                      "The starting settings are examples, not a calibrated printer profile. Review every value before running.", 4) + 4;
        float lw = 92, fw = 76;
        build_field(ui, x, y, lw, fw, "layer", "layer", "1", "mm (simulation)");
        y += bh + 4;
        build_field(ui, x, y, lw, fw, "printed layer", "printed layer", "0.2", "mm (machine)");
        y += bh + 4;
        build_field(ui, x, y, lw, fw, "nozzle", "nozzle", "210", "degC");
        y += bh + 4;
        build_field(ui, x, y, lw, fw, "bed", "bed", "60", "degC");
        y += bh + 4;
        build_field(ui, x, y, lw, fw, "ambient", "ambient", "30", "degC");
        y += bh + 4;
        build_field(ui, x, y, lw, fw, "rate", "rate", "8", "mm\xC2\xB3/s");
        y += bh + 6;
        ui_text(ui, FONT_SMALL, x, y + 5, UI_DIM, "provenance");
        float pw = (w - 80 - 2 * gap) / 3;
        for (int i = 0; i < 3; i++) {
            char label[48];
            snprintf(label, sizeof label, "%c%s##fdmprov", (char)toupper((unsigned char)BUILD_PROV[i][0]), BUILD_PROV[i] + 1);
            if (ui_button(ui, label, x + 80 + i * (pw + gap), y, pw, bh, fdm_prov == i)) fdm_prov = i;
        }
        y += bh + 6;
    }

    /* where the defaults on this page come from */
    snprintf(a, sizeof a, "Material properties use the assigned material: %s. Process settings are examples until reviewed.",
             s->material_name[0] ? s->material_name : "none assigned yet");
    y = wrap_text(ui, x, y, w, s->material[0] ? UI_DIM : UI_ACCENT2, a, 3);
    return y + 4;
}

/* ---- 5 SOLVE ------------------------------------------------------------------------------------------------ */

static float step_solve(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    const float gap = 4, bh = 22;
    char a[220];
    static const char *const LABEL[3] = {"STATIC##an", "THERMAL##an", "THERMO-MECH##an"};
    float bw = (w - 2 * gap) / 3;
    if (!solid_build_path) { /* the build chose its own analysis in step 4 */
        for (int i = 0; i < 3; i++)
            if (ui_button(ui, LABEL[i], x + i * (bw + gap), y, bw, bh, solid_analysis == i)) solid_analysis = i;
        y += bh + 8;
    }
    if (!solid_build_path && solid_analysis > 0) {
        double v = solid_end_time;
        snprintf(a, sizeof a, "%.4g s", solid_end_time);
        if (slider_row(ui, "sl_endtime", "simulated time", a, x, y, w, &v, 1.0, 36000.0, true)) solid_end_time = v;
        y += 32;
        v = solid_time_step;
        snprintf(a, sizeof a, "%.4g s \xC2\xB7 %d steps", solid_time_step, (int)ceil(solid_end_time / MAXI(solid_time_step, 1e-6)));
        if (slider_row(ui, "sl_timestep", "time step", a, x, y, w, &v, 0.001, 600.0, true)) solid_time_step = v;
        y += 32;
    }
    bool busy = s->job_active;
    const char *why = busy                                     ? NULL
                      : !s->meshed                             ? "the part has no mesh yet"
                      : !s->material[0]                        ? "the part has no material yet"
                      : (!solid_build_path && s->nbcs == 0)    ? "nothing is held or loaded yet"
                                                               : NULL;
    if (busy) {
        if (fem_job_stopping()) step_button(ui, "STOPPING...##run", x, y, w, 26, false, "it ends at the next layer or iteration");
        else if (ui_button(ui, "STOP##run", x, y, w, 26, false)) fem_stop_job(s->job_id);
    } else if (step_button(ui, solid_build_path ? (build_kind == 1 ? "RUN PRINT##run" : "RUN BUILD##run") : "SOLVE##run", x, y, w, 26, false, why)) {
        if (solid_build_path) build_run_now(field_num("layer", 1.0));
        else solid_run_now();
    }
    y += 32;
    if (s->job_id[0]) {
        const char *st = s->job_state[0] ? s->job_state : "submitted";
        uint32_t col = !strcmp(st, "failed") ? UI_BAD : !strcmp(st, "succeeded") ? UI_GOOD : UI_ACCENT2;
        snprintf(a, sizeof a, "%s \xC2\xB7 %s%s", s->job_id, st, s->job_adopted ? " \xC2\xB7 started by an agent" : "");
        ui_text(ui, FONT_SMALL, x, y, col, a);
        if (busy) {
            snprintf(a, sizeof a, "%.0f%%", s->job_progress * 100);
            ui_text_right(ui, FONT_MONO, x + w, y - 1, UI_TEXT, a);
        }
        y += 16;
        progress_bar(ui, x, y, w, 6, busy ? s->job_progress : (!strcmp(st, "succeeded") ? 1.0 : 0.0), col);
        y += 12;
        if (busy && s->job_stage[0]) {
            ui_text(ui, FONT_SMALL, x, y, UI_DIM, s->job_stage);
            y += 16;
        } else if (s->job_error[0]) {
            y = wrap_text(ui, x, y, w, UI_BAD, s->job_error, 3) + 2;
        }
    }
    /* the solver's own checks, in words */
    const JsonValue *sum = fem_job_summary();
    const JsonValue *ck = sum ? json_get(sum, "checks") : NULL;
    if (ck && !busy) {
        bool eq = json_get_bool(ck, "equilibrium_ok", false), en = json_get_bool(ck, "energy_ok", false);
        float cw = (w - 16) * 0.5f, x2 = x + cw + 16;
        kv(ui, x, y, cw, "equilibrium", eq ? "balanced" : "off", eq ? UI_GOOD : UI_BAD);
        kv(ui, x2, y, cw, "energy", en ? "consistent" : "off", en ? UI_GOOD : UI_BAD);
        y += 19;
    }
    return y + 4;
}

/* ---- the engineering summary: every number with its unit, its meaning and its caveat --------------------------- */

static double num_at3(const JsonValue *arr, size_t i) {
    const JsonValue *v = json_at(arr, i);
    return v && v->type == JSON_NUMBER ? v->u.number : 0;
}


/* ---- OPTIMISE: remove the material that carries little load (docs/contracts/topology-optimisation.md) ----------- */

static bool optimise_open;
static double optimise_vf = 0.4, optimise_radius_mm = 0; /* 0: 1.5 element widths */

/* the compliance of every iteration, drawn as a curve: the objective the optimiser is minimising */
static void optimise_curve(Ui *ui, float x, float y, float w, float h) {
    int n = 0;
    const double *c = fem_optimize_history(&n);
    ui_rect(ui, x, y, w, h, 0xEFEDE999u, 3);
    if (!c || n < 2) return;
    /* the compliance falls by decades in the first iterations: a logarithmic axis shows the whole run */
    double lo = c[0], hi = c[0];
    for (int i = 1; i < n; i++) {
        if (c[i] < lo) lo = c[i];
        if (c[i] > hi) hi = c[i];
    }
    if (!(lo > 0)) lo = hi > 0 ? hi * 1e-6 : 1e-12;
    if (!(hi > lo)) hi = lo * 10;
    double llo = log10(lo), lhi = log10(hi);
    float px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        float fx = x + w * (n > 1 ? (float)i / (n - 1) : 0.0f);
        double t = c[i] > 0 ? (log10(c[i]) - llo) / (lhi - llo) : 0;
        float fy = y + h - (float)t * (h - 4) - 2;
        if (i) ui_line(ui, px, py, fx, fy, 1.5f, UI_ACCENT2);
        px = fx, py = fy;
    }
    char a[64];
    snprintf(a, sizeof a, "%.3g J", hi);
    ui_text(ui, FONT_SMALL, x + 4, y + 2, UI_DIM, a);
    snprintf(a, sizeof a, "%.3g J", lo);
    ui_text(ui, FONT_SMALL, x + 4, y + h - 14, UI_DIM, a);
    ui_text_right(ui, FONT_SMALL, x + w - 4, y + 2, UI_DIM, "compliance per iteration (log)");
}

static float optimise_block(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    bool ready = s->have_project && s->meshed && s->nbcs >= 2; /* a hold and a load have been picked */
    bool running = fem_optimize_running();
    bool have = fem_optimize_have();
    if (!ready && !have && !running) return y;
    char a[220];
    if (ui_button(ui, optimise_open ? "\xE2\x96\xBE  OPTIMISE##topo" : "\xE2\x96\xB8  OPTIMISE##topo", x, y, 110, 22, optimise_open))
        optimise_open = !optimise_open;
    ui_text(ui, FONT_SMALL, x + 118, y + 5, UI_DIM, "remove the material that carries little load");
    y += 26;
    if (!optimise_open) return y;

    double vf = optimise_vf;
    snprintf(a, sizeof a, "%.0f %%", 100 * vf);
    if (slider_row(ui, "sl_topovf", "volume to keep", a, x, y, w, &vf, 0.05, 0.9, false)) optimise_vf = vf;
    y += 32;
    double r = optimise_radius_mm;
    if (r > 0) snprintf(a, sizeof a, "%.3g mm", r);
    else snprintf(a, sizeof a, "1.5 elements");
    if (slider_row(ui, "sl_toporad", "filter radius", a, x, y, w, &r, 0, 20, false)) optimise_radius_mm = r < 0.2 ? 0 : r;
    y += 32;

    float half = (w - 8) / 2;
    if (step_button(ui, running ? "OPTIMISING...##topogo" : "OPTIMISE NOW##topogo", x, y, half, 26, running,
                    ready ? NULL : "hold and load the part first"))
        if (!running && ready) fem_optimize_start(optimise_vf, 1e-3 * optimise_radius_mm, 0);
    if (have) {
        if (ui_button(ui, fem_optimize_show() ? "SHOW WHOLE##topoall" : "SHOW OPTIMISED##topoall", x + half + 8, y, half, 26,
                      fem_optimize_show()))
            fem_optimize_set_show(!fem_optimize_show());
    }
    y += 30;
    if (running) {
        ui_rect(ui, x, y, w, 6, 0xEFEDE9FFu, 3);
        ui_rect(ui, x, y, w * (float)CLAMP(fem_optimize_progress(), 0.0, 1.0), 6, UI_ACCENT2, 3);
        y += 10;
        y = wrap_text(ui, x, y, w, UI_ACCENT2, fem_optimize_stage(), 2) + 2;
        return y + 4;
    }
    if (fem_optimize_failed()) return wrap_text(ui, x, y, w, UI_BAD, fem_optimize_message(), 3) + 6;
    if (!have)
        return wrap_text(ui, x, y, w, UI_DIM,
                         "One solve per iteration on the voxel mesh: the density of each element is pushed towards "
                         "solid or empty until the volume you asked for carries the load with the least deflection. "
                         "The faces you held and loaded stay solid.", 4) + 6;

    optimise_curve(ui, x, y, w, 54);
    y += 58;
    snprintf(a, sizeof a, "%d iterations \xC2\xB7 %.1f s", fem_optimize_iterations(), fem_optimize_seconds());
    ui_text(ui, FONT_SMALL, x, y, UI_DIM, a);
    snprintf(a, sizeof a, "%d of %d elements kept", fem_optimize_kept(), fem_optimize_elements());
    ui_text_right(ui, FONT_MONO, x + w, y - 1, UI_TEXT, a);
    y += 18;
    ui_text(ui, FONT_SMALL, x, y, UI_DIM, "compliance");
    snprintf(a, sizeof a, "%.4g J   was %.4g J", fem_optimize_compliance(), fem_optimize_compliance_initial());
    ui_text_right(ui, FONT_MONO, x + w, y - 1, UI_TEXT, a);
    y += 20;
    if (ui_button(ui, "EXPORT STL##topostl", x, y, w, 24, false)) {
        char path[512], msg[512];
        snprintf(path, sizeof path, "%s/optimised.stl", s->project_dir[0] ? s->project_dir : ".");
        if (fem_export_optimised_stl(path, msg, sizeof msg)) LOGOK("optimised part: %s", msg);
        else LOGE("%s", msg);
    }
    y += 28;
    y = wrap_text(ui, x, y, w, UI_DIM,
                  "A stiffness-optimal density field on this mesh, at this volume fraction, for this one load case: "
                  "no stress limit, no printability rule, no second load case. EXPORT STL writes the surviving part "
                  "so it can be printed in the build simulation and checked again.", 5);
    return y + 6;
}

static float solid_summary(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    const JsonValue *sum = fem_job_summary();
    char a[220];
    float cw = (w - 16) * 0.5f, x2 = x + cw + 16;
    const JsonValue *dp = sum ? json_get(sum, "displacement") : NULL;
    const JsonValue *vm = sum ? json_get(sum, "von_mises") : NULL;
    const JsonValue *ck = sum ? json_get(sum, "checks") : NULL;

    if (s->has_temperature && !s->has_displacement && !s->has_stress) {
        snprintf(a, sizeof a, "%.4g C", s->peak);
        kv(ui, x, y, w, "peak shown temperature", a, UI_ACCENT); y += 18;
        snprintf(a, sizeof a, "%.5g s", s->time_s);
        kv(ui, x, y, w, "shown stored time", a, UI_TEXT); y += 18;
        snprintf(a, sizeof a, "%.4g to %.4g C", s->range_lo, s->range_hi);
        kv(ui, x, y, w, "colour scale", a, UI_DIM); y += 18;
        y = wrap_text(ui, x, y, w, UI_DIM, "Thermal result. No stress or displacement field was computed.", 2) + 6;
        return y;
    }

    /* how far it moves, and where */
    double dmax = dp ? json_get_num(dp, "max_magnitude_mm", 0) : s->max_displacement_mm;
    snprintf(a, sizeof a, "%.4g mm", dmax);
    kv(ui, x, y, cw, "max displacement", a, UI_ACCENT);
    if (dp && json_get(dp, "at_mm")) {
        double at[3] = {0, 0, 0};
        json_get_numbers(json_get(dp, "at_mm"), at, 3);
        snprintf(a, sizeof a, "at %.4g, %.4g, %.4g", at[0], at[1], at[2]);
        kv(ui, x2, y, cw, "", a, UI_DIM);
    }
    y += 18;

    /* stress: the peak, and the number to design with */
    double peak = vm ? json_get_num(vm, "nodal_average_max_mpa", 0) : 0;
    double p99 = vm ? json_get_num(vm, "nodal_p99_mpa", 0) : 0;
    const char *caveat = fem_result_caveat();
    snprintf(a, sizeof a, "%.4g MPa", peak);
    kv(ui, x, y, cw, "peak stress", a, caveat ? UI_ACCENT2 : UI_ACCENT); /* the line below says why it is amber */
    snprintf(a, sizeof a, "%.4g MPa", p99);
    kv(ui, x2, y, cw, "99th percentile", a, UI_TEXT);
    y += 17;
    if (caveat) y = wrap_text(ui, x, y, w, UI_ACCENT2, caveat, 2) + 2;
    else if (vm) y = wrap_text(ui, x, y, w, UI_DIM, "The 99th percentile is the value to design with: a single peak on "
                                                    "a voxel corner is a property of the mesh, not of the part.", 2) + 2;
    /* the legend is the range of what is drawn, and the peak above is the solver's nodal peak: they differ because
     * interpolating onto the part's surface smooths the extremes and because the worst node may be inside the part */
    if (vm && fem_field() == FEM_VON_MISES && s->have_result && s->range_hi > 0 && peak > 0 && fabs(peak - s->range_hi) > 0.005 * fabs(peak)) {
        snprintf(a, sizeof a, "nodal peak %.4g %s (inside the part or at a corner); largest on the drawn surface "
                              "%.4g %s, which is what the colour bar spans.",
                 peak, fem_field_unit(fem_field()), s->range_hi, fem_field_unit(fem_field()));
        y = wrap_text(ui, x, y, w, UI_DIM, a, 3) + 2;
    }

    /* a printed part (GOALS.md G17): the strength criterion's answer and what it is worth; it replaces the isotropic
     * safety factor below, which would be a second, different answer */
    const JsonValue *sg = sum ? json_get(sum, "strength") : NULL;
    /* the answer of GOALS.md G17 step 4 in words, read away from flagged corners and supports, before the numbers behind it */
    const JsonValue *answer = sg ? json_get(sg, "answer") : NULL;
    if (answer && json_get_str(answer, "text", NULL)) {
        y = wrap_text(ui, x, y, w, UI_TEXT, json_get_str(answer, "text", ""), 16) + 2; /* the whole answer: it is the result */
        if (json_get_str(answer, "accuracy_note", NULL)) {
            char note[400];
            snprintf(note, sizeof note, "Accuracy: %s.", json_get_str(answer, "accuracy_note", ""));
            y = wrap_text(ui, x, y, w, UI_DIM, note, 5) + 4;
        }
    }
    if (sg && json_get(sg, "largest_failure_index")) {
        snprintf(a, sizeof a, "%.3g", json_get_num(sg, "largest_failure_index", 0));
        kv(ui, x, y, cw, "failure index", a, UI_ACCENT);
        snprintf(a, sizeof a, "%.3g", json_get_num(sg, "smallest_strength_ratio", 0));
        kv(ui, x2, y, cw, "strength ratio", a, UI_TEXT);
        y += 17;
        const char *crit = json_get_str(sg, "criterion", "");
        double at[3] = {0, 0, 0};
        json_get_numbers(json_get(sg, "location_mm"), at, 3);
        char b[480];
        snprintf(b, sizeof b, "%s, first reached by %s at %.3g, %.3g, %.3g mm. A linear-elastic first-failure estimate for "
                              "non-critical parts, not a guarantee; strengths: %s.",
                 !strcmp(crit, "tsai_wu") ? "Tsai-Wu" : !strcmp(crit, "tsai_hill") ? "Tsai-Hill" : !strcmp(crit, "max_stress") ? "Maximum stress" : crit,
                 json_get_str(sg, "governing_mode", "?"), at[0], at[1], at[2], json_get_str(sg, "strength_source", "?"));
        y = wrap_text(ui, x, y, w, UI_DIM, b, 4) + 2;
    } else if (sg)
        y = wrap_text(ui, x, y, w, UI_DIM, json_get_str(sg, "criterion", ""), 2) + 2;

    /* safety factor, only ever next to what the material numbers are worth */
    if (!sg && s->yield_mpa > 0 && p99 > 0) {
        double sf = s->yield_mpa / p99;
        /* a factor of thirty thousand needs no more precision than a factor of three, and a short key leaves the
         * number room: "safety factor (yield/p99)" and 2.96e+04 ran into each other on a customer part */
        if (sf >= 1000) snprintf(a, sizeof a, "%.0f", sf);
        else snprintf(a, sizeof a, "%.3g", sf);
        kv(ui, x, y, cw, "safety factor", a, UI_ACCENT);
        snprintf(a, sizeof a, "%.4g MPa yield", s->yield_mpa);
        kv(ui, x2, y, cw, "", a, UI_DIM);
        y += 17;
        bool demo = !strcmp(s->material_status, "demonstration");
        y = wrap_text(ui, x, y, w, demo ? UI_ACCENT2 : UI_GOOD,
                      demo ? "Demonstration material values: not for design. Define the real material to get a factor "
                             "you can rely on."
                           : "Material values supplied by you.", 2) + 2;
    }

    /* the load actually carried, against the load asked for */
    if (ck) {
        double ap[3] = {0, 0, 0}, re[3] = {0, 0, 0};
        json_get_numbers(json_get(ck, "applied_load_total_n"), ap, 3);
        json_get_numbers(json_get(ck, "reaction_total_n"), re, 3);
        double an = sqrt(ap[0] * ap[0] + ap[1] * ap[1] + ap[2] * ap[2]);
        double rn = sqrt(re[0] * re[0] + re[1] * re[1] + re[2] * re[2]);
        double res = sqrt((ap[0] + re[0]) * (ap[0] + re[0]) + (ap[1] + re[1]) * (ap[1] + re[1]) +
                          (ap[2] + re[2]) * (ap[2] + re[2]));
        snprintf(a, sizeof a, "%.5g N", an);
        kv(ui, x, y, cw, "load applied", a, UI_TEXT);
        snprintf(a, sizeof a, "%.5g N", rn);
        kv(ui, x2, y, cw, "supports carry", a, UI_TEXT);
        y += 17;
        snprintf(a, sizeof a, "%.3g N", res);
        kv(ui, x, y, w, "residual (must be ~0)", a, res < MAXI(an * 1e-6, 1e-6) ? UI_GOOD : UI_BAD);
        y += 19;
    }

    /* what the part weighs: the solver's own figure for the mesh it used */
    const JsonValue *mass = sum ? json_get(json_at(json_get(json_get(sum, "mass"), "bodies"), 0), "mass_mesh_kg") : NULL;
    double kg = mass ? mass->u.number : (s->density_kg_m3 > 0 ? s->mesh_volume_mm3 * 1e-9 * s->density_kg_m3 : 0);
    double rho = sum ? json_get_num(json_at(json_get(json_get(sum, "mass"), "bodies"), 0), "density_kg_m3", s->density_kg_m3)
                     : s->density_kg_m3;
    if (kg > 0) {
        snprintf(a, sizeof a, "%.4g g", kg * 1e3);
        kv(ui, x, y, cw, "mass (mesh volume)", a, UI_TEXT);
        snprintf(a, sizeof a, "%.0f kg/m\xC2\xB3", rho);
        kv(ui, x2, y, cw, "", a, UI_DIM);
        y += 18;
    }

    /* and the mesh the numbers came from - the result's own, which is not always the mesh in the project now */
    const JsonValue *model = sum ? json_get(sum, "model") : NULL;
    double hmm = model ? num_at3(json_get(model, "element_size_mm"), 0) : fem_project_mesh_mm();
    int nel = model ? (int)json_get_int(model, "elements", s->nelems) : s->nelems;
    if (s->mesh_current && s->nelems == nel)
        snprintf(a, sizeof a, "%.3g mm \xC2\xB7 %d elements \xC2\xB7 volume %+.2f %% against the STL", hmm, nel,
                 s->mesh_volume_error_pct);
    else
        snprintf(a, sizeof a, "%.3g mm \xC2\xB7 %d elements (the mesh this result came from)", hmm, nel);
    y = wrap_text(ui, x, y, w, UI_DIM, a, 2) + 6;
    return y;
}

/* ---- 6 RESULTS ---------------------------------------------------------------------------------------------- */

/* These are the declared model and input provenance, not a confidence claim inferred from the picture. */
static void printing_model_note(char *note, size_t cap) {
    note[0] = 0;
    const JsonValue *sum = fem_job_summary();
    if (!sum) return;
    const char *analysis = json_get_str(sum, "analysis", "");
    const JsonValue *model = json_get(sum, "model"), *results = json_get(sum, "results");
    const JsonValue *scope = json_get(sum, "scope");
    const char *comparison = json_get_bool(scope, "compared_with_measurement", false) ? "measurement compared" : "no measurement comparison";
    /* the model's own blind record against published prints (GOALS.md G20), beside what this run is */
    const char *acc = json_get_str(json_get(scope, "measured_accuracy"), "short", NULL);
    if (!strcmp(analysis, "lpbf_build")) {
        const char *prov = json_get_str(json_get(model, "inherent_strain"), "provenance", "unspecified");
        bool plastic = json_get(results, "plasticity") != NULL;
        snprintf(note, cap, "LPBF: %s inherent strain; input %s; %s%s%s.",
                 plastic ? "J2 plastic" : "elastic (no yielding)", prov, comparison, acc ? "; " : "", acc ? acc : "");
    } else if (!strcmp(analysis, "fff_print")) {
        const char *prov = json_get_str(json_get(model, "process"), "provenance", "unspecified");
        const char *status = json_get_str(json_get(model, "material"), "status", "unspecified");
        snprintf(note, cap, "FDM: deposition, heat and stress; process %s; material %s; %s%s%s.", prov, status, comparison,
                 acc ? "; " : "", acc ? acc : "");
    }
}

/* the viewer tools: everything that changes how the result is drawn, out of the way until asked for */
static float view_tools(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    const float gap = 4;
    char a[160];
    if (s->nsteps > 1) {
        double v = fem_step();
        snprintf(a, sizeof a, "%d / %d", fem_step() + 1, s->nsteps);
        if (slider_row(ui, "sl_step", !strcmp(s->result_kind, "lpbf_build") ? "process step" : "stored time", a, x, y, w, &v, 0, s->nsteps - 1, false)) {
            if (fem_playing()) exec_cmd("fem pause");
            fem_set_step((int)lround(v));
        }
        y += 32;
        bool playing = fem_playing();
        const float pbw = 92;
        if (ui_button(ui, playing ? "\xE2\x8F\xB8  PAUSE##play" : "\xE2\x96\xB6  PLAY##play", x, y + 4, pbw, 22, playing))
            exec_cmd(playing ? "fem pause" : "fem play");
        double sp = fem_play_speed();
        snprintf(a, sizeof a, "%.3g steps/s", sp);
        if (slider_row(ui, "sl_rate", "playback", a, x + pbw + 12, y, w - pbw - 12, &sp, 0.5, 30.0, true))
            fem_set_play_speed(sp);
        y += 32;
        ui_text(ui, FONT_SMALL, x, y + 4, UI_DIM, "colour range");
        float rw = (w - 84 - gap) / 2;
        if (ui_button(ui, "ALL TIMES##rng", x + 84, y, rw, 22, fem_range_all())) exec_cmd("fem range all");
        if (ui_button(ui, "THIS STEP##rng", x + 84 + rw + gap, y, rw, 22, !fem_range_all())) exec_cmd("fem range step");
        y += 28;
    }
    if (s->has_displacement) {
        double v = fem_deform_auto() ? fem_deform_applied() : fem_deform_scale();
        if (fem_deform_auto()) snprintf(a, sizeof a, "auto \xC3\x97%.4g", fem_deform_applied());
        else snprintf(a, sizeof a, "\xC3\x97%.4g", fem_deform_scale());
        if (slider_row(ui, "sl_deform", "deformation scale", a, x, y, w - 118, &v, 1.0, 1e5, true))
            fem_set_deform_scale(v);
        if (ui_button(ui, "TRUE##def", x + w - 112, y + 4, 54, 22, fem_deform_scale() == 1)) exec_cmd("fem deform true");
        if (ui_button(ui, "AUTO##def", x + w - 54, y + 4, 54, 22, fem_deform_auto())) fem_set_deform_scale(-1);
        y += 32;
        snprintf(a, sizeof a, "%.5g mm", s->max_displacement_mm);
        kv(ui, x, y, w, "max displacement, this time", a, UI_ACCENT);
        y += 20;
    }
    if (ui_button(ui, "SECTION", x, y, 78, 22, fem_section_on()))
        fem_section(fem_section_on() ? -1 : fem_section_axis(), fem_section_position());
    if (fem_section_on()) {
        static const char *axes[] = {"X##section", "Y##section", "Z##section"};
        for (int i = 0; i < 3; i++)
            if (ui_button(ui, axes[i], x + 82 + i * 32, y, 28, 22, fem_section_axis() == i))
                fem_section(i, fem_section_position());
        if (ui_button(ui, "FLIP##section", x + 180, y, 52, 22, fem_section_flipped())) fem_section_flip();
    }
    y += 26;
    if (fem_section_on()) {
        double v = fem_section_position();
        snprintf(a, sizeof a, "%.3f", v);
        if (slider_row(ui, "sl_section", "position", a, x, y, w, &v, 0, 1, false)) fem_section(fem_section_axis(), v);
        y += 30;
    }
    if (s->has_groups) {
        static const char *gn[] = {"PART##group", "SUPPORT##group", "PLATE##group"};
        for (int i = 0; i < 3; i++)
            if (ui_button(ui, gn[i], x + i * (w + gap) / 3, y, (w - 2 * gap) / 3, 22, fem_group_shown(i)))
                fem_show_group(i, !fem_group_shown(i));
        y += 26;
    }
    /* presentation: glass, edges, ambient occlusion, and the pieces pushed apart */
    float bw3 = (w - 2 * gap) / 3;
    if (ui_button(ui, "EDGES##pres", x, y, bw3, 22, fem_edges_on())) fem_set_edges(!fem_edges_on());
    if (ui_button(ui, "SHADOWS##pres", x + bw3 + gap, y, bw3, 22, fem_ao_on())) fem_set_ao(!fem_ao_on());
    if (ui_button(ui, "EXPORT IMAGE##pres", x + 2 * (bw3 + gap), y, bw3, 22, false)) {
        char path[512];
        const char *dir = s->project_dir[0] ? s->project_dir : ".";
        snprintf(path, sizeof path, "%s/view-3x.png", dir);
        fem_export_image(path, 3);
    }
    y += 26;
    if (ui_button(ui, "CLEAN VIEW##pres", x, y, w, 22, false)) exec_cmd("hud clean");
    y += 26;
    if (fem_pieces() > 1) {
        double ex = fem_explode();
        snprintf(a, sizeof a, ex > 0 ? "%.0f %%" : "together", 100 * ex);
        if (slider_row(ui, "sl_explode", "explode", a, x, y, w, &ex, 0, 1.5, false)) fem_set_explode(ex);
        y += 30;
        for (int i = 0; i < fem_pieces() && i < 6; i++) {
            double op = fem_piece_opacity(i);
            char id[32];
            snprintf(id, sizeof id, "sl_glass%d", i);
            snprintf(a, sizeof a, op > 0.99 ? "solid" : "%.0f %% glass", 100 * (1 - op));
            if (slider_row(ui, id, fem_piece_name(i), a, x, y, w, &op, 0.05, 1.0, false)) fem_set_piece_opacity(i, (float)op);
            y += 26;
        }
        y += 4;
    }
    float bw2 = (w - gap) / 2;
    if (ui_button(ui, "EXPORT##res", x, y, bw2, 22, false)) fem_op("results_export", "{\"job_id\": \"%s\"}", s->result_job);
    if (ui_button(ui, fem_visible() ? "HIDE##res" : "SHOW##res", x + bw2 + gap, y, bw2, 22, fem_visible()))
        exec_cmd(fem_visible() ? "fem hide" : "fem show");
    return y + 26;
}

static float step_results(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    const float gap = 4;
    char a[220];
    if (!s->have_result) {
        const char *why = fem_unsupported_reason();
        y = wrap_text(ui, x, y, w, why ? UI_ACCENT2 : UI_DIM,
                      why ? why : "Solve the part to see displacements and stresses.", 3) + 6;
        return y;
    }
    /* which fields this result actually carries */
    static const char *const FLABEL[FEM_FIELD_COUNT] = {"STRESS##fld", "DISPL##fld", "TEMP##fld", "FAILURE##fld"};
    int avail[FEM_FIELD_COUNT], na = 0;
    for (int i = 0; i < FEM_FIELD_COUNT; i++)
        if (fem_field_available(i)) avail[na++] = i;
    float bw = na > 0 ? (w - gap * (na - 1)) / na : w;
    for (int k = 0; k < na; k++) {
        int i = avail[k];
        if (ui_button(ui, FLABEL[i], x + k * (bw + gap), y, bw, 22, fem_field() == i)) {
            char cmd[64];
            snprintf(cmd, sizeof cmd, "fem field %s", fem_field_name(i));
            exec_cmd(cmd);
        }
    }
    y += 28;

    /* The owner's rule: what is drawn is the part, not the mesh. The mesh is one click away, and the panel says how
     * far the part's own surface had to be snapped onto it, so the smooth picture is never mistaken for a smooth
     * mesh. */
    float hwv = (w - gap) / 2;
    if (s->tet_mesh) {
        /* a tetrahedral mesh follows the part's surface: there is nothing to choose between */
        y = wrap_text(ui, x, y, w, UI_DIM,
                      s->tet_sharp_kept ? "Drawn on the tetrahedral mesh's own faces, which lie on the part's surface; its sharp edges and corners are kept."
                                        : "Drawn on the tetrahedral mesh's own faces, which lie on the part's surface; sharp edges are rounded at "
                                          "the surface cell scale.",
                      3) + 3;
    } else {
    if (ui_button(ui, "SURFACE##draw", x, y, hwv, 22, fem_surface_view())) fem_set_surface_view(true);
    if (ui_button(ui, "VOXELS##draw", x + hwv + gap, y, hwv, 22, !fem_surface_view())) fem_set_surface_view(false);
    y += 26;
    }
    if (s->tet_mesh) {
    } else if (fem_surface_view() && s->on_surface) {
        snprintf(a, sizeof a, "The part's own surface carries the result, interpolated from the mesh inside it. "
                              "Largest snap onto the mesh %.3g mm%s.",
                 s->snap_max_mm, s->unmapped_vertices ? ", some vertices too far out to draw" : "");
        y = wrap_text(ui, x, y, w, UI_DIM, a, 3) + 3;
    } else if (fem_surface_view() && s->visibility_mesh) {
        y = wrap_text(ui, x, y, w, UI_ACCENT2,
                      "Active element boundary: only printed, uncut and visible elements. "
                      "The original STL cannot represent this topology.", 3) + 3;
    } else if (fem_surface_view()) {
        y = wrap_text(ui, x, y, w, UI_ACCENT2,
                      "The voxel mesh is drawn: the part this result was solved on is not the one open.", 2) + 3;
    } else {
        y = wrap_text(ui, x, y, w, UI_DIM, "The voxel mesh the numbers came from, staircase and all.", 2) + 3;
    }

    /* a build answers a different question: what the machine did to the part, played at true scale */
    const JsonValue *bsum = fem_job_summary();
    const JsonValue *bres = bsum ? json_get(bsum, "results") : NULL;
    const char *bana = bsum ? json_get_str(bsum, "analysis", "") : "";
    bool is_lpbf = !strcmp(bana, "lpbf_build"), is_fff = !strcmp(bana, "fff_print");
    const JsonValue *cut = bsum ? json_get(json_get(bsum, "model"), "cut") : NULL;
    const JsonValue *supports = bres ? json_get(bres, "supports") : NULL;
    bool supports_removed = supports && json_get_bool(supports, "removed", false);
    if (bres && (is_lpbf || is_fff)) {
        char note[260];
        printing_model_note(note, sizeof note);
        y = wrap_text(ui, x, y, w, UI_ACCENT2, note, 3) + 4;
        const float bh2 = 24;
        if (is_lpbf) {
            snprintf(a, sizeof a, "%.4g mm", json_get_num(bres, "tip_uz_before_cut_mm", 0));
            kv(ui, x, y, w, "tip, still on the plate", a, UI_TEXT);
            y += 17;
            if (cut) {
                snprintf(a, sizeof a, "%.4g mm", json_get_num(bres, "tip_uz_after_cut_mm", 0));
                kv(ui, x, y, w, "tip, after the cut", a, UI_ACCENT);
                y += 17;
                snprintf(a, sizeof a, "%.4g mm", json_get_num(bres, "springback_mm", 0));
                kv(ui, x, y, w, "springback", a, UI_TEXT);
                y += 17;
            } else if (supports_removed) {
                snprintf(a, sizeof a, "%.4g mm", json_get_num(supports, "tip_uz_after_support_removal_mm", 0));
                kv(ui, x, y, w, "tip, supports removed", a, UI_ACCENT);
                y += 17;
            }
            y = wrap_text(ui, x, y, w, UI_DIM, json_get_str(bres, "tip_definition", ""), 2) + 3;
            if (cut)
                snprintf(a, sizeof a, "%.4g MPa on the plate \xC2\xB7 %.4g MPa cut",
                         json_get_num(bres, "peak_von_mises_before_cut_mpa", 0), json_get_num(bres, "peak_von_mises_after_cut_mpa", 0));
            else snprintf(a, sizeof a, "%.4g MPa on the plate", json_get_num(bres, "peak_von_mises_before_cut_mpa", 0));
            kv(ui, x, y, w, "peak stress", a, UI_TEXT);
            y += 17;
            if (cut) {
                char label[64];
                snprintf(label, sizeof label, "cut (%.42s)", json_get_str(cut, "provenance", "unspecified"));
                snprintf(a, sizeof a, "%.4g mm high \xC2\xB7 %.4g kerf \xC2\xB7 %lld removed",
                         json_get_num(cut, "height_mm", 0), json_get_num(cut, "kerf_mm", 0),
                         json_get_int(cut, "elements_removed", 0));
                kv(ui, x, y, w, label, a, UI_TEXT);
                y += 17;
            }
        } else {
            snprintf(a, sizeof a, "%.4g to %.4g mm", json_get_num(bres, "warp_z_min_mm", 0),
                     json_get_num(bres, "warp_z_max_mm", 0));
            kv(ui, x, y, w, "warp after release", a, UI_ACCENT);
            y += 17;
            snprintf(a, sizeof a, "%.4g MPa on the bed \xC2\xB7 %.4g MPa released",
                     json_get_num(bres, "peak_von_mises_on_bed_mpa", 0),
                     json_get_num(bres, "peak_von_mises_released_mpa", 0));
            kv(ui, x, y, w, "peak stress", a, UI_TEXT);
            y += 17;
            y = wrap_text(ui, x, y, w, UI_DIM,
                          "Creep below the relaxation temperature is not modelled. No local stress bound is "
                          "established.", 3) + 3;
        }
        /* the two analyses count their layers and their compute time in their own summaries */
        long long layers = is_lpbf ? json_get_int(bres, "layers", 0)
                                   : json_get_int(json_get(bsum, "deposition"), "simulation_layers", 0);
        double secs = is_lpbf ? json_get_num(bres, "seconds", 0)
                              : json_get_num(json_get(bsum, "timing"), "compute_seconds", 0);
        snprintf(a, sizeof a, "%lld layers \xC2\xB7 %d stored times \xC2\xB7 %.3g s", layers, fem_step_count(), secs);
        kv(ui, x, y, w, is_lpbf ? "build" : "print", a, UI_DIM);
        y += 21;

        /* watching it happen: true scale, from the first layer, with the part growing as it was deposited */
        float half = (w - gap) / 2;
        if (ui_button(ui, is_lpbf ? "PLAY BUILD##bplay" : "PLAY PRINT##bplay", x, y, half, bh2, false)) {
            exec_cmd("fem field displacement");
            exec_cmd("fem deform true");
            exec_cmd("fem range all");
            exec_cmd("fem step 1"); /* the stored-time index the panel speaks is 1-based */
            exec_cmd("fem play");
        }
        const char *last_label = !is_lpbf ? "AFTER RELEASE##bcut" : cut ? "AFTER THE CUT##bcut"
                                : supports_removed ? "SUPPORTS REMOVED##bcut" : "BUILD COMPLETE##bcut";
        if (ui_button(ui, last_label, x + half + gap, y, half, bh2, false)) {
            exec_cmd("fem pause");
            exec_cmd("fem step last");
        }
        y += bh2 + 4;
        y = wrap_text(ui, x, y, w, UI_DIM,
                      is_lpbf ? (cut ? "PLAY BUILD shows the part growing layer by layer and opening at the cut, with the "
                                "shape drawn at true scale: what you see is the distortion, not a magnified picture "
                                "of it."
                                : "PLAY BUILD shows the deposited layers and their computed distortion at true scale.")
                              : "PLAY PRINT shows the part growing layer by layer, with the shape drawn at true "
                                "scale: what you see is the distortion, not a magnified picture of it. The last "
                                "stored time is the part after release from the bed.", 4) + 4;
        const LayerStudy *ls = fem_layer_study();
        bool lsbusy = ls->state == LS_RUNNING;
        if (step_button(ui, lsbusy ? "STUDYING...##layer" : "LAYER STUDY##layer", x, y, half, bh2, lsbusy,
                        !is_lpbf      ? "the layer study is for an LPBF build"
                        : lsbusy      ? "the second build is running"
                        : s->job_active ? "wait for the build to finish"
                                      : NULL)) {
            double coarse = field_num("layer", 1.0);
            if (fem_layer_study_begin(coarse, coarse * 0.5)) build_run_now(coarse * 0.5);
        }
        if (step_button(ui, "REPORT##rep", x + half + gap, y, half, bh2, false, lsbusy ? "wait for the layer study" : NULL)) {
            char dir[1024];
            if (fem_write_report(dir, sizeof dir) && !app.headless) platform_open_path(dir);
        }
        y += bh2 + 6;
        if (ls->state == LS_DONE) {
            snprintf(a, sizeof a, "%.4g mm -> %.4g mm", ls->coarse_layer_mm, ls->fine_layer_mm);
            kv(ui, x, y, w, "layer study", a, UI_ACCENT2);
            y += 17;
            snprintf(a, sizeof a, "tip %+.1f %% \xC2\xB7 springback %+.1f %% \xC2\xB7 %d \xE2\x86\x92 %d layers",
                     ls->tip_change_pct, ls->spring_change_pct, ls->coarse_layers, ls->fine_layers);
            y = wrap_text(ui, x, y, w, UI_DIM, a, 2) + 2;
            y = wrap_text(ui, x, y, w, UI_ACCENT2,
                          "Halving the simulation layer changes the model, not only the discretisation: the part is "
                          "strained twice as many times. The move above is not a convergence error.", 4) + 3;
            if (ls->limited_by_mesh) {
                snprintf(a, sizeof a,
                         "An element is activated whole, and the elements are %.3g mm, so a %.3g mm layer activates "
                         "the same elements: refine the mesh to %.3g mm before the layer can move the answer.",
                         ls->element_mm, ls->fine_layer_mm, ls->fine_layer_mm);
                y = wrap_text(ui, x, y, w, UI_BAD, a, 4) + 3;
            }
        } else if (ls->state == LS_FAILED) {
            y = wrap_text(ui, x, y, w, UI_BAD, ls->message, 2) + 3;
        } else if (lsbusy) {
            y = wrap_text(ui, x, y, w, UI_ACCENT2, ls->message, 2) + 3;
        }
        /* the viewer tools, closed until asked for */
        if (ui_button(ui, solid_view_open ? "\xE2\x96\xBE  VIEW##view" : "\xE2\x96\xB8  VIEW##view", x, y, 86, 22, solid_view_open))
            solid_view_open = !solid_view_open;
        ui_text(ui, FONT_SMALL, x + 94, y + 5, UI_DIM, "playback, section, groups, deformed shape");
        y += 26;
        if (solid_view_open) y = view_tools(ui, x, y, w);
        return y + 4;
    }

    y = solid_summary(ui, x, y, w);

    /* the two buttons that turn a number into an answer somebody else can check */
    const ConvResult *cv = fem_convergence();
    const float bh = 24;
    float hw = (w - gap) / 2;
    bool busy = cv->state == CONV_MESHING || cv->state == CONV_SOLVING;
    /* The comparison solves the part again as a static structural analysis, so any other result is refused. A click
     * then leaves the reason beside the step and in 'fem status', as a refusal from the engine does, until a result
     * it can compare is open. */
    bool comparable = !strcmp(s->result_kind, "static_structural");
    const char *refused = NULL;
    if (refusable_button(ui, busy ? "CHECKING...##conv" : "CHECK MESH##conv", x, y, hw, bh, busy,
                         busy ? "the finer analysis is running" : comparable ? NULL : "this mesh comparison supports static structural results",
                         &refused))
        fem_check_mesh();
    if (refused && !busy) fem_panel_refusal("CHECK MESH", refused);
    else if (comparable) fem_panel_refusal("CHECK MESH", NULL);
    if (step_button(ui, "REPORT##rep", x + hw + gap, y, hw, bh, false, busy ? "wait for the mesh check" : NULL)) {
        char dir[1024];
        /* the folder opens in Finder for a person; a headless test only writes it */
        if (fem_write_report(dir, sizeof dir) && !app.headless) platform_open_path(dir);
    }
    y += bh + 6;
    if (cv->state == CONV_DONE) {
        snprintf(a, sizeof a, "%.3g mm -> %.3g mm", cv->coarse_mm, cv->fine_mm);
        kv(ui, x, y, w, cv->converged ? "mesh change: below 5%" : "mesh change: refine again", a,
           cv->converged ? UI_GOOD : UI_ACCENT2);
        y += 17;
        snprintf(a, sizeof a, "displacement %+.1f %% \xC2\xB7 stress %+.1f %% \xC2\xB7 %d \xE2\x86\x92 %d elements",
                 cv->disp_change_pct, cv->stress_change_pct, cv->coarse_elements, cv->fine_elements);
        y = wrap_text(ui, x, y, w, UI_DIM, a, 2) + 4;
        y = wrap_text(ui, x, y, w, UI_DIM,
                      "Two meshes show sensitivity, not an accuracy bound or proof of convergence.", 3) + 4;
        if (cv->capped) y = wrap_text(ui, x, y, w, UI_DIM, "The finer mesh was capped at about 60 000 elements.", 2) + 2;
    } else if (cv->state == CONV_FAILED) {
        y = wrap_text(ui, x, y, w, UI_BAD, cv->message, 2) + 4;
    } else if (busy) {
        y = wrap_text(ui, x, y, w, UI_ACCENT2, cv->message[0] ? cv->message : "making the finer mesh", 2) + 4;
    } else {
        y = wrap_text(ui, x, y, w, UI_DIM, strcmp(s->result_kind, "static_structural")
                      ? "REPORT writes the numbers, the setup and the pictures to the project folder. "
                        "Mesh comparison here is available for static structural results."
                      : "CHECK MESH solves it again on a finer mesh and tells you how much the answer moved. "
                        "REPORT writes the numbers, the setup and the pictures to the project folder.", 3) + 4;
    }

    y = optimise_block(ui, x, y, w) ;

    /* the viewer tools, closed until asked for */
    /* the caption is drawn beside the button, not in its name: a name holding "playback" would collide with PLAY */
    if (ui_button(ui, solid_view_open ? "\xE2\x96\xBE  VIEW##view" : "\xE2\x96\xB8  VIEW##view", x, y, 86, 22, solid_view_open))
        solid_view_open = !solid_view_open;
    ui_text(ui, FONT_SMALL, x + 94, y + 5, UI_DIM, "playback, section, groups, deformed shape");
    y += 26;
    if (solid_view_open) y = view_tools(ui, x, y, w);
    return y + 4;
}

/* =================================================================================================================
 * SIMPLE MODE: five screens for someone with a part and a printer, not an engineer.
 *   1 Choose a part   2 How will it be printed?   3 Simulate   4 Results   5 Report
 * One primary action per screen, drawn larger than anything else; a small "back"; words from docs/app/DESIGN.md.
 * Every action is the same operation Advanced mode runs. Where the software would have to guess, it asks.
 * ================================================================================================================= */

enum { SIMPLE_PART = 0, SIMPLE_PRINT, SIMPLE_RUN, SIMPLE_RESULTS, SIMPLE_REPORT, SIMPLE_COUNT };
static const char *const SIMPLE_TITLE[SIMPLE_COUNT] = {"Choose a part", "How will it be printed?", "Simulate", "Results",
                                                       "Report"};
static int simple_screen;
static int simple_tech = -1;       /* 0 metal powder, 1 plastic filament, -1 until the print screen first shows */
static int simple_profile = -1;    /* index into the profiles file */
static bool simple_more = false;
static float simple_scroll_y, simple_scroll_max; /* Simple's screen, when it is taller than the panel */

void hud_simple_scroll(float dy) {
    simple_scroll_y = CLAMP(simple_scroll_y - dy, 0.0f, simple_scroll_max);
}    /* the results screen's MORE: supports on and off, and the way to Advanced */
static int simple_sample = -1;     /* index into the samples file, -1 for a file of the user's own */
static bool simple_face_mode;      /* the next click on the part chooses the face that sits on the plate */
static bool simple_true_size = true;
static char simple_job[64];        /* the build this screen started */
static double simple_job_t0;
static bool simple_meshed_for_estimate;
static char simple_report_dir[1024];

/* ---- the two data files the screens read ---- */

static JsonValue *simple_json(const char *name) {
    char path[1024];
    if (!app_resource_path(name, path, sizeof path)) return NULL;
    JsonError err;
    return json_read_file(path, 1 << 20, &err);
}

static const JsonValue *simple_samples(void) {
    static JsonValue *doc;
    static bool tried;
    if (!tried) tried = true, doc = simple_json("samples/samples.json");
    return doc ? json_get(doc, "samples") : NULL;
}

static const JsonValue *simple_profiles(void) {
    static JsonValue *doc;
    static bool tried;
    if (!tried) tried = true, doc = simple_json("print_profiles.json");
    if (!doc && tried == true) {
        static bool tried2;
        if (!tried2) tried2 = true, doc = simple_json("src/ctl/print_profiles.json");
    }
    return doc ? json_get(doc, "profiles") : NULL;
}

static const JsonValue *simple_current_profile(void) {
    const JsonValue *ps = simple_profiles();
    return simple_profile >= 0 && (size_t)simple_profile < json_len(ps) ? json_at(ps, (size_t)simple_profile) : NULL;
}

static const JsonValue *simple_current_sample(void) {
    const JsonValue *ss = simple_samples();
    return simple_sample >= 0 && (size_t)simple_sample < json_len(ss) ? json_at(ss, (size_t)simple_sample) : NULL;
}

/* What a preset resolves to. A metal preset names a library record; the metal may yield when that record has a yield
 * value with a source, and then the strain fitted with plasticity on is used, at the layer and detail it was fitted
 * at. Otherwise the elastic fit. Nothing here is a number the software chose. */
typedef struct SimpleSetup {
    bool metal, library, plastic;
    FemMaterialInfo mat;
    const JsonValue *strain; /* the fit this run uses */
    double layer_mm, detail_mm;
    char material_id[64];
} SimpleSetup;

static bool simple_setup(const JsonValue *pr, SimpleSetup *ss) {
    memset(ss, 0, sizeof *ss);
    if (!pr) return false;
    const JsonValue *m = json_get(pr, "material");
    ss->metal = !strcmp(json_get_str(pr, "technology", ""), "metal");
    const char *lib = json_get_str(m, "library_id", "");
    if (lib[0]) {
        ss->library = fem_library_material(lib, &ss->mat);
        if (!ss->library) return false;
        str_copy(ss->material_id, sizeof ss->material_id, lib);
    } else {
        str_copy(ss->material_id, sizeof ss->material_id, json_get_str(m, "id", ""));
    }
    if (ss->metal) {
        ss->plastic = ss->library && ss->mat.yield_pa > 0;
        ss->strain = json_get(pr, ss->plastic ? "strain" : "strain_elastic");
        if (!ss->strain) ss->strain = json_get(pr, "strain"), ss->plastic = false;
        ss->layer_mm = json_get_num(ss->strain, "layer_mm", json_get_num(pr, "layer_mm", 0.5));
        ss->detail_mm = json_get_num(ss->strain, "detail_mm", json_get_num(pr, "detail_mm", ss->layer_mm));
    } else {
        ss->layer_mm = json_get_num(json_get(pr, "process"), "layer_mm", 1.0);
        ss->detail_mm = json_get_num(pr, "detail_mm", 1.0);
    }
    return true;
}

/* the first sentence of a long source line, for a line of a tooltip */
static void first_part(char *out, size_t cap, const char *src, bool at_semicolon) {
    size_t n = 0;
    for (const char *p = src; *p && n + 1 < cap; p++) {
        if ((*p == '.' || (at_semicolon && *p == ';')) && (p[1] == ' ' || !p[1]) && n > 20) break;
        out[n++] = *p;
    }
    out[n] = 0;
}

/* up to the first full stop or semicolon: a value's source, which runs on in clauses */
static void first_sentence(char *out, size_t cap, const char *src) { first_part(out, cap, src, true); }
/* up to the first full stop: a record's source line, whose clauses name different documents */
static void source_sentence(char *out, size_t cap, const char *src) { first_part(out, cap, src, false); }

/* ---- drawing, one size up from Advanced ---- */

static float say(Ui *ui, int font, float x, float y, float w, uint32_t col, const char *text, int max_lines) {
    panel_say("%s\n", text);
    float lh = ui_line_height(ui, font);
    const char *p = text;
    for (int line = 0; line < max_lines && *p; line++) {
        int n = 0, last_space = 0;
        while (p[n] && ui_text_width_n(ui, font, p, n + 1) <= w) {
            if (p[n] == ' ') last_space = n;
            n++;
        }
        if (p[n] && last_space > 0) n = last_space;
        char buf[256];
        snprintf(buf, sizeof buf, "%.*s", (int)MINI((size_t)n, sizeof buf - 1), p);
        ui_text(ui, font, x, y, col, buf);
        y += lh + 1;
        p += n;
        while (*p == ' ') p++;
    }
    return y;
}

/* the one thing to do on this screen: full width, bright, in the title face */
static bool big_button(Ui *ui, const char *label, float x, float y, float w, bool enabled, const char *why) {
    const float h = 42;
    bool hover = ui_hover(ui, x, y, w, h);
    uint32_t bg = !enabled ? 0xEFEDE9FFu : hover ? 0xE8551CFFu : 0xE8551CFFu;
    ui_rect(ui, x, y, w, h, bg, 8);
    float tw = ui_text_width(ui, FONT_TITLE, label);
    ui_text(ui, FONT_TITLE, x + (w - tw) * 0.5f, y + (h - ui_line_height(ui, FONT_TITLE)) * 0.5f,
            enabled ? 0xF7F6F3FFu : 0xB4B1ABFFu, label);
    ui_block_mouse(ui, x, y, w, h);
    bool clicked = ui_clickable(ui, label, x, y, w, h);
    if (!enabled) {
        if (clicked && why) LOGI("%s", why);
        if (hover && why) {
            float tipw = MINI(ui_text_width(ui, FONT_SMALL, why) + 16, 380.0f);
            ui_rect(ui, x, y - 26, tipw, 22, 0xEFEDE9F4u, 5);
            ui_text(ui, FONT_SMALL, x + 8, y - 22, UI_TEXT, why);
        }
        return false;
    }
    return clicked;
}

/* a card: a choice with a name and a line under it */
static bool card(Ui *ui, const char *id, const char *name, const char *line, float x, float y, float w, float h,
                 bool chosen) {
    bool hover = ui_hover(ui, x, y, w, h);
    ui_rect(ui, x, y, w, h, chosen ? 0xDDDAD4F0u : hover ? 0xEFEDE9F0u : 0xF7F6F3E8u, 6);
    ui_rect_outline(ui, x, y, w, h, chosen ? 0xE8551CD0u : 0xDDDAD4FFu, 6, chosen ? 1.5f : 1);
    ui_text(ui, FONT_BOLD, x + 12, y + 8, chosen ? 0x111111FFu : UI_TEXT, name);
    panel_say("%s\n", name);
    if (line && line[0]) {
        ui_push_clip(ui, x + 4, y, w - 8, h);
        say(ui, FONT_SMALL, x + 12, y + 30, w - 24, UI_DIM, line, 2);
        ui_pop_clip(ui);
    }
    ui_block_mouse(ui, x, y, w, h);
    return ui_clickable(ui, id, x, y, w, h);
}

/* a tooltip that lists where each value of a preset comes from */
static void sources_tooltip(Ui *ui, float x, float y, const JsonValue *pr) {
    const char *lines[8];
    char buf[8][300];
    int n = 0;
    SimpleSetup ss;
    simple_setup(pr, &ss);
    const JsonValue *m = json_get(pr, "material"), *st = ss.strain, *pc = json_get(pr, "process");
    char sent[300];
    if (m) snprintf(buf[n], 300, "material: %s", json_get_str(m, "source", "")), lines[n] = buf[n], n++;
    if (ss.library && ss.mat.youngs_pa > 0) {
        first_sentence(sent, sizeof sent, ss.mat.youngs_source);
        snprintf(buf[n], 300, "stiffness %.4g GPa, Poisson's ratio %.4g (%s): %s", ss.mat.youngs_pa * 1e-9, ss.mat.poisson,
                 ss.mat.youngs_provenance, sent), lines[n] = buf[n], n++;
    }
    if (ss.library) {
        if (ss.mat.yield_pa > 0) {
            first_sentence(sent, sizeof sent, ss.mat.yield_source);
            snprintf(buf[n], 300, "yields at %.0f MPa (%s), so the metal is allowed to yield: %s", ss.mat.yield_pa * 1e-6,
                     ss.mat.yield_provenance, sent), lines[n] = buf[n], n++;
        } else {
            snprintf(buf[n], 300, "no yield value with a source in the record, so the metal is kept elastic"), lines[n] = buf[n], n++;
        }
    }
    if (st) snprintf(buf[n], 300, "how it shrinks as it cools: %s", json_get_str(st, "source", "")), lines[n] = buf[n], n++;
    if (ss.metal)
        snprintf(buf[n], 300, "layer %.3g mm and detail %.3g mm: %s", ss.layer_mm, ss.detail_mm,
                 json_get_str(pr, "layer_source", "")), lines[n] = buf[n], n++;
    else if (json_get(pr, "detail_mm"))
        snprintf(buf[n], 300, "detail %.3g mm: %s", json_get_num(pr, "detail_mm", 0), json_get_str(pr, "detail_source", "")),
            lines[n] = buf[n], n++;
    if (pc) snprintf(buf[n], 300, "nozzle %.0f degC: %s", json_get_num(pc, "nozzle_c", 0), json_get_str(pc, "nozzle_source", "")),
                lines[n] = buf[n], n++;
    if (pc) snprintf(buf[n], 300, "bed %.0f degC: %s", json_get_num(pc, "bed_c", 0), json_get_str(pc, "bed_source", "")),
                lines[n] = buf[n], n++;
    float w = 520, lh = ui_line_height(ui, FONT_SMALL) + 2, h = 14 + n * lh * 2;
    float tx = CLAMP(x - w - 10, 10.0f, (float)app.win_w - w - 10), ty = CLAMP(y, 60.0f, (float)app.win_h - h - 10);
    ui_rect(ui, tx, ty, w, h, 0xEFEDE9F8u, 6);
    ui_rect_outline(ui, tx, ty, w, h, 0xE8551C80u, 6, 1);
    float yy = ty + 7;
    for (int i = 0; i < n; i++) yy = say(ui, FONT_SMALL, tx + 10, yy, w - 20, UI_TEXT, lines[i], 2) + 2;
}

/* ---- the screens ---- */

static void simple_take_sample(int i) {
    const JsonValue *smp = json_at(simple_samples(), (size_t)i);
    char rel[512], path[1024];
    snprintf(rel, sizeof rel, "samples/%s", json_get_str(smp, "file", ""));
    if (!app_resource_path(rel, path, sizeof path)) {
        LOGE("the sample '%s' is missing from this installation: reinstall the application", json_get_str(smp, "name", ""));
        return;
    }
    simple_sample = i;
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "load '%s'", path);
    exec_cmd(cmd);
    snprintf(cmd, sizeof cmd, "solid import %s", json_get_str(smp, "unit", "mm")); /* the sample knows its unit */
    exec_cmd(cmd);
    fem_simple_after_import();
    exec_cmd("fem fit");
}

static float simple_part(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    char a[400];
    const char *waiting = fem_pending_import();
    if (waiting) { /* the one thing never guessed: the unit */
        double ext[3];
        const char *base = strrchr(waiting, '/');
        y = say(ui, FONT_BOLD, x, y, w, UI_TEXT, base ? base + 1 : waiting, 1) + 6;
        if (fem_pending_extent(ext)) {
            double L = MAXI(MAXI(ext[0], ext[1]), ext[2]);
            snprintf(a, sizeof a, "How long is this part? The file says %.3g, but not in what unit.", L);
            y = say(ui, FONT_BOLD, x, y, w, UI_ACCENT, a, 3) + 10;
            static const struct { const char *unit, *word; } U[3] = {{"mm", "mm"}, {"cm", "cm"}, {"in", "inches"}};
            for (int i = 0; i < 3; i++) {
                char label[64], id[64];
                snprintf(label, sizeof label, "%.3g %s long", L, U[i].word);
                snprintf(id, sizeof id, "unit-%s", U[i].unit);
                if (card(ui, id, label, NULL, x, y, w, 34, false)) {
                    char cmd[48];
                    snprintf(cmd, sizeof cmd, "solid import %s", U[i].unit);
                    exec_cmd(cmd);
                    fem_simple_after_import();
                    exec_cmd("fem fit");
                }
                y += 40;
            }
        } else {
            y = say(ui, FONT_BOLD, x, y, w, UI_BAD, "This file could not be read as an STL. Export it again from your CAD program as STL.", 3) + 6;
        }
        return y;
    }
    if (s->nbodies == 0) {
        y = say(ui, FONT_BOLD, x, y, w, UI_TEXT, "Drop an STL file of your part onto the window, or start with one of these:", 3) + 12;
        const JsonValue *ss = simple_samples();
        for (size_t i = 0; i < json_len(ss) && i < 3; i++) {
            const JsonValue *smp = json_at(ss, i);
            char id[64];
            snprintf(id, sizeof id, "%s", json_get_str(smp, "name", "sample"));
            if (card(ui, id, json_get_str(smp, "name", ""), json_get_str(smp, "about", ""), x, y, w, 64, false))
                simple_take_sample((int)i);
            y += 70;
        }
        if (!json_len(ss))
            y = say(ui, FONT_SMALL, x, y, w, UI_ACCENT2, "The sample parts are missing from this installation.", 2) + 6;
        y += 4;
        if (ui_button(ui, "OPEN A FILE...##simple", x, y, w, 28, false)) exec_cmd("open");
        return y + 34;
    }
    /* a part is here: what it is, in one sentence */
    const JsonValue *smp = simple_current_sample();
    y = say(ui, FONT_BOLD, x, y, w, UI_TEXT, smp ? json_get_str(smp, "name", s->body) : s->body, 1) + 4;
    snprintf(a, sizeof a, "%.3g x %.3g x %.3g mm", s->size_mm[0], s->size_mm[1], s->size_mm[2]);
    y = say(ui, FONT_SMALL, x, y, w, UI_DIM, a, 1) + 8;
    const char *verdict = fem_part_sentence();
    if (verdict[0]) y = say(ui, FONT_BOLD, x, y, w, strstr(verdict, "closed solid") || strstr(verdict, "small gaps") ? UI_GOOD : UI_ACCENT2, verdict, 4) + 8;
    if (ui_button(ui, "CHOOSE ANOTHER PART##simple", x, y, w, 26, false)) {
        exec_cmd("solid new");
        simple_sample = -1;
    }
    return y + 32;
}

/* Every material of the library for this kind of printer, with its status and the first sentence of where its numbers
 * come from. Only a material with calibrated print settings above can be simulated here: a print also needs the
 * distortion measured for that material on a machine, and the list says so rather than offering what cannot run. */
static bool simple_other_open;

static float simple_other_materials(Ui *ui, float x, float y, float w, const JsonValue *ps, const char *tech) {
    const JsonValue *arr = fem_material_list(); /* the same records an agent gets from material_list */
    const char *proc = !strcmp(tech, "metal") ? "lpbf" : "fff";
    const JsonValue *list[64];
    int n = 0;
    for (size_t i = 0; i < json_len(arr) && n < 64; i++) {
        const JsonValue *rec = json_at(arr, i);
        if (!record_has_process(rec, proc)) continue;
        const char *id = json_get_str(rec, "id", "");
        bool in_preset = false;
        for (size_t k = 0; k < json_len(ps) && !in_preset; k++) {
            const JsonValue *m = json_get(json_at(ps, k), "material");
            in_preset = !strcmp(json_get_str(m, "library_id", ""), id) || !strcmp(json_get_str(m, "id", ""), id);
        }
        if (!in_preset) list[n++] = rec;
    }
    if (!n) return y;
    char label[96];
    snprintf(label, sizeof label, "OTHER MATERIALS IN THE LIBRARY (%d)##simple", n);
    if (ui_button(ui, label, x, y, w, 24, simple_other_open)) simple_other_open = !simple_other_open;
    y += 30;
    if (!simple_other_open) return y;
    y = say(ui, FONT_SMALL, x, y, w, UI_DIM,
            "Green: every number has a source. Amber: demonstration values. None of these can be printed here yet: a print "
            "also needs the distortion measured for the material on a machine.", 4) + 6;
    for (int i = 0; i < n; i++) {
        const JsonValue *rec = list[i];
        const char *st = json_get_str(rec, "status", "");
        bool sourced = !strcmp(st, "measured") || !strcmp(st, "published");
        ui_rect(ui, x, y + 5, 6, 6, sourced ? UI_GOOD : UI_ACCENT2, 3);
        y = say(ui, FONT_BOLD, x + 12, y, w - 12, UI_TEXT, json_get_str(rec, "name", ""), 2) + 2;
        char line[480];
        snprintf(line, sizeof line, "%s. %s", !strcmp(st, "demonstration") ? "Demonstration values, not for design"
                                            : !strcmp(st, "measured")   ? "Measured"
                                                                        : "Published values", json_get_str(rec, "source_line", ""));
        y = say(ui, FONT_SMALL, x + 12, y, w - 12, UI_DIM, line, 3) + 8;
    }
    return y;
}

/* does any installed preset print with this technology ("metal" or "plastic")? */
static bool simple_has_tech(const char *tech) {
    const JsonValue *ps = simple_profiles();
    for (size_t i = 0; i < json_len(ps); i++)
        if (!strcmp(json_get_str(json_at(ps, i), "technology", ""), tech)) return true;
    return false;
}

/* No metal preset ships: how much a metal shrinks as it cools on a machine is calibrated on prints from that machine,
 * never assumed, so the screen asks for that calibration instead of offering a number nobody measured. */
static const char SIMPLE_NO_METAL[] =
    "No metal print settings are installed. A metal print needs to know how much the metal shrinks as it cools on your "
    "machine, and that is measured, never assumed: print the calibration cantilever (kit/README.md), measure how far its "
    "free end rises once it is cut from the plate, and calibrate it in Advanced mode (lpbf_calibrate). Then add the "
    "result, with its source, to print_profiles.json. Until then, choose plastic filament.";

static float simple_print(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    const float gap = 6;
    float hw = (w - gap) / 2;
    /* the screen opens on a kind of printer that can be simulated: metal once a calibrated metal preset is installed */
    if (simple_tech < 0) simple_tech = simple_has_tech("metal") ? 0 : 1;
    if (card(ui, "Metal powder", "Metal powder", "a laser melts metal powder, layer by layer", x, y, hw, 64, simple_tech == 0))
        simple_tech = 0, simple_profile = -1;
    if (card(ui, "Plastic filament", "Plastic filament", "a hot nozzle lays down melted plastic", x + hw + gap, y, hw, 64, simple_tech == 1))
        simple_tech = 1, simple_profile = -1;
    y += 76;
    y = say(ui, FONT_BOLD, x, y, w, UI_TEXT, "Print settings", 1) + 4;
    const JsonValue *ps = simple_profiles();
    const char *want = simple_tech == 0 ? "metal" : "plastic";
    int shown = 0;
    for (size_t i = 0; i < json_len(ps); i++) {
        const JsonValue *pr = json_at(ps, i);
        if (strcmp(json_get_str(pr, "technology", ""), want) != 0) continue;
        if (simple_profile < 0) simple_profile = (int)i; /* the first one that fits is chosen, and shown as chosen */
        char id[96];
        snprintf(id, sizeof id, "%s", json_get_str(pr, "subtitle", json_get_str(pr, "name", "")));
        if (card(ui, id, json_get_str(pr, "name", ""), json_get_str(pr, "subtitle", ""), x, y, w, 58, simple_profile == (int)i))
            simple_profile = (int)i;
        if (ui_hover(ui, x, y, w, 58)) sources_tooltip(ui, x, y, pr);
        y += 64;
        shown++;
    }
    if (!shown)
        y = say(ui, FONT_SMALL, x, y, w, UI_ACCENT2,
                simple_tech == 0 ? SIMPLE_NO_METAL : "No print settings for this kind of printer are installed yet.", 9) + 6;
    y = say(ui, FONT_SMALL, x, y, w, UI_FAINT, "Rest the pointer on a setting to see where each of its numbers comes from.", 2) + 6;
    y = simple_other_materials(ui, x, y, w, ps, want) + 6;

    /* which face sits on the plate */
    y = say(ui, FONT_BOLD, x, y, w, UI_TEXT, "Which face sits on the plate?", 1) + 4;
    if (simple_current_sample()) {
        y = say(ui, FONT_SMALL, x, y, w, UI_DIM, "The sample is placed the way it was printed.", 2) + 6;
    } else {
        y = say(ui, FONT_SMALL, x, y, w, UI_DIM,
                simple_face_mode ? "Click the face on the part that should sit on the plate."
                                 : "Now: the face that is at the bottom in your file. If the part was printed another way up, choose that face.",
                3) + 6;
        const char *ps2 = fem_plate_sentence();
        if (ps2[0] && !simple_face_mode) y = say(ui, FONT_SMALL, x, y, w, UI_GOOD, ps2, 2) + 4;
        if (ui_button(ui, simple_face_mode ? "CANCEL##face" : "CHOOSE THE FACE ON THE PART##face", x, y, w, 26, simple_face_mode))
            simple_face_mode = !simple_face_mode;
        y += 32;
    }
    (void)s;
    return y;
}

/* how long a build takes, from the two sample brackets timed on an M2 Air: 28.6 s for 4 800 elements and 24 layers,
 * 78 s for 7 680 and 30; the time grows a little faster than elements times layers */
static double simple_estimate_s(int elements, int layers) {
    double work = (double)elements * (double)layers;
    return work > 0 ? 28.6 * pow(work / 115200.0, 1.45) : 0;
}

static void simple_duration(char *out, size_t cap, double sec) {
    if (sec < 90) snprintf(out, cap, "%.0f seconds", sec);
    else snprintf(out, cap, "%.0f minutes", ceil(sec / 60.0));
}

/* One simulation at a time is what a person expects of a Simulate button. The engine runs one job at a time, so
 * anything still queued or running would make this one wait unseen: it is stopped first, and the log says so. */
static void simple_make_way(void) {
    int n = fem_stop_all_jobs();
    if (n > 0) LOGI("stopped %d earlier simulation%s so that this one starts now", n, n == 1 ? "" : "s");
}

static bool simple_start(void) {
    const FemState *s = fem_state();
    const JsonValue *pr = simple_current_profile();
    if (!pr || s->nbodies == 0) return false;
    const JsonValue *m = json_get(pr, "material");
    SimpleSetup ss;
    if (!simple_setup(pr, &ss)) {
        LOGE("the print settings name the material '%s', which is not in this application's material library: "
             "update the application, or choose other print settings", json_get_str(m, "library_id", "?"));
        return false;
    }
    const char *mid = ss.material_id;
    char cmd[2600];
    if (!ss.library && json_get(m, "youngs_modulus_mpa")) { /* a preset that carries its own material: define it, then give it */
        snprintf(cmd, sizeof cmd,
                 "{\"material\": {\"id\": \"%s\", \"name\": \"%s\", \"family\": \"metal\", \"status\": \"%s\", \"processes\": [\"lpbf\"], "
                 "\"provenance\": \"%s\", \"density_kg_m3\": {\"value\": %.10g}, \"youngs_modulus_pa\": {\"value\": %.10g}, "
                 "\"poisson_ratio\": {\"value\": %.10g}}}",
                 mid, json_get_str(m, "name", mid), json_get_str(m, "status", "user_supplied"), json_get_str(m, "source", "preset"),
                 json_get_num(m, "density_kg_m3", 0), json_get_num(m, "youngs_modulus_mpa", 0) * 1e6,
                 json_get_num(m, "poissons_ratio", 0.33));
        fem_op("material_define", "%s", cmd);
    }
    simple_make_way();
    if (!fem_op("material_assign", "{\"body\": \"%s\", \"material\": \"%s\", \"source\": \"user\"}", s->body, mid)) return false;
    if (!fem_op("mesh_generate", "{\"element_size\": \"%.10g mm\"}", ss.detail_mm)) return false;
    const JsonValue *smp = simple_current_sample();
    const JsonValue *cut = smp ? json_get(smp, "cut") : NULL;
    bool ok;
    if (ss.metal) {
        const JsonValue *st = ss.strain;
        char cutjs[300] = "";
        if (cut)
            snprintf(cutjs, sizeof cutjs, ", \"cut\": {\"height\": \"%.10g mm\", \"kerf\": \"%.10g mm\", \"from_x\": \"%.10g mm\", \"provenance\": \"assumed\"}",
                     json_get_num(cut, "height_mm", 0), json_get_num(cut, "kerf_mm", 0), json_get_num(cut, "from_x_mm", 0));
        /* the elastic constants are the library record's; an inline preset material keeps its own */
        double e_mpa = ss.library ? ss.mat.youngs_pa * 1e-6 : json_get_num(m, "youngs_modulus_mpa", 70000);
        double nu = ss.library ? ss.mat.poisson : json_get_num(m, "poissons_ratio", 0.33);
        char plastic[700] = "";
        if (ss.plastic) {
            char hard[80] = "";
            if (ss.mat.hardening_pa > 0) snprintf(hard, sizeof hard, ", \"hardening_modulus\": \"%.10g MPa\"", ss.mat.hardening_pa * 1e-6);
            /* the source field holds 191 characters: the record and the key say where the full line is */
            char src[192];
            snprintf(src, sizeof src, "library record %s, yield_strength_pa: %s", ss.mat.id, ss.mat.yield_source);
            for (char *q = src; *q; q++) if (*q == '"' || *q == '\\') *q = '\'';
            snprintf(plastic, sizeof plastic,
                     ", \"plasticity\": {\"yield_strength\": \"%.10g MPa\"%s, \"source\": \"%s\", \"provenance\": \"%s\"}",
                     ss.mat.yield_pa * 1e-6, hard, src, ss.mat.yield_provenance[0] ? ss.mat.yield_provenance : "published");
        }
        snprintf(cmd, sizeof cmd,
                 "{\"body\": \"%s\", \"build_orientation\": \"%s\", \"layer_thickness_sim\": \"%.10g mm\", "
                 "\"inherent_strain\": {\"exx\": %.12g, \"eyy\": %.12g, \"ezz\": %.12g, \"provenance\": \"%s\", \"source\": \"%.150s\"}, "
                 "\"material\": {\"youngs_modulus\": \"%.10g MPa\", \"poissons_ratio\": %.10g, \"provenance\": \"user\"}%s%s, "
                 "\"label\": \"Simple mode: %s\"}",
                 s->body, json_get_str(pr, "orientation", "Y"), ss.layer_mm,
                 json_get_num(st, "exx", 0), json_get_num(st, "eyy", 0), json_get_num(st, "ezz", 0),
                 json_get_str(st, "provenance", "calibrated"), json_get_str(st, "source", ""),
                 e_mpa, nu, plastic, cutjs, json_get_str(pr, "name", ""));
        ok = fem_op("lpbf_build_run", "%s", cmd);
    } else {
        const JsonValue *pc = json_get(pr, "process");
        snprintf(cmd, sizeof cmd,
                 "{\"body\": \"%s\", \"process\": {\"layer_height\": \"%.10g mm\", \"printed_layer_height\": \"%.10g mm\", "
                 "\"nozzle_temperature\": \"%.10g degC\", \"bed_temperature\": \"%.10g degC\", \"ambient_temperature\": \"%.10g degC\", "
                 "\"deposition_rate\": \"%.10g mm^3/s\", \"provenance\": \"user\"}, \"label\": \"Simple mode: %s\"}",
                 s->body, json_get_num(pc, "layer_mm", 1), json_get_num(pc, "printed_layer_mm", 0.2), json_get_num(pc, "nozzle_c", 210),
                 json_get_num(pc, "bed_c", 60), json_get_num(pc, "ambient_c", 30), json_get_num(pc, "rate_mm3_s", 8),
                 json_get_str(pr, "name", ""));
        ok = fem_op("mech_print_run", "%s", cmd);
    }
    if (!ok) return false;
    const char *id = json_get_str(fem_last_value(), "job_id", NULL);
    if (!id) return false;
    str_copy(simple_job, sizeof simple_job, id);
    simple_job_t0 = app.time;
    fem_follow_job(id);
    fem_set_build_preview(0);
    LOGOK("simulation started");
    return true;
}

static float simple_run(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    const JsonValue *pr = simple_current_profile();
    char a[300];
    bool running = simple_job[0] && s->job_active && !strcmp(s->job_id, simple_job);
    if (!pr) return say(ui, FONT_BOLD, x, y, w, UI_ACCENT2, "Choose the print settings first.", 2);
    SimpleSetup ss;
    simple_setup(pr, &ss);
    double layer = ss.layer_mm;
    int layers = layer > 0 ? (int)ceil(s->size_mm[2] / layer - 1e-9) : 0;
    snprintf(a, sizeof a, "%s, %s. %d layers of %.3g mm.", simple_current_sample() ? json_get_str(simple_current_sample(), "name", s->body) : s->body,
             json_get_str(pr, "name", ""), layers, layer);
    y = say(ui, FONT_BOLD, x, y, w, UI_TEXT, a, 3) + 8;
    /* an estimate needs the size of the calculation, so the part is divided before the button is pressed */
    if (!running && !simple_meshed_for_estimate && s->nbodies) {
        fem_op("mesh_generate", "{\"element_size\": \"%.10g mm\"}", ss.detail_mm);
        simple_meshed_for_estimate = true;
    }
    if (!running) {
        double est = simple_estimate_s(s->nelems, layers);
        if (est > 0 && ss.plastic) {
            /* with plasticity each layer iterates, and how much depends on how much of the part yields, which is not
             * known before it runs. Timed at 1 mm with the AlSi10Mg record on an M2 Air: the bracket took 5 times the
             * elastic estimate (2.7 s), the cantilever 7.5 times (47.6 s), the specimen 21 times (226 s). */
            char lo[40], hi[40];
            simple_duration(lo, sizeof lo, MAXI(5.0 * est, 3.0));
            simple_duration(hi, sizeof hi, MAXI(21.0 * est, 10.0));
            snprintf(a, sizeof a, "Between %s and %s on this computer: how much the metal yields decides, and that is only "
                                  "known once it runs.", lo, hi);
            y = say(ui, FONT_SMALL, x, y, w, UI_DIM, a, 3) + 8;
        } else if (est > 0) {
            snprintf(a, sizeof a, "About %.0f seconds on this computer, estimated from the size of the part.", MAXI(est, 5.0));
            y = say(ui, FONT_SMALL, x, y, w, UI_DIM, a, 2) + 8;
        }
        if (simple_job[0] && !strcmp(s->job_state, "cancelled") && !strcmp(s->job_id, simple_job)) {
            y = say(ui, FONT_BOLD, x, y, w, UI_TEXT, "Stopped. Nothing is running now.", 1) + 4;
            y = say(ui, FONT_SMALL, x, y, w, UI_DIM, "Press Simulate to run it again, or go back to change the part or the print settings.", 3) + 6;
        } else if (s->job_error[0] && simple_job[0]) {
            y = say(ui, FONT_BOLD, x, y, w, UI_BAD, s->job_error, 4) + 6;
        }
        return y;
    }
    /* running: a bar that speaks, and how long is left from how fast it has gone */
    y = say(ui, FONT_BOLD, x, y, w, UI_ACCENT, s->job_stage[0] ? s->job_stage : "starting", 1) + 6;
    progress_bar(ui, x, y, w, 10, s->job_progress, UI_ACCENT);
    y += 18;
    double el = app.time - simple_job_t0;
    if (s->job_progress > 0.05) {
        /* a layer costs more the more of the part already exists, so the work done grows about as the square of the
         * share of layers done; a straight line from the progress would promise far too little */
        double fw = s->job_progress * s->job_progress;
        snprintf(a, sizeof a, "about %.0f seconds left", MAXI(0.0, el / fw - el));
        y = say(ui, FONT_SMALL, x, y, w, UI_DIM, a, 1) + 4;
    }
    fem_set_build_preview(s->job_progress);
    if (fem_job_stopping()) {
        y = say(ui, FONT_SMALL, x, y, w, UI_ACCENT2, "Stopping: it ends with the layer it is on.", 2);
        return y + 8;
    }
    if (ui_button(ui, "STOP##simple", x, y, 100, 26, false)) fem_stop_job(simple_job);
    return y + 32;
}

static void simple_story_text(char *out, size_t cap, const FemStory *st, bool with_marker) {
    const JsonValue *pr = simple_current_profile();
    const JsonValue *smp = simple_current_sample();
    const FemState *s = fem_state();
    SimpleSetup ss;
    simple_setup(pr, &ss);
    bool metal = pr ? ss.metal : !strcmp(s->result_kind, "lpbf_build");
    const JsonValue *sum = fem_result_summary();
    const JsonValue *model = json_get(sum, "model");
    bool cut = (smp && json_get(smp, "cut")) || json_get(model, "cut");
    /* a build Simple mode did not start says what it was given, not what a preset would have said */
    char given[260] = "";
    if (!simple_job[0] && metal)
        snprintf(given, sizeof given, "It uses the settings the run was given, not a preset of this window: "
                                      "MORE, then ALL CONTROLS, shows each one with its source.");
    bool demo = !strcmp(s->material_status, "demonstration");
    /* the layer the run itself used, which is the one the recoater lays */
    double layer = json_get_num(model, "layer_thickness_mm", ss.layer_mm > 0 ? ss.layer_mm : 1.0);
    /* the yield value the stress is compared with: the one the run used, else the assigned record's, only with a source */
    double yield = st->plastic ? st->yield_mpa : 0;
    if (!(yield > 0) && s->material[0]) {
        FemMaterialInfo mi;
        if (fem_library_material(s->material, &mi) && mi.yield_pa > 0) yield = mi.yield_pa * 1e-6;
    }
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n,
                          "What this is: a simulation of how printing bends this part and the stress it leaves inside. %s%s "
                          "It has not been checked against a measurement of this part. %s %s%s\n\n",
                          given[0] ? given
                          : metal  ? "The print settings were calibrated on test bars printed with the same settings."
                                   : "The plastic's values were measured on printed parts and each names its source; the print settings are typical, not calibrated on this printer.",
                          st->plastic ? " The metal is allowed to yield: where the stress reaches the material's yield value, "
                                        "the metal stretches for good instead of carrying more."
                                      : "",
                          st->support_elements ? "The supports are simulated as a lighter lattice under the part and drawn in grey."
                                               : "Supports are not simulated.",
                          cut ? (smp ? "The part is cut off the plate at the end, as the calibration bars were."
                                     : "The part is cut off the plate at the end.")
                              : "The part is shown still on the plate: cutting it off is simulated only for the calibration bar.",
                          demo ? " The material values are demonstration values, not a grade you can buy." : "");
    if (n < cap)
        n += (size_t)snprintf(out + n, cap - n, "Warp: up to %.2g mm, %s, %s%s.\n\n", st->warp_mm, st->warp_direction,
                              st->warp_where, with_marker ? " (the cross marks it)" : "");
    if (n < cap)
        n += (size_t)snprintf(out + n, cap - n, "Residual stress: up to %.0f MPa, %s.%s\n\n", st->stress_mpa, st->stress_where,
                              strstr(st->stress_where, "plate") ? " A peak at a sharp corner on the plate depends on how finely the part is divided: read it as \"high here\", not as an exact value." : "");
    if (n < cap) {
        /* the recoater first, because it is the one that stops a print; then the stress against the yield value */
        if (st->rise_mm > layer)
            n += (size_t)snprintf(out + n, cap - n, "Risk: while it was printed, part of it rose %.2g mm, more than one layer (%.3g mm). "
                                                    "The recoater may hit the part. ", st->rise_mm, layer);
        else
            n += (size_t)snprintf(out + n, cap - n, "Risk: nothing rose more than one layer (%.3g mm) while it was printed. ", layer);
    }
    if (n < cap) {
        if (yield > 0) {
            n += (size_t)snprintf(out + n, cap - n, "Residual stress up to %.0f MPa %s; the material yields at %.0f MPa.",
                                  st->stress_mpa, st->stress_where, yield);
            if (n < cap && st->plastic && st->yielded && st->peak_plastic > 0)
                n += (size_t)snprintf(out + n, cap - n, " It yielded in places: the largest permanent stretch is %.2g %%.",
                                      st->peak_plastic * 100.0);
            else if (n < cap && st->stress_mpa < yield)
                n += (size_t)snprintf(out + n, cap - n, " It stays below that everywhere.");
        } else {
            n += (size_t)snprintf(out + n, cap - n, "Residual stress up to %.0f MPa %s; no yield value with a source, so it "
                                                    "is not compared with one.", st->stress_mpa, st->stress_where);
        }
    }
    if (n < cap && st->support_elements)
        n += (size_t)snprintf(out + n, cap - n, "\n\nSupports: %.0f mm\xC2\xB3 of support material under the part, drawn in grey%s.",
                              st->support_mm3, st->supports_removed ? "; they are removed after the build" : "");
}

static float simple_results(Ui *ui, float x, float y, float w) {
    const FemState *s = fem_state();
    FemStory st;
    if (!s->have_result || !fem_build_story(&st)) {
        return say(ui, FONT_BOLD, x, y, w, UI_DIM, "Simulate the part first.", 2);
    }
    char text[3072];
    simple_story_text(text, sizeof text, &st, true);
    /* the statement of what this is comes first, and in amber: read it before the numbers */
    char *sep = strstr(text, "\n\n");
    if (sep) *sep = 0;
    y = say(ui, FONT_SMALL, x, y, w, UI_ACCENT2, text, 12) + 10;
    const char *rest = sep ? sep + 2 : "";
    while (*rest) {
        const char *e = strstr(rest, "\n\n");
        char para[600];
        snprintf(para, sizeof para, "%.*s", e ? (int)(e - rest) : (int)strlen(rest), rest);
        uint32_t col = !strncmp(para, "Risk: while", 11) ? UI_BAD : UI_TEXT;
        y = say(ui, FONT_BOLD, x, y, w, col, para, 10) + 8;
        rest = e ? e + 2 : rest + strlen(rest);
    }
    /* three controls, and everything else behind MORE */
    const float gap = 6;
    float tw = (w - 2 * gap) / 3;
    if (ui_button(ui, "PLAY##simple", x, y, tw, 26, fem_playing())) {
        exec_cmd("fem pause");
        exec_cmd(simple_true_size ? "fem deform true" : "fem deform auto");
        exec_cmd("fem step 1");
        exec_cmd("fem play");
    }
    if (ui_button(ui, "TRUE SIZE##simple", x + tw + gap, y, tw, 26, simple_true_size)) {
        simple_true_size = !simple_true_size;
        exec_cmd(simple_true_size ? "fem deform true" : "fem deform auto");
    }
    if (ui_button(ui, "MORE##simple", x + 2 * (tw + gap), y, tw, 26, simple_more)) simple_more = !simple_more;
    y += 32;
    if (simple_more) {
        /* what the first look does not need: the supports on and off, and every control */
        float hw = (w - gap) / 2;
        if (st.support_elements) {
            if (ui_button(ui, "SUPPORT##simple", x, y, hw, 24, fem_group_shown(1))) fem_show_group(1, !fem_group_shown(1));
        } else {
            say(ui, FONT_SMALL, x, y + 5, hw, UI_FAINT, "no supports in this build", 1);
        }
        if (ui_button(ui, "ALL CONTROLS##simple", x + hw + gap, y, hw, 24, false)) app_set_ui_mode(UI_ADVANCED);
        y += 30;
    }
    y = say(ui, FONT_SMALL, x, y, w, UI_FAINT,
            simple_true_size ? "True size: the shape is drawn as it really moves." : "Exaggerated so the shape of the warp can be seen.", 2) + 4;
    double v = fem_section_on() ? fem_section_position() : 1.0;
    if (slider_row(ui, "sl_simple_section", "look inside", fem_section_on() ? "cut" : "whole", x, y, w, &v, 0.05, 1.0, false)) {
        if (v > 0.98) fem_section(-1, 0.5);
        else fem_section(0, v);
    }
    return y + 34;
}

/* back to the first screen with nothing chosen. Anything still running is stopped first: starting over means
 * starting over, and a simulation left behind would hold up the next one. */
static void simple_restart(bool new_project) {
    int n = fem_stop_all_jobs();
    if (n > 0) LOGI("stopped %d simulation%s to start over", n, n == 1 ? "" : "s");
    if (new_project) exec_cmd("solid new");
    simple_sample = -1, simple_job[0] = 0, simple_report_dir[0] = 0, simple_meshed_for_estimate = false;
    simple_face_mode = false;
    simple_screen = SIMPLE_PART;
    fem_set_build_preview(-1);
}

static float simple_report(Ui *ui, float x, float y, float w) {
    if (simple_report_dir[0]) {
        char a[1200];
        y = say(ui, FONT_BOLD, x, y, w, UI_GOOD, "The report is written.", 1) + 6;
        snprintf(a, sizeof a, "It is in %s. It says what the results screen says, with the pictures, and where every number came from.", simple_report_dir);
        y = say(ui, FONT_SMALL, x, y, w, UI_DIM, a, 5) + 8;
    } else {
        y = say(ui, FONT_BOLD, x, y, w, UI_TEXT, "The report holds what the results screen says, the pictures, and where every number came from.", 4) + 8;
    }
    if (ui_button(ui, "START AGAIN WITH ANOTHER PART##simple", x, y, w, 26, false)) simple_restart(true);
    return y + 32;
}

bool hud_simple_face_mode(void) { return app.ui_mode == UI_SIMPLE && simple_screen == SIMPLE_PRINT && simple_face_mode; }

void hud_simple_face_clicked(float px, float py) {
    int patch = fem_patch_at(px, py);
    if (patch < 0) {
        LOGI("that was not on the part: click a face of the part");
        return;
    }
    if (fem_place_face_down(patch)) {
        simple_face_mode = false;
        exec_cmd("fem fit");
    }
}

static void simple_panel(float px, float py, float pw, float ph, double time) {
    Ui *ui = app.ui;
    const FemState *s = fem_state();
    ui_block_mouse(ui, px, py, pw, ph);
    ui_rect_grad(ui, px, py, pw, ph, 0xF7F6F3F2u, 0xFFFFFFF6u, 10);
    ui_rect_outline(ui, px, py, pw, ph, 0xB4B1AB70u, 10, 1);
    float x = px + 18, w = pw - 36, y = py + 16;
    panel_text[0] = 0, panel_text_len = 0;
    (void)time;

    /* the part is gone from under this screen (a new project made in another mode or by an agent): the first
     * screen, not a Simulate button that can never be pressed */
    if (simple_screen > SIMPLE_PART && s->nbodies == 0 && !fem_pending_import()) simple_restart(false);
    /* a print simulation that is already going, started in Advanced or by an agent, is the one this screen shows:
     * switching mode never hides work that is running, and never offers a second Simulate behind it */
    /* never a job that is on its way out (it ends with its layer, and would be adopted again every frame), and never
     * without a part on screen: that job belongs to a project this screen has just left */
    if (s->job_active && s->job_id[0] && strcmp(s->job_id, simple_job) && !fem_job_stopping() && s->nbodies > 0 &&
        (!strcmp(s->job_kind, "lpbf_build") || !strcmp(s->job_kind, "fff_print"))) {
        str_copy(simple_job, sizeof simple_job, s->job_id);
        simple_job_t0 = app.time;
        simple_screen = SIMPLE_RUN;
    }
    /* follow the work: a finished build moves on to its results by itself */
    bool running = simple_job[0] && s->job_active && !strcmp(s->job_id, simple_job);
    if (simple_screen == SIMPLE_RUN && simple_job[0] && !running && s->have_result && !strcmp(s->result_job, simple_job)) {
        fem_set_build_preview(-1);
        exec_cmd("fem field displacement");
        exec_cmd(simple_true_size ? "fem deform true" : "fem deform auto");
        exec_cmd("fem step last"); /* the finished part, not the first layer */
        simple_screen = SIMPLE_RESULTS;
    }
    /* a build made elsewhere (Agent mode, a project opened from disk) is shown as a result too: switching mode never
     * loses work. Once per result, so "back" still goes back. */
    static char adopted[64] = "";
    if (!simple_job[0] && s->have_result && strcmp(adopted, s->result_job) &&
        (!strcmp(s->result_kind, "lpbf_build") || !strcmp(s->result_kind, "fff_print")) && simple_screen == SIMPLE_PART) {
        str_copy(adopted, sizeof adopted, s->result_job);
        exec_cmd("fem field displacement");
        exec_cmd("fem step last");
        simple_screen = SIMPLE_RESULTS;
    }

    /* where you are: five dots and the screen's name */
    for (int i = 0; i < SIMPLE_COUNT; i++) {
        float cx = x + i * 18;
        uint32_t col = i == simple_screen ? UI_ACCENT : i < simple_screen ? 0xE8551CFFu : 0xDDDAD4FFu;
        ui_rect(ui, cx, y + 4, 10, 10, col, 5);
    }
    char where[64];
    snprintf(where, sizeof where, "step %d of %d", simple_screen + 1, SIMPLE_COUNT);
    ui_text_right(ui, FONT_SMALL, x + w, y + 2, UI_DIM, where);
    y += 26;
    y = say(ui, FONT_TITLE, x, y, w, 0x111111FFu, SIMPLE_TITLE[simple_screen], 1) + 12;

    /* the screen's content scrolls between the title and the back link when it is taller than that space */
    static int last_screen = -1;
    if (simple_screen != last_screen) simple_scroll_y = 0, last_screen = simple_screen;
    float top = y, room = py + ph - 62 - 28 - top;
    ui_push_clip(ui, px, top, pw, room);
    float y0 = y - simple_scroll_y;
    switch (simple_screen) {
    case SIMPLE_PART: y = simple_part(ui, x, y0, w); break;
    case SIMPLE_PRINT: y = simple_print(ui, x, y0, w); break;
    case SIMPLE_RUN: y = simple_run(ui, x, y0, w); break;
    case SIMPLE_RESULTS: y = simple_results(ui, x, y0, w); break;
    default: y = simple_report(ui, x, y0, w); break;
    }
    ui_pop_clip(ui);
    simple_scroll_max = MAXI(0.0f, (y - y0) - room);
    simple_scroll_y = MINI(simple_scroll_y, simple_scroll_max);
    if (simple_scroll_max > 0) { /* a thin bar says there is more, and where you are in it */
        float frac = room / (room + simple_scroll_max), bh = MAXI(24.0f, room * frac);
        float bt = top + (room - bh) * (simple_scroll_y / simple_scroll_max);
        ui_rect(ui, px + pw - 7, bt, 3, bh, 0xE8551C60u, 1.5f);
    }
    y = MINI(y, top + room);
    if (fem_last_error()[0] && !running) y = say(ui, FONT_SMALL, x, y, w, UI_BAD, fem_last_error(), 3) + 4;

    /* the one thing to do, at the bottom, always in the same place */
    float by = py + ph - 62;
    const char *label = NULL, *why = NULL;
    bool enabled = true;
    switch (simple_screen) {
    case SIMPLE_PART:
        label = "Next";
        enabled = s->nbodies > 0 && !fem_pending_import();
        why = fem_pending_import() ? "answer how long the part is first" : "choose a part first";
        break;
    case SIMPLE_PRINT:
        label = "Next";
        enabled = simple_current_profile() != NULL;
        why = simple_tech == 0 && !simple_has_tech("metal") ? "a metal print needs your calibration first, or choose plastic filament"
                                                             : "choose the print settings first";
        break;
    case SIMPLE_RUN:
        label = running ? "Simulating..." : "Simulate";
        enabled = !running && simple_current_profile() && s->nbodies;
        why = running ? "the simulation is running" : "choose a part and print settings first";
        break;
    case SIMPLE_RESULTS:
        label = "Write the report";
        enabled = s->have_result;
        why = "simulate the part first";
        break;
    default:
        label = "Open the report";
        enabled = simple_report_dir[0] != 0;
        why = "write the report first";
        break;
    }
    if (big_button(ui, label, x, by, w, enabled, why)) {
        switch (simple_screen) {
        case SIMPLE_PART: simple_screen = SIMPLE_PRINT; break;
        case SIMPLE_PRINT: simple_screen = SIMPLE_RUN, simple_meshed_for_estimate = false, simple_face_mode = false; break;
        case SIMPLE_RUN: simple_start(); break;
        case SIMPLE_RESULTS: {
            FemStory st;
            char text[2048] = "";
            if (fem_build_story(&st)) simple_story_text(text, sizeof text, &st, false);
            fem_set_report_preface(text);
            if (fem_write_report(simple_report_dir, sizeof simple_report_dir) && !app.headless) platform_open_path(simple_report_dir);
            simple_screen = SIMPLE_REPORT;
            break;
        }
        default:
            if (!app.headless) platform_open_path(simple_report_dir);
            break;
        }
    }
    if (simple_screen > SIMPLE_PART && !running) {
        ui_text(ui, FONT_SMALL, x, by - 20, UI_DIM, "\xE2\x86\x90 back");
        if (ui_clickable(ui, "back", x, by - 22, 60, 18)) simple_screen--;
    }
    if (simple_screen > SIMPLE_PART) {
        const char *so = running ? "stop and start over" : "start over with another part";
        float sw = ui_text_width(ui, FONT_SMALL, so);
        ui_text(ui, FONT_SMALL, x + w - sw, by - 20, UI_DIM, so);
        if (ui_clickable(ui, "start over", x + w - sw - 4, by - 22, sw + 8, 18)) simple_restart(true);
    }
}

/* ---- what is running: every mode, top left of the view --------------------------------------------------------
 * The engine runs one job at a time. Whatever is running or waiting is listed here with its own STOP, so nobody has
 * to guess why a new simulation does not start, or hunt through the modes for the one that holds it up. */
static void activity_strip(float x, float y) {
    Ui *ui = app.ui;
    if (labapp_running() && !labapp_active() && !labapp_prepared()) {
        ui_rect(ui, x, y, 470, 42, 0xF7F6F3F2u, 8);
        char note[180]; snprintf(note, sizeof note, "Computing: %.60s", labapp_run_name());
        ui_text(ui, FONT_SMALL, x + 12, y + 7, UI_ACCENT2, note);
        ui_text(ui, FONT_SMALL, x + 12, y + 24, UI_DIM, "You can keep working. Its result will appear in Saved.");
        panel_say("%s\n", note);
        y += 50;
    }
    const FemJobInfo *jobs;
    int n = fem_active_jobs(&jobs);
    /* Simple mode already shows its own simulation in its panel */
    const FemState *fs = fem_state();
    if (app.ui_mode == UI_SIMPLE && n == 1 && !strcmp(jobs[0].id, simple_job) && fs->job_active && !strcmp(fs->job_id, simple_job)) return;
    if (n <= 0) return;
    float w = 470, row = 26, h = 26 + n * row + (n > 1 ? 30 : 6);
    ui_block_mouse(ui, x, y, w, h);
    ui_rect_grad(ui, x, y, w, h, 0xF7F6F3F2u, 0xFFFFFFF6u, 8);
    ui_rect_outline(ui, x, y, w, h, 0xB4B1AB70u, 8, 1);
    char a[220];
    snprintf(a, sizeof a, n == 1 ? "RUNNING NOW" : "RUNNING NOW, AND %d WAITING (one runs at a time)", n - 1);
    ui_text(ui, FONT_SMALL, x + 12, y + 7, UI_ACCENT, a);
    panel_say("%s\n", a);
    float ry = y + 26;
    for (int i = 0; i < n; i++, ry += row) {
        const FemJobInfo *j = &jobs[i];
        bool running = strcmp(j->state, "queued") != 0;
        ui_rect(ui, x + 12, ry + 7, 8, 8, j->stopping ? UI_ACCENT2 : running ? UI_GOOD : 0xB4B1ABFFu, 4);
        if (j->stopping) snprintf(a, sizeof a, "%.60s: stopping", fem_job_words(j));
        else if (running) snprintf(a, sizeof a, "%.60s: %.0f %%", fem_job_words(j), 100 * j->progress);
        else snprintf(a, sizeof a, "%.60s: waiting", fem_job_words(j));
        ui_text(ui, FONT_SMALL, x + 28, ry + 5, running ? UI_TEXT : UI_DIM, a);
        panel_say("%s\n", a);
        char bid[96];
        snprintf(bid, sizeof bid, "STOP##act%d", i);
        if (!j->stopping && ui_button(ui, bid, x + w - 72, ry + 1, 60, 22, false)) fem_stop_job(j->id);
    }
    if (n > 1 && ui_button(ui, "STOP EVERYTHING##act", x + 12, ry + 2, w - 24, 22, false)) {
        int k = fem_stop_all_jobs();
        LOGI("asked %d simulation%s to stop", k, k == 1 ? "" : "s");
    }
}

/* ---- the glossary under "?" ---- */

static void glossary(float W, float H) {
    Ui *ui = app.ui;
    static const char *const G[][2] = {
        {"part", "The object you want to print, from an STL file. Its size and shape are exactly those in the file."},
        {"print settings", "The machine, the material and how it is printed, as one named preset. Rest the pointer on one to see where each number comes from."},
        {"the plate", "The metal plate the part is printed on. The part is fixed to it while it is printed and cut off from it afterwards."},
        {"layer", "How thick each slice of the simulation is, in mm. The real printer lays much thinner layers; the simulation groups them."},
        {"warp", "How far the part moves out of the shape you drew, in mm, once it is printed. Upwards means away from the plate."},
        {"residual stress", "The stress printing locks into the part, in MPa (newtons per square millimetre). It is there before any load is put on the part."},
        {"the recoater", "The blade that spreads each new layer of powder. If the part rises more than one layer while it is printed, the blade can hit it."},
        {"cut", "Separating the part from the plate with a wire. The part springs back when it is cut free."},
        {"detail", "How finely the part is divided for the calculation. Finer is slower and closer to the truth."},
        {"source", "Where a number comes from: a measurement, a calibration, a datasheet, or a typical value."},
    };
    float gw = 560, gh = 70 + 10 * 46.0f, gx = (W - app.panel_w - gw) * 0.5f, gy = (H - gh) * 0.5f;
    ui_block_mouse(ui, gx, gy, gw, gh);
    ui_rect(ui, gx, gy, gw, gh, 0xF7F6F3F6u, 10);
    ui_rect_outline(ui, gx, gy, gw, gh, 0xE8551CA0u, 10, 1);
    ui_text(ui, FONT_TITLE, gx + 18, gy + 14, 0x111111FFu, "The words used here");
    if (ui_button(ui, "CLOSE##glossary", gx + gw - 90, gy + 12, 76, 24, false)) glossary_open = false;
    float y = gy + 50;
    for (size_t i = 0; i < sizeof G / sizeof G[0]; i++) {
        ui_text(ui, FONT_BOLD, gx + 18, y, UI_ACCENT, G[i][0]);
        y = say(ui, FONT_SMALL, gx + 150, y + 1, gw - 170, UI_TEXT, G[i][1], 3) + 8;
    }
}

/* =================================================================================================================
 * AGENT MODE: one question, STOP, and the window as the agent's view.
 * ================================================================================================================= */

static char agent_prompt[1024];
static char agent_command[2048];
static char agent_dir[1024];
static bool agent_settings_open;
static bool agent_said_follow;
static AgentTool agent_tools[4];
static int agent_ntools = -1;
static char agent_error[400];

/* how many lines a text takes at a width, by the same rule say() breaks it */
static int wrapped_lines(Ui *ui, int font, const char *p, float w) {
    int lines = 0;
    while (*p) {
        int n = 0, last_space = 0;
        while (p[n] && ui_text_width_n(ui, font, p, n + 1) <= w) {
            if (p[n] == ' ') last_space = n;
            n++;
        }
        if (p[n] && last_space > 0) n = last_space;
        if (n == 0) n = 1;
        p += n;
        while (*p == ' ') p++;
        lines++;
    }
    return lines ? lines : 1;
}

/* an agent line as a person reads it: an arrow for what it did, and the markdown asterisks and hashes taken off */
static void agent_display_line(const char *line, int kind, char *out, size_t cap) {
    size_t n = 0;
    if (kind == 1) n += (size_t)snprintf(out, cap, "\xE2\x86\x92 ");
    const char *p = line;
    while (*p == '#') p++;
    while (*p == ' ') p++;
    for (; *p && n + 1 < cap; p++) {
        if (p[0] == '*' && p[1] == '*') { p++; continue; }
        if (p[0] == '`') continue;
        out[n++] = *p;
    }
    out[n] = 0;
}

/* a command saved before the rename (2026-10-21) names the old variables and the old tool prefix: it is read in the new
 * words ($NAVIER_X becomes $OPENPHYSICS_X, mcp__navier-am becomes mcp__openphysics, mcp_servers.navier-am becomes
 * mcp_servers.openphysics), so that it reaches the tools the window now offers */
static void agent_command_renamed(char *cmd, size_t cap) {
    static const char *const map[][2] = {{"$NAVIER_", "$OPENPHYSICS_"}, {"mcp__navier-am", "mcp__openphysics"},
                                         {"mcp_servers.navier-am", "mcp_servers.openphysics"}};
    for (size_t m = 0; m < sizeof map / sizeof map[0]; m++) {
        char out[2048];
        size_t o = 0, lf = strlen(map[m][0]), lt = strlen(map[m][1]);
        const char *s = cmd;
        bool changed = false;
        while (*s && o + 1 < sizeof out) {
            if (!strncmp(s, map[m][0], lf) && o + lt + 1 < sizeof out) {
                memcpy(out + o, map[m][1], lt), o += lt, s += lf, changed = true;
            } else out[o++] = *s++;
        }
        out[o] = 0;
        if (changed && !*s) str_copy(cmd, cap, out);
    }
}

void hud_agent_set_command(const char *line) {
    str_copy(agent_command, sizeof agent_command, line);
    agent_command_renamed(agent_command, sizeof agent_command);
}

static void agent_load_settings(void) {
    if (agent_ntools >= 0) return;
    agent_ntools = agent_detect(agent_tools, 4);
    char v[2048];
    /* a command set from a script (agent command ...) stays; a headless run never falls back to a real AI tool on its
     * own, so a test can never spend the owner's AI usage */
    if (!agent_command[0] && !app.headless) {
        if (app_ui_state_get("agent_command", v, sizeof v)) {
            str_copy(agent_command, sizeof agent_command, v);
            agent_command_renamed(agent_command, sizeof agent_command);
        }
        else if (agent_ntools > 0) str_copy(agent_command, sizeof agent_command, agent_tools[0].command);
    }
    if (app_ui_state_get("agent_dir", v, sizeof v)) str_copy(agent_dir, sizeof agent_dir, v);
    /* the picks, "claude=MODEL|EFFORT;codex=MODEL|EFFORT" (an older single number here means nothing was picked) */
    if (app_ui_state_get("agent_effort", v, sizeof v)) {
        static const char *const tag[2] = {"claude=", "codex="};
        for (int k = 0; k < 2; k++) {
            const char *p = strstr(v, tag[k]);
            if (!p) continue;
            p += strlen(tag[k]);
            size_t m = strcspn(p, "|;");
            snprintf(agent_model_pick[k], sizeof agent_model_pick[k], "%.*s", (int)MINI(m, 63), p);
            if (p[m] == '|') {
                const char *e = p + m + 1;
                size_t el = strcspn(e, "|;");
                snprintf(agent_effort_pick[k], sizeof agent_effort_pick[k], "%.*s", (int)MINI(el, 31), e);
                if (e[el] == '|') snprintf(agent_perm_pick[k], sizeof agent_perm_pick[k], "%.*s", (int)MINI(strcspn(e + el + 1, ";"), 23), e + el + 1);
            }
        }
    }
    /* before the first question here: the runs recorded (a headless run only when its script names the runs folder) */
    if (!app_ui_state_get("agent_seen", agent_seen, sizeof agent_seen) && (!app.headless || op_getenv("AGENT_RUNS_DIR")))
        agent_seen_from_records(agent_seen, sizeof agent_seen);
}

/* ---- Ask AI: which model and effort, in the tool's own words ---- */

static int agent_kind(void) { return agent_tool_kind(agent_command); }

static struct {
    char command[2048], model[64];
    AgentChoice models[16], efforts[8], perms[6];
    int nmodels, nefforts, nperms;
    char def_model[64], def_effort[32], def_perm[24];
} AC;

static const AgentChoice *choice_find(const AgentChoice *c, int n, const char *value) {
    for (int i = 0; i < n; i++)
        if (!strcmp(c[i].value, value)) return &c[i];
    return NULL;
}

/* the lists are read from the tool's own files once, and again when the tool or (for Codex) the model changes */
static void agent_choices_refresh(void) {
    int k = agent_kind();
    const char *m = k >= 0 ? agent_model_pick[k] : "";
    if (AC.command[0] && !strcmp(AC.command, agent_command) && !strcmp(AC.model, m)) return;
    str_copy(AC.command, sizeof AC.command, agent_command[0] ? agent_command : " ");
    str_copy(AC.model, sizeof AC.model, m);
    AC.nmodels = agent_model_choices(agent_command, AC.models, 16, AC.def_model, sizeof AC.def_model);
    AC.nefforts = agent_effort_choices(agent_command, m, AC.efforts, 8, AC.def_effort, sizeof AC.def_effort);
    AC.nperms = agent_permission_choices(agent_command, AC.perms, 6, AC.def_perm, sizeof AC.def_perm);
    if (k >= 0 && agent_effort_pick[k][0] && !choice_find(AC.efforts, AC.nefforts, agent_effort_pick[k]))
        agent_effort_pick[k][0] = 0; /* a level the new model does not have */
}

static void agent_choices_save(void) {
    char v[300];
    snprintf(v, sizeof v, "claude=%s|%s|%s;codex=%s|%s|%s", agent_model_pick[0], agent_effort_pick[0], agent_perm_pick[0],
             agent_model_pick[1], agent_effort_pick[1], agent_perm_pick[1]);
    app_ui_state_set("agent_effort", v);
}

/* the model a pick resolved to last time, as "claude:opus=claude-opus-4-8;..." */
static void seen_get(const char *key, char *out, size_t cap) {
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = agent_seen; *p; p += strcspn(p, ";"), p += *p == ';') {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            snprintf(out, cap, "%.*s", (int)strcspn(p + kl + 1, ";"), p + kl + 1);
            return;
        }
    }
}

static void seen_note(void) {
    const char *id = agent_model_seen();
    char cur[96];
    if (!id[0] || !agent_run_key[0] || strpbrk(id, ";=")) return;
    seen_get(agent_run_key, cur, sizeof cur);
    if (!strcmp(cur, id)) return;
    agent_seen_put(agent_seen, sizeof agent_seen, agent_run_key, id);
    app_ui_state_set("agent_seen", agent_seen);
}

/* the model, effort and permission the next question goes with, set by name (the console's `agent model|effort|permission`;
 * the window has no menu for them, GOALS.md G18); without a value it lists the choices */
void hud_agent_choose(const char *what, const char *value) {
    int k = agent_kind();
    if (k < 0) {
        LOGW("the agent command is neither Claude Code nor Codex: it has no model, effort or permission to choose");
        return;
    }
    agent_choices_refresh();
    bool model = !strcmp(what, "model"), effort = !strcmp(what, "effort");
    const AgentChoice *list = model ? AC.models : effort ? AC.efforts : AC.perms;
    int n = model ? AC.nmodels : effort ? AC.nefforts : AC.nperms;
    char *pick = model ? agent_model_pick[k] : effort ? agent_effort_pick[k] : agent_perm_pick[k];
    size_t cap = model ? sizeof agent_model_pick[k] : effort ? sizeof agent_effort_pick[k] : sizeof agent_perm_pick[k];
    if (!value || !value[0]) {
        char all[600] = "";
        for (int i = 0; i < n; i++) snprintf(all + strlen(all), sizeof all - strlen(all), "%s%s", i ? ", " : "", list[i].value);
        LOGI("agent %s: %s (choices: %s)", what, pick[0] ? pick : "default", all[0] ? all : "none");
        return;
    }
    if (strcmp(value, "default") && !choice_find(list, n, value)) {
        LOGE("agent %s: '%s' is not one of the tool's choices", what, value);
        return;
    }
    str_copy(pick, cap, strcmp(value, "default") ? value : "");
    if (model) agent_choices_refresh(); /* the efforts can depend on the model */
    agent_choices_save();
    LOGOK("agent %s: %s", what, pick[0] ? pick : "default");
}

/* the command a question runs with: the tool's own flags for the picked model and effort (Claude Code --model and
 * --effort, Codex -m and its reasoning effort), or the voice model for a spoken one; the run key names the pick */
static void agent_command_for(bool voice, char *cmd, size_t cap, char *key, size_t kcap) {
    str_copy(cmd, cap, agent_command);
    int k = agent_kind();
    agent_choices_refresh();
    const char *pm = k >= 0 ? agent_model_pick[k] : "", *pe = k >= 0 ? agent_effort_pick[k] : "";
    if (voice && k == 0 && strcmp(hud_agent_voice_model(), "same")) pm = hud_agent_voice_model(), pe = "";
    const char *pp = k >= 0 ? (agent_perm_pick[k][0] ? agent_perm_pick[k] : AC.def_perm) : "";
    if (k == 0) {
        if (pm[0] && !strstr(agent_command, "--model")) snprintf(cmd + strlen(cmd), cap - strlen(cmd), " --model \"%s\"", pm);
        if (pe[0] && !strstr(agent_command, "--effort")) snprintf(cmd + strlen(cmd), cap - strlen(cmd), " --effort %s", pe);
        /* permissions in Claude Code's own modes; what it would ask is asked here, in the window (openphysics-mcp approve) */
        if (!strstr(agent_command, "--permission-mode") && !strstr(agent_command, "skip-permissions")) {
            if (!strcmp(pp, "bypassPermissions")) snprintf(cmd + strlen(cmd), cap - strlen(cmd), " --dangerously-skip-permissions");
            else snprintf(cmd + strlen(cmd), cap - strlen(cmd), " --permission-mode %s%s", pp[0] ? pp : "default",
                          strstr(agent_command, "--permission-prompt-tool") ? "" : " --permission-prompt-tool mcp__openphysics__approve");
        }
        if (!strstr(agent_command, "--add-dir")) /* the lab's examples and docs read without asking; last: it takes several */
            snprintf(cmd + strlen(cmd), cap - strlen(cmd),
                     " --add-dir \"$OPENPHYSICS_LAB_ROOT/examples/lab\" \"$OPENPHYSICS_LAB_ROOT/docs/lab\" \"$OPENPHYSICS_LAB_RESULTS\"");
    } else if (k == 1) { /* Codex: its sandbox preset, the lab's tools, and no refusal outside a git repository */
        char flags[700] = "";
        const char *e = pe[0] ? pe : AC.def_effort; /* a picked model goes with its own level, not config.toml's */
        if (pm[0] && !strstr(agent_command, " -m ") && !strstr(agent_command, "--model"))
            snprintf(flags, sizeof flags, " -m \"%s\"", pm);
        if ((pm[0] || pe[0]) && e[0] && !strstr(agent_command, "reasoning_effort"))
            snprintf(flags + strlen(flags), sizeof flags - strlen(flags), " -c model_reasoning_effort=%s", e);
        if (!strstr(agent_command, "--sandbox") && !strstr(agent_command, "--full-auto") && !strstr(agent_command, "dangerously"))
            snprintf(flags + strlen(flags), sizeof flags - strlen(flags), "%s",
                     !strcmp(pp, "read-only") ? " --sandbox read-only"
                     : !strcmp(pp, "full-access") ? " --dangerously-bypass-approvals-and-sandbox" : " --full-auto");
        if (!strstr(agent_command, "skip-git-repo-check")) snprintf(flags + strlen(flags), sizeof flags - strlen(flags), " --skip-git-repo-check");
        snprintf(flags + strlen(flags), sizeof flags - strlen(flags),
                 " -c \"mcp_servers.openphysics.args=[\\\"--connect\\\",\\\"$OPENPHYSICS_SOCKET\\\",\\\"--lab\\\",\\\"$OPENPHYSICS_LAB_ROOT\\\","
                 "\\\"--lab-results\\\",\\\"$OPENPHYSICS_LAB_RESULTS\\\"]\" -c mcp_servers.openphysics.tool_timeout_sec=900");
        const char *ex = strstr(agent_command, " exec");
        if (ex) snprintf(cmd, cap, "%.*s%s%s", (int)(ex - agent_command + 5), agent_command, flags, ex + 5);
        else snprintf(cmd + strlen(cmd), cap - strlen(cmd), "%s", flags);
    }
    snprintf(key, kcap, "%s:%s", k == 1 ? "codex" : "claude", pm[0] ? pm : "default");
}

/* the agent works in the open project's folder, else in the lab's folder (its edits land there, not across home) */
static void agent_workdir(char *dir, size_t cap) {
    const FemState *s = fem_state();
    str_copy(dir, cap, agent_dir[0] ? agent_dir : s->project_dir[0] ? s->project_dir : labapp_results_dir());
    path_mkdirs(dir); /* with its parents: on a first run ~/OpenPhysicsAI-Projects does not exist yet */
}

/* spoken questions run with a fast model, so that a conversation keeps its pace (Haiku 4.5 answers in about 2 s where
 * the default model takes 9 to 15 s); "agent_voice_model" in the settings, or $OPENPHYSICS_VOICE_MODEL, chooses another, and
 * "same" keeps the model picked for typed questions */
static bool agent_voice;
const char *hud_agent_voice_model(void) {
    static char m[64];
    const char *env = op_getenv("VOICE_MODEL");
    if (env && env[0]) str_copy(m, sizeof m, env);
    else if (!app_ui_state_get("agent_voice_model", m, sizeof m) || !m[0]) str_copy(m, sizeof m, "haiku");
    return m;
}

static void agent_ask(void) {
    agent_load_settings();
    agent_error[0] = 0;
    if (!agent_prompt[0]) {
        str_copy(agent_error, sizeof agent_error, "Write what you want to know first.");
        return;
    }
    const FemState *s = fem_state();
    char dir[1024];
    agent_workdir(dir, sizeof dir);
    /* the user's words, and one sentence saying what the window has open, so the agent starts where the user is */
    char prompt[6144], labctx[1600];
    { /* the lab: what is open, and how to make or change a simulation so that the window shows it; the shown result is
       * named by its file, so that a change takes one lab_change call */
        char stem[128] = "", named[300] = "";
        if (labapp_active()) {
            const char *p = labapp_path(), *b = strrchr(p, '/');
            str_copy(stem, sizeof stem, b ? b + 1 : p);
            char *dot = strrchr(stem, '.');
            if (dot) *dot = 0;
            if (stem[0]) snprintf(named, sizeof named, " (result '%s': lab_change with base '%s' changes its values in one call)", stem, stem);
        }
        snprintf(labctx, sizeof labctx,
                 " The physics lab: %s%s%s%s. To make or change a simulation use the openphysics lab tools, which need no "
                 "permission: lab_change changes values of a result or scenario by their paths and computes it, lab_scenarios "
                 "lists the scenarios, lab_scenario_get gives one with the documentation of its keys, lab_run saves your version "
                 "under a new name and computes it (start coarse and short), and the window opens the result by itself.",
                 labapp_active() ? "the window shows '" : labapp_prepared() ? "the selected scenario is '" : "nothing is open in it",
                 labapp_active() ? labapp_title() : labapp_prepared() ? labapp_scenario_path() : "",
                 labapp_active() || labapp_prepared() ? "'" : "", named);
    }
    char pointed[700] = ""; /* the faces the person clicked on since the last question (G17 step 6) */
    if (s->have_project && s->nbodies) {
        int ids[64], n = fem_picked_patches(ids, 64);
        if (n > 0) {
            const double *nm = fem_picked_normal();
            char list[400] = "";
            for (int i = 0; i < n; i++) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%d", i ? ", " : "", ids[i]);
            snprintf(pointed, sizeof pointed,
                     " The person pointed at %d face%s of the part as they asked: surface patches %s, %.0f mm2 in all, facing (%.2f, "
                     "%.2f, %.2f) in the build frame; selection_create with {\"patches\": [%s]} names them.",
                     n, n == 1 ? "" : "s", list, fem_picked_area_mm2(), nm[0], nm[1], nm[2], list);
            fem_clear_pick(); /* the next question points afresh */
        }
    }
    if (s->have_project && s->nbodies)
        snprintf(prompt, sizeof prompt,
                 "%s\n\n(The OpenPhysicsAI window has project '%s' open, with part '%s'. Use the openphysics tools.%s For a strength "
                 "question in the person's words, make the supports and loads of what they say, call setup_confirm with them in "
                 "plain words, and run only when they confirm.%s)",
                 agent_prompt, s->project, s->body, pointed, labctx);
    else
        snprintf(prompt, sizeof prompt, "%s\n\n(Use the openphysics tools for parts; no project is open yet in the window.%s)", agent_prompt, labctx);
    char cmd[2400], run_key[96];
    agent_command_for(agent_voice, cmd, sizeof cmd, run_key, sizeof run_key);
    agent_voice = false;
    model_menu_open = false;
    if (!agent_start(cmd, dir, prompt, agent_error, sizeof agent_error)) {
        LOGW("agent: %s", agent_error); /* the log says why, as the window does */
        return;
    }
    str_copy(agent_run_key, sizeof agent_run_key, run_key);
    agent_prompt[0] = 0; /* the question now lives in the conversation */
    chat_scroll = 0;     /* and the conversation shows its newest end */
    app_ui_state_set("agent_command", agent_command);
    if (agent_dir[0]) app_ui_state_set("agent_dir", agent_dir);
}

void hud_agent_ask_text(const char *text) {
    str_copy(agent_prompt, sizeof agent_prompt, text);
    agent_ask();
}

/* the conversation's process started before the first spoken question (the headset is connected), so the question
 * waits only for the model, not for Claude Code and its tools to start; nothing is asked and nothing is spent */
void hud_agent_warm(void) {
    agent_load_settings();
    if (agent_kind() != 0) return;
    char cmd[2400], key[96], dir[1024], err[256];
    agent_command_for(true, cmd, sizeof cmd, key, sizeof key);
    agent_workdir(dir, sizeof dir);
    agent_start(cmd, dir, NULL, err, sizeof err);
}

void hud_agent_ask_voice(const char *text) {
    agent_voice = true;
    hud_agent_ask_text(text);
    agent_voice = false;
}

static void agent_panel(float px, float py, float pw, float ph, double time) {
    Ui *ui = app.ui;
    (void)time;
    agent_load_settings();
    agent_poll();
    /* a result the agent produced is shown as it arrives: framed, at its last stored time */
    {
        static char shown_job[64];
        const FemState *fs = fem_state();
        if (fs->have_result && strcmp(shown_job, fs->result_job) != 0) {
            str_copy(shown_job, sizeof shown_job, fs->result_job);
            exec_cmd("fem step last");
            exec_cmd("fem fit");
        }
    }
    ui_block_mouse(ui, px, py, pw, ph);
    ui_rect_grad(ui, px, py, pw, ph, 0xF7F6F3F2u, 0xFFFFFFF6u, 10);
    ui_rect_outline(ui, px, py, pw, ph, 0xB4B1AB70u, 10, 1);
    float x = px + 18, w = pw - 36, y = py + 16;
    panel_text[0] = 0, panel_text_len = 0;
    if (!agent_said_follow) {
        agent_said_follow = true;
        LOGI("agent mode: every job the agent starts is followed in this window automatically");
    }
    labapp_follow(app.dt); /* results the agent writes into the lab's folder open here */
    y = say(ui, FONT_TITLE, x, y, w, 0x111111FFu, "Ask the lab", 1) + 10;
    if (agent_ntools == 0 && !agent_command[0]) {
        y = say(ui, FONT_BOLD, x, y, w, UI_ACCENT2,
                "No AI tool is installed on this computer. Agent mode uses one you already have: install Claude Code "
                "(claude.com/claude-code) or Codex, sign in once in a terminal, and come back.", 6) + 8;
    }
    y = text_field(ui, "text:agent", x, y, w, 3, "What do you want to simulate, change or know?", agent_prompt, sizeof agent_prompt, agent_ask) + 8;
    bool running = agent_running();
    const float gap = 6;
    float hw = (w - gap) / 2;
    if (step_button(ui, running ? "WORKING...##ask" : "ASK##ask", x, y, hw, 30, running, running ? "the agent is working" : NULL))
        agent_ask();
    if (step_button(ui, "STOP##agent", x + hw + gap, y, hw, 30, false, running ? NULL : "nothing is running"))
        agent_stop();
    y += 38;
    if (agent_error[0]) y = say(ui, FONT_SMALL, x, y, w, UI_BAD, agent_error, 4) + 6;
    y = say(ui, FONT_SMALL, x, y, w, UI_FAINT, "Every job the agent starts is followed in the view. The window never holds a key to any AI service.", 2) + 8;

    /* the transcript: the agent's words, what it changed, and what the window did */
    float ty = y, th = py + ph - 120 - 118 - ty; /* room below for the recent results */
    ui_rect(ui, x - 4, ty, w + 8, th, 0xFFFFFFE0u, 6);
    ui_push_clip(ui, x - 4, ty, w + 8, th);
    float lh = ui_line_height(ui, FONT_SMALL) + 1;
    int n = agent_line_count();
    /* newest at the bottom: walk back from the last line until the box is full, counting wrapped lines */
    int first = n;
    float used = 12;
    while (first > 0) {
        int kind;
        char shown[420];
        agent_display_line(agent_line_at(first - 1, &kind), kind, shown, sizeof shown);
        float need = MINI(wrapped_lines(ui, FONT_SMALL, shown, w - 8), 3) * lh;
        if (used + need > th) break;
        used += need;
        first--;
    }
    float yy = ty + 6;
    for (int i = first; i < n; i++) {
        int kind;
        char shown[420];
        agent_display_line(agent_line_at(i, &kind), kind, shown, sizeof shown);
        uint32_t col = kind == 1 ? UI_ACCENT : kind == 2 ? UI_DIM : kind == 3 ? UI_BAD : UI_TEXT;
        yy = say(ui, FONT_SMALL, x + 4, yy, w - 8, col, shown, 3);
    }
    if (!n) say(ui, FONT_SMALL, x + 4, yy, w - 8, UI_FAINT, "What the agent says and does will appear here.", 2);
    ui_pop_clip(ui);

    /* the lab's recent results, newest first: one click opens one */
    {
        static char rp[5][1024], rt[5][120];
        static int rn;
        static double last;
        if (time - last > 2.0 || last == 0) rn = labapp_recent(rp, rt, 5), last = time;
        float ry = ty + th + 10;
        section(ui, x, ry, w, "RECENT RESULTS", NULL);
        ry += 20;
        if (!rn) say(ui, FONT_SMALL, x, ry, w, UI_FAINT, "none yet: ask for a simulation", 1);
        for (int i = 0; i < rn; i++) {
            bool hov = ui_hover(ui, x, ry - 1, w, 17);
            if (hov) ui_rect(ui, x - 2, ry - 1, w + 4, 17, 0xDDDAD4A0u, 4);
            char shown[140];
            snprintf(shown, sizeof shown, "%s", rt[i]);
            while (ui_text_width(ui, FONT_SMALL, shown) > w - 8 && strlen(shown) > 4) shown[strlen(shown) - 4] = 0, strcat(shown, "...");
            ui_text(ui, FONT_SMALL, x + 4, ry, hov ? UI_TEXT : UI_DIM, shown);
            char id[64], cmd[1100];
            snprintf(id, sizeof id, "recent %d", i);
            if (ui_clickable(ui, id, x, ry - 1, w, 17)) snprintf(cmd, sizeof cmd, "lab open \"%s\"", rp[i]), exec_cmd(cmd);
            ry += 17;
        }
    }

    /* which tool, and where it works */
    float sy = py + ph - 108;
    if (ui_button(ui, agent_settings_open ? "\xE2\x96\xBE  SETTINGS##agent" : "\xE2\x96\xB8  SETTINGS##agent", x, sy, 120, 24, agent_settings_open))
        agent_settings_open = !agent_settings_open;
    char which[160];
    snprintf(which, sizeof which, "%s", agent_command[0] ? (strstr(agent_command, "claude") ? "Claude Code" : strstr(agent_command, "codex") ? "Codex" : "your command") : "no tool chosen");
    ui_text(ui, FONT_SMALL, x + 128, sy + 5, UI_DIM, which);
    if (agent_settings_open) {
        /* drawn over the transcript, since this is the moment it matters */
        float oy = ty, oh = th + 70;
        ui_rect(ui, x - 4, oy, w + 8, oh, 0xF7F6F3F8u, 6);
        ui_rect_outline(ui, x - 4, oy, w + 8, oh, 0xE8551C60u, 6, 1);
        float yy2 = oy + 8;
        yy2 = say(ui, FONT_BOLD, x + 4, yy2, w - 8, UI_TEXT, "Found on this computer", 1) + 4;
        if (agent_ntools == 0) yy2 = say(ui, FONT_SMALL, x + 4, yy2, w - 8, UI_ACCENT2, "none: no claude and no codex command on the path", 2) + 4;
        for (int i = 0; i < agent_ntools; i++) {
            char id[64];
            snprintf(id, sizeof id, "%s##tool", agent_tools[i].name);
            if (ui_button(ui, id, x + 4, yy2, 140, 24, !strcmp(agent_command, agent_tools[i].command)))
                str_copy(agent_command, sizeof agent_command, agent_tools[i].command);
            ui_text(ui, FONT_SMALL, x + 152, yy2 + 5, UI_FAINT, agent_tools[i].path);
            yy2 += 30;
        }
        yy2 = say(ui, FONT_SMALL, x + 4, yy2 + 2, w - 8, UI_DIM, "The command line ($OPENPHYSICS_PROMPT is your question, $OPENPHYSICS_MCP_CONFIG the tool connection):", 2) + 4;
        yy2 = text_field(ui, "text:agentcmd", x + 4, yy2, w - 8, 4, "any command", agent_command, sizeof agent_command, NULL) + 8;
        yy2 = say(ui, FONT_SMALL, x + 4, yy2, w - 8, UI_DIM, "Working folder (empty: the project's folder):", 1) + 4;
        text_field(ui, "text:agentdir", x + 4, yy2, w - 8, 1, "the project's folder", agent_dir, sizeof agent_dir, NULL);
    }
}

/* ---- the panel ---------------------------------------------------------------------------------------------- */

bool hud_pick_mode(void) {
    if (lab_library_open || labapp_active() || labapp_prepared()) return false;
    if (app.ui_mode == UI_SIMPLE) return app.hud_on && hud_simple_face_mode();
    /* Agentic (GOALS.md G17 step 6): with a part open and no result shown, a click points at a face for the AI */
    if (app.ui_mode == UI_AGENT) return app.workspace == WS_SOLID && fem_state()->have_project && fem_state()->nbodies > 0 && !fem_state()->have_result;
    if (app.ui_mode != UI_ADVANCED) return false;
    return app.workspace == WS_SOLID && app.hud_on && solid_step == STEP_HOLD && !solid_build_path;
}
bool hud_box_mode(void) { return solid_box_pick && hud_pick_mode(); }
bool hud_box_front_only(void) { return solid_box_front; }

static void analysis_panel(float px, float py, float pw, float ph, double time) {
    Ui *ui = app.ui;
    const FemState *s = fem_state();
    /* A reopened or agent-produced result selects its own workflow once, while later manual choices stay put. */
    static char adopted_job[64];
    if (!s->have_result) adopted_job[0] = 0;
    else if (strcmp(adopted_job, s->result_job)) {
        str_copy(adopted_job, sizeof adopted_job, s->result_job);
        bool metal = !strcmp(s->result_kind, "lpbf_build"), plastic = !strcmp(s->result_kind, "fff_print");
        solid_build_path = metal || plastic;
        if (solid_build_path) build_kind = plastic ? 1 : 0;
        else solid_analysis = !strcmp(s->result_kind, "transient_thermal") ? 1 : !strcmp(s->result_kind, "thermomechanical") ? 2 : 0;
        solid_step = STEP_RESULTS;
        solid_step_pinned = true;
    }
    ui_block_mouse(ui, px, py, pw, ph);
    ui_rect_grad(ui, px, py, pw, ph, 0xF7F6F3EEu, 0xFFFFFFF2u, 10);
    ui_rect_outline(ui, px, py, pw, ph, 0xB4B1AB70u, 10, 1);
    float x = px + 14, w = pw - 28, y = py + 12;
    char a[220];

    panel_text[0] = 0, panel_text_len = 0;
    StepInfo info[STEP_COUNT];
    solid_steps(info);
    int first = solid_first_open(info);
    if (!solid_step_pinned) solid_step = first;
    solid_step = CLAMP(solid_step, 0, STEP_COUNT - 1);

    /* header: what the analysis is doing, not what the flow is doing */
    const char *state = !s->have_engine    ? "NO ENGINE"
                        : !s->have_project ? "NO PROJECT"
                        : s->job_active    ? "SOLVING"
                        : s->have_result   ? "RESULTS"
                                           : "SETUP";
    uint32_t led = !s->have_engine ? UI_BAD : s->job_active ? UI_ACCENT2 : s->have_result ? UI_GOOD : UI_DIM;
    float pulse = s->job_active ? 0.6f + 0.4f * (float)sin(time * 5.0) : 1.0f;
    ui_rect(ui, x, y + 4, 9, 9, (led & 0xFFFFFF00u) | (uint32_t)(255 * pulse), 4.5f);
    ui_text(ui, FONT_BOLD, x + 16, y, led, state);
    snprintf(a, sizeof a, "%s", s->have_project ? s->project : "OpenPhysicsAI");
    ui_text_right(ui, FONT_SMALL, x + w, y + 1, UI_DIM, a);
    y += 22;

    /* the strip: six steps, two rows, each showing where it stands */
    const char *CHIP[STEP_COUNT] = {"1 PART", "2 MESH", "3 MATERIAL",
                                    solid_build_path ? (build_kind == 1 ? "4 PROCESS" : "4 BUILD") : "4 HOLD & LOAD",
                                    solid_build_path ? (build_kind == 1 ? "5 PRINT" : "5 RUN") : "5 SOLVE", "6 RESULTS"};
    const float cgap = 4, cw = (w - 2 * cgap) / 3, chh = 34;
    for (int i = 0; i < STEP_COUNT; i++) {
        float cx = x + (i % 3) * (cw + cgap), cy = y + (i / 3) * (chh + cgap);
        bool open = i == solid_step;
        uint32_t dot = info[i].state == ST_DONE ? UI_GOOD : info[i].state == ST_ATTENTION ? UI_ACCENT2 : 0xB4B1ABFFu;
        ui_rect(ui, cx, cy, cw, chh, open ? 0xDDDAD4F0u : 0xF7F6F3E8u, 5);
        ui_rect_outline(ui, cx, cy, cw, chh, open ? 0xE8551CC0u : 0xDDDAD4FFu, 5, 1);
        ui_rect(ui, cx + 7, cy + 7, 7, 7, dot, 3.5f);
        ui_push_clip(ui, cx + 2, cy, cw - 4, chh);
        ui_text(ui, FONT_SMALL, cx + 19, cy + 3, open ? 0x111111FFu : UI_TEXT, CHIP[i]);
        ui_text(ui, FONT_SMALL, cx + 8, cy + 18, UI_DIM, info[i].line);
        ui_pop_clip(ui);
        ui_block_mouse(ui, cx, cy, cw, chh);
        if (ui_clickable(ui, CHIP[i], cx, cy, cw, chh)) {
            solid_step = i;
            solid_step_pinned = true;
        }
    }
    y += 2 * chh + cgap + 8;

    /* one line telling the user what to do now */
    y = wrap_text(ui, x, y, w, UI_ACCENT, solid_hint(info, solid_step), 2) + 6;

    float step_y = y, bottom = py + ph - (manual_terminal_open || console_focused() ? 240 : 72);
    static int previous_step = -1;
    if (previous_step != solid_step) { inspector_scroll = 0; previous_step = solid_step; }
    inspector_begin(ui, x - 4, step_y, w + 8, bottom);
    y -= inspector_scroll;
    if (solid_step == STEP_PART) y = step_part(ui, x, y, w);
    else if (solid_step == STEP_MESH) y = step_mesh(ui, x, y, w);
    else if (solid_step == STEP_MATERIAL) y = step_material(ui, x, y, w);
    else if (solid_step == STEP_HOLD) y = solid_build_path ? step_build(ui, x, y, w) : step_hold(ui, x, y, w);
    else if (solid_step == STEP_SOLVE) y = step_solve(ui, x, y, w);
    else y = step_results(ui, x, y, w);
    if (fem_last_error()[0]) { /* a refusal from the engine belongs next to the control that caused it */
        y += 2;
        y = wrap_text(ui, x, y, w, UI_BAD, fem_last_error(), 3) + 2;
    }
    inspector_end(ui, x - 4, step_y, w + 8, bottom, y + 8);
    y = bottom + 4;

    if (s->pending_job[0]) {
        snprintf(a, sizeof a, "an agent started %s %.8s", s->pending_kind, s->pending_job);
        ui_text(ui, FONT_SMALL, x, y + 4, UI_DIM, a);
        if (ui_button(ui, "FOLLOW", x + w - 60, y, 60, 22, false)) exec_cmd("fem follow");
        y += 26;
    }
    y += 2;

    manual_terminal(ui, x, y, w, py + ph - 6, time);
}

/* ---- the lab in Advanced mode: a minimal panel to play with the world by hand ------------------------------------ */

typedef struct LibEntry {
    char path[1024], title[120], domain[16], base[96];
    char thumb_path[1024]; /* docs/media/thumbs/<base>.jpg: the home screen shows a scenario only when it has one */
    bool has_thumb;
    bool project; /* a computed part (the finite-element engine: printing, heat, stress) rather than a lab scenario */
    GLuint thumb;
} LibEntry;
static LibEntry lib[96];
static int lib_n = -1;

/* the scenarios shipped in examples/lab, read once: their titles and domains */
static void lib_load(void) {
    lib_n = 0;
    char dir[1024];
    if (!app_resource_path("examples/lab", dir, sizeof dir)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && lib_n < (int)(sizeof lib / sizeof lib[0])) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".json")) continue;
        LibEntry *L = &lib[lib_n];
        snprintf(L->path, sizeof L->path, "%s/%s", dir, e->d_name);
        JsonValue *j = json_read_file(L->path, 16u << 20, NULL);
        if (!j) continue;
        snprintf(L->title, sizeof L->title, "%s", json_get_str(j, "title", e->d_name));
        snprintf(L->domain, sizeof L->domain, "%s", json_get_str(j, "domain", ""));
        snprintf(L->base, sizeof L->base, "%.*s", (int)(n - 5), e->d_name);
        char rel[160];
        snprintf(rel, sizeof rel, "docs/media/thumbs/%s.jpg", L->base);
        L->has_thumb = app_resource_path(rel, L->thumb_path, sizeof L->thumb_path);
        L->thumb = 0;
        json_free(j);
        lib_n++;
    }
    closedir(d);
    { /* the computed parts in the projects folder that have a picture: the 3D-printing and finite-element showcase */
        static char pp[64][1024], pt[64][120];
        int pn = fem_recent_projects(pp, pt, 64);
        for (int i = 0; i < pn && lib_n < (int)(sizeof lib / sizeof lib[0]); i++) {
            const char *b = strrchr(pp[i], '/') ? strrchr(pp[i], '/') + 1 : pp[i];
            char rel[200];
            snprintf(rel, sizeof rel, "docs/media/thumbs/%s.jpg", b);
            LibEntry *L = &lib[lib_n];
            if (!app_resource_path(rel, L->thumb_path, sizeof L->thumb_path)) continue;
            str_copy(L->path, sizeof L->path, pp[i]);
            str_copy(L->base, sizeof L->base, b);
            str_copy(L->title, sizeof L->title, pt[i][0] ? pt[i] : b);
            for (char *c = L->title; *c; c++) if (*c == '_') *c = ' ';
            str_copy(L->domain, sizeof L->domain, "parts");
            L->has_thumb = L->project = true;
            L->thumb = 0;
            lib_n++;
        }
    }
    for (int i = 1; i < lib_n; i++) /* by domain, then by title */
        for (int k = i; k > 0; k--) {
            int c = strcmp(lib[k - 1].domain, lib[k].domain);
            if (c < 0 || (c == 0 && strcmp(lib[k - 1].title, lib[k].title) <= 0)) break;
            LibEntry t = lib[k];
            lib[k] = lib[k - 1], lib[k - 1] = t;
        }
}

/* the domains as a visitor would name them */
static const char *lib_domain_name(const char *d) {
    static const char *const N[][2] = {{"acoustic", "sound in rooms"}, {"em", "radar and light"}, {"flow", "flow past bodies"},
                                       {"gas", "shocks and hypersonic flow"}, {"heat", "heat and air"}, {"impact", "impact of metals"},
                                       {"magnet", "motors and magnets"}, {"orbit", "orbits"}, {"relativity", "black holes"},
                                       {"water", "water"}, {"fracture", "glass and fracture"}, {"sheet", "rubber and cloth"},
                                       {"melt", "metal melting"}, {"fire", "fire, rooms and ventilation"}, {"battery", "batteries"},
                                       {"parts", "3D-printed parts: heat, stress and distortion"}};
    for (size_t k = 0; k < sizeof N / sizeof N[0]; k++)
        if (!strcmp(N[k][0], d)) return N[k][1];
    return d;
}

/* One browser covers both engines and saved work. Browsing never launches a solver. */
static void lib_open(const LibEntry *L) {
    if (L->project) { /* a computed part opens at its stored result, as Saved opens it */
        if (fem_open_project(L->path)) {
            labapp_close();
            app_set_workspace(WS_SOLID);
            fem_follow_latest_result();
            solid_step_pinned = false;
            exec_cmd("fem edges off"), exec_cmd("fem outline off"); /* as its picture in the gallery shows it */
            exec_cmd("fem fit"); /* the part fills the view, not the build volume */
        }
        lab_library_open = false;
        inspector_scroll = 0;
        solid_field_id[0] = 0;
        return;
    }
    labapp_select(L->path);
    lab_library_open = false;
    inspector_scroll = 0;
    solid_field_id[0] = 0;
}

static bool browser_matches(const char *a, const char *b, const char *c) {
    if (!browser_search[0]) return true;
    char hay[512], needle[128];
    snprintf(hay, sizeof hay, "%s %s %s", a, b, c);
    snprintf(needle, sizeof needle, "%s", browser_search);
    for (char *p = hay; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (char *p = needle; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strstr(hay, needle) != NULL;
}

static bool browser_card(Ui *ui, const char *id, const char *title, const char *detail,
                         float x, float y, float w, float h, uint32_t accent) {
    bool hover = ui_hover(ui, x, y, w, h);
    ui_rect(ui, x, y, w, h, hover ? 0xDDDAD4FFu : 0xEFEDE9FFu, 8);
    ui_rect_outline(ui, x, y, w, h, hover ? accent : 0xDDDAD4FFu, 8, 1);
    ui_rect(ui, x + 12, y + 15, 3, h - 30, accent, 1);
    ui_push_clip(ui, x + 20, y + 6, w - 36, h - 12);
    say(ui, FONT_BOLD, x + 24, y + 12, w - 48, UI_TEXT, title, 2);
    ui_text(ui, FONT_SMALL, x + 24, y + h - 22, UI_DIM, detail);
    ui_pop_clip(ui);
    panel_say("%s | %s\n", title, detail);
    return ui_clickable(ui, id, x, y, w, h);
}

static void browser_workflow(int n) {
    labapp_close();
    lab_library_open = false;
    solid_field_id[0] = 0;
    inspector_scroll = 0;
    if (n == 0) { app_set_workspace(WS_FLUID); return; }
    app_set_workspace(WS_SOLID);
    solid_build_path = n >= 3;
    build_kind = n == 4;
    solid_analysis = n == 2 ? 1 : 0;
    solid_step = STEP_PART;
    solid_step_pinned = true;
}

/* the domains in a word or two, for the filter pills */
static const char *lib_domain_short(const char *d) {
    static const char *const N[][2] = {{"acoustic", "Sound"}, {"em", "Radar"}, {"flow", "Flow"}, {"gas", "Shocks"}, {"heat", "Heat"},
                                       {"impact", "Impact"}, {"magnet", "Motors"}, {"orbit", "Orbits"}, {"relativity", "Black holes"},
                                       {"water", "Water"}, {"fracture", "Fracture"}, {"sheet", "Cloth"}, {"melt", "Melting"},
                                       {"fire", "Fire and air"}, {"battery", "Batteries"}, {"fsi", "Heart"}, {"parts", "3D printing"}};
    for (size_t k = 0; k < sizeof N / sizeof N[0]; k++)
        if (!strcmp(N[k][0], d)) return N[k][1];
    return d;
}

/* a scenario's title is a sentence ("Shoebox concert hall, 30 x 16 x 12 m: a pulse from the stage"); a card shows its
 * name, the part before the first colon, or before the first comma when that is still long */
static const char *lib_short_title(const LibEntry *L, char *buf, size_t cap) {
    str_copy(buf, cap, L->title);
    char *c = strchr(buf, ':');
    if (c) *c = 0;
    if (strlen(buf) > 34 && (c = strchr(buf, ','))) *c = 0;
    return buf;
}

static GLuint lib_thumb(LibEntry *L) {
    if (L->thumb || !L->has_thumb) return L->thumb;
    int w = 0, h = 0;
    uint8_t *px = image_read(L->thumb_path, &w, &h);
    if (!px) { L->has_thumb = false; return 0; }
    L->thumb = gl_texture_2d(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, px, true);
    free(px);
    return L->thumb;
}

/* a picture card: the computed scene on top, its title and its physics below; on hover it lifts, the picture moves a
 * little closer, and an accent ring marks it. ih is the picture's height; the card is ih + 52 tall. */
static bool gallery_card(Ui *ui, const char *id, LibEntry *L, float x, float y, float w, float ih, bool subtitle) {
    float h = ih + (subtitle ? 52 : 34);
    bool hover = ui_hover(ui, x, y, w, h);
    ui_shadow(ui, x, y + (hover ? 10 : 5), w, ih, 16, hover ? 20 : 12, hover ? 0x0000002Eu : 0x00000018u);
    GLuint t = lib_thumb(L);
    float crop = 0.5f * (1.0f - (ih / w) / 0.75f), z = hover ? 0.03f : 0.0f; /* thumbs are 4:3; cards are wider */
    if (t) ui_image(ui, t, x, y, w, ih, z, crop + z, 1 - z, 1 - crop - z, 16, 0xFFFFFFFFu);
    else ui_rect(ui, x, y, w, ih, 0xE7E5E0FFu, 16);
    if (hover) ui_rect_outline(ui, x, y, w, ih, UI_ACCENT, 16, 2);
    ui_push_clip(ui, x, y + ih, w, 52);
    char st[124];
    lib_short_title(L, st, sizeof st);
    if (ui_text_width(ui, FONT_BOLD, st) > w - 4) { /* a long name ends on a whole word and an ellipsis */
        size_t n = strlen(st);
        while (n > 4 && ui_text_width_n(ui, FONT_BOLD, st, (int)n) > w - 22) n--;
        while (n > 4 && st[n] != ' ') n--;
        snprintf(st + n, sizeof st - n, "\xE2\x80\xA6");
    }
    ui_text(ui, FONT_BOLD, x + 2, y + ih + 12, UI_TEXT, st);
    if (subtitle) ui_text(ui, FONT_SMALL, x + 2, y + ih + 31, UI_DIM, lib_domain_name(L->domain));
    ui_pop_clip(ui);
    panel_say("%s | %s\n", L->title, lib_domain_name(L->domain));
    return ui_clickable(ui, id, x, y, w, h);
}

/* a small white pill with a hairline, or black when chosen; its widget id is `id` itself (tests click "libdom heat") */
static bool pill(Ui *ui, const char *id, const char *label, float x, float y, bool on) {
    float w = ui_text_width(ui, FONT_SMALL, label) + 26;
    bool hov = ui_hover(ui, x, y, w, 28);
    ui_rect(ui, x, y, w, 28, on ? UI_INK : hov ? 0xF2F1EEFFu : 0xFFFFFFFFu, 14);
    if (!on) ui_rect_outline(ui, x, y, w, 28, hov ? 0xC9C6C0FFu : 0xDDDAD4FFu, 14, 1);
    ui_text(ui, FONT_SMALL, x + 13, y + 6, on ? 0xFFFFFFFFu : UI_TEXT, label);
    return ui_clickable(ui, id, x, y, w, 28);
}

static float pill_width(Ui *ui, const char *label) { return ui_text_width(ui, FONT_SMALL, label) + 26; }

/* a question asked from the gallery: the gallery steps aside so the answer is seen arriving */
static void home_ask(void) {
    if (!agent_prompt[0]) return;
    agent_ask();
    lab_library_open = false;
}

/* The home screen: what the lab can do, shown by computed pictures. A scenario appears when it has a picture in
 * docs/media/thumbs (tools/showcase/gallery_thumbs.py); one without is still found by typing its file name. */
static void simulation_browser(float W, float H) {
    Ui *ui = app.ui;
    if (lib_n < 0) lib_load();
    panel_text[0] = 0, panel_text_len = 0;
    ui_block_mouse(ui, 0, 30, W, H - 30);
    ui_rect(ui, 0, 30, W, H - 30, UI_PAGE, 0);
    const float margin = W < 1200 ? 28 : 44;
    float x = margin, w = W - 2 * margin;
    double now = now_seconds();

    /* hero: the headline on the left, a featured simulation on the right */
    float heroW = MAXI(w * 0.50f, 420), heroX = W - margin - heroW, heroH = MINI(heroW * 0.46f, MAXI(H * 0.31f, 220)), heroY = 48;
    float tx = x, ty = heroY + 4, tw = heroX - x - 32;
    ui_text(ui, FONT_HERO, tx, ty, UI_TEXT, "Explore");
    ui_text(ui, FONT_HERO, tx, ty + 56, UI_TEXT, "the physics.");
    say(ui, FONT_DISPLAY, tx, ty + 122, tw, 0x9A9A9FFFu, "Real solvers, on this computer.", 2);
    if (expert_view())
        say(ui, FONT_SMALL, tx, ty + 168, MINI(tw, 460), UI_DIM,
            "Choose a simulation, change its inputs, run it and look inside the result. Or start from a part of your own.", 3);
    else if (app.ui_mode == UI_AGENT) { /* Ask AI starts here: the question goes straight from the gallery */
        float fw = MINI(tw, 520);
        ui_shadow(ui, tx, ty + 180, fw, 34, 17, 12, 0x0000001Eu);
        text_field(ui, "text:agent", tx, ty + 176, fw - 46, 1, "Ask for a simulation", agent_prompt, sizeof agent_prompt, home_ask);
        if (round_action(ui, "ASK##home", tx + fw - 18, ty + 191, 17, "\xE2\x86\x91", false)) home_ask();
    }

    int visible[96], nv = 0;
    for (int i = 0; i < lib_n; i++)
        if (lib[i].has_thumb) visible[nv++] = i;
    static const char *featured[] = {"Helical_Lantern", "glass_bottle", "Thermal_Corolla", "black_hole", "heart_valve",
                                     "Helical_Flute", "flag_3d", "pm_motor_3d", "breaking_wave"};
    int feat[10], nf = 0;
    for (size_t k = 0; k < sizeof featured / sizeof featured[0]; k++)
        for (int i = 0; i < lib_n; i++)
            if (lib[i].has_thumb && !strcmp(lib[i].base, featured[k]) && nf < 10) feat[nf++] = i;
    if (!nf && nv) feat[nf++] = visible[0];
    if (nf) {
        double period = 7.0, ph = now / period;
        int cur = (int)ph % nf, nxt = (cur + 1) % nf;
        float fade = (float)CLAMP((ph - floor(ph) - 0.85) / 0.15, 0.0, 1.0);
        LibEntry *A = &lib[feat[cur]], *B = &lib[feat[nxt]];
        float crop = 0.5f * (1.0f - (heroH / heroW) / 0.75f);
        ui_shadow(ui, heroX, heroY + 12, heroW, heroH, 22, 26, 0x00000026u);
        GLuint ta = lib_thumb(A), tb = lib_thumb(B);
        if (ta) ui_image(ui, ta, heroX, heroY, heroW, heroH, 0, crop, 1, 1 - crop, 22, 0xFFFFFFFFu);
        if (tb && fade > 0) ui_image(ui, tb, heroX, heroY, heroW, heroH, 0, crop, 1, 1 - crop, 22, 0xFFFFFF00u | (uint32_t)(255 * fade));
        LibEntry *F = fade > 0.5f ? B : A;
        char st[120];
        lib_short_title(F, st, sizeof st);
        float lw = ui_text_width(ui, FONT_BOLD, st) + 150;
        ui_rect(ui, heroX + 16, heroY + heroH - 56, lw, 40, 0xFFFFFFE8u, 20);
        ui_text(ui, FONT_BOLD, heroX + 34, heroY + heroH - 44, UI_TEXT, st);
        char fid[128];
        if (expert_view()) {
            snprintf(fid, sizeof fid, "Open##hero %s", F->base);
            if (ui_button(ui, fid, heroX + 16 + lw - 92, heroY + heroH - 52, 84, 32, true)) lib_open(F);
        } else {
            ui_text(ui, FONT_BOLD, heroX + 16 + lw - 74, heroY + heroH - 44, UI_ACCENT, "Open \xE2\x86\x92");
            snprintf(fid, sizeof fid, "hero %s", F->base);
            if (ui_clickable(ui, fid, heroX, heroY, heroW, heroH)) lib_open(F);
        }
    }

    /* start from your own part */
    float wy = ty + (!expert_view() && app.ui_mode == UI_AGENT ? 236 : 214);
    static bool own_open;
    bool show_own = expert_view() || own_open;
    if (expert_view()) ui_text(ui, FONT_MONO, tx, wy, 0x9A9A9FFFu, "START FROM YOUR OWN PART");
    else {
        const char *ol = own_open ? "Start from your own part \xE2\x96\xB4" : "Start from your own part \xE2\x96\xBE";
        float ow = ui_text_width(ui, FONT_SMALL, ol) + 30;
        bool hov = ui_hover(ui, tx, wy - 6, ow, 32);
        ui_rect(ui, tx, wy - 6, ow, 32, hov ? 0x2A2A2CFFu : UI_INK, 16);
        ui_text(ui, FONT_SMALL, tx + 15, wy + 2, 0xFFFFFFFFu, ol);
        if (ui_clickable(ui, "own part", tx, wy - 6, ow, 32)) own_open = !own_open;
        wy += 18;
    }
    if (show_own) {
        static const char *names[] = {"Live flow tunnel", "Solid mechanics", "Heat transfer", "Metal printing", "Plastic printing"};
        static const char *ids[] = {"workflow fluid", "workflow solid", "workflow thermal", "workflow lpbf", "workflow fdm"};
        float px = tx, py = wy + 20;
        for (int i = 0; i < 5; i++) {
            float pw = ui_text_width(ui, FONT_SMALL, names[i]) + 28;
            if (px + pw > heroX - 24) { px = tx; py += 36; }
            bool hov = ui_hover(ui, px, py, pw, 30);
            ui_rect(ui, px, py, pw, 30, hov ? 0xF2F1EEFFu : 0xFFFFFFFFu, 15);
            ui_rect_outline(ui, px, py, pw, 30, hov ? UI_ACCENT : 0xDDDAD4FFu, 15, 1);
            ui_text(ui, FONT_SMALL, px + 14, py + 7, UI_TEXT, names[i]);
            if (ui_clickable(ui, ids[i], px, py, pw, 30)) browser_workflow(i);
            px += pw + 8;
        }
    }

    /* the bar: browse or saved, the physics, search */
    bool focus = !expert_view();
    float by = MAXI(heroY + heroH + 22, wy + (show_own && !focus ? 64 : show_own ? 50 : 24));
    float sw = MINI(320, w * 0.28f);
    if (!focus) {
        if (ui_button(ui, "BROWSE##simtab", x, by, 86, 30, browser_tab == 0)) { browser_tab = 0; browser_page = 0; }
        if (ui_button(ui, "SAVED##simtab", x + 92, by, 86, 30, browser_tab == 1)) { browser_tab = 1; browser_page = 0; browser_refresh = true; }
        text_field(ui, "simsearch", W - margin - sw - 66, by + 1, sw, 1, "Search simulations", browser_search, sizeof browser_search, NULL);
        if (ui_button(ui, "CLEAR##simsearch", W - margin - 60, by + 1, 60, 28, false)) { browser_search[0] = 0; browser_page = 0; }
        if (ui_button(ui, "BACK TO SIMULATION##library", 128, 3, 150, 24, false)) {
            lab_library_open = false; solid_field_id[0] = 0; app.hud_on = !browser_return_clean;
        }
    } else {
        text_field(ui, "simsearch", x, by + 1, sw, 1, "Search simulations", browser_search, sizeof browser_search, NULL);
        if (browser_search[0] && ui_clickable(ui, "CLEAR##simsearch", x + sw - 30, by + 1, 30, 28)) { browser_search[0] = 0; browser_page = 0; }
        if (browser_search[0]) ui_text(ui, FONT_SMALL, x + sw - 22, by + 7, UI_DIM, "\xC3\x97");
        const char *sl = browser_tab == 1 ? "All simulations" : "Saved";
        float slw = ui_text_width(ui, FONT_SMALL, sl);
        bool hov = ui_hover(ui, W - margin - slw - 4, by + 2, slw + 8, 26);
        ui_text(ui, FONT_SMALL, W - margin - slw, by + 7, hov ? UI_ACCENT : UI_DIM, sl);
        if (ui_clickable(ui, browser_tab == 1 ? "BROWSE##simtab" : "SAVED##simtab", W - margin - slw - 4, by + 2, slw + 8, 26)) {
            browser_tab = !browser_tab, browser_page = 0, browser_refresh = true;
        }
        browser_domain[0] = 0;
    }
    static char previous_search[128], previous_domain[32];
    if (strcmp(previous_search, browser_search) || strcmp(previous_domain, browser_domain)) {
        browser_page = 0;
        str_copy(previous_search, sizeof previous_search, browser_search);
        str_copy(previous_domain, sizeof previous_domain, browser_domain);
    }
    if (labapp_running() || labapp_run_notice()[0]) {
        char job[160]; snprintf(job, sizeof job, labapp_running() ? "Computing: %.50s" : "%.150s",
                               labapp_running() ? labapp_run_name() : labapp_run_notice());
        ui_push_clip(ui, x + 190, by + 7, W - margin - sw - 90 - x - 190, 20);
        ui_text(ui, FONT_SMALL, x + 190, by + 7, labapp_running() ? UI_ACCENT2 : UI_GOOD, job);
        ui_pop_clip(ui);
    }
    float y = by + 46;
    if (browser_tab == 0) {
        /* physics filters, on the bar between the tabs and the search: every domain that has something to show */
        float fx = x + 196, fright = W - margin - sw - 80;
        y = by + 1;
        if (focus) goto grid;
        if (pill(ui, "libdom all", "All", fx, y, !browser_domain[0])) browser_domain[0] = 0;
        fx += pill_width(ui, "All") + 6;
        for (int i = 0; i < lib_n; i++) {
            if (i && !strcmp(lib[i - 1].domain, lib[i].domain)) continue;
            bool any = false;
            for (int k = i; k < lib_n && !strcmp(lib[k].domain, lib[i].domain); k++) any |= lib[k].has_thumb;
            if (!any) continue;
            const char *label = lib_domain_short(lib[i].domain);
            float pw = pill_width(ui, label);
            if (fx + pw > fright) { fx = x + 196; y += 34; fright = W - margin; } /* wrap: every filter stays visible */
            char id[64]; snprintf(id, sizeof id, "libdom %s", lib[i].domain);
            if (pill(ui, id, label, fx, y, !strcmp(browser_domain, lib[i].domain))) str_copy(browser_domain, sizeof browser_domain, lib[i].domain);
            fx += pw + 6;
        }
    grid:
        y += 48;
        /* the grid: browsing shows the scenarios with pictures; a search also finds the others by name */
        int indices[96], n = 0;
        for (int i = 0; i < lib_n; i++) {
            if (browser_domain[0] && strcmp(lib[i].domain, browser_domain)) continue;
            if (!lib[i].has_thumb && !(browser_search[0] && strstr(lib[i].base, browser_search))) continue;
            if (browser_matches(lib[i].title, lib[i].base, lib_domain_name(lib[i].domain))) indices[n++] = i;
        }
        int cols = w > 1250 ? 5 : w > 980 ? 4 : w > 720 ? 3 : 2;
        float gap = 20, cw = (w - (cols - 1) * gap) / cols, ih = cw * 0.58f, chh = ih + 52;
        int rows = MAXI(1, (int)((H - 64 - y + gap) / (chh + gap))), per_page = rows * cols;
        int pages = MAXI(1, (n + per_page - 1) / per_page);
        browser_page = CLAMP(browser_page, 0, pages - 1);
        for (int k = browser_page * per_page; k < n && k < (browser_page + 1) * per_page; k++) {
            LibEntry *L = &lib[indices[k]];
            int off = k % per_page;
            char id[128]; snprintf(id, sizeof id, "lib %s", L->base);
            if (gallery_card(ui, id, L, x + (off % cols) * (cw + gap), y + (off / cols) * (chh + gap), cw, ih, true)) lib_open(L);
        }
        if (!n) say(ui, FONT_TITLE, x, y + 20, w, UI_DIM, "Nothing matches. Clear the search or choose All.", 2);
        if (!focus) {
            if (ui_button(ui, "PREV SCENARIOS##library", x, H - 46, 110, 30, false)) browser_page = (browser_page + pages - 1) % pages;
            if (ui_button(ui, "NEXT SCENARIOS##library", W - margin - 110, H - 46, 110, 30, false)) browser_page = (browser_page + 1) % pages;
        }
        if (pages > 1) {
            char page[80]; snprintf(page, sizeof page, "%d of %d", browser_page + 1, pages);
            float pw2 = ui_text_width(ui, FONT_SMALL, page), cx = W * 0.5f;
            ui_text(ui, FONT_SMALL, cx - pw2 / 2, H - 38, UI_DIM, page);
            if (focus) { /* two quiet arrows either side of the page count */
                if (round_action(ui, "PREV SCENARIOS##library", cx - pw2 / 2 - 26, H - 31, 13, "\xE2\x80\xB9", false)) browser_page = (browser_page + pages - 1) % pages;
                if (round_action(ui, "NEXT SCENARIOS##library", cx + pw2 / 2 + 26, H - 31, 13, "\xE2\x80\xBA", false)) browser_page = (browser_page + 1) % pages;
            }
        }
        panel_say("Selected domain: %s\nSearch: %s\n%d matching experiments; page %d / %d\n", browser_domain, browser_search, n, browser_page + 1, pages);
    } else {
        const float sidebar = W < 1100 ? 218 : 250;
        float ch = 86, gap = 12;
        int cols = w > 850 ? 3 : 2;
        x = margin + sidebar + 24, w = W - x - margin;
        float cw = (w - (cols - 1) * gap) / cols;
        static char pp[256][1024], pt[256][120], rp[96][1024], rt[96][120];
        static int pn, rn;
        if (browser_refresh) { pn = fem_recent_projects(pp, pt, 256); rn = labapp_recent(rp, rt, 96); browser_refresh = false; }
        say(ui, FONT_BOLD, margin, y, sidebar, UI_TEXT, "Continue your work", 2);
        say(ui, FONT_SMALL, margin, y + 36, sidebar, UI_DIM, "Saved projects in this workspace and computed lab results. Opening a result does not run it again.", 6);
        if (ui_button(ui, "REFRESH##saved", margin, y + 140, sidebar, 28, false)) browser_refresh = true;
        int rows = MAXI(1, (int)((H - 70 - y) / (ch + gap))), per_page = rows * cols;
        int indices[352], n = 0;
        for (int i = 0; i < pn + rn; i++) if (browser_matches(i < pn ? pt[i] : rt[i-pn], i < pn ? pp[i] : rp[i-pn], i < pn ? "project" : "result")) indices[n++] = i;
        int pages = MAXI(1, (n + per_page - 1) / per_page);
        browser_page = CLAMP(browser_page, 0, pages - 1);
        for (int k = browser_page * per_page; k < n && k < (browser_page + 1) * per_page; k++) {
            int i = indices[k], off = k % per_page;
            char id[160];
            const char *title = i < pn ? pt[i] : rt[i - pn];
            snprintf(id, sizeof id, i < pn ? "project %s" : "recent %s", title);
            if (i >= pn) snprintf(id, sizeof id, "recent %d", i - pn);
            if (browser_card(ui, id, title, i < pn ? "Saved project / setup and results" : "Computed lab result / replay", x + (off % cols) * (cw + gap), y + (off / cols) * (ch + gap), cw, ch, UI_GOOD)) {
                bool opened = i < pn ? fem_open_project(pp[i]) : labapp_open(rp[i-pn]);
                if (opened) {
                    if (i < pn) { labapp_close(); app_set_workspace(WS_SOLID); fem_follow_latest_result(); solid_step_pinned = false; }
                    lab_library_open = false; solid_field_id[0] = 0; inspector_scroll = 0;
                }
            }
        }
        if (!n) say(ui, FONT_BOLD, x, y + 20, w, UI_DIM, "No saved work matches. Browse a scenario to create your first result.", 2);
        if (ui_button(ui, "PREV SCENARIOS##library", x, H - 48, 140, 28, false)) browser_page = (browser_page + pages - 1) % pages;
        if (ui_button(ui, "NEXT SCENARIOS##library", x + w - 140, H - 48, 140, 28, false)) browser_page = (browser_page + 1) % pages;
        char page[80]; snprintf(page, sizeof page, "%d saved / page %d of %d", n, browser_page + 1, pages);
        ui_text(ui, FONT_SMALL, x + (w - ui_text_width(ui, FONT_SMALL, page)) / 2, H - 40, UI_DIM, page);
    }
}


static void focus_agent(float W, float H);

/* the gallery in the focus view (GOALS.md G18): pictures with one line each, your own part, the saved work, and in
 * Agentic the one bar to ask by text or voice */
static void focus_gallery(float W, float H) {
    Ui *ui = app.ui;
    if (lib_n < 0) lib_load();
    panel_text[0] = 0, panel_text_len = 0;
    ui_block_mouse(ui, 0, 44, W, H - 44);
    ui_rect(ui, 0, 44, W, H - 44, UI_PAGE, 0);
    const float margin = W < 1200 ? 28 : 44;
    float x = margin, w = W - 2 * margin, y = 64;
    bool agentic = app.ui_mode == UI_AGENT;
    float bottom = H - (agentic ? 128 : 72); /* the grid ends here; the page arrows and, in Agentic, the bar below */
    static bool own_open;
    { /* your own part: the five workflows, behind one button */
        const char *ol = own_open ? "Your part \xE2\x96\xB4" : "Your part \xE2\x96\xBE";
        float ow = ui_text_width(ui, FONT_SMALL, ol) + 30;
        bool hov = ui_hover(ui, x, y, ow, 32);
        ui_rect(ui, x, y, ow, 32, hov ? 0x2A2A2CFFu : UI_INK, 16);
        ui_text(ui, FONT_SMALL, x + 15, y + 8, 0xFFFFFFFFu, ol);
        if (ui_clickable(ui, "own part", x, y, ow, 32)) own_open = !own_open;
        const char *sl = browser_tab == 1 ? "All" : "Saved";
        float slw = ui_text_width(ui, FONT_SMALL, sl);
        bool sh = ui_hover(ui, W - margin - slw - 8, y + 2, slw + 12, 28);
        ui_text(ui, FONT_SMALL, W - margin - slw, y + 8, sh ? UI_ACCENT : UI_DIM, sl);
        if (ui_clickable(ui, browser_tab == 1 ? "BROWSE##simtab" : "SAVED##simtab", W - margin - slw - 8, y + 2, slw + 12, 28))
            browser_tab = !browser_tab, browser_page = 0, browser_refresh = true;
        y += 44;
        if (own_open) {
            static const char *names[] = {"Live flow tunnel", "Solid mechanics", "Heat transfer", "Metal printing", "Plastic printing"};
            static const char *ids[] = {"workflow fluid", "workflow solid", "workflow thermal", "workflow lpbf", "workflow fdm"};
            float px = x;
            for (int i = 0; i < 5; i++) {
                float pw = ui_text_width(ui, FONT_SMALL, names[i]) + 28;
                if (px + pw > W - margin) px = x, y += 36;
                if (pill(ui, ids[i], names[i], px, y, false)) browser_workflow(i);
                px += pw + 8;
            }
            y += 44;
        }
    }
    browser_search[0] = 0, browser_domain[0] = 0;
    int pages = 1;
    if (browser_tab == 0) { /* the scenarios with pictures */
        int indices[96], n = 0;
        for (int i = 0; i < lib_n; i++)
            if (lib[i].has_thumb) indices[n++] = i;
        int cols = w > 1250 ? 5 : w > 980 ? 4 : w > 720 ? 3 : 2;
        float gap = 20, cw = (w - (cols - 1) * gap) / cols, ih = cw * 0.58f, chh = ih + 34;
        int rows = MAXI(1, (int)((bottom - y + gap) / (chh + gap))), per_page = rows * cols;
        pages = MAXI(1, (n + per_page - 1) / per_page);
        browser_page = CLAMP(browser_page, 0, pages - 1);
        for (int k = browser_page * per_page; k < n && k < (browser_page + 1) * per_page; k++) {
            LibEntry *L = &lib[indices[k]];
            int off = k % per_page;
            char id[128];
            snprintf(id, sizeof id, "lib %s", L->base);
            if (gallery_card(ui, id, L, x + (off % cols) * (cw + gap), y + (off / cols) * (chh + gap), cw, ih, false)) lib_open(L);
        }
    } else { /* the saved projects and computed results, one line each */
        static char pp[256][1024], pt[256][120], rp[96][1024], rt[96][120];
        static int pn, rn;
        if (browser_refresh) pn = fem_recent_projects(pp, pt, 256), rn = labapp_recent(rp, rt, 96), browser_refresh = false;
        float ch = 64, gap = 12;
        int cols = w > 850 ? 3 : 2, n = pn + rn;
        float cw = (w - (cols - 1) * gap) / cols;
        int rows = MAXI(1, (int)((bottom - y + gap) / (ch + gap))), per_page = rows * cols;
        pages = MAXI(1, (n + per_page - 1) / per_page);
        browser_page = CLAMP(browser_page, 0, pages - 1);
        for (int k = browser_page * per_page; k < n && k < (browser_page + 1) * per_page; k++) {
            int off = k % per_page;
            char id[160];
            if (k < pn) snprintf(id, sizeof id, "project %s", pt[k]);
            else snprintf(id, sizeof id, "recent %d", k - pn);
            if (browser_card(ui, id, k < pn ? pt[k] : rt[k - pn], "", x + (off % cols) * (cw + gap), y + (off / cols) * (ch + gap), cw, ch, UI_GOOD)) {
                bool opened = k < pn ? fem_open_project(pp[k]) : labapp_open(rp[k - pn]);
                if (opened) {
                    if (k < pn) { labapp_close(); app_set_workspace(WS_SOLID); fem_follow_latest_result(); solid_step_pinned = false; }
                    lab_library_open = false, solid_field_id[0] = 0, inspector_scroll = 0;
                }
            }
        }
    }
    if (pages > 1) { /* two quiet arrows either side of the page count */
        char page[40];
        snprintf(page, sizeof page, "%d of %d", browser_page + 1, pages);
        float pw2 = ui_text_width(ui, FONT_SMALL, page), cx = W * 0.5f, py = bottom + 26;
        ui_text(ui, FONT_SMALL, cx - pw2 / 2, py - 7, UI_DIM, page);
        if (round_action(ui, "PREV SCENARIOS##library", cx - pw2 / 2 - 26, py, 13, "\xE2\x80\xB9", false)) browser_page = (browser_page + pages - 1) % pages;
        if (round_action(ui, "NEXT SCENARIOS##library", cx + pw2 / 2 + 26, py, 13, "\xE2\x80\xBA", false)) browser_page = (browser_page + 1) % pages;
    }
    if (agentic) focus_agent(W, H);
}

static void lab_panel(float px, float py, float pw, float ph, double time) {
    Ui *ui = app.ui;
    ui_block_mouse(ui, px, py, pw, ph);
    ui_rect_grad(ui, px, py, pw, ph, 0xF7F6F3EEu, 0xFFFFFFF2u, 10);
    ui_rect_outline(ui, px, py, pw, ph, 0xB4B1AB70u, 10, 1);
    panel_text[0] = 0, panel_text_len = 0;
    float x = px + 14, w = pw - 28, y = py + 12;
    char a[160];
    /* state */
    bool running = labapp_running(), playing = labapp_playing();
    uint32_t led = running ? UI_ACCENT2 : playing ? UI_GOOD : UI_DIM;
    float pulse = (running || playing) ? 0.6f + 0.4f * (float)sin(time * 5.0) : 1.0f;
    ui_rect(ui, x, y + 4, 9, 9, (led & 0xFFFFFF00u) | (uint32_t)(255 * pulse), 4.5f);
    ui_text(ui, FONT_BOLD, x + 16, y, led, running ? "COMPUTING" : !labapp_active() ? "READY" : playing ? "PLAYING" : "PAUSED");
    if (labapp_active()) {
        snprintf(a, sizeof a, "%s \xC2\xB7 frame %d of %d", labapp_domain(), labapp_frame() + 1, labapp_nframes());
        ui_text_right(ui, FONT_SMALL, x + w, y + 1, UI_DIM, a);
    }
    y += 24;
    float half = (w - 6) / 2;
    if (ui_button(ui, "RESULT##labtab", x, y, half, 26, !lab_library_open) && labapp_active()) lab_library_open = false;
    if (ui_button(ui, "SIMULATIONS##labtab", x + half + 6, y, half, 26, false)) hud_open_simulations();
    y += 36;
    if (labapp_run_error()[0]) y = say(ui, FONT_SMALL, x, y, w, UI_BAD, labapp_run_error(), 3) + 8;
    if (labapp_run_notice()[0]) y = say(ui, FONT_SMALL, x, y, w, UI_GOOD, labapp_run_notice(), 3) + 8;
    if (running) {
        snprintf(a, sizeof a, "Running %s. It opens here when it is done.", labapp_run_name());
        y = say(ui, FONT_SMALL, x, y, w, UI_ACCENT2, a, 3) + 8;
    }
    float content_top = y, content_bottom = py + ph - (manual_terminal_open || console_focused() ? 308 : 138);
    inspector_begin(ui, x - 4, content_top, w + 8, content_bottom);
    y -= inspector_scroll;
    if (!labapp_active()) {
        y = say(ui, FONT_TITLE, x, y, w, UI_TEXT, labapp_prepared() ? labapp_scenario_title() : "Choose a simulation", 3) + 10;
        y = say(ui, FONT_SMALL, x, y, w, UI_DIM,
                labapp_prepared() ? "Review the example inputs below, then Run simulation. Calculation continues in the background."
                                  : "Use Simulations to open an example or a saved project.", 4) + 14;
    }
    if (labapp_active()) {
        y = say(ui, FONT_TITLE, x, y, w, 0x111111FFu, labapp_title(), 2) + 10;
        /* a picture (relativity: the rays already chose the colours and the camera) has no fields or views to pick */
        bool picture = !strcmp(labapp_domain(), "relativity");
        /* what to see: the result's fields */
        if (!picture) {
        section(ui, x, y, w, "SHOW", NULL);
        y += 20;
        char names[16][48];
        int nf = labapp_field_names(names, 16);
        float cx = x;
        for (int i = 0; i < nf; i++) {
            float bw = ui_text_width(ui, FONT_SMALL, names[i]) + 16;
            if (cx + bw > x + w) cx = x, y += 26;
            char id[80];
            snprintf(id, sizeof id, "%s##field", names[i]);
            if (ui_button(ui, id, cx, y, bw, 22, !strcmp(names[i], labapp_field()))) {
                char cmd[80];
                snprintf(cmd, sizeof cmd, "lab field %s", names[i]);
                exec_cmd(cmd);
            }
            cx += bw + 4;
        }
        y += 32;
        /* how to see it */
        section(ui, x, y, w, "VIEW", NULL);
        y += 20;
        static const char *const VL[4] = {"3D##labview", "FROM ABOVE##labview", "FLAT##labview", "MESH##labview"};
        static const char *const VC[4] = {"lab view iso", "lab view top", "lab view flat", "lab mesh"};
        float vw4 = (w - 12) / 4;
        for (int i = 0; i < 4; i++)
            if (ui_button(ui, i==2 && labapp_native3d()?"FIT##labview":VL[i], x + i * (vw4 + 4), y, vw4, 22, false)) exec_cmd(i==2 && labapp_native3d()?"lab fit":VC[i]);
        y += 32;
        }
        if (labapp_native3d()) {
            section(ui,x,y,w,"SECTION",NULL);y+=20;
            static const char *const axes[4]={"OFF##labcut","X##labcut","Y##labcut","Z##labcut"};
            float aw = (w - 64) / 4;
            for(int k=0;k<4;k++)if(ui_button(ui,axes[k],x+k*aw,y,aw-4,22,labapp_section_axis()==k-1))labapp_section_set(k-1,labapp_section_fraction());
            if(ui_button(ui,"FLIP##labcut",x+w-60,y,60,22,labapp_section_flipped()))exec_cmd("lab section flip");
            y+=28;
            if(labapp_section_axis()>=0){double f=labapp_section_fraction();char val[32];snprintf(val,sizeof val,"%.0f %%",100*f);
                if(slider_row(ui,"labsection","Position",val,x,y,w,&f,0,1,false))labapp_section_set(labapp_section_axis(),f);y+=34;}
        }
        if(labapp_is_water()){
            if(ui_button(ui,"SURFACE##labwater",x,y,(w-4)/2,22,labapp_water_surface()))exec_cmd("lab surface on");
            if(ui_button(ui,"PARTICLES##labwater",x+(w+4)/2,y,(w-4)/2,22,!labapp_water_surface()))exec_cmd("lab surface off");
            y+=30;
        }
    }
    if (labapp_prepared()) {
        /* The same physical inputs are available before and after computation. */
        LabControl cs[16];
        int nc = labapp_controls(cs, 16);
        if (nc > 0) {
            section(ui, x, y, w, "SIMULATION INPUTS", NULL);
            y += 20;
            for (int i = 0; i < nc; i++) {
                double v = cs[i].value;
                char id[48], val[48];
                snprintf(id, sizeof id, "labctl%d", i);
                snprintf(val, sizeof val, "%.4g", v);
                if (slider_row(ui, id, cs[i].label, val, x, y, w, &v, cs[i].min, cs[i].max, cs[i].log)) labapp_control_set(i, v);
                y += 34;
            }
        } else {
            y = say(ui, FONT_SMALL, x, y, w, UI_DIM, "This example uses its published scenario inputs. No adjustable physical inputs are declared.", 3) + 8;
        }
        if (step_button(ui, running ? "COMPUTING...##labrerun" : labapp_active() ? "RUN WITH THESE##labrerun" : "RUN SIMULATION##labrerun",
                        x, y, w, 30, false, running ? "a simulation is already computing" : NULL)) labapp_rerun();
        y += 42;
        y = say(ui, FONT_SMALL, x, y, w, UI_DIM, "Inputs change the next run. The displayed result stays tied to the inputs that computed it.", 3) + 8;
    }
    inspector_end(ui, x - 4, content_top, w + 8, content_bottom, y);
    y = content_bottom + 8;
    if (ui_button(ui, "CHOOSE ANOTHER SIMULATION##library", x, y, w, 26, false)) hud_open_simulations();
    if (ui_button(ui, "CLEAN VIEW##labclean", x, y + 32, w, 22, false)) exec_cmd("hud clean");
    manual_terminal(ui, x, y + 60, w, py + ph - 6, time);

}

/* play, pause and scrub the open lab result */
static void playback_bar(float x, float y, float w) {
    Ui *ui = app.ui;
    int n = labapp_nframes();
    if (n < 1) return;
    ui_block_mouse(ui, x, y - 4, w, 30);
    ui_rect(ui, x, y - 4, w, 30, 0xF7F6F3D0u, 8);
    if (ui_button(ui, labapp_playing() ? "PAUSE##labplay" : "PLAY##labplay", x + 6, y, 64, 22, labapp_playing()))
        exec_cmd(labapp_playing() ? "lab pause" : "lab play");
    if (ui_button(ui, "PREV##labframe", x + 74, y, 42, 22, false)) {
        labapp_set_playing(false); labapp_set_frame(labapp_frame() - 1);
    }
    if (ui_button(ui, "NEXT##labframe", x + 120, y, 42, 22, false)) {
        labapp_set_playing(false); labapp_set_frame(labapp_frame() + 1);
    }
    char speed[48];
    snprintf(speed, sizeof speed, "%.0f fps##labfps", labapp_fps());
    if (ui_button(ui, speed, x + 166, y, 62, 22, false)) {
        char cmd[48];
        snprintf(cmd, sizeof cmd, "lab fps %.0f", labapp_fps() < 12 ? 12.0 : labapp_fps() < 24 ? 24.0 : 6.0);
        exec_cmd(cmd);
    }
    double f = labapp_frame();
    char t[48];
    double tm = labapp_frame_time(labapp_frame());
    if (labapp_time_unit()[0]) snprintf(t, sizeof t, "%.4g %s", tm, labapp_time_unit());
    else if (fabs(tm) < 1e-6 && tm != 0) snprintf(t, sizeof t, "%.3g ns", tm * 1e9);
    else if (fabs(tm) < 1e-3 && tm != 0) snprintf(t, sizeof t, "%.3g us", tm * 1e6);
    else if (fabs(tm) < 1 && tm != 0) snprintf(t, sizeof t, "%.3g ms", tm * 1e3);
    else snprintf(t, sizeof t, "%.4g s", tm);
    float tw = ui_text_width(ui, FONT_MONO, t) + 16;
    if (n > 1 && w > 250 + tw && ui_slider(ui, "labscrub", x + 240, y + 6, w - 252 - tw, 10, &f, 0, n - 1, false)) {
        labapp_set_playing(false);
        labapp_set_frame((int)lround(f));
    }
    ui_text_right(ui, FONT_MONO, x + w - 8, y + 3, UI_TEXT, t);
}

/* A saved print or thermal run uses the same always-visible transport as a lab result. */
static void fem_playback_bar(float x, float y, float w) {
    Ui *ui = app.ui;
    int n = fem_step_count();
    if (n < 2) return;
    ui_block_mouse(ui, x, y - 4, w, 30);
    ui_rect(ui, x, y - 4, w, 30, 0xF7F6F3D0u, 8);
    if (ui_button(ui, fem_playing() ? "PAUSE##femplay" : "PLAY##femplay", x + 6, y, 64, 22, fem_playing()))
        fem_set_playing(!fem_playing());
    if (ui_button(ui, "PREV##femframe", x + 74, y, 42, 22, false)) { fem_set_playing(false); fem_set_step(MAXI(0, fem_step() - 1)); }
    if (ui_button(ui, "NEXT##femframe", x + 120, y, 42, 22, false)) { fem_set_playing(false); fem_set_step(MINI(n - 1, fem_step() + 1)); }
    char speed[48]; snprintf(speed, sizeof speed, "%.0f/s##femfps", fem_play_speed());
    if (ui_button(ui, speed, x + 166, y, 56, 22, false))
        fem_set_play_speed(fem_play_speed() < 6 ? 6 : fem_play_speed() < 12 ? 12 : 2);
    char label[96];
    if (!strcmp(fem_state()->result_kind, "lpbf_build")) snprintf(label, sizeof label, "step %d/%d", fem_step() + 1, n);
    else snprintf(label, sizeof label, "%.4g s", fem_step_time(fem_step()));
    float tw = ui_text_width(ui, FONT_MONO, label) + 16;
    double step = fem_step();
    if (w > 250 + tw && ui_slider(ui, "femscrub", x + 234, y + 6, w - 246 - tw, 10, &step, 0, n - 1, false)) {
        fem_set_playing(false); fem_set_step((int)lround(step));
    }
    ui_text_right(ui, FONT_MONO, x + w - 8, y + 3, UI_TEXT, label);
}

static void experiment_panel(float px, float py, float pw, float ph, double time) {
    Ui *ui = app.ui;
    const SimStatus *st = &app.status;
    const SimUnits *u = &st->units;
    SimParams *p = &app.params;
    ui_block_mouse(ui, px, py, pw, ph);
    ui_rect_grad(ui, px, py, pw, ph, 0xF7F6F3EEu, 0xFFFFFFF2u, 10);
    ui_rect_outline(ui, px, py, pw, ph, 0xB4B1AB70u, 10, 1);

    float x = px + 14, w = pw - 28, y = py + 12;
    char a[64], b[64];
    panel_text[0] = 0, panel_text_len = 0;
    float content_top = y, content_bottom = py + ph - (manual_terminal_open || console_focused() ? 240 : 72);
    inspector_begin(ui, x - 4, content_top, w + 8, content_bottom);
    y -= inspector_scroll;

    /* header */
    uint32_t led = st->diverged ? UI_BAD : st->busy ? UI_ACCENT2 : st->running ? UI_GOOD : UI_DIM;
    float pulse = st->running ? 0.6f + 0.4f * (float)sin(time * 5.0) : 1.0f;
    ui_rect(ui, x, y + 4, 9, 9, (led & 0xFFFFFF00u) | (uint32_t)(255 * pulse), 4.5f);
    const char *state = st->diverged ? "DIVERGED" : st->busy ? "BUSY" : st->running ? "RUNNING" : (st->step ? "PAUSED" : "READY");
    float sw = ui_text(ui, FONT_BOLD, x + 16, y, led, state) + 20;
    if (st->running && app.playback > 0 && app.playback < 1.0) {
        snprintf(b, sizeof b, "slow motion %.0f%%", app.playback * 100);
        ui_text(ui, FONT_SMALL, x + sw + 2, y + 1, UI_ACCENT2, b);
    }
    if (!st->busy && ui_hover(ui, x - 4, y - 2, sw + 4, 20)) ui_rect_outline(ui, x - 4, y - 2, sw + 4, 20, 0xE8551C70u, 4, 1);
    if (!st->busy && ui_clickable(ui, "run state", x - 4, y - 2, sw + 4, 20)) exec_cmd(st->running ? "pause" : "start");
    snprintf(a, sizeof a, "t = %.4g s \xC2\xB7 step %llu", st->sim_time, (unsigned long long)st->step);
    ui_text_right(ui, FONT_SMALL, x + w, y + 1, UI_TEXT, a);
    y += 24;

    /* experiment */
    section(ui, x, y, w, "EXPERIMENT", app.scene[0] ? app.scene : NULL);
    y += 20;
    float cw = (w - 16) * 0.5f, x2 = x + cw + 16;
    snprintf(a, sizeof a, "%s %.1f\xC2\xB0""C", fluid_label(p->fluid), p->temperature);
    kv_cmd(ui, x, y, cw, "fluid", a, UI_TEXT, "fluid ");
    snprintf(a, sizeof a, "%.1f kg/m\xC2\xB3", u->rho);
    kv_cmd(ui, x2, y, cw, "density", a, UI_TEXT, "density ");
    y += 17;
    fmt_eng(a, sizeof a, u->nu, "m\xC2\xB2/s");
    kv_cmd(ui, x, y, cw, "\xCE\xBD", a, UI_TEXT, "viscosity ");
    const bool re_limited = u->re_eff > 0 && u->re_eff < 0.99 * u->re; /* above the lattice's viscosity floor */
    if (re_limited) snprintf(b, sizeof b, "%.2g eff", u->re_eff);
    else snprintf(b, sizeof b, "%.4g", u->re);
    kv_cmd(ui, x2, y, cw, "Reynolds", b, re_limited ? UI_ACCENT2 : UI_ACCENT, "re ");
    y += 17;
    fmt_eng(a, sizeof a, p->ref_length, "m");
    kv_cmd(ui, x, y, cw, "length L", a, UI_TEXT, "length ");
    fmt_eng(b, sizeof b, u->frontal_area, "m\xC2\xB2");
    kv_cmd(ui, x2, y, cw, "frontal A", b, UI_TEXT, "fit ");
    y += 22;

    /* controls: flow conditions, model orientation and simulation pace share one block */
    controls_header(ui, x, y, w);
    y += 20;
    const float controls_y = y;
    double v = p->speed;
    if (app.panel_tab == PANEL_MODEL) {
        y = model_controls(ui, x, y, w);
    } else if (app.panel_tab == PANEL_TIME) {
        y = time_controls(ui, x, y, w);
    } else if (app.panel_tab == PANEL_LINES) {
        y = lines_controls(ui, x, y, w);
    } else {
        fmt_eng(a, sizeof a, p->speed, "m/s");
        if (slider_row(ui, "sl_speed", "free-stream speed", a, x, y, w, &v, 0.001, 60.0, true)) {
            p->speed = v;
            app_push_params();
        }
        y += 32;
        v = p->aoa;
        snprintf(a, sizeof a, "%+.1f\xC2\xB0", p->aoa);
        /* widen the range instead of snapping an angle that was set elsewhere (MODEL tab, aoa command, option-drag) */
        double alo = p->aoa < -25.0 ? -90.0 : -25.0, ahi = p->aoa > 25.0 ? 90.0 : 25.0;
        if (slider_row(ui, "sl_aoa", "angle of attack", a, x, y, w, &v, alo, ahi, false)) {
            double nv = round(v * 2.0) / 2.0;
            if (nv != p->aoa) { /* a rounded slider would otherwise resend the parameters every frame it is held */
                p->aoa = nv;
                app_push_params();
            }
        }
        y += 32;
        v = p->roughness > 0 ? p->roughness : 1e-6;
        if (p->roughness <= 0) snprintf(a, sizeof a, "smooth");
        else fmt_eng(a, sizeof a, p->roughness * 1e3, "mm");
        if (slider_row(ui, "sl_rough", "surface roughness ks", a, x, y, w, &v, 1e-6, 0.01, true)) {
            p->roughness = v < 1.5e-6 ? 0.0 : v;
            app_push_params();
        }
        y += 32;
        v = p->temperature;
        double tlo = p->fluid == FLUID_AIR ? -30.0 : 0.0, thi = p->fluid == FLUID_AIR ? 200.0 : 95.0;
        snprintf(a, sizeof a, "%.1f \xC2\xB0""C", p->temperature);
        if (slider_row(ui, "sl_temp", "temperature", a, x, y, w, &v, tlo, thi, false)) {
            double nv = round(v * 2.0) / 2.0;
            if (nv != p->temperature) {
                p->temperature = nv;
                app_push_params();
            }
        }
        y += 32;
        v = p->turbulence * 100.0;
        snprintf(a, sizeof a, "%.1f %%", p->turbulence * 100.0);
        if (slider_row(ui, "sl_turb", "inlet turbulence", a, x, y, w, &v, 0.0, 20.0, false)) {
            double nv = round(v * 10.0) / 1000.0;
            if (nv != p->turbulence) {
                p->turbulence = nv;
                app_push_params();
            }
        }
        y += 32;
    }
    y = MAXI(y, controls_y + 160.0f); /* same height on every tab, so the terminal below stays put */
    if (app.rs.slice_on) {
        double sp = app.rs.slice_pos;
        snprintf(a, sizeof a, "%c = %.2f", 'x' + app.rs.slice_axis, app.rs.slice_pos);
        if (slider_row(ui, "sl_slice", "slice position", a, x, y, w, &sp, 0.0, 1.0, false)) app.rs.slice_pos = (float)sp;
        y += 32;
    }
    y += 2;

    /* forces */
    snprintf(a, sizeof a, "St %.3f", st->strouhal);
    section(ui, x, y, w, "FORCES", st->strouhal > 0 ? a : NULL);
    y += 20;
    float c3 = (w - 20) / 3.0f;
    snprintf(a, sizeof a, "%.4f", st->cd);
    kv_cmd(ui, x, y, c3, "Cd", a, UI_ACCENT2, "status");
    snprintf(a, sizeof a, "%+.4f", st->cl);
    kv_cmd(ui, x + c3 + 10, y, c3, "Cl", a, UI_ACCENT, "status");
    snprintf(a, sizeof a, "%+.4f", st->cs);
    kv_cmd(ui, x + 2 * (c3 + 10), y, c3, "Cs", a, UI_VIOLET, "status");
    y += 17;
    fmt_eng(a, sizeof a, st->fx, "N");
    kv_cmd(ui, x, y, cw, "drag", a, UI_TEXT, "forces forces.csv");
    fmt_eng(b, sizeof b, st->fy, "N");
    kv_cmd(ui, x2, y, cw, "lift", b, UI_TEXT, "forces forces.csv");
    y += 20;

    static ForceSample hist[SIM_FORCE_HISTORY];
    static float cd[SIM_FORCE_HISTORY], cl[SIM_FORCE_HISTORY];
    int n = sim_force_history(app.sim, hist, 800);
    float plot_h = 54;
    ui_rect(ui, x, y, w, plot_h, 0xFFFFFFD0u, 5);
    if (n > 8) {
        float lo = 1e30f, hi = -1e30f;
        for (int i = n / 8; i < n; i++) {
            cd[i] = hist[i].cd, cl[i] = hist[i].cl;
            lo = MINI(lo, MINI(cd[i], cl[i])), hi = MAXI(hi, MAXI(cd[i], cl[i]));
        }
        for (int i = 0; i < n / 8; i++) cd[i] = hist[n / 8].cd, cl[i] = hist[n / 8].cl;
        float pad = (hi - lo) * 0.12f + 1e-3f;
        lo -= pad, hi += pad;
        if (lo < 0 && hi > 0) {
            float zy = y + plot_h * (1.0f - (0 - lo) / (hi - lo));
            ui_rect(ui, x + 2, zy, w - 4, 1, 0xB4B1AB60u, 0);
        }
        ui_push_clip(ui, x, y, w, plot_h);
        ui_plot(ui, x + 2, y + 2, w - 4, plot_h - 4, cd, n, lo, hi, UI_ACCENT2, 1.4f);
        ui_plot(ui, x + 2, y + 2, w - 4, plot_h - 4, cl, n, lo, hi, UI_ACCENT, 1.4f);
        ui_pop_clip(ui);
        snprintf(a, sizeof a, "%.3g", hi);
        ui_text(ui, FONT_SMALL, x + 4, y + 2, UI_FAINT, a);
        snprintf(a, sizeof a, "%.3g", lo);
        ui_text(ui, FONT_SMALL, x + 4, y + plot_h - 15, UI_FAINT, a);
    } else {
        ui_text(ui, FONT_SMALL, x + 8, y + plot_h * 0.5f - 7, UI_FAINT, "force history appears once the flow runs");
    }
    y += plot_h + 8;

    /* solver */
    section(ui, x, y, w, "SOLVER", NULL);
    y += 20;
    snprintf(a, sizeof a, "%d\xC3\x97%d\xC3\x97%d", st->nx, st->ny, st->nz);
    kv_cmd(ui, x, y, cw, "lattice", a, UI_TEXT, "grid ");
    snprintf(b, sizeof b, "%.2fM \xC2\xB7 %.0f MB", st->cells / 1e6, st->mem_mb);
    kv_cmd(ui, x2, y, cw, "cells", b, UI_TEXT, "quality ");
    y += 17;
    snprintf(a, sizeof a, "%.1f /s", st->steps_per_sec);
    kv_cmd(ui, x, y, cw, "steps", a, UI_TEXT, "perf");
    snprintf(b, sizeof b, "\xC3\x97%.3g", st->realtime_factor);
    kv_cmd(ui, x2, y, cw, "real-time", b, UI_TEXT, "ulb ");
    y += 17;
    fmt_eng(a, sizeof a, u->dx * 1e3, "mm");
    kv_cmd(ui, x, y, cw, "\xCE\x94x", a, UI_TEXT, "quality ");
    fmt_eng(b, sizeof b, u->dt, "s");
    kv_cmd(ui, x2, y, cw, "\xCE\x94t", b, UI_TEXT, "ulb ");
    y += 17;
    snprintf(a, sizeof a, "%.5f", u->tau0);
    kv_cmd(ui, x, y, cw, "\xCF\x84\xE2\x82\x80", a, u->tau0 < 0.5005 ? UI_ACCENT2 : UI_TEXT, "les ");
    snprintf(b, sizeof b, "%s%s%s", p->collision == LBM_RECURSIVE ? "RR3" : p->collision ? "REG" : "BGK",
             p->hrr_sigma > 0 && p->hrr_sigma < 0.9999 ? "+HRR" : "", p->smagorinsky > 0 ? "+LES" : "");
    kv_cmd(ui, x2, y, cw, "operator", b, UI_TEXT, "collision ");
    y += 22;

    inspector_end(ui, x - 4, content_top, w + 8, content_bottom, y);
    manual_terminal(ui, x, content_bottom + 4, w, py + ph - 6, time);
}

/* integer exponent as UTF-8 superscript digits, e.g. -3 -> "⁻³" */
static void superscript_int(char *buf, size_t cap, int v) {
    static const char *digit[10] = {"\xE2\x81\xB0", "\xC2\xB9", "\xC2\xB2", "\xC2\xB3", "\xE2\x81\xB4",
                                    "\xE2\x81\xB5", "\xE2\x81\xB6", "\xE2\x81\xB7", "\xE2\x81\xB8", "\xE2\x81\xB9"};
    char tmp[16];
    snprintf(tmp, sizeof tmp, "%d", v);
    buf[0] = 0;
    for (const char *p = tmp; *p; p++) strncat(buf, *p == '-' ? "\xE2\x81\xBB" : digit[*p - '0'], cap - strlen(buf) - 1);
}

/* the lab's legend: the palette as the picture wears it, its range and the field, drawn by the panel (crisp at any size) */
static void lab_legend(float x, float bottom) {
    Ui *ui = app.ui;
    double lo, hi;
    int cmap;
    const char *field, *unit;
    if (!labapp_legend(&lo, &hi, &cmap, &field, &unit)) return;
    float w = 300, h = 60, y = bottom - h;
    ui_rect(ui, x, y, w, h, 0xF7F6F3D8u, 8);
    ui_rect_outline(ui, x, y, w, h, 0xB4B1AB60u, 8, 1);
    char title[120];
    snprintf(title, sizeof title, "%s%s%s%s", field, unit[0] ? "  [" : "", unit, unit[0] ? "]" : "");
    ui_text(ui, FONT_SMALL, x + 10, y + 7, UI_TEXT, title);
    const int N = 70;
    float bw = (w - 20) / N;
    for (int i = 0; i < N; i++) {
        float rgb[3];
        labview_cmap_rgb(cmap, (i + 0.5) / N, rgb);
        uint32_t c = ((uint32_t)(255 * rgb[0]) << 24) | ((uint32_t)(255 * rgb[1]) << 16) | ((uint32_t)(255 * rgb[2]) << 8) | 0xFFu;
        ui_rect(ui, x + 10 + i * bw, y + 26, bw + 0.6f, 10, c, 0);
    }
    char a[32], b[32], m[32];
    snprintf(a, sizeof a, "%.4g", lo), snprintf(b, sizeof b, "%.4g", hi), snprintf(m, sizeof m, "%.4g", 0.5 * (lo + hi));
    ui_text(ui, FONT_SMALL, x + 10, y + 41, UI_DIM, a);
    ui_text_right(ui, FONT_SMALL, x + w - 10, y + 41, UI_DIM, b);
    if (expert_view()) ui_text(ui, FONT_SMALL, x + 0.5f * w - 0.5f * ui_text_width(ui, FONT_SMALL, m), y + 41, UI_FAINT, m); /* the window: the two ends only */
}

static void legend(float x, float bottom) {
    Ui *ui = app.ui;
    const SimUnits *u = &app.status.units;
    float w = 300, h = app.rs.line_cmap >= 0 ? 82 : 64;
    float y = bottom - h;
    ui_block_mouse(ui, x, y, w, h);
    ui_rect(ui, x, y, w, h, 0xF7F6F3D8u, 8);
    ui_rect_outline(ui, x, y, w, h, 0xB4B1AB60u, 8, 1);
    const char *unit = display_field_unit(app.display_field);
    double s = display_field_scale(app.display_field, u);
    double vlo = app.lat_lo * s, vhi = app.lat_hi * s;
    if (vhi < vlo) {
        double tmp = vlo;
        vlo = vhi, vhi = tmp;
    }
    /* labels stay short plain numbers: kPa/MPa and mm/s where natural, otherwise a x10^n factor in the title */
    double vmax = MAXI(fabs(vlo), fabs(vhi)), mul = 1.0;
    char ustr[48], title[160];
    str_copy(ustr, sizeof ustr, unit);
    if (!strcmp(unit, "Pa") && vmax >= 1e3) {
        mul = vmax >= 1e6 ? 1e-6 : 1e-3;
        str_copy(ustr, sizeof ustr, vmax >= 1e6 ? "MPa" : "kPa");
    } else if (!strcmp(unit, "m/s") && vmax > 0 && vmax < 0.1) {
        mul = 1e3;
        str_copy(ustr, sizeof ustr, "mm/s");
    } else if (vmax > 0 && (vmax >= 1e4 || vmax < 1e-2)) {
        int e = (int)floor(log10(vmax) / 3.0) * 3;
        char sup[24];
        superscript_int(sup, sizeof sup, e);
        mul = pow(10.0, -e);
        snprintf(ustr, sizeof ustr, "\xC3\x97" "10%s%s%s", sup, unit[0] ? " " : "", unit);
    }
    snprintf(title, sizeof title, "%s%s%s%s", display_field_label(app.display_field), ustr[0] ? "  [" : "", ustr,
             ustr[0] ? "]" : "");
    ui_text(ui, FONT_SMALL, x + 10, y + 7, UI_TEXT, title);
    if (expert_view())
        ui_text_right(ui, FONT_SMALL, x + w - 10, y + 7, app.manual_range ? UI_ACCENT2 : UI_FAINT,
                      app.manual_range ? "MANUAL" : colormap_name(app.rs.cmap));
    ui_colormap_bar(ui, x + 10, y + 26, w - 20, 10, render_colormap_texture(app.renderer), 3);
    if (ui_clickable(ui, "legend", x, y, w, 56)) {
        char cmd[64];
        snprintf(cmd, sizeof cmd, "cmap %s", colormap_name((app.rs.cmap + 1) % CMAP_COUNT));
        exec_cmd(cmd);
    }
    /* ticks at round 1-2-5 values inside the colour range; a label that would touch its neighbour is skipped */
    double lo_d = vlo * mul, hi_d = vhi * mul, span = hi_d - lo_d;
    if (span > 0 && span < 1e30) {
        double raw = span / 4.0, mag = pow(10.0, floor(log10(raw))), rr = raw / mag;
        double step = (rr < 1.5 ? 1.0 : rr < 3.5 ? 2.0 : rr < 7.5 ? 5.0 : 10.0) * mag;
        int decimals = CLAMP((int)-floor(log10(step) + 1e-9), 0, 6);
        long i0 = (long)ceil(lo_d / step - 1e-9), i1 = (long)floor(hi_d / step + 1e-9);
        float last_right = -1e30f;
        for (long i = i0; i <= i1 && i - i0 < 12; i++) {
            double v = (double)i * step, t = (v - lo_d) / span;
            char buf[32];
            snprintf(buf, sizeof buf, "%.*f", decimals, v);
            float tx = x + 10 + (w - 20) * (float)t;
            ui_rect(ui, tx - 0.5f, y + 37, 1, 4, 0x7A7A8080u, 0);
            float tw = ui_text_width(ui, FONT_SMALL, buf);
            float lx = CLAMP(tx - tw * 0.5f, x + 6, x + w - 6 - tw);
            if (lx < last_right + 6) continue;
            ui_text(ui, FONT_SMALL, lx, y + 43, UI_DIM, buf);
            last_right = lx + tw;
        }
    }
    if (app.rs.line_cmap >= 0) {
        char lbl[48];
        snprintf(lbl, sizeof lbl, "lines \xC2\xB7 %s", colormap_name(app.rs.line_cmap));
        ui_text(ui, FONT_SMALL, x + 10, y + 61, UI_DIM, lbl);
        float lw = ui_text_width(ui, FONT_SMALL, lbl) + 16;
        ui_colormap_bar(ui, x + 10 + lw, y + 66, w - 20 - lw, 6, render_line_colormap_texture(app.renderer), 3);
        if (ui_clickable(ui, "line colours", x, y + 57, w, h - 57)) {
            char cmd[64];
            /* cycle through the maps and then back to "same", so the field colours can be restored from the interface */
            int next = app.rs.line_cmap + 1;
            snprintf(cmd, sizeof cmd, "linecmap %s", next >= CMAP_COUNT ? "same" : colormap_name(next));
            exec_cmd(cmd);
        }
    }
}

/* the same colour bar for the finite-element field, in its own unit and with the peak marked */
static void solid_legend(float x, float bottom) {
    Ui *ui = app.ui;
    const FemState *s = fem_state();
    float w = 300, h = 64, y = bottom - h;
    ui_block_mouse(ui, x, y, w, h);
    ui_rect(ui, x, y, w, h, 0xF7F6F3D8u, 8);
    ui_rect_outline(ui, x, y, w, h, 0xB4B1AB60u, 8, 1);
    char title[160];
    const char *unit = fem_field_unit(fem_field());
    if (app.ui_mode == UI_SIMPLE)
        snprintf(title, sizeof title, "%s%s%s%s",
                 fem_field() == FEM_DISPLACEMENT ? "warp" : fem_field() == FEM_VON_MISES ? "residual stress" : fem_field() == FEM_FAILURE ? fem_field_label(FEM_FAILURE) : "temperature",
                 unit[0] ? "  [" : "", unit, unit[0] ? "]" : "");
    else
        snprintf(title, sizeof title, "%s%s%s%s", fem_field_label(fem_field()), unit[0] ? "  [" : "", unit, unit[0] ? "]" : "");
    ui_text(ui, FONT_SMALL, x + 10, y + 7, UI_TEXT, title);
    /* Results and the legend share one texture; clicks cycle the presentation palettes. */
    int rc = render_result_colormap(app.renderer);
    /* a clipped bar has to say so, or the top colour reads as the peak */
    char cmlbl[48];
    if (expert_view()) snprintf(cmlbl, sizeof cmlbl, "%s%s", fem_range_p99() ? "99th pct \xC2\xB7 " : "", colormap_name(rc));
    else snprintf(cmlbl, sizeof cmlbl, "%s", fem_range_p99() ? "99th pct" : ""); /* the window: only what changes the reading */
    ui_text_right(ui, FONT_SMALL, x + w - 10, y + 7, UI_FAINT, cmlbl);
    ui_colormap_bar(ui, x + 10, y + 26, w - 20, 10, render_result_colormap_texture(app.renderer), 3);
    if (ui_clickable(ui, "legend", x, y, w, h)) {
        static const int cycle[] = {CMAP_STUDIO, CMAP_VIRIDIS, CMAP_INFERNO, CMAP_PLASMA, CMAP_TURBO};
        int next = CMAP_STUDIO;
        for (int i = 0; i < (int)(sizeof cycle / sizeof cycle[0]); i++)
            if (rc == cycle[i]) next = cycle[(i + 1) % (sizeof cycle / sizeof cycle[0])];
        render_set_result_colormap(app.renderer, next);
    }
    char lo[32], hi[32];
    if (s->have_result) {
        snprintf(lo, sizeof lo, "%.4g", s->range_lo);
        snprintf(hi, sizeof hi, "%.4g", s->range_hi);
    } else {
        str_copy(lo, sizeof lo, "-"), str_copy(hi, sizeof hi, "-");
    }
    ui_text(ui, FONT_SMALL, x + 10, y + 42, UI_DIM, lo);
    ui_text_right(ui, FONT_SMALL, x + w - 10, y + 42, UI_DIM, hi);
    if (s->have_result && s->nsteps > 1) {
        char t[64];
        if (!strcmp(s->result_kind, "lpbf_build"))
            snprintf(t, sizeof t, "process step %d/%d", fem_step() + 1, s->nsteps);
        else snprintf(t, sizeof t, "t = %.4g s   step %d/%d", s->time_s, fem_step() + 1, s->nsteps);
        float tw = ui_text_width(ui, FONT_SMALL, t);
        ui_text(ui, FONT_SMALL, x + (w - tw) * 0.5f, y + 42, UI_ACCENT2, t);
    }
}

static void axis_gizmo(float cx, float cy) {
    Ui *ui = app.ui;
    const mat4 *V = &app.cam.view;
    ui_block_mouse(ui, cx - 36, cy - 36, 72, 72);
    if (ui_hover(ui, cx - 36, cy - 36, 72, 72)) ui_rect(ui, cx - 36, cy - 36, 72, 72, 0xE8551C12u, 36);
    if (ui_clickable(ui, "axes", cx - 36, cy - 36, 72, 72)) exec_cmd("camera hero");
    static const char *labels[3] = {"X", "Y", "Z"};
    uint32_t cols[3] = {0xD23B3BFFu, 0x1E8E4EFFu, 0xE8551CFFu};
    for (int a = 0; a < 3; a++) {
        vec3 d = v3(a == 0, a == 1, a == 2);
        vec3 e = m4_mul_dir(*V, d);
        float ex = cx + e.x * 26, ey = cy - e.y * 26;
        ui_line(ui, cx, cy, ex, ey, 1.6f, cols[a]);
        ui_text(ui, FONT_SMALL, ex + (e.x >= 0 ? 3 : -9), ey - 7, cols[a], labels[a]);
    }
    if (app.workspace != WS_SOLID) ui_text(ui, FONT_SMALL, cx - 30, cy + 22, 0x7A7A80C0u, "X = flow");
    else if (app.ui_mode == UI_ADVANCED) ui_text(ui, FONT_SMALL, cx - 34, cy + 22, 0x7A7A80C0u, "build frame");
}

static void probe_card(void) {
    if (!app.probe_on) return;
    Ui *ui = app.ui;
    float sx, sy;
    if (!camera_project(&app.cam, app.probe, &sx, &sy)) return;
    sx /= app.scale, sy /= app.scale;
    float rho, u[3];
    if (!app_probe_sample(app.probe, &rho, u)) return;
    const SimUnits *un = &app.status.units;
    double vs = units_speed(un);
    double speed = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) * vs;
    double pa = (rho - 1) / 3.0 * units_pressure(un), cp = (rho - 1) / 3.0 / (0.5 * un->u_lb * un->u_lb);
    float x = sx + 16, y = sy - 70, w = 200, h = 78;
    ui_rect(ui, x, y, w, h, 0xF7F6F3DDu, 7);
    ui_rect_outline(ui, x, y, w, h, 0xB0700A90u, 7, 1);
    ui_line(ui, sx + 3, sy - 3, x, y + h, 1, 0xB0700A90u);
    char a[64];
    ui_text(ui, FONT_BOLD, x + 9, y + 6, UI_ACCENT2, "PROBE");
    snprintf(a, sizeof a, "(%.3f, %.3f, %.3f) m", app.probe.x * un->dx, app.probe.y * un->dx, app.probe.z * un->dx);
    ui_text_right(ui, FONT_SMALL, x + w - 8, y + 7, UI_DIM, a);
    format_si(a, sizeof a, speed, "m/s");
    kv(ui, x + 9, y + 26, w - 18, "|u|", a, UI_TEXT);
    format_si(a, sizeof a, pa, "Pa");
    kv(ui, x + 9, y + 42, w - 18, "p (gauge)", a, UI_TEXT);
    snprintf(a, sizeof a, "%.3f", cp);
    kv(ui, x + 9, y + 58, w - 18, "Cp", a, UI_TEXT);
}

/* ---- the focus interface (owner, 2026-10-04: "very few buttons, clear and straightforward") --------------------
 * On screen: the scene, one action, the inputs that matter, the legend. Everything else (layers, sections, meshes,
 * solver numbers, the terminal) is behind "...", which opens the full controls: the panels as they were. Headless
 * runs and scripts start with the full controls, so tests and agents find every control where it always was. */
static bool hud_expert = false, hud_expert_set = false;
void hud_set_expert(bool on) { hud_expert = on, hud_expert_set = true; }
static bool expert_view(void) { return hud_expert_set ? hud_expert : app.headless; }
bool hud_focus_view(void) { return app.hud_on && !expert_view() && !lab_library_open; }

/* top: the name (back to the gallery) and the Agentic | Manual switch; nothing else (GOALS.md G18) */
static void focus_topbar(float W, bool library) {
    Ui *ui = app.ui;
    (void)library;
    ui_block_mouse(ui, 0, 0, W, 44);
    ui_rect(ui, 0, 0, W, 44, 0xF2F1EEEEu, 0);
    float ww = ui_text_width(ui, FONT_TITLE, "OpenPhysicsAI");
    bool hov = ui_hover(ui, 14, 8, ww + 12, 28);
    ui_text(ui, FONT_TITLE, 20, 12, hov ? UI_ACCENT : UI_TEXT, "OpenPhysicsAI");
    if (ui_clickable(ui, "SIMULATIONS##labnav", 14, 8, ww + 12, 28)) hud_open_simulations();
    const char *names[2] = {"Agentic", "Manual"};
    const char *ids[2] = {"Agentic##mode", "Manual##mode"};
    const int modes[2] = {UI_AGENT, UI_ADVANCED};
    float sw[2] = {ui_text_width(ui, FONT_SMALL, names[0]) + 26, ui_text_width(ui, FONT_SMALL, names[1]) + 26};
    float x = W - 16 - (sw[0] + sw[1] + 4);
    ui_rect(ui, x - 2, 6, sw[0] + sw[1] + 8, 32, 0xE6E4E0FFu, 16);
    for (int i = 0; i < 2; i++) {
        bool on = app.ui_mode == modes[i] || (i == 1 && app.ui_mode == UI_SIMPLE);
        if (on) ui_shadow(ui, x, 8, sw[i], 28, 14, 4, 0x00000020u), ui_rect(ui, x, 8, sw[i], 28, 0xFFFFFFFFu, 14);
        ui_text(ui, FONT_SMALL, x + 13, 14, on ? UI_TEXT : UI_DIM, names[i]);
        if (ui_clickable(ui, ids[i], x, 8, sw[i], 28) && !on) {
            char cmd[32];
            snprintf(cmd, sizeof cmd, "mode %s", ui_mode_name(modes[i]));
            exec_cmd(cmd);
        }
        x += sw[i] + 4;
    }
}

/* a round black button with a white glyph: the one primary action */
static bool round_action(Ui *ui, const char *id, float cx, float cy, float r, const char *glyph, bool accent) {
    bool hov = ui_hover(ui, cx - r, cy - r, 2 * r, 2 * r);
    ui_shadow(ui, cx - r, cy - r + 3, 2 * r, 2 * r, r, 8, 0x00000030u);
    ui_rect(ui, cx - r, cy - r, 2 * r, 2 * r, accent ? UI_ACCENT : hov ? 0x2A2A2CFFu : UI_INK, r);
    if (!strcmp(glyph, "pause")) { /* two bars: no font carries a pause sign we can count on */
        ui_rect(ui, cx - r * 0.32f, cy - r * 0.36f, r * 0.22f, r * 0.72f, 0xFFFFFFFFu, 1);
        ui_rect(ui, cx + r * 0.10f, cy - r * 0.36f, r * 0.22f, r * 0.72f, 0xFFFFFFFFu, 1);
    } else {
        float gw = ui_text_width(ui, FONT_BOLD, glyph);
        ui_text(ui, FONT_BOLD, cx - gw * 0.5f + (strstr(glyph, "\xE2\x96\xB6") ? 1.5f : 0), cy - 8, 0xFFFFFFFFu, glyph);
    }
    return ui_clickable(ui, id, cx - r, cy - r, 2 * r, 2 * r);
}

/* the legend, bottom left; a click on a lab legend shows the next field the result carries */
static void focus_legend(float H) {
    if (labapp_active()) {
        if (strcmp(labapp_domain(), "relativity")) lab_legend(16, H - 16);
        char names[16][48];
        int n = labapp_field_names(names, 16);
        if (n > 1 && ui_clickable(app.ui, "focus field", 16, H - 76, 300, 60)) {
            int cur = 0;
            for (int i = 0; i < n; i++) if (!strcmp(names[i], labapp_field())) cur = i;
            labapp_set_field(names[(cur + 1) % n]);
        }
    } else if (app.workspace == WS_SOLID) {
        if (fem_state()->have_result) solid_legend(14, H - 14);
    } else if (app.status.step > 0) {
        legend(14, H - 14);
    }
}

/* a question from the AI: its request to use a tool (Allow, Deny), or the setup it made of the person's words, shown on
 * the part in colour before it runs anything (Run it, Not right; GOALS.md G17 step 6) */
static float approval_card(Ui *ui, float x, float bottom, float w) {
    char tool[64], detail[600], head[160];
    if (!agent_approval_pending(tool, sizeof tool, detail, sizeof detail)) return 0;
    bool setup = !strcmp(tool, "setup_confirm");
    if (setup) snprintf(head, sizeof head, "Is this how the part is held and loaded?");
    else snprintf(head, sizeof head, "%s asks to use %s", agent_kind() == 1 ? "Codex" : "Claude Code", tool);
    const char *yes = setup ? "Run it" : "Allow", *no = setup ? "Not right" : "Deny";
    float lw = w - 32, lh = ui_line_height(ui, FONT_MONO) + 1;
    int lines = MAXI(1, MINI(wrapped_lines(ui, FONT_MONO, detail, lw - 12), setup ? 6 : 4));
    float h = 14 + 22 + lines * lh + 8 + 12 + 30 + 14, y = bottom - h;
    ui_block_mouse(ui, x, y, w, h);
    ui_shadow(ui, x, y + 4, w, h, 14, 18, 0x00000022u);
    ui_rect(ui, x, y, w, h, 0xFFFFFFFFu, 14);
    ui_rect_outline(ui, x, y, w, h, (UI_ACCENT & 0xFFFFFF00u) | 0xB0u, 14, 1.2f);
    ui_text(ui, FONT_BOLD, x + 16, y + 14, UI_TEXT, head);
    ui_rect(ui, x + 16, y + 38, lw, lines * lh + 8, 0xF5F4F1FFu, 8);
    say(ui, FONT_MONO, x + 22, y + 42, lw - 12, UI_TEXT, detail, setup ? 6 : 4);
    float by = y + h - 44, bw = setup ? 96 : 84;
    float ax = x + w - 16 - bw, dx = ax - 8 - bw;
    bool ha = ui_hover(ui, ax, by, bw, 30), hd = ui_hover(ui, dx, by, bw, 30);
    ui_rect(ui, dx, by, bw, 30, hd ? 0xF2F1EEFFu : 0xFFFFFFFFu, 15);
    ui_rect_outline(ui, dx, by, bw, 30, 0xDDDAD4FFu, 15, 1);
    ui_rect(ui, ax, by, bw, 30, ha ? 0x2A2A2CFFu : UI_INK, 15);
    float tw = ui_text_width(ui, FONT_BOLD, no);
    ui_text(ui, FONT_BOLD, dx + (bw - tw) * 0.5f, by + 7, UI_TEXT, no);
    tw = ui_text_width(ui, FONT_BOLD, yes);
    ui_text(ui, FONT_BOLD, ax + (bw - tw) * 0.5f, by + 7, 0xFFFFFFFFu, yes);
    if (ui_clickable(ui, setup ? "setup run" : "approval allow", ax, by, bw, 30)) agent_approval_answer(true);
    else if (ui_clickable(ui, setup ? "setup not right" : "approval deny", dx, by, bw, 30)) agent_approval_answer(false);
    return h;
}

/* the microphone: a round button with a capsule, its cradle and stand; red and breathing while it listens */
static bool mic_button(Ui *ui, const char *id, float cx, float cy, float r, bool listening) {
    bool hov = ui_hover(ui, cx - r, cy - r, 2 * r, 2 * r);
    if (listening) {
        float pulse = 0.5f + 0.5f * (float)sin(app.time * 4.0);
        ui_rect(ui, cx - r - 6, cy - r - 6, 2 * r + 12, 2 * r + 12, (UI_ACCENT & 0xFFFFFF00u) | (uint32_t)(40 + 60 * pulse), r + 6);
    }
    ui_shadow(ui, cx - r, cy - r + 3, 2 * r, 2 * r, r, 8, 0x00000030u);
    ui_rect(ui, cx - r, cy - r, 2 * r, 2 * r, listening ? UI_ACCENT : hov ? 0x2A2A2CFFu : UI_INK, r);
    uint32_t c = 0xFFFFFFFFu;
    ui_rect(ui, cx - r * 0.18f, cy - r * 0.52f, r * 0.36f, r * 0.62f, c, r * 0.18f);       /* the capsule */
    ui_line(ui, cx - r * 0.34f, cy - r * 0.02f, cx - r * 0.34f, cy + r * 0.06f, 1.6f, c); /* the cradle */
    ui_line(ui, cx + r * 0.34f, cy - r * 0.02f, cx + r * 0.34f, cy + r * 0.06f, 1.6f, c);
    ui_line(ui, cx - r * 0.34f, cy + r * 0.06f, cx, cy + r * 0.32f, 1.6f, c);
    ui_line(ui, cx + r * 0.34f, cy + r * 0.06f, cx, cy + r * 0.32f, 1.6f, c);
    ui_line(ui, cx, cy + r * 0.32f, cx, cy + r * 0.50f, 1.6f, c);                       /* the stand */
    ui_line(ui, cx - r * 0.22f, cy + r * 0.50f, cx + r * 0.22f, cy + r * 0.50f, 1.6f, c);
    return ui_clickable(ui, id, cx - r, cy - r, 2 * r, 2 * r);
}

/* Agentic (GOALS.md G18): one bar, the text field and one round button (the microphone, or stop while the agent works);
 * the agent's words for the latest question above it, fading after 8 s; the AI's request to use a tool when there is
 * one. A press on the microphone starts listening, a second press sends the words as the question. */
static void focus_agent(float W, float H) {
    Ui *ui = app.ui;
    agent_load_settings();
    agent_poll();
    {
        static char shown_job[64], shown_part[200];
        const FemState *fs = fem_state();
        if (fs->have_result && strcmp(shown_job, fs->result_job) != 0) {
            str_copy(shown_job, sizeof shown_job, fs->result_job);
            exec_cmd("fem step last");
            exec_cmd("fem fit");
        }
        /* a part just opened fills the view, so that a person can point at its faces (G17 step 6) */
        char part[200];
        snprintf(part, sizeof part, "%s/%s/%d", fs->project_id, fs->body, fs->triangles);
        vec3 lo, hi;
        if (app.workspace == WS_SOLID && fs->have_project && fs->nbodies > 0 && !fs->have_result && strcmp(shown_part, part) != 0 &&
            fem_world_bounds(&lo, &hi)) {
            str_copy(shown_part, sizeof shown_part, part);
            exec_cmd("fem fit");
        }
    }
    labapp_follow(app.dt);
    static char voice_err[320], shown[420];
    static double shown_at = -100;
    {
        char words[2048], verr[320];
        int v = voice_poll(words, sizeof words, verr, sizeof verr);
        if (v > 0) { /* the log says what the microphone heard, as it says what the agent was asked */
            LOGI("voice: heard \"%s\"", words);
            hud_agent_ask_voice(words); /* a spoken question runs with the voice model, as in the headset */
            lab_library_open = false;
        } else if (v < 0) {
            LOGW("voice: %s", verr);
            str_copy(voice_err, sizeof voice_err, verr);
        }
    }
    float w = MINI(680, W - 64), x = (W - w) * 0.5f, y = H - 76;
    bool running = agent_running();
    char last[420] = {0}; /* the agent's words since the latest question */
    for (int i = agent_line_count() - 1; i >= 0; i--) {
        int kind;
        const char *l = agent_line_at(i, &kind);
        if (kind == 2 && !strncmp(l, "asked: ", 7)) break;
        if (kind == 0 || kind == 3) {
            agent_display_line(l, kind, last, sizeof last);
            break;
        }
    }
    {   /* a conversation's answer as it comes: its newest words, a word at a time */
        int turn;
        bool done;
        const char *live = agent_turn_words(&turn, &done);
        size_t n = strlen(live);
        if (running && !done && n) {
            const char *from = n > 260 ? live + n - 260 : live;
            if (from > live) {
                while (*from && *from != ' ' && *from != '\n') from++;
                while (*from == ' ' || *from == '\n') from++;
            }
            snprintf(last, sizeof last, "%s%s", from > live ? "\xE2\x80\xA6" : "", from);
            for (char *c = last; *c; c++)
                if (*c == '\n') *c = ' ';
        }
    }
    if (agent_error[0]) str_copy(last, sizeof last, agent_error);
    else if (voice_err[0]) str_copy(last, sizeof last, voice_err);
    else if (agent_ntools == 0 && !agent_command[0]) str_copy(last, sizeof last, "No AI tool found. Install Claude Code or Codex, sign in once, then ask here.");
    if (strcmp(last, shown)) str_copy(shown, sizeof shown, last), shown_at = app.time;
    seen_note();
    bool asking = approval_card(ui, x + 10, y - 16, w - 20) > 0; /* a question from the AI comes first */
    double age = app.time - shown_at;
    if (!asking && (running || (last[0] && age < 10))) {
        float a = running || age < 8 ? 1.0f : (float)(1 - (age - 8) / 2);
        float lw = w - 60;
        int lines = last[0] ? MINI(wrapped_lines(ui, FONT_SMALL, last, lw), 3) : 1;
        float lh = 16 + lines * (ui_line_height(ui, FONT_SMALL) + 1), ly = y - lh - 16;
        ui_rect(ui, x + 10, ly, w - 20, lh, 0xFFFFFF00u | (uint32_t)(235 * a), 14);
        if (running) {
            float pulse = 0.5f + 0.5f * (float)sin(app.time * 5.0);
            ui_rect(ui, x + 26, ly + lh * 0.5f - 4, 8, 8, (UI_ACCENT & 0xFFFFFF00u) | (uint32_t)(120 + 135 * pulse), 4);
        }
        uint32_t col = (agent_error[0] || voice_err[0]) ? UI_BAD : UI_TEXT;
        if (last[0]) say(ui, FONT_SMALL, x + (running ? 44 : 26), ly + 8, lw - (running ? 18 : 0), (col & 0xFFFFFF00u) | (uint32_t)(255 * a), last, 3);
    }
    ui_shadow(ui, x, y + 6, w, 52, 26, 18, 0x00000026u);
    int kind = agent_kind(); /* which AI answers stays known (owner, 2026-10-04) without a chip on screen */
    text_field(ui, "text:agent", x, y, w - 60, 1, kind == 0 ? "Ask Claude Code by text or voice" : kind == 1 ? "Ask Codex by text or voice" : "Ask by text or voice",
               agent_prompt, sizeof agent_prompt, home_ask);
    if (running) {
        if (round_action(ui, "STOP##agent", x + w - 26, y + 15, 20, "\xE2\x96\xA0", false)) agent_stop();
    } else if (mic_button(ui, "MIC##agent", x + w - 26, y + 15, 20, voice_listening())) {
        if (voice_listening()) voice_stop();
        else {
            char e[320];
            voice_err[0] = 0;
            if (!voice_start(e, sizeof e)) str_copy(voice_err, sizeof voice_err, e), LOGW("voice: %s", e);
            else LOGI("voice: listening");
        }
    }
}

/* a pill button: the label in the middle, dark when it is the primary action; false when disabled */
static bool pill_button(Ui *ui, const char *id, const char *label, float x, float y, float w, float h, bool primary, bool enabled) {
    bool hov = enabled && ui_hover(ui, x, y, w, h);
    ui_shadow(ui, x, y + 4, w, h, h * 0.5f, 12, 0x00000026u);
    uint32_t bg = !enabled ? 0xE6E4E0FFu : primary ? (hov ? 0x2A2A2CFFu : UI_INK) : (hov ? 0xFFFFFFFFu : 0xFFFFFFF0u);
    ui_rect(ui, x, y, w, h, bg, h * 0.5f);
    if (!primary || !enabled) ui_rect_outline(ui, x, y, w, h, 0xDDDAD4FFu, h * 0.5f, 1);
    float tw = ui_text_width(ui, FONT_BOLD, label);
    ui_text(ui, FONT_BOLD, x + (w - tw) * 0.5f, y + h * 0.5f - 9, !enabled ? UI_DIM : primary ? 0xFFFFFFFFu : UI_TEXT, label);
    return enabled && ui_clickable(ui, id, x, y, w, h);
}

/* Manual (GOALS.md G18): Open and Run, nothing else. Run does what fits what is open: a lab simulation runs, the flow
 * starts or pauses, the open part is solved. A refusal shows above the two for a few seconds, then fades. */
static void focus_manual(float W, float H, double time) {
    Ui *ui = app.ui;
    static double run_at = -100;
    float bw = 104, bh = 44, gap = 10, y = H - 18 - bh, xr = W - 18 - bw, xo = xr - gap - bw;
    bool lab = labapp_active() || labapp_prepared(), fluid = !lab && app.workspace == WS_FLUID, solid = !lab && app.workspace == WS_SOLID;
    const FemState *fs = fem_state();
    bool computing = (lab && labapp_running()) || (solid && fs->job_active);
    bool can_run = lab || fluid || (solid && fs->have_project);
    const char *label = computing ? "Running" : fluid && app.status.running ? "Pause" : "Run";
    ui_block_mouse(ui, xo, y, 2 * bw + gap, bh);
    if (pill_button(ui, "Open##manual", "Open", xo, y, bw, bh, false, true)) hud_open_simulations();
    if (pill_button(ui, "Run##manual", label, xr, y, bw, bh, true, can_run && !computing)) {
        run_at = time;
        if (lab) labapp_rerun();
        else if (fluid) exec_cmd(app.status.running ? "pause" : "start");
        else exec_cmd("solid run static");
    }
    if (computing) { /* working: a dot that breathes inside the button, no words */
        float pulse = 0.5f + 0.5f * (float)sin(time * 5.0);
        ui_rect(ui, xr + 14, y + bh * 0.5f - 4, 8, 8, (UI_ACCENT & 0xFFFFFF00u) | (uint32_t)(120 + 135 * pulse), 4);
    }
    const char *why = solid ? fem_last_error() : "";
    double age = time - run_at;
    if (why && why[0] && age < 8) {
        float a = age < 6 ? 1.0f : (float)(1 - (age - 6) / 2);
        float lw = MINI(520, W - 40), lx = W - 18 - lw;
        int lines = MINI(wrapped_lines(ui, FONT_SMALL, why, lw - 28), 3);
        float lh = 16 + lines * (ui_line_height(ui, FONT_SMALL) + 1), ly = y - 12 - lh;
        ui_rect(ui, lx, ly, lw, lh, 0xFFFFFF00u | (uint32_t)(230 * a), 14);
        say(ui, FONT_SMALL, lx + 14, ly + 8, lw - 28, (UI_BAD & 0xFFFFFF00u) | (uint32_t)(255 * a), why, 3);
    }
}

static void focus_ui(float W, float H, double time) {
    focus_topbar(W, false);
    focus_legend(H);
    if (app.ui_mode == UI_AGENT) focus_agent(W, H);
    else focus_manual(W, H, time);
}

void hud_draw(void) {
    Ui *ui = app.ui;
    float W = (float)app.win_w, H = (float)app.win_h;
    double time = app.time;
    const SimStatus *st = &app.status;
    if (app.box_zoom || app.box_pick) {
        float bx = (float)MINI(app.box_x0, app.box_x1), by = (float)MINI(app.box_y0, app.box_y1);
        float bw = (float)fabs(app.box_x1 - app.box_x0), bh = (float)fabs(app.box_y1 - app.box_y0);
        uint32_t tint = app.box_pick ? (app.box_pick_remove ? 0xB0700A14u : 0x1E8E4E14u) : 0xE8551C14u;
        uint32_t edge = app.box_pick ? (app.box_pick_remove ? 0xB0700AC0u : 0x1E8E4EC0u) : 0xE8551CC0u;
        ui_rect(ui, bx, by, bw, bh, tint, 2);
        ui_rect_outline(ui, bx, by, bw, bh, edge, 2, 1);
        ui_text(ui, FONT_SMALL, bx + 4, by + bh + 4, app.box_pick ? UI_GOOD : UI_ACCENT,
                app.box_pick ? (app.box_pick_remove ? "drop the faces inside" : "take the faces inside")
                             : "zoom to region");
    }
    if (app.model_drag && app.press_moved) {
        char d[128];
        snprintf(d, sizeof d, "pitch %+.1f\xC2\xB0   yaw %+.1f\xC2\xB0   roll %+.1f\xC2\xB0", app.params.aoa, app.params.yaw,
                 app.params.roll);
        float dw = ui_text_width(ui, FONT_SMALL, d) + 18, dx = (float)app.press_x + 18, dy = (float)app.press_y - 34;
        ui_rect(ui, dx, dy, dw, 22, 0xF7F6F3E0u, 6);
        ui_rect_outline(ui, dx, dy, dw, 22, 0xB0700AA0u, 6, 1);
        ui_text(ui, FONT_SMALL, dx + 9, dy + 4, UI_TEXT, d);
    }
    if (!app.hud_on) {
        if (clean_view) {
            char title[240], meta[360], note[260] = {0}, section_meta[80] = {0};
            if (labapp_active()) {
                snprintf(title, sizeof title, "%s", labapp_title());
                snprintf(meta, sizeof meta, "%s", labapp_status());
                if (strcmp(labapp_domain(), "relativity")) lab_legend(16, H - 46);
                if (labapp_native3d()) {
                    int axis = labapp_section_axis();
                    if (axis >= 0) snprintf(section_meta, sizeof section_meta, "SECTION %c at %.3g%% - %s", "XYZ"[axis],
                                             100 * labapp_section_fraction(), labapp_section_flipped() ? "flipped" : "normal");
                    else snprintf(section_meta, sizeof section_meta, "SECTION OFF");
                }
            } else if (app.workspace == WS_SOLID && fem_state()->have_result) {
                const FemState *s = fem_state();
                const char *unit = fem_field_unit(fem_field());
                snprintf(title, sizeof title, "%s \xC2\xB7 %s%s%s%s", s->project, fem_field_label(fem_field()), unit[0] ? " [" : "", unit, unit[0] ? "]" : "");
                char progress[80];
                bool process = !strcmp(s->result_kind, "lpbf_build");
                if (process) snprintf(progress, sizeof progress, "process step %d/%d", fem_step() + 1, fem_step_count());
                else snprintf(progress, sizeof progress, "t = %.5g s \xC2\xB7 stored time %d/%d", s->time_s, fem_step() + 1, fem_step_count());
                snprintf(meta, sizeof meta, "%s \xC2\xB7 deformation \xC3\x97%.4g \xC2\xB7 max displacement %.5g mm \xC2\xB7 range: %s",
                         progress, fem_deform_applied(), s->max_displacement_mm,
                         fem_range_all() ? (process ? "all process steps" : "all stored times") : (process ? "this process step" : "this stored time"));
                solid_legend(14, H - 14);
                printing_model_note(note, sizeof note);
                if (fem_section_on()) snprintf(section_meta, sizeof section_meta, "SECTION %c at %.3g%% - %s", "XYZ"[fem_section_axis()],
                                               100 * fem_section_position(), fem_section_flipped() ? "flipped" : "normal");
                else snprintf(section_meta, sizeof section_meta, "SECTION OFF");
            } else {
                snprintf(title, sizeof title, "%s \xC2\xB7 %s", app.scene, display_field_label(app.display_field));
                snprintf(meta, sizeof meta, "t = %.5g s \xC2\xB7 step %llu", st->sim_time, (unsigned long long)st->step);
                legend(14, H - 14);
            }
            float tw = MAXI(ui_text_width(ui, FONT_BOLD, title), ui_text_width(ui, FONT_SMALL, meta));
            tw = MAXI(tw, ui_text_width(ui, FONT_SMALL, note));
            tw = MAXI(tw, ui_text_width(ui, FONT_SMALL, section_meta));
            ui_rect(ui, 10, 10, MINI(tw + 20, W - 20), 52 + (note[0] ? 20 : 0) + (section_meta[0] ? 20 : 0), 0xF7F6F3D0u, 7);
            ui_text(ui, FONT_BOLD, 20, 18, UI_TEXT, title);
            ui_text(ui, FONT_SMALL, 20, 38, UI_DIM, meta);
            if (note[0]) ui_text(ui, FONT_SMALL, 20, 58, UI_ACCENT2, note);
            if (section_meta[0]) ui_text(ui, FONT_SMALL, 20, note[0] ? 78 : 58, UI_DIM, section_meta);
            panel_text[0] = 0, panel_text_len = 0;
            panel_say("Clean view: %s\n%s\n%s\n%s\n", title, meta, note, section_meta);
        }
        if (clean_view) {
            float by = H - 88;
            ui_block_mouse(ui, W - 252, by, 240, 36);
            ui_rect(ui, W - 252, by, 240, 36, 0xF7F6F3EEu, 8);
            if (ui_button(ui, "SIMULATIONS##labnav", W - 246, by + 5, 118, 26, false)) hud_open_simulations();
            if (ui_button(ui, "CONTROLS##presentation", W - 122, by + 5, 104, 26, false)) exec_cmd("hud on");
        }
        return;
    }
    if (!expert_view()) {
        if (lab_library_open) { focus_gallery(W, H); focus_topbar(W, true); return; }
        focus_ui(W, H, time);
        return;
    }
    title_strip(W);
    toolbar(W);
    if (lab_library_open) { simulation_browser(W, H); return; }
    float px = W - app.panel_w - 10, py = TOOLBAR_Y + TOOLBAR_H + 10;
    if (app.ui_mode == UI_SIMPLE)
        simple_panel(px, py, app.panel_w, H - py - 10, time);
    else if (app.ui_mode == UI_AGENT)
        agent_panel(px, py, app.panel_w, H - py - 10, time);
    else if (labapp_active() || labapp_prepared())
        lab_panel(px, py, app.panel_w, H - py - 10, time);
    else if (app.workspace == WS_SOLID)
        analysis_panel(px, py, app.panel_w, H - py - 10, time);
    else
        experiment_panel(px, py, app.panel_w, H - py - 10, time);
    if (labapp_prepared() && !labapp_active() && app.ui_mode == UI_ADVANCED) {
        ui_rect(ui, 0, TOOLBAR_Y + TOOLBAR_H + 1, px - 6, H - TOOLBAR_Y - TOOLBAR_H - 1, 0xF7F6F3FFu, 0);
        float cx = 42, cy = H * 0.36f, cw = px - 84;
        ui_text(ui, FONT_SMALL, cx, cy, UI_ACCENT, labapp_running() ? "COMPUTING IN THE BACKGROUND" : "READY TO COMPUTE");
        cy = say(ui, FONT_TITLE, cx, cy + 30, cw, UI_TEXT, labapp_scenario_title(), 4) + 20;
        say(ui, FONT_SMALL, cx, cy, cw, UI_DIM, labapp_running() ? "The computed result will appear here when it is ready. You can browse other simulations while it runs."
                                                     : "Adjust the simulation inputs in the panel, then choose Run simulation. A saved result can be reopened from Simulations > Saved.", 4);
        return;
    }
    activity_strip(14, TOOLBAR_Y + TOOLBAR_H + 40);
    if (glossary_open) glossary(W, H);

    float vw = px; /* viewport area to the left of the panel */
    if (labapp_active()) {
        /* a physics-lab result fills the viewport: its own status line and how to move it, nothing of the tunnel */
        ui_text(ui, FONT_SMALL, 14, TOOLBAR_Y + TOOLBAR_H + 10, UI_DIM, labapp_status());
        if (strcmp(labapp_domain(), "relativity")) lab_legend(16, H - 46); /* a picture carries its own colours */
        playback_bar(16, H - 34, vw - 32);
        return;
    }
    if (app.workspace == WS_SOLID) {
        /* a colour bar with no numbers in it is a control that cannot be used: it waits for a result. The geometry
         * on its own carries no field, so it does not bring the legend with it either. */
        if (fem_state()->have_result) {
            solid_legend(14, H - (fem_step_count() > 1 ? 46 : 14));
            fem_playback_bar(16, H - 34, vw - 32);
        }
    } else {
        legend(14, H - 14);
    }
    axis_gizmo(vw - 60, H - (app.workspace == WS_SOLID && fem_step_count() > 1 ? 86 : 52));
    probe_card();

    if (app.workspace == WS_SOLID) {
        const FemState *fs = fem_state();
        char sa[200];
        if (fs->have_result)
            snprintf(sa, sizeof sa, "%s \xC2\xB7 %s \xC2\xB7 peak %.4g %s \xC2\xB7 deformation \xC3\x97%.4g",
                     fs->project, fem_field_label(fem_field()), fs->peak, fem_field_unit(fem_field()), fem_deform_applied());
        else if (fs->have_project)
            snprintf(sa, sizeof sa, "%s \xC2\xB7 %d bod%s \xC2\xB7 %d elements \xC2\xB7 %d conditions", fs->project,
                     fs->nbodies, fs->nbodies == 1 ? "y" : "ies", fs->nelems, fs->nbcs);
        else
            snprintf(sa, sizeof sa, "Open an STL of the part, or drag it onto the window.");
        if (app.ui_mode != UI_ADVANCED) {
            /* Simple and Agent: how to move the view, and nothing about the engine */
            const char *mh = "drag to turn \xC2\xB7 shift-drag to move \xC2\xB7 scroll to zoom \xC2\xB7 F to see the whole part";
            float mw = ui_text_width(ui, FONT_SMALL, mh);
            if (mw < vw - 360) ui_text(ui, FONT_SMALL, 330 + (vw - 330 - mw) * 0.5f, H - 22, 0x3A3A3EC0u, mh);
            return;
        }
        ui_text(ui, FONT_SMALL, 14, TOOLBAR_Y + TOOLBAR_H + 10, UI_DIM, sa);
        const char *shint = "drag orbit \xC2\xB7 shift-drag pan \xC2\xB7 scroll zoom \xC2\xB7 F frame the part \xC2\xB7 "
                            "W back to the tunnel \xC2\xB7 the analysis runs on the engine the MCP tools drive";
        float shw = ui_text_width(ui, FONT_SMALL, shint);
        if (shw < vw - 360) ui_text(ui, FONT_SMALL, 330 + (vw - 330 - shw) * 0.5f, H - 22, 0x7A7A80A0u, shint);
        return;
    }

    /* status readout */
    char a[160];
    char reb[64];
    if (st->units.re_eff > 0 && st->units.re_eff < 0.99 * st->units.re)
        snprintf(reb, sizeof reb, "Re %.3g (lattice %.2g)", st->units.re, st->units.re_eff);
    else
        snprintf(reb, sizeof reb, "Re %.3g", st->units.re);
    snprintf(a, sizeof a, "%s \xC2\xB7 U %.3g m/s \xC2\xB7 AoA %+.1f\xC2\xB0 \xC2\xB7 %s", reb, app.params.speed, app.params.aoa,
             display_field_label(app.display_field));
    ui_text(ui, FONT_SMALL, 14, TOOLBAR_Y + TOOLBAR_H + 10, UI_DIM, a);
    if (st->ramp > 0 && st->ramp < 1 && st->running) {
        snprintf(a, sizeof a, "inflow ramping up %.0f%%", st->ramp * 100);
        ui_text(ui, FONT_SMALL, 14, TOOLBAR_Y + TOOLBAR_H + 26, UI_ACCENT, a);
    }

    const char *hint = "drag orbit \xC2\xB7 shift-drag pan \xC2\xB7 scroll zoom \xC2\xB7 \xE2\x8C\x98-drag box zoom \xC2\xB7 "
                       "double-click focus \xC2\xB7 \xE2\x8C\xA5-drag turn model \xC2\xB7 \xE2\x8C\xA5-click probe \xC2\xB7 space run \xC2\xB7 W analysis";
    float hw = ui_text_width(ui, FONT_SMALL, hint);
    if (hw < vw - 360) ui_text(ui, FONT_SMALL, 330 + (vw - 330 - hw) * 0.5f, H - 22, 0x7A7A80A0u, hint);

    /* start overlay */
    if (!st->running && st->step == 0 && !st->busy && !st->diverged) {
        /* below the framed model, so dragging or Option-dragging a fresh model doesn't press the button */
        float bw = 330, bh = 56, bx = (vw - bw) * 0.5f, by = H * 0.74f - bh * 0.5f;
        (void)time;
        ui_block_mouse(ui, bx, by, bw, bh);
        bool hover = ui_hover(ui, bx, by, bw, bh);
        ui_shadow(ui, bx, by + 6, bw, bh, bh * 0.5f, 14, 0x00000038u);
        ui_rect(ui, bx, by, bw, bh, hover ? 0x2A2A2CFFu : UI_INK, bh * 0.5f);
        ui_rect(ui, bx + 9, by + 9, bh - 18, bh - 18, UI_ACCENT, (bh - 18) * 0.5f);
        ui_text(ui, FONT_SMALL, bx + 9 + (bh - 18) * 0.5f - 3, by + bh * 0.5f - 8, 0xFFFFFFFFu, "\xE2\x96\xB6");
        const char *label = "Start experiment";
        ui_text(ui, FONT_TITLE, bx + bh + 6, by + 9, 0xFFFFFFFFu, label);
        const char *sub = "Space \xC2\xB7 the water starts still and the inflow ramps up";
        ui_text(ui, FONT_SMALL, bx + bh + 6, by + 31, 0xFFFFFFA0u, sub);
        if (ui_clickable(ui, "START EXPERIMENT", bx, by, bw, bh)) exec_cmd("start");
    }
    if (st->busy) {
        float bw = 300, bh = 40, bx = (vw - bw) * 0.5f, by = 110;
        ui_rect(ui, bx, by, bw, bh, 0xEFEDE9EEu, 8);
        ui_rect_outline(ui, bx, by, bw, bh, 0xB0700A90u, 8, 1);
        for (int i = 0; i < 8; i++) {
            double ang = time * 5.0 + i * M_PI / 4;
            float r = 8, cx = bx + 22 + (float)cos(ang) * r, cy = by + 20 + (float)sin(ang) * r;
            ui_rect(ui, cx - 1.8f, cy - 1.8f, 3.6f, 3.6f, 0xB0700A00u | (uint32_t)(40 + 200 * i / 8), 1.8f);
        }
        ui_text(ui, FONT_SMALL, bx + 44, by + 13, UI_TEXT, st->busy_msg);
    }
    if (st->diverged) {
        float bw = 480, bh = 44, bx = (vw - bw) * 0.5f, by = 110;
        ui_rect(ui, bx, by, bw, bh, 0xF7F6F3EEu, 8);
        ui_rect_outline(ui, bx, by, bw, bh, 0xD23B3BC0u, 8, 1);
        ui_text(ui, FONT_BOLD, bx + 14, by + 7, UI_BAD, "SIMULATION DIVERGED");
        ui_text(ui, FONT_SMALL, bx + 14, by + 24, UI_TEXT, "lower speed or 'ulb', enable 'les', raise 'quality', then press START");
    }
}
