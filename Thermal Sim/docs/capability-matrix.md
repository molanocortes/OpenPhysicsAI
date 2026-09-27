# Thermal Sim capability matrix

Every feature carries one of four marks. The definitions are strict and are applied strictly.

| Mark | Meaning |
|---|---|
| **unavailable** | not implemented. Requesting it returns a structured limitation, never a quietly simplified answer. |
| **experimental** | implemented and it runs, but not verified against a reference, or reachable only from the C core. |
| **verified** | the equations are solved correctly, demonstrated against an analytical or manufactured solution with a quantitative criterion fixed before the comparison. |
| **validated** | verified *and* compared against physical measurements, within a stated regime. |

**Nothing in this module is `validated`.** No experimental data has been used; all six library materials (five solids and air) are labelled
`demonstration`. The `validated` column exists so that the distinction cannot be blurred later.

Scope of the "regime" column: outside it the feature may still run, but nothing is claimed about the answer.

---

## Conduction

| Feature | Mark | Regime / evidence |
|---|---|---|
| Steady conduction | **verified** | exact linear profile, flux in = flux out; `thermtest` |
| Transient conduction, θ-method | **verified** | semi-infinite `erfc`; lumped cooling; BE first order, CN second order confirmed by a time-step study |
| Temperature-dependent isotropic `k` | **verified** | Kirchhoff transform, O(h²) spatial convergence |
| Quadrature-point property evaluation | **verified** | the constant-isotropic fast path is algebraically identical to the Gauss sum; every conduction case passes under both settings |
| Anisotropic conductivity tensor | **verified** | 45°-rotated linear patch test < 1e-7 K; three rotated slab cases match `q = ΔT/(L/kA + 1/hA)` to < 1e-9 relative |
| Material → global frame transform | **verified** | same; non-orthonormal frames rejected at material definition |
| Temperature-dependent anisotropic `k` | **experimental** | implemented per Gauss point, no dedicated reference case |
| Lumped vs consistent capacity | **verified** | both satisfy the enthalpy identity (10); documented trade-off |
| Element activation / deactivation | **verified** | energy accounting exact; inactive nodes hold their temperature |

## Boundary conditions

| Feature | Mark | Regime / evidence |
|---|---|---|
| Prescribed temperature | **verified** | reaction heat from the nodal residual closes the budget |
| Heat flux (W/m²) | **verified** | steady 1-D flux case |
| Volumetric generation (W/m³) | **verified** | insulated-block energy accounting |
| Convection to a prescribed ambient | **verified** | fin with lateral convection; bar with convection through MCP, exact to the analytical steady state |
| Grey-body radiation to **one ambient temperature** | **verified** | radiation equilibrium exact; Newton linearisation iterated to convergence |
| Radiation: transient cooling against a reference | **experimental** | equilibrium tested, transient not |
| Thermal insulation | **verified** | the natural boundary condition; zero-flux by construction |
| Time-dependent schedules | **verified** | piecewise-constant factors per condition (`boundary_apply` `schedule`), landed on exactly; a switched heater's energy is exact (200 J of 200 J) and fixed steps land on schedule changes; `transientflow` (Stage A) |
| Periodic conditions | **unavailable** | — |
| Total power (W) distributed over a region | **experimental** | schema-distinct from flux and volumetric generation; no dedicated verification case |
| **Surface-to-surface (enclosure) radiation** | **unavailable** | no view factors, occlusion, reciprocity/closure checks or radiosity |

## Interfaces and contact

| Feature | Mark | Regime / evidence |
|---|---|---|
| Perfect thermal contact | **verified** | shared nodes; the limit of a large conductance reproduces it (< 1e-4 K jump at 1e9 W/(m²·K)) |
| Finite contact conductance | **verified** | two blocks through MCP against the series resistance: jump 2.499999 K (2.5), heat rate within 2.6e-7 W; core two-slab case exact to < 1e-9 K (`stageC-evidence.md`) |
| Thin-layer resistance | **verified** | `g = k_l/d`; equal to the conductance run to < 1e-9 K through MCP |
| Insulated interface | **verified** | exactly 0 W and 0 J across; the other body stays at its initial temperature within round-off |
| Interfaces through MCP (`interface_define`, `interface_list`, `interface_preview`, `interface_remove`, `results_interface`) | **verified** | thermal-only node split, bodies stay bonded mechanically; area-lumped pairs; per-body balances close independently to 1e-9; save/reopen in a new process reproduces to 1e-12 |
| Gaps, edge-only contact, overlaps, three-body junctions | **unavailable** | refused with a diagnostic (a gap is named with its size) |
| Gap-dependent conductance | **unavailable** | — |
| Pressure-dependent conductance | **unavailable** | needs structural contact pressure, which Solid Sim does not produce |

