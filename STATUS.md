# NAVIER-AM status

Additive-manufacturing FEM and AI-control extension of NAVIER. This file is the checkpoint: what works, what was
actually tested, known failures, exact commands and the next task.

## Milestones

| # | Milestone | State |
|---|---|---|
| 1 | Repository audit and baseline | done |
| 2 | Shared typed operations, socket control, minimal MCP (stdio) | done (real-client check blocked, see below) |
| 3 | Surface selection, volume mesh, static structural FEM, first MCP-controlled path | done (scripted MCP client; real AI client unverified) |
| 4 | Transient thermal FEM and thermoelastic coupling | done (checkpoint/resume added 2026-09-16, see `Thermal Sim/STATUS.md`) |
| 5 | FFF/FDM activation, cooling and bed release | next |
| 6 | LPBF inherent strain and transient elastoplastic workflows | not started |
| 7 | Fluid coupling, viewer integration, Streamable HTTP, hardening | viewer integration done; fluid coupling: first conjugate heat transfer study through MCP (2026-09-16, `Thermal Sim/STATUS.md`); HTTP not started |
| R | Integration release 0.3.0-rc1: headless comparison of bracket designs (MCP and CLI, evidence record, replay, package) | release candidate for internal review (2026-09-16); real AI-client sessions, the CalculiX cross-check and user tests still open. See `release/READINESS.md` |
| E | Independent evaluation of 0.3.0-rc1, giving 0.3.0-rc2 | done 2026-09-17: scientific claims audited; CalculiX comparisons (same problem and independent build) agree; beam discrepancy explained; estimator, root-limit and storage defects fixed with regression tests (`make test` now includes `build/evaltest`); rc2 package clean-install tested. Real AI-client sessions run 2026-09-17 (18 graded): competent tool use, but questions meant for the user were answered by the model and claims went beyond the record; five NAVIER design findings open. Still open: independent users, physical measurements, licensing. See `release/evaluation/EVALUATION_REPORT.md` |

Thermal Sim has its own folder: **`Thermal Sim/`** holds the module's theory document (governing equations,
assumptions, discretisation, declared unsupported regimes), the capability matrix (every feature marked unavailable /
experimental / verified / validated with its evidence), the baseline audit, the verification logs and three
demonstration studies with their saved specifications. `Thermal Sim/STATUS.md` is the checkpoint for the thermal work;
this file stays the checkpoint for the repository as a whole.

## Build, run, test

```bash
make            # fluid GUI (with the AM engine and the `am` terminal command) + navier-server, navier-mcp, navier-ctl
make test       # headless: core, surface, mesh, FEM and thermal verification, render, operations, socket, AM path,
                # MCP protocol, and the two complete AI-driven workflows (amflow: structural, thermflow: thermal)
make headless   # everything that builds without Cocoa/OpenGL (the Linux target)
./navier --listen                  # GUI serving the control socket at ~/.navier/run/control.sock
./navier-server                    # headless engine behind the control socket
./navier-ctl call capabilities_get # talk to a running server
./navier --headless --size 1440x900 --exec "exec tools/uitest.nav"   # GUI regression (read the log)
make test-ui                       # clicks every control off-screen and asserts what it changed (tools/uicheck.py)
./navier-ctl doctor                # diagnostics: binaries, libraries, writable folders, MCP stdio, reference solve
./navier-ctl study run examples/bracket_comparison/study.json --dir ~/NAVIER-Projects/bracket_ab   # comparison study
tools/package.sh && tools/install_test.sh dist/navier-*.tar.gz                                   # release package and clean install
./navier --workspace /tmp/ws       # put NAVIER-AM projects somewhere other than ~/NAVIER-Projects
```

In the application, `W` (or the FLUID / SOLID buttons) switches between the water tunnel and the analysis
workspace; both halves share one window, camera, colour map and terminal.

MCP client setup: `docs/mcp-clients.md`. Control protocol: `docs/control-protocol.md`. Analysis workflow, run files and
verification: `docs/analysis.md`.

## Current test results (2026-09-15, this machine)

