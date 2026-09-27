/* mech_ops.c - MCP operations of the mechanics layer: assembly and study editing, validation, dynamics jobs, result queries
 * and FEM load assessment of parts. Handlers run under the engine lock (except mech_results_query); the mechanical state
 * of the open project is cached per engine and written to <project>/mechanics/ after every change. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../core/base64.h"
#include "../core/sha256.h"
#include "../ctl/ops_internal.h"
#include "../ctl/lpbf_build.h"
#include "../ctl/lpbf_calibrate.h"
#include "../ctl/print_analysis.h"
#include "../ctl/static_analysis.h"
#include "../fem/hex8.h"
#include "assembly.h"
#include "loads.h"
#include "mech_ops.h"
#include "mech_struct.h"
#include "mech_vib.h"
#include "mech_flex.h"
#include "mechsim.h"
#include "mechspec.h"
#include "mechunits.h"

struct LpbfSettings;
static bool supports_block(const JsonValue *sup, bool remove_default, LpbfSettings *sp, OpResult *out);

/* ------------------------------------------------------------------------------------------------ project state */

typedef struct MechCache {
    Engine *e;
    char project_id[33];
    Assembly *asm;
    MechStudy *study;
} MechCache;

enum { MECH_CACHE_SLOTS = 32 };
static MechCache g_cache[MECH_CACHE_SLOTS];
static pthread_mutex_t g_cache_mtx = PTHREAD_MUTEX_INITIALIZER;

static void mech_dir(const Project *p, char *out, size_t cap) { path_join(out, cap, p->dir, "mechanics"); }

static void cache_reset(MechCache *c) {
    asm_free(c->asm);
    mspec_free(c->study);
    c->asm = NULL, c->study = NULL, c->project_id[0] = 0;
}

static MechCache *mech_state(Engine *e, OpResult *out) {
    if (!op_need_project(e, out)) return NULL;
    pthread_mutex_lock(&g_cache_mtx);
    MechCache *c = NULL;
    for (int i = 0; i < MECH_CACHE_SLOTS && !c; i++)
        if (g_cache[i].e == e) c = &g_cache[i];
    for (int i = 0; i < MECH_CACHE_SLOTS && !c; i++)
        if (!g_cache[i].e) c = &g_cache[i], c->e = e;
    pthread_mutex_unlock(&g_cache_mtx);
    if (!c) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "too many engines hold mechanical state");
        return NULL;
    }
    Project *p = e->proj;
    if (strcmp(c->project_id, p->id) != 0) {
        cache_reset(c);
        snprintf(c->project_id, sizeof c->project_id, "%s", p->id);
        char dir[NV_PATH_MAX], path[NV_PATH_MAX];
        mech_dir(p, dir, sizeof dir);
        MechDiag d;
        mdiag_init(&d);
        path_join(path, sizeof path, dir, "assembly.json");
        if (path_is_file(path)) c->asm = asm_load_json(path, NULL, &d);
        path_join(path, sizeof path, dir, "study.json");
        if (path_is_file(path)) {
            JsonError je;
            JsonValue *doc = json_read_file(path, 16 << 20, &je);
            if (doc) c->study = mspec_from_json(doc, &d);
            json_free(doc);
        }
        mdiag_free(&d);
    }
    return c;
}

static bool mech_save(Engine *e, MechCache *c, OpResult *out) {
    char dir[NV_PATH_MAX], path[NV_PATH_MAX], err[512];
    mech_dir(e->proj, dir, sizeof dir);
    if (!engine_resolve_write_path(e, NULL, dir, true, true, dir, sizeof dir, err, sizeof err)) {
        op_fail(out, NV_ERR_PERMISSION, "the project folder must be writable", "cannot store the mechanical model: %s", err);
        return false;
    }
    bool ok = true;
    if (c->asm) {
        JsonValue *doc = asm_to_json(c->asm);
        path_join(path, sizeof path, dir, "assembly.json");
        ok &= doc && json_write_file(path, doc, JSON_PRETTY);
        json_free(doc);
    }
    if (c->study) {
        JsonValue *doc = mspec_to_json(c->study);
        path_join(path, sizeof path, dir, "study.json");
        ok &= doc && json_write_file(path, doc, JSON_PRETTY);
        json_free(doc);
    }
    if (!ok) op_fail(out, NV_ERR_IO, NULL, "cannot write the mechanical model to %s", dir);
    return ok;
}

static MechStudy *study_of(MechCache *c, Engine *e) {
    if (!c->study) {
        c->study = mspec_new(e->proj->name);
        if (c->study) c->study->require_units = true;
    }
    return c->study;
}

/* ------------------------------------------------------------------------------------------------ diagnostics */

static JsonValue *diag_list(const MechDiag *d, MechSeverity sev) {
    JsonValue *a = json_array();
    for (int i = 0; i < d->n; i++) {
        const MechMessage *m = &d->msgs[i];
        if (m->severity != sev) continue;
        JsonValue *o = json_object();
        json_set_string(o, "code", m->code);
        if (m->subject[0]) json_set_string(o, "subject", m->subject);
        json_set_string(o, "message", m->message);
        if (m->hint[0]) json_set_string(o, "hint", m->hint);
        json_push(a, o);
    }
    return a;
}

static void set_diagnostics(JsonValue *v, const MechDiag *d) {
    json_set(v, "errors", diag_list(d, MD_ERROR));
    json_set(v, "missing_inputs", diag_list(d, MD_MISSING_INPUT));
    json_set(v, "warnings", diag_list(d, MD_WARNING));
    json_set(v, "notes", diag_list(d, MD_INFO));
}

/* fails the operation with the first error or missing input and attaches every diagnostic */
static void fail_diag(OpResult *out, const MechDiag *d, const char *what) {
    const MechMessage *first = NULL;
    for (int i = 0; i < d->n && !first; i++)
        if (d->msgs[i].severity == MD_MISSING_INPUT) first = &d->msgs[i];
    for (int i = 0; i < d->n && !first; i++)
        if (d->msgs[i].severity == MD_ERROR) first = &d->msgs[i];
    NvErr code = first && first->severity == MD_MISSING_INPUT ? NV_ERR_PRECONDITION : NV_ERR_INVALID_PARAMS;
    if (first && (!strcmp(first->code, "OUT_OF_MEMORY"))) code = NV_ERR_RESOURCE_LIMIT;
    op_fail(out, code, first && first->hint[0] ? first->hint : "see details.missing_inputs and details.errors", "%s%s%s%s%s", what, first ? ": " : "",
            first ? first->subject : "", first && first->subject[0] ? ": " : "", first ? first->message : "");
    op_fail_detail(out, "missing_inputs", diag_list(d, MD_MISSING_INPUT));
    op_fail_detail(out, "errors", diag_list(d, MD_ERROR));
    op_fail_detail(out, "warnings", diag_list(d, MD_WARNING));
}

static JsonValue *merged_units(const JsonValue *own, const JsonValue *request_units) {
    JsonValue *u = own && own->type == JSON_OBJECT ? json_clone(own) : json_object();
    for (size_t i = 0; request_units && i < json_len(request_units); i++)
        if (!json_get(u, json_key_at(request_units, i))) json_set(u, json_key_at(request_units, i), json_clone(json_value_at(request_units, i)));
    return u;
}

/* ------------------------------------------------------------------------------------------------ capabilities */

void mech_capabilities_json(JsonValue *v) {
    JsonValue *mech = json_set_object(v, "mechanics");
    json_set_string(mech, "contract", "navier-mech 0.5.0 (operations named mech_*)");
    json_set_string(mech, "conventions",
                    "build frame, SI internally; quaternions (w, x, y, z) map body to parent; spatial vectors [angular; linear]; joint wrenches are "
                    "exerted by the parent on the child, in the child joint frame about its origin; inline MCP definitions need explicit units");
    JsonValue *lv = json_set_array(mech, "fidelity_levels");
    static const struct {
        int level;
        const char *name, *status, *what, *not_included;
    } L[] = {
        {1, "kinematics", "available inside the dynamics runtime and mech_assembly_inspect (assembly, loop closure, constraint rank)", "", ""},
        {2, "rigid dynamics", "available: mech_dynamics_run",
         "rigid bodies in generalised coordinates; revolute, prismatic, spherical, free and fixed joints; closed loops as exact constraints "
         "with measured drift and explicit projection; joint limits as located impact events with Newton restitution; springs, dampers, "
         "regularised Coulomb joint friction, gear couplings and compliant transmissions with backlash; effort, servo and DC-motor "
         "actuators; sampled PID controllers with delay and anti-windup; sensors with seeded noise, bias, latency, filtering and "
         "quantisation; energy, work and dissipation ledger; RKMK4 integration",
         "elastic deformation, motor inductance, torque ripple, cogging, thermal derating of motors; contact is available as rigid contact "
         "(sphere, capsule, box, plane; Coulomb friction; Newton restitution) with first-order time stepping when enabled"},
        {3, "rigid motion + FEM load assessment", "available: mech_fem_assess",
         "quasi-static linear elasticity of one part at a load snapshot: joint wrenches and persistent contact forces distributed over "
         "named selections (force and moment reproduced exactly) plus d'Alembert body forces; isostatic 3-2-1 support with reported "
         "reactions; isotropic project material (shared solver) or orthotropic printed material with axes from the print orientation "
         "(mech_material_define, mechanics solver) and a named strength criterion (maximum stress, Tsai-Wu, Tsai-Hill) evaluated against "
         "strengths with their source (mech_structure_query)",
         "vibration, dynamic amplification, impact stress, local contact pressure, geometric or material nonlinearity, layer-scale "
         "defects, fatigue; no printed-material data are built in"},
        {4, "flexible dynamics", "available: mech_flexible_reduce + mech_flexible_attach (coupled flexible multibody dynamics in mech_dynamics_run), mech_modal_run (natural modes of a part) and mech_transient_assess (elastic response of a moving part, one-way, mode-acceleration method)",
         "flexible bodies from meshed parts by Craig-Bampton reduction (rigid interfaces at child joints, clamped root) in a first-order floating frame formulation: the elastic coordinates are integrated with the rigid motion and act back on it (two-way), with modal damping stated with its source; interface deflection, rotation and strain energy histories and peaks; free-free and clamped modes with consistent mass; exact modal integration of sampled joint, contact and d'Alembert loads; dynamic and quasi-static peak stresses and their amplification",
         "second-order effects of the large motion on the stiffness (centrifugal stiffening or softening, geometric stiffness from axial load), large deformation, contact on the deformed surface of a flexible body (contact geometry rides interfaces), stress recovery from the coupled elastic coordinates, impact response"},
        {5, "nonlinear mechanical response", "not available", "", ""},
    };
    for (size_t i = 0; i < sizeof L / sizeof *L; i++) {
        JsonValue *o = json_object();
        json_set_int(o, "level", L[i].level);
        json_set_string(o, "name", L[i].name);
        json_set_string(o, "status", L[i].status);
        if (L[i].what[0]) json_set_string(o, "includes", L[i].what);
        if (L[i].not_included[0]) json_set_string(o, "not_included", L[i].not_included);
        json_push(lv, o);
    }
    JsonValue *imp = json_set_array(mech, "assembly_inputs");
    json_push(imp, json_string("navier-assembly JSON (native, units per quantity, provenance)"));
    json_push(imp, json_string("URDF subset (links, inertials, visual/collision geometry, fixed/revolute/continuous/prismatic/floating joints, "
                               "limits, dynamics, mimic); unsupported elements are listed"));
    json_push(imp, json_string("project STL parts with a declared fill model (solid, shell_infill, measured_mass)"));
}

/* ------------------------------------------------------------------------------------------------ fill and parts */

static bool fill_from_json(const JsonValue *f, FillSpec *fs, AsmSource *src, MechDiag *d, const char *subject) {
    memset(fs, 0, sizeof *fs);
    fs->model = FILL_MODEL_COUNT;
    int m = fill_model_from_name(json_get_str(f, "model", NULL));
    if (m < 0) {
        mdiag_add(d, MD_MISSING_INPUT, "FILL_MODEL_REQUIRED", subject, "solid, shell_infill or measured_mass", "the part needs a fill model");
        return false;
    }
    fs->model = (FillModel)m;
    char e[256];
    bool ok = true;
    const JsonValue *v;
    if ((v = json_get(f, "density")) && !mech_qty_from_json_ex(v, MQ_DENSITY, NULL, true, &fs->density, e, sizeof e))
        mdiag_add(d, MD_ERROR, "INVALID_QUANTITY", subject, NULL, "fill.density: %s", e), ok = false;
    if ((v = json_get(f, "shell_thickness")) && !mech_qty_from_json_ex(v, MQ_LENGTH, NULL, true, &fs->shell_thickness, e, sizeof e))
        mdiag_add(d, MD_ERROR, "INVALID_QUANTITY", subject, NULL, "fill.shell_thickness: %s", e), ok = false;
    if ((v = json_get(f, "measured_mass")) && !mech_qty_from_json_ex(v, MQ_MASS, NULL, true, &fs->measured_mass, e, sizeof e))
        mdiag_add(d, MD_ERROR, "INVALID_QUANTITY", subject, NULL, "fill.measured_mass: %s", e), ok = false;
    fs->infill_fraction = json_get_num(f, "infill_fraction", 0);
    int s = asm_source_from_name(json_get_str(f, "source", "user"));
    *src = s < 0 ? SRC_USER : (AsmSource)s;
    return ok;
}

/* mass properties of a project part (its analysis surface, in metres in the STL file frame) */
static bool part_mass_properties(Project *p, const char *part, const FillSpec *fill, MassProperties *mp, MechDiag *d) {
    Body *b = project_body(p, part);
    if (!b || strcmp(b->name, part)) {
        mdiag_add(d, MD_ERROR, "UNKNOWN_PART", part, "import the STL with geometry_import first", "project body '%s' does not exist", part);
        return false;
    }
    double *v = malloc((size_t)(3 * (b->surf.nv ? b->surf.nv : 1)) * sizeof *v);
    if (!v) return false;
    for (int i = 0; i < 3 * b->surf.nv; i++) v[i] = b->surf.v[i] * b->unit_scale;
    bool ok = mass_properties_from_mesh(v, b->surf.nv, b->surf.tri, b->surf.nt, fill, mp, d, part);
    free(v);
    return ok;
}

static JsonValue *explicit_mass_json(const MassProperties *mp, const FillSpec *fill, AsmSource fsrc, const char *part) {
    JsonValue *o = json_object();
    char buf[64];
    json_set_string(o, "source", "explicit");
    json_set_string(o, "provenance", "computed");
    snprintf(buf, sizeof buf, "%.17g kg", mp->mass);
    json_set_string(o, "mass", buf);
    JsonValue *com = json_set_array(o, "com");
    for (int k = 0; k < 3; k++) snprintf(buf, sizeof buf, "%.17g m", mp->com[k]), json_push(com, json_string(buf));
    JsonValue *in = json_set_object(o, "inertia");
    static const char *const N[6] = {"ixx", "iyy", "izz", "ixy", "ixz", "iyz"};
    static const int IDX[6] = {0, 4, 8, 1, 2, 5};
    for (int k = 0; k < 6; k++) snprintf(buf, sizeof buf, "%.17g kg*m^2", mp->inertia_com[IDX[k]]), json_set_string(in, N[k], buf);
    char note[400];
    snprintf(note, sizeof note, "computed from project part '%s' with %s fill (%s): volume %.6g cm^3, walls %.1f%% of the volume", part, fill_model_name(fill->model),
             asm_source_name(fsrc), mp->volume * 1e6, 100 * mp->wall_fraction);
    json_set_string(o, "note", note);
    return o;
}

static bool add_part_body(Engine *e, Assembly *a, const char *name, const char *part, const JsonValue *fillj, MechDiag *d) {
    FillSpec fill;
    AsmSource fsrc;
    if (!fill_from_json(fillj, &fill, &fsrc, d, name)) return false;
    MassProperties mp;
    if (!part_mass_properties(e->proj, part, &fill, &mp, d)) return false;
    JsonValue *body = json_object();
    json_set_string(body, "name", name);
    json_set_string(body, "part", part);
    json_set(body, "mass_properties", explicit_mass_json(&mp, &fill, fsrc, part));
    AsmLoadOptions opt = {.require_units = true};
    bool ok = asm_body_set_json(a, body, NULL, e->proj->dir, &opt, d);
    json_free(body);
    return ok;
}

/* ------------------------------------------------------------------------------------------------ editing operations */

static JsonValue *assembly_value(Engine *e, MechCache *c, const MechDiag *d) {
    JsonValue *v = json_object();
    json_set(v, "assembly", c->asm ? asm_summary_json(c->asm) : json_null());
    set_diagnostics(v, d);
    json_set_int(v, "revision", (long long)e->proj->revision);
    return v;
}

static void op_mech_assembly_import(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    const char *file = json_get_str(p, "file", NULL);
    const JsonValue *doc = json_get(p, "document"), *parts = json_get(p, "parts");
    if ((file != NULL) + (doc != NULL) + (parts != NULL) != 1) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "pass exactly one of file, document or parts", "no single assembly source given");
        return;
    }
    MechDiag d;
    mdiag_init(&d);
    AsmLoadOptions opt = {.root_joint = json_get_str(p, "root_joint", NULL), .max_file_bytes = e->cfg.max_stl_bytes, .max_triangles = e->cfg.max_triangles};
    Assembly *a = NULL;
    if (file) {
        char path[NV_PATH_MAX], err[1024];
        if (!engine_resolve_read_path(e, file, path, sizeof path, err, sizeof err)) {
            op_fail(out, NV_ERR_PERMISSION, NULL, "%s", err);
            mdiag_free(&d);
            return;
        }
        a = asm_load_file(path, &opt, &d);
    } else if (doc) {
        opt.require_units = true;
        a = asm_from_json(doc, e->proj->dir, &opt, &d);
    } else {
        a = asm_new(json_get_str(p, "name", e->proj->name));
        if (a) {
            snprintf(a->source_format, sizeof a->source_format, "stl");
            const JsonValue *g = json_get(p, "gravity");
            if (g) {
                char err[256];
                bool gok = json_len(g) == 3;
                for (int k = 0; k < 3 && gok; k++) gok = mech_qty_from_json_ex(json_at(g, (size_t)k), MQ_ACCELERATION, NULL, true, &a->gravity[k], err, sizeof err);
                if (!gok) mdiag_add(&d, MD_ERROR, "INVALID_QUANTITY", "gravity", NULL, "gravity needs three accelerations with units");
                a->gravity_source = SRC_USER;
            } else
                asm_note_assumption(a, "gravity", SRC_DEFAULT, "gravity not given: 9.80665 m/s^2 along -Z of the build frame");
            bool ok = d.nerrors == 0;
            for (size_t i = 0; i < json_len(parts) && ok; i++) {
                const JsonValue *pt = json_at(parts, i);
                const char *body = json_get_str(pt, "body", ""), *nm = json_get_str(pt, "name", body);
                ok = add_part_body(e, a, nm, body, json_get(pt, "fill"), &d);
            }
            if (!ok) asm_free(a), a = NULL;
        }
    }
    if (!a) {
        fail_diag(out, &d, "the assembly could not be imported");
        mdiag_free(&d);
        return;
    }
    asm_free(c->asm);
    c->asm = a;
    if (!mech_save(e, c, out)) {
        mdiag_free(&d);
        return;
    }
    engine_touch(e, "mech_assembly_import", "mechanical assembly '%s' (%d bodies, %d joints)", a->name, a->nbodies, a->njoints);
    JsonValue *v = assembly_value(e, c, &d);
    json_set_string(v, "next_step", "check mass properties and joints with mech_assembly_inspect; add joints with mech_joint_define and components with mech_component_define");
    op_succeed(out, v);
    mdiag_free(&d);
}

static void op_mech_body_define(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    const char *name = json_get_str(p, "name", "");
    const char *part = json_get_str(p, "part", NULL);
    if (part && json_get(p, "mass_properties")) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "give either part with fill or mass_properties", "both part and mass_properties were given");
        return;
    }
    if (!part && json_get(p, "fill")) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "fill describes the inside of a part: give part as well", "fill was given without part");
        return;
    }
    MechDiag d;
    mdiag_init(&d);
    Assembly *a = c->asm ? asm_clone(c->asm) : asm_new(e->proj->name);
    JsonValue *body = json_object();
    if (!a || !body) {
        asm_free(a), json_free(body);
        op_fail(out, NV_ERR_INTERNAL, NULL, "cannot copy the assembly");
        return;
    }
    /* the fields given replace the body's fields; the others are kept */
    json_set_string(body, "name", name);
    bool ok = true;
    if (part) {
        FillSpec fill;
        AsmSource fsrc;
        MassProperties mp;
        ok = fill_from_json(json_get(p, "fill"), &fill, &fsrc, &d, name) && part_mass_properties(e->proj, part, &fill, &mp, &d);
        if (ok) {
            json_set_string(body, "part", part);
            json_set(body, "mass_properties", explicit_mass_json(&mp, &fill, fsrc, part));
        }
    } else if (json_get(p, "mass_properties"))
        json_set(body, "mass_properties", json_clone(json_get(p, "mass_properties")));
    if (json_get(p, "frames")) json_set(body, "frames", json_clone(json_get(p, "frames")));
    if (json_get(p, "material")) json_set_string(body, "material", json_get_str(p, "material", ""));
    if (json_get(p, "collision")) {
        JsonValue *geo = json_set_object(body, "geometry");
        json_set(geo, "collision", json_clone(json_get(p, "collision")));
    }
    if (ok) {
        AsmLoadOptions opt = {.require_units = true};
        ok = asm_body_update_json(a, body, json_get(p, "units"), e->proj->dir, &opt, &d);
    }
    json_free(body);
    if (!ok) {
        fail_diag(out, &d, "the body could not be defined");
        asm_free(a);
        mdiag_free(&d);
        return;
    }
    asm_free(c->asm);
    c->asm = a;
    if (!mech_save(e, c, out)) {
        mdiag_free(&d);
        return;
    }
    engine_touch(e, "mech_body_define", "body '%s'", name);
    op_succeed(out, assembly_value(e, c, &d));
    mdiag_free(&d);
}

static void op_mech_joint_define(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    if (!c->asm) {
        op_fail(out, NV_ERR_PRECONDITION, "create the assembly with mech_assembly_import or mech_body_define", "there is no mechanical assembly");
        return;
    }
    MechDiag d;
    mdiag_init(&d);
    Assembly *a = asm_clone(c->asm);
    AsmLoadOptions opt = {.require_units = true};
    if (!a || !asm_joint_set_json(a, json_get(p, "joint"), json_get(p, "units"), &opt, &d)) {
        fail_diag(out, &d, "the joint could not be defined");
        asm_free(a);
        mdiag_free(&d);
        return;
    }
    asm_free(c->asm);
    c->asm = a;
    if (!mech_save(e, c, out)) {
        mdiag_free(&d);
        return;
    }
    engine_touch(e, "mech_joint_define", "joint '%s'", json_get_str(json_get(p, "joint"), "name", ""));
    op_succeed(out, assembly_value(e, c, &d));
    mdiag_free(&d);
}

static void op_mech_environment_define(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    if (!c->asm) {
        op_fail(out, NV_ERR_PRECONDITION, "create the assembly first", "there is no mechanical assembly");
        return;
    }
    MechDiag d;
    mdiag_init(&d);
    Assembly *a = asm_clone(c->asm);
    AsmLoadOptions opt = {.require_units = true};
    if (!a || !asm_environment_set_json(a, json_get(p, "shapes"), json_get(p, "units"), &opt, &d)) {
        fail_diag(out, &d, "the environment could not be defined");
        asm_free(a);
        mdiag_free(&d);
        return;
    }
    asm_free(c->asm);
    c->asm = a;
    if (!mech_save(e, c, out)) {
        mdiag_free(&d);
        return;
    }
    engine_touch(e, "mech_environment_define", "%d environment shapes", a->nenvironment);
    op_succeed(out, assembly_value(e, c, &d));
    mdiag_free(&d);
}

static JsonValue *study_value(Engine *e, const MechStudy *s, const MechDiag *d) {
    JsonValue *v = json_object();
    JsonValue *st = json_set_object(v, "study");
    json_set_int(st, "actuators", (long long)json_len(s->actuators));
    json_set_int(st, "sensors", (long long)json_len(s->sensors));
    json_set_int(st, "controllers", (long long)json_len(s->controllers));
    json_set(st, "settings", json_clone(s->settings));
    set_diagnostics(v, d);
    json_set_int(v, "revision", (long long)e->proj->revision);
    json_set_string(v, "next_step", "resolve and check everything with mech_study_validate");
    return v;
}

static void op_mech_component_define(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    MechStudy *s = study_of(c, e);
    if (!s) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    MechDiag d;
    mdiag_init(&d);
    JsonValue *def = json_clone(json_get(p, "definition"));
    if (def && json_get(p, "units")) json_set(def, "units", merged_units(json_get(def, "units"), json_get(p, "units")));
    const char *kind = json_get_str(p, "kind", "");
    bool ok = def && mspec_set_component(s, kind, def, &d);
    /* resolve now against the current assembly when possible, so unit and reference errors surface at once */
    if (ok && c->asm) {
        MechDiag rd;
        mdiag_init(&rd);
        MbModelDef *md = asm_to_model(c->asm, &rd);
        if (md && !mspec_resolve(s, md, &rd)) {
            for (int i = 0; i < rd.n; i++) {
                const MechMessage *m = &rd.msgs[i];
                if (m->severity == MD_ERROR && strstr(m->message, json_get_str(def, "name", "\x01")))
                    mdiag_add(&d, MD_ERROR, m->code, m->subject, m->hint, "%s", m->message), ok = false;
            }
        }
        mbdef_free(md);
        mdiag_free(&rd);
        if (!ok) mspec_remove_component(s, kind, json_get_str(def, "name", ""));
    }
    json_free(def);
    if (!ok) {
        fail_diag(out, &d, "the component could not be defined");
        mdiag_free(&d);
        return;
    }
    if (!mech_save(e, c, out)) {
        mdiag_free(&d);
        return;
    }
    engine_touch(e, "mech_component_define", "%s '%s'", kind, json_get_str(json_get(p, "definition"), "name", ""));
    op_succeed(out, study_value(e, s, &d));
    mdiag_free(&d);
}

