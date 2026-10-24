# `src/fem/` — finite elements and time integration

The numerical core. Nothing here knows about projects, files, MCP or the app: it takes a mesh, materials and loads,
and returns a solution. That separation is why the same solver serves the app, the socket and an agent.

Up: [module map](../../docs/map/MODULES.md#srcfem--finite-elements-and-time-integration) · [AGENTS.md](../../AGENTS.md).

## Read first

| File | What it gives you |
|---|---|
| `solid.h` | `solid_solve` — static small-strain linear elasticity; and `solid_check_constraints`, which reports rigid-body motion *before* a solve |
| `thermal.h` | transient heat conduction: theta method, Picard iterations, enthalpy formulation with phase change, discrete energy balance |
| `orchestrator.h` | one lifecycle for coupled solvers: participants, plan, iterated cycles with Aitken relaxation, commit-all or rollback-all |
| `hex8.h` | the element: shape functions, full and incompatible-mode stiffness, initial-strain loads, stress recovery |
| `tet.h` | TET4 and quadratic TET10: elements, loads on curved faces, stress recovery, and the static solve `solid_solve` dispatches to when `HexModel.elem_type` is a tetrahedron (with a two-level PCG whose coarse space is the TET4 problem) |
| `sparse.h` | CSR assembly, PCG and sparse Cholesky |

Also here: `thermal_integrator.h` (transactional fixed and adaptive steps), `timestep.h` (event schedule, PI
controller), `thermal_participant.h`, `krylov.h` (BiCGSTAB + ILU(0) for advection), `flow.h` (a steady laminar field
from the lattice Boltzmann kernel, used for conjugate heat transfer), `dense.h`.

## Talks to

- [`geom/`](../geom/README.md) — the hex mesh it solves on.
- [`ctl/`](../ctl/README.md) — `static_run.c` and `thermal_run.c` call in from a job; results go back as arrays.
- [`core/`](../core/README.md) — only for basic utilities.

## Tests

```bash
make headless
./build/femtest      # against exact and analytical solutions
./build/tettest      # the tetrahedral mesh and the TET4 / TET10 elements: patch, cantilever, Kirsch, fillet, balance
./build/thermtest    # conduction, phase change (Stefan), energy balance
./build/thermtest --radiation # independent polynomial face flux, tangent, nonlinear and transient radiation checks
./build/tsteptest    # adaptive integration
./build/orchtest     # the orchestrator, with thermal participants
./build/advtest      # nonsymmetric solver and advective transport
```

The structural path is additionally cross-checked against CalculiX 2.23 in
[../../release/evaluation/EVALUATION_REPORT.md](../../release/evaluation/EVALUATION_REPORT.md) §2 (same assembled
problem to ~1e-7; an independently built model to 0.0015 %). Stage evidence for the thermal work:
[../../Thermal Sim/verification/](../../Thermal%20Sim/verification/).

Radiation uses the interpolated temperature at 3 x 3 surface quadrature points for its Stefan-Boltzmann flux,
Newton tangent and energy ledger. This integrates bilinear temperatures exactly on planar parallelogram faces;
warped-face geometry remains a quadrature approximation. Accurate integration on a voxel face does not correct
the staircase surface-area bias relative to the original STL.

## Known numerical limits

- Small strain, linear elasticity: large deformation is **detected and reported**, not modelled.
- Stress at a sharp re-entrant corner or an ideal support is singular and grows with refinement; it is flagged, never
  reported as a design value.
- One recorded criterion is **not met**: the enthalpy-flux form of the energy balance in conjugate heat transfer
  closes only to 2.4e-4 of the source because the mapped flow is not discretely divergence-free
  (`Thermal Sim/STATUS.md`, stage E5c).
