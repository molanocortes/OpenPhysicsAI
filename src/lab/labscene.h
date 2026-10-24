/* Retained geometry from actual 3D result nodes. No extrusion or inferred volume. */
#pragma once
#include "labio.h"
#include "labview.h"
/* n: a smooth normal (triangle surfaces, welded by position), or zero for a flat one from the face */
/* part: the index in the frame of the part it was drawn from (the headset names what you point at) */
typedef struct LabVertex { float p[3], value, radius, field, n[3]; int part; } LabVertex;
typedef struct LabScene {
    LabVertex *tri, *points, *lines;
    size_t ntri, npoints, nlines;
    double lo[3], hi[3];
} LabScene;
/* axis -1 disables the cell-centroid section; fraction in [0,1], flip keeps the other side. */
bool labscene_build(const LabFrame *fr, const LabViewOpts *o, int axis, double fraction, bool flip, LabScene *s);
void labscene_free(LabScene *s);
bool labscene_supported(const LabFrame *fr);
/* the material looks a header may give parts ("looks": {"windings": "copper"}): 0 none; drawn by labgpu.c */
enum { LOOK_NONE, LOOK_COPPER, LOOK_MAGNET_NORTH, LOOK_MAGNET_SOUTH, LOOK_STEEL, LOOK_ALUMINIUM, LOOK_GLASS, LOOK_FABRIC, LOOK_TISSUE, LOOK_COUNT };
int labscene_look_id(const char *name);
const char *labscene_look_name(int look); /* "copper", ...; "" for LOOK_NONE or out of range */

/* One regular 3D grid, suitable for a retained volume texture. */
const LabPart *labscene_volume(const LabFrame *fr);
/* The volume's second texture, one value per cell, as the app ray-marches it (labgpu.c) and the headset does (labxr.h):
 * the iso field over twice its level, clamped to 0..1, so that 0.5 is the surface (vortex cores, when iso_field is given
 * and level > 0); else a stored "coverage" (water, as stored); else a body's "material" occupancy box-filtered once, so
 * that its surface is traced smooth rather than in voxel steps; else zero. Returns 'i', 'c', 'm' or 0 for which. */
int labscene_volume_mask(const LabPart *volume, const char *iso_field, double level, float *mask);
