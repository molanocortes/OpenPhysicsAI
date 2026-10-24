# Thermal Sim — theory and model definition

This document defines what the thermal module actually solves. It is written to be checked against the code, not to
describe an aspiration: every equation here is implemented in `src/fem/thermal.c` and exercised by
`tools/thermtest.c`. Where a model is *not* implemented, this document says so rather than leaving it implied.

Units are SI throughout the numerical core. **Temperature is absolute (kelvin) everywhere inside the solver.** The
operation layer accepts and reports degrees Celsius; conversion happens at the boundary (`src/core/units.c`) and the
unit system distinguishes an absolute temperature (`degC` → K by offset) from a temperature *difference* (`K`), so
that a heat-transfer coefficient in `W/(m^2*K)` is never confused with an absolute temperature.

---

## 1. Energy balance

The solved equation is the enthalpy form of the first law for a **fixed, non-deforming solid** occupying a region that
does not exchange mass with its surroundings:

```
    ∂(ρ h)/∂t  =  −∇·q  +  Q                                    (1)
    q          =  −K ∇T                                          (2)
```

* `h` — specific enthalpy (J/kg), `ρ` — density (kg/m³). The product `ρh` is the volumetric enthalpy `H` used below.
* `q` — heat flux (W/m²), `K` — the conductivity tensor (W/(m·K)), `T` — absolute temperature (K).
* `Q` — volumetric heat generation (W/m³).

### What this form does and does not cover

Equation (1) is written for a **closed system at fixed volume with no mechanical work term**. It is correct for the
supported regime — heat conduction in a solid whose configuration the thermal solver does not change — and it is
*not* a general energy balance. In particular this form must **not** be extended by analogy to:

| Regime | Why (1) is not the right balance |
|---|---|
| Compressible flow | needs the `∂p/∂t + u·∇p` and viscous-dissipation terms, and `h` is a function of `(T, p)` |
| Variable-density transport | needs the mass balance solved simultaneously; `∂(ρh)/∂t` is not `ρ ∂h/∂t` |
| Open domains (inflow/outflow) | needs the advected-enthalpy flux `∇·(ρ u h)` and an inflow enthalpy condition |
| Deforming bodies | needs the balance on the reference configuration with the deformation gradient |

None of those regimes is implemented. The solver refuses no input on these grounds — it simply has no advection term,
no pressure and no mesh motion — so **the responsibility of not applying it outside its regime is documented here and
declared through `capabilities_get`**, which lists `advection` and `conjugate heat transfer` as unavailable.

### Reference state

Volumetric enthalpy is measured from a fixed reference temperature `T_ref = THERMAL_H_REF = 0 K`:

```
    H(T)  =  ∫_{T_ref}^{T} ρ(s) c_p(s) ds  +  ρ_m L f_liq(T)     (3)
```

Only *differences* of `H` ever enter the discrete equations and the energy budget, so the choice of `T_ref` does not
affect any result. It is fixed rather than configurable so that stored enthalpies from different runs are comparable.

---

## 2. Constitutive relations

### 2.1 Conductivity

Conductivity is given as up to three principal values `k₁(T), k₂(T), k₃(T)` and an orthonormal frame `R` whose **rows
are the principal directions expressed in the body frame**:

```
    K_global(T)  =  Rᵀ diag(k₁(T), k₂(T), k₃(T)) R               (4)
```

* Omitting `k₂` makes the material isotropic: `K = k₁ I`. This is the default.
* Omitting `k₃` sets `k₃ = k₁`.
* Omitting `R` uses the identity, i.e. principal directions aligned with the body axes.

`R` is **checked for orthonormality when the material is defined** (`material_from_json`, `src/ctl/matlib.c`) and
again in `thermal_create`. A non-orthogonal frame is rejected with the offending inner product, because `K` would then
not be symmetric positive definite and Fourier's law would admit heat flow up the temperature gradient.

Each `kᵢ(T)` must be positive everywhere. Property tables are **piecewise linear in `T` between their breakpoints and
constant outside the tabulated range** — clamped, never extrapolated. Clamping is a deliberate, documented choice: a
linear extrapolation of a conductivity fit can go negative a few hundred kelvin outside its data and produce a
non-physical solve that still converges. The cost is that a run above the last tabulated temperature silently uses the
last value, so the tabulated range should cover the expected temperatures.

### 2.2 Heat capacity and latent heat

The sensible part of (3) is the integral of the **product** of two independently tabulated piecewise-linear functions
`ρ(T)` and `c_p(T)`. On any interval where both are linear the product is a quadratic, which Simpson's rule integrates
exactly; `sensible_enthalpy` splits the range at every breakpoint of either table, so the integral is exact and not a
quadrature approximation. When both tables are single constants it reduces to `ρ c_p (T₂ − T₁)`.

The latent part uses an **equilibrium liquid fraction that is linear between the solidus `T_s` and the liquidus `T_l`**:

```
    f_liq(T)  =  0                      T ≤ T_s
                 (T − T_s)/(T_l − T_s)  T_s < T < T_l
                 1                      T ≥ T_l                  (5)
```

and the latent density `ρ_m` is evaluated **once, at the melting midpoint `(T_s + T_l)/2`**. This matters: if `ρ_m`
were evaluated at the local temperature, a temperature-dependent density would release a different latent heat on
freezing than it absorbed on melting, and a melt/re-freeze cycle would not conserve energy. The verification suite
tests exactly that cycle.

**Assumptions and limits of the phase-change model**

* Local thermodynamic equilibrium: the phase fraction is a function of the local temperature alone.
* No undercooling, no nucleation kinetics, no hysteresis. Freezing follows the same `f_liq(T)` as melting.
* `T_l − T_s` is a **model parameter that affects the answer**, not only the conditioning. A pure substance has
  `T_l = T_s`, which this model cannot represent; a small artificial interval must be chosen and its effect on the
  result established by repeating the run with a different one. The Stefan verification below reports the interval
  used alongside the error.
* No solute redistribution, no non-equilibrium (Scheil) solidification path, no solid-state transformations.
* Convection in the melt pool is not modelled: molten material conducts like a solid with the liquid conductivity.

### 2.3 Thermal expansion (thermomechanical coupling)

