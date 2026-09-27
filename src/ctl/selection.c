/* selection.c - query evaluation, patch aggregation, statistics, hashing, revalidation, mesh projection */
#include "selection.h"
#include "../core/sha256.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    Body *b;
    const SelectionContext *ctx;
    NvErr code;
    char *err;
    size_t errlen;
    int depth;
} Eval;

static bool fail(Eval *ev, NvErr code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool fail(Eval *ev, NvErr code, const char *fmt, ...) {
    ev->code = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev->err, ev->errlen, fmt, ap);
    va_end(ap);
    return false;
}

static bool mm_vec3(const JsonValue *v, double out[3]) {
    double t[3];
    if (!json_get_numbers(v, t, 3)) return false;
    for (int k = 0; k < 3; k++) out[k] = 1e-3 * t[k];
    return true;
}

static bool direction(Eval *ev, const JsonValue *v, double d[3]) {
    const char *s = json_str(v);
    if (s) {
        static const struct {
            const char *name;
            double d[3];
        } D[] = {{"+x", {1, 0, 0}}, {"-x", {-1, 0, 0}}, {"+y", {0, 1, 0}}, {"-y", {0, -1, 0}}, {"+z", {0, 0, 1}},
                 {"-z", {0, 0, -1}}, {"up", {0, 0, 1}},  {"down", {0, 0, -1}}};
        for (size_t i = 0; i < sizeof D / sizeof D[0]; i++)
            if (!strcmp(s, D[i].name)) {
                memcpy(d, D[i].d, 3 * sizeof(double));
                return true;
            }
        return fail(ev, NV_ERR_INVALID_PARAMS, "unknown direction '%s' (use +x -x +y -y +z -z up down or [x, y, z])", s);
    }
    if (!json_get_numbers(v, d, 3)) return fail(ev, NV_ERR_INVALID_PARAMS, "direction must be a name or a vector of 3 numbers");
    double l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 0)) return fail(ev, NV_ERR_INVALID_PARAMS, "direction vector must not be zero");
    for (int k = 0; k < 3; k++) d[k] /= l;
    return true;
}

bool direction_from_json(const JsonValue *v, double d[3], char *err, size_t errlen) {
    Eval ev;
    memset(&ev, 0, sizeof ev);
    ev.err = err, ev.errlen = errlen;
    return direction(&ev, v, d);
}

static const double *bv(const Body *b, int t, int q) { return b->build_v + 3 * (size_t)b->surf.tri[3 * t + q]; }

static void mark_patch(const Body *b, int p, unsigned char *mask) {
    for (int k = b->patches.start[p]; k < b->patches.start[p + 1]; k++) mask[b->patches.order[k]] = 1;
}

static bool eval(Eval *ev, const JsonValue *q, unsigned char *mask);

static bool eval_list(Eval *ev, const JsonValue *arr, unsigned char *mask, bool conj) {
    Body *b = ev->b;
    int nt = b->surf.nt;
    if (!arr || arr->type != JSON_ARRAY || !json_len(arr)) return fail(ev, NV_ERR_INVALID_PARAMS, "\"all\"/\"any\" need a non-empty array of queries");
    unsigned char *tmp = malloc((size_t)nt);
    if (!tmp) return fail(ev, NV_ERR_RESOURCE_LIMIT, "out of memory");
    memset(mask, conj ? 1 : 0, (size_t)nt);
    for (size_t i = 0; i < json_len(arr); i++) {
        if (!eval(ev, json_at(arr, i), tmp)) {
            free(tmp);
            return false;
        }
        for (int t = 0; t < nt; t++) mask[t] = conj ? (mask[t] & tmp[t]) : (mask[t] | tmp[t]);
    }
    free(tmp);
    return true;
}

