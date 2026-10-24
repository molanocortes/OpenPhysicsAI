/* app.c - application helpers: display fields, model loading, scenes, camera presets, probing */
#include "app.h"
#include "core/paths.h"

#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include "colormap.h"
#include "common.h"
#include "fembridge.h"
#include "labapp.h"
#include "geom/shapes.h"

App app;

static const struct {
    const char *name, *label, *unit;
    int vis;
    bool sign;
} DF[DF_COUNT] = {
    [DF_SPEED] = {"speed", "Velocity magnitude |u|", "m/s", VIS_SPEED, false},
    [DF_UX] = {"ux", "Streamwise velocity u", "m/s", VIS_UX, true},
    [DF_UY] = {"uy", "Vertical velocity v", "m/s", VIS_UY, true},
    [DF_UZ] = {"uz", "Spanwise velocity w", "m/s", VIS_UZ, true},
    [DF_PRESSURE] = {"pressure", "Static pressure (gauge)", "Pa", VIS_PRESSURE, true},
    [DF_CP] = {"cp", "Pressure coefficient Cp", "", VIS_PRESSURE, true},
    [DF_VORTICITY] = {"vorticity", "Vorticity magnitude |\xCF\x89|", "1/s", VIS_VORTICITY, false},
    [DF_QCRIT] = {"q", "Q-criterion", "1/s\xC2\xB2", VIS_QCRITERION, true},
};

const char *display_field_name(int f) { return (f >= 0 && f < DF_COUNT) ? DF[f].name : "?"; }
const char *display_field_label(int f) { return (f >= 0 && f < DF_COUNT) ? DF[f].label : "?"; }
const char *display_field_unit(int f) { return (f >= 0 && f < DF_COUNT) ? DF[f].unit : ""; }
int display_field_vis(int f) { return (f >= 0 && f < DF_COUNT) ? DF[f].vis : VIS_SPEED; }
bool display_field_signed(int f) { return (f >= 0 && f < DF_COUNT) ? DF[f].sign : false; }

int display_field_find(const char *name) {
    for (int i = 0; i < DF_COUNT; i++)
        if (str_ieq(name, DF[i].name)) return i;
    if (str_ieq(name, "velocity") || str_ieq(name, "u") || str_ieq(name, "magnitude")) return DF_SPEED;
    if (str_ieq(name, "p")) return DF_PRESSURE;
    if (str_ieq(name, "vort") || str_ieq(name, "omega")) return DF_VORTICITY;
    if (str_ieq(name, "qcrit") || str_ieq(name, "qcriterion")) return DF_QCRIT;
    return -1;
}

double display_field_scale(int f, const SimUnits *u) {
    if (u->dt <= 0) return 1.0;
    switch (f) {
    case DF_SPEED:
    case DF_UX:
    case DF_UY:
    case DF_UZ: return units_speed(u);
    case DF_PRESSURE: return units_pressure(u);
    case DF_CP: return 1.0 / (0.5 * u->u_lb * u->u_lb);
    case DF_VORTICITY: return units_rate(u);
    case DF_QCRIT: return units_rate(u) * units_rate(u);
    default: return 1.0;
    }
}

static const char *SEED_NAMES[SEED_COUNT] = {"plane", "grid", "line", "wake", "random", "point"};
const char *seed_mode_name(int m) { return (m >= 0 && m < SEED_COUNT) ? SEED_NAMES[m] : "?"; }
int seed_mode_find(const char *name) {
    for (int i = 0; i < SEED_COUNT; i++)
        if (str_ieq(name, SEED_NAMES[i])) return i;
    return -1;
}

void format_si(char *buf, size_t cap, double v, const char *unit) {
    double a = fabs(v);
    const char *sp = unit && unit[0] ? " " : "";
    if (a == 0)
        snprintf(buf, cap, "0%s%s", sp, unit ? unit : "");
    else if (a >= 1e-2 && a < 1e5)
        snprintf(buf, cap, "%.4g%s%s", v, sp, unit ? unit : "");
    else
        snprintf(buf, cap, "%.3e%s%s", v, sp, unit ? unit : "");
}

void app_mark_vis_dirty(void) { app.req.version++; }

void app_update_model(void) {
    app.nx = app.params.nx, app.ny = app.params.ny, app.nz = app.params.nz;
    if (!app.has_model) {
        app.model_lo = v3(app.nx * 0.3f, app.ny * 0.4f, app.nz * 0.4f);
        app.model_hi = v3(app.nx * 0.35f, app.ny * 0.6f, app.nz * 0.6f);
        app.model_M = m4_identity();
    } else {
        app.model_M = sim_model_transform(&app.mesh, &app.params, NULL);
        vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
        for (uint32_t i = 0; i < 3 * app.mesh.tri_count; i++) {
            vec3 p = m4_mul_point(app.model_M, app.mesh.pos[i]);
            lo = v3_min(lo, p), hi = v3_max(hi, p);
        }
        app.model_lo = lo, app.model_hi = hi;
    }
    for (int a = 0; a < 3; a++) {
        app.req.box_lo[a] = a == 0 ? app.model_lo.x : a == 1 ? app.model_lo.y : app.model_lo.z;
        app.req.box_hi[a] = a == 0 ? app.model_hi.x : a == 1 ? app.model_hi.y : app.model_hi.z;
    }
    app.req.has_model = app.has_model;
    app_mark_vis_dirty();
}

void app_push_params(void) {
    sim_set_params(app.sim, &app.params);
    app_update_model();
}

static void install_mesh(Mesh *m, const char *label, const char *path) {
    MeshStats st;
    mesh_compute_stats(m, &st);
    mesh_free(&app.mesh);
    app.mesh = *m;
    mesh_free(&app.mesh_orig);
    mesh_init(&app.mesh_orig);
    mesh_append(&app.mesh_orig, &app.mesh);
    app.mesh_orig.bmin = app.mesh.bmin, app.mesh_orig.bmax = app.mesh.bmax;
    app.has_model = app.mesh.tri_count > 0;
    str_copy(app.model_name, sizeof app.model_name, label);
    str_copy(app.model_path, sizeof app.model_path, path ? path : "");
    render_set_mesh(app.renderer, &app.mesh);
    sim_set_mesh(app.sim, &app.mesh);
    vec3 e = v3_sub(app.mesh.bmax, app.mesh.bmin);
    LOGOK("model '%s': %u triangles, extent %.3g x %.3g x %.3g, %s", label, app.mesh.tri_count, e.x, e.y, e.z,
          st.watertight ? "watertight" : "NOT watertight");
    if (!st.watertight) {
        LOGW("%u open / %u non-manifold edges - using a thin-surface voxel shell to keep it leak-free", st.open_edges,
             st.nonmanifold_edges);
        if (app.params.shell < 0.5) app.params.shell = 0.5;
    }
    app_push_params();
}

bool app_load_stl(const char *path) {
    Mesh m;
    char err[256];
    mesh_init(&m);
    double t0 = now_seconds();
    if (!mesh_load_stl(path, &m, err, sizeof err)) {
        LOGE("could not load '%s': %s", path, err);
        return false;
    }
    LOGI("loaded %s in %.0f ms", path, (now_seconds() - t0) * 1e3);
    const char *base = strrchr(path, '/');
    install_mesh(&m, m.name[0] ? m.name : (base ? base + 1 : path), path);
    app.scene[0] = 0;
    app.scene_kick = 0;
    app.panel_tab = PANEL_MODEL; /* a new file usually needs its orientation fixed */
    return true;
}

bool app_load_shape(const char *name) {
    Mesh m;
    mesh_init(&m);
    if (!shape_generate(name, &m)) {
        LOGE("unknown shape '%s'", name);
        return false;
    }
    install_mesh(&m, m.name[0] ? m.name : name, NULL);
    return true;
}

void app_mesh_changed(void) {
    render_set_mesh(app.renderer, &app.mesh);
    sim_set_mesh(app.sim, &app.mesh);
    app_update_model();
}

void app_restore_mesh(void) {
    if (!app.mesh_orig.tri_count) return;
    Mesh m;
    mesh_init(&m);
    mesh_append(&m, &app.mesh_orig);
    m.bmin = app.mesh_orig.bmin, m.bmax = app.mesh_orig.bmax;
    str_copy(m.name, sizeof m.name, app.mesh.name);
    mesh_free(&app.mesh);
    app.mesh = m;
    app_mesh_changed();
}

void app_unload_model(void) {
    mesh_free(&app.mesh);
    mesh_free(&app.mesh_orig);
    mesh_init(&app.mesh_orig);
    app.has_model = false;
    app.model_name[0] = 0;
    app.scene[0] = 0;
    app.scene_kick = 0;
    render_set_mesh(app.renderer, NULL);
    sim_set_mesh(app.sim, NULL);
    app_update_model();
    LOGI("model removed - empty tunnel");
}

static const char *const WS_NAME[WS_COUNT] = {"fluid", "solid"};

const char *workspace_name(int ws) { return (ws >= 0 && ws < WS_COUNT) ? WS_NAME[ws] : "?"; }

/* Which half of the application the window opened in last time. One line in ~/.openphysicsai/app/ui-state, written when the
 * user switches and read at startup, so an engineer who works on parts does not land in the water tunnel every
 * morning. A headless run never reads it: a script says what it wants, and a test must not depend on a file in the
 * home directory. Until the first write after the rename (2026-10-21) the settings are read from ~/.navier/ui-state. */
static bool ui_state_path(char *out, size_t cap) {
    if (!op_state_dir(out, cap, "app")) return false;
    path_mkdirs(out); /* with its parent; they may exist already, the write is what actually matters */
    size_t n = strlen(out);
    return (size_t)snprintf(out + n, cap - n, "/ui-state") < cap - n;
}

static bool ui_state_read_path(char *out, size_t cap) {
    if (!op_state_dir(out, cap, "app/ui-state")) return false;
    char old[1024];
    if (!path_exists(out) && op_legacy_state_path(old, sizeof old, "ui-state") && strlen(old) < cap) snprintf(out, cap, "%s", old);
    return true;
}

/* ~/.openphysicsai/app/ui-state holds "key value" lines: the workspace, the mode, the agent's command. A headless run reads and
 * writes none of it, so a script decides for itself and no test depends on the home directory. */
#define UI_STATE_KEYS 16
static bool ui_state_read(char keys[UI_STATE_KEYS][32], char vals[UI_STATE_KEYS][1024], int *n) {
    char path[1024], line[1200];
    *n = 0;
    if (!ui_state_read_path(path, sizeof path)) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    while (fgets(line, sizeof line, f) && *n < UI_STATE_KEYS) {
        line[strcspn(line, "\r\n")] = 0;
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        snprintf(keys[*n], 32, "%s", line);
        snprintf(vals[*n], 1024, "%s", sp + 1);
        (*n)++;
    }
    fclose(f);
    return true;
}

