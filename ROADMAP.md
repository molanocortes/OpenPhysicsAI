# Roadmap

What is being built, goal by goal, in the order the goals were set. A goal is done when every step under it passes its
verification case: a criterion written before the first run, against a closed form or an independent solution
([AGENTS.md](AGENTS.md), rule 2). Comparisons with measurements are made blind against held-out data; their answers
and per-case errors stay sealed, and only the boards' banded scores are published ([flags/README.md](flags/README.md)).
Up: [AGENTS.md](AGENTS.md) · intent: [VISION.md](VISION.md) · what was built when: [docs/map/HISTORY.md](docs/map/HISTORY.md).

Source comments and records cite the working programme as "GOALS.md Gn step k". That file is the project's working
log and is kept with its private records; this page gives each goal's aim and state under the same number.

States: **done**, **in progress**, **open**.

## The physics lab (from 2026-09-25)

**G0. Baseline** (done). The build and the session test tier green before any lab work.

**G1. Lab infrastructure** (done). One result format for every lab solver (`src/lab/labio.h`) and one headless film
maker (`tools/labfilm.c`), so every result can be drawn the same way. [docs/lab/README.md](docs/lab/README.md)

**G2. Flags** (done). Real measurements from controlled experiments, hidden from the entrant: public cases, sealed
answers with a published commitment hash, scores that cannot be inverted into the answers, and boards drawn from the
attempts. [flags/README.md](flags/README.md)

**G3. Compressible flow with adaptive mesh refinement** (done). 2D and axisymmetric Euler and Navier-Stokes with
block-structured refinement and cut-cell bodies, verified against exact Riemann solutions and oblique and normal shock
relations; the capsule's bow shock and the shock-bubble flags. [docs/lab/gas.md](docs/lab/gas.md)

**G4. Sound in rooms** (in progress). 3D time-domain acoustics with impedance walls, verified on room modes, free-field
decay and wall reflection; open: a flag from a measured room. [docs/lab/acoustic.md](docs/lab/acoustic.md)

**G5. Impact** (in progress). Large-strain explicit solid dynamics with Johnson-Cook plasticity and erosion, and
particles for hypervelocity fragmentation (a Whipple shield, a laboratory crater); open: flags for Taylor impact and
crater scaling. [docs/lab/impact.md](docs/lab/impact.md)

**G6. Orbital dynamics** (done). A symplectic N-body integrator with post-Newtonian correction, verified on Kepler
orbits and long-run invariants; the planets against the JPL ephemeris and an asteroid's close approach.
[docs/lab/orbit.md](docs/lab/orbit.md)

**G7. Electromagnetism** (in progress). 3D Maxwell (Yee) with absorbing layers and plane-wave scattering, verified on
Mie scattering and cavity resonances; open: a measured radar cross section as a flag. [docs/lab/em.md](docs/lab/em.md)

**G8. Flow against photographs** (in progress). The flow solver verified on the Schaefer-Turek benchmark, streaklines
as in the photographs of a cylinder's wake; open: the cylinder and aerofoil cases side by side with the photographs,
and their quantitative flags. [docs/lab/flow.md](docs/lab/flow.md)

**G9. The native app shows every lab result** (done). A lab workspace that opens, plays and draws any lab result with
its mesh and refinement levels. [docs/LAB.md](docs/LAB.md)

**G10. The front page** (in progress). The README in plain words: what this is, the flags and their boards, how a person
or an agent runs it; open: every new document linked from the map. [README.md](README.md)

**G11. Industrial applications** (in progress). Scenarios built on the solvers above, each with a stated set-up and,
where a published measurement exists, a flag: data-centre and office air, an electric motor's field and losses, rooms
in 3D, rotating bodies; open: chip cooling, heat exchangers, pumps and fans, turbomachinery, vehicle aerodynamics and
others. [docs/lab/rooms.md](docs/lab/rooms.md), [docs/lab/heat.md](docs/lab/heat.md)

**G12. One world in 3D, one aesthetic** (in progress). Every simulation in a 3D view with one shared style, and 2D
results shown as the sections they are; open: GPU rendering of lab results in the app's scene, and the same augmented
views (streamlines, isosurfaces, probes) for every domain. [docs/LAB.md](docs/LAB.md)

