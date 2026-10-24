#!/usr/bin/env python3
"""loop_bench.py - the whole spoken loop with a real model, timed: from a sentence said in the headset to the changed
simulation in front of the person (docs/xr.md "Talking with the AI").

    python3 tools/xr/loop_bench.py ["Make the fire twice as strong."] [--base FIRE.lab]

What runs, all of it real and none of it the owner's: a headless app with a home of its own (its lab folder, its
agent's files), a bridge on a folder of its own, Claude Code (the one installed, signed in as the owner, on the voice
model, Haiku 4.5 unless ~/.openphysicsai/app/ui-state says otherwise) with openphysics-mcp --lean, and the lab's own solver. The
result shown when the sentence is said is a small fire (examples/lab/methane_fire.json cut to 3 s and 24 frames,
computed here first, a few minutes on a busy laptop; --base FIRE.lab takes it from that file, and writes it there
when the file is not there yet). No page is opened: this script asks the bridge as the page would, and reads the
app's state as the page does.

It costs money: one turn of the voice model, a few US cents (the record says what Claude Code reported). It prints the
time of each step after the sentence: the first words, the run started (in the state's computing list), the run shown
while it computes (lab.growing, from its second frame), each frame after (with the frames in the result's file, how
old the app's state is, and whether the app and the run are alive), the answer done, the run done. When the run is
not seen to its end, the folders are kept and the logs' ends printed; otherwise nothing is kept (Claude Code keeps
its own record of the conversation, under ~/.claude, as for any session).
"""
import json, os, pathlib, shutil, subprocess, sys, tempfile, time, urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "xr"))
from xr_test import Proc, free_port, http, wait  # noqa: E402


def frames_in(path):
    """the complete frames in a lab result as it is now (src/lab/labio.h)"""
    try:
        b = path.read_bytes()
    except OSError:
        return -1
    at, n = 12 + int.from_bytes(b[8:12], "little") if len(b) >= 12 else len(b), 0
    while at + 12 <= len(b) and b[at:at + 4] == b"FRAM":
        size = int.from_bytes(b[at + 4:at + 12], "little")
        if at + 12 + size > len(b):
            break
        at, n = at + 12 + size, n + 1
    return n


