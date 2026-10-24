/* cadpart.h - exact parts from a feature list, for printed-part design by agents (GOALS.md G23 step 2, docs/cad.md).
 *
 * A part is a list of features in millimetres, each with an id, built in order with Open CASCADE 7.9.3: primitives
 * (box, cylinder, sphere, cone), a sketch (a closed polygon or a circle in the xy, xz or yz plane) extruded or
 * revolved, booleans (union, cut, intersect), linear and circular patterns, edges rounded (fillet) or bevelled
 * (chamfer), a solid hollowed to walls of a thickness with chosen faces left open (shell), and moves (translate,
 * rotate). Edges and faces are chosen by geometric selectors, never by index (an index changes when the part does):
 *
 *   {"parallel": "z"}           edges along z; planar faces facing along z (either way)
 *   {"between": "planes"}       edges whose two faces are both planes (not a seam, not a curved edge)
 *   {"extreme": "+z"}           the faces (by centroid) or edges (by midpoint) furthest along +z; also -z, +x...
 *   {"kind": "circle"}          edges that are circles (or lines); faces that are planes, cylinders, cones, spheres
 *   {"near_mm": [x, y, z], "count": n}   the n entities nearest to a point (1 by default)
 *
 * every key of one selector must hold; a list of selectors chooses what any of them chooses. Every feature's result is
 * measured (volume, area, box, solids, validity) and returned, a selector says how many entities it chose, and the
 * finished part names every face with its kind and parameters. A feature that cannot be made, or that leaves an
 * invalid solid, refuses the whole part with its id and the reason; nothing is written then.
 *
 * It runs in its own program (build/openphysics-cad, src/bin/openphysics_cad.cpp), so that the libraries never run inside the
 * app or openphysics-mcp. */
#pragma once

#include <string>

extern "C" {
#include "../core/json.h"
}

/* builds the part of `model` ({"features": [...], "result": id}); on success writes OUTDIR/part.step and
 * OUTDIR/part.stl (chordal deflection `deflection_mm`) and returns the record {"ok": true, "features": [...], "part":
 * {...}, "stl": {...}, "files": {...}}; on refusal returns {"ok": false, "error": {"feature", "op", "message"}} and
 * writes nothing. The caller owns the returned value. */
JsonValue *cadpart_build(const JsonValue *model, const std::string &outdir, double deflection_mm);

/* the record of an existing STEP file: its solids measured and its faces named, as the "part" of cadpart_build */
JsonValue *cadpart_measure_step(const std::string &path);
