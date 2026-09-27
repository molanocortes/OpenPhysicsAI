# The physics map

Up: [AGENTS.md](../../AGENTS.md) · every directory: [MODULES.md](MODULES.md) · the lab's pages: [docs/lab/](../lab/README.md)
· the lab in pictures: [LAB.md](../LAB.md) · the flags: [the front page](../../README.md)

**For a person or an AI agent with a physics task: find the physics below and take the solver.** Each row names what it
computes, the files to read first (their length in lines is what reading them costs), what it was verified against,
the scenarios that run it, and what it does not model. Reading this page costs a few thousand tokens. Each solver on it
took days to write, debug and verify; taking one takes minutes. That is the point of this repository: stand on it
rather than rebuild it.

## Use a solver without reading its code

1. `make lab` builds `build/labrun`, `build/labfilm`, `build/labprobe` and every lab test.
2. Copy the nearest scenario from `examples/lab/` and change it. Every number carries its unit in its key
   (`length_m`, `speed_m_s`, `heat_release_rate_w`); a number without a unit is refused, and so is a material without
   a `source`.
3. `./build/labrun my.json out.lab` runs it and prints a JSON record: the scenario's hash, the steps taken, the wall
   time and the solver's own diagnostics (balances, measured quantities).
4. Look at the result in the app (`lab open out.lab`); take numbers out with `./build/labprobe out.lab --field F --stats`
   or with `tools/labread.py` into NumPy. The format of every result: [src/lab/labio.h](../../src/lab/labio.h) (98 lines).
5. From code: every lab solver is a plain C library, a header with create, step, fields and free, linked into the core
   and callable from C, C++, Swift or Python through a thin binding. Its test in `tools/` is the shortest complete
   example of calling it, and its scenario reader (`src/lab/<domain>/*_scenario.c`) shows every key the JSON accepts.

Structural and thermal analyses of parts (an STL in, stresses and temperatures out) go through the engine's operation
layer instead: typed operations over MCP or `navier-ctl`, described in [analysis.md](../analysis.md) and
[mcp-clients.md](../mcp-clients.md).

## Fluids

