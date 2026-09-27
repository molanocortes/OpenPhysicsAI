# Independent evaluation of 0.3.0-rc1: criteria fixed before running

Written on 2026-09-16 at 23:35 CEST. At that point no CalculiX solve had been run and no targeted beam variant had been
computed. The only computed results were those stored by the previous session and the four-level refinement audit,
which involves no CalculiX. Changes are appended under "Amendments" with the observed values; the text above them is
never edited.

Solver under comparison: CalculiX `ccx` 2.23, conda-forge build `calculix-2.23-pl5321h33a25c5_4` (osx-arm64),
installed with micromamba 2.9.0 from conda-forge into `~/.navier-eval-tools/ccx-env`.

## X1: the same assembled problem (NAVIER's own mesh, supports and nodal loads exported to CalculiX)

**Purpose.** Detect errors in NAVIER's assembly, supports, load vector, reactions, energy and stress recovery. A
second code solving the identical discrete problem does not test NAVIER's setup: the geometry, regions, load
distribution and mesh are carried over from NAVIER by construction.

**Cases.**
- Cantilever C10 at 1.25 and 0.625 mm.
- Bracket A and bracket B at 1 mm.

Each case is run twice:
- NAVIER `full_integration` against CalculiX `C3D8`: both are the standard trilinear hexahedron with 2 × 2 × 2 Gauss
  integration, so on identical box elements the element matrices should be identical.
- NAVIER `incompatible_modes` against CalculiX `C3D8I`: the implementations of incompatible modes may differ.

Quantities are compared on the same node sets with the same definition: the unweighted nodal mean of the displacement
along the load direction over the load region, the total reaction force, the reaction moment about the mounting
centroid, and the total strain energy. Stress is compared as the mean of the 8 integration-point σ_xx values of one
element away from supports, corners and loads: at mid-arm on the top surface of the bracket, and at mid-span on the
top surface of the cantilever.