const char *app_ui_state_get(const char *key, char *out, size_t cap) {
    char keys[UI_STATE_KEYS][32], vals[UI_STATE_KEYS][1024];
    int n;
    if (out && cap) out[0] = 0;
    if (app.headless || !ui_state_read(keys, vals, &n)) return NULL;
    for (int i = 0; i < n; i++)
        if (!strcmp(keys[i], key)) {
            snprintf(out, cap, "%s", vals[i]);
            return out;
        }
    return NULL;
}

void app_ui_state_set(const char *key, const char *value) {
    char keys[UI_STATE_KEYS][32], vals[UI_STATE_KEYS][1024], path[1024];
    int n = 0;
    if (app.headless || !ui_state_path(path, sizeof path)) return;
    ui_state_read(keys, vals, &n);
    int at = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(keys[i], key)) at = i;
    if (at < 0 && n < UI_STATE_KEYS) at = n++;
    if (at < 0) return;
    snprintf(keys[at], 32, "%s", key);
    snprintf(vals[at], 1024, "%s", value);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s %s\n", keys[i], vals[i]);
    fclose(f);
}

void app_remember_workspace(int ws) { app_ui_state_set("workspace", workspace_name(ws)); }

int app_remembered_workspace(void) {
    char v[64];
    if (app.first_run || !app_ui_state_get("workspace", v, sizeof v)) return -1;
    for (int i = 0; i < WS_COUNT; i++)
        if (!strcmp(v, workspace_name(i))) return i;
    return -1;
}

static const char *const UI_MODE_NAMES[UI_MODE_COUNT] = {"simple", "advanced", "agent"};
const char *ui_mode_name(int mode) { return mode >= 0 && mode < UI_MODE_COUNT ? UI_MODE_NAMES[mode] : "?"; }

int app_remembered_ui_mode(void) {
    char v[64];
    if (app.first_run || !app_ui_state_get("mode", v, sizeof v)) return -1;
    if (!strcmp(v, "manual")) return UI_ADVANCED;
    if (!strcmp(v, "agentic")) return UI_AGENT;
    for (int i = 0; i < UI_MODE_COUNT; i++)
        if (!strcmp(v, UI_MODE_NAMES[i])) return i;
    return -1;
}

/* Manual and Agentic share the current simulation; the legacy Simple workflow is solid-only. */
void app_set_ui_mode(int mode) {
    if (mode < 0 || mode >= UI_MODE_COUNT) return;
    app.ui_mode = mode;
    if (mode == UI_SIMPLE && app.workspace != WS_SOLID) app_set_workspace(WS_SOLID);
    app_ui_state_set("mode", UI_MODE_NAMES[mode]);
    fem_set_supports_neutral(mode == UI_SIMPLE); /* Simple draws the supports grey, Advanced in colour */
}

/* A file shipped with the application: the bundle's Resources folder when running from OpenPhysicsAI.app, else the source
 * tree the binary was built in (the binary sits at its root), else the working directory. */
bool app_resource_path(const char *name, char *out, size_t cap) {
    char exe[1024] = "";
    uint32_t sz = sizeof exe;
    extern int _NSGetExecutablePath(char *buf, uint32_t *bufsize);
    if (_NSGetExecutablePath(exe, &sz) == 0) {
        char *slash = strrchr(exe, '/');
        if (slash) *slash = 0;
        const char *cands[2] = {"%s/../Resources/%s", "%s/%s"};
        for (int i = 0; i < 2; i++) {
            snprintf(out, cap, cands[i], exe, name);
            if (access(out, R_OK) == 0) return true;
        }
    }
    snprintf(out, cap, "%s", name);
    return access(out, R_OK) == 0;
}


int workspace_find(const char *name) {
    for (int i = 0; i < WS_COUNT; i++)
        if (str_ieq(name, WS_NAME[i])) return i;
    return -1;
}

/* The two solvers share this window, camera and colour maps, so switching hides the other one's layers instead of
 * drawing both at once. The flow keeps its state; it is only paused, because a finite-element solve wants the cores. */
void app_set_workspace(int ws) {
    if (ws < 0 || ws >= WS_COUNT || ws == app.workspace) return;
    RenderSettings *rs = &app.rs;
    if (ws == WS_SOLID) {
        app.fluid_layers[0] = rs->stream_on, app.fluid_layers[1] = rs->particles_on;
        app.fluid_layers[2] = rs->vortex_on, app.fluid_layers[3] = rs->slice_on;
        app.fluid_layers[4] = rs->volume_on, app.fluid_layers[5] = rs->surface_mode == SURFACE_FIELD;
        app.fluid_saved = true;
        rs->stream_on = rs->particles_on = rs->vortex_on = rs->slice_on = rs->volume_on = false;
        rs->surface_mode = SURFACE_HIDDEN;
        rs->result_on = true;
        if (app.status.running) {
            sim_run(app.sim, false);
            LOGI("flow paused while the analysis workspace is open");
        }
        app.workspace = ws;
        app_remember_workspace(ws);
        fem_mark_dirty();
        LOGOK("SOLID workspace - finite-element analysis (the same operations as 'am' and the control socket)");
    } else {
        if (app.scene_pending[0]) { /* the tunnel was not loaded at start: load it now */
            char sc[64];
            snprintf(sc, sizeof sc, "%s", app.scene_pending);
            app.scene_pending[0] = 0;
            app_apply_scene(sc);
        }
        rs->result_on = false;
        if (app.fluid_saved) {
            rs->stream_on = app.fluid_layers[0], rs->particles_on = app.fluid_layers[1];
            rs->vortex_on = app.fluid_layers[2], rs->slice_on = app.fluid_layers[3];
            rs->volume_on = app.fluid_layers[4];
            rs->surface_mode = app.fluid_layers[5] ? SURFACE_FIELD : SURFACE_MATERIAL;
        } else {
            rs->surface_mode = SURFACE_MATERIAL;
        }
        app.workspace = ws;
        app_remember_workspace(ws);
        app_mark_vis_dirty();
        LOGOK("FLUID workspace - lattice-Boltzmann tunnel");
    }
}

void app_camera_preset(const char *name) {
    vec3 c = v3(app.nx * 0.5f, app.ny * 0.5f, app.nz * 0.5f);
    float d = MAXI((float)app.nx, MAXI((float)app.ny, (float)app.nz)) * 1.35f;
    Camera *cam = &app.cam;
    if (str_ieq(name, "model") && app.has_model) {
        vec3 mc = v3_scale(v3_add(app.model_lo, app.model_hi), 0.5f);
        float ext = v3_len(v3_sub(app.model_hi, app.model_lo));
        camera_focus(cam, mc, MAXI(ext * 1.6f, 10.0f));
        return;
    }
    if (str_ieq(name, "hero") && app.has_model) {
        /* three-quarter view from above and behind the model, wake in frame */
        vec3 e = v3_sub(app.model_hi, app.model_lo);
        vec3 mc = v3_scale(v3_add(app.model_lo, app.model_hi), 0.5f);
        float ext = MAXI(e.x, MAXI(e.y, e.z));
        cam->locked = false;
        camera_focus(cam, v3(mc.x + 0.3f * ext, mc.y, mc.z), MAXI(ext * 2.1f, 16.0f));
        camera_set_angles(cam, 220, 34);
        return;
    }
    cam->locked = false;
    camera_focus(cam, c, d);
    if (str_ieq(name, "front")) camera_set_angles(cam, 180, 8);
    else if (str_ieq(name, "back")) camera_set_angles(cam, 0, 8);
    else if (str_ieq(name, "side")) camera_set_angles(cam, 90, 2);
    else if (str_ieq(name, "top")) camera_set_angles(cam, 90, 89), camera_focus(cam, c, d * 0.95f);
    else if (str_ieq(name, "bottom")) camera_set_angles(cam, 90, -89);
    else camera_set_angles(cam, 212, 32), camera_focus(cam, v3(c.x * 0.9f, c.y * 0.8f, c.z), d * 0.95f);
}

void app_request_screenshot(const char *path) {
    str_copy(app.shot_path, sizeof app.shot_path, path);
    app.shot_countdown = 1;
}

typedef struct {
    const char *name, *shape, *fluid;
    int nx, ny, nz;
    double speed, length, fit, fit_len, px, py, pz, aoa, temp, turb;
    int walls[4];
    int field, cmap, line_cmap, seed_mode, seeds;
    bool vortices, slice, streams, particles;
    int slice_axis;
    const char *cam;
    const char *description;
    double kick;   /* start-up cross-flow pulse amplitude (fraction of U), 0 = none */
    int ref_axis;  /* extent that defines the reference length; default REF_AXIS_X (along the flow) */
} Scene;

#define W_S LBM_WALL_SLIP
#define W_N LBM_WALL_NOSLIP
#define W_M LBM_WALL_MOVING
#define W_P LBM_WALL_PERIODIC

/* Bluff bodies sit half a cell off the centre line and see 0.5% inlet turbulence: a perfectly symmetric
 * lattice would otherwise hold an unstable symmetric wake for a very long time before shedding. */
