#!/usr/bin/env python3
"""change_test.py - a spoken change made in one step: openphysics-mcp lab_change, and the headset's question naming the
simulation shown with the keys it can change (docs/xr.md "Talking with the AI", docs/mcp-clients.md).

    python3 tools/xr/change_test.py          (make test-xr runs it; needs openphysics-mcp and build/labrun)

Why: with lab_scenario_get and lab_run, "make the wind twice as strong" costs the voice model a read of the solver's
documentation (up to 15 000 tokens), the whole scenario written out again (about 800 tokens for the flag) and two tool
round trips before the run starts. lab_change takes the scenario of the result shown and only the keys to change.

Criteria, written 2026-10-08 before the first run (amended only openly, below them):
1. Offered: openphysics-mcp lists lab_change, with base and set required, both in its default profile and with --lean.
2. The base is what is shown: a result in the results folder is changed from the scenario in its own header (what was
   computed and is on screen), before a scenario made here of the same name, before the example; the saved scenario
   equals that base with exactly the named values changed, plus "changed_from" (the base's name and the set), and
   nothing else differs.
3. Paths: a nested key ("stream.speed_m_s"), an element of a list of objects ("bodies.0.radius_m"), a list of numbers of
   the same length ("box_m") and a string ("collision") are each changed, and nothing beside them.
4. Refusals, each with nothing saved and nothing started: a path not in the scenario (the message names it and lists
   keys that are there, among them "stream.speed_m_s"), a string for a number, a number for a string, a list of another
   length, a group of keys or a list of objects replaced whole, "domain", a "source", an empty set, and an example's
   name as the new name.
5. Provenance: the "source" beside a changed value records its old and new value; a title that was not set notes the
   change; a title that was set is used as given.
6. Names: the first change of "tiny" is tiny_v2, the next tiny_v3 (once tiny_v2 exists), and a change of tiny_v3 is
   tiny_v4.
7. The run: lab_change starts labrun (a stand-in here, which records what it was given) on the saved scenario; with
   wait_seconds 0 it answers "running" in under 2 s with the run's pid file there, lab_run_status then gives labrun's
   record, and with the default wait a short run's record comes in the answer itself.
8. The real solver: lab_change of the example flow_volume with {"run.end_time_s": 0.05, "run.frames": 2}, in this
   repository's lab with results in a folder of the test's own, ends within 30 s with labrun's record (exit code 0,
   2 or more frames), and the result's header holds the changed scenario (end_time_s 0.05, changed_from.base
   flow_volume).
9. The headset's question names the simulation shown: when the app shows a lab result, the question the bridge gives
   the AI says to use lab_change with that result's name as base and lists the scenario's values by path (shallow keys
   before deeper ones, at most 1200 characters of them, "source" texts left out); when no lab result is shown it
   mentions neither.
"""
import json, os, pathlib, shutil, struct, subprocess, sys, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
MCP = ROOT / "openphysics-mcp"
LABRUN = ROOT / "build" / "labrun"
failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)
    return ok


def header_of(path):
    with open(path, "rb") as f:
        if f.read(8) != b"OPLAB01\n":
            return None
        n = struct.unpack("<I", f.read(4))[0]
        return json.loads(f.read(n))


def write_result(path, scenario):
    """a result file with only its header (lab_change reads nothing else)"""
    hdr = json.dumps({"domain": scenario["domain"], "title": scenario.get("title", ""), "scenario": scenario}).encode()
    path.write_bytes(b"OPLAB01\n" + struct.pack("<I", len(hdr)) + hdr)


STUB = r'''#!/usr/bin/env python3
# a stand-in for build/labrun: records what it was given, writes a result with the scenario in its header, and prints
# a record as labrun does; run.stub_sleep_s makes it take that long
import json, struct, sys, time
sc = json.load(open(sys.argv[1]))
time.sleep(float(sc.get("run", {}).get("stub_sleep_s", 0)))
hdr = json.dumps({"domain": sc["domain"], "title": sc.get("title", ""), "scenario": sc}).encode()
open(sys.argv[2], "wb").write(b"OPLAB01\n" + struct.pack("<I", len(hdr)) + hdr)
with open(__file__ + ".calls", "a") as f:
    f.write(json.dumps({"scenario": sys.argv[1], "out": sys.argv[2]}) + "\n")
print(json.dumps({"scenario": sys.argv[1], "domain": sc["domain"], "frames": 1, "steps": 1, "wall_s": 0.0, "diagnostics": {}}))
'''

