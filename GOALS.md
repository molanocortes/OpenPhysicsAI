# GOALS: the physics lab programme

The owner's brief of 2026-09-25, turned into goals that one architect agent works through in order, one at a time,
each ending in code, a verification test, pictures and a pushed update. A goal is done only when every box under it
is ticked; the loop that drives the work reads this file, takes the first open goal and continues it.
Up: [AGENTS.md](AGENTS.md) · intent: [VISION.md](VISION.md).

The brief, in the owner's words condensed: simulate projectiles and small asteroids hitting shielding with meshes that
show the deformation; show where a mesh is refined and why; streamlines through the whole body, not one plane; sound
waves in a room; supersonic and hypersonic flow with the shock in front of a capsule; orbital dynamics and
electromagnetism; compare with photographs and papers of the real world and improve the physics where it falls short;
replace public targets by **flags**: real measurements from controlled experiments, hidden from the entrant, scored
so that the score cannot be inverted into the answer and so that a general simulator beats one tuned to a single
flag; put the rankings under the hero picture of the README and say plainly what the project is.

Rules that hold for every goal (from [AGENTS.md](AGENTS.md)): own solvers plus existing open-source work under rule 1
(compatible licence, recorded in docs/THIRD_PARTY.md), double precision in numerical code; a verification case against a closed form written before the first run; units and sources on every number;
no run over ten minutes; results shown in the native app; no AI attribution in commits; no em dashes in prose.
Impact work is framed as the published physics of materials testing, spacecraft shielding and planetary cratering.

## Architecture (decided 2026-09-25)

- New physics domains live in `src/lab/`, one directory per domain, each a plain C library with no UI and no
  OpenGL, so it builds headless on Linux like the rest of the core. Each has a README with the files to read first.
- Every lab solver writes the same result format, `src/lab/labio.h`: a sequence of frames, each holding a mesh
  (structured blocks with their refinement level, or unstructured points, lines and cells) and named scalar fields.
  One format means one headless film maker (`tools/labfilm.c`, on the software renderer) and one viewer in the app.
- Verification suites are `tools/lab*test.c`, built by `make test-lab` and part of `make test-fast`.
- Flags live in `flags/`: public scenario, sealed answer, scorer. Design: [flags/README.md](flags/README.md).

## Goals

Status keys: `[ ]` open, `[~]` in progress, `[x]` done (with the commit).

### G0. Baseline
- [x] branch `lab/physics-lab`, `make` and `make test-fast` green before any change (5 min 24 s, 2026-09-25)

### G1. Lab infrastructure
- [x] `src/lab/labio.[ch]`: frame writer and reader (blocks with levels, points, cells, fields), round-trip test
- [x] `tools/labfilm.c`: headless pictures and films of any lab result (2D field maps with the mesh drawn,
      3D points and meshes), colour bar, title, PNG frames, GIF through `tools/encode_gif.swift`

### G2. Flags: hidden measurements, general scoring, rankings
- [x] `flags/README.md`: what a flag is, what is public, what is sealed, how scores are formed and why they
      cannot be inverted, why generality wins, how a fork enters
- [x] sealed answers outside the repository, a SHA-256 commitment of each in the repository
- [x] `tools/flags.py`: run a simulator on every public scenario, score against the sealed answers, band the score,
      write the board and an SVG podium for the README
- [x] the first flags from papers, each with its source recorded in the sealed file (bow shock, NACA TN 2000; planets, JPL; shedding, NACA Report 1191)

### G3. Compressible flow with adaptive mesh refinement (supersonic and hypersonic)
- [x] `src/lab/gas/`: 2D planar and axisymmetric Euler and Navier-Stokes, finite volume, HLLC flux, MUSCL-Hancock
      and a fifth-order WENO option, block-structured AMR with refinement on density gradient and vorticity,
      bodies as cut cells with merging (the ghost-cell method was tried first; see docs/lab/gas.md)
- [x] verification: Sod shock tube against the exact Riemann solution, oblique shock on a wedge against
      theta-beta-M, normal shock jump, AMR against uniform fine grid
