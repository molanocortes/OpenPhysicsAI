# `src/` — the fluid tunnel and the native application

The files directly in `src/` are the real-time 3D lattice Boltzmann water tunnel and the macOS application that hosts
both the fluid and the solid workspace. Everything headless lives in the subdirectories:
[`core/`](core/README.md), [`geom/`](geom/README.md), [`fem/`](fem/README.md), [`ctl/`](ctl/README.md),
[`net/`](net/README.md), [`render/`](render/README.md), [`bin/`](bin/README.md).

Up: [module map](../docs/map/MODULES.md#src-root-files--the-fluid-tunnel-and-the-app) · [AGENTS.md](../AGENTS.md).

## Read first

| File | Why |
|---|---|
| `lbm.h` / `lbm.c` | the solver: D3Q19 fused stream-collide, regularised BGK with a Smagorinsky LES term |
| `sim.h` / `sim.c` | the simulation manager: physical units, model placement, the solver thread, snapshots, forces |
| `main.c` | entry point and frame loop |
| `fembridge.h` / `fembridge.c` | **the bridge to the headless engine**: every app action goes through `ops_invoke`, the same operations an agent calls |
| `app.h` / `app.c` | application state, display fields, scenes, camera presets, probing |

## The rest, by job

- **Interface:** `ui.c` (immediate-mode widgets), `hud.c` (toolbar, panels, legend), `console.c` + `commands.c`
  (in-app terminal and its commands), `font.c` (CoreText atlas), `colormap.c`.
- **Graphics:** `render.c` (OpenGL 4.1 scene), `glutil.c`, `image.c` (PNG through ImageIO),
  `platform_macos.c` (Cocoa window, context and input, in plain C through the Objective-C runtime).
- **Visualisation kernels (CPU):** `vis_fields.c` (derived scalar fields), `vis_stream.c` (streamlines),
  `vis_iso.c` (isosurfaces), `visworker.c` (background pipeline). Header: `vis.h`.
- **Shared utilities:** `common.c` (time, memory, logging), `threads.c` (thread pool), `math3d.h`.

## Talks to

- [`geom/`](geom/README.md) — voxelisation of the model onto the lattice, built-in shapes.
- [`ctl/`](ctl/README.md) — through `fembridge.c`: projects, meshes, analyses, results.
- [`core/`](core/README.md) — JSON and paths, through the bridge.

## Build and check

```bash
make            # ./navier plus the headless binaries
make test-ui    # tools/uicheck.py: clicks every control off-screen and asserts its effect
make tools      # build/lbmbench, build/geomtest, build/vistest, build/gltest (developer tools)
./navier        # W switches between the FLUID and SOLID workspaces
```

The fluid solver has **no closed-form suite in `make test`**; its benchmarks are in
[../docs/water-tunnel.md](../docs/water-tunnel.md) and its kernels are exercised by `build/vistest` and
`build/lbmbench` ([GEMS.md](../docs/map/GEMS.md)). It is **not reachable through MCP**: only `src/fem/flow.c` uses the
kernel headlessly, for conjugate heat transfer.

Compiled with `-O3 -ffast-math` (`CFLAGS`), unlike everything numerical in the subdirectories, which uses
`-O2 -ffp-contract=off`. `lbm.c` is compiled once in each regime; the two objects never meet in one binary.
