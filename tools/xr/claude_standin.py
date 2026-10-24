#!/usr/bin/env python3
"""claude_standin.py - a stand-in for Claude Code's session protocol, for testing the conversation without a model.

The app keeps one Claude Code process for a whole conversation: `claude -p --input-format stream-json --output-format
stream-json --include-partial-messages ...`, one JSON user message a line on its stdin, its answer streamed back as
events. This script speaks that protocol with scripted answers and records what it was given, so tools/xr/talk_test.py
can check the app and the headset bridge in seconds. It holds no key and calls no model.

    STANDIN_LOG=path  where it writes one JSON line per thing it saw or did (start, user, control, emit, interrupted)

Answers: two sentences, the first 0.2 s after the question and the second 1.5 s later; a question containing "long
answer" gets twenty sentences 0.3 s apart. A control_request interrupt ends the answer being given (the turn ends
with error_during_execution, as Claude Code's does); set_model changes the model it reports. Words are streamed one at a
time as content_block_delta events, as --include-partial-messages streams them.
"""
import base64
import hashlib
import json
import os
import sys
import threading
import time
import uuid

LOG = os.environ.get("STANDIN_LOG")
OUT = threading.Lock()


def log(**kv):
    if LOG:
        with open(LOG, "a") as f:
            f.write(json.dumps(dict(kv, pid=os.getpid(), t=time.time())) + "\n")


def send(obj):
    with OUT:
        sys.stdout.write(json.dumps(obj) + "\n")
        sys.stdout.flush()


args = sys.argv[1:]
log(event="start", argv=args)
session = "--input-format" in args and args[args.index("--input-format") + 1] == "stream-json"
model = args[args.index("--model") + 1] if "--model" in args else "standin-default"
sid = args[args.index("--resume") + 1] if "--resume" in args else str(uuid.uuid4())
send({"type": "system", "subtype": "init", "session_id": sid, "model": model, "claude_code_version": "standin",
      "mcp_servers": [], "tools": []})

interrupt = threading.Event()
inbox = []
cond = threading.Condition()


def answer(text, turn):
    """stream the scripted answer; True if it ran to its end"""
    words = text.split()
    if "long answer" in text:
        sentences, gap = ["Sentence number %d of the long answer." % (i + 1) for i in range(20)], 0.3
    else:
        sentences, gap = ["I heard you ask about %s." % " ".join(words[:5]).rstrip(".?!"), "This second sentence ends the answer."], 1.5
    send({"type": "stream_event", "session_id": sid, "event": {"type": "message_start", "message": {"model": model}}})
    send({"type": "stream_event", "session_id": sid, "event": {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}})
    said = ""
    time.sleep(0.2)
    for k, s in enumerate(sentences):
        if k:
            time.sleep(gap)
        for w in (s + " ").split(" "):
            if interrupt.is_set():
                return said, False
            if w:
                piece = w + " "
                said += piece
                send({"type": "stream_event", "session_id": sid, "event": {"type": "content_block_delta", "index": 0,
                                                                           "delta": {"type": "text_delta", "text": piece}}})
                time.sleep(0.01)
        log(event="emit", turn=turn, sentence=k, text=s)
    send({"type": "stream_event", "session_id": sid, "event": {"type": "content_block_stop", "index": 0}})
    return said, True


def reader():
    for line in sys.stdin:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        if m.get("type") == "control_request":
            sub = (m.get("request") or {}).get("subtype")
            global model
            if sub == "set_model":
                model = m["request"].get("model") or model
            log(event="control", subtype=sub, model=model)
            if sub == "interrupt":
                interrupt.set()
            send({"type": "control_response", "response": {"subtype": "success", "request_id": m.get("request_id")}})
        elif m.get("type") == "user":
            content = m["message"]["content"]
            text = content if isinstance(content, str) else " ".join(c.get("text", "") for c in content if c.get("type") == "text")
            images = [] if isinstance(content, str) else [
                hashlib.sha256(base64.b64decode(c["source"]["data"])).hexdigest() for c in content if c.get("type") == "image"]
            first = None if isinstance(content, str) or not content else content[0].get("type")
            log(event="user", text=text, model=model, images=images, first=first)
            with cond:
                inbox.append(text)
                cond.notify()
    with cond:
        inbox.append(None)
        cond.notify()


def turn_loop():
    turn = 0
    while True:
        with cond:
            cond.wait_for(lambda: inbox)
            text = inbox.pop(0)
        if text is None:
            return
        turn += 1
        interrupt.clear()
        t0 = time.time()
        said, whole = answer(text, turn)
        said = said.strip()
        if said:
            send({"type": "assistant", "session_id": sid, "message": {"model": model, "content": [{"type": "text", "text": said}]}})
        if whole:
            send({"type": "result", "subtype": "success", "is_error": False, "result": said, "num_turns": 1, "session_id": sid,
                  "duration_ms": int((time.time() - t0) * 1000), "total_cost_usd": 0})
        else:
            log(event="interrupted", turn=turn)
            send({"type": "result", "subtype": "error_during_execution", "is_error": True, "num_turns": 1, "session_id": sid,
                  "duration_ms": int((time.time() - t0) * 1000)})


if session:
    threading.Thread(target=reader, daemon=True).start()
    turn_loop()
else:  # one question on the command line, as `claude -p PROMPT` takes it
    prompt = args[args.index("-p") + 1] if "-p" in args and args.index("-p") + 1 < len(args) else ""
    log(event="user", text=prompt, model=model)
    said, _ = answer(prompt, 1)
    send({"type": "assistant", "session_id": sid, "message": {"model": model, "content": [{"type": "text", "text": said.strip()}]}})
    send({"type": "result", "subtype": "success", "is_error": False, "result": said.strip(), "num_turns": 1, "session_id": sid})
