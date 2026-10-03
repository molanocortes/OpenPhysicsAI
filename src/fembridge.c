/* fembridge.c - interface <-> NAVIER-AM engine bridge: state mirror, operation dispatch and the result surface */
#include "fembridge.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#include "app.h"
#include "common.h"
#include "ctl/engine_internal.h"
#include "ctl/ops.h"
#include "ctl/ops_internal.h"
#include "ctl/static_analysis.h"
#include "ctl/transient_analysis.h"
#include "core/base64.h"
#include "fem/hex8.h"
#include "geom/mesh.h"
#include "geom/repair.h"
#include "vis.h"

typedef struct Bridge {
    FemState st;
    uint64_t seen_changes;
    bool force_refresh;
    double next_job_poll;   /* app.time of the next job_status call */
    double next_job_scan;   /* app.time of the next job_list scan for jobs started elsewhere */
    FemJobInfo active[8];   /* queued and running jobs of the whole engine, running first */
    int nactive;
    char scan_newest[64];   /* newest job id seen by a scan, so each job is examined once */
    char unsupported[200];       /* why the followed job cannot be drawn here */
    bool supports_neutral;       /* supports drawn grey rather than coloured (Simple mode) */
    JsonValue *material_list;    /* material_list's answer, and the engine change counter it was asked at */
    uint64_t material_list_changes;
    char result_body[64];        /* the body a result loaded from its run directory was built on */
    char result_body_job[64];    /* ... and that result's job */
    JsonValue *disk_summary;     /* that run's summary.json, for a job from an earlier session */
    char unsupported_logged[200]; /* the last reason written to the log, so a retry does not repeat it */
    JsonValue *last_value;
    JsonValue *conditions;
    uint64_t conditions_rev;
    JsonValue *summary;      /* of the followed job, once it has finished */
    char caveat[120];
    char last_error[220];

    int field;
    int step;
    bool range_all;   /* transient: colour by the range over every stored time */
    bool range_p99;   /* clip the top of the colour range to the 99th percentile of what is drawn */
    /* playback: a position in stored times advanced from the frame clock, never from a sleep */
    bool playing;
    double play_speed;      /* stored times per second */
    double play_pos;        /* fractional stored-time position */
    double play_clock;      /* app.time of the last advance */
    bool range_all_before;  /* the user's colour-range choice, restored when playback stops */
    double deform;          /* < 0 = automatic */
    double deform_used;
    bool visible;

    /* what the surface was built from */
    bool surface_valid, surface_dirty;
    char surface_job[64];
    int surface_field, surface_step;
    double surface_deform;
    uint64_t surface_changes;
    IsoMesh surf;
    uint64_t surf_generation;
    float surf_lo, surf_hi;
    vec3 wlo, whi;

    /* element visibility: face adjacency of the mesh (computed once per result) and the debug growth field */
    int *adj;               /* 6*nelems: the element across each local face, -1 when there is none */
    int adj_nelems;
    int adj_etype; /* element type the adjacency was built for */
    char adj_job[64];
    /* 'fem grow', 'fem cut', 'fem groups': stand-ins used only on a result that carries no element life of its
     * own. The FFF print writes elem_birth and the LPBF build writes birth, death and group, and those win. */
    int *fake_birth;
    int *fake_death;
    unsigned char *fake_group;
    char fake_job[64];
    int section_axis;
    double section_fraction, section_lo[3], section_hi[3];
    bool section_on, section_flip, groups[3];
    float *outline;
    int outline_vertices;
    int fake_nelems;
    int fake_axis;          /* 0 x, 1 y, 2 z, -1 off */
    /* the range over all stored times depends on the result, the field and what is visible, not on the stored time
     * being drawn: computing it once per change keeps a playback frame cheap */
    uint64_t vis_gen;
    double mesh_mm;           /* the size the user set in the MESH step */
    uint64_t mesh_mm_rev;     /* the project revision it was set at: a later change wins over a stale slider */
    char pending_import[1024];/* a dropped or chosen file whose length unit has not been answered yet */
    int picked[512], npicked; /* surface patches collected by clicking on the part or by dragging a box */
    double picked_area_mm2, picked_normal[3];
    int hover_patch;
    double hover_next;
    float hover_px, hover_py;
    int hold_count, load_count;
    const int *tri_patch;     /* triangle -> patch of the drawn geometry, for the highlight */
    int tri_patch_n;
    double deform_umax_all;   /* largest displacement over all stored times, so AUTO does not jump per step */
    double print_fit_lo[3], print_fit_hi[3]; /* visible domain over all stored times, build frame */
    bool print_fit_valid;                  /* cached with deform_job/deform_gen, not rescanned for each frame */
    uint64_t deform_gen;
    char deform_job[64];
    int deform_field_unused;
    uint64_t range_gen;
    char range_job[64];
    int range_field;
    double range_all_lo, range_all_hi;
    bool range_all_valid;
    float *cent;            /* 3*nelems element centroids (m), cached with the adjacency */
    double cell[3];         /* element size of the cached mesh (m) */
    double rebuild_ms;      /* time the last surface rebuild took */

    /* The result drawn on the part's own surface. The solver's domain is the voxel mesh; what the engineer looks at
     * is the STL, with every one of its vertices carrying the field and the displacement interpolated from the
     * element that contains it. SURFACE / VOXELS switches between the two, because the mesh the numbers came from
     * has to stay visible. */
    bool surface_view;      /* true: the STL carries the result; false: the voxel boundary does */
    /* Pieces of what is drawn: the connected components of the surface (the shells of an STL, the face-connected
     * regions of a mesh, one per body when a project has several). Each can be made glass and moved outwards. */
    ResultPart parts[FEM_MAX_DRAW_PARTS];
    ResultPart edge_parts[FEM_MAX_DRAW_PARTS];
    char piece_name[FEM_MAX_DRAW_PARTS][40];
    double piece_home_m[FEM_MAX_DRAW_PARTS][3]; /* piece centroid in the build frame (m) */
    double piece_lo_m[FEM_MAX_DRAW_PARTS][3], piece_hi_m[FEM_MAX_DRAW_PARTS][3];
    int npieces;
    float piece_opacity[FEM_MAX_DRAW_PARTS];
    double explode;         /* 0: home; 1: one part-size apart */
    bool edges_on;
    float *edges;           /* feature edges of the geometry: GL_LINES vertex pairs in world space */
    int edge_vertices;
    int *comp_piece;        /* STL component -> drawn piece (surface view) */
    int comp_piece_n;
    int *elem_piece;        /* piece per element (mesh views), NULL when there is one piece */
    int elem_piece_n;
    int draw_piece;         /* the piece being emitted, -1 for all */
    double preview;         /* 0..1: the build has reached this fraction of the part's height; -1 off */
    int *map_elem;          /* nv: element containing each STL vertex, -1 when none was near enough */
    float *map_w;           /* 8*nv trilinear weights inside that element */
    float *map_normal;      /* 3*nv crease-smoothed vertex normals of the STL, in the build frame */
    int map_nv;
    char map_job[64];
    uint64_t map_geom_revision;
    int map_nelems;
    double map_ms;          /* time the mapping took to build */
    double map_snap_max_mm, map_snap_mean_mm;
    int map_snapped, map_unmapped;
    vec3 peak_world;        /* where the largest field value of the drawn surface sits */
    bool peak_valid;
    float tet_peak; /* the largest drawn value of a tetrahedral surface, for the peak marker */

    /* build frame -> world */
    bool have_place;
    double centre_m[3];     /* build-frame centre of the displayed body */
    double to_world_scale;  /* world units per metre */
    vec3 world_centre;
} Bridge;

static Bridge B;

static const char *const FIELD_NAME[FEM_FIELD_COUNT] = {"von_mises", "displacement", "temperature"};
static const char *const FIELD_LABEL[FEM_FIELD_COUNT] = {"von Mises stress", "displacement", "temperature"};
static const char *const FIELD_UNIT[FEM_FIELD_COUNT] = {"MPa", "mm", ("\xC2\xB0" "C")};

const char *fem_field_name(int f) { return FIELD_NAME[CLAMP(f, 0, FEM_FIELD_COUNT - 1)]; }
const char *fem_field_label(int f) { return FIELD_LABEL[CLAMP(f, 0, FEM_FIELD_COUNT - 1)]; }
const char *fem_field_unit(int f) { return FIELD_UNIT[CLAMP(f, 0, FEM_FIELD_COUNT - 1)]; }

const FemState *fem_state(void) { return &B.st; }
int fem_field(void) { return B.field; }
int fem_step(void) { return B.step; }
int fem_step_count(void) { return MAXI(B.st.nsteps, 1); }
double fem_deform_scale(void) { return B.deform; }
bool fem_deform_auto(void) { return B.deform < 0; }
bool fem_range_all(void) { return B.range_all; }
bool fem_range_p99(void) { return B.range_p99; }
void fem_set_range_p99(bool on) {
    if (B.range_p99 == on) return;
    B.range_p99 = on;
    B.surface_dirty = true;
}

void fem_set_range_all(bool on) {
    if (on == B.range_all) return;
    B.range_all = on;
    B.surface_dirty = true;
}
double fem_deform_applied(void) { return B.deform_used; }
bool fem_visible(void) { return B.visible; }
const struct JsonValue *fem_last_value(void) { return B.last_value; }

int fem_operation_count(void) { return ops_count(); }

const char *fem_last_error(void) { return B.last_error; }

/* ---- projects in the workspace ------------------------------------------------------------------------------ */

static bool project_file_in(const char *dir, const char *name, time_t *mtime) {
    char path[NV_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s/project.json", dir, name);
    struct stat sb;
    if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) return false;
    if (mtime) *mtime = sb.st_mtime;
    return true;
}

bool fem_new_project(const char *base, char *name_out, size_t cap) {
    if (!app.engine) {
        LOGE("the NAVIER-AM engine is not available");
        return false;
    }
    const char *ws = engine_get_config(app.engine)->workspace;
    char name[64];
    snprintf(name, sizeof name, "%s", base && base[0] ? base : "project");
    for (int i = 2; project_file_in(ws, name, NULL) && i < 1000; i++) snprintf(name, sizeof name, "%.50s-%d", base, i);
    if (!fem_op("project_create", "{\"name\": \"%s\", \"description\": \"created from the NAVIER interface\"}", name)) return false;
    if (name_out) str_copy(name_out, cap, name);
    LOGOK("new project '%s' in %s - next: IMPORT STL, or 'solid import <unit>' for the model in the tunnel", name, ws);
    return true;
}

typedef struct ProjectEntry {
    char name[64];
    time_t mtime;
} ProjectEntry;

static int by_newest(const void *a, const void *b) {
    const ProjectEntry *x = a, *y = b;
    return (x->mtime < y->mtime) - (x->mtime > y->mtime);
}

int fem_list_projects(void) {
    if (!app.engine) return 0;
    const char *ws = engine_get_config(app.engine)->workspace;
    DIR *d = opendir(ws);
    if (!d) {
        LOGI("no projects yet in %s", ws);
        return 0;
    }
    ProjectEntry e[256];
    int n = 0;
    for (struct dirent *de; (de = readdir(d)) && n < (int)ARRAY_LEN(e);) {
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof e[n].name) continue;
        if (project_file_in(ws, de->d_name, &e[n].mtime)) str_copy(e[n].name, sizeof e[n].name, de->d_name), n++;
    }
    closedir(d);
    if (n == 0) {
        LOGI("no projects yet in %s - NEW PROJECT creates one", ws);
        return 0;
    }
    qsort(e, (size_t)n, sizeof e[0], by_newest);
    LOGOK("%d project%s in %s (newest first) - open one with: solid open <name>", n, n == 1 ? "" : "s", ws);
    for (int i = 0; i < n; i++) {
        char when[32];
        strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&e[i].mtime));
        LOGD("  %-32s %s", e[i].name, when);
    }
    return n;
}

bool fem_open_project(const char *name_or_path) {
    if (!app.engine || !name_or_path || !name_or_path[0]) return false;
    char path[NV_PATH_MAX];
    if (strchr(name_or_path, '/'))
        str_copy(path, sizeof path, name_or_path);
    else
        snprintf(path, sizeof path, "%s/%s", engine_get_config(app.engine)->workspace, name_or_path);
    if (!fem_op("project_open", "{\"path\": \"%s\"}", path)) return false;
    fem_follow_job("");
    B.st.result_job[0] = 0;
    LOGOK("opened %s", path);
    return true;
}

const JsonValue *fem_job_summary(void) { return B.summary; }

static double record_value(const MaterialRecord *r, MatProperty p, const char *key, char *src, size_t scap, char *prov, size_t pcap) {
    src[0] = prov[0] = 0;
    double v = mat_eval(&r->prop[p], 293.15);
    if (!(v == v)) return 0;
    const JsonValue *o = json_get(r->json, key);
    str_copy(src, scap, json_get_str(o, "source", ""));
    str_copy(prov, pcap, json_get_str(o, "provenance", ""));
    return v;
}

bool fem_library_material(const char *id, FemMaterialInfo *out) {
    memset(out, 0, sizeof *out);
    MaterialRecord r;
    char err[200];
    if (!id || !id[0] || !material_lookup(NULL, id, &r, err, sizeof err)) return false;
    str_copy(out->id, sizeof out->id, r.id);
    str_copy(out->name, sizeof out->name, r.name);
    str_copy(out->status, sizeof out->status, r.status);
    char s[400], p[24];
    out->density_kg_m3 = record_value(&r, MATP_DENSITY, "density_kg_m3", s, sizeof s, p, sizeof p);
    out->poisson = record_value(&r, MATP_NU, "poisson_ratio", s, sizeof s, p, sizeof p);
    out->youngs_pa = record_value(&r, MATP_E, "youngs_modulus_pa", out->youngs_source, sizeof out->youngs_source,
                                  out->youngs_provenance, sizeof out->youngs_provenance);
    out->yield_pa = record_value(&r, MATP_YIELD, "yield_strength_pa", out->yield_source, sizeof out->yield_source,
                                 out->yield_provenance, sizeof out->yield_provenance);
    if (!out->yield_source[0]) out->yield_pa = 0; /* a yield value is used only with the line that says where it is from */
    out->hardening_pa = record_value(&r, MATP_HARDENING, "hardening_modulus_pa", out->hardening_source,
                                     sizeof out->hardening_source, out->hardening_provenance, sizeof out->hardening_provenance);
    return true;
}

/* the summary of the result on screen: the followed job's, or the one its run directory holds */
const JsonValue *fem_result_summary(void) {
    if (!B.st.result_job[0]) return NULL;
    if (B.summary && !strcmp(B.st.result_job, B.st.job_id)) return B.summary;
    if (B.disk_summary && !strcmp(B.st.result_job, B.result_body_job)) return B.disk_summary;
    return B.summary;
}

const char *fem_result_caveat(void) { return B.caveat[0] ? B.caveat : NULL; }

const char *fem_unsupported_reason(void) { return B.unsupported[0] ? B.unsupported : NULL; }

bool fem_body_source_path(char *out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (!app.engine || !out || !cap) return false;
    engine_lock(app.engine);
    Project *p = engine_project_locked(app.engine);
    Body *b = p ? project_body(p, NULL) : NULL;
    if (b) str_copy(out, cap, b->source_path);
    engine_unlock(app.engine);
    return out[0] != 0;
}

const JsonValue *fem_conditions(void) {
    if (!app.engine || !B.st.have_project) return NULL;
    if (B.conditions && B.conditions_rev == B.st.revision) return json_get(B.conditions, "boundary_conditions");
    json_free(B.conditions);
    B.conditions = NULL;
    B.conditions_rev = B.st.revision;
    OpResult r;
    OpCaller caller = {"ui", "navier-interface"};
    JsonValue *params = json_object();
    ops_invoke(app.engine, "boundary_list", params, &caller, &r);
    json_free(params);
    if (r.ok && r.value) B.conditions = json_clone(r.value);
    op_result_free(&r);
    return B.conditions ? json_get(B.conditions, "boundary_conditions") : NULL;
}

/* the material library as the operation material_list reports it (library and project records, what each lacks and
 * its first source line): the pickers read this, not a copy of their own. Asked again when the engine changes, so a
 * project's own materials appear as soon as they are defined. */
const JsonValue *fem_material_list(void) {
    if (!app.engine) return NULL;
    uint64_t ch = engine_change_counter(app.engine);
    if (B.material_list && B.material_list_changes == ch) return json_get(B.material_list, "materials");
    json_free(B.material_list);
    B.material_list = NULL;
    B.material_list_changes = ch;
    OpResult r;
    OpCaller caller = {"ui", "navier-interface"};
    JsonValue *params = json_object();
    ops_invoke(app.engine, "material_list", params, &caller, &r);
    json_free(params);
    if (r.ok && r.value) B.material_list = json_clone(r.value);
    op_result_free(&r);
    return B.material_list ? json_get(B.material_list, "materials") : NULL;
}

void fem_mark_dirty(void) { B.surface_dirty = true; }

void fem_set_visible(bool on) {
    B.visible = on;
    B.surface_dirty = true;
}

void fem_set_field(int f) {
    if (f < 0 || f >= FEM_FIELD_COUNT || f == B.field) return;
    B.field = f;
    B.surface_dirty = true;
}

bool fem_playing(void) { return B.playing; }
double fem_play_speed(void) { return B.play_speed; }

void fem_set_play_speed(double per_second) { B.play_speed = CLAMP(per_second, 0.25, 60.0); }

void fem_set_playing(bool on) {
    if (on == B.playing) return;
    if (on) {
        if (fem_step_count() < 2) {
            LOGW("this result has a single stored time: nothing to play");
            return;
        }
        B.range_all_before = B.range_all;
        if (!B.range_all) { /* one range for the whole run, or the part would look the same in every frame */
            B.range_all = true;
            B.surface_dirty = true;
        }
        B.play_pos = B.step;
        B.play_clock = app.time;
    } else if (B.range_all != B.range_all_before) {
        B.range_all = B.range_all_before;
        B.surface_dirty = true;
    }
    B.playing = on;
}

/* advances the shown stored time from the frame clock; it never sleeps and never waits for the worker */
static void advance_playback(void) {
    int n = fem_step_count();
    if (!B.playing) return;
    if (n < 2) {
        B.playing = false;
        return;
    }
    double dt = app.time - B.play_clock;
    B.play_clock = app.time;
    if (dt <= 0 || dt > 1.0) dt = dt > 1.0 ? 1.0 : 0.0; /* a long stall (a solve, a resize) must not jump the film */
    B.play_pos += dt * B.play_speed;
    while (B.play_pos >= n) B.play_pos -= n; /* loop */
    int i = (int)B.play_pos;
    if (i != B.step) {
        B.step = CLAMP(i, 0, n - 1);
        B.surface_dirty = true;
    }
}

void fem_set_step(int i) {
    int n = fem_step_count();
    i = CLAMP(i, 0, n - 1);
    if (i == B.step) return;
    B.step = i;
    B.play_pos = i;
    B.surface_dirty = true;
}

void fem_set_deform_scale(double s) {
    B.deform = s;
    B.surface_dirty = true;
}

void fem_follow_job(const char *id) {
    json_free(B.summary);
    B.summary = NULL;
    B.caveat[0] = 0;
    B.unsupported[0] = 0;
    B.unsupported_logged[0] = 0;
    B.st.job_adopted = false;
    B.st.pending_job[0] = B.st.pending_kind[0] = 0;
    str_copy(B.st.job_id, sizeof B.st.job_id, id ? id : "");
    B.st.job_state[0] = 0;
    B.st.job_stage[0] = 0;
    B.st.job_error[0] = 0;
    B.st.job_progress = 0;
    B.st.job_active = B.st.job_id[0] != 0;
    B.next_job_poll = 0;
}

static void *acquire_result(const char *id, char *kind, size_t kindcap);
static bool result_geometry(const void *data, const char *kind, const double **xyz, const int **conn, int *nelems);

int fem_growth_axis(void) { return B.fake_axis; }



bool fem_fake_growth(int axis) {
    free(B.fake_birth);
    B.fake_birth = NULL;
    B.fake_axis = -1;
    B.hover_patch = -1;
    B.vis_gen++;
    B.surface_dirty = true;
    if (axis < 0) return true;
    if (!app.engine || !B.st.result_job[0]) return false;
    char kind[32] = "";
    void *data = acquire_result(B.st.result_job, kind, sizeof kind);
    if (!data) return false;
    const double *xyz = NULL;
    const int *conn = NULL;
    int nelems = 0;
    bool ok = result_geometry(data, kind, &xyz, &conn, &nelems);
    int nsteps = MAXI(B.st.nsteps, 1);
    if (ok) {
        int *birth = malloc((size_t)nelems * sizeof *birth);
        double *c = malloc((size_t)nelems * sizeof *c);
        if (birth && c) {
            double lo = 1e300, hi = -1e300;
            for (int e = 0; e < nelems; e++) {
                double sum = 0;
                for (int k = 0; k < 8; k++) sum += xyz[3 * (size_t)conn[8 * (size_t)e + k] + axis];
                c[e] = sum / 8.0;
                lo = MINI(lo, c[e]), hi = MAXI(hi, c[e]);
            }
            double span = hi - lo > 1e-12 ? hi - lo : 1.0;
            for (int e = 0; e < nelems; e++) {
                int b = (int)floor((c[e] - lo) / span * (double)nsteps);
                birth[e] = CLAMP(b, 0, nsteps - 1);
            }
            B.fake_birth = birth, B.fake_nelems = nelems, B.fake_axis = axis;
            B.vis_gen++;
            str_copy(B.fake_job, sizeof B.fake_job, B.st.result_job);
            birth = NULL;
        }
        free(birth);
        free(c);
    }
    jobs_release(app.engine->jobs, B.st.result_job);
    return B.fake_birth != NULL;
}

void fem_show_job(const char *id) {
    fem_follow_job(id);
    str_copy(B.st.result_job, sizeof B.st.result_job, id ? id : "");
    B.step = 0;
    B.surface_dirty = true;
}

void fem_init(void) {
    memset(&B, 0, sizeof B);
    B.field = FEM_VON_MISES;
    B.range_all = true;
    B.play_speed = 4.0;
    B.fake_axis = -1;
    B.section_axis = 0;
    B.section_fraction = 0.5;
    B.groups[0] = B.groups[1] = B.groups[2] = true;
    B.deform = -1.0;
    B.deform_used = 1.0;
    B.visible = true;
    B.surface_view = true; /* the part, not the mesh: the mesh is one click away */
    B.preview = -1;
    B.hover_patch = -1; /* nothing is under the pointer until it moves: patch 0 used to glow at start */
    B.surface_dirty = true;
    B.st.have_engine = app.engine != NULL;
}

void fem_shutdown(void) {
    isomesh_free(&B.surf);
    free(B.adj);
    free(B.cent);
    free(B.outline);
    free(B.fake_death);
    free(B.fake_group);
    free(B.fake_birth);
    json_free(B.last_value);
    json_free(B.conditions);
    json_free(B.summary);
    json_free(B.disk_summary);
    json_free(B.material_list);
    B.last_value = NULL;
    B.conditions = NULL;
    B.summary = NULL;
    memset(&B, 0, sizeof B);
}

/* ---- operations ---------------------------------------------------------------------------------------------- */

static void log_error(const char *op, const JsonValue *err) {
    const char *code = json_get_str(err, "code", "ERROR");
    const char *msg = json_get_str(err, "message", "failed");
    const char *hint = json_get_str(err, "hint", NULL);
    LOGE("%s: %s (%s)", op, msg, code);
    if (hint) LOGI("hint: %s", hint);
    const JsonValue *det = json_get(err, "details");
    const JsonValue *errs = det ? json_get(det, "errors") : NULL;
    for (size_t i = 0; errs && i < json_len(errs) && i < 6; i++)
        LOGI("  %s", json_get_str(json_at(errs, i), "message", ""));
}

bool fem_op(const char *op, const char *params_fmt, ...) {
    if (!app.engine) {
        LOGE("the NAVIER-AM engine is not available");
        return false;
    }
    JsonValue *params = NULL;
    if (params_fmt) {
        char buf[8192]; /* a box selection can name hundreds of patches */
        va_list ap;
        va_start(ap, params_fmt);
        vsnprintf(buf, sizeof buf, params_fmt, ap);
        va_end(ap);
        JsonError jerr;
        params = json_parse(buf, strlen(buf), NULL, &jerr);
        if (!params) {
            LOGE("%s: bad parameters (%s)", op, jerr.message);
            return false;
        }
    } else {
        params = json_object();
    }
    OpResult r;
    OpCaller caller = {"ui", "navier-interface"};
    ops_invoke(app.engine, op, params, &caller, &r);
    json_free(params);
    json_free(B.last_value);
    B.last_value = r.ok && r.value ? json_clone(r.value) : NULL;
    if (!r.ok) {
        log_error(op, r.error);
        snprintf(B.last_error, sizeof B.last_error, "%s: %s", op, json_get_str(r.error, "message", "refused"));
    } else {
        B.last_error[0] = 0;
    }
    bool ok = r.ok;
    op_result_free(&r);
    B.force_refresh = true;
    B.surface_dirty = true;
    return ok;
}

/* ---- state mirror -------------------------------------------------------------------------------------------- */

