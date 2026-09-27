# Thermal Sim

Heat transfer, thermal energy storage and phase change inside the NAVIER multiphysics application.

This is a **module, not a separate program**. It shares the project format, material library, geometry and selection
system, job manager, result files, software renderer and MCP server with Fluid Sim and the structural solver. There is
one material database, one project format, one job manager and one MCP server; Thermal Sim adds physics to them rather
than duplicating them.

The code is in the parent repository:

| Path | What |
|---|---|
| `src/fem/thermal.c`, `thermal.h` | the numerical core: enthalpy balance, conduction, boundary conditions, interfaces, sources, energy diagnostics |
| `src/ctl/thermal_model.c` | builds a self-contained case from a project setup |
| `src/ctl/thermal_run.c` | the job: time stepping, sequential structural solves, summary |
| `src/ctl/thermal_io.c` | `results.nvt`, VTU series, CSV time history |
| `src/ctl/ops_transient.c` | the operations an AI host calls |
| `tools/thermtest.c` | verification against analytical solutions (72 checks) |
| `tools/thermflow.py` | the complete workflow over MCP (89 checks) |

## What it solves

The enthalpy form of the energy balance for a fixed solid,

```
    ∂(ρh)/∂t = ∇·(K ∇T) + Q
```

with temperature-dependent or anisotropic conductivity, latent heat of melting, prescribed temperatures, heat flux,
convection, grey-body radiation to an ambient, volumetric and moving sources, finite-conductance interfaces, and
element activation for additive manufacturing. One-way thermomechanical coupling drives thermal strain from the
temperature history.

Since the 2026-09-16 release it also solves a **conjugate heat transfer** study (`analysis_run` with
`conjugate_heat_transfer`). A steady laminar flow from the application's lattice Boltzmann solver carries the energy of
a fluid body (SUPG advection). That fluid exchanges heat with the solid bodies it touches through a resolved interface
(no convection coefficient), coupled partitioned or monolithically through the shared orchestrator, and the solids can
expand thermally. Every transient job runs through that orchestrator, and every job can be checkpointed, paused and
resumed.

Full statement of the equations, assumptions, discretisation, valid ranges and unsupported regimes:
**[`docs/theory.md`](docs/theory.md)**.

## Documents

| Document | What it is for |
|---|---|
| [`docs/theory.md`](docs/theory.md) | governing equations, constitutive relations, discretisation, diagnostics, verification results, declared unsupported regimes |
| [`docs/capability-matrix.md`](docs/capability-matrix.md) | every feature marked unavailable / experimental / verified / validated, with its evidence |
| [`docs/audit.md`](docs/audit.md) | the baseline audit: what was actually in the repository, what the tests really covered, and every defect found |
| [`STATUS.md`](STATUS.md) | resumable checkpoint: build commands, test results, known gaps in priority order, the next task |
| [`verification/`](verification/) | raw logs of the verification runs, and per-stage acceptance criteria and evidence tables for the 2026-09-16 release |
| [`examples/`](examples/) | demonstration studies with their saved specifications and exports, including `make_three_domain.py` (heated plate in an air channel with thermal expansion) |

**Nothing in this module is validated against physical measurements.** Every result is verified against an analytical
or manufactured solution or against a second numerical path, and every library material (including `air_demo`) is
labelled `demonstration`. The capability matrix keeps the
distinction between verification, validation and calibration explicit.

## Quick start

```bash
# from the repository root
make                 # GUI + navier-server, navier-mcp, navier-ctl
make test            # 1015 checks across 13 suites
```

Verification and the full workflow on their own:

```bash
./build/thermtest                                 # 72 analytical checks
python3 tools/thermflow.py                        # 89 checks through MCP
python3 "Thermal Sim/examples/make_examples.py"   # rebuild the demonstration studies
```

Connecting an AI host (Claude Code, Codex) is described in `docs/mcp-clients.md` of the parent repository. The control
protocol is in `docs/control-protocol.md`.

## A complete thermal study through MCP

The operations an AI host calls, in order. This is exactly what `tools/thermflow.py` drives.

```
capabilities_get                       # what is available, and what is declared unavailable
project_create  /  project_open
geometry_import        path, units
material_define        conductivity (isotropic, or principal values + axes), density, specific heat,
                       solidus/liquidus/latent heat, provenance and status
material_assign        body, material, source
mesh_generate          element_size
selection_create       named surface queries (plane, facing, patch, box, pick, ...)
boundary_apply         temperature | heat_flux | convection | radiation | heat_source
setup_validate         analysis: transient_thermal          -> ready, errors, warnings, assumptions
analysis_run           analysis: transient_thermal | thermomechanical
                       end_time, time_step, initial_temperature, theta, capacity,
                       property_evaluation, capacity_model, phase_change, ...
job_status             progress, then the summary with the energy budget
results_query          quantity: temperature | displacement | von_mises
results_probe          points_mm, history
results_render         a PNG with legend, units and colour scale
results_export         formats: vtu | csv | summary
```

Every run writes a hashed `spec.json` holding the resolved setup — geometry and mesh revisions, resolved materials
with their provenance, every condition, the solver and constitutive choices, and the software version and
floating-point settings. **A study is reproducible from that file without any AI interpretation.**

## Reading a result honestly

Every thermal run reports an energy budget. Two numbers in it mean different things and both matter:

* `closure_error` — did the discrete equations balance? Small means the linear algebra worked.
* `enthalpy_mismatch` — does the energy the capacity matrix stored equal the energy the *enthalpy function* says
  should have been stored? This is computed by a different code path, from `H(T)` at the Gauss points, with no
  reference to the assembled matrix. Small means the **model** conserves energy, not just the solve.

A capacity model that quietly loses latent heat would still show a tiny `closure_error`, because the same wrong
capacity appears on both sides of the balance. It would not survive `enthalpy_mismatch`. Measured values are ~1e-13
even while a block is melting.

Also read the warnings. The module states which modelling choice it made — `PHASE_CHANGE_ACTIVE`,
`PHASE_CHANGE_DISABLED`, `ANISOTROPIC_CONDUCTIVITY`, `DEMONSTRATION_MATERIAL` — rather than applying one silently.
