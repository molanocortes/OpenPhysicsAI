/* commands.c - terminal commands */
#include <dirent.h>
#include "app.h"
#include "labapp.h"
#include "colormap.h"
#include "console.h"
#include "geom/shapes.h"
#include "platform.h"
#include "ctl/ops.h"
#include "fembridge.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>

/* ---- parsing helpers ------------------------------------------------------------------------------ */

typedef struct {
    const char *suffix;
    double factor;
} UnitDef;

static const UnitDef LENGTH_UNITS[] = {{"m", 1},        {"cm", 1e-2},     {"mm", 1e-3},     {"um", 1e-6},
                                       {"\xC2\xB5m", 1e-6}, {"micron", 1e-6}, {"in", 0.0254}, {"ft", 0.3048},
                                       {NULL, 0}};
static const UnitDef SPEED_UNITS[] = {{"m/s", 1},        {"mps", 1},         {"cm/s", 1e-2},   {"mm/s", 1e-3},
                                      {"km/h", 1 / 3.6}, {"kmh", 1 / 3.6},  {"kph", 1 / 3.6}, {"kn", 0.514444},
                                      {"knots", 0.514444}, {"kt", 0.514444}, {"mph", 0.44704}, {"ft/s", 0.3048},
                                      {NULL, 0}};

static bool parse_number(const char *s, double *out) {
    char *end;
    double v = strtod(s, &end);
    if (end == s) {
        LOGE("'%s' is not a number", s);
        return false;
    }
    *out = v;
    return true;
}

static bool parse_quantity(int argc, char **argv, int i, const UnitDef *units, double *out) {
    if (i >= argc) return false;
    char *end;
    double v = strtod(argv[i], &end);
    if (end == argv[i]) {
        LOGE("'%s' is not a number", argv[i]);
        return false;
    }
    const char *u = *end ? end : (i + 1 < argc ? argv[i + 1] : "");
    if (*u) {
        bool found = false;
        for (int k = 0; units[k].suffix; k++)
            if (str_ieq(u, units[k].suffix)) {
                v *= units[k].factor;
                found = true;
                break;
            }
        if (!found && *end) {
            LOGE("unknown unit '%s'", u);
            return false;
        }
    }
    *out = v;
    return true;
}

/* 1 on, 0 off, 2 toggle, -1 invalid */
static int parse_onoff(const char *s) {
    if (str_ieq(s, "on") || str_ieq(s, "1") || str_ieq(s, "true") || str_ieq(s, "yes") || str_ieq(s, "show")) return 1;
    if (str_ieq(s, "off") || str_ieq(s, "0") || str_ieq(s, "false") || str_ieq(s, "no") || str_ieq(s, "hide")) return 0;
    if (str_ieq(s, "toggle")) return 2;
    return -1;
}

static bool apply_onoff(bool *flag, int argc, char **argv, int i, const char *what) {
    int v = i < argc ? parse_onoff(argv[i]) : 2;
    if (v < 0) {
        LOGE("expected on/off, got '%s'", argv[i]);
        return false;
    }
    *flag = v == 2 ? !*flag : v == 1;
    LOGI("%s %s", what, *flag ? "on" : "off");
    return true;
}

static const char *ONOFF[] = {"on", "off", NULL};
static const char *WALLS[] = {"slip", "freestream", "noslip", "moving", "periodic", NULL};
static const char *FIELDS[] = {"speed", "ux", "uy", "uz", "pressure", "cp", "vorticity", "q", NULL};
static const char *FLUIDS[] = {"water", "seawater", "air", "glycerin", "oil", "honey", "mercury", NULL};
static const char *CAMS[] = {"hero", "iso", "front", "back", "side", "top", "bottom", "model", "lock", "unlock", "orbit", "zoom", "fov", NULL};

static int wall_find(const char *s) {
    for (int i = 0; WALLS[i]; i++)
        if (str_ieq(s, WALLS[i])) return i;
    if (str_ieq(s, "wall")) return LBM_WALL_NOSLIP;
    if (str_ieq(s, "open") || str_ieq(s, "farfield")) return LBM_WALL_FREESTREAM;
    return -1;
}

static const char *complete_onoff(int argi, const char *p, int i) { return argi == 1 ? complete_from_list(ONOFF, p, i) : NULL; }
static const char *complete_walls(int argi, const char *p, int i) { return argi == 1 ? complete_from_list(WALLS, p, i) : NULL; }
static const char *complete_fields(int argi, const char *p, int i) { return argi == 1 ? complete_from_list(FIELDS, p, i) : NULL; }
static const char *complete_fluids(int argi, const char *p, int i) { return argi == 1 ? complete_from_list(FLUIDS, p, i) : NULL; }
static const char *complete_cams(int argi, const char *p, int i) { return argi == 1 ? complete_from_list(CAMS, p, i) : NULL; }
static const char *complete_scenes(int argi, const char *p, int i) { return argi == 1 ? complete_from_list(scene_names(), p, i) : NULL; }

static const char *complete_shapes(int argi, const char *p, int index) {
    if (argi != 1) return NULL;
    int k = 0;
    for (int i = 0; i < shape_count(); i++)
        if (str_starts_with_i(shape_info(i)->name, p) && k++ == index) return shape_info(i)->name;
    return NULL;
}

static const char *complete_cmaps(int argi, const char *p, int index) {
    if (argi != 1) return NULL;
    int k = 0;
    for (int i = 0; i < CMAP_COUNT; i++)
        if (str_starts_with_i(colormap_name(i), p) && k++ == index) return colormap_name(i);
    return NULL;
}

static const char *complete_sub(const char *const *subs, int argi, const char *p, int i) {
    return argi == 1 ? complete_from_list(subs, p, i) : NULL;
}
static const char *STREAM_SUBS[] = {"on", "off", "volume", "seeds", "mode", "width", "steps", "animate", "plane", "grid", "line",
                                    "wake", "random", "point", "at", "size", "normal", "auto", "probe", NULL};
static const char *PARTICLE_SUBS[] = {"on", "off", "count", "size", "emitter", "reset", NULL};
static const char *SLICE_SUBS[] = {"off", "on", "x", "y", "z", NULL};
static const char *SURFACE_SUBS[] = {"off", "solid", "field", "grid", NULL};
static const char *VORTEX_SUBS[] = {"on", "off", "level", NULL};
static const char *VOLUME_SUBS[] = {"on", "off", "density", NULL};
static const char *c_stream(int a, const char *p, int i) { return complete_sub(STREAM_SUBS, a, p, i); }
static const char *c_particles(int a, const char *p, int i) { return complete_sub(PARTICLE_SUBS, a, p, i); }
static const char *c_slice(int a, const char *p, int i) { return complete_sub(SLICE_SUBS, a, p, i); }
static const char *ORIENT_SUBS[] = {"zup", "flip", "auto", "reset", NULL};
static const char *PLAYBACK_SUBS[] = {"max", "slower", "faster", "50%", "25%", "10%", NULL};
static const char *c_surface(int a, const char *p, int i) { return complete_sub(SURFACE_SUBS, a, p, i); }
static const char *c_orient(int a, const char *p, int i) { return complete_sub(ORIENT_SUBS, a, p, i); }
static const char *c_playback(int a, const char *p, int i) { return complete_sub(PLAYBACK_SUBS, a, p, i); }
static const char *c_vortex(int a, const char *p, int i) { return complete_sub(VORTEX_SUBS, a, p, i); }
static const char *c_volume(int a, const char *p, int i) { return complete_sub(VOLUME_SUBS, a, p, i); }

static void refresh_status(void) { sim_status(app.sim, &app.status); }

/* ---- simulation ----------------------------------------------------------------------------------- */

static void cmd_start(int argc, char **argv) {
    sim_run(app.sim, true);
    LOGOK("experiment running");
}

static void cmd_pause(int argc, char **argv) {
    sim_run(app.sim, false);
    refresh_status();
    LOGI("paused at step %llu (t = %.4g s)", (unsigned long long)app.status.step, app.status.sim_time);
}

static void cmd_reset(int argc, char **argv) {
    sim_reset(app.sim);
    if (app.scene_kick > 0) sim_kick(app.sim, app.scene_kick, app.params.start_with_flow ? 0 : app.params.ramp_steps);
    render_reset_particles(app.renderer);
    LOGI("flow field reset");
}

static void cmd_kick(int argc, char **argv) {
    double amp = 0.1;
    if (argc > 1) {
        char *end;
        amp = strtod(argv[1], &end);
        if (end == argv[1] || amp <= 0) {
            LOGE("usage: kick [amplitude]   e.g. kick 0.1 = cross-flow pulse of 10%% of the free-stream speed");
            return;
        }
    }
    sim_kick(app.sim, amp, 0);
    LOGI("cross-flow pulse of %.0f%% U over ~3 convective times - breaks wake symmetry so shedding can start", amp * 100);
}

static void cmd_step(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    sim_run(app.sim, false);
    sim_step(app.sim, MAXI(n, 1));
    LOGI("advancing %d step%s", MAXI(n, 1), n == 1 ? "" : "s");
}

static void cmd_status(int argc, char **argv) {
    refresh_status();
    const SimStatus *s = &app.status;
    const SimUnits *u = &s->units;
    const SimParams *p = &app.params;
    char a[64], b[64], c[64];
    LOGD("\xE2\x94\x80\xE2\x94\x80 STATUS \xE2\x94\x80\xE2\x94\x80");
    LOGD("state      %s%s, step %llu, t = %.5g s", s->running ? "running" : "paused", s->diverged ? " (DIVERGED)" : "",
         (unsigned long long)s->step, s->sim_time);
    LOGD("solver     D3Q19 %s%s, bulk tau %.2f, LES Cs = %.3f, %d threads, %.1f MLUPS, %.1f steps/s",
         p->collision == LBM_RECURSIVE ? "recursive regularized" : p->collision ? "regularized" : "BGK",
         p->hrr_sigma > 0 && p->hrr_sigma < 0.9999 ? " + FD hybrid" : "", MAXI(p->tau_bulk, u->tau0), p->smagorinsky,
         s->threads, s->mlups, s->steps_per_sec);
    LOGD("lattice    %d x %d x %d = %.2fM cells, %.0f MB, %llu solid", s->nx, s->ny, s->nz, s->cells / 1e6, s->mem_mb,
         (unsigned long long)s->solid_cells);
    double rho, nu;
    fluid_properties(p->fluid, p->temperature, &rho, &nu);
    format_si(a, sizeof a, u->nu, "m\xC2\xB2/s");
    LOGD("fluid      %s at %.1f \xC2\xB0""C: rho = %.1f kg/m\xC2\xB3, nu = %s", fluid_label(p->fluid), p->temperature, u->rho, a);
    format_si(a, sizeof a, u->U, "m/s");
    format_si(b, sizeof b, p->ref_length, "m");
    LOGD("flow       U = %s, L = %s, Re = %.4g, lattice u = %.4f (Ma %.3f), tau0 = %.6f", a, b, u->re, u->u_lb, u->mach, u->tau0);
    if (u->re_eff > 0 && u->re_eff < 0.99 * u->re)
        LOGW("Re %.3g is beyond this lattice: its viscosity floor corresponds to Re %.3g and the resolved flow is an "
             "LES whose small scales are set by the grid - refine with 'quality' and compare before trusting numbers",
             u->re, u->re_eff);
    format_si(a, sizeof a, u->dx, "m");
    format_si(b, sizeof b, u->dt, "s");
    LOGD("units      dx = %s, dt = %s, L = %.1f cells, realtime x%.3g", a, b, u->ref_cells, s->realtime_factor);
    double cf = 0.0576 * pow(MAXI(u->re, 1e3), -0.2), utau = u->U * sqrt(cf / 2), ksp = p->roughness * utau / u->nu;
    format_si(a, sizeof a, p->roughness, "m");
    LOGD("roughness  ks = %s (%.3f cells), ks+ ~ %.3g -> %s", a, u->ks_cells, ksp,
         p->roughness <= 0 ? "smooth wall" : ksp < 5 ? "hydraulically smooth" : ksp < 70 ? "transitional rough" : "fully rough");
    format_si(a, sizeof a, s->fx, "N");
    format_si(b, sizeof b, s->fy, "N");
    format_si(c, sizeof c, s->fz, "N");
    LOGD("forces     Cd = %.4f  Cl = %.4f  Cs = %.4f   Fx = %s  Fy = %s  Fz = %s", s->cd, s->cl, s->cs, a, b, c);
    format_si(a, sizeof a, u->frontal_area, "m\xC2\xB2");
    LOGD("model      %s, frontal area %s, blockage %.1f%%, St = %.3f", app.has_model ? app.model_name : "(none)", a,
         100 * u->blockage, s->strouhal);
    if (app.has_model) {
        if (s->curved_fraction > 0)
            LOGD("surface    curved walls: interpolated bounce-back, %.1f%% of boundary links on the mesh surface",
                 100 * s->curved_fraction);
        else
            LOGD("surface    staircase walls: halfway bounce-back on voxel faces%s", p->curved_walls ? " (no link crosses the mesh)" : "");
    }
    LOGD("walls      floor %s, ceiling %s, sides %s / %s, inlet turbulence %.1f%%", WALLS[p->wall[0]], WALLS[p->wall[1]],
         WALLS[p->wall[2]], WALLS[p->wall[3]], 100 * p->turbulence);
}

/* ---- model ---------------------------------------------------------------------------------------- */

static void cmd_scene(int argc, char **argv) {
    if (argc < 2) {
        LOGI("scenes:");
        scene_list();
        return;
    }
    app_apply_scene(argv[1]);
}

static void cmd_shape(int argc, char **argv) {
    if (argc < 2) {
        LOGI("built-in shapes:");
        for (int i = 0; i < shape_count(); i++) LOGD("  %-10s %s", shape_info(i)->name, shape_info(i)->description);
        return;
    }
    app_load_shape(argv[1]);
}

/* A part for the analysis needs the unit its file is written in, and that is never guessed: the file waits in the
 * PART step until the unit is answered there (or by 'solid import <unit>'). */
static void solid_take_file(const char *path) {
    if (!fem_state()->have_project && !fem_new_project("project", NULL, 0)) return;
    fem_set_pending_import(path);
    const char *base = strrchr(path, '/');
    LOGOK("%s is waiting in the PART step: choose the unit it is written in (mm, cm, inch)", base ? base + 1 : path);
}

static void cmd_load(int argc, char **argv) {
    if (argc < 2) {
        LOGE("usage: load <file.stl>");
        return;
    }
    char path[2048];
    if (argv[1][0] == '~')
        snprintf(path, sizeof path, "%s%s", getenv("HOME") ? getenv("HOME") : "", argv[1] + 1);
    else
        str_copy(path, sizeof path, argv[1]);
    if (app.workspace == WS_SOLID) {
        solid_take_file(path); /* dropping a part while the analysis is open imports it there, not into the tunnel */
        return;
    }
    app_load_stl(path);
}

static void cmd_open(int argc, char **argv) {
    char path[2048];
    if (!platform_open_file_dialog(path, sizeof path, "stl")) {
        LOGI("no file selected");
        return;
    }
    if (app.workspace == WS_SOLID) {
        solid_take_file(path);
        return;
    }
    app_load_stl(path);
}

