/* static_io.c - persistence of static results (results.nvr) and exports (VTK XML .vtu, CSV) */
#include "static_analysis.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char NVR_MAGIC[8] = {'N', 'V', 'R', 'E', 'S', '0', '0', '1'};

typedef struct {
    char name[32];
    char type; /* 'd' float64, 'i' int32, 'b' uint8, 'c' int8 */
    size_t count;
    void *slot; /* address of the pointer member */
} ArrDesc;

static size_t type_size(char t) { return t == 'd' ? 8 : (t == 'i' ? 4 : 1); }

static bool little_endian(void) {
    uint16_t x = 1;
    return *(unsigned char *)&x == 1;
}

enum { MAX_ARRS = 24 + 2 * SA_MAX_SETS };

static int array_table(StaticResults *r, ArrDesc *a) {
    StaticModel *m = &r->model;
    size_t nn = (size_t)m->nnodes, ne = (size_t)m->nelems, nf = (size_t)m->nfaces;
    int n = 0;
#define ADD(nm, t, c, p)                                                                                                                              \
    do {                                                                                                                                              \
        snprintf(a[n].name, sizeof a[n].name, "%s", nm);                                                                                              \
        a[n].type = t, a[n].count = c, a[n].slot = (void *)(p);                                                                                       \
        n++;                                                                                                                                          \
    } while (0)
    ADD("xyz", 'd', 3 * nn, &m->xyz);
    ADD("conn", 'i', (size_t)sa_npe(m) * ne, &m->conn);
    ADD("elem_mat", 'i', ne, &m->elem_mat);
    ADD("elem_body", 'c', ne, &m->elem_body);
    ADD("face_elem", 'i', nf, &m->face_elem);
    ADD("face_local", 'b', nf, &m->face_local);
    ADD("fixed", 'b', 3 * nn, &m->fixed);
    ADD("fixed_value", 'd', 3 * nn, &m->fixed_value);
    ADD("nodal_force", 'd', 3 * nn, &m->nodal_force);
    ADD("node_bc", 'i', nn, &m->node_bc);
    ADD("u", 'd', 3 * nn, &r->sol.u);
    ADD("reaction", 'd', 3 * nn, &r->sol.reaction);
    ADD("node_stress", 'd', 6 * nn, &r->sol.node_stress);
    ADD("gp_stress", 'd', 6 * (size_t)sa_ngp(m) * ne, &r->sol.gp_stress);
    ADD("node_vm", 'd', nn, &r->node_vm);
    ADD("elem_vm", 'd', ne, &r->elem_vm);
    ADD("concave_seg", 'd', 6 * (size_t)m->nconcave, &m->concave_seg);
    for (int i = 0; i < m->nsets && i < SA_MAX_SETS; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "set%d_nodes", i);
        ADD(nm, 'i', (size_t)m->set[i].nnodes, &m->set[i].nodes);
        snprintf(nm, sizeof nm, "set%d_faces", i);
        ADD(nm, 'i', (size_t)m->set[i].nfaces, &m->set[i].faces);
    }
#undef ADD
    return n;
}

static void *slot_get(const ArrDesc *a) {
    void *p;
    memcpy(&p, a->slot, sizeof p);
    return p;
}

static void slot_set(const ArrDesc *a, void *p) { memcpy(a->slot, &p, sizeof p); }

