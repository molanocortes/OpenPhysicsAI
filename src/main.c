/* main.c - NAVIER: real-time 3D lattice-Boltzmann water tunnel (entry point and frame loop) */
#include "app.h"
#include "fembridge.h"
#include "colormap.h"
#include "console.h"
#include "image.h"
#include "labapp.h"
#include "platform.h"
#include "ctl/engine.h"
#include "net/ctlserver.h"

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOOLBAR_Y_LIMIT 64.0

static UiInput g_input;
static uint64_t g_frame;
static double g_upload_bytes;
static int g_field_uploads;

typedef struct {
    uint64_t frame;
    PlatformEvent ev;
} SyntheticEvent;
static SyntheticEvent g_synth[256];
static int g_nsynth;

/* default rake for the current seed mode, placed from the model's bounding box */
static void rake_auto(void) {
    vec3 lo = app.model_lo, hi = app.model_hi;
    if (!app.has_model) lo = v3(app.nx * 0.3f, app.ny * 0.3f, app.nz * 0.3f), hi = v3(app.nx * 0.35f, app.ny * 0.7f, app.nz * 0.7f);
    const vec3 c = v3_scale(v3_add(lo, hi), 0.5f), e = v3_sub(hi, lo);
    const float xin = MAXI((float)MAXI(3, app.nx / 40) + 2.5f, lo.x - 0.35f * e.x);
    const float grid_h = e.y * 1.1f + 3.0f, grid_w = e.z * 1.1f + 3.0f;
    app.rake_axis = 0;
    switch (app.seed_mode) {
    case SEED_GRID: app.rake_pos = v3(xin, c.y, c.z), app.rake_size[0] = grid_h, app.rake_size[1] = grid_w; break;
    case SEED_WAKE:
        app.rake_pos = v3(MINI(hi.x + 0.4f * e.x, app.nx - 3.0f), c.y, c.z), app.rake_size[0] = grid_h, app.rake_size[1] = grid_w;
        break;
    case SEED_LINE: app.rake_pos = v3(xin, c.y, c.z), app.rake_size[0] = e.y * 2.2f + 6.0f, app.rake_size[1] = 0; break;
    case SEED_RANDOM:
        app.rake_pos = v3(lo.x + 0.8f * e.x, c.y, c.z), app.rake_size[0] = e.y * 1.6f + 6.0f, app.rake_size[1] = e.z * 1.4f + 6.0f;
        break;
    case SEED_POINT: /* upstream of the right-hand tip, where a lifting wing sheds its tip vortex */
        app.rake_pos = v3(xin, c.y, c.z + 0.45f * e.z), app.rake_size[0] = app.rake_size[1] = MAXI(4.0f, e.y * 0.5f);
        break;
    default: app.rake_pos = v3(xin, c.y + e.y * 0.08f, c.z), app.rake_size[0] = 0, app.rake_size[1] = e.z * 1.3f + 6.0f; break;
    }
}

static void set_request(void) {
    VisRequest r = app.req;
    r.field = display_field_vis(app.display_field);
    r.velocity = app.rs.particles_on;
    r.stream_on = app.rs.stream_on;
    r.seed_mode = app.seed_mode;
    r.seed_count = app.seed_count;
    r.stream_steps = app.stream_steps > 0 ? app.stream_steps : 4 * MAXI(app.nx, 64);
    r.iso_on = app.rs.vortex_on;
    if (!app.rake_manual) rake_auto();
    r.rake_pos[0] = app.rake_pos.x, r.rake_pos[1] = app.rake_pos.y, r.rake_pos[2] = app.rake_pos.z;
    r.rake_size[0] = app.rake_size[0], r.rake_size[1] = app.rake_size[1];
    r.rake_axis = app.rake_axis;
    r.iso_level = app.rs.vortex_level;
    VisRequest a = r, b = app.req;
    a.version = b.version = 0;
    if (memcmp(&a, &b, sizeof a) != 0) r.version = app.req.version + 1;
    if (r.version != app.req.version || memcmp(&r, &app.req, sizeof r) != 0) {
        app.req = r;
        visworker_request(app.worker, &app.req);
    }
}

static void consume_results(void) {
    VisResults *R = visworker_lock(app.worker);
    if (R->nx > 0) {
        render_set_grid(app.renderer, R->nx, R->ny, R->nz);
        if (R->solid_new && R->solid) {
            render_upload_solid(app.renderer, R->solid);
            g_upload_bytes += (double)R->nx * R->ny * R->nz;
            R->solid_new = false;
        }
        if (R->field_new && R->field_half) {
            render_upload_field_half(app.renderer, R->field_half, R->field_scale);
            g_upload_bytes += 2.0 * R->nx * R->ny * R->nz;
            g_field_uploads++;
            if (R->field_id == display_field_vis(app.display_field)) {
                app.stats = R->range;
                app.stats_field = app.display_field;
                if (!app.have_field) app.lat_lo = R->range.lo, app.lat_hi = R->range.hi;
                app.have_field = true;
            }
            app.field_ms = R->field_ms;
            R->field_new = false;
        }
        if (R->vel_new && R->vel_half) {
            render_upload_velocity_half(app.renderer, R->vel_half);
            g_upload_bytes += 6.0 * ((R->nx + 1) / 2) * ((R->ny + 1) / 2) * ((R->nz + 1) / 2);
            R->vel_new = false;
        }
        if (R->stream_new) {
            render_set_streamlines(app.renderer, &R->stream);
            g_upload_bytes += R->stream.nverts * 96.0;
            app.stream_ms = R->stream_ms;
            app.stream_lines = R->stream.nlines, app.stream_verts = R->stream.nverts;
            R->stream_new = false;
        }
        if (R->iso_new) {
            render_set_isosurface(app.renderer, &R->iso);
            g_upload_bytes += R->iso.nverts * 28.0 + R->iso.nindices * 4.0;
            app.iso_ms = R->iso_ms;
            app.iso_tris = R->iso.nindices / 3;
            R->iso_new = false;
        }
    }
    visworker_unlock(app.worker);
}

static void update_range(void) {
    if (!app.have_field) return;
    const SimUnits *u = &app.status.units;
    double s = display_field_scale(app.display_field, u);
    float lo, hi;
    if (app.manual_range && s != 0) {
        lo = (float)(app.range_lo / s), hi = (float)(app.range_hi / s);
    } else {
        VisRange r = app.stats;
        switch (app.display_field) {
        case DF_SPEED: lo = 0, hi = MAXI(r.hi, (float)(u->u_lb * 1.1)); break;
        case DF_VORTICITY: lo = 0, hi = MAXI(r.hi, 1e-9f); break;
        case DF_CP: lo = MINI(r.lo, -0.2f * (float)(0.5 * u->u_lb * u->u_lb)), hi = MAXI(r.hi, 1e-9f); break;
        default: {
            float m = MAXI(fabsf(r.lo), fabsf(r.hi));
            lo = -MAXI(m, 1e-12f), hi = MAXI(m, 1e-12f);
        }
        }
    }
    if (hi <= lo) hi = lo + 1e-12f;
    /* a newly selected field starts at its own range instead of easing out of the previous field's */
    bool snap = app.stats_field == app.display_field && app.range_field != app.display_field;
    if (snap) app.range_field = app.display_field;
    float k = app.manual_range || snap ? 1.0f : 1.0f - (float)exp(-app.dt * 3.0);
    app.lat_lo += (lo - app.lat_lo) * k;
    app.lat_hi += (hi - app.lat_hi) * k;
    if (app.display_field == DF_SPEED || app.display_field == DF_VORTICITY) app.lat_lo = MAXI(app.lat_lo, 0.0f);
    float target_speed = (float)MAXI(u->u_lb * 1.5, 1e-4);
    app.speed_hi += (target_speed - app.speed_hi) * (app.speed_hi <= 0 ? 1.0f : k);
}

