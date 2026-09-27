#!/bin/bash
# package.sh - build the headless release candidate in an isolated copy of the sources and assemble
# dist/navier-<version>-<os>-<arch>.tar.gz (binaries, docs, example with its expected output, tests, tools).
#
#   tools/package.sh [OUTPUT_DIR]
#
# The build runs in a temporary copy of src/ and the Makefile, so the working tree and its build/ folder are not
# touched, and the result shows that the release builds from the sources alone. On Apple silicon the binaries target
# -mcpu=apple-m1 (every Apple-silicon Mac) instead of the build machine's own CPU.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^#define NAVIER_AM_VERSION "\(.*\)"/\1/p' "$ROOT/src/ctl/engine.h")
OS=$(uname -s | tr '[:upper:]' '[:lower:]'); [ "$OS" = darwin ] && OS=macos
ARCH=$(uname -m)
NAME="navier-$VERSION-$OS-$ARCH"
OUT="${1:-$ROOT/dist}"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/navier-package-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "== isolated build of $NAME"
mkdir -p "$WORK/tree/tools"
cp -R "$ROOT/src" "$ROOT/Makefile" "$WORK/tree/"
cp "$ROOT/tools/embed.c" "$WORK/tree/tools/"
CPU=""
[ "$OS" = macos ] && [ "$ARCH" = arm64 ] && CPU="-mcpu=apple-m1"
if ! (cd "$WORK/tree" && make CPUFLAG="$CPU" -j4 am > "$WORK/build.log" 2>&1); then
    tail -40 "$WORK/build.log"
    exit 1
fi
if grep -E "(error|warning): " "$WORK/build.log"; then
    echo "package.sh: the release build produced warnings" >&2
    exit 1
fi

P="$WORK/$NAME"
mkdir -p "$P/bin" "$P/doc" "$P/share/navier/examples" "$P/share/navier/tests" "$P/share/navier/tools"
cp "$WORK/tree/navier-mcp" "$WORK/tree/navier-ctl" "$WORK/tree/navier-server" "$P/bin/"
cp "$ROOT/docs/release/README.md" "$P/README.md"
cp "$ROOT/docs/release/NOTICE.md" "$P/NOTICE.md"
for f in INSTALL WORKFLOW ASSUMPTIONS EVIDENCE LIMITATIONS AI_CLIENTS REFERENCE_CASES ADDING_A_TEST REPORTING_FAILURES; do
    cp "$ROOT/docs/release/$f.md" "$P/doc/"
done
EX="$P/share/navier/examples/bracket_comparison"
mkdir -p "$EX/geometry"
cp "$ROOT/examples/bracket_comparison/study.json" "$ROOT/examples/bracket_comparison/README.md" "$ROOT/examples/bracket_comparison/make_geometry.py" "$EX/"
cp "$ROOT"/examples/bracket_comparison/geometry/*.stl "$EX/geometry/"
cp "$ROOT/tools/studyflow.py" "$ROOT/tools/mcptest.py" "$P/share/navier/tests/"
cp "$ROOT/tools/ccx_crosscheck.py" "$P/share/navier/tools/"
# the independent-user test kit: participant files only (the facilitator reference stays in the source tree)
KIT="$P/share/navier/user-test-kit"
mkdir -p "$KIT/task_geometry"
for f in README.md QUICK_START.md TASK.md EXPECTED_ARTIFACTS.md FEEDBACK_FORM.md DIAGNOSTICS.md make_task_geometry.py; do
    cp "$ROOT/release/evaluation/user-test-kit/$f" "$KIT/"
done
cp "$ROOT"/release/evaluation/user-test-kit/task_geometry/*.stl "$KIT/task_geometry/"
cp "$ROOT/release/reference_cases/CRITERIA.md" "$P/doc/REFERENCE_CRITERIA.md"
cp "$ROOT/release/evaluation/CASES.md" "$P/doc/EVALUATION_CASES.md"

echo "== expected output of the example (packaged binaries)"
EXP="$WORK/example-run"
"$P/bin/navier-ctl" --embedded --workspace "$WORK/ws" --allow-read "$P" --allow-write "$WORK" \
    study run "$EX/study.json" --dir "$EXP" > "$WORK/example.log" 2>&1 || { cat "$WORK/example.log"; exit 1; }
mkdir -p "$EX/expected_output/previews"
cp "$EXP/evidence.json" "$EXP/report.md" "$EXP/study.json" "$EX/expected_output/"
cp "$EXP"/previews/*.png "$EX/expected_output/previews/"

# Finder metadata (.DS_Store) is not source: it appears when a folder is browsed and must not change the identity
SRC_HASH=$(cd "$ROOT" && find src Makefile tools/embed.c -type f ! -name '*.o' ! -name '.DS_Store' | LC_ALL=C sort | xargs shasum -a 256 | shasum -a 256 | cut -d' ' -f1)
CONTRACT=$(python3 -c "import json;print(json.load(open('$ROOT/src/ctl/ops_schema.json'))['contract_version'])" 2>/dev/null || echo unknown)
cat > "$P/VERSION" <<EOF
{
  "name": "NAVIER",
  "version": "$VERSION",
  "operation_contract": "$CONTRACT",
  "platform": "$OS-$ARCH",
  "cpu_target": "${CPU:-native default}",
  "compiler": "$(cc --version | head -1)",
  "built": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "sources_sha256": "$SRC_HASH",
  "sources_sha256_covers": "src/, Makefile and tools/embed.c of the working tree (without .DS_Store and object files), file hashes in sorted path order",
  "status": "release candidate for internal review: scripted clients and an independent CalculiX comparison on one machine; not evaluated by a real AI client, external users or measurements"
}
EOF
(cd "$P" && find . -type f ! -name SHA256SUMS | LC_ALL=C sort | xargs shasum -a 256 > SHA256SUMS)
tar -czf "$OUT/$NAME.tar.gz" -C "$WORK" "$NAME"
(cd "$OUT" && shasum -a 256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
echo "== $OUT/$NAME.tar.gz"
cat "$OUT/$NAME.tar.gz.sha256"
du -h "$OUT/$NAME.tar.gz" | cut -f1
