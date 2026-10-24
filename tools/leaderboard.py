#!/usr/bin/env python3
"""leaderboard.py - the challenge leaderboards, generated from the result files.

Reads every challenges/*/challenge.json, scores the result files in its results directory under its own rules, and
writes challenges/<id>/LEADERBOARD.md and one static page docs/leaderboard/index.html. No dependencies beyond the
standard library. Exit 0 when every challenge could be scored, 2 when a challenge or a result file is unreadable.

Rules implemented (see challenge.json):
  - an entry is one solver name at one commit; the newest run per case wins if a case was run twice
  - a held-out case counts only if the file declares inherent_strain.fitted_on_this_case == false
  - a complete entry has every held-out case in every orientation; incomplete entries are listed, not ranked
  - primary score: mean absolute percentage error against the measurement over the held-out cases
  - laptop category: every case with run.wall_s <= 600 and run.peak_rss_mb <= 8192; missing figures exclude the entry
  - reference solvers (named in the challenge, or with no held-out results and no declaration) are listed, never ranked
"""
import csv
import datetime as dt
import html
import json
import pathlib
import statistics
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CHALLENGES = ROOT / "challenges"
SITE = ROOT / "docs" / "leaderboard"


def read_measurements(path):
    rows = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(l for l in f if not l.startswith("#")):
            rows[r["strategy"].strip()] = r
    return rows


def norm_strategy(s):
    s = s.replace("Calibrated-", "").replace("Original (OT)", "Ot").replace("OriginalOT", "Ot").replace("Original", "Ot")
    return {"Rot": "Rotation"}.get(s, s)


def load_results(directory):
    out, problems = [], []
    for f in sorted(directory.glob("*.json")):
        try:
            d = json.loads(f.read_text())
        except json.JSONDecodeError as e:
            problems.append(f"{f.name}: not JSON ({e})")
            continue
        if d.get("format") != "openphysicsai-validation-result":
            problems.append(f"{f.name}: not a validation result")
            continue
        d["_file"] = f.name
        out.append(d)
    return out, problems


def pct(a, b):
    return 100.0 * (a - b) / b if b else float("nan")


