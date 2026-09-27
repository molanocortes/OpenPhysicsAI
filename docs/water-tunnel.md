# NAVIER — real-time 3D water tunnel (lattice Boltzmann, pure C)

NAVIER is an interactive wind/water tunnel that solves the incompressible Navier–Stokes equations in 3D, live, on
your Mac. Drop in any STL model, set the fluid, speed, temperature and surface roughness, press **START**, and watch
the flow develop: streamlines, tracer particles, vortex cores, pressure and velocity fields, drag and lift.

Everything is plain C11 with no third-party libraries. It uses the macOS system frameworks only: Cocoa via the
Objective-C runtime, OpenGL 4.1, CoreText and ImageIO.

## Headless engineering comparison (NAVIER 0.3.0-rc1)

The same repository contains a headless structural workflow. An AI client (over MCP) or `navier-ctl` compares two to
four designs of a bracket or fixture under equivalent mounting and load. It runs a mesh-refinement study and
sensitivities, and records an Engineering Evidence Record. It predicts stiffness and mass under stated conditions,
not safety. See `docs/release/README.md`, the example in `examples/bracket_comparison/`, and the readiness assessment
in `release/READINESS.md`.

An independent evaluation of rc1 led to **0.3.0-rc2** (2026-09-17): see `release/evaluation/EVALUATION_REPORT.md`.

## Build and run

```bash
make
```

```bash
./navier
```

Requirements: macOS 11 or later and the Xcode Command Line Tools (`clang`). No Homebrew is needed.

Useful start-up options:

| option | effect |
|---|---|
| `--scene <name>` | `glider` (default), `sphere`, `cylinder`, `car`, `submarine`, `wing`, `airfoil`, `plate`, `torus` |
| `--load model.stl` | start with your own model |
| `--exec "speed 2; roughness 0.5mm; start"` | run terminal commands at launch |
| `--run` | start simulating immediately |
| `--headless --steps 3000 --shot out.png` | off-screen run that saves an image |

## Using it

**Mouse and trackpad**
- Drag: orbit. Shift-drag or right-drag: pan.
- Scroll wheel or pinch: zoom toward the cursor. Two-finger scroll orbits; Shift pans.
- Cmd-drag a rectangle: zoom into that region. `K` locks the pivot so you can orbit around it.
- Double-click the model or slice: focus there.
- Option-drag the model to turn it: left/right yaws, up/down pitches, add Shift to roll. Option-click without
  dragging places a flow probe (velocity, pressure, Cp).
- Drop an `.stl` onto the window to load it.
- Anything clickable shows a hand cursor; the terminal shows a text cursor.
- Click a value in the side panel to put its command in the terminal, ready to edit (lattice → `grid`, Reynolds →
  `re`, τ₀ → `les`). Cd/Cl print the `status` report, drag/lift export `forces.csv`, steps/s prints `perf`.
- Click RUNNING/PAUSED to start or pause, the colour legend to cycle colour maps, the `lines` bar to cycle line
  colours, the axes gizmo for the hero camera, and the MLUPS · FPS pill for a performance report.

