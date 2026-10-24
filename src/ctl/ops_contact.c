/* ops_contact.c - thermal interfaces between bodies: interface_define, interface_list, interface_remove, interface_preview */
#include "../fem/hex8.h"
#include "contact.h"
#include "ops_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mesh_body(const Project *p, const char *name) {
    for (int b = 0; b < p->mesh.nbodies; b++)
        if (!strcmp(p->mesh.body_name[b], name)) return b;
    return -1;
}

/* the part of a thermal condition's selection that faces the other body of an interface: those faces are internal in
 * the mesh, so the condition does not act there. Probed by stepping half an element outward from every selected
 * triangle and asking which body owns that cell. */
static double selection_area_facing(Project *p, const Selection *sel, int other) {
    const HexMesh *hm = &p->mesh.hm;
    Body *b = project_body(p, sel->body);
    if (!b || !b->build_centroid) return 0;
    double step = 0.5 * fmin(hm->h[0], fmin(hm->h[1], hm->h[2])), area = 0;
    for (int i = 0; i < sel->ntri; i++) {
        int t = sel->tris[i];
        const double *c = b->build_centroid + 3 * (size_t)t, *n = b->build_normal + 3 * (size_t)t;
        double q[3] = {c[0] + step * n[0], c[1] + step * n[1], c[2] + step * n[2]};
        int e = hexmesh_locate(hm, q);
        if (e >= 0 && hm->region[e] != HEX_REGION_PLATE && hm->body[e] == other) area += b->build_area[t];
    }
    return area;
}

/* the interface as stored, its coverage in the current mesh, and whether it can be used */
static JsonValue *interface_report(Project *p, const ThermalContact *tc, bool *valid, char *problem, size_t cap) {
    JsonValue *o = thermal_contact_json(tc);
    *valid = true;
    problem[0] = 0;
    double h = contact_conductance(tc);
    if (tc->model == CONTACT_CONDUCTANCE || tc->model == CONTACT_THIN_LAYER) json_set_number(o, "effective_conductance_w_m2k", h);
    char why[300];
    if (!project_mesh_current(p, why, sizeof why)) {
        json_set_string(o, "topology", "not checked: there is no current mesh; mesh_generate, then interface_list checks it");
        return o;
    }
    int a = mesh_body(p, tc->body_a), b = mesh_body(p, tc->body_b);
    if (a < 0 || b < 0) {
        *valid = false;
        snprintf(problem, cap, "'%s' and '%s' are not both part of the mesh", tc->body_a, tc->body_b);
        json_set_string(o, "problem", problem);
        return o;
    }
    ContactTopology t;
    if (!contact_topology(&p->mesh.hm, a, b, &t)) {
        *valid = false;
        snprintf(problem, cap, "out of memory analysing the interface");
        json_set_string(o, "problem", problem);
        return o;
    }
    json_set(o, "coverage", contact_topology_json(&t));
    const char *prob = contact_topology_problem(&t, tc->model, tc->body_a, tc->body_b, problem, cap);
    if (prob) {
        *valid = false;
        json_set_string(o, "problem", prob);
    }
    if (t.nfaces > 0 && h > 0 && isfinite(h)) {
        json_set_number(o, "total_conductance_w_k", h * t.area);
        json_set_number(o, "interface_resistance_k_w", 1.0 / (h * t.area));
    }
    json_set_string(o, "mechanical", "the bodies stay bonded structurally: only the thermal model is split at this interface");
    /* exterior thermal conditions whose selections reach onto the interface */
    JsonValue *ext = json_array();
    for (int i = 0; i < p->nbcs; i++) {
        const BoundaryCondition *bc = &p->bcs[i];
        if (bc->kind != BC_HEAT_FLUX && bc->kind != BC_CONVECTION && bc->kind != BC_RADIATION) continue;
        Selection *sel = project_selection(p, bc->selection);
        if (!sel || sel->stale) continue;
        int own = mesh_body(p, sel->body), other = own == a ? b : (own == b ? a : -1);
        if (other < 0) continue;
        double area = selection_area_facing(p, sel, other);
        if (area <= 0) continue;
        JsonValue *eo = json_object();
        json_set_string(eo, "condition", bc->name);
        json_set_string(eo, "kind", bc_kind_name(bc->kind));
        json_set_string(eo, "selection", sel->name);
        json_set_number(eo, "area_on_interface_mm2", 1e6 * area);
        json_set_number(eo, "fraction_of_selection", sel->area > 0 ? area / sel->area : 0);
        json_set_string(eo, "effect", "this part of the selection is internal in the mesh, so the exterior condition does not act on it (no double counting)");
        json_push(ext, eo);
    }
    if (json_len(ext)) json_set(o, "exterior_conditions_reaching_the_interface", ext);
    else json_free(ext);
    contact_topology_free(&t);
    return o;
}

