# Contract: large deformation, explicit dynamics and deforming contact (T18, phase B)

Up: [ROADMAP-SOLVERS.md](../design/ROADMAP-SOLVERS.md) phase B, [AGENTS.md](../../AGENTS.md). Written 2026-09-20
**before the code**. The linear path is untouched: `solid_solve` and everything that calls it keep their behaviour,
and the unchanged suites prove it. Every verification case below runs in **seconds**; anything longer goes to
[validation/QUEUE.md](../../validation/QUEUE.md) with its expected time (rule 8).

Five states, as everywhere in this project: what is implemented, what is verified against a closed form or a
published benchmark, what is integrated, what is reachable through MCP, what is measured against reality. A case that
fails is recorded with its numbers; tolerances are not widened after a run.

## 1. What is built

**Step 1, large deformation, implicit.** A total-Lagrangian hex8: deformation gradient F = I + du/dX, Green-Lagrange
strain E = (F'F - I)/2, second Piola-Kirchhoff stress S. Material: St. Venant-Kirchhoff first (S = C:E with the same
C the linear path uses), then J2 plasticity at finite strain. Internal force and the consistent tangent
(material plus geometric term) at 2x2x2 Gauss points, full integration. Newton with the consistent tangent, load
stepping with cut-back: a step that does not converge is halved and retried, and the result reports the steps taken.

**The finite-strain plasticity route: multiplicative, with the return map in logarithmic strains.** F = Fe Fp; the
elastic predictor is formed from the elastic left Cauchy-Green tensor, its logarithm gives the elastic Hencky strain,
the existing radial return of `src/mech/lpbf.c` (J2, linear isotropic hardening) is applied to the Kirchhoff stress in
that strain measure, and the plastic part is updated by the exponential map. Why this and not a hypoelastic rate with
a Jaumann or Green-Naghdi rate: the multiplicative route is objective by construction (no incremental rotation to
integrate, no stress drift under large rotation) and the return map is literally the small-strain one, so the
plasticity already verified for the LPBF build is reused rather than rewritten. It is the standard route for metals
(Simo and Hughes, *Computational Inelasticity*, chapter 9).

**Known limit, stated before the runs:** the nonlinear element is fully integrated and therefore locks in bending;
the bending cases below say how many elements through the thickness they use. Enhanced assumed strain or B-bar for
the nonlinear element is future work, and the contract will say so in its results table rather than the tolerances
being loosened.

**Step 2, explicit dynamics.** Central difference with a lumped (row-sum) mass matrix. The stable time step is
`dt = safety * L_min / c` with `c = sqrt((lambda + 2 mu) / rho)` the dilatational wave speed and `L_min` the smallest
element dimension; the step used, the element that sets it and the safety factor are reported. One-point (reduced)
integration with **Flanagan-Belytschko stiffness-form hourglass control**, its coefficient an input with a default
that is stated as assumed. Mass scaling is available and always reported (the scaled mass, the steps it bought and
the ratio of added mass to real mass). Every step accounts for kinetic, internal, hourglass and external work energy,
and the balance is written to the result.

**Step 3, contact.** Node-to-surface penalty contact with Coulomb friction, including self-contact, with a bucket
(uniform grid) search over the deforming surface rebuilt every N steps. The friction law and its parameters are the
mechanics layer's (`src/mech/contact.h`): the rigid-body path is not duplicated, and its tests are the reference for
the same physical cases.

**Step 4, the picture.** A thin-walled square tube crushed between rigid plates, elastic-plastic, folding with
self-contact, from a library material with its provenance. Time-indexed result file so the application plays it.

## 2. Verification cases and criteria (registered before the code)

