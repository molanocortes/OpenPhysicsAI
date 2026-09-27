/* thermal_run.c - the transient thermal (and thermomechanical) job: time stepping through the shared orchestrator, with
 * the thermal model as a transient participant and the structure as a quasi-static one evaluated at the stored times,
 * and the run summary with the energy balance of every step */
#include "../core/sha256.h"
#include "../fem/dense.h"
#include "../fem/flow.h"
#include "../fem/hex8.h"
#include "../fem/orchestrator.h"
#include "../fem/thermal_participant.h"
#include "../threads.h"
#include "contact.h"
#include "transient_analysis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static ThermalTable as_table(const MatTable *t) { return (ThermalTable){t->n, t->t, t->v}; }

/* structural solve for the stored frame out_index from the nodal temperature T: thermal strain from the stress-free
 * temperature, modulus at the local temperature. A conjugate study leaves the fluid out: its elements carry no stiffness,
 * and nodes used only by fluid elements are held so that the system stays regular (their displacement is not a result). */
static bool solve_mechanical(ThermalCase *c, int out_index, const double *T, ThreadPool *pool, char *err, size_t errlen) {
    StaticModel *m = &c->mech;
    int nn = c->nnodes, ne = c->nelems, nm = m->nnodes;
    int nsel = 0;
    for (int e = 0; e < ne; e++) nsel += !(c->advect && c->advect[e]);
    double *eps0 = calloc(6 * (size_t)(nsel ? nsel : 1), sizeof(double)), *scale = malloc((size_t)(nsel ? nsel : 1) * sizeof(double));
    int *conn = c->advect ? malloc(8 * (size_t)(nsel ? nsel : 1) * sizeof(int)) : NULL, *emat = c->advect ? malloc((size_t)(nsel ? nsel : 1) * sizeof(int)) : NULL;
    unsigned char *fixed = c->advect ? malloc(3 * (size_t)nm) : NULL;
    if (!eps0 || !scale || (c->advect && (!conn || !emat || !fixed))) {
        free(eps0), free(scale), free(conn), free(emat), free(fixed);
        snprintf(err, errlen, "out of memory for the thermal strain field");
        return false;
    }
    int q = 0;
    for (int e = 0; e < ne; e++) {
        if (c->advect && c->advect[e]) continue;
        double Te = 0;
        for (int a = 0; a < 8; a++) Te += 0.125 * T[c->conn[8 * (size_t)e + a]];
        const MaterialRecord *rec = &c->rec[c->elem_mat[e]];
        ThermalTable alpha = as_table(&rec->prop[MATP_ALPHA]), emod = as_table(&rec->prop[MATP_E]);
        double eth = alpha.n ? thermal_table_integral(&alpha, c->settings.reference_temperature, Te) : 0;
        for (int k = 0; k < 3; k++) eps0[6 * (size_t)q + k] = eth;
        double Eref = m->mat[m->elem_mat[e]].E, ET = emod.n ? thermal_table_eval(&emod, Te) : Eref;
        scale[q] = Eref > 0 ? ET / Eref : 1.0;
        if (c->advect) {
            memcpy(conn + 8 * (size_t)q, m->conn + 8 * (size_t)e, 8 * sizeof(int));
            emat[q] = m->elem_mat[e];
        }
        q++;
    }
    if (c->advect) {
        memcpy(fixed, m->fixed, 3 * (size_t)nm);
        unsigned char *used = calloc((size_t)(nm ? nm : 1), 1);
        if (!used) {
            free(eps0), free(scale), free(conn), free(emat), free(fixed);
            snprintf(err, errlen, "out of memory");
            return false;
        }
        for (int k = 0; k < 8 * nsel; k++) used[conn[k]] = 1;
        for (int n = 0; n < nm; n++)
            if (!used[n]) fixed[3 * (size_t)n] = fixed[3 * (size_t)n + 1] = fixed[3 * (size_t)n + 2] = 1;
        free(used);
    }
    /* element temperatures come from the thermal connectivity (each side of a split interface has its own), the
     * structure is solved on the mesh, where the bodies stay bonded */
    HexModel hm = {nm, nsel, m->xyz, c->advect ? conn : m->conn, c->advect ? emat : m->elem_mat, m->nmat, m->mat, c->settings.formulation, scale};
    SolidLoads L = {c->advect ? fixed : m->fixed, m->fixed_value, m->nodal_force, eps0, {m->gravity[0], m->gravity[1], m->gravity[2]}};
    SolidOptions so = {c->settings.solver, c->settings.pcg_tol, 0, 0, pool, NULL, NULL};
    SolidResult r;
    bool ok = solid_solve(&hm, &L, &so, &r, err, errlen);
    if (ok) {
        double *u = c->mech_u + (size_t)out_index * 3 * (size_t)nn, *vm = c->mech_vm + (size_t)out_index * (size_t)nn;
        double peak = 0, umax = 0;
        for (int n = 0; n < nn; n++) { /* mesh results on every thermal node, twins included */
            int qn = c->tnode_mesh ? c->tnode_mesh[n] : n;
            bool fluid_only = c->node_part && c->node_part[n] == 2;
            memcpy(u + 3 * (size_t)n, r.u + 3 * (size_t)qn, 3 * sizeof(double));
            vm[n] = fluid_only ? 0 : von_mises(r.node_stress + 6 * (size_t)qn);
            if (fluid_only) continue;
            peak = fmax(peak, vm[n]);
            const double *un = r.u + 3 * (size_t)qn;
            umax = fmax(umax, sqrt(un[0] * un[0] + un[1] * un[1] + un[2] * un[2]));
        }
        c->mech_peak[out_index] = peak;
        c->mech_umax[out_index] = umax;
        solid_result_free(&r);
    }
    free(eps0), free(scale), free(conn), free(emat), free(fixed);
    return ok;
}

/* ---- the structure as a quasi-static participant ----------------------------------------------------------------
 * Linear elastic, temperature-dependent modulus, thermal strain: no history, so the orchestrator evaluates it only at
 * the stored times, which is exact for the stored results. It imports the temperature and exports nothing: the
 * coupling is one-way. */
typedef struct {
    ThermalCase *c;
    ThreadPool *pool;
    double *T;               /* nodal temperature in the case's numbering */
    const int *node_global;  /* the exporter's local -> case node numbers (a partitioned study), NULL: case numbering */
    int nimport;             /* values imported */
    int index;               /* the stored frame the next evaluation belongs to */
} StructurePart;

static int structure_size(void *self, const char *field) {
    StructurePart *sp = self;
    return !strcmp(field, "temperature") ? sp->nimport : -1;
}

static bool structure_import(void *self, const char *field, const double *in) {
    StructurePart *sp = self;
    if (strcmp(field, "temperature")) return false;
    if (!sp->node_global) memcpy(sp->T, in, (size_t)sp->nimport * sizeof(double));
    else
        for (int i = 0; i < sp->nimport; i++) sp->T[sp->node_global[i]] = in[i];
    return true;
}

static bool structure_evaluate(void *self, double t, char *err, size_t errlen) {
    StructurePart *sp = self;
    (void)t;
    return solve_mechanical(sp->c, sp->index, sp->T, sp->pool, err, errlen);
}

static double wall_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* ---- the flow as a quasi-static, time-independent participant ------------------------------------------------------
 * The steady laminar flow of a constant-property fluid with fixed boundary conditions does not change in time and does
 * not depend on the temperature, so it is computed once at the start and exported to the fluid's energy equation. A
 * resumed run restores it from the checkpoint instead of recomputing it. */
typedef struct {
    ThermalCase *c;
    ThreadPool *pool;
    const int *node_global; /* the importer's local -> case node numbers, NULL: case numbering */
    int nnodes;             /* importer nodes */
} FlowPart;

static int flow_size(void *self, const char *field) {
    FlowPart *fp = self;
    return !strcmp(field, "velocity") ? 3 * fp->nnodes : -1;
}

static bool flow_export(void *self, const char *field, bool trial, double *out) {
    FlowPart *fp = self;
    (void)trial;
    if (strcmp(field, "velocity")) return false;
    if (!fp->node_global) memcpy(out, fp->c->velocity, 3 * (size_t)fp->nnodes * sizeof(double));
    else
        for (int i = 0; i < fp->nnodes; i++) memcpy(out + 3 * (size_t)i, fp->c->velocity + 3 * (size_t)fp->node_global[i], 3 * sizeof(double));
    return true;
}

static bool flow_evaluate(void *self, double t, char *err, size_t errlen) {
    FlowPart *fp = self;
    ThermalCase *c = fp->c;
    const ThermalSettings *s = &c->settings;
    (void)t;
    if (c->flow.done) return true;
    int slot = -1;
    for (int e = 0; e < c->nelems && slot < 0; e++)
        if (c->advect[e]) slot = c->elem_mat[e];
    double nu = c->rec[slot].prop[MATP_VISCOSITY].v[0] / c->tmat[slot].rho.v[0];
    FlowSpec spec = {c->flow_n[0], c->flow_n[1], c->flow_n[2], c->flow_dx, c->flow_solid, s->inlet_velocity, nu,
                     {(FlowWall)s->flow_wall[0], (FlowWall)s->flow_wall[1], (FlowWall)s->flow_wall[2], (FlowWall)s->flow_wall[3]},
                     s->flow_steady_tolerance, 0, fp->pool};
    double w0 = wall_now();
    FlowField f;
    if (!flow_solve(&spec, &f, err, errlen)) return false;
    size_t ncorner = (size_t)(c->flow_n[0] + 1) * (size_t)(c->flow_n[1] + 1) * (size_t)(c->flow_n[2] + 1);
    double *corner = malloc(3 * ncorner * sizeof(double));
    FlowMapStats ms = {0};
    bool ok = corner && flow_nodal_velocity(&spec, &f, corner, &ms, err, errlen);
    if (!corner) snprintf(err, errlen, "out of memory mapping the flow");
    if (ok) {
        for (int q = 0; q < c->nnodes; q++)
            if (c->flow_corner[q] >= 0) memcpy(c->velocity + 3 * (size_t)q, corner + 3 * (size_t)c->flow_corner[q], 3 * sizeof(double));
        c->flow.done = true;
        c->flow.tau = f.tau, c->flow.u_lattice = f.u_lattice, c->flow.mach = f.mach, c->flow.reynolds_cell = f.reynolds_cell, c->flow.dt = f.dt;
        c->flow.steps = f.steps, c->flow.physical_time = f.physical_time, c->flow.change = f.change;
        c->flow.density_min = f.density_min, c->flow.density_max = f.density_max, c->flow.inlet_flux = f.inlet_flux, c->flow.outlet_flux = f.outlet_flux;
        c->flow.scale_min = ms.scale_min, c->flow.scale_max = ms.scale_max, c->flow.seconds = wall_now() - w0;

        int D = 1 << 30; /* the hydraulic length: the narrowest walled cross direction */
        if (s->flow_wall[0] != FLOW_WALL_PERIODIC) D = c->flow_n[1];
        if (s->flow_wall[2] != FLOW_WALL_PERIODIC && c->flow_n[2] < D) D = c->flow_n[2];
        c->flow.reynolds_hydraulic = D < (1 << 30) ? s->inlet_velocity * D * c->flow_dx / nu : -1;
    }
    free(corner);
    flow_field_free(&f);
    return ok;
}