Thermal strain is integrated from the **instantaneous** expansion coefficient `α(T)` between the stress-free reference
temperature `T₀` and the current temperature:

```
    ε_th  =  ∫_{T₀}^{T} α(s) ds · I                               (6)
```

`α` is the instantaneous coefficient, not a mean/secant coefficient referred to some other datum — the two differ by
several percent for most alloys over a wide range, and the distinction is recorded in the material schema key name
(`expansion_1_per_k`) and in the run specification. `T₀` (`stress_free_temperature`) is an explicit input, defaulting
to the initial temperature.

---

## 3. Discretisation

### 3.1 Spatial

Eight-node trilinear hexahedra (`hex8`) on layer-aligned voxel meshes. Weak form of (1)–(2): find `T ∈ H¹` with

```
    ∫_Ω  w  Ḣ  dV  +  ∫_Ω  ∇w · K ∇T  dV
        =  ∫_Ω w Q dV  +  ∫_Γq w q_n dΓ  +  ∫_Γh w h (T_a − T) dΓ  +  ∫_Γr w εσ(T_a⁴ − T⁴) dΓ   (7)
```

for all admissible `w`, with `T` prescribed on `Γ_T`.

**Quadrature.** 2×2×2 Gauss. Under the default `property_evaluation = quadrature_point`, `K` is evaluated at each
Gauss point from the temperature interpolated there:

```
    K_ab  =  Σ_g  w_g det J_g  (∇N_a)ᵀ K(T_g) (∇N_b)              (8)
```

`property_evaluation = element_mean` reproduces the earlier behaviour — one `K` per element at the element's mean
temperature — and is kept so the difference can be measured rather than argued about. Both appear in the hashed run
specification, because they change the answer.

For a **constant isotropic** conductivity the code uses the cached Laplacian `∫∇N_a·∇N_b dV` scaled by `k`, which is
algebraically identical to (8) and costs one multiply per entry. That fast path is why quadrature-point evaluation is
nearly free on constant-property problems (measured below).

**Capacity.** The step uses the **secant apparent capacity**

```
    c_app  =  [H(T¹) − H(T⁰)] / (T¹ − T⁰)                         (9)
```

evaluated at each Gauss point, falling back to `dH/dT` at the midpoint when `|T¹ − T⁰| < 1e-9 K`. Row-sum (lumped)
capacity then gives, using `Σ_b N_b = 1`,

```
    Σ_a C_a (T¹_a − T⁰_a)  =  Σ_g w_g [H(T¹_g) − H(T⁰_g)]         (10)
```

— **the discrete stored energy of a step is exactly the Gauss quadrature of the enthalpy change.** This identity is
the reason the formulation carries latent heat without losing it or counting it twice, and it is what makes the
independent enthalpy diagnostic (§5) meaningful: the two quantities are computed by different code paths and must
agree. The same identity holds for the consistent capacity matrix by symmetry and row-sum.

`capacity_model = element_rho_cp` restores `ρ c_p` at the element mean temperature. It **cannot** carry latent heat —
there is no enthalpy jump for it to live in — so requesting it together with `phase_change` is rejected rather than
silently ignoring the latent heat.

### 3.2 Temporal

θ-method, `θ ∈ [0.5, 1]`: `θ = 1` backward Euler (first order, L-stable), `θ = 0.5` Crank–Nicolson (second order,
A-stable). Both are unconditionally stable **for the linear constant-coefficient problem**. That result does not
carry over to the nonlinear problem: it says nothing about positivity, about whether the Picard iteration converges,
or about accuracy. Crank–Nicolson in particular can ring on a steep front with a large step while remaining "stable".

Two stepping modes share one transactional integrator (`src/fem/thermal_integrator.c`):

* **fixed** — steps `t_start + k Δt` (computed by multiplication, not accumulation), shortened only to land on an event.
  No error estimate. Temporal accuracy must be established by repeating the run with a smaller step.
* **adaptive** — step doubling with error control, described in §3.2.1–3.2.4.

#### 3.2.1 Step-doubling error estimate

From the accepted state `Tⁿ` at `tⁿ` the integrator computes, independently,

* one full step of size `Δt`: `T_full`,
* two half steps of size `Δt/2`: `T_two`,

both ending at exactly `tⁿ⁺¹`. For a method of order `p` with local truncation error `C Δt^{p+1}`,

```
    T_full − T(tⁿ⁺¹) ≈ C Δt^{p+1}
    T_two  − T(tⁿ⁺¹) ≈ 2 C (Δt/2)^{p+1} = C Δt^{p+1} / 2^p
    ⇒ error of T_two ≈ (T_two − T_full) / (2^p − 1)                                   (13)
```

`p = 1` for backward Euler and for every `θ ≠ 1/2`; `p = 2` for Crank–Nicolson. **`T_two` is the accepted state**, with the
energy contributions of its two half steps. `T_full` is used only in (13) and its energy never enters the budget.
**No Richardson extrapolation** is applied: an extrapolated state satisfies none of the discrete equations, so its
energy balance would not close and its liquid fraction could leave `[0, 1]`.

The estimate is measured twice, and the step is accepted when both are ≤ 1:

```
    temperature, per active node that is not prescribed:
        e_T,i = |(13)_i| / ( tol_T + tol_rel · max(|Tⁿ_i − T_ref|, |T_two,i − T_ref|) )
    enthalpy, per Gauss point g of every active element:
        e_H,g = |H(T_two,g) − H(T_full,g)| / (2^p − 1)
                / ( tol_H + tol_rel · max(|H(Tⁿ_g) − H(T_ref)|, |H(T_two,g) − H(T_ref)|) )
    err = max( max_i e_T,i , max_g e_H,g )                                              (14)
```

* `tol_T` is a **temperature difference** (K); `tol_H` is an energy density (J/m³), by default `tol_T · ρc_p(T_ref)` of
  each element's material, i.e. the sensible heat of the temperature tolerance.
* The relative parts scale with the **change since the reference temperature** `T_ref` (the initial temperature),
  never with the absolute temperature: a 5 K rise above 293 K is held to its own size, not to 0.1 % of 298 K.
