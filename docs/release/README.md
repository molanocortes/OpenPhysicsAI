# NAVIER 0.3.0-rc2: headless bracket and fixture comparison

NAVIER compares two to four versions of a part (a bracket or fixture) under equivalent mounting and loading. It
predicts their deformation with a linear-elastic finite-element model, and it records the evidence behind the
comparison. An AI client drives it through the Model Context Protocol (MCP). A person can drive the same workflow
from the command line. No graphical interface is needed.

This is a **release candidate**. It has been tested on one machine by scripted clients and compared with an
independent solver (CalculiX) on reference cases. Graded sessions with a real AI client showed it is not yet
dependable when a model drives it (see `doc/AI_CLIENTS.md`). No external user has evaluated it, and nothing has been
compared with measurements (see `doc/LIMITATIONS.md`).

## What it answers, and what it does not

It answers narrow, explicit questions:

- Which design has the lower displacement (or higher stiffness) at a defined load region, under the same mounting
  and load?
- How much of that depends on Young's modulus, Poisson's ratio, the mounting idealisation or self-weight?
- Does the ranking hold on every tested mesh? Does each design meet the convergence criterion of the refinement
  study? When the conditions for an estimate hold, is the difference larger than the estimated discretisation errors?

It does **not** say whether a part is safe or strong enough. There are no strength margins, no fatigue, no fastener
or contact models, no buckling or large deformation, and no printed-material anisotropy. The results are not
validated against measurements.

## Quick start (macOS arm64)

```bash
tar -xzf navier-0.3.0-rc2-macos-arm64.tar.gz
cd navier-0.3.0-rc2-macos-arm64
bin/navier-ctl doctor
bin/navier-ctl study check share/navier/examples/bracket_comparison/study.json
bin/navier-ctl study run share/navier/examples/bracket_comparison/study.json --dir ~/NAVIER-Projects/bracket_ab
bin/navier-ctl study report ~/NAVIER-Projects/bracket_ab
```

The study writes to `~/NAVIER-Projects/bracket_ab`:

- `study.json`: the resolved specification and its hash;
- `evidence.json` and `report.md`: the Engineering Evidence Record;
- `operations.jsonl`: every operation the study executed;
- `designs/<name>/`: one ordinary project per design, with each analysis run.

To connect an AI client, see `doc/AI_CLIENTS.md`.

## Contents

| Path | What |
|---|---|
| `bin/navier-mcp` | MCP server on stdio (embedded engine, or a bridge to `navier-server`) |
| `bin/navier-ctl` | command line: `study`, `doctor`, `call`, and a client for `navier-server` |
| `bin/navier-server` | persistent engine behind a local control socket |
| `share/navier/examples/bracket_comparison` | the example: two bracket designs, their study definition, and the expected output |
| `share/navier/tests` | the scripted evaluation (`studyflow.py`, requires Python 3) |
| `share/navier/user-test-kit` | the task sheet, feedback form and diagnostics instructions for an independent user test |
| `share/navier/tools/ccx_crosscheck.py` | exports a run for an independent CalculiX check (requires NumPy and CalculiX) |
| `doc/` | installation, workflow, assumptions, numerical evidence, limitations, reference cases, tests, failure reports |
| `VERSION`, `NOTICE.md`, `SHA256SUMS` | build identity, licence and dependency status, checksums |
