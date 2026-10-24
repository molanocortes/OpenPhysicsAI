/* ops_setup.c - materials (library, project definitions, assignments) and boundary conditions */
#include "matlib.h"
#include "ops_internal.h"
#include "selection.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- materials ---------------------------------------------------------------------------------- */

static bool has_process(const JsonValue *rec, const char *process) {
    const JsonValue *pr = json_get(rec, "processes");
    for (size_t i = 0; i < json_len(pr); i++)
        if (json_str(json_at(pr, i)) && !strcmp(json_str(json_at(pr, i)), process)) return true;
    return false;
}

static JsonValue *material_ids(const JsonValue *user) {
    JsonValue *ids = json_array();
    const JsonValue *mats = json_get(matlib_builtin(NULL, 0), "materials");
    for (size_t i = 0; i < json_len(user); i++) json_push(ids, json_string(json_get_str(json_at(user, i), "id", "")));
    for (size_t i = 0; i < json_len(mats); i++) json_push(ids, json_string(json_get_str(json_at(mats, i), "id", "")));
    return ids;
}

static void op_materials_list(Engine *e, JsonValue *p, OpResult *out) {
    char err[300];
    const JsonValue *lib = matlib_builtin(err, sizeof err);
    if (!lib) {
        op_fail(out, NV_ERR_INTERNAL, NULL, "material library unavailable: %s", err);
        return;
    }
    const JsonValue *user = e->proj ? e->proj->user_materials : NULL;
    const char *id = json_get_str(p, "id", NULL), *family = json_get_str(p, "family", "any"), *process = json_get_str(p, "process", "any");
    JsonValue *v = json_object();
    if (id) {
        MaterialRecord m;
        if (!material_lookup(user, id, &m, err, sizeof err)) {
            json_free(v);
            op_fail(out, NV_ERR_NOT_FOUND, "call materials_list without id to see every id", "%s", err);
            op_fail_detail(out, "available_ids", material_ids(user));
            return;
        }
        json_set(v, "material", material_summary_json(&m));
        json_set(v, "record", json_clone(m.json));
        op_succeed(out, v);
        return;
    }
    JsonValue *arr = json_set_array(v, "materials");
    for (int pass = 0; pass < 2; pass++) {
        const JsonValue *list = pass == 0 ? user : json_get(lib, "materials");
        for (size_t i = 0; i < json_len(list); i++) {
            const JsonValue *rec = json_at(list, i);
            if (strcmp(family, "any") && strcmp(family, json_get_str(rec, "family", ""))) continue;
            if (strcmp(process, "any") && !has_process(rec, process)) continue;
            MaterialRecord m;
            if (material_from_json(rec, pass == 1, &m, err, sizeof err)) json_push(arr, material_summary_json(&m));
        }
    }
    json_set_string(v, "note",
                    "A library entry is either demonstration data (status demonstration: for running workflows, not for design) or labelled "
                    "measured or published, when every one of its values names its source; material_list says what each record lacks.");
    op_succeed(out, v);
}

/* what an analysis needs of a record, in the words the interface and an agent use */
static const struct { const char *key, *words; } NEEDS_STRESS[] = {{"youngs_modulus_pa", "Young's modulus"}, {"poisson_ratio", "Poisson's ratio"}};
static const struct { const char *key, *words; } NEEDS_HEAT[] = {
    {"density_kg_m3", "density"}, {"conductivity_w_per_mk", "conductivity"}, {"specific_heat_j_per_kgk", "specific heat"}};

static const char *process_group(const JsonValue *rec) {
    const char *fam = json_get_str(rec, "family", "");
    if (!strcmp(fam, "metal") && has_process(rec, "lpbf")) return "metal powder bed (LPBF)";
    if (!strcmp(fam, "polymer") && has_process(rec, "fff")) return "plastic filament (FFF)";
    if (!strcmp(fam, "fluid")) return "fluid";
    return "other";
}

/* the record's first sentence: which documents its values come from */
static JsonValue *source_line(const JsonValue *rec) {
    const char *prov = json_get_str(rec, "provenance", "");
    const char *end = strstr(prov, ". ");
    size_t n = end ? (size_t)(end - prov) + 1 : strlen(prov);
    return json_stringn(prov, n);
}

