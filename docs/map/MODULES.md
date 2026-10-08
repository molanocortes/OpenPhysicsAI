# Module map

Every directory of the repository: what it is, what to read, what it consumes and provides, how it is tested, and what
state it is in. Derived from the code (headers, `Makefile`, `src/ctl/ops_schema.json`) on 2026-09-17. Where a statement
comes from a document rather than from the code it is marked "(from &lt;file&gt;, not checked)".

Entry point: [../../AGENTS.md](../../AGENTS.md) · request path: [DATAFLOW.md](DATAFLOW.md) ·
what was built when: [HISTORY.md](HISTORY.md) · working things that are easy to miss: [GEMS.md](GEMS.md).

## State vocabulary

The five states of rule 4 in [AGENTS.md](../../AGENTS.md), used in every table below:

| State | Means |
|---|---|
| implemented | the code exists and builds into a binary named in the `Makefile` |
| verified | a suite in `tools/` checks it against a closed form, an analytical solution or an independent code |
| integrated | reachable from the app (`./navier`) or from a higher layer, not only from its own test |
| MCP | an operation in `src/ctl/ops_schema.json` reaches it |
| validated | compared against a physical measurement — **nothing in this repository is validated**, see [../release/LIMITATIONS.md](../release/LIMITATIONS.md) |

## Build targets (from `Makefile`)

| Target | Produces | Sources |
|---|---|---|
| `make` / `all` | `./navier` (app) and `navier-server`, `navier-mcp`, `navier-ctl` | `GUI_OBJ`, `CORE_OBJ` |
| `am` | the three headless binaries | `CORE_SRC` = `src/core` + `src/ctl` + `src/net` + `src/fem` + `src/render` + five files of `src/geom` |
| `headless` | the same plus the 13 test binaries (`TESTS`) | builds without Cocoa/OpenGL |
| `test` | runs 9 C suites, 1 renderer suite, 8 Python flows, `navier-ctl doctor` | see each module's *Tests* row |
| `test-ui` | `tools/femviewtest.c` (result-view contract: birth/death/group visibility, section, playback, on synthetic fields) then `tools/uicheck.py`: clicks every app control off-screen and checks the drawn pixels | needs the app |
| `tools` | `build/gltest`, `build/lbmbench`, `build/geomtest`, `build/vistest` | developer tools, not in `make test` ([GEMS.md](GEMS.md)) |
| `check` | the quality gate added by this branch: warnings, sanitizers, tests, link check, schema coverage, path and commit hygiene | `tools/check.sh` |

Two compilation regimes, deliberate: `CFLAGS` (app, `-O3 -ffast-math`) and `CORE_CFLAGS` (everything numerical,
`-O2 -ffp-contract=off`, no fast-math) so results do not change between compilers. `src/lbm.c` is compiled twice, once
per regime; the two objects never meet in one binary.

---

## `src/core` — dependency-free foundations