static const Scene SCENES[] = {
    {"glider", "glider", "water", 192, 64, 224, 0.8, 0.30, 0.78, 0.42, 0.30, 0.5, 0.5, 4, 20, 0.01, {W_S, W_S, W_S, W_S},
     DF_SPEED, CMAP_STUDIO, CMAP_STUDIO, SEED_PLANE, 420, true, false, true, false, 1, "hero",
     "sailplane at 4 deg AoA - wingtip vortices"},
    {"sphere", "sphere", "water", 192, 96, 96, 0.015, 0.02, 0.26, 0.3, 0.28, 0.503, 0.5, 0, 20, 0.005, {W_S, W_S, W_S, W_S},
     DF_VORTICITY, CMAP_STUDIO, CMAP_STUDIO, SEED_GRID, 400, true, true, true, false, 2, "iso",
     "sphere at Re~300 - laminar hairpin vortex shedding", 0.12},
    {"cylinder", "cylinder", "water", 320, 160, 80, 0.015, 0.01, 1.0, 0.5, 0.25, 0.503, 0.5, 0, 20, 0.005, {W_S, W_S, W_P, W_P},
     DF_UY, CMAP_COOLWARM, CMAP_STUDIO, SEED_LINE, 150, false, true, true, false, 2, "side",
     "circular cylinder at Re~150 - Karman vortex street", 0.12},
    {"car", "ahmed", "air", 240, 72, 120, 40.0, 1.044, 0.55, 0.3, 0.3, 0.187, 0.5, 0, 20, 0.01, {W_M, W_S, W_S, W_S},
     DF_CP, CMAP_COOLWARM, CMAP_STUDIO, SEED_GRID, 360, true, false, true, false, 1, "iso",
     "Ahmed body (25 deg slant) on a rolling road in air"},
    {"submarine", "submarine", "seawater", 256, 80, 80, 2.0, 1.0, 0.5, 0.45, 0.35, 0.5, 0.5, 0, 12, 0.01, {W_S, W_S, W_S, W_S},
     DF_PRESSURE, CMAP_COOLWARM, CMAP_STUDIO, SEED_GRID, 300, false, false, true, true, 1, "hero",
     "SUBOFF-like hull in sea water"},
    {"wing", "wing", "water", 192, 80, 240, 1.0, 0.1, 0.8, 0.3, 0.3, 0.5, 0.5, 6, 20, 0.01, {W_S, W_S, W_S, W_S},
     DF_SPEED, CMAP_STUDIO, CMAP_STUDIO, SEED_PLANE, 360, true, false, true, false, 1, "hero",
     "tapered NACA 2412 wing at 6 deg - tip vortices"},
    {"airfoil", "airfoil", "water", 256, 128, 48, 0.5, 0.1, 1.0, 0.25, 0.3, 0.503, 0.5, 8, 20, 0.005, {W_S, W_S, W_P, W_P},
     DF_VORTICITY, CMAP_STUDIO, CMAP_STUDIO, SEED_LINE, 160, false, true, true, false, 2, "side",
     "NACA 0012 section at 8 deg (spanwise periodic)", 0.08},
    {"plate", "plate", "water", 192, 96, 96, 0.2, 0.1, 0.4, 0.3, 0.3, 0.5, 0.5, 0, 20, 0.01, {W_S, W_S, W_S, W_S},
     DF_VORTICITY, CMAP_STUDIO, CMAP_STUDIO, SEED_GRID, 300, true, false, true, true, 1, "iso",
     "flat plate normal to the flow - bluff body wake", 0.12, REF_AXIS_Y},
    {"torus", "torus", "water", 192, 96, 96, 0.1, 0.1, 0.45, 0.3, 0.3, 0.5, 0.5, 0, 20, 0.01, {W_S, W_S, W_S, W_S},
     DF_SPEED, CMAP_HYDRO, -1, SEED_GRID, 360, true, false, true, true, 1, "iso",
     "ring (torus) aligned with the flow"},
};

const char *const *scene_names(void) {
    static const char *names[ARRAY_LEN(SCENES) + 1];
    for (size_t i = 0; i < ARRAY_LEN(SCENES); i++) names[i] = SCENES[i].name;
    names[ARRAY_LEN(SCENES)] = NULL;
    return names;
}

void scene_list(void) {
    for (size_t i = 0; i < ARRAY_LEN(SCENES); i++) LOGD("  %-10s %s", SCENES[i].name, SCENES[i].description);
}

bool app_apply_scene(const char *name) {
    const Scene *sc = NULL;
    for (size_t i = 0; i < ARRAY_LEN(SCENES); i++)
        if (str_ieq(name, SCENES[i].name)) sc = &SCENES[i];
    if (!sc) {
        LOGE("unknown scene '%s'. Scenes:", name);
        scene_list();
        return false;
    }
    sim_run(app.sim, false);
    SimParams *p = &app.params;
    p->fluid = fluid_find(sc->fluid);
    p->temperature = sc->temp;
    p->speed = sc->speed;
    p->ref_length = sc->length;
    p->ref_axis = sc->ref_axis;
    p->nx = sc->nx, p->ny = sc->ny, p->nz = sc->nz;
    p->fit = sc->fit, p->fit_length = sc->fit_len;
    p->pos[0] = sc->px, p->pos[1] = sc->py, p->pos[2] = sc->pz;
    p->aoa = sc->aoa, p->yaw = 0, p->roll = 0;
    p->roughness = 0;
    p->turbulence = sc->turb;
    for (int i = 0; i < 4; i++) p->wall[i] = sc->walls[i];
    p->shell = 0.5;
    app.display_field = sc->field;
    app.rs.field = display_field_vis(sc->field);
    app.rs.cmap = sc->cmap;
    render_set_colormap(app.renderer, sc->cmap);
    app.manual_range = false;
    app.seed_mode = sc->seed_mode;
    app.seed_count = sc->seeds;
    app.rs.vortex_on = sc->vortices;
    app.rs.slice_on = sc->slice;
    app.rs.slice_axis = sc->slice_axis;
    app.rs.slice_pos = 0.5f;
    app.rs.stream_on = sc->streams;
    app.rs.particles_on = sc->particles;
    app.rs.line_cmap = sc->line_cmap;
    render_set_line_colormap(app.renderer, sc->line_cmap);
    str_copy(app.scene, sizeof app.scene, sc->name);
    Mesh m;
    mesh_init(&m);
    if (!shape_generate(sc->shape, &m)) {
        LOGE("shape '%s' unavailable", sc->shape);
        return false;
    }
    str_copy(app.model_name, sizeof app.model_name, sc->shape);
    install_mesh(&m, sc->shape, NULL);
    sim_reset(app.sim);
    app.scene_kick = sc->kick;
    if (sc->kick > 0) sim_kick(app.sim, sc->kick, p->start_with_flow ? 0 : p->ramp_steps);
    render_reset_particles(app.renderer);
    app_camera_preset(sc->cam);
    LOGOK("scene '%s': %s", sc->name, sc->description);
    return true;
}

bool app_probe_sample(vec3 p, float *rho, float u[3]) {
    SimSnapshot *s = app.snap;
    if (!s) return false;
    VisGrid g = {s->nx, s->ny, s->nz, s->rho, s->ux, s->uy, s->uz, s->solid};
    bool ok = vis_sample_velocity(&g, p.x, p.y, p.z, u);
    *rho = vis_sample_scalar(s->rho, s->nx, s->ny, s->nz, p.x, p.y, p.z);
    return ok;
}

/* Ray cast in lattice space against the solid voxels and the active slice plane. */
bool app_pick(double mx, double my, vec3 *hit) {
    SimSnapshot *s = app.snap;
    vec3 o, d;
    camera_ray(&app.cam, (float)(mx * app.scale), (float)(my * app.scale), &o, &d);
    float best = 1e30f;
    bool found = false;
    vec3 N = v3((float)app.nx, (float)app.ny, (float)app.nz);
    if (app.rs.slice_on) {
        int a = app.rs.slice_axis;
        float plane = app.rs.slice_pos * (a == 0 ? N.x : a == 1 ? N.y : N.z);
        float oc = a == 0 ? o.x : a == 1 ? o.y : o.z, dc = a == 0 ? d.x : a == 1 ? d.y : d.z;
        if (fabsf(dc) > 1e-6f) {
            float t = (plane - oc) / dc;
            vec3 p = v3_add(o, v3_scale(d, t));
            if (t > 0 && p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x <= N.x && p.y <= N.y && p.z <= N.z) best = t, found = true;
        }
    }
    if (s && s->nx == app.nx && s->ny == app.ny && s->nz == app.nz) {
        /* clip to the domain box */
        float t0 = 0, t1 = 1e30f;
        float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z}, nn[3] = {N.x, N.y, N.z};
        for (int a = 0; a < 3; a++) {
            if (fabsf(dd[a]) < 1e-9f) {
                if (oo[a] < 0 || oo[a] > nn[a]) t1 = -1;
                continue;
            }
            float ta = (0 - oo[a]) / dd[a], tb = (nn[a] - oo[a]) / dd[a];
            if (ta > tb) { float tmp = ta; ta = tb; tb = tmp; }
            t0 = MAXI(t0, ta), t1 = MINI(t1, tb);
        }
        if (t0 < t1 && t0 < best) {
            float t = t0 + 1e-3f;
            int ci[3], step[3];
            float tmax[3], tdelta[3];
            for (int a = 0; a < 3; a++) {
                float p = oo[a] + dd[a] * t;
                ci[a] = CLAMP((int)floorf(p), 0, (int)nn[a] - 1);
                step[a] = dd[a] >= 0 ? 1 : -1;
                float next = dd[a] >= 0 ? (float)(ci[a] + 1) : (float)ci[a];
                tdelta[a] = fabsf(dd[a]) > 1e-9f ? fabsf(1.0f / dd[a]) : 1e30f;
                tmax[a] = fabsf(dd[a]) > 1e-9f ? t + (next - p) / dd[a] : 1e30f;
            }
            for (int it = 0; it < 4 * (app.nx + app.ny + app.nz); it++) {
                size_t id = (size_t)ci[0] + (size_t)s->nx * ((size_t)ci[1] + (size_t)s->ny * (size_t)ci[2]);
                if (s->solid[id]) {
                    if (t < best) best = t, found = true;
                    break;
                }
                int a = tmax[0] < tmax[1] ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
                t = tmax[a];
                if (t > best || t > t1) break;
                ci[a] += step[a];
                if (ci[a] < 0 || ci[a] >= (int)nn[a]) break;
                tmax[a] += tdelta[a];
            }
        }
    }
    if (found) *hit = v3_add(o, v3_scale(d, best));
    return found;
}

/* =================================================================================================================
 * AGENT MODE: the user's own AI tool, driving this window's engine through the Model Context Protocol.
 *
 * The app holds no key and speaks to no AI service. It finds the command-line tools that are installed, writes an MCP
 * configuration that points the chosen one at this window's engine (a private control socket), passes the user's
 * words as the prompt, and shows what comes back. Every job the agent starts is followed automatically, because the
 * control socket being open is what makes the window adopt jobs started elsewhere. STOP ends the child.
 * ================================================================================================================= */

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include "ctl/engine_internal.h"
#include "core/base64.h"
#include "core/json.h"
#include "net/ctlserver.h"

extern char **environ;

