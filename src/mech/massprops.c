/* massprops.c - closed-mesh checks, polyhedral moment integrals, fill models (see massprops.h) */
#include "massprops.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmath.h"

static const char *const FILL_NAMES[FILL_MODEL_COUNT] = {"solid", "shell_infill", "measured_mass"};
const char *fill_model_name(FillModel f) { return (unsigned)f < FILL_MODEL_COUNT ? FILL_NAMES[f] : "?"; }
int fill_model_from_name(const char *s) {
    for (int i = 0; s && i < FILL_MODEL_COUNT; i++)
        if (!strcmp(s, FILL_NAMES[i])) return i;
    return -1;
}

/* ------------------------------------------------------------------------------------------------ topology */

typedef struct Edge {
    int a, b;  /* a < b */
    int tri;
    int dir;   /* +1 when the triangle traverses a->b, -1 for b->a */
} Edge;

static int edge_cmp(const void *pa, const void *pb) {
    const Edge *x = pa, *y = pb;
    if (x->a != y->a) return x->a < y->a ? -1 : 1;
    if (x->b != y->b) return x->b < y->b ? -1 : 1;
    return x->tri < y->tri ? -1 : (x->tri > y->tri);
}

static int uf_find(int *p, int x) {
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

static void tri_volume_terms(const double *p0, const double *p1, const double *p2, double *acc) {
    double x0 = p0[0], y0 = p0[1], z0 = p0[2], x1 = p1[0], y1 = p1[1], z1 = p1[2], x2 = p2[0], y2 = p2[1], z2 = p2[2];
    double a1 = x1 - x0, b1 = y1 - y0, c1 = z1 - z0, a2 = x2 - x0, b2 = y2 - y0, c2 = z2 - z0;
    double d0 = b1 * c2 - b2 * c1, d1 = a2 * c1 - a1 * c2, d2 = a1 * b2 - a2 * b1;
#define SUBEXPR(w0, w1, w2, f1, f2, f3, g0, g1, g2)                                                                                       \
    double f1, f2, f3, g0, g1, g2;                                                                                                          \
    {                                                                                                                                       \
        double t0 = w0 + w1;                                                                                                                \
        f1 = t0 + w2;                                                                                                                       \
        double t1 = w0 * w0, t2 = t1 + w1 * t0;                                                                                             \
        f2 = t2 + w2 * f1;                                                                                                                  \
        f3 = w0 * t1 + w1 * t2 + w2 * f2;                                                                                                   \
        g0 = f2 + w0 * (f1 + w0);                                                                                                           \
        g1 = f2 + w1 * (f1 + w1);                                                                                                           \
        g2 = f2 + w2 * (f1 + w2);                                                                                                           \
    }
    SUBEXPR(x0, x1, x2, f1x, f2x, f3x, g0x, g1x, g2x)
    SUBEXPR(y0, y1, y2, f1y, f2y, f3y, g0y, g1y, g2y)
    SUBEXPR(z0, z1, z2, f1z, f2z, f3z, g0z, g1z, g2z)
#undef SUBEXPR
    (void)f1y, (void)f1z, (void)f3x, (void)f3y, (void)f3z;
    acc[0] += d0 * f1x;
    acc[1] += d0 * f2x;
    acc[2] += d1 * f2y;
    acc[3] += d2 * f2z;
    acc[4] += d0 * f3x;
    acc[5] += d1 * f3y;
    acc[6] += d2 * f3z;
    acc[7] += d0 * (y0 * g0x + y1 * g1x + y2 * g2x);
    acc[8] += d1 * (z0 * g0y + z1 * g1y + z2 * g2y);
    acc[9] += d2 * (x0 * g0z + x1 * g1z + x2 * g2z);
}

static void bbox_centre(const double *v, const int *tri, int nt, double c[3]) {
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int t = 0; t < 3 * nt; t++) {
        const double *p = v + 3 * tri[t];
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], p[k]), hi[k] = fmax(hi[k], p[k]);
    }
    for (int k = 0; k < 3; k++) c[k] = nt ? 0.5 * (lo[k] + hi[k]) : 0;
}