static void op_mech_component_remove(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    const char *kind = json_get_str(p, "kind", ""), *name = json_get_str(p, "name", "");
    if (!c->study || !mspec_remove_component(c->study, kind, name)) {
        op_fail(out, NV_ERR_NOT_FOUND, NULL, "no %s named '%s'", kind, name);
        return;
    }
    if (!mech_save(e, c, out)) return;
    engine_touch(e, "mech_component_remove", "%s '%s' removed", kind, name);
    MechDiag d;
    mdiag_init(&d);
    op_succeed(out, study_value(e, c->study, &d));
    mdiag_free(&d);
}

static void op_mech_study_settings(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    MechStudy *s = study_of(c, e);
    MechDiag d;
    mdiag_init(&d);
    JsonValue *settings = NULL;
    if (json_get(p, "settings") || json_get(p, "units")) {
        settings = json_clone(s->settings);
        const JsonValue *ns = json_get(p, "settings");
        for (size_t i = 0; i < json_len(ns); i++) json_set(settings, json_key_at(ns, i), json_clone(json_value_at(ns, i)));
        if (json_get(p, "units")) json_set(settings, "units", merged_units(json_get(p, "units"), json_get(settings, "units")));
    }
    bool ok = mspec_set_settings(s, settings, json_get(p, "snapshots"), &d);
    json_free(settings);
    if (!ok) {
        fail_diag(out, &d, "the settings could not be applied");
        mdiag_free(&d);
        return;
    }
    if (!mech_save(e, c, out)) {
        mdiag_free(&d);
        return;
    }
    engine_touch(e, "mech_study_settings", "study settings");
    op_succeed(out, study_value(e, s, &d));
    mdiag_free(&d);
}

/* ------------------------------------------------------------------------------------------------ building a study */

static const char *const FIDELITY2_ASSUMPTIONS[] = {
    "rigid bodies: no elastic deformation of parts, joints or transmissions (except declared compliant transmissions)",
    "ideal joints: no clearance, no friction or damping beyond what is declared",
    "DC motors: inductance neglected (current follows voltage instantly), constant winding resistance",
    "controllers and sensors behave exactly as declared (period, delay, noise model)",
    NULL};

/* level 2 or 4 (a run with flexible bodies) for dynamics, 3 for load assessments; nflex = flexible bodies in the model */
static JsonValue *fidelity_json(int level, bool contact, int nflex) {
    JsonValue *f = json_object();
    json_set_int(f, "level", level);
    json_set_string(f, "name", level == 2   ? "rigid multibody dynamics"
                               : level == 4 ? "flexible multibody dynamics (first-order floating frame with reduced elastic bodies)"
                                            : "rigid motion plus quasi-static FEM load assessment");
    JsonValue *a = json_set_array(f, "assumptions");
    for (int i = 0; FIDELITY2_ASSUMPTIONS[i]; i++) {
        if (i == 0 && nflex) {
            json_push(a, json_stringf("bodies are rigid except %d flexible bodies, whose small linear elastic deformation (reduced Craig-Bampton coordinates with "
                                      "stated modal damping) is superposed on their large motion and acts back on it; second-order stiffness effects of the "
                                      "motion (centrifugal stiffening or softening, geometric stiffness from axial load) are neglected", nflex));
            json_push(a, json_string("flexible bodies: the root region is held by the reference frame and each interface is a rigid region at its child joint; "
                                     "static interface loads are exact, distributed inertia converges with the modes kept; no stresses are recovered in the run"));
            continue;
        }
        json_push(a, json_string(FIDELITY2_ASSUMPTIONS[i]));
    }
    if (contact) {
        json_push(a, json_string("rigid contact between the declared primitive shapes only (box, sphere, capsule, plane): no compliance, no contact pressure distribution"));
        json_push(a, json_string("Coulomb friction and Newton restitution with the declared coefficients, which are model parameters with their stated provenance"));
        json_push(a, json_string("first-order time stepping: contact forces are impulse / step averages; impacts are impulses and their average force depends on the step"));
    } else
        json_push(a, json_string("contact between bodies is not modelled: parts may pass through each other"));
    if (level == 3) {
        if (nflex)
            json_push(a, json_string("the motion comes from a run with flexible bodies: a flexible body's d'Alembert load uses the motion of its reference frame and "
                                     "omits its elastic inertia (the imbalance shows in the support reactions)"));
        json_push(a, json_string("the part is assessed at one instant with d'Alembert's principle; its elastic response does not feed back into the motion"));
        json_push(a, json_string("joint wrenches enter the part as a linear traction field over the named selection (distributing coupling)"));
        json_push(a, json_string("isotropic linear elastic material at the reference temperature; no anisotropy, plasticity or damage"));
        if (contact)
            json_push(a, json_string("contact forces of persistent contact enter the part as resultant-preserving tractions over the named selection; local contact stresses are not resolved"));
    }
    return f;
}

typedef struct Built {
    MbModelDef *def;
    JsonValue *spec;
    char hash[65];
} Built;

static void built_free(Built *b) {
    mbdef_free(b->def);
    json_free(b->spec);
    memset(b, 0, sizeof *b);
}

static void hash_json(const JsonValue *v, char out[65]) {
    size_t len;
    char *text = json_dump(v, JSON_SORTED, &len, NULL);
    sha256_hex_of(text ? text : "", text ? len : 0, out);
    free(text);
}

/* resolves and checks the whole study; on success the canonical specification and its hash */
static bool build_study(MechCache *c, MechDiag *d, Built *b) {
    memset(b, 0, sizeof *b);
    if (!c->asm) {
        mdiag_add(d, MD_MISSING_INPUT, "NO_ASSEMBLY", "assembly", "create it with mech_assembly_import", "there is no mechanical assembly");
        return false;
    }
    b->def = asm_to_model(c->asm, d);
    if (!b->def) return false;
    for (int i = 0; i < c->asm->nbodies; i++) {
        const AsmFlexible *X = c->asm->bodies[i].flexible;
        const char *mh = X ? json_get_str(X->provenance, "mesh_hash", "") : NULL;
        if (X && c->e && c->e->proj && strcmp(mh, c->e->proj->mesh.hash))
            mdiag_add(d, MD_WARNING, "FLEXIBLE_MODEL_MESH_CHANGED", c->asm->bodies[i].name, "reduce the body again with mech_flexible_reduce and attach the result",
                      "the elastic model of '%s' was reduced from a mesh that is no longer the project mesh", c->asm->bodies[i].name);
    }
    if (!c->study) {
        mdiag_add(d, MD_MISSING_INPUT, "STUDY_TIME", "settings", "set end_time and max_step with mech_study_settings", "the study has no settings");
        return false;
    }
    if (!mspec_resolve(c->study, b->def, d)) return false;
    MbOptions opt;
    mb_options_default(&opt);
    MechStudy *s = c->study;
    static ContactShape shapes[512];
    int nshapes = 0;
    if (s->st.contact) {
        nshapes = asm_contact_shapes(c->asm, b->def, shapes, 512, d);
        if (nshapes < 0) return false;
    } else {
        bool any = c->asm->nenvironment > 0;
        for (int i = 0; i < c->asm->nbodies && !any; i++)
            for (int g = 0; g < c->asm->bodies[i].ngeoms && !any; g++) any = !strcmp(c->asm->bodies[i].geoms[g].role, "collision");
        if (any)
            mdiag_add(d, MD_WARNING, "CONTACT_DISABLED", "settings.contact", "enable it with mech_study_settings {\"settings\": {\"contact\": {\"enabled\": true}}}",
                      "collision geometry exists but contact is disabled: bodies pass through each other");
    }
    MechSim *ms = mechsim_new(b->def, &opt, s->act, s->nact, s->ctl, s->nctl, s->sen, s->nsen, &s->st, d);
    bool ok = ms && mechsim_set_contact_shapes(ms, shapes, nshapes) && mechsim_init(ms, d) && mechsim_set_snapshot_times(ms, s->snapshot_s, s->nsnapshots);
    if (ms && !ok && d->nerrors == 0 && d->nmissing == 0)
        mdiag_add(d, MD_ERROR, "INVALID_SNAPSHOTS", "snapshots", NULL, "snapshot times must be non-negative and increasing");
    mechsim_free(ms);
    if (!ok) return false;
    b->spec = json_object();
    json_set_string(b->spec, "format", "navier-mech-run-spec");
    json_set_int(b->spec, "version", 1);
    json_set(b->spec, "assembly", asm_to_json(c->asm));
    json_set(b->spec, "study", mspec_canonical_json(s, b->def));
    JsonValue *integ = json_set_object(b->spec, "integrator");
    json_set_string(integ, "method", "RKMK4 (Runge-Kutta-Munthe-Kaas, order 4) with located limit events and mass-weighted drift projection");
    json_set_number(integ, "rank_tolerance", opt.rank_tol);
    json_set_number(integ, "projection_tolerance", opt.projection_tol);
    json_set_number(integ, "event_tolerance", opt.event_tol);
    hash_json(b->spec, b->hash);
    return true;
}

/* model definition, resolved study and contact shapes from a stored specification */
static bool from_spec(const JsonValue *spec, MechDiag *d, MbModelDef **def, MechStudy **study, ContactShape **shapes, int *nshapes) {
    *def = NULL, *study = NULL;
    if (shapes) *shapes = NULL, *nshapes = 0;
    if (strcmp(json_get_str(spec, "format", ""), "navier-mech-run-spec")) {
        mdiag_add(d, MD_ERROR, "INVALID_SPEC", "spec", NULL, "not a navier-mech run specification");
        return false;
    }
    Assembly *a = asm_from_json(json_get(spec, "assembly"), NULL, NULL, d);
    if (!a) return false;
    *def = asm_to_model(a, d);
    if (!*def) {
        asm_free(a);
        return false;
    }
    *study = mspec_from_json(json_get(spec, "study"), d);
    if (!*study || !mspec_resolve(*study, *def, d)) {
        mspec_free(*study), mbdef_free(*def), asm_free(a);
        *study = NULL, *def = NULL;
        return false;
    }
    if (shapes && (*study)->st.contact) {
        *shapes = calloc(512, sizeof **shapes);
        int n = *shapes ? asm_contact_shapes(a, *def, *shapes, 512, d) : -1;
        if (n < 0) {
            free(*shapes), mspec_free(*study), mbdef_free(*def), asm_free(a);
            *shapes = NULL, *study = NULL, *def = NULL;
            return false;
        }
        *nshapes = n;
    }
    asm_free(a);
    return true;
}

/* ------------------------------------------------------------------------------------------------ inspect and validate */

static void op_mech_assembly_inspect(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    JsonValue *v = json_object();
    if (!c->asm) {
        json_set(v, "assembly", json_null());
        json_set_string(v, "next_step", "create the assembly with mech_assembly_import (file, document or project parts)");
        op_succeed(out, v);
        return;
    }
    json_set(v, "assembly", asm_summary_json(c->asm));
    MechDiag d;
    mdiag_init(&d);
    if (json_get_bool(p, "analyze", true)) {
        MbModelDef *def = asm_to_model(c->asm, &d);
        MbModel *m = def ? mb_compile(def, NULL, &d) : NULL;
        JsonValue *mo = json_set_object(v, "model");
        json_set_bool(mo, "compiles", m != NULL);
        if (m) {
            json_set_int(mo, "coordinates", m->nv);
            json_set_number(mo, "length_scale_m", m->length_scale);
            JsonValue *co = json_set_array(mo, "joints");
            MbSim *sim = mb_sim_new(m);
            MbState *st = mb_state_new(m);
            bool assembled = sim && st && mb_state_initial(m, st, &d) && mb_assemble(sim, st, &d);
            json_set_bool(mo, "assembles", assembled);
            for (int j = 0; j < m->njoints; j++) {
                const MbJointDef *J = &m->joints[j];
                JsonValue *o = json_object();
                json_set_string(o, "name", J->name);
                json_set_string(o, "type", mb_joint_type_name(J->type));
                json_set_bool(o, "closes_loop", m->joint_loop[j]);
                json_set_int(o, "degrees_of_freedom", m->joint_loop[j] ? 0 : mb_joint_nv(J->type));
                if (!m->joint_loop[j] && (J->type == MB_REVOLUTE || J->type == MB_PRISMATIC) && st) {
                    bool rot = J->type == MB_REVOLUTE;
                    double q = st->q[m->joint_qadr[j]];
                    json_set_number(o, rot ? "assembled_position_deg" : "assembled_position_mm", rot ? q * 180 / M_PI : q * 1e3);
                    json_set_number(o, rot ? "reflected_armature_kg_m2" : "reflected_armature_kg", J->armature);
                }
                json_push(co, o);
            }
            mb_state_free(st);
            mb_sim_free(sim);
        }
        mb_model_free(m);
        mbdef_free(def);
    }
    if (json_get_bool(p, "include_document", false)) {
        json_set(v, "assembly_document", asm_to_json(c->asm));
        if (c->study) json_set(v, "study_document", mspec_to_json(c->study));
    }
    set_diagnostics(v, &d);
    op_succeed(out, v);
    mdiag_free(&d);
}

static void op_mech_study_validate(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    MechDiag d;
    mdiag_init(&d);
    Built b;
    bool ok = build_study(c, &d, &b);
    JsonValue *v = json_object();
    json_set_bool(v, "valid", ok);
    if (ok) {
        json_set_string(v, "spec_hash", b.hash);
        json_set(v, "fidelity", fidelity_json(b.def->nflex ? 4 : 2, c->study->st.contact, b.def->nflex));
        JsonValue *st = json_set_object(v, "resolved");
        const MechStudy *s = c->study;
        json_set_number(st, "end_time_s", s->st.end_time);
        json_set_number(st, "max_step_s", s->st.max_step);
        json_set_int(st, "actuators", s->nact), json_set_int(st, "sensors", s->nsen), json_set_int(st, "controllers", s->nctl);
        json_set(st, "snapshots_s", json_numbers(s->snapshot_s, (size_t)s->nsnapshots));
    }
    if (c->asm) json_set(v, "assumptions", json_clone(json_get(asm_summary_json(c->asm), "assumptions")));
    set_diagnostics(v, &d);
    json_set_string(v, "next_step", ok ? "start the run with mech_dynamics_run" : "resolve missing_inputs and errors, then validate again");
    built_free(&b);
    op_succeed(out, v);
    mdiag_free(&d);
}

/* ------------------------------------------------------------------------------------------------ dynamics job */

typedef struct MechRunData {
    MbModelDef *def;
    MechStudy *study;
    ContactShape *shapes;
    int nshapes;
    bool ts_check;
    char run_dir[NV_PATH_MAX];
} MechRunData;

static void mech_run_free(void *p) {
    MechRunData *r = p;
    if (!r) return;
    mbdef_free(r->def);
    mspec_free(r->study);
    free(r->shapes);
    free(r);
}

/* binary histories: "NVMHIST1", uint32 channels, uint32 rows, per channel (uint16 name length, name, uint16 unit length, unit),
 * then channels * rows little-endian doubles, channel by channel */
static bool write_histories_bin(const MechSim *ms, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t nc = (uint32_t)mechsim_channels(ms), nr = (uint32_t)mechsim_rows(ms);
    fwrite("NVMHIST1", 1, 8, f);
    fwrite(&nc, 4, 1, f), fwrite(&nr, 4, 1, f);
    for (uint32_t c = 0; c < nc; c++) {
        const char *n = mechsim_channel_name(ms, (int)c), *u = mechsim_channel_unit(ms, (int)c);
        uint16_t ln = (uint16_t)strlen(n), lu = (uint16_t)strlen(u);
        fwrite(&ln, 2, 1, f), fwrite(n, 1, ln, f), fwrite(&lu, 2, 1, f), fwrite(u, 1, lu, f);
    }
    for (uint32_t c = 0; c < nc; c++) fwrite(mechsim_column(ms, (int)c), sizeof(double), nr, f);
    bool ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

static double rel_change(double a, double b) {
    double s = fmax(fabs(a), fabs(b));
    return s > 0 ? fabs(a - b) / s : 0;
}

/* relative change with a floor below which differences are resolution or round-off (stated in the report) */
static double rel_change_floor(double a, double b, double floor) {
    double s = fmax(fmax(fabs(a), fabs(b)), floor);
    return s > 0 ? fabs(a - b) / s : 0;
}

/* compares the key results of two summaries and envelopes */
static JsonValue *timestep_comparison(const JsonValue *s1, const JsonValue *s2, const JsonValue *e1, const JsonValue *e2, double h) {
    JsonValue *o = json_object();
    json_set_number(o, "max_step_s", h);
    json_set_number(o, "half_step_s", h / 2);
    JsonValue *items = json_set_array(o, "changes");
    double worst = 0;
    static const char *const CKEYS[] = {"rms_error", "max_abs_error", NULL}, *const AKEYS[] = {"peak_effort", "rms_effort", "peak_current_A", "electrical_energy_J", NULL};
    const char *const *keys[2] = {CKEYS, AKEYS};
    static const char *const ARR[2] = {"controllers", "actuators"};
    for (int g = 0; g < 2; g++) {
        const JsonValue *a1 = json_get(s1, ARR[g]), *a2 = json_get(s2, ARR[g]);
        for (size_t i = 0; i < json_len(a1) && i < json_len(a2); i++)
            for (int k = 0; keys[g][k]; k++) {
                const JsonValue *x1 = json_get(json_at(a1, i), keys[g][k]), *x2 = json_get(json_at(a2, i), keys[g][k]);
                if (!x1 || !x2) continue;
                double c = rel_change(x1->u.number, x2->u.number);
                JsonValue *it = json_object();
                char what[160];
                snprintf(what, sizeof what, "%s.%s", json_get_str(json_at(a1, i), "name", "?"), keys[g][k]);
                json_set_string(it, "quantity", what);
                json_set_number(it, "value", x1->u.number);
                json_set_number(it, "value_half_step", x2->u.number);
                json_set_number(it, "relative_change", c);
                json_push(items, it);
                worst = fmax(worst, c);
            }
    }
    double env_scale[2] = {0, 0}; /* largest joint force and moment: changes below 1e-9 of them are round-off */
    for (size_t i = 0; i < json_len(e1); i++)
        for (int k = 0; k < 2; k++)
            env_scale[k] = fmax(env_scale[k], json_get_num(json_get(json_at(e1, i), k ? "peak_moment" : "peak_force"), k ? "magnitude_Nm" : "magnitude_N", 0));
    for (size_t i = 0; i < json_len(e1) && i < json_len(e2); i++)
        for (int k = 0; k < 2; k++) {
            const char *pk = k ? "peak_moment" : "peak_force", *mk = k ? "magnitude_Nm" : "magnitude_N";
            double a = json_get_num(json_get(json_at(e1, i), pk), mk, 0), b = json_get_num(json_get(json_at(e2, i), pk), mk, 0);
            double c = rel_change_floor(a, b, 1e-9 * env_scale[k]);
            JsonValue *it = json_object();
            char what[160];
            snprintf(what, sizeof what, "%s.%s", json_get_str(json_at(e1, i), "joint", "?"), pk);
            json_set_string(it, "quantity", what);
            json_set_number(it, "value", a), json_set_number(it, "value_half_step", b), json_set_number(it, "relative_change", c);
            json_push(items, it);
            worst = fmax(worst, c);
        }
    /* contact pairs, matched by shape names */
    const JsonValue *p1 = json_get(json_get(s1, "contact"), "pairs"), *p2 = json_get(json_get(s2, "contact"), "pairs");
    static const char *const PKEYS[] = {"peak_normal_force_N", "normal_force_at_end_N", "friction_force_at_end_N", "sliding_time_s", "peak_impact_impulse_Ns", NULL};
    double pscale[5] = {0, 0, 0, 0, 0};
    for (size_t i = 0; i < json_len(p1); i++)
        for (int k = 0; PKEYS[k]; k++) pscale[k] = fmax(pscale[k], fabs(json_get_num(json_at(p1, i), PKEYS[k], 0)));
    pscale[2] = pscale[1] = pscale[0] = fmax(pscale[0], fmax(pscale[1], pscale[2]));
    for (size_t i = 0; i < json_len(p1); i++) {
        const JsonValue *x1 = json_at(p1, i), *x2 = NULL;
        for (size_t j = 0; j < json_len(p2); j++)
            if (!strcmp(json_get_str(json_at(p2, j), "shape_a", ""), json_get_str(x1, "shape_a", "?")) &&
                !strcmp(json_get_str(json_at(p2, j), "shape_b", ""), json_get_str(x1, "shape_b", "?")))
                x2 = json_at(p2, j);
        for (int k = 0; PKEYS[k]; k++) {
            double a = json_get_num(x1, PKEYS[k], 0), b = json_get_num(x2, PKEYS[k], 0);
            if (a == 0 && b == 0) continue;
            /* sliding time is resolved to about ten steps; forces and impulses below 1e-9 of the largest are round-off */
            double c = rel_change_floor(a, b, k == 3 ? 10 * h : 1e-9 * pscale[k]);
            JsonValue *it = json_object();
            char what[200];
            snprintf(what, sizeof what, "contact.%s.%s.%s", json_get_str(x1, "shape_a", "?"), json_get_str(x1, "shape_b", "?"), PKEYS[k]);
            json_set_string(it, "quantity", what);
            json_set_number(it, "value", a), json_set_number(it, "value_half_step", b), json_set_number(it, "relative_change", c);
            json_push(items, it);
            worst = fmax(worst, c);
        }
    }
    /* flexible bodies: peak interface deflection and strain energy, matched by body and interface */
    const JsonValue *f1 = json_get(s1, "flexible_bodies"), *f2 = json_get(s2, "flexible_bodies");
    for (size_t i = 0; i < json_len(f1) && i < json_len(f2); i++) {
        const JsonValue *b1 = json_at(f1, i), *b2 = json_at(f2, i);
        const JsonValue *i1 = json_get(b1, "interfaces"), *i2 = json_get(b2, "interfaces");
        for (size_t k = 0; k <= json_len(i1) && k <= json_len(i2); k++) {
            bool energy = k == json_len(i1);
            const JsonValue *x1 = energy ? b1 : json_at(i1, k), *x2 = energy ? b2 : json_at(i2, k);
            const char *key = energy ? "peak_strain_energy_j" : "peak_deflection_m";
            double a = json_get_num(x1, key, 0), b = json_get_num(x2, key, 0);
            double c = rel_change_floor(a, b, 1e-9 * fmax(fabs(a), fabs(b)));
            JsonValue *it = json_object();
            char what[200];
            if (energy) snprintf(what, sizeof what, "%s.%s", json_get_str(b1, "body", "?"), key);
            else snprintf(what, sizeof what, "%s.%s.%s", json_get_str(b1, "body", "?"), json_get_str(x1, "joint", "interface"), key);
            json_set_string(it, "quantity", what);
            json_set_number(it, "value", a), json_set_number(it, "value_half_step", b), json_set_number(it, "relative_change", c);
            json_push(items, it);
            worst = fmax(worst, c);
        }
    }
    json_set_number(o, "largest_relative_change", worst);
    json_set_string(o, "reading",
                    "a small change supports (does not prove) time-step convergence of these outputs; controller and sensor periods are the same in both "
                    "runs, so the change measures the integration of the mechanics between samples");
    json_set_string(o, "floors",
                    "relative changes use floors: 1e-9 of the largest joint load or contact force or impulse (round-off), and ten steps for sliding times "
                    "(the time resolution of first-order contact)");
    return o;
}

static MechSim *run_once(MechRunData *R, double max_step, Job *job, double p0, double p1, MechDiag *d, char *code, size_t codelen, char *err, size_t errlen) {
    MechStudy *s = R->study;
    StudySettings st = s->st;
    st.max_step = max_step;
    MbOptions opt;
    mb_options_default(&opt);
    MechSim *ms = mechsim_new(R->def, &opt, s->act, s->nact, s->ctl, s->nctl, s->sen, s->nsen, &st, d);
    if (!ms || !mechsim_set_contact_shapes(ms, R->shapes, R->nshapes) || !mechsim_init(ms, d) || !mechsim_set_snapshot_times(ms, s->snapshot_s, s->nsnapshots)) {
        snprintf(code, codelen, "PRECONDITION_FAILED");
        snprintf(err, errlen, "the study no longer builds: %s", d->n ? d->msgs[0].message : "unknown problem");
        mechsim_free(ms);
        return NULL;
    }
    enum { CHUNKS = 200 };
    for (int k = 1; k <= CHUNKS; k++) {
        if (job_cancel_requested(job)) {
            snprintf(code, codelen, "CANCELLED");
            snprintf(err, errlen, "cancelled at t = %.6g s", mechsim_time(ms));
            mechsim_free(ms);
            return NULL;
        }
        double t = st.end_time * k / CHUNKS;
        if (!mechsim_run_until(ms, t, err, errlen)) {
            snprintf(code, codelen, "SOLVER_FAILED");
            mechsim_free(ms);
            return NULL;
        }
        char stage[96];
        snprintf(stage, sizeof stage, "t = %.4g s of %.4g s (max step %.3g s)", mechsim_time(ms), st.end_time, max_step);
        job_progress(job, p0 + (p1 - p0) * k / CHUNKS, stage);
    }
    return ms;
}

static bool mech_run_job(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    MechRunData *R = data;
    MechDiag d;
    mdiag_init(&d);
    MechSim *ms = run_once(R, R->study->st.max_step, job, 0.0, R->ts_check ? 0.6 : 0.95, &d, code, codelen, err, errlen);
    if (!ms) {
        mdiag_free(&d);
        return false;
    }
    char path[NV_PATH_MAX];
    JsonValue *summary = mechsim_summary_json(ms), *envelope = mechsim_load_envelope_json(ms), *snaps = mechsim_snapshots_json(ms);
    json_set(summary, "fidelity", fidelity_json(R->def->nflex ? 4 : 2, R->study->st.contact, R->def->nflex));
    json_set(summary, "diagnostics", mdiag_json(&d));
    if (R->ts_check) {
        MechDiag d2;
        mdiag_init(&d2);
        double h_eff = mechsim_step_limit(ms); /* flexible bodies can hold the steps below max_step: halve the step actually taken */
        MechSim *ms2 = run_once(R, h_eff / 2, job, 0.6, 0.95, &d2, code, codelen, err, errlen);
        if (!ms2) {
            mdiag_free(&d2);
            json_free(summary), json_free(envelope), json_free(snaps);
            mechsim_free(ms);
            mdiag_free(&d);
            return false;
        }
        JsonValue *s2 = mechsim_summary_json(ms2), *e2 = mechsim_load_envelope_json(ms2);
        json_set(summary, "time_step_sensitivity", timestep_comparison(summary, s2, envelope, e2, h_eff));
        json_free(s2), json_free(e2);
        mechsim_free(ms2);
        mdiag_free(&d2);
    }
    job_progress(job, 0.97, "writing results");
    bool ok = true;
    path_join(path, sizeof path, R->run_dir, "histories.bin");
    ok &= write_histories_bin(ms, path);
    path_join(path, sizeof path, R->run_dir, "histories.csv");
    ok &= mechsim_write_csv(ms, path, err, errlen);
    path_join(path, sizeof path, R->run_dir, "envelope.json");
    ok &= json_write_file(path, envelope, JSON_PRETTY);
    path_join(path, sizeof path, R->run_dir, "snapshots.json");
    ok &= json_write_file(path, snaps, JSON_PRETTY);
    JsonValue *files = json_set_array(summary, "files");
    static const char *const F[] = {"spec.json", "summary.json", "histories.bin", "histories.csv", "envelope.json", "snapshots.json", NULL};
    for (int i = 0; F[i]; i++) json_push(files, json_string(F[i]));
    path_join(path, sizeof path, R->run_dir, "summary.json");
    ok &= json_write_file(path, summary, JSON_PRETTY);
    json_free(envelope), json_free(snaps);
    mechsim_free(ms);
    mdiag_free(&d);
    if (!ok) {
        json_free(summary);
        snprintf(code, codelen, "IO_ERROR");
        snprintf(err, errlen, "cannot write the results to %s", R->run_dir);
        return false;
    }
    job_set_summary(job, summary);
    return true;
}

static bool make_run_dir(Engine *e, OpResult *out, char *id, size_t idcap, char *dir, size_t dircap) {
    char runs[NV_PATH_MAX], err[1024];
    jobs_new_id(e->jobs, id, idcap);
    path_join(runs, sizeof runs, e->proj->dir, "runs");
    path_join(dir, dircap, runs, id);
    if (path_exists(dir) || !engine_resolve_write_path(e, NULL, dir, true, true, dir, dircap, err, sizeof err)) {
        op_fail(out, NV_ERR_IO, NULL, "cannot create the run directory %s%s%s", dir, err[0] ? ": " : "", err);
        return false;
    }
    return true;
}

static void op_mech_dynamics_run(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    MechDiag d;
    mdiag_init(&d);
    Built b;
    if (!build_study(c, &d, &b)) {
        fail_diag(out, &d, "the study cannot run");
        built_free(&b);
        mdiag_free(&d);
        return;
    }
    bool ts = json_get_bool(p, "time_step_check", false);
    json_set_bool(b.spec, "time_step_check", ts);
    hash_json(b.spec, b.hash);
    char active[64];
    if (jobs_find_active(e->jobs, b.hash, active, sizeof active)) {
        JsonValue *v = json_object();
        json_set_string(v, "job_id", active);
        json_set_bool(v, "deduplicated", true);
        json_set_string(v, "spec_hash", b.hash);
        built_free(&b);
        mdiag_free(&d);
        op_succeed(out, v);
        return;
    }
    char id[64], dir[NV_PATH_MAX], path[NV_PATH_MAX], now[32], err[512];
    if (!make_run_dir(e, out, id, sizeof id, dir, sizeof dir)) {
        built_free(&b);
        mdiag_free(&d);
        return;
    }
    iso_time_now(now, sizeof now);
    JsonValue *spec = json_clone(b.spec);
    json_set_string(spec, "job_id", id);
    json_set_string(spec, "created", now);
    json_set_int(spec, "project_revision", (long long)e->proj->revision);
    json_set_string(spec, "spec_hash", b.hash);
    json_set_string(spec, "spec_hash_covers", "every field except job_id, created, project_revision and spec_hash");
    path_join(path, sizeof path, dir, "spec.json");
    bool written = json_write_file(path, spec, JSON_PRETTY | JSON_SORTED);
    json_free(spec);
    MechRunData *R = calloc(1, sizeof *R);
    MechDiag rd;
    mdiag_init(&rd);
    bool ok = written && R && from_spec(b.spec, &rd, &R->def, &R->study, &R->shapes, &R->nshapes);
    if (!ok) {
        mech_run_free(R);
        built_free(&b);
        mdiag_free(&d);
        if (written)
            op_fail(out, NV_ERR_INTERNAL, NULL, "the stored specification does not rebuild");
        else
            op_fail(out, NV_ERR_IO, NULL, "cannot write %s", path);
        op_fail_detail(out, "errors", diag_list(&rd, MD_ERROR));
        op_fail_detail(out, "missing_inputs", diag_list(&rd, MD_MISSING_INPUT));
        mdiag_free(&rd);
        return;
    }
    mdiag_free(&rd);
    R->ts_check = ts;
    snprintf(R->run_dir, sizeof R->run_dir, "%s", dir);
    JobSpec js = {id, "mech_dynamics", json_get_str(p, "label", ""), dir, b.hash, e->proj->id, e->proj->name, e->proj->revision, R, mech_run_job, mech_run_free};
    if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
        built_free(&b);
        mdiag_free(&d);
        op_fail(out, NV_ERR_BUSY, "check job_list and retry when jobs have finished", "%s", err);
        return;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "state", "queued");
    json_set_string(v, "analysis", "mech_dynamics");
    json_set_string(v, "run_directory", dir);
    json_set_string(v, "spec_hash", b.hash);
    json_set(v, "fidelity", fidelity_json(b.def->nflex ? 4 : 2, c->study->st.contact, b.def->nflex));
    set_diagnostics(v, &d);
    json_set_string(v, "next_step", "poll job_status until succeeded, then mech_results_query (summary, history, envelope, snapshots)");
    built_free(&b);
    mdiag_free(&d);
    op_succeed(out, v);
}

