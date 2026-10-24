#!/usr/bin/env python3
"""talk_test.py - the conversation with the AI through the headset link, end to end, without a model (docs/xr.md).

    python3 tools/xr/talk_test.py      (make test-xr; about half a minute)

It starts its own headless app (its own headset folder and runs folder) and its own bridge, with the stand-in for
Claude Code's session protocol (tools/xr/claude_standin.py) as the agent: no AI is started and nothing is spent.
Everything is removed when it passes.

Criteria, written 2026-10-07 before the first run:
 1. One conversation, one process: three questions from the headset are answered by the same process (one pid in the
    stand-in's log), started once with --input-format stream-json and --include-partial-messages, each question
    arriving as one stream-json user message that holds its words.
 2. The words as they come: while an answer streams, the app's state.json shows the turn's words growing: a state with
    part of the answer and done false comes before the one with all of it and done true.
 3. Spoken sentence by sentence: for an answer whose second sentence comes 1.5 s after the first, the bridge has the
    first sentence as speech (a RIFF WAVE of at least 0.3 s) before the second sentence leaves the stand-in, and within
    0.8 s of the first one leaving it; the sentences are spoken in order and once each (the finished answer is not
    spoken again).
 4. Stop: POST /stop while an answer streams ends that turn within 1 s; the stand-in got an interrupt control request
    and was not killed (the same process answers the next question); no speech of the stopped turn is made after the
    stop.
 5. Talking over it: a question from the headset while the AI is answering stops that answer and is asked next, in
    the same process.
 6. The fast model: questions from the headset run with the voice model (haiku unless set otherwise): the stand-in was
    started with --model haiku, or told set_model haiku before the question.
 7. A session that ends is resumed: when the stand-in's process is killed, the next question starts a new one with
    --resume and the session id the first one announced.
 8. The voice: build/openphysics-voice turns a sentence into a mono 16-bit RIFF WAVE whose length is within 25 % of what
    `say` makes of the same sentence, and after its first sentence takes under 0.2 s for each of the next three.
    Amended openly 2026-10-08: the speed is judged against `say` timed on the same sentence in the same run (each of
    the next three under a quarter of its time), the times in ms still printed. On this shared laptop the helper took
    42 to 120 ms at load averages of 5 to 15 and 132 to 984 ms at 27 to 45, with no change in its code, so a fixed
    wall-clock limit judged the laptop's load, not the helper; what the check guards is that the kept synthesizer is far
    faster than starting `say` (1.0 to 1.5 s a sentence when this was built).
 9. What they saw (added the same day, before its first run): a question asked through the bridge right after the page
    posted a picture taken at the question reaches the stand-in as one user message whose first block is that JPEG
    (base64 of the same bytes) and whose text holds the words; the copy the bridge made for it is gone from the folder
    afterwards; a question with no fresh picture carries none.
10. Ready before the first question (added the same day, before its first run): with the headset page connected (its
    polls of /state), the app starts the conversation's process before any question is asked, and sends it nothing.
11. Speech into words through the background app (added the same day, before its automated run): with
    build/OpenPhysicsAIListen.app in its stand-in mode (it answers "stand-in: N bytes" and asks macOS for no
    permission), three WAVs posted to /voice come back as their words, the first within 3 s (the app is started then)
    and the next two within 0.5 s each, all from one app process; the app ends when the bridge ends.
12. What is computing (added the same day, before its first run): a simulation the AI started that is still running
    (agent-runs/NAME.pid holding a live process, its log ending "frame 3/40") is in the state as computing NAME, 0.07 or
    0.08 done, within 2 s; when its process has ended it is gone from the state within 2 s.
13. The provider's voice, GPT-Live (added 2026-10-08, written before its first run), against a stand-in for OpenAI's
    live sessions (tools/xr/live_standin.py: no key, no network, nothing spent). Without a key, /live/start says so
    (503) and the state names the Mac voice. With a key: /live/start sends OpenAI the documented session (model
    gpt-live-1, client delegation, the voice, instructions that name the delegation and the view commands) with the
    browser's SDP offer and the key in the Authorization header only, and gives the page OpenAI's SDP answer; the bridge
    then attaches its sideband to that session, and the state says the voice is live.
14. Delegation: words the person said (input transcript fragments) followed by session.delegation.created reach the
    app's agent as one question holding those words and the headset's context; when the agent's answer is complete,
    it goes back as session.commentary.append with that delegation's id and the answer's words, within 1 s of the
    answer ending; while the agent works longer than 3 s, session.thinking.append tells the voice what it is doing; the
    bridge's own voice makes no sound of that answer while the provider's voice is live.
15. View commands said aloud: a command the headset did itself (POST /live/handled) is told to the voice as quiet
    context, and a delegation that follows within 3 s for it is answered "Done" without asking the agent.
16. The person's and the voice's words reach the page: the input and output transcript fragments are in the state the
    page reads (live.you, live.ai) within 0.5 s; /live/stop closes the session (session.close on the sideband).
17. A run that stopped (added 2026-10-08, written before its first run): a simulation the AI started whose process has
    ended without labrun's record is in the state's "stopped" list within 2 s, with why: labrun's own words when its
    log ends "labrun: ..." (that prefix taken off), else that it ended before its result was complete; a run whose log
    ends with labrun's record is not in it.
18. A finished run not yet reaped (added 2026-10-08, written before its first run): a run whose process has ended but
    whose parent has not collected it (a zombie: openphysics-mcp collects a finished labrun only when lab_run_status asks
    about it) leaves the state's computing list within 2 s and is listed as stopped.
19. A run shown while it computes (added 2026-10-08, written before its first run): with agent-runs/NAME.pid holding a
    live process and NAME.lab written frame by frame (real frames of a short flow_volume run) into the results folder,
    the app shows it once it holds two frames (lab.path is it, lab.growing true, within 4 s); lab.nframes follows the
    frames written while lab.version and lab.view_key stay the same; when the process ends lab.growing turns false
    within 3 s. Amended openly after its first run the same day: the view key changed between two and three frames
    because the third frame widened the colour range, which is part of the view, as it must be for the headset's frames
    to share one range; what is checked is that the key stays the same whenever lab.view does (it no longer changes
    because the file grew, as it did with the file's time in it).
"""
import json, os, pathlib, shutil, signal, subprocess, sys, tempfile, time, urllib.error, urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "xr"))
from xr_test import Proc, check, free_port, http, wait  # noqa: E402  (the same helpers as the link's test)
import xr_test  # noqa: E402

