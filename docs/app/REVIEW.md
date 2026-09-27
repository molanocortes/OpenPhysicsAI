# NAVIER-AM: what to click, and what should happen

One page for a manual review, in the order you meet things. Every line is one control, what it should do, and the
screenshot that shows it. Open the app by double-clicking `NAVIER.app`. The design these screens are held to is
[DESIGN.md](DESIGN.md).

## Three modes

A switch at the top right, `Simple | Advanced | Agent`, and `?` for the words used. The choice is remembered. A new
user starts in Simple, on the sample parts over a neutral empty view: no tunnel box, no floor until there is a part. The
tunnel scene loads the first time the flow workspace is opened.

| Click | What should happen | Picture |
|---|---|---|
| Simple | Five screens, one big button each, no engineering words. | [simple-1-part.png](simple-1-part.png) |
| Advanced | Everything below this section: the six-step strip, every control, the terminal. Nothing is removed. | [cold-1-empty.png](cold-1-empty.png) |
| Agent | One question field and STOP; your own AI tool does the work and the window shows what it does. | [agent-2-result.png](agent-2-result.png) |
| ? | The glossary: every word Simple mode uses, in two sentences. | |

## Simple mode, as a first-time user

| Screen | Click | What should happen | Picture |
|---|---|---|---|
| 1 Choose a part | a sample card, or drop an STL | The part appears. For your own file you are asked how long it is, with its size shown in mm, cm and inches. One sentence says whether it can be simulated; small gaps are closed without asking and the sentence says how many. | [simple-1-part.png](simple-1-part.png), [simple-1b-part-chosen.png](simple-1b-part-chosen.png) |
| 2 How will it be printed? | Metal powder / Plastic filament, a preset card | Rest the pointer on a preset: every number says where it comes from. A metal preset names the library record `alsi10mg_lpbf`, shows its stiffness and Poisson's ratio, and says the metal is allowed to yield at 273 MPa (published, Renishaw data sheet). For your own part, CHOOSE THE FACE ON THE PART and click the face that sits on the plate. | [simple-2-print.png](simple-2-print.png) |
| 3 Simulate | Simulate | A time estimate before (for a metal that may yield, a range: how much it yields decides, and that is known only once it runs), then a bar that says "layer 6 of 24", seconds left, and the part growing in the view. STOP ends it. | [simple-3-simulate.png](simple-3-simulate.png), [simple-3b-running.png](simple-3b-running.png) |
| 4 Results | PLAY, TRUE SIZE, look inside, MORE | First, in amber, what the simulation is and is not, including "the metal is allowed to yield". Then warp, residual stress and risk, each with its unit and where it is; a cross marks the warp. The risk line compares the stress with the material's yield value ("up to 296 MPa near the plate; the material yields at 292 MPa"), or says there is no yield value with a source. When the build carried supports they are grey and their volume is stated. MORE shows SUPPORT (on and off) and ALL CONTROLS (Advanced). A build made in Agent mode or opened from disk opens on this screen. | [simple-4-results.png](simple-4-results.png) |
| 5 Report | Write the report, Open the report | The report opens with the same words as the results screen, then the numbers and their sources. | [simple-5-report.png](simple-5-report.png) |

The same path at 1280 x 800, from the installed bundle with an empty home folder: [first-run/](first-run/).

## Agent mode

| Click | What should happen | Picture |
|---|---|---|
| The question field, Return (or ASK) | Your question goes to the tool chosen in SETTINGS. Its words, and every change it makes in the engine's own words, appear in the transcript; its jobs are followed in the view automatically. When the run ends the last line says where its record was written: `challenges/agents/runs/<date>-agentmode-<tool>-<short>.json`, with the model and turns the tool reported, for a maintainer to rerun. | [agent-2-result.png](agent-2-result.png) |
| STOP | Ends the tool and everything it started. | |
| SETTINGS | Lists the AI tools found on this computer (Claude Code, Codex), the command line, and the folder it works in. The window holds no key. With no tool installed it says how to get one. | |

## Advanced mode

### First run, nothing open