static void emitter_box(vec3 *lo, vec3 *hi) {
    float x0 = (float)MAXI(3, app.nx / 40) + 1.0f;
    *lo = v3(x0, 1.0f, 1.0f);
    *hi = v3(x0 + 3.0f, app.ny - 1.0f, app.nz - 1.0f);
    if (app.emitter_mode == 2) return;
    vec3 c = v3_scale(v3_add(app.model_lo, app.model_hi), 0.5f), e = v3_sub(app.model_hi, app.model_lo);
    if (!app.has_model) c = v3(app.nx * 0.5f, app.ny * 0.5f, app.nz * 0.5f), e = v3(0, 0, app.nz * 0.7f);
    if (app.emitter_mode == 0) {
        /* thin smoke-wire sheet slightly above the model centre */
        float y = c.y + e.y * 0.08f;
        lo->y = MAXI(1.0f, y - 1.2f), hi->y = MINI(app.ny - 1.0f, y + 1.2f);
        lo->z = MAXI(1.0f, c.z - e.z * 0.65f - 3), hi->z = MINI(app.nz - 1.0f, c.z + e.z * 0.65f + 3);
    } else {
        lo->y = MAXI(1.0f, c.y - e.y * 0.8f - 3), hi->y = MINI(app.ny - 1.0f, c.y + e.y * 0.8f + 3);
        lo->z = MAXI(1.0f, c.z - e.z * 0.7f - 3), hi->z = MINI(app.nz - 1.0f, c.z + e.z * 0.7f + 3);
    }
}

static int rake_overlay(float *out, int max_pairs);

/* Everything a frame needs, in one place: the loop draws it to the window, EXPORT IMAGE draws the same frame into an
 * off-screen target three times as large. */
static float g_overlay[3 * 64];
static float g_home_lines[3 * 2 * FEM_MAX_DRAW_PARTS];

static void fill_render_frame(RenderFrame *f, RenderSettings *rs_frame, double now) {
    memset(f, 0, sizeof *f);
    f->cam = &app.cam;
    f->rs = rs_frame;
    f->time = now;
    f->dt = (float)app.dt;
    f->nx = app.nx, f->ny = app.ny, f->nz = app.nz;
    f->has_model = app.has_model;
    f->solid_workspace = app.workspace == WS_SOLID;
    f->model = app.model_M;
    f->field_lo = app.lat_lo, f->field_hi = app.lat_hi;
    f->speed_hi = app.speed_hi;
    f->particle_steps = app.particle_steps;
    emitter_box(&f->emitter_lo, &f->emitter_hi);
    f->probe_on = app.probe_on;
    f->probe = app.probe;
    f->result_outline = fem_outline(&f->result_outline_vertices);
    f->result_parts = fem_draw_parts(&f->result_nparts);
    f->result_edges = fem_edge_lines(&f->result_edge_vertices, &f->edge_parts, &f->edge_nparts);
    float peak[3];
    f->peak_on = app.workspace == WS_SOLID && fem_peak_marker(peak);
    if (f->peak_on) f->peak = v3(peak[0], peak[1], peak[2]);
    if (!fem_surface_range(&f->result_lo, &f->result_hi)) f->result_lo = 0, f->result_hi = 1;
    f->overlay = g_overlay;
    f->overlay_verts = rake_overlay(g_overlay, 64);
    /* the thin lines from an exploded piece back to where it belongs share the overlay channel */
    if (f->overlay_verts == 0 && app.workspace == WS_SOLID) {
        int n = fem_explode_lines(g_home_lines, (int)(sizeof g_home_lines / sizeof(float) / 3));
        if (n > 0) f->overlay = g_home_lines, f->overlay_verts = n;
    }
}

/* the current view at `scale` times the window, written as a PNG */
bool app_export_image(const char *path, int scale) {
    if (scale < 1) scale = 1;
    int w = app.fb_w * scale, h = app.fb_h * scale;
    const int LIMIT = 8192;
    while ((w > LIMIT || h > LIMIT) && scale > 1) scale--, w = app.fb_w * scale, h = app.fb_h * scale;
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        RenderSettings rs_frame = app.rs;
        RenderFrame f;
        fill_render_frame(&f, &rs_frame, now_seconds());
        f.target_fbo = fbo;
        f.target_w = w, f.target_h = h;
        render_frame(app.renderer, &f);
        uint8_t *px = malloc((size_t)w * (size_t)h * 4);
        ok = px != NULL;
        if (px) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            ok = image_write_png(path, w, h, px, true);
            free(px);
        }
        if (ok) LOGOK("image exported: %s (%dx%d, %dx the window)", path, w, h, scale);
        else LOGE("could not write %s", path);
    } else {
        LOGE("the export target could not be created (%dx%d)", w, h);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, app.target_fbo);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glViewport(0, 0, app.fb_w, app.fb_h);
    return ok;
}

static void save_screenshot(void) {
    int w = app.fb_w, h = app.fb_h;
    uint8_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) return;
    glBindFramebuffer(GL_FRAMEBUFFER, app.target_fbo);
    if (!app.target_fbo) glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (image_write_png(app.shot_path, w, h, px, true))
        LOGOK("screenshot saved: %s (%dx%d)", app.shot_path, w, h);
    else
        LOGE("could not write %s", app.shot_path);
    free(px);
}

/* codepoint of the key that has just focused the terminal, so the text event it produces is not typed into it */
static uint32_t g_focus_swallow;

