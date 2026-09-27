# Thermal Sim — status checkpoint

Resumable state of the thermal module. Written to be picked up cold: what works, what was actually run, the exact
commands, and the next concrete task. Last updated 2026-09-16.

The thermal module is **not a separate program**. It lives inside the existing NAVIER application and shares its
project format, material library, selection system, job manager, result files, renderer and MCP server. This folder
holds the module's documentation, verification evidence and demonstration studies; the code is in `src/fem/thermal.c`,
`src/ctl/thermal_*.c` and `src/ctl/ops_transient.c` of the parent repository.

## Release in progress: reliable AI-controlled multiphysics (started 2026-09-16)

| Stage | Scope | State |
|---|---|---|
| A | adaptive time integration: step doubling in temperature and enthalpy, PI controller, transactional trials, events and stored times | **done** — `tools/tsteptest.c` (54 checks against pre-registered criteria), `tools/transientflow.py` (77 checks); evidence in `verification/stageA-evidence.md` |
| B | durable checkpoint and restart: atomic SHA-256-verified checkpoints, frame stream, pause/resume/cancel policy, resume in a new process after SIGKILL, compatibility hashes | **done** — `tools/restartflow.py` (154 checks against pre-registered criteria); every resumed run bitwise identical to the uninterrupted one; evidence in `verification/stageB-evidence.md` |
| C | thermal contact through MCP: thermal-only node split with mechanical bonding kept, perfect / conductance / thin-layer / insulated, topology diagnostics, `interface_*` and `results_interface`, per-body energy balances | **done** — `tools/contactflow.py` (104 checks, one recorded criterion amendment); series resistance reproduced to 6e-5 K and 2.6e-7 W; evidence in `verification/stageC-evidence.md` |
| D | shared numerical orchestrator: participants, Tarjan/topological plan, iterated feedback cycles with Aitken relaxation, commit-all/rollback-all, stage-resolved coupling data for step doubling, quasi-static synchronisation; thermal and thermomechanical jobs routed through it | **done** — `tools/orchtest.c` (24 checks; D2 failed first and exposed two defects, D3 criterion amended, both recorded in `verification/stageD-criteria.md`); partitioned conduction = monolithic to 3.5e-12 K, the adaptive partitioned run takes the monolithic step sequence, rollback replay agrees to 6.8e-10 K; every MCP regression unchanged; evidence in `verification/stageD-evidence.md` |
| E | nonsymmetric solver (BiCGSTAB/ILU(0)); SUPG advection with open inflow/outflow/backflow faces and an advective energy budget; the unchanged lattice Boltzmann solver in the headless core as a steady flow participant; conjugate heat transfer (partitioned or monolithic) through MCP as `analysis_run` `conjugate_heat_transfer` with optional structural response; three-domain demonstration | **done, with one criterion not met** — `tools/advtest.c` (43 checks) and `tools/chtflow.py` (102 checks, MCP only). SUPG order 4.0 on the boundary layer, channel Nusselt number 5.3855 (+0.009 %), lattice Poiseuille profile 0.056 %, partitioned = monolithic conjugate transfer to 1e-10 K (core) and 1e-6 K (MCP), pause/resume bitwise, reopen reproduces. **Not met:** the enthalpy-flux form of the energy balance closes only to 2.4e-4 (2 mm) / 1.3e-4 (1 mm) of the source, because the mapped flow is not discretely divergence-free; the discrete balance closes to 1e-11 (E5c amended, recorded). Evidence in `verification/stageE-evidence.md` |

Recovery snapshot of the tree before Stage A: `.recovery/2026-09-16T1137-pre-stageA.tar.gz` (sha256 bb51bf4c…).
Copies of single files before larger rewrites were kept in `.recovery/` (e.g. `thermal_run.pre-orchestrator.c`) until
2026-09-27, when they were removed from the repository.

