/* app.h - application state shared by the main loop, HUD and terminal commands */
#pragma once

#include "camera.h"
#include "geom/mesh.h"
#include "render.h"
#include "sim.h"
#include "threads.h"
#include "ui.h"
#include "vis.h"
#include "visworker.h"

typedef enum { DF_SPEED = 0, DF_UX, DF_UY, DF_UZ, DF_PRESSURE, DF_CP, DF_VORTICITY, DF_QCRIT, DF_COUNT } DisplayField;

enum { PANEL_FLOW = 0, PANEL_MODEL, PANEL_TIME, PANEL_LINES };
/* the two halves of the same application: the lattice-Boltzmann tunnel and the finite-element analysis */
enum { WS_FLUID = 0, WS_SOLID, WS_COUNT };
/* who the window is for: SIMPLE guides a newcomer through five screens, ADVANCED is every control, AGENT hands the
 * part to the user's own AI tool and shows what it does */
enum { UI_SIMPLE = 0, UI_ADVANCED, UI_AGENT, UI_MODE_COUNT };

typedef struct App {
    Sim *sim;
    SimParams params;
    SimStatus status;
    Renderer *renderer;
    RenderSettings rs;
    Camera cam;
    Ui *ui;
    VisWorker *worker;
    VisRequest req;

    /* model */
    Mesh mesh;
    Mesh mesh_orig; /* the model as loaded, restored by 'orient reset' */
    bool has_model;
    char model_name[128];
    char model_path[1024];
    vec3 model_lo, model_hi; /* placed model bounding box, lattice coordinates */
    mat4 model_M;
    char scene[64];
    double scene_kick; /* start-up perturbation re-applied on reset (bluff-body scenes) */

    /* grid / snapshot mirror */
    int nx, ny, nz;
    SimSnapshot *snap;       /* main-thread reference for probing and picking */
    uint64_t prev_step;

    /* display field and range */
    int display_field;
    bool manual_range;
    double range_lo, range_hi; /* display units */
    float lat_lo, lat_hi;      /* applied lattice range (smoothed) */
    VisRange stats;            /* last field statistics (lattice units) */
    int stats_field, range_field; /* display field the statistics belong to / the colour range was set up for */
    bool have_field;
    float speed_hi;            /* smoothed lattice speed for particle colouring */

    /* visual options not in RenderSettings */
    int seed_mode, seed_count, stream_steps;
    uint32_t stream_lines, stream_verts, iso_tris; /* sizes of the last uploaded visualisation data */
    int emitter_mode; /* 0 smoke sheet through the model, 1 volume around the model, 2 full cross-section */
    /* streamline rake: centre and extents in cells; follows the model until the user moves it */
    vec3 rake_pos;
    float rake_size[2];
    int rake_axis;
    bool rake_manual, rake_drag;
    bool probe_on;
    vec3 probe;

    /* UI */
    bool hud_on;
    float panel_w;
    bool orbiting, panning;
    bool box_zoom; /* Cmd-drag rectangle zoom in progress */
    bool box_pick; /* BOX in the HOLD & LOAD step: the same rectangle, collecting faces instead of zooming */
    bool box_pick_remove;
    double box_x0, box_y0, box_x1, box_y1;
    double press_x, press_y;
    bool press_moved;
    bool model_drag;     /* Option-drag turning the model (Shift: roll) */
    double drag_push_t;  /* last time a drag sent the new attitude to the solver */
    int panel_tab;       /* CONTROLS tab: PANEL_FLOW, PANEL_MODEL or PANEL_TIME */
    int workspace;       /* WS_FLUID or WS_SOLID: which solver the interface is driving */
    int ui_mode;         /* UI_SIMPLE, UI_ADVANCED or UI_AGENT */
    bool first_run;      /* nothing was remembered: a new user, or --first-run */
    char scene_pending[64]; /* the tunnel scene, loaded the first time the flow workspace opens (Simple and Agent start without it) */
    bool fluid_layers[6];/* stream, particles, vortex, slice, volume, surface-field: kept while in the solid workspace */
    bool fluid_saved;
    double stream_ms, iso_ms, field_ms;

    /* frame timing and pacing */
    double playback;     /* fraction of full solver speed (slow motion below 1) */
    double max_fps;      /* render loop cap in a window, 0 = uncapped */
    double time, dt, fps;
    float particle_steps;
    double particle_debt; /* solver steps not yet applied to the particles */

    /* frame profiler: smoothed ms per phase, upload volume */
    double perf_events, perf_results, perf_render, perf_ui, perf_swap, perf_frame;
    double upload_mb_s, field_uploads_s;
    bool perf_sync; /* glFinish after GPU phases so their cost is attributed correctly */

    /* window */
    int win_w, win_h, fb_w, fb_h;
    float scale;
    bool headless;
    GLuint target_fbo;
    bool quit;
    char shot_path[1024];
    int shot_countdown;

    /* NAVIER-AM: headless engine (projects, geometry, FEM) shared with the control socket and MCP */
    struct Engine *engine;
    struct CtlServer *ctl; /* optional control socket (--listen) */
} App;