static void cmd_export(int argc, char **argv) {
    if (!app.has_model) {
        LOGE("no model loaded");
        return;
    }
    const char *path = argc > 1 ? argv[1] : "model.stl";
    char err[256];
    if (mesh_save_stl(path, &app.mesh, err, sizeof err))
        LOGOK("saved %u triangles to %s", app.mesh.tri_count, path);
    else
        LOGE("export failed: %s", err);
}

static void cmd_unload(int argc, char **argv) { app_unload_model(); }

static void set_angle(int argc, char **argv, double *target, const char *what) {
    if (argc < 2) {
        LOGI("%s = %.2f\xC2\xB0", what, *target);
        return;
    }
    double v;
    if (!parse_number(argv[1], &v)) return;
    *target = CLAMP(v, -180.0, 180.0);
    app_push_params();
    LOGI("%s = %.2f\xC2\xB0 (re-voxelising)", what, *target);
}
static void cmd_aoa(int argc, char **argv) { set_angle(argc, argv, &app.params.aoa, "angle of attack"); }
static void cmd_yaw(int argc, char **argv) { set_angle(argc, argv, &app.params.yaw, "yaw"); }
static void cmd_roll(int argc, char **argv) { set_angle(argc, argv, &app.params.roll, "roll"); }

static void cmd_fit(int argc, char **argv) {
    if (argc < 2) {
        LOGI("fit = %.2f of the cross-section, %.2f of the length", app.params.fit, app.params.fit_length);
        return;
    }
    double v, w;
    if (!parse_number(argv[1], &v)) return;
    app.params.fit = CLAMP(v, 0.02, 1.0);
    if (argc > 2 && parse_number(argv[2], &w)) app.params.fit_length = CLAMP(w, 0.02, 0.95);
    app_push_params();
    LOGI("model fit: %.2f cross-section, %.2f length", app.params.fit, app.params.fit_length);
}

static void cmd_pos(int argc, char **argv) {
    if (argc < 4) {
        LOGI("model position = (%.3f, %.3f, %.3f) of the tunnel", app.params.pos[0], app.params.pos[1], app.params.pos[2]);
        return;
    }
    for (int a = 0; a < 3; a++) {
        double v;
        if (!parse_number(argv[1 + a], &v)) return;
        app.params.pos[a] = CLAMP(v, 0.0, 1.0);
    }
    app_push_params();
}

static void cmd_shell(int argc, char **argv) {
    if (argc < 2) {
        LOGI("voxel shell radius = %.2f cells", app.params.shell);
        return;
    }
    double v;
    if (!parse_number(argv[1], &v)) return;
    app.params.shell = CLAMP(v, 0.0, 3.0);
    app_push_params();
}

static void cmd_rotate(int argc, char **argv) {
    if (argc < 3 || !app.has_model) {
        LOGE("usage: rotate <x|y|z> <degrees>   (permanently rotates the model mesh)");
        return;
    }
    double deg;
    if (!parse_number(argv[2], &deg)) return;
    vec3 axis = str_ieq(argv[1], "x") ? v3(1, 0, 0) : str_ieq(argv[1], "y") ? v3(0, 1, 0) : v3(0, 0, 1);
    mesh_transform(&app.mesh, m4_rotate(axis, DEG2RAD(deg)));
    app_mesh_changed();
    LOGI("model rotated %.1f\xC2\xB0 about %s", deg, argv[1]);
}

/* quarter turns that put the longest side along the flow (X) and the thinnest side vertical (Y), fewest turns first */
static mat4 auto_orientation(const Mesh *m) {
    vec3 e = v3_sub(m->bmax, m->bmin);
    float tol = 1e-4f * MAXI(e.x, MAXI(e.y, e.z));
    mat4 best = m4_identity();
    int best_cost = 1 << 20;
    for (int rx = 0; rx < 4; rx++)
        for (int ry = 0; ry < 4; ry++)
            for (int rz = 0; rz < 4; rz++) {
                mat4 R = m4_mul(m4_rotate_z((float)(M_PI * 0.5 * rz)),
                                m4_mul(m4_rotate_y((float)(M_PI * 0.5 * ry)), m4_rotate_x((float)(M_PI * 0.5 * rx))));
                vec3 d = m4_mul_dir(R, e);
                float ax = fabsf(d.x), ay = fabsf(d.y), az = fabsf(d.z);
                if (ax + tol < MAXI(ay, az) || ay > MINI(ax, az) + tol) continue;
                int cost = (rx == 3 ? 1 : rx) + (ry == 3 ? 1 : ry) + (rz == 3 ? 1 : rz);
                if (cost < best_cost) best_cost = cost, best = R;
            }
    return best;
}

static void cmd_boundary(int argc, char **argv) {
    SimParams *p = &app.params;
    if (argc > 1) {
        if (str_starts_with_i(argv[1], "curv") || str_ieq(argv[1], "bouzidi") || parse_onoff(argv[1]) == 1) {
            p->curved_walls = true;
        } else if (str_starts_with_i(argv[1], "stair") || str_starts_with_i(argv[1], "half") || parse_onoff(argv[1]) == 0) {
            p->curved_walls = false;
        } else {
            LOGE("usage: boundary curved|staircase");
            return;
        }
        app_push_params();
    }
    if (p->curved_walls)
        LOGI("model walls: curved - interpolated bounce-back at the exact mesh surface (Bouzidi, Firdaouss & Lallemand 2001)");
    else
        LOGI("model walls: staircase - halfway bounce-back on the voxel faces");
}

static void cmd_orient(int argc, char **argv) {
    if (argc < 2) {
        LOGI("orient zup    - model was authored Z-up (most CAD / STL files)");
        LOGI("orient flip   - nose was pointing downstream (+X): turn it around");
        LOGI("orient auto   - longest side along the flow, thinnest side vertical");
        LOGI("orient reset  - back to the file as loaded, with pitch, yaw and roll at 0");
        LOGI("also: rotate x|y|z <deg>, aoa/yaw/roll <deg>, pos x y z, Option-drag the model, MODEL tab in the panel");
        return;
    }
    if (!app.has_model) {
        LOGE("no model loaded");
        return;
    }
    if (str_ieq(argv[1], "zup")) {
        mesh_transform(&app.mesh, m4_rotate_x(DEG2RAD(-90.0f)));
    } else if (str_ieq(argv[1], "flip")) {
        mesh_transform(&app.mesh, m4_rotate_y(DEG2RAD(180.0f)));
    } else if (str_ieq(argv[1], "auto")) {
        mesh_transform(&app.mesh, auto_orientation(&app.mesh));
    } else if (str_ieq(argv[1], "reset")) {
        app_restore_mesh();
        app.params.aoa = app.params.yaw = app.params.roll = 0;
        app_push_params();
        LOGI("model back to its orientation as loaded");
        return;
    } else {
        LOGE("unknown orientation '%s' (zup, flip, auto, reset)", argv[1]);
        return;
    }
    app_mesh_changed();
    vec3 e = v3_sub(app.mesh.bmax, app.mesh.bmin);
    LOGI("model extent now %.3g along the flow, %.3g tall, %.3g wide", e.x, e.y, e.z);
}

static void cmd_playback(int argc, char **argv) {
    double f = app.playback > 0 ? app.playback : 1.0;
    if (argc > 1) {
        if (str_ieq(argv[1], "max") || str_ieq(argv[1], "full")) {
            f = 1.0;
        } else if (str_ieq(argv[1], "slower")) {
            f *= 0.5;
        } else if (str_ieq(argv[1], "faster")) {
            f *= 2.0;
        } else {
            double v;
            if (!parse_number(argv[1], &v)) return;
            f = (strchr(argv[1], '%') || v > 1.0) ? v / 100.0 : v;
        }
        f = CLAMP(f, 0.01, 1.0);
        if (f > 0.97) f = 1.0;
        app.playback = f;
        sim_set_playback(app.sim, f);
    }
    refresh_status();
    double full = app.status.cells > 0 ? app.status.mlups * 1e6 / (double)app.status.cells : 0;
    if (f >= 1.0)
        LOGI("playback: full solver speed%s", app.status.running ? "" : " (paused)");
    else if (full > 0)
        LOGI("playback %.0f%% of full speed: ~%.2g of %.0f steps/s ('playback max' to undo)", f * 100, f * full, full);
    else
        LOGI("playback %.0f%% of full speed ('playback max' to undo)", f * 100);
}

static void cmd_fps(int argc, char **argv) {
    if (argc > 1) {
        double v = 0;
        if (!str_ieq(argv[1], "off") && !parse_number(argv[1], &v)) return;
        app.max_fps = v <= 0 ? 0 : CLAMP(v, 10.0, 240.0);
    }
    if (app.max_fps > 0)
        LOGI("frame rate capped at %.0f fps so the solver gets the spare CPU ('fps off' to uncap)", app.max_fps);
    else
        LOGI("frame rate uncapped");
}

static void cmd_model(int argc, char **argv) {
    refresh_status();
    if (!app.has_model) {
        LOGI("no model - empty tunnel ('scene', 'shape' or 'load')");
        return;
    }
    vec3 e = v3_sub(app.model_hi, app.model_lo);
    LOGD("model '%s'  %u triangles  %s", app.model_name, app.mesh.tri_count, app.model_path);
    LOGD("placed extent %.1f x %.1f x %.1f cells, AoA %.1f\xC2\xB0 yaw %.1f\xC2\xB0 roll %.1f\xC2\xB0", e.x, e.y, e.z,
         app.params.aoa, app.params.yaw, app.params.roll);
    LOGD("reference length %.1f cells (%s axis), frontal area %.0f cells\xC2\xB2, blockage %.1f%%", app.status.units.ref_cells,
         app.params.ref_axis == 0 ? "x" : app.params.ref_axis == 1 ? "y" : app.params.ref_axis == 2 ? "z" : "max",
         app.status.units.frontal_cells, 100 * app.status.units.blockage);
}

/* ---- fluid and flow ------------------------------------------------------------------------------ */

static void print_flow(void) {
    double rho, nu;
    if (app.params.fluid == FLUID_CUSTOM)
        nu = app.params.nu;
    else
        fluid_properties(app.params.fluid, app.params.temperature, &rho, &nu);
    char a[64], b[64];
    format_si(a, sizeof a, app.params.speed, "m/s");
    format_si(b, sizeof b, nu, "m\xC2\xB2/s");
    LOGD("U = %s  L = %.4g m  nu = %s  ->  Re = %.4g", a, app.params.ref_length, b,
         app.params.speed * app.params.ref_length / MAXI(nu, 1e-12));
}

static void cmd_fluid(int argc, char **argv) {
    if (argc < 2) {
        for (int i = 0; i < FLUID_CUSTOM; i++) {
            double rho, nu;
            fluid_properties(i, app.params.temperature, &rho, &nu);
            char b[64];
            format_si(b, sizeof b, nu, "m\xC2\xB2/s");
            LOGD("  %-9s rho %7.1f kg/m\xC2\xB3  nu %s%s", fluid_name(i), rho, b, i == app.params.fluid ? "   <" : "");
        }
        return;
    }
    int f = fluid_find(argv[1]);
    if (f < 0) {
        LOGE("unknown fluid '%s'", argv[1]);
        return;
    }
    app.params.fluid = f;
    app_push_params();
    LOGOK("fluid: %s", fluid_label(f));
    print_flow();
}

static void cmd_temp(int argc, char **argv) {
    if (argc < 2) {
        LOGI("temperature = %.1f \xC2\xB0""C", app.params.temperature);
        return;
    }
    double v;
    if (!parse_number(argv[1], &v)) return;
    app.params.temperature = CLAMP(v, -50.0, 400.0);
    app_push_params();
    print_flow();
}

static void cmd_speed(int argc, char **argv) {
    if (argc < 2) {
        print_flow();
        return;
    }
    double v;
    if (!parse_quantity(argc, argv, 1, SPEED_UNITS, &v)) return;
    if (v <= 0) {
        LOGE("speed must be positive");
        return;
    }
    app.params.speed = v;
    app_push_params();
    print_flow();
}

static void cmd_re(int argc, char **argv) {
    if (argc < 2) {
        print_flow();
        return;
    }
    double re;
    if (!parse_number(argv[1], &re) || re <= 0) return;
    double rho, nu;
    if (app.params.fluid == FLUID_CUSTOM)
        nu = app.params.nu;
    else
        fluid_properties(app.params.fluid, app.params.temperature, &rho, &nu);
    app.params.speed = re * nu / MAXI(app.params.ref_length, 1e-9);
    app_push_params();
    print_flow();
}

static void cmd_length(int argc, char **argv) {
    if (argc < 2) {
        print_flow();
        return;
    }
    double v;
    if (!parse_quantity(argc, argv, 1, LENGTH_UNITS, &v) || v <= 0) return;
    app.params.ref_length = v;
    app_push_params();
    print_flow();
}

static void cmd_refaxis(int argc, char **argv) {
    static const char *names[] = {"x", "y", "z", "max", NULL};
    if (argc < 2) {
        LOGI("reference axis = %s", names[app.params.ref_axis]);
        return;
    }
    for (int i = 0; names[i]; i++)
        if (str_ieq(argv[1], names[i])) {
            app.params.ref_axis = i;
            app_push_params();
            return;
        }
    LOGE("expected x, y, z or max");
}

static void cmd_viscosity(int argc, char **argv) {
    double v;
    if (argc < 2 || !parse_number(argv[1], &v) || v <= 0) {
        LOGE("usage: viscosity <kinematic m\xC2\xB2/s>  e.g. viscosity 1e-6");
        return;
    }
    double rho, nu;
    fluid_properties(app.params.fluid, app.params.temperature, &rho, &nu);
    if (app.params.fluid != FLUID_CUSTOM) app.params.rho = rho;
    app.params.fluid = FLUID_CUSTOM;
    app.params.nu = v;
    app_push_params();
    print_flow();
}

static void cmd_density(int argc, char **argv) {
    double v;
    if (argc < 2 || !parse_number(argv[1], &v) || v <= 0) {
        LOGE("usage: density <kg/m\xC2\xB3>");
        return;
    }
    double rho, nu;
    fluid_properties(app.params.fluid, app.params.temperature, &rho, &nu);
    if (app.params.fluid != FLUID_CUSTOM) app.params.nu = nu;
    app.params.fluid = FLUID_CUSTOM;
    app.params.rho = v;
    app_push_params();
}

static void cmd_roughness(int argc, char **argv) {
    if (argc < 2) {
        char a[64];
        format_si(a, sizeof a, app.params.roughness, "m");
        LOGI("surface roughness ks = %s", a);
        LOGI("examples: glass 0.001mm, paint 0.02mm, steel 0.05mm, concrete 1mm, barnacles 5mm");
        return;
    }
    double v;
    if (str_ieq(argv[1], "smooth"))
        v = 0;
    else if (!parse_quantity(argc, argv, 1, LENGTH_UNITS, &v))
        return;
    app.params.roughness = MAXI(v, 0.0);
    app_push_params();
    refresh_status();
    char a[64];
    format_si(a, sizeof a, app.params.roughness, "m");
    LOGOK("roughness ks = %s (%.3f lattice cells)", a, app.params.roughness / MAXI(app.status.units.dx, 1e-12));
}

static void cmd_turbulence(int argc, char **argv) {
    if (argc < 2) {
        LOGI("inlet turbulence intensity = %.2f%%", 100 * app.params.turbulence);
        return;
    }
    double v;
    if (!parse_number(argv[1], &v)) return;
    app.params.turbulence = CLAMP(v / 100.0, 0.0, 0.5);
    app_push_params();
    LOGI("inlet turbulence intensity = %.2f%%", 100 * app.params.turbulence);
}

