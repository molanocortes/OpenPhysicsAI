#!/usr/bin/env python3
"""xr_bridge.py - the Mac side of the headset link (docs/xr.md): the app's result in a Meta Quest, questions spoken
there answered by Ask AI, and the answers spoken back.

    python3 tools/xr/xr_bridge.py [--port 8791] [--dir DIR] [--voice NAME]

While it runs it keeps the headset folder (~/.openphysicsai/run/xr, or --dir, or $OPENPHYSICS_XR_DIR), where the app writes
scene.glb (a finite-element result) and state.json (what is shown: the finite-element field, or the lab result's file,
frame and view; what the AI is doing and its last answer). It serves the headset page (tools/xr/web) and:
    GET  /state                      the app's state, with how long ago the app wrote it
    GET  /scene.glb                  the finite-element result on screen
    GET  /lab/info                   the open lab result: frames, times, fields and units, its box (build/labxr --info)
    GET  /lab/frame.glb?f=N&key=K    frame N drawn as the window shows it (build/labxr --frame N --view ...); K is the
                                     view's key from the state, so no frame is drawn with a view that has changed (409)
    GET  /events                     the state again each time it changes (server-sent events): the page needs no polling
    GET  /speech?id=N                the AI's words as speech, sentence N of the conversation (build/openphysics-voice, a
                                     synthesizer kept warm: about 50 ms a sentence; `say` when it is not built)
    POST /voice?lang=en-US&ctx=...   a spoken question as 16-bit mono WAV, into words by build/openphysics-listen (the Mac's
                                     own speech recognition), to Ask AI; ctx: what the person points at in the headset;
                                     with ask=0 only the words come back (the page runs a plain view command itself)
    POST /ask      {"text": ..., "context": ...}   a typed question
    POST /command  {"cmd": "lab frame 12"}         a view command (tools/xr/commands.json: what the headset may do)
    POST /answer   {"allow": true}                 the answer to the AI's permission prompt
    POST /live/start {"sdp": offer}                the provider's voice (live.py: GPT-Live, when an OpenAI key is
                                                   here): OpenAI's SDP answer for the headset's WebRTC offer; the
                                                   bridge attaches to the session and answers its delegations
    POST /live/stop, /live/context {"context"}, /live/handled {"text"}   end it; what the headset shows and the
                                                   person points at; a view command the headset did itself
    POST /stop                                     "stop", or the person talking over the AI: the answer ends
                                                   (stop.txt: the app interrupts its turn) and what is not yet said is not
    POST /pins     {"pins": [...]}                 the points the person pinned, kept in page.json for the AI
    POST /marks/clear                              takes away the marks the AI placed (openphysics-mcp headset_mark)
    POST /look?id=ID&moment=now|question&view=ar|vr|screen   a JPEG of what the person sees, for the AI's
                                     headset_look: look-<moment>.jpg, then look-<moment>.json with its facts
    POST /health   {...}             how the page runs (the device, the session, its frame rate, its errors): kept in
                                     health.json for headset_status, and each headset session's last report appended to
                                     ../xr-sessions.jsonl beside the folder, which outlives the bridge
The state the page reads carries the AI's marks (marks.json) and, while openphysics-mcp headset_look waits for a picture,
look_want: the id of its request (look-request.json); page.json says when the page last asked (the AI's
headset_status reads it, with the pins).
Only localhost names are served (127.0.0.1, localhost, ::1, *.localhost: no DNS rebinding), a browser's Origin must be
one of them (no other site's page can post a question to the AI), and a POST must say it is JSON (WAV for /voice, JPEG for /look),
which a page from another site cannot send without a CORS preflight that this bridge never grants.
The page is opened in the Quest at http://openphysics-xr.localhost:8080 through the localhost-router and a USB tunnel
(adb reverse tcp:8080 tcp:8080): *.localhost is a secure origin, so WebXR and the microphone work without certificates.
Python is glue here: nothing physical is computed by this program; build/labxr draws what the app's lab draws.
"""
import argparse, collections, http.server, json, os, pathlib, queue, re, subprocess, sys, tempfile, threading, time
import urllib.parse

import live  # the provider's voice (GPT-Live), tools/xr/live.py

ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB = ROOT / "tools" / "xr" / "web"
LISTEN = ROOT / "build" / "openphysics-listen"
VOICE_HELPER = ROOT / "build" / "openphysics-voice"
LISTEN_APP = ROOT / "build" / "OpenPhysicsAIListen.app"
LABXR = ROOT / "build" / "labxr"
XR = pathlib.Path(os.environ.get("OPENPHYSICS_XR_DIR") or os.environ.get("NAVIER_XR_DIR")  # NAVIER_: the name before 2026-10-21
                  or pathlib.Path.home() / ".openphysicsai" / "run" / "xr")
VOICE = None
TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css",
         ".glb": "model/gltf-binary", ".json": "application/json", ".png": "image/png", ".wav": "audio/wav"}
STALE_S = 6  # the app writes state.json at least every 2 s while it runs
LOOK_ID = re.compile(r"[A-Za-z0-9_-]{1,40}")
LOOK_WAIT_S = 10  # a request for a picture older than this is not answered (openphysics-mcp waits 6 s)


# ---- the view commands the headset may give: the same rule as the app's and openphysics-mcp's (src/core/xrlink.c) ----

LAB1, FEM1 = {"play", "pause", "next", "prev"}, {"play", "pause"}
OFFFLIP, STEPS, AXES = {"off", "flip"}, {"next", "prev", "last", "first"}, {"x", "y", "z"}
DEFORM = {"auto", "true"}