extern App app;

void app_push_params(void);
void app_update_model(void); /* recompute placement / bbox after params or mesh changes */
void app_mesh_changed(void); /* after editing app.mesh in place: re-upload, re-voxelise, re-place */
void app_restore_mesh(void); /* back to the mesh as loaded */
bool app_load_stl(const char *path);
bool app_load_shape(const char *name);
void app_unload_model(void);
bool app_apply_scene(const char *name);
void app_set_workspace(int ws);  /* switches the interface between the tunnel and the analysis */
const char *workspace_name(int ws);
void app_remember_workspace(int ws); /* keep the choice for the next launch (never in a headless run) */
void app_set_ui_mode(int mode);       /* switch Simple / Advanced / Agent, and remember it */
const char *ui_mode_name(int mode);
int app_remembered_ui_mode(void);     /* -1 when nothing was remembered */
bool app_resource_path(const char *name, char *out, size_t cap); /* bundle Resources, then the source tree */
const char *app_ui_state_get(const char *key, char *out, size_t cap); /* NULL when not remembered */

/* Agent mode: the user's own AI tool, pointed at this window's engine */
typedef struct AgentTool {
    char name[32];
    char path[1024];
    char command[2048]; /* a shell line; the prompt and the paths arrive as $NAVIER_PROMPT, $NAVIER_MCP_CONFIG ... */
} AgentTool;
int agent_detect(AgentTool *tools, int cap);
bool agent_start(const char *command, const char *workdir, const char *prompt, char *err, size_t cap);
void agent_poll(void);
void agent_stop(void);
bool agent_running(void);
const char *agent_record_path(void); /* the leaderboard record of the last run, "" before one ends */
int agent_line_count(void);
const char *agent_line_at(int i, int *kind); /* kind: 0 the agent, 1 an operation, 2 the app, 3 an error */
void app_ui_state_set(const char *key, const char *value);
int app_remembered_workspace(void);  /* -1 when nothing was remembered */
int workspace_find(const char *name); /* -1 when unknown */
void app_camera_preset(const char *name);
void app_request_screenshot(const char *path);
/* the current view at `scale` times the window, written as a PNG (EXPORT IMAGE) */
bool app_export_image(const char *path, int scale);
void app_mark_vis_dirty(void);

const char *display_field_name(int f);
const char *display_field_label(int f);
const char *display_field_unit(int f);
int display_field_find(const char *name);
int display_field_vis(int f);
double display_field_scale(int f, const SimUnits *u);
bool display_field_signed(int f);

const char *seed_mode_name(int m);
int seed_mode_find(const char *name);

bool app_probe_sample(vec3 p, float *rho, float u[3]);
/* synthetic mouse click (UI tests): move, press, hold for hold_frames, release */
/* both return the number of frames until the injected events have been delivered, 0 when the queue is full */
int app_inject_click(float x, float y, int hold_frames);
int app_inject_drag(float x0, float y0, float x1, float y1, uint32_t mods, int frames); /* UI testing */
int app_inject_scroll(float x, float y, float dy);                                     /* UI testing */
int app_inject_key(int key);                                                         /* UI testing */
bool app_rake_handle(float *px, float *py); /* streamline rake handle in window points, if shown */
bool app_pick(double mx, double my, vec3 *hit);

const char *const *scene_names(void);
void scene_list(void);

void register_commands(void);
void commands_update(void); /* advances queued script commands; call once per frame */
bool commands_pending(void); /* script commands still queued or waiting */
void hud_draw(void);
bool hud_pick_mode(void); /* the HOLD & LOAD step is open: a click on the part picks a face */
bool hud_box_mode(void);  /* BOX is armed: a drag collects every face whose centre falls inside the rectangle */
bool hud_simple_face_mode(void);                /* Simple mode is asking which face sits on the plate */
void hud_simple_face_clicked(float px, float py);
void hud_agent_set_command(const char *line); /* the command Agent mode runs */
void hud_simple_scroll(float dy);             /* a wheel turn over Simple mode's panel */
bool hud_box_front_only(void); /* FRONT ONLY: the box skips the faces turned away from the camera */
const char *hud_panel_text(void); /* every sentence the analysis panel wrote this frame, for tests to read */
/* a panel field is taking typed characters (a force, a unit); keys go there before the terminal or the hotkeys */
bool hud_text_focused(void);
void hud_text_key(uint32_t key);
const char *hud_text_value(void);
void format_si(char *buf, size_t cap, double v, const char *unit);
