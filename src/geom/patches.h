/* patches.h - surface patches of an STL body for AI-addressable face selection
 *
 * STL has no named CAD faces. Patches recover face-like regions deterministically:
 *   1. planar patches: region growing from the largest triangles across manifold edges while the neighbour's normal
 *      stays within planar_tol_deg of the seed normal;
 *   2. curved patches: the remaining triangles grouped across edges whose dihedral angle is below feature_angle_deg;
 *   3. classification: planar, cylindrical (normals perpendicular to a common axis and points on a circle, with
 *      radius and whether it is a hole or a boss) or other.
 * Patch ids depend only on the triangle soup and the two angles, not on placement, so selections by id survive
 * re-orientation; geometric attributes are evaluated in any frame (build frame for the API). */
#pragma once

#include <stdbool.h>

#include "surface.h"

typedef enum { PATCH_PLANAR = 0, PATCH_CYLINDRICAL, PATCH_OTHER } PatchType;

typedef struct PatchSet {
    int n;
    int *tri_patch;   /* per surface triangle */
    int *start;       /* n + 1: triangles of patch p are order[start[p] .. start[p+1]) */
    int *order;
    PatchType *type;
    int *adj_start;   /* n + 1 */
    int *adj;         /* adjacent patch ids */
    double planar_tol_deg, feature_angle_deg;
} PatchSet;

bool patches_build(const Surface *s, double planar_tol_deg, double feature_angle_deg, PatchSet *ps, char *err, size_t errlen);
void patches_free(PatchSet *ps);

typedef struct PatchGeom {
    int ntri;
    double area;
    double centroid[3];
    double normal[3];         /* area-weighted mean unit normal */
    double normal_spread_deg; /* largest deviation of a triangle normal from the mean */
    double bmin[3], bmax[3];
    PatchType type;
    double plane_d;           /* planar: normal . x = plane_d */
    double axis[3], axis_point[3], radius; /* cylindrical */
    bool concave;             /* cylindrical: normals point toward the axis (a hole) */
    double fit_rms;           /* planar: rms distance to the plane; cylindrical: rms radial deviation */
} PatchGeom;

/* v: 3*nv vertex positions in the target frame; R (row-major 3x3) rotates surface normals into that frame (NULL = identity) */
void patch_geometry(const Surface *s, const double *v, const double *R, const PatchSet *ps, int patch, PatchGeom *g);
const char *patch_type_name(PatchType t);
