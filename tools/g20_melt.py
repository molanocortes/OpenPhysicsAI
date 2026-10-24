#!/usr/bin/env python3
"""g20_melt.py - GOALS.md G20 step 2: NIST AM-Bench single tracks predicted blind by the lab's melt solver.

The rules are in GOALS.md (written before any run): conduction, sourced IN625 properties, the absorptivity fixed on
the calibration tracks in conduction mode (measured depth at most half the measured width), the cross-section read by
melt_track_section (melttest M11), the cell size from a refinement study on the calibration track. The set-ups and
calibration outcomes are read from ~/.openphysicsai/g20/metal/; the test outcomes stay sealed until the predictions
are frozen and their SHA-256 recorded in GOALS.md.

    make build/g20melt
    python3 tools/g20_melt.py study                 # refinement on the calibration track (detached: minutes per run)
    python3 tools/g20_melt.py calibrate --cell UM   # the absorptivity on the conduction-mode calibration tracks
    python3 tools/g20_melt.py predict --cell UM --absorptivity A [--alloys IN625,IN718]   # the test tracks, frozen with their SHA-256
    python3 tools/g20_melt.py score PREDICTIONS.json SHA256        # only after the hash is recorded in GOALS.md
"""
import hashlib
import json
import math
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
G20 = Path.home() / ".openphysicsai" / "g20" / "metal"
SEALED = Path.home() / ".openphysicsai" / "sealed" / "g20-metal-outcomes.json"
RUNS = G20 / "runs"
EXE = ROOT / "build" / "g20melt"
T0 = {"AMB2018-02": 298.15, "AMB2022-03-TRACKS": 296.65}  # K: 25 C and 23.5 C (setups: dataset_common, substrate)
T_SOLIDUS, T_LIQUIDUS = 1563.15, 1623.15  # K, IN625: Special Metals' 1290 to 1350 C
IN718_SOLIDUS, IN718_LIQUIDUS = 1533.15, 1609.15  # K, IN718: Special Metals' 1260 to 1336 C, AM-Bench's nominal


def jmatpro_cp(T):  # Lim et al. 2023 (arXiv:2302.03146) Table 1, T in K
    return 360.4 + 0.26 * T - 4e-5 * T * T


def jmatpro_k(T):
    return 0.56 + 2.9e-2 * T - 7e-6 * T * T


def in625_metal(setups):
    """the alloy as GOALS.md G20 step 2 states it, every value from the curated set-ups"""
    m = setups["materials"]["IN625"]
    cp = [(r["T_C"] + 273.15, r["cp"]) for r in m["specific_heat_J_per_kgK"]["table"]] + [(T_SOLIDUS, jmatpro_cp(T_SOLIDUS))]
    kk = [(r["T_C"] + 273.15, r["k"]) for r in m["thermal_conductivity_W_per_mK"]["table"]] + [(T_SOLIDUS, jmatpro_k(T_SOLIDUS))]

    def lin(pts, T):
        if T <= pts[0][0]:
            return pts[0][1]
        for (a, va), (b, vb) in zip(pts, pts[1:]):
            if T <= b:
                return va + (vb - va) * (T - a) / (b - a)
        return pts[-1][1]
    temps = sorted(set(round(t, 6) for t, _ in cp + kk))
    table = [{"temperature_k": t, "conductivity_w_mk": round(lin(kk, t), 6), "specific_heat_j_kgk": round(lin(cp, t), 6)} for t in temps]
    assert len(table) <= 32, len(table)
    return {
        "density_kg_m3": m["density_g_per_cm3"]["value"] * 1000,
        "solidus_k": T_SOLIDUS, "liquidus_k": T_LIQUIDUS,
        "latent_heat_j_kg": m["latent_heat_of_fusion_J_per_kg"]["value"],
        "specific_heat_solid_j_kgk": table[-1]["specific_heat_j_kgk"], "conductivity_solid_w_mk": table[-1]["conductivity_w_mk"],
        "specific_heat_liquid_j_kgk": round(jmatpro_cp(T_LIQUIDUS), 6), "conductivity_liquid_w_mk": round(jmatpro_k(T_LIQUIDUS), 6),
        "emissivity": m["emissivity"]["model_choice_only"],
        "solid_table": table,
        "source": "IN625 for GOALS.md G20 step 2: Special Metals' datasheet (density 8.44 g/cm3, melting range 1290 to 1350 C, specific heat "
                  "table 2 to 1093 C, conductivity table 3 to 982 C), each continued by one point at the solidus from Lim et al. 2023's JMatPro "
                  "fits (arXiv:2302.03146, Table 1), which also give the liquid's specific heat and conductivity at the liquidus and the latent "
                  "heat 209.2 kJ/kg; emissivity 0.4, their model's choice",
    }


