#!/usr/bin/env bash
# check.sh - the quality gate. One command, seven stages, a table at the end, non-zero exit when something is wrong.
#
#     make check            # or: bash tools/check.sh
#     bash tools/check.sh d e f g      # only the cheap stages, by letter
#
# Stages
#   A  every core and tool translation unit compiled with -Wall -Wextra, warnings counted per file
#   B  the C suites rebuilt with -fsanitize=address,undefined into build/asan and run under it
#   C  make test (the project's own suite: 13 C suites, 8 Python workflows, navier-ctl doctor)
#   D  tools/linkcheck.py (dead links, reachability from AGENTS.md, undescribed modules) and the flag boards
#      (tools/flags.py board --check: every drawn board, card and front-page table agrees with the attempts)
#   E  every operation in src/ctl/ops_schema.json is exercised by a test
#   F  no tracked file carries an absolute home path
#   G  no commit message on this branch carries AI attribution
#   H  tools/matcheck.py: the materials library carries a source on every value a record claims
#
# Each stage reports its wall time and the peak resident memory of its largest process. A stage that cannot run on
# this machine says so and is marked UNAVAILABLE; nothing is skipped silently.
#
# Exit: 0 when every stage passed, 1 when any stage failed. WARN and UNAVAILABLE do not fail the gate but are printed.
# Logs: build/check/stage_<letter>.log (full output of every stage, kept after the run).

set -u
cd "$(dirname "$0")/.." || exit 1
SELF="tools/check.sh"
LOGDIR="build/check"
CC=${CC:-clang}
JOBS=${JOBS:-3}          # fanless 8 GB M2: three compiles at a time, one heavy stage at a time
SUITE_TIMEOUT=${SUITE_TIMEOUT:-600}
CPUFLAG=$($CC -mcpu=native -E -x c /dev/null >/dev/null 2>&1 && echo -mcpu=native)

# exit codes a stage may return
PASS=0; FAIL=1; WARN=2; UNAVAIL=3

# ---------------------------------------------------------------- helpers

hr() { printf '%s\n' "----------------------------------------------------------------------"; }

# run a command with a wall-clock limit, without coreutils' timeout (not on this machine)
run_limited() {
  local secs=$1; shift
  "$@" & local pid=$! i=0
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$i" -ge "$secs" ]; then kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; return 124; fi
    sleep 1; i=$((i + 1))
  done
  wait "$pid"
}

# ---------------------------------------------------------------- A: compiler warnings

# The flags the Makefile really compiles a given file with, minus the -Wno- suppressions (stage A adds them back
# through proj_flags_for). Every suppression here is one the Makefile itself sets; none was added to quieten a warning.
proj_flags_for() {
  # CFLAGS and CORE_CFLAGS both carry these two
  local f="$1" extra="-Wno-unused-parameter -Wno-missing-field-initializers"
  # Makefile, build/obj/headless/lbm.o: the IEEE build of the kernel is compiled with -Wno-pass-failed, because at -O2
  # without fast-math clang cannot honour the file's vectorisation pragmas and says so once per loop.
  [ "$f" = src/lbm.c ] && extra="$extra -Wno-pass-failed"
  printf '%s' "$extra"
}

