/* lattice.h - a printed part's skin and core (GOALS.md G23 step 3, docs/cad.md).
 *
 * The part (a STEP file, made by openphysics-cad part or anyone, or an STL) keeps a solid skin of a thickness under its
 * surface; inside the skin the core is a lattice: a triply periodic minimal surface (gyroid, Schwarz P, diamond) as a
 * sheet of a wall thickness or as one of its two networks, or struts (body-centred cubic, octet) of a diameter, or
 * nothing, or solid. The whole is one signed-distance field (positive inside): the part's own distance, computed on a
 * grid from its triangles (exact within a band under the surface, its sign by counting crossings along each column),
 * combined with the lattice's; Manifold's level set makes it one mesh, closed and manifold by construction. A solid
 * fraction may be asked for instead of a wall: the wall (or the network's level, or the strut) that gives it on one
 * periodic cell is found by bisection, and the fraction reached in the part is measured afterwards from the meshes.
 *
 * Request (JSON, millimetres):
 *   {"part": "part.step", "skin_mm": 1, "resolution_mm": 0.15, "origin_mm": [0, 0, 0],
 *    "core": {"kind": "gyroid", "form": "sheet", "cell_mm": 6, "wall_mm": 0.6}}       (or "solid_fraction": 0.3)
 * kinds: gyroid, schwarz_p, diamond (form sheet or network; a network's "level" shifts it), bcc, octet (wall_mm is the
 * strut's diameter), none (a hollow part, only its skin), solid.
 * A closed skin round a core leaves enclosed voids, which keep their powder or resin when printed; the record counts
 * them, and "drains" drills holes: {"radius_mm": r} one along each void's shortest way out (printcheck.h), again while
 * any is left, or a list [{"from_mm", "to_mm", "radius_mm"}], each a cylinder reaching r past both ends. */
#pragma once

#include <string>

extern "C" {
#include "../core/json.h"
}

/* writes OUTDIR/infilled.stl and returns {"ok": true, "part": ..., "skin": ..., "core": ..., "result": ...}, or
 * {"ok": false, "error": {"message"}} with nothing written. The caller owns the returned value. */
JsonValue *lattice_build(const JsonValue *request, const std::string &outdir);

/* the homogenised stiffness of one periodic cell of a core (GOALS.md G23 step 5): {"core": {as above, or "form":
 * "band" for the sheet |G| < level}, "poisson_ratio": the solid's (required), "voxels": across the cell (48)}. Each
 * voxel's solid fraction from 4 x 4 x 4 samples, solved by support_cell_stiffness (src/mech/support_cell.h). Returns
 * {"ok", "solid_fraction", "stiffness_over_Es" (6 x 6 in the project's order xx, yy, zz, xy, yz, zx, engineering shear), "cube_axes" {moduli, shear
 * moduli, Poisson's ratios}, "cubic" {C11, C12, C44, modulus, zener_ratio, bulk}, "bounds"}. */
JsonValue *lattice_cell(const JsonValue *request);

/* a test cube of a core compressed between rigid platens (GOALS.md G23 step 5c, src/cad/specimen.h): {"core": {...},
 * "poisson_ratio", "cells": [nx, ny, nz] (loaded along z), "voxels_per_cell": 10, "platens": "both" | "frictionless"
 * | "bonded"}. Returns the specimen's apparent modulus over E_s for each platen, the periodic cell's at the same voxels
 * and their ratio, the size factor. */
JsonValue *lattice_specimen(const JsonValue *request);
