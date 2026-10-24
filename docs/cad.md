# Printed-part geometry: exact parts and lattice cores, made by agents

Up: [AGENTS.md](../AGENTS.md) · the goal: [ROADMAP.md](../ROADMAP.md) G23

The owner, 2026-10-08, after a scored comparison of this project's geometry for agents with Onshape, Fusion and
SolidWorks: "go ahead with OpenCascade, focus on printed-part geometry". So: exact solids as far as printed parts need
them (Open CASCADE Technology 7.9.3), and where the lab leads, the skin and lattice core of a printed part, made so that
the result is always a closed, manifold solid (Manifold 3.5.4), measured and checked before anyone prints it.

Both run in one program of their own, `build/openphysics-cad` ([src/bin/openphysics_cad.cpp](../src/bin/openphysics_cad.cpp)), as
`labrun` does for the lab: the app and openphysics-mcp never load the libraries, and a fault in them cannot take the app
down. Every command prints one JSON record on stdout.

## Build

```bash
make                  # builds openphysics-cad only once its libraries exist (make cad); it never starts their hour by itself
make cad              # only the libraries: Open CASCADE 7.9.3, Manifold 3.5.4 (and on macOS CMake 3.31.12), fetched, checked, built static
make build/openphysics-cad
make test-cad         # tools/cadtest.cpp, tools/cadparttest.py, tools/cadlatticetest.py, tools/cadprinttest.py
```

`tools/build_cad.py` fetches the pinned archives, checks their SHA-256 and builds with two jobs (`CAD_JOBS` to change
it); no Homebrew. Everything lands in one place per machine, `~/.openphysicsai/third_party/` (`CAD_TP` to change it),
shared by every copy of the repository on that machine: the hour is paid once per machine, not once per worktree.
On Linux the distribution's CMake builds them (Ubuntu: `sudo apt install cmake`). Licences and checksums:
[THIRD_PARTY.md](THIRD_PARTY.md).

## An exact part from a feature list

```bash
build/openphysics-cad part MODEL.json OUTDIR [--deflection-mm 0.01]
```

`MODEL.json` lists features in millimetres, each with an `id`, built in order; `result` names the part (the last
feature by default). On success the program writes `OUTDIR/part.step` and `OUTDIR/part.stl` (closed and manifold,
chordal deflection 0.01 mm by default) and prints every feature's measures and the part's named faces.

```json
{"features": [
  {"id": "plate", "op": "box", "size_mm": [60, 40, 8]},
  {"id": "boss", "op": "cylinder", "radius_mm": 8, "height_mm": 20, "at_mm": [30, 20, 0], "axis": "z"},
  {"id": "body", "op": "union", "of": ["plate", "boss"]},
  {"id": "bore", "op": "cylinder", "radius_mm": 4, "height_mm": 40, "at_mm": [30, 20, -10]},
  {"id": "part", "op": "cut", "from": "body", "remove": ["bore"]},
  {"id": "round", "op": "fillet", "of": "part", "edges": {"parallel": "z", "between": "planes"}, "radius_mm": 5}
]}
```

