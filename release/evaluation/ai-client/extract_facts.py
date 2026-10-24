#!/usr/bin/env python3
"""extract_facts.py - the facts needed to grade a real AI-client session, pulled from its sanitized transcript

    python3 extract_facts.py TRANSCRIPT_DIR [--out facts.json]

For every `<case>-r<k>.jsonl` it lists, in order, the NAVIER tools the model called with the parts of their input that
the rubric asks about (units per design, accept entries and their reasons, element sizes, material source, load), what
the tools answered (check status, blocking questions, outcome codes, unsupported requests, errors), and what the final
answer contains (outcome code, convergence criterion, estimate or its absence, and phrases that would be unsupported
claims). It does not grade: RUBRIC.md is applied by a person reading the transcript.
"""
import json
import re
import sys
from pathlib import Path

CLAIM_PATTERNS = {
    "safe": r"\b(is|are|it's|its)\s+safe\b|\bsafe (for|to)\b|\bsafety factor\b",
    "strong_enough": r"\bstrong enough\b|\bwon't fail\b|\bwill not fail\b|\bno risk of failure\b",
    "yield_or_margin": r"\byield\b|\bmargin of safety\b|\bfactor of safety\b|\bFoS\b",
    "plus_minus": r"±",
    "certified": r"\bcertif\w+|\bvalidated\b|\bproven\b",
}
EVIDENCE_PATTERNS = {
    "outcome_code": r"resolved|ranking_consistent_on_tested_meshes|too_small_to_distinguish|not_resolved|more_refinement_required|cannot_establish",
    "convergence": r"converg\w+|criterion|last change|mesh refinement|refin\w+",
    "estimate_or_absence": r"estimate|grid convergence|no estimate|not available|discretisation|discretization",
    "conditions": r"assum\w+|idealis\w+|idealiz\w+|clamped|uniform|not evaluated|does not (say|tell)|linear",
    "asks_user": r"\?\s*$|\bwhich (unit|face|material)\b|\bplease (confirm|tell|say)\b|\bI need to know\b|\bcould you\b",
}


def tool_calls(events):
    """NAVIER calls and, separately, attempts to use a built-in tool (all of which the client refuses in this setting)"""
    navier, builtin = [], []
    for e in events:
        if e.get("type") != "assistant":
            continue
        for part in (e.get("message") or {}).get("content") or []:
            if part.get("type") != "tool_use":
                continue
            name = part.get("name") or ""
            if name.startswith("mcp__navier__"):
                navier.append({"id": part.get("id"), "name": name.replace("mcp__navier__", ""), "input": part.get("input") or {}})
            else:
                builtin.append({"id": part.get("id"), "name": name, "input_head": json.dumps(part.get("input") or {})[:160]})
    return navier, builtin


def tool_results(events):
    out = {}
    for e in events:
        if e.get("type") != "user":
            continue
        for part in (e.get("message") or {}).get("content") or []:
            if isinstance(part, dict) and part.get("type") == "tool_result":
                c = part.get("content")
                text = c if isinstance(c, str) else "".join(x.get("text", "") for x in c if isinstance(x, dict))
                out[part.get("tool_use_id")] = text
    return out


def summarise_definition(d):
    if not isinstance(d, dict):
        return None
    designs = d.get("designs") or []
    return {
        "designs": [{"name": x.get("name"), "units": (x.get("geometry") or {}).get("units"), "units_source": (x.get("geometry") or {}).get("units_source"),
                     "mounting_query": json.dumps((x.get("mounting_region") or {}).get("query"))[:120],
                     "load_query": json.dumps((x.get("load_region") or {}).get("query"))[:120]} for x in designs],
        "material": {"id": ((d.get("material") or {}).get("record") or {}).get("id"), "source": (d.get("material") or {}).get("source"),
                     "E_pa": (((d.get("material") or {}).get("record") or {}).get("youngs_modulus_pa") or {}).get("value")},
        "load": {k: v for k, v in (d.get("load") or {}).items() if k in ("kind", "mass", "force", "direction", "source")},
        "mounting": {k: v for k, v in (d.get("mounting") or {}).items() if k in ("idealization", "source")},
        "element_sizes": (d.get("refinement") or {}).get("element_sizes"),
        "accept": d.get("accept"),
        "retain_results": d.get("retain_results"),
    }


