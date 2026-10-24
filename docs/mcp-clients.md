# Connecting AI clients to OpenPhysicsAI

`/path/to/OpenPhysicsAI` is the repository root when built from source, or `<release>/bin` for an installed release. Projects
are written below `~/OpenPhysicsAI-Projects` by default (`--workspace DIR` changes it). The release documentation for the
bracket comparison workflow, including the data that tool results send to an AI client, is in `docs/release/`.

`openphysics-mcp` is a Model Context Protocol server on stdio. It negotiates MCP revision 2025-11-25 and also accepts
2025-06-18, 2025-03-26 and 2024-11-05. Tools are generated from `src/ctl/ops_schema.json`, the same contract the
terminal (`am ...`), the control socket and the UI use.

## Build

```bash
make am
```

This produces `openphysics-mcp`, `openphysics-server` and `openphysics-ctl` in the repository root.

## Where the engine runs

| Mode | How | Use when |
|---|---|---|
| Embedded | `openphysics-mcp --embedded` | a single AI client; state lives as long as the client keeps the server process |
| Persistent server | start `openphysics-server`, then `openphysics-mcp --connect` | long simulations should survive client restarts; several clients share one project |
| Live GUI | start `./openphysics --listen`, then `openphysics-mcp --connect` | watch the AI's setup in the viewer |

Without `--embedded` or `--connect`, `openphysics-mcp` connects to `~/.openphysicsai/run/control.sock` if a server answers
there and otherwise runs embedded. It logs its choice to stderr.

## File access

The server reads input files only below its read roots and writes projects and exports only below its write roots
(`capabilities_get` lists them). Defaults: read `$HOME`, `/tmp`, `/Volumes` and the working directory; write
`~/OpenPhysicsAI-Projects` and `$TMPDIR`. Add roots with `--allow-read DIR` and `--allow-write DIR`, and change the
default project folder with `--workspace DIR`.

## Claude Code

Project scope, in a `.mcp.json` next to where you run `claude`:

```json
{
  "mcpServers": {
    "openphysics": {
      "command": "/path/to/OpenPhysicsAI/openphysics-mcp",
      "args": ["--allow-read", "/path/to/your/cad-exports"]
    }
  }
}
```

Or register it once for your user:

```bash
claude mcp add --scope user openphysics -- /path/to/OpenPhysicsAI/openphysics-mcp --allow-read /path/to/your/cad-exports
```

A one-off session that loads only this server, without changing any settings (this is how the integration check
below was run):

```bash
claude -p "Call capabilities_get and summarise the model limitations" --mcp-config mcp.json --strict-mcp-config --allowedTools mcp__openphysics
```

## Codex

Add to `~/.codex/config.toml`:

```toml
[mcp_servers.openphysics]
command = "/path/to/OpenPhysicsAI/openphysics-mcp"
args = ["--allow-read", "/path/to/your/cad-exports"]
```

## Claude Desktop

In `claude_desktop_config.json` (Settings, Developer, Edit Config; on macOS the file is in
`~/Library/Application Support/Claude/`), then restart Claude Desktop:

```json
{
  "mcpServers": {
    "openphysics": {
      "command": "/path/to/OpenPhysicsAI/openphysics-mcp",
      "args": ["--lab", "/path/to/OpenPhysicsAI", "--allow-read", "/path/to/your/cad-exports"]
    }
  }
}
```

## Any other client

Any MCP host that starts a server on stdio works: the command is the absolute path of `openphysics-mcp`, the arguments
are the options of this page (`--lab REPO` for the physics lab, `--allow-read DIR` for your files, `--embedded` or
`--connect`). The server writes only protocol messages to standard output and logs to standard error. Check it from a
shell with `./openphysics-ctl doctor`, whose `mcp_stdio_transport` line runs the same `initialize` and `tools/list` a
client does. Before the rename of 2026-10-21 the server was `navier-mcp`; that name still works as a link for one
release.

## The physics lab and permission prompts (the app's Ask AI, 2026-10-04)

