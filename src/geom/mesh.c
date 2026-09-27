/* mesh.c - triangle soups: growth, welding, normals, statistics, STL import/export */
#include "mesh.h"
#include "../common.h"

#include <errno.h>
#include <stdarg.h>

#define EMPTY32 0xFFFFFFFFu

void mesh_init(Mesh *m) { memset(m, 0, sizeof *m); }

void mesh_free(Mesh *m) {
    free(m->pos);
    free(m->nrm);
    memset(m, 0, sizeof *m);
}

bool mesh_reserve(Mesh *m, uint32_t cap) {
    if (cap <= m->tri_cap) return true;
    size_t bytes = (size_t)cap * 3 * sizeof(vec3);
    vec3 *p = realloc(m->pos, bytes);
    if (!p) return false;
    m->pos = p;
    if (m->nrm) {
        vec3 *n = realloc(m->nrm, bytes);
        if (!n) return false; /* pos is larger than tri_cap says; harmless */
        m->nrm = n;
    }
    m->tri_cap = cap;
    return true;
}

/* Double-precision (unnormalised) face normal = 2 * area vector. */
static void face_cross(vec3 a, vec3 b, vec3 c, double *n) {
    double ux = (double)b.x - a.x, uy = (double)b.y - a.y, uz = (double)b.z - a.z;
    double vx = (double)c.x - a.x, vy = (double)c.y - a.y, vz = (double)c.z - a.z;
    n[0] = uy * vz - uz * vy;
    n[1] = uz * vx - ux * vz;
    n[2] = ux * vy - uy * vx;
}

static vec3 face_normal(vec3 a, vec3 b, vec3 c) {
    double n[3];
    face_cross(a, b, c, n);
    double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(l > 0)) return v3(0, 0, 0);
    return v3((float)(n[0] / l), (float)(n[1] / l), (float)(n[2] / l));
}

static bool grow_for(Mesh *m, uint32_t extra) {
    uint64_t need = (uint64_t)m->tri_count + extra;
    if (need > 0xFFFFFFFFu) return false;
    if (need <= m->tri_cap) return true;
    uint64_t cap = m->tri_cap ? (uint64_t)m->tri_cap * 2 : 256;
    while (cap < need) cap *= 2;
    if (cap > 0xFFFFFFFFu) cap = 0xFFFFFFFFu;
    return mesh_reserve(m, (uint32_t)cap);
}

void mesh_add_tri(Mesh *m, vec3 a, vec3 b, vec3 c) {
    if (!grow_for(m, 1)) {
        LOGE("mesh: out of memory at %u triangles", m->tri_count);
        return;
    }
    size_t i = 3 * (size_t)m->tri_count;
    m->pos[i] = a, m->pos[i + 1] = b, m->pos[i + 2] = c;
    if (m->nrm) m->nrm[i] = m->nrm[i + 1] = m->nrm[i + 2] = face_normal(a, b, c);
    m->tri_count++;
}

void mesh_add_quad(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 d) {
    mesh_add_tri(m, a, b, c);
    mesh_add_tri(m, a, c, d);
}

/* Allocate nrm (to capacity) filled with face normals for the existing triangles. */
static bool ensure_nrm(Mesh *m) {
    if (m->nrm) return true;
    m->nrm = malloc((size_t)(m->tri_cap ? m->tri_cap : 1) * 3 * sizeof(vec3));
    if (!m->nrm) return false;
    for (uint32_t t = 0; t < m->tri_count; t++) {
        const vec3 *p = m->pos + 3 * (size_t)t;
        m->nrm[3 * (size_t)t] = m->nrm[3 * (size_t)t + 1] = m->nrm[3 * (size_t)t + 2] = face_normal(p[0], p[1], p[2]);
    }
    return true;
}

