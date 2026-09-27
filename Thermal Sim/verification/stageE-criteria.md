# Stage E: acceptance criteria, fixed before each part's first comparison

Stage E is verified in parts, in dependency order. Each part's criteria are written here before its test is run for
the first time. Amendments, if any, are appended at the end with the observed result that motivated them.

## E1: nonsymmetric linear solver (written 2026-09-16, before `tools/advtest.c` was first run)

BiCGSTAB with right ILU(0) or Jacobi preconditioning (`src/fem/krylov.c`).

| # | Test | Criterion |
|---|---|---|
| E1a | tridiagonal nonsymmetric system: ILU(0) is the exact LU | converges in 1 iteration; solution error ≤ 1e-12 relative |
| E1b | random sparse diagonally dominant nonsymmetric system, n = 60, against dense LU with partial pivoting | both preconditioners converge to 1e-12; solution difference to dense LU ≤ 1e-9 relative |
| E1c | Galerkin advection–diffusion matrix on a 3-D grid (no stabilisation, manufactured solution) at cell Péclet numbers 0.1, 1, 10, 100 | ILU(0) converges to 1e-10 at every Péclet number; error to the manufactured solution ≤ 1e-7 relative; iteration counts reported; whether Jacobi converges is reported, not required |
| E1d | symmetric positive-definite system (3-D Laplacian + mass) | BiCGSTAB-ILU(0) solution equals PCG's within 1e-9 relative |
| E1e | singular matrix (a zero row) and a matrix with a zero diagonal | fail with a message; never report convergence |
| E1f | a transient thermal step with an advective element (after E2 exists) | the solver used is BiCGSTAB, never CG, and the step reports it |

## E2: advective energy transport in the thermal FEM (written 2026-09-16, before the E2 tests were first run)

Convective form `ρc_p (∂T/∂t + u·∇T) = ∇·(k∇T) + q` in elements marked advected, with a nodal velocity field,
Galerkin or SUPG. Open faces take outflow where `u·n > 0` and weak inflow at the face's ambient temperature where
`u·n < 0`.

| # | Test | Criterion |
|---|---|---|
| E2a | steady 1-D advection–diffusion, T(0) = 0, T(L) = 1, global Péclet 20, meshes of 10/20/40/80 elements (cell Péclet uh/α = 2, 1, 0.5, 0.25) against `(e^{Pe x/L} − 1)/(e^{Pe} − 1)` | Galerkin and SUPG: observed order of the nodal max error between 40 and 80 elements ≥ 1.8 |
| E2a′ | same at global Péclet 200 on 20 elements (cell Péclet 10) | SUPG: every nodal value within [−0.01, 1.01]. The Galerkin undershoot is reported, not required |
| E2b | uniform temperature, random non-solenoidal nodal velocity, open faces at the same temperature, 10 backward-Euler steps | max \|T − T₀\| ≤ 1e-9 K |
| E2c | transient Gaussian pulse in uniform flow with diffusion against the exact moving, spreading Gaussian; Crank–Nicolson, Δt = 0.5 s, meshes of 100/200/400 elements; **consistent capacity**, the configuration the fluid participant uses (lumping the capacity of an advected field adds a dispersive error of the order of the one being measured). Decided before the first run; the lumped result is reported | order between 200 and 400 elements ≥ 1.7; finest L∞ error ≤ 5e-4 K for a 1 K pulse |
| E2d | energy with advection: 3-D channel, uniform velocity, internal heat source, open inlet/outlet faces, convection on one wall; backward Euler to steady state | every step's balance (stored − sources − boundary − prescribed − advection − inflow) ≤ 1e-10 relative; unexplained advective energy (divergence) ≤ 1e-10 relative for the solenoidal field; at steady state, enthalpy out − enthalpy in + convective loss = source power within 1e-6 relative (sign corrected before the first run) |
| E2e | laminar parallel-plate channel, Poiseuille velocity, uniform flux on one wall, the other adiabatic, steady, meshes of 8/16/32 elements across the gap | fully developed Nusselt number (hydraulic diameter 2H) on the finest mesh within 1 % of 5.385; the Richardson-extrapolated value within 0.5 % |
| E1f | the thermal step with advection | reports `bicgstab/ilu0` as its linear method; a step without advection keeps `pcg/jacobi` |

