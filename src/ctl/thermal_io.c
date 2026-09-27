/* thermal_io.c - persistence of transient results (results.nvt) and exports (VTU series with a PVD collection, CSV) */
#include "transient_analysis.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char NVT_MAGIC[8] = {'N', 'V', 'T', 'H', 'R', '0', '0', '1'};

typedef struct {
    char name[32];
    char type; /* 'd' float64, 'i' int32, 'b' uint8, 'c' int8 */
    size_t count;
    void *slot;
} Arr;

static size_t type_size(char t) { return t == 'd' ? 8 : (t == 'i' ? 4 : 1); }

static bool little_endian(void) {
    uint16_t x = 1;
    return *(unsigned char *)&x == 1;
}

enum { MAX_ARRS = 33 + 2 * SA_MAX_SETS };

static int array_table(ThermalCase *c, Arr *a) {
    size_t nn = (size_t)c->nnodes, ne = (size_t)c->nelems, nf = (size_t)c->nfaces, no = (size_t)c->noutputs;
    int n = 0;
#define ADD(nm, t, cnt, p)                                                                                                                            \
    do {                                                                                                                                              \
        snprintf(a[n].name, sizeof a[n].name, "%s", nm);                                                                                              \
        a[n].type = t, a[n].count = cnt, a[n].slot = (void *)(p);                                                                                     \
        n++;                                                                                                                                          \
    } while (0)
    ADD("xyz", 'd', 3 * nn, &c->xyz);
    ADD("conn", 'i', 8 * ne, &c->conn);
    ADD("elem_mat", 'i', ne, &c->elem_mat);
    ADD("elem_body", 'c', ne, &c->elem_body);
    ADD("face_elem", 'i', nf, &c->face_elem);
    ADD("face_local", 'b', nf, &c->face_local);
    ADD("fixed", 'b', nn, &c->fixed);
    ADD("fixed_T", 'd', nn, &c->fixed_T);
    ADD("elem_source", 'd', ne, &c->elem_source);
    ADD("times", 'd', no, &c->times);
    if (!c->no_temperature) { /* format version 4: a build without a temperature field writes none */
        ADD("T", 'd', no * nn, &c->T);
        ADD("tmin", 'd', no, &c->tmin);
        ADD("tmax", 'd', no, &c->tmax);
    }
    if (c->tnode_mesh) ADD("tnode_mesh", 'i', nn, &c->tnode_mesh);
    if (c->settings.cht) { /* conjugate heat transfer: the steady flow that carried the energy, and which elements it advected */
        ADD("velocity", 'd', 3 * nn, &c->velocity);
        ADD("advect", 'b', ne, &c->advect);
    }
    if (c->ninterface > 0) {
        size_t ni = (size_t)c->ninterface;
        ADD("iface_a", 'i', ni, &c->io_iface_a);
        ADD("iface_b", 'i', ni, &c->io_iface_b);
        ADD("iface_g", 'd', ni, &c->io_iface_g);
        ADD("iface_area", 'd', ni, &c->io_iface_area);
        ADD("iface_contact", 'i', ni, &c->interface_contact);
    }
    if (c->nhistory > 0) { /* format version 2: the accepted steps */
        size_t nh = (size_t)c->nhistory;
        ADD("step_t", 'd', nh, &c->step_t);
        ADD("step_dt", 'd', nh, &c->step_dt);
        ADD("step_err", 'd', nh, &c->step_err);
    }
    if (c->elem_birth) ADD("elem_birth", 'i', ne, &c->elem_birth); /* format version 3: element activation of a print */
    if (c->elem_death) ADD("elem_death", 'i', ne, &c->elem_death);       /* version 4: elements removed during the run */
    if (c->elem_group) ADD("elem_group", 'b', ne, &c->elem_group);       /* version 4: 0 part, 1 support, 2 plate */
    if (c->has_mech) {
        ADD("mech_u", 'd', 3 * no * nn, &c->mech_u);
        ADD("mech_vm", 'd', no * nn, &c->mech_vm);
        ADD("mech_peak", 'd', no, &c->mech_peak);
        ADD("mech_umax", 'd', no, &c->mech_umax);
    }
    for (int i = 0; i < c->nsets && i < SA_MAX_SETS; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "set%d_nodes", i);
        ADD(nm, 'i', (size_t)c->set[i].nnodes, &c->set[i].nodes);
        snprintf(nm, sizeof nm, "set%d_faces", i);
        ADD(nm, 'i', (size_t)c->set[i].nfaces, &c->set[i].faces);
    }