* Both are **maxima over the mesh**: a local hot spot cannot hide in an average. An RMS is reported but not used.
* Prescribed and inactive nodes carry no temporal error and are excluded.
* The enthalpy measure is what catches a phase transition: inside a mushy zone `dH/dT` is `ρL/(T_l − T_s)` larger than
  `ρc_p` (21× for the 316L demonstration material), so a temperature error that passes `e_T` can be a large
  latent-energy error. Measured on the Stefan problem, temperature-only control at the same tolerance had 3.9× the
  latent-energy error (1.86 mJ against 0.48 mJ) at 63 % of the solves.

**Inner tolerances.** The estimate is only meaningful if the nonlinear iteration error is small compared with it, so an
adaptive job tightens the Picard tolerance to at most `10⁻³ tol_T`. The linear-solver tolerance (relative residual
1e-12) is already far below both.

#### 3.2.2 Step-size controller

PI controller (Gustafsson; in the form of Hairer, Nørsett and Wanner) on the normalised error (14), with `k = p + 1`:

```
    accepted, previous accepted error available:  fac = safety · err^(−0.7/k) · err_prev^(0.4/k)
    first accepted step, or first after a rejection: fac = safety · err^(−1/k)
    rejected:                                          fac = min(1, safety · err^(−1/k))
    fac ∈ [max_shrink, max_growth], and ≤ 1 on the first attempt after a rejection or failure
```

Defaults: safety 0.9, max_growth 3, max_shrink 0.2, dt_min `10⁻¹⁰ · duration`, dt_max = duration, 25 consecutive
rejections, 10⁶ accepted steps. `err` is floored at 1e-10 so an exact step cannot request an unbounded step; a
non-finite estimate is rejected. A step shortened to land on an event keeps the controller's own proposal for the next
step instead of ratcheting down to the event-limited size.

Failures are classified before anything is retried:

| Class | Retried with a smaller step? | Result when it persists |
|---|---|---|
| error estimate > 1 | yes, by the rejection factor | `TOO_MANY_REJECTIONS`, or `MIN_STEP` once a step at dt_min is rejected |
| Picard non-convergence | yes, ×0.25, at most 8 in a row | `NONLINEAR_FAILURE` or `MIN_STEP` |
| CG breakdown or non-convergence | yes, ×0.25, at most 8 in a row | `LINEAR_FAILURE` or `MIN_STEP` |
| non-finite or sub-zero temperature | yes, ×0.25, at most 8 in a row | `INVALID_STATE` or `MIN_STEP` |
| malformed input (invalid face, material, schedule) | **no** | `INVALID_PARAMS` at once |
| out of memory | **no** | `RESOURCE_LIMIT` at once |
| accepted-step limit | — | `STEP_LIMIT` |

A smaller step cannot repair a malformed input or a singular system, so those are never retried.

#### 3.2.3 Transactions and rollback

The integrator owns the accepted state and changes it in exactly one function (`commit`):

| Part | Contents | Changed by a rejected trial? |
|---|---|---|
| accepted physical state | time, temperature, accepted energy budget, fixed-step grid index | **never** |
| control state | proposed step, previous accepted error, consecutive rejection counters | yes (that is its purpose) |
| trial buffers | `T_full`, `T_half`, `T_two` | written, never read as state |
| diagnostics | work counters, ring of the last 64 attempts | yes |

Restoring the temperature is sufficient **only because of three properties that were audited, not assumed**:

1. `ThermalSolver` keeps no history between steps. Element coefficients, linearised radiation faces, the moving-source
   load vector, active-node flags, the face grouping and the Aitken relaxation state are rebuilt inside every
   `thermal_step` from `(Tⁿ, model, t, Δt)`.
2. The phase state is the equilibrium liquid fraction `f(T)`, a function of temperature with no memory.
3. Time-dependent model data — load schedules, source position, activation — is a **pure function of the step
   interval**, supplied through a schedule callback that is called again before every trial solve. Schedules are
   piecewise constant between events that steps land on, so the midpoint of a step is unambiguous.

A constitutive model with memory (hysteresis, plasticity, damage) breaks property 2 and must add its history variables
to the accepted state and to checkpoints. Verified by test A10: 141 rejected trials on the Stefan problem each left
time, temperature and budget bit-identical, and 8 of 8 steps replayed in fresh solver and integrator objects from the
saved pre-step state reproduced the accepted step bit for bit.

#### 3.2.4 Events and stored times

Stored times (`output_times`, or `output_interval`, or every `output_every`-th fixed step), load-schedule changes and
the end time form an event schedule. Steps are shortened to land on the next event; the new time is set to the stored
event time itself, not `t + Δt`, so landings do not accumulate round-off. A step that would leave less than a quarter of
itself before an event is stretched by at most 10 % (covered by the safety factor) or split into two equal steps.
Events closer than `64 ulp · max(|t|)` merge. Internal steps and stored frames are therefore distinct: a frame is the
state at exactly its time, never the state of the nearest step.

#### 3.2.5 What a tolerance does and does not bound

The controller bounds the error **of each step**. Errors of successive steps accumulate, so the global error is not
bounded by the tolerance. Measured on the sudden-surface-heating problem (backward Euler, 20 elements, 2000 s, errors
against a Richardson-extrapolated reference whose own error is 6.2e-5 K):

| relative tol | absolute tol | accepted steps | max error over stored times | error ÷ per-step allowance |
|---|---|---|---|---|
| 1e-2 | 1 K | 49 | 1.23 K | ≈ 0.6 |
| 1e-3 | 0.1 K | 103 | 0.547 K | ≈ 2.7 |
| 1e-4 | 0.01 K | 261 | 0.210 K | ≈ 10 |
| 1e-5 | 0.001 K | 768 | 0.068 K | ≈ 34 |

(the per-step allowance is `tol_T + tol_rel · 100 K`, the rise at the hot end). The global error scales as `tol^0.42`,
close to the `tol^{p/(p+1)} = tol^0.5` expected of error-per-step control of a first-order method, and it **grows late in
the run** as the relative allowance grows with the temperature rise. To make a quantitative accuracy statement, repeat
the run with tolerances ten times tighter and compare the quantities of interest.

### 3.3 Nonlinear solution

Picard (successive substitution) in correction form: each iteration assembles `A` and `b` at the current iterate,
forms the true nonlinear residual `r = b − A T`, solves `A δ = r`, and updates. Because `A` contains the secant
capacity (9), `A T − b` reproduces `[H(T¹) − H(T⁰)]/Δt` exactly, so `r` is the residual of the enthalpy equation and
not of a linearised surrogate.