static void hotkey(const PlatformEvent *e) {
    if (e->mods & MOD_CTRL) return; /* Ctrl chords belong to terminal editing, not to hotkeys */
    if (e->repeat && e->key != KEY_LEFT && e->key != KEY_RIGHT && e->key != KEY_UP && e->key != KEY_DOWN && e->key != '=' && e->key != '+' &&
        e->key != '-')
        return; /* holding a key must not toggle repeatedly */
    if (e->mods & MOD_CMD) {
        switch (e->key) {
        case 'o': console_exec("open", true); break;
        case 's': console_exec("screenshot", true); break;
        case 'r': console_exec(labapp_active() ? "lab frame 0" : "reset", true); break;
        case 'k': console_clear(); break;
        default: break;
        }
        return;
    }
    char cmd[64];
    /* The displayed result owns keyboard navigation. Never send a lab shortcut to a hidden tunnel or FEM result. */
    if (labapp_active() && e->key != KEY_ENTER && e->key != '`' && e->key != 't' && e->key != 'h') {
        switch (e->key) {
        case ' ': console_exec(labapp_playing() ? "lab pause" : "lab play", true); break;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': {
            char names[8][48]; int n = labapp_field_names(names, 8), i = e->key - '1';
            if (i < n) labapp_set_field(names[i]);
            break;
        }
        case 'x': case 'y': case 'z':
            if (labapp_native3d()) labapp_section_set(e->key == 'x' ? 0 : e->key == 'y' ? 1 : 2, labapp_section_fraction());
            break;
        case '[': case ']':
            if (labapp_section_axis() >= 0)
                labapp_section_set(labapp_section_axis(), CLAMP(labapp_section_fraction() + (e->key == '[' ? -0.02 : 0.02), 0, 1));
            break;
        case KEY_LEFT: case KEY_RIGHT:
            labapp_set_playing(false);
            labapp_set_frame(labapp_frame() + (e->key == KEY_LEFT ? -1 : 1));
            break;
        case KEY_UP: case KEY_DOWN: labapp_orbit(0, e->key == KEY_UP ? 30 : -30); break;
        case '=': case '+': labapp_zoom(1.18f); break;
        case '-': labapp_zoom(0.85f); break;
        case 'f': console_exec("lab fit", true); break;
        case '0': console_exec("lab view iso", true); break;
        case 'r': console_exec("lab frame 0", true); break;
        case 'm': case 'g': console_exec("lab mesh", true); break;
        case ',': case '.':
            snprintf(cmd, sizeof cmd, "lab fps %.6g", CLAMP(labapp_fps() * (e->key == ',' ? 0.5 : 2), 0.5, 60));
            console_exec(cmd, true); break;
        case 'p': if (labapp_is_water()) console_exec("lab surface off", true); break;
        case 's': if (labapp_is_water()) console_exec("lab surface on", true); break;
        default: break;
        }
        return;
    }
    switch (e->key) {
    case ' ':
        if (app.workspace == WS_SOLID) {
            /* Solid owns the input even before it has a result; never start a hidden fluid run. */
            if (fem_visible() && fem_state()->have_result)
                console_exec(fem_playing() ? "fem pause" : "fem play", true);
        } else console_exec(app.status.running ? "pause" : "start", true);
        break;
    /* only when the interface is visible: a focused terminal that is not drawn would swallow the key that shows it again */
    case KEY_ENTER: case '`': case 't':
        if (app.hud_on) {
            console_set_focus(true);
            g_focus_swallow = e->key == KEY_ENTER ? 0 : (uint32_t)e->key;
        }
        break;
    case KEY_ESCAPE: app.probe_on = false; break;
    case 'w': console_exec("workspace toggle", true); break;
    case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8':
        if (app.workspace == WS_SOLID) {
            if (e->key - '1' >= FEM_FIELD_COUNT) break;
            snprintf(cmd, sizeof cmd, "fem field %s", fem_field_name(e->key - '1'));
        } else {
            snprintf(cmd, sizeof cmd, "view %s", display_field_name(e->key - '1'));
        }
        console_exec(cmd, true);
        break;
    case 's': console_exec("streamlines toggle", true); break;
    case 'p': console_exec("particles toggle", true); break;
    case 'v': console_exec("vortices toggle", true); break;
    case 'o': console_exec("volume toggle", true); break;
    case 'l': console_exec("slice", true); break;
    case 'x': case 'y': case 'z':
        snprintf(cmd, sizeof cmd, "slice %c %.2f", e->key, app.rs.slice_pos);
        console_exec(cmd, true);
        break;
    case '[': app.rs.slice_pos = CLAMP(app.rs.slice_pos - 0.02f, 0.0f, 1.0f); break;
    case ']': app.rs.slice_pos = CLAMP(app.rs.slice_pos + 0.02f, 0.0f, 1.0f); break;
    case 'g': console_exec(app.rs.surface_grid ? "surface grid off" : "surface grid on", true); break;
    case 'm': console_exec(app.rs.surface_mode == SURFACE_FIELD ? "surface solid" : "surface field", true); break;
    case 'h': app.hud_on = !app.hud_on; break;
    case 'f':
        if (app.workspace == WS_SOLID) console_exec("fem fit", true);
        else app_camera_preset("model");
        break;
    case '0': app_camera_preset("iso"); break;
    case 'r': console_exec("reset", true); break;
    case 'k': console_exec(app.cam.locked ? "camera unlock" : "camera lock", true); break;
    case 'c':
        snprintf(cmd, sizeof cmd, "cmap %s", colormap_name((app.rs.cmap + 1) % CMAP_COUNT));
        console_exec(cmd, true);
        break;
    case KEY_LEFT: camera_orbit(&app.cam, -40, 0); break;
    case KEY_RIGHT: camera_orbit(&app.cam, 40, 0); break;
    case KEY_UP: camera_orbit(&app.cam, 0, 30); break;
    case KEY_DOWN: camera_orbit(&app.cam, 0, -30); break;
    case '=': case '+': camera_zoom(&app.cam, 0.85f); break;
    case '-': camera_zoom(&app.cam, 1.18f); break;
    case ',': console_exec("playback slower", true); break;
    case '.': console_exec("playback faster", true); break;
    case '/': console_exec("step 1", true); break;
    default: break;
    }
}

static double wrap_deg(double a) {
    a = fmod(a + 180.0, 360.0);
    return (a < 0 ? a + 360.0 : a) - 180.0;
}

/* Option-drag: left/right yaws the model, up/down pitches it, Shift+left/right rolls it. The drawn model follows
 * every mouse move; the solver re-voxelises at ~15 Hz and once more on release. */
static void drag_model(const PlatformEvent *e) {
    if (!app.has_model) return;
    if (!app.press_moved && fabs(e->x - app.press_x) + fabs(e->y - app.press_y) < 4) return;
    app.press_moved = true;
    const double k = 0.37; /* degrees per point: the camera orbit rate, so both drags feel alike */
    if (e->mods & MOD_SHIFT) {
        app.params.roll = wrap_deg(app.params.roll + e->dx * k);
    } else {
        app.params.yaw = wrap_deg(app.params.yaw + e->dx * k);
        app.params.aoa = CLAMP(app.params.aoa - e->dy * k, -90.0, 90.0);
    }
    double now = now_seconds();
    if (now - app.drag_push_t >= 1.0 / 15.0) {
        app.drag_push_t = now;
        app_push_params();
    } else {
        app_update_model();
    }
}