void mesh_append(Mesh *dst, const Mesh *src) {
    if (!src->tri_count) return;
    if (!grow_for(dst, src->tri_count) || ((dst->nrm || src->nrm) && !ensure_nrm(dst))) {
        LOGE("mesh: out of memory appending %u triangles", src->tri_count);
        return;
    }
    size_t o = 3 * (size_t)dst->tri_count, n = 3 * (size_t)src->tri_count;
    memcpy(dst->pos + o, src->pos, n * sizeof(vec3));
    if (dst->nrm) {
        if (src->nrm) {
            memcpy(dst->nrm + o, src->nrm, n * sizeof(vec3));
        } else {
            for (size_t i = 0; i < n; i += 3)
                dst->nrm[o + i] = dst->nrm[o + i + 1] = dst->nrm[o + i + 2] =
                    face_normal(src->pos[i], src->pos[i + 1], src->pos[i + 2]);
        }
    }
    dst->tri_count += src->tri_count;
}

void mesh_compute_bounds(Mesh *m) {
    if (!m->tri_count) {
        m->bmin = m->bmax = v3(0, 0, 0);
        return;
    }
    vec3 lo = m->pos[0], hi = m->pos[0];
    size_t n = 3 * (size_t)m->tri_count;
    for (size_t i = 1; i < n; i++) lo = v3_min(lo, m->pos[i]), hi = v3_max(hi, m->pos[i]);
    m->bmin = lo, m->bmax = hi;
}

/* ---------------------------------------------------------------------------------------------- */
/* Vertex welding: spatial hash with cell size 4*tol; a point only probes a neighbouring cell on an
 * axis when it lies within tol of that cell boundary, so every point within tol is found. */

static uint32_t hash_cell(int32_t x, int32_t y, int32_t z) {
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h;
}

static int32_t cell_coord(float p, float lo, double inv, int *d0, int *d1) {
    double f = ((double)p - lo) * inv;
    if (!(f >= 0 && f < 1e9)) f = 0; /* non-finite guard */
    int32_t c = (int32_t)f;
    double fr = f - c;
    *d0 = fr < 0.25 ? -1 : 0;
    *d1 = fr > 0.75 ? 1 : 0;
    return c;
}

/* Fills ids[i] (i < n) with a unique vertex index; returns the unique count or EMPTY32 on OOM. */
static uint32_t weld_positions(const vec3 *pos, size_t n, uint32_t *ids) {
    if (!n) return 0;
    vec3 lo = pos[0], hi = pos[0];
    for (size_t i = 1; i < n; i++) lo = v3_min(lo, pos[i]), hi = v3_max(hi, pos[i]);
    double dx = (double)hi.x - lo.x, dy = (double)hi.y - lo.y, dz = (double)hi.z - lo.z;
    double tol = 1e-6 * sqrt(dx * dx + dy * dy + dz * dz);
    if (!(tol > 1e-30)) tol = 1e-30;
    double inv = 1.0 / (4.0 * tol), tol2 = tol * tol;

    size_t tsize = 64;
    while (tsize < 2 * n) tsize <<= 1;
    size_t mask = tsize - 1;
    uint32_t *table = malloc(tsize * sizeof(uint32_t));
    uint32_t *first = malloc(n * sizeof(uint32_t)); /* unique vertex -> first corner index */
    if (!table || !first) {
        free(table), free(first);
        return EMPTY32;
    }
    memset(table, 0xFF, tsize * sizeof(uint32_t));
    uint32_t nu = 0;
    for (size_t i = 0; i < n; i++) {
        vec3 p = pos[i];
        int x0, x1, y0, y1, z0, z1;
        int32_t cx = cell_coord(p.x, lo.x, inv, &x0, &x1);
        int32_t cy = cell_coord(p.y, lo.y, inv, &y0, &y1);
        int32_t cz = cell_coord(p.z, lo.z, inv, &z0, &z1);
        uint32_t found = EMPTY32;
        /* probe own cell first (exact duplicates always land there) */
        for (int pass = 0; pass < 2 && found == EMPTY32; pass++) {
            for (int oz = z0; oz <= z1 && found == EMPTY32; oz++)
                for (int oy = y0; oy <= y1 && found == EMPTY32; oy++)
                    for (int ox = x0; ox <= x1 && found == EMPTY32; ox++) {
                        bool own = !ox && !oy && !oz;
                        if (own != (pass == 0)) continue;
                        for (size_t h = hash_cell(cx + ox, cy + oy, cz + oz) & mask; table[h] != EMPTY32;
                             h = (h + 1) & mask) {
                            vec3 q = pos[first[table[h]]];
                            double ex = (double)q.x - p.x, ey = (double)q.y - p.y, ez = (double)q.z - p.z;
                            if (ex * ex + ey * ey + ez * ez <= tol2) {
                                found = table[h];
                                break;
                            }
                        }
                    }
        }
        if (found == EMPTY32) {
            found = nu++;
            first[found] = (uint32_t)i;
            size_t h = hash_cell(cx, cy, cz) & mask;
            while (table[h] != EMPTY32) h = (h + 1) & mask;
            table[h] = found;
        }
        ids[i] = found;
    }
    free(table);
    free(first);
    return nu;
}

