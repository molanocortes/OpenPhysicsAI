#!/usr/bin/env python3
"""site_build.py - assemble the public website from site/ into build/site/ and check it against the rules it keeps.

    python3 tools/site_build.py [--out DIR] [--brand NAME] [--ref GIT_REF]

site/ holds the page templates, the stylesheet, the script, the icon and the poster frames. The films are the public
ones already in docs/media and flags/, copied in at build time (site/site.json maps them) so no film is stored twice.
The templates name the project as %BRAND% and the repository as %REPO%; site/site.json sets both, --brand overrides
the name.

Nothing is written unless every check passes:
  - every local file a page references exists in the output, and every in-page anchor has its target;
  - every link into the repository on GitHub names a file that exists on the branch the site describes (site.json
    "publish_ref", by default origin/main; the working tree when that ref is not available);
  - nothing is loaded from another site (no fonts, scripts, styles, images or frames from elsewhere), there is no
    form, and no e-mail address or phone link;
  - none of the phrases the project has decided the public site must not carry (the list and the reasons are in
    site/README.md), in the pages, scripts, scenarios or the text inside the lab's WebAssembly module. Further
    patterns that must not be named even in this repository are read, when present, from
    ~/.openphysicsai/site-never-public.txt, one regular expression per line;
  - the lab page's module and scenarios (site.json "web", made by tools/web_build.py) were built and checked from the
    very commit the site describes, with this checkout's src/web/ and site/lab/catalogue.json.
Exit 0 when the site is written and clean; 1 with the problems listed. Python 3.9 or later, no dependencies.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import shutil
import subprocess
import sys
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / "site"
MARKER = ".site-build"  # written into the output, so a rebuild only ever deletes a folder this script made
PRIVATE_PATTERNS = Path.home() / ".openphysicsai" / "site-never-public.txt"

# The public site carries the project, its downloads, its documents, its flags and a neutral way to get in touch.
# Nothing is offered for sale, nobody is asked to sign up, and the internal program name is not used as a name.
NEVER_PUBLIC = [
    (r"\bnavier\b(?![-\s]stokes)", "the internal program name is not a public name (it is another company's name)"),
    (r"give (it|us) a star|star (this|the|our) repo|star us\b", "no request for stars"),
    (r"\bpric(e|es|ing)\b|\bbuy\b|\bpurchase\b|\bsubscri(be|ption)\b|\bnewsletter\b|\bsponsor|\bdonat(e|ion)",
     "nothing is for sale and nobody is asked to sign up"),
    (r"€|\bEUR\b|\bUSD\b|\$\s?\d", "no prices"),
    (r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}", "no e-mail address on the public site"),
    (r"\btel:", "no phone number"),
]


class Refs(HTMLParser):
    """Every reference a page makes, with the tag and attribute it sits in, and every id it defines."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.refs: list[tuple[str, str, str]] = []
        self.ids: set[str] = set()
        self.tags: set[str] = set()

    def handle_starttag(self, tag, attrs):
        self.tags.add(tag)
        for name, value in attrs:
            if name == "id" and value:
                self.ids.add(value)
            if name in ("src", "href", "data-film", "data-poster", "poster", "action", "data-worker", "data-wasi",
                        "data-wasm", "data-catalogue") and value is not None:
                self.refs.append((tag, name, value))
            if name == "srcset" and value:
                for part in value.split(","):
                    self.refs.append((tag, name, part.strip().split(" ")[0]))


def published_files(ref: str) -> tuple[set[str] | None, str]:
    """The files on the branch the site describes, or None when git or the ref is not available."""
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "ls-tree", "-r", "--name-only", ref],
                             capture_output=True, text=True, check=True).stdout
        return set(out.split("\n")) - {""}, ref
    except (OSError, subprocess.CalledProcessError):
        return None, "the working tree"


def module_strings(wasm: bytes) -> list[str]:
    """The C strings of a WebAssembly module: the NUL-terminated printable runs of its data section."""
    def leb(i: int) -> tuple[int, int]:
        value = shift = 0
        while True:
            b = wasm[i]
            value |= (b & 0x7F) << shift
            shift += 7
            i += 1
            if b < 0x80:
                return value, i
    out, i = [], 8
    while i < len(wasm):
        section, i = wasm[i], i + 1
        size, i = leb(i)
        end = i + size
        if section == 11:  # data: count, then segments of (flags, [memory], offset expression, size, bytes)
            count, j = leb(i)
            for _ in range(count):
                flags, j = leb(j)
                if flags == 2:
                    _, j = leb(j)
                if flags in (0, 2):
                    j += 1  # i32.const or global.get, its operand, then end
                    _, j = leb(j)
                    j += 1
                n, j = leb(j)
                for piece in wasm[j:j + n].split(b"\0"):
                    if len(piece) >= 4 and all(32 <= c < 127 or c in (9, 10) for c in piece):
                        out.append(piece.decode())
                j += n
        i = end
    return out