STANDIN = ROOT / "tools" / "xr" / "claude_standin.py"


def wav_seconds(b):
    """the length of a RIFF WAVE, from its fmt and data chunks"""
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        return 0
    i, rate, frame, data = 12, 0, 0, 0
    while i + 8 <= len(b):
        cid, n = b[i:i + 4], int.from_bytes(b[i + 4:i + 8], "little")
        if cid == b"fmt ":
            rate, frame = int.from_bytes(b[i + 12:i + 16], "little"), int.from_bytes(b[i + 20:i + 22], "little")
        elif cid == b"data":
            data = min(n, len(b) - i - 8)
        i += 8 + n + (n & 1)
    return data / (rate * frame) if rate and frame else 0


def main():
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-talktest-"))
    procs = []
    try:
        for exe in ("openphysics", "openphysics-mcp", "build/openphysics-voice"):
            if not (ROOT / exe).exists():
                print("missing %s: make openphysics openphysics-mcp build/openphysics-voice" % exe)
                return 1
        xr, log = tmp / "xr", tmp / "standin.log"
        xr.mkdir()
        (tmp / "runs").mkdir()
        # the app's own home, short (its agent's socket path must stay under macOS's 104 bytes): its lab results, its
        # agent's files, nothing of the owner's; removed at the end
        home = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-th-", dir="/tmp"))
        (home / "OpenPhysicsAI-Projects" / "lab" / "agent-runs").mkdir(parents=True)
        live_port, live_log = free_port(), tmp / "live.log"
        procs.append(Proc([sys.executable, str(ROOT / "tools" / "xr" / "live_standin.py"), str(live_port), str(live_log)], str(tmp / "live_standin.out")))
        env = dict(os.environ, OPENPHYSICS_XR="1", OPENPHYSICS_XR_DIR=str(xr), OPENPHYSICS_AGENT_RUNS_DIR=str(tmp / "runs"), STANDIN_LOG=str(log),
                   OPENPHYSICS_LISTEN_STANDIN="1", HOME=str(home), OPENAI_API_KEY="sk-standin-not-a-key",
                   OPENPHYSICS_OPENAI_BASE="http://127.0.0.1:%d" % live_port, OPENPHYSICS_LIVE_NO_KEYCHAIN="1")
        cmd = 'agent command "%s" -p $OPENPHYSICS_PROMPT --mcp-config $OPENPHYSICS_MCP_CONFIG --output-format stream-json --verbose' % STANDIN
        app = Proc([str(ROOT / "openphysics"), "--headless", "--size", "320x240", "--exec", cmd + "; frames 100000000"], str(tmp / "app.log"), env)
        procs.append(app)
        state = lambda: json.loads((xr / "state.json").read_text())  # noqa: E731
        check(wait(lambda: "agent" in state(), 30) is not None, "the headless app writes the headset's state, with the AI's part")
        port = free_port()
        bridge = Proc([sys.executable, str(ROOT / "tools" / "xr" / "xr_bridge.py"), "--port", str(port), "--dir", str(xr)], str(tmp / "bridge.log"), env)
        procs.append(bridge)
        base = "http://127.0.0.1:%d" % port
        wait(lambda: http(base + "/state")[0] == 200, 15)

        def events():
            return [json.loads(l) for l in log.read_text().splitlines()] if log.exists() else []

        # 10: the page polls (as /state does), the app starts the conversation and asks nothing
        warm = wait(lambda: http(base + "/state")[0] == 200 and [e for e in events() if e["event"] == "start"], 15)
        time.sleep(0.5)
        check(bool(warm) and not [e for e in events() if e["event"] == "user"],
              "10. with the page connected, the conversation's process starts before any question and is sent nothing")

        def ask(text):  # as the bridge leaves a question from the headset
            (xr / "tmp-q.txt").write_text(text + "\n")
            (xr / "tmp-q.txt").rename(xr / ("ask-%d.txt" % time.time_ns()))

        def bstate():
            return json.loads(http(base + "/state")[1])

        def turn_done(n):
            s = state().get("agent", {}).get("say") or {}
            return s.get("turn") == n and s.get("done")

        # 1, 2, 3: the first question, watched as it streams (the bridge's voice loaded first, as it is long before anyone
        # speaks in the headset)
        wait(lambda: bstate().get("speech", {}).get("voice"), 20)
        ask("first question about the rising plume")
        seen, partial = [], False
        end = time.time() + 15
        while time.time() < end:
            try:
                s = state().get("agent", {}).get("say") or {}
            except (OSError, ValueError):
                continue
            if s.get("turn") == 1:
                seen.append((time.time(), s.get("text", ""), bool(s.get("done"))))
                partial = partial or (bool(s.get("text")) and not s.get("done"))
                if s.get("done"):
                    break
            time.sleep(0.02)
        full = seen[-1][1] if seen else ""
        check(partial and seen and seen[-1][2] and full.startswith("I heard you ask about first question about the rising"),
              "2. the words grow in the state while the answer streams, then the turn is done: %r" % full[:80])
        emits = [e for e in events() if e["event"] == "emit" and e["turn"] == 1]
        sp = wait(lambda: (lambda c: c if len(c) >= 2 else None)([c for c in bstate().get("speech", {}).get("chunks", []) if c.get("turn") == 1]), 5) or []
        first = sp[0] if sp else {}
        t_s1 = emits[0]["t"] if emits else 0
        t_s2 = emits[1]["t"] if len(emits) > 1 else 0
        code, wav = http(base + "/speech?id=%s" % first.get("id")) if first else (0, b"")
        secs = wav_seconds(wav)
        check(first and first.get("made", 1e18) < t_s2 and first["made"] - t_s1 < 0.8 and code == 200 and wav[8:12] == b"WAVE" and secs >= 0.3,
              "3. the first sentence is speech %.2f s after it left the stand-in (in the state after %.2f s), %.2f s before the second (%.2f s of WAVE)"
              % (first.get("made", 0) - t_s1, first.get("seen", 0) - t_s1, t_s2 - first.get("made", 0), secs))
        time.sleep(1.0)
        allc = [c for c in bstate().get("speech", {}).get("chunks", []) if c.get("turn") == 1]
        check([c["text"] for c in allc] == ["I heard you ask about first question about the rising.", "This second sentence ends the answer."],
              "3. spoken in order and once each: %s" % [c["text"] for c in allc])
        starts = [e for e in events() if e["event"] == "start"]
        users = [e for e in events() if e["event"] == "user"]
        a0 = starts[0]["argv"] if starts else []
        check(len(starts) == 1 and "--include-partial-messages" in a0 and "stream-json" in a0 and len(users) == 1
              and users[0]["text"].startswith("first question about the rising plume"),
              "1. one process, started for a session; the question arrived as one user message")
        check(("--model" in a0 and a0[a0.index("--model") + 1] == "haiku") or any(e["event"] == "control" and e.get("model") == "haiku" for e in events()),
              "6. a question from the headset runs with the voice model, haiku")
        for k, q in enumerate(("second question", "third question")):
            ask(q)
            check(wait(lambda: turn_done(k + 2), 10) is not None, "the %s is answered" % q)
        pids = {e["pid"] for e in events()}
        check(len(pids) == 1 and len([e for e in events() if e["event"] == "user"]) == 3, "1. three questions, one process (pid %s)" % pids)

        # 4: stop while it speaks
        ask("please give me a long answer")
        wait(lambda: len([e for e in events() if e["event"] == "emit" and e["turn"] == 4]) >= 2, 10)
        t_stop = time.time()
        code, _ = http(base + "/stop", b"{}")
        ended = wait(lambda: not state().get("agent", {}).get("busy") and (state().get("agent", {}).get("say") or {}).get("done"), 3)
        took = time.time() - t_stop
        time.sleep(0.6)
        late = [c for c in bstate().get("speech", {}).get("chunks", []) if c.get("turn") == 4 and c.get("made", 0) > t_stop + 0.15]
        check(code == 200 and ended is not None and took < 1.0 and any(e["event"] == "control" and e["subtype"] == "interrupt" for e in events())
              and not late, "4. /stop ends the answer in %.2f s by an interrupt; no speech of it is made after the stop" % took)
        ask("fifth question after the stop")
        check(wait(lambda: turn_done(5), 10) is not None and len({e["pid"] for e in events()}) == 1, "4. the same process answers the next question")

        # 5: talking over it
        ask("another long answer please")
        wait(lambda: len([e for e in events() if e["event"] == "emit" and e["turn"] == 6]) >= 1, 10)
        ask("the question said over it")
        ok5 = wait(lambda: turn_done(7) and "said over it" in [e for e in events() if e["event"] == "user"][-1]["text"], 10)
        stopped6 = any(e["event"] == "interrupted" and e["turn"] == 6 for e in events())
        check(ok5 is not None and stopped6 and len({e["pid"] for e in events()}) == 1, "5. a question said over an answer stops it and is asked next")

        # 7: resumed after the process ends
        old = ([e for e in events() if e["event"] == "start"] or [{"pid": 0}])[0]
        if old["pid"]:
            os.kill(old["pid"], signal.SIGKILL)
        time.sleep(0.5)
        ask("a question after the crash")
        wait(lambda: len([e for e in events() if e["event"] == "start"]) == 2, 10)
        st2 = [e for e in events() if e["event"] == "start"]
        a2 = st2[-1]["argv"] if len(st2) == 2 else []
        sid = state().get("agent", {}).get("session") or ""
        check(len(st2) == 2 and "--resume" in a2 and a2[a2.index("--resume") + 1] == sid and sid,
              "7. after the process ended, the next question resumes session %s" % sid)
        check(wait(lambda: turn_done(8), 10) is not None, "7. and it is answered")

        # 9: the picture the page took as the question was asked goes with it
        jpeg = xr_test.TEST_JPEG
        code9, _ = http(base + "/look?id=q9&moment=question&view=ar", jpeg, "image/jpeg")
        n_users = len([e for e in events() if e["event"] == "user"])
        http(base + "/ask", json.dumps({"text": "what is this hot spot", "context": "the person points at part 'block'"}).encode())
        got = wait(lambda: [e for e in events() if e["event"] == "user"][n_users:] or None, 10)
        u9 = got[0] if got else {}
        import base64, hashlib
        check(code9 == 200 and u9.get("images") == [hashlib.sha256(jpeg).hexdigest()] and u9.get("first") == "image"
              and u9.get("text", "").startswith("what is this hot spot") and not list(xr.glob("q-*.jpg")),
              "9. the picture taken as the question was asked reaches the AI with it, as its first block; its copy is removed")
        wait(lambda: turn_done(state().get("agent", {}).get("say", {}).get("turn")), 10)
        n_users = len([e for e in events() if e["event"] == "user"])
        time.sleep(5.2)  # the picture is now older than 5 s
        http(base + "/ask", json.dumps({"text": "and without a picture", "context": ""}).encode())
        got = wait(lambda: [e for e in events() if e["event"] == "user"][n_users:] or None, 10)
        check(bool(got) and got[0].get("images") == [], "9. a question with no fresh picture carries none")

        # 13 to 16: the provider's voice (GPT-Live) against its stand-in
        def lived():
            return [json.loads(l) for l in live_log.read_text().splitlines()] if live_log.exists() else []

        def sent(kind):  # what the bridge sent down the sideband, of one kind
            return [e["data"] for e in lived() if e["event"] == "client" and e.get("data", {}).get("type") == kind]

        def drive(ev):
            req = urllib.request.Request("http://127.0.0.1:%d/drive" % live_port, data=json.dumps(ev).encode(), headers={"Content-Type": "application/json"})
            try:
                return urllib.request.urlopen(req, timeout=5).status
            except urllib.error.HTTPError as e:
                return e.code
        nokey = dict(env, OPENPHYSICS_LIVE_NO_KEYCHAIN="1")
        nokey.pop("OPENAI_API_KEY")
        (tmp / "xr-nokey").mkdir()
        port3 = free_port()
        b3 = Proc([sys.executable, str(ROOT / "tools" / "xr" / "xr_bridge.py"), "--port", str(port3), "--dir", str(tmp / "xr-nokey")], str(tmp / "bridge3.log"), nokey)
        wait(lambda: http("http://127.0.0.1:%d/state" % port3)[0] == 200, 15)
        offer = "v=0\r\no=- 42 2 IN IP4 127.0.0.1\r\ns=headset offer\r\n"
        c3, r3 = http("http://127.0.0.1:%d/live/start" % port3, json.dumps({"sdp": offer}).encode())
        v3 = json.loads(http("http://127.0.0.1:%d/state" % port3)[1]).get("voice_provider")
        b3.stop()
        check(c3 == 503 and b"OPENAI_API_KEY" in r3 and v3 == "mac", "13. without a key, /live/start says so (%d) and the voice is the Mac's (%s)" % (c3, v3))
        wait(lambda: http("http://127.0.0.1:%d/drive" % live_port, b"{}")[0] in (200, 409), 10)
        c13, r13 = http(base + "/live/start", json.dumps({"sdp": offer}).encode())
        j13 = json.loads(r13) if c13 == 200 else {}
        created = ([e for e in lived() if e["event"] == "create"] or [{}])[-1]
        body = created.get("body") or {}
        sess = body.get("session") or {}
        attached = wait(lambda: [e for e in lived() if e["event"] == "attach"] or None, 5)
        st13 = bstate()
        check(c13 == 200 and j13.get("sdp", "").startswith("v=0") and "standin answer" in j13["sdp"] and j13.get("session") == "sess_standin"
              and created.get("authorization") == "Bearer sk-standin-not-a-key" and "sk-standin" not in json.dumps(body)
              and sess.get("model") == "gpt-live-1" and sess.get("delegation") == {"type": "client"}
              and (sess.get("audio") or {}).get("output", {}).get("voice") == "marin" and "Delegate" in sess.get("instructions", "")
              and "view commands" in sess.get("instructions", "") and body.get("transport") == {"type": "webrtc", "sdp": offer}
              and attached and attached[0].get("authorization") == "Bearer sk-standin-not-a-key"
              and st13.get("live", {}).get("active") and st13.get("voice_provider") == "openai-live",
              "13. /live/start: OpenAI gets the documented session and the offer, the key only in the Authorization header; the page "
              "gets the answer; the sideband is attached; the voice is live")
        # 14: the person's words and a delegation reach the agent, its answer goes back to be spoken
        http(base + "/live/context", json.dumps({"context": "the person points at part 'block'"}).encode())
        n_users = len([e for e in events() if e["event"] == "user"])
        t16 = time.time()
        for d in ("what is ", "the hottest part"):
            drive({"type": "session.input_transcript.delta", "delta": d, "start_ms": 0, "end_ms": 100})
        you = wait(lambda: "what is the hottest part" in bstate().get("live", {}).get("you", "") or None, 2)
        you_s = time.time() - t16
        drive({"type": "session.delegation.created", "offset_ms": 900, "delegation": {"id": "del_1", "type": "delegation", "target": "client"}})
        q14 = wait(lambda: [e for e in events() if e["event"] == "user"][n_users:] or None, 10) or [{}]
        said14 = wait(lambda: [d for d in sent("session.commentary.append") if d.get("delegation_id") == "del_1"] or None, 15) or [{}]
        last_emit = max([e["t"] for e in events() if e["event"] == "emit"] or [0])
        c_at = ([e["t"] for e in lived() if e["event"] == "client" and e["data"].get("delegation_id") == "del_1"
                 and e["data"].get("type") == "session.commentary.append"] or [1e18])[0]
        turn14 = (state().get("agent", {}).get("say") or {}).get("turn")
        spoken14 = [c for c in bstate().get("speech", {}).get("chunks", []) if c.get("turn") == turn14]
        check(q14[0].get("text", "").startswith("what is the hottest part") and "points at part 'block'" in q14[0].get("text", "")
              and said14[0].get("content", "").startswith("I heard you ask about what is the hottest part") and c_at - last_emit < 1.0
              and not spoken14, "14. the person's words reach the agent with the headset's context; its answer goes back as commentary for "
              "del_1 %.2f s after it ended; the bridge's own voice stays silent" % (c_at - last_emit))
        drive({"type": "session.input_transcript.delta", "delta": "please give me a long answer", "start_ms": 0, "end_ms": 100})
        drive({"type": "session.delegation.created", "offset_ms": 900, "delegation": {"id": "del_2", "type": "delegation", "target": "client"}})
        thought = wait(lambda: [d for d in sent("session.thinking.append") if d.get("delegation_id") == "del_2"] or None, 8)
        done2 = wait(lambda: [d for d in sent("session.commentary.append") if d.get("delegation_id") == "del_2"] or None, 20)
        check(bool(thought) and bool(done2), "14. while the agent works longer than 3 s the voice is told what it does (%s); then its answer"
              % (thought[0]["content"][:60] if thought else None))
        # 15: a view command the headset did itself
        n_users = len([e for e in events() if e["event"] == "user"])
        http(base + "/live/handled", json.dumps({"text": "next frame"}).encode())
        told = wait(lambda: [d for d in sent("session.thinking.append") if d.get("delegation_id") is None and "next frame" in d.get("content", "")] or None, 3)
        drive({"type": "session.input_transcript.delta", "delta": "next frame", "start_ms": 0, "end_ms": 100})
        drive({"type": "session.delegation.created", "offset_ms": 900, "delegation": {"id": "del_3", "type": "delegation", "target": "client"}})
        said15 = wait(lambda: [d for d in sent("session.commentary.append") if d.get("delegation_id") == "del_3"] or None, 5) or [{}]
        time.sleep(0.5)
        check(bool(told) and said15[0].get("content", "").startswith("Done") and len([e for e in events() if e["event"] == "user"]) == n_users,
              "15. a view command the headset did is told to the voice, and its delegation is answered Done without the agent")
        # 16: both sides' words reach the page; the session ends
        t16b = time.time()
        drive({"type": "session.output_transcript.delta", "delta": "The hottest part is the block", "start_ms": 0, "end_ms": 100})
        ai = wait(lambda: "The hottest part is the block" in bstate().get("live", {}).get("ai", "") or None, 2)
        ai_s = time.time() - t16b
        http(base + "/live/stop", b"{}")
        closed = wait(lambda: sent("session.close") or None, 3)
        check(bool(you) and you_s < 0.5 and bool(ai) and ai_s < 0.5 and bool(closed) and not bstate().get("live", {}).get("active"),
              "16. what the person and the voice say reach the page (%.2f s, %.2f s); /live/stop closes the session" % (you_s, ai_s))

        # 12: what is computing: a run the AI started, still going, and how far along it is
        runs = home / "OpenPhysicsAI-Projects" / "lab" / "agent-runs"
        sleeper = subprocess.Popen(["sleep", "30"])
        (runs / "fire_200kw.log").write_text("labrun: fire\n  frame   2/40  t 0.1 s\n  frame   3/40  t 0.15 s\n")
        (runs / "fire_200kw.pid").write_text("%d\n" % sleeper.pid)
        comp = wait(lambda: (state().get("agent", {}).get("computing") or None), 3) or []
        check(len(comp) == 1 and comp[0].get("name") == "fire_200kw" and comp[0].get("done") in (0.07, 0.08),
              "12. a simulation the AI started is in the state while it computes: %s" % comp)
        sleeper.kill()
        sleeper.wait()
        check(wait(lambda: state().get("agent", {}).get("computing") == [], 3) is not None, "12. and gone from it when its process ends")

        # 17: a run that stopped without its record, and why; one that finished is not listed
        gone = subprocess.Popen(["true"])
        gone.wait()
        (runs / "flag_v2.log").write_text("labrun: flow\nlabrun: lbm3d: the lattice speed 0.2 is above 0.1\n")
        (runs / "flag_v2.pid").write_text("%d\n" % gone.pid)
        (runs / "flag_v3.log").write_text("  frame  40/40\n{\"scenario\":\"x\",\"frames\":40}\n")
        (runs / "flag_v3.pid").write_text("%d\n" % gone.pid)

        def stopped_now():
            got = {r.get("name"): r.get("why") for r in state().get("agent", {}).get("stopped") or []}
            return got if "flag_v2" in got and "fire_200kw" in got else None
        got = wait(stopped_now, 3) or {}
        check(got.get("flag_v2") == "lbm3d: the lattice speed 0.2 is above 0.1" and got.get("fire_200kw") == "it ended before its result was complete"
              and "flag_v3" not in got, "17. runs that stopped without their record are listed with why, a finished one is not: %s" % got)
        for f in ("flag_v2.log", "flag_v2.pid", "flag_v3.log", "flag_v3.pid", "fire_200kw.log", "fire_200kw.pid"):
            (runs / f).unlink(missing_ok=True)

        # 18: a run that has ended but is not reaped yet (a zombie) is not computing
        zombie = subprocess.Popen(["sleep", "30"])
        (runs / "wave_v2.log").write_text("  frame   2/40  t 0.1 s\n")
        (runs / "wave_v2.pid").write_text("%d\n" % zombie.pid)
        seen = wait(lambda: any(r.get("name") == "wave_v2" for r in state().get("agent", {}).get("computing") or []) or None, 3)
        zombie.kill()  # not waited for: a zombie until the end of this check
        gone = wait(lambda: not any(r.get("name") == "wave_v2" for r in state().get("agent", {}).get("computing") or []) or None, 3)
        listed = wait(lambda: any(r.get("name") == "wave_v2" for r in state().get("agent", {}).get("stopped") or []) or None, 3)
        check(bool(seen) and bool(gone) and bool(listed), "18. a run ended but not reaped leaves computing and is listed as stopped")
        zombie.wait()
        for f in ("wave_v2.log", "wave_v2.pid"):
            (runs / f).unlink(missing_ok=True)

        # 19: a run shown while it computes, from real frames written one at a time
        src = tmp / "grow-src"
        src.mkdir()
        sc = json.loads((ROOT / "examples" / "lab" / "flow_volume.json").read_text())
        sc["run"]["end_time_s"], sc["run"]["frames"] = 0.04, 4
        (src / "s.json").write_text(json.dumps(sc))
        subprocess.run([str(ROOT / "build" / "labrun"), str(src / "s.json"), str(src / "s.lab"), "--quiet"], capture_output=True, timeout=120)
        whole = (src / "s.lab").read_bytes() if (src / "s.lab").exists() else b""
        hl = int.from_bytes(whole[8:12], "little") if len(whole) > 12 else 0
        head, frames, at = whole[:12 + hl], [], 12 + hl
        while at + 12 <= len(whole) and whole[at:at + 4] == b"FRAM":
            n = int.from_bytes(whole[at + 4:at + 12], "little")
            frames.append(whole[at:at + 12 + n])
            at += 12 + n
        res = home / "OpenPhysicsAI-Projects" / "lab"
        runner = subprocess.Popen(["sleep", "60"])
        (runs / "flowgrow.pid").write_text("%d\n" % runner.pid)
        growing = res / "flowgrow.lab"
        first, more, early = {}, {}, ""
        if len(frames) >= 3:
            with open(growing, "wb") as g:
                g.write(head + frames[0])
                g.flush()
                time.sleep(1.5)
                early = (state().get("lab") or {}).get("path", "")
                g.write(frames[1])
                g.flush()
                first = dict(wait(lambda: (lambda l: l if l.get("path") == str(growing) and l.get("growing") else None)(state().get("lab") or {}), 4) or {})
                g.write(frames[2])
                g.flush()
                more = wait(lambda: (lambda l: l if l.get("nframes") == 3 else None)(state().get("lab") or {}), 4) or {}
        check(len(frames) >= 3 and early != str(growing) and first.get("nframes") == 2 and more.get("nframes") == 3
              and more.get("version") == first.get("version")
              and (more.get("view_key") == first.get("view_key")) == (more.get("view") == first.get("view")),
              "19. a computing run is shown from its second frame and grows with the same version and view key: %s" %
              {k: (first.get(k), more.get(k)) for k in ("nframes", "version", "view_key", "growing")})
        runner.kill()
        runner.wait()
        ended = wait(lambda: (state().get("lab") or {}).get("growing") is False or None, 3)
        check(bool(ended), "19. when its process ends it is no longer growing")
        (runs / "flowgrow.pid").unlink(missing_ok=True)

        # 11: speech into words through OpenPhysicsAI Listen (stand-in mode: no permission asked)
        if (ROOT / "build" / "OpenPhysicsAIListen.app").exists():
            wav = (b"RIFF" + (36 + 16000).to_bytes(4, "little") + b"WAVEfmt " + bytes([16, 0, 0, 0, 1, 0, 1, 0, 0x80, 0x3e, 0, 0, 0, 0x7d, 0, 0, 2, 0, 16, 0])
                   + b"data" + (16000).to_bytes(4, "little") + bytes(16000))
            took, words = [], []
            for _ in range(3):
                t0 = time.time()
                code, b = http(base + "/voice?lang=en-US&ask=0", wav, "audio/wav")
                took.append(time.time() - t0)
                words.append(json.loads(b).get("text") if code == 200 else None)
            ready = json.loads((xr / "listen" / "ready.json").read_text()) if (xr / "listen" / "ready.json").exists() else {}
            check(words == ["stand-in: %d bytes" % len(wav)] * 3 and took[0] < 3 and max(took[1:]) < 0.5 and ready.get("standin"),
                  "11. speech into words through OpenPhysicsAI Listen: %s s, one app (pid %s)" % ([round(t, 2) for t in took], ready.get("pid")))
            listen_pid = ready.get("pid")
        else:
            print("  (build/OpenPhysicsAIListen.app is not built: 11 skipped)")
            listen_pid = None

        # 8: the voice
        v = subprocess.Popen([str(ROOT / "build" / "openphysics-voice")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        ready = json.loads(v.stdout.readline())
        times, lens = [], []
        sentence = "The peak temperature is near the outlet, about four hundred kelvin."
        for i in range(4):
            t0 = time.time()
            v.stdin.write(json.dumps({"id": i, "text": sentence, "out": str(tmp / ("v%d.wav" % i))}) + "\n")
            v.stdin.flush()
            r = json.loads(v.stdout.readline())
            times.append(time.time() - t0)
            lens.append(r.get("seconds", 0))
        v.stdin.close()
        v.wait(10)
        t_say = time.time()
        subprocess.run(["say", "-o", str(tmp / "say.wav"), "--data-format=LEI16@22050", sentence], check=True)
        t_say = time.time() - t_say
        say_s = wav_seconds((tmp / "say.wav").read_bytes())
        w = (tmp / "v1.wav").read_bytes()
        check(ready.get("ready") and w[:4] == b"RIFF" and w[22] == 1 and w[34] == 16 and say_s > 0 and abs(lens[1] / say_s - 1) < 0.25
              and max(times[1:]) < 0.25 * t_say, "8. %s: %.2f s of speech (say: %.2f s), %s ms each after the first, say took %d ms"
              % (ready.get("voice"), lens[1], say_s, [round(t * 1000) for t in times[1:]], round(t_say * 1000)))
    finally:
        for p in procs:
            try:
                p.stop()
            except Exception:
                pass
    if locals().get("listen_pid"):
        def gone():
            try:
                os.kill(listen_pid, 0)
                return None
            except OSError:
                return True
        check(wait(gone, 5) is not None, "11. OpenPhysicsAI Listen ends when the bridge ends")
    if locals().get("home"):
        shutil.rmtree(home, ignore_errors=True)
    if xr_test.failures:
        print("TALK TEST FAILED (%d); the workspace is kept for a look: %s" % (xr_test.failures, tmp))
        return 1
    shutil.rmtree(tmp, ignore_errors=True)
    print("TALK TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
