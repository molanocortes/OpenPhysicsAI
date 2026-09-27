# Connecting an AI client

`navier-mcp` is an MCP server on stdio. It negotiates protocol revision 2025-11-25 and also accepts 2025-06-18,
2025-03-26 and 2024-11-05. Its tools are generated from the same operation contract as the command line, so an AI
client can do nothing that `navier-ctl` cannot, and the reverse also holds.

Replace `/opt/navier-0.3.0-rc2-macos-arm64` below with the folder you extracted, and `/path/to/your/cad-exports` with
the folder that holds your STL files.

## Claude Code

A project-scoped `.mcp.json` next to where you run `claude`:

```json
{
  "mcpServers": {
    "navier": {
      "command": "/opt/navier-0.3.0-rc2-macos-arm64/bin/navier-mcp",
      "args": ["--embedded", "--allow-read", "/path/to/your/cad-exports"]
    }
  }
}
```

Or register it for your user:

```bash
claude mcp add --scope user navier -- /opt/navier-0.3.0-rc2-macos-arm64/bin/navier-mcp --embedded --allow-read /path/to/your/cad-exports
```

## Codex

In `~/.codex/config.toml`:

```toml
[mcp_servers.navier]
command = "/opt/navier-0.3.0-rc2-macos-arm64/bin/navier-mcp"
args = ["--embedded", "--allow-read", "/path/to/your/cad-exports"]
```

## Other MCP hosts

Start `navier-mcp --embedded` as a stdio server. `--embedded` keeps the engine inside that process. Without it,
`navier-mcp` connects to a running `navier-server` if one answers on `~/.navier/run/control.sock`, which lets long jobs
outlive the client.

## The sequence a client is expected to follow

1. `capabilities_get`: what is supported, including the `comparison_study` entry and its exclusions.
2. `study_check` with a definition: it returns questions, recorded assumptions, region previews, equivalence of the
   conditions, and a plan.
3. Ask the user every **blocking** question. An *acceptable* question may be accepted only with the user's reason
   (`accept: [{id, reason}]`); never invent an answer.
4. `study_run`: it returns a job id at once.
5. `job_status {job_id, wait_seconds}` until the job finishes.
6. `study_evidence {job_id}`: the outcome, the ranking on each tested mesh, each design's convergence criterion and estimate (or why there is none), the checks and the exclusions. Report the conditional
   statement, not a verdict.
7. For inspection or recovery, open a design's project (`project_open`) and use the detailed operations:
   `selection_preview`, `mesh_inspect`, `results_quantities`, `results_render`.

The workflow was designed around failures that an AI client must not paper over:

- missing units or material;
- ambiguous or unsupported mounting;
- printed parts without process information;
- designs loaded differently;
- meshes that cannot represent a feature;
- deformation too large for a linear model;
- a stress plot presented as proof of safety.

## What leaves your computer

Everything is computed locally. The MCP server sends its tool results to the AI client, which usually forwards them
to the model provider. Those results can include:

- geometry summaries (sizes, volumes, face patches, region coordinates);
- file paths and hashes;
- material values and every other input of the study definition;
- numerical results, stress summaries and the evidence record;
- PNG preview images of the geometry and its regions (from `study_check`, `selection_preview`, `view_render`,
  `results_render`).

The STL files themselves and the result fields (`results.nvr`) are not sent unless the client reads them through
another tool. If your geometry or loads are confidential, check your AI client's data policy before connecting it.

## What to check in an AI client's answer

The evaluation sessions showed these failure patterns. Check for them in any client's answer:

- **Questions answered for you.** In `study.json`, look at `resolved.designs[].geometry.units_source` (`inferred`
  means the client decided the unit), and at `resolved.accept` in `study.json` or `accepted_questions` in
  `evidence.json` (every accepted question and the reason given). A reason you did not give was written by the client.
- **Numbers without a comparison.** When the outcome is `cannot_establish`, no displacement or ranking in that study
  is a prediction, whatever the client presents.
- **Strength or safety statements.** NAVIER makes none. A client that says "safe" or "will not yield" is going beyond
  the tool, including when it used single analyses instead of a study.
- **Claims beyond the record.** "±" bands, "the true value is lower", "mesh-converged": compare them with the
  record's refinement section.

## Verification status of client integrations

| Client | Kind | Status |
|---|---|---|
| `share/navier/tests/studyflow.py` | scripted MCP client (no model) | runs the reference and evaluation cases; see `doc/REFERENCE_CASES.md` |
| Claude Code (`claude -p`) | real AI client | **evaluated, not dependable yet** (2026-09-17, model `claude-opus-4-8[1m]`, 18 graded sessions, NAVIER tools only). It ran studies correctly and passed on failed checks, but it answered questions meant for the user (units, ambiguous faces, accepted questions with its own reason), reported numbers when the comparison could not be established, and in one session called a part safe. See `release/evaluation/ai-client/RESULTS.md` in the source tree |
| Codex | real AI client | not verified: not installed on the build machine |