| Test | Result |
|---|---|
| `build/coretest` (JSON, schema, units, SHA-256/base64 vectors, paths) | 199 passed, 0 failed |
| `build/surftest` (topology, repairs, shells, holes, intersections, thickness) | 30 passed, 0 failed |
| `build/meshtest` (voxel mesher: counts, plate, curved volume error, face mapping, limits) | 29 passed, 0 failed |
| `build/femtest` (patch test, rigid-body modes, tension, cantilever convergence, gravity, thermal strain, constraints) | 42 passed, 0 failed |
| `build/rendertest` + `tools/pngcheck.py` (rasteriser, colour maps, PNG; independent decoder) | 29 passed, 0 failed; PNG check passed |
| `build/opstest` (operation layer: validation, revisions, idempotency, import, placement, save/open) | 111 passed, 0 failed |
| `build/ctltest` (socket framing, malformed input, limits, timeouts, disconnects, TCP auth) | 47 passed, 0 failed |
| `build/thermtest` (conduction, k(T), semi-infinite step, order of accuracy, radiation, fin, moving source, activation, phase change, Stefan front, contact resistance, thermoelastic) | 72 passed, 0 failed |
| `build/amtest` (operation-level static, transient thermal and thermomechanical paths, conflicts, staleness, jobs, reload) | 102 passed, 0 failed |
| `python3 tools/mcptest.py` (independent MCP client: negotiation, errors, tools, bridge) | 194 passed, 0 failed |
| `python3 tools/amflow.py` (complete MCP stdio path on an L bracket, VTU check, disconnect and retry) | 71 passed, 0 failed |
| `python3 tools/thermflow.py` (complete thermal MCP path: conduction against theory, anisotropy, melting, thermomechanical) | 89 passed, 0 failed |
| `tools/uitest.nav` (GUI, real input path) | 0 errors |
| `python3 tools/uicheck.py` (every control clicked through the real input path, effects asserted, both workspaces) | 52 passed, 0 failed |
| Claude Code real client | not re-run: login expired on this machine (see known failures) |

## 1. Baseline audit

macOS 15.7.3 arm64 (M2, 8 GB), Apple clang 17.0.0, Python 3.10 + NumPy 2.2.6 (no SciPy), no Homebrew/CMake/Gmsh. Not a
git repository; the original sources were snapshotted before any change. Clean build of the untouched sources: 0
warnings; existing checks passed. Observation (not investigated): `lbmbench` measured 5-29 MLUPS for 1-8 threads,
below the README's 39-130 MLUPS.

Design facts: the viewer is Y-up with float math and the GUI is built with `-ffast-math`; the AM core uses
`CORE_CFLAGS` (no fast-math, `-ffp-contract=off`, double precision). The LBM "temperature" only sets fluid properties.
Gmsh 4.15.2 (GPL-2.0-or-later with linking exception, C API) was evaluated but is not installed; the dependency-free
voxel hexahedral mesher was chosen, with its staircase error quantified.

## 2. Milestone 2: shared operations, control socket, MCP (done)

- `src/core`: strict JSON, JSON Schema validator with defaults and quantity checks, unit parser, SHA-256, base64,
  root-restricted paths, stable error codes.
- `src/ctl`: operation registry from the embedded `ops_schema.json`, revisions, idempotent replay, change journal,
  access roots, `project.json` with provenance and assumptions.
- `src/net`: navier-ctl/1 control socket (Unix socket 0600 with peer-uid check, optional loopback TCP with token,
  NDJSON JSON-RPC, limits and timeouts), client library, MCP core (2025-11-25 down to 2024-11-05).
- `navier-server`, `navier-mcp` (embedded or bridge), `navier-ctl`; GUI `am` command and `--listen`.

## 3. Milestone 3: selection, mesh, static structural FEM, first MCP path (done)

Implemented:
- Geometry: `patches.c` (planar, cylindrical and other face patches from STL triangles), `hexmesh.c` (layer-aligned
  voxel hex8 mesh, optional build plate, boundary faces projected to STL triangles, volume/area/distance errors,
  face-connected regions, edge contacts).
