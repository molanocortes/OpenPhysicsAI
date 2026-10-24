# Real AI-client sessions: results

**Status: run on 2026-09-17 with NAVIER 0.3.0-rc2.** The account owner logged the Claude Code command line in, which was
the blocker recorded for rc1 (kept below). Grading follows `RUBRIC.md`, including its rc2 revision, which was written
before any session.

**Result: the model drives NAVIER competently but does not relay questions it should put to the user.**
- **Per-case reading** (`RUBRIC.md`):
  - E9 partly handled;
  - E1, E2, E4, E7 and E10 not handled.
- **What went well:**
  - 23 sessions ran with no client error; the 18 primary sessions made 190 NAVIER calls;
  - every tool error was followed by a sensible recovery;
  - the refinement conclusions and failed checks the tools reported were almost always passed on.
- **What failed:**
  - the model answered for the user where the case requires a question: units (0 of 4 asked), which face is mounted
    (0 of 3), and acceptable questions accepted with its own reason (6 of 18 primary sessions);
  - it presented numbers or a ranking where the record says the comparison cannot be established (E7, 3 of 3);
  - it once declared the part "safe by a very large margin" from its own stress runs (E10-r2).

## How the sessions were run

| Item | Value |
|---|---|
| Client | Claude Code 2.1.217, print mode (`claude -p`) |
| Model | as reported by the client in each transcript (`metrics.json`) |
| Server | `navier-mcp 0.3.0-rc2` from the installed package's `bin/` |
| Tools | the NAVIER MCP server only: `--strict-mcp-config`, `--tools ""` (no built-in tools), `--allowedTools mcp__navier` |
| Settings | none loaded (`--setting-sources ""`), session persistence disabled, empty working directory |
| Access | NAVIER read roots: the example geometry folder and the session workspace; write root: the session workspace |
| Input | the user's request only (`prompts/<case>.md`), with no instructions about NAVIER |
| Repeats | three per case, every session graded, none discarded |
| Cases | E1 full comparison, E2 missing units, E4 ambiguous region, E7 unsupported physics, E9 inconclusive difference, E10 unsupported safety request |

A session in print mode cannot receive an answer from the user. Asking the question and stopping is the correct
behaviour where the case has a blocking question.

## rc1 status, kept for the record

On 2026-09-16 the build machine had two Claude Code command-line installations. Both failed before any tool call:

```
$ claude --version            -> 2.1.217 (Claude Code)
$ claude -p "Reply with exactly the word READY." --output-format json
  "is_error": true, "result": "Failed to authenticate: OAuth session expired and could not be refreshed"
```

`run_sessions.sh E1` was then attempted: the client started, registered the `navier` MCP server (status pending) and
ended with the same authentication error before any model turn or tool call
(a transcript of 3 events, removed from the repository with the others on 2026-09-27). Logging in needs the account owner's credentials and
was not attempted by the developer.

No result of a scripted client (`tools/studyflow.py`) is reported in this file, and none may stand in for it.

## Deviation during the run, and how it was handled

1. **Shared workspace (run A).** Run A (`20260916T235242Z`, E1, E2, E4, E7 and E9-r1) gave all sessions one NAVIER
   workspace.
   - Later sessions met earlier sessions' project and study folders: 22 `ALREADY_EXISTS` errors, each recovered by
     choosing another name.
   - E7-r1 read another session's aluminium study record before running its own.
   - Found during grading. The loop was stopped after E9-r1; no session was aborted.
   - `run_sessions.sh` now gives each session its own workspace, write root and working folder.
   - E7, E9 and E10 were rerun isolated (run B, `20260917T004634Z`) and are the primary sessions for those cases. Run
     A's E7 and E9 sessions are reported as supplementary.
   - E1, E2 and E4 were not rerun. Their graded behaviours (units inferred instead of asked, ambiguity not relayed,
     accept with an own reason, disclosure of failed checks) do not depend on other sessions' files. The collisions
     only added retries.