**Keys** (when the terminal isn't focused)
- `Space`: start/pause. `R`: reset the flow.
- `,` / `.`: halve / double the playback speed (slow motion down to 1%). `/`: advance one time step.
- `1`–`8`: switch field (speed, ux, uy, uz, pressure, Cp, vorticity, Q-criterion).
- `S` / `P` / `V` / `O`: toggle streamlines, particles, vortex surfaces, volume rendering.
- `L`: toggle the slice. `X` `Y` `Z`: slice axis. `[` `]`: move the slice.
- `C`: next colour map. `G`: surface grid. `M`: surface colouring.
- `F`: frame the model (in the analysis workspace: frame the part). `0`: reset the camera. `K`: lock the pivot.
  `H`: hide the interface.
- `W`: switch between the water tunnel and the finite-element analysis.
- `` ` `` or `Return`: focus the terminal. `Esc`: leave it.
- `Cmd-O`: open STL. `Cmd-S`: screenshot.

**CONTROLS tabs** in the side panel
- FLOW: free-stream speed, angle of attack, roughness, temperature, inlet turbulence.
- MODEL: quarter turns (X/Y/Z 90°), Z-UP, FLIP, AUTO (longest side along the flow), RESET (as loaded), and pitch,
  yaw, roll and size sliders. Loading an STL opens this tab.
- TIME: playback speed (slow motion), time step, resolution presets and +1/+10/+100 steps. To run faster, raise the
  time step (up to about 2×, less accurate as the lattice Mach number grows) or choose DRAFT (about 3× more steps per
  second, each covering more time). The readout says how long the water takes to travel one model length on screen.
- LINES: where streamlines start. Pick a rake (SHEET, GRID, LINE, POINT, WAKE, RANDOM), move it with the position
  sliders or by dragging its orange handle in the view, and set its size and the number of lines. AUTO puts it back
  around the model. A POINT rake just upstream of a wing tip follows the tip vortex.

**The terminal** (right-hand panel) accepts commands with Tab completion and ↑/↓ history. Every toolbar button also
prints the equivalent command, so you learn as you click. Type `help` for everything. Common ones:

```text
scene glider                 load a preset experiment
load ~/Desktop/part.stl      import an STL (quotes for paths with spaces)
orient zup                   fix CAD files authored Z-up
orient auto                  longest side along the flow, thinnest side up (orient reset undoes it)
rotate y 90                  quarter turn of the model about the tunnel's vertical axis
playback 25%                 slow motion at 25% of full solver speed (playback max to undo)
aoa 6                        angle of attack in degrees (re-voxelises live)
fluid seawater               water seawater air glycerin oil honey mercury
temp 12                      temperature, updates density and viscosity
speed 3 kn                   m/s, km/h, kn, mph ...
re 50000                     choose the speed that gives a Reynolds number
length 25 cm                 physical size of the model
roughness 0.5mm              equivalent sand-grain roughness (or: smooth)
turbulence 2                 inlet turbulence intensity in %
quality high                 finer lattice (draft / normal / high / ultra)
view vorticity               field shown on the surface, slice and lines
stats                        min/max/percentiles of the field, colour range, visualisation timings
cmap inferno                 turbo cfd jet viridis plasma inferno magma coolwarm hydro icefire gray
linecmap hydro               separate colour map for streamlines and particles (linecmap same to undo)
gradient #03045e #00b4d8 #caf0f8    your own colour gradient
range 0 1.2                  fixed colour range in display units (range auto to undo)
slice z 0.5                  cut plane through the tunnel
streamlines grid seeds 900   rakes: plane grid line point wake random
streamlines at 0.2 0.5 0.8   move the rake (fractions of the tunnel); also size h w, normal x|y|z, auto, probe
particles count 400000       GPU tracer particles (emitter sheet | model | full)
vortices level 50            Q-criterion vortex surfaces at Q (L/U)² = 50 from the 2-cell filtered velocity
                             gradient: a fixed physical threshold, higher shows only the stronger cores
collision rr bulk 1          collision model (rr default), bulk viscosity, optional FD hybrid (hrr 0.98)
boundary curved              model walls at the exact mesh surface (or staircase for plain voxel walls)
floor moving                 rolling road for ground vehicles
camera side | camera lock    presets and pivot lock
forces run1.csv              export drag/lift/side coefficients over time
status                       full report (units, Reynolds, roughness regime, forces)
```

## The analysis workspace (NAVIER-AM)

The same window drives a second solver: a hexahedral finite-element core for additive-manufacturing parts
(static structural, transient thermal, thermomechanical). `W`, or the FLUID / SOLID buttons in the toolbar,
switches between them. Both halves share the camera, the colour maps, the legend and the terminal; switching to
SOLID puts the flow layers away and pauses the tunnel, and switching back restores them.

Every button in the ANALYSIS panel runs one of the typed operations that the control socket and the MCP tools call,
so the interface and an AI client drive exactly the same engine. `am <operation>` runs any of them by hand.

```text
workspace solid              switch (also: W, or the SOLID button)
solid import mm              hand the STL in the tunnel to the analysis (the file's unit is never guessed)
am mesh_generate element_size=2      voxel hex8 mesh
am material_assign body=part material=ss316l_lpbf_demo source=user
solid fixbase                hold the lowest face of the part
am boundary_apply '{"name": "pull", "kind": "force", "selection": "top", "force": [0, 0, -500], "source": "user"}'
solid run static             or: solid run thermal 600 20 | solid run thermomechanical 600 20
solid wait                   the solve runs on a worker thread; the interface keeps drawing
fem field von_mises          displayed field (also displacement, temperature; keys 1-3)
fem deform auto              deformation exaggeration
fem step last                stored time of a transient result (fem range all|step for the colour range)
fem fit                      frame the part
solid export                 load the analysed body back into the tunnel
```

The result is drawn in the same 3D view: the boundary of the hex mesh, displaced by the chosen exaggeration and
coloured by the selected field with the shared colour map. The RUN tab shows the solver's own checks (equilibrium,
energy, peak stress and its 99th percentile, maximum displacement, solve time) and marks a peak that sits at a
support or a sharp inside corner as the singularity it is, not as a design value.

All library materials are demonstration values, the mesh boundary is a staircase, and no flow load is transferred to
the structure yet. `STATUS.md` records what has been verified and against what.

## Automated experiments

A script is a `.nav` file with one command per line (`#` starts a comment). Run it with `exec file.nav`, or drop it
onto the window. Commands run in order, and each one waits until the solver has applied the previous change.

| command | effect |
|---|---|
| `wait 3000` | run the solver for 3000 steps before the next command |
| `log results.csv` | append the conditions and mean Cd, Cl, Cs to a CSV (averaged over the second half of the last `wait`, after the start-up transient) |
| `sweep aoa -4 12 2 steps 2500 polar.csv` | a whole automated series; sweepable: `aoa yaw roll speed re roughness temp turbulence` (roughness in metres) |
| `cancel` | stop a running script or sweep |
| `kick 0.1` | start-up perturbation: a short cross-flow pulse (10% of U) that breaks wake symmetry so vortex shedding starts quickly; bluff-body scenes apply it automatically after every `reset` |

Ready-made scripts in `examples/`:
- `glider_polar.nav`: lift polar of the sailplane.
- `roughness_study.nav`: drag versus hull roughness.
- `karman_street.nav`: vortex shedding and the Strouhal number.
- `my_model.nav`: a template for your own STL.

```bash
./navier --exec "exec examples/glider_polar.nav"
```

### Profiling and UI tests

| command | effect |
|---|---|
| `perf` | frame time split into events, texture uploads, 3D render, UI and swap; upload volume; worker and solver timings |
| `perf sync on` | wait for the GPU after each phase so its time lands in the right place (slower; `perf sync off` undoes it) |
| `frames 60` | in scripts: pause for 60 rendered frames |
| `uilist` | list the buttons, sliders and clickable panel items on screen, with positions |
| `uiclick PARTICLES` | click a control through the real mouse path (press, hold 4 frames, release); `uiclick sl_aoa 0.25` clicks a slider a quarter of the way along |

`tools/uitest.nav` clicks every button, slider and panel control and saves `build/uitest.png`.
`tools/particles_test.nav` switches GPU particles on and off mid-run. Run either off-screen and read the log:

```bash
./navier --headless --size 1440x900 --exec "exec tools/uitest.nav"
```

`tools/wall_study.sh` measures sphere drag against resolution for both model-wall treatments, which is the check to
run before quoting a force from a new geometry:

```bash
tools/wall_study.sh 800 5 8 12 16 24
```

## What is being solved

- **Method.** Lattice Boltzmann, D3Q19 velocity set. In the low-Mach limit it recovers the incompressible
  Navier–Stokes equations, so the lattice Mach number is kept at about 0.14.
- **Collision.**
  - Regularized with third-order recursive Hermite terms (Malaspinas 2015), on by default. D3Q19 carries the pairs
    xxy±yzz, xzz±xyy and yyz±xxz; their lattice norms set the weights. The scheme stays stable as the relaxation
    time approaches 1/2, which is where high-Reynolds water flows live (see "Free-stream noise" below).
  - The stress trace relaxes separately (bulk viscosity, `collision bulk 1`). That damps the pressure waves of a
    weakly compressible solver without changing the incompressible flow.
  - The Smagorinsky–Lilly LES sub-grid model is on by default, with the eddy viscosity computed locally from the
    deviatoric non-equilibrium stress, so pressure noise can't raise it.
  - For comparison: a finite-difference hybrid (HRR, Jacob et al. 2018, `collision hrr 0.98`), second-order
    regularized (`collision reg`) and BGK (`collision bgk`).
  - Cells uncovered when the model moves start from their neighbours' state instead of at rest.
- **Units.** Physical inputs (U, L, ν from fluid and temperature) set the Reynolds number. The lattice spacing is
  Δx = L / cells-across-model; the time step follows from the lattice velocity, Δt = u_lb·Δx/U.
  - Changing the speed live changes Re and the physical time step, not the lattice velocity.
  - Very viscous flows automatically lower u_lb to keep τ ≤ 1.1.
- **Boundaries.**
  - Velocity inlet with optional synthetic turbulence, and pressure outlet.
  - Absorbing sponge layers at both ends relax toward the free stream, so pressure waves don't reflect.
  - Tunnel walls are selectable: free-slip, far-field, no-slip, moving (rolling road) or periodic.
- **Geometry.** STL meshes are voxelised with a multi-axis winding-number test (robust to overlapping parts and small
  holes), plus a thin-surface shell so wings and plates stay leak-free.
- **Model walls.** Curved by default (`boundary curved`). For every fluid-to-solid link a ray is cast against the mesh
  (double-precision BVH, rebuilt only when the mesh itself changes; moving or turning the model only moves the rays) to
  find where the surface really crosses the link, and the bounce-back is interpolated to that point (Bouzidi,
  Firdaouss & Lallemand 2001, linear). The same wall positions enter the momentum exchange that gives drag and lift.
  Links whose ray misses the surface — where the thin-surface shell pushes the solid boundary past the mesh — keep
  halfway bounce-back, and `status` reports the percentage that sit on the surface. `boundary staircase` switches back
  to plain voxel walls for comparison. Placing the walls costs about 6 ms for the glider's 33,000 links (the mesh BVH
  is built once, in 3 ms), so it keeps up with dragging the model.
  - The shell that keeps thin features leak-free (`shell`, default 0.5 cells) also inflates the body by up to half a
    cell, and that is where the missing links come from: on the glider, shell 0.5 puts 82% of links on the surface,
    0.25 puts 91%, and 0 puts 100%. With no shell, though, the outer wing is thinner than a cell and starts leaking —
    its drag force falls by 37%. Leave the shell at 0.5 unless the body is closed and everywhere thicker than a cell.
- **Forces.** Momentum exchange on every boundary link, at the interpolated wall position when curved walls are on,
  reported as Cd, Cl, Cs and Newtons. A Strouhal number is detected from lift oscillations.
- **Roughness.**
  - A discrete-element roughness model adds the drag of sub-grid roughness elements of height ks in the first fluid
    cell layer: F = −½·C_D·(ks/Δx)·|u_t|·u_t.
  - `status` reports the estimated ks⁺ and whether the surface is hydraulically smooth, transitional or fully rough.

### Validation

Sphere at Re = 100 in a 5D × 5D tunnel (3.1% blockage), recursive regularized collision, both wall treatments
(`tools/wall_study.sh 800 5 8 12 16 24`; Cd averaged over the last 40% of 800 steps per cell of diameter):

| D (cells) | staircase walls | curved walls |
|---|---|---|
| 8 | 1.4543 | 1.3384 |
| 12 | 1.3052 | 1.2249 |
| 16 | 1.2665 | 1.1948 |
| 24 | 1.2046 | 1.1712 |

- Curved walls at D = 16 are already closer to the limit than staircase walls at D = 24: the same accuracy from
  3.4× fewer cells.
- The curved sequence converges smoothly — observed order 1.7 on the three finest grids, Richardson limit 1.148. The
  staircase sequence does not: its decrements per unit of grid spacing run 3.6, 1.9, 3.0 with no consistent order,
  because a stair-stepped sphere changes shape discontinuously with resolution. That is the practical reason to use
  curved walls for numbers you intend to publish.
- The Schiller–Naumann correlation for an unbounded sphere gives **1.092**. With a nominal blockage correction,
  Cd/(1 + 2·blockage), the curved D = 24 value becomes 1.10 and the staircase one 1.13. The correction constant is
  itself uncertain, so read this as a consistency check rather than a calibration.
- Lift is zero to machine precision, as symmetry requires.

Kármán vortex street: circular cylinder at Re = 150, D = 20 cells, spanwise periodic, 13% blockage (`scene cylinder`):
- After the start-up perturbation the wake sheds alternating vortices, and the lift coefficient oscillates periodically.
- The detected Strouhal number is **St = 0.192** with the current defaults (curved walls, recursive collision) after
  14,000 steps; staircase walls give 0.190 and the older second-order collision gave 0.170. Drag falls from Cd = 1.576
  to 1.532 when the walls follow the cylinder instead of its voxel staircase, with 94% of the links on the surface.
- Unconfined experiments give about 0.18 at this Reynolds number (Williamson). Wall blockage raises the shedding
  frequency, which fits the higher value here, but quote it only after a blockage and resolution study.

Reproduce it with:

```bash
make tools
```

```bash
./build/lbmbench sphere 100 16 6000
```

What curved walls change on a real model: the glider scene at 4° (2.75M cells, 2500 steps) gives Cd = 1.153 and
Cl = 0.360 with curved walls, against Cd = 1.296 and Cl = 0.057 with staircase walls. The wing is only a few cells
thick, so its stair-stepped surface largely destroys the circulation that makes lift — that 6× difference in lift,
not the 11% in drag, is why curved walls matter for anything with a thin lifting surface. Both runs are
under-resolved; do a resolution study before trusting either number.

### Free-stream noise

`./build/lbmbench noise` runs an empty tunnel with the app's inlet, sponges and slip walls at τ₀ = 0.50001 (the
highest Reynolds number the lattice reaches) and no inlet turbulence, where nothing should move:

