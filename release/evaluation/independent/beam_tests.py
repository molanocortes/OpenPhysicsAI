#!/usr/bin/env python3
"""beam_tests.py - applies the pre-registered reading of the beam-discrepancy tests (../CRITERIA-independent.md, section B)

    python3 beam_tests.py NAVIER_WORK_DIR X2_DIR OUT_JSON

Reads NAVIER's studies cantilevers (C10, C20, nu 0.3), t1_nu0 (C10, C20, nu 0) and t2_t3 (C10L 200x10x10, C10W 100x20x10)
and CalculiX's x2_summary.json; writes the table and the outcome of the reading fixed before the runs.
"""
import json
import sys
from pathlib import Path

E_MPA, F = 200e3, 10.0
DIMS = {"C10": (100, 10, 10), "C20": (100, 10, 5), "C10L": (200, 10, 10), "C10W": (100, 20, 10)}


def timoshenko(L, b, h, nu):
    g = E_MPA / (2 * (1 + nu))
    return F * L ** 3 / (3 * E_MPA * b * h ** 3 / 12) + F * L / (5 / 6 * g * b * h)


def main():
    work, x2, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    rows = {}
    for study, nu, label in (("cantilevers", 0.3, "baseline"), ("t1_nu0", 0.0, "T1"), ("t2_t3", 0.3, "T2/T3")):
        ev = json.loads((work / study / "evidence.json").read_text())
        for d in ev["designs"]:
            L, b, h = DIMS[d["name"]]
            ref = timoshenko(L, b, h, nu)
            levels = sorted(((lv["element_size_mm"], lv["quantities"]["load_region_displacement_mm"]) for lv in d["levels"] if lv.get("valid")), reverse=True)
            rows[f"{d['name']}_nu{nu}"] = {"test": label, "design": d["name"], "L_mm": L, "b_mm": b, "h_mm": h, "nu": nu, "timoshenko_mm": ref,
                                           "levels": [{"element_size_mm": s, "value_mm": u, "relative_to_timoshenko": u / ref - 1} for s, u in levels],
                                           "finest_relative_to_timoshenko": levels[-1][1] / ref - 1}
    xs = json.loads((x2 / "x2_summary.json").read_text())
    ccx = {c["case"]: c for c in xs["cases"]}
    c10 = rows["C10_nu0.3"]["finest_relative_to_timoshenko"]
    t1 = max(abs(rows["C10_nu0.0"]["finest_relative_to_timoshenko"]), abs(rows["C20_nu0.0"]["finest_relative_to_timoshenko"]))
    t2 = rows["C10L_nu0.3"]["finest_relative_to_timoshenko"]
    t3 = rows["C10W_nu0.3"]["finest_relative_to_timoshenko"]
    clamped = ccx["clamped_C3D20R_80x8x8"]["vs_navier_finest"]
    t4 = ccx["sliding_C3D20R_80x8x8"]["vs_timoshenko_5_6"]
    items = [
        {"item": "T1: |discrepancy| with nu = 0 below 0.1 %", "observed": t1, "met": abs(t1) < 0.001},
        {"item": "T2: L = 200 mm discrepancy about half of C10's, within +-0.15 percentage points", "observed": t2, "half_of_c10": c10 / 2,
         "met": abs(t2 - c10 / 2) <= 0.0015},
        {"item": "T3: b = 20 mm discrepancy larger than C10's", "observed": t3, "c10": c10, "met": abs(t3) > abs(c10)},
        {"item": "CalculiX clamped C3D20R within 0.2 % of NAVIER", "observed": clamped, "met": abs(clamped) <= 0.002},
        {"item": "T4: CalculiX sliding-root C3D20R within 0.2 % of Timoshenko", "observed": t4, "met": abs(t4) <= 0.002},
    ]
    if all(i["met"] for i in items):
        conclusion = "H1 supported, H2 and H3 not indicated"
    elif abs(clamped) > 0.005:
        conclusion = "H2 or H3 indicated: investigate NAVIER"
    else:
        conclusion = "cause unresolved"
    # the size of the stiffened root zone implied by H1 (delta/delta ~ -3 nu^2 c / L), for information only
    implied = {k: {"c_mm": -r["finest_relative_to_timoshenko"] * r["L_mm"] / (3 * 0.3 ** 2), "c_over_b": -r["finest_relative_to_timoshenko"] * r["L_mm"] / (3 * 0.3 ** 2) / r["b_mm"]}
               for k, r in rows.items() if r["nu"] == 0.3}
    result = {"navier": rows, "calculix": {k: ccx[k] for k in ccx}, "reading": items, "conclusion": conclusion,
              "implied_root_zone_if_h1": implied,
              "notes": ["Timoshenko reference with shear coefficient 5/6, as in rc1 case R1.",
                        "T4's support restrains u_z along the root line z = h/2, which carries the shear reaction as a line load; its value rose by "
                        "+0.025 % and +0.016 % per mesh halving (20x2x2 to 80x8x8).",
                        "The supplementary minimal-support case s2 carries the whole shear reaction at one node (a point support) and is not "
                        "informative about the Poisson effect; it is not used."]}
    out.write_text(json.dumps(result, indent=1))
    for i in items:
        print(f"{'met ' if i['met'] else 'NOT '} {i['item']}: {100 * i['observed']:+.4f} %")
    print("conclusion:", conclusion)
    for k, v in implied.items():
        print(f"  {k}: implied root zone c = {v['c_mm']:.2f} mm = {v['c_over_b']:.3f} b")


if __name__ == "__main__":
    main()
