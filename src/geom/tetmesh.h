/* tetmesh.h - conforming tetrahedral meshes by isosurface stuffing on a graded BCC lattice
 *
 * Contract: docs/contracts/tet-mesh.md. The part is seen through a signed distance f (negative inside). A 2:1 balanced
 * octree is refined to surface_size where the surface passes and to interior_size inside; its leaves carry a
 * body-centred-cubic background mesh (BCC tetrahedra between equal leaves, cones over split faces at transitions).
 * Isosurface stuffing (Labelle and Shewchuk, ACM TOG 26(3), 2007) then cuts it by the zero level set: cut points on the
 * surface, warping of lattice vertices too close to a cut point (alpha 0.24999 on axis-aligned edges, 0.41189 on the
 * others), and stencils per background tetrahedron. Quadrilaterals are split by their shorter diagonal (a face-local
 * rule, so neighbours agree); a truncated tetrahedron whose three quadrilaterals form a cycle gets a Steiner vertex.
 * The paper's angle bound is therefore not inherited: angles are measured, and a mesh below the floor is refused.
 *
 * Sharp edges and corners are rounded at the scale of one surface cell; faces on lattice planes are exact.
 *
 * TET10 node order: corners 0-3, then mid-edge nodes 4 (0,1), 5 (1,2), 6 (2,0), 7 (0,3), 8 (1,3), 9 (2,3).
 * Local face k is the face opposite corner k. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../threads.h"

typedef double (*TetSdfFn)(void *ctx, const double p[3]);                     /* signed distance, < 0 inside (m) */
typedef void (*TetProjectFn)(void *ctx, const double p[3], double q[3]);     /* closest surface point */
/* optional: the parameter t in (0,1) where the segment a->b (a inside, b outside) crosses the surface */
typedef bool (*TetCrossFn)(void *ctx, const double a[3], const double b[3], double *t);

extern const int TETMESH_EDGE[6][2];   /* corner pairs of the six edges, in TET10 mid-node order */
extern const int TETMESH_FACE[4][3];   /* corners of local face k (opposite corner k), outward winding */

typedef struct TetMeshSettings {
    double surface_size;    /* leaf size at the surface (m) */
    double interior_size;   /* largest leaf inside (m); 0 = 4 x surface_size */
    int order;              /* 1 (TET4) or 2 (TET10) */
    double lo[3], hi[3];    /* bounding box of the part (m) */
    TetSdfFn sdf;
    TetProjectFn project;   /* required for order 2 (curved mid-edge nodes); may be NULL for order 1 */
    TetCrossFn cross;       /* optional: exact segment crossings (an STL); otherwise f is bracketed */
    void *ctx;
    double ref_volume;      /* the part's volume for the volume error (m^3), 0 = unknown */
    double min_angle_floor; /* degrees; 0 = the contract's 5 */
    int thin_levels;        /* extra levels below surface_size where a wall is thinner than 1.5 cells; 0 = 2, -1 = none */
    uint64_t max_elements;  /* 0 = no limit */
    ThreadPool *pool;
} TetMeshSettings;

typedef struct TetMesh {
    int order, npe;          /* 1 / 4 or 2 / 10 */
    int nnodes, nelems;
    double *xyz;             /* 3*nnodes (m) */
    int *conn;               /* npe*nelems */
    /* boundary faces: (element, local face) of every face without a neighbour */
    int nfaces;
    int *face_elem;
    unsigned char *face_local;
    /* quality (section 4 of the contract) */
    double min_dihedral, max_dihedral; /* degrees, over the corner tetrahedra */
    double min_dihedral_at[3];         /* centroid of the worst element (m) */
    double worst_aspect;               /* circumradius / (3 inradius), 1 = regular */
    double surf_node_dist_max, surf_node_dist_mean;   /* |f| at boundary corner nodes (m) */
    double surf_face_dist_max, surf_face_dist_mean;   /* |f| at boundary face centroids (m): the chordal error */
    double volume, ref_volume;                         /* m^3 */
    int regions;                                       /* face-connected pieces */
    int curved_edges, straightened_edges;              /* TET10 boundary edges */
    int nonmanifold_faces;                             /* faces shared by more than two elements (0 when conforming) */
    /* how the mesh was made */
    int octree_leaves, octree_depth, lattice_vertices, background_tets;
    int thin_splits;                                   /* leaves split below surface_size for a thin wall */
    int unresolved_thin;                               /* thin walls still thinner than 1.5 cells at the finest cell:
                                                        * the mesh there is perforated or lost */
    double finest_cell;                                /* the smallest cell the octree could use (m) */
    int pinches, pinch_splits;                         /* edges held by 4+ boundary faces in the last round; leaves split for them */
    int cut_points, warped_vertices, steiner_vertices;
    int unwarped_vertices;
    int smoothed_nodes;                                /* node moves accepted by the quality smoothing */                             /* warps undone because they flattened a background tetrahedron */
    int bad_stencils;                                  /* pyramids or Steiner prisms not star-shaped (should be 0) */
    long long uncertain_vertices;                      /* inside test not unanimous (STL input) */
    double seconds;
} TetMesh;

bool tetmesh_generate(const TetMeshSettings *s, TetMesh *out, char *err, size_t errlen);
void tetmesh_free(TetMesh *m);

/* element quality helpers (corner coordinates) */
double tet_signed_volume(const double X[4][3]);
void tet_dihedral_angles(const double X[4][3], double deg[6]);
double tet_aspect_ratio(const double X[4][3]);

/* ---- an STL as a signed distance ------------------------------------------------------------------------------- */

typedef struct TetStlGeometry {
    const double *v;  /* 3*nv (m) */
    const int *tri;   /* 3*nt */
    int nt;
    void *bvh;        /* owned */
    double scale;     /* bounding-box diagonal (m) */
    double lo[3], hi[3];
    double volume;    /* signed volume of the triangles (m^3) */
    long long votes_split; /* evaluations where the three axis rays disagreed */
    long long evaluations;
} TetStlGeometry;

bool tet_stl_init(TetStlGeometry *g, const double *v, const int *tri, int nt, char *err, size_t errlen);
void tet_stl_free(TetStlGeometry *g);
double tet_stl_sdf(void *g, const double p[3]);
void tet_stl_project(void *g, const double p[3], double q[3]);
bool tet_stl_cross(void *g, const double a[3], const double b[3], double *t);
/* inside test only: ray parity along x, y and z, majority of the axes that voted */
bool tet_stl_inside(TetStlGeometry *g, const double p[3], bool *unanimous);
