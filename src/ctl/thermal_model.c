/* thermal_model.c - builds a transient thermal (and optionally thermomechanical) case from a project setup */
#include "../fem/flow.h"
#include "../fem/hex8.h"
#include "contact.h"
#include "selection.h"
#include "transient_analysis.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void issue(JsonValue *arr, NvErr code, const char *hint, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void issue(JsonValue *arr, NvErr code, const char *hint, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    json_push(arr, nv_error_json(code, hint, "%s", msg));
}

static void warn(JsonValue *arr, const char *code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void warn(JsonValue *arr, const char *code, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    JsonValue *o = json_object();
    json_set_string(o, "code", code);
    json_set_string(o, "message", msg);
    json_push(arr, o);
}

static void *dup_mem(const void *src, size_t n) {
    void *d = malloc(n ? n : 1);
    if (d && n) memcpy(d, src, n);
    return d;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int face_nodes(const ThermalCase *c, const int *faces, int nf, int **out) {
    int *nodes = malloc((size_t)(nf > 0 ? 4 * nf : 1) * sizeof(int));
    *out = nodes;
    if (!nodes) return -1;
    int k = 0;
    for (int i = 0; i < nf; i++) {
        int e = c->face_elem[faces[i]];
        const int *fn = HEX8_FACE_NODES[c->face_local[faces[i]]];
        for (int q = 0; q < 4; q++) nodes[k++] = c->conn[8 * (size_t)e + fn[q]];
    }
    qsort(nodes, (size_t)k, sizeof(int), cmp_int);
    int u = 0;
    for (int i = 0; i < k; i++)
        if (!u || nodes[i] != nodes[u - 1]) nodes[u++] = nodes[i];
    return u;
}

/* material slot for a body name ("build_plate" for the plate); adds it on first use */
static int material_slot(Project *p, ThermalCase *c, const char *target, JsonValue *errors, JsonValue *warnings) {
    for (int i = 0; i < c->nmat; i++)
        if (!strcmp(c->mat_id[i], target)) return i; /* slots are keyed by target, so bodies keep their own tables */
    MaterialAssignment *ma = project_material_for(p, target);
    if (!ma) {
        issue(errors, NV_ERR_PRECONDITION, "assign one with material_assign (materials_list shows what is available)", "'%s' has no material", target);
        return -1;
    }
    MaterialRecord rec;
    char err[300];
    if (!material_lookup(p->user_materials, ma->material, &rec, err, sizeof err)) {
        issue(errors, NV_ERR_NOT_FOUND, "assign an existing material", "'%s': %s", target, err);
        return -1;
    }
    if (!rec.prop[MATP_K].n || !rec.prop[MATP_CP].n || !rec.prop[MATP_DENSITY].n) {
        issue(errors, NV_ERR_PRECONDITION, "define conductivity_w_per_mk, specific_heat_j_per_kgk and density_kg_m3 (material_define)",
              "material '%s' of '%s' lacks the thermal properties", rec.id, target);
        return -1;
    }
    if (c->nmat == SA_MAX_MATERIALS) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "more than %d materials", SA_MAX_MATERIALS);
        return -1;
    }
    int i = c->nmat++;
    rec.json = NULL; /* the job runs while the project may change: keep no pointer into it */
    c->rec[i] = rec;
    snprintf(c->mat_id[i], sizeof c->mat_id[i], "%s", target);
    snprintf(c->mat_status[i], sizeof c->mat_status[i], "%s", rec.status);
    memset(&c->tmat[i], 0, sizeof c->tmat[i]); /* the optional anisotropy and phase-change fields must start at zero */
    c->tmat[i].k = (ThermalTable){c->rec[i].prop[MATP_K].n, c->rec[i].prop[MATP_K].t, c->rec[i].prop[MATP_K].v};
    c->tmat[i].cp = (ThermalTable){c->rec[i].prop[MATP_CP].n, c->rec[i].prop[MATP_CP].t, c->rec[i].prop[MATP_CP].v};
    c->tmat[i].rho = (ThermalTable){c->rec[i].prop[MATP_DENSITY].n, c->rec[i].prop[MATP_DENSITY].t, c->rec[i].prop[MATP_DENSITY].v};
    if (c->rec[i].prop[MATP_K2].n) {
        c->tmat[i].k2 = (ThermalTable){c->rec[i].prop[MATP_K2].n, c->rec[i].prop[MATP_K2].t, c->rec[i].prop[MATP_K2].v};
        if (c->rec[i].prop[MATP_K3].n)
            c->tmat[i].k3 = (ThermalTable){c->rec[i].prop[MATP_K3].n, c->rec[i].prop[MATP_K3].t, c->rec[i].prop[MATP_K3].v};
        if (c->rec[i].has_k_axes) memcpy(c->tmat[i].axes, c->rec[i].k_axes, sizeof c->tmat[i].axes);
        warn(warnings, "ANISOTROPIC_CONDUCTIVITY",
             "'%s' conducts anisotropically (%g / %g / %g W/(m K) along its principal directions%s): the conductivity tensor is "
             "rotated into the build frame and integrated at the Gauss points",
             target, mat_eval(&c->rec[i].prop[MATP_K], 293.15), mat_eval(&c->rec[i].prop[MATP_K2], 293.15),
             c->rec[i].prop[MATP_K3].n ? mat_eval(&c->rec[i].prop[MATP_K3], 293.15) : mat_eval(&c->rec[i].prop[MATP_K], 293.15),
             c->rec[i].has_k_axes ? ", rotated axes" : ", aligned with the body axes");
    }
    /* latent heat of melting. The enthalpy formulation carries it through an equilibrium liquid fraction that is
     * linear between solidus and liquidus; without both temperatures it cannot be applied and is reported as
     * ignored rather than silently dropped. */
    if (c->settings.phase_change && c->rec[i].latent_heat > 0) {
        if (isfinite(c->rec[i].solidus_k) && isfinite(c->rec[i].liquidus_k) && c->rec[i].liquidus_k > c->rec[i].solidus_k) {
            c->tmat[i].latent_heat = c->rec[i].latent_heat;
            c->tmat[i].solidus = c->rec[i].solidus_k;
            c->tmat[i].liquidus = c->rec[i].liquidus_k;
            warn(warnings, "PHASE_CHANGE_ACTIVE",
                 "'%s' melts between %.1f and %.1f degC with %g J/kg of latent heat: the enthalpy formulation is used and a "
                 "mushy interval of %.1f K is assumed to be at equilibrium",
                 target, c->rec[i].solidus_k - 273.15, c->rec[i].liquidus_k - 273.15, c->rec[i].latent_heat,
                 c->rec[i].liquidus_k - c->rec[i].solidus_k);
        } else {
            warn(warnings, "LATENT_HEAT_IGNORED", "'%s' states %g J/kg of latent heat but no solidus/liquidus pair, so melting is not modelled", target,
                 c->rec[i].latent_heat);
        }
    } else if (c->rec[i].latent_heat > 0) {
        warn(warnings, "PHASE_CHANGE_DISABLED", "'%s' can melt (%g J/kg) but phase_change is off: only sensible heat is stored", target,
             c->rec[i].latent_heat);
    }
    if (!strcmp(rec.status, "demonstration"))
        warn(warnings, "DEMONSTRATION_MATERIAL", "'%s' uses the demonstration material '%s': temperatures follow its uncalibrated conductivity and heat capacity",
             target, rec.id);
    bool flow_domain = c->settings.cht && !strcmp(target, c->settings.fluid_body); /* no stiffness: left out of the structure */
    if (c->settings.mechanical && !flow_domain && (!rec.prop[MATP_E].n || !rec.prop[MATP_NU].n || !rec.prop[MATP_ALPHA].n))
        issue(errors, NV_ERR_PRECONDITION, "define youngs_modulus_pa, poisson_ratio and expansion_1_per_k (material_define)",
              "a thermomechanical analysis needs elastic and expansion properties for material '%s' of '%s'", rec.id, target);
    return i;
}

