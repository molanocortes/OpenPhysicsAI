# `src/geom/` — geometry

From an STL file to something a solver can use: triangle meshes, surface topology and repair diagnostics, addressable
face patches, the voxel hexahedral mesher, a BVH for ray work, procedural shapes, and voxelisation onto the fluid
lattice.

Up: [module map](../../docs/map/MODULES.md#srcgeom--geometry) · [AGENTS.md](../../AGENTS.md).

## Read first

| File | What it gives you |
|---|---|
| `hexmesh.h` | `hexmesh_generate`: the layer-aligned voxel hex8 mesh every FEM analysis runs on, with boundary faces and staircase statistics |
| `tetmesh.h` | `tetmesh_generate`: a conforming tetrahedral mesh (TET4 or curved TET10) by isosurface stuffing on a graded BCC lattice, with its quality report; contract [docs/contracts/tet-mesh.md](../../docs/contracts/tet-mesh.md) |
| `surface.h` | `surface_build`: welding, edge topology, shells, self-intersections, thickness — the input diagnostics an agent is shown |
| `patches.h` | `patches_build`: planar and smooth regions, so a face can be named and selected instead of picked by index |
| `mesh.h` | STL import/export, normals, statistics |
| `bvh.h` | ray and closest-point queries (picking, projection, thickness) |

Also here: `voxel.h` (fluid lattice voxelisation, used by the app) and `shapes.h` (the built-in bodies behind the
app's `scene` command).

## Talks to

- [`core/`](../core/README.md) — JSON, paths, hashing.
- [`ctl/`](../ctl/README.md) — `ops_geometry.c`, `ops_surface.c`, `ops_mesh.c`, `selection.c` call into here.
- [`fem/`](../fem/README.md) — consumes the hex mesh.
- [`../`](../README.md) — the app voxelises models onto the lattice.

## Tests

```bash
make headless && ./build/surftest && ./build/meshtest
make tools && ./build/geomtest       # developer target: shapes, STL I/O, voxeliser, performance
```

`meshtest` checks patches and hex meshes against exact answers; `surftest` checks topology, repair reporting, shells,
self-intersections and thickness.

## What to know before trusting a mesh

The default mesher is a **voxel** mesher: the boundary is a staircase, so inclined and curved faces are approximated and their
volume error changes with the element size. That error is reported per level and is the reason a comparison study
withholds a discretisation-error estimate for such geometry
([EVIDENCE.md](../../docs/release/EVIDENCE.md)). The conforming mesher (`tetmesh.h`, `mesh_generate` with method tet) follows
the surface instead; it rounds sharp edges at the surface cell scale and serves the static structural analysis only.
Its verification: `./build/tettest` (M1 to M6); the owner's STL files with `./build/tettest --stl FILE --size 2`.

Printing-mesh robustness, 2026-10-03: `meshtest` PM1 to PM5 verify anisotropic layer alignment, exact multi-body
ownership and overlaps, serial/threaded classification equality, numeric input refusals and translation-stable closed
surface volume. The reported mean boundary offset is weighted by face area, so thin z layers do not bias it toward
their numerous small vertical faces. The thickness warning uses the largest grid spacing and remains conservative:
wall orientation determines which spacing resolves a wall. Refinement is still required before trusting stresses.
Body membership is retained from the original three-axis vote rather than queried again for every element. On the
eight-body, 64,000-element synthetic ownership fixture, three serial timings had median 0.224780 s before and
0.012112 s after (18.6 times faster); this is a mesher timing, not a print-solver speed claim. Retained membership
costs two bytes per bounding-grid cell for multiple bodies, and none for a single body.
