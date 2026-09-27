/* style.h - the physics lab's one look: every result, of every domain, drawn in the same room with the same palette,
 * light and materials, so that the lab reads as one world rather than a set of separate programs (GOALS.md G12).
 *
 * Palette: two maps only. Magnitudes (a speed, a temperature, a plastic strain, |B|) use STYLE_SEQUENTIAL; signed
 * quantities (vorticity, a pressure deviation, a scattered field, a velocity component) use STYLE_SIGNED, whose zero is
 * the dark of the room so that structure glows out of it on both sides. style_pick chooses between them from the data,
 * so no domain chooses its own colours.
 *
 * Room: a dark gradient behind, a floor with a faint grid under every object, one key light from the upper left and a
 * soft fill; solids are a light, slightly warm metal; the text is light on the band at the top. */
#pragma once

#include "../render/swrender.h"

#define STYLE_SEQUENTIAL SW_CMAP_LAB
#define STYLE_SIGNED SW_CMAP_LAB_SIGNED
#define STYLE_AUTO 200 /* LabViewOpts.cmap: choose from the data */

/* signed data (both signs present beyond 5 % of the larger magnitude) gets the diverging map, the rest the sequential */
static inline int style_pick(double lo, double hi) {
    double m = (lo < 0 ? -lo : lo) > (hi < 0 ? -hi : hi) ? (lo < 0 ? -lo : lo) : (hi < 0 ? -hi : hi);
    return (lo < -0.05 * m && hi > 0.05 * m) ? STYLE_SIGNED : STYLE_SEQUENTIAL;
}

/* the room */
static const unsigned char STYLE_BG_TOP[3] = {12, 15, 24}, STYLE_BG_BOTTOM[3] = {27, 32, 47};
static const float STYLE_FLOOR[3] = {0.075f, 0.090f, 0.125f}, STYLE_GRID[3] = {0.16f, 0.24f, 0.33f};
static const float STYLE_SOLID[3] = {0.80f, 0.80f, 0.78f};  /* light metal, slightly warm */
static const float STYLE_SLAB_SIDE[3] = {0.20f, 0.23f, 0.30f};
static const double STYLE_LIGHT[3] = {-0.45, -0.35, 0.82}; /* toward the key light, normalised by the user */
