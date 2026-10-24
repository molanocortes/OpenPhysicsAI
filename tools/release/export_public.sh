#!/usr/bin/env bash
# export_public.sh - make the public tree and check it: the tracked files of one commit, minus the paths that
# .release-exclude keeps on the private branch, copied into a folder of their own, where every check then runs.
# Nothing in this repository is changed. docs/release/LAUNCH.md says when it is run.
#
#     bash tools/release/export_public.sh SCRATCH [TARGET]
#
# SCRATCH is a scratch folder of your own; the copy goes to TARGET, by default SCRATCH/public-tree. TARGET must not
# exist, be empty, or be a copy this script made before (that one is replaced). REF=<commit> exports another commit
# than HEAD; uncommitted changes are never in the copy.
#
# The copy is a git repository holding one commit of the public tree. Its objects are borrowed from this repository
# (.git/objects/info/alternates), so it costs the disk space of its files only; to make it stand alone before
# publishing it:  git -C TARGET repack -a -d && rm TARGET/.git/objects/info/alternates
#
# The checks, all on the copy; any hit fails the run (exit 1):
#   1. left out: no path that .release-exclude lists is in the copy, nor any picture that
#      tools/release/private/removed-media.txt lists (checked by path);
#   2. home paths: bash tools/check.sh f (no tracked file holds an absolute home path);
#   3. privacy: no scratchpad path, private network address, mDNS or default Windows host name, no pattern of
#      tools/release/private/never-public.txt, and no compute node's address, host name or login from
#      ~/.openphysicsai/nodes.json, in the text of a text file, the bytes of any other file, or a path;
#   4. secrets: no API key or token of the common forms, no private key block, no file named like a credential;
#   5. links: python3 tools/linkcheck.py on the copy (every relative link resolves inside it, every page is reachable
#      from AGENTS.md, every module and tool is described in the map).
# Exit 0 when every check passes, 1 when any fails, 2 when the copy could not be made.

set -u
usage() { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; }
[ $# -ge 1 ] && [ "$1" != "-h" ] && [ "$1" != "--help" ] || { usage; exit 2; }

here=$(cd "$(dirname "$0")" && pwd)
src=$(git -C "$here" rev-parse --show-toplevel) || exit 2
scratch=$1
target=${2:-$scratch/public-tree}
commit=$(git -C "$src" rev-parse --verify "${REF:-HEAD}^{commit}") || exit 2
short=$(git -C "$src" rev-parse --short "$commit")

# a shared laptop must not feel it: lowest priority, and on macOS the efficiency cores
renice -n 19 -p $$ >/dev/null 2>&1 || true
if [ "$(uname -s)" = Darwin ]; then taskpolicy -b -p $$ >/dev/null 2>&1 || true; fi

mkdir -p "$scratch" || exit 2
work=$(mktemp -d "$scratch/.export-public-work.XXXXXX") || exit 2
trap 'rm -rf "$work"' EXIT

if [ "$commit" = "$(git -C "$src" rev-parse HEAD)" ] && [ -n "$(git -C "$src" status --porcelain --untracked-files=no)" ]; then
  echo "note: the working tree has uncommitted changes; the copy holds $short as committed"
fi

# what .release-exclude leaves out, as git glob pathspecs, read from the exported commit itself
specs=()
if git -C "$src" cat-file -e "$commit:.release-exclude" 2>/dev/null; then
  while IFS= read -r line || [ -n "$line" ]; do
    line=${line%$'\r'}
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    case "$line" in ''|'#'*) continue ;; esac
    case "$line" in
      */) specs+=(":(glob)${line}**") ;;
      *) specs+=(":(glob)$line") ;;
    esac
  done < <(git -C "$src" show "$commit:.release-exclude")
else
  echo "note: $short has no .release-exclude; nothing is left out"
fi
for f in removed-media.txt never-public.txt; do
  git -C "$src" show "$commit:tools/release/private/$f" > "$work/$f" 2>/dev/null || : > "$work/$f"
done

# the copy: a repository of its own whose index is the commit minus the excluded paths
if [ -e "$target" ]; then
  if [ -f "$target/.git/export-public" ]; then
    rm -rf "$target" || exit 2
  elif [ -n "$(ls -A "$target" 2>/dev/null)" ]; then
    echo "export_public: $target exists and was not made by this script; refusing to replace it"
    exit 2
  fi