/* ---------------------------------------------------------------------------------------------- */

void mesh_compute_normals(Mesh *m, float crease_angle_deg) {
    uint32_t nt = m->tri_count;
    size_t nc = 3 * (size_t)nt;
    if (!m->nrm) {
        m->nrm = malloc((size_t)(m->tri_cap ? m->tri_cap : 1) * 3 * sizeof(vec3));
        if (!m->nrm) {
            LOGE("mesh: out of memory computing normals");
            return;
        }
    }
    if (!nt) return;

    uint32_t *ids = malloc(nc * sizeof(uint32_t));
    vec3 *fn = malloc((size_t)nt * sizeof(vec3));
    float *wgt = malloc(nc * sizeof(float));
    uint32_t nu = ids ? weld_positions(m->pos, nc, ids) : EMPTY32;
    uint32_t *off = nu != EMPTY32 ? calloc((size_t)nu + 1, sizeof(uint32_t)) : NULL;
    uint32_t *lst = malloc(nc * sizeof(uint32_t));
    if (!ids || !fn || !wgt || !off || !lst) {
        LOGE("mesh: out of memory computing normals");
        for (size_t i = 0; i < nc; i += 3)
            m->nrm[i] = m->nrm[i + 1] = m->nrm[i + 2] = face_normal(m->pos[i], m->pos[i + 1], m->pos[i + 2]);
        free(ids), free(fn), free(wgt), free(off), free(lst);
        return;
    }

    /* unit face normals and corner angles (angle-weighted smoothing) */
    for (uint32_t t = 0; t < nt; t++) {
        const vec3 *p = m->pos + 3 * (size_t)t;
        double c[3];
        face_cross(p[0], p[1], p[2], c);
        double a2 = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        size_t i = 3 * (size_t)t;
        if (!(a2 > 0) || !isfinite(a2)) {
            fn[t] = v3(0, 0, 0);
            wgt[i] = wgt[i + 1] = wgt[i + 2] = 0;
            continue;
        }
        fn[t] = v3((float)(c[0] / a2), (float)(c[1] / a2), (float)(c[2] / a2));
        for (int k = 0; k < 3; k++) {
            vec3 o = p[k], u = p[(k + 1) % 3], v = p[(k + 2) % 3];
            double d = ((double)u.x - o.x) * ((double)v.x - o.x) + ((double)u.y - o.y) * ((double)v.y - o.y) +
                       ((double)u.z - o.z) * ((double)v.z - o.z);
            wgt[i + k] = (float)atan2(a2, d);
        }
    }

    /* vertex -> incident corners (CSR) */
    for (size_t i = 0; i < nc; i++) off[ids[i] + 1]++;
    uint32_t maxdeg = 0;
    for (uint32_t v = 0; v < nu; v++) {
        if (off[v + 1] > maxdeg) maxdeg = off[v + 1];
        off[v + 1] += off[v];
    }
    for (size_t i = 0; i < nc; i++) lst[off[ids[i]]++] = (uint32_t)i;
    for (uint32_t v = nu; v > 0; v--) off[v] = off[v - 1];
    off[0] = 0;

    enum { CLUSTER_DEG = 128 };
    int32_t *cl = NULL;
    vec3 *csum = NULL, *rep = NULL;
    if (maxdeg > CLUSTER_DEG) {
        cl = malloc((size_t)maxdeg * sizeof(int32_t));
        csum = malloc((size_t)maxdeg * sizeof(vec3));
        rep = malloc((size_t)maxdeg * sizeof(vec3));
        if (!cl || !csum || !rep) free(cl), free(csum), free(rep), cl = NULL, csum = rep = NULL;
    }

    float cosc = cosf(DEG2RAD(CLAMP(crease_angle_deg, 0.0f, 180.0f)));
    for (uint32_t v = 0; v < nu; v++) {
        uint32_t b = off[v], e = off[v + 1];
        if (e - b > CLUSTER_DEG && cl) {
            /* huge fans: greedy clustering against representative normals, O(deg * clusters) */
            uint32_t nrep = 0;
            for (uint32_t k = b; k < e; k++) {
                uint32_t c2 = lst[k];
                vec3 f = fn[c2 / 3];
                cl[k - b] = -1;
                if (wgt[c2] == 0) continue;
                uint32_t r = 0;
                while (r < nrep && v3_dot(f, rep[r]) < cosc) r++;
                if (r == nrep) rep[nrep] = f, csum[nrep++] = v3(0, 0, 0);
                cl[k - b] = (int32_t)r;
                csum[r] = v3_add(csum[r], v3_scale(f, wgt[c2]));
            }
            for (uint32_t k = b; k < e; k++) {
                uint32_t ci = lst[k];
                vec3 f = fn[ci / 3], n = cl[k - b] >= 0 ? v3_norm(csum[cl[k - b]]) : f;
                if (n.x == 0 && n.y == 0 && n.z == 0) n = v3(0, 1, 0);
                m->nrm[ci] = n;
            }
            continue;
        }
        for (uint32_t k = b; k < e; k++) {
            uint32_t ci = lst[k], t = ci / 3;
            vec3 f = fn[t], s = v3(0, 0, 0);
            bool degen = wgt[ci] == 0; /* degenerate face: take the plain smooth average */
            for (uint32_t k2 = b; k2 < e; k2++) {
                uint32_t c2 = lst[k2], t2 = c2 / 3;
                if (wgt[c2] == 0) continue;
                if (t2 == t || degen || v3_dot(f, fn[t2]) >= cosc) s = v3_add(s, v3_scale(fn[t2], wgt[c2]));
            }
            vec3 n = v3_norm(s);
            if (n.x == 0 && n.y == 0 && n.z == 0) n = degen ? v3(0, 1, 0) : f;
            m->nrm[ci] = n;
        }
    }
    free(cl), free(csum), free(rep);
    free(ids), free(fn), free(wgt), free(off), free(lst);
}

