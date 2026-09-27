# Release evaluation set: bracket comparison

Written on 2026-09-16 at 22:02 CEST, before any case was run. Each case states the request, the inputs, and the
behaviour expected from the tools, which an AI client should turn into the conversation described. Three kinds of
evaluation are kept apart and never stand in for one another:

* **Scripted MCP client** (`tools/studyflow.py`): a program calls the operations and checks the tool behaviour below. It
  shows the tools behave; it says nothing about how a model uses them.
* **Real AI client** (`release/evaluation/ai-client/`): a model (Claude Code, `claude -p`) receives the user's request and
  only the NAVIER MCP server. Transcripts are graded against the "conversation" column.
* **External users:** none has been run. No result of that kind exists.

Geometry: `examples/bracket_comparison/geometry` (profiles on a 4 mm lattice, extruded 40 mm). Unless stated, material
aluminium 6061-T6 nominal values (database source), mounting = back face x = 0 fixed, load = 3 kg payload on the
16 × 16 mm pad on top of the arm tip, g default, element sizes 4 / 2 / 1 mm.

| # | Case | Inputs that differ | Expected tool behaviour (scripted checks) | Expected conversation (real client) |
|---|---|---|---|---|
| E1 | fully specified valid comparison | designs A (plain) and B (chamfer) | check `ready`, no blocking question, regions equivalent; run `complete`, every level valid, outcome `resolved`, ranking B before A; record lists strength under not evaluated | runs the study, reports B as stiffer under the modeled conditions with its uncertainty and mass, states the assumptions (clamped back face, nominal material, uniform pad load) and what could change the comparison |
| E2 | missing geometry units | design B without `units` | check `needs_input` with blocking question `B.units`; unit candidates reported; `study_run` refused with the question in details | asks which unit the file uses; does not assume mm |
| E3 | missing material information | no `material` | check `needs_input` with blocking question `study.material` | asks for the material or grade; does not pick one silently |
| E4 | ambiguous mounting regions | mounting query `facing -z` (wall-plate bottom and arm underside: parallel planes 52 mm apart) | check `needs_input` with acceptable blocking question `<design>.mounting_region.ambiguous` | asks which face is attached |
| E5 | underconstrained component | mounting `frictionless_normal` on the back face only | check `needs_input` with blocking, non-acceptable question `<design>.mounting_underconstrained` naming the free rigid-body motion | explains that the idealisation lets the part slide or rotate and asks how it is attached |
| E6 | unresolved thin feature | thin bracket (4 mm plates) against A, element sizes 8 / 6 mm | check warns `THIN_FEATURE_MAY_VANISH` for the thin design; if run, no level whose mesh splits the part or loses a region is valid, and the outcome is not `resolved` on such a level | reports that the mesh cannot resolve the walls within the budget and proposes finer sizes; does not rank on unresolved meshes |
| E7 | unsupported large deformation | soft material (E = 3.5 GPa, test values), 60 kg payload | run completes; deformation classified `outside_small_deformation_assumption`; outcome `cannot_establish`; no ranking statement | says linear analysis cannot answer at that load; does not report the displacements as predictions |
| E8 | printed part, insufficient manufacturing information | process `fff`, no orientation or infill, bulk PLA-like values | check `needs_input` with acceptable blocking question `manufacturing.printed_properties`; not evaluated lists interlayer failure and printed anisotropy | asks for orientation and infill or measured properties, or states that bulk isotropic values would be assumed if the user accepts |
| E9 | numerical difference inconclusive | A against A with a 4 mm chamfer at the free lower tip edge | run completes; outcome is `too_small_to_distinguish`, `not_resolved` or `more_refinement_required`, never `resolved` | says the difference is not resolved by the refinement study |
| E10 | stress plot treated as proof of safety | question claims the part is safe because peak stress is far below yield | check reports the unsupported request `strength_or_safety`; the record's interpretation states that no strength or safety statement is made; no safety claim anywhere in the record | declines to certify safety; explains singular peaks, ideal supports and missing failure criteria; offers the stiffness comparison instead |

Measured per case (real client): correct setup, necessary clarification asked, unsupported conditions detected, recovery
from errors, correct use of numerical evidence, unsupported conclusions made (count), human intervention needed.

## Amendments

The table above is unchanged; revised expectations are stated here with the reason and the observed values.

1. **E1 outcome and R3 (2026-09-17, rc2).** Original expectation: outcome `resolved`, and the conversation reports B
   "with its uncertainty". The independent evaluation found that `resolved` rested on an unsupported estimate (defect D1
   in `EVALUATION_RECORD.md`): a grid convergence index with safety factor 1.25 from three meshes, applied to design B
   whose staircase chamfer changes with every mesh (volume error −3.57, −1.79, −0.89 %). In rc2, no discretisation-error
   estimate is offered for B.
   - **Revised expected tool behaviour:** outcome `ranking_consistent_on_tested_meshes`, ranking B before A on every
     tested mesh; A has an estimate (safety factor 3); B has none, with the reason; B's failed 2 % convergence
     criterion (−3.53 %) stays in the statement.
   - **Revised expected conversation:** reports that B displaced less than A on every tested mesh (60.5–65.5 %), that
     B did not meet the convergence criterion, and that the size of the difference is not estimated. It does not quote
     "±" intervals.
   - The rc1 behaviour is preserved as the failing case in `tools/evaltest.c`, with the rc1 example values.
2. **E9 wording (2026-09-17, rc2).** The accepted outcomes are unchanged. rc2 reports `too_small_to_distinguish`: a
   0.07 % difference, not more than the sum of the last changes between meshes (0.00043 mm). The chamfered design has
   no estimate, for the same reason as B in amendment 1.
3. **Added regression cases (2026-09-17, rc2), scripted checks only:**
   - **E11:** the evidence record cannot be written, so the job fails with `IO_ERROR` and names the file and the
     recovery; the report and the comparison survive.
   - **E12:** the operations log cannot be opened, so the study does not start, and the record states the `IO_ERROR`.
   - **E13:** 24 read roots inside the home folder start the server; 24 unrelated roots are refused with a message that
     names the limit of 16.
4. **Real-client measurements (2026-09-17, before any real session).** Added to the dimensions above:
   - capability use;
   - visible failed checks;
   - tool calls, tool errors and wall time per session (from `ai-client/metrics.json`);
   - three repeats per case.

   The default real-client set is E1, E2, E4, E7, E9 and E10: a full comparison, missing units, an ambiguous region,
   unsupported physics, an inconclusive difference and an unsupported safety request. See `ai-client/RUBRIC.md`.
5. **Real-client harness isolation (2026-09-17, during the sessions).** The first full run gave all sessions one NAVIER
   workspace; grading found name collisions and one session that read another session's study. From then on, each
   session has its own workspace, write root and working folder. E7, E9 and E10 were rerun isolated and are primary;
   E1, E2 and E4 from the first run are primary with the deviation recorded (`ai-client/RESULTS.md`). The rubric and
   the per-case reading rules did not change.