/* ---- numerics ------------------------------------------------------------------------------------ */

static uint64_t physical_memory(void) {
    uint64_t mem = 0;
    size_t len = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    return mem;
}

static bool set_grid(int nx, int ny, int nz, bool force) {
    nx = CLAMP(nx, 16, 2048), ny = CLAMP(ny, 16, 2048), nz = CLAMP(nz, 16, 2048);
    double cells = (double)nx * ny * nz;
    double bytes = (double)lbm_bytes_estimate(nx, ny, nz) + cells * 17.0 * 3;
    double mem = (double)physical_memory();
    if (bytes > mem * 0.7 && !force) {
        LOGE("%dx%dx%d needs ~%.0f MB, more than 70%% of RAM (%.0f MB). Add 'force' to try anyway.", nx, ny, nz,
             bytes / 1048576, mem / 1048576);
        return false;
    }
    if (bytes > mem * 0.4) LOGW("large lattice: ~%.0f MB of %.0f MB RAM", bytes / 1048576, mem / 1048576);
    app.params.nx = nx, app.params.ny = ny, app.params.nz = nz;
    app_push_params();
    render_reset_particles(app.renderer);
    refresh_status();
    double sps = app.status.mlups > 1 ? app.status.mlups * 1e6 / cells : 130e6 / cells;
    LOGOK("lattice %dx%dx%d = %.2fM cells (~%.0f MB) - expect ~%.0f steps/s", nx, ny, nz, cells / 1e6, bytes / 1048576, sps);
    return true;
}

static void cmd_grid(int argc, char **argv) {
    if (argc < 4) {
        LOGI("lattice %d x %d x %d  (usage: grid <nx> <ny> <nz> [force])", app.params.nx, app.params.ny, app.params.nz);
        return;
    }
    set_grid(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), argc > 4 && str_ieq(argv[4], "force"));
}

static void cmd_quality(int argc, char **argv) {
    static const char *names[] = {"draft", "normal", "high", "ultra", NULL};
    static const double cells[] = {0.9e6, 2.8e6, 5.5e6, 9.0e6};
    int q = -1;
    for (int i = 0; argc > 1 && names[i]; i++)
        if (str_ieq(argv[1], names[i])) q = i;
    if (q < 0) {
        LOGI("usage: quality <draft|normal|high|ultra>  (~0.9M / 2.8M / 5.5M / 9M cells, same tunnel proportions)");
        return;
    }
    double cur = (double)app.params.nx * app.params.ny * app.params.nz;
    double k = cbrt(cells[q] / cur);
    int nx = (int)lround(app.params.nx * k / 4.0) * 4, ny = (int)lround(app.params.ny * k / 4.0) * 4;
    int nz = (int)lround(app.params.nz * k / 4.0) * 4;
    set_grid(nx, ny, nz, argc > 2 && str_ieq(argv[2], "force"));
}

static void cmd_ulb(int argc, char **argv) {
    if (argc < 2) {
        LOGI("lattice inlet velocity = %.4f (higher = faster simulated time, lower = more accurate)", app.params.u_lattice);
        return;
    }
    double v;
    if (!parse_number(argv[1], &v)) return;
    app.params.u_lattice = CLAMP(v, 0.005, 0.2);
    app_push_params();
}

static void print_collision(void) {
    static const char *names[] = {"bgk (single relaxation time)", "reg (second-order regularized)",
                                  "rr (regularized + third-order recursive terms)"};
    const SimParams *p = &app.params;
    char bulk[32] = "same as shear", hyb[32] = "off";
    if (p->tau_bulk > 0.5) snprintf(bulk, sizeof bulk, "tau %.2f", p->tau_bulk);
    if (p->hrr_sigma > 0 && p->hrr_sigma < 0.9999) snprintf(hyb, sizeof hyb, "sigma %.3f", p->hrr_sigma);
    LOGI("collision %s, bulk viscosity %s, finite-difference hybrid %s", names[CLAMP(p->collision, 0, 2)], bulk, hyb);
}

static void cmd_collision(int argc, char **argv) {
    SimParams *p = &app.params;
    if (argc < 2) {
        print_collision();
        LOGI("  usage: collision [rr|reg|bgk] [bulk <tau|off>] [hrr <sigma|off>]");
        LOGI("  rr stays stable as tau -> 1/2; bulk damps pressure waves; hrr adds grid-scale damping (costs ~2x)");
        return;
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        double v;
        if (str_ieq(a, "rr") || str_ieq(a, "rr3") || str_starts_with_i(a, "rec")) {
            p->collision = LBM_RECURSIVE;
        } else if (str_starts_with_i(a, "reg")) {
            p->collision = LBM_REGULARIZED;
        } else if (str_ieq(a, "bgk") || str_ieq(a, "srt")) {
            p->collision = LBM_BGK;
        } else if ((str_ieq(a, "bulk") || str_ieq(a, "hrr")) && i + 1 < argc) {
            bool bulk = str_ieq(a, "bulk");
            const char *arg = argv[++i];
            int oo = parse_onoff(arg);
            if (oo >= 0) v = oo ? (bulk ? 1.0 : 0.98) : 0.0;
            else if (!parse_number(arg, &v)) return;
            if (bulk) p->tau_bulk = v <= 0 ? 0.0 : CLAMP(v, 0.5, 2.0);
            else p->hrr_sigma = v <= 0 || v >= 1 ? 0.0 : CLAMP(v, 0.9, 0.9999);
        } else {
            LOGE("usage: collision [rr|reg|bgk] [bulk <tau|off>] [hrr <sigma|off>]");
            return;
        }
    }
    app_push_params();
    print_collision();
}

static void cmd_les(int argc, char **argv) {
    if (argc < 2) {
        LOGI("Smagorinsky Cs = %.3f%s", app.params.smagorinsky, app.params.smagorinsky <= 0 ? " (LES off)" : "");
        return;
    }
    double v = 0;
    if (parse_onoff(argv[1]) == 0) v = 0;
    else if (parse_onoff(argv[1]) == 1) v = 0.14;
    else if (!parse_number(argv[1], &v)) return;
    app.params.smagorinsky = CLAMP(v, 0.0, 0.5);
    app_push_params();
    LOGI("Smagorinsky Cs = %.3f", app.params.smagorinsky);
}

static void set_walls(int argc, char **argv, int first, int last, const char *what) {
    if (argc < 2) {
        LOGI("%s: %s", what, WALLS[app.params.wall[first]]);
        return;
    }
    int w = wall_find(argv[1]);
    if (w < 0) {
        LOGE("unknown wall type '%s' (slip, freestream, noslip, moving, periodic)", argv[1]);
        return;
    }
    for (int i = first; i <= last; i++) app.params.wall[i] = w;
    app_push_params();
    LOGI("%s -> %s", what, WALLS[w]);
}
static void cmd_walls(int argc, char **argv) { set_walls(argc, argv, 0, 3, "all tunnel walls"); }
static void cmd_floor(int argc, char **argv) { set_walls(argc, argv, 0, 0, "floor"); }
static void cmd_ceiling(int argc, char **argv) { set_walls(argc, argv, 1, 1, "ceiling"); }
static void cmd_sides(int argc, char **argv) { set_walls(argc, argv, 2, 3, "side walls"); }

static void cmd_threads(int argc, char **argv) {
    if (argc < 2) {
        LOGI("solver threads = %d (cpu has %d)", app.params.threads, cpu_count());
        return;
    }
    app.params.threads = CLAMP(atoi(argv[1]), 1, 64);
    app_push_params();
}

static void cmd_init(int argc, char **argv) {
    if (argc < 2) {
        LOGI("init = %s", app.params.start_with_flow ? "flow (uniform stream)" : "rest (water starts still, inflow ramps up)");
        return;
    }
    app.params.start_with_flow = str_ieq(argv[1], "flow");
    app_push_params();
}

static void cmd_ramp(int argc, char **argv) {
    if (argc < 2) {
        LOGI("inflow ramp = %d steps", app.params.ramp_steps);
        return;
    }
    app.params.ramp_steps = MAXI(0, atoi(argv[1]));
    app_push_params();
}

/* ---- visualisation ------------------------------------------------------------------------------- */

static void cmd_view(int argc, char **argv) {
    if (argc < 2) {
        LOGI("field = %s (%s)", display_field_name(app.display_field), display_field_label(app.display_field));
        return;
    }
    int f = display_field_find(argv[1]);
    if (f < 0) {
        LOGE("unknown field '%s' (speed ux uy uz pressure cp vorticity q)", argv[1]);
        return;
    }
    app.display_field = f;
    app.rs.field = display_field_vis(f); /* the volume transfer function follows the displayed field */
    app.manual_range = false;
    app_mark_vis_dirty();
}

static void cmd_stats(int argc, char **argv) {
    refresh_status();
    const SimUnits *u = &app.status.units;
    const VisRange *r = &app.stats;
    double s = display_field_scale(app.display_field, u);
    const char *unit = display_field_unit(app.display_field);
    LOGD("%s  [%s]", display_field_label(app.display_field), unit[0] ? unit : "-");
    LOGD("  min %.4g  p1 %.4g  p99 %.4g  max %.4g", r->min * s, r->lo * s, r->hi * s, r->max * s);
    LOGD("  lattice: min %.3e  p1 %.3e  p99 %.3e  max %.3e", r->min, r->lo, r->hi, r->max);
    LOGD("  colour range %.4g .. %.4g %s", app.lat_lo * s, app.lat_hi * s, unit);
    LOGD("  vortex level %.3g x Q-criterion 99th percentile", app.rs.vortex_level);
    LOGD("  worker: field %.1f ms | streamlines %.1f ms, %u lines, %u vertices | vortices %.1f ms, %u triangles",
         app.field_ms, app.stream_ms, app.stream_lines, app.stream_verts, app.iso_ms, app.iso_tris);
    LOGD("  %.1f fps, solver %.1f MLUPS", app.fps, app.status.mlups);
}

static void cmd_cmap(int argc, char **argv) {
    if (argc < 2) {
        char list[512] = {0};
        for (int i = 0; i < CMAP_COUNT; i++) {
            strcat(list, colormap_name(i));
            strcat(list, i == app.rs.cmap ? "* " : "  ");
        }
        LOGI("colour maps: %s", list);
        return;
    }
    int c = colormap_find(argv[1]);
    if (c < 0) {
        LOGE("unknown colour map '%s'", argv[1]);
        return;
    }
    app.rs.cmap = c;
    render_set_colormap(app.renderer, c);
}

static void cmd_gradient(int argc, char **argv) {
    if (argc < 2) {
        LOGI("usage: gradient #03045e #00b4d8 #caf0f8   or   gradient 0:#000 0.6:#0af 1:#fff");
        return;
    }
    char spec[512] = {0};
    for (int i = 1; i < argc; i++) {
        strncat(spec, argv[i], sizeof spec - strlen(spec) - 2);
        strcat(spec, " ");
    }
    char err[128];
    if (!colormap_set_custom(spec, err, sizeof err)) {
        LOGE("gradient: %s", err);
        return;
    }
    app.rs.cmap = CMAP_CUSTOM;
    render_set_colormap(app.renderer, CMAP_CUSTOM);
    if (app.rs.line_cmap == CMAP_CUSTOM) render_set_line_colormap(app.renderer, CMAP_CUSTOM);
    LOGOK("custom gradient applied");
}

static void cmd_linecmap(int argc, char **argv) {
    if (argc < 2) {
        LOGI("streamline & particle colour map: %s  (usage: linecmap <name|same>)",
             app.rs.line_cmap < 0 ? "same as field" : colormap_name(app.rs.line_cmap));
        return;
    }
    int c = str_ieq(argv[1], "same") ? -1 : colormap_find(argv[1]);
    if (c < 0 && !str_ieq(argv[1], "same")) {
        LOGE("unknown colour map '%s'", argv[1]);
        return;
    }
    app.rs.line_cmap = c;
    render_set_line_colormap(app.renderer, c);
}

static void cmd_range(int argc, char **argv) {
    if (argc < 2) {
        LOGI("range %s", app.manual_range ? "manual" : "auto");
        return;
    }
    if (str_ieq(argv[1], "auto")) {
        app.manual_range = false;
        return;
    }
    double lo, hi;
    if (argc < 3 || !parse_number(argv[1], &lo) || !parse_number(argv[2], &hi) || hi <= lo) {
        LOGE("usage: range auto | range <min> <max>  (in %s)", display_field_unit(app.display_field));
        return;
    }
    app.manual_range = true;
    app.range_lo = lo, app.range_hi = hi;
}

static void cmd_surface(int argc, char **argv) {
    if (argc < 2) {
        LOGI("surface: %s, grid %s", app.rs.surface_mode == 0 ? "off" : app.rs.surface_mode == 1 ? "solid" : "field",
             app.rs.surface_grid ? "on" : "off");
        return;
    }
    if (str_ieq(argv[1], "grid")) {
        apply_onoff(&app.rs.surface_grid, argc, argv, 2, "surface grid");
        return;
    }
    if (str_ieq(argv[1], "off")) app.rs.surface_mode = SURFACE_HIDDEN;
    else if (str_ieq(argv[1], "solid") || str_ieq(argv[1], "material")) app.rs.surface_mode = SURFACE_MATERIAL;
    else if (str_ieq(argv[1], "field") || str_ieq(argv[1], "on")) app.rs.surface_mode = SURFACE_FIELD;
    else LOGE("expected off, solid, field or grid");
}

static void cmd_slice(int argc, char **argv) {
    if (argc < 2) {
        app.rs.slice_on = !app.rs.slice_on;
    } else if (str_ieq(argv[1], "off")) {
        app.rs.slice_on = false;
    } else if (str_ieq(argv[1], "on")) {
        app.rs.slice_on = true;
    } else if (str_ieq(argv[1], "x") || str_ieq(argv[1], "y") || str_ieq(argv[1], "z")) {
        app.rs.slice_on = true;
        app.rs.slice_axis = tolower((unsigned char)argv[1][0]) - 'x';
        double v;
        if (argc > 2 && parse_number(argv[2], &v)) app.rs.slice_pos = (float)CLAMP(v, 0.0, 1.0);
        if (argc > 3 && parse_number(argv[3], &v)) app.rs.slice_opacity = (float)CLAMP(v, 0.05, 1.0);
    } else {
        double v;
        if (!parse_number(argv[1], &v)) return;
        app.rs.slice_on = true;
        app.rs.slice_pos = (float)CLAMP(v, 0.0, 1.0);
    }
    LOGI("slice %s: %c = %.2f", app.rs.slice_on ? "on" : "off", 'x' + app.rs.slice_axis, app.rs.slice_pos);
}