**G13. Flagship phenomena** (in progress). Each a verified capability, a scenario and a film: brittle fracture, impact
through a plate, hyperelastic sheets, free-surface water, a re-entry shock layer, fire, an electric motor in 3D,
induction heating, a loudspeaker, a battery under load, a heart valve, melting with Marangoni flow, light bending
around a black hole. Open among them: a lightning streamer, a combustion chamber, and the comparison of breaking water
with the SPHERIC benchmark's measurements. [docs/LAB.md](docs/LAB.md)

**G14. The app: two modes** (in progress). An agent mode that hands a question to the user's own AI tool, and a
simplified manual mode with a library of every scenario; open: quick switching between simulations in one world.
[docs/app/REVIEW.md](docs/app/REVIEW.md)

**G15. The thirteen flags** (in progress). The repository built around thirteen measurements of the real world: eight
flags, two whose answers nature reveals on a known date, and three beyond today's simulators. Flag 05 (AM-Bench melt
pools) is open with its answers sealed; open: the other flags' cases, the registered predictions, a first attempt at
every flag. [README.md](README.md), [flags/LEADERBOARD.md](flags/LEADERBOARD.md)

## Printing and the engineering core (from 2026-10-03)

**G16. Printing foundation and a consolidated app** (done). Numerical correctness of the metal and plastic print
simulations before any new claim (energy conservation in deposition, plate reactions, radiation, refinement studies
with fixed set-ups), MCP over local HTTP, and computed sculptures. [docs/art-gallery.md](docs/art-gallery.md),
[docs/contracts/print-results.md](docs/contracts/print-results.md)

**G17. Will my print break?** (in progress). In about a minute: will this printed part hold the load described, where
would it break first, and which orientation is stronger. Done: printed-part strength in the app, directional strengths
in the materials library, walls and infill, the answer in words, the answer on curved parts with TET10. Open: real
breaks printed and measured, a plain-language set-up for non-engineers, a community test kit. No accuracy is stated
before the real breaks are compared. [docs/analysis.md](docs/analysis.md), [kit/README.md](kit/README.md)

**G18. A minimal, agentic window** (done). Manual mode with two buttons, Open and Run; agentic mode as one bar with
text and a microphone; the colour legend stays. [docs/app/DESIGN.md](docs/app/DESIGN.md)

**G20. Printing physics against real data** (in progress). Blind comparisons with published, controlled experiments,
set up by a curator agent that makes no prediction, the outcomes sealed before any prediction. Done: the data found
and sealed, the metal melt pool, plastic strength beyond first failure, answers that say what backs them. Open: metal
distortion, the plastic print process, fresh sealed data, distortion that carries between builds, keyholes.
[flags/metal-printing/FLAG.md](flags/metal-printing/FLAG.md), [docs/contracts/lpbf-build.md](docs/contracts/lpbf-build.md)

**G21. Mechanisms and robot joints against real data** (in progress). Gears as a cutter makes them, spur and helical,
internal gears, planetary stages and gear trains of any layout, with frictionless contact solved exactly; compared with
published strain-gauge and stiffness measurements, two of them posed as trials with sealed answers (T5, T6). Open: a
measured root strain the model misses, the helical mesh stiffness, low-torque load sharing.
[docs/lab/mechanism.md](docs/lab/mechanism.md)

**G22. Surface loads on curved voxel faces** (done for the engine). Pressures, tractions and heat on a voxel mesh
corrected face by face, so a curved or inclined surface carries the load the STL surface would, verified on a ring
against closed forms; the print model's own skin follows the same rule. [docs/analysis.md](docs/analysis.md)

**G23. Printed-part geometry made by agents** (in progress). Exact parts from a feature list (Open CASCADE), skins,
cores and lattices (Manifold), printability checks, all reachable by agents over MCP and always closed and measured.
Open: the lattice core's stiffness homogenised and the part analysed against measurement. [docs/cad.md](docs/cad.md)

**G24. A physics kernel for applications nobody programmed** (in progress). A composer on MFEM where a scenario names
its mesh, equations, materials, boundaries and outputs, formulas included, and equations an agent writes itself. Open:
the verified applications rebuilt as scenarios, contact, more agent trials, and many processors and graphics cards.
The measure is the time an agent needs from this platform to a verified, measured simulation of something new.
[docs/lab/kernel.md](docs/lab/kernel.md), [challenges/agents/trials/README.md](challenges/agents/trials/README.md)