static JsonValue *material_entry(const JsonValue *rec, bool builtin) {
    JsonValue *o = json_object();
    json_set_string(o, "id", json_get_str(rec, "id", ""));
    json_set_string(o, "name", json_get_str(rec, "name", ""));
    json_set_string(o, "status", json_get_str(rec, "status", ""));
    json_set_string(o, "family", json_get_str(rec, "family", ""));
    json_set_string(o, "group", process_group(rec));
    const JsonValue *pr = json_get(rec, "processes");
    json_set(o, "processes", pr ? json_clone(pr) : json_array());
    json_set_string(o, "defined_in", builtin ? "library" : "project");
    JsonValue *props = json_set_array(o, "properties");
    for (size_t i = 0; i < json_len(rec); i++) {
        const JsonValue *val = json_value_at(rec, i);
        const char *k = json_key_at(rec, i);
        if (val && val->type == JSON_OBJECT && strcmp(k, "powder") && strcmp(k, "calibration")) json_push(props, json_string(k));
    }
    JsonValue *lacks = json_set_object(o, "lacks");
    JsonValue *ls = json_set_array(lacks, "stress"), *lh = json_set_array(lacks, "heat"), *lt = json_set_array(lacks, "thermal_stress");
    for (size_t i = 0; i < sizeof NEEDS_STRESS / sizeof NEEDS_STRESS[0]; i++)
        if (!json_get(rec, NEEDS_STRESS[i].key)) json_push(ls, json_string(NEEDS_STRESS[i].words)), json_push(lt, json_string(NEEDS_STRESS[i].words));
    for (size_t i = 0; i < sizeof NEEDS_HEAT / sizeof NEEDS_HEAT[0]; i++)
        if (!json_get(rec, NEEDS_HEAT[i].key)) json_push(lh, json_string(NEEDS_HEAT[i].words)), json_push(lt, json_string(NEEDS_HEAT[i].words));
    if (!json_get(rec, "expansion_1_per_k")) json_push(lt, json_string("expansion"));
    json_set(o, "source_line", source_line(rec));
    return o;
}

/* Every record at a glance, for an agent and for the interface's pickers: status, process group, the properties it
 * carries, what it lacks for a stress, a heat and a thermal-stress analysis, and the first line of where its numbers
 * come from. materials_list gives one record in full. */
static void op_material_list(Engine *e, JsonValue *p, OpResult *out) {
    char err[300];
    const JsonValue *lib = matlib_builtin(err, sizeof err);
    if (!lib) {
        op_fail(out, NV_ERR_INTERNAL, NULL, "material library unavailable: %s", err);
        return;
    }
    const JsonValue *user = e->proj ? e->proj->user_materials : NULL;
    const char *process = json_get_str(p, "process", "any");
    JsonValue *v = json_object();
    JsonValue *arr = json_set_array(v, "materials");
    int total = 0, sourced = 0, demo = 0;
    for (int pass = 0; pass < 2; pass++) {
        const JsonValue *list = pass == 0 ? user : json_get(lib, "materials");
        for (size_t i = 0; i < json_len(list); i++) {
            const JsonValue *rec = json_at(list, i);
            if (strcmp(process, "any") && !has_process(rec, process)) continue;
            const char *st = json_get_str(rec, "status", "");
            total++;
            sourced += !strcmp(st, "measured") || !strcmp(st, "published");
            demo += !strcmp(st, "demonstration");
            json_push(arr, material_entry(rec, pass == 1));
        }
    }
    JsonValue *c = json_set_object(v, "counts");
    json_set_int(c, "materials", total);
    json_set_int(c, "with_a_source_on_every_value", sourced);
    json_set_int(c, "demonstration", demo);
    json_set_string(v, "next_step", "materials_list with id gives one record in full, each value with its source");
    op_succeed(out, v);
}