static bool quantity_positive(OpResult *out, const JsonValue *v, Dimension d, const char *unit, const char *what, double *si) {
    char err[256];
    if (!v) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "use the value the user stated; if none was stated, ask instead of assuming one", "the model needs %s", what);
        return false;
    }
    if (!quantity_from_json(v, d, unit, si, err, sizeof err)) {
        op_fail(out, NV_ERR_INVALID_UNIT, NULL, "%s: %s", what, err);
        return false;
    }
    if (!(*si > 0) || !isfinite(*si)) {
        op_fail(out, NV_ERR_OUT_OF_RANGE, NULL, "%s must be positive (got %g)", what, *si);
        return false;
    }
    return true;
}

static void op_interface_define(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    const char *name = json_get_str(p, "name", ""), *na = json_get_str(p, "body_a", ""), *nb = json_get_str(p, "body_b", "");
    if (!body_name_valid(name)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "use letters, digits, '_' and '-'", "invalid interface name '%s'", name);
        return;
    }
    Body *ba = na[0] ? project_body(proj, na) : NULL, *bb = nb[0] ? project_body(proj, nb) : NULL;
    if (!ba || !bb) {
        op_fail(out, NV_ERR_NOT_FOUND, "use body names from project_inspect", "no body named '%s'", !ba ? na : nb);
        return;
    }
    if (!strcmp(na, nb)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "an interface joins two different bodies ('%s' twice)", na);
        return;
    }
    ThermalContact *existing = project_contact(proj, name);
    if (existing && !json_get_bool(p, "replace", false)) {
        op_fail(out, NV_ERR_ALREADY_EXISTS, "pass replace: true to change it", "an interface named '%s' exists", name);
        return;
    }
    for (int i = 0; i < proj->ncontacts; i++) {
        const ThermalContact *o = &proj->contacts[i];
        if (o == existing) continue;
        if ((!strcmp(o->body_a, na) && !strcmp(o->body_b, nb)) || (!strcmp(o->body_a, nb) && !strcmp(o->body_b, na))) {
            op_fail(out, NV_ERR_CONFLICTING_BC, "change that interface instead (replace: true), or remove it", "'%s' and '%s' already have interface '%s'", na,
                    nb, o->name);
            return;
        }
    }
    ThermalContact tc;
    memset(&tc, 0, sizeof tc);
    snprintf(tc.name, sizeof tc.name, "%s", name);
    snprintf(tc.body_a, sizeof tc.body_a, "%s", na);
    snprintf(tc.body_b, sizeof tc.body_b, "%s", nb);
    int model = contact_model_from_name(json_get_str(p, "model", ""));
    tc.model = (ContactModel)model;
    const JsonValue *cond = json_get(p, "conductance"), *layer = json_get(p, "layer");
    switch (tc.model) {
    case CONTACT_CONDUCTANCE:
        if (layer) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "use model thin_layer for a layer", "'layer' does not apply to a conductance interface");
            return;
        }
        if (!quantity_positive(out, cond, DIM_HEAT_TRANSFER_COEFFICIENT, "W/(m^2*K)", "conductance", &tc.conductance)) return;
        break;
    case CONTACT_THIN_LAYER:
        if (cond) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "use model conductance for a contact conductance", "'conductance' does not apply to a thin_layer interface");
            return;
        }
        if (!quantity_positive(out, json_get(layer, "thickness"), DIM_LENGTH, "mm", "layer.thickness", &tc.layer_thickness) ||
            !quantity_positive(out, json_get(layer, "conductivity"), DIM_THERMAL_CONDUCTIVITY, "W/(m*K)", "layer.conductivity", &tc.layer_conductivity))
            return;
        break;
    default:
        if (cond || layer) {
            op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "a %s interface takes neither conductance nor layer", contact_model_name(tc.model));
            return;
        }
        break;
    }
    snprintf(tc.description, sizeof tc.description, "%s", json_get_str(p, "description", ""));
    tc.source = (Provenance)provenance_from_name(json_get_str(p, "source", "user"));
    bool valid;
    char problem[512];
    JsonValue *rep = interface_report(proj, &tc, &valid, problem, sizeof problem);
    if (!valid) {
        op_fail(out, NV_ERR_GEOMETRY_INVALID, "interface_define needs two meshed bodies that meet face to face; see details.interface", "%s", problem);
        op_fail_detail(out, "interface", rep);
        return;
    }
    ThermalContact *slot = existing ? existing : project_add_contact(proj);
    if (!slot) {
        json_free(rep);
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    tc.revision = proj->revision + 1;
    *slot = tc;
    char subj[96];
    snprintf(subj, sizeof subj, "thermal_contacts/%s", name);
    if (tc.source != PROV_USER)
        project_note_assumption(proj, subj, tc.source, "%s thermal interface '%s' between '%s' and '%s'%s%s", contact_model_name(tc.model), name, na, nb,
                                tc.description[0] ? ": " : "", tc.description);
    else project_clear_assumptions(proj, subj);
    engine_touch(e, "interface_define", "%s thermal interface %s between %s and %s", existing ? "changed" : "defined", name, na, nb);
    JsonValue *v = json_object();
    json_set(v, "interface", rep);
    json_set_string(v, "next_step",
                    "run a transient_thermal or thermomechanical analysis; the summary reports the heat crossing each interface and each body's "
                    "own energy balance, and results_interface gives both sides' temperatures and the heat rate at a stored time");
    op_succeed(out, v);
}

