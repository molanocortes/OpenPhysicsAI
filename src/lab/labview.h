/* labview.h - pictures of any physics-lab result (labio.h), on the software renderer: tools/labfilm.c writes them to
 * files, the app shows them in its window.
 *
 *   build/labfilm RESULT.lab --info
 *   build/labfilm RESULT.lab --out DIR [options]      writes DIR/frame_0000.png ...
 *
 * Two ways of drawing:
 *   plan   (default when every part is 2D blocks in the xy plane): each pixel takes the value of the finest block that
 *          covers it, so an adaptive mesh is shown exactly as the solver holds it. --mesh draws the cell edges of
 *          the owning block; --levels tints each refinement level; --mirror reflects about y = 0 (axisymmetric runs
 *          are stored as the half plane r >= 0); --schlieren shows exp(-k |grad f| / max), the look of a schlieren
 *          photograph; --mesh-below draws the flow above the axis and the mesh itself below it (needs --mirror).
 *   3d     (--view iso|front|top|left|...): blocks in any plane, points as round splats, cells as shaded surfaces
 *          (the outer faces of hexahedra), lines as lines; --mesh draws element edges.
 *
 * Options: --focus N --span M (a 3D view centred on point N)  --labels (body names)
 *          --azim DEG --elev DEG (a camera direction, in place of --view)  --field NAME  --range LO HI  --cmap turbo|viridis|coolwarm|heat|gray|ink  --size WxH  --every N
 *          --first N --last N  --title TEXT  --unit TEXT  --radius PX  --solid NAME (cells where NAME > 0.5 drawn as
 *          a solid body)  --nearest  --log  --abs  --light (light background)  --zoom F  --center X Y
 * The colour range is fixed over the whole film (given, or the extremes over all written frames), so frames compare. */
#pragma once

#include <stdbool.h>

#include "../render/swrender.h"
#include "labio.h"

typedef struct LabViewOpts {
    const char *in, *out, *field, *title, *unit, *view, *solid;
    double lo, hi;
    bool range_given, mesh, levels, mirror, schlieren, nearest, logscale, absval, light, info, mesh_below;
    int cmap; /* SwColormap or 100 = gray, 101 = ink */
    int w, h, every, first, last;
    int supersample;   /* render at this many times the size, then box-filter (1: no anti-aliasing, for interaction) */
    double radius, zoom, cx, cy;
    bool center_given;
    double azim, elev; /* a camera direction in degrees (--azim, --elev); used when given instead of a preset */
    bool angles;
    int focus;         /* --focus N: centre the 3D view on point N of the first points part (-1: none) */
    double span;       /* --span M: half-width of the view around the focus, m */
    double look_at[3]; /* the 3D view's centre, m, when look_at_given (the app's "lab focus X Y Z") */
    bool look_at_given;
    bool labels;       /* --labels: write the header's body names beside the points */
    bool true_size;    /* --true-size: points drawn with their "radius" field, in metres */
    const char *lines; /* --lines F [N]: contour lines of field F drawn over a 2D field (flux lines, isotherms) */
    int nlines;        /* how many levels across F's range, default 24 */
    bool no_chrome;    /* no title band, title or time in the picture (the app shows them in its own panel) */
    bool solid_points; /* points are pieces of a solid (particles of a metal): coloured from the metal up; set from the header */
    double point_radius_m; /* > 0: points drawn at this radius in metres (never smaller than `radius` pixels), so that
                              the particles of a liquid overlap into one body; set from the header for water */
    bool no_colorbar;  /* no colour bar in the picture (the app draws its own legend) */
    bool solid_volume; /* a computed volume is a solid body: its outside is opaque, sections show its inside */
    struct { char part[48]; int look; } looks[12]; /* parts drawn as a material, not by the field (the header's "looks") */
    int nlooks;
    bool walls;        /* a computed volume is a room: its far walls are drawn where the view leaves it (acoustics) */
} LabViewOpts;

void labview_defaults(LabViewOpts *o);
/* the colour range of a field over the frames the options select (first, last, every) */
void labview_range(LabFile *lf, const LabViewOpts *o, double *lo, double *hi);
/* one frame into *out (allocated, o->w by o->h); false on failure */
bool labview_render(LabFile *lf, const LabFrame *fr, const LabViewOpts *o, double lo, double hi, SwImage *out);
/* what a result holds, printed to stdout */
int labview_info(LabFile *lf);
/* the colour maps: SwColormap values, 100 gray, 101 ink, 102 ember, 103 glass */
void labview_cmap_rgb(int cmap, double t, float rgb[3]);