/* ------------------------------------------------------------------------------------------------ results */

/* locked: the caller holds the engine lock (op_project_run_dir would take it again) */
/* the run directory of a job of the open project, for operations that already hold the engine lock (op_project_run_dir takes it) */
static bool run_dir_locked(Engine *e, const char *id, char *dir, size_t cap) {
    size_t n = strlen(id);
    bool valid = n > 0 && n < 64;
    for (size_t i = 0; i < n && valid; i++)
        valid = (id[i] >= 'a' && id[i] <= 'z') || (id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '_' || id[i] == '-';
    char runs[NV_PATH_MAX];
    return valid && e->proj && path_join(runs, sizeof runs, e->proj->dir, "runs") && path_join(dir, cap, runs, id) && path_is_dir(dir);
}

static bool mech_run_dir(Engine *e, OpResult *out, const char *id, char *dir, size_t cap, bool locked) {
    char path[NV_PATH_MAX];
    bool found = locked ? run_dir_locked(e, id, dir, cap) : op_project_run_dir(e, id, dir, cap);
    if (!found) {
        op_fail(out, NV_ERR_NOT_FOUND, "use job_list for the jobs of the open project", "no run directory for job '%s'", id);
        return false;
    }
    path_join(path, sizeof path, dir, "spec.json");
    JsonError je;
    JsonValue *spec = json_read_file(path, 64 << 20, &je);
    bool mech = spec && !strcmp(json_get_str(spec, "format", ""), "navier-mech-run-spec");
    json_free(spec);
    if (!mech) {
        op_fail(out, NV_ERR_UNSUPPORTED, "use results_query for structural and thermal analyses", "job '%s' is not a mechanical dynamics run", id);
        return false;
    }
    return true;
}

static void op_mech_results_query(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", "");
    char dir[NV_PATH_MAX], path[NV_PATH_MAX];
    if (!mech_run_dir(e, out, id, dir, sizeof dir, false)) return;
    const char *what = json_get_str(p, "what", "summary");
    path_join(path, sizeof path, dir, "summary.json");
    if (!path_is_file(path)) {
        JsonValue *st = jobs_status_json(e->jobs, id, 0);
        op_fail(out, NV_ERR_PRECONDITION, "poll job_status until the job has succeeded", "job '%s' has no results (state %s)", id,
                json_get_str(st, "state", "unknown"));
        json_free(st);
        return;
    }
    JsonError je;
    if (!strcmp(what, "summary") || !strcmp(what, "envelope") || !strcmp(what, "specification") || !strcmp(what, "snapshots")) {
        const char *file = !strcmp(what, "summary") ? "summary.json" : (!strcmp(what, "envelope") ? "envelope.json" : (!strcmp(what, "snapshots") ? "snapshots.json" : "spec.json"));
        path_join(path, sizeof path, dir, file);
        JsonValue *doc = json_read_file(path, 256 << 20, &je);
        if (!doc) {
            op_fail(out, NV_ERR_IO, NULL, "cannot read %s", path);
            return;
        }
        if (!strcmp(what, "snapshots"))
            for (size_t i = 0; i < json_len(doc); i++) json_remove(json_at(doc, i), "state_base64");
        JsonValue *v = json_object();
        json_set_string(v, "job_id", id);
        json_set(v, what, doc);
        op_succeed(out, v);
        return;
    }
    /* channels and history from the binary file */
    path_join(path, sizeof path, dir, "histories.bin");
    FILE *f = fopen(path, "rb");
    char magic[8];
    uint32_t nc = 0, nr = 0;
    if (!f || fread(magic, 1, 8, f) != 8 || memcmp(magic, "NVMHIST1", 8) || fread(&nc, 4, 1, f) != 1 || fread(&nr, 4, 1, f) != 1 || nc > 100000) {
        if (f) fclose(f);
        op_fail(out, NV_ERR_IO, NULL, "cannot read %s", path);
        return;
    }
    char (*names)[80] = calloc(nc, 80), (*units)[16] = calloc(nc, 16);
    bool ok = names && units;
    for (uint32_t c = 0; c < nc && ok; c++) {
        uint16_t ln, lu;
        ok = fread(&ln, 2, 1, f) == 1 && ln < 80 && fread(names[c], 1, ln, f) == ln && fread(&lu, 2, 1, f) == 1 && lu < 16 && fread(units[c], 1, lu, f) == lu;
    }
    long data0 = ftell(f);
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_int(v, "rows", nr);
    if (ok && !strcmp(what, "channels")) {
        JsonValue *arr = json_set_array(v, "channels");
        for (uint32_t c = 0; c < nc; c++) {
            JsonValue *o = json_object();
            json_set_string(o, "name", names[c]), json_set_string(o, "unit", units[c]);
            json_push(arr, o);
        }
    } else if (ok) {
        const JsonValue *want = json_get(p, "channels");
        if (!json_len(want)) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "list channel names (what: channels shows them)", "history needs channels");
            ok = false;
        }
        double *t = malloc((size_t)(nr ? nr : 1) * sizeof(double));
        ok = ok && t && fseek(f, data0, SEEK_SET) == 0 && fread(t, sizeof(double), nr, f) == nr; /* channel 0 is time */
        double t0 = json_get_num(p, "time_from", 0), t1 = json_get_num(p, "time_to", nr ? t[nr - 1] : 0);
        int maxp = (int)json_get_int(p, "max_points", 500);
        uint32_t first = 0, last = 0, count = 0;
        for (uint32_t r = 0; ok && r < nr; r++)
            if (t[r] >= t0 - 1e-12 && t[r] <= t1 + 1e-12) {
                if (!count) first = r;
                last = r, count++;
            }
        uint32_t stride = count > (uint32_t)maxp ? (count + (uint32_t)maxp - 1) / (uint32_t)maxp : 1;
        JsonValue *series = ok ? json_set_object(v, "series") : NULL;
        JsonValue *tj = ok ? json_array() : NULL;
        for (uint32_t r = first; ok && count && r <= last; r += stride) json_push(tj, json_number(t[r]));
        if (ok) json_set(series, "time", tj);
        JsonValue *un = ok ? json_set_object(v, "units") : NULL;
        if (un) json_set_string(un, "time", "s");
        double *col = malloc((size_t)(nr ? nr : 1) * sizeof(double));
        for (size_t w = 0; ok && w < json_len(want); w++) {
            const char *cn = json_str(json_at(want, w));
            uint32_t ci = nc;
            for (uint32_t c = 0; cn && c < nc; c++)
                if (!strcmp(names[c], cn)) ci = c;
            if (ci == nc) {
                op_fail(out, NV_ERR_NOT_FOUND, "what: channels lists the available names", "unknown channel '%s'", cn ? cn : "");
                ok = false;
                break;
            }
            if (!col || fseek(f, data0 + (long)ci * (long)nr * (long)sizeof(double), SEEK_SET) || fread(col, sizeof(double), nr, f) != nr) {
                op_fail(out, NV_ERR_IO, NULL, "cannot read channel '%s'", cn);
                ok = false;
                break;
            }
            JsonValue *arr = json_array();
            for (uint32_t r = first; count && r <= last; r += stride) json_push(arr, json_number(col[r]));
            json_set(series, cn, arr);
            json_set_string(un, cn, units[ci]);
        }
        json_set_int(v, "stride", stride);
        free(col), free(t);
    }
    fclose(f);
    free(names), free(units);
    if (!ok) {
        json_free(v);
        if (!out->error) op_fail(out, NV_ERR_IO, NULL, "cannot read the histories");
        return;
    }
    op_succeed(out, v);
}

/* ------------------------------------------------------------------------------------------------ FEM load assessment */

/* copies the elements of one meshed body (with their nodes, faces, sets and re-entrant edges) into a new model */
static bool extract_body(const StaticModel *m, int bi, StaticModel *s) {
    memset(s, 0, sizeof *s);
    int *nmap = malloc((size_t)m->nnodes * sizeof *nmap), *emap = malloc((size_t)(m->nelems ? m->nelems : 1) * sizeof *emap);
    int *fmap = malloc((size_t)(m->nfaces ? m->nfaces : 1) * sizeof *fmap);
    if (!nmap || !emap || !fmap) {
        free(nmap), free(emap), free(fmap);
        return false;
    }
    for (int i = 0; i < m->nnodes; i++) nmap[i] = -1;
    int ne = 0, nn = 0, nf = 0;
    for (int e = 0; e < m->nelems; e++) {
        emap[e] = m->elem_body[e] == bi ? ne++ : -1;
        if (emap[e] < 0) continue;
        for (int a = 0; a < 8; a++) {
            int nd = m->conn[8 * (size_t)e + a];
            if (nmap[nd] < 0) nmap[nd] = nn++;
        }
    }
    for (int f = 0; f < m->nfaces; f++) fmap[f] = emap[m->face_elem[f]] >= 0 ? nf++ : -1;
    s->nnodes = nn, s->nelems = ne, s->nfaces = nf;
    s->xyz = malloc((size_t)(3 * (nn ? nn : 1)) * sizeof(double));
    s->conn = malloc((size_t)(8 * (ne ? ne : 1)) * sizeof(int));
    s->elem_mat = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    s->elem_body = calloc((size_t)(ne ? ne : 1), 1);
    s->face_elem = malloc((size_t)(nf ? nf : 1) * sizeof(int));
    s->face_local = malloc((size_t)(nf ? nf : 1));
    s->fixed = calloc((size_t)(3 * (nn ? nn : 1)), 1);
    s->fixed_value = calloc((size_t)(3 * (nn ? nn : 1)), sizeof(double));
    s->nodal_force = calloc((size_t)(3 * (nn ? nn : 1)), sizeof(double));
    s->node_bc = malloc((size_t)(nn ? nn : 1) * sizeof(int));
    bool ok = s->xyz && s->conn && s->elem_mat && s->elem_body && s->face_elem && s->face_local && s->fixed && s->fixed_value && s->nodal_force && s->node_bc;
    if (ok) {
        for (int i = 0; i < m->nnodes; i++)
            if (nmap[i] >= 0) memcpy(s->xyz + 3 * (size_t)nmap[i], m->xyz + 3 * (size_t)i, 3 * sizeof(double));
        for (int e = 0; e < m->nelems; e++) {
            if (emap[e] < 0) continue;
            for (int a = 0; a < 8; a++) s->conn[8 * (size_t)emap[e] + a] = nmap[m->conn[8 * (size_t)e + a]];
            s->elem_mat[emap[e]] = m->elem_mat[e];
        }
        for (int f = 0; f < m->nfaces; f++)
            if (fmap[f] >= 0) s->face_elem[fmap[f]] = emap[m->face_elem[f]], s->face_local[fmap[f]] = m->face_local[f];
        for (int i = 0; i < nn; i++) s->node_bc[i] = -1;
        memcpy(s->h, m->h, sizeof s->h);
        s->nbodies = 1;
        snprintf(s->body_name[0], sizeof s->body_name[0], "%s", m->body_name[bi]);
        memcpy(s->mesh_hash, m->mesh_hash, sizeof s->mesh_hash);
        s->nmat = m->nmat;
        memcpy(s->mat, m->mat, sizeof s->mat);
        memcpy(s->mat_id, m->mat_id, sizeof s->mat_id);
        memcpy(s->mat_status, m->mat_status, sizeof s->mat_status);
        s->settings = m->settings;
        for (int k = 0; k < m->nsets && ok; k++) {
            const SaSet *src = &m->set[k];
            SaSet *dst = &s->set[s->nsets];
            memset(dst, 0, sizeof *dst);
            dst->faces = malloc((size_t)(src->nfaces ? src->nfaces : 1) * sizeof(int));
            dst->nodes = malloc((size_t)(src->nnodes ? src->nnodes : 1) * sizeof(int));
            if (!dst->faces || !dst->nodes) {
                free(dst->faces), free(dst->nodes);
                ok = false;
                break;
            }
            for (int i = 0; i < src->nfaces; i++)
                if (fmap[src->faces[i]] >= 0) dst->faces[dst->nfaces++] = fmap[src->faces[i]];
            for (int i = 0; i < src->nnodes; i++)
                if (nmap[src->nodes[i]] >= 0) dst->nodes[dst->nnodes++] = nmap[src->nodes[i]];
            if (!dst->nfaces) {
                free(dst->faces), free(dst->nodes);
                continue;
            }
            /* renumbering can break the sorted order the result queries rely on */
            for (int i = 1; i < dst->nnodes; i++)
                for (int j = i; j > 0 && dst->nodes[j - 1] > dst->nodes[j]; j--) {
                    int t = dst->nodes[j];
                    dst->nodes[j] = dst->nodes[j - 1], dst->nodes[j - 1] = t;
                }
            snprintf(dst->name, sizeof dst->name, "%s", src->name);
            s->nsets++;
        }
        if (ok && m->nconcave) {
            s->concave_seg = malloc((size_t)(6 * m->nconcave) * sizeof(double));
            if (s->concave_seg) memcpy(s->concave_seg, m->concave_seg, (size_t)(6 * m->nconcave) * sizeof(double)), s->nconcave = m->nconcave;
        }
    }
    free(nmap), free(emap), free(fmap);
    if (!ok) static_model_free(s);
    return ok;
}

static bool mech_fem_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    StaticResults *r = data;
    if (!static_job_run(job, data, code, codelen, err, errlen)) return false;
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, r->run_dir, "mech_assessment.json");
    JsonError je;
    JsonValue *rep = json_read_file(path, 16 << 20, &je);
    if (rep && r->summary) {
        /* reactions of the isostatic support: they must vanish for self-equilibrated loads */
        const JsonValue *nodes = json_get(json_get(rep, "support"), "nodes");
        double rmax = 0;
        for (size_t i = 0; i < json_len(nodes); i++) {
            long long nd = json_get_int(json_at(nodes, i), "node", -1);
            if (nd < 0 || nd >= r->model.nnodes) continue;
            for (int k = 0; k < 3; k++) rmax = fmax(rmax, fabs(r->sol.reaction[3 * (size_t)nd + (size_t)k]));
        }
        double scale = json_get_num(json_get(rep, "balance"), "load_scale_N", 0);
        JsonValue *chk = json_set_object(rep, "support_reactions");
        json_set_number(chk, "largest_component_N", rmax);
        json_set_number(chk, "relative_to_load_scale", scale > 0 ? rmax / scale : 0);
        json_set_string(chk, "reading", rmax <= 1e-6 * scale ? "negligible: the load set is self-equilibrated and the stresses do not depend on the support choice"
                                                               : "not negligible: the rigid-body and FEM models disagree (mass, centre of mass or load placement); stresses near the support nodes are not physical");
        json_set(r->summary, "mechanical_assessment", json_clone(rep));
        json_set(r->summary, "fidelity", fidelity_json(3, json_get_bool(rep, "source_contact", false), (int)json_get_int(rep, "source_flexible_bodies", 0)));
        json_write_file(path, rep, JSON_PRETTY);
        path_join(path, sizeof path, r->run_dir, "summary.json");
        json_write_file(path, r->summary, JSON_PRETTY | JSON_SORTED);
        job_set_summary(job, json_clone(r->summary));
    }
    json_free(rep);
    return true;
}

/* ------------------------------------------------------------------------------------------------ load assessment context */

/* a load that can enter the assessed part: a joint adjacent to the body or one of the body's contact shapes, with the named
 * selection where it enters (sel < 0: none given) */
typedef struct AssessAttach {
    bool contact;
    int index;       /* joint or contact shape index */
    int side;        /* joints: +1 the body is the child, -1 the parent */
    char name[MB_NAME];
    char selection[64];
    int sel;         /* index into the part's mesh sets */
} AssessAttach;

typedef struct AssessLoad {
    double F[3], M[3], P[3]; /* FEM axes: force on the part, moment about P, application point (m) */
    int points;              /* contact points */
    bool active;             /* joints always; contact shapes when they carry load */
} AssessLoad;

enum { ASSESS_MAX_ATTACH = 128 };

typedef struct AssessCtx {
    char job_id[64], bname[MB_NAME], part_name[64], run_dir[NV_PATH_MAX];
    JsonValue *spec;
    MechStructSetup ortho;
    bool use_ortho;
    MechDiag d;
    MbModelDef *def;
    MechStudy *study;
    MechSim *ms;
    ContactShape *cshapes;
    int ncshapes, bi;
    const MbModel *m;
    StaticModel full, sub;
    double *density;
    FemMeshView mv;
    JsonValue *errors, *warnings;
    double Rbf[9], pbf[3], align[3], diag;
    MPose *pose;
    int natt;
    AssessAttach att[ASSESS_MAX_ATTACH];
} AssessCtx;

static void assess_close(AssessCtx *c) {
    if (!c) return;
    json_free(c->spec);
    mech_struct_setup_free(&c->ortho);
    mdiag_free(&c->d);
    mechsim_free(c->ms);
    mspec_free(c->study);
    mbdef_free(c->def);
    free(c->cshapes), free(c->density), free(c->pose);
    static_model_free(&c->full), static_model_free(&c->sub);
    json_free(c->errors), json_free(c->warnings);
    free(c);
}

/* the dynamics study of a job rebuilt (not advanced), the part's mesh and material, the body-to-mesh frame and the load
 * sources with their selections; fails the operation with a hint */