fi
objects=$(cd "$src" && cd "$(git rev-parse --git-path objects)" && pwd) || exit 2
mkdir -p "$target" && git -C "$target" init -q || exit 2
printf '%s\n' "$objects" > "$target/.git/objects/info/alternates"
printf 'made by tools/release/export_public.sh from %s; replaced on every run\n' "$commit" > "$target/.git/export-public"
G() { git -C "$target" "$@"; }
G read-tree "$commit" || exit 2
n_all=$(G ls-files | wc -l | tr -d ' ')
: > "$work/excluded.z"
if [ ${#specs[@]} -gt 0 ]; then
  G ls-files -z -- "${specs[@]}" > "$work/excluded.z"
  G update-index -z --force-remove --stdin < "$work/excluded.z" || exit 2
fi
n_excl=$(tr -cd '\000' < "$work/excluded.z" | wc -c | tr -d ' ')

# room for it: the files' size against the free space, with 256 MB to spare
bytes=$(G ls-files -s -z | tr '\0' '\n' | awk '{print $2}' | G cat-file --batch-check='%(objectsize)' | awk '{s += $1} END {printf "%.0f", s}')
free_kb=$(df -Pk "$target" | awk 'NR == 2 {print $4}')
if [ "$(( bytes / 1024 + 262144 ))" -gt "$free_kb" ]; then
  echo "export_public: the copy needs $(( bytes / 1048576 )) MB and $(( free_kb / 1024 )) MB are free; refusing (256 MB must stay free)"
  rm -rf "$target"
  exit 2
fi
G checkout-index -a -f || exit 2
G -c user.name="public export" -c user.email="export@localhost" -c commit.gpgsign=false \
  commit -q --no-verify -m "The public tree of $short" || exit 2
n_files=$(G ls-files | wc -l | tr -d ' ')
disk_kb=$(du -sk "$target" | awk '{print $1}')
git_kb=$(du -sk "$target/.git" | awk '{print $1}')

fails=0
row() { printf '  %-12s %-6s %s\n' "$1" "$2" "$3"; }
rows=""

# 1. left out
leaks=0
while IFS= read -r -d '' f; do
  if [ -e "$target/$f" ] || [ -n "$(G ls-files -- ":(literal)$f")" ]; then echo "left out, but present: $f"; leaks=$((leaks + 1)); fi
done < "$work/excluded.z"
n_media=0
while IFS= read -r f || [ -n "$f" ]; do
  case "$f" in ''|'#'*) continue ;; esac
  n_media=$((n_media + 1))
  if [ -e "$target/$f" ]; then echo "removed picture, but present: $f"; leaks=$((leaks + 1)); fi
done < "$work/removed-media.txt"
[ "$leaks" -eq 0 ] && st=PASS || { st=FAIL; fails=$((fails + 1)); }
rows="$rows$(row "left out" "$st" "$n_excl paths of .release-exclude and $n_media removed pictures checked by path: $leaks present")
"

# 2. absolute home paths, by the quality gate's own stage
(cd "$target" && bash tools/check.sh f) > "$work/check_f.log" 2>&1
rc=$?
[ "$rc" -eq 0 ] && st=PASS || { st=FAIL; fails=$((fails + 1)); sed -n '/tracked files containing/,/^$/p' "$work/check_f.log"; }
rows="$rows$(row "home paths" "$st" "bash tools/check.sh f in the copy: exit $rc")
"

# 3. privacy and 4. secrets, in one pass over every file of the copy
python3 -I - "$target" "$work/never-public.txt" "$HOME/.openphysicsai/nodes.json" "$work/scan.txt" <<'PY'
import json, os, re, subprocess, sys
target, private_list, nodes_file, summary = sys.argv[1:5]
# Generic patterns, written so that this file does not match itself.
privacy = [
    ("a Claude Code scratchpad path", r"/private/tmp/claude[-]"),
    ("a Claude Code temporary folder", r"\bclaude-[0-9]{3}\b"),
    ("a home folder encoded in a folder name", r"-Users-[A-Za-z]"),
    ("a private network address", r"\b192[.]168[.][0-9]{1,3}[.][0-9]{1,3}\b"),
    ("an mDNS host name", r"(?<![A-Za-z0-9_-])[A-Za-z0-9]+(?:-[A-Za-z0-9]+)+[.]local(?![A-Za-z0-9_-])"),
    ("an mDNS host name in an address", r"(?:@|//)[A-Za-z0-9][A-Za-z0-9-]*[.]local(?![A-Za-z0-9_-])"),
    ("a default Windows host name", r"\bDESKTOP-[A-Z0-9]{5,}\b"),
    ("a default Windows laptop name", r"\bLAPTOP-[A-Z0-9]{5,}\b"),
]
home = os.path.expanduser("~")
if os.path.dirname(home) == os.sep + "Users":  # a personal home folder on macOS
    privacy.append(("this Mac's home folder", re.escape(home) + r"\b"))
if os.path.exists(private_list):
    for line in open(private_list, encoding="utf-8"):
        s = line.strip()
        if s and not s.startswith("#"):
            privacy.append(("tools/release/private/never-public.txt: " + s, s))
generic = {"node", "root", "admin", "user", "users", "ubuntu", "home", "linux", "windows", "desktop", "surface"}
if os.path.exists(nodes_file):
    try:
        nodes = json.load(open(nodes_file))
    except ValueError:
        nodes = {}
    for name, n in nodes.items():
        for key in ("host", "mdns", "user"):
            v = str(n.get(key, "")).strip() if isinstance(n, dict) else ""
            if len(v) >= 4 and v.lower() not in generic:
                privacy.append(("the %s of node %s (nodes.json)" % (key, name), r"(?<![A-Za-z0-9_.-])" + re.escape(v) + r"(?![A-Za-z0-9_-])"))
secrets = [
    ("an OpenAI-style key", r"\bsk-[A-Za-z0-9_-]{20,}"),
    ("a GitHub token", r"\bgh[pousr]_[A-Za-z0-9]{20,}"),
    ("a GitHub fine-grained token", r"\bgithub_pat_[A-Za-z0-9_]{20,}"),
    ("an AWS access key", r"\bAKIA[0-9A-Z]{16}\b"),
    ("a private key or certificate block", r"-{5}BEGIN"),
    ("a Google API key", r"\bAIza[0-9A-Za-z_-]{30,}"),
    ("a Slack token", r"\bxox[baprs]-[A-Za-z0-9-]{10,}"),
]
names = r"(^|/)\.env($|\.)|\.(pem|key|p12|pfx|keystore|jks)$|(^|/)id_(rsa|dsa|ecdsa|ed25519)(\.pub)?$|(^|/)\.netrc$|(^|/)credentials(\.json)?$"
P = [(label, re.compile(rx, re.I)) for label, rx in privacy]
S = [(label, re.compile(rx)) for label, rx in secrets]
STRINGS = re.compile(rb"[\x20-\x7e]{6,}")  # the printable runs of a binary file, as strings(1) finds them
files = [f for f in subprocess.run(["git", "-C", target, "ls-files", "-z"], capture_output=True, check=True).stdout.decode().split("\0") if f]
hits = {"privacy": [], "secrets": []}
for f in files:
    for label, rx in P:
        if rx.search(f):
            hits["privacy"].append("%s (its path): %s" % (f, label))
    if re.search(names, f, re.I):
        hits["secrets"].append("%s: named like a credential" % f)
    path = os.path.join(target, f)
    if os.path.islink(path) or not os.path.isfile(path):
        continue
    data = open(path, "rb").read()
    if b"\0" in data[:8000]:
        runs = "\n".join(m.group(0).decode("ascii") for m in STRINGS.finditer(data))
        for kind, pats in (("privacy", P), ("secrets", S)):
            for label, rx in pats:
                m = rx.search(runs)
                if m:
                    hits[kind].append("%s (binary): %s: %r" % (f, label, m.group(0)[:60]))
        continue
    text = data.decode("utf-8", errors="replace")
    for kind, pats in (("privacy", P), ("secrets", S)):
        for label, rx in pats:
            for m in rx.finditer(text):
                line = text.count("\n", 0, m.start()) + 1
                hits[kind].append("%s:%d: %s: %r" % (f, line, label, m.group(0)[:60]))
for kind in ("privacy", "secrets"):
    for h in hits[kind][:200]:
        print("%s: %s" % (kind, h))
    if len(hits[kind]) > 200:
        print("%s: ... and %d more" % (kind, len(hits[kind]) - 200))
with open(summary, "w") as out:
    out.write("%d %d %d %d %d\n" % (len(hits["privacy"]), len(P), len(hits["secrets"]), len(S), len(files)))
PY
read -r n_priv n_ppat n_sec n_spat n_scanned < "$work/scan.txt" 2>/dev/null || { n_priv=1; n_ppat=0; n_sec=1; n_spat=0; n_scanned=0; }
[ "$n_priv" -eq 0 ] && st=PASS || { st=FAIL; fails=$((fails + 1)); }
rows="$rows$(row "privacy" "$st" "$n_ppat patterns over $n_scanned files and their paths: $n_priv hits")
"
[ "$n_sec" -eq 0 ] && st=PASS || { st=FAIL; fails=$((fails + 1)); }
rows="$rows$(row "secrets" "$st" "$n_spat key and token forms, and credential file names: $n_sec hits")
"

# 5. links, by the repository's own checker, run inside the copy
(cd "$target" && python3 tools/linkcheck.py) > "$work/links.log" 2>&1
rc=$?
[ "$rc" -eq 0 ] && st=PASS || { st=FAIL; fails=$((fails + 1)); cat "$work/links.log"; }
rows="$rows$(row "links" "$st" "$(tail -1 "$work/links.log" | cut -c1-150)")
"

echo ""
echo "public tree of $short in $target"
echo "  $n_files files ($n_excl of the commit's $n_all left out), $(( bytes / 1048576 )) MB of files, $(( (disk_kb - git_kb) / 1024 )) MB on disk"
printf '%s' "$rows"
if [ "$fails" -ne 0 ]; then
  echo "RESULT: FAILED ($fails check(s)). The copy stays in $target for inspection."
  exit 1
fi
echo "RESULT: passed. Remove the copy with: rm -rf '$target'"
exit 0
