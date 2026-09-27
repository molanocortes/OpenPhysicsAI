/* project.c - bodies, placement, patches, setup entries, JSON views and project.json persistence */
#include "project.h"
#include "../core/sha256.h"
#include "selection.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *PROV_NAMES[PROV_COUNT] = {"user", "inferred", "default", "calibrated"};
static const char *ROLE_NAMES[ROLE_COUNT] = {"part", "support", "build_plate"};

const char *provenance_name(Provenance p) { return p >= 0 && p < PROV_COUNT ? PROV_NAMES[p] : "default"; }

int provenance_from_name(const char *s) {
    for (int i = 0; s && i < PROV_COUNT; i++)
        if (!strcmp(s, PROV_NAMES[i])) return i;
    return -1;
}

const char *body_role_name(BodyRole r) { return r >= 0 && r < ROLE_COUNT ? ROLE_NAMES[r] : "part"; }

int body_role_from_name(const char *s) {
    for (int i = 0; s && i < ROLE_COUNT; i++)
        if (!strcmp(s, ROLE_NAMES[i])) return i;
    return -1;
}

void iso_time_now(char *buf, size_t cap) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void random_hex(char *out, int nbytes) {
    unsigned char b[32] = {0};
    int fd = open("/dev/urandom", O_RDONLY);
    bool ok = fd >= 0 && read(fd, b, (size_t)nbytes) == nbytes;
    if (fd >= 0) close(fd);
    if (!ok) {
        char seed[64];
        snprintf(seed, sizeof seed, "%ld-%ld-%p", (long)time(NULL), (long)getpid(), (void *)out);
        char hex[65];
        sha256_hex_of(seed, strlen(seed), hex);
        memcpy(out, hex, (size_t)(2 * nbytes));
        out[2 * nbytes] = 0;
        return;
    }
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < nbytes; i++) out[2 * i] = digits[b[i] >> 4], out[2 * i + 1] = digits[b[i] & 15];
    out[2 * nbytes] = 0;
}

double length_unit_scale(const char *u) {
    if (!u) return 0;
    if (!strcmp(u, "mm")) return 1e-3;
    if (!strcmp(u, "cm")) return 1e-2;
    if (!strcmp(u, "m")) return 1.0;
    if (!strcmp(u, "um")) return 1e-6;
    if (!strcmp(u, "in")) return 0.0254;
    if (!strcmp(u, "ft")) return 0.3048;
    return 0;
}

bool body_name_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 63 || s[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

Project *project_new(const char *name, const char *dir, const char *description) {
    Project *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    random_hex(p->id, 16);
    snprintf(p->name, sizeof p->name, "%s", name ? name : "project");
    snprintf(p->dir, sizeof p->dir, "%s", dir ? dir : "");
    snprintf(p->description, sizeof p->description, "%s", description ? description : "");
    iso_time_now(p->created, sizeof p->created);
    memcpy(p->modified, p->created, sizeof p->modified);
    return p;
}

void body_free(Body *b) {
    if (!b) return;
    mesh_free(&b->file_mesh);
    surface_free(&b->surf);
    patches_free(&b->patches);
    free(b->comps);
    free(b->build_v);
    free(b->build_normal);
    free(b->build_centroid);
    free(b->build_area);
    if (b->bvh_ready) bvh_free(&b->bvh);
    free(b);
}

void project_free(Project *p) {
    if (!p) return;
    for (int i = 0; i < p->nbodies; i++) body_free(p->bodies[i]);
    free(p->bodies);
    free(p->assumptions);
    for (int i = 0; i < p->nselections; i++) selection_free_data(&p->selections[i]);
    free(p->selections);
    free(p->materials);
    json_free(p->user_materials);
    free(p->bcs);
    free(p->contacts);
    mesh_state_free(&p->mesh);
    free(p);
}

Body *project_body(Project *p, const char *name) {
    if (!p) return NULL;
    if (name) {
        for (int i = 0; i < p->nbodies; i++)
            if (!strcmp(p->bodies[i]->name, name)) return p->bodies[i];
        return NULL;
    }
    for (int i = 0; i < p->nbodies; i++)
        if (p->bodies[i]->role == ROLE_PART) return p->bodies[i];
    return p->nbodies ? p->bodies[0] : NULL;
}

bool project_add_body(Project *p, Body *b) {
    Body **nb = realloc(p->bodies, (size_t)(p->nbodies + 1) * sizeof *nb);
    if (!nb) return false;
    p->bodies = nb;
    p->bodies[p->nbodies++] = b;
    b->geom_revision = ++p->geom_counter;
    return true;
}

bool project_remove_body(Project *p, const char *name) {
    for (int i = 0; i < p->nbodies; i++) {
        if (strcmp(p->bodies[i]->name, name) != 0) continue;
        body_free(p->bodies[i]);
        memmove(p->bodies + i, p->bodies + i + 1, (size_t)(p->nbodies - i - 1) * sizeof *p->bodies);
        p->nbodies--;
        return true;
    }
    return false;
}

void project_note_assumption(Project *p, const char *subject, Provenance src, const char *fmt, ...) {
    Assumption *a = NULL;
    for (int i = 0; i < p->nassumptions && !a; i++)
        if (!strcmp(p->assumptions[i].subject, subject)) a = &p->assumptions[i];
    if (!a) {
        if (p->nassumptions == p->cap_assumptions) {
            int cap = p->cap_assumptions ? 2 * p->cap_assumptions : 16;
            Assumption *na = realloc(p->assumptions, (size_t)cap * sizeof *na);
            if (!na) return;
            p->assumptions = na;
            p->cap_assumptions = cap;
        }
        a = &p->assumptions[p->nassumptions++];
        memset(a, 0, sizeof *a);
    }
    snprintf(a->subject, sizeof a->subject, "%s", subject);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->text, sizeof a->text, fmt, ap);
    va_end(ap);
    a->source = src;
    a->revision = p->revision;
}

void project_clear_assumptions(Project *p, const char *subject) {
    int w = 0;
    size_t n = strlen(subject);
    for (int i = 0; i < p->nassumptions; i++) {
        bool match = strncmp(p->assumptions[i].subject, subject, n) == 0 &&
                     (p->assumptions[i].subject[n] == 0 || p->assumptions[i].subject[n] == '/');
        if (!match) p->assumptions[w++] = p->assumptions[i];
    }
    p->nassumptions = w;
}

/* ---- setup entries ----------------------------------------------------------------------------- */

Selection *project_selection(Project *p, const char *name) {
    for (int i = 0; p && name && i < p->nselections; i++)
        if (!strcmp(p->selections[i].name, name)) return &p->selections[i];
    return NULL;
}

Selection *project_add_selection(Project *p) {
    if (p->nselections == p->cap_selections) {
        int cap = p->cap_selections ? 2 * p->cap_selections : 16;
        Selection *ns = realloc(p->selections, (size_t)cap * sizeof *ns);
        if (!ns) return NULL;
        p->selections = ns;
        p->cap_selections = cap;
    }
    Selection *s = &p->selections[p->nselections++];
    memset(s, 0, sizeof *s);
    return s;
}

bool project_remove_selection(Project *p, const char *name) {
    for (int i = 0; i < p->nselections; i++) {
        if (strcmp(p->selections[i].name, name) != 0) continue;
        selection_free_data(&p->selections[i]);
        memmove(p->selections + i, p->selections + i + 1, (size_t)(p->nselections - i - 1) * sizeof *p->selections);
        p->nselections--;
        return true;
    }
    return false;
}

MaterialAssignment *project_material_for(Project *p, const char *target) {
    for (int i = 0; p && target && i < p->nmaterials; i++)
        if (!strcmp(p->materials[i].target, target)) return &p->materials[i];
    return NULL;
}

