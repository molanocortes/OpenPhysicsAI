#!/usr/bin/env python3
"""mcptest.py - protocol-level tests of openphysics-mcp over stdio.

The client below is written independently with the Python standard library (it does not reuse the C JSON code),
so it checks the server against the MCP 2025-11-25 specification from the outside. It is a protocol test, NOT a
test with a real AI client; see docs/mcp-clients.md for the real-client integration check.

Run from the repository root after `make am`:
    python3 tools/mcptest.py
"""
import json
import os
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# OPENPHYSICS_BIN (NAVIER_BIN before the rename of 2026-10-21) selects another folder of binaries (an installed release:
# <release>/bin); default: the repository root
_bin = os.environ.get("OPENPHYSICS_BIN") or os.environ.get("NAVIER_BIN")
BIN = Path(_bin) if _bin else ROOT
MCP = BIN / "openphysics-mcp"
SERVER = BIN / "openphysics-server"
CTL = BIN / "openphysics-ctl"

PASS = 0
FAIL = 0


def check(cond, what):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"  FAIL: {what}")


def box_stl(path, sx, sy, sz):
    """binary STL of an axis-aligned box with outward counter-clockwise faces"""
    p = [(x, y, z) for z in (0, sz) for y in (0, sy) for x in (0, sx)]
    quads = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    tris = []
    for a, b, c, d in quads:
        tris.append((p[a], p[b], p[c]))
        tris.append((p[a], p[c], p[d]))
    with open(path, "wb") as f:
        f.write(b"box for mcptest".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


# The limit on each response (changed 2026-10-08 with the owner's leave, GOALS.md G16): `timeout` seconds while the
# machine has a core to spare. When it is oversubscribed, its 1-minute load average above its core count, the same work
# takes that much longer, so the limit stretches by the load over the cores, at most BUSY_MAX times, and the client says
# so on stderr. A hung or runaway server still fails; on an idle machine nothing changes.
BUSY_MAX = 10.0


def busy_factor():
    """the 1-minute load average over the cores, between 1 and BUSY_MAX"""
    try:
        return min(BUSY_MAX, max(1.0, os.getloadavg()[0] / (os.cpu_count() or 1)))
    except (OSError, AttributeError):
        return 1.0


class Client:
    busy = staticmethod(busy_factor)   # how much slower than an idle machine a response may come (a test replaces it)

    def __init__(self, args, env=None):
        self.proc = subprocess.Popen([str(MCP)] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, env=env)
        self.next_id = 1
        self.buf = b""
        self.all_lines = []

    def send_raw(self, data: bytes):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def send(self, obj):
        self.send_raw(json.dumps(obj, separators=(",", ":")).encode() + b"\n")

    def recv(self, timeout=20.0):
        start, factor = time.time(), 1.0
        fd = self.proc.stdout.fileno()
        while b"\n" not in self.buf:
            left = start + timeout * factor - time.time()
            if left <= 0:
                busy = self.busy()
                if busy > factor:   # oversubscribed: the same work takes longer, so wait that much longer
                    factor = busy
                    continue
                raise TimeoutError(f"no response from openphysics-mcp after {time.time() - start:.1f} s (limit {timeout:g} s"
                                   + (f", stretched {factor:.2f} times for a busy machine" if factor > 1 else "") + ")")
            r, _, _ = select.select([fd], [], [], left)
            if not r:
                continue
            chunk = os.read(fd, 1 << 20)
            if not chunk:
                raise EOFError("openphysics-mcp closed stdout")
            self.buf += chunk
        if factor > 1:
            print(f"mcptest: a response took {time.time() - start:.1f} s against the {timeout:g} s limit, accepted because the "
                  f"machine was busy (the limit stretched {factor:.2f} times, its load average over its cores)", file=sys.stderr, flush=True)
        line, self.buf = self.buf.split(b"\n", 1)
        self.all_lines.append(line)
        return json.loads(line)

    def request(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        resp = self.recv()
        assert resp.get("id") == rid, f"response id {resp.get('id')} != {rid}: {resp}"
        return resp

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)

    def initialize(self, version="2025-11-25"):
        r = self.request("initialize", {"protocolVersion": version, "capabilities": {},
                                        "clientInfo": {"name": "mcptest", "version": "1.0"}})
        self.notify("notifications/initialized")
        return r

    def call(self, name, args=None):
        params = {"name": name}
        if args is not None:
            params["arguments"] = args
        return self.request("tools/call", params)

    def close(self, timeout=10):
        try:
            self.proc.stdin.close()
        except BrokenPipeError:
            pass
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = None
        err = self.proc.stderr.read().decode(errors="replace")
        return code, err


def structured(resp):
    return resp["result"].get("structuredContent")


def close(a, b, tol=1e-9):
    return isinstance(a, list) and len(a) == len(b) and all(abs(x - y) <= tol for x, y in zip(a, b))


def main():
    if not MCP.exists():
        print("openphysics-mcp not built (run: make am)")
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="nvmcp"))
    ws = tmp / "ws"
    stl = tmp / "block.stl"
    box_stl(stl, 30.0, 20.0, 10.0)
    common = ["--workspace", str(ws), "--allow-read", str(tmp)]

    print("== the client's limit on a busy machine")
    def late_server(delay):
        """a stand-in for the server that answers one line after delay seconds"""
        c = Client.__new__(Client)
        c.proc = subprocess.Popen([sys.executable, "-c", f"import sys, time; time.sleep({delay}); print('{{}}', flush=True); time.sleep(30)"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        c.next_id, c.buf, c.all_lines = 1, b"", []
        return c
    for busy, accepted in ((1.0, False), (10.0, True)):
        c = late_server(1.0)
        c.busy = lambda b=busy: b
        t0, got = time.time(), None
        try:
            got = c.recv(timeout=0.4)
        except TimeoutError:
            pass
        waited = time.time() - t0
        c.proc.kill(), c.proc.wait()
        if accepted:
            check(got == {} and waited >= 0.4, f"a response 1 s late is accepted against a 0.4 s limit on a machine ten times "
                                               f"oversubscribed ({got}, {waited:.2f} s)")
        else:
            check(got is None and waited >= 0.4, f"with a core to spare the same response times out at the 0.4 s limit ({waited:.2f} s)")
    check(1.0 <= busy_factor() <= BUSY_MAX, f"this machine's busy factor is between 1 and {BUSY_MAX:g} ({busy_factor():.2f})")

    print("== lifecycle and negotiation")
    c = Client(["--embedded"] + common)
    r = c.request("tools/list")
    check("error" in r, "tools/list before initialize is refused")
    r = c.request("ping")
    check(r.get("result") == {}, "ping is allowed before initialization")
    r = c.initialize("2025-11-25")
    res = r.get("result", {})
    check(res.get("protocolVersion") == "2025-11-25", "latest version echoed")
    check("tools" in res.get("capabilities", {}), "tools capability declared")
    check(res.get("serverInfo", {}).get("name") == "openphysics" and "title" in res["serverInfo"], "serverInfo with title")
    check(isinstance(res.get("instructions"), str) and "units" in res["instructions"], "instructions explain units")
    r = c.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "x", "version": "1"}})
    check("error" in r, "second initialize refused")

    print("== tool discovery")
    r = c.request("tools/list")
    tools = r["result"]["tools"]
    names = {t["name"] for t in tools}
    check({"capabilities_get", "project_create", "geometry_import", "geometry_place", "geometry_diagnostics"} <= names, "expected tools listed")
    for t in tools:
        schema = t.get("inputSchema", {})
        check(schema.get("type") == "object", f"{t['name']}: inputSchema is an object schema")
        check("$ref" not in json.dumps(schema), f"{t['name']}: schema self-contained")
        check(isinstance(t.get("annotations", {}).get("readOnlyHint"), bool), f"{t['name']}: readOnlyHint")
        # Acceptance declared before this change's first run: every advertised tool
        # has explicit impact hints and a schema for its structured result envelope.
        check(all(isinstance(t.get("annotations", {}).get(k), bool) for k in
                  ("destructiveHint", "idempotentHint", "openWorldHint")), f"{t['name']}: explicit impact hints")
        check(t.get("outputSchema", {}).get("type") == "object" and
              "ok" in t["outputSchema"].get("required", []), f"{t['name']}: result envelope schema")
        check(1 <= len(t["name"]) <= 128 and all(ch.isalnum() or ch in "_-." for ch in t["name"]), f"{t['name']}: valid tool name")
    gi = next(t for t in tools if t["name"] == "geometry_import")
    check("units" in gi["inputSchema"].get("required", []), "geometry_import requires units")
    check(gi["annotations"]["readOnlyHint"] is False, "geometry_import is not read-only")
    r = c.request("tools/list", {"cursor": "bogus"})
    check(r.get("error", {}).get("code") == -32602, "invalid cursor rejected")

    print("== protocol errors")
    c.send_raw(b"{not json\n")
    r = c.recv()
    check(r.get("error", {}).get("code") == -32700 and r.get("id") is None, "parse error -32700 with null id")
    r = c.request("no/such/method")
    check(r.get("error", {}).get("code") == -32601, "unknown method -32601")
    r = c.call("no_such_tool", {})
    check(r.get("error", {}).get("code") == -32602, "unknown tool is a protocol error")
    r = c.request("tools/call", {"name": "capabilities_get", "arguments": [1, 2]})
    check(r.get("error", {}).get("code") == -32602, "non-object arguments rejected")
    c.send([{"jsonrpc": "2.0", "id": 99, "method": "ping"}])
    r = c.recv()
    check(r.get("error", {}).get("code") == -32600, "batches refused for 2025-11-25")
    c.send_raw(b"\n")  # blank lines are ignored
    r = c.request("ping")
    check(r.get("result") == {}, "session usable after errors")

    print("== tool execution")
    r = c.call("capabilities_get", {})
    res = r["result"]
    check(res["isError"] is False and structured(r)["ok"] is True, "capabilities_get succeeds")
    check(json.loads(res["content"][0]["text"])["ok"] is True, "text content mirrors structured content")
    check(len(structured(r)["value"]["model_limitations"]) > 0, "limitations reported")
    analyses = {a["name"]: a for a in structured(r)["value"]["analyses"]}
    check(analyses["fff_print"]["status"] == "available" and analyses["fff_print"]["operation"] == "mech_print_run",
          "capabilities expose the implemented FDM operation")
    check(analyses["lpbf_build"]["status"] == "available" and "inherent strain" in analyses["lpbf_build"]["physics"],
          "capabilities expose LPBF with its actual model scope")
    r = c.call("geometry_import", {"path": str(stl)})
    res = r["result"]
    check(res["isError"] is True and "units" in res["content"][0]["text"], "input validation is a tool execution error mentioning units")
    r = c.call("project_create", {"name": "mcp_demo", "idempotency_key": "create-1"})
    check(r["result"]["isError"] is False, "project_create")
    rev = structured(r)["revision"]
    r = c.call("project_create", {"name": "mcp_demo", "idempotency_key": "create-1"})
    check(structured(r).get("replayed") is True and structured(r)["revision"] == rev, "idempotent retry replayed")
    r = c.call("geometry_import", {"path": str(stl), "units": "mm"})
    sc = structured(r)
    check(r["result"]["isError"] is False and sc["value"]["body"]["closed_solid"] is True, "geometry_import succeeds")
    check(sc["value"]["body"]["size_mm"] == [30, 20, 10], f"size {sc['value']['body']['size_mm']}")
    rev = sc["revision"]
    r = c.call("geometry_place", {"body": "block", "up_axis": "+X", "expected_revision": rev - 1})
    check(r["result"]["isError"] is True and structured(r)["error"]["code"] == "REVISION_CONFLICT", "stale revision conflict")
    r = c.call("geometry_place", {"body": "block", "up_axis": "+X", "rotate_z": "90 deg", "expected_revision": rev})
    sc = structured(r)
    check(r["result"]["isError"] is False and sc["value"]["body"]["size_mm"] == [20, 10, 30], f"placement size {sc['value']['body']['size_mm'] if sc.get('value') else sc}")
    r = c.call("geometry_diagnostics", {"body": "block"})
    wt = structured(r)["value"]["diagnostics"]["checks"]["wall_thickness"]
    check(abs(wt["min_mm"] - 10.0) < 1e-6, f"min wall thickness {wt['min_mm']}")
    r = c.call("project_save", {})
    check(r["result"]["isError"] is False, "project_save")
    project_dir = ws / "mcp_demo"
    check((project_dir / "project.json").exists(), "project.json on disk")

    print("== stdout purity and shutdown")
    for line in c.all_lines:
        obj = json.loads(line)
        check(obj.get("jsonrpc") == "2.0" and ("result" in obj or "error" in obj), "every stdout line is a JSON-RPC response")
    code, err = c.close()
    check(code == 0, f"clean exit on stdin EOF (code {code})")
    check("embedded engine" in err, "logs go to stderr")

    print("== older protocol revisions")
    c = Client(["--embedded"] + common)
    r = c.initialize("2024-11-05")
    check(r["result"]["protocolVersion"] == "2024-11-05", "2024-11-05 negotiated")
    r = c.request("tools/list")
    t0 = r["result"]["tools"][0]
    check("title" not in t0 and "annotations" not in t0, "no fields newer than 2024-11-05")
    r = c.call("capabilities_get", {})
    check("structuredContent" not in r["result"], "no structuredContent for 2024-11-05")
    c.send([{"jsonrpc": "2.0", "id": 50, "method": "ping"}, {"jsonrpc": "2.0", "id": 51, "method": "ping"}])
    r = c.recv()
    check(isinstance(r, list) and [m["id"] for m in r] == [50, 51], "batch answered for 2024-11-05")
    c.close()
    c = Client(["--embedded"] + common)
    r = c.initialize("1999-01-01")
    check(r["result"]["protocolVersion"] == "2025-11-25", "unsupported version answered with the latest supported")
    c.close()

    print("== bridge mode: jobs and projects outlive the MCP client")
    sock = tmp / "run" / "ctl.sock"
    ready = tmp / "ready"
    srv = subprocess.Popen([str(SERVER), "--socket", str(sock), "--ready-file", str(ready)] + common,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for _ in range(100):
        if ready.exists():
            break
        time.sleep(0.05)
    check(ready.exists(), "openphysics-server ready")
    c = Client(["--connect", str(sock)])
    c.initialize()
    r = c.call("project_create", {"name": "bridged"})
    check(r["result"]["isError"] is False, "project created through the bridge")
    r = c.call("geometry_import", {"path": str(stl), "units": "mm", "name": "part"})
    check(r["result"]["isError"] is False, "geometry imported through the bridge")
    r = c.call("geometry_import", {"path": str(stl)})
    check(r["result"]["isError"] is True and structured(r)["error"]["code"] == "INVALID_PARAMS", "errors cross the bridge with their codes")
    code, err = c.close()
    check(code == 0 and "bridging" in err, "bridge client exits cleanly")
    out = subprocess.run([str(CTL), "--socket", str(sock), "call", "project_inspect", '{"sections":["bodies"]}'],
                         capture_output=True, text=True, timeout=20)
    check(out.returncode == 0 and '"part"' in out.stdout, "server still holds the project after the MCP client left")
    srv.send_signal(signal.SIGTERM)
    try:
        code = srv.wait(timeout=10)
    except subprocess.TimeoutExpired:
        srv.kill()
        code = None
    check(code == 0, f"openphysics-server stops cleanly on SIGTERM (code {code})")
    check(not sock.exists(), "socket removed at shutdown")

    shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{'MCP PROTOCOL TESTS FAILED' if FAIL else 'ALL MCP PROTOCOL TESTS PASSED'}: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