static void refresh_project(void) {
    FemState *s = &B.st;
    Engine *e = app.engine;
    s->have_engine = e != NULL;
    if (!e) {
        s->have_project = false;
        return;
    }
    engine_lock(e);
    Project *p = engine_project_locked(e);
    s->have_project = p != NULL;
    if (!p) {
        engine_unlock(e);
        s->project[0] = 0;
        s->project_id[0] = 0;
        s->nbodies = 0;
        s->meshed = false;
        s->nbcs = s->nselections = 0;
        return;
    }
    if (strcmp(s->project_id, p->id) != 0) B.scan_newest[0] = s->pending_job[0] = s->pending_kind[0] = 0; /* a different project: look at its jobs again */
    str_copy(s->project_id, sizeof s->project_id, p->id);
    str_copy(s->project, sizeof s->project, p->name);
    str_copy(s->project_dir, sizeof s->project_dir, p->dir);
    s->revision = p->revision;
    s->nbodies = p->nbodies;
    s->nselections = p->nselections;
    s->nbcs = p->nbcs;
    s->nthermal_bcs = 0;
    for (int i = 0; i < p->nbcs; i++)
        if (bc_is_thermal(p->bcs[i].kind)) s->nthermal_bcs++;

    Body *b = project_body(p, NULL);
    if (b) {
        str_copy(s->body, sizeof s->body, b->name);
        s->triangles = b->file_mesh.tri_count;
        s->watertight = b->diag.watertight;
        /* the diagnostics are in the file's own unit; the panel speaks millimetres */
        double mm = b->unit_scale * 1e3;
        s->volume_mm3 = b->diag.volume * mm * mm * mm;
        s->area_mm2 = b->diag.area * mm * mm;
        s->min_thickness_mm = b->diag.min_thickness == b->diag.min_thickness ? b->diag.min_thickness * mm : 0;
        s->components = b->diag.components;
        s->open_edges = (int)b->diag.open_edges;
        s->nonmanifold_edges = (int)b->diag.nonmanifold_edges;
        s->nested_shells = b->diag.nested_shells;
        s->patches = b->patches.n;
        s->degenerate_removed = (int)b->diag.degenerate_removed;
        s->closed_solid = b->diag.closed_solid;
        for (int i = 0; i < 3; i++) {
            s->size_mm[i] = (b->bmax[i] - b->bmin[i]) * 1e3;
            B.centre_m[i] = 0.5 * (b->bmax[i] + b->bmin[i]);
        }
        B.have_place = true;
    } else {
        s->body[0] = 0;
        s->triangles = 0;
        s->watertight = false;
        s->volume_mm3 = s->area_mm2 = s->min_thickness_mm = 0;
        s->components = s->open_edges = s->nonmanifold_edges = s->nested_shells = s->patches = 0;
        s->degenerate_removed = 0;
        s->closed_solid = false;
        B.have_place = false;
    }
    s->material[0] = 0;
    for (int i = 0; i < p->nmaterials; i++)
        if (!b || !strcmp(p->materials[i].target, b->name)) {
            str_copy(s->material, sizeof s->material, p->materials[i].material);
            break;
        }
    s->meshed = p->mesh.valid;
    s->nelems = p->mesh.valid ? p->mesh.hm.nelems : 0;
    s->nnodes = p->mesh.valid ? p->mesh.hm.nnodes : 0;
    s->mesh_volume_mm3 = s->mesh_volume_error_pct = 0;
    if (p->mesh.valid) { /* what the staircase mesh costs against the STL it came from */
        double vm = 0, vs = 0;
        for (int i = 0; i < p->mesh.hm.nbodies && i < 16; i++) {
            vm += p->mesh.hm.body_volume_mesh[i];
            vs += p->mesh.hm.body_volume_stl[i];
        }
        s->mesh_volume_mm3 = vm * 1e9; /* m^3 -> mm^3 */
        if (vs > 0) s->mesh_volume_error_pct = (vm - vs) / vs * 100.0;
        s->uncertain_cells = p->mesh.hm.uncertain_cells;
        s->abstained_rays = p->mesh.hm.abstained_rays;
        s->mesh_method = p->mesh.hm.elem_type ? 1 : 0;
        s->tet_order = p->mesh.tet_order;
        s->tet_min_dihedral = p->mesh.tetq.min_dihedral, s->tet_max_dihedral = p->mesh.tetq.max_dihedral;
        s->tet_worst_aspect = p->mesh.tetq.worst_aspect;
        s->tet_regions = p->mesh.hm.regions;
        s->tet_unresolved_thin = p->mesh.tetq.unresolved_thin;
        s->tet_finest_mm = 1e3 * p->mesh.tetq.finest_cell;
    } else {
        s->uncertain_cells = s->abstained_rays = 0;
    }
    s->element_size_mm = (p->mesh.has_settings || p->mesh.valid) ? p->mesh.h[0] * 1e3 : 0;
    char why[128] = "";
    s->mesh_current = p->mesh.valid && project_mesh_current_any(p, why, sizeof why);
    engine_unlock(e);

    if (s->material[0]) {
        MaterialRecord rec;
        char merr[200];
        if (material_lookup(NULL, s->material, &rec, merr, sizeof merr)) {
            str_copy(s->material_name, sizeof s->material_name, rec.name);
            str_copy(s->material_status, sizeof s->material_status, rec.status);
            str_copy(s->material_source, sizeof s->material_source, json_get_str(rec.json, "provenance", ""));
            double y = mat_eval(&rec.prop[MATP_YIELD], 293.15);
            double d = mat_eval(&rec.prop[MATP_DENSITY], 293.15);
            double e = mat_eval(&rec.prop[MATP_E], 293.15);
            double nu = mat_eval(&rec.prop[MATP_NU], 293.15);
            s->yield_mpa = y == y ? y * 1e-6 : 0;      /* NAN when the record has no yield strength */
            s->density_kg_m3 = d == d ? d : 0;
            s->youngs_gpa = e == e ? e * 1e-9 : 0;
            s->poisson = nu == nu ? nu : 0;
        } else {
            str_copy(s->material_name, sizeof s->material_name, s->material);
            s->material_status[0] = s->material_source[0] = 0;
            s->yield_mpa = s->density_kg_m3 = s->youngs_gpa = s->poisson = 0;
        }
    } else {
        s->material_name[0] = 0;
        s->material_status[0] = s->material_source[0] = 0;
        s->yield_mpa = s->density_kg_m3 = s->youngs_gpa = s->poisson = 0;
    }
}

/* job_status takes no engine lock, so this is cheap enough to call while a solve runs */
static void refresh_job(void) {
    FemState *s = &B.st;
    if (!app.engine || !s->job_id[0]) return;
    OpResult r;
    OpCaller caller = {"ui", "navier-interface"};
    JsonValue *params = json_object();
    json_set_string(params, "job_id", s->job_id);
    ops_invoke(app.engine, "job_status", params, &caller, &r);
    json_free(params);
    if (r.ok && r.value) {
        str_copy(s->job_state, sizeof s->job_state, json_get_str(r.value, "state", ""));
        str_copy(s->job_kind, sizeof s->job_kind, json_get_str(r.value, "kind", s->job_kind));
        /* job_status reports progress as a number and stage as a sibling string (jobs.c job_json_locked) */
        s->job_progress = json_get_num(r.value, "progress", s->job_progress);
        str_copy(s->job_stage, sizeof s->job_stage, json_get_str(r.value, "stage", s->job_stage));
        bool done = !strcmp(s->job_state, "succeeded");
        if (done) s->job_progress = 1.0;
        if (done && !B.summary) {
            const JsonValue *sum = json_get(r.value, "summary");
            B.summary = sum ? json_clone(sum) : NULL;
            /* a peak at an ideal support or a sharp re-entrant corner is a singularity of the model, not a strength
             * figure: the panel must say so next to the number it shows */
            B.caveat[0] = 0;
            const JsonValue *w = B.summary ? json_get(B.summary, "result_warnings") : NULL;
            for (size_t i = 0; w && i < json_len(w) && !B.caveat[0]; i++) {
                const char *code = json_get_str(json_at(w, i), "code", "");
                if (!strcmp(code, "PEAK_STRESS_AT_SUPPORT"))
                    str_copy(B.caveat, sizeof B.caveat, "peak at a support: a singularity, not a design value");
                else if (!strcmp(code, "PEAK_AT_REENTRANT_CORNER"))
                    str_copy(B.caveat, sizeof B.caveat, "peak at a sharp inside corner: a singularity, not a design value");
            }
        }
        bool over = done || !strcmp(s->job_state, "failed") || !strcmp(s->job_state, "cancelled");
        if (!done) {
            const JsonValue *err = json_get(r.value, "error");
            if (err) str_copy(s->job_error, sizeof s->job_error, json_get_str(err, "message", ""));
        }
        if (over && s->job_active) {
            s->job_active = false;
            if (done) {
                LOGOK("%s finished (%s)", s->job_kind[0] ? s->job_kind : "analysis", s->job_id);
                str_copy(s->result_job, sizeof s->result_job, s->job_id);
                B.step = 0;
                B.surface_dirty = true;
            } else if (!strcmp(s->job_state, "cancelled")) {
                /* the person asked for it: said plainly, not as a failure */
                LOGI("stopped %s (%s). Nothing else is running because of it: start the next one whenever you like", s->job_id,
                     s->job_error[0] ? s->job_error : "on request");
            } else if (s->job_error[0]) {
                LOGE("%s %s: %s", s->job_id, s->job_state, s->job_error);
            }
        } else if (!over) {
            s->job_active = true;
        }
    }
    op_result_free(&r);
}

/* ---- everything queued or running ---------------------------------------------------------------------------- */

static void refresh_active_jobs(void) {
    B.nactive = 0;
    if (!app.engine) return;
    OpResult r;
    OpCaller caller = {"ui", "navier-interface"};
    JsonValue *params = json_object();
    json_set_int(params, "limit", 32);
    ops_invoke(app.engine, "job_list", params, &caller, &r);
    json_free(params);
    const JsonValue *jobs = r.ok && r.value ? json_get(r.value, "jobs") : NULL;
    size_t n = jobs ? json_len(jobs) : 0;
    /* the list is newest first; the queue runs oldest first. Two passes: the running job, then the queue in its order. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t k = 0; k < n && B.nactive < (int)(sizeof B.active / sizeof B.active[0]); k++) {
            const JsonValue *j = json_at(jobs, n - 1 - k);
            const char *st = json_get_str(j, "state", "");
            bool running = !strcmp(st, "running") || !strcmp(st, "paused");
            if (pass == 0 ? !running : strcmp(st, "queued") != 0) continue;
            FemJobInfo *a = &B.active[B.nactive++];
            memset(a, 0, sizeof *a);
            str_copy(a->id, sizeof a->id, json_get_str(j, "job_id", ""));
            str_copy(a->kind, sizeof a->kind, json_get_str(j, "kind", ""));
            str_copy(a->label, sizeof a->label, json_get_str(j, "label", ""));
            str_copy(a->state, sizeof a->state, st);
            str_copy(a->stage, sizeof a->stage, json_get_str(j, "stage", ""));
            a->progress = json_get_num(j, "progress", 0);
            a->stopping = !strcmp(a->stage, "cancelling");
        }
    }
    op_result_free(&r);
}

int fem_active_jobs(const FemJobInfo **out) {
    if (out) *out = B.active;
    return B.nactive;
}

const char *fem_job_words(const FemJobInfo *j) {
    if (!j) return "";
    if (j->label[0]) return j->label;
    if (!strcmp(j->kind, "lpbf_build")) return "metal print simulation";
    if (!strcmp(j->kind, "fff_print")) return "plastic print simulation";
    if (!strcmp(j->kind, "static_structural")) return "stress analysis";
    if (!strcmp(j->kind, "topology_optimization")) return "shape optimisation";
    if (!strncmp(j->kind, "thermal", 7)) return "heat analysis";
    return j->kind[0] ? j->kind : "analysis";
}

bool fem_stop_job(const char *id) {
    if (!id || !id[0]) return false;
    bool ok = fem_op("job_cancel", "{\"job_id\": \"%s\"}", id);
    for (int i = 0; i < B.nactive; i++)
        if (!strcmp(B.active[i].id, id)) B.active[i].stopping = true;
    B.next_job_scan = 0; /* look again at once: a queued job is gone immediately */
    B.next_job_poll = 0;
    return ok;
}

int fem_stop_all_jobs(void) {
    refresh_active_jobs();
    int n = 0;
    /* the queue first, newest first, so nothing starts in the gap when the running job ends */
    for (int i = B.nactive - 1; i >= 0; i--) {
        if (B.active[i].stopping) continue; /* already asked: it ends with its layer, asking again is noise */
        char id[64];
        str_copy(id, sizeof id, B.active[i].id);
        if (fem_stop_job(id)) n++;
    }
    return n;
}

bool fem_job_stopping(void) {
    if (!B.st.job_active) return false;
    if (!strcmp(B.st.job_stage, "cancelling")) return true;
    for (int i = 0; i < B.nactive; i++)
        if (!strcmp(B.active[i].id, B.st.job_id)) return B.active[i].stopping;
    return false;
}

/* ---- jobs started by other clients ---------------------------------------------------------------------------- */

/* An agent drives the same engine through MCP or the control socket, and its jobs land in this process' job manager
 * (the app must be started with --listen for the socket). The window looks for the newest job of the open project and
 * follows it by itself, so an analysis an agent starts appears here without anyone clicking. */
static bool job_of_open_project(const char *id) {
    OpResult r;
    OpCaller caller = {"ui", "navier-interface"};
    JsonValue *p = json_object();
    json_set_string(p, "job_id", id);
    ops_invoke(app.engine, "job_status", p, &caller, &r);
    json_free(p);
    bool mine = false;
    if (r.ok && r.value) {
        const JsonValue *pr = json_get(r.value, "project");
        const char *pid = pr ? json_get_str(pr, "id", "") : "";
        mine = pid[0] && !strcmp(pid, B.st.project_id);
    }
    op_result_free(&r);
    return mine;
}

static void scan_for_new_jobs(void) {
    if (!app.engine || !B.st.have_project || !B.st.project_id[0]) return;
    OpResult r;
    OpCaller caller = {"ui", "navier-interface"};
    JsonValue *params = json_object();
    json_set_int(params, "limit", 8);
    ops_invoke(app.engine, "job_list", params, &caller, &r);
    json_free(params);
    const JsonValue *jobs = r.ok && r.value ? json_get(r.value, "jobs") : NULL;
    char newest[64] = "", adopt[64] = "", adopt_kind[32] = "";
    for (size_t i = 0; jobs && i < json_len(jobs); i++) { /* the list is newest first */
        const JsonValue *j = json_at(jobs, i);
        const char *id = json_get_str(j, "job_id", "");
        if (!id[0]) continue;
        if (!newest[0]) str_copy(newest, sizeof newest, id);
        if (B.scan_newest[0] && !strcmp(id, B.scan_newest)) break; /* everything from here was examined before */
        if (!strcmp(id, B.st.job_id)) continue;                    /* already following it */
        if (!strcmp(json_get_str(j, "kind", ""), "topology_optimization")) continue; /* the OPTIMISE panel follows it */
        if (!adopt[0] && job_of_open_project(id)) {
            str_copy(adopt, sizeof adopt, id);
            str_copy(adopt_kind, sizeof adopt_kind, json_get_str(j, "kind", ""));
        }
    }
    op_result_free(&r);
    if (newest[0]) str_copy(B.scan_newest, sizeof B.scan_newest, newest);
    if (!adopt[0]) return;
    if (!app.ctl) {
        str_copy(B.st.pending_job, sizeof B.st.pending_job, adopt);
        str_copy(B.st.pending_kind, sizeof B.st.pending_kind, adopt_kind);
        LOGI("agent started %s %s - FOLLOW", adopt_kind, adopt);
        return;
    }
    fem_follow_job(adopt);
    B.st.job_adopted = true;
    str_copy(B.st.job_kind, sizeof B.st.job_kind, adopt_kind);
    LOGOK("following %s job %s started outside the interface", adopt_kind[0] ? adopt_kind : "analysis", adopt);
    refresh_job();
}

/* ---- the result surface -------------------------------------------------------------------------------------- */

/* Result kinds this window can draw. static_structural is a StaticResults file; the three transient kinds are all
 * ThermalCase files (conjugate_heat_transfer included - ops_transient.c attaches the same structure). Anything else
 * (comparison_study today) is reported in the panel instead of being loaded as something it is not. */
typedef enum { RK_STATIC = 0, RK_TRANSIENT, RK_UNSUPPORTED } ResultKind;

static ResultKind result_kind_of(const char *kind) {
    if (!strcmp(kind, "static_structural")) return RK_STATIC;
    if (!strcmp(kind, "transient_thermal") || !strcmp(kind, "thermomechanical") ||
        !strcmp(kind, "conjugate_heat_transfer") || !strcmp(kind, "fff_print") || !strcmp(kind, "lpbf_build"))
        return RK_TRANSIENT;
    return RK_UNSUPPORTED;
}

