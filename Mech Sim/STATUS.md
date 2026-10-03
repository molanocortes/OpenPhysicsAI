# Mechanics layer (robotics and mechanical testing): status

Tree: a separate worktree on branch `am-process`, which **is** release 0.3.0-rc2:
the layer was ported onto it on 2026-09-17 (this replaced the planned re-baseline 4), and it is built and tested there.
The earlier private working copy (baseline `d2c87bc`) holds the history up to the
port and is no longer where work happens. Shared files carry only the four hooks of `INTEGRATION_CONTRACT.md` section 3
plus the additive lines listed in the port and print commits; nothing has been merged into `main`.

The five states are kept apart. "Reachable through MCP" means the operation exists in the served tool list of the
working-copy build and the end-to-end flow exercises it. No capability is experimentally validated: no measurements exist yet.

| Capability | Implemented | Verified numerically | Integrated (working copy) | Reachable through MCP | Experimentally validated |
|---|---|---|---|---|---|
| Spatial algebra, quaternions, SO(3) exponential and dexp⁻¹, dense solvers | yes | yes (math cases) | yes | n/a | no |
| Mesh mass properties, closed-mesh checks, fill models (solid, shell_infill, measured_mass) | yes | yes | yes | yes (`mech_assembly_import parts`, `mech_body_define part`) | no |
| Assembly model, native format, canonical round trip | yes | yes | yes | yes | n/a |
| URDF subset import | yes | yes | yes | yes (`mech_assembly_import file`) | n/a |
| Rigid multibody dynamics (tree, loops, couplings, limits, springs, dampers, friction, compliant transmissions with backlash) | yes | yes (analytic cases below) | yes | yes (`mech_dynamics_run`) | no |
| Constraint diagnostics: redundancy, mobility, singular configurations, inconsistent loops, loop-closure adjustment | yes | yes | yes | yes (`mech_assembly_inspect`, `mech_study_validate`) | n/a |
| Actuators: effort, position and velocity servo, DC motor with gear ratio, efficiency, rotor inertia, voltage/current limits, losses | yes | yes (DC spin-up, efficiency, envelope) | yes | yes | no |
| Sampled PID with delay, derivative filter, feedforward, saturation, anti-windup; trajectories | yes | yes | yes | yes | no |
| Sensors: joint, effort, current, force/torque, IMU; seeded noise, bias, latency, filter, quantisation | yes | yes (statistics, determinism, latency, IMU) | yes | yes | no |
| Checkpoint, rollback, snapshots (bitwise replay) | yes | yes | yes | snapshots yes; rollback through the orchestrator no | n/a |
| FEM load transfer (joint wrenches + d'Alembert body forces, isostatic support) | yes | yes (spinning bar, cantilever, exact transfer) | yes | yes (`mech_fem_assess`, results through `results_query`) | no |
| Rigid contact: sphere, capsule, box, plane (convex hull against plane in the core only); Coulomb friction, Newton restitution, impulses, warm-started solver | yes | yes (analytic cases below) | yes | yes (`mech_body_define collision`, `mech_environment_define`, `mech_study_settings contact`, `mech_results_query`) | no |
| FEM assessment with contact loads (persistent contact only; impact snapshots refused) | yes | yes (finger of a printed gripper: support reactions 8e-14 N) | yes | yes (`mech_fem_assess` with `contact_shape` attachments) | no |
| Orthotropic printed material with print-oriented axes; strength criteria (maximum stress, Tsai–Wu, Tsai–Hill) | yes | yes (patch tests, off-axis coupon, isotropic limit, Timoshenko, closed-form strengths) | yes | yes (`mech_material_define`, `mech_fem_assess material_model`, `mech_structure_query`) | no |
| Natural modes of parts (supported or free-free), exact modal transient integration, Newmark, FRF | yes | yes (beam and bar closed forms, rigid-body subspace, SDOF closed forms, Newmark agreement) | yes | yes (`mech_modal_run`, `mech_vibration_query`) | no |
| Transient elastic response of a moving part, one-way from the rigid dynamics (mode-acceleration method) | yes | yes (patterns reproduce the d'Alembert loads; quasi-static = static assessment at the same instant) | yes | yes (`mech_transient_assess`) | no |
| Flexible bodies coupled two-way with the multibody dynamics (Craig–Bampton reduction of meshed parts, first-order floating frame), with stresses recovered from the coupled coordinates | yes | yes (rigid equivalents, full-order FE, beam theory, energy, stiff limit, stress convergence) | yes | yes (`mech_flexible_reduce`, `mech_flexible_attach`, `mech_flexible_detach`, `mech_flexible_stress`, `mech_dynamics_run`, `mech_results_query`, `mech_vibration_query`) | no |
| Layer-by-layer FFF print: deposition plan, heat with activation, residual stress and release from the bed | yes | yes (10 cases below: incremental exactness, Timoshenko bimetal, free and restrained bars through a modulus collapse, relaxation, deposition order, a whole print, the result file) | yes (`src/ctl/print_analysis.[ch]`, job kind `fff_print`, results in `results.nvt`) | yes (`mech_print_run`; read with `results_query`, `results_probe`, `results_export`) | no |
| Inherent-strain LPBF build (layers activated strain-free on the deformed part, orthotropic eigenstrain, wire cut with the released forces) | yes | yes (8 cases below: eigenstrain free and restrained, strain-free activation, programmed layer height, Timoshenko bimetal, layer-summed beam, mesh halving, cut in closed form) | yes (`src/mech/lpbf.[ch]`, `src/ctl/lpbf_build.[ch]`, job kind `lpbf_build`, results in `results.nvt` with `elem_birth`/`elem_death`/`elem_group`) | yes (`lpbf_build_run`; read with `results_query`, `results_probe`, `results_render`, `results_export`) | compared with measured cantilevers whose measurements are not in the public repository; the answer is not layer converged: see below |
| Measurement import, calibration, parameter studies | not started | no | no | no | no |
| Orchestrator participant adapter | not started (the shared orchestrator is available since re-baseline 2) | no | no | no | no |

## Stage 1: isolation and integration contract (done)

- **Working copy:** a separate copy with a SHA-256 manifest (288 files), zero drift at copy time, and a recorded
  in-flight file list.
- **Consumed interfaces:** base hashes recorded.
- **Baseline:** built and fully tested before any change (the snapshot record and the drift check were removed from the
  repository on 2026-09-27).
- **Documents:** integration contract, backend decision (native C core, MuJoCo as an optional cross-check needing download
  permission), and a read-only drift check.

## Stage 2: assembly and verified rigid dynamics (done)

`tools/mechtest.c` has 227 checks (all stages), all passing. Quantitative results:

| Case | Reference | Result |
|---|---|---|
| Torque-free asymmetric body, COM off the body origin, under gravity, 2 s | Jacobi-elliptic solution (itself checked against RK4 at 1e-5 s: 1e-12) | rate error 1.1e-11 rad/s at h = 1 ms; observed order 4.00, 4.00; COM on the parabola to 2e-10 m; \|ΔL_com\| 3e-13 |
| Compound pendulum, 143° amplitude | θ(t) = 2 asin(k sn(K − ω₀t, k)) and period 4K/ω₀ | max angle error 8.5e-11 rad at h = 1 ms; orders 3.31, 3.74, 3.91, 3.97 (pre-asymptotic at coarse steps); period 2.3e-10 relative |
| Damped spring–mass on a prismatic joint | analytic underdamped response | 1.3e-11 m over 3 s; stored + dissipated energy = initial to 1e-12 J; support wrench exact |
| Two-link arm, 50 random states | closed-form Lagrange equations | torques 3e-15 N·m, mass matrix 2e-16, forward∘inverse 6e-14, shoulder reaction vs Newton 1e-14 N |
| Chaotic double pendulum, 5 s | energy conservation | 9.2e-6 J (h = 2 ms), 2.9e-7 J (h = 1 ms) |
| Joint limit, restitution 0.6 | Newton impact law | rebound ratio 0.600000000; no penetration; energy balance with impact dissipation |
| Plastic limit held by gravity | static torque m g d sin q | limit reaction exact to 1e-9; released when the input pulls away |
| Four-bar crank-rocker, 3.3 crank revolutions | Freudenstein closed form | angles 2.7e-11 rad; residual 1e-11; 3 redundant rows found (the out-of-plane rows, named); energy conserved |
| Four-bar statics under gravity | virtual work | holding torque leaves accelerations 2e-9; ground reactions balance the weight to 1e-10 N |
| Spherical loop joint / parallelogram at the change point / impossible link lengths | — | 1 redundant row / SINGULAR_CONFIGURATION / LOOP_CLOSURE_FAILED |
| Gear coupling | τ/(J₁ + r²J₂) | exact; ratio held to 1e-10 over 1 s |
| Determinism | — | identical runs and restored checkpoints replay bitwise |
| Mesh mass properties | cube, rotated box, tetrahedron, hollow box | machine precision; icosphere converges at order 2 (ratio 4.00) |
| Shell + infill model | exact hollow box with infill | 1.28% mass and 3.95% Ixx error at 1.2 mm walls; 0.38% and 1.28% at 0.6 mm |

## Stage 3: actuators, controllers, sensors and contact (done)

| Case | Reference | Result |
|---|---|---|
| Geared DC motor spin-up with reflected rotor inertia | ω_ss (1 − e^(−t/τ)) | 1e-9 relative; current exact; electrical = copper + kinetic to 3e-8 (energies integrated with the Runge–Kutta stages) |
| Gearbox efficiency 0.8 against a viscous load | steady state | speed 1e-6 relative; gearbox loss = (1 − η) P_motor |
| Effort actuator with torque–speed envelope | τ_lim (1 − v/v_lim) = c v | exact; over-rating flagged |
| Sample-and-hold with a 3-sample delay | — | effort changes at exactly 14.000 ms |
| Encoder noise | σ 0.01, bias 0.002, 5001 samples | mean 0.00202, σ 0.01014; same seed identical, other seed different |
| Latency and quantisation | measured(t) = quantised truth(t − 5 samples) | exact |
| IMU 50 mm off a 3 rad/s spin axis | −ω²r, +g, ω | exact |
| PID with voltage saturation | — | clamping anti-windup: overshoot 0.015 rad vs 0.225 rad without |
| Noisy closed loop restored from a checkpoint | — | replays bitwise, including random samples |

Contact uses first-order Moreau–Jean time stepping: velocity-level impulses, an exact local friction-disc problem per
contact, speculative contacts, Newton restitution above a threshold speed, and a projected Gauss–Seidel solver
warm-started from the previous step. `tools/mechtest.c`:

| Case | Reference | Result |
|---|---|---|
| Box resting on a plane | total force m g | 19.620000000 N over 4 points; height drift 5e-15 m |
| Incline, μ = 0.5 (slip angle 26.57°) | 25°: sticks; 28°: ½ g (sin − μ cos) t² | 5e-16 m after 1 s; 0.137466 m vs 0.137329 m (+0.10%, first order in h) |
| Sliding block stopped by friction | distance v²/(2 μ g), dissipated energy | −0.015%; friction dissipation 2.000000 J of 2 J |
| Ball, e = 0.8, dropped 0.5 m | apex e² h₀; impulse m (1 + e) v | apex 0.32112 m (h = 1 ms), 0.32005 m (0.1 ms) vs 0.32 m; impulse 0.56374 N s vs 0.56378; the average force 563 → 5637 N scales with 1/h and is reported as an impulse |
| Two-pad grasp, μ = 0.6, 0.2 kg | holds when 2 μ F > m g, else a = g − 2 μ F/m | F = 2.5 N: 3e-21 m in 0.1 s; F = 1.2 N: −2.6126 vs −2.6100 m/s² |
| Pellet at 30 m/s against a 1 mm plate, h = 1 ms (30 mm per step) | no tunnelling | stops at the plate face (−5.500 mm) |
| Stack of four boxes, 1 s | height held | warm start: 2326 sweeps, converged every step after the first, drift 2e-10 m; cold start: 500000 sweeps, never converged, drift 2e-7 m |
| Grasp study from an assembly document (runtime) | statics | normal force 2.500000000 N per pad, friction on the pads 1.962000000 N = m g (the split between pads is statically indeterminate); evaluation with contact forces gives accelerations of 1e-13 |
| Pads closing a 1 mm gap | pad momentum | impact impulse 0.0165 N s vs 0.0158 N s (the squeeze force adds F h per impact step); impact steps excluded from force peaks, peak snapshots and the joint envelope |
| Snapshot restored in a new runtime | force balance of the held block from the stored contact rows | 3e-16 N |
| Rollback with contact | — | restored checkpoint replays state and summary bitwise (the warm-start store is part of the state) |
| Refusals | — | mesh collision geometry, missing friction, missing friction provenance, duplicate shape names |

## Stage 4: MCP workflow (vertical slice done)

The `mech_*` operations are served by the same MCP server, merged into the shared registry from a schema fragment
(contract `navier-mech` 0.5.0, 21 operations). `tools/mechflow.py` (214 checks) drives two projects as an independent client.

The first project is a printed arm:

- printed arm STL with a declared fill, an explicit base and a measured payload
- joints, then a geared DC motor, encoder and PID
- validation, then a dynamics job with a time-step check
- histories, load envelope and snapshots
- selections, material and mesh, then a FEM assessment at the peak hinge moment and a stress query

It also checks the refusals: plain numbers without units, joints without frames, a URDF without a root connection, and
an assessment without attachment selections. Results of the arm project:

- **Tracking:** the arm settles 0.21° from 60°.
- **Motor:** peak joint torque 0.343 N·m, peak current 0.43 A.
- **Energy:** the balance closes to 6e-5 of the input work.
- **Time-step check:** halving the step changes the outputs by at most 0.43%. The largest change is the peak motor effort:
  the declared gearbox efficiency switches with the power flow, and peaks are sampled at step ends.
- **FEM assessment:** support reactions 3e-11 N (1e-11 of the load scale); FEM and rigid-body masses equal (23.81 g); peak von Mises 0.90 MPa.

The second project is a printed gripper holding a block:

- **Setup:** two finger STLs placed as assembled, collision boxes added to the part bodies (the part link and computed
  mass are kept), a measured block with its own collision box, a floor, slides with open-loop effort actuators, and a
  study validated first without contact (warning: bodies pass through each other) and then with contact.
- **Refusals:** friction without its source, shape sizes without units, an assessment snapshot at an impact step, a
  contact load without its selection, and a contact shape of another body.
- **Hold:** normal forces 2.500000000 N per pad and friction 1.961330000 N (the block's weight); two impact steps
  reported as impulses (0.0125 N s); penetration 2e-15 m; no creep in the hold.
- **Slip margin:** grasp friction utilisation 0.654 (margin 0.35); per pad 0.676 and 0.632, whose split is statically
  indeterminate and is reported as such. A weak grasp (1.2 N, runtime test) reaches utilisation 1, slides for 0.19 s
  and the block comes to rest on the floor with the floor carrying its weight.
- **Time-step check:** the held contact forces do not change when the step is halved. The largest change is the sliding
  time during the landing, which is under ten steps.
- **Finger assessment:** the slide wrench goes on the back face, the contact load (2.5 N outward, 1.01 N down) on the
  pad face, plus the body force. Support reactions 8e-14 N. The 1 mm voxel snapping of the finger is measured,
  corrected by aligning mass centres, reported and warned about.

## Stage 5 (started): FEM load transfer

`src/mech/loads.c`:

| Case | Reference | Result |
|---|---|---|
| Wrench distribution, arbitrary force, moment and reference point | exact resultant | 1e-14 relative |
| Spinning bar (only transferred hub wrench + centrifugal body force) | σ = ρω²(L² − x²)/2 | −0.03% (20 elements along), −0.01% (40); support reactions 1e-13 N |
| Cantilever (only transferred tip and root wrenches) | dσ/dz = M/I | 0.00%; support reactions 3e-10 N |
| Line of faces loaded in twist | — | refused, with the degenerate direction named |
| Contact loads (MCP gripper flow) | self-equilibrium with joint loads and body force | support reactions 8e-14 N; the rigid body is aligned with the voxel mesh by the mass centre (offset reported) |

## Stage 5: orthotropic printed parts (done)

`src/mech/ortho.c` builds the stiffness of a homogenised orthotropic solid whose axis 3 is the build direction. The shared
hex8 element is assembled with a per-element D = TᵀCT; the solve uses the shared sparse Cholesky or PCG, and strength is
evaluated in material axes. The shared static solver is isotropic and is not changed.

| Case | Reference | Result |
|---|---|---|
| Compliance and stiffness | C S = I; engineering constants | 1e-16; E, ν, G recovered to 1e-14 |
| Admissibility | positive-definite compliance | ν12 = 1.05 with E1 = E2 refused, with the condition named |
| Isotropic constants | shared `isotropic_D` | 1e-16 |
| Rotated material | strain-energy invariance; material stress = C T ε | 1e-16 |
| Transversely isotropic material rotated about the build direction | unchanged D | 1e-16 |
| Patch test, distorted 2×2×2 mesh, rotated orthotropic material, full and incompatible-mode elements | exact homogeneous field | interior node exact; stress = Dε at every Gauss point to 1e-15 |
| Off-axis tensile coupon, raster 0/30/45/90° | 1/E_x = c⁴/E1 + (1/G12 − 2ν12/E1)s²c² + s⁴/E2 | 3.2000 / 2.6917 / 2.4105 / 2.2000 GPa, exact to 1e-9; support reactions vanish |
| Isotropic limit on a cantilever | shared isotropic solver | displacements and stresses equal to 1e-15 relative |
| 200 mm printed cantilever, flat (layers ⊥ load) and upright (built along the length) | Timoshenko with the modulus along the length | −0.50% and −0.37% at 80×4×4 elements |
| Same cantilever, Tsai–Wu | orientation effect | flat: strength ratio 44.3, tension along the raster; upright: 22.5, interlayer tension |
| Maximum stress; Tsai–Wu at every uniaxial and shear strength; mixed state | closed forms | index 1 to 1e-12; the strength ratio of a mixed state lands on the envelope to 1e-12 |
| Tsai–Hill off-axis strength 0–90° | [c⁴/X² + (1/S² − 1/X²)c²s² + s⁴/Y²]^(−½) | 1e-12 |

Over MCP (`tools/mechflow.py`), the finger of the printed gripper is assessed as an orthotropic part:

- **Material record:** refused without a source, with unitless moduli, or with inadmissible Poisson ratios. The record
  used for the test is marked `assumed`, with the reference "illustrative values for this test, not measured", and the
  response warns about the assumed data and the default Tsai–Wu interaction.
- **Orientation:** built along z and along y. Support reactions are 3e-14 of the load scale and equilibrium closes to
  7e-14. The governing mode is interlayer shear, with smallest strength ratios 8334 and 8946; the loads are small.
- **Queries:** `results_query` refuses the job (the shared result operations are isotropic). `mech_structure_query`
  returns the summary, failure indices (largest first, with modes and strength ratios) and material-axes stresses.

## Stage 6: modal and transient structural response (done)

`src/mech/modal.c` covers consistent mass, shift-invert subspace iteration and modal response. Four details matter:

- **Stable reduced stiffness.** The reduced stiffness comes from the identity (K − σM)Y = MX, not from multiplying by K,
  whose norm would put round-off into the low Ritz values.
- **Free-free parts.** The shift is moderate, and M-orthonormalisation separates the rigid-body directions it amplifies.
- **Rigid-body modes.** They are checked against the analytic rigid-body subspace, and their convergence is measured
  at the shift scale.
- **Response.** Nigam–Jennings exact integration for piecewise-linear loads, Newmark average acceleration, and FRFs
  with static residual flexibility.

| Case | Reference | Result |
|---|---|---|
| Consistent mass of a distorted element | ρV in every direction | 1e-12 |
| Cantilever, 40 elements, L/h = 40, ν = 0 | Euler–Bernoulli 13.0342 / 81.6840 Hz; effective masses 0.6131 / 0.1883; axial c/4L = 806.87 Hz | 13.0305 / 81.6028 Hz (−0.03% / −0.10%); 0.6131 / 0.1885 summed over each degenerate y–z pair; 806.92 Hz (+0.006%); 9 iterations, residuals 2e-8, M-orthogonality 9e-16 |
| Free-free beam | six rigid-body modes; β₁L = 4.7300 → 82.94 Hz | exactly six, carrying the whole mass and inertia to 1e-9; 82.86 Hz (−0.10%) |
| Printed cantilever flat / upright | f ∝ √E along the length | ratio 0.86601 vs √(E3/E1) = 0.86603 |
| Exact modal integration | undamped step peak 2F/k; damped step and ramp closed forms | 1e-10 |
| Step on the cantilever tip over one period | Newmark (T₁/400) on the full system | modal superposition (16 modes) within 0.19%; dynamic amplification 1.96 |
| Tip FRF | static limit K⁻¹F; first resonance | residual flexibility gives the static limit to 1e-8; peak = φφᵀF/(2ζω²) of the degenerate pair |
| Linear body-force patterns | d'Alembert load of an arbitrary rigid motion | 12 patterns reproduce it to 1e-13 |

Over MCP (`tools/mechflow.py`, printed arm of 120×16×10 mm, PLA demonstration data):

- **Modes.** Clamped at the hinge selection: first bending 175.23 Hz (Euler–Bernoulli 174.49 Hz; the beam is short, L/h =
  12), 7 iterations. The mode shape query puts the largest amplitude at the free end. Free-free: six rigid-body modes,
  first elastic 1085 Hz.
- **Transient.** Over ±50 ms around the peak hinge moment of the lift, 51 samples, 12 free-free modes, 2% modal damping
  (assumed, stated):
  - the load patterns reproduce the direct loads to 1e-12, and the rigid-body load share is 1e-13
  - peak von Mises is 0.8369 MPa dynamic and quasi-static; the amplification is 1.0000, because the lift is slow compared
    with the 1085 Hz mode
  - at the snapshot instant (0.2485 s) the quasi-static solution equals the static assessment (0.836878 MPa at Gauss
    points)
- **Refusals.** A transient assessment without a damping source; a history query on a modal job.

## Stage 7: flexible bodies (done)

A flexible body is a first-order floating frame of reference. Its reference frame is the body frame, held by the body's
own joint at a root region of the part. Small linear elastic deformation in mass-normalised coordinates η is superposed
on the large motion, with kinetic energy ½VᵀI(η)V + Vᵀℓη̇ + ½η̇ᵀη̇ and strain energy ½Σω²η². I(η) changes to first
order through ∫ρφ and dJ. Child joints ride interfaces, which translate by Φη and rotate by Ψη; in the recursive
Newton–Euler and composite-rigid-body algorithms they are extra motion-subspace columns of the parent. Elastic
coordinates enter energy, momentum, damping dissipation, checkpoints and rollback like joint coordinates.

`src/mech/flexbody.c` builds the elastic model from a meshed part by Craig–Bampton reduction. The root region is clamped.
Each child interface is a rigid node set at its joint origin with 6 static constraint modes, and fixed-interface normal
modes are added. The reduced problem is diagonalised to mass-normalised coordinates, and the invariants come from the
consistent mass matrix about the body origin in body axes. An optional frequency cutoff drops coordinates and reports
the static interface flexibility lost, per direction.

| Case | Reference | Result |
|---|---|---|
| One elastic coordinate as a mass on a spring inside a spinning hub | rigid equivalent (slider body on a prismatic spring joint) | 3.27e-5 m of 9.1e-4 m, exactly the predicted second-order phase drift (the first-order theory omits the centrifugal softening −mω²u); energy drift 1e-14 J |
| Child on a translating / rotating (out-of-plane) interface of a spinning hub | rigid equivalent | 3.9e-4 m over a 0.387 m range (first order) / 5e-16 m (exact for an axisymmetric segment); energy drift 3e-14 / 1e-14 J |
| Reduction without interfaces, 40-element cantilever | full-order clamped modal solve | frequencies 1.6e-10 relative; f₁ 13.0305 Hz (Euler–Bernoulli 13.0342) |
| Invariants ∫ρφ, ∫ρx×φ, dJ; unit modal mass | independent Gauss integration; central difference of the deformed inertia tensor | 6e-16 relative / 3e-16 / 6e-15; unit modal mass to 1e-15 |
| Static compliance of the tip interface | Timoshenko: 1.28048e-2 m/N, 0.24 rad/N·m, coupling 0.048 m/N·m, axial 2e-6 m/N | 1.28020e-2 (−0.02%); moment, coupling and axial exact to the printed digits; the 6 fixed-interface modes change it by 1e-13 (stiffness-orthogonal to the constraint modes) |
| Reduced frequencies with a free rigid tip face | full-order clamped–free modes | 13.0307 vs 13.0305 Hz |
| Frequency cutoff at 1 kHz | — | 2 of 12 coordinates dropped; 9.6% of the most affected interface direction's static flexibility lost, reported |
| Gravity sag of the clamped reduced beam in the multibody (damped run to rest) | full-order static FE under gravity | 5e-8 relative (tip −2.261166 mm; Euler–Bernoulli −2.260224 mm) |
| Static initial state | the settled damped run | same deflection to 4e-9; no motion over 0.1 s without damping |
| Cantilever with a tip mass equal to its own mass, released under gravity in the multibody | Euler–Bernoulli frequency equation with a tip mass (βL = 1.247917) | 5.7722 vs 5.7731 Hz (−0.016%) |
| Flexible pendulum with a tip mass, 73° swing in 0.3 s | energy; the rigid pendulum as the stiff limit | energy drift 3.8e-7 of m g L; angle deviation from the rigid pendulum 1.40e-3 rad at E and 8.78e-5 rad at 16E, a ratio of 15.97 for a compliance ratio of 16 |
| Assembly with a flexible body | canonical round trip; the directly built model | identical canonical text after reload; the tip joint rides interface 0 and the root joint the reference frame; identical motion |
| Refusals | — | body mass properties different from the reduced mesh; renamed interface joint; interface joint moved away from its point; contact geometry on a flexible body |
| Runtime | — | steps limited to 0.5/ω_max; peak interface deflection tracked every step (at least the recorded maximum); rollback replays the flexible state and its peaks bit for bit |
| Stresses from the elastic coordinates, σ = Σ ηₖσₖ, at static equilibrium under gravity | full-order static FE stresses (clamped root) | largest von Mises difference 0.42% with 6 fixed-interface modes, 0.05% with 24 (the distributed load converges with the modes kept); bending stress next to the root within 1.5% of Mz/I |

Over MCP (`tools/mechflow.py`), the printed arm of stages 4 and 6 becomes a flexible link:

- **Reduction.** Root at the hinge selection, one interface at the payload joint on the tip selection, 6 constraint +
  4 fixed-interface modes. Uncut frequencies: 175.2 Hz (the clamped modal run gives 175.23), 277.6, 1066, 1623, 1667,
  2869, 3582, 5755, 8140 and 11190 Hz.
  - **Tip compliance:** 1.4265e-4 m/N (Timoshenko 1.4482e-4, −1.5%); rotation 2.970e-2 rad/N·m (L/EI 3.0e-2, −1.0%).
  - **Mass:** mesh and body both 23.808 g.
- **Cutoff at 2 kHz.** 5 coordinates kept, so the stability step rises from 7.1e-6 s to 4.8e-5 s. Static flexibility
  lost per direction: bending 0.45% (z) and 0.25% (y); axial 100% (irrelevant for the lift, and reported as such);
  rotations 18–20%.
- **Attach.** The arm is flexible with the mesh's mass properties, and the study validates at fidelity level 4.
- **Lift** (geared DC motor, encoder, PID, 2 s):
  - 42 000 steps at the stability limit (max_step 0.5 ms)
  - time-step check 0.69%, halving the step actually taken, flexible peaks included
  - final tracking error −0.21°
  - peak tip deflection 0.28 mm at 0.237 s
  - held at 60.22°: tip deflection normal to the arm −0.10984 mm, against −0.11001 mm from the payload weight on the
    reduced compliance plus the arm's own weight (0.15%)
- **Rigid against flexible.** Holding effort within 0.5%; peak hinge moment −4.0%. The peaks ride on the controller's
  encoder-quantisation ripple (about 0.27 N·m peak to peak in both runs), and the arm's first coupled mode (about 30 Hz)
  changes how the ripple reaches the hinge; the flexible ripple decays and does not grow. When the arm started undeformed,
  it fell into its sag at t = 0 and the peak rose 27%. That transient came from the initial condition, which is why the
  default initial state is static equilibrium.
- **Coupled stresses** (`mech_flexible_stress`, all 1001 recorded instants of the lift). Peak von Mises 0.8174 MPa at
  0.236 s, at a Gauss point 1.6 mm from the clamped hinge on the outer layer.
  - **At t = 0** (static equilibrium, horizontal): 0.6457 MPa. Beam bending at that Gauss point's depth gives 0.645 MPa;
    the outer fibre carries 0.7145 MPa.
  - **Held at 60.22°:** 0.3205 MPa. The ratio to t = 0 is 0.4964, against cos 60.22° = 0.4966.
  - **One-way comparison:** the transient assessment of the rigid run gave 0.8369 MPa at a hinge moment of 0.2512 N·m.
    Scaled to the flexible run's 0.2413 N·m that is 0.804 MPa, 1.7% below the coupled value. The two runs differ, and so
    does the hinge idealisation: a clamped root against a traction over the hinge selection.
- **Refusals.** A reduction without damping and its source; an interface on a joint the body does not carry; attaching
  a stale reduction after the interface joint moved; stresses of a rigid body.

## Full regression (the worktree on 0.3.0-rc2)

`make test` passes every suite, run on 2026-09-17 after the port and the print analysis: core 199, surface 30, mesh 29, FEM 42,
thermal 72, time integration 54, orchestrator 24, advection 43, render 29, evaluation 62, operations 181, control socket 47,
AM path 102, MCP protocol 334, AM flow 71, thermal flow 100, transient flow 81, restart flow 154, contact flow 104, conjugate
flow 102, study flow 87, mechanics 237, mechanics flow 214, print flow 75, and `ALL DIAGNOSTICS PASSED` (`navier-ctl doctor`:
66 operations, contract 0.6.0, reference cantilever within -0.79 % of beam theory). The operations and MCP protocol suites
exercise every registered operation, so their counts include the 22 mechanics operations.

## Demonstration (2026-09-17)

`demo/` builds a printed robot upper-arm link as a closed STL (`make_link_geometry.c`: shoulder yoke with two bearing bores, I-section
web with lightening holes, elbow boss, NEMA-17 flange with four bolt holes) and drives a two-motor arm over MCP
(`build_robot_demo.py`, about 2.5 minutes): rigid dynamics with a time-step check, FEM assessment through the bores and bolt holes at the
peak shoulder moment and at a steady hold, orthotropic print directions with Tsai–Wu, clamped modes, a flexible link with two
interfaces, a coupled run and coupled stresses. `robot_study.json` compares a solid and a lightened web with the comparison-study feature
(86 s). `robot_arm.nav` opens the project in the app. All values are demonstration data.

The demonstration exposed one modelling gap (see Remaining work, item 1): at the peak shoulder moment the support-reaction check is 9.7%
of the load scale, because the assessment applies the elbow motor's rotor reaction to the flange while the dynamics does not feed it back
into the motion; with a negligible rotor inertia the same check is 8e-4.

## Layer-by-layer printing (2026-09-17/18)

The fused-filament print is an engine analysis: `mech_print_run` (contract `navier-mech` 0.6.0) starts a job of kind
`fff_print` through the shared job manager, and the stored times go into the ordinary time-indexed result file, so
`results_query`, `results_probe` and `results_export` read a print with `time_index`/`time_s` and the application's
stored-time slider moves through the build. The promise, the refusals, the file layout, the measured speed and the
acceptance criteria are in [`docs/contracts/print-results.md`](../docs/contracts/print-results.md); the physics and its
limits are in the header of [`src/mech/fffprint.h`](../src/mech/fffprint.h).

**Model.** Simulation layers lump several printed layers and are deposited at once at the nozzle temperature; material
is laid only where it has a whole face on the build plate or shares a face with printed material (overhangs wait for
the layer that carries them; fragments that never connect are counted and not printed). Heat: conduction with k, cp(T)
and density from the library, convection and grey radiation on every exposed face, the bed as a prescribed temperature
that is switched off at the end. Stress: incremental thermo-elasticity where each increment uses the free thermal
strain integral alpha dT with the alpha-weighted path modulus integral E alpha dT / integral alpha dT, counted only
below the relaxation temperature; new material is born stress-free at its programmed position; material above the
relaxation temperature loses its stress and its internal forces are redistributed. Release: the accumulated bed
reactions reversed on an isostatic support.

**Verification** (`tools/mechtest.c`, `test_fff_print` and `test_print_results`; 237 checks in the suite):

| Case | Reference | Result |
|---|---|---|
| Bed-bonded block cooled 100 K in 1 and in 4 increments | the same stress either way | 3.2627e+07 Pa both, difference 1.2e-07 Pa |
| The same block released | stress-free, load-free support | 8.5e-06 Pa, support reaction 3.2e-11 N |
| Layer printed on a cooled layer (bimetal) | Timoshenko 1.5 alpha dT / h = 2.62500 1/m | 2.62490 1/m (-0.004 %) |
| Free bar heated through a modulus collapse (3.0 to 0.6 GPa over 40 K) | its own thermal strain, and its length back on cooling | tip 1.1200e-04 m against alpha dT L = 1.1200e-04 m; 7.7e-18 m after cooling back, largest stress 1.6e-06 Pa |
| Restrained bar through the same collapse, in 1 and 5 increments | integral E alpha dT = 5.04000e+06 Pa | 5.04000e+06 Pa both |
| Reheating above the relaxation temperature | stress released, equilibrium kept | 1.243e+07 Pa to 0, bed reaction resultant 3.2e-12 N |
| Deposition plan: span over pillars, block under the span, floating block | the span in its own layer, the hanging block later, the floating block never | layers 3 3 3 3, hanging 3, floating -1 |
| A whole print of a PLA wall, 8 layers | energy conserved every step, self-equilibrated release | worst balance 5.2e-11, support reaction 4.7e-11 N of bed reactions 1.7e-11 N |
| `results.nvt` with element birth | round-trips exactly | 3 stored times, birth 0 and 2, fields identical |
| A file without element birth | loads as version 3 and as version 2 | both load |

**End to end over MCP** (`tools/printflow.py`, 75 checks, in `make test`): a PLA wall printed by an independent client
through `navier-mcp`; the refusals (missing input or provenance, a material assigned with provenance `default`, a part
lifted off the plate, a stale mesh, a rate that is not a volume per time); duplicate detection; cancellation; the
summary's machine-readable `scope`; the stored-time count (two per deposited layer plus the two cool-downs and the
release); the probe histories; the print read through `results_query`, `results_probe` and `results_export`; a second
engine session that reopens the run directory and reads the same peak von Mises to 1e-9; and an explicitly iterative
run that agrees with the automatic one within 1e-6. Measured on that wall: 9 stored times, peak von Mises 7.791 MPa on
the bed and 1.072 MPa released, warp -0.0231 to 0.0021 mm, worst heat balance 1.21e-10, support reaction after release
2.95e-12 N.

**Demonstration.** The printed Pratt truss bridge (516 x 96 x 105 mm, 681 cm3 solid, 25 234 hex elements at 3 mm, 35
simulation layers of 3 mm, 12.6 h of machine time at 15 mm3/s): peak von Mises 14.76 MPa while on the bed, 5.40 MPa
after release, vertical warp -0.539 to +0.030 mm, worst heat balance 1.5e-10, 0 elements unprintable, 48 deposited
late. `demo/run_bridge_print.sh` rebuilds it with `build/amprint`, the stand-alone driver.

**Limits, beyond those of the physics header.** Layer lumping (a simulation layer is deposited at once; no toolpath
inside it); no creep below the relaxation temperature, so the stress reported while the part is held on the bed is an
upper bound; no plasticity, anisotropy, crystallisation, supports, gravity or adhesion failure; the material is the
library's demonstration PLA; **nothing has been compared with a measured print**, and the summary says so in
`scope.compared_with_measurement`. One body and one material per print. A simulation layer thinner than an element
deposits nothing in the layers that hold no element centroid, which is warned about. Speed: the structural increments
now run on the performance cores and the solver and its tolerance are settings (4 mm bridge: 220.4 s automatic against
97.8 s iterative at 1e-8, peak stress within 2.6e-8 and warp within 3.7e-6); at 3 mm the 180 s target was not reached
(482 s best, 729 s on a repeat of the same command), and the contract states why with numbers. Metal is not supported;
[`docs/lpbf_outlook.md`](docs/lpbf_outlook.md) says what would carry over and what is missing.

## Inherent-strain LPBF build (2026-09-18)

`lpbf_build_run` (contract `navier-mech` 0.7.0) starts a job of kind `lpbf_build`. Layers are activated bottom to top,
each **stress-free on the already deformed part below**, then given an orthotropic inherent strain; the whole active
part re-equilibrates at every layer; the wire cut removes the elements it crosses and releases their internal forces
onto what is left. The method, the refusals, the file layout and the solver's own criteria are in
[`docs/contracts/lpbf-build.md`](../docs/contracts/lpbf-build.md); the mechanics are in the header of
[`src/mech/lpbf.h`](../src/mech/lpbf.h). The inherent strain is a **calibrated input, not a material property**, and
every summary says so. No change was needed in the shared solver: an element activated at step k simply carries no
stress from before k, which the incremental form `K(active) du = f(eps0) + f(released)` already gives.

**Verification (`build/mechtest`, closed forms, no reference data):**

| # | Case | Result |
|---|---|---|
| V1/V2 | one element, free and fully fixed, eigenstrain (-1e-3, -2e-3, -3e-3) | free corner = `eps . x` to 1.5e-14; largest stress 3.4e-7 Pa; fixed `sigma = -C : eps` to 1.7e-16 |
| V3a | activating a layer on the deformed part | moved the part by 0.0 m and changed the stress by 0.0 Pa |
| V3b | 5 layers laid one by one, each strained after activation | top at -2.000000000e-6 m = **one** layer's contraction `eps h`, whatever N (7.0e-15) |
| V4 | bilayer strip, eigenstrain in the upper half | tip 6.005073e-4 m against Timoshenko `3 d_eps / (2 h)` 6.0e-4 m (**0.08 %**); a contracting top layer curls the tip up |
| V5 | cantilever built in 6 layers, largest `u_z` over the free-end face | 1.445653e-3 m against the summed layer moments 1.445267e-3 m (**0.03 %**) |
| V6 | V5 with the element size halved at the same layer thickness | 1.444929e-3 m, **-0.05 %** (protocol A3) |
| V8 | bar between fixed ends, middle element removed | `sigma_xx` 7.000000e7 Pa against `-E eps` exactly; after removal the largest stress is 6.9e-7 Pa and each new face sits at its own `eps L` |

**Validation against measured cantilevers (2026-09-18 to 2026-09-20).** The build was compared with LPBF cantilever
measurements under a protocol written before the runs: first with a commercial simulator's calibrated strains, nothing
tuned, then with strains fitted by `lpbf_calibrate` on two scan strategies and interpolated to predict three others,
which agreed within 3.3 per cent. The measurements, the protocol and every result computed on them were removed from
the repository on 2026-09-27 by the owner's decision, so none of those numbers can be rechecked from it. Two findings
do not depend on the data. **The simulation-layer thickness, not the element size, is what the answer depends on**: a
layer-summed curvature grows as layers are made thinner, so a calibrated inherent strain belongs to the layer size it
was calibrated at. **The strain is given in the machine frame and the part rotates on the plate**
(`docs/contracts/lpbf-build.md` section 7); in the scan frame a Y build comes out bit identical to X, which predicts no
orientation effect at all.

**Plasticity (J2, isotropic linear hardening; contract section 9).** Radial return at the eight Gauss points, the
equivalent plastic strain carried between layers and through the cut, a modified Newton loop per layer on the elastic
operator. Verified first: a uniaxial bar follows the closed form to 1.2e-11 (perfectly plastic) and 9.6e-12
(hardening), unloads elastically and keeps exactly the residual strain `eps_max - sigma_max / E`, an eigenstrain driven
past yield is capped at exactly 230.000 MPa, and the fully plastic moment of a rectangular section is **1.5003 times**
the first-yield moment against 1.5. The elastic path is bit identical: mechtest 255 (the 247 elastic cases unchanged),
lpbfflow 116. Aitken acceleration was tried and **converged to the wrong state at the limit load** (P3 gave 1.15
instead of 1.50), so it was removed; the convergence tolerance was set to 1e-6 after measuring that it moves the answer
by 0.009 per cent against 1e-8.

**Material.** The AlSi10Mg record the fits were made with held measured values that are not public; on 2026-09-27 it
was replaced by `alsi10mg_lpbf`, whose values come from a published data sheet, each with its source. The library
accepts `measured` and `published` for a built-in record only when every value names its source.

**Supports (step C).** Every voxel column below a part element, down to the plate at z = 0, that is not part becomes
support: group 1, a homogenised lattice at a stiffness fraction that is an input with a provenance (Simufact's default
is not known to this project), activated with its layer, and with plasticity yielding at the same fraction of the
solid's yield stress. With supports a part may stand above the plate. They are removed after the cut, or at the end
when asked. Verified: a box on the plate gets none and builds bit identically; a bridge on two legs gets exactly the
96 cells under its span; every support is born with its layer and dies at its removal in the result file.

**The owner's cylinder head (step D)** was studied here and removed from the repository on 2026-09-27 for copyright,
with its comparison, runs and scripts.

**Adaptive voxel mesh (wave 5, step A; [`docs/contracts/adaptive-mesh.md`](../docs/contracts/adaptive-mesh.md)).**
Fine at the surface, coarse inside, 2:1 balanced, coarsened in x and y only because an element must stay within one
layer (the Simufact Additive tutorial coarsens the same way: x and y by a coarsening factor, z by the layers, p. 155).
Coupling by hanging-node constraints eliminated in assembly (`src/fem/solid.c`, additive: a conforming mesh takes the
old path). A patch test across a 2:1 interface reproduces `eps . x` to 1.4e-14 with no stress, forces cross the
interface in balance (free-body reactions 1.7e-15), and a constraint chain resolves to the same field. The cantilever
at 0.5 mm, coarse up to 2 mm with a two-voxel fine band, is within 0.57 per cent of the uniform mesh with 29 820
elements instead of 36 000, in 20 s instead of 60. With a one-voxel band it was 3.3 per cent stiff, which is why the
band is two.

**Support rule (wave 5, step B; [`docs/contracts/lpbf-build.md`](../docs/contracts/lpbf-build.md) section 11).**
Supports only under downward faces flatter than the critical angle (45 degrees, assumed: the Simufact Additive
tutorial names the parameter on p. 55 without a value), and only where the column reaches the plate through empty
space, so none inside channels; stiffness fraction 0.3, assumed (the tutorial gives no default, p. 60). Box none,
bridge the 96 cells of its span, channel none (the wave-4 rule put 256), 30-degree face supported, 60-degree face not.

**Supports as typed physics (T16; [`docs/contracts/supports.md`](../docs/contracts/supports.md)).** Five types (block,
thin wall, cone, tree, lattice) plus explicit (meshed with the part) and the wave-4 homogeneous fraction, every
parameter a quantity with a provenance; the tutorial names the parameters and gives no default for any (pp. 54 to 60,
224), so every default is recorded as assumed. `lpbf_supports_generate` returns the supports without building: volume,
contact area, faces served and left, and the homogenised bands. Each type's unit cell is voxelised with volume fractions
and solved explicitly: the full orthotropic stiffness by periodic homogenisation (six macro strains, periodic
fluctuation through the solver's constraint table), conduction by finite volumes, and the surface per volume. Support
elements in the LPBF build take their band's matrix (elastic); in the FFF print they conduct and store heat as their
band, exchange heat over their own surface, are scaled in stiffness and are removed after the bed release. The
interface tear-off is reported. Verification: S1 to S4 (the cells) pass exactly or within 4.2 %; S7 to S9 (generator,
refusals, removal) pass. **S5 passes but does not discriminate; S5b fails** (-23.5 % on the plate at the slab's
overhanging corners, while the top surface agrees within 2 %); **S6 fails** (deck +9.0 %, heat into the bed +108 %,
after a surface-exchange change that closed most of a +27 % and +241 % gap). The homogenised support reproduces the
part's field away from the supported face when the pitch is small against the part; its extremes at the supported
face, and its heat path, are not yet verified.

**What the cheap cases settled instead (wave 5, part 1).** A3 with the adaptive mesh on the cantilever, element size
alone: -6.94 per cent in X and -5.18 per cent in Y when the surface cell is halved, against -6.49 and -4.71 on the
uniform mesh, so the adaptive mesh reproduces the uniform mesh's convergence (each size within 0.5 per cent) and A3
fails in X on both, as it has since wave 3. S10, the cantilever's comb replaced by the homogenised supports: it fails
the protocol's 15 per cent by +74 to +108 per cent with the comb's own wall pattern and by -79 to -82 per cent with
block supports, where the explicit teeth predict within 3.2 per cent. The unit cell shows why (no shear stiffness
across parallel plates), and the rule that follows is in the supports contract: a homogenised support may not replace
a support structure a few pitches tall or one whose stiffness comes from its ends.

**What the application shows** (re-checked on 2026-09-18 after the merge of main). The window **draws the part
growing**: at stored time 1 of 10 the surface is 2 960 triangles, the first millimetre of the build, against 11 920 for
the finished shape, and it plays the ten stored times and draws the springback (shown by a demonstration script removed on 2026-09-27).
The application session hooked `elem_birth` up in `result_visibility` (`src/fembridge.c`) in the meantime. **The cut is
still drawn closed**: that function returns as soon as it has the birth array and never sets `v->death` or `v->group`,
and its comment there still says the two arrays are not written, which is no longer true (results.nvt version 4 carries
both, and the LPBF flow test round-trips them). Two more lines in that function, which belongs to the application
session, would open the kerf; the solver side needs nothing.

## Performance and memory (MacBook Air M2, 8 GB, fanless; `build/mechbench`)

Throughput varies by up to 2× between runs as the passively cooled machine heats up; the ranges below span two runs.

| Model | Throughput |
|---|---|
| Revolute chain, RKMK4, h = 1 ms | 2 links 340k–780k steps/s; 8 links 150k–220k; 32 links 33k (O(n²)–O(n³) from the dense mass matrix and constraint solves) |
| DC motor + encoder + PID, 1 kHz control, 0.5 ms max step | 185–195× real time; 44 history channels × 2001 rows = 0.7 MB; checkpoint 5.6 kB |
| Box stacks with contact, h = 1 ms, warm-started solver | 1 box 480k steps/s; 2 boxes 145k; 4 boxes 31k; 8 boxes 4.9k (11 unconverged steps, 0.8 µm drift in 1 s); 16 boxes 610 (44 unconverged steps, 12.5 µm drift) |
| Grasp study over MCP (0.3 s at 0.2 ms, plus the half-step check) | completes in well under a second of solver time |
| Craig–Bampton reduction of the 120×16×10 mm arm with a tip interface | 2 mm mesh (3294 nodes), 4 fixed-interface modes: 1.2 s; with 24: 6.6 s; 1 mm mesh (22 627 nodes), 8 modes: 46.5 s |
| Reduced arm as a pendulum with a 150 g payload, at the stability step | 10 coordinates (highest 11.2 kHz, h = 7.1 µs): 163k steps/s, 1.2× real time; cut at 2 kHz, 5 coordinates (h = 48 µs): 267k steps/s, 12.7× real time; 30 coordinates (54.6 kHz, h = 1.5 µs): 42k steps/s, 0.06× real time |

State size is about 1.2 kB plus 8 bytes per coordinate. With contact, the checkpoint adds 64 bytes per possible contact
row (the gripper: 28 rows, 1.8 kB). Histories are the main memory cost (8 bytes × channels × rows, capped by
`max_records`).

## Supported regimes and limits (as of this report)

- **Rigid bodies.** Joint compliance appears only as declared springs and transmissions. Parts do not deform in the
  dynamics; FEM assessments are quasi-static snapshots.
- **Integration without contact.** RKMK4 is explicit: stiff springs, high servo gains or tiny regularisation speeds need
  small steps. Joint limits are events; resting limits release at step boundaries (O(h) timing).
- **Integration with contact.** First-order Moreau–Jean time stepping (O(h): the incline case is 0.10% off at 1 ms).
  Impacts are instantaneous impulses: impact forces, contact duration and impact stress need compliant contact, which
  does not exist yet, so impact steps are excluded from force peaks and refused for FEM assessment.
- **Contact geometry.** Sphere, capsule, box and plane from assemblies; convex hulls only against planes and only
  through the C API. Mesh and cylinder collision geometry are refused, never approximated. There is no rolling or
  torsional friction. Bodies joined by a joint never touch each other.
- **Contact solver.** Projected Gauss–Seidel with warm start. Tall stacks converge slowly while they settle (4 boxes:
  converged after the first step; 16 boxes: 44 of 1000 steps stop at 500 sweeps, 12.5 µm drift in 1 s). The split of
  forces within a flat face and among the contacts of a grasp is statically indeterminate; the reported split is one
  admissible solution.
- **Outputs.** Histories are kept for the first 64 shape pairs. The warm-start store makes checkpoints 64 bytes per
  possible contact row (the largest manifold of every pair that can touch, at most 1024 rows).
- **DC motors.** Quasi-static electrical model (no inductance) with constant resistance. The efficiency switch between
  driving and back-driven is nonsmooth and affects sampled peaks at the 0.1–1% level.
- **FEM assessment.** Quasi-static linear elasticity of one part at one instant, isotropic (shared solver) or orthotropic
  (mechanics solver). Contact loads are resultant-preserving tractions over a selection, not contact pressures.
- **Flexible bodies.** First-order floating frame: small linear elastic deformation, no centrifugal stiffening or
  softening and no geometric stiffness from axial load (the moving-mass case measures the omitted term). Interfaces and
  the root region are rigid. The integrators are explicit, so steps are limited to 0.5/ω_max of the kept coordinates;
  `max_frequency_hz` trades step size against the reported loss of interface flexibility (the time cost grows fast with
  the highest kept frequency; see Performance). Coupled stresses are evaluated at recorded instants only, carry nothing
  for dropped coordinates, and near the clamped root and rigid interfaces depend on that idealisation; von Mises is only
  an indicator for orthotropic parts (failure indices come from `mech_fem_assess` or `mech_transient_assess`, which use
  the reference-frame inertia of a flexible body and warn about it). Loops
  through flexible bodies, contact geometry on them and massless links fixed to them are refused. IMUs on them follow
  the reference frame (warned).
- **Vibration and transient response.** Linear vibration about the undeformed configuration. `mech_transient_assess` is
  one-way: the deformation does not act back on the rigid motion (coupled flexible dynamics are the flexible bodies above). There is no centrifugal stiffening, gyroscopic coupling or
  impact response (windows with impacts are refused). Damping is a stated modal ratio, never a default. Stress histories
  cost samples × Gauss points × (patterns + modes); above 4e9 operations they are evaluated at every k-th sample, which
  the summary reports.
- **Printed material.** A homogenised orthotropic solid: no layer-scale stress concentrations, voids, bond defects,
  fatigue or damage. Strength criteria compare stresses with the strengths the user supplies. No printed-material
  values are built in. The default Tsai–Wu interaction (−½, Tsai–Hahn) is recorded as an assumption; with it, the 3D
  quadratic form is semidefinite, so the criterion has no curvature along one stress direction. The voxel mesh may sit a fraction of an element away from the placed part: loads follow the mesh
  (offset reported, warned above 1% of the part size).

## Remaining work, in order

-1. Printing: a measured print to compare against (nothing is validated); creep below the relaxation temperature, which
   would lower the stresses reported on the bed; more than one material or body; and the speed levers that live in
   `src/fem` (a symbolic factorisation reused between increments with the same active set, a preconditioner stronger
   than block Jacobi for the 3 000:1 molten-to-cold stiffness contrast). Metal: `docs/lpbf_outlook.md`.
0. Rotor reaction on the mount: model the reflected rotor's angular momentum with its coupling to the parent body (mass matrix,
   recursive Newton–Euler, energy) so the reaction of a geared motor acts on the body that carries it in the dynamics, as it already
   does in the load assessment; verify against a rotor-on-a-spinning-base closed form and energy conservation.
1. Measurement import, bounded calibration, parameter and sensitivity studies; demonstrations A (two-link arm),
   B (printed gripper: grasp force, slip margin, orthotropic finger stress; the MCP flow above is its first version) and
   C (flexible printed link: the flexible arm above, with coupled stresses, is its first version; an orthotropic printed
   link with failure indices from the coupled coordinates is still to come).
2. Failure indices of orthotropic flexible bodies from the coupled coordinates (the reduction stores the material with the
   shapes).
3. Orchestrator participant adapter against the shared orchestrator (available since re-baseline 2), one-way first;
   then the serial merge into the shared checkout.
4. Contact extensions: compliant contact for impact forces, hull–hull and part pieces from meshes (convex
   decomposition declared by the user), rolling friction.

## Printing numerical integrity, 2026-10-03

Criteria were added before their first runs in [the FDM contract](../docs/contracts/print-results.md#8-numerical-amendments-2026-10-03)
and [the LPBF contract](../docs/contracts/lpbf-build.md#13-numerical-amendments-2026-10-03).
`make build/mechtest` and `build/mechtest --printing` exercise the same checks included in the normal mechanics suite.

- FDM now supplies the complete deposited nozzle enthalpy, including the heat missing at conforming interface/bed
  nodes. Constant/linear-capacity three-layer checks close against an independent polynomial enthalpy integral at
  1.85e-16/4.93e-16 relative; removing a 0.25-capacity support closes at 3.08e-15. Nodal temperatures stayed 300 to 400 K.
- The eight-layer demonstration PLA wall supplied 3449.70746 J, including a 1500.57176 J deposition correction;
  stored + bed + air heat closes at 1.63e-11 relative. This is numerical conservation, not measured-print validation.
- Exact piecewise-table thermoelastic integration replaces the fixed temperature-grid trapezoid. A 0.01 K transition
  with an interior modulus-floor crossing gives the closed-form restrained stress 448742.083333 Pa within
  2.72e-15 relative in one increment and 1.69e-15 in multiple increments.
- Elastic LPBF plate reactions now carry the accumulated stress: the fully held verification hex gives 33.653846 MN
  after one eigenstrain increment, 67.307692 MN after two and the same after a zero-strain equilibration.
  A free uniform contraction retains a stable last-solve equilibrium error of 1.26e-16, rather than normalising by
  near-zero recovered stress. Removed support-only nodes no longer determine shown-part temperature/warp extrema.
- Both print stress fields now average Gauss-point von Mises after evaluating the invariant. In the checked bent
  strips, taking the mean tensor first understated this scalar by up to 28.58 percent in FDM and 52.01 percent in LPBF.

The first-substep deposition pulse is a stated simulation-layer approximation and needs temporal convergence for
local temperatures. FDM still lacks resolved roads, inter-road thermal contact, raster anisotropy/interlayer strength,
crystallisation and sub-transition creep. LPBF remains an inherent-strain process model with optional isothermal J2
plasticity; no laser-resolved thermal cycle or melt-pool prediction was added. No new measured material values were invented.
