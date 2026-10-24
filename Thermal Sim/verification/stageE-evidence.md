# Stage E evidence: the first fluid–thermal–solid study

Criteria were fixed before each part's first run (`stageE-criteria.md`). Four defects were found and fixed by the
criteria and the demonstration (E2e, E3a, E3c, the E5c investigation, and BiCGSTAB's rounding floor on the 0.5 mm
demonstration mesh), one criterion was amended before its test existed (E5b), and one
was amended after it failed and could not be met with this discretisation (E5c). All are recorded there with the
observed values. Raw output: `advtest.log` (E1–E4, `tools/advtest.c`, 43 checks) and `chtflow.log` (E5, MCP only,
`tools/chtflow.py`, 102 checks). One machine (Apple M2, 8 GB, macOS 15.7.3, Apple clang 17.0.0). **Nothing here is
validated against measurements; every material is a demonstration record.**

## E1: nonsymmetric linear solver (BiCGSTAB, right-preconditioned, residual replacement)

| # | Model | Reference | Size | Metric | Observed | Criterion | Limitations |
|---|---|---|---|---|---|---|---|
| E1a | tridiagonal nonsymmetric | ILU(0) = exact LU | n = 50 | iterations; error | 1; 1.9e-16 | 1; ≤ 1e-12 | — |
| E1b | random sparse, diagonally dominant | dense LU with pivoting | n = 60 | difference | ILU(0) 3.0e-14 (6 it.), Jacobi 6.3e-13 (12 it.) | ≤ 1e-9 | small system |
| E1c | 7-point advection–diffusion, cell Péclet 0.1/1/10/100 | manufactured solution | 20³ | error; iterations | ILU(0): 5.6e-9, 6.9e-11, 9.9e-11, 1.05e-10 in 17/14/11/12 it.; Jacobi fails at Péclet 100 | ≤ 1e-7 | ILU(0) is sequential |
| E1d | SPD Laplacian + mass | PCG | 20³ | difference | 1.4e-12 (9 vs 43 it.) | ≤ 1e-9 | — |
| E1e | zero row; inconsistent singular system | — | n = 8 | refusal | both preconditioners refuse the zero row; the singular system reports breakdown | never report convergence | — |
| E1f | thermal step with advection | — | — | method used | `bicgstab/ilu0`; without advection `pcg/jacobi` | as stated | — |
| E1g | unattainable tolerance 1e-18; a solve stopped at 3 iterations (added after the 0.5 mm finding) | backward error | 30³, cell Péclet 50 | acceptance | converged at backward error 4.2e-16 (solution error 3.4e-16); the early stop refused (residual 1.1e-3) | floor ≤ 64 ε; refusal | — |

## E2: advective energy transport in the thermal FEM (convective form, SUPG with a time-step-independent τ)

| # | Model | Reference | Mesh / time | Metric | Observed | Criterion | Limitations |
|---|---|---|---|---|---|---|---|
| E2a | steady 1-D advection–diffusion, Pe 20 | `(e^{Pe x} − 1)/(e^{Pe} − 1)` | 10/20/40/80 elements | nodal max error, order 40→80 | Galerkin 1.35e-1 … 1.93e-3, order 2.03; SUPG 1.19e-3 … 3.3e-7, order 4.01 (1-D nodal superconvergence) | ≥ 1.8 | 1-D |
| E2a′ | same, Pe 200, 20 elements (cell Péclet 10) | bounds [0, 1] | 20 elements | extremes | SUPG −0.0000 … 1.0000 (error 2.8e-2); Galerkin undershoots to −0.667 | SUPG within [−0.01, 1.01] | — |
| E2b | uniform T, random non-solenoidal velocity, open faces all round | uniform T | 6×5×4, 10 BE steps | max deviation | 6.3e-13 K | ≤ 1e-9 K | — |
| E2c | moving, spreading Gaussian, CN, Δt 0.5 s | exact solution | 100/200/400 elements, 300 s | L∞ error; order | consistent capacity 2.41e-3/6.02e-4/1.47e-4 K, order 2.04 (lumped 1.96, 1.59e-4 K) | ≥ 1.7; ≤ 5e-4 K | 1-D |
| E2d | channel, uniform flow, source, open ends, convective wall | balance identity | 10×4×4, 50 steps + steady | balance; divergence term; steady closure | 6.2e-12; 7.3e-16; 4.5e-12 | ≤ 1e-10; ≤ 1e-10; ≤ 1e-6 | solenoidal velocity |
| E2e | parallel plates, Poiseuille velocity, uniform flux on one wall | Nu = 5.385 | 8/16/32 across, steady | Nu | 5.39748 / 5.38790 / 5.38548, order 1.98, extrapolated 5.38466 (−0.006 %) | ≤ 1 %; extrapolated ≤ 0.5 % | first run exposed BiCGSTAB residual drift (fixed: residual replacement) |

