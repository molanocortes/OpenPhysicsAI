/* static_model.c - static structural model assembly from a project setup, with setup validation */
#include "../fem/hex8.h"
#include "matlib.h"
#include "selection.h"
#include "static_analysis.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *static_formulation_name(Hex8Formulation f) { return f == HEX8_FULL ? "full_integration" : "incompatible_modes"; }

static void issue(JsonValue *arr, NvErr code, const char *hint, JsonValue *details, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void issue(JsonValue *arr, NvErr code, const char *hint, JsonValue *details, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    JsonValue *o = nv_error_json(code, hint, "%s", msg);
    if (details) json_set(o, "details", details);
    json_push(arr, o);
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

int sa_npe(const StaticModel *m) { return m->elem_type == SOLID_ELEM_TET10 ? 10 : (m->elem_type == SOLID_ELEM_TET4 ? 4 : 8); }
int sa_ngp(const StaticModel *m) { return m->elem_type == SOLID_ELEM_TET10 ? 4 : (m->elem_type == SOLID_ELEM_TET4 ? 1 : 8); }
const char *sa_element_name(const StaticModel *m) {
    return m->elem_type == SOLID_ELEM_TET10 ? "tet10" : (m->elem_type == SOLID_ELEM_TET4 ? "tet4" : "hex8");
}

void sa_elem_coords(const StaticModel *m, int e, double (*X)[3]) {
    int n = sa_npe(m);
    for (int a = 0; a < n; a++) {
        int nd = m->conn[(size_t)n * e + a];
        for (int k = 0; k < 3; k++) X[a][k] = m->xyz[3 * (size_t)nd + k];
    }
}

int sa_face_nodes_of(const StaticModel *m, int face, int *nodes) {
    int e = m->face_elem[face], lf = m->face_local[face], n = sa_npe(m);
    if (!m->elem_type) {
        for (int q = 0; q < 4; q++) nodes[q] = m->conn[8 * (size_t)e + HEX8_FACE_NODES[lf][q]];
        return 4;
    }
    for (int q = 0; q < 3; q++) nodes[q] = m->conn[(size_t)n * e + TET_FACE[lf][q]];
    if (m->elem_type != SOLID_ELEM_TET10) return 3;
    for (int q = 0; q < 3; q++) nodes[3 + q] = m->conn[(size_t)n * e + TET10_FACE_MID[lf][q]];
    return 6;
}

double sa_elem_volume(const StaticModel *m, int e) {
    double X[10][3];
    sa_elem_coords(m, e, X);
    return m->elem_type ? tet_volume(m->elem_type, (const double (*)[3])X) : hex8_volume(X);
}

void sa_elem_centroid(const StaticModel *m, int e, double c[3]) {
    double X[10][3];
    sa_elem_coords(m, e, X);
    int nc = m->elem_type ? 4 : 8;
    c[0] = c[1] = c[2] = 0;
    for (int a = 0; a < nc; a++)
        for (int k = 0; k < 3; k++) c[k] += X[a][k] / nc;
}

void sa_face_load(const StaticModel *m, int face, const double t[3], double *fe, double *area, double normal[3]) {
    double X[10][3];
    int e = m->face_elem[face];
    sa_elem_coords(m, e, X);
    if (m->elem_type) tet_face_load(m->elem_type, (const double (*)[3])X, m->face_local[face], t, fe, area, normal);
    else hex8_face_load(X, m->face_local[face], t, fe, area, normal);
}


static double norm3(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

/* sorted, unique nodes of boundary faces; -1 on allocation failure */
static int face_nodes(const StaticModel *m, const int *faces, int nf, int **out) {
    int *nodes = malloc((size_t)(nf > 0 ? 6 * nf : 1) * sizeof(int));
    *out = nodes;
    if (!nodes) return -1;
    int k = 0;
    for (int i = 0; i < nf; i++) k += sa_face_nodes_of(m, faces[i], nodes + k);
    qsort(nodes, (size_t)k, sizeof(int), cmp_int);
    int u = 0;
    for (int i = 0; i < k; i++)
        if (!u || nodes[i] != nodes[u - 1]) nodes[u++] = nodes[i];
    return u;
}

static int shared_triangles(const Selection *a, const Selection *b) {
    if (strcmp(a->body, b->body) != 0) return 0;
    int i = 0, j = 0, n = 0;
    while (i < a->ntri && j < b->ntri) {
        if (a->tris[i] < b->tris[j]) i++;
        else if (a->tris[i] > b->tris[j]) j++;
        else n++, i++, j++;
    }
    return n;
}

typedef struct {
    Selection *sel;
    Body *body;
    int nfaces;
    int *faces;
    double mesh_area;
} BcTarget;

static bool bc_target(Project *p, const BoundaryCondition *bc, BcTarget *t, JsonValue *errors) {
    memset(t, 0, sizeof *t);
    Selection *sel = project_selection(p, bc->selection);
    if (!sel) {
        issue(errors, NV_ERR_NOT_FOUND, "recreate the selection or remove the condition with boundary_remove", NULL,
              "boundary condition '%s' refers to selection '%s', which does not exist", bc->name, bc->selection);
        return false;
    }
    if (sel->stale) {
        issue(errors, NV_ERR_STALE_REFERENCE, "check the selection against the changed geometry, redefine it (selection_create with replace) and re-apply the condition",
              NULL, "selection '%s' used by '%s' is stale: %s", sel->name, bc->name, sel->stale_reason);
        return false;
    }
    if (strcmp(sel->set_hash, bc->selection_hash) != 0) {
        issue(errors, NV_ERR_STALE_REFERENCE, "check the selection, then re-apply the condition with boundary_apply (replace: true)", NULL,
              "selection '%s' now resolves to different faces than when '%s' was applied", sel->name, bc->name);
        return false;
    }
    int n = selection_mesh_faces(p, sel, NULL, 0, &t->mesh_area);
    if (n <= 0) {
        issue(errors, NV_ERR_MESH_INVALID, "refine the mesh (smaller element_size) or check that the selection lies on a meshed body", NULL,
              "no mesh face carries selection '%s' used by '%s' (%.4g mm2 on the STL surface)", sel->name, bc->name, sel->area * 1e6);
        return false;
    }
    t->faces = malloc((size_t)n * sizeof(int));
    if (!t->faces) {
        issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, NULL, "out of memory mapping '%s'", bc->name);
        return false;
    }
    selection_mesh_faces(p, sel, t->faces, n, NULL);
    t->nfaces = n;
    t->sel = sel;
    t->body = project_body(p, sel->body);
    return t->body != NULL;
}

static bool constraint_components(const BoundaryCondition *bc, const BcTarget *t, bool comp[3], double val[3], JsonValue *errors) {
    for (int k = 0; k < 3; k++) comp[k] = false, val[k] = 0;
    if (bc->kind == BC_FIXED) {
        comp[0] = comp[1] = comp[2] = true;
    } else if (bc->kind == BC_DISPLACEMENT) {
        for (int k = 0; k < 3; k++) comp[k] = bc->component[k], val[k] = bc->component[k] ? bc->vec[k] : 0;
    } else if (bc->kind == BC_FRICTIONLESS) {
        int ax = 0;
        for (int k = 1; k < 3; k++)
            if (fabs(t->sel->normal[k]) > fabs(t->sel->normal[ax])) ax = k;
        const double c = cos(1.0 * 3.14159265358979323846 / 180.0);
        for (int i = 0; i < t->sel->ntri; i++)
            if (fabs(t->body->build_normal[3 * (size_t)t->sel->tris[i] + ax]) < c) {
                issue(errors, NV_ERR_UNSUPPORTED, "use it on flat faces normal to x, y or z, or use a displacement condition with the normal component",
                      NULL, "frictionless support '%s' needs a flat face normal to a build axis; faces of '%s' deviate by more than 1 degree", bc->name,
                      t->sel->name);
                return false;
            }
        comp[ax] = true;
    }
    return true;
}

bool static_model_build(Project *p, const StaticSettings *s, StaticModel *m, JsonValue *errors, JsonValue *warnings) {
    memset(m, 0, sizeof *m);
    m->settings = *s;
    size_t nerr0 = json_len(errors);
    char why[300];
    if (!p->nbodies) {
        issue(errors, NV_ERR_PRECONDITION, "import geometry with geometry_import", NULL, "the project has no bodies");
        return false;
    }
    if (!project_mesh_current_any(p, why, sizeof why)) {
        issue(errors, p->mesh.valid ? NV_ERR_STALE_REFERENCE : NV_ERR_PRECONDITION, "generate the mesh with mesh_generate", NULL, "no current mesh: %s", why);
        return false;
    }
    if (p->mesh.include_plate) {
        issue(errors, NV_ERR_UNSUPPORTED, "regenerate the mesh with include_build_plate: false; plate interaction belongs to the process analyses", NULL,
              "static_structural analyses the parts alone, but the mesh includes the build plate");
        return false;
    }
    const HexMesh *hm = &p->mesh.hm;
    int nn = hm->nnodes, ne = hm->nelems, nf = hm->nfaces;

    /* materials, evaluated at the reference temperature */
    bool need_density = false;
    for (int i = 0; i < p->nbcs; i++) need_density |= p->bcs[i].kind == BC_GRAVITY;
    int mat_of_body[MESH_MAX_BODIES];
    double T = s->reference_temperature_k;
    m->nbodies = p->mesh.nbodies;
    for (int bi = 0; bi < p->mesh.nbodies; bi++) {
        const char *bn = p->mesh.body_name[bi];
        snprintf(m->body_name[bi], sizeof m->body_name[bi], "%s", bn);
        mat_of_body[bi] = -1;
        if (bi < 32 && (s->exclude_bodies >> bi) & 1) continue; /* no stiffness: left out of the structural solve by the caller */
        MaterialAssignment *ma = project_material_for(p, bn);
        if (!ma) {
            issue(errors, NV_ERR_PRECONDITION, "assign a material with material_assign (materials_list shows what is available)", NULL, "body '%s' has no material", bn);
            continue;
        }
        MaterialRecord rec;
        char err[300];
        if (!material_lookup(p->user_materials, ma->material, &rec, err, sizeof err)) {
            issue(errors, NV_ERR_NOT_FOUND, "assign an existing material with material_assign", NULL, "body '%s': %s", bn, err);
            continue;
        }
        double E = mat_eval(&rec.prop[MATP_E], T), nu = mat_eval(&rec.prop[MATP_NU], T), rho = mat_eval(&rec.prop[MATP_DENSITY], T);
        if (!isfinite(E) || !isfinite(nu)) {
            issue(errors, NV_ERR_PRECONDITION, "define youngs_modulus_pa and poisson_ratio (material_define)", NULL, "material '%s' of body '%s' has no elastic properties",
                  rec.id, bn);
            continue;
        }
        if (need_density && !isfinite(rho)) {
            issue(errors, NV_ERR_PRECONDITION, "define density_kg_m3 (material_define)", NULL, "gravity needs the density of material '%s' (body '%s')", rec.id, bn);
            continue;
        }
        static const MatProperty USED[3] = {MATP_E, MATP_NU, MATP_DENSITY};
        for (int q = 0; q < 3; q++) {
            double a, b;
            if (mat_table_range(&rec.prop[USED[q]], &a, &b) && (T < a || T > b))
                warn(warnings, "TEMPERATURE_OUTSIDE_TABLE", "%s of '%s' is tabulated from %.4g to %.4g degC; at the reference temperature %.4g degC the nearest table value is used",
                     matprop_key(USED[q]), rec.id, a - 273.15, b - 273.15, T - 273.15);
        }
        int slot = -1;
        for (int k = 0; k < m->nmat; k++)
            if (!strcmp(m->mat_id[k], rec.id)) slot = k;
        if (slot < 0) {
            if (m->nmat == SA_MAX_MATERIALS) {
                issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, NULL, "more than %d different materials", SA_MAX_MATERIALS);
                continue;
            }
            slot = m->nmat++;
            snprintf(m->mat_id[slot], sizeof m->mat_id[slot], "%s", rec.id);
            snprintf(m->mat_status[slot], sizeof m->mat_status[slot], "%s", rec.status);
            m->mat[slot] = (SolidMaterial){E, nu, isfinite(rho) ? rho : 0};
            if (!strcmp(rec.status, "demonstration"))
                warn(warnings, "DEMONSTRATION_MATERIAL", "material '%s' holds demonstration values that are not calibrated: displacement and stress magnitudes are indicative only",
                     rec.id);
        }
        mat_of_body[bi] = slot;
    }

    /* owned copy of the mesh */
    m->nnodes = nn, m->nelems = ne, m->nfaces = nf;
    memcpy(m->h, hm->h, sizeof m->h);
    for (int bi = 0; bi < MESH_MAX_BODIES; bi++) {
        bool known = bi < hm->nbodies && bi < p->mesh.nbodies;
        m->body_volume_stl[bi] = known ? hm->body_volume_stl[bi] : NAN;
        m->body_volume_mesh[bi] = known ? hm->body_volume_mesh[bi] : NAN;
    }
    snprintf(m->mesh_hash, sizeof m->mesh_hash, "%s", p->mesh.hash);
    m->xyz = dup_mem(hm->xyz, 3 * (size_t)nn * sizeof(double));
    m->elem_type = hm->elem_type;
    m->conn = dup_mem(hm->conn, (size_t)sa_npe(m) * ne * sizeof(int));
    m->elem_body = dup_mem(hm->body, (size_t)ne);
    m->face_elem = dup_mem(hm->face_elem, (size_t)nf * sizeof(int));
    m->face_local = dup_mem(hm->face_local, (size_t)nf);
    m->elem_mat = malloc((size_t)(ne ? ne : 1) * sizeof(int));
    m->fixed = calloc(3 * (size_t)(nn ? nn : 1), 1);
    m->fixed_value = calloc(3 * (size_t)(nn ? nn : 1), sizeof(double));
    m->nodal_force = calloc(3 * (size_t)(nn ? nn : 1), sizeof(double));
    m->node_bc = malloc((size_t)(nn ? nn : 1) * sizeof(int));
    int *owner = malloc(3 * (size_t)(nn ? nn : 1) * sizeof(int));
    if (!m->xyz || !m->conn || !m->elem_body || !m->face_elem || !m->face_local || !m->elem_mat || !m->fixed || !m->fixed_value || !m->nodal_force ||
        !m->node_bc || !owner) {
        free(owner);
        issue(errors, NV_ERR_RESOURCE_LIMIT, "use a coarser mesh", NULL, "out of memory copying the mesh (%d nodes, %d elements)", nn, ne);
        return false;
    }
    for (int e = 0; e < ne; e++) {
        int bi = hm->body[e];
        m->elem_mat[e] = bi >= 0 && bi < MESH_MAX_BODIES && mat_of_body[bi] >= 0 ? mat_of_body[bi] : 0;
    }
    for (int i = 0; i < nn; i++) m->node_bc[i] = -1;
    for (size_t i = 0; i < 3 * (size_t)nn; i++) owner[i] = -1;

    /* sharp re-entrant edges (inside corners) of the meshed bodies: stress singularities in linear elasticity */
    const double sharp = cos(30.0 * 3.14159265358979323846 / 180.0);
    int segcap = 0;
    for (int bi = 0; bi < p->mesh.nbodies; bi++) {
        Body *b = project_body(p, p->mesh.body_name[bi]);
        for (int t = 0; b && t < b->surf.nt; t++)
            for (int k = 0; k < 3; k++) {
                int nb = b->surf.nbr[3 * t + k];
                if (nb <= t) continue; /* each manifold edge once */
                const double *n1 = b->build_normal + 3 * (size_t)t, *n2 = b->build_normal + 3 * (size_t)nb;
                if (n1[0] * n2[0] + n1[1] * n2[1] + n1[2] * n2[2] > sharp) continue;
                int va = b->surf.tri[3 * t + k], vb = b->surf.tri[3 * t + (k + 1) % 3], opp = -1;
                for (int q = 0; q < 3; q++)
                    if (b->surf.tri[3 * nb + q] != va && b->surf.tri[3 * nb + q] != vb) opp = b->surf.tri[3 * nb + q];
                if (opp < 0) continue;
                const double *pa = b->build_v + 3 * (size_t)va, *po = b->build_v + 3 * (size_t)opp;
                /* concave when the neighbouring face rises above the outward side of this face's plane */
                if (!((po[0] - pa[0]) * n1[0] + (po[1] - pa[1]) * n1[1] + (po[2] - pa[2]) * n1[2] > 1e-9)) continue;
                if (m->nconcave == segcap) {
                    if (segcap >= 1 << 20) continue;
                    int nc = segcap ? 2 * segcap : 64;
                    double *ns = realloc(m->concave_seg, 6 * (size_t)nc * sizeof(double));
                    if (!ns) continue;
                    m->concave_seg = ns, segcap = nc;
                }
                double *sg = m->concave_seg + 6 * (size_t)m->nconcave++;
                memcpy(sg, pa, 3 * sizeof(double));
                memcpy(sg + 3, b->build_v + 3 * (size_t)vb, 3 * sizeof(double));
            }
    }

    /* selections on the mesh, for result queries */
    for (int i = 0; i < p->nselections && m->nsets < SA_MAX_SETS; i++) {
        Selection *sel = &p->selections[i];
        int n = sel->stale ? 0 : selection_mesh_faces(p, sel, NULL, 0, NULL);
        if (n <= 0) continue;
        SaSet *st = &m->set[m->nsets];
        st->faces = malloc((size_t)n * sizeof(int));
        if (!st->faces) continue;
        selection_mesh_faces(p, sel, st->faces, n, NULL);
        st->nfaces = n;
        st->nnodes = face_nodes(m, st->faces, n, &st->nodes);
        if (st->nnodes < 0) {
            free(st->faces), free(st->nodes);
            memset(st, 0, sizeof *st);
            continue;
        }
        snprintf(st->name, sizeof st->name, "%s", sel->name);
        m->nsets++;
    }

    /* supports: prescribed displacement components */
    size_t nerr_bcs = json_len(errors);
    int nconstraints = 0;
    for (int i = 0; i < p->nbcs; i++) {
        const BoundaryCondition *bc = &p->bcs[i];
        if (!bc_is_constraint(bc->kind)) continue;
        nconstraints++;
        if (m->nbc == SA_MAX_BCS) {
            issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, NULL, "more than %d boundary conditions", SA_MAX_BCS);
            break;
        }
        BcTarget t;
        if (!bc_target(p, bc, &t, errors)) {
            free(t.faces);
            continue;
        }
        bool comp[3];
        double val[3];
        int *nodes = NULL;
        int nnod = constraint_components(bc, &t, comp, val, errors) ? face_nodes(m, t.faces, t.nfaces, &nodes) : -2;
        if (nnod < 0) {
            if (nnod == -1) issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, NULL, "out of memory applying '%s'", bc->name);
            free(nodes), free(t.faces);
            continue;
        }
        SaBcInfo *info = &m->bc[m->nbc];
        memset(info, 0, sizeof *info);
        snprintf(info->name, sizeof info->name, "%s", bc->name);
        snprintf(info->selection, sizeof info->selection, "%s", bc->selection);
        info->kind = bc->kind;
        info->faces = t.nfaces, info->nodes = nnod, info->mesh_area = t.mesh_area, info->stl_area = t.sel->area;
        int conflicts = 0, other = -1;
        double where[3] = {0, 0, 0};
        for (int a = 0; a < nnod; a++) {
            int nd = nodes[a];
            for (int k = 0; k < 3; k++) {
                if (!comp[k]) continue;
                size_t idx = 3 * (size_t)nd + (size_t)k;
                if (m->fixed[idx]) {
                    if (fabs(m->fixed_value[idx] - val[k]) > 1e-12 + 1e-9 * fabs(val[k])) {
                        if (!conflicts) other = owner[idx], memcpy(where, m->xyz + 3 * (size_t)nd, sizeof where);
                        conflicts++;
                    }
                } else {
                    m->fixed[idx] = 1;
                    m->fixed_value[idx] = val[k];
                    owner[idx] = i;
                }
            }
            if (m->node_bc[nd] < 0) m->node_bc[nd] = m->nbc;
        }
        if (conflicts && other >= 0) {
            const BoundaryCondition *ob = &p->bcs[other];
            Selection *os = project_selection(p, ob->selection);
            int shared = os ? shared_triangles(t.sel, os) : 0;
            if (shared > 0) {
                JsonValue *d = json_object();
                json_set_string(d, "condition", bc->name);
                json_set_string(d, "other_condition", ob->name);
                json_set_int(d, "shared_faces", shared);
                json_set_int(d, "conflicting_node_components", conflicts);
                json_set(d, "example_location_mm", json_vec3(1e3 * where[0], 1e3 * where[1], 1e3 * where[2]));
                issue(errors, NV_ERR_CONFLICTING_BC, "remove one of the conditions or make their prescribed values agree", d,
                      "'%s' and '%s' prescribe different displacements on %d shared faces", bc->name, ob->name, shared);
            } else {
                warn(warnings, "CONSTRAINT_OVERLAP",
                     "'%s' and '%s' prescribe different displacements on %d node components where their faces meet (e.g. at %.4g, %.4g, %.4g mm); the "
                     "earlier condition '%s' is kept there",
                     bc->name, ob->name, conflicts, 1e3 * where[0], 1e3 * where[1], 1e3 * where[2], ob->name);
            }
        }
        free(nodes), free(t.faces);
        m->nbc++;
    }

    /* loads: consistent nodal forces; surface loads reproduce their resultant on the STL surface */
    int nloads = 0, nthermal = 0;
    for (int i = 0; i < p->nbcs; i++) {
        const BoundaryCondition *bc = &p->bcs[i];
        if (bc_is_constraint(bc->kind)) continue;
        if (bc_is_thermal(bc->kind)) {
            nthermal++;
            continue;
        }
        if (m->nbc == SA_MAX_BCS) {
            issue(errors, NV_ERR_RESOURCE_LIMIT, NULL, NULL, "more than %d boundary conditions", SA_MAX_BCS);
            break;
        }
        SaBcInfo *info = &m->bc[m->nbc];
        memset(info, 0, sizeof *info);
        snprintf(info->name, sizeof info->name, "%s", bc->name);
        info->kind = bc->kind;
        if (bc->kind == BC_GRAVITY) {
            double mass = 0, first[3] = {0, 0, 0}; /* first moment of mass about the origin (kg m) */
            for (int e = 0; e < ne; e++) {
                double c[3];
                sa_elem_centroid(m, e, c);
                double me = m->mat[m->elem_mat[e]].density * sa_elem_volume(m, e);
                mass += me;
                for (int k = 0; k < 3; k++) first[k] += me * c[k];
            }
            for (int k = 0; k < 3; k++) {
                m->gravity[k] += bc->vec[k];
                info->requested[k] = info->applied[k] = mass * bc->vec[k];
            }
            const double *g = bc->vec;
            double mg[3] = {first[1] * g[2] - first[2] * g[1], first[2] * g[0] - first[0] * g[2], first[0] * g[1] - first[1] * g[0]};
            memcpy(info->requested_moment, mg, sizeof mg);
            memcpy(info->applied_moment, mg, sizeof mg);
            m->nbc++, nloads++;
            continue;
        }
        snprintf(info->selection, sizeof info->selection, "%s", bc->selection);
        BcTarget t;
        if (!bc_target(p, bc, &t, errors)) {
            free(t.faces);
            continue;
        }
        double scale = t.mesh_area > 0 ? t.sel->area / t.mesh_area : 1, req[3] = {0, 0, 0}, reqm[3] = {0, 0, 0};
        if (bc->kind == BC_FORCE || bc->kind == BC_TRACTION) {
            /* uniform over the selected STL area: the resultant acts at the area centroid */
            for (int k = 0; k < 3; k++) req[k] = bc->kind == BC_FORCE ? bc->vec[k] : bc->vec[k] * t.sel->area;
            const double *c = t.sel->centroid;
            reqm[0] = c[1] * req[2] - c[2] * req[1], reqm[1] = c[2] * req[0] - c[0] * req[2], reqm[2] = c[0] * req[1] - c[1] * req[0];
        } else {
            for (int a = 0; a < t.sel->ntri; a++) {
                int tri = t.sel->tris[a];
                double ft[3];
                for (int k = 0; k < 3; k++) {
                    ft[k] = -bc->magnitude * t.body->build_normal[3 * (size_t)tri + k] * t.body->build_area[tri];
                    req[k] += ft[k];
                }
                const double *c = t.body->build_centroid + 3 * (size_t)tri;
                reqm[0] += c[1] * ft[2] - c[2] * ft[1], reqm[1] += c[2] * ft[0] - c[0] * ft[2], reqm[2] += c[0] * ft[1] - c[1] * ft[0];
            }
        }
        for (int f = 0; f < t.nfaces; f++) {
            int fc = t.faces[f], e = m->face_elem[fc], npe = sa_npe(m);
            double tr[3], fe[30], area, nrm[3];
            for (int k = 0; k < 3; k++) {
                if (bc->kind == BC_FORCE) tr[k] = bc->vec[k] / t.mesh_area;
                else if (bc->kind == BC_TRACTION) tr[k] = bc->vec[k] * scale;
                else tr[k] = -bc->magnitude * t.body->build_normal[3 * (size_t)hm->face_tri[fc] + k] * scale;
            }
            sa_face_load(m, fc, tr, fe, &area, nrm);
            for (int a = 0; a < npe; a++) {
                int nd = m->conn[(size_t)npe * e + a];
                const double *x = m->xyz + 3 * (size_t)nd, *fa = fe + 3 * a;
                for (int k = 0; k < 3; k++) {
                    m->nodal_force[3 * (size_t)nd + k] += fa[k];
                    info->applied[k] += fa[k];
                }
                info->applied_moment[0] += x[1] * fa[2] - x[2] * fa[1];
                info->applied_moment[1] += x[2] * fa[0] - x[0] * fa[2];
                info->applied_moment[2] += x[0] * fa[1] - x[1] * fa[0];
            }
        }
        int *nodes = NULL;
        int nnod = face_nodes(m, t.faces, t.nfaces, &nodes), cn = 0;
        for (int a = 0; a < nnod; a++) {
            size_t q = 3 * (size_t)nodes[a];
            cn += m->fixed[q] || m->fixed[q + 1] || m->fixed[q + 2];
        }
        info->faces = t.nfaces, info->nodes = nnod > 0 ? nnod : 0, info->mesh_area = t.mesh_area, info->stl_area = t.sel->area;
        memcpy(info->requested, req, sizeof req);
        memcpy(info->requested_moment, reqm, sizeof reqm);
        info->constrained_nodes = cn;
        if (cn)
            warn(warnings, "LOAD_ON_SUPPORT", "%d of the %d nodes loaded by '%s' also carry prescribed displacements; load on those components goes directly into the reactions",
                 cn, nnod, bc->name);
        double d[3] = {info->applied[0] - req[0], info->applied[1] - req[1], info->applied[2] - req[2]};
        double rn = norm3(req);
        if (rn == 0)
            warn(warnings, "ZERO_LOAD", "boundary condition '%s' has a zero resultant", bc->name);
        else if (norm3(d) > 0.02 * rn)
            warn(warnings, "LOAD_RESULTANT_DEVIATION",
                 "the %s faces carrying '%s' reproduce its resultant with %.1f%% deviation (%.4g N applied, %.4g N on the STL surface): %s",
                 m->elem_type ? "tetrahedral" : "voxel", bc->name, 100 * norm3(d) / rn, norm3(info->applied), rn,
                 m->elem_type ? "the faceted mesh surface departs from the STL surface there" : "the staircase boundary distorts pressure on curved or inclined faces");
        free(nodes), free(t.faces);
        m->nbc++, nloads++;
    }
    free(owner);

    if (!nconstraints) {
        issue(errors, NV_ERR_INSUFFICIENT_CONSTRAINTS, "add a fixed, displacement or frictionless_support condition with boundary_apply", NULL,
              "the model has no supports, so it can move as a rigid body");
    } else if (json_len(errors) == nerr_bcs) {
        HexModel hmod = {nn, ne, m->xyz, m->conn, m->elem_mat, m->nmat, m->mat, s->formulation, NULL, .elem_type = m->elem_type};
        ConstraintReport rep;
        if (!solid_check_constraints(&hmod, m->fixed, &rep)) {
            for (int k = 0; k < rep.nissues; k++) {
                const ConstraintIssue *is = &rep.issue[k];
                char text[1024];
                constraint_issue_text(is, text, sizeof text);
                JsonValue *d = json_object();
                json_set_int(d, "region", is->region);
                json_set_int(d, "elements", is->elements);
                json_set(d, "centroid_mm", json_vec3(1e3 * is->centroid[0], 1e3 * is->centroid[1], 1e3 * is->centroid[2]));
                json_set_int(d, "free_rigid_body_modes", is->free_modes);
                json_set_int(d, "edge_or_vertex_links", is->weak_links);
                issue(errors, NV_ERR_INSUFFICIENT_CONSTRAINTS, "add supports that remove these motions; no artificial springs or stabilisation are added", d, "%s", text);
            }
            if (!rep.nissues) issue(errors, NV_ERR_INSUFFICIENT_CONSTRAINTS, NULL, NULL, "rigid-body motion is not fully prevented");
        }
    }

    if (nthermal)
        warn(warnings, "THERMAL_CONDITIONS_IGNORED",
             "%d thermal conditions (temperature, flux, convection, radiation or heat source) are part of the setup but do not act in a static structural "
             "analysis; run a thermal or thermomechanical analysis to use them",
             nthermal);
    bool imposed = false;
    for (size_t i = 0; i < 3 * (size_t)nn && !imposed; i++) imposed = m->fixed[i] && m->fixed_value[i] != 0;
    if (!nloads && !imposed)
        warn(warnings, "NO_LOADS", "there are no loads, gravity or nonzero prescribed displacements: every displacement and stress will be zero");
    for (int bi = 0; bi < hm->nbodies && bi < MESH_MAX_BODIES; bi++) {
        double vs = hm->body_volume_stl[bi], vm = hm->body_volume_mesh[bi];
        if (vs > 0 && fabs(vm - vs) > 0.03 * vs)
            warn(warnings, "MESH_VOLUME_ERROR", "the voxel mesh of '%s' has %.1f%% %s volume than the STL, which changes stiffness and weight; refine the mesh",
                 m->body_name[bi], 100 * fabs(vm - vs) / vs, vm > vs ? "more" : "less");
    }
    if (hm->edge_contacts > 0)
        warn(warnings, "EDGE_CONTACTS", "%d mesh nodes join regions only along edges or corners; such joints are not realistic load paths", hm->edge_contacts);
    warn(warnings, "STAIRCASE_BOUNDARY",
         "the voxel mesh approximates inclined and curved surfaces by steps (boundary faces lie up to %.3g mm from the STL surface); stresses at those "
         "surfaces and at re-entrant steps depend on the mesh",
         1e3 * hm->max_face_distance);
    return json_len(errors) == nerr0;
}