**Aitken relaxation.** For a lumped node crossing the mushy zone the undamped fixed-point map has derivative `−B/Q`,
where `B = ρ L (T_s − T⁰)/(T_l − T_s)` is the latent heat still to be absorbed and `Q` the heat delivered in the step.
The iteration **diverges whenever `B > Q`** — that is, whenever an element crosses a large part of the solidus–liquidus
interval in one step. Relaxing the update by `ω` makes the derivative `1 − ω(1 + B/Q)`, and Aitken's formula

```
    ω  ←  −ω (δ_{k−1} · (δ_k − δ_{k−1})) / ‖δ_k − δ_{k−1}‖²       (11)
```

estimates the `ω` that cancels it from the corrections alone, with no extra solves. `ω` is clamped to `[0.02, 1.5]`.
For a linear problem (11) returns `ω = 1`, so conduction without phase change is unaffected. Measured effect: the
Stefan benchmark drops from 7.3 to 5.0 iterations per step on average; a block driven through its entire mushy zone
in one step goes from *not converging in 1500 iterations* to converging.

Convergence is measured on the **undamped** correction `‖δ‖∞` in kelvin (`temperature_tolerance`). Note that inside a
mushy zone `dH/dT` is very large — for the 316L demonstration material, `ρL/(T_l − T_s) ≈ 8.6·10⁷ J/(m³·K)` — so a
temperature tolerance is effectively a much looser *energy* tolerance there. Phase-change problems need
`temperature_tolerance` tightened (1e-9 K or below) together with a raised `max_iterations`; the schema description
says so and the verification suite does it.

Newton with the consistent tangent `dH/dT` is **not** implemented. It is the obvious next step for the phase-change
case, with the caveat that `H(T)` from (3)/(5) is only C⁰ — it has kinks at `T_s` and `T_l` — so a plain Newton
iteration can chatter across them and would need a line search or a smoothed `f_liq`.

### 3.4 Linear solution

Jacobi-preconditioned conjugate gradients on the correction. **CG is valid here because the assembled matrix is
symmetric positive definite**: the conduction matrix is symmetric by (8) with symmetric `K`, the capacity matrix is a
mass matrix, convection and linearised radiation add symmetric positive-semidefinite face blocks, and an interface
pair adds `g·[[1,−1],[−1,1]]`, which is symmetric positive semidefinite. **An advection term destroys this.** A step with
any advected element is solved by BiCGSTAB with ILU(0) instead (§3.7), and the step reports which method it used.

A steady problem with no temperature sink — no prescribed temperature, no convection, no radiation — has a constant
nullspace and is rejected with an explicit message rather than solved to an arbitrary offset.

### 3.5 Checkpoints and restart

A run's **accepted state** is everything the next step depends on: the accepted time and nodal temperature, the step
controller (proposed step, previous accepted error, consecutive rejections, its resolved settings), the grid index of
fixed stepping, the accepted energy budget and work counters, the stored-frame count and times, the accepted-step
history and the resume history. Nothing else carries information forward: the liquid fraction is the equilibrium
function of temperature (§2.2), schedules are functions of time (§3.2.4), and the linear-elastic structure has no
history. A material model with memory (hysteresis, plasticity, damage) would add history variables. The checkpoint
header records `history_variables` explicitly, so such a change is visible rather than silently ignored.

**Format and publication.** A JSON header (format version, software version, component hashes, array directory) is
followed by little-endian arrays. The SHA-256 of the payload is stored in the header. A checkpoint is written to a
temporary file, flushed with `fsync`, and renamed over `checkpoint.nvc`. The previous checkpoint is first renamed to
`checkpoint.prev.nvc`, so a crash at any moment leaves at least one complete file. On reading, a truncated file, a
payload that fails its hash or an unknown format version is refused. The previous checkpoint is then tried, and the
fallback is reported.

**Stored frames** go to a separate append-only stream (`frames.nvf`), flushed before every checkpoint. The checkpoint
records how many frames belong to it. On resume the stream is truncated after that many, so frames written after the
checkpoint are recomputed, never duplicated.

**Compatibility.** The resolved mesh, materials, conditions and physics settings are each hashed when the run is
built. A resume recomputes the hashes from the current project and refuses on any difference, naming the component
(`mesh`, `materials`, `conditions`, `settings`). A change that leaves the resolved solver inputs identical, such as
renaming a selection, is by design not a mismatch.

**Why a resumed run is bitwise identical.** Checkpoints are taken at accepted step boundaries and never influence the
step sequence. The restored controller proposes exactly the step the uninterrupted run would have taken. With the same
build, machine and thread count the arithmetic is therefore identical (`verification/stageB-evidence.md`). A different
thread count or machine can change round-off, not the model.

### 3.6 Coupled participants and the shared orchestrator

Every transient run goes through one numerical lifecycle (`src/fem/orchestrator.h`). A **participant** is either
transient (integrates in time and holds an accepted state) or quasi-static (evaluated at a time from its inputs). A
**coupling** moves one field from an exporter to an importer. It is *intensive* (point values such as temperature,
copied) or *conservative* (an integrated quantity such as the heat in joules that crossed a node's share of an interface
during the step, whose sum is preserved).

**Execution plan.** The couplings form a directed graph. Its strongly connected components (Tarjan) run in topological
order (Kahn). A component of one participant runs once per attempt (one-way, sequential). A component of several
participants is a **feedback cycle** and is iterated. Links that enter a cycle backwards (to a participant that runs no
later than their source) are *cut*: predicted from the accepted states at the start of the attempt (constant
extrapolation) and relaxed between sweeps. A quasi-static participant inside a cycle is refused at creation.

**One attempt over [t₀, t₁].**
1. Every transient participant starts from its accepted state.
2. The cut fields are predicted.
3. Participants run trials in the plan's order, importing their fields first.
4. After a sweep, the change of the cut fields `r = x_new − x` is normalised by `abs + rel·max|x_new|`.
5. If the largest normalised change is ≤ 1 the cycle has converged. Otherwise the cut fields are updated
   `x ← x + ω r` with Aitken's factor `ω_k = −ω_{k−1} (r_{k−1}·(r_k − r_{k−1})) / |r_k − r_{k−1}|²`, clamped to
   [0.01, 1], and every member runs again **from its accepted state**. No interval is integrated twice from an
   advanced state.