| | |
|---|---|
| Purpose | JSON, JSON Schema, units, hashing, PNG, paths, error codes: everything the rest of the project is built on, with no third-party code |
| Read first | [README](../../src/core/README.md), `json.h`, `units.h`, `jschema.h` |
| Key files | `json.c` (strict RFC 8259 parser/DOM/writer), `jschema.c` (draft 2020-12 subset + defaults), `units.c` (unit expressions → SI, dimension checks), `sha256.c`, `png.c` (encoder incl. deflate), `paths.c` (root-confined resolution, atomic writes), `errors.c`, `base64.c`, `sbuf.c` |
| Entry points | `json_parse`, `json_dump`, `jschema_validate`, `unit_to_si`, `sha256_file`, `png_encode`, `path_resolve_new`, `path_within`, `nv_error_json` |
| Consumes | nothing inside the project |
| Provides | every other module; the operation layer's validation and every hash in an evidence record |
| Tests | `tools/coretest.c` (`build/coretest`), plus `tools/pngcheck.py` which decodes `src/core/png.c` output independently |
| State | implemented, verified |
| Neighbours | used by [`src/ctl`](#srcctl--engine-operations-projects-jobs-studies), [`src/net`](#srcnet--control-socket-and-mcp), [`src/fem`](#srcfem--finite-elements-and-time-integration), [`src/geom`](#srcgeom--geometry), [`src/render`](#srcrender--software-renderer) |

## `src/geom` — geometry

| | |
|---|---|
| Purpose | Triangle meshes and STL, surface topology and repair diagnostics, face patches for addressable selections, the voxel hexahedral mesher, the BVH, procedural shapes, and voxelisation for the fluid lattice |
| Read first | [README](../../src/geom/README.md), `hexmesh.h`, `surface.h`, `patches.h` |
| Key files | `mesh.c` (STL in/out, welding, statistics), `surface.c` (indexed surface, shells, self-intersections, thickness), `patches.c` (planar/smooth region growing → addressable patches), `hexmesh.c` (layer-aligned voxel hex8 mesh of a closed STL, boundary faces, staircase statistics; `hexmesh_coarsen_xy`, added 2026-09-19: the 2:1 balanced adaptive mesh, fine at the surface, coarse inside, with its hanging-node constraints, [adaptive-mesh.md](../contracts/adaptive-mesh.md)), `bvh.c`, `voxel.c` (fluid lattice), `shapes.c` (built-in bodies) |
| Entry points | `mesh_load_stl`/`mesh_save_stl`, `surface_build`, `patches_build`, `hexmesh_generate`, `hexmesh_locate`, `bvh_build`, `voxelize_mesh`, `shape_generate` |
| Consumes | `src/core` |
| Provides | meshes and patches to `src/ctl` (geometry, selection, mesh operations) and elements to `src/fem`; `voxel.c`/`shapes.c` also serve the fluid app |
| Tests | `tools/surftest.c`, `tools/meshtest.c` (patches and hex meshes against exact answers), `tools/geomtest.c` (developer target, not in `make test`) |
| State | implemented, verified, integrated, MCP (`geometry_import`, `geometry_place`, `geometry_diagnostics`, `surfaces_list`, `mesh_generate`, `mesh_inspect`) |
| Neighbours | [`src/ctl`](#srcctl--engine-operations-projects-jobs-studies), [`src/fem`](#srcfem--finite-elements-and-time-integration), the app's `src/sim.c` |

## `src/fem` — finite elements and time integration

| | |
|---|---|
| Purpose | The numerical core: static linear elasticity, transient heat conduction with phase change, the shared time-stepping and orchestration machinery, sparse and dense linear algebra, and a steady laminar flow field derived from the lattice Boltzmann kernel |
| Read first | [README](../../src/fem/README.md), `solid.h`, `thermal.h`, `orchestrator.h` |
| Key files | `hex8.c` (8-node hexahedron, full and incompatible-mode stiffness, stress recovery), `solid.c` (constraint analysis, coloured parallel assembly, solve, recovery; since 2026-09-19 hanging-node constraints eliminated in assembly and an optional full 6x6 matrix per material, `HexModel.mat_D`), `tet.c` (added 2026-09-19: TET4 and TET10 elements, loads, stress recovery and a two-level preconditioner, [tet-mesh.md](../contracts/tet-mesh.md)), `hex8_nl.c` and `nlsolid.c` (added 2026-09-20: total-Lagrangian hex8 with enhanced assumed strain, finite-strain J2, Newton with load stepping and buckling by loss of definiteness), `explicit.c` (central difference, lumped mass, hourglass control, energy account; contact in `excontact.h`), all three under [dynamics.md](../contracts/dynamics.md), `topopt.c` (added 2026-09-20: density-method topology optimisation on the voxel grid, compliance with a volume constraint, sensitivity filter, optimality criteria, [topology-optimisation.md](../contracts/topology-optimisation.md)), `sparse.c` (CSR, PCG, sparse Cholesky), `thermal.c` (theta method, Picard iterations, discrete energy balance), `thermal_integrator.c` (transactional fixed/adaptive steps with rollback), `timestep.c` (event schedule, PI controller), `orchestrator.c` (one lifecycle for coupled participants), `krylov.c` (BiCGSTAB + ILU(0) for advection), `flow.c`, `dense.c` |
| Entry points | `solid_solve`, `solid_check_constraints`, `thermal_*`, `thermal_integrator_*`, `orch_create`/`orch_step`, `csr_from_elements`, `bicgstab_solve`, `flow_solve` |
| Consumes | `src/core`, hex meshes from `src/geom`, the LBM kernel `src/lbm.c` (through `flow.c`) |
| Provides | solves to `src/ctl` analysis jobs; nothing here knows about projects, files or MCP |
| Tests | `tools/femtest.c` (against exact and analytical solutions), `tools/thermtest.c`, `tools/tsteptest.c`, `tools/orchtest.c`, `tools/advtest.c`, `tools/tettest.c` (the conforming tetrahedral mesher and the TET4/TET10 elements: patch test, cantilever, Kirsch, fillet, balance; added 2026-09-19), `tools/dyntest.c` (large deformation and explicit dynamics, D1 to D10: stretch, rotation, finite-strain J2, elastica, Euler buckling, bar wave, beam frequency, energy, hourglass; two superseded criteria printed as recorded failures on every run; added 2026-09-20); the structural path is additionally cross-checked against CalculiX in [../../release/evaluation/EVALUATION_REPORT.md](../../release/evaluation/EVALUATION_REPORT.md) §2 |
| State | implemented, verified, integrated, MCP (through `analysis_run`) |
| Neighbours | [`src/ctl`](#srcctl--engine-operations-projects-jobs-studies), [`src/geom`](#srcgeom--geometry) |

## `src/ctl` — engine, operations, projects, jobs, studies

| | |
|---|---|
| Purpose | The headless application core: the typed operation registry and dispatcher, the persisted project, setup and materials, the job queue, the static and transient analyses, results and their exports, rendered views, and comparison studies with the Engineering Evidence Record. 53 files, about 19,800 lines — the largest module |
| Read first | [README](../../src/ctl/README.md), `ops.h`, `engine.h`, `project.h`, `ops_schema.json` |
| Key files | `ops.c` (registry from the embedded schema, validation, concurrency guard, idempotent replay, dispatch), `engine.c` (lifetime, access roots, revisions, journal), `jobs.c` (single worker thread, queue), `project.c` (bodies, patches, setup, `project.json`), `selection.c`, `setup.c`, `matlib.c`, the eleven `ops_*.c` handler files, `static_model.c`/`static_run.c`/`static_io.c`/`static_quantities.c`, `thermal_model.c`/`thermal_run.c`/`thermal_io.c`/`thermal_checkpoint.c`, `contact.c`, `views.c`/`viewrender.c`, `study*.c` (7 files), `cli_local.c` (`navier-ctl` without a server: `study`, `doctor`) |
| Entry points | `ops_registry_init`, `ops_invoke`, `engine_create`, `jobs_submit`, `static_job_run`, `thermal_job_run`, `study_check`, `study_submit`, `cli_local_main` |
| Consumes | `src/core`, `src/geom`, `src/fem`, `src/render` |
| Provides | the 50 operations of `ops_schema.json` (contract 0.6.0), with the 25 of `src/mech/mech_ops_schema.json` merged in at start (75 over MCP, counted 2026-09-27), to `src/net` (socket, MCP), to `src/bin`, and to the app through `src/fembridge.c` |
| Tests | `tools/opstest.c` (operation layer), `tools/ctltest.c` (socket protocol), `tools/amtest.c` (control-to-solver path), `tools/evaltest.c` (evidence, roots, storage regressions), `tools/studyflow.py`, `tools/amflow.py`, `tools/thermflow.py`, `tools/transientflow.py`, `tools/restartflow.py`, `tools/contactflow.py`, `tools/chtflow.py`, `tools/mcptest.py` |
| State | implemented, verified, integrated, MCP |
| Neighbours | [`src/net`](#srcnet--control-socket-and-mcp), [`src/bin`](#srcbin--executables), [`src/fem`](#srcfem--finite-elements-and-time-integration), [`src/geom`](#srcgeom--geometry), [`src/render`](#srcrender--software-renderer), app bridge `src/fembridge.c` |

### Operations by handler file (all 45, from `ops_schema.json` and the `OPS_*_BINDINGS` tables)

| Handler file | Operations |
|---|---|
| `ops_project.c` | `capabilities_get`, `project_create`, `project_open`, `project_save`, `project_inspect` |
| `ops_geometry.c` | `geometry_import`, `geometry_place`, `geometry_diagnostics` |
| `ops_surface.c` | `surfaces_list`, `selection_create`, `selection_list`, `selection_delete`, `selection_preview` |
| `ops_view.c` | `view_render`, `view_pick` |
| `ops_setup.c` | `materials_list`, `material_define`, `material_assign`, `boundary_apply`, `boundary_list`, `boundary_remove` |
| `ops_mesh.c` | `mesh_generate`, `mesh_inspect` |
| `ops_contact.c` | `interface_define`, `interface_list`, `interface_remove`, `interface_preview` |
| `ops_analysis.c` | `setup_validate`, `analysis_run`, `job_status`, `job_cancel`, `job_checkpoint`, `job_pause`, `job_resume`, `job_list` |
| `ops_results.c` | `results_query`, `results_quantities`, `results_probe`, `results_render`, `results_export`, `results_interface` |
| `ops_study.c` | `study_check`, `study_run`, `study_evidence`, `study_replay` |
| `ops_transient.c` | no bindings of its own: it serves the transient side of `analysis_run`, `job_*` and `results_*` |

## `src/net` — control socket and MCP

| | |
|---|---|
| Purpose | The two remote transports over the same operations: a local Unix/TCP control socket with framing, limits, timeouts and authentication, and a transport-independent Model Context Protocol server core |
| Read first | [README](../../src/net/README.md), `ctlserver.h`, `mcp.h` |
| Key files | `ctlserver.c` (JSON-RPC over the socket, dispatch to `ops_invoke`), `ctlclient.c`, `mcp.c` (MCP lifecycle, `tools/list`, `tools/call` over an operation backend), `netutil.c` |
| Entry points | `ctl_server_start`, `ctl_request`, `mcp_session_create`, `mcp_handle_text` |
| Consumes | `src/core`, `src/ctl` (`ops_find`, `ops_invoke`) |
| Provides | remote access for `navier-server`, `navier-mcp`, `navier-ctl` |
| Tests | `tools/ctltest.c` (framing, partial reads, pipelining, malformed requests), `tools/mcptest.py` (protocol level) |
| State | implemented, verified, integrated, MCP |
| Neighbours | [`src/bin`](#srcbin--executables), [`src/ctl`](#srcctl--engine-operations-projects-jobs-studies) |

## `src/render` — software renderer

| | |
|---|---|
| Purpose | Headless images without OpenGL: triangle and line rasterisation with a depth buffer, bitmap text, colour maps and legends, so an agent can *see* geometry, selections and fields |
| Read first | [README](../../src/render/README.md), `swrender.h` |
| Key files | `swrender.c` (525 lines with its header) |
| Entry points | `sw_image_init`, `sw_camera_look`, `sw_camera_preset`, the `sw_draw_*` family |
| Consumes | `src/core` (PNG) |
| Provides | images to `src/ctl/viewrender.c` → `view_render`, `selection_preview`, `results_render`, `interface_preview` |
| Tests | `tools/rendertest.c` + `tools/pngcheck.py` |
| State | implemented, verified, integrated, MCP |
| Neighbours | [`src/ctl`](#srcctl--engine-operations-projects-jobs-studies), [`src/core`](#srccore--dependency-free-foundations) |

## `src/bin` — executables

| | |
|---|---|
| Purpose | The three headless programs. Thin: argument parsing, access roots, and one engine |
| Read first | [README](../../src/bin/README.md), `navier_mcp.c` |
| Key files | `navier_ctl.c` (client + `--embedded` local engine, `study`, `doctor` through `cli_local.c`), `navier_mcp.c` (stdio MCP, embedded engine or bridge to a server), `navier_server.c` (engine behind the control socket) |
| Entry points | `main` in each; `--help` in each |
| Consumes | `src/net`, `src/ctl` |
| Provides | `navier-ctl`, `navier-mcp`, `navier-server` |
| Tests | `tools/ctltest.c`, `tools/mcptest.py`, every `*flow.py`, and `navier-ctl doctor` at the end of `make test` |
| State | implemented, verified, integrated, MCP |
| Neighbours | [`src/net`](#srcnet--control-socket-and-mcp), [`src/ctl`](#srcctl--engine-operations-projects-jobs-studies) |

## `src/` (root files) — the fluid tunnel and the app

| | |
|---|---|
| Purpose | The real-time 3D lattice Boltzmann water tunnel and the native macOS application that hosts both the fluid and the solid workspace |
| Read first | [README](../../src/README.md), `lbm.h`, `sim.h`, `app.h`, `fembridge.h` |
| Key files | `lbm.c` (D3Q19 fused stream-collide, regularised BGK + Smagorinsky LES), `sim.c` (units, placement, solver thread, snapshots, forces), `main.c` (entry point and frame loop), `render.c` (OpenGL 4.1), `ui.c`/`hud.c`/`console.c`/`commands.c` (immediate-mode UI and in-app terminal), `vis_fields.c`/`vis_stream.c`/`vis_iso.c`/`visworker.c` (CPU visualisation), `platform_macos.c` (Cocoa/OpenGL in plain C), `fembridge.c` (**the app calls the same operations agents call**, through `ops_invoke`), `common.c`, `threads.c` |
| Entry points | `./navier` (see `main.c`), the in-app terminal (`commands.c`), `fembridge.c` → `ops_invoke` |
| Consumes | `src/geom` (voxelisation, shapes), `src/ctl` (through `fembridge.c`), `src/core` |
| Provides | the human interface; the fluid solver also feeds `src/fem/flow.c` |
| Tests | `tools/uicheck.py` (`make test-ui`, clicks every control through the real input path), `tools/vistest.c`, `tools/lbmbench.c`, `tools/gltest.c` (developer targets, [GEMS.md](GEMS.md)); the fluid solver has no closed-form suite in `make test` |
| State | implemented, integrated; **fluid: not reachable through MCP**, and its benchmarks live in [../water-tunnel.md](../water-tunnel.md) (from that document, not checked) |
| Neighbours | [`src/ctl`](#srcctl--engine-operations-projects-jobs-studies) through `fembridge.c`, [`src/geom`](#srcgeom--geometry) |

---

## `tools/` — tests, verification flows and developer utilities

| | |
|---|---|
| Purpose | Everything that checks the project, plus the packaging and install scripts and a few benchmarks |
| Key files | C suites: `coretest.c`, `surftest.c`, `meshtest.c`, `femtest.c`, `thermtest.c`, `tsteptest.c`, `orchtest.c`, `advtest.c`, `evaltest.c`, `rendertest.c`, `opstest.c`, `ctltest.c`, `amtest.c`, `tettest.c`, `dyntest.c`, `topotest.c` (the MBB beam and short cantilever of the published 99-line problem, a 3D box against a centred hole). Python flows over MCP: `mcptest.py`, `amflow.py`, `thermflow.py`, `transientflow.py`, `restartflow.py`, `contactflow.py`, `chtflow.py`, `studyflow.py`. Utilities: `pngcheck.py`, `uicheck.py`, `embed.c` (JSON → C array at build time), `package.sh`, `install_test.sh`, `ccx_crosscheck.py` (CalculiX comparison), `linkcheck.py` and `check.sh` (the quality gate). Developer-only: `gltest.c`, `lbmbench.c`, `geomtest.c`, `vistest.c`, `showcase.sh`, `wall_study.sh`, `*.nav` scripts (`walkthrough_tet.nav`, added 2026-09-19, is the structural walkthrough on tetrahedra: `python3 tools/uicheck.py --walkthrough-tet` or the command in `docs/app/tet-comparison.md`); `gallery.nav`, `gallery_bracket.nav`, `gallery_bracket_print.nav`, `gallery_three.nav` (the scripts that render every picture of `docs/images/GALLERY.md` in the app, each under ten minutes) and `topowalk.nav` (optimise, export the STL, import it, run the LPBF build); `tetcompare.py` (voxels against TET10 on the filleted bracket and the assembly, figures into `docs/app/`); `demo_playback.nav` (added 2026-09-18) runs a real thermomechanical analysis on the sample bracket and plays it back sectioned: `./navier --workspace /tmp/navier-playback --exec 'exec tools/demo_playback.nav'`. `Info.plist` is copied into the bundle by `make app` |
| Consumes | the binaries and the C objects |
| Provides | `make test`, `make test-ui`, `make check`, the release package |
| State | implemented; `make test` runs 18 of them, the rest are listed in [GEMS.md](GEMS.md) |
| Neighbours | every module; [ADDING_A_TEST](../release/ADDING_A_TEST.md) explains where a new case goes |

## `docs/` — documentation

| | |
|---|---|
| Purpose | Guides for humans and agents |
| Key files | [analysis.md](../analysis.md) (structural/thermal walk-through), [water-tunnel.md](../water-tunnel.md) (fluid app), [control-protocol.md](../control-protocol.md) (socket), [mcp-clients.md](../mcp-clients.md) (client set-up), `release/` (the eleven release documents, shipped in the package), `map/` (this map), `design/` ([certificate.md](../design/certificate.md)), `images/` (the README renders) |
| State | implemented; `docs/release/*` is the packaged documentation set |
| Neighbours | [AGENTS.md](../../AGENTS.md) links all of them |

## `examples/` — runnable inputs

| | |
|---|---|
| Purpose | Ready-made inputs: the bracket comparison study used by the release example and by `studyflow.py`, and four fluid `.nav` terminal scripts |
| Key files | `bracket_comparison/` (`study.json`, `make_geometry.py`, `geometry/*.stl`, `expected_output/` with the evidence record and previews, `README.md`), `glider_polar.nav`, `karman_street.nav`, `roughness_study.nav`, `my_model.nav` |
| Entry points | `./navier-ctl study run examples/bracket_comparison/study.json --dir <out>`; in the app: `exec examples/glider_polar.nav` |
| Tests | the bracket example is re-run and compared bitwise by `tools/install_test.sh` |
| State | implemented, verified (the study), integrated; the `.nav` scripts are app-only and are listed in [GEMS.md](GEMS.md) |
| Neighbours | [`release/`](#release--packaging-readiness-and-the-independent-evaluation), [`src/ctl`](#srcctl--engine-operations-projects-jobs-studies) |

## `models/` — built-in geometry

| | |
|---|---|
| Purpose | Ten STL bodies used by the fluid app's scenes and by tests: `ahmed.stl`, `airfoil.stl`, `cube.stl`, `cylinder.stl`, `glider.stl`, `plate.stl`, `sphere.stl`, `submarine.stl`, `torus.stl`, `wing.stl` |
| Origin | written by this project's own geometry code (STL header "NAVIER binary STL"), see [../../release/DISTRIBUTION_INVENTORY.md](../../release/DISTRIBUTION_INVENTORY.md) §2 |
| Entry points | in the app: `scene car`, `scene glider`, …; as input to any analysis |
| State | implemented, integrated; listed in [GEMS.md](GEMS.md) because nothing but the app's scene table references them |
| Neighbours | [`src/`](#src-root-files--the-fluid-tunnel-and-the-app), [`src/geom`](#srcgeom--geometry) |

## `release/` — packaging, readiness and the independent evaluation

| | |
|---|---|
| Purpose | The release candidate's evidence: what was tested, what is open, and the independent evaluation of rc1 that produced rc2 |
| Key files | [READINESS.md](../../release/READINESS.md) (rc1 assessment), [GAP_ASSESSMENT.md](../../release/GAP_ASSESSMENT.md), [DISTRIBUTION_INVENTORY.md](../../release/DISTRIBUTION_INVENTORY.md) (inventory + 8 open licensing decisions), `reference_cases/CRITERIA.md`, `evaluation/` (the whole independent evaluation: [EVALUATION_REPORT.md](../../release/evaluation/EVALUATION_REPORT.md), `EVALUATION_RECORD.md`, `CRITERIA-independent.md`, `independent/` CalculiX decks and scripts, `ai-client/` graded sessions, `user-test-kit/`, `physical/`, `baseline-rc1/`) |
| Entry points | `tools/package.sh` builds the package; `tools/install_test.sh <tarball>` installs and checks it |
| Tests | `install_test.sh` (27 checks, from that script's own output, not re-run here) |
| State | implemented; the AI-client evaluation is graded "not dependable yet" ([EVALUATION_REPORT.md](../../release/evaluation/EVALUATION_REPORT.md) §4) |
| Neighbours | [`tools/`](#tools--tests-verification-flows-and-developer-utilities), [`examples/`](#examples--runnable-inputs), [certificate.md](../design/certificate.md) builds on the evidence record |
| Note | **This branch does not modify `release/evaluation/**` or `dist/`.** |

## `Thermal Sim/` — the thermal module's own record

| | |
|---|---|
| Purpose | Not code: the thermal work's theory, capability matrix, verification logs and demonstration studies. The code lives in `src/fem/thermal*.c` and `src/ctl/thermal_*.c` |
| Key files | [README.md](../../Thermal%20Sim/README.md), [STATUS.md](../../Thermal%20Sim/STATUS.md) (checkpoint), `docs/theory.md`, `docs/capability-matrix.md`, `docs/audit.md`, `docs/baseline/`, `verification/`, `examples/` |
| State | (from `Thermal Sim/STATUS.md`, not checked) implemented and verified; the capability matrix marks each feature unavailable / experimental / verified / validated |
| Neighbours | [`src/fem`](#srcfem--finite-elements-and-time-integration), [`src/ctl`](#srcctl--engine-operations-projects-jobs-studies) |

## `dist/`, `NAVIER.app/`, `build/`

| | |
|---|---|
| `dist/` | the rc1 and rc2 release packages with their `.sha256`. **Not touched by this branch**; rc1 must stay byte-identical |
| `NAVIER.app/` | the macOS bundle produced by `make app`; reported stale against `./navier` (not checked here) |
| `build/` | build output: `build/obj`, `build/gen` (embedded schema and materials), the test binaries, and `build/asan` from `make check` |

---

## `src/mech/` and `src/ctl/print_analysis.*`: mechanics and print simulation (merged into main 2026-09-18)

Not in `main`. Read from a private working copy (branch `rebaseline-release`, last commit `9639919`) and from the
untracked port in the `am-process` worktree, on 2026-09-17. Described here so that it is not rebuilt from scratch.

| | |
|---|---|
| Purpose | Rigid multibody dynamics with joints, contact and friction; actuators, controllers and sensors; flexible bodies by Craig–Bampton reduction; modal and transient structural response; orthotropic printed materials; load transfer to the FEM mesh; and a layer-by-layer FFF print simulation |
| Key files | `src/mech/` (41 files, ~18,600 lines): `multibody.c`, `contact.c`, `assembly.c`, `mechsim.c`, `mechspec.c`, `massprops.c`, `flexbody.c`, `modal.c`, `ortho.c`, `loads.c`, `mech_ops.c` + `mech_ops_schema.json` (its own operations), `mmath.c`, `urdf.c`/`xml.c`, **`fffprint.c`/`fffprint.h`** (deposition plan, activation, incremental thermo-elastic stress, bed release; since 2026-09-19 homogenised supports in its heat model and stress history, removed after the release), **`support_cell.c`/`support_cell.h`** (added 2026-09-19: block, thin wall, cone, tree and lattice support patterns, their unit cells solved for the orthotropic stiffness by periodic homogenisation, the conduction and the surface per volume, [supports.md](../contracts/supports.md)), `lpbf.c` (the inherent-strain build; anisotropic support elements by `lpbf_set_elem_D`, tear-off forces by `lpbf_element_forces`) |
| Entry points | `mech_ops_schema.json` operations; `tools/mechtest.c`, `tools/mechflow.py`, `tools/printflow.py` (independent MCP client: a small PLA wall printed end to end, refusals, reopen, energy balance and release equilibrium), `tools/mechbench.c`; `demo/run_bridge_print.sh`, `demo/am_print.c`, `demo/build_arm_demo.py` |
| Consumes | the verified core unchanged: `src/fem/thermal.c` element activation, `src/fem/solid.c` per-element modulus scale and initial strain, `src/geom/hexmesh.c` |
| Tests | `tools/mechtest.c`: "235 passed, 0 failed" including `test_fff_print` (8 checks) (from the commit message of `9639919`, **not re-run here**) |
| State | implemented, verified (`mechtest` 237, `mechflow` 214, `printflow` 82, all passing on main 2026-09-18), integrated, reachable through MCP (`mech_*` operations, 22, contract navier-mech 0.6.0, and the `fff_print` job kind); results carry `elem_birth` for the app's growing-part display; NOT validated against any measured print |
| Superseded | `demo/print_player/player.html`, a browser viewer for print results, rejected by the owner on 2026-09-17: results belong in the native app ([VISION.md](../../VISION.md), [HISTORY.md](HISTORY.md)) |
| Neighbours | [`src/fem`](#srcfem--finite-elements-and-time-integration), [`src/ctl`](#srcctl--engine-operations-projects-jobs-studies) |

## `src/lab` — the physics lab (added 2026-09-25)

| | |
|---|---|
| Purpose | new physics domains, each a plain C library with no UI, writing one result format: compressible flow with adaptive mesh refinement and cut-cell bodies (`src/lab/gas`), room acoustics (`src/lab/acoustic`), impact of metals on a mesh and, for hypervelocity, with particles (`src/lab/impact`), orbits (`src/lab/orbit`), electromagnetism (`src/lab/em`), flow past bodies in 2D and on the GPU in 3D (`src/lab/flow`), heat and flow with conjugate heat transfer (`src/lab/heat`), magnetostatics for motors and shields in 2D and 3D, eddy currents (`src/lab/magnet`), light near a black hole (`src/lab/relativity`), water with a free surface (`src/lab/water`), brittle fracture by peridynamics (`src/lab/fracture`), thin sheets (`src/lab/sheet`), metal melting with Marangoni flow (`src/lab/melt`), fluid-structure interaction (`src/lab/fsi`), fire and rooms by low-Mach flow (`src/lab/fire`), the multigrid Poisson solver (`src/lab/mg`), batteries (`src/lab/battery`). What each computes, how it was verified and what to read first: [PHYSICS.md](PHYSICS.md) |
| Read first | [README](../../src/lab/README.md), `labio.h`, `lab_domains.h`, then each domain's header |
| Built by | `src/lab/lab.mk`, joined to `CORE_SRC`; `make lab` builds `build/labrun`, `build/labfilm`, `build/labprobe` |
| Tests | `tools/labtest.c` (format), `tools/gastest.c` (G1 to G8), `tools/actest.c` (A1 to A4), `tools/imptest.c` (I1 to I5), `tools/orbtest.c` (O1 to O4), `tools/emtest.c` (E1 to E3), `tools/flowtest.c` (F1, F2), `tools/sphtest.c` (S1 to S3), `tools/hftest.c` (H1 to H5), `tools/mgtest.c` (M1 to M7), `tools/rttest.c` (R1 to R4), `tools/wtest.c` (W0 to W2; W0 alone in `make test-fast`, the rest take minutes); all in `make test-fast` |
| State | implemented, verified against closed forms and balances; reachable from the command line (`build/labrun`) and from the app (`lab open`, `lab run`: `src/labapp.c` with retained 3D geometry in `src/lab/labscene.c` and GPU rendering in `src/labgpu.c`; other results use `src/lab/labview.c`, which `tools/labfilm.c` also uses), not yet from MCP |
| Docs | [docs/lab/README.md](../lab/README.md) |

### Printing and plugin verification additions (2026-10-03)

`tools/mcp_http.py` adapts the native stdio MCP server to bounded loopback Streamable HTTP; it does not compute physics.
`tools/mcp_http_test.py` is an independent HTTP client checking lifecycle, state, annotations, schemas and transport bounds.
Both are described in [the client guide](../mcp-clients.md). `make test-plugin` runs both transport suites.
`make test-printing` runs analytic printing checks, mesh checks and the complete FDM/LPBF MCP workflows.
`tools/demo_printing.py --capture --film` computes a small open-cell FDM/LPBF example, waits for both real jobs,
and records native views and replay frames with their inputs and numerical summaries. It uses demonstration materials
and explicitly inferred process choices, not measurements. See [the application guide](../analysis.md#in-the-application).

`tools/printing_refinement.py` compares three fixed-setup mesh levels and three FDM thermal substep counts through
the native MCP operations. It records criteria and amendments explicitly, including an invalid zero-net-reaction
normalization found on its first run. The study is numerical self-convergence, not experimental validation.
