# Batteries: a lithium-ion cell and a module under load

Up: [the lab's documents](README.md) · code: [src/lab/battery/](../../src/lab/battery/dfn.h) (the cell,
[dfn.h](../../src/lab/battery/dfn.h); the module, [pack.h](../../src/lab/battery/pack.h)) · verification:
`tools/battest.c` (`./build/battest`, about 40 seconds)

![Twelve cells in parallel on a cold plate, discharged at 2C: the cells warm from the top, the weak one most](../media/lab/battery-module.gif)

Twelve LG M50 cells (21700, NMC 811 against graphite-SiOx) stand in parallel on a liquid-cooled aluminium plate and are
discharged at 2C, 120 A, until the module falls to 2.5 V. Every cell is computed by the Doyle-Fuller-Newman model at
its own temperature. Their heat is conducted through the module in 3D, down each cell's axis into the plate, and the
coolant warms as it flows under the plate. One cell's weld is four times as resistive as the others'. It delivers less
current early on, keeps more charge, and delivers more late, so it ends the hottest.

## The model

| Part | What is computed | Reference |
|---|---|---|
| The cell | the pseudo-two-dimensional model: lithium diffusing in spherical particles in each electrode, ions moving through the electrolyte by diffusion and migration (concentrated solution, transference number), currents in solid and electrolyte, Butler-Volmer kinetics with Arrhenius exchange currents | Doyle, Fuller and Newman 1993 |
| Its parameters | the LG M50 as measured and fitted by Chen et al. (2020), as tabulated by PyBaMM: thicknesses, porosities, particle radii, diffusivities, conductivities, open-circuit potential fits, the electrolyte of Nyman et al. (2008) | [THIRD_PARTY.md](../THIRD_PARTY.md) |
| Its numerics | finite volumes across the cell and in shells in each particle; backward Euler; the particles' concentrations eliminated (their diffusion is linear in the reaction current) so that Newton's method works on the electrolyte's concentration and potential, the solid's potential and the reaction current, with a dense Jacobian by finite differences | |
| Its heat | reaction overpotentials, Joule heating in solid and electrolyte (each face's current times its potential drop), reversible heat where dU/dT is given (it is not, in this set) | Bernardi, Pawlikowski and Newman 1985 |
| The module | cells in parallel sharing the terminal voltage through their own interconnect resistances; the split found by Newton's method each step | |
| Conduction | 3D, a grid of 1.5 mm cubes, backward Euler, conjugate gradients; the jelly roll transversely isotropic (1.16 W/(m K) across its layers, 24.7 along them, computed from the parameter set's layer data), the plate aluminium, air between the cells | |
| Cooling | the plate's underside into a coolant flowing along the module and warming by what it takes; still air elsewhere | |

## Verification

`tools/battest.c`, criteria written before the first run:

| Case | Against | Criterion | Result |
|---|---|---|---|
| B1 a particle losing lithium at a constant flux | Crank's closed form for the sphere | 0.5 % of the drop on 20 shells, second order | 0.10 %, the error falling 4.01 times per halving |
| B2 lithium over a 1C discharge | conserved | 1e-10 | 2.6e-14 |
| B3 lithium leaving the negative electrode | the charge over Faraday's constant | 1e-9 | 4.5e-14 |
| B4 the heat's local parts | I (U_eff - V), an identity of charge conservation | 1e-8 | 1.3e-14 |
| B5 the capacity at C/10 | the set's nominal 5.0 Ah | 5 % | 5.075 Ah, +1.5 % |
| B6 the voltage at 1C, 1800 s | resolution across the cell | 2 mV (10 to 20), 0.5 mV (20 to 40) | 0.21 and 0.05 mV |
| P1 conduction, a block cooled from below | the closed form plus the grid's known half-cube offset | 1e-9 of the rise | 3.6e-13 |
| P2 the module's energy | made = stored + to the coolant + to the air | 1e-9 | 1.5e-13 |
| P3 the parallel split | currents add up, terminal voltages agree | 1e-9 A, 1e-6 V | 3e-13 A, 7e-13 V |
| P4 two identical cells, insulated | equal currents | 1e-9 | 1.8e-14 |

The first runs failed on two defects in the code, found before any criterion could be measured: the solid's current
balance had the wrong sign on interior faces, and the LU solve interleaved the row interchanges with the substitution
while the factors had moved whole rows. P4 then failed (7.5e-4) because the thermal grid overhung one side of the
module by half a millimetre, so the two cells were cut into different cubes; the grid is now centred.

A 1C discharge of one cell gives 4.93 Ah to 2.5 V. Validation against a measured discharge of the LG M50 (Chen et
al. publish theirs) is open.

Not modelled: the electrolyte's and the particles' properties changing with temperature (the set gives Arrhenius
factors for the exchange currents only), degradation (SEI growth, lithium plating), the current collectors' in-plane
resistance and the tabs' current distribution, the can, and each cell's heat source spread over its cubes rather than
by its own current distribution.

## Running it

```bash
make lab
./build/battest
./build/labrun examples/lab/battery_module.json /tmp/battery.lab     # about 80 seconds
```

Keys ([battery_module.json](../../examples/lab/battery_module.json)): `cell` {`parameters`: `"lgm50_chen2020"`,
`volumes_per_region`, `shells`}, `module` {`rows`, `cols`, `pitch_m`, `radius_m`, `height_m`, `plate_m`, `margin_m`,
`grid_m`, `layers`}, `thermal` {`jelly_roll` {`radial_w_mk`, `axial_w_mk`, `heat_capacity_j_m3k`}, `plate` and `gap`
{`conductivity_w_mk`, `heat_capacity_j_m3k`}, each with its `source`}, `cooling` {`coolant_inlet_k`,
`coolant_capacity_rate_w_k`, `plate_coefficient_w_m2k`, `air_coefficient_w_m2k`, `air_k`}, `electrical`
{`interconnect_ohm`, `weak_cells` [{`cell`, `interconnect_ohm`}]}, `load` {`current_a`, `cutoff_v`}, `run` {`dt_s`,
`end_s`, `frames`}. The result is a part `module`: each cell a cylinder of hexahedra and the plate a slab, with `T` (K)
and each cell's `current` (A). The run record gives the capacity delivered, the time to the cut-off, the hottest cell,
the spread of the currents, the coolant's outlet and the energy balance.