/* shifts integrals taken about centre c to the coordinate origin */
static void shift_integrals(MomentIntegrals *o, const double c[3]) {
    double M = o->zeroth, F[3] = {o->first[0], o->first[1], o->first[2]};
    /* x = x' + c: int x = int x' + c M; int x_i x_j = int x'_i x'_j + c_i int x'_j + c_j int x'_i + c_i c_j M */
    double S[6];
    static const int I[6] = {0, 1, 2, 0, 1, 2}, J[6] = {0, 1, 2, 1, 2, 0};
    for (int k = 0; k < 6; k++) S[k] = o->second[k] + c[I[k]] * F[J[k]] + c[J[k]] * F[I[k]] + c[I[k]] * c[J[k]] * M;
    memcpy(o->second, S, sizeof S);
    for (int k = 0; k < 3; k++) o->first[k] = F[k] + c[k] * M;
}

void mesh_volume_integrals(const double *v, const int *tri, int nt, MomentIntegrals *out) {
    double c[3], acc[10] = {0};
    bbox_centre(v, tri, nt, c);
    for (int t = 0; t < nt; t++) {
        double p[3][3];
        for (int k = 0; k < 3; k++) mv3_sub(p[k], v + 3 * tri[3 * t + k], c);
        tri_volume_terms(p[0], p[1], p[2], acc);
    }
    out->zeroth = acc[0] / 6;
    for (int k = 0; k < 3; k++) out->first[k] = acc[1 + k] / 24;
    out->second[0] = acc[4] / 60, out->second[1] = acc[5] / 60, out->second[2] = acc[6] / 60;
    out->second[3] = acc[7] / 120, out->second[4] = acc[8] / 120, out->second[5] = acc[9] / 120;
    shift_integrals(out, c);
}

void mesh_surface_integrals(const double *v, const int *tri, int nt, MomentIntegrals *out) {
    double c[3];
    bbox_centre(v, tri, nt, c);
    memset(out, 0, sizeof *out);
    for (int t = 0; t < nt; t++) {
        double p[3][3], e1[3], e2[3], n[3], s[3] = {0, 0, 0};
        for (int k = 0; k < 3; k++) {
            mv3_sub(p[k], v + 3 * tri[3 * t + k], c);
            mv3_addto(s, p[k]);
        }
        mv3_sub(e1, p[1], p[0]);
        mv3_sub(e2, p[2], p[0]);
        mv3_cross(n, e1, e2);
        double A = 0.5 * mv3_norm(n);
        out->zeroth += A;
        for (int k = 0; k < 3; k++) out->first[k] += A * s[k] / 3;
        static const int I[6] = {0, 1, 2, 0, 1, 2}, J[6] = {0, 1, 2, 1, 2, 0};
        for (int k = 0; k < 6; k++) {
            double pp = p[0][I[k]] * p[0][J[k]] + p[1][I[k]] * p[1][J[k]] + p[2][I[k]] * p[2][J[k]];
            out->second[k] += A / 12 * (pp + s[I[k]] * s[J[k]]);
        }
    }
    shift_integrals(out, c);
}