/* ---- thermal parts -------------------------------------------------------------------------------------------------
 * A thermal or thermomechanical run, and a monolithic conjugate study, step one thermal model: the whole case. A
 * partitioned conjugate study steps two: the fluid (its elements; it holds the conjugate interface nodes at the solids'
 * temperature) and the solids (every other element; they receive the fluid's interface heat as nodal power). A
 * partition has its own node and element numbering and owns its arrays; the case keeps the global description that
 * schedules and stored frames use. */
typedef struct {
    const char *name;
    ThermalCase *c;
    int role; /* 0 whole case, 1 fluid, 2 solids */
    int nn, ne, nfaces, ninterface, nshared;
    int *node_global, *elem_global, *face_global, *node_local;
    double *xyz, *fixed_T, *elem_source, *node_power, *velocity, *T0;
    int *conn, *elem_mat, *elem_group, *face_group, *node_group, *interface_group, *shared;
    unsigned char *fixed;
    ThermalFace *faces;
    ThermalInterfaceNode *interfaces;
    ThermalModel m;
    ThermalSolver *s;
    ThermalIntegrator *ti;
    ThermalParticipant tp;
    bool bound;
} ThermalPart;

static void part_free(ThermalPart *p) {
    if (p->bound) thermal_participant_release(&p->tp);
    tint_free(p->ti), thermal_free(p->s);
    free(p->node_global), free(p->elem_global), free(p->face_global), free(p->node_local), free(p->xyz), free(p->fixed_T), free(p->elem_source);
    free(p->node_power), free(p->velocity), free(p->T0), free(p->conn), free(p->elem_mat), free(p->elem_group), free(p->face_group), free(p->node_group);
    free(p->interface_group), free(p->shared), free(p->fixed), free(p->faces), free(p->interfaces);
    memset(p, 0, sizeof *p);
}

static bool part_whole(ThermalPart *p, ThermalCase *c) {
    memset(p, 0, sizeof *p);
    p->name = "thermal", p->c = c, p->role = 0, p->nn = c->nnodes, p->ne = c->nelems;
    p->elem_group = malloc((size_t)(c->nelems > 0 ? c->nelems : 1) * sizeof(int));
    p->T0 = malloc((size_t)(c->nnodes > 0 ? c->nnodes : 1) * sizeof(double));
    if (!p->elem_group || !p->T0) return false;
    for (int e = 0; e < c->nelems; e++) p->elem_group[e] = c->elem_body[e] >= 0 ? c->elem_body[e] : c->nbodies; /* the plate after the bodies */
    /* designated initialisers: the model grows as physics is added, and a positional list would go silently wrong */
    p->m = (ThermalModel){.nnodes = c->nnodes,
                          .nelems = c->nelems,
                          .xyz = c->xyz,
                          .conn = c->conn,
                          .elem_mat = c->elem_mat,
                          .nmat = c->nmat,
                          .mat = c->tmat,
                          .fixed = c->fixed,
                          .fixed_T = c->fixed_T,
                          .elem_source = c->elem_source,
                          .nfaces = c->ntfaces,
                          .faces = c->tfaces,
                          .ninterface = c->ninterface,
                          .interfaces = c->interfaces,
                          .elem_group = p->elem_group,
                          .face_group = c->tface_bc,
                          .node_group = c->fixed_bc,
                          .interface_group = c->interface_contact,
                          .velocity = c->advect ? c->velocity : NULL,
                          .advect = c->advect};
    return true;
}

/* the values of the time-dependent arrays of a partition from the case's (after apply_schedule) */
static void part_restrict(ThermalPart *p) {
    const ThermalCase *c = p->c;
    for (int e = 0; e < p->ne; e++) p->elem_source[e] = c->elem_source[p->elem_global[e]];
    for (int f = 0; f < p->nfaces; f++) p->faces[f].value = c->tfaces[p->face_global[f]].value;
    for (int n = 0; n < p->nn; n++) {
        int g = p->node_global[n];
        p->fixed[n] = c->fixed[g] || (p->role == 1 && c->node_part[g] == 3); /* the fluid holds the conjugate interface */
    }
}

static bool part_split(ThermalPart *p, ThermalCase *c, int role) {
    memset(p, 0, sizeof *p);
    p->name = role == 1 ? "fluid" : "solids", p->c = c, p->role = role;
    int nn = c->nnodes, ne = c->nelems;
    p->node_local = malloc((size_t)nn * sizeof(int));
    p->elem_global = malloc((size_t)ne * sizeof(int));
    if (!p->node_local || !p->elem_global) return false;
    for (int n = 0; n < nn; n++) p->node_local[n] = -1;
    for (int e = 0; e < ne; e++) {
        if ((c->advect[e] != 0) != (role == 1)) continue;
        p->elem_global[p->ne++] = e;
        for (int a = 0; a < 8; a++) p->node_local[c->conn[8 * (size_t)e + a]] = 0;
    }
    for (int n = 0; n < nn; n++)
        if (p->node_local[n] == 0) p->node_local[n] = p->nn++;
    size_t nl = (size_t)(p->nn ? p->nn : 1), el = (size_t)(p->ne ? p->ne : 1);
    p->node_global = malloc(nl * sizeof(int)), p->xyz = malloc(3 * nl * sizeof(double)), p->fixed = calloc(nl, 1), p->fixed_T = malloc(nl * sizeof(double));
    p->node_group = malloc(nl * sizeof(int)), p->node_power = calloc(nl, sizeof(double)), p->T0 = malloc(nl * sizeof(double));
    p->conn = malloc(8 * el * sizeof(int)), p->elem_mat = malloc(el * sizeof(int)), p->elem_group = malloc(el * sizeof(int));
    p->elem_source = malloc(el * sizeof(double));
    p->shared = malloc(nl * sizeof(int));
    if (role == 1) p->velocity = calloc(3 * nl, sizeof(double));
    if (!p->node_global || !p->xyz || !p->fixed || !p->fixed_T || !p->node_group || !p->node_power || !p->T0 || !p->conn || !p->elem_mat ||
        !p->elem_group || !p->elem_source || !p->shared || (role == 1 && !p->velocity))
        return false;
    for (int n = 0; n < nn; n++) {
        int l = p->node_local[n];
        if (l < 0) continue;
        p->node_global[l] = n;
        memcpy(p->xyz + 3 * (size_t)l, c->xyz + 3 * (size_t)n, 3 * sizeof(double));
        p->fixed_T[l] = c->fixed[n] ? c->fixed_T[n] : c->settings.initial_temperature;
        p->node_group[l] = c->fixed_bc[n];
        if (c->node_part[n] == 3) p->shared[p->nshared++] = l; /* increasing global order in both partitions */
    }
    for (int k = 0; k < p->ne; k++) {
        int e = p->elem_global[k];
        for (int a = 0; a < 8; a++) p->conn[8 * (size_t)k + a] = p->node_local[c->conn[8 * (size_t)e + a]];
        p->elem_mat[k] = c->elem_mat[e];
        p->elem_group[k] = c->elem_body[e] >= 0 ? c->elem_body[e] : c->nbodies;
    }
    int *elem_local = malloc((size_t)ne * sizeof(int));
    if (!elem_local) return false;
    for (int e = 0; e < ne; e++) elem_local[e] = -1;
    for (int k = 0; k < p->ne; k++) elem_local[p->elem_global[k]] = k;
    p->faces = malloc((size_t)(c->ntfaces > 0 ? c->ntfaces : 1) * sizeof(ThermalFace));
    p->face_global = malloc((size_t)(c->ntfaces > 0 ? c->ntfaces : 1) * sizeof(int));
    p->face_group = malloc((size_t)(c->ntfaces > 0 ? c->ntfaces : 1) * sizeof(int));
    if (!p->faces || !p->face_global || !p->face_group) {
        free(elem_local);
        return false;
    }
    for (int f = 0; f < c->ntfaces; f++) {
        int k = elem_local[c->tfaces[f].elem];
        if (k < 0) continue;
        p->faces[p->nfaces] = c->tfaces[f];
        p->faces[p->nfaces].elem = k;
        p->face_global[p->nfaces] = f;
        p->face_group[p->nfaces++] = c->tface_bc[f];
    }
    free(elem_local);
    if (role == 2 && c->ninterface > 0) { /* contact pairs join solid bodies only (conjugate study refuses them on the fluid) */
        p->interfaces = malloc((size_t)c->ninterface * sizeof(ThermalInterfaceNode));
        p->interface_group = malloc((size_t)c->ninterface * sizeof(int));
        if (!p->interfaces || !p->interface_group) return false;
        for (int i = 0; i < c->ninterface; i++) {
            int a = p->node_local[c->interfaces[i].node_a], b = p->node_local[c->interfaces[i].node_b];
            if (a < 0 || b < 0) continue;
            p->interfaces[p->ninterface] = c->interfaces[i];
            p->interfaces[p->ninterface].node_a = a, p->interfaces[p->ninterface].node_b = b;
            p->interface_group[p->ninterface++] = c->interface_contact ? c->interface_contact[i] : -1;
        }
    }
    part_restrict(p);
    for (int k = 0; k < p->nshared && role == 1; k++) {
        int l = p->shared[k], g = p->node_global[l];
        p->node_group[l] = c->fixed[g] ? c->fixed_bc[g] : -1; /* the coupling reaction is not a condition's heat */
    }
    p->m = (ThermalModel){.nnodes = p->nn,
                          .nelems = p->ne,
                          .xyz = p->xyz,
                          .conn = p->conn,
                          .elem_mat = p->elem_mat,
                          .nmat = c->nmat,
                          .mat = c->tmat,
                          .fixed = p->fixed,
                          .fixed_T = p->fixed_T,
                          .elem_source = p->elem_source,
                          .nfaces = p->nfaces,
                          .faces = p->faces,
                          .ninterface = p->ninterface,
                          .interfaces = p->interfaces,
                          .elem_group = p->elem_group,
                          .face_group = p->face_group,
                          .node_group = p->node_group,
                          .interface_group = p->interface_group,
                          .node_power = role == 2 ? p->node_power : NULL,
                          .velocity = role == 1 ? p->velocity : NULL};
    return true;
}