static bool eval(Eval *ev, const JsonValue *q, unsigned char *mask) {
    Body *b = ev->b;
    int nt = b->surf.nt, np = b->patches.n;
    if (++ev->depth > 32) return fail(ev, NV_ERR_INVALID_PARAMS, "selection query nested too deeply");
    if (!q || q->type != JSON_OBJECT || json_len(q) != 1)
        return fail(ev, NV_ERR_INVALID_PARAMS, "each query node must be an object with exactly one key (e.g. {\"facing\": {...}})");
    const char *key = json_key_at(q, 0);
    const JsonValue *v = json_value_at(q, 0);
    memset(mask, 0, (size_t)nt);
    bool ok = true;
    if (!strcmp(key, "all") || !strcmp(key, "any")) {
        ok = eval_list(ev, v, mask, key[1] == 'l');
    } else if (!strcmp(key, "not")) {
        ok = eval(ev, v, mask);
        if (ok)
            for (int t = 0; t < nt; t++) mask[t] = !mask[t];
    } else if (!strcmp(key, "patches")) {
        if (!v || v->type != JSON_ARRAY || !json_len(v)) return fail(ev, NV_ERR_INVALID_PARAMS, "\"patches\" needs a non-empty array of patch ids");
        for (size_t i = 0; i < json_len(v); i++) {
            const JsonValue *id = json_at(v, i);
            if (!json_is_integer(id) || id->u.number < 0 || id->u.number >= np)
                return fail(ev, NV_ERR_NOT_FOUND, "patch id %s does not exist on body '%s' (valid 0..%d)", id && id->type == JSON_NUMBER ? "given" : "?", b->name, np - 1);
            mark_patch(b, (int)id->u.number, mask);
        }
    } else if (!strcmp(key, "triangles")) {
        if (!v || v->type != JSON_ARRAY || !json_len(v)) return fail(ev, NV_ERR_INVALID_PARAMS, "\"triangles\" needs a non-empty array of triangle ids");
        for (size_t i = 0; i < json_len(v); i++) {
            const JsonValue *id = json_at(v, i);
            if (!json_is_integer(id) || id->u.number < 0 || id->u.number >= nt) return fail(ev, NV_ERR_NOT_FOUND, "triangle id out of range (valid 0..%d)", nt - 1);
            mask[(int)id->u.number] = 1;
        }
    } else if (!strcmp(key, "adjacent_to")) {
        const JsonValue *ids = json_get(v, "patches");
        if (!ids || ids->type != JSON_ARRAY || !json_len(ids)) return fail(ev, NV_ERR_INVALID_PARAMS, "\"adjacent_to\" needs {\"patches\": [ids]}");
        for (size_t i = 0; i < json_len(ids); i++) {
            const JsonValue *id = json_at(ids, i);
            if (!json_is_integer(id) || id->u.number < 0 || id->u.number >= np)
                return fail(ev, NV_ERR_NOT_FOUND, "patch id out of range in adjacent_to (valid 0..%d)", np - 1);
            int p = (int)id->u.number;
            for (int k = b->patches.adj_start[p]; k < b->patches.adj_start[p + 1]; k++) mark_patch(b, b->patches.adj[k], mask);
        }
    } else if (!strcmp(key, "facing")) {
        double d[3];
        if (!direction(ev, json_get(v, "direction"), d)) return false;
        double ang = json_get_num(v, "max_angle_deg", 10);
        if (!(ang >= 0 && ang <= 180)) return fail(ev, NV_ERR_OUT_OF_RANGE, "max_angle_deg must be within 0..180");
        double c = cos(ang * M_PI / 180.0);
        for (int t = 0; t < nt; t++) {
            const double *n = b->build_normal + 3 * (size_t)t;
            mask[t] = n[0] * d[0] + n[1] * d[1] + n[2] * d[2] >= c - 1e-12;
        }
    } else if (!strcmp(key, "plane")) {
        double tol = 1e-3 * json_get_num(v, "tolerance", 0.01);
        const char *axis = json_get_str(v, "axis", NULL);
        if (axis) {
            int k = axis[0] - 'x';
            if (k < 0 || k > 2 || axis[1]) return fail(ev, NV_ERR_INVALID_PARAMS, "plane axis must be x, y or z");
            const JsonValue *at = json_get(v, "at");
            double val;
            if (json_str(at) && !strcmp(json_str(at), "min")) val = b->bmin[k];
            else if (json_str(at) && !strcmp(json_str(at), "max")) val = b->bmax[k];
            else if (at && at->type == JSON_NUMBER) val = 1e-3 * at->u.number;
            else return fail(ev, NV_ERR_INVALID_PARAMS, "plane \"at\" must be \"min\", \"max\" or a coordinate in mm");
            for (int t = 0; t < nt; t++) {
                bool on = true;
                for (int qq = 0; qq < 3 && on; qq++) on = fabs(bv(b, t, qq)[k] - val) <= tol;
                mask[t] = on;
            }
        } else {
            double p[3], n[3];
            if (!mm_vec3(json_get(v, "point"), p)) return fail(ev, NV_ERR_INVALID_PARAMS, "plane needs \"axis\" and \"at\", or \"point\" [mm] and \"normal\"");
            if (!direction(ev, json_get(v, "normal"), n)) return false;
            for (int t = 0; t < nt; t++) {
                bool on = true;
                for (int qq = 0; qq < 3 && on; qq++) {
                    const double *x = bv(b, t, qq);
                    on = fabs((x[0] - p[0]) * n[0] + (x[1] - p[1]) * n[1] + (x[2] - p[2]) * n[2]) <= tol;
                }
                mask[t] = on;
            }
        }
    } else if (!strcmp(key, "extreme")) {
        double d[3];
        if (!direction(ev, json_get(v, "direction"), d)) return false;
        double tol = 1e-3 * json_get_num(v, "tolerance", 0.01);
        double smax = -INFINITY;
        for (int i = 0; i < b->surf.nv; i++) {
            const double *x = b->build_v + 3 * (size_t)i;
            smax = fmax(smax, x[0] * d[0] + x[1] * d[1] + x[2] * d[2]);
        }
        for (int t = 0; t < nt; t++) {
            bool on = true;
            for (int qq = 0; qq < 3 && on; qq++) {
                const double *x = bv(b, t, qq);
                on = x[0] * d[0] + x[1] * d[1] + x[2] * d[2] >= smax - tol;
            }
            mask[t] = on;
        }
    } else if (!strcmp(key, "box")) {
        double lo[3], hi[3];
        if (!mm_vec3(json_get(v, "min"), lo) || !mm_vec3(json_get(v, "max"), hi)) return fail(ev, NV_ERR_INVALID_PARAMS, "box needs \"min\" and \"max\" [x, y, z] in mm");
        for (int k = 0; k < 3; k++)
            if (lo[k] > hi[k]) return fail(ev, NV_ERR_INVALID_PARAMS, "box min must not exceed max");
        for (int t = 0; t < nt; t++) {
            const double *c = b->build_centroid + 3 * (size_t)t;
            mask[t] = c[0] >= lo[0] && c[0] <= hi[0] && c[1] >= lo[1] && c[1] <= hi[1] && c[2] >= lo[2] && c[2] <= hi[2];
        }
    } else if (!strcmp(key, "sphere")) {
        double c0[3];
        if (!mm_vec3(json_get(v, "center"), c0)) return fail(ev, NV_ERR_INVALID_PARAMS, "sphere needs \"center\" [mm] and \"radius\" (mm)");
        double r = 1e-3 * json_get_num(v, "radius", -1);
        if (!(r > 0)) return fail(ev, NV_ERR_OUT_OF_RANGE, "sphere radius must be positive");
        for (int t = 0; t < nt; t++) {
            const double *c = b->build_centroid + 3 * (size_t)t;
            double dx = c[0] - c0[0], dy = c[1] - c0[1], dz = c[2] - c0[2];
            mask[t] = dx * dx + dy * dy + dz * dz <= r * r;
        }
    } else if (!strcmp(key, "near")) {
        double p[3];
        if (!mm_vec3(json_get(v, "point"), p)) return fail(ev, NV_ERR_INVALID_PARAMS, "near needs \"point\" [mm] and \"distance\" (mm)");
        double dist = 1e-3 * json_get_num(v, "distance", -1);
        if (!(dist >= 0)) return fail(ev, NV_ERR_OUT_OF_RANGE, "near distance must be >= 0");
        for (int t = 0; t < nt; t++) {
            double c[3];
            closest_point_triangle(p, bv(b, t, 0), bv(b, t, 1), bv(b, t, 2), c);
            double dx = c[0] - p[0], dy = c[1] - p[1], dz = c[2] - p[2];
            mask[t] = dx * dx + dy * dy + dz * dz <= dist * dist;
        }
    } else if (!strcmp(key, "type")) {
        const char *ty = json_str(v);
        if (!ty) return fail(ev, NV_ERR_INVALID_PARAMS, "type must be \"planar\", \"cylindrical\" or \"other\"");
        for (int p = 0; p < np; p++)
            if (!strcmp(patch_type_name(b->patches.type[p]), ty)) mark_patch(b, p, mask);
    } else if (!strcmp(key, "cylinder") || !strcmp(key, "area")) {
        bool cyl = key[0] == 'c';
        double lo = cyl ? 1e-3 * json_get_num(v, "radius_min", 0) : 1e-6 * json_get_num(v, "min", 0);
        double hi = cyl ? 1e-3 * json_get_num(v, "radius_max", INFINITY) : 1e-6 * json_get_num(v, "max", INFINITY);
        const JsonValue *hole = json_get(v, "hole");
        for (int p = 0; p < np; p++) {
            if (cyl && b->patches.type[p] != PATCH_CYLINDRICAL) continue;
            PatchGeom g;
            patch_geometry(&b->surf, b->build_v, b->place.R, &b->patches, p, &g);
            double s2 = b->unit_scale * b->unit_scale;
            (void)s2;
            double val = cyl ? g.radius : 0;
            if (!cyl) {
                val = 0;
                for (int k = b->patches.start[p]; k < b->patches.start[p + 1]; k++) val += b->build_area[b->patches.order[k]];
            }
            if (cyl && g.type != PATCH_CYLINDRICAL) continue;
            if (val < lo || val > hi) continue;
            if (cyl && hole && hole->type == JSON_BOOL && g.concave != hole->u.boolean) continue;
            mark_patch(b, p, mask);
        }
    } else if (!strcmp(key, "pick")) {
        const char *view = json_get_str(v, "view_id", NULL);
        double px[2];
        if (!view || !json_get_numbers(json_get(v, "pixel"), px, 2)) return fail(ev, NV_ERR_INVALID_PARAMS, "pick needs \"view_id\" and \"pixel\" [x, y]");
        if (!ev->ctx || !ev->ctx->pick) return fail(ev, NV_ERR_UNSUPPORTED, "picking is not available in this context");
        int tri = -1;
        char perr[256] = "";
        /* pixel centres are at +0.5: the given integer pixel refers to its centre */
        if (!ev->ctx->pick(ev->ctx->pick_ctx, view, px[0] + 0.5, px[1] + 0.5, b, &tri, perr, sizeof perr))
            return fail(ev, NV_ERR_NOT_FOUND, "pick: %s", perr);
        mask[tri] = 1;
        mark_patch(b, b->patches.tri_patch[tri], mask);
        if (json_get_bool(v, "triangle_only", false)) {
            memset(mask, 0, (size_t)nt);
            mask[tri] = 1;
        }
    } else {
        ok = fail(ev, NV_ERR_INVALID_PARAMS, "unknown selection predicate \"%s\"", key);
    }
    ev->depth--;
    return ok;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : (x > y);
}