stage_a() {
  local strict proj files f n_files=0 n_warn=0 n_err=0 offenders=0 gate=0
  strict="-std=c11 -O2 $CPUFLAG -ffp-contract=off -Wall -Wextra -pthread"
  proj="$strict $(proj_flags_for '')"
  files=$(ls src/*.c src/*/*.c tools/*.c 2>/dev/null)
  [ -n "$files" ] || { echo "no sources found"; return $FAIL; }

  echo "flags (strict, no -Wno- suppressions): $strict"
  echo "flags (the project's own):             $proj"
  echo "per-file, as in the Makefile:          src/lbm.c adds $(proj_flags_for src/lbm.c | sed 's/.*-Wno-missing-field-initializers //')"
  echo "compiling every translation unit to /dev/null, $JOBS at a time"
  rm -rf "$LOGDIR/warn"; mkdir -p "$LOGDIR/warn"
  printf '%s\n' "$files" | CCC="$CC" FLAGS="$strict" OUT="$LOGDIR/warn" xargs -P "$JOBS" -I{} sh -c \
    'o="$OUT/$(echo "{}" | tr / _).log"; $CCC $FLAGS -c -o /dev/null "{}" >"$o" 2>&1 || echo "COMPILE FAILED" >>"$o"'

  hr
  printf '%-44s %8s %8s\n' "file" "warnings" "errors"
  for f in $files; do
    local log w e
    log="$LOGDIR/warn/$(echo "$f" | tr / _).log"
    w=$(grep -c 'warning:' "$log" 2>/dev/null || true); e=$(grep -c 'error:' "$log" 2>/dev/null || true)
    n_files=$((n_files + 1)); n_warn=$((n_warn + w)); n_err=$((n_err + e))
    if [ "$w" != 0 ] || [ "$e" != 0 ]; then
      offenders=$((offenders + 1))
      printf '%-44s %8s %8s\n' "$f" "$w" "$e"
    fi
  done
  [ "$offenders" = 0 ] && printf '%-44s %8s %8s\n' "(none)" 0 0
  hr
  echo "$n_files files, $n_warn warnings, $n_err errors under the strict flags"

  # the gate is the project's own flags: a warning there is a defect, one the project chose to suppress is not.
  for f in $files; do
    local log w
    log="$LOGDIR/warn/$(echo "$f" | tr / _).log"
    w=$(grep -c 'warning:' "$log" 2>/dev/null || true)
    [ "$w" = 0 ] && continue
    local out
    out=$($CC $strict $(proj_flags_for "$f") -c -o /dev/null "$f" 2>&1)
    if printf '%s' "$out" | grep -q 'warning:\|error:'; then
      gate=$((gate + 1))
      echo "STILL WARNS under the project's own flags: $f"
      printf '%s\n' "$out" | grep 'warning:\|error:' | sed 's/^/    /'
    else
      echo "suppressed by the flags the Makefile uses for it ($(proj_flags_for "$f")): $f ($w strict warnings)"
      grep 'warning:' "$log" | sed 's/^/    /' | head -12
    fi
  done
  [ "$n_err" != 0 ] && { echo "a translation unit failed to compile"; return $FAIL; }
  [ "$gate" != 0 ] && { echo "$gate file(s) warn under the flags the project actually builds with: that is the gate"; return $FAIL; }
  echo "0 warnings under the project's own flags ($n_warn under the strict ones, every category suppressed on purpose)"
  return $PASS
}

# ---------------------------------------------------------------- B: AddressSanitizer + UndefinedBehaviorSanitizer

stage_b() {
  local aflags obj="build/asan/obj" suites core_src objs o s rc findings=0 failed=0 built=0
  aflags="-std=c11 -O1 -g -fno-omit-frame-pointer $CPUFLAG -ffp-contract=off -pthread \
-Wno-unused-parameter -Wno-missing-field-initializers -fsanitize=address,undefined"

  echo "flags: $aflags"
  echo "note: LeakSanitizer is not supported on macOS arm64 (checked: detect_leaks=1 aborts with"
  echo "      'detect_leaks is not supported on this platform'), so this stage finds memory errors and"
  echo "      undefined behaviour, NOT leaks. Leak checking needs a Linux run and is not done here."

  # the operation contract and the material library are generated C; reuse the Makefile's own rules
  make build/gen/ops_schema.c build/gen/materials_data.c >/dev/null 2>&1 || { echo "generated sources failed"; return $FAIL; }

  core_src=$(ls src/core/*.c src/ctl/*.c src/net/*.c src/fem/*.c src/render/*.c 2>/dev/null)
  core_src="$core_src src/geom/mesh.c src/geom/bvh.c src/geom/surface.c src/geom/patches.c src/geom/hexmesh.c"
  core_src="$core_src src/common.c src/threads.c build/gen/ops_schema.c build/gen/materials_data.c"

  [ "${CHECK_CLEAN:-0}" = 1 ] && rm -rf build/asan
  mkdir -p "$obj"
  echo "compiling $(printf '%s\n' $core_src | wc -l | tr -d ' ') core objects, $JOBS at a time"
  echo "(an object newer than its source is reused; CHECK_CLEAN=1 forces a full rebuild)"
  printf '%s\n' $core_src | CCC="$CC" FLAGS="$aflags" OBJ="$obj" xargs -P "$JOBS" -I{} sh -c \
    'o="$OBJ/$(echo "{}" | tr / _).o"; [ "$o" -nt "{}" ] && exit 0; $CCC $FLAGS -c "{}" -o "$o" 2>&1 | grep -v "^$" ; :'
  # the lattice Boltzmann kernel, IEEE build (the GUI fast-math build never enters a headless binary)
  [ "$obj/headless_lbm.o" -nt src/lbm.c ] || $CC $aflags -Wno-pass-failed -c src/lbm.c -o "$obj/headless_lbm.o" >/dev/null 2>&1

  local n_src n_obj
  n_src=$(printf '%s\n' $core_src | wc -l | tr -d ' '); n_obj=$(ls "$obj"/*.o 2>/dev/null | wc -l | tr -d ' ')
  if [ "$n_obj" -lt "$((n_src + 1))" ]; then
    echo "only $n_obj of $((n_src + 1)) objects compiled under the sanitizers; the sources that failed:"
    for o in $core_src src/lbm.c; do
      [ -f "$obj/$(echo "$o" | tr / _).o" ] || [ "$o" = src/lbm.c ] || echo "    $o"
    done
    return $FAIL
  fi
  objs=$(ls "$obj"/*.o | tr '\n' ' ')
  suites=$(grep '^TESTS' Makefile | sed 's/^TESTS *:= *//' | tr ' ' '\n' | sed 's|build/||' | grep -v '^$')
  [ -n "$suites" ] || { echo "could not read TESTS from the Makefile"; return $FAIL; }
  echo "suites (from the Makefile's TESTS): $(printf '%s ' $suites)"

  export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:halt_on_error=0:symbolize=1"
  export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"
  export MallocNanoZone=0   # macOS: keeps the allocator quiet under ASan

  hr
  printf '%-14s %-10s %10s  %s\n' "suite" "result" "seconds" "sanitizer findings"
  for s in $suites; do
    local link_objs="$objs" t0 t1 log="build/asan/$s.log" n
    case "$s" in
      coretest) link_objs=$(ls "$obj"/src_core_*.o | tr '\n' ' ') ;;
      surftest) link_objs="$obj/src_geom_mesh.c.o $obj/src_geom_bvh.c.o $obj/src_geom_surface.c.o $obj/src_common.c.o" ;;
    esac
    if ! $CC $aflags "tools/$s.c" $link_objs -o "build/asan/$s" -lm >"build/asan/$s.link.log" 2>&1; then
      echo "LINK FAILED: $s (see build/asan/$s.link.log)"; sed 's/^/    /' "build/asan/$s.link.log" | head -8
      failed=$((failed + 1)); continue
    fi
    built=$((built + 1))
    t0=$(date +%s)
    if [ "$s" = rendertest ]; then
      run_limited "$SUITE_TIMEOUT" "./build/asan/$s" build/asan/rendertest_out >"$log" 2>&1; rc=$?
    else
      run_limited "$SUITE_TIMEOUT" "./build/asan/$s" >"$log" 2>&1; rc=$?
    fi
    t1=$(date +%s)
    n=$(grep -cE 'runtime error:|ERROR: AddressSanitizer|ERROR: LeakSanitizer' "$log" 2>/dev/null || true)
    findings=$((findings + n))
    local verdict=ok
    if [ "$rc" = 124 ]; then verdict="TIMEOUT"; failed=$((failed + 1))
    elif [ "$rc" != 0 ]; then verdict="EXIT $rc"; failed=$((failed + 1))
    elif [ "$n" != 0 ]; then verdict="FINDINGS"; fi
    printf '%-14s %-10s %10s  %s\n' "$s" "$verdict" "$((t1 - t0))" "$n"
  done
  hr
  echo "built and ran $built suites under address+undefined sanitizers"

  if [ "$findings" != 0 ]; then
    echo "sanitizer findings, with their stacks:"
    for s in $suites; do
      [ -f "build/asan/$s.log" ] || continue
      grep -qE 'runtime error:|ERROR: AddressSanitizer|ERROR: LeakSanitizer' "build/asan/$s.log" || continue
      echo "=== $s ==="
      awk '/runtime error:|ERROR: AddressSanitizer|ERROR: LeakSanitizer/{p=14} p>0{print "    " $0; p--}' "build/asan/$s.log"
    done
  fi
  [ "$failed" != 0 ] && return $FAIL
  [ "$findings" != 0 ] && return $FAIL
  return $PASS
}