static void finish_model_drag(void) {
    app.model_drag = false;
    if (app.press_moved) {
        app_push_params();
        LOGI("model attitude: pitch %+.1f\xC2\xB0, yaw %+.1f\xC2\xB0, roll %+.1f\xC2\xB0", app.params.aoa, app.params.yaw,
             app.params.roll);
        return;
    }
    vec3 hit;
    if (app_pick(app.press_x, app.press_y, &hit)) {
        app.probe = hit;
        app.probe_on = true;
        console_exec("probe", false);
    }
}

static bool rake_visible(void) { return app.hud_on && app.rs.stream_on && app.panel_tab == PANEL_LINES && app.nx > 0; }

/* the rake handle's position in window points; false when hidden or behind the camera */
bool app_rake_handle(float *px, float *py) {
    if (!rake_visible() || !camera_project(&app.cam, app.rake_pos, px, py)) return false;
    *px /= app.scale, *py /= app.scale;
    return true;
}

static bool rake_handle_hit(double x, double y) {
    float hx, hy;
    return app_rake_handle(&hx, &hy) && (x - hx) * (x - hx) + (y - hy) * (y - hy) < 16.0 * 16.0;
}

/* dragging the handle moves the rake in the plane facing the camera, at the handle's depth */
static void drag_rake(const PlatformEvent *e) {
    const vec3 d = v3_sub(app.rake_pos, app.cam.eye);
    const float depth = MAXI(v3_dot(d, app.cam.forward), 1.0f);
    const float wpp = 2.0f * depth * tanf(DEG2RAD(app.cam.fov_deg) * 0.5f) / (float)MAXI(app.win_h, 1);
    vec3 p = v3_add(app.rake_pos, v3_add(v3_scale(app.cam.right, (float)e->dx * wpp), v3_scale(app.cam.up, (float)-e->dy * wpp)));
    p.x = CLAMP(p.x, 0.6f, app.nx - 0.6f), p.y = CLAMP(p.y, 0.6f, app.ny - 0.6f), p.z = CLAMP(p.z, 0.6f, app.nz - 0.6f);
    app.rake_pos = p;
    app.rake_manual = true;
}

/* outline of the current rake plus a handle cross, as GL_LINES pairs in lattice space */
static int rake_overlay(float *v, int cap) {
    if (!rake_visible() && !app.rake_drag) return 0;
    int n = 0;
    #define SEG(A, B) do { if (n + 2 <= cap) { v[3 * n] = (A).x, v[3 * n + 1] = (A).y, v[3 * n + 2] = (A).z, n++; \
                                               v[3 * n] = (B).x, v[3 * n + 1] = (B).y, v[3 * n + 2] = (B).z, n++; } } while (0)
    const vec3 c = app.rake_pos;
    const float ha = app.rake_size[0] * 0.5f, hb = app.rake_size[1] * 0.5f;
    const vec3 X = v3(1, 0, 0), Y = v3(0, 1, 0), Z = v3(0, 0, 1);
    switch (app.seed_mode) {
    case SEED_GRID:
    case SEED_WAKE: {
        const int ax = CLAMP(app.rake_axis, 0, 2);
        const vec3 u = v3_scale(ax == 0 ? Y : X, ha), w = v3_scale(ax == 2 ? Y : Z, hb);
        const vec3 p0 = v3_sub(v3_sub(c, u), w), p1 = v3_add(v3_sub(c, w), u), p2 = v3_add(v3_add(c, u), w), p3 = v3_add(v3_sub(c, u), w);
        SEG(p0, p1);
        SEG(p1, p2);
        SEG(p2, p3);
        SEG(p3, p0);
        break;
    }
    case SEED_LINE: SEG(v3_sub(c, v3_scale(Y, ha)), v3_add(c, v3_scale(Y, ha))); break;
    case SEED_POINT:
        SEG(v3_sub(c, v3_scale(X, ha)), v3_add(c, v3_scale(X, ha)));
        SEG(v3_sub(c, v3_scale(Y, ha)), v3_add(c, v3_scale(Y, ha)));
        SEG(v3_sub(c, v3_scale(Z, ha)), v3_add(c, v3_scale(Z, ha)));
        break;
    case SEED_RANDOM: {
        const float hx = MAXI(ha, hb);
        for (int i = 0; i < 4; i++) { /* the 12 edges of the box, grouped by direction */
            const float sy = (i & 1) ? ha : -ha, sz = (i & 2) ? hb : -hb, sx = (i & 1) ? hx : -hx;
            SEG(v3_add(c, v3(-hx, sy, sz)), v3_add(c, v3(hx, sy, sz)));
            SEG(v3_add(c, v3(sx, -ha, sz)), v3_add(c, v3(sx, ha, sz)));
            SEG(v3_add(c, v3(sx, sy * (ha > 0 ? 1.0f : 0.0f), -hb)), v3_add(c, v3(sx, sy, hb)));
        }
        break;
    }
    default: SEG(v3_sub(c, v3_scale(Z, hb)), v3_add(c, v3_scale(Z, hb))); break;
    }
    const float hs = 2.5f;
    SEG(v3_sub(c, v3(hs, 0, 0)), v3_add(c, v3(hs, 0, 0)));
    SEG(v3_sub(c, v3(0, hs, 0)), v3_add(c, v3(0, hs, 0)));
    SEG(v3_sub(c, v3(0, 0, hs)), v3_add(c, v3(0, 0, hs)));
    #undef SEG
    return n;
}

static bool over_panel(double x, double y) { return app.hud_on && x >= app.win_w - app.panel_w - 10 && y > TOOLBAR_Y_LIMIT; }

/* Cmd-drag rectangle: move the pivot to what is under the box and zoom until the box fills the view */
static void finish_box_zoom(void) {
    app.box_zoom = false;
    double w = fabs(app.box_x1 - app.box_x0), h = fabs(app.box_y1 - app.box_y0);
    if (w < 8 || h < 8) return;
    double cx = (app.box_x0 + app.box_x1) * 0.5, cy = (app.box_y0 + app.box_y1) * 0.5;
    vec3 hit;
    if (!app_pick(cx, cy, &hit)) {
        vec3 o, d;
        camera_ray(&app.cam, (float)(cx * app.scale), (float)(cy * app.scale), &o, &d);
        float denom = v3_dot(d, app.cam.forward);
        if (denom < 1e-4f) return;
        float t = v3_dot(v3_sub(app.cam.target, o), app.cam.forward) / denom;
        hit = v3_add(o, v3_scale(d, t));
    }
    double vw = app.hud_on ? app.win_w - app.panel_w - 10 : app.win_w;
    double frac = MAXI(w / MAXI(vw, 1.0), h / MAXI((double)app.win_h, 1.0));
    camera_focus(&app.cam, hit, app.cam.dist_goal * (float)CLAMP(frac, 0.04, 1.0));
    LOGI("zoomed into region around (%.1f, %.1f, %.1f)", hit.x, hit.y, hit.z);
}

