# The physics kernel: compose an application instead of programming it (`src/lab/kernel`, G24)

Up: [the lab](README.md) · code: `src/lab/kernel/` · test: `tools/kerneltest.c` (`make test-lab3d`) · goal: [G24 in ROADMAP.md](../../ROADMAP.md)

## What it is for

The other lab domains are engines written for one kind of problem each. The kernel is the other way round: you write a
scenario that says what the problem is (the shape, the equations it needs from a library, the materials, what holds
and loads it, what to report), and the kernel assembles and solves it on [MFEM](https://mfem.org)'s finite elements. A
new application is a new scenario, not new code. Any number in a scenario may be a formula of the position, so a load
that varies, a material that is graded or an exact solution to check against is written as mathematics.

It is the first step of [G24](../../ROADMAP.md): today heat conduction, linear elasticity and the thermal strain that
couples them, on a laptop, on blocks, blocks bent by formulas and real parts read from STL files; next, contact (so
gears, bearings and snap-fits are one set of equations), more of the library, and the same scenarios on bigger
computers.

## Run it

On macOS only, for now. MFEM builds on Linux too, but the kernel's `src/lab/kernel/kn_lu.c` (and 3D magnetics'
`src/lab/magnet/spdirect.c`) solve with the sparse direct solvers of Apple's Accelerate framework, so on Linux `labrun`
answers "the physics kernel needs macOS for now" and `make mfem` does not change that. A portable sparse solver in
their place is open work ([TROUBLESHOOTING.md](../TROUBLESHOOTING.md)).

```bash
make mfem                       # once per clone, if build/third_party/mfem-4.8/libmfem.a is not there: fetches and builds MFEM (BSD), see docs/THIRD_PARTY.md
make build/labrun
build/labrun examples/lab/kernel_heated_ring.json out.lab
build/labfilm out.lab --out pictures --field von_mises --light
```

The run prints a short record; the whole of it is written beside the result as `out.lab.kernel.json`.

## A scenario, key by key

Every number carries its unit in its key. Any number may instead be a string holding a formula of `x`, `y`, `z`
(metres): `"293 + 1000*(y - 0.005)"`, `"3*pi^2*sin(pi*x)"`. Formulas know `+ - * / ^`, `pi`, `e`, and `sin cos tan asin
acos atan sinh cosh tanh exp log log10 sqrt abs floor ceil sign step`, `min max pow atan2` and `if(c, a, b)`.

```json
{
 "domain": "kernel",
 "title": "what this is",
 "mesh": {"box": {"size_m": [0.4, 0.02, 0.02], "cells": [40, 2, 2], "origin_m": [0, 0, 0]}, "refine": 0},
 "order": 2,
 "physics": ["heat", "elasticity"],
 "materials": {"steel": {"youngs_modulus_pa": 200e9, "poisson_ratio": 0.3, "conductivity_w_mk": 45,
                         "expansion_per_k": 1.2e-5, "density_kg_m3": 7850, "source": "where the values come from"}},
 "regions": [{"where": {"box": [[0, 0, 0], [0.1, 0.02, 0.02]]}, "material": "steel"}],
 "reference_temperature_k": 293,
 "loads": {"body_force_n_m3": [0, 0, 0], "gravity_m_s2": [0, 0, -9.81], "heat_source_w_m3": 0, "temperature_k": 293},
 "boundaries": [
  {"name": "clamp", "where": {"plane": ["x", 0]}, "fixed": true},
  {"name": "tip", "where": {"plane": ["x", 0.4]}, "force_n": [0, -100, 0]},
  {"name": "hot", "where": {"box": [[0, 0, 0], [0.01, 0.02, 0.02]]}, "temperature_k": 400},
  {"name": "air", "where": "all", "convection": {"h_w_m2k": 15, "ambient_k": 293}}
 ],
 "check": {"displacement_m": ["formula", "formula", "formula"], "temperature_k": "formula"},
 "outputs": {"probes": [{"name": "middle", "point_m": [0.2, 0.01, 0.01]}]},
 "picture": {"deformation_scale": 100},
 "run": {"frames": true}
}
```