| op | keys |
|---|---|
| `box` | `size_mm` [x, y, z]; `at_mm` (its lowest corner) or `centre_mm` |
| `cylinder` | `radius_mm`, `height_mm`, `at_mm` (the base's centre), `axis` (`"z"` by default, `"-x"`, or [x, y, z]) |
| `cone` | `radius1_mm` (base), `radius2_mm` (top), `height_mm`, `at_mm`, `axis` |
| `sphere` | `radius_mm`, `centre_mm` |
| `sketch` | `plane` `"xy"`, `"xz"` or `"yz"`, `offset_mm`; `points_mm` [[u, v], ...] (a closed outline) or `circle` {`centre_mm` [u, v], `radius_mm`} |
| `extrude` | `sketch`, `distance_mm` along the plane's normal (+z, +y, +x; negative goes the other way) |
| `revolve` | `sketch`, `axis`, `axis_point_mm`, `angle_deg` (360 by default) |
| `union`, `intersect` | `of` [ids] |
| `cut` | `from` id, `remove` [ids] |
| `translate`, `rotate` | `of`; `by_mm`; or `axis`, `about_mm`, `angle_deg` |
| `pattern` | `of`, `count`; `step_mm` (a row) or `axis`, `about_mm`, `angle_deg` (a ring) |
| `fillet`, `chamfer` | `of`, `edges` (a selector), `radius_mm` or `distance_mm` |
| `shell` | `of`, `open_faces` (a selector), `thickness_mm` (the walls, inwards) |

Edges and faces are chosen by their geometry, never by index, since an index changes whenever the part does:
`{"parallel": "z"}` (edges along z; planar faces facing along z), `{"between": "planes"}` (edges between two planar
faces: not a seam, not a curved edge), `{"extreme": "+z"}` (furthest along +z), `{"kind": "circle"}` (or `line`;
faces `plane`, `cylinder`, `cone`, `sphere`), `{"near_mm": [x, y, z], "count": n}`. Every key of a selector must hold;
a list of selectors chooses what any of them chooses. The record says how many entities each selector chose.

A feature that cannot be made, or that leaves an invalid solid, refuses the whole part: `{"ok": false, "error":
{"feature", "op", "message"}}`, and nothing is written. `build/openphysics-cad measure PART.step` measures a STEP file
(anyone's) and names its faces.

## A printed part's skin and lattice core

```bash
build/openphysics-cad lattice REQUEST.json OUTDIR
```

```json
{"part": "part.step", "skin_mm": 1, "resolution_mm": 0.15,
 "core": {"kind": "gyroid", "form": "sheet", "cell_mm": 6, "solid_fraction": 0.3}}
```

The part (STEP or STL) keeps a solid skin of `skin_mm` under its surface; its core is a lattice: `gyroid`,
`schwarz_p` or `diamond` (triply periodic minimal surfaces) as a `sheet` of `wall_mm` or as a `network` (one of the
surface's two labyrinths; `level` shifts it), or `bcc` or `octet` struts of diameter `wall_mm`, or `none` (a hollow
part) or `solid`. `solid_fraction` may be asked for instead of a wall: the wall (or level, or strut) that gives it on
one periodic cell is found by bisection, and the fraction the part's core really has is measured afterwards.

How: the part's signed distance is computed on a grid of `resolution_mm` from its triangles (exact within a band
under and over the surface, its sign from the crossings along each column), combined with the lattice's field, and
Manifold's level set turns the whole into one mesh, closed and manifold by construction. The record gives the part's
volume, the skin's (and its thinnest depth where sampled), the core's region, solid volume and solid fraction, the
result's triangles and genus, how far any vertex lies outside the part, and whether the written STL's edges close.
A wall or skin thinner than two grid steps, a solid fraction outside 0.05 to 0.95, an unknown kind or a part that
cannot be read are refused with the reason.

A closed skin round a core leaves enclosed voids: the gyroid's two labyrinths, cut off by the skin, or the whole inside
of a hollow part. They keep their powder (or resin) when printed, so the record counts them (`result.enclosed_voids`),
and `"drains"` drills holes: `{"radius_mm": 1}` drills one along each void's shortest way out of the solid (found as
the check below finds it) and looks again, up to three rounds, until none is left; `[{"from_mm", "to_mm",
"radius_mm"}]` drills the holes given; `drains` in the record lists them. Each hole is a round-ended cylinder (a
capsule) from the point in the void to its radius past the point outside, put into the same field as the skin and the
core, and the solid is made again by the level set, so the holes come out as clean as the rest (the first try, cutting
them from the mesh with Manifold's booleans, ran holes tangent to the skin's flat inner faces and left slivers that a
32-bit STL joins wrongly). A void that touches the outside at a single point is drilled too: nothing drains through a
point. Each round costs one more level set, about as long as the first.

## A cell's stiffness

```bash
build/openphysics-cad cell REQUEST.json
```

```json
{"core": {"kind": "gyroid", "form": "band", "cell_mm": 6, "solid_fraction": 0.3}, "poisson_ratio": 0.33, "voxels": 48}
```