void mesh_transform(Mesh *m, mat4 M) {
    size_t nc = 3 * (size_t)m->tri_count;
    for (size_t i = 0; i < nc; i++) m->pos[i] = m4_mul_point(M, m->pos[i]);
    const float *a = M.m;
    double det = (double)a[0] * ((double)a[5] * a[10] - (double)a[9] * a[6]) -
                 (double)a[4] * ((double)a[1] * a[10] - (double)a[9] * a[2]) +
                 (double)a[8] * ((double)a[1] * a[6] - (double)a[5] * a[2]);
    if (m->nrm) {
        mat4 inv;
        mat4 N = m4_invert(M, &inv) ? m4_transpose(inv) : M;
        for (size_t i = 0; i < nc; i++) m->nrm[i] = v3_norm(m4_mul_dir(N, m->nrm[i]));
    }
    if (det < 0) { /* mirror: swap winding so faces stay outward CCW */
        for (size_t i = 0; i < nc; i += 3) {
            vec3 t = m->pos[i + 1];
            m->pos[i + 1] = m->pos[i + 2], m->pos[i + 2] = t;
            if (m->nrm) t = m->nrm[i + 1], m->nrm[i + 1] = m->nrm[i + 2], m->nrm[i + 2] = t;
        }
    }
    mesh_compute_bounds(m);
}