/* the case temperature from the parts: fluid values first, then the solids' (the interface belongs to the solids) */
static void gather_temperature(ThermalPart *parts, int nparts, double *T) {
    if (nparts == 1) {
        memcpy(T, tint_temperature(parts[0].ti), (size_t)parts[0].nn * sizeof(double));
        return;
    }
    for (int k = 0; k < nparts; k++) {
        const double *Tp = tint_temperature(parts[k].ti);
        for (int l = 0; l < parts[k].nn; l++) T[parts[k].node_global[l]] = Tp[l];
    }
}

/* the integrator's failure classes, for which the failure details and recovery options are written */
static ThermalIntegratorStatus thermal_status(OrchStatus st, const ThermalParticipant *tp) {
    switch (st) {
    case ORCH_FAIL_MIN_STEP: return TINT_FAIL_MIN_STEP;
    case ORCH_FAIL_REJECTIONS: return TINT_FAIL_REJECTIONS;
    case ORCH_FAIL_STEP_LIMIT: return TINT_FAIL_STEP_LIMIT;
    case ORCH_FAIL_RESOURCE: return TINT_FAIL_RESOURCE;
    case ORCH_FAIL_SOLVER:
        switch (tp->trial.failure) {
        case THERMAL_FAIL_NONLINEAR: return TINT_FAIL_NONLINEAR;
        case THERMAL_FAIL_LINEAR: return TINT_FAIL_LINEAR;
        case THERMAL_FAIL_STATE: return TINT_FAIL_STATE;
        case THERMAL_FAIL_RESOURCE: return TINT_FAIL_RESOURCE;
        default: return TINT_FAIL_INPUT;
        }
    default: return TINT_FAIL_INPUT;
    }
}

static void store_output(ThermalCase *c, int index, double time, const double *T) {
    c->times[index] = time;
    memcpy(c->T + (size_t)index * (size_t)c->nnodes, T, (size_t)c->nnodes * sizeof(double));
    double lo = INFINITY, hi = -INFINITY;
    for (int n = 0; n < c->nnodes; n++) lo = fmin(lo, T[n]), hi = fmax(hi, T[n]);
    c->tmin[index] = lo, c->tmax[index] = hi;
}

static const char *method_name(double theta) {
    return theta >= 0.999 ? "backward Euler" : (fabs(theta - 0.5) < 1e-9 ? "Crank-Nicolson" : "theta method");
}