def in718_metal(setups):
    """IN718 as GOALS.md G20 step 2 states it (the amendment for IN718, before any IN718 run)"""
    m = setups["materials"]["IN718"]
    props = json.loads((G20 / "in718_properties.json").read_text())
    cp = [(r["T_K"], r["cp_J_per_kgK"]) for r in props["specific_heat_solid"]["table"]]
    kk = [(r["T_C"] + 273.15, r["k_W_per_mK_annealed"]) for r in m["thermal_conductivity"]["table_SI"]]

    def lin(pts, T):
        if T <= pts[0][0]:
            return pts[0][1]
        for (a, va), (b, vb) in zip(pts, pts[1:]):
            if T <= b:
                return va + (vb - va) * (T - a) / (b - a)
        return pts[-1][1]
    temps = sorted(set(round(t, 6) for t, _ in cp + kk))
    table = [{"temperature_k": t, "conductivity_w_mk": round(lin(kk, t), 6), "specific_heat_j_kgk": round(lin(cp, t), 6)} for t in temps]
    assert len(table) <= 32, len(table)
    return {
        "density_kg_m3": m["density"]["g_per_cm3_curator_converted"]["annealed"] * 1000,
        "solidus_k": IN718_SOLIDUS, "liquidus_k": IN718_LIQUIDUS,
        "latent_heat_j_kg": props["latent_heat_J_per_kg"]["value"],
        "specific_heat_solid_j_kgk": table[-1]["specific_heat_j_kgk"], "conductivity_solid_w_mk": table[-1]["conductivity_w_mk"],
        "specific_heat_liquid_j_kgk": round(jmatpro_cp(T_LIQUIDUS), 6), "conductivity_liquid_w_mk": round(jmatpro_k(T_LIQUIDUS), 6),
        "emissivity": 0.4,
        "solid_table": table,
        "source": "IN718 for GOALS.md G20 step 2: density 8.193 g/cm3, melting range 1260 to 1336 C and conductivity (Special Metals' "
                  "datasheet, annealed); solid specific heat and latent heat 196.85 kJ/kg measured by DSC (Agazhanov, Samoshkin and Kozlovskii "
                  "2019, J. Phys.: Conf. Ser. 1382 012175, Tables 1 and 2, CC BY 3.0), the recommended table without the solid-state peaks; the "
                  "liquid's specific heat and conductivity IN625's (Lim et al. 2023's JMatPro fits at its liquidus), no open IN718 value being "
                  "found (an assumption); emissivity 0.4 as for IN625",
    }


def metal_for(c, setups):
    return in625_metal(setups) if c.get("alloy") == "IN625" else in718_metal(setups)


