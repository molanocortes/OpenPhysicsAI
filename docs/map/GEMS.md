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
| `tools/wall_study.sh` | sphere drag against resolution for both wall treatments at Re = 100, Cd averaged over the last 40 % of each run | `tools/wall_study.sh [steps_per_cell] [width_in_D] [D ...]` | no — needs `./openphysics` and minutes per case |
| `tools/showcase.sh` | renders a gallery of the preset experiments off-screen into `screenshots/` | `tools/showcase.sh [steps]` | no — needs `./openphysics` |
| `tools/labrun.c` | runs any physics-lab scenario (JSON) to a result file, prints a JSON run record | `make lab && ./build/labrun examples/lab/X.json out.lab` | yes, 2026-09-25, every scenario in examples/lab |
| `tools/labfilm.c` | pictures and films of any lab result: 2D maps with the adaptive mesh drawn, schlieren, 3D points and meshes | `./build/labfilm out.lab --out DIR --field F ...` | yes, the films in docs/media/lab |
| `tools/render/` (`lab2ply.c`, `beauty.py`, `film.sh`) | photoreal films of lab results in Blender's Cycles: lab2ply.c turns a `.lab` result into PLY surfaces with the field per vertex, beauty.py lights and films them, film.sh encodes the MP4 and the README GIF with a caption ([README](../../tools/render/README.md)) | `bash tools/render/film.sh RESULT.lab OUT` (needs Blender and ffmpeg) | yes, the helical gearbox still and film, 2026-10-21 to 24 |
| `tools/render/` generative design (`gendesign_film.sh`, `topofilm.py`, `topoencode.py`, with `tools/gendesign.c`) | the topology optimiser's titanium bracket, from the run's density snapshots to closed, checked surfaces, a Blender film and its encodes ([README](../../tools/render/README.md#generative-design)) | `bash tools/render/gendesign_film.sh` | yes, the README's generative-design film, 2026-10-22 to 23 |
| `tools/render/` metal printing (`printbuild.py`, `print_amb2022_bridge.json`, `nvt2ply.c`, `nvt2ply_check.py`, `printfilm.py`, `printfilm.sh`) | any part built layer by layer through `lpbf_build_run`, exported per stored time as PLY (checked against the `.nvt` to float32 rounding) and filmed growing, cut and springing back ([README](../../tools/render/README.md#the-metal-printing-film)) | `python3 tools/render/printbuild.py ...`, then `bash tools/render/printfilm.sh` | yes, NIST's AM-Bench 2022 bridge, 2026-10-22 to 24 |
| `tools/showcase/lab_films.sh` | re-makes the README's lab films in the shared style from results in `RUNS` (each film names its scenario's result and its view) | `RUNS=/tmp/opai_runs sh tools/showcase/lab_films.sh [name ...]` | yes, docs/media/lab/*.gif |
| `tools/labprobe.c`, `tools/labread.py` | numbers out of a lab result: along a line, at a point, or into NumPy | `./build/labprobe out.lab --field p --line ...` | yes |
| `tools/lab_scenarios.py` | writes the showcase and flag scenarios (shapes from stated dimensions) to examples/lab | `python3 tools/lab_scenarios.py` | yes |
| `tools/flags.py` | seals flag answers, runs entries, scores against the sealed answers, writes the rankings and the podium | `python3 tools/flags.py --help` | yes, sealing; scoring needs the sealed store |
| `tools/horizons.py` | initial conditions for orbits from JPL Horizons and the DE440 masses | `python3 tools/horizons.py solar_system --epoch 2000-01-01 --out X.json` | yes |
| `tools/showcase/mosaic.py` | the films the front page does not already show, one per subject, in one light (under 2 MB) animated mosaic | `python3 tools/showcase/mosaic.py` (about a minute; `--frame` for one still) | yes, docs/media/lab-all.webp |
| `tools/showcase/gallery_thumbs.py` | the home screen's pictures: each library scenario computed and filmed in the app's studio look, one 640 x 480 still in docs/media/thumbs (a scenario without one is not shown in the gallery) | `python3 tools/showcase/gallery_thumbs.py [names] [--timeout s]` (minutes per scenario; long ones detached) | yes, docs/media/thumbs/*.jpg |
| `tools/labxr.c` | a lab result's frame as the headset's scene (GLB): what the app's lab draws, every vertex's value and part, the volume as half floats ([src/lab/labxr.h](../../src/lab/labxr.h), [docs/xr.md](../xr.md)) | `./build/labxr out.lab --info`, `./build/labxr out.lab --frame N -o frame.glb`, `./build/labxr out.lab --print-view` (the view the app takes for it) | yes, 2026-10-07: every result in ~/OpenPhysicsAI-Projects/lab |
| `tools/xr/xr_bridge.py` | the headset link's Mac side: serves the Quest page, the app's state and finite-element scene, lab frames drawn by `build/labxr` on demand, the AI's answers spoken by the Mac's voice; turns spoken questions into text and passes questions with what the headset points at, view commands and Allow/Deny answers ([docs/xr.md](../xr.md)) | `python3 tools/xr/xr_bridge.py [--check-voice] [--voice NAME]` | yes, 2026-10-07, in `make test-xr` (no AI is started there); not yet inside the headset |
| `tools/xr/commands.json` | the view commands the headset may give the app (and the ones refused), which the bridge and the app (`xrlink_command_allowed`, src/core/xrlink.c) are both tested against | read by `tools/xr/xr_test.py` | yes, 2026-10-07: both rules agree on every line |
| `tools/xr/xr_check.py` | checks a headset scene: valid glTF 2.0, every primitive's attributes one per vertex, the volume's buffers, the extras; for a part, true size against its STL | `python3 tools/xr/xr_check.py scene.glb [--stl part.stl]` | yes, 2026-10-07: Thermal Corolla, and lab frames of the flag, hall, motor and laser tracks |
| `tools/xr/xr_test.py` (with `tools/xr/pagetest.mjs`) | the headset link end to end without a headset: its own headless app and bridges, frames and commands, questions and spoken answers, a finite-element scene, and the page in headless Chrome through the DevTools protocol with real mouse events and synthetic controller events; criteria in its header | `make test-xr` (about a minute; the page part needs Node 22 and Chrome) | yes, 2026-10-07 |
| `tools/xr/listen.swift` | a spoken question as text with the Mac's own speech recognition (`build/openphysics-listen`); `tools/xr/listen-Info.plist` gives it its permission text | `make build/openphysics-listen` | built; not yet run (macOS permission pending) |
| `tools/xr/web/app.js` (with `tools/xr/web/index.html`) | the headset page: WebXR passthrough AR and VR, the result on a table or a real surface, true size, carry, two-handed scale, the card, pointing to read values, pins, hold-to-talk; on a computer, mouse and keys; three.js r165 vendored as `tools/xr/web/vendor/three.module.js`, `tools/xr/web/vendor/GLTFLoader.js` and `tools/xr/web/utils/BufferGeometryUtils.js` (MIT) | opened in the Quest at `http://openphysics-xr.localhost:8080` | yes on a computer, 2026-10-07 (`make test-xr`); not yet in the headset |
| `tools/xr/web/scene.js`, `tools/xr/web/volume.js` | the page's scenes: frames streamed and played in step with the app, particles as lit spheres, and the app's volume shader (src/labvolume_shader.h) in WebGL2 with the same ray read on the CPU for a probe | imported by app.js | yes, 2026-10-07, by eye against the app's pictures and in `make test-xr` |
| `tools/xr/voice.swift` (`build/openphysics-voice`) | text to speech in about 50 ms a sentence: one macOS synthesizer kept warm, one JSON line in and one out per sentence, the best installed voice for the language (Premium, Enhanced, then Siri) | the headset bridge's sentence-by-sentence speech; any program that must talk back fast | yes, 2026-10-07: `tools/xr/talk_test.py` criterion 8 |
| `tools/xr/listen.swift` with `tools/xr/listen-app-Info.plist` (`build/OpenPhysicsAIListen.app`) | speech into words with the Mac's own recogniser, as a background app with its own name, so that macOS asks once for speech recognition whoever started the caller; serves a folder (NAME.req in, NAME.json out), recogniser kept loaded, ends with its parent | the headset bridge's `/voice`; any program that must recognise speech from a terminal | partly, 2026-10-07: the plumbing in its stand-in mode (`talk_test.py` criterion 11); real recognition awaits the one OK in macOS's dialog |
| `tools/voice/mic.swift` with `tools/voice/mic-Info.plist` (`build/openphysics-mic`, `make voice`) | a question spoken at the Mac for the agentic bar (`src/voice.c`): the microphone until the window stops it, turned into words by the Mac's own speech recognition, on the device when the language allows | the microphone button of Agentic mode | partly, 2026-10-07: it builds and the bar's path runs with a stand-in recogniser (`tools/minimal_window.py`); the real microphone and its permission are the owner's check |
| `tools/xr/live.py` | the provider's voice for an agent: GPT-Live sessions (WebRTC offer traded server-side, a sideband WebSocket attached), client delegation answered by your own agent, quiet progress, "Done" for commands done locally; plus a standard-library WebSocket client (RFC 6455) with TLS against the system's certificates | giving any agent OpenAI's full-duplex voice without the key leaving the Mac | yes, 2026-10-08: `talk_test.py` criteria 13 to 16 against `tools/xr/live_standin.py`; not yet against OpenAI |
| `tools/xr/web/live.js` | the browser side of it: the microphone to GPT-Live over WebRTC, the voice played back, mute, an instant quiet for "stop" | the headset page | by hand, 2026-10-08, with a synthetic microphone and the stand-in |
| `tools/xr/live_standin.py` | a stand-in for OpenAI's live sessions: session creation (answering with a mirror of the browser's own offer, which a browser accepts), the sideband WebSocket, events driven by a test, everything logged | testing a GPT-Live integration with no key, network or cost | yes, 2026-10-08 |
| `tools/xr/web/vad.js` | when someone is speaking: 20 ms energy frames against a noise floor that follows the room, a sentence after 80 ms 12 dB above it and ended by 0.7 s of quiet, 0.3 s of lead-in kept; while the AI speaks, its own voice coming back must be beaten by 18 dB for 240 ms. Pure: frames in, events out | any page that must listen without a button | yes, 2026-10-07: synthetic sentences, clicks, noise and echo in `make test-xr` (part 5, criterion 10) |
| `tools/xr/web/dock.js` | the headset's minimal interface in 3D: the legend, the latest words fading 8 s after the answer, one round button (microphone, listening ring, stop), Manual | the headset page; the shape of any spatial agent interface | yes, 2026-10-07: placement, a controller press, the fade, in `make test-xr` part 7 |
| `lab_refresh` (src/lab/labio.h) | a lab result read while it is still written: the frames appended since it was opened, complete ones only | showing any run as it computes (the app's follower, `src/labapp.c`) | yes, 2026-10-08: `tools/labtest.c` L4 |
| `labapp_run_alive` (src/labapp.h) | whether a run's process is computing: alive, not a zombie, and the one that wrote its pid file (process start time against the file's) | any "is it still running" from a pid file on macOS | yes, 2026-10-08: `tools/labapptest.c`, `talk_test.py` criterion 18 |
| `build/openphysics-cad` (`src/cad/`, [docs/cad.md](../cad.md)) | exact parts from a feature list (Open CASCADE: primitives, sketches, booleans, patterns, fillets, chamfers, shells; selectors, never indices; every step measured) and a part's skin and lattice core (Manifold level sets: gyroid, Schwarz P, diamond, BCC, octet; a wall or a solid fraction; always closed and manifold, cross-sections on request, enclosed voids counted and drained) and whether a part can be printed (`openphysics-cad check`: overhangs and their support along any build direction and the six axes, thin walls, enclosed voids with their ways out, for an ISO/ASTM 52900 process and the caller's limits); and a core's cell homogenised (`openphysics-cad cell`: its stiffness over the solid's, through `support_cell_stiffness`, the support module's periodic solver opened to any voxel cell); openphysics-mcp `cad_part`, `cad_lattice`, `cad_check` and `cad_cell` | an agent making a printed part, a lattice core for any STEP or STL, or asking whether anything printable is | yes, 2026-10-08: `make test-cad` (with `tools/cadprinttest.py`) and `tools/cadagenttest.py` (GOALS.md G23) |
| `openphysics-mcp` `lab_change` | a simulation changed in one call: the scenario of the result shown (from its own header) with only the values named by path changed, each checked to be there and of the same kind, provenance kept (title, source notes, `changed_from`), saved as NAME_v2 and run ([docs/mcp-clients.md](../mcp-clients.md)) | "make the wind twice as strong" without reading the solver's documentation or writing the scenario out again | yes, 2026-10-08: `tools/xr/change_test.py`; with Haiku 4.5 the run started 4.2 to 4.6 s after the question instead of 14.4 s |
| `tools/remote.py` ([docs/compute-nodes.md](../compute-nodes.md)) | work sent from the Mac to compute nodes on the local network: status, sync of the tracked sources, build, detached jobs with logs, fetch of results; Windows nodes run Linux in WSL2 behind Windows' OpenSSH, kept alive between connections | any run too long or too large for the laptop | yes, 2026-10-08: every command against the first node, a job's file fetched back |
| `tools/monitor/` ([docs/compute-nodes.md](../compute-nodes.md#the-monitor)) | a recorder on each node (`node_sampler.py`, a system service in its Linux: every thread's busy share, floating-point operations, instructions and cycles from the processor's own counters, jobs) and a page on the Mac (`monitor.py`, `web/index.html`, `app.js`, `style.css`) that follows it live, keeps the history across the Mac's sleep, draws every thread over any range from five minutes to everything recorded (`/api/threads`), samples the Mac itself (cores, memory pressure, swap, away or in use) and shows the machines' benchmark; `countertest.py`, `loadtest.c` and `powertest.sh` check the counters and the power supply, `monitortest.py` the history and the Mac's readings | knowing whether the machines are full or idle, and what they computed overnight or last week | yes, 2026-10-08: the counters against a loop with a known count (ratio 1.00000, scalar and AVX2-FMA), the page against the first node live; 2026-10-09: `monitortest.py` (columns against hand-worked values, the old endpoints identical on the real history) and the page by real clicks at desktop and phone width |
| `tools/xr/loop_bench.py` | the whole spoken loop timed with a real model: a sentence posted to a bridge of the bench's own, the app, openphysics-mcp and the run real; the first words, the run started, the run shown while it computes, the answer done, every new frame | measuring "say it, see it change" end to end without a headset | yes, 2026-10-08: two runs in [docs/xr.md](../xr.md) (run started after 4.4 and 4.7 s) |
| `tools/xr/claude_standin.py` | a stand-in for Claude Code's session protocol (`-p --input-format stream-json --include-partial-messages`): scripted answers streamed word by word, interrupt and set_model honoured, everything logged; no model | testing anything that drives Claude Code as a conversation, for nothing | yes, 2026-10-07: the whole of `tools/xr/talk_test.py` |
| `openphysics-mcp --lean` | the engine's 75 operations behind three tools (`engine_ops`, `engine_op`, `engine_call`): 16 tools in about 5 400 tokens instead of 88 in 85 500 | a fast agent session that still reaches every operation | yes, 2026-10-07: listed and called in a session; the app's conversations use it |
| `tools/xr/web/test/fakexr.js` | a stand-in WebXR runtime for the page test (`?fakexr=1`): a session, two eyes, two controllers with gamepads, select and squeeze, hit testing, driven through `window.fakexr`; it emulates no Quest, it makes the headset code run | loaded by app.js with `?fakexr=1` | yes, 2026-10-07: every headset code path, in `make test-xr` part 7 |
| `tools/xr/web/commands.js` | plain view commands said or typed in the headset ("next frame", "show the temperature", "cut along z", "true size"), done at once; anything else goes to the AI | imported by app.js | yes, 2026-10-07: its sample sentences in `make test-xr` |
| `tools/xr/web/card.js`, `tools/xr/web/probe.js`, `tools/xr/web/link.js`, `tools/xr/web/voice.js` | the card beside the result (studio look), values read under the pointer and pinned (what the AI is told), the line to the bridge, the voice (16 kHz recording with a windowed-sinc fallback, answers played) | imported by app.js | yes, 2026-10-07, in `make test-xr` |
| `tools/showcase/crop_films.py` | cuts the front page's films to their scene (the app's panel, old captions and colour bars away), frames and palettes untouched | `python3 tools/showcase/crop_films.py [name ...]` | yes, the `*-view.gif` films |
| `tools/showcase/hero_collage.py` | the front page's opening film: seven lab films cropped and played together in one animated bento (WebP) | `python3 tools/showcase/hero_collage.py` (about three minutes; `--frame` for one still) | yes, docs/media/lab-hero.webp |
| `tools/showcase/montage.swift` | one picture from frames of several films, with captions (the README's hero) | `swift tools/showcase/montage.swift OUT.png 3 440 300 "caption|film.gif|frame" ...` | yes |
| `tools/labtest.c`, `tools/gastest.c`, `tools/actest.c`, `tools/imptest.c`, `tools/orbtest.c`, `tools/emtest.c` | the lab's verification suites (also in `make test-fast`) | `make test-lab` | yes |
| `tools/hole_plate.py` | a plate with a central hole in tension on TET10 through the MCP server, Poisson's ratio 0: the largest von Mises stress anywhere against Heywood's finite-width concentration on the net section (GOALS.md G20 step 5 (c), [tet-mesh contract section 12](../contracts/tet-mesh.md#12-amendment-2026-10-08-sharp-edges-and-corners-kept-goalsmd-g20-step-5)) | `make openphysics-mcp && python3 tools/hole_plate.py [--size MM]` (about two minutes at 0.5 mm) | yes, 2026-10-08: +0.51 % (criterion 3 %), 323160 elements |
| `tools/plain_setup.py` | G17 step 6 end to end without an AI: pointing at faces in the Agentic window, the held and loaded colours, the `setup_confirm` card (Run it, Not right) and the answer's sentence, with a stand-in agent through openphysics-mcp | `make openphysics openphysics-mcp && python3 tools/plain_setup.py` | yes, 2026-10-08: 11 passed |
| `tools/minimal_window.py` | G18's minimal window by real clicks off-screen: the controls each screen registers, Open and Run, the microphone with a stand-in recogniser, a tool request's Allow card, the gallery | `make openphysics && python3 tools/minimal_window.py` | yes, 2026-10-07 |
| `tools/g20melt.c` (`build/g20melt`) | one melt-domain scenario run without frames, its solidified track read as a metallographic cross-section (`melt_track_section`, to a fraction of a cell), the pool's length, the energy balance and the cost as one JSON line ([docs/lab/melt.md](../lab/melt.md)) | `make build/g20melt && ./build/g20melt CASE.json` | yes, 2026-10-08 (GOALS.md G20 step 2) |
| `tools/g20_melt.py` | GOALS.md G20 step 2: NIST AM-Bench single tracks predicted blind (the refinement study, the absorptivity on the conduction-mode calibration track, the frozen predictions, the score), set-ups read from `~/.openphysicsai/g20/metal/` | `python3 tools/g20_melt.py study` (then `calibrate`, `predict`, `score`) | yes, 2026-10-08 |
| `src/mech/breakest.c`, `tools/breaktest.c` | the break estimate of a printed plastic: a section that flows at its tensile strength in tension and at c times it in compression, its moment-axial interaction, the critical-distance formulas (GOALS.md G20 step 5); B1 to B6 against closed forms | `make build/breaktest && ./build/breaktest` | yes, 2026-10-08: 9 passed |
| `tools/g20_strength.py` (with `build/libbreakest.dylib`) | GOALS.md G20 step 5: the break estimate calibrated on the calibration studies only and the test groups predicted blind, set-ups read from `~/.openphysicsai/g20/fff-strength/` | `make build/libbreakest.dylib && python3 tools/g20_strength.py calibrate` | yes, 2026-10-08 (calibration) |
| `tools/g20_warp.py` | GOALS.md G20 step 4: published FFF warp cases through `mech_print_run` over MCP (the specimens' outlines, the materials as the step states them, the released shape's end lift, MAT24's grating strain, TRO22's camber); one relaxation time at the glass transition per study, fixed on its calibration case, then the test cases frozen with their SHA-256 | `python3 tools/g20_warp.py calibrate` (then `predict`, `predict-tro22`, `score`) | yes, 2026-10-08 |
| `tools/g20_bridge.py` | GOALS.md G20 step 3: the AM-Bench bridges through `lpbf_build_run` over MCP, the attached base found on the geometry and put at x = 0 (NIST's 2018 STL mirrored, the 2022 CAD as drawn), the twelve legs cut, each ridge read as its measurement reads it; one isotropic inherent strain from one unit-strain build, its linearity checked by a second build | `python3 tools/g20_bridge.py geometry` (then `smoke`, `calibrate`, `predict`, `score`) | yes, 2026-10-08 |

## App scripts (the in-app terminal, driven off-screen)

The app takes `--headless --size WxH --exec "exec <file>"`, so every one of these runs without a visible window
(`src/main.c:578-584`).

| Gem | What it is | Command | Ran it? |
|---|---|---|---|
| `examples/glider_polar.nav` | angle-of-attack sweep of the sailplane, logs mean Cd/Cl/Cs to `glider_polar.csv` | `./openphysics --headless --size 1440x900 --exec "exec examples/glider_polar.nav"` | no |
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
| `tools/demo_print_result.nav` | a finished print simulation put on screen in the app and exported as two pictures (the print on its supports, the released part with its warp); `fem follow last` shows the newest stored run of an opened project | `./openphysics --workspace <folder holding the project> --exec 'exec tools/demo_print_result.nav'` | yes, results in [validation/prints/README.md](../../validation/prints/README.md) |
| `tools/html2pdf.swift` | an HTML file to an A4 PDF with the system's WebKit, for the one-page documents in `docs/release/` | `swiftc -O tools/html2pdf.swift -o build/html2pdf && build/html2pdf in.html out.pdf` | yes |
| `tools/frame_subject.py` | crops a rendered picture so its subject sits in the centre at a given aspect ratio (the application frames a part left of its side panel, so exports come out off centre); needs Pillow and NumPy | `python3 tools/frame_subject.py in.png out.png --aspect 1.6` | yes, the two figures of the one-page paper |
| `tools/agentwalk.nav` | Agent mode: a typed question reaches a result in the window with no button pressed | `python3 tools/uicheck.py --agentwalk` | by the app session, 28 s |
| `tools/agent_standin.py` | a scripted MCP client standing in for an AI tool; calls no model | run by `agentwalk.nav` | yes |
| `samples/`, `src/ctl/print_profiles.json` | Simple mode's sample parts and print presets, each value with its source | read by the app | n/a |
| `demo/verify_supports.py` | supports verification S5, S5b and S6: explicitly meshed support walls against the homogenised block, in the LPBF build and the FFF heat model | `python3 demo/verify_supports.py s5 s5b s6` (about 40 min) | yes, 2026-09-19 |
| `am` (in-app terminal command) | runs any of the typed operations from inside the app; `am` alone lists them (`src/commands.c:1918`) | open `./openphysics`, press the terminal key, type `am` | no |

## Built-in geometry

| Gem | What it is | Command | Ran it? |
|---|---|---|---|
| `models/*.stl` | ten bodies written by this project's own geometry code: `ahmed`, `airfoil`, `cube`, `cylinder`, `glider`, `plate`, `sphere`, `submarine`, `torus`, `wing` | in the app: `scene car`; headless: `./openphysics-ctl --embedded call geometry_import '{"path":"models/cube.stl","units":"mm"}'` | partly — used through `capabilities_get`/import paths in the suites, not each one by hand |
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
| `tools/release/export_public.sh` | the public tree: the tracked files of a commit minus what the private branch's `.release-exclude` keeps back, copied into a folder of their own (a git repository borrowing this one's objects), then checked there: nothing left out is present, `tools/check.sh f`, no private name, path, address or host, no secret, every link resolving (`tools/linkcheck.py`); a summary and exit 1 on any hit ([docs/release/LAUNCH.md](../release/LAUNCH.md)) | `bash tools/release/export_public.sh SCRATCH [TARGET]` | yes, 2026-10-21: `release/prep` at f4ed63a, 1 622 files and 364 MB, every check passed |
| `tools/ccx_crosscheck.py` | exports a finished run as a CalculiX deck and compares displacement, reactions, energy and stress | `python3 tools/ccx_crosscheck.py export <run_dir> <out>` then `compare` | no — needs CalculiX (`~/.openphysicsai/eval-tools/`, not part of the repo) |
| `release/evaluation/independent/*.py` | the scripts behind the CalculiX comparison: `navier_runs.py`, `x1_ccx.py`, `x2_ccx.py`, `beam_tests.py`, `retain_navier.py` | see [EVALUATION_RECORD.md](../../release/evaluation/EVALUATION_RECORD.md) | no |
| `release/evaluation/physical/analyze.py` | the pre-registered analysis of a physical comparison; refuses to report anything without real readings, and has a synthetic self-test | `python3 release/evaluation/physical/analyze.py --self-test` | **yes** — "self-test passed"; on the empty template it exits 3 with "validation pending" |
| `release/evaluation/physical/predict.py`, `PROTOCOL.md`, `measurement_template.csv` | the whole physical test protocol: specimens, mounting, load steps, uncertainty budget, and the predictions to compare against | `python3 release/evaluation/physical/predict.py --openphysics-ctl ./openphysics-ctl` | no |
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
| `tools/site_build.py` | assembles the website from `site/` into `build/site/`: fills in the name, copies the public films, names the stylesheet and script by content hash, and writes nothing unless every link resolves on the public branch, nothing is loaded from another site, and no never-public phrase appears; added 2026-10-07 | `python3 tools/site_build.py [--brand NAME]` | yes: 34 files, 187 kB for the first view, links checked against origin/main |
| `tools/site_poster.swift` | writes one frame of an animated GIF or WebP as a JPEG poster for the website, from the middle of the film by default; added 2026-10-07 | `swiftc -O tools/site_poster.swift -o /tmp/site_poster` then `/tmp/site_poster IN OUT.jpg [fraction]` | yes: the 15 posters in `site/media/posters` |
| `tools/site_video.swift` | turns an animated GIF or WebP into a small H.264 MP4 for the website, every frame over the films' dark background and with its own duration; added 2026-10-07 | `swiftc -O tools/site_video.swift -o /tmp/site_video` then `/tmp/site_video IN OUT.mp4 [kbps]` | yes: the 15 films in `site/media/films`, 4.6 MB against 45 MB of GIF and WebP |
| `tools/site_card.swift` | draws the website's share card (1200 x 630) and home-screen icon (180 x 180) in the studio look; added 2026-10-07 | `swiftc -O tools/site_card.swift -o /tmp/site_card` then `/tmp/site_card HERO.jpg CARD.jpg ICON.png NAME HEADLINE LABEL` | yes: `site/media/card.jpg`, `site/apple-touch-icon.png` |
| `tools/web_build.py` | the physics lab as WebAssembly for the website's lab page: compiles a `git archive` of the branch the site describes (origin/main) plus `src/web/` with wasi-sdk, writes the page's scenarios from `site/lab/catalogue.json`, and with `--check` runs each in WebAssembly under Node and natively from the same sources, compares them and draws each card picture with the module; added 2026-10-07 | `python3 tools/web_build.py --check` | yes, 2026-10-08: seven scenarios, three identical to the native results byte for byte, four with identical diagnostics and fields within 2e-7 of their largest value |
| `tools/web_run.mjs` | runs a lab module under Node through the page's own WASI layer (`site/lab/wasi.js`): `labrun.wasm` with labrun's command line, or the page's calls on `lab.wasm` (run, open, view, render) writing frames as PNG; added 2026-10-07 | `node tools/web_run.mjs build/web/site/lab/lab.wasm SCENARIO.json OUTDIR --frames 3` | yes: the previews and card pictures of the lab page |
| `tools/labdiff.c` | how far apart two lab results are, field by field: the largest difference over all frames relative to the field's largest magnitude; added 2026-10-07 | built by `tools/web_build.py --check` as `build/web/labdiff-native`; `labdiff A.lab B.lab` | yes: WebAssembly against native results |
| `Mech Sim/docs/assembly-and-study-formats.md`, `backend-decision.md` | the formats and the backend decision record | read | no |
| `demo/print_player/player.html` | a browser viewer for print results — **rejected by the owner**; kept only as history ([HISTORY.md](HISTORY.md)) | — | no |

## Small but easy to miss

- **`tools/embed.c`** turns `ops_schema.json` and `materials.json` into C arrays at build time, which is why every
  binary carries its own contract (`Makefile` → `build/gen/`).
- **`openphysics-ctl doctor`** is a full self-diagnosis, including a reference solve; it runs at the end of `make test`.
- **`./openphysics-ctl --embedded call capabilities_get`** answers what the engine can and cannot do, machine-readable.
  **Ran it:** it lists `static_structural`, `transient_thermal`, `thermomechanical`, `conjugate_heat_transfer`,
  `comparison_study`, `fff_process`, `lpbf_process` — the last two as **planned**, so an agent is not misled.
- **`docs/images/`** holds the renders used by the root `README.md`, including the two print sequences and the robot
  link, all produced by this software.
- **`tools/pngcheck.py`** decodes what the C PNG encoder wrote, independently of it — a genuine cross-check, not a
  round trip through the same code.
- **Surface loads on curved voxel faces are already corrected** (GOALS.md G22, 2026-10-08): pressure, traction, force,
  heat flux, convection and radiation act on each voxel face over its staircase factor |nx| + |ny| + |nz|
  (`mesh_face_staircase` in `src/ctl/selection.c`). Apply one condition to a whole curved surface; do not split it
  into groups or divide h by an area ratio in a script, which would correct it twice. Checked by
  `tools/ringloads.py` (a thin ring against Lame and radial conduction) and the 45 degree plate in `tools/amtest.c`.
  The FFF print process (`fff_print`) cools every exposed face the same way (G22 step 4, `FffMesh.face_weight`,
  checked by `tools/printskin.py` on a turned wall); its summary's `results.skin` states the surface it cools through.

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

[tools/labxrtest.c](../../tools/labxrtest.c) checks that the headset's lab scene (src/lab/labxr.c) is the scene the app
draws: triangles, values, particles, lines, looks, volume samples and mask against `labscene_build` in six views, half
floats against an independent reference, and every result on the computer; `labxrtest --write FILE` writes its
synthetic result for `tools/xr/xr_test.py`.

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


## Reproducible computed art

`tools/art_gallery.py` generates and validates three closed sculptural STL models using the standard library.
`tools/art_gallery_compute.py` runs their FDM, LPBF and thermal demonstrations over native MCP, with fixed numerical
acceptance, preserved evidence and explicit mesh-area bias. `tools/art_gallery_capture.py` records fixed-range native
stills and films. `tools/encode_video.swift` encodes the frames through macOS frameworks and verifies the output
stream. The [gallery and exact recipe](../art-gallery.md) distinguish computed fields from presentation choices.