BoundaryCondition *project_bc(Project *p, const char *name) {
    for (int i = 0; p && name && i < p->nbcs; i++)
        if (!strcmp(p->bcs[i].name, name)) return &p->bcs[i];
    return NULL;
}

BoundaryCondition *project_add_bc(Project *p) {
    if (p->nbcs == p->cap_bcs) {
        int cap = p->cap_bcs ? 2 * p->cap_bcs : 16;
        BoundaryCondition *nb = realloc(p->bcs, (size_t)cap * sizeof *nb);
        if (!nb) return NULL;
        p->bcs = nb;
        p->cap_bcs = cap;
    }
    BoundaryCondition *b = &p->bcs[p->nbcs++];
    memset(b, 0, sizeof *b);
    return b;
}

bool project_remove_bc(Project *p, const char *name) {
    for (int i = 0; i < p->nbcs; i++) {
        if (strcmp(p->bcs[i].name, name) != 0) continue;
        memmove(p->bcs + i, p->bcs + i + 1, (size_t)(p->nbcs - i - 1) * sizeof *p->bcs);
        p->nbcs--;
        return true;
    }
    return false;
}

ThermalContact *project_contact(Project *p, const char *name) {
    for (int i = 0; p && name && i < p->ncontacts; i++)
        if (!strcmp(p->contacts[i].name, name)) return &p->contacts[i];
    return NULL;
}

ThermalContact *project_add_contact(Project *p) {
    if (p->ncontacts == p->cap_contacts) {
        int cap = p->cap_contacts ? 2 * p->cap_contacts : 8;
        ThermalContact *nc = realloc(p->contacts, (size_t)cap * sizeof *nc);
        if (!nc) return NULL;
        p->contacts = nc;
        p->cap_contacts = cap;
    }
    ThermalContact *c = &p->contacts[p->ncontacts++];
    memset(c, 0, sizeof *c);
    return c;
}

bool project_remove_contact(Project *p, const char *name) {
    for (int i = 0; i < p->ncontacts; i++) {
        if (strcmp(p->contacts[i].name, name) != 0) continue;
        memmove(p->contacts + i, p->contacts + i + 1, (size_t)(p->ncontacts - i - 1) * sizeof *p->contacts);
        p->ncontacts--;
        return true;
    }
    return false;
}

JsonValue *thermal_contact_json(const ThermalContact *c) {
    JsonValue *o = json_object();
    json_set_string(o, "name", c->name);
    json_set_string(o, "body_a", c->body_a);
    json_set_string(o, "body_b", c->body_b);
    json_set_string(o, "model", contact_model_name(c->model));
    if (c->model == CONTACT_CONDUCTANCE) json_set_number(o, "conductance_w_m2k", c->conductance);
    if (c->model == CONTACT_THIN_LAYER) {
        json_set_number(o, "layer_thickness_mm", 1e3 * c->layer_thickness);
        json_set_number(o, "layer_conductivity_w_mk", c->layer_conductivity);
    }
    if (c->description[0]) json_set_string(o, "description", c->description);
    json_set_string(o, "source", provenance_name(c->source));
    return o;
}

static bool contact_from_json(const JsonValue *o, ThermalContact *c) {
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", json_get_str(o, "name", ""));
    snprintf(c->body_a, sizeof c->body_a, "%s", json_get_str(o, "body_a", ""));
    snprintf(c->body_b, sizeof c->body_b, "%s", json_get_str(o, "body_b", ""));
    int m = contact_model_from_name(json_get_str(o, "model", ""));
    if (m < 0 || !c->name[0] || !c->body_a[0] || !c->body_b[0]) return false;
    c->model = (ContactModel)m;
    c->conductance = json_get_num(o, "conductance_w_m2k", 0);
    c->layer_thickness = 1e-3 * json_get_num(o, "layer_thickness_mm", 0);
    c->layer_conductivity = json_get_num(o, "layer_conductivity_w_mk", 0);
    snprintf(c->description, sizeof c->description, "%s", json_get_str(o, "description", ""));
    int src = provenance_from_name(json_get_str(o, "source", "user"));
    c->source = src >= 0 ? (Provenance)src : PROV_USER;
    return true;
}

bool project_mesh_current(Project *p, char *why, size_t cap) {
    if (!project_mesh_current_any(p, why, cap)) return false;
    if (p->mesh.hm.elem_type != 0) {
        if (why)
            snprintf(why, cap, "the current mesh is tetrahedral; this analysis runs on the voxel mesh (mesh_generate with method voxel, the "
                               "default)");
        return false;
    }
    return true;
}

bool project_mesh_current_any(Project *p, char *why, size_t cap) {
    if (!p->mesh.valid) {
        if (why) snprintf(why, cap, p->mesh.has_settings ? "the mesh has not been generated in this session (settings are stored)" : "no mesh has been generated");
        return false;
    }
    for (int i = 0; i < p->mesh.nbodies; i++) {
        Body *b = project_body(p, p->mesh.body_name[i]);
        if (!b) {
            if (why) snprintf(why, cap, "body '%s' was removed after meshing", p->mesh.body_name[i]);
            return false;
        }
        if (b->geom_revision != p->mesh.body_geom_revision[i]) {
            if (why) snprintf(why, cap, "body '%s' changed (geometry or placement) after meshing", b->name);
            return false;
        }
    }
    int parts = 0;
    for (int i = 0; i < p->nbodies; i++) parts += p->bodies[i]->role != ROLE_BUILD_PLATE;
    if (parts != p->mesh.nbodies) {
        if (why) snprintf(why, cap, "bodies were added after meshing");
        return false;
    }
    return true;
}

/* ---- placement --------------------------------------------------------------------------------- */

static void axis_rotation(char axis, double deg, double *m) {
    double c, s;
    double q = deg / 90.0;
    if (fabs(q - nearbyint(q)) < 1e-12) {
        long k = ((long)nearbyint(q) % 4 + 4) % 4;
        c = k == 0 ? 1 : (k == 2 ? -1 : 0);
        s = k == 1 ? 1 : (k == 3 ? -1 : 0);
    } else {
        double a = deg * M_PI / 180.0;
        c = cos(a), s = sin(a);
    }
    double x[9] = {1, 0, 0, 0, c, -s, 0, s, c}, y[9] = {c, 0, s, 0, 1, 0, -s, 0, c}, z[9] = {c, -s, 0, s, c, 0, 0, 0, 1};
    memcpy(m, axis == 'x' ? x : (axis == 'y' ? y : z), sizeof x);
}

