/* hexmesh.h - layer-aligned voxel hexahedral meshes of closed STL solids
 *
 * VOXEL-DERIVED MESH. The build volume is sampled on a regular grid aligned with the build frame. Element layers start at
 * the build plate top (z = 0), so element layers coincide with deposited layer groups. A cell belongs to a body when its
 * sample point near its centre is inside the body's closed surface (majority of ray-parity tests along x/y/z). All elements are
 * undistorted boxes (Jacobian = hx hy hz / 8 everywhere) and share nodes conformingly.
 *
 * Consequences that callers must report: the boundary is a staircase, so volume and surface area differ from the STL,
 * curved and inclined faces are blunted, re-entrant stair steps create artificial stress concentrations, and walls
 * thinner than two elements are poorly resolved or lost. hexmesh_generate measures these errors (volume and area against
 * the STL, distance from boundary faces to the true surface) so resolution dependence can be quantified.
 *
 * Boundary faces of part elements are projected onto the nearest body triangle with a compatible normal, which maps
 * surface selections made on the STL deterministically onto the mesh. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../threads.h"

typedef enum { HEX_REGION_PART = 0, HEX_REGION_PLATE = 1, HEX_REGION_SUPPORT = 2, HEX_REGION_COUNT } HexRegion;

/* the largest share of cells the three axis tests may disagree on before the mesh is refused */
#define HEXMESH_UNCERTAIN_LIMIT 0.05

typedef struct HexMeshBody {
    const double *v;      /* 3*nv vertices in the build frame (m) */
    const int *tri;       /* 3*nt vertex indices, outward winding */
    int nt;
    const double *normal; /* 3*nt unit normals in the build frame */
    HexRegion region;     /* HEX_REGION_PART or HEX_REGION_SUPPORT */
} HexMeshBody;

typedef struct HexMeshSettings {
    double h[3];            /* element size along x, y, z (m) */
    bool include_plate;
    double plate_thickness; /* m */
    double plate_margin;    /* m of plate beyond the bodies' footprint */
    uint64_t max_elements;
    ThreadPool *pool;
} HexMeshSettings;

typedef struct HexMesh {
    int nnodes, nelems;
    double *xyz;             /* 3*nnodes (m) */
    int *conn;               /* 8*nelems, hex8 node order */
    unsigned char *region;   /* per element: HexRegion */
    signed char *body;       /* per element: index into the bodies passed in, -1 for the plate */
    int *ijk;                /* 3*nelems cell indices */
    double origin[3], h[3];  /* cell (i,j,k) spans origin + (i..i+1, j..j+1, k..k+1) * h */
    int dims[3];
    int plate_layers;        /* element layers below z = 0 */
    int *cell_elem;          /* dims[0]*dims[1]*dims[2]: element index or -1 */
    /* boundary faces: element faces without a neighbour element */
    int nfaces;
    int *face_elem;
    unsigned char *face_local;  /* hex8 local face 0..5 */
    int *face_tri;              /* nearest compatible triangle of the owning body, -1 for plate faces */
    float *face_dist;           /* distance from the face centre to that triangle (m) */
    /* quality of the staircase approximation (part and support bodies) */
    int nbodies;
    double body_volume_mesh[16], body_volume_stl[16];
    double body_area_mesh[16], body_area_stl[16];
    double mean_face_distance, max_face_distance; /* area-weighted mean of projected boundary-face centres, maximum (m) */
    int regions;             /* face-connected element regions */
    int edge_contacts;       /* nodes shared by different face-connected regions (edge or vertex contact) */
    /* cells whose centre lies inside more than one body of the same region: the mesher gives such a cell to one body
     * (the later one), so a non-zero count means the geometry overlaps. overlap_pairs[a][b] (a > b) counts cells that
     * went to body a but are also inside body b. Counting only; ownership is unchanged. */
    int overlap_cells;
    int overlap_pairs[16][16];
    /* how sure the inside test was. A cell is inside when at least two of the three axis rays say so; a cell the
     * three axes did not agree on is counted here, with the box those cells occupy (build frame, m). On a closed
     * surface this is only the handful of cells a ray grazes; a large count means the surface has gaps or doubled
     * facets big enough to flip a ray, and the mesh there is a majority opinion rather than a fact. */
    long long uncertain_cells;
    long long abstained_rays;  /* rays with an odd crossing count: they went through a hole or a zero-thickness sheet */
    double uncertain_min[3], uncertain_max[3];
    double seconds;
    /* element type: 0 for the voxel meshes of this file (npe 8). A tetrahedral mesh (tetmesh.h, carried in this struct
     * by the mesh operation) sets 1 or 2 (TET4, TET10) and npe 4 or 10; conn then holds npe nodes per element,
     * face_local is the tetrahedral local face 0..3, and the voxel-only arrays (ijk, cell_elem) are NULL. */
    int elem_type;
    int npe;
} HexMesh;

bool hexmesh_generate(const HexMeshBody *bodies, int nbodies, const HexMeshSettings *s, HexMesh *out, char *err, size_t errlen);
void hexmesh_free(HexMesh *m);
/* element containing a point (build frame, m), -1 if none */
int hexmesh_locate(const HexMesh *m, const double p[3]);

/* ---- adaptive coarsening of a layer-aligned voxel mesh (docs/contracts/adaptive-mesh.md) ------------------------- *
 *
 * Merges aligned 2 x 2 blocks of voxel elements into one, recursively, in x and y only (an element stays one voxel
 * tall, so it belongs to one layer), up to 2^max_level voxels wide. A block merges only if all its voxels exist, share
 * one kind (kind 255 never merges), and none of them touches a missing voxel across a face, edge or corner in its own
 * layer or the layers above and below: the free surface stays fine. Neighbours across faces, edges and corners then
 * differ by at most a factor of two (2:1 balance, enforced by lowering levels until nothing violates it).
 *
 * The input is any mesh of axis-aligned voxel boxes of one size h (the uniform mesh, supports included); the output is a
 * new mesh whose nodes are a subset of the input grid points, so every output node exists in the input. A node inside an
 * edge or a face of a coarser element is hanging: it is listed with its masters (the edge ends, or the face corners),
 * their weights and the coarse element that owns the constraint; the same node may be listed once per owner. */
typedef struct HexCoarsened {
    int nnodes, nelems;
    double *xyz;          /* 3 * nnodes */
    int *conn;            /* 8 * nelems, the input's corner order */
    int *parent;          /* nelems: an input element it covers (the block's first voxel) */
    unsigned char *level; /* nelems: 0 for a voxel, L for 2^L x 2^L voxels */
    int *node_of_input;   /* input nnodes: output node, or -1 when the node fell inside a coarse element */
    int nhang;
    int *hang_node, *hang_nmaster, *hang_master, *hang_owner; /* nhang, nhang, 4 * nhang, nhang */
    double *hang_weight;                                      /* 4 * nhang */
} HexCoarsened;

/* fine_band: the width, in voxels, of the fine band kept along every free surface, and above and below it (at least 1) */
bool hexmesh_coarsen_xy(const double *xyz, int nnodes, const int *conn, int nelems, const double h[3],
                        const unsigned char *kind, int max_level, int fine_band, HexCoarsened *out, char *err, size_t errlen);
void hexmesh_coarsened_free(HexCoarsened *c);
