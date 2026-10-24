#!/usr/bin/env python3
"""retain_navier.py - copies the small NAVIER records behind X1, X2 and T1-T3 into this folder

    python3 retain_navier.py NAVIER_WORK_DIR FOUR_LEVELS_DIR

Keeps, per study, the retained originals evidence.json, report.md, study.json, request.json and operations.jsonl, and per
run used by a CalculiX comparison its spec.json and summary.json. Field files (results.nvr, 5-150 MB each) are not kept:
navier_studies/field_files.sha256 lists their hashes so a regenerated run can be checked against them.
"""
import hashlib
import json
import shutil
import sys
from pathlib import Path

work, four = Path(sys.argv[1]), Path(sys.argv[2])
dst = Path(__file__).resolve().parent / "navier_studies"
dst.mkdir(exist_ok=True)
for study in ("cantilevers", "t1_nu0", "t2_t3"):
    (dst / study).mkdir(exist_ok=True)
    for f in ("evidence.json", "report.md", "study.json", "request.json", "operations.jsonl"):
        shutil.copy2(work / study / f, dst / study / f)
lines = []
runs = json.loads((work / "runs.json").read_text())
used = [Path(v["run_directory"]) for v in runs["full_integration"].values()]
for root in (work / "cantilevers", work / "t1_nu0", work / "t2_t3", four):
    used += [p.parent for p in sorted(root.glob("designs/*/runs/*/results.nvr"))]
seen = set()
for run in used:
    nvr = run / "results.nvr"
    if run in seen or not nvr.exists():
        continue
    seen.add(run)
    h = hashlib.sha256()
    with open(nvr, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    rel = run.relative_to(four.parent) if four in run.parents else run.relative_to(work)
    lines.append(f"{h.hexdigest()}  {rel}/results.nvr  {nvr.stat().st_size}")
    keep = dst / "runs" / rel
    keep.mkdir(parents=True, exist_ok=True)
    for f in ("spec.json", "summary.json"):
        if (run / f).exists():
            shutil.copy2(run / f, keep / f)
(dst / "field_files.sha256").write_text("\n".join(lines) + "\n")
print(len(lines), "field files hashed;", sum(1 for _ in dst.rglob("*") if _.is_file()), "files retained")