## Amendments and findings

**E2e, first run: solver defect found and fixed; criterion unchanged.** On the steady advection-dominated channel
(global Péclet about 750 along the channel), BiCGSTAB's recursively updated residual reached the target (7e-14) while
the true residual was 1.9e-6, 6.3e-2 and 1.5e-2 on the three meshes. The solver reported failure because the true
residual decides, so no wrong answer was returned. Fix: residual replacement. When the recursive residual has
converged but the true one has not, the iteration restarts from the current iterate with the true residual. The
acceptance test was also tightened from true residual ≤ 10 × target to ≤ target. Result: Nu = 5.39748 / 5.38790 /
5.38548 on 8 / 16 / 32 elements across, observed order 1.98, extrapolated 5.38466 (−0.006 %). E1a–E1e still pass
under the stricter acceptance.

## E3: the lattice Boltzmann flow in the headless core (written 2026-09-16, before the E3 tests were first run)

The unmodified D3Q19 solver (`src/lbm.c`) runs to a steady laminar state in SI units (`src/fem/flow.c`). The
advecting velocity is the momentum density divided by the reference density, which is solenoidal in a steady weakly
compressible flow (the velocity itself is not). Cell values are mapped to FEM nodes (zero at no-slip nodes) and scaled
to the specified inlet volume flow.

| # | Test | Criterion |
|---|---|---|
| E3a | plane Poiseuille flow: no-slip plates, periodic spanwise, uniform inlet velocity, Re_H = 10, H = 16 and 32 cells, L = 12 H; profile at x = 8 H | converges to the steady tolerance; max deviation from the parabola with the measured flow rate ≤ 1.5 % of the centreline value at H = 16 and ≤ 0.5 % at H = 32; flow rate at x = 8 H within 3 % of U·H |
| E3b | parameter policy | a flow whose cell Reynolds number needs a relaxation time below the stability margin is refused with a message naming the cell Reynolds number and the largest admissible inlet velocity |
| E3c | nodal mapping of the E3a H = 16 field | velocity exactly zero at every no-slip node; FEM inlet volume flux equal to U·A within 1e-12 relative after scaling; volume flux through every cross-section within 2 % of U·A |
| E3d | the mapped LBM velocity drives the energy equation of E2e (H = 16 cells, L = 30 H, Pe = 50, measured at 20 H) | fully developed Nusselt number within 3 % of 5.385 |

**E3a, first run: defect found and fixed; criteria unchanged.** Flow rate 1.173 × U·H (H = 16) and 1.048 × (H = 32),
with lattice density ranging 0.995–1.223 and 0.999–1.065. The profile shape already met its criterion (0.29 % and
0.072 %). The parameter policy bounded only the Mach number. The viscous pressure drop of a long low-Re channel
compressed the lattice fluid by up to 22 % and saturated the inlet's density clamp, which injected mass. Fix: the
relaxation time is also chosen so that the estimated pressure-driven density change, 36 ν_lb² Re_c L/D², stays below
1 %, and a run whose measured density varies by more than 3 % is refused. Result: flow rate 1.0108 and 1.0110,
deviation 0.280 % and 0.056 % (ratio 5.0), density ranges 0.9988–1.0144 and 0.9991–1.0168.

**E3c, first two runs: mapping defect found and fixed; the criterion is now met by construction.** The field was
scaled so that the FEM flux through the *inlet plane* equalled U·A. At the inlet the lattice imposes a plug profile
whose wall layer is under-represented by bilinear faces between zero wall nodes (by about 5 % at 16 cells), so the one
factor over-scaled every developed downstream section: worst cross-section error 3.9 % (first run) and 4.35 % (after the
E3a fix). Fix: each cross-section is scaled to the inlet volume flow (mass conservation per section). The flux criterion
therefore holds by construction. The evidence reports the range of the factors applied (1.0004–1.044, largest at the
inlet), and the energy equation's unexplained advective energy fell from 3.2e-2 to 3.4e-7 of the outflow in E3d
(Nu = 5.3832, −0.03 %).

## E4: conjugate heat transfer through the orchestrator (written 2026-09-16, before the E4 tests were first run)

