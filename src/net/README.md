# `src/net/` — control socket and MCP

Two ways in from outside the process, over the same operations: a local control socket and a Model Context Protocol
server core. Neither contains physics; both end in `ops_invoke`.

Up: [module map](../../docs/map/MODULES.md#srcnet--control-socket-and-mcp) ·
request path: [DATAFLOW.md](../../docs/map/DATAFLOW.md) · [AGENTS.md](../../AGENTS.md).

## Read first

| File | What it gives you |
|---|---|
| `ctlserver.h` / `ctlserver.c` | the Unix/TCP control socket: framing, size limits, timeouts, authentication, JSON-RPC dispatch to `ops_find` + `ops_invoke` |
| `mcp.h` / `mcp.c` | the MCP session: lifecycle, `tools/list` built from the operation schemas, `tools/call` → an operation. Transport independent |
| `ctlclient.h` | the client side used by `navier-ctl`, the MCP bridge and the tests |
| `netutil.h` | the small POSIX socket helpers shared by all three |

## Talks to

- [`ctl/`](../ctl/README.md) — every request becomes an operation there.
- [`bin/`](../bin/README.md) — `navier-server` hosts the socket, `navier-mcp` the MCP session, `navier-ctl` the client.

## Tests

```bash
make headless
./build/ctltest            # framing, partial reads and writes, pipelining, malformed requests, limits
python3 tools/mcptest.py   # MCP protocol level
```

## Safety properties to preserve

- The socket is **local by default** (`~/.navier/run/control.sock`, mode 0600 in a 0700 directory); a TCP bind is
  refused on a non-loopback address, and TCP requires a token.
- There is no shell execution and no arbitrary file access: paths are confined to the engine's roots
  ([`ctl/engine.c`](../ctl/README.md), `path_within`).
- The MCP server serves exactly the operations in the embedded schema, so a client cannot reach anything the contract
  does not describe. Client set-up: [../../docs/mcp-clients.md](../../docs/mcp-clients.md), protocol:
  [../../docs/control-protocol.md](../../docs/control-protocol.md).
