# Baseline audit — what was actually there on 2026-09-16

Milestone 1 of the Thermal Sim brief: reconcile the source, the documentation and the tests that actually run, and
classify every piece rather than trusting `STATUS.md`. Everything below was established by building and running the
code, not by reading it.

## Method

```bash
make headless          # clean build, 0 warnings
make test              # every headless suite
python3 tools/mcptest.py; python3 tools/amflow.py   # independent MCP clients
```

plus a scripted MCP session that drove `capabilities_get` → `project_create` → `geometry_import` →
`material_assign` → `mesh_generate` → `selection_create` → `boundary_apply` → `setup_validate` → `analysis_run` →
`job_status` → `results_query`, to find out what is reachable from an AI host rather than from C.

## Headline finding

**`STATUS.md` understated the repository.** It listed milestone 4 (transient thermal) as "next, not started". In fact
`src/fem/thermal.c` (789 lines), `src/ctl/thermal_model.c`, `thermal_run.c`, `thermal_io.c`, `ops_transient.c` and a
41-check verification suite `tools/thermtest.c` all existed and passed, and `analysis_run` with
`analysis: "transient_thermal"` ran end to end over MCP and produced a physically sensible temperature history.

The lag was not harmless. Two user-visible surfaces still declared the feature missing:

* `capabilities_get` listed `transient_thermal` and `thermomechanical` with `status: "planned, not available"`;
* `model_limitations` asserted *"No transient thermal, thermo-mechanical, FFF or LPBF process simulation is available
  in this build yet."*

An AI host reading the capability document — which is exactly what it is for — would have concluded the analysis could
not be run and would have declined the request or substituted something else. This was the single highest-priority
defect found and is fixed.

## Baseline test results (before any change, this machine)

| Suite | Result |
|---|---|
| `build/coretest` | 199 passed, 0 failed |
| `build/surftest` | 30 passed, 0 failed |
| `build/meshtest` | 29 passed, 0 failed |
| `build/femtest` | 42 passed, 0 failed |
| `build/thermtest` | 41 passed, 0 failed |
| `build/rendertest` + `tools/pngcheck.py` | 29 passed, 0 failed; PNG check passed |
| `build/opstest` | 111 passed, 0 failed |
| `build/ctltest` | 47 passed, 0 failed |
| `build/amtest` | 79 passed (102 after the thermal ops were exercised), 0 failed |
| `tools/mcptest.py` | 194 passed, 0 failed |
| `tools/amflow.py` | 71 passed, 0 failed |

Clean build, zero warnings, `-O2`, no `-ffast-math`, `-ffp-contract=off` for the numerical core.

## Classification

### 1 — Implemented and independently verified

* JSON, schema validation, unit parsing, SHA-256, base64, path restriction (`src/core`).
* Surface topology, patch extraction, voxel hex8 meshing with quantified staircase error.
* Static structural FEM: patch test, rigid-body modes, cantilever convergence against Timoshenko, gravity,
  initial (thermal) strain, constraint analysis.
* Transient conduction core: steady 1-D, Kirchhoff-transform temperature-dependent `k` with O(h²) convergence,
  semi-infinite `erfc`, lumped cooling with first/second-order time convergence, radiation equilibrium, fin with
  lateral convection, moving-source energy, element activation.
* Control socket and MCP protocol, including framing, limits, timeouts, disconnects and older protocol revisions.

### 2 — Implemented but insufficiently tested

* Transient thermal **through the operation layer**: the C core had 41 checks, but nothing tested
  `analysis_run`/`results_query`/`results_export` for a thermal run the way `amflow.py` tested the structural path.
  A whole class of defects — wrong schema keys, missing dispatch, results that cannot be queried — was invisible.
  *Resolved:* `tools/thermflow.py`, 89 checks, now in `make test`.
* Thermomechanical coupling: exercised in the C core only.
* Radiation linearisation: equilibrium was tested; transient radiative cooling against a reference was not.

### 3 — Partially connected to the application

* `analysis_run` accepted `transient_thermal` in the schema and dispatched correctly, but `capabilities_get` denied
  it existed (above).
* `results_query` on a thermal run defaulted to a structural quantity and failed with
  `UNSUPPORTED: this run stored temperatures only` unless `quantity: "temperature"` was passed explicitly.
* **Latent heat, solidus and liquidus were parsed, validated, stored in `MaterialRecord` and exposed in the material
  schema — and then never reached the solver.** Two of the five library materials (`ss316l_lpbf_demo`,
  `ti6al4v_lpbf_demo`) carry 270 and 286 kJ/kg of latent heat that was silently discarded.
  *Resolved:* wired through `thermal_model.c` with `PHASE_CHANGE_ACTIVE` / `PHASE_CHANGE_DISABLED` /
  `LATENT_HEAT_IGNORED` warnings so the choice is always stated.