def case_json(c, metal, absorptivity, cell, L_pool, scale=1.0):
    """a half block on the track's plane of symmetry; the track 4.5 pool lengths, read from 2 to 3 behind its start"""
    v = c["scan_speed_mm_per_s"] * 1e-3
    w = 0.5 * c["spot"]["D4sigma_diameter_um"] * 1e-6
    x_s, L_tr = 100e-6, 4.5 * L_pool
    X = x_s + L_tr + 100e-6
    Y, Z = 200e-6 * scale, 250e-6 * scale
    return {
        "domain": "melt", "title": "G20 step 2: %s" % c["id"],
        "metal": metal,
        "block": {"size_m": [X, Y, Z], "origin_m": [0, 0, -Z], "cell_m": cell, "mirror_y": True, "held_faces": ["x-", "x+", "y+", "z-"]},
        "beam": {"power_w": c["laser_power_W"], "absorptivity": absorptivity, "radius_m": w,
                 "tracks": [{"from_m": [x_s, 0], "to_m": [x_s + L_tr, 0], "speed_m_s": v}],
                 "source": "%s: power %s W, speed %s mm/s, D4 sigma %s um (1/e^2 radius half of it), as the AM-Bench set-ups state" % (
                     c["id"], c["laser_power_W"], c["scan_speed_mm_per_s"], c["spot"]["D4sigma_diameter_um"])},
        "initial_temperature_k": T0[c["dataset"]], "convection_w_m2k": 0.0,
        "run": {"end_s": L_tr / v, "frames": 1},
        "measure": {"from_m": x_s + 2 * L_pool, "to_m": x_s + 3 * L_pool},
    }


def run_case(name, case):
    RUNS.mkdir(parents=True, exist_ok=True)
    p = RUNS / (name + ".json")
    p.write_text(json.dumps(case, indent=1) + "\n")
    t = time.time()
    r = subprocess.run([str(EXE), str(p)], capture_output=True, text=True)
    if r.returncode not in (0, 1) or not r.stdout.strip():
        raise SystemExit("%s: g20melt failed: %s" % (name, r.stderr.strip()[-500:]))
    out = json.loads(r.stdout.strip().splitlines()[-1])
    out["wall_s"] = time.time() - t
    (RUNS / (name + ".out.json")).write_text(json.dumps(out, indent=1) + "\n")
    return out


def solve(c, metal, A, cell, L_guess=None, scale=1.0, tag=""):
    """run until the read stretch lies 2 to 3 pool lengths behind the start and wholly behind the pool at the end, and the
    melt touches no face but the top and the mirror (the block enlarged by half otherwise)"""
    L = L_guess or 450e-6
    for attempt in range(4):
        name = "%s_A%.4f_h%.2f_s%.2f%s_%d" % (c["id"], A, cell * 1e6, scale, tag, attempt)
        out = run_case(name, case_json(c, metal, A, cell, L, scale))
        Lp = out["pool_length_at_end_m"]
        if out["touches_a_face"]:
            scale *= 1.5
            continue
        if abs(Lp / L - 1) <= 0.05:  # the stretch read lay 2 to 3 of these pool lengths behind the start, and behind the pool
            out.update({"pool_length_assumed_m": L, "block_scale": scale, "name": name})
            return out
        L = Lp
    raise SystemExit("%s: no steady stretch after 4 runs" % c["id"])


def load():
    setups = json.loads((G20 / "setups.json").read_text())
    cal = json.loads((G20 / "calibration.json").read_text())["cases"]
    return setups, cal


def conduction(width_um, depth_um):
    return depth_um <= 0.5 * width_um  # King et al. 2014's shape rule, GOALS.md G20 step 2


def cmd_study():
    setups, cal = load()
    metal = in625_metal(setups)
    c = next(x for x in setups["cases"] if x["id"] == "AMB2018-02-AMMT-B")
    rows = []
    L = None
    for cell in (5e-6, 3.3333333e-6, 2.5e-6):
        out = solve(c, metal, 0.5, cell, L)
        L = out["pool_length_assumed_m"]
        rows.append((cell, out))
        print("  h %.2f um: width %.2f um, depth %.2f um, pool %.0f um long, %d cells, %.0f s" % (
            cell * 1e6, out["width_m"] * 1e6, out["depth_m"] * 1e6, out["pool_length_at_end_m"] * 1e6, out["cells"], out["wall_s"]), flush=True)
    chosen = None
    for (h1, a), (h2, b) in zip(rows, rows[1:]):
        dw, dd = a["width_m"] / b["width_m"] - 1, a["depth_m"] / b["depth_m"] - 1
        print("  %.2f against %.2f um: width %+.2f %%, depth %+.2f %%" % (h1 * 1e6, h2 * 1e6, 100 * dw, 100 * dd))
        if chosen is None and abs(dw) <= 0.02 and abs(dd) <= 0.02:
            chosen = h1
    out = solve(c, metal, 0.5, chosen or rows[-1][0], L, scale=1.5, tag="_wide")
    base = next(o for h, o in rows if h == (chosen or rows[-1][0]))
    print("  the block half as large again: width %+.2f %%, depth %+.2f %%" % (100 * (out["width_m"] / base["width_m"] - 1),
                                                                            100 * (out["depth_m"] / base["depth_m"] - 1)))
    print("  cell size chosen by the rule: %s" % ("%.2f um" % (chosen * 1e6) if chosen else "none (the finest is not converged)"))


