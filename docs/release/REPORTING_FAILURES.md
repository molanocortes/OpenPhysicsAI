# Reporting a failure

## Collect

1. **The diagnostic:** `navier-ctl doctor --json > doctor.json`.
2. **The study folder.** Its small files are enough to understand and replay the run:
   - `request.json` and `study.json` (the definition and its resolution);
   - `operations.jsonl` (every operation, with its parameters and outcome);
   - `evidence.json` and `report.md`, when they exist;
   - `designs/<name>/project.json` and `designs/<name>/runs/*/spec.json` and `summary.json`.

   The `inputs/*.stl` copies and `results.nvr` fields are only needed if the geometry may be shared.
3. **For a job that failed:** the output of `job_status` (MCP) or of `navier-ctl study run`. Its `error.details`
   holds the failure class, stage, design, element size and recovery options.
4. **For an MCP problem:** the client's log, and the server's stderr (`navier-mcp` logs there; stdout carries only
   protocol messages).
5. **The version:** `navier-mcp --version` and the `VERSION` file.

## Describe

- What you asked for (the question and the definition).
- What you expected, and what happened: the exact error code or outcome.
- Whether it reproduces with `navier-ctl study replay <study folder>`.

## Classify

| Failure class | Typical cause | First thing to try |
|---|---|---|
| `invalid_input` | a region that does not resolve, an under-constrained mounting, a stale selection | the question or error text names the design and region; fix the definition |
| `unsupported_physics` | a request outside the model (strength, large deformation, printed anisotropy) | not a bug: the record says what is not evaluated |
| `numerical_failure` | a solver did not converge | `analysis.solver: direct`, or a coarser size; please report |
| `resource_exhaustion` | element or time budget | raise `limits`, or accept the completed levels |
| `io` | disk full, permissions | check `doctor` (writable folders) and disk space |
| internal (`INTERNAL`) | a bug | please report with everything above |

Send reports to the project maintainer. This release candidate has no public issue tracker.

## Confidential geometry

Remove the `inputs/` copies and the `results.nvr` files. The remaining JSON files still describe the geometry
(sizes, volumes, region coordinates, hashes). Say so if that is too much to share.