typedef struct {
    Selection *sel;
    Body *body;
    int nfaces, *faces;
    double mesh_area;
} Target;

static bool resolve(Project *p, const BoundaryCondition *bc, Target *t, JsonValue *errors) {
    memset(t, 0, sizeof *t);
    Selection *sel = project_selection(p, bc->selection);
    if (!sel) {
        issue(errors, NV_ERR_NOT_FOUND, "recreate the selection or remove the condition", "'%s' refers to selection '%s', which does not exist", bc->name,
              bc->selection);
        return false;
    }
    if (sel->stale || strcmp(sel->set_hash, bc->selection_hash) != 0) {
        issue(errors, NV_ERR_STALE_REFERENCE, "check the selection and re-apply the condition with boundary_apply (replace: true)",
              "selection '%s' used by '%s' %s", sel->name, bc->name, sel->stale ? "is stale" : "resolves to different faces than when it was applied");
        return false;
    }
    int n = selection_mesh_faces(p, sel, NULL, 0, &t->mesh_area);
    if (n <= 0) {
        issue(errors, NV_ERR_MESH_INVALID, "refine the mesh or check the selection", "no mesh face carries selection '%s' used by '%s'", sel->name, bc->name);
        return false;
    }
    t->faces = malloc((size_t)n * sizeof(int));
    if (!t->faces) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory mapping '%s'", bc->name);
        return false;
    }
    selection_mesh_faces(p, sel, t->faces, n, NULL);
    t->nfaces = n;
    t->sel = sel;
    t->body = project_body(p, sel->body);
    return true;
}

/* ---- conjugate heat transfer: the flow domain ----------------------------------------------------------------------- */

