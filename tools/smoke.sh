#!/usr/bin/env bash
# smoke.sh - the first check after a clone, on macOS or Linux (make smoke; about a minute or two on a laptop, most of
# it the build). It builds what it needs, then runs, each with PASS or FAIL:
#
#   build    make openphysics-ctl openphysics-mcp openphysics-server build/labrun build/labfilm
#   doctor   openphysics-ctl doctor, the self-diagnosis with its reference solve
#   lab      one small scenario of the physics lab through build/labrun
#   picture  one frame of that result as a PNG through build/labfilm
#   mcp      an MCP initialize and tools/list against openphysics-mcp, as an AI client starts it
#
# The criteria, written on 2026-10-22 before the first run: build, doctor and lab pass when their command exits 0, the
# lab's result must also hold at least one frame (labfilm --info); the picture passes when the file starts with the PNG
# signature and its header gives the size that was asked for; mcp passes when initialize answers with the server name
# "openphysics" and tools/list gives at least one tool. Exit 1 when any step fails, 0 when all pass. The picture is
# kept in build/smoke/; everything else the check writes goes to a temporary folder that it removes.
#
#   bash tools/smoke.sh [MAKE]          (make smoke passes its own make, so that its -j applies)
set -u
cd "$(dirname "$0")/.." || exit 1
MAKE_CMD=${1:-make}
J=""
case " ${MAKEFLAGS:-} " in *-j*|*jobserver*) ;; *) J=-j4 ;; esac  # a plain `make smoke` builds four files at a time
SCENARIO=examples/lab/lisle_tooth_root.json  # a gear tooth's root stress against a measured gauge: a few seconds
T=$(mktemp -d "${TMPDIR:-/tmp}/openphysics-smoke-XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
mkdir -p build/smoke
fails=0
start_all=$(date +%s)

# step NAME DETAIL-COMMAND -- COMMAND...: runs COMMAND with its output in a log, prints PASS or FAIL, the seconds and
# what DETAIL-COMMAND says about it (or the log's last lines when it failed)
step() {
    local name=$1 detail=$2 s r
    shift 3
    s=$(date +%s)
    if "$@" > "$T/$name.log" 2>&1; then r=PASS; else r=FAIL; fails=$((fails + 1)); fi
    printf '%s  %-8s %4d s  %s\n' "$r" "$name" $(( $(date +%s) - s )) "$($detail "$T/$name.log" "$r")"
    if [ "$r" = FAIL ]; then tail -n 8 "$T/$name.log" | sed 's/^/        /'; fi
}
last_line() { if [ "$2" = PASS ]; then tail -n 1 "$1" | cut -c1-150; else echo "see the lines below"; fi; }
quiet() { [ "$2" = PASS ] && echo "$3" || echo "see the lines below"; }

echo "make smoke: OpenPhysicsAI's first check on $(uname -s) $(uname -m)"
build_detail() { quiet "$1" "$2" "openphysics-ctl, openphysics-mcp, openphysics-server, build/labrun, build/labfilm"; }
step build build_detail -- $MAKE_CMD $J openphysics-ctl openphysics-mcp openphysics-server build/labrun build/labfilm

doctor_detail() { if [ "$2" = PASS ]; then grep -E 'ALL DIAGNOSTICS PASSED|reference_solve' "$1" | tail -n 1 | cut -c1-150; else echo "see the lines below"; fi; }
step doctor doctor_detail -- ./openphysics-ctl doctor

lab_run() {
    ./build/labrun "$SCENARIO" "$T/result.lab" > "$T/labrun.out" 2>&1 || { cat "$T/labrun.out"; return 1; }
    ./build/labfilm "$T/result.lab" --info > "$T/info.out" 2>&1 || { cat "$T/info.out"; return 1; }
    python3 - "$T/info.out" "$T/frames.txt" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r"^frames:\s*(\d+)(.*)$", text, re.M)
open(sys.argv[2], "w").write(m.group(1) if m else "0")
print("%s frames%s" % (m.group(1), m.group(2)) if m else "no frames in the result")
sys.exit(0 if m and int(m.group(1)) >= 1 else 1)
PY
}
lab_detail() { quiet "$1" "$2" "$SCENARIO: $(head -n 1 "$1" | cut -c1-110)"; }
step lab lab_detail -- lab_run

picture() {
    rm -rf "$T/frames" && mkdir -p "$T/frames"
    local n
    n=$(cat "$T/frames.txt" 2>/dev/null || echo 1)
    ./build/labfilm "$T/result.lab" --out "$T/frames" --size 800x500 --field mises --first $(( n > 0 ? n - 1 : 0 )) || return 1  # the von Mises stress, with its legend (a 2D field labfilm draws)
    local png
    png=$(ls "$T/frames"/*.png 2>/dev/null | head -n 1)
    [ -n "$png" ] || { echo "labfilm wrote no picture"; return 1; }
    cp "$png" build/smoke/picture.png
    python3 - build/smoke/picture.png 800 500 <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read(32)
ok = data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR"
w, h = struct.unpack(">II", data[16:24]) if ok else (0, 0)
print("build/smoke/picture.png, %d x %d pixels" % (w, h))
sys.exit(0 if ok and (w, h) == (int(sys.argv[2]), int(sys.argv[3])) else 1)
PY
}
step picture last_line -- picture

mcp_handshake() {
    python3 - ./openphysics-mcp "$T/ws" <<'PY'
import json, subprocess, sys
mcp, ws = sys.argv[1], sys.argv[2]
p = subprocess.Popen([mcp, "--embedded", "--workspace", ws], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, text=True)
def send(msg):
    p.stdin.write(json.dumps(msg) + "\n")
    p.stdin.flush()
def answer(id_):
    for line in p.stdout:
        msg = json.loads(line)
        if msg.get("id") == id_:
            return msg
    return {}
try:
    send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
          "params": {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "smoke", "version": "1"}}})
    init = answer(1).get("result", {})
    send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    tools = answer(2).get("result", {}).get("tools", [])
finally:
    p.stdin.close()
    p.wait(timeout=30)
info = init.get("serverInfo", {})
print("initialize: %s %s (protocol %s); tools/list: %d tools" % (info.get("name"), info.get("version"),
                                                                  init.get("protocolVersion"), len(tools)))
sys.exit(0 if info.get("name") == "openphysics" and len(tools) >= 1 else 1)
PY
}
step mcp last_line -- mcp_handshake

total=$(( $(date +%s) - start_all ))
if [ "$fails" -eq 0 ]; then
    echo "smoke: all 5 steps passed in $total s"
    exit 0
fi
echo "smoke: $fails of 5 steps FAILED ($total s)"
exit 1
