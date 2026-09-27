# The graded engineering certificate — design proposal

**Every choice on this page is a proposal, not a decision.** It is written to be argued with. Nothing here is
implemented; the record it builds on is implemented and is called the Engineering Evidence Record
(`format_version: 2`, written by `src/ctl/study_evidence.c`, described in [../release/EVIDENCE.md](../release/EVIDENCE.md)).

Why it exists: the owner's end goal is that an agent runs the physics here and returns "an engineering certificate
that the part will not break in its use… graded: with little information a low-grade statement with its assumptions;
with full engineering data a high-grade one with a safety factor" ([../../VISION.md](../../VISION.md)). The anchor
application is LPBF process simulation (VISION, focus of 2026-09-17), so the certificate must be able to speak about
an **as-printed** part, not only a nominal one.

Up: [AGENTS.md](../../AGENTS.md) · map: [MODULES.md](../map/MODULES.md) · record: [EVIDENCE.md](../release/EVIDENCE.md).

## 1. What a certificate asserts, and what it never asserts

**Proposed assertion, one sentence:** *under the stated conditions, and with the inputs recorded here at the stated
provenance, the model predicts that this part meets the stated criterion with margin M, at grade G, and the numerical
evidence behind that prediction is E.*

It asserts only:

1. a **criterion** that was written down before the run (for example: peak von Mises below the material's stated
   yield divided by a safety factor; deflection at the load region below a stated limit);
2. the **margin** against that criterion, with the definition used;
3. the **grade** (§2) and the reason for it;
4. the **conditions** it holds under: geometry, material, process, loads, mounting, environment, each with provenance;
5. the **numerical evidence**: the checks that passed, the convergence statement and any independent comparison.

It never asserts:

- that the part is "safe" in general, or fit for a use not in the stated conditions;
- anything about a load case, an environment or a failure mode that was not evaluated (fatigue, impact, creep,
  corrosion, buckling, fastener pull-out — unless that mode is an evaluated criterion in its own right);
- that a measurement was made. **No prediction becomes a measurement**, and validation against measurements does not
  exist in this project today ([../release/LIMITATIONS.md](../release/LIMITATIONS.md));
- a grade above what its weakest input supports (§2);
- anything when the underlying study outcome is `cannot_establish`.

## 2. Grades: driven by inputs and by evidence, and limited by the weaker of the two

Two independent ladders. **Proposed rule: the grade is the lower of the two rungs, never an average.**

**Input completeness and provenance** (six input classes, each scored by the provenance already recorded in the
evidence record: `measured` > `user` > `database` > `inferred` > `default`):

| Rung | Material | Process | Geometry | Loads | Mounting | Environment |
|---|---|---|---|---|---|---|
| **I0** | demonstration library values | unspecified | as supplied | stated by the user | idealised | assumed room conditions |
| **I1** | datasheet grade, `database` | named process, no parameters | as supplied, checked closed | stated, with direction and distribution | idealised, with a declared alternative | stated temperature |
| **I2** | datasheet + batch identification, `user` | process parameters recorded | as supplied + tolerance statement | duty cycle or load case set | joint idealisation with a stiffness bound | operating temperature range |
| **I3** | **measured** on the batch, with test method | measured process record (machine log) | measured part (scan or metrology) | measured or standard-derived loads | measured or bounded joint stiffness | measured environment |

**Numerical evidence:**

| Rung | Requires |
|---|---|
| **N0** | one valid mesh; all per-run checks pass (equilibrium, energy, no rigid-body motion) |
| **N1** | ≥ 2 valid meshes, the convergence criterion met for the reported quantity, singular locations excluded from the criterion |
| **N2** | N1 + a defensible discretisation-error estimate under the stated conditions (as in the record today) |
| **N3** | N2 + an **independent check**: another code, another element family, or a closed-form reference for the same problem (the CalculiX comparison of [../../release/evaluation/EVALUATION_REPORT.md](../../release/evaluation/EVALUATION_REPORT.md) §2 is the pattern) |

**Proposed grade letters** (lower of the two ladders, then:)