#undef ADD
    return n;
}

static void *slot_get(const Arr *a) {
    void *p;
    memcpy(&p, a->slot, sizeof p);
    return p;
}

static void slot_set(const Arr *a, void *p) { memcpy(a->slot, &p, sizeof p); }

bool thermal_results_save(const ThermalCase *cc, const char *path, char *err, size_t errlen) {
    if (!little_endian()) {
        snprintf(err, errlen, "results files are little-endian; this host is not");
        return false;
    }
    ThermalCase *c = (ThermalCase *)cc;
    JsonValue *h = json_object();
    json_set_string(h, "format", "navier-am-thermal-results");
    json_set_int(h, "version", 4);
    json_set_bool(h, "has_elem_birth", c->elem_birth != NULL);
    json_set_bool(h, "has_elem_death", c->elem_death != NULL);
    json_set_bool(h, "has_elem_group", c->elem_group != NULL);
    json_set_bool(h, "has_temperature", !c->no_temperature);
    json_set_int(h, "nhistory", c->nhistory);
    json_set_int(h, "nmesh_nodes", c->nmesh_nodes);
    json_set_bool(h, "has_node_map", c->tnode_mesh != NULL);
    json_set_int(h, "ninterface", c->ninterface);
    JsonValue *ctc = json_set_array(h, "contacts");
    for (int i = 0; i < c->ncontacts; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "name", c->contact[i].name);
        json_set_string(o, "body_a", c->contact[i].body_a);
        json_set_string(o, "body_b", c->contact[i].body_b);
        json_set_int(o, "model", c->contact[i].model);
        json_set_int(o, "mesh_body_a", c->contact[i].mesh_body_a);
        json_set_int(o, "mesh_body_b", c->contact[i].mesh_body_b);
        json_set_number(o, "conductance", isfinite(c->contact[i].conductance) ? c->contact[i].conductance : -1);
        json_set_number(o, "area", c->contact[i].area);
        json_set_int(o, "faces", c->contact[i].faces);
        json_set_number(o, "heat_b_to_a", c->group_budget.interface_heat[i]);
        json_push(ctc, o);
    }
    json_set_string(h, "kind", c->settings.cht ? "conjugate_heat_transfer" : (c->settings.mechanical ? "thermomechanical" : "transient_thermal"));
    json_set_string(h, "byte_order", "little");
    json_set_string(h, "job_id", c->job_id);
    json_set_int(h, "nnodes", c->nnodes);
    json_set_int(h, "nelems", c->nelems);
    json_set_int(h, "nfaces", c->nfaces);
    json_set_int(h, "noutputs", c->noutputs);
    json_set_int(h, "nsteps", c->nsteps);
    json_set_bool(h, "has_mechanical", c->has_mech);
    json_set(h, "h_m", json_numbers(c->h, 3));
    json_set_string(h, "mesh_hash", c->mesh_hash);
    JsonValue *bodies = json_set_array(h, "bodies");
    for (int i = 0; i < c->nbodies; i++) json_push(bodies, json_string(c->body_name[i]));
    JsonValue *st = json_set_object(h, "settings");
    json_set_number(st, "end_time", c->settings.end_time);
    json_set_number(st, "time_step", c->settings.time_step);
    json_set_number(st, "theta", c->settings.theta);
    json_set_bool(st, "consistent_capacity", c->settings.consistent_capacity);
    json_set_number(st, "initial_temperature", c->settings.initial_temperature);
    json_set_number(st, "reference_temperature", c->settings.reference_temperature);
    json_set_int(st, "output_every", c->settings.output_every);
    json_set_bool(st, "mechanical", c->settings.mechanical);
    json_set_string(st, "time_stepping", c->settings.stepping == THERMAL_STEPPING_ADAPTIVE ? "adaptive" : "fixed");
    if (c->settings.cht) {
        json_set_bool(st, "cht", true);
        json_set_string(st, "fluid_body", c->settings.fluid_body);
        json_set_number(st, "inlet_velocity", c->settings.inlet_velocity);
        json_set_number(st, "inlet_temperature", c->settings.inlet_temperature);
    }
    JsonValue *bcs = json_set_array(h, "conditions");
    for (int i = 0; i < c->nbc; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "name", c->bc[i].name);
        json_set_string(o, "kind", bc_kind_name(c->bc[i].kind));
        json_set_string(o, "applies_to", c->bc[i].selection);
        json_set_int(o, "faces", c->bc[i].faces);
        json_set_number(o, "mesh_area", c->bc[i].mesh_area);
        json_set_number(o, "nominal", c->bc[i].requested[0]);
        json_push(bcs, o);
    }
    JsonValue *sets = json_set_array(h, "sets");
    for (int i = 0; i < c->nsets; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "name", c->set[i].name);
        json_set_int(o, "nnodes", c->set[i].nnodes);
        json_set_int(o, "nfaces", c->set[i].nfaces);
        json_push(sets, o);
    }
    JsonValue *en = json_set_object(h, "energy");
    json_set_number(en, "source", c->energy_source);
    json_set_number(en, "boundary", c->energy_boundary);
    json_set_number(en, "prescribed", c->energy_prescribed);
    json_set_number(en, "stored", c->energy_stored);
    json_set_number(en, "worst_balance", c->worst_balance);
    json_set_int(en, "picard_max", c->picard_max);
    json_set_int(en, "cg_total", c->linear_total);
    if (c->summary) json_set(h, "summary", json_clone(c->summary));
    if (c->build_warnings) json_set(h, "build_warnings", json_clone(c->build_warnings));
    if (c->ninterface > 0) {
        size_t ni = (size_t)c->ninterface;
        c->io_iface_a = malloc(ni * sizeof(int)), c->io_iface_b = malloc(ni * sizeof(int));
        c->io_iface_g = malloc(ni * sizeof(double)), c->io_iface_area = malloc(ni * sizeof(double));
        if (!c->io_iface_a || !c->io_iface_b || !c->io_iface_g || !c->io_iface_area) {
            json_free(h);
            free(c->io_iface_a), free(c->io_iface_b), free(c->io_iface_g), free(c->io_iface_area);
            c->io_iface_a = c->io_iface_b = NULL, c->io_iface_g = c->io_iface_area = NULL;
            snprintf(err, errlen, "out of memory writing the interfaces");
            return false;
        }
        for (size_t i = 0; i < ni; i++)
            c->io_iface_a[i] = c->interfaces[i].node_a, c->io_iface_b[i] = c->interfaces[i].node_b, c->io_iface_g[i] = c->interfaces[i].conductance,
            c->io_iface_area[i] = c->interfaces[i].area;
    }
    Arr arr[MAX_ARRS];
    int na = array_table(c, arr);
    JsonValue *al = json_set_array(h, "arrays");
    for (int i = 0; i < na; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "name", arr[i].name);
        char t[2] = {arr[i].type, 0};
        json_set_string(o, "type", t);
        json_set_int(o, "count", (long long)arr[i].count);
        json_push(al, o);
    }
    size_t hlen = 0;
    char *text = json_dump(h, 0, &hlen, NULL);
    json_free(h);
    if (!text) {
        snprintf(err, errlen, "out of memory writing the results header");
        return false;
    }
    char tmp[NV_PATH_MAX + 16];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(text);
        snprintf(err, errlen, "cannot write %s", tmp);
        return false;
    }
    uint64_t hl = hlen;
    bool ok = fwrite(NVT_MAGIC, 1, 8, f) == 8 && fwrite(&hl, sizeof hl, 1, f) == 1 && fwrite(text, 1, hlen, f) == hlen;
    free(text);
    for (int i = 0; ok && i < na; i++) {
        void *p = slot_get(&arr[i]);
        if (arr[i].count && !p) {
            fclose(f), remove(tmp);
            snprintf(err, errlen, "results array %s is missing", arr[i].name);
            return false;
        }
        if (arr[i].count) ok = fwrite(p, type_size(arr[i].type), arr[i].count, f) == arr[i].count;
    }
    if (fclose(f) != 0) ok = false;
    free(c->io_iface_a), free(c->io_iface_b), free(c->io_iface_g), free(c->io_iface_area);
    c->io_iface_a = c->io_iface_b = NULL, c->io_iface_g = c->io_iface_area = NULL;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(err, errlen, "cannot write %s (disk full?)", path);
        return false;
    }
    return true;
}

