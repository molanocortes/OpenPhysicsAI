# Impact: metals hit at high speed (`src/lab/impact`)

A projectile against a plate, a bar against an anvil, a body against a target: large plastic strain at high strain
rate, shock pressures, heating by plastic work, failure. The mesh moves with the metal, so the picture is the
deformed body. Up: [docs/lab/README.md](README.md) · code: [impact.h](../../src/lab/impact/impact.h) · tests:
[tools/imptest.c](../../tools/imptest.c) · materials: [impact_materials.c](../../src/lab/impact/impact_materials.c)

The framing of this module is the published physics of materials testing (the Taylor anvil test), spacecraft
shielding against micrometeoroids and planetary cratering; it does not hold data on weapons or armour.

## Model

- **Elements:** eight-node hexahedra, one-point integration with the uniform gradient (Flanagan and Belytschko
  1981), computed exactly by quadrature of the volume's derivative; viscous hourglass control.
- **Deviator:** hypoelastic with the Jaumann rate, radial return to the Johnson-Cook flow stress (strain
  hardening, strain rate, thermal softening); plastic work heats the element (Taylor-Quinney fraction 0.9).
- **Pressure:** Mie-Gruneisen equation of state referenced to the linear shock Hugoniot us = c0 + s up; the
  pressure work is centred in time and solved with the energy, since the pressure is linear in it.
- **Shocks:** von Neumann-Richtmyer bulk viscosity, linear and quadratic, on the element's smallest dimension.
- **Failure:** erosion when the equivalent plastic strain (or the volumetric strain) passes a limit that the
  scenario must source; the mass stays at the nodes as debris.
- **Contact:** penalty, node against face, between bodies, frictionless; a node only meets a face it faces, faces
  on symmetry planes take no part, and a penetration exposed by erosion is not pushed on. A rigid anvil plane.
- **Not modelled:** friction, damage evolution (Johnson-Cook damage), spall, phase changes, adaptive remeshing.

## Verification (tools/imptest.c, criteria written before the first run)

| Case | Against | Criterion | Result |
|---|---|---|---|
| I1 element | exact volume, gradient identities | 1e-12 | exact |
| I2 elastic bar on the anvil | 2L / c_bar, rebound speed | 5 % | +2.2 %, -4.2 % |
| I3 copper slab at 500 m/s, uniaxial | Hugoniot pressure rho0 (c0 + s u) u, shock speed | 2 % | +0.04 %, -0.5 % |
| I4 Taylor test, OFHC copper, 190 m/s | energy balance, hourglass share | 3 %, 5 % | 0.00 %, 0.8 % |
| I5 two bars head-on | momentum, energy (added, recorded), rebound | 1e-9, 2 %, 10 % | 4e-15, 0.6 %, 5 % |

Defects found by these tests and fixed, on record: the bulk viscosity used the cube root of the volume and was
unstable on flat elements (I3); the anvil counted as lost the velocity its own element gave a resting node, then
the lagged pressure left the energy 3 per cent short (I4); faces on symmetry planes and side faces were taken as
contact surfaces and created 1500 times the energy of a collision (I5, which passed on momentum alone until the
energy criterion was added).

## Particles, for hypervelocity impact (`sph.c`, tools/sphtest.c)

Above a few kilometres per second a projectile and a thin plate shatter into a cloud of fragments and droplets that a
mesh cannot follow. For these, the same bodies are filled with particles (`"method": "sph"` in a scenario): smoothed
particle hydrodynamics for solids (Libersky and Petschek), cubic spline kernel, the symmetric momentum and energy
forms that conserve both pair by pair, Monaghan's artificial viscosity with Balsara's switch, the artificial stress of
Gray, Monaghan and Swift against the tensile instability, a Jaumann rate with the Johnson-Cook return, and Heun's
predictor-corrector. The material laws are the same functions as the mesh solver's (`im_jc_flow`, `im_mg_pressure`).
Symmetry planes are mirror particles; a rigid wall is a mirror that pushes and does not pull.

| Case | Against | Criterion | Result |
|---|---|---|---|
| S1 copper on copper at 500 m/s, uniaxial | the Hugoniot: up = v/2, p = rho0 us up, us = c0 + s up | 2 %, 3 %, 3 % | -0.27 %, -0.61 %, +1.59 % |
| S2 copper sphere at 1 km/s into a plate, quarter | momentum along the impact, total energy | 1e-9, 1 % | 7e-15, +0.001 % |
| S3 Taylor test, OFHC copper, 190 m/s | the hexahedral solver's L/L0 = 0.685 (an independent method) | 3 % | 0.679 (-0.9 %); D/D0 1.92 against 1.91 |

S3 failed its first run: L/L0 0.717 (+4.6 %) and D/D0 1.70, the rod too stiff. Runs that changed one thing each
showed the cause: without the artificial stress, 0.717; with the gradient normalised at free surfaces, 0.719; with half
the viscosity, 0.699. The viscosity acted on every approaching pair, also in the shearing plastic flow of the
mushroom where there is no shock, and took energy that belongs to plastic work. Balsara's switch, which scales it by
the share of compression in the local motion, is the standard remedy; with it the test passes and S1 and S2 are
unchanged. The criterion was not touched; the first result is printed by the test.

Not modelled by the particles: spall and fracture as damage (fragments form where the material separates, not by a
fracture law), phase changes beyond melting (vaporisation at the highest speeds), friction on walls.

## Materials

`ofhc_copper`: Johnson-Cook strength of Johnson and Cook (1983), checked against LANL LA-UR-22-29965; the equation
of state from Steinberg (1996), typed from the table and not yet re-checked. Records for iron, steel and aluminium
were typed from memory and removed before any use, after one of their citations proved wrong; each returns only
with a source that has been read.

`aluminium_6061_t6` (2026-09-26), every value read in its source: density and the ultrasonic shear speed from Marsh,
*LASL Shock Hugoniot Data* (1980), pp. 181-182; the Hugoniot us = 5.349 + 1.338 up km/s fitted here by least squares
to the 25 shock states of that table (7 to 108 GPa); Johnson-Cook strength from Lesuer, Kay and LeBlanc, LLNL
UCRL-JC-134118, Table 1; melting temperature and specific heat from Banerjee and Bhawalkar (2008). The Grueneisen
parameter 2.01 is derived from the Hugoniot slope by Slater's relation, not measured.

## Demonstrations

- [taylor_copper.json](../../examples/lab/taylor_copper.json): the classic copper Taylor test; final length
  17.40 mm of 25.4 (L/L0 0.685), largest diameter 14.6 mm of 7.62 (D/D0 1.91), unchanged between two meshes.
- [sphere_plate.json](../../examples/lab/sphere_plate.json): a copper sphere at 800 m/s through a 4 mm copper
  plate; the erosion limit is assumed for the demonstration and the scenario says so.

## Scenario keys

`domain` = "impact", `title`, `symmetry` (none, half, quarter), `anvil_z_m`, `bodies` [{`shape` cylinder
{`radius_m`, `z0_m`, `length_m`, `cells` [per quarter circumference, rings, layers]} | box {`lo_m`, `hi_m`,
`cells`} | sphere {`center_m`, `radius_m`, `cells`}, `material`, `velocity_m_s`, `fail_strain` with
`fail_strain_source`}], `run` {`end_time_s`, `frames`, `mirror`}.
With `"method": "sph"`: `particle_spacing_m`, and shapes without `cells`.