def score_challenge(ch):
    meas = read_measurements(ROOT / ch["data"]["measurements"])
    results, problems = load_results(ROOT / ch["data"]["results_directory"])
    q = ch["quantity"]
    held = ch["held_out_set"]
    oris = ch["orientations"]
    cond = ch["condition"]
    geom = ROOT / ch["data"]["geometry"]
    import hashlib
    want_hash = hashlib.sha256(geom.read_bytes()).hexdigest() if geom.exists() else None

    # group by (solver name, commit); newest run per case
    entries = {}
    for r in results:
        key = (r["solver"]["name"], r["solver"].get("git_commit", "?"))
        e = entries.setdefault(key, {"cases": {}, "files": [], "notes": r["solver"].get("notes", ""),
                                     "version": r["solver"].get("version", "")})
        e["files"].append(r["_file"])
        strat = norm_strategy(r["case"]["strategy"])
        ori = r["case"].get("orientation") or "X"
        if ori == "n/a":
            ori = "X"
        ck = (strat, ori)
        prev = e["cases"].get(ck)
        started = (r.get("run") or {}).get("started") or ""
        if prev is None or started >= prev.get("_started", ""):
            r["_started"] = started
            e["cases"][ck] = r

    scored = []
    for (name, commit), e in entries.items():
        rows, undeclared, missing = [], [], []
        laptop_ok = True
        provisional = False
        geometry_ok = True
        for strat in held:
            for ori in oris:
                r = e["cases"].get((strat, ori))
                if r is None:
                    missing.append(f"{strat} {ori}")
                    continue
                fitted = (r.get("inherent_strain") or {}).get("fitted_on_this_case")
                if fitted is None:
                    undeclared.append(f"{strat} {ori}")
                    continue
                if fitted:
                    undeclared.append(f"{strat} {ori} (fitted on it)")
                    continue
                if r["geometry"].get("sha256") != want_hash:
                    geometry_ok = False
                m = float(meas[strat][f"{ori}_{cond}"])
                v = float(r["result"][q])
                run = r.get("run") or {}
                if run.get("wall_s") is None or run.get("peak_rss_mb") is None:
                    laptop_ok = False
                elif run["wall_s"] > 600 or run["peak_rss_mb"] > 8192:
                    laptop_ok = False
                provisional = provisional or bool(r.get("provisional"))
                if r["solver"].get("notes"):
                    e["notes"] = r["solver"]["notes"]  # describe the entry by a predicted case, not by a fit residual
                rows.append({"strategy": strat, "orientation": ori, "predicted": v, "measured": m, "error_pct": pct(v, m),
                             "element_mm": r["mesh"]["element_size_mm"], "layer_mm": (r.get("layer_scheme") or {}).get("layer_thickness_mm"),
                             "source": (r.get("inherent_strain") or {}).get("source", "")})
        complete = not missing and not undeclared and len(rows) == len(held) * len(oris)
        errs = [abs(x["error_pct"]) for x in rows]
        # ordering (A2): X falls and Y rises with finer hatch across held-out + calibration where present
        def series(ori):
            pts = []
            for strat in ch["calibration_set"] + held:
                r = e["cases"].get((strat, ori))
                if r is not None:
                    hatch = meas[strat]["hatch_spacing_um"]
                    if hatch:
                        pts.append((float(hatch), float(r["result"][q])))
            pts.sort(reverse=True)
            return [p[1] for p in pts]
        sx, sy = series("X"), series("Y")
        ordering = None
        if len(sx) >= 2 and len(sy) >= 2:
            ordering = all(a > b for a, b in zip(sx, sx[1:])) and all(a < b for a, b in zip(sy, sy[1:]))
        is_reference = name in ch.get("reference_solvers", []) or (not rows and not undeclared and missing == [f"{s} {o}" for s in held for o in oris])
        scored.append({
            "solver": name, "commit": commit, "version": e["version"], "notes": e["notes"], "rows": rows,
            "missing": missing, "undeclared": undeclared, "complete": complete,
            "mean_abs_pct": statistics.fmean(errs) if errs else None,
            "max_abs_pct": max(errs) if errs else None,
            "ordering": ordering, "laptop": laptop_ok and complete, "provisional": provisional,
            "geometry_ok": geometry_ok, "reference": is_reference, "n_files": len(e["files"]),
        })
    ranked = sorted([s for s in scored if s["complete"] and s["geometry_ok"] and not s["reference"]], key=lambda s: s["mean_abs_pct"])
    # Ranks are shared when two entries differ by less than the measurement scatter, expressed as a mean percentage
    # over the held-out cases (challenge "scatter_pct", or computed from a scatter file next to the measurements).
    scatter_pct = ch.get("scatter_pct")
    sc_path = (ROOT / ch["data"]["measurements"]).parent / "scatter.json"
    if scatter_pct is None and sc_path.exists():
        try:
            sc = json.loads(sc_path.read_text())
            fr = [float(sc[st][f"{o}_{cond}"]) / float(meas[st][f"{o}_{cond}"]) * 100.0
                  for st in held for o in oris if st in sc and f"{o}_{cond}" in sc[st]]
            scatter_pct = statistics.fmean(fr) if fr else None
        except (json.JSONDecodeError, KeyError, ValueError, ZeroDivisionError):
            scatter_pct = None
    rank = 0
    for i, s in enumerate(ranked):
        if i == 0 or scatter_pct is None or s["mean_abs_pct"] - ranked[i - 1]["mean_abs_pct"] > scatter_pct:
            rank = i + 1
        s["rank"] = rank
        s["tied"] = i > 0 and s["rank"] == ranked[i - 1]["rank"]
    ch["_scatter_pct"] = scatter_pct
    others = [s for s in scored if s not in ranked]
    return {"challenge": ch, "ranked": ranked, "others": others, "problems": problems, "measurements": meas}


# ---- path-profile challenges (challenge.json "kind": "path_profile"): a field along a node path against a reference
# table of another model, scored in the quantity's own unit; the cantilever scoring above is untouched by them