static bool tri_is_degenerate(const vec3 *p) {
    for (int k = 0; k < 3; k++)
        if (!is_finite_f32(p[k].x) || !is_finite_f32(p[k].y) || !is_finite_f32(p[k].z)) return true;
    double c[3], emax2 = 0;
    face_cross(p[0], p[1], p[2], c);
    for (int k = 0; k < 3; k++) {
        vec3 u = p[k], v = p[(k + 1) % 3];
        double ex = (double)v.x - u.x, ey = (double)v.y - u.y, ez = (double)v.z - u.z;
        double e2 = ex * ex + ey * ey + ez * ez;
        if (e2 > emax2) emax2 = e2;
    }
    double a2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2]; /* |cross|^2 vs (1e-10 * emax^2)^2 */
    return !(a2 > 1e-20 * emax2 * emax2);
}

uint32_t mesh_remove_degenerate(Mesh *m) {
    uint32_t w = 0, n = m->tri_count;
    for (uint32_t t = 0; t < n; t++) {
        const vec3 *p = m->pos + 3 * (size_t)t;
        if (tri_is_degenerate(p)) continue;
        if (w != t) {
            memcpy(m->pos + 3 * (size_t)w, p, 3 * sizeof(vec3));
            if (m->nrm) memcpy(m->nrm + 3 * (size_t)w, m->nrm + 3 * (size_t)t, 3 * sizeof(vec3));
        }
        w++;
    }
    m->tri_count = w;
    return n - w;
}

static double signed_volume(const Mesh *m) {
    double vol = 0;
    for (uint32_t t = 0; t < m->tri_count; t++) {
        const vec3 *p = m->pos + 3 * (size_t)t;
        double c[3] = {(double)p[1].y * p[2].z - (double)p[1].z * p[2].y,
                       (double)p[1].z * p[2].x - (double)p[1].x * p[2].z,
                       (double)p[1].x * p[2].y - (double)p[1].y * p[2].x};
        vol += p[0].x * c[0] + p[0].y * c[1] + p[0].z * c[2];
    }
    return vol / 6.0;
}

void mesh_fix_orientation(Mesh *m) {
    if (signed_volume(m) >= 0) return;
    size_t nc = 3 * (size_t)m->tri_count;
    for (size_t i = 0; i < nc; i += 3) {
        vec3 t = m->pos[i + 1];
        m->pos[i + 1] = m->pos[i + 2], m->pos[i + 2] = t;
        if (m->nrm) {
            t = m->nrm[i + 1], m->nrm[i + 1] = m->nrm[i + 2], m->nrm[i + 2] = t;
            for (int k = 0; k < 3; k++) m->nrm[i + k] = v3_neg(m->nrm[i + k]);
        }
    }
}