static AssessCtx *assess_open(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return NULL;
    AssessCtx *c = calloc(1, sizeof *c);
    if (!c) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return NULL;
    }
    mdiag_init(&c->d);
    c->errors = json_array(), c->warnings = json_array();
    Project *proj = e->proj;
    snprintf(c->job_id, sizeof c->job_id, "%s", json_get_str(p, "job_id", ""));
    snprintf(c->bname, sizeof c->bname, "%s", json_get_str(p, "body", ""));
    char path[NV_PATH_MAX];
    if (!mech_run_dir(e, out, c->job_id, c->run_dir, sizeof c->run_dir, true)) goto fail;
    JsonError je;
    path_join(path, sizeof path, c->run_dir, "spec.json");
    c->spec = json_read_file(path, 64 << 20, &je);
    if (!c->spec) {
        op_fail(out, NV_ERR_PRECONDITION, "wait for the dynamics job to succeed", "job '%s' has no stored specification", c->job_id);
        goto fail;
    }
    /* part linked to the body */
    const char *part = json_get_str(p, "part", NULL);
    const JsonValue *abodies = json_get(json_get(c->spec, "assembly"), "bodies");
    for (size_t i = 0; !part && i < json_len(abodies); i++)
        if (!strcmp(json_get_str(json_at(abodies, i), "name", ""), c->bname)) part = json_get_str(json_at(abodies, i), "part", NULL);
    Body *pb = part ? project_body(proj, part) : NULL;
    if (!pb || strcmp(pb->name, part)) {
        op_fail(out, NV_ERR_PRECONDITION, "create the body from a project part (mech_assembly_import parts, or mech_body_define part) so its frame is known",
                "body '%s' is not linked to a meshed project part", c->bname);
        goto fail;
    }
    snprintf(c->part_name, sizeof c->part_name, "%s", pb->name);
    memcpy(c->Rbf, pb->place.R, sizeof c->Rbf);
    mv3_copy(c->pbf, pb->place.t);
    /* material model: the part's project material (isotropic, default) or a mechanics orthotropic record with print axes */
    const JsonValue *mmj = json_get(p, "material_model");
    const char *mmtype = json_get_str(mmj, "type", "project_material");
    c->use_ortho = !strcmp(mmtype, "orthotropic");
    if (!c->use_ortho && strcmp(mmtype, "project_material")) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "material_model.type is project_material or orthotropic", "unknown material model '%s'", mmtype);
        goto fail;
    }
    if (c->use_ortho && !mech_struct_setup(proj, mmj, &c->ortho, out)) goto fail;
    /* the dynamics study */
    bool ok = from_spec(c->spec, &c->d, &c->def, &c->study, &c->cshapes, &c->ncshapes);
    MbOptions opt;
    mb_options_default(&opt);
    if (ok) {
        c->ms = mechsim_new(c->def, &opt, c->study->act, c->study->nact, c->study->ctl, c->study->nctl, c->study->sen, c->study->nsen, &c->study->st, &c->d);
        ok = c->ms && mechsim_set_contact_shapes(c->ms, c->cshapes, c->ncshapes) && mechsim_init(c->ms, &c->d);
    }
    c->m = ok ? mechsim_model(c->ms) : NULL;
    c->bi = ok ? mbdef_body_index(c->def, c->bname) : -1;
    if (ok && c->bi < 0) {
        mdiag_add(&c->d, MD_ERROR, "UNKNOWN_BODY", c->bname, "a massless link merged as a frame has no loads of its own", "body '%s' is not in the model", c->bname);
        ok = false;
    }
    if (!ok) {
        fail_diag(out, &c->d, "the dynamics study cannot be rebuilt");
        goto fail;
    }
    const MbModel *m = c->m;
    for (int f = 0; f < m->nflex; f++)
        if (m->flex[f].body == c->bi)
            json_push(c->warnings, json_stringf("'%s' is a flexible body in this run: the assessment loads it with its joint and contact loads and the d'Alembert "
                                                "forces of its reference frame; its elastic inertia is not included (the imbalance appears in the support reactions)",
                                                c->bname));
    c->pose = malloc((size_t)(m->nbodies ? m->nbodies : 1) * sizeof *c->pose);
    if (!c->pose) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        goto fail;
    }
    /* FEM model of the part */
    StaticSettings sset = {HEX8_INCOMPATIBLE, SOLID_SOLVER_AUTO, 1e-10, 293.15};
    if (!static_model_build(proj, &sset, &c->full, c->errors, c->warnings)) {
        bool only_supports = json_len(c->errors) > 0;
        for (size_t i = 0; i < json_len(c->errors); i++) only_supports &= !strcmp(json_get_str(json_at(c->errors, i), "code", ""), "INSUFFICIENT_CONSTRAINTS");
        if (!only_supports) {
            const JsonValue *first = json_at(c->errors, 0);
            op_fail(out, nv_err_from_name(json_get_str(first, "code", "PRECONDITION_FAILED")), json_get_str(first, "hint", NULL), "the part cannot be meshed for assessment: %s",
                    json_get_str(first, "message", ""));
            op_fail_detail(out, "errors", json_clone(c->errors));
            goto fail;
        }
    }
    int mb = -1;
    for (int k = 0; k < c->full.nbodies; k++)
        if (!strcmp(c->full.body_name[k], c->part_name)) mb = k;
    if (mb < 0 || !extract_body(&c->full, mb, &c->sub)) {
        op_fail(out, NV_ERR_PRECONDITION, "include the part in mesh_generate", "part '%s' is not in the current mesh", c->part_name);
        goto fail;
    }
    StaticModel *sub = &c->sub;
    c->density = malloc((size_t)(sub->nelems ? sub->nelems : 1) * sizeof(double));
    if (!c->density) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        goto fail;
    }
    for (int k = 0; k < sub->nelems; k++) c->density[k] = c->use_ortho && c->ortho.mat.density > 0 ? c->ortho.mat.density : sub->mat[sub->elem_mat[k]].density;
    for (int k = 0; k < sub->nelems; k++)
        if (!(c->density[k] > 0)) {
            op_fail(out, NV_ERR_PRECONDITION,
                    c->use_ortho ? "give density in the mechanics material record or for the part's project material" : "define density_kg_m3 for the part's material (material_define)",
                    "inertial loads need the density of material '%s'", c->use_ortho ? c->ortho.mat.id : sub->mat_id[sub->elem_mat[k]]);
            goto fail;
        }
    c->mv = (FemMeshView){sub->nnodes, sub->nelems, sub->xyz, sub->conn, c->density};
    /* the voxel mesh approximates the placed part and can sit a fraction of an element away from it; the rigid body is aligned
     * with the mesh by the mass centre so every load (joints, contact, body force) is applied at the matching point of the
     * discretised part and the load set stays self-equilibrated. The offset is reported. */
    {
        InertialLoad il0;
        double zero3[3] = {0, 0, 0}, comb0[3], lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        double *scratch = calloc(3 * (size_t)(sub->nnodes ? sub->nnodes : 1), sizeof(double));
        if (!scratch) {
            op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
            goto fail;
        }
        loads_inertial(&c->mv, NULL, 0, zero3, zero3, zero3, scratch, &il0);
        free(scratch);
        mm3_mulv(comb0, c->Rbf, m->bodies[c->bi].com);
        mv3_addto(comb0, c->pbf);
        mv3_sub(c->align, il0.com, comb0);
        mv3_addto(c->pbf, c->align);
        for (int k = 0; k < sub->nnodes; k++)
            for (int c3 = 0; c3 < 3; c3++) lo[c3] = fmin(lo[c3], sub->xyz[3 * (size_t)k + (size_t)c3]), hi[c3] = fmax(hi[c3], sub->xyz[3 * (size_t)k + (size_t)c3]);
        double dd[3];
        mv3_sub(dd, hi, lo);
        c->diag = mv3_norm(dd);
        if (mv3_norm(c->align) > 0.01 * c->diag)
            json_push(c->warnings, json_stringf("the voxel mesh of '%s' is offset from the placed part by %.3g mm (%.1f%% of its size): the loads were applied at the "
                                                "matching points of the mesh; a finer mesh or a part aligned with the element grid matches the geometry more closely",
                                                c->part_name, mv3_norm(c->align) * 1e3, 100 * mv3_norm(c->align) / c->diag));
    }
    /* load sources: every joint adjacent to the body and every contact shape of the body */
    for (int j = 0; j < m->njoints; j++) {
        int side = m->joints[j].child == c->bi ? 1 : (m->joints[j].parent == c->bi ? -1 : 0);
        if (!side || c->natt == ASSESS_MAX_ATTACH) continue;
        AssessAttach *A = &c->att[c->natt++];
        A->contact = false, A->index = j, A->side = side, A->sel = -1;
        snprintf(A->name, sizeof A->name, "%s", m->joints[j].name);
    }
    for (int k = 0; k < c->ncshapes; k++) {
        if (c->cshapes[k].body != c->bi || c->natt == ASSESS_MAX_ATTACH) continue;
        AssessAttach *A = &c->att[c->natt++];
        A->contact = true, A->index = k, A->sel = -1;
        snprintf(A->name, sizeof A->name, "%s", c->cshapes[k].name);
    }
    /* every attachment names one joint or one contact shape that loads this body, and a selection on its mesh */
    const JsonValue *att = json_get(p, "attachments");
    for (size_t i = 0; i < json_len(att); i++) {
        const JsonValue *a = json_at(att, i);
        const char *jn = json_get_str(a, "joint", NULL), *cn = json_get_str(a, "contact_shape", NULL), *sel = json_get_str(a, "selection", "");
        if ((jn != NULL) == (cn != NULL)) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "use {joint, selection} or {contact_shape, selection}", "attachment %zu must name exactly one joint or one contact shape", i);
            goto fail;
        }
        AssessAttach *A = NULL;
        for (int k = 0; k < c->natt && !A; k++)
            if (c->att[k].contact == (cn != NULL) && !strcmp(c->att[k].name, jn ? jn : cn)) A = &c->att[k];
        if (!A) {
            op_fail(out, NV_ERR_NOT_FOUND, "attachments name the joints connected to the body and the body's own contact shapes", "%s '%s' does not load body '%s'",
                    jn ? "joint" : "contact shape", jn ? jn : cn, c->bname);
            goto fail;
        }
        snprintf(A->selection, sizeof A->selection, "%s", sel);
        A->sel = -1;
        for (int k = 0; k < sub->nsets; k++)
            if (!strcmp(sub->set[k].name, sel)) A->sel = k;
        if (A->sel < 0) {
            op_fail(out, NV_ERR_NOT_FOUND, "create the selection on the part with selection_create before mesh_generate or regenerate the mesh",
                    "selection '%s' has no mesh faces on part '%s'", sel, c->part_name);
            goto fail;
        }
    }
    return c;
fail:
    assess_close(c);
    return NULL;
}

/* loads on the part at the runtime's current state: the evaluated joint wrenches, the contact forces of the given rows (impulse
 * over step h) and the body motion in FEM axes; NULL when the state cannot be evaluated */
static const MbEval *assess_state_loads(AssessCtx *c, const JsonValue *rows, double h, AssessLoad *L, double s0[3], double omega[3], double alpha[3]) {
    const MbModel *m = c->m;
    for (int b = 0; b < m->nbodies; b++) mb_body_pose(mechsim_mb(c->ms), mechsim_state(c->ms), b, &c->pose[b]); /* poses first: they recompute kinematics */
    const MbEval *ev = mechsim_evaluate(c->ms);
    if (!ev) return NULL;
    const MPose *Tb = &c->pose[c->bi];
    double RWbT[9], Rfw[9], y0[3], neg[3], sf_w[3], w_w[3], al_w[3];
    mm3_transpose(RWbT, Tb->R);
    mm3_mul(Rfw, c->Rbf, RWbT);
    mv3_scale(neg, c->pbf, -1);
    mm3_tmulv(y0, c->Rbf, neg); /* body point at the FEM origin */
    mb_point_motion(mechsim_mb(c->ms), c->bi, y0, sf_w, w_w, al_w, NULL);
    mm3_mulv(s0, Rfw, sf_w), mm3_mulv(omega, Rfw, w_w), mm3_mulv(alpha, Rfw, al_w);
    for (int a = 0; a < c->natt; a++) {
        const AssessAttach *A = &c->att[a];
        AssessLoad *l = &L[a];
        memset(l, 0, sizeof *l);
        double Fw[3], Mw[3], Pw[3];
        if (!A->contact) {
            const MbJointDef *J = &m->joints[A->index];
            int j = A->index;
            MPose TJ;
            mpose_mul(&TJ, &c->pose[J->child], &J->child_frame);
            double wW[6], pxf[3];
            msv_force_to_parent(wW, &TJ, ev->joint_wrench + 6 * j); /* on the child, world axes, about the world origin */
            mv3_copy(Pw, TJ.p);
            mv3_scale(Fw, wW + 3, A->side);
            mv3_cross(pxf, Pw, wW + 3);
            for (int k = 0; k < 3; k++) Mw[k] = A->side * (wW[k] - pxf[k]); /* moment about the joint origin */
            if (A->side < 0 && J->armature > 0 && !m->joint_loop[j]) { /* the rotor's inertial torque reacts on the housing */
                double ax[3], axw[3];
                mm3_mulv(ax, J->child_frame.R, J->axis);
                mm3_mulv(axw, c->pose[J->child].R, ax);
                mv3_addscaled(Mw, axw, -J->armature * ev->qacc[m->joint_vadr[j]]);
            }
            l->active = true;
        } else {
            const ContactShape *S = &c->cshapes[A->index];
            double cp[3] = {0, 0, 0}, M0[3] = {0, 0, 0};
            mv3_set(Fw, 0, 0, 0);
            for (size_t r = 0; r < json_len(rows) && h > 0; r++) {
                const JsonValue *cr = json_at(rows, r);
                bool on_a = !strcmp(json_get_str(cr, "shape_a", ""), S->name), on_b = !strcmp(json_get_str(cr, "shape_b", ""), S->name);
                if (!on_a && !on_b) continue;
                double nrm[3], pt[3], ptg[3], pn = json_get_num(cr, "impulse_normal", 0);
                json_get_numbers(json_get(cr, "normal"), nrm, 3);
                json_get_numbers(json_get(cr, "impulse_tangent"), ptg, 3);
                json_get_numbers(json_get(cr, on_a ? "point_a" : "point_b"), pt, 3);
                if (pn <= 0 && mv3_norm(ptg) <= 0) continue;
                double sgn = on_a ? 1 : -1, f[3], m3[3];
                for (int k = 0; k < 3; k++) f[k] = sgn * (pn * nrm[k] + ptg[k]) / h; /* the impulse on A is +p (normal from B to A) */
                mv3_cross(m3, pt, f);
                mv3_addto(Fw, f), mv3_addto(M0, m3), mv3_addto(cp, pt);
                l->points++;
            }
            if (!l->points) continue;
            mv3_scale(Pw, cp, 1.0 / l->points);
            double pxf[3];
            mv3_cross(pxf, Pw, Fw);
            mv3_sub(Mw, M0, pxf); /* moment about the mean contact point */
            l->active = true;
        }
        double d3[3], tmpb[3];
        mm3_mulv(l->F, Rfw, Fw), mm3_mulv(l->M, Rfw, Mw);
        mv3_sub(d3, Pw, Tb->p);
        mm3_tmulv(tmpb, Tb->R, d3);
        mm3_mulv(l->P, c->Rbf, tmpb), mv3_addto(l->P, c->pbf);
    }
    return ev;
}

/* faces of a mesh set as element/local-face arrays (caller frees both) */
static bool set_faces(const StaticModel *sub, int sj, FaceSet *fs, int **fe, unsigned char **fl) {
    *fe = malloc((size_t)(sub->set[sj].nfaces ? sub->set[sj].nfaces : 1) * sizeof(int));
    *fl = malloc((size_t)(sub->set[sj].nfaces ? sub->set[sj].nfaces : 1));
    if (!*fe || !*fl) {
        free(*fe), free(*fl);
        *fe = NULL, *fl = NULL;
        return false;
    }
    for (int k = 0; k < sub->set[sj].nfaces; k++) (*fe)[k] = sub->face_elem[sub->set[sj].faces[k]], (*fl)[k] = sub->face_local[sub->set[sj].faces[k]];
    *fs = (FaceSet){sub->set[sj].nfaces, *fe, *fl};
    return true;
}

static void op_mech_fem_assess(Engine *e, JsonValue *p, OpResult *out) {
    AssessCtx *c = assess_open(e, p, out);
    if (!c) return;
    Project *proj = e->proj;
    const char *label = json_get_str(p, "snapshot", "");
    char path[NV_PATH_MAX];
    JsonError je;
    path_join(path, sizeof path, c->run_dir, "snapshots.json");
    JsonValue *snaps = json_read_file(path, 256 << 20, &je), *report = json_object();
    unsigned char *blob = NULL;
    const JsonValue *snap = NULL;
    for (size_t i = 0; i < json_len(snaps); i++)
        if (!strcmp(json_get_str(json_at(snaps, i), "label", ""), label)) snap = json_at(snaps, i);
    if (!snaps) {
        op_fail(out, NV_ERR_PRECONDITION, "wait for the dynamics job to succeed", "job '%s' has no stored snapshots", c->job_id);
        goto done;
    }
    if (!snap) {
        op_fail(out, NV_ERR_NOT_FOUND, "mech_results_query with what: snapshots lists the labels", "snapshot '%s' not found", label);
        goto done;
    }
    size_t blen = 0;
    const char *b64 = json_get_str(snap, "state_base64", "");
    blob = base64_decode(b64, strlen(b64), &blen);
    if (!blob || !mechsim_restore_snapshot(c->ms, blob, blen)) {
        op_fail(out, NV_ERR_PRECONDITION, "rerun the dynamics study with this build", "the stored snapshot '%s' does not match the specification", label);
        goto done;
    }
    const JsonValue *crow = json_get(snap, "contact_rows");
    double ch = json_get_num(snap, "contact_step_s", 0);
    if (json_get_bool(snap, "contact_impulsive", false)) {
        op_fail(out, NV_ERR_UNSUPPORTED,
                "assess a snapshot of persistent contact: peak_contact and peak joint snapshots exclude impact steps; impact stress needs contact duration and compliance, which the rigid model does not have",
                "snapshot '%s' ends a step with an impact: a rigid impact transfers an impulse whose average force depends on the step, so no structural load follows from it",
                label);
        goto done;
    }
    if (json_len(crow) && !(ch > 0)) {
        op_fail(out, NV_ERR_PRECONDITION, "rerun the dynamics study with this build", "snapshot '%s' has contact rows without their step", label);
        goto done;
    }
    AssessLoad L[ASSESS_MAX_ATTACH];
    double s0[3], omega[3], alpha[3];
    if (!assess_state_loads(c, crow, ch, L, s0, omega, alpha)) {
        op_fail(out, NV_ERR_SOLVER_FAILED, NULL, "cannot evaluate the snapshot state");
        goto done;
    }
    StaticModel *sub = &c->sub;
    json_set_string(report, "source_job", c->job_id);
    json_set_string(report, "snapshot", label);
    json_set_number(report, "time_s", mechsim_time(c->ms));
    json_set_string(report, "body", c->bname);
    json_set_string(report, "part", c->part_name);
    json_set_bool(report, "source_contact", c->study->st.contact);
    json_set_int(report, "source_flexible_bodies", c->m->nflex);
    JsonValue *kin = json_set_object(report, "body_motion_fem_axes");
    json_set(kin, "angular_velocity_rad_s", json_vec3(omega[0], omega[1], omega[2]));
    json_set(kin, "angular_acceleration_rad_s2", json_vec3(alpha[0], alpha[1], alpha[2]));
    json_set(kin, "specific_force_at_origin_m_s2", json_vec3(s0[0], s0[1], s0[2]));
    /* transfer of every active load over its selection */
    JsonValue *transfers = json_set_array(report, "joint_loads");
    char missing[512] = "";
    size_t mused = 0;
    double load_scale = 0;
    for (int a = 0; a < c->natt; a++) {
        const AssessAttach *A = &c->att[a];
        const AssessLoad *l = &L[a];
        if (!l->active) continue;
        load_scale = fmax(load_scale, mv3_norm(l->F));
        JsonValue *tr = json_object();
        if (!A->contact) {
            json_set_string(tr, "joint", A->name);
            json_set_string(tr, "side", A->side > 0 ? "child (load from the parent)" : "parent (reaction of the child)");
        } else {
            json_set_string(tr, "contact_shape", A->name);
            json_set_int(tr, "points", l->points);
        }
        json_set(tr, "force_N", json_vec3(l->F[0], l->F[1], l->F[2]));
        json_set(tr, "moment_about_point_Nm", json_vec3(l->M[0], l->M[1], l->M[2]));
        json_set(tr, "point_mm", json_vec3(l->P[0] * 1e3, l->P[1] * 1e3, l->P[2] * 1e3));
        if (A->contact) json_set_string(tr, "force_basis", "average force over the contact step (impulse / step) of persistent contact");
        if (A->sel < 0) {
            if (mused < sizeof missing - 70)
                mused += (size_t)snprintf(missing + mused, sizeof missing - mused, "%s%s%s", mused ? ", " : "", A->contact ? "contact " : "", A->name);
            json_push(transfers, tr);
            continue;
        }
        json_set_string(tr, "selection", A->selection);
        FaceSet fs;
        int *fe = NULL;
        unsigned char *fl = NULL;
        WrenchTransfer wt;
        char err[400];
        bool dist = set_faces(sub, A->sel, &fs, &fe, &fl) && loads_distribute_wrench(&c->mv, &fs, l->F, l->M, l->P, sub->nodal_force, &wt, err, sizeof err);
        free(fe), free(fl);
        if (!dist) {
            json_free(tr);
            op_fail(out, NV_ERR_INVALID_PARAMS, A->contact ? NULL : "choose a selection with extent in every direction the joint transmits moment", "%s '%s' on selection '%s': %s",
                    A->contact ? "contact" : "joint", A->name, A->selection, fe ? err : "out of memory");
            goto done;
        }
        json_set_number(tr, "region_area_mm2", wt.area * 1e6);
        json_set(tr, "region_centroid_mm", json_vec3(wt.centroid[0] * 1e3, wt.centroid[1] * 1e3, wt.centroid[2] * 1e3));
        json_set_number(tr, "max_traction_Pa", wt.max_traction);
        json_set_number(tr, "force_reproduction_error", wt.force_error);
        json_set_number(tr, "moment_reproduction_error", wt.moment_error);
        if (A->contact) json_set_string(tr, "distribution", "resultant-preserving traction over the selection (linear in position); not a contact pressure (Hertzian) distribution");
        json_push(transfers, tr);
    }
    if (mused) {
        op_fail(out, NV_ERR_PRECONDITION, "add {joint, selection} or {contact_shape, selection} for each listed load: the selection is where it enters the part",
                "%s load body '%s' but have no attachment selection", missing, c->bname);
        goto done;
    }
    /* inertial body force and balance */
    InertialLoad il;
    loads_inertial(&c->mv, NULL, 0, s0, omega, alpha, sub->nodal_force, &il);
    double Rall[3], Mall[3];
    loads_resultant(&c->mv, sub->nodal_force, il.com, Rall, Mall);
    load_scale = fmax(load_scale, mv3_norm(il.resultant));
    JsonValue *bal = json_set_object(report, "balance");
    json_set(bal, "inertial_resultant_N", json_vec3(il.resultant[0], il.resultant[1], il.resultant[2]));
    json_set(bal, "net_force_N", json_vec3(Rall[0], Rall[1], Rall[2]));
    json_set(bal, "net_moment_about_fem_mass_centre_Nm", json_vec3(Mall[0], Mall[1], Mall[2]));
    json_set_number(bal, "load_scale_N", load_scale);
    json_set_number(bal, "net_force_relative", load_scale > 0 ? mv3_norm(Rall) / load_scale : 0);
    const MbBodyDef *RB = &c->m->bodies[c->bi];
    double comb[3], com_fem[3], dc[3];
    mm3_mulv(comb, c->Rbf, RB->com);
    mv3_add(com_fem, comb, c->pbf);
    mv3_sub(dc, il.com, com_fem);
    JsonValue *mc = json_set_object(report, "mass_consistency");
    json_set_number(mc, "rigid_model_mass_kg", RB->mass);
    json_set_number(mc, "fem_mass_kg", il.mass);
    json_set_number(mc, "relative_difference", RB->mass > 0 ? (il.mass - RB->mass) / RB->mass : 0);
    json_set_number(mc, "centre_of_mass_offset_mm", mv3_norm(dc) * 1e3);
    json_set(mc, "mesh_alignment_offset_mm", json_vec3(c->align[0] * 1e3, c->align[1] * 1e3, c->align[2] * 1e3));
    json_set_string(mc, "mesh_alignment", "the rigid body is aligned with the FEM mesh by the mass centre before the loads are applied");
    if (RB->mass > 0 && fabs(il.mass - RB->mass) > 0.05 * RB->mass)
        json_push(c->warnings, json_stringf("the FEM mass (%.4g kg, material density over the voxel mesh) differs from the rigid-body mass (%.4g kg) by %.0f%%: the "
                                            "joint loads and the body force do not balance, and the difference ends up in the support reactions",
                                            il.mass, RB->mass, 100 * (il.mass - RB->mass) / RB->mass));
    /* isostatic support */
    IsostaticSupport sup;
    if (!loads_isostatic_choose(&c->mv, NULL, 0, &sup)) {
        op_fail(out, NV_ERR_MESH_INVALID, NULL, "cannot choose three non-collinear support nodes on part '%s'", c->part_name);
        goto done;
    }
    loads_isostatic_fixed(&sup, sub->fixed);
    JsonValue *sj = json_set_object(report, "support");
    json_set_string(sj, "kind", "isostatic 3-2-1 support: removes rigid-body motion only; displacements are relative to these nodes");
    JsonValue *sn = json_set_array(sj, "nodes");
    for (int k = 0; k < 3; k++) {
        JsonValue *o = json_object();
        json_set_int(o, "node", sup.node[k]);
        const double *x = sub->xyz + 3 * (size_t)sup.node[k];
        json_set(o, "position_mm", json_vec3(x[0] * 1e3, x[1] * 1e3, x[2] * 1e3));
        json_set_string(o, "constrained", k == 0 ? "x y z" : (k == 1 ? "two directions normal to the line to the first node" : "one direction normal to the plane"));
        json_push(sn, o);
    }
    memset(sub->gravity, 0, sizeof sub->gravity);
    json_set(report, "warnings", json_clone(c->warnings));
    /* run directory and job */
    {
        char id[64], dir[NV_PATH_MAX], err[512], hash[65];
        JsonValue *aspec = json_object();
        json_set_string(aspec, "format", "navier-mech-fem-assessment");
        json_set_int(aspec, "version", 1);
        json_set_string(aspec, "source_spec_hash", json_get_str(c->spec, "spec_hash", ""));
        json_set_string(aspec, "snapshot", label);
        json_set_string(aspec, "body", c->bname);
        json_set_string(aspec, "part", c->part_name);
        json_set(aspec, "attachments", json_clone(json_get(p, "attachments")));
        json_set_string(aspec, "mesh_hash", sub->mesh_hash);
        if (c->use_ortho) json_set(aspec, "material_model", json_clone(c->ortho.description));
        hash_json(aspec, hash);
        char active[64];
        if (jobs_find_active(e->jobs, hash, active, sizeof active)) {
            JsonValue *v = json_object();
            json_set_string(v, "job_id", active);
            json_set_bool(v, "deduplicated", true);
            json_free(aspec);
            op_succeed(out, v);
            goto done;
        }
        if (!make_run_dir(e, out, id, sizeof id, dir, sizeof dir)) {
            json_free(aspec);
            goto done;
        }
        json_set_string(aspec, "job_id", id);
        json_set_string(aspec, "spec_hash", hash);
        path_join(path, sizeof path, dir, "spec.json");
        json_write_file(path, aspec, JSON_PRETTY | JSON_SORTED);
        json_free(aspec);
        path_join(path, sizeof path, dir, "mech_assessment.json");
        json_write_file(path, report, JSON_PRETTY);
        if (c->use_ortho) { /* mechanics-owned orthotropic solve */
            MechStructJob *J = calloc(1, sizeof *J);
            if (!J) {
                op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
                goto done;
            }
            J->model = *sub;
            memset(sub, 0, sizeof *sub);
            J->setup = c->ortho;
            memset(&c->ortho, 0, sizeof c->ortho);
            J->report = json_clone(report);
            json_set(J->report, "material_model", json_clone(J->setup.description));
            snprintf(J->job_id, sizeof J->job_id, "%s", id);
            snprintf(J->run_dir, sizeof J->run_dir, "%s", dir);
            JsonValue *fid = fidelity_json(3, c->study->st.contact, c->m->nflex);
            JsonValue *as = json_get(fid, "assumptions");
            for (size_t i = json_len(as); i-- > 0;)
                if (!strncmp(json_str(json_at(as, i)) ? json_str(json_at(as, i)) : "", "isotropic linear elastic", 24)) {
                    json_free(as->u.array.items[i]);
                    memmove(as->u.array.items + i, as->u.array.items + i + 1, (as->u.array.len - i - 1) * sizeof *as->u.array.items);
                    as->u.array.len--;
                }
            mech_struct_assumptions(as, &J->setup);
            JsonValue *desc = json_clone(J->setup.description);
            JobSpec js = {id, "mech_structural", json_get_str(p, "label", "orthotropic load assessment"), dir, hash, proj->id, proj->name, proj->revision, J,
                          mech_struct_job_run, mech_struct_job_free};
            if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
                json_free(fid), json_free(desc);
                op_fail(out, NV_ERR_BUSY, NULL, "%s", err);
                goto done;
            }
            JsonValue *v = json_object();
            json_set_string(v, "job_id", id);
            json_set_string(v, "state", "queued");
            json_set_string(v, "analysis", "mech_structural (orthotropic printed part, mechanical load snapshot)");
            json_set_string(v, "run_directory", dir);
            json_set(v, "fidelity", fid);
            json_set(v, "material_model", desc);
            json_set(v, "assessment", json_clone(report));
            json_set_string(v, "next_step",
                            "poll job_status; then mech_structure_query with quantity summary, displacement, stress, material_stress, strain or failure_index "
                            "(results_query serves isotropic analyses only)");
            op_succeed(out, v);
            goto done;
        }
        StaticResults *r = calloc(1, sizeof *r);
        if (!r) {
            op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
            goto done;
        }
        r->model = *sub;
        memset(sub, 0, sizeof *sub); /* owned by the job now */
        snprintf(r->job_id, sizeof r->job_id, "%s", id);
        snprintf(r->run_dir, sizeof r->run_dir, "%s", dir);
        r->build_warnings = json_clone(c->warnings);
        JobSpec js = {id, "static_structural", json_get_str(p, "label", "mechanical load assessment"), dir, hash, proj->id, proj->name, proj->revision, r, mech_fem_job_run,
                      static_results_free};
        if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
            op_fail(out, NV_ERR_BUSY, NULL, "%s", err);
            goto done;
        }
        JsonValue *v = json_object();
        json_set_string(v, "job_id", id);
        json_set_string(v, "state", "queued");
        json_set_string(v, "analysis", "static_structural (mechanical load snapshot)");
        json_set_string(v, "run_directory", dir);
        json_set(v, "fidelity", fidelity_json(3, c->study->st.contact, c->m->nflex));
        json_set(v, "assessment", json_clone(report));
        json_set_string(v, "next_step", "poll job_status; the summary adds mechanical_assessment with the support reactions; query stresses with results_query");
        op_succeed(out, v);
    }