def main():
    args = sys.argv[1:]
    base_file = None
    if "--base" in args:
        i = args.index("--base")
        base_file = pathlib.Path(args[i + 1]) if i + 1 < len(args) else None
        args = args[:i] + args[i + 2:]
    sentence = " ".join(args) or "Make the fire twice as strong."
    claude = shutil.which("claude")
    for need in (ROOT / "openphysics", ROOT / "openphysics-mcp", ROOT / "build" / "labrun"):
        if not need.exists():
            print("loop_bench: %s is not built (make openphysics openphysics-mcp lab)" % need)
            return 2
    if not claude:
        print("loop_bench: Claude Code (claude) is not installed")
        return 2
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-loopbench-"))
    home = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-lb-", dir="/tmp"))  # short: the agent's socket path
    procs, keep = [], False
    try:
        lab = home / "OpenPhysicsAI-Projects" / "lab"
        (lab / "agent-runs").mkdir(parents=True)
        xr = tmp / "xr"
        xr.mkdir()
        (tmp / "runs").mkdir()
        # the result shown: a small fire, computed here
        sc = json.loads((ROOT / "examples" / "lab" / "methane_fire.json").read_text())
        sc["run"]["end_s"], sc["run"]["frames"] = 3.0, 24
        sc["run"].pop("average_from_s", None)
        (lab / "fire_small.json").write_text(json.dumps(sc, indent=1))
        t = time.time()
        if base_file and base_file.exists():
            shutil.copyfile(base_file, lab / "fire_small.lab")
            print("the shown result: fire_small, from %s" % base_file)
        else:
            r = subprocess.run([str(ROOT / "build" / "labrun"), str(lab / "fire_small.json"), str(lab / "fire_small.lab"), "--quiet"],
                               capture_output=True, text=True, timeout=900)
            if r.returncode != 0:
                print("loop_bench: the small fire did not compute: %s" % r.stderr[-400:])
                return 1
            print("the shown result: fire_small, computed in %.1f s" % (time.time() - t))
            if base_file:
                shutil.copyfile(lab / "fire_small.lab", base_file)
        env = dict(os.environ, OPENPHYSICS_XR="1", OPENPHYSICS_XR_DIR=str(xr), OPENPHYSICS_AGENT_RUNS_DIR=str(tmp / "runs"), HOME=str(home))
        real_home = os.environ.get("HOME", "")
        cmd = ('agent command env HOME=%s %s -p $OPENPHYSICS_PROMPT --mcp-config $OPENPHYSICS_MCP_CONFIG --strict-mcp-config '
               '--allowedTools mcp__openphysics --output-format stream-json --verbose' % (real_home, claude))
        app = Proc([str(ROOT / "openphysics"), "--headless", "--size", "320x240", "--exec",
                    "%s; lab open %s; frames 100000000" % (cmd, lab / "fire_small.lab")], str(tmp / "app.log"), env)
        procs.append(app)
        state = lambda: json.loads((xr / "state.json").read_text())  # noqa: E731
        if not wait(lambda: (state().get("lab") or {}).get("path", "").endswith("fire_small.lab") or None, 40):
            print("loop_bench: the app did not show the small fire")
            return 1
        port = free_port()
        bridge = Proc([sys.executable, str(ROOT / "tools" / "xr" / "xr_bridge.py"), "--port", str(port), "--dir", str(xr)],
                      str(tmp / "bridge.log"), env)
        procs.append(bridge)
        base = "http://127.0.0.1:%d" % port
        wait(lambda: http(base + "/state")[0] == 200, 15)
        # the page's polls make the app start the conversation before anything is said, as in the headset
        for _ in range(60):
            http(base + "/state")
            time.sleep(0.25)
        ctx = "the headset shows A fire, coloured by T [K], t = 3 s (24 of 24)"
        t0 = time.time()
        code, _ = http(base + "/ask", json.dumps({"text": sentence, "context": ctx}).encode())
        print("said: %r (the bridge answered %d)" % (sentence, code))
        marks, name, next_look, last_look = {}, "", 0.0, ()

        def mark(what, extra=""):
            if what not in marks:
                marks[what] = time.time() - t0
                print("  %6.2f s  %s%s" % (marks[what], what, extra))
        end = t0 + 480
        while time.time() < end:
            http(base + "/state")  # the page, polling
            try:
                st = state()
            except (OSError, ValueError):
                time.sleep(0.05)
                continue
            ag, lb = st.get("agent") or {}, st.get("lab") or {}
            say = ag.get("say") or {}
            if say.get("text") and say.get("asked", "").strip() == sentence:
                mark("first words", ": %s" % say["text"][:60])
                if say.get("done") and not ag.get("busy"):
                    mark("the answer done", ": %s" % say["text"])
            comp = ag.get("computing") or []
            if comp:
                name = comp[0].get("name", name)
                mark("the run started", " (%s)" % name)
            if lb.get("growing") and name and lb.get("path", "").endswith(name + ".lab"):
                mark("shown while it computes", " (%d frames)" % lb.get("nframes", 0))
            if name and time.time() >= next_look:  # every 2 s: the file, the app, the run
                next_look = time.time() + 2
                pidf = lab / "agent-runs" / (name + ".pid")
                try:
                    run_alive = os.kill(int(pidf.read_text().split()[0]), 0) is None
                except (OSError, ValueError):
                    run_alive = False
                look = (frames_in(lab / (name + ".lab")), lb.get("nframes"), round(time.time() - (xr / "state.json").stat().st_mtime, 1),
                        app.p.poll() is None, run_alive, lb.get("growing"), os.path.basename(lb.get("path", "")))
                if look[:2] != last_look[:2] or not look[3] or look[2] > 5:
                    print("  %6.2f s  file %s frames, app shows %s; state %.1f s old; app alive %s, run alive %s, growing %s, %s"
                          % ((time.time() - t0,) + look))
                last_look = look
            if "shown while it computes" in marks and not lb.get("growing") and lb.get("path", "").endswith(name + ".lab"):
                mark("the run done", " (%d frames)" % lb.get("nframes", 0))
                break
            for s in ag.get("stopped") or []:
                mark("the run stopped", ": %s" % s.get("why"))
            if "the run stopped" in marks:
                break
            time.sleep(0.1)
        if "the run done" not in marks:  # not seen to its end: the logs' ends, and the folders kept for a look
            keep = True
            for log in (tmp / "app.log", tmp / "bridge.log", lab / "agent-runs" / (name + ".log")):
                try:
                    print("--- the end of %s:\n%s" % (log, "\n".join(log.read_text(errors="replace").splitlines()[-25:])))
                except OSError:
                    pass
        records = sorted((tmp / "runs").glob("*.json"))
        if records:
            rec = json.loads(records[-1].read_text())
            print("the agent's record: model %s, cost %s USD, %s turns" % (
                (rec.get("model") or {}).get("version") or (rec.get("model") or {}).get("name"), rec.get("cost_usd"), rec.get("turns")))
        made = sorted(p.stem for p in (lab / "agent-runs").glob("*.json"))
        for m in made:
            sc2 = json.loads((lab / "agent-runs" / (m + ".json")).read_text())
            print("the scenario it made: %s, burner %s W (was %s W), changed_from %s" % (
                m, (sc2.get("burner") or {}).get("heat_release_rate_w"), sc["burner"]["heat_release_rate_w"], sc2.get("changed_from")))
        return 0
    finally:
        for p in procs:
            p.stop()
        for f in (home / "OpenPhysicsAI-Projects" / "lab" / "agent-runs").glob("*.pid"):  # a run still going ends with this
            try:
                os.kill(int(f.read_text().split()[0]), 15)
            except (OSError, ValueError):
                pass
        if keep:
            print("kept for a look: %s and %s" % (tmp, home))
        else:
            shutil.rmtree(tmp, ignore_errors=True)
            shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