TINY = {
    "domain": "flow", "model": "lbm3d", "title": "A tiny flag in a 6 m/s wind",
    "fluid": {"kinematic_viscosity_m2_s": 1.589e-05, "density_kg_m3": 1.1614, "source": "air at 300 K (a test value)"},
    "stream": {"speed_m_s": 6.0},
    "box_m": [-0.6, -0.6, -0.45, 2.4, 0.6, 0.75],
    "cell_m": 0.015,
    "collision": "regularized",
    "bodies": [{"shape": "cylinder", "centre_m": [0.0, 0.0, 0.15], "radius_m": 0.02, "source": "a pole (a test value)"}],
    "run": {"end_s": 3.0, "frames": 60, "stub_sleep_s": 0},
}


class Mcp:
    def __init__(self, lab, results, lean=False):
        args = [str(MCP), "--embedded", "--workspace", str(results.parent / "ws"), "--lab", str(lab), "--lab-results",
                str(results)] + (["--lean"] if lean else [])
        self.p = subprocess.Popen(args, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True)
        self.id = 0
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "change_test", "version": "1"}})
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.p.stdin.flush()

    def rpc(self, method, params=None):
        self.id += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.id, "method": method, "params": params or {}}) + "\n")
        self.p.stdin.flush()
        while True:
            m = json.loads(self.p.stdout.readline())
            if m.get("id") == self.id:
                return m

    def tools(self):
        return {t["name"]: t for t in self.rpc("tools/list")["result"]["tools"]}

    def call(self, name, args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})["result"]
        return r.get("structuredContent") or json.loads(r["content"][0]["text"])

    def stop(self):
        self.p.kill()
        self.p.wait()


def leaves_differ(a, b, path=""):
    """the paths where two scenarios differ"""
    if isinstance(a, dict) and isinstance(b, dict):
        out = []
        for k in list(a) + [k for k in b if k not in a]:
            out += leaves_differ(a.get(k, "<absent>"), b.get(k, "<absent>"), path + "." + k if path else k)
        return out
    if isinstance(a, list) and isinstance(b, list) and len(a) == len(b) and any(isinstance(x, (dict, list)) for x in a):
        out = []
        for i, (x, y) in enumerate(zip(a, b)):
            out += leaves_differ(x, y, "%s.%d" % (path, i))
        return out
    return [] if a == b else [path]