def read_path_table(path):
    """a reference node table exported by a commercial solver: comment lines, then ID;Label;value;Geometry;x;y;z"""
    rows = []
    with open(path, encoding="latin-1") as f:
        for line in f:
            if not line[:1].isdigit():
                continue
            c = line.strip().split(";")
            rows.append({"node": int(c[0]), "value": float(c[2]), "z_mm": float(c[6])})
    return rows


def score_path_challenge(ch):
    ref = {st: {state: read_path_table(ROOT / f) for state, f in files.items()}
           for st, files in ch["data"]["reference_tables"].items()}
    rdir = ROOT / ch["data"]["results_directory"]
    results, problems = [], []
    for f in sorted(rdir.glob("*.json")) if rdir.exists() else []:
        try:
            d = json.loads(f.read_text())
        except json.JSONDecodeError as e:
            problems.append(f"{f.name}: not JSON ({e})")
            continue
        if d.get("format") != ch["result_format"]["format"]:
            problems.append(f"{f.name}: not a {ch['result_format']['format']} file")
            continue
        d["_file"] = f.name
        results.append(d)
    entries = {}
    for r in results:
        key = (r["solver"]["name"], r["solver"].get("git_commit", "?"))
        e = entries.setdefault(key, {"cases": {}, "files": [], "notes": r["solver"].get("notes", ""),
                                     "version": r["solver"].get("version", "")})
        e["files"].append(r["_file"])
        st = norm_strategy(r["case"]["strategy"])
        started = (r.get("run") or {}).get("started") or ""
        if st not in e["cases"] or started >= e["cases"][st].get("_started", ""):
            r["_started"] = started
            e["cases"][st] = r
    scored = []
    for (name, commit), e in entries.items():
        rows, missing, undeclared, mismatch = [], [], [], []
        laptop_ok, geometry_ok = True, True
        for st in ch["strategies"]:
            r = e["cases"].get(st)
            if r is None:
                missing.append(st)
                continue
            tuned = (r.get("inherent_strain") or {}).get("tuned_on_reference")
            if tuned is None or tuned:
                undeclared.append(st + (" (tuned on the reference)" if tuned else ""))
                continue
            if (r.get("geometry") or {}).get("sha256") != ch["data"]["geometry_sha256"]:
                geometry_ok = False
            run = r.get("run") or {}
            if run.get("wall_s") is None or run.get("peak_rss_mb") is None or run["wall_s"] > 600 or run["peak_rss_mb"] > 8192:
                laptop_ok = False
            row = {"strategy": st, "diffs": [], "diffs_inner": [], "max": {}}
            for state, table in ref[st].items():
                got = {p["node"]: p for p in (r.get("path") or {}).get(state, [])}
                vals = []
                for i, p in enumerate(table):
                    q = got.get(p["node"])
                    if q is None or q.get("von_mises_mpa") is None:
                        mismatch.append(f"{st} {state} node {p['node']} missing")
                        continue
                    if abs(q["z_mm"] - p["z_mm"]) > 0.05:
                        mismatch.append(f"{st} {state} node {p['node']} at z {q['z_mm']:.3f}, the reference at {p['z_mm']:.3f}")
                        continue
                    d = abs(q["von_mises_mpa"] - p["value"])
                    row["diffs"].append(d)
                    if i < len(table) - 1:
                        row["diffs_inner"].append(d)
                    vals.append(q["von_mises_mpa"])
                row["max"][state] = (max(vals) if vals else None, max(p["value"] for p in table))
            rows.append(row)
        complete = not missing and not undeclared and not mismatch and len(rows) == len(ch["strategies"])
        alld = [d for r in rows for d in r["diffs"]]
        inner = [d for r in rows for d in r["diffs_inner"]]
        release = None
        if complete:
            drop = {r["strategy"]: r["max"]["before"][0] - r["max"]["after"][0] for r in rows}
            ref_drop = {r["strategy"]: r["max"]["before"][1] - r["max"]["after"][1] for r in rows}
            release = max(drop, key=drop.get) == max(ref_drop, key=ref_drop.get)
        scored.append({"solver": name, "commit": commit, "version": e["version"], "notes": e["notes"], "rows": rows,
                       "missing": missing, "undeclared": undeclared, "mismatch": mismatch, "complete": complete,
                       "mean_abs": statistics.fmean(alld) if alld else None,
                       "mean_abs_inner": statistics.fmean(inner) if inner else None,
                       "release_ordering": release, "laptop": laptop_ok and complete, "geometry_ok": geometry_ok,
                       "reference": name in ch.get("reference_solvers", []), "n_files": len(e["files"])})
    ranked = sorted([s for s in scored if s["complete"] and s["geometry_ok"] and not s["reference"]], key=lambda s: s["mean_abs"])
    others = [s for s in scored if s not in ranked]
    return {"challenge": ch, "ranked": ranked, "others": others, "problems": problems, "reference": ref}