def _int(w):
    return re.fullmatch(r"-?[0-9]+", w) is not None


def _fraction(w):
    try:
        return re.fullmatch(r"[0-9.eE+-]+", w) is not None and 0 <= float(w) <= 1
    except ValueError:
        return False


def _positive(w):
    try:
        return re.fullmatch(r"[0-9.eE+-]+", w) is not None and float(w) > 0
    except ValueError:
        return False


def command_allowed(line):
    if len(line) > 120 or any(ord(c) < 0x20 or ord(c) > 0x7E for c in line):
        return False
    w = line.split()
    if len(w) < 2 or len(w) > 4:
        return False
    n, head, verb = len(w), w[0], w[1]
    if head == "lab":
        if n == 2:
            return verb in LAB1
        if verb == "frame":
            return n == 3 and _int(w[2])
        if verb == "field":
            return n == 3 and re.fullmatch(r"[A-Za-z0-9_.+-]{1,47}", w[2]) is not None
        if verb == "range":
            return n == 3 and w[2] == "auto"
        if verb == "section":
            return (n == 3 and w[2] in OFFFLIP) or (n == 4 and w[2] in AXES and _fraction(w[3]))
        return False
    if head == "fem":
        if n == 2:
            return verb in FEM1
        if verb == "step":
            return n == 3 and (_int(w[2]) or w[2] in STEPS)
        if verb == "field":  # any finite-element field by name; the app ignores one it does not have
            return n == 3 and re.fullmatch(r"[a-z_]{1,40}", w[2]) is not None
        if verb == "deform":
            return n == 3 and (w[2] in DEFORM or _positive(w[2]))
        if verb == "section":
            return (n == 3 and w[2] in OFFFLIP) or (n == 4 and w[2] in AXES and _fraction(w[3]))
        return False
    return False


# ---- files for the app, each written whole before it appears ----

def drop(prefix, text):
    tmp = XR / ("tmp-%d.txt" % time.time_ns())
    tmp.write_text(text + "\n")
    tmp.rename(XR / ("%s-%d.txt" % (prefix, time.time_ns())))


def put_question(text, context=""):
    """a question for Ask AI; the context (what is pointed at in the headset) follows the words the way the window adds
    its own, after a blank line in brackets, so the conversation shows only the words"""
    text = " ".join(text.split())[:1500]
    if not text:
        return False
    context = " ".join((context or "").split())[:800]
    tools = ("headset_status and headset_look see what the headset shows (headset_look moment question: the picture of what "
             "they saw as they asked), headset_view and headset_mark change and mark it")
    spoken = ("Your answer is spoken aloud as you write it: one to three short sentences, no lists or markdown, units in "
              "words. Do what is asked and nothing more")
    pic = question_picture()
    seen = " The picture is what they see as they ask: the result, the card, their pointer." if pic else ""
    shown = shown_simulation(read_state())
    if shown:  # a change in one step: no documentation to read, no scenario to write out again
        seen += (" The simulation shown is '%s': to change it, say in a few words what you will change, then call "
                 "lab_change with base '%s', wait_seconds 1 and only the values to change; after it say only that it is "
                 "computing, or why it was refused (lab_run makes a new one). Its values by path: %s."
                 % (shown[0], shown[0], shown[1]))
    else:
        spoken += (". To make a simulation, call lab_run with wait_seconds 1 and say that it is computing: the headset "
                   "shows the result when it is written")
    drop("ask", text + "\n\n(In the headset%s. %s. %s.%s)%s" % (": " + context if context else "", tools, spoken, seen,
                                                             "\n[picture: %s]" % pic if pic else ""))
    return True


def shown_simulation(state, limit=1200):
    """the lab result the app shows, for a spoken change in one step (openphysics-mcp lab_change): its name and its
    scenario's values by path, from the scenario in the result's own header (what was computed), shallow paths before
    deeper ones, without source texts, long notes or long lists, at most `limit` characters of them; None when no lab
    result is shown"""
    path = ((state or {}).get("lab") or {}).get("path") if (state or {}).get("scene") == "lab" else None
    if not path or not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", pathlib.Path(path).stem):  # lab_change's names
        return None
    try:
        with open(path, "rb") as f:
            if f.read(8) != b"OPLAB01\n":
                return None
            n = int.from_bytes(f.read(4), "little")
            sc = json.loads(f.read(n)).get("scenario") if n <= 16 << 20 else None
    except (OSError, ValueError, AttributeError):
        return None
    if not isinstance(sc, dict):
        return None
    levels = collections.defaultdict(list)

    def walk(v, at):
        if isinstance(v, dict):
            for k, x in v.items():
                if k not in ("source", "changed_from") and not (not at and k in ("domain", "title")):
                    walk(x, at + [k])
        elif isinstance(v, list) and any(isinstance(x, (dict, list)) for x in v):
            for i, x in enumerate(v):
                walk(x, at + [str(i)])
        elif not (isinstance(v, str) and len(v) > 40) and not (isinstance(v, list) and len(v) > 8):
            levels[len(at)].append("%s %s" % (".".join(at), json.dumps(v, separators=(",", ":"))))

    walk(sc, [])
    out, size = [], 0
    for depth in sorted(levels):
        for item in levels[depth]:
            if size + len(item) + 2 > limit:
                out.append("and more (lab_scenario_get has them all)")
                return pathlib.Path(path).stem, ", ".join(out)
            out.append(item)
            size += len(item) + 2
    return pathlib.Path(path).stem, ", ".join(out)