bool mesh_topology(const double *v, int nv, const int *tri, int nt, MeshTopology *out) {
    memset(out, 0, sizeof *out);
    out->ntri = nt, out->nvert = nv;
    for (int k = 0; k < 3; k++) out->bmin[k] = INFINITY, out->bmax[k] = -INFINITY;
    if (nt <= 0) return false;
    for (int i = 0; i < 3 * nt; i++)
        if (tri[i] < 0 || tri[i] >= nv) return false;
    for (int i = 0; i < nv; i++)
        for (int k = 0; k < 3; k++) out->bmin[k] = fmin(out->bmin[k], v[3 * i + k]), out->bmax[k] = fmax(out->bmax[k], v[3 * i + k]);
    double ext = 0;
    for (int k = 0; k < 3; k++) ext = fmax(ext, out->bmax[k] - out->bmin[k]);
    Edge *E = malloc((size_t)(3 * nt) * sizeof *E);
    int *parent = malloc((size_t)nt * sizeof *parent);
    if (!E || !parent) {
        free(E), free(parent);
        return false;
    }
    for (int t = 0; t < nt; t++) {
        parent[t] = t;
        const int *ix = tri + 3 * t;
        double e1[3], e2[3], n[3];
        mv3_sub(e1, v + 3 * ix[1], v + 3 * ix[0]);
        mv3_sub(e2, v + 3 * ix[2], v + 3 * ix[0]);
        mv3_cross(n, e1, e2);
        out->area += 0.5 * mv3_norm(n);
        if (ix[0] == ix[1] || ix[1] == ix[2] || ix[0] == ix[2] || mv3_norm(n) <= 1e-12 * ext * ext) out->degenerate_triangles++;
        for (int k = 0; k < 3; k++) {
            int a = ix[k], b = ix[(k + 1) % 3];
            Edge *e = &E[3 * t + k];
            e->a = a < b ? a : b, e->b = a < b ? b : a, e->tri = t, e->dir = a < b ? 1 : -1;
        }
    }
    qsort(E, (size_t)(3 * nt), sizeof *E, edge_cmp);
    for (int i = 0; i < 3 * nt;) {
        int j = i;
        while (j < 3 * nt && E[j].a == E[i].a && E[j].b == E[i].b) j++;
        int cnt = j - i;
        if (cnt == 1)
            out->boundary_edges++;
        else if (cnt > 2)
            out->nonmanifold_edges++;
        else if (E[i].dir == E[i + 1].dir)
            out->inconsistent_edges++;
        for (int k = i + 1; k < j; k++) {
            int ra = uf_find(parent, E[i].tri), rb = uf_find(parent, E[k].tri);
            if (ra != rb) parent[ra] = rb;
        }
        i = j;
    }
    /* components and their signed volumes */
    int *root_index = malloc((size_t)nt * sizeof *root_index);
    double *vol = calloc((size_t)nt, sizeof *vol);
    bool *open = calloc((size_t)nt, sizeof *open);
    if (!root_index || !vol || !open) {
        free(E), free(parent), free(root_index), free(vol), free(open);
        return false;
    }
    for (int t = 0; t < nt; t++) root_index[t] = -1;
    int ncomp = 0;
    int *comp_of = malloc((size_t)nt * sizeof *comp_of);
    if (!comp_of) {
        free(E), free(parent), free(root_index), free(vol), free(open);
        return false;
    }
    double c[3];
    bbox_centre(v, tri, nt, c);
    for (int t = 0; t < nt; t++) {
        int r = uf_find(parent, t);
        if (root_index[r] < 0) root_index[r] = ncomp++;
        comp_of[t] = root_index[r];
        double p[3][3], acc[10] = {0};
        for (int k = 0; k < 3; k++) mv3_sub(p[k], v + 3 * tri[3 * t + k], c);
        tri_volume_terms(p[0], p[1], p[2], acc);
        vol[comp_of[t]] += acc[0] / 6;
        out->signed_volume += acc[0] / 6;
    }
    for (int i = 0; i < 3 * nt;) {
        int j = i;
        while (j < 3 * nt && E[j].a == E[i].a && E[j].b == E[i].b) j++;
        if (j - i != 2)
            for (int k = i; k < j; k++) open[comp_of[E[k].tri]] = true;
        i = j;
    }
    out->components = ncomp;
    for (int k = 0; k < ncomp; k++) {
        if (open[k])
            out->open_components++;
        else if (vol[k] > 0)
            out->positive_shells++;
        else
            out->negative_shells++;
    }
    out->closed = out->boundary_edges == 0 && out->nonmanifold_edges == 0;
    out->oriented = out->inconsistent_edges == 0;
    free(E), free(parent), free(root_index), free(vol), free(open), free(comp_of);
    return true;
}