static void cmd_streamlines(int argc, char **argv) {
    if (argc < 2) {
        LOGI("streamlines %s: %s rake at (%.2f, %.2f, %.2f) of the tunnel%s, %.0f x %.0f cells, %d lines, width %.1f px",
             app.rs.stream_on ? "on" : "off", seed_mode_name(app.seed_mode), app.rake_pos.x / MAXI(app.nx, 1),
             app.rake_pos.y / MAXI(app.ny, 1), app.rake_pos.z / MAXI(app.nz, 1), app.rake_manual ? "" : " (following the model)",
             app.rake_size[0], app.rake_size[1], app.seed_count, app.rs.stream_width);
        float hx, hy;
        if (app_rake_handle(&hx, &hy)) LOGI("  rake handle at (%.0f, %.0f) in the window - drag it to move the rake", hx, hy);
        return;
    }
    const char *sub = argv[1];
    int m = seed_mode_find(sub);
    double v;
    if (parse_onoff(sub) >= 0) apply_onoff(&app.rs.stream_on, argc, argv, 1, "streamlines");
    else if (m >= 0) {
        app.seed_mode = m, app.rs.stream_on = true;
        if (app.rake_manual && app.rake_size[0] < 1) app.rake_size[0] = app.ny * 0.4f; /* a moved rake keeps its place */
        if (app.rake_manual && app.rake_size[1] < 1) app.rake_size[1] = app.nz * 0.4f;
    }
    else if (str_ieq(sub, "volume")) { /* through the whole body: a grid across its cross-section, drawn as tubes */
        app.seed_mode = SEED_GRID, app.rake_manual = false, app.rs.stream_on = true;
        app.seed_count = MAXI(app.seed_count, 400);
        app.rs.stream_width = MAXI(app.rs.stream_width, 4.5f);
    }
    else if (str_ieq(sub, "mode") && argc > 2 && seed_mode_find(argv[2]) >= 0) app.seed_mode = seed_mode_find(argv[2]);
    else if (str_ieq(sub, "seeds") && argc > 2 && parse_number(argv[2], &v)) app.seed_count = (int)CLAMP(v, 1, 20000);
    else if (str_ieq(sub, "width") && argc > 2 && parse_number(argv[2], &v)) app.rs.stream_width = (float)CLAMP(v, 0.3, 12);
    else if (str_ieq(sub, "steps") && argc > 2 && parse_number(argv[2], &v)) app.stream_steps = (int)CLAMP(v, 16, 20000);
    else if (str_ieq(sub, "animate")) apply_onoff(&app.rs.stream_animate, argc, argv, 2, "streamline animation");
    else if (str_ieq(sub, "auto")) app.rake_manual = false, app.rs.stream_on = true;
    else if (str_ieq(sub, "at") && argc > 4) {
        double fx, fy, fz;
        if (!parse_number(argv[2], &fx) || !parse_number(argv[3], &fy) || !parse_number(argv[4], &fz)) return;
        app.rake_pos = v3((float)(CLAMP(fx, 0.0, 1.0) * app.nx), (float)(CLAMP(fy, 0.0, 1.0) * app.ny),
                          (float)(CLAMP(fz, 0.0, 1.0) * app.nz));
        app.rake_manual = true, app.rs.stream_on = true;
    } else if (str_ieq(sub, "probe")) {
        if (!app.probe_on) {
            LOGE("no probe placed - Option-click a point in the flow first");
            return;
        }
        app.rake_pos = app.probe;
        app.rake_manual = true, app.rs.stream_on = true;
    } else if (str_ieq(sub, "size") && argc > 2 && parse_number(argv[2], &v)) {
        double v2 = v;
        if (argc > 3 && !parse_number(argv[3], &v2)) return;
        app.rake_size[0] = (float)(CLAMP(v, 0.0, 2.0) * app.ny), app.rake_size[1] = (float)(CLAMP(v2, 0.0, 2.0) * app.nz);
        app.rake_manual = true, app.rs.stream_on = true;
    } else if (str_ieq(sub, "normal") && argc > 2 && (argv[2][0] | 32) >= 'x' && (argv[2][0] | 32) <= 'z') {
        app.rake_axis = (argv[2][0] | 32) - 'x';
        app.rake_manual = true, app.rs.stream_on = true;
    } else {
        LOGE("usage: streamlines on|off | plane|grid|line|point|wake|random | at x y z | size h [w] | normal x|y|z | auto | "
             "probe | seeds N | width px | steps N | animate on|off");
        return;
    }
    app_mark_vis_dirty();
}

static void cmd_particles(int argc, char **argv) {
    static const char *emitters[] = {"sheet", "model", "full"};
    if (argc < 2) {
        LOGI("particles %s: %d, size %.1f px, emitter %s", app.rs.particles_on ? "on" : "off", app.rs.particle_count,
             app.rs.particle_size, emitters[CLAMP(app.emitter_mode, 0, 2)]);
        return;
    }
    const char *sub = argv[1];
    double v;
    if (parse_onoff(sub) >= 0) apply_onoff(&app.rs.particles_on, argc, argv, 1, "particles");
    else if (str_ieq(sub, "count") && argc > 2 && parse_number(argv[2], &v)) {
        app.rs.particle_count = (int)CLAMP(v, 0, 4000000);
        LOGI("%d GPU particles", app.rs.particle_count);
    } else if (str_ieq(sub, "size") && argc > 2 && parse_number(argv[2], &v)) app.rs.particle_size = (float)CLAMP(v, 0.2, 12);
    else if (str_ieq(sub, "emitter") && argc > 2) {
        int m = -1;
        for (int i = 0; i < 3; i++)
            if (str_ieq(argv[2], emitters[i])) m = i;
        if (m < 0) {
            LOGE("emitter: sheet (smoke sheet), model (volume around the model) or full (whole cross-section)");
            return;
        }
        app.emitter_mode = m;
        render_reset_particles(app.renderer);
        LOGI("particle emitter: %s", emitters[m]);
    } else if (str_ieq(sub, "reset")) render_reset_particles(app.renderer);
    else LOGE("usage: particles on|off | count N | size px | emitter sheet|model|full | reset");
    app_mark_vis_dirty();
}

static void cmd_vortices(int argc, char **argv) {
    if (argc < 2) {
        LOGI("vortex isosurfaces %s, at Q (L/U)^2 = %.3g - higher shows only the strongest cores",
             app.rs.vortex_on ? "on" : "off", app.rs.vortex_level);
        return;
    }
    double v;
    if (str_ieq(argv[1], "level") && argc > 2 && parse_number(argv[2], &v)) app.rs.vortex_level = (float)MAXI(v, 1e-4), app.rs.vortex_on = true;
    else if (parse_onoff(argv[1]) >= 0) apply_onoff(&app.rs.vortex_on, argc, argv, 1, "vortex isosurfaces (Q-criterion)");
    else if (parse_number(argv[1], &v)) app.rs.vortex_level = (float)MAXI(v, 1e-4), app.rs.vortex_on = true;
    app_mark_vis_dirty();
}

static void cmd_volume(int argc, char **argv) {
    double v;
    if (argc > 2 && str_ieq(argv[1], "density") && parse_number(argv[2], &v)) {
        app.rs.volume_density = (float)CLAMP(v, 0.01, 50);
        app.rs.volume_on = true;
        return;
    }
    apply_onoff(&app.rs.volume_on, argc, argv, 1, "volume rendering");
}

static void cmd_bloom(int argc, char **argv) {
    double v;
    if (argc > 1 && parse_number(argv[1], &v)) app.rs.bloom = (float)CLAMP(v, 0.0, 3.0);
    LOGI("bloom = %.2f", app.rs.bloom);
}

/* 'backdrop neutral' gives the tunnel the dark neutral gradient of the showcase preset, so that a flow picture stands in
 * the same frame as the pictures of the result renderer; 'backdrop default' is the blue vignette. Six numbers set the
 * linear top and bottom colours (before exposure and tone mapping), which is how the preset's values were calibrated. */
static void cmd_backdrop(int argc, char **argv) {
    if (argc > 1 && str_ieq(argv[1], "default")) app.rs.backdrop_neutral = false;
    else if (argc > 1 && str_ieq(argv[1], "neutral")) {
        static const float T[3] = {0.00476f, 0.00591f, 0.00911f}, B[3] = {0.00179f, 0.00215f, 0.00372f}; /* calibrated: sRGB (20, 22, 27) over (13, 14, 18) at exposure 1 */
        app.rs.backdrop_neutral = true;
        for (int k = 0; k < 3; k++) app.rs.backdrop_top[k] = T[k], app.rs.backdrop_bottom[k] = B[k];
    }
    else if (argc == 7) {
        app.rs.backdrop_neutral = true;
        for (int k = 0; k < 3; k++) app.rs.backdrop_top[k] = (float)atof(argv[1 + k]), app.rs.backdrop_bottom[k] = (float)atof(argv[4 + k]);
    }
    else LOGI("usage: backdrop default | neutral | <top r g b> <bottom r g b>");
    LOGI("backdrop %s", app.rs.backdrop_neutral ? "neutral" : "default");
}

static void cmd_exposure(int argc, char **argv) {
    double v;
    if (argc > 1 && parse_number(argv[1], &v)) app.rs.exposure = (float)CLAMP(v, 0.1, 8.0);
    LOGI("exposure = %.2f", app.rs.exposure);
}

static void cmd_msaa(int argc, char **argv) {
    if (argc > 1) app.rs.msaa = CLAMP(atoi(argv[1]), 1, 8);
    LOGI("MSAA x%d", app.rs.msaa);
}

static void cmd_floorgrid(int argc, char **argv) { apply_onoff(&app.rs.floor_on, argc, argv, 1, "floor grid"); }
static void cmd_box(int argc, char **argv) { apply_onoff(&app.rs.box_on, argc, argv, 1, "tunnel box"); }
static void cmd_hud(int argc, char **argv) { apply_onoff(&app.hud_on, argc, argv, 1, "HUD"); }

/* ---- camera and output -------------------------------------------------------------------------- */

static void cmd_camera(int argc, char **argv) {
    if (argc < 2) {
        LOGI("camera: yaw %.1f\xC2\xB0 pitch %.1f\xC2\xB0 distance %.0f%s", RAD2DEG(app.cam.yaw_goal), RAD2DEG(app.cam.pitch_goal),
             app.cam.dist_goal, app.cam.locked ? " (locked)" : "");
        return;
    }
    double a, b;
    if (str_ieq(argv[1], "lock")) {
        app.cam.locked = true;
        LOGI("camera pivot locked (orbit + zoom only)");
    } else if (str_ieq(argv[1], "unlock")) {
        app.cam.locked = false;
    } else if (str_ieq(argv[1], "orbit") && argc > 3 && parse_number(argv[2], &a) && parse_number(argv[3], &b)) {
        camera_set_angles(&app.cam, (float)a, (float)b);
    } else if (str_ieq(argv[1], "zoom") && argc > 2 && parse_number(argv[2], &a) && a > 0) {
        camera_zoom(&app.cam, (float)(1.0 / a));
    } else if (str_ieq(argv[1], "fov") && argc > 2 && parse_number(argv[2], &a)) {
        app.cam.fov_deg = (float)CLAMP(a, 10.0, 110.0);
    } else {
        app_camera_preset(argv[1]);
    }
}

static void cmd_probe(int argc, char **argv) {
    if (argc > 1 && str_ieq(argv[1], "off")) {
        app.probe_on = false;
        return;
    }
    if (argc >= 4) {
        double x, y, z;
        if (!parse_number(argv[1], &x) || !parse_number(argv[2], &y) || !parse_number(argv[3], &z)) return;
        app.probe = v3((float)(CLAMP(x, 0, 1) * app.nx), (float)(CLAMP(y, 0, 1) * app.ny), (float)(CLAMP(z, 0, 1) * app.nz));
        app.probe_on = true;
    }
    if (!app.probe_on) {
        LOGI("usage: probe <x> <y> <z> (fractions of the tunnel) | probe off   - or Alt-click in the view");
        return;
    }
    float rho, u[3];
    refresh_status();
    if (!app_probe_sample(app.probe, &rho, u)) {
        LOGW("probe is inside the model or outside the tunnel");
        return;
    }
    const SimUnits *un = &app.status.units;
    double vs = units_speed(un);
    LOGD("probe (%.3f, %.3f, %.3f) m: u = (%.4g, %.4g, %.4g) m/s |u| = %.4g m/s, p = %.4g Pa, Cp = %.3f",
         app.probe.x * un->dx, app.probe.y * un->dx, app.probe.z * un->dx, u[0] * vs, u[1] * vs, u[2] * vs,
         sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) * vs, (rho - 1) / 3.0 * units_pressure(un),
         (rho - 1) / 3.0 / (0.5 * un->u_lb * un->u_lb));
}

static void cmd_screenshot(int argc, char **argv) {
    char path[1024];
    if (argc > 1) {
        str_copy(path, sizeof path, argv[1]);
    } else {
        mkdir("screenshots", 0755);
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(path, sizeof path, "screenshots/navier-%Y%m%d-%H%M%S.png", &tmv);
    }
    app_request_screenshot(path);
}

static void cmd_forces(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "forces.csv";
    ForceSample *hist = malloc(sizeof(ForceSample) * SIM_FORCE_HISTORY);
    int n = sim_force_history(app.sim, hist, SIM_FORCE_HISTORY);
    FILE *f = fopen(path, "w");
    if (!f) {
        LOGE("cannot write %s", path);
        free(hist);
        return;
    }
    refresh_status();
    fprintf(f, "# NAVIER force history: model=%s Re=%.6g U=%.6g m/s L=%.6g m frontal_area=%.6g m2\n",
            app.has_model ? app.model_name : "none", app.status.units.re, app.status.units.U, app.params.ref_length,
            app.status.units.frontal_area);
    fprintf(f, "step,time_s,cd,cl,cs\n");
    for (int i = 0; i < n; i++)
        fprintf(f, "%llu,%.8g,%.6f,%.6f,%.6f\n", (unsigned long long)hist[i].step, hist[i].time, hist[i].cd, hist[i].cl, hist[i].cs);
    fclose(f);
    free(hist);
    LOGOK("wrote %d samples to %s", n, path);
}

/* ---- scripting: command queue, waits, logging and parameter sweeps ------------------------------ */

#define QUEUE_MAX 4096
static char *g_queue[QUEUE_MAX];
static int g_qcount;
static bool g_waiting;
static long g_wait_target, g_wait_progress, g_last_wait;
static uint64_t g_wait_prev_step;
static int g_wait_frames;
static char g_wait_job[64];   /* scripts: hold the queue until this analysis job finishes */
static double g_wait_job_until;
static double g_wait_agent_until; /* scripts: hold the queue until the agent's command ends (0: not waiting) */
static double g_wait_topo_until;  /* scripts: hold the queue until the topology optimisation ends */

static void queue_insert_front(char **lines, int n) {
    if (g_qcount + n > QUEUE_MAX) {
        LOGE("command queue full");
        for (int i = 0; i < n; i++) free(lines[i]);
        return;
    }
    memmove(g_queue + n, g_queue, (size_t)g_qcount * sizeof(char *));
    memcpy(g_queue, lines, (size_t)n * sizeof(char *));
    g_qcount += n;
}

static void queue_clear(void) {
    for (int i = 0; i < g_qcount; i++) free(g_queue[i]);
    g_qcount = 0;
    g_waiting = false;
    g_wait_frames = 0;
    g_wait_job[0] = 0;
}

bool commands_pending(void) {
    return g_qcount > 0 || g_waiting || g_wait_frames > 0 || g_wait_job[0] != 0 || g_wait_agent_until > 0 || g_wait_topo_until > 0;
}

