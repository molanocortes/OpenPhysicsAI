#!/bin/bash
# install_test.sh - install a release tarball into a clean location with a fresh home folder and check that it works
# without the source tree: checksums, doctor, the example study against its expected output, a replay, MCP cases.
#
#   tools/install_test.sh dist/navier-<version>-<os>-<arch>.tar.gz
#
# Every command runs under `env -i` with HOME, TMPDIR and PATH=/usr/bin:/bin only, from inside the extracted release.
set -uo pipefail
TARBALL=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
T=$(mktemp -d "${TMPDIR:-/tmp}/navier-install-XXXXXX")
PASS=0
FAIL=0
ok() { if [ "$1" = 0 ]; then PASS=$((PASS + 1)); echo "  pass: $2"; else FAIL=$((FAIL + 1)); echo "  FAIL: $2"; fi; }

mkdir -p "$T/opt" "$T/home" "$T/tmp"
tar -xzf "$TARBALL" -C "$T/opt"
REL=$(ls -d "$T"/opt/navier-*)
run() { (cd "$REL" && env -i HOME="$T/home" TMPDIR="$T/tmp/" PATH=/usr/bin:/bin "$@"); }
echo "== installed into $REL (HOME=$T/home)"

(cd "$REL" && shasum -a 256 -c SHA256SUMS > "$T/sums.log" 2>&1); ok $? "every file matches SHA256SUMS"
for f in README.md NOTICE.md VERSION doc/INSTALL.md doc/WORKFLOW.md doc/ASSUMPTIONS.md doc/EVIDENCE.md doc/LIMITATIONS.md doc/AI_CLIENTS.md \
         doc/REFERENCE_CASES.md doc/ADDING_A_TEST.md doc/REPORTING_FAILURES.md share/navier/examples/bracket_comparison/expected_output/evidence.json \
         share/navier/user-test-kit/TASK.md share/navier/user-test-kit/FEEDBACK_FORM.md share/navier/user-test-kit/task_geometry/plate_stepped.stl; do
    [ -s "$REL/$f" ]; ok $? "$f present"
done
[ ! -e "$REL/share/navier/user-test-kit/facilitator" ]; ok $? "the facilitator reference of the user-test kit is not in the package"
python3 - "$REL" <<'PY'
import json, sys
rel = sys.argv[1]
v = json.load(open(f"{rel}/VERSION"))
ev = json.load(open(f"{rel}/share/navier/examples/bracket_comparison/expected_output/evidence.json"))
md = open(f"{rel}/share/navier/examples/bracket_comparison/expected_output/report.md").read()
name_ok = rel.rstrip("/").split("/")[-1].startswith(f"navier-{v['version']}-")
ok = name_ok and ev["reproduction"]["software"]["version"] == v["version"] and ev["format_version"] == 2 and "\u00b1" not in md
print(f"  VERSION {v['version']}, expected output made by {ev['reproduction']['software']['version']}, record format {ev['format_version']}")
sys.exit(0 if ok else 1)
PY
ok $? "version, expected output and record format agree (no ± in the report)"
u=Users; dev=$(strings "$REL"/bin/* | grep -c "/$u/" || true)
[ "$dev" = 0 ]; ok $? "no developer home paths inside the binaries ($dev found)"

run bin/navier-ctl doctor --json > "$T/doctor.json" 2> "$T/doctor.err"
ok $? "navier-ctl doctor passes in a fresh home"
grep -q '"ok": true' "$T/doctor.json"; ok $? "doctor reports ok"

run bin/navier-ctl study check share/navier/examples/bracket_comparison/study.json > "$T/check.log" 2>&1
ok $? "example study check is ready"
run bin/navier-ctl study run share/navier/examples/bracket_comparison/study.json --dir "$T/home/NAVIER-Projects/bracket_ab" > "$T/run.log" 2> "$T/run.err"
ok $? "example study runs to completion"
python3 - "$T/home/NAVIER-Projects/bracket_ab/evidence.json" "$REL/share/navier/examples/bracket_comparison/expected_output/evidence.json" <<'PY'
import json, sys
a, b = (json.load(open(p)) for p in sys.argv[1:3])
keys = ("load_region_displacement_mm", "stiffness_n_per_mm", "mass_mesh_kg", "peak_displacement_mm")
n = same = 0
for da, db in zip(a["designs"], b["designs"]):
    for la, lb in zip(da["levels"], db["levels"]):
        for k in keys:
            n += 1
            same += la["quantities"][k] == lb["quantities"][k]
    for sa, sb in zip(da["sensitivity"], db["sensitivity"]):
        for k in keys[:2]:
            n += 1
            same += sa["quantities"][k] == sb["quantities"][k]
ok = n > 0 and same == n and a["comparison"]["outcome"] == b["comparison"]["outcome"] and a["study"]["study_hash"] == b["study"]["study_hash"]
print(f"  {same} of {n} quantities bitwise identical to the expected output; outcome {a['comparison']['outcome']}; same study hash: {a['study']['study_hash'] == b['study']['study_hash']}")
sys.exit(0 if ok else 1)
PY
ok $? "example reproduces the packaged expected output bitwise (other folder, other home)"
run bin/navier-ctl study replay "$T/home/NAVIER-Projects/bracket_ab" > "$T/replay.log" 2> "$T/replay.err"
ok $? "replay of the study reproduces it"
grep "replay:" "$T/replay.log" | sed 's/^/  /'
(cd "$REL" && env -i HOME="$T/home" TMPDIR="$T/tmp/" PATH=/usr/bin:/bin:/Library/Frameworks/Python.framework/Versions/Current/bin:/usr/local/bin \
    NAVIER_BIN="$REL/bin" python3 share/navier/tests/studyflow.py --only E2,E3,E4,E5,E8,E11,E12,E13 > "$T/studyflow.log" 2>&1)
ok $? "scripted MCP evaluation cases E2, E3, E4, E5, E8, E11, E12, E13 against the installed navier-mcp"
tail -1 "$T/studyflow.log" | sed 's/^/  /'

echo
echo "install test: $PASS passed, $FAIL failed (logs in $T)"
[ "$FAIL" = 0 ] && rm -rf "$T/home/NAVIER-Projects"
exit $([ "$FAIL" = 0 ] && echo 0 || echo 1)
