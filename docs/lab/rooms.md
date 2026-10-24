# Rooms: ventilation, data halls and fires in rooms, in 3D

Up: [the lab's documents](README.md) · code: [src/lab/fire/](../../src/lab/fire/fire.h) (the same engine as
[fire.md](fire.md)), the pressure solver [src/lab/mg/](../../src/lab/mg/mg3d.h) · verification: `tools/roomtest.c`
(`./build/roomtest`, about 6 minutes; `ROOM_FULL=1` adds the 64-cell cavity, about 20 minutes: run it detached)

The fire solver's low-Mach engine is also the engine for the air of rooms: buoyant flow in 3D between walls, around
furniture and machines, driven by supply vents, fans and heat. Its sides can be open or walls (no slip or free slip,
adiabatic or at a temperature); inside, solid obstacles, vents and openings in the walls, fans that hold a flow through
a region (a fan, a rack of servers), and heat sources. The same engine burns a fire in the room when a burner is given.
Nothing here is a 2D section extruded: every scenario is computed in three dimensions.

## What it adds to the fire solver

| Part | What is computed |
|---|---|
| Walls | per side of the box: open, a no-slip wall or a free-slip wall; a wall adiabatic or held at a temperature, its heat flux k (T_wall - T) / (dx / 2) with the gas's conductivity (molecular plus subgrid) |
| Obstacles | solid cells (whole cells, a staircase); their faces closed, no slip along them (the tangential velocity's ghost is its negative, the wall half a cell away); adiabatic or at a temperature |
| Vents | faces of a wall held at a speed into the room (negative: an exhaust), the air coming in at its own temperature (a burner is a vent carrying fuel) |
| Openings | patches of a wall made open: the pressure perturbation zero on them (a door, a window, a return grille); the Poisson solver's side then mixes Dirichlet and closed faces |
| Fans | faces inside a box, normal to an axis, set to a speed before each projection and left open to it (the projection may trim them): a fan, or the air a rack of servers pulls through itself. Holding them closed, as vents are, cut a rack into slabs that the pressure solver's coarse grids could not represent, and the solve stalled |
| Heat sources | a power (W) spread over a box: people, machines |
| A sealed room | with no open face, the background pressure rises as the room heats, so that div u integrates to what the vents bring in (FDS's pressure zone): dp0/dt = gamma p0 (sum of D_heat dV - vent inflow) / V |
| Turbulence | Vreman's subgrid model (2004) for rooms: it vanishes at walls and in laminar shear, where Smagorinsky's does not; none at all for a resolved flow |
| The age of the air | a passive scalar that grows by one second per second and enters at zero through vents and openings: how long the air at each point has been in the room, the measure of ventilation (ASHRAE's and ISO's local mean age of air) |

The pressure is solved by conjugate gradients preconditioned with one multigrid V-cycle each iteration. Multigrid alone
stalled (a residual falling 0.81 per cycle) in a room joined to the outside only by a doorway: two-cell walls average
away on the coarse grids, so the room's pressure level is a mode the coarse grids misrepresent; the gradients remove
that mode in a few iterations. On the plain cases of `poissontest` the same solve takes 8 to 10 iterations where
multigrid alone took 10 to 13. A side that is closed except for an opening (an office's return grille in its ceiling)
is a Dirichlet side of the Poisson problem whose closed faces carry no coefficient; the coarse grids' correction must
then mirror across those closed faces rather than change sign (it first did, which made the V-cycle an indefinite
preconditioner and the gradients stalled at 0.4); with each boundary face's own rule it takes 9 iterations.

## Verification

`tools/roomtest.c`, criteria written before the first run; the runs that changed it are in its header.

| Case | Against | Criterion | Result |
|---|---|---|---|
| A1 a square duct, 25 cells across, Re 20: walls of the box on two sides, obstacles on the other two, a vent feeding it | the centreline speed over the mean, 2.0962 (Shah and London 1978) | 1.5 % | 2.0836, -0.60 % |
| A2 the same duct's pressure drop | f Re = 56.91 | 3 % | 56.814, -0.17 % |
| A3 mass through it | out equals in | 1e-6 | exact to the printed digits |
| B1 the differentially heated cube, Ra 1e4, 32 cells, sealed, no slip | Tric, Henry and Le Quere (2000), Nu 2.0542 | 2 %, cold wall within 1 % of hot | 2.0797, +1.24 %; walls equal to 1e-5 |
| B2 the same at Ra 1e5, 64 cells | Nu 4.3370 | 2 %, the same | 4.3864, +1.14 %; walls equal to 1e-5 |
| B3 the sealed cube's mass | unchanged | 1e-10 | 6.6e-13 |
| C a ventilated room: a supply vent, an opening, an obstacle, a 400 W heat source, steady | the outflow's temperature rise, Q / (m cp) | 1 % | 8.139 K against 8.208, -0.84 % (-0.28 % in an earlier run: a turbulent room's 50 s average) |
| D a sealed room heated by 1 kW for 10 s | the first law, dp = (gamma - 1) Q t / V | 1 % | -0.41 % |
| E a fan across a duct open at both ends | the flow, the fan's speed times the section | 2 % | -0.42 % |
| F the age of the air in C's room, steady | the exhaust's mean age equals the room's air mass over the mass flow, however the room mixes (Sandberg 1981) | 2 % | 23.96 s against 24.03 s, -0.29 % |
| every case | the projection's residual divergence | 1e-6 | 1e-10 |

What the runs taught: the duct's first run at 13 cells across gave 2.0599 (-1.73 %, a failure). The same
discretization solved directly in 2D gives 2.0609 on 13 cells and converges to 2.0962 at second order (2.0865 on 25,
2.0939 on 51), so the 3D solver was right about its own discretization and the criterion wanted a finer duct; the test
now uses 25 cells. The ventilated room's first run (-2.71 %) was averaged while the room was still warming: its mass
was still falling, and that 0.07 % difference between outflow and inflow carries the absolute temperature (0.2 K
against the 8.2 K rise); averaged ten air changes in, it is -0.28 %.

Not modelled: wall functions (a wall's heat and shear come from the gas's conductivity and viscosity at the first
cell, which underestimates both on coarse grids: a room's cold window exchanges too little heat at 10 cm cells),
radiation between surfaces, humidity, the heat capacity of walls and furniture (they are adiabatic or held at a
temperature), leakage through the envelope, anything below a cell (a diffuser's vanes, a server's internals).

## Scenarios

| Scenario | What it shows |
|---|---|
| [office_hvac_3d.json](../../examples/lab/office_hvac_3d.json) | an office cooled by a ceiling diffuser: the cold jet falls, spreads over the floor and rises in the plumes of two people and two computers, leaving by a return grille (demonstration loads) |
| [data_centre_3d.json](../../examples/lab/data_centre_3d.json) | a data hall: perforated tiles feed a cold aisle between two rows of racks whose fans draw a fifth more air than the tiles supply, so hot air curls over the rows and round their ends into the cold aisle |
| [room_fire_3d.json](../../examples/lab/room_fire_3d.json) | a 250 kW methane fire in a room with one doorway: the plume, the ceiling jet, the hot layer deepening, smoke leaving by the top of the door and fresh air entering below |

![A 250 kW fire in a room, seen through a section at the doorway: the hot layer, the door's outflow and inflow](../media/lab/room-fire.gif)

![An office cooled by a ceiling diffuser: the cold jet, the floor layer and the plumes](../media/lab/office-hvac.gif)

The office, 600 s in 7 minutes (48 x 32 x 24 cells of 10 cm, 14 914 steps): the supply's jet at 17 C falls to the
floor and spreads as a cool layer across it, the plumes of the two people and the two computers rise through it to
the ceiling, and the air leaves by the return grille. Over the last 300 s the supply brought 0.097 kg/s at 290.15 K
and the return took it out at 294.0 K; 350 W over m cp gives 3.56 K, the return's 3.85 K is higher because the room,
started at 21 C, was still giving up heat (its mean fell from 294.15 to 293.58 K). The picture shows the temperature
against 21 C: blue colder, orange warmer.

![A data hall: the cold aisle between two rows of racks, hot exhaust at their backs](../media/lab/data-centre.gif)

The data hall, 120 s in 12 minutes (64 x 48 x 32 cells of 10 cm): the tiles' air rises into the cold aisle and the
racks draw it through; their hot exhaust fills the hot aisles, and because the racks draw a fifth more than the tiles
supply, hot air comes back over the tops of the rows and round their ends into the cold aisle, the upper racks
breathing warmer air than the lower. Over the last 60 s the air left 6.3 K above the tiles' 18 C (48 kW over the
tiles' m cp would be 6.7 K; the hall, started at 24 C, was still settling). Shown against 24 C.

The room fire, 60 s in 7 minutes (48 x 32 x 32 cells of 10 cm, 9 011 steps): the plume strikes the ceiling and the
ceiling jet spreads to the walls; the hot layer deepens until it reaches about a third of the room's height above the
floor, and settles there; smoke leaves by the upper part of the doorway and rises along the outside of the wall, while
air from outside enters under it, runs along the floor and leans the flame away from the door. Averaged over the last
30 s: 250 kW released, the room's gas at 375 K on average. Its flame is 0.80 m tall where Heskestad's correlation
gives 1.56 m for the same fire in open air: at D* / dx = 5.4 the flame is not resolved (the open fire of fire.md ran
26 % short at D* / dx = 7.7), which is why this scenario is a demonstration and the layer, not the flame, is its
subject.

```bash
make lab
./build/roomtest
./build/labrun examples/lab/room_fire_3d.json /tmp/room_fire.lab     # about 5 minutes
```

Keys, besides the fire's ([fire.md](fire.md)): `sides` {`"x-"` ... `"z+"`: `"open"` | `"wall"` | `"slip"`} (default:
open, a free-slip floor), `wall_temperature_k` {side: K}, `turbulence` (`"vreman"`, the default without a burner;
`"smagorinsky"`, the default with one; `"none"`), `sponge_cells`, `obstacles` [{`box_m`, `temperature_k`}], `vents`
[{`side`, `rect_m` [a0, b0, a1, b1] in the side's other two coordinates in order, `speed_m_s`, `temperature_k`}],
`openings` [{`side`, `rect_m`}], `heat_sources` [{`box_m`, `power_w`}], `fans` [{`box_m`, `axis`, `speed_m_s`}],
`reference_temperature_k` (the result's `dT` is measured from it). The `burner` and `fuel` are optional. The result is a
block `gas` with `dT`, `T` and `speed` (and `hrr` with a burner) and the obstacles as `material`; the vents and openings as
surfaces and the fans' boxes as solid cabinets (`fan_look`, default aluminium), as a rack of servers is. The run record gives the averaged inflow and outflow and their temperatures, the heat from walls,
obstacles and sources, the mean and hottest temperatures and the background pressure.
