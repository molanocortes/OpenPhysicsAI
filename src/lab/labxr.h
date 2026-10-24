/* labxr.h - a physics-lab frame as the headset's scene (docs/xr.md).
 *
 * What the app draws of a 3D lab result (labscene.h, labgpu.c), as a glTF 2.0 binary the headset page loads: surfaces
 * coloured from the legend's palette, parts with a material look in that material, particles, lines, and the computed
 * volume as a 3D texture the page ray-marches the way labgpu.c does. Every vertex keeps its stored value (_VALUE), so the
 * headset reads numbers, not colours; every vertex keeps its part (_PART, named in the extras).
 *
 *   build/labxr RESULT.lab --info                                   title, frames, times, fields, units, one box
 *   build/labxr RESULT.lab --frame N [--view JSON] -o OUT.glb|-     one frame as the window shows it
 *
 * Axes: the lab is z up and glTF y up, so glTF (x, y, z) = lab (x, z, -y), in metres, with no offset: every frame of a
 * result keeps one placement, and the box in the info (lab axes) places all of them. The node "result" carries the
 * frame's facts in extras.result (field, unit, range, palette, parts, section, volume); the volume's samples are
 * (value - lo) / (hi - lo) as IEEE half floats, x fastest, in its own buffer view, and its mask (iso surface, water
 * coverage or a body's occupancy, labscene_volume_mask) as bytes 0..255 in another. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"
#include "labio.h"

/* how the window shows a result (labapp.c), written into the headset link's state.json and read back by build/labxr */
typedef struct LabXrView {
    char field[48], unit[32];
    double lo, hi;          /* the legend's range */
    int cmap;               /* the legend's palette (labapp_legend); 200 (STYLE_AUTO): chosen from the range */
    int section_axis;       /* 0 x, 1 y, 2 z, -1 none */
    double section_fraction;
    bool section_flip;
    char iso_field[48];     /* "volume_look": "iso": surfaces where it crosses iso_level (vortex cores) */
    double iso_level;
    bool water_surface;     /* water particles drawn as their reconstructed surface */
    double water_spacing;   /* m */
    bool mesh, absval, logscale, solid_points, true_size, solid_volume, walls;
    double radius;          /* points without a size of their own: pixels at 720 lines */
    double point_radius_m;  /* points: a size in metres when > 0 */
    int nlooks;
    struct { char part[48]; int look; } looks[12];
} LabXrView;

void labxr_view_defaults(LabXrView *v);
JsonValue *labxr_view_json(const LabXrView *v);
/* false with a reason when a member has the wrong type or lies outside its range; members left out keep defaults */
bool labxr_view_from_json(const JsonValue *j, LabXrView *v, char *err, size_t cap);

/* frame i of lf, as the view shows it, as a GLB (malloc'd); false with a reason (no such frame, nothing to draw). A
 * result that is a plane (2D blocks) is drawn as an upright picture of it at true size, the field sampled in the finest
 * block at each pixel, with a coarser grid of the values for probing (extras.result.picture); an r, g, b picture (the
 * black hole) 0.4 m wide. */
bool labxr_frame_glb(LabFile *lf, int frame, const LabXrView *v, unsigned char **glb, size_t *len, char *err, size_t cap);
/* the facts the headset reads once per result: title, domain, time unit, frame count and times, fields and units,
 * whether it is drawn in 3D, and the box (lab axes, m) holding the first, middle and last frames */
JsonValue *labxr_info(LabFile *lf, char *err, size_t cap);

/* the palette as a 256 x 1 PNG: texels 0 to 247 the map, 248 to 255 a neutral grey; a value at t in 0..1 of the range
 * is read at u = (0.5 + 247 t) / 256 (the centre of a texel) */
enum { LABXR_TEXELS = 248 };
unsigned char *labxr_palette_png(int cmap, size_t *len);