bool static_results_save(const StaticResults *rc, const char *path, char *err, size_t errlen) {
    if (!little_endian()) {
        snprintf(err, errlen, "results files are little-endian; this host is not");
        return false;
    }
    StaticResults *r = (StaticResults *)rc; /* array_table needs member addresses; nothing is modified */
    const StaticModel *m = &r->model;
    const SolidResult *s = &r->sol;
    JsonValue *h = json_object();
    json_set_string(h, "format", "navier-am-results");
    json_set_int(h, "version", 1);
    json_set_string(h, "kind", "static_structural");
    /* written for every mesh; a reader that knows only hexahedra refuses a file whose element is not hex8 */
    json_set_string(h, "element_type", sa_element_name(m));
    json_set_int(h, "nodes_per_element", sa_npe(m));
    json_set_string(h, "byte_order", "little");
    json_set_string(h, "job_id", r->job_id);
    json_set_int(h, "nnodes", m->nnodes);
    json_set_int(h, "nelems", m->nelems);
    json_set_int(h, "nfaces", m->nfaces);
    json_set_int(h, "nconcave", m->nconcave);
    json_set(h, "h_m", json_numbers(m->h, 3));
    json_set_string(h, "mesh_hash", m->mesh_hash);
    JsonValue *bodies = json_set_array(h, "bodies");
    for (int i = 0; i < m->nbodies; i++) json_push(bodies, json_string(m->body_name[i]));
    JsonValue *mats = json_set_array(h, "materials");
    for (int i = 0; i < m->nmat; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "id", m->mat_id[i]);
        json_set_string(o, "status", m->mat_status[i]);
        json_set_number(o, "E", m->mat[i].E);
        json_set_number(o, "nu", m->mat[i].nu);
        json_set_number(o, "density", m->mat[i].density);
        json_push(mats, o);
    }
    JsonValue *st = json_set_object(h, "settings");
    json_set_string(st, "formulation", static_formulation_name(m->settings.formulation));
    json_set_int(st, "solver", m->settings.solver);
    json_set_number(st, "pcg_tol", m->settings.pcg_tol);
    json_set_number(st, "reference_temperature_k", m->settings.reference_temperature_k);
    json_set(h, "gravity", json_numbers(m->gravity, 3));
    JsonValue *bcs = json_set_array(h, "bcs");
    for (int i = 0; i < m->nbc; i++) {
        const SaBcInfo *b = &m->bc[i];
        JsonValue *o = json_object();
        json_set_string(o, "name", b->name);
        json_set_string(o, "selection", b->selection);
        json_set_string(o, "kind", bc_kind_name(b->kind));
        json_set_int(o, "faces", b->faces);
        json_set_int(o, "nodes", b->nodes);
        json_set_number(o, "mesh_area", b->mesh_area);
        json_set_number(o, "stl_area", b->stl_area);
        json_set(o, "requested", json_numbers(b->requested, 3));
        json_set(o, "applied", json_numbers(b->applied, 3));
        if (isfinite(b->requested_moment[0])) json_set(o, "requested_moment", json_numbers(b->requested_moment, 3));
        if (isfinite(b->applied_moment[0])) json_set(o, "applied_moment", json_numbers(b->applied_moment, 3));
        json_set_int(o, "constrained_nodes", b->constrained_nodes);
        json_push(bcs, o);
    }
    JsonValue *vs = json_set_array(h, "body_volume_stl_m3"), *vm = json_set_array(h, "body_volume_mesh_m3");
    for (int i = 0; i < m->nbodies; i++) {
        json_push(vs, isfinite(m->body_volume_stl[i]) ? json_number(m->body_volume_stl[i]) : json_null());
        json_push(vm, isfinite(m->body_volume_mesh[i]) ? json_number(m->body_volume_mesh[i]) : json_null());
    }
    JsonValue *sets = json_set_array(h, "sets");
    for (int i = 0; i < m->nsets; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "name", m->set[i].name);
        json_set_int(o, "nnodes", m->set[i].nnodes);
        json_set_int(o, "nfaces", m->set[i].nfaces);
        json_push(sets, o);
    }
    JsonValue *so = json_set_object(h, "solution");
    json_set_string(so, "method", s->stats.method);
    json_set_int(so, "iterations", s->stats.iterations);
    json_set_number(so, "rel_residual", s->stats.rel_residual);
    json_set_number(so, "true_residual", s->stats.true_residual);
    json_set_bool(so, "converged", s->stats.converged);
    json_set_number(so, "seconds", s->stats.seconds);
    json_set_int(so, "factor_nnz", s->stats.factor_nnz);
    json_set_number(so, "factor_mb", s->stats.factor_mb);
    json_set_number(so, "min_pivot", s->stats.min_pivot);
    json_set_number(so, "max_pivot", s->stats.max_pivot);
    json_set_int(so, "neq", s->neq);
    json_set_number(so, "strain_energy", s->strain_energy);
    json_set_number(so, "external_work", s->external_work);
    json_set_number(so, "equilibrium_error", s->equilibrium_error);
    json_set(so, "load_total", json_numbers(s->load_total, 3));
    json_set(so, "reaction_total", json_numbers(s->reaction_total, 3));
    json_set_number(so, "min_detJ", s->min_detJ);
    if (r->summary) json_set(h, "summary", json_clone(r->summary));
    if (r->build_warnings) json_set(h, "build_warnings", json_clone(r->build_warnings));
    ArrDesc arr[MAX_ARRS];
    int na = array_table(r, arr);
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
    bool nonfinite = false;
    char *text = json_dump(h, 0, &hlen, &nonfinite);
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
    bool ok = fwrite(NVR_MAGIC, 1, 8, f) == 8 && fwrite(&hl, sizeof hl, 1, f) == 1 && fwrite(text, 1, hlen, f) == hlen;
    free(text);
    for (int i = 0; ok && i < na; i++) {
        void *p = slot_get(&arr[i]);
        if (arr[i].count && !p) {
            snprintf(err, errlen, "results array %s is missing", arr[i].name);
            fclose(f), remove(tmp);
            return false;
        }
        if (arr[i].count) ok = fwrite(p, type_size(arr[i].type), arr[i].count, f) == arr[i].count;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(err, errlen, "cannot write %s (disk full?)", path);
        return false;
    }
    return true;
}