done:
    free(blob);
    json_free(snaps), json_free(report);
    assess_close(c);
}

/* ------------------------------------------------------------------------------------------------ vibration of parts */

/* mesh of one project part with its density (orthotropic record density when given); fails the operation */
static bool part_mesh(Project *proj, const char *part, const MechStructSetup *ortho, StaticModel *full, StaticModel *sub, double **density, JsonValue *errors,
                      JsonValue *warnings, OpResult *out) {
    StaticSettings sset = {HEX8_INCOMPATIBLE, SOLID_SOLVER_AUTO, 1e-10, 293.15};
    if (!static_model_build(proj, &sset, full, errors, warnings)) {
        bool only_supports = json_len(errors) > 0;
        for (size_t i = 0; i < json_len(errors); i++) only_supports &= !strcmp(json_get_str(json_at(errors, i), "code", ""), "INSUFFICIENT_CONSTRAINTS");
        if (!only_supports) {
            const JsonValue *first = json_at(errors, 0);
            op_fail(out, nv_err_from_name(json_get_str(first, "code", "PRECONDITION_FAILED")), json_get_str(first, "hint", NULL), "the part cannot be meshed: %s",
                    json_get_str(first, "message", ""));
            op_fail_detail(out, "errors", json_clone(errors));
            return false;
        }
    }
    int mb = -1;
    for (int k = 0; k < full->nbodies; k++)
        if (!strcmp(full->body_name[k], part)) mb = k;
    if (mb < 0 || !extract_body(full, mb, sub)) {
        op_fail(out, NV_ERR_PRECONDITION, "include the part in mesh_generate", "part '%s' is not in the current mesh", part);
        return false;
    }
    *density = malloc((size_t)(sub->nelems ? sub->nelems : 1) * sizeof(double));
    if (!*density) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return false;
    }
    for (int k = 0; k < sub->nelems; k++) {
        (*density)[k] = ortho && ortho->mat.density > 0 ? ortho->mat.density : sub->mat[sub->elem_mat[k]].density;
        if (!((*density)[k] > 0)) {
            op_fail(out, NV_ERR_PRECONDITION, "give the density in the material record or for the part's project material", "the part's material has no density");
            return false;
        }
    }
    return true;
}

static void op_mech_modal_run(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    const char *part = json_get_str(p, "part", "");
    Body *pb = project_body(proj, part);
    if (!pb || strcmp(pb->name, part)) {
        op_fail(out, NV_ERR_NOT_FOUND, "the part is a project body with a current mesh", "no project body '%s'", part);
        return;
    }
    const JsonValue *mmj = json_get(p, "material_model");
    const char *mmtype = json_get_str(mmj, "type", "project_material");
    bool use_ortho = !strcmp(mmtype, "orthotropic");
    if (!use_ortho && strcmp(mmtype, "project_material")) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "material_model.type is project_material or orthotropic", "unknown material model '%s'", mmtype);
        return;
    }
    MechStructSetup ortho;
    memset(&ortho, 0, sizeof ortho);
    if (use_ortho && !mech_struct_setup(proj, mmj, &ortho, out)) return;
    StaticModel full, sub;
    memset(&full, 0, sizeof full), memset(&sub, 0, sizeof sub);
    JsonValue *errors = json_array(), *warnings = json_array(), *setup = NULL, *aspec = NULL;
    double *density = NULL;
    unsigned char *fixed = NULL;
    VibMaterial vm;
    memset(&vm, 0, sizeof vm);
    if (!part_mesh(proj, part, use_ortho ? &ortho : NULL, &full, &sub, &density, errors, warnings, out)) goto done;
    const JsonValue *sj = json_get(p, "supports");
    const char *stype = json_get_str(sj, "type", "free");
    if (!strcmp(stype, "fixed")) {
        const JsonValue *sels = json_get(sj, "selections");
        if (!json_len(sels)) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "supports {type: fixed, selections: [...]} clamps every node of the named selections", "fixed supports need selections");
            goto done;
        }
        fixed = calloc(3 * (size_t)sub.nnodes, 1);
        if (!fixed) {
            op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
            goto done;
        }
        for (size_t i = 0; i < json_len(sels); i++) {
            const char *nm = json_str(json_at(sels, i));
            int k = -1;
            for (int s = 0; nm && s < sub.nsets; s++)
                if (!strcmp(sub.set[s].name, nm)) k = s;
            if (k < 0) {
                op_fail(out, NV_ERR_NOT_FOUND, "create the selection on the part before mesh_generate", "selection '%s' has no mesh faces on part '%s'", nm ? nm : "?", part);
                goto done;
            }
            for (int n = 0; n < sub.set[k].nnodes; n++)
                for (int c3 = 0; c3 < 3; c3++) fixed[3 * (size_t)sub.set[k].nodes[n] + (size_t)c3] = 1;
        }
    } else if (strcmp(stype, "free")) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "supports.type is free or fixed", "unknown supports type '%s'", stype);
        goto done;
    }
    char err[512];
    if (!vib_material_from(&sub, use_ortho ? &ortho : NULL, &vm, err, sizeof err)) {
        op_fail(out, NV_ERR_PRECONDITION, "assign a material with elastic constants to the part (material_assign) or use an orthotropic record", "%s", err);
        goto done;
    }
    int nel = (int)json_get_int(p, "modes", 10);
    {
        char id[64], dir[NV_PATH_MAX], path[NV_PATH_MAX], hash[65];
        aspec = json_object();
        json_set_string(aspec, "format", "navier-mech-modal");
        json_set_int(aspec, "version", 1);
        json_set_string(aspec, "part", part);
        json_set(aspec, "supports", sj ? json_clone(sj) : json_string("free"));
        json_set_int(aspec, "elastic_modes", nel);
        json_set(aspec, "material_model", vm.ortho ? json_clone(vm.setup.description) : json_string("project_material"));
        json_set_string(aspec, "mesh_hash", sub.mesh_hash);
        hash_json(aspec, hash);
        char active[64];
        if (jobs_find_active(e->jobs, hash, active, sizeof active)) {
            JsonValue *v = json_object();
            json_set_string(v, "job_id", active);
            json_set_bool(v, "deduplicated", true);
            op_succeed(out, v);
            goto done;
        }
        if (!make_run_dir(e, out, id, sizeof id, dir, sizeof dir)) goto done;
        json_set_string(aspec, "job_id", id);
        json_set_string(aspec, "spec_hash", hash);
        path_join(path, sizeof path, dir, "spec.json");
        json_write_file(path, aspec, JSON_PRETTY | JSON_SORTED);
        MechModalJob *J = calloc(1, sizeof *J);
        if (!J) {
            op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
            goto done;
        }
        J->model = sub, J->mat = vm, J->density = density, J->fixed = fixed, J->elastic_modes = nel, J->setup = json_clone(aspec);
        memset(&sub, 0, sizeof sub), memset(&vm, 0, sizeof vm);
        density = NULL, fixed = NULL;
        snprintf(J->job_id, sizeof J->job_id, "%s", id);
        snprintf(J->run_dir, sizeof J->run_dir, "%s", dir);
        JobSpec js = {id, "mech_modal", json_get_str(p, "label", "natural modes"), dir, hash, proj->id, proj->name, proj->revision, J, mech_modal_job_run, mech_modal_job_free};
        if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
            op_fail(out, NV_ERR_BUSY, NULL, "%s", err);
            goto done;
        }
        JsonValue *v = json_object();
        json_set_string(v, "job_id", id);
        json_set_string(v, "state", "queued");
        json_set_string(v, "analysis", "mech_modal (natural modes of a part)");
        json_set_string(v, "run_directory", dir);
        json_set(v, "warnings", json_clone(warnings));
        json_set_string(v, "next_step", "poll job_status; then mech_vibration_query with what summary or mode_shape");
        op_succeed(out, v);
    }
done:
    mech_struct_setup_free(&ortho);
    vib_material_free(&vm);
    static_model_free(&full), static_model_free(&sub);
    free(density), free(fixed);
    json_free(errors), json_free(warnings), json_free(setup), json_free(aspec);
}

static void op_mech_transient_assess(Engine *e, JsonValue *p, OpResult *out) {
    AssessCtx *c = assess_open(e, p, out);
    if (!c) return;
    Project *proj = e->proj;
    double *patterns = NULL, *hist = NULL, *scratch = NULL, *fdir = NULL;
    JsonValue *report = json_object(), *aspec = NULL;
    VibMaterial vm;
    memset(&vm, 0, sizeof vm);
    char qerr[200], err[512];
    double t0 = 0, t1 = 0, dt = 0;
    if (!mech_qty_from_json_ex(json_get(p, "time_from"), MQ_TIME, NULL, true, &t0, qerr, sizeof qerr) ||
        !mech_qty_from_json_ex(json_get(p, "time_to"), MQ_TIME, NULL, true, &t1, qerr, sizeof qerr)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "for example \"0.2 s\"", "time window: %s", qerr);
        goto done;
    }
    double Tend = c->study->st.end_time;
    if (!(t0 >= 0 && t1 > t0 && t1 <= Tend * (1 + 1e-12))) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "the window must satisfy 0 <= time_from < time_to <= %.9g s (the study end)", Tend);
        goto done;
    }
    if (json_get(p, "output_step")) {
        if (!mech_qty_from_json_ex(json_get(p, "output_step"), MQ_TIME, NULL, true, &dt, qerr, sizeof qerr) || !(dt > 0)) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "for example \"1 ms\"", "output_step: %s", dt > 0 ? qerr : "must be positive");
            goto done;
        }
    } else
        dt = c->study->st.record_period > 0 ? c->study->st.record_period : (t1 - t0) / 200;
    int ns = (int)ceil((t1 - t0) / dt - 1e-9);
    if (ns < 2) ns = 2;
    if (ns > 20000) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "shorten the window or increase output_step", "%d load samples exceed the limit of 20000", ns);
        goto done;
    }
    dt = (t1 - t0) / ns;
    const JsonValue *dj = json_get(p, "damping");
    double zeta = json_get_num(dj, "ratio", NAN);
    const char *dsrc = json_get_str(dj, "source", NULL);
    if (!dj || !isfinite(zeta) || !dsrc) {
        op_fail(out, NV_ERR_PRECONDITION, "give damping {ratio, source}: printed parts have uncertain damping, so no default is assumed (for example {\"ratio\": 0.02, \"source\": \"assumed\"})",
                "the modal damping ratio and its source are missing");
        goto done;
    }
    if (!(zeta >= 0 && zeta < 1)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "damping ratio must be in [0, 1)");
        goto done;
    }
    int nel = (int)json_get_int(p, "modes", 20);
    StaticModel *sub = &c->sub;
    size_t NN3 = 3 * (size_t)sub->nnodes, S = (size_t)ns + 1;
    /* load patterns: 6 per attachment (unit force and moment about the selection centroid), 12 inertial */
    int nsel = 0;
    for (int a = 0; a < c->natt; a++) nsel += c->att[a].sel >= 0;
    size_t P = 6 * (size_t)nsel + 12;
    double P0[ASSESS_MAX_ATTACH][3];
    patterns = calloc(P * NN3, sizeof(double));
    hist = calloc(P * S, sizeof(double));
    scratch = calloc(NN3, sizeof(double));
    fdir = calloc(NN3, sizeof(double));
    if (!patterns || !hist || !scratch || !fdir) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory for %zu load patterns", P);
        goto done;
    }
    {
        size_t k = 0;
        for (int a = 0; a < c->natt; a++) {
            const AssessAttach *A = &c->att[a];
            if (A->sel < 0) continue;
            FaceSet fs;
            int *fe = NULL;
            unsigned char *fl = NULL;
            WrenchTransfer wt;
            double zero3[3] = {0, 0, 0}, ex[3] = {1, 0, 0};
            bool okd = set_faces(sub, A->sel, &fs, &fe, &fl) && loads_distribute_wrench(&c->mv, &fs, ex, zero3, zero3, scratch, &wt, err, sizeof err);
            if (okd) mv3_copy(P0[a], wt.centroid);
            for (int u = 0; okd && u < 6; u++) {
                double F[3] = {0, 0, 0}, M[3] = {0, 0, 0};
                if (u < 3) F[u] = 1;
                else M[u - 3] = 1;
                okd = loads_distribute_wrench(&c->mv, &fs, F, M, P0[a], patterns + k * NN3, &wt, err, sizeof err);
                k++;
            }
            free(fe), free(fl);
            if (!okd) {
                op_fail(out, NV_ERR_INVALID_PARAMS, A->contact ? NULL : "choose a selection with extent in every direction the joint transmits moment", "%s '%s' on selection '%s': %s",
                        A->contact ? "contact" : "joint", A->name, A->selection, fe ? err : "out of memory");
                goto done;
            }
        }
        for (int u = 0; u < 12; u++, k++) {
            double B[9] = {0}, cc[3] = {0, 0, 0};
            if (u < 3)
                cc[u] = 1; /* specific force */
            else if (u < 6) { /* angular acceleration: B = [e]x */
                int i = u - 3, j = (i + 1) % 3, l = (i + 2) % 3;
                B[3 * j + l] = -1, B[3 * l + j] = 1;
            } else if (u < 9) { /* w_i^2: e_i e_i^T - I */
                int i = u - 6;
                B[0] = B[4] = B[8] = -1;
                B[3 * i + i] += 1;
            } else { /* w_i w_j (i, j) = (x, y), (y, z), (z, x): e_i e_j^T + e_j e_i^T */
                int i = u - 9, j = (i + 1) % 3;
                B[3 * i + j] = B[3 * j + i] = 1;
            }
            loads_body_linear(&c->mv, B, cc, patterns + k * NN3);
        }
    }
    /* samples of the load histories along the rigid dynamics */
    char runerr[400];
    if (!mechsim_run_until(c->ms, t0, runerr, sizeof runerr)) {
        op_fail(out, NV_ERR_SOLVER_FAILED, NULL, "the dynamics study failed before the window: %s", runerr);
        goto done;
    }
    int imp0 = mechsim_impulsive_steps(c->ms);
    bool missing[ASSESS_MAX_ATTACH] = {false};
    AssessLoad L[ASSESS_MAX_ATTACH];
    double s0[3], omega[3], alpha[3], recon = 0, netrel = 0;
    for (size_t n = 0; n < S; n++) {
        if (n > 0 && !mechsim_run_until(c->ms, t0 + (double)n * dt, runerr, sizeof runerr)) {
            op_fail(out, NV_ERR_SOLVER_FAILED, NULL, "the dynamics study failed inside the window: %s", runerr);
            goto done;
        }
        double h = 0;
        bool imp = false;
        JsonValue *rows = mechsim_last_contact_json(c->ms, &h, &imp);
        const MbEval *ev = assess_state_loads(c, rows, h, L, s0, omega, alpha);
        json_free(rows);
        if (!ev) {
            op_fail(out, NV_ERR_SOLVER_FAILED, NULL, "cannot evaluate the state at t = %.9g s", t0 + (double)n * dt);
            goto done;
        }
        size_t k = 0;
        for (int a = 0; a < c->natt; a++) {
            const AssessAttach *A = &c->att[a];
            if (A->sel < 0) {
                missing[a] |= L[a].active;
                continue;
            }
            if (L[a].active) {
                double d[3], dxf[3];
                mv3_sub(d, L[a].P, P0[a]);
                mv3_cross(dxf, d, L[a].F);
                for (int u = 0; u < 3; u++) hist[(k + (size_t)u) * S + n] = L[a].F[u], hist[(k + 3 + (size_t)u) * S + n] = L[a].M[u] + dxf[u];
            }
            k += 6;
        }
        double wv[6] = {omega[0] * omega[0], omega[1] * omega[1], omega[2] * omega[2], omega[0] * omega[1], omega[1] * omega[2], omega[2] * omega[0]};
        for (int u = 0; u < 3; u++) hist[(k + (size_t)u) * S + n] = s0[u], hist[(k + 3 + (size_t)u) * S + n] = alpha[u];
        for (int u = 0; u < 6; u++) hist[(k + 6 + (size_t)u) * S + n] = wv[u];
        if (n == S - 1) { /* the patterns reproduce the direct load computation, and the loads balance the rigid motion */
            memset(fdir, 0, NN3 * sizeof(double)), memset(scratch, 0, NN3 * sizeof(double));
            for (int a = 0; a < c->natt; a++) {
                const AssessAttach *A = &c->att[a];
                if (A->sel < 0 || !L[a].active) continue;
                FaceSet fs;
                int *fe = NULL;
                unsigned char *fl = NULL;
                WrenchTransfer wt;
                if (set_faces(sub, A->sel, &fs, &fe, &fl)) loads_distribute_wrench(&c->mv, &fs, L[a].F, L[a].M, L[a].P, fdir, &wt, err, sizeof err);
                free(fe), free(fl);
            }
            InertialLoad il;
            loads_inertial(&c->mv, NULL, 0, s0, omega, alpha, fdir, &il);
            for (size_t j = 0; j < P; j++) {
                double g = hist[j * S + n];
                if (g != 0)
                    for (size_t q = 0; q < NN3; q++) scratch[q] += g * patterns[j * NN3 + q];
            }
            double dd = 0, ff = 0, scale = mv3_norm(il.resultant);
            for (size_t q = 0; q < NN3; q++) dd += (scratch[q] - fdir[q]) * (scratch[q] - fdir[q]), ff += fdir[q] * fdir[q];
            recon = ff > 0 ? sqrt(dd / ff) : sqrt(dd);
            for (int a = 0; a < c->natt; a++) scale = fmax(scale, L[a].active ? mv3_norm(L[a].F) : 0);
            double Rn[3], Mn[3];
            loads_resultant(&c->mv, fdir, il.com, Rn, Mn);
            netrel = scale > 0 ? mv3_norm(Rn) / scale : 0;
        }
    }
    if (mechsim_impulsive_steps(c->ms) > imp0) {
        op_fail(out, NV_ERR_UNSUPPORTED, "choose a window of persistent contact or free motion; an impact needs compliant contact to give forces",
                "%d impact steps fall inside the window: rigid impacts transfer impulses, not structural loads", mechsim_impulsive_steps(c->ms) - imp0);
        goto done;
    }
    {
        char miss[512] = "";
        size_t used = 0;
        for (int a = 0; a < c->natt; a++)
            if (missing[a] && used < sizeof miss - 70) used += (size_t)snprintf(miss + used, sizeof miss - used, "%s%s%s", used ? ", " : "", c->att[a].contact ? "contact " : "", c->att[a].name);
        if (used) {
            op_fail(out, NV_ERR_PRECONDITION, "add {joint, selection} or {contact_shape, selection} for each listed load", "%s load body '%s' in the window but have no attachment selection", miss,
                    c->bname);
            goto done;
        }
    }
    /* report and job */
    json_set_string(report, "source_job", c->job_id);
    json_set_string(report, "body", c->bname);
    json_set_string(report, "part", c->part_name);
    json_set_number(report, "time_from_s", t0);
    json_set_number(report, "time_to_s", t1);
    json_set_int(report, "samples", (long long)S);
    json_set_number(report, "output_step_s", dt);
    json_set_number(report, "damping_ratio", zeta);
    json_set_string(report, "damping_source", dsrc);
    json_set_int(report, "load_patterns", (long long)P);
    json_set_number(report, "pattern_reconstruction_error", recon);
    json_set_number(report, "net_force_relative_at_window_end", netrel);
    json_set(report, "attachments", json_clone(json_get(p, "attachments")));
    json_set(report, "mass_alignment_offset_mm", json_vec3(c->align[0] * 1e3, c->align[1] * 1e3, c->align[2] * 1e3));
    json_set(report, "warnings", json_clone(c->warnings));
    if (!vib_material_from(sub, c->use_ortho ? &c->ortho : NULL, &vm, err, sizeof err)) {
        op_fail(out, NV_ERR_PRECONDITION, "assign a material with elastic constants to the part or use an orthotropic record", "%s", err);
        goto done;
    }
    {
        char id[64], dir[NV_PATH_MAX], path[NV_PATH_MAX], hash[65];
        aspec = json_object();
        json_set_string(aspec, "format", "navier-mech-transient");
        json_set_int(aspec, "version", 1);
        json_set_string(aspec, "source_spec_hash", json_get_str(c->spec, "spec_hash", ""));
        json_set_string(aspec, "body", c->bname);
        json_set_string(aspec, "part", c->part_name);
        json_set(aspec, "attachments", json_clone(json_get(p, "attachments")));
        json_set_number(aspec, "time_from_s", t0), json_set_number(aspec, "time_to_s", t1), json_set_number(aspec, "output_step_s", dt);
        json_set_int(aspec, "elastic_modes", nel);
        json_set(aspec, "damping", json_clone(dj));
        json_set(aspec, "material_model", vm.ortho ? json_clone(vm.setup.description) : json_string("project_material"));
        json_set_string(aspec, "mesh_hash", sub->mesh_hash);
        hash_json(aspec, hash);
        char active[64];
        if (jobs_find_active(e->jobs, hash, active, sizeof active)) {
            JsonValue *v = json_object();
            json_set_string(v, "job_id", active);
            json_set_bool(v, "deduplicated", true);
            op_succeed(out, v);
            goto done;
        }
        if (!make_run_dir(e, out, id, sizeof id, dir, sizeof dir)) goto done;
        json_set_string(aspec, "job_id", id);
        json_set_string(aspec, "spec_hash", hash);
        path_join(path, sizeof path, dir, "spec.json");
        json_write_file(path, aspec, JSON_PRETTY | JSON_SORTED);
        MechTransientJob *J = calloc(1, sizeof *J);
        if (!J) {
            op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
            goto done;
        }
        J->model = *sub, J->mat = vm, J->density = c->density, J->elastic_modes = nel, J->zeta = zeta, J->npat = (int)P, J->nsteps = ns, J->dt = dt, J->t0 = t0;
        J->patterns = patterns, J->hist = hist, J->report = json_clone(report);
        memset(sub, 0, sizeof *sub), memset(&vm, 0, sizeof vm);
        c->density = NULL, patterns = NULL, hist = NULL;
        snprintf(J->job_id, sizeof J->job_id, "%s", id);
        snprintf(J->run_dir, sizeof J->run_dir, "%s", dir);
        JobSpec js = {id, "mech_transient", json_get_str(p, "label", "transient elastic response"), dir, hash, proj->id, proj->name, proj->revision, J,
                      mech_transient_job_run, mech_transient_job_free};
        if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
            op_fail(out, NV_ERR_BUSY, NULL, "%s", err);
            goto done;
        }
        JsonValue *v = json_object();
        json_set_string(v, "job_id", id);
        json_set_string(v, "state", "queued");
        json_set_string(v, "analysis", "mech_transient (elastic response of a moving part, one-way from the rigid dynamics)");
        json_set_string(v, "run_directory", dir);
        json_set(v, "sampling", json_clone(report));
        json_set_string(v, "next_step", "poll job_status; then mech_vibration_query with what summary or history");
        op_succeed(out, v);
    }