#define AGENT_LINES 400
static struct {
    pid_t pid;
    int fd;
    char *pending;          /* one line of the agent's output; a tool result can be hundreds of kilobytes */
    size_t pending_len, pending_cap;
    char lines[AGENT_LINES][400];
    unsigned char kind[AGENT_LINES]; /* 0 the agent, 1 an operation, 2 the app, 3 an error */
    int nlines, head;
    char socket_path[512];
    char mcp_path[1024];
    uint64_t journal_since;
    double started;
    int exit_code;
    bool exited;
    /* what the run record needs: said by the tool itself in its stream-json events, never guessed */
    char prompt[6144], command[4096];
    char model_name[128], model_id[128], tool_version[64];
    long long turns;
    double cost_usd, wall_start;
    bool have_turns, have_cost, stopped, recorded, is_error;
    char answer[1200];
    char record_path[1024];
    /* a conversation: Claude Code's own session protocol (-p --input-format stream-json), one process answering every
     * question in turn, its words streamed as they come (--include-partial-messages) for the headset to speak */
    bool session, busy;
    int in_fd;                 /* the session's stdin: one JSON message a line */
    char session_id[80];       /* as the tool announced it, to resume the conversation if its process ends */
    char session_cmd[4096];    /* the command it runs, its --model taken out: a question with another needs another */
    char session_dir[1024];
    char model_now[64];        /* the model it was started with or switched to (set_model) */
    int turn;                  /* questions asked in this window */
    char partial[2400];        /* the words of the current answer so far */
    bool partial_done;
    double t_asked, t_first;   /* wall seconds: the question sent, its first words back */
    double stop_at;            /* an interrupt sent: the process is ended if the turn has not ended by then */
    char picture[1024];        /* a JPEG to send with the next question: what the person saw as they asked */
} AG = {.pid = -1, .fd = -1, .in_fd = -1};

/* a buffer cut by snprintf can end half way through a UTF-8 character: drop the partial one */
static void utf8_trim_end(char *s) {
    size_t n = strlen(s), i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--; /* continuation bytes */
    if (i > 0 && ((unsigned char)s[i - 1] & 0x80)) {
        size_t lead = i - 1;
        if (utf8_sequence_length((const unsigned char *)s + lead, n - lead) == 0) s[lead] = 0;
    }
}

static void agent_line(int kind, const char *fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    utf8_trim_end(buf);
    int at = (AG.head + AG.nlines) % AGENT_LINES;
    if (AG.nlines == AGENT_LINES) AG.head = (AG.head + 1) % AGENT_LINES;
    else AG.nlines++;
    snprintf(AG.lines[at], sizeof AG.lines[at], "%s", buf);
    AG.kind[at] = (unsigned char)kind;
    LOGI("agent: %s", buf); /* the terminal keeps the same record, so a test and a person read the same thing */
}

int agent_line_count(void) { return AG.nlines; }
const char *agent_line_at(int i, int *kind) {
    if (i < 0 || i >= AG.nlines) return "";
    int at = (AG.head + i) % AGENT_LINES;
    if (kind) *kind = AG.kind[at];
    return AG.lines[at];
}
static bool agent_alive(void) { return AG.pid > 0 && !AG.exited; }
/* answering a question: the whole run for a one-shot command, the current turn for a conversation */
bool agent_running(void) { return agent_alive() && (!AG.session || AG.busy); }

/* the current answer's words so far, its turn (0 before the first question) and whether it has ended */
const char *agent_turn_words(int *turn, bool *done) {
    if (turn) *turn = AG.turn;
    if (done) *done = AG.partial_done || !agent_running();
    return AG.partial;
}
const char *agent_session_id(void) { return AG.session_id; }
const char *agent_turn_prompt(void) { return AG.prompt; } /* the question of the current turn, with its context */
/* seconds from the question to its first words, for the turn just asked (-1 before they come) */
double agent_first_words_s(void) { return AG.t_first > 0 ? AG.t_first - AG.t_asked : -1; }

/* ---- which tools are here ---- */

static bool find_on_path(const char *name, char *out, size_t cap) {
    const char *home = getenv("HOME");
    char extra[1024];
    snprintf(extra, sizeof extra, "%s/.local/bin:/opt/homebrew/bin:/usr/local/bin", home ? home : "");
    const char *paths[2] = {getenv("PATH"), extra};
    for (int k = 0; k < 2; k++) {
        if (!paths[k]) continue;
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", paths[k]);
        for (char *dir = strtok(buf, ":"); dir; dir = strtok(NULL, ":")) {
            snprintf(out, cap, "%s/%s", dir, name);
            if (access(out, X_OK) == 0) return true;
        }
    }
    return false;
}

int agent_detect(AgentTool *tools, int cap) {
    int n = 0;
    char path[1024];
    if (n < cap && find_on_path("claude", path, sizeof path)) {
        AgentTool *t = &tools[n++];
        snprintf(t->name, sizeof t->name, "Claude Code");
        snprintf(t->path, sizeof t->path, "%s", path);
        snprintf(t->command, sizeof t->command,
                 "\"%s\" -p \"$OPENPHYSICS_PROMPT\" --mcp-config \"$OPENPHYSICS_MCP_CONFIG\" --strict-mcp-config "
                 "--allowedTools mcp__openphysics --output-format stream-json --verbose",
                 path);
    }
    if (n < cap && find_on_path("codex", path, sizeof path)) {
        AgentTool *t = &tools[n++];
        snprintf(t->name, sizeof t->name, "Codex");
        snprintf(t->path, sizeof t->path, "%s", path);
        snprintf(t->command, sizeof t->command,
                 "\"%s\" exec -c \"mcp_servers.openphysics.command=\\\"$OPENPHYSICS_MCP_COMMAND\\\"\" "
                 "-c \"mcp_servers.openphysics.args=[\\\"--connect\\\",\\\"$OPENPHYSICS_SOCKET\\\"]\" \"$OPENPHYSICS_PROMPT\"",
                 path);
    }
    return n;
}

/* ---- the models and efforts each tool offers ---- */

/* which tool a command line runs, by its programs' names (not by any folder on the way): 0 Claude Code, 1 Codex, -1 another */
int agent_tool_kind(const char *command) {
    const char *p = command;
    while (*p) {
        char tok[1024];
        size_t n = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) { if (n + 1 < sizeof tok) tok[n++] = *p; p++; }
            if (*p) p++;
        } else
            while (*p && *p != ' ' && *p != '\t') { if (n + 1 < sizeof tok) tok[n++] = *p; p++; }
        tok[n] = 0;
        const char *b = strrchr(tok, '/') ? strrchr(tok, '/') + 1 : tok;
        if (b[0] == '-' || b[0] == '$') continue;
        if (strstr(b, "claude")) return 0;
        if (strstr(b, "codex")) return 1;
    }
    return -1;
}

static int add_choice(AgentChoice *out, int n, int cap, const char *value, const char *label, const char *desc) {
    if (n >= cap) return n;
    snprintf(out[n].value, sizeof out[n].value, "%s", value);
    snprintf(out[n].label, sizeof out[n].label, "%s", label);
    snprintf(out[n].desc, sizeof out[n].desc, "%s", desc);
    utf8_trim_end(out[n].desc);
    return n + 1;
}

static void codex_home(char *out, size_t cap) {
    const char *ch = getenv("CODEX_HOME"), *home = getenv("HOME");
    if (ch && ch[0]) snprintf(out, cap, "%s", ch);
    else snprintf(out, cap, "%s/.codex", home ? home : "");
}

/* model = "..." and model_reasoning_effort = "..." at the top of Codex's config.toml (before the first [table]) */
static void codex_config(char *model, size_t mcap, char *effort, size_t ecap) {
    char path[1100], line[512];
    codex_home(path, sizeof path);
    strncat(path, "/config.toml", sizeof path - strlen(path) - 1);
    model[0] = effort[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') break;
        char *eq = strchr(p, '='), *q0, *q1;
        if (!eq || !(q0 = strchr(eq, '"')) || !(q1 = strchr(q0 + 1, '"'))) continue;
        size_t klen = (size_t)(eq - p);
        while (klen && (p[klen - 1] == ' ' || p[klen - 1] == '\t')) klen--;
        int vlen = (int)(q1 - q0 - 1);
        if (klen == 5 && !strncmp(p, "model", 5)) snprintf(model, mcap, "%.*s", vlen, q0 + 1);
        else if (klen == 22 && !strncmp(p, "model_reasoning_effort", 22)) snprintf(effort, ecap, "%.*s", vlen, q0 + 1);
    }
    fclose(f);
}

static JsonValue *codex_models(void) {
    char path[1100];
    codex_home(path, sizeof path);
    strncat(path, "/models_cache.json", sizeof path - strlen(path) - 1);
    return json_read_file(path, 8u << 20, NULL);
}

static const char *effort_label(const char *e) {
    static const char *const names[][2] = {{"none", "None"}, {"minimal", "Minimal"}, {"low", "Low"}, {"medium", "Medium"},
                                           {"high", "High"}, {"xhigh", "Extra high"}, {"max", "Max"}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(e, names[i][0])) return names[i][1];
    static char other[32]; /* a level this list does not know yet ("ultra"): its own word, capitalised */
    snprintf(other, sizeof other, "%c%s", toupper((unsigned char)e[0]), e[0] ? e + 1 : "");
    return other;
}

int agent_model_choices(const char *command, AgentChoice *out, int cap, char *def, size_t dcap) {
    int n = 0;
    if (def && dcap) def[0] = 0;
    int kind = agent_tool_kind(command);
    if (kind == 0) { /* Claude Code's /model menu, then the extra models it caches for this account */
        n = add_choice(out, n, cap, "", "Default", "Claude Code's own choice");
        n = add_choice(out, n, cap, "opus", "Opus", "Most capable, for complex work");
        n = add_choice(out, n, cap, "sonnet", "Sonnet", "Best for everyday tasks");
        n = add_choice(out, n, cap, "haiku", "Haiku", "Fastest, for quick answers");
        const char *home = getenv("HOME");
        char path[1100];
        snprintf(path, sizeof path, "%s/.claude.json", home ? home : "");
        JsonValue *j = json_read_file(path, 32u << 20, NULL);
        const JsonValue *extra = json_get(j, "additionalModelOptionsCache");
        for (size_t i = 0; i < json_len(extra); i++) {
            const JsonValue *m = json_at(extra, i);
            const char *v = json_get_str(m, "value", "");
            if (!v[0] || json_get_bool(m, "disabled", false) || strpbrk(v, "\"$`\\")) continue;
            n = add_choice(out, n, cap, v, json_get_str(m, "label", v), json_get_str(m, "description", ""));
        }
        json_free(j);
    } else if (kind == 1) { /* the models Codex lists in its own picker */
        char cmodel[64], ceffort[32];
        codex_config(cmodel, sizeof cmodel, ceffort, sizeof ceffort);
        JsonValue *j = codex_models();
        const JsonValue *models = json_get(j, "models");
        for (size_t i = 0; i < json_len(models); i++) {
            const JsonValue *m = json_at(models, i);
            const char *slug = json_get_str(m, "slug", "");
            if (!slug[0] || strcmp(json_get_str(m, "visibility", "list"), "list") || strpbrk(slug, "\"$`\\")) continue;
            n = add_choice(out, n, cap, slug, json_get_str(m, "display_name", slug), json_get_str(m, "description", ""));
        }
        json_free(j);
        if (!n) n = add_choice(out, n, cap, "", "Default", "Codex's own choice");
        else if (def && dcap) snprintf(def, dcap, "%s", cmodel[0] ? cmodel : out[0].value);
    }
    return n;
}

