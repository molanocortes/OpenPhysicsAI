#!/usr/bin/env python3
"""minimal_window.py - G18 (GOALS.md): the window a person sees is minimal and mostly agentic.

Criteria, written in GOALS.md before the work (2026-10-07):
 1. Besides the 3D view and the legend, the window draws at the top the name and the Agentic | Manual switch; in
    Manual exactly two buttons, Open and Run; in Agentic one text field and one round button (the microphone, or stop
    while the agent works), with the agent's latest words above them, fading. Checked here by the controls each screen
    registers (`uilist`), on the gallery, a lab result, the flow tunnel and a solid part.
 2. Run does what fits what is open (a lab simulation runs, the flow starts and pauses, the open part is solved);
    Open shows the gallery. Checked by real clicks.
 3. Voice: a press on the microphone starts listening, a second press sends the words as the question. Checked through
    the real click path with a stand-in recogniser (OPENPHYSICS_VOICE_HELPER) that answers a fixed sentence when stopped.
 4. The AI's request to use a tool shows its Allow and Deny card: a stand-in agent files a request and waits for the
    answer; Allow is clicked. The full controls stay behind the hidden `hud controls` command.
 5. The gallery: pictures with one line each, your own part and the saved work; no search field or featured picture.

No AI is started: the agent is a stand-in script. The window runs off-screen; the workspace is deleted when every
check passes.

    make openphysics && python3 tools/minimal_window.py
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "openphysics"
results = []


def check(ok, what, detail=""):
    results.append(bool(ok))
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok and detail:
        print("       " + detail[:600].replace("\n", "\n       "))
    return ok


def controls(section):
    """the widget ids a `uilist` printed in one section of the log"""
    return re.findall(r"\[data\]\s+\w+\s+.*?id=(.+?)\s+\(\s*-?\d+,", section)


SENTENCE = "make the wall of the bracket two millimetres thicker"
# a shell script: it is listening within a millisecond of starting, as the real helper is long before a person stops
MIC = '''#!/bin/sh
trap 'echo "%s"; exit 0' INT TERM
sleep 60 &
wait $!
exit 1
''' % SENTENCE
AGENT = '''import json, os, sys, time
d = os.environ.get("OPENPHYSICS_APPROVALS", "")
req = os.path.join(d, "0001-standin.req")
with open(req + ".tmp", "w") as f:
    json.dump({"tool_name": "Bash", "input": {"command": "echo stand-in"}}, f)
os.rename(req + ".tmp", req)
ans = os.path.join(d, "0001-standin.ans")
for _ in range(600):
    if os.path.exists(ans):
        open(sys.argv[1], "w").write(open(ans).read().strip())  # what the window answered, for the test to read
        sys.exit(0)
    time.sleep(0.1)
'''


def main():
    print("== G18: the minimal, agentic window")
    if not APP.exists():
        print("  build the app first: make openphysics")
        return 2
    # under /tmp: the agent's channel is a Unix socket below HOME, and its path must stay under 104 bytes
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-g18-", dir="/tmp"))
    (tmp / "home").mkdir()
    (tmp / "mic.sh").write_text(MIC)
    (tmp / "mic.sh").chmod(0o755)
    (tmp / "agent.py").write_text(AGENT)
    script = [
        "hud simple", "mode manual", "frames 12",
        "echo G18 tunnel manual", "uilist",
        'uiclick "Run##manual"', "frames 20", "echo G18 tunnel running", "uilist", "status",
        'uiclick "Run##manual"', "frames 10", "echo G18 tunnel paused", "status",
        'uiclick "Open##manual"', "frames 20", "echo G18 gallery manual", "uilist",
        "mode agentic", "frames 12", "echo G18 gallery agentic", "uilist",
        "mode manual", "frames 6",
        'uiclick "lib shoebox_hall"', "frames 60", "echo G18 lab manual", "uilist",
        "echo G18 lab run", 'uiclick "Run##manual"', "frames 20",
        "lab close", "workspace solid", "frames 4",
        """am project_create '{"name": "g18_part"}'""",
        """am geometry_import '{"path": "samples/bracket.stl", "units": "mm", "name": "bracket"}'""",
        """am material_assign '{"body": "bracket", "material": "pla_generic_demo", "source": "user"}'""",
        """am mesh_generate '{"element_size": "1 mm"}'""",
        """am selection_create '{"name": "foot", "query": {"all": [{"facing": {"direction": "down"}}, {"plane": {"axis": "z", "at": "min"}}]}, "source": "user"}'""",
        """am selection_create '{"name": "top", "query": {"extreme": {"direction": "+z"}}, "source": "user"}'""",
        """am boundary_apply '{"name": "hold", "kind": "fixed", "selection": "foot", "source": "user"}'""",
        """am boundary_apply '{"name": "push", "kind": "force", "selection": "top", "force": ["20 N", 0, 0], "source": "user"}'""",
        "mode manual", "frames 10", "echo G18 solid manual", "uilist",
        "echo G18 solid run", 'uiclick "Run##manual"', "frames 20",
        "mode agentic", "frames 12", "echo G18 lab agentic", "uilist",
        "agent command python3 %s %s" % (tmp / "agent.py", tmp / "answer.txt"),
        'uiclick "MIC##agent"', "frames 30", "echo G18 listening", "uilist",
        'uiclick "MIC##agent"', "frames 60", "echo G18 asked", "uilist", "screenshot %s" % (tmp / "asked.png"),
        'uiclick "approval allow"', "frames 30", "agent wait 30", "frames 10", "echo G18 answered",
        "mode manual", "hud controls", "frames 10", "echo G18 controls", "uilist",
        "echo G18 done", "quit"]
    (tmp / "g18.nav").write_text("\n".join(script) + "\n")
    # the window's own home: lab results and settings land in the test's folder, never the owner's
    env = dict(__import__("os").environ, OPENPHYSICS_VOICE_HELPER=str(tmp / "mic.sh"), HOME=str(tmp / "home"),
               OPENPHYSICS_AGENT_RUNS_DIR=str(tmp / "runs"))  # the stand-in's run record stays out of the repository
    run = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--workspace", str(tmp / "ws"), "--exec", "exec %s" % (tmp / "g18.nav")],
                         capture_output=True, text=True, timeout=600, cwd=str(ROOT), env=env)
    log = run.stdout + run.stderr
    (tmp / "app.log").write_text(log)
    check("G18 done" in log, "the script ran to the end in the window")
    parts = re.split(r"\[info\] G18 ", log)
    sec = {}
    for p in parts[1:]:
        name, _, body = p.partition("\n")
        sec[name.strip()] = body
    top = {"SIMULATIONS##labnav", "Agentic##mode", "Manual##mode"}
    legend = {"focus field", "legend", "line colours"}  # the legend's own click areas: the next field, palette or line colours
    manual = top | {"Open##manual", "Run##manual"}
    for key in ("tunnel manual", "lab manual", "solid manual"):
        ids = set(controls(sec.get(key, "")))
        check(ids and ids - legend == manual, "%s: exactly the name, the switch, Open and Run (%s)" % (key, ", ".join(sorted(ids))))
    ids = set(controls(sec.get("lab agentic", "")))
    check(ids and ids - legend == top | {"text:agent", "MIC##agent"}, "lab agentic: the name, the switch, one text field and the microphone (%s)" %
          ", ".join(sorted(ids)))
    for key, bar in (("gallery manual", set()), ("gallery agentic", {"text:agent", "MIC##agent"})):
        ids = set(controls(sec.get(key, "")))
        cards = {i for i in ids if i.startswith("lib ")}
        rest = ids - cards - top - bar - {"own part", "SAVED##simtab", "PREV SCENARIOS##library", "NEXT SCENARIOS##library"}
        check(len(cards) >= 6 and not rest and bar <= ids and "simsearch" not in ids,
              "%s: pictures (%d), your part, saved%s, nothing else%s" % (key, len(cards), " and the bar" if bar else "",
                                                                    "" if not rest else " (also: %s)" % ", ".join(sorted(rest))))
    st_run, st_pause = sec.get("tunnel running", ""), sec.get("tunnel paused", "")
    check("Pause##manual" in st_run or re.search(r"running|step \d+", st_run) is not None,
          "Run starts the flow", st_run[-400:])
    check(re.search(r"paused|pause", st_pause, re.I) is not None, "a second press pauses it", st_pause[-400:])
    check(any(i.startswith("lib ") for i in controls(sec.get("gallery manual", ""))), "Open shows the gallery")
    check(re.search(r"lab: .*(run|comput)", sec.get("lab run", ""), re.I) is not None or "Running" in sec.get("lab run", ""),
          "Run runs the lab simulation", sec.get("lab run", "")[-400:])
    check(re.search(r"static_structural submitted as job-", sec.get("solid run", "")) is not None, "Run solves the open part",
          sec.get("solid run", "")[-400:])
    ids = set(controls(sec.get("listening", "")))
    check("MIC##agent" in ids, "the microphone is listening after one press")
    check("agent: asked: %s" % SENTENCE in log, "a second press sends the words as the question (\"%s\")" % SENTENCE, log[-800:])
    ids = set(controls(sec.get("asked", "")))
    check("approval allow" in ids and "approval deny" in ids and "STOP##agent" in ids,
          "the stand-in agent's request shows Allow and Deny, and stop replaces the microphone while it works (%s)" % ", ".join(sorted(ids)))
    answer = (tmp / "answer.txt").read_text() if (tmp / "answer.txt").exists() else ""
    check(answer == "allow" and "agent: allowed Bash" in log, "Allow reaches the agent (it received \"%s\")" % answer)
    ids = set(controls(sec.get("controls", "")))
    check(len(ids) > 20, "the hidden `hud controls` command still opens the full controls (%d widgets)" % len(ids))
    ok = all(results)
    if ok:
        shutil.rmtree(tmp)
    else:
        print("  kept for diagnosis: %s (app.log, g18.nav)" % tmp)
    print("\nMINIMAL WINDOW (G18): %d passed, %d failed" % (sum(results), len(results) - sum(results)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
