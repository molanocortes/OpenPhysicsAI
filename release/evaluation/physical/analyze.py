#!/usr/bin/env python3
"""analyze.py - the analysis of PROTOCOL.md section 7, fixed before any measurement

    python3 analyze.py MEASUREMENTS.csv PREDICTIONS.json [--out result.json]
    python3 analyze.py --self-test

Reads the filled measurement template (record types specimen, reading, coupon) and predict.py's predictions.json.
Computes per cycle the compliance (least squares through the preload point), the linearity and return-to-zero checks,
per design the mean compliance and its uncertainty budget, the ratio B/A, the coupon modulus, and the agreement numbers
E_n. With no reading rows it reports nothing and exits 3: validation stays pending until real measurements exist.
--self-test runs the arithmetic on numbers generated in memory, labelled synthetic, and writes no file.
"""
import csv
import json
import math
import statistics
import sys
from pathlib import Path

G = 9.80665
RESOLUTION_MM, CALIBRATION_REL, POSITION_REL, TEMPERATURE_REL = 0.001, 0.003, 0.004, 0.0035
NOMINAL_PLATE_MM, THICKNESS_FLAG_MM = 8.0, 0.15
LINEARITY_LIMIT, RETURN_LIMIT, T_RANGE = 0.03, 0.02, (19.0, 23.0)


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def read_rows(path):
    lines = [ln for ln in Path(path).read_text().splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
    return list(csv.DictReader(lines))


def cycle_result(rows):
    loads = [num(r["load_kg"]) for r in rows]
    lo = min(loads)
    zero = rows[loads.index(lo)]
    back = [r for r in rows if num(r["load_kg"]) == lo and r is not zero]
    pz, bz = num(zero["pad_indicator_mm"]), num(zero["base_indicator_mm"]) or 0.0
    steps = [(num(r["load_kg"]) - lo, (num(r["pad_indicator_mm"]) - pz) - ((num(r["base_indicator_mm"]) or 0.0) - bz)) for r in rows if num(r["load_kg"]) > lo]
    temps = [num(r["temperature_c"]) for r in rows if num(r["temperature_c"]) is not None]
    res = {"steps": [{"load_above_preload_kg": dl, "net_displacement_mm": d} for dl, d in steps], "excluded_reasons": []}
    if len(steps) < 2:
        res["excluded_reasons"].append("fewer than two load steps")
        return res
    f = [dl * G for dl, _ in steps]
    d = [x for _, x in steps]
    slope = sum(fi * di for fi, di in zip(f, d)) / sum(fi * fi for fi in f)
    dmax = max(abs(x) for x in d)
    lin = max(abs(di - slope * fi) for fi, di in zip(f, d)) / dmax if dmax > 0 else float("inf")
    res.update({"compliance_mm_per_n": slope, "linearity_deviation": lin})
    if lin > LINEARITY_LIMIT:
        res["excluded_reasons"].append(f"linearity deviation {100 * lin:.1f} % > {100 * LINEARITY_LIMIT:.0f} %")
    if back:
        r = back[-1]
        ret = abs((num(r["pad_indicator_mm"]) - pz) - ((num(r["base_indicator_mm"]) or 0.0) - bz)) / dmax
        res["return_to_zero"] = ret
        if ret > RETURN_LIMIT:
            res["excluded_reasons"].append(f"return to zero {100 * ret:.1f} % > {100 * RETURN_LIMIT:.0f} %")
    else:
        res["excluded_reasons"].append("no unloaded reading after the cycle")
    if temps and not all(T_RANGE[0] <= t <= T_RANGE[1] for t in temps):
        res["excluded_reasons"].append(f"temperature outside {T_RANGE[0]}-{T_RANGE[1]} degC")
    return res


def analyze(rows, pred):
    specimens = {r["specimen_id"]: r for r in rows if r["record_type"] == "specimen"}
    readings = [r for r in rows if r["record_type"] == "reading"]
    coupons = [r for r in rows if r["record_type"] == "coupon"]
    if not readings:
        return None
    out = {"nature": "analysis of recorded measurements (PROTOCOL.md 7)", "specimens": {}, "designs": {}, "flags": []}
    by_cycle = {}
    for r in readings:
        by_cycle.setdefault((r["specimen_id"], r["cycle"]), []).append(r)
    per_spec = {}
    for (sid, cyc), rs in by_cycle.items():
        cr = cycle_result(rs)
        s = out["specimens"].setdefault(sid, {"design": specimens.get(sid, rs[0])["design"], "cycles": {}})
        s["cycles"][cyc] = cr
        if not cr["excluded_reasons"]:
            per_spec.setdefault(sid, []).append(cr["compliance_mm_per_n"])
    for sid, s in out["specimens"].items():
        failed = [c for c, v in s["cycles"].items() if v["excluded_reasons"]]
        sp = specimens.get(sid, {})
        th = [num(sp.get(k)) for k in ("wall_thickness_mm_1", "wall_thickness_mm_2", "wall_thickness_mm_3", "arm_thickness_mm_1", "arm_thickness_mm_2", "arm_thickness_mm_3")]
        th = [t for t in th if t is not None]
        s["mean_plate_thickness_mm"] = statistics.mean(th) if th else None
        s["thickness_sd_mm"] = statistics.stdev(th) if len(th) > 1 else None
        if th and abs(s["mean_plate_thickness_mm"] - NOMINAL_PLATE_MM) > THICKNESS_FLAG_MM:
            out["flags"].append(f"{sid}: mean plate thickness {s['mean_plate_thickness_mm']:.2f} mm deviates by more than {THICKNESS_FLAG_MM} mm")
        if len(failed) >= 2:
            s["excluded"] = f"{len(failed)} cycles failed the linearity, return or temperature checks"
            per_spec.pop(sid, None)
        s["mean_compliance_mm_per_n"] = statistics.mean(per_spec[sid]) if sid in per_spec else None
    for design in sorted({s["design"] for s in out["specimens"].values()}):
        means = [s["mean_compliance_mm_per_n"] for sid, s in out["specimens"].items() if s["design"] == design and s["mean_compliance_mm_per_n"] is not None]
        if not means:
            out["designs"][design] = {"valid_specimens": 0}
            continue
        c = statistics.mean(means)
        n = len(means)
        type_a = (statistics.stdev(means) / math.sqrt(n) / c) if n > 1 else None
        full_disp = c * (3.0 - 0.2) * G
        res_rel = math.sqrt(4) * RESOLUTION_MM / math.sqrt(12) / full_disp
        sds = [s["thickness_sd_mm"] for s in out["specimens"].values() if s["design"] == design and s.get("thickness_sd_mm") is not None]
        thick_rel = 3 * statistics.mean(sds) / NOMINAL_PLATE_MM if sds else 0.0
        budget = {"type_a": type_a, "resolution": res_rel, "calibration": CALIBRATION_REL, "load_position": POSITION_REL, "temperature": TEMPERATURE_REL, "thickness": thick_rel}
        u_rel = math.sqrt(sum(v ** 2 for v in budget.values() if v is not None))
        out["designs"][design] = {"valid_specimens": n, "compliance_mm_per_n": c, "relative_standard_uncertainty": u_rel, "budget_relative": budget,
                                  "type_a_note": None if type_a is not None else "one specimen: no Type A estimate; the result is flagged"}
        if type_a is None:
            out["flags"].append(f"design {design}: only one valid specimen")
    if coupons:
        es = []
        for r in coupons:
            L, b, h, m = (num(r[k]) for k in ("coupon_span_mm", "coupon_width_mm", "coupon_thickness_mm", "coupon_slope_n_per_mm"))
            if None not in (L, b, h, m):
                es.append(L ** 3 * m / (4 * b * h ** 3))
        if es:
            out["coupon_modulus_mpa"] = {"values": es, "mean": statistics.mean(es),
                                         "relative_standard_uncertainty": (statistics.stdev(es) / math.sqrt(len(es)) / statistics.mean(es)) if len(es) > 1 else None}
    A, B = out["designs"].get("A", {}), out["designs"].get("B", {})
    if A.get("valid_specimens") and B.get("valid_specimens"):
        r_meas = B["compliance_mm_per_n"] / A["compliance_mm_per_n"]
        u_r = r_meas * math.sqrt(A["relative_standard_uncertainty"] ** 2 + B["relative_standard_uncertainty"] ** 2)
        r_pred = pred["ratio_B_over_A"]
        U_pred = pred["prediction_allowance"]["expanded_k2_relative"] * r_pred
        en = abs(r_meas - r_pred) / math.sqrt((2 * u_r) ** 2 + U_pred ** 2)
        out["primary_ratio"] = {"measured_B_over_A": r_meas, "expanded_uncertainty_k2": 2 * u_r, "predicted": r_pred, "prediction_allowance_k2": U_pred,
                                "E_n": en, "reading": "consistent" if en <= 1 else "inconsistent"}
    if "coupon_modulus_mpa" in out:
        e_c = out["coupon_modulus_mpa"]["mean"]
        e_s = pred["planning_material"]["youngs_modulus_pa"] / 1e6
        u_e = out["coupon_modulus_mpa"]["relative_standard_uncertainty"] or 0.0
        sec = {}
        for name, dm in out["designs"].items():
            if not dm.get("valid_specimens"):
                continue
            c_pred = pred["designs"][name]["compliance_mm_per_n"] * e_s / e_c
            u = dm["relative_standard_uncertainty"]
            allowance = pred["prediction_allowance"]["relative_standard"]
            en = abs(dm["compliance_mm_per_n"] - c_pred) / (2 * c_pred * math.sqrt(u ** 2 + u_e ** 2 + allowance ** 2))
            sec[name] = {"measured": dm["compliance_mm_per_n"], "predicted_with_coupon_modulus": c_pred, "E_n": en,
                         "reading": "consistent" if en <= 1 else "inconsistent",
                         "model_difference": "printed parts are anisotropic; the model is isotropic"}
        out["secondary_absolute"] = sec
    return out


def self_test():
    """synthetic, in memory only: known compliances must be recovered and the checks must trigger"""
    print("SELF-TEST WITH SYNTHETIC NUMBERS (not measurements; nothing is written)")
    cA, cB = 0.0017, 0.00058  # mm/N
    rows = []
    for sid, design, c, t in (("A1", "A", cA, 8.02), ("A2", "A", cA * 1.01, 7.98), ("B1", "B", cB, 8.01), ("B2", "B", cB * 0.99, 8.00)):
        rows.append({"record_type": "specimen", "specimen_id": sid, "design": design, **{f"wall_thickness_mm_{k}": str(t) for k in (1, 2, 3)},
                     **{f"arm_thickness_mm_{k}": str(t) for k in (1, 2, 3)}})
        for cyc in ("1", "2", "3"):
            for load in (0.2, 1.0, 2.0, 3.0, 0.2):
                disp = c * (load - 0.2) * G
                rows.append({"record_type": "reading", "specimen_id": sid, "design": design, "cycle": cyc, "load_kg": str(load), "temperature_c": "21.0",
                             "pad_indicator_mm": str(1.0 + disp), "base_indicator_mm": str(0.5 + 0.01 * disp)})
    pred = {"ratio_B_over_A": cB / cA, "prediction_allowance": {"expanded_k2_relative": 0.05, "relative_standard": 0.025},
            "planning_material": {"youngs_modulus_pa": 3.5e9}, "designs": {"A": {"compliance_mm_per_n": cA / 0.99}, "B": {"compliance_mm_per_n": cB / 0.99}}}
    for row in rows:
        for k in ("hold_s", "coupon_span_mm"):
            row.setdefault(k, "")
    res = analyze(rows, pred)
    ok = True
    ca = res["designs"]["A"]["compliance_mm_per_n"]
    expect_a = statistics.mean([cA * 0.99, cA * 1.01 * 0.99])  # base indicator moves 1 % of the pad displacement
    ok &= abs(ca / expect_a - 1) < 1e-9
    ok &= res["primary_ratio"]["reading"] == "consistent"
    bad = [dict(r) for r in rows if r["specimen_id"] == "B2"]
    for r in bad:
        if r["record_type"] == "reading" and r["load_kg"] == "2.0":
            r["pad_indicator_mm"] = str(float(r["pad_indicator_mm"]) * 1.02)
    res2 = analyze([r for r in rows if r["specimen_id"] != "B2"] + bad, pred)
    ok &= res2["specimens"]["B2"].get("excluded") is not None and res2["designs"]["B"]["valid_specimens"] == 1
    print("self-test", "passed" if ok else "FAILED")
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv:
        return self_test()
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    rows = read_rows(sys.argv[1])
    pred = json.loads(Path(sys.argv[2]).read_text())
    res = analyze(rows, pred)
    if res is None:
        print(f"no measurements in {sys.argv[1]}: validation pending; nothing is reported")
        return 3
    text = json.dumps(res, indent=1)
    if "--out" in sys.argv:
        Path(sys.argv[sys.argv.index("--out") + 1]).write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