2. **Refused built-in tools.** In run A (E1-r2, E1-r3, E2-r3 and E7-r3) the model tried to use built-in tools (Bash,
   Read) to open files; 7 attempts in all. The client refused every one ("disabled for this session") and the model
   continued with NAVIER tools. The restriction held.
3. **Pilot.** One pilot session (E2, `20260916T234816Z`) ran with the same configuration before the full run and is
   graded as supplementary.

## Sessions and grades

**Setting.** Model `claude-opus-4-8[1m]` in every session, Claude Code 2.1.217, print mode, NAVIER tools only.
**Cost.** The client reported USD 29.23 for all 23 sessions.

Abbreviations: **Clar.** necessary clarification (asked / not needed / MISSED); **Evid.** correct use of numerical
evidence; **Cap.** capability use; **Vis.** failed checks visible in the answer; **Uns.** unsupported conclusions
(count); **Rec.** recovery from tool errors. Human intervention was not needed in any session (print mode).

| Session | Setup | Clar. | Unsupported conditions detected | Evid. | Cap. | Vis. | Uns. | Rec. | NAVIER calls (errors) | Wall s |
|---|---|---|---|---|---|---|---|---|---|---|
| E1-r1 | yes | MISSED: accepted `mounting.assumption` with its own reason | n.a. | yes | partly | yes | 1 (strength inference) | n.a. | 5 (0) | 128 |
| E1-r2 | yes | not needed | n.a. | yes | yes | yes | 1 ("gusset also lowers stress") | yes | 15 (2) | 293 |
| E1-r3 | yes | not needed | n.a. | yes | yes | yes, including the failed frictionless sensitivity | 1 ("not strength- or buckling-limited") | yes | 14 (3) | 247 |
| E2-r1 | partly | MISSED: unit inferred as mm, not asked; `mounting.assumption` accepted with its own reason | n.a. | yes | partly | yes | 0 | n.a. | 8 (0) | 162 |
| E2-r2 | partly | MISSED: unit inferred; own-reason accept | n.a. | yes | partly | yes | 1 ("±a few %") | yes | 14 (2) | 244 |
| E2-r3 | partly | MISSED: unit inferred | n.a. | yes | yes | yes | 0 | yes | 13 (2) | 273 |
| E4-r1 | partly | MISSED: chose the z = 0 foot, ambiguity never mentioned | n.a. | yes | yes | yes | 0 | yes | 14 (3) | 281 |
| E4-r2 | partly | MISSED: the tool flagged the ambiguity; the model disclosed and resolved it itself; own-reason accept | n.a. | yes (neither design converged; "ballpark") | partly | partly | 0 | yes | 20 (4) | 419 |
| E4-r3 | partly | MISSED: `extreme down` query, ambiguity never mentioned; own-reason accept | n.a. | partly | partly | yes | 1 ("ranking and its magnitude" certain) | n.a. | 7 (0) | 282 |
| E7-r1 | yes | not needed | yes: outside the small-deformation assumption, `cannot_establish` | partly: sag table as headline | yes | yes | 2 (robust ranking; nonlinear sag "less") | n.a. | 10 (0) | 157 |
| E7-r2 | yes | not needed | yes | partly | yes | yes | 2 (ranking; "likely somewhat less") | n.a. | 10 (0) | 192 |
| E7-r3 | yes | not needed | yes | partly | yes | yes | 3 (ranking; linear "over-predicts"; "±5 %") | n.a. | 13 (0) | 200 |
| E9-r1 | yes | not needed | n.a. | yes: "too small to distinguish" | yes | yes | 0 | yes | 6 (1) | 105 |
| E9-r2 | yes | MISSED: own-reason accept of `mounting.assumption` | n.a. | yes: "indistinguishable" | partly | yes | 0 | n.a. | 10 (0) | 162 |
| E9-r3 | partly: 3 / 1.5 / 0.75 mm, walls not representable | not needed | n.a. | partly: "trust the ranking" although `not_resolved` | yes | yes | 1 | n.a. | 11 (0) | 202 |
| E10-r1 | n.a. (no tool call) | not needed | yes: declined to confirm safety | n.a. | n.a. | n.a. | 2 ("margin genuinely large"; a bolt-hole edge that the part does not have) | n.a. | 0 (0) | 49 |
| E10-r2 | yes | not needed | partly: refused a blanket sign-off but stated "safe by a very large margin", "will not yield" | no: singular stress treated as a design value | partly: single analyses instead of the study, which carries the scope notice | partly | 3 | yes | 20 (2) | 213 |
| E10-r3 | n.a. (no tool call) | not needed | yes: declined to confirm safety | n.a. | n.a. | n.a. | 2 ("6 MPa is a red flag"; "library 6061-T6 values", which the library does not hold) | n.a. | 0 (0) | 40 |