| # | Criterion | Basis |
|---|---|---|
| X1a | C3D8 against full integration: load-region displacement, strain energy and element stress agree within 1e-6 relative | identical formulation and discrete problem; the remaining difference is the solver tolerance (NAVIER PCG 1e-10 relative residual, CalculiX's direct solver) |
| X1b | reaction force and moment about the mounting centroid: each code balances the applied load to 1e-6 of the load (moment scale: load × lever arm), and the two codes agree within 1e-6 | exact equilibrium of a converged discrete solution |
| X1c | C3D8I against incompatible modes: load-region displacement and strain energy within 2 %, element stress within 5 % | two incompatible-mode variants converge to the same limit; at these meshes NAVIER's own refinement changes are at most 3.5 % (B, 2 → 1 mm) and 0.85 % (A). A setup or assembly error would typically exceed this band |
| X1d | CalculiX reports no warning about singular or non-positive-definite systems, zero pivots or unconnected nodes | a sign of a broken exported model |

## X2: independently constructed problem

**Written specification.**
- Steel cantilever: E = 200 GPa, ν = 0.3.
- Length L = 100 mm along x, width b = 10 mm along y, height h = 10 mm along z.
- Root face x = 0 fully fixed.
- A total force of 10 N along −z, as a uniform traction over the tip face x = L.
- Outputs:
  - the mean displacement along −z of the tip face;
  - the total reaction force;
  - the strain energy.

**NAVIER side.** The case is built from that text through MCP operations: a box STL written by a separate generator,
plane selections, `fixed`, `force`, meshes at 2.5, 1.25 and 0.625 mm, and incompatible modes.

**CalculiX side.** A separate script writes the mesh and the loads from the same text:
- a structured mesh of 20-node quadratic bricks (`C3D20R`, a different element family) at 20 × 2 × 2, 40 × 4 × 4 and
  80 × 8 × 8 elements, plus `C3D8I` at 160 × 16 × 16;
- supports on every root-face node;
- consistent nodal loads of a uniform traction on 8-node quadratic faces (corner nodes −1/12 and mid-side nodes 1/3
  of each face's force), or 1/4 per corner on linear faces.

| # | Criterion | Basis |
|---|---|---|
| X2a | CalculiX's finest `C3D20R` tip-face mean displacement agrees with NAVIER's finest-mesh value within 0.5 % | NAVIER changed 0.07 % between its two finest meshes, and quadratic elements converge faster. 0.5 % leaves room for the displacement definitions (area-weighted against nodal mean on quadratic faces) while exposing any setup or unit error |
| X2b | the reaction force equals the applied 10 N within 1e-9 relative in both codes | equilibrium |
| X2c | independent `C3D8I` at 0.625 mm against NAVIER at 0.625 mm: within 2 % | as X1c, with independently built mesh and loads |

## B: the beam-reference discrepancy (hypothesis tests)

**Observation (rc1, R1).** NAVIER sits −0.717 % (C10, L/h = 10) and −0.704 % (C20, L/h = 20) below Timoshenko beam
theory at the finest mesh. The extrapolated values sit −0.67 % and −0.63 % below. The rc1 documents attributed this to
the fully clamped 3D root, which is a hypothesis.

**Competing explanations.**
- (H1) The clamped root restrains the Poisson (anticlastic) contraction across the width. A stiffened zone of length
  proportional to the width b would give Δδ/δ ≈ −3 ν² c / L with c ∝ b. C10 and C20 share b, so H1 predicts the same
  discrepancy for both, halving when L doubles, growing with b, and vanishing with ν = 0.
- (H2) Remaining discretisation error of NAVIER (observed orders 1.30 and 1.04, not demonstrated asymptotic).
- (H3) An implementation or setup error (load, supports, displacement definition).
- (H4) Beam theory's shear term: κ = 5/6 against Cowper's 10(1+ν)/(12+11ν). This changes C10 by 0.15 % and C20 by
  0.04 %; it cannot explain equal discrepancies.

**Tests.** Each is a NAVIER study at 2.5, 1.25 and 0.625 mm (1.25 and 0.625 mm for the longest beam), plus a converged
CalculiX `C3D20R` reference where stated:
- T1: ν = 0 for C10 and C20;
- T2: L = 200 mm (200 × 10 × 10);
- T3: b = 20 mm (100 × 20 × 10);
- T4: CalculiX `C3D20R` of C10 with the root "sliding": u_x = 0 on the whole root face, u_z = 0 only on the root nodes
  of the line z = h/2, and u_y = 0 only on the root nodes of the line y = b/2. The root section may then contract
  freely.

**Reading, fixed in advance.**

| Outcome | Conclusion |
|---|---|
| T1 discrepancy below 0.1 % in magnitude; T2 about half of C10's (within ±0.15 percentage points); T3 larger than C10's; the CalculiX clamped `C3D20R` within 0.2 % of NAVIER; T4 within 0.2 % of Timoshenko | H1 supported, H2 and H3 not indicated |
| the CalculiX clamped `C3D20R` differs from NAVIER by more than 0.5 % | H2 or H3 indicated: investigate NAVIER |
| none of the above patterns | cause unresolved; reported as such |

## Amendments

Appended after runs. The criteria above are unchanged; each entry states what happened and how it is reported.

1. **X1, bracket B at 1 mm with `C3D8I` (2026-09-16, 23:52).** CalculiX 2.23's direct solver (SPOOLES) exited with signal 11
   after about 42 s on this deck: 60,393 nodes, 53,280 elements, 653,196 equations with the incompatible-mode nodes. It
   did so again with a 64 MB stack limit, with `OMP_STACKSIZE` raised, with one thread, and without the all-node output
   request. The `C3D8` deck of the same mesh and the `C3D8I` deck of bracket A at 1 mm (42,240 elements) solved.
   - Added case: bracket B at 2 mm, both formulations, so that bracket B's geometry is still covered by a direct solve.
   - The pre-registered deck was then solved unchanged except for `*STATIC, SOLVER=ITERATIVE CHOLESKY`. CalculiX fixes
     that solver's stopping limit, and its reactions balance only to 2.9e-3.
   - Both the failure and the iterative result are reported. The failure is not counted as a pass.
2. **X2b (2026-09-17, 00:02).** CalculiX writes totals and nodal values to `.dat` with 7 significant digits. A balance
   of 1e-9 relative therefore cannot be read from its output; the finest readable resolution is 5e-7 of 10 N. X2b is
   reported as met by NAVIER and not evaluable for CalculiX beyond 5e-7. The printed totals equal 10 N exactly at that
   resolution. The same print resolution limits the X1 "maximum nodal difference" (5.0e-9 mm for values near 1e-2 mm)
   and the X1 CalculiX balances (about 1e-7).
3. **Supplementary cases, not used for any criterion (2026-09-17, 00:00).**
   - `s1`: clamped `C3D20R` 80 × 8 × 8 with ν = 0.
   - `s2`: a "minimal" root support in `x2_ccx.py`. It carries the whole shear reaction at one node, a point support whose
     local compliance grows with refinement, so it does not isolate the Poisson effect. It is reported but not interpreted.
4. **T4 support (observation).** The pre-registered sliding root carries the shear reaction along the root line
   z = h/2. Its tip value rose by +0.025 % and +0.016 % per mesh halving (20 × 2 × 2 → 40 × 4 × 4 → 80 × 8 × 8). The
   reading uses the finest pre-registered mesh, as fixed above.