## Phase change

| Feature | Mark | Regime / evidence |
|---|---|---|
| Latent heat through the enthalpy | **verified** | melt/re-freeze returns to the starting temperature; `enthalpy_mismatch` 8.7e-14 |
| Equilibrium liquid fraction, linear in `T` | **verified** | Stefan similarity solution, **−0.12 %** (St = 1, mushy 0.5 K, h = 0.5 mm, Δt = 2 s) |
| Molten volume reporting | **verified** | exact half-volume at mid-mushy; reported through MCP |
| Latent heat from the material library | **verified** | 316L demonstration material through MCP: 716.8 mm³ molten, peak held to 1392.9 °C against 1669.4 °C without latent heat |
| Hysteresis, undercooling, nucleation | **unavailable** | freezing follows the melting curve exactly |
| Non-equilibrium (Scheil) solidification | **unavailable** | — |
| Melt-pool convection | **unavailable** | molten material conducts like a solid |

## Sources

| Feature | Mark | Regime / evidence |
|---|---|---|
| Volumetric sources | **verified** | energy accounting exact |
| Moving Gaussian source with absorption depth | **verified** *(core API only)* | deposited energy = absorbed power, with constant and temperature-dependent `c_p` |
| Power-loss diagnostics (`source_power_mesh`, `source_scale`, `source_fallback`, `source_missed`) | **verified** | incident / absorbed / intersecting / quadrature error kept distinct |
| Moving source through MCP | **unavailable** | no operation exposes a source path yet |

## Time integration and solvers

| Feature | Mark | Regime / evidence |
|---|---|---|
| Backward Euler | **verified** | first-order convergence confirmed |
| Crank–Nicolson | **verified** | second-order convergence confirmed |
| Picard iteration on all nonlinearities | **verified** | residual is the true enthalpy residual, not a linearised surrogate |
| Aitken relaxation | **verified** | the divergence condition `B > Q` is derived and reproduced; a case that failed after 1500 iterations now converges; Stefan 7.3 → 5.0 iterations/step |
| Jacobi-preconditioned CG | **verified** | matrix is SPD for every assembled term without advection |
| BiCGSTAB with ILU(0), residual replacement, backward-error floor | **verified** | nonsymmetric systems (advection): exact LU on tridiagonal, dense-LU agreement 3e-14, advection–diffusion to cell Péclet 100, unattainable tolerances met at the rounding floor while early stops are refused; singular and defective matrices refused; used by every advective step, never CG (`stageE-evidence.md`) |
| Sparse direct solver | **verified** | used by the structural path; available to thermal runs |
| **Adaptive time stepping** | **verified** | step doubling with temperature and enthalpy max norms, PI controller, transactional trials (rejected trials leave the accepted state bitwise unchanged), events landed exactly; order 1 and 2 estimates confirmed; **per-step control only**: global error measured at 0.6–34× the per-step tolerance (`stageA-evidence.md`) |
| **Newton with a consistent tangent** | **unavailable** | `H(T)` is only C⁰; would need a line search or a smoothed liquid fraction |
| Stagnation / non-finite detection | **verified** | non-convergence reports the change, the relaxation reached, and what to do |
| Pure-Neumann steady problem detection | **verified** | rejected with an explicit message instead of solved to an arbitrary offset |

## Coupling

