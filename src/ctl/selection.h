/* selection.h - AI-addressable surface selections on STL bodies
 *
 * A selection is a JSON query over the analysis surface of one body, evaluated in the build frame (lengths in mm):
 *   {"patches": [3, 7]}                                   patch ids from surfaces_list
 *   {"facing": {"direction": "-z", "max_angle_deg": 10}}  normals within an angle of a direction (+x -x +y -y +z -z
 *                                                          up down, or [x, y, z])
 *   {"plane": {"axis": "z", "at": "min", "tolerance": 0.01}}  all vertices on an axis-aligned plane ("min", "max" or mm)
 *   {"plane": {"point": [0, 0, 5], "normal": [0, 0, 1], "tolerance": 0.01}}
 *   {"extreme": {"direction": "+z", "tolerance": 0.05}}   the outermost surface along a direction
 *   {"box": {"min": [..], "max": [..]}}                   triangle centroids inside a box
 *   {"sphere": {"center": [..], "radius": 5}}             triangle centroids inside a sphere
 *   {"near": {"point": [..], "distance": 2}}              triangles within a distance of a point
 *   {"type": "planar" | "cylindrical" | "other"}          patch surface type
 *   {"cylinder": {"radius_min": 2, "radius_max": 4, "hole": true}}
 *   {"area": {"min": 100, "max": 1e4}}                    patch area in mm^2
 *   {"adjacent_to": {"patches": [5]}}                     patches sharing an edge with the given patches
 *   {"pick": {"view_id": "v12", "pixel": [412, 305]}}     the patch under a pixel of a rendered view
 *   {"all": [q, ...]}  {"any": [q, ...]}  {"not": q}
 * Mode "patch" (default) selects whole patches whose matching area fraction is at least `coverage`; mode "triangle"
 * selects the matching triangles. Resolution is deterministic, and its set hash detects when a geometry or placement
 * change makes a selection resolve differently. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/errors.h"
#include "project.h"

/* resolves the pixel of a rendered view to a triangle of body b (build frame ray cast); returns false with err */
typedef bool (*SelectionPickFn)(void *ctx, const char *view_id, double px, double py, Body *b, int *tri, char *err, size_t errlen);

typedef struct SelectionContext {
    SelectionPickFn pick;
    void *pick_ctx;
} SelectionContext;

/* (Re)evaluates sel->query on body b and fills the resolution fields. Returns false with an error code and message
 * when the query is invalid or matches nothing (the previous resolution is kept in that case). */
bool selection_resolve(Body *b, Selection *sel, const SelectionContext *ctx, NvErr *code, char *err, size_t errlen);
/* re-resolves after a geometry/placement change; marks the selection stale when the resolved set changed */
void selection_revalidate(Body *b, Selection *sel, const SelectionContext *ctx);
/* mesh faces of the current mesh that belong to the selection (-1 terminated list not used: returns count) */
int selection_mesh_faces(const Project *p, const Selection *sel, int *faces, int max_faces, double *mesh_area);
bool selection_has_triangle(const Selection *sel, int tri);
/* a named build-frame direction (+x -x +y -y +z -z up down) or a vector [x, y, z], normalised */
bool direction_from_json(const JsonValue *v, double d[3], char *err, size_t errlen);
JsonValue *selection_json(const Project *p, const Selection *sel, bool detailed);