static bool fail_load(char *err, size_t errlen, const char *path, const char *why) {
    snprintf(err, errlen, "%s: %s", path, why);
    return false;
}

static bool check_indices(const StaticResults *r) {
    const StaticModel *m = &r->model;
    for (size_t i = 0; i < (size_t)sa_npe(m) * m->nelems; i++)
        if (m->conn[i] < 0 || m->conn[i] >= m->nnodes) return false;
    for (int e = 0; e < m->nelems; e++)
        if (m->elem_mat[e] < 0 || (m->nmat > 0 && m->elem_mat[e] >= m->nmat) || m->elem_body[e] >= m->nbodies) return false;
    for (int f = 0; f < m->nfaces; f++)
        if (m->face_elem[f] < 0 || m->face_elem[f] >= m->nelems || m->face_local[f] > (m->elem_type ? 3 : 5)) return false;
    for (int nd = 0; nd < m->nnodes; nd++)
        if (m->node_bc[nd] < -1 || m->node_bc[nd] >= m->nbc) return false;
    for (int s = 0; s < m->nsets; s++) {
        for (int i = 0; i < m->set[s].nnodes; i++)
            if (m->set[s].nodes[i] < 0 || m->set[s].nodes[i] >= m->nnodes) return false;
        for (int i = 0; i < m->set[s].nfaces; i++)
            if (m->set[s].faces[i] < 0 || m->set[s].faces[i] >= m->nfaces) return false;
    }
    return true;
}

