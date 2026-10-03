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

## ChatGPT plugin development (2026-10-03)

The native C MCP tools also run through a local Streamable HTTP adapter. It is Python standard-library transport
and process glue; the physics, project validation, asynchronous jobs and result queries remain in the same C engine.
The native viewer remains the result inspector. No model API key or external inference service is needed by the adapter.

```bash
make am
./navier --listen
# Or run ./navier-server when the viewer is not needed.
python3 tools/mcp_http.py --port 8787
```

Endpoint: `http://127.0.0.1:8787/mcp`. The adapter defaults to `navier-mcp --connect`: all sessions use the running
native engine, and jobs survive an HTTP-session DELETE or disconnect. Start a persistent engine first. For an isolated
protocol check only: `python3 tools/mcp_http.py --port 8787 -- --embedded --workspace /tmp/navier-http`.
Embedded sessions each own an engine and stopping that session cancels its jobs; use connect mode for real work.

The adapter supports POST JSON responses, MCP session IDs, version checks and DELETE. GET returns 405 because this
implementation does not offer an optional SSE stream. There are at most eight native backend processes (configurable
1-32), and input messages are limited to 1 MiB. It binds loopback only and rejects foreign Host and Origin headers.
Close idle sessions with DELETE to recover capacity. It does not serve result files or arbitrary filesystem URLs.

Tool discovery now includes explicit read-only, destructive, idempotent and open-world annotations for every tool,
and output schemas for the structured result envelope. The capabilities result names the available FDM/FFF and
inherent-strain LPBF operations and their exclusions. It must not be read as a claim that all lab solvers are typed
MCP operations: lab scenarios still use `build/labrun` and the app's library.

Verification: `python3 tools/mcp_http_test.py` uses an independent HTTP client to check the native engine, lifecycle,
state retention, embedded-session isolation, bounds and rejected origins; `python3 tools/mcptest.py` checks stdio.
These are protocol checks, not a model-driven ChatGPT session or a published plugin.

The [official MCP server guide](https://developers.openai.com/plugins/build/mcp-server) specifies Streamable HTTP,
accurate tool annotations and a stable authenticated HTTPS deployment for public submission. This adapter is the
single-user local development building block. Public hosting, identity/authorization, multi-user workspace isolation,
privacy information and an actual ChatGPT developer-mode test remain required before publication. Do not expose this
local adapter through a public proxy as a production server.