void mesh_compute_stats(const Mesh *m, MeshStats *s) {
    memset(s, 0, sizeof *s);
    uint32_t nt = m->tri_count;
    s->triangles = nt;
    if (!nt) return;
    size_t nc = 3 * (size_t)nt;
    for (uint32_t t = 0; t < nt; t++) {
        const vec3 *p = m->pos + 3 * (size_t)t;
        double c[3];
        face_cross(p[0], p[1], p[2], c);
        s->surface_area += 0.5 * sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        s->degenerate_removed += tri_is_degenerate(p); /* const mesh: count of degenerates present */
    }
    s->volume = signed_volume(m);

    uint32_t *ids = malloc(nc * sizeof(uint32_t));
    uint32_t nu = ids ? weld_positions(m->pos, nc, ids) : EMPTY32;
    size_t tsize = 64;
    while (tsize < nc + nc / 3) tsize <<= 1;
    uint64_t *keys = nu != EMPTY32 ? calloc(tsize, sizeof(uint64_t)) : NULL;
    uint32_t *cnt = keys ? calloc(tsize, sizeof(uint32_t)) : NULL;
    if (!cnt) {
        LOGE("mesh: out of memory computing statistics");
        free(ids), free(keys);
        return;
    }
    s->unique_vertices = nu;
    int bits = 0;
    while (((size_t)1 << bits) < tsize) bits++;
    size_t mask = tsize - 1;
    for (size_t i = 0; i < nc; i += 3) {
        for (int k = 0; k < 3; k++) {
            uint32_t a = ids[i + k], b = ids[i + (k + 1) % 3];
            if (a == b) continue;
            uint64_t key = a < b ? ((uint64_t)a << 32 | b) : ((uint64_t)b << 32 | a); /* never 0 */
            size_t h = (size_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - bits));
            while (keys[h] && keys[h] != key) h = (h + 1) & mask;
            keys[h] = key;
            cnt[h]++;
        }
    }
    for (size_t h = 0; h < tsize; h++) {
        if (cnt[h] == 1) s->open_edges++;
        else if (cnt[h] > 2) s->nonmanifold_edges++;
    }
    s->watertight = s->open_edges == 0 && s->nonmanifold_edges == 0;
    free(ids), free(keys), free(cnt);
}

/* ---------------------------------------------------------------------------------------------- */
/* STL I/O */