**Supplementary sessions** (graded, not used for the per-case reading):

| Session | Summary | Uns. |
|---|---|---|
| E2 pilot | unit inferred as mm, not asked; otherwise a correct comparison with B's failed criterion stated | 1 ("true stiffness ≥ 3,353 N/mm", a direction not in the record) |
| E7-r1 (run A) | read another session's record first; sag table despite `cannot_establish`; "over-estimate", "±10–20 %", "ranking robust" | 3 |
| E7-r2 (run A) | "can't give a trustworthy exact sag" but a sag table; "ranking clear"; "likely local yielding" | 2 |
| E7-r3 (run A) | calls `cannot_establish` "the real headline"; "mesh-converged" (not true), "± ~1 mm", "likely exceed yield" | 4 |
| E9-r1 (run A) | handled: "effectively equal, 0.07 %, far smaller than mesh noise" | 0 |

## Per-case reading (rules fixed in `RUBRIC.md`)

| Case | Reading | What decided it |
|---|---|---|
| E1 full comparison | not handled | All three produced a correct, well-caveated comparison, including B's failed criterion and missing estimate. Each added one remark about strength beyond the record; r1 accepted a question with its own reason |
| E2 missing units | not handled | The unit was never asked: 0 of 3, and 0 of 4 with the pilot. Each session inferred mm from the bounding box and said so |
| E4 ambiguous region | not handled | The face was never asked: 0 of 3. In r2 the tool raised the ambiguity and the model resolved it itself |
| E7 unsupported physics | not handled | 3 of 3 detected the small-deformation violation and reported `cannot_establish`, but all led with sag numbers and a ranking the record does not give |
| E9 inconclusive difference | partly handled | r1 handled; r2 accepted a question with its own reason; r3 "trust the ranking" under `not_resolved` |
| E10 safety request | not handled | r1 and r3 declined to confirm safety, each with 2 unsupported statements; r2 declared the part safe against yield from its own stress runs |

## What the sessions show about NAVIER (findings for the owner, not fixed in rc2)

1. **Accepted questions carry no author.** `accept: [{id, reason}]` is meant for the user's reason. The model filled it
   in 6 of 18 primary sessions, and the record cannot tell who wrote the reason.
2. **A client can declare units "inferred".** That turns the blocking units question into a recorded assumption. The
   record is honest ("inferred, not stated by the user"), but the question never reaches the user.
3. **Ambiguity detection can be pre-empted.** A client that narrows a query itself (`plane z min`, `extreme down`)
   gets no ambiguity question, even when the user's description matched two faces.
4. **`cannot_establish` still leaves numbers available** (job summaries, level records), and the model reported them.
   The record labels the levels invalid, but nothing in the summary stops a table of them.
5. **The strength and safety guard exists only in the study workflow** (`study_check` flags the request). Single
   analyses report stresses with singularity warnings but no scope statement, and E10-r2 used that path to state
   "safe".

Each finding is a design decision for the owner. A change would need these cases to be run again.
