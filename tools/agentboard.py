#!/usr/bin/env python3
"""agentboard.py - the agent leaderboard: which model, with which tool, solved which task.

Reads challenges/agents/runs/*.json (format openphysicsai-agent-run) and writes challenges/agents/AGENT_LEADERBOARD.md.
A run counts as solved only when acceptance.passed is true and acceptance.rerun_by is filled (a maintainer reran the
acceptance test from the commit). Ranking: tasks solved, then total cost where known. No dependencies.
"""
import datetime as dt
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNS = ROOT / "challenges" / "agents" / "runs"
OUT = ROOT / "challenges" / "agents" / "AGENT_LEADERBOARD.md"


def main():
    runs, problems = [], []
    for f in sorted(RUNS.glob("*.json")) if RUNS.is_dir() else []:
        try:
            d = json.loads(f.read_text())
        except json.JSONDecodeError as e:
            problems.append(f"{f.name}: not JSON ({e})")
            continue
        if d.get("format") != "openphysicsai-agent-run":
            problems.append(f"{f.name}: not an agent run")
            continue
        d["_file"] = f.name
        runs.append(d)
    by_model = {}
    for r in runs:
        key = f"{r['model'].get('name','?')} {r['model'].get('version','')}".strip()
        m = by_model.setdefault(key, {"solved": set(), "attempted": set(), "cost": 0.0, "cost_known": True, "tools": set()})
        m["attempted"].add(r["task"])
        acc = r.get("acceptance") or {}
        if acc.get("passed") is True and acc.get("rerun_by"):
            m["solved"].add(r["task"])
        if r.get("cost_usd") is None:
            m["cost_known"] = False
        else:
            m["cost"] += float(r["cost_usd"])
        m["tools"].add((r.get("agent") or {}).get("tool", "?"))
    ranked = sorted(by_model.items(), key=lambda kv: (-len(kv[1]["solved"]), kv[1]["cost"] if kv[1]["cost_known"] else 1e9))
    lines = ["# Agent leaderboard", "",
             f"Generated {dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%d %H:%M UTC')} by `tools/agentboard.py` from "
             f"`challenges/agents/runs/`. Rules: [README.md](README.md). Tasks: [TASKS.md](../../TASKS.md).", "",
             "A task counts as solved only when a maintainer reran its acceptance test from the recorded commit. Cost and time are self-reported.", "",
             "| # | model | tasks solved | attempted | tools | total cost (USD) |", "|---|---|---|---|---|---|"]
    if not ranked:
        lines.append("| | no run recorded yet | | | | |")
    for i, (model, m) in enumerate(ranked, 1):
        cost = f"{m['cost']:.2f}" if m["cost_known"] else "not fully reported"
        lines.append(f"| {i} | {model} | {len(m['solved'])}: {', '.join(sorted(m['solved'])) or '-'} | {len(m['attempted'])} | {', '.join(sorted(m['tools']))} | {cost} |")
    lines += ["", "## Every recorded run", "", "| date | task | model | tool | passed on rerun | wall (s) | cost (USD) | commit | file |", "|---|---|---|---|---|---|---|---|---|"]
    for r in sorted(runs, key=lambda r: r.get("date", "")):
        acc = r.get("acceptance") or {}
        if acc.get("passed") is True and acc.get("rerun_by"):
            passed = "yes"
        elif acc.get("passed") is True:
            passed = "claimed, not rerun"
        elif acc.get("passed") is None:
            passed = "not rerun yet"
        else:
            passed = "no"
        lines.append(f"| {r.get('date','')} | {r['task']} | {r['model'].get('name','?')} | {(r.get('agent') or {}).get('tool','?')} | {passed} | "
                     f"{r.get('wall_s') if r.get('wall_s') is not None else '-'} | {r.get('cost_usd') if r.get('cost_usd') is not None else '-'} | `{r.get('commit','')}` | {r['_file']} |")
    if problems:
        lines += ["", "## Files refused", ""] + [f"- {p}" for p in problems]
    OUT.write_text("\n".join(lines) + "\n")
    print(f"agent board: {len(runs)} runs, {len(ranked)} models -> {OUT.relative_to(ROOT)}")
    return 2 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
