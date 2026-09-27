/* static_quantities.c - engineering quantities of a static structural result, each with one stated definition
 * (static_analysis.h). They are computed from the stored result (mesh, displacements, reactions, loads), so they can be
 * evaluated again from any run directory. */
#include "static_analysis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double norm3(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static void cross3(const double a[3], const double b[3], double out[3]) {
    double r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    memcpy(out, r, sizeof r);
}

static JsonValue *mm3(const double *x) { return json_vec3(1e3 * x[0], 1e3 * x[1], 1e3 * x[2]); }

static void elem_coords(const StaticModel *m, int e, double X[][3]) { sa_elem_coords(m, e, X); }

int static_find_bc(const StaticModel *m, const char *name) {
    for (int i = 0; name && i < m->nbc; i++)
        if (!strcmp(m->bc[i].name, name)) return i;
    return -1;
}

static const SaSet *find_set(const StaticModel *m, const char *name) {
    for (int i = 0; name && i < m->nsets; i++)
        if (!strcmp(m->set[i].name, name)) return &m->set[i];
    return NULL;
}

/* nodes, the weights that give the mean of the displacement over the face (exact for its interpolation), area and
 * area centroid of a boundary face: a planar quadrilateral (4 corners, 1/4 each), a flat triangle (3 corners, 1/3
 * each) or a six-node triangle (the corner functions integrate to zero, the mid-edge ones to a third each) */
static int face_geometry(const StaticModel *m, int fc, int nodes[6], double w[6], double *area, double centroid[3]) {
    int e = m->face_elem[fc], local = m->face_local[fc];
    if (m->elem_type) {
        int n = sa_face_nodes_of(m, fc, nodes);
        const double *a = m->xyz + 3 * (size_t)nodes[0], *b = m->xyz + 3 * (size_t)nodes[1], *c = m->xyz + 3 * (size_t)nodes[2];
        double d1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, d2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]}, cr[3];
        cross3(d1, d2, cr);
        *area = 0.5 * norm3(cr);
        for (int k = 0; k < 3; k++) centroid[k] = (a[k] + b[k] + c[k]) / 3;
        for (int q = 0; q < n; q++) w[q] = n == 3 ? 1.0 / 3 : (q < 3 ? 0 : 1.0 / 3);
        return n;
    }
    for (int a = 0; a < 4; a++) w[a] = 0.25;
    double x[4][3];
    for (int a = 0; a < 4; a++) {
        nodes[a] = m->conn[8 * (size_t)e + HEX8_FACE_NODES[local][a]];
        memcpy(x[a], m->xyz + 3 * (size_t)nodes[a], sizeof x[a]);
    }
    double d1[3], d2[3], c[3];
    for (int k = 0; k < 3; k++) d1[k] = x[2][k] - x[0][k], d2[k] = x[3][k] - x[1][k];
    cross3(d1, d2, c);
    *area = 0.5 * norm3(c);
    for (int k = 0; k < 3; k++) centroid[k] = 0.25 * (x[0][k] + x[1][k] + x[2][k] + x[3][k]);
    return 4;
}