def path_markdown(sc):
    ch = sc["challenge"]
    out = [f"# Leaderboard: {ch['title']}", "",
           f"Generated {dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%d %H:%M UTC')} by `tools/leaderboard.py` from "
           f"`{ch['data']['results_directory']}/`. Rules: [challenge.json](challenge.json), [CHALLENGE.md](CHALLENGE.md).",
           "", f"**Scored:** {ch['score']['primary']}.", "",
           f"**The reference is another model, not a measurement:** {ch['status_note']}", "",
           "## Ranked entries (complete, declared, replayable)", "",
           "| # | solver | commit | mean abs difference | without the end nodes | release ordering | laptop class |",
           "|---|---|---|---|---|---|---|"]
    if not sc["ranked"]:
        out.append("| | no complete entry yet | | | | | |")
    for i, s in enumerate(sc["ranked"], 1):
        out.append(f"| {i} | {s['solver']} {s['version']} | `{s['commit']}` | {fmt(s['mean_abs'], 1)} MPa | "
                   f"{fmt(s['mean_abs_inner'], 1)} MPa | {'yes' if s['release_ordering'] else 'no'} | "
                   f"{'yes' if s['laptop'] else 'no'} |")
    for s in sc["ranked"]:
        out += ["", f"### {s['solver']} `{s['commit']}`", "", (s["notes"] or "").strip(), "",
                "| strategy | path max before (entry / reference) | path max after (entry / reference) | mean abs difference |",
                "|---|---|---|---|"]
        for r in s["rows"]:
            b, a = r["max"]["before"], r["max"]["after"]
            out.append(f"| {r['strategy']} | {fmt(b[0], 0)} / {fmt(b[1], 0)} MPa | {fmt(a[0], 0)} / {fmt(a[1], 0)} MPa | "
                       f"{fmt(statistics.fmean(r['diffs']) if r['diffs'] else None, 1)} MPa |")
    out += ["", "## Listed, not ranked", "", "| solver | commit | why | files |", "|---|---|---|---|"]
    for s in sc["others"]:
        why = []
        if s["reference"]:
            why.append("reference model")
        if s["missing"]:
            why.append("missing: " + ", ".join(s["missing"]))
        if s["undeclared"]:
            why.append("undeclared or tuned: " + ", ".join(s["undeclared"]))
        if s["mismatch"]:
            why.append("path does not match the reference nodes: " + "; ".join(s["mismatch"][:3]))
        if not s["geometry_ok"]:
            why.append("computed on another geometry file")
        out.append(f"| {s['solver']} {s['version']} | `{s['commit']}` | {'; '.join(why) or 'incomplete'} | {s['n_files']} |")
    if sc["problems"]:
        out += ["", "## Files refused", ""] + [f"- {p}" for p in sc["problems"]]
    out += ["", "A difference from this reference is a difference between two models. Quote it as a challenge score, "
            "never as validated accuracy.", ""]
    return "\n".join(out)