# ---------------------------------------------------------------- C: the project's own suite

stage_c() {
  echo "make -j$JOBS test"
  make -j"$JOBS" test 2>&1
  local rc=$?
  [ "$rc" = 0 ] || { echo "make test exited $rc"; return $FAIL; }
  return $PASS
}

# ---------------------------------------------------------------- D: the map is true

stage_d() {
  python3 tools/linkcheck.py && python3 tools/flags.py board --check
}

# ---------------------------------------------------------------- E: every operation is exercised

stage_e() {
  python3 - <<'PY'
import json, pathlib, re, sys

root = pathlib.Path('.')
ops = [o['name'] for o in json.loads((root / 'src/ctl/ops_schema.json').read_text())['operations']]

# How this is determined, and what it does not prove:
#   direct   - the operation name appears as a quoted string in a test source under tools/ (a C suite, a Python
#              workflow or a shell script). That is evidence the name is sent, not that the result is asserted.
#   indirect - the name appears in a src/ file at a call site that is not the dispatch table (study_op(...),
#              ops_invoke(...)), so it runs whenever a test drives that workflow.
#   none     - nothing names it anywhere but its own registration. Nothing exercises it.
test_files = sorted(p for p in (root / 'tools').iterdir() if p.suffix in ('.c', '.py', '.sh'))
test_text = {p: p.read_text(errors='replace') for p in test_files}
src_files = sorted(root.glob('src/**/*.c'))
src_lines = [(p, i + 1, ln) for p in src_files for i, ln in enumerate(p.read_text(errors='replace').splitlines())]

direct, indirect, missing = {}, {}, []
for op in ops:
    q = f'"{op}"'
    hits = [p.name for p, t in test_text.items() if q in t]
    if hits:
        direct[op] = hits
        continue
    calls = [(p, n, ln.strip()) for p, n, ln in src_lines
             if q in ln and re.search(r'(study_op|ops_invoke|op_call|invoke)\s*\(', ln)]
    if calls:
        indirect[op] = calls
    else:
        missing.append(op)

print(f'{len(ops)} operations in src/ctl/ops_schema.json (contract_version '
      f'{json.loads((root / "src/ctl/ops_schema.json").read_text())["contract_version"]})')
print(f'  named in a tools/ test:            {len(direct)}')
print(f'  only reached through another op:   {len(indirect)}')
print(f'  exercised by nothing:              {len(missing)}')
print('method: the operation name is searched for as a quoted string in tools/*.{c,py,sh}; if absent, in src/**/*.c')
print('        at a call site (study_op/ops_invoke), which is weaker evidence: the call runs, the result is asserted')
print('        by whatever test drives that workflow, not by a test of this operation.')
for op, calls in sorted(indirect.items()):
    p, n, ln = calls[0]
    print(f'  indirect: {op:24s} {p}:{n}')
# Operations that no test exercises today. They are reported on every run and must reach zero; an operation that
# falls off a test and is NOT on this list is a regression and fails the gate.
KNOWN_UNEXERCISED = set()
for op in missing:
    tag = 'known, must reach zero' if op in KNOWN_UNEXERCISED else 'NEW - a test was lost'
    print(f'  NOT EXERCISED: {op:24s} {tag}')
stale = sorted(KNOWN_UNEXERCISED - set(missing))
for op in stale:
    print(f'  now exercised, remove from the baseline in tools/check.sh: {op}')
new_gaps = [op for op in missing if op not in KNOWN_UNEXERCISED]
sys.exit(1 if new_gaps else (2 if missing else 0))
PY
}