JsonValue *thermal_summary_json(const ThermalCase *c) {
    const ThermalSettings *set = &c->settings;
    bool adaptive = set->stepping == THERMAL_STEPPING_ADAPTIVE;
    JsonValue *o = json_object();
    json_set_string(o, "analysis", set->cht ? "conjugate_heat_transfer" : (set->mechanical ? "thermomechanical" : "transient_thermal"));
    json_set(o, "model", thermal_case_model_json(c));
    JsonValue *tm = json_set_object(o, "temperature");
    double lo = INFINITY, hi = -INFINITY, t_hi = 0;
    for (int i = 0; i < c->noutputs; i++) {
        lo = fmin(lo, c->tmin[i]);
        if (c->tmax[i] > hi) hi = c->tmax[i], t_hi = c->times[i];
    }
    json_set_number(tm, "min_c", lo - 273.15);
    json_set_number(tm, "max_c", hi - 273.15);
    json_set_number(tm, "max_at_s", t_hi);
    json_set_string(tm, "note", "the extremes over the stored times; a peak between stored times is not seen here");
    json_set_number(tm, "final_min_c", c->noutputs ? c->tmin[c->noutputs - 1] - 273.15 : 0);
    json_set_number(tm, "final_max_c", c->noutputs ? c->tmax[c->noutputs - 1] - 273.15 : 0);
    JsonValue *series = json_set_array(o, "history");
    for (int i = 0; i < c->noutputs; i++) {
        JsonValue *row = json_object();
        json_set_number(row, "time_s", c->times[i]);
        json_set_number(row, "min_c", c->tmin[i] - 273.15);
        json_set_number(row, "max_c", c->tmax[i] - 273.15);
        if (c->has_mech) {
            json_set_number(row, "max_displacement_mm", 1e3 * c->mech_umax[i]);
            json_set_number(row, "peak_von_mises_mpa", 1e-6 * c->mech_peak[i]);
        }
        json_push(series, row);
    }
    double supplied = c->energy_source + c->energy_boundary + c->energy_prescribed;
    if (set->cht) supplied += c->energy_advection + c->energy_inflow;
    double big = fmax(fabs(c->energy_stored), fabs(supplied));
    double closure = big > 0 ? fabs(c->energy_stored - supplied) / big : 0;
    double eb = fmax(fabs(c->energy_stored), fabs(c->energy_enthalpy));
    double mismatch = eb > 0 ? fabs(c->energy_stored - c->energy_enthalpy) / eb : 0;
    JsonValue *en = json_set_object(o, "energy");
    json_set_number(en, "sources_j", c->energy_source);
    json_set_number(en, "boundary_j", c->energy_boundary);
    json_set_number(en, "prescribed_nodes_j", c->energy_prescribed);
    json_set_number(en, "stored_j", c->energy_stored);
    json_set_number(en, "enthalpy_change_j", c->energy_enthalpy);
    if (set->cht) {
        json_set_number(en, "advected_into_fluid_j", c->energy_advection + c->energy_inflow);
        json_set_number(en, "enthalpy_in_j", c->energy_enthalpy_in);
        json_set_number(en, "enthalpy_out_j", c->energy_enthalpy_out);
        json_set_number(en, "unexplained_advective_j", c->energy_divergence);
        json_set_string(en, "advection_note",
                        "advected_into_fluid_j is the energy the velocity carried into the fluid (volume term plus the weak inflow at the open "
                        "faces); for a mass-conserving velocity it equals enthalpy_in_j - enthalpy_out_j, and unexplained_advective_j is the "
                        "difference, a measure of the mapped flow's divergence");
    }
    json_set_number(en, "supplied_j", supplied);
    json_set_number(en, "closure_error", closure);
    json_set_number(en, "worst_step_balance_error", c->worst_balance);
    /* the enthalpy change is integrated from H(T) over the elements without reference to the capacity matrix that was
     * assembled, so agreement is evidence that energy is conserved physically and not merely algebraically */
    json_set_number(en, "enthalpy_mismatch", mismatch);
    json_set_number(en, "worst_step_enthalpy_mismatch", c->worst_enthalpy_mismatch);
    json_set_string(en, "note",
                    "accepted steps only: rejected trial steps never enter these sums. The stored energy of every step must equal the heat "
                    "delivered by sources, boundary faces and prescribed-temperature nodes; enthalpy_change_j is the same energy obtained by "
                    "integrating the enthalpy function over the elements, which checks the capacity model itself rather than the linear solve");
    if (c->liquid_volume_max > 0) {
        JsonValue *ph = json_set_object(o, "phase_change");
        json_set_number(ph, "largest_molten_volume_mm3", c->liquid_volume_max * 1e9);
        json_set_string(ph, "model", "equilibrium liquid fraction, linear between solidus and liquidus; latent heat carried in the enthalpy");
    }
    JsonValue *sv = json_set_object(o, "solver");
    json_set_int(sv, "steps", c->nsteps);
    json_set_int(sv, "largest_picard_iterations", c->picard_max);
    json_set_int(sv, "total_cg_iterations", c->linear_total);

    /* ---- time integration: what was done, and what is known about its error ---- */
    const ThermalWork *w = &c->work;
    JsonValue *ti = json_set_object(o, "time_integration");
    json_set_string(ti, "stepping", adaptive ? "adaptive" : "fixed");
    json_set_string(ti, "method", method_name(set->theta));
    json_set_int(ti, "order", fabs(set->theta - 0.5) < 1e-9 ? 2 : 1);
    json_set_int(ti, "accepted_steps", w->accepted);
    json_set_int(ti, "rejected_steps", w->rejected);
    json_set_int(ti, "attempts", w->attempts);
    json_set_int(ti, "nonlinear_failures", w->nonlinear_failures);
    json_set_int(ti, "linear_failures", w->linear_failures);
    json_set_int(ti, "invalid_states", w->state_failures);
    json_set_number(ti, "smallest_step_s", w->accepted ? w->dt_min : 0);
    json_set_number(ti, "largest_step_s", w->dt_max);
    JsonValue *wk = json_set_object(ti, "work");
    json_set_int(wk, "solves", w->solves);
    json_set_int(wk, "assemblies", w->assemblies);
    json_set_int(wk, "picard_iterations", w->picard_iterations);
    json_set_int(wk, "cg_iterations", w->linear_iterations);
    json_set_number(wk, "solve_seconds", w->seconds_solve);
    json_set_number(wk, "estimate_seconds", w->seconds_estimate);
    int nload = 0, nout = 0;
    for (int i = 0; i < c->events.n; i++) nload += (c->events.ev[i].kinds & EVENT_LOAD) != 0, nout += (c->events.ev[i].kinds & EVENT_OUTPUT) != 0;
    JsonValue *evs = json_set_object(ti, "events");
    json_set_int(evs, "stored_times", nout);
    json_set_int(evs, "condition_changes", nload);
    json_set_string(evs, "note", "every stored time and every schedule change was landed on exactly");
    if (adaptive) {
        JsonValue *tol = json_set_object(ti, "tolerances");
        json_set_number(tol, "temporal_relative", set->temporal_relative);
        json_set_number(tol, "temporal_temperature_k", set->temporal_temperature);
        if (set->temporal_enthalpy > 0) json_set_number(tol, "temporal_enthalpy_j_m3", set->temporal_enthalpy);
        else json_set_string(tol, "temporal_enthalpy_j_m3", "derived: temporal_temperature_k times rho cp of each material at the initial temperature");
        json_set_string(tol, "measure", set->temporal_temperature_only ? "temperature only" : "temperature and enthalpy");
        json_set_number(tol, "nonlinear_temperature_k", set->picard_tol);
        JsonValue *ctl = json_set_object(ti, "controller");
        json_set_number(ctl, "initial_step_s", c->control.dt_initial);
        json_set_number(ctl, "min_step_s", c->control.dt_min);
        json_set_number(ctl, "max_step_s", c->control.dt_max);
        json_set_number(ctl, "safety", c->control.safety);
        json_set_number(ctl, "max_growth", c->control.max_growth);
        json_set_number(ctl, "max_shrink", c->control.max_shrink);
        json_set_int(ctl, "max_consecutive_rejections", c->control.max_rejections);
        json_set_int(ctl, "max_steps", c->control.max_steps);
        JsonValue *es = json_set_object(ti, "error_estimate");
        json_set_string(es, "method", "step doubling: one full and two half steps from the accepted state; the two-half-step solution is accepted");
        json_set_number(es, "largest_normalised_local_error", w->error_max);
        json_set_number(es, "sum_of_normalised_local_errors", w->error_sum);
        json_set_string(es, "note",
                        "a normalised local error of 1 is the tolerance of ONE step. Errors of successive steps accumulate, so the global error "
                        "can be several times the per-step tolerance: on the verification problem it was 3 to 34 times as the tolerance "
                        "tightened. For a quantitative accuracy statement repeat the run with the tolerances ten times tighter and compare.");
    } else {
        JsonValue *es = json_set_object(ti, "error_estimate");
        json_set_string(es, "method", "none: fixed steps carry no temporal error estimate");
        json_set_string(es, "note", "repeat the run with half the time step (or run adaptively) to establish the temporal error");
    }

    /* ---- how the participants were coupled: stated, so a one-way run is never read as a coupled one ---- */
    if (c->orchestration) {
        JsonValue *cp = json_clone(c->orchestration);
        if (c->has_mech) {
            json_set_string(cp, "summary",
                            "one-way thermomechanical coupling: the structure is evaluated from the computed temperature at every stored time and "
                            "nothing feeds back to the thermal model (no thermal effect of deformation, no gap or contact-pressure dependence)");
            json_set_int(cp, "stored_times_with_structural_results", c->noutputs);
            json_set_string(cp, "structural_synchronisation",
                            "at the initial time and every stored time; exact for this linear elastic structure, which has no history. A model with "
                            "history (plasticity, creep) would have to be evaluated at every accepted step");
        } else if (!set->cht) {
            json_set_string(cp, "summary", "a single transient thermal participant: nothing is coupled");
        }
        if (set->cht) {
            json_set_string(cp, "summary",
                            c->cht_partitioned
                                ? "flow -> fluid energy is one-way (the steady flow is computed once and does not depend on temperature); fluid "
                                  "and solids are strongly coupled: every step iterates the conjugate interface (the fluid holds it at the solids' "
                                  "temperature, the solids receive the fluid's interface heat) until the interface temperature stops changing"
                                : "flow -> fluid energy is one-way (the steady flow is computed once and does not depend on temperature); fluid "
                                  "and solids are solved together in one system (monolithic), so the interface needs no iteration");
            if (c->has_mech)
                json_set_string(cp, "structure", "one-way: the solids' temperature drives their thermal expansion at the stored times; nothing feeds back");
            JsonValue *st = json_set_object(cp, "statistics");
            json_set_int(st, "attempts", c->orch_work.attempts);
            json_set_int(st, "coupling_iterations", c->orch_work.coupling_iterations);
            json_set_number(st, "iterations_per_accepted_step", c->orch_work.accepted ? (double)c->orch_work.coupling_iterations / c->orch_work.accepted : 0);
            json_set_int(st, "rejected_for_coupling", c->orch_work.rejected_coupling);
            json_set_number(st, "largest_final_coupling_change", c->orch_work.coupling_residual_max);
            json_set_string(st, "note", "the coupling change is normalised by its tolerance (<= 1 converged); it is reported apart from the temporal error");
        }
        json_set(o, "coupling", cp);
    }
    if (set->cht) {
        JsonValue *fl = json_set_object(o, "flow");
        json_set_string(fl, "model",
                        "steady laminar incompressible flow of a constant-property fluid without buoyancy, from the D3Q19 lattice Boltzmann solver "
                        "(regularized recursive collision, halfway bounce-back walls, velocity inlet, pressure outlet)");
        json_set_bool(fl, "computed", c->flow.done);
        json_set_number(fl, "relaxation_time", c->flow.tau);
        json_set_number(fl, "lattice_velocity", c->flow.u_lattice);
        json_set_number(fl, "lattice_mach", c->flow.mach);
        json_set_number(fl, "reynolds_cell", c->flow.reynolds_cell);
        if (c->flow.reynolds_hydraulic > 0) json_set_number(fl, "reynolds_hydraulic", c->flow.reynolds_hydraulic);
        json_set_int(fl, "lattice_steps", c->flow.steps);
        json_set_number(fl, "physical_time_to_steady_s", c->flow.physical_time);
        json_set_number(fl, "final_relative_change", c->flow.change);
        json_set_number(fl, "density_min", c->flow.density_min);
        json_set_number(fl, "density_max", c->flow.density_max);
        json_set_number(fl, "inlet_flux_m3_s", c->flow.inlet_flux);
        json_set_number(fl, "outlet_flux_m3_s", c->flow.outlet_flux);
        json_set_number(fl, "specified_flux_m3_s", set->inlet_velocity * c->inlet_area);
        json_set_number(fl, "section_scale_min", c->flow.scale_min);
        json_set_number(fl, "section_scale_max", c->flow.scale_max);

        json_set_number(fl, "wall_seconds", c->flow.seconds);
        json_set_string(fl, "mapping",
                        "momentum density / reference density at cell centres, averaged to the mesh nodes (zero at walls and solids), then "
                        "each cross-section scaled to the specified volume flow; section_scale_* is the size of that correction. The mapped "
                        "field is not exactly divergence-free for the energy equation's elements: see energy.unexplained_advective_j");
        json_set_string(fl, "time_synchronisation",
                        "the steady flow is used from t = 0: its start-up transient is not resolved. physical_time_to_steady_s is how long the "
                        "lattice needed to reach it from rest; compare it with the thermal times of the study");
        if (c->flow.density_max - c->flow.density_min > 0.01)
            json_set_string(fl, "warning", "the lattice density varied by more than 1 %: compressibility errors of the flow are of that order");
        JsonValue *fe = json_set_object(o, "fluid_energy");
        json_set_string(fe, "equation", "rho cp (dT/dt + u . grad T) = div(k grad T) + q in the fluid (convective form)");
        json_set_string(fe, "stabilization", "SUPG with tau = [(2|u|/h)^2 + 9 (4 alpha / h^2)^2]^(-1/2), independent of the time step");
        json_set_string(fe, "linear_solver", "BiCGSTAB with ILU(0) and residual replacement (the system is nonsymmetric)");
        json_set_string(fe, "open_boundaries",
                        "x-min and x-max planes of the fluid box: outflow where the velocity leaves, weak inflow at the inlet temperature where it "
                        "enters (inlet and any backflow)");
        json_set_number(fe, "inlet_temperature_c", set->inlet_temperature - 273.15);
        double denom = fmax(fabs(c->energy_enthalpy_out), fabs(c->energy_enthalpy_in));
        json_set_number(fe, "unexplained_advective_fraction", denom > 0 ? fabs(c->energy_divergence) / denom : 0);
        /* maximum principle: without heat sources in the fluid, its temperature stays between the lowest and highest of the
         * inlet, the initial state and the solids' temperatures seen so far. Values beyond are overshoots of the discretisation. */
        bool fluid_source = false;
        for (int e = 0; e < c->nelems && !fluid_source; e++) fluid_source = c->advect && c->advect[e] && c->elem_source[e] != 0;
        if (c->node_part && !fluid_source) {
            double lo = fmin(set->inlet_temperature, set->initial_temperature), hi = fmax(set->inlet_temperature, set->initial_temperature);
            double worst = 0;
            long nodes_out = 0;
            for (int i = 0; i < c->noutputs; i++) {
                const double *T = c->T + (size_t)i * (size_t)c->nnodes;
                for (int n = 0; n < c->nnodes; n++)
                    if (c->node_part[n] & 1) lo = fmin(lo, T[n]), hi = fmax(hi, T[n]);
                for (int n = 0; n < c->nnodes; n++) {
                    if (c->node_part[n] != 2) continue;
                    double ex = T[n] > hi ? T[n] - hi : (T[n] < lo ? lo - T[n] : 0);
                    if (ex > 1e-6) nodes_out++;
                    worst = fmax(worst, ex);
                }
            }
            JsonValue *ov = json_set_object(fe, "overshoot");
            json_set_number(ov, "largest_k", worst);
            json_set_int(ov, "node_values_beyond_bounds", nodes_out);
            json_set_string(ov, "bounds",
                            "at each stored time: the inlet, the initial temperature and the solids' extremes up to that time (the maximum "
                            "principle of advection-diffusion without fluid sources); a stored-time check, not a check of every step");
        }
        JsonValue *ci = json_set_object(o, "conjugate_interface");
        json_set_number(ci, "area_mm2", 1e6 * c->interface_area);
        json_set_int(ci, "nodes", c->ninterface_nodes);
        json_set_string(ci, "model", "resolved: temperature continuity and heat through the computed flow; no convection coefficient");
        if (c->cht_partitioned) {
            json_set_number(ci, "heat_into_fluid_j", c->cht_heat_into_fluid);
            json_set_number(ci, "heat_into_solids_j", c->cht_heat_into_solid);
            double big = fmax(fabs(c->cht_heat_into_fluid), fabs(c->cht_heat_into_solid));
            json_set_number(ci, "conservation_error", big > 0 ? fabs(c->cht_heat_into_fluid + c->cht_heat_into_solid) / big : 0);
            json_set_number(ci, "largest_temperature_jump_k", c->cht_continuity);
            json_set_string(ci, "accounts",
                            "heat_into_fluid_j is the reaction of the interface nodes the fluid holds; heat_into_solids_j is the nodal heat the "
                            "solids received from it (a conservative transfer, so the two are equal and opposite). The temperature jump is "
                            "bounded by the coupling tolerance");
        } else {
            json_set_string(ci, "accounts",
                            "monolithic: fluid and solids share the interface nodes, so the heat crossing it is not a separate measurement; the "
                            "fluid body's balance in bodies[] gives it as a residual");
        }
    }

    /* ---- energy by condition, by body and by interface: two independent accounts of the interface heat ---- */
    const ThermalBudget *gb = &c->group_budget;
    double rate_div = c->last_accepted_time > 0 ? c->last_accepted_time : set->end_time;
    JsonValue *conds = json_set_array(o, "conditions");
    for (int b = 0; b < c->nbc && b < THERMAL_MAX_GROUPS; b++) {
        JsonValue *co = json_object();
        json_set_string(co, "name", c->bc[b].name);
        json_set_string(co, "kind", bc_kind_name(c->bc[b].kind));
        double ej = c->bc[b].kind == BC_TEMPERATURE ? gb->group_prescribed[b] : gb->group_boundary[b];
        if (c->bc[b].kind == BC_HEAT_SOURCE) {
            ej = 0;
            for (int k = 0; k < c->nsources; k++)
                if (c->source_bc[k] == b) ej = NAN; /* sources are counted per body, see bodies[] */
        }
        if (isfinite(ej)) {
            json_set_number(co, "energy_into_model_j", ej);
            json_set_number(co, "mean_power_w", ej / rate_div);
        }
        if (c->bc_body[b] >= 0 && c->bc_body[b] < c->nbodies) json_set_string(co, "body", c->body_name[c->bc_body[b]]);
        json_push(conds, co);
    }
    JsonValue *bods = json_set_array(o, "bodies");
    /* a body whose energy barely changes has a balance of round-off size: normalise by the model's energy scale too, so
     * that picojoules of drift in an untouched body are not reported as a 100 % error */
    double model_scale = fmax(fabs(c->energy_stored), fabs(supplied));
    for (int bi = 0; bi < c->nbodies && bi < THERMAL_MAX_GROUPS; bi++) {
        JsonValue *bo = json_object();
        json_set_string(bo, "name", c->body_name[bi]);
        double through = 0;
        for (int b = 0; b < c->nbc && b < THERMAL_MAX_GROUPS; b++)
            if (c->bc_body[b] == bi) through += c->bc[b].kind == BC_TEMPERATURE ? gb->group_prescribed[b] : (c->bc[b].kind == BC_HEAT_SOURCE ? 0 : gb->group_boundary[b]);
        double iface = 0;
        bool bonded = false;
        for (int i = 0; i < c->ncontacts; i++) {
            if (c->contact[i].model == CONTACT_PERFECT && (c->contact[i].mesh_body_a == bi || c->contact[i].mesh_body_b == bi)) bonded = true;
            if (c->contact[i].model == CONTACT_PERFECT) continue;
            if (c->contact[i].mesh_body_a == bi) iface += gb->interface_heat[i];
            if (c->contact[i].mesh_body_b == bi) iface -= gb->interface_heat[i];
        }
        double stored = gb->group_stored[bi], source = gb->group_source[bi];
        double advected = 0;
        if (set->cht && bi == c->fluid_body) {
            advected = gb->group_advection[bi] + c->energy_inflow; /* every open face belongs to the fluid */
            if (c->cht_partitioned) iface += c->cht_heat_into_fluid;
            else bonded = true;
        } else if (set->cht && c->cht_partitioned) {
            int touching = 0; /* solid bodies sharing interface nodes with the fluid */
            for (int b2 = 0; b2 < c->nbodies; b2++) {
                if (b2 == c->fluid_body) continue;
                bool t = false;
                for (int e = 0; e < c->nelems && !t; e++)
                    if (c->elem_body[e] == b2)
                        for (int a = 0; a < 8 && !t; a++) t = c->node_part[c->conn[8 * (size_t)e + a]] == 3;
                touching += t;
            }
            bool mine = false;
            for (int e = 0; e < c->nelems && !mine; e++)
                if (c->elem_body[e] == bi)
                    for (int a = 0; a < 8 && !mine; a++) mine = c->node_part[c->conn[8 * (size_t)e + a]] == 3;
            if (mine && touching == 1) iface += c->cht_heat_into_solid;
            else if (mine) bonded = true;
        } else if (set->cht) {
            bonded = true;
        }
        double residual = stored - source - through - iface - advected;
        double scale = fmax(model_scale, fmax(fabs(stored), fmax(fabs(source), fmax(fabs(through), fabs(iface)))));
        json_set_number(bo, "stored_j", stored);
        json_set_number(bo, "sources_j", source);
        json_set_number(bo, "boundary_and_prescribed_j", through);
        json_set_number(bo, "through_interfaces_j", iface);
        if (advected != 0) json_set_number(bo, "advected_j", advected);
        json_set_number(bo, "residual_j", residual);
        json_set_number(bo, "balance_error", scale > 0 ? fabs(residual) / scale : 0);
        json_set_bool(bo, "independently_closed", !bonded);
        if (bonded)
            json_set_string(bo, "note",
                            "the body is perfectly bonded to another through shared nodes, whose exchange is not a measured quantity, so its own "
                            "balance does not close by itself");
        json_push(bods, bo);
    }
    if (c->ncontacts > 0) {
        JsonValue *ifs = json_set_array(o, "interfaces");
        for (int i = 0; i < c->ncontacts; i++) {
            JsonValue *io = json_object();
            json_set_string(io, "name", c->contact[i].name);
            json_set_string(io, "model", contact_model_name((ContactModel)c->contact[i].model));
            json_set_string(io, "body_a", c->contact[i].body_a);
            json_set_string(io, "body_b", c->contact[i].body_b);
            json_set_int(io, "faces", c->contact[i].faces);
            json_set_number(io, "area_mm2", 1e6 * c->contact[i].area);
            if (c->contact[i].model == CONTACT_PERFECT) {
                json_set_string(io, "heat", "not measured: perfect contact shares nodes, so no interface flux is computed");
            } else {
                if (c->contact[i].model != CONTACT_INSULATED) json_set_number(io, "conductance_w_m2k", c->contact[i].conductance);
                json_set_number(io, "heat_b_to_a_j", gb->interface_heat[i]);
                json_set_number(io, "mean_rate_b_to_a_w", gb->interface_heat[i] / rate_div);
            }
            json_push(ifs, io);
        }
    }

    JsonValue *rs = json_set_object(o, "restart");
    json_set_int(rs, "checkpoints_written", c->checkpoint_sequence);
    JsonValue *rt = json_set_array(rs, "resumed_from_s");
    for (int i = 0; i < c->nresumes; i++) json_push(rt, json_number(c->resume_times[i]));
    json_set_string(rs, "note", "a resumed run continues the same accepted state, controller history and budget: it is the same physical run");

    /* ---- the assessment an AI must read before reporting numbers ---- */
    JsonValue *as = json_set_object(o, "assessment");
    json_set_string(as, "execution", c->status[0] ? c->status : "COMPLETED");
    JsonValue *nc = json_set_object(as, "numerical_convergence");
    json_set_string(nc, "nonlinear", "every accepted step converged its Picard iteration to the nonlinear tolerance");
    json_set_int(nc, "largest_picard_iterations", c->picard_max);
    json_set_int(nc, "failed_attempts_recovered", w->nonlinear_failures + w->linear_failures + w->state_failures);
    JsonValue *cs = json_set_object(as, "conservation");
    json_set_number(cs, "energy_closure_error", closure);
    json_set_number(cs, "enthalpy_mismatch", mismatch);
    json_set_bool(cs, "passed", closure < 1e-6 && mismatch < 1e-6);
    JsonValue *dz = json_set_object(as, "discretization_evidence");
    json_set_string(dz, "temporal", adaptive ? "estimated per step by step doubling (see time_integration.error_estimate)" : "not estimated (fixed steps)");
    json_set_string(dz, "spatial",
                    "not estimated: one mesh was solved. Establish mesh convergence by repeating the study with a finer element_size; voxel "
                    "boundaries are staircases, so surface values on curved or inclined faces depend on the element size");
    json_set(dz, "element_size_mm", json_vec3(1e3 * c->h[0], 1e3 * c->h[1], 1e3 * c->h[2]));
    JsonValue *mp = json_set_array(as, "material_provenance");
    bool demo = false;
    for (int i = 0; i < c->nmat; i++) {
        JsonValue *mo = json_object();
        json_set_string(mo, "target", c->mat_id[i]);
        json_set_string(mo, "material", c->rec[i].id);
        json_set_string(mo, "status", c->mat_status[i]);
        demo |= !strcmp(c->mat_status[i], "demonstration");
        json_push(mp, mo);
    }
    if (set->cht) {
        JsonValue *cc = json_set_object(as, "coupling_convergence");
        json_set_string(cc, "scheme", c->cht_partitioned ? "partitioned" : "monolithic");
        if (c->cht_partitioned) {
            json_set_number(cc, "largest_final_coupling_change", c->orch_work.coupling_residual_max);
            json_set_bool(cc, "converged_every_step", c->orch_work.coupling_residual_max <= 1);
        }
        json_set_string(cc, "flow", c->flow.done ? "steady state reached by the lattice (see flow.final_relative_change)" : "not computed");
    }
    json_set_string(as, "experimental_validation", "none: no comparison with measurements is part of this run");
    json_set_string(as, "reading",
                    demo ? "The job completed, which says nothing about accuracy. Demonstration materials were used, so values are indicative only."
                         : "The job completed, which says nothing about accuracy by itself: check conservation, the temporal error estimate and a "
                           "mesh refinement before quoting values.");

    json_set(o, "setup_warnings", c->build_warnings ? json_clone(c->build_warnings) : json_array());
    JsonValue *notes = json_set_array(o, "interpretation");
    if (set->cht)
        json_push(notes, json_string("Conjugate heat transfer: a steady laminar flow of a constant-property fluid, computed once and without buoyancy, "
                                     "carries the energy of the fluid body; the heat exchanged with the solids follows from the resolved temperature "
                                     "field on both sides of their shared nodes, not from a convection coefficient. The flow does not respond to "
                                     "temperature. Accuracy depends on the cells across the flow: repeat with a finer element_size."));
    json_push(notes, json_string("Heat conduction in solids with an enthalpy formulation: latent heat of melting is carried when materials state a solidus, "
                                 "a liquidus and a latent heat and phase_change is on. There is no convection inside the material: convection and "
                                 "radiation act as boundary conditions with coefficients the user supplies."));
    json_push(notes, json_string("Conductivity is integrated at the Gauss points (unless property_evaluation is element_mean), and the voxel mesh has a "
                                 "staircase boundary, so surface temperatures on curved or inclined faces depend on the element size."));
    if (c->has_mech)
        json_push(notes, json_string("The structural step is sequential (temperatures drive the structure, not the other way round) and linear elastic: it is "
                                     "evaluated at the stored times, which is exact for a material without memory. With no plasticity or element "
                                     "activation it is not a residual-stress prediction, and stresses return to zero once the part is uniformly back at "
                                     "the stress-free temperature."));
    JsonValue *units = json_set_object(o, "units");
    json_set_string(units, "temperature", "degC");
    json_set_string(units, "time", "s");
    json_set_string(units, "energy", "J");
    json_set_string(units, "length", "mm");
    json_set_string(units, "stress", "MPa");
    return o;
}