StaticResults *static_results_load(const char *path, char *err, size_t errlen) {
    if (!little_endian()) {
        fail_load(err, errlen, path, "results files are little-endian; this host is not");
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fail_load(err, errlen, path, "cannot open the results file");
        return NULL;
    }
    char magic[8];
    uint64_t hl = 0;
    char *text = NULL;
    JsonValue *h = NULL;
    StaticResults *r = NULL;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, NVR_MAGIC, 8) != 0 || fread(&hl, sizeof hl, 1, f) != 1 || hl == 0 || hl > (64u << 20)) {
        fail_load(err, errlen, path, "not a NAVIER-AM results file (version 1)");
        goto bad;
    }
    text = malloc((size_t)hl);
    JsonError je;
    if (!text || fread(text, 1, (size_t)hl, f) != hl || !(h = json_parse(text, (size_t)hl, NULL, &je))) {
        fail_load(err, errlen, path, "unreadable results header");
        goto bad;
    }
    if (strcmp(json_get_str(h, "format", ""), "navier-am-results") || json_get_int(h, "version", 0) != 1 || strcmp(json_get_str(h, "kind", ""), "static_structural")) {
        fail_load(err, errlen, path, "unsupported results format or kind");
        goto bad;
    }
    r = calloc(1, sizeof *r);
    if (!r) {
        fail_load(err, errlen, path, "out of memory");
        goto bad;
    }
    StaticModel *m = &r->model;
    long long nn = json_get_int(h, "nnodes", -1), ne = json_get_int(h, "nelems", -1), nf = json_get_int(h, "nfaces", -1);
    const JsonValue *bodies = json_get(h, "bodies"), *mats = json_get(h, "materials"), *bcs = json_get(h, "bcs"), *sets = json_get(h, "sets");
    if (nn <= 0 || nn > 200000000LL || ne <= 0 || ne > 100000000LL || nf < 0 || nf > 600000000LL || json_len(bodies) > MESH_MAX_BODIES ||
        json_len(mats) > SA_MAX_MATERIALS || json_len(bcs) > SA_MAX_BCS || json_len(sets) > SA_MAX_SETS) {
        fail_load(err, errlen, path, "implausible sizes in the results header");
        goto bad;
    }
    m->nnodes = (int)nn, m->nelems = (int)ne, m->nfaces = (int)nf;
    long long ncv = json_get_int(h, "nconcave", -1);
    if (ncv < 0 || ncv > (1LL << 20)) {
        fail_load(err, errlen, path, "implausible re-entrant edge count in the results header");
        goto bad;
    }
    m->nconcave = (int)ncv;
    json_get_numbers(json_get(h, "h_m"), m->h, 3);
    snprintf(m->mesh_hash, sizeof m->mesh_hash, "%s", json_get_str(h, "mesh_hash", ""));
    snprintf(r->job_id, sizeof r->job_id, "%s", json_get_str(h, "job_id", ""));
    m->nbodies = (int)json_len(bodies);
    for (int i = 0; i < m->nbodies; i++) snprintf(m->body_name[i], sizeof m->body_name[i], "%s", json_str(json_at(bodies, i)) ? json_str(json_at(bodies, i)) : "");
    m->nmat = (int)json_len(mats);
    for (int i = 0; i < m->nmat; i++) {
        const JsonValue *o = json_at(mats, i);
        snprintf(m->mat_id[i], sizeof m->mat_id[i], "%s", json_get_str(o, "id", ""));
        snprintf(m->mat_status[i], sizeof m->mat_status[i], "%s", json_get_str(o, "status", ""));
        m->mat[i] = (SolidMaterial){json_get_num(o, "E", 0), json_get_num(o, "nu", 0), json_get_num(o, "density", 0)};
    }
    const JsonValue *st = json_get(h, "settings");
    m->settings.formulation = strcmp(json_get_str(st, "formulation", ""), "full_integration") ? HEX8_INCOMPATIBLE : HEX8_FULL;
    m->settings.solver = (SolidSolver)json_get_int(st, "solver", 0);
    m->settings.pcg_tol = json_get_num(st, "pcg_tol", 1e-10);
    m->settings.reference_temperature_k = json_get_num(st, "reference_temperature_k", 293.15);
    json_get_numbers(json_get(h, "gravity"), m->gravity, 3);
    m->nbc = (int)json_len(bcs);
    for (int i = 0; i < m->nbc; i++) {
        const JsonValue *o = json_at(bcs, i);
        SaBcInfo *b = &m->bc[i];
        snprintf(b->name, sizeof b->name, "%s", json_get_str(o, "name", ""));
        snprintf(b->selection, sizeof b->selection, "%s", json_get_str(o, "selection", ""));
        int kind = bc_kind_from_name(json_get_str(o, "kind", ""));
        b->kind = kind >= 0 ? (BcKind)kind : BC_FIXED;
        b->faces = (int)json_get_int(o, "faces", 0);
        b->nodes = (int)json_get_int(o, "nodes", 0);
        b->mesh_area = json_get_num(o, "mesh_area", 0);
        b->stl_area = json_get_num(o, "stl_area", 0);
        json_get_numbers(json_get(o, "requested"), b->requested, 3);
        json_get_numbers(json_get(o, "applied"), b->applied, 3);
        /* absent in results written before the moments were recorded */
        for (int k = 0; k < 3; k++) b->requested_moment[k] = b->applied_moment[k] = NAN;
        if (json_len(json_get(o, "requested_moment")) == 3) json_get_numbers(json_get(o, "requested_moment"), b->requested_moment, 3);
        if (json_len(json_get(o, "applied_moment")) == 3) json_get_numbers(json_get(o, "applied_moment"), b->applied_moment, 3);
        b->constrained_nodes = (int)json_get_int(o, "constrained_nodes", 0);
    }
    const JsonValue *vs = json_get(h, "body_volume_stl_m3"), *vm = json_get(h, "body_volume_mesh_m3");
    for (int i = 0; i < MESH_MAX_BODIES; i++) {
        const JsonValue *a = json_at(vs, i), *b = json_at(vm, i);
        m->body_volume_stl[i] = a && a->type == JSON_NUMBER ? a->u.number : NAN;
        m->body_volume_mesh[i] = b && b->type == JSON_NUMBER ? b->u.number : NAN;
    }
    m->nsets = (int)json_len(sets);
    for (int i = 0; i < m->nsets; i++) {
        const JsonValue *o = json_at(sets, i);
        snprintf(m->set[i].name, sizeof m->set[i].name, "%s", json_get_str(o, "name", ""));
        long long a = json_get_int(o, "nnodes", -1), b = json_get_int(o, "nfaces", -1);
        if (a < 0 || a > nn || b < 0 || b > nf) {
            fail_load(err, errlen, path, "implausible selection sizes in the results header");
            goto bad;
        }
        m->set[i].nnodes = (int)a, m->set[i].nfaces = (int)b;
    }
    const JsonValue *so = json_get(h, "solution");
    SolveStats *ss = &r->sol.stats;
    snprintf(ss->method, sizeof ss->method, "%s", json_get_str(so, "method", ""));
    ss->iterations = (int)json_get_int(so, "iterations", 0);
    ss->rel_residual = json_get_num(so, "rel_residual", 0);
    ss->true_residual = json_get_num(so, "true_residual", 0);
    ss->converged = json_get_bool(so, "converged", false);
    ss->seconds = json_get_num(so, "seconds", 0);
    ss->factor_nnz = json_get_int(so, "factor_nnz", 0);
    ss->factor_mb = json_get_num(so, "factor_mb", 0);
    ss->min_pivot = json_get_num(so, "min_pivot", 0);
    ss->max_pivot = json_get_num(so, "max_pivot", 0);
    r->sol.neq = (int)json_get_int(so, "neq", 0);
    r->sol.strain_energy = json_get_num(so, "strain_energy", 0);
    r->sol.external_work = json_get_num(so, "external_work", 0);
    r->sol.equilibrium_error = json_get_num(so, "equilibrium_error", 0);
    json_get_numbers(json_get(so, "load_total"), r->sol.load_total, 3);
    json_get_numbers(json_get(so, "reaction_total"), r->sol.reaction_total, 3);
    r->sol.min_detJ = json_get_num(so, "min_detJ", 0);
    if (json_get(h, "summary")) r->summary = json_clone(json_get(h, "summary"));
    if (json_get(h, "build_warnings")) r->build_warnings = json_clone(json_get(h, "build_warnings"));

    {
        const char *et = json_get_str(h, "element_type", "hex8"); /* files written before tetrahedra carry no element type */
        m->elem_type = !strcmp(et, "tet10") ? SOLID_ELEM_TET10 : (!strcmp(et, "tet4") ? SOLID_ELEM_TET4 : SOLID_ELEM_HEX8);
        if (strcmp(et, "hex8") && !m->elem_type) {
            fail_load(err, errlen, path, "unknown element type in the results header");
            goto bad;
        }
        r->sol.gp_per_elem = m->elem_type ? sa_ngp(m) : 0;
    }
    ArrDesc arr[MAX_ARRS];
    int na = array_table(r, arr);
    const JsonValue *al = json_get(h, "arrays");
    if ((int)json_len(al) != na) {
        fail_load(err, errlen, path, "array table does not match the header sizes");
        goto bad;
    }
    for (int i = 0; i < na; i++) {
        const JsonValue *o = json_at(al, (size_t)i);
        if (strcmp(json_get_str(o, "name", ""), arr[i].name) || json_get_str(o, "type", "")[0] != arr[i].type ||
            (size_t)json_get_int(o, "count", -1) != arr[i].count) {
            fail_load(err, errlen, path, "array table does not match the header sizes");
            goto bad;
        }
        void *p = malloc(arr[i].count ? arr[i].count * type_size(arr[i].type) : 1);
        if (!p) {
            fail_load(err, errlen, path, "out of memory loading results");
            goto bad;
        }
        slot_set(&arr[i], p);
        if (arr[i].count && fread(p, type_size(arr[i].type), arr[i].count, f) != arr[i].count) {
            fail_load(err, errlen, path, "the results file is truncated");
            goto bad;
        }
    }
    if (fgetc(f) != EOF) {
        fail_load(err, errlen, path, "unexpected data after the last array");
        goto bad;
    }
    if (!check_indices(r)) {
        fail_load(err, errlen, path, "index out of range: the results file is corrupt");
        goto bad;
    }
    fclose(f);
    free(text);
    json_free(h);
    return r;