- [x] pictures: hypersonic capsule with its bow shock and the refined mesh drawn; double Mach reflection, nozzle, helium bubble (films in the README, 2026-09-26)
- [x] flags: bow-shock standoff on a sphere (NACA TN 2000); a shock on a cylinder of helium or R22 (Haas and Sturtevant
      1987, read in Quirk and Karni's tables, 2026-09-26)

### G4. Sound in rooms
- [x] `src/lab/acoustic/`: 3D finite-difference time-domain linear acoustics, fourth-order in space option,
      frequency-independent impedance walls, point sources, receivers (a perfectly matched layer: open)
- [x] verification: rectangular room modes, free-field 1/r decay, reflection coefficient of an impedance wall
- [x] pictures: shoebox room with audience seating, wavefront through the room
- [ ] flag: a measured room acoustic quantity from a controlled experiment

### G5. Impact: projectiles, shielding, asteroids
- [x] `src/lab/impact/`: explicit Lagrangian solid dynamics at large strain with Johnson-Cook plasticity,
      Mie-Gruneisen equation of state, element erosion and contact; smoothed particle hydrodynamics for
      hypervelocity fragmentation (`sph.c`, 2026-09-26, verified S1 to S3); a second material with read sources
      (6061-T6 aluminium)
- [x] verification: elastic wave speed in a bar, rigid-wall impact momentum and energy, Hugoniot jump
- [x] pictures: Taylor anvil test with the deformed mesh, sphere through a plate, a hypervelocity Whipple shield and a
      laboratory crater with particles (2026-09-26); a crater in regolith waits for a granular material with sources
- [ ] flags: Taylor impact final shape (published tests); crater size scaling from laboratory impacts

### G6. Orbital dynamics
- [x] `src/lab/orbit/`: N-body with a Gauss-Legendre integrator of order 12 (symplectic), adaptive for encounters,
      first post-Newtonian correction
- [x] verification: Kepler two-body closed form, energy and angular momentum over long runs
- [x] pictures: an asteroid's close approach to Earth (Apophis 2029, against JPL)
- [x] flags: planet positions against the JPL ephemeris (Mercury's advance is verification O2)

### G7. Electromagnetism
- [x] `src/lab/em/`: 3D finite-difference time-domain Maxwell (Yee), convolutional PML, total-field
      scattered-field plane wave, scattered power (a near-to-far-field transform: open)
- [x] verification: Mie scattering of a dielectric sphere, cavity resonances
- [x] pictures: a plane wave scattering off a body
- [ ] flag: a measured radar cross section or resonance from a controlled experiment

### G8. Flow against photographs
- [x] the flow domain verified against the Schaefer-Turek benchmark (`tools/flowtest.c`); the test found a defect in
      the app's lattice (wall links across a periodic seam were missing) and it is fixed
- [ ] cylinder at Re below 1, 26 and 105; aerofoil at Re 5300 at 0 to 20 degrees: set up, film, side by side
      with the photographs' description, quantitative flags (recirculation length, Strouhal number, drag)
- [~] streaklines (dye released at points, as in the photographs: done, Re 26 and 105 in the README), volumetric
      streamlines through the whole body (`streamlines volume` in the tunnel; its screenshot waits for a free machine)
- [ ] show the lattice resolution and, where refinement exists, the refined regions

### G9. The native app shows every lab result
- [x] a Lab workspace that opens any `labio` result, plays it, draws the mesh and the refinement levels (`lab open`,
      `lab run` in the app's terminal; drag and wheel checked through the injected input path)

### G10. The front page
- [x] README rewritten in plain words: what this is, the hero picture, the rankings with a podium, the showcase of
      every domain, how a person or an agent runs it, how a fork enters
- [ ] every new document linked from AGENTS.md and the map

### G11. Industrial applications (owner, 2026-09-25: "add implementations for all these fields")
Each application is a scenario built on the modules above, with a picture, a stated set-up and, where a published
measurement exists, a flag. The physics each one needs, and where it comes from:
- [x] a 2D heat-and-flow solver for six of them (`src/lab/heat`: lattice flow with buoyancy, fans, porous media and
      a Smagorinsky option; conjugate heat transfer; a passive scalar), verified H1 to H4 (2026-09-26)
- [x] rotating bodies (rotors with blades in `src/lab/heat`: moving walls, torque and power), verified on
      Taylor-Couette flow (H5, 2026-09-26); needed by fan, pump, turbomachinery and ducted fan
- [ ] Chip cooling: conjugate heat transfer through a cold plate with serpentine channel (flow + solid conduction)
- [x] Data centre cooling: rack rows, hot and cold aisles, buoyant air (flow with a temperature field) (2026-09-26: in 3D
      on the room engine, `examples/lab/data_centre_3d.json`, [docs/lab/rooms.md](docs/lab/rooms.md))
- [ ] Heat exchanger: shell and tube, two streams through a conducting wall
- [ ] Gas turbine: combustor or turbine passage, compressible flow with heat release
- [ ] Centrifugal pump: rotating impeller in a volute, head against flow
- [ ] HVAC fan: axial fan, rotating blades, the wake and the flow rate
- [~] HVAC rightsizing: a room with supply and return vents, comfort temperature and air age (2026-09-26: the office in
      3D, `examples/lab/office_hvac_3d.json`; open: the air's age and a comfort index)
- [ ] Microfluidic chip: low Reynolds serpentine mixer, species concentration
- [ ] Seat climate: ventilated seat, air through a porous cushion and heat to the occupant
- [~] Electric motor: magnetic field of stator and rotor (2D magnetostatics, `src/lab/magnet`, verified M1 to M3; the
      12-slot motor's torque sweep), losses into heat (copper loss computed; the coupling to the heat solver open)
- [ ] Vehicle aero: a car body in the tunnel, drag, the wake
- [ ] Golf ball: dimpled sphere, drag crisis against a smooth sphere
- [ ] Turbomachinery: a compressor or turbine stage, rotating
- [ ] Thruster nozzle: converging-diverging nozzle, supersonic exhaust, the shock diamonds (gas solver)
- [ ] Ducted-fan blade: rotor in a duct, thrust

### G12. One world: every simulation in 3D, one aesthetic (owner, 2026-09-26)
The owner's direction: reality happens in 3D, so every experiment runs in a 3D viewer; all simulations share one
aesthetic and feel like parts of the same physics lab, not separate programs that switch colours or switch between
2D and 3D; they are accurate and beautiful, and they show what is normally invisible (stress concentrations, flux
lines, shock surfaces, heat flow) where it helps the viewer understand.
- [x] a shared style (`src/lab/style.h`, the `lab`, `lab-signed` and `lab-solid` maps in `src/render/swrender.c`): one
      palette family chosen from the data (signed or magnitude), solids starting at their own metal, one room
      (background, floor with grid and shadow, key light); used by the films and by the app alike (2026-09-26)
- [x] 2D results placed in 3D: a planar section drawn as a slab in the room with its bodies extruded through it;
      axisymmetric bodies revolved into solids rising from the cut plane (the capsule becomes a capsule); `--view flat`
      keeps the old picture on request. Superseded by the owner's 3D-first rule (2026-09-26): 2D results are sections,
      shown as sections; every flagship gets a 3D-native scenario (motor, capsule, standing cylinder, sphere, wing done)
- [ ] every library scenario reviewed for 2D thinking: the 2D ones grouped as sections for reference, a 3D-native
      scenario for each phenomenon (radar off an aircraft, industrial heat and air in 3D, the photographs' cylinder).
      Heat and air in 3D done 2026-09-26: the fire solver's room engine (walls, obstacles, vents, openings, fans, heat
      sources, sealed rooms; roomtest A to E against Shah and London's duct, Tric's heated cube at Ra 1e4 and 1e5 and
      the first law), an office, a data hall and a room fire in 3D; the 2D room and aisle left the library
      ([docs/lab/rooms.md](docs/lab/rooms.md)). Open: the other 2D heat scenarios (cold plate, heat exchanger, seat)
- [ ] the app renders lab results on the GPU in the same scene as the tunnel, orbitable at full frame rate
- [ ] augmented views, the same controls for every domain: streamlines and flux lines, isosurfaces (shock surfaces,
      isotherms), probes with values, labels at the place of interest (largest stress, standoff distance)
- [x] every README film re-made in the shared style (`tools/showcase/lab_films.sh`, a new hero, 2026-09-26; the
      Apophis film kept, it is in space)

### G13. Flagship phenomena (owner, 2026-09-26: "implement them well, to a high quality, state of the art")
Each one a verified capability (criterion before the first run), a scenario, a film in the shared style.
- [x] a glass bottle shatters on concrete: brittle fracture with crack nucleation and branching (2026-09-26: bond-based
      peridynamics, peritest P1 to P4, docs/lab/fracture.md, film); open: the GPU for finer lattices and longer falls
- [x] a bullet or meteorite through a plate: particles with Johnson-Cook and Mie-Gruneisen (Whipple shield, crater);
      fracture as a damage law still open
- [x] cloth and rubber deform: hyperelastic large deformation, membranes and shells (2026-09-26: `src/lab/sheet`,
      neo-Hookean membranes with mid-edge plate bending, contact and friction; sheettest C1 to C4, a rubber sheet drapes
      over a ball, [docs/lab/sheet.md](docs/lab/sheet.md), film); open: the sheet touching itself, woven cloth's
      anisotropic stiffness
- [~] water sloshes and a wave breaks: free-surface flow (2026-09-26: `src/lab/water`, weakly compressible SPH, wtest W0 to
      W2 on the CPU and the GPU, dam break and sloshing films, a solitary wave breaking on a beach around a pier in 3D on
      the GPU, [docs/lab/water.md](docs/lab/water.md)); open: the comparison with the SPHERIC benchmark 2 measurements (a
      2.5 MB archive at spheric-sph.org, to fetch with the owner's agreement), a plunging breaker (a finer spacing,
      after the GPU's neighbour search is tiled for speed), the GPU's stillness (wtest W1 fails on the GPU by 10 %: its
      single precision breaks the periodic slab's symmetry that the CPU holds; the runs repeat exactly)
- [~] a hypersonic re-entry vehicle: the capsule's shock layer in 3D at its trim angle (2026-09-26, euler3d, E1 to E3,
      docs/lab/gas.md); viscous heating of its wall and the heat soaking into the heat shield (open)
- [~] a flame and a combustion chamber: reacting flow with heat release and species (2026-09-26: `src/lab/fire`, low-Mach
      reacting LES in the manner of FDS on a new multigrid Poisson solver (`src/lab/mg`, poissontest Q1 to Q5); a 100 kW
      methane fire: firetest F1 to F3 and V1 (Heskestad's flame height, -8 %), V3 (McCaffrey's velocity) pass, V2
      (McCaffrey's plume temperature 2.0 m up, +27 % +- 17 %) recorded open; [docs/lab/fire.md](docs/lab/fire.md), film);
      open: V2, radiation as transport, a combustion chamber. A room fire (walls, a doorway, the hot layer) done
      2026-09-26 on the room engine ([docs/lab/rooms.md](docs/lab/rooms.md)). Since the room engine (2026-09-27) the
      plume runs cool and slow (V3 fails); three reruns scatter from -18 % to -40 % where earlier runs gave +15 % to
      +37 %: a two-minute average, overnight, decides between a real shift and an unconverged average
      ([docs/lab/fire.md](docs/lab/fire.md))
- [x] an electric motor in 3D (2026-09-26: MFEM edge elements, end windings, Arkkio torque, spin-up; mag3dtest,
      motor3dtest); in 2D: field and torque; torque into rotor motion and copper losses into heat (2026-09-26,
      `mg_drive.c`, `mthermal.c`, mgtest M5 to M7, film); open: saturation of the steel, iron losses, grid convergence of
      the torque (13 % between 0.33 and 0.25 mm cells)
- [x] induction heating: eddy currents and their losses heating a workpiece (2026-09-26: time-harmonic eddy currents and
      heat on MFEM, sector grids, eddy3dtest I1 to I3, a gear hardened by a ring coil, docs/lab/magnet.md); open: the
      steel's magnetism and properties changing with temperature (two-way coupling)
- [ ] lightning or a plasma discharge: a streamer with charge transport and its field
- [x] a loudspeaker: a voice coil in a magnet field drives a cone that radiates sound (EM, mechanics, acoustics)
      (2026-09-26: Thiele-Small driver coupled to the 3D acoustic field, actest A5 to A7, docs/lab/acoustic.md)
- [x] a battery under heavy load: ion transport, reaction and the heat it makes (2026-09-26: `src/lab/battery`, the
      Doyle-Fuller-Newman model of the LG M50 (Chen et al. 2020) with twelve cells in parallel on a cooled plate and 3D
      conduction; battest B1 to B6 and P1 to P4, [docs/lab/battery.md](docs/lab/battery.md), film); open: validation
      against the measured LG M50 discharge, temperature-dependent transport, degradation
- [x] a heart valve or an artery: a flexible structure in a pulsing flow (fluid-structure interaction) (2026-09-26: an
      immersed boundary on the GPU flow engine with the sheet solver, fsitest F1 to F3; a flag flutters at Strouhal
      0.27; a three-leaflet valve in an aortic root opens, jets and closes through a heartbeat, its leaflets sealing by
      contact, [docs/lab/fsi.md](docs/lab/fsi.md)); open: a pressure-driven ventricle and aorta, anisotropic stiffening
      tissue, blood's shear thinning, a validation against a measured valve
- [~] metal additive manufacturing: thermal history and residual stress (existing, validated on cantilevers); melting,
      the melt pool and resolidification in the lab's style (2026-09-26: `src/lab/melt`, the enthalpy method with a
      Gaussian beam, melttest M1 to M3 against Rosenthal and Neumann, three tracks in 316L, [docs/lab/melt.md](docs/lab/melt.md));
      Marangoni flow in the melt done 2026-09-27 (enthalpy-porosity, the surface's thermocapillary stress, melttest M4 to
      M7 against the return-flow closed form, a pool widened and flattened by the flow, film); open: the keyhole and
      the free surface's shape, the powder
- [x] an orbital system (Apophis, the planets against JPL)
- [x] light bends around a black hole: geodesics in the Schwarzschild metric, a lensed sky and an accretion disk
      (2026-09-26: `src/lab/relativity`, rttest R1 to R4, film, [docs/lab/relativity.md](docs/lab/relativity.md))
- [~] the demonstration set, mundane to extreme: cantilever bends, bottle shatters, water around an obstacle, a wing
      stalls, a radar pulse scatters, a speaker sounds, a motor heats while running, metal melts and resolidifies, a
      spacecraft re-enters, light bends around a black hole (2026-09-26 done: water around an obstacle (cylinder films,
      dam break over a block), a wing stalls, a radar pulse scatters, a motor heats while running, light bends around a
      black hole)

### G15. The thirteen flags (owner, 2026-09-27: "the entire github repo should be based around these flags")
The repository is built around thirteen flags, measurements of the real world that together cover the main branches of
physics, like stones that together let a simulator simulate most of the world: eight flags, two semi holy grails whose
answers nature reveals on a known date, and three holy grails beyond the reach of today's simulators. The board credits
the person who captures a flag, the open-source libraries and the AI model; the reward is the ranking itself. Front
page: [README.md](README.md); rules: [flags/README.md](flags/README.md).
- [x] the repository reorganised around the flags (2026-09-27): the front page (the flags, their boards, the hall of
      fame, the message to people and agents to use the repository instead of rebuilding physics, the star), a page
      and a card for every flag, `tools/flags.py board` drawing every board from `flags/attempts.jsonl`, credit to the
      challenger, the libraries and the AI model, attempts and difficulty; each flag a gem with its own animated glyph
      and colour; `tools/flags.py board --check` in CI and `make check`, so no board is edited by hand; the lab's
      gallery moved to [docs/LAB.md](docs/LAB.md); [docs/map/PHYSICS.md](docs/map/PHYSICS.md) and [llms.txt](llms.txt)
      for agents
- [ ] the four trials (once called warm-ups) rebuilt natively in 3D, as beautiful and accurate as before, and re-entered: vortex shedding
      behind a cylinder (the 3D wake), the bow shock ahead of a sphere (3D), a shock through a sphere of helium (3D),
      the planets (already 3D in their physics: the pictures and page)
- [ ] each flag opened: its cases, outputs and units in flag.json and its measurements sealed (each dataset
      downloaded only with the owner's agreement, its source and size stated first): 01 car wake, 02 FIRE II,
      03 Flame D, 04 splash, 05 AM-Bench, 06 Sandia fracture, 07 NASA almond, 08 streamer, 11 JET, 12 Otis,
      13 heartbeat
- [ ] the semi holy grails' questions fixed and our predictions registered before their deadlines: 10 the corona,
      before 2 August 2027; 09 Apophis, before 13 April 2029
- [ ] our first attempt at every flag, scored and on its board, with its film

### G14. The app: two modes (owner, 2026-09-26)
- [x] Agent mode: a clean browser of past projects and results, and a chat box that changes and runs simulations
      (2026-09-26: "Ask the lab" hands the question to the user's own Claude Code or Codex with the lab's context and
      commands; every result written to ~/NAVIER-Projects/lab opens in the window; RECENT RESULTS reopens one)
- [~] Advanced mode, simplified: a minimal panel for people to play with the world themselves (playback, the few
      parameters that matter for the open simulation, switching between simulations), far fewer buttons than today.
      Done for lab simulations (2026-09-26): state, field chips, four views, a scenario's own knobs (`controls`) with
      RUN WITH THESE, a library of every scenario, a playback bar with a scrubber; the top bar has two modes only.
      Knobs declared in the shipped scenarios (tools/lab_scenarios.py CONTROLS); `uicheck.py --lab` clicks the whole
      loop. The library groups scenarios by domain under plain names with one domain open at a time, so it fits
      the panel however many there are (2026-09-26, clicked by `uicheck.py --lab`). Open: the tunnel's own crowded
      toolbar
- [ ] switching and modifying simulations is quick and fun: every lab scenario opens in the same world

## Log

| Date | Goal | What happened | Commit |
|---|---|---|---|
| 2026-09-25 | all | brief received, goals written | |
| 2026-09-25 | G1, G2, G3 | lab format and films; flags with sealed answers; compressible AMR solver, capsule film | 3e2c5a1, 67cd786 |
| 2026-09-25 | G3 | cut cells after two wall defects found on the capsule; G8 entropy test | d7c390f |
| 2026-09-25 | G4, G5 | acoustics verified, concert hall; impact solver verified, Taylor and plate films | c71aaa6 |
| 2026-09-25 | G6, G7, G2 | orbits (Apophis against JPL), electromagnetism (Mie), second flag | 08143ac |
| 2026-09-25 | G10, G8, G2 | README rewritten, rankings on the front page, third flag, flow domain | 8f06673 |
| 2026-09-26 | G9, G5, G8, G3, G11 | app lab viewer; particles for hypervelocity; flow verified (lattice defect fixed); fourth flag; heat and flow; rotors; magnetostatics | 20caa03 to 192c462 |
| 2026-09-26 | all | merged to main (pull request 5); the owner's new direction written as G12 to G14 | 66f1ba5 |
| 2026-09-26 | G13 | light near a black hole: geodesic tracer verified (R1 to R4), lensed disk and sky, film; pictures in the viewer | d0a0d54 |
| 2026-09-26 | G13, G8 | water with a free surface (SPH, W0 to W2), dam-break film; a wing stalls (NACA 0012 at 10 and 20 degrees) | 5ab1510 |
| 2026-09-26 | G13, G14 | a motor spins up and heats (M5 to M7); the app's library by domain, one open at a time | |

### G12/G14 integration work on lab/3d-native

- Retained native GPU path for existing computed 3D points and unstructured cells; camera-only changes reuse uploaded
  geometry and one cached frame. Planar results keep the software reference path; no extrusion is claimed as 3D physics.
- Native SECTION axis buttons and position slider; cell-centroid cuts expose hex interior faces with their stored
  fields. A fixed playback camera and FIT avoid reframing the object at every stored time.
- Lighting and sphere glyphs are visual presentation, documented in docs/lab/README.md. Native app screenshots are
  the GPU film source. The portable labfilm utility remains a separate software reference path for now.
- M2 observation on a 5,200-particle dam-break result, 60 identical camera positions: CPU 316.705 ms/draw, GPU
  4.910 ms/draw including GPU completion; zero reads/uploads during GPU orbit. Concurrent laptop work limits comparison.
  Native UI checks: 11 passed, 0 failed. make test-fast passed, including the new geometry test and 366 MCP checks; G12 remains in progress.

- Reproducible native film and before/after images: `tools/showcase/retained_3d.py`, linked in docs/lab/README.md.
  No solver equations changed. Water remains particle glyphs, not a reconstructed liquid surface.
- Next architectural gap: the 3D EM solver currently exports a fixed slice, and labview skips volumetric blocks.
  Exporting the computed volume and rendering it with movable sections is separate work; planar gas, flow and motor
  solvers still need explicit 3D extensions and verification. G12 remains in progress.
- A second actual result was inspected in the native renderer: 11,520-element copper Taylor impact, including its
  interior plastic strain. SECTION now includes FLIP directly in the panel.

### G12/G14 volume increment (saved checkpoint)

- Previous milestone's detached `make test` completed successfully (exit 0).
- Full EM volume export, retained 3D textures, native ray marching and movable cut planes implemented. No Maxwell
  update equations changed. New total/scattered quantities remove the TF/SF storage discontinuity from presentation.
- New criteria passed: 15,360 legacy scalar samples agree exactly across 16 slices; empty-domain total reconstruction
  error 1.87e-8 V/m and scattered peak 2.22e-16 V/m, both below the predeclared 1e-5 V/m bound.
- Native first volume capture and 30-view section benchmark succeeded (3.048 ms/draw including GPU completion).
  This is a checkpoint: final volume UI tests, visual review, film and the required test-fast rerun remain pending.
- Next: continuous water surface reconstructed from particle samples; preserve particle inspection mode and label
  reconstruction/lighting choices. The overall G12/G14 mission remains in progress.

### G12/G14 water-surface checkpoint

- Continuous water display implemented from stored particles with normalized Wendland coverage; particle mode stays
  available. Sparse spray samples remain visible. No fluid equations changed. Reconstruction and studio lighting are
  explicitly described in docs/lab/README.md; this is not a measured interface or an optics solve.
- Reconstruction criteria passed: reference-volume error 7.232e-5, interior coverage 0.999928, constant scalar error
  below 1e-5. Native combined volume/water/geometry UI checks passed 16/16, including reversible mode switching.
- Measured 5,200-particle water: 6.931 ms/cached view; 24.412 ms/changing frame including read, reconstruction, upload,
  draw and GPU completion. Native stills at /tmp/water-surface.png and /tmp/water-surface-section.png.
- The volume milestone test-fast is running in /tmp/volume-test-fast.log. Water's final regression rerun and updated
  native films remain to do. Save this checkpoint before further presentation and performance work.

### G12/G14 saved native films and owner policy update

- Merged lab/physics-lab at 2916c61, including revised rule 1 and docs/THIRD_PARTY.md. No dependency added yet.
- Native volume sections now reuse textures; water section changes reuse reconstructed fields. Expanded UI checks
  passed 17/17. Captured 72 water frames and 106 radar frames through the native renderer; films linked in docs/lab/README.md.
- Visually inspected both posters: continuous water is improved; radar material surface still looks rough and needs
  refinement. No claim of finished game-quality rendering. New solvers and third-party numerical code must retain
  double precision, no fast-math, and independently verified criteria.
- The volume test-fast run remains in progress in /tmp/volume-test-fast.log. Final water regression rerun remains pending.

### G12/G14 complete acoustic volume export

- Owner requests genuine 3D across all simulations. Acoustic computation was already 3D; now its default export
  preserves the complete pressure volume for the native viewer, instead of fixed slices. Legacy export remains optional.
- Predeclared export identity passed: 9,261 pressure samples match slices exactly, with nonzero variation in z.
- Previous volume test-fast reached ALL MCP PROTOCOL TESTS PASSED (366/366). A fresh regression run covers water
  and acoustic changes; see /tmp/native-3d-regression.log and .exit. Native acoustic visual inspection remains pending.
- Still outstanding: genuinely 3D flow, heat/convection and magnet/motor formulations, their independent verification,
  and consistent interactive presentation across every domain. Do not represent planar results as 3D by extrusion.

### G12/G14 true 3D thermal foundation

- Added double-precision conservative 3D conduction, sourced scenario inputs, all six insulating boundaries and
  full-volume native output. This does not yet implement 3D convection, fans or motor coupling.
- Preregistered test passed: cosine-product L2 errors 0.000916427484 (16 cubed), 0.000229217106 (32 cubed),
  ratio 0.25012; heterogeneous energy drift 1.38262864e-15 with no new extrema.
- Real 48x32x24 thermal run: 41 frames, 360 steps, 0.15 s reported wall time. Native sound and thermal captures
  inspected; unsigned scalar opacity corrected to reference the selected range minimum.
- New reproducible scenarios: examples/lab/heat_volume.json and examples/lab/sound_volume.json.
- Required regression rerun and native UI checks are being completed; do not mark overall 3D mission done.

- Native UI regression after the thermal opacity fix passed 17/17; screenshots are linked in docs/lab/README.md.

### G12/G14 three-dimensional fluid core

- Double-precision D3Q19 periodic flow added without replacing the existing verified planar cases. No fast-math.
- Preregistered uniform-flow, three-axis Beltrami decay and mass criteria passed: L2 1.4694% at 16 cubed,
  0.3682% at 32 cubed, mass drift below 3e-14. Full velocity and vorticity exported and inspected in the native app.
- Example 32 cubed, 200 steps, 41 frames completed in 1.00 s reported wall time.
- This checkpoint has no bodies or inlet/outlet boundaries; those require independent checks before claiming wings.

### G12/G14 finite-span wing checkpoint

- Added stationary voxel bounce-back and Guo body forcing to the double-precision 3D flow core.
- New criteria passed: Poiseuille velocity L2 0.00712008175, transverse velocity 4.65e-16, mass drift 1.67e-13.
- Finite-wing integration fixture exports a 24 cubed result with spanwise velocity peak 0.00852042343 m/s.
  This guards dimensionality only; no wing-force validation claimed.
- Ran 64 cubed finite wing at 20 degrees, 600 steps, 41 frames, 25.79 s. Inspected native vorticity and spanwise
  velocity captures. Cached orbit measured 5.548 ms/draw, 0 reads/uploads. Coarse voxel surface is visibly rough.
- Still required: isolated inlet/outlet boundaries, refined geometry and force verification; 3D convection, full 3D
  compressible bodies and motor fields remain outstanding. Overall checkpoints are not complete.

### G12/G14 three-dimensional compressible foundation

- Added double-precision periodic 3D Euler with MC/MUSCL, SSP-RK2 and HLLC.
- Recorded intermediate failures in tools/gas3dtest.c; no acceptance bound loosened. Final wave errors
  0.9956% / 0.2992% (24/48 cubed); Sod central density L1 .0242382 / .0164828 (32/64 cubed).
- Native full pressure volume and movable section inspected. New reproducible case examples/lab/gas_volume.json.
- No 3D capsule, cut cells or AMR yet; the original gas solver remains available and verified separately.
- Next research: pinned BSD MFEM for 3D electromagnetic finite elements, before extending the motor.

### Architect's review of lab/3d-native and the next step (2026-09-26)

- lab/3d-native (ChatGPT, ten commits) reviewed and merged by fast-forward: a retained OpenGL renderer, volume ray
  marching with sections, a water surface reconstructed from particles, full-volume EM and acoustic export, and new
  3D foundations (periodic D3Q19 flow with bounce-back bodies, periodic 3D Euler with HLLC, 3D conduction), each with
  criteria written first and passing. Its final `make test-fast` passed (exit 0). What it did not reach: inlet and
  outlet boundaries for 3D flow, bodies in 3D gas, 3D motor fields; an MFEM (BSD) magnetostatics backend was left
  uncommitted and unbuilt into the lab.
- The renderer is now one lit scene (docs/lab/README.md): shadows, occlusion, volume and geometry composited by depth,
  a cutaway room for acoustics, a solid look for bodies. Lab UI walk 17/17 (paths with spaces were unquoted before).
- A loudspeaker (G13): Thiele-Small driver coupled to the 3D acoustic field, A5 to A7 passing (A7 after correcting
  the test's peak-for-RMS drive); film docs/media/lab/loudspeaker.gif.
- MFEM (BSD, pinned 4.8) integrated into the build (`make mfem`; the lab links it when built). The motor is now solved
  in 3D (G13, the owner's "motor not simulated at all"): Nedelec elements, windings closed round their end turns as a
  current vector potential, Arkkio torque, a spin-up driven by the 3D torque table; mag3dtest T1 to T4 and motor3dtest
  P1, P2 pass after recorded first-run failures (docs/lab/magnet.md); film docs/media/lab/motor-3d.gif. Material looks
  (copper, magnet poles, steel, aluminium, glass) for parts declared by a result's header.
- A 3D flow engine for bodies in a stream, on the CPU (double) and on the GPU through Metal (single precision with
  shifted populations): TRT or regularised collision, LES, inlet, outlet, sponges, Bouzidi curved walls from signed
  distances, momentum-exchange forces; lbm3dtest L1, L2 (amended openly), L4 pass; 370 to 440 million cell updates a
  second on the M2 against 21 to 26 on its CPU. The Karman street in 3D (Strouhal 0.206), a sphere at Re 300 and a
  finite wing run in 1.5 to 2 minutes; vortex cores drawn as iso-surfaces of Q; bodies meshed from their distance field
  with smooth normals. Open: mode A of the street, a converged sphere at Re 300, a wing resolved enough to validate.
- Owner, 2026-09-26: 3D first, never 2D projected into 3D. The periodic-span Karman street was withdrawn; the flow
  engine gained seeded 3D disturbances (start and inlet) and a no-slip floor; the standing cylinder at Re 1000 shows the
  horseshoe, tip and wake vortices in 3D (docs/media/lab/cylinder-mounted-3d.gif). Every scenario is to be reviewed for
  2D-looking setups (periodic spans, uniform disturbances, extrusions) and redesigned.
- The capsule in 3D (owner's "capsule Mach 6" mismatch): euler3d (MUSCL-HLLC/HLL, ghost-cell bodies) with E1 to E3
  passing after recorded failures and an open amendment; the Apollo-shaped capsule at 25 degrees and Mach 6, drag and
  lift within 4 % of Newtonian theory on the same surface; film docs/media/lab/capsule-3d.gif. Meshed bodies now shade
  smoothly (triangles oriented outward at generation).
- A radar finds an aircraft (the owner's "real radar, bouncing off a plane"): conductors from the shape library in the
  EM solver, wings in any direction, a radar antenna with a range gate, the scattered field from a free-space twin run;
  emtest E5 (image theory, 0.04 %); film docs/media/lab/radar-aircraft.gif.
- Thin sheets (G13 cloth and rubber): `src/lab/sheet`, neo-Hookean membranes, plate bending from each triangle's
  shape operator (mid-edge normals; the discrete-shell hinge and the squared Laplacian were measured and rejected),
  clamped and free edges, Kelvin-Voigt viscosity, contact with friction; sheettest C1 to C4 pass (C2 replaced openly by
  the clamped circular plate after three recorded failures of the strip); a rubber sheet drapes over a ball in a
  minute (docs/media/lab/rubber-sheet.gif). The shape library's distance is now exact near a body (it was the
  bounding box's up to the surface, which contact cannot use); euler3dtest, emtest, peritest and lbm3dtest still pass.
- Metal melting (G13 additive manufacturing): `src/lab/melt`, 3D conduction with melting and solidification by the
  enthalpy method, a Gaussian beam integrated exactly over each cell, tracks with pauses; melttest M1 (Rosenthal,
  within 0.9 %), M2 (Neumann, 0.001 %), M3 (energy, 4e-12) pass on their first run; three 316L tracks with the
  library's Pichler (2020) values in 90 s (docs/media/lab/laser-tracks.gif, and the cut across the tracks). The app
  gained `lab zoom F` and `lab focus X Y Z` to frame small features. Bodies in results are now written with shared
  nodes (the radar aircraft 255k nodes instead of 1.55M, the rubber result 177 MB instead of 1.5 GB).
- Fluid-structure interaction (G13): an immersed boundary on the GPU flow engine (direct forcing iterated, the
  three-point kernel, Guo's forcing split for TRT), a mass-aware force that keeps light sheets stable and a predictor
  for stiff ones; fsitest F1 (Couette, +1.3 %), F2 (Hasimoto, -3.8 %), F3 (momentum, 0.002 %) pass after F3's recorded
  first failure (the added-mass instability). Any 3D flow scenario takes a `sheet`; a cloth flag flutters at 6.2 Hz,
  Strouhal 0.27 (docs/media/lab/flag-3d.gif). A fabric look for cloth.
- A heart valve (G13): sheets touch themselves and each other (a spatial hash and a damped penalty; sheettest C5, C6),
  a pulsing inflow, a pressure outlet that takes backflow (lbm3dtest L5, both engines), pipe and aortic-root shapes,
  cutaway drawing, a tissue look, `lab iso LEVEL`. The regularised collision gave the immersed boundary's force only
  partly (j + F (3/2 - omega/2)); fixed and checked by fsitest F1R at relaxation time 0.55; the flag was remade (6.2 Hz,
  Strouhal 0.27). In a straight tube the valve never closed; with sinuses it closes into the Y-shaped seal
  (docs/media/lab/heart-valve.gif).
- Water on the GPU (G13 a wave breaks): the SPH solver ported to Metal (a GPU sort into cells every step, the step
  computed on the GPU, compensated single precision); wtest W0 to W2 on both engines; W1's stillness fails on the GPU
  by 10 % (recorded; its runs repeat bit for bit since each cell's particles are kept in order). Walls and water shaped by distance functions, a beach, bodies from the shape library, a solitary wave. A
  wave breaks on a beach around a pier in 3D (docs/media/lab/breaking-wave.gif); the dam break runs 6 times faster.
- Fire (G13 a flame): a geometric multigrid for Poisson's equation with constant or face coefficients (second order,
  11 V-cycles to 1e-10, a sevenfold coefficient jump in 13), and a low-Mach reacting flow solver after FDS: variable-
  density projection, mixing-limited combustion, temperature-dependent heat capacities from NIST-JANAF, LES, open
  boundaries. A 100 kW methane fire: flame height within 8 % of Heskestad, plume velocity within 16 % of McCaffrey, plume
  temperature 2.0 m up 27 % hot (recorded open). The runs that shaped it are in firetest.c (constant heat capacity 30 %
  hot, open faces that grew a wind, a first step that emptied a cell).
- Rooms in 3D (G12 heat and air, G13 a room fire): the fire solver became the engine for rooms: walls per side (no
  slip, free slip, at a temperature), obstacles, vents, openings, fans, heat sources, sealed rooms whose pressure rises.
  roomtest A to E: Shah and London's square duct (-0.60 %, -0.17 %), Tric's heated cube at Ra 1e4 and 1e5 (+1.24 %,
  +1.14 %), a ventilated room's energy balance, the first law in a sealed room, a fan's flow. The pressure solve is now
  conjugate gradients preconditioned by the V-cycle (multigrid alone stalled behind a doorway), its prolongation taking
  each boundary face's own ghost. An office, a data hall and a fire in a room in 3D, with films; the 2D room and aisle
  left the library ([docs/lab/rooms.md](docs/lab/rooms.md)).
- A battery under load (G13): the Doyle-Fuller-Newman model of a lithium-ion cell (particles in shells eliminated
  before Newton's method), the LG M50's published parameters, twelve cells in parallel on a liquid-cooled plate with
  3D conduction and a warming coolant. battest: Crank's sphere (second order), lithium and charge conserved to 1e-14,
  the heat identity to 1e-14, the capacity within 1.5 % of nominal, the module's energy balance, the parallel split and
  its symmetry. A module at 2C: the cell with a poor weld delivers less early, more late, and ends hottest.
- Marangoni flow in the melt (G13 metal AM): incompressible flow in the liquid on the melt solver's grid, the solid held
  by a Carman-Kozeny drag and closed to the projection, the Marangoni stress on the flat surface, heat carried by the
  flow. melttest M4 (the thermocapillary return flow, 0.19 %), M5 energy, M6 divergence, M7 the pool wider and
  shallower. The projection solved in a window around the pool runs sixty times faster than over the block.
- The thirteen flags (G15): the repository rebuilt around them on the owner's direction. Eight flags (the Ahmed body,
  FIRE II, Sandia Flame D, the splashing drop, AM-Bench, the Sandia Fracture Challenge, the NASA almond, the positive
  streamer), two semi holy grails (Apophis in 2029, the corona of the eclipse of 2027) and three holy grails (a JET
  fusion shot, Hurricane Otis, a human heartbeat), each with its page, card and board; the hall of fame; credit to the
  person, the libraries and the AI model; the front page, AGENTS.md, llms.txt and the physics map tell people and
  agents to use the repository rather than rebuild the physics.

### G16. Printing foundation and consolidated app (owner, 2026-10-03)

- Current checkout has independent nozzle work in progress. Changes for this goal use the isolated
  `improvement/printing-foundation` branch and do not overwrite that work.
- Printing priority: improve numerical correctness of LPBF and FDM/FFF before extending claims; keep material and
  process provenance, verify against independent analytic answers, and preserve explicit model limitations.
- Mesh priority: retain deterministic body classification, protect finite/grid bounds, report geometry fidelity for
  anisotropic layer meshes, and measure optimization on unchanged geometry.
- App priority: one native application, clear Manual and Agentic modes, all-domain scenario navigation, restored
  scenario controls and usable playback, verified through real clicks and before/after screenshots.
- Plugin priority: transport the existing C MCP operations over bounded local Streamable HTTP, with explicit impact
  hints and structured output schemas. Public deployment and ChatGPT model integration are separate acceptance steps.
- Research-quality evidence requires spatial/time refinement and independent physical measurements for the stated
  process. Passing analytic regressions is numerical verification, not new experimental validation.
- Visual quality is an acceptance requirement: clear shape, consistent result lighting, readable quantitative
  legends and smooth exploration. Decorative lighting is documented; no generated image substitutes for a result.
- Refinement must hold the physical setup and simulation layer thickness fixed. A separate three-mesh and
  three-time-step study reports solution changes and conservation, without treating self-convergence as physical
  validation or a bound on constitutive-model error. Criteria are recorded before the first run.

Checkpoints completed on this branch:

- `973ef54`: mesh ownership and diagnostics. Mesh suite 52/52, operations 279/279, independent ASan/UBSan 52/52.
  Eight coincident bodies, unchanged 64,000 elements: median construction 0.224780 to 0.012112 s (18.6 times faster).
  The 25,000-element anisotropic fixture has 25 complete layers and minimum det J 0.005 mm3.
- `9bf44fd`: conservative FDM deposition and explicit full-print heat ledger; exact piecewise thermoelastic integration;
  mean Gauss-point von Mises; accumulated LPBF reactions; active-node extrema after support removal. Printing 35/35.
  Independent constant/table-capacity/support-removal energy errors: 1.85e-16, 4.93e-16, 3.08e-15 relative.
  The demonstration wall accounts for 1500.57176 J previously omitted and closes its full heat ledger to 1.63e-11.
- `0047162`: Manual/Agentic navigation, separate paged library, scenario control restoration and result playback.
  A further real-key-path check covers playback and section shortcuts without mutating the hidden tunnel/FEM state.
- Local HTTP transport: 190 assertions passed, including a real FDM job retained after session deletion.
  Native stdio MCP: 518 assertions passed. Actual ChatGPT use and public hosting have not been tested.
- A fresh build without optional MFEM now reports missing 3D magnetics instead of failing to link. Generated backend
  configuration rebuilds the dispatcher when the dependency is added or removed.

- `82a03b8`, `460fcde`: active-result keyboard routing, Manual terminal collapse, quantitative CLEAN VIEW and honest
  printing workflow labels. Final lab controls: 38/38 through real input.
- `e14aefc`: active FE boundaries prevent smooth STL facets from spanning missing material or hiding generated
  supports/plates. FIT spans all stored times and eligible source geometry. Renderer 69/69, default UI 132/132,
  real printing view 19/19. Final 24,389-element rebuild mean 2.554 ms, maximum 5.288 ms over twelve rebuilds.
- `7f359e1`, `e7436a6`: [portable native demo](tools/demo_printing.py) and
  [view evidence](docs/media/foundation/evidence.json), including [FDM replay](docs/media/foundation/fdm-replay.gif),
  [plastic view](docs/media/foundation/fdm-clean.png) and [metal section](docs/media/foundation/lpbf-section.png).
  Both demonstrations have 1,124 elements; process/material inputs are inferred/demonstration, no measurements.
- `dabec98`: export FDM residuals at release and the last actual solve, using the existing eigenstrain RHS force scale.
  Printing checks 40/40, complete FDM workflow 88/88. Independent hex: residuals 1.34e-16 on bed, 2.49e-16 released.
- `71f5d0c`: [independent mesh/time study](tools/printing_refinement.py), eight real MCP jobs on a fixed 8 x 2 x 4 mm wall
  and fixed 1 mm simulation layers. Finest changes: LPBF 2.4583%, FDM mesh 2.5468%, FDM thermal substeps 0.38339%.
  LPBF is nonmonotone; no order or experimental accuracy is claimed. Maximum residual 8.93e-11, independently
  reconstructed FDM whole heat closure 1.53e-11. The
  [initial invalid zero-net-reaction normalization](validation/printing-refinement/initial-invalid-normalization.json)
  and [explicitly amended PASS](validation/printing-refinement/amended.json) are retained without erasing history.
- `e410df9`: eight real `mesh_inspect` calls passed 40 independent checks of current/generated state, exact box
  element/node counts and det J in mm3. The complete [inspector follow-up run](validation/printing-refinement/inspected.json)
  is retained separately from the original and amended studies. This removes the known unexercised operation
  from the coverage baseline.
- Both the app and engine link with the existing cached MFEM 4.8 backend as well as without it. The cache was copied
  into the isolated worktree; the original dependency and checkout were not changed. Fresh MFEM download/build and
  the complete 3D magnetic numerical suite were not rerun here.
- Complete engineering `make test` finished detached with exit 0; base `make test-fast` finished with exit 0.
  The omitted fire/room/FSI executables are now included in that recipe and are being checked separately. Topology
  checks also enter the recipe (25/25 in the completed full suite). The runtime remains above the three-minute goal.
- Quality gates A and D-H passed: zero warnings under the project compiler flags, links/flag boards, operation
  coverage, portable paths, commit hygiene and material provenance (34 sourced records, four demonstration/other
  records). Final HTTP client checks remain 190/190. The full sanitizer gate was not run; the independent mesh
  ASan/UBSan run above passed.
- Presentation 14/14 and topology UI 18/18. The cap-colour check now compares with a matched empty studio instead of
  misclassifying neutral background pixels as a simulated face; its original contrast criterion is unchanged.
- `9947599`: final scientific review caught the same near-zero-force normalization defect in the optional J2 path. A freely
  contracting hex reported 0.854 despite matching its affine closed form. The accepted nonlinear residual now uses
  applied/release forces, individual incremental reactions and the initial nonlinear predictor as its force scale.
  It reports 6.06e-16 with unchanged displacement/stress/strain; printing checks pass 44/44 and LPBF MCP 239/239. The exported result
  names the elastic or J2 definition, and both retain an explicit absolute-N fallback for a zero force scale.
- FDM scope now explicitly names its incremental (hypoelastic) approximation: old stress is not rescaled as E(T)
  changes, with an instantaneous reset above T_relax. The exact thermal integral verifies that implemented law;
  time-dependent polymer viscoelasticity remains a separate, required constitutive upgrade. FDM MCP checks pass 89/89.
- `86f0701`: real Solid keyboard checks fail before the routing fix (3 passed, five failed) and pass afterwards (8/8); Space
  controls visible stored-time FEM playback without starting hidden fluid. CLEAN VIEW labels section axis, position
  and flip. The two-mesh interface and report say sensitivity, not proof of convergence or an accuracy bound.
- `2bd014b`: long lab validation retains its original cases and thresholds in `make test-lab-validation`, run detached.
  Session subsets explicitly omit statistical/steady validations: fire F1/F2 (36.29 s, relative projection 1.15e-10,
  mass 1.45e-13), room D (7.42 s, pressure error -0.411%, relative mass 2.00e-15), FSI F3 (14.40 s with native GPU,
  unchanged 2000 steps, momentum change +0.002%). The [full fire run](validation/lab/fire-2026-10-03.log) confirms
  the existing open plume issue: F1/F2/F3/V1 pass, V2 at 2.6 m is -27.5 percent and V3 at 2.6 m is -38.4 percent
  against the unchanged 25 percent criterion. It exits 1 after 3178 s under concurrent load. Room/FSI full cases
  were not reached by that failing wrapper; their session subsets above are the only new results claimed here.
- A further independent review exposed face-mean radiation cooling errors of 8.16 and 9.75 percent on nonuniform
  temperatures. Radiation now integrates its nonlinear flux, Newton tangent and ledger with 3 x 3 face quadrature.
  [Before](validation/printing-refinement/radiation-before.log): three pass, six fail; [after](validation/printing-refinement/radiation-after.log):
  ten pass. Flux errors are at most 1.20e-15, tangent finite-difference error 3.47e-10, manufactured temperature
  error 1.14e-13 K. Theta = 0.5 gives 0.719781097868 J against the independent polynomial integral; the complete
  thermal suite passes 82/82. These are planar-face integration checks, not a cure for staircase STL boundary area.
- Removed the unproved general local stress upper bound from the FDM summary, schema, panel and report. Neglecting
  creep alone does not prove a bound under temperature-dependent stiffness and heterogeneous stress redistribution.
  The new scope regressions [fail before](validation/printing-refinement/stress-scope-before.log) (88 pass, two fail)
  and [pass after](validation/printing-refinement/stress-scope-after.log) (90/90); historical results retain a correction note.
- [Post-radiation refinement](validation/printing-refinement/post-radiation.json): the same eight real jobs and all
  40 mesh-inspector checks pass without altered inputs or criteria. Finest changes: LPBF 2.4583153 percent,
  FDM space 2.5474215 percent, FDM time 0.3831007 percent. LPBF remains nonmonotone; these are sensitivities.
- Recomputed and captured both native open-cell demonstrations after that cooling correction, retaining the
  [previous numerical capture](validation/printing-refinement/native-before-radiation.json). Current FDM has 27
  stored states, released peak 1.54044 MPa and z warp [-0.0507017, 0.00107592] mm; LPBF has 12 states and its
  elastic 690.734 MPa peak is unchanged. [Current evidence](docs/media/foundation/evidence.json) records source,
  engine, renderer, result and media hashes. The 27-frame 960 x 600 native film is 5680 ms. No new measurement validation.

Remaining research work: conforming printing meshes and documented spatial/time convergence; FDM bead/toolpath,
raster anisotropy, interlayer bonding and viscoelastic constitutive laws with sourced parameters; connecting the
separate laser/melt model to part-scale mechanical response for LPBF. Measurements and independent print validation
are still required. The manual app exposes the existing scenario/setup controls, not every conceivable solver parameter.
The plugin adapter is local development transport; lab scenarios do not yet have typed MCP job operations, and public
deployment still needs authorization, user/workspace isolation and a real ChatGPT session.

The next mesh acceptance case must include rotated walls: the current aligned box study does not measure staircase
surface-area bias, which also biases convection. Use the same 8 x 2 x 4 mm wall at 0, 30 and 45 degrees and h = 1,
0.5, 0.25 mm, with z-aligned layers. Before running, require positive J, one face-connected part and report volume,
surface area and uniform-convection flux against independent 64 mm3, 112 mm2 and h_film A delta T. A boundary
treatment improvement must bring the finest flux within 2 percent; do not infer that from an aligned-wall PASS.

The next FDM constitutive acceptance starts with a synthetic single-Maxwell-branch material, before fitting any
polymer: E_infinity = 1 GPa, E_1 = 2 GPa, tau = 10 s, held strain 0.001. The material-point response must match
sigma(t) = 0.001 [E_infinity + E_1 exp(-t/tau)] to 1e-10 relative; stored energy plus nonnegative dissipation must
close to 1e-8 relative. Then verify a 3D held coupon and the elastic limit before using sourced polymer parameters.