def question_picture():
    """the picture the page took as the person asked (look-question.jpg, at most 5 s old), copied under a name of its own
    that the app sends with the question and then removes; None without one"""
    try:
        facts = json.loads((XR / "look-question.json").read_text())
        if time.time() - float(facts.get("made", 0)) > 5:
            return None
        name = XR / ("q-%d.jpg" % time.time_ns())
        tmp = XR / ("q-%d.tmp" % time.time_ns())
        tmp.write_bytes((XR / "look-question.jpg").read_bytes())
        tmp.rename(name)
        return str(name)
    except (OSError, ValueError, TypeError, AttributeError):
        return None


def read_state():
    f = XR / "state.json"
    try:
        state, seen = json.loads(f.read_text()), round(time.time() - f.stat().st_mtime, 1)
    except (OSError, ValueError):
        state, seen = {"app": "OpenPhysicsAI", "scene": "none", "has_scene": False, "version": 0,
                       "note": "the OpenPhysicsAI app is not running"}, None
    if seen is not None and seen > STALE_S:
        state.update(scene="none", has_scene=False, note="the OpenPhysicsAI app is not running on the Mac")
    state["app_seen_s"] = seen
    state["voice"] = LISTEN.exists()
    try:
        state["marks"] = json.loads((XR / "marks.json").read_text())
    except (OSError, ValueError):
        state["marks"] = None
    state["speech"] = SPEAKER.public()
    state["voice_provider"] = "openai-live" if live.api_key() else "mac"
    state["live"] = LIVE.public()
    try:  # the AI waits for a picture of what the person sees
        want = json.loads((XR / "look-request.json").read_text())
        if time.time() - float(want.get("made", 0)) < LOOK_WAIT_S and LOOK_ID.fullmatch(str(want.get("id", ""))):
            state["look_want"] = want["id"]
    except (OSError, ValueError, AttributeError, TypeError):
        pass
    return state


def put_look(lid, moment, view, jpeg):
    """what the person sees, for openphysics-mcp headset_look: the picture, then its facts, which the AI waits for (each
    written whole before it appears); a picture taken now answers the request it was taken for"""
    st = read_state()
    facts = {"id": lid, "moment": moment, "view": view, "made": time.time(), "bytes": len(jpeg), "width": 0, "height": 0,
             "title": st.get("title", ""), "scene": st.get("scene", "none")}
    facts["width"], facts["height"] = jpeg_size(jpeg)
    for name, data in (("look-%s.jpg" % moment, jpeg), ("look-%s.json" % moment, json.dumps(facts).encode())):
        tmp = XR / ("look-%d.tmp" % time.time_ns())
        tmp.write_bytes(data)
        tmp.rename(XR / name)
    if moment == "now":
        try:
            if json.loads((XR / "look-request.json").read_text()).get("id") == lid:
                (XR / "look-request.json").unlink()
        except (OSError, ValueError, AttributeError):
            pass
    return facts


HEALTH_KEYS = ("presenting", "mode", "fps", "fps_low", "frame_ms_worst", "frames", "session_s", "volume_steps", "features",
               "blend", "frame_rate", "frame_rates", "eye_pixels", "foveation", "scale", "content", "errors", "device", "heap_mb")
SESSION = {"last": None}
SESSION_LOCK = threading.Lock()


def clean_health(h):
    """what the page says about how it runs, kept to plain values of bounded size"""
    def plain(v, depth=0):
        if isinstance(v, bool) or v is None:
            return v
        if isinstance(v, (int, float)):
            return v if v == v and abs(v) < 1e12 else None
        if isinstance(v, str):
            return v[:300]
        if isinstance(v, list) and depth < 3:
            return [plain(x, depth + 1) for x in v[:24]]
        if isinstance(v, dict) and depth < 3:
            return {str(k)[:40]: plain(x, depth + 1) for k, x in list(v.items())[:24]}
        return None
    return {k: plain(h[k]) for k in HEALTH_KEYS if isinstance(h, dict) and k in h}


def put_health(h):
    """the latest report for headset_status; a headset session's last one goes to the log beside the folder when the
    session ends, so a trial in the Quest leaves its numbers behind after the bridge has gone"""
    h = clean_health(h)
    h["received"] = round(time.time(), 1)
    with SESSION_LOCK:
        tmp = XR / "health.tmp"
        tmp.write_text(json.dumps(h))
        tmp.rename(XR / "health.json")
        last = SESSION["last"]
        if last and last.get("presenting") and not h.get("presenting"):
            log_session(last)
        SESSION["last"] = h if h.get("presenting") else None
    return h


def log_session(h):
    log = XR.parent / "xr-sessions.jsonl"
    try:
        lines = log.read_text().splitlines()[-99:] if log.exists() else []
        lines.append(json.dumps(dict(h, ended=time.strftime("%Y-%m-%d %H:%M:%S"))))
        tmp = XR.parent / "xr-sessions.tmp"
        tmp.write_text("\n".join(lines) + "\n")
        tmp.rename(log)
    except OSError as e:
        sys.stderr.write("xr bridge: cannot keep the session's report: %s\n" % e)


def jpeg_size(b):
    """(width, height) from a JPEG's start-of-frame marker, or (0, 0)"""
    i = 2
    while i + 9 < len(b) and b[i] == 0xFF:
        marker, n = b[i + 1], int.from_bytes(b[i + 2:i + 4], "big")
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            return int.from_bytes(b[i + 7:i + 9], "big"), int.from_bytes(b[i + 5:i + 7], "big")
        i += 2 + n
    return 0, 0


