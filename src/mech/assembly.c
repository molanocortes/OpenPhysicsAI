/* assembly.c - assembly model, native JSON format, STL parts, conversion to a multibody definition (see assembly.h) */
#include "assembly.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../core/sha256.h"
#include "../geom/mesh.h"
#include "../geom/surface.h"
#include "mechunits.h"

static const char *const SRC_NAMES[SRC_COUNT] = {"user", "measured", "cad", "computed", "inferred", "default", "calibrated"};
const char *asm_source_name(AsmSource s) { return (unsigned)s < SRC_COUNT ? SRC_NAMES[s] : "?"; }
int asm_source_from_name(const char *s) {
    for (int i = 0; s && i < SRC_COUNT; i++)
        if (!strcmp(s, SRC_NAMES[i])) return i;
    return -1;
}

static const char *const GEOM_NAMES[AG_COUNT] = {"mesh", "box", "sphere", "cylinder", "capsule", "plane"};
const char *asm_geom_type_name(AsmGeomType t) { return (unsigned)t < AG_COUNT ? GEOM_NAMES[t] : "?"; }

/* ------------------------------------------------------------------------------------------------ model basics */

static bool grow_array(void **p, int *cap, int need, size_t size) {
    if (need <= *cap) return true;
    int nc = *cap ? 2 * *cap : 8;
    while (nc < need) nc *= 2;
    void *q = realloc(*p, (size_t)nc * size);
    if (!q) return false;
    memset((char *)q + (size_t)*cap * size, 0, (size_t)(nc - *cap) * size);
    *p = q, *cap = nc;
    return true;
}

static bool name_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n >= MB_NAME) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return strcmp(s, "world") != 0;
}

Assembly *asm_new(const char *name) {
    Assembly *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    snprintf(a->name, sizeof a->name, "%s", name ? name : "assembly");
    snprintf(a->source_format, sizeof a->source_format, "api");
    mv3_set(a->gravity, 0, 0, -9.80665);
    a->gravity_source = SRC_DEFAULT;
    return a;
}

void asm_free(Assembly *a) {
    if (!a) return;
    for (int i = 0; i < a->nbodies; i++) {
        free(a->bodies[i].geoms);
        free(a->bodies[i].frames);
        json_free(a->bodies[i].manufacturing);
        asm_flexible_free(a->bodies[i].flexible);
    }
    free(a->bodies);
    free(a->joints);
    free(a->couplings);
    free(a->assumptions);
    free(a->unsupported);
    free(a->environment);
    free(a);
}

int asm_body_index(const Assembly *a, const char *name) {
    for (int i = 0; name && i < a->nbodies; i++)
        if (!strcmp(a->bodies[i].name, name)) return i;
    return -1;
}

int asm_joint_index(const Assembly *a, const char *name) {
    for (int i = 0; name && i < a->njoints; i++)
        if (!strcmp(a->joints[i].def.name, name)) return i;
    return -1;
}

AsmBody *asm_add_body(Assembly *a, const char *name) {
    if (asm_body_index(a, name) >= 0) return NULL;
    if (!grow_array((void **)&a->bodies, &a->cap_bodies, a->nbodies + 1, sizeof *a->bodies)) return NULL;
    AsmBody *b = &a->bodies[a->nbodies++];
    memset(b, 0, sizeof *b);
    snprintf(b->name, sizeof b->name, "%s", name);
    mpose_identity(&b->mass_mesh_pose);
    return b;
}

AsmJoint *asm_add_joint(Assembly *a, const char *name, MbJointType type, int parent, int child) {
    if (asm_joint_index(a, name) >= 0) return NULL;
    if (!grow_array((void **)&a->joints, &a->cap_joints, a->njoints + 1, sizeof *a->joints)) return NULL;
    AsmJoint *j = &a->joints[a->njoints++];
    memset(j, 0, sizeof *j);
    MbJointDef *J = &j->def;
    snprintf(J->name, sizeof J->name, "%s", name);
    J->type = type, J->parent = parent, J->child = child;
    mpose_identity(&J->parent_frame);
    mpose_identity(&J->child_frame);
    mv3_set(J->axis, 0, 0, 1);
    if (type == MB_SPHERICAL) J->q0[0] = 1;
    if (type == MB_FREE) J->q0[3] = 1;
    return j;
}

static void add_note(AsmNote **arr, int *n, int *cap, const char *subject, AsmSource src, const char *fmt, va_list ap) {
    AsmNote x;
    memset(&x, 0, sizeof x);
    snprintf(x.subject, sizeof x.subject, "%s", subject ? subject : "");
    vsnprintf(x.text, sizeof x.text, fmt, ap);
    x.source = src;
    for (int i = 0; i < *n; i++) /* notes are a set: the same statement is recorded once */
        if (!strcmp((*arr)[i].subject, x.subject) && !strcmp((*arr)[i].text, x.text) && (*arr)[i].source == x.source) return;
    if (!grow_array((void **)arr, cap, *n + 1, sizeof **arr)) return;
    (*arr)[(*n)++] = x;
}

void asm_note_assumption(Assembly *a, const char *subject, AsmSource src, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    add_note(&a->assumptions, &a->nassumptions, &a->cap_assumptions, subject, src, fmt, ap);
    va_end(ap);
}

