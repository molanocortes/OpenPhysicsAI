#!/bin/sh
# run_bridge_print.sh - the printed truss bridge demo: geometry, layer-by-layer FFF print simulation and a verification
# record, through the stand-alone driver build/amprint.
#
#   demo/run_bridge_print.sh [output directory]        (default demo/workspace/print_bridge)
#
# Results are meant to be read in the application or through the print operations, not in a browser.
set -e
cd "$(dirname "$0")/.."
OUT=${1:-demo/workspace/print_bridge}

make build/amprint build/mechtest >/dev/null
if [ ! -x build/bridgegen ] || [ demo/make_bridge_geometry.c -nt build/bridgegen ]; then
    clang -O2 demo/make_bridge_geometry.c -o build/bridgegen -lm
fi
if [ ! -f demo/geometry/truss_bridge.stl ] || [ build/bridgegen -nt demo/geometry/truss_bridge.stl ]; then
    mkdir -p demo/geometry
    ./build/bridgegen demo/geometry/truss_bridge.stl 1.0
fi

# RECORD_ONLY=1 keeps an existing simulation and only refreshes the verification record
if [ -z "$RECORD_ONLY" ]; then
    ./build/amprint --stl demo/geometry/truss_bridge.stl --out "$OUT" --element 3 --layer 3 --rate 15 \
        --probe "deck plate, mid-panel@270,0,1.5" \
        --probe "bottom chord, mid-span@270,-40,9" \
        --probe "vertical post, mid-height@240,-40,55" \
        --probe "top chord, mid-span@270,-40,100" \
        --probe "top bracing crossing@270,0,100"
fi

# verification record: the print simulation checks of the mechanical verification suite, as run now
./build/mechtest > "$OUT/mechtest.log" 2>&1 || true
python3 - "$OUT/mechtest.log" "$OUT/verification.json" <<'EOF'
import json, re, subprocess, sys
log, out = sys.argv[1], sys.argv[2]
lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
titles = [
    ("bed-bonded block", "Stress increments add up exactly, and a uniformly cooled part leaves the bed stress-free"),
    ("two-layer strip", "A layer printed on a cooled layer warps like the Timoshenko bimetal"),
    ("relaxation above", "Material reheated above the relaxation temperature loses its stress, and equilibrium holds"),
    ("deposition plan", "Deposition order: spans cross their pillars, overhangs wait, floating fragments are never printed"),
    ("small PLA wall", "A whole print conserves heat at every step and comes off the bed self-equilibrated"),
]
checks, inside = [], False
for line in lines:
    if line.startswith("== printing"):
        inside = True
        continue
    if inside and (line.startswith("==") or not line.strip()):
        break
    if inside and line.startswith("  "):
        text = line.strip()
        if text.startswith("FAIL"):
            checks.append({"title": "FAILED", "detail": text})
            continue
        title = next((t for key, t in titles if text.startswith(key)), text.split(":")[0])
        checks.append({"title": title, "detail": text})
summary = next((l.strip() for l in lines if "MECH VERIFICATION TESTS" in l), "verification suite did not finish")
try:
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], capture_output=True, text=True).stdout.strip())
except OSError:
    commit, dirty = "", False
m = re.search(r"(\d+) passed, (\d+) failed", summary)
summary_text = f"{m.group(1)} of {int(m.group(1)) + int(m.group(2))} mechanical verification checks pass" if m else summary
json.dump({"checks": checks, "summary": summary_text,
           "source": "build/mechtest, test_fff_print in tools/mechtest.c" + (f", commit {commit}" + (" with local changes" if dirty else "") if commit else "")},
          open(out, "w"), indent=1)
print(f"{out}: {len(checks)} print checks; {summary}")
EOF