## E3: lattice Boltzmann flow in the headless core, mapped to the FEM mesh

| # | Model | Reference | Lattice | Metric | Observed | Criterion | Limitations |
|---|---|---|---|---|---|---|---|
| E3a | plane Poiseuille, Re_H 10, L = 12 H | parabola with the measured flow rate | H = 16, 32 cells | profile deviation; flow rate | 0.280 % / 0.056 % (ratio 5.0); 1.0108 / 1.0110 of U·H; density 0.9988–1.0144 / 0.9991–1.0168 | ≤ 1.5 % / 0.5 %; within 3 % | first run: compressed 22 %, flow 1.17× (fixed: compressibility-bounded parameters) |
| E3b | parameter policy | — | Re_c 667 | refusal | refused, naming Re_c and the largest admissible velocity (0.135 m/s at 1 mm) | as stated | the bound depends on geometry |
| E3c | nodal mapping of E3a H = 16 | — | 192×16×4 | no-slip nodes; inlet flux; section fluxes | exactly 0; 8.9e-16; exact by construction (factors 1.0004–1.044) | 0; ≤ 1e-12; ≤ 2 % | the section criterion holds by construction after the fix |
| E3d | mapped flow drives E2e's energy equation | Nu = 5.385 | 480×16×4 | Nu | 5.3832 (−0.03 %); unexplained advective energy 3.4e-7 of the outflow | ≤ 3 % | — |

**Cost.** The IEEE build of the lattice kernel runs at 84–115 MLUPS on 4 threads here; the H = 32 channel took 15.9 s.

## E4: conjugate heat transfer through the orchestrator (core)

Heated solid slab (k 200) under a water channel with a Poiseuille profile (mean 0.01 m/s), 80×12×4 elements at 0.5 mm.
The interface is resolved, with no coefficient.

| # | Scenario | Reference | Metric | Observed | Criterion |
|---|---|---|---|---|---|
| E4a | fixed BE 0.5 s, 0–20 s | monolithic | max difference; heat per solve | 6.3e-11 K; exactly 0 | ≤ 1e-6 K; ≤ 1e-12 |
| E4b | adaptive 1e-4 / 1e-3 K | monolithic | steps; max difference | 66 = 66; 8.2e-11 K (9.5e-11 K before the rounding-floor change of E1g) | equal; ≤ 1e-6 K |
| E4c | E4a | continuity; source = stored + (out − in) | 2.4e-10 K; 1.2e-10 (monolithic 1.2e-10) | ≤ 1e-7 K; ≤ 1e-9 |

Cost: partitioned 3.26 s against monolithic 0.70 s (fixed), 14.7 s against 3.1 s (adaptive). The analytic velocity here
is exactly divergence-free, which is why the enthalpy-flux statement closes (compare E5c).

## E5: the three-domain study through MCP (steel plate 0.1 W in a 0.02 m/s air channel, plate expansion)