static void mat3_mul(const double *a, const double *b, double *r) {
    double t[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) t[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
    memcpy(r, t, sizeof t);
}

bool placement_apply(Body *b, char *err, size_t errlen) {
    static const struct {
        const char *axis;
        double m[9];
    } UP[] = {
        {"+Z", {1, 0, 0, 0, 1, 0, 0, 0, 1}},  {"-Z", {1, 0, 0, 0, -1, 0, 0, 0, -1}}, {"+Y", {1, 0, 0, 0, 0, -1, 0, 1, 0}},
        {"-Y", {1, 0, 0, 0, 0, 1, 0, -1, 0}}, {"+X", {0, 0, -1, 0, 1, 0, 1, 0, 0}},  {"-X", {0, 0, 1, 0, 1, 0, -1, 0, 0}},
    };
    Placement *pl = &b->place;
    double R[9];
    bool found = false;
    for (size_t i = 0; i < sizeof UP / sizeof UP[0]; i++)
        if (!strcmp(UP[i].axis, pl->up_axis)) memcpy(R, UP[i].m, sizeof R), found = true;
    if (!found) {
        if (err) snprintf(err, errlen, "invalid up axis '%s'", pl->up_axis);
        return false;
    }
    double M[9];
    axis_rotation('z', pl->rotate_z_deg, M);
    mat3_mul(M, R, R);
    for (int i = 0; i < pl->nrot; i++) {
        axis_rotation(pl->rot_axis[i], pl->rot_deg[i], M);
        mat3_mul(M, R, R);
    }
    for (int i = 0; i < 9; i++)
        if (fabs(R[i]) < 1e-15) R[i] = 0;
    memcpy(pl->R, R, sizeof R);
    int nv = b->surf.nv, nt = b->surf.nt;
    double *bv = malloc((size_t)(nv ? nv : 1) * 3 * sizeof(double));
    double *bn = malloc((size_t)(nt ? nt : 1) * 3 * sizeof(double));
    double *bc = malloc((size_t)(nt ? nt : 1) * 3 * sizeof(double));
    double *ba = malloc((size_t)(nt ? nt : 1) * sizeof(double));
    if (!bv || !bn || !bc || !ba) {
        free(bv), free(bn), free(bc), free(ba);
        if (err) snprintf(err, errlen, "out of memory");
        return false;
    }
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    double s = b->unit_scale;
    for (int v = 0; v < nv; v++) {
        const double *x = b->surf.v + 3 * (size_t)v;
        for (int k = 0; k < 3; k++) {
            double y = s * (R[3 * k] * x[0] + R[3 * k + 1] * x[1] + R[3 * k + 2] * x[2]);
            bv[3 * (size_t)v + k] = y;
            lo[k] = fmin(lo[k], y);
            hi[k] = fmax(hi[k], y);
        }
    }
    pl->t[0] = pl->position_m[0] - 0.5 * (lo[0] + hi[0]);
    pl->t[1] = pl->position_m[1] - 0.5 * (lo[1] + hi[1]);
    pl->t[2] = pl->z_offset_m - lo[2];
    for (int v = 0; v < nv; v++)
        for (int k = 0; k < 3; k++) bv[3 * (size_t)v + k] += pl->t[k];
    for (int k = 0; k < 3; k++) b->bmin[k] = lo[k] + pl->t[k], b->bmax[k] = hi[k] + pl->t[k];
    for (int t = 0; t < nt; t++) {
        const double *n = b->surf.normal + 3 * (size_t)t;
        for (int k = 0; k < 3; k++) {
            bn[3 * (size_t)t + k] = R[3 * k] * n[0] + R[3 * k + 1] * n[1] + R[3 * k + 2] * n[2];
            bc[3 * (size_t)t + k] = (bv[3 * (size_t)b->surf.tri[3 * t] + k] + bv[3 * (size_t)b->surf.tri[3 * t + 1] + k] + bv[3 * (size_t)b->surf.tri[3 * t + 2] + k]) / 3.0;
        }
        ba[t] = b->surf.area[t] * s * s;
    }
    free(b->build_v), free(b->build_normal), free(b->build_centroid), free(b->build_area);
    b->build_v = bv, b->build_normal = bn, b->build_centroid = bc, b->build_area = ba;
    if (b->bvh_ready) {
        bvh_free(&b->bvh);
        b->bvh_ready = false;
    }
    return true;
}

bool body_ensure_bvh(Body *b) {
    if (b->bvh_ready) return true;
    if (!bvh_build(&b->bvh, b->build_v, b->surf.tri, b->surf.nt)) return false;
    b->bvh_ready = true;
    return true;
}

void body_file_to_build(const Body *b, const double *x, double *out) {
    for (int k = 0; k < 3; k++)
        out[k] = b->unit_scale * (b->place.R[3 * k] * x[0] + b->place.R[3 * k + 1] * x[1] + b->place.R[3 * k + 2] * x[2]) + b->place.t[k];
}

static void file_to_build_mm(const Body *b, const double *x, double *out) {
    body_file_to_build(b, x, out);
    for (int k = 0; k < 3; k++) out[k] *= 1e3;
}

Body *body_load(const char *path, const char *name, const char *units, const SurfaceRepairOptions *repair, uint64_t max_bytes,
                uint32_t max_triangles, char *err, size_t errlen) {
    double scale = length_unit_scale(units);
    if (!(scale > 0)) {
        snprintf(err, errlen, "unknown length unit '%s'", units ? units : "");
        return NULL;
    }
    Body *b = calloc(1, sizeof *b);
    if (!b) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if (!mesh_load_stl_ex(path, &b->file_mesh, &b->stl, max_bytes, max_triangles, err, errlen)) {
        free(b);
        return NULL;
    }
    uint64_t bytes = 0;
    if (!sha256_file(path, b->sha256, &bytes)) {
        snprintf(err, errlen, "cannot read '%s' to hash it", path);
        body_free(b);
        return NULL;
    }
    b->file_bytes = bytes;
    snprintf(b->name, sizeof b->name, "%s", name);
    snprintf(b->units, sizeof b->units, "%s", units);
    b->unit_scale = scale;
    if (repair) b->repair = *repair;
    else surface_repair_defaults(&b->repair);
    if (!surface_build(&b->file_mesh, &b->repair, &b->surf, &b->comps, &b->diag, err, errlen) ||
        !patches_build(&b->surf, PATCH_PLANAR_TOL_DEG, PATCH_FEATURE_ANGLE_DEG, &b->patches, err, errlen)) {
        body_free(b);
        return NULL;
    }
    snprintf(b->place.up_axis, sizeof b->place.up_axis, "+Z");
    b->place.up_source = PROV_DEFAULT;
    if (!placement_apply(b, err, errlen)) {
        body_free(b);
        return NULL;
    }
    return b;
}

/* ---- JSON views -------------------------------------------------------------------------------- */

static JsonValue *vec_mm(const double *m) { return json_vec3(1e3 * m[0], 1e3 * m[1], 1e3 * m[2]); }

static JsonValue *placement_json(const Body *b) {
    const Placement *pl = &b->place;
    JsonValue *o = json_object();
    json_set_string(o, "up_axis", pl->up_axis);
    json_set_string(o, "up_axis_source", provenance_name(pl->up_source));
    json_set_number(o, "rotate_z_deg", pl->rotate_z_deg);
    JsonValue *rots = json_set_array(o, "rotations");
    for (int i = 0; i < pl->nrot; i++) {
        JsonValue *r = json_object();
        char ax[2] = {pl->rot_axis[i], 0};
        json_set_string(r, "axis", ax);
        json_set_number(r, "angle_deg", pl->rot_deg[i]);
        json_push(rots, r);
    }
    double pos[2] = {1e3 * pl->position_m[0], 1e3 * pl->position_m[1]};
    json_set(o, "position_mm", json_numbers(pos, 2));
    json_set_number(o, "z_offset_mm", 1e3 * pl->z_offset_m);
    JsonValue *rm = json_set_array(o, "rotation_matrix");
    for (int i = 0; i < 3; i++) json_push(rm, json_vec3(pl->R[3 * i], pl->R[3 * i + 1], pl->R[3 * i + 2]));
    json_set(o, "translation_mm", vec_mm(pl->t));
    json_set_string(o, "transform", "x_build = R * (unit_scale * x_file) + t");
    return o;
}

JsonValue *body_json(const Body *b, bool detailed) {
    JsonValue *o = json_object();
    json_set_string(o, "name", b->name);
    json_set_string(o, "role", body_role_name(b->role));
    JsonValue *u = json_set_object(o, "units");
    json_set_string(u, "value", b->units);
    json_set_string(u, "source", provenance_name(b->units_source));
    if (b->units_note[0]) json_set_string(u, "note", b->units_note);
    double size[3] = {1e3 * (b->bmax[0] - b->bmin[0]), 1e3 * (b->bmax[1] - b->bmin[1]), 1e3 * (b->bmax[2] - b->bmin[2])};
    json_set(o, "size_mm", json_numbers(size, 3));
    JsonValue *bd = json_set_object(o, "bounds_mm");
    json_set(bd, "min", vec_mm(b->bmin));
    json_set(bd, "max", vec_mm(b->bmax));
    double s3 = b->unit_scale * b->unit_scale * b->unit_scale, s2 = b->unit_scale * b->unit_scale;
    json_set_number(o, "volume_mm3", b->diag.volume * s3 * 1e9);
    json_set_number(o, "surface_area_mm2", b->diag.area * s2 * 1e6);
    json_set_int(o, "triangles", b->surf.nt);
    json_set_int(o, "surface_patches", b->patches.n);
    json_set_bool(o, "closed_solid", b->diag.closed_solid);
    json_set_int(o, "geometry_revision", (long long)b->geom_revision);
    if (detailed) {
        JsonValue *src = json_set_object(o, "source");
        json_set_string(src, "original_path", b->source_path);
        json_set_string(src, "stored_file", b->stored_file);
        json_set_string(src, "sha256", b->sha256);
        json_set_int(src, "bytes", (long long)b->file_bytes);
        json_set_string(src, "format", b->stl.binary ? "binary" : "ascii");
        if (b->stl.binary && b->stl.header[0]) json_set_string(src, "header", b->stl.header);
        if (!b->stl.binary && b->stl.solid_name[0]) json_set_string(src, "solid_name", b->stl.solid_name);
        if (b->stl.trailing_bytes) json_set_int(src, "ignored_trailing_bytes", (long long)b->stl.trailing_bytes);
        json_set(o, "placement", placement_json(b));
        JsonValue *rep = json_set_object(o, "repair_options");
        json_set_bool(rep, "remove_degenerate", b->repair.remove_degenerate);
        json_set_bool(rep, "remove_duplicate_faces", b->repair.remove_duplicate_faces);
        json_set_bool(rep, "orient_outward", b->repair.orient_outward);
    }
    return o;
}

static void add_issue(JsonValue *arr, const char *severity, const char *code, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void add_issue(JsonValue *arr, const char *severity, const char *code, const char *fmt, ...) {
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    JsonValue *o = json_object();
    json_set_string(o, "severity", severity);
    json_set_string(o, "code", code);
    json_set_string(o, "message", msg);
    json_push(arr, o);
}

JsonValue *body_diagnostics_json(const Body *b) {
    const SurfaceDiagnostics *d = &b->diag;
    double mm = b->unit_scale * 1e3;
    JsonValue *o = json_object();
    JsonValue *topo = json_set_object(o, "topology");
    json_set_int(topo, "source_triangles", d->source_triangles);
    json_set_int(topo, "analysis_triangles", b->surf.nt);
    json_set_int(topo, "unique_vertices", d->unique_vertices);
    json_set_int(topo, "merged_duplicate_vertices", d->merged_vertices);
    json_set_number(topo, "weld_tolerance_mm", d->weld_tolerance * mm);
    json_set_int(topo, "open_edges", d->open_edges);
    json_set_int(topo, "nonmanifold_edges", d->nonmanifold_edges);
    json_set_int(topo, "boundary_loops", d->boundary_loops);
    json_set_bool(topo, "watertight", d->watertight);
    json_set_bool(topo, "consistently_oriented", d->consistently_oriented);
    json_set_bool(topo, "closed_solid", d->closed_solid);
    json_set_int(topo, "components", d->components);
    json_set_int(topo, "closed_components", d->closed_components);
    json_set_int(topo, "nested_shells", d->nested_shells);
    json_set_int(topo, "surface_patches", b->patches.n);

    JsonValue *rep = json_set_object(o, "repairs");
    json_set_int(rep, "degenerate_faces_found", d->degenerate);
    json_set_int(rep, "degenerate_faces_removed", d->degenerate_removed);
    json_set_int(rep, "duplicate_faces_found", d->duplicate_faces);
    json_set_int(rep, "duplicate_faces_removed", d->duplicate_removed);
    json_set_int(rep, "opposite_duplicate_faces", d->opposite_duplicates);
    json_set_int(rep, "inconsistent_edges_found", d->inconsistent_edges);
    json_set_int(rep, "faces_flipped_for_consistency", d->flipped_triangles);
    json_set_int(rep, "shells_reoriented", d->flipped_components);
    json_set_int(rep, "non_orientable_components", d->non_orientable_components);
    json_set_string(rep, "holes_filled", "none (no automatic hole filling)");

    if (d->nholes) {
        JsonValue *holes = json_set_array(o, "largest_holes");
        for (int i = 0; i < d->nholes; i++) {
            JsonValue *h = json_object();
            json_set_int(h, "edges", d->holes[i].edges);
            json_set_number(h, "perimeter_mm", d->holes[i].perimeter * mm);
            double c[3];
            file_to_build_mm(b, d->holes[i].centroid, c);
            json_set(h, "centroid_mm", json_numbers(c, 3));
            json_push(holes, h);
        }
    }
    JsonValue *comps = json_set_array(o, "components");
    double s3 = b->unit_scale * b->unit_scale * b->unit_scale;
    for (int c = 0; c < b->surf.ncomp && c < 50; c++) {
        const SurfaceComponent *sc = &b->comps[c];
        JsonValue *co = json_object();
        json_set_int(co, "id", c);
        json_set_int(co, "triangles", sc->ntri);
        json_set_bool(co, "closed", sc->closed);
        json_set_number(co, "area_mm2", sc->area * b->unit_scale * b->unit_scale * 1e6);
        if (sc->closed) json_set_number(co, "enclosed_volume_mm3", sc->volume * s3 * 1e9);
        json_set_int(co, "nesting_depth", sc->depth);
        if (sc->parent >= 0) json_set_int(co, "inside_component", sc->parent);
        if (sc->flipped) json_set_bool(co, "reoriented", true);
        json_push(comps, co);
    }
    if (b->surf.ncomp > 50) json_set_int(o, "components_not_listed", b->surf.ncomp - 50);

    JsonValue *checks = json_set_object(o, "checks");
    if (d->self_intersections >= 0) {
        json_set_int(checks, "self_intersecting_face_pairs", d->self_intersections);
        if (d->self_intersection_samples) {
            JsonValue *pts = json_set_array(checks, "self_intersection_locations_mm");
            for (int i = 0; i < d->self_intersection_samples; i++) {
                double c[3];
                file_to_build_mm(b, d->intersection_points + 3 * i, c);
                json_push(pts, json_numbers(c, 3));
            }
        }
        json_set_string(checks, "self_intersection_scope", "faces sharing a vertex are not tested");
    } else {
        json_set_string(checks, "self_intersections", "not checked");
    }
    if (d->thickness_samples > 0) {
        JsonValue *t = json_set_object(checks, "wall_thickness");
        json_set_int(t, "samples", d->thickness_samples);
        json_set_number(t, "min_mm", d->min_thickness * mm);
        json_set_number(t, "p05_mm", d->thickness_p05 * mm);
        json_set_number(t, "median_mm", d->thickness_median * mm);
        double c[3];
        file_to_build_mm(b, d->min_thickness_at, c);
        json_set(t, "min_location_mm", json_numbers(c, 3));
        json_set_string(t, "method", "inward rays from area-stratified surface points to the opposite surface");
    } else {
        json_set_string(checks, "wall_thickness", "not checked");
    }

    JsonValue *issues = json_set_array(o, "issues");
    if (d->open_edges)
        add_issue(issues, "error", "OPEN_BOUNDARY",
                  "%u open edges in %u boundary loops%s: the surface does not enclose a volume and cannot be volume-meshed; no automatic hole filling is applied",
                  d->open_edges, d->boundary_loops, d->nholes ? " (see largest_holes)" : "");
    if (d->nonmanifold_edges)
        add_issue(issues, "error", "NON_MANIFOLD_EDGES", "%u edges are shared by more than two faces (touching or overlapping shells)", d->nonmanifold_edges);
    if (d->non_orientable_components)
        add_issue(issues, "error", "NON_ORIENTABLE", "%u shells cannot be oriented consistently", d->non_orientable_components);
    if (d->watertight && !(d->volume > 0))
        add_issue(issues, "error", "NON_POSITIVE_VOLUME", "the closed surface encloses no positive volume (%.6g mm^3)", d->volume * s3 * 1e9);
    if (d->self_intersections > 0)
        add_issue(issues, "error", "SELF_INTERSECTIONS", "%d pairs of faces intersect; the solid is ambiguous where shells overlap", d->self_intersections);
    if (d->degenerate)
        add_issue(issues, d->degenerate_removed ? "info" : "warning", "DEGENERATE_FACES", "%u zero-area or collapsed faces%s", d->degenerate,
                  d->degenerate_removed ? " were removed from the analysis copy" : " were kept");
    if (d->duplicate_faces)
        add_issue(issues, d->duplicate_removed ? "info" : "warning", "DUPLICATE_FACES", "%u repeated faces%s", d->duplicate_faces,
                  d->duplicate_removed ? " were removed from the analysis copy" : " were kept");
    if (d->opposite_duplicates)
        add_issue(issues, "warning", "INTERNAL_DOUBLE_WALLS", "%u faces appear twice with opposite orientation (zero-thickness internal walls)", d->opposite_duplicates);
    if (d->inconsistent_edges)
        add_issue(issues, d->flipped_triangles ? "info" : "warning", "INCONSISTENT_WINDING", "%u edges had inconsistent face winding%s",
                  d->inconsistent_edges, d->flipped_triangles ? "; faces were reoriented in the analysis copy" : "");
    if (d->flipped_components)
        add_issue(issues, "info", "REORIENTED_SHELLS", "%d shells had inward-facing normals and were reversed in the analysis copy", d->flipped_components);
    if (d->components > 1)
        add_issue(issues, "warning", "MULTIPLE_SHELLS",
                  "%d disconnected shells (%d closed, %d nested inside others): separate solids each need their own support or constraints",
                  d->components, d->closed_components, d->nested_shells);
    return o;
}

JsonValue *units_plausibility_json(const Body *b) {
    static const char *UNITS[] = {"mm", "cm", "m", "in", "ft", "um"};
    double ext = 0;
    for (int k = 0; k < 3; k++) ext = fmax(ext, b->diag.bmax[k] - b->diag.bmin[k]);
    JsonValue *o = json_object();
    json_set_number(o, "largest_extent_in_file_units", ext);
    json_set_string(o, "declared_unit", b->units);
    double declared_mm = ext * b->unit_scale * 1e3;
    json_set_number(o, "largest_extent_mm", declared_mm);
    bool plausible = declared_mm >= 1.0 && declared_mm <= 1000.0;
    json_set_bool(o, "plausible", plausible);
    json_set_string(o, "criterion", "largest extent between 1 mm and 1000 mm (typical additive manufacturing parts)");
    JsonValue *alts = json_set_array(o, "if_unit_were");
    const char *alt_ok = NULL;
    double alt_mm = 0;
    for (size_t i = 0; i < sizeof UNITS / sizeof UNITS[0]; i++) {
        double mmv = ext * length_unit_scale(UNITS[i]) * 1e3;
        JsonValue *a = json_object();
        json_set_string(a, "unit", UNITS[i]);
        json_set_number(a, "largest_extent_mm", mmv);
        json_set_bool(a, "plausible", mmv >= 1.0 && mmv <= 1000.0);
        json_push(alts, a);
        if (!alt_ok && strcmp(UNITS[i], b->units) && mmv >= 1.0 && mmv <= 1000.0) alt_ok = UNITS[i], alt_mm = mmv;
    }
    char text[400];
    if (plausible)
        snprintf(text, sizeof text, "plausible: with %s the part is %.4g mm across its largest extent", b->units, declared_mm);
    else if (alt_ok)
        snprintf(text, sizeof text, "implausible: with %s the part would be %.4g mm; if the file is in %s it would be %.4g mm - confirm the unit with the user",
                 b->units, declared_mm, alt_ok, alt_mm);
    else
        snprintf(text, sizeof text, "unusual size: %.4g mm across the largest extent with %s", declared_mm, b->units);
    json_set_string(o, "assessment", text);
    const char *hdr = b->stl.binary ? b->stl.header : b->stl.solid_name;
    if (hdr[0]) {
        static const char *HINTS[] = {"mm", "millimet", "inch", "unit", "meter", "metre"};
        char low[81];
        size_t n = strlen(hdr);
        for (size_t k = 0; k <= n && k < sizeof low; k++) low[k] = (char)((hdr[k] >= 'A' && hdr[k] <= 'Z') ? hdr[k] + 32 : hdr[k]);
        low[80] = 0;
        for (size_t i = 0; i < sizeof HINTS / sizeof HINTS[0]; i++) {
            if (strstr(low, HINTS[i])) {
                json_set_string(o, "header_text", hdr);
                json_set_string(o, "header_note", "the file header mentions units; headers are free text and not authoritative");
                break;
            }
        }
    }
    return o;
}

JsonValue *assumptions_json(const Project *p) {
    JsonValue *arr = json_array();
    for (int i = 0; i < p->nassumptions; i++) {
        JsonValue *a = json_object();
        json_set_string(a, "subject", p->assumptions[i].subject);
        json_set_string(a, "text", p->assumptions[i].text);
        json_set_string(a, "source", provenance_name(p->assumptions[i].source));
        json_set_int(a, "revision", (long long)p->assumptions[i].revision);
        json_push(arr, a);
    }
    return arr;
}

JsonValue *project_summary_json(const Project *p) {
    JsonValue *o = json_object();
    json_set_string(o, "id", p->id);
    json_set_string(o, "name", p->name);
    if (p->description[0]) json_set_string(o, "description", p->description);
    json_set_string(o, "directory", p->dir);
    json_set_int(o, "revision", (long long)p->revision);
    json_set_bool(o, "unsaved_changes", p->saved_revision != p->revision);
    json_set_string(o, "created", p->created);
    json_set_string(o, "modified", p->modified);
    json_set_int(o, "bodies", p->nbodies);
    json_set_int(o, "selections", p->nselections);
    json_set_int(o, "material_assignments", p->nmaterials);
    json_set_int(o, "boundary_conditions", p->nbcs);
    json_set_bool(o, "mesh_generated", p->mesh.valid);
    json_set_int(o, "assumptions", p->nassumptions);
    return o;
}

/* ---- persistence ------------------------------------------------------------------------------- */

static JsonValue *bc_to_json(const BoundaryCondition *b) {
    JsonValue *o = json_object();
    json_set_string(o, "name", b->name);
    json_set_string(o, "kind", bc_kind_name(b->kind));
    if (b->selection[0]) json_set_string(o, "selection", b->selection);
    switch (b->kind) {
    case BC_DISPLACEMENT: {
        JsonValue *d = json_set_object(o, "displacement_mm");
        for (int k = 0; k < 3; k++) {
            const char *ax = k == 0 ? "x" : (k == 1 ? "y" : "z");
            json_set(d, ax, b->component[k] ? json_number(1e3 * b->vec[k]) : json_null());
        }
        break;
    }
    case BC_FORCE: json_set(o, "force_n", json_vec3(b->vec[0], b->vec[1], b->vec[2])); break;
    case BC_TRACTION: json_set(o, "traction_mpa", json_vec3(1e-6 * b->vec[0], 1e-6 * b->vec[1], 1e-6 * b->vec[2])); break;
    case BC_PRESSURE: json_set_number(o, "pressure_mpa", 1e-6 * b->magnitude); break;
    case BC_GRAVITY: json_set(o, "acceleration_m_s2", json_vec3(b->vec[0], b->vec[1], b->vec[2])); break;
    case BC_TEMPERATURE: json_set_number(o, "temperature_c", b->magnitude - 273.15); break;
    case BC_HEAT_FLUX: json_set_number(o, "heat_flux_w_m2", b->magnitude); break;
    case BC_CONVECTION:
        json_set_number(o, "convection_w_m2k", b->magnitude);
        json_set_number(o, "ambient_c", b->ambient - 273.15);
        break;
    case BC_RADIATION:
        json_set_number(o, "emissivity", b->magnitude);
        json_set_number(o, "ambient_c", b->ambient - 273.15);
        break;
    case BC_HEAT_SOURCE: json_set_number(o, "power_density_w_m3", b->magnitude); break;
    default: break;
    }
    if (b->body[0]) json_set_string(o, "body", b->body);
    if (b->nschedule > 0) {
        JsonValue *sc = json_set_array(o, "schedule");
        for (int i = 0; i < b->nschedule; i++) {
            JsonValue *e = json_object();
            json_set_number(e, "time_s", b->schedule_t[i]);
            json_set_number(e, "factor", b->schedule_factor[i]);
            json_push(sc, e);
        }
    }
    if (b->description[0]) json_set_string(o, "description", b->description);
    json_set_string(o, "source", provenance_name(b->source));
    if (b->selection_hash[0]) json_set_string(o, "selection_hash", b->selection_hash);
    return o;
}

static bool bc_from_json(const JsonValue *o, BoundaryCondition *b) {
    memset(b, 0, sizeof *b);
    snprintf(b->name, sizeof b->name, "%s", json_get_str(o, "name", ""));
    int kind = bc_kind_from_name(json_get_str(o, "kind", ""));
    if (kind < 0 || !b->name[0]) return false;
    b->kind = (BcKind)kind;
    snprintf(b->selection, sizeof b->selection, "%s", json_get_str(o, "selection", ""));
    double v[3] = {0, 0, 0};
    switch (b->kind) {
    case BC_DISPLACEMENT: {
        const JsonValue *d = json_get(o, "displacement_mm");
        for (int k = 0; k < 3; k++) {
            const JsonValue *c = json_get(d, k == 0 ? "x" : (k == 1 ? "y" : "z"));
            if (c && c->type == JSON_NUMBER) b->component[k] = true, b->vec[k] = 1e-3 * c->u.number;
        }
        break;
    }
    case BC_FORCE:
        if (json_get_numbers(json_get(o, "force_n"), v, 3)) memcpy(b->vec, v, sizeof v);
        break;
    case BC_TRACTION:
        if (json_get_numbers(json_get(o, "traction_mpa"), v, 3))
            for (int k = 0; k < 3; k++) b->vec[k] = 1e6 * v[k];
        break;
    case BC_PRESSURE: b->magnitude = 1e6 * json_get_num(o, "pressure_mpa", 0); break;
    case BC_GRAVITY:
        if (json_get_numbers(json_get(o, "acceleration_m_s2"), v, 3)) memcpy(b->vec, v, sizeof v);
        break;
    case BC_TEMPERATURE: b->magnitude = json_get_num(o, "temperature_c", 20) + 273.15; break;
    case BC_HEAT_FLUX: b->magnitude = json_get_num(o, "heat_flux_w_m2", 0); break;
    case BC_CONVECTION:
        b->magnitude = json_get_num(o, "convection_w_m2k", 0);
        b->ambient = json_get_num(o, "ambient_c", 20) + 273.15;
        break;
    case BC_RADIATION:
        b->magnitude = json_get_num(o, "emissivity", 0);
        b->ambient = json_get_num(o, "ambient_c", 20) + 273.15;
        break;
    case BC_HEAT_SOURCE: b->magnitude = json_get_num(o, "power_density_w_m3", 0); break;
    default: break;
    }
    snprintf(b->body, sizeof b->body, "%s", json_get_str(o, "body", ""));
    const JsonValue *sc = json_get(o, "schedule");
    if (sc && sc->type == JSON_ARRAY) {
        if (json_len(sc) > BC_SCHEDULE_MAX) return false;
        for (size_t i = 0; i < json_len(sc); i++) {
            const JsonValue *e = json_at(sc, i);
            double t = json_get_num(e, "time_s", NAN), f = json_get_num(e, "factor", NAN);
            if (!isfinite(t) || !isfinite(f) || (i == 0 ? t != 0 : !(t > b->schedule_t[i - 1]))) return false;
            b->schedule_t[i] = t, b->schedule_factor[i] = f;
        }
        b->nschedule = (int)json_len(sc);
    }
    snprintf(b->description, sizeof b->description, "%s", json_get_str(o, "description", ""));
    int src = provenance_from_name(json_get_str(o, "source", "user"));
    b->source = src >= 0 ? (Provenance)src : PROV_USER;
    snprintf(b->selection_hash, sizeof b->selection_hash, "%s", json_get_str(o, "selection_hash", ""));
    return true;
}

JsonValue *boundary_condition_json(const BoundaryCondition *b) { return bc_to_json(b); }

JsonValue *project_to_json(const Project *p) {
    JsonValue *o = json_object();
    json_set_string(o, "format", "navier-am-project");
    json_set_int(o, "format_version", 1);
    json_set_string(o, "generator", "NAVIER-AM " "0.3.0");
    json_set_string(o, "id", p->id);
    json_set_string(o, "name", p->name);
    json_set_string(o, "description", p->description);
    json_set_string(o, "created", p->created);
    json_set_string(o, "modified", p->modified);
    json_set_int(o, "revision", (long long)p->revision);
    JsonValue *bodies = json_set_array(o, "bodies");
    for (int i = 0; i < p->nbodies; i++) {
        const Body *b = p->bodies[i];
        JsonValue *bo = json_object();
        json_set_string(bo, "name", b->name);
        json_set_string(bo, "role", body_role_name(b->role));
        JsonValue *src = json_set_object(bo, "source");
        json_set_string(src, "original_path", b->source_path);
        json_set_string(src, "stored_file", b->stored_file);
        json_set_string(src, "sha256", b->sha256);
        json_set_int(src, "bytes", (long long)b->file_bytes);
        json_set_string(src, "format", b->stl.binary ? "binary" : "ascii");
        JsonValue *u = json_set_object(bo, "units");
        json_set_string(u, "value", b->units);
        json_set_string(u, "source", provenance_name(b->units_source));
        json_set_string(u, "note", b->units_note);
        JsonValue *rep = json_set_object(bo, "repair");
        json_set_bool(rep, "remove_degenerate", b->repair.remove_degenerate);
        json_set_bool(rep, "remove_duplicate_faces", b->repair.remove_duplicate_faces);
        json_set_bool(rep, "orient_outward", b->repair.orient_outward);
        json_set_number(rep, "weld_tolerance_rel", b->repair.weld_tolerance_rel);
        JsonValue *pl = json_set_object(bo, "placement");
        json_set_string(pl, "up_axis", b->place.up_axis);
        json_set_string(pl, "up_axis_source", provenance_name(b->place.up_source));
        json_set_number(pl, "rotate_z_deg", b->place.rotate_z_deg);
        JsonValue *rots = json_set_array(pl, "rotations");
        for (int r = 0; r < b->place.nrot; r++) {
            JsonValue *ro = json_object();
            char ax[2] = {b->place.rot_axis[r], 0};
            json_set_string(ro, "axis", ax);
            json_set_number(ro, "angle_deg", b->place.rot_deg[r]);
            json_push(rots, ro);
        }
        double pos[2] = {1e3 * b->place.position_m[0], 1e3 * b->place.position_m[1]};
        json_set(pl, "position_mm", json_numbers(pos, 2));
        json_set_number(pl, "z_offset_mm", 1e3 * b->place.z_offset_m);
        json_push(bodies, bo);
    }
    JsonValue *sels = json_set_array(o, "selections");
    for (int i = 0; i < p->nselections; i++) {
        const Selection *s = &p->selections[i];
        JsonValue *so = json_object();
        json_set_string(so, "name", s->name);
        json_set_string(so, "body", s->body);
        json_set(so, "query", json_clone(s->query));
        json_set_string(so, "mode", s->mode == SEL_MODE_PATCH ? "patch" : "triangle");
        json_set_number(so, "coverage", s->coverage);
        json_set_int(so, "keep_largest", s->keep_largest);
        json_set_string(so, "description", s->description);
        json_set_string(so, "source", provenance_name(s->source));
        json_set_string(so, "set_hash", s->set_hash);
        json_set_int(so, "triangles", s->ntri);
        json_set_number(so, "area_mm2", s->area * 1e6);
        json_push(sels, so);
    }
    JsonValue *mats = json_set_object(o, "materials");
    JsonValue *assign = json_set_array(mats, "assignments");
    for (int i = 0; i < p->nmaterials; i++) {
        JsonValue *a = json_object();
        json_set_string(a, "target", p->materials[i].target);
        json_set_string(a, "material", p->materials[i].material);
        json_set_string(a, "source", provenance_name(p->materials[i].source));
        json_set_string(a, "note", p->materials[i].note);
        json_push(assign, a);
    }
    json_set(mats, "defined", p->user_materials ? json_clone(p->user_materials) : json_array());
    if (p->ncontacts > 0) {
        JsonValue *tc = json_set_array(o, "thermal_contacts");
        for (int i = 0; i < p->ncontacts; i++) json_push(tc, thermal_contact_json(&p->contacts[i]));
    }
    JsonValue *bcs = json_set_array(o, "boundary_conditions");
    for (int i = 0; i < p->nbcs; i++) json_push(bcs, bc_to_json(&p->bcs[i]));
    if (p->mesh.has_settings) {
        JsonValue *m = json_set_object(o, "mesh");
        json_set_string(m, "method", p->mesh.method == 1 ? "tet" : "voxel_hex8");
        json_set(m, "element_size_mm", json_vec3(1e3 * p->mesh.h[0], 1e3 * p->mesh.h[1], 1e3 * p->mesh.h[2]));
        if (p->mesh.method == 1) {
            json_set_int(m, "order", p->mesh.tet_order);
            json_set_number(m, "interior_size_mm", 1e3 * p->mesh.tet_interior);
            json_set_int(m, "thin_wall_levels", p->mesh.tet_thin);
        }
        json_set_bool(m, "include_build_plate", p->mesh.include_plate);
        json_set_number(m, "plate_thickness_mm", 1e3 * p->mesh.plate_thickness);
        json_set_number(m, "plate_margin_mm", 1e3 * p->mesh.plate_margin);
        if (p->mesh.valid) json_set_string(m, "last_mesh_hash", p->mesh.hash);
    }
    json_set(o, "assumptions", assumptions_json(p));
    return o;
}

bool project_save_file(Project *p, char *err, size_t errlen) {
    char path[NV_PATH_MAX];
    if (!p->dir[0] || !path_join(path, sizeof path, p->dir, "project.json")) {
        snprintf(err, errlen, "project has no directory");
        return false;
    }
    JsonValue *j = project_to_json(p);
    size_t n;
    char *text = j ? json_dump(j, JSON_PRETTY, &n, NULL) : NULL;
    json_free(j);
    if (!text) {
        snprintf(err, errlen, "out of memory serialising the project");
        return false;
    }
    bool ok = path_write_file_atomic(path, text, n, err, errlen);
    free(text);
    if (ok) p->saved_revision = p->revision;
    return ok;
}

Project *project_load_file(const char *json_path, uint64_t max_bytes, uint32_t max_triangles, char *err, size_t errlen) {
    JsonError jerr;
    JsonValue *root = json_read_file(json_path, (size_t)64 << 20, &jerr);
    if (!root) {
        snprintf(err, errlen, "cannot read '%s': %s (line %d)", json_path, jerr.message, jerr.line);
        return NULL;
    }
    if (strcmp(json_get_str(root, "format", ""), "navier-am-project") != 0 || json_get_int(root, "format_version", 0) != 1) {
        snprintf(err, errlen, "'%s' is not a navier-am-project version 1 file", json_path);
        json_free(root);
        return NULL;
    }
    char dir[NV_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", json_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    Project *p = project_new(json_get_str(root, "name", "project"), dir, json_get_str(root, "description", ""));
    if (!p) {
        json_free(root);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    const char *id = json_get_str(root, "id", NULL);
    if (id && strlen(id) == 32) snprintf(p->id, sizeof p->id, "%s", id);
    snprintf(p->created, sizeof p->created, "%s", json_get_str(root, "created", p->created));
    snprintf(p->modified, sizeof p->modified, "%s", json_get_str(root, "modified", p->modified));
    p->revision = (uint64_t)json_get_int(root, "revision", 0);
    const JsonValue *bodies = json_get(root, "bodies");
    for (size_t i = 0; i < json_len(bodies); i++) {
        const JsonValue *bo = json_at(bodies, i);
        const JsonValue *src = json_get(bo, "source"), *u = json_get(bo, "units"), *rep = json_get(bo, "repair"), *pl = json_get(bo, "placement");
        const char *name = json_get_str(bo, "name", "");
        const char *stored = json_get_str(src, "stored_file", "");
        if (!body_name_valid(name) || !stored[0] || strstr(stored, "..") || stored[0] == '/') {
            snprintf(err, errlen, "body %zu in the project has an invalid name or stored file", i);
            goto fail;
        }
        char full[NV_PATH_MAX];
        path_join(full, sizeof full, dir, stored);
        SurfaceRepairOptions ro;
        surface_repair_defaults(&ro);
        ro.remove_degenerate = json_get_bool(rep, "remove_degenerate", ro.remove_degenerate);
        ro.remove_duplicate_faces = json_get_bool(rep, "remove_duplicate_faces", ro.remove_duplicate_faces);
        ro.orient_outward = json_get_bool(rep, "orient_outward", ro.orient_outward);
        ro.weld_tolerance_rel = json_get_num(rep, "weld_tolerance_rel", ro.weld_tolerance_rel);
        char berr[512];
        Body *b = body_load(full, name, json_get_str(u, "value", ""), &ro, max_bytes, max_triangles, berr, sizeof berr);
        if (!b) {
            snprintf(err, errlen, "body '%s': %s", name, berr);
            goto fail;
        }
        const char *want = json_get_str(src, "sha256", "");
        if (strcmp(want, b->sha256) != 0) {
            snprintf(err, errlen, "body '%s': stored file %s has SHA-256 %s but the project records %s (the input was modified)", name, stored, b->sha256, want);
            body_free(b);
            goto fail;
        }
        int role = body_role_from_name(json_get_str(bo, "role", "part"));
        b->role = role >= 0 ? (BodyRole)role : ROLE_PART;
        snprintf(b->source_path, sizeof b->source_path, "%s", json_get_str(src, "original_path", ""));
        snprintf(b->stored_file, sizeof b->stored_file, "%s", stored);
        int us = provenance_from_name(json_get_str(u, "source", "user"));
        b->units_source = us >= 0 ? (Provenance)us : PROV_USER;
        snprintf(b->units_note, sizeof b->units_note, "%s", json_get_str(u, "note", ""));
        snprintf(b->place.up_axis, sizeof b->place.up_axis, "%s", json_get_str(pl, "up_axis", "+Z"));
        int ups = provenance_from_name(json_get_str(pl, "up_axis_source", "default"));
        b->place.up_source = ups >= 0 ? (Provenance)ups : PROV_DEFAULT;
        b->place.rotate_z_deg = json_get_num(pl, "rotate_z_deg", 0);
        const JsonValue *rots = json_get(pl, "rotations");
        b->place.nrot = 0;
        for (size_t r = 0; r < json_len(rots) && r < PLACEMENT_MAX_ROTATIONS; r++) {
            const char *ax = json_get_str(json_at(rots, r), "axis", "z");
            b->place.rot_axis[b->place.nrot] = ax[0];
            b->place.rot_deg[b->place.nrot++] = json_get_num(json_at(rots, r), "angle_deg", 0);
        }
        double pos[2] = {0, 0};
        json_get_numbers(json_get(pl, "position_mm"), pos, 2);
        b->place.position_m[0] = 1e-3 * pos[0];
        b->place.position_m[1] = 1e-3 * pos[1];
        b->place.z_offset_m = 1e-3 * json_get_num(pl, "z_offset_mm", 0);
        if (!placement_apply(b, berr, sizeof berr) || !project_add_body(p, b)) {
            snprintf(err, errlen, "body '%s': %s", name, berr);
            body_free(b);
            goto fail;
        }
    }
    const JsonValue *sels = json_get(root, "selections");
    for (size_t i = 0; i < json_len(sels); i++) {
        const JsonValue *so = json_at(sels, i);
        Selection *s = project_add_selection(p);
        if (!s) {
            snprintf(err, errlen, "out of memory");
            goto fail;
        }
        snprintf(s->name, sizeof s->name, "%s", json_get_str(so, "name", ""));
        snprintf(s->body, sizeof s->body, "%s", json_get_str(so, "body", ""));
        s->query = json_clone(json_get(so, "query"));
        s->mode = strcmp(json_get_str(so, "mode", "patch"), "triangle") ? SEL_MODE_PATCH : SEL_MODE_TRIANGLE;
        s->coverage = json_get_num(so, "coverage", 0.5);
        s->keep_largest = (int)json_get_int(so, "keep_largest", 0);
        snprintf(s->description, sizeof s->description, "%s", json_get_str(so, "description", ""));
        int src = provenance_from_name(json_get_str(so, "source", "inferred"));
        s->source = src >= 0 ? (Provenance)src : PROV_INFERRED;
        const char *stored_hash = json_get_str(so, "set_hash", "");
        Body *b = project_body(p, s->body);
        NvErr code;
        char serr[512];
        if (!b || !s->query || !selection_resolve(b, s, NULL, &code, serr, sizeof serr)) {
            s->stale = true;
            snprintf(s->stale_reason, sizeof s->stale_reason, "could not be resolved when the project was opened: %s", b ? serr : "body not found");
        } else if (stored_hash[0] && strcmp(stored_hash, s->set_hash) != 0) {
            s->stale = true;
            snprintf(s->stale_reason, sizeof s->stale_reason, "resolves to different faces than when it was saved");
        }
    }
    const JsonValue *mats = json_get(root, "materials");
    const JsonValue *assign = json_get(mats, "assignments");
    for (size_t i = 0; i < json_len(assign); i++) {
        if (p->nmaterials == p->cap_materials) {
            int cap = p->cap_materials ? 2 * p->cap_materials : 8;
            MaterialAssignment *nm = realloc(p->materials, (size_t)cap * sizeof *nm);
            if (!nm) {
                snprintf(err, errlen, "out of memory");
                goto fail;
            }
            p->materials = nm;
            p->cap_materials = cap;
        }
        MaterialAssignment *m = &p->materials[p->nmaterials++];
        memset(m, 0, sizeof *m);
        const JsonValue *a = json_at(assign, i);
        snprintf(m->target, sizeof m->target, "%s", json_get_str(a, "target", ""));
        snprintf(m->material, sizeof m->material, "%s", json_get_str(a, "material", ""));
        int src = provenance_from_name(json_get_str(a, "source", "user"));
        m->source = src >= 0 ? (Provenance)src : PROV_USER;
        snprintf(m->note, sizeof m->note, "%s", json_get_str(a, "note", ""));
    }
    if (json_get(mats, "defined")) p->user_materials = json_clone(json_get(mats, "defined"));
    const JsonValue *bcs = json_get(root, "boundary_conditions");
    for (size_t i = 0; i < json_len(bcs); i++) {
        BoundaryCondition *b = project_add_bc(p);
        if (!b || !bc_from_json(json_at(bcs, i), b)) {
            snprintf(err, errlen, "boundary condition %zu is invalid", i);
            goto fail;
        }
    }
    const JsonValue *tcs = json_get(root, "thermal_contacts");
    for (size_t i = 0; i < json_len(tcs); i++) {
        ThermalContact *tc = project_add_contact(p);
        if (!tc || !contact_from_json(json_at(tcs, i), tc)) {
            snprintf(err, errlen, "thermal contact %zu is invalid", i);
            goto fail;
        }
    }
    const JsonValue *mesh = json_get(root, "mesh");
    if (mesh) {
        double h[3];
        if (json_get_numbers(json_get(mesh, "element_size_mm"), h, 3)) {
            for (int k = 0; k < 3; k++) p->mesh.h[k] = 1e-3 * h[k];
            p->mesh.include_plate = json_get_bool(mesh, "include_build_plate", false);
            p->mesh.plate_thickness = 1e-3 * json_get_num(mesh, "plate_thickness_mm", 10);
            p->mesh.plate_margin = 1e-3 * json_get_num(mesh, "plate_margin_mm", 5);
            p->mesh.method = !strcmp(json_get_str(mesh, "method", ""), "tet") ? 1 : 0;
            p->mesh.tet_order = (int)json_get_int(mesh, "order", 2);
            p->mesh.tet_interior = 1e-3 * json_get_num(mesh, "interior_size_mm", 4e3 * p->mesh.h[0]);
            p->mesh.tet_thin = (int)json_get_int(mesh, "thin_wall_levels", 2);
            p->mesh.has_settings = true;
            /* hash of the last generated mesh: regenerating from these settings must reproduce it */
            snprintf(p->mesh.hash, sizeof p->mesh.hash, "%s", json_get_str(mesh, "last_mesh_hash", ""));
        }
    }
    const JsonValue *as = json_get(root, "assumptions");
    for (size_t i = 0; i < json_len(as); i++) {
        const JsonValue *a = json_at(as, i);
        int src = provenance_from_name(json_get_str(a, "source", "default"));
        project_note_assumption(p, json_get_str(a, "subject", "?"), src >= 0 ? (Provenance)src : PROV_DEFAULT, "%s", json_get_str(a, "text", ""));
        p->assumptions[p->nassumptions - 1].revision = (uint64_t)json_get_int(a, "revision", 0);
    }
    p->saved_revision = p->revision;
    json_free(root);
    return p;
fail:
    json_free(root);
    project_free(p);
    return NULL;
}