/* the fluid body, its material, the lattice over its bounding box, the advected elements and the conjugate interface */
static bool cht_prepare(Project *p, ThermalCase *c, const HexMesh *hm, const int *slot_of_body, JsonValue *errors, JsonValue *warnings) {
    const ThermalSettings *s = &c->settings;
    int nn = c->nnodes, ne = c->nelems, fb = -1;
    for (int b = 0; b < p->mesh.nbodies; b++)
        if (!strcmp(p->mesh.body_name[b], s->fluid_body)) fb = b;
    if (fb < 0) {
        issue(errors, NV_ERR_NOT_FOUND, "name a meshed body as flow.fluid_body (project_get lists the bodies)", "the fluid body '%s' is not part of the mesh",
              s->fluid_body);
        return false;
    }
    c->fluid_body = fb;
    int slot = slot_of_body[fb];
    if (slot < 0) return false; /* the material problem is already reported */
    const MaterialRecord *rec = &c->rec[slot];
    const ThermalMaterial *tm = &c->tmat[slot];
    if (strcmp(rec->family, "fluid") || rec->prop[MATP_VISCOSITY].n == 0) {
        issue(errors, NV_ERR_PRECONDITION, "assign a fluid material such as air_demo with material_assign, or define one with family fluid and dynamic_viscosity_pa_s",
              "the flow domain '%s' has the material '%s', which is not a fluid with a dynamic viscosity", s->fluid_body, rec->id);
        return false;
    }
    if (rec->prop[MATP_VISCOSITY].n != 1 || tm->k.n != 1 || tm->cp.n != 1 || tm->rho.n != 1 || thermal_is_anisotropic(tm) || tm->latent_heat != 0) {
        issue(errors, NV_ERR_UNSUPPORTED, "give the fluid material constant (single-value) properties",
              "the fluid '%s' has temperature-dependent or anisotropic properties: the flow and its energy transport are implemented for "
              "constant-property fluids only",
              rec->id);
        return false;
    }
    for (int i = 0; i < c->ncontacts; i++)
        if (c->contact[i].mesh_body_a == fb || c->contact[i].mesh_body_b == fb) {
            issue(errors, NV_ERR_UNSUPPORTED, "remove the interface with interface_remove: the fluid-solid interface is resolved",
                  "interface '%s' is declared on the fluid body '%s': the conjugate interface exchanges heat through the computed flow and perfect "
                  "thermal contact, so an empirical resistance or coefficient there would count the wall resistance twice",
                  c->contact[i].name, s->fluid_body);
            return false;
        }
    double h = hm->h[0];
    if (fabs(hm->h[1] - h) > 1e-9 * h || fabs(hm->h[2] - h) > 1e-9 * h) {
        issue(errors, NV_ERR_INVALID_PARAMS, "regenerate the mesh with a single element_size",
              "the lattice Boltzmann flow needs cubic cells, but the mesh cells are %.4g x %.4g x %.4g mm", 1e3 * hm->h[0], 1e3 * hm->h[1], 1e3 * hm->h[2]);
        return false;
    }
    int lo[3] = {1 << 30, 1 << 30, 1 << 30}, hi[3] = {-1, -1, -1}, nfluid = 0;
    for (int e = 0; e < ne; e++) {
        if (hm->body[e] != fb) continue;
        nfluid++;
        for (int d = 0; d < 3; d++) {
            int v = hm->ijk[3 * (size_t)e + d];
            if (v < lo[d]) lo[d] = v;
            if (v > hi[d]) hi[d] = v;
        }
    }
    int n[3] = {hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1};
    if (!nfluid || n[0] < 4 || n[1] < 4 || n[2] < 4) {
        issue(errors, NV_ERR_INVALID_PARAMS, "refine the mesh: the flow lattice needs at least 4 cells across every direction of the fluid body",
              "the fluid body '%s' spans %d x %d x %d cells", s->fluid_body, nfluid ? n[0] : 0, nfluid ? n[1] : 0, nfluid ? n[2] : 0);
        return false;
    }
    size_t ncell = (size_t)n[0] * (size_t)n[1] * (size_t)n[2];
    if (ncell > 20000000) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, "coarsen the mesh", "the flow lattice would have %zu cells (limit 20 million)", ncell);
        return false;
    }
    memcpy(c->flow_n, n, sizeof n);
    c->flow_dx = h;
    for (int d = 0; d < 3; d++) c->flow_origin[d] = hm->origin[d] + lo[d] * h;
    c->flow_solid = malloc(ncell);
    c->advect = calloc((size_t)ne, 1);
    c->node_part = calloc((size_t)nn, 1);
    c->velocity = calloc(3 * (size_t)nn, sizeof(double));
    c->flow_corner = malloc((size_t)nn * sizeof(int));
    if (!c->flow_solid || !c->advect || !c->node_part || !c->velocity || !c->flow_corner) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory for the flow domain");
        return false;
    }
    memset(c->flow_solid, 1, ncell);
    for (int e = 0; e < ne; e++) {
        bool fluid = hm->body[e] == fb;
        c->advect[e] = fluid;
        for (int a = 0; a < 8; a++) c->node_part[c->conn[8 * (size_t)e + a]] |= fluid ? 2 : 1;
        if (!fluid) continue;
        int i = hm->ijk[3 * (size_t)e] - lo[0], j = hm->ijk[3 * (size_t)e + 1] - lo[1], k = hm->ijk[3 * (size_t)e + 2] - lo[2];
        c->flow_solid[(size_t)i + (size_t)n[0] * ((size_t)j + (size_t)n[1] * (size_t)k)] = 0;
    }
    for (int q = 0; q < nn; q++) {
        c->flow_corner[q] = -1;
        if (!(c->node_part[q] & 2)) continue;
        int ci[3];
        bool on_grid = true;
        for (int d = 0; d < 3; d++) {
            double r = (c->xyz[3 * (size_t)q + d] - c->flow_origin[d]) / h;
            ci[d] = (int)lround(r);
            on_grid &= fabs(r - ci[d]) < 1e-6 && ci[d] >= 0 && ci[d] <= n[d];
        }
        if (!on_grid) {
            issue(errors, NV_ERR_INTERNAL, NULL, "fluid node %d does not lie on the flow lattice", q);
            return false;
        }
        c->flow_corner[q] = ci[0] + (n[0] + 1) * (ci[1] + (n[1] + 1) * ci[2]);
    }
    for (int q = 0; q < nn; q++) c->ninterface_nodes += c->node_part[q] == 3;
    /* conjugate interface area: fluid cells whose face neighbour is an element of another body */
    static const int NB[6][3] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}};
    for (int e = 0; e < ne; e++) {
        if (hm->body[e] != fb) continue;
        for (int f = 0; f < 6; f++) {
            int i = hm->ijk[3 * (size_t)e] + NB[f][0], j = hm->ijk[3 * (size_t)e + 1] + NB[f][1], k = hm->ijk[3 * (size_t)e + 2] + NB[f][2];
            if (i < 0 || j < 0 || k < 0 || i >= hm->dims[0] || j >= hm->dims[1] || k >= hm->dims[2]) continue;
            int o = hm->cell_elem[(size_t)i + (size_t)hm->dims[0] * ((size_t)j + (size_t)hm->dims[1] * (size_t)k)];
            if (o >= 0 && hm->body[o] != fb) c->interface_area += h * h;
        }
    }
    if (!c->ninterface_nodes) {
        issue(errors, NV_ERR_PRECONDITION, "place a solid body in face contact with the fluid domain (the mesh must share nodes between them)",
              "no solid body touches the fluid body '%s': there is no conjugate interface", s->fluid_body);
        return false;
    }
    for (int w = 0; w < 4; w++)
        if (s->flow_wall[w] == FLOW_WALL_PERIODIC) {
            warn(warnings, "PERIODIC_FLOW_SIDES",
                 "the flow is periodic across the fluid box's %s sides, but the energy equation is not: those sides are adiabatic walls for "
                 "the temperature", w < 2 ? "y" : "z");
            break;
        }
    warn(warnings, "ONE_WAY_FLOW",
         "the flow is computed once, as a steady laminar field with constant properties and without buoyancy, and then carries the "
         "energy: temperature does not change the flow");
    return true;
}

