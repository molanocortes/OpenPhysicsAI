#!/usr/bin/env python3
"""xr_test.py - the headset link end to end (docs/xr.md), without a headset: the app, the bridge, the page.

    python3 tools/xr/xr_test.py [--no-page]   (make test-xr; about a minute; --no-page leaves the browser part out)

It starts its own headless app (OPENPHYSICS_XR=1, its own folder) and its own bridges on free ports, never the owner's;
everything is removed when it passes. No AI is ever started: questions are sent only to a bridge with no app behind it.

Criteria, written before the first run (2026-10-07):
 1. The view commands: the bridge's rule and the app's own (src/xr.c, through its real inbox: one cmd file per line,
    the app's log says "headset: <line>" or "headset: refused") agree with tools/xr/commands.json on every line.
 2. The lab link, with the app showing the synthetic result of build/labxrtest --write (three frames, every kind of
    part): /state names the lab result, its frames and its view key; /lab/info is the result's (3 frames, 6 parts);
    /lab/frame.glb for every frame passes xr_check.py and is byte for byte what build/labxr writes with the view of the
    state; a stale key gets 409 and a frame outside the result 400; a view command through /command reaches the app
    (its frame and section change, and the view key with the section); a refused command gets 400 and no file.
 3. Questions and answers, on a bridge with no app behind it: /ask writes one ask file holding the words and, after a
    blank line, "(In the headset: <context>)"; a refused body gets 400; an answer in the state is spoken: /speech.wav
    for its number is a RIFF WAVE of at least half a second, and the next answer replaces it.
    Amended 2026-10-07, when answers came to be spoken sentence by sentence as they stream (tools/xr/talk_test.py
    criterion 3) and /speech.wav by answer number gave way to /speech by sentence: an answer in the state becomes a
    sentence in the state's speech, /speech for it is a RIFF WAVE of at least half a second, its markdown is gone and
    its unit is said in words ("megapascals"); the next answer is a new sentence after it.
 4. A finite-element scene, when the project Thermal_Corolla is on this computer: the app's scene.glb passes
    xr_check.py against the part's STL, carries one value per vertex and a to_source matrix in mm.
 5. The page (tools/xr/pagetest.mjs, headless Chrome through the DevTools protocol, real mouse events and synthetic
    controller events), when Node 22 and Chrome are here: it loads every frame; what it draws of each frame is what
    the frame files hold (triangles, particles, line segments, volume); a click on a field chip changes the app's
    field and the page draws every frame again; the value read by pointing at the block is its stored field there
    (T is linear in position: 6 (x - 0.1 k) + 1.5 y + 3 z + k, to 1e-4); a click pins it and the AI's context names
    the pin; play, pause, next, the time slider and the cut on the card reach the app and the page follows; a
    controller aimed at the card presses Off and picks a field, a controller tap on the result pins a value; no error
    in the page. Added the same day, before its first run: the voice the page records is 16 kHz mono 16-bit WAV, and
    the resampler used where the browser will not record at 16 kHz passes a 1 kHz tone recorded at 48 kHz within
    0.5 dB and keeps a 12 kHz tone (which would fold to 4 kHz) at least 40 dB down.
 3b. Added 2026-10-07 before its first run: the bridge refuses what a page from another site, or a DNS-rebinding name,
    could send it: a foreign Host header (403), a foreign Origin (403), a POST that is not application/json (415, the
    kind a browser sends across sites without asking first); no ask file is written for any of them.
 7. The headset's own code (added 2026-10-07, written before its first automated run), in the page with the stand-in
    WebXR runtime tools/xr/web/test/fakexr.js (?fakexr=1; it emulates no Quest, it makes the code run): Enter AR presents
    in passthrough; the result is placed 0.5 m in front of the head and 0.42 m below the eyes, the card to its left
    facing the head; a controller pointed at the block reads the block; the thumbstick makes it bigger; B gives true size
    and, the result being wider than 2 m, stands it on the floor (y 0), and B again puts it back on the table; A and a
    found surface place it there, facing the head; two hands moving from 0.2 m to 0.8 m apart scale it four times (within
    10 %); the card turns toward a head that has walked a quarter of the way round; ending the session gives the studio
    back; no error in the page through all of it.
 6. The AI's headset tools (added 2026-10-07, written before their first run), through openphysics-mcp over MCP with the
    test app's folder: tools/list offers headset_status, headset_view and headset_mark; headset_view sends an allowed
    command, which the app takes (its frame changes), and refuses one the rule refuses, which never reaches the folder;
    headset_mark writes a mark for the result shown, and the state the page reads carries it; headset_status says the app
    runs, names the result and returns the mark, and, after the page test, the two points the person pinned there (part
    'block', places in m). In the page (part 5): the mark is drawn with its label; plain commands typed in the question
    box (real key events) are done at once and reach the app: "next frame", "show the temperature" (the field T),
    "cut along z" and "remove the cut"; the command grammar maps its sample sentences and leaves questions to the AI.
 8. What the person sees, for the AI: openphysics-mcp headset_look (added 2026-10-07, written before its first run).
    In part 6: tools/list offers it; with no page answering it fails within 7 s and leaves no request in the folder;
    with a stand-in page (it reads the request in /state and posts a JPEG to /look) it returns that JPEG as an image
    (image/jpeg, the same bytes) made now, with its view; a picture posted at a question is the one moment "question"
    returns; the bridge refuses a /look that is not image/jpeg (415), not a whole JPEG (400), with a bad id (400) or
    from another site's page (403), and writes no picture for any of them. In the page (part 5), through the real
    headset_look over MCP: the page answers with a 960 x 720 JPEG; its background is the studio page's colour; at the
    block's place in it the colour is the colour the window shows at the block's place on the screen (each channel
    within 12 of 255: JPEG); a picture taken as a question is asked is what moment "question" returns. In the headset
    (part 7): taken from the head (looking 30 degrees down at the result placed 0.5 m in front and 0.42 m below the
    eyes), the picture says AR, the block lies in its lower half and is not background there, and where passthrough
    would show the room the picture has the studio colour, never the camera's view (the page has none) nor black.
 9. How the headset runs (added 2026-10-07, written before its first run). On the bridge (part 8): /health keeps a JSON
    object's known keys only, refuses anything else (400), and when a report that is not presenting follows one that
    is, the presenting one becomes a line of xr-sessions.jsonl beside the headset folder. In the headset (part 7): within
    6 s of entering AR the page's report says presenting, mode immersive-ar, a frame rate above 0 measured from its own
    frames, hit-test among the session's features, and names a GPU; an error thrown in the page is in the report within
    3 s and in headset_status; after the session ends the report says it is not presenting and the session's last
    report is a line of xr-sessions.jsonl with mode immersive-ar and its frames.
10. Talking in the page, with the minimal look (added 2026-10-07, written before its first automated run; the test app's
    agent is the stand-in for Claude Code, tools/xr/claude_standin.py: no model). (a) The voice activity detector
    (tools/xr/web/vad.js) on synthetic sound: a 0.8 s voice 25 dB above a noise floor gives one sentence, 0.8 s plus
    at most 0.5 s of lead-in and tail; a 50 ms click gives none, nor does noise alone; while the AI speaks, its own voice
    15 dB above the floor gives none and a person 25 dB above gives one. (b) The page opens as the window does: no
    controls card, the legend, one bar with one round button; Manual shows the card (the card's checks of part 5 run
    under Manual). (c) Words recognised in the page (given as if heard) reach the AI and its answer streams into the
    words line; while it answers the round button is stop, and pressing it ends the answer: the bridge's speech says
    stopped and the line says so. (d) In the headset (part 7): the dock stands to the result's right facing the head
    and the card is hidden; a controller pointed at the round button presses it (listening, or stop while the AI
    answers); the AI's words in the dock fade to nothing within 10.5 s of the answer's end.
"""
import base64, json, os, pathlib, shutil, socket, struct, subprocess, sys, tempfile, threading, time, urllib.error, urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "xr"))
import xr_bridge  # noqa: E402  (its command rule, the same module the bridge runs)