- Selections (`selection.c`): JSON queries (patches, triangles, facing, plane, extreme, box, sphere, near, type,
  cylinder, area, adjacent_to, pick, all/any/not), patch coverage, SHA-256 set hashes, stale detection after geometry
  or placement changes, projection onto mesh faces. Picks on rendered views are stored as patch ids.
- Views (`swrender.c`, `viewrender.c`, `views.c`): software rasteriser with per-pixel colour mapping, PNG, patch
  labels, highlighted selections, camera metadata with the pixel convention, ray picking; result images with legend,
  magnified deformation, crease outline of the undeformed shape and a maximum marker.
- Materials (`matlib.c`, `materials.json`): embedded library of five entries, all labelled demonstration; project
  records validated (SI plausibility ranges, temperature tables); assignments with provenance.
- Boundary conditions: fixed, displacement, frictionless (flat axis-aligned faces), force (total, by area), pressure
  and traction (STL resultant preserved), gravity. Conflicts on shared faces are rejected; overlaps at shared edges
  are warned.
- Static analysis (`static_model.c`, `static_run.c`, `static_io.c`, `jobs.c`): validation (mesh currency, materials,
  stale references, conflicts, load resultants, rigid-body modes before solving), FIFO job worker with progress,
  cancellation, duplicate detection by specification hash, idempotent retries; run directory with `spec.json`
  (hashed setup, software, resolved materials), `results.nvr`, `summary.json`; VTU (appended raw) and CSV exports;
  queries, probes, renders; flags for peaks at supports and at sharp re-entrant corners.
- 32 operations; job and result operations run without the engine lock so long solves never block them.

Verification numbers:
- Cantilever 100 x 10 x 10 mm, E = 200 GPa, nu = 0.3, 100 N tip load, 2.5 mm elements (40 x 4 x 4), incompatible
  modes: tip deflection 0.19962 mm against Timoshenko 0.20156 mm (-0.96 %); mid-span bending stress within the 5 %
  test tolerance of +-30 MPa; reaction 100 N to 1e-6 N; with gravity the reaction grows by the weight 0.770085 N;
  equilibrium error about 1e-12; 2U/W = 1 to 1e-6. Full integration: 0.19297 mm (3.3 % stiffer, shear locking).
- Earlier FEM convergence (femtest, L/h = 10, Timoshenko 4.024): incompatible modes 4.010 / 4.0175 / 4.0222 / 4.0236
  for 10x1x1 / 20x2x2 / 40x4x4 / 80x8x8 elements; standard hex8 2.680 / 3.573 / 3.901 / 3.993.
- L bracket (amflow): 2115 elements (2 mm), bolted underside, 500 N lateral push on the upright, 316L demonstration
  material: max displacement 0.0774 mm; nodal von Mises peak 96.2 MPa 1.4 mm from the inner corner, flagged
  PEAK_AT_REENTRANT_CORNER (99th percentile 80.3 MPa); reaction -500 N; direct solve 0.11 s. Indicative only
  (demonstration material, singular corner).
- Voxel mesher (meshtest): cylinder volume error 5.78 % / 0.59 % / 0.59 % at h = 2 / 1 / 0.5 mm.

Issues found and fixed during M3: scalar fields were interpolated as colours (wrong hues inside elements); a schema
default ("auto") that was not a length; recursive `$ref` in tool schemas (now inlined with a plain nested schema);
reopened projects regenerated an automatic mesh instead of the stored one (now rebuilt and hash-checked).

## 4. Milestone 4: transient thermal FEM and thermoelastic coupling (done)

Implemented:
- `src/fem/thermal.c`: hex8 conduction with temperature-dependent k, cp and rho, lumped or consistent capacity,
  theta-method integration (backward Euler and Crank-Nicolson), prescribed temperature, heat flux, convection,
  radiation (Newton-linearised about the face-mean temperature), volumetric and moving Gaussian sources with power
  renormalisation, element activation, anisotropic conductivity, latent heat by apparent capacity, and thermal
  contact resistance between bodies. Each step is solved in correction (delta) form, so the solver tolerance applies
  to the change over the step; the per-step energy balance is reported. Element geometry is cached by quantised
  shape; assembly is parallel over a greedy element colouring.
