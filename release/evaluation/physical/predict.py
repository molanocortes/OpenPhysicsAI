#!/usr/bin/env python3
"""predict.py - NAVIER predictions for the physical comparison protocol (PROTOCOL.md), before any test

    python3 predict.py --navier-ctl PATH [--example DIR] [--modulus-gpa 3.5] [--poisson 0.35] [--density 1240]
                       [--tensile-strength-mpa 50] [--sizes 4,2,1] [--work DIR] [--out predictions.json]

Writes a study definition for the test set-up and runs it with navier-ctl:
- the example brackets A and B;
- the back face x = 0 and the clamped front strip (x = 8 mm, z 0-24 mm) held fixed;
- 3 kg on the 16 x 16 mm pad;
- the planning material values given.

It then writes the predicted displacement at every load step, the compliance, the ratio B/A, the prediction allowance
of PROTOCOL.md section 7.5, and the stress planning indicator of section 6. Predictions are not measurements. The
modulus given here is a planning value: analyze.py rescales the absolute predictions with the modulus measured on
coupons, and the ratio does not depend on it.
"""
import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

G = 9.80665
PRELOAD_KG, STEPS_KG, MAX_KG = 0.2, [1.0, 2.0, 3.0], 3.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--navier-ctl", required=True)
    ap.add_argument("--example", default=str(Path(__file__).resolve().parents[3] / "examples/bracket_comparison"))
    ap.add_argument("--modulus-gpa", type=float, default=3.5)
    ap.add_argument("--poisson", type=float, default=0.35)
    ap.add_argument("--density", type=float, default=1240.0)
    ap.add_argument("--tensile-strength-mpa", type=float, default=50.0)
    ap.add_argument("--sizes", default="4,2,1")
    ap.add_argument("--work", default=None)
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent / "predictions.json"))
    a = ap.parse_args()
    example = Path(a.example).resolve()
    work = Path(a.work).resolve() if a.work else Path(__file__).resolve().parent / "prediction_run"
    work.mkdir(parents=True, exist_ok=True)
    base = json.loads((example / "study.json").read_text())
    mount = {"query": {"any": [{"all": [{"plane": {"axis": "x", "at": "min"}}, {"facing": {"direction": "-x", "max_angle_deg": 5}}]},
                               {"all": [{"box": {"min": [7.5, -0.5, -0.5], "max": [8.5, 40.5, 24.5]}}, {"facing": {"direction": "+x", "max_angle_deg": 5}}]}]},
             "mode": "triangle", "description": "back face against the steel base and the front strip under the clamp plate (x = 8 mm, z 0-24 mm)",
             "source": "user"}
    for d in base["designs"]:
        d["geometry"]["path"] = str(example / d["geometry"]["path"])
        d["mounting_region"] = mount
    base["name"] = "physical_protocol_prediction"
    base["question"] = "Predicted load-pad displacement of brackets A and B in the physical test set-up (PROTOCOL.md), for test planning and comparison."
    base["material"] = {"record": {"id": "printed_planning_values", "name": "printed polymer, planning values", "family": "polymer", "status": "user_supplied",
                                   "provenance": f"planning values for the protocol: E {a.modulus_gpa} GPa, nu {a.poisson}, density {a.density} kg/m3; replaced by the coupon modulus in analyze.py",
                                   "youngs_modulus_pa": {"value": a.modulus_gpa * 1e9}, "poisson_ratio": {"value": a.poisson}, "density_kg_m3": {"value": a.density}},
                        "source": "user", "reference": "PROTOCOL.md planning values"}
    base["manufacturing"] = {"process": "unspecified", "source": "user",
                             "notes": "printed specimens are modelled as homogeneous and isotropic; PROTOCOL.md reports this model difference"}
    base["mounting"] = {"idealization": "fixed", "description": "clamped between the steel base and the clamp plate (ideal rigid clamp)", "source": "user"}
    base["load"] = {"kind": "payload_mass", "mass": f"{MAX_KG} kg", "direction": "-z", "source": "user", "description": "largest load step of the protocol"}
    base["refinement"] = {"element_sizes": [f"{s} mm" for s in a.sizes.split(",")], "convergence_criterion": 0.02}
    base.pop("sensitivity", None)
    base["retain_results"] = "none"
    defn = work / "prediction_study.json"
    defn.write_text(json.dumps(base, indent=2))
    study = work / "study"
    if (study / "evidence.json").exists():
        print(f"using the existing prediction run in {study}")
    else:
        cmd = [a.navier_ctl, "--embedded", "--workspace", str(work / "ws"), "--allow-read", str(example), "--allow-read", str(work), "--allow-write", str(work),
               "study", "run", str(defn), "--dir", str(study)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        print(r.stdout.strip())
        if r.returncode != 0:
            print(r.stderr.strip(), file=sys.stderr)
            raise SystemExit(f"navier-ctl study run failed with exit {r.returncode}")
    ev = json.loads((study / "evidence.json").read_text())
    force = MAX_KG * G
    out = {"nature": "predictions of the NAVIER model for the physical protocol; not measurements", "navier": ev["reproduction"]["software"],
           "study_hash": ev["study"]["study_hash"], "outcome": ev["comparison"]["outcome"], "statement": ev["comparison"]["statement"],
           "planning_material": {"youngs_modulus_pa": a.modulus_gpa * 1e9, "poisson_ratio": a.poisson, "density_kg_m3": a.density},
           "load_at_prediction_kg": MAX_KG, "force_at_prediction_n": force, "preload_kg": PRELOAD_KG, "designs": {}}
    changes = {}
    for d in ev["designs"]:
        levels = [lv for lv in d["levels"] if lv.get("valid")]
        fine = levels[-1]
        u = fine["quantities"]["load_region_displacement_mm"]
        rf = d["refinement"]["load_region_displacement_mm"]
        changes[d["name"]] = rf.get("last_relative_change")
        out["designs"][d["name"]] = {
            "element_size_mm": fine["element_size_mm"], "displacement_at_3kg_mm": u, "compliance_mm_per_n": u / force,
            "mounting_area_mm2": next(x["regions"]["mounting"]["area_mm2"] for x in ev["modeled_conditions"]["designs"] if x["name"] == d["name"]),
            "last_relative_change": rf.get("last_relative_change"), "convergence_criterion_met": rf.get("convergence_criterion_met"),
            "estimate_available": rf.get("estimate_available"), "discretisation_error_estimate_mm": rf.get("discretisation_error_estimate"),
            "nodal_p99_von_mises_mpa_at_3kg": fine["stress"]["nodal_p99_mpa"], "peak_at_sharp_corner_or_support": fine["stress"]["peak_near_reentrant_corner"] or fine["stress"]["peak_at_support"],
            "net_displacement_at_steps_mm": {str(s): u * (s - PRELOAD_KG) / MAX_KG for s in STEPS_KG}}
    a_c, b_c = out["designs"]["A"]["compliance_mm_per_n"], out["designs"]["B"]["compliance_mm_per_n"]
    out["ratio_B_over_A"] = b_c / a_c
    allowance = math.sqrt(sum((c or 0) ** 2 for c in changes.values()))
    out["prediction_allowance"] = {"relative_standard": allowance, "expanded_k2_relative": 2 * allowance,
                                   "basis": "last relative changes between the two finest meshes of A and B combined in quadrature: an allowance, not an estimate (PROTOCOL.md 7.5)"}
    p99 = max(v["nodal_p99_von_mises_mpa_at_3kg"] for v in out["designs"].values())
    limit = 0.1 * a.tensile_strength_mpa
    out["non_destructive_planning_check"] = {"nodal_p99_von_mises_mpa_at_3kg": p99, "limit_mpa": limit, "tensile_strength_datasheet_mpa": a.tensile_strength_mpa,
                                             "proceed": p99 <= limit,
                                             "note": "planning indicator only (PROTOCOL.md 6); stresses do not depend on E for these supports and loads; peaks at the sharp corner are singular and not used"}
    Path(a.out).write_text(json.dumps(out, indent=1))
    print(json.dumps({k: out[k] for k in ("outcome", "ratio_B_over_A", "prediction_allowance", "non_destructive_planning_check")}, indent=1))
    for n, v in out["designs"].items():
        print(f"{n}: {v['displacement_at_3kg_mm']:.4f} mm at 3 kg (E {a.modulus_gpa} GPa), mounting area {v['mounting_area_mm2']:.0f} mm2, last change {100 * (v['last_relative_change'] or 0):+.2f} %")


if __name__ == "__main__":
    main()