static void handle_event(const PlatformEvent *e) {
    switch (e->type) {
    case EV_FILE_DROP: {
        const char *ext = strrchr(e->path, '.');
        char cmd[1200];
        if (ext && str_ieq(ext, ".stl")) snprintf(cmd, sizeof cmd, "load \"%s\"", e->path);
        else if (ext && (str_ieq(ext, ".nav") || str_ieq(ext, ".txt"))) snprintf(cmd, sizeof cmd, "exec \"%s\"", e->path);
        else {
            LOGW("drop an .stl model (or a .nav command script)");
            break;
        }
        console_exec(cmd, true);
        break;
    }
    case EV_TEXT:
        if (g_focus_swallow && e->codepoint == g_focus_swallow) { /* the key that opened the terminal, not input for it */
            g_focus_swallow = 0;
            break;
        }
        g_focus_swallow = 0;
        if (hud_text_focused()) { /* a field in the panel is being typed into */
            hud_text_key(e->codepoint);
            break;
        }
        if (console_focused() && console_handle_event(e)) break;
        break;
    case EV_KEY_DOWN:
        if (hud_text_focused() && (e->key == KEY_ENTER || e->key == KEY_ESCAPE || e->key == KEY_BACKSPACE)) {
            hud_text_key(e->key);
            break;
        }
        if (console_focused() && console_handle_event(e)) break;
        hotkey(e);
        break;
    case EV_MOUSE_DOWN:
        g_input.pressed[e->button] = true;
        g_input.down[e->button] = true;
        g_input.clicks = e->clicks;
        g_input.mx = (float)e->x, g_input.my = (float)e->y;
        if (ui_wants_mouse_at(app.ui, (float)e->x, (float)e->y)) break;
        console_set_focus(false);
        app.press_x = e->x, app.press_y = e->y, app.press_moved = false; /* a click that does not move can pick */
        if (e->button == 0 && (e->mods & MOD_ALT)) {
            /* Option-drag turns the model; an Option-click that doesn't move places the probe on release */
            app.model_drag = true;
            app.press_x = e->x, app.press_y = e->y;
            app.press_moved = false;
        } else if (e->button == 0 && e->clicks >= 2) {
            vec3 hit;
            if (app_pick(e->x, e->y, &hit)) {
                camera_focus(&app.cam, hit, app.cam.dist_goal * 0.55f);
                LOGI("focus (%.1f, %.1f, %.1f)", hit.x, hit.y, hit.z);
            }
        } else if (e->button == 0 && hud_box_mode()) {
            /* BOX is armed in the HOLD & LOAD step: the drag collects faces, shift-drag drops them */
            app.box_pick = true;
            app.box_pick_remove = (e->mods & MOD_SHIFT) != 0;
            app.box_x0 = app.box_x1 = e->x;
            app.box_y0 = app.box_y1 = e->y;
        } else if (e->button == 0 && (e->mods & MOD_CMD)) {
            app.box_zoom = true;
            app.box_x0 = app.box_x1 = e->x;
            app.box_y0 = app.box_y1 = e->y;
        } else if (e->button == 0 && !(e->mods & MOD_SHIFT) && rake_handle_hit(e->x, e->y)) {
            app.rake_drag = true;
        } else if (e->button == 0 && !(e->mods & MOD_SHIFT)) {
            app.orbiting = true;
        } else {
            app.panning = true;
        }
        break;
    case EV_MOUSE_UP:
        g_input.released[e->button] = true;
        g_input.down[e->button] = false;
        app.orbiting = app.panning = app.rake_drag = false;
        if (app.box_zoom) finish_box_zoom();
        if (app.box_pick) {
            app.box_pick = false;
            fem_pick_box((float)app.box_x0, (float)app.box_y0, (float)app.box_x1, (float)app.box_y1,
                         app.box_pick_remove, hud_box_front_only());
        }
        if (app.model_drag) finish_model_drag();
        /* in the HOLD & LOAD step a left click that did not turn the camera picks the face under the cursor */
        if (e->button == 0 && !app.press_moved && !app.model_drag && hud_pick_mode() &&
            !ui_wants_mouse_at(app.ui, (float)e->x, (float)e->y)) {
            if (hud_simple_face_mode()) hud_simple_face_clicked((float)e->x, (float)e->y); /* "which face sits on the plate" */
            else fem_pick_at((float)e->x, (float)e->y, (e->mods & MOD_SHIFT) != 0);
        }
        break;
    case EV_MOUSE_MOVE:
        g_input.mx = (float)e->x, g_input.my = (float)e->y;
        if (app.box_zoom || app.box_pick) app.box_x1 = e->x, app.box_y1 = e->y;
        if (app.model_drag) drag_model(e);
        if (app.rake_drag) drag_rake(e);
        if (app.orbiting && labapp_active()) labapp_orbit((float)e->dx, (float)e->dy);
        else if (app.orbiting) camera_orbit(&app.cam, (float)e->dx, (float)e->dy);
        if (app.panning) camera_pan(&app.cam, (float)(e->dx * app.scale), (float)(e->dy * app.scale));
        if (!app.orbiting && !app.panning && !app.model_drag && hud_pick_mode() &&
            !ui_wants_mouse_at(app.ui, (float)e->x, (float)e->y))
            fem_hover_at((float)e->x, (float)e->y); /* the face under the cursor lights up */
        break;
    case EV_SCROLL:
        if (over_panel(e->x, e->y)) {
            /* Simple mode has no terminal: the wheel scrolls the screen when it is taller than the panel */
            if (app.ui_mode == UI_SIMPLE) hud_simple_scroll((float)e->dy);
            else console_scroll((float)(e->dy / 14.0));
            break;
        }
        if (ui_wants_mouse_at(app.ui, (float)e->x, (float)e->y)) break;
        if (labapp_active()) {
            labapp_zoom((float)exp(e->dy * (e->precise ? 0.01 : 0.012)));
            break;
        }
        if (e->precise) {
            if (e->mods & MOD_SHIFT) camera_pan(&app.cam, (float)(-e->dx * app.scale), (float)(-e->dy * app.scale));
            else if (e->mods & (MOD_CMD | MOD_CTRL)) camera_zoom_at(&app.cam, (float)exp(-e->dy * 0.01), (float)(e->x * app.scale), (float)(e->y * app.scale));
            else camera_orbit(&app.cam, (float)(-e->dx), (float)(-e->dy));
        } else {
            camera_zoom_at(&app.cam, (float)exp(-e->dy * 0.012), (float)(e->x * app.scale), (float)(e->y * app.scale));
        }
        break;
    case EV_MAGNIFY:
        if (!ui_wants_mouse_at(app.ui, (float)e->x, (float)e->y))
            camera_zoom_at(&app.cam, (float)exp(-e->magnify * 1.6), (float)(e->x * app.scale), (float)(e->y * app.scale));
        break;
    default: break;
    }
}

/* queued clicks never share a frame: two presses in one frame would collide and only the last position would be pressed */
static uint64_t g_synth_until;