int agent_effort_choices(const char *command, const char *model, AgentChoice *out, int cap, char *def, size_t dcap) {
    int n = 0;
    if (def && dcap) def[0] = 0;
    int kind = agent_tool_kind(command);
    if (kind == 0) { /* claude --effort: low, medium, high, xhigh, max */
        n = add_choice(out, n, cap, "", "Default", "Claude Code picks the effort for the model");
        n = add_choice(out, n, cap, "low", "Low", "Quickest and cheapest, to look around");
        n = add_choice(out, n, cap, "medium", "Medium", "Most everyday requests");
        n = add_choice(out, n, cap, "high", "High", "Harder problems, uses more of your plan");
        n = add_choice(out, n, cap, "xhigh", "Extra high", "Long, careful work");
        n = add_choice(out, n, cap, "max", "Max", "The deepest thinking, uses the most");
    } else if (kind == 1) { /* the chosen model's own reasoning levels */
        char cmodel[64], ceffort[32];
        codex_config(cmodel, sizeof cmodel, ceffort, sizeof ceffort);
        JsonValue *j = codex_models();
        const JsonValue *models = json_get(j, "models");
        const char *want = model && model[0] ? model : cmodel;
        for (size_t i = 0; i < json_len(models); i++) {
            const JsonValue *m = json_at(models, i);
            if (want[0] ? strcmp(json_get_str(m, "slug", ""), want) : i > 0) continue;
            const JsonValue *levels = json_get(m, "supported_reasoning_levels");
            bool has_config_level = false;
            for (size_t k = 0; k < json_len(levels); k++) {
                const char *e = json_get_str(json_at(levels, k), "effort", "");
                if (e[0]) n = add_choice(out, n, cap, e, effort_label(e), json_get_str(json_at(levels, k), "description", ""));
                if (e[0] && !strcmp(e, ceffort)) has_config_level = true;
            }
            if (def && dcap) /* config.toml's level applies to every model that has it; another starts at its own */
                snprintf(def, dcap, "%s", has_config_level ? ceffort : json_get_str(m, "default_reasoning_level", ""));
            break;
        }
        json_free(j);
        if (!n) {
            static const char *const lv[] = {"low", "medium", "high"};
            for (int k = 0; k < 3; k++) n = add_choice(out, n, cap, lv[k], effort_label(lv[k]), "");
            if (def && dcap) snprintf(def, dcap, "%s", ceffort[0] ? ceffort : "medium");
        }
    }
    return n;
}

/* how freely the tool may act beyond the lab's own tools (which never need asking), in the tool's own modes */
int agent_permission_choices(const char *command, AgentChoice *out, int cap, char *def, size_t dcap) {
    int n = 0, kind = agent_tool_kind(command);
    if (def && dcap) def[0] = 0;
    if (kind == 0) { /* claude --permission-mode; "Ask first" is its default mode, answered here in the window */
        n = add_choice(out, n, cap, "default", "Ask first", "Anything outside the lab asks you here: Allow or Deny");
        n = add_choice(out, n, cap, "acceptEdits", "Accept edits", "File edits go ahead; commands still ask you here");
        n = add_choice(out, n, cap, "auto", "Auto", "Claude Code approves routine steps itself and asks about risky ones");
        n = add_choice(out, n, cap, "bypassPermissions", "Bypass", "Nothing asks at all; only on a computer set aside for it");
        if (def && dcap) snprintf(def, dcap, "default");
    } else if (kind == 1) { /* codex exec: its sandbox presets (it cannot ask while it runs) */
        n = add_choice(out, n, cap, "read-only", "Read only", "Reads and runs the lab; changes no files itself");
        n = add_choice(out, n, cap, "auto", "Auto", "Works inside its folder, without network");
        n = add_choice(out, n, cap, "full-access", "Full access", "No sandbox and no approvals; only on a computer set aside for it");
        if (def && dcap) snprintf(def, dcap, "auto");
    }
    return n;
}

const char *agent_model_seen(void) { return AG.model_id[0] ? AG.model_id : AG.model_name; }

/* ---- permission prompts answered in the window (openphysics-mcp's approve tool writes NAME.req, the window NAME.ans) ---- */

static char approval_dir[700], approval_base[1100], approval_tool[64], approval_detail[600];
static double wall_seconds(void);

static void approval_summary(const JsonValue *in, const char *tool, char *out, size_t cap) {
    static const char *const keys[] = {"summary", "command", "file_path", "notebook_path", "path", "url", "query", "pattern", NULL};
    for (int k = 0; keys[k]; k++) {
        const char *v = json_get_str(in, keys[k], NULL);
        if (v && v[0]) {
            snprintf(out, cap, "%s", v);
            return;
        }
    }
    size_t n;
    char *t = json_dump(in, 0, &n, NULL);
    snprintf(out, cap, "%s", t && n > 2 ? t : tool);
    free(t);
}

bool agent_approval_pending(char *tool, size_t tcap, char *detail, size_t dcap) {
    static double last;
    if (!approval_dir[0] || !agent_running()) {
        approval_base[0] = 0;
        return false;
    }
    double now = wall_seconds();
    if (now - last > 0.25) { /* a look at the folder four times a second */
        last = now;
        char found[1100] = "";
        DIR *d = opendir(approval_dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (n < 5 || strcmp(e->d_name + n - 4, ".req")) continue;
            char b[1100];
            snprintf(b, sizeof b, "%s/%.*s", approval_dir, (int)(n - 4), e->d_name);
            if (!found[0] || strcmp(b, found) < 0) snprintf(found, sizeof found, "%s", b); /* the oldest first */
        }
        if (d) closedir(d);
        if (strcmp(found, approval_base)) {
            snprintf(approval_base, sizeof approval_base, "%s", found);
            approval_tool[0] = approval_detail[0] = 0;
            if (found[0]) {
                char p[1200];
                snprintf(p, sizeof p, "%s.req", found);
                JsonValue *j = json_read_file(p, 1u << 20, NULL);
                snprintf(approval_tool, sizeof approval_tool, "%s", json_get_str(j, "tool_name", "a tool"));
                approval_summary(json_get(j, "input"), approval_tool, approval_detail, sizeof approval_detail);
                utf8_trim_end(approval_detail);
                json_free(j);
            }
        }
    }
    if (!approval_base[0]) return false;
    snprintf(tool, tcap, "%s", approval_tool);
    snprintf(detail, dcap, "%s", approval_detail);
    return true;
}

void agent_approval_answer(bool allow) {
    if (!approval_base[0]) return;
    char p[1200], tmp[1200];
    snprintf(p, sizeof p, "%s.ans", approval_base);
    snprintf(tmp, sizeof tmp, "%s.anstmp", approval_base);
    FILE *f = fopen(tmp, "w");
    if (f) fputs(allow ? "allow" : "deny", f), fclose(f), rename(tmp, p);
    if (!strcmp(approval_tool, "setup_confirm")) /* the person's answer to the setup the AI made of their words */
        agent_line(1, "%s: %s", allow ? "confirmed the setup" : "said the setup is not right", approval_detail);
    else
        agent_line(1, "%s %s: %s", allow ? "allowed" : "declined", approval_tool, approval_detail);
    snprintf(p, sizeof p, "%s.req", approval_base); /* gone from the window at once; the server removes its files */
    unlink(p);
    approval_base[0] = 0;
}

/* "key=value;key=value": the value for key is replaced (or added in front) */
void agent_seen_put(char *map, size_t cap, const char *key, const char *id) {
    char rest[1024] = "";
    size_t kl = strlen(key);
    for (const char *p = map; *p; p += strcspn(p, ";"), p += *p == ';') {
        int n = (int)strcspn(p, ";");
        if (!(strncmp(p, key, kl) == 0 && p[kl] == '='))
            snprintf(rest + strlen(rest), sizeof rest - strlen(rest), "%s%.*s", rest[0] ? ";" : "", n, p);
    }
    snprintf(map, cap, "%s=%s%s%s", key, id, rest[0] ? ";" : "", rest);
}

/* ---- the child ---- */

static bool agent_ensure_socket(char *err, size_t cap) {
    if (app.ctl) {
        if (!AG.socket_path[0]) {
            const char *home = getenv("HOME");
            snprintf(AG.socket_path, sizeof AG.socket_path, "%s/.openphysicsai/run/control.sock", home ? home : "");
        }
        return true;
    }
    if (!app.engine) {
        snprintf(err, cap, "the analysis engine did not start, so there is nothing for an agent to drive");
        return false;
    }
    char dir[512];
    if (!op_state_dir(dir, sizeof dir, "run")) snprintf(dir, sizeof dir, "/tmp/openphysics-%d", (int)getuid());
    /* a socket's path has room for 103 bytes (104 on macOS with its end): a long home goes to a folder of this user's in /tmp */
    if (strlen(dir) + 24 >= sizeof(((struct sockaddr_un *)0)->sun_path)) snprintf(dir, sizeof dir, "/tmp/openphysics-%d", (int)getuid());
    path_mkdirs(dir); /* with its parents: on a first run ~/.openphysicsai does not exist yet */
    chmod(dir, 0700); /* the window's channel is for this user only */
    struct stat dst;
    if (stat(dir, &dst) != 0 || dst.st_uid != getuid() || (dst.st_mode & 077)) {
        snprintf(err, cap, "%s is not a folder of this user's alone, so the agent's channel is not opened there", dir);
        return false;
    }
    snprintf(AG.socket_path, sizeof AG.socket_path, "%s/agent-%d.sock", dir, (int)getpid());
    CtlServerConfig sc;
    ctl_server_config_default(&sc);
    snprintf(sc.unix_path, sizeof sc.unix_path, "%s", AG.socket_path);
    signal(SIGPIPE, SIG_IGN);
    char e2[512];
    app.ctl = ctl_server_start(app.engine, &sc, e2, sizeof e2);
    if (!app.ctl) {
        snprintf(err, cap, "the window could not open a channel for the agent: %s", e2);
        return false;
    }
    return true;
}