static void op_interface_list(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    JsonValue *v = json_object(), *arr = json_set_array(v, "interfaces");
    for (int i = 0; i < proj->ncontacts; i++) {
        bool valid;
        char problem[512];
        JsonValue *rep = interface_report(proj, &proj->contacts[i], &valid, problem, sizeof problem);
        json_set_bool(rep, "valid", valid);
        json_push(arr, rep);
    }
    char why[300];
    if (project_mesh_current(proj, why, sizeof why)) {
        JsonValue *und = json_set_array(v, "touching_bodies_without_interface");
        for (int a = 0; a < proj->mesh.nbodies; a++)
            for (int b = a + 1; b < proj->mesh.nbodies; b++) {
                bool declared = false;
                for (int i = 0; i < proj->ncontacts; i++) {
                    const ThermalContact *c = &proj->contacts[i];
                    declared |= (!strcmp(c->body_a, proj->mesh.body_name[a]) && !strcmp(c->body_b, proj->mesh.body_name[b])) ||
                                (!strcmp(c->body_a, proj->mesh.body_name[b]) && !strcmp(c->body_b, proj->mesh.body_name[a]));
                }
                if (declared) continue;
                ContactTopology t;
                if (!contact_topology(&proj->mesh.hm, a, b, &t)) continue;
                if (t.nfaces > 0) {
                    JsonValue *o = json_object();
                    json_set_string(o, "body_a", proj->mesh.body_name[a]);
                    json_set_string(o, "body_b", proj->mesh.body_name[b]);
                    json_set(o, "coverage", contact_topology_json(&t));
                    json_set_string(o, "treated_as", "perfect contact (shared nodes), thermally and mechanically");
                    json_push(und, o);
                }
                contact_topology_free(&t);
            }
    } else {
        json_set_string(v, "topology", "not checked: there is no current mesh");
    }
    op_succeed(out, v);
}

static void op_interface_remove(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    const char *name = json_get_str(p, "name", "");
    if (!project_contact(e->proj, name)) {
        op_fail(out, NV_ERR_NOT_FOUND, "list them with interface_list", "no interface named '%s'", name);
        return;
    }
    char nm[64], subj[96];
    snprintf(nm, sizeof nm, "%s", name);
    project_remove_contact(e->proj, nm);
    snprintf(subj, sizeof subj, "thermal_contacts/%s", nm);
    project_clear_assumptions(e->proj, subj);
    engine_touch(e, "interface_remove", "removed thermal interface %s", nm);
    JsonValue *v = json_object();
    json_set_string(v, "removed", nm);
    json_set_string(v, "effect", "the two bodies are again perfectly bonded where they touch");
    op_succeed(out, v);
}