bool static_region_displacement(const StaticResults *r, const char *selection, const double dir[3], StaticRegionDisplacement *out, char *err,
                                size_t errlen) {
    memset(out, 0, sizeof *out);
    const StaticModel *m = &r->model;
    const SaSet *set = find_set(m, selection);
    if (!set || set->nfaces <= 0) {
        snprintf(err, errlen,
                 "selection '%s' has no mesh faces in this result: it was not defined when the analysis ran, was stale, or is smaller than the "
                 "elements",
                 selection ? selection : "");
        return false;
    }
    double dn = norm3(dir);
    if (!(dn > 0) || !isfinite(dn)) {
        snprintf(err, errlen, "the direction must be a nonzero vector");
        return false;
    }
    double d[3] = {dir[0] / dn, dir[1] / dn, dir[2] / dn};
    double A = 0, mean[3] = {0, 0, 0}, along = 0, cen[3] = {0, 0, 0};
    for (int i = 0; i < set->nfaces; i++) {
        int nodes[6];
        double area, c[3], um[3] = {0, 0, 0}, wq[6];
        int nq = face_geometry(m, set->faces[i], nodes, wq, &area, c);
        for (int a = 0; a < nq; a++)
            for (int k = 0; k < 3; k++) um[k] += wq[a] * r->sol.u[3 * (size_t)nodes[a] + k];
        A += area;
        for (int k = 0; k < 3; k++) mean[k] += area * um[k], cen[k] += area * c[k];
        along += area * (um[0] * d[0] + um[1] * d[1] + um[2] * d[2]);
    }
    if (!(A > 0)) {
        snprintf(err, errlen, "the faces of selection '%s' have no area", selection);
        return false;
    }
    out->faces = set->nfaces;
    out->area = A;
    for (int k = 0; k < 3; k++) out->mean[k] = mean[k] / A, out->centroid[k] = cen[k] / A;
    out->along = along / A;
    out->min_along = INFINITY, out->max_along = -INFINITY;
    for (int i = 0; i < set->nnodes; i++) {
        const double *u = r->sol.u + 3 * (size_t)set->nodes[i];
        double v = u[0] * d[0] + u[1] * d[1] + u[2] * d[2];
        out->min_along = fmin(out->min_along, v), out->max_along = fmax(out->max_along, v);
    }
    return true;
}

bool static_load_work(const StaticResults *r, int bc, StaticLoadWork *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    const StaticModel *m = &r->model;
    if (bc < 0 || bc >= m->nbc) {
        snprintf(err, errlen, "unknown load condition");
        return false;
    }
    const SaBcInfo *b = &m->bc[bc];
    if (b->kind != BC_FORCE && b->kind != BC_TRACTION) {
        snprintf(err, errlen,
                 "'%s' is a %s condition: the work-conjugate displacement and stiffness are defined here for force and traction loads (a uniform "
                 "traction over their faces)",
                 b->name, bc_kind_name(b->kind));
        return false;
    }
    const SaSet *set = find_set(m, b->selection);
    if (!set || set->nfaces != b->faces || !(b->mesh_area > 0)) {
        snprintf(err, errlen, "the faces of '%s' (selection '%s') are not stored in this result", b->name, b->selection);
        return false;
    }
    /* the consistent nodal forces of this load alone: the uniform traction resultant / mesh area, as the model applied it */
    double tr[3] = {b->requested[0] / b->mesh_area, b->requested[1] / b->mesh_area, b->requested[2] / b->mesh_area};
    double F[3] = {0, 0, 0}, W = 0;
    for (int i = 0; i < set->nfaces; i++) {
        int fc = set->faces[i], e = m->face_elem[fc], npe = sa_npe(m);
        double fe[30], area, nrm[3];
        sa_face_load(m, fc, tr, fe, &area, nrm);
        for (int a = 0; a < npe; a++) {
            const double *u = r->sol.u + 3 * (size_t)m->conn[(size_t)npe * e + a];
            for (int k = 0; k < 3; k++) {
                F[k] += fe[3 * a + k];
                W += fe[3 * a + k] * u[k];
            }
        }
    }
    double diff[3] = {F[0] - b->applied[0], F[1] - b->applied[1], F[2] - b->applied[2]};
    if (norm3(diff) > 1e-9 * (norm3(b->applied) + 1e-300)) {
        snprintf(err, errlen, "the nodal forces of '%s' cannot be reproduced from the stored result (difference %.3g N)", b->name, norm3(diff));
        return false;
    }
    memcpy(out->force, F, sizeof F);
    out->work = W;
    double fn = norm3(F);
    out->conjugate_displacement = fn > 0 ? W / fn : NAN;
    out->stiffness = W > 0 ? fn * fn / W : NAN;
    return true;
}