/* open faces on the inlet (x-min) and outlet (x-max) planes of the fluid box; conditions there would conflict */
static bool cht_open_faces(ThermalCase *c, const HexMesh *hm, JsonValue *errors) {
    int fb = c->fluid_body;
    int lo = 1 << 30, hi = -1;
    for (int e = 0; e < c->nelems; e++)
        if (hm->body[e] == fb) {
            int v = hm->ijk[3 * (size_t)e];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    int base = c->ntfaces;
    for (int f = 0; f < hm->nfaces; f++) {
        int e = hm->face_elem[f], local = hm->face_local[f], i = hm->ijk[3 * (size_t)e];
        if (hm->body[e] != fb) continue;
        bool inlet = local == 5 && i == lo, outlet = local == 3 && i == hi; /* hex8 local faces: 5 is x-, 3 is x+ */
        if (!inlet && !outlet) continue;
        for (int k = 0; k < base; k++)
            if (c->tfaces[k].elem == e && c->tfaces[k].face == local) {
                int b = c->tface_bc[k];
                issue(errors, NV_ERR_CONFLICTING_BC, "remove that condition: the open boundary sets what enters (flow.inlet_temperature) and what leaves",
                      "condition '%s' acts on the flow %s plane of '%s'", b >= 0 ? c->bc[b].name : "?", inlet ? "inlet" : "outlet", c->settings.fluid_body);
                return false;
            }
        c->tface_bc[c->ntfaces] = -1; /* not a user condition: the open boundary's energy is reported as advection */
        c->tface_base[c->ntfaces] = 0;
        c->tfaces[c->ntfaces++] = (ThermalFace){e, (unsigned char)local, THERMAL_OPEN, 0, c->settings.inlet_temperature};
        if (inlet) c->nopen_in++, c->inlet_area += c->flow_dx * c->flow_dx;
        else c->nopen_out++;
    }
    if (!c->nopen_in || !c->nopen_out) {
        issue(errors, NV_ERR_PRECONDITION, "the flow enters at the fluid body's smallest x and leaves at its largest x: those faces must be outer faces of the mesh",
              "the fluid body '%s' has no exposed %s face", c->settings.fluid_body, !c->nopen_in ? "inlet (x-min)" : "outlet (x-max)");
        return false;
    }
    return true;
}

bool thermal_case_build(Project *p, const ThermalSettings *s, ThermalCase *c, JsonValue *errors, JsonValue *warnings) {
    memset(c, 0, sizeof *c);
    c->settings = *s;
    size_t nerr0 = json_len(errors);
    char why[300];
    if (!p->nbodies) {
        issue(errors, NV_ERR_PRECONDITION, "import geometry with geometry_import", "the project has no bodies");
        return false;
    }
    if (!project_mesh_current(p, why, sizeof why)) {
        issue(errors, p->mesh.valid ? NV_ERR_STALE_REFERENCE : NV_ERR_PRECONDITION, "generate the mesh with mesh_generate", "no current mesh: %s", why);
        return false;
    }
    if (!(s->end_time > 0) || (s->stepping == THERMAL_STEPPING_FIXED && !(s->time_step > 0))) {
        issue(errors, NV_ERR_INVALID_PARAMS, "give end_time, and time_step for fixed stepping (or time_stepping: \"adaptive\")",
              "the analysis needs a positive end_time%s", s->stepping == THERMAL_STEPPING_FIXED ? " and time_step" : "");
        return false;
    }
    if (s->stepping == THERMAL_STEPPING_ADAPTIVE) {
        /* the controller limits are checked when the run is submitted, not when the worker starts it */
        StepControllerSettings cs = {.dt_initial = s->time_step, .dt_min = s->dt_min, .dt_max = s->dt_max, .safety = s->step_safety,
                                     .max_growth = s->max_step_growth, .max_shrink = s->max_step_shrink, .max_rejections = s->max_rejections,
                                     .max_steps = s->max_steps, .order = 2};
        char cerr[300];
        if (!stepctl_defaults(&cs, s->end_time, cerr, sizeof cerr)) {
            issue(errors, NV_ERR_OUT_OF_RANGE, "make min_time_step <= time_step (the first step) <= max_time_step", "adaptive stepping: %s", cerr);
            return false;
        }
    }
    const HexMesh *hm = &p->mesh.hm;
    int nn = hm->nnodes, ne = hm->nelems, nf = hm->nfaces;
    c->nnodes = nn, c->nelems = ne, c->nfaces = nf;
    memcpy(c->h, hm->h, sizeof c->h);
    snprintf(c->mesh_hash, sizeof c->mesh_hash, "%s", p->mesh.hash);
    c->nbodies = p->mesh.nbodies;
    for (int i = 0; i < c->nbodies; i++) snprintf(c->body_name[i], sizeof c->body_name[i], "%s", p->mesh.body_name[i]);
    c->xyz = dup_mem(hm->xyz, 3 * (size_t)nn * sizeof(double));
    c->conn = dup_mem(hm->conn, 8 * (size_t)ne * sizeof(int));
    c->elem_body = dup_mem(hm->body, (size_t)ne);
    c->face_elem = dup_mem(hm->face_elem, (size_t)nf * sizeof(int));
    c->face_local = dup_mem(hm->face_local, (size_t)nf);
    c->nmesh_nodes = nn;
    if (!c->xyz || !c->conn) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, "use a coarser mesh", "out of memory copying the mesh (%d nodes, %d elements)", nn, ne);
        return false;
    }

    /* thermal interfaces: resolve the bodies, check the topology, split the thermal nodes of non-perfect interfaces */
    if (p->ncontacts > THERMAL_MAX_GROUPS) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "at most %d thermal interfaces are supported (the project has %d)", THERMAL_MAX_GROUPS, p->ncontacts);
        return false;
    }
    int ca[THERMAL_MAX_GROUPS], cb[THERMAL_MAX_GROUPS];
    bool any_split = false;
    for (int i = 0; i < p->ncontacts; i++) {
        const ThermalContact *tc = &p->contacts[i];
        ca[i] = cb[i] = -1;
        for (int b = 0; b < p->mesh.nbodies; b++) {
            if (!strcmp(p->mesh.body_name[b], tc->body_a)) ca[i] = b;
            if (!strcmp(p->mesh.body_name[b], tc->body_b)) cb[i] = b;
        }
        if (ca[i] < 0 || cb[i] < 0) {
            issue(errors, NV_ERR_NOT_FOUND, "mesh both bodies (mesh_generate) or remove the interface (interface_remove)",
                  "interface '%s' joins '%s' and '%s', which are not both part of the mesh", tc->name, tc->body_a, tc->body_b);
            return false;
        }
        ContactTopology top;
        if (!contact_topology(hm, ca[i], cb[i], &top)) {
            issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory analysing interface '%s'", tc->name);
            return false;
        }
        char why2[512];
        if (contact_topology_problem(&top, tc->model, tc->body_a, tc->body_b, why2, sizeof why2)) {
            issue(errors, NV_ERR_GEOMETRY_INVALID, "interface_define inspects the pair; fix the geometry or remove the interface", "interface '%s': %s",
                  tc->name, why2);
            contact_topology_free(&top);
            return false;
        }
        snprintf(c->contact[i].name, sizeof c->contact[i].name, "%s", tc->name);
        snprintf(c->contact[i].body_a, sizeof c->contact[i].body_a, "%s", tc->body_a);
        snprintf(c->contact[i].body_b, sizeof c->contact[i].body_b, "%s", tc->body_b);
        c->contact[i].model = tc->model;
        c->contact[i].mesh_body_a = ca[i], c->contact[i].mesh_body_b = cb[i];
        c->contact[i].conductance = contact_conductance(tc);
        c->contact[i].area = top.area;
        c->contact[i].faces = top.nfaces;
        contact_topology_free(&top);
        any_split |= tc->model != CONTACT_PERFECT;
    }
    c->ncontacts = p->ncontacts;
    if (any_split) {
        ContactSplit sp;
        char serr[512];
        if (!contact_split(hm, c->xyz, c->conn, p->ncontacts, p->contacts, ca, cb, &sp, serr, sizeof serr)) {
            issue(errors, NV_ERR_GEOMETRY_INVALID, "check the interface with interface_define", "%s", serr);
            return false;
        }
        free(c->xyz);
        c->xyz = sp.xyz, sp.xyz = NULL;
        c->tnode_mesh = sp.tnode_mesh, sp.tnode_mesh = NULL;
        c->interfaces = sp.pairs, sp.pairs = NULL;
        c->interface_contact = sp.pair_contact, sp.pair_contact = NULL;
        c->ninterface = sp.npairs;
        nn = sp.nnodes;
        c->nnodes = nn;
        contact_split_free(&sp);
    }
    if (!c->tnode_mesh) {
        c->tnode_mesh = malloc((size_t)(nn ? nn : 1) * sizeof(int));
        if (!c->tnode_mesh) {
            issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
            return false;
        }
        for (int i = 0; i < nn; i++) c->tnode_mesh[i] = i;
    }
    /* bodies that touch without a declared interface are bonded perfectly: say so */
    for (int a = 0; a < p->mesh.nbodies; a++)
        for (int b = a + 1; b < p->mesh.nbodies; b++) {
            bool declared = false;
            for (int i = 0; i < p->ncontacts; i++) declared |= (ca[i] == a && cb[i] == b) || (ca[i] == b && cb[i] == a);
            if (declared) continue;
            /* a conjugate study's fluid-solid contact is the resolved interface, not an undeclared bond */
            if (s->cht && (!strcmp(p->mesh.body_name[a], s->fluid_body) || !strcmp(p->mesh.body_name[b], s->fluid_body))) continue;
            ContactTopology top;
            if (!contact_topology(hm, a, b, &top)) continue;
            if (top.nfaces > 0)
                warn(warnings, "UNDECLARED_CONTACT",
                     "'%s' and '%s' share %d mesh faces (%.4g mm2) and are treated as perfectly bonded, thermally and mechanically; declare an interface "
                     "with interface_define to state this or to model a contact resistance",
                     p->mesh.body_name[a], p->mesh.body_name[b], top.nfaces, 1e6 * top.area);
            if (top.overlap_cells > 0)
                warn(warnings, "OVERLAPPING_BODIES", "'%s' and '%s' overlap in %d mesh cells, which were given to one of them", p->mesh.body_name[a],
                     p->mesh.body_name[b], top.overlap_cells);
            contact_topology_free(&top);
        }
    c->elem_mat = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    c->fixed = calloc((size_t)(nn ? nn : 1), 1);
    c->fixed_T = calloc((size_t)(nn ? nn : 1), sizeof(double));
    c->elem_source = calloc((size_t)(ne ? ne : 1), sizeof(double));
    for (int i = 0; i < SA_MAX_BCS; i++) c->bc_body[i] = -1;
    if (!c->xyz || !c->conn || !c->elem_body || !c->face_elem || !c->face_local || !c->elem_mat || !c->fixed || !c->fixed_T || !c->elem_source) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, "use a coarser mesh", "out of memory copying the mesh (%d nodes, %d elements)", nn, ne);
        return false;
    }

    /* materials per body (and for the build plate when it is meshed) */
    int slot_of_body[MESH_MAX_BODIES], plate_slot = -1;
    for (int i = 0; i < MESH_MAX_BODIES; i++) slot_of_body[i] = -1;
    for (int bi = 0; bi < p->mesh.nbodies; bi++) slot_of_body[bi] = material_slot(p, c, p->mesh.body_name[bi], errors, warnings);
    if (p->mesh.include_plate) plate_slot = material_slot(p, c, "build_plate", errors, warnings);
    for (int e = 0; e < ne; e++) {
        int bi = hm->body[e];
        int slot = bi >= 0 && bi < MESH_MAX_BODIES ? slot_of_body[bi] : plate_slot;
        c->elem_mat[e] = slot >= 0 ? slot : 0;
    }
    if (json_len(errors) != nerr0) return false;
    if (s->cht && !cht_prepare(p, c, hm, slot_of_body, errors, warnings)) return false;

    /* selections on the mesh, for result queries */
    for (int i = 0; i < p->nselections && c->nsets < SA_MAX_SETS; i++) {
        Selection *sel = &p->selections[i];
        int n = sel->stale ? 0 : selection_mesh_faces(p, sel, NULL, 0, NULL);
        if (n <= 0) continue;
        SaSet *st = &c->set[c->nsets];
        st->faces = malloc((size_t)n * sizeof(int));
        if (!st->faces) continue;
        selection_mesh_faces(p, sel, st->faces, n, NULL);
        st->nfaces = n;
        st->nnodes = face_nodes(c, st->faces, n, &st->nodes);
        if (st->nnodes < 0) {
            free(st->faces), free(st->nodes);
            memset(st, 0, sizeof *st);
            continue;
        }
        snprintf(st->name, sizeof st->name, "%s", sel->name);
        c->nsets++;
    }

    /* thermal conditions */
    int face_cap = 0;
    for (int i = 0; i < p->nbcs; i++) {
        BcKind k = p->bcs[i].kind;
        if (k == BC_HEAT_FLUX || k == BC_CONVECTION || k == BC_RADIATION) face_cap += nf;
    }
    if (s->cht) face_cap += nf; /* the open inlet and outlet faces */
    if (face_cap > 0) {
        c->tfaces = malloc((size_t)face_cap * sizeof(ThermalFace));
        c->tface_bc = malloc((size_t)face_cap * sizeof(int));
        c->tface_base = malloc((size_t)face_cap * sizeof(double));
        if (!c->tfaces || !c->tface_bc || !c->tface_base) {
            issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
            return false;
        }
    }
    c->fixed_bc = malloc((size_t)(nn ? nn : 1) * sizeof(int));
    c->fixed_base = calloc((size_t)(nn ? nn : 1), 1);
    if (!c->fixed_bc || !c->fixed_base) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return false;
    }
    for (int i = 0; i < nn; i++) c->fixed_bc[i] = -1;
    int *owner = malloc((size_t)(nn ? nn : 1) * sizeof(int));
    if (!owner) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory");
        return false;
    }
    for (int i = 0; i < nn; i++) owner[i] = -1;
    int nmech = 0, nthermal = 0;
    for (int i = 0; i < p->nbcs; i++) {
        const BoundaryCondition *bc = &p->bcs[i];
        if (!bc_is_thermal(bc->kind)) {
            nmech++;
            continue;
        }
        nthermal++;
        if (c->nbc == SA_MAX_BCS) {
            issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "more than %d boundary conditions", SA_MAX_BCS);
            break;
        }
        SaBcInfo *info = &c->bc[c->nbc];
        memset(info, 0, sizeof *info);
        snprintf(info->name, sizeof info->name, "%s", bc->name);
        info->kind = bc->kind;
        c->nsched[c->nbc] = bc->nschedule;
        for (int k = 0; k < bc->nschedule && k < BC_SCHEDULE_MAX; k++) c->sched_t[c->nbc][k] = bc->schedule_t[k], c->sched_f[c->nbc][k] = bc->schedule_factor[k];
        if (bc->nschedule > 0) c->scheduled = true;
        if (bc->kind == BC_HEAT_SOURCE) {
            int bi = -1;
            for (int b = 0; b < p->mesh.nbodies; b++)
                if (!strcmp(p->mesh.body_name[b], bc->body)) bi = b;
            if (bi < 0) {
                issue(errors, NV_ERR_NOT_FOUND, "the body must be part of the mesh", "heat source '%s' refers to body '%s', which is not meshed", bc->name,
                      bc->body);
                continue;
            }
            double vol = 0;
            for (int e = 0; e < ne; e++) {
                if (hm->body[e] != bi) continue;
                c->elem_source[e] += bc->magnitude;
                vol += hm->h[0] * hm->h[1] * hm->h[2];
            }
            c->source_bc[c->nsources] = c->nbc, c->source_body[c->nsources] = bi, c->source_magnitude[c->nsources] = bc->magnitude;
            c->nsources++;
            c->bc_body[c->nbc] = bi;
            info->requested[0] = bc->magnitude * vol;
            info->applied[0] = info->requested[0];
            snprintf(info->selection, sizeof info->selection, "%s", bc->body);
            c->nbc++;
            continue;
        }
        Target t;
        if (!resolve(p, bc, &t, errors)) {
            free(t.faces);
            continue;
        }
        snprintf(info->selection, sizeof info->selection, "%s", bc->selection);
        for (int b = 0; b < p->mesh.nbodies; b++)
            if (t.sel && !strcmp(p->mesh.body_name[b], t.sel->body)) c->bc_body[c->nbc] = b;
        info->faces = t.nfaces;
        info->mesh_area = t.mesh_area;
        info->stl_area = t.sel->area;
        int *nodes = NULL;
        int nnod = face_nodes(c, t.faces, t.nfaces, &nodes);
        info->nodes = nnod > 0 ? nnod : 0;
        if (bc->kind == BC_TEMPERATURE) {
            int conflicts = 0, other = -1;
            for (int a = 0; a < nnod; a++) {
                int nd = nodes[a];
                if (c->fixed[nd] && fabs(c->fixed_T[nd] - bc->magnitude) > 1e-9) {
                    if (!conflicts) other = owner[nd];
                    conflicts++;
                } else {
                    c->fixed[nd] = 1;
                    c->fixed_T[nd] = bc->magnitude;
                    owner[nd] = i;
                    c->fixed_bc[nd] = c->nbc;
                }
            }
            if (conflicts && other >= 0)
                issue(errors, NV_ERR_CONFLICTING_BC, "remove one of the conditions or give them the same temperature",
                      "'%s' and '%s' prescribe different temperatures on %d shared nodes", bc->name, p->bcs[other].name, conflicts);
            info->requested[0] = bc->magnitude - 273.15; /* degC, for reporting */
        } else {
            unsigned char kind = bc->kind == BC_HEAT_FLUX ? THERMAL_FLUX : (bc->kind == BC_CONVECTION ? THERMAL_CONVECTION : THERMAL_RADIATION);
            for (int f = 0; f < t.nfaces; f++) {
                int fc = t.faces[f];
                c->tface_bc[c->ntfaces] = c->nbc;
                c->tface_base[c->ntfaces] = bc->magnitude;
                c->tfaces[c->ntfaces++] = (ThermalFace){c->face_elem[fc], c->face_local[fc], kind, bc->magnitude, bc->ambient};
            }
            if (bc->kind == BC_HEAT_FLUX) info->requested[0] = bc->magnitude * t.mesh_area;
            else if (bc->kind == BC_CONVECTION) info->requested[0] = bc->magnitude * t.mesh_area * (bc->ambient - s->initial_temperature);
            else {
                double Ta = bc->ambient, T0 = s->initial_temperature;
                info->requested[0] = bc->magnitude * THERMAL_SIGMA * t.mesh_area * (Ta * Ta * Ta * Ta - T0 * T0 * T0 * T0);
            }
        }
        free(nodes), free(t.faces);
        c->nbc++;
    }
    free(owner);
    if (json_len(errors) != nerr0) return false;
    if (s->cht && !cht_open_faces(c, hm, errors)) return false;

    bool has_fixed = false;
    for (int n = 0; n < nn && !has_fixed; n++) has_fixed = c->fixed[n] != 0;
    if (!nthermal)
        warn(warnings, "NO_THERMAL_CONDITIONS",
             "the model has no thermal conditions: it stays at its initial temperature. Apply temperature, heat_flux, convection, radiation or heat_source "
             "conditions with boundary_apply");
    if (nmech && !s->mechanical)
        warn(warnings, "MECHANICAL_CONDITIONS_IGNORED", "%d mechanical conditions are part of the setup but do not act in a thermal analysis", nmech);
    (void)has_fixed;

    memcpy(c->fixed_base, c->fixed, (size_t)nn);

    /* event schedule: stored times, condition changes and the end. Stored frames are events, so a frame is always the
     * state at exactly its time, never the state of whatever step happened to be nearest. */
    events_init(&c->events, 0, s->end_time);
    c->nsteps = s->stepping == THERMAL_STEPPING_FIXED ? (int)ceil(s->end_time / s->time_step - 1e-9) : 0;
    bool ok_ev = true;
    if (s->noutput_times > 0) {
        for (int i = 0; i < s->noutput_times; i++) {
            double t = s->output_times[i];
            if (!(t > 0) || t > s->end_time * (1 + 1e-12)) {
                issue(errors, NV_ERR_OUT_OF_RANGE, "output times must lie after 0 and not after end_time", "output time %g s is outside (0, %g] s", t,
                      s->end_time);
                return false;
            }
            ok_ev &= events_add(&c->events, t, EVENT_OUTPUT);
        }
    } else if (s->output_interval > 0) {
        double k = ceil(s->end_time / s->output_interval - 1e-9);
        if (k > TC_MAX_OUTPUTS) {
            issue(errors, NV_ERR_RESOURCE_LIMIT, "use a longer output_interval", "an output interval of %g s stores %.0f times, above the limit of %d",
                  s->output_interval, k, TC_MAX_OUTPUTS);
            return false;
        }
        for (int i = 1; i <= (int)k; i++) ok_ev &= events_add(&c->events, fmin((double)i * s->output_interval, s->end_time), EVENT_OUTPUT);
    } else if (s->stepping == THERMAL_STEPPING_FIXED) {
        int every = s->output_every > 0 ? s->output_every : 1;
        if (c->nsteps / every + 1 > TC_MAX_OUTPUTS) {
            issue(errors, NV_ERR_RESOURCE_LIMIT, "increase output_every, give output_interval, or shorten the analysis", "%d stored times exceed the limit of %d",
                  c->nsteps / every + 1, TC_MAX_OUTPUTS);
            return false;
        }
        for (int k = every; k < c->nsteps; k += every) ok_ev &= events_add(&c->events, (double)k * s->time_step, EVENT_OUTPUT);
    } else {
        for (int i = 1; i <= 20; i++) ok_ev &= events_add(&c->events, s->end_time * i / 20.0, EVENT_OUTPUT);
    }
    ok_ev &= events_add(&c->events, s->end_time, EVENT_OUTPUT); /* the end state is always stored */
    for (int b = 0; b < c->nbc; b++)
        for (int k = 1; k < c->nsched[b]; k++)
            if (c->sched_t[b][k] < s->end_time) ok_ev &= events_add(&c->events, c->sched_t[b][k], EVENT_LOAD);
    ok_ev &= events_finalize(&c->events);
    if (!ok_ev) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory for the event schedule");
        return false;
    }
    c->noutputs = 1;
    for (int i = 0; i < c->events.n; i++)
        if (c->events.ev[i].kinds & EVENT_OUTPUT) c->noutputs++;
    if (c->noutputs > TC_MAX_OUTPUTS) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, "store fewer times", "%d stored times exceed the limit of %d", c->noutputs, TC_MAX_OUTPUTS);
        return false;
    }
    double bytes = (double)c->noutputs * nn * 8 * (s->mechanical ? 5 : 1);
    if (bytes > 512e6) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, "increase output_every, shorten the analysis or coarsen the mesh",
              "storing %d times of %d nodes needs %.1f GB", c->noutputs, nn, bytes / 1e9);
        return false;
    }
    c->times = calloc((size_t)c->noutputs, sizeof(double));
    c->T = calloc((size_t)c->noutputs * (size_t)nn, sizeof(double));
    c->tmin = calloc((size_t)c->noutputs, sizeof(double));
    c->tmax = calloc((size_t)c->noutputs, sizeof(double));
    if (!c->times || !c->T || !c->tmin || !c->tmax) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory for %d stored temperature fields", c->noutputs);
        return false;
    }
    if (s->mechanical) {
        StaticSettings ss = {s->formulation, s->solver, s->pcg_tol, s->reference_temperature, s->cht && c->fluid_body < 32 ? 1u << c->fluid_body : 0};
        if (!static_model_build(p, &ss, &c->mech, errors, warnings)) return false;
        c->has_mech = true;
        c->mech_u = calloc((size_t)c->noutputs * 3 * (size_t)nn, sizeof(double));
        c->mech_vm = calloc((size_t)c->noutputs * (size_t)nn, sizeof(double));
        c->mech_peak = calloc((size_t)c->noutputs, sizeof(double));
        c->mech_umax = calloc((size_t)c->noutputs, sizeof(double));
        if (!c->mech_u || !c->mech_vm || !c->mech_peak || !c->mech_umax) {
            issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory for the mechanical results");
            return false;
        }
    }
    c->last_checkpoint_time = -1;
    if (json_len(errors) == nerr0) thermal_case_hashes(c, &c->hashes);
    return json_len(errors) == nerr0;
}