typedef struct {
    int p;
    double area;
} PatchArea;

static int cmp_parea(const void *a, const void *b) {
    const PatchArea *x = a, *y = b;
    if (x->area != y->area) return x->area > y->area ? -1 : 1;
    return x->p - y->p;
}

bool selection_resolve(Body *b, Selection *sel, const SelectionContext *ctx, NvErr *code, char *err, size_t errlen) {
    int nt = b->surf.nt, np = b->patches.n;
    unsigned char *mask = malloc((size_t)(nt ? nt : 1));
    if (!mask) {
        *code = NV_ERR_RESOURCE_LIMIT;
        snprintf(err, errlen, "out of memory");
        return false;
    }
    Eval ev = {b, ctx, NV_OK, err, errlen, 0};
    if (!eval(&ev, sel->query, mask)) {
        *code = ev.code;
        free(mask);
        return false;
    }
    if (sel->mode == SEL_MODE_PATCH) {
        double *matched = calloc((size_t)(np ? np : 1), sizeof(double)), *total = calloc((size_t)(np ? np : 1), sizeof(double));
        PatchArea *pa = malloc((size_t)(np ? np : 1) * sizeof(PatchArea));
        if (!matched || !total || !pa) {
            free(matched), free(total), free(pa), free(mask);
            *code = NV_ERR_RESOURCE_LIMIT;
            snprintf(err, errlen, "out of memory");
            return false;
        }
        for (int t = 0; t < nt; t++) {
            int p = b->patches.tri_patch[t];
            total[p] += b->build_area[t];
            if (mask[t]) matched[p] += b->build_area[t];
        }
        int npick = 0;
        for (int p = 0; p < np; p++)
            if (total[p] > 0 && matched[p] >= sel->coverage * total[p] - 1e-18 && matched[p] > 0) pa[npick++] = (PatchArea){p, total[p]};
        qsort(pa, (size_t)npick, sizeof *pa, cmp_parea);
        if (sel->keep_largest > 0 && npick > sel->keep_largest) npick = sel->keep_largest;
        memset(mask, 0, (size_t)nt);
        for (int i = 0; i < npick; i++) mark_patch(b, pa[i].p, mask);
        free(matched), free(total), free(pa);
    }
    int count = 0;
    for (int t = 0; t < nt; t++) count += mask[t];
    if (count == 0) {
        free(mask);
        *code = NV_ERR_NOT_FOUND;
        snprintf(err, errlen, "the query matched no surface of body '%s'", b->name);
        return false;
    }
    int *tris = malloc((size_t)count * sizeof(int));
    unsigned char *pmark = calloc((size_t)(np ? np : 1), 1);
    if (!tris || !pmark) {
        free(tris), free(pmark), free(mask);
        *code = NV_ERR_RESOURCE_LIMIT;
        snprintf(err, errlen, "out of memory");
        return false;
    }
    double area = 0, cen[3] = {0, 0, 0}, nrm[3] = {0, 0, 0}, lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    int w = 0, npatches = 0;
    for (int t = 0; t < nt; t++) {
        if (!mask[t]) continue;
        tris[w++] = t;
        double a = b->build_area[t];
        area += a;
        for (int k = 0; k < 3; k++) {
            cen[k] += a * b->build_centroid[3 * (size_t)t + k];
            nrm[k] += a * b->build_normal[3 * (size_t)t + k];
            for (int qq = 0; qq < 3; qq++) {
                double x = bv(b, t, qq)[k];
                lo[k] = fmin(lo[k], x), hi[k] = fmax(hi[k], x);
            }
        }
        int p = b->patches.tri_patch[t];
        if (!pmark[p]) pmark[p] = 1, npatches++;
    }
    free(mask);
    int *patches = malloc((size_t)(npatches ? npatches : 1) * sizeof(int));
    if (!patches) {
        free(tris), free(pmark);
        *code = NV_ERR_RESOURCE_LIMIT;
        snprintf(err, errlen, "out of memory");
        return false;
    }
    int wp = 0;
    for (int p = 0; p < np; p++)
        if (pmark[p]) patches[wp++] = p;
    free(pmark);
    qsort(tris, (size_t)count, sizeof(int), cmp_int);
    Sha256 h;
    sha256_init(&h);
    sha256_update(&h, b->sha256, strlen(b->sha256));
    unsigned char mode = (unsigned char)sel->mode;
    sha256_update(&h, &mode, 1);
    for (int i = 0; i < count; i++) {
        unsigned char le[4] = {(unsigned char)tris[i], (unsigned char)(tris[i] >> 8), (unsigned char)(tris[i] >> 16), (unsigned char)(tris[i] >> 24)};
        sha256_update(&h, le, 4);
    }
    unsigned char dig[32];
    sha256_final(&h, dig);
    free(sel->tris);
    free(sel->patches);
    sel->tris = tris;
    sel->ntri = count;
    sel->patches = patches;
    sel->npatches = npatches;
    sel->area = area;
    double ln = sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    for (int k = 0; k < 3; k++) {
        sel->centroid[k] = area > 0 ? cen[k] / area : 0;
        sel->normal[k] = ln > 0 ? nrm[k] / ln : 0;
        sel->bmin[k] = lo[k], sel->bmax[k] = hi[k];
    }
    sha256_hex(dig, sel->set_hash);
    sel->geom_revision = b->geom_revision;
    sel->stale = false;
    sel->stale_reason[0] = 0;
    *code = NV_OK;
    return true;
}