* `setup_validate` for a transient analysis returned `ready: false` with *"the analysis needs a positive end_time and
  time_step"* and no hint, because the schema gives those keys no defaults.

### 4 — Planned or missing

Advection and conjugate heat transfer; enclosure radiation; adaptive time stepping; conforming meshes; FFF/LPBF
process analyses; a multiphysics orchestrator; Streamable HTTP transport.

## Findings in the thermal solver itself

Against the audit list in the brief. Items marked **fixed** were changed in this work; the rest are recorded with
their status.

| # | Finding | Status |
|---|---|---|
| 1 | **Conductivity was evaluated once per element at the element mean temperature** and multiplied into a cached Laplacian. First-order in the property variation; smears any steep gradient. | **fixed** — Gauss-point evaluation (`property_evaluation`), with the exact cached path kept for constant isotropic `k`. Cost measured at ~8 %. |
| 2 | **Only isotropic conductivity.** No tensor, no material frame. | **fixed** — three principal values plus an orthonormality-checked rotation; patch test and three rotated slab cases. |
| 3 | **Capacity was `ρ(T_m) c_p(T_m)` at the mid-step mean temperature** — a midpoint approximation of the enthalpy change, not the enthalpy change. Latent heat had nowhere to live. | **fixed** — secant capacity `[H(T¹)−H(T⁰)]/(T¹−T⁰)` at Gauss points, which makes the discrete stored energy identically the quadrature of the enthalpy change. |
| 4 | `enthalpy_change` was computed per step from `∫c_p dT` at element mean temperatures but **never accumulated, reported or compared** with the discrete stored energy. The independent check existed and was thrown away. | **fixed** — accumulated, reported as `enthalpy_change_j`, and compared as `enthalpy_mismatch` per step and per run. |
| 5 | **Moving-source power renormalisation on by default**: power that geometrically misses the body is redistributed onto it. | already an explicit choice (`raw_source`) with `source_scale` and `source_power_mesh` reported; documented in `docs/theory.md` §4.2 as a modelling decision, not a correction. |
| 6 | **Under-resolved source fallback** deposits the whole power in the element under the source point. | already flagged (`source_fallback`); documented as something never to accept silently. |
| 7 | **Phase change was not counted as a nonlinearity**, so with constant `k`, `c_p`, `ρ` tables the Picard loop exited after one iteration. | **fixed** — found by the new melt/re-freeze test, which failed by 17 K and 50 % of the energy before the fix. |
| 8 | **Picard diverges when an element crosses the mushy zone in one step**: the fixed-point map has derivative `−B/Q`, `B = ρL(T_s−T⁰)/(T_l−T_s)`, so `B > Q` diverges. A realistic melting case failed after 1500 iterations. | **fixed** — Aitken relaxation (11). Stefan benchmark 7.3 → 5.0 iterations/step average; the divergent case now converges. |
| 9 | Material tables clamp outside their range rather than extrapolating. | correct and deliberate; now stated in `docs/theory.md` §2.1 with its cost. |
| 10 | Radiation Newton linearisation about the face mean, iterated to convergence. | correct; the limitation is that it is ambient-only, now declared explicitly. |
| 11 | Prescribed-temperature reaction heat recovered from the nodal residual. | correct. |
| 12 | **CG on a symmetric positive-definite matrix.** | valid for every term currently assembled (conduction, capacity, convection, linearised radiation, interface blocks); `docs/theory.md` §3.4 records that advection would break the assumption. |
| 13 | Unconditional stability of the θ-method claimed without qualification. | **fixed in the documentation** — the result is for the linear constant-coefficient problem and implies nothing about nonlinear convergence, positivity or accuracy. |
| 14 | No thermal contact or interface resistance. | **implemented and verified in the core** (node-pair conductance); not reachable through MCP until the mesher can duplicate nodes at body interfaces. |
| 15 | No adaptive time stepping. | **still missing** — the largest remaining numerical gap; now declared in `capabilities_get`. |

## API hazard found while extending the solver

`ThermalMaterial` grew new optional fields. Any caller that declared one uninitialised and assigned only `.k`, `.cp`
and `.rho` — as `tools/thermtest.c` did — then passed garbage for the new fields, and the solver segfaulted on the
first run. Production code was safe (`ThermalCase` is `calloc`'d), but the hazard is real for any user of the header.
Three changes were made: the test now zeroes the struct, `thermal_model.c` zeroes each entry explicitly, and the
header states the requirement. Positional initialiser lists of `ThermalOptions` and `ThermalModel` were converted to
designated initialisers in production code for the same reason.