done:
    vib_material_free(&vm);
    free(patterns), free(hist), free(scratch), free(fdir);
    json_free(report), json_free(aspec);
    assess_close(c);
}

/* ------------------------------------------------------------------------------------------------ bindings */

/* ------------------------------------------------------------------------------------------------ flexible bodies */

static int mesh_set(const StaticModel *sub, const char *name) {
    for (int k = 0; name && k < sub->nsets; k++)
        if (!strcmp(sub->set[k].name, name)) return k;
    return -1;
}

static void op_mech_flexible_reduce(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    if (!c->asm) {
        op_fail(out, NV_ERR_PRECONDITION, "create the assembly with mech_assembly_import or mech_body_define", "there is no mechanical assembly");
        return;
    }
    Project *proj = e->proj;
    const Assembly *A = c->asm;
    const char *bname = json_get_str(p, "body", "");
    int bi = asm_body_index(A, bname);
    if (bi < 0) {
        op_fail(out, NV_ERR_NOT_FOUND, "mech_assembly_inspect lists the bodies", "no body '%s'", bname);
        return;
    }
    const AsmBody *B = &A->bodies[bi];
    Body *pb = B->part[0] ? project_body(proj, B->part) : NULL;
    if (!pb || strcmp(pb->name, B->part)) {
        op_fail(out, NV_ERR_PRECONDITION, "create the body from a project part (mech_body_define with part) so its mesh and frame are known",
                "body '%s' is not linked to a meshed project part", bname);
        return;
    }
    const JsonValue *dj = json_get(p, "damping");
    double zeta = json_get_num(dj, "ratio", NAN);
    const char *dsrc = json_get_str(dj, "source", NULL);
    if (!dj || !isfinite(zeta) || !dsrc) {
        op_fail(out, NV_ERR_PRECONDITION,
                "give damping {ratio, source}: printed parts have uncertain damping, so no default is assumed (for example {\"ratio\": 0.02, \"source\": \"assumed\"})",
                "the modal damping ratio and its source are missing");
        return;
    }
    if (!(zeta >= 0 && zeta < 1)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "damping ratio must be in [0, 1)");
        return;
    }
    long long nfix = json_get_int(p, "fixed_interface_modes", 8);
    double fcut = json_get_num(p, "max_frequency_hz", 0);
    if (nfix < 0 || nfix > 200 || !(fcut >= 0)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "fixed_interface_modes must be 0..200 and max_frequency_hz >= 0");
        return;
    }
    const char *root_sel = json_get_str(p, "root_selection", NULL);
    const JsonValue *itf = json_get(p, "interfaces");
    int ni = (int)json_len(itf);
    if (!root_sel || ni > 16 || (itf && itf->type != JSON_ARRAY)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "root_selection names the selection held by the body's own joint; interfaces is [{joint, selection}] (at most 16)",
                "root_selection is required and interfaces must be an array of at most 16 entries");
        return;
    }
    if (ni + nfix < 1) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "keep fixed-interface modes or name interfaces", "the reduction has no coordinates");
        return;
    }
    const MbJointDef *ij[16];
    for (int i = 0; i < ni; i++) {
        const JsonValue *it = json_at(itf, i);
        const char *jn = json_get_str(it, "joint", ""), *sel = json_get_str(it, "selection", NULL);
        int j = asm_joint_index(A, jn);
        if (j < 0 || !sel) {
            op_fail(out, NV_ERR_NOT_FOUND, "each interface is {joint, selection}: a joint whose parent is the body, and the selection it holds", "interface %d: %s", i,
                    j < 0 ? "no such joint" : "selection missing");
            return;
        }
        ij[i] = &A->joints[j].def;
        if (ij[i]->parent != bi) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "interfaces are the joints that carry child bodies of the flexible body", "joint '%s' does not have body '%s' as its parent",
                    jn, bname);
            return;
        }
        for (int k = 0; k < i; k++)
            if (ij[k] == ij[i]) {
                op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "joint '%s' is named by two interfaces", jn);
                return;
            }
    }
    const JsonValue *mmj = json_get(p, "material_model");
    const char *mmtype = json_get_str(mmj, "type", "project_material");
    bool use_ortho = !strcmp(mmtype, "orthotropic");
    if (!use_ortho && strcmp(mmtype, "project_material")) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "material_model.type is project_material or orthotropic", "unknown material model '%s'", mmtype);
        return;
    }
    MechStructSetup ortho;
    memset(&ortho, 0, sizeof ortho);
    if (use_ortho && !mech_struct_setup(proj, mmj, &ortho, out)) return;
    StaticModel full, sub;
    memset(&full, 0, sizeof full), memset(&sub, 0, sizeof sub);
    JsonValue *errors = json_array(), *warnings = json_array(), *aspec = NULL;
    double *density = NULL;
    VibMaterial vm;
    memset(&vm, 0, sizeof vm);
    MechFlexJob *J = NULL;
    char err[512];
    if (!part_mesh(proj, B->part, use_ortho ? &ortho : NULL, &full, &sub, &density, errors, warnings, out)) goto done;
    int rs = mesh_set(&sub, root_sel);
    if (rs < 0) {
        op_fail(out, NV_ERR_NOT_FOUND, "create the selection on the part before mesh_generate", "selection '%s' has no mesh faces on part '%s'", root_sel, B->part);
        goto done;
    }
    J = calloc(1, sizeof *J);
    if (!J) goto oom;
    J->ninterfaces = ni;
    J->root = malloc((size_t)sub.set[rs].nnodes * sizeof(int));
    J->itf_nodes = calloc((size_t)(ni ? ni : 1), sizeof *J->itf_nodes);
    J->itf_count = calloc((size_t)(ni ? ni : 1), sizeof(int));
    J->itf_point = calloc((size_t)(ni ? ni : 1), sizeof *J->itf_point);
    J->itf_joint = calloc((size_t)(ni ? ni : 1), sizeof *J->itf_joint);
    unsigned char *owner = calloc((size_t)(sub.nnodes ? sub.nnodes : 1), 1);
    if (!J->root || !J->itf_nodes || !J->itf_count || !J->itf_point || !J->itf_joint || !owner) {
        free(owner);
        goto oom;
    }
    J->nroot = sub.set[rs].nnodes;
    memcpy(J->root, sub.set[rs].nodes, (size_t)J->nroot * sizeof(int));
    for (int k = 0; k < J->nroot; k++) owner[J->root[k]] = 1;
    memcpy(J->R, pb->place.R, sizeof J->R), mv3_copy(J->p, pb->place.t);
    for (int i = 0; i < ni; i++) {
        const JsonValue *it = json_at(itf, i);
        const char *sel = json_get_str(it, "selection", "");
        int s = mesh_set(&sub, sel);
        if (s < 0) {
            free(owner);
            op_fail(out, NV_ERR_NOT_FOUND, "create the selection on the part before mesh_generate", "selection '%s' has no mesh faces on part '%s'", sel, B->part);
            goto done;
        }
        J->itf_count[i] = sub.set[s].nnodes;
        J->itf_nodes[i] = malloc((size_t)sub.set[s].nnodes * sizeof(int));
        if (!J->itf_nodes[i]) {
            free(owner);
            goto oom;
        }
        memcpy(J->itf_nodes[i], sub.set[s].nodes, (size_t)sub.set[s].nnodes * sizeof(int));
        for (int k = 0; k < J->itf_count[i]; k++) {
            if (owner[J->itf_nodes[i][k]]) {
                free(owner);
                op_fail(out, NV_ERR_INVALID_PARAMS, "the root and interface selections must not share mesh nodes: leave at least one element between them",
                        "selection '%s' shares nodes with the root or an earlier interface", sel);
                goto done;
            }
            owner[J->itf_nodes[i][k]] = (unsigned char)(2 + i);
        }
        snprintf(J->itf_joint[i], MB_NAME, "%s", ij[i]->name);
        mm3_mulv(J->itf_point[i], J->R, ij[i]->parent_frame.p);
        mv3_addto(J->itf_point[i], J->p);
    }
    free(owner);
    /* the joints of the body that are not interfaces ride the reference frame; the body's own joint should hold the root */
    {
        double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY}, rc[3] = {0, 0, 0};
        for (int k = 0; k < sub.nnodes; k++)
            for (int d3 = 0; d3 < 3; d3++) lo[d3] = fmin(lo[d3], sub.xyz[3 * k + d3]), hi[d3] = fmax(hi[d3], sub.xyz[3 * k + d3]);
        for (int k = 0; k < J->nroot; k++)
            for (int d3 = 0; d3 < 3; d3++) rc[d3] += sub.xyz[3 * J->root[k] + d3] / J->nroot;
        double size = sqrt(pow(hi[0] - lo[0], 2) + pow(hi[1] - lo[1], 2) + pow(hi[2] - lo[2], 2));
        for (int j = 0; j < A->njoints; j++) {
            const MbJointDef *Jd = &A->joints[j].def;
            if (Jd->child == bi) {
                double x[3];
                mm3_mulv(x, J->R, Jd->child_frame.p);
                mv3_addto(x, J->p);
                double dd = sqrt(pow(x[0] - rc[0], 2) + pow(x[1] - rc[1], 2) + pow(x[2] - rc[2], 2));
                if (dd > 0.1 * size)
                    json_push(warnings, json_stringf("joint '%s' holds '%s' %.3g mm from the centre of the root selection '%s' (%.0f%% of the part size): the root "
                                                     "region is clamped to the reference frame, so the load path between the joint and the root is rigid",
                                                     Jd->name, bname, dd * 1e3, root_sel, 100 * dd / size));
            }
            bool is_itf = false;
            for (int i = 0; i < ni; i++) is_itf |= ij[i] == Jd;
            if (Jd->parent == bi && !is_itf)
                json_push(warnings, json_stringf("joint '%s' is carried by '%s' but is not an interface: it will ride the reference frame (the root region), not the "
                                                 "deformed part", Jd->name, bname));
        }
    }
    if (!vib_material_from(&sub, use_ortho ? &ortho : NULL, &vm, err, sizeof err)) {
        op_fail(out, NV_ERR_PRECONDITION, "assign a material with elastic constants to the part (material_assign) or use an orthotropic record", "%s", err);
        goto done;
    }
    {
        char id[64], dir[NV_PATH_MAX], path[NV_PATH_MAX], hash[65];
        aspec = json_object();
        json_set_string(aspec, "format", "navier-mech-flexible");
        json_set_int(aspec, "version", 1);
        json_set_string(aspec, "body", bname);
        json_set_string(aspec, "part", B->part);
        json_set_string(aspec, "root_selection", root_sel);
        JsonValue *ia = json_set_array(aspec, "interfaces");
        for (int i = 0; i < ni; i++) {
            JsonValue *o = json_object();
            json_set_string(o, "joint", J->itf_joint[i]);
            json_set_string(o, "selection", json_get_str(json_at(itf, i), "selection", ""));
            json_set(o, "point_body_m", json_numbers(ij[i]->parent_frame.p, 3));
            json_push(ia, o);
        }
        json_set_int(aspec, "fixed_interface_modes", nfix);
        if (fcut > 0) json_set_number(aspec, "max_frequency_hz", fcut);
        json_set(aspec, "damping", json_clone(dj));
        json_set(aspec, "material_model", vm.ortho ? json_clone(vm.setup.description) : json_string("project_material"));
        json_set_string(aspec, "mesh_hash", sub.mesh_hash);
        JsonValue *pl = json_set_object(aspec, "part_placement");
        json_set(pl, "R", json_numbers(J->R, 9));
        json_set(pl, "t_m", json_numbers(J->p, 3));
        hash_json(aspec, hash);
        char active[64];
        if (jobs_find_active(e->jobs, hash, active, sizeof active)) {
            JsonValue *v = json_object();
            json_set_string(v, "job_id", active);
            json_set_bool(v, "deduplicated", true);
            op_succeed(out, v);
            goto done;
        }
        if (!make_run_dir(e, out, id, sizeof id, dir, sizeof dir)) goto done;
        json_set_string(aspec, "job_id", id);
        json_set_string(aspec, "spec_hash", hash);
        path_join(path, sizeof path, dir, "spec.json");
        json_write_file(path, aspec, JSON_PRETTY | JSON_SORTED);
        J->model = sub, J->mat = vm, J->density = density, J->fixed_modes = (int)nfix, J->zeta = zeta, J->max_frequency_hz = fcut;
        J->body_has_mass = B->has_inertial, J->body_mass = B->mass;
        memcpy(J->body_com, B->com, sizeof J->body_com), memcpy(J->body_inertia, B->inertia, sizeof J->body_inertia);
        J->setup = json_clone(aspec);
        memset(&sub, 0, sizeof sub), memset(&vm, 0, sizeof vm);
        density = NULL;
        snprintf(J->job_id, sizeof J->job_id, "%s", id);
        snprintf(J->run_dir, sizeof J->run_dir, "%s", dir);
        MechFlexJob *owned = J;
        J = NULL;
        JobSpec js = {id, "mech_flexible", json_get_str(p, "label", "flexible body reduction"), dir, hash, proj->id, proj->name, proj->revision, owned,
                      mech_flex_job_run, mech_flex_job_free};
        if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
            mech_flex_job_free(owned);
            op_fail(out, NV_ERR_BUSY, NULL, "%s", err);
            goto done;
        }
        JsonValue *v = json_object();
        json_set_string(v, "job_id", id);
        json_set_string(v, "state", "queued");
        json_set_string(v, "analysis", "mech_flexible (Craig-Bampton reduction of a body's part)");
        json_set_string(v, "run_directory", dir);
        json_set(v, "warnings", json_clone(warnings));
        json_set_string(v, "next_step", "poll job_status; the summary lists frequencies, interface compliance and mass properties; then mech_flexible_attach");
        op_succeed(out, v);
    }
    goto done;
oom:
    op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
done:
    mech_flex_job_free(J);
    mech_struct_setup_free(&ortho);
    vib_material_free(&vm);
    static_model_free(&full), static_model_free(&sub);
    free(density);
    json_free(errors), json_free(warnings), json_free(aspec);
}

static JsonValue *mass_json_si(double mass, const double com[3], const double I[9], const char *note) {
    JsonValue *o = json_object();
    char buf[64];
    json_set_string(o, "source", "explicit");
    json_set_string(o, "provenance", "computed");
    snprintf(buf, sizeof buf, "%.17g kg", mass);
    json_set_string(o, "mass", buf);
    JsonValue *cj = json_set_array(o, "com");
    for (int k = 0; k < 3; k++) snprintf(buf, sizeof buf, "%.17g m", com[k]), json_push(cj, json_string(buf));
    JsonValue *in = json_set_object(o, "inertia");
    static const char *const N[6] = {"ixx", "iyy", "izz", "ixy", "ixz", "iyz"};
    static const int IDX[6] = {0, 4, 8, 1, 2, 5};
    for (int k = 0; k < 6; k++) snprintf(buf, sizeof buf, "%.17g kg*m^2", I[IDX[k]]), json_set_string(in, N[k], buf);
    json_set_string(o, "note", note);
    return o;
}

/* replaces the assembly with a (validated) copy carrying the body patch */
static bool apply_body_patch(Engine *e, MechCache *c, JsonValue *body, MechDiag *d, OpResult *out, const char *what) {
    Assembly *a = asm_clone(c->asm);
    AsmLoadOptions opt = {.require_units = true};
    bool ok = a && asm_body_update_json(a, body, NULL, e->proj->dir, &opt, d);
    MbModelDef *def = ok ? asm_to_model(a, d) : NULL;
    MbModel *m = def ? mb_compile(def, NULL, d) : NULL;
    ok = ok && m;
    mb_model_free(m), mbdef_free(def);
    if (!ok) {
        fail_diag(out, d, what);
        asm_free(a);
        return false;
    }
    asm_free(c->asm);
    c->asm = a;
    return mech_save(e, c, out);
}

static void op_mech_flexible_attach(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    if (!c->asm) {
        op_fail(out, NV_ERR_PRECONDITION, "create the assembly first", "there is no mechanical assembly");
        return;
    }
    const char *id = json_get_str(p, "job_id", "");
    char dir[NV_PATH_MAX], path[NV_PATH_MAX];
    if (!run_dir_locked(e, id, dir, sizeof dir)) { /* mutations hold the engine lock */
        op_fail(out, NV_ERR_NOT_FOUND, "use the job_id returned by mech_flexible_reduce", "no run directory for job '%s'", id);
        return;
    }
    JsonError je;
    path_join(path, sizeof path, dir, "spec.json");
    JsonValue *spec = json_read_file(path, 16 << 20, &je), *block = NULL, *body = NULL;
    AsmFlexible *X = NULL;
    MechDiag d;
    mdiag_init(&d);
    if (!spec || strcmp(json_get_str(spec, "format", ""), "navier-mech-flexible")) {
        op_fail(out, NV_ERR_UNSUPPORTED, "attach the result of mech_flexible_reduce", "job '%s' is not a flexible-body reduction", id);
        goto done;
    }
    const char *bname = json_get_str(spec, "body", "");
    if (json_get_str(p, "body", NULL) && strcmp(json_get_str(p, "body", ""), bname)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "job '%s' reduced body '%s', not '%s'", id, bname, json_get_str(p, "body", ""));
        goto done;
    }
    path_join(path, sizeof path, dir, "flexible_model.json");
    block = json_read_file(path, 64 << 20, &je);
    if (!block) {
        op_fail(out, NV_ERR_PRECONDITION, "poll job_status until the reduction job has succeeded", "job '%s' has no reduced model yet", id);
        goto done;
    }
    X = asm_flexible_from_json(block, &d, bname);
    if (!X) {
        fail_diag(out, &d, "the reduced model cannot be read");
        goto done;
    }
    int bi = asm_body_index(c->asm, bname);
    if (bi < 0) {
        op_fail(out, NV_ERR_NOT_FOUND, NULL, "body '%s' is no longer in the assembly", bname);
        goto done;
    }
    const AsmBody *B = &c->asm->bodies[bi];
    if (strcmp(e->proj->mesh.hash, json_get_str(spec, "mesh_hash", ""))) {
        op_fail(out, NV_ERR_PRECONDITION, "reduce the body again (mech_flexible_reduce) with the current mesh",
                "the reduction is stale: the project mesh changed since job '%s' (mesh_generate or a geometry change)", id);
        goto done;
    }
    if (strcmp(B->part, json_get_str(spec, "part", ""))) {
        op_fail(out, NV_ERR_PRECONDITION, "reduce the body again (mech_flexible_reduce)", "body '%s' is now linked to part '%s', the reduction used '%s'", bname, B->part,
                json_get_str(spec, "part", ""));
        goto done;
    }
    for (int i = 0; i < X->model.ninterfaces; i++) {
        int j = asm_joint_index(c->asm, X->interface_joint[i]);
        const MbJointDef *Jd = j >= 0 ? &c->asm->joints[j].def : NULL;
        double dp = Jd ? sqrt(pow(Jd->parent_frame.p[0] - X->model.interfaces[i].point[0], 2) + pow(Jd->parent_frame.p[1] - X->model.interfaces[i].point[1], 2) +
                              pow(Jd->parent_frame.p[2] - X->model.interfaces[i].point[2], 2))
                     : 0;
        if (!Jd || Jd->parent != bi || dp > 1e-9) {
            op_fail(out, NV_ERR_PRECONDITION, "reduce the body again with the current joints (mech_flexible_reduce)",
                    "the reduction is stale: interface joint '%s' %s", X->interface_joint[i],
                    !Jd ? "no longer exists" : (Jd->parent != bi ? "no longer has the body as its parent" : "has moved since the reduction"));
            goto done;
        }
    }
    double old_mass = B->has_inertial ? B->mass : 0;
    char note[400];
    snprintf(note, sizeof note, "reduced FE mesh of part '%s' (flexible body reduction job %s); consistent with the elastic coordinates", B->part, id);
    body = json_object();
    json_set_string(body, "name", bname);
    json_set(body, "mass_properties", mass_json_si(X->fe_mass, X->fe_com, X->fe_inertia, note));
    json_set(body, "flexible", json_clone(block));
    if (!apply_body_patch(e, c, body, &d, out, "the flexible body does not fit the assembly")) goto done;
    engine_touch(e, "mech_flexible_attach", "body '%s' flexible (%d coordinates)", bname, X->model.nmodes);
    JsonValue *v = assembly_value(e, c, &d);
    JsonValue *fx = json_set_object(v, "flexible");
    json_set_string(fx, "body", bname);
    json_set_int(fx, "elastic_coordinates", X->model.nmodes);
    json_set_number(fx, "lowest_frequency_hz", sqrt(X->model.omega2[0]) / (2 * M_PI));
    json_set_number(fx, "highest_frequency_hz", sqrt(X->model.omega2[X->model.nmodes - 1]) / (2 * M_PI));
    json_set_number(fx, "stability_step_s", 0.5 / sqrt(X->model.omega2[X->model.nmodes - 1]));
    json_set_number(fx, "mass_kg", X->fe_mass);
    if (old_mass > 0) json_set_number(fx, "previous_mass_kg", old_mass);
    json_set_string(fx, "reading", "the body's mass properties are now those of the reduced mesh; dynamics runs integrate the elastic coordinates with the rigid "
                                   "motion, with steps limited to the stability step");
    op_succeed(out, v);
done:
    asm_flexible_free(X);
    json_free(spec), json_free(block), json_free(body);
    mdiag_free(&d);
}

static void op_mech_flexible_detach(Engine *e, JsonValue *p, OpResult *out) {
    MechCache *c = mech_state(e, out);
    if (!c) return;
    const char *bname = json_get_str(p, "body", "");
    int bi = c->asm ? asm_body_index(c->asm, bname) : -1;
    if (bi < 0) {
        op_fail(out, NV_ERR_NOT_FOUND, "mech_assembly_inspect lists the bodies", "no body '%s'", bname);
        return;
    }
    if (!c->asm->bodies[bi].flexible) {
        op_fail(out, NV_ERR_PRECONDITION, NULL, "body '%s' is already rigid", bname);
        return;
    }
    MechDiag d;
    mdiag_init(&d);
    JsonValue *body = json_object();
    json_set_string(body, "name", bname);
    json_set(body, "flexible", json_null());
    if (apply_body_patch(e, c, body, &d, out, "the body cannot be made rigid")) {
        engine_touch(e, "mech_flexible_detach", "body '%s' rigid", bname);
        JsonValue *v = assembly_value(e, c, &d);
        json_set_string(v, "note", "the body keeps the mass properties of the reduced mesh; redefine them with mech_body_define to return to the fill model");
        op_succeed(out, v);
    }
    json_free(body);
    mdiag_free(&d);
}

/* the elastic coordinates of a body recorded by a run (channels <body>.elastic[k]) at the instants of a window */
static bool read_elastic_histories(const char *path, const char *body, int ncoord, double t0, double t1, double **t_out, double **eta_out, int *rows_out,
                                   char *err, size_t errlen) {
    *t_out = NULL, *eta_out = NULL, *rows_out = 0;
    FILE *f = fopen(path, "rb");
    char magic[8];
    uint32_t nc = 0, nr = 0;
    if (!f || fread(magic, 1, 8, f) != 8 || memcmp(magic, "NVMHIST1", 8) || fread(&nc, 4, 1, f) != 1 || fread(&nr, 4, 1, f) != 1 || nc > 100000 || nr == 0) {
        if (f) fclose(f);
        snprintf(err, errlen, "cannot read the recorded histories %s", path);
        return false;
    }
    int *col = malloc((size_t)ncoord * sizeof(int));
    double *t = malloc(nr * sizeof(double)), *tmp = malloc(nr * sizeof(double));
    bool ok = col && t && tmp;
    for (int k = 0; ok && k < ncoord; k++) col[k] = -1;
    for (uint32_t c = 0; c < nc && ok; c++) {
        char name[81] = "", unit[17] = "", want[96];
        uint16_t ln, lu;
        ok = fread(&ln, 2, 1, f) == 1 && ln < 80 && fread(name, 1, ln, f) == ln && fread(&lu, 2, 1, f) == 1 && lu < 16 && fread(unit, 1, lu, f) == lu;
        name[ok ? ln : 0] = 0;
        for (int k = 0; ok && k < ncoord; k++) {
            snprintf(want, sizeof want, "%s.elastic[%d]", body, k);
            if (!strcmp(name, want)) col[k] = (int)c;
        }
    }
    long data0 = ftell(f);
    for (int k = 0; ok && k < ncoord; k++)
        if (col[k] < 0) {
            snprintf(err, errlen, "the run did not record %s.elastic[%d]: it was not run with this flexible body", body, k);
            ok = false;
        }
    ok = ok && fseek(f, data0, SEEK_SET) == 0 && fread(t, sizeof(double), nr, f) == nr; /* channel 0 is time */
    uint32_t first = 0, count = 0;
    for (uint32_t r = 0; ok && r < nr; r++)
        if (t[r] >= t0 - 1e-12 && t[r] <= t1 + 1e-12) {
            if (!count) first = r;
            count++;
        }
    if (ok && !count) {
        snprintf(err, errlen, "no recorded instant between %.9g s and %.9g s", t0, t1);
        ok = false;
    }
    double *eta = ok ? malloc((size_t)count * (size_t)ncoord * sizeof(double)) : NULL, *tw = ok ? malloc(count * sizeof(double)) : NULL;
    ok = ok && eta && tw;
    for (int k = 0; ok && k < ncoord; k++) {
        ok = fseek(f, data0 + (long)col[k] * (long)nr * (long)sizeof(double), SEEK_SET) == 0 && fread(tmp, sizeof(double), nr, f) == nr;
        for (uint32_t r = 0; ok && r < count; r++) eta[(size_t)r * (size_t)ncoord + (size_t)k] = tmp[first + r];
    }
    for (uint32_t r = 0; ok && r < count; r++) tw[r] = t[first + r];
    fclose(f);
    free(col), free(t), free(tmp);
    if (!ok) {
        if (!err[0]) snprintf(err, errlen, "the recorded histories %s are truncated", path);
        free(eta), free(tw);
        return false;
    }
    *t_out = tw, *eta_out = eta, *rows_out = (int)count;
    return true;
}