static void cannot_display(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void cannot_display(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(B.unsupported, sizeof B.unsupported, fmt, ap);
    va_end(ap);
    if (strcmp(B.unsupported, B.unsupported_logged) == 0) return; /* the same reason is not repeated every retry */
    str_copy(B.unsupported_logged, sizeof B.unsupported_logged, B.unsupported);
    LOGW("%s", B.unsupported);
}

static void unsupported_kind(const char *id, const char *kind) {
    cannot_display("%s results are not drawn in this window; read them with the operations (job %s)",
                   kind[0] ? kind : "these", id);
}

static bool file_exists(const char *dir, const char *name, char *out, size_t cap) {
    path_join(out, cap, dir, name);
    FILE *f = fopen(out, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* pins a finished job's results, loading them from the run directory when they are no longer in memory */
static void *acquire_result(const char *id, char *kind, size_t kindcap) {
    Engine *e = app.engine;
    JobState st = JOB_QUEUED;
    void *data = NULL;
    char dir[NV_PATH_MAX] = "";
    kind[0] = 0;
    bool known = jobs_acquire(e->jobs, id, &st, &data, kind, kindcap, dir, sizeof dir);
    if (kind[0] && result_kind_of(kind) == RK_UNSUPPORTED) {
        if (data) jobs_release(e->jobs, id); /* never hand an unknown structure to the surface builder */
        unsupported_kind(id, kind);
        return NULL;
    }
    if (data) return data;
    if (known && st != JOB_SUCCEEDED) return NULL;
    if (!known && !op_project_run_dir(e, id, dir, sizeof dir)) {
        cannot_display("no job '%s' in this session and no run directory for it in the open project", id);
        return NULL;
    }
    char path[NV_PATH_MAX], err[256];
    ResultKind rk;
    if (kind[0]) {
        rk = result_kind_of(kind);
    } else { /* a job from an earlier session: the run directory's own files decide, nothing is guessed */
        /* spec.json names the analysis (lpbf_build, fff_print, thermomechanical, ...); the result file only says
         * whether it is static or time-indexed, and a build is a time-indexed mechanical result like any other */
        char spec[NV_PATH_MAX];
        path_join(spec, sizeof spec, dir, "spec.json");
        JsonError je;
        JsonValue *sj = json_read_file(spec, 4u << 20, &je);
        if (sj) {
            str_copy(kind, kindcap, json_get_str(sj, "analysis", ""));
            str_copy(B.result_body, sizeof B.result_body, json_get_str(json_get(sj, "model"), "body", ""));
            str_copy(B.result_body_job, sizeof B.result_body_job, id);
            json_free(sj);
            /* what the run said about itself (supports, plasticity), for the panel */
            char sp[NV_PATH_MAX];
            path_join(sp, sizeof sp, dir, "summary.json");
            json_free(B.disk_summary);
            B.disk_summary = json_read_file(sp, 16u << 20, &je);
        }
        if (kind[0] && result_kind_of(kind) == RK_UNSUPPORTED) {
            unsupported_kind(id, kind);
            return NULL;
        }
        if (kind[0]) rk = result_kind_of(kind);
        else if (file_exists(dir, "results.nvr", path, sizeof path)) rk = RK_STATIC;
        else if (file_exists(dir, "results.nvt", path, sizeof path)) rk = RK_TRANSIENT;
        else {
            cannot_display("no result file this window can read in %s (job %s)", dir, id);
            return NULL;
        }
    }
    if (rk == RK_TRANSIENT) {
        path_join(path, sizeof path, dir, "results.nvt");
        ThermalCase *c = thermal_results_load(path, err, sizeof err);
        if (!c) return NULL;
        snprintf(c->run_dir, sizeof c->run_dir, "%s", dir);
        if (!c->job_id[0]) snprintf(c->job_id, sizeof c->job_id, "%s", id);
        const char *k = kind[0] ? kind : (c->settings.mechanical ? "thermomechanical" : "transient_thermal");
        str_copy(kind, kindcap, k);
        return jobs_attach(e->jobs, id, k, dir, c, thermal_case_free, c->summary ? json_clone(c->summary) : NULL);
    }
    path_join(path, sizeof path, dir, "results.nvr");
    StaticResults *r = static_results_load(path, err, sizeof err);
    if (!r) return NULL;
    snprintf(r->run_dir, sizeof r->run_dir, "%s", dir);
    if (!r->job_id[0]) snprintf(r->job_id, sizeof r->job_id, "%s", id);
    str_copy(kind, kindcap, "static_structural");
    return jobs_attach(e->jobs, id, "static_structural", dir, r, static_results_free, r->summary ? json_clone(r->summary) : NULL);
}

typedef struct Visibility {
    const int *birth, *death;
    const unsigned char *group;
    int nelems;
} Visibility;

typedef struct SurfSrc {
    const double *xyz;
    const int *conn;
    const int *face_elem;
    const unsigned char *face_local;
    int nfaces, nnodes, nelems;
    const double *u;     /* 3*nnodes displacement (m), NULL when there is none */
    double *value;       /* nnodes in display units (owned by the caller) */
    bool have_value;
    const int *adj;      /* 6*nelems neighbours, from build_adjacency */
    Visibility vis;      /* which elements exist at this stored time */
    int step;            /* the stored time being drawn */
    int elem_type;       /* 0 hexahedra; SOLID_ELEM_TET4 / TET10: a conforming tetrahedral mesh, drawn on its own faces */
    const signed char *elem_body; /* body per element, when the analysis has more than one */
    int nbodies;
    const char (*body_name)[64];
} SurfSrc;

/* element topology: nodes per element, faces per element, and the nodes of a local face (4 corners of a hexahedral
 * face; 3 corners, then 3 mid-edge nodes or -1, of a tetrahedral one). Returns the number of corners. */
static int et_npe(int t) { return t == SOLID_ELEM_TET10 ? 10 : (t ? 4 : 8); }
static int et_nfe(int t) { return t ? 4 : 6; }
static int et_face(int t, const int *conn, int e, int f, int out[6]) {
    int npe = et_npe(t);
    if (!t) {
        for (int k = 0; k < 4; k++) out[k] = conn[8 * (size_t)e + HEX8_FACE_NODES[f][k]];
        return 4;
    }
    for (int k = 0; k < 3; k++) out[k] = conn[(size_t)npe * e + TET_FACE[f][k]];
    for (int k = 0; k < 3; k++) out[3 + k] = t == SOLID_ELEM_TET10 ? conn[(size_t)npe * e + TET10_FACE_MID[f][k]] : -1;
    return 3;
}
/* the triangles that draw a tetrahedral face: one, or four through the mid-edge nodes (a curved TET10 face) */
static int et_face_tris(const int n[6], int tri[4][3]) {
    if (n[3] < 0) {
        tri[0][0] = n[0], tri[0][1] = n[1], tri[0][2] = n[2];
        return 1;
    }
    int t[4][3] = {{n[0], n[3], n[5]}, {n[3], n[1], n[4]}, {n[5], n[4], n[2]}, {n[3], n[4], n[5]}};
    memcpy(tri, t, sizeof t);
    return 4;
}

static bool field_values(const void *data, const char *kind, int field, int step, SurfSrc *s, double *time_s,
                         int *nsteps, bool *has_mech) {
    *time_s = 0;
    *nsteps = 1;
    *has_mech = false;
    if (result_kind_of(kind) == RK_STATIC) {
        const StaticResults *r = data;
        s->xyz = r->model.xyz, s->conn = r->model.conn;
        s->face_elem = r->model.face_elem, s->face_local = r->model.face_local;
        s->nfaces = r->model.nfaces, s->nnodes = r->model.nnodes, s->nelems = r->model.nelems;
        s->u = r->sol.u;
        s->elem_type = r->model.elem_type;
        s->elem_body = r->model.elem_body, s->nbodies = r->model.nbodies, s->body_name = r->model.body_name;
        *has_mech = true;
        if (field == FEM_TEMPERATURE) return false;
        s->value = malloc((size_t)s->nnodes * sizeof *s->value);
        if (!s->value) return false;
        for (int n = 0; n < s->nnodes; n++) {
            if (field == FEM_VON_MISES) {
                s->value[n] = r->node_vm ? r->node_vm[n] * 1e-6 : 0.0;
            } else {
                const double *u = &r->sol.u[3 * (size_t)n];
                s->value[n] = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) * 1e3;
            }
        }
        return true;
    }
    const ThermalCase *c = data;
    s->elem_type = 0;
    s->xyz = c->xyz, s->conn = c->conn;
    s->face_elem = c->face_elem, s->face_local = c->face_local;
    s->nfaces = c->nfaces, s->nnodes = c->nnodes, s->nelems = c->nelems;
    *nsteps = MAXI(c->noutputs, 1);
    *has_mech = c->mech_u || c->mech_vm;
    step = CLAMP(step, 0, *nsteps - 1);
    *time_s = c->times && c->noutputs > 0 ? c->times[step] : 0;
    size_t off = (size_t)step * (size_t)c->nnodes;
    s->u = c->mech_u ? c->mech_u + 3 * off : NULL;
    if ((field == FEM_TEMPERATURE && !c->T) || (field == FEM_VON_MISES && !c->mech_vm) ||
        (field == FEM_DISPLACEMENT && !c->mech_u)) return false;
    s->value = malloc((size_t)s->nnodes * sizeof *s->value);
    if (!s->value) return false;
    for (int n = 0; n < s->nnodes; n++) {
        if (field == FEM_TEMPERATURE) {
            s->value[n] = c->T[off + (size_t)n] - 273.15;
        } else if (field == FEM_VON_MISES) {
            s->value[n] = c->mech_vm[off + (size_t)n] * 1e-6;
        } else {
            const double *u = &c->mech_u[3 * (off + (size_t)n)];
            s->value[n] = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) * 1e3;
        }
    }
    return true;
}

static bool reserve_surface(IsoMesh *m, uint32_t nverts, uint32_t nindices) {
    if (nverts > m->cap_verts) {
        float *v = realloc(m->verts, (size_t)nverts * 7 * sizeof *v);
        if (!v) return false;
        m->verts = v, m->cap_verts = nverts;
    }
    if (nindices > m->cap_indices) {
        uint32_t *i = realloc(m->indices, (size_t)nindices * sizeof *i);
        if (!i) return false;
        m->indices = i, m->cap_indices = nindices;
    }
    m->nverts = 0, m->nindices = 0;
    return true;
}

/* One quad per boundary face with its own normal: a voxel mesh is faceted, and smoothing it would suggest a
 * smoothness the discretisation does not have. */
/* mesh geometry of a result, without the field arrays */
static bool result_geometry(const void *data, const char *kind, const double **xyz, const int **conn, int *nelems) {
    if (result_kind_of(kind) == RK_STATIC) {
        const StaticResults *r = data;
        *xyz = r->model.xyz, *conn = r->model.conn, *nelems = r->model.nelems;
    } else if (result_kind_of(kind) == RK_TRANSIENT) {
        const ThermalCase *c = data;
        *xyz = c->xyz, *conn = c->conn, *nelems = c->nelems;
    } else {
        return false;
    }
    return *xyz && *conn && *nelems > 0;
}

/* ---- which elements exist at the shown stored time ------------------------------------------------------------ */

/* A printed part grows: at an early stored time only the elements deposited so far exist. The drawn surface is
 * therefore built from element visibility and face adjacency instead of from the mesher's boundary faces, so the
 * same code shows a finished part and a part half-printed.
 *
 * This is the only place that reads a result's per-element life and grouping. Each array may be absent:
 *   birth[e] <= t                  the element has been deposited by stored time t
 *   death[e] < 0 or t < death[e]   it has not been cut away or removed yet
 *   group[e]                       0 part, 1 support, 2 base plate
 * The FFF print writes elem_birth; the LPBF build writes all three (results.nvt format 4). */
static void result_visibility(const void *data, const char *kind, int nelems, Visibility *v) {
    memset(v, 0, sizeof *v);
    v->nelems = nelems;
    if (result_kind_of(kind) == RK_TRANSIENT) { /* a print or a build says when each element exists */
        const ThermalCase *c = data;
        if (c->elem_birth || c->elem_death || c->elem_group) {
            v->birth = c->elem_birth;
            v->death = c->elem_death;
            v->group = c->elem_group;
            return; /* the result's own fields; the fakes below stand in only where it carries none */
        }
    }
    if (B.fake_nelems == nelems && !strcmp(B.fake_job, B.st.result_job)) { /* 'fem grow', 'fem cut' for a plain result */
        v->birth = B.fake_birth;
        v->death = B.fake_death;
        v->group = B.fake_group;
    }
}

static bool element_alive(const Visibility *v, int e, int step) {
    if (v->birth && (v->birth[e] < 0 || v->birth[e] > step)) return false; /* -1: never deposited at all */
    if (v->death && v->death[e] >= 0 && step >= v->death[e]) return false;
    return true;
}

/* ---- the result, told in plain words ------------------------------------------------------------------------- */

/* where a point sits in the part, as a person would say it */
static void place_words(const double p[3], const double lo[3], const double hi[3], char *out, size_t cap) {
    double f[3];
    for (int i = 0; i < 3; i++) f[i] = hi[i] > lo[i] ? (p[i] - lo[i]) / (hi[i] - lo[i]) : 0.5;
    const char *height = f[2] < 0.2 ? "near the plate" : f[2] > 0.8 ? "at the top" : "half way up";
    bool end = f[0] < 0.1 || f[0] > 0.9 || f[1] < 0.1 || f[1] > 0.9;
    snprintf(out, cap, "%s%s", height, end ? ", at an outer edge" : "");
}

/* The three statements the results screen makes, computed from the result itself: the largest warp and where and
 * which way, the largest residual stress and where, and whether the part rose above its layer during the build (the
 * recoater would hit it). Everything is read at the stored times the solver wrote; nothing is estimated. */
bool fem_build_story(FemStory *st) {
    memset(st, 0, sizeof *st);
    if (!app.engine || !B.st.result_job[0]) return false;
    char kind[32] = "";
    void *data = acquire_result(B.st.result_job, kind, sizeof kind);
    if (!data) return false;
    bool ok = false;
    if (result_kind_of(kind) == RK_TRANSIENT) {
        const ThermalCase *c = data;
        const double *xyz = NULL;
        const int *conn = NULL;
        int nelems = 0;
        if (c->mech_u && c->mech_vm && c->noutputs > 0 && result_geometry(data, kind, &xyz, &conn, &nelems)) {
            Visibility v;
            result_visibility(data, kind, nelems, &v);
            int nn = c->nnodes, last = c->noutputs - 1;
            char *alive = calloc((size_t)nn, 1);
            if (alive) {
                double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
                for (int e = 0; e < nelems; e++) {
                    if (!element_alive(&v, e, last)) continue;
                    if (v.group && (v.group[e] == 2 || v.group[e] == 1)) continue; /* the plate and the supports are not the part */
                    for (int k = 0; k < 8; k++) {
                        int n = conn[8 * (size_t)e + k];
                        alive[n] = 1;
                        for (int i = 0; i < 3; i++) {
                            double x = xyz[3 * (size_t)n + i];
                            lo[i] = MINI(lo[i], x), hi[i] = MAXI(hi[i], x);
                        }
                    }
                }
                int wn = -1, sn = -1;
                double wmax = -1, smax = -1;
                for (int n = 0; n < nn; n++) {
                    if (!alive[n]) continue;
                    const double *u = &c->mech_u[3 * ((size_t)last * nn + n)];
                    double m = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                    if (m > wmax) wmax = m, wn = n;
                    double vm = c->mech_vm[(size_t)last * nn + n];
                    if (vm > smax) smax = vm, sn = n;
                }
                /* rising during the build: the largest upward movement of anything already printed, at any stored
                 * time before the last, against the layer the recoater lays next */
                double rise = 0;
                int rise_t = -1;
                for (int t = 0; t < last; t++)
                    for (int e = 0; e < nelems; e++) {
                        if (!element_alive(&v, e, t)) continue;
                        for (int k = 0; k < 8; k++) {
                            int n = conn[8 * (size_t)e + k];
                            double uz = c->mech_u[3 * ((size_t)t * nn + n) + 2];
                            if (uz > rise) rise = uz, rise_t = t;
                        }
                    }
                free(alive);
                if (wn >= 0 && sn >= 0) {
                    const double *u = &c->mech_u[3 * ((size_t)last * nn + wn)];
                    st->warp_mm = wmax * 1e3;
                    int ax = fabs(u[2]) >= fabs(u[0]) && fabs(u[2]) >= fabs(u[1]) ? 2 : (fabs(u[0]) >= fabs(u[1]) ? 0 : 1);
                    str_copy(st->warp_direction, sizeof st->warp_direction,
                             ax == 2 ? (u[2] > 0 ? "upwards" : "downwards") : "sideways");
                    place_words(&xyz[3 * (size_t)wn], lo, hi, st->warp_where, sizeof st->warp_where);
                    st->stress_mpa = smax * 1e-6;
                    place_words(&xyz[3 * (size_t)sn], lo, hi, st->stress_where, sizeof st->stress_where);
                    st->rise_mm = rise * 1e3;
                    st->rise_stored_time = rise_t;
                    st->part_height_mm = (hi[2] - lo[2]) * 1e3;
                    ok = true;
                    /* the supports: counted from the element groups, their volume from the element size */
                    if (v.group)
                        for (int e = 0; e < nelems; e++) st->support_elements += v.group[e] == 1;
                    const JsonValue *sum = fem_result_summary();
                    const JsonValue *hv = json_get(json_get(sum, "model"), "element_size_mm");
                    double h[3];
                    if (st->support_elements && hv && json_get_numbers(hv, h, 3)) st->support_mm3 = st->support_elements * h[0] * h[1] * h[2];
                    const JsonValue *res = json_get(sum, "results");
                    st->supports_removed = json_get_bool(json_get(res, "supports"), "removed", false);
                    const JsonValue *pl = json_get(res, "plasticity");
                    if (pl) {
                        st->plastic = true;
                        st->yield_mpa = json_get_num(pl, "yield_strength_mpa", 0);
                        st->peak_plastic = json_get_num(pl, "peak_equivalent_plastic_strain", 0);
                        st->yielded = json_get_bool(pl, "yielded", st->peak_plastic > 0);
                    }
                }
            }
        }
    }
    jobs_release(app.engine->jobs, B.st.result_job);
    return ok;
}

/* Which piece an element or an STL triangle belongs to, and which piece is being emitted. The pieces are the
 * connected components of what is drawn: the face-connected regions of the mesh in the mesh views, the shells of the
 * STL in the surface view. -1 in draw_piece means "everything", which is what one piece needs. */
static bool in_piece(int piece) { return B.draw_piece < 0 || piece == B.draw_piece; }

static bool elem_in_piece(int e) { return B.draw_piece < 0 || !B.elem_piece || B.elem_piece[e] == B.draw_piece; }

/* One test for an element and for the element across a face: a face is drawn when the first is shown and the second
 * is not, which is what opens a surface where a part has been cut. */
static bool topo_element_kept(const SurfSrc *s, int e);

static bool element_shown(const SurfSrc *s, int e) {
    if (!element_alive(&s->vis, e, s->step)) return false;
    if (!topo_element_kept(s, e)) return false;
    unsigned g = s->vis.group ? s->vis.group[e] : 0;
    if (g < 3 && !B.groups[g]) return false;
    if (B.section_on && B.cent) {
        int a = B.section_axis;
        double cut = B.section_lo[a] + B.section_fraction * (B.section_hi[a] - B.section_lo[a]);
        double x = B.cent[3 * (size_t)e + a];
        if (B.section_flip ? x < cut : x > cut) return false;
    }
    return true;
}

/* Is the face between element e and the element nb across it a surface of what is drawn? It is when nothing is
 * there, when the neighbour is hidden (a section, a cut, a density below the threshold), and, while the pieces
 * stand apart, when the neighbour belongs to another piece: each piece is then a body of its own on screen and has
 * to be closed, the faces that were interior to the joined mesh included. Joined, the two sides of an interface are
 * coincident, so neither is drawn and nothing z-fights. The estimate of how many faces will be drawn asks the same
 * question, so the reserved buffer always holds them all. */
static int cmp_float_asc(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static bool face_is_surface(const SurfSrc *s, int e, int nb) {
    if (nb < 0 || !element_shown(s, nb)) return true;
    return B.explode > 0 && B.elem_piece && B.elem_piece[nb] != B.elem_piece[e];
}

/* A stable range across playback, restricted to the selected groups and section. Plate is neutral, not a field. */
/* drawn grey rather than coloured by the field: the plate always; in Simple mode also the supports, so the colours
 * belong to the part alone */
static bool neutral_group(const SurfSrc *s, int e) {
    if (!s->vis.group) return false;
    unsigned g = s->vis.group[e];
    return g == 2 || (g == 1 && B.supports_neutral);
}

void fem_set_supports_neutral(bool on) {
    if (B.supports_neutral == on) return;
    B.supports_neutral = on;
    B.surface_dirty = true;
}

static bool transient_range(const ThermalCase *c, SurfSrc *s, int field, double *lo, double *hi) {
    *lo = 1e300; *hi = -1e300;
    int saved = s->step;
    for (int t = 0; t < c->noutputs; t++) {
        s->step = t;
        for (int e = 0; e < s->nelems; e++) {
            if (!element_shown(s, e) || neutral_group(s, e)) continue;
            for (int k = 0; k < 8; k++) {
                size_t n = (size_t)t * c->nnodes + s->conn[8 * (size_t)e + k];
                double v = 0;
                if (field == FEM_TEMPERATURE && c->T) v = c->T[n] - 273.15;
                else if (field == FEM_VON_MISES && c->mech_vm) v = c->mech_vm[n] * 1e-6;
                else if (field == FEM_DISPLACEMENT && c->mech_u) {
                    const double *u = c->mech_u + 3*n;
                    v = sqrt(u[0]*u[0]+u[1]*u[1]+u[2]*u[2])*1e3;
                } else continue;
                *lo = MINI(*lo, v); *hi = MAXI(*hi, v);
            }
        }
    }
    s->step = saved;
    return *hi >= *lo;
}

/* the four nodes of a local face, sorted, so the same face of two elements produces the same key */
static void face_nodes_sorted(const int *conn, int e, int f, int out[4]) {
    if (B.adj_etype) { /* a tetrahedral face: three corners, the fourth slot a sentinel above every node id */
        int n6[6];
        et_face(B.adj_etype, conn, e, f, n6);
        out[0] = n6[0], out[1] = n6[1], out[2] = n6[2], out[3] = 0x7fffffff;
        for (int i = 1; i < 3; i++)
            for (int j = i; j > 0 && out[j] < out[j - 1]; j--) {
                int t = out[j];
                out[j] = out[j - 1], out[j - 1] = t;
            }
        return;
    }
    const int *en = &conn[8 * (size_t)e];
    const int *fn = HEX8_FACE_NODES[f];
    for (int k = 0; k < 4; k++) out[k] = en[fn[k]];
    for (int i = 1; i < 4; i++) /* four elements: insertion sort is the cheapest thing that is also clear */
        for (int j = i; j > 0 && out[j] < out[j - 1]; j--) {
            int t = out[j];
            out[j] = out[j - 1], out[j - 1] = t;
        }
}

/* neighbour element across every local face, from conn alone (the mesher's own face list only covers the boundary
 * of the whole mesh, which is not the boundary of a part that is still growing) */
static bool build_adjacency(const int *conn, int nelems, const char *job, int etype) {
    if (B.adj && B.adj_nelems == nelems && !strcmp(B.adj_job, job) && B.adj_etype == etype) return true;
    B.adj_etype = etype; /* the table keeps six slots per element; a tetrahedron uses the first four */
    free(B.adj);
    B.adj = NULL;
    B.adj_nelems = 0;
    B.adj_job[0] = 0;
    if (nelems <= 0) return false;
    size_t nfaces = (size_t)nelems * 6;
    int *adj = malloc(nfaces * sizeof *adj);
    size_t cap = 1;
    while (cap < nfaces * 2) cap <<= 1;
    uint64_t *key = calloc(cap, sizeof *key);
    int *val = malloc(cap * sizeof *val);
    if (!adj || !key || !val) {
        free(adj), free(key), free(val);
        return false;
    }
    for (size_t i = 0; i < nfaces; i++) adj[i] = -1;
    for (int e = 0; e < nelems; e++)
        for (int f = 0; f < et_nfe(etype); f++) {
            int q[4], o[4];
            face_nodes_sorted(conn, e, f, q);
            uint64_t h = 1469598103934665603ull;
            for (int k = 0; k < 4; k++) {
                h ^= (uint64_t)(uint32_t)q[k];
                h *= 1099511628211ull;
            }
            h |= 1ull; /* 0 marks an empty slot */
            size_t i = (size_t)(h >> 13) & (cap - 1);
            for (;; i = (i + 1) & (cap - 1)) {
                if (!key[i]) { /* first time this face is seen */
                    key[i] = h, val[i] = 6 * e + f;
                    break;
                }
                if (key[i] != h) continue;
                if (val[i] < 0) break;  /* already paired: a hex face has at most two owners */
                face_nodes_sorted(conn, val[i] / 6, val[i] % 6, o);
                if (o[0] != q[0] || o[1] != q[1] || o[2] != q[2] || o[3] != q[3]) continue;
                adj[6 * e + f] = val[i] / 6; /* the two elements share it */
                adj[val[i]] = e;
                val[i] = -1; /* the slot stays occupied: clearing it would break later probe chains */
                break;
            }
        }
    free(key);
    free(val);
    B.adj = adj;
    B.adj_nelems = nelems;
    str_copy(B.adj_job, sizeof B.adj_job, job);
    return true;
}

static bool build_mesh_cache(const double *xyz, const int *conn, int nelems, const char *job, int etype) {
    bool cached = B.cent && B.adj && B.adj_nelems == nelems && !strcmp(B.adj_job, job) && B.adj_etype == etype;
    if (!build_adjacency(conn, nelems, job, etype)) return false;
    int npe = et_npe(etype), nc = etype ? 4 : 8; /* the centroid of the corners */
    if (cached) return true;
    free(B.cent);
    B.cent = calloc((size_t)nelems * 3, sizeof *B.cent);
    if (!B.cent) return false;
    for (int a = 0; a < 3; a++) B.section_lo[a] = 1e300, B.section_hi[a] = -1e300, B.cell[a] = 0;
    for (int e = 0; e < nelems; e++) for (int a = 0; a < 3; a++) {
        double lo = 1e300, hi = -1e300, sum = 0;
        for (int k = 0; k < nc; k++) {
            double x = xyz[3 * (size_t)conn[(size_t)npe * e + k] + a];
            sum += x; lo = MINI(lo, x); hi = MAXI(hi, x);
        }
        B.cent[3 * (size_t)e + a] = (float)(sum / nc);
        B.section_lo[a] = MINI(B.section_lo[a], lo); B.section_hi[a] = MAXI(B.section_hi[a], hi);
        B.cell[a] = MAXI(B.cell[a], hi-lo);
    }
    return true;
}

double fem_project_mesh_mm(void) { return B.st.element_size_mm; }

/* about 20 000 elements over the part's own volume, rounded to a whole or half millimetre so the number reads well */
double fem_suggested_mesh_mm(void) {
    double v = B.st.volume_mm3;
    if (!(v > 0)) {
        double d = MAXI(MAXI(B.st.size_mm[0], B.st.size_mm[1]), B.st.size_mm[2]);
        v = d > 0 ? d * d * d * 0.25 : 0;
    }
    if (!(v > 0)) return 2.0;
    double h = pow(v / 20000.0, 1.0 / 3.0);
    double step = h >= 1.0 ? 0.5 : 0.1;
    h = floor(h / step + 0.5) * step;
    return CLAMP(h, 0.2, 20.0);
}

double fem_mesh_size_mm(void) {
    if (B.mesh_mm > 0 && B.mesh_mm_rev == B.st.revision) return B.mesh_mm; /* the user's choice, still current */
    if (B.st.element_size_mm > 0) return B.st.element_size_mm;             /* what the project actually meshed at */
    return fem_suggested_mesh_mm();
}

void fem_set_mesh_size_mm(double mm) {
    B.mesh_mm = CLAMP(mm, 0.05, 100.0);
    B.mesh_mm_rev = B.st.revision;
}

void fem_free_body_name(const char *wanted, char *out, size_t cap) {
    char base[64];
    str_copy(base, sizeof base, wanted && wanted[0] ? wanted : "part");
    for (char *c = base; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.') *c = '_';
    str_copy(out, cap, base);
    if (!app.engine) return;
    engine_lock(app.engine);
    Project *p = engine_project_locked(app.engine);
    for (int n = 2; p && n < 100; n++) {
        bool taken = false;
        for (int i = 0; i < p->nbodies; i++) taken |= !strcmp(p->bodies[i]->name, out);
        if (!taken) break;
        snprintf(out, cap, "%.50s-%d", base, n);
    }
    engine_unlock(app.engine);
}


/* ---- Simple mode's questions and answers ---------------------------------------------------------------------- */

static int pick_patch(float px, float py, double point_mm[3], double normal[3], double *area_mm2);

/* The size of a file that is waiting for its unit, in the file's own numbers, so the question can be asked with the
 * part's real length in each candidate unit ("Is this part 70 mm or 70 cm long?"). Read once per file. */
bool fem_pending_extent(double ext[3]) {
    static char cached[1024];
    static double cext[3];
    static bool ok;
    const char *path = fem_pending_import();
    if (!path) return false;
    if (strcmp(cached, path) != 0) {
        str_copy(cached, sizeof cached, path);
        Mesh m;
        char err[256];
        mesh_init(&m);
        ok = mesh_load_stl(path, &m, err, sizeof err) && m.tri_count > 0;
        if (ok) {
            mesh_compute_bounds(&m);
            cext[0] = m.bmax.x - m.bmin.x, cext[1] = m.bmax.y - m.bmin.y, cext[2] = m.bmax.z - m.bmin.z;
        }
        mesh_free(&m);
    }
    if (ok) memcpy(ext, cext, sizeof cext);
    return ok;
}

/* After a part arrives in Simple mode: a file that is almost closed is repaired without asking, and the sentence says
 * what changed. "Almost" is measured before the repair: fewer bad edges than one in a hundred facets. A file with
 * more is left as it is, still meshed by the majority vote, and the sentence says so and offers the repair. */
static char g_part_sentence[400];
const char *fem_part_sentence(void) { return g_part_sentence; }

void fem_simple_after_import(void) {
    const FemState *s = &B.st;
    fem_poll();
    if (s->nbodies == 0) {
        g_part_sentence[0] = 0;
        return;
    }
    if (s->closed_solid) {
        snprintf(g_part_sentence, sizeof g_part_sentence, "Your file is a closed solid. It can be simulated.");
        return;
    }
    int bad = s->open_edges + s->nonmanifold_edges;
    if ((double)bad < 0.01 * (double)s->triangles) {
        fem_repair_body();
        fem_poll();
        int changed = B.st.repair_holes_filled + B.st.repair_shells_dropped;
        double share = s->triangles > 0 ? 100.0 * B.st.repair_facets_dropped / (double)(s->triangles + B.st.repair_facets_dropped) : 0;
        if (share > 5.0)
            snprintf(g_part_sentence, sizeof g_part_sentence,
                     "Your file had gaps. We closed them, but that removed %.0f %% of the surface: look at the part "
                     "before you trust the result.", share);
        else
            snprintf(g_part_sentence, sizeof g_part_sentence,
                     "Your file had small gaps; we closed %d of them. It can be simulated.", changed);
    } else {
        snprintf(g_part_sentence, sizeof g_part_sentence,
                 "Your file has larger gaps than we repair without asking (%d broken edges). It can still be "
                 "simulated; REPAIR in Advanced mode closes them and says what it changed.", bad);
    }
}

/* "Which face sits on the plate?" The clicked face's outward normal, taken back into the file's own axes, is rounded
 * to the nearest axis of the file, and that axis is placed facing down. A face that is not square to the file's axes
 * gets the nearest one, and the sentence says how far off it was. */
static char g_plate_sentence[200];
const char *fem_plate_sentence(void) { return g_plate_sentence; }

bool fem_place_face_down(int patch) {
    if (!app.engine || patch < 0) return false;
    double nb[3] = {0, 0, 0};
    char name[64] = "";
    double R[9];
    engine_lock(app.engine);
    Project *pj = engine_project_locked(app.engine);
    Body *b = pj ? project_body(pj, NULL) : NULL;
    bool ok = false;
    if (b && patch < b->patches.n) {
        PatchGeom g;
        patch_geometry(&b->surf, b->build_v, NULL, &b->patches, patch, &g);
        memcpy(nb, g.normal, sizeof nb);
        memcpy(R, b->place.R, sizeof R);
        str_copy(name, sizeof name, b->name);
        ok = true;
    }
    engine_unlock(app.engine);
    if (!ok) return false;
    /* build = R file, so file = R^T build: the face normal in the file's axes */
    double nf[3];
    for (int i = 0; i < 3; i++) nf[i] = R[0 * 3 + i] * nb[0] + R[1 * 3 + i] * nb[1] + R[2 * 3 + i] * nb[2];
    int ax = 0;
    for (int i = 1; i < 3; i++)
        if (fabs(nf[i]) > fabs(nf[ax])) ax = i;
    double len = sqrt(nf[0] * nf[0] + nf[1] * nf[1] + nf[2] * nf[2]);
    double off = len > 0 ? acos(CLAMP(fabs(nf[ax]) / len, 0.0, 1.0)) * 180.0 / M_PI : 0;
    /* the face goes down, so the opposite direction becomes up */
    char up[4];
    snprintf(up, sizeof up, "%c%c", nf[ax] > 0 ? '-' : '+', "XYZ"[ax]);
    if (!fem_op("geometry_place", "{\"body\": \"%s\", \"up_axis\": \"%s\", \"up_axis_source\": \"user\"}", name, up))
        return false;
    if (off > 1.0)
        snprintf(g_plate_sentence, sizeof g_plate_sentence,
                 "That face is tilted %.0f degrees from the file's axes; the nearest flat way down was used.", off);
    else
        snprintf(g_plate_sentence, sizeof g_plate_sentence, "That face now sits on the plate.");
    fem_clear_pick();
    B.surface_dirty = true;
    return true;
}

/* the patch under the cursor, for the plate question (the same ray as HOLD & LOAD uses) */
int fem_patch_at(float px, float py) { return pick_patch(px, py, NULL, NULL, NULL); }

const char *fem_pending_import(void) { return B.pending_import[0] ? B.pending_import : NULL; }

void fem_set_pending_import(const char *path) { str_copy(B.pending_import, sizeof B.pending_import, path ? path : ""); }

bool fem_surface_view(void) { return B.surface_view; }

void fem_set_build_preview(double fraction) {
    double f = fraction < 0 ? -1 : CLAMP(fraction, 0.0, 1.0);
    if (fabs(f - B.preview) < 1e-3) return;
    B.preview = f;
    B.surface_dirty = true;
}

void fem_set_surface_view(bool on) {
    if (B.surface_view == on) return;
    B.surface_view = on;
    B.surface_dirty = true;
    B.vis_gen++;
}

bool fem_peak_marker(float out[3]) {
    if (!B.peak_valid || !B.surface_valid) return false;
    out[0] = B.peak_world.x, out[1] = B.peak_world.y, out[2] = B.peak_world.z;
    return true;
}

bool fem_section_on(void) { return B.section_on; }
int fem_section_axis(void) { return B.section_axis; }
double fem_section_position(void) { return B.section_fraction; }
bool fem_section_flipped(void) { return B.section_flip; }
void fem_section(int axis, double fraction) {
    B.section_on = axis >= 0;
    if (axis >= 0) B.section_axis = CLAMP(axis, 0, 2);
    B.section_fraction = CLAMP(fraction, 0, 1);
    B.vis_gen++;
    B.surface_dirty = true;
}
void fem_section_flip(void) { B.section_flip = !B.section_flip; B.vis_gen++; B.surface_dirty = true; }
bool fem_group_shown(int g) { return g >= 0 && g < 3 && B.groups[g]; }
void fem_show_group(int g, bool on) { if (g >= 0 && g < 3) B.groups[g] = on; B.vis_gen++; B.surface_dirty = true; }

bool fem_fake_cut(int axis, double height_mm, int step) {
    if (!B.cent || !B.st.have_result || axis < 0 || axis > 2 || step < 0 || step >= B.st.nsteps) return false;
    if (!B.fake_death) {
        B.fake_death = malloc((size_t)B.adj_nelems * sizeof *B.fake_death);
        if (!B.fake_death) return false;
        for (int e = 0; e < B.adj_nelems; e++) B.fake_death[e] = -1;
    }
    /* Half-open band: an exact boundary belongs to only one voxel layer. */
    double h = height_mm * 1e-3, half = B.cell[axis]*0.5;
    int count = 0;
    for (int e = 0; e < B.adj_nelems; e++) {
        double d = B.cent[3*(size_t)e+axis] - h;
        if (d >= -half-1e-9 && d < half-1e-9) B.fake_death[e] = step, count++;
    }
    B.fake_nelems = B.adj_nelems;
    str_copy(B.fake_job, sizeof B.fake_job, B.st.result_job);
    B.vis_gen++;
    B.surface_dirty = true;
    LOGI("cut: %d elements removed from stored index %d", count, step);
    return true;
}
bool fem_fake_groups(int axis, double h1, double h2) {
    if (!B.cent || !B.st.have_result || axis < 0 || axis > 2 || h2 < h1) return false;
    unsigned char *g = malloc((size_t)B.adj_nelems);
    if (!g) return false;
    for (int e = 0; e < B.adj_nelems; e++) {
        double h = 1e3 * B.cent[3*(size_t)e+axis];
        g[e] = h < h1 ? 2 : h < h2 ? 1 : 0;
    }
    free(B.fake_group); B.fake_group = g; B.fake_nelems = B.adj_nelems;
    str_copy(B.fake_job, sizeof B.fake_job, B.st.result_job);
    B.vis_gen++;
    B.surface_dirty = true;
    return true;
}
void fem_fake_cut_clear(void) { free(B.fake_death); B.fake_death = NULL; B.vis_gen++; B.surface_dirty = true; }

/* ---- the result on the part's own surface -------------------------------------------------------------------
 *
 * The mesh is a voxel mesh, so its boundary is a staircase; the part is not. Every vertex of the STL is placed in
 * the element that contains it, and the displacement and the field are interpolated trilinearly inside that element,
 * exactly as the solver's shape functions do. A vertex that falls outside the staircase (the STL surface is up to
 * about half an element away from it) is snapped to the nearest point of the nearest element within SNAP_CELLS
 * element sizes, and the distance it had to move is reported in the panel, so nobody mistakes a smooth picture for a
 * smooth mesh. The mapping costs one pass over the STL vertices and is cached until the mesh or the geometry changes.
 */

#define SNAP_CELLS 2.0 /* how far, in element sizes, a vertex may be from any element before it is not drawn */

static void map_free(void) {
    free(B.map_elem); free(B.map_w); free(B.map_normal);
    B.map_elem = NULL; B.map_w = NULL; B.map_normal = NULL;
    B.map_nv = 0; B.map_job[0] = 0; B.map_nelems = 0; B.map_geom_revision = 0;
    B.map_snap_max_mm = B.map_snap_mean_mm = 0; B.map_snapped = B.map_unmapped = 0;
}

/* crease-smoothed vertex normals: the average of the facet normals around a vertex, except where a facet turns away
 * from that average by more than CREASE, which keeps a chamfer sharp and a fillet smooth */
static void build_vertex_normals(const Surface *sf, const double *normal, float *out) {
    const double CREASE = 0.7071; /* cos 45 degrees */
    memset(out, 0, 3 * sizeof(float) * (size_t)sf->nv);
    for (int t = 0; t < sf->nt; t++) {
        const double *n = &normal[3 * (size_t)t];
        for (int k = 0; k < 3; k++) {
            float *a = &out[3 * (size_t)sf->tri[3 * (size_t)t + k]];
            a[0] += (float)n[0], a[1] += (float)n[1], a[2] += (float)n[2];
        }
    }
    for (int v = 0; v < sf->nv; v++) {
        float *a = &out[3 * (size_t)v];
        float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (len > 1e-20f) a[0] /= len, a[1] /= len, a[2] /= len;
    }
    (void)CREASE; /* the per-corner crease test is done when the triangle is emitted, against this average */
}

/* the regular grid the voxel mesh lives on, recovered from its own nodes so a reloaded result needs nothing else */
typedef struct CellGrid {
    double origin[3], h[3];
    int dims[3];
    int *cell;              /* dims product: element index or -1 */
} CellGrid;

static void grid_free(CellGrid *g) { free(g->cell); g->cell = NULL; }

static bool grid_build(const SurfSrc *s, CellGrid *g) {
    memset(g, 0, sizeof *g);
    if (s->nelems <= 0) return false;
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (int n = 0; n < s->nnodes; n++)
        for (int a = 0; a < 3; a++) {
            double x = s->xyz[3 * (size_t)n + a];
            lo[a] = MINI(lo[a], x), hi[a] = MAXI(hi[a], x);
        }
    /* element 0 gives the cell size: every element of a voxel mesh is the same box */
    double elo[3] = {1e300, 1e300, 1e300}, ehi[3] = {-1e300, -1e300, -1e300};
    for (int k = 0; k < 8; k++) {
        const double *x = &s->xyz[3 * (size_t)s->conn[k]];
        for (int a = 0; a < 3; a++) elo[a] = MINI(elo[a], x[a]), ehi[a] = MAXI(ehi[a], x[a]);
    }
    for (int a = 0; a < 3; a++) {
        g->h[a] = ehi[a] - elo[a];
        if (!(g->h[a] > 0)) return false;
        g->origin[a] = lo[a];
        g->dims[a] = (int)lround((hi[a] - lo[a]) / g->h[a]);
        if (g->dims[a] < 1) g->dims[a] = 1;
    }
    size_t n = (size_t)g->dims[0] * g->dims[1] * g->dims[2];
    if (n > 200000000u) return false; /* a grid this big means the mesh is not the voxel mesh we think it is */
    g->cell = malloc(n * sizeof *g->cell);
    if (!g->cell) return false;
    for (size_t i = 0; i < n; i++) g->cell[i] = -1;
    for (int e = 0; e < s->nelems; e++) {
        const double *x = &s->xyz[3 * (size_t)s->conn[8 * (size_t)e]];
        int ijk[3];
        bool ok = true;
        for (int a = 0; a < 3; a++) {
            ijk[a] = (int)lround((x[a] - g->origin[a]) / g->h[a]);
            if (ijk[a] < 0 || ijk[a] >= g->dims[a]) ok = false;
        }
        if (ok) g->cell[(size_t)ijk[0] + (size_t)g->dims[0] * ((size_t)ijk[1] + (size_t)g->dims[1] * ijk[2])] = e;
    }
    return true;
}

/* the weights of a point inside element e, clamped to the element; returns how far the point had to move (m) */
static double element_weights(const SurfSrc *s, int e, const double p[3], float w[8]) {
    const int *en = &s->conn[8 * (size_t)e];
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (int k = 0; k < 8; k++) {
        const double *x = &s->xyz[3 * (size_t)en[k]];
        for (int a = 0; a < 3; a++) lo[a] = MINI(lo[a], x[a]), hi[a] = MAXI(hi[a], x[a]);
    }
    double xi[3], d2 = 0;
    for (int a = 0; a < 3; a++) {
        double c = 0.5 * (lo[a] + hi[a]), half = 0.5 * (hi[a] - lo[a]);
        double q = half > 0 ? (p[a] - c) / half : 0;
        double qc = CLAMP(q, -1.0, 1.0);
        double off = (q - qc) * half;
        d2 += off * off;
        xi[a] = qc;
    }
    double N[8];
    hex8_shape(xi[0], xi[1], xi[2], N, NULL);
    for (int k = 0; k < 8; k++) w[k] = (float)N[k];
    return sqrt(d2);
}

/* Place every STL vertex in an element. One pass, O(1) per vertex inside the staircase and a small spiral outside. */
static bool build_surface_map(const SurfSrc *s, const Body *b) {
    const Surface *sf = &b->surf;
    if (!b->build_v || sf->nv <= 0 || sf->nt <= 0) return false;
    if (B.map_elem && B.map_nv == sf->nv && B.map_nelems == s->nelems &&
        B.map_geom_revision == b->geom_revision && !strcmp(B.map_job, B.st.result_job))
        return true; /* still the same geometry on the same mesh */
    map_free();
    CellGrid g;
    if (!grid_build(s, &g)) return false;
    int *elem = malloc((size_t)sf->nv * sizeof *elem);
    float *w = malloc(8 * (size_t)sf->nv * sizeof *w);
    float *nrm = malloc(3 * (size_t)sf->nv * sizeof *nrm);
    if (!elem || !w || !nrm) {
        free(elem); free(w); free(nrm); grid_free(&g);
        return false;
    }
    double t0 = now_seconds();
    build_vertex_normals(sf, b->build_normal, nrm);
    int rings = (int)ceil(SNAP_CELLS);
    double snap_sum = 0, snap_max = 0;
    int snapped = 0, unmapped = 0;
    for (int v = 0; v < sf->nv; v++) {
        const double *p = &b->build_v[3 * (size_t)v];
        int c[3];
        for (int a = 0; a < 3; a++) {
            c[a] = (int)floor((p[a] - g.origin[a]) / g.h[a]);
            c[a] = CLAMP(c[a], 0, g.dims[a] - 1);
        }
        int best = -1;
        double bestd = 1e300;
        for (int r = 0; r <= rings && best < 0; r++) {
            for (int dk = -r; dk <= r; dk++)
                for (int dj = -r; dj <= r; dj++)
                    for (int di = -r; di <= r; di++) {
                        if (r > 0 && abs(di) != r && abs(dj) != r && abs(dk) != r) continue; /* shell only */
                        int i = c[0] + di, j = c[1] + dj, k = c[2] + dk;
                        if (i < 0 || j < 0 || k < 0 || i >= g.dims[0] || j >= g.dims[1] || k >= g.dims[2]) continue;
                        int e = g.cell[(size_t)i + (size_t)g.dims[0] * ((size_t)j + (size_t)g.dims[1] * k)];
                        if (e < 0) continue;
                        float tw[8];
                        double d = element_weights(s, e, p, tw);
                        if (d < bestd) {
                            bestd = d, best = e;
                            memcpy(&w[8 * (size_t)v], tw, sizeof tw);
                        }
                    }
            if (best >= 0 && bestd <= 1e-12) break; /* inside an element: nothing closer exists */
        }
        elem[v] = best;
        if (best < 0) unmapped++;
        else if (bestd > 1e-12) {
            snapped++;
            snap_sum += bestd;
            if (bestd > snap_max) snap_max = bestd;
        }
    }
    grid_free(&g);
    B.map_elem = elem, B.map_w = w, B.map_normal = nrm, B.map_nv = sf->nv;
    B.map_nelems = s->nelems;
    B.map_geom_revision = b->geom_revision;
    str_copy(B.map_job, sizeof B.map_job, B.st.result_job);
    B.map_ms = (now_seconds() - t0) * 1e3;
    B.map_snap_max_mm = snap_max * 1e3;
    B.map_snap_mean_mm = snapped ? snap_sum / snapped * 1e3 : 0;
    B.map_snapped = snapped, B.map_unmapped = unmapped;
    LOGI("surface map: %d vertices onto %d elements in %.0f ms; %d snapped (mean %.3g mm, largest %.3g mm), %d not drawn",
         sf->nv, s->nelems, B.map_ms, snapped, B.map_snap_mean_mm, B.map_snap_max_mm, unmapped);
    return true;
}

/* Draw the STL, coloured and displaced by the result. Returns false when the mapping is not usable. */
static bool emit_stl_surface(const SurfSrc *s, const Body *b, double deform, IsoMesh *m, float *lo, float *hi,
                             vec3 *wlo, vec3 *whi) {
    if (!B.map_elem || B.map_nv != b->surf.nv) return false;
    const Surface *sf = &b->surf;
    const double sc = B.to_world_scale;
    const vec3 c0 = B.world_centre;
    const double CREASE = 0.7071;
    double cut = 0;
    int caxis = B.section_axis;
    if (B.section_on) cut = B.section_lo[caxis] + B.section_fraction * (B.section_hi[caxis] - B.section_lo[caxis]);
    double peak = -1e300;
    B.peak_valid = false;
    for (int t = 0; t < sf->nt; t++) {
        if (B.draw_piece >= 0 && sf->comp && !in_piece(B.comp_piece ? B.comp_piece[sf->comp[t]] : sf->comp[t])) continue;
        const int *tv = &sf->tri[3 * (size_t)t];
        vec3 p[3];
        float val[3], nv[9];
        bool ok = true;
        for (int k = 0; k < 3 && ok; k++) {
            int v = tv[k];
            int e = B.map_elem[v];
            if (e < 0 || !element_shown(s, e)) { ok = false; break; }
            const double *x = &b->build_v[3 * (size_t)v];
            if (B.section_on && (B.section_flip ? x[caxis] < cut : x[caxis] > cut)) { ok = false; break; }
            const float *ws = &B.map_w[8 * (size_t)v];
            const int *en = &s->conn[8 * (size_t)e];
            double d[3] = {0, 0, 0};
            double fv = 0;
            for (int a = 0; a < 8; a++) {
                int n = en[a];
                if (s->u) {
                    const double *u = &s->u[3 * (size_t)n];
                    d[0] += ws[a] * u[0], d[1] += ws[a] * u[1], d[2] += ws[a] * u[2];
                }
                if (s->value) fv += ws[a] * s->value[n];
            }
            p[k] = v3((float)(c0.x + sc * (x[0] + d[0] * deform - B.centre_m[0])),
                      (float)(c0.y + sc * (x[2] + d[2] * deform - B.centre_m[2])),
                      (float)(c0.z - sc * (x[1] + d[1] * deform - B.centre_m[1])));
            val[k] = s->value ? (float)fv : 0.0f;
            const float *vn = &B.map_normal[3 * (size_t)v];
            nv[3 * k] = vn[0], nv[3 * k + 1] = vn[1], nv[3 * k + 2] = vn[2];
        }
        if (!ok) continue;
        /* a corner whose own facet turns away from the smoothed vertex normal keeps the facet normal: sharp edges
         * stay sharp, curved faces stay smooth */
        const double *fn = &b->build_normal[3 * (size_t)t];
        for (int k = 0; k < 3; k++) {
            double dot = fn[0] * nv[3 * k] + fn[1] * nv[3 * k + 1] + fn[2] * nv[3 * k + 2];
            if (dot < CREASE) nv[3 * k] = (float)fn[0], nv[3 * k + 1] = (float)fn[1], nv[3 * k + 2] = (float)fn[2];
        }
        uint32_t base = m->nverts;
        if (base + 3 > m->cap_verts || m->nindices + 3 > m->cap_indices) break;
        for (int k = 0; k < 3; k++) {
            float *vp = &m->verts[7 * (size_t)(base + k)];
            vp[0] = p[k].x, vp[1] = p[k].y, vp[2] = p[k].z;
            /* build frame (x, y, z up) -> viewer world (x, z, -y), the same turn the positions take */
            vp[3] = nv[3 * k], vp[4] = nv[3 * k + 2], vp[5] = -nv[3 * k + 1];
            vp[6] = val[k];
            *lo = MINI(*lo, val[k]), *hi = MAXI(*hi, val[k]);
            *wlo = v3_min(*wlo, p[k]), *whi = v3_max(*whi, p[k]);
            if (s->value && val[k] > peak) peak = val[k], B.peak_world = p[k], B.peak_valid = true;
            m->indices[m->nindices++] = base + k;
        }
        m->nverts += 3;
    }
    return m->nindices > 0;
}

/* one triangle of a tetrahedral face into the draw mesh: displaced corners, their values, a flat normal */
static bool emit_tri(const SurfSrc *s, const int n3[3], double deform, bool grey, IsoMesh *m, float *lo, float *hi, vec3 *wlo, vec3 *whi) {
    const double sc = B.to_world_scale;
    const vec3 c0 = B.world_centre;
    vec3 p[3];
    float val[3];
    for (int k = 0; k < 3; k++) {
        int n = n3[k];
        const double *x = &s->xyz[3 * (size_t)n];
        double d[3] = {0, 0, 0};
        if (s->u) {
            const double *u = &s->u[3 * (size_t)n];
            d[0] = u[0] * deform, d[1] = u[1] * deform, d[2] = u[2] * deform;
        }
        p[k] = v3((float)(c0.x + sc * (x[0] + d[0] - B.centre_m[0])), (float)(c0.y + sc * (x[2] + d[2] - B.centre_m[2])),
                  (float)(c0.z - sc * (x[1] + d[1] - B.centre_m[1])));
        val[k] = s->value ? (float)s->value[n] : 0.0f;
        if (grey) val[k] = -3e30f;
        else *lo = MINI(*lo, val[k]), *hi = MAXI(*hi, val[k]);
        *wlo = v3_min(*wlo, p[k]), *whi = v3_max(*whi, p[k]);
    }
    vec3 nrm = v3_norm(v3_cross(v3_sub(p[1], p[0]), v3_sub(p[2], p[0])));
    uint32_t base = m->nverts;
    if (base + 3 > m->cap_verts || m->nindices + 3 > m->cap_indices) return false;
    for (int k = 0; k < 3; k++) {
        float *vp = &m->verts[7 * (size_t)(base + k)];
        vp[0] = p[k].x, vp[1] = p[k].y, vp[2] = p[k].z;
        vp[3] = nrm.x, vp[4] = nrm.y, vp[5] = nrm.z;
        vp[6] = val[k];
        if (s->value && !grey && val[k] > B.tet_peak) B.tet_peak = val[k], B.peak_world = p[k], B.peak_valid = true;
        m->indices[m->nindices++] = base + (uint32_t)k;
    }
    m->nverts += 3;
    return true;
}

/* A tetrahedral result is drawn on its own boundary faces (the mesh follows the part's surface, so nothing needs to
 * be mapped onto the STL), and a section or a cut opens the faces between shown and hidden elements. */
static void emit_tet_surface(const SurfSrc *s, double deform, IsoMesh *m, float *lo, float *hi, vec3 *wlo, vec3 *whi) {
    *lo = 1e30f, *hi = -1e30f;
    *wlo = v3(1e30f, 1e30f, 1e30f), *whi = v3(-1e30f, -1e30f, -1e30f);
    for (int e = 0; e < s->nelems; e++) {
        if (!element_shown(s, e) || !elem_in_piece(e)) continue;
        for (int lf = 0; lf < 4; lf++) {
            int nb = s->adj ? s->adj[6 * (size_t)e + lf] : -1;
            if (!face_is_surface(s, e, nb)) continue;
            int n6[6], tri[4][3];
            et_face(s->elem_type, s->conn, e, lf, n6);
            int nt = et_face_tris(n6, tri);
            for (int t = 0; t < nt; t++)
                if (!emit_tri(s, tri[t], deform, neutral_group(s, e), m, lo, hi, wlo, whi)) return;
        }
    }
    if (*hi < *lo) *lo = 0, *hi = 1;
}

/* The faces the section or the cut opened: those are the voxel mesh, and they are drawn as such so the interior of
 * a sectioned part is never mistaken for the part's own surface. */
static void emit_cut_faces(const SurfSrc *s, double deform, IsoMesh *m, float *lo, float *hi, vec3 *wlo, vec3 *whi) {
    const double sc = B.to_world_scale;
    const vec3 c0 = B.world_centre;
    if (!s->adj) return;
    for (int e = 0; e < s->nelems; e++) {
        if (!element_shown(s, e) || !elem_in_piece(e)) continue;
        /* supports are not in the part's STL, so every face of theirs that is exposed is drawn from the mesh */
        bool support = s->vis.group && s->vis.group[e] == 1;
        for (int lf = 0; lf < 6; lf++) {
            int nb = s->adj[6 * (size_t)e + lf];
            if (support) {
                if (nb >= 0 && element_shown(s, nb)) continue;
            } else if (nb < 0 || element_shown(s, nb)) continue; /* only a face an interior neighbour used to hide */
            bool grey = neutral_group(s, e);
            const int *en = &s->conn[8 * (size_t)e];
            const int *fn = HEX8_FACE_NODES[lf];
            vec3 p[4];
            float val[4];
            for (int k = 0; k < 4; k++) {
                int n = en[fn[k]];
                const double *x = &s->xyz[3 * (size_t)n];
                double d[3] = {0, 0, 0};
                if (s->u) {
                    const double *u = &s->u[3 * (size_t)n];
                    d[0] = u[0] * deform, d[1] = u[1] * deform, d[2] = u[2] * deform;
                }
                p[k] = v3((float)(c0.x + sc * (x[0] + d[0] - B.centre_m[0])),
                          (float)(c0.y + sc * (x[2] + d[2] - B.centre_m[2])),
                          (float)(c0.z - sc * (x[1] + d[1] - B.centre_m[1])));
                val[k] = s->value ? (float)s->value[n] : 0.0f;
                if (grey) val[k] = -3e30f; /* the shader's neutral sentinel */
                else *lo = MINI(*lo, val[k]), *hi = MAXI(*hi, val[k]);
                *wlo = v3_min(*wlo, p[k]), *whi = v3_max(*whi, p[k]);
            }
            vec3 nrm = v3_norm(v3_cross(v3_sub(p[1], p[0]), v3_sub(p[2], p[0])));
            uint32_t base = m->nverts;
            if (base + 4 > m->cap_verts || m->nindices + 6 > m->cap_indices) return;
            for (int k = 0; k < 4; k++) {
                float *vp = &m->verts[7 * (size_t)(base + k)];
                vp[0] = p[k].x, vp[1] = p[k].y, vp[2] = p[k].z;
                vp[3] = nrm.x, vp[4] = nrm.y, vp[5] = nrm.z;
                vp[6] = val[k];
            }
            m->nverts += 4;
            uint32_t *ip = &m->indices[m->nindices];
            ip[0] = base, ip[1] = base + 1, ip[2] = base + 2;
            ip[3] = base, ip[4] = base + 2, ip[5] = base + 3;
            m->nindices += 6;
        }
    }
}

static void emit_surface(const SurfSrc *s, double deform, IsoMesh *m, float *lo, float *hi, vec3 *wlo, vec3 *whi) {
    const double sc = B.to_world_scale;
    const vec3 c0 = B.world_centre;
    *lo = 1e30f, *hi = -1e30f;
    *wlo = v3(1e30f, 1e30f, 1e30f), *whi = v3(-1e30f, -1e30f, -1e30f);
    for (int e = 0; e < s->nelems; e++) {
        if (!element_shown(s, e) || !elem_in_piece(e)) continue;
        for (int lf = 0; lf < 6; lf++) {
        int nb = s->adj ? s->adj[6 * (size_t)e + lf] : -1;
        if (!face_is_surface(s, e, nb)) continue; /* interior face: not a surface */
        const int *en = &s->conn[8 * (size_t)e];
        const int *fn = HEX8_FACE_NODES[lf];
        vec3 p[4];
        float val[4];
        for (int k = 0; k < 4; k++) {
            int n = en[fn[k]];
            const double *x = &s->xyz[3 * (size_t)n];
            double d[3] = {0, 0, 0};
            if (s->u) {
                const double *u = &s->u[3 * (size_t)n];
                d[0] = u[0] * deform, d[1] = u[1] * deform, d[2] = u[2] * deform;
            }
            /* build frame (x, y, z up) -> viewer world (x, z, -y), scaled about the part centre */
            p[k] = v3((float)(c0.x + sc * (x[0] + d[0] - B.centre_m[0])),
                      (float)(c0.y + sc * (x[2] + d[2] - B.centre_m[2])),
                      (float)(c0.z - sc * (x[1] + d[1] - B.centre_m[1])));
            val[k] = s->value ? (float)s->value[n] : 0.0f;
            if (!neutral_group(s, e)) {
                *lo = MINI(*lo, val[k]), *hi = MAXI(*hi, val[k]);
            } else val[k] = -3e30f; /* result shader's neutral plate sentinel, outside physical field values */
            *wlo = v3_min(*wlo, p[k]), *whi = v3_max(*whi, p[k]);
        }
        vec3 nrm = v3_norm(v3_cross(v3_sub(p[1], p[0]), v3_sub(p[2], p[0])));
        uint32_t base = m->nverts;
        if (base + 4 > m->cap_verts || m->nindices + 6 > m->cap_indices) return;
        for (int k = 0; k < 4; k++) {
            float *vp = &m->verts[7 * (size_t)(base + k)];
            vp[0] = p[k].x, vp[1] = p[k].y, vp[2] = p[k].z;
            vp[3] = nrm.x, vp[4] = nrm.y, vp[5] = nrm.z;
            vp[6] = val[k];
        }
        m->nverts += 4;
        uint32_t *ip = &m->indices[m->nindices];
        ip[0] = base, ip[1] = base + 1, ip[2] = base + 2;
        ip[3] = base, ip[4] = base + 2, ip[5] = base + 3;
        m->nindices += 6;
        }
    }
    if (*hi < *lo) *lo = 0, *hi = 1;
}

/* Undeformed crease edges of exactly the elements shown in this frame; no triangulation diagonals. */
static void rebuild_outline(const SurfSrc *s, size_t nfaces) {
    typedef struct { int a, b, dir; bool crease; } Edge;
    free(B.outline); B.outline = NULL; B.outline_vertices = 0;
    if (!s->u || !nfaces) return;
    size_t cap = 16;
    while (cap < nfaces * 8) cap <<= 1;
    Edge *tab = malloc(cap * sizeof *tab);
    if (!tab) return;
    for (size_t i = 0; i < cap; i++) tab[i].a = -1;
    for (int e = 0; e < s->nelems; e++) if (element_shown(s, e)) {
        for (int f = 0; f < et_nfe(s->elem_type); f++) {
            int nb = s->adj[6*(size_t)e+f];
            if (nb >= 0 && element_shown(s, nb)) continue;
            int n6[6];
            int nc = et_face(s->elem_type, s->conn, e, f, n6);
            for (int k = 0; k < nc; k++) {
                int a = n6[k];
                int b = n6[(k+1)%nc];
                if (a > b) { int tmp = a; a = b; b = tmp; }
                size_t h = ((size_t)a*73856093u ^ (size_t)b*19349663u) & (cap-1);
                while (tab[h].a >= 0 && (tab[h].a != a || tab[h].b != b)) h = (h+1)&(cap-1);
                /* hexahedra: a crease where the two faces face different ways (dir is the local face); tetrahedra: where
                 * their normals differ by more than 30 degrees (dir is 6 * element + face, decoded below) */
                int dir = s->elem_type ? 6 * e + f : f;
                if (tab[h].a < 0) tab[h] = (Edge){a,b,dir,false};
                else if (!s->elem_type && tab[h].dir != f) tab[h].crease = true;
                else if (s->elem_type) {
                    double nrm[2][3];
                    int ef[2][2] = {{tab[h].dir / 6, tab[h].dir % 6}, {e, f}};
                    for (int q = 0; q < 2; q++) {
                        int m6[6];
                        et_face(s->elem_type, s->conn, ef[q][0], ef[q][1], m6);
                        const double *p0 = s->xyz + 3 * (size_t)m6[0], *p1 = s->xyz + 3 * (size_t)m6[1], *p2 = s->xyz + 3 * (size_t)m6[2];
                        double u1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, u2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
                        nrm[q][0] = u1[1] * u2[2] - u1[2] * u2[1], nrm[q][1] = u1[2] * u2[0] - u1[0] * u2[2], nrm[q][2] = u1[0] * u2[1] - u1[1] * u2[0];
                    }
                    double d = nrm[0][0] * nrm[1][0] + nrm[0][1] * nrm[1][1] + nrm[0][2] * nrm[1][2];
                    double l = sqrt((nrm[0][0] * nrm[0][0] + nrm[0][1] * nrm[0][1] + nrm[0][2] * nrm[0][2]) *
                                    (nrm[1][0] * nrm[1][0] + nrm[1][1] * nrm[1][1] + nrm[1][2] * nrm[1][2]));
                    if (l > 0 && d < 0.866 * l) tab[h].crease = true;
                }
            }
        }
    }
    size_t count = 0;
    for (size_t i = 0; i < cap; i++) if (tab[i].a >= 0 && tab[i].crease) count++;
    B.outline = malloc(count * 6 * sizeof(float));
    if (B.outline) for (size_t i = 0; i < cap; i++) if (tab[i].a >= 0 && tab[i].crease) {
        for (int k = 0; k < 2; k++) {
            const double *x = s->xyz + 3*(size_t)(k ? tab[i].b : tab[i].a);
            float *v = B.outline + 3*(size_t)B.outline_vertices++;
            v[0] = B.world_centre.x + B.to_world_scale*(x[0]-B.centre_m[0]);
            v[1] = B.world_centre.y + B.to_world_scale*(x[2]-B.centre_m[2]);
            v[2] = B.world_centre.z - B.to_world_scale*(x[1]-B.centre_m[1]);
        }
    }
    free(tab);
}
const float *fem_outline(int *vertices) {
    /* the ghost of the undeformed shape belongs where the piece was, so it is dropped while the pieces stand apart */
    *vertices = B.visible && B.surface_valid && !(B.explode > 0 && B.npieces > 1) ? B.outline_vertices : 0;
    return B.outline;
}



/* ---- pieces and feature edges ------------------------------------------------------------------------------- */

/* The pieces of the mesh: its face-connected regions, from the adjacency that is already built. Small pieces beyond
 * the limit are folded into the last one, so the controls stay a handful. */
static int build_elem_pieces(const SurfSrc *s) {
    free(B.elem_piece);
    B.elem_piece = NULL;
    B.elem_piece_n = 0;
    if (s->nelems <= 0) return 1;
    /* several bodies in one analysis: the bodies are the pieces, whether or not they touch */
    if (s->elem_body && s->nbodies > 1) {
        int *pc = malloc((size_t)s->nelems * sizeof(int));
        if (!pc) return 1;
        int n = s->nbodies < FEM_MAX_DRAW_PARTS ? s->nbodies : FEM_MAX_DRAW_PARTS;
        for (int e = 0; e < s->nelems; e++) {
            int b = s->elem_body[e];
            pc[e] = b >= 0 && b < n ? b : 0;
        }
        B.elem_piece = pc;
        B.elem_piece_n = s->nelems;
        return n;
    }
    if (!s->adj) return 1;
    int *par = malloc((size_t)s->nelems * sizeof(int));
    if (!par) return 1;
    for (int e = 0; e < s->nelems; e++) par[e] = e;
    for (int e = 0; e < s->nelems; e++)
        for (int lf = 0; lf < et_nfe(s->elem_type); lf++) {
            int nb = s->adj[6 * (size_t)e + lf];
            if (nb < 0) continue;
            int a = e, b = nb;
            while (par[a] != a) par[a] = par[par[a]], a = par[a];
            while (par[b] != b) par[b] = par[par[b]], b = par[b];
            if (a != b) par[a < b ? b : a] = a < b ? a : b;
        }
    int *label = malloc((size_t)s->nelems * sizeof(int));
    if (!label) {
        free(par);
        return 1;
    }
    for (int e = 0; e < s->nelems; e++) label[e] = -1;
    int n = 0;
    for (int e = 0; e < s->nelems; e++) {
        int a = e;
        while (par[a] != a) a = par[a];
        if (label[a] < 0 && n < FEM_MAX_DRAW_PARTS) label[a] = n++;
        par[e] = label[a] >= 0 ? label[a] : (n ? n - 1 : 0);
    }
    free(label);
    B.elem_piece = par;
    B.elem_piece_n = s->nelems;
    return n ? n : 1;
}

/* The pieces of the STL: its outer shells. A cavity (or an island inside one) belongs to the shell that encloses it,
 * so a hollow part stays one piece and only genuinely separate shells are pulled apart. */
static int build_comp_pieces(const Body *b) {
    free(B.comp_piece);
    B.comp_piece = NULL;
    B.comp_piece_n = 0;
    if (!b || !b->surf.comp || b->surf.nt <= 0) return 1;
    int nc = b->surf.ncomp > 0 ? b->surf.ncomp : 1;
    int *map = malloc((size_t)nc * sizeof(int));
    if (!map) return 1;
    for (int i = 0; i < nc; i++) map[i] = -1;
    int n = 0;
    for (int c = 0; c < nc; c++) {
        int root = c, guard = 0;
        while (b->comps && b->comps[root].parent >= 0 && b->comps[root].parent < nc && guard++ < nc) root = b->comps[root].parent;
        if (map[root] < 0) map[root] = n < FEM_MAX_DRAW_PARTS ? n++ : FEM_MAX_DRAW_PARTS - 1;
        map[c] = map[root];
    }
    B.comp_piece = map;
    B.comp_piece_n = nc;
    return n ? n : 1;
}

/* the offset of a piece in the exploded view: from the whole centre towards the piece's own centre */
static void piece_offset(int i, float out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (B.explode <= 0 || B.npieces < 2) return;
    double c[3] = {0, 0, 0};
    for (int k = 0; k < B.npieces; k++)
        for (int a = 0; a < 3; a++) c[a] += B.piece_home_m[k][a] / B.npieces;
    double d[3];
    for (int a = 0; a < 3; a++) d[a] = B.piece_home_m[i][a] - c[a];
    double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 0)) {
        d[0] = (i % 2) ? 1 : -1, d[1] = 0, d[2] = 0;
        len = 1;
    }
    /* the world scale turns metres into the viewer's units; one whole part size at explode = 1 */
    double size = 0;
    for (int a = 0; a < 3; a++) size = MAXI(size, B.piece_hi_m[i][a] - B.piece_lo_m[i][a]);
    double amount = B.explode * (size > 0 ? size : 0.05) * 1.2 * B.to_world_scale;
    const double f[3] = {d[0] / len, d[2] / len, -d[1] / len}; /* build frame -> viewer world */
    for (int a = 0; a < 3; a++) out[a] = (float)(f[a] * amount);
}

/* the same displacement in the build frame and in metres, for picking in the exploded view */
static void piece_offset_build(int i, double out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (B.explode <= 0 || B.npieces < 2 || !(B.to_world_scale > 0)) return;
    float w[3];
    piece_offset(i, w);
    out[0] = w[0] / B.to_world_scale, out[1] = -w[2] / B.to_world_scale, out[2] = w[1] / B.to_world_scale;
}

/* Feature edges of the geometry: STL edges whose two triangles meet at more than the threshold, in world space, and
 * displaced with the surface when a result is drawn deformed. Every body of the project contributes, grouped by the
 * piece it belongs to, so the exploded view moves its edges with it. */
static void rebuild_edges(const SurfSrc *s, double deform) {
    free(B.edges);
    B.edges = NULL;
    B.edge_vertices = 0;
    memset(B.edge_parts, 0, sizeof B.edge_parts);
    /* Original STL creases can span removed material too. The visibility mesh has its own undeformed crease
     * outline, built from exactly the active element faces; never overlay the original skin's edges there. */
    if (!B.edges_on || !app.engine || B.st.visibility_mesh) return;
    const double cosang = cos(35.0 * M_PI / 180.0);
    int cap = 4096, n = 0;
    float *v = malloc((size_t)cap * 6 * sizeof(float));
    int *piece = malloc((size_t)cap * sizeof(int));
    if (!v || !piece) {
        free(v), free(piece);
        return;
    }
    engine_lock(app.engine);
    Project *pj = engine_project_locked(app.engine);
    int nb = pj ? pj->nbodies : 0;
    bool pieces_are_bodies = B.npieces > 1 && s && s->nbodies > 1;
    for (int bi = 0; bi < nb; bi++) {
        Body *b = pj->bodies[bi];
        if (!b || !b->build_v || b->surf.nt <= 0 || !b->surf.nbr) continue;
        const Surface *sf = &b->surf;
        bool mapped = B.map_elem && B.map_nv == sf->nv && s && s->u && deform != 0;
        for (int t = 0; t < sf->nt; t++)
            for (int k = 0; k < 3; k++) {
                int nbtri = sf->nbr[3 * (size_t)t + k];
                if (nbtri <= t) continue; /* each edge once; an open edge (nbtri < 0) is a feature too */
                const double *n1 = b->build_normal + 3 * (size_t)t, *n2 = b->build_normal + 3 * (size_t)nbtri;
                if (n1[0] * n2[0] + n1[1] * n2[1] + n1[2] * n2[2] > cosang) continue;
                if (n == cap) {
                    cap *= 2;
                    float *nv = realloc(v, (size_t)cap * 6 * sizeof(float));
                    int *np = realloc(piece, (size_t)cap * sizeof(int));
                    if (!nv || !np) {
                        free(nv ? nv : v), free(np ? np : piece);
                        engine_unlock(app.engine);
                        return;
                    }
                    v = nv, piece = np;
                }
                int ends[2] = {sf->tri[3 * (size_t)t + k], sf->tri[3 * (size_t)t + (k + 1) % 3]};
                for (int q = 0; q < 2; q++) {
                    const double *x = b->build_v + 3 * (size_t)ends[q];
                    double d[3] = {0, 0, 0};
                    if (mapped) {
                        int e = B.map_elem[ends[q]];
                        if (e >= 0) {
                            const float *w = &B.map_w[8 * (size_t)ends[q]];
                            const int *en = &s->conn[8 * (size_t)e];
                            for (int a = 0; a < 8; a++) {
                                const double *u = &s->u[3 * (size_t)en[a]];
                                d[0] += w[a] * u[0], d[1] += w[a] * u[1], d[2] += w[a] * u[2];
                            }
                        }
                    }
                    float *out = v + 6 * (size_t)n + 3 * q;
                    out[0] = (float)(B.world_centre.x + B.to_world_scale * (x[0] + d[0] * deform - B.centre_m[0]));
                    out[1] = (float)(B.world_centre.y + B.to_world_scale * (x[2] + d[2] * deform - B.centre_m[2]));
                    out[2] = (float)(B.world_centre.z - B.to_world_scale * (x[1] + d[1] * deform - B.centre_m[1]));
                }
                piece[n] = pieces_are_bodies ? (bi < B.npieces ? bi : 0)
                                             : (B.comp_piece && sf->comp && sf->comp[t] < B.comp_piece_n ? B.comp_piece[sf->comp[t]] : 0);
                n++;
            }
    }
    engine_unlock(app.engine);
    /* group the line pairs by piece so each can carry its own offset */
    float *grouped = malloc((size_t)(n ? n : 1) * 6 * sizeof(float));
    if (!grouped) {
        free(v), free(piece);
        return;
    }
    int at = 0;
    for (int p = 0; p < MAXI(B.npieces, 1) && p < FEM_MAX_DRAW_PARTS; p++) {
        uint32_t start = (uint32_t)(2 * at);
        for (int i = 0; i < n; i++)
            if (piece[i] == p) memcpy(grouped + 6 * (size_t)at++, v + 6 * (size_t)i, 6 * sizeof(float));
        B.edge_parts[p].index_start = start;
        B.edge_parts[p].index_count = (uint32_t)(2 * at) - start;
        B.edge_parts[p].opacity = 1;
        piece_offset(p, B.edge_parts[p].offset);
    }
    free(v), free(piece);
    B.edges = grouped;
    B.edge_vertices = 2 * at;
}

/* ---- what the renderer asks for ------------------------------------------------------------------------------ */

int fem_pieces(void) { return B.npieces; }
const char *fem_piece_name(int i) { return i >= 0 && i < B.npieces ? B.piece_name[i] : ""; }
float fem_piece_opacity(int i) { return i >= 0 && i < FEM_MAX_DRAW_PARTS ? B.piece_opacity[i] : 1.0f; }

void fem_set_piece_opacity(int i, float a) {
    if (i < 0 || i >= FEM_MAX_DRAW_PARTS) return;
    a = a < 0.05f ? 0.05f : (a > 1 ? 1 : a);
    if (B.piece_opacity[i] == a) return;
    B.piece_opacity[i] = a;
    if (i < B.npieces) B.parts[i].opacity = a; /* no rebuild: the renderer reads it every frame */
}

double fem_explode(void) { return B.explode; }

void fem_set_explode(double f) {
    f = f < 0 ? 0 : (f > 1.5 ? 1.5 : f);
    if (B.explode == f) return;
    bool was_apart = B.explode > 0;
    B.explode = f;
    if (was_apart != (f > 0)) { /* the faces between pieces are drawn while they stand apart: rebuild once */
        B.surface_dirty = true;
        B.vis_gen++;
    }
    for (int i = 0; i < B.npieces; i++) {
        piece_offset(i, B.parts[i].offset);
        piece_offset(i, B.edge_parts[i].offset);
        B.parts[i].centre[0] = (float)(B.world_centre.x + B.to_world_scale * (B.piece_home_m[i][0] - B.centre_m[0])) + B.parts[i].offset[0];
        B.parts[i].centre[1] = (float)(B.world_centre.y + B.to_world_scale * (B.piece_home_m[i][2] - B.centre_m[2])) + B.parts[i].offset[1];
        B.parts[i].centre[2] = (float)(B.world_centre.z - B.to_world_scale * (B.piece_home_m[i][1] - B.centre_m[1])) + B.parts[i].offset[2];
    }
}

bool fem_edges_on(void) { return B.edges_on; }

void fem_set_edges(bool on) {
    if (B.edges_on == on) return;
    B.edges_on = on;
    B.surface_dirty = true;
}

bool fem_ao_on(void) { return app.rs.ao_on; }
void fem_set_ao(bool on) { app.rs.ao_on = on; }

const ResultPart *fem_draw_parts(int *n) {
    *n = B.visible && B.surface_valid && B.npieces > 1 ? B.npieces : 0;
    return B.parts;
}

const float *fem_edge_lines(int *vertices, const ResultPart **parts, int *nparts) {
    *vertices = B.visible && B.edges_on ? B.edge_vertices : 0;
    *parts = B.edge_parts;
    *nparts = B.npieces > 1 ? B.npieces : 0;
    return B.edges;
}

bool fem_export_image(const char *path, int scale) { return app_export_image(path, scale); }

int fem_explode_lines(float *out, int max_vertices) {
    if (!(B.explode > 0) || B.npieces < 2) return 0;
    int n = 0;
    for (int i = 0; i < B.npieces && n + 2 <= max_vertices; i++) {
        float off[3];
        piece_offset(i, off);
        float home[3] = {(float)(B.world_centre.x + B.to_world_scale * (B.piece_home_m[i][0] - B.centre_m[0])),
                         (float)(B.world_centre.y + B.to_world_scale * (B.piece_home_m[i][2] - B.centre_m[2])),
                         (float)(B.world_centre.z - B.to_world_scale * (B.piece_home_m[i][1] - B.centre_m[1]))};
        for (int k = 0; k < 3; k++) out[3 * n + k] = home[k];
        for (int k = 0; k < 3; k++) out[3 * (n + 1) + k] = home[k] + off[k];
        n += 2;
    }
    return n;
}

/* ---- the mesh-convergence check ------------------------------------------------------------------------------ */

static ConvResult g_conv;

const ConvResult *fem_convergence(void) { return &g_conv; }

/* the numbers this check compares, straight out of the job summary the solver wrote */
static void summary_numbers(double *disp_mm, double *p99_mpa) {
    const JsonValue *sum = fem_job_summary();
    *disp_mm = sum ? json_get_num(json_get(sum, "displacement"), "max_magnitude_mm", 0) : 0;
    *p99_mpa = sum ? json_get_num(json_get(sum, "von_mises"), "nodal_p99_mpa", 0) : 0;
}

/* Repair the open part, through the operation, and keep what it changed so the PART step can say it in numbers. */
void fem_repair_body(void) {
    if (!fem_op("geometry_repair", "{\"provenance\": \"user\", \"max_hole_edges\": 64}")) return;
    const JsonValue *ch = json_get(fem_last_value(), "changes");
    B.st.repaired = true;
    B.st.repair_facets_dropped = (int)json_get_int(ch, "facets_dropped", 0);
    B.st.repair_shells_dropped = (int)json_get_int(ch, "shells_dropped", 0);
    B.st.repair_holes_filled = (int)json_get_int(ch, "holes_filled", 0);
    B.st.repair_facets_added = (int)json_get_int(ch, "facets_added", 0);
    const JsonValue *af = json_get(fem_last_value(), "after");
    LOGOK("repaired: %d facets dropped with %d shell(s), %d hole(s) filled with %d facets; %lld open and %lld "
          "non-manifold edges left",
          B.st.repair_facets_dropped, B.st.repair_shells_dropped, B.st.repair_holes_filled,
          B.st.repair_facets_added, json_get_int(af, "open_edges", 0), json_get_int(af, "nonmanifold_edges", 0));
    B.force_refresh = true;
    B.surface_dirty = true;
}

bool fem_check_mesh(void) {
    const FemState *s = &B.st;
    if (!s->have_result || !s->meshed || s->nelems <= 0) {
        LOGW("solve the part first: the check compares this answer with the same answer on a finer mesh");
        return false;
    }
    memset(&g_conv, 0, sizeof g_conv);
    g_conv.coarse_mm = fem_project_mesh_mm();
    g_conv.coarse_elements = s->nelems;
    summary_numbers(&g_conv.coarse_disp, &g_conv.coarse_p99);
    /* 0.7 of the element size is about three times the elements; stop at 60 000 so the wait stays short. A tetrahedral
     * mesh starts with many more elements for the same size: 0.8 of the surface cell, stopped at 250 000. */
    bool tet = s->mesh_method == 1;
    double cap = tet ? 250000.0 : 60000.0;
    double fine = g_conv.coarse_mm * (tet ? 0.8 : 0.7);
    double predicted = (double)s->nelems * pow(g_conv.coarse_mm / fine, 3.0);
    if (predicted > cap) {
        fine = g_conv.coarse_mm * pow((double)s->nelems / cap, 1.0 / 3.0);
        g_conv.capped = true;
    }
    if (!(fine > 0) || fine >= g_conv.coarse_mm) {
        snprintf(g_conv.message, sizeof g_conv.message, "the mesh is already at the element limit for this check");
        g_conv.state = CONV_FAILED;
        return false;
    }
    g_conv.fine_mm = fine;
    g_conv.state = CONV_MESHING;
    LOGOK("mesh check: %.3g mm -> %.3g mm%s", g_conv.coarse_mm, fine, g_conv.capped ? (tet ? " (capped at about 250 000 elements)" : " (capped at about 60 000 elements)") : "");
    bool made = s->mesh_method == 1 ? fem_op("mesh_generate", "{\"method\": \"tet\", \"surface_size\": %.6g, \"order\": %d, \"thin_wall_levels\": 1}",
                                             fine, s->tet_order == 1 ? 1 : 2)
                                     : fem_op("mesh_generate", "{\"element_size\": %.6g}", fine);
    if (!made) {
        g_conv.state = CONV_FAILED;
        str_copy(g_conv.message, sizeof g_conv.message, "the finer mesh could not be made");
        return false;
    }
    fem_set_mesh_size_mm(fine);
    if (!fem_op("analysis_run", "{\"analysis\": \"static_structural\", \"label\": \"mesh check\"}")) {
        g_conv.state = CONV_FAILED;
        str_copy(g_conv.message, sizeof g_conv.message, "the finer analysis was refused");
        return false;
    }
    const char *id = json_get_str(fem_last_value(), "job_id", NULL);
    if (id) {
        str_copy(g_conv.message, sizeof g_conv.message, "solving on the finer mesh");
        fem_follow_job(id);
        g_conv.state = CONV_SOLVING;
        return true;
    }
    g_conv.state = CONV_FAILED;
    return false;
}

/* called from the poll once the followed job has finished */
/* ---- the layer study ----------------------------------------------------------------------------------------- */

static LayerStudy g_layer;

const LayerStudy *fem_layer_study(void) { return &g_layer; }

/* the three numbers a build is judged by, out of the summary the solver wrote */
static void build_numbers(double *tip_mm, double *spring_mm, int *layers) {
    const JsonValue *res = json_get(fem_job_summary(), "results");
    *tip_mm = res ? json_get_num(res, "tip_uz_after_cut_mm", 0) : 0;
    *spring_mm = res ? json_get_num(res, "springback_mm", 0) : 0;
    *layers = res ? (int)json_get_int(res, "layers", 0) : 0;
}

bool fem_layer_study_begin(double coarse_layer_mm, double fine_layer_mm) {
    if (!B.st.have_result || !(coarse_layer_mm > 0) || !(fine_layer_mm > 0)) return false;
    const JsonValue *sum = fem_job_summary();
    if (!sum || strcmp(json_get_str(sum, "analysis", ""), "lpbf_build") != 0) {
        LOGW("the layer study is for an LPBF build: run one first");
        return false;
    }
    memset(&g_layer, 0, sizeof g_layer);
    g_layer.coarse_layer_mm = coarse_layer_mm;
    g_layer.fine_layer_mm = fine_layer_mm;
    build_numbers(&g_layer.coarse_tip_mm, &g_layer.coarse_spring_mm, &g_layer.coarse_layers);
    g_layer.state = LS_RUNNING;
    str_copy(g_layer.message, sizeof g_layer.message, "building again at half the simulation layer");
    LOGOK("layer study: %.4g mm -> %.4g mm simulation layer", coarse_layer_mm, fine_layer_mm);
    return true;
}

static void layer_study_finish(void) {
    const FemState *s = &B.st;
    if (g_layer.state != LS_RUNNING || s->job_active) return;
    if (!s->job_id[0]) return;
    if (strcmp(s->job_state, "succeeded") != 0) {
        if (!s->job_state[0]) return; /* nothing has been submitted yet: the panel starts the job after us */
        g_layer.state = LS_FAILED;
        snprintf(g_layer.message, sizeof g_layer.message, "the second build %s", s->job_state);
        return;
    }
    const JsonValue *sum = fem_job_summary();
    if (!sum) return;
    double layer_mm = json_get_num(json_get(sum, "model"), "layer_thickness_mm", 0);
    if (fabs(layer_mm - g_layer.fine_layer_mm) > 1e-9 * MAXI(1.0, g_layer.fine_layer_mm)) return; /* still the first run */
    build_numbers(&g_layer.fine_tip_mm, &g_layer.fine_spring_mm, &g_layer.fine_layers);
    g_layer.tip_change_pct = g_layer.coarse_tip_mm != 0
                                 ? (g_layer.fine_tip_mm - g_layer.coarse_tip_mm) / fabs(g_layer.coarse_tip_mm) * 100
                                 : 0;
    g_layer.spring_change_pct = g_layer.coarse_spring_mm != 0
                                    ? (g_layer.fine_spring_mm - g_layer.coarse_spring_mm) / fabs(g_layer.coarse_spring_mm) * 100
                                    : 0;
    /* An element is activated whole, so a simulation layer thinner than the element height activates the same
     * elements as the layer above it and the answer cannot move. Saying "no change" there would be a lie. */
    g_layer.element_mm = fem_project_mesh_mm();
    g_layer.limited_by_mesh = g_layer.element_mm > 0 && g_layer.fine_layer_mm < g_layer.element_mm - 1e-12;
    snprintf(g_layer.message, sizeof g_layer.message,
             "tip after the cut %+.1f %%, springback %+.1f %% from %d to %d layers%s",
             g_layer.tip_change_pct, g_layer.spring_change_pct, g_layer.coarse_layers, g_layer.fine_layers,
             g_layer.limited_by_mesh ? " (the element height, not the layer, is the limit here)" : "");
    g_layer.state = LS_DONE;
    LOGOK("layer study: %s", g_layer.message);
}

static void convergence_finish(void) {
    const FemState *s = &B.st;
    if (g_conv.state != CONV_SOLVING || s->job_active) return;
    if (strcmp(s->job_state, "succeeded") != 0) {
        g_conv.state = CONV_FAILED;
        snprintf(g_conv.message, sizeof g_conv.message, "the finer analysis %s",
                 s->job_state[0] ? s->job_state : "did not finish");
        return;
    }
    if (!fem_job_summary()) return; /* the summary arrives with the status; wait one more poll */
    g_conv.fine_elements = s->nelems;
    summary_numbers(&g_conv.fine_disp, &g_conv.fine_p99);
    g_conv.disp_change_pct = g_conv.coarse_disp > 0 ? (g_conv.fine_disp - g_conv.coarse_disp) / g_conv.coarse_disp * 100 : 0;
    g_conv.stress_change_pct = g_conv.coarse_p99 > 0 ? (g_conv.fine_p99 - g_conv.coarse_p99) / g_conv.coarse_p99 * 100 : 0;
    g_conv.converged = fabs(g_conv.disp_change_pct) < 5 && fabs(g_conv.stress_change_pct) < 5;
    snprintf(g_conv.message, sizeof g_conv.message,
             g_conv.converged ? "converged: displacement %+.1f %%, stress %+.1f %% from %d to %d elements"
                              : "not converged: displacement %+.1f %%, stress %+.1f %% from %d to %d elements - refine again or accept",
             g_conv.disp_change_pct, g_conv.stress_change_pct, g_conv.coarse_elements, g_conv.fine_elements);
    g_conv.state = CONV_DONE;
    LOGOK("mesh check: %s", g_conv.message);
}

/* ---- the report ----------------------------------------------------------------------------------------------- */

/* runs an operation that returns an image and writes the first one to a file */
static bool save_op_image(const char *path, const char *op, const char *params_fmt, ...) {
    if (!app.engine) return false;
    char buf[1024];
    va_list ap;
    va_start(ap, params_fmt);
    vsnprintf(buf, sizeof buf, params_fmt, ap);
    va_end(ap);
    JsonError jerr;
    JsonValue *params = json_parse(buf, strlen(buf), NULL, &jerr);
    if (!params) return false;
    OpResult r;
    OpCaller caller = {"ui", "navier-interface"};
    ops_invoke(app.engine, op, params, &caller, &r);
    json_free(params);
    bool ok = false;
    if (r.ok && r.nimages > 0) {
        FILE *f = fopen(path, "wb");
        if (f) {
            ok = fwrite(r.images[0].data, 1, r.images[0].len, f) == r.images[0].len;
            fclose(f);
        }
    } else if (!r.ok) {
        log_error(op, r.error);
    }
    op_result_free(&r);
    return ok;
}

/* what the Simple results screen said, written first in the report so the report reads like the screen */
static char g_report_preface[2048];
void fem_set_report_preface(const char *text) { str_copy(g_report_preface, sizeof g_report_preface, text ? text : ""); }

bool fem_write_report(char *dir_out, size_t cap) {
    const FemState *s = &B.st;
    if (!s->have_result || !s->project_dir[0]) {
        LOGW("there is no result to report yet");
        return false;
    }
    /* one folder per report, stamped with the moment it was written: a second report never overwrites the first */
    char dir[NV_PATH_MAX], parent[NV_PATH_MAX], stamp[32];
    snprintf(parent, sizeof parent, "%s/report", s->project_dir);
    if (mkdir(parent, 0755) != 0 && errno != EEXIST) {
        LOGE("cannot make %s", parent);
        return false;
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    snprintf(dir, sizeof dir, "%s/%s", parent, stamp);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        LOGE("cannot make %s", dir);
        return false;
    }
    char path[NV_PATH_MAX];
    /* the pictures: two from the software renderer, one of the window as it stands */
    snprintf(path, sizeof path, "%s/stress.png", dir);
    bool img1 = save_op_image(path, "results_render",
                              "{\"job_id\": \"%s\", \"quantity\": \"von_mises\", \"width\": 1600, \"height\": 900}",
                              s->result_job);
    snprintf(path, sizeof path, "%s/displacement.png", dir);
    bool img2 = save_op_image(path, "results_render",
                              "{\"job_id\": \"%s\", \"quantity\": \"displacement\", \"width\": 1600, \"height\": 900}",
                              s->result_job);
    snprintf(path, sizeof path, "%s/view.png", dir);
    app_request_screenshot(path); /* what is on screen, section and all */

    /* the files the operations write: VTU, CSV and the summary */
    fem_op("results_export", "{\"job_id\": \"%s\", \"directory\": \"%s\"}", s->result_job, dir);

    snprintf(path, sizeof path, "%s/report.md", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        LOGE("cannot write %s", path);
        return false;
    }
    const JsonValue *sum = fem_job_summary();
    const JsonValue *dp = sum ? json_get(sum, "displacement") : NULL;
    const JsonValue *vm = sum ? json_get(sum, "von_mises") : NULL;
    const JsonValue *ck = sum ? json_get(sum, "checks") : NULL;
    double at[3] = {0, 0, 0};
    if (dp) json_get_numbers(json_get(dp, "at_mm"), at, 3);
    double p99 = vm ? json_get_num(vm, "nodal_p99_mpa", 0) : 0;
    double peak = vm ? json_get_num(vm, "nodal_average_max_mpa", 0) : 0;
    /* a build is a different question, so it gets a different report: what the machine did to the part */
    const JsonValue *bres = sum ? json_get(sum, "results") : NULL;
    const char *ana = sum ? json_get_str(sum, "analysis", "") : "";
    bool is_lpbf = !strcmp(ana, "lpbf_build"), is_fff = !strcmp(ana, "fff_print");
    if (bres && (is_lpbf || is_fff)) {
        const JsonValue *model = json_get(sum, "model");
        const JsonValue *strain = model ? json_get(model, "inherent_strain") : NULL;
        const JsonValue *cut = model ? json_get(model, "cut") : NULL;
        fprintf(f, "# %s - %s\n\n", s->project[0] ? s->project : "part",
                is_lpbf ? "inherent-strain LPBF build" : "FFF print");
        if (g_report_preface[0]) fprintf(f, "## In plain words\n\n%s\n\n", g_report_preface);
        fprintf(f, "Job `%s`. Written by the NAVIER-AM application from the operations it ran.\n\n", s->result_job);
        fprintf(f, "## The answer\n\n| Quantity | Value | What it means |\n|---|---|---|\n");
        if (is_lpbf) {
            fprintf(f, "| Tip deflection, still on the plate | %.4g mm | %s |\n",
                    json_get_num(bres, "tip_uz_before_cut_mm", 0),
                    json_get_str(bres, "tip_definition", "largest upward displacement of the free end"));
            fprintf(f, "| Tip deflection, after the cut | %.4g mm | the part released from the build plate |\n",
                    json_get_num(bres, "tip_uz_after_cut_mm", 0));
            fprintf(f, "| Springback | %.4g mm | what the cut released |\n", json_get_num(bres, "springback_mm", 0));
            fprintf(f, "| Peak von Mises, on the plate | %.4g MPa | before the cut |\n",
                    json_get_num(bres, "peak_von_mises_before_cut_mpa", 0));
            fprintf(f, "| Peak von Mises, released | %.4g MPa | after the cut |\n",
                    json_get_num(bres, "peak_von_mises_after_cut_mpa", 0));
        } else {
            fprintf(f, "| Warp of the released part | %.4g to %.4g mm | vertical displacement after release from the bed |\n",
                    json_get_num(bres, "warp_z_min_mm", 0), json_get_num(bres, "warp_z_max_mm", 0));
            fprintf(f, "| Peak von Mises, on the bed | %.4g MPa | an upper bound: creep is not modelled |\n",
                    json_get_num(bres, "peak_von_mises_on_bed_mpa", 0));
            fprintf(f, "| Peak von Mises, released | %.4g MPa | after release |\n",
                    json_get_num(bres, "peak_von_mises_released_mpa", 0));
        }
        fprintf(f, "\n## The setup\n\n");
        fprintf(f, "- Part: %s, %.4g x %.4g x %.4g mm, %u triangles.\n", s->body, s->size_mm[0], s->size_mm[1],
                s->size_mm[2], s->triangles);
        fprintf(f, "- Mesh: %lld elements, %d stored times, %lld build layers.\n",
                model ? json_get_int(model, "elements", s->nelems) : s->nelems, s->nsteps,
                json_get_int(bres, "layers", 0));
        if (is_lpbf && strain) {
            fprintf(f, "- Inherent strain: exx %.6g, eyy %.6g, ezz %.6g, in the machine frame; build orientation %s.\n",
                    json_get_num(strain, "exx", 0), json_get_num(strain, "eyy", 0), json_get_num(strain, "ezz", 0),
                    json_get_str(model, "build_orientation", "X"));
            fprintf(f, "- Strain provenance: %s, source `%s`.\n", json_get_str(strain, "provenance", "unstated"),
                    json_get_str(strain, "source", "unstated"));
        }
        if (is_lpbf && cut)
            fprintf(f, "- Cut: %.4g mm high, %.4g mm kerf, from %.4g mm along the part; %lld elements removed; "
                       "provenance %s.\n",
                    json_get_num(cut, "height_mm", 0), json_get_num(cut, "kerf_mm", 0),
                    json_get_num(cut, "from_x_mm", 0), json_get_int(cut, "elements_removed", 0),
                    json_get_str(cut, "provenance", "unstated"));
        fprintf(f, "- Material assigned to the body: %s (%s).\n", s->material_name[0] ? s->material_name : "none",
                s->material_status[0] ? s->material_status : "unknown provenance");
        const JsonValue *emat = model ? json_get(model, "material") : NULL;
        if (emat)
            fprintf(f, "- Elastic constants the build actually used: %.6g MPa, poisson %.4g, provenance %s.%s\n",
                    json_get_num(emat, "youngs_modulus_mpa", 0), json_get_num(emat, "poissons_ratio", 0),
                    json_get_str(emat, "provenance", "unstated"),
                    is_lpbf ? " A body loaded only by an eigenstrain has a displacement that does not depend on the "
                              "modulus; the stresses do."
                            : "");
        fprintf(f, "\n## The checks the solver ran\n\n");
        if (is_lpbf)
            fprintf(f, "- Equilibrium error of the last solve: %.3g (dimensionless residual).\n"
                       "- Largest plate reaction: %.4g N.\n",
                    json_get_num(bres, "equilibrium_error_last_solve", 0),
                    json_get_num(bres, "largest_plate_reaction_n", 0));
        else
            fprintf(f, "- Worst heat balance: %.3g relative.\n- Support reaction after release: %.4g N (zero when the "
                       "released part is self-equilibrated).\n",
                    json_get_num(bres, "worst_heat_balance_relative", 0),
                    json_get_num(bres, "support_reaction_after_release_n", 0));
        fprintf(f, "\n## What these numbers are not\n\n");
        const JsonValue *scope = json_get(sum, "scope");
        const char *st = scope ? json_get_str(scope, "statement", NULL) : NULL;
        if (st) fprintf(f, "- %s\n", st);
        if (is_lpbf)
            fprintf(f, "- The inherent strain is a calibrated input, not a material property: the answer is only as "
                       "good as the calibration it came from, and it was calibrated on one geometry.\n"
                       "- Small-strain linear elasticity, no plasticity, no melt pool, no powder.\n");
        else
            fprintf(f, "- Toolpath within a layer, creep below the relaxation temperature, plasticity, supports and "
                       "adhesion failure are not modelled.\n");
        if (!strcmp(s->material_status, "demonstration"))
            fprintf(f, "- The material values are demonstration data: not traceable to a grade, a supplier or a test.\n");
        fprintf(f, "\n## Files here\n\n- `report.md` (this file)\n- `stress.png`%s\n- `displacement.png`%s\n"
                   "- `view.png`: the application window as it stood, at the stored time on screen\n"
                   "- `results.vtu`, `nodes.csv`, `summary.json` from results_export\n",
                img1 ? "" : " (not written)", img2 ? "" : " (not written)");
        fclose(f);
        if (dir_out) str_copy(dir_out, cap, dir);
        LOGOK("report written to %s", dir);
        return true;
    }

    fprintf(f, "# %s - structural analysis\n\n", s->project[0] ? s->project : "part");
    fprintf(f, "Job `%s`, %s. Written by the NAVIER-AM application from the operations it ran.\n\n",
            s->result_job, s->result_kind);
    fprintf(f, "## The answer\n\n");
    fprintf(f, "| Quantity | Value | What it means |\n|---|---|---|\n");
    fprintf(f, "| Largest displacement | %.4g mm | at %.4g, %.4g, %.4g mm in the build frame |\n",
            dp ? json_get_num(dp, "max_magnitude_mm", 0) : s->max_displacement_mm, at[0], at[1], at[2]);
    fprintf(f, "| Peak von Mises stress | %.4g MPa | %s |\n", peak,
            fem_result_caveat() ? fem_result_caveat() : "highest nodal average anywhere in the part");
    fprintf(f, "| 99th percentile stress | %.4g MPa | the value to design with |\n", p99);
    if (s->yield_mpa > 0 && p99 > 0)
        fprintf(f, "| Safety factor | %.3g | yield %.4g MPa over the 99th percentile |\n", s->yield_mpa / p99, s->yield_mpa);
    const JsonValue *mb = sum ? json_at(json_get(json_get(sum, "mass"), "bodies"), 0) : NULL;
    if (mb)
        fprintf(f, "| Mass | %.4g g | of the meshed volume, at %.0f kg/m3 |\n",
                json_get_num(mb, "mass_mesh_kg", 0) * 1e3, json_get_num(mb, "density_kg_m3", 0));
    fprintf(f, "\n## The setup\n\n");
    char vol[64];
    if (s->closed_solid || s->volume_mm3 > 0) snprintf(vol, sizeof vol, "%.4g mm3", s->volume_mm3);
    else snprintf(vol, sizeof vol, "volume not available until repaired"); /* an open surface encloses nothing */
    fprintf(f, "- Part: %s, %.4g x %.4g x %.4g mm, %s, %u triangles, %s.\n", s->body, s->size_mm[0], s->size_mm[1],
            s->size_mm[2], vol, s->triangles, s->closed_solid ? "closed solid" : "not a closed solid");
    fprintf(f, "- Material: %s (%s).\n", s->material_name[0] ? s->material_name : "none",
            s->material_status[0] ? s->material_status : "unknown provenance");
    const JsonValue *model = sum ? json_get(sum, "model") : NULL;
    const JsonValue *esz = model ? json_get(model, "element_size_mm") : NULL;
    const JsonValue *e0 = esz ? json_at(esz, 0) : NULL;
    fprintf(f, "- Mesh: %.3g mm hexahedra, %lld elements (%s), %+.2f %% volume against the STL.\n",
            e0 && e0->type == JSON_NUMBER ? e0->u.number : fem_project_mesh_mm(),
            model ? json_get_int(model, "elements", s->nelems) : s->nelems,
            model ? json_get_str(model, "formulation", "hex8") : "hex8", s->mesh_volume_error_pct);
    const JsonValue *conds = fem_conditions();
    for (size_t i = 0; i < json_len(conds); i++) {
        const JsonValue *c = json_at(conds, i);
        fprintf(f, "- Condition `%s`: %s on `%s`.\n", json_get_str(c, "name", ""), json_get_str(c, "kind", ""),
                json_get_str(c, "selection", "the whole body"));
    }
    fprintf(f, "\n## The checks the solver ran\n\n");
    if (ck) {
        fprintf(f, "- Equilibrium: %s (applied loads plus reactions should vanish).\n",
                json_get_bool(ck, "equilibrium_ok", false) ? "balanced" : "OFF - do not use these numbers");
        fprintf(f, "- Energy: %s (twice the strain energy equals the external work).\n",
                json_get_bool(ck, "energy_ok", false) ? "consistent" : "OFF - do not use these numbers");
    }
    fprintf(f, "\n## Mesh convergence\n\n");
    if (g_conv.state == CONV_DONE)
        fprintf(f, "%s. Element size %.3g mm (%d elements) against %.3g mm (%d elements): displacement %.4g -> %.4g mm "
                   "(%+.1f %%), 99th-percentile stress %.4g -> %.4g MPa (%+.1f %%).%s\n",
                g_conv.converged ? "Converged" : "NOT converged", g_conv.coarse_mm, g_conv.coarse_elements,
                g_conv.fine_mm, g_conv.fine_elements, g_conv.coarse_disp, g_conv.fine_disp, g_conv.disp_change_pct,
                g_conv.coarse_p99, g_conv.fine_p99, g_conv.stress_change_pct,
                g_conv.capped ? " The finer mesh was capped at about 60 000 elements." : "");
    else
        fprintf(f, "Not run. Press CHECK MESH in the application: an answer from a single mesh is not evidence.\n");
    fprintf(f, "\n## What these numbers are not\n\n");
    fprintf(f, "- Small-strain linear elasticity: no plasticity, no contact, no large deflection, no buckling.\n");
    fprintf(f, "- The mesh is a voxel mesh: the surface is a staircase, so stresses on inclined and curved faces are "
               "approximate and a peak on a corner or at a support is a property of the mesh, not of the part.\n");
    if (!strcmp(s->material_status, "demonstration"))
        fprintf(f, "- The material values are demonstration data: not traceable to a grade, a supplier or a test. "
                   "The safety factor above is an example, not a design value.\n");
    fprintf(f, "- One load case, applied statically, at one temperature.\n");
    fprintf(f, "\n## Files here\n\n- `report.md` (this file)\n- `stress.png`%s\n- `displacement.png`%s\n"
               "- `view.png`: the application window as it stood, including any section\n"
               "- `results.vtu`, `nodes.csv`, `summary.json` from results_export\n",
            img1 ? "" : " (not written)", img2 ? "" : " (not written)");
    fclose(f);
    if (dir_out) str_copy(dir_out, cap, dir);
    LOGOK("report written to %s", dir);
    return true;
}

/* ---- picking faces in the 3D view --------------------------------------------------------------------------- */

/* the ray under the cursor, in the build frame and millimetres, the way the operation wants it */
static bool cursor_ray(float px, float py, double origin_mm[3], double dir[3]) {
    mat4 to_world, to_build;
    if (!B.have_place || !fem_world_transform(&to_world, &to_build)) return false;
    vec3 o, d;
    camera_ray(&app.cam, px * app.scale, py * app.scale, &o, &d);
    vec3 ob = m4_mul_point(to_build, o);
    vec3 db = m4_mul_dir(to_build, d);
    double ro[3] = {ob.x, ob.y, ob.z}, rd[3] = {db.x, db.y, db.z};
    /* in the exploded view the ray meets a piece where it has been moved to: find which piece it enters first and
     * take that displacement out again, so picking and probing answer about the part, not about the picture */
    if (B.explode > 0 && B.npieces > 1) {
        double best = 1e300, pick[3] = {0, 0, 0};
        for (int i = 0; i < B.npieces; i++) {
            double off[3];
            piece_offset_build(i, off);
            double t0 = -1e300, t1 = 1e300;
            bool hit = true;
            for (int a = 0; a < 3 && hit; a++) {
                double lo = B.piece_lo_m[i][a] + off[a], hi = B.piece_hi_m[i][a] + off[a];
                if (fabs(rd[a]) < 1e-12) {
                    if (ro[a] < lo || ro[a] > hi) hit = false;
                    continue;
                }
                double ta = (lo - ro[a]) / rd[a], tb = (hi - ro[a]) / rd[a];
                if (ta > tb) {
                    double t = ta;
                    ta = tb, tb = t;
                }
                t0 = ta > t0 ? ta : t0, t1 = tb < t1 ? tb : t1;
                if (t0 > t1) hit = false;
            }
            if (hit && t1 > 0 && t0 < best) best = t0, memcpy(pick, off, sizeof pick);
        }
        if (best < 1e299)
            for (int a = 0; a < 3; a++) ro[a] -= pick[a];
    }
    origin_mm[0] = ro[0] * 1e3, origin_mm[1] = ro[1] * 1e3, origin_mm[2] = ro[2] * 1e3;
    dir[0] = rd[0], dir[1] = rd[1], dir[2] = rd[2];
    return true;
}

/* one ray cast through the operation layer; returns the patch id or -1 */
static int pick_patch(float px, float py, double point_mm[3], double normal[3], double *area_mm2) {
    double o[3], d[3];
    if (!cursor_ray(px, py, o, d)) return -1;
    OpResult r;
    OpCaller caller = {"ui", "navier-interface"};
    JsonValue *p = json_object();
    json_set(p, "origin_mm", json_vec3(o[0], o[1], o[2]));
    json_set(p, "direction", json_vec3(d[0], d[1], d[2]));
    ops_invoke(app.engine, "geometry_pick", p, &caller, &r);
    json_free(p);
    int patch = -1;
    if (r.ok && r.value && json_get_bool(r.value, "hit", false)) {
        patch = (int)json_get_int(r.value, "patch", -1);
        if (point_mm) json_get_numbers(json_get(r.value, "point_mm"), point_mm, 3);
        const JsonValue *pi = json_get(r.value, "patch_info");
        if (pi) {
            if (normal) json_get_numbers(json_get(pi, "normal"), normal, 3);
            if (area_mm2) *area_mm2 = json_get_num(pi, "area_mm2", 0);
        }
    }
    op_result_free(&r);
    return patch;
}

void fem_hover_at(float px, float py) {
    if (!app.engine || !B.st.have_project || B.st.nbodies == 0) return;
    if (app.time < B.hover_next || (fabsf(px - B.hover_px) < 2 && fabsf(py - B.hover_py) < 2)) return;
    B.hover_next = app.time + 0.05; /* a ray per frame would lock the engine sixty times a second for nothing */
    B.hover_px = px, B.hover_py = py;
    double t0 = now_seconds();
    int patch = pick_patch(px, py, NULL, NULL, NULL);
    B.st.pick_ms = (now_seconds() - t0) * 1e3;
    if (patch != B.hover_patch) {
        B.hover_patch = patch;
        B.surface_dirty = true;
    }
}

int fem_hover_patch(void) { return B.hover_patch; }
int fem_picked_count(void) { return B.npicked; }
double fem_picked_area_mm2(void) { return B.picked_area_mm2; }
const double *fem_picked_normal(void) { return B.picked_normal; }

void fem_clear_pick(void) {
    B.npicked = 0;
    B.picked_area_mm2 = 0;
    B.picked_normal[0] = B.picked_normal[1] = B.picked_normal[2] = 0;
    B.surface_dirty = true;
}

/* the picked patches as a selection query the engine understands; also gives back its area and normal */
static bool picked_selection(const char *name, bool describe) {
    if (B.npicked <= 0) return false;
    char q[4096] = "[";
    for (int i = 0; i < B.npicked; i++)
        snprintf(q + strlen(q), sizeof q - strlen(q), "%s%d", i ? ", " : "", B.picked[i]);
    str_copy(q + strlen(q), sizeof q - strlen(q), "]");
    if (!fem_op("selection_create",
                "{\"name\": \"%s\", \"query\": {\"patches\": %s}, \"source\": \"user\", \"replace\": true,"
                " \"description\": \"picked in the 3D view\"}",
                name, q))
        return false;
    if (describe) {
        const JsonValue *sel = json_get(fem_last_value(), "selection");
        B.picked_area_mm2 = sel ? json_get_num(sel, "area_mm2", 0) : 0;
        if (sel) json_get_numbers(json_get(sel, "normal"), B.picked_normal, 3);
    }
    return true;
}

/* Pick by dragging a rectangle: every patch whose centre falls inside it is collected. There is no occlusion test,
 * so a box takes the faces behind the part as well as the ones in front; that is the rule, and the panel says so. */
int fem_pick_box(float x0, float y0, float x1, float y1, bool remove, bool front_only) {
    if (!app.engine || B.st.nbodies == 0 || !B.have_place) return 0;
    float lox = MINI(x0, x1), hix = MAXI(x0, x1), loy = MINI(y0, y1), hiy = MAXI(y0, y1);
    if (hix - lox < 4 || hiy - loy < 4) return 0;
    double t0 = now_seconds();
    int added = 0, dropped = 0, full = 0, inside_box = 0;
    engine_lock(app.engine);
    Project *pj = engine_project_locked(app.engine);
    Body *b = pj ? project_body(pj, NULL) : NULL;
    if (b && b->build_v && b->patches.n > 0) {
        for (int p = 0; p < b->patches.n; p++) {
            PatchGeom g;
            patch_geometry(&b->surf, b->build_v, NULL, &b->patches, p, &g);
            if (g.ntri <= 0) continue;
            vec3 w = v3((float)(B.world_centre.x + B.to_world_scale * (g.centroid[0] - B.centre_m[0])),
                        (float)(B.world_centre.y + B.to_world_scale * (g.centroid[2] - B.centre_m[2])),
                        (float)(B.world_centre.z - B.to_world_scale * (g.centroid[1] - B.centre_m[1])));
            if (front_only) { /* only the faces the camera can see, for a box over a part with two sides */
                vec3 nw = v3((float)g.normal[0], (float)g.normal[2], (float)-g.normal[1]);
                if (v3_dot(nw, app.cam.forward) >= 0) continue;
            }
            float sx, sy;
            if (!camera_project(&app.cam, w, &sx, &sy)) continue;
            sx /= (float)app.scale, sy /= (float)app.scale; /* the camera works in pixels, the mouse in points */
            if (sx < lox || sx > hix || sy < loy || sy > hiy) continue;
            inside_box++;
            int at = -1;
            for (int i = 0; i < B.npicked; i++)
                if (B.picked[i] == p) at = i;
            if (remove) {
                if (at >= 0) {
                    for (int i = at; i + 1 < B.npicked; i++) B.picked[i] = B.picked[i + 1];
                    B.npicked--;
                    dropped++;
                }
            } else if (at < 0) {
                if (B.npicked < (int)(sizeof B.picked / sizeof B.picked[0])) B.picked[B.npicked++] = p, added++;
                else full++;
            }
        }
    }
    engine_unlock(app.engine);
    B.st.pick_ms = (now_seconds() - t0) * 1e3;
    if (!added && !dropped) {
        LOGI(inside_box ? "every face inside the box was already picked" : "no face centre fell inside the box");
        return 0;
    }
    B.surface_dirty = true;
    if (B.npicked > 0) picked_selection("pick", true);
    else B.picked_area_mm2 = 0; /* the selection itself is left where it is, as a single-click clear does */
    if (full)
        LOGW("the box holds more faces than one selection carries: %d were left out", full);
    LOGI("box %s %d face%s%s \xC2\xB7 %d picked \xC2\xB7 %.4g mm\xC2\xB2 \xC2\xB7 %.1f ms", remove ? "dropped" : "took",
         remove ? dropped : added, (remove ? dropped : added) == 1 ? "" : "s", front_only ? " (front only)" : "",
         B.npicked, B.picked_area_mm2, B.st.pick_ms);
    return remove ? dropped : added;
}

bool fem_pick_at(float px, float py, bool remove) {
    if (!app.engine || B.st.nbodies == 0) return false;
    double t0 = now_seconds();
    int patch = pick_patch(px, py, NULL, NULL, NULL);
    B.st.pick_ms = (now_seconds() - t0) * 1e3;
    if (patch < 0) {
        LOGI("nothing under the cursor - click on the part");
        return false;
    }
    int at = -1;
    for (int i = 0; i < B.npicked; i++)
        if (B.picked[i] == patch) at = i;
    if (remove || at >= 0) { /* shift-click, or clicking a face that is already in the set, takes it out */
        if (at >= 0) {
            for (int i = at; i + 1 < B.npicked; i++) B.picked[i] = B.picked[i + 1];
            B.npicked--;
        }
    } else if (B.npicked < (int)ARRAY_LEN(B.picked)) {
        B.picked[B.npicked++] = patch;
    } else {
        LOGW("that is as many faces as one condition takes here (%d)", (int)ARRAY_LEN(B.picked));
        return false;
    }
    B.surface_dirty = true;
    if (B.npicked == 0) {
        fem_clear_pick();
        LOGI("nothing picked");
        return true;
    }
    if (!picked_selection("pick", true)) return false;
    LOGI("face %d \xC2\xB7 %d picked \xC2\xB7 %.4g mm\xC2\xB2 \xC2\xB7 %.1f ms", patch, B.npicked, B.picked_area_mm2, B.st.pick_ms);
    return true;
}

bool fem_apply_hold(void) {
    char name[64];
    snprintf(name, sizeof name, "hold_%d", ++B.hold_count);
    if (!picked_selection(name, false)) return false;
    if (!fem_op("boundary_apply", "{\"name\": \"%s\", \"kind\": \"fixed\", \"selection\": \"%s\", \"source\": \"user\","
                                  " \"description\": \"held, picked in the 3D view\"}",
                name, name)) {
        B.hold_count--;
        return false;
    }
    fem_clear_pick();
    return true;
}

bool fem_apply_load(double newtons, int direction) {
    if (!(fabs(newtons) > 0)) {
        LOGW("give the force in newtons before pressing LOAD");
        return false;
    }
    char name[64];
    snprintf(name, sizeof name, "load_%d", ++B.load_count);
    if (!picked_selection(name, false)) {
        B.load_count--;
        return false;
    }
    double v[3] = {0, 0, 0};
    const double *n = B.picked_normal;
    double nl = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (direction <= 1 && nl > 0) {
        double sgn = direction == 0 ? 1.0 : -1.0;
        for (int i = 0; i < 3; i++) v[i] = sgn * newtons * n[i] / nl;
    } else {
        v[CLAMP(direction - 2, 0, 2)] = newtons;
    }
    if (!fem_op("boundary_apply", "{\"name\": \"%s\", \"kind\": \"force\", \"selection\": \"%s\", \"source\": \"user\","
                                  " \"force\": [%.10g, %.10g, %.10g], \"description\": \"load picked in the 3D view\"}",
                name, name, v[0], v[1], v[2])) {
        B.load_count--;
        return false;
    }
    fem_clear_pick();
    return true;
}

bool fem_apply_gravity(void) {
    return fem_op("boundary_apply", "{\"name\": \"gravity\", \"kind\": \"gravity\", \"source\": \"user\","
                                    " \"acceleration\": [0, 0, -9.80665], \"replace\": true,"
                                    " \"description\": \"weight of the part\"}");
}

/* The part itself, drawn from the analysis surface of the body, so an engineer sees what was imported and can click
 * on it long before there is a result. Same world transform as a result, neutral grey through the shader's plate
 * sentinel. */
static bool rebuild_geometry_surface(void) {
    B.st.surface_tris = 0;
    B.surface_valid = false;
    B.surf.nverts = B.surf.nindices = 0;
    if (!app.engine) return false;
    engine_lock(app.engine);
    Project *p = engine_project_locked(app.engine);
    Body *b = p ? project_body(p, NULL) : NULL;
    bool ok = false;
    if (b && b->build_v && b->surf.nt > 0) {
        double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
        for (int v = 0; v < b->surf.nv; v++)
            for (int a = 0; a < 3; a++) {
                double c = b->build_v[3 * (size_t)v + a];
                lo[a] = MINI(lo[a], c), hi[a] = MAXI(hi[a], c);
            }
        double ext = MAXI(MAXI(hi[0] - lo[0], hi[1] - lo[1]), hi[2] - lo[2]);
        if (!(ext > 0)) ext = 1e-3;
        for (int a = 0; a < 3; a++) B.centre_m[a] = 0.5 * (lo[a] + hi[a]);
        double world_ext = MINI(MINI(app.nx, app.ny), app.nz);
        if (world_ext < 1) world_ext = 64;
        B.to_world_scale = 0.55 * world_ext / ext;
        B.world_centre = v3(app.nx * 0.5f, app.ny * 0.5f, app.nz * 0.5f);
        B.have_place = true;
        double clip_z = B.preview >= 0 ? lo[2] + B.preview * (hi[2] - lo[2]) : 1e300;
        uint32_t per = clip_z < 1e299 ? 6 : 3; /* a facet cut by the build height can become two */
        if (reserve_surface(&B.surf, (uint32_t)b->surf.nt * per, (uint32_t)b->surf.nt * per)) {
            B.wlo = v3(1e30f, 1e30f, 1e30f), B.whi = v3(-1e30f, -1e30f, -1e30f);
            for (int t = 0; t < b->surf.nt; t++) {
                if (clip_z < 1e299) { /* the build has reached clip_z: draw what lies below it, cutting facets through */
                    const int *tv = &b->surf.tri[3 * (size_t)t];
                    double P[3][3];
                    int below = 0;
                    for (int k = 0; k < 3; k++) {
                        memcpy(P[k], &b->build_v[3 * (size_t)tv[k]], sizeof P[k]);
                        if (P[k][2] <= clip_z) below++;
                    }
                    if (below == 0) continue;
                    if (below < 3) {
                        /* Sutherland-Hodgman against the plane z = clip_z: at most four corners, one or two facets */
                        double Q[4][3];
                        int nq = 0;
                        for (int k = 0; k < 3; k++) {
                            const double *a = P[k], *c2 = P[(k + 1) % 3];
                            bool ain = a[2] <= clip_z, cin = c2[2] <= clip_z;
                            if (ain) memcpy(Q[nq++], a, sizeof Q[0]);
                            if (ain != cin) {
                                double tt = (clip_z - a[2]) / (c2[2] - a[2]);
                                for (int i = 0; i < 3; i++) Q[nq][i] = a[i] + tt * (c2[i] - a[i]);
                                nq++;
                            }
                        }
                        const double *n0 = b->build_normal ? &b->build_normal[3 * (size_t)t] : NULL;
                        vec3 nr = n0 ? v3((float)n0[0], (float)n0[2], (float)-n0[1]) : v3(0, 1, 0);
                        for (int f = 1; f + 1 < nq; f++) {
                            const double *Tq[3] = {Q[0], Q[f], Q[f + 1]};
                            uint32_t base = B.surf.nverts;
                            if (base + 3 > B.surf.cap_verts) break;
                            for (int k = 0; k < 3; k++) {
                                vec3 pw = v3((float)(B.world_centre.x + B.to_world_scale * (Tq[k][0] - B.centre_m[0])),
                                             (float)(B.world_centre.y + B.to_world_scale * (Tq[k][2] - B.centre_m[2])),
                                             (float)(B.world_centre.z - B.to_world_scale * (Tq[k][1] - B.centre_m[1])));
                                B.wlo = v3_min(B.wlo, pw), B.whi = v3_max(B.whi, pw);
                                float *vp = &B.surf.verts[7 * (size_t)(base + k)];
                                vp[0] = pw.x, vp[1] = pw.y, vp[2] = pw.z;
                                vp[3] = nr.x, vp[4] = nr.y, vp[5] = nr.z;
                                vp[6] = -3e30f;
                                B.surf.indices[B.surf.nindices++] = base + k;
                            }
                            B.surf.nverts += 3;
                        }
                        continue;
                    }
                }
                const double *n = b->build_normal ? &b->build_normal[3 * (size_t)t] : NULL;
                int patch = b->patches.tri_patch ? b->patches.tri_patch[t] : -1;
                float tone = -3e30f; /* grey */
                if (patch >= 0) {
                    for (int i = 0; i < B.npicked; i++)
                        if (B.picked[i] == patch) tone = -2.8e30f; /* chosen */
                    if (tone < -2.9e30f && patch == B.hover_patch) tone = -2.9e30f; /* under the cursor */
                }
                uint32_t base = B.surf.nverts;
                vec3 pw[3];
                for (int k = 0; k < 3; k++) {
                    const double *x = &b->build_v[3 * (size_t)b->surf.tri[3 * (size_t)t + k]];
                    pw[k] = v3((float)(B.world_centre.x + B.to_world_scale * (x[0] - B.centre_m[0])),
                               (float)(B.world_centre.y + B.to_world_scale * (x[2] - B.centre_m[2])),
                               (float)(B.world_centre.z - B.to_world_scale * (x[1] - B.centre_m[1])));
                    B.wlo = v3_min(B.wlo, pw[k]), B.whi = v3_max(B.whi, pw[k]);
                }
                vec3 nr = n ? v3((float)n[0], (float)n[2], (float)-n[1])
                            : v3_norm(v3_cross(v3_sub(pw[1], pw[0]), v3_sub(pw[2], pw[0])));
                for (int k = 0; k < 3; k++) {
                    float *vp = &B.surf.verts[7 * (size_t)(base + k)];
                    vp[0] = pw[k].x, vp[1] = pw[k].y, vp[2] = pw[k].z;
                    vp[3] = nr.x, vp[4] = nr.y, vp[5] = nr.z;
                    vp[6] = tone; /* geometry, not a field: grey, or lit up when hovered or chosen */
                    B.surf.indices[B.surf.nindices++] = base + k;
                }
                B.surf.nverts += 3;
            }
            B.st.surface_tris = (int)(B.surf.nindices / 3);
            B.surface_valid = B.surf.nindices > 0;
            B.surf_lo = 0, B.surf_hi = 1;
            ok = B.surface_valid;
        }
    }
    engine_unlock(app.engine);
    return ok;
}

static void rebuild_surface(void) {
    double t0 = now_seconds();
    B.surface_dirty = false;
    B.surf_generation++;
    B.unsupported[0] = 0;
    B.surface_valid = false;
    B.surf.nverts = B.surf.nindices = 0;
    B.st.surface_tris = 0;
    B.outline_vertices = 0;
    B.st.have_result = false;
    B.st.on_surface = B.st.visibility_mesh = false;
    /* built even while hidden, so the result controls stay on the panel and SHOW brings it straight back */
    if (!app.engine || !B.st.result_job[0]) {
        B.st.showing_geometry = rebuild_geometry_surface();
        B.rebuild_ms = (now_seconds() - t0) * 1e3;
        B.st.rebuild_ms = B.rebuild_ms;
        return;
    }
    B.st.showing_geometry = false;

    char kind[32] = "";
    void *data = acquire_result(B.st.result_job, kind, sizeof kind);
    if (!data) {
        B.st.result_job[0] = 0;
        B.st.showing_geometry = rebuild_geometry_surface();
        return;
    }
    SurfSrc s;
    memset(&s, 0, sizeof s);
    double time_s = 0;
    int nsteps = 1;
    bool has_mech = false;
    if (!field_values(data, kind, B.field, B.step, &s, &time_s, &nsteps, &has_mech)) {
        /* the field does not exist in this result: fall back to one that does */
        for (int alt = 0; alt < FEM_FIELD_COUNT; alt++) {
            if (field_values(data, kind, alt, B.step, &s, &time_s, &nsteps, &has_mech)) { B.field = alt; break; }
        }
    }
    B.st.nsteps = nsteps;
    B.st.time_s = time_s;
    B.st.has_mechanical = has_mech;
    B.st.has_displacement = s.u != NULL;
    B.st.has_temperature = result_kind_of(kind) == RK_TRANSIENT && ((ThermalCase *)data)->T;
    B.st.has_stress = result_kind_of(kind) == RK_STATIC || ((ThermalCase *)data)->mech_vm;
    str_copy(B.st.result_kind, sizeof B.st.result_kind, kind);
    B.step = CLAMP(B.step, 0, nsteps - 1);
    if (!s.value || s.nelems <= 0) {
        free(s.value);
        jobs_release(app.engine->jobs, B.st.result_job);
        return;
    }
    if (strcmp(B.fake_job, B.st.result_job)) {
        free(B.fake_birth); free(B.fake_death); free(B.fake_group);
        B.fake_birth = B.fake_death = NULL; B.fake_group = NULL; B.fake_nelems = 0; B.fake_axis = -1;
    }
    s.step = B.step;
    result_visibility(data, kind, s.nelems, &s.vis);
    if (!build_mesh_cache(s.xyz, s.conn, s.nelems, B.st.result_job, s.elem_type)) {
        free(s.value); jobs_release(app.engine->jobs, B.st.result_job); return;
    }
    s.adj = B.adj;
    B.st.has_groups = s.vis.group != NULL;
    /* place the undeformed part in the viewer's world and pick the deformation factor */
    double bmin[3] = {1e300, 1e300, 1e300}, bmax[3] = {-1e300, -1e300, -1e300};
    double umax = 0;
    for (int n = 0; n < s.nnodes; n++) {
        const double *x = &s.xyz[3 * (size_t)n];
        for (int i = 0; i < 3; i++) {
            if (x[i] < bmin[i]) bmin[i] = x[i];
            if (x[i] > bmax[i]) bmax[i] = x[i];
        }
    }
    bool part_only = true;
    for (int e = 0; e < s.nelems; e++) {
        if (s.vis.group && s.vis.group[e] != 0) part_only = false;
        if (element_shown(&s, e) && s.u) {
            for (int k = 0; k < et_npe(s.elem_type); k++) {
                const double *u = &s.u[3 * (size_t)s.conn[(size_t)et_npe(s.elem_type) * e + k]];
                umax = MAXI(umax, u[0]*u[0]+u[1]*u[1]+u[2]*u[2]);
            }
        }
    }
    umax = sqrt(umax);
    B.st.max_displacement_mm = umax * 1e3;
    double ext = MAXI(MAXI(bmax[0] - bmin[0], bmax[1] - bmin[1]), bmax[2] - bmin[2]);
    if (!(ext > 0)) ext = 1e-3;
    for (int i = 0; i < 3; i++) B.centre_m[i] = 0.5 * (bmin[i] + bmax[i]);
    double world_ext = MINI(MINI(app.nx, app.ny), app.nz);
    if (world_ext < 1) world_ext = 64;
    B.to_world_scale = 0.55 * world_ext / ext;
    B.world_centre = v3(app.nx * 0.5f, app.ny * 0.5f, app.nz * 0.5f);
    B.have_place = true;
    /* AUTO is sized from the largest displacement of the whole run, not of the stored time on screen: a step with
     * almost no movement would otherwise blow the exaggeration up to thousands and the part would jump. */
    double umax_scale = umax;
    bool print_result = !strcmp(kind, "lpbf_build") || !strcmp(kind, "fff_print");
    if (!print_result || !s.u) B.print_fit_valid = false;
    if (result_kind_of(kind) == RK_TRANSIENT && s.u) {
        const ThermalCase *tc = data;
        if (B.deform_gen != B.vis_gen || strcmp(B.deform_job, B.st.result_job) != 0) {
            double big = 0;
            bool complete_in_run = false;
            B.print_fit_valid = false;
            for (int a = 0; a < 3; a++) B.print_fit_lo[a] = 1e300, B.print_fit_hi[a] = -1e300;
            for (int t = 0; t < tc->noutputs; t++) {
                int saved = s.step;
                s.step = t;
                bool complete_at_time = true;
                for (int e = 0; e < s.nelems; e++) {
                    if (!element_shown(&s, e)) { complete_at_time = false; continue; }
                    for (int k = 0; k < 8; k++) {
                        int n = s.conn[8 * (size_t)e + k];
                        const double *u = tc->mech_u + 3 * ((size_t)t * tc->nnodes + n);
                        big = MAXI(big, u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                        if (print_result) for (int a = 0; a < 3; a++) {
                            double x = s.xyz[3 * (size_t)n + a];
                            B.print_fit_lo[a] = MINI(B.print_fit_lo[a], x);
                            B.print_fit_hi[a] = MAXI(B.print_fit_hi[a], x);
                        }
                    }
                }
                complete_in_run = complete_in_run || complete_at_time;
                s.step = saved;
            }
            /* A smooth mapped skin can extend beyond the staircase even with zero displacement. Include the
             * source vertices in the cached envelope if any stored state can show that skin. Support and plate
             * meshes have geometry absent from the source part, so they always use the FE boundary instead. */
            if (print_result && part_only && complete_in_run && !s.elem_type) {
                engine_lock(app.engine);
                Project *pj = engine_project_locked(app.engine);
                const Body *fit_body = pj ? project_body(pj, NULL) : NULL;
                if (fit_body && build_surface_map(&s, fit_body)) {
                    for (int v = 0; v < fit_body->surf.nv; v++) {
                        if (B.map_elem[v] < 0) continue;
                        for (int a = 0; a < 3; a++) {
                            double x = fit_body->build_v[3 * (size_t)v + a];
                            B.print_fit_lo[a] = MINI(B.print_fit_lo[a], x);
                            B.print_fit_hi[a] = MAXI(B.print_fit_hi[a], x);
                        }
                    }
                }
                engine_unlock(app.engine);
            }
            B.deform_umax_all = sqrt(big);
            B.print_fit_valid = print_result && B.print_fit_lo[0] <= B.print_fit_hi[0];
            B.deform_gen = B.vis_gen;
            str_copy(B.deform_job, sizeof B.deform_job, B.st.result_job);
        }
        umax_scale = B.deform_umax_all;
    }
    B.deform_used = B.deform >= 0 ? B.deform : (umax_scale > 0 ? CLAMP(0.08 * ext / umax_scale, 1.0, 1e7) : 1.0);
    if (!s.u) B.deform_used = 0;

    /* visibility drives the surface: a face is drawn when its element exists now and the one across it does not */
    s.step = B.step;
    result_visibility(data, kind, s.nelems, &s.vis);
    s.adj = B.adj;
    size_t nvis = 0;
    bool complete_topology = true;
    for (int e = 0; e < s.nelems; e++) {
        if (!element_shown(&s, e)) { complete_topology = false; continue; }
        for (int lf = 0; lf < et_nfe(s.elem_type); lf++) {
            int nb = s.adj ? s.adj[6 * (size_t)e + lf] : -1;
            if (face_is_surface(&s, e, nb)) nvis++;
        }
    }
    rebuild_outline(&s, nvis);
    /* A mapped STL facet may span many elements. Testing only its three corner elements cannot detect a kerf or
     * unborn material inside that facet. Until the STL is clipped against the active mesh, only a complete topology
     * can use the original skin; birth/death, sections, groups and topology cuts use the exact visible FE boundary.
     * Generated supports and plates are not necessarily part of that skin, even when every element is shown. */
    bool mapped_topology = complete_topology && part_only;
    B.st.visibility_mesh = !s.elem_type && !mapped_topology;
    /* SURFACE draws the part the engineer drew; VOXELS draws the mesh the numbers came from. The surface needs the
     * body the mesh was made from, so it also falls back when that body is not the one on screen. */
    const Body *body = NULL;
    bool surfaced = false;
    size_t want_verts = nvis * 4, want_indices = nvis * 6;
    if (s.elem_type) want_verts = want_indices = nvis * 12; /* up to four triangles per curved face */
    if (B.surface_view && !s.elem_type && mapped_topology) {
        engine_lock(app.engine);
        Project *pj = engine_project_locked(app.engine);
        body = pj ? project_body(pj, NULL) : NULL;
        if (body && build_surface_map(&s, body)) {
            want_verts = (size_t)body->surf.nt * 3 + nvis * 4;
            want_indices = (size_t)body->surf.nt * 3 + nvis * 6;
        } else body = NULL;
        engine_unlock(app.engine);
    }
    if (want_verts > 0xFFFFFFFFu) want_verts = 0xFFFFFFFFu;
    /* the pieces of what is about to be drawn, and where each of them sits */
    int npieces = 1;
    if (body) {
        engine_lock(app.engine);
        npieces = build_comp_pieces(body);
        engine_unlock(app.engine);
    } else {
        npieces = build_elem_pieces(&s);
    }
    if (npieces > FEM_MAX_DRAW_PARTS) npieces = FEM_MAX_DRAW_PARTS;
    if (npieces != B.npieces)
        for (int i = 0; i < FEM_MAX_DRAW_PARTS; i++) B.piece_opacity[i] = 1.0f;
    B.npieces = npieces;
    for (int i = 0; i < npieces; i++) {
        for (int a = 0; a < 3; a++) B.piece_lo_m[i][a] = 1e300, B.piece_hi_m[i][a] = -1e300;
        if (!body && s.body_name && s.nbodies > 1 && i < s.nbodies) snprintf(B.piece_name[i], sizeof B.piece_name[i], "%s", s.body_name[i]);
        else snprintf(B.piece_name[i], sizeof B.piece_name[i], npieces > 1 ? "piece %d" : "the part", i + 1);
    }
    if (body) {
        engine_lock(app.engine);
        for (int t = 0; t < body->surf.nt; t++) {
            int pc = B.comp_piece && body->surf.comp ? B.comp_piece[body->surf.comp[t]] : 0;
            if (pc < 0 || pc >= npieces) continue;
            for (int k = 0; k < 3; k++) {
                const double *x = body->build_v + 3 * (size_t)body->surf.tri[3 * (size_t)t + k];
                for (int a = 0; a < 3; a++) B.piece_lo_m[pc][a] = MINI(B.piece_lo_m[pc][a], x[a]), B.piece_hi_m[pc][a] = MAXI(B.piece_hi_m[pc][a], x[a]);
            }
        }
        engine_unlock(app.engine);
    } else {
        int npe = et_npe(s.elem_type);
        for (int e = 0; e < s.nelems; e++) {
            int pc = B.elem_piece ? B.elem_piece[e] : 0;
            if (pc < 0 || pc >= npieces) continue;
            for (int k = 0; k < npe; k++) {
                const double *x = &s.xyz[3 * (size_t)s.conn[(size_t)npe * e + k]];
                for (int a = 0; a < 3; a++) B.piece_lo_m[pc][a] = MINI(B.piece_lo_m[pc][a], x[a]), B.piece_hi_m[pc][a] = MAXI(B.piece_hi_m[pc][a], x[a]);
            }
        }
    }
    for (int i = 0; i < npieces; i++)
        for (int a = 0; a < 3; a++) B.piece_home_m[i][a] = 0.5 * (B.piece_lo_m[i][a] + B.piece_hi_m[i][a]);
    if (reserve_surface(&B.surf, (uint32_t)want_verts, (uint32_t)want_indices)) {
        B.surf_lo = 1e30f, B.surf_hi = -1e30f;
        B.wlo = v3(1e30f, 1e30f, 1e30f), B.whi = v3(-1e30f, -1e30f, -1e30f);
        B.tet_peak = -3e38f;
        B.peak_valid = false;
        memset(B.parts, 0, sizeof B.parts);
        for (int pc = 0; pc < npieces; pc++) {
            B.draw_piece = npieces > 1 ? pc : -1;
            uint32_t start = B.surf.nindices;
            if (body) {
                engine_lock(app.engine);
                bool got = emit_stl_surface(&s, body, B.deform_used, &B.surf, &B.surf_lo, &B.surf_hi, &B.wlo, &B.whi);
                surfaced = surfaced || got;
                if (got) emit_cut_faces(&s, B.deform_used, &B.surf, &B.surf_lo, &B.surf_hi, &B.wlo, &B.whi);
                engine_unlock(app.engine);
            }
            if (s.elem_type) {
                /* the mesh is the part's surface: drawn on its own faces, with the undeformed outline kept */
                emit_tet_surface(&s, B.deform_used, &B.surf, &B.surf_lo, &B.surf_hi, &B.wlo, &B.whi);
            } else if (!surfaced) {
                emit_surface(&s, B.deform_used, &B.surf, &B.surf_lo, &B.surf_hi, &B.wlo, &B.whi);
            }
            B.parts[pc].index_start = start;
            B.parts[pc].index_count = B.surf.nindices - start;
            B.parts[pc].opacity = B.piece_opacity[pc];
            piece_offset(pc, B.parts[pc].offset);
            B.parts[pc].centre[0] = (float)(B.world_centre.x + B.to_world_scale * (B.piece_home_m[pc][0] - B.centre_m[0])) + B.parts[pc].offset[0];
            B.parts[pc].centre[1] = (float)(B.world_centre.y + B.to_world_scale * (B.piece_home_m[pc][2] - B.centre_m[2])) + B.parts[pc].offset[1];
            B.parts[pc].centre[2] = (float)(B.world_centre.z - B.to_world_scale * (B.piece_home_m[pc][1] - B.centre_m[1])) + B.parts[pc].offset[2];
        }
        B.draw_piece = -1;
        rebuild_edges(&s, B.deform_used);
        B.st.on_surface = surfaced || s.elem_type;
        B.st.tet_mesh = s.elem_type;
        if (s.elem_type) B.outline_vertices = 0; /* a faceted curved surface turns every facet edge into a "crease" */
        if (B.explode > 0 && B.npieces > 1) B.outline_vertices = 0; /* the ghost belongs where the piece was, not where it is */
        /* the white outline is the undeformed VOXEL boundary; on the part's own surface it is only noise, so it
         * belongs to the VOXELS view, where it says something true about what is drawn */
        if (surfaced) B.outline_vertices = 0;
        if (B.surf_hi < B.surf_lo) B.surf_lo = 0, B.surf_hi = 1;
        B.surface_valid = B.surf.nindices > 0;
        B.st.surface_tris = (int)(B.surf.nindices / 3);
        B.st.have_result = true; /* empty section/group still has usable controls */
        /* one line per result, so an agent (or a test) watching the log sees that the window drew it */
        if (B.surface_valid && strcmp(B.surface_job, B.st.result_job) != 0)
            LOGOK("result surface built for %s (%s): %d triangles, %s", B.st.result_job, kind, B.st.surface_tris,
                  fem_field_label(B.field));
        B.st.peak = fabs(B.surf_hi) >= fabs(B.surf_lo) ? B.surf_hi : B.surf_lo;
        if (B.range_all && strcmp(kind, "static_structural") != 0) {
            if (!B.range_all_valid || B.range_gen != B.vis_gen || B.range_field != B.field ||
                strcmp(B.range_job, B.st.result_job) != 0) {
                double glo, ghi;
                B.range_all_valid = transient_range(data, &s, B.field, &glo, &ghi) && ghi > glo;
                B.range_all_lo = glo, B.range_all_hi = ghi;
                B.range_gen = B.vis_gen, B.range_field = B.field;
                str_copy(B.range_job, sizeof B.range_job, B.st.result_job);
            }
            if (B.range_all_valid) B.surf_lo = (float)B.range_all_lo, B.surf_hi = (float)B.range_all_hi;
        }
        /* One singular corner can own the whole colour bar. Clipped, the bar spans the field the part actually
         * carries; the peak itself is still reported in the panel, with its warning. */
        if (B.range_p99 && B.surf.nverts > 16) {
            float *v = malloc((size_t)B.surf.nverts * sizeof *v);
            if (v) {
                size_t n = 0;
                for (uint32_t i = 0; i < B.surf.nverts; i++) {
                    float t = B.surf.verts[7 * (size_t)i + 6];
                    if (t > -1e30f) v[n++] = t;
                }
                if (n > 16) {
                    qsort(v, n, sizeof *v, cmp_float_asc);
                    float hi = v[(size_t)(0.99 * (double)(n - 1))];
                    if (hi > B.surf_lo) B.surf_hi = hi;
                }
                free(v);
            }
        }
        B.st.range_lo = B.surf_lo, B.st.range_hi = B.surf_hi;
    }
    B.st.snap_max_mm = B.map_snap_max_mm;
    B.st.snap_mean_mm = B.map_snap_mean_mm;
    B.st.snapped_vertices = B.map_snapped;
    B.st.unmapped_vertices = B.map_unmapped;
    B.st.map_ms = B.map_ms;
    free(s.value);
    jobs_release(app.engine->jobs, B.st.result_job);
    B.rebuild_ms = (now_seconds() - t0) * 1e3;
    B.st.rebuild_ms = B.rebuild_ms;
    B.surface_job[0] = 0;
    str_copy(B.surface_job, sizeof B.surface_job, B.st.result_job);
    B.surface_field = B.field, B.surface_step = B.step, B.surface_deform = B.deform;
}

/* ---- topology optimisation (docs/contracts/topology-optimisation.md) -------------------------------------------- */

/* The optimiser runs as a job in the engine, exactly as an analysis does. The app starts it, follows its progress,
 * and when it finishes reads the density of every element back and hides the elements below the threshold, so what
 * stays on screen is the part the optimiser kept. The field is also what EXPORT STL contours at 0.5. */

typedef enum { TOPO_IDLE = 0, TOPO_RUNNING, TOPO_DONE, TOPO_FAILED } TopoState;

static struct {
    TopoState state;
    char job[64];
    double next_poll, progress;
    char stage[160];
    double volume_fraction, radius_m;
    float *density;
    int nelems;
    bool show;
    double threshold;
    double *hist;
    int nhist;
    double compliance, compliance0, reached_vf, seconds;
    int iterations, above_half, passive;
    char message[240];
} g_topo;

static void topo_clear(void) {
    free(g_topo.density);
    free(g_topo.hist);
    memset(&g_topo, 0, sizeof g_topo);
    g_topo.threshold = 0.5;
    g_topo.volume_fraction = 0.3;
    g_topo.radius_m = 0;
}

bool fem_optimize_start(double volume_fraction, double filter_radius_m, int max_iterations) {
    if (!app.engine || !B.st.have_project) return false;
    double keep_vf = volume_fraction, keep_r = filter_radius_m;
    topo_clear();
    g_topo.volume_fraction = keep_vf;
    g_topo.radius_m = keep_r;
    int iters = max_iterations > 0 ? max_iterations : 60;
    bool ok = keep_r > 0 ? fem_op("topology_optimize",
                                  "{\"volume_fraction\": %.4f, \"filter_radius\": \"%.4f mm\", \"max_iterations\": %d, \"label\": \"app\"}",
                                  keep_vf, 1e3 * keep_r, iters)
                         : fem_op("topology_optimize", "{\"volume_fraction\": %.4f, \"max_iterations\": %d, \"label\": \"app\"}", keep_vf, iters);
    if (!ok) {
        g_topo.state = TOPO_FAILED;
        str_copy(g_topo.message, sizeof g_topo.message, B.last_error[0] ? B.last_error : "the optimisation was refused");
        return false;
    }
    str_copy(g_topo.job, sizeof g_topo.job, json_get_str(B.last_value, "job_id", ""));
    g_topo.radius_m = 1e-3 * json_get_num(B.last_value, "filter_radius_mm", 1e3 * keep_r);
    g_topo.state = TOPO_RUNNING;
    g_topo.next_poll = 0;
    snprintf(g_topo.message, sizeof g_topo.message, "optimising %lld elements to %.0f %% volume",
             json_get_int(B.last_value, "elements", 0), 100 * keep_vf);
    LOGOK("topology optimisation started: %s", g_topo.job);
    return true;
}

static void topo_read_result(void) {
    if (!fem_op("topology_result", "{\"job_id\": \"%s\", \"include_density\": true}", g_topo.job)) {
        g_topo.state = TOPO_FAILED;
        str_copy(g_topo.message, sizeof g_topo.message, B.last_error[0] ? B.last_error : "the density field could not be read");
        return;
    }
    const JsonValue *v = B.last_value;
    int n = (int)json_get_int(v, "elements", 0);
    const char *b64 = json_get_str(v, "density_base64", "");
    size_t len = 0;
    unsigned char *raw = b64[0] ? base64_decode(b64, strlen(b64), &len) : NULL;
    if (!raw || n <= 0 || len != (size_t)n * 4) {
        free(raw);
        g_topo.state = TOPO_FAILED;
        str_copy(g_topo.message, sizeof g_topo.message, "the density field came back in a shape this build does not know");
        return;
    }
    free(g_topo.density);
    g_topo.density = malloc((size_t)n * sizeof(float));
    if (!g_topo.density) {
        free(raw);
        g_topo.state = TOPO_FAILED;
        str_copy(g_topo.message, sizeof g_topo.message, "out of memory for the density field");
        return;
    }
    memcpy(g_topo.density, raw, (size_t)n * 4);
    free(raw);
    g_topo.nelems = n;
    g_topo.iterations = (int)json_get_int(v, "iterations", 0);
    g_topo.compliance = json_get_num(v, "compliance_j", 0);
    g_topo.compliance0 = json_get_num(v, "compliance_initial_j", 0);
    g_topo.reached_vf = json_get_num(v, "volume_fraction", 0);
    g_topo.above_half = (int)json_get_int(v, "elements_above_half", 0);
    g_topo.passive = (int)json_get_int(v, "passive_elements", 0);
    g_topo.seconds = json_get_num(v, "seconds", 0);
    const JsonValue *h = json_get(v, "history");
    free(g_topo.hist);
    g_topo.nhist = (int)json_len(h);
    g_topo.hist = g_topo.nhist > 0 ? malloc((size_t)g_topo.nhist * sizeof(double)) : NULL;
    for (int i = 0; g_topo.hist && i < g_topo.nhist; i++) g_topo.hist[i] = json_get_num(json_at(h, i), "compliance_j", 0);
    if (!g_topo.hist) g_topo.nhist = 0;
    g_topo.show = true;
    g_topo.state = TOPO_DONE;
    fem_set_surface_view(false); /* the optimised part is elements, not the STL the part started as */
    snprintf(g_topo.message, sizeof g_topo.message,
             "%d iterations, %d of %d elements kept (%.0f %% of the volume), compliance %.4g J; the grey start of the same volume was %.4g J",
             g_topo.iterations, g_topo.above_half, g_topo.nelems, 100 * g_topo.reached_vf, g_topo.compliance, g_topo.compliance0);
    B.surface_dirty = true;
    B.vis_gen++;
    LOGOK("topology optimisation: %s", g_topo.message);
}

static void topo_poll(void) {
    if (g_topo.state != TOPO_RUNNING || !g_topo.job[0] || app.time < g_topo.next_poll) return;
    g_topo.next_poll = app.time + 0.3;
    if (!fem_op("job_status", "{\"job_id\": \"%s\"}", g_topo.job)) {
        g_topo.state = TOPO_FAILED;
        str_copy(g_topo.message, sizeof g_topo.message, B.last_error);
        return;
    }
    const char *st = json_get_str(B.last_value, "state", "");
    g_topo.progress = json_get_num(B.last_value, "progress", g_topo.progress);
    str_copy(g_topo.stage, sizeof g_topo.stage, json_get_str(B.last_value, "stage", g_topo.stage));
    if (!strcmp(st, "succeeded")) {
        topo_read_result();
    } else if (!strcmp(st, "failed") || !strcmp(st, "cancelled")) {
        g_topo.state = TOPO_FAILED;
        const JsonValue *e = json_get(B.last_value, "error");
        snprintf(g_topo.message, sizeof g_topo.message, "%s", json_get_str(e, "message", st));
        LOGE("topology optimisation %s: %s", st, g_topo.message);
    }
}

/* the element is drawn only while its density is at or above the threshold (the elements the optimiser kept) */
static bool topo_element_kept(const SurfSrc *s, int e) {
    if (!g_topo.show || !g_topo.density || g_topo.state != TOPO_DONE) return true;
    if (s->nelems != g_topo.nelems || e < 0 || e >= g_topo.nelems) return true;
    return g_topo.density[e] >= g_topo.threshold;
}

bool fem_optimize_running(void) { return g_topo.state == TOPO_RUNNING; }
bool fem_optimize_have(void) { return g_topo.state == TOPO_DONE && g_topo.density != NULL; }
double fem_optimize_progress(void) { return g_topo.progress; }
const char *fem_optimize_stage(void) { return g_topo.stage; }
const char *fem_optimize_message(void) { return g_topo.message; }
bool fem_optimize_failed(void) { return g_topo.state == TOPO_FAILED; }
bool fem_optimize_show(void) { return g_topo.show; }
double fem_optimize_volume_fraction(void) { return g_topo.volume_fraction; }
double fem_optimize_radius_mm(void) { return 1e3 * g_topo.radius_m; }
int fem_optimize_iterations(void) { return g_topo.iterations; }
double fem_optimize_compliance(void) { return g_topo.compliance; }
double fem_optimize_compliance_initial(void) { return g_topo.compliance0; }
double fem_optimize_reached_fraction(void) { return g_topo.reached_vf; }
int fem_optimize_kept(void) { return g_topo.above_half; }
int fem_optimize_elements(void) { return g_topo.nelems; }
double fem_optimize_seconds(void) { return g_topo.seconds; }
const double *fem_optimize_history(int *n) {
    *n = g_topo.nhist;
    return g_topo.hist;
}

void fem_optimize_set_show(bool on) {
    if (g_topo.show == on) return;
    g_topo.show = on;
    B.vis_gen++;
    B.surface_dirty = true;
}

void fem_optimize_set_settings(double volume_fraction, double radius_m) {
    g_topo.volume_fraction = CLAMP(volume_fraction, 0.05, 0.95);
    g_topo.radius_m = radius_m > 0 ? radius_m : 0;
}

void fem_optimize_clear(void) {
    topo_clear();
    B.vis_gen++;
    B.surface_dirty = true;
}

/* The surviving part as a closed surface: the density on the voxel grid, contoured at the threshold with the
 * isosurface code the viewer already uses, written as an STL in millimetres so geometry_import reads it back. */
bool fem_export_optimised_stl(const char *path, char *msg, size_t cap) {
    if (!fem_optimize_have()) {
        snprintf(msg, cap, "there is no optimisation result to export");
        return false;
    }
    if (!app.engine || !B.st.result_job[0]) {
        snprintf(msg, cap, "the STL is built on the mesh the result was solved on: solve the part first");
        return false;
    }
    char kind[32] = "";
    void *data = acquire_result(B.st.result_job, kind, sizeof kind);
    if (!data) {
        snprintf(msg, cap, "the result is no longer loaded");
        return false;
    }
    SurfSrc s;
    memset(&s, 0, sizeof s);
    const double *xyz = NULL;
    const int *conn = NULL;
    int nelems = 0;
    bool ok = result_geometry(data, kind, &xyz, &conn, &nelems);
    if (!ok || nelems != g_topo.nelems) {
        jobs_release(app.engine->jobs, B.st.result_job);
        snprintf(msg, cap, "the mesh on screen has %d elements and the optimisation ran on %d: mesh and solve again, then optimise",
                 nelems, g_topo.nelems);
        return false;
    }
    s.xyz = xyz;
    s.conn = conn;
    s.nelems = nelems;
    s.nnodes = 0;
    for (int e = 0; e < nelems; e++)
        for (int k = 0; k < 8; k++)
            if (conn[8 * (size_t)e + k] + 1 > s.nnodes) s.nnodes = conn[8 * (size_t)e + k] + 1;
    CellGrid g;
    if (!grid_build(&s, &g)) {
        jobs_release(app.engine->jobs, B.st.result_job);
        snprintf(msg, cap, "this mesh is not a regular voxel grid, so the density cannot be contoured");
        return false;
    }
    /* one empty cell of padding on every side, so the surface closes around the part */
    int nx = g.dims[0] + 2, ny = g.dims[1] + 2, nz = g.dims[2] + 2;
    size_t n = (size_t)nx * ny * nz;
    float *field = calloc(n, sizeof *field);
    uint8_t *solid = calloc(n, 1);
    if (!field || !solid) {
        free(field), free(solid), grid_free(&g);
        jobs_release(app.engine->jobs, B.st.result_job);
        snprintf(msg, cap, "out of memory for the density grid");
        return false;
    }
    for (int k = 0; k < g.dims[2]; k++)
        for (int j = 0; j < g.dims[1]; j++)
            for (int i = 0; i < g.dims[0]; i++) {
                int e = g.cell[(size_t)i + (size_t)g.dims[0] * ((size_t)j + (size_t)g.dims[1] * k)];
                if (e < 0 || e >= g_topo.nelems) continue;
                field[(size_t)(i + 1) + (size_t)nx * ((size_t)(j + 1) + (size_t)ny * (k + 1))] = g_topo.density[e];
            }
    /* Two elements that meet only along an edge pinch the contour into a non-manifold edge, which no mesher and no
     * printer can read: the surface has no boundary but it is not a solid, and its volume is not defined. Where a
     * 2 x 2 window in any coordinate plane holds two solid cells on one diagonal and two empty ones on the other,
     * the emptier pair's better cell is filled to the threshold, which turns the pinch into a neck. Counted and
     * reported; nothing else about the field is touched. */
    int necks = 0;
    const double thr = g_topo.threshold;
    for (int pass = 0; pass < 3; pass++) {
        int du[3] = {1, 1, (int)nx}, dv[3] = {(int)nx, (int)nx * ny, (int)nx * ny};
        int su = du[pass], sv = dv[pass];
        int lu = pass == 2 ? ny : nx, lv = pass == 0 ? ny : nz;
        int lw = pass == 0 ? nz : (pass == 1 ? ny : nx);
        for (int w = 0; w < lw; w++)
            for (int v = 0; v + 1 < lv; v++)
                for (int u = 0; u + 1 < lu; u++) {
                    size_t o = pass == 0 ? (size_t)u + (size_t)nx * ((size_t)v + (size_t)ny * w)
                             : pass == 1 ? (size_t)u + (size_t)nx * ((size_t)w + (size_t)ny * v)
                                         : (size_t)w + (size_t)nx * ((size_t)u + (size_t)ny * v);
                    float a = field[o], b = field[o + su], c = field[o + sv], d = field[o + su + sv];
                    bool as = a >= thr, bs = b >= thr, cs = c >= thr, ds = d >= thr;
                    if (as && ds && !bs && !cs) {
                        field[b >= c ? o + su : o + sv] = (float)(thr + 1e-3); /* inside is field > iso, so sit just above it */
                        necks++;
                    } else if (bs && cs && !as && !ds) {
                        field[a >= d ? o : o + su + sv] = (float)(thr + 1e-3);
                        necks++;
                    }
                }
    }
    VisGrid vg = {nx, ny, nz, NULL, NULL, NULL, NULL, solid};
    IsoParams ip = {(float)g_topo.threshold, 1, false};
    IsoMesh iso;
    memset(&iso, 0, sizeof iso);
    vis_isosurface(&vg, field, NULL, &ip, &iso, NULL);
    Mesh m;
    mesh_init(&m);
    for (uint32_t t = 0; t + 2 < iso.nindices; t += 3) {
        vec3 p[3];
        for (int c = 0; c < 3; c++) {
            const float *v = iso.verts + 7 * (size_t)iso.indices[t + c];
            float w[3];
            for (int a = 0; a < 3; a++) {
                double lat = v[a] - 1.0; /* undo the padding: lattice cell centres sit on the element centres */
                w[a] = (float)(1e3 * (g.origin[a] + g.h[a] * (lat + 0.5)));
            }
            p[c] = (vec3){w[0], w[1], w[2]};
        }
        mesh_add_tri(&m, p[0], p[1], p[2]);
    }
    /* A contour of voxel densities comes out as a triangle soup: vertices repeated, the odd sliver, and holes
     * where the surface met the edge of the grid. The export repairs it here rather than handing the user a part
     * that imports without a volume, and says what it changed. */
    uint32_t tris = m.tri_count;
    char err[256] = {0};
    bool saved = false;
    if (tris > 0) {
        MeshRepairOptions ro;
        mesh_repair_defaults(&ro);
        ro.keep_largest_component = false; /* an optimiser may leave islands: they are the part, not stray sheets */
        Mesh fixed;
        MeshRepairReport rep;
        memset(&rep, 0, sizeof rep);
        if (mesh_repair(&m, &ro, &fixed, &rep, err, sizeof err)) {
            saved = mesh_save_stl(path, &fixed, err, sizeof err);
            if (saved)
                snprintf(msg, cap,
                         "%u triangles written to %s (millimetres): %d diagonal contact%s filled to a neck, %u vertices "
                         "merged, %u degenerate and %u duplicate faces dropped, %d hole%s filled with %u triangles, open "
                         "edges %u to %u, non-manifold edges %u to %u, %s, volume %.4g mm3",
                         rep.triangles_out, path, necks, necks == 1 ? "" : "s", rep.merged_vertices,
                         rep.degenerate_removed, rep.duplicate_removed, rep.holes_filled,
                         rep.holes_filled == 1 ? "" : "s", rep.triangles_added, rep.open_edges_before,
                         rep.open_edges_after, rep.nonmanifold_edges_before, rep.nonmanifold_edges_after,
                         rep.closed_solid_after ? "closed solid" : "still not a closed solid", rep.volume_after);
            tris = rep.triangles_out;
            mesh_free(&fixed);
        }
    }
    if (!saved) snprintf(msg, cap, "%s", tris ? err : "the threshold left no material to export");
    mesh_free(&m);
    isomesh_free(&iso);
    free(field);
    free(solid);
    grid_free(&g);
    jobs_release(app.engine->jobs, B.st.result_job);
    return saved;
}

void fem_poll(void) {
    if (!app.engine) {
        B.st.have_engine = false;
        return;
    }
    uint64_t ch = engine_change_counter(app.engine);
    if (ch != B.seen_changes || B.force_refresh) {
        B.seen_changes = ch;
        B.force_refresh = false;
        refresh_project();
        B.surface_dirty = true;
    }
    if (app.time >= B.next_job_scan) { /* job_list and job_status take no engine lock */
        B.next_job_scan = app.time + 1.0;
        scan_for_new_jobs();
        refresh_active_jobs();
    }
    if (B.st.job_id[0] && (B.st.job_active || !B.st.job_state[0]) && app.time >= B.next_job_poll) {
        B.next_job_poll = app.time + 0.2;
        refresh_job();
    }
    convergence_finish();
    topo_poll();
    layer_study_finish();
    advance_playback();
    if (B.surface_dirty) rebuild_surface();
    /* the body the result on screen was built on: its run says so, whatever project.json lists */
    const JsonValue *sum = fem_result_summary();
    str_copy(B.st.result_body, sizeof B.st.result_body, sum ? json_get_str(json_get(sum, "model"), "body", "") : "");
}

bool fem_field_available(int f) {
    if (!B.st.have_result && !B.st.result_job[0]) return false;
    bool transient = B.st.result_kind[0] && strcmp(B.st.result_kind, "static_structural") != 0;
    (void)transient;
    if (f == FEM_TEMPERATURE) return B.st.has_temperature;
    if (f == FEM_DISPLACEMENT) return B.st.has_displacement;
    return B.st.has_stress;
}

double fem_step_time(int i) {
    (void)i;
    return B.st.time_s;
}

bool fem_world_transform(mat4 *to_world, mat4 *to_build) {
    if (!B.have_place) return false;
    /* world = centre + scale * (x, z, -y) about the part centre */
    mat4 m = m4_identity();
    float s = (float)B.to_world_scale;
    m.m[0] = s;  m.m[1] = 0;  m.m[2] = 0;   /* column for build x */
    m.m[4] = 0;  m.m[5] = 0;  m.m[6] = -s;  /* build y */
    m.m[8] = 0;  m.m[9] = s;  m.m[10] = 0;  /* build z */
    m.m[12] = B.world_centre.x - s * (float)B.centre_m[0];
    m.m[13] = B.world_centre.y - s * (float)B.centre_m[2];
    m.m[14] = B.world_centre.z + s * (float)B.centre_m[1];
    if (to_world) *to_world = m;
    if (to_build) return m4_invert(m, to_build);
    return true;
}

const IsoMesh *fem_surface(void) { return B.surface_valid && B.visible ? &B.surf : NULL; }

uint64_t fem_surface_generation(void) { return B.surf_generation; }

bool fem_surface_range(float *lo, float *hi) {
    if (!B.surface_valid) return false;
    *lo = B.surf_lo, *hi = B.surf_hi;
    return true;
}

bool fem_world_bounds(vec3 *lo, vec3 *hi) {
    if (!B.surface_valid) return false;
    *lo = B.wlo, *hi = B.whi;
    if (B.print_fit_valid && B.st.nsteps > 1 &&
        (!strcmp(B.st.result_kind, "lpbf_build") || !strcmp(B.st.result_kind, "fff_print"))) {
        /* FIT at a shallow birth step must still fit the later part. The maximum displacement norm bounds every
         * component at every stored time; this changes camera framing only, never the computed surface. */
        double pad = B.deform_umax_all * B.deform_used * B.to_world_scale;
        const double *a = B.print_fit_lo, *b = B.print_fit_hi;
        vec3 full_lo = v3((float)(B.world_centre.x + B.to_world_scale * (a[0] - B.centre_m[0]) - pad),
                          (float)(B.world_centre.y + B.to_world_scale * (a[2] - B.centre_m[2]) - pad),
                          (float)(B.world_centre.z - B.to_world_scale * (b[1] - B.centre_m[1]) - pad));
        vec3 full_hi = v3((float)(B.world_centre.x + B.to_world_scale * (b[0] - B.centre_m[0]) + pad),
                          (float)(B.world_centre.y + B.to_world_scale * (b[2] - B.centre_m[2]) + pad),
                          (float)(B.world_centre.z - B.to_world_scale * (a[1] - B.centre_m[1]) + pad));
        *lo = v3_min(*lo, full_lo), *hi = v3_max(*hi, full_hi);
    }
    /* the exploded pieces stand outside the surface's own box: frame what is on screen, not where it came from */
    if (B.explode > 0 && B.npieces > 1)
        for (int i = 0; i < B.npieces; i++) {
            float off[3];
            piece_offset(i, off);
            vec3 a = v3(B.wlo.x + off[0], B.wlo.y + off[1], B.wlo.z + off[2]);
            vec3 b = v3(B.whi.x + off[0], B.whi.y + off[1], B.whi.z + off[2]);
            *lo = v3_min(*lo, a), *hi = v3_max(*hi, b);
        }
    if (B.explode > 0 && B.npieces > 1) { /* a little air, so the pieces do not touch the edge of the window */
        vec3 d = v3_scale(v3_sub(*hi, *lo), 0.10f);
        *lo = v3_sub(*lo, d), *hi = v3_add(*hi, d);
    }
    return true;
}
