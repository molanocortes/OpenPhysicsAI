# Heat and flow: cooling, ventilation, mixing (`src/lab/heat`)

Up: [the lab](README.md) · code: `src/lab/heat/` · test: `tools/hftest.c` · goals: [GOALS.md](../../GOALS.md) G11

## What it is

Air or water moving through channels, rooms, racks and cushions, carrying heat into and out of the solids it touches:
the physics of chip cold plates, heat exchangers, data-centre aisles, ventilated rooms, ventilated seats and
microfluidic mixers, in two dimensions. Rooms and data halls are now computed in 3D by the fire solver's room engine
([rooms.md](rooms.md)); their 2D scenarios left the library on 2026-09-26 (the generators remain in
`tools/lab_scenarios.py`).

- **Flow:** lattice Boltzmann, D2Q9, two-relaxation-time collision with Ginzburg's magic parameter 1/4 (bounce-back
  walls then sit half-way between nodes whatever the viscosity), Guo's forcing for buoyancy (Boussinesq), fans (a
  body force in a region) and porous media (Brinkman's Darcy drag, implicit). Inlets are walls moving at the inlet
  speed, outlets hold the pressure and extrapolate the non-equilibrium part.
- **Heat:** a finite-volume energy equation on the same cells, in fluid and solid alike, each cell with its own
  conductivity and heat capacity (harmonic means at faces: conjugate heat transfer with continuous temperature and
  flux), advection second order with the van Leer limiter, explicit with sub-steps where conduction is fast.
- **A passive scalar** (a dye, a species, or the age of the air) on the same machinery, with its own diffusivity; it
  does not enter solids.

## Verification (`tools/hftest.c`, in `make test-fast`)

Criteria written before the first run:

| Case | Against | Criterion |
|---|---|---|
| H1 square cavity, heated side walls, Pr 0.71, Ra 1e4 and 1e5 | de Vahl Davis (1983): mean Nusselt number 2.243 and 4.519 | 2 %, and hot and cold walls within 1 % of each other |
| H2 a wall of three layers between fixed temperatures | the closed form dT / sum(L / k) and its interface temperatures | 0.1 % |
| H3 a channel driven by a body force | plane Poiseuille flow, a H^2 / (8 nu) | 1 % |
| H4 forced convection between isothermal plates | the fully developed Nusselt number 7.541 (Shah and London 1978) | 3 % |
| H5 (added with the rotors) a disk turning inside a fixed ring, Re 2.5 | Taylor-Couette: torque 4 pi mu omega R1^2 R2^2 / (R2^2 - R1^2), speed A r + B / r | 3 %, 2 % |

Results of 2026-09-26: H1 Nu 2.2484 (+0.24 %) at Ra 1e4 and 4.5281 (+0.20 %) at Ra 1e5, hot and cold walls equal to
the fourth digit; H2 exact to the printed digits (312.50000 W/m^2, interfaces 306.8750 and 306.2500 K); H3 +0.033 %;
H4 Nu 7.539 (-0.03 %); H5 torque +0.67 %, mid-gap speed +0.94 % (first run). All pass.

The first run failed H1: Nu 2.172 at Ra 1e4 (-3.2 %), and on 32 cells the cavity showed no steady convection and
temperatures outside the range of its walls. Conduction alone was exact, so the fault was in advection: the heat was
carried in conservative form with absolute temperatures near 300 K, and the lattice velocity, free of divergence only
to the order of the Mach number squared, turned T div u into a spurious source. The advective form (each face carries
the difference to the cell's own value) removed it; the criterion was not touched.

## Rotors

Fans, pumps and impellers turn: a rotor is a hub and blades (straight, or curved on a logarithmic spiral at a given
angle to the tangent) about a centre at a fixed speed. Each step the cells it covers are solid; the fluid sees its
surface move (bounce-back with the wall velocity omega x r) and cells it uncovers start at that velocity. The torque of
the fluid on it comes from the momentum exchanged at its links, and the power from torque times speed. H5 checks the
moving wall and the torque on a turning disk; blades that sweep across cells are the same mechanism, not separately
verified.

## Turbulence

Room-scale air is turbulent and its molecular viscosity is far below what a lattice of centimetre cells can carry.
With `turbulence` {`smagorinsky`, `turbulent_prandtl`} the solver adds a Smagorinsky eddy viscosity computed from the
local non-equilibrium stress (Hou, Sterling, Chen and Doolen 1996) and an eddy diffusivity for heat and the scalar. It is a model, not
verified here against a turbulent benchmark: the room and data-centre scenarios that use it are demonstrations.

## Scenario keys

`domain` = "heat", `title`; `grid` {`nx`, `ny`, `cell_m`}; `fluid` {`kinematic_viscosity_m2_s`,
`thermal_diffusivity_m2_s`, `volumetric_heat_capacity_j_m3k`, `expansion_1_k`, `source`}; `gravity_m_s2` [gx, gy];
`reference_temperature_k`, `initial_temperature_k`; `largest_speed_m_s` (sets the time step), `lattice_speed`
(default 0.05); `periodic_x`; `scalar` {`diffusivity_m2_s`, `initial`}; `materials` [{`name`, `kind` solid or
porous, `conductivity_w_mk`, `volumetric_heat_capacity_j_m3k`, `permeability_m2`, `source`}]; `regions`
[{`box_m` [x0, y0, x1, y1] or `circle_m` [x, y, r], `material`, `heat_w_m3`, `fan_m_s2` [ax, ay],
`scalar_source_per_s`}]; `rotors` [{`centre_m`, `rpm`, `material`, `hub_radius_m`, `blades`,
`blade_inner_radius_m`, `blade_outer_radius_m`, `blade_thickness_m`, `blade_angle_deg`}] (needs the fluid's
`density_kg_m3`); `turbulence` {`smagorinsky`, `turbulent_prandtl`}; `boundaries` [{`side` x-, x+, y- or y+, `from_m`, `to_m`, `type` wall, inlet or outlet,
`speed_m_s`, `parabolic`, `temperature_k`, `heat_flux_w_m2`, `thermal` adiabatic, fixed or flux, `scalar`}] (the
rest of every side is an adiabatic no-slip wall); `run` {`end_time_s`, `frames`, `first_frame_s`}.

A material or a fluid without a `source` for its properties is refused.

## Limits

Two-dimensional; laminar (no turbulence model); Boussinesq buoyancy (small temperature differences); constant
properties; the lattice needs a relaxation time above 0.502, so thin, fast water flows need small cells.

## Three-dimensional conduction

`examples/lab/heat_volume.json` selects `model: conduction3d`. This is a full 3D solid conduction solver,
not yet a replacement for the 2D coupled fluid/thermal model above. It solves
`capacity * dT/dt = div(conductivity * grad(T)) + source` in double precision with conservative
face fluxes and harmonic conductivity across material interfaces. All six outer faces are explicitly insulated.
The local sum of conductances bounds the explicit time step.

Required inputs are `grid` (`nx`, `ny`, `nz`, `cell_m`), a sourced `material` with
`conductivity_w_mk` and `volumetric_heat_capacity_j_m3k`, `initial_temperature_k`,
`boundary: insulated`, and `run` (`end_time_s`, `frames`). Optional sourced `regions` carry
`box_m: [xmin,ymin,zmin,xmax,ymax,zmax]`, material overrides, initial temperature, and
constant `heat_w_m3`. Regions override previous regions where they overlap.
The output is the full temperature volume in K; no velocities or convection are claimed.

`tools/heat3dtest.c` records criteria before its first run: an exact cosine product that varies in all
three axes, second-order spatial convergence, conservation with heterogeneous materials, bounded temperatures,
and invalid-state rejection. Observed mode errors were 0.000916427484 at 16 cubed and 0.000229217106
at 32 cubed; energy relative drift was 1.38262864e-15. These verify conduction, not measured hardware.

The viewer maps unsigned volume opacity from the selected range minimum, while signed wave fields use magnitude
around zero. This transfer function and lighting are presentation choices, not additional physical quantities.