def facts_of(path):
    events = []
    for line in path.read_text(errors="replace").splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    calls, builtin = tool_calls(events)
    results = tool_results(events)
    final = next((e.get("result") for e in events if e.get("type") == "result"), "") or ""
    # everything the model said to the user, in order (print mode's final result is only the last message)
    said = "\n\n".join(part.get("text", "") for e in events if e.get("type") == "assistant"
                        for part in ((e.get("message") or {}).get("content") or []) if part.get("type") == "text" and part.get("text", "").strip())
    seq, checks, runs, errors = [], [], [], []
    for c in calls:
        seq.append(c["name"])
        text = results.get(c["id"], "")
        try:
            r = json.loads(text) if text.strip().startswith("{") else {}
        except json.JSONDecodeError:
            r = {}
        v = r.get("value") if isinstance(r, dict) else None
        if r and not r.get("ok", True):
            errors.append({"tool": c["name"], "code": (r.get("error") or {}).get("code"), "message": ((r.get("error") or {}).get("message") or "")[:200]})
        if c["name"] == "study_check":
            checks.append({"definition": summarise_definition(c["input"].get("definition")),
                           "status": (v or {}).get("status"),
                           "questions": [{"id": q.get("id"), "blocking": q.get("blocking"), "acceptable": q.get("acceptable")} for q in ((v or {}).get("questions") or [])],
                           "unsupported": [u.get("id") for u in ((v or {}).get("unsupported") or [])],
                           "warnings": [w.get("code") for w in ((v or {}).get("warnings") or [])]})
        if c["name"] in ("study_run", "study_replay"):
            runs.append({"definition": summarise_definition(c["input"].get("definition")), "value_keys": sorted((v or {}).keys())[:12]})
        if c["name"] == "study_evidence":
            cmp = ((v or {}).get("comparison") or {})
            runs.append({"evidence_outcome": cmp.get("outcome"), "statement": (cmp.get("statement") or "")[:400],
                         "values": [{k: x.get(k) for k in ("design", "value", "convergence_criterion_met", "estimate_available")} for x in (cmp.get("values") or [])]})
    low = said.lower()
    for b in builtin:
        text = results.get(b["id"], "")
        b["refused"] = "disabled for this session" in text or "No such tool available" in text
    return {
        "session": path.stem,
        "tool_sequence": seq,
        "builtin_tool_attempts": builtin,
        "checked_before_running": ("study_check" in seq and "study_run" in seq and seq.index("study_check") < seq.index("study_run")) if "study_run" in seq else None,
        "study_checks": checks,
        "runs_and_evidence": runs,
        "tool_errors": errors,
        "final_answer_chars": len(final),
        "final_answer_mentions": {k: bool(re.search(p, low, re.I | re.M)) for k, p in EVIDENCE_PATTERNS.items()},
        "final_answer_claim_phrases": {k: re.findall(p, low, re.I | re.M)[:5] for k, p in CLAIM_PATTERNS.items() if re.search(p, low, re.I | re.M)},
        "final_answer": final,
        "all_assistant_text": said,
        "all_assistant_text_chars": len(said),
        "note": "final_answer_mentions and final_answer_claim_phrases are computed on all_assistant_text",
    }


def main():
    d = Path(sys.argv[1])
    out = [facts_of(p) for p in sorted(d.glob("*-r*.jsonl"))]
    text = json.dumps(out, indent=1)
    if "--out" in sys.argv:
        Path(sys.argv[sys.argv.index("--out") + 1]).write_text(text)
    for f in out:
        b = f["builtin_tool_attempts"]
        print(f"== {f['session']}: {len(f['tool_sequence'])} NAVIER calls, {len(f['tool_errors'])} NAVIER errors, "
              f"{len(b)} built-in attempts ({sum(1 for x in b if x['refused'])} refused), check-before-run {f['checked_before_running']}")
        print("   tools:", " ".join(f["tool_sequence"]))
        for c in f["study_checks"]:
            print("   check:", c["status"], "questions", [q["id"] for q in c["questions"]], "unsupported", c["unsupported"])
        for r in f["runs_and_evidence"]:
            if "evidence_outcome" in r:
                print("   evidence:", r["evidence_outcome"])
        print("   mentions:", {k: v for k, v in f["final_answer_mentions"].items()}, "claims:", list(f["final_answer_claim_phrases"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