- Sequential thermoelastic coupling (`transient_analysis.h`, `thermal_run.c`): at every stored time the same mesh is
  solved structurally with the thermal strain integrated from the stress-free reference temperature and with E(T)
  applied per element. One-way only: the structure does not feed back into the temperature field.
- Operations: thermal boundary conditions (`temperature`, `heat_flux`, `convection`, `radiation`, `heat_source`),
  `analysis_run` kinds `transient_thermal` and `thermomechanical`, time-indexed `results_query` / `results_probe` /
  `results_render` / `results_export`, `results.nvt`, one VTU per stored time plus a `.pvd` collection, and a CSV
  time series.

Verification numbers (`build/thermtest`, 72 checks):
- Steady 1D conduction with a flux boundary, and Kirchhoff's transform for k(T): nodally exact.
- Semi-infinite solid after a surface-temperature step, decaying sine mode: second order in space and in time for
  Crank-Nicolson, first order for backward Euler, measured against a fine-step reference.
- Lumped cooling, radiation equilibrium and a fin with lateral convection against their closed-form solutions.
- Moving source: the energy put in equals the energy stored plus the energy lost, per step.
- Latent heat: melt and re-freeze of 2.106e9 J/m^3, worst enthalpy mismatch 8.7e-14 (at most 93 Picard iterations in
  a step; 144 before Aitken relaxation was added). The Stefan run needs 5.0 iterations per step on average.
- Stefan melting front, Stefan number 1: front at 39.167 mm against the similarity solution 39.216 mm (-0.12 %).
- Thermal contact resistance: 3.448 W through a wall with a 29 K/W series resistance, 17.24 K jump at a
  2000 W/(m^2 K) contact.
- Speed: 96000 elements / 102541 nodes, 0.051-0.064 s per backward Euler step on a cool machine (assembly
  0.007-0.011 s, solve 0.039-0.048 s, energy 0.005 s, 40 CG iterations, 4 threads). This M2 is fanless and throttles,
  so the figure doubles on a warm machine; the comparison against the pre-enthalpy solver (0.047-0.057 s) was measured
  back to back and puts the cost of quadrature-point properties plus the independent enthalpy check at about 8 %.

Through the operation layer (`build/amtest`): the energy closes over a whole run (closure error < 1e-6), the heat
absorbed by a bar with a prescribed surface temperature is within 12 % of the semi-infinite solution, the stored
times, history, probes, rendered images (PNG with a degC legend) and the three export formats are all produced, and
a thermomechanical run builds thermal stress that vanishes again when the part returns to a uniform temperature.

## 5. Combined interface: one window for both solvers (milestone 7, viewer part)

The application now has two workspaces that share the window, the camera, the colour maps, the legend and the
terminal. `W`, or the FLUID / SOLID buttons in the toolbar, switches between them; switching to SOLID puts the flow
layers away and pauses the solver (the cores are wanted for the finite-element solve) and switching back restores
exactly the layers that were on.

- `src/fembridge.c` is the only place where the interface meets the engine. It mirrors the analysis state for the
  panel, runs every interface action as one of the typed operations (`ops_invoke` with transport `ui`) so the
  buttons can do nothing the AI cannot and the other way round, follows the running job through the lock-free
  `job_status`, and turns a finished result into a triangle surface for the 3D view.
- The result surface is the boundary of the hex mesh, one quad per face with its own normal (a voxel mesh is
  faceted, and smoothing it would suggest a smoothness the discretisation does not have), displaced by the chosen
  exaggeration and carrying the nodal value as a vertex scalar. `render_set_result` uploads it; the isosurface
  shader draws it with a matte term (`u_flat`) so the colour stays the one the legend promises instead of blooming.