static int body_material(const StaticModel *m, int bi) {
    for (int e = 0; e < m->nelems; e++)
        if (m->elem_body[e] == bi) return m->elem_mat[e];
    return -1;
}

JsonValue *static_mass_json(const StaticModel *m) {
    JsonValue *o = json_object();
    JsonValue *bodies = json_set_array(o, "bodies");
    double tm = 0, tg = 0;
    bool mesh_known = true, geom_known = true;
    for (int bi = 0; bi < m->nbodies; bi++) {
        int mat = body_material(m, bi);
        if (mat < 0) continue; /* not in the structural model */
        double vol = 0;
        for (int e = 0; e < m->nelems; e++) {
            if (m->elem_body[e] != bi) continue;
            vol += sa_elem_volume(m, e);
        }
        double rho = m->mat[mat].density, vs = m->body_volume_stl[bi];
        JsonValue *b = json_object();
        json_set_string(b, "body", m->body_name[bi]);
        json_set_string(b, "material", m->mat_id[mat]);
        json_set_string(b, "material_status", m->mat_status[mat]);
        json_set_number(b, "volume_mesh_mm3", vol * 1e9);
        if (isfinite(vs)) json_set_number(b, "volume_geometry_mm3", vs * 1e9);
        if (rho > 0) {
            json_set_number(b, "density_kg_m3", rho);
            json_set_number(b, "mass_mesh_kg", rho * vol);
            tm += rho * vol;
            if (isfinite(vs)) {
                json_set_number(b, "mass_geometry_kg", rho * vs);
                tg += rho * vs;
            } else {
                geom_known = false;
            }
        } else {
            json_set_string(b, "mass_unavailable", "the material defines no density_kg_m3");
            mesh_known = geom_known = false;
        }
        json_push(bodies, b);
    }
    if (mesh_known) json_set_number(o, "total_mesh_kg", tm);
    if (geom_known) json_set_number(o, "total_geometry_kg", tg);
    json_set_string(o, "definition",
                    m->elem_type ? "density at the reference temperature times volume. mass_geometry uses the closed STL volume (the design); mass_mesh the "
                                   "tetrahedral mesh volume (what the stiffness and any gravity load were computed on)"
                                 : "density at the reference temperature times volume. mass_geometry uses the closed STL volume (the design); mass_mesh the voxel "
                                   "mesh volume (what the stiffness and any gravity load were computed on)");
    return o;
}