/* ---- time-dependent conditions ---------------------------------------------------------------------------------- */

static double sched_factor(const ThermalCase *c, int b, double t) {
    int n = c->nsched[b];
    if (n <= 0) return 1;
    int i = 0;
    while (i + 1 < n && c->sched_t[b][i + 1] <= t) i++;
    return c->sched_f[b][i];
}

/* rebuilds the time-dependent model arrays for a solve over [t0, t1] from the base values of the case. Steps land on
 * every schedule time, so the midpoint lies strictly inside one constant piece. Pure in the interval: a rejected trial
 * leaves nothing to undo. */
static void apply_schedule(ThermalCase *c, double t) {
    double f[SA_MAX_BCS];
    for (int b = 0; b < c->nbc; b++) f[b] = sched_factor(c, b, t);
    for (int i = 0; i < c->ntfaces; i++)
        if (c->tface_bc[i] >= 0) c->tfaces[i].value = c->tface_base[i] * f[c->tface_bc[i]]; /* open faces have no condition */
    if (c->nsources > 0) {
        memset(c->elem_source, 0, (size_t)c->nelems * sizeof(double));
        for (int k = 0; k < c->nsources; k++) {
            double q = f[c->source_bc[k]] * c->source_magnitude[k];
            for (int e = 0; e < c->nelems; e++)
                if (c->elem_body[e] == c->source_body[k]) c->elem_source[e] += q;
        }
    }
    for (int n = 0; n < c->nnodes; n++) c->fixed[n] = c->fixed_base[n] && (c->fixed_bc[n] < 0 || f[c->fixed_bc[n]] != 0);
}