def path_html(sc):
    ch = sc["challenge"]
    parts = [f"<h2>{html.escape(ch['title'])}</h2><p class='muted'>{html.escape(ch['question'])} "
             f"<span class='tag'>{html.escape(ch['status'])}</span></p>",
             "<table><thead><tr><th>#</th><th>solver</th><th>commit</th><th>mean abs difference</th>"
             "<th>without end nodes</th><th>release ordering</th><th>laptop class</th></tr></thead><tbody>"]
    if not sc["ranked"]:
        parts.append("<tr><td colspan='7'>no complete entry yet</td></tr>")
    for i, s in enumerate(sc["ranked"], 1):
        parts.append(f"<tr><td class='rank'>{i}</td><td>{html.escape(s['solver'])} {html.escape(s['version'])}</td>"
                     f"<td><code>{html.escape(s['commit'])}</code></td><td class='num'>{fmt(s['mean_abs'], 1)} MPa</td>"
                     f"<td class='num'>{fmt(s['mean_abs_inner'], 1)} MPa</td><td>{'yes' if s['release_ordering'] else 'no'}</td>"
                     f"<td>{'yes' if s['laptop'] else 'no'}</td></tr>")
    parts.append("</tbody></table>")
    parts.append(f"<p class='muted'>{html.escape(ch['status_note'])}</p>")
    return "\n".join(parts)


def fmt(x, d=2):
    return "-" if x is None else f"{x:.{d}f}"


def markdown(sc):
    ch = sc["challenge"]
    out = [f"# Leaderboard: {ch['title']}", "",
           f"Generated {dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%d %H:%M UTC')} by `tools/leaderboard.py` from "
           f"`{ch['data']['results_directory']}/`. Rules: [challenge.json](challenge.json), [CHALLENGE.md](CHALLENGE.md).",
           "", f"**Scored:** {ch['score']['primary']}.", "",
           f"**Held out:** {', '.join(ch['held_out_set'])} in {' and '.join(ch['orientations'])}. "
           f"**Measurement scatter:** {ch['measurement_uncertainty']}"
           + (f"; entries within {ch['_scatter_pct']:.2f} % of each other share a rank (marked =)." if ch.get('_scatter_pct') is not None
              else "; no scatter is declared, so differences of a few percent between entries are not meaningful and no tie can be declared."), ""]
    out += ["## Ranked entries (complete, declared, replayable)", "",
            "| # | solver | commit | mean abs error | largest error | ordering (A2) | laptop class | provisional |",
            "|---|---|---|---|---|---|---|---|"]
    if not sc["ranked"]:
        out.append("| | no complete entry yet | | | | | | |")
    for s in sc["ranked"]:
        i = f"{s['rank']}{'=' if s['tied'] else ''}"
        out.append(f"| {i} | {s['solver']} {s['version']} | `{s['commit']}` | {fmt(s['mean_abs_pct'])} % | {fmt(s['max_abs_pct'])} % | "
                   f"{'yes' if s['ordering'] else ('no' if s['ordering'] is False else 'n/a')} | {'yes' if s['laptop'] else 'no run figures' if s['complete'] else '-'} | "
                   f"{'yes' if s['provisional'] else 'no'} |")
    for s in sc["ranked"]:
        out += ["", f"### {s['solver']} `{s['commit']}`", "", (s["notes"] or "").strip(), "",
                "| case | predicted (mm) | measured (mm) | error | elements / layer (mm) |", "|---|---|---|---|---|"]
        for r in s["rows"]:
            out.append(f"| {r['strategy']} {r['orientation']} | {r['predicted']:.4f} | {r['measured']:.2f} | {r['error_pct']:+.2f} % | {r['element_mm']} / {fmt(r['layer_mm'],2)} |")
    out += ["", "## Listed, not ranked", "", "| solver | commit | why | files |", "|---|---|---|---|"]
    for s in sc["others"]:
        why = []
        if s["reference"]:
            why.append("no held-out case predicted (reference model or borrowed tensors)")
        if s["missing"] and not s["reference"]:
            why.append("missing: " + ", ".join(s["missing"]))
        if s["undeclared"]:
            why.append("undeclared or fitted: " + ", ".join(s["undeclared"]))
        if not s["geometry_ok"]:
            why.append("computed on another geometry file")
        out.append(f"| {s['solver']} {s['version']} | `{s['commit']}` | {'; '.join(why) or 'incomplete'} | {s['n_files']} |")
    if sc["problems"]:
        out += ["", "## Files refused", ""] + [f"- {p}" for p in sc["problems"]]
    out += ["", "Every ranked number is provisional while the geometry file carries assumptions (cut height, kerf). "
            "No entry may be quoted as validated accuracy; quote it as a challenge score.", ""]
    return "\n".join(out)