void commands_update(void) {
    if (g_qcount == 0 && !g_waiting && g_wait_frames == 0 && !g_wait_job[0] && g_wait_agent_until <= 0 && g_wait_topo_until <= 0) return;
    if (g_wait_frames > 0) {
        g_wait_frames--;
        return;
    }
    if (g_wait_agent_until > 0) {
        /* the agent works in its own process: the script waits, the window keeps drawing what it does */
        if (agent_running() && app.time < g_wait_agent_until) return;
        if (agent_running()) LOGW("the agent is still working after the wait");
        else LOGOK("the agent's command has ended");
        g_wait_agent_until = 0;
    }
    if (g_wait_topo_until > 0) {
        /* the optimisation runs on the job worker: one solve per iteration, and the window keeps drawing */
        if (fem_optimize_running() && app.time < g_wait_topo_until) return;
        if (fem_optimize_running()) LOGW("the optimisation is still running after the wait");
        else LOGOK("optimisation: %s", fem_optimize_message());
        g_wait_topo_until = 0;
    }
    if (g_wait_job[0]) {
        /* the solve runs on the job worker: the script waits, the interface keeps drawing */
        const FemState *fs = fem_state();
        bool same = !strcmp(fs->job_id, g_wait_job);
        if (same && fs->job_active && app.time < g_wait_job_until) return;
        if (same && fs->job_active) LOGW("job %s is still running after the wait", g_wait_job);
        else if (same) LOGOK("job %s: %s", g_wait_job, fs->job_state[0] ? fs->job_state : "finished");
        g_wait_job[0] = 0;
    }
    if (g_waiting) {
        uint64_t step = app.status.step;
        g_wait_progress += step >= g_wait_prev_step ? (long)(step - g_wait_prev_step) : (long)step;
        g_wait_prev_step = step;
        if (app.status.diverged) {
            LOGW("run diverged - cancelling the remaining %d queued commands", g_qcount);
            queue_clear();
            return;
        }
        if (g_wait_progress < g_wait_target) return;
        g_waiting = false;
    }
    /* one change at a time: let the solver thread apply each request before the next command runs */
    /* a queued screenshot is taken before the script moves on, so it shows the state the script set up */
    while (g_qcount > 0 && !g_waiting && g_wait_frames == 0 && !g_wait_job[0] && !app.shot_path[0] && !sim_requests_pending(app.sim)) {
        char *line = g_queue[0];
        memmove(g_queue, g_queue + 1, (size_t)(g_qcount - 1) * sizeof(char *));
        g_qcount--;
        console_exec(line, true);
        free(line);
    }
}

static void cmd_exec(int argc, char **argv) {
    if (argc < 2) {
        LOGE("usage: exec <script.nav>");
        return;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        LOGE("cannot open %s", argv[1]);
        return;
    }
    char *lines[QUEUE_MAX];
    int n = 0;
    char line[1024];
    while (fgets(line, sizeof line, f) && n < QUEUE_MAX) {
        line[strcspn(line, "\r\n")] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        lines[n++] = strdup(p);
    }
    fclose(f);
    queue_insert_front(lines, n);
    LOGI("script %s: %d commands queued", argv[1], n);
}

static void cmd_wait(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000;
    if (n <= 0) return;
    g_waiting = true;
    g_wait_target = n;
    g_wait_progress = 0;
    g_last_wait = n;
    g_wait_prev_step = app.status.step;
    sim_run(app.sim, true);
    LOGI("running %ld steps before the next command", n);
}

static void cmd_cancel(int argc, char **argv) {
    int n = g_qcount;
    queue_clear();
    LOGI("cancelled %d queued commands", n);
}

/* 'stop': what a person types when something is running and they want it to end: every queued and running
 * simulation of the engine, whoever started it. ('cancel' is the one for a script or a sweep.) */
static void cmd_stop(int argc, char **argv) {
    (void)argc, (void)argv;
    int n = fem_stop_all_jobs();
    if (n == 0) LOGI("nothing is running");
    else LOGOK("asked %d simulation%s to stop; a running build ends with the layer it is on", n, n == 1 ? "" : "s");
}

static void cmd_log(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "experiments.csv";
    refresh_status();
    const SimStatus *st = &app.status;
    double cd = st->cd, cl = st->cl, cs = st->cs;
    int used = 0;
    if (g_last_wait > 0) {
        /* average over the second half of the last 'wait' window (after the start-up transient) */
        ForceSample *h = malloc(sizeof(ForceSample) * SIM_FORCE_HISTORY);
        int n = sim_force_history(app.sim, h, SIM_FORCE_HISTORY);
        uint64_t from = st->step > (uint64_t)(g_last_wait / 2) ? st->step - (uint64_t)(g_last_wait / 2) : 0;
        double a = 0, b = 0, c = 0;
        for (int i = 0; i < n; i++)
            if (h[i].step >= from) a += h[i].cd, b += h[i].cl, c += h[i].cs, used++;
        if (used) cd = a / used, cl = b / used, cs = c / used;
        free(h);
    }
    bool exists = access(path, F_OK) == 0;
    FILE *f = fopen(path, "a");
    if (!f) {
        LOGE("cannot write %s", path);
        return;
    }
    if (!exists)
        fprintf(f, "model,fluid,temp_C,speed_mps,Re,aoa_deg,yaw_deg,roughness_m,turbulence,grid,step,time_s,Cd,Cl,Cs,"
                   "Fx_N,Fy_N,Fz_N,Strouhal\n");
    const SimUnits *u = &st->units;
    double qA = 0.5 * u->rho * u->U * u->U * u->frontal_area;
    fprintf(f, "%s,%s,%.2f,%.6g,%.6g,%.3f,%.3f,%.6g,%.4f,%dx%dx%d,%llu,%.6g,%.6f,%.6f,%.6f,%.6g,%.6g,%.6g,%.4f\n",
            app.has_model ? app.model_name : "none", fluid_name(app.params.fluid), app.params.temperature, app.params.speed,
            u->re, app.params.aoa, app.params.yaw, app.params.roughness, app.params.turbulence, st->nx, st->ny, st->nz,
            (unsigned long long)st->step, st->sim_time, cd, cl, cs, cd * qA, cl * qA, cs * qA, st->strouhal);
    fclose(f);
    LOGOK("logged Cd = %.4f  Cl = %.4f  Cs = %.4f  (%d samples) -> %s", cd, cl, cs, used, path);
}

static void cmd_sweep(int argc, char **argv) {
    static const char *params[] = {"aoa", "yaw", "roll", "speed", "re", "roughness", "temp", "turbulence", NULL};
    if (argc < 5) {
        LOGI("usage: sweep <aoa|yaw|roll|speed|re|roughness|temp|turbulence> <from> <to> <step> [steps N] [file.csv]");
        LOGI("e.g.   sweep aoa -4 12 2 steps 2500 polar.csv     (roughness values in metres)");
        return;
    }
    bool known = false;
    for (int i = 0; params[i]; i++)
        if (str_ieq(argv[1], params[i])) known = true;
    double a, b, s;
    if (!known) {
        LOGE("cannot sweep '%s'", argv[1]);
        return;
    }
    if (!parse_number(argv[2], &a) || !parse_number(argv[3], &b) || !parse_number(argv[4], &s) || s == 0 ||
        (b - a) / s < 0) {
        LOGE("sweep: need from, to and a step pointing from 'from' to 'to'");
        return;
    }
    int steps = 3000;
    const char *file = "sweep.csv";
    for (int i = 5; i < argc; i++) {
        if (str_ieq(argv[i], "steps") && i + 1 < argc) {
            int v = atoi(argv[++i]);
            steps = MAXI(100, v);
        } else {
            file = argv[i];
        }
    }
    int n = (int)floor((b - a) / s + 1e-9) + 1;
    if (n < 1 || n > 500) {
        LOGE("sweep would need %d runs (max 500)", n);
        return;
    }
    char **lines = malloc(sizeof(char *) * (size_t)(4 * n + 1));
    int k = 0;
    char buf[1200];
    for (int j = 0; j < n; j++) {
        double v = a + j * s;
        snprintf(buf, sizeof buf, "%s %.6g", argv[1], v);
        lines[k++] = strdup(buf);
        lines[k++] = strdup("reset");
        snprintf(buf, sizeof buf, "wait %d", steps);
        lines[k++] = strdup(buf);
        snprintf(buf, sizeof buf, "log \"%s\"", file);
        lines[k++] = strdup(buf);
    }
    lines[k++] = strdup("pause");
    queue_insert_front(lines, k);
    free(lines);
    refresh_status();
    double cells = (double)app.params.nx * app.params.ny * app.params.nz;
    double sps = (app.status.mlups > 1 ? app.status.mlups : 120.0) * 1e6 / MAXI(cells, 1.0);
    LOGOK("sweep %s: %d runs x %d steps -> %s  (~%.1f min, 'cancel' to stop)", argv[1], n, steps, file,
          n * steps / sps / 60.0);
}

static void cmd_frames(int argc, char **argv) { g_wait_frames = argc > 1 ? CLAMP(atoi(argv[1]), 0, 100000) : 1; }

/* UI testing: type into the panel field that has the keyboard, through the same entry point as a real key */
/* UI testing: click a point of the 3D view (picking a face), not a named control */
static void cmd_uiclickat(int argc, char **argv) {
    if (argc < 3) {
        LOGE("usage: uiclickat <x> <y> [frames]   (window points, the 3D view is left of the panel)");
        return;
    }
    float px = (float)atof(argv[1]), py = (float)atof(argv[2]);
    int hold = argc > 3 ? CLAMP(atoi(argv[3]), 1, 60) : 4;
    int wait = app_inject_click(px, py, hold);
    if (!wait) {
        LOGE("the input queue is full");
        return;
    }
    LOGI("click at (%.0f, %.0f), held %d frames", px, py, hold);
    g_wait_frames = MAXI(g_wait_frames, wait + 2);
}

static void cmd_uikeys(int argc, char **argv) {
    if (argc < 2) {
        LOGE("usage: uikeys <text>   (use \\n for Return)");
        return;
    }
    if (!hud_text_focused()) {
        LOGE("no panel field has the keyboard - click one first");
        return;
    }
    int typed = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) hud_text_key(' '), typed++; /* the words were separate arguments: put the spaces back */
        for (const char *c = argv[i]; *c; c++) {
            if (c[0] == '\\' && c[1] == 'n') {
                hud_text_key(KEY_ENTER);
                c++;
            } else {
                hud_text_key((unsigned char)*c);
            }
            typed++;
        }
    }
    LOGI("typed %d characters into the panel field: '%s'", typed, hud_text_value());
}

static void cmd_uikey(int argc, char **argv) {
    int key = 0;
    if (argc > 1) {
        if (strlen(argv[1]) == 1) key = (unsigned char)tolower((unsigned char)argv[1][0]);
        else if (str_ieq(argv[1], "space")) key = ' ';
        else if (str_ieq(argv[1], "left")) key = KEY_LEFT;
        else if (str_ieq(argv[1], "right")) key = KEY_RIGHT;
        else if (str_ieq(argv[1], "up")) key = KEY_UP;
        else if (str_ieq(argv[1], "down")) key = KEY_DOWN;
        else if (str_ieq(argv[1], "escape")) key = KEY_ESCAPE;
    }
    if (!key) { LOGE("usage: uikey <character|space|left|right|up|down|escape>"); return; }
    int wait = app_inject_key(key);
    if (!wait) { LOGE("the input queue is full"); return; }
    g_wait_frames = MAXI(g_wait_frames, wait + 2);
    LOGI("key pressed through the window input path: %s", argv[1]);
}

static void cmd_uilist(int argc, char **argv) {
    static const char *kinds[] = {"button", "slider", "region"};
    int n = ui_widget_count(app.ui);
    for (int i = 0; i < n; i++) {
        const UiWidgetInfo *w = ui_widget(app.ui, i);
        LOGD("  %-7s %-24s id=%-20s (%4.0f,%4.0f) %3.0fx%-3.0f", kinds[CLAMP(w->kind, 0, 2)], w->label, w->id, w->x, w->y,
             w->w, w->h);
    }
    LOGI("%d widgets on screen", n);
}

static void cmd_uiscroll(int argc, char **argv) {
    if (argc < 4) {
        LOGI("usage: uiscroll <x> <y> <points, positive scrolls the content down>   (a wheel turn at a window point)");
        return;
    }
    int wait = app_inject_scroll((float)atof(argv[1]), (float)atof(argv[2]), -(float)atof(argv[3]));
    if (wait > 0) g_wait_frames = wait;
    LOGI("scroll %s points at (%s, %s)", argv[3], argv[1], argv[2]);
}

static void cmd_uiclick(int argc, char **argv) {
    if (argc < 2) {
        LOGI("usage: uiclick <label> [position 0..1 along the widget] [frames held]   (uilist shows labels)");
        return;
    }
    const UiWidgetInfo *w = ui_find_widget(app.ui, argv[1]);
    if (!w) {
        LOGE("no widget '%s' on screen, or the name matches several (uilist shows them; names match exactly first)", argv[1]);
        return;
    }
    double frac = argc > 2 ? CLAMP(atof(argv[2]), 0.0, 1.0) : 0.5;
    int hold = argc > 3 ? CLAMP(atoi(argv[3]), 0, 120) : 4;
    float x = w->x + w->w * (float)frac, y = w->y + w->h * 0.5f;
    int wait = app_inject_click(x, y, hold);
    if (!wait) {
        LOGE("too many queued clicks; '%s' was not clicked", w->label);
        return;
    }
    /* hold the script until the click has actually been delivered, so the next command sees its effect */
    g_wait_frames = MAXI(g_wait_frames, wait + 2);
    LOGI("click '%s' at (%.0f, %.0f), held %d frames", w->label, x, y, hold);
}

static void cmd_uidrag(int argc, char **argv) {
    if (argc < 5) {
        LOGI("usage: uidrag <x0> <y0> <x1> <y1> [alt] [shift] [cmd] [frames N]   (window points)");
        return;
    }
    uint32_t mods = 0;
    int frames = 20;
    for (int i = 5; i < argc; i++) {
        if (str_ieq(argv[i], "alt") || str_ieq(argv[i], "option")) mods |= MOD_ALT;
        else if (str_ieq(argv[i], "shift")) mods |= MOD_SHIFT;
        else if (str_ieq(argv[i], "cmd")) mods |= MOD_CMD;
        else if (str_ieq(argv[i], "frames") && i + 1 < argc) frames = atoi(argv[++i]);
    }
    int wait = app_inject_drag((float)atof(argv[1]), (float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4]), mods, frames);
    if (!wait) {
        LOGE("too many queued input events; the drag was not performed");
        return;
    }
    g_wait_frames = MAXI(g_wait_frames, wait + 2);
    LOGI("drag (%s, %s) -> (%s, %s) over %d frames", argv[1], argv[2], argv[3], argv[4], CLAMP(frames, 2, 60));
}

static void cmd_perf(int argc, char **argv) {
    if (argc > 2 && str_ieq(argv[1], "sync")) {
        app.perf_sync = parse_onoff(argv[2]) == 1;
        LOGI("perf sync %s (glFinish after GPU phases: slower, but GPU time is attributed to the right phase)",
             app.perf_sync ? "on" : "off");
        return;
    }
    refresh_status();
    const SimStatus *st = &app.status;
    LOGD("frame %.2f ms (%.0f fps): events %.2f | uploads %.2f | 3D render %.2f | ui %.2f | swap %.2f", app.perf_frame,
         app.fps, app.perf_events, app.perf_results, app.perf_render, app.perf_ui, app.perf_swap);
    if (!app.perf_sync) LOGI("  GPU work is asynchronous - 'perf sync on' attributes it to its phase");
    char stream_s[16] = "off", iso_s[16] = "off"; /* hidden stages aren't computed, so their last timing is stale */
    if (app.rs.stream_on) snprintf(stream_s, sizeof stream_s, "%.1f ms", app.stream_ms);
    if (app.rs.vortex_on) snprintf(iso_s, sizeof iso_s, "%.1f ms", app.iso_ms);
    LOGD("uploads %.1f MB/s (%.1f field textures/s) | worker: field %.1f ms, streamlines %s, vortices %s",
         app.upload_mb_s, app.field_uploads_s, app.field_ms, stream_s, iso_s);
    LOGD("solver %.1f MLUPS, %.1f steps/s, %.1f snapshots/s, %d threads, %.0f MB", st->mlups, st->steps_per_sec,
         st->outputs_per_sec, st->threads, st->mem_mb);
}