| id | case | criterion | expected time |
|---|---|---|---|
| **D1** | one element stretched uniformly, St. Venant-Kirchhoff | S and the internal force equal the closed form to 1e-10 relative | < 1 s |
| **D2** | rigid-body rotation of a strained-free block through 90 degrees | every stress component stays below 1e-10 of E (objectivity) | < 1 s |
| **D3** | a bar stretched to **50 per cent** (lambda = 1.5), lateral faces free | the axial stress and the lateral contraction match the closed form of the St. Venant-Kirchhoff bar to **1e-6** relative | < 2 s |
| **D4** | the same bar with J2 plasticity, stretched and released | the stress follows the closed form of uniaxial J2 with linear hardening in logarithmic strain to **1e-6**; the plastic strain after release matches | < 2 s |
| **D5** | **elastica**: a cantilever with a transverse tip load, deflection up to about a third of its length | tip deflection and tip rotation within **2 per cent** of the exact elliptic-integral solution, mesh stated | < 20 s |
| **D6** | **Euler buckling**: an axially loaded column, the load at which the tangent stops being positive definite | within **2 per cent** of pi^2 EI / (K L)^2, for pinned-pinned (K = 1) and clamped-free (K = 2) | < 20 s |
| **D7** | a free bar struck at one end, explicit | the wave arrives at the far end at L / c and the reflected velocity matches the 1D solution within **2 per cent**; dt reported | < 5 s |
| **D8** | a clamped beam released from a deflected shape, explicit | its first natural frequency, from the free vibration, within **2 per cent** of the mechanics layer's modal solver on the same mesh | < 20 s |
| **D9** | the same run without damping and without contact | total energy (kinetic + internal + hourglass) stays within **1 per cent** of the initial value | < 20 s |
| **D10** | hourglass control on a single reduced-integration element under pure bending | the hourglass modes stay below 5 per cent of the elastic energy, and the fully integrated element's answer is reproduced within 5 per cent | < 1 s |
| **D11** | a block on an incline, explicit with contact | it stays below the friction angle and slides above it with the acceleration g (sin a - mu cos a) within **2 per cent**, matching the rigid-body case of the mechanics layer | < 10 s |
| **D12** | **Hertz**: an elastic sphere pressed on a rigid plane | contact radius, approach and peak pressure within **5 per cent** of the closed form | < 30 s |
| **D13** | an elastic ball dropped on a rigid floor | the rebound height is within **5 per cent** of the drop height (no dissipation), and the contact impulse matches the momentum change to 1e-6 | < 20 s |
| **D14** | self-contact: a sheet folded onto itself | no penetration beyond the penalty tolerance, and the fold does not pass through | < 30 s |
| **D15** | **the crush tube, coarse**: a thin-walled square tube crushed 30 per cent | the mean crushing force within **15 per cent** of Abramowicz and Jones, the fold half-wavelength within 20 per cent of their estimate; the fine mesh goes to the queue | < 10 min, coarse in the suite |

D15's reference: W. Abramowicz and N. Jones, "Dynamic axial crushing of square tubes", *International Journal of
Impact Engineering* 2(2), 1984, 179-208: mean crushing force `P_m = 38.27 M_0 (b/t)^(1/3)` per the superfolding
element with `M_0 = sigma_0 t^2 / 4`, and the fold half-wavelength `H = 0.99 b^(2/3) t^(1/3)`. The constants and the
flow stress `sigma_0` used are stated with the result.

## 3. What this contract does not claim

Nothing here is validated against a measurement of ours. D15 is compared with a published empirical formula, which is
itself a model fitted to experiments; the comparison is a benchmark, not a validation. No result of this contract may
be quoted as validated accuracy.

## 4. Results

### Step 1, large deformation, implicit (2026-09-20, `build/dyntest`, the whole step in about 10 s)

| case | measured | criterion | verdict |
|---|---|---|---|
| D1 uniform stretch (lambda 1.2) | S11 and S22 equal the closed form to 1e-16; the face force equals the first Piola-Kirchhoff traction to 1e-16 | 1e-10 | pass |
| D2 rigid rotation through 90 degrees | largest stress 4.27e-16 of E | 1e-10 | pass |
| D3 bar to 50 per cent stretch | axial force 2.625000000e7 N against E E11 lambda A0, 6.7e-16 relative; lateral stretch 0.790569415 against 0.790569415 | 1e-6 | pass |
| D4 finite-strain J2 (5 per cent stretch) | the Kirchhoff von Mises sits on sigma_y + H alpha to 1.3e-15; det Fp = 1 to 1e-12; the logarithmic strain splits into 0.004828 elastic and 0.043963 plastic against 0.048790 total | 1e-6, 1e-10 | pass |
| D5 elastica (P L^2 / EI = 3) | tip rise 0.301690 m against the exact 0.301627 (+0.02 per cent), shortening +0.02 per cent, tip rotation 0.9860 rad; 40 x 2 x 4 elements, 12 increments, 85 iterations, no cut-backs, 5.4 s | 2 per cent | pass |
| D6 buckling, pinned-pinned | 26 056 N against Euler's 26 319 (-1.00 per cent, the load increment being 1 per cent); 2 x 2 x 40 elements, 1.1 s | 2 per cent | pass |
| D6 buckling, clamped-free | 6 579.7 N against 6 579.7 (+0.00 per cent), 1.0 s | 2 per cent | pass |