| Grade | Rungs | What it may say |
|---|---|---|
| **D** | I0 + N0 | "indicative only": a ranking or an order of magnitude, with every assumption listed. No margin, no safety factor |
| **C** | I1 + N1 | a margin against a criterion, stated as *conditional on* the assumed inputs; no safety factor |
| **B** | I2 + N2 | a margin **with a safety factor** that the user supplied or that a named standard prescribes |
| **A** | I3 + N3 | a margin with a safety factor **and** a statement that the inputs are measured and the numerics independently checked |
| — | outcome `cannot_establish`, or any failed check | **no certificate**: the record is returned with the reason |

**Proposed hard rule:** a safety factor may be printed only at B or above, and only when its source is recorded
(user, standard, or the owner's engineering judgement with a name attached). A factor invented by the model is a
defect, not a certificate. **Grade A is unreachable today**: no measurement path exists
([../../release/evaluation/physical/PROTOCOL.md](../../release/evaluation/physical/PROTOCOL.md) is written, unused).

## 3. What must be resolved first: the five open AI-client findings

The certificate is only as trustworthy as the inputs the agent recorded. The graded sessions
([../../release/evaluation/ai-client/RESULTS.md](../../release/evaluation/ai-client/RESULTS.md)) found five holes; each
maps to a certificate rule. **Proposed resolutions:**

| # | Finding | Why it breaks a certificate | Proposed resolution |
|---|---|---|---|
| 1 | an accepted question records no author of its reason | a model-written reason can masquerade as the user's, inflating the input ladder | record `reason_source` (`user` \| `assistant`) on every accepted question; an assistant-sourced reason caps the input rung at **I1** and is printed on the certificate |
| 2 | a client can declare units "inferred" and skip the units question | a wrong unit is a factor-of-ten error that no numerical evidence catches | treat `units_source: inferred` as an acceptable blocking question: it needs an explicit accept, and it caps the grade at **C** |
| 3 | ambiguity detection is pre-empted by a narrowed query | the part may be held somewhere the user never meant | record which candidate regions matched the user's description; if more than one did and the user did not choose, cap at **C** and print both |
| 4 | numbers stay available under `cannot_establish` | the model reported a ranking the record does not support | mark every quantity of an invalid level `"valid": false` in the payload itself, and refuse certificate generation when the outcome is `cannot_establish` |
| 5 | the strength/safety scope notice exists only in the study workflow | a single analysis can be used to claim "safe" outside any record | attach the scope statement to every analysis result, and make the certificate a **separate operation** that can only read a complete record |

**Proposed operation:** `certificate_issue {study_dir | job_id, criterion, safety_factor?}` → the JSON of §5, refusing
with the reason when the rules above are not met. It must not be reachable by assembling one from raw results.

## 4. What an additive-manufacturing-aware certificate needs

A printed part is not its CAD model. For the anchor application (LPBF) and for FFF, a certificate about an
**as-printed** part needs, from the process simulation, these inputs — none of which exist in `main` today
(`fffprint.c` is arriving on `am-process`, LPBF is not started):

| Needed | Why | Where it would come from |
|---|---|---|
| **as-printed residual stress field** | a part can fail from what the process left in it, before any service load; and residual stress changes the margin under load | the print simulation's incremental thermo-elastic history, carried into the structural analysis as an initial stress state |
| **warp / distortion after release** | the geometry that is loaded is the distorted one; fit and tolerance claims depend on it | bed-release step of the print simulation; compare against the nominal geometry and report the deviation |
| **print orientation** | strength and stiffness are direction-dependent; the same part is a different part at 90° | recorded as an input class of its own (process), never inferred |
| **orthotropic elastic constants and direction-dependent strength** | an isotropic assumption is optimistic in the weak direction (interlayer for FFF, build direction and melt-pool structure for LPBF) | `ortho.c` in the mechanics layer; for LPBF, from process parameters plus published or measured data |
| **porosity / density and its provenance** | it scales both stiffness and strength | process record or measurement |
| **supports and their removal** | they change the thermal path during the build and the geometry after it | process definition |
| **post-processing** (stress relief, HIP, machining) | it can remove most of the residual stress the simulation predicts | a process step the certificate must record, since it invalidates an as-built stress claim |

**Proposed rule:** a certificate for a printed part **must** state the process, the orientation and the material
direction model. Without them the grade is capped at **D**, whatever the numerics, and the certificate says
"nominal geometry, isotropic material: not an as-printed statement".

## 5. Draft JSON shape (proposal)

```json
{
  "format": "openphysicsai-engineering-certificate",
  "format_version": 1,
  "issued": "2026-09-17T21:40:00Z",
  "grade": "C",
  "grade_reason": {
    "input_rung": "I1", "evidence_rung": "N1", "rule": "lower of the two ladders",
    "capped_by": ["units_source inferred for design B"]
  },
  "assertion": "Under the conditions recorded here, the model predicts a peak von Mises of 34.2 MPa against the stated criterion of 68.0 MPa (yield 204 MPa / safety factor 3.0, source: user): margin 1.99, conditional on the inputs below.",
  "criterion": { "quantity": "peak_von_mises_excluding_singular", "limit_mpa": 68.0,
                 "safety_factor": { "value": 3.0, "source": "user" }, "written_before_run": true },
  "result": { "value_mpa": 34.2, "margin": 1.99, "location": "arm root fillet",
              "singular_locations_excluded": ["re-entrant corner at x=8 mm"] },
  "inputs": [
    { "class": "material", "id": "al6061_t6", "provenance": "database", "reference": "handbook nominal", "rung": "I1" },
    { "class": "process",  "value": "machined", "provenance": "user", "rung": "I1" },
    { "class": "geometry", "sha256": "…", "units": "mm", "units_source": "inferred", "rung": "I1" },
    { "class": "loads", "value": "3 kg payload, -z, uniform over the pad", "provenance": "user", "rung": "I1" },
    { "class": "mounting", "value": "back face fixed", "provenance": "user", "alternative_assessed": "two bolt pads", "rung": "I1" },
    { "class": "environment", "value": "20 degC", "provenance": "default", "rung": "I0" }
  ],
  "as_printed": { "applies": false, "reason": "no process simulation: nominal geometry, isotropic material" },
  "numerical_evidence": { "outcome": "ranking_consistent_on_tested_meshes", "meshes_mm": [4, 2, 1],
                          "convergence_criterion_met": true, "discretisation_error_estimate": null,
                          "independent_check": null, "checks_passed": ["equilibrium", "energy", "no_rigid_body_motion"] },
  "never_asserts": ["fatigue", "impact", "creep", "buckling", "fasteners", "as-printed state", "measurement"],
  "open_questions_answered_by": [ { "id": "B.units", "reason": "…", "reason_source": "assistant" } ],
  "evidence_record": { "path": "study/evidence.json", "format_version": 2, "sha256": "…", "study_hash": "…" },
  "replay": { "command": "navier-ctl study replay <dir>", "software": "0.3.0-rc2",
              "contract_version": "0.6.0", "expected": "every quantity within 1e-9 relative" },
  "signature": null
}
```

`signature` is left null on purpose: who signs, and with what authority, is the owner's decision, not a design detail.

## 6. How a third party re-verifies it

Without trusting this project, and without the original machine:

1. **Check the record is the one cited.** Hash `evidence.json` and compare with `evidence_record.sha256`; compare
   `study_hash` with the one inside the record.
2. **Check the inputs are the ones certified.** The geometry hash in the record must match the STL they hold; the
   material, loads and mounting in `modeled_conditions` must match the `inputs` block above.
3. **Replay the study**: `navier-ctl study replay <dir>` re-runs from `study.json` and the stored, hashed geometry and
   compares every quantity (tolerance 1e-9 relative; bitwise on the same build). A replay that differs invalidates the
   certificate.
4. **Re-derive the grade** from the record alone, using §2. If their grade is lower, theirs wins.
5. **Check the criterion was fixed before the run** (`written_before_run`, and its presence in the study definition's
   hash, not only in the certificate).
6. **Cross-check the physics independently**, at grade A/N3: the CalculiX path in
   [../../tools/ccx_crosscheck.py](../../tools/ccx_crosscheck.py) exports the same discrete problem for another solver.
   The vision explicitly allows standing on established open-source solvers as references.

**Proposed rule:** a certificate that cannot be replayed is void. That is why the record's reproduction section, the
hashes and `study_replay` exist before the certificate does.