/* ---- the two workspaces ---------------------------------------------------------------------------- */

static void cmd_workspace(int argc, char **argv) {
    if (argc < 2) {
        LOGI("workspace: %s", workspace_name(app.workspace));
        LOGI("usage: workspace fluid|solid|toggle");
        return;
    }
    int ws = str_ieq(argv[1], "toggle") ? (app.workspace == WS_FLUID ? WS_SOLID : WS_FLUID) : workspace_find(argv[1]);
    if (ws < 0) {
        LOGE("unknown workspace '%s' - fluid or solid", argv[1]);
        return;
    }
    app_set_workspace(ws);
}

/* display of the finite-element result inside the shared 3D view */
static void cmd_fem(int argc, char **argv) {
    const FemState *st = fem_state();
    if (argc < 2 || str_ieq(argv[1], "status")) {
        fem_poll(); /* the surface is rebuilt in the poll: report the range of what is actually drawn */
        LOGI("project %s \xC2\xB7 %d bodies \xC2\xB7 %d elements \xC2\xB7 %d conditions \xC2\xB7 material %s",
             st->have_project ? st->project : "(none)", st->nbodies, st->nelems, st->nbcs,
             st->material[0] ? st->material : "none");
        if (st->have_result && st->result_body[0])
            LOGI("result %s: %s, built on body '%s'%s", st->result_job, st->result_kind, st->result_body,
                 st->nbodies == 0 ? " (project.json lists no body: the project was not saved after the import)" : "");
        if (st->job_id[0]) LOGI("job %s: %s %.0f%% %s", st->job_id, st->job_state, st->job_progress * 100, st->job_stage);
        if (st->have_result)
            LOGI("result %s \xC2\xB7 %s \xC2\xB7 %.6g to %.6g %s \xC2\xB7 %d triangles \xC2\xB7 rebuild %.2f ms",
                 st->result_job, fem_field_label(fem_field()), st->range_lo, st->range_hi, fem_field_unit(fem_field()),
                 st->surface_tris, st->rebuild_ms);
        else if (fem_unsupported_reason())
            LOGI("no result displayed: %s", fem_unsupported_reason());
        else
            LOGI("no result displayed");
        LOGI("shown displacement max %.9g mm; deformation scale %.9g; stored index %d", st->max_displacement_mm, fem_deform_applied(), fem_step());
        LOGI("result colour map %s", colormap_name(render_result_colormap(app.renderer)));
        LOGI("groups part %d support %d plate %d", fem_group_shown(0), fem_group_shown(1), fem_group_shown(2));
        if (fem_last_error()[0]) LOGI("last refusal shown in the panel: %s", fem_last_error());
        LOGI("usage: fem field <von_mises|displacement|temperature> | deform <x|auto|true> | step <i|next|prev|last> | range <all|step> | play | pause | speed <n> | surface <on|off|toggle> | section <x|y|z fraction|off|flip> | glass <piece> <opacity> | explode <0..1.5> | edges on|off | shadows on|off | export <file.png> [scale] | job <id> | follow | show [part|support|plate on|off] | hide | fit | debug: grow <x|y|z|off>, cut z <mm> [at <i>], groups z <h1> <h2>");
        return;
    }
    if (str_ieq(argv[1], "field")) {
        if (argc < 3) {
            LOGI("showing %s [%s]", fem_field_label(fem_field()), fem_field_unit(fem_field()));
            return;
        }
        for (int i = 0; i < FEM_FIELD_COUNT; i++)
            if (str_ieq(argv[2], fem_field_name(i))) {
                if (!fem_field_available(i) && st->have_result) {
                    LOGW("%s is not part of this result (%s)", fem_field_name(i), st->result_kind);
                    return;
                }
                fem_set_field(i);
                LOGOK("showing %s [%s]", fem_field_label(i), fem_field_unit(i));
                return;
            }
        LOGE("unknown field '%s' - von_mises, displacement or temperature", argv[2]);
        return;
    }
    if (str_ieq(argv[1], "deform")) {
        if (argc < 3) {
            LOGI("deformation scale: %s\xC3\x97%.6g", fem_deform_auto() ? "auto " : "", fem_deform_applied());
            return;
        }
        if (str_ieq(argv[2], "auto")) {
            fem_set_deform_scale(-1);
            LOGOK("deformation scale follows the part size");
            return;
        }
        double v = str_ieq(argv[2], "true") ? 1.0 : atof(argv[2]);
        if (!(v >= 0)) {
            LOGE("deformation scale must be >= 0 (or 'auto')");
            return;
        }
        fem_set_deform_scale(v);
        LOGOK("deformation scale \xC3\x97%.6g", v);
        return;
    }
    if (str_ieq(argv[1], "step")) {
        int n = fem_step_count();
        if (argc < 3) {
            LOGI("stored time %d of %d (t = %.6g s)", fem_step() + 1, n, st->time_s);
            return;
        }
        int i = str_ieq(argv[2], "next")   ? fem_step() + 1
                : str_ieq(argv[2], "prev") ? fem_step() - 1
                : str_ieq(argv[2], "last") ? n - 1
                : str_ieq(argv[2], "first") ? 0
                                            : atoi(argv[2]) - 1;
        fem_set_step(i);
        fem_poll();
        LOGOK("stored time %d of %d (t = %.6g s)", fem_step() + 1, n, fem_state()->time_s);
        return;
    }
    if (str_ieq(argv[1], "follow")) {
        /* no argument: the job an agent started; an id: that run; 'last': the newest run stored with the open project,
         * so a project opened from disk shows the result it already has */
        char id[64]; str_copy(id, sizeof id, st->pending_job);
        if (argc > 2 && !str_ieq(argv[2], "last")) str_copy(id, sizeof id, argv[2]);
        else if (argc > 2 && st->project_dir[0]) {
            char runs[512];
            snprintf(runs, sizeof runs, "%s/runs", st->project_dir);
            DIR *d = opendir(runs);
            struct dirent *e;
            id[0] = 0;
            while (d && (e = readdir(d)))
                if (!strncmp(e->d_name, "job-", 4) && strcmp(e->d_name, id) > 0) str_copy(id, sizeof id, e->d_name);
            if (d) closedir(d);
        }
        if (id[0]) { fem_follow_job(id); LOGOK("following %s", id); }
        else LOGI("no run to follow");
        return;
    }
    if (str_ieq(argv[1], "surface")) {
        if (argc > 2) {
            if (str_ieq(argv[2], "on")) fem_set_surface_view(true);
            else if (str_ieq(argv[2], "off") || str_ieq(argv[2], "voxels")) fem_set_surface_view(false);
            else if (str_ieq(argv[2], "toggle")) fem_set_surface_view(!fem_surface_view());
            else { LOGE("usage: fem surface on|off|toggle"); return; }
            fem_poll();
        }
        LOGI("drawing %s: %d triangles, largest snap %.4g mm, %d vertices snapped, %d not drawn, mapping %.0f ms",
             fem_surface_view() ? (st->on_surface ? "the part surface" : "the voxel mesh (no surface map)")
                                : "the voxel mesh",
             st->surface_tris, st->snap_max_mm, st->snapped_vertices, st->unmapped_vertices, st->map_ms);
        return;
    }
    if (str_ieq(argv[1], "section")) {
        if (argc > 2) {
            if (str_ieq(argv[2], "off")) fem_section(-1, fem_section_position());
            else if (str_ieq(argv[2], "flip")) fem_section_flip();
            else if (argc == 4 && strlen(argv[2]) == 1 && strchr("xyz", argv[2][0]))
                fem_section((int)(strchr("xyz", argv[2][0])-"xyz"), atof(argv[3]));
            else { LOGE("usage: fem section x|y|z <fraction> | off | flip"); return; }
        }
        LOGI("section %s %c %.4g flip %s", fem_section_on() ? "on" : "off", "xyz"[fem_section_axis()],
             fem_section_position(), fem_section_flipped() ? "on" : "off");
        return;
    }
    /* presentation: glass pieces, the exploded view, feature edges, ambient occlusion and the 3x image */
    if (str_ieq(argv[1], "glass")) {
        if (argc == 4) fem_set_piece_opacity(atoi(argv[2]), (float)atof(argv[3]));
        else if (argc != 2) { LOGE("usage: fem glass <piece> <opacity 0.05..1>"); return; }
        for (int i = 0; i < fem_pieces(); i++) LOGI("piece %d '%s': opacity %.2f", i, fem_piece_name(i), fem_piece_opacity(i));
        if (fem_pieces() < 2) LOGI("one piece: nothing to see through");
        return;
    }
    if (str_ieq(argv[1], "explode")) {
        if (argc == 3) fem_set_explode(atof(argv[2]));
        LOGI("explode %.2f over %d piece(s)", fem_explode(), fem_pieces());
        return;
    }
    if (str_ieq(argv[1], "edges")) {
        if (argc == 3) fem_set_edges(str_ieq(argv[2], "on") || str_ieq(argv[2], "1"));
        LOGI("feature edges %s", fem_edges_on() ? "on" : "off");
        return;
    }
    if (str_ieq(argv[1], "shadows")) {
        if (argc == 3) fem_set_ao(str_ieq(argv[2], "on") || str_ieq(argv[2], "1"));
        LOGI("ambient occlusion %s", fem_ao_on() ? "on" : "off");
        return;
    }
    if (str_ieq(argv[1], "export")) {
        if (argc < 3) { LOGE("usage: fem export <file.png> [scale]"); return; }
        fem_export_image(argv[2], argc > 3 ? atoi(argv[3]) : 3);
        return;
    }
    if (str_ieq(argv[1], "optimise") || str_ieq(argv[1], "optimize")) {
        if (argc < 3) { LOGE("usage: fem optimise <volume fraction 0..1> [filter radius mm] [max iterations]"); return; }
        double vf = atof(argv[2]), r = argc > 3 ? 1e-3 * atof(argv[3]) : 0;
        int iters = argc > 4 ? atoi(argv[4]) : 0;
        if (!(vf > 0 && vf <= 1)) { LOGE("the volume fraction is between 0 and 1"); return; }
        fem_optimize_set_settings(vf, r);
        if (fem_optimize_start(vf, r, iters))
            LOGI("optimising to %.0f %% volume, filter radius %.3g mm, at most %d iterations", 100 * vf,
                 fem_optimize_radius_mm(), iters > 0 ? iters : 60);
        else LOGE("%s", fem_optimize_message());
        return;
    }
    if (str_ieq(argv[1], "optimwait")) { /* scripts: hold the queue until the optimisation ends */
        double secs = argc > 2 ? atof(argv[2]) : 600.0;
        if (!fem_optimize_running()) {
            LOGI("no optimisation is running: %s", fem_optimize_message());
            return;
        }
        g_wait_topo_until = app.time + CLAMP(secs, 1.0, 3600.0);
        LOGI("waiting for the optimisation (up to %.0f s); the window keeps running", CLAMP(secs, 1.0, 3600.0));
        return;
    }
    if (str_ieq(argv[1], "density")) {
        if (argc == 3) fem_optimize_set_show(str_ieq(argv[2], "on") || str_ieq(argv[2], "1"));
        if (!fem_optimize_have()) { LOGI("no optimisation result yet"); return; }
        LOGI("density view %s: %d of %d elements kept at 0.5, compliance %.4g J over %d iterations",
             fem_optimize_show() ? "on" : "off", fem_optimize_kept(), fem_optimize_elements(), fem_optimize_compliance(),
             fem_optimize_iterations());
        return;
    }
    if (str_ieq(argv[1], "optipart")) {
        if (argc < 3) { LOGE("usage: fem optipart <file.stl>"); return; }
        char msg[512];
        if (fem_export_optimised_stl(argv[2], msg, sizeof msg)) LOGOK("optimised part: %s", msg);
        else LOGE("%s", msg);
        return;
    }
    if (str_ieq(argv[1], "cut")) {
        fem_poll();
        if (argc == 3 && str_ieq(argv[2], "off")) { fem_fake_cut_clear(); return; }
        if ((argc == 4 || (argc == 6 && str_ieq(argv[4], "at"))) && str_ieq(argv[2], "z")) {
            if (!fem_fake_cut(2, atof(argv[3]), argc == 6 ? atoi(argv[5]) : fem_step())) LOGE("cut needs a result and a valid zero-based stored index");
        } else LOGE("usage: fem cut z <height_mm> [at <zero-based stored index>]");
        return;
    }
    if (str_ieq(argv[1], "groups")) {
        fem_poll();
        if (argc != 5 || !str_ieq(argv[2], "z") || !fem_fake_groups(2, atof(argv[3]), atof(argv[4])))
            LOGE("usage: fem groups z <h1_mm> <h2_mm> (needs a result and h1 <= h2)");
        return;
    }
    if (str_ieq(argv[1], "show") && argc > 2) {
        int g = str_ieq(argv[2], "part") ? 0 : str_ieq(argv[2], "support") ? 1 : str_ieq(argv[2], "plate") ? 2 : -1;
        if (g < 0 || argc != 4 || (!str_ieq(argv[3], "on") && !str_ieq(argv[3], "off"))) {
            LOGE("usage: fem show part|support|plate on|off"); return;
        }
        fem_show_group(g, str_ieq(argv[3], "on"));
        LOGI("group %s %s", argv[2], fem_group_shown(g) ? "on" : "off"); return;
    }
    if (str_ieq(argv[1], "grow")) {
        /* debug aid: fakes element birth times on a result that carries none of its own */
        const char *a = argc > 2 ? argv[2] : "z";
        int axis = str_ieq(a, "x") ? 0 : str_ieq(a, "y") ? 1 : str_ieq(a, "z") ? 2 : -1;
        if (axis < 0 && !str_ieq(a, "off")) {
            LOGE("usage: fem grow x|y|z|off   (fakes growth on a result that has no element birth of its own)");
            return;
        }
        if (!fem_fake_growth(axis)) {
            if (axis >= 0) LOGE("no result to grow: run an analysis first");
            else LOGOK("growth off: the whole mesh is drawn again");
            return;
        }
        fem_poll();
        if (axis >= 0)
            LOGOK("faked growth along %s over %d stored times: %d triangles at stored time %d", a, fem_step_count(),
                  fem_state()->surface_tris, fem_step() + 1);
        else
            LOGOK("growth off: the whole mesh is drawn again");
        return;
    }
    if (str_ieq(argv[1], "play") || str_ieq(argv[1], "pause")) {
        bool on = str_ieq(argv[1], "play");
        if (on && fem_step_count() < 2) {
            LOGW("this result has a single stored time: nothing to play");
            return;
        }
        fem_set_playing(on);
        if (on)
            LOGOK("playing %d stored times at %.3g per second (colour range: all stored times)", fem_step_count(),
                  fem_play_speed());
        else
            LOGOK("playback paused at stored time %d of %d", fem_step() + 1, fem_step_count());
        return;
    }
    if (str_ieq(argv[1], "speed")) {
        if (argc < 3) {
            LOGI("playback speed: %.3g stored times per second", fem_play_speed());
            return;
        }
        double v = atof(argv[2]);
        if (!(v > 0)) {
            LOGE("playback speed must be positive (stored times per second)");
            return;
        }
        fem_set_play_speed(v);
        LOGOK("playback speed %.3g stored times per second", fem_play_speed());
        return;
    }
    if (str_ieq(argv[1], "job")) {
        if (argc < 3) {
            LOGI("usage: fem job <job_id>   (job_list shows the ids; the window follows the newest one by itself)");
            return;
        }
        fem_show_job(argv[2]);
        fem_poll();
        const FemState *now = fem_state();
        if (now->have_result)
            LOGOK("showing %s \xC2\xB7 %s \xC2\xB7 %d triangles", now->result_job, fem_field_label(fem_field()), now->surface_tris);
        else if (!fem_unsupported_reason()) /* the bridge logs its own reason once */
            LOGI("job %s: %s", argv[2], now->job_state[0] ? now->job_state : "no result yet");
        return;
    }
    if (str_ieq(argv[1], "range")) {
        if (argc < 3) {
            LOGI("colour range: %s (%.6g to %.6g %s)", fem_range_all() ? "all stored times" : "this step",
                 st->range_lo, st->range_hi, fem_field_unit(fem_field()));
            return;
        }
        if (str_ieq(argv[2], "p99") || str_ieq(argv[2], "peak")) {
            fem_set_range_p99(str_ieq(argv[2], "p99"));
            fem_poll();
            LOGOK("colour range %s: %.6g to %.6g %s", fem_range_p99() ? "clipped to the 99th percentile" : "to the peak",
                  fem_state()->range_lo, fem_state()->range_hi, fem_field_unit(fem_field()));
            return;
        }
        if (str_ieq(argv[2], "all") || str_ieq(argv[2], "step")) {
            fem_set_range_all(str_ieq(argv[2], "all"));
            fem_poll();
            LOGOK("colour range over %s: %.6g to %.6g %s", fem_range_all() ? "all stored times" : "this step",
                  fem_state()->range_lo, fem_state()->range_hi, fem_field_unit(fem_field()));
            return;
        }
        LOGE("usage: fem range all|step");
        return;
    }
    if (str_ieq(argv[1], "show") || str_ieq(argv[1], "hide")) {
        bool on = str_ieq(argv[1], "show");
        fem_set_visible(on);
        app.rs.result_on = on && app.workspace == WS_SOLID;
        LOGOK("result surface %s", on ? "shown" : "hidden");
        return;
    }
    if (str_ieq(argv[1], "fit")) {
        vec3 lo, hi;
        if (!fem_world_bounds(&lo, &hi)) {
            LOGW("nothing to frame - run an analysis first");
            return;
        }
        vec3 c = v3_scale(v3_add(lo, hi), 0.5f);
        camera_focus(&app.cam, c, MAXI(v3_len(v3_sub(hi, lo)) * 1.5f, 8.0f));
        LOGOK("framed the analysed part");
        return;
    }
    LOGE("unknown 'fem' action '%s'", argv[1]);
}