bad:
    fclose(f);
    free(text);
    json_free(h);
    static_results_free(r);
    return NULL;
}

/* ---- exports ------------------------------------------------------------------------------------ */

typedef struct {
    const char *type, *name;
    int ncomp;
    const void *data;
    size_t nbytes;
} VtuArray;

static void vtu_decl(FILE *f, const VtuArray *a, uint64_t offset) {
    fprintf(f, "        <DataArray type=\"%s\" Name=\"%s\" NumberOfComponents=\"%d\" format=\"appended\" offset=\"%llu\"/>\n", a->type, a->name, a->ncomp,
            (unsigned long long)offset);
}

bool static_export_vtu(const StaticResults *r, const char *path, char *err, size_t errlen) {
    const StaticModel *m = &r->model;
    const SolidResult *s = &r->sol;
    size_t nn = (size_t)m->nnodes, ne = (size_t)m->nelems;
    if (!little_endian() || !s->u || !s->node_stress || !s->gp_stress || !r->node_vm || !r->elem_vm) {
        snprintf(err, errlen, "results are incomplete; cannot export");
        return false;
    }
    int32_t *offsets = malloc(ne * sizeof(int32_t));
    uint8_t *types = malloc(ne);
    double *mean = malloc(6 * ne * sizeof(double));
    int32_t *body = malloc(ne * sizeof(int32_t));
    if (!offsets || !types || !mean || !body) {
        free(offsets), free(types), free(mean), free(body);
        snprintf(err, errlen, "out of memory exporting %zu elements", ne);
        return false;
    }
    int npe = sa_npe(m), ng = sa_ngp(m);
    for (size_t e = 0; e < ne; e++) {
        offsets[e] = (int32_t)(npe * (e + 1));
        /* VTK_HEXAHEDRON, VTK_TETRA, VTK_QUADRATIC_TETRA: node orders as hex8.h and tet.h */
        types[e] = m->elem_type == SOLID_ELEM_TET10 ? 24 : (m->elem_type == SOLID_ELEM_TET4 ? 10 : 12);
        body[e] = m->elem_body[e];
        for (int k = 0; k < 6; k++) {
            double acc = 0;
            for (int g = 0; g < ng; g++) acc += s->gp_stress[6 * ((size_t)ng * e + (size_t)g) + (size_t)k];
            mean[6 * e + (size_t)k] = acc / ng;
        }
    }
    VtuArray pd[] = {
        {"Float64", "displacement_m", 3, s->u, 3 * nn * 8},
        {"Float64", "von_mises_nodal_average_pa", 1, r->node_vm, nn * 8},
        {"Float64", "stress_nodal_average_pa", 6, s->node_stress, 6 * nn * 8},
        {"Float64", "reaction_force_n", 3, s->reaction, 3 * nn * 8},
        {"UInt8", "prescribed_displacement", 3, m->fixed, 3 * nn},
    };
    VtuArray cd[] = {
        {"Float64", "von_mises_gauss_point_max_pa", 1, r->elem_vm, ne * 8},
        {"Float64", "stress_gauss_point_mean_pa", 6, mean, 6 * ne * 8},
        {"Int32", "body", 1, body, ne * 4},
        {"Int32", "material", 1, m->elem_mat, ne * 4},
    };
    VtuArray pts = {"Float64", "Points", 3, m->xyz, 3 * nn * 8};
    VtuArray cells[] = {
        {"Int32", "connectivity", 1, m->conn, (size_t)npe * ne * 4},
        {"Int32", "offsets", 1, offsets, ne * 4},
        {"UInt8", "types", 1, types, ne},
    };
    const size_t npd = sizeof pd / sizeof pd[0], ncd = sizeof cd / sizeof cd[0], ncell = sizeof cells / sizeof cells[0];
    char tmp[NV_PATH_MAX + 16];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(offsets), free(types), free(mean), free(body);
        snprintf(err, errlen, "cannot write %s", tmp);
        return false;
    }
    uint64_t off = 0;
    fprintf(f, "<?xml version=\"1.0\"?>\n<!-- NAVIER-AM static structural results (job %s): SI units, build frame -->\n", r->job_id);
    fprintf(f, "<VTKFile type=\"UnstructuredGrid\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\">\n  <UnstructuredGrid>\n");
    fprintf(f, "    <Piece NumberOfPoints=\"%zu\" NumberOfCells=\"%zu\">\n      <PointData Vectors=\"displacement_m\" Scalars=\"von_mises_nodal_average_pa\">\n", nn, ne);
    for (size_t i = 0; i < npd; i++) vtu_decl(f, &pd[i], off), off += 8 + pd[i].nbytes;
    fprintf(f, "      </PointData>\n      <CellData Scalars=\"von_mises_gauss_point_max_pa\">\n");
    for (size_t i = 0; i < ncd; i++) vtu_decl(f, &cd[i], off), off += 8 + cd[i].nbytes;
    fprintf(f, "      </CellData>\n      <Points>\n");
    vtu_decl(f, &pts, off), off += 8 + pts.nbytes;
    fprintf(f, "      </Points>\n      <Cells>\n");
    for (size_t i = 0; i < ncell; i++) vtu_decl(f, &cells[i], off), off += 8 + cells[i].nbytes;
    fprintf(f, "      </Cells>\n    </Piece>\n  </UnstructuredGrid>\n  <AppendedData encoding=\"raw\">\n   _");
    bool ok = true;
    const VtuArray *all[16];
    size_t nall = 0;
    for (size_t i = 0; i < npd; i++) all[nall++] = &pd[i];
    for (size_t i = 0; i < ncd; i++) all[nall++] = &cd[i];
    all[nall++] = &pts;
    for (size_t i = 0; i < ncell; i++) all[nall++] = &cells[i];
    for (size_t i = 0; i < nall && ok; i++) {
        uint64_t nb = all[i]->nbytes;
        ok = fwrite(&nb, sizeof nb, 1, f) == 1 && (nb == 0 || fwrite(all[i]->data, 1, (size_t)nb, f) == nb);
    }
    fprintf(f, "\n  </AppendedData>\n</VTKFile>\n");
    if (fclose(f) != 0) ok = false;
    free(offsets), free(types), free(mean), free(body);
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(err, errlen, "cannot write %s (disk full?)", path);
        return false;
    }
    return true;
}

