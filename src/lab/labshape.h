/* labshape.h - bodies for the lab's 3D solvers, described once: signed distances (negative inside, metres) for
 * spheres, cylinders, boxes and wings of NACA four-digit section, read from a scenario's "bodies" list, and a smooth
 * triangle surface of their union for the pictures (marching tetrahedra on the distance field).
 *
 *   {"shape": "sphere",   "centre_m": [x, y, z], "radius_m": r}
 *   {"shape": "cylinder", "centre_m": [x, y, z], "radius_m": r, "axis": "x"|"y"|"z", "length_m": L}
 *   {"shape": "box",      "box_m": [x0, y0, z0, x1, y1, z1]}
 *   {"shape": "revolved", "profile_m": [[a, r], ...], "origin_m": [x, y, z], "pitch_deg": p}: a closed half profile in
 *    (axial, radial) coordinates, closed along its axis, turned about the axis; the axis runs along +x pitched by p
 *    degrees (nose up for a stream along +x when p > 0), the profile's (0, 0) at origin_m (a capsule, a projectile)
 *   {"shape": "wing", "root_leading_edge_m": [x, y, z], "chord_m": c, "span_m": b, "naca": "0012", "angle_deg": a,
 *    "tip_chord_m": c_tip (optional, a straight taper), "sweep_deg": s (optional, of the leading edge),
 *    "span_axis": "y" (default), "-y" or "z" (a left wing, a fin)}
 *
 * A wing's span runs along +y from its root; its angle of attack pitches the nose up about the spanwise axis through
 * the root quarter chord, for a stream along +x. Sections are the NACA four-digit profiles (Abbott and von Doenhoff),
 * closed trailing edge (the last coefficient -0.1036). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"

enum { SHAPE_SPHERE, SHAPE_CYLINDER, SHAPE_BOX, SHAPE_WING, SHAPE_REVOLVED, SHAPE_PIPE, SHAPE_ROOT };
enum { SHAPE_MAX = 16, SHAPE_PROFILE = 161 };

typedef struct LabShape {
    int kind;
    double c[3], r, len;          /* sphere, cylinder; pipe: the bore's axis through c, radius r (solid outside it, unbounded) */
    double sc[3][3], sr;          /* aortic root: a pipe along x with three sinuses, spheres of radius sr centred at sc */
    int axis;                     /* cylinder: 0 x, 1 y, 2 z */
    double lo[3], hi[3];          /* box */
    double le[3], chord, tip_chord, span, angle, sweep, m, p, t; /* wing: camber m, its position p, thickness t */
    double prof[2 * SHAPE_PROFILE][2]; /* wing: the unit-chord section, closed polygon; revolved: (a, r) */
    int nprof;                    /* revolved: vertices */
    int span_axis;                /* wing: 0 +y, 1 -y, 2 +z */
    double dir[3];                /* revolved: unit axis */
    double aabb_lo[3], aabb_hi[3];
} LabShape;

typedef struct LabShapes {
    int n;
    LabShape s[SHAPE_MAX];
} LabShapes;

/* reads "bodies"; false with a message on a malformed entry (an absent or empty list is fine: no bodies) */
bool labshape_parse(const JsonValue *bodies, LabShapes *out, char *err, size_t errlen);
/* signed distance to the union of the bodies at p (metres; negative inside) */
double labshape_sdf(const LabShapes *S, const double p[3]);
/* the surface of the union inside the box [lo, hi], sampled every h metres: a triangle list (3 points per triangle,
 * xyz interleaved), malloc'd; *ntri triangles. NULL if there is no surface. */
double *labshape_mesh(const LabShapes *S, const double lo[3], const double hi[3], double h, int *ntri);
/* a triangle soup (9 per triangle) as shared nodes and connectivity (3 per triangle), positions equal to a nanometre;
 * returns the node count (-1 out of memory); a result then stores each vertex once, not six times */
int labshape_weld(const double *soup, int ntri, double **nodes, int **conn);
/* frontal area seen along x (a projection on a fine grid over the box), m^2, for force coefficients */
double labshape_frontal_area(const LabShapes *S, const double lo[3], const double hi[3], double h);