void thermal_case_free(void *vc) {
    ThermalCase *c = vc;
    if (!c) return;
    thermal_frames_close(c);
    thermal_checkpoint_free(c->resume);
    free(c->xyz), free(c->conn), free(c->elem_mat), free(c->elem_body), free(c->face_elem), free(c->face_local);
    free(c->fixed), free(c->fixed_T), free(c->elem_source), free(c->tfaces);
    free(c->tface_bc), free(c->tface_base), free(c->fixed_bc), free(c->fixed_base);
    free(c->tnode_mesh), free(c->interface_contact), free(c->interfaces);
    free(c->step_t), free(c->step_dt), free(c->step_err);
    events_free(&c->events);
    for (int i = 0; i < c->nsets; i++) free(c->set[i].nodes), free(c->set[i].faces);
    free(c->times), free(c->T), free(c->tmin), free(c->tmax), free(c->elem_birth), free(c->elem_death), free(c->elem_group);
    free(c->hang_node), free(c->hang_nmaster), free(c->hang_master), free(c->hang_owner), free(c->hang_weight);
    free(c->elem_stiff_scale), free(c->elem_D_of), free(c->support_D), free(c->support_band_props), free(c->elem_band);
    free(c->mech_u), free(c->mech_vm), free(c->mech_peak), free(c->mech_umax);
    if (c->has_mech) static_model_free(&c->mech);
    json_free(c->build_warnings);
    json_free(c->summary);
    json_free(c->orchestration);
    free(c->advect), free(c->node_part), free(c->velocity), free(c->flow_solid), free(c->flow_corner);
    free(c);
}