void static_model_free(StaticModel *m) {
    free(m->xyz), free(m->conn), free(m->elem_mat), free(m->elem_body), free(m->face_elem), free(m->face_local);
    free(m->fixed), free(m->fixed_value), free(m->nodal_force), free(m->node_bc), free(m->concave_seg);
    for (int i = 0; i < m->nsets; i++) free(m->set[i].nodes), free(m->set[i].faces);
    memset(m, 0, sizeof *m);
}

JsonValue *static_model_summary_json(const StaticModel *m) {
    JsonValue *o = json_object();
    int prescribed = 0;
    for (size_t i = 0; i < 3 * (size_t)m->nnodes; i++) prescribed += m->fixed[i] != 0;
    json_set_int(o, "nodes", m->nnodes);
    json_set_int(o, "elements", m->nelems);
    json_set_int(o, "dofs", 3LL * m->nnodes);
    json_set_int(o, "prescribed_dofs", prescribed);
    json_set_int(o, "free_dofs", 3LL * m->nnodes - prescribed);
    json_set_string(o, "element", "hex8");
    json_set_string(o, "formulation", static_formulation_name(m->settings.formulation));
    json_set(o, "element_size_mm", json_vec3(1e3 * m->h[0], 1e3 * m->h[1], 1e3 * m->h[2]));
    json_set_number(o, "reference_temperature_c", m->settings.reference_temperature_k - 273.15);
    json_set_string(o, "mesh_hash", m->mesh_hash);
    JsonValue *bodies = json_set_array(o, "bodies");
    for (int i = 0; i < m->nbodies; i++) json_push(bodies, json_string(m->body_name[i]));
    JsonValue *mats = json_set_array(o, "materials");
    for (int i = 0; i < m->nmat; i++) {
        JsonValue *mo = json_object();
        json_set_string(mo, "id", m->mat_id[i]);
        json_set_string(mo, "status", m->mat_status[i]);
        json_set_number(mo, "youngs_modulus_mpa", m->mat[i].E * 1e-6);
        json_set_number(mo, "poisson_ratio", m->mat[i].nu);
        json_set_number(mo, "density_kg_m3", m->mat[i].density);
        json_push(mats, mo);
    }
    JsonValue *bcs = json_set_array(o, "boundary_conditions");
    for (int i = 0; i < m->nbc; i++) {
        const SaBcInfo *b = &m->bc[i];
        JsonValue *bo = json_object();
        json_set_string(bo, "name", b->name);
        json_set_string(bo, "kind", bc_kind_name(b->kind));
        if (b->selection[0]) {
            json_set_string(bo, "selection", b->selection);
            json_set_int(bo, "mesh_faces", b->faces);
            json_set_int(bo, "nodes", b->nodes);
            json_set_number(bo, "mesh_area_mm2", b->mesh_area * 1e6);
            json_set_number(bo, "stl_area_mm2", b->stl_area * 1e6);
        }
        if (!bc_is_constraint(b->kind)) {
            json_set(bo, "resultant_on_stl_n", json_vec3(b->requested[0], b->requested[1], b->requested[2]));
            json_set(bo, "resultant_applied_n", json_vec3(b->applied[0], b->applied[1], b->applied[2]));
            if (b->constrained_nodes) json_set_int(bo, "loaded_nodes_on_supports", b->constrained_nodes);
        }
        json_push(bcs, bo);
    }
    if (m->gravity[0] != 0 || m->gravity[1] != 0 || m->gravity[2] != 0) json_set(o, "gravity_m_s2", json_vec3(m->gravity[0], m->gravity[1], m->gravity[2]));
    return o;
}

void static_strain_from_stress(const SolidMaterial *mt, const double s[6], double eps[6]) {
    double E = mt->E, nu = mt->nu, g = 2 * (1 + nu) / E;
    eps[0] = (s[0] - nu * (s[1] + s[2])) / E;
    eps[1] = (s[1] - nu * (s[0] + s[2])) / E;
    eps[2] = (s[2] - nu * (s[0] + s[1])) / E;
    eps[3] = g * s[3], eps[4] = g * s[4], eps[5] = g * s[5];
}