One periodic cell of a core (the same `core` object as the lattice's; `"form": "band"` is the sheet most papers use,
the band |G| < level, beside our sheet of constant thickness) is voxelised, each voxel's solid fraction from 4 x 4 x 4
samples, and homogenised by the support module's periodic solver (`support_cell_stiffness` in
[src/mech/support_cell.h](../src/mech/support_cell.h): hex8 voxels scaled by their fraction, six macro strains,
periodic fluctuations). The record gives the cell's stiffness over the solid's modulus (6 x 6, order xx, yy, zz, xy,
yz, zx, engineering shear), the moduli, shear moduli and Poisson's ratios along the cube axes, the cubic constants
and Zener ratio, the solid fraction the voxels hold, the Voigt bound's margin and Hashin and Shtrikman's bound on the
bulk modulus. `poisson_ratio` is the solid's and is required. 48 voxels across the cell take about a minute (64: about
three, within 0.4 per cent of 48 for the band gyroid). openphysics-mcp offers it as `cad_cell`.

A periodic cell is an infinite lattice. Printed cubes of six cells measured 35 to 45 per cent softer than it in two
studies (below): use it as the lattice's upper value.

```bash
build/openphysics-cad specimen REQUEST.json
```

```json
{"core": {"kind": "gyroid", "form": "band", "cell_mm": 6, "solid_fraction": 0.3}, "poisson_ratio": 0.33,
 "cells": [6, 6, 6], "voxels_per_cell": 10, "platens": "both"}
```

A whole test cube of the core instead of one cell ([src/cad/specimen.h](../src/cad/specimen.h)): voxelised from the
lattice's origin, its sides free and its sheets cut where it ends, the largest connected piece kept, and compressed
along z between rigid platens, frictionless (only the axial displacement held) and bonded (all three held), by the
same elements (`src/fem/solid.c`, PCG on the performance cores). The record gives each platen's apparent modulus over
E_s, the periodic cell's at the same voxels, and their ratio, the size factor. A 6 x 6 x 6 gyroid cube at 10 voxels
per cell is about 0.6 million unknowns and solves in half a minute; `max_unknowns` (1.6 million by default) refuses
larger ones. Li et al.'s cubes came out within a few per cent of the infinite lattice (size factor 0.97 to 1.09), so
specimen size does not explain why printed cubes measure softer. At 16 voxels per cell (about 2 million unknowns, more than the
8 GB laptop holds; on the desktop compute node, 2 to 6 minutes a solve) the size factors moved by 1.1 per cent at most
from 10 (GOALS.md G23, S5).

## Printability

```bash
build/openphysics-cad check REQUEST.json OUTDIR
```

```json
{"part": "part.step", "process": "powder_bed_fusion", "overhang_deg": 45, "min_wall_mm": 0.4, "build_direction": "+z"}
```

The process is named by its ISO/ASTM 52900 category: `powder_bed_fusion`, `binder_jetting`,
`vat_photopolymerization`, `material_jetting` or `material_extrusion`. `overhang_deg` (the least angle between a
downward surface and the plate that prints without support) and `min_wall_mm` (the thinnest wall) are the process's
or the machine's own values: they are required and never guessed (AGENTS.md rule 3). `build_direction` is the part's
own +z when not given, and the record says so.

- Overhangs: the downward surfaces at less than `overhang_deg` to the plate, in connected regions, each with its area,
  its lowest angle and the support under it: every piece of such a surface, projected on the plate, times its drop to
  the part below or to the plate. The angle is followed across a curved surface from its corners' normals (each the
  mean of the normals around a vertex within 30 degrees of the triangle's own), so the 45 degree line on a sphere is
  found between the triangles' corners; a flat face keeps its own normal exactly. A downward face within 0.01 mm of the
  plate rests on it. The same is computed along each of the six axis directions, and the one that needs the least
  support is named.
- Thin walls: each triangle's depth of solid along its inward normal, to the surface on the far side; the triangles
  thinner than `min_wall_mm`, in connected regions, each with its thinnest point.
