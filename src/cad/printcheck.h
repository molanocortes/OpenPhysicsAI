/* printcheck.h - can a part be printed as it is (GOALS.md G23 step 6, docs/cad.md#printability).
 *
 * For a process named by its ISO/ASTM 52900 category and the limits the caller gives (never guessed):
 *  - overhangs: downward surfaces at less than overhang_deg to the build plate, and the support under them (each piece
 *    of such a surface times its drop to the part below or to the plate), for the build direction and for the six axis
 *    directions; the angle is followed across a curved surface from its corners' normals, so a sphere's 45 degree line
 *    is found between the triangles' corners, while a flat face keeps its own normal exactly. A downward face within
 *    0.01 mm of the plate rests on it.
 *  - thin walls: each triangle's depth of solid along its inward normal, to the surface on the far side; thinner than
 *    min_wall_mm is reported, in connected regions with their thinnest point.
 *  - enclosed voids: closed surfaces inside the solid. They keep their powder (powder bed fusion, binder jetting), resin
 *    (vat photopolymerization) or support material (material jetting); in material extrusion they print as closed
 *    cavities. Each comes with its shortest way out through the solid, where a drain hole would go.
 *
 * Request (JSON, millimetres and degrees):
 *   {"part": "part.step" or "x.stl", "process": "powder_bed_fusion", "overhang_deg": 45, "min_wall_mm": 0.4,
 *    "build_direction": "+z" (or [x, y, z]; +z, the part's own, when not given), "deflection_mm": 0.01 (STEP only)}
 * processes: powder_bed_fusion, binder_jetting, vat_photopolymerization, material_jetting, material_extrusion. */
#pragma once

#include <string>

extern "C" {
#include "../core/json.h"
}

/* writes OUTDIR/checked.stl (the part, its triangles coloured: overhang red, thin blue, both magenta, a void's surface
 * orange; VisCAM and SolidView's colour bits) and OUTDIR/check.json (every region and void) and returns the record
 * ({"ok": true, ...}, its lists cut to the largest), or {"ok": false, "error": {"message"}} with nothing written. The
 * caller owns the returned value. */
JsonValue *printcheck_run(const JsonValue *request, const std::string &outdir);
