/* surface.h - indexed triangle surfaces for engineering geometry: vertex welding, edge topology, connected
 * components, orientation, nested shells, holes, self-intersections and wall thickness.
 *
 * A Surface is built from an STL triangle soup without modifying the source mesh. Every repair is optional and
 * counted in SurfaceDiagnostics, so nothing is fixed silently. Coordinates stay in the file's length unit. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bvh.h"
#include "mesh.h"

typedef struct Surface {
    int nv, nt;
    double *v;      /* 3*nv welded positions */
    int *tri;       /* 3*nt vertex indices */
    int *src;       /* nt: index of the originating STL facet */
    int *nbr;       /* 3*nt: triangle across edge k (tri[k] -> tri[k+1 mod 3]); -1 open edge, -2 non-manifold edge */
    int *comp;      /* nt: edge-connected component */
    int ncomp;
    double *normal; /* 3*nt unit face normals */
    double *area;   /* nt */
} Surface;

typedef struct SurfaceComponent {
    int ntri;
    double area;
    double volume; /* signed volume enclosed by the shell (meaningful when closed) */
    double bmin[3], bmax[3];
    bool closed;   /* no open or non-manifold edges */
    bool flipped;  /* orientation reversed by the repair */
    int parent;    /* smallest closed component enclosing this one, -1 if none */
    int depth;     /* nesting depth: 0 outer shell, 1 cavity, 2 island inside a cavity, ... */
} SurfaceComponent;

typedef struct SurfaceRepairOptions {
    bool remove_degenerate;      /* zero-area slivers and faces collapsed by welding */
    bool remove_duplicate_faces; /* exact repeats of a face with the same orientation */
    bool orient_outward;         /* consistent winding per shell, outward for solids and inward for cavities */
    double weld_tolerance_rel;   /* vertex merge distance relative to the bounding-box diagonal (default 1e-6) */
} SurfaceRepairOptions;

typedef struct SurfaceHole {
    int edges;
    double perimeter;
    double centroid[3];
} SurfaceHole;

enum { SURFACE_MAX_HOLES = 16, SURFACE_MAX_INTERSECTIONS = 32 };

typedef struct SurfaceDiagnostics {
    uint32_t source_triangles;
    uint32_t degenerate;          /* zero-area or collapsed faces found */
    uint32_t degenerate_removed;
    uint32_t duplicate_faces;     /* repeated faces with the same orientation */
    uint32_t duplicate_removed;
    uint32_t opposite_duplicates; /* the same face twice with opposite orientation (internal walls) */
    uint32_t unique_vertices, merged_vertices;
    double weld_tolerance;
    uint32_t open_edges, nonmanifold_edges;
    uint32_t inconsistent_edges;  /* manifold edges traversed in the same direction by both faces (before repair) */
    uint32_t non_orientable_components;
    uint32_t boundary_loops;
    int nholes;                   /* listed holes (largest perimeters first) */
    SurfaceHole holes[SURFACE_MAX_HOLES];
    int components, closed_components, flipped_triangles, flipped_components, nested_shells;
    double area, volume;
    double bmin[3], bmax[3];
    bool watertight;             /* no open and no non-manifold edges */
    bool consistently_oriented;  /* after repair */
    bool closed_solid;           /* watertight, consistently oriented, positive volume */
    /* optional checks (surface_check_*) */
    int self_intersections;      /* -1 not checked */
    int self_intersection_samples;
    double intersection_points[SURFACE_MAX_INTERSECTIONS * 3];
    double min_thickness, thickness_p05, thickness_median; /* NAN when not computed */
    double min_thickness_at[3];
    int thickness_samples;
} SurfaceDiagnostics;

void surface_repair_defaults(SurfaceRepairOptions *o);
bool surface_build(const Mesh *m, const SurfaceRepairOptions *opt, Surface *out, SurfaceComponent **comps,
                   SurfaceDiagnostics *diag, char *err, size_t errlen);
void surface_free(Surface *s);

/* Counts intersecting pairs of faces that share no vertex (touching counts). Stops after max_pairs. */
int surface_check_self_intersections(const Surface *s, const Bvh *bvh, int max_pairs, SurfaceDiagnostics *diag);
/* Local wall thickness by casting rays inward from `samples` face centroids (area-stratified, deterministic). */
void surface_check_thickness(const Surface *s, const Bvh *bvh, int samples, SurfaceDiagnostics *diag);
/* Point-in-solid by ray parity over all closed components (majority of three ray directions). */
bool surface_point_inside(const Surface *s, const Bvh *bvh, const double p[3]);
