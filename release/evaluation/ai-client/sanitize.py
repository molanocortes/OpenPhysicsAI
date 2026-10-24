#!/usr/bin/env python3
"""sanitize.py - shareable transcripts and measured metrics of real AI-client sessions

    python3 sanitize.py RAW_RUN_DIR OUT_DIR WORKSPACE REPO_ROOT

For every RAW_RUN_DIR/<case>-r<k>.jsonl (Claude Code stream-json) writes OUT_DIR/<case>-r<k>.jsonl with the home folder,
the temporary workspace and the repository path replaced by placeholders, session and request identifiers and e-mail
addresses removed, and OUT_DIR/metrics.json with, per session: the model reported by the client, the number and names
of MCP tool calls, tool errors, the wall time, the number of turns, and the final answer text. Grading stays manual
(RUBRIC.md); nothing here judges the answer.
"""
import json
import os
import re
import sys
from pathlib import Path

raw, out, ws, repo = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4]
home = os.path.expanduser("~")
out.mkdir(parents=True, exist_ok=True)
EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
USER = os.path.basename(home)
# the account name also appears inside encoded folder names (-Users-<name>-...) and per-user temporary folders
HOME_ENC = home.replace("/", "-")  # the home folder as the client encodes it in folder names
ENCODED = [(re.compile(re.escape(HOME_ENC) + r"(?=-|$)"), "-<home>"), (re.compile(re.escape(home) + r"\b"), "~"),
           (re.compile(r"claude-\d+"), "claude-<uid>")]
IDS = {"session_id", "uuid", "request_id", "parent_tool_use_id", "message_id"}


def clean(v):
    if isinstance(v, dict):
        return {k: ("<id>" if k in IDS and isinstance(x, str) else clean(x)) for k, x in v.items()}
    if isinstance(v, list):
        return [clean(x) for x in v]
    if isinstance(v, str):
        s = v.replace(ws, "<workspace>").replace(os.path.realpath(ws), "<workspace>").replace(repo, "<repo>").replace(home, "~")
        for pattern, repl in ENCODED:
            s = pattern.sub(repl, s)
        return EMAIL.sub("<email>", s)
    return v


timing = {}
if (raw / "timing.jsonl").exists():
    for line in (raw / "timing.jsonl").read_text().splitlines():
        t = json.loads(line)
        timing[f"{t['case']}-r{t['repeat']}"] = t
metrics = []
for f in sorted(raw.glob("*-r*.jsonl")):
    name = f.stem
    events = []
    for line in f.read_text(errors="replace").splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            events.append({"type": "unparsed_line"})
    with open(out / f.name, "w") as o:
        for e in events:
            o.write(json.dumps(clean(e)) + "\n")
    calls, builtin, errors, model, final, turns, is_error = [], [], 0, None, None, None, None
    for e in events:
        if e.get("type") == "system" and e.get("subtype") == "init":
            model = e.get("model")
        if e.get("type") == "assistant":
            for part in (e.get("message") or {}).get("content") or []:
                if part.get("type") == "tool_use":
                    (calls if (part.get("name") or "").startswith("mcp__navier__") else builtin).append(part.get("name"))
        if e.get("type") == "user":
            for part in (e.get("message") or {}).get("content") or []:
                if isinstance(part, dict) and part.get("type") == "tool_result" and part.get("is_error"):
                    errors += 1
        if e.get("type") == "result":
            final, turns, is_error = e.get("result"), e.get("num_turns"), e.get("is_error")
    t = timing.get(name, {})
    metrics.append({"session": name, "model": model, "tool_calls": len(calls), "tools": calls,
                    "builtin_tool_attempts": builtin, "builtin_note": "built-in tools are disabled in this setting; the client refuses each attempt",
                    "tool_errors": errors, "tool_errors_note": "includes refused built-in attempts; see builtin_tool_attempts", "turns": turns,
                    "client_error": is_error, "exit_code": t.get("exit"), "wall_seconds": (t["ended"] - t["started"]) if t else None,
                    "final_answer": clean(final) if isinstance(final, str) else final})
cfg = raw / "session_config.json"
(out / "metrics.json").write_text(json.dumps({"config": clean(json.loads(cfg.read_text())) if cfg.exists() else None, "sessions": metrics}, indent=1))
print(f"{len(metrics)} sessions sanitized into {out}")