static void op_mech_flexible_stress(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    const char *id = json_get_str(p, "job_id", ""), *bname = json_get_str(p, "body", "");
    char dir[NV_PATH_MAX], rdir[NV_PATH_MAX], path[NV_PATH_MAX], err[512] = "", qerr[200];
    if (!mech_run_dir(e, out, id, dir, sizeof dir, true)) return; /* mutations hold the engine lock */
    JsonError je;
    path_join(path, sizeof path, dir, "spec.json");
    JsonValue *spec = json_read_file(path, 64 << 20, &je), *report = NULL, *rsum = NULL;
    MechFlexStressJob *J = NULL;
    const JsonValue *bodies = json_get(json_get(spec, "assembly"), "bodies"), *fx = NULL;
    for (size_t i = 0; i < json_len(bodies); i++)
        if (!strcmp(json_get_str(json_at(bodies, i), "name", ""), bname)) fx = json_get(json_at(bodies, i), "flexible");
    if (!fx) {
        op_fail(out, NV_ERR_PRECONDITION, "attach a reduction with mech_flexible_attach and run the study again", "body '%s' is not a flexible body in run '%s'", bname, id);
        goto done;
    }
    const char *rid = json_get_str(json_get(fx, "provenance"), "job_id", "");
    if (!run_dir_locked(e, rid, rdir, sizeof rdir) || !path_join(path, sizeof path, rdir, "mech_flex_shapes.bin") || !path_is_file(path)) {
        op_fail(out, NV_ERR_PRECONDITION, "reduce the body again (mech_flexible_reduce), attach it and rerun the study",
                "the coordinate shapes of reduction job '%s' are not available", rid[0] ? rid : "?");
        goto done;
    }
    J = calloc(1, sizeof *J);
    if (!J) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        goto done;
    }
    snprintf(J->shapes_path, sizeof J->shapes_path, "%s", path);
    J->ncoord = (int)json_len(json_get(fx, "omega2"));
    double Tend = json_get_num(json_get(json_get(spec, "study"), "settings"), "end_time", 0), t0 = 0, t1 = Tend;
    if ((json_get(p, "time_from") && !mech_qty_from_json_ex(json_get(p, "time_from"), MQ_TIME, NULL, true, &t0, qerr, sizeof qerr)) ||
        (json_get(p, "time_to") && !mech_qty_from_json_ex(json_get(p, "time_to"), MQ_TIME, NULL, true, &t1, qerr, sizeof qerr))) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "for example \"0.2 s\"", "time window: %s", qerr);
        goto done;
    }
    if (!(t0 >= 0 && t1 >= t0)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "the window must satisfy 0 <= time_from <= time_to");
        goto done;
    }
    path_join(path, sizeof path, dir, "histories.bin");
    if (!read_elastic_histories(path, bname, J->ncoord, t0, t1, &J->t, &J->eta, &J->nrows, err, sizeof err)) {
        op_fail(out, NV_ERR_PRECONDITION, "choose a window inside the run and a body that was flexible in it", "%s", err);
        goto done;
    }
    J->record_period = J->nrows > 1 ? (J->t[J->nrows - 1] - J->t[0]) / (J->nrows - 1) : 0;
    report = json_object();
    json_set_string(report, "format", "navier-mech-flexible-stress");
    json_set_int(report, "version", 1);
    json_set_string(report, "source_job", id);
    json_set_string(report, "body", bname);
    json_set_string(report, "reduction_job", rid);
    json_set_number(report, "time_from_s", t0), json_set_number(report, "time_to_s", t1);
    json_set_int(report, "samples", J->nrows);
    json_set_int(report, "elastic_coordinates", J->ncoord);
    path_join(path, sizeof path, rdir, "summary.json");
    rsum = json_read_file(path, 16 << 20, &je);
    const JsonValue *red = json_get(rsum, "reduction");
    if (red) {
        JsonValue *rl = json_set_object(report, "reduction_truncation");
        json_set_int(rl, "dropped_above_cutoff", json_get_int(red, "dropped_above_cutoff", 0));
        json_set_number(rl, "largest_interface_compliance_loss", json_get_num(red, "largest_interface_compliance_loss", 0));
    }
    {
        char jid[64], jdir[NV_PATH_MAX], hash[65];
        hash_json(report, hash);
        char active[64];
        if (jobs_find_active(e->jobs, hash, active, sizeof active)) {
            JsonValue *v = json_object();
            json_set_string(v, "job_id", active);
            json_set_bool(v, "deduplicated", true);
            op_succeed(out, v);
            goto done;
        }
        if (!make_run_dir(e, out, jid, sizeof jid, jdir, sizeof jdir)) goto done;
        json_set_string(report, "job_id", jid);
        json_set_string(report, "spec_hash", hash);
        path_join(path, sizeof path, jdir, "spec.json");
        json_write_file(path, report, JSON_PRETTY | JSON_SORTED);
        J->report = json_clone(report);
        snprintf(J->job_id, sizeof J->job_id, "%s", jid);
        snprintf(J->run_dir, sizeof J->run_dir, "%s", jdir);
        MechFlexStressJob *owned = J;
        J = NULL;
        JobSpec js = {jid, "mech_flexible_stress", json_get_str(p, "label", "flexible body stress"), jdir, hash, proj->id, proj->name, proj->revision, owned,
                      mech_flex_stress_job_run, mech_flex_stress_job_free};
        if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
            mech_flex_stress_job_free(owned);
            op_fail(out, NV_ERR_BUSY, NULL, "%s", err);
            goto done;
        }
        JsonValue *v = json_object();
        json_set_string(v, "job_id", jid);
        json_set_string(v, "state", "queued");
        json_set_string(v, "analysis", "mech_flexible_stress (stresses of a flexible body from its coupled elastic coordinates)");
        json_set_string(v, "run_directory", jdir);
        json_set_int(v, "samples", json_get_int(report, "samples", 0));
        json_set_string(v, "next_step", "poll job_status; then mech_vibration_query with what summary or history");
        op_succeed(out, v);
    }
done:
    mech_flex_stress_job_free(J);
    json_free(spec), json_free(report), json_free(rsum);
}

/* ------------------------------------------------------------------------------------------------ printing */

/* one process input with its unit; missing required inputs are collected and refused together */
static bool print_input(OpResult *out, const JsonValue *pr, const char *key, Dimension dim, const char *unit, double *si, bool required,
                        JsonValue *missing, JsonValue *defaults) {
    bool present = false;
    if (!op_quantity(out, pr, key, dim, unit, si, &present)) return false;
    if (!present) {
        if (required) json_push(missing, json_string(key));
        else json_push(defaults, json_string(key));
    }
    return true;
}

static void op_mech_print_run(Engine *e, JsonValue *p, OpResult *out) {
    if (!e->proj) {
        op_fail(out, NV_ERR_PRECONDITION, "project_open or project_create first", "no project is open");
        return;
    }
    const JsonValue *pr = json_get(p, "process");
    if (!pr) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "give process with at least layer_height, nozzle_temperature, bed_temperature, ambient_temperature, "
                                            "deposition_rate and provenance",
                "the print needs a process block");
        return;
    }
    PrintSettings s;
    memset(&s, 0, sizeof s);
    snprintf(s.body, sizeof s.body, "%s", json_get_str(p, "body", ""));
    s.h_conv = 15.0, s.radiation = true, s.min_layer_time = 10.0, s.cooldown_bed_on = 1200.0, s.cooldown_bed_off = 3600.0;
    s.thermal_substeps = 8, s.relaxation_offset = 10.0, s.printed_layer_height = 0.0002;
    s.formulation = HEX8_INCOMPATIBLE, s.solver = SOLID_SOLVER_AUTO, s.pcg_tol = 1e-10;
    JsonValue *missing = json_array(), *defaults = json_array();
    bool ok = print_input(out, pr, "layer_height", DIM_LENGTH, "mm", &s.layer_height, true, missing, defaults) &&
              print_input(out, pr, "printed_layer_height", DIM_LENGTH, "mm", &s.printed_layer_height, false, missing, defaults) &&
              print_input(out, pr, "nozzle_temperature", DIM_TEMPERATURE, "degC", &s.nozzle_t, true, missing, defaults) &&
              print_input(out, pr, "bed_temperature", DIM_TEMPERATURE, "degC", &s.bed_t, true, missing, defaults) &&
              print_input(out, pr, "ambient_temperature", DIM_TEMPERATURE, "degC", &s.ambient_t, true, missing, defaults) &&
              print_input(out, pr, "convection", DIM_HEAT_TRANSFER_COEFFICIENT, "W/(m^2 K)", &s.h_conv, false, missing, defaults) &&
              print_input(out, pr, "min_layer_time", DIM_TIME, "s", &s.min_layer_time, false, missing, defaults) &&
              print_input(out, pr, "cooldown_bed_on", DIM_TIME, "s", &s.cooldown_bed_on, false, missing, defaults) &&
              print_input(out, pr, "cooldown_bed_off", DIM_TIME, "s", &s.cooldown_bed_off, false, missing, defaults) &&
              print_input(out, pr, "relaxation_offset", DIM_TEMPERATURE_DIFFERENCE, "K", &s.relaxation_offset, false, missing, defaults);
    if (!ok) {
        json_free(missing), json_free(defaults);
        return;
    }
    s.radiation = json_get_bool(pr, "radiation", true);
    s.thermal_substeps = (int)json_get_int(pr, "thermal_substeps", 8);
    const JsonValue *rate = json_get(pr, "deposition_rate");
    char uerr[256];
    if (!rate) json_push(missing, json_string("deposition_rate"));
    else if (!print_parse_volume_rate(rate, &s.deposition_rate, uerr, sizeof uerr)) {
        json_free(missing), json_free(defaults);
        op_fail(out, NV_ERR_INVALID_UNIT, NULL, "deposition_rate: %s", uerr);
        return;
    }
    const char *prov = json_get_str(pr, "provenance", "");
    int pv = provenance_from_name(prov);
    s.provenance = pv < 0 ? PROV_COUNT : (Provenance)pv;
    if (!prov[0]) json_push(missing, json_string("provenance"));
    if (json_len(missing) > 0) {
        op_fail(out, NV_ERR_PRECONDITION, "state every input with its unit, and provenance (user, inferred or calibrated); the print does not guess "
                                          "printer settings",
                "the print process is incomplete: %zu required inputs are missing", json_len(missing));
        op_fail_detail(out, "missing_inputs", missing);
        json_free(defaults);
        return;
    }
    if (pv < 0) {
        json_free(missing), json_free(defaults);
        op_fail(out, NV_ERR_INVALID_PARAMS, "user, inferred or calibrated", "provenance '%s' is not one of the known kinds", prov);
        return;
    }
    json_free(missing);
    s.store_substeps = !strcmp(json_get_str(p, "store", "key_times"), "substeps");
    const JsonValue *num = json_get(p, "numerics");
    const char *sv = json_get_str(num, "solver", "auto");
    s.solver = !strcmp(sv, "direct") ? SOLID_SOLVER_DIRECT : (!strcmp(sv, "iterative") ? SOLID_SOLVER_PCG : SOLID_SOLVER_AUTO);
    s.pcg_tol = json_get_num(num, "tolerance", 1e-10);
    if (!op_quantity(out, num, "skip_increment_below", DIM_TEMPERATURE_DIFFERENCE, "K", &s.skip_below_k, NULL)) {
        json_free(defaults);
        return;
    }
    const JsonValue *probes = json_get(p, "probes");
    for (size_t i = 0; i < json_len(probes) && s.nprobes < PRINT_MAX_PROBES; i++) {
        const JsonValue *o = json_at(probes, i);
        double xyz[3] = {0, 0, 0};
        const JsonValue *at = json_get(o, "at");
        for (int k = 0; k < 3 && k < (int)json_len(at); k++) {
            double v = 0;
            char qerr[200];
            if (!quantity_from_json(json_at(at, (size_t)k), DIM_LENGTH, "mm", &v, qerr, sizeof qerr)) {
                json_free(defaults);
                op_fail(out, NV_ERR_INVALID_UNIT, NULL, "probe %zu: %s", i, qerr);
                return;
            }
            xyz[k] = v;
        }
        memcpy(s.probe[s.nprobes], xyz, sizeof xyz);
        snprintf(s.probe_label[s.nprobes], sizeof s.probe_label[s.nprobes], "%s", json_get_str(o, "label", "probe"));
        s.nprobes++;
    }
    const JsonValue *sup = json_get(p, "supports");
    if (sup) { /* the same supports object as lpbf_build_run; removed after the bed release unless remove is false */
        snprintf(s.sup.body, sizeof s.sup.body, "%s", s.body);
        s.sup.support_height = -1;
        if (!supports_block(sup, true, &s.sup, out)) {
            json_free(defaults);
            return;
        }
        s.supports = true;
    }
    JsonValue *errors = json_array(), *warnings = json_array();
    PrintCase *pc = NULL;
    if (!print_case_build(e->proj, &s, &pc, errors, warnings)) {
        const JsonValue *first = json_at(errors, 0);
        op_fail(out, nv_err_from_name(json_get_str(first, "code", "PRECONDITION_FAILED")), json_get_str(first, "hint", NULL), "the print cannot run: %s",
                json_get_str(first, "message", ""));
        op_fail_detail(out, "errors", errors);
        op_fail_detail(out, "warnings", warnings);
        json_free(defaults);
        return;
    }
    JsonValue *model = print_model_json(pc);
    json_set(model, "defaults_used", json_clone(defaults));
    JsonValue *spec = json_object();
    json_set_string(spec, "analysis", "fff_print");
    json_set_string(spec, "project", e->proj->name);
    json_set(spec, "model", json_clone(model));
    json_set_string(spec, "material", pc->material_id);
    json_set_string(spec, "material_status", pc->material_status);
    json_set_string(spec, "mesh_hash", pc->c.mesh_hash);
    JsonValue *pj = json_set_array(spec, "probes");
    for (int i = 0; i < s.nprobes; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "label", s.probe_label[i]);
        json_set(o, "at_mm", json_vec3(1e3 * s.probe[i][0], 1e3 * s.probe[i][1], 1e3 * s.probe[i][2]));
        json_push(pj, o);
    }
    char hash[65];
    hash_json(spec, hash);
    char active[64];
    if (!json_get_bool(p, "allow_duplicate", false) && jobs_find_active(e->jobs, hash, active, sizeof active)) {
        JsonValue *v = json_object();
        json_set_string(v, "job_id", active);
        json_set_bool(v, "deduplicated", true);
        json_set_string(v, "spec_hash", hash);
        json_set_string(v, "message", "an identical print is already queued or running; its job is returned instead of starting a second one");
        print_case_free(pc);
        json_free(spec), json_free(model), json_free(errors), json_free(warnings), json_free(defaults);
        op_succeed(out, v);
        return;
    }
    char id[64], dir[NV_PATH_MAX], path[NV_PATH_MAX], now[32], err[512];
    if (!make_run_dir(e, out, id, sizeof id, dir, sizeof dir)) {
        print_case_free(pc);
        json_free(spec), json_free(model), json_free(errors), json_free(warnings), json_free(defaults);
        return;
    }
    iso_time_now(now, sizeof now);
    JsonValue *stored = json_clone(spec);
    json_set_string(stored, "job_id", id);
    json_set_string(stored, "created", now);
    json_set_int(stored, "project_revision", (long long)e->proj->revision);
    json_set_string(stored, "spec_hash", hash);
    json_set_string(stored, "spec_hash_covers", "every field except job_id, created, project_revision and spec_hash");
    path_join(path, sizeof path, dir, "spec.json");
    bool written = json_write_file(path, stored, JSON_PRETTY | JSON_SORTED);
    json_free(stored);
    if (!written) {
        print_case_free(pc);
        json_free(spec), json_free(model), json_free(errors), json_free(warnings), json_free(defaults);
        op_fail(out, NV_ERR_IO, NULL, "cannot write %s", path);
        return;
    }
    snprintf(pc->c.run_dir, sizeof pc->c.run_dir, "%s", dir);
    snprintf(pc->c.job_id, sizeof pc->c.job_id, "%s", id);
    JobSpec js = {id, "fff_print", json_get_str(p, "label", ""), dir, hash, e->proj->id, e->proj->name, e->proj->revision, pc, print_job_run, print_case_free};
    if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
        json_free(spec), json_free(model), json_free(errors), json_free(warnings), json_free(defaults);
        op_fail(out, NV_ERR_BUSY, "check job_list and retry when jobs have finished", "%s", err);
        return;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "state", "queued");
    json_set_string(v, "analysis", "fff_print");
    json_set_string(v, "run_directory", dir);
    json_set_string(v, "spec_file", path);
    json_set_string(v, "spec_hash", hash);
    json_set(v, "model", model);
    json_set(v, "warnings", warnings);
    json_set_string(v, "scope", "process simulation on library material data: the summary's scope block states what these numbers are not");
    json_set_string(v, "next_step",
                    "poll job_status with {\"job_id\": \"...\", \"wait_seconds\": 30}; then results_query, results_probe, results_render and results_export "
                    "read the print with time_index or time_s, the last stored time being the state after release from the bed");
    json_free(spec), json_free(errors), json_free(defaults);
    op_succeed(out, v);
}

/* ------------------------------------------------------------------------------------------------ LPBF build */

/* the engine's provenance vocabulary plus "assumed", which the validation geometry file uses for a value no source
 * states. It is recorded literally and treated as a stated modelling choice (inferred). */
/* The enum has four values; the words people actually write are more than four, and the word matters. The literal
 * word is kept in `text` and reported; the enum is what the rest of the engine stores. */
static Provenance lpbf_provenance(const char *word, char *text, size_t textlen) {
    snprintf(text, textlen, "%s", word);
    if (!strcmp(word, "assumed")) return PROV_INFERRED;
    if (!strcmp(word, "measured") || !strcmp(word, "published")) return PROV_USER;
    int pv = provenance_from_name(word);
    return pv < 0 ? PROV_COUNT : (Provenance)pv;
}

/* One support parameter: a quantity ("0.5 mm", or {"value": "0.5 mm", "provenance": ..., "source": ...}), or absent,
 * when the default is taken and recorded as assumed with the reason. A value given without a provenance and a source,
 * per value or once for the whole supports object, is refused. */
static bool sup_param(const JsonValue *obj, const char *key, Dimension dim, const char *unit, double scale_to_unit, double def_si,
                      const char *def_why, const char *prov0, const char *src0, LpbfSettings *s, double *dst, OpResult *out) {
    const JsonValue *v = json_get(obj, key);
    char prov[16] = "assumed", src[200];
    double si = def_si;
    snprintf(src, sizeof src, "%s", def_why);
    if (v) {
        bool present = false;
        const JsonValue *holder = obj;
        const char *k = key;
        if (v->type == JSON_OBJECT && (json_get(v, "provenance") || json_get(v, "source"))) {
            holder = v, k = "value";
            snprintf(prov, sizeof prov, "%s", json_get_str(v, "provenance", ""));
            snprintf(src, sizeof src, "%s", json_get_str(v, "source", ""));
        } else {
            snprintf(prov, sizeof prov, "%s", prov0);
            snprintf(src, sizeof src, "%s", src0);
        }
        if (dim == DIM_DIMENSIONLESS) { /* a plain number */
            const JsonValue *q = json_get(holder, k);
            present = q && q->type == JSON_NUMBER;
            if (present) si = q->u.number;
            else if (q) {
                op_fail(out, NV_ERR_INVALID_PARAMS, "a number, for example 0.5", "supports.%s must be a plain number", key);
                return false;
            }
        } else if (!op_quantity(out, holder, k, dim, unit, &si, &present))
            return false;
        char tmp[24];
        if (!present || lpbf_provenance(prov, tmp, sizeof tmp) == PROV_COUNT || !src[0]) {
            op_fail(out, NV_ERR_PRECONDITION, "give it as {\"value\": \"0.2 mm\", \"provenance\": \"user\", \"source\": \"...\"}, or give "
                                              "supports.provenance and supports.source once for the plain values",
                    "supports.%s has no %s: every support parameter is a modelling input with a source", key,
                    !present ? "value" : (!src[0] ? "source" : "valid provenance (user, published, measured, assumed, inferred, calibrated)"));
            return false;
        }
    }
    *dst = si;
    if (s->support_nparam < (int)(sizeof s->support_param / sizeof s->support_param[0])) {
        __typeof__(s->support_param[0]) *r = &s->support_param[s->support_nparam++];
        snprintf(r->name, sizeof r->name, "%s", key);
        snprintf(r->unit, sizeof r->unit, "%s", unit);
        snprintf(r->prov, sizeof r->prov, "%s", prov);
        snprintf(r->source, sizeof r->source, "%s", src);
        r->value = si * scale_to_unit;
    }
    return true;
}

#define TUT_NONE "assumed: CADS Additive's support parameters are documented in its infosheets, not in the Simufact Additive tutorial (p. 54), and the owner's files do not hold them"
#define TUT_SHELL "assumed: the tutorial says a support's shell thickness directly sets its stiffness (p. 56) and gives no value"
#define TUT_RADIUS "assumed: the tutorial's example sets a support radius of 0.45 mm (p. 224), an example and not a default"

static bool lpbf_parse_supports(const JsonValue *sup, LpbfSettings *sp, OpResult *out) {
    LpbfSettings *s = sp;
    const char *prov0 = json_get_str(sup, "provenance", ""), *src0 = json_get_str(sup, "source", "");
    const char *type = json_get_str(sup, "type", NULL);
    SupportSpec *t = &s->support;
    memset(t, 0, sizeof *t);
    if (!type && !json_get(sup, "stiffness_fraction")) {
        op_fail(out, NV_ERR_PRECONDITION, "supports.type: block, thin_wall, cone, tree or lattice (or stiffness_fraction for a "
                                          "homogeneous lattice of a given fraction)",
                "the supports need a type or a stiffness fraction");
        return false;
    }
    if (!type || !strcmp(type, "homogeneous")) { /* wave 4: a given fraction, one provenance for it */
        t->type = SUPPORT_HOMOGENEOUS, t->relative_density = 1;
        s->support_fraction = json_get_num(sup, "stiffness_fraction", -1);
        if (!(s->support_fraction > 0 && s->support_fraction <= 1)) {
            op_fail(out, NV_ERR_PRECONDITION, "supports.stiffness_fraction in (0, 1]",
                    "a homogeneous support needs a stiffness fraction of the solid; Simufact's default is not known to us");
            return false;
        }
        t->fraction = s->support_fraction;
        s->support_provenance = lpbf_provenance(prov0, s->support_prov_text, sizeof s->support_prov_text);
        snprintf(s->support_source, sizeof s->support_source, "%s", src0);
        if (s->support_provenance == PROV_COUNT || !s->support_source[0]) {
            op_fail(out, NV_ERR_PRECONDITION, "supports.provenance and supports.source",
                    "a support stiffness fraction is a modelling choice: say where it comes from");
            return false;
        }
    } else {
        if (!support_type_from_name(type, &t->type)) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "block, thin_wall, cone, tree, lattice or homogeneous", "unknown support type '%s'", type);
            return false;
        }
        s->support_provenance = lpbf_provenance(prov0[0] ? prov0 : "assumed", s->support_prov_text, sizeof s->support_prov_text);
        snprintf(s->support_source, sizeof s->support_source, "%s", src0[0] ? src0 : "the parameters below, each with its own source");
        const double MM = 1e-3, TO_MM = 1e3;
        bool ok = true;
        switch (t->type) {
        case SUPPORT_BLOCK:
        case SUPPORT_THIN_WALL:
            ok = sup_param(sup, "wall_thickness", DIM_LENGTH, "mm", TO_MM, 0.2 * MM, TUT_SHELL, prov0, src0, s, &t->wall_thickness, out) &&
                 sup_param(sup, "spacing", DIM_LENGTH, "mm", TO_MM, 1.0 * MM, TUT_NONE, prov0, src0, s, &t->spacing, out);
            if (ok && t->type == SUPPORT_THIN_WALL) {
                const char *dir = json_get_str(sup, "direction", "x");
                t->direction = !strcmp(dir, "y");
                if (s->support_nparam < 24) {
                    __typeof__(s->support_param[0]) *r = &s->support_param[s->support_nparam++];
                    snprintf(r->name, sizeof r->name, "direction_%s", t->direction ? "y" : "x");
                    snprintf(r->unit, sizeof r->unit, "-");
                    snprintf(r->prov, sizeof r->prov, "%s", json_get(sup, "direction") ? (prov0[0] ? prov0 : "user") : "assumed");
                    snprintf(r->source, sizeof r->source, "%s", json_get(sup, "direction") ? "given with the request" : "assumed: walls along the model's x");
                    r->value = t->direction;
                }
            }
            break;
        case SUPPORT_CONE:
            ok = sup_param(sup, "base_radius", DIM_LENGTH, "mm", TO_MM, 1.0 * MM, TUT_NONE, prov0, src0, s, &t->base_radius, out) &&
                 sup_param(sup, "top_radius", DIM_LENGTH, "mm", TO_MM, 0.45 * MM, TUT_RADIUS, prov0, src0, s, &t->top_radius, out) &&
                 sup_param(sup, "spacing", DIM_LENGTH, "mm", TO_MM, 3.0 * MM, TUT_NONE, prov0, src0, s, &t->spacing, out);
            break;
        case SUPPORT_TREE:
            ok = sup_param(sup, "trunk_radius", DIM_LENGTH, "mm", TO_MM, 1.0 * MM, TUT_NONE, prov0, src0, s, &t->trunk_radius, out) &&
                 sup_param(sup, "branch_radius", DIM_LENGTH, "mm", TO_MM, 0.45 * MM, TUT_RADIUS, prov0, src0, s, &t->branch_radius, out) &&
                 sup_param(sup, "spacing", DIM_LENGTH, "mm", TO_MM, 2.0 * MM, TUT_NONE, prov0, src0, s, &t->spacing, out) &&
                 sup_param(sup, "trunk_spacing", DIM_LENGTH, "mm", TO_MM, 6.0 * MM, TUT_NONE, prov0, src0, s, &t->trunk_spacing, out) &&
                 sup_param(sup, "branch_height", DIM_LENGTH, "mm", TO_MM, 5.0 * MM, TUT_NONE, prov0, src0, s, &t->branch_height, out);
            break;
        case SUPPORT_EXPLICIT: {
            const JsonValue *rg = json_get(sup, "region_mm");
            if (!rg || rg->type != JSON_ARRAY || rg->u.array.len != 6) {
                op_fail(out, NV_ERR_INVALID_PARAMS, "region_mm: [x0, y0, z0, x1, y1, z1] in mm, in the build frame",
                        "an explicit support names the region of the meshed body that is support");
                return false;
            }
            for (int k = 0; k < 6; k++) {
                const JsonValue *q = rg->u.array.items[k];
                if (q->type != JSON_NUMBER) {
                    op_fail(out, NV_ERR_INVALID_PARAMS, "six numbers in mm", "region_mm must hold numbers");
                    return false;
                }
                t->region[k] = 1e-3 * q->u.number;
            }
            if (!prov0[0] || !src0[0]) {
                op_fail(out, NV_ERR_PRECONDITION, "supports.provenance and supports.source",
                        "an explicit support region is an input: say where the support geometry comes from");
                return false;
            }
            break;
        }
        case SUPPORT_LATTICE:
            ok = sup_param(sup, "cell_size", DIM_LENGTH, "mm", TO_MM, 2.0 * MM, TUT_NONE, prov0, src0, s, &t->cell_size, out) &&
                 sup_param(sup, "strut_diameter", DIM_LENGTH, "mm", TO_MM, 0.5 * MM, TUT_NONE, prov0, src0, s, &t->strut_diameter, out);
            break;
        default: break;
        }
        const JsonValue *itf = json_get(sup, "interface");
        ok = ok &&
             sup_param(itf, "tooth_height", DIM_LENGTH, "mm", TO_MM, 0.0, "assumed: no tooth interface unless one is given", prov0, src0, s,
                       &t->tooth_height, out);
        if (ok && t->tooth_height > 0)
            ok = sup_param(itf, "tooth_pitch", DIM_LENGTH, "mm", TO_MM, 1.0 * MM, TUT_NONE, prov0, src0, s, &t->tooth_pitch, out) &&
                 sup_param(itf, "contact_fraction", DIM_DIMENSIONLESS, "-", 1.0, 0.5, TUT_NONE, prov0, src0, s, &t->contact_fraction, out);
        ok = ok && sup_param(sup, "relative_density", DIM_DIMENSIONLESS, "-", 1.0, 1.0,
                             "assumed: supports printed like the part are 100 % dense (tutorial p. 60)", prov0, src0, s,
                             &t->relative_density, out);
        if (!ok) return false;
        char err[256];
        if (!support_spec_check(t, err, sizeof err)) {
            op_fail(out, NV_ERR_PRECONDITION, "change the support parameters", "supports: %s", err);
            return false;
        }
    }
    if (!sup_param(sup, "offset_from_part", DIM_LENGTH, "mm", 1e3, 0.0, "assumed: no offset from the part's walls", prov0, src0, s,
                   &s->support_offset, out))
        return false;
    bool present = false;
    if (!op_quantity(out, sup, "height_above_plate", DIM_LENGTH, "mm", &s->support_height, &present)) return false;
    if (!present) s->support_height = -1;
    return true;
}

