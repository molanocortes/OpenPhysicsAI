#!/usr/bin/env python3
"""cadagenttest.py - GOALS.md G23 step 4: printed-part geometry for agents, through openphysics-mcp as an AI client calls it.

    python3 tools/cadagenttest.py                  A1 and A2: no model, nothing spent
    python3 tools/cadagenttest.py --real-model     also A3: one turn of Claude Haiku 4.5 through Claude Code (a few cents)

Criteria, written 2026-10-08 before the first run (GOALS.md G23 step 4), amended only openly below them:
A1 openphysics-mcp lists cad_part and cad_lattice in its default and lean profiles, the lean list's size reported
A2 through openphysics-mcp over stdio: cad_part with P1's model answers with the part's volume 19200 + 448 pi mm^3 within
   1e-9 relative and its STEP and STL paths; cad_lattice with that part's name, a 1 mm skin and a gyroid sheet at 0.3
   answers with a core fraction within 0.02 of 0.3; geometry_import of the part's STL in millimetres makes it a body of
   a new project, its volume within 0.1 per cent of the part's
A3 one run with a real model (Claude Haiku 4.5 through Claude Code with openphysics-mcp --lean, as the app's voice session
   runs it), asked in one sentence for the bracket of P2 (the plate, the boss, the hole, the four vertical corners
   rounded at 5 mm) with a 1 mm skin and a 30 per cent gyroid core: it makes both with the tools in one turn; the part's
   volume 18400 + 648 pi mm^3 within 1e-6 relative if it built that part; the core's fraction within 0.02 of 0.3; the
   time and the cost reported, the result said as it is
"""
import json, math, os, pathlib, shutil, subprocess, sys, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parents[1]
MCP = ROOT / "openphysics-mcp"
PI = math.pi
failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)
    return ok


class Mcp:
    def __init__(self, tmp, lean):
        args = [str(MCP), "--embedded", "--workspace", str(tmp / "ws"), "--lab", str(ROOT), "--lab-results", str(tmp / "res")]
        self.p = subprocess.Popen(args + (["--lean"] if lean else []), cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True)
        self.n = 0
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "cadagenttest", "version": "1"}})
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.p.stdin.flush()

    def rpc(self, method, params=None):
        self.n += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params or {}}) + "\n")
        self.p.stdin.flush()
        while True:
            m = json.loads(self.p.stdout.readline())
            if m.get("id") == self.n:
                return m

    def call(self, name, args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})["result"]
        return r.get("structuredContent") or json.loads(r["content"][-1]["text"])

    def stop(self):
        self.p.kill()
        self.p.wait()


def find(o, key):
    """the first value of key anywhere in o"""
    if isinstance(o, dict):
        if key in o:
            return o[key]
        o = list(o.values())
    if isinstance(o, list):
        for x in o:
            v = find(x, key)
            if v is not None:
                return v
    return None


P1 = {"features": [
    {"id": "plate", "op": "box", "size_mm": [60, 40, 8]},
    {"id": "boss", "op": "cylinder", "radius_mm": 8, "height_mm": 20, "at_mm": [30, 20, 0], "axis": "z"},
    {"id": "body", "op": "union", "of": ["plate", "boss"]},
    {"id": "bore", "op": "cylinder", "radius_mm": 4, "height_mm": 40, "at_mm": [30, 20, -10], "axis": "z"},
    {"id": "part", "op": "cut", "from": "body", "remove": ["bore"]},
]}