6. When every cycle has converged and every temporal estimate is ≤ 1, all transient participants commit together, then
   quasi-static participants are evaluated.
7. Otherwise every trial is dropped and the step is retried smaller (coupling failures are treated like solver
   failures by the controller), or the run stops with a stated class.

The coupling error (largest final normalised change) and the temporal error (largest estimate over participants) are
reported separately. **Only the converged sweep's temporal estimates count.** An unconverged sweep solves a different
problem. Keeping the largest estimate over all sweeps made the partitioned verification problem take 2.4× the
monolithic step count even with stage-resolved coupling data, described next (`verification/stageD-criteria.md`,
Amendments).

**Step doubling needs stage-resolved coupling data.** A participant that estimates its error from a full step and two
half steps must receive coupled data *per solve*. If its half steps see the end-of-step value of a partner's interface
temperature, both half steps solve a different problem than a monolithic model's half steps would. The difference of
the two solutions then no longer measures the coupled temporal error. The first version had both defects, and the
partitioned run took 2.9× the monolithic step count. The thermal participant therefore resolves every interface field
into three blocks (full step, first half, second half). Each solve uses the block of the matching solve of its
partner, so a converged partitioned trial reproduces both solutions of the monolithic estimate. On the verification
problem the partitioned adaptive run took exactly the monolithic step sequence (527 steps) with the same error. A trial
that does not estimate derives the half-step blocks from its single solve: linear interpolation for temperatures,
proportional to duration for heat.

**Quasi-static participants and history.** One declared without history is evaluated at the initial time and at every
stored time, which is exact for what is stored. One declared history-dependent is evaluated after every accepted step,
so the output frequency never becomes the integration frequency of a model with memory.

**What the thermomechanical job is.** The thermal model is the only transient participant. The structure is quasi-static
without history and imports the nodal temperature. **The coupling is one-way:** nothing is exported back, so there is no
thermal effect of deformation and no gap- or pressure-dependent conductance. The summary's `coupling` section states the
plan in these terms. Strong coupling is implemented and verified in the core (partitioned conduction, §6) but is not yet
reachable through MCP.

**Partitioned conduction used for verification (Dirichlet–Neumann).** Participant B holds its interface nodes at A's
temperature (the cut, relaxed link) and exports the reaction energy of those nodes over each solve, `E = Δt·r`, where
`r` is the nodal residual of the held node. Participant A imports that energy as a constant nodal power `E/Δt` over the
same solve. At convergence, A's interface rows plus B's reactions are the monolithic interface rows, so the partitioned
solution equals the monolithic one to the coupling tolerance, and the transfer is conservative by construction. Plain
Dirichlet–Neumann iteration diverges when the Dirichlet side's interface stiffness exceeds the Neumann side's (for long
steps, roughly `k_D/L_D > k_N/L_N`). Relaxation handles moderate ratios, and the controller's step rejection is the last
resort (D3).

---

### 3.7 Advection: fluid energy transport

In elements marked advected (the fluid body of a conjugate study), the energy balance is solved in convective form

```
    ρ c_p (∂T/∂t + u · ∇T) = ∇ · (k ∇T) + q                                                                   (13)
```

for a constant-property fluid (constant ρ, c_p, k, no latent heat; other materials are refused). The velocity is a
nodal field, interpolated trilinearly. The convective form keeps a uniform temperature exactly uniform under any
velocity field, including one that is not discretely divergence-free (verified to 6e-13 K).

**Stabilisation: SUPG.** The weighting functions are `W_a = N_a + τ u·∇N_a` on the capacity, advection and source terms
(consistent SUPG). The diffusion term's second derivatives vanish for constant isotropic `k` on parallelepiped hex8
elements and are dropped otherwise. The parameter is

```
    τ = [ (2|u|/h_u)² + 9 (4α/h_u²)² ]^(−1/2),     h_u = 2|u| / Σ_a |u·∇N_a|,     α = k_u/(ρ c_p)            (14)
```

evaluated at the element centre, with `k_u` the conductivity along the flow. **τ deliberately contains no time step.**
The common transient form `(2/Δt)²` would make the spatial discretisation depend on Δt, and step doubling (§3.2.1) would
then measure the change of stabilisation between the full and the half steps as if it were temporal error. The SUPG
terms sum to zero over each element's nodes (`Σ_a ∇N_a = 0`), so they move no energy between elements and leave every
element-level balance unchanged.

**Open boundaries.** A face of kind *open* imposes nothing where the velocity leaves (`u·n > 0`: no diffusive flux; the
advected enthalpy leaves with the local temperature). Where the velocity enters (`u·n < 0`) it adds the weak inflow term
`ρc_p |u·n| (T_amb − T)`, so an inlet and a backflow region of an outlet are one condition, with the entering fluid at
the face's ambient temperature.

**Energy accounting.** With advection the balance of every step is

```
    stored = sources + boundary + prescribed + advection + inflow                                              (15)
    advection = −∫ ρc_p u·∇T_θ dV     (Galerkin part only),     inflow = ∫_open ρc_p |u·n|⁻ (T_amb − T_θ) dA
```

and closes to the solver tolerance (6e-12 per step on the verification channel). The step also reports the enthalpy
entering and leaving through open faces, `∮ ρc_p T u·n dA`, and the difference `advection + inflow − (in − out)`.
That difference equals `∫ ρc_p T ∇·u_h dV`. It vanishes for a velocity that is divergence-free for these elements
(7e-16 for a uniform field) and measures the mass-conservation error of a mapped flow (§3.9).

**Linear solution: BiCGSTAB/ILU(0).** Right preconditioning makes the monitored residual the true one. ILU(0) is exact
LU on a tridiagonal matrix (one iteration). Breakdowns restart from the current iterate, a bounded number of times.
**Residual replacement:** on strongly advective steady systems the recursively updated residual drifted eight orders
below the true one (7e-14 against 6e-2 on the Nusselt channel). The iteration therefore restarts from the true residual
whenever the recursive one has converged, and only the true residual decides convergence. It must meet the relative
tolerance, or its backward error `‖b − Ax‖∞ / (‖A‖∞‖x‖∞ + ‖b‖∞)` must be at 64 machine epsilons. The second test
exists because a 1e-12 relative target lay below the rounding floor of a 107 000-node system (true residual 1.31e-12
after ten replacements). A singular or defective matrix fails with a message, never with an unconverged vector.