int app_inject_click(float x, float y, int hold_frames) {
    if (g_nsynth + 3 > (int)ARRAY_LEN(g_synth)) return 0;
    uint64_t base = g_frame + 1;
    if (g_synth_until >= base) base = g_synth_until + 1;
    PlatformEvent e;
    memset(&e, 0, sizeof e);
    e.x = x, e.y = y, e.clicks = 1;
    e.type = EV_MOUSE_MOVE;
    g_synth[g_nsynth++] = (SyntheticEvent){base, e};
    e.type = EV_MOUSE_DOWN;
    g_synth[g_nsynth++] = (SyntheticEvent){base + 1, e};
    e.type = EV_MOUSE_UP;
    g_synth_until = base + 1 + (uint64_t)MAXI(hold_frames, 0);
    g_synth[g_nsynth++] = (SyntheticEvent){g_synth_until, e};
    return (int)(g_synth_until - g_frame) + 1;
}

/* press at (x0, y0), move to (x1, y1) over `frames` frames with modifier keys held, release */
int app_inject_drag(float x0, float y0, float x1, float y1, uint32_t mods, int frames) {
    int n = CLAMP(frames, 2, 60);
    if (g_nsynth + n + 3 > (int)ARRAY_LEN(g_synth)) return 0;
    uint64_t base = g_frame + 1;
    if (g_synth_until >= base) base = g_synth_until + 1;
    PlatformEvent e;
    memset(&e, 0, sizeof e);
    e.x = x0, e.y = y0, e.mods = mods, e.clicks = 1;
    e.type = EV_MOUSE_MOVE;
    g_synth[g_nsynth++] = (SyntheticEvent){base, e};
    e.type = EV_MOUSE_DOWN;
    g_synth[g_nsynth++] = (SyntheticEvent){base + 1, e};
    double px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        e.type = EV_MOUSE_MOVE;
        e.x = x0 + (x1 - x0) * (float)i / n, e.y = y0 + (y1 - y0) * (float)i / n;
        e.dx = e.x - px, e.dy = e.y - py;
        px = e.x, py = e.y;
        g_synth[g_nsynth++] = (SyntheticEvent){base + 1 + (uint64_t)i, e};
    }
    e.type = EV_MOUSE_UP, e.dx = e.dy = 0;
    g_synth_until = base + 2 + (uint64_t)n;
    g_synth[g_nsynth++] = (SyntheticEvent){g_synth_until, e};
    return (int)(g_synth_until - g_frame) + 1;
}

/* a wheel turn at (x, y): the same event a mouse or trackpad sends */
int app_inject_scroll(float x, float y, float dy) {
    if (g_nsynth + 2 > (int)ARRAY_LEN(g_synth)) return 0;
    uint64_t base = g_frame + 1;
    if (g_synth_until >= base) base = g_synth_until + 1;
    PlatformEvent e;
    memset(&e, 0, sizeof e);
    e.x = x, e.y = y;
    e.type = EV_MOUSE_MOVE;
    g_synth[g_nsynth++] = (SyntheticEvent){base, e};
    e.type = EV_SCROLL, e.dy = dy;
    g_synth_until = base + 1;
    g_synth[g_nsynth++] = (SyntheticEvent){g_synth_until, e};
    return (int)(g_synth_until - g_frame) + 1;
}

/* A key press traverses handle_event, exactly like the platform's keyboard events. */
int app_inject_key(int key) {
    if (g_nsynth + 2 > (int)ARRAY_LEN(g_synth)) return 0;
    uint64_t base = g_frame + 1;
    if (g_synth_until >= base) base = g_synth_until + 1;
    PlatformEvent e = {0};
    e.type = EV_KEY_DOWN; e.key = key;
    g_synth[g_nsynth++] = (SyntheticEvent){base, e};
    e.type = EV_KEY_UP;
    g_synth_until = base + 1;
    g_synth[g_nsynth++] = (SyntheticEvent){g_synth_until, e};
    return (int)(g_synth_until - g_frame) + 1;
}

/* deliver synthetic events that are due, in order, through the normal event path */
static void inject_due_events(void) {
    int kept = 0;
    for (int i = 0; i < g_nsynth; i++) {
        if (g_synth[i].frame <= g_frame)
            handle_event(&g_synth[i].ev);
        else
            g_synth[kept++] = g_synth[i];
    }
    g_nsynth = kept;
}

static void usage(void) {
    printf("NAVIER - real-time 3D lattice-Boltzmann water tunnel\n"
           "usage: navier [options]\n"
           "  --scene <name>        glider sphere cylinder car submarine wing airfoil plate torus (default glider)\n"
           "  --load <file.stl>     start with an STL model\n"
           "  --exec \"cmd; cmd\"     run terminal commands at start-up\n"
           "  --run                 start the experiment immediately\n"
           "  --size <W>x<H>        window size in points (default 1440x900)\n"
           "  --headless            render off-screen (use with --steps and --shot)\n"
           "  --steps <n>           headless: simulate n steps before the screenshot\n"
           "  --shot <file.png>     save a screenshot and exit (headless, or windowed with --quit-after)\n"
           "  --quit-after <sec>    windowed: exit after this many seconds\n"
           "  --listen [socket]     serve the NAVIER-AM control socket (default ~/.navier/run/control.sock)\n"
           "  --first-run           open as a new user would: Simple mode, the sample parts, nothing remembered\n"
           "  --workspace <dir>     parent folder of NAVIER-AM projects (default ~/NAVIER-Projects)\n");
}