static bool agent_mcp_server(char *out, size_t cap) {
    char exe[1024] = "";
    uint32_t sz = sizeof exe;
    extern int _NSGetExecutablePath(char *buf, uint32_t *bufsize);
    if (_NSGetExecutablePath(exe, &sz) != 0) return false;
    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    snprintf(out, cap, "%s/openphysics-mcp", exe);
    return access(out, X_OK) == 0;
}

static double wall_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* a Claude Code command that can hold a conversation: the one the app detects (-p "$OPENPHYSICS_PROMPT", stream-json out) */
static bool session_capable(const char *command) {
    return agent_tool_kind(command) == 0 && strstr(command, "-p \"$OPENPHYSICS_PROMPT\"") && strstr(command, "stream-json");
}

/* the command without its --model (as the window adds it: --model "X"), and that model */
static void split_model(const char *command, char *base, size_t bcap, char *model, size_t mcap) {
    model[0] = 0;
    const char *m = strstr(command, " --model ");
    if (!m) {
        snprintf(base, bcap, "%s", command);
        return;
    }
    const char *v = m + 9, *e;
    if (*v == '"') e = strchr(v + 1, '"'), snprintf(model, mcap, "%.*s", e ? (int)(e - v - 1) : 0, v + 1), e = e ? e + 1 : v + strlen(v);
    else e = v + strcspn(v, " "), snprintf(model, mcap, "%.*s", (int)(e - v), v);
    snprintf(base, bcap, "%.*s%s", (int)(m - command), command, e);
}