static void op_material_define(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    const JsonValue *rec = json_get(p, "material");
    MaterialRecord m;
    char err[300];
    if (!material_from_json(rec, false, &m, err, sizeof err)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "values are SI in the unit named by each key; temperature tables need increasing t_c in degC", "%s", err);
        return;
    }
    if (matlib_is_builtin_id(m.id)) {
        op_fail(out, NV_ERR_ALREADY_EXISTS, "choose another id; library entries cannot be redefined", "'%s' is a library material", m.id);
        return;
    }
    Project *proj = e->proj;
    if (!proj->user_materials) proj->user_materials = json_array();
    JsonValue *list = proj->user_materials;
    int idx = -1;
    for (size_t i = 0; i < json_len(list); i++)
        if (!strcmp(json_get_str(json_at(list, i), "id", ""), m.id)) idx = (int)i;
    if (idx >= 0 && !json_get_bool(p, "replace", false)) {
        op_fail(out, NV_ERR_ALREADY_EXISTS, "pass replace: true to redefine it", "the project already defines material '%s'", m.id);
        return;
    }
    JsonValue *copy = json_clone(rec);
    if (!copy) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    if (idx >= 0) {
        json_free(list->u.array.items[idx]);
        list->u.array.items[idx] = copy;
    } else {
        json_push(list, copy);
    }
    material_from_json(copy, false, &m, err, sizeof err); /* refer to the project-owned copy */
    JsonValue *users = json_array();
    for (int i = 0; i < proj->nmaterials; i++)
        if (!strcmp(proj->materials[i].material, m.id)) json_push(users, json_string(proj->materials[i].target));
    engine_touch(e, "material_define", "%s material %s (%s)", idx >= 0 ? "redefined" : "defined", m.id, m.status);
    JsonValue *v = json_object();
    json_set(v, "material", material_summary_json(&m));
    JsonValue *w = json_set_array(v, "warnings");
    if (idx >= 0 && json_len(users)) json_push(w, json_string("bodies already use this material; earlier analysis results do not reflect the new values"));
    if (!m.prop[MATP_E].n || !m.prop[MATP_NU].n)
        json_push(w, json_string("without youngs_modulus_pa and poisson_ratio the material cannot be used in structural analyses"));
    json_set(v, "assigned_to", users);
    op_succeed(out, v);
}

static void op_material_assign(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    const char *target = json_get_str(p, "body", "");
    if (strcmp(target, "build_plate") != 0 && !project_body(proj, target)) {
        op_fail(out, NV_ERR_NOT_FOUND, "use a body name from project_inspect, or build_plate", "no body named '%s'", target);
        return;
    }
    const char *id = json_get_str(p, "material", "");
    MaterialRecord m;
    char err[300];
    if (!material_lookup(proj->user_materials, id, &m, err, sizeof err)) {
        op_fail(out, NV_ERR_NOT_FOUND, "materials_list shows the available ids; define new ones with material_define", "%s", err);
        op_fail_detail(out, "available_ids", material_ids(proj->user_materials));
        return;
    }
    Provenance src = (Provenance)provenance_from_name(json_get_str(p, "source", "user"));
    const char *note = json_get_str(p, "note", "");
    MaterialAssignment *ma = project_material_for(proj, target);
    if (!ma) {
        if (proj->nmaterials == proj->cap_materials) {
            int cap = proj->cap_materials ? 2 * proj->cap_materials : 8;
            MaterialAssignment *nm = realloc(proj->materials, (size_t)cap * sizeof *nm);
            if (!nm) {
                op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
                return;
            }
            proj->materials = nm;
            proj->cap_materials = cap;
        }
        ma = &proj->materials[proj->nmaterials++];
        memset(ma, 0, sizeof *ma);
        snprintf(ma->target, sizeof ma->target, "%s", target);
    }
    snprintf(ma->material, sizeof ma->material, "%s", m.id);
    ma->source = src;
    snprintf(ma->note, sizeof ma->note, "%s", note);
    char subj[96];
    snprintf(subj, sizeof subj, "materials/%s", target);
    if (src != PROV_USER) project_note_assumption(proj, subj, src, "material '%s' is used for %s%s%s", m.id, target, note[0] ? ": " : "", note);
    else project_clear_assumptions(proj, subj);
    engine_touch(e, "material_assign", "%s uses %s (%s)", target, m.id, provenance_name(src));
    JsonValue *v = json_object();
    JsonValue *a = json_set_object(v, "assignment");
    json_set_string(a, "body", target);
    json_set_string(a, "material", m.id);
    json_set_string(a, "source", provenance_name(src));
    if (note[0]) json_set_string(a, "note", note);
    json_set(v, "material", material_summary_json(&m));
    JsonValue *w = json_set_array(v, "warnings");
    if (!strcmp(m.status, "demonstration"))
        json_push(w, json_string("demonstration material: its values are not calibrated, so analysis magnitudes are indicative only"));
    if (src != PROV_USER) json_push(w, json_string("the material choice is recorded as an assumption; confirm the grade with the user"));
    op_succeed(out, v);
}