- Build frame to viewer world is explicit and written down once: `world = centre + scale * (x_b, z_b, -y_b)` about
  the part's bounding-box centre, which keeps the mapping right-handed (the engine is Z-up in metres, the viewer is
  Y-up in lattice units).
- Panel (ANALYSIS): SETUP (project, geometry, element size and GENERATE, material), LOADS (the conditions with
  removal, FIX BASE, and prefilled terminal lines for anything that needs a number the user must choose), RUN
  (analysis kind, times, RUN ANALYSIS or CANCEL, progress, and the solver's own checks: equilibrium, energy, peak
  stress with its 99th percentile, maximum displacement, solve time and the warnings). RESULT shows the field
  buttons the result actually carries, the peak (labelled `peak (singular)` when the solver flagged a peak at a
  support or a sharp corner), the deformation scale, the stored-time slider and the colour-range choice.
- New terminal commands: `workspace fluid|solid|toggle`, `solid run|wait|cancel|fixbase|import|export|status`,
  `fem field|deform|step|range|show|hide|fit`. `solid import` hands the model in the tunnel to the analysis (asking
  for the length unit of the file, which is never guessed) and `solid export` loads the analysed body back into the
  tunnel.
- What this is not: the two solvers share geometry through the STL file, not a common in-memory model, and the part
  is framed on its own in the viewer rather than overlaid on the tunnel model - the two placements are different by
  design. No flow load is transferred to the structure yet; that is the remaining part of milestone 7.

Verified end to end off-screen (`tools/uicheck.py`, 52 checks, both workspaces): the workspace switch, project
creation, mesh generation from the slider, material assignment, FIX BASE, the conditions list, RUN ANALYSIS,
the result fields, the deformation slider and AUTO, HIDE/SHOW, and the return to the tunnel with the layers
restored. A 10 mm cube held at its base and pulled with 500 N over its 100 mm^2 top face gives a surface peak of
25.4 MPa at the support (99th percentile 22.8 MPa) against the 5 MPa nominal section stress, with equilibrium and
Clapeyron checks satisfied - the peak is a support singularity and is labelled as one. A 10 mm 316L cube with
2 MW/m^3 of internal heating and h = 25 W/(m^2 K) reaches 138.9 degC at t = 600 s, against 139 degC from the lumped
estimate (tau = rho cp V / hA = 263 s, steady rise 133 K).

## Known failures and limitations

- Real AI-client tool calls are unverified: the Claude Code login on this machine expired; Codex is not installed.
  `tools/amflow.py` drives the full path over MCP stdio with a scripted client, not with a model.
- No Streamable HTTP transport yet (milestone 7).
- Meshes are voxel-derived only (staircase boundaries); no conforming mesher.
- Static analysis is linear elastic and isotropic; frictionless supports only on axis-aligned faces; bodies interact
  only through shared voxel nodes (no contact); the build plate is not allowed in static analyses.
- All library materials are demonstration values.
- One job worker. Transient jobs still have no checkpoint and resume: a cancelled or interrupted run is lost and has
  to be repeated.
- Thermomechanical coupling is sequential and one-way, without plasticity or element activation, so it is not a
  residual-stress prediction: thermal stresses vanish again when the part returns to a uniform temperature.
- The interface shows the finite-element result surface, not the interior; slices through the solid, and playback of
  the stored times as an animation, are not implemented (step through them with the stored-time slider).
- The tunnel and the analysis share geometry through the STL file only; no flow load is mapped onto the structure.

## Next concrete tasks

1. Finish milestone 4: per-step checkpoints for transient jobs and resume after a cancel or a restart, and layer
   playback images.
2. Milestone 5 (FFF): layer-by-layer element activation driven by a deposition schedule, nozzle and bed temperatures,
   interlayer cooling, bed release, and verification against the energy balance and against published warpage trends
   (labelled as trends, not as calibrated predictions).
3. Milestone 6 (LPBF): inherent-strain and transient elastoplastic workflows.
4. Milestone 7 remainder: map the tunnel's surface pressure onto the structure as a traction condition (one part, one
   placement, both solvers), and the Streamable HTTP transport with its authentication and hardening.