# ---- the page: when it last asked, and the points the person pinned there (page.json, for the AI) ----

PAGE = {"seen": 0, "pins": [], "written": 0}
PAGE_LOCK = threading.Lock()


def page_seen(pins=None):
    with PAGE_LOCK:
        now = time.time()
        PAGE["seen"] = now
        if pins is not None:
            PAGE["pins"] = pins
        if pins is None and now - PAGE["written"] < 2:
            return
        PAGE["written"] = now
        tmp = XR / "page.tmp"
        tmp.write_text(json.dumps({"seen": int(now), "pins": PAGE["pins"]}))
        tmp.rename(XR / "page.json")


def clean_pins(pins):
    """what the page says about a pin, kept to its plain facts"""
    out = []
    for p in (pins if isinstance(pins, list) else [])[:12]:
        if not isinstance(p, dict):
            continue
        at = p.get("at")
        if not (isinstance(at, list) and len(at) == 3 and all(isinstance(x, (int, float)) for x in at)):
            continue
        q = {"at": [float(x) for x in at]}
        for k in ("part", "unit", "field", "value_unit", "what"):
            if isinstance(p.get(k), str):
                q[k] = p[k][:60]
        for k in ("value", "frame", "time"):
            if isinstance(p.get(k), (int, float)) and p[k] == p[k]:
                q[k] = p[k]
        out.append(q)
    return out


# ---- lab frames: drawn by build/labxr, the app's own scene code; kept a while in memory, never on disk ----

class Frames:
    def __init__(self, cap_bytes=160 << 20):
        self.cap, self.size, self.lock = cap_bytes, 0, threading.Lock()
        self.cache = collections.OrderedDict()
        self.busy = {}  # a key being drawn: its event, so a second request waits for the first
        self.gate = threading.Semaphore(2)  # at most two drawings at once on a shared laptop

    def get(self, key, argv):
        while True:
            with self.lock:
                if key in self.cache:
                    self.cache.move_to_end(key)
                    return self.cache[key], None
                ev = self.busy.get(key)
                if ev is None:
                    self.busy[key] = ev = threading.Event()
                    break
            ev.wait(60)
        try:
            with self.gate:
                r = subprocess.run(argv, capture_output=True, timeout=60)
            if r.returncode:
                return None, (r.stderr.decode(errors="replace").strip().replace("labxr: ", "") or "build/labxr failed")[:300]
            with self.lock:
                self.cache[key] = r.stdout
                self.size += len(r.stdout)
                while self.size > self.cap and len(self.cache) > 1:
                    self.size -= len(self.cache.popitem(last=False)[1])
            return r.stdout, None
        except subprocess.TimeoutExpired:
            return None, "build/labxr took longer than 60 s"
        finally:
            with self.lock:
                self.busy.pop(key, None)
            ev.set()


FRAMES = Frames()


# ---- the AI's answers, spoken: synthesised as soon as one appears in the state ----

UNITS = [("MPa", "megapascals"), ("kPa", "kilopascals"), ("GPa", "gigapascals"), ("Pa", "pascals"), ("kW", "kilowatts"),
         ("MW", "megawatts"), ("W", "watts"), ("°C", "degrees Celsius"), ("K", "kelvin"), ("mm", "millimetres"),
         ("cm", "centimetres"), ("m/s", "metres per second"), ("km", "kilometres"), ("m", "metres"), ("ms", "milliseconds"),
         ("µs", "microseconds"), ("s", "seconds"), ("kHz", "kilohertz"), ("Hz", "hertz"), ("kg", "kilograms"), ("N", "newtons"),
         ("kN", "kilonewtons"), ("V/m", "volts per metre"), ("V", "volts"), ("A", "amperes"), ("T", "tesla"), ("%", "percent")]
UNIT_RE = re.compile(r"(?<=\d)\s?(" + "|".join(re.escape(u) for u, _ in sorted(UNITS, key=lambda x: -len(x[0]))) + r")(?![\w/])")


def speakable(text):
    """words for the voice: no markdown, code, links or paths; a unit after a number said in full"""
    t = re.sub(r"```.*?```", " ", text, flags=re.S)
    t = re.sub(r"`([^`]*)`", r"\1", t)
    t = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", t)
    t = re.sub(r"https?://\S+", "", t)
    t = re.sub(r"(?<!\w)(?:~|\.{1,2})?/[\w.~/-]+", "", t)  # file paths
    t = re.sub(r"[*_#>|~]+", " ", t)
    words = dict(UNITS)
    t = UNIT_RE.sub(lambda m: " " + words[m.group(1)], t)
    return re.sub(r"\s+", " ", t).strip()


SENTENCE_END = re.compile(r"[.!?]+[\"')\]]*(?=\s)|\n+")


def sentences(text, final):
    """the whole sentences at the start of text and how many characters they take; with final, the rest as well. A
    first sentence that runs long is cut at a comma, so the voice starts sooner"""
    out, used = [], 0
    for m in SENTENCE_END.finditer(text):
        piece = text[used:m.end()].strip()
        if piece:
            out.append(piece)
        used = m.end()
    rest = text[used:]
    if not final and not out and len(rest) > 160:
        cut = rest.find(", ", 60)
        if 0 < cut < 200:
            out.append(rest[:cut + 1].strip())
            used += cut + 2
            rest = text[used:]
    if final and rest.strip():
        out.append(rest.strip())
        used = len(text)
    return out, used