| Click | What should happen | Picture |
|---|---|---|
| (nothing) | The panel reads NO PROJECT and shows only step 1 and the three ways to open a part. Nothing else is offered. |
| `solid open <project>` on a build run over MCP and never saved | The result is labelled lpbf_build, and step 1 names the body the run was built on and says the project file lists none because it was not saved after the import. | [1440x900-1-part-empty.png](1440x900-1-part-empty.png) |
| Drag an STL onto the window | The file waits in step 1 and the panel asks the one thing that is never guessed: which unit its numbers are in. | [1440x900-2-unit.png](1440x900-2-unit.png) |
| MM / CM / INCH | The part is imported, repaired and drawn, and step 1 gives the geometry verdict in words: closed or not, how many components, the thinnest wall. | [1440x900-3-part.png](1440x900-3-part.png) |
| REPAIR (only for a part that is not closed) | Drops stray shells, fills small holes, and says in numbers what it changed. The part is changed, so nothing happens until you press it. | |
| OPEN / NEW PROJECT / OPEN... | The same three things by file dialog instead of by drag. | [1440x900-1-part-empty.png](1440x900-1-part-empty.png) |
| ANALYSE STRESS / SIMULATE THE BUILD | Chooses which question the part is asked. The rest of the strip follows: steps 4 and 5 become BUILD and RUN. | [build-1-part.png](build-1-part.png) |

### 2 MESH

| Click | What should happen | Picture |
|---|---|---|
| The element-size slider | The size changes and the panel says what it will cost. | [1440x900-4-mesh.png](1440x900-4-mesh.png) |
| "or type it" field | The element size can be typed exactly, which is what a mesh study needs. | [1440x900-4-mesh.png](1440x900-4-mesh.png) |
| GENERATE | A hex mesh is made and the panel reports the element count and the volume the staircase costs against the STL. | [1440x900-4-mesh.png](1440x900-4-mesh.png) |

### 3 MATERIAL

| Click | What should happen | Picture |
|---|---|---|
| A material button | The material is assigned and its status is shown. Every material in the library is labelled demonstration. | [1440x900-5-material.png](1440x900-5-material.png) |

### 4 HOLD & LOAD (the stress path)

| Click | What should happen | Picture |
|---|---|---|
| Move the pointer over the part | The face under it lights up. | [step-4-picked.png](step-4-picked.png) |
| Click a face | The face is collected and the panel gives its area. Clicking it again, or shift-clicking, drops it. | [step-4-picked.png](step-4-picked.png) |
| BOX, then drag a rectangle | Every face whose centre falls inside is taken at once. Shift-drag drops them. There is no occlusion test, so a box takes the faces behind the part too, and the panel says so. | [pick-by-box.png](pick-by-box.png) |
| FRONT ONLY | The box then takes only the faces turned towards you. | |
| HOLD | The collected faces become a fixed support. | [walk-3-conditions.png](walk-3-conditions.png) |
| The force field, then LOAD | The number typed, in newtons, in the chosen direction, on the collected faces. | [walk-3-conditions.png](walk-3-conditions.png) |
| GRAVITY | Gravity on the whole body. | [walk-3-conditions.png](walk-3-conditions.png) |
| CLEAR | Drops everything collected but keeps the conditions already applied. | [walk-3-conditions.png](walk-3-conditions.png) |
| VALIDATE | The solver's own pre-flight checks, in words. | [walk-3-conditions.png](walk-3-conditions.png) |

### 4 BUILD (the printing path)

| Click | What should happen | Picture |
|---|---|---|
| LPBF METAL / FFF PLASTIC | Switches between the two job kinds the engine has. | (picture removed 2026-09-27, to be recaptured) / [build-fff-setup.png](build-fff-setup.png) |
| X / Y | The machine axis the part's long side lay along. | (picture removed 2026-09-27, to be recaptured) |
| The layer field | The simulation layer, in mm. | (picture removed 2026-09-27, to be recaptured) |
| exx / eyy / ezz, User / Inferred / Calibrated | Three strain components typed by hand, with a provenance the operation insists on. Until 2026-09-27 the panel also offered named strain sets (OT, P05, P17, ROT), which the screenshot still shows; they were removed with the data they came from. | (picture removed 2026-09-27, to be recaptured) |
| CUT ON / OFF, height, kerf, from | The wire cut that frees the part. The panel says these three are assumed, not published. | (picture removed 2026-09-27, to be recaptured) |
| E and the Poisson field | The elastic constants, defaulting to the assigned material and typeable for the material the build is really for. | (picture removed 2026-09-27, to be recaptured) |

### 5 SOLVE / 5 RUN

| Click | What should happen | Picture |
|---|---|---|
| STATIC / THERMAL / THERMO-MECH | The analysis kind, on the stress path only. | [1440x900-7-solve.png](1440x900-7-solve.png) |
| SOLVE or RUN BUILD | The job is submitted and the window follows it. The window stays live while it runs. | [1440x900-7-solve.png](1440x900-7-solve.png) |
| CANCEL | Stops a running job at its next progress check. | [1440x900-7-solve.png](1440x900-7-solve.png) |

### 6 RESULTS