/* ------------------------------------------------------------------------------------------------ inertia helpers */

void inertia_about_point(const double *Ic, double m, const double *com, const double *point, double *Ip) {
    double d[3];
    mv3_sub(d, com, point);
    double dd = mv3_dot(d, d);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) Ip[3 * r + k] = Ic[3 * r + k] + m * ((r == k ? dd : 0) - d[r] * d[k]);
}

void inertia_to_com(const double *Ip, double m, const double *com, const double *point, double *Ic) {
    double d[3];
    mv3_sub(d, com, point);
    double dd = mv3_dot(d, d);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) Ic[3 * r + k] = Ip[3 * r + k] - m * ((r == k ? dd : 0) - d[r] * d[k]);
}

void inertia_rotate(const double *R, const double *I, double *out) { mm3_rotate_sym(out, R, I); }

void inertia_principal(const double *I, double w[3], double axes[9]) {
    double a[9];
    memcpy(a, I, sizeof a);
    mm_sym_eig(a, 3, w, axes);
    if (mm3_det(axes) < 0)
        for (int r = 0; r < 3; r++) axes[3 * r + 2] = -axes[3 * r + 2];
}

bool inertia_admissible(const double *I, double tol, char *why, int whylen) {
    double scale = 0, asym = 0;
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            scale = fmax(scale, fabs(I[3 * r + k]));
            asym = fmax(asym, fabs(I[3 * r + k] - I[3 * k + r]));
            if (!isfinite(I[3 * r + k])) {
                if (why) snprintf(why, (size_t)whylen, "inertia has non-finite entries");
                return false;
            }
        }
    if (!(scale > 0)) {
        if (why) snprintf(why, (size_t)whylen, "inertia is zero");
        return false;
    }
    if (asym > tol * scale) {
        if (why) snprintf(why, (size_t)whylen, "inertia is not symmetric (asymmetry %.3g of %.3g)", asym, scale);
        return false;
    }
    double w[3], ax[9];
    inertia_principal(I, w, ax);
    if (!(w[2] > 0)) {
        if (why) snprintf(why, (size_t)whylen, "principal moment %.6g is not positive", w[2]);
        return false;
    }
    if (w[1] + w[2] < w[0] * (1 - tol)) {
        if (why) snprintf(why, (size_t)whylen, "principal moments %.6g, %.6g, %.6g violate I2 + I3 >= I1", w[0], w[1], w[2]);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------------------------------ mass properties */

static void props_from_moments(const MomentIntegrals *mi, double scale, MassProperties *mp) {
    double m = mi->zeroth * scale;
    mp->mass = m;
    for (int k = 0; k < 3; k++) mp->com[k] = m > 0 ? mi->first[k] * scale / m : 0;
    double xx = mi->second[0] * scale, yy = mi->second[1] * scale, zz = mi->second[2] * scale;
    double xy = mi->second[3] * scale, yz = mi->second[4] * scale, zx = mi->second[5] * scale;
    double Io[9] = {yy + zz, -xy, -zx, -xy, zz + xx, -yz, -zx, -yz, xx + yy};
    double origin[3] = {0, 0, 0};
    inertia_to_com(Io, m, mp->com, origin, mp->inertia_com);
    /* exact symmetry after round-off */
    for (int r = 0; r < 3; r++)
        for (int k = r + 1; k < 3; k++) {
            double a = 0.5 * (mp->inertia_com[3 * r + k] + mp->inertia_com[3 * k + r]);
            mp->inertia_com[3 * r + k] = mp->inertia_com[3 * k + r] = a;
        }
    inertia_principal(mp->inertia_com, mp->principal, mp->axes);
}

bool mass_properties_from_mesh(const double *v, int nv, const int *tri, int nt, const FillSpec *fill, MassProperties *mp, MechDiag *d,
                               const char *subject) {
    memset(mp, 0, sizeof *mp);
    MeshTopology topo;
    if (!mesh_topology(v, nv, tri, nt, &topo)) {
        mdiag_add(d, MD_ERROR, "MESH_INVALID", subject, NULL, "mesh has no triangles or invalid vertex indices");
        return false;
    }
    bool ok = true;
    if (topo.boundary_edges) {
        mdiag_add(d, MD_ERROR, "MESH_NOT_CLOSED", subject,
                  "mass properties need a watertight surface: repair the mesh, or give measured mass, centre of mass and inertia",
                  "%d boundary edges: the surface does not enclose a volume", topo.boundary_edges);
        ok = false;
    }
    if (topo.nonmanifold_edges) {
        mdiag_add(d, MD_ERROR, "MESH_NONMANIFOLD", subject, "separate the touching solids or repair the mesh",
                  "%d edges are shared by more than two triangles", topo.nonmanifold_edges);
        ok = false;
    }
    if (topo.inconsistent_edges) {
        mdiag_add(d, MD_ERROR, "MESH_INCONSISTENT_ORIENTATION", subject, "re-orient the triangles consistently (outward normals)",
                  "%d shared edges are traversed in the same direction by both triangles", topo.inconsistent_edges);
        ok = false;
    }
    if (ok && !(topo.signed_volume > 0)) {
        mdiag_add(d, MD_ERROR, "MESH_INVERTED", subject, "flip the triangle winding so normals point outward",
                  "enclosed signed volume is %.6g m^3: the surface is oriented inward", topo.signed_volume);
        ok = false;
    }
    if (topo.degenerate_triangles)
        mdiag_add(d, MD_WARNING, "MESH_DEGENERATE_TRIANGLES", subject, NULL, "%d degenerate (zero-area) triangles were ignored by the integrals",
                  topo.degenerate_triangles);
    if (!ok) return false;
    if (topo.negative_shells)
        mdiag_add(d, MD_INFO, "MESH_CAVITIES", subject, NULL,
                  "%d inward-facing inner shell(s) subtract volume: treated as explicit internal cavities", topo.negative_shells);
    if (topo.positive_shells > 1)
        mdiag_add(d, MD_WARNING, "MESH_MULTIPLE_SOLIDS", subject, "import the pieces as separate bodies if they move relative to each other",
                  "%d disjoint closed solids are treated as one rigid body", topo.positive_shells);

    MomentIntegrals vol, surf;
    mesh_volume_integrals(v, tri, nt, &vol);
    mesh_surface_integrals(v, tri, nt, &surf);
    mp->volume = vol.zeroth;
    mp->area = surf.zeroth;
    switch (fill ? fill->model : FILL_MODEL_COUNT) {
    case FILL_SOLID:
        if (!(fill->density > 0)) {
            mdiag_add(d, MD_MISSING_INPUT, "DENSITY_REQUIRED", subject, "give the density of the printed or machined material", "solid fill needs a density");
            return false;
        }
        props_from_moments(&vol, fill->density, mp);
        mdiag_add(d, MD_INFO, "FILL_ASSUMPTION", subject, NULL, "uniform solid material of density %.6g kg/m^3 over %.6g cm^3", fill->density,
                  mp->volume * 1e6);
        break;
    case FILL_SHELL_INFILL: {
        if (!(fill->density > 0) || !(fill->shell_thickness > 0) || !(fill->infill_fraction >= 0 && fill->infill_fraction <= 1)) {
            mdiag_add(d, MD_MISSING_INPUT, "SHELL_INFILL_PARAMETERS", subject,
                      "give density, shell_thickness (walls * extrusion width, or top/bottom thickness if larger) and infill_fraction (0..1)",
                      "shell_infill fill needs density > 0, shell_thickness > 0 and infill_fraction in [0, 1]");
            return false;
        }
        double t = fill->shell_thickness, phi = fill->infill_fraction;
        double wall = surf.zeroth * t;
        mp->wall_fraction = wall / vol.zeroth;
        if (mp->wall_fraction >= 1) {
            mdiag_add(d, MD_WARNING, "SHELL_EXCEEDS_PART", subject, NULL,
                      "wall volume (area x thickness = %.4g cm^3) exceeds the enclosed volume (%.4g cm^3): the part is treated as solid", wall * 1e6,
                      vol.zeroth * 1e6);
            props_from_moments(&vol, fill->density, mp);
            mp->wall_fraction = 1;
            break;
        }
        MomentIntegrals mix;
        mix.zeroth = t * surf.zeroth + phi * (vol.zeroth - t * surf.zeroth);
        for (int k = 0; k < 3; k++) mix.first[k] = t * surf.first[k] + phi * (vol.first[k] - t * surf.first[k]);
        for (int k = 0; k < 6; k++) mix.second[k] = t * surf.second[k] + phi * (vol.second[k] - t * surf.second[k]);
        props_from_moments(&mix, fill->density, mp);
        mp->volume = vol.zeroth;
        mdiag_add(d, MD_INFO, "FILL_ASSUMPTION", subject, NULL,
                  "homogenised print: %.3g mm walls at %.6g kg/m^3 (thin-shell approximation, walls are %.1f%% of the volume) and %.0f%% infill elsewhere",
                  t * 1e3, fill->density, 100 * mp->wall_fraction, 100 * phi);
        if (mp->wall_fraction > 0.3)
            mdiag_add(d, MD_WARNING, "THIN_SHELL_APPROXIMATION", subject,
                      "for thick walls relative to the part, model the internal geometry explicitly or measure mass and inertia",
                      "walls occupy %.0f%% of the volume: the thin-shell approximation (wall mass on the outer surface) overestimates inertia",
                      100 * mp->wall_fraction);
        break;
    }
    case FILL_MEASURED_MASS:
        if (!(fill->measured_mass > 0)) {
            mdiag_add(d, MD_MISSING_INPUT, "MEASURED_MASS_REQUIRED", subject, "weigh the part", "measured_mass fill needs the measured mass");
            return false;
        }
        props_from_moments(&vol, fill->measured_mass / vol.zeroth, mp);
        mdiag_add(d, MD_WARNING, "UNIFORM_DISTRIBUTION_ASSUMED", subject,
                  "for sparse infill use shell_infill or measure the centre of mass and inertia (for example with a pendulum test)",
                  "measured mass %.6g kg spread uniformly over %.6g cm^3 (effective density %.6g kg/m^3): centre of mass and inertia assume a "
                  "uniform distribution",
                  fill->measured_mass, vol.zeroth * 1e6, fill->measured_mass / vol.zeroth);
        if (fill->density > 0)
            mdiag_add(d, MD_INFO, "IMPLIED_FILL", subject, NULL, "measured mass implies %.1f%% of a solid part of density %.6g kg/m^3",
                      100 * fill->measured_mass / (fill->density * vol.zeroth), fill->density);
        break;
    default:
        mdiag_add(d, MD_MISSING_INPUT, "FILL_MODEL_REQUIRED", subject,
                  "choose solid, shell_infill (walls, infill fraction) or measured_mass; an outer surface does not say whether a printed part is solid",
                  "mass properties from a mesh need a declared fill model");
        return false;
    }
    mp->effective_density = mp->mass / mp->volume;
    char why[200];
    if (!inertia_admissible(mp->inertia_com, 1e-9, why, sizeof why)) {
        mdiag_add(d, MD_ERROR, "INERTIA_NOT_ADMISSIBLE", subject, NULL, "computed inertia is not physical: %s", why);
        return false;
    }
    return true;
}