### 3.8 Steady laminar flow from the lattice Boltzmann solver

The flow of a conjugate study is computed once by the application's D3Q19 lattice Boltzmann kernel (`src/lbm.c`, used
unchanged; the headless core links an IEEE build of the same source): regularized collision with third-order
recursive terms, halfway bounce-back at solid cells and no-slip sides, a velocity inlet at the box's x-min plane and a
pressure outlet at its x-max plane. It is run from rest, with the inlet velocity ramped over a few acoustic crossings,
until the momentum field changes by less than 2e-6 of the inlet velocity per 200 lattice steps.

**Units and admissibility.** With cell size `h`, viscosity `ν` and inlet velocity `U`, the cell Reynolds number
`Re_c = Uh/ν` fixes `u_lb = Re_c ν_lb`. The relaxation time starts at 0.8. It is lowered when the lattice velocity
would exceed 0.06 (Mach 0.10), or when the estimated pressure-driven density change of a confined flow,
`36 ν_lb² Re_c L/D²` for a box `L` cells long and `D` cells across its narrowest walled direction, would exceed 1 %. A
relaxation time below 0.52 is refused with the largest admissible velocity, and a run whose measured density varies by
more than 3 % is refused afterwards. The compressibility bound exists because the first verification run, bounded by
Mach number only, compressed a Re_H = 10 channel by 22 %; that saturated the inlet's density clamp and delivered 1.17×
the specified flow (stageE-criteria.md).

**What is advected.** A steady weakly compressible lattice flow conserves momentum density, not velocity, so the field
passed on is `ρu/ρ_ref`. It is averaged from cell centres to mesh nodes (zero at no-slip sides and at nodes touching
solid cells; periodic sides wrap, slip sides drop the normal component). Each cross-section is then scaled so that its
bilinear-face volume flux equals `U` times the open inlet area. The largest factors occur at the inlet, where the plug
profile meets zero wall nodes: 1.04 at 16 cells across a channel, 1.19 at 5.

### 3.9 The conjugate heat transfer study

Participants and couplings (§3.6): **flow** (quasi-static, time-independent: evaluated once) → **fluid** energy
(transient, imports the velocity) ↔ **solids** (transient) → **structure** (quasi-static at stored times, optional).
The fluid and the solid bodies are cells of one voxel mesh, so the interface nodes match one to one and every transfer
is the identity. *Partitioned* (default): the fluid holds the interface at the solids' temperature (Dirichlet on the
low-conductivity side, the cut link, Aitken-relaxed) and returns the reaction heat of each solve, which the solids
receive as nodal power. The iteration stops when the interface temperature changes by at most the relative tolerance
(default 1e-8). *Monolithic*: one model, advection in the fluid elements only. There is **no convection coefficient**
on the interface: the heat follows from the resolved temperature field. A declared contact interface on the fluid
body is refused, because it would count the wall resistance twice.

**What is one-way and what is not.** Temperature does not change the flow (constant properties, no buoyancy): one-way.
Fluid and solids exchange heat both ways every step: strong coupling. The structure responds to the solids'
temperature and nothing feeds back: one-way. The summary's `coupling` section states this plan. The steady flow acts
from t = 0: its start-up is not resolved, and the summary reports how much physical time the lattice needed to reach
it.

**Two accounts of the interface heat (partitioned).** The fluid's reactions at the nodes it holds, and the nodal heat
the solids received. A conservative identity transfer makes them equal and opposite (conservation error 1e-15 through
MCP).

**The enthalpy statement and its limit.** Equation (15) closes to round-off. The statement *source = stored + (enthalpy
out − enthalpy in)* holds only to `∫ ρc_p T ∇·u_h dV`, because the mapped velocity is not divergence-free for the
energy equation's elements: `∫ N_a ∇·u_h dV ≠ 0` at some nodes even when every cross-section carries the right flow. On
the demonstration study that term is 2.4e-4 of the source at 2 mm cells, 1.3e-4 at 1 mm and 3.5e-5 at 0.5 mm. It is reported in every
summary (`energy.unexplained_advective_j`), and it does not depend on the temperature reference, because the net mass
imbalance is zero. A least-squares projection onto the nodal constraint was tried and rejected: with no-slip and inlet
values held, part of the constraint is reachable only through the outlet velocities, which the projection distorted
(stageE-criteria.md, E5c).

---

## 4. Boundary and interface conditions

| Condition | Contribution | Notes |
|---|---|---|
| Prescribed temperature | eliminated from the system | reaction heat recovered from the residual |
| Heat flux `q_n` (W/m²) | `∫ N_a q_n dΓ` | positive into the body |
| Volumetric generation (W/m³) | `∫ N_a Q dV` | distinct from a flux and from a total power |
| Convection `h (T_a − T)` | symmetric face block + load | `h` in W/(m²·K), `T_a` absolute |
| Radiation `εσ(T_a⁴ − T⁴)` | Newton-linearised per face | see below |
| Moving Gaussian source | consistent nodal load | see §4.2 |
| Interface conductance | node-pair block `g[[1,−1],[−1,1]]` | see §4.3 |

Total power in W, flux in W/m² and volumetric generation in W/m³ are separate schema keys with separate dimensions,
checked by the unit system; they cannot be interchanged by accident.

### 4.1 Radiation — what is and is not modelled

Radiation is **grey-body exchange between a surface and a single ambient temperature**:
`q = ε σ (T_a⁴ − T⁴)`. It is linearised per face about the face mean temperature `T*` by Newton's method,
`q ≈ h_r (T_a* − T)` with `h_r = 4εσT*³` and `T_a* = T* − (T*⁴ − T_a⁴)/(4T*³)`, which is exact at convergence and
robust from a cold start. The linearisation is iterated with the rest of the nonlinearities.

**This is not enclosure radiation.** There are no view factors, no occlusion, no reciprocity or closure checks and no
radiosity solve, so two surfaces of the model do not radiate to each other — each radiates only to the stated ambient.
Calling this "radiation" without the qualifier would overstate it, so `capabilities_get` names surface-to-surface
radiation explicitly as not included.

