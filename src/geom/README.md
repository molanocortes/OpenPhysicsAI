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