/* analysis actions; the heavy lifting is the same typed operations the control socket and MCP use */
static void cmd_solid(int argc, char **argv) {
    const FemState *st = fem_state();
    if (argc < 2 || str_ieq(argv[1], "help")) {
        LOGI("usage: solid run [static|thermal|thermomechanical] [end_time] [time_step]");
        LOGI("       solid new [name] | open [name] | wait [seconds] | cancel | fixbase | import [unit] | export | mesh | status");
        LOGI("every action is an 'am' operation: the interface and the AI drive the same engine");
        return;
    }
    if (str_ieq(argv[1], "status")) {
        cmd_fem(1, argv);
        return;
    }
    if (str_ieq(argv[1], "new")) {
        fem_new_project(argc > 2 ? argv[2] : "project", NULL, 0);
        return;
    }
    if (str_ieq(argv[1], "open")) {
        if (argc < 3) {
            if (fem_list_projects() > 0) console_set_input("solid open ");
            return;
        }
        fem_open_project(argv[2]);
        return;
    }
    if (str_ieq(argv[1], "run")) {
        const char *kind = argc > 2 ? argv[2] : "static";
        const char *analysis = str_ieq(kind, "static") || str_ieq(kind, "static_structural") ? "static_structural"
                               : str_ieq(kind, "thermal") || str_ieq(kind, "transient_thermal") ? "transient_thermal"
                               : str_ieq(kind, "thermomechanical") || str_ieq(kind, "thermomech") ? "thermomechanical"
                                                                                                  : NULL;
        if (!analysis) {
            LOGE("unknown analysis '%s' - static, thermal or thermomechanical", kind);
            return;
        }
        bool ok;
        if (!strcmp(analysis, "static_structural")) {
            ok = fem_op("analysis_run", "{\"analysis\": \"static_structural\"}");
        } else {
            double end = argc > 3 ? atof(argv[3]) : 300.0, dt = argc > 4 ? atof(argv[4]) : 10.0;
            if (!(end > 0) || !(dt > 0)) {
                LOGE("end time and time step must be positive");
                return;
            }
            ok = fem_op("analysis_run", "{\"analysis\": \"%s\", \"end_time\": %.10g, \"time_step\": %.10g}", analysis, end, dt);
        }
        if (!ok) return;
        const JsonValue *v = fem_last_value();
        const char *id = json_get_str(v, "job_id", NULL);
        if (id) {
            fem_follow_job(id);
            LOGOK("%s submitted as %s", analysis, id);
        }
        return;
    }
    if (str_ieq(argv[1], "wait")) {
        if (!st->job_id[0]) {
            LOGW("no analysis has been submitted");
            return;
        }
        if (!st->job_active) {
            LOGI("job %s: %s", st->job_id, st->job_state[0] ? st->job_state : "finished");
            return;
        }
        double secs = argc > 2 ? atof(argv[2]) : 300.0;
        str_copy(g_wait_job, sizeof g_wait_job, st->job_id);
        g_wait_job_until = app.time + CLAMP(secs, 1.0, 3600.0);
        LOGI("waiting for %s (up to %.0f s); the interface keeps running", st->job_id, CLAMP(secs, 1.0, 3600.0));
        return;
    }
    if (str_ieq(argv[1], "cancel")) {
        if (!st->job_id[0]) {
            LOGW("no analysis is running");
            return;
        }
        fem_stop_job(st->job_id);
        return;
    }
    if (str_ieq(argv[1], "fixbase")) {
        /* purely geometric: the lowest face of the part in the build frame, named and described in the project */
        if (!fem_op("selection_create",
                    "{\"name\": \"base\", \"query\": {\"plane\": {\"axis\": \"z\", \"at\": \"min\"}},"
                    " \"description\": \"lowest face of the part, chosen in the interface\","
                    " \"source\": \"user\", \"replace\": true}"))
            return;
        if (fem_op("boundary_apply", "{\"name\": \"base_support\", \"kind\": \"fixed\", \"selection\": \"base\","
                                     " \"description\": \"base held fixed, chosen in the interface\", \"source\": \"user\"}"))
            LOGOK("selection 'base' (lowest face) is held fixed by condition 'base_support'");
        return;
    }
    if (str_ieq(argv[1], "import")) {
        /* a file waiting for its unit wins; otherwise the model in the tunnel. The unit is never guessed. */
        const char *waiting = fem_pending_import();
        char path[1200];
        str_copy(path, sizeof path, waiting ? waiting : app.model_path);
        if (!path[0] || (!waiting && !app.has_model)) {
            LOGE("no file to import - drop an STL on the window, press OPEN, or load one into the tunnel first");
            return;
        }
        if (!st->have_project && !fem_new_project("project", NULL, 0)) return;
        if (argc < 3) {
            LOGI("choose the unit '%s' is written in: solid import mm|cm|m|um|in|ft (the PART step has buttons)", path);
            return;
        }
        char stem[128], name[128];
        const char *base = strrchr(path, '/');
        str_copy(stem, sizeof stem, base ? base + 1 : path);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        fem_free_body_name(stem, name, sizeof name); /* a second copy of the same file gets its own name */
        if (fem_op("geometry_import", "{\"path\": \"%s\", \"units\": \"%s\", \"units_source\": \"user\", \"name\": \"%s\"}",
                   path, argv[2], name)) {
            fem_set_pending_import("");
            LOGOK("imported %s as '%s' in %s", path, fem_state()->body, argv[2]);
        }
        return;
    }
    if (str_ieq(argv[1], "mesh")) { /* what the MESH step shows, so a test can read it back */
        LOGI("element size shown %.6g mm; project %.6g mm; suggested %.6g mm", fem_mesh_size_mm(),
             fem_project_mesh_mm(), fem_suggested_mesh_mm());
        LOGI("inside test: %lld uncertain cells of %d elements, %lld rays abstained", st->uncertain_cells, st->nelems,
             st->abstained_rays);
        return;
    }
    if (str_ieq(argv[1], "repair")) {
        if (!st->have_project || st->nbodies == 0) {
            LOGE("open a part first");
            return;
        }
        fem_repair_body();
        return;
    }
    if (str_ieq(argv[1], "export")) {
        if (!st->have_project || st->nbodies == 0) {
            LOGE("the analysis project has no geometry to send to the tunnel");
            return;
        }
        char path[1024] = "";
        if (!fem_body_source_path(path, sizeof path)) {
            LOGE("the body has no source file on disk");
            return;
        }
        if (app_load_stl(path)) LOGOK("%s is now the model in the tunnel", st->body);
        return;
    }
    LOGE("unknown 'solid' action '%s' - try 'solid help'", argv[1]);
}

static void cmd_agent(int argc, char **argv) {
    if (argc > 1 && str_ieq(argv[1], "wait")) {
        double secs = argc > 2 ? atof(argv[2]) : 600.0;
        if (!agent_running()) {
            LOGI("the agent is not running");
            return;
        }
        g_wait_agent_until = app.time + CLAMP(secs, 1.0, 3600.0);
        LOGI("waiting for the agent (up to %.0f s); the window keeps running", CLAMP(secs, 1.0, 3600.0));
        return;
    }
    if (argc > 1 && str_ieq(argv[1], "stop")) {
        agent_stop();
        return;
    }
    if (argc > 2 && str_ieq(argv[1], "command")) { /* the settings field, from a script */
        char line[2048] = "";
        for (int i = 2; i < argc; i++) {
            /* a relative path that exists here is made absolute: the agent runs in the project's folder */
            char abs[1024] = "";
            if (argv[i][0] != '/' && strchr(argv[i], '/') && access(argv[i], R_OK) == 0 && getcwd(abs, sizeof abs))
                snprintf(abs + strlen(abs), sizeof abs - strlen(abs), "/%s", argv[i]);
            snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s", i > 2 ? " " : "", abs[0] ? abs : argv[i]);
        }
        hud_agent_set_command(line);
        LOGOK("agent command: %s", line);
        return;
    }
    LOGI("agent %s, %d transcript lines", agent_running() ? "running" : "idle", agent_line_count());
}

static void cmd_mode(int argc, char **argv) {
    if (argc > 1) {
        int m = -1;
        if (str_ieq(argv[1], "manual")) m = UI_ADVANCED;
        if (str_ieq(argv[1], "agentic")) m = UI_AGENT;
        for (int i = 0; i < UI_MODE_COUNT; i++)
            if (str_ieq(argv[1], ui_mode_name(i))) m = i;
        if (m < 0) {
            LOGE("usage: mode manual|agentic (advanced|agent|simple remain aliases)");
            return;
        }
        app_set_ui_mode(m);
    }
    LOGOK("mode %s", ui_mode_name(app.ui_mode));
}

static void cmd_uitext(int argc, char **argv) {
    (void)argc, (void)argv;
    const char *t = hud_panel_text();
    if (!t || !t[0]) {
        LOGI("the analysis panel wrote nothing this frame");
        return;
    }
    LOGI("panel text:");
    char line[256];
    for (const char *p = t; *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        snprintf(line, sizeof line, "%.*s", (int)MINI(n, sizeof line - 1), p);
        if (line[0]) LOGD("  %s", line);
        p += n + (nl ? 1 : 0);
    }
}

/* A stopwatch for scripts: 'elapsed reset' starts it, 'elapsed <label>' says how long since then. Measuring is the
 * only way to claim a load or a mesh is fast enough. */
static void cmd_elapsed(int argc, char **argv) {
    static double t0 = -1;
    int first = 1;
    if (argc > 1 && str_ieq(argv[1], "reset")) {
        t0 = now_seconds();
        first = 2;
    }
    if (t0 < 0) t0 = now_seconds();
    char label[160] = "";
    for (int i = first; i < argc; i++)
        snprintf(label + strlen(label), sizeof label - strlen(label), "%s%s", i > first ? " " : "", argv[i]);
    LOGI("elapsed %.2f s%s%s", now_seconds() - t0, label[0] ? " - " : "", label);
}

static void cmd_echo(int argc, char **argv) {
    char buf[1024] = {0};
    for (int i = 1; i < argc; i++) {
        strncat(buf, argv[i], sizeof buf - strlen(buf) - 2);
        strcat(buf, " ");
    }
    LOGI("%s", buf);
}

static void cmd_clear(int argc, char **argv) { console_clear(); }
static void cmd_quit(int argc, char **argv) { app.quit = true; }

/* ---- NAVIER-AM operations: the terminal front end of the typed operation layer ----------------------------- */

/* key=value values: JSON objects/arrays/strings when they start with { [ or ", booleans, null, plain numbers;
 * anything else (including quantities such as 0.2mm) is passed as a string */
static JsonValue *am_value(const char *s) {
    if (*s == '{' || *s == '[' || *s == '"') {
        JsonValue *v = json_parse(s, strlen(s), NULL, NULL);
        if (v) return v;
    }
    if (!strcmp(s, "true")) return json_bool(true);
    if (!strcmp(s, "false")) return json_bool(false);
    if (!strcmp(s, "null")) return json_null();
    char *end;
    double d = strtod(s, &end);
    if (end != s && *end == 0 && isfinite(d)) return json_number(d);
    return json_string(s);
}

static void am_print(const JsonValue *v, int max_lines) {
    char *text = json_dump(v, JSON_PRETTY, NULL, NULL);
    if (!text) return;
    int lines = 0;
    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (lines++ < max_lines) LOGD("%s", line);
        line = nl ? nl + 1 : NULL;
    }
    if (lines > max_lines) LOGI("... %d more lines", lines - max_lines);
    free(text);
}