void asm_note_unsupported(Assembly *a, const char *subject, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    add_note(&a->unsupported, &a->nunsupported, &a->cap_unsupported, subject, SRC_USER, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------------------------------ JSON reading helpers */

typedef struct Ctx {
    MechDiag *d;
    const char *base_dir;
    const AsmLoadOptions *opt;
    Assembly *a;
    char unit[MQ_COUNT][32];
    int errors;
    bool replace; /* bodies and joints with an existing name are replaced in place */
    bool partial; /* body update: fields that are absent are kept by the caller (no missing mass properties) */
} Ctx;

static void err_at(Ctx *c, const char *path, const char *code, const char *hint, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void err_at(Ctx *c, const char *path, const char *code, const char *hint, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    mdiag_add(c->d, MD_ERROR, code, path, hint, "%s", msg);
    c->errors++;
}

static void missing_at(Ctx *c, const char *path, const char *code, const char *hint, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void missing_at(Ctx *c, const char *path, const char *code, const char *hint, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    mdiag_add(c->d, MD_MISSING_INPUT, code, path, hint, "%s", msg);
    c->errors++;
}

static void check_keys(Ctx *c, const JsonValue *o, const char *path, const char *const *allowed) {
    for (size_t i = 0; i < json_len(o); i++) {
        const char *k = json_key_at(o, i);
        bool ok = false;
        for (int j = 0; allowed[j] && !ok; j++) ok = !strcmp(allowed[j], k);
        if (!ok) mdiag_add(c->d, MD_WARNING, "UNKNOWN_KEY", path, "check the spelling against Mech Sim/docs/assembly-format.md", "unknown key '%s' ignored", k);
    }
}

static bool qty(Ctx *c, const JsonValue *v, MechQty q, const char *path, double *out) {
    char e[256];
    if (!mech_qty_from_json_ex(v, q, c->unit[q][0] ? c->unit[q] : NULL, c->opt && c->opt->require_units, out, e, sizeof e)) {
        err_at(c, path, "INVALID_QUANTITY", NULL, "%s", e);
        return false;
    }
    return true;
}

static bool jvec3(Ctx *c, const JsonValue *v, MechQty q, const char *path, double out[3]) {
    const JsonValue *arr = v;
    char unitbuf[32] = "";
    if (v && v->type == JSON_OBJECT) {
        arr = json_get(v, "value");
        snprintf(unitbuf, sizeof unitbuf, "%s", json_get_str(v, "unit", ""));
    }
    if (!arr || arr->type != JSON_ARRAY || json_len(arr) != 3) {
        err_at(c, path, "INVALID_VECTOR", "give three components, e.g. [0, 0, 12.5] or [\"0 mm\", \"0 mm\", \"12.5 mm\"]", "expected a 3-vector");
        return false;
    }
    for (int k = 0; k < 3; k++) {
        const JsonValue *e = json_at(arr, (size_t)k);
        char ebuf[256];
        const char *du = unitbuf[0] ? unitbuf : (c->unit[q][0] ? c->unit[q] : NULL);
        if (!mech_qty_from_json_ex(e, q, du, c->opt && c->opt->require_units, &out[k], ebuf, sizeof ebuf)) {
            err_at(c, path, "INVALID_QUANTITY", NULL, "component %d: %s", k, ebuf);
            return false;
        }
    }
    return true;
}

static bool unit_vector(Ctx *c, const JsonValue *v, const char *path, double out[3]) {
    char saved[32];
    memcpy(saved, c->unit[MQ_DIMENSIONLESS], sizeof saved);
    bool ok = jvec3(c, v, MQ_DIMENSIONLESS, path, out);
    if (!ok) return false;
    double n = mv3_norm(out);
    if (!(n > 0)) {
        err_at(c, path, "AXIS_ZERO", NULL, "direction vector is zero");
        return false;
    }
    if (fabs(n - 1) > 1e-9) {
        mdiag_add(c->d, MD_INFO, "AXIS_NORMALISED", path, NULL, "direction of length %.9g normalised", n);
        mv3_scale(out, out, 1 / n);
    }
    return true;
}

static bool jpose(Ctx *c, const JsonValue *v, const char *path, MPose *T) {
    mpose_identity(T);
    if (!v || v->type != JSON_OBJECT) {
        err_at(c, path, "INVALID_POSE", "use {\"position\": [x, y, z], \"rpy\": [roll, pitch, yaw]} or a quaternion [w, x, y, z]", "expected a pose object");
        return false;
    }
    static const char *const K[] = {"position", "rpy", "quaternion", "name", "description", NULL};
    check_keys(c, v, path, K);
    char p2[256];
    const JsonValue *pos = json_get(v, "position"), *rpy = json_get(v, "rpy"), *qu = json_get(v, "quaternion");
    if (pos) {
        snprintf(p2, sizeof p2, "%s.position", path);
        if (!jvec3(c, pos, MQ_LENGTH, p2, T->p)) return false;
    }
    if (rpy && qu) {
        err_at(c, path, "INVALID_POSE", NULL, "give either rpy or quaternion, not both");
        return false;
    }
    if (rpy) {
        double r[3];
        snprintf(p2, sizeof p2, "%s.rpy", path);
        if (!jvec3(c, rpy, MQ_ANGLE, p2, r)) return false;
        mrot_from_rpy(T->R, r[0], r[1], r[2]);
    } else if (qu) {
        double q[4];
        snprintf(p2, sizeof p2, "%s.quaternion", path);
        if (!json_get_numbers(qu, q, 4)) {
            err_at(c, p2, "INVALID_POSE", NULL, "quaternion must be four numbers [w, x, y, z]");
            return false;
        }
        double n = mq_normalize(q);
        if (!(n > 0)) {
            err_at(c, p2, "INVALID_POSE", NULL, "quaternion is zero");
            return false;
        }
        if (fabs(n - 1) > 1e-6) mdiag_add(c->d, MD_INFO, "QUATERNION_NORMALISED", p2, NULL, "quaternion of norm %.9g normalised", n);
        mq_to_mat(T->R, q);
    }
    return true;
}

static const char *resolve_file(Ctx *c, const char *given, char *out, size_t cap) {
    out[0] = 0;
    if (!given || !*given) return NULL;
    char tmp[NV_PATH_MAX];
    if (given[0] == '/' || !c->base_dir)
        snprintf(tmp, sizeof tmp, "%s", given);
    else if (!path_join(tmp, sizeof tmp, c->base_dir, given))
        return NULL;
    if (!path_is_file(tmp)) return NULL;
    snprintf(out, cap, "%s", tmp);
    return out;
}

/* ------------------------------------------------------------------------------------------------ mesh mass properties */

bool asm_mesh_mass_properties(const char *path, const char *units, const MPose *pose_in, const FillSpec *fill, const AsmLoadOptions *opt,
                              MassProperties *out, MechDiag *d, const char *subject) {
    double scale, off;
    char e[256];
    if (!units || !mech_unit_factor(units, MQ_LENGTH, &scale, &off, e, sizeof e)) {
        mdiag_add(d, MD_MISSING_INPUT, "MESH_UNITS_REQUIRED", subject, "STL files store no units: state the length unit the file was exported in",
                  "mesh '%s' needs a length unit%s%s", path, units ? ": " : "", units ? e : "");
        return false;
    }
    Mesh mesh;
    mesh_init(&mesh);
    StlInfo info;
    uint64_t maxb = opt && opt->max_file_bytes ? opt->max_file_bytes : 64ull << 20;
    uint32_t maxt = opt && opt->max_triangles ? opt->max_triangles : 5000000u;
    if (!mesh_load_stl_ex(path, &mesh, &info, maxb, maxt, e, sizeof e)) {
        mdiag_add(d, MD_ERROR, "MESH_LOAD_FAILED", subject, NULL, "%s", e);
        mesh_free(&mesh);
        return false;
    }
    SurfaceRepairOptions ro;
    surface_repair_defaults(&ro);
    ro.remove_duplicate_faces = false;
    ro.orient_outward = false; /* orientation problems are reported, not silently repaired */
    Surface surf;
    SurfaceComponent *comps = NULL;
    SurfaceDiagnostics sd;
    memset(&surf, 0, sizeof surf);
    bool ok = surface_build(&mesh, &ro, &surf, &comps, &sd, e, sizeof e);
    mesh_free(&mesh);
    if (!ok) {
        mdiag_add(d, MD_ERROR, "MESH_INVALID", subject, NULL, "%s", e);
        free(comps);
        surface_free(&surf);
        return false;
    }
    MPose T;
    if (pose_in)
        T = *pose_in;
    else
        mpose_identity(&T);
    for (int i = 0; i < surf.nv; i++) {
        double p[3] = {surf.v[3 * i] * scale, surf.v[3 * i + 1] * scale, surf.v[3 * i + 2] * scale};
        mpose_apply(surf.v + 3 * i, &T, p);
    }
    if (sd.degenerate_removed)
        mdiag_add(d, MD_INFO, "MESH_DEGENERATE_REMOVED", subject, NULL, "%u degenerate facets of %u removed before integrating", sd.degenerate_removed,
                  sd.source_triangles);
    ok = mass_properties_from_mesh(surf.v, surf.nv, surf.tri, surf.nt, fill, out, d, subject);
    free(comps);
    surface_free(&surf);
    return ok;
}

/* ------------------------------------------------------------------------------------------------ native format: read */

static bool read_fill(Ctx *c, const JsonValue *f, const char *path, FillSpec *fs, AsmSource *src) {
    memset(fs, 0, sizeof *fs);
    fs->model = FILL_MODEL_COUNT;
    if (!f || f->type != JSON_OBJECT) {
        missing_at(c, path, "FILL_MODEL_REQUIRED", "choose solid, shell_infill or measured_mass; an outer surface does not say whether a printed part is solid",
                   "mass properties from a mesh need a fill description");
        return false;
    }
    static const char *const K[] = {"model", "density", "shell_thickness", "infill_fraction", "measured_mass", "source", "note", NULL};
    check_keys(c, f, path, K);
    int m = fill_model_from_name(json_get_str(f, "model", NULL));
    if (m < 0) {
        missing_at(c, path, "FILL_MODEL_REQUIRED", "model is one of solid, shell_infill, measured_mass", "fill model missing or unknown");
        return false;
    }
    fs->model = (FillModel)m;
    char p2[256];
    bool ok = true;
    const JsonValue *v;
    if ((v = json_get(f, "density"))) snprintf(p2, sizeof p2, "%s.density", path), ok &= qty(c, v, MQ_DENSITY, p2, &fs->density);
    if ((v = json_get(f, "shell_thickness"))) snprintf(p2, sizeof p2, "%s.shell_thickness", path), ok &= qty(c, v, MQ_LENGTH, p2, &fs->shell_thickness);
    if ((v = json_get(f, "infill_fraction"))) snprintf(p2, sizeof p2, "%s.infill_fraction", path), ok &= qty(c, v, MQ_DIMENSIONLESS, p2, &fs->infill_fraction);
    if ((v = json_get(f, "measured_mass"))) snprintf(p2, sizeof p2, "%s.measured_mass", path), ok &= qty(c, v, MQ_MASS, p2, &fs->measured_mass);
    int s = asm_source_from_name(json_get_str(f, "source", "user"));
    *src = s < 0 ? SRC_USER : (AsmSource)s;
    return ok;
}

static bool read_inertia(Ctx *c, const JsonValue *v, const char *path, double I[9]) {
    if (!v || v->type != JSON_OBJECT) {
        missing_at(c, path, "INERTIA_REQUIRED", "give {ixx, iyy, izz, ixy, ixz, iyz} about the centre of mass (tensor components)",
                   "inertia missing");
        return false;
    }
    static const char *const K[] = {"ixx", "iyy", "izz", "ixy", "ixz", "iyz", NULL};
    check_keys(c, v, path, K);
    double x[6] = {0};
    static const char *const N[6] = {"ixx", "iyy", "izz", "ixy", "ixz", "iyz"};
    for (int k = 0; k < 6; k++) {
        const JsonValue *e = json_get(v, N[k]);
        char p2[256];
        snprintf(p2, sizeof p2, "%s.%s", path, N[k]);
        if (!e) {
            if (k < 3) {
                missing_at(c, p2, "INERTIA_REQUIRED", NULL, "principal component %s missing", N[k]);
                return false;
            }
            continue; /* omitted products of inertia are zero and recorded as an assumption below */
        }
        if (!qty(c, e, MQ_INERTIA, p2, &x[k])) return false;
    }
    if (!json_get(v, "ixy") || !json_get(v, "ixz") || !json_get(v, "iyz"))
        asm_note_assumption(c->a, path, SRC_DEFAULT, "products of inertia not given are taken as zero (axes assumed principal)");
    double t[9] = {x[0], x[3], x[4], x[3], x[1], x[5], x[4], x[5], x[2]};
    memcpy(I, t, sizeof t);
    return true;
}

static bool read_geom(Ctx *c, const JsonValue *g, const char *role, const char *path, AsmGeom *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->role, sizeof out->role, "%s", role);
    mpose_identity(&out->pose);
    out->scale[0] = out->scale[1] = out->scale[2] = 1;
    static const char *const K[] = {"type", "file", "units", "size", "radius", "length", "pose", "name", "friction", "friction_source", "restitution", "group", NULL};
    check_keys(c, g, path, K);
    snprintf(out->name, sizeof out->name, "%s", json_get_str(g, "name", ""));
    out->group = (int)json_get_int(g, "group", 0);
    {
        char pp[260];
        const JsonValue *fv = json_get(g, "friction"), *rv = json_get(g, "restitution");
        if (fv) {
            snprintf(pp, sizeof pp, "%s.friction", path);
            if (qty(c, fv, MQ_DIMENSIONLESS, pp, &out->friction)) out->has_friction = true;
            if (out->friction < 0) err_at(c, pp, "OUT_OF_RANGE", NULL, "friction coefficient must be >= 0");
            int src = asm_source_from_name(json_get_str(g, "friction_source", NULL));
            if (src < 0) {
                missing_at(c, pp, "CONTACT_PROVENANCE_REQUIRED",
                           "friction_source: measured (with a test), calibrated, user, inferred or default; coefficients are model parameters",
                           "say where the friction coefficient comes from");
            } else
                out->contact_source = (AsmSource)src;
        }
        if (rv) {
            snprintf(pp, sizeof pp, "%s.restitution", path);
            if (qty(c, rv, MQ_DIMENSIONLESS, pp, &out->restitution)) out->has_restitution = true;
            if (out->restitution < 0 || out->restitution > 1) err_at(c, pp, "OUT_OF_RANGE", NULL, "restitution must be in [0, 1]");
        }
    }
    const char *type = json_get_str(g, "type", json_get(g, "file") ? "mesh" : NULL);
    int t = -1;
    for (int i = 0; type && i < AG_COUNT; i++)
        if (!strcmp(type, GEOM_NAMES[i])) t = i;
    if (t < 0) {
        err_at(c, path, "INVALID_GEOMETRY", "type is mesh, box, sphere, cylinder, capsule or plane", "unknown geometry type");
        return false;
    }
    out->type = (AsmGeomType)t;
    char p2[256];
    bool ok = true;
    if (json_get(g, "pose")) snprintf(p2, sizeof p2, "%s.pose", path), ok &= jpose(c, json_get(g, "pose"), p2, &out->pose);
    switch (out->type) {
    case AG_MESH: {
        const char *f = json_get_str(g, "file", NULL);
        snprintf(out->file_as_given, sizeof out->file_as_given, "%s", f ? f : "");
        if (!f) {
            err_at(c, path, "INVALID_GEOMETRY", NULL, "mesh geometry needs a file");
            return false;
        }
        if (!resolve_file(c, f, out->file, sizeof out->file))
            mdiag_add(c->d, MD_WARNING, "GEOMETRY_FILE_MISSING", path, NULL, "mesh file '%s' not found relative to the assembly", f);
        const char *u = json_get_str(g, "units", NULL);
        double sc, off;
        char e[200];
        if (!u) {
            missing_at(c, path, "MESH_UNITS_REQUIRED", "STL files store no units: state the unit the file was exported in", "mesh '%s' needs units", f);
            return false;
        }
        if (!mech_unit_factor(u, MQ_LENGTH, &sc, &off, e, sizeof e)) {
            err_at(c, path, "INVALID_QUANTITY", NULL, "%s", e);
            return false;
        }
        snprintf(out->units, sizeof out->units, "%s", u);
        out->scale[0] = out->scale[1] = out->scale[2] = sc;
        break;
    }
    case AG_BOX:
        snprintf(p2, sizeof p2, "%s.size", path);
        ok &= jvec3(c, json_get(g, "size"), MQ_LENGTH, p2, out->size);
        break;
    case AG_SPHERE:
        snprintf(p2, sizeof p2, "%s.radius", path);
        ok &= qty(c, json_get(g, "radius"), MQ_LENGTH, p2, &out->size[0]);
        break;
    case AG_CYLINDER:
    case AG_CAPSULE:
        snprintf(p2, sizeof p2, "%s.radius", path);
        ok &= qty(c, json_get(g, "radius"), MQ_LENGTH, p2, &out->size[0]);
        snprintf(p2, sizeof p2, "%s.length", path);
        ok &= qty(c, json_get(g, "length"), MQ_LENGTH, p2, &out->size[1]);
        break;
    case AG_PLANE: break;
    default: break;
    }
    return ok;
}

static bool read_body(Ctx *c, const JsonValue *b, int index) {
    char path[160];
    snprintf(path, sizeof path, "bodies[%d]", index);
    if (!b || b->type != JSON_OBJECT) {
        err_at(c, path, "INVALID_BODY", NULL, "body must be an object");
        return false;
    }
    static const char *const K[] = {"name", "description", "part", "frames", "geometry", "mass_properties", "material", "manufacturing", "metadata", "flexible", NULL};
    check_keys(c, b, path, K);
    const char *name = json_get_str(b, "name", NULL);
    if (!name_ok(name)) {
        err_at(c, path, "INVALID_NAME", "names use letters, digits, '_' and '-' (1-63 characters) and 'world' is reserved", "body name missing or invalid");
        return false;
    }
    snprintf(path, sizeof path, "bodies.%s", name);
    int existing = asm_body_index(c->a, name);
    AsmBody *B;
    if (existing >= 0 && c->replace) {
        B = &c->a->bodies[existing];
        free(B->geoms), free(B->frames), json_free(B->manufacturing), asm_flexible_free(B->flexible);
        memset(B, 0, sizeof *B);
        snprintf(B->name, sizeof B->name, "%s", name);
        mpose_identity(&B->mass_mesh_pose);
    } else
        B = asm_add_body(c->a, name);
    if (!B) {
        err_at(c, path, "DUPLICATE_NAME", NULL, "body '%s' defined twice", name);
        return false;
    }
    snprintf(B->part, sizeof B->part, "%s", json_get_str(b, "part", ""));
    char p2[256];
    bool ok = true;
    /* frames */
    const JsonValue *fr = json_get(b, "frames");
    if (fr) {
        int n = (int)json_len(fr);
        B->frames = calloc((size_t)(n ? n : 1), sizeof *B->frames);
        for (int i = 0; i < n && B->frames; i++) {
            const JsonValue *f = json_at(fr, (size_t)i);
            snprintf(p2, sizeof p2, "%s.frames[%d]", path, i);
            const char *fn = json_get_str(f, "name", NULL);
            if (!name_ok(fn)) {
                err_at(c, p2, "INVALID_NAME", NULL, "frame name missing or invalid");
                ok = false;
                continue;
            }
            AsmFrame *F = &B->frames[B->nframes];
            snprintf(F->name, sizeof F->name, "%s", fn);
            if (jpose(c, f, p2, &F->pose)) B->nframes++;
            else ok = false;
        }
    }
    /* geometry */
    const JsonValue *geo = json_get(b, "geometry");
    if (geo) {
        static const char *const GK[] = {"visual", "collision", "fem", NULL};
        check_keys(c, geo, path, GK);
        int total = 0;
        for (int r = 0; GK[r]; r++) total += (int)json_len(json_get(geo, GK[r]));
        B->geoms = calloc((size_t)(total ? total : 1), sizeof *B->geoms);
        for (int r = 0; GK[r] && B->geoms; r++) {
            const JsonValue *arr = json_get(geo, GK[r]);
            for (size_t i = 0; i < json_len(arr); i++) {
                snprintf(p2, sizeof p2, "%s.geometry.%s[%zu]", path, GK[r], i);
                if (read_geom(c, json_at(arr, i), GK[r], p2, &B->geoms[B->ngeoms])) B->ngeoms++;
                else ok = false;
            }
        }
    }
    /* mass properties */
    const JsonValue *mpj = json_get(b, "mass_properties");
    snprintf(p2, sizeof p2, "%s.mass_properties", path);
    if (!mpj && c->partial) {
        /* kept from the existing body */
    } else if (!mpj) {
        missing_at(c, p2, "MASS_PROPERTIES_REQUIRED",
                   "give measured or CAD mass, centre of mass and inertia, or a closed mesh with a fill model (solid, shell_infill, measured_mass)",
                   "body '%s' has no mass properties", name);
        ok = false;
    } else {
        static const char *const MK[] = {"source", "provenance", "mass", "com", "inertia", "inertia_rpy", "file", "units", "pose", "fill", "note", "derived_from", NULL};
        check_keys(c, mpj, p2, MK);
        const char *src = json_get_str(mpj, "source", json_get(mpj, "mass") ? "explicit" : (json_get(mpj, "file") ? "mesh" : ""));
        const char *prov = json_get_str(mpj, "provenance", NULL);
        int pv = prov ? asm_source_from_name(prov) : -1;
        if (prov && pv < 0) {
            err_at(c, p2, "INVALID_PROVENANCE", "user, measured, cad, computed, inferred, default or calibrated", "unknown provenance '%s'", prov);
            ok = false;
        }
        snprintf(B->inertial_note, sizeof B->inertial_note, "%s", json_get_str(mpj, "note", ""));
        if (!strcmp(src, "explicit")) {
            char p3[300];
            snprintf(p3, sizeof p3, "%s.mass", p2);
            if (!json_get(mpj, "mass")) {
                missing_at(c, p3, "MASS_REQUIRED", NULL, "mass missing");
                ok = false;
            } else
                ok &= qty(c, json_get(mpj, "mass"), MQ_MASS, p3, &B->mass);
            snprintf(p3, sizeof p3, "%s.com", p2);
            if (!json_get(mpj, "com")) {
                missing_at(c, p3, "COM_REQUIRED", "the centre of mass changes gravity and inertial loads: give it in body coordinates", "centre of mass missing");
                ok = false;
            } else
                ok &= jvec3(c, json_get(mpj, "com"), MQ_LENGTH, p3, B->com);
            snprintf(p3, sizeof p3, "%s.inertia", p2);
            double I[9];
            if (read_inertia(c, json_get(mpj, "inertia"), p3, I)) {
                if (json_get(mpj, "inertia_rpy")) { /* inertia given in axes rotated from the body axes */
                    double r[3], R[9];
                    snprintf(p3, sizeof p3, "%s.inertia_rpy", p2);
                    if (jvec3(c, json_get(mpj, "inertia_rpy"), MQ_ANGLE, p3, r)) {
                        mrot_from_rpy(R, r[0], r[1], r[2]);
                        inertia_rotate(R, I, B->inertia);
                    } else
                        ok = false;
                } else
                    memcpy(B->inertia, I, sizeof I);
            } else
                ok = false;
            B->has_inertial = ok;
            B->inertial_source = pv >= 0 ? (AsmSource)pv : SRC_USER;
            if (pv < 0)
                asm_note_assumption(c->a, name, SRC_DEFAULT, "explicit mass properties without provenance are recorded as user input");
        } else if (!strcmp(src, "mesh")) {
            const char *f = json_get_str(mpj, "file", NULL);
            char resolved[NV_PATH_MAX];
            if (!f || !resolve_file(c, f, resolved, sizeof resolved)) {
                err_at(c, p2, "MESH_NOT_FOUND", NULL, "mass-properties mesh '%s' not found", f ? f : "(missing)");
                ok = false;
            } else {
                snprintf(B->mass_mesh, sizeof B->mass_mesh, "%s", resolved);
                snprintf(B->mass_mesh_units, sizeof B->mass_mesh_units, "%s", json_get_str(mpj, "units", ""));
                char p3[300];
                if (json_get(mpj, "pose")) {
                    snprintf(p3, sizeof p3, "%s.pose", p2);
                    ok &= jpose(c, json_get(mpj, "pose"), p3, &B->mass_mesh_pose);
                }
                AsmSource fsrc;
                snprintf(p3, sizeof p3, "%s.fill", p2);
                if (read_fill(c, json_get(mpj, "fill"), p3, &B->fill, &fsrc) && ok) {
                    MassProperties mp;
                    int before = c->d->nerrors + c->d->nmissing;
                    if (asm_mesh_mass_properties(B->mass_mesh, B->mass_mesh_units[0] ? B->mass_mesh_units : NULL, &B->mass_mesh_pose, &B->fill, c->opt, &mp,
                                                 c->d, name)) {
                        B->mass = mp.mass;
                        memcpy(B->com, mp.com, sizeof B->com);
                        memcpy(B->inertia, mp.inertia_com, sizeof B->inertia);
                        B->mp = mp;
                        B->has_inertial = true;
                        B->mass_from_mesh = true;
                        B->inertial_source = SRC_COMPUTED;
                        snprintf(B->inertial_note, sizeof B->inertial_note, "computed from %s (%s) with %s fill (%s)", path_basename(B->mass_mesh),
                                 B->mass_mesh_units, fill_model_name(B->fill.model), asm_source_name(fsrc));
                    } else {
                        c->errors += (c->d->nerrors + c->d->nmissing) - before;
                        ok = false;
                    }
                } else
                    ok = false;
            }
        } else {
            err_at(c, p2, "INVALID_MASS_PROPERTIES", "source is explicit (mass, com, inertia) or mesh (file, units, fill)", "unknown mass-properties source '%s'",
                   src);
            ok = false;
        }
    }
    /* material and manufacturing */
    const JsonValue *mat = json_get(b, "material");
    if (mat && mat->type == JSON_STRING) {
        snprintf(B->material, sizeof B->material, "%s", mat->u.string.ptr);
        B->material_source = SRC_USER;
    } else if (mat && mat->type == JSON_OBJECT) {
        snprintf(B->material, sizeof B->material, "%s", json_get_str(mat, "id", ""));
        int s = asm_source_from_name(json_get_str(mat, "source", "user"));
        B->material_source = s < 0 ? SRC_USER : (AsmSource)s;
    }
    const JsonValue *man = json_get(b, "manufacturing");
    if (man) B->manufacturing = json_clone(man);
    const JsonValue *fx = json_get(b, "flexible");
    if (fx && fx->type != JSON_NULL) {
        char p3[200];
        snprintf(p3, sizeof p3, "%s.flexible", path);
        B->flexible = asm_flexible_from_json(fx, c->d, p3);
        if (!B->flexible) c->errors++, ok = false;
    }
    return ok;
}

/* "body.frame" or a pose object; body_expected is the joint's own body (-1 world) */
static bool joint_frame(Ctx *c, const JsonValue *v, int body_expected, const char *path, MPose *T) {
    if (v && v->type == JSON_STRING) {
        const char *s = v->u.string.ptr, *dot = strchr(s, '.');
        char bname[MB_NAME] = "";
        if (!dot || (size_t)(dot - s) >= sizeof bname) {
            err_at(c, path, "INVALID_FRAME_REFERENCE", "write body.frame, e.g. \"upper_arm.elbow\"", "frame reference '%s' is not body.frame", s);
            return false;
        }
        memcpy(bname, s, (size_t)(dot - s));
        bname[dot - s] = 0;
        int bi = !strcmp(bname, "world") ? -1 : asm_body_index(c->a, bname);
        if (bi != body_expected) {
            err_at(c, path, "INVALID_FRAME_REFERENCE", NULL, "frame '%s' is not on the joint's body", s);
            return false;
        }
        if (bi < 0) {
            err_at(c, path, "INVALID_FRAME_REFERENCE", "give world frames as pose objects", "the world has no named frames");
            return false;
        }
        const AsmBody *B = &c->a->bodies[bi];
        for (int i = 0; i < B->nframes; i++)
            if (!strcmp(B->frames[i].name, dot + 1)) {
                *T = B->frames[i].pose;
                return true;
            }
        err_at(c, path, "INVALID_FRAME_REFERENCE", NULL, "body '%s' has no frame '%s'", bname, dot + 1);
        return false;
    }
    return jpose(c, v, path, T);
}

static bool read_joint(Ctx *c, const JsonValue *j, int index) {
    char path[160];
    snprintf(path, sizeof path, "joints[%d]", index);
    if (!j || j->type != JSON_OBJECT) {
        err_at(c, path, "INVALID_JOINT", NULL, "joint must be an object");
        return false;
    }
    static const char *const K[] = {"name", "description", "type", "parent", "child", "parent_frame", "child_frame", "axis", "motion", "initial",
                                    "limits", "spring", "damping", "friction", "metadata", NULL};
    check_keys(c, j, path, K);
    const char *name = json_get_str(j, "name", NULL);
    if (!name_ok(name)) {
        err_at(c, path, "INVALID_NAME", NULL, "joint name missing or invalid");
        return false;
    }
    snprintf(path, sizeof path, "joints.%s", name);
    int type = mb_joint_type_from_name(json_get_str(j, "type", NULL));
    if (type < 0) {
        err_at(c, path, "UNKNOWN_JOINT_TYPE", "fixed, revolute, prismatic, spherical or free", "joint type missing or unknown");
        return false;
    }
    const char *pn = json_get_str(j, "parent", NULL), *cn = json_get_str(j, "child", NULL);
    int pi = pn && !strcmp(pn, "world") ? -1 : asm_body_index(c->a, pn);
    int ci = asm_body_index(c->a, cn);
    if (!pn || (pi < 0 && strcmp(pn, "world")) || ci < 0) {
        err_at(c, path, "INVALID_JOINT_BODIES", "parent is \"world\" or a body name; child is a body name", "joint '%s': unknown parent or child", name);
        return false;
    }
    int existing = asm_joint_index(c->a, name);
    AsmJoint *AJ;
    if (existing >= 0 && c->replace) {
        AJ = &c->a->joints[existing];
        memset(AJ, 0, sizeof *AJ);
        MbJointDef *Jr = &AJ->def;
        snprintf(Jr->name, sizeof Jr->name, "%s", name);
        Jr->type = (MbJointType)type, Jr->parent = pi, Jr->child = ci;
        mpose_identity(&Jr->parent_frame);
        mpose_identity(&Jr->child_frame);
        mv3_set(Jr->axis, 0, 0, 1);
        if (type == MB_SPHERICAL) Jr->q0[0] = 1;
        if (type == MB_FREE) Jr->q0[3] = 1;
    } else
        AJ = asm_add_joint(c->a, name, (MbJointType)type, pi, ci);
    if (!AJ) {
        err_at(c, path, "DUPLICATE_NAME", NULL, "joint '%s' defined twice", name);
        return false;
    }
    MbJointDef *J = &AJ->def;
    char p2[256];
    bool ok = true;
    const JsonValue *pf = json_get(j, "parent_frame"), *cf = json_get(j, "child_frame");
    if (type == MB_FREE && !pf && !cf) {
        asm_note_assumption(c->a, name, SRC_DEFAULT, "free joint frames not given: the child body frame moves relative to the parent frame origin");
    } else {
        if (!pf || !cf) {
            missing_at(c, path, "JOINT_FRAMES_REQUIRED",
                       "give parent_frame (on the parent) and child_frame (on the child); for parts exported in one common frame both are the hinge pose",
                       "joint '%s' needs explicit frames on both bodies", name);
            return false;
        }
        snprintf(p2, sizeof p2, "%s.parent_frame", path);
        ok &= joint_frame(c, pf, pi, p2, &J->parent_frame);
        snprintf(p2, sizeof p2, "%s.child_frame", path);
        ok &= joint_frame(c, cf, ci, p2, &J->child_frame);
    }
    MechQty qpos = type == MB_PRISMATIC ? MQ_LENGTH : MQ_ANGLE, qvel = type == MB_PRISMATIC ? MQ_VELOCITY : MQ_ANGULAR_VELOCITY;
    if (type == MB_REVOLUTE || type == MB_PRISMATIC) {
        snprintf(p2, sizeof p2, "%s.axis", path);
        if (!json_get(j, "axis")) {
            missing_at(c, p2, "AXIS_REQUIRED", "give the axis as a vector in the joint frame", "joint '%s' needs an axis", name);
            ok = false;
        } else
            ok &= unit_vector(c, json_get(j, "axis"), p2, J->axis);
    }
    const char *motion = json_get_str(j, "motion", NULL);
    if (!motion) {
        J->motion = MB_PASSIVE;
        if (type != MB_FIXED) asm_note_assumption(c->a, name, SRC_DEFAULT, "motion not stated: passive joint (no actuator)");
    } else if (!strcmp(motion, "passive"))
        J->motion = MB_PASSIVE;
    else if (!strcmp(motion, "actuated"))
        J->motion = MB_ACTUATED;
    else if (!strcmp(motion, "prescribed"))
        J->motion = MB_PRESCRIBED;
    else {
        err_at(c, path, "INVALID_MOTION", "passive, actuated or prescribed", "unknown motion '%s'", motion);
        ok = false;
    }
    /* initial state */
    const JsonValue *ini = json_get(j, "initial");
    if (ini) {
        static const char *const IK[] = {"position", "velocity", "orientation", "angular_velocity", "linear_velocity", NULL};
        check_keys(c, ini, path, IK);
        const JsonValue *v;
        if (type == MB_REVOLUTE || type == MB_PRISMATIC) {
            if ((v = json_get(ini, "position"))) snprintf(p2, sizeof p2, "%s.initial.position", path), ok &= qty(c, v, qpos, p2, &J->q0[0]);
            if ((v = json_get(ini, "velocity"))) snprintf(p2, sizeof p2, "%s.initial.velocity", path), ok &= qty(c, v, qvel, p2, &J->v0[0]);
        } else if (type == MB_SPHERICAL || type == MB_FREE) {
            int qo = type == MB_FREE ? 3 : 0, vo = 0;
            if (type == MB_FREE && (v = json_get(ini, "position"))) snprintf(p2, sizeof p2, "%s.initial.position", path), ok &= jvec3(c, v, MQ_LENGTH, p2, J->q0);
            if ((v = json_get(ini, "orientation"))) {
                MPose T;
                snprintf(p2, sizeof p2, "%s.initial.orientation", path);
                if (jpose(c, v, p2, &T)) mq_from_mat(J->q0 + qo, T.R);
                else ok = false;
            }
            if ((v = json_get(ini, "angular_velocity"))) snprintf(p2, sizeof p2, "%s.initial.angular_velocity", path), ok &= jvec3(c, v, MQ_ANGULAR_VELOCITY, p2, J->v0 + vo);
            if (type == MB_FREE && (v = json_get(ini, "linear_velocity")))
                snprintf(p2, sizeof p2, "%s.initial.linear_velocity", path), ok &= jvec3(c, v, MQ_VELOCITY, p2, J->v0 + 3);
        }
    } else if (type != MB_FIXED)
        asm_note_assumption(c->a, name, SRC_DEFAULT, "initial position and velocity not given: zero (joint frames coincide, at rest)");
    /* limits and passive elements */
    const JsonValue *lim = json_get(j, "limits");
    if (lim) {
        static const char *const LK[] = {"lower", "upper", "restitution", "effort", "velocity", NULL};
        check_keys(c, lim, path, LK);
        const JsonValue *v;
        bool has_range = json_get(lim, "lower") || json_get(lim, "upper");
        if (has_range) {
            if (!json_get(lim, "lower") || !json_get(lim, "upper")) {
                missing_at(c, path, "LIMITS_INCOMPLETE", NULL, "joint '%s' limits need both lower and upper", name);
                ok = false;
            } else {
                snprintf(p2, sizeof p2, "%s.limits.lower", path);
                ok &= qty(c, json_get(lim, "lower"), qpos, p2, &J->lower);
                snprintf(p2, sizeof p2, "%s.limits.upper", path);
                ok &= qty(c, json_get(lim, "upper"), qpos, p2, &J->upper);
                J->limited = true;
                if ((v = json_get(lim, "restitution"))) {
                    snprintf(p2, sizeof p2, "%s.limits.restitution", path);
                    ok &= qty(c, v, MQ_DIMENSIONLESS, p2, &J->restitution);
                } else
                    asm_note_assumption(c->a, name, SRC_DEFAULT, "limit restitution not given: 0 (the joint stops at its limit)");
            }
        }
        if ((v = json_get(lim, "effort"))) snprintf(p2, sizeof p2, "%s.limits.effort", path), ok &= qty(c, v, type == MB_PRISMATIC ? MQ_FORCE : MQ_TORQUE, p2, &AJ->effort_limit);
        if ((v = json_get(lim, "velocity"))) snprintf(p2, sizeof p2, "%s.limits.velocity", path), ok &= qty(c, v, qvel, p2, &AJ->velocity_limit);
    }
    const JsonValue *spr = json_get(j, "spring");
    if (spr) {
        snprintf(p2, sizeof p2, "%s.spring.stiffness", path);
        ok &= qty(c, json_get(spr, "stiffness"), type == MB_PRISMATIC ? MQ_LINEAR_STIFFNESS : MQ_ROTATIONAL_STIFFNESS, p2, &J->stiffness);
        if (json_get(spr, "reference")) {
            snprintf(p2, sizeof p2, "%s.spring.reference", path);
            ok &= qty(c, json_get(spr, "reference"), qpos, p2, &J->spring_reference);
        } else
            asm_note_assumption(c->a, name, SRC_DEFAULT, "spring reference position not given: 0");
    }
    if (json_get(j, "damping")) {
        snprintf(p2, sizeof p2, "%s.damping", path);
        ok &= qty(c, json_get(j, "damping"), type == MB_PRISMATIC ? MQ_LINEAR_DAMPING : MQ_ROTATIONAL_DAMPING, p2, &J->damping);
    }
    const JsonValue *fric = json_get(j, "friction");
    if (fric) {
        snprintf(p2, sizeof p2, "%s.friction.coulomb", path);
        ok &= qty(c, json_get(fric, "coulomb"), type == MB_PRISMATIC ? MQ_FORCE : MQ_TORQUE, p2, &J->coulomb);
        if (json_get(fric, "regularization_velocity")) {
            snprintf(p2, sizeof p2, "%s.friction.regularization_velocity", path);
            ok &= qty(c, json_get(fric, "regularization_velocity"), qvel, p2, &J->coulomb_vreg);
        }
    }
    if ((type == MB_REVOLUTE || type == MB_PRISMATIC) && !fric && !json_get(j, "damping"))
        asm_note_assumption(c->a, name, SRC_DEFAULT, "no friction or damping given: ideal frictionless joint");
    return ok;
}

static void read_units(Ctx *c, const JsonValue *u) {
    if (!u) return;
    if (u->type != JSON_OBJECT) {
        err_at(c, "units", "INVALID_UNITS", NULL, "units must be an object such as {\"length\": \"mm\", \"angle\": \"deg\"}");
        return;
    }
    for (size_t i = 0; i < json_len(u); i++) {
        const char *k = json_key_at(u, i);
        const char *val = json_str(json_value_at(u, i));
        int q = -1;
        for (int j = 0; j < MQ_COUNT; j++)
            if (!strcmp(mech_qty_name((MechQty)j), k)) q = j;
        char p2[128], e[256];
        snprintf(p2, sizeof p2, "units.%s", k);
        double f, off;
        if (q < 0) {
            mdiag_add(c->d, MD_WARNING, "UNKNOWN_KEY", p2, NULL, "unknown quantity '%s' in units", k);
            continue;
        }
        if (!val || !mech_unit_factor(val, (MechQty)q, &f, &off, e, sizeof e)) {
            err_at(c, p2, "INVALID_UNITS", NULL, "%s", val ? e : "unit must be a string");
            continue;
        }
        snprintf(c->unit[q], sizeof c->unit[q], "%s", val);
    }
}

Assembly *asm_from_json(const JsonValue *doc, const char *base_dir, const AsmLoadOptions *opt, MechDiag *d) {
    Ctx c = {.d = d, .base_dir = base_dir, .opt = opt};
    if (!doc || doc->type != JSON_OBJECT) {
        mdiag_add(d, MD_ERROR, "INVALID_DOCUMENT", "", NULL, "assembly document must be a JSON object");
        return NULL;
    }
    if (strcmp(json_get_str(doc, "format", ""), "navier-assembly") || json_get_int(doc, "version", 0) != 1) {
        mdiag_add(d, MD_ERROR, "INVALID_DOCUMENT", "format", "set \"format\": \"navier-assembly\" and \"version\": 1", "not a navier-assembly version 1 document");
        return NULL;
    }
    const char *name = json_get_str(doc, "name", NULL);
    if (!name_ok(name)) {
        mdiag_add(d, MD_ERROR, "INVALID_NAME", "name", NULL, "assembly name missing or invalid");
        return NULL;
    }
    Assembly *a = asm_new(name);
    if (!a) return NULL;
    c.a = a;
    snprintf(a->source_format, sizeof a->source_format, "navier-assembly");
    static const char *const K[] = {"format", "version", "name", "description", "units", "gravity", "gravity_source", "bodies", "joints", "couplings",
                                    "assumptions", "unsupported", "source", "metadata", "$schema", "environment", NULL};
    check_keys(&c, doc, "", K);
    snprintf(a->description, sizeof a->description, "%s", json_get_str(doc, "description", ""));
    /* notes carried by a canonical document come first, so reloading keeps their order */
    const JsonValue *carried = json_get(doc, "assumptions");
    for (size_t i = 0; i < json_len(carried); i++) {
        const JsonValue *n = json_at(carried, i);
        int s = asm_source_from_name(json_get_str(n, "source", "default"));
        asm_note_assumption(a, json_get_str(n, "subject", ""), s < 0 ? SRC_DEFAULT : (AsmSource)s, "%s", json_get_str(n, "text", ""));
    }
    const JsonValue *un = json_get(doc, "unsupported");
    for (size_t i = 0; i < json_len(un); i++)
        asm_note_unsupported(a, json_get_str(json_at(un, i), "subject", ""), "%s", json_get_str(json_at(un, i), "text", ""));
    const JsonValue *src = json_get(doc, "source");
    if (src && src->type == JSON_OBJECT) { /* provenance of a canonical document */
        snprintf(a->source_format, sizeof a->source_format, "%s", json_get_str(src, "format", "navier-assembly"));
        snprintf(a->source_path, sizeof a->source_path, "%s", json_get_str(src, "path", ""));
        snprintf(a->source_sha256, sizeof a->source_sha256, "%s", json_get_str(src, "sha256", ""));
    }
    read_units(&c, json_get(doc, "units"));
    const JsonValue *g = json_get(doc, "gravity");
    if (g) {
        if (jvec3(&c, g, MQ_ACCELERATION, "gravity", a->gravity)) {
            int s = asm_source_from_name(json_get_str(doc, "gravity_source", "user"));
            a->gravity_source = s < 0 ? SRC_USER : (AsmSource)s;
        }
    } else
        asm_note_assumption(a, "gravity", SRC_DEFAULT, "gravity not given: 9.80665 m/s^2 along -Z of the build frame");
    const JsonValue *bodies = json_get(doc, "bodies");
    if (!bodies || bodies->type != JSON_ARRAY || json_len(bodies) == 0)
        err_at(&c, "bodies", "NO_BODIES", NULL, "an assembly needs at least one body");
    for (size_t i = 0; i < json_len(bodies); i++) read_body(&c, json_at(bodies, i), (int)i);
    const JsonValue *joints = json_get(doc, "joints");
    for (size_t i = 0; i < json_len(joints); i++) read_joint(&c, json_at(joints, i), (int)i);
    const JsonValue *env = json_get(doc, "environment");
    if (env && json_len(env)) {
        a->environment = calloc(json_len(env), sizeof *a->environment);
        for (size_t i = 0; a->environment && i < json_len(env); i++) {
            char p2[64];
            snprintf(p2, sizeof p2, "environment[%zu]", i);
            if (read_geom(&c, json_at(env, i), "environment", p2, &a->environment[a->nenvironment])) a->nenvironment++;
        }
    }
    const JsonValue *cps = json_get(doc, "couplings");
    for (size_t i = 0; i < json_len(cps); i++) {
        const JsonValue *cp = json_at(cps, i);
        char p2[128];
        snprintf(p2, sizeof p2, "couplings[%zu]", i);
        static const char *const CK[] = {"name", "follower", "driver", "ratio", "offset", "description", NULL};
        check_keys(&c, cp, p2, CK);
        const char *cn = json_get_str(cp, "name", NULL), *fo = json_get_str(cp, "follower", NULL), *dr = json_get_str(cp, "driver", NULL);
        int fi = asm_joint_index(a, fo), di = asm_joint_index(a, dr);
        if (!name_ok(cn) || fi < 0 || di < 0 || !json_get(cp, "ratio")) {
            err_at(&c, p2, "INVALID_COUPLING", "give name, follower and driver joint names and ratio (q_follower = ratio * q_driver + offset)",
                   "coupling incomplete or refers to unknown joints");
            continue;
        }
        if (!grow_array((void **)&a->couplings, &a->cap_couplings, a->ncouplings + 1, sizeof *a->couplings)) break;
        AsmCoupling *C = &a->couplings[a->ncouplings++];
        snprintf(C->name, sizeof C->name, "%s", cn);
        snprintf(C->follower, sizeof C->follower, "%s", fo);
        snprintf(C->driver, sizeof C->driver, "%s", dr);
        C->ratio = json_get_num(cp, "ratio", 0);
        C->offset = 0;
        if (json_get(cp, "offset")) {
            MechQty qo = a->joints[fi].def.type == MB_PRISMATIC ? MQ_LENGTH : MQ_ANGLE;
            qty(&c, json_get(cp, "offset"), qo, p2, &C->offset);
        }
    }
    if (c.errors) {
        asm_free(a);
        return NULL;
    }
    return a;
}

bool asm_body_set_json(Assembly *a, const JsonValue *body, const JsonValue *units, const char *base_dir, const AsmLoadOptions *opt, MechDiag *d) {
    Ctx c = {.d = d, .base_dir = base_dir, .opt = opt, .a = a, .replace = true};
    read_units(&c, units);
    if (c.errors) return false;
    return read_body(&c, body, 0) && c.errors == 0;
}

bool asm_body_update_json(Assembly *a, const JsonValue *patch, const JsonValue *units, const char *base_dir, const AsmLoadOptions *opt, MechDiag *d) {
    int bi = asm_body_index(a, json_get_str(patch, "name", NULL));
    if (bi < 0) return asm_body_set_json(a, patch, units, base_dir, opt, d);
    AsmBody old = a->bodies[bi]; /* owns geoms, frames, manufacturing and the flexible model from here on */
    a->bodies[bi].geoms = NULL, a->bodies[bi].frames = NULL, a->bodies[bi].manufacturing = NULL, a->bodies[bi].flexible = NULL;
    Ctx c = {.d = d, .base_dir = base_dir, .opt = opt, .a = a, .replace = true, .partial = true};
    read_units(&c, units);
    bool ok = c.errors == 0 && read_body(&c, patch, 0) && c.errors == 0;
    AsmBody *B = &a->bodies[bi];
    if (!ok) {
        free(B->geoms), free(B->frames), json_free(B->manufacturing), asm_flexible_free(B->flexible);
        *B = old;
        return false;
    }
    if (!json_get(patch, "mass_properties")) {
        B->has_inertial = old.has_inertial, B->mass = old.mass;
        memcpy(B->com, old.com, sizeof B->com), memcpy(B->inertia, old.inertia, sizeof B->inertia);
        B->inertial_source = old.inertial_source;
        memcpy(B->inertial_note, old.inertial_note, sizeof B->inertial_note);
        B->mass_from_mesh = old.mass_from_mesh;
        memcpy(B->mass_mesh, old.mass_mesh, sizeof B->mass_mesh), memcpy(B->mass_mesh_units, old.mass_mesh_units, sizeof B->mass_mesh_units);
        B->mass_mesh_pose = old.mass_mesh_pose, B->fill = old.fill, B->mp = old.mp;
    }
    if (!json_get(patch, "part")) memcpy(B->part, old.part, sizeof B->part); /* measured mass properties keep the part link */
    if (!json_get(patch, "frames")) B->frames = old.frames, B->nframes = old.nframes, old.frames = NULL;
    /* geometry by role: roles the patch does not name are kept */
    const JsonValue *geo = json_get(patch, "geometry");
    int keep = 0;
    for (int g = 0; g < old.ngeoms; g++) keep += !json_get(geo, old.geoms[g].role);
    if (keep) {
        AsmGeom *merged = calloc((size_t)(B->ngeoms + keep), sizeof *merged);
        if (!merged) {
            free(old.geoms), free(old.frames), json_free(old.manufacturing), asm_flexible_free(old.flexible);
            return false;
        }
        if (B->ngeoms) memcpy(merged, B->geoms, (size_t)B->ngeoms * sizeof *merged);
        int n = B->ngeoms;
        for (int g = 0; g < old.ngeoms; g++)
            if (!json_get(geo, old.geoms[g].role)) merged[n++] = old.geoms[g];
        free(B->geoms);
        B->geoms = merged, B->ngeoms = n;
    }
    if (!json_get(patch, "material")) memcpy(B->material, old.material, sizeof B->material), B->material_source = old.material_source;
    if (!json_get(patch, "manufacturing")) B->manufacturing = old.manufacturing, old.manufacturing = NULL;
    if (!json_get(patch, "flexible")) B->flexible = old.flexible, old.flexible = NULL; /* "flexible": null makes the body rigid */
    free(old.geoms), free(old.frames), json_free(old.manufacturing), asm_flexible_free(old.flexible);
    return true;
}

bool asm_joint_set_json(Assembly *a, const JsonValue *joint, const JsonValue *units, const AsmLoadOptions *opt, MechDiag *d) {
    Ctx c = {.d = d, .a = a, .opt = opt, .replace = true};
    read_units(&c, units);
    if (c.errors) return false;
    return read_joint(&c, joint, 0) && c.errors == 0;
}

bool asm_environment_set_json(Assembly *a, const JsonValue *shapes, const JsonValue *units, const AsmLoadOptions *opt, MechDiag *d) {
    Ctx c = {.d = d, .opt = opt, .a = a};
    read_units(&c, units);
    size_t n = json_len(shapes);
    AsmGeom *env = calloc(n ? n : 1, sizeof *env);
    int count = 0;
    for (size_t i = 0; env && i < n; i++) {
        char p2[64];
        snprintf(p2, sizeof p2, "environment[%zu]", i);
        if (read_geom(&c, json_at(shapes, i), "environment", p2, &env[count])) count++;
    }
    if (!env || c.errors) {
        free(env);
        return false;
    }
    free(a->environment);
    a->environment = env, a->nenvironment = count;
    return true;
}

Assembly *asm_clone(const Assembly *a) {
    JsonValue *doc = asm_to_json(a);
    MechDiag d;
    mdiag_init(&d);
    Assembly *b = doc ? asm_from_json(doc, NULL, NULL, &d) : NULL;
    json_free(doc);
    mdiag_free(&d);
    return b;
}

Assembly *asm_load_json(const char *path, const AsmLoadOptions *opt, MechDiag *d) {
    JsonError je;
    size_t maxb = opt && opt->max_file_bytes ? (size_t)opt->max_file_bytes : (size_t)64 << 20;
    JsonValue *doc = json_read_file(path, maxb, &je);
    if (!doc) {
        mdiag_add(d, MD_ERROR, "INVALID_JSON", path, NULL, "%s (line %d, column %d)", je.message, je.line, je.column);
        return NULL;
    }
    char dir[NV_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(dir, sizeof dir, ".");
    Assembly *a = asm_from_json(doc, dir, opt, d);
    json_free(doc);
    if (a) {
        snprintf(a->source_path, sizeof a->source_path, "%s", path);
        uint64_t sz;
        sha256_file(path, a->source_sha256, &sz);
    }
    return a;
}

Assembly *asm_load_file(const char *path, const AsmLoadOptions *opt, MechDiag *d) {
    const char *ext = strrchr(path, '.');
    if (ext && (!strcasecmp(ext, ".urdf") || !strcasecmp(ext, ".xml"))) return asm_load_urdf(path, opt, d);
    return asm_load_json(path, opt, d);
}

/* ------------------------------------------------------------------------------------------------ STL parts */

AsmBody *asm_add_stl_body(Assembly *a, const char *name, const char *path, const char *units, const FillSpec *fill, AsmSource fill_source,
                          const AsmLoadOptions *opt, MechDiag *d) {
    if (!name_ok(name)) {
        mdiag_add(d, MD_ERROR, "INVALID_NAME", name ? name : "", NULL, "body name missing or invalid");
        return NULL;
    }
    if (asm_body_index(a, name) >= 0) {
        mdiag_add(d, MD_ERROR, "DUPLICATE_NAME", name, NULL, "body '%s' already exists", name);
        return NULL;
    }
    MassProperties mp;
    MPose I;
    mpose_identity(&I);
    if (!asm_mesh_mass_properties(path, units, &I, fill, opt, &mp, d, name)) return NULL;
    AsmBody *B = asm_add_body(a, name);
    if (!B) return NULL;
    B->has_inertial = true;
    B->mass = mp.mass;
    memcpy(B->com, mp.com, sizeof B->com);
    memcpy(B->inertia, mp.inertia_com, sizeof B->inertia);
    B->mp = mp;
    B->mass_from_mesh = true;
    B->inertial_source = SRC_COMPUTED;
    B->fill = *fill;
    snprintf(B->mass_mesh, sizeof B->mass_mesh, "%s", path);
    snprintf(B->mass_mesh_units, sizeof B->mass_mesh_units, "%s", units);
    snprintf(B->inertial_note, sizeof B->inertial_note, "computed from %s (%s) with %s fill (%s)", path_basename(path), units, fill_model_name(fill->model),
             asm_source_name(fill_source));
    B->geoms = calloc(2, sizeof *B->geoms);
    if (B->geoms) {
        for (int r = 0; r < 2; r++) {
            AsmGeom *G = &B->geoms[B->ngeoms++];
            snprintf(G->role, sizeof G->role, "%s", r ? "fem" : "visual");
            G->type = AG_MESH;
            snprintf(G->file, sizeof G->file, "%s", path);
            snprintf(G->file_as_given, sizeof G->file_as_given, "%s", path);
            snprintf(G->units, sizeof G->units, "%s", units);
            double sc, off;
            char e[64];
            mech_unit_factor(units, MQ_LENGTH, &sc, &off, e, sizeof e);
            G->scale[0] = G->scale[1] = G->scale[2] = sc;
            mpose_identity(&G->pose);
        }
    }
    return B;
}

/* ------------------------------------------------------------------------------------------------ native format: write */

static JsonValue *pose_json(const MPose *T) {
    JsonValue *o = json_object();
    double q[4];
    mq_from_mat(q, T->R);
    json_set(o, "position", json_numbers(T->p, 3));
    json_set(o, "quaternion", json_numbers(q, 4));
    return o;
}

static JsonValue *inertia_json(const double *I) {
    JsonValue *o = json_object();
    json_set_number(o, "ixx", I[0]);
    json_set_number(o, "iyy", I[4]);
    json_set_number(o, "izz", I[8]);
    json_set_number(o, "ixy", I[1]);
    json_set_number(o, "ixz", I[2]);
    json_set_number(o, "iyz", I[5]);
    return o;
}

static JsonValue *notes_json(const AsmNote *n, int count) {
    JsonValue *a = json_array();
    for (int i = 0; i < count; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "subject", n[i].subject);
        json_set_string(o, "text", n[i].text);
        json_set_string(o, "source", asm_source_name(n[i].source));
        json_push(a, o);
    }
    return a;
}

JsonValue *asm_to_json(const Assembly *a) {
    JsonValue *doc = json_object();
    json_set_string(doc, "format", "navier-assembly");
    json_set_int(doc, "version", 1);
    json_set_string(doc, "name", a->name);
    if (a->description[0]) json_set_string(doc, "description", a->description);
    JsonValue *u = json_set_object(doc, "units");
    json_set_string(u, "length", "m");
    json_set_string(u, "angle", "rad");
    json_set_string(u, "mass", "kg");
    json_set(doc, "gravity", json_numbers(a->gravity, 3));
    json_set_string(doc, "gravity_source", asm_source_name(a->gravity_source));
    JsonValue *src = json_set_object(doc, "source");
    json_set_string(src, "format", a->source_format);
    if (a->source_path[0]) json_set_string(src, "path", a->source_path);
    if (a->source_sha256[0]) json_set_string(src, "sha256", a->source_sha256);
    JsonValue *bodies = json_set_array(doc, "bodies");
    for (int i = 0; i < a->nbodies; i++) {
        const AsmBody *B = &a->bodies[i];
        JsonValue *b = json_object();
        json_set_string(b, "name", B->name);
        if (B->part[0]) json_set_string(b, "part", B->part);
        if (B->has_inertial) {
            JsonValue *mp = json_set_object(b, "mass_properties");
            json_set_string(mp, "source", "explicit");
            json_set_string(mp, "provenance", asm_source_name(B->inertial_source));
            json_set_number(mp, "mass", B->mass);
            json_set(mp, "com", json_numbers(B->com, 3));
            json_set(mp, "inertia", inertia_json(B->inertia));
            if (B->inertial_note[0]) json_set_string(mp, "note", B->inertial_note);
            if (B->mass_from_mesh) {
                JsonValue *df = json_set_object(mp, "derived_from");
                json_set_string(df, "file", B->mass_mesh);
                json_set_string(df, "units", B->mass_mesh_units);
                json_set(df, "pose", pose_json(&B->mass_mesh_pose));
                JsonValue *f = json_set_object(df, "fill");
                json_set_string(f, "model", fill_model_name(B->fill.model));
                if (B->fill.density > 0) json_set_number(f, "density", B->fill.density);
                if (B->fill.model == FILL_SHELL_INFILL) {
                    json_set_number(f, "shell_thickness", B->fill.shell_thickness);
                    json_set_number(f, "infill_fraction", B->fill.infill_fraction);
                }
                if (B->fill.model == FILL_MEASURED_MASS) json_set_number(f, "measured_mass", B->fill.measured_mass);
                json_set_number(df, "volume", B->mp.volume);
                json_set_number(df, "area", B->mp.area);
            }
        }
        if (B->nframes) {
            JsonValue *fr = json_set_array(b, "frames");
            for (int k = 0; k < B->nframes; k++) {
                JsonValue *f = pose_json(&B->frames[k].pose);
                json_set_string(f, "name", B->frames[k].name);
                json_push(fr, f);
            }
        }
        if (B->ngeoms) {
            JsonValue *geo = json_set_object(b, "geometry");
            for (int k = 0; k < B->ngeoms; k++) {
                const AsmGeom *G = &B->geoms[k];
                JsonValue *arr = json_get(geo, G->role);
                if (!arr) arr = json_set_array(geo, G->role);
                JsonValue *g = json_object();
                json_set_string(g, "type", GEOM_NAMES[G->type]);
                if (G->type == AG_MESH) {
                    json_set_string(g, "file", G->file[0] ? G->file : G->file_as_given);
                    json_set_string(g, "units", G->units[0] ? G->units : "m");
                } else if (G->type == AG_BOX)
                    json_set(g, "size", json_numbers(G->size, 3));
                else if (G->type == AG_SPHERE)
                    json_set_number(g, "radius", G->size[0]);
                else if (G->type == AG_CYLINDER || G->type == AG_CAPSULE) {
                    json_set_number(g, "radius", G->size[0]);
                    json_set_number(g, "length", G->size[1]);
                }
                json_set(g, "pose", pose_json(&G->pose));
                if (G->name[0]) json_set_string(g, "name", G->name);
                if (G->has_friction) json_set_number(g, "friction", G->friction), json_set_string(g, "friction_source", asm_source_name(G->contact_source));
                if (G->has_restitution) json_set_number(g, "restitution", G->restitution);
                if (G->group) json_set_int(g, "group", G->group);
                json_push(arr, g);
            }
        }
        if (B->material[0]) {
            JsonValue *m = json_set_object(b, "material");
            json_set_string(m, "id", B->material);
            json_set_string(m, "source", asm_source_name(B->material_source));
        }
        if (B->manufacturing) json_set(b, "manufacturing", json_clone(B->manufacturing));
        if (B->flexible) json_set(b, "flexible", asm_flexible_to_json(B->flexible));
        json_push(bodies, b);
    }
    JsonValue *joints = json_set_array(doc, "joints");
    for (int i = 0; i < a->njoints; i++) {
        const AsmJoint *AJ = &a->joints[i];
        const MbJointDef *J = &AJ->def;
        JsonValue *j = json_object();
        json_set_string(j, "name", J->name);
        json_set_string(j, "type", mb_joint_type_name(J->type));
        json_set_string(j, "parent", J->parent < 0 ? "world" : a->bodies[J->parent].name);
        json_set_string(j, "child", a->bodies[J->child].name);
        json_set(j, "parent_frame", pose_json(&J->parent_frame));
        json_set(j, "child_frame", pose_json(&J->child_frame));
        bool scalar = J->type == MB_REVOLUTE || J->type == MB_PRISMATIC;
        if (scalar) json_set(j, "axis", json_numbers(J->axis, 3));
        json_set_string(j, "motion", J->motion == MB_ACTUATED ? "actuated" : (J->motion == MB_PRESCRIBED ? "prescribed" : "passive"));
        if (J->type != MB_FIXED) {
            JsonValue *ini = json_set_object(j, "initial");
            if (scalar) {
                json_set_number(ini, "position", J->q0[0]);
                json_set_number(ini, "velocity", J->v0[0]);
            } else {
                MPose T;
                mpose_identity(&T);
                const double *q = J->type == MB_FREE ? J->q0 + 3 : J->q0;
                mq_to_mat(T.R, q);
                JsonValue *o = pose_json(&T);
                json_remove(o, "position");
                json_set(ini, "orientation", o);
                json_set(ini, "angular_velocity", json_numbers(J->v0, 3));
                if (J->type == MB_FREE) {
                    json_set(ini, "position", json_numbers(J->q0, 3));
                    json_set(ini, "linear_velocity", json_numbers(J->v0 + 3, 3));
                }
            }
        }
        if (J->limited || AJ->effort_limit > 0 || AJ->velocity_limit > 0) {
            JsonValue *l = json_set_object(j, "limits");
            if (J->limited) {
                json_set_number(l, "lower", J->lower);
                json_set_number(l, "upper", J->upper);
                json_set_number(l, "restitution", J->restitution);
            }
            if (AJ->effort_limit > 0) json_set_number(l, "effort", AJ->effort_limit);
            if (AJ->velocity_limit > 0) json_set_number(l, "velocity", AJ->velocity_limit);
        }
        if (J->stiffness > 0) {
            JsonValue *s = json_set_object(j, "spring");
            json_set_number(s, "stiffness", J->stiffness);
            json_set_number(s, "reference", J->spring_reference);
        }
        if (J->damping > 0) json_set_number(j, "damping", J->damping);
        if (J->coulomb > 0) {
            JsonValue *f = json_set_object(j, "friction");
            json_set_number(f, "coulomb", J->coulomb);
            if (J->coulomb_vreg > 0) json_set_number(f, "regularization_velocity", J->coulomb_vreg);
        }
        json_push(joints, j);
    }
    if (a->ncouplings) {
        JsonValue *cps = json_set_array(doc, "couplings");
        for (int i = 0; i < a->ncouplings; i++) {
            JsonValue *o = json_object();
            json_set_string(o, "name", a->couplings[i].name);
            json_set_string(o, "follower", a->couplings[i].follower);
            json_set_string(o, "driver", a->couplings[i].driver);
            json_set_number(o, "ratio", a->couplings[i].ratio);
            json_set_number(o, "offset", a->couplings[i].offset);
            json_push(cps, o);
        }
    }
    if (a->nenvironment) {
        JsonValue *env = json_set_array(doc, "environment");
        for (int k = 0; k < a->nenvironment; k++) {
            const AsmGeom *G = &a->environment[k];
            JsonValue *g = json_object();
            json_set_string(g, "type", GEOM_NAMES[G->type]);
            if (G->type == AG_BOX) json_set(g, "size", json_numbers(G->size, 3));
            else if (G->type == AG_SPHERE) json_set_number(g, "radius", G->size[0]);
            else if (G->type == AG_CYLINDER || G->type == AG_CAPSULE) json_set_number(g, "radius", G->size[0]), json_set_number(g, "length", G->size[1]);
            json_set(g, "pose", pose_json(&G->pose));
            if (G->name[0]) json_set_string(g, "name", G->name);
            if (G->has_friction) json_set_number(g, "friction", G->friction), json_set_string(g, "friction_source", asm_source_name(G->contact_source));
            if (G->has_restitution) json_set_number(g, "restitution", G->restitution);
            if (G->group) json_set_int(g, "group", G->group);
            json_push(env, g);
        }
    }
    json_set(doc, "assumptions", notes_json(a->assumptions, a->nassumptions));
    json_set(doc, "unsupported", notes_json(a->unsupported, a->nunsupported));
    return doc;
}

/* ------------------------------------------------------------------------------------------------ to multibody */

void asm_flexible_free(AsmFlexible *f) {
    if (!f) return;
    mbflex_free(&f->model);
    free(f->interface_joint);
    json_free(f->provenance);
    free(f);
}

static bool flex_array(const JsonValue *v, const char *key, double *out, size_t n, MechDiag *d, const char *subject) {
    const JsonValue *arr = json_get(v, key);
    if (!arr || arr->type != JSON_ARRAY || json_len(arr) != n || !json_get_numbers(arr, out, n)) {
        mdiag_add(d, MD_ERROR, "INVALID_FLEXIBLE_MODEL", subject, "the flexible block is written by mech_flexible_attach; do not edit it by hand",
                  "'%s' must be an array of %zu numbers", key, n);
        return false;
    }
    for (size_t i = 0; i < n; i++)
        if (!isfinite(out[i])) {
            mdiag_add(d, MD_ERROR, "INVALID_FLEXIBLE_MODEL", subject, NULL, "'%s' has a non-finite entry", key);
            return false;
        }
    return true;
}

AsmFlexible *asm_flexible_from_json(const JsonValue *v, MechDiag *d, const char *subject) {
    static const char *const K[] = {"method", "units", "omega2", "zeta", "ell", "dJ", "interfaces", "fe_mass_properties", "provenance", NULL};
    if (!v || v->type != JSON_OBJECT) {
        mdiag_add(d, MD_ERROR, "INVALID_FLEXIBLE_MODEL", subject, NULL, "the flexible model must be an object");
        return NULL;
    }
    for (size_t i = 0; i < json_len(v); i++) {
        const char *k = json_key_at(v, i);
        bool ok = false;
        for (int j = 0; K[j] && !ok; j++) ok = !strcmp(K[j], k);
        if (!ok) mdiag_add(d, MD_WARNING, "UNKNOWN_KEY", subject, NULL, "unknown key '%s' ignored", k);
    }
    if (strcmp(json_get_str(v, "method", ""), "craig_bampton") || strcmp(json_get_str(v, "units", ""), "SI")) {
        mdiag_add(d, MD_ERROR, "INVALID_FLEXIBLE_MODEL", subject, NULL, "method must be \"craig_bampton\" and units \"SI\"");
        return NULL;
    }
    size_t n = json_len(json_get(v, "omega2"));
    const JsonValue *itf = json_get(v, "interfaces");
    size_t ni = itf && itf->type == JSON_ARRAY ? json_len(itf) : 0;
    if (n < 1 || n > 512 || ni > 64 || (itf && itf->type != JSON_ARRAY)) {
        mdiag_add(d, MD_ERROR, "INVALID_FLEXIBLE_MODEL", subject, NULL, "a flexible model has 1..512 coordinates and at most 64 interfaces");
        return NULL;
    }
    AsmFlexible *f = calloc(1, sizeof *f);
    if (!f || !mbflex_alloc(&f->model, -1, (int)n, (int)ni) || !(f->interface_joint = calloc(ni ? ni : 1, sizeof *f->interface_joint))) {
        asm_flexible_free(f);
        return NULL;
    }
    bool ok = flex_array(v, "omega2", f->model.omega2, n, d, subject) && flex_array(v, "zeta", f->model.zeta, n, d, subject) &&
              flex_array(v, "ell", f->model.ell, 6 * n, d, subject) && flex_array(v, "dJ", f->model.dJ, 9 * n, d, subject);
    for (size_t i = 0; ok && i < ni; i++) {
        const JsonValue *it = json_at(itf, i);
        const char *jn = json_get_str(it, "joint", "");
        if (!jn[0] || strlen(jn) >= MB_NAME) {
            mdiag_add(d, MD_ERROR, "INVALID_FLEXIBLE_MODEL", subject, NULL, "interface %zu names no joint", i);
            ok = false;
            break;
        }
        snprintf(f->interface_joint[i], MB_NAME, "%s", jn);
        MbFlexInterface *I = &f->model.interfaces[i];
        ok = flex_array(it, "point", I->point, 3, d, subject) && flex_array(it, "phi", I->phi, 3 * n, d, subject) && flex_array(it, "psi", I->psi, 3 * n, d, subject);
    }
    const JsonValue *mp = json_get(v, "fe_mass_properties");
    ok = ok && flex_array(mp, "com", f->fe_com, 3, d, subject) && flex_array(mp, "inertia", f->fe_inertia, 9, d, subject);
    f->fe_mass = json_get_num(mp, "mass", NAN);
    if (ok && !(f->fe_mass > 0)) {
        mdiag_add(d, MD_ERROR, "INVALID_FLEXIBLE_MODEL", subject, NULL, "fe_mass_properties.mass must be positive");
        ok = false;
    }
    for (size_t k = 0; ok && k < n; k++)
        if (!(f->model.omega2[k] > 0) || !(f->model.zeta[k] >= 0 && f->model.zeta[k] < 1) || (k && f->model.omega2[k] < f->model.omega2[k - 1])) {
            mdiag_add(d, MD_ERROR, "INVALID_FLEXIBLE_MODEL", subject, NULL, "coordinate %zu: omega2 must be positive and ascending, zeta in [0, 1)", k);
            ok = false;
        }
    if (!ok) {
        asm_flexible_free(f);
        return NULL;
    }
    const JsonValue *prov = json_get(v, "provenance");
    f->provenance = prov ? json_clone(prov) : json_object();
    return f;
}

JsonValue *asm_flexible_to_json(const AsmFlexible *f) {
    const MbFlexDef *F = &f->model;
    size_t n = (size_t)F->nmodes;
    JsonValue *o = json_object();
    json_set_string(o, "method", "craig_bampton");
    json_set_string(o, "units", "SI");
    json_set(o, "omega2", json_numbers(F->omega2, n));
    json_set(o, "zeta", json_numbers(F->zeta, n));
    json_set(o, "ell", json_numbers(F->ell, 6 * n));
    json_set(o, "dJ", json_numbers(F->dJ, 9 * n));
    JsonValue *ia = json_set_array(o, "interfaces");
    for (int i = 0; i < F->ninterfaces; i++) {
        JsonValue *it = json_object();
        json_set_string(it, "joint", f->interface_joint[i]);
        json_set(it, "point", json_numbers(F->interfaces[i].point, 3));
        json_set(it, "phi", json_numbers(F->interfaces[i].phi, 3 * n));
        json_set(it, "psi", json_numbers(F->interfaces[i].psi, 3 * n));
        json_push(ia, it);
    }
    JsonValue *mp = json_set_object(o, "fe_mass_properties");
    json_set_number(mp, "mass", f->fe_mass);
    json_set(mp, "com", json_numbers(f->fe_com, 3));
    json_set(mp, "inertia", json_numbers(f->fe_inertia, 9));
    json_set(o, "provenance", f->provenance ? json_clone(f->provenance) : json_object());
    return o;
}

MbModelDef *asm_to_model(const Assembly *a, MechDiag *d) {
    int nb = a->nbodies, nj = a->njoints;
    /* massless bodies fixed to a parent become frames of that parent */
    int *map = malloc((size_t)(nb ? nb : 1) * sizeof *map);      /* assembly body -> model body (-1 world, -2 merged) */
    MPose *in_parent = malloc((size_t)(nb ? nb : 1) * sizeof *in_parent); /* merged body frame in its surviving ancestor */
    int *ancestor = malloc((size_t)(nb ? nb : 1) * sizeof *ancestor);
    bool *drop_joint = calloc((size_t)(nj ? nj : 1), sizeof *drop_joint);
    MbModelDef *def = mbdef_new();
    if (!map || !in_parent || !ancestor || !drop_joint || !def) {
        free(map), free(in_parent), free(ancestor), free(drop_joint), mbdef_free(def);
        return NULL;
    }
    mv3_copy(def->gravity, a->gravity);
    for (int b = 0; b < nb; b++) map[b] = 0, ancestor[b] = b, mpose_identity(&in_parent[b]);
    bool changed = true, fail = false;
    while (changed) {
        changed = false;
        for (int b = 0; b < nb; b++) {
            if (a->bodies[b].has_inertial || map[b] == -2) continue;
            int tj = -1, count = 0;
            for (int j = 0; j < nj; j++)
                if (a->joints[j].def.child == b && !drop_joint[j]) tj = j, count++;
            if (count == 1 && a->joints[tj].def.type == MB_FIXED) {
                const MbJointDef *J = &a->joints[tj].def;
                MPose inv, t;
                mpose_inverse(&inv, &J->child_frame);
                mpose_mul(&t, &J->parent_frame, &inv); /* body b in parent coordinates */
                map[b] = -2;
                drop_joint[tj] = true;
                int p = J->parent;
                ancestor[b] = p < 0 ? -1 : ancestor[p];
                if (p >= 0 && map[p] == -2)
                    mpose_mul(&in_parent[b], &in_parent[p], &t);
                else
                    in_parent[b] = t;
                mdiag_add(d, MD_INFO, "MASSLESS_LINK_MERGED", a->bodies[b].name, NULL, "'%s' has no mass properties and is fixed to '%s': treated as a frame of it",
                          a->bodies[b].name, p < 0 ? "world" : a->bodies[p].name);
                changed = true;
            }
        }
    }
    for (int b = 0; b < nb; b++) {
        if (map[b] == -2) continue;
        const AsmBody *B = &a->bodies[b];
        if (!B->has_inertial) {
            mdiag_add(d, MD_MISSING_INPUT, "MASS_PROPERTIES_REQUIRED", B->name,
                      "give mass, centre of mass and inertia, or a closed mesh with a fill model; only links fixed to another body may be massless",
                      "moving body '%s' has no mass properties", B->name);
            fail = true;
            continue;
        }
        map[b] = mbdef_add_body(def, B->name, B->mass, B->com, B->inertia);
    }
    /* resolve ancestors through chains of merged bodies */
    for (int b = 0; b < nb; b++) {
        if (map[b] != -2) continue;
        int anc = ancestor[b];
        while (anc >= 0 && map[anc] == -2) anc = ancestor[anc];
        ancestor[b] = anc;
        if (anc >= 0 && a->bodies[anc].flexible) {
            mdiag_add(d, MD_ERROR, "FLEXIBLE_MERGED_LINK", a->bodies[b].name, "give the link mass properties and attach it through an interface of the flexible body",
                      "massless link '%s' is fixed to flexible body '%s' and would follow its reference frame, not the deformed part", a->bodies[b].name,
                      a->bodies[anc].name);
            fail = true;
        }
    }
    /* flexible bodies: the reduced model, with mass properties that must be those of the reduced mesh */
    for (int b = 0; b < nb && !fail; b++) {
        const AsmBody *B = &a->bodies[b];
        const AsmFlexible *X = B->flexible;
        if (!X || map[b] < 0) continue;
        double dc = 0, di = 0, isc = 0;
        for (int k = 0; k < 3; k++) dc = fmax(dc, fabs(B->com[k] - X->fe_com[k]));
        for (int k = 0; k < 9; k++) di = fmax(di, fabs(B->inertia[k] - X->fe_inertia[k])), isc = fmax(isc, fabs(X->fe_inertia[k]));
        if (fabs(B->mass - X->fe_mass) > 1e-9 * X->fe_mass || dc > 1e-9 * (sqrt(isc / X->fe_mass) + 1e-12) || di > 1e-9 * isc) {
            mdiag_add(d, MD_ERROR, "FLEXIBLE_MASS_PROPERTIES", B->name, "attach the reduction again (mech_flexible_attach) or make the body rigid (mech_flexible_detach)",
                      "body '%s' has mass properties (%.9g kg) that differ from those of its reduced mesh (%.9g kg): the elastic and rigid inertia would be "
                      "inconsistent", B->name, B->mass, X->fe_mass);
            fail = true;
            continue;
        }
        int k = mbdef_add_flex(def, map[b], X->model.nmodes, X->model.ninterfaces);
        if (k < 0) {
            fail = true;
            break;
        }
        MbFlexDef *F = &def->flex[k];
        size_t n = (size_t)X->model.nmodes;
        memcpy(F->omega2, X->model.omega2, n * sizeof(double)), memcpy(F->zeta, X->model.zeta, n * sizeof(double));
        memcpy(F->ell, X->model.ell, 6 * n * sizeof(double)), memcpy(F->dJ, X->model.dJ, 9 * n * sizeof(double));
        for (int i = 0; i < X->model.ninterfaces; i++) {
            mv3_copy(F->interfaces[i].point, X->model.interfaces[i].point);
            memcpy(F->interfaces[i].phi, X->model.interfaces[i].phi, 3 * n * sizeof(double));
            memcpy(F->interfaces[i].psi, X->model.interfaces[i].psi, 3 * n * sizeof(double));
            int j = asm_joint_index(a, X->interface_joint[i]);
            if (j < 0 || a->joints[j].def.parent != b) {
                mdiag_add(d, MD_ERROR, "FLEXIBLE_INTERFACE_JOINT", B->name, "reduce the body again with the current joints (mech_flexible_reduce)",
                          "interface %d of flexible body '%s' is ridden by joint '%s', which %s", i, B->name, X->interface_joint[i],
                          j < 0 ? "does not exist" : "no longer has the body as its parent");
                fail = true;
            }
        }
    }
    for (int j = 0; j < nj; j++) {
        if (drop_joint[j]) continue;
        MbJointDef J = a->joints[j].def;
        int p = J.parent;
        if (p >= 0 && map[p] == -2) {
            MPose t;
            mpose_mul(&t, &in_parent[p], &J.parent_frame);
            J.parent_frame = t;
            p = ancestor[p];
        }
        if (map[J.child] == -2) {
            mdiag_add(d, MD_ERROR, "MERGED_CHILD", J.name, NULL, "joint '%s' moves a massless link that was merged as a frame", J.name);
            fail = true;
            continue;
        }
        int idx = mbdef_add_joint(def, J.name, J.type, p < 0 ? -1 : map[p], map[J.child]);
        if (idx < 0) {
            fail = true;
            break;
        }
        MbJointDef *out = &def->joints[idx];
        char nm[MB_NAME];
        memcpy(nm, out->name, sizeof nm);
        *out = J;
        memcpy(out->name, nm, sizeof nm);
        out->parent = p < 0 ? -1 : map[p];
        out->child = map[J.child];
        out->parent_interface = -1;
        const AsmFlexible *PX = p >= 0 && J.parent == p ? a->bodies[p].flexible : NULL;
        for (int i = 0; PX && i < PX->model.ninterfaces; i++)
            if (!strcmp(PX->interface_joint[i], J.name)) out->parent_interface = i;
    }
    for (int c = 0; c < a->ncouplings && !fail; c++) {
        const AsmCoupling *C = &a->couplings[c];
        int f = mbdef_joint_index(def, C->follower), dr = mbdef_joint_index(def, C->driver);
        if (f < 0 || dr < 0) {
            mdiag_add(d, MD_ERROR, "INVALID_COUPLING", C->name, NULL, "coupling '%s' refers to a joint that is not in the model", C->name);
            fail = true;
        } else
            mbdef_add_coupling(def, C->name, f, dr, C->ratio, C->offset);
    }
    free(map), free(in_parent), free(ancestor), free(drop_joint);
    if (fail) {
        mbdef_free(def);
        return NULL;
    }
    return def;
}

/* ------------------------------------------------------------------------------------------------ summary */

/* contact-relevant summary of a collision or environment shape (names as asm_contact_shapes gives them) */
static JsonValue *contact_geom_summary(const AsmGeom *G, const char *body, int index) {
    JsonValue *g = json_object();
    char name[MB_NAME];
    if (G->name[0])
        snprintf(name, sizeof name, "%s", G->name);
    else if (!body)
        snprintf(name, sizeof name, "environment%d", index);
    else
        snprintf(name, sizeof name, "%.40s.collision%d", body, index);
    json_set_string(g, "name", name);
    json_set_string(g, "type", asm_geom_type_name(G->type));
    if (G->type == AG_BOX) json_set(g, "size_mm", json_vec3(G->size[0] * 1e3, G->size[1] * 1e3, G->size[2] * 1e3));
    if (G->type == AG_SPHERE || G->type == AG_CAPSULE || G->type == AG_CYLINDER) json_set_number(g, "radius_mm", G->size[0] * 1e3);
    if (G->type == AG_CAPSULE || G->type == AG_CYLINDER) json_set_number(g, "length_mm", G->size[1] * 1e3);
    if (G->has_friction) json_set_number(g, "friction", G->friction), json_set_string(g, "friction_source", asm_source_name(G->contact_source));
    json_set_number(g, "restitution", G->has_restitution ? G->restitution : 0);
    if (G->group) json_set_int(g, "group", G->group);
    return g;
}

JsonValue *asm_summary_json(const Assembly *a) {
    JsonValue *o = json_object();
    json_set_string(o, "name", a->name);
    json_set_string(o, "source_format", a->source_format);
    JsonValue *bs = json_set_array(o, "bodies");
    double total = 0;
    for (int i = 0; i < a->nbodies; i++) {
        const AsmBody *B = &a->bodies[i];
        JsonValue *b = json_object();
        json_set_string(b, "name", B->name);
        if (B->has_inertial) {
            json_set_number(b, "mass_kg", B->mass);
            json_set(b, "com_mm", json_vec3(B->com[0] * 1e3, B->com[1] * 1e3, B->com[2] * 1e3));
            double w[3], ax[9];
            inertia_principal(B->inertia, w, ax);
            json_set(b, "principal_inertia_kg_mm2", json_vec3(w[0] * 1e6, w[1] * 1e6, w[2] * 1e6));
            json_set_string(b, "mass_provenance", asm_source_name(B->inertial_source));
            if (B->inertial_note[0]) json_set_string(b, "mass_note", B->inertial_note);
            total += B->mass;
        } else
            json_set_string(b, "mass_provenance", "none (massless frame)");
        if (B->part[0]) json_set_string(b, "part", B->part);
        json_set_int(b, "frames", B->nframes);
        json_set_int(b, "geometries", B->ngeoms);
        JsonValue *cs = NULL;
        for (int k = 0; k < B->ngeoms; k++) {
            if (strcmp(B->geoms[k].role, "collision")) continue;
            if (!cs) cs = json_set_array(b, "collision_shapes");
            json_push(cs, contact_geom_summary(&B->geoms[k], B->name, k));
        }
        if (B->material[0]) json_set_string(b, "material", B->material);
        if (B->flexible) {
            const MbFlexDef *F = &B->flexible->model;
            JsonValue *fx = json_set_object(b, "flexible");
            json_set_string(fx, "method", "craig_bampton (first-order floating frame)");
            json_set_int(fx, "elastic_coordinates", F->nmodes);
            json_set_number(fx, "lowest_frequency_hz", sqrt(F->omega2[0]) / (2 * M_PI));
            json_set_number(fx, "highest_frequency_hz", sqrt(F->omega2[F->nmodes - 1]) / (2 * M_PI));
            JsonValue *ij = json_set_array(fx, "interface_joints");
            for (int i = 0; i < F->ninterfaces; i++) json_push(ij, json_string(B->flexible->interface_joint[i]));
        }
        json_push(bs, b);
    }
    json_set_number(o, "total_mass_kg", total);
    JsonValue *js = json_set_array(o, "joints");
    for (int i = 0; i < a->njoints; i++) {
        const MbJointDef *J = &a->joints[i].def;
        JsonValue *j = json_object();
        json_set_string(j, "name", J->name);
        json_set_string(j, "type", mb_joint_type_name(J->type));
        json_set_string(j, "parent", J->parent < 0 ? "world" : a->bodies[J->parent].name);
        json_set_string(j, "child", a->bodies[J->child].name);
        json_set_string(j, "motion", J->motion == MB_ACTUATED ? "actuated" : (J->motion == MB_PRESCRIBED ? "prescribed" : "passive"));
        json_push(js, j);
    }
    json_set_int(o, "couplings", a->ncouplings);
    JsonValue *env = json_set_array(o, "environment");
    for (int i = 0; i < a->nenvironment; i++) json_push(env, contact_geom_summary(&a->environment[i], NULL, i));
    json_set(o, "assumptions", notes_json(a->assumptions, a->nassumptions));
    json_set(o, "unsupported", notes_json(a->unsupported, a->nunsupported));
    return o;
}