static bool agent_write_line(const char *text) {
    if (AG.in_fd < 0) return false;
    size_t n = strlen(text), done = 0;
    char *line = malloc(n + 2);
    if (!line) return false;
    memcpy(line, text, n), line[n] = '\n';
    while (done < n + 1) {
        ssize_t w = write(AG.in_fd, line + done, n + 1 - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += (size_t)w;
    }
    free(line);
    return done == n + 1;
}

/* the picture what the person saw as they asked goes with the next question (a conversation only); read once, then
 * removed */
void agent_attach_picture(const char *jpeg_path) { snprintf(AG.picture, sizeof AG.picture, "%s", jpeg_path ? jpeg_path : ""); }

static bool agent_send_user(const char *prompt) {
    JsonValue *m = json_object(), *msg = json_set_object(m, "message"), *content, *c = json_object();
    json_set_string(m, "type", "user");
    json_set_string(msg, "role", "user");
    content = json_set_array(msg, "content");
    if (AG.picture[0]) { /* the picture first, as the model reads it best */
        FILE *pf = fopen(AG.picture, "rb");
        unsigned char *b = pf ? malloc(4u << 20) : NULL;
        size_t n = b ? fread(b, 1, 4u << 20, pf) : 0;
        if (pf) fclose(pf);
        char *b64 = n > 3 && b[0] == 0xFF && b[1] == 0xD8 ? base64_encode(b, n, NULL) : NULL;
        if (b64) {
            JsonValue *im = json_object(), *src = json_set_object(im, "source");
            json_set_string(im, "type", "image");
            json_set_string(src, "type", "base64");
            json_set_string(src, "media_type", "image/jpeg");
            json_set_string(src, "data", b64);
            json_push(content, im);
        }
        free(b64), free(b);
        unlink(AG.picture);
        AG.picture[0] = 0;
    }
    json_set_string(c, "type", "text");
    json_set_string(c, "text", prompt);
    json_push(content, c);
    char *t = json_dump(m, 0, NULL, NULL);
    json_free(m);
    bool ok = t && agent_write_line(t);
    free(t);
    return ok;
}

/* the session's own requests: interrupt (the answer ends, the conversation stays) and set_model */
static bool agent_control(const char *subtype, const char *model) {
    static int seq;
    char id[32];
    snprintf(id, sizeof id, "app-%d", ++seq);
    JsonValue *m = json_object(), *r = json_set_object(m, "request");
    json_set_string(m, "type", "control_request");
    json_set_string(m, "request_id", id);
    json_set_string(r, "subtype", subtype);
    if (model) json_set_string(r, "model", model);
    char *t = json_dump(m, 0, NULL, NULL);
    json_free(m);
    bool ok = t && agent_write_line(t);
    free(t);
    return ok;
}

/* a question begins: what its record and the headset read about it */
static void agent_turn_begins(const char *prompt, const char *command) {
    AG.busy = true, AG.turn++, AG.partial[0] = 0, AG.partial_done = false;
    AG.t_asked = wall_seconds(), AG.t_first = 0, AG.stop_at = 0;
    AG.started = app.time;
    snprintf(AG.prompt, sizeof AG.prompt, "%s", prompt);
    snprintf(AG.command, sizeof AG.command, "%s", command);
    AG.answer[0] = AG.record_path[0] = 0;
    AG.have_turns = AG.have_cost = AG.stopped = AG.recorded = AG.is_error = false;
    AG.turns = 0, AG.cost_usd = 0;
    AG.wall_start = wall_seconds();
    AG.journal_since = app.engine ? engine_revision(app.engine) : 0;
    agent_line(2, "asked: %s", prompt);
}

/* what the agent's command line finds in its environment, under OPENPHYSICS_<name> and, for one release after the rename
 * (2026-10-21), under the old NAVIER_<name>, so that a command saved before it still finds its question and tools */
static void agent_setenv(const char *name, const char *value) {
    char key[64];
    snprintf(key, sizeof key, "OPENPHYSICS_%s", name);
    setenv(key, value, 1);
    snprintf(key, sizeof key, "NAVIER_%s", name);
    setenv(key, value, 1);
}

/* prompt NULL: a conversation's process started and waiting, before its first question (nothing is asked or spent) */
bool agent_start(const char *command, const char *workdir, const char *prompt, char *err, size_t cap) {
    if (!prompt && (!command || !session_capable(command))) return false;
    if (agent_running()) {
        snprintf(err, cap, "the agent is still working: press STOP first");
        return false;
    }
    if (!command || !command[0]) {
        snprintf(err, cap, "no AI tool is chosen: pick one in the settings below");
        return false;
    }
    const char *home0 = getenv("HOME");
    const char *where = workdir && workdir[0] ? workdir : (home0 ? home0 : "/");
    char base[4096], model[64];
    bool session = session_capable(command);
    split_model(command, base, sizeof base, model, sizeof model);
    if (session && AG.session && agent_alive()) {
        if (!strcmp(base, AG.session_cmd) && !strcmp(where, AG.session_dir)) { /* the conversation goes on */
            if (!prompt) return true;
            if (strcmp(model, AG.model_now)) { /* this question's model: the voice model, or the one picked (none: the default) */
                if (agent_control("set_model", model[0] ? model : NULL)) snprintf(AG.model_now, sizeof AG.model_now, "%s", model);
            }
            if (agent_send_user(prompt)) {
                agent_turn_begins(prompt, command);
                return true;
            }
            agent_line(3, "the conversation's process does not listen any more: a new one starts");
        }
    }
    if (agent_alive()) { /* an idle conversation with another command or folder: it ends, and another starts */
        kill(-AG.pid, SIGTERM);
        int i = 0;
        while (i++ < 40 && waitpid(AG.pid, NULL, WNOHANG) != AG.pid) usleep(25000);
        if (i > 40) kill(-AG.pid, SIGKILL), waitpid(AG.pid, NULL, 0);
        AG.exited = true;
    }
    if (AG.in_fd >= 0) close(AG.in_fd), AG.in_fd = -1;
    if (AG.fd >= 0) close(AG.fd), AG.fd = -1;
    /* the same conversation resumed after its process ended (STOP that had to end it, or a crash) */
    bool resume = session && AG.session_id[0] && !strcmp(base, AG.session_cmd) && !strcmp(where, AG.session_dir);
    if (!resume) AG.session_id[0] = 0;
    char mcp[1024];
    if (!agent_mcp_server(mcp, sizeof mcp)) {
        snprintf(err, cap, "openphysics-mcp is missing beside the application, so an agent cannot reach this window: rebuild or reinstall");
        return false;
    }
    if (!agent_ensure_socket(err, cap)) return false;
    const char *home = getenv("HOME");
    char dir[600];
    snprintf(dir, sizeof dir, "%s/.openphysicsai/agent", home ? home : "/tmp");
    path_mkdirs(dir); /* with its parents: on a first run ~/.openphysicsai does not exist yet */
    snprintf(AG.mcp_path, sizeof AG.mcp_path, "%s/mcp.json", dir);
    FILE *f = fopen(AG.mcp_path, "w");
    if (!f) {
        snprintf(err, cap, "cannot write %s", AG.mcp_path);
        return false;
    }
    /* the lab's tools (openphysics-mcp --lab) and the window's answers to permission prompts (--approvals) */
    char lab_root[1024] = "", examples[1024];
    if (app_resource_path("examples/lab", examples, sizeof examples)) {
        snprintf(lab_root, sizeof lab_root, "%s", examples);
        char *e = strstr(lab_root, "/examples/lab");
        if (e) *e = 0;
    }
    snprintf(approval_dir, sizeof approval_dir, "%s/approvals-%d", dir, (int)getpid());
    mkdir(approval_dir, 0700);
    { /* questions left by an earlier run are void */
        DIR *d = opendir(approval_dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[1400];
            snprintf(p, sizeof p, "%s/%s", approval_dir, e->d_name);
            unlink(p);
        }
        if (d) closedir(d);
    }
    approval_base[0] = 0;
    fprintf(f, "{\n  \"mcpServers\": {\n    \"openphysics\": {\n      \"command\": \"%s\",\n      \"args\": [\"--connect\", \"%s\"",
            mcp, AG.socket_path);
    if (lab_root[0]) fprintf(f, ", \"--lab\", \"%s\", \"--lab-results\", \"%s\"", lab_root, labapp_results_dir());
    if (session) fprintf(f, ", \"--lean\""); /* a conversation carries 3 000 tokens of tools, not 81 000 (openphysics-mcp --lean) */
    fprintf(f, ", \"--approvals\", \"%s\"]\n    }\n  }\n}\n", approval_dir);
    fclose(f);

    int pipefd[2], infd[2] = {-1, -1};
    if (pipe(pipefd) != 0 || (session && pipe(infd) != 0)) {
        snprintf(err, cap, "cannot open a pipe to the agent: %s", strerror(errno));
        return false;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC); /* no other program the window starts holds the agent's pipes open */
    if (session) fcntl(infd[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 2);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    if (session) posix_spawn_file_actions_adddup2(&fa, infd[0], 0), posix_spawn_file_actions_addclose(&fa, infd[1]);
    else posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP); /* its own group, so STOP ends it and everything it started */
    posix_spawnattr_setpgroup(&at, 0);
    /* the words and the paths travel as environment variables, never spliced into the shell line */
    agent_setenv("PROMPT", prompt ? prompt : ""); /* a conversation started before its question has none yet */
    agent_setenv("MCP_CONFIG", AG.mcp_path);
    agent_setenv("MCP_COMMAND", mcp);
    agent_setenv("SOCKET", AG.socket_path);
    agent_setenv("LAB_ROOT", lab_root);
    agent_setenv("LAB_RESULTS", labapp_results_dir());
    agent_setenv("APPROVALS", approval_dir);
    /* a lab run or a question to the user can take minutes: Claude Code waits 15 min for this server's tools */
    if (!getenv("MCP_TOOL_TIMEOUT")) setenv("MCP_TOOL_TIMEOUT", "900000", 0);
    char line[4400], run[4300];
    if (session) { /* one process for the conversation: the questions come on its stdin, its words as they come; no
                    * built-in tools and no tool search, so that each question carries a small prompt (measured 2026-10-07:
                    * the first answer took 9 to 10 s with them, the engine's own tools reach everything it needs) */
        const char *p = strstr(command, "-p \"$OPENPHYSICS_PROMPT\"");
        snprintf(run, sizeof run, "%.*s-p --input-format stream-json --include-partial-messages --tools \"\"%s%s%s%s", (int)(p - command), command,
                 resume ? " --resume \"" : "", resume ? AG.session_id : "", resume ? "\"" : "", p + strlen("-p \"$OPENPHYSICS_PROMPT\""));
    } else snprintf(run, sizeof run, "%s", command);
    snprintf(line, sizeof line, "cd \"%s\" && %sexec %s", where, session ? "ENABLE_TOOL_SEARCH=false " : "", run);
    char *argv[] = {"/bin/sh", "-c", line, NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, "/bin/sh", &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(pipefd[1]);
    if (session) close(infd[0]);
    if (rc != 0) {
        close(pipefd[0]);
        if (session) close(infd[1]);
        snprintf(err, cap, "the agent could not be started: %s", strerror(rc));
        return false;
    }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    AG.pid = pid, AG.fd = pipefd[0], AG.pending_len = 0, AG.exited = false, AG.exit_code = 0;
    AG.session = session, AG.in_fd = session ? infd[1] : -1;
    AG.model_name[0] = AG.model_id[0] = AG.tool_version[0] = 0;
    if (session) {
        snprintf(AG.session_cmd, sizeof AG.session_cmd, "%s", base);
        snprintf(AG.session_dir, sizeof AG.session_dir, "%s", where);
        snprintf(AG.model_now, sizeof AG.model_now, "%s", model);
        if (resume) agent_line(2, "the conversation resumes (session %s)", AG.session_id);
    }
    if (!prompt) { /* ready for the first question */
        AG.busy = false, AG.partial_done = true;
        snprintf(AG.command, sizeof AG.command, "%s", command);
        return true;
    }
    agent_turn_begins(prompt, command);
    if (session && !agent_send_user(prompt)) {
        snprintf(err, cap, "the agent did not take the question");
        return false;
    }
    return true;
}

static void agent_record(void);

/* one line of the agent's output: stream-json events become their text, anything else is shown as it came */
static void agent_consume(const char *raw) {
    if (!raw[0]) return;
    JsonError je;
    JsonValue *j = raw[0] == '{' ? json_parse(raw, strlen(raw), NULL, &je) : NULL;
    if (!j) {
        agent_line(0, "%s", raw);
        return;
    }
    const char *type = json_get_str(j, "type", "");
    if (!strcmp(type, "stream_event")) { /* the words as they come (--include-partial-messages) */
        const JsonValue *ev = json_get(j, "event");
        const char *et = json_get_str(ev, "type", "");
        size_t n = strlen(AG.partial);
        if (!strcmp(et, "content_block_start") && !strcmp(json_get_str(json_get(ev, "content_block"), "type", ""), "text")) {
            if (n && AG.partial[n - 1] != '\n' && n + 2 < sizeof AG.partial) AG.partial[n] = '\n', AG.partial[n + 1] = 0;
        } else if (!strcmp(et, "content_block_delta") && !strcmp(json_get_str(json_get(ev, "delta"), "type", ""), "text_delta")) {
            const char *t = json_get_str(json_get(ev, "delta"), "text", "");
            snprintf(AG.partial + n, sizeof AG.partial - n, "%s", t);
            utf8_trim_end(AG.partial);
            if (!AG.t_first && t[0]) AG.t_first = wall_seconds();
        }
        json_free(j);
        return;
    }
    if (!strcmp(type, "assistant")) {
        const char *m = json_get_str(json_get(j, "message"), "model", "");
        if (m[0] && m[0] != '<') snprintf(AG.model_name, sizeof AG.model_name, "%s", m); /* "<synthetic>" is not a model */
        const JsonValue *content = json_get(json_get(j, "message"), "content");
        for (size_t i = 0; i < json_len(content); i++) {
            const JsonValue *c = json_at(content, i);
            const char *ct = json_get_str(c, "type", "");
            if (!strcmp(ct, "text")) {
                const char *t = json_get_str(c, "text", "");
                if (!AG.t_first && t[0]) { /* a tool that does not stream its words: the whole message at once */
                    size_t n = strlen(AG.partial);
                    snprintf(AG.partial + n, sizeof AG.partial - n, "%s%s", n ? "\n" : "", t);
                    utf8_trim_end(AG.partial);
                }
                snprintf(AG.answer, sizeof AG.answer, "%s", t); /* the last words the agent said */
                utf8_trim_end(AG.answer);
                char buf[4096];
                snprintf(buf, sizeof buf, "%s", t);
                for (char *p = strtok(buf, "\n"); p; p = strtok(NULL, "\n")) agent_line(0, "%s", p);
            } else if (!strcmp(ct, "tool_use")) {
                const char *name = json_get_str(c, "name", "");
                const char *shortn = strstr(name, "__") ? strrchr(name, '_') + 1 : name;
                (void)shortn;
                static const char prefix[] = "mcp__openphysics__", old[] = "mcp__navier-am__"; /* old: a command saved before the rename */
                agent_line(1, "calls %s", !strncmp(name, prefix, sizeof prefix - 1) ? name + sizeof prefix - 1
                                          : !strncmp(name, old, sizeof old - 1) ? name + sizeof old - 1 : name);
            }
        }
    } else if (!strcmp(type, "user")) {
        /* a tool result: its content is data for the agent; what it changed is already a line from the journal */
    } else if (!strcmp(type, "result")) {
        AG.is_error = json_get_bool(j, "is_error", false);
        if (json_get(j, "num_turns")) AG.have_turns = true, AG.turns = json_get_int(j, "num_turns", 0);
        const JsonValue *cost = json_get(j, "total_cost_usd");
        if (cost && cost->type == JSON_NUMBER) AG.have_cost = true, AG.cost_usd = cost->u.number;
        const char *r = json_get_str(j, "result", "");
        if (r[0]) snprintf(AG.answer, sizeof AG.answer, "%s", r), utf8_trim_end(AG.answer);
        if (AG.stopped && AG.session) agent_line(2, "the answer was stopped");
        else if (json_get_bool(j, "is_error", false)) agent_line(3, "the agent stopped with an error: %s", json_get_str(j, "result", "unknown"));
        else agent_line(2, "the agent finished (%.0f s, %lld turns)", json_get_num(j, "duration_ms", 0) / 1000.0, json_get_int(j, "num_turns", 0));
        if (AG.session) { /* the turn ends, the conversation stays */
            AG.busy = false, AG.partial_done = true, AG.stop_at = 0;
            agent_record();
        }
    } else if (!strcmp(type, "system")) {
        if (!strcmp(json_get_str(j, "subtype", ""), "init")) {
            if (json_get_str(j, "session_id", NULL)) snprintf(AG.session_id, sizeof AG.session_id, "%s", json_get_str(j, "session_id", ""));
            snprintf(AG.model_id, sizeof AG.model_id, "%s", json_get_str(j, "model", ""));
            snprintf(AG.tool_version, sizeof AG.tool_version, "%s", json_get_str(j, "claude_code_version", ""));
        }
        const JsonValue *servers = json_get(j, "mcp_servers");
        for (size_t i = 0; i < json_len(servers); i++) {
            const JsonValue *s = json_at(servers, i);
            agent_line(2, "tool connection %s: %s", json_get_str(s, "name", "?"), json_get_str(s, "status", "?"));
        }
    }
    json_free(j);
}

/* ---- the run record for the agent leaderboard (challenges/agents/runs, format openphysicsai-agent-run) ---- */

#include "../build/gen/app_commit.h"

static bool dir_has_template(const char *dir) {
    char p[1200];
    snprintf(p, sizeof p, "%s/challenges/agents/run_template.json", dir);
    return access(p, R_OK) == 0;
}

/* the repository this app was built from: walk up from the executable, then from the working folder */
static bool agent_runs_dir(char *out, size_t cap) {
    const char *env = op_getenv("AGENT_RUNS_DIR");
    if (env && env[0]) {
        snprintf(out, cap, "%s", env);
        mkdir(out, 0755);
        return true;
    }
    char starts[2][1024] = {"", ""};
    uint32_t sz = sizeof starts[0];
    extern int _NSGetExecutablePath(char *buf, uint32_t *bufsize);
    if (_NSGetExecutablePath(starts[0], &sz) != 0) starts[0][0] = 0;
    if (!getcwd(starts[1], sizeof starts[1])) starts[1][0] = 0;
    for (int k = 0; k < 2; k++) {
        char d[1024];
        snprintf(d, sizeof d, "%s", starts[k]);
        for (int up = 0; up < 6 && d[0]; up++) {
            char *sl = strrchr(d, '/');
            if (!sl) break;
            if (k == 1 && up == 0) { /* the working folder itself is a candidate */
                if (dir_has_template(d)) {
                    snprintf(out, cap, "%s/challenges/agents/runs", d);
                    mkdir(out, 0755);
                    return true;
                }
            }
            *sl = 0;
            if (d[0] && dir_has_template(d)) {
                snprintf(out, cap, "%s/challenges/agents/runs", d);
                mkdir(out, 0755);
                return true;
            }
        }
    }
    if (!op_state_dir(out, cap, "agent/runs")) snprintf(out, cap, "/tmp/openphysics-agent-runs");
    path_mkdirs(out);
    chmod(out, 0700);
    return false;
}

/* what Claude Code ran for each pick in the runs recorded on this computer, oldest first so that the newest stays, so
 * the chip can name the model before this window's first question ("claude:default=claude-opus-4-8[1m];...") */
typedef struct {
    char name[256];
    time_t t;
} RunFile;
static int runfile_cmp(const void *a, const void *b) {
    time_t x = ((const RunFile *)a)->t, y = ((const RunFile *)b)->t;
    return x < y ? -1 : x > y;
}
void agent_seen_from_records(char *map, size_t cap) {
    char dir[1024], p[1400];
    agent_runs_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;
    RunFile *f = calloc(512, sizeof *f);
    int n = 0;
    struct dirent *e;
    while (f && n < 512 && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        struct stat st;
        if (l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) == 0) snprintf(f[n].name, sizeof f[n].name, "%s", e->d_name), f[n++].t = st.st_mtime;
    }
    closedir(d);
    if (f) qsort(f, (size_t)n, sizeof *f, runfile_cmp);
    for (int i = 0; i < n; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, f[i].name);
        JsonValue *j = json_read_file(p, 1u << 20, NULL);
        const char *cmd = json_get_str(json_get(j, "agent"), "command", "");
        const char *id = json_get_str(json_get(j, "model"), "version", "");
        if (!id[0]) id = json_get_str(json_get(j, "model"), "name", "");
        if (agent_tool_kind(cmd) == 0 && !strncmp(id, "claude-", 7) && !strpbrk(id, ";=")) {
            char pick[64] = "default", key[96];
            const char *m = strstr(cmd, "--model ");
            if (m) {
                m += 8 + (m[8] == '"');
                snprintf(pick, sizeof pick, "%.*s", (int)strcspn(m, "\" "), m);
            }
            snprintf(key, sizeof key, "claude:%s", pick);
            agent_seen_put(map, cap, key, id);
        }
        json_free(j);
    }
    free(f);
}