/* ---- boundary conditions ------------------------------------------------------------------------ */

static int dominant_axis(const double n[3]) {
    int ax = 0;
    for (int k = 1; k < 3; k++)
        if (fabs(n[k]) > fabs(n[ax])) ax = k;
    return ax;
}

static void prescribed(const BoundaryCondition *bc, const Selection *sel, bool comp[3], double val[3]) {
    for (int k = 0; k < 3; k++) comp[k] = false, val[k] = 0;
    if (bc->kind == BC_FIXED) comp[0] = comp[1] = comp[2] = true;
    else if (bc->kind == BC_DISPLACEMENT)
        for (int k = 0; k < 3; k++) comp[k] = bc->component[k], val[k] = bc->component[k] ? bc->vec[k] : 0;
    else if (bc->kind == BC_FRICTIONLESS && sel) comp[dominant_axis(sel->normal)] = true;
}

static int shared_tris(const Selection *a, const Selection *b) {
    if (strcmp(a->body, b->body) != 0) return 0;
    int i = 0, j = 0, n = 0;
    while (i < a->ntri && j < b->ntri) {
        if (a->tris[i] < b->tris[j]) i++;
        else if (a->tris[i] > b->tris[j]) j++;
        else n++, i++, j++;
    }
    return n;
}

static bool vec3_quantity(OpResult *out, const JsonValue *arr, Dimension dim, const char *unit, const char *what, double v[3]) {
    if (!arr || arr->type != JSON_ARRAY || json_len(arr) != 3) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "%s must be [x, y, z]", what);
        return false;
    }
    for (int k = 0; k < 3; k++) {
        char err[256];
        if (!quantity_from_json(json_at(arr, (size_t)k), dim, unit, &v[k], err, sizeof err)) {
            op_fail(out, NV_ERR_INVALID_UNIT, NULL, "%s[%d]: %s", what, k, err);
            return false;
        }
    }
    return true;
}

static JsonValue *bc_details(Project *proj, const BoundaryCondition *bc) {
    JsonValue *o = boundary_condition_json(bc);
    json_set_int(o, "applied_at_revision", (long long)bc->revision);
    if (!bc->selection[0]) return o;
    Selection *sel = project_selection(proj, bc->selection);
    const char *state = !sel ? "missing" : (sel->stale ? "stale" : (strcmp(sel->set_hash, bc->selection_hash) ? "selection_changed" : "current"));
    json_set_string(o, "selection_state", state);
    if (!sel) return o;
    json_set_string(o, "body", sel->body);
    json_set_number(o, "selection_area_mm2", sel->area * 1e6);
    if (bc->kind == BC_FORCE && sel->area > 0)
        json_set_number(o, "mean_traction_mpa", 1e-6 * sqrt(bc->vec[0] * bc->vec[0] + bc->vec[1] * bc->vec[1] + bc->vec[2] * bc->vec[2]) / sel->area);
    Body *b = project_body(proj, sel->body);
    if (bc->kind == BC_PRESSURE && b) {
        double r[3] = {0, 0, 0};
        for (int i = 0; i < sel->ntri; i++)
            for (int k = 0; k < 3; k++) r[k] -= bc->magnitude * b->build_normal[3 * (size_t)sel->tris[i] + k] * b->build_area[sel->tris[i]];
        json_set(o, "resultant_n", json_vec3(r[0], r[1], r[2]));
    }
    if (bc->kind == BC_TRACTION) json_set(o, "resultant_n", json_vec3(bc->vec[0] * sel->area, bc->vec[1] * sel->area, bc->vec[2] * sel->area));
    if (proj->mesh.valid) {
        double ma = 0;
        int nf = selection_mesh_faces(proj, sel, NULL, 0, &ma);
        json_set_int(o, "mesh_faces", nf);
        json_set_number(o, "mesh_area_mm2", ma * 1e6);
    }
    return o;
}