class Events:
    """a counter the page's event stream waits on: bumped whenever what /state says may have changed"""
    def __init__(self):
        self.cond, self.n = threading.Condition(), 0

    def bump(self):
        with self.cond:
            self.n += 1
            self.cond.notify_all()

    def wait(self, seen, timeout):
        with self.cond:
            self.cond.wait_for(lambda: self.n != seen, timeout)
            return self.n


EVENTS = Events()


class Speaker:
    """the AI's words as speech, sentence by sentence as they are written (the app streams them into the state), made by a
    synthesizer kept warm; each sentence is a numbered chunk the page plays in order"""
    def __init__(self):
        self.lock = threading.Lock()
        self.chunks = collections.OrderedDict()  # id -> {id, turn, text, seconds, made, wav}
        self.next_id, self.turn, self.taken, self.said = 1, 0, 0, 0
        self.stopped, self.answer_seen = set(), None
        self.jobs = queue.Queue()
        self.helper, self.helper_lock = None, threading.Lock()
        self.voice = None  # the voice's name once it is loaded

    def feed(self, st, silent=False):
        ag = st.get("agent") or {}
        say, ans = ag.get("say") or {}, ag.get("answer") or {}
        turn, text, done = int(say.get("turn") or 0), say.get("text") or "", bool(say.get("done"))
        with self.lock:
            if turn and turn != self.turn:
                self.turn, self.taken, self.said = turn, 0, 0
            parts = []
            if turn and turn not in self.stopped:
                parts, used = sentences(text[self.taken:], done)
                self.taken += used
                self.said += len(parts)
            # a tool whose words do not stream (codex exec): its finished answer, once (the page plays only what is
            # made after it opened, so an answer older than the page is never heard)
            aid = int(ans.get("id") or 0)
            if aid and aid != self.answer_seen:
                self.answer_seen = aid
                if not self.said and ans.get("text"):  # the app wrote this state just now: it is running
                    parts = sentences(ans["text"], True)[0]
        seen = time.time()
        for p in parts if not silent else []:  # the provider's voice says the answers while it is live
            words = speakable(p)
            if words:
                self.jobs.put((turn, words, seen))

    def stop(self):
        with self.lock:
            self.stopped.add(self.turn)
        EVENTS.bump()

    def work(self):
        while True:
            turn, text, seen = self.jobs.get()
            with self.lock:
                if turn in self.stopped:
                    continue
            try:
                wav, secs = self.synth(text)
            except Exception as e:  # the bridge keeps serving whatever happens to a voice
                sys.stderr.write("xr bridge: speech: %s\n" % e)
                continue
            if not wav:
                continue
            with self.lock:
                if turn in self.stopped:
                    continue
                cid, self.next_id = self.next_id, self.next_id + 1
                self.chunks[cid] = {"id": cid, "turn": turn, "text": text, "seconds": round(secs, 2), "seen": seen, "made": time.time(), "wav": wav}
                while len(self.chunks) > 60:
                    self.chunks.popitem(last=False)
            EVENTS.bump()

    def warm(self):
        """the voice helper started and its voice loaded (about a second, once) before the first answer needs it"""
        with self.helper_lock:
            if VOICE_HELPER.exists() and (self.helper is None or self.helper.poll() is not None):
                args = [str(VOICE_HELPER)] + (["--voice", VOICE] if VOICE else [])
                self.helper = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
                ready = json.loads(self.helper.stdout.readline() or "{}")
                self.voice = ready.get("voice") or "?"
                sys.stderr.write("xr bridge: the voice is %s\n" % self.voice)
                EVENTS.bump()

    def synth(self, text):
        """(wav bytes, seconds): build/openphysics-voice when it is built, else say"""
        self.warm()
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "s.wav")
            with self.helper_lock:
                if VOICE_HELPER.exists() and self.helper and self.helper.poll() is None:
                    self.helper.stdin.write(json.dumps({"id": 1, "text": text, "out": out}) + "\n")
                    self.helper.stdin.flush()
                    r = json.loads(self.helper.stdout.readline() or "{}")
                    if r.get("out"):
                        return pathlib.Path(out).read_bytes(), float(r.get("seconds") or 0)
                cmd = ["say", "-o", out, "--data-format=LEI16@22050"] + (["-v", VOICE] if VOICE else []) + [text]
                if subprocess.run(cmd, capture_output=True, timeout=120).returncode == 0:
                    b = pathlib.Path(out).read_bytes()
                    return b, max(0, len(b) - 4096) / 44100
        return None, 0

    def get(self, cid):
        with self.lock:
            c = self.chunks.get(cid)
            return c["wav"] if c else None

    def public(self):
        """what the page plays: the chunks of the last two turns, without their sound"""
        with self.lock:
            last = [c for c in self.chunks.values() if c["turn"] >= self.turn - 1]
            return {"turn": self.turn, "stopped": self.turn in self.stopped, "voice": self.voice,
                    "chunks": [{k: c[k] for k in ("id", "turn", "text", "seconds", "seen", "made")} for c in last]}


SPEAKER = Speaker()