ThermalCase *thermal_results_load(const char *path, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "%s: cannot open the results file", path);
        return NULL;
    }
    char magic[8], *text = NULL;
    uint64_t hl = 0;
    JsonValue *h = NULL;
    ThermalCase *c = NULL;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, NVT_MAGIC, 8) != 0 || fread(&hl, sizeof hl, 1, f) != 1 || hl == 0 || hl > (64u << 20)) {
        snprintf(err, errlen, "%s: not a NAVIER-AM thermal results file", path);
        goto bad;
    }
    text = malloc((size_t)hl);
    JsonError je;
    if (!text || fread(text, 1, (size_t)hl, f) != hl || !(h = json_parse(text, (size_t)hl, NULL, &je))) {
        snprintf(err, errlen, "%s: unreadable results header", path);
        goto bad;
    }
    long long version = json_get_int(h, "version", 0);
    if (strcmp(json_get_str(h, "format", ""), "navier-am-thermal-results") || version < 1 || version > 4) {
        snprintf(err, errlen, "%s: unsupported results format", path);
        goto bad;
    }
    c = calloc(1, sizeof *c);
    if (!c) {
        snprintf(err, errlen, "out of memory");
        goto bad;
    }
    long long nn = json_get_int(h, "nnodes", -1), ne = json_get_int(h, "nelems", -1), nf = json_get_int(h, "nfaces", -1),
              no = json_get_int(h, "noutputs", -1);
    const JsonValue *bodies = json_get(h, "bodies"), *sets = json_get(h, "sets"), *conds = json_get(h, "conditions");
    if (nn <= 0 || nn > 200000000LL || ne <= 0 || ne > 100000000LL || nf < 0 || no <= 0 || no > TC_MAX_OUTPUTS || json_len(sets) > SA_MAX_SETS ||
        json_len(conds) > SA_MAX_BCS || json_len(bodies) > MESH_MAX_BODIES) {
        snprintf(err, errlen, "%s: implausible sizes in the results header", path);
        goto bad;
    }
    c->nnodes = (int)nn, c->nelems = (int)ne, c->nfaces = (int)nf, c->noutputs = (int)no;
    c->nsteps = (int)json_get_int(h, "nsteps", 0);
    long long nh = version >= 2 ? json_get_int(h, "nhistory", 0) : 0;
    if (nh < 0 || nh > 200000) {
        snprintf(err, errlen, "%s: implausible step history length", path);
        goto bad;
    }
    c->nhistory = (int)nh;
    c->nmesh_nodes = (int)json_get_int(h, "nmesh_nodes", nn);
    long long nif = json_get_int(h, "ninterface", 0);
    const JsonValue *ctc = json_get(h, "contacts");
    if (nif < 0 || nif > 50000000LL || json_len(ctc) > THERMAL_MAX_GROUPS) {
        snprintf(err, errlen, "%s: implausible interface sizes", path);
        goto bad;
    }
    c->ninterface = (int)nif;
    c->ncontacts = (int)json_len(ctc);
    for (int i = 0; i < c->ncontacts; i++) {
        const JsonValue *o = json_at(ctc, (size_t)i);
        snprintf(c->contact[i].name, sizeof c->contact[i].name, "%s", json_get_str(o, "name", ""));
        snprintf(c->contact[i].body_a, sizeof c->contact[i].body_a, "%s", json_get_str(o, "body_a", ""));
        snprintf(c->contact[i].body_b, sizeof c->contact[i].body_b, "%s", json_get_str(o, "body_b", ""));
        c->contact[i].model = (int)json_get_int(o, "model", 0);
        c->contact[i].mesh_body_a = (int)json_get_int(o, "mesh_body_a", -1);
        c->contact[i].mesh_body_b = (int)json_get_int(o, "mesh_body_b", -1);
        double g = json_get_num(o, "conductance", 0);
        c->contact[i].conductance = g < 0 ? INFINITY : g;
        c->contact[i].area = json_get_num(o, "area", 0);
        c->contact[i].faces = (int)json_get_int(o, "faces", 0);
        c->group_budget.interface_heat[i] = json_get_num(o, "heat_b_to_a", 0);
    }
    c->has_mech = json_get_bool(h, "has_mechanical", false);
    json_get_numbers(json_get(h, "h_m"), c->h, 3);
    snprintf(c->mesh_hash, sizeof c->mesh_hash, "%s", json_get_str(h, "mesh_hash", ""));
    snprintf(c->job_id, sizeof c->job_id, "%s", json_get_str(h, "job_id", ""));
    c->nbodies = (int)json_len(bodies);
    for (int i = 0; i < c->nbodies; i++) {
        const char *s = json_str(json_at(bodies, (size_t)i));
        snprintf(c->body_name[i], sizeof c->body_name[i], "%s", s ? s : "");
    }
    const JsonValue *st = json_get(h, "settings");
    c->settings.end_time = json_get_num(st, "end_time", 0);
    c->settings.time_step = json_get_num(st, "time_step", 0);
    c->settings.theta = json_get_num(st, "theta", 1);
    c->settings.consistent_capacity = json_get_bool(st, "consistent_capacity", false);
    c->settings.initial_temperature = json_get_num(st, "initial_temperature", 293.15);
    c->settings.reference_temperature = json_get_num(st, "reference_temperature", 293.15);
    c->settings.output_every = (int)json_get_int(st, "output_every", 1);
    c->settings.mechanical = json_get_bool(st, "mechanical", false);
    c->settings.stepping = strcmp(json_get_str(st, "time_stepping", "fixed"), "adaptive") ? THERMAL_STEPPING_FIXED : THERMAL_STEPPING_ADAPTIVE;
    c->settings.cht = json_get_bool(st, "cht", false);
    snprintf(c->settings.fluid_body, sizeof c->settings.fluid_body, "%s", json_get_str(st, "fluid_body", ""));
    c->settings.inlet_velocity = json_get_num(st, "inlet_velocity", 0);
    c->settings.inlet_temperature = json_get_num(st, "inlet_temperature", 0);
    c->nbc = (int)json_len(conds);
    for (int i = 0; i < c->nbc; i++) {
        const JsonValue *o = json_at(conds, (size_t)i);
        snprintf(c->bc[i].name, sizeof c->bc[i].name, "%s", json_get_str(o, "name", ""));
        snprintf(c->bc[i].selection, sizeof c->bc[i].selection, "%s", json_get_str(o, "applies_to", ""));
        int k = bc_kind_from_name(json_get_str(o, "kind", ""));
        c->bc[i].kind = k >= 0 ? (BcKind)k : BC_TEMPERATURE;
        c->bc[i].faces = (int)json_get_int(o, "faces", 0);
        c->bc[i].mesh_area = json_get_num(o, "mesh_area", 0);
        c->bc[i].requested[0] = json_get_num(o, "nominal", 0);
    }
    c->nsets = (int)json_len(sets);
    for (int i = 0; i < c->nsets; i++) {
        const JsonValue *o = json_at(sets, (size_t)i);
        snprintf(c->set[i].name, sizeof c->set[i].name, "%s", json_get_str(o, "name", ""));
        long long a = json_get_int(o, "nnodes", -1), b = json_get_int(o, "nfaces", -1);
        if (a < 0 || a > nn || b < 0 || b > nf) {
            snprintf(err, errlen, "%s: implausible selection sizes", path);
            goto bad;
        }
        c->set[i].nnodes = (int)a, c->set[i].nfaces = (int)b;
    }
    const JsonValue *en = json_get(h, "energy");
    c->energy_source = json_get_num(en, "source", 0);
    c->energy_boundary = json_get_num(en, "boundary", 0);
    c->energy_prescribed = json_get_num(en, "prescribed", 0);
    c->energy_stored = json_get_num(en, "stored", 0);
    c->worst_balance = json_get_num(en, "worst_balance", 0);
    c->picard_max = (int)json_get_int(en, "picard_max", 0);
    c->linear_total = (int)json_get_int(en, "cg_total", 0);
    if (json_get(h, "summary")) c->summary = json_clone(json_get(h, "summary"));
    if (json_get(h, "build_warnings")) c->build_warnings = json_clone(json_get(h, "build_warnings"));

    if (json_get_bool(h, "has_node_map", false)) c->tnode_mesh = (int *)1; /* placeholder: array_table lists it, the loader allocates it */
    if (version >= 3 && json_get_bool(h, "has_elem_birth", false)) c->elem_birth = (int *)1; /* placeholder, as above */
    if (version >= 4 && json_get_bool(h, "has_elem_death", false)) c->elem_death = (int *)1;
    if (version >= 4 && json_get_bool(h, "has_elem_group", false)) c->elem_group = (unsigned char *)1;
    c->no_temperature = version >= 4 && !json_get_bool(h, "has_temperature", true);
    Arr arr[MAX_ARRS];
    int na = array_table(c, arr);
    if (c->tnode_mesh == (int *)1) c->tnode_mesh = NULL;
    if (c->elem_birth == (int *)1) c->elem_birth = NULL;
    if (c->elem_death == (int *)1) c->elem_death = NULL;
    if (c->elem_group == (unsigned char *)1) c->elem_group = NULL;
    const JsonValue *al = json_get(h, "arrays");
    if ((int)json_len(al) != na) {
        snprintf(err, errlen, "%s: the array table does not match the header", path);
        goto bad;
    }
    for (int i = 0; i < na; i++) {
        const JsonValue *o = json_at(al, (size_t)i);
        if (strcmp(json_get_str(o, "name", ""), arr[i].name) || json_get_str(o, "type", "")[0] != arr[i].type ||
            (size_t)json_get_int(o, "count", -1) != arr[i].count) {
            snprintf(err, errlen, "%s: the array table does not match the header", path);
            goto bad;
        }
        void *p = malloc(arr[i].count ? arr[i].count * type_size(arr[i].type) : 1);
        if (!p) {
            snprintf(err, errlen, "out of memory loading results");
            goto bad;
        }
        slot_set(&arr[i], p);
        if (arr[i].count && fread(p, type_size(arr[i].type), arr[i].count, f) != arr[i].count) {
            snprintf(err, errlen, "%s: the results file is truncated", path);
            goto bad;
        }
    }
    if (fgetc(f) != EOF) {
        snprintf(err, errlen, "%s: unexpected data after the last array", path);
        goto bad;
    }
    if (c->ninterface > 0) {
        c->interfaces = malloc((size_t)c->ninterface * sizeof(ThermalInterfaceNode));
        if (!c->interfaces) {
            snprintf(err, errlen, "out of memory loading the interfaces");
            goto bad;
        }
        for (int i = 0; i < c->ninterface; i++) {
            if (c->io_iface_a[i] < 0 || c->io_iface_a[i] >= c->nnodes || c->io_iface_b[i] < 0 || c->io_iface_b[i] >= c->nnodes ||
                c->interface_contact[i] < 0 || c->interface_contact[i] >= c->ncontacts) {
                snprintf(err, errlen, "%s: interface pair %d out of range", path, i);
                goto bad;
            }
            c->interfaces[i] = (ThermalInterfaceNode){c->io_iface_a[i], c->io_iface_b[i], c->io_iface_g[i], c->io_iface_area[i]};
        }
        free(c->io_iface_a), free(c->io_iface_b), free(c->io_iface_g), free(c->io_iface_area);
        c->io_iface_a = c->io_iface_b = NULL, c->io_iface_g = c->io_iface_area = NULL;
    }
    if (c->elem_birth)
        for (int i = 0; i < c->nelems; i++)
            if (c->elem_birth[i] < -1 || c->elem_birth[i] >= c->noutputs) {
                snprintf(err, errlen, "%s: element birth index out of range", path);
                goto bad;
            }
    if (c->elem_death)
        for (int i = 0; i < c->nelems; i++)
            if (c->elem_death[i] < -1 || c->elem_death[i] >= c->noutputs) {
                snprintf(err, errlen, "%s: element death index out of range", path);
                goto bad;
            }
    for (size_t i = 0; i < 8 * (size_t)c->nelems; i++)
        if (c->conn[i] < 0 || c->conn[i] >= c->nnodes) {
            snprintf(err, errlen, "%s: connectivity out of range", path);
            goto bad;
        }
    for (int i = 0; i < c->nfaces; i++)
        if (c->face_elem[i] < 0 || c->face_elem[i] >= c->nelems || c->face_local[i] > 5) {
            snprintf(err, errlen, "%s: boundary faces out of range", path);
            goto bad;
        }
    fclose(f);
    free(text);
    json_free(h);
    return c;
