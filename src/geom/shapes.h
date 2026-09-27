/* shapes.h - procedural test geometries (watertight, outward CCW winding, model-space convention
 * from mesh.h: flow along +X, Y up, Z span). Sizes are in meters at "natural" scale; the application
 * rescales to the requested model size anyway. */
#pragma once

#include <stdbool.h>
#include "mesh.h"

typedef struct ShapeInfo {
    const char *name;        /* identifier used by the terminal, e.g. "glider" */
    const char *description; /* one-line description */
} ShapeInfo;

/* Number of built-in shapes and their descriptors. */
int shape_count(void);
const ShapeInfo *shape_info(int index);

/* Generate shape by name (case-insensitive). Output mesh is initialised by this call, has normals and
 * bounds computed, and name set. Returns false if the name is unknown. */
bool shape_generate(const char *name, Mesh *out);