def page(all_scores):
    css = """
:root{--bg:#f7f7f5;--fg:#1a1a1a;--muted:#5a5a5a;--line:#d8d8d3;--accent:#0b6e8f;--card:#ffffff}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#111417;--fg:#e8e8e4;--muted:#9a9a94;--line:#2b3036;--accent:#5fb3d3;--card:#181c21}}
:root[data-theme="dark"]{--bg:#111417;--fg:#e8e8e4;--muted:#9a9a94;--line:#2b3036;--accent:#5fb3d3;--card:#181c21}
body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);font:15px/1.5 -apple-system,Segoe UI,Helvetica,Arial,sans-serif;max-width:1100px;margin-inline:auto}
h1{font-size:26px;margin:0 0 4px}h2{font-size:19px;margin:28px 0 8px}h3{font-size:16px;margin:18px 0 6px}
p.muted{color:var(--muted);margin:0 0 16px}
table{border-collapse:collapse;width:100%;background:var(--card);border:1px solid var(--line);margin:8px 0 16px;display:block;overflow-x:auto}
th,td{padding:7px 10px;border-bottom:1px solid var(--line);text-align:left;white-space:nowrap}th{color:var(--muted);font-weight:600}
td.num{font-variant-numeric:tabular-nums}code{font-size:13px}
.rank{font-weight:700;color:var(--accent)}.tag{display:inline-block;padding:1px 7px;border:1px solid var(--line);border-radius:10px;font-size:12px;color:var(--muted)}
"""
    css += """
body{max-width:1200px;background:#0b1220;color:#e4edf7;padding:48px 24px;--muted:#a6b8cb;--card:#121e30;--line:#293b50;--accent:#72e4d3}
h1{font-size:clamp(30px,5vw,58px);letter-spacing:-.045em;line-height:1.1;margin-bottom:16px}
.challenge{border:1px solid var(--line);border-radius:18px;padding:24px;margin:24px 0;background:var(--card)}
.challenge h2{margin-top:0}nav{display:flex;align-items:center;gap:16px;flex-wrap:wrap;padding:20px 0}
input,select{font:inherit;background:#121e30;color:#e4edf7;border:1px solid #526880;border-radius:8px;padding:10px;max-width:100%;box-sizing:border-box}
button{font:inherit;background:none;color:inherit;border:0;cursor:pointer;padding:4px;text-align:left}button:hover{color:#72e4d3}
summary{cursor:pointer;padding:14px 0;color:#72e4d3}details{border-top:1px solid var(--line)}
[hidden]{display:none!important}a{color:#72e4d3}nav p{color:var(--muted)}
"""
    parts = [f"<!doctype html><html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
             f"<title>OpenPhysicsAI leaderboards</title><style>{css}</style></head><body>",
             "<p class='muted'>OPENPHYSICSAI / THE CHALLENGE BOARD</p><h1>How close can we get to reality?</h1><p>Inspect a prediction. Replay an entry. Improve the next result.</p><p><a href='../../challenges/agents/AGENT_LEADERBOARD.md'>Agent rankings</a> · <a href='../../challenges/README.md'>Challenge rules</a> · <a href='../../CONTRIBUTING.md'>Enter a result</a></p>",
             f"<p class='muted'>Simulation against measured reality. Any solver may enter. Generated "
             f"{dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%d %H:%M UTC')} from the result files in the repository.</p>"]
    if not all_scores:
        parts.append("<p>No challenge with measured data is open yet. <a href='../../challenges/README.md'>How to add one</a>.</p>")
    for sc in all_scores:
        ch = sc["challenge"]
        if ch.get("kind") == "path_profile":
            parts.append(path_html(sc))
            continue
        parts.append(f"<h2>{html.escape(ch['title'])}</h2><p class='muted'>{html.escape(ch['question'])} "
                     f"<span class='tag'>{html.escape(ch['status'])}</span></p>")
        parts.append("<table><thead><tr><th>#</th><th>solver</th><th>commit</th><th>mean abs error</th><th>largest</th>"
                     "<th>ordering</th><th>laptop class</th><th>provisional</th></tr></thead><tbody>")
        if not sc["ranked"]:
            parts.append("<tr><td colspan='8'>no complete entry yet</td></tr>")
        for s in sc["ranked"]:
            i = f"{s['rank']}{'=' if s['tied'] else ''}"
            parts.append(f"<tr><td class='rank'>{i}</td><td>{html.escape(s['solver'])} {html.escape(s['version'])}</td>"
                         f"<td><code>{html.escape(s['commit'])}</code></td><td class='num'>{fmt(s['mean_abs_pct'])} %</td>"
                         f"<td class='num'>{fmt(s['max_abs_pct'])} %</td><td>{'yes' if s['ordering'] else 'no' if s['ordering'] is False else 'n/a'}</td>"
                         f"<td>{'yes' if s['laptop'] else 'no run figures'}</td><td>{'yes' if s['provisional'] else 'no'}</td></tr>")
        parts.append("</tbody></table>")
        for s in sc["ranked"]:
            parts.append(f"<h3>{html.escape(s['solver'])} <code>{html.escape(s['commit'])}</code></h3><p class='muted'>{html.escape(s['notes'] or '')}</p>")
            parts.append("<table><thead><tr><th>case</th><th>predicted (mm)</th><th>measured (mm)</th><th>error</th><th>elements / layer (mm)</th></tr></thead><tbody>")
            for r in s["rows"]:
                parts.append(f"<tr><td>{r['strategy']} {r['orientation']}</td><td class='num'>{r['predicted']:.4f}</td><td class='num'>{r['measured']:.2f}</td>"
                             f"<td class='num'>{r['error_pct']:+.2f} %</td><td>{r['element_mm']} / {fmt(r['layer_mm'])}</td></tr>")
            parts.append("</tbody></table>")
        if sc["others"]:
            parts.append("<h3>Listed, not ranked</h3><table><thead><tr><th>solver</th><th>commit</th><th>why</th></tr></thead><tbody>")
            for s in sc["others"]:
                why = "reference model" if s["reference"] else ("missing " + ", ".join(s["missing"]) if s["missing"] else "") + (" undeclared " + ", ".join(s["undeclared"]) if s["undeclared"] else "")
                parts.append(f"<tr><td>{html.escape(s['solver'])}</td><td><code>{html.escape(s['commit'])}</code></td><td>{html.escape(why or 'incomplete')}</td></tr>")
            parts.append("</tbody></table>")
        parts.append(f"<p class='muted'>Scatter of the measurements: {html.escape(ch['measurement_uncertainty'])}. "
                     "Ranked numbers are provisional while the geometry carries assumptions; quote them as challenge scores, not as validated accuracy.</p>")
    parts.append("""<script>
const body = document.body;
const headings = [...body.querySelectorAll('h2')];
headings.forEach((heading, i) => {
  const section = document.createElement('section');
  section.className = 'challenge';
  section.id = 'challenge-' + i;
  heading.before(section);
  section.append(heading);
  while (section.nextSibling && !(section.nextSibling.nodeType === 1 &&
         ['H2', 'SCRIPT'].includes(section.nextSibling.tagName))) {
    section.append(section.nextSibling);
  }
  section.querySelectorAll('h3').forEach(heading => {
    const details = document.createElement('details');
    const summary = document.createElement('summary');
    summary.textContent = heading.textContent;
    heading.before(details);
    heading.remove();
    details.append(summary);
    let next = details.nextElementSibling;
    if (next?.tagName === 'P') {
      details.append(next);
      next = details.nextElementSibling;
    }
    if (next?.tagName === 'TABLE') details.append(next);
  });
});
const nav = document.createElement('nav');
nav.setAttribute('aria-label', 'Challenge selection');
const label = document.createElement('label');
label.textContent = 'Search entries ';
const input = document.createElement('input');
input.type = 'search';
input.placeholder = 'Solver, commit or case';
label.append(input);
nav.append(label);
const select = document.createElement('select');
select.setAttribute('aria-label', 'Choose challenge');
select.add(new Option('All challenges', 'all'));
headings.forEach((heading, i) => select.add(new Option(heading.textContent, 'challenge-' + i)));
nav.append(select);
const status = document.createElement('p');
status.setAttribute('aria-live', 'polite');
nav.append(status);
headings[0]?.parentElement.before(nav);
function filter() {
  let count = 0;
  document.querySelectorAll('.challenge').forEach(section => {
    section.hidden = select.value !== 'all' && section.id !== select.value;
    section.querySelectorAll('tbody tr').forEach(row => {
      row.hidden = !row.textContent.toLowerCase().includes(input.value.toLowerCase());
      if (!section.hidden && !row.hidden) count++;
    });
    section.querySelectorAll('details').forEach(details => {
      details.hidden = !!input.value &&
        ![...details.querySelectorAll('tbody tr')].some(row => !row.hidden);
      if (input.value && !details.hidden) details.open = true;
    });
  });
  status.textContent = count + ' matching rows';
}
input.addEventListener('input', filter);
select.addEventListener('change', filter);
document.querySelectorAll('table').forEach(table => {
  table.querySelectorAll('th').forEach((heading, index) => {
    const button = document.createElement('button');
    button.textContent = heading.textContent + ' ↕';
    heading.textContent = '';
    heading.append(button);
    let ascending = true;
    button.onclick = () => {
      const rows = [...table.tBodies[0].rows];
      rows.sort((a, b) => {
        const x = a.cells[index]?.textContent.trim() || '';
        const y = b.cells[index]?.textContent.trim() || '';
        const nx = parseFloat(x), ny = parseFloat(y);
        return (ascending ? 1 : -1) *
          (Number.isFinite(nx) && Number.isFinite(ny) ? nx - ny : x.localeCompare(y));
      });
      rows.forEach(row => table.tBodies[0].append(row));
      table.querySelectorAll('th').forEach(h => h.removeAttribute('aria-sort'));
      heading.setAttribute('aria-sort', ascending ? 'ascending' : 'descending');
      ascending = !ascending;
    };
  });
});
filter();
</script></body></html>""")
    return "\n".join(parts)