def watch_state():
    """the app's state.json, looked at 25 times a second: each change goes to the speaker and to the page's stream"""
    last = None
    while True:
        try:
            f = XR / "state.json"
            stt = f.stat()
            key = (stt.st_mtime_ns, stt.st_size)
            if key != last:
                last = key
                st = json.loads(f.read_text())
                SPEAKER.feed(st, silent=LIVE.active)
                LIVE.on_state(st)
                EVENTS.bump()
        except (OSError, ValueError):
            pass
        except Exception as e:  # the bridge keeps serving whatever happens here
            sys.stderr.write("xr bridge: state: %s\n" % e)
        time.sleep(0.04)


class Listener:
    """speech into words: the background app OpenPhysicsAI Listen (tools/xr/listen.swift), started once with open(1) so
    that macOS asks under its own name whether it may recognise speech, whoever started this bridge; it keeps the
    recogniser loaded and serves the folder listen/ (NAME.req in, NAME.json out). Where the app is not built, the
    command-line helper runs for each sentence (refused by macOS when this bridge was started from Terminal)."""
    def __init__(self):
        self.lock = threading.Lock()

    def ready(self):
        try:
            r = json.loads((XR / "listen" / "ready.json").read_text())
            os.kill(int(r["pid"]), 0)
            return r
        except (OSError, ValueError, KeyError, TypeError):
            return None

    def start(self, wait_s):
        d = XR / "listen"
        d.mkdir(exist_ok=True)
        (d / "ready.json").unlink(missing_ok=True)
        args = ["open", "-g", "-n", "-a", str(LISTEN_APP), "--args", "--serve", str(d), "--parent", str(os.getpid())]
        if (os.environ.get("OPENPHYSICS_LISTEN_STANDIN") or os.environ.get("NAVIER_LISTEN_STANDIN")) == "1":
            args.append("--standin")  # tests: the plumbing, with no permission asked
        subprocess.run(args, capture_output=True, timeout=30)
        end = time.time() + wait_s  # the first time, macOS is asking the person: the answer can take a while
        while time.time() < end:
            r = self.ready()
            if r:
                return r
            time.sleep(0.05)
        return None

    def words(self, wav, lang, timeout=25):
        """(text, None) or (None, why)"""
        if not LISTEN_APP.exists():
            if not LISTEN.exists():
                return None, "the speech helper is not built (make xr)"
            with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as t:
                t.write(wav)
            try:
                r = subprocess.run([str(LISTEN), t.name, lang], capture_output=True, text=True, timeout=60)
            finally:
                os.unlink(t.name)
            return (r.stdout.strip(), None) if r.returncode == 0 and r.stdout.strip() else (None, (r.stderr.strip() or "no words were recognised")[:300])
        with self.lock:
            ready = self.ready() or self.start(90)
        if not ready:
            return None, "OpenPhysicsAI Listen did not start (is macOS asking whether it may recognise speech?)"
        if ready.get("status") != "authorized":
            return None, ("speech recognition is not allowed for OpenPhysicsAI Listen: System Settings > Privacy & Security > "
                          "Speech Recognition")
        d, rid = XR / "listen", "u%d" % time.time_ns()
        (d / (rid + ".wav")).write_bytes(wav)
        tmp = d / (rid + ".tmp")
        tmp.write_text(json.dumps({"wav": str(d / (rid + ".wav")), "lang": lang}))
        tmp.rename(d / (rid + ".req"))
        out, end = d / (rid + ".json"), time.time() + timeout
        try:
            while time.time() < end:
                if out.exists():
                    try:
                        r = json.loads(out.read_text())
                    except ValueError:
                        time.sleep(0.01)
                        continue
                    return (r["text"], None) if r.get("text") else (None, r.get("error") or "no words were recognised")
                time.sleep(0.01)
            return None, "recognition took longer than %d s" % timeout
        finally:
            for f in (d / (rid + ".wav"), out, d / (rid + ".req")):
                try:
                    f.unlink()
                except FileNotFoundError:
                    pass


LISTENER = Listener()
LIVE = live.Live(ask=lambda words, context: put_question(words, context), bump=lambda: EVENTS.bump())


