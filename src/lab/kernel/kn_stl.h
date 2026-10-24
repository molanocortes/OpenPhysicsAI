/* kn_stl.h - a part read from an STL file, meshed in tetrahedra for the kernel (G24) by the repository's own mesher
 * (src/geom/tetmesh: isosurface stuffing, sharp edges and corners kept, mid-edge nodes on the surface), the same path
 * the engineering core takes (src/ctl/ops_mesh.c). Plain C types only, so that the kernel's C++ needs none of geom's
 * headers. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KnTetMesh {
    int order;              /* 1: corners only; 2: a node in the middle of every edge, on the surface where the edge is */
    int nnodes, nelems, nfaces;
    double *xyz;            /* 3 * nnodes (m) */
    int *corner;            /* 4 * nelems: the corner nodes */
    int *edge;              /* 18 * nelems when order 2: per edge, its two corner nodes and its middle node */
    int *face;              /* 3 * nfaces: the corner nodes of each boundary face, wound outward */
    /* what the mesh is: quality and how far it is from the file */
    int stl_triangles, patches, features_used, feature_nodes, corner_nodes;
    double min_dihedral_deg, max_dihedral_deg, volume_m3, stl_volume_m3, surface_distance_max_m, seconds;
} KnTetMesh;

/* path: the STL; scale: the file's unit in metres; origin_m: added after scaling (NULL: none); surface_size_m: the
 * tetrahedra's size at the surface; interior_size_m: inside (0: four times it); max_elements: 0 for no limit.
 * False with a message when the file is not a closed solid or the mesher refuses it. */
bool kn_stl_mesh(const char *path, double scale, const double *origin_m, double surface_size_m, double interior_size_m,
                 int order, long long max_elements, KnTetMesh *out, char *err, size_t errlen);
void kn_tet_free(KnTetMesh *m);

#ifdef __cplusplus
}
#endif