**What the runs forced, recorded rather than smoothed over.**

1. **The element gained enhanced assumed strain.** Fully integrated, it was 26 per cent too stiff on the elastica and
   carried more than 1.2 times Euler's load without the tangent losing definiteness: shear locking, as section 1
   predicted. Nine enhanced modes, condensed element by element, fixed it (D5 +0.02 per cent, D6 within the load
   increment) with no tuning constant. For St. Venant-Kirchhoff the enhanced parameters are linear and solve exactly
   in one step, so the element keeps no state for them. The plastic branch stays fully integrated: its verification
   (D4) is a stress and a plastic strain, not a bending stiffness, and the crush tube of step 4 runs explicitly.
2. **Both tangents were checked against finite differences of the element's own force** (a development check, not a
   contract case): 7.1e-10 relative for the fully integrated element and 7.3e-10 with the enhanced modes. The first
   version of the condensation was wrong by 14 per cent and Newton stalled at 8e-6; the check found it in one run.
3. **The plastic branch uses the elastic tangent** (modified Newton), as the LPBF build does. D4 converges with it;
   the consistent elastoplastic tangent in principal logarithmic space is not written. Stated here rather than in the
   criteria, because no criterion of this contract measures the convergence rate.
4. **D4's yield check is on the Kirchhoff stress**, which is where this algorithm's yield function lives; the Cauchy
   stress differs from it by the elastic volume change (337.3 against 337.9 MPa here), and comparing the Cauchy
   stress with sigma_y is what made the case look 0.2 per cent wrong before.
5. **A pinned end has to be a pin.** Holding the whole end face in the axial direction stops the end section from
   turning, which is a clamp: the column then carried more than 1.2 Pcr. Only the centre node is held now, plus one
   node against the turn about the column's own axis.

Nothing else in the contract changed. Steps 2 to 4 follow.

### Step 2, explicit dynamics (2026-09-20, `build/dyntest`, the whole step in 16 s)

| case | measured | criterion | verdict |
|---|---|---|---|
| D7 wave in a bar | wave speed 5063.7 m/s against sqrt((lambda + 2 mu)/rho) = 5063.7; the far end sits at 0.0000 m/s before the front arrives and at 1.9616 m/s after it (the 1D solution gives 2 v0 = 2.0, -1.92 per cent); the stress behind the front 3.9966e7 Pa against rho c v0 = 3.9497e7 (+1.19 per cent); dt 1.777e-6 s set by element 56, 156 steps, safety 0.9 | 2 per cent | pass |
| D8 beam frequency against the modal solver, 30 x 3 x 3 elements | explicit 133.24 Hz, modal solver on the same mesh 111.23 Hz (**+19.79 per cent**), 31 683 steps, 3.4 s | 2 per cent | **fail** |
| D9 energy | drift -0.0019 per cent over the run, hourglass share 0.00 per cent of the total | 1 per cent | pass |
| D10 hourglass on one element in bending | one point 4.3264e2 J plus hourglass 1.1538e2 J = 5.4803e2 J against the full element's 5.7686e2 J (**-5.00 per cent**), hourglass share **21.05 per cent** | share below 5 per cent **and** within 5 per cent of the full element | **fail** (the share) |

**D8: what it actually compares, and why it fails.** The criterion as written compares two different
elements on the same mesh: the modal solver is the fully integrated linear hex8, the explicit path is the one-point
element with hourglass stabilisation. On a mesh three elements through the thickness neither is close to the beam:
the analytic clamped-free first frequency is 90.89 Hz, the modal solver gives 111.23 (+22.4 per cent, shear locking)
and the explicit element 133.24 (+46.6 per cent, hourglass stiffness carrying the bending). Two answers that are
wrong in different ways cannot agree to 2 per cent, and the criterion was registered without noticing that. It is
recorded as a failure. It is not widened, and the code is not tuned to pass it.

**D8b, an amendment registered 2026-09-20 after the D8 result, now in the suite.** The explicit solution must
approach the analytic beam frequency as the mesh is refined along the beam:

| elements along the beam | Q = 0.05 | Q = 0.0125 |
|---|---|---|
| 30 | 133.24 Hz (+46.60 per cent) | 99.68 Hz (+9.68 per cent) |
| 60 | 101.75 Hz (+11.95 per cent) | 89.93 Hz (-1.06 per cent) |
| 120 | 92.19 Hz (+1.44 per cent) | not run |