/* the setup values (every schedule factor 1), which results and exports describe */
static void restore_base(ThermalCase *c) {
    for (int i = 0; i < c->ntfaces; i++)
        if (c->tface_bc[i] >= 0) c->tfaces[i].value = c->tface_base[i];
    memcpy(c->fixed, c->fixed_base, (size_t)c->nnodes);
    if (c->nsources > 0) {
        memset(c->elem_source, 0, (size_t)c->nelems * sizeof(double));
        for (int k = 0; k < c->nsources; k++)
            for (int e = 0; e < c->nelems; e++)
                if (c->elem_body[e] == c->source_body[k]) c->elem_source[e] += c->source_magnitude[k];
    }
}

static bool case_schedule(void *ctx, ThermalModel *m, double t0, double t1, char *err, size_t errlen) {
    (void)m, (void)err, (void)errlen;
    apply_schedule(ctx, 0.5 * (t0 + t1));
    return true;
}

static bool part_schedule(void *ctx, ThermalModel *m, double t0, double t1, char *err, size_t errlen) {
    ThermalPart *p = ctx;
    (void)m, (void)err, (void)errlen;
    apply_schedule(p->c, 0.5 * (t0 + t1));
    part_restrict(p);
    return true;
}

static const char *failure_code(ThermalIntegratorStatus st) {
    switch (st) {
    case TINT_FAIL_INPUT: return "INVALID_PARAMS";
    case TINT_FAIL_RESOURCE: return "RESOURCE_LIMIT";
    default: return "SOLVER_FAILED";
    }
}

static JsonValue *failure_details(const ThermalCase *c, ThermalIntegratorStatus st, double t_accepted, const char *cause, bool checkpoint) {
    JsonValue *d = json_object();
    json_set_string(d, "failure_class", tint_status_name(st));
    json_set_number(d, "last_accepted_time_s", t_accepted);
    json_set_number(d, "end_time_s", c->settings.end_time);
    json_set_string(d, "cause", cause);
    json_set_string(d, "affected_field", "temperature");
    json_set_bool(d, "checkpoint_available", checkpoint);
    if (checkpoint)
        json_set_string(d, "checkpoint_note",
                        "a checkpoint at the last accepted time was written; job_resume continues from it only with identical mesh, materials, "
                        "conditions and physics settings, so it helps after a resource failure, not after a numerical one");
    JsonValue *rec = json_set_array(d, "recovery_options");
    switch (st) {
    case TINT_FAIL_MIN_STEP:
    case TINT_FAIL_REJECTIONS:
        json_push(rec, json_string("loosen temporal_relative_tolerance or temporal_temperature_tolerance if the accuracy target is stricter than needed"));
        json_push(rec, json_string("lower min_time_step: the solution changes faster than the minimum step can follow"));
        json_push(rec, json_string("check for a discontinuity that is not declared as a schedule time (a load that jumps between events)"));
        break;
    case TINT_FAIL_NONLINEAR:
        json_push(rec, json_string("raise max_iterations; latent heat and radiation make the step nonlinear"));
        json_push(rec, json_string("widen the solidus-liquidus interval of a melting material, which is a model parameter and changes the answer"));
        json_push(rec, json_string("use adaptive stepping, which shrinks the step after a nonlinear failure"));
        break;
    case TINT_FAIL_LINEAR:
        json_push(rec, json_string("check that every body has a material and that conductivities are not many orders of magnitude apart"));
        break;
    case TINT_FAIL_STATE:
        json_push(rec, json_string("use backward Euler (theta 1): Crank-Nicolson can ring below absolute zero at a sharp front"));
        json_push(rec, json_string("check boundary temperatures, ambients and source magnitudes for unit mistakes"));
        break;
    case TINT_FAIL_STEP_LIMIT: json_push(rec, json_string("raise max_steps or loosen the temporal tolerances")); break;
    case TINT_FAIL_RESOURCE: json_push(rec, json_string("use a coarser mesh or fewer stored times")); break;
    default: json_push(rec, json_string("run setup_validate and correct the reported input")); break;
    }
    return d;
}

static void record_history(ThermalCase *c, double t1, double dt, double error) {
    enum { HISTORY_CAP = 200000 };
    if (c->nhistory >= HISTORY_CAP) return;
    static const int GROW = 1024;
    if (c->nhistory % GROW == 0) {
        size_t cap = (size_t)c->nhistory + (size_t)GROW;
        double *a = realloc(c->step_t, cap * sizeof(double)), *b = realloc(c->step_dt, cap * sizeof(double)), *e = realloc(c->step_err, cap * sizeof(double));
        if (a) c->step_t = a;
        if (b) c->step_dt = b;
        if (e) c->step_err = e;
        if (!a || !b || !e) return;
    }
    c->step_t[c->nhistory] = t1;
    c->step_dt[c->nhistory] = dt;
    c->step_err[c->nhistory] = error;
    c->nhistory++;
}

static JsonValue *checkpoint_status_json(const ThermalCase *c, const char *reason) {
    JsonValue *o = json_object();
    json_set_bool(o, "available", c->checkpoint_sequence > 0);
    json_set_int(o, "sequence", c->checkpoint_sequence);
    json_set_number(o, "time_s", c->last_checkpoint_time);
    json_set_string(o, "reason", reason);
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, c->run_dir, "checkpoint.nvc");
    json_set_string(o, "file", path);
    return o;
}

/* writes a checkpoint; a failure to write is reported as a job warning, never as a failure of the physics. The
 * orchestrator chooses the steps, so its controller and grid index are what the saved accepted state continues with. */
static bool take_checkpoint(Job *job, ThermalCase *c, ThermalPart *parts, int nparts, Orchestrator *orch, int frames, const char *reason) {
    if (!c->run_dir[0]) return false;
    const ThermalIntegrator *tis[2];
    for (int k = 0; k < nparts; k++) {
        tint_adopt_control(parts[k].ti, orch_controller(orch), orch_fixed_index(orch));
        tis[k] = parts[k].ti;
    }
    c->orch_work = *orch_work(orch);
    char cerr[512];
    if (!thermal_checkpoint_write(c, tis, nparts, frames, cerr, sizeof cerr)) {
        char msg[640];
        snprintf(msg, sizeof msg, "checkpoint at t = %.6g s not written: %s", orch_time(orch), cerr);
        job_add_warning(job, msg);
        return false;
    }
    job_set_checkpoint_info(job, checkpoint_status_json(c, reason));
    return true;
}

static void restore_history(ThermalCase *c, const ThermalCheckpoint *ck) {
    c->nhistory = 0;
    for (int i = 0; i < ck->nhistory; i++) record_history(c, ck->step_t[i], ck->step_dt[i], ck->step_err[i]);
}

static void restore_flow_info(ThermalCase *c, const JsonValue *fl) {
    c->flow.tau = json_get_num(fl, "tau", 0), c->flow.u_lattice = json_get_num(fl, "u_lattice", 0), c->flow.mach = json_get_num(fl, "mach", 0);
    c->flow.reynolds_cell = json_get_num(fl, "reynolds_cell", 0), c->flow.reynolds_hydraulic = json_get_num(fl, "reynolds_hydraulic", 0);
    c->flow.dt = json_get_num(fl, "dt", 0), c->flow.physical_time = json_get_num(fl, "physical_time", 0), c->flow.change = json_get_num(fl, "change", 0);
    c->flow.density_min = json_get_num(fl, "density_min", 0), c->flow.density_max = json_get_num(fl, "density_max", 0);
    c->flow.inlet_flux = json_get_num(fl, "inlet_flux", 0), c->flow.outlet_flux = json_get_num(fl, "outlet_flux", 0);
    c->flow.scale_min = json_get_num(fl, "scale_min", 0), c->flow.scale_max = json_get_num(fl, "scale_max", 0);
    c->flow.seconds = json_get_num(fl, "seconds", 0), c->flow.steps = (long)json_get_int(fl, "steps", 0);

}

static void add_budget_into(ThermalBudget *t, const ThermalBudget *b) {
    t->source += b->source, t->boundary += b->boundary, t->prescribed += b->prescribed, t->interface_net += b->interface_net;
    t->stored += b->stored, t->enthalpy += b->enthalpy;
    t->worst_balance = fmax(t->worst_balance, b->worst_balance), t->worst_mismatch = fmax(t->worst_mismatch, b->worst_mismatch);
    t->liquid_volume_max = fmax(t->liquid_volume_max, b->liquid_volume_max);
    if (b->picard_max > t->picard_max) t->picard_max = b->picard_max;
    for (int g = 0; g < THERMAL_MAX_GROUPS; g++) {
        t->group_stored[g] += b->group_stored[g], t->group_source[g] += b->group_source[g], t->group_boundary[g] += b->group_boundary[g];
        t->group_prescribed[g] += b->group_prescribed[g], t->interface_heat[g] += b->interface_heat[g];
        t->group_advection[g] += b->group_advection[g], t->group_inflow[g] += b->group_inflow[g];
    }
    t->advection += b->advection, t->inflow += b->inflow, t->enthalpy_inflow += b->enthalpy_inflow, t->enthalpy_outflow += b->enthalpy_outflow;
    t->divergence += b->divergence;
}