bool static_export_csv(const StaticResults *r, const char *path, char *err, size_t errlen) {
    const StaticModel *m = &r->model;
    const SolidResult *s = &r->sol;
    if (!s->u || !s->node_stress || !r->node_vm) {
        snprintf(err, errlen, "results are incomplete; cannot export");
        return false;
    }
    char tmp[NV_PATH_MAX + 16];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        snprintf(err, errlen, "cannot write %s", tmp);
        return false;
    }
    fprintf(f, "node,x_mm,y_mm,z_mm,ux_mm,uy_mm,uz_mm,u_mm,von_mises_mpa,sxx_mpa,syy_mpa,szz_mpa,sxy_mpa,syz_mpa,szx_mpa,rx_n,ry_n,rz_n,prescribed_xyz\n");
    for (int nd = 0; nd < m->nnodes; nd++) {
        const double *x = m->xyz + 3 * (size_t)nd, *u = s->u + 3 * (size_t)nd, *sg = s->node_stress + 6 * (size_t)nd, *rf = s->reaction + 3 * (size_t)nd;
        const unsigned char *fx = m->fixed + 3 * (size_t)nd;
        fprintf(f, "%d,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%d%d%d\n", nd, 1e3 * x[0],
                1e3 * x[1], 1e3 * x[2], 1e3 * u[0], 1e3 * u[1], 1e3 * u[2], 1e3 * sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]), 1e-6 * r->node_vm[nd],
                1e-6 * sg[0], 1e-6 * sg[1], 1e-6 * sg[2], 1e-6 * sg[3], 1e-6 * sg[4], 1e-6 * sg[5], rf[0], rf[1], rf[2], fx[0] != 0, fx[1] != 0, fx[2] != 0);
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
