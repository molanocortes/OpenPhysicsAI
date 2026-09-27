#!/usr/bin/env python3
"""studyflow.py - the bracket-comparison workflow over MCP stdio with a scripted client

Runs the reference cases (release/reference_cases/CRITERIA.md: R1 cantilevers against beam theory, R2 replay, R3 the
bracket example), the release evaluation set (release/evaluation/CASES.md: E1-E10) and the regression cases of the
independent evaluation (E11-E13: record write failure, unwritable operations log, access-root limit), and checks the tool behaviour
each case defines. This is a scripted client, NOT an AI model: it shows that the tools behave as specified, not how a
model uses them (release/evaluation/ai-client/ holds the real-client sessions).

Run from the repository root after `make am`:
    python3 tools/studyflow.py                 # everything (about three minutes on an M2)
    python3 tools/studyflow.py --only E2,E4    # selected cases
    python3 tools/studyflow.py --results FILE  # also write the measured values as JSON
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcptest import Client  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
GEOM = ROOT / "examples" / "bracket_comparison" / "geometry"
PASS = 0
FAIL = 0
RESULTS = {}


def check(cond, what):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"  FAIL: {what}")
    return cond


def call(c, name, args):
    r = c.call(name, args).get("result", {})
    sc = r.get("structuredContent") or {}
    images = [x for x in r.get("content", []) if x.get("type") == "image"]
    return bool(sc.get("ok")), (sc.get("value") if sc.get("ok") else sc.get("error")), images


AL = {"id": "al6061_t6_nominal", "name": "Aluminium 6061-T6, nominal handbook values", "family": "metal", "status": "user_supplied",
      "provenance": "nominal handbook values for 6061-T6; not measured for this batch",
      "youngs_modulus_pa": {"value": 68.9e9}, "poisson_ratio": {"value": 0.33}, "density_kg_m3": {"value": 2700}}
SOFT = {"id": "soft_polymer_test", "name": "soft polymer, test values", "family": "polymer", "status": "user_supplied", "provenance": "test values for the evaluation set",
        "youngs_modulus_pa": {"value": 3.5e9}, "poisson_ratio": {"value": 0.35}, "density_kg_m3": {"value": 1240}}
STEEL = {"id": "steel_test", "name": "steel, test values", "family": "metal", "status": "user_supplied", "provenance": "test values of the beam benchmark",
         "youngs_modulus_pa": {"value": 200e9}, "poisson_ratio": {"value": 0.3}, "density_kg_m3": {"value": 7850}}
MOUNT = {"query": {"plane": {"axis": "x", "at": "min"}}, "description": "back face bolted to the wall", "source": "user"}
PAD = {"query": {"box": {"min": [63, 11, 59], "max": [81, 29, 61]}}, "mode": "triangle", "description": "16 x 16 mm pad on top of the arm tip", "source": "user"}


def design(name, stl, units="mm", mounting=MOUNT, load=PAD, geom=GEOM):
    g = {"path": str(geom / stl)}
    if units:
        g["units"] = units
        g["units_source"] = "user"
    return {"name": name, "geometry": g, "mounting_region": mounting, "load_region": load}


def definition(name, designs, question="Compare the stiffness and mass of these two bracket versions under a 3 kg load at the arm tip.", material=AL,
               mounting=None, load=None, sizes=("4 mm", "2 mm", "1 mm"), **extra):
    d = {"name": name, "question": question, "designs": designs,
         "mounting": mounting or {"idealization": "fixed", "description": "back face clamped to a rigid wall", "source": "user"},
         "load": load or {"kind": "payload_mass", "mass": "3 kg", "direction": "-z", "source": "user"},
         "refinement": {"element_sizes": list(sizes)}}
    if material:
        d["material"] = {"record": material, "source": "database" if material is AL else "user", "reference": material["provenance"]}
    d.update(extra)
    return d


def run_study(c, defn, directory, timeout=1200, tool="study_run", args=None):
    ok, v, _ = call(c, tool, args or {"definition": defn, "directory": str(directory)})
    if not ok:
        print("   submission refused:", json.dumps(v)[:1500])
        return v, None, None
    jid = v["job_id"]
    t0 = time.time()
    while True:
        ok2, st, _ = call(c, "job_status", {"job_id": jid, "wait_seconds": 10})
        if not ok2 or st["state"] not in ("queued", "running") or time.time() - t0 > timeout:
            break
    ok3, ev, _ = call(c, "study_evidence", {"job_id": jid, "detail": "full"})
    return st, ev if ok3 else None, jid


def questions(v):
    return {q["id"]: q for q in (v.get("questions") or [])}


def box_stl(path, lx, ly, lz):
    p = [(0, 0, 0), (lx, 0, 0), (lx, ly, 0), (0, ly, 0), (0, 0, lz), (lx, 0, lz), (lx, ly, lz), (0, ly, lz)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, cc, d in quads:
        tris += [(p[a], p[b], p[cc]), (p[a], p[cc], p[d])]
    with open(path, "wb") as f:
        f.write(b"reference box".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


def level_values(ev, dname, key):
    for d in ev["designs"]:
        if d["name"] == dname:
            return [lv.get("quantities", {}).get(key) for lv in d["levels"] if lv.get("completed")]
    return []


# ------------------------------------------------------------------------------------------------ reference cases

def case_R1(c, work):
    print("== R1 cantilevers against Timoshenko beam theory (through study_run)")
    box_stl(work / "c10.stl", 100, 10, 10)
    box_stl(work / "c20.stl", 100, 10, 5)
    root = {"query": {"plane": {"axis": "x", "at": "min"}}, "description": "root face, clamped", "source": "user"}
    tip = {"query": {"plane": {"axis": "x", "at": "max"}}, "description": "tip face, loaded", "source": "user"}
    designs = [design("C10", "c10.stl", mounting=root, load=tip, geom=work), design("C20", "c20.stl", mounting=root, load=tip, geom=work)]
    defn = definition("cantilever_benchmark", designs, question="Which cantilever deflects less under 10 N at the tip?", material=STEEL,
                      mounting={"idealization": "fixed", "description": "clamped root", "source": "user"},
                      load={"kind": "force", "force": "10 N", "direction": "-z", "source": "user"},
                      sizes=("2.5 mm", "1.25 mm", "0.625 mm"), sensitivity={"youngs_modulus_relative": [0.9, 1.1]},
                      accept=[{"id": "equivalence", "reason": "the cross-section, and with it the root and tip face areas, is the design difference being compared"}])
    st, ev, _ = run_study(c, defn, work / "ws" / "R1")
    if not check(ev is not None, "R1: the study produced an evidence record"):
        print("   ", json.dumps(st)[:800])
        return
    E, nu, F, L = 200e3, 0.3, 10.0, 100.0
    G = E / (2 * (1 + nu))

    def timoshenko(b, h):
        return F * L ** 3 / (3 * E * b * h ** 3 / 12) + F * L / (5 / 6 * G * b * h)
    ref = {"C10": timoshenko(10, 10), "C20": timoshenko(10, 5)}
    out = {"reference_mm": ref}
    check(ev["status"] == "complete", f"R1a: status complete (got {ev['status']})")
    for d in ev["designs"]:
        check(all(lv.get("valid") for lv in d["levels"]), f"R1a: every level of {d['name']} valid")
        fine = d["levels"][-1]["quantities"]
        err = fine["load_region_displacement_mm"] / ref[d["name"]] - 1
        out[d["name"]] = {"finest_mm": fine["load_region_displacement_mm"], "relative_error": err,
                          "values_mm": [lv["quantities"]["load_region_displacement_mm"] for lv in d["levels"]],
                          "refinement": d["refinement"]["load_region_displacement_mm"]}
        check(abs(err) <= 0.02, f"R1b: {d['name']} finest displacement within 2% of beam theory (error {100 * err:.3f}%)")
        rf = d["refinement"]["load_region_displacement_mm"]
        order = rf.get("observed_order")
        changes = rf.get("relative_changes") or [1]
        check((order is not None and 0.5 <= order <= 4) or abs(changes[-1]) <= 0.02, f"R1h: {d['name']} converges (order {order}, last change {changes[-1]})")
        for lv in d["levels"]:
            ck, q = lv["checks"], lv["quantities"]
            check(ck["equilibrium_error"] <= 1e-6 and ck["moment_balance_error"] <= 1e-6 and abs(ck["energy_ratio"] - 1) <= 1e-6,
                  f"R1f: {d['name']} {lv['element_size_mm']} mm balance and energy")
            check(ck["conjugate_vs_region_mean"] <= 1e-9, f"R1f: {d['name']} {lv['element_size_mm']} mm conjugate displacement = region mean ({ck['conjugate_vs_region_mean']})")
            vol = 100 * 10 * (10 if d["name"] == "C10" else 5) * 1e-9
            check(abs(q["mass_geometry_kg"] / (7850 * vol) - 1) <= 1e-9 and abs(q["mass_mesh_kg"] / (7850 * vol) - 1) <= 1e-9,
                  f"R1e: {d['name']} mass exact ({q['mass_geometry_kg']}, {q['mass_mesh_kg']})")
    vals = {v["design"]: v for v in ev["comparison"].get("values", [])}
    ratio = vals["C10"]["stiffness_n_per_mm"] / vals["C20"]["stiffness_n_per_mm"] if len(vals) == 2 else float("nan")
    ref_ratio = ref["C20"] / ref["C10"]
    out["stiffness_ratio"] = ratio
    out["reference_ratio"] = ref_ratio
    check(abs(ratio / ref_ratio - 1) <= 0.02, f"R1c: stiffness ratio {ratio:.4f} within 2% of {ref_ratio:.4f}")
    check(ev["comparison"]["outcome"] == "resolved" and ev["comparison"]["ranking"] == ["C10", "C20"],
          f"R1d: outcome resolved with C10 first (got {ev['comparison']['outcome']} {ev['comparison'].get('ranking')})")
    scal = [r for r in ev["numerical_evidence"] if r["check"] == "Young's modulus scaling"]
    check(len(scal) == 2 and all(r["outcome"] == "pass" for r in scal), "R1g: Young's modulus scaling verified for both designs")
    RESULTS["R1"] = out
    for n in ("C10", "C20"):
        print(f"  {n}: {out[n]['values_mm']} mm against {ref[n]:.7f} mm (finest {100 * out[n]['relative_error']:+.3f}%)")


def bracket_definition(name="bracket_ab", **kw):
    return definition(name, [design("A", "bracket_a_plain.stl"), design("B", "bracket_b_chamfer.stl")],
                      sensitivity={"youngs_modulus_relative": [0.9, 1.1], "poisson_ratio": [0.3, 0.36]}, **kw)


def case_E1_R2_R3(c, work):
    print("== E1 / R3 valid comparison of brackets A and B")
    defn = bracket_definition()
    ok, v, images = call(c, "study_check", {"definition": defn})
    check(ok and v["status"] == "ready", f"E1: check ready (got {v.get('status') if ok else v})")
    check(ok and not [q for q in v["questions"] if q["blocking"]], "E1: no blocking question")
    check(ok and v["equivalence"]["status"] == "equivalent", "E1: mounting and load equivalent between designs")
    check(len(images) == 2, f"E1: one region preview per design ({len(images)})")
    t0 = time.time()
    st, ev, jid = run_study(c, defn, work / "ws" / "bracket_ab")
    wall = time.time() - t0
    if not check(ev is not None, "E1: evidence record produced"):
        print("   ", json.dumps(st)[:800])
        return
    cmpn = ev["comparison"]
    check(ev["status"] == "complete", f"E1/R3: status complete ({ev['status']})")
    check(all(lv.get("valid") for d in ev["designs"] for lv in d["levels"]), "E1/R3: every level valid")
    # revised in rc2 (release/evaluation/CASES.md, amendment 1): B's staircase chamfer has no defensible discretisation-error estimate
    check(cmpn["outcome"] == "ranking_consistent_on_tested_meshes" and cmpn["ranking"] == ["B", "A"],
          f"E1: outcome ranking_consistent_on_tested_meshes, B first ({cmpn['outcome']} {cmpn.get('ranking')})")
    vals = {v["design"]: v for v in cmpn["values"]}
    check(cmpn.get("ranking_consistent_on_tested_meshes") is True and cmpn.get("meshes_compared") == 3, "R3: ranking the same on the 3 tested meshes")
    check(all(isinstance(v.get("convergence_criterion_met"), bool) and isinstance(v.get("estimate_available"), bool) for v in cmpn["values"]),
          "R3: convergence criterion and estimate availability reported per design")
    check(vals.get("A", {}).get("estimate_available") and vals["A"].get("discretisation_error_estimate", 0) > 0 and vals["A"].get("safety_factor") == 3,
          "R3: design A (exact geometry) has a grid convergence index with safety factor 3")
    check(vals.get("B", {}).get("estimate_available") is False and vals["B"].get("estimate_unavailable_reasons"), "R3: design B has no estimate, with the reason")
    check(vals.get("B", {}).get("convergence_criterion_met") is False and "not met by design B" in cmpn["statement"], "R3: B's failed 2% criterion stays in the statement")
    check("\u00b1" not in cmpn["statement"] and "uncertainty" not in cmpn["statement"], "R3: no ± or uncertainty wording in the statement")
    check(any("strength" in n["item"] for n in ev["not_evaluated"]), "E1: strength listed as not evaluated")
    directory = Path(ev["study"]["directory"])
    for f in ("study.json", "request.json", "evidence.json", "report.md", "operations.jsonl", "previews/A-regions.png"):
        check((directory / f).exists(), f"E1: {f} written")
    RESULTS["E1"] = {"wall_seconds": wall, "comparison": cmpn, "job_id": jid}
    print(f"  {cmpn['statement']} ({wall:.1f} s)")

    print("== R2 replay of the bracket study")
    st2, ev2, _ = run_study(c, None, None, tool="study_replay", args={"directory": str(directory)})
    if not check(ev2 is not None and "replay" in ev2 or (ev2 is not None and ev2.get("status")), "R2: replay produced a record"):
        print("   ", json.dumps(st2)[:800])
        return
    ok, rv, _ = call(c, "study_evidence", {"directory": ev2["study"]["directory"]})
    rep = rv.get("replay", {}) if ok else {}
    check(ev2["study"]["study_hash"] == ev["study"]["study_hash"], "R2: replay resolves to the same study hash")
    check(rep.get("outcome") == "reproduced", f"R2: outcome reproduced ({rep.get('outcome')}, largest difference {rep.get('largest_relative_difference')})")
    check(rep.get("compared", 0) > 0 and rep.get("bitwise_identical") == rep.get("compared"),
          f"R2: every quantity bitwise identical ({rep.get('bitwise_identical')} of {rep.get('compared')})")
    RESULTS["R2"] = {k: rep.get(k) for k in ("outcome", "compared", "bitwise_identical", "largest_relative_difference")}


# ------------------------------------------------------------------------------------------------ evaluation cases

def case_E2(c, work):
    print("== E2 missing geometry units")
    defn = definition("e2", [design("A", "bracket_a_plain.stl"), design("B", "bracket_b_chamfer.stl", units=None)])
    ok, v, _ = call(c, "study_check", {"definition": defn})
    qs = questions(v) if ok else {}
    check(ok and v["status"] == "needs_input", f"E2: needs_input ({v.get('status') if ok else v})")
    check("B.units" in qs and qs["B.units"]["blocking"] and not qs["B.units"]["acceptable"], "E2: blocking, non-acceptable question B.units")
    check(ok and any(w["code"] == "UNIT_CANDIDATES" for w in v["warnings"]), "E2: sizes under candidate units reported")
    ok, err, _ = call(c, "study_run", {"definition": defn, "directory": str(work / "ws" / "e2")})
    ids = [q["id"] for q in (err or {}).get("details", {}).get("questions", [])] if not ok else []
    check(not ok and err["code"] == "PRECONDITION_FAILED" and "B.units" in ids, "E2: study_run refused with the question in details")
    RESULTS["E2"] = {"status": v.get("status") if isinstance(v, dict) else None, "questions": list(qs)}


def case_E3(c, work):
    print("== E3 missing material")
    ok, v, _ = call(c, "study_check", {"definition": bracket_definition("e3", material=None)})
    qs = questions(v) if ok else {}
    check(ok and v["status"] == "needs_input" and "study.material" in qs and qs["study.material"]["blocking"], f"E3: blocking question study.material ({list(qs)})")
    RESULTS["E3"] = {"questions": list(qs)}


def case_E4(c, work):
    print("== E4 ambiguous mounting region")
    down = {"query": {"facing": {"direction": "-z", "max_angle_deg": 10}}, "description": "the face that sits on the support", "source": "inferred"}
    defn = definition("e4", [design("A", "bracket_a_plain.stl", mounting=down), design("B", "bracket_b_chamfer.stl", mounting=down)])
    ok, v, _ = call(c, "study_check", {"definition": defn})
    qs = questions(v) if ok else {}
    amb = [k for k in qs if k.endswith("mounting_region.ambiguous")]
    check(ok and v["status"] == "needs_input" and amb and all(qs[k]["blocking"] and qs[k]["acceptable"] for k in amb),
          f"E4: acceptable blocking ambiguity question ({list(qs)})")
    RESULTS["E4"] = {"questions": list(qs)}


def case_E5(c, work):
    print("== E5 underconstrained component")
    defn = bracket_definition("e5", mounting={"idealization": "frictionless_normal", "description": "back face resting against the wall", "source": "user"})
    ok, v, _ = call(c, "study_check", {"definition": defn})
    qs = questions(v) if ok else {}
    uc = [k for k in qs if k.endswith("mounting_underconstrained")]
    check(ok and v["status"] == "needs_input" and uc and all(qs[k]["blocking"] and not qs[k]["acceptable"] for k in uc),
          f"E5: blocking, non-acceptable underconstrained question ({list(qs)})")
    check(uc and "rigid" in json.dumps(qs[uc[0]]).lower() or uc and "free" in json.dumps(qs[uc[0]]).lower(), "E5: the question names the free motion")
    RESULTS["E5"] = {"questions": list(qs)}


def case_E6(c, work):
    print("== E6 unresolved thin feature")
    defn = definition("e6", [design("A", "bracket_a_plain.stl"), design("T", "bracket_thin.stl")], sizes=("8 mm", "6 mm"))
    ok, v, _ = call(c, "study_check", {"definition": defn})
    check(ok and any(w["code"] == "THIN_FEATURE_MAY_VANISH" and "'T'" in w["message"] for w in v["warnings"]), "E6: THIN_FEATURE_MAY_VANISH for design T")
    if not ok or v["status"] != "ready":
        RESULTS["E6"] = {"status": v.get("status") if ok else None, "questions": list(questions(v)) if ok else []}
        check(ok, "E6: check completed")
        return
    st, ev, _ = run_study(c, defn, work / "ws" / "e6")
    if not check(ev is not None, "E6: record produced"):
        return
    bad = [(d["name"], lv["element_size_mm"]) for d in ev["designs"] for lv in d["levels"]
           if lv.get("completed") and lv.get("valid") and (lv["mesh"]["face_connected_regions"] != 1 or abs(lv["mesh"].get("volume_error_percent", 0)) > 10)]
    check(not bad, f"E6: no split or badly represented mesh marked valid ({bad})")
    check(ev["comparison"]["outcome"] != "resolved" or all(lv.get("valid") for d in ev["designs"] for lv in d["levels"]),
          f"E6: no resolved outcome from invalid levels ({ev['comparison']['outcome']})")
    RESULTS["E6"] = {"outcome": ev["comparison"]["outcome"], "statement": ev["comparison"]["statement"]}
    print(f"  {ev['comparison']['statement'][:300]}")


def case_E7(c, work):
    print("== E7 unsupported large deformation")
    defn = definition("e7", [design("A", "bracket_a_plain.stl"), design("B", "bracket_b_chamfer.stl")], material=SOFT,
                      load={"kind": "payload_mass", "mass": "60 kg", "direction": "-z", "source": "user"})
    st, ev, _ = run_study(c, defn, work / "ws" / "e7")
    if not check(ev is not None, f"E7: record produced ({json.dumps(st)[:300] if ev is None else ''})"):
        return
    cls = {d["name"]: [lv["checks"]["deformation_classification"] for lv in d["levels"] if lv.get("completed")] for d in ev["designs"]}
    check(any("outside_small_deformation_assumption" in v for v in cls.values()), f"E7: deformation outside the assumption detected ({cls})")
    check(ev["comparison"]["outcome"] == "cannot_establish", f"E7: outcome cannot_establish ({ev['comparison']['outcome']})")
    check("ranking" not in ev["comparison"], "E7: no ranking reported")
    RESULTS["E7"] = {"classification": cls, "statement": ev["comparison"]["statement"]}


def case_E8(c, work):
    print("== E8 printed part without manufacturing information")
    defn = bracket_definition("e8", material=SOFT, manufacturing={"process": "fff", "source": "user"})
    ok, v, _ = call(c, "study_check", {"definition": defn})
    qs = questions(v) if ok else {}
    q = qs.get("manufacturing.printed_properties")
    check(ok and v["status"] == "needs_input" and q and q["blocking"] and q["acceptable"], f"E8: acceptable blocking printed-properties question ({list(qs)})")
    items = " ".join(n["item"] for n in v.get("not_evaluated", [])) if ok else ""
    check("interlayer" in items and "anisotropic" in items, "E8: interlayer failure and printed anisotropy listed as not evaluated")
    RESULTS["E8"] = {"questions": list(qs)}


def case_E9(c, work):
    print("== E9 inconclusive numerical difference")
    defn = definition("e9", [design("A", "bracket_a_plain.stl"), design("A_tip", "bracket_a_tip_chamfer.stl")])
    st, ev, _ = run_study(c, defn, work / "ws" / "e9")
    if not check(ev is not None, "E9: record produced"):
        return
    oc = ev["comparison"]["outcome"]
    check(oc in ("too_small_to_distinguish", "not_resolved", "more_refinement_required"), f"E9: difference not resolved ({oc})")
    RESULTS["E9"] = {"outcome": oc, "statement": ev["comparison"]["statement"], "relative_difference": ev["comparison"].get("relative_difference"),
                     "combined_discretisation_error_estimate": ev["comparison"].get("combined_discretisation_error_estimate"),
                     "sum_of_last_changes": ev["comparison"].get("sum_of_last_changes")}
    print(f"  {ev['comparison']['statement'][:300]}")


def case_E10(c, work):
    print("== E10 stress plot presented as proof of safety")
    defn = bracket_definition("e10", question="The stress plot of bracket A peaks at 6 MPa, far below the 276 MPa yield of 6061-T6, so the bracket is safe. Confirm it is safe for the 3 kg load.",
                              sizes=("4 mm", "2 mm"))
    defn.pop("sensitivity", None)
    ok, v, _ = call(c, "study_check", {"definition": defn})
    check(ok and any(u["id"] == "strength_or_safety" for u in v["unsupported"]), "E10: unsupported request strength_or_safety reported")
    st, ev, _ = run_study(c, defn, work / "ws" / "e10")
    if not check(ev is not None, "E10: record produced"):
        return
    check(any("No strength margin or safety statement is made" in s for s in ev["interpretation"]), "E10: the record states that no safety statement is made")
    text = json.dumps({k: ev[k] for k in ("comparison", "interpretation")}).lower()
    check(" is safe" not in text and "safe for" not in text, "E10: no safety claim in the comparison or interpretation")
    RESULTS["E10"] = {"unsupported": [u["id"] for u in v["unsupported"]] if ok else []}


def small_cantilevers(work, name, directory):
    box_stl(work / "k10.stl", 100, 10, 10)
    box_stl(work / "k20.stl", 100, 10, 5)
    root = {"query": {"plane": {"axis": "x", "at": "min"}}, "description": "root face", "source": "user"}
    tip = {"query": {"plane": {"axis": "x", "at": "max"}}, "description": "tip face", "source": "user"}
    return definition(name, [design("K10", "k10.stl", mounting=root, load=tip, geom=work), design("K20", "k20.stl", mounting=root, load=tip, geom=work)],
                      question="Which cantilever deflects less?", material=STEEL, mounting={"idealization": "fixed", "description": "clamped root", "source": "user"},
                      load={"kind": "force", "force": "10 N", "direction": "-z", "source": "user"}, sizes=("5 mm", "2.5 mm"),
                      accept=[{"id": "equivalence", "reason": "the cross-section is the difference being compared"}])


def case_E11(c, work):
    print("== E11 the evidence record cannot be written")
    target = work / "ws" / "e11"
    (target / "evidence.json").mkdir(parents=True)  # a folder where the record must go: the atomic rename fails
    st, ev, jid = run_study(c, small_cantilevers(work, "e11", target), target)
    err = (st or {}).get("error") or {}
    check(st and st.get("state") == "failed" and err.get("code") == "IO_ERROR", f"E11: job failed with IO_ERROR ({(st or {}).get('state')} {err.get('code')})")
    check("evidence.json" in err.get("message", "") and "study_replay" in err.get("message", ""), f"E11: message names the file and the recovery ({err.get('message', '')[:200]})")
    details = err.get("details") or {}
    check(details.get("report_write") == "written" and details.get("comparison", {}).get("outcome"), "E11: the report and the comparison survive in the job")
    check((target / "report.md").exists(), "E11: report.md written")
    RESULTS["E11"] = {"state": (st or {}).get("state"), "code": err.get("code"), "message": err.get("message")}


def case_E12(c, work):
    print("== E12 the operations log cannot be opened")
    target = work / "ws" / "e12"
    (target / "operations.jsonl").mkdir(parents=True)
    st, ev, _ = run_study(c, small_cantilevers(work, "e12", target), target)
    err = (st or {}).get("error") or {}
    check(st and st.get("state") == "failed" and err.get("code") == "IO_ERROR", f"E12: job failed with IO_ERROR ({(st or {}).get('state')} {err.get('code')})")
    check("operations log" in err.get("message", ""), f"E12: message names the operations log ({err.get('message', '')[:200]})")
    check(ev is not None and ev["status"] == "failed" and any(f["code"] == "IO_ERROR" and f["stage"] == "setup" for f in ev["failures"]),
          "E12: the record states the failure; no analysis ran")
    RESULTS["E12"] = {"state": (st or {}).get("state"), "code": err.get("code")}


def case_E13(c, work):
    print("== E13 access-root limit")
    mcp = Path(os.environ["NAVIER_BIN"]) / "navier-mcp" if os.environ.get("NAVIER_BIN") else ROOT / "navier-mcp"
    inside = sum((["--allow-read", str(Path.home() / f"navier-e13-{k}")] for k in range(24)), [])
    r = subprocess.run([str(mcp)] + inside + ["--version"], capture_output=True, text=True, timeout=30)
    check(r.returncode == 0, f"E13: 24 folders inside the home folder take no root slot (exit {r.returncode}: {r.stderr.strip()[:200]})")
    outside = sum((["--allow-read", f"/usr/navier-e13-{k}"] for k in range(24)), [])
    r = subprocess.run([str(mcp)] + outside + ["--version"], capture_output=True, text=True, timeout=30)
    check(r.returncode == 2 and "limit of 16 read roots" in r.stderr and "common parent folder" in r.stderr,
          f"E13: the limit is named when exceeded (exit {r.returncode}: {r.stderr.strip()[:300]})")
    RESULTS["E13"] = {"stderr": r.stderr.strip()[:400]}


CASES = {"R1": case_R1, "E1": case_E1_R2_R3, "E2": case_E2, "E3": case_E3, "E4": case_E4, "E5": case_E5, "E6": case_E6, "E7": case_E7, "E8": case_E8,
         "E9": case_E9, "E10": case_E10, "E11": case_E11, "E12": case_E12, "E13": case_E13}


def main():
    only = None
    results_path = None
    args = sys.argv[1:]
    if "--only" in args:
        only = set(args[args.index("--only") + 1].split(","))
    if "--results" in args:
        results_path = Path(args[args.index("--results") + 1])
    with tempfile.TemporaryDirectory(prefix="studyflow-") as tmp:
        work = Path(tmp).resolve()
        (work / "ws").mkdir()
        c = Client(["--embedded", "--workspace", str(work / "ws"), "--allow-read", str(GEOM), "--allow-read", str(work), "--allow-write", str(work)])
        c.initialize()
        for name, fn in CASES.items():
            if only and name not in only and not (name == "E1" and only & {"R2", "R3"}):
                continue
            t0 = time.time()
            fn(c, work)
            print(f"  ({time.time() - t0:.1f} s)")
        c.close()
    if results_path:
        results_path.write_text(json.dumps(RESULTS, indent=1, default=str))
    print(f"\n{'ALL STUDY FLOW TESTS PASSED' if FAIL == 0 else 'STUDY FLOW TESTS FAILED'}: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