- Enclosed voids: closed surfaces inside the solid, each with its volume, its box and its shortest way out (the point
  of the void nearest an outer surface, that outer point, and the depth of solid between). They keep powder in powder
  bed fusion and binder jetting, resin in vat photopolymerization and support material in material jetting; in
  material extrusion they print as closed cavities and are noted, not counted as an issue.

The record's `issues` say each problem in a sentence (`needs_support`, `too_thin`, `traps_material` as flags), with the
twenty largest regions and voids of each kind; `OUTDIR/check.json` holds all of them, and `OUTDIR/checked.stl` the part
with its triangles coloured in the VisCAM and SolidView convention (overhang red, too thin blue, both magenta, a void's
surface orange). A part whose surface is not closed and manifold is refused, as are missing or unknown values.

## Verified, and not yet

All on 2026-10-08, criteria written before the first runs (GOALS.md G23 and the tops of the test files):

- The libraries (`tools/cadtest.cpp`, 8 s): Open CASCADE's volumes of a box less a hole, a rounded edge and a revolved
  rectangle against their closed forms within 1.6e-16, 1.1e-16 and 5.8e-16 relative; STEP out and back exact; the box's
  STL at 0.01 mm closed and manifold within 2.9e-5; Manifold's gyroid network 0.49984 solid against an exact 0.5 (1.8
  million triangles); two spheres' union and difference exact by inclusion and exclusion. All passed at the first run.
- Exact parts (`tools/cadparttest.py`, 0.7 s): every feature of the plate, boss and hole at its closed form within
  1e-9; four vertical edges chosen by a selector and rounded, 18400 + 648 pi mm^3; an L profile revolved (Pappus); a
  box shelled open at the top, 8672 mm^3 exactly; STEP read back exactly, the STL closed; the hole named a cylinder
  of radius 4 mm along z through (30, 20); three refusals with the feature's id, nothing written. All passed at the
  first run.
- Skins and cores (`tools/cadlatticetest.py`, about 100 s): the gyroid network 0.49989 solid; a gyroid sheet's mesh
  within 0.07 per cent of an independent count of its field and 1.5 per cent of the thin-sheet value; a 40 mm cube's
  1 mm skin 9128.00 mm^3 exactly; the plate with a 1 mm skin and a gyroid core asked at 0.3: 0.3033 measured, no vertex
  outside the part (0.0001 mm), the skin at least 0.994 mm where sampled, 37 s; every STL closed and manifold by
  Manifold, by the program and by the test; four refusals. The first run failed three checks, each a fault of the
  program, fixed without changing a criterion: walls came out 3.6 per cent thin (0.5785 mm for 0.6, the distance
  taken as G / |grad G| at the point; now the point is projected onto the surface, and 0.6 mm measures 0.5994 mm);
  1459 points where two pieces of the solid touch broke the STL once its floats were joined (each copy of such a point
  is now moved 0.1 micrometre into its own piece); and the skin's depth was read from points within a grid step of a
  sharp edge, which the level set rounds (now measured from where the solid begins, that start reported apart).
