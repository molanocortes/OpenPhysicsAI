# NAVIER 0.3.0-rc1: independent evaluation report, and release candidate rc2

Date: 2026-09-17. Machine: Apple M2, 8 GB, macOS 15.7.3.
- **Evaluated:** `navier-0.3.0-rc1-macos-arm64.tar.gz`, SHA-256 `14330796…`, preserved unchanged.
- **Result of the fixes:** `navier-0.3.0-rc2-macos-arm64.tar.gz`, SHA-256 `564a691f…`.

Details, raw numbers and every deviation are in `EVALUATION_RECORD.md`; criteria fixed before running are in
`CRITERIA-independent.md` and `CASES.md`.

## 1. Scientific claims of rc1 (bracket example)

| rc1 claim | Finding |
|---|---|
| B displaces 65.5 % less than A | **True at 1 mm.** B displaced less on every mesh: 60.5, 63.9, 65.5 and 66.2 % at 4, 2, 1 and 0.5 mm (a fourth mesh was run). The ranking is established on the tested meshes; the size of the difference was still moving |
| B is 27 % heavier | **True:** 27.3 % from the geometry (26.1 % from the 1 mm meshes) |
| B's finest change is 3.4 %, failing the 2 % criterion | **The failure stands; the number was imprecise.** 3.53 % relative to the finer mesh, the definition stored in the record. At 0.5 mm, B's change is −1.70 % and meets the criterion |
| "numerical uncertainty ±0.00025 mm (A), ±0.00032 mm (B)" | **Not supported.** A grid convergence index with safety factor 1.25 from three meshes, printed as "±", which reads as a bound. It was applied to B, whose staircase chamfer changes with every mesh (volume error −3.57 → −0.45 %), so Richardson extrapolation does not apply. The 0.5 mm values fell inside the rc1 bands, so the numbers were not contradicted, but they were not justified (defect D1) |
| Beam results 0.7 % below Timoshenko, attributed to the clamped root | **Supported for the Poisson part** by tests fixed in advance: ν = 0 removes it (−0.002 %); doubling the length halves it (−0.359 %); doubling the width increases it (−1.318 %); CalculiX with a root free to contract matches beam theory within 0.05 %. The rc1 mention of warping restraint is not supported |

**Equivalence of the comparison is confirmed at every mesh level:**
- the same 29.42 N force, direction and uniform distribution over the same 256 mm² pad;
- the same 2400 mm² fixed back face;
- equal lever arms (78 mm) and reaction moments (−2.1182 N·m);
- the same evaluation region and definition;
- self-weight excluded for both (at most 0.98 % as a sensitivity).

**Mesh evidence, three conclusions kept apart:**
- **Ranking on the tested meshes:** B before A on all four meshes.
- **Convergence criterion (2 %):** A meets it from 2 mm on. B fails it at 1 mm and meets it at 0.5 mm.
- **A defensible discretisation-error estimate:**
  - A: 2.4 % from three meshes (safety factor 3), 0.41 % from four meshes (orders 1.04 and 1.12, safety factor 1.25).
  - B: none, because its represented geometry changes with the mesh.

## 2. Independent numerical comparison (CalculiX 2.23, conda-forge)

- **The same assembled problem (X1)** exports NAVIER's own mesh, supports and nodal loads for:
  - cantilever C10 at 1.25 and 0.625 mm;
  - brackets A and B at 1 mm, and B at 2 mm.
- **X1 results:**
  - `C3D8` against full integration, and `C3D8I` against incompatible modes, agreed within 4e-7 in load-region
    displacement, strain energy, reactions, moments and element stress, and 5e-9 mm at every node. That is
    CalculiX's print resolution.
  - Both codes balanced the load; CalculiX printed no warning.
  - **Deviation:** CalculiX's direct solver crashed (signal 11) on the largest deck (B, 1 mm, `C3D8I`, 653,196
    equations). Its iterative solver reproduced NAVIER's displacement there to 3e-7, but balanced its own reactions only
    to 2.9e-3, failing X1b for that one run.
- **An independently built problem (X2):** CalculiX models written from a text specification of a steel cantilever.
  - The finest quadratic `C3D20R` model agreed with NAVIER's finest mesh within **0.0015 %**.
  - An independent `C3D8I` mesh at the same size agreed within 1e-8.
  - The 1e-9 reaction criterion was met by NAVIER but cannot be read from CalculiX's 7-digit output. It is reported as
    not evaluable for CalculiX and was not relaxed.
- **What this establishes:**
  - NAVIER's assembly, supports, loads, reactions, energy and stress recovery match an independent code to round-off
    on these meshes;
  - its set-up of a simple case from plain regions produces the intended discrete problem;
  - its converged cantilever values agree with a different element family.

  This is numerical evidence, **not validation**. X1 cannot detect errors in how NAVIER builds regions or loads, and
  X2 covers one box geometry.

## 3. Defects fixed in rc2 (each with a regression test; criteria changes reported with both versions)