# ---------------------------------------------------------------- F: no absolute home path leaves the workspace

stage_f() {
  local u=Users pat found
  pat="/$u/"
  # no exemptions: the repository is published as it stands, so one absolute home path in any tracked file fails
  found=$(git grep -I -l -F -e "$pat" || true)
  echo "tracked files containing an absolute home path:"
  if [ -z "$found" ]; then
    echo "  (none)"
    return $PASS
  fi
  printf '%s\n' "$found" | sed 's/^/  /'
  return $FAIL
}

# ---------------------------------------------------------------- G: the history carries no AI attribution

stage_g() {
  local n hits
  n=$(git rev-list --count HEAD)
  hits=$(git log --format='%H %s%n%b' | grep -niE 'co-authored-by|generated with \[|generated with claude|noreply@anthropic\.com|🤖' || true)
  echo "$n commits reachable from HEAD scanned (subject and body)"
  echo "patterns: co-authored-by, 'generated with [', 'generated with claude', noreply@anthropic.com, robot emoji"
  if [ -n "$hits" ]; then
    echo "attribution found:"; printf '%s\n' "$hits" | sed 's/^/  /'
    return $FAIL
  fi
  echo "no attribution found"
  return $PASS
}

# ---------------------------------------------------------------- H: the materials library is sourced