/* the supports object of both builds: its type and parameters, when they are removed, the rule and the angle */
static bool supports_block(const JsonValue *sup, bool remove_default, LpbfSettings *sp, OpResult *out) {
    LpbfSettings *s_ = sp;
    if (!lpbf_parse_supports(sup, s_, out)) return false;
    s_->supports = true;
    s_->remove_supports = json_get_bool(sup, "remove", remove_default);
    const char *rule = json_get_str(sup, "rule", !strcmp(json_get_str(sup, "channels", "excluded"), "supported") ? "overhang_all" : "overhang");
    s_->support_rule = !strcmp(rule, "every_column") ? 1 : (!strcmp(rule, "overhang_all") ? 2 : 0);
    double deg = json_get_num(sup, "critical_angle_deg", 45.0);
    s_->support_angle = deg * M_PI / 180.0;
    snprintf(s_->support_angle_source, sizeof s_->support_angle_source, "%s",
             json_get(sup, "critical_angle_deg")
                 ? json_get_str(sup, "critical_angle_source", "given with the request")
                 : "assumed: the Simufact Additive tutorial names the critical surface angle (p. 55) but gives no value; "
                   "45 degrees is the common convention");
    return true;
}

/* The settings both LPBF operations take: body, orientation, layer, strain, material, cut, numerics. The strain is
 * optional, because a calibration is what produces it. Returns false with `out` already failed. */
static bool lpbf_parse_settings(JsonValue *p, LpbfSettings *sp, bool strain_required, OpResult *out) {
    LpbfSettings s;
    memset(&s, 0, sizeof s);
    snprintf(s.body, sizeof s.body, "%s", json_get_str(p, "body", ""));
    s.orientation = !strcmp(json_get_str(p, "build_orientation", "X"), "Y");
    s.formulation = HEX8_INCOMPATIBLE, s.solver = SOLID_SOLVER_AUTO, s.pcg_tol = 1e-10;
    s.whole_part = !strcmp(json_get_str(p, "mode", "layer_by_layer"), "whole_part");
    s.strain_provenance = s.material_provenance = s.cut_provenance = PROV_COUNT;
    bool present = false;
    if (!op_quantity(out, p, "layer_thickness_sim", DIM_LENGTH, "mm", &s.layer_thickness, &present)) return false;
    if (!present && !s.whole_part) {
        op_fail(out, NV_ERR_PRECONDITION, "give layer_thickness_sim, the simulation layer of the build",
                "a layer-by-layer build needs a simulation layer thickness");
        return false;
    }
    if (s.whole_part && !present) s.layer_thickness = 1.0; /* one layer holds everything */
    /* the inherent strain: three numbers with a provenance and a source. No named strain set ships with the engine. */
    const JsonValue *st = json_get(p, "inherent_strain");
    if (st && json_get(st, "strain_set")) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "give exx, eyy and ezz with a provenance and a source",
                "no named strain set ships with this engine");
        return false;
    }
    if (st) {
        const JsonValue *xx = json_get(st, "exx"), *yy = json_get(st, "eyy"), *zz = json_get(st, "ezz");
        if (!xx || !yy || !zz) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "give exx, eyy and ezz", "the inherent strain needs all three components");
            return false;
        }
        s.eps[0] = xx->u.number, s.eps[1] = yy->u.number, s.eps[2] = zz->u.number;
        s.strain_provenance = lpbf_provenance(json_get_str(st, "provenance", ""), s.prov_text[0], sizeof s.prov_text[0]);
        snprintf(s.strain_source, sizeof s.strain_source, "%s", json_get_str(st, "source", "given with the request"));
    }
    const JsonValue *mt = json_get(p, "material");
    s.E = 70000e6, s.nu = 0.33;
    if (!op_quantity(out, mt, "youngs_modulus", DIM_PRESSURE, "MPa", &s.E, &present)) return false;
    s.nu = json_get_num(mt, "poissons_ratio", 0.33);
    s.material_provenance = lpbf_provenance(json_get_str(mt, "provenance", ""), s.prov_text[1], sizeof s.prov_text[1]);
    const JsonValue *cut = json_get(p, "cut");
    if (cut) {
        s.has_cut = true;
        if (!op_quantity(out, cut, "height", DIM_LENGTH, "mm", &s.cut_height, &present)) return false;
        if (!op_quantity(out, cut, "kerf", DIM_LENGTH, "mm", &s.cut_kerf, &present)) return false;
        if (!op_quantity(out, cut, "from_x", DIM_LENGTH, "mm", &s.cut_from_x, &present)) return false;
        s.cut_provenance = lpbf_provenance(json_get_str(cut, "provenance", ""), s.prov_text[2], sizeof s.prov_text[2]);
    }
    /* the adaptive mesh: coarse inside up to this size in x and y, fine at the surface; absent keeps the uniform mesh */
    const JsonValue *am = json_get(p, "adaptive_mesh");
    if (am && !op_quantity(out, am, "max_element_size", DIM_LENGTH, "mm", &s.max_element_size, &present)) return false;
    if (am) s.fine_band = (int)json_get_num(am, "fine_band_voxels", 2);
    /* supports are off unless asked for; every parameter is an input with a provenance (docs/contracts/supports.md) */
    const JsonValue *sup = json_get(p, "supports");
    s.support_height = -1;
    if (sup) {
        s.supports = true;
        if (!supports_block(sup, json_get(p, "cut") != NULL, &s, out)) return false;
    }
    /* plasticity is off unless a yield strength is given, and then it needs a provenance like any other material value */
    const JsonValue *pl = json_get(p, "plasticity");
    if (pl) {
        if (!op_quantity(out, pl, "yield_strength", DIM_PRESSURE, "MPa", &s.yield_stress, &present)) return false;
        if (!present || !(s.yield_stress > 0)) {
            op_fail(out, NV_ERR_PRECONDITION, "plasticity.yield_strength, for example \"292 MPa\"",
                    "plasticity was asked for without a yield strength");
            return false;
        }
        if (!op_quantity(out, pl, "hardening_modulus", DIM_PRESSURE, "MPa", &s.hardening, &present)) return false;
        s.plastic_provenance = lpbf_provenance(json_get_str(pl, "provenance", ""), s.plastic_prov_text,
                                               sizeof s.plastic_prov_text);
        snprintf(s.plastic_source, sizeof s.plastic_source, "%s", json_get_str(pl, "source", ""));
        if (s.plastic_provenance == PROV_COUNT || !s.plastic_source[0]) {
            op_fail(out, NV_ERR_PRECONDITION, "plasticity.provenance and plasticity.source",
                    "a yield strength is a material property, not a knob: say where it comes from");
            return false;
        }
        s.max_newton = (int)json_get_num(pl, "max_iterations", 80);
        s.newton_tol = json_get_num(pl, "tolerance", 1e-6);
    }
    const JsonValue *num = json_get(p, "numerics");
    const char *sv = json_get_str(num, "solver", "auto");
    s.solver = !strcmp(sv, "direct") ? SOLID_SOLVER_DIRECT : (!strcmp(sv, "iterative") ? SOLID_SOLVER_PCG : SOLID_SOLVER_AUTO);
    s.pcg_tol = json_get_num(num, "tolerance", 1e-10);
    *sp = s;
    (void)strain_required;
    return true;
}

static void op_lpbf_build_run(Engine *e, JsonValue *p, OpResult *out) {
    if (!e->proj) {
        op_fail(out, NV_ERR_PRECONDITION, "project_open or project_create first", "no project is open");
        return;
    }
    LpbfSettings s;
    if (!lpbf_parse_settings(p, &s, true, out)) return;
    JsonValue *errors = json_array(), *warnings = json_array();
    LpbfCase *lc = NULL;
    if (!lpbf_case_build(e->proj, &s, &lc, errors, warnings)) {
        const JsonValue *first = json_at(errors, 0);
        op_fail(out, nv_err_from_name(json_get_str(first, "code", "PRECONDITION_FAILED")), json_get_str(first, "hint", NULL), "the build cannot run: %s",
                json_get_str(first, "message", ""));
        op_fail_detail(out, "errors", errors);
        op_fail_detail(out, "warnings", warnings);
        return;
    }
    JsonValue *model = lpbf_model_json(lc);
    JsonValue *spec = json_object();
    json_set_string(spec, "analysis", "lpbf_build");
    json_set_string(spec, "project", e->proj->name);
    json_set(spec, "model", json_clone(model));
    json_set_string(spec, "mesh_hash", lc->c.mesh_hash);
    char hash[65];
    hash_json(spec, hash);
    char active[64];
    if (!json_get_bool(p, "allow_duplicate", false) && jobs_find_active(e->jobs, hash, active, sizeof active)) {
        JsonValue *v = json_object();
        json_set_string(v, "job_id", active);
        json_set_bool(v, "deduplicated", true);
        json_set_string(v, "spec_hash", hash);
        json_set_string(v, "message", "an identical build is already queued or running; its job is returned instead of starting a second one");
        thermal_case_free(lc);
        json_free(spec), json_free(model), json_free(errors), json_free(warnings);
        op_succeed(out, v);
        return;
    }
    char id[64], dir[NV_PATH_MAX], path[NV_PATH_MAX], now[32], err[512];
    if (!make_run_dir(e, out, id, sizeof id, dir, sizeof dir)) {
        thermal_case_free(lc);
        json_free(spec), json_free(model), json_free(errors), json_free(warnings);
        return;
    }
    iso_time_now(now, sizeof now);
    JsonValue *stored = json_clone(spec);
    json_set_string(stored, "job_id", id);
    json_set_string(stored, "created", now);
    json_set_int(stored, "project_revision", (long long)e->proj->revision);
    json_set_string(stored, "spec_hash", hash);
    json_set_string(stored, "spec_hash_covers", "every field except job_id, created, project_revision and spec_hash");
    path_join(path, sizeof path, dir, "spec.json");
    bool written = json_write_file(path, stored, JSON_PRETTY | JSON_SORTED);
    json_free(stored);
    if (!written) {
        thermal_case_free(lc);
        json_free(spec), json_free(model), json_free(errors), json_free(warnings);
        op_fail(out, NV_ERR_IO, NULL, "cannot write %s", path);
        return;
    }
    snprintf(lc->c.run_dir, sizeof lc->c.run_dir, "%s", dir);
    snprintf(lc->c.job_id, sizeof lc->c.job_id, "%s", id);
    JobSpec js = {id, "lpbf_build", json_get_str(p, "label", ""), dir, hash, e->proj->id, e->proj->name, e->proj->revision, lc, lpbf_job_run, thermal_case_free};
    if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
        json_free(spec), json_free(model), json_free(errors), json_free(warnings);
        op_fail(out, NV_ERR_BUSY, "check job_list and retry when jobs have finished", "%s", err);
        return;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "state", "queued");
    json_set_string(v, "analysis", "lpbf_build");
    json_set_string(v, "run_directory", dir);
    json_set_string(v, "spec_file", path);
    json_set_string(v, "spec_hash", hash);
    json_set(v, "model", model);
    json_set(v, "warnings", warnings);
    json_set_string(v, "scope", "inherent strain is a calibrated input; the summary's scope block states what the number is and is not");
    json_set_string(v, "next_step",
                    "poll job_status with {\"job_id\": \"...\", \"wait_seconds\": 30}; the summary carries tip_uz_before_cut_mm, "
                    "tip_uz_after_cut_mm and springback_mm, and results_query reads every stored time (displacement, von_mises)");
    json_free(spec), json_free(errors);
    op_succeed(out, v);
}

/* lpbf_supports_generate: the supports a build would get, without building. The same settings as lpbf_build_run
 * (the strain may be left out); the case is assembled, its supports generated and homogenised, and described. */
static void op_lpbf_supports_generate(Engine *e, JsonValue *p, OpResult *out) {
    if (!e->proj) {
        op_fail(out, NV_ERR_PRECONDITION, "project_open or project_create first", "no project is open");
        return;
    }
    if (!json_get(p, "supports")) {
        op_fail(out, NV_ERR_PRECONDITION, "give supports: {\"type\": \"block\", ...}", "there is nothing to generate without a supports object");
        return;
    }
    LpbfSettings s;
    if (!lpbf_parse_settings(p, &s, false, out)) return;
    if (s.strain_provenance == PROV_COUNT) { /* the strain plays no part in generating supports */
        s.strain_provenance = PROV_INFERRED;
        snprintf(s.prov_text[0], sizeof s.prov_text[0], "not used");
    }
    JsonValue *errors = json_array(), *warnings = json_array();
    LpbfCase *lc = NULL;
    if (!lpbf_case_build(e->proj, &s, &lc, errors, warnings)) {
        const JsonValue *first = json_at(errors, 0);
        op_fail(out, nv_err_from_name(json_get_str(first, "code", "PRECONDITION_FAILED")), json_get_str(first, "hint", NULL),
                "the supports cannot be generated: %s", json_get_str(first, "message", ""));
        op_fail_detail(out, "errors", errors);
        op_fail_detail(out, "warnings", warnings);
        return;
    }
    JsonValue *v = json_object();
    json_set_int(v, "support_elements", lc->support_elements);
    json_set_int(v, "part_elements", lc->part_elements);
    json_set(v, "supports", lpbf_supports_json(lc));
    json_set(v, "warnings", warnings);
    json_set_string(v, "next_step", "lpbf_build_run with the same supports object builds with exactly these supports");
    thermal_case_free(lc);
    json_free(errors);
    op_succeed(out, v);
}

static void op_lpbf_calibrate(Engine *e, JsonValue *p, OpResult *out) {
    if (!e->proj) {
        op_fail(out, NV_ERR_PRECONDITION, "project_open or project_create first", "no project is open");
        return;
    }
    LpbfSettings s;
    if (!lpbf_parse_settings(p, &s, false, out)) return;
    if (s.whole_part) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "leave mode at layer_by_layer",
                "a whole-part model has no build history, so a strain fitted with it is not an inherent strain");
        return;
    }
    LpbfCalibration *cal = calloc(1, sizeof *cal);
    if (!cal) {
        op_fail(out, NV_ERR_INTERNAL, NULL, "out of memory");
        return;
    }
    snprintf(cal->strategy, sizeof cal->strategy, "%s", json_get_str(p, "strategy", "unnamed"));
    const JsonValue *m = json_get(p, "measured");
    bool present = false;
    if (!op_quantity(out, m, "tip_uz_after_cut_x", DIM_LENGTH, "mm", &cal->target[0], &present) || !present) {
        if (present || out->ok) op_fail(out, NV_ERR_PRECONDITION, "measured.tip_uz_after_cut_x, in mm",
                                        "the fit needs the measured deflection in the X build orientation");
        free(cal);
        return;
    }
    if (!op_quantity(out, m, "tip_uz_after_cut_y", DIM_LENGTH, "mm", &cal->target[1], &present) || !present) {
        if (present || out->ok) op_fail(out, NV_ERR_PRECONDITION, "measured.tip_uz_after_cut_y, in mm",
                                        "the fit needs the measured deflection in the Y build orientation");
        free(cal);
        return;
    }
    snprintf(cal->target_source, sizeof cal->target_source, "%s", json_get_str(m, "source", ""));
    cal->target_prov = lpbf_provenance(json_get_str(m, "provenance", ""), cal->target_prov_text,
                                       sizeof cal->target_prov_text);
    if (cal->target_prov == PROV_COUNT || !cal->target_source[0]) {
        op_fail(out, NV_ERR_PRECONDITION, "measured.provenance (\"measured\") and measured.source",
                "a fit is only as good as the numbers it is fitted to: say where they come from");
        free(cal);
        return;
    }
    const JsonValue *fit = json_get(p, "fit");
    cal->ezz = json_get_num(fit, "ezz", -0.03);
    cal->tol = json_get_num(fit, "tolerance_pct", 1.0) / 100.0;
    cal->max_builds = (int)json_get_num(fit, "max_builds", 12);
    cal->lo[0] = cal->lo[1] = json_get_num(fit, "lower_bound", -0.05);
    cal->hi[0] = cal->hi[1] = json_get_num(fit, "upper_bound", 0.02);
    cal->start[0] = s.eps[0], cal->start[1] = s.eps[1];
    snprintf(cal->start_source, sizeof cal->start_source, "%s",
             s.strain_source[0] ? s.strain_source : "default start, no strain given with the request");
    if (cal->start[0] == 0 && cal->start[1] == 0) cal->start[0] = -0.002, cal->start[1] = -0.004;
    if (cal->max_builds < 4 || cal->max_builds > 60) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "fit.max_builds between 4 and 60",
                "the fit needs at least four builds and this machine will not sit through sixty");
        free(cal);
        return;
    }
    /* the case is built once and re-run; the start values only have to pass the builder's provenance check */
    s.eps[0] = cal->start[0], s.eps[1] = cal->start[1], s.eps[2] = cal->ezz;
    s.strain_provenance = PROV_CALIBRATED;
    snprintf(s.prov_text[0], sizeof s.prov_text[0], "calibrated");
    snprintf(s.strain_source, sizeof s.strain_source, "the fit's starting point");
    JsonValue *errors = json_array(), *warnings = json_array();
    LpbfCase *lc = NULL;
    if (!lpbf_case_build(e->proj, &s, &lc, errors, warnings)) {
        const JsonValue *first = json_at(errors, 0);
        op_fail(out, nv_err_from_name(json_get_str(first, "code", "PRECONDITION_FAILED")), json_get_str(first, "hint", NULL),
                "the calibration cannot run: %s", json_get_str(first, "message", ""));
        op_fail_detail(out, "errors", errors);
        op_fail_detail(out, "warnings", warnings);
        free(cal);
        return;
    }
    cal->c = lc;
    JsonValue *spec = json_object();
    json_set_string(spec, "analysis", "lpbf_calibrate");
    json_set_string(spec, "project", e->proj->name);
    json_set_string(spec, "strategy", cal->strategy);
    json_set(spec, "model", lpbf_model_json(lc));
    json_set_number(spec, "target_x_mm", 1e3 * cal->target[0]);
    json_set_number(spec, "target_y_mm", 1e3 * cal->target[1]);
    json_set_number(spec, "tolerance_pct", 100.0 * cal->tol);
    json_set_string(spec, "mesh_hash", lc->c.mesh_hash);
    char hash[65];
    hash_json(spec, hash);
    char active[64];
    if (!json_get_bool(p, "allow_duplicate", false) && jobs_find_active(e->jobs, hash, active, sizeof active)) {
        JsonValue *v = json_object();
        json_set_string(v, "job_id", active);
        json_set_bool(v, "deduplicated", true);
        json_set_string(v, "message", "an identical calibration is already queued or running");
        lpbf_calibration_free(cal);
        json_free(spec), json_free(errors), json_free(warnings);
        op_succeed(out, v);
        return;
    }
    char id[64], dir[NV_PATH_MAX], path[NV_PATH_MAX], now[32], err[512];
    if (!make_run_dir(e, out, id, sizeof id, dir, sizeof dir)) {
        lpbf_calibration_free(cal);
        json_free(spec), json_free(errors), json_free(warnings);
        return;
    }
    iso_time_now(now, sizeof now);
    JsonValue *stored = json_clone(spec);
    json_set_string(stored, "job_id", id);
    json_set_string(stored, "created", now);
    json_set_string(stored, "spec_hash", hash);
    path_join(path, sizeof path, dir, "spec.json");
    bool written = json_write_file(path, stored, JSON_PRETTY | JSON_SORTED);
    json_free(stored);
    if (!written) {
        lpbf_calibration_free(cal);
        json_free(spec), json_free(errors), json_free(warnings);
        op_fail(out, NV_ERR_IO, NULL, "cannot write %s", path);
        return;
    }
    snprintf(lc->c.run_dir, sizeof lc->c.run_dir, "%s", dir);
    snprintf(lc->c.job_id, sizeof lc->c.job_id, "%s", id);
    JobSpec js = {id, "lpbf_calibrate", json_get_str(p, "label", ""), dir, hash, e->proj->id, e->proj->name,
                  e->proj->revision, cal, lpbf_calibrate_job_run, lpbf_calibration_free};
    if (!jobs_submit(e->jobs, &js, err, sizeof err)) {
        json_free(spec), json_free(errors), json_free(warnings);
        op_fail(out, NV_ERR_BUSY, "check job_list and retry when jobs have finished", "%s", err);
        return;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "state", "queued");
    json_set_string(v, "analysis", "lpbf_calibrate");
    json_set_string(v, "run_directory", dir);
    json_set_string(v, "spec_hash", hash);
    json_set_int(v, "builds_at_most", cal->max_builds);
    json_set(v, "warnings", warnings);
    json_set_string(v, "scope", "the fit matches tip deflection only, at this element size and layer thickness, and "
                                "absorbs every error of the model it was fitted with");
    json_set_string(v, "next_step", "poll job_status; the summary carries inherent_strain (exx, eyy, ezz), the achieved "
                                    "deflections with their errors, and the whole iteration history");
    json_free(spec), json_free(errors);
    op_succeed(out, v);
}

const OpBinding OPS_MECH_BINDINGS[] = {
    {"mech_assembly_import", op_mech_assembly_import},
    {"mech_body_define", op_mech_body_define},
    {"mech_joint_define", op_mech_joint_define},
    {"mech_environment_define", op_mech_environment_define},
    {"mech_material_define", op_mech_material_define},
    {"mech_structure_query", op_mech_structure_query},
    {"mech_modal_run", op_mech_modal_run},
    {"mech_transient_assess", op_mech_transient_assess},
    {"mech_vibration_query", op_mech_vibration_query},
    {"mech_flexible_reduce", op_mech_flexible_reduce},
    {"mech_flexible_attach", op_mech_flexible_attach},
    {"mech_flexible_detach", op_mech_flexible_detach},
    {"mech_flexible_stress", op_mech_flexible_stress},
    {"mech_component_define", op_mech_component_define},
    {"mech_component_remove", op_mech_component_remove},
    {"mech_study_settings", op_mech_study_settings},
    {"mech_assembly_inspect", op_mech_assembly_inspect},
    {"mech_study_validate", op_mech_study_validate},
    {"mech_dynamics_run", op_mech_dynamics_run},
    {"mech_results_query", op_mech_results_query},
    {"mech_fem_assess", op_mech_fem_assess},
    {"mech_print_run", op_mech_print_run},
    {"lpbf_build_run", op_lpbf_build_run},
    {"lpbf_supports_generate", op_lpbf_supports_generate},
    {"lpbf_calibrate", op_lpbf_calibrate},
    {NULL, NULL},
};
