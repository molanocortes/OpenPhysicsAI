# Gems: things that work but are linked from almost nowhere

Swept every directory of the repository (and the mechanics working copy) on 2026-09-17. Each entry says what it is,
how to run it, and **whether this session ran it**. Nothing here is new work; it is existing work that an agent would
otherwise rebuild because no document points at it.

Entry point: [../../AGENTS.md](../../AGENTS.md) · modules: [MODULES.md](MODULES.md) · history: [HISTORY.md](HISTORY.md).

Build once before the binaries below: `make headless` (≈6 s here) and, for the four developer tools, `make tools`.

## Developer tools that are not in `make test`

| Gem | What it is | Command | Ran it? |
|---|---|---|---|
| `tools/lbmbench.c` | throughput benchmark of the lattice Boltzmann kernel: three collision operators × thread counts, MLUPS and ms/step | `make tools && ./build/lbmbench` | **yes** — 1.77 M cells, 315 MB: 1 thread BGK 31.1 MLUPS, REG 34.7, RR3 33.5; 4 threads BGK 77.5 MLUPS |
| `tools/geomtest.c` | geometry verification and visuals: shapes, STL I/O, voxeliser, performance, slice images | `make tools && ./build/geomtest` | **yes** — "ALL CHECKS PASSED (0 failures)", wrote `build/geomtest_out/*.png` |
| `tools/vistest.c` | the CPU visualisation kernels: derived fields, streamlines, isosurfaces, with benchmarks | `make tools && ./build/vistest` | **yes** — "51 checks passed, 0 failed", wrote `build/vistest_out/*.bmp` |
| `tools/gltest.c` | checks the Cocoa window, the OpenGL 4.1 context, the CoreText atlas and PNG output | `make tools && ./build/gltest` | no — it opens a window; not run in this session |
| `tools/wall_study.sh` | sphere drag against resolution for both wall treatments at Re = 100, Cd averaged over the last 40 % of each run | `tools/wall_study.sh [steps_per_cell] [width_in_D] [D ...]` | no — needs `./navier` and minutes per case |
| `tools/showcase.sh` | renders a gallery of the preset experiments off-screen into `screenshots/` | `tools/showcase.sh [steps]` | no — needs `./navier` |
| `tools/labrun.c` | runs any physics-lab scenario (JSON) to a result file, prints a JSON run record | `make lab && ./build/labrun examples/lab/X.json out.lab` | yes, 2026-09-25, every scenario in examples/lab |
| `tools/labfilm.c` | pictures and films of any lab result: 2D maps with the adaptive mesh drawn, schlieren, 3D points and meshes | `./build/labfilm out.lab --out DIR --field F ...` | yes, the films in docs/media/lab |
| `tools/showcase/lab_films.sh` | re-makes the README's lab films in the shared style from results in `RUNS` (each film names its scenario's result and its view) | `RUNS=/tmp/opai_runs sh tools/showcase/lab_films.sh [name ...]` | yes, docs/media/lab/*.gif |
| `tools/labprobe.c`, `tools/labread.py` | numbers out of a lab result: along a line, at a point, or into NumPy | `./build/labprobe out.lab --field p --line ...` | yes |
| `tools/lab_scenarios.py` | writes the showcase and flag scenarios (shapes from stated dimensions) to examples/lab | `python3 tools/lab_scenarios.py` | yes |
| `tools/flags.py` | seals flag answers, runs entries, scores against the sealed answers, writes the rankings and the podium | `python3 tools/flags.py --help` | yes, sealing; scoring needs the sealed store |
| `tools/horizons.py` | initial conditions for orbits from JPL Horizons and the DE440 masses | `python3 tools/horizons.py solar_system --epoch 2000-01-01 --out X.json` | yes |
| `tools/showcase/mosaic.py` | the films the front page does not already show, one per subject, in one light (under 2 MB) animated mosaic | `python3 tools/showcase/mosaic.py` (about a minute; `--frame` for one still) | yes, docs/media/lab-all.webp |
| `tools/showcase/crop_films.py` | cuts the front page's films to their scene (the app's panel, old captions and colour bars away), frames and palettes untouched | `python3 tools/showcase/crop_films.py [name ...]` | yes, the `*-view.gif` films |
| `tools/showcase/hero_collage.py` | the front page's opening film: seven lab films cropped and played together in one animated bento (WebP) | `python3 tools/showcase/hero_collage.py` (about three minutes; `--frame` for one still) | yes, docs/media/lab-hero.webp |
| `tools/showcase/montage.swift` | one picture from frames of several films, with captions (the README's hero) | `swift tools/showcase/montage.swift OUT.png 3 440 300 "caption|film.gif|frame" ...` | yes |
| `tools/labtest.c`, `tools/gastest.c`, `tools/actest.c`, `tools/imptest.c`, `tools/orbtest.c`, `tools/emtest.c` | the lab's verification suites (also in `make test-fast`) | `make test-lab` | yes |

## App scripts (the in-app terminal, driven off-screen)

The app takes `--headless --size WxH --exec "exec <file>"`, so every one of these runs without a visible window
(`src/main.c:578-584`).

| Gem | What it is | Command | Ran it? |
|---|---|---|---|
| `examples/glider_polar.nav` | angle-of-attack sweep of the sailplane, logs mean Cd/Cl/Cs to `glider_polar.csv` | `./navier --headless --size 1440x900 --exec "exec examples/glider_polar.nav"` | no |
| `examples/karman_street.nav` | Kármán vortex street behind a cylinder, Strouhal number in the FORCES panel | as above | no |
| `examples/roughness_study.nav` | drag of a submarine hull against surface roughness, to `submarine_roughness.csv` | as above | no |
| `examples/my_model.nav` | the template to copy for your own STL (path, orientation, length, fluid) | edit, then `exec examples/my_model.nav` | no |
| `tools/uitest.nav`, `tools/controls_test.nav`, `tools/particles_test.nav` | the interface regressions: every button, the CONTROLS tabs, and the GPU-particle path that once crashed | `make test-ui` runs the first through `tools/uicheck.py` | no |
| `tools/lpbfflow.py` | drives `lpbf_build_run` over MCP end to end: refusals, build, result operations, elem_birth/elem_death round trip, a second session, whole-part cross-check (59 checks, in `make test`) | `python3 tools/lpbfflow.py` | yes, 2026-09-18 |
| `tools/walkthrough.nav` | the acceptance path of the SOLID workspace on the owner's own assembly STL, clicks only: part, unit, mesh, material, hold and load by picking, solve, CHECK MESH, REPORT | `python3 tools/uicheck.py --walkthrough` | by the app session, 95-161 s |
| `tools/leaderboard.py` | scores every challenge in `challenges/` from the result files it names and writes the Markdown boards and `docs/leaderboard/index.html` (no challenge with data is open since 2026-09-27) | `make leaderboard` | yes, 2026-09-19 |
| `tools/agentboard.py` | the agent leaderboard from `challenges/agents/runs/*.json` (run by `make leaderboard`) | `make leaderboard` | yes, 2026-09-19 |
| `tools/matcheck.py` | the materials library check: every value of a measured or published record has a source; stage H of `make check` | `make matcheck` | yes, 2026-09-19 |
| `llms.txt` | the entry page for language models and crawlers, at the repository root | n/a | n/a |
| `mcp/server.json` | draft manifest for the public MCP registry; `CITATION.cff`, `.zenodo.json`, `LICENSE`, `LICENSE-DATA` are the publication files | n/a | n/a |
| `tools/simplewalk.nav` | Simple mode as a first-time user: the sample bracket to a report, clicks only, every screen checked for jargon | `python3 tools/uicheck.py --simplewalk` | by the app session, 35 s |
| `tools/stopwalk.nav` | stopping and starting again by clicks: simulate, STOP, simulate again, change mode while it runs (Advanced lists what is running with its own STOP), type `stop`, start over with another part while one is running; added 2026-09-20 after the owner could not start a second simulation | `python3 tools/uicheck.py --stopwalk` | by the architect, see the commit |
| `tools/run_queue.py` | the unattended runner of the overnight queue: the jobs of `validation/queue.jobs` one after the other, each with its log and timeout, a hung job killed with its children, the laptop kept awake, `build/overnight/SUMMARY.md` after every job; added 2026-09-20 | see [validation/QUEUE.md](../../validation/QUEUE.md) | self-tested (a job, a hung job killed, the job after it), then started for the first night |
| `tools/gcode_read.py` | what a sliced print job says about itself: the process values the slicer embedded and what the toolpath deposits, split into part and support (volume per feature, boxes, layers, time, mean deposition rate); OrcaSlicer, Bambu Studio and PrusaSlicer dialects, arcs included; added 2026-09-20 | `python3 tools/gcode_read.py job.gcode` | yes: reproduces the slicer's own filament total to 0.2 per cent on a 36 MB job |
| `demo/run_gcode_print.py` | simulate the print a slicer prepared: the part's STL, the job's gcode for the process and the support settings, the library's published PLA, tree supports by this simulator's rule; refuses up front when the mesh falls into pieces | `python3 demo/run_gcode_print.py --gcode job.gcode --stl part.stl --mesh 1 --layer 1` | yes, on the owner's drone frame |
| `tools/demo_print_result.nav` | a finished print simulation put on screen in the app and exported as two pictures (the print on its supports, the released part with its warp); `fem follow last` shows the newest stored run of an opened project | `./navier --workspace <folder holding the project> --exec 'exec tools/demo_print_result.nav'` | yes, results in [validation/prints/README.md](../../validation/prints/README.md) |
| `tools/html2pdf.swift` | an HTML file to an A4 PDF with the system's WebKit, for the one-page documents in `docs/release/` | `swiftc -O tools/html2pdf.swift -o build/html2pdf && build/html2pdf in.html out.pdf` | yes |
| `tools/frame_subject.py` | crops a rendered picture so its subject sits in the centre at a given aspect ratio (the application frames a part left of its side panel, so exports come out off centre); needs Pillow and NumPy | `python3 tools/frame_subject.py in.png out.png --aspect 1.6` | yes, the two figures of the one-page paper |
| `tools/agentwalk.nav` | Agent mode: a typed question reaches a result in the window with no button pressed | `python3 tools/uicheck.py --agentwalk` | by the app session, 28 s |
| `tools/agent_standin.py` | a scripted MCP client standing in for an AI tool; calls no model | run by `agentwalk.nav` | yes |
| `samples/`, `src/ctl/print_profiles.json` | Simple mode's sample parts and print presets, each value with its source | read by the app | n/a |
| `demo/verify_supports.py` | supports verification S5, S5b and S6: explicitly meshed support walls against the homogenised block, in the LPBF build and the FFF heat model | `python3 demo/verify_supports.py s5 s5b s6` (about 40 min) | yes, 2026-09-19 |
| `am` (in-app terminal command) | runs any of the typed operations from inside the app; `am` alone lists them (`src/commands.c:1918`) | open `./navier`, press the terminal key, type `am` | no |

## Built-in geometry

| Gem | What it is | Command | Ran it? |
|---|---|---|---|
| `models/*.stl` | ten bodies written by this project's own geometry code: `ahmed`, `airfoil`, `cube`, `cylinder`, `glider`, `plate`, `sphere`, `submarine`, `torus`, `wing` | in the app: `scene car`; headless: `./navier-ctl --embedded call geometry_import '{"path":"models/cube.stl","units":"mm"}'` | partly — used through `capabilities_get`/import paths in the suites, not each one by hand |
| `examples/bracket_comparison/make_geometry.py` | regenerates the example brackets (and the evaluation-set variants) on a 4 mm lattice so every face is exactly representable | `python3 examples/bracket_comparison/make_geometry.py <outdir>` | **yes** — wrote four STLs to `/tmp/qm-gem-geom` (e.g. `bracket_b_chamfer.stl`, 1616 triangles, 53,760 mm³) |
| `examples/bracket_comparison/expected_output/` | the evidence record, report and region previews the packaged example must reproduce **bitwise** | compared by `tools/install_test.sh` | no |

## Thermal module

| Gem | What it is | Command | Ran it? |
|---|---|---|---|
| `Thermal Sim/examples/make_examples.py` | rebuilds the three demonstration studies over MCP exactly as an AI host would, leaving project, hashed specification, energy budget, VTU/CSV | `python3 "Thermal Sim/examples/make_examples.py"` | no |
| `Thermal Sim/examples/make_three_domain.py` | the fluid–thermal–solid demonstration: steel plate under an air channel, LBM flow + conduction + thermal stress | `python3 "Thermal Sim/examples/make_three_domain.py"` | no |
| `Thermal Sim/examples/0*/export/` | finished VTU series, `.pvd` collections, `history.csv` and `summary.json` of those studies — open them in ParaView without running anything | — | no |
| `Thermal Sim/verification/*.md` + `*.log` | the pre-registered criteria and the evidence for stages A–E, including the one criterion that was **not met** (E5c) and the two defects stage D exposed | read | read, not re-run |
| `Thermal Sim/docs/theory.md`, `capability-matrix.md`, `audit.md` | the governing equations and declared unsupported regimes; the per-feature matrix (unavailable / experimental / verified / validated); the audit that found `capabilities_get` lying to clients | read | read, not re-run |

## Release and evaluation (read-only for this branch)

| Gem | What it is | Command | Ran it? |
|---|---|---|---|
| `tools/package.sh` | builds the release tarball in an isolated copy of the sources, regenerates the example's expected output, writes `VERSION` and `SHA256SUMS` | `tools/package.sh [outdir]` | no — it writes to `dist/`, which this branch must not touch |
| `tools/install_test.sh` | installs a tarball into a clean location with a fresh `HOME` and checks 27 things, including a bitwise reproduction of the example | `tools/install_test.sh dist/navier-0.3.0-rc2-macos-arm64.tar.gz` | no — same reason |
| `tools/ccx_crosscheck.py` | exports a finished run as a CalculiX deck and compares displacement, reactions, energy and stress | `python3 tools/ccx_crosscheck.py export <run_dir> <out>` then `compare` | no — needs CalculiX (`~/.navier-eval-tools/`, not part of the repo) |
| `release/evaluation/independent/*.py` | the scripts behind the CalculiX comparison: `navier_runs.py`, `x1_ccx.py`, `x2_ccx.py`, `beam_tests.py`, `retain_navier.py` | see [EVALUATION_RECORD.md](../../release/evaluation/EVALUATION_RECORD.md) | no |
| `release/evaluation/physical/analyze.py` | the pre-registered analysis of a physical comparison; refuses to report anything without real readings, and has a synthetic self-test | `python3 release/evaluation/physical/analyze.py --self-test` | **yes** — "self-test passed"; on the empty template it exits 3 with "validation pending" |
| `release/evaluation/physical/predict.py`, `PROTOCOL.md`, `measurement_template.csv` | the whole physical test protocol: specimens, mounting, load steps, uncertainty budget, and the predictions to compare against | `python3 release/evaluation/physical/predict.py --navier-ctl ./navier-ctl` | no |
| `release/evaluation/user-test-kit/` | a complete independent-user test: quick start, an unfamiliar two-plate task with its geometry, expected artefacts, feedback form, diagnostics, and a facilitator reference with the known pitfall | hand the participant files over; `python3 .../make_task_geometry.py <outdir>` | no |
| `release/evaluation/ai-client/` | the graded real-client sessions: harness, rubric, `extract_facts.py`, `RESULTS.md` (the transcripts themselves were removed before publication) | **do not run** `run_sessions.sh` (it spends the owner's money); read the results | read only |

## Arriving on branch `am-process` (mechanics working copy, read-only here)

From a private working copy, commit `9639919`. Described so it is not rebuilt; see
[MODULES.md](MODULES.md#arriving-on-branch-am-process-mechanics-and-print-simulation).

| Gem | What it is | Command (in that copy) | Ran it? |
|---|---|---|---|
| `src/mech/fffprint.c` | the layer-by-layer FFF print simulation: deposition plan, element activation, incremental thermo-elastic stress, bed release | `./build/mechtest` (`test_fff_print`) | no |
| `demo/run_bridge_print.sh`, `demo/am_print.c`, `demo/make_bridge_geometry.c` | the printed-bridge demonstration that produced the README's print images | `demo/run_bridge_print.sh` | no |
| `demo/build_arm_demo.py`, `demo/build_robot_demo.py`, `demo/arm_demo.nav` | the printed robot-arm demonstration, built over MCP | `python3 demo/build_arm_demo.py` | no |
| `tools/mechbench.c`, `tools/mechflow.py`, `tools/mechtest.c` | the mechanics benchmark, its end-to-end MCP flow and the 235-check suite | `./build/mechtest` | no |
| `tools/showcase/` | the media of the front page, from one capture preset: `showcase.py` (the engine client, the renderer's colour maps read from its source, the framing compositor, the film writer), `print_film.py` (the polymer build replayed from its stored states), `heat_sink.py`, `bracket_static.py`, `topology.py` (each solves its own case) and `flow_film.py` (the tunnel, advanced by exactly ten solver steps per frame); added 2026-09-21 | each script's first lines; provenance in [docs/images/SHOWCASE.md](../images/SHOWCASE.md) | yes: all six; the metal-printing film and the contact sheet were removed on 2026-09-27 for copyright |
| `Mech Sim/docs/assembly-and-study-formats.md`, `backend-decision.md` | the formats and the backend decision record | read | no |
| `demo/print_player/player.html` | a browser viewer for print results — **rejected by the owner**; kept only as history ([HISTORY.md](HISTORY.md)) | — | no |

## Small but easy to miss

- **`tools/embed.c`** turns `ops_schema.json` and `materials.json` into C arrays at build time, which is why every
  binary carries its own contract (`Makefile` → `build/gen/`).
- **`navier-ctl doctor`** is a full self-diagnosis, including a reference solve; it runs at the end of `make test`.
- **`./navier-ctl --embedded call capabilities_get`** answers what the engine can and cannot do, machine-readable.
  **Ran it:** it lists `static_structural`, `transient_thermal`, `thermomechanical`, `conjugate_heat_transfer`,
  `comparison_study`, `fff_process`, `lpbf_process` — the last two as **planned**, so an agent is not misled.
- **`docs/images/`** holds the renders used by the root `README.md`, including the two print sequences and the robot
  link, all produced by this software.
- **`tools/pngcheck.py`** decodes what the C PNG encoder wrote, independently of it — a genuine cross-check, not a
  round trip through the same code.

## Repository presentation

[tools/repo_showcase.nav](../../tools/repo_showcase.nav) captures a completed LPBF build in three clean native-app views.
The [visual tour](../images/SHOWCASE.md) records the setup, field meanings and reproduction command.

[tools/repo_flow.nav](../../tools/repo_flow.nav) photographs glider tracers and Q-criterion surfaces directly in the native fluid renderer.

[tools/repo_films.py](../../tools/repo_films.py) captures short flow, build and thermal sequences from the native app;
[tools/encode_gif.swift](../../tools/encode_gif.swift) encodes the unretouched PNG frames as GIFs using macOS ImageIO.

[tools/make_showcase_aircraft.py](../../tools/make_showcase_aircraft.py) builds the original
[showcase sailplane STL](../../models/showcase-sailplane.stl), checking each closed shell before export.

[tools/labscenetest.c](../../tools/labscenetest.c) checks retained lab geometry, internal section faces and field identity;
its two-cell fixture drives the native GPU section checks in `tools/uicheck.py --lab`.

`tools/showcase/retained_3d.py` computes a small 3D water case, captures the native CPU/GPU comparison and section playback, and encodes the unretouched PNGs with the existing system GIF encoder.

`tools/labvoltest.c` checks full 3D EM export against every legacy slice and verifies reconstructed total/scattered fields across TF/SF faces in an empty domain.

`tools/labwatertest.c` checks kernel coverage against reference particle volume, uniform interior normalization, and constant-field reproduction; writes a small native UI fixture.

`tools/showcase/volume_surface.py` captures continuous water and full-volume radar playback from existing computed results, using the native renderer and system GIF encoder.

`tools/heat3dtest.c` verifies the 3D conduction core against an exact three-axis mode, refinement and heterogeneous energy conservation.

`tools/flow3dtest.c` checks the double-precision 3D periodic flow core against uniform flow and exact Beltrami decay.

`tools/gas3dtest.c` checks uniform conservation, a diagonal 3D sound wave and Sod shock refinement against an exact Riemann solution.

`src/lab/magnet/mag3d.h` solves 3D magnetostatics with MFEM's Nedelec edge elements on a cylindrical grid (any
reluctivity, remanence, and currents given as J or as a current vector potential T), with a Cholesky or LDL^T
factorisation from Accelerate (`src/lab/magnet/spdirect.h`, reusable for any sparse symmetric system); verified by
`tools/mag3dtest.cpp` (toroid, magnetised cylinder and sphere) and `tools/motor3dtest.c`.