| Feature | Mark | Regime / evidence |
|---|---|---|
| One-way thermomechanical (temperature → thermal strain, `E(T)`) | **verified** | free and constrained expansion in `femtest`; 0.2855 mm on a heated bar through MCP, consistent with `α ΔT̄ L` |
| Stress-free reference temperature, instantaneous `α` | **verified** | integrated from `T₀`; distinguished from a mean/secant coefficient |
| Two-way thermomechanical | **unavailable** | deformation does not change the thermal problem |
| Plasticity / creep, hence residual stress | **unavailable** | a thermomechanical run is **not** a residual-stress prediction |
| Mechanical dissipation as a heat source | **unavailable** | — |
| **Advection / fluid energy transport** (SUPG, open inflow/outflow/backflow faces, advective energy budget) | **verified** | 1-D boundary layer (SUPG order 4.0, bounded at cell Péclet 10), moving Gaussian (order 2.04), uniform state preserved under a non-solenoidal field (6e-13 K), per-step closure 6e-12, channel Nusselt number 5.3855 (+0.009 %); constant-property fluids only |
| Steady laminar flow from the lattice Boltzmann solver, mapped to the FEM mesh | **verified** | Poiseuille profile 0.28 % / 0.056 % (16 / 32 cells), flow rate 1.011; compressibility-bounded parameters; the mapped field drives the channel energy equation to Nu within 0.03 %. The mapped field is not exactly divergence-free for the energy equation's elements (stated below) |
| **Conjugate heat transfer** (`conjugate_heat_transfer`, partitioned or monolithic) | **verified** | partitioned = monolithic to 1e-10 K in the core and 1e-6 K through MCP; interface heat conserved to 1e-15; pause/resume bitwise; reopen reproduces; **demonstration materials only, no validation** |
| Enthalpy-flux statement of a conjugate study (source = stored + enthalpy out − in) | **experimental** | holds only to the mapped flow's discrete divergence: 2.4e-4 of the source at 2 mm cells, 1.3e-4 at 1 mm, 3.5e-5 at 0.5 mm (demonstration study); reported as `unexplained_advective_j` (the discrete balance closes to 1e-10) |
| Mesh convergence of a conjugate study | **experimental** | demonstration study, 2 / 1 / 0.5 mm cells: after the temporal error is shown to cancel between meshes (to 0.4 %), the observed spatial order is 0.91–1.11, not the 2 of trilinear elements on smooth solutions, and the cause is not isolated. Extrapolated values are therefore indicative. A single run still reports `discretization_evidence.spatial: not estimated` |
| Turbulence, buoyancy, temperature-dependent fluid properties, flow start-up, flow other than +x | **unavailable** | refused or stated in the capability declaration |
| Buoyancy (Boussinesq) | **unavailable** | — |
| Shared orchestrator (participants, one-way and iterated couplings, commit/rollback of all participants) | **verified** | partitioned Dirichlet–Neumann conduction vs monolithic: 3.5e-12 K at fixed steps; adaptive run takes the monolithic step sequence with the same error; rollback after 1357 coupling rejections matches a monolithic replay to 6.8e-10 K (`stageD-evidence.md`) |
| Thermal and thermomechanical jobs through the orchestrator | **verified** | bitwise identical to the direct integrator path; every thermal, transient, restart and contact regression unchanged; summary declares the coupling one-way with the structure at stored times |
| Strongly coupled studies through MCP | **verified** *(fluid–solid conjugate heat transfer only)* | `coupling.scheme: partitioned`; coupling statistics and convergence reported separately from temporal error |
| Conservative transfer between non-matching meshes | **unavailable** | only matching interface nodes (identity transfer) |

## Workflow, results and reproducibility

| Feature | Mark | Regime / evidence |
|---|---|---|
| Complete thermal workflow through MCP | **verified** | `tools/thermflow.py`, in `make test` |
| Energy budget per step and per run | **verified** | closure 1.2e-11 on the MCP bar case |
| Independent enthalpy check | **verified** | `enthalpy_mismatch` 0.0 (conduction), 8.7e-14 (melting) |
| Hashed run specification with the constitutive choices | **verified** | `property_evaluation`, `capacity_model`, `phase_change` all in the hash |
| Job control: status, cancel, duplicate detection, survival across client disconnects | **verified** | `amtest`, `mcptest`, `amflow` |
| Temperature query, probe with history, PNG render | **verified** | `thermflow` |
| VTU series + `.pvd`, CSV time history | **verified** | `thermflow`; VTU decoded independently in `amflow` |
| Checkpoint / resume of a transient run | **verified** | periodic, requested, pause, cancel and failure checkpoints; atomic, SHA-256 verified, previous kept; resume after pause, after SIGKILL in a new process, after cancel and after a corrupted checkpoint are **bitwise identical** to the uninterrupted run; changed inputs refused naming the component (`stageB-evidence.md`) |
| Uncertainty propagation, sensitivity, parameter studies | **unavailable** | — |
| Streamable HTTP transport | **unavailable** | stdio and local socket only |
| GUI display of thermal meshes and results | **unavailable** | images come from the software renderer |

## Materials

| Feature | Mark | Regime / evidence |
|---|---|---|
| Temperature tables, linear interpolation, clamped extrapolation | **verified** | `coretest`, `thermtest` |
| SI plausibility ranges and provenance on every record | **verified** | `opstest` |
| Anisotropic conductivity definition and orthonormality check | **verified** | rejected frames tested through MCP |
| Latent heat / solidus / liquidus definition | **verified** | now reaches the solver, with an explicit warning either way |
| **Calibrated material data** | **unavailable** | all library entries are `demonstration`; no measured data in the repository |