Suite results after Stage E (2026-09-16, `make test`, exit 0): coretest 199, surftest 30, meshtest 29, femtest 42,
thermtest 72, tsteptest 54, orchtest 24, advtest 43, rendertest 29 + PNG check, opstest 127, ctltest 47, amtest 102,
mcptest 226, amflow 71, thermflow 95, transientflow 81, restartflow 154, contactflow 104, chtflow 102; 0 failures. The
GUI (`make`) builds without warnings and keeps its own fast-math lattice object. The suite was rerun in full after the
last core change (BiCGSTAB's rounding floor, E1g) with the counts above, and the GUI's functional check (`make test-ui`,
off-screen, every control clicked through the real input path) passed 52, 0 failed. The table further down is the historical
record of the first thermal session.

### Definition of done for this release: what holds and what does not

| Requirement | State |
|---|---|
| adaptive stepping verified; rejected steps leave the state unchanged | holds (Stage A) |
| jobs survive checkpoint and restart | holds (Stage B; the conjugate study too, bitwise, E5e) |
| finite contact configurable and verifiable through MCP | holds (Stage C) |
| thermal and structural analyses use one orchestration lifecycle | holds (Stage D; conjugate studies add flow, fluid and solid participants) |
| fluid energy transport and conjugate heat transfer implemented and verified in the declared regime | holds for the discrete energy balance, the SUPG and solver verification, the channel Nusselt number, partitioned = monolithic and the MCP workflow; **the enthalpy-flux statement is approximate** (2.4e-4 / 1.3e-4 of the source, reported per run) |
| a three-domain demonstration runs through MCP reproducibly | holds: `tools/chtflow.py` (pause/resume bitwise, reopen reproduces) and `examples/make_three_domain.py`. Its convergence study rests on three meshes at fixed 60 s and 30 s steps, and the temporal error cancels between meshes to 0.4 %. Observed spatial order is **about 1** (plate peak 1.11, outlet 0.91). Extrapolated plate peak 46.145 °C; the reference run is 0.196 K (0.7 % of the rise) below it. See `verification/stageE-evidence.md` |
| regressions pass; capabilities, schemas, theory and status match the implementation | holds as of the suite above; `capabilities_get` declares `conjugate_heat_transfer` and its exclusions, and `docs/theory.md` §3.7–3.9 and `docs/capability-matrix.md` were updated in the same change |

### Next concrete steps

1. Make the mapped flow divergence-free for the energy equation's elements by construction. For example, derive nodal
   velocities from the lattice's face fluxes with a compatible (Raviart–Thomas-like) interpolation, or advect with face
   fluxes in a finite-volume energy equation for the fluid. Then close the enthalpy-flux statement to round-off (E5c as
   originally registered).
2. Make the partitioned step controller remember the step size at which coupling failed (Stage D limitation), and
   report a coupling-limited step history.
3. Add a dedicated backflow benchmark, and a flow start-up (unsteady lattice participant sub-cycled per thermal step),
   before any buoyancy or temperature-dependent fluid properties.

## Machine and toolchain

macOS 15.7.3, arm64 (Apple M2, 8 GB, fanless — timings vary with thermal state, so A/B comparisons are run back to
back), Apple clang 17.0.0, Python 3.10 with NumPy, no SciPy, no Homebrew, no CMake, no Gmsh. Not a git repository.

Numerical core built with `-O2 -ffp-contract=off` and **without** `-ffast-math`, so NaN/Inf semantics and summation
order are preserved and results do not change between compilers.

## Build and test

```bash
# from the repository root
make                 # fluid GUI + navier-server, navier-mcp, navier-ctl
make headless        # everything that builds without Cocoa/OpenGL, plus the test binaries
make test            # the full suite below
```

Individual suites:

```bash
./build/thermtest                              # thermal verification, 72 checks
./build/tsteptest                              # adaptive time integration (Stage A), 54 checks
./build/orchtest                               # shared orchestrator, partitioned conduction (Stage D), 24 checks
./build/advtest                                # solver, SUPG advection, lattice flow, conjugate transfer (Stage E), 43 checks
python3 tools/thermflow.py                     # complete thermal workflow over MCP, 95 checks
python3 tools/transientflow.py                 # adaptive settings and failures over MCP, 81 checks
python3 tools/restartflow.py                   # checkpoint, pause, resume, SIGKILL, corruption over MCP, 154 checks
python3 tools/contactflow.py                   # thermal contact over MCP, 104 checks
python3 tools/chtflow.py                       # the fluid-thermal-solid study over MCP, 102 checks (--quick skips E5h)
python3 "Thermal Sim/examples/make_examples.py"  # rebuild the three thermal demonstration studies
python3 "Thermal Sim/examples/make_three_domain.py"  # the three-domain study with its convergence studies (~1 h)
python3 "Thermal Sim/examples/make_three_domain.py" --spatial-fixed   # only the fixed 60 s mesh study (~7 min)
python3 "Thermal Sim/examples/make_three_domain.py" --temporal-check  # only the 30 s / 15 s cancellation runs (~13 min)
python3 "Thermal Sim/examples/make_three_domain.py" --report-only     # rebuild study.json, README.md and exports
```

## Test results of the first session (2026-09-16, this machine; historical, superseded by the release table above)

| Suite | Result |
|---|---|
| `build/coretest` | 199 passed, 0 failed |
| `build/surftest` | 30 passed, 0 failed |
| `build/meshtest` | 29 passed, 0 failed |
| `build/femtest` | 42 passed, 0 failed |
| **`build/thermtest`** | **72 passed, 0 failed** (was 41) |
| `build/rendertest` + `tools/pngcheck.py` | 29 passed, 0 failed; PNG check passed |
| `build/opstest` | 111 passed, 0 failed |
| `build/ctltest` | 47 passed, 0 failed |
| `build/amtest` | 102 passed, 0 failed |
| `tools/mcptest.py` | 194 passed, 0 failed |
| `tools/amflow.py` | 71 passed, 0 failed |
| **`tools/thermflow.py`** | **89 passed, 0 failed** (new) |

1015 checks, 0 failures. Clean build, 0 warnings.

## What changed in this work

Milestone 1 (audit) and milestone 2 (thermal foundation) of the brief, plus the parts of milestone 3 that the thermal
path needed. Details and evidence in `docs/audit.md`.

1. **Audit** — built and ran everything, classified each component, and found that `STATUS.md` understated the
   repository: transient thermal was implemented and passing while `capabilities_get` still told AI clients it was
   *"planned, not available"*. Fixed, with an honest per-analysis capability document in its place.
2. **Quadrature-point property evaluation** — conductivity is evaluated at each Gauss point instead of once per
   element at the element mean. `property_evaluation: element_mean` keeps the old behaviour for comparison. Measured
   cost ~8 % on a constant-property benchmark, because the constant-isotropic fast path is preserved.
3. **Anisotropic conductivity** — three principal values plus an orthonormality-checked rotation from the material
   frame to the body frame, evaluated per Gauss point. Verified by a 45°-rotated linear patch test (< 1e-7 K) and
   three rotated slab cases matching the series-resistance solution to < 1e-9 relative.
4. **Enthalpy formulation** — the step now uses the secant capacity `[H(T¹)−H(T⁰)]/(T¹−T⁰)` at Gauss points, which
   makes the discrete stored energy *identically* the quadrature of the enthalpy change. That single change fixes the
   nonlinear-capacity approximation and makes latent heat possible without double counting.
5. **Phase change** — latent heat through an equilibrium liquid fraction linear between solidus and liquidus.
   Verified against the one-phase Stefan similarity solution to **−0.12 %** and by a melt/re-freeze cycle that returns
   to its starting temperature with an enthalpy mismatch of 8.7e-14.
6. **Latent heat reached the solver at last** — `solidus_c`, `liquidus_c` and `latent_heat_j_per_kg` were parsed,
   validated and stored, then discarded before the solve. Two library materials carry 270 and 286 kJ/kg that were
   being silently dropped. Now wired through, with `PHASE_CHANGE_ACTIVE` / `PHASE_CHANGE_DISABLED` /
   `LATENT_HEAT_IGNORED` warnings so the choice is always stated.
7. **Phase change was not counted as a nonlinearity** — with constant property tables the Picard loop exited after
   one iteration, losing the entire latent heat. Found by the new melt/re-freeze test (it failed by 17 K and 50 % of
   the energy), fixed.
8. **Aitken relaxation** — the undamped Picard map for a node crossing the mushy zone has derivative `−B/Q` and
   provably diverges when the latent heat still to be absorbed exceeds the heat delivered in the step. A realistic
   melting case failed after 1500 iterations. Aitken's formula estimates the relaxation that cancels it from the
   corrections alone: the case now converges, and the Stefan benchmark drops from 7.3 to 5.0 iterations per step.
9. **Thermal contact resistance** — finite-conductance interfaces between coincident node pairs, in the sparsity
   pattern and compatible with element activation. Verified against a two-slab series resistance (interface
   temperatures and the jump exact to < 1e-9 K) and against the perfect-contact limit. **Core API only** — see the
   gap below.
10. **Independent energy verification** — the enthalpy change is now integrated from `H(T)` with no reference to the
    assembled capacity matrix, accumulated over the run, and reported as `enthalpy_change_j` and `enthalpy_mismatch`.
    A small linear-system residual is not evidence of energy conservation; this is.
11. **`tools/thermflow.py`** — the complete thermal workflow over MCP, 89 checks, now in `make test`. It tests what
    an AI host can actually reach, which is where the real gaps were.
12. **Three demonstration studies** under `examples/`, each with its saved project, hashed run specification, energy
    budget, VTU/CSV exports and a README stating its analytical reference and its limitations.

## Known gaps, in priority order (after the 2026-09-16 release)

The gaps listed by the first session were adaptive stepping, interfaces through MCP, advection and conjugate heat
transfer, an orchestrator, and checkpoint/resume. Each is now implemented and verified in its declared regime (the
release table at the top). What remains:

1. **The enthalpy-flux statement of a conjugate study is approximate.** The mapped lattice flow is not divergence-free
   for the energy equation's trilinear elements. Inflow and outflow enthalpy therefore match the stored and dissipated
   energy only to 2.4e-4 (2 mm), 1.3e-4 (1 mm) and 3.5e-5 (0.5 mm) of the source on the demonstration. The discrete
   balance closes to round-off, and the difference is reported per run.
2. **The demonstration converges at first order in space** (observed 0.91–1.11 over 2, 1 and 0.5 mm, with the temporal
   error shown to cancel), not the second order of trilinear elements on smooth solutions. Candidate causes, not isolated:
   singular gradients where the plate starts at the inlet and ends at an adiabatic floor, a lattice relaxation time
   that changes with the mesh (0.528 → 0.612), and the nodal flow mapping, whose section-flux correction shrinks only
   about as fast as the cell size. Extrapolated values are therefore indicative.
3. **Flow physics is the narrowest possible regime:** steady, laminar, constant properties, no buoyancy, flow along +x,
   no start-up transient, the lattice relaxation time and compressibility bounding which velocities a mesh can carry.
4. **The partitioned controller forgets coupling-limited step sizes** and grows back into coupling failures (Stage D).
5. **No enclosure radiation.** Ambient-only grey-body exchange; no view factors, occlusion or radiosity.
6. **Global temporal error is not bounded by the tolerance** (per-step control). The demonstration's plate peak still
   moved by 0.027 K between tolerances 1e-4 and 1e-5.
7. **No Newton with a consistent tangent**, no uncertainty propagation or sensitivity analysis, no Streamable HTTP
   transport, no GUI display of thermal results, voxel meshes only, and **no validated material data**: every library
   material, air included, holds demonstration values.

## Next concrete task

**Close the enthalpy-flux statement of conjugate studies by construction** (gap 1). Build the fluid's advecting
velocity so that it is divergence-free for the energy equation's elements. Candidate routes:

1. Take the lattice's face fluxes (mass through each cell face) as the primary data and advect with a finite-volume
   energy equation in the fluid cells. Couple it to the FEM solids through the same Dirichlet–Neumann interface; face
   fluxes are exactly conservative per cell.
2. Or keep the FEM energy equation and replace nodal averaging by an interpolation that is compatible with the
   trilinear divergence (a discrete stream-function construction), accepting a non-nodal representation.

Whichever is chosen, the verification is already written. E5c as originally registered (source = stored + enthalpy out
− in within 1e-6) must then pass without amendment, and E3d, E4 and E5d–E5f must not regress. Then rerun
`make_three_domain.py --spatial-fixed --temporal-check` (or the full study). If the observed spatial order rises toward
2, the mapping was the cause of gap 2; if not, isolate the plate edges next. Do not start buoyancy,
temperature-dependent fluid properties or enclosure radiation before this.