`openphysics-mcp --lab REPO --lab-results DIR` adds four tools that the engine does not serve: `lab_scenarios`,
`lab_scenario_get` (a scenario with the documentation of its solver's keys from `docs/lab/<domain>.md`), `lab_run`
(saves the scenario under a new name in `DIR/agent-runs/`, runs `REPO/build/labrun` in its own session into `DIR/NAME.lab`,
answers with labrun's JSON record when it ends or after `wait_seconds`, at most 600) and `lab_run_status`. An example's own
name is refused, so the gallery's stored results stay. With these, making or changing a simulation needs no shell and no
file permission in the AI tool.

`lab_change` (2026-10-08, for a spoken change, [docs/xr.md](xr.md#talking-with-the-ai)) changes a simulation in one
call: `{"base": "flag_3d", "set": {"stream.speed_m_s": 12}}` takes the scenario of the result `DIR/flag_3d.lab` from
its own header (what was computed and is shown), else `DIR/agent-runs/flag_3d.json`, else the example, changes only the
values named by their path (`bodies.0.radius_m`, `run.end_s`), saves it as `flag_3d_v2` (then `_v3`...; or `name`) and
runs it as `lab_run` does, answering after `wait_seconds` (default 2). A path that is not in the scenario is refused with
the paths that are, as is a value of another kind (a number for a text, a list of another length, a group of keys
replaced whole), `domain` and a `source`: the solvers ignore keys they do not know, so a misspelt key would otherwise
change nothing. The title notes the change unless `title` is set, the `source` beside a changed value records the old
and new value, and `changed_from` keeps the base's name and the set. Tested in `tools/xr/change_test.py`.

With `--lab` it also offers four tools for the headset (2026-10-07, [docs/xr.md](xr.md)), which act while the headset
link runs and say so when it does not: `headset_status` (what the person sees in the Meta Quest: the result, its field,
frame and cut, whether the page is connected, the points the person pinned there with their parts, places and values,
and the marks), `headset_look` (a picture of what the person sees, returned as an MCP image: the result, the card, the
pointers, pins and marks from between their eyes, never the room; a new one, or the one taken as they asked their last
question), `headset_view` (view commands for the app and the headset together: playback, a frame, a stored time, a
field, a cut; each checked by `xrlink_command_allowed` in `src/core/xrlink.c`, the same rule the app applies, and
withdrawn if the app does not take them within 4 s) and `headset_mark` (labelled points on the result, at places in its
own coordinates: a part's build frame in mm, a lab result's axes in m). Verified by calling `openphysics-mcp` directly in
`make test-xr` against a headless app; not yet in a session with a real model.

`cad_part` and `cad_lattice` (2026-10-08, with `--lab`; [docs/cad.md](cad.md), GOALS.md G23) make printed-part
geometry: `cad_part` an exact part from a list of features in millimetres (primitives, sketches extruded and revolved,
booleans, patterns, fillets, chamfers, shells; edges and faces chosen by geometric selectors, never by index), written
as STEP and as a closed STL with every feature's measures and the part's named faces; `cad_lattice` the part's skin
and lattice core (gyroid, Schwarz P or diamond sheets and networks, body-centred cubic or octet struts, or hollow) at a
wall or a solid fraction, one level set that is always closed and manifold, with the fraction it reached measured.
`cad_lattice` counts the enclosed voids a closed skin leaves and, with `drains`, drills them open
([docs/cad.md](cad.md#a-printed-parts-skin-and-lattice-core)). `cad_check`
(2026-10-08) says whether a part can be printed as it is, for a process named by its ISO/ASTM 52900 category and the
overhang angle and thinnest wall the caller gives (required, never guessed): the overhangs and the support under them
along the build direction and the six axes, walls thinner than the minimum, and enclosed voids with their shortest way
out ([docs/cad.md](cad.md#printability)). `cad_cell` (2026-10-08) homogenises one periodic cell of a core and
answers with its stiffness over the solid's ([docs/cad.md](cad.md#a-cells-stiffness)): an infinite lattice's, which
printed cubes of a few cells measured well below. All four run `build/openphysics-cad`, a program of its own (Open CASCADE and
Manifold are never loaded into this server), and write under `DIR/cad/NAME/`; `geometry_import` with the STL and units
mm then brings the part into a project for a mesh and an analysis.

`--lean` (with `--lab`), for a spoken conversation (2026-10-07, [docs/xr.md](xr.md#talking-with-the-ai)): instead of
listing the engine's operations one by one (83 tools, about 81 000 tokens of definitions that every question carries),
it offers `engine_ops` (every operation's name, kind and what it does in a line, narrowed by `match`), `engine_op` (one
operation's description and input schema) and `engine_call` (runs one with its input; its result, errors and images
are the operation's own), beside the lab, headset and geometry tools: 16 tools in about 5 400 tokens. The app's conversation
sessions use it.

`--approvals DIR` adds `approve`, for Claude Code's `--permission-prompt-tool mcp__openphysics__approve`: it writes
`DIR/ID.req` (`{"tool_name", "input"}`), waits up to 10 minutes for `DIR/ID.ans` (`allow` or `deny`) and answers in
the form Claude Code expects (`{"behavior":"allow","updatedInput":...}` or `{"behavior":"deny","message":...}`).
It also adds `setup_confirm` (GOALS.md G17 step 6): before an analysis of a part the person described in their own
words, the agent applies the supports and loads it made of them and calls it with `{"summary"}`, the setup in plain
words with units. The window colours the held faces violet and the loaded ones gold-brown, shows the summary on a card
("Is this how the part is held and loaded?", **Run it**, **Not right**) and returns `{"confirmed", "message"}`; the
agent runs only when confirmed. A question asked in the window also names the faces the person clicked on ("surface
patches 2, 3, 1545 mm2 in all"), which `selection_create` with `{"patches": [...]}` turns into a selection.

The app's Ask AI starts every question with both, and with the permission mode chosen in its model menu, in each tool's
own terms: Claude Code `--permission-mode default` (Ask first: its prompts become Allow / Deny cards in the window),
`acceptEdits`, `auto`, or `--dangerously-skip-permissions`, plus `--add-dir` for the lab's examples, docs and results;
Codex `--sandbox read-only`, `--full-auto` (the default) or `--dangerously-bypass-approvals-and-sandbox`, always with
`--skip-git-repo-check` and `tool_timeout_sec=900` for this server (Claude Code gets `MCP_TOOL_TIMEOUT=900000`). The
agent works in the open project's folder, otherwise in the lab's results folder. Verified with stand-in programs in
place of the AI tools (the flags each receives, the request and answer files, the cards) and by calling `openphysics-mcp`
directly (an orbit scenario run through `lab_run` in 1.8 s with its record); not yet in a session with a real model.

## Verification status

| Check | Kind | Result (2026-09-15) |
|---|---|---|
| `python3 tools/mcptest.py` | protocol test with an independent stdlib client | 194 passed, 0 failed: lifecycle, version negotiation, tool discovery, protocol errors, tool execution errors, idempotent retry, revision conflict, stdout purity, clean shutdown, bridge mode surviving client exit |
| `python3 tools/amflow.py` | complete analysis path over MCP stdio with a scripted client (no model) | 71 passed, 0 failed: STL import, surface discovery, selections, view images as MCP image content, material, mesh, supports and loads, validation, analysis job, result query and image, VTU/CSV/summary export with an independent VTU check, job surviving a client disconnect, idempotent retry without a duplicate run |
| Claude Code 2.1.217, `claude -p --mcp-config` | real client | the client started `openphysics-mcp`, completed initialization and discovered all 8 tools of that build (`status: connected`; the server now has 32). The model turn then failed with "OAuth session expired and could not be refreshed", so no tool was called by the model. Re-run after `claude` has been logged in again. |
| Codex | real client | not verified: no `codex` command-line client is installed on this machine; the configuration above follows the format of the existing `~/.codex/config.toml` |

Until a model-driven session completes, treat the Claude Code and Codex integrations as protocol-verified only.

## ChatGPT plugin development (2026-10-03)

The native C MCP tools also run through a local Streamable HTTP adapter. It is Python standard-library transport
and process glue; the physics, project validation, asynchronous jobs and result queries remain in the same C engine.
The native viewer remains the result inspector. No model API key or external inference service is needed by the adapter.

```bash
make am
./openphysics --listen
# Or run ./openphysics-server when the viewer is not needed.
python3 tools/mcp_http.py --port 8787
```

Endpoint: `http://127.0.0.1:8787/mcp`. The adapter defaults to `openphysics-mcp --connect`: all sessions use the running
native engine, and jobs survive an HTTP-session DELETE or disconnect. Start a persistent engine first. For an isolated
protocol check only: `python3 tools/mcp_http.py --port 8787 -- --embedded --workspace /tmp/openphysics-http`.
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