def cmd_calibrate(cell):
    setups, cal = load()
    metal = in625_metal(setups)
    tracks = []
    for cid, rec in cal.items():
        w, d = rec["outcomes"]["melt_pool_width"]["value"], rec["outcomes"]["melt_pool_depth"]["value"]
        mode = "conduction" if conduction(w, d) else "keyhole"
        print("  %s: measured %s um wide, %s deep: %s mode%s" % (cid, w, d, mode, "" if mode == "conduction" else ", not used"))
        if mode == "conduction":
            tracks.append((next(x for x in setups["cases"] if x["id"] == cid), w * 1e-6, d * 1e-6))
    grid = [0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.50]  # spans the misfit's minimum: at 0.5 the calibration track came out 1.38 times too wide
    res = {}
    L = {}
    for A in grid:
        for c, w, d in tracks:
            o = solve(c, metal, A, cell, L.get(c["id"]))
            L[c["id"]] = o["pool_length_assumed_m"]
            res[(A, c["id"])] = o
            print("  A %.2f %s: width %.2f um, depth %.2f um" % (A, c["id"], o["width_m"] * 1e6, o["depth_m"] * 1e6), flush=True)

    def F(A):  # the misfit through the grid's piecewise-cubic interpolation (Lagrange on the four nearest points)
        tot = 0
        for c, w, d in tracks:
            for key, meas in (("width_m", w), ("depth_m", d)):
                xs = sorted(grid, key=lambda g: abs(g - A))[:4]
                val = sum(res[(xi, c["id"])][key] * math.prod((A - xj) / (xi - xj) for xj in xs if xj != xi) for xi in xs)
                tot += (val / meas - 1) ** 2
        return tot
    best = min((F(grid[0] + 0.0005 * i), grid[0] + 0.0005 * i) for i in range(int(round((grid[-1] - grid[0]) / 0.0005)) + 1))
    A = round(best[1], 4)
    print("  the smallest misfit on the grid's interpolation: A = %.4f (sum of squared relative errors %.4g)" % (A, best[0]))
    for c, w, d in tracks:
        o = solve(c, metal, A, cell, L.get(c["id"]), tag="_fit")
        print("  run at A = %.4f, %s: width %.2f um (%+.1f %%), depth %.2f um (%+.1f %%)" % (
            A, c["id"], o["width_m"] * 1e6, 100 * (o["width_m"] / w - 1), o["depth_m"] * 1e6, 100 * (o["depth_m"] / d - 1)))
    rec = {"absorptivity": A, "cell_m": cell, "tracks": [c["id"] for c, _, _ in tracks], "grid": grid, "misfit": best[0],
           "rule": "GOALS.md G20 step 2: the smallest sum of squared relative errors of width and depth over the calibration tracks in conduction mode"}
    (G20 / "melt-calibration.json").write_text(json.dumps(rec, indent=1) + "\n")
    print("  written: %s" % (G20 / "melt-calibration.json"))