failures = 0
# a 32 x 24 JPEG (made once with macOS sips), the stand-in page's picture in part 8
TEST_JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAASABIAAD/4QCARXhpZgAATU0AKgAAAAgABAEaAAUAAAABAAAAPgEbAAUAAAABAAAARgEoAAMAAAABAAIAAIdpAAQAAAAB"
    "AAAATgAAAAAAAABIAAAAAQAAAEgAAAABAAOgAQADAAAAAQABAACgAgAEAAAAAQAAACCgAwAEAAAAAQAAABgAAAAA/+0AOFBob3Rvc2hvcCAzLjAA"
    "OEJJTQQEAAAAAAAAOEJJTQQlAAAAAAAQ1B2M2Y8AsgTpgAmY7PhCfv/AABEIABgAIAMBIgACEQEDEQH/xAAfAAABBQEBAQEBAQAAAAAAAAAAAQID"
    "BAUGBwgJCgv/xAC1EAACAQMDAgQDBQUEBAAAAX0BAgMABBEFEiExQQYTUWEHInEUMoGRoQgjQrHBFVLR8CQzYnKCCQoWFxgZGiUmJygpKjQ1Njc4"
    "OTpDREVGR0hJSlNUVVZXWFlaY2RlZmdoaWpzdHV2d3h5eoOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX"
    "2Nna4eLj5OXm5+jp6vHy8/T19vf4+fr/xAAfAQADAQEBAQEBAQEBAAAAAAAAAQIDBAUGBwgJCgv/xAC1EQACAQIEBAMEBwUEBAABAncAAQIDEQQF"
    "ITEGEkFRB2FxEyIygQgUQpGhscEJIzNS8BVictEKFiQ04SXxFxgZGiYnKCkqNTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqC"
    "g4SFhoeIiYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2dri4+Tl5ufo6ery8/T19vf4+fr/2wBDAAICAgICAgMC"
    "AgMEAwMDBAUEBAQEBQcFBQUFBQcIBwcHBwcHCAgICAgICAgKCgoKCgoLCwsLCw0NDQ0NDQ0NDQ3/2wBDAQICAgMDAwYDAwYNCQcJDQ0NDQ0NDQ0N"
    "DQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ3/3QAEAAL/2gAMAwEAAhEDEQA/ANqO796vx3fvXFR3fvV6O796/I+G8Hto"
    "ft9biXzO1ju/er8d371xUd371eju/ev6A4bwe2h5VbiXzP/Q4yO796vx3fvWBH1q/H0r4fhulHQutjq3c347v3q9Hd+9YEdX4+tf0Bw3SjoeVWx1"
    "buf/2Q==")