void selection_revalidate(Body *b, Selection *sel, const SelectionContext *ctx) {
    if (sel->geom_revision == b->geom_revision && !sel->stale) return;
    char old_hash[65];
    memcpy(old_hash, sel->set_hash, sizeof old_hash);
    int old_ntri = sel->ntri;
    NvErr code;
    char err[512];
    if (!selection_resolve(b, sel, ctx, &code, err, sizeof err)) {
        sel->stale = true;
        snprintf(sel->stale_reason, sizeof sel->stale_reason, "no longer resolves after the geometry change: %s", err);
        return;
    }
    if (strcmp(old_hash, sel->set_hash) != 0 && old_hash[0]) {
        sel->stale = true;
        snprintf(sel->stale_reason, sizeof sel->stale_reason,
                 "resolves to a different set of faces after the geometry or placement change (%d -> %d triangles); confirm it with selection_create", old_ntri,
                 sel->ntri);
    }
}

bool selection_has_triangle(const Selection *sel, int tri) {
    int lo = 0, hi = sel->ntri - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sel->tris[mid] < tri) lo = mid + 1;
        else if (sel->tris[mid] > tri) hi = mid - 1;
        else return true;
    }
    return false;
}

int selection_mesh_faces(const Project *p, const Selection *sel, int *faces, int max_faces, double *mesh_area) {
    if (mesh_area) *mesh_area = 0;
    const MeshState *ms = &p->mesh;
    if (!ms->valid) return 0;
    int body = -1;
    for (int i = 0; i < ms->nbodies; i++)
        if (!strcmp(ms->body_name[i], sel->body)) body = i;
    if (body < 0) return 0;
    const HexMesh *hm = &ms->hm;
    int n = 0;
    static const int AX[6] = {2, 2, 1, 0, 1, 0};
    for (int f = 0; f < hm->nfaces; f++) {
        int e = hm->face_elem[f];
        if (hm->body[e] != body || hm->face_tri[f] < 0 || !selection_has_triangle(sel, hm->face_tri[f])) continue;
        if (faces && n < max_faces) faces[n] = f;
        n++;
        if (mesh_area && hm->elem_type) {
            /* a tetrahedral face: the area of its corner triangle */
            static const int TF[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
            const double *a = hm->xyz + 3 * (size_t)hm->conn[(size_t)hm->npe * e + TF[hm->face_local[f]][0]];
            const double *b = hm->xyz + 3 * (size_t)hm->conn[(size_t)hm->npe * e + TF[hm->face_local[f]][1]];
            const double *c = hm->xyz + 3 * (size_t)hm->conn[(size_t)hm->npe * e + TF[hm->face_local[f]][2]];
            double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
            *mesh_area += 0.5 * sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        } else if (mesh_area) {
            int ax = AX[hm->face_local[f]];
            *mesh_area += ax == 0 ? hm->h[1] * hm->h[2] : (ax == 1 ? hm->h[0] * hm->h[2] : hm->h[0] * hm->h[1]);
        }
    }
    return n;
}

JsonValue *selection_json(const Project *p, const Selection *sel, bool detailed) {
    JsonValue *o = json_object();
    json_set_string(o, "name", sel->name);
    json_set_string(o, "body", sel->body);
    json_set_string(o, "mode", sel->mode == SEL_MODE_PATCH ? "patch" : "triangle");
    json_set_int(o, "triangles", sel->ntri);
    JsonValue *pl = json_set_array(o, "patches");
    for (int i = 0; i < sel->npatches && i < 200; i++) json_push(pl, json_number(sel->patches[i]));
    json_set_number(o, "area_mm2", sel->area * 1e6);
    json_set(o, "centroid_mm", json_vec3(1e3 * sel->centroid[0], 1e3 * sel->centroid[1], 1e3 * sel->centroid[2]));
    json_set(o, "mean_normal", json_vec3(sel->normal[0], sel->normal[1], sel->normal[2]));
    JsonValue *bd = json_set_object(o, "bounds_mm");
    json_set(bd, "min", json_vec3(1e3 * sel->bmin[0], 1e3 * sel->bmin[1], 1e3 * sel->bmin[2]));
    json_set(bd, "max", json_vec3(1e3 * sel->bmax[0], 1e3 * sel->bmax[1], 1e3 * sel->bmax[2]));
    json_set_string(o, "frame", "build (mm)");
    json_set_int(o, "geometry_revision", (long long)sel->geom_revision);
    json_set_bool(o, "stale", sel->stale);
    if (sel->stale) json_set_string(o, "stale_reason", sel->stale_reason);
    if (sel->description[0]) json_set_string(o, "description", sel->description);
    json_set_string(o, "source", provenance_name(sel->source));
    if (detailed) {
        json_set(o, "query", json_clone(sel->query));
        if (sel->mode == SEL_MODE_PATCH) json_set_number(o, "coverage", sel->coverage);
        if (sel->keep_largest) json_set_int(o, "keep_largest", sel->keep_largest);
        json_set_string(o, "set_hash", sel->set_hash);
    }
    if (p->mesh.valid) {
        double marea;
        int nf = selection_mesh_faces(p, sel, NULL, 0, &marea);
        JsonValue *mp = json_set_object(o, "mesh_projection");
        json_set_int(mp, "faces", nf);
        json_set_number(mp, "mesh_area_mm2", marea * 1e6);
        if (sel->area > 0) json_set_number(mp, "mesh_to_stl_area_ratio", marea / sel->area);
        if (nf == 0) json_set_string(mp, "warning", "no mesh face maps to this selection: refine the mesh or check the selection");
    }
    return o;
}