The suite runs the first column (30 and 60 elements, 12 s) and checks that refinement moves the frequency toward the
analytic value; the 120-element case was run once by hand (61 031 steps) and is quoted, not re-run every build. The
energy drift over these runs is between 0.002 and 0.019 per cent, so the integrator is doing what a central
difference should: the error is the element in bending, not the time integration.

**The hourglass coefficient stays at Q = 0.05.** The table above shows that a smaller coefficient flatters the
bending cases, which is exactly why it is not chosen that way. 0.05 is the value the Flanagan-Belytschko stiffness
form is commonly used with; it is an assumed default, it is an input, and the two columns above say what it costs on
a coarse bending mesh so a user can judge it. Tuning it to a verification case would make the case meaningless.

**D10: its two criteria cannot both hold, for the reason the case exists.** A single element in pure bending
deforms *entirely* in the hourglass modes: the one-point evaluation at the centre sees 75.0 per cent of the full
element's energy (4.3264e2 of 5.7686e2 J) and the rest can only come from the stabilisation. Requiring the
stabilisation to supply that and to stay below 5 per cent of the energy is a contradiction, as the sweep shows:

| Q | hourglass share | total against the full element |
|---|---|---|
| 0.05 | 21.1 per cent | -5.0 per cent |
| 0.0125 | 6.3 per cent | -20.0 per cent |
| 0.0008 | 0.4 per cent | -24.7 per cent |

The energy criterion is met at Q = 0.05 (-5.00 per cent); the share criterion is not. Recorded as a failure of the
criterion as registered.

**D10b, an amendment registered 2026-09-20, now in the suite:** the hourglass modes must not answer a deformation
that is not hourglass. A uniform stretch of the same element gives a hourglass share of exactly 0 (0.00e+00), and on
the beam mesh of D8 the share is 0.00 per cent of the total energy. That is what the stabilisation has to satisfy;
the share on a single element that *is* one hourglass mode is not a defect of the control.

**Also recorded.**

1. **The internal energy is the stored energy, not an accumulated work.** D9 first read a drift of -100 per cent
   because the run started from a deflected shape whose stored energy was not counted. The explicit path now
   evaluates the internal energy as the integral of S:E/2 at the element centre, and the balance is read after a
   zero-length advance so the starting state includes it.
2. **The critical step is reported with the element that sets it** (D7: element 56), because on a graded mesh one
   small element sets the cost of the whole run, and a user who cannot see which one cannot fix it.
3. **Mass scaling is implemented and reports the added-mass ratio, but no case of this contract uses it.** It is
   implemented and not verified.

### Step 3, contact (2026-09-20, `build/dyntest`)

| case | measured | criterion | verdict |
|---|---|---|---|
| D11 block on an incline, 25 degrees | it moved 2.9e-09 m in 0.10 s; mean normal force 16.6380 N against m g cos th = 16.6380 | sticks below the friction angle | pass |
| D11 block on an incline, 28 degrees | acceleration 0.2748 m/s^2 against g (sin th - mu cos th) = 0.2746 (**+0.09 per cent**); mean normal force +0.03 per cent of m g cos th, mean friction 8.1067 N against mu N = 8.1046; 66 722 steps, 0.7 s | 2 per cent | pass |
| D12 Hertz, force | 3.1249e6 N against 3.1516e6 at an approach of 1.3226 mm (-0.85 per cent), 512 elements, penalty scale 50, 22 200 steps, 4 s | 5 per cent | pass, **but see below** |
| D12 Hertz, contact radius | 8.803 mm against sqrt(R delta) = 8.132 (**+8.25 per cent**) | 5 per cent | **fail** |
| D12 Hertz, peak pressure | 2.0641e10 Pa against 3F/(2 pi a^2) = 2.2756e10 (**-9.29 per cent**) | 5 per cent | **fail** |
| D13 ball rebound | height ratio 1.0000 (**-0.00 per cent**), contact 596 us, energy drift 0.0000 per cent | 5 per cent | pass |
| D13 contact impulse | 7.890402735e-01 N s against the momentum change 7.890402476e-01 (**3.3e-08** relative) | 1e-6 | pass |