JsonValue *static_balance_json(const StaticResults *r, const double *about) {
    const StaticModel *m = &r->model;
    const SolidResult *s = &r->sol;
    int nn = m->nnodes;
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int i = 0; i < nn; i++)
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], m->xyz[3 * (size_t)i + k]), hi[k] = fmax(hi[k], m->xyz[3 * (size_t)i + k]);
    double p[3], ext[3];
    for (int k = 0; k < 3; k++) p[k] = about ? about[k] : 0.5 * (lo[k] + hi[k]), ext[k] = nn ? hi[k] - lo[k] : 0;
    double L = norm3(ext);

    double Fn[3] = {0, 0, 0}, Mn[3] = {0, 0, 0}, Fr[3] = {0, 0, 0}, Mr[3] = {0, 0, 0}, Fg[3] = {0, 0, 0}, Mg[3] = {0, 0, 0};
    for (int i = 0; i < nn; i++) {
        const double *x = m->xyz + 3 * (size_t)i, *f = m->nodal_force + 3 * (size_t)i, *rr = s->reaction + 3 * (size_t)i;
        double arm[3] = {x[0] - p[0], x[1] - p[1], x[2] - p[2]}, t[3];
        cross3(arm, f, t);
        for (int k = 0; k < 3; k++) Fn[k] += f[k], Mn[k] += t[k];
        cross3(arm, rr, t);
        for (int k = 0; k < 3; k++) Fr[k] += rr[k], Mr[k] += t[k];
    }
    if (norm3(m->gravity) > 0) {
        /* consistent body forces sum to rho V g per element and their moment to rho V (c - p) x g, c the volume centroid */
        for (int e = 0; e < m->nelems; e++) {
            double X[10][3], detj[8], vol = 0, c[3] = {0, 0, 0};
            elem_coords(m, e, X);
            if (m->elem_type) { /* the rule the body load was integrated with */
                double L[4][4], wg[4], dv[4], N[10];
                int ng = tet_gauss_count(m->elem_type), n = tet_nodes(m->elem_type);
                tet_gauss_points(m->elem_type, L, wg);
                tet_gauss_dv(m->elem_type, (const double (*)[3])X, dv);
                for (int g = 0; g < ng; g++) {
                    tet_shape(m->elem_type, L[g], N, NULL);
                    vol += dv[g];
                    for (int a = 0; a < n; a++)
                        for (int k = 0; k < 3; k++) c[k] += dv[g] * N[a] * X[a][k];
                }
            } else {
            hex8_gauss_detj(X, detj);
            for (int g = 0; g < 8; g++) {
                double N[8], dN[8][3];
                hex8_shape(HEX8_XI[g][0] / sqrt(3.0), HEX8_XI[g][1] / sqrt(3.0), HEX8_XI[g][2] / sqrt(3.0), N, dN);
                vol += detj[g];
                for (int a = 0; a < 8; a++)
                    for (int k = 0; k < 3; k++) c[k] += detj[g] * N[a] * X[a][k];
            }
            }
            if (!(vol > 0)) continue;
            double me = m->mat[m->elem_mat[e]].density * vol, arm[3], t[3], w[3];
            for (int k = 0; k < 3; k++) arm[k] = c[k] / vol - p[k], w[k] = me * m->gravity[k];
            cross3(arm, w, t);
            for (int k = 0; k < 3; k++) Fg[k] += w[k], Mg[k] += t[k];
        }
    }
    double Fres[3], Mres[3];
    for (int k = 0; k < 3; k++) Fres[k] = Fn[k] + Fg[k] + Fr[k], Mres[k] = Mn[k] + Mg[k] + Mr[k];
    double fscale = norm3(Fn) + norm3(Fg) + norm3(Fr), mscale = norm3(Mn) + norm3(Mg) + norm3(Mr) + fscale * L;
    double ferr = fscale > 0 ? norm3(Fres) / fscale : 0, merr = mscale > 0 ? norm3(Mres) / mscale : 0;

    JsonValue *o = json_object();
    json_set(o, "about_mm", mm3(p));
    json_set(o, "applied_surface_force_n", json_vec3(Fn[0], Fn[1], Fn[2]));
    json_set(o, "applied_surface_moment_nm", json_vec3(Mn[0], Mn[1], Mn[2]));
    if (norm3(m->gravity) > 0) {
        json_set(o, "body_force_n", json_vec3(Fg[0], Fg[1], Fg[2]));
        json_set(o, "body_force_moment_nm", json_vec3(Mg[0], Mg[1], Mg[2]));
    }
    json_set(o, "reaction_force_n", json_vec3(Fr[0], Fr[1], Fr[2]));
    json_set(o, "reaction_moment_nm", json_vec3(Mr[0], Mr[1], Mr[2]));
    json_set(o, "residual_force_n", json_vec3(Fres[0], Fres[1], Fres[2]));
    json_set(o, "residual_moment_nm", json_vec3(Mres[0], Mres[1], Mres[2]));
    json_set_number(o, "force_balance_error", ferr);
    json_set_number(o, "moment_balance_error", merr);
    json_set_bool(o, "force_balance_ok", ferr <= 1e-6);
    json_set_bool(o, "moment_balance_ok", merr <= 1e-6);
    json_set_string(o, "criterion",
                    "force error |sum F| / (|applied| + |body| + |reactions|) and moment error |sum M| / (|M applied| + |M body| + |M reactions| + "
                    "force scale x model diagonal), both at most 1e-6: a discrete linear-elastic solution balances forces and moments to round-off, so "
                    "a larger value means a lost load, a wrong reaction or an unconverged solve");

    JsonValue *sup = json_set_array(o, "supports");
    for (int i = 0; i < m->nbc; i++) {
        if (!bc_is_constraint(m->bc[i].kind)) continue;
        double f[3] = {0, 0, 0}, mo[3] = {0, 0, 0};
        int nodes = 0;
        for (int nd = 0; nd < nn; nd++) {
            if (m->node_bc[nd] != i) continue;
            nodes++;
            const double *x = m->xyz + 3 * (size_t)nd, *rr = s->reaction + 3 * (size_t)nd;
            double arm[3] = {x[0] - p[0], x[1] - p[1], x[2] - p[2]}, t[3];
            cross3(arm, rr, t);
            for (int k = 0; k < 3; k++) f[k] += rr[k], mo[k] += t[k];
        }
        JsonValue *so = json_object();
        json_set_string(so, "condition", m->bc[i].name);
        json_set_string(so, "kind", bc_kind_name(m->bc[i].kind));
        json_set_int(so, "nodes", nodes);
        json_set(so, "force_n", json_vec3(f[0], f[1], f[2]));
        json_set(so, "moment_nm", json_vec3(mo[0], mo[1], mo[2]));
        json_push(sup, so);
    }
    JsonValue *loads = json_set_array(o, "loads");
    for (int i = 0; i < m->nbc; i++) {
        const SaBcInfo *b = &m->bc[i];
        if (bc_is_constraint(b->kind) || bc_is_thermal(b->kind)) continue;
        JsonValue *lo2 = json_object();
        json_set_string(lo2, "condition", b->name);
        json_set_string(lo2, "kind", bc_kind_name(b->kind));
        json_set(lo2, "force_specified_n", json_vec3(b->requested[0], b->requested[1], b->requested[2]));
        json_set(lo2, "force_applied_n", json_vec3(b->applied[0], b->applied[1], b->applied[2]));
        double fs = norm3(b->requested), df[3];
        for (int k = 0; k < 3; k++) df[k] = b->applied[k] - b->requested[k];
        json_set_number(lo2, "force_deviation", fs > 0 ? norm3(df) / fs : 0);
        if (isfinite(b->requested_moment[0]) && isfinite(b->applied_moment[0])) {
            double ms[3], ma[3], pr[3], pa[3], dm[3];
            cross3(p, b->requested, pr);
            cross3(p, b->applied, pa);
            for (int k = 0; k < 3; k++) {
                ms[k] = b->requested_moment[k] - pr[k]; /* moments about p */
                ma[k] = b->applied_moment[k] - pa[k];
                dm[k] = ma[k] - ms[k];
            }
            json_set(lo2, "moment_specified_nm", json_vec3(ms[0], ms[1], ms[2]));
            json_set(lo2, "moment_applied_nm", json_vec3(ma[0], ma[1], ma[2]));
            if (fs > 0) json_set_number(lo2, "line_of_action_shift_mm", 1e3 * norm3(dm) / fs);
        } else {
            json_set_string(lo2, "moment_unavailable", "this result was written before load moments were recorded");
        }
        json_push(loads, lo2);
    }
    return o;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