- For agents (`tools/cadagenttest.py`): both tools in openphysics-mcp's default and lean profiles (the lean list is now 14
  tools in about 4 200 tokens, 1 000 more than before); through MCP a part, its lattice (core 0.3033) and the part
  brought into a project by `geometry_import` (20607.10 mm^3 against 20607.43, the STL's facets). The first run found
  that openphysics-mcp's own engine could not read the lab folder its tools write into; it now can. With `--real-model`,
  Claude Haiku 4.5 through Claude Code, one sentence: both made in one turn of 70 s for $0.05, the model correcting
  itself after a refusal (a grid too coarse for the wall it needed), the core at 0.3031; the model read "a boss
  standing 20 mm from the plate's floor" as 20 mm above the plate, so it made a different part than the test meant
  (18400 + 1032 pi mm^3, exactly that part's volume), which the criterion records as such.
- Printability (`tools/cadprinttest.py`, 2026-10-08, about five minutes; the third run, no criterion changed): a
  sphere's overhang below 45
  degrees 183.565 mm^2 against 184.030 (0.25 per cent) and its support 216.409 mm^3 against 216.882 (0.22 per cent);
  the prism's 30 degree underside alone, 200 mm^2 and 1000 sqrt 3 mm^3 exactly (its 60 degree one not); the table
  exactly along each of the six axes, -z named the best; the fin's thinnest wall 0.5 mm and thin area 240 mm^2 in two
  regions, exactly; the tube's 0.399876 mm and 1231.38 mm^2 against 1231.50; the cube's hidden void 522.63 mm^3
  against 523.60, its way out 10.0000 mm; the latticed plate's 295 voids (9489.81 mm^3 of powder) found in 3.5 s,
  the outer surface less the voids equal to the written solid within 4e-7, the outer surface within 0.05 per cent of
  the part; every refusal; `cad_check` through openphysics-mcp; and the plate drained by `cad_lattice` itself: 299 holes of
  1 mm in two rounds (107 s for the whole), no enclosed void left, its STL closed and manifold, and the check of it
  finds no void. The first run's holes, cut from the mesh with booleans, left 12 edges its STL joined wrongly; the
  second's, drilled in the level set, left 4 tiny voids touching the outside at a single point, until such points
  were passed over as ways out.
- Checked as it is, the gyroid bracket of step 3 shows what a lattice still needs: 298 enclosed voids (9432 mm^3 of
  powder), two specks of 0.00001 mm^3 apart from the solid, slivers thinner than 0.4 mm (24 mm^2 in all, in 2827
  places) where its sheets meet the skin, and 2957 mm^2 of its sheets' own downward faces counted as overhangs, which
  a lattice usually prints without; the check says all of it, and does not yet tell a lattice's inside from the part's
  outside.
- A cell's stiffness (`tools/cellstifftest.c`, `tools/cadcelltest.py`, first run 2026-10-08): a full cell gives the
  solid's matrix within 1.2e-13 and a plate a quarter of the cell's height its plane-stress terms exactly; every
  gyroid, Schwarz P and diamond cell, sheet or network, cubic within 4e-5; the band gyroid at a solid fraction of 0.3
  E/Es 0.12089, 0.11858 and 0.11817 at 32, 48 and 64 voxels; every cell inside the Voigt bound, the sheets' bulk
  moduli at 87 to 88 per cent of Hashin and Shtrikman's bound. Against others, it failed both criteria set for it:
  Li, Liao, Dai and Xie (Materials 12:2183, 2019) homogenised the same two gyroids in ABAQUS at 0.0749 and 0.0477
  (ours 58 and 47 per cent higher), and printed them in an SLA resin, 36 mm cubes of 6 x 6 x 6 cells, measured at
  62.51 and 39.21 MPa where E_s times our fraction predicts 100.46 and 59.50 MPa. DeValk and Lakes (Physica Status
  Solidi B 258:2100081, 2021) measured laser-sintered PA2200 gyroid sheets at 0.062 of the feedstock's modulus at 0.3.
- The printed specimen (`tools/specimentest.cpp`, `tools/cadspecimentest.py`, 2026-10-08): a solid cube and a wall
  between platens exactly; Li et al.'s 36 mm cubes at 8 and 10 voxels per cell, size factors converged within 2 per
  cent (0.97 frictionless, 1.04 bonded for the sheet; 1.01 and 1.09 for the network); predicted 97 to 104 MPa and 60
  to 65 MPa against the measured 62.51 and 39.21: the criterion failed, and the six-cell cube is not why.
- Not yet: why printed lattices measure about 0.6 of their designed geometry (the printed walls' solid and thickness,
  the test), the homogenised core inside a part's analysis, specks dropped and slivers smoothed in the lattice, overhangs inside a lattice told apart, the core's
  strength (step 5), graded cores, 3MF, sketches with arcs, lofts and sweeps, and the parts in the app's window
  and the headset. A gyroid part's STL is large (the plate: 0.46 million triangles written, 5.8 million in the level
  set before it is simplified).