**D12 is not verified, and the force result must not be quoted as if it were.** The sphere is 512 elements and the
Hertz patch at the end of the press is 0.7 of an element across, so the force is a sawtooth about Hertz as each ring
of nodes comes into contact. Along the press it runs **+82, +30, +6, -8, -17, -25, -17, -12, -5, -1 per cent**
(`D12_TABLE=1 ./build/dyntest` prints the table), and a power fit sees `delta^1.32` where Hertz has `delta^1.5`. The
-0.85 per cent at the end is where the sawtooth happens to cross zero. Refining to 1728 elements moves the radius to
-1.46 per cent and the pressure to -6.85 per cent but the force to +14.10 per cent, and costs 89 s: still inside the
sawtooth. Hertz needs a mesh graded at the pole, which this ball mesh cannot produce; the case is on
[validation/QUEUE.md](../../validation/QUEUE.md) with its plan and expected time. No tolerance was widened and no
constant was tuned: the criteria stand as registered and two of the three are recorded as failed.

**The penalty scale had to be 50 for D12, against the default 0.1**, and that is a real limit of a penalty contact
worth stating: the sink depth of a body pressed into a surface is about `a / (scale * L)` times the approach, with
`a` the patch radius and `L` the element size. At the default the plate sank thirteen times the Hertz approach into
the mesh. The scale is an input, the penetration is reported with every result, and a case that needs a hard contact
has to say what scale it used.

**What the runs forced.**

1. **The stable step is taken from the element's own highest frequency, not from a length over the wave speed.** The
   mapped sphere of D12 and D13 blew up in twelve steps at a step that "looked safe" (safety 0.9 on the shortest
   edge). The step now comes from a power iteration on `M^-1 K` of each element at the start, with the length
   estimate kept only as an upper bound and for the report. For a cube the two agree; for the sphere's distorted
   elements the frequency bound is 1.7 times smaller. D7's numbers are unchanged.
2. **A stiff penalty needs normal damping, or a resting body never rests.** The static penetration of the D11 block
   is 4e-9 m, so an oscillation of a few nanometres lifts it off and friction comes and goes: the measured sliding
   acceleration was +10.1 per cent out with a perfectly good friction law. A dashpot at a fraction of critical
   (default 0.1, assumed and reported, acting only while the surfaces approach or separate) fixed it to +0.09 per
   cent. What it dissipates is accounted separately from friction.
3. **Self-contact must exclude the neighbourhood of a node, not only its own facets.** A node on the side of a strip
   sits half a thickness from its own top and bottom faces, and the first version pushed it away from them: the
   strip contacted itself where it was merely thin, and an element inverted in 36 steps. Facets whose element is
   within two element rings of the node are now excluded.
4. **A prescribed node cannot be stopped by a contact force.** The first version of D14 drove the two ends of the
   hairpin into each other and they passed straight through, which proved nothing about contact. Both D14 and the
   crush tube of step 4 press with rigid plates and hold nothing.

| case | measured | criterion | verdict |
|---|---|---|---|
| D14 self-contact, the fold | the two inner surfaces come within 1.22 mm and do not cross; 60 elements, 184 facets, 133 400 steps, 51 s | the fold does not pass through | pass |
| D14 penetration | 0.025 mm against the declared tolerance of 0.080 mm (a tenth of the strip's thickness), 0 nodes past the depth limit | the declared tolerance | pass |
| D14 momentum | the transverse momentum stays at 1e-18 to 4e-16 kg m/s (it starts at zero) | not a registered criterion, reported | - |

**D14's energy balance cannot be read, and this is a limit of the implementation, not of the case.** It reports
**+103 per cent**. The internal energy is `1/2 S : E` at the element centre, which is the stored elastic energy only
while the point is elastic: where the strip folds it yields, E is then the total strain, the term over-counts, and
the plastic work is not accumulated anywhere. Run with the yield removed (`D14_ELASTIC=1`) the same case drifts
**-2.40 per cent**, and there the arms spring back 1.36 mm short of meeting instead of folding, which is why the
plastic form is the one in the suite. **Open:** take the elastic energy from the elastic Hencky strain and accumulate
the plastic work, so that D9's criterion can be applied to plastic runs as well. Until then no energy balance from a
run that yields, the crush tube of step 4 included, may be quoted.

**D14 also forced the contact step limit to be corrected.** A spring against a rigid surface sees the node's mass; a
spring between two pieces of the same deforming body sees their reduced mass, and the facet's share is spread over
four nodes by the shape weights. Taking the step from `sqrt(k/m)` blew the folded strip up twice (stored contact
energy 9.4e4 J against 0.05 J of kinetic energy, an element inverted); the limit for self-contact now carries a
factor of two, and the case runs with a drift of -2.4 per cent in its elastic form.