**Problem.** A heated solid slab (k = 200 W/(m·K), ρ = 2700 kg/m³, c_p = 900 J/(kg·K), 1e6 W/m³) under a water channel
(k = 0.6, ρ = 1000, c_p = 4180) with a Poiseuille velocity of mean 0.01 m/s. The domain is 40 mm long, the slab 2 mm, the
channel 4 mm, 2 mm deep, with 0.5 mm voxels. Fluid inlet and outlet are open faces at 300 K; every other outer face is
adiabatic; the initial temperature is 300 K. There is no convection coefficient anywhere: the interface is resolved.
The monolithic model is one mesh with shared interface nodes, advection in the fluid elements only. The partitioned
model is two participants with matching interface nodes, Dirichlet–Neumann: the fluid holds the interface at the
solid's temperature (the relaxed cut link) and returns its reaction heat per stage, which the solid receives as nodal
heat.

| # | Test | Criterion |
|---|---|---|
| E4a | fixed backward-Euler steps (0.5 s, 0–20 s) | max nodal difference, partitioned vs monolithic, over both regions at the stored times ≤ 1e-6 K; heat leaving the fluid equals heat entering the solid per solve ≤ 1e-12 relative |
| E4b | adaptive (1e-4 relative, 1e-3 K) | the partitioned run takes the monolithic number of accepted steps; max nodal difference at the stored times ≤ 1e-6 K |
| E4c | conservation of the coupled run (E4a) | temperature continuity at the interface nodes ≤ 1e-7 K at every stored time; volumetric source energy = stored energy of both participants + (enthalpy out − enthalpy in) within 1e-9 relative |

## E5: the fluid–thermal–solid study through MCP (written 2026-09-16, before `tools/chtflow.py` was first run)

**Study.** A steel plate (steel_plate_demo) 40 × 20 × 4 mm with a volumetric heat source of 0.1 W, under an air channel
(air_demo) 60 × 20 × 10 mm. The plate's top face is the conjugate interface over 40 × 20 mm. Air enters at 20 °C at
0.02 m/s along +x; the channel is periodic across y and has no-slip walls in z. The initial temperature is 20 °C, cells
are 2 mm, and every other outer face is adiabatic. The structural response is on, with the plate held at its x-min
face. Adaptive stepping, 0–7200 s. All steps go through MCP only.

| # | Test | Criterion |
|---|---|---|
| E5a | setup and refusals | the valid study validates ready, with the ONE_WAY_FLOW and PERIODIC_FLOW_SIDES warnings. Refused with an actionable error: a fluid body whose material is not a fluid; a mesh with non-cubic cells; a condition on the inlet plane; a fluid body touching no solid; a periodic flow side without its opposite; flow settings on a transient_thermal run |
| E5b | partitioned adaptive run | completes. The summary reports the flow (relaxation time ≥ 0.52, lattice Mach ≤ 0.11, density variation ≤ 3 %, section scale factors), energy closure ≤ 1e-6, conjugate interface conservation error ≤ 1e-9, largest interface temperature jump ≤ 1e-6 K, coupling converged every step, an overshoot report, and an assessment naming one-way flow, strongly coupled fluid and solids, one-way structure and demonstration materials |
| E5c | conservation of the whole study | source energy = stored energy + (enthalpy out − enthalpy in) within 1e-6 relative (all other faces adiabatic) |
| E5d | monolithic against partitioned through MCP | the same number of accepted steps; stored temperatures agree within 1e-5 K at every stored time |
| E5e | checkpoint, pause, resume | a pause at about 40 % and a resume complete the run. Stored temperatures, displacements, energy terms and interface heat are bitwise identical to an uninterrupted run with the same settings |
| E5f | reproducibility | save, reopen in a new server process, re-mesh and re-run: stored temperatures identical to the first run (≤ 1e-12 relative) |
| E5g | results through MCP | results_query temperature at the final time; results_probe history at a plate point; displacement > 0 with the plate's free end moving towards +x; the VTU export carries velocity_m_s |
| E5h | discretization evidence | the final plate peak temperature and the mean outlet air temperature are reported for temporal tolerances τ and τ/10 and for 2 mm and 1 mm cells; conservation criteria (E5b, E5c) hold on every run. No convergence order is claimed from two levels |