### 4.2 Moving heat source

A Gaussian surface source with exponential absorption,
`q = 2P/(πr²d) · exp(−2ρ²/r²) · exp(−depth/d)`, which integrates to `P` over a half-space. Four distinct powers must
be kept apart and are reported separately:

1. **incident** power — not modelled; the user supplies absorbed power;
2. **absorbed** power `P` — the input;
3. the part that **geometrically intersects the modelled body** — `source_power_mesh`, the integral over the actual
   elements before any scaling;
4. **quadrature error** — the difference between (3) and the analytic integral over the same region.

By default the deposited load is scaled by `P / source_power_mesh` so the mesh absorbs exactly `P`. **This is a
modelling choice, not a correction**: it redistributes power that geometrically missed the body back onto it.
`raw_source = true` disables the scaling, and `source_scale` and `source_power_mesh` are reported every step so the
choice is visible. If the Gaussian falls entirely between Gauss points the solver deposits `P` in the element beneath
the source point and **flags `source_fallback`** — this conceals an under-resolved source and the flag exists so that
it is never silently accepted; the correct response is to refine the mesh or widen the source, not to ignore the flag.
When no active element is found at all, `source_missed` is set and no energy is deposited.

### 4.3 Thermal interfaces

Perfect thermal contact is represented by a **shared node** and needs no interface. A finite resistance is a pair of
coincident nodes on the two sides of a cut exchanging

```
    Q  =  g A (T_b − T_a)        g in W/(m²·K), A the nodal share of the interface area   (12)
```

A thin layer of thickness `d` and conductivity `k_l` is `g = k_l/d`; a contact resistance `R″` is `g = 1/R″`. Interface
pairs are added to the sparsity pattern at `thermal_create` (they may join nodes that share no element) and assembled
sequentially after the coloured element passes. A pair whose two sides are not both active exchanges nothing, which is
what makes interfaces compatible with element activation.

Gap-dependent and contact-pressure-dependent conductance are **not** implemented: `g` is a constant per pair. Reading
contact pressure from Solid Sim would require a structural contact solution, which does not exist yet.

**Through MCP (`interface_define`).** Bodies that touch in the voxel mesh share nodes, which is perfect contact. An
interface declared between two bodies changes only the **thermal** model:

* every node used by elements of both bodies gets a thermal twin, and body B's elements are renumbered onto the twins;
* the structural model keeps the shared mesh nodes, so the bodies stay bonded mechanically. Displacements are mapped
  back to every thermal node, twins included;
* each interface face gives a quarter of its area to each of its four node pairs, so the total conductance is exactly
  `g·A` whatever the node count;
* models: `perfect` (no split), `conductance` (`g` given), `thin_layer` (`g = k_l/d`, no heat capacity), `insulated`
  (no pairs: the faces are adiabatic);
* the topology is checked before meshing is accepted. Only coincident, face-matching contact of exactly two bodies is
  supported. A gap, edge-only contact, overlapping geometry (cells claimed by two bodies) and junctions of three bodies
  are refused with a diagnostic naming the problem and, for a gap, its size;
* exterior conditions applied to faces that lie on a declared interface are reported with the interface area and are
  not double counted, because internal faces are not boundary faces of the mesh.

**Two independent accounts of the interface heat.** The pairs report the heat crossing each interface. Each body's own
stored, source, boundary and prescribed energies are summed separately from its elements and faces, so a body's
imbalance *is* the heat it exchanged through its interfaces. The two agree to 1e-9 relative on the verification problem
(`verification/stageC-evidence.md`). Perfectly bonded bodies cannot close independently, because their exchange is not
a measured quantity, and the summary says so.

---

## 5. Energy and enthalpy diagnostics

Every step reports signed contributions in joules (watts for a steady solve):

* `stored_energy` — `Σ C (T¹ − T⁰)` with the capacity actually assembled;
* `source_energy` — volumetric and moving sources;
* `boundary_energy` — flux, convection and radiation faces;
* `prescribed_energy` — heat supplied through prescribed-temperature nodes, from the nodal residual;
* `interface_energy` — net heat crossing interfaces into the active region;
* `balance_error` — `|stored − source − boundary − prescribed| / max(|·|)`;
* `enthalpy_change` — **`∫ [H(T¹) − H(T⁰)] dV` evaluated independently**, by integrating (3) at the Gauss points
  with no reference to the capacity matrix that was assembled;
* `enthalpy_mismatch` — `|stored − enthalpy| / max(|·|)`;
* `liquid_volume` — molten volume at the end of the step.

The distinction between the last three and the first is the point. A small `balance_error` shows the *discrete*
equations were solved; it would remain small with a capacity model that loses latent heat, because the same wrong
capacity appears on both sides. `enthalpy_mismatch` compares the assembled capacity against the constitutive enthalpy
function and is therefore evidence about the **model**, not about the linear solve. Measured values are ~1e-13 with
the secant capacity and grow immediately if the capacity model is made inconsistent.

**Entropy generation is not computed.** Conduction and contact entropy generation would be straightforward to add, but
a *global* second-law statement also needs entropy transported through the boundaries, and reporting a partial
accounting as a second-law assessment would be misleading. Nothing is reported rather than something incomplete.

---

## 6. Verification status

All figures below are produced by `./build/thermtest` and `python3 tools/thermflow.py` on the machine recorded in
`Thermal Sim/STATUS.md`, and the raw logs are in `Thermal Sim/verification/`.