| # | Defect | Fix |
|---|---|---|
| D1 | unsupported estimate and "±/uncertainty" wording drove the `resolved` outcome | ranking per mesh, convergence criterion and an estimate only under stated conditions; new outcome `ranking_consistent_on_tested_meshes`; the example's outcome changes from `resolved` to it, with unchanged numbers |
| D2 | 3.4 % against 3.53 %, definition not stated | definition recorded and quoted |
| D3 | at most 3 user roots; start-up failure with an unclear message | roots inside roots take no slot, limit 16, a message that names the limit |
| D4 | no storage estimate or free-space check; write failures of the record ignored | preflight storage plan and refusal; a free-space check before each analysis; `IO_ERROR` for the log, evidence and report with recovery; retention categories in the record |
| D5–D7 | unsupported wording (a mounting sensitivity called a bound, warping restraint), duplicated words | corrected |

**Tests:**
- unit and integration: 594 checks pass;
- the scripted MCP flow: 87 checks on the installed rc2 package;
- the clean-install test of the rc2 artefact: 27 checks.

The rc2 example reproduces its expected output bitwise, and its replay reproduces bitwise.

## 4. AI-client evaluation: run, and the model does not yet relay questions reliably

**Setting.**
- Run after the owner logged the command line in. Model `claude-opus-4-8[1m]` in Claude Code print mode, with only the
  NAVIER tools of the installed rc2 package.
- Six cases with three repeats: 18 primary sessions, 190 NAVIER calls, about 61 minutes, USD 29.23 reported for all 23
  sessions including a pilot and supplementary runs.
- The rubric was fixed before the first session. Details: `ai-client/RESULTS.md`.

**Results.**
- **Worked:**
  - set-up and use of the study workflow;
  - recovery after every tool error;
  - passing on failed checks the tools reported;
  - E9 ("the tip chamfer makes no meaningful difference", 3 of 3 in substance).
- **Did not:**
  - relay questions to the user: units 0 of 4, the ambiguous face 0 of 3, and acceptable questions accepted with the
    model's own reason in 6 of 18 sessions;
  - withhold numbers under `cannot_establish`: E7 led with sag values and a ranking, 3 of 3;
  - keep strength claims out: E10-r2 declared the part "safe by a very large margin" from its own stress runs, while
    E10-r1 and r3 declined.
- **Per-case reading:** E9 partly handled; E1, E2, E4, E7 and E10 not handled. Most "not handled" readings come from
  one or two statements beyond the record per answer. For E2 and E4 they come from never asking the user.
- **Deviation:** the first run shared one workspace between sessions, found during grading. E7, E9 and E10 were rerun
  isolated; E1, E2 and E4 were kept, and the reason is recorded.
- **Findings about NAVIER for the owner:**
  1. accepted questions record no author of their reason;
  2. clients can declare units "inferred";
  3. ambiguity detection can be pre-empted by a narrowed query;
  4. `cannot_establish` still exposes per-level numbers;
  5. the safety scope notice is absent from single analyses.

## 5. External usability: pending

- No participant has used NAVIER.
- The test kit is ready: package quick start, an unfamiliar two-plate task with its geometry, expected artifacts, a
  feedback form and diagnostics. The participant files ship in the package.
- The developer's own reference run of the task is not usability evidence. It did expose a concrete risk:
  - two first set-ups with loose box queries selected extra triangles (700 and 630 mm² instead of 600 mm²);
  - that changed the outcome from `resolved` (4.8 % difference) to `ranking_consistent_on_tested_meshes`;
  - the reported region areas made the error visible.

## 6. Physical comparison: validation pending

- No specimen has been made and no measurement exists.
- The protocol is ready: printed brackets A and B, a clamped mounting, hanging masses to 3 kg, indicators at the pad
  and the base, linearity and return checks, an uncertainty budget, and E_n criteria.
- **Planning predictions:** A 0.497 mm and B 0.172 mm at 3 kg with E 3.5 GPa. The primary comparison is the
  E-independent ratio 0.346, with a 3.6 % allowance.
- The measurement template and the analysis script are included; the script reports nothing until real readings exist.

## 7. Reproducibility and distribution

- The rc2 artefact:
  - builds from the sources alone in an isolated copy (sources hash `77d2f7ed…`, per-file manifest kept);
  - installs in a fresh home;
  - passes `doctor`;
  - reproduces the example bitwise.

  This was shown on one machine and one platform (macOS arm64) only.
- The binaries are not signed or notarised.
- **No licence exists.** `../DISTRIBUTION_INVENTORY.md` lists every item (no third-party code in the package) and
  eight decisions for the owner.

## 8. Recommendation

- **rc2 is suitable for internal use,** on Apple-silicon Macs, by the owner and invited testers. Its purpose is to rank
  two to four bracket or fixture designs by predicted displacement or stiffness under idealised mounting and static
  load. Its evidence record must be read as it states itself:
  - ranking on tested meshes;
  - convergence criterion;
  - estimate or no estimate;
  - modelled conditions.
- **rc2 is not ready for external release or for the claims such a release would imply:**
  - dependable AI-client operation (a real model ran it competently but answered questions meant for the user and added
    claims beyond the record; findings 1–5 in section 4 are open);
  - usability (no user);
  - agreement with reality (no measurement);
  - redistribution (no licence, unsigned).
- **It must not be used for strength, safety or printed-part decisions:** those are out of scope by design.
- **To move beyond internal use,** in this order:
  1. decide on findings 1–5 of section 4 (above all: record who supplied an accepted reason, and do not let a client
     declare consequential inputs "inferred" without a question), then rerun the six AI-client cases;
  2. run the user test with at least two participants, watching the region set-up;
  3. make the licensing and signing decisions.

  The physical ratio test is needed before any claim that predictions match real parts.