stage_h() {
  python3 tools/matcheck.py
}

# ---------------------------------------------------------------- stage runner

if [ "${1:-}" = "--run-stage" ]; then
  case "$2" in
    a) stage_a ;; b) stage_b ;; c) stage_c ;; d) stage_d ;; e) stage_e ;; f) stage_f ;; g) stage_g ;; h) stage_h ;;
    *) echo "unknown stage $2"; exit 1 ;;
  esac
  exit $?
fi

TITLE_a="compiler warnings (-Wall -Wextra)"
TITLE_b="address + undefined sanitizers"
TITLE_c="make test"
TITLE_d="links, reachability, flag boards"
TITLE_e="operation coverage by tests"
TITLE_f="absolute home paths in tracked files"
TITLE_g="AI attribution in commit messages"
TITLE_h="materials library sourced (matcheck)"

want=${*:-a b c d e f g h}
mkdir -p "$LOGDIR"
tree_before=$(git status --porcelain | sort)
started=$(date +%s)
rows=""; failures=0; warnings=0

for letter in $want; do
  eval "title=\$TITLE_$letter"
  echo ""
  hr
  echo "stage $(echo "$letter" | tr a-z A-Z): $title"
  hr
  log="$LOGDIR/stage_$letter.log"; tm="$LOGDIR/stage_$letter.time"
  /usr/bin/time -l sh -c 'bash "$0" --run-stage "$1" >"$2" 2>&1' "$SELF" "$letter" "$log" 2>"$tm"
  rc=$?
  cat "$log"
  wall=$(awk '/ real /{print $1; exit}' "$tm")
  rss=$(awk '/maximum resident set size/{print $1; exit}' "$tm")
  rssmb=$(awk -v b="${rss:-0}" 'BEGIN{printf "%.0f", b/1048576}')
  case $rc in
    0) status=PASS ;;
    2) status=WARN; warnings=$((warnings + 1)) ;;
    3) status=UNAVAILABLE ;;
    *) status=FAIL; failures=$((failures + 1)) ;;
  esac
  rows="$rows$(printf '%-2s %-40s %-12s %9s %8s MB' "$(echo "$letter" | tr a-z A-Z)" "$title" "$status" "${wall:-?}" "$rssmb")
"
done

tree_after=$(git status --porcelain | sort)
elapsed=$(( $(date +%s) - started ))

echo ""
hr
echo "SUMMARY   $(date '+%Y-%m-%d %H:%M')   $(git rev-parse --abbrev-ref HEAD) $(git rev-parse --short HEAD)"
hr
printf '%-2s %-40s %-12s %9s %11s\n' "" "stage" "status" "wall (s)" "peak memory"
printf '%s' "$rows"
hr
printf 'total wall time %s s. Peak memory is the largest single process in the stage, not the sum.\n' "$elapsed"

if [ "$tree_before" != "$tree_after" ]; then
  echo "WORKING TREE CHANGED during the run (a check must not modify the repository):"
  diff <(printf '%s\n' "$tree_before") <(printf '%s\n' "$tree_after") | sed 's/^/  /'
  failures=$((failures + 1))
fi

if [ "$failures" != 0 ]; then
  echo "RESULT: FAILED ($failures stage(s)). Logs in $LOGDIR/."
  exit 1
fi
if [ "$warnings" != 0 ]; then
  echo "RESULT: passed, with $warnings stage(s) reporting a known gap. Logs in $LOGDIR/."
else
  echo "RESULT: passed. Logs in $LOGDIR/."
fi
exit 0