/* body a drawn from its boundary faces plus the interface faces, with the interface in the top colour */
static void op_interface_preview(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    Project *proj = e->proj;
    const char *name = json_get_str(p, "name", "");
    ThermalContact *tc = project_contact(proj, name);
    if (!tc) {
        op_fail(out, NV_ERR_NOT_FOUND, "list them with interface_list", "no interface named '%s'", name);
        return;
    }
    char why[300];
    if (!project_mesh_current(proj, why, sizeof why)) {
        op_fail(out, NV_ERR_PRECONDITION, "generate the mesh with mesh_generate", "a preview needs the mesh: %s", why);
        return;
    }
    bool valid;
    char problem[512];
    JsonValue *rep = interface_report(proj, tc, &valid, problem, sizeof problem);
    const HexMesh *hm = &proj->mesh.hm;
    int a = mesh_body(proj, tc->body_a), b = mesh_body(proj, tc->body_b);
    ContactTopology t;
    if (a < 0 || b < 0 || !contact_topology(hm, a, b, &t)) {
        json_free(rep);
        op_fail(out, NV_ERR_PRECONDITION, NULL, "the interface bodies are not meshed");
        return;
    }
    ViewOptions o;
    if (!op_parse_view(out, p, &o)) {
        json_free(rep), contact_topology_free(&t);
        return;
    }
    int nf = 0;
    for (int f = 0; f < hm->nfaces; f++) nf += hm->body[hm->face_elem[f]] == a && hm->region[hm->face_elem[f]] != HEX_REGION_PLATE;
    int *fe = malloc((size_t)(nf + t.nfaces + 1) * sizeof(int));
    unsigned char *fl = malloc((size_t)(nf + t.nfaces + 1));
    double *val = calloc((size_t)hm->nnodes, sizeof(double));
    if (!fe || !fl || !val) {
        free(fe), free(fl), free(val), json_free(rep), contact_topology_free(&t);
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return;
    }
    int k = 0;
    for (int f = 0; f < hm->nfaces; f++)
        if (hm->body[hm->face_elem[f]] == a && hm->region[hm->face_elem[f]] != HEX_REGION_PLATE) fe[k] = hm->face_elem[f], fl[k++] = hm->face_local[f];
    for (int f = 0; f < t.nfaces; f++) {
        fe[k] = t.elem_a[f], fl[k++] = t.face_a[f];
        const int *fn = HEX8_FACE_NODES[t.face_a[f]];
        for (int q = 0; q < 4; q++) val[hm->conn[8 * (size_t)t.elem_a[f] + fn[q]]] = 1;
    }
    char title[256];
    snprintf(title, sizeof title, "interface '%s' on '%s': %d faces, %.4g mm2 (%s)", name, tc->body_a, t.nfaces, 1e6 * t.area, contact_model_name(tc->model));
    FieldView fv = {hm->xyz, NULL, hm->conn, fe, fl, k, val, 0, 1, 0, false, SW_CMAP_VIRIDIS, title, "interface", "", NULL, NULL};
    ViewResult vr;
    char err[512];
    bool ok = view_render_field(&fv, &o, &vr, err, sizeof err);
    free(fe), free(fl), free(val);
    contact_topology_free(&t);
    if (!ok) {
        json_free(rep);
        op_fail(out, NV_ERR_PRECONDITION, NULL, "%s", err);
        return;
    }
    JsonValue *v = json_object();
    json_set(v, "interface", rep);
    json_set_bool(v, "valid", valid);
    json_set_string(v, "image_shows", "the mesh of body_a; the interface faces are drawn in the top colour of the scale (value 1), the rest of the body at 0");
    op_finish_view(e, out, &vr, "interface", NULL, "interface.png", v);
    op_succeed(out, v);
}

const OpBinding OPS_CONTACT_BINDINGS[] = {
    {"interface_define", op_interface_define},
    {"interface_list", op_interface_list},
    {"interface_remove", op_interface_remove},
    {"interface_preview", op_interface_preview},
    {NULL, NULL},
};