| collision | RMS deviation from the inflow after 3000 steps |
|---|---|
| second-order regularized (previous default) | 20% of U and growing: a grid-scale instability |
| third-order recursive + bulk viscosity (default) | 4×10⁻⁶ of U, flat |

With a sphere that moves by one cell at half time, the unsteadiness ahead of it stays around 10⁻⁴ U. The new scheme
costs no speed (124 vs 115 MLUPS on 1.8M cells) and gives the same sphere drag at Re 100 (1.2999 vs 1.3012 in a
4D × 4D tunnel).

### Honest limits

- Real-time resolutions (a few million cells) resolve large-scale separation, wakes, tip vortices and vortex shedding
  well. They do not resolve thin boundary layers at water-tunnel Reynolds numbers of 10⁵–10⁶; there, LES and the
  roughness model stand in for near-wall physics.
- Numbers from coarse runs are qualitative-to-semi-quantitative. Refine with `quality high`/`ultra` and compare.
- The lattice viscosity has a floor (τ₀ ≥ 0.50001). Past it the Reynolds number the grid runs at stops rising: the
  panel then shows the effective value marked "eff", and `status` warns. Beyond that point the result is an LES
  whose smallest scales are set by the grid.
- The fluid is single-phase and fills the tunnel: no free surface, cavitation or buoyancy.
- On a fanless MacBook Air, sustained runs will thermally throttle after a few minutes.

