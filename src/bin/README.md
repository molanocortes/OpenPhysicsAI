# `src/bin/` — the three executables

Thin programs: parse arguments, set the access roots, create or reach an engine, and serve. All the behaviour is in
[`ctl/`](../ctl/README.md) and [`net/`](../net/README.md).

Up: [module map](../../docs/map/MODULES.md#srcbin--executables) · [AGENTS.md](../../AGENTS.md).

## The three

| File | Binary | What it is |
|---|---|---|
| `openphysics_mcp.c` | `openphysics-mcp` | MCP server on stdio (newline-delimited JSON-RPC). Either an embedded engine (`--embedded`) or a bridge to a running `openphysics-server`. This is what an AI client launches |
| `openphysics_ctl.c` | `openphysics-ctl` | the command line: a socket client, or `--embedded` for an engine inside the process. Also `study` and `doctor`, implemented in [`ctl/cli_local.c`](../ctl/README.md) |
| `openphysics_server.c` | `openphysics-server` | the long-running headless engine behind the control socket |

## Typical commands

```bash
./openphysics-ctl doctor                                   # self-diagnosis, including a reference solve
./openphysics-ctl --embedded call capabilities_get         # what the engine can and cannot do, machine-readable
./openphysics-ctl --embedded call geometry_import '{"path":"models/cube.stl","units":"mm"}'
./openphysics-ctl study run examples/bracket_comparison/study.json --dir /tmp/study
./openphysics-server &                                     # then: ./openphysics-ctl call job_list
./openphysics-mcp --embedded --allow-read /path/to/stl     # what an MCP client spawns
```

Every binary takes `--help`. Access roots are given with `--allow-read` and `--allow-write` (up to 16 of each; a
folder inside an existing root needs none).

## Talks to

- [`net/`](../net/README.md) — transports.
- [`ctl/`](../ctl/README.md) — the engine and the operations.

## Tests

`./build/ctltest`, `python3 tools/mcptest.py`, every `tools/*flow.py`, and `./openphysics-ctl doctor` at the end of
`make test`. Client set-up for humans: [../../docs/mcp-clients.md](../../docs/mcp-clients.md).