| Key | Meaning |
|---|---|
| `mesh.box` | a block of hexahedra (`"tetrahedra": true` for tetrahedra); `size_m`, `cells`, `origin_m` |
| `mesh.stl` | a part from an STL file (the path from the working directory), meshed in tetrahedra by the repository's own mesher ([tet-mesh contract](../contracts/tet-mesh.md)): the file welded and checked to be a closed solid, its sharp edges and corners kept, quadratic tetrahedra whose mid-edge nodes lie on the surface when `order` is 2 or more. `element_size_m` (required) at the surface, `interior_size_m` inside (four times it if absent), `scale` turns the file's unit into metres, `origin_m` moves it, `max_elements` (300000 if absent) |
| `mesh.file` | a mesh MFEM reads (its own format, Gmsh, VTK; curved meshes too), the path from the working directory; `scale` turns the file's unit into metres, `origin_m` moves it. MFEM's sample shapes are in `build/third_party/mfem-4.8/data/` |
| `mesh.map_m` | three formulas of a box's own `x`, `y`, `z` giving each point's true position, so a box becomes a ring, a tube or a twisted bar: `["x*cos(y)", "x*sin(y)", "z"]` takes a box in (r, theta, z) onto a ring; a solid cylinder of radius R is the box [-R, R] x [-R, R] x [0, L] under `["x*sqrt(1 - 0.5*(y/R)^2)", "y*sqrt(1 - 0.5*(x/R)^2)", "z"]` (R written as a number), its side the box's four planes `x = +-R`, `y = +-R` chosen with `"space": "box"` ([kernel_fuel_rod.json](../../examples/lab/kernel_fuel_rod.json)). The mapped mesh is curved to the elements' order (`curved_order` to choose another). A mesh that folds over itself is refused (any mesh: an element's Jacobian must be positive inside it and not negative at its corners): a map whose derivative vanishes on a face, like `x^3` at `x = 0`, folds quadratic elements there, so give it a slope (`0.1*x + 0.9*x^3`) |
| `mesh.refine` | uniform refinements after loading |
| `order` | the elements' polynomial degree, 1 to 4, for every field |
| `physics` | from the library: `heat` (steady conduction), `elasticity` (linear, small strain); both: the temperature strains the solid. Heat in time, or any equation the library lacks: write it as a field ([below](#your-own-equations-fields)) |
| `materials` | per material: `youngs_modulus_pa`, `poisson_ratio` (elasticity), `conductivity_w_mk` (heat), `expansion_per_k`, `density_kg_m3` (gravity), and its `source`, required |
| `regions` | which material where, by element centres (the last match wins); one material needs none |
| `loads` | on the body: `body_force_n_m3`, `gravity_m_s2` (with densities), `heat_source_w_m3`; `temperature_k` gives the temperature field for the thermal strain when heat is not solved (with `fields`, a formula that may name them: [the solid strained by a field in time](#the-solid-strained-by-a-field-in-time)); the strain-free temperature is `reference_temperature_k`, 293.15 K if not given |
| `boundaries[].where` | `"all"`, `{"plane": ["x", value]}`, `{"box": [[x0, y0, z0], [x1, y1, z1]]}` (faces whose centres are inside), `{"point": [x, y, z]}` (the nearest vertex), `{"cylinder": {"point_m": [x, y, z], "axis": [dx, dy, dz], "radius_m": r}}` (faces whose centres, taken on the mesh's own curved surface, lie at r from the axis: a bore, a pin, a shaft; an STL's cylinder is faceted, so by default a thousandth of r either side is accepted, enough from about 70 facets around), `{"attribute": n}` (a mesh file's own boundary number); `tolerance_m` widens it; `"space": "box"` chooses in a mapped box's own coordinates, before `map_m` (a ring's end is then the plane `["y", theta]`). A face may be in several boundaries |
| elasticity on a boundary | `fixed`, `fix: ["x", "z"]` (some components), `displacement_m` (prescribed, null for free), `traction_pa`, `pressure_pa` (pushes in), `force_n` (spread evenly over the area) |
| heat on a boundary | `temperature_k`, `heat_flux_w_m2` (into the body), `convection: {h_w_m2k, ambient_k}`; unnamed faces are insulated |
| contact on a boundary | `contact`: a rigid obstacle these faces may touch, without friction: `{"plane": {"point_m": [x, y, z], "normal": [nx, ny, nz]}}` (the normal toward the body), `{"sphere": {"center_m", "radius_m"}}`, `{"cylinder": {"point_m", "axis", "radius_m"}}` (the body outside both), or `{"distance": "a formula of x, y, z"}` (the signed distance, positive on the body's side); `penalty_pa_m` (if absent, ten times the stiffness of each point's own nodes over its area), `report_points` (every contact point's place and pressure in the record) ([below](#contact-with-rigid-obstacles)) |
| `check` | exact solutions: their L2 errors and norms go into the record (a manufactured solution checks a new equation) |
| `outputs.probes` | points where the fields and the stress are reported: `temperature_k`, `displacement_m`, `von_mises_pa`, `stress_pa` (xx, yy, zz, xy, yz, xz) and `principal_pa` (the principal stresses, largest first); a field interpolated at the point from the element that holds it, the stress that element's own value there |
| `outputs.lines` | `[{"name", "from_m": [x, y, z], "to_m": [x, y, z], "points": 21}]`: a line sampled at evenly spaced probes, gathered in the record as arrays along it (`lines.NAME.s_m`, the distance from its start, and one array per probed quantity); a curvature or a profile is read from them |
| `picture.deformation_scale` | how much the displacement is exaggerated in the frame |
| `run.frames` | false: the numbers only |

## Holding a body that is otherwise free

A body held by nothing has six rigid motions and its system cannot be solved. Hold only what removes them, so that the
support carries no load: a symmetry plane holds one component on its faces (`"fix": ["x"]` on the plane x = 0), and
points finish the job (`{"point": [x, y, z]}` with `fix`): three points holding x, y and z, then y and z, then z remove
the six motions and restrain nothing else (`tools/kerneltest.c` K6 and K17, the bimetal trial). The reactions in the
record then come out at round-off, which shows that the support carries nothing. A body pressed on a rigid obstacle is
held by it where it touches and needs points only for the motions the obstacle leaves free (K18).

## Your own equations: fields

When the library has no equation for what you need, write it. Each field u you name obeys

    mass du/dt - div(diffusion grad u) + convection . grad u + reaction u = source

and every coefficient is a number or a formula of `x`, `y`, `z`, `t`, of any field's value (by its name) and gradient
(`dx(name)`, `dy(name)`, `dz(name)`), and of the material's properties (by their keys, so one formula serves several
materials). Heat is a field with mass rho c_p and diffusion k; an electric potential one with diffusion sigma; a
concentration one with diffusion D and the flow's velocity as convection; sound at one frequency one with diffusion 1
and reaction -k^2. A formula that names a field makes the fields depend on each other: they are solved in turn until
they settle.

```json
"materials": {"copper": {"electrical_conductivity_s_m": 5.8e7, "conductivity_w_mk": 401, "density_kg_m3": 8935,
                         "specific_heat_j_kgk": 384.6, "source": "where the values come from"}},
"fields": {
 "phi": {"diffusion": "electrical_conductivity_s_m / (1 + 0.00393*(T - 293.15))", "unit": "V"},
 "T": {"mass": "density_kg_m3*specific_heat_j_kgk", "diffusion": "conductivity_w_mk", "initial": 293.15, "unit": "K",
       "source": "electrical_conductivity_s_m / (1 + 0.00393*(T - 293.15)) * (dx(phi)^2 + dy(phi)^2 + dz(phi)^2)"}
},
"boundaries": [
 {"name": "in", "where": {"plane": ["x", 0]}, "flux": {"phi": 6.67e6}, "value": {"T": 293.15}},
 {"name": "out", "where": {"plane": ["x", 0.1]}, "value": {"phi": 0, "T": 293.15}},
 {"name": "air", "where": "all", "exchange": {"T": {"h": 10, "ambient": 293.15}}}
],
"time": {"end_s": 1800, "step_s": 10, "method": "bdf2", "outputs": 18}
```

| Key | Meaning |
|---|---|
| `fields.NAME` | a field and its equation: `mass`, `diffusion` (one value; three for a diagonal; nine for a full tensor, row by row), `convection` ([vx, vy, vz]), `reaction`, `source`, `initial` (a formula of x, y, z), `order` (the scenario's if absent), `unit` |
| on a boundary | `value: {NAME: ...}` holds the field; `flux: {NAME: q}` puts q per unit area in (`diffusion` times the outward derivative); `exchange: {NAME: {"h": ..., "ambient": ...}}` lets h (ambient - u) in. A boundary that names nothing lets nothing through. A point may hold a value (a number or a formula of x, y, z, t) |
| `time` | `end_s`, `step_s`, `method` (`bdf2`, second order, the default; `backward_euler`, first order), `outputs` (the frames and the entries of `series`: spread evenly over the steps, at most one a step, 20 or every step if fewer; the start, t = 0, comes as one more). Without it the fields are steady. A field without mass follows the others at each instant |
| `outputs.crossings` | `[{"probe": a probe's name, "field": a field's name, "value": v}]`: the first time the probe's field crosses v, watched at every step and placed within the step by a straight line (`crossings` in the record: `first_s`, `direction`, or `first_s` null if it never does) |
| `solver` | `tolerance` (1e-12: the fields are solved in turn until the largest change is under it, relative to their size) and `max_iterations` (50) |
| `check.fields` | exact solutions, formulas of x, y, z, t, compared at the end |

The record then holds `fields.NAME` (`min`, `max`, `integral` over the body, `dofs`, the `system` it made: symmetric
by Cholesky, non-symmetric, with convection, by LU, and how many `factorizations`), `boundaries.NAME` (`faces`,
`area_m2`, and `flow_in` of each field it names at the end: where the field is held, the residual of its system, so
that the flows balance the sources; elsewhere its flux and exchange; a degree of freedom on the edge between two held
boundaries counts for both), `solver` (`coupled`, `converged`, `iterations_max`, `last_change`), `time`, `series`
(each output time: every field's integral and probe values), `crossings` and `probes` (the final values). The frames
carry every field on the body's surface, one frame per output time, so `build/labfilm` makes a film of them. Fields
and the library's heat are not combined: write heat as a field; the library's elasticity may follow it
([below](#the-solid-strained-by-a-field-in-time)).

## The solid strained by a field in time

With `"physics": ["elasticity"]` beside `fields`, `loads.temperature_k` may name the fields (`"T"`, or any formula of
them, `"T - 0.5*dz(T)"` if one wished): at every output time, steady or in time, the solid is solved again with that
temperature's thermal strain (each material's `expansion_per_k`, stress-free at `reference_temperature_k`), its
stiffness assembled and factored once. Its other loads and holds are the library's, constant in time; it does not act
back on the fields (no heat from deformation), its materials do not depend on the temperature yet, and contact is not
followed in time yet. The record adds `results.max_von_mises_pa`, `max_principal_pa` (the most tension) and
`min_principal_pa` (the most compression), each with its place (`_at_m`) and the output time it came at (`_t_s`), the
largest displacement, the reactions at the last output time; each `series` entry its own peaks
(`max_von_mises_pa`, `max_principal_pa`, `min_principal_pa`) and its probes' `displacement_m`, `stress_pa`,
`principal_pa` and `von_mises_pa`. The frames add `von_mises`, `max_principal` and `displacement`, the shape deformed
by `picture.deformation_scale`. K21 checks it against a bar heated at one end (to 5e-13 of the step's stress through
the field's own temperature, 0.008 % from erfc's). A glass plate plunged into water, its surface in tension while its
middle is still hot: [kernel_glass_quench.json](../../examples/lab/kernel_glass_quench.json) (about 15 s). Its face's
centre peaks at 7.13 MPa, 3.1 % under Manson's estimate for an infinite plate (7.37 MPa); the plate's edge, cooled
from two sides, reaches 9.02 MPa, which the formula cannot see. The water's 2000 W/(m^2 K) is assumed: a
demonstration.

## Contact with rigid obstacles

A boundary with `contact` may touch a rigid obstacle, without friction: the body may not cross it and is pushed back by
a pressure where it touches (G24 step 3, the first part; contact between two elastic parts comes next). The contact is
solved by an augmented Lagrangian: Newton's method on the active set with a penalty, the multipliers (the pressures)
updated until the deepest penetration is under `solver.contact_tolerance` (1e-8) of the largest displacement, at most
`solver.contact_iterations` (200) updates. The constraints sit at the faces' nodes (the Gauss-Lobatto points of
quadrilateral faces, one per node, shared nodes merged); triangular faces (tetrahedral meshes from STL files) use Gauss
points, which may make the pressure oscillate between neighbouring points (K19's first run). Small strain only, and one
state: a moving obstacle is not followed in time yet.

The penalty sets how fast the multipliers settle, not the answer: by default each point's spring is ten times the
stiffness of its own nodes (the system's diagonal there), and the system is factored again only when the set of
touching points changes. A point without area carries no force and is left out; where a map has no slope, as K19's at
its contact line, the nodes keep only a trace of area, carry almost nothing and read pressure 0. A rigid ball pressed
0.04 mm into a steel block, a quarter of it by symmetry, solves in about 13 s with 4 multiplier updates:
[kernel_ball_on_block.json](../../examples/lab/kernel_ball_on_block.json). Its force lands 4.2 % over Hertz's for a
half-space and its peak pressure 2.4 % over his; the block is 10 mm and held at its base, not a half-space, so this is
a demonstration, not a verification.

The record gives `results.contact` (`newton_iterations`, `multiplier_updates`, `factorizations`, `converged`,
`points`) and, for each contact boundary, `boundaries.NAME.contact`: `force_n` (the obstacle's resultant on the body),
`area_m2` (where it touches), `max_pressure_pa`, `max_penetration_m`, and `points` (`[x, y, z, pressure]` each) with
`report_points`. The held boundaries' reactions include the contact. The frames carry `contact_pressure` at the
vertices.

## What comes back (`OUT.lab.kernel.json`)

- `results`: elements, degrees of freedom, `max_von_mises_pa` and `max_von_mises_at_m` (the largest over every
  element's quadrature points and nodes, each element's own value, so that a peak on the surface is found; quadrature
  points alone read the C-ring's 3.5 % under. At a held edge or a sharp inner corner the largest value grows as the
  mesh is refined: a singularity of the model there, not a strength), `max_principal_pa` and `min_principal_pa` with
  their places (the most tension and the most compression, searched the same way: what a brittle part such as glass,
  a ceramic or cast iron fails by), `max_displacement_component_m`,
  `min_temperature_k`, `max_temperature_k`, the factor's size and the times.
- `boundaries.<name>`: `faces`, `area_m2`, `reaction_n` (where something is held: the residual of the unconstrained
  system, so the reactions balance the loads to round-off), `mean_displacement_m`, `heat_in_w` (held: from the
  residual; convection and flux: from the condition), `mean_temperature_k`.
- `mesh` (a part from an STL): its `tetrahedra`, `nodes`, `stl_triangles`, `patches`, `sharp_edges_kept`, the dihedral angles (`min_dihedral_deg`, `max_dihedral_deg`), `volume_m3` beside `stl_volume_m3`, `surface_distance_max_m` (how far a face's centre is from the file's surface) and the mesher's `seconds`.
- `probes`, `check` as asked. The frame is the body's surface with `von_mises`, `max_principal`, `displacement` and
  `temperature` at its vertices.

## Verification (`tools/kerneltest.c`)

Each case is a scenario run through the lab's entry point, as an agent runs one. Criteria written on 2026-10-08 before
the first run:

| Case | Against | Criterion | Result 2026-10-08 |
|---|---|---|---|
| K1 elasticity, manufactured solution, unit cube | the L2 error's rate | at least p + 0.75 for p = 1, 2 | 1.99 and 3.01 |
| K2 heat conduction, manufactured solution | the same | the same | 2.00 and 2.97 |
| K3 a slender cantilever, quadratic elements on 40 x 2 x 2 | Timoshenko's beam | 1 % | -0.46 % |
| K4 a heated block: free, then held at both ends | alpha dT L and no stress; -E alpha dT and its reaction | 1e-9 | exact to 1e-9 |
| K5 a slab held at 400 K, cooled by convection | the heat balance and the closed form | 1e-9 | 1e-12 |
| K6 a free bar bent by a linear temperature | the exact arc, no stress | 1e-7 (1e-9 before the first run, see below); 1e-6 | 2.9e-9; 2e-9 |
| K7 the C-ring of `tools/printmesh_ccx.py` (G17), a box mapped onto the ring by formulas, 4 x 120 x 4 quadratic elements | Winkler-Bach's curved beam, the hoop stress at the apex's inner fibre, mid-width | 2 % | +2.53 %: failed, printed and not counted since (below) |
| K7b the same ring, 8 x 120 x 4 | CalculiX 2.23 solving the same 3D problem on 8 x 160 x 8 C3D20R elements, at three places | 1 % | +0.16 %, +0.08 %, +0.18 % |
| K8 a thick-walled tube from an STL file, the repository's mesher at 1 mm, quadratic tetrahedra, 10 MPa in the bore | Lame (free ends, exact in 3D): the hoop stress and the bore's growth on x and y, the largest von Mises | 2 %, 1 %, 3 % | -0.16 %; -0.004 %, -0.014 %; +0.74 % |
| K9 the engineering core's wall brackets A and B ([bracket_comparison](../../examples/bracket_comparison/README.md)) from their STL files, 4 mm tetrahedra, quadratic and cubic | the core's own study: A's pad displacement against its extrapolated 0.025603 mm; both settled from quadratic to cubic; B under A; the wall's reaction | 2.4 % (the core's convergence index), 2 %, 1e-6 | -0.45 %; -0.52 % and -0.36 %; B / A 0.333; exact |
| K10 a field quadratic in space with e^-t in time, 2 x 2 x 2 quadratic cells, 5, 10 and 20 steps to t = 1 | its exact solution (all the error left is the time stepping's) | order 0.9 (backward Euler), 1.8 (BDF2) | 1.026, 2.053 |
| K11 heat soaking into a steel bar from one end, written as a field, BDF2 in steps of 0.1 s | the semi-infinite solid's erfc at 5, 10 and 20 mm after 20 s | 0.5 % of the 100 K step | 0.000 % (1e-4 K) |
| K12 Joule heating: a potential and a temperature, the heat source written with dx(phi) | phi linear, T a parabola (quadratic elements hold both) | 1e-9 | 3e-15, 4e-12 |
| K13 a conductivity that varies with the temperature, a formula of the field itself | Kirchhoff's transform at mid-thickness; the iteration settling | 1e-4; 50 iterations | 8e-14; 13 iterations |
| K14 convection and diffusion at Peclet 10, a non-symmetric system (LU) | the exact boundary layer (e^(Pe x) - 1) / (e^Pe - 1) | 1e-4 rms | 2.5e-6 |
| K15 the first time a probe crosses a value: K11's bar, 350 K at 10 mm | erfc's time, 8.7924 s | 0.5 % | +0.001 % |
| K16 the flow of a field through each boundary: K13's slab | Kirchhoff's 67.5 W in through the hot face and out through the cold | 1e-6 | 5e-13 |
| K17 a bimetal strip (124 and 148 GPa, 26.46e-6 and 1.30e-6 /K, Poisson's ratio 0) heated by 80 K, free | Timoshenko's bimetal formula (1925), the curvature at mid-length | 0.1 % | -0.0000 % |
| K18 a steel block on a frictionless rigid plane, 10 MPa on its top | uniform contact pressure, the load carried, no penetration | 1e-6, 1e-9, 1e-6 | 5.9e-8, exact, 9e-10 (1.7e-14, exact, 7e-17 before the penalty change, below) |
| K19 Hertz's line contact: a cylinder (radius 10 mm) pressed 0.05 mm on a rigid plane, plane strain | equilibrium; Hertz's peak pressure and half width from the resultant | 1e-6; 3 %; 10 % | 1e-10; +0.25 %; -3.3 % (fourth run, below) |
| K20 a field sampled along a line: K12's bar | the exact parabola at 21 points | 1e-9 | 4e-12 |
| K21 thermal stress in time: K11's bar heated at one end, its sides on rollers, the library's elasticity following the field | the transverse stress from the field's own temperature; from erfc's; no axial stress | 1e-6; 0.5 %; 1e-6 of the step's stress | 5e-13; 0.008 %; 1e-12 |
| K22 principal stresses: a block under a uniform strain with shear | the eigenvalues of Hooke's stress by Jacobi's rotations, in the body and at a probe | 1e-9 | 1.1e-14 |

K6 failed its first criterion: the displacement matched the exact field to 2.9e-9 of its size, over the 1e-9 asked.
On twice the cells the difference grows (1.2e-8) and on cubic elements it stays (3.0e-9): round-off, which grows with
the system's conditioning, not discretisation (quadratic elements hold this quadratic field exactly). The criterion is
now 1e-7, with the note in the file.

K7 failed: the hoop stress at the apex's inner fibre came out 26.48 MPa, 2.53 % over Winkler-Bach's 25.82. The same
scenario refined (up to 16 x 160 x 8 cells, and cubic elements) settles 1.8 to 1.9 % over Winkler-Bach at mid-width
and 2.1 % under it on the ring's flat faces; the first mesh added about 0.7 % of its own. Winkler-Bach is a beam's
theory, one stress across the width, and on this square section the stress varies by 4 % across it, so its 2 % left
little room for the discretisation. CalculiX, solving the same 3D problem, finds the same shape across the width:

| Hoop stress at the apex (MPa) | inner fibre, mid-width | inner fibre, flat face | outer fibre, mid-width |
|---|---|---|---|
| Winkler-Bach (a beam, one value across the width) | 25.82 | 25.82 | 19.51 |
| CalculiX, 8 x 160 x 8 C3D20R (K7b's reference) | 26.22 | 25.29 | 19.41 |
| CalculiX, 12 x 160 x 12 C3D20R | 26.25 | 25.32 | 19.40 |
| kernel, 8 x 120 x 4 quadratic elements (K7b) | 26.26 | 25.31 | 19.45 |

(isotropic, E 3426.9 MPa, nu 0.323, as `python3 tools/printmesh_ccx.py --iso` builds the ring; the stress depends on nu
only through the variation across the width.) K7 is still run and printed as it was written; K7b, written before
CalculiX's isotropic solution was computed, is what counts. The 0.7 % first quoted for CalculiX in K7's text was its
Tsai-Wu strength ratio for the printed material, not this hoop stress. The ring as a scenario:
[kernel_c_ring.json](../../examples/lab/kernel_c_ring.json).

K8 and K9 passed on their first runs. K9 is the engineering core's flagship comparison rebuilt from its files alone: the
core's voxels give the chamfered bracket B a staircase that changes with the mesh, so its study could not call B
converged (-3.53 % from 2 to 1 mm); on tetrahedra that keep the chamfer's sharp edges the kernel settles within 0.36 %
from quadratic to cubic elements at 0.008474 mm, 3.4 % under the core's 1 mm voxels, B / A 0.333 (the core: 0.346 at
1 mm, 0.338 at 0.5 mm in its independent evaluation). The two designs as scenarios:
[kernel_bracket_a_plain.json](../../examples/lab/kernel_bracket_a_plain.json),
[kernel_bracket_b_chamfer.json](../../examples/lab/kernel_bracket_b_chamfer.json) (cubic elements, about 10 s each).

K10 to K14 passed on their first runs (criteria written 2026-10-08 before any of the fields' code existed), and so did
K15 to K17, K20, K21 and K22. K18 printed FAIL on its first run with every number inside its criterion: the test
counted the contact points after freeing the record that held them. K19 failed twice before passing, its criterion
unchanged: first the peak pressure came out 31.6 % over Hertz's with the multipliers unsettled, the contact enforced
at every Gauss point of the quadratic faces (more constraints than nodes); then, at the faces' nodes, +26.8 %, because
a node shared by two faces was kept twice and the load split arbitrarily between its copies (their average was
Hertz's). Merged into one constraint per node, the peak pressure lands 0.24 % from Hertz's and the profile follows his
ellipse within 0.2 %. A fourth run followed two changes to the solver, not to the criterion (the penalty scaled to the
nodes' stiffness, the system factored again only when the touching set changes;
[above](#contact-with-rigid-obstacles)): +0.25 %, in 15 Newton iterations and 5 multiplier updates instead of 131 and
27. K18 passed again with its pressure uniform to 5.9e-8 instead of 1.7e-14: the penalty now differs between a face's
corner, side and centre nodes, and the multipliers stop at the tolerance with a trace of that difference.

Assembly is the slow part on large meshes: a problem whose matrices do not change in time is assembled and factored once
and its loads too when nothing in them varies; a coefficient that depends on a field is assembled again at every
iteration (on hexahedra with p + 1 Gauss points a direction, on tetrahedra with MFEM's own rules).

## Not yet

Contact between two elastic parts, friction, and obstacles that move in time; plasticity and large deformation;
materials that depend on the temperature in the solid, and loads on it that vary in time; vector fields of one's own
(flow, electromagnetism); waves (a second time derivative); many processors and graphics cards: these are G24's next
steps. Assembly is not yet fast on curved high-order meshes (the ring example: 34 s of its 42 s).