def cmd_predict(cell, A, alloys):
    setups, _ = load()
    preds = []
    for c in setups["cases"]:
        if c["role"] != "test" or c.get("alloy") not in alloys or not c.get("laser_power_W"):
            continue
        o = solve(c, metal_for(c, setups), A, cell)
        preds.append({"id": c["id"], "width_um": round(o["width_m"] * 1e6, 3), "depth_um": round(o["depth_m"] * 1e6, 3),
                      "pool_length_um": round(o["pool_length_at_end_m"] * 1e6, 1), "run": o["name"],
                      "energy_imbalance": (o["absorbed_j"] - o["surface_loss_j"] - o["held_faces_out_j"] - o["stored_j"]) / o["absorbed_j"]})
        print("  %s: width %.2f um, depth %.2f um, pool %.0f um long" % (c["id"], preds[-1]["width_um"], preds[-1]["depth_um"],
                                                                       preds[-1]["pool_length_um"]), flush=True)
    rec = {"frozen_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "absorptivity": A, "cell_m": cell, "alloys": alloys,
           "model": "the lab's melt solver in conduction (src/lab/melt), the alloys as GOALS.md G20 step 2 states, tools/g20_melt.py",
           "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
           "solver_sha256": hashlib.sha256((ROOT / "src/lab/melt/melt.c").read_bytes()).hexdigest(), "predictions": preds}
    p = G20 / ("predictions-melt-%s-%s.json" % ("-".join(alloys), time.strftime("%Y%m%d-%H%M%S")))
    p.write_text(json.dumps(rec, indent=1) + "\n")
    print("  frozen: %s\n  SHA-256 %s" % (p, hashlib.sha256(p.read_bytes()).hexdigest()))


def cmd_score(path, sha):
    p = Path(path)
    got = hashlib.sha256(p.read_bytes()).hexdigest()
    if got != sha:
        raise SystemExit("the predictions' SHA-256 is %s, not the recorded %s: not scored" % (got, sha))
    goals = (ROOT / "GOALS.md").read_text()
    if sha not in goals:
        raise SystemExit("the hash is not recorded in GOALS.md yet: record it before the sealed file is opened")
    sealed = json.loads(SEALED.read_text())
    pred = json.loads(p.read_text())["predictions"]
    rows = []
    for q in pred:
        rec = sealed["cases"][q["id"]] if isinstance(sealed.get("cases"), dict) else next(x for x in sealed["cases"] if x["id"] == q["id"])
        o = rec["outcomes"]
        w, d = o["melt_pool_width"]["value"], o["melt_pool_depth"]["value"]
        rows.append({"id": q["id"], "measured_width_um": w, "measured_depth_um": d, "mode": "conduction" if conduction(w, d) else "keyhole",
                     "width_error": q["width_um"] / w - 1, "depth_error": q["depth_um"] / d - 1, "predicted": q})
    def med(v):
        v = sorted(v)
        return v[len(v) // 2] if len(v) % 2 else 0.5 * (v[len(v) // 2 - 1] + v[len(v) // 2]) if v else float("nan")
    cond = [r for r in rows if r["mode"] == "conduction"]
    out = {"predictions": str(p), "predictions_sha256": sha, "rows": rows,
           "conduction_median_abs_width_error": med([abs(r["width_error"]) for r in cond]),
           "conduction_median_abs_depth_error": med([abs(r["depth_error"]) for r in cond]), "n_conduction": len(cond)}
    (G20 / ("score-" + p.stem + ".json")).write_text(json.dumps(out, indent=1) + "\n")
    for r in rows:
        print("  %-20s %-10s width %7.2f against %5.1f um (%+6.1f %%)  depth %7.2f against %5.1f um (%+6.1f %%)" % (
            r["id"], r["mode"], r["predicted"]["width_um"], r["measured_width_um"], 100 * r["width_error"], r["predicted"]["depth_um"],
            r["measured_depth_um"], 100 * r["depth_error"]))
    print("  conduction mode (%d tracks): median absolute error %.1f %% in width (criterion 15 %%), %.1f %% in depth (criterion 20 %%)" % (
        len(cond), 100 * out["conduction_median_abs_width_error"], 100 * out["conduction_median_abs_depth_error"]))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    opt = lambda k, d=None: float(a[a.index(k) + 1]) if k in a else d
    if a[0] == "study":
        cmd_study()
    elif a[0] == "calibrate":
        cmd_calibrate(opt("--cell") * 1e-6)
    elif a[0] == "predict":
        cmd_predict(opt("--cell") * 1e-6, opt("--absorptivity"), a[a.index("--alloys") + 1].split(",") if "--alloys" in a else ["IN625", "IN718"])
    elif a[0] == "score":
        cmd_score(a[1], a[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