def real_model(tmp):
    """A3: one turn of the voice model with the lean tools, as the app's conversation runs it"""
    claude = shutil.which("claude")
    if not claude:
        print("  (A3 skipped: Claude Code is not installed)")
        return
    cfg = tmp / "mcp.json"
    cfg.write_text(json.dumps({"mcpServers": {"openphysics": {"command": str(MCP), "args": [
        "--embedded", "--workspace", str(tmp / "ws3"), "--lab", str(ROOT), "--lab-results", str(tmp / "res3"), "--lean"]}}}))
    ask = ("Design a mounting bracket for 3D printing: a 60 by 40 by 8 mm plate with its corner at the origin, a round boss "
           "of radius 8 mm standing 20 mm from the plate's floor at the plate's centre, a hole of radius 4 mm through the "
           "boss and the plate, and the plate's four vertical corner edges rounded at 5 mm. Then make the printed version "
           "lighter: a 1 mm skin and a 30 percent gyroid core. Say the volumes and the core's solid fraction.")
    cmd = [claude, "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose", "--tools", "",
           "--mcp-config", str(cfg), "--strict-mcp-config", "--allowedTools", "mcp__openphysics", "--model", "haiku", "--permission-mode", "default"]
    env = dict(os.environ, ENABLE_TOOL_SEARCH="false", MCP_TOOL_TIMEOUT="900000")
    p = subprocess.Popen(cmd, cwd=tmp, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=env)
    time.sleep(8)
    t0 = time.time()
    p.stdin.write(json.dumps({"type": "user", "message": {"role": "user", "content": [{"type": "text", "text": ask}]}}) + "\n")
    p.stdin.flush()
    calls, results, out = [], {}, None
    for line in p.stdout:
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if e.get("type") == "assistant":
            for c in e["message"].get("content", []):
                if c.get("type") == "tool_use":
                    calls.append((round(time.time() - t0, 1), c["name"].replace("mcp__openphysics__", ""), c.get("input"), c["id"]))
        elif e.get("type") == "user":
            for c in e.get("message", {}).get("content", []) if isinstance(e.get("message", {}).get("content"), list) else []:
                if c.get("type") == "tool_result":
                    txt = c.get("content")
                    txt = "".join(x.get("text", "") for x in txt) if isinstance(txt, list) else str(txt)
                    results[c["tool_use_id"]] = txt
        elif e.get("type") == "result":
            out = e
            break
    secs = time.time() - t0
    p.stdin.close()
    p.kill()
    p.wait()
    print("  A3: %.1f s, %s USD, %s turns" % (secs, (out or {}).get("total_cost_usd"), (out or {}).get("num_turns")))
    for t, name, inp, cid in calls:
        res = results.get(cid, "")
        print("    %5.1f s  %s %s -> %s" % (t, name, json.dumps(inp)[:160], res[:160].replace("\n", " ")))
    said = ((out or {}).get("result") or "").strip()
    print("    said: %s" % said[:500])
    parts = [c for c in calls if c[1] == "cad_part"]
    lats = [c for c in calls if c[1] == "cad_lattice"]
    vol = frac = None
    for c in parts:
        try:
            r = json.loads(results.get(c[3], "").splitlines()[-1])
            vol = find(r.get("value", r).get("part", {}), "volume_mm3") or vol
        except (ValueError, IndexError, AttributeError):
            pass
    for c in lats:
        try:
            r = json.loads(results.get(c[3], "").splitlines()[-1])
            frac = find(r.get("value", r).get("core", {}), "solid_fraction") or frac
        except (ValueError, IndexError, AttributeError):
            pass
    want = 18400 + 648 * PI
    check(bool(parts) and bool(lats) and out is not None and not (out or {}).get("is_error"), "A3 the part and its lattice made with the tools in one turn")
    print("    the part's volume %s mm^3 (P2's closed form %.6f; %s)" % (vol, want, "the same part" if vol and abs(vol - want) / want < 1e-6 else "not the same part, or none"))
    check(frac is not None and abs(frac - 0.3) <= 0.02, "A3 the core's fraction %s against 0.3" % frac)


def main():
    if not MCP.exists() or not (ROOT / "build" / "openphysics-cad").exists():
        print("cadagenttest: openphysics-mcp and build/openphysics-cad must be built (make openphysics-mcp build/openphysics-cad)")
        return 2
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-cadagent-"))
    servers = []
    try:
        print("A1 the tools offered")
        for lean in (False, True):
            m = Mcp(tmp, lean)
            servers.append(m)
            tools = m.rpc("tools/list")["result"]["tools"]
            names = {t["name"] for t in tools}
            size = len(json.dumps(tools))
            check({"cad_part", "cad_lattice"} <= names, "%s profile: cad_part and cad_lattice among %d tools (%d characters, about %d tokens)"
                  % ("lean" if lean else "default", len(tools), size, size // 4))
        m = servers[0]

        print("A2 a part, its lattice, and the part in a project")
        r = m.call("cad_part", {"name": "bracket", "model": P1})
        v = r.get("value") or {}
        vol = (v.get("part") or {}).get("volume_mm3", float("nan"))
        files = v.get("files") or {}
        check(r.get("ok") and abs(vol - (19200 + 448 * PI)) / (19200 + 448 * PI) < 1e-9 and os.path.exists(files.get("step", "")) and os.path.exists(files.get("stl", "")),
              "cad_part: %.10g mm^3, STEP and STL written (%s)" % (vol, (r.get("error") or {}).get("message", "")))
        r = m.call("cad_lattice", {"name": "bracket_gyroid", "part": "bracket", "skin_mm": 1, "resolution_mm": 0.15,
                                   "core": {"kind": "gyroid", "form": "sheet", "cell_mm": 6, "solid_fraction": 0.3}})
        lv = r.get("value") or {}
        f = (lv.get("core") or {}).get("solid_fraction", float("nan"))
        check(r.get("ok") and abs(f - 0.3) <= 0.02, "cad_lattice: core fraction %.4f, %s s (%s)" % (f, lv.get("seconds"), (r.get("error") or {}).get("message", "")))
        pr = m.call("project_create", {"name": "cadagent", "directory": str(tmp / "project")})
        gi = m.call("geometry_import", {"path": files.get("stl", ""), "units": "mm", "units_source": "user", "name": "bracket"})
        bv = find(gi, "volume_mm3")
        check(pr.get("ok") and gi.get("ok") and bv is not None and abs(bv - vol) / vol < 1e-3,
              "geometry_import: a body of %s mm^3 against the part's %.6g (%s)" % (bv, vol, (gi.get("error") or pr.get("error") or {}).get("message", "")))
        for s in servers:
            s.stop()
        servers.clear()
        if "--real-model" in sys.argv:
            print("A3 a real model, one sentence")
            real_model(tmp)
    finally:
        for s in servers:
            s.stop()
        if not failures:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("kept for a look: %s" % tmp)
    print("cadagenttest: %s" % ("all checks passed" if not failures else "%d failed" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