| Case | Reference | Result |
|---|---|---|
| Linear patch test, 45°-rotated anisotropic tensor | exact linear field | < 1e-7 K over all nodes |
| Slab with convection, `k₁/k₂/k₃` rotated onto x | `q = ΔT/(L/kA + 1/hA)` | agreement to < 1e-9 relative, all three |
| Steady 1-D conduction with a flux face | exact linear profile | exact; flux in = flux out |
| Temperature-dependent `k` (Kirchhoff transform) | closed form | O(h²) convergence confirmed |
| Semi-infinite solid, surface step | `erfc` | within test tolerance |
| Lumped cooling, time-step study | exponential | BE first order, CN second order confirmed |
| Radiation equilibrium | `εσ(T⁴ − T_a⁴) = 0` | exact |
| Fin with lateral convection | closed form | within test tolerance |
| Moving-source energy | deposited = absorbed | exact, constant and temperature-dependent `c_p` |
| **Melt / re-freeze cycle** | enthalpy inversion | returns to 1600.000000 K; mismatch 8.7e-14 |
| **Stefan melting front** | similarity solution `s = 2λ√(αt)` | **−0.12 %** (λ = 0.620063, St = 1, mushy 0.5 K, h = 0.5 mm, Δt = 2 s) |
| **Contact resistance, two slabs** | series resistance | interface temperatures exact to < 1e-9 K |
| Bar with convection, through MCP | `q = ΔT/(L/kA + 1/hA)` | 235.385 °C vs 235.385 °C analytic |
| Anisotropic bar, through MCP | same with `k₂` along x | 147.269 °C vs 147.273 °C analytic |
| Element activation | energy accounting | exact |
| **Adaptive stepping** (step doubling, PI control, events) | fine fixed-step references, Stefan front, switched heater | orders 1 and 2 confirmed; rejected trials leave the accepted state bitwise unchanged; global error 0.6–34× the per-step tolerance (`stageA-evidence.md`) |
| **Checkpoint and restart** through MCP | the uninterrupted run | pause/resume, SIGKILL + new process, cancel + resume, fallback after corruption: **bitwise identical** (`stageB-evidence.md`) |
| **Thermal contact** through MCP, two blocks, h = 2000 W/(m²·K) | series resistance | jump 2.499999 K (2.5), heat rate within 2.6e-7 W; per-body balances close to 1e-9 (`stageC-evidence.md`) |
| **Partitioned conduction** (Dirichlet–Neumann, orchestrator), fixed Δt = 20 s | monolithic model, same mesh | 3.5e-12 K max difference; heat transfer exactly conservative (`stageD-evidence.md`) |
| **Partitioned adaptive** (tolerance 1e-4 / 1e-3 K) | monolithic adaptive run | identical step sequence (527 steps), identical error against an 8000-step reference |
| **Rollback after coupling failures** (plain DN, 1357 rejections) | monolithic replay of the accepted steps | 6.8e-10 K max difference |
| **BiCGSTAB/ILU(0)** on advection–diffusion matrices, cell Péclet 0.1–100 | manufactured solution | ≤ 5.6e-9 relative in ≤ 17 iterations; Jacobi fails at Péclet 100 (`stageE-evidence.md`) |
| **SUPG advection–diffusion**, 1-D boundary layer (Pe 20) | exponential solution | order 4.0 (nodal superconvergence); Galerkin order 2.0; bounded at cell Péclet 10 where Galerkin undershoots by 67 % |
| **Transient advection** of a Gaussian (Crank–Nicolson) | moving, spreading Gaussian | order 2.04; 1.5e-4 K on 400 elements |
| **Laminar channel, uniform flux on one wall** | Nu = 5.385 | 5.3855 on 32 elements across (+0.009 %); order 1.98 |
| **Lattice Boltzmann Poiseuille flow** (Re_H 10) | parabola | 0.28 % (16 cells), 0.056 % (32 cells); flow rate 1.011 |
| **Lattice velocity in the channel energy equation** | Nu = 5.385 | 5.3832 (−0.03 %) |
| **Conjugate heat transfer, partitioned vs monolithic** | monolithic model | 6.3e-11 K (fixed), 8.2e-11 K and equal step counts (adaptive) |
| **Three-domain study through MCP** (plate in an air channel) | partitioned vs monolithic; uninterrupted vs resumed | 1.0e-6 K; bitwise; enthalpy-flux statement only to 2.4e-4 (§3.9) |
| **Three-domain study, mesh convergence** (2 / 1 / 0.5 mm, fixed 60 s and 30 s steps) | Richardson in time, then in space | temporal error cancels between meshes to 0.4 %; observed spatial order **0.91–1.11**, not 2 (cause not isolated), so extrapolations are indicative; the 2 mm reference run's plate peak is 0.2 K (0.7 % of the rise) below the extrapolated value |

**Validation against physical measurements: none.** Every case above is an analytical or manufactured solution. No
experimental data has been compared, no parameter has been calibrated, and all six library materials (five solids and air) are labelled
`demonstration` — uncalibrated values. Results obtained with them are indicative of the solver, not predictive of a
real part. The distinction between verification (the equations are solved correctly), validation (the equations
describe reality) and calibration (parameters fitted to data) is maintained deliberately: only the first is claimed.

### Performance

96 000 elements / 102 541 nodes, one backward-Euler step, 4 threads, M2 (8 GB, fanless — figures vary with thermal
state, so baseline and new were measured back to back):

| | assembly | solve | energy | total (median of 3) |
|---|---|---|---|---|
| Before this work | 0.007 s | 0.037–0.045 s | 0.004 s | 0.053 s |
| With quadrature properties, enthalpy capacity, independent enthalpy check | 0.007–0.011 s | 0.039–0.048 s | 0.005 s | 0.057 s |

About **8 % median cost** for the added physics and the independent diagnostic, because the constant-isotropic fast
paths keep the common case on the cached element matrices.

---

## 7. Declared unsupported regimes

Requested through the operation layer, these return a structured limitation rather than a simplified answer under a
higher-fidelity label:

* surface-to-surface (enclosure) radiation — view factors, occlusion, radiosity;
* fluid energy transport outside a conjugate study, and within one: turbulence, buoyancy, temperature-dependent fluid
  properties, a resolved flow start-up, flow directions other than +x, non-matching fluid and solid meshes;
* a global temporal error bound (adaptive stepping controls the error of each step, §3.2.5);
* resuming a checkpoint with changed inputs (§3.5: a checkpoint continues only the identical problem);
* boiling, participating-media radiation, reactive combustion, compressible gas dynamics, thermodynamic cycles;
* two-way thermomechanical coupling, plasticity and creep — so a thermomechanical run is **not** a residual-stress
  prediction: without inelasticity, thermal stresses vanish when the part returns to a uniform temperature;
* strongly coupled studies other than fluid–solid conjugate heat transfer (§3.9);
* gap- and pressure-dependent contact conductance;
* conforming (non-voxel) meshes, so curved and inclined surfaces are staircases and surface temperatures there depend
  on the element size.