| You need | Solver | Read first | Verified by | Run | Does not model |
|---|---|---|---|---|---|
| Incompressible flow past bodies in 3D: wakes, wings, vortex shedding | lattice Boltzmann D3Q19, CPU in double precision or GPU (Metal) in single, curved walls, pressure outlet: `src/lab/flow/lbm3d.h`, `lbm3d_metal.h` | [lbm3d.h](../../src/lab/flow/lbm3d.h) (81), [flow.md](../lab/flow.md) | `tools/lbm3dtest.c` (L1 to L5, every case on both engines) | `cylinder_mounted_3d`, `sphere_wake_3d`, `wing_3d_10` | turbulence closure for walls at high Re, local refinement |
| Periodic 3D flow in a box | D3Q19 BGK: `src/lab/flow/flow3d.h` | [flow3d.h](../../src/lab/flow/flow3d.h) (20) | `tools/flow3dtest.c` (the decaying ABC Beltrami flow) | `flow_volume` | inlets, outlets |
| Flow past a section (2D), the app's real-time tunnel | `src/lbm.c` (D3Q19, recursive regularised collision, Smagorinsky LES), the lab's instruments in `src/lab/flow/flow.h` | [lbm.h](../../src/lbm.h) (128), [flow.h](../../src/lab/flow/flow.h) (65) | `tools/flowtest.c` (Schaefer-Turek: drag +2.0 %, Strouhal +0.2 %) | `cylinder_re105`, `naca0012_re5300_aoa10` | the third dimension (a span of four periodic cells) |
| A flexible sheet in a flow: flags, valves, leaflets | the immersed boundary on the GPU engine with the sheet solver, two-way: `src/lab/fsi` | [fsi.h](../../src/lab/fsi/fsi.h) (28), [fsi.md](../lab/fsi.md) | `tools/fsitest.c` (Couette through an immersed plane; Hasimoto's periodic spheres -3.79 %; momentum 0.002 %) | `flag_3d`, `heart_valve` | blood's shear thinning, active tissue |
| Water with a free surface: dam breaks, sloshing, breaking waves | weakly compressible SPH, CPU or GPU: `src/lab/water` | [water.h](../../src/lab/water/water.h) (96), [water.md](../lab/water.md) | `tools/wtest.c` (hydrostatics 1.65 %, sloshing period 0.21 %; the GPU's stillness case W1 is a recorded failure) | `dam_break`, `breaking_wave`, `sloshing_tank` | air, surface tension |
| Buoyant air, fire and rooms: plumes, ventilation, data halls, fires in rooms | low-Mach variable-density LES after NIST's FDS, mixing-limited combustion, walls, obstacles, vents, openings, fans, heat sources, sealed rooms, the age of the air: `src/lab/fire` | [fire.h](../../src/lab/fire/fire.h) (114), [fire.md](../lab/fire.md), [rooms.md](../lab/rooms.md) | `tools/roomtest.c` (square duct -0.60 %; heated cube Ra 1e4 +1.24 %, Ra 1e5 +1.14 %; the first law -0.41 %; the age of the air -0.29 %), `tools/firetest.c` (Heskestad's flame height; McCaffrey's plume, one check open) | `room_fire_3d`, `office_hvac_3d`, `data_centre_3d`, `methane_fire` | detailed chemistry, radiation as transport, wall functions |
| Poisson's equation on a box, with face coefficients (projections, potentials) | geometric multigrid preconditioning conjugate gradients: `src/lab/mg` | [mg3d.h](../../src/lab/mg/mg3d.h) (32) | `tools/poissontest.c` (second order, 8 to 10 iterations to 1e-10) | used by fire, rooms and melting | anything but a uniform grid |
| Heat carried by flow through channels and solids, in 2D (conjugate heat transfer) | D2Q9 lattice Boltzmann with a finite-volume energy equation: `src/lab/heat/heat.h` | [heat.h](../../src/lab/heat/heat.h) (140), [heat.md](../lab/heat.md) | `tools/hftest.c` (de Vahl Davis cavity +0.24 %, Poiseuille, Shah and London) | `chip_cold_plate`, `heat_exchanger` | the third dimension |

## Shocks and high-speed gas

| You need | Solver | Read first | Verified by | Run | Does not model |
|---|---|---|---|---|---|
| Supersonic and hypersonic flow in 2D or axisymmetric, shocks, two gases, adaptive mesh, bodies | block-structured AMR Euler with cut cells: `src/lab/gas/gas.h` | [gas.h](../../src/lab/gas/gas.h) (118), [gas.md](../lab/gas.md) | `tools/gastest.c` (exact shock tube, oblique shocks, the Taylor-Maccoll cone) | `capsule_mach6`, `double_mach_reflection`, `shock_helium_cylinder` | viscosity, heat at walls, real-gas chemistry |
| The same around bodies in 3D | finite-volume Euler, MUSCL and HLLC, bodies: `src/lab/gas/euler3d.h`; a periodic 3D foundation in `gas3d.h` | [euler3d.h](../../src/lab/gas/euler3d.h) (48) | `tools/euler3dtest.c` (Sod along x, E1 to E3), `tools/gas3dtest.c` (a 3D sound wave, second order) | `capsule_3d` | as above |

## Heat, melting and energy

| You need | Solver | Read first | Verified by | Run | Does not model |
|---|---|---|---|---|---|
| Metal melted by a moving laser, with Marangoni flow in the pool | the enthalpy method, a Gaussian beam, incompressible flow in the liquid with a Carman-Kozeny drag in the mush: `src/lab/melt` | [melt.h](../../src/lab/melt/melt.h) (74), [melt.md](../lab/melt.md) | `tools/melttest.c` (Rosenthal +0.86 %, Neumann -0.001 %, the thermocapillary return flow 0.19 %, energy 6e-15) | `laser_tracks`, `melt_pool_flow` | the keyhole, the powder |
| A lithium-ion cell and a module of cells | the Doyle-Fuller-Newman model (the LG M50's published parameters), cells in parallel, 3D conduction, a cooled plate: `src/lab/battery` | [dfn.h](../../src/lab/battery/dfn.h) (76), [pack.h](../../src/lab/battery/pack.h) (61), [battery.md](../lab/battery.md) | `tools/battest.c` (Crank's sphere, lithium and charge to 1e-14, the heat identity, capacity +1.5 %, the module's energy) | `battery_module` | degradation, temperature-dependent transport |
| Conduction in a 3D block | `src/lab/heat/heat3d.h` | [heat3d.h](../../src/lab/heat/heat3d.h) (17) | `tools/heat3dtest.c` (an exact decaying mode, second order) | `heat_volume` | flow |
| Transient conduction in a part (hex meshes, phase change, time adaptivity) | the engine's thermal solver: `src/fem/thermal.h` | [thermal.h](../../src/fem/thermal.h) (293), [Thermal Sim/README.md](../../Thermal%20Sim/README.md) | `tools/thermtest.c`, `tools/tsteptest.c`, `tools/advtest.c`, `tools/orchtest.c` | the MCP operations | |

## Solids

| You need | Solver | Read first | Verified by | Run | Does not model |
|---|---|---|---|---|---|
| Metals hit at high speed: large plastic strain, shocks, erosion | explicit Lagrangian hexahedra with Johnson-Cook and Mie-Gruneisen: `src/lab/impact/impact.h` | [impact.h](../../src/lab/impact/impact.h) (127), [impact.md](../lab/impact.md) | `tools/imptest.c` (the copper Hugoniot +0.04 %, the Taylor test's energy) | `taylor_copper`, `sphere_plate` | fracture as a damage law |
| Hypervelocity impact that shatters metal into clouds | SPH for solids with the same material laws: `src/lab/impact/sph.h` | [sph.h](../../src/lab/impact/sph.h) (94) | `tools/sphtest.c` (the Hugoniot, the Taylor test against the mesh solver) | `whipple_shield`, `crater_aluminium` | |
| Brittle solids that crack: glass, ceramics | bond-based peridynamics in 3D: `src/lab/fracture` | [peri.h](../../src/lab/fracture/peri.h) (61), [fracture.md](../lab/fracture.md) | `tools/peritest.c` (the elastic energy against the continuum, the fracture energy, a crack below the Rayleigh wave speed) | `glass_bottle` | plasticity, ductile tearing |
| Thin sheets that stretch, bend and touch: rubber, cloth, balloons, leaflets | neo-Hookean membranes with plate bending, contact and friction: `src/lab/sheet` | [sheet.h](../../src/lab/sheet/sheet.h) (65), [sheet.md](../lab/sheet.md) | `tools/sheettest.c` (a balloon, Kirchhoff's clamped plate -0.95 %, contact) | `rubber_sheet` | woven anisotropy |
| Stress in a part: linear and large-deformation statics, explicit dynamics, contact, topology optimisation | the engine's finite elements: `src/fem` (hex8, TET4, TET10) | [solid.h](../../src/fem/solid.h) (117), [nlsolid.h](../../src/fem/nlsolid.h), [explicit.h](../../src/fem/explicit.h) (102), [topopt.h](../../src/fem/topopt.h) (57) | `tools/femtest.c` (patch tests), `tools/tettest.c` (Kirsch -1.33 %), `tools/dyntest.c`, `tools/topotest.c` | the MCP operations, [analysis.md](../analysis.md) | |
| Mechanisms: joints, actuators, sensors, contact, flexible bodies, the FFF print process | the mechanics layer: `src/mech` | [mechsim.h](../../src/mech/mechsim.h) (202), [fffprint.h](../../src/mech/fffprint.h) (161) | `tools/mechtest.c` | [Mech Sim/STATUS.md](../../Mech%20Sim/STATUS.md) | |
| How a laser-printed metal part distorts | the inherent-strain build: `src/ctl/lpbf_build.h`, `src/mech/lpbf.c` | [lpbf_build.h](../../src/ctl/lpbf_build.h) (136) | compared with laboratory measurements at 1.84 % mean error; the measurements are not part of the public repository | [lpbf-build.md](../contracts/lpbf-build.md) | |

## Waves and fields

| You need | Solver | Read first | Verified by | Run | Does not model |
|---|---|---|---|---|---|
| Sound in rooms and around objects; a loudspeaker driving it | the wave equation by finite differences in time, impedance walls; a Thiele-Small driver: `src/lab/acoustic` | [acoustic.h](../../src/lab/acoustic/acoustic.h) (98), [acoustic.md](../lab/acoustic.md) | `tools/actest.c` (room modes 0.01 %, reflection, Eyring's reverberation) | `shoebox_hall`, `loudspeaker` | air absorption, wind |
| Electromagnetic waves: radar, antennas, cavities, scattering | Maxwell's equations on Yee's grid with perfectly matched layers: `src/lab/em` | [em.h](../../src/lab/em/em.h) (86), [em.md](../lab/em.md) | `tools/emtest.c` (a resonant cavity -0.016 %, Mie scattering -0.9 %), `tools/labvoltest.c` | `radar_pulse`, `radar_aircraft` | curved surfaces without staircases, the far field |
| Magnetic fields of motors and actuators in 2D, with the rotor's motion and the heat | magnetostatics of the vector potential, a rotor under its torque, the losses as heat: `src/lab/magnet/magnet.h`, `mg_drive.c`, `mthermal.h` | [magnet.h](../../src/lab/magnet/magnet.h) (77), [magnet.md](../lab/magnet.md) | `tools/mgtest.c` (M1 to M7) | `pm_motor`, `pm_motor_drive` | saturation, iron losses |
| Magnetic fields in 3D; eddy currents and induction heating | MFEM's edge finite elements on cylindrical grids, sparse factorisation by Accelerate: `src/lab/magnet/mag3d.h`, `eddy3d.h`, `magnet3d.h` (MFEM is fetched and built by `tools/build_mfem.py`) | [mag3d.h](../../src/lab/magnet/mag3d.h) (62), [eddy3d.h](../../src/lab/magnet/eddy3d.h) (38) | `tools/mag3dtest.cpp`, `tools/magnet3dtest.cpp`, `tools/motor3dtest.c` (the torque within 3 % of the 2D solver), `tools/eddy3dtest.cpp` (the skin effect, I1 to I3) | `pm_motor_3d_drive`, `induction_gear` | the steel's magnetism changing with temperature |

## Gravity and spacetime

| You need | Solver | Read first | Verified by | Run | Does not model |
|---|---|---|---|---|---|
| Planets, moons, asteroids, spacecraft | N-body with the Sun's first relativistic correction, Gauss-Legendre implicit Runge-Kutta: `src/lab/orbit` | [orbit.h](../../src/lab/orbit/orbit.h) (64), [orbit.md](../lab/orbit.md) | `tools/orbtest.c` (Kepler 5.6e-10, Mercury's 42.98 arcseconds a century) | `apophis_flyby`, `apophis_2029` | extended bodies, tides on them |
| Light near a black hole | null geodesics of the Schwarzschild metric, a thin disk: `src/lab/relativity` | [blackhole.h](../../src/lab/relativity/blackhole.h) (61), [relativity.md](../lab/relativity.md) | `tools/rttest.c` (the photon sphere to 2e-13, redshift to 1.5e-11) | `black_hole` | rotation (Kerr) |

## What is not here yet

Plasma and electric discharges, chemistry beyond one step, turbulence closures for walls at high Reynolds number,
two-phase flow with a sharp interface and surface tension, ductile fracture, the moist atmosphere, cardiac muscle and its
electricity, and the magnetohydrodynamics of stars and fusion plasmas. Each is what one of [the thirteen
flags](../../README.md#the-thirteen-flags) asks for; each flag's page says where to start and which open-source
libraries to stand on.
