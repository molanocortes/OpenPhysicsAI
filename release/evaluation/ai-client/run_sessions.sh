#!/bin/bash
# run_sessions.sh - real AI-client sessions of the evaluation set: Claude Code in print mode whose only tools are the
# NAVIER MCP server's.
#
#   release/evaluation/ai-client/run_sessions.sh [CASE ...]
#       CASES    default: E1 E2 E4 E7 E9 E10 (full comparison, missing units, ambiguous region, unsupported physics,
#                inconclusive difference, unsupported safety request); E3 E5 E6 E8 may be added
#       REPEATS  sessions per case (default 3)
#       NAVIER_BIN  folder with navier-mcp (default: the repository root); use the evaluated package's bin/
#       MODEL    optional --model value; the model actually used is read back from each transcript
#
# Needs a logged-in `claude` (`claude auth login`); the script stops before any session when `claude auth status`
# reports no login. Sessions are independent: each has its own NAVIER workspace, write root and working folder, so no
# session can see another session's projects or studies. Each session gets the user's request only, runs in an empty folder with no built-in tools
# (--tools ""), no other MCP servers (--strict-mcp-config), no user or project settings (--setting-sources ""), and no
# saved session. Raw transcripts stay in raw/ (not for sharing); sanitize.py writes transcripts/<case>-r<k>.jsonl with
# home paths and identifiers replaced, and metrics.json (tool calls, errors, wall time, final answer). Grade with RUBRIC.md.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
GEOM="$ROOT/examples/bracket_comparison/geometry"
CLAUDE=${CLAUDE:-claude}
BIN=${NAVIER_BIN:-$ROOT}
REPEATS=${REPEATS:-3}
CASES=${*:-E1 E2 E4 E7 E9 E10}

status=$("$CLAUDE" auth status --json 2>/dev/null || true)
if ! printf '%s' "$status" | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin).get("loggedIn") else 1)' 2>/dev/null; then
    echo "run_sessions.sh: the Claude Code CLI is not logged in (claude auth status: $status)." >&2
    echo "Log in with 'claude auth login', then run this script again. No session was started." >&2
    exit 3
fi

RUN=$(date -u +%Y%m%dT%H%M%SZ)
WS=$(mktemp -d "${TMPDIR:-/tmp}/navier-ai-eval-XXXXXX")
mkdir -p "$HERE/raw/$RUN" "$HERE/transcripts/$RUN"
TOOLS_ALLOWED="mcp__navier"
python3 - "$HERE/raw/$RUN/session_config.json" <<PY
import json, subprocess, sys
cfg = {"run": "$RUN", "client": subprocess.run(["$CLAUDE", "--version"], capture_output=True, text=True).stdout.strip(),
       "navier_mcp": subprocess.run(["$BIN/navier-mcp", "--version"], capture_output=True, text=True).stdout.strip(),
       "mode": "print mode (claude -p): the model cannot receive answers; asking and stopping is correct for blocking questions",
       "built_in_tools": "none (--tools \"\")", "mcp_servers": "navier only (--strict-mcp-config)", "allowed_tools": "$TOOLS_ALLOWED",
       "settings_loaded": "none (--setting-sources \"\")", "session_persistence": "disabled",
       "navier_roots": {"read": ["$GEOM", "<session folder>"], "write": ["<session folder>"]}, "working_directory": "an empty temporary folder per session",
       "isolation": "per session: own NAVIER workspace, write root and working folder (<run workspace>/<case>-r<k>/)",
       "cases": "$CASES".split(), "repeats": int("$REPEATS"), "model_flag": "${MODEL:-default}"}
json.dump(cfg, open(sys.argv[1], "w"), indent=1)
PY
for c in $CASES; do
    prompt=$(sed "s#GEOM#$GEOM#g" "$HERE/prompts/$c.md")
    for k in $(seq 1 "$REPEATS"); do
        echo "== $c repeat $k"
        S="$WS/$c-r$k"
        mkdir -p "$S/cwd" "$S/projects"
        cat > "$S/mcp.json" <<JSON
{"mcpServers": {"navier": {"command": "$BIN/navier-mcp", "args": ["--embedded", "--workspace", "$S/projects", "--allow-read", "$GEOM", "--allow-write", "$S"]}}}
JSON
        t0=$(python3 -c 'import time; print(time.time())')
        (cd "$S/cwd" && "$CLAUDE" -p "$prompt" --mcp-config "$S/mcp.json" --strict-mcp-config --tools "" --allowedTools "$TOOLS_ALLOWED" \
            --setting-sources "" --no-session-persistence ${MODEL:+--model "$MODEL"} \
            --output-format stream-json --verbose > "$HERE/raw/$RUN/$c-r$k.jsonl" 2> "$HERE/raw/$RUN/$c-r$k.stderr")
        rc=$?
        t1=$(python3 -c 'import time; print(time.time())')
        echo "{\"case\": \"$c\", \"repeat\": $k, \"exit\": $rc, \"started\": $t0, \"ended\": $t1}" >> "$HERE/raw/$RUN/timing.jsonl"
        echo "   exit $rc; $(wc -l < "$HERE/raw/$RUN/$c-r$k.jsonl") events"
    done
done
python3 "$HERE/sanitize.py" "$HERE/raw/$RUN" "$HERE/transcripts/$RUN" "$WS" "$ROOT"
echo "raw transcripts: $HERE/raw/$RUN (not for sharing); sanitized: $HERE/transcripts/$RUN; workspace: $WS"
