/* kn_stl.c - see kn_stl.h. The STL is welded and oriented (surface.c), its triangles grouped into smooth patches
 * (patches.c) so that the edges between them stay sharp, and meshed by isosurface stuffing (tetmesh.c), with the
 * engineering core's settings: patches planar within 2 degrees, an edge sharp past 30 degrees, an inside test that must
 * be nearly unanimous. */
#include "kn_stl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../geom/hexmesh.h"
#include "../../geom/mesh.h"
#include "../../geom/patches.h"
#include "../../geom/surface.h"
#include "../../geom/tetmesh.h"
#include "../../threads.h"

void kn_tet_free(KnTetMesh *m) {
    if (!m) return;
    free(m->xyz), free(m->corner), free(m->edge), free(m->face);
    memset(m, 0, sizeof *m);
}

bool kn_stl_mesh(const char *path, double scale, const double *origin_m, double surface_size_m, double interior_size_m,
                 int order, long long max_elements, KnTetMesh *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (!(scale > 0) || !(surface_size_m > 0) || (order != 1 && order != 2)) {
        snprintf(err, errlen, "a positive scale and element size, and order 1 or 2");
        return false;
    }
    Mesh sm;
    mesh_init(&sm);
    if (!mesh_load_stl(path, &sm, err, errlen)) return false;
    SurfaceRepairOptions ro;
    surface_repair_defaults(&ro);
    Surface s;
    SurfaceComponent *comps = NULL;
    SurfaceDiagnostics diag;
    bool ok = surface_build(&sm, &ro, &s, &comps, &diag, err, errlen);
    mesh_free(&sm);
    if (!ok) return false;
    if (!diag.closed_solid) {
        snprintf(err, errlen, "%s is not a closed solid (%u open edges, %u edges shared by more than two triangles): it has no inside to mesh",
                 path, diag.open_edges, diag.nonmanifold_edges);
        surface_free(&s), free(comps);
        return false;
    }
    PatchSet ps;
    if (!patches_build(&s, 2.0, 30.0, &ps, err, errlen)) {
        surface_free(&s), free(comps);
        return false;
    }
    double *v = malloc(3 * (size_t)s.nv * sizeof(double));
    if (!v) {
        snprintf(err, errlen, "out of memory");
        patches_free(&ps), surface_free(&s), free(comps);
        return false;
    }
    for (int i = 0; i < 3 * s.nv; i++) v[i] = s.v[i] * scale + (origin_m ? origin_m[i % 3] : 0);
    TetStlGeometry g;
    if (!tet_stl_init(&g, v, s.tri, s.nt, err, errlen)) {
        free(v), patches_free(&ps), surface_free(&s), free(comps);
        return false;
    }
    char ferr[256];
    TetFeatures feat;
    memset(&feat, 0, sizeof feat);
    bool have_feat = tet_stl_features(&g, ps.tri_patch, 30.0, &feat, ferr, sizeof ferr);
    TetMeshSettings st;
    memset(&st, 0, sizeof st);
    st.surface_size = surface_size_m, st.interior_size = interior_size_m > 0 ? interior_size_m : 4 * surface_size_m, st.order = order;
    memcpy(st.lo, g.lo, sizeof st.lo), memcpy(st.hi, g.hi, sizeof st.hi);
    st.sdf = tet_stl_sdf, st.project = tet_stl_project, st.cross = tet_stl_cross, st.ctx = &g, st.ref_volume = fabs(g.volume);
    st.features = have_feat ? &feat : NULL;
    st.max_elements = max_elements > 0 ? (uint64_t)max_elements : 0;
    int threads = cpu_perf_count();
    ThreadPool *pool = pool_create(threads > 0 ? threads : 1);
    st.pool = pool;
    TetMesh tm;
    ok = tetmesh_generate(&st, &tm, err, errlen);
    if (pool) pool_destroy(pool);
    if (ok && g.evaluations > 0 && (double)g.votes_split > HEXMESH_UNCERTAIN_LIMIT * (double)g.evaluations) {
        snprintf(err, errlen, "the inside test was not unanimous on %lld of %lld points: the surface has gaps too large to call this a mesh of the part",
                 g.votes_split, g.evaluations);
        tetmesh_free(&tm);
        ok = false;
    }
    if (ok) {
        int ne = tm.nelems, npe = tm.npe;
        out->order = order, out->nnodes = tm.nnodes, out->nelems = ne, out->nfaces = tm.nfaces;
        out->xyz = tm.xyz, tm.xyz = NULL;
        out->corner = malloc(4 * (size_t)(ne ? ne : 1) * sizeof(int));
        out->face = malloc(3 * (size_t)(tm.nfaces ? tm.nfaces : 1) * sizeof(int));
        out->edge = order == 2 ? malloc(18 * (size_t)(ne ? ne : 1) * sizeof(int)) : NULL;
        if (!out->corner || !out->face || (order == 2 && !out->edge)) {
            snprintf(err, errlen, "out of memory");
            ok = false;
        }
        for (int e = 0; ok && e < ne; e++) {
            const int *c = tm.conn + (size_t)npe * e;
            for (int a = 0; a < 4; a++) out->corner[4 * (size_t)e + a] = c[a];
            for (int k = 0; order == 2 && k < 6; k++) {
                int *q = out->edge + 18 * (size_t)e + 3 * k;
                q[0] = c[TETMESH_EDGE[k][0]], q[1] = c[TETMESH_EDGE[k][1]], q[2] = c[4 + k];
            }
        }
        for (int f = 0; ok && f < tm.nfaces; f++) {
            const int *c = tm.conn + (size_t)npe * tm.face_elem[f];
            for (int a = 0; a < 3; a++) out->face[3 * (size_t)f + a] = c[TETMESH_FACE[tm.face_local[f]][a]];
        }
        out->stl_triangles = s.nt, out->patches = ps.n, out->features_used = tm.features_used;
        out->feature_nodes = tm.feature_nodes, out->corner_nodes = tm.corner_nodes;
        out->min_dihedral_deg = tm.min_dihedral, out->max_dihedral_deg = tm.max_dihedral;
        out->volume_m3 = tm.volume, out->stl_volume_m3 = fabs(g.volume), out->surface_distance_max_m = tm.surf_face_dist_max;
        out->seconds = tm.seconds;
        tetmesh_free(&tm);
        if (!ok) kn_tet_free(out);
    }
    tet_stl_free(&g);
    free(v), patches_free(&ps), surface_free(&s), free(comps);
    return ok;
}