def web_files(web: dict | None, ref: str, problems: list[str]) -> dict[str, Path]:
    """The lab page's built files (tools/web_build.py), when they were made from ref and checked; else a problem."""
    if not web:
        return {}
    base = ROOT / web["from"]
    info_path = base / web["build"]
    if not info_path.is_file():
        problems.append(f"{web['from']} has no {web['build']}: run python3 tools/web_build.py --check")
        return {}
    info = json.loads(info_path.read_text())
    commit = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--verify", "-q", ref + "^{commit}"],
                            capture_output=True, text=True).stdout.strip()
    glue = hashlib.sha256()  # the glue's code (src/web/*.c, *.h), as tools/web_build.py hashes it
    for f in sorted((ROOT / "src" / "web").glob("*.[ch]")):
        glue.update(f.read_bytes())
    catalogue = hashlib.sha256((ROOT / web["catalogue"]).read_bytes()).hexdigest()
    module = base / web["module"]
    again = "; run python3 tools/web_build.py --check"
    if commit and info.get("commit") != commit:
        problems.append(f"the lab's module was built from {info.get('built_from')}, not from {ref} at {commit[:12]}{again}")
    if info.get("glue_sha256") != glue.hexdigest():
        problems.append(f"src/web/ has changed since the lab's module was built{again}")
    if info.get("catalogue_sha256") != catalogue:
        problems.append(f"{web['catalogue']} has changed since the lab's scenarios were written{again}")
    if not info.get("checked"):
        problems.append(f"the lab's scenarios were not all checked natively and in WebAssembly{again}")
    if not module.is_file() or hashlib.sha256(module.read_bytes()).hexdigest() != info.get("module_sha256"):
        problems.append(f"{module.relative_to(ROOT)} is not the module {web['build']} describes{again}")
    return {str(f.relative_to(base)): f for f in sorted(base.rglob("*")) if f.is_file() and not f.name.startswith(".")}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=str(ROOT / "build" / "site"))
    ap.add_argument("--brand")
    ap.add_argument("--ref")
    args = ap.parse_args()

    cfg = json.loads((SITE / "site.json").read_text())
    brand = args.brand or cfg["brand"]
    repo = cfg["repo"].rstrip("/")
    site_url = cfg.get("url", "").rstrip("/")
    built_on = datetime.date.today().isoformat()
    out = Path(args.out).resolve()
    problems: list[str] = []

    # render the pages in memory first; nothing touches the output folder until every check has passed
    pages = {}
    for name in cfg["pages"]:
        text = (SITE / name).read_text(encoding="utf-8")
        text = text.replace("%BRAND%", brand).replace("%REPO%", repo).replace("%URL%", site_url).replace("%DATE%", built_on)
        left = sorted(set(re.findall(r"%[A-Z_]+%", text)))
        if left:
            problems.append(f"{name}: template names left unfilled: {', '.join(left)}")
        pages[name] = text

    # the files the output will hold: copied site files, posters, and the films mapped from the repository
    files: dict[str, Path] = {}
    for item in cfg["copy"]:
        src = SITE / item
        if src.is_dir():
            for f in sorted(src.rglob("*")):
                if f.is_file() and not f.name.startswith("."):
                    files[str(f.relative_to(SITE))] = f
        elif src.is_file():
            files[item] = src
        else:
            problems.append(f"site.json copies {item}, which does not exist in site/")
    for dest, src in cfg["films"].items():
        p = ROOT / src
        if p.is_file():
            files[dest] = p
        else:
            problems.append(f"site.json maps {dest} to {src}, which does not exist")
    web = web_files(cfg.get("web"), args.ref or cfg.get("publish_ref", "origin/main"), problems)
    files.update(web)

    # a stylesheet, script or icon is named with a hash of its content, so a browser never keeps an old one
    for item in list(cfg["copy"]) + [n for n in web if n.endswith((".wasm", "scenarios.json"))]:
        if item in files:
            digest = hashlib.sha256(files[item].read_bytes()).hexdigest()[:10]
            for name in pages:
                pages[name] = re.sub(r'(="/?)' + re.escape(item) + r'(")', r"\g<1>" + item + "?v=" + digest + r"\g<2>",
                                     pages[name])

    on_branch, branch_name = published_files(args.ref or cfg.get("publish_ref", "origin/main"))
    texts = dict(pages)
    for name in files:
        if name.endswith((".css", ".js", ".svg", ".json", ".txt")) and not name.startswith("media/"):
            texts[name] = files[name].read_text(encoding="utf-8")
        elif name.endswith(".wasm"):  # the text inside the module: its messages, names and scenario keys
            texts[name] = "\n".join(module_strings(files[name].read_bytes()))

    for name, html in pages.items():
        parser = Refs()
        parser.feed(html)
        for bad in ("form", "iframe", "embed", "object"):
            if bad in parser.tags:
                problems.append(f"{name}: has a <{bad}>; the public site has no forms and frames nothing from elsewhere")
        for tag, attr, value in parser.refs:
            if value.startswith("#"):
                if value != "#" and value[1:] not in parser.ids:
                    problems.append(f"{name}: in-page link {value} has no target")
                continue
            if re.match(r"^[a-z][a-z0-9+.-]*:", value, re.I):
                if tag == "link" and site_url and value.startswith(site_url + "/"):
                    continue  # the canonical address of this site
                if tag != "a" or attr != "href":
                    problems.append(f"{name}: <{tag} {attr}> loads {value} from another site")
                elif not value.startswith("https://github.com/"):
                    problems.append(f"{name}: links outside GitHub: {value}")
                elif value.startswith(repo + "/blob/main/") or value.startswith(repo + "/tree/main/"):
                    path = value.split("/main/", 1)[1].split("#")[0]
                    exists = path in on_branch if on_branch is not None else (ROOT / path).exists()
                    if not exists and on_branch is not None:
                        exists = any(f.startswith(path.rstrip("/") + "/") for f in on_branch)
                    if not exists:
                        problems.append(f"{name}: links to {path}, which is not on {branch_name}")
                continue
            local = value.split("#")[0].split("?")[0].lstrip("/")
            if local.endswith("/"):
                local += "index.html"  # a folder is served by its index
            if local and local not in files and local not in pages:
                problems.append(f"{name}: <{tag} {attr}> points to {local}, which the site does not contain")

    for name, text in texts.items():
        for m in re.finditer(r"url\(\s*['\"]?(https?:)?//|@import", text, re.I):
            problems.append(f"{name}: loads a resource from another site ({m.group(0)})")

    patterns = list(NEVER_PUBLIC)
    if PRIVATE_PATTERNS.is_file():
        for i, line in enumerate(PRIVATE_PATTERNS.read_text(encoding="utf-8").splitlines(), 1):
            line = line.strip()
            if line and not line.startswith("#"):
                patterns.append((line, f"private pattern on line {i} of {PRIVATE_PATTERNS}"))
    for name, text in texts.items():
        for pattern, why in patterns:
            for m in re.finditer(pattern, text, re.I):
                line = text.count("\n", 0, m.start()) + 1
                problems.append(f"{name}:{line}: '{m.group(0)}': {why}")

    if problems:
        print(f"site_build: {len(problems)} problem(s), nothing written")
        for p in problems:
            print("  " + p)
        return 1

    # write: replace only a folder this script made before
    if out.exists():
        if not (out / MARKER).exists() and any(out.iterdir()):
            print(f"site_build: {out} exists and was not made by this script; refusing to replace it")
            return 1
        shutil.rmtree(out)
    out.mkdir(parents=True)
    (out / MARKER).write_text("written by tools/site_build.py; deleted and rewritten on every build\n")
    for name, html in pages.items():
        (out / name).parent.mkdir(parents=True, exist_ok=True)
        (out / name).write_text(html, encoding="utf-8")
    total = 0
    for dest, src in files.items():
        target = out / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, target)
        total += target.stat().st_size
    total += sum((out / n).stat().st_size for n in pages)
    first_view = sum((out / n).stat().st_size for n in list(pages) + ["style.css", "site.js", "favicon.svg",
                                                                      "media/posters/lab-hero.jpg"])
    print(f"site_build: {out} written for '{brand}': {len(pages)} page(s), {len(files)} files, "
          f"{total / 1e6:.1f} MB in all, {first_view / 1e3:.0f} kB for the first view; "
          f"links checked against {branch_name}; "
          f"{len(patterns)} never-public patterns ({len(patterns) - len(NEVER_PUBLIC)} private) found nothing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