static void cmd_am(int argc, char **argv) {
    if (!app.engine) {
        LOGE("the NAVIER-AM engine is not available");
        return;
    }
    if (argc < 2 || str_ieq(argv[1], "list") || str_ieq(argv[1], "help")) {
        LOGOK("NAVIER-AM operations (the same as the control socket and the MCP tools):");
        for (int i = 0; i < ops_count(); i++) LOGD("  %-22s %s", ops_at(i)->name, ops_at(i)->title);
        LOGI("usage: am <operation> key=value ...  or  am <operation> '{\"json\": \"parameters\"}'");
        LOGI("values: numbers, true/false, text, quantities with units (z_offset=2mm), JSON in single quotes");
        return;
    }
    JsonValue *params = json_object();
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (argc == 3 && a[0] == '{') {
            JsonError jerr;
            JsonValue *p = json_parse(a, strlen(a), NULL, &jerr);
            if (!p) {
                LOGE("invalid JSON parameters: %s (column %d)", jerr.message, jerr.column);
                json_free(params);
                return;
            }
            json_free(params);
            params = p;
            break;
        }
        const char *eq = strchr(a, '=');
        if (!eq || eq == a) {
            LOGE("expected key=value, got '%s'", a);
            json_free(params);
            return;
        }
        char key[128];
        snprintf(key, sizeof key, "%.*s", (int)(eq - a), a);
        json_set(params, key, am_value(eq + 1));
    }
    OpResult r;
    OpCaller caller = {"console", "navier-terminal"};
    ops_invoke(app.engine, argv[1], params, &caller, &r);
    json_free(params);
    if (r.ok) {
        LOGOK("%s ok (project revision %llu%s)", argv[1], (unsigned long long)r.revision, r.replayed ? ", replayed" : "");
        am_print(r.value, 80);
    } else {
        LOGE("%s: %s", json_get_str(r.error, "code", "ERROR"), json_get_str(r.error, "message", ""));
        const char *hint = json_get_str(r.error, "hint", NULL);
        if (hint) LOGI("hint: %s", hint);
    }
    op_result_free(&r);
}

static const char *complete_am(int argi, const char *p, int index) {
    if (argi != 1) return NULL;
    int k = 0;
    for (int i = 0; i < ops_count(); i++)
        if (str_starts_with_i(ops_at(i)->name, p) && k++ == index) return ops_at(i)->name;
    return NULL;
}

static void cmd_help(int argc, char **argv);

static void cmd_lab(int argc, char **argv) { labapp_command(argc, argv); }

static const Command COMMANDS[] = {
    /* simulation */
    {"start", "", "start / resume the experiment (Space)", cmd_start, NULL},
    {"pause", "", "pause the solver (Space)", cmd_pause, NULL},
    {"reset", "", "restart the flow field, keep the geometry (R)", cmd_reset, NULL},
    {"step", "[n]", "advance n time steps while paused", cmd_step, NULL},
    {"playback", "<percent|max|slower|faster>", "simulation speed: slow motion down to 1% of full solver speed (, and . keys)", cmd_playback, c_playback},
    {"kick", "[amplitude]", "start-up perturbation: short cross-flow pulse that triggers vortex shedding", cmd_kick, NULL},
    {"status", "", "full report: solver, units, forces, roughness regime", cmd_status, NULL},
    /* model */
    {"scene", "<name>", "load a preset experiment (glider sphere cylinder car submarine wing airfoil plate torus)", cmd_scene, complete_scenes},
    {"shape", "<name>", "replace the model with a built-in shape", cmd_shape, complete_shapes},
    {"load", "<file.stl>", "import an STL model (or drag & drop onto the window)", cmd_load, NULL},
    {"open", "", "choose an STL file with a dialog (Cmd-O)", cmd_open, NULL},
    {"export", "<file.stl>", "save the current model as binary STL", cmd_export, NULL},
    {"unload", "", "remove the model (empty tunnel)", cmd_unload, NULL},
    {"model", "", "model placement and size info", cmd_model, NULL},
    {"aoa", "<deg>", "angle of attack (nose-up pitch)", cmd_aoa, NULL},
    {"yaw", "<deg>", "yaw angle (nose left)", cmd_yaw, NULL},
    {"roll", "<deg>", "roll angle", cmd_roll, NULL},
    {"fit", "<frac> [length]", "model size as a fraction of the tunnel cross-section [and length]", cmd_fit, NULL},
    {"pos", "<x> <y> <z>", "model position as fractions of the tunnel", cmd_pos, NULL},
    {"rotate", "<x|y|z> <deg>", "permanently rotate the model mesh", cmd_rotate, NULL},
    {"orient", "<zup|flip|auto|reset>", "fix STL orientation: Z-up file, reversed nose, longest side along the flow, as loaded", cmd_orient, c_orient},
    {"shell", "<cells>", "thin-surface voxel shell radius (0 = off)", cmd_shell, NULL},
    {"boundary", "<curved|staircase>", "model walls: interpolated bounce-back at the exact mesh surface, or voxel staircase", cmd_boundary, NULL},
    /* fluid & flow */
    {"fluid", "<name>", "water seawater air glycerin oil honey mercury", cmd_fluid, complete_fluids},
    {"temp", "<\xC2\xB0""C>", "fluid temperature (sets density & viscosity)", cmd_temp, NULL},
    {"speed", "<v> [m/s|km/h|kn]", "free-stream speed", cmd_speed, NULL},
    {"re", "<Reynolds>", "set the speed that gives this Reynolds number", cmd_re, NULL},
    {"length", "<L> [m|cm|mm]", "physical reference length of the model", cmd_length, NULL},
    {"refaxis", "<x|y|z|max>", "model extent used as the reference length", cmd_refaxis, NULL},
    {"viscosity", "<m\xC2\xB2/s>", "custom kinematic viscosity", cmd_viscosity, NULL},
    {"density", "<kg/m\xC2\xB3>", "custom density", cmd_density, NULL},
    {"roughness", "<ks> [um|mm|m]", "equivalent sand-grain surface roughness ('smooth' = 0)", cmd_roughness, NULL},
    {"turbulence", "<percent>", "inlet turbulence intensity", cmd_turbulence, NULL},
    /* numerics */
    {"grid", "<nx> <ny> <nz>", "lattice size (rebuilds the simulation)", cmd_grid, NULL},
    {"quality", "<draft|normal|high|ultra>", "rescale the lattice keeping the tunnel proportions", cmd_quality, NULL},
    {"ulb", "<u>", "lattice inlet velocity 0.005..0.2, slider 0.02..0.15 (speed vs accuracy)", cmd_ulb, NULL},
    {"collision", "<rr|reg|bgk> [bulk tau] [hrr sigma]", "collision operator (rr default), bulk viscosity, finite-difference hybrid", cmd_collision, NULL},
    {"les", "<Cs|on|off>", "Smagorinsky LES turbulence model constant", cmd_les, complete_onoff},
    {"walls", "<type>", "all tunnel walls: slip freestream noslip moving periodic", cmd_walls, complete_walls},
    {"floor", "<type>", "floor boundary (moving = rolling road)", cmd_floor, complete_walls},
    {"ceiling", "<type>", "ceiling boundary", cmd_ceiling, complete_walls},
    {"sides", "<type>", "side wall boundaries", cmd_sides, complete_walls},
    {"threads", "<n>", "solver threads", cmd_threads, NULL},
    {"init", "<rest|flow>", "initial condition after reset", cmd_init, NULL},
    {"ramp", "<steps>", "inflow start-up ramp length", cmd_ramp, NULL},
    /* visualisation */
    {"view", "<field>", "speed ux uy uz pressure cp vorticity q  (keys 1-8)", cmd_view, complete_fields},
    {"stats", "", "field statistics, colour range and visualisation timings", cmd_stats, NULL},
    {"cmap", "<name>", "colour map: turbo cfd jet viridis plasma inferno magma coolwarm hydro icefire gray custom", cmd_cmap, complete_cmaps},
    {"linecmap", "<name|same>", "colour map for streamlines and particles", cmd_linecmap, complete_cmaps},
    {"gradient", "<#hex ...>", "define a custom colour gradient", cmd_gradient, NULL},
    {"range", "<auto|min max>", "colour range in display units", cmd_range, NULL},
    {"surface", "<off|solid|field|grid>", "model surface colouring", cmd_surface, c_surface},
    {"slice", "<off|x|y|z> [pos] [opacity]", "cut plane through the flow (L, X/Y/Z, [ ])", cmd_slice, c_slice},
    {"streamlines", "<on|off|volume|plane|grid|line|point|wake|random|at x y z|size h w|normal|auto|probe|seeds N|width|steps|animate>", "streamline rakes: shape, position (fractions of the tunnel), size (S)", cmd_streamlines, c_stream},
    {"particles", "<on|off|count N|size px|emitter sheet|model|full|reset>", "GPU tracer particles (P)", cmd_particles, c_particles},
    {"vortices", "<on|off|level L>", "Q-criterion vortex surfaces; at Q (L/U)^2 = level, from the 2-cell filtered field (V)", cmd_vortices, c_vortex},
    {"volume", "<on|off|density D>", "volume rendering of the field (O)", cmd_volume, c_volume},
    {"bloom", "<0..3>", "glow strength", cmd_bloom, NULL},
    {"backdrop", "<default|neutral>", "the tunnel's backdrop: blue vignette, or the showcase preset's dark neutral", cmd_backdrop, NULL},
    {"exposure", "<v>", "image exposure", cmd_exposure, NULL},
    {"msaa", "<1|2|4|8>", "anti-aliasing samples", cmd_msaa, NULL},
    {"floorgrid", "<on|off>", "floor grid", cmd_floorgrid, complete_onoff},
    {"box", "<on|off>", "tunnel outline", cmd_box, complete_onoff},
    {"hud", "<on|off>", "interface overlay (H)", cmd_hud, complete_onoff},
    {"fps", "<n|off>", "cap the render frame rate (default 60) so the solver gets the spare CPU", cmd_fps, NULL},
    /* camera & output */
    {"camera", "<iso|front|back|side|top|bottom|model|lock|unlock|orbit y p|zoom f|fov d>", "camera presets and control", cmd_camera, complete_cams},
    {"probe", "<x y z|off>", "sample the flow at a point (or Alt-click)", cmd_probe, NULL},
    {"screenshot", "[file.png]", "save an image (Cmd-S)", cmd_screenshot, NULL},
    {"forces", "[file.csv]", "export the force coefficient history", cmd_forces, NULL},
    {"exec", "<file>", "run a script of commands (.nav)", cmd_exec, NULL},
    {"wait", "<steps>", "scripts: run the solver this many steps before continuing", cmd_wait, NULL},
    {"log", "[file.csv]", "append the averaged force coefficients and conditions to a CSV", cmd_log, NULL},
    {"sweep", "<param> <from> <to> <step> [steps N] [file.csv]", "automated series of runs (aoa, speed, roughness, ...)", cmd_sweep, NULL},
    {"cancel", "", "stop a running script or sweep", cmd_cancel, NULL},
    {"stop", "", "stop every simulation that is running or waiting", cmd_stop, NULL},
    {"frames", "<n>", "scripts: pause the script for n rendered frames", cmd_frames, NULL},
    {"perf", "[sync on|off]", "frame-time breakdown, upload volume and solver throughput", cmd_perf, NULL},
    {"uilist", "", "list the buttons and sliders on screen (UI testing)", cmd_uilist, NULL},
    {"uiclickat", "<x> <y> [frames]", "click a point of the 3D view, e.g. to pick a face (UI testing)", cmd_uiclickat, NULL},
    {"uiscroll", "<x> <y> <points>", "turn the wheel at a window point (UI testing)", cmd_uiscroll, NULL},
    {"uikeys", "<text>", "type into the focused panel field through the real key path (UI testing)", cmd_uikeys, NULL},
    {"uikey", "<key>", "press a keyboard shortcut through the window event path (UI testing)", cmd_uikey, NULL},
    {"uiclick", "<label> [pos] [frames]", "click a button or slider through the real input path (UI testing)", cmd_uiclick, NULL},
    {"uidrag", "<x0> <y0> <x1> <y1> [alt] [shift] [frames N]", "drag through the real input path, e.g. Option-drag the model (UI testing)", cmd_uidrag, NULL},
    /* additive manufacturing */
    {"am", "<operation> [key=value ...]", "NAVIER-AM operations: the typed operations shared with the control socket and MCP (am lists them)", cmd_am, complete_am},
    {"lab", "<open|run|close|field|frame|play|pause|view|orbit|mesh|cmap|range|info>", "the physics lab: open a result (.lab) or run a scenario (.json), play it, turn it", cmd_lab, NULL},
    {"workspace", "[fluid|solid|toggle]", "switch between the water tunnel and the finite-element analysis", cmd_workspace, NULL},
    {"solid", "<new|open|run|wait|cancel|fixbase|import|export|mesh|repair|status>", "analysis actions: run a solve, support the base, share geometry with the tunnel", cmd_solid, NULL},
    {"fem", "<field|deform|step|range|play|pause|speed|section|glass|explode|edges|shadows|export|job|show|hide|fit>",
     "how the finite-element result is drawn in the shared 3D view", cmd_fem, NULL},
    {"agent", "wait [seconds] | stop | status | command <line>", "scripts: wait for the agent's command to end, or stop it", cmd_agent, NULL},
    {"mode", "[manual|agentic]", "control the physics yourself or use your AI tool", cmd_mode, NULL},
    {"uitext", "", "print every sentence the analysis panel wrote this frame (UI testing)", cmd_uitext, NULL},
    {"elapsed", "[reset] [label]", "seconds since the last reset: how long a step of a script actually took", cmd_elapsed, NULL},
    {"echo", "<text>", "print text", cmd_echo, NULL},
    {"clear", "", "clear the terminal (Ctrl-L)", cmd_clear, NULL},
    {"help", "[command]", "list commands", cmd_help, NULL},
    {"quit", "", "exit NAVIER (Cmd-Q)", cmd_quit, NULL},
};

static void cmd_help(int argc, char **argv) {
    bool all = argc > 1 && str_ieq(argv[1], "all");
    if (argc > 1 && !all) {
        const Command *c = console_find(argv[1]);
        if (!c) {
            LOGE("no command '%s'", argv[1]);
            return;
        }
        LOGD("%s %s", c->name, c->args);
        LOGI("    %s", c->help);
        return;
    }
    static const struct {
        const char *title;
        const char *first;
    } groups[] = {{"SIMULATION", "start"}, {"MODEL", "scene"}, {"FLUID & FLOW", "fluid"}, {"NUMERICS", "grid"},
                  {"VISUALISATION", "view"}, {"CAMERA & OUTPUT", "camera"}, {"SCRIPTS & EXPERIMENTS", "exec"},
                  {"ADDITIVE MANUFACTURING", "am"}, {"TERMINAL", "echo"}};
    const int ngroups = (int)ARRAY_LEN(groups);
    int g = 0;
    char names[1024] = {0};
    for (size_t i = 0; i <= ARRAY_LEN(COMMANDS); i++) {
        bool end = i == ARRAY_LEN(COMMANDS);
        if (end || (g < ngroups && !strcmp(COMMANDS[i].name, groups[g].first))) {
            if (names[0]) {
                LOGD("  %s", names);
                names[0] = 0;
            }
            if (end) break;
            LOGOK("%s", groups[g++].title);
        }
        if (all) {
            LOGD("  %s %s", COMMANDS[i].name, COMMANDS[i].args);
            LOGI("      %s", COMMANDS[i].help);
        } else {
            strncat(names, COMMANDS[i].name, sizeof names - strlen(names) - 3);
            strcat(names, "  ");
        }
    }
    LOGI("'help <command>' for details (try: help sweep), 'help all' lists everything. Tab completes, "
         "\xE2\x86\x91/\xE2\x86\x93 history, Esc leaves the terminal.");
}

void register_commands(void) { console_register(COMMANDS, (int)ARRAY_LEN(COMMANDS)); }