| # | Item | Observed | Criterion | Result |
|---|---|---|---|---|
| E5a | refusals | non-fluid material, non-cubic cells, condition on the inlet plane, fluid touching no solid, one-sided periodic flow, flow settings on transient_thermal: each refused with an actionable error; the valid study validates ready with ONE_WAY_FLOW and PERIODIC_FLOW_SIDES | as stated | met |
| E5b | partitioned adaptive, 0–7200 s | 42 steps, 7.1 s wall. Flow τ 0.528, Mach 0.043, Re_h 13.3, density 0.997–1.014, section factors 1.096–1.194. Closure 1.7e-11. Interface conservation 1.4e-15. Jump 1.95e-8 K. Coupling converged every step, 5.45 iterations per step. Overshoot reported (0 K and no node beyond bounds in the demonstration's reference run). Labels as required | as stated (jump bound amended before the test) | met |
| E5c | source = stored + (enthalpy out − in) | discrete balance (source = stored + advected) 7e-12; **enthalpy-flux form 2.37e-4** | originally ≤ 1e-6 | **original not met**; amended (a) discrete ≤ 1e-6, (b) reported and decreasing with refinement: met |
| E5d | monolithic vs partitioned | 42 = 42 steps; 1.03e-6 K (monolithic 2.9 s) | equal; ≤ 1e-5 K | met |
| E5e | pause at t = 3277 s, resume | temperatures, displacements, energies and interface heat bitwise identical | bitwise | met |
| E5f | save, new process, reopen, re-mesh, re-run | stored temperatures identical | ≤ 1e-12 relative | met |
| E5g | results through MCP | final field; probe history; free-end displacement +x; VTU with velocity_m_s | as stated | met |
| E5h | tolerance ÷ 10; 1 mm cells | plate peak 45.9133 → 45.9791 °C (tolerance), 45.9717 °C (1 mm); outlet face 37.9517 → 37.9973 / 37.9282 °C; enthalpy-flux closure 2.37e-4 → 2.36e-4 (tolerance) / 1.30e-4 (1 mm); conservation holds on every run | reported | met; no order claimed from two levels |
| wiring | coupling.relative_tolerance, coupling.max_iterations, flow.steady_tolerance | each changes the solver's behaviour as its schema describes (checks in chtflow.log) | as stated | met |

## Demonstration convergence (`examples/04_three_domain_cht`, MCP, monolithic)

Verification of the study's discretisation, not of its physics: no measurement is involved. At a fixed 60 s step every
mesh takes the same 120 backward-Euler steps. The same meshes were rerun at 30 s, and 2 mm and 1 mm at 15 s, to check
that the temporal error cancels between meshes. Estimates extrapolate in time (order 1.00 measured on 2 mm and 1 mm,
from 60/30/15 s) and then in space (Richardson, three levels, ratio 2).

| Quantity | 2 / 1 / 0.5 mm at 60 s | Observed order | Extrapolated (time and space) | 0.5 mm − extrapolated | Reference run − extrapolated |
|---|---|---|---|---|---|
| plate peak | 45.9686 / 46.0273 / 46.0545 °C | 1.11 | 46.145 °C | −0.024 K | −0.196 K (−0.7 % of the rise) |
| outlet face mean | 37.9900 / 37.9665 / 37.9540 °C | 0.91 | 37.986 °C | +0.014 K | −0.009 K |
| interface heat | 417.092 / 416.402 / 416.082 J | 1.11 | 415.02 J | +0.28 J | +2.30 J (+0.6 %) |
| largest displacement | 0.013287 / 0.013281 / 0.013280 mm | 2.51 | 0.013314 mm | +2e-7 mm | −3.7e-5 mm (−0.3 %) |
| enthalpy-flux closure | 2.36e-4 / 1.29e-4 / 3.47e-5 | 0.87, 1.90 (ratios) | limit 0 | — | — |

The reference run is partitioned, 2 mm, tolerance 1e-3 / 0.05 K. Temporal cancellation: T(30 s) − T(60 s) is
0.03323 / 0.03334 / 0.03339 K for the plate peak on the three meshes. Its change between meshes contaminates the mesh
differences by at most 0.4 % (1.5 % for the displacement). Storing fields hourly instead of every 300 s changed nothing
(bitwise). The spatial orders are the same at 30 s and on the time-extrapolated values. Cost (wall): 3.1 / 38.7 / 366 s
at 60 s, 5.0 / 53.5 / 633 s at 30 s, and 6.5 / 76 / 1636 s adaptive at 1e-4. The 0.5 mm level at 60 s first failed with
a solver defect, fixed and recorded as E1g.

## What Stage E does not establish

* **The enthalpy-flux statement of a mapped flow.** The discrete energy balance closes to round-off. Inflow and outflow
  enthalpy equal the stored and dissipated energy only to the mapped velocity's discrete divergence: 2.4e-4 of the
  source at 2 mm, 1.3e-4 at 1 mm, 3.5e-5 at 0.5 mm (demonstration study). A least-squares projection was tried and
  rejected (stageE-criteria.md).
* **Why the demonstration converges at first order in space.** Its observed order is about 1, not the 2 of trilinear
  elements on smooth solutions. The candidate causes (singular gradients at the plate's edges, a relaxation time that
  changes with the mesh, the nodal flow mapping) were not isolated, so its extrapolated values are indicative.
* **Physics beyond the declared regime.** No turbulence, buoyancy, temperature-dependent fluid properties, flow
  start-up, flow directions other than +x or non-matching meshes. The flow does not respond to temperature, and the
  structure does not respond back.
* **Backflow.** It uses the same weak inflow term as the inlet: mixed-sign open faces preserve a uniform state (E2b) and
  the balance closes (E2d). No dedicated backflow benchmark was run, and the lattice outlet itself admits no backflow.
* **Validation.** None: demonstration materials, no measurements.