JsonValue *thermal_case_model_json(const ThermalCase *c) {
    JsonValue *o = json_object();
    json_set_string(o, "analysis", c->settings.cht ? "conjugate_heat_transfer" : (c->settings.mechanical ? "thermomechanical" : "transient_thermal"));
    json_set_int(o, "nodes", c->nnodes);
    if (c->settings.cht) {
        const ThermalSettings *s = &c->settings;
        JsonValue *fl = json_set_object(o, "flow");
        json_set_string(fl, "fluid_body", s->fluid_body);
        json_set_string(fl, "direction", "+x: enters at the fluid body's x-min plane, leaves at its x-max plane");
        json_set_number(fl, "inlet_velocity_m_s", s->inlet_velocity);
        json_set_number(fl, "inlet_temperature_c", s->inlet_temperature - 273.15);
        json_set(fl, "lattice_cells", json_vec3(c->flow_n[0], c->flow_n[1], c->flow_n[2]));
        json_set_number(fl, "cell_size_mm", 1e3 * c->flow_dx);
        json_set_number(fl, "inlet_area_mm2", 1e6 * c->inlet_area);
        json_set_number(fl, "conjugate_interface_area_mm2", 1e6 * c->interface_area);
        json_set_int(fl, "conjugate_interface_nodes", c->ninterface_nodes);
        static const char *const WALL[3] = {"no_slip", "slip", "periodic"};
        JsonValue *ws = json_set_object(fl, "walls");
        static const char *const SIDE[4] = {"y_min", "y_max", "z_min", "z_max"};
        for (int w = 0; w < 4; w++) json_set_string(ws, SIDE[w], WALL[s->flow_wall[w] >= 0 && s->flow_wall[w] < 3 ? s->flow_wall[w] : 0]);
        json_set_string(fl, "coupling", s->coupling_monolithic ? "monolithic: fluid and solids in one system" : "partitioned: fluid and solids iterated to convergence every step");
    }
    json_set_int(o, "elements", c->nelems);
    json_set_string(o, "mesh_hash", c->mesh_hash);
    json_set(o, "element_size_mm", json_vec3(1e3 * c->h[0], 1e3 * c->h[1], 1e3 * c->h[2]));
    JsonValue *t = json_set_object(o, "time");
    bool adaptive = c->settings.stepping == THERMAL_STEPPING_ADAPTIVE;
    json_set_number(t, "end_s", c->settings.end_time);
    json_set_string(t, "stepping", adaptive ? "adaptive" : "fixed");
    if (!adaptive || c->settings.time_step > 0) json_set_number(t, adaptive ? "initial_step_s" : "step_s", c->settings.time_step);
    if (adaptive && !c->work.accepted) json_set_string(t, "steps", "decided by the error control during the run");
    else json_set_int(t, "steps", c->nsteps);
    json_set_int(t, "stored_times", c->noutputs);
    int nload = 0;
    for (int i = 0; i < c->events.n; i++) nload += (c->events.ev[i].kinds & EVENT_LOAD) != 0;
    if (nload) json_set_int(t, "condition_changes", nload);
    json_set_string(t, "integration", c->settings.theta >= 0.999 ? "backward Euler" : (fabs(c->settings.theta - 0.5) < 1e-9 ? "Crank-Nicolson" : "theta method"));
    json_set_string(t, "capacity", c->settings.consistent_capacity ? "consistent" : "lumped");
    json_set_number(o, "initial_temperature_c", c->settings.initial_temperature - 273.15);
    if (c->settings.mechanical) json_set_number(o, "stress_free_temperature_c", c->settings.reference_temperature - 273.15);
    JsonValue *mats = json_set_array(o, "materials");
    for (int i = 0; i < c->nmat; i++) {
        JsonValue *mo = json_object();
        json_set_string(mo, "target", c->mat_id[i]);
        json_set_string(mo, "material", c->rec[i].id);
        json_set_string(mo, "status", c->mat_status[i]);
        json_set_number(mo, "conductivity_at_20c", thermal_table_eval(&c->tmat[i].k, 293.15));
        json_set_number(mo, "specific_heat_at_20c", thermal_table_eval(&c->tmat[i].cp, 293.15));
        json_set_number(mo, "density_at_20c", thermal_table_eval(&c->tmat[i].rho, 293.15));
        json_push(mats, mo);
    }
    JsonValue *bcs = json_set_array(o, "thermal_conditions");
    for (int i = 0; i < c->nbc; i++) {
        const SaBcInfo *b = &c->bc[i];
        JsonValue *bo = json_object();
        json_set_string(bo, "name", b->name);
        json_set_string(bo, "kind", bc_kind_name(b->kind));
        if (b->selection[0]) json_set_string(bo, "applies_to", b->selection);
        if (b->faces) {
            json_set_int(bo, "mesh_faces", b->faces);
            json_set_number(bo, "mesh_area_mm2", 1e6 * b->mesh_area);
            json_set_number(bo, "stl_area_mm2", 1e6 * b->stl_area);
        }
        if (b->kind == BC_TEMPERATURE) json_set_number(bo, "temperature_c", b->requested[0]);
        else json_set_number(bo, "nominal_power_w", b->requested[0]);
        json_push(bcs, bo);
    }
    json_set_int(o, "boundary_faces", c->ntfaces);
    return o;
}