def check(ok, what):
    global failures
    print(("  ok   " if ok else "  FAIL ") + what, flush=True)
    failures += 0 if ok else 1
    return ok


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def http(url, data=None, ctype="application/json"):
    req = urllib.request.Request(url, data=data, headers={"Content-Type": ctype} if data is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def wait(cond, seconds=20, step=0.1):
    end = time.time() + seconds
    while time.time() < end:
        try:
            v = cond()
            if v:
                return v
        except Exception:
            pass
        time.sleep(step)
    return None


def glb_counts(b):
    """triangles, particles, line segments and the volume of a scene file, read independently of the page"""
    jl = struct.unpack_from("<I", b, 12)[0]
    g = json.loads(b[20:20 + jl])
    acc, tri, pts, seg = g.get("accessors", []), 0, 0, 0
    for m in g.get("meshes", []):
        for p in m["primitives"]:
            n = acc[p["indices"]]["count"] if "indices" in p else acc[p["attributes"]["POSITION"]]["count"]
            mode = p.get("mode", 4)
            tri, pts, seg = tri + (n // 3 if mode == 4 else 0), pts + (n if mode == 0 else 0), seg + (n // 2 if mode == 1 else 0)
    v = g["nodes"][0]["extras"]["result"].get("volume")
    return {"triangles": tri, "particles": pts, "segments": seg, "volume": ("x".join(map(str, v["dims"])) + " " + v["mode"]) if v else None}


class Proc:
    def __init__(self, argv, log, env=None):
        self.log = log
        self.p = subprocess.Popen(argv, cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT)

    def text(self):
        return pathlib.Path(self.log).read_text(errors="replace")

    def stop(self):
        self.p.kill()
        self.p.wait()


def main():
    global failures
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-xrtest-"))
    procs = []
    try:
        for exe in ("openphysics", "build/labxr", "build/labxrtest"):
            if not (ROOT / exe).exists():
                print("missing %s: make openphysics build/labxr build/labxrtest" % exe)
                return 1
        result = tmp / "every.lab"
        subprocess.run([str(ROOT / "build" / "labxrtest"), "--write", str(result)], check=True)
        xr = tmp / "xr"
        xr.mkdir()
        env = dict(os.environ, OPENPHYSICS_XR="1", OPENPHYSICS_XR_DIR=str(xr))
        standin = ROOT / "tools" / "xr" / "claude_standin.py"  # the agent, for the page's conversation (criterion 10): no model
        env = dict(env, OPENPHYSICS_AGENT_RUNS_DIR=str(tmp / "runs"), STANDIN_LOG=str(tmp / "standin.log"))
        (tmp / "runs").mkdir()
        app = Proc([str(ROOT / "openphysics"), "--headless", "--size", "480x320", "--exec",
                    'lab open %s; lab pause; agent command "%s" -p $OPENPHYSICS_PROMPT --mcp-config $OPENPHYSICS_MCP_CONFIG --output-format '
                    'stream-json --verbose; frames 100000000' % (result, standin)], str(tmp / "app.log"), env)
        procs.append(app)
        state = lambda: json.loads((xr / "state.json").read_text())  # noqa: E731
        check(wait(lambda: state().get("scene") == "lab", 30) is not None, "the app shows the synthetic result and writes the headset's state")

        print("1. view commands")
        vec = json.loads((ROOT / "tools" / "xr" / "commands.json").read_text())
        bad = [c for c in vec["allowed"] if not xr_bridge.command_allowed(c)] + [c for c in vec["refused"] if xr_bridge.command_allowed(c)]
        check(not bad, "the bridge's rule agrees with commands.json on %d lines%s" % (len(vec["allowed"]) + len(vec["refused"]), (": " + repr(bad[:4])) if bad else ""))
        lines = [(c, True) for c in vec["allowed"]] + [(c, False) for c in vec["refused"] if c and "\n" not in c]
        for i, (c, _) in enumerate(lines):  # named so that the app takes them in this order
            (xr / ("cmd-%020d.txt" % i)).write_text(c + "\n")
        check(wait(lambda: not list(xr.glob("cmd-*.txt")), 30) is not None, "the app takes every command file from its inbox")
        log = app.text()
        disagree = []
        for c, allowed in lines:
            said_ok, said_no = ("[info] headset: %s\n" % c) in log, ("headset: refused '%s'" % c[:80]) in log
            if (allowed and not said_ok) or (not allowed and not said_no):
                disagree.append(c)
        check(not disagree, "the app's rule agrees with commands.json on %d lines that fit in a file%s" % (len(lines), (": " + repr(disagree[:4])) if disagree else ""))
        for i, c in enumerate(["lab pause", "lab frame 0", "lab field stress", "lab section off", "lab range auto"]):
            (xr / ("cmd-%020d.txt" % (1000 + i))).write_text(c + "\n")
        wait(lambda: not list(xr.glob("cmd-*.txt")) and state()["lab"]["view"]["field"] == "stress" and state()["lab"]["frame"] == 0, 20)

        print("2. the lab link")
        port = free_port()
        bridge = Proc([sys.executable, str(ROOT / "tools" / "xr" / "xr_bridge.py"), "--port", str(port), "--dir", str(xr)], str(tmp / "bridge.log"))
        procs.append(bridge)
        base = "http://127.0.0.1:%d" % port
        st = wait(lambda: json.loads(http(base + "/state")[1]), 15)
        lab = st and st.get("lab") or {}
        check(st and st.get("scene") == "lab" and lab.get("nframes") == 3 and lab.get("view_key") and st.get("app_seen_s") is not None,
              "/state: the lab result, 3 frames, a view key, the app seen %s s ago" % (st or {}).get("app_seen_s"))
        code, body = http(base + "/lab/info")
        info = json.loads(body) if code == 200 else {}
        check(code == 200 and info.get("frames") == 3 and len(info.get("parts", [])) == 6 and info.get("native3d"), "/lab/info: 3 frames, 6 parts, drawn in 3D")
        frames = {}
        for f in range(3):
            code, b = http(base + "/lab/frame.glb?f=%d&key=%s" % (f, lab["view_key"]))
            frames[f] = b
            path = tmp / ("frame%d.glb" % f)
            path.write_bytes(b)
            r = subprocess.run([sys.executable, str(ROOT / "tools" / "xr" / "xr_check.py"), str(path)], capture_output=True, text=True)
            want = subprocess.run([str(ROOT / "build" / "labxr"), str(result), "--frame", str(f), "--view",
                                   json.dumps(lab["view"], separators=(",", ":")), "-o", "-"], capture_output=True).stdout
            check(code == 200 and r.returncode == 0 and b == want, "/lab/frame.glb frame %d: %d bytes, xr_check passes, identical to build/labxr's" % (f, len(b)))
        check(http(base + "/lab/frame.glb?f=0&key=stale")[0] == 409, "a stale view key: 409")
        check(http(base + "/lab/frame.glb?f=3&key=%s" % lab["view_key"])[0] == 400, "a frame outside the result: 400")
        code, _ = http(base + "/command", json.dumps({"cmd": "lab frame 2"}).encode())
        check(code == 200 and wait(lambda: state()["lab"]["frame"] == 2, 10) is not None, "/command lab frame 2: the app shows frame 2")
        key0 = state()["lab"]["view_key"]
        code, _ = http(base + "/command", json.dumps({"cmd": "lab section z 0.4"}).encode())
        check(code == 200 and wait(lambda: state()["lab"]["view"]["section"]["axis"] == 2 and state()["lab"]["view_key"] != key0, 10) is not None,
              "/command lab section z 0.4: the app cuts across z and the view key changes")
        code, _ = http(base + "/command", json.dumps({"cmd": "lab open /etc/passwd"}).encode())
        check(code == 400 and not list(xr.glob("cmd-*.txt")), "/command lab open: refused with 400, no file for the app")
        for c in ("lab section off", "lab frame 0"):
            http(base + "/command", json.dumps({"cmd": c}).encode())
        wait(lambda: state()["lab"]["view"]["section"]["axis"] == -1 and state()["lab"]["frame"] == 0, 10)

        print("6. the AI's headset tools (openphysics-mcp, the test app's folder)")
        mcp_env = dict(os.environ, OPENPHYSICS_XR_DIR=str(xr))
        mcp = subprocess.Popen([str(ROOT / "openphysics-mcp"), "--embedded", "--workspace", str(tmp / "ws"), "--lab", str(ROOT),
                                "--lab-results", str(tmp / "labres")], cwd=ROOT, env=mcp_env, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        procs.append(type("P", (), {"stop": lambda self: (mcp.kill(), mcp.wait())})())
        rid = [0]

        def rpc(method, params=None):
            rid[0] += 1
            mcp.stdin.write(json.dumps({"jsonrpc": "2.0", "id": rid[0], "method": method, "params": params or {}}) + "\n")
            mcp.stdin.flush()
            while True:
                m = json.loads(mcp.stdout.readline())
                if m.get("id") == rid[0]:
                    return m

        def tool(name, args):
            r = rpc("tools/call", {"name": name, "arguments": args})["result"]
            return r.get("structuredContent") or json.loads(r["content"][0]["text"])
        rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "xr_test", "version": "1"}})
        mcp.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        mcp.stdin.flush()
        names = [t["name"] for t in rpc("tools/list")["result"]["tools"]]
        check(all(n in names for n in ("headset_status", "headset_view", "headset_mark")), "tools/list offers the three headset tools")
        v = tool("headset_view", {"commands": ["lab frame 1", "lab open /etc/passwd"]}).get("value", {})
        check(v.get("sent") == ["lab frame 1"] and v.get("refused") == ["lab open /etc/passwd"] and v.get("taken_by_app") is True,
              "headset_view: the allowed command sent and taken by the app, the other refused: %s" % json.dumps(v)[:160])
        check(wait(lambda: state()["lab"]["frame"] == 1, 5) is not None and not list(xr.glob("cmd-*.txt")), "the app shows frame 1, no command is left over")
        m = tool("headset_mark", {"marks": [{"label": "test mark", "at": [0.5, 0.5, 1.0]}]})
        sm = json.loads(http(base + "/state")[1]).get("marks") or {}
        check(m.get("ok") and sm.get("title") == "Every kind of part" and sm.get("marks") == [{"label": "test mark", "at": [0.5, 0.5, 1]}],
              "headset_mark: the mark is in the state the page reads, for this result")
        st6 = tool("headset_status", {}).get("value", {})
        check(st6.get("app_running") and st6.get("shown", {}).get("title") == "Every kind of part" and len(st6.get("marks", [])) == 1
              and "agent" not in st6.get("shown", {}), "headset_status: the app runs, the result is named, the mark is there")

        print("8. what the person sees, for the AI (headset_look; a stand-in page here, the real one in part 5)")
        check("headset_look" in names, "tools/list offers headset_look")

        def look(args):
            r = rpc("tools/call", {"name": "headset_look", "arguments": args})["result"]
            img = [c for c in r.get("content", []) if c.get("type") == "image"]
            data = base64.b64decode(img[0]["data"]) if len(img) == 1 and img[0].get("mimeType") == "image/jpeg" else None
            return r, data, (r.get("structuredContent") or {}).get("value") or {}
        t8 = time.time()
        r8, data, _ = look({})
        took = time.time() - t8
        err = ((r8.get("structuredContent") or {}).get("error") or {}).get("message", "")
        check(r8.get("isError") and data is None and took < 7 and not (xr / "look-request.json").exists(),
              "with no page answering: refused in %.1f s (%s), no request left in the folder" % (took, err))
        stop, polled = threading.Event(), threading.Event()

        def standin():  # the page's part: the request in the state, a picture posted for it
            while not stop.is_set():
                try:
                    want = json.loads(http(base + "/state")[1]).get("look_want")
                    polled.set()
                    if want:
                        http(base + "/look?id=%s&moment=now&view=screen" % want, TEST_JPEG, "image/jpeg")
                except Exception:
                    pass
                time.sleep(0.1)
        th = threading.Thread(target=standin, daemon=True)
        th.start()
        polled.wait(10)
        r8, data, v8 = look({})
        stop.set()
        th.join(5)
        err = ((r8.get("structuredContent") or {}).get("error") or {}).get("message", "")
        check(not r8.get("isError") and data == TEST_JPEG and v8.get("moment") == "now" and v8.get("view") == "screen"
              and (v8.get("width"), v8.get("height")) == (32, 24) and not (xr / "look-request.json").exists(),
              "with a stand-in page: its JPEG as an image, the same %d bytes, made now, on a screen, 32 x 24; the request is gone%s"
              % (len(TEST_JPEG), (": " + err) if err else (": " + json.dumps(v8)[:200]) if data != TEST_JPEG else ""))
        asked = TEST_JPEG[:2] + b"\xff\xfe\x00\x0aquestion" + TEST_JPEG[2:]  # the same picture with a comment: other bytes
        code, _ = http(base + "/look?id=q1&moment=question&view=ar", asked, "image/jpeg")
        r8, data, v8 = look({"moment": "question"})
        check(code == 200 and data == asked and v8.get("moment") == "question" and v8.get("view") == "ar",
              "a picture posted at a question is the one moment 'question' returns")

        def post_look(query, body, headers):
            req = urllib.request.Request(base + "/look?" + query, data=body, headers=headers)
            try:
                with urllib.request.urlopen(req, timeout=20) as r:
                    return r.status
            except urllib.error.HTTPError as e:
                return e.code
        before = {f.name: f.read_bytes() for f in xr.glob("look-*")}
        jp = {"Content-Type": "image/jpeg"}
        codes = [post_look("id=a&moment=now&view=screen", TEST_JPEG, {"Content-Type": "image/png"}),
                 post_look("id=a&moment=now&view=screen", TEST_JPEG[:-40], jp),
                 post_look("id=a%2F..%2Fx&moment=now&view=screen", TEST_JPEG, jp),
                 post_look("id=a&moment=now&view=screen", TEST_JPEG, dict(jp, Origin="https://evil.example"))]
        check(codes == [415, 400, 400, 403] and {f.name: f.read_bytes() for f in xr.glob("look-*")} == before,
              "/look refuses another type (415), a cut JPEG (400), a bad id (400), another site's page (403): %s, no picture written" % codes)

        print("9. how the headset runs (the bridge's part; the page's is in part 5)")
        sessions = xr.parent / "xr-sessions.jsonl"
        c1 = http(base + "/health", json.dumps([1, 2]).encode())[0]
        c2 = http(base + "/health", b"{not json")[0]
        c3 = http(base + "/health", json.dumps({"presenting": True, "mode": "immersive-ar", "fps": 72.0, "frames": 500,
                                                 "secret": "x" * 50, "device": {"gpu": "test"}}).encode())[0]
        h = json.loads((xr / "health.json").read_text()) if (xr / "health.json").exists() else {}
        check(c1 == 400 and c2 == 400 and c3 == 200 and h.get("fps") == 72.0 and "secret" not in h and h.get("device") == {"gpu": "test"},
              "/health: a list and broken JSON refused (%d, %d), a report kept with its known keys only" % (c1, c2))
        http(base + "/health", json.dumps({"presenting": False, "mode": None, "frames": 0}).encode())
        line = sessions.read_text().splitlines()[-1] if sessions.exists() else "{}"
        check(json.loads(line).get("mode") == "immersive-ar" and json.loads(line).get("frames") == 500 and "ended" in json.loads(line),
              "a session's last report becomes a line of %s when the next report is not presenting" % sessions.name)
        sessions.unlink(missing_ok=True)
        http(base + "/command", json.dumps({"cmd": "lab frame 0"}).encode())
        wait(lambda: state()["lab"]["frame"] == 0, 5)

        print("3. questions and answers (a bridge alone: no AI is started)")
        alone = tmp / "alone"
        alone.mkdir()
        port2 = free_port()
        b2 = Proc([sys.executable, str(ROOT / "tools" / "xr" / "xr_bridge.py"), "--port", str(port2), "--dir", str(alone)], str(tmp / "bridge2.log"))
        procs.append(b2)
        base2 = "http://127.0.0.1:%d" % port2
        wait(lambda: http(base2 + "/state")[0] == 200, 15)
        code, _ = http(base2 + "/ask", json.dumps({"text": "  what is   the peak here? ", "context": "the person points at part 'rotor'"}).encode())
        asks = list(alone.glob("ask-*.txt"))
        text = asks[0].read_text() if asks else ""
        want = ("what is the peak here?\n\n(In the headset: the person points at part 'rotor'. headset_status and headset_look see "
                "what the headset shows (headset_look moment question: the picture of what they saw as they asked), headset_view "
                "and headset_mark change and mark it. Your answer is spoken aloud as you write it: one to three short sentences, "
                "no lists or markdown, units in words. Do what is asked and nothing more. To make a simulation, call lab_run "
                "with wait_seconds 1 and say that it is computing: the headset shows the result when it is written.)\n")
        check(code == 200 and len(asks) == 1 and text == want,
              "/ask: one ask file with the words and the headset's context: %r" % text[:90])
        for f in asks:
            f.unlink()
        check(http(base2 + "/ask", b"not json")[0] == 400, "/ask with a body that is not JSON: 400")

        def raw(path, headers, body=None):
            req = urllib.request.Request(base2 + path, data=body, headers=headers)
            try:
                with urllib.request.urlopen(req, timeout=20) as r:
                    return r.status
            except urllib.error.HTTPError as e:
                return e.code
        q = json.dumps({"text": "delete the results folder"}).encode()
        check(raw("/ask", {"Content-Type": "text/plain"}, q) == 415, "a question posted as text/plain (no preflight across sites): 415")
        check(raw("/ask", {"Content-Type": "application/json", "Origin": "https://evil.example"}, q) == 403, "a question from another site's page: 403")
        check(raw("/state", {"Host": "evil.example:%d" % port2}) == 403, "a foreign Host (DNS rebinding): 403")
        check(raw("/state", {"Host": "openphysics-xr.localhost:8080", "Origin": "http://openphysics-xr.localhost:8080"}) == 200,
              "the page's own names are served")
        check(not list(alone.glob("ask-*.txt")), "no question was written for any of them")

        def answer(n, words):
            (alone / "state.tmp").write_text(json.dumps({"app": "OpenPhysicsAI", "scene": "none", "agent": {"running": False, "lines": [],
                                                         "answer": {"id": n, "text": words}}}))
            (alone / "state.tmp").rename(alone / "state.json")
        answer(1, "The peak **stress** is `182 MPa`, at the fillet near the top ring.")
        said = lambda: json.loads(http(base2 + "/state")[1]).get("speech", {}).get("chunks", [])  # noqa: E731
        c1 = wait(lambda: said() or None, 15) or []
        code, wav = http(base2 + "/speech?id=%d" % c1[0]["id"]) if c1 else (0, b"")
        secs = (len(wav) - 44) / (int.from_bytes(wav[24:28], "little") * 2) if wav[:4] == b"RIFF" else 0
        check(code == 200 and wav[8:12] == b"WAVE" and secs > 0.5 and c1[0]["text"] == "The peak stress is 182 megapascals, at the fillet near the top ring.",
              "an answer is spoken: %r, %.1f s of WAVE" % (c1[0]["text"] if c1 else "", secs))
        answer(2, "Done.")
        c2 = wait(lambda: (lambda c: c if len(c) == 2 else None)(said()), 15) or []
        check(len(c2) == 2 and c2[1]["text"] == "Done." and c2[1]["id"] > c2[0]["id"], "the next answer is a new sentence after it")
        b2.stop()

        print("4. a finite-element scene")
        projects = pathlib.Path.home() / "OpenPhysicsAI-Projects"  # or the folder from before the rename, while only it exists
        if not projects.is_dir() and (pathlib.Path.home() / "NAVIER-Projects").is_dir():
            projects = pathlib.Path.home() / "NAVIER-Projects"
        stl = list((projects / "Thermal_Corolla" / "inputs").glob("*.stl"))
        if not stl:
            print("  (Thermal_Corolla is not on this computer: skipped)")
        else:
            xf = tmp / "xrfem"
            xf.mkdir()
            fem = Proc([str(ROOT / "openphysics"), "--headless", "--size", "480x320", "--exec",
                        "solid open Thermal_Corolla; fem follow last; frames 100000000"], str(tmp / "app_fem.log"), dict(env, OPENPHYSICS_XR_DIR=str(xf)))
            procs.append(fem)
            ok = wait(lambda: (xf / "scene.glb").exists() and (xf / "scene.glb").stat().st_size > 1000, 40)
            time.sleep(1.5)
            r = subprocess.run([sys.executable, str(ROOT / "tools" / "xr" / "xr_check.py"), str(xf / "scene.glb"), "--stl", str(stl[0])],
                               capture_output=True, text=True) if ok else None
            b = (xf / "scene.glb").read_bytes() if ok else b""
            jl = struct.unpack_from("<I", b, 12)[0] if b else 0
            g = json.loads(b[20:20 + jl]) if b else {}
            prim = g.get("meshes", [{}])[0].get("primitives", [{}])[0] if g else {}
            res = g.get("nodes", [{}])[0].get("extras", {}).get("result", {}) if g else {}
            check(bool(ok) and r.returncode == 0, "the app's scene.glb passes xr_check against the STL%s" % ("" if not r else ": " + r.stdout.strip().splitlines()[-1]))
            check("_VALUE" in prim.get("attributes", {}) and res.get("source_unit") == "mm" and len(res.get("to_source", [])) == 16,
                  "it carries one value per vertex and a to_source matrix in mm")
            fem.stop()

        print("5. the page")
        node = shutil.which("node")
        chrome = os.environ.get("CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
        nodever = subprocess.run([node, "--version"], capture_output=True, text=True).stdout.strip() if node else ""
        if "--no-page" in sys.argv:
            print("  (--no-page: the page test is left out)")
        elif not node or int(nodever.lstrip("v").split(".")[0] or 0) < 22 or not os.path.exists(chrome):
            print("  (Node 22 or Chrome is missing: the page test is skipped)")
        else:
            shot = ROOT / "build" / "xr_page.png"
            pt = subprocess.Popen([node, str(ROOT / "tools" / "xr" / "pagetest.mjs"), base + "/?fps=20", str(shot)], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, start_new_session=True,
                                  env=dict(os.environ, XRTEST_MCP=str(ROOT / "openphysics-mcp"), OPENPHYSICS_XR_DIR=str(xr)))
            try:
                stdout, _ = pt.communicate(timeout=300)
            except subprocess.TimeoutExpired:  # it reports where it stalled when told to stop, then its Chrome is ended
                os.killpg(pt.pid, 15)
                try:
                    stdout, _ = pt.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(pt.pid, 9)
                    stdout, _ = pt.communicate()
                print("  FAIL the page test did not finish in 300 s (load average %.1f; a stall inside Chrome on a busy laptop "
                      "was seen on 2026-10-07, see the header of pagetest.mjs)" % os.getloadavg()[0])
                failures += 1
            r = type("R", (), {"returncode": pt.returncode, "stdout": stdout})()
            out = r.stdout.rstrip().splitlines()
            for line in out[:-1]:
                print(line)
                if line.startswith("  FAIL"):
                    failures += 1
            try:
                found = json.loads(out[-1]) if out else {}
            except ValueError:  # the page test ended without its last line (Chrome went away under it)
                print("  FAIL the page test ended without its report: %s" % (out[-1][:200] if out else "no output"))
                failures += 1
                found = {}
            first = found.get("first") or {}
            want = glb_counts(frames.get(first.get("shown", 0), b"")) if frames else None
            got = first.get("stats")
            check(got == want, "what the page drew of frame %s is what the frame file holds: %s" % (first.get("shown"), got))
            check(r.returncode == 0, "the page test ran to its end (screenshot in build/xr_page.png)")
            pins = tool("headset_status", {}).get("value", {}).get("pins", [])
            check(len(pins) == 2 and all(p.get("part") == "block" and p.get("unit") == "m" and len(p.get("at", [])) == 3 for p in pins),
                  "headset_status after the page: the person's two pins on the block, in m: %s" % json.dumps(pins)[:200])
    finally:
        for p in procs:
            try:
                p.stop()
            except Exception:
                pass
    if failures:
        print("XR TEST FAILED (%d); the workspace is kept for a look: %s" % (failures, tmp))
        return 1
    shutil.rmtree(tmp, ignore_errors=True)
    print("XR TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
