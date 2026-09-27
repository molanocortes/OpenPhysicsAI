# Connecting AI clients to NAVIER-AM

`/path/to/navier` is the repository root when built from source, or `<release>/bin` for an installed release. Projects
are written below `~/NAVIER-Projects` by default (`--workspace DIR` changes it). The release documentation for the
bracket comparison workflow, including the data that tool results send to an AI client, is in `docs/release/`.

`navier-mcp` is a Model Context Protocol server on stdio. It negotiates MCP revision 2025-11-25 and also accepts
2025-06-18, 2025-03-26 and 2024-11-05. Tools are generated from `src/ctl/ops_schema.json`, the same contract the
terminal (`am ...`), the control socket and the UI use.

## Build

```bash
make am
```

This produces `navier-mcp`, `navier-server` and `navier-ctl` in the repository root.

## Where the engine runs

| Mode | How | Use when |
|---|---|---|
| Embedded | `navier-mcp --embedded` | a single AI client; state lives as long as the client keeps the server process |
| Persistent server | start `navier-server`, then `navier-mcp --connect` | long simulations should survive client restarts; several clients share one project |
| Live GUI | start `./navier --listen`, then `navier-mcp --connect` | watch the AI's setup in the viewer |

Without `--embedded` or `--connect`, `navier-mcp` connects to `~/.navier/run/control.sock` if a server answers
there and otherwise runs embedded. It logs its choice to stderr.

## File access

The server reads input files only below its read roots and writes projects and exports only below its write roots
(`capabilities_get` lists them). Defaults: read `$HOME`, `/tmp`, `/Volumes` and the working directory; write
`~/NAVIER-Projects` and `$TMPDIR`. Add roots with `--allow-read DIR` and `--allow-write DIR`, and change the
default project folder with `--workspace DIR`.

## Claude Code

Project scope, in a `.mcp.json` next to where you run `claude`:

```json
{
  "mcpServers": {
    "navier-am": {
      "command": "/path/to/navier/navier-mcp",
      "args": ["--allow-read", "/path/to/your/cad-exports"]
    }
  }
}
```

Or register it once for your user:

```bash
claude mcp add --scope user navier-am -- /path/to/navier/navier-mcp --allow-read /path/to/your/cad-exports
```

A one-off session that loads only this server, without changing any settings (this is how the integration check
below was run):

```bash
claude -p "Call capabilities_get and summarise the model limitations" --mcp-config mcp.json --strict-mcp-config --allowedTools mcp__navier-am
```

## Codex

Add to `~/.codex/config.toml`:

```toml
[mcp_servers.navier-am]
command = "/path/to/navier/navier-mcp"
args = ["--allow-read", "/path/to/your/cad-exports"]
```

## Verification status

| Check | Kind | Result (2026-09-15) |
|---|---|---|
| `python3 tools/mcptest.py` | protocol test with an independent stdlib client | 194 passed, 0 failed: lifecycle, version negotiation, tool discovery, protocol errors, tool execution errors, idempotent retry, revision conflict, stdout purity, clean shutdown, bridge mode surviving client exit |
| `python3 tools/amflow.py` | complete analysis path over MCP stdio with a scripted client (no model) | 71 passed, 0 failed: STL import, surface discovery, selections, view images as MCP image content, material, mesh, supports and loads, validation, analysis job, result query and image, VTU/CSV/summary export with an independent VTU check, job surviving a client disconnect, idempotent retry without a duplicate run |
| Claude Code 2.1.217, `claude -p --mcp-config` | real client | the client started `navier-mcp`, completed initialization and discovered all 8 tools of that build (`status: connected`; the server now has 32). The model turn then failed with "OAuth session expired and could not be refreshed", so no tool was called by the model. Re-run after `claude` has been logged in again. |
| Codex | real client | not verified: no `codex` command-line client is installed on this machine; the configuration above follows the format of the existing `~/.codex/config.toml` |

Until a model-driven session completes, treat the Claude Code and Codex integrations as protocol-verified only.
