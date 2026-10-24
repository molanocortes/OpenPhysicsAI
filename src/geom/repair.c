#include "repair.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mesh_repair_defaults(MeshRepairOptions *o) {
    memset(o, 0, sizeof *o);
    o->keep_largest_component = true;
    o->max_hole_edges = 64;
    surface_repair_defaults(&o->surface);
}

/* ---- directed edges of the kept triangles -------------------------------------------------------------------- */

typedef struct {
    int a, b;   /* the directed edge a -> b; a < 0 marks an empty slot */
    int tri;    /* the triangle that uses it */
    int next;   /* chain within the bucket */
} Edge;

typedef struct {
    Edge *e;
    int *head;
    size_t nbuckets;
    int n, cap;
} EdgeMap;

static size_t edge_hash(int a, int b, size_t nbuckets) {
    uint64_t h = (uint64_t)(uint32_t)a * 0x9E3779B185EBCA87ull ^ ((uint64_t)(uint32_t)b * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 29;
    return (size_t)(h & (nbuckets - 1));
}

static bool edgemap_init(EdgeMap *m, int nedges) {
    size_t n = 16;
    while (n < (size_t)nedges * 2) n <<= 1;
    m->nbuckets = n;
    m->head = malloc(n * sizeof *m->head);
    m->cap = nedges > 0 ? nedges : 1;
    m->e = malloc((size_t)m->cap * sizeof *m->e);
    m->n = 0;
    if (!m->head || !m->e) return false;
    for (size_t i = 0; i < n; i++) m->head[i] = -1;
    return true;
}

static void edgemap_free(EdgeMap *m) {
    free(m->e);
    free(m->head);
    memset(m, 0, sizeof *m);
}

static void edgemap_add(EdgeMap *m, int a, int b, int tri) {
    if (m->n == m->cap) return;
    size_t h = edge_hash(a, b, m->nbuckets);
    Edge *e = &m->e[m->n];
    e->a = a, e->b = b, e->tri = tri, e->next = m->head[h];
    m->head[h] = m->n++;
}

static int edgemap_find(const EdgeMap *m, int a, int b) {
    for (int i = m->head[edge_hash(a, b, m->nbuckets)]; i >= 0; i = m->e[i].next)
        if (m->e[i].a == a && m->e[i].b == b) return i;
    return -1;
}

/* ---- the repair ---------------------------------------------------------------------------------------------- */

static vec3 vpos(const Surface *s, int v) {
    const double *p = &s->v[3 * (size_t)v];
    return v3((float)p[0], (float)p[1], (float)p[2]);
}

bool mesh_repair(const Mesh *in, const MeshRepairOptions *o, Mesh *out, MeshRepairReport *rep, char *err,
                 size_t errlen) {
    MeshRepairOptions defaults;
    if (!o) {
        mesh_repair_defaults(&defaults);
        o = &defaults;
    }
    memset(rep, 0, sizeof *rep);
    mesh_init(out);

    Surface s;
    SurfaceComponent *comps = NULL;
    SurfaceDiagnostics diag;
    if (!surface_build(in, &o->surface, &s, &comps, &diag, err, errlen)) return false;

    rep->triangles_in = diag.source_triangles;
    rep->merged_vertices = diag.merged_vertices;
    rep->degenerate_removed = diag.degenerate_removed;
    rep->duplicate_removed = diag.duplicate_removed;
    rep->open_edges_before = diag.open_edges;
    rep->nonmanifold_edges_before = diag.nonmanifold_edges;
    rep->volume_before = diag.volume;
    rep->area_before = diag.area;
    rep->closed_solid_before = diag.closed_solid;

    /* Which triangles survive.
     *
     * Surface::comp joins triangles across every shared edge, including the non-manifold ones, so a sheet stitched to
     * the shell along a seam of four-triangle edges counts as the same component. The shell of a solid is what you get
     * by walking only MANIFOLD edges: across such an edge there is exactly one other triangle, and the walk cannot
     * step from the outside of the part onto a sheet hanging inside it. That is the split used here. */
    int *shell = malloc((size_t)s.nt * sizeof *shell);
    int *stack = malloc((size_t)s.nt * sizeof *stack);
    int *keep = malloc((size_t)s.nt * sizeof *keep);
    if (!shell || !stack || !keep) {
        free(shell), free(stack), free(keep);
        surface_free(&s);
        free(comps);
        snprintf(err, errlen, "out of memory while choosing the shell to keep");
        return false;
    }
    for (int t = 0; t < s.nt; t++) shell[t] = -1;
    int nshell = 0;
    for (int t0 = 0; t0 < s.nt; t0++) {
        if (shell[t0] >= 0) continue;
        int top = 0;
        stack[top++] = t0;
        shell[t0] = nshell;
        while (top) {
            int t = stack[--top];
            for (int k = 0; k < 3; k++) {
                int nb = s.nbr[3 * (size_t)t + k];
                if (nb < 0 || shell[nb] >= 0) continue; /* -1 open, -2 non-manifold: not a step of this walk */
                shell[nb] = nshell;
                stack[top++] = nb;
            }
        }
        nshell++;
    }
    double *shell_area = calloc((size_t)(nshell > 0 ? nshell : 1), sizeof *shell_area);
    if (!shell_area) {
        free(shell), free(stack), free(keep);
        surface_free(&s);
        free(comps);
        snprintf(err, errlen, "out of memory while measuring the shells");
        return false;
    }
    for (int t = 0; t < s.nt; t++) shell_area[shell[t]] += s.area[t];
    int keep_shell = -1;
    if (o->keep_largest_component && nshell > 1) {
        double best = -1;
        for (int c = 0; c < nshell; c++)
            if (shell_area[c] > best) best = shell_area[c], keep_shell = c;
    }
    rep->components_found = nshell;
    int nkeep = 0;
    for (int t = 0; t < s.nt; t++) {
        bool k = keep_shell < 0 || shell[t] == keep_shell;
        keep[t] = k;
        if (k) nkeep++;
    }
    rep->triangles_dropped = (uint32_t)(s.nt - nkeep);
    if (keep_shell >= 0) {
        for (int c = 0; c < nshell; c++) {
            if (c == keep_shell) rep->kept_area = shell_area[c];
            else rep->dropped_area += shell_area[c], rep->components_dropped++;
        }
    } else {
        rep->kept_area = diag.area;
    }
    free(shell), free(stack), free(shell_area);

    /* the holes left in what is kept: boundary loops of the kept triangles */
    EdgeMap em;
    if (!edgemap_init(&em, nkeep * 3)) {
        free(keep);
        surface_free(&s);
        free(comps);
        snprintf(err, errlen, "out of memory while walking the boundary");
        return false;
    }
    for (int t = 0; t < s.nt; t++) {
        if (!keep[t]) continue;
        const int *tv = &s.tri[3 * (size_t)t];
        for (int k = 0; k < 3; k++) edgemap_add(&em, tv[k], tv[(k + 1) % 3], t);
    }
    /* a boundary edge is one whose reverse is not used by any kept triangle */
    char *is_boundary = calloc((size_t)em.n, 1); /* one byte per edge, and the pointer says so */
    if (!is_boundary) {
        edgemap_free(&em);
        free(keep);
        surface_free(&s);
        free(comps);
        snprintf(err, errlen, "out of memory while walking the boundary");
        return false;
    }
    int nboundary = 0;
    for (int i = 0; i < em.n; i++)
        if (edgemap_find(&em, em.e[i].b, em.e[i].a) < 0) is_boundary[i] = 1, nboundary++;
    /* boundary edges indexed by the vertex they leave, so walking a loop is a lookup and not a scan */
    int *bstart_head = NULL, *bstart_next = NULL;
    size_t bbuckets = 16;
    while (bbuckets < (size_t)(nboundary ? nboundary : 1) * 2) bbuckets <<= 1;
    if (nboundary) {
        bstart_head = malloc(bbuckets * sizeof *bstart_head);
        bstart_next = malloc((size_t)em.n * sizeof *bstart_next);
        if (!bstart_head || !bstart_next) {
            free(bstart_head), free(bstart_next), free(is_boundary);
            edgemap_free(&em);
            free(keep);
            surface_free(&s);
            free(comps);
            snprintf(err, errlen, "out of memory while walking the boundary");
            return false;
        }
        for (size_t i = 0; i < bbuckets; i++) bstart_head[i] = -1;
        for (int i = 0; i < em.n; i++) {
            bstart_next[i] = -1;
            if (!is_boundary[i]) continue;
            size_t h = edge_hash(em.e[i].a, 0, bbuckets);
            bstart_next[i] = bstart_head[h];
            bstart_head[h] = i;
        }
    }

    /* the triangles to add: each boundary loop, fanned from its first vertex, wound the other way round so the
     * patch closes the surface instead of doubling it */
    int *loop = nboundary ? malloc((size_t)nboundary * sizeof *loop) : NULL;
    int *added_a = NULL, *added_b = NULL, *added_c = NULL;
    int nadded = 0, added_cap = 0;
    char *used = nboundary ? calloc((size_t)em.n, 1) : NULL;
    if (nboundary && (!loop || !used)) {
        free(loop), free(used), free(is_boundary), free(bstart_head), free(bstart_next);
        edgemap_free(&em);
        free(keep);
        surface_free(&s);
        free(comps);
        snprintf(err, errlen, "out of memory while filling holes");
        return false;
    }
    for (int i = 0; i < em.n; i++) {
        if (!is_boundary[i] || used[i]) continue;
        int nl = 0;
        int cur = i;
        bool closed = false;
        while (cur >= 0 && nl < nboundary) {
            used[cur] = 1;
            loop[nl++] = em.e[cur].a;
            int next = -1;
            for (int j = bstart_head[edge_hash(em.e[cur].b, 0, bbuckets)]; j >= 0; j = bstart_next[j])
                if (!used[j] && em.e[j].a == em.e[cur].b) { next = j; break; } /* leaves where this one arrives */
            if (next < 0) {
                closed = em.e[cur].b == em.e[i].a;
                break;
            }
            cur = next;
        }
        if (!closed) continue; /* an open chain is not a hole this can fill */
        if (nl > o->max_hole_edges) {
            rep->holes_too_large++;
            if (nl > rep->largest_hole_edges_left) rep->largest_hole_edges_left = nl;
            continue;
        }
        for (int k = 1; k + 1 < nl; k++) {
            if (nadded == added_cap) {
                int cap = added_cap ? added_cap * 2 : 64;
                int *na = realloc(added_a, (size_t)cap * sizeof *na);
                int *nb = realloc(added_b, (size_t)cap * sizeof *nb);
                int *nc = realloc(added_c, (size_t)cap * sizeof *nc);
                if (!na || !nb || !nc) {
                    free(na ? na : added_a), free(nb ? nb : added_b), free(nc ? nc : added_c);
                    free(loop), free(used), free(is_boundary), free(bstart_head), free(bstart_next);
                    edgemap_free(&em);
                    free(keep);
                    surface_free(&s);
                    free(comps);
                    snprintf(err, errlen, "out of memory while filling holes");
                    return false;
                }
                added_a = na, added_b = nb, added_c = nc, added_cap = cap;
            }
            added_a[nadded] = loop[0], added_b[nadded] = loop[k + 1], added_c[nadded] = loop[k];
            nadded++;
        }
        rep->holes_filled++;
    }
    rep->triangles_added = (uint32_t)nadded;

    /* emit the repaired soup */
    if (!mesh_reserve(out, (uint32_t)(nkeep + nadded))) {
        free(added_a), free(added_b), free(added_c), free(loop), free(used), free(is_boundary), free(bstart_head), free(bstart_next);
        edgemap_free(&em);
        free(keep);
        surface_free(&s);
        free(comps);
        snprintf(err, errlen, "out of memory for the repaired mesh");
        return false;
    }
    snprintf(out->name, sizeof out->name, "%s", in->name);
    for (int t = 0; t < s.nt; t++) {
        if (!keep[t]) continue;
        const int *tv = &s.tri[3 * (size_t)t];
        mesh_add_tri(out, vpos(&s, tv[0]), vpos(&s, tv[1]), vpos(&s, tv[2]));
    }
    for (int i = 0; i < nadded; i++)
        mesh_add_tri(out, vpos(&s, added_a[i]), vpos(&s, added_b[i]), vpos(&s, added_c[i]));
    rep->triangles_out = out->tri_count;

    free(added_a), free(added_b), free(added_c), free(loop), free(used), free(is_boundary), free(bstart_head), free(bstart_next);
    edgemap_free(&em);
    free(keep);
    surface_free(&s);
    free(comps);

    /* what the repaired soup looks like, measured the same way as the original */
    Surface s2;
    SurfaceComponent *c2 = NULL;
    SurfaceDiagnostics d2;
    char e2[256];
    if (surface_build(out, &o->surface, &s2, &c2, &d2, e2, sizeof e2)) {
        rep->open_edges_after = d2.open_edges;
        rep->nonmanifold_edges_after = d2.nonmanifold_edges;
        rep->volume_after = d2.volume;
        rep->area_after = d2.area;
        rep->closed_solid_after = d2.closed_solid;
        surface_free(&s2);
        free(c2);
    }
    return true;
}
