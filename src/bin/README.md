# `src/bin/` — the three executables

Thin programs: parse arguments, set the access roots, create or reach an engine, and serve. All the behaviour is in
[`ctl/`](../ctl/README.md) and [`net/`](../net/README.md).

Up: [module map](../../docs/map/MODULES.md#srcbin--executables) · [AGENTS.md](../../AGENTS.md).

## The three

| File | Binary | What it is |
|---|---|---|
| `navier_mcp.c` | `navier-mcp` | MCP server on stdio (newline-delimited JSON-RPC). Either an embedded engine (`--embedded`) or a bridge to a running `navier-server`. This is what an AI client launches |
| `navier_ctl.c` | `navier-ctl` | the command line: a socket client, or `--embedded` for an engine inside the process. Also `study` and `doctor`, implemented in [`ctl/cli_local.c`](../ctl/README.md) |
| `navier_server.c` | `navier-server` | the long-running headless engine behind the control socket |

## Typical commands

```bash
./navier-ctl doctor                                   # self-diagnosis, including a reference solve
./navier-ctl --embedded call capabilities_get         # what the engine can and cannot do, machine-readable
./navier-ctl --embedded call geometry_import '{"path":"models/cube.stl","units":"mm"}'
./navier-ctl study run examples/bracket_comparison/study.json --dir /tmp/study
./navier-server &                                     # then: ./navier-ctl call job_list
./navier-mcp --embedded --allow-read /path/to/stl     # what an MCP client spawns
```

Every binary takes `--help`. Access roots are given with `--allow-read` and `--allow-write` (up to 16 of each; a
folder inside an existing root needs none).

## Talks to

- [`net/`](../net/README.md) — transports.
- [`ctl/`](../ctl/README.md) — the engine and the operations.

## Tests

`./build/ctltest`, `python3 tools/mcptest.py`, every `tools/*flow.py`, and `./navier-ctl doctor` at the end of
`make test`. Client set-up for humans: [../../docs/mcp-clients.md](../../docs/mcp-clients.md).