| Click | What should happen | Picture |
|---|---|---|
| STRESS / DISPL / TEMP | Only the fields this result actually carries are offered. | [walk-4-result.png](walk-4-result.png) |
| SURFACE | The part's own surface carries the result, interpolated from the mesh inside it. A chamfer is a chamfer. The panel states the largest distance a vertex had to be snapped onto the mesh. | [chamfer-surface.png](chamfer-surface.png), [result-on-surface.png](result-on-surface.png) |
| VOXELS | The mesh the numbers actually came from, staircase and all. | [chamfer-voxels.png](chamfer-voxels.png), [result-on-voxels.png](result-on-voxels.png) |
| (the marker) | A cross sits on the largest value of what is drawn. | [result-on-surface.png](result-on-surface.png) |
| CHECK MESH | Solves again at 0.7 of the element size, capped near 60 000 elements, and says how far the answer moved. Under 5 % is called converged. | [walk-5-convergence.png](walk-5-convergence.png) |
| PLAY BUILD | Plays the build from the first layer with the deformation at true scale: what you see is the distortion itself. | (picture removed 2026-09-27, to be recaptured) |
| AFTER THE CUT | Jumps to the released part. | (picture removed 2026-09-27, to be recaptured) |
| LAYER STUDY | Runs the build again at half the simulation layer and says how far the answer moved, labelled for what it is: halving the layer changes the model, not only the discretisation. | (picture removed 2026-09-27, to be recaptured) |
| REPORT | Writes `<project>/report/<date>-<time>/` with the numbers, the setup, the checks, the images and what the numbers are not. One folder per report. | [walk-6-report.png](walk-6-report.png) |
| VIEW | Opens the viewer tools: stored-time slider, PLAY, playback speed, colour range, deformation scale, section, groups, HIDE. | [walk-4-result.png](walk-4-result.png) |

### The viewer tools, under VIEW

| Click | What should happen |
|---|---|
| The stored-time slider | Steps a transient result one stored time at a time. |
| PLAY / PAUSE and the speed slider | Runs through the stored times; the colour range stays over all of them so the change is what you see. |
| ALL TIMES / THIS STEP | Whether the colour range covers the whole run or only the step on screen. |
| The deformation slider, AUTO, TRUE | How much the displacement is exaggerated. TRUE is scale 1. |
| SECTION, the axis, the position slider, FLIP | Cuts the drawn part with a plane. The STL is clipped and the faces the cut opens are drawn as voxel faces, because that is what they are. |
| PART / SUPPORT / PLATE | Which element groups are drawn. |
| HIDE / SHOW | Takes the result surface away and brings it back. |

## The scripted demos

Each runs off-screen and writes its own screenshots. Run them from the repository root.

```bash
./navier --workspace /tmp/navier-walkthrough --exec 'exec tools/walkthrough.nav'
```
The owner's own assembly, from opening the STL to a written report, by clicking only. Measured at 89 s. The assembly is
not in the repository: `NAVIER_WALK_PART=<its path> python3 tools/uicheck.py --walkthrough` runs it with the path filled in.

```bash
python3 tools/uicheck.py --simplewalk
```
Simple mode as a first-time user: the sample bracket to a report, timed, with every screen's words checked for jargon.

```bash
python3 tools/uicheck.py --agentwalk
```
Agent mode with a scripted stand-in for the AI tool: a typed question reaches a result with no button pressed.

## Known limitations, in plain words

- **The material values are demonstration data.** Every material in the library is labelled so. They are typical
  magnitudes, not a grade, a supplier or a test. Any safety factor built on them is an example, not a design value.
- **The mesh inside is voxels.** The part you see is your own STL, but the solver works on a staircase of boxes.
  Stresses on inclined and curved faces are approximate, and a peak at a corner or a support is usually a property
  of the mesh rather than of the part. The panel says so next to the peak, and VOXELS shows you the mesh.
- **The structural path is linear elastic.** Small strains, one load case, one temperature. No plasticity, no
  contact between parts, no large deflection, no buckling, no fatigue.
- **A part must be watertight to be meshed.** The app says what is wrong with a part it cannot mesh and what to do,
  but it does not repair holes for you.
- **The validation is not public.** The LPBF build was compared with measured calibration cantilevers whose
  measurements are not part of this repository, and nothing else is validated against measurement yet. The inherent strain is a calibrated
  input, not a material property.
- **A voxel mesh of a thin-walled part falls into islands.** Walls thinner than two elements are lost, and the solver
  refuses a model with a piece that nothing holds, naming the region and where it is. A coarser mesh can keep such a
  part in one region, so refining is not always the safer direction on a part like this.
- **A repair is a change to the part.** REPAIR drops shells and fills holes; the counts are on screen and in the
  report, and the volume before and after says how much of the part they were.
