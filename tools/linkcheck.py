#!/usr/bin/env python3
"""linkcheck.py - keeps the map true.

    python3 tools/linkcheck.py [--quiet]

Three rules, each of which has cost this project real work when it was broken:

  1. No dead relative link. Every `[text](path)` and `<img src="path">` in a tracked markdown file must point at
     something that exists.
  2. Every markdown file is reachable from AGENTS.md by following links. An agent that runs out of context reads the
     entry point and nothing else; work that is not reachable from it gets forgotten and redone.
  3. Every directory under src/ and every file under tools/ is mentioned somewhere in docs/map/. A module nobody
     described is a module the next agent will rewrite.

Exit code 0 when all three hold, 1 otherwise, with the offenders printed. No dependencies beyond Python 3.10.
"""
from __future__ import annotations

import re
import subprocess
import sys
import urllib.parse
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY = "AGENTS.md"
MAP_DIR = "docs/map"

# [text](target) but not ![image](target) handled separately; also HTML <img src="...">
LINK_RE = re.compile(r"(?<!!)\[[^\]]*\]\(([^)]+)\)")
IMAGE_RE = re.compile(r"!\[[^\]]*\]\(([^)]+)\)")
HTML_SRC_RE = re.compile(r"""<img[^>]*\ssrc=["']([^"']+)["']""", re.IGNORECASE)
SKIP_SCHEMES = ("http://", "https://", "mailto:", "ftp://", "#")


def tracked_files() -> list[Path]:
    """Tracked files plus new files that are not ignored, so a check is possible before committing."""
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "--cached", "--others", "--exclude-standard"],
                         capture_output=True, text=True, check=True).stdout.split("\n")
    return [Path(p) for p in out if p.strip()]


def targets_in(text: str) -> list[str]:
    return LINK_RE.findall(text) + IMAGE_RE.findall(text) + HTML_SRC_RE.findall(text)


def resolve(src: Path, target: str) -> Path | None:
    """Repository-relative path a link points at, or None when it is external or a pure anchor."""
    t = target.strip().split(" ")[0]  # drop a "title" after the path
    if not t or t.startswith(SKIP_SCHEMES):
        return None
    t = t.split("#")[0]
    if not t:
        return None
    t = urllib.parse.unquote(t)
    base = (ROOT / src).parent if not t.startswith("/") else ROOT
    return (base / t.lstrip("/")).resolve()


def main() -> int:
    quiet = "--quiet" in sys.argv
    files = tracked_files()
    md = sorted(p for p in files if p.suffix.lower() == ".md")
    md_set = {str(p) for p in md}
    dead: list[tuple[str, str]] = []
    links: dict[str, set[str]] = {}

    for f in md:
        try:
            text = (ROOT / f).read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            dead.append((str(f), f"cannot read: {e}"))
            continue
        out: set[str] = set()
        for target in targets_in(text):
            p = resolve(f, target)
            if p is None:
                continue
            if not p.exists():
                dead.append((str(f), target))
                continue
            try:
                out.add(str(p.relative_to(ROOT)))
            except ValueError:
                dead.append((str(f), f"{target} (outside the repository)"))
        links[str(f)] = out

    # rule 2: reachability from the entry point
    seen: set[str] = set()
    q: deque[str] = deque([ENTRY])
    while q:
        cur = q.popleft()
        if cur in seen or cur not in md_set:
            continue
        seen.add(cur)
        for nxt in sorted(links.get(cur, ())):
            if nxt in md_set and nxt not in seen:
                q.append(nxt)
    unreachable = sorted(md_set - seen)

    # rule 3: src/* directories and tools/* files must be described in docs/map/
    map_text = "\n".join((ROOT / p).read_text(encoding="utf-8", errors="replace")
                         for p in md if str(p).startswith(MAP_DIR))
    src_dirs = sorted({str(p.parent) for p in files if str(p).startswith("src/") and p.parent != Path("src")})
    undescribed = [d for d in src_dirs if d not in map_text and Path(d).name not in map_text]
    tool_files = sorted(str(p) for p in files if str(p).startswith("tools/"))
    undescribed += [t for t in tool_files if t not in map_text and Path(t).name not in map_text]

    fail = bool(dead or unreachable or undescribed)
    if dead:
        print(f"dead links ({len(dead)}):")
        for f, t in dead:
            print(f"  {f} -> {t}")
    if unreachable:
        print(f"markdown not reachable from {ENTRY} ({len(unreachable)}):")
        for f in unreachable:
            print(f"  {f}")
        print(f"  fix: link each from the page it belongs to, or list it in {MAP_DIR}/INDEX.md")
    if undescribed:
        print(f"not mentioned anywhere in {MAP_DIR}/ ({len(undescribed)}):")
        for d in undescribed:
            print(f"  {d}")
        print(f"  fix: describe it in {MAP_DIR}/MODULES.md (a module) or {MAP_DIR}/GEMS.md (a tool)")
    if not fail and not quiet:
        print(f"linkcheck: {len(md)} markdown files, {sum(len(v) for v in links.values())} relative links, "
              f"all resolve; all reachable from {ENTRY}; {len(src_dirs)} src directories and {len(tool_files)} tools "
              f"described in {MAP_DIR}/")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