bool thermal_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    ThermalCase *c = data;
    int nn = c->nnodes;
    const ThermalSettings *set = &c->settings;
    bool adaptive = set->stepping == THERMAL_STEPPING_ADAPTIVE, partitioned = set->cht && !set->coupling_monolithic;
    int threads = cpu_perf_count();
    ThreadPool *pool = pool_create(threads > 0 ? threads : 1);
    job_progress(job, 0.02, "building the thermal system");
    snprintf(c->status, sizeof c->status, "RUNNING");
    c->cht_partitioned = partitioned;
    if (c->scheduled) apply_schedule(c, 0);
    /* ---- thermal parts ---- */
    ThermalPart parts[2];
    memset(parts, 0, sizeof parts);
    int nparts = partitioned ? 2 : 1;
    bool built = partitioned ? part_split(&parts[0], c, 1) && part_split(&parts[1], c, 2) : part_whole(&parts[0], c);
    double *Tglobal = malloc((size_t)(nn > 0 ? nn : 1) * sizeof(double));
    StructurePart sp = {.c = c, .pool = pool, .T = c->has_mech ? malloc((size_t)nn * sizeof(double)) : NULL};
    if (!built || !Tglobal || (c->has_mech && !sp.T)) {
        for (int k = 0; k < 2; k++) part_free(&parts[k]);
        free(Tglobal), free(sp.T), pool_destroy(pool);
        snprintf(code, codelen, "RESOURCE_LIMIT");
        snprintf(err, errlen, "out of memory building the thermal parts");
        return false;
    }
    /* the nonlinear tolerance must sit well below the temporal one, or the error estimate measures iteration noise */
    double picard_tol = set->picard_tol;
    if (adaptive && set->temporal_temperature > 0) picard_tol = fmin(picard_tol, 1e-3 * set->temporal_temperature);
    ThermalOptions o = {.theta = set->theta,
                        .consistent_capacity = set->consistent_capacity,
                        .max_picard = set->max_picard,
                        .picard_tol = picard_tol,
                        .property_eval = set->property_eval,
                        .element_capacity = set->element_capacity,
                        .pool = pool};
    ThermalIntegratorSettings is = {.mode = set->stepping,
                                    .t_start = 0,
                                    .t_end = set->end_time,
                                    .dt_fixed = set->time_step,
                                    .control = {.dt_initial = adaptive ? set->time_step : 0,
                                                .dt_min = set->dt_min,
                                                .dt_max = set->dt_max,
                                                .safety = set->step_safety,
                                                .max_growth = set->max_step_growth,
                                                .max_shrink = set->max_step_shrink,
                                                .max_rejections = set->max_rejections,
                                                .max_steps = set->max_steps},
                                    .tol = {.relative = set->temporal_relative,
                                            .temperature = set->temporal_temperature,
                                            .enthalpy = set->temporal_enthalpy,
                                            .temperature_only = set->temporal_temperature_only},
                                    .T_reference = set->initial_temperature};
    bool ok = true;
    for (int k = 0; k < nparts && ok; k++) {
        ThermalPart *p = &parts[k];
        for (int n = 0; n < p->nn; n++) p->T0[n] = p->m.fixed && p->m.fixed[n] ? p->m.fixed_T[n] : set->initial_temperature;
        if (!(p->s = thermal_create(&p->m, &o, err, errlen))) {
            snprintf(code, codelen, "SOLVER_FAILED");
            ok = false;
            break;
        }
        ThermalIntegratorSettings pis = is;
        if (c->scheduled) {
            pis.schedule = p->role == 0 ? case_schedule : part_schedule;
            pis.schedule_ctx = p->role == 0 ? (void *)c : (void *)p;
        }
        if (!(p->ti = tint_create(p->s, &p->m, &pis, &c->events, p->T0, err, errlen))) {
            snprintf(code, codelen, "INVALID_PARAMS");
            ok = false;
            break;
        }
        p->tp = (ThermalParticipant){.name = p->name,
                                     .ti = p->ti,
                                     .m = &p->m,
                                     .fixed = p->role == 0 ? c->fixed : p->fixed,
                                     .fixed_T = p->role == 0 ? c->fixed_T : p->fixed_T,
                                     .node_power = p->role == 2 ? p->node_power : NULL,
                                     .velocity = p->role == 1 ? p->velocity : (set->cht ? c->velocity : NULL),
                                     .ninterface = p->role == 0 ? 0 : p->nshared,
                                     .interface_nodes = p->shared,
                                     .estimate = adaptive};
    }
    /* ---- the orchestration: flow -> fluid <-> solids -> structure ---- */
    FlowPart fp = {.c = c, .pool = pool, .node_global = partitioned ? parts[0].node_global : NULL, .nnodes = parts[0].nn};
    OrchParticipant op[4];
    OrchCoupling link[4];
    int np = 0, nl = 0, i_flow = -1, i_thermal = -1, i_fluid = -1, i_solid = -1, i_struct = -1;
    if (ok && set->cht)
        op[i_flow = np++] = (OrchParticipant){.name = "flow",
                                              .self = &fp,
                                              .transient = false,
                                              .time_independent = true,
                                              .evaluate = flow_evaluate,
                                              .field_size = flow_size,
                                              .export_field = flow_export};
    for (int k = 0; k < nparts && ok; k++) {
        int idx = np++;
        if (partitioned) (k == 0 ? (i_fluid = idx) : (i_solid = idx));
        else i_thermal = idx;
        ok = parts[k].bound = thermal_participant_bind(&parts[k].tp, &op[idx], err, errlen);
        if (!ok) snprintf(code, codelen, "RESOURCE_LIMIT");
    }
    if (ok && c->has_mech) {
        const ThermalPart *src = partitioned ? &parts[1] : &parts[0];
        sp.node_global = partitioned ? src->node_global : NULL;
        sp.nimport = src->nn;
        for (int n = 0; n < nn; n++) sp.T[n] = set->initial_temperature;
        op[i_struct = np++] = (OrchParticipant){.name = "structure",
                                                .self = &sp,
                                                .transient = false,
                                                .history_dependent = false,
                                                .evaluate = structure_evaluate,
                                                .field_size = structure_size,
                                                .import_field = structure_import};
    }
    if (i_flow >= 0) link[nl++] = (OrchCoupling){i_flow, partitioned ? i_fluid : i_thermal, "velocity", "velocity", false};
    if (partitioned) {
        link[nl++] = (OrchCoupling){i_solid, i_fluid, "interface_temperature", "interface_temperature", false};
        link[nl++] = (OrchCoupling){i_fluid, i_solid, "interface_heat_out", "interface_heat_in", true};
    }
    if (i_struct >= 0) link[nl++] = (OrchCoupling){partitioned ? i_solid : i_thermal, i_struct, "temperature", "temperature", false};
    const OrchSettings os = {.mode = set->stepping,
                             .t_start = 0,
                             .t_end = set->end_time,
                             .dt_fixed = set->time_step,
                             .control = is.control,
                             .max_coupling_iterations = set->cht ? set->coupling_max_iterations : 0,
                             .coupling_relative = set->cht ? set->coupling_relative : 0,
                             .relaxation = set->cht ? set->coupling_relaxation : 0,
                             .aitken = partitioned};
    Orchestrator *orch = ok ? orch_create(&os, &c->events, op, np, link, nl, err, errlen) : NULL;
    if (ok && !orch) snprintf(code, codelen, "INVALID_PARAMS");
    if (!orch) {
        for (int k = 0; k < 2; k++) part_free(&parts[k]);
        free(Tglobal), free(sp.T), pool_destroy(pool);
        return false;
    }
    c->control = tint_controller(parts[0].ti)->set;
    int out = 0;
    ThermalIntegratorStatus status = TINT_STEPPED;
    const ThermalParticipant *failing = &parts[0].tp;
    if (c->resume) {
        /* continue the same physical run: accepted states, controller history, budgets, work and coupling counters from the
         * checkpoint; the stored frames up to it (structural results included) were restored and the frame file truncated
         * after them; the flow is restored, not recomputed */
        const ThermalCheckpoint *ck = c->resume;
        char why[512] = "";
        if (ck->nparticipants != nparts || ck->nnodes != parts[0].nn || (nparts == 2 && ck->nnodes2 != parts[1].nn))
            snprintf(why, sizeof why, "the checkpoint holds %d thermal participant(s) of %d and %d nodes, this run has %d of %d and %d", ck->nparticipants,
                     ck->nnodes, ck->nnodes2, nparts, parts[0].nn, nparts == 2 ? parts[1].nn : 0);
        else if (set->cht && ck->nvelocity != 3 * nn)
            snprintf(why, sizeof why, "the checkpoint holds no flow field for this conjugate study");
        if (why[0] || !tint_set_state(parts[0].ti, &ck->state, ck->T, err, errlen) ||
            (nparts == 2 && !tint_set_state(parts[1].ti, &ck->state2, ck->T2, err, errlen))) {
            if (why[0]) snprintf(err, errlen, "%s", why);
            orch_free(orch);
            for (int k = 0; k < 2; k++) part_free(&parts[k]);
            free(Tglobal), free(sp.T), pool_destroy(pool);
            snprintf(code, codelen, "INVALID_PARAMS");
            return false;
        }
        if (set->cht) {
            memcpy(c->velocity, ck->velocity, 3 * (size_t)nn * sizeof(double));
            c->flow.done = true;
            restore_flow_info(c, ck->flow_info);
        }
        orch_set_clock(orch, ck->state.t, &ck->state.controller, ck->state.fixed_index);
        orch_set_work(orch, &ck->orch_work);
        out = ck->frames - 1;
        restore_history(c, ck);
        c->checkpoint_sequence = ck->sequence;
        c->last_checkpoint_time = ck->state.t;
        c->nresumes = ck->nresumes;
        memcpy(c->resume_times, ck->resume_times, sizeof c->resume_times);
        if (c->nresumes < 16) c->resume_times[c->nresumes++] = ck->state.t;
        char stage[160];
        snprintf(stage, sizeof stage, "resumed at t = %.4g s from checkpoint %ld", ck->state.t, ck->sequence);
        job_progress(job, 0.05 + 0.85 * ck->state.t / set->end_time, stage);
    } else {
        gather_temperature(parts, nparts, Tglobal);
        store_output(c, out, 0, Tglobal);
        c->last_checkpoint_time = -1;
        sp.index = 0;
        if (set->cht) job_progress(job, 0.03, "computing the steady flow (lattice Boltzmann)");
        if (!(ok = orch_initialize(orch, err, errlen))) snprintf(code, codelen, "SOLVER_FAILED"); /* the flow, then the structure at t = 0 */
        char ferr[512];
        if (ok && !thermal_frames_create(c, ferr, sizeof ferr)) {
            char msg[640];
            snprintf(msg, sizeof msg, "stored frames are kept in memory only (no checkpoints possible): %s", ferr);
            job_add_warning(job, msg);
        }
    }
    double next_physical = set->checkpoint_interval > 0 ? (floor(orch_time(orch) / set->checkpoint_interval) + 1) * set->checkpoint_interval : INFINITY;
    double wall_last = wall_now();
    while (ok) {
        bool pause = job_pause_requested(job), cancel = job_cancel_requested(job);
        if (pause || cancel) {
            bool saved = c->frames_file && take_checkpoint(job, c, parts, nparts, orch, out + 1, pause ? "pause" : "cancel");
            if (pause && saved) {
                snprintf(code, codelen, "PAUSED");
                snprintf(err, errlen, "paused at t = %.6g s of %.6g s after %ld accepted steps; job_resume continues from checkpoint %ld", orch_time(orch),
                         set->end_time, tint_work(parts[0].ti)->accepted, c->checkpoint_sequence);
            } else {
                snprintf(code, codelen, "CANCELLED");
                snprintf(err, errlen, "%s at t = %.6g s of %.6g s after %ld accepted steps%s", pause ? "could not pause (no checkpoint could be written), stopped" : "cancelled",
                         orch_time(orch), set->end_time, tint_work(parts[0].ti)->accepted, saved ? "; job_resume continues from the checkpoint" : "");
                JsonValue *d = json_object();
                json_set_number(d, "last_accepted_time_s", orch_time(orch));
                json_set_bool(d, "checkpoint_available", saved);
                json_set_string(d, "partial_results_policy",
                                "a cancelled run publishes no results: its stored frames and checkpoint stay in the run directory so that job_resume "
                                "can finish it, but results_query refuses a partial history that could be mistaken for the requested analysis");
                job_set_error_details(job, d);
            }
            ok = false;
            break;
        }
        OrchStepReport rep;
        char serr[1024] = "";
        sp.index = out + 1 < c->noutputs ? out + 1 : out; /* where a stored time reached by this step goes */
        OrchStatus ost = orch_step(orch, &rep, serr, sizeof serr);
        if (ost == ORCH_FINISHED) break;
        if (ost == ORCH_FAIL_EVALUATION) { /* the thermal step was accepted; the structure failed at the stored time */
            snprintf(code, codelen, "SOLVER_FAILED");
            snprintf(err, errlen, "%s", serr);
            ok = false;
            break;
        }
        if (ost != ORCH_STEPPED) {
            /* the participant whose trial failed decides the failure class */
            for (int k = 0; k < nparts; k++)
                if (parts[k].tp.trial.failure != THERMAL_FAIL_NONE) failing = &parts[k].tp;
            status = thermal_status(ost, failing);
            char cause[1400];
            const ThermalErrorNorm *nm = &failing->trial.norm;
            if ((status == TINT_FAIL_MIN_STEP || status == TINT_FAIL_REJECTIONS) && failing->trial.estimated && failing->trial.failure == THERMAL_FAIL_NONE)
                snprintf(cause, sizeof cause,
                         "%s (temperature %.3g at node %d, enthalpy %.3g at element %d; estimated %.3g K / %.3g J/m^3; minimum step %.3g s, %d consecutive rejections)",
                         serr, nm->err_T, nm->worst_node, nm->err_H, nm->worst_elem, nm->max_dT, nm->max_dH, orch_controller(orch)->set.dt_min,
                         orch_controller(orch)->rejections);
            else snprintf(cause, sizeof cause, "%s", serr);
            bool saved = c->frames_file && take_checkpoint(job, c, parts, nparts, orch, out + 1, "failure");
            snprintf(code, codelen, "%s", failure_code(status));
            snprintf(err, errlen, "%s at t = %.6g s (last accepted time) of %.6g s: %s", tint_status_name(status), orch_time(orch), set->end_time, cause);
            job_set_error_details(job, failure_details(c, status, orch_time(orch), cause, saved));
            ok = false;
            break;
        }
        record_history(c, rep.t1, rep.dt, adaptive ? rep.error : -1);
        if (rep.event_kinds & EVENT_OUTPUT) {
            if (out + 1 < c->noutputs) out++;
            gather_temperature(parts, nparts, Tglobal);
            store_output(c, out, rep.t1, Tglobal); /* the structure was evaluated for this frame within the step */
            char ferr[512];
            if (!thermal_frames_append(c, out, ferr, sizeof ferr)) {
                job_add_warning(job, ferr);
                thermal_frames_close(c); /* no further checkpoints: they would refer to frames that are not on disk */
            }
        }
        /* checkpoints never change the step sequence: they are taken at whatever accepted boundary is due */
        bool requested = job_checkpoint_requested(job), physical = rep.t1 >= next_physical,
             wall = set->checkpoint_wall_interval > 0 && wall_now() - wall_last >= set->checkpoint_wall_interval;
        if ((requested || physical || wall) && c->frames_file && !orch_finished(orch)) {
            take_checkpoint(job, c, parts, nparts, orch, out + 1, requested ? "request" : (physical ? "interval" : "wall_interval"));
            wall_last = wall_now();
            while (next_physical <= rep.t1) next_physical += set->checkpoint_interval;
        }
        const ThermalWork *w = tint_work(parts[0].ti);
        const ThermalStepStats *last = parts[0].tp.trial.estimated ? &parts[0].tp.trial.s2 : &parts[0].tp.trial.s1;
        char stage[200];
        if (partitioned)
            snprintf(stage, sizeof stage, "t = %.4g of %.4g s, dt %.3g s, %ld accepted / %ld rejected, %d coupling iterations", rep.t1, set->end_time, rep.dt,
                     w->accepted, w->rejected, rep.coupling_iterations);
        else
            snprintf(stage, sizeof stage, "t = %.4g of %.4g s, dt %.3g s, %ld accepted / %ld rejected, %.1f to %.1f degC", rep.t1, set->end_time, rep.dt,
                     w->accepted, w->rejected, last->tmin - 273.15, last->tmax - 273.15);
        job_progress(job, 0.05 + 0.85 * rep.t1 / set->end_time, stage);
        if (orch_finished(orch)) break;
    }
    /* the accepted budget and the diagnostics, whatever the outcome */
    ThermalBudget total;
    memset(&total, 0, sizeof total);
    for (int k = 0; k < nparts; k++) add_budget_into(&total, tint_budget(parts[k].ti));
    if (partitioned) {
        /* the two accounts of the conjugate interface heat: the fluid's reactions at the nodes it holds, and the nodal heat
         * the solids received (their sources minus the volumetric sources of their elements) */
        const ThermalBudget *bf = tint_budget(parts[0].ti), *bs = tint_budget(parts[1].ti);
        double cond = 0, vol = 0;
        for (int g = 0; g < THERMAL_MAX_GROUPS; g++) cond += bf->group_prescribed[g], vol += bs->group_source[g];
        c->cht_heat_into_fluid = bf->prescribed - cond;
        c->cht_heat_into_solid = bs->source - vol;
        const double *Tf = tint_temperature(parts[0].ti), *Ts = tint_temperature(parts[1].ti);
        c->cht_continuity = 0;
        for (int k = 0; k < parts[0].nshared; k++) c->cht_continuity = fmax(c->cht_continuity, fabs(Tf[parts[0].shared[k]] - Ts[parts[1].shared[k]]));
    }
    /* reported without the internal exchange of a partitioned study, which the two participants count with opposite signs */
    c->energy_source = total.source - (partitioned ? c->cht_heat_into_solid : 0), c->energy_boundary = total.boundary;
    c->energy_prescribed = total.prescribed - (partitioned ? c->cht_heat_into_fluid : 0);
    c->energy_stored = total.stored, c->energy_enthalpy = total.enthalpy;
    c->energy_advection = total.advection, c->energy_inflow = total.inflow;
    c->energy_enthalpy_in = total.enthalpy_inflow, c->energy_enthalpy_out = total.enthalpy_outflow, c->energy_divergence = total.divergence;
    c->worst_balance = total.worst_balance, c->worst_enthalpy_mismatch = total.worst_mismatch, c->liquid_volume_max = total.liquid_volume_max;
    c->picard_max = total.picard_max;
    c->work = *tint_work(parts[0].ti);
    if (nparts == 2) { /* the attempts are the orchestrator's and shared; the solves are each participant's */
        const ThermalWork *w2 = tint_work(parts[1].ti);
        c->work.solves += w2->solves, c->work.assemblies += w2->assemblies, c->work.picard_iterations += w2->picard_iterations;
        c->work.linear_iterations += w2->linear_iterations, c->work.seconds_solve += w2->seconds_solve, c->work.seconds_estimate += w2->seconds_estimate;
        c->work.nonlinear_failures += w2->nonlinear_failures, c->work.linear_failures += w2->linear_failures, c->work.state_failures += w2->state_failures;
    }
    c->linear_total = (int)c->work.linear_iterations;
    c->nsteps = (int)c->work.accepted;
    c->last_accepted_time = orch_time(orch);
    ThermalAttempt ring[THERMAL_ATTEMPT_RING];
    int nr = tint_recent_attempts(parts[0].ti, ring, THERMAL_ATTEMPT_RING);
    c->nrecent = nr < 16 ? nr : 16;
    for (int i = 0; i < c->nrecent; i++) c->recent[i] = ring[nr - c->nrecent + i];
    snprintf(c->status, sizeof c->status, "%s",
             ok ? "COMPLETED" : (!strcmp(code, "CANCELLED") ? "CANCELLED" : (!strcmp(code, "PAUSED") ? "PAUSED" : tint_status_name(status))));
    c->group_budget = total;
    c->orch_work = *orch_work(orch);
    json_free(c->orchestration);
    c->orchestration = orch_describe(orch);
    orch_free(orch);
    for (int k = 0; k < 2; k++) part_free(&parts[k]);
    free(sp.T), free(Tglobal);
    thermal_frames_close(c);
    pool_destroy(pool);
    if (c->scheduled) restore_base(c); /* results describe the setup values, not those of the last interval */
    if (!ok) return false;
    c->noutputs = out + 1; /* a shorter run than planned stores fewer times */
    job_progress(job, 0.93, "writing results");
    json_free(c->summary);
    c->summary = thermal_summary_json(c);
    if (c->run_dir[0]) {
        char path[NV_PATH_MAX], hex[65];
        uint64_t bytes = 0;
        path_join(path, sizeof path, c->run_dir, "results.nvt");
        if (!thermal_results_save(c, path, err, errlen)) {
            snprintf(code, codelen, "IO_ERROR");
            return false;
        }
        JsonValue *files = json_set_array(c->summary, "files");
        if (sha256_file(path, hex, &bytes)) {
            JsonValue *fo = json_object();
            json_set_string(fo, "name", "results.nvt");
            json_set_string(fo, "format", "navier-am-thermal-results 2: JSON header and little-endian arrays");
            json_set_int(fo, "bytes", (long long)bytes);
            json_set_string(fo, "sha256", hex);
            json_push(files, fo);
        }
        path_join(path, sizeof path, c->run_dir, "summary.json");
        if (!json_write_file(path, c->summary, JSON_PRETTY | JSON_SORTED)) {
            snprintf(code, codelen, "IO_ERROR");
            snprintf(err, errlen, "cannot write %s", path);
            return false;
        }
        /* results.nvt now holds every stored frame: the restart files of the finished run are no longer needed */
        static const char *const TRANSIENT_FILES[] = {"frames.nvf", "checkpoint.nvc", "checkpoint.prev.nvc", NULL};
        for (int i = 0; TRANSIENT_FILES[i]; i++) {
            path_join(path, sizeof path, c->run_dir, TRANSIENT_FILES[i]);
            if (path_is_file(path)) remove(path);
        }
    }
    job_set_summary(job, json_clone(c->summary));
    return true;
}