def local_name(hostname):
    """a name that can only mean this computer"""
    h = (hostname or "").lower()
    return h in ("127.0.0.1", "localhost", "::1") or h.endswith(".localhost")


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "openphysics-xr"
    protocol_version = "HTTP/1.1"

    def refused(self, post, path):
        """why a request is not served, or None: a foreign Host (DNS rebinding), a page from another site, or a POST a
        page from another site could send without asking first (anything but JSON, or WAV for /voice)"""
        if not local_name(urllib.parse.urlsplit("//" + (self.headers.get("Host") or "")).hostname):
            return 403, "this bridge answers only to localhost names"
        origin = self.headers.get("Origin")
        if origin is not None and not local_name(urllib.parse.urlsplit(origin).hostname):
            return 403, "requests from another site are refused"
        if post:
            ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
            if ctype != {"/voice": "audio/wav", "/look": "image/jpeg"}.get(path, "application/json"):
                return 415, "a POST here is application/json (audio/wav for /voice, image/jpeg for /look)"
        return None

    def log_message(self, fmt, *args):  # one short line per request that is not a poll, a frame or a sentence
        if not any(p in self.path for p in ("/state", "/scene.glb", "/lab/frame.glb", "/speech", "/events", "/health")):
            sys.stderr.write("xr bridge: %s\n" % (fmt % args))

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def query(self):
        return {k: v[-1] for k, v in urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).items()}

    def lab(self):
        """the lab result the app shows, or an error response already sent"""
        st = read_state()
        lab = st.get("lab") if st.get("scene") == "lab" else None
        if not lab or not lab.get("path"):
            self.send(404, {"error": "no lab result is open in the app"})
            return None
        return lab

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        no = self.refused(False, path)
        if no:
            return self.send(no[0], {"error": no[1]})
        if path == "/state":
            page_seen()
            return self.send(200, read_state())
        if path == "/scene.glb":
            f = XR / "scene.glb"
            return self.send(200, f.read_bytes(), TYPES[".glb"]) if f.exists() else self.send(404, {"error": "no scene yet"})
        if path == "/lab/info":
            lab = self.lab()
            if lab is None:
                return None
            data, err = FRAMES.get(("info", lab["path"], lab.get("mtime")), [str(LABXR), lab["path"], "--info"])
            return self.send(200, data, TYPES[".json"]) if data else self.send(422, {"error": err})
        if path == "/lab/frame.glb":
            lab, q = self.lab(), self.query()
            if lab is None:
                return None
            if q.get("key") != lab.get("view_key"):
                return self.send(409, {"error": "the view changed", "key": lab.get("view_key")})
            try:
                f = int(q.get("f", ""))
            except ValueError:
                return self.send(400, {"error": "f: a frame number"})
            if not 0 <= f < int(lab.get("nframes") or 0):
                return self.send(400, {"error": "frame %d: the result has %s" % (f, lab.get("nframes"))})
            view = json.dumps(lab.get("view") or {}, separators=(",", ":"))
            data, err = FRAMES.get(("frame", lab["path"], lab.get("version", lab.get("mtime")), lab["view_key"], f),
                                   [str(LABXR), lab["path"], "--frame", str(f), "--view", view, "-o", "-"])
            return self.send(200, data, TYPES[".glb"]) if data else self.send(422, {"error": err})
        if path == "/speech":
            try:
                cid = int(self.query().get("id", ""))
            except ValueError:
                return self.send(400, {"error": "id: a sentence number"})
            wav = SPEAKER.get(cid)
            return self.send(200, wav, TYPES[".wav"]) if wav else self.send(404, {"error": "no speech %d (kept: the last 60)" % cid})
        if path == "/events":
            return self.events()
        rel = "index.html" if path in ("/", "") else path.lstrip("/")
        f = (WEB / rel).resolve()
        if WEB.resolve() not in f.parents or not f.is_file():  # nothing outside the page's own folder
            return self.send(404, {"error": "not found"})
        return self.send(200, f.read_bytes(), TYPES.get(f.suffix, "application/octet-stream"))

    def events(self):
        """the state again whenever it may have changed (server-sent events), a comment every 10 s to keep the line open"""
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        seen = -1
        try:
            while True:
                n = EVENTS.wait(seen, 10)
                page_seen()
                if n == seen:
                    self.wfile.write(b": ping\n\n")
                else:
                    seen = n
                    self.wfile.write(b"data: " + json.dumps(read_state()).encode() + b"\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            return

    def body(self, limit):
        n = int(self.headers.get("Content-Length", "0") or 0)
        if n <= 0 or n > limit:
            raise ValueError("a body of 1 byte to %d bytes is expected" % limit)
        return self.rfile.read(n)

    def do_POST(self):
        path = urllib.parse.urlsplit(self.path).path
        no = self.refused(True, path)
        if no:
            if int(self.headers.get("Content-Length", "0") or 0) <= 20 << 20:
                self.rfile.read(int(self.headers.get("Content-Length", "0") or 0))  # keep the connection's stream in step
            return self.send(no[0], {"error": no[1]})
        try:
            if path == "/voice":
                wav, q = self.body(20 << 20), self.query()
                text, why = LISTENER.words(wav, q.get("lang", "en-US"))
                if not text:
                    return self.send(422, {"error": why})
                if q.get("ask", "1") != "0":
                    put_question(text, q.get("ctx", ""))
                return self.send(200, {"text": text})
            if path == "/ask":
                j = json.loads(self.body(20000))
                text = str(j.get("text", ""))
                return self.send(200 if put_question(text, str(j.get("context") or "")) else 400, {"text": text})
            if path == "/command":
                cmd = str(json.loads(self.body(1000)).get("cmd", ""))
                if not command_allowed(cmd):
                    return self.send(400, {"error": "not a view command the headset may give", "cmd": cmd[:120]})
                drop("cmd", cmd)
                return self.send(200, {"cmd": cmd})
            if path == "/pins":
                pins = clean_pins(json.loads(self.body(100000)).get("pins"))
                page_seen(pins)
                return self.send(200, {"pins": len(pins)})
            if path == "/look":
                jpeg, q = self.body(4 << 20), self.query()
                lid, moment, view = q.get("id", ""), q.get("moment", ""), q.get("view", "")
                if not LOOK_ID.fullmatch(lid) or moment not in ("now", "question") or view not in ("ar", "vr", "screen"):
                    return self.send(400, {"error": "id: letters, digits, _ and - (up to 40); moment: now or question; view: ar, vr or screen"})
                if jpeg[:3] != b"\xff\xd8\xff" or jpeg[-2:] != b"\xff\xd9":
                    return self.send(400, {"error": "the picture is not a whole JPEG"})
                return self.send(200, put_look(lid, moment, view, jpeg))
            if path == "/health":
                h = json.loads(self.body(65536))
                if not isinstance(h, dict):
                    return self.send(400, {"error": "a JSON object"})
                return self.send(200, {"received": put_health(h)["received"]})
            if path == "/marks/clear":
                try:
                    (XR / "marks.json").unlink()
                except FileNotFoundError:
                    pass
                return self.send(200, {"marks": 0})
            if path == "/live/start":
                offer = str(json.loads(self.body(200000)).get("sdp") or "")
                if not offer.startswith("v="):
                    return self.send(400, {"error": "sdp: the headset's WebRTC offer"})
                try:
                    return self.send(200, LIVE.start(offer))
                except live.LiveError as e:
                    return self.send(e.status, {"error": str(e)})
            if path == "/live/stop":
                LIVE.stop()
                return self.send(200, {"live": False})
            if path == "/live/context":
                LIVE.context = " ".join(str(json.loads(self.body(20000)).get("context") or "").split())[:800]
                return self.send(200, {"ok": True})
            if path == "/live/handled":
                words = " ".join(str(json.loads(self.body(2000)).get("text") or "").split())[:200]
                if words:
                    LIVE.handled_command(words)
                return self.send(200, {"ok": True})
            if path == "/stop":
                SPEAKER.stop()
                if LIVE.active:
                    LIVE.cancel()
                    LIVE.send("instructions", "The person asked you to stop. Stop talking now and wait for them.")
                tmp = XR / "stop.tmp"
                tmp.write_text("stop\n")
                tmp.rename(XR / "stop.txt")
                return self.send(200, {"stopped": True})
            if path == "/answer":
                allow = bool(json.loads(self.body(1000)).get("allow"))
                tmp = XR / "answer.tmp"
                tmp.write_text("allow\n" if allow else "deny\n")
                tmp.rename(XR / "answer.txt")
                return self.send(200, {"allow": allow})
            return self.send(404, {"error": "not found"})
        except (ValueError, json.JSONDecodeError, subprocess.TimeoutExpired, AttributeError) as e:
            return self.send(400, {"error": str(e)[:300]})


def check_voice():
    """a sentence spoken by the Mac's own voice, turned back into words: proves speech recognition works here (the first
    run makes macOS ask once whether OpenPhysicsAI Listen may recognise speech: answer OK)"""
    global XR
    with tempfile.TemporaryDirectory() as d:
        XR = pathlib.Path(d) / "xr"
        XR.mkdir()
        aiff, wav = os.path.join(d, "q.aiff"), os.path.join(d, "q.wav")
        said = "make the wall of the lantern twice as thick"
        subprocess.run(["say", "-o", aiff, said], check=True)
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", aiff, wav], check=True)
        if LISTEN_APP.exists():
            print("starting OpenPhysicsAI Listen; the first time, macOS asks whether it may recognise speech: answer OK")
        t0 = time.time()
        text, why = LISTENER.words(pathlib.Path(wav).read_bytes(), "en-US")
        r = LISTENER.ready()
        if r:
            os.kill(int(r["pid"]), 15)
    print("said:  " + said)
    print("heard: " + (text or "(nothing) " + (why or "")) + "  (%.1f s)" % (time.time() - t0))
    return 0 if text else 1


def cleanup():
    """the app stops exporting once the folder is gone; only the link's own files are removed"""
    names = ("scene.glb", "scene.glb.tmp", "state.json", "state.json.tmp", "answer.txt", "answer.tmp", "page.json", "page.tmp", "marks.json",
             "stop.txt", "stop.tmp",
             "look-request.json", "look-now.jpg", "look-now.json", "look-question.jpg", "look-question.json", "health.json", "health.tmp")
    for f in (list(XR.glob("ask-*.txt")) + list(XR.glob("cmd-*.txt")) + list(XR.glob("tmp-*.txt")) + list(XR.glob("look-*.tmp"))
              + list(XR.glob("q-*.jpg")) + list(XR.glob("q-*.tmp")) + [XR / n for n in names]):
        try:
            f.unlink()
        except FileNotFoundError:
            pass
    for f in (XR / "listen").glob("*"):  # OpenPhysicsAI Listen's folder; the app ends with this bridge
        try:
            f.unlink()
        except OSError:
            pass
    for d in (XR / "listen", XR):
        try:
            d.rmdir()
        except OSError:
            pass


def main():
    global XR, VOICE
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8791)
    ap.add_argument("--dir", help="the headset folder (default ~/.openphysicsai/run/xr, or $OPENPHYSICS_XR_DIR)")
    ap.add_argument("--voice", help="the Mac voice that speaks the answers (say -v '?' lists them)")
    ap.add_argument("--check-voice", action="store_true", help="test the speech helper and exit")
    a = ap.parse_args()
    if a.check_voice:
        sys.exit(check_voice())
    if a.dir:
        XR = pathlib.Path(a.dir)
    VOICE = a.voice
    XR.mkdir(parents=True, exist_ok=True)
    if not LABXR.exists():
        print("build/labxr is missing (make xr): lab results will not reach the headset", flush=True)
    threading.Thread(target=watch_state, daemon=True).start()
    threading.Thread(target=SPEAKER.work, daemon=True).start()
    threading.Thread(target=SPEAKER.warm, daemon=True).start()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    srv.daemon_threads = True
    print("OpenPhysicsAI headset link on http://127.0.0.1:%d (the app writes into %s)" % (a.port, XR), flush=True)
    print("  in the Quest: http://openphysics-xr.localhost:8080  (adb reverse tcp:8080 tcp:8080; docs/xr.md)", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        LIVE.stop()
        if SESSION["last"]:  # the bridge stopped while the headset was in a session
            log_session(SESSION["last"])
        cleanup()


if __name__ == "__main__":
    main()