## Performance (MacBook Air M2, 8 GB)

The CPU solver is multithreaded and uses NEON vectorization (the compiler auto-vectorizes the fused stream-collide
loop; it was 3× faster than scalar code in testing).

| threads | throughput |
|---|---|
| 1 | about 39 MLUPS |
| 8 | about 130 MLUPS |

MLUPS means million lattice updates per second. The GPU handles rendering, particle advection and bloom.

The solver publishes snapshots at up to 24 Hz. A background worker turns them into half-float textures (the particle
velocity volume at half resolution), streamlines and vortex surfaces, so the render loop only uploads finished data.
On the default glider (2.75 M cells) with streamlines, vortex surfaces and 60,000 particles on, the render thread
spends about 4.7 ms per frame, 2.5 ms of it on texture uploads (about 190 MB/s). The solver keeps about 118 MLUPS.
The window is capped at 60 fps (`fps off` uncaps it), so frames the display can't show don't take CPU time from the
solver.

| quality | cells | memory | steps/s |
|---|---|---|---|
| draft | ~0.9 M | ~0.2 GB | ~140 |
| normal | ~2.8 M | ~0.5 GB | ~45 |
| high | ~5.5 M | ~1.0 GB | ~23 |
| ultra | ~9 M | ~1.6 GB | ~14 |

## Project layout

```text
src/lbm.c           D3Q19 solver: fused stream+collide, boundaries, sponges, forces, roughness
src/sim.c           units, fluids, model placement, solver thread, snapshots, force history
src/geom/           STL import/export, procedural shapes, voxeliser
src/vis_*.c         derived fields (vorticity, Q), streamlines, surface-nets isosurfaces
src/visworker.c     background thread turning snapshots into render data
src/render.c        OpenGL 4.1 renderer: surface, slices, streamlines, GPU particles, volume, bloom
src/ui.c, hud.c     immediate-mode UI, the experiment panel and the analysis panel
src/fembridge.c     the interface's connection to the NAVIER-AM engine: state mirror, operation dispatch,
                    and the finite-element result surface drawn in the shared 3D view
src/fem/, src/ctl/  hex8 structural and thermal solvers, typed operations, projects, jobs (see STATUS.md)
src/console.c       terminal (history, completion); src/commands.c: the command set
src/platform_macos.c  Cocoa window, GL context and input written in C via the Objective-C runtime
tools/              lbmbench (solver benchmark, free-stream noise, sphere validation), wall_study.sh,
                    geomtest, vistest, gltest, UI test scripts
models/             sample STL files generated by the built-in shape library
```