**E5b, first (smoke) run: criterion amended before the test was written.** The bound "largest interface temperature
jump ≤ 1e-6 K" was inconsistent with the study's default coupling tolerance. The iteration stops when the interface
temperature changes by at most 1e-8 of itself, about 3e-6 K at 300 K. The smoke run observed 2.1e-6 K. Rather than
tighten the tolerance until the bound held, the criterion now states what the iteration guarantees: the largest jump
≤ coupling relative tolerance × the largest interface temperature (in K). All other E5b criteria are unchanged.

**E5c, first run of `tools/chtflow.py`: failed. Defect identified, criterion unchanged.** Source 720 J, stored 302.426 J,
enthalpy out − in 417.745 J: closure 2.4e-4 against the required 1e-6, on every run including E5h. The method's own
discrete balance (stored = source + advection volume term + weak inflow) closed to 1.7e-11. The difference is
∫ρc_p T ∇·u dV of the mapped velocity: averaging cell values to nodes and scaling cross-sections does not make the
nodal field divergence-free in the finite-element sense, ∫ N_a ∇·u_h dV = 0 at every node, which is what equates the
volume advection term with the open-boundary enthalpy fluxes. Temperatures are absolute (≈ 293 K), so a relative mass
imbalance of 1.7e-5 in the enthalpy fluxes is 0.17 J. The other results of this first run: E5a all refusals as
required; E5b every item including the amended jump bound (1.95e-8 K); E5d 42 = 42 steps, 1.03e-6 K; E5e bitwise; E5f
reproduced; E5g as required.

*Attempted fix, rejected.* The mapped velocity was projected onto the nodal constraint by weighted least squares, with
the inlet and wall values held. Preconditioned CG on the constraint system diverged. CGLS converged, but on the E3
channel only 73 % of the largest nodal divergence was reachable. It reduced the rest partly by changing outlet
velocities: the cross-section flux error grew from 0 to 0.57 %, and the unexplained advective energy in E3d grew from
3.4e-7 to 2.2e-3 of the outflow. With no-slip and inlet values held, trilinear velocity under nodal constraints has
alternating modes that are reachable only through the outlet, so exact nodal solenoidality is not a well-posed target
for this mapping. The projection was removed.

*Measured instead.* The net mass imbalance is zero after section scaling, so the unexplained term does not depend on
the temperature reference. It is the correlation of temperature variations with the local discrete divergence of the
mapped flow. Enthalpy-flux closure: 2.37e-4 at 2 mm, 2.36e-4 with tolerances ten times tighter (not a temporal
effect), and 1.30e-4 at 1 mm, a reduction by 1.82 per halving (about first order).

*E5c amended.* (a) The study's discrete energy balance, source = stored + energy advected into the fluid (volume term
plus weak inflow), closes within 1e-6. (b) The enthalpy-flux closure is reported in every summary as
`unexplained_advective_j`, and it decreases from 2 mm to 1 mm cells. **The original E5c criterion is not met.** The
enthalpy-flux statement holds only to the discretisation error of the mapped velocity: a stated limitation, not a
passed check.

**Demonstration study, fixed-step spatial level 0.5 mm: solver defect found and fixed.** With a fixed 60 s step on the
0.5 mm mesh (monolithic, about 107 000 thermal nodes), the first backward-Euler step failed with LINEAR_FAILURE:
BiCGSTAB had exhausted its 10 residual replacements at a true relative residual of 1.31e-12 against the 1e-12 target.
That target lies below the rounding floor of this system, so no iteration can reach it. Fix: the true residual also
counts as converged when its backward error ‖b − Ax‖∞ / (‖A‖∞‖x‖∞ + ‖b‖∞) is at 64 machine epsilons, i.e. x solves a
problem perturbed only by rounding. Added check E1g (not an original criterion): a 27 000-unknown advection–diffusion
system with an unattainable tolerance of 1e-18 converges at backward error 4.2e-16 (solution error 3.4e-16), and a solve
stopped after 3 iterations (relative residual 1.1e-3) is still refused. E1a–E1f, E2–E4 are unchanged (43 checks pass).