def main():
    status = 0
    all_scores = []
    for cj in sorted(CHALLENGES.glob("*/challenge.json")):
        try:
            ch = json.loads(cj.read_text())
        except json.JSONDecodeError as e:
            print(f"{cj}: not JSON ({e})")
            status = 2
            continue
        path = ch.get("kind") == "path_profile"
        sc = score_path_challenge(ch) if path else score_challenge(ch)
        (cj.parent / "LEADERBOARD.md").write_text(path_markdown(sc) if path else markdown(sc))
        all_scores.append(sc)
        print(f"{ch['id']}: {len(sc['ranked'])} ranked, {len(sc['others'])} listed, {len(sc['problems'])} refused -> {cj.parent.name}/LEADERBOARD.md")
        for s in sc["ranked"]:
            if path:
                print(f"  #{sc['ranked'].index(s)+1} {s['solver']} {s['commit']}: mean {s['mean_abs']:.1f} MPa, "
                      f"without end nodes {s['mean_abs_inner']:.1f} MPa")
            else:
                print(f"  #{sc['ranked'].index(s)+1} {s['solver']} {s['commit']}: mean {s['mean_abs_pct']:.2f} %, max {s['max_abs_pct']:.2f} %")
        if sc["problems"]:
            status = 2
    SITE.mkdir(parents=True, exist_ok=True)
    (SITE / "index.html").write_text(page(all_scores))
    print(f"site: {SITE.relative_to(ROOT)}/index.html")
    return status


if __name__ == "__main__":
    sys.exit(main())