static void op_boundary_apply(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    const char *name = json_get_str(p, "name", "");
    if (!body_name_valid(name)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "use 1-63 letters, digits, '_', '-' or '.'", "invalid boundary condition name '%s'", name);
        return;
    }
    BoundaryCondition *existing = project_bc(proj, name);
    if (existing && !json_get_bool(p, "replace", false)) {
        op_fail(out, NV_ERR_ALREADY_EXISTS, "pass replace: true to redefine it, or choose another name", "a boundary condition named '%s' already exists", name);
        return;
    }
    BoundaryCondition bc;
    memset(&bc, 0, sizeof bc);
    snprintf(bc.name, sizeof bc.name, "%s", name);
    int kind = bc_kind_from_name(json_get_str(p, "kind", ""));
    if (kind < 0) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "unknown kind");
        return;
    }
    bc.kind = (BcKind)kind;
    static const char *const VALUE_KEYS[] = {"displacement", "force",       "pressure",  "traction",   "acceleration",
                                             "temperature",  "heat_flux",   "convection", "radiation", "power_density"};
    const char *expected = bc.kind == BC_DISPLACEMENT ? "displacement"
                           : bc.kind == BC_FORCE       ? "force"
                           : bc.kind == BC_PRESSURE    ? "pressure"
                           : bc.kind == BC_TRACTION    ? "traction"
                           : bc.kind == BC_GRAVITY     ? "acceleration"
                           : bc.kind == BC_TEMPERATURE ? "temperature"
                           : bc.kind == BC_HEAT_FLUX   ? "heat_flux"
                           : bc.kind == BC_CONVECTION  ? "convection"
                           : bc.kind == BC_RADIATION   ? "radiation"
                           : bc.kind == BC_HEAT_SOURCE ? "power_density"
                                                       : NULL;
    for (int i = 0; i < (int)(sizeof VALUE_KEYS / sizeof VALUE_KEYS[0]); i++)
        if (json_get(p, VALUE_KEYS[i]) && (!expected || strcmp(VALUE_KEYS[i], expected))) {
            op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "'%s' does not apply to a %s condition", VALUE_KEYS[i], bc_kind_name(bc.kind));
            return;
        }
    if (expected && !json_get(p, expected)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "use the value the user stated; if none was stated, ask instead of assuming one", "a %s condition needs '%s'",
                bc_kind_name(bc.kind), expected);
        return;
    }
    const char *selname = json_get_str(p, "selection", NULL), *bodyname = json_get_str(p, "body", NULL);
    Selection *sel = NULL;
    if (bc.kind == BC_GRAVITY) {
        if (selname) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "omit selection", "gravity acts on every meshed body");
            return;
        }
    } else if (bc.kind == BC_HEAT_SOURCE) {
        if (selname) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "use body instead of selection", "a volumetric heat source acts on a body, not on a surface");
            return;
        }
        if (!bodyname || !project_body(proj, bodyname)) {
            op_fail(out, NV_ERR_NOT_FOUND, "name a body from project_inspect", "a heat source needs an existing body ('%s')", bodyname ? bodyname : "");
            return;
        }
        snprintf(bc.body, sizeof bc.body, "%s", bodyname);
    } else {
        if (bodyname) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "use selection for surface conditions", "'%s' applies to a selection, not to a whole body", bc_kind_name(bc.kind));
            return;
        }
        if (!selname) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "create one with selection_create", "a %s condition needs a selection", bc_kind_name(bc.kind));
            return;
        }
        sel = project_selection(proj, selname);
        if (!sel) {
            op_fail(out, NV_ERR_NOT_FOUND, "list selections with selection_list", "no selection named '%s'", selname);
            return;
        }
        if (sel->stale) {
            op_fail(out, NV_ERR_STALE_REFERENCE, "check it against the changed geometry and redefine it with selection_create (replace: true)", "selection '%s' is stale: %s",
                    selname, sel->stale_reason);
            return;
        }
        snprintf(bc.selection, sizeof bc.selection, "%s", selname);
        snprintf(bc.selection_hash, sizeof bc.selection_hash, "%s", sel->set_hash);
    }
    char err[256];
    switch (bc.kind) {
    case BC_DISPLACEMENT: {
        const JsonValue *d = json_get(p, "displacement");
        int any = 0;
        for (int k = 0; k < 3; k++) {
            const JsonValue *c = json_get(d, k == 0 ? "x" : (k == 1 ? "y" : "z"));
            if (!c || c->type == JSON_NULL) continue;
            if (!quantity_from_json(c, DIM_LENGTH, "mm", &bc.vec[k], err, sizeof err)) {
                op_fail(out, NV_ERR_INVALID_UNIT, NULL, "displacement.%c: %s", "xyz"[k], err);
                return;
            }
            bc.component[k] = true;
            any++;
        }
        if (!any) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "give at least one of x, y, z; null or an omitted component stays free", "the displacement prescribes no component");
            return;
        }
        break;
    }
    case BC_FORCE:
        if (!vec3_quantity(out, json_get(p, "force"), DIM_FORCE, "N", "force", bc.vec)) return;
        break;
    case BC_TRACTION:
        if (!vec3_quantity(out, json_get(p, "traction"), DIM_PRESSURE, "MPa", "traction", bc.vec)) return;
        break;
    case BC_PRESSURE:
        if (!quantity_from_json(json_get(p, "pressure"), DIM_PRESSURE, "MPa", &bc.magnitude, err, sizeof err)) {
            op_fail(out, NV_ERR_INVALID_UNIT, NULL, "pressure: %s", err);
            return;
        }
        break;
    case BC_GRAVITY:
        if (!vec3_quantity(out, json_get(p, "acceleration"), DIM_ACCELERATION, "m/s^2", "acceleration", bc.vec)) return;
        break;
    case BC_TEMPERATURE:
        if (!quantity_from_json(json_get(p, "temperature"), DIM_TEMPERATURE, "degC", &bc.magnitude, err, sizeof err)) {
            op_fail(out, NV_ERR_INVALID_UNIT, NULL, "temperature: %s", err);
            return;
        }
        break;
    case BC_HEAT_FLUX:
        if (!quantity_from_json(json_get(p, "heat_flux"), DIM_HEAT_FLUX, "W/m^2", &bc.magnitude, err, sizeof err)) {
            op_fail(out, NV_ERR_INVALID_UNIT, NULL, "heat_flux: %s", err);
            return;
        }
        break;
    case BC_CONVECTION:
        if (!quantity_from_json(json_get(json_get(p, "convection"), "coefficient"), DIM_HEAT_TRANSFER_COEFFICIENT, "W/(m^2*K)", &bc.magnitude, err, sizeof err) ||
            !quantity_from_json(json_get(json_get(p, "convection"), "ambient"), DIM_TEMPERATURE, "degC", &bc.ambient, err, sizeof err)) {
            op_fail(out, NV_ERR_INVALID_UNIT, NULL, "convection: %s", err);
            return;
        }
        if (!(bc.magnitude >= 0)) {
            op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "the convection coefficient must not be negative");
            return;
        }
        break;
    case BC_RADIATION:
        bc.magnitude = json_get_num(json_get(p, "radiation"), "emissivity", -1);
        if (!quantity_from_json(json_get(json_get(p, "radiation"), "ambient"), DIM_TEMPERATURE, "degC", &bc.ambient, err, sizeof err)) {
            op_fail(out, NV_ERR_INVALID_UNIT, NULL, "radiation: %s", err);
            return;
        }
        if (!(bc.magnitude >= 0 && bc.magnitude <= 1)) {
            op_fail(out, NV_ERR_OUT_OF_RANGE, "emissivity is a fraction between 0 and 1", "emissivity %g is outside 0..1", bc.magnitude);
            return;
        }
        break;
    case BC_HEAT_SOURCE:
        if (!quantity_from_json(json_get(p, "power_density"), DIM_VOLUMETRIC_POWER, "W/m^3", &bc.magnitude, err, sizeof err)) {
            op_fail(out, NV_ERR_INVALID_UNIT, NULL, "power_density: %s", err);
            return;
        }
        break;
    default: break;
    }
    const JsonValue *sched = json_get(p, "schedule");
    if (sched) {
        if (!bc_is_thermal(bc.kind)) {
            op_fail(out, NV_ERR_UNSUPPORTED, "omit schedule: mechanical loads are constant in this build",
                    "a schedule applies to thermal conditions only, not to a %s condition", bc_kind_name(bc.kind));
            return;
        }
        size_t n = json_len(sched);
        if (n < 1 || n > BC_SCHEDULE_MAX) {
            op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "a schedule needs 1 to %d entries (got %zu)", BC_SCHEDULE_MAX, n);
            return;
        }
        for (size_t i = 0; i < n; i++) {
            const JsonValue *en = json_at(sched, i);
            double t, f = json_get_num(en, "factor", NAN);
            if (!quantity_from_json(json_get(en, "time"), DIM_TIME, "s", &t, err, sizeof err)) {
                op_fail(out, NV_ERR_INVALID_UNIT, NULL, "schedule[%zu].time: %s", i, err);
                return;
            }
            if (i == 0 && t != 0) {
                op_fail(out, NV_ERR_INVALID_PARAMS, "start the schedule with {\"time\": \"0 s\", \"factor\": ...} so the value before the first change is stated",
                        "the first schedule entry must be at time 0 (got %g s)", t);
                return;
            }
            if (i > 0 && !(t > bc.schedule_t[i - 1])) {
                op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "schedule times must increase strictly (entry %zu at %g s follows %g s)", i, t, bc.schedule_t[i - 1]);
                return;
            }
            const char *bad = NULL;
            if (!isfinite(f)) bad = "the factor must be a finite number";
            else if (bc.kind == BC_TEMPERATURE && f != 0 && f != 1) bad = "a prescribed temperature is either held (factor 1) or released (factor 0)";
            else if (bc.kind == BC_CONVECTION && f < 0) bad = "a convection coefficient cannot be scaled below zero";
            else if (bc.kind == BC_RADIATION && (f < 0 || f * bc.magnitude > 1)) bad = "the scaled emissivity must stay within 0..1";
            if (bad) {
                op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "schedule[%zu]: %s (factor %g)", i, bad, f);
                return;
            }
            bc.schedule_t[i] = t, bc.schedule_factor[i] = f;
        }
        bc.nschedule = (int)n;
    }
    if (bc.kind == BC_FRICTIONLESS) {
        Body *b = project_body(proj, sel->body);
        int ax = dominant_axis(sel->normal);
        const double c = cos(M_PI / 180.0);
        for (int i = 0; b && i < sel->ntri; i++)
            if (fabs(b->build_normal[3 * (size_t)sel->tris[i] + ax]) < c) {
                op_fail(out, NV_ERR_UNSUPPORTED, "use flat faces normal to x, y or z, or a displacement condition on the normal component",
                        "frictionless supports need a flat face normal to a build axis; faces of '%s' deviate by more than 1 degree", selname);
                return;
            }
    }
    if (bc.kind == BC_TEMPERATURE && sel) {
        for (int i = 0; i < proj->nbcs; i++) {
            const BoundaryCondition *o = &proj->bcs[i];
            if (o->kind != BC_TEMPERATURE || !strcmp(o->name, bc.name)) continue;
            Selection *os = project_selection(proj, o->selection);
            int shared = os ? shared_tris(sel, os) : 0;
            if (shared && fabs(o->magnitude - bc.magnitude) > 1e-9) {
                op_fail(out, NV_ERR_CONFLICTING_BC, "remove or change one of the two conditions",
                        "'%s' would prescribe %.4g degC on %d faces where '%s' prescribes %.4g degC", bc.name, bc.magnitude - 273.15, shared, o->name,
                        o->magnitude - 273.15);
                return;
            }
        }
    }
    if (bc_is_constraint(bc.kind)) {
        bool comp[3], oc[3];
        double val[3], ov[3];
        prescribed(&bc, sel, comp, val);
        for (int i = 0; i < proj->nbcs; i++) {
            const BoundaryCondition *o = &proj->bcs[i];
            if (!bc_is_constraint(o->kind) || !strcmp(o->name, bc.name)) continue;
            Selection *os = project_selection(proj, o->selection);
            int shared = os ? shared_tris(sel, os) : 0;
            if (!shared) continue;
            prescribed(o, os, oc, ov);
            for (int k = 0; k < 3; k++)
                if (comp[k] && oc[k] && fabs(val[k] - ov[k]) > 1e-12 + 1e-9 * fabs(val[k])) {
                    op_fail(out, NV_ERR_CONFLICTING_BC, "remove or change one of the two conditions",
                            "'%s' would prescribe %c = %.6g mm on %d faces where '%s' prescribes %.6g mm", bc.name, "xyz"[k], 1e3 * val[k], shared, o->name,
                            1e3 * ov[k]);
                    return;
                }
        }
    }
    snprintf(bc.description, sizeof bc.description, "%s", json_get_str(p, "description", ""));
    bc.source = (Provenance)provenance_from_name(json_get_str(p, "source", "user"));
    BoundaryCondition *slot = existing ? existing : project_add_bc(proj);
    if (!slot) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    *slot = bc;
    char subj[96];
    snprintf(subj, sizeof subj, "boundary_conditions/%s", name);
    if (bc.source != PROV_USER)
        project_note_assumption(proj, subj, bc.source, "%s condition '%s' on %s%s%s", bc_kind_name(bc.kind), name, selname ? selname : "all bodies",
                                bc.description[0] ? ": " : "", bc.description);
    else
        project_clear_assumptions(proj, subj);
    engine_touch(e, "boundary_apply", "%s %s condition %s on %s", existing ? "replaced" : "applied", bc_kind_name(bc.kind), name, selname ? selname : "all bodies");
    slot->revision = proj->revision;
    JsonValue *v = json_object();
    json_set(v, "boundary_condition", bc_details(proj, slot));
    JsonValue *w = json_set_array(v, "warnings");
    if (bc.source != PROV_USER && !bc_is_constraint(bc.kind))
        json_push(w, json_stringf("the load value is recorded as %s, not stated by the user: confirm it before relying on results", provenance_name(bc.source)));
    if ((bc.kind == BC_FORCE || bc.kind == BC_TRACTION || bc.kind == BC_GRAVITY) && bc.vec[0] == 0 && bc.vec[1] == 0 && bc.vec[2] == 0)
        json_push(w, json_string("the load is zero"));
    if (sel && proj->mesh.valid && selection_mesh_faces(proj, sel, NULL, 0, NULL) == 0)
        json_push(w, json_string("no face of the current mesh carries this selection; refine the mesh"));
    op_succeed(out, v);
}