JsonValue *static_deformation_json(const StaticResults *r) {
    const StaticModel *m = &r->model;
    const SolidResult *s = &r->sol;
    int nn = m->nnodes, ne = m->nelems;
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY}, umax = 0;
    int un = -1;
    for (int i = 0; i < nn; i++) {
        const double *u = s->u + 3 * (size_t)i;
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], m->xyz[3 * (size_t)i + k]), hi[k] = fmax(hi[k], m->xyz[3 * (size_t)i + k]);
        double mag = norm3(u);
        if (mag > umax) umax = mag, un = i;
    }
    double ext[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}, L = nn ? norm3(ext) : 0;
    /* infinitesimal rotation at each element centre: half the curl of the (compatible) displacement field */
    double rotmax = 0;
    int re = -1;
    double *strain = malloc((size_t)(ne > 0 ? ne : 1) * sizeof(double));
    for (int e = 0; e < ne; e++) {
        double X[10][3], N[10], dN[10][3], J[3][3], dNdx[10][3], g[3][3] = {{0}};
        int npe = sa_npe(m);
        elem_coords(m, e, X);
        if (m->elem_type) {
            const double Lc[4] = {0.25, 0.25, 0.25, 0.25};
            tet_shape(m->elem_type, Lc, N, dN);
            if (!(tet_jacobian(m->elem_type, (const double (*)[3])X, (const double (*)[3])dN, J, dNdx) > 0)) continue;
        } else {
            hex8_shape(0, 0, 0, N, dN);
            if (!(hex8_jacobian(X, dN, J, dNdx) > 0)) continue;
        }
        for (int a = 0; a < npe; a++) {
            const double *u = s->u + 3 * (size_t)m->conn[(size_t)npe * e + a];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) g[i][j] += u[i] * dNdx[a][j];
        }
        double w[3] = {0.5 * (g[2][1] - g[1][2]), 0.5 * (g[0][2] - g[2][0]), 0.5 * (g[1][0] - g[0][1])};
        double rot = norm3(w);
        if (rot > rotmax) rotmax = rot, re = e;
        if (strain) strain[e] = r->elem_vm ? r->elem_vm[e] / m->mat[m->elem_mat[e]].E : 0;
    }
    double smax = 0, sp99 = 0;
    if (strain && ne > 0) {
        for (int e = 0; e < ne; e++) smax = fmax(smax, strain[e]);
        qsort(strain, (size_t)ne, sizeof(double), cmp_double);
        sp99 = strain[(size_t)floor(0.99 * (double)(ne - 1))];
    }
    free(strain);
    double ratio = L > 0 ? umax / L : 0, rot_err = 0.5 * rotmax * rotmax;
    /* 0 within the small-deformation assumption, 1 questionable, 2 outside it */
    int cls_u = ratio <= 0.01 ? 0 : (ratio <= 0.05 ? 1 : 2), cls_r = rot_err <= 1e-3 ? 0 : (rot_err <= 1e-2 ? 1 : 2);
    int cls = cls_u > cls_r ? cls_u : cls_r;
    static const char *const NAMES[3] = {"within_small_deformation_assumption", "geometric_nonlinearity_possible", "outside_small_deformation_assumption"};
    JsonValue *o = json_object();
    json_set_number(o, "model_diagonal_mm", 1e3 * L);
    json_set_number(o, "max_displacement_mm", 1e3 * umax);
    if (un >= 0) json_set(o, "max_displacement_at_mm", mm3(m->xyz + 3 * (size_t)un));
    json_set_number(o, "displacement_to_size_ratio", ratio);
    json_set_number(o, "max_rotation_rad", rotmax);
    if (re >= 0) json_set_int(o, "max_rotation_element", re);
    json_set_number(o, "rotation_linearisation_error", rot_err);
    json_set_number(o, "max_strain_indicator", smax);
    json_set_number(o, "p99_strain_indicator", sp99);
    json_set_string(o, "classification", NAMES[cls]);
    json_set_string(o, "criteria",
                    "small displacement: largest displacement at most 1% of the model diagonal (up to 5%: questionable); small rotation: linearised "
                    "kinematics replace cos(theta) by 1, an error of theta^2/2, at most 1e-3 (up to 1e-2: questionable). Beyond either upper limit the "
                    "linear result is outside its assumptions. The strain indicator (Gauss-point von Mises stress / E) is reported, not classified: "
                    "at supports and sharp corners it is singular and grows with refinement");
    if (smax > 0.01)
        json_set_string(o, "strain_note",
                        "local strains above 1% occur; linear elasticity is not reliable there (possible yielding or large strain), even when the "
                        "displacement and rotation criteria are met");
    return o;
}
