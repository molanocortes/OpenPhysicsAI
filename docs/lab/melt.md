# Metal melting: a laser melts tracks into steel

Up: [the lab's documents](README.md) · code: [src/lab/melt/](../../src/lab/melt/melt.h) · verification:
`tools/melttest.c` (`./build/melttest`, in `make test-lab`)

![A 200 W laser at 0.8 m/s melts three tracks side by side into a 316L plate; pale yellow is above the liquidus, the
glow behind is the metal cooling](../media/lab/laser-tracks.gif)

A laser of 200 W, focused to a spot of 80 um, crosses a plate of 316L stainless steel at 0.8 m/s, turns, and comes
back 80 um to the side, three times: one layer's scan of laser powder bed fusion without the powder. The melt pool
runs with the beam, about 490 um long, 110 um wide and 45 um deep; the steel solidifies within a millisecond behind it,
and each track remelts the edge of the one before. Cut across the tracks, the metal that was molten shows the
overlapping fusion boundaries a metallographer sees under the microscope:

![A cut across the three tracks: the metal that melted (pale) above the three overlapping fusion boundaries in the
plate (dark)](../media/lab/laser-tracks-section.png)

2.9 million cells of 5 um, 8 700 steps, 90 s on the development laptop.

## The model

| Part | What is computed | Reference |
|---|---|---|
| Heat | 3D conduction on a uniform grid, finite volumes, harmonic-mean conductivity at faces, explicit at the stable step, threaded | |
| Melting | the enthalpy method: the state is the enthalpy per volume, temperature and liquid fraction follow from it (solid, mushy between solidus and liquidus with the latent heat, liquid) | Voller and Prakash 1987 |
| Beam | a Gaussian of 1/e^2 radius w, each top cell taking the exact integral of the beam over its face (error functions) times the absorptivity; straight tracks with pauses | |
| Surface | convection and radiation from the top face | |
| Boundaries | each face adiabatic or held at a temperature; the energy through held faces counted | |

Steel: wrought 316L from the materials library (`src/ctl/materials.json`, record `ss316l_lpbf`): density 7904 kg/m3,
solidus 1675 K, liquidus 1708 K and latent heat 290 kJ/kg (Pichler, Simonds, Sowards and Pottlacher, J Mater Sci 55,
2020); solid specific heat 580 J/(kg K) and conductivity 22 W/(m K), constants taken from the library's measured
curves. Assumed and labelled as demonstration values in the scenario: the liquid's 800 J/(kg K) and 30 W/(m K), the
emissivity 0.4, the absorptivity 0.35 of a bare steel surface. Not modelled: evaporation and the keyhole at higher
power (the surface stays flat), the powder, properties varying with temperature within a phase. Without `melt_flow`
this is conduction-mode melting; with it, the melt flows (below).

## Flow in the melt: Marangoni convection

![A laser track in 316L with the Marangoni flow: the pool wide and shallow](../media/lab/melt-pool-flow.gif)

A melt pool is stirred by its own surface. Surface tension changes with temperature, so the hot surface under the beam
is pulled toward the cooler rim when dsigma/dT < 0 (a steel low in sulphur). The flow reaches metres per second and
carries heat outward: the pool grows wider and shallower than conduction alone makes it. With enough sulphur,
dsigma/dT turns positive, the flow reverses and the pool digs deeper; that is the Heiple and Roper effect known from
welding.

| Part | What is computed |
|---|---|
| Flow | incompressible Navier-Stokes on the cell faces (staggered), constant density, first-order upwind advection, explicit viscosity |
| The solid | the Carman-Kozeny drag of the enthalpy-porosity method (Voller and Prakash 1987), implicit; a face between two cells with no liquid is held at rest |
| The surface | flat, w = 0, the Marangoni stress mu du/dz = dsigma/dT dT/dx as the tangential velocities' boundary condition |
| Pressure | a projection whose face coefficients are 1 / (rho + dt A), A the drag's coefficient, so pressure cannot push the mushy metal; solved in a window around the pool (the solid drops out) by the multigrid-preconditioned gradients to 1e-12 |
| Heat | carried by the flow with a minmod-limited upwind flux of the enthalpy (latent heat travels with the liquid) |

In the scenario ([melt_pool_flow.json](../../examples/lab/melt_pool_flow.json), 288 000 cells of 5 um, 2 213 steps,
46 s) the surface flows outward at up to 6 m/s and the pool at the track's end is 170 um wide and 25 um deep, where
conduction alone gives 110 and 45 (melttest M7). The liquid's viscosity (6 mPa s) and dsigma/dT (-0.4 mN/(m K)) are
demonstration values of the magnitude reported for liquid iron-based alloys. The pressure first ran over the whole
block and took a second a step; held to the pool, it takes 20 ms.

## Verification (`tools/melttest.c`)

Criteria written on 2026-09-26 before the first run; all passed on it.

| Case | Against | Criterion | Result |
|---|---|---|---|
| M1 a Gaussian beam crossing a block at 0.2 m/s, no melting | Rosenthal's moving point source (1946) convolved with the beam, at four points 2 to 5 radii from the beam | 2 % | +0.86, -0.06, -0.04, +0.23 % |
| M2 a bar at its melting point, one end raised by 200 K (Stefan number 0.5) | Neumann's similarity solution s = 2 lambda sqrt(alpha t) | 1 % | -0.001 % |
| M3 two tracks with melting and surface losses, insulated block | enthalpy gained against energy absorbed less lost | 1e-4 of absorbed | 4e-12 |
| M4 a liquid layer under a uniform temperature gradient, the top free with the Marangoni stress (added 2026-09-27, with the flow) | the thermocapillary return flow u = (tau / mu)(3 z^2 / (4 H) - z / 2) | 2 % of the surface speed | 0.19 % |
| M5 a track with the flow, insulated block | energy balance | 1e-9 of absorbed | 5.7e-15 |
| M6 the same run | the divergence left by each projection | 1e-8 of the fastest speed | 2.7e-12 |
| M7 the same run against the same track without flow | the pool wider for its depth (Heiple and Roper), qualitative | 10 % | width over depth 2.44 to 6.80 |

M6 failed twice before passing, and the test's header has both runs. First, the projection solved to 1e-10, which left
1e-7 of divergence against the solid's tiny coefficients; it now solves to 1e-12. Then, the steps just after the first
cell melts have a fastest speed of 1e-16 m/s, where the ratio is round-off; M6 now counts from 1 mm/s, by an open
amendment. `melttest heat` runs M1 to M3 and `melttest flow` runs M4 to M7 (together about 5 minutes).

## Running it

```bash
make lab
./build/melttest                                                  # about 5 minutes (heat, then flow)
./build/labrun examples/lab/melt_pool_flow.json /tmp/pool.lab    # one track with Marangoni flow, 46 s
./build/labrun examples/lab/laser_tracks.json /tmp/laser.lab      # 8 700 steps, 6.8 ms of scanning, 90 s, 200 MB
```

Keys ([laser_tracks.json](../../examples/lab/laser_tracks.json)): `metal` {`density_kg_m3`,
`specific_heat_solid_j_kgk`, `specific_heat_liquid_j_kgk`, `conductivity_solid_w_mk`, `conductivity_liquid_w_mk`,
`solidus_k`, `liquidus_k`, `latent_heat_j_kg`, `emissivity`, `source`}, `block` {`size_m`, `origin_m`, `cell_m`,
`output_every` (cells averaged per output cell along each axis), `bottom_held_k`}, `beam` {`power_w`, `absorptivity`,
`radius_m`, `tracks` [{`from_m`, `to_m`, `speed_m_s`, `pause_s`}], `source`}, `initial_temperature_k`,
`ambient_temperature_k`, `convection_w_m2k`, `run` {`end_s`, `frames`}. The result has a block part `plate` (fields
`T` and `melted`, the largest liquid fraction each cell reached) and the beam as a line. In the app, `lab section x 0.5`,
`lab focus X Y Z` and `lab zoom F` frame the cut across the tracks.
