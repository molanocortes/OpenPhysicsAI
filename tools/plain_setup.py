#!/usr/bin/env python3
"""plain_setup.py - G17 step 6 (GOALS.md): a person points at a part, says how it is used, sees the setup the AI made of
it in colour on the part, confirms it, and gets the answer.

Criteria, written in GOALS.md before the first run (2026-10-08):
 a. Pointing: in the Agentic window with a part open and no result shown, a click on the part picks the face under it,
    and the next question names the pointed patches, in the order picked, with their area; the patches are the faces
    under the clicked points, and the selections the agent makes of them have the area the question states (1e-6).
 b. Showing: the geometry view colours held faces violet and loaded faces gold-brown; `fem where` reports both faces as
    held and loaded once the agent has applied its conditions.
 c. Confirming: setup_confirm puts the agent's summary on a card (Run it, Not right) and returns the answer; Run it
    returns confirmed and only then is an analysis submitted; Not right returns not confirmed and none is submitted.
 d. The answer: after Run it, the printed-part analysis ends with its sentence, which the window shows as the agent's
    words.

No AI is started: the agent is a stand-in script that talks to the window's engine through openphysics-mcp, as Claude Code
does, with the window's own MCP configuration. The part is the L bracket of the breaks kit. The window runs off-screen;
the workspace is deleted when every check passes.

    make openphysics openphysics-mcp && python3 tools/plain_setup.py
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "openphysics"
PART = ROOT / "validation" / "breaks" / "parts" / "bracket.stl"
results = []


def check(ok, what, detail=""):
    results.append(bool(ok))
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok and detail:
        print("       " + detail[:700].replace("\n", "\n       "))
    return ok


SUMMARY = ("Held: screwed to the wall on the face you pointed at first. Load: a 5 kg bag, 49 N, pulling down on the face you "
           "pointed at second.")
STANDIN = r'''import json, os, re, sys, time
sys.path.insert(0, %(tools)r)
from mcptest import Client
out = sys.argv[1]
prompt = os.environ.get("OPENPHYSICS_PROMPT", "")
run = 2 if os.path.exists(os.path.join(out, "run1.json")) else 1
open(os.path.join(out, "prompt%%d.txt" %% run), "w").write(prompt)
cfg = json.load(open(os.environ["OPENPHYSICS_MCP_CONFIG"]))["mcpServers"]["openphysics"]
c = Client(cfg["args"])
c.initialize()
def call(n, a):
    r = c.call(n, a)
    sc = r.get("result", {}).get("structuredContent") or {}
    if not sc.get("ok"):
        json.dump({"run": run, "failed": n, "error": sc.get("error")}, open(os.path.join(out, "run%%d.json" %% run), "w"))
        raise SystemExit("%%s failed: %%s" %% (n, json.dumps(sc.get("error"))[:300]))
    return sc.get("value", {})
res = {"run": run}
if run == 1:
    m = re.search(r"surface patches ([0-9, ]+), ([0-9.]+) mm2", prompt)
    ids = [int(x) for x in m.group(1).split(",")] if m else []
    res["patches"], res["pointed_mm2"] = ids, float(m.group(2)) if m else None
    area = 0.0
    for name, pid in (("held", ids[0]), ("bag", ids[1])):
        v = call("selection_create", {"name": name, "query": {"patches": [pid]}, "source": "user"})
        area += (v.get("selection") or v).get("area_mm2", 0.0)
    res["selected_mm2"] = area
    call("boundary_apply", {"name": "wall", "kind": "fixed", "selection": "held", "source": "user"})
    call("boundary_apply", {"name": "bag", "kind": "force", "selection": "bag", "force": [0, "-49.03325 N", 0], "source": "user"})
    open(os.path.join(out, "applied"), "w").write("yes")
t0 = time.time()
v = call("setup_confirm", {"summary": %(summary)r})
res["confirmed"], res["waited_s"], res["answered_at"] = v["confirmed"], time.time() - t0, time.time()
if v["confirmed"]:
    j = call("analysis_run", {"label": "plain setup", "material_model": {"type": "orthotropic", "library": "pla_fff"}})
    res["job"], res["submitted_at"] = j["job_id"], time.time()
    st = {}
    for _ in range(120):
        st = call("job_status", {"job_id": j["job_id"], "wait_seconds": 10})
        if st.get("state") not in ("queued", "running"):
            break
    A = ((st.get("summary") or {}).get("strength") or {}).get("answer") or {}
    res["state"], res["answer"] = st.get("state"), A.get("text", "")
    print(res["answer"])
else:
    print("You said the setup is not right: tell me what to change.")
c.close()
json.dump(res, open(os.path.join(out, "run%%d.json" %% run), "w"))
'''


def controls(section):
    return re.findall(r"\[data\]\s+\w+\s+.*?id=(.+?)\s+\(\s*-?\d+,", section)


def main():
    print("== G17 step 6: pointing, the setup in colour, confirming, the answer")
    if not APP.exists() or not (ROOT / "openphysics-mcp").exists():
        print("  build first: make openphysics openphysics-mcp")
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-g17s6-", dir="/tmp"))  # short: the agent's socket lives below HOME
    (tmp / "home").mkdir()
    out = tmp / "out"
    out.mkdir()
    (tmp / "standin.py").write_text(STANDIN % {"tools": str(ROOT / "tools"), "summary": SUMMARY})
    # the bracket is placed centred in x and y, its foot at z = 0: part (0..45, 0..70, 0..15) mm -> (-22.5.., -35.., 0..)
    back = (-22.5, 22.0, 7.5)   # the back plate's face against the wall (part x = 0), in its clamped part
    arm = (12.5, -30.5, 7.5)    # the arm's top (part y = 4.5), near the lip
    where = lambda p, extra="": "fem where %g %g %g%s" % (p[0], p[1], p[2], extra)
    script = [
        "hud simple", "mode agentic", "workspace solid", "frames 4",
        """am project_create '{"name": "g17s6"}'""",
        """am geometry_import '{"path": "%s", "units": "mm", "name": "bracket"}'""" % PART,
        """am material_assign '{"body": "bracket", "material": "pla_fff", "source": "user"}'""",
        """am mesh_generate '{"element_size": "1 mm"}'""",
        "frames 20",
        "agent command python3 %s %s" % (tmp / "standin.py", out),
        "echo G17S6 before", where(back), where(arm),
        where(back, " click"), "frames 12", where(arm, " click"), "frames 12",
        "echo G17S6 pointed", where(back), where(arm),
        'uiclick "text:agent"', "frames 4",
        "uikeys it is screwed to the wall here and it holds a 5 kg bag here\\\\n", "frames 90",  # the console takes one backslash
        "echo G17S6 card", "uilist", where(back), where(arm), "screenshot %s" % (tmp / "card.png"),
        'uiclick "setup run"', "frames 10", "agent wait 120", "frames 30",
        "echo G17S6 answered", "screenshot %s" % (tmp / "answered.png"),
        'uiclick "text:agent"', "frames 4",
        "uikeys check it again\\\\n", "frames 90",
        "echo G17S6 second card", "uilist",
        'uiclick "setup not right"', "frames 10", "agent wait 60", "frames 10",
        "echo G17S6 done", "quit"]
    (tmp / "s6.nav").write_text("\n".join(script) + "\n")
    env = dict(os.environ, HOME=str(tmp / "home"), OPENPHYSICS_AGENT_RUNS_DIR=str(tmp / "runs"))
    run = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--workspace", str(tmp / "ws"), "--exec", "exec %s" % (tmp / "s6.nav")],
                         capture_output=True, text=True, timeout=600, cwd=str(ROOT), env=env)
    log = run.stdout + run.stderr
    (tmp / "app.log").write_text(log)
    check("G17S6 done" in log, "the script ran to the end in the window")
    sec = {}
    for part in re.split(r"\[info\] G17S6 ", log)[1:]:
        name, _, body = part.partition("\n")
        sec[name.strip()] = body

    def wheres(text):
        return [(int(m.group(1)), m.group(2)) for m in re.finditer(r"on face patch (-?\d+) \((\w+)\)", text)]

    before, pointed, card = wheres(sec.get("before", "")), wheres(sec.get("pointed", "")), wheres(sec.get("card", ""))
    r1 = json.loads((out / "run1.json").read_text()) if (out / "run1.json").exists() else {}
    r2 = json.loads((out / "run2.json").read_text()) if (out / "run2.json").exists() else {}
    p1 = (out / "prompt1.txt").read_text() if (out / "prompt1.txt").exists() else ""
    faces = [b[0] for b in before[:2]]
    # a. pointing
    check(len(faces) == 2 and faces[0] >= 0 and faces[1] >= 0 and faces[0] != faces[1],
          "the back face and the arm's top are two faces in the view (patches %s)" % faces)
    check(r1.get("patches") == faces, "the question named the two pointed patches in the order clicked (%s, against %s)" % (r1.get("patches"), faces),
          p1[-500:])
    pa, sa = r1.get("pointed_mm2"), r1.get("selected_mm2")
    check(pa and sa and abs(sa / pa - 1) <= 1e-6, "the selections made of them have the area the question states (%s mm2, %s mm2)" % (sa, pa))
    # b. showing
    check([c[1] for c in card[:2]] == ["held", "loaded"], "with the conditions applied the back face is drawn held and the arm loaded (%s)" % card[:2])
    check([c[1] for c in before[:2]] == ["plain", "plain"], "and before them both were plain (%s)" % before[:2])
    # c. confirming
    ids = set(controls(sec.get("card", "")))
    check({"setup run", "setup not right"} <= ids, "the setup card is shown with Run it and Not right while the agent waits (%s)" %
          ", ".join(sorted(i for i in ids if "setup" in i or "approval" in i)))
    check("agent: confirmed the setup: " + SUMMARY[:60] in log, "the card carries the agent's summary and the window logs the confirmation")
    check(r1.get("confirmed") is True and r1.get("job") and r1.get("submitted_at", 0) >= r1.get("answered_at", 1e18),
          "Run it returned confirmed, and the analysis was submitted after the answer (%s)" % r1.get("job"))
    jobs_before_second = len(re.findall(r"static_structural submitted as job-", log.split("G17S6 second card")[0]))
    jobs_all = len(re.findall(r"static_structural submitted as job-", log))
    check(r2.get("confirmed") is False and "job" not in r2 and jobs_all == jobs_before_second and "agent: said the setup is not right" in log,
          "Not right returned not confirmed and no analysis was submitted (%d analyses in all)" % jobs_all)
    # d. the answer
    ans = r1.get("answer", "")
    check(r1.get("state") == "succeeded" and ans.startswith("Holds about") and ("agent: " + ans[:50]) in log,
          "the analysis succeeded and the window shows its sentence: \"%s\"" % ans[:160])
    ok = all(results)
    if "--shots" in sys.argv:  # keep the two screenshots for a look: --shots DIR
        dst = Path(sys.argv[sys.argv.index("--shots") + 1])
        dst.mkdir(parents=True, exist_ok=True)
        for name in ("card.png", "answered.png"):
            if (tmp / name).exists():
                shutil.copy(tmp / name, dst / ("plain-setup-" + name))
    if ok:
        shutil.rmtree(tmp)
    else:
        print("  kept for diagnosis: %s (app.log, s6.nav, out/, card.png)" % tmp)
    print("\nPLAIN-LANGUAGE SETUP (G17 step 6): %d passed, %d failed" % (sum(results), len(results) - sum(results)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