static void slug_words(char *out, size_t cap, const char *text, int words) {
    size_t n = 0;
    int w = 0;
    bool in_word = false;
    for (const char *p = text; *p && w < words && n + 2 < cap; p++) {
        char c = (char)tolower((unsigned char)*p);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (!in_word && n) out[n++] = '-';
            out[n++] = c;
            in_word = true;
        } else if (in_word) {
            in_word = false;
            w++;
        }
    }
    out[n] = 0;
    if (!n) snprintf(out, cap, "run");
}

/* the tool, as the command line names it: claude, codex, or the program it runs */
static void agent_tool_of(const char *command, char *name, size_t ncap, char *slug, size_t scap) {
    if (strstr(command, "claude")) {
        snprintf(name, ncap, "Claude Code (claude -p) through the app's Agent mode");
        snprintf(slug, scap, "claude");
        return;
    }
    if (strstr(command, "codex")) {
        snprintf(name, ncap, "Codex (codex exec) through the app's Agent mode");
        snprintf(slug, scap, "codex");
        return;
    }
    /* the last word that looks like a program: "python3 tools/agent_standin.py" is agent_standin */
    char buf[1024], last[256] = "";
    snprintf(buf, sizeof buf, "%s", command);
    for (char *t = strtok(buf, " \t\""); t; t = strtok(NULL, " \t\"")) {
        if (t[0] == '-' || t[0] == '$') continue;
        const char *b = strrchr(t, '/') ? strrchr(t, '/') + 1 : t;
        snprintf(last, sizeof last, "%s", b);
        char *dot = strrchr(last, '.');
        if (dot && dot != last) *dot = 0;
    }
    snprintf(name, ncap, "%s through the app's Agent mode", last[0] ? last : "a command");
    slug_words(slug, scap, last[0] ? last : "command", 3);
}

static void agent_record(void) {
    if (AG.recorded || !AG.prompt[0]) return;
    AG.recorded = true;
    char dir[1024];
    bool in_repo = agent_runs_dir(dir, sizeof dir);
    time_t now = time(NULL);
    char date[16];
    strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
    char tool[256], tslug[64], words[64];
    agent_tool_of(AG.command, tool, sizeof tool, tslug, sizeof tslug);
    slug_words(words, sizeof words, AG.prompt, 4);
    char path[1400];
    snprintf(path, sizeof path, "%s/%s-agentmode-%s-%s.json", dir, date, tslug, words);
    for (int k = 2; k < 100 && access(path, F_OK) == 0; k++)
        snprintf(path, sizeof path, "%s/%s-agentmode-%s-%s-%d.json", dir, date, tslug, words, k);

    JsonValue *r = json_object();
    json_set_string(r, "format", "openphysicsai-agent-run");
    json_set_int(r, "format_version", 1);
    json_set_string(r, "task", "agent-mode");
    json_set_string(r, "date", date);
    JsonValue *m = json_set_object(r, "model");
    json_set_string(m, "name", AG.model_name[0] ? AG.model_name : AG.model_id[0] ? AG.model_id : "not reported by the tool");
    json_set_string(m, "version", strcmp(AG.model_id, AG.model_name) ? AG.model_id : "");
    JsonValue *a = json_set_object(r, "agent");
    json_set_string(a, "tool", tool);
    json_set_string(a, "version", AG.tool_version);
    json_set_string(a, "command", AG.command);
    json_set_string(r, "prompt", AG.prompt);
    json_set_string(r, "commit", OPENPHYSICS_APP_COMMIT);
    JsonValue *acc = json_set_object(r, "acceptance");
    json_set(acc, "passed", json_null());
    json_set_string(acc, "rerun_by", "");
    json_set_string(acc, "rerun_date", "");
    json_set_string(acc, "final_lines", "");
    json_set_string(r, "human_edits_after_agent", "none");
    json_set_int(r, "wall_s", (long long)(wall_seconds() - AG.wall_start + 0.5));
    json_set(r, "cost_usd", AG.have_cost ? json_number(AG.cost_usd) : json_null());
    json_set(r, "turns", AG.have_turns ? json_number((double)AG.turns) : json_null());
    char notes[2400];
    const char *how = AG.stopped ? "stopped by the user with STOP"
                    : AG.is_error ? "the tool reported an error"
                    : AG.exit_code ? "the command ended with a non-zero code"
                    : "the tool finished";
    snprintf(notes, sizeof notes,
             "Written by the app's Agent mode when the run ended (%s). Model, tool version, turns and cost are the tool's "
             "own words from its output%s; the cost is the tool's estimate. Not a TASKS.md task: acceptance is for a "
             "maintainer to fill after rerunning the question. The agent's last words: %s",
             how, AG.model_name[0] || AG.model_id[0] ? "" : " (this tool reported no model)",
             AG.answer[0] ? AG.answer : "(none)");
    json_set_string(r, "notes", notes);
    bool ok = json_write_file(path, r, JSON_PRETTY);
    json_free(r);
    if (!ok) {
        agent_line(3, "the run could not be recorded: cannot write %s", path);
        return;
    }
    snprintf(AG.record_path, sizeof AG.record_path, "%s", path);
    agent_line(2, "run recorded%s: %s", in_repo ? " for the agent leaderboard" : " (no repository found, so outside the leaderboard)", path);
}

const char *agent_record_path(void) { return AG.record_path; }

void agent_poll(void) {
    /* what the agent changed, in the engine's own words */
    if (app.engine && AG.pid > 0) {
        JsonValue *jr = engine_journal_json(app.engine, AG.journal_since, 64);
        for (size_t i = 0; i < json_len(jr); i++) {
            const JsonValue *e = json_at(jr, i);
            agent_line(1, "%s", json_get_str(e, "summary", ""));
            uint64_t r = (uint64_t)json_get_int(e, "revision", 0);
            if (r > AG.journal_since) AG.journal_since = r;
        }
        json_free(jr);
    }
    if (AG.fd >= 0) {
        char buf[4096];
        for (;;) {
            ssize_t n = read(AG.fd, buf, sizeof buf);
            if (n <= 0) {
                if (n == 0) close(AG.fd), AG.fd = -1;
                break;
            }
            for (ssize_t i = 0; i < n; i++) {
                if (AG.pending_len + 2 > AG.pending_cap) {
                    size_t cap = AG.pending_cap ? AG.pending_cap * 2 : 65536;
                    if (cap > (64u << 20)) cap = 64u << 20; /* a line longer than this is cut, not followed */
                    char *np = realloc(AG.pending, cap);
                    if (np) AG.pending = np, AG.pending_cap = cap;
                }
                if (buf[i] == '\n' || AG.pending_len + 2 > AG.pending_cap) {
                    AG.pending[AG.pending_len] = 0;
                    agent_consume(AG.pending);
                    AG.pending_len = 0;
                } else {
                    AG.pending[AG.pending_len++] = buf[i];
                }
            }
        }
    }
    if (AG.session && AG.busy && AG.stop_at > 0 && wall_seconds() > AG.stop_at) { /* the interrupt was not heard */
        kill(-AG.pid, SIGTERM);
        AG.stop_at = 0;
    }
    if (AG.pid > 0 && !AG.exited) {
        int status;
        pid_t r = waitpid(AG.pid, &status, WNOHANG);
        if (r == AG.pid) {
            AG.exited = true;
            AG.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            if (AG.in_fd >= 0) close(AG.in_fd), AG.in_fd = -1;
            if (AG.session) {
                if (AG.busy) agent_line(AG.stopped ? 2 : 3, AG.stopped ? "stopped" : "the conversation's process ended while answering");
                else agent_line(2, "the conversation's process ended; the next question resumes the conversation");
                AG.busy = false, AG.partial_done = true;
            } else if (AG.exit_code != 0)
                agent_line(3, "the agent's command ended with code %d: check the command in the settings below", AG.exit_code);
            agent_record();
        }
    }
}

void agent_stop(void) {
    if (AG.session && agent_alive()) { /* the answer ends, the conversation stays; ended outright if it does not listen */
        if (!AG.busy || AG.stopped) return;
        AG.stopped = true;
        if (agent_control("interrupt", NULL)) {
            AG.stop_at = wall_seconds() + 3.0;
            return;
        }
    }
    if (AG.pid > 0 && !AG.exited) {
        kill(-AG.pid, SIGTERM); /* the whole group: the tool and the MCP server it started */
        agent_line(2, "stopped");
        for (int i = 0; i < 20; i++) {
            int status;
            if (waitpid(AG.pid, &status, WNOHANG) == AG.pid) {
                AG.exited = true;
                break;
            }
            usleep(50000);
        }
        if (!AG.exited) kill(-AG.pid, SIGKILL), waitpid(AG.pid, NULL, 0), AG.exited = true;
        AG.stopped = true;
        if (AG.in_fd >= 0) close(AG.in_fd), AG.in_fd = -1;
        AG.busy = false, AG.partial_done = true;
        agent_record();
    }
}