def main():
    for need in (MCP, LABRUN):
        if not need.exists():
            print("change_test: %s is not built (make openphysics-mcp lab)" % need)
            return 2
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-changetest-"))
    servers = []
    try:
        lab, res = tmp / "lab", tmp / "res"
        (lab / "examples" / "lab").mkdir(parents=True)
        (lab / "build").mkdir()
        res.mkdir()
        stub = lab / "build" / "labrun"
        stub.write_text(STUB)
        stub.chmod(0o755)
        calls = pathlib.Path(str(stub) + ".calls")
        (lab / "examples" / "lab" / "tiny.json").write_text(json.dumps(TINY, indent=1))
        runs = res / "agent-runs"

        print("1. offered")
        m = Mcp(lab, res)
        servers.append(m)
        lean = Mcp(lab, res, lean=True)
        servers.append(lean)
        for prof, srv in (("default", m), ("lean", lean)):
            t = srv.tools().get("lab_change")
            req = sorted((t or {}).get("inputSchema", (t or {}).get("input_schema", {})).get("required", []))
            check(t is not None and req == ["base", "set"], "%s profile lists lab_change with base and set required: %s" % (prof, req))
        lean.stop()
        servers.remove(lean)

        def change(args):
            return m.call("lab_change", dict({"wait_seconds": 5}, **args))

        def saved(name):
            try:
                return json.loads((runs / (name + ".json")).read_text())
            except (OSError, ValueError):
                return None

        print("2. the base is what is shown; 3. paths")
        r = change({"base": "tiny", "set": {"stream.speed_m_s": 12}})
        v = r.get("value") or {}
        s2 = saved("tiny_v2")
        check(r.get("ok") and v.get("name") == "tiny_v2" and s2 is not None, "the example changed and saved as tiny_v2: %s" % json.dumps(r)[:300])
        if s2:
            diff = sorted(leaves_differ(TINY, s2))
            check(diff == ["changed_from", "stream.speed_m_s", "title"] and s2["stream"]["speed_m_s"] == 12,
                  "tiny_v2 differs from the example only in the speed, the title and changed_from: %s" % diff)
            check(s2.get("changed_from") == {"base": "tiny", "set": {"stream.speed_m_s": 12}},
                  "changed_from holds the base's name and the set: %s" % s2.get("changed_from"))
        # a scenario made here of the same name comes before the example, and a result's header before both
        made = json.loads(json.dumps(TINY))
        made["cell_m"] = 0.03
        runs.mkdir(exist_ok=True)
        (runs / "tiny.json").write_text(json.dumps(made))
        r = change({"base": "tiny", "set": {"run.frames": 30}, "name": "from_made"})
        s = saved("from_made")
        check(s is not None and s["cell_m"] == 0.03 and s["run"]["frames"] == 30 and str((r.get("value") or {}).get("base_from", "")).endswith("agent-runs/tiny.json"),
              "a scenario made here comes before the example: cell_m %s, base_from %s" % (s and s.get("cell_m"), (r.get("value") or {}).get("base_from")))
        shown = json.loads(json.dumps(TINY))
        shown["cell_m"] = 0.05
        write_result(res / "tiny.lab", shown)
        r = change({"base": "tiny", "set": {"run.frames": 20}, "name": "from_shown"})
        s = saved("from_shown")
        check(s is not None and s["cell_m"] == 0.05 and str((r.get("value") or {}).get("base_from", "")).endswith("tiny.lab"),
              "the result shown comes first: its header's scenario was changed (cell_m %s, base_from %s)" % (s and s.get("cell_m"), (r.get("value") or {}).get("base_from")))
        (res / "tiny.lab").unlink()
        (runs / "tiny.json").unlink()
        r = change({"base": "tiny", "set": {"bodies.0.radius_m": 0.05, "box_m": [-1, -1, -1, 3, 1, 1], "collision": "bgk"},
                    "name": "paths"})
        s = saved("paths")
        if check(s is not None, "a list element, a list of numbers and a string changed together: %s" % json.dumps(r)[:200]):
            diff = sorted(leaves_differ(TINY, s))
            check(diff == ["bodies.0.radius_m", "bodies.0.source", "box_m", "changed_from", "collision", "title"]
                  and s["bodies"][0]["radius_m"] == 0.05 and s["box_m"] == [-1, -1, -1, 3, 1, 1] and s["collision"] == "bgk"
                  and s["bodies"][0]["centre_m"] == TINY["bodies"][0]["centre_m"],
                  "each changed and nothing beside them (the pole's source notes the change): %s" % diff)

        print("4. refusals")
        before = sorted(p.name for p in runs.iterdir())
        ncalls = len(calls.read_text().splitlines()) if calls.exists() else 0
        bad = [
            ({"base": "tiny", "set": {"stream.speed": 12}}, "a path not in the scenario"),
            ({"base": "tiny", "set": {"cell_m": "fine"}}, "a string for a number"),
            ({"base": "tiny", "set": {"collision": 2}}, "a number for a string"),
            ({"base": "tiny", "set": {"box_m": [0, 0, 0, 1, 1]}}, "a list of another length"),
            ({"base": "tiny", "set": {"stream": {"speed_m_s": 3}}}, "a group of keys replaced whole"),
            ({"base": "tiny", "set": {"bodies": []}}, "a list of objects replaced whole"),
            ({"base": "tiny", "set": {"domain": "fire"}}, "the domain"),
            ({"base": "tiny", "set": {"fluid.source": "made up"}}, "a source"),
            ({"base": "tiny", "set": {}}, "an empty set"),
            ({"base": "tiny", "set": {"cell_m": 0.02}, "name": "tiny"}, "an example's name as the new name"),
        ]
        for args, what in bad:
            r = change(args)
            err = r.get("error") or {}
            ok = r.get("ok") is False and err.get("code") == "INVALID_ARGUMENT"
            if what == "a path not in the scenario":
                ok = ok and "stream.speed" in err.get("message", "") and "stream.speed_m_s" in err.get("message", "") + err.get("hint", "")
            check(ok, "refused: %s (%s)" % (what, (err.get("message", "") + " / " + err.get("hint", ""))[:150]))
        after = sorted(p.name for p in runs.iterdir())
        ncalls2 = len(calls.read_text().splitlines()) if calls.exists() else 0
        check(before == after and ncalls == ncalls2, "nothing was saved and nothing started by the refusals")

        print("5. provenance")
        s = saved("paths")
        src = (s or {}).get("bodies", [{}])[0].get("source", "")
        check("0.02" in src and "0.05" in src and src.startswith(TINY["bodies"][0]["source"]),
              "the pole's source keeps its words and records 0.02 and 0.05: %s" % src[-160:])
        s2 = saved("tiny_v2") or {}
        check(s2.get("title", "").startswith(TINY["title"]) and "12" in s2.get("title", "")[len(TINY["title"]):],
              "an unset title notes the change: %s" % s2.get("title"))
        r = change({"base": "tiny", "set": {"stream.speed_m_s": 3, "title": "A tiny flag in a 3 m/s breeze"}, "name": "titled"})
        check((saved("titled") or {}).get("title") == "A tiny flag in a 3 m/s breeze", "a title set is used as given")

        print("6. names")
        r = change({"base": "tiny", "set": {"stream.speed_m_s": 8}})
        check((r.get("value") or {}).get("name") == "tiny_v3", "the next change of tiny is tiny_v3: %s" % (r.get("value") or {}).get("name"))
        r = change({"base": "tiny_v3", "set": {"stream.speed_m_s": 9}})
        check((r.get("value") or {}).get("name") == "tiny_v4", "a change of tiny_v3 is tiny_v4: %s" % (r.get("value") or {}).get("name"))

        print("7. the run")
        last = json.loads(calls.read_text().splitlines()[-1]) if calls.exists() else {}
        check(last.get("scenario", "").endswith("agent-runs/tiny_v4.json") and last.get("out", "").endswith("res/tiny_v4.lab"),
              "labrun was given the saved scenario and the result's place: %s" % last)
        t0 = time.time()
        r = m.call("lab_change", {"base": "tiny", "set": {"run.stub_sleep_s": 3}, "name": "slow2", "wait_seconds": 0})
        took = time.time() - t0
        v = r.get("value") or {}
        check(r.get("ok") and v.get("running") is True and (runs / "slow2.pid").exists() and took < 2,
              "with wait_seconds 0: running, its pid file there, answered in %.2f s" % took)
        st = m.call("lab_run_status", {"name": "slow2", "wait_seconds": 10}).get("value") or {}
        check(st.get("running") is False and (st.get("record") or {}).get("frames") == 1,
              "lab_run_status then gives labrun's record: %s" % json.dumps(st.get("record")))
        r = m.call("lab_change", {"base": "tiny", "set": {"run.frames": 10}, "name": "quick"})
        v = r.get("value") or {}
        check(r.get("ok") and v.get("running") is False and (v.get("record") or {}).get("frames") == 1,
              "with the default wait a short run's record is in the answer: %s" % json.dumps(v.get("record")))
        m.stop()
        servers.remove(m)

        print("8. the real solver")
        res2 = tmp / "res2"
        res2.mkdir()
        real = Mcp(ROOT, res2)
        servers.append(real)
        t0 = time.time()
        r = real.call("lab_change", {"base": "flow_volume", "set": {"run.end_time_s": 0.05, "run.frames": 2}, "wait_seconds": 30})
        took = time.time() - t0
        v = r.get("value") or {}
        rec = v.get("record") or {}
        h = header_of(res2 / ("%s.lab" % v.get("name", "?"))) if v.get("name") else None
        hs = (h or {}).get("scenario") or {}
        check(r.get("ok") and v.get("exit_code") == 0 and rec.get("frames", 0) >= 2 and took < 30,
              "flow_volume shortened and computed in %.1f s: %s" % (took, json.dumps(rec)[:200]))
        check(hs.get("run", {}).get("end_time_s") == 0.05 and hs.get("changed_from", {}).get("base") == "flow_volume",
              "the result's header holds the changed scenario: %s" % json.dumps(hs.get("run")))
        real.stop()
        servers.remove(real)

        print("9. the headset's question names the simulation shown")
        xr = tmp / "xr"
        xr.mkdir()
        os.environ["OPENPHYSICS_XR_DIR"] = str(xr)
        sys.path.insert(0, str(ROOT / "tools" / "xr"))
        import xr_bridge
        shown_lab = res2 / ("%s.lab" % v.get("name", "x"))

        def question(state):
            (xr / "state.json").write_text(json.dumps(state))
            for f in xr.glob("ask-*.txt"):
                f.unlink()
            xr_bridge.put_question("make the flow twice as fast", "the person points at nothing")
            asks = sorted(xr.glob("ask-*.txt"))
            return asks[-1].read_text() if asks else ""
        q = question({"app": "OpenPhysicsAI", "scene": "lab", "lab": {"path": str(shown_lab)}})
        name = shown_lab.stem
        keys = q[q.find("lab_change"):]
        check("lab_change" in q and ("'%s'" % name) in q and "run.end_time_s 0.05" in q,
              "a lab result shown: lab_change, base '%s' and its values by path are in the question" % name)
        hdr = header_of(shown_lab) or {}
        top = [k for k, x in (hdr.get("scenario") or {}).items() if not isinstance(x, (dict, list)) and k not in ("source", "title")]
        deep = "run.end_time_s"
        check(all(keys.find(k + " ") < keys.find(deep) for k in top if k + " " in keys) and "source" not in keys,
              "shallow keys before deeper ones, no source text: %s" % keys[:300])
        listed = keys[keys.find(":") + 1:] if ":" in keys else keys
        check(len(listed) <= 1200 + 200, "at most 1200 characters of values (%d with the sentence around them)" % len(listed))
        q2 = question({"app": "OpenPhysicsAI", "scene": "fem"})
        check(q2 and "lab_change" not in q2, "no lab result shown: lab_change is not mentioned")
    finally:
        for srv in servers:
            srv.stop()
        if not failures:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("kept for a look: %s" % tmp)
    print("change_test: %s" % ("all checks passed" if not failures else "%d failed" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
