/* breakest_fe.h - the break estimate on a solved part (GOALS.md G20 step 5): through the wall at the governing point,
 * the governing stress split into its membrane and bending parts, and the load factor at which that section breaks
 * (breakest.h).
 *
 * The line through the wall starts at the governing point and runs where the governing stress falls fastest (the
 * nodal stresses' gradient there, averaged over the elements around the point), until it leaves the part. Along it
 * the stress is interpolated inside the elements (hex8, tet4, tet10) and linearised as ASME VIII Division 2 Annex 5-A
 * does: the membrane part its mean over the wall, the bending part 6/t^2 times its first moment about the wall's
 * middle, positive when the governing point's side is the more tensile. A uniform stress (no gradient) is all membrane. */
#pragma once
#include <stdbool.h>

#include "../ctl/static_analysis.h"

typedef struct BreakSection {
    bool ok;              /* false: the point is not inside the mesh, or the stress there is not tensile */
    double dir[3];        /* the line's direction into the wall (unit), zero for a uniform stress */
    double wall;          /* m, the line's length inside the part */
    double membrane;      /* Pa: the mean of the governing stress along the line */
    double bending;       /* Pa: the linear part at the governing point's side */
    double at_point;      /* Pa: the governing stress at the point, interpolated */
    int samples;
    double load_factor;   /* the factor on all loads at which the section breaks (breakest_load_factor) */
} BreakSection;

/* node_stress: 6 per node, Voigt in the build frame; R: material axes as columns; comp: the governing component in the
 * material axes (0, 1, 2 for the normal stress along axis 1, 2, 3); s_t: its tensile strength (Pa); c and m the
 * section's material (breakest.h) */
bool breakest_fe_section(const StaticModel *mdl, const double *node_stress, const double R[9], int comp, const double x0[3], double s_t, double c,
                         double m, BreakSection *out);