bad:
    fclose(f);
    free(text);
    json_free(h);
    thermal_case_free(c);
    return NULL;
}

/* ---- exports ------------------------------------------------------------------------------------ */

typedef struct {
    const char *type, *name;
    int ncomp;
    const void *data;
    size_t nbytes;
} VtuArray;

static bool write_vtu(const ThermalCase *c, int out, const char *path, char *err, size_t errlen) {
    size_t nn = (size_t)c->nnodes, ne = (size_t)c->nelems;
    double *tc = malloc(nn * sizeof(double));
    int32_t *offsets = malloc(ne * sizeof(int32_t));
    uint8_t *types = malloc(ne);
    if (!tc || !offsets || !types) {
        free(tc), free(offsets), free(types);
        snprintf(err, errlen, "out of memory exporting");
        return false;
    }
    if (c->T)
        for (size_t n = 0; n < nn; n++) tc[n] = c->T[(size_t)out * nn + n] - 273.15;
    for (size_t e = 0; e < ne; e++) offsets[e] = (int32_t)(8 * (e + 1)), types[e] = 12;
    VtuArray pd[5];
    size_t npd = 0;
    if (c->T) pd[npd++] = (VtuArray){"Float64", "temperature_c", 1, tc, nn * 8}; /* lpbf-build: a build has no temperatures */
    if (c->velocity) pd[npd++] = (VtuArray){"Float64", "velocity_m_s", 3, c->velocity, 3 * nn * 8};
    if (c->has_mech) {
        pd[npd++] = (VtuArray){"Float64", "displacement_m", 3, c->mech_u + (size_t)out * 3 * nn, 3 * nn * 8};
        pd[npd++] = (VtuArray){"Float64", "von_mises_nodal_average_pa", 1, c->mech_vm + (size_t)out * nn, nn * 8};
    }
    VtuArray pts = {"Float64", "Points", 3, c->xyz, 3 * nn * 8};
    VtuArray cells[3] = {{"Int32", "connectivity", 1, c->conn, 8 * ne * 4}, {"Int32", "offsets", 1, offsets, ne * 4}, {"UInt8", "types", 1, types, ne}};
    char tmp[NV_PATH_MAX + 16];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(tc), free(offsets), free(types);
        snprintf(err, errlen, "cannot write %s", tmp);
        return false;
    }
    uint64_t off = 0;
    fprintf(f, "<?xml version=\"1.0\"?>\n<!-- NAVIER-AM transient results, t = %.9g s (job %s): SI units, build frame -->\n", c->times[out], c->job_id);
    fprintf(f, "<VTKFile type=\"UnstructuredGrid\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\">\n  <UnstructuredGrid>\n");
    fprintf(f, "    <Piece NumberOfPoints=\"%zu\" NumberOfCells=\"%zu\">\n      <PointData Scalars=\"temperature_c\">\n", nn, ne);
    for (size_t i = 0; i < npd; i++) {
        fprintf(f, "        <DataArray type=\"%s\" Name=\"%s\" NumberOfComponents=\"%d\" format=\"appended\" offset=\"%llu\"/>\n", pd[i].type, pd[i].name,
                pd[i].ncomp, (unsigned long long)off);
        off += 8 + pd[i].nbytes;
    }
    fprintf(f, "      </PointData>\n      <Points>\n");
    fprintf(f, "        <DataArray type=\"%s\" Name=\"%s\" NumberOfComponents=\"%d\" format=\"appended\" offset=\"%llu\"/>\n", pts.type, pts.name, pts.ncomp,
            (unsigned long long)off);
    off += 8 + pts.nbytes;
    fprintf(f, "      </Points>\n      <Cells>\n");
    for (int i = 0; i < 3; i++) {
        fprintf(f, "        <DataArray type=\"%s\" Name=\"%s\" NumberOfComponents=\"%d\" format=\"appended\" offset=\"%llu\"/>\n", cells[i].type,
                cells[i].name, cells[i].ncomp, (unsigned long long)off);
        off += 8 + cells[i].nbytes;
    }
    fprintf(f, "      </Cells>\n    </Piece>\n  </UnstructuredGrid>\n  <AppendedData encoding=\"raw\">\n   _");
    bool ok = true;
    const VtuArray *all[8];
    size_t nall = 0;
    for (size_t i = 0; i < npd; i++) all[nall++] = &pd[i];
    all[nall++] = &pts;
    for (int i = 0; i < 3; i++) all[nall++] = &cells[i];
    for (size_t i = 0; i < nall && ok; i++) {
        uint64_t nb = all[i]->nbytes;
        ok = fwrite(&nb, sizeof nb, 1, f) == 1 && (nb == 0 || fwrite(all[i]->data, 1, (size_t)nb, f) == nb);
    }
    fprintf(f, "\n  </AppendedData>\n</VTKFile>\n");
    if (fclose(f) != 0) ok = false;
    free(tc), free(offsets), free(types);
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(err, errlen, "cannot write %s (disk full?)", path);
        return false;
    }
    return true;
}