int main(int argc, char **argv) {
    const char *scene = "glider", *load = NULL, *exec = NULL, *shot = NULL;
    bool scene_given = false;
    int W = 1440, H = 900;
    long steps = 0;
    bool run = false;
    double quit_after = 0;
    bool listen = false;
    const char *listen_path = NULL;
    const char *am_workspace = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) app.headless = true;
        else if (!strcmp(argv[i], "--run")) run = true;
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i], scene_given = true;
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) load = argv[++i];
        else if (!strcmp(argv[i], "--exec") && i + 1 < argc) exec = argv[++i];
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = atol(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--quit-after") && i + 1 < argc) quit_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--workspace") && i + 1 < argc) am_workspace = argv[++i];
        else if (!strcmp(argv[i], "--first-run")) app.first_run = true; /* behave as a new install: Simple, samples */
        else if (!strcmp(argv[i], "--listen")) {
            listen = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') listen_path = argv[++i];
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { fprintf(stderr, "unknown option %s\n", argv[i]); usage(); return 1; }
    }

    /* launched from Finder (cwd "/"): keep screenshots and exports in ~/Documents/NAVIER */
    char cwd[1024];
    if (getcwd(cwd, sizeof cwd) && strcmp(cwd, "/") == 0 && getenv("HOME")) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/Documents/NAVIER", getenv("HOME"));
        mkdir(dir, 0755);
        if (chdir(dir) != 0) fprintf(stderr, "could not enter %s\n", dir);
    }

    console_init();
    if (!platform_init("NAVIER", W, H, app.headless)) {
        fprintf(stderr, "could not create an OpenGL 4.1 context\n");
        return 1;
    }
    GLTarget headless_target = {0};
    if (app.headless) {
        gl_target_create(&headless_target, W, H, GL_RGBA8, false, 1);
        app.target_fbo = headless_target.fbo;
    }

    sim_params_default(&app.params);
    render_settings_default(&app.rs);
    app.display_field = DF_SPEED;
    app.seed_mode = SEED_PLANE;
    app.seed_count = 400;
    app.hud_on = true;
    app.playback = 1.0;
    app.max_fps = 60.0;
    app.stats_field = app.range_field = -1;
    app.panel_w = 430;
    app.scale = (float)platform_backing_scale();
    if (app.scale >= 1.5f) app.rs.msaa = 2; /* Retina: pixel density already smooths edges */

    app.sim = sim_create(&app.params);
    app.renderer = render_create();
    app.ui = ui_create(app.scale);
    app.worker = visworker_create(app.sim, 3);
    app.req.version = 1;
    register_commands();
    {
        /* the additive-manufacturing engine: the same typed operations as navier-server and navier-mcp */
        char am_err[1024];
        EngineConfig am_cfg;
        engine_config_default(&am_cfg);
        if (am_workspace && !engine_config_set_workspace(&am_cfg, am_workspace, am_err, sizeof am_err))
            LOGW("--workspace %s: %s", am_workspace, am_err);
        /* the sample parts and presets ship inside the application: the engine may read them wherever the
         * application is installed (/Applications is outside the home folder the default roots cover) */
        {
            char res[1024];
            if (app_resource_path("samples/samples.json", res, sizeof res)) {
                char *cut = strstr(res, "/samples/samples.json");
                if (cut) *cut = 0;
                char e2[256];
                if (!engine_config_add_root(&am_cfg, false, res, e2, sizeof e2)) LOGW("the sample parts cannot be read: %s", e2);
            }
        }
        app.engine = engine_create(&am_cfg, am_err, sizeof am_err);
        if (!app.engine) LOGW("NAVIER-AM engine unavailable: %s", am_err);
        fem_init();
    }
    app_update_model();
    camera_init(&app.cam, v3(app.nx * 0.5f, app.ny * 0.5f, app.nz * 0.5f), (float)app.nx * 1.3f, 212, 32);

    LOGOK("NAVIER \xE2\x80\x94 real-time lattice-Boltzmann water tunnel");
    LOGI("GPU %s \xC2\xB7 OpenGL %s \xC2\xB7 %d CPU threads", glGetString(GL_RENDERER), glGetString(GL_VERSION), cpu_count());
    LOGI("type 'help' for commands, 'scene' for experiments, drop an .stl onto the window to test your own shape");
    if (listen && app.engine) {
        char ctl_err[1024];
        CtlServerConfig sc;
        ctl_server_config_default(&sc);
        if (listen_path) snprintf(sc.unix_path, sizeof sc.unix_path, "%s", listen_path);
        signal(SIGPIPE, SIG_IGN);
        app.ctl = ctl_server_start(app.engine, &sc, ctl_err, sizeof ctl_err);
        if (app.ctl) LOGOK("control socket %s: navier-ctl and navier-mcp --connect drive this window", sc.unix_path);
        else LOGE("control socket: %s", ctl_err);
    }

    /* open where the last session left off. A new user (nothing remembered, or --first-run) starts in Simple mode;
     * a headless run remembers nothing and starts in Advanced, which is what every existing script is written for,
     * unless --first-run asks for the newcomer's window. */
    int start_mode = app_remembered_ui_mode();
    /* two modes (owner, 2026-09-26): a person starts in Advanced; a remembered Simple opens as Advanced too */
    if (start_mode < 0 || start_mode == UI_SIMPLE) start_mode = UI_ADVANCED;
    if (load) {
        app_load_stl(load);
        app_camera_preset("iso");
    } else if (start_mode == UI_ADVANCED || scene_given) {
        app_apply_scene(scene);
    } else {
        /* Simple and Agent open on the sample parts over a neutral view: the tunnel and its lattice (half a
         * gigabyte) wait until someone opens the flow workspace */
        snprintf(app.scene_pending, sizeof app.scene_pending, "%s", scene);
    }
    {
        int mode = start_mode;
        if (mode == UI_ADVANCED) {
            app.ui_mode = UI_ADVANCED;
            int ws = app_remembered_workspace();
            if (ws >= 0 && ws != app.workspace) app_set_workspace(ws);
        } else {
            app.ui_mode = mode;
            app_set_workspace(WS_SOLID);
        }
        fem_set_supports_neutral(app.ui_mode == UI_SIMPLE);
    }
    camera_snap(&app.cam);
    render_set_result_colormap(app.renderer, CMAP_VIRIDIS); /* colour-blind safe; click the legend to cycle palettes */
    if (exec) console_exec(exec, true);
    if (run || (app.headless && steps > 0)) sim_run(app.sim, true);
    if (shot && !(quit_after > 0 && !app.headless)) app_request_screenshot(shot), app.shot_countdown = 0;

    double last = now_seconds(), fps_t0 = last, start_time = last;
    int fps_frames = 0;
    bool shot_armed = false;
    PlatformEvent events[256];
    double perf_t0 = now_seconds();
    while (!app.quit && !platform_should_quit()) {
        const double t_start = now_seconds();
        memset(g_input.pressed, 0, sizeof g_input.pressed);
        memset(g_input.released, 0, sizeof g_input.released);
        int n = platform_poll_events(events, 256);
        for (int i = 0; i < n; i++) handle_event(&events[i]);
        inject_due_events();
        g_frame++;
        for (int b = 0; b < 3; b++) g_input.down[b] = platform_mouse_down(b) || (g_input.down[b] && !g_input.released[b]);
        const double t_events = now_seconds();

        double now = now_seconds();
        app.dt = MINI(now - last, 0.1);
        last = now;
        app.time = now;
        if (++fps_frames >= 20) {
            app.fps = fps_frames / (now - fps_t0);
            fps_t0 = now, fps_frames = 0;
        }

        sim_status(app.sim, &app.status);
        commands_update();
        /* the analysis side of the application: mirror the engine and keep the result surface current */
        fem_poll();
        agent_poll(); /* an agent's output is read every frame, whichever mode is on screen */
        {
            static uint64_t uploaded_gen = 0;
            uint64_t gen = fem_surface_generation();
            if (gen != uploaded_gen) {
                render_set_result(app.renderer, fem_surface());
                uploaded_gen = gen;
            }
        }
        SimSnapshot *s = sim_acquire_snapshot(app.sim, app.snap ? app.snap->serial : 0);
        if (s) {
            sim_release_snapshot(app.sim, app.snap);
            app.snap = s;
        }
        if (app.status.nx > 0 && (app.status.nx != app.nx || app.status.ny != app.ny || app.status.nz != app.nz)) {
            app.nx = app.status.nx, app.ny = app.status.ny, app.nz = app.status.nz;
            app_update_model();
        }
        /* particles advance at the solver's measured step rate rather than in whole steps per frame (43 steps/s
         * at 60 fps would move them 1, 1, 0, 1, 0 ... steps: visible jitter); the debt keeps them in sync */
        uint64_t step = app.status.step;
        double fresh = step >= app.prev_step ? (double)(step - app.prev_step) : 0.0;
        app.prev_step = step;
        app.particle_debt = MINI(app.particle_debt + fresh, 48.0);
        double adv = app.status.running ? MINI(app.particle_debt, app.status.steps_per_sec * app.dt) : app.particle_debt;
        if (app.particle_debt - adv > 3.0) adv = app.particle_debt - 3.0;
        app.particle_debt -= adv;
        app.particle_steps = (float)MINI(adv, 12.0);

        set_request();
        consume_results();
        update_range();
        if (app.perf_sync) glFinish();
        const double t_results = now_seconds();

        platform_get_size(&app.win_w, &app.win_h, &app.fb_w, &app.fb_h);
        app.scale = app.win_w > 0 ? (float)app.fb_w / (float)app.win_w : 1.0f;
        float shift_goal = app.hud_on ? -(app.panel_w + 10.0f) / (float)MAXI(app.win_w, 1) : 0.0f;
        app.cam.shift_x += (shift_goal - app.cam.shift_x) * (float)MINI(1.0, app.dt * 10.0);
        camera_update(&app.cam, app.dt, app.fb_w, app.fb_h);

        /* before the first step the field is uniformly zero: show the bare material instead of a black model */
        RenderSettings rs_frame = app.rs;
        if (app.status.step == 0 && rs_frame.surface_mode == SURFACE_FIELD) rs_frame.surface_mode = SURFACE_MATERIAL;
        if (app.ui_mode != UI_ADVANCED && app.workspace == WS_SOLID) {
            /* Simple and Agent: no tunnel box, and no floor until there is a part to stand on it */
            const FemState *fsn = fem_state();
            rs_frame.box_on = false;
            if (!(fsn->nbodies > 0 || fsn->have_result || fem_pending_import())) rs_frame.floor_on = false;
            /* the shading costs about 3 ms a frame on the owner's assembly: playback in Simple mode keeps the frames */
            if (fem_playing()) rs_frame.ao_on = false;
        }
        labapp_tick(app.dt);
        if (labapp_active()) {
            /* a physics-lab result is open: its frame fills the window beside the panel */
            glBindFramebuffer(GL_FRAMEBUFFER, app.target_fbo);
            glViewport(0, 0, app.fb_w, app.fb_h);
            glClearColor(0.055f, 0.067f, 0.10f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            int lx1 = app.hud_on ? app.fb_w - (int)((app.panel_w + 10.0f) * app.scale) : app.fb_w;
            int ly1 = app.hud_on ? app.fb_h - (int)(84.0f * app.scale) : app.fb_h; /* below the toolbar and the lab's status line */
            int ly0 = app.hud_on ? (int)(36.0f * app.scale) : 0;                   /* above the hint line */
            labapp_draw(app.target_fbo, 0, lx1, ly0, ly1);
        } else {
            RenderFrame f;
            fill_render_frame(&f, &rs_frame, now);
            f.target_fbo = app.target_fbo;
            f.target_w = app.fb_w, f.target_h = app.fb_h;
            render_frame(app.renderer, &f);
        }
        if (app.perf_sync) glFinish();
        const double t_render = now_seconds();

        g_input.mods = platform_mods();
        ui_begin(app.ui, app.fb_w, app.fb_h, app.scale, &g_input);
        hud_draw();
        ui_end(app.ui, app.target_fbo);
        if (app.perf_sync) glFinish();
        const double t_ui = now_seconds();
        if (!app.headless) {
            /* affordance: pointing hand over anything clickable, I-beam over the terminal */
            const UiWidgetInfo *hw = ui_widget_at(app.ui, g_input.mx, g_input.my);
            PlatformCursor cur = CURSOR_ARROW;
            if (app.orbiting || app.panning || app.rake_drag || (app.model_drag && app.press_moved)) cur = CURSOR_GRABBING;
            else if (hw) cur = (hw->kind == 2 && !strcmp(hw->id, "terminal")) ? CURSOR_IBEAM : CURSOR_HAND;
            else if (rake_handle_hit(g_input.mx, g_input.my)) cur = CURSOR_HAND;
            platform_set_cursor(cur);
        }

        if (app.shot_path[0]) {
            bool ready = !app.headless || (steps <= 0 ? true : (app.status.step >= (uint64_t)steps));
            if (app.headless && steps > 0 && ready && !shot_armed) {
                sim_run(app.sim, false);
                shot_armed = true;
                app.shot_countdown = 45; /* let the vis worker catch up with the final snapshot */
            } else if (!app.headless || steps <= 0) {
                if (!shot_armed) shot_armed = true, app.shot_countdown = app.headless ? 45 : 1;
            }
            if (shot_armed && --app.shot_countdown <= 0) {
                save_screenshot();
                app.shot_path[0] = 0;
                shot_armed = false;
                if (app.headless && !commands_pending()) app.quit = true; /* scripts carry on after mid-run shots */
            }
        }
        if (quit_after > 0 && !app.headless && now - start_time > quit_after) {
            if (shot) {
                str_copy(app.shot_path, sizeof app.shot_path, shot);
                save_screenshot();
                app.shot_path[0] = 0;
            }
            app.quit = true;
        }
        platform_swap();
        const double t_swap = now_seconds();
        {
            const double k = 0.1;
            app.perf_events += ((t_events - t_start) * 1e3 - app.perf_events) * k;
            app.perf_results += ((t_results - t_events) * 1e3 - app.perf_results) * k;
            app.perf_render += ((t_render - t_results) * 1e3 - app.perf_render) * k;
            app.perf_ui += ((t_ui - t_render) * 1e3 - app.perf_ui) * k;
            app.perf_swap += ((t_swap - t_ui) * 1e3 - app.perf_swap) * k;
            app.perf_frame += ((t_swap - t_start) * 1e3 - app.perf_frame) * k;
            if (t_swap - perf_t0 >= 1.0) {
                app.upload_mb_s = g_upload_bytes / 1048576.0 / (t_swap - perf_t0);
                app.field_uploads_s = g_field_uploads / (t_swap - perf_t0);
                g_upload_bytes = 0, g_field_uploads = 0, perf_t0 = t_swap;
            }
        }
        if (app.headless) {
            usleep(8000);
        } else if (app.max_fps > 0) {
            /* frames beyond the display rate are never seen and take CPU time from the solver */
            double spare = 1.0 / app.max_fps - (now_seconds() - t_start);
            if (spare > 0.0005) usleep((useconds_t)(spare * 1e6));
        }
    }

    sim_run(app.sim, false);
    ctl_server_stop(app.ctl);
    fem_shutdown();
    engine_destroy(app.engine);
    visworker_destroy(app.worker);
    sim_release_snapshot(app.sim, app.snap);
    sim_destroy(app.sim);
    ui_destroy(app.ui);
    render_destroy(app.renderer);
    if (app.headless) gl_target_destroy(&headless_target);
    log_set_sink(NULL);
    platform_shutdown();
    return 0;
}