static void op_boundary_list(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    JsonValue *v = json_object();
    JsonValue *arr = json_set_array(v, "boundary_conditions");
    int supports = 0, loads = 0;
    for (int i = 0; i < e->proj->nbcs; i++) {
        json_push(arr, bc_details(e->proj, &e->proj->bcs[i]));
        if (bc_is_constraint(e->proj->bcs[i].kind)) supports++;
        else loads++;
    }
    json_set_int(v, "supports", supports);
    json_set_int(v, "loads", loads);
    op_succeed(out, v);
}

static void op_boundary_remove(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    const char *name = json_get_str(p, "name", "");
    if (!project_bc(e->proj, name)) {
        op_fail(out, NV_ERR_NOT_FOUND, "list them with boundary_list", "no boundary condition named '%s'", name);
        return;
    }
    char nm[64], subj[96];
    snprintf(nm, sizeof nm, "%s", name);
    project_remove_bc(e->proj, nm);
    snprintf(subj, sizeof subj, "boundary_conditions/%s", nm);
    project_clear_assumptions(e->proj, subj);
    engine_touch(e, "boundary_remove", "removed boundary condition %s", nm);
    JsonValue *v = json_object();
    json_set_string(v, "removed", nm);
    op_succeed(out, v);
}

const OpBinding OPS_SETUP_BINDINGS[] = {
    {"materials_list", op_materials_list},
    {"material_list", op_material_list},
    {"material_define", op_material_define},
    {"material_assign", op_material_assign},
    {"boundary_apply", op_boundary_apply},
    {"boundary_list", op_boundary_list},
    {"boundary_remove", op_boundary_remove},
    {NULL, NULL},
};