bool thermal_export_vtu_series(const ThermalCase *c, const char *dir, int *files, char *err, size_t errlen) {
    char path[NV_PATH_MAX], name[64];
    if (files) *files = 0;
    for (int i = 0; i < c->noutputs; i++) {
        snprintf(name, sizeof name, "time_%04d.vtu", i);
        if (!path_join(path, sizeof path, dir, name)) {
            snprintf(err, errlen, "path too long");
            return false;
        }
        if (!write_vtu(c, i, path, err, errlen)) return false;
        if (files) (*files)++;
    }
    if (!path_join(path, sizeof path, dir, "series.pvd")) {
        snprintf(err, errlen, "path too long");
        return false;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(err, errlen, "cannot write %s", path);
        return false;
    }
    fprintf(f, "<?xml version=\"1.0\"?>\n<VTKFile type=\"Collection\" version=\"1.0\" byte_order=\"LittleEndian\">\n  <Collection>\n");
    for (int i = 0; i < c->noutputs; i++)
        fprintf(f, "    <DataSet timestep=\"%.9g\" group=\"\" part=\"0\" file=\"time_%04d.vtu\"/>\n", c->times[i], i);
    fprintf(f, "  </Collection>\n</VTKFile>\n");
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (ok && files) (*files)++;
    if (!ok) snprintf(err, errlen, "cannot write %s", path);
    return ok;
}

bool thermal_export_csv(const ThermalCase *c, const char *path, char *err, size_t errlen) {
    char tmp[NV_PATH_MAX + 16];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        snprintf(err, errlen, "cannot write %s", tmp);
        return false;
    }
    bool temps = c->T && c->tmin && c->tmax; /* lpbf-build: a build stores no temperature field */
    fprintf(f, "time_s%s%s\n", temps ? ",min_c,max_c" : "", c->has_mech ? ",max_displacement_mm,peak_von_mises_mpa" : "");
    for (int i = 0; i < c->noutputs; i++) {
        fprintf(f, "%.10g", c->times[i]);
        if (temps) fprintf(f, ",%.10g,%.10g", c->tmin[i] - 273.15, c->tmax[i] - 273.15);
        if (c->has_mech) fprintf(f, ",%.10g,%.10g", 1e3 * c->mech_umax[i], 1e-6 * c->mech_peak[i]);
        fprintf(f, "\n");
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(err, errlen, "cannot write %s (disk full?)", path);
        return false;
    }
    return true;
}