static void set_err(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static uint32_t rd_u32(const unsigned char *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static float rd_f32(const unsigned char *b) {
    uint32_t u = rd_u32(b);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void wr_u32(unsigned char *b, uint32_t u) {
    b[0] = (unsigned char)u, b[1] = (unsigned char)(u >> 8), b[2] = (unsigned char)(u >> 16),
    b[3] = (unsigned char)(u >> 24);
}
static void wr_f32(unsigned char *b, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    wr_u32(b, u);
}

static bool finite3(vec3 v) { return is_finite_f32(v.x) && is_finite_f32(v.y) && is_finite_f32(v.z); }

static bool parse_binary(const unsigned char *buf, uint32_t count, Mesh *m, char *err, size_t errlen) {
    if (!mesh_reserve(m, count)) {
        set_err(err, errlen, "out of memory for %u triangles", count);
        return false;
    }
    for (uint32_t t = 0; t < count; t++) {
        const unsigned char *r = buf + 84 + 50 * (size_t)t + 12;
        vec3 v[3];
        for (int k = 0; k < 3; k++) v[k] = v3(rd_f32(r + 12 * k), rd_f32(r + 12 * k + 4), rd_f32(r + 12 * k + 8));
        if (!finite3(v[0]) || !finite3(v[1]) || !finite3(v[2])) {
            set_err(err, errlen, "binary STL: triangle %u has a NaN/Inf coordinate", t);
            return false;
        }
        mesh_add_tri(m, v[0], v[1], v[2]);
    }
    return true;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

/* buf must be NUL-terminated at buf[len]. */
static bool parse_ascii(const char *buf, size_t len, Mesh *m, char *err, size_t errlen) {
    const char *p = buf, *end = buf + len;
    size_t line = 1;
    vec3 tri[3];
    int nv = 0;
    while (p < end) {
        while (p < end && is_ws(*p)) line += *p++ == '\n';
        if (p >= end) break;
        const char *tok = p;
        while (p < end && !is_ws(*p)) p++;
        if (p - tok != 6 || !str_starts_with_i(tok, "vertex")) continue;
        float c[3];
        for (int k = 0; k < 3; k++) {
            while (p < end && is_ws(*p)) line += *p++ == '\n';
            char *e;
            c[k] = strtof(p, &e);
            if (e == p) {
                set_err(err, errlen, "ASCII STL line %zu: expected 3 numbers after 'vertex'", line);
                return false;
            }
            if (!is_finite_f32(c[k])) {
                set_err(err, errlen, "ASCII STL line %zu: NaN/Inf vertex coordinate", line);
                return false;
            }
            p = e;
        }
        tri[nv++] = v3(c[0], c[1], c[2]);
        if (nv == 3) {
            if (m->tri_count == 0xFFFFFFFFu) {
                set_err(err, errlen, "ASCII STL: too many triangles");
                return false;
            }
            mesh_add_tri(m, tri[0], tri[1], tri[2]);
            nv = 0;
        }
    }
    if (nv) {
        set_err(err, errlen, "ASCII STL: vertex count is not a multiple of 3");
        return false;
    }
    if (!m->tri_count) {
        set_err(err, errlen, "no 'vertex' records found (not an ASCII STL)");
        return false;
    }
    return true;
}

static void name_from_path(char *dst, size_t cap, const char *path) {
    const char *b = path;
    for (const char *s = path; *s; s++)
        if (*s == '/' || *s == '\\') b = s + 1;
    str_copy(dst, cap, b);
    char *dot = strrchr(dst, '.');
    if (dot && dot != dst) *dot = 0;
}

bool mesh_load_stl_ex(const char *path, Mesh *out, StlInfo *info, uint64_t max_bytes, uint32_t max_triangles, char *err,
                      size_t errlen) {
    mesh_init(out);
    StlInfo local_info;
    if (!info) info = &local_info;
    memset(info, 0, sizeof *info);
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err, errlen, "cannot open '%s': %s", path, strerror(errno));
        return false;
    }
    long long size = -1;
    if (fseeko(f, 0, SEEK_END) == 0) size = (long long)ftello(f);
    if (size < 0 || fseeko(f, 0, SEEK_SET) != 0) {
        set_err(err, errlen, "cannot determine size of '%s'", path);
        fclose(f);
        return false;
    }
    info->file_bytes = (uint64_t)size;
    if (max_bytes && (uint64_t)size > max_bytes) {
        set_err(err, errlen, "'%s' is %lld bytes, above the limit of %llu bytes", path, size, (unsigned long long)max_bytes);
        fclose(f);
        return false;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        set_err(err, errlen, "out of memory reading '%s' (%lld bytes)", path, size);
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        set_err(err, errlen, "read error on '%s' (%zu of %lld bytes)", path, got, size);
        free(buf);
        return false;
    }
    buf[size] = 0;

    bool ok;
    uint32_t count = size >= 84 ? rd_u32((unsigned char *)buf + 80) : 0;
    uint64_t bin_size = 84 + 50 * (uint64_t)count;
    if (max_triangles && size >= 84 && count > max_triangles && bin_size <= (uint64_t)size && strncmp(buf, "solid", 5) != 0) {
        set_err(err, errlen, "binary STL declares %u triangles, above the limit of %u", count, max_triangles);
        free(buf);
        return false;
    }
    if (size >= 84 && (uint64_t)size == bin_size) {
        info->binary = true;
        ok = parse_binary((unsigned char *)buf, count, out, err, errlen);
    } else {
        char aerr[256] = "";
        ok = parse_ascii(buf, (size_t)size, out, aerr, sizeof aerr);
        if (!ok) {
            mesh_free(out);
            if (size >= 84 && count > 0 && bin_size < (uint64_t)size && strncmp(buf, "solid", 5) != 0) {
                LOGW("STL '%s': %llu trailing bytes after %u binary triangles ignored", path,
                     (unsigned long long)((uint64_t)size - bin_size), count);
                info->binary = true;
                info->trailing_bytes = (uint64_t)size - bin_size;
                ok = parse_binary((unsigned char *)buf, count, out, err, errlen);
            } else if (size >= 84 && bin_size > (uint64_t)size && strncmp(buf, "solid", 5) != 0) {
                set_err(err, errlen, "truncated binary STL: header says %u triangles (%llu bytes), file has %lld",
                        count, (unsigned long long)bin_size, size);
            } else {
                set_err(err, errlen, "not a valid STL: %s", aerr);
            }
        }
    }
    if (info->binary) {
        info->declared_triangles = count;
        for (int i = 0; i < 80; i++) {
            unsigned char c = (unsigned char)buf[i];
            info->header[i] = (c >= 32 && c < 127) ? (char)c : (c ? '.' : ' ');
        }
        int end = 80;
        while (end > 0 && info->header[end - 1] == ' ') end--;
        info->header[end] = 0;
    } else if (size >= 5 && strncmp(buf, "solid", 5) == 0) {
        const char *s = buf + 5;
        while (*s == ' ' || *s == '\t') s++;
        int n = 0;
        while (s[n] && s[n] != '\n' && s[n] != '\r' && n < 80) {
            unsigned char c = (unsigned char)s[n];
            info->solid_name[n] = (c >= 32 && c < 127) ? (char)c : '.';
            n++;
        }
        info->solid_name[n] = 0;
    }
    free(buf);
    if (ok && !out->tri_count) {
        set_err(err, errlen, "STL contains no triangles");
        ok = false;
    }
    if (ok && max_triangles && out->tri_count > max_triangles) {
        set_err(err, errlen, "STL has %u triangles, above the limit of %u", out->tri_count, max_triangles);
        ok = false;
    }
    if (!ok) {
        mesh_free(out);
        return false;
    }
    mesh_compute_bounds(out);
    name_from_path(out->name, sizeof out->name, path);
    return true;
}

bool mesh_load_stl(const char *path, Mesh *out, char *err, size_t errlen) {
    if (!mesh_load_stl_ex(path, out, NULL, 0, 0, err, errlen)) return false;
    uint32_t n0 = out->tri_count, removed = mesh_remove_degenerate(out);
    if (!out->tri_count) {
        set_err(err, errlen, "all %u triangles are degenerate", n0);
        mesh_free(out);
        return false;
    }
    if (removed) LOGW("STL '%s': removed %u degenerate triangles", path, removed);
    mesh_compute_bounds(out);
    mesh_compute_normals(out, 35.0f);
    return true;
}

bool mesh_save_stl(const char *path, const Mesh *m, char *err, size_t errlen) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        set_err(err, errlen, "cannot create '%s': %s", path, strerror(errno));
        return false;
    }
    unsigned char hdr[84] = {0};
    memcpy(hdr, "NAVIER binary STL", 17);
    wr_u32(hdr + 80, m->tri_count);
    bool ok = fwrite(hdr, 1, 84, f) == 84;
    enum { CHUNK = 4096 };
    unsigned char *rec = malloc(50 * CHUNK);
    if (!rec) ok = false;
    for (uint32_t t = 0; ok && t < m->tri_count;) {
        uint32_t n = MINI(m->tri_count - t, (uint32_t)CHUNK);
        for (uint32_t j = 0; j < n; j++) {
            const vec3 *p = m->pos + 3 * (size_t)(t + j);
            unsigned char *r = rec + 50 * (size_t)j;
            vec3 fnrm = face_normal(p[0], p[1], p[2]);
            wr_f32(r, fnrm.x), wr_f32(r + 4, fnrm.y), wr_f32(r + 8, fnrm.z);
            for (int k = 0; k < 3; k++)
                wr_f32(r + 12 + 12 * k, p[k].x), wr_f32(r + 16 + 12 * k, p[k].y), wr_f32(r + 20 + 12 * k, p[k].z);
            r[48] = r[49] = 0;
        }
        ok = fwrite(rec, 50, n, f) == n;
        t += n;
    }
    free(rec);
    if (fclose(f) != 0) ok = false;
    if (!ok) set_err(err, errlen, "write error on '%s'", path);
    return ok;
}
