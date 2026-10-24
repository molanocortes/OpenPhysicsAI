#!/usr/bin/env python3
"""g20_strength.py - GOALS.md G20 step 5: the break estimate of printed plastics, calibrated on the calibration
studies only and predicting the sealed test groups blind.

The rules are in GOALS.md (written before any fit): a section that flows at its tensile strength in tension and at c
times it in compression and breaks when its outer tensile fibre reaches m times the elastic limit strain
(src/mech/breakest.c, the same C through build/libbreakest.dylib); notched bend bars by the theory of critical
distances with Creager and Paris's field. The set-ups and calibration data are read from
~/.openphysicsai/g20/fff-strength/; the test outcomes stay sealed until the predictions are frozen and their SHA-256
recorded in GOALS.md.

    make build/libbreakest.dylib
    python3 tools/g20_strength.py calibrate      # c per family, m, the critical distance and s0/s_t
    python3 tools/g20_strength.py predict        # the test groups, frozen with their SHA-256
    python3 tools/g20_strength.py score PREDICTIONS.json SHA256   # only once the hash is recorded in GOALS.md
"""
import ctypes
import hashlib
import json
import math
import statistics
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
G20 = Path.home() / ".openphysicsai" / "g20" / "fff-strength"
SEALED = Path.home() / ".openphysicsai" / "sealed" / "g20-fff-strength-test.json"
LIB = ctypes.CDLL(str(ROOT / "build" / "libbreakest.dylib"))
for name, n in (("breakest_bending_ratio", 2), ("breakest_load_factor", 4), ("breakest_critical_distance", 2), ("breakest_creager_paris", 3),
                ("breakest_senb_K", 4)):
    f = getattr(LIB, name)
    f.restype, f.argtypes = ctypes.c_double, [ctypes.c_double] * n


def g(c, m):
    return LIB.breakest_bending_ratio(c, m)


def family(material):
    t = material.upper()
    if "PLA" in t:
        return "PLA"
    if "ABS" in t or "ASA" in t:
        return "styrenic"
    return "other"


def mean_of(v):
    if isinstance(v, dict):
        return v.get("mean")
    return v


def tcd_ratio(rho, L):
    """the apparent toughness at notch radius rho over the cracked one: the point method on Creager and Paris's field"""
    return math.sqrt(1 + rho / L) * (L + rho) / (L + 2 * rho)


def golden(f, lo, hi, it=200):
    """the minimum of f on [lo, hi] (log-spaced arguments are the caller's)"""
    r = 0.5 * (math.sqrt(5) - 1)
    a, b = lo, hi
    x1, x2 = b - r * (b - a), a + r * (b - a)
    f1, f2 = f(x1), f(x2)
    for _ in range(it):
        if f1 < f2:
            b, x2, f2 = x2, x1, f1
            x1 = b - r * (b - a)
            f1 = f(x1)
        else:
            a, x1, f1 = x1, x2, f2
            x2 = a + r * (b - a)
            f2 = f(x2)
    return 0.5 * (a + b)


def studies():
    return {s["study_id"]: s for s in json.loads((G20 / "calibration.json").read_text())["studies"]}


def cmd_calibrate():
    cal = studies()
    out = {"made_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "rule": "GOALS.md G20 step 5, written before this fit"}
    # 1. c, the compressive over the tensile strength, where a study gives both for one material and orientation
    cs = []
    st = cal["CAL-04"]
    t = next(x for x in st["groups"] if x["group_id"] == "CAL-04-T")
    st_t = statistics.mean(s["tensile_strength_MPa"] for s in t["specimens"])
    E_04 = statistics.mean(s["E_MPa"] for s in t["specimens"])
    comp = next(x for x in st["groups"] if x["group_id"] == "CAL-04-C")["results"]
    sc = [v["compressive_yield_strength_MPa"]["mean"] for v in comp.values() if isinstance(v, dict) and "compressive_yield_strength_MPa" in v]
    cs.append(("CAL-04 PLA", "PLA", statistics.mean(sc) / st_t))
    c_04 = cs[-1][2]
    st9 = cal["CAL-09"]
    tens9 = {x["group_id"].replace("CAL-09-T-", ""): x for x in st9["groups"] if x["test"] == "tensile"}
    for b in st9["groups"]:
        if b["test"] != "three_point_bending":
            continue
        key = b["group_id"].replace("CAL-09-B3-", "")
        r = b["results"]
        sc = r.get("compressive_strength_MPa_ASTM_D695_same_colour", r.get("compressive_strength_MPa"))
        if key in tens9 and sc:
            cs.append(("CAL-09 " + key, family(b.get("material", key)), sc / tens9[key]["results"]["tensile_strength_MPa"]))
    fam = {}
    for _, f, c in cs:
        fam.setdefault(f, []).append(c)
    c_family = {f: statistics.median(v) for f, v in fam.items()}
    c_family["other"] = statistics.median([c for _, _, c in cs])
    out["c_values"] = [{"group": a, "family": f, "c": c} for a, f, c in cs]
    out["c_family"] = c_family
    print("== c, compressive over tensile strength: %d pairs" % len(cs))
    for f, v in sorted(fam.items()):
        print("  %-9s %2d pairs, median %.4f (from %.3f to %.3f)" % (f, len(v), statistics.median(v), min(v), max(v)))
    print("  every other polymer: the median over all, %.4f" % c_family["other"])

    # 2. m, one for every material, on the calibration bending groups
    bend = []
    b4 = next(x for x in st["groups"] if x["group_id"] == "CAL-04-B3")
    bend.append({"group": "CAL-04-B3", "s_t": st_t, "s_f": b4["results"]["flexural_strength_MPa"]["mean"], "c": c_04, "cap": 0.05 * E_04 / st_t})
    st8 = cal["CAL-08"]
    tens8 = {x["group_id"].split("-T")[-1]: x for x in st8["groups"] if x["test"] == "tensile"}
    for b in st8["groups"]:
        if b["test"] != "three_point_bending":
            continue
        code = b["group_id"].split("-B")[-1]
        tg = tens8.get(code)
        if tg:
            bend.append({"group": b["group_id"], "s_t": tg["results"]["tensile_strength_MPa"], "s_f": b["results"]["max_bending_stress_MPa"],
                         "c": c_family["PLA"], "cap": None})
    for b in st9["groups"]:
        if b["test"] != "three_point_bending":
            continue
        key = b["group_id"].replace("CAL-09-B3-", "")
        r = b["results"]
        sc = r.get("compressive_strength_MPa_ASTM_D695_same_colour", r.get("compressive_strength_MPa"))
        if key in tens9:
            s_t = tens9[key]["results"]["tensile_strength_MPa"]
            bend.append({"group": b["group_id"], "s_t": s_t, "s_f": r["flexural_strength_MPa"],
                         "c": sc / s_t if sc else c_family[family(b.get("material", key))], "cap": None})

    def misfit(logm):
        m = math.exp(logm)
        return sum(math.log(g(x["c"], min(m, x["cap"]) if x["cap"] else m) * x["s_t"] / x["s_f"]) ** 2 for x in bend)
    grid = [math.log(1 + 0.01 * i) for i in range(0, 2000)]
    best = min(grid, key=misfit)
    lo, hi = max(0.0, best - 0.02), best + 0.02
    logm = golden(misfit, lo, hi)
    m = math.exp(logm)
    out["m"] = m
    out["bending_groups"] = []
    print("== m, the outer tensile fibre's strain at the break over s_t/E: %.4f (sum of squared log errors %.4g over %d groups)" % (m, misfit(logm), len(bend)))
    errs = []
    for x in bend:
        mm = min(m, x["cap"]) if x["cap"] else m
        pred = g(x["c"], mm) * x["s_t"]
        errs.append(pred / x["s_f"] - 1)
        out["bending_groups"].append(dict(x, predicted=pred, error=pred / x["s_f"] - 1))
        print("  %-26s s_t %6.2f c %.3f: g %.3f, predicted %6.2f against %6.2f MPa (%+5.1f %%)" % (x["group"], x["s_t"], x["c"], g(x["c"], mm), pred,
                                                                                            x["s_f"], 100 * errs[-1]))
    print("  calibration fit: median absolute error %.1f %%" % (100 * statistics.median(abs(e) for e in errs)))

    # 3. the critical distance and s0/s_t on CAL-03's notched PLA bend bars
    st3 = cal["CAL-03"]
    tens3 = {(x["material"], x["raster_deg"]): x for x in st3["groups"] if x["test"] == "tensile"}
    series = {}
    for x in st3["groups"]:
        if x["test"].startswith("SENB"):
            series.setdefault((x["material"], x["raster_deg"]), {})[x["notch_root_radius_mm"] * 1e-3] = \
                x["results"]["apparent_fracture_toughness_KmatN_MPa_sqrt_m"]["mean"]
    Ls, ks = [], []
    out["tcd_series"] = []
    print("== the critical distance on CAL-03 (point method, Creager and Paris)")
    for key, pts in sorted(series.items()):
        Kc = pts[0.0]
        notched = [(r, K) for r, K in pts.items() if r > 0]

        def mis(logL):
            L = math.exp(logL)
            return sum(math.log(Kc * tcd_ratio(r, L) / K) ** 2 for r, K in notched)
        grid = [math.log(1e-6) + i * (math.log(1e-1) - math.log(1e-6)) / 2000 for i in range(2001)]
        b0 = min(grid, key=mis)
        logL = golden(mis, b0 - 0.01, b0 + 0.01)
        L = math.exp(logL)
        s_t = tens3[key]["results"]["ultimate_tensile_strength_MPa"]["mean"]
        s0 = Kc / math.sqrt(math.pi * L)  # MPa sqrt(m) / sqrt(m)
        Ls.append(L), ks.append(s0 / s_t)
        out["tcd_series"].append({"material": key[0], "raster": key[1], "Kc": Kc, "L_m": L, "s0_MPa": s0, "s_t_MPa": s_t, "k": s0 / s_t})
        print("  %-7s %-7s K_c %.2f MPa sqrt(m): L %.3f mm, s0 %.1f MPa = %.2f s_t; fit %s" % (
            key[0], key[1], Kc, 1e3 * L, s0, s0 / s_t, ", ".join("%.2f/%.2f" % (Kc * tcd_ratio(r, L), K) for r, K in sorted(notched))))
    out["L_m"], out["k"] = statistics.median(Ls), statistics.median(ks)
    print("  medians: L %.3f mm, s0/s_t %.3f" % (1e3 * out["L_m"], out["k"]))
    (G20 / "break-calibration.json").write_text(json.dumps(out, indent=1) + "\n")
    print("  written: %s" % (G20 / "break-calibration.json"))


def bend_strength(s_t, c, m, E=None, d790=False):
    """the bending strength the elastic formula reports: g(c, m) s_t, m capped at ASTM D790's 5 % strain"""
    mm = min(m, 0.05 * E / s_t) if (d790 and E) else m
    return g(c, mm) * s_t, mm


def senb(spec, s_t, cal, c):
    """one notched bend bar (ASTM D6068/D5045 geometry, span 4 W): the apparent toughness by the point method and the
    largest load, the lesser of the notch's and the ligament's bending capacity"""
    L, k, m = cal["L_m"], cal["k"], cal["m"]
    rho, W, B, a = spec["rho_mm"] * 1e-3, spec["W_mm"] * 1e-3, spec["B_mm"] * 1e-3, spec["a0_mm"] * 1e-3
    s0 = k * s_t  # MPa
    Kc = s0 * math.sqrt(math.pi * L)  # MPa sqrt(m)
    KN = Kc * tcd_ratio(rho, L)
    S = 4 * W
    f = LIB.breakest_senb_K(1.0, B, W, a)  # Pa sqrt(m) per N: K = P f
    P_notch = KN * 1e6 / f
    P_lig = 4 * g(c, m) * s_t * 1e6 * B * (W - a) ** 2 / (6 * S)
    return KN, min(P_notch, P_lig), P_notch, P_lig


def kirsch_heywood_strength(s_t, s0, L, d, W):
    """an open hole's gross-section strength: the point method on Kirsch's stress along the net section scaled to Heywood's
    finite-width peak, and the net section at s_t; the lesser"""
    a = 0.5 * d
    kt_net = 2 + (1 - d / W) ** 3
    kt_gross = kt_net / (1 - d / W)
    x = a + 0.5 * L
    shape = 1 + 0.5 * (a / x) ** 2 + 1.5 * (a / x) ** 4  # Kirsch, 3 at the edge
    tcd = s0 / (kt_gross / 3 * shape)
    net = s_t * (1 - d / W)
    return min(tcd, net), tcd, net


def predict_test(cal):
    """the step 1 test set (test-setups.json), with the curator's addendum of methods-figure inputs where it exists"""
    d = json.loads((G20 / "test-setups.json").read_text())
    add = {}
    if (G20 / "test-setups-addendum.json").exists():
        add = json.loads((G20 / "test-setups-addendum.json").read_text()).get("studies", {})
    st = {x["study_id"]: x for x in d["studies"]}
    cf, m = cal["c_family"], cal["m"]
    out, refused = {}, {}
    # TST-04: ABS, ASTM D790, the tensile coupon of the same raster
    s4 = st["TST-04"]
    tens = {x["raster_deg"]: x for x in s4["groups"] if x["test"].startswith("tensile")}
    for gr in s4["groups"]:
        if gr["test"] != "three_point_bending":
            continue
        t = tens[gr["raster_deg"]]["results"]
        s_t, E = t["ultimate_strength_MPa"]["mean"], t["effective_modulus_MPa"]["mean"]
        sf, mm = bend_strength(s_t, cf["styrenic"], m, E, d790=True)
        out[gr["group_id"]] = {"flexural_strength_MPa": sf, "s_t_MPa": s_t, "c": cf["styrenic"], "m": mm,
                               "how": "g(c, m) s_t, the ABS family's c, m capped at D790's 5 %% strain (E %.0f MPa)" % E}
    # TST-05: PLA, ISO 178; the bars' orientation from the addendum decides the matched coupon
    s5 = st["TST-05"]
    po = json.dumps((add.get("TST-05") or {}).get("print_orientation_bending", {}))
    orient = "H" if "flat (XY)" in po else None  # the bars lie as the horizontal coupons do (the curator's reading of Figure 2c)
    tens = {(x["group_id"].split("-T-")[1].split("-")[0], x["nozzle_temperature_C"]): x for x in s5["groups"] if x["test"] == "tensile"}
    for gr in s5["groups"]:
        if gr["test"] != "three_point_bending":
            continue
        if orient not in ("H", "Z"):
            refused[gr["group_id"]] = "the bending bars' print orientation is not known (Figure 2c not read)"
            continue
        t = tens[(orient, gr["nozzle_temperature_C"])]["results"]
        s_t = t["tensile_strength_MPa"]["mean"]
        sf, mm = bend_strength(s_t, cf["PLA"], m)
        out[gr["group_id"]] = {"flexural_strength_MPa": sf, "s_t_MPa": s_t, "c": cf["PLA"], "m": mm,
                               "how": "g(c, m) s_t, the PLA family's c, the %s coupon at the same nozzle temperature" % orient}
    # TST-01: ASA notched bend bars, the apparent toughness and the largest load
    s1 = st["TST-01"]
    tens = {x["raster_deg"]: x for x in s1["groups"] if x["test"] == "tensile"}
    for gr in s1["groups"]:
        if not gr["test"].startswith("SENB"):
            continue
        s_t = tens[gr["raster_deg"]]["results"]["tensile_strength_MPa"]["mean"]
        rows = [senb(sp, s_t, cal, cf["styrenic"]) for sp in gr["specimens_measured_geometry"]]
        out[gr["group_id"]] = {"apparent_toughness_MPa_sqrt_m": statistics.mean(r[0] for r in rows),
                               "largest_load_N": statistics.mean(r[1] for r in rows), "s_t_MPa": s_t,
                               "how": "point method on Creager and Paris, L %.3f mm and s0 = %.3f s_t from CAL-03's PLA (carried to ASA, an "
                                      "assumption), span 4 W (ASTM D6068); the load the lesser of the notch's and the ligament's" % (1e3 * cal["L_m"], cal["k"]),
                               "per_specimen": [{"K": r[0], "P": r[1], "P_notch": r[2], "P_ligament": r[3]} for r in rows]}
    # TST-02: every run printed with 20 to 80 % infill, which a solid section does not model; TST-06: the bars' thickness and span are
    # not given; TST-03: its outcomes are in figures only (GOALS.md G20 step 5, before these predictions)
    for gr in st["TST-02"]["groups"]:
        refused[gr["group_id"]] = "printed with %g %% infill: the break estimate is for solid sections" % gr["settings"]["infill_density_percent"]
    for gr in st["TST-06"]["groups"]:
        if gr.get("test") == "three_point_bending":
            refused[gr["group_id"]] = "the bending bars' thickness and span are not given (text and figures)"
    refused["TST-03"] = "its outcomes are given in figures only"
    return out, refused


def predict_published(cal):
    """the 26 published groups of G17 step 5 with the break estimate (a new, versioned file beside the first)"""
    sys.path.insert(0, str(ROOT / "tools"))
    import publishedbreaks as pb
    setups = json.loads(pb.SETUPS.read_text())
    mod = pb.models(setups)
    cf, m = cal["c_family"], cal["m"]
    out = {}
    for t in setups["scored_tests"]:
        tid = t["id"]
        if t["tier"] != "primary":
            out[tid] = {"not_modelled": "secondary group (V-notched shear), left out as declared"}
            continue
        if tid.startswith("ncamp"):
            mm_, fam = mod["ncamp"], "other"
        elif tid.startswith("gomezgras"):
            mm_, fam = mod["pciso " + {"0": "0", "pm45": "+/- 45", "90": "90"}[tid.rsplit("-", 1)[1]]], "other"
        else:
            mm_, fam = mod["caminero " + {"pla": "PLA", "pla3d850": "PLA 3D850", "plagraphene": "PLA-Graphene"}[tid.rsplit("-", 1)[1]]], "PLA"
        upright = "-z" in tid and "z45" not in tid or "zflat" in tid or "upright" in tid
        if "z45" in tid and "oht" in tid:
            out[tid] = {"not_modelled": "open-hole bar at 45 degrees, as in the first predictions"}
            continue
        d790 = "D790" in json.dumps(t.get("test", "")) or "D790" in json.dumps(t.get("standard", ""))
        if "oht" in tid:
            across = tid.rsplit("-", 1)[1] == "z"  # built along the load: the hole's net section pulls the layers apart
            s_t = mm_["Zt"] if across else mm_["Xt"]
            gm = t["geometry"]
            val, tcd, net = kirsch_heywood_strength(s_t, cal["k"] * s_t, cal["L_m"] * 1e3, gm["hole_diameter_mm"], gm["width_mm"])
            out[tid] = {"predicted_nominal_strength_mpa": val, "tcd_mpa": tcd, "net_section_mpa": net, "s_t_MPa": s_t,
                        "how": "point method on Kirsch scaled to Heywood (L %.3f mm, s0 %.3f s_t from CAL-03), or the net section at s_t" % (
                            1e3 * cal["L_m"], cal["k"])}
        elif "z45" in tid and "flex" in tid:
            # bending at 45 degrees to the layers: g(c, m) times the bar axis's first-failure strength (Tsai-Wu at 45 degrees), the
            # D790 cap with the axis's modulus (the transversely isotropic record turned 45 degrees about the bar's width)
            s_t = pb.tsai_wu_45(mm_)
            inv = 0.25 / mm_["E1"] + 0.25 / mm_["E3"] + 0.25 * (1 / mm_["G13"] - 2 * pb.NU / mm_["E1"])
            sf, mmm = bend_strength(s_t, cf[fam], m, 1 / inv, d790)
            out[tid] = {"predicted_nominal_strength_mpa": sf, "s_t_MPa": s_t, "c": cf[fam], "m": mmm, "d790_cap": d790, "E45_MPa": 1 / inv,
                        "how": "g(c, m) times Tsai-Wu's strength at 45 degrees, the %s family's c" % fam}
        elif "z45" in tid:
            out[tid] = {"predicted_nominal_strength_mpa": pb.tsai_wu_45(mm_), "how": "uniform tension at 45 degrees: first failure, as before"}
        else:
            s_t, E = (mm_["Zt"], mm_["E3"]) if upright else (mm_["Xt"], mm_["E1"])
            sf, mmm = bend_strength(s_t, cf[fam], m, E, d790)
            out[tid] = {"predicted_nominal_strength_mpa": sf, "s_t_MPa": s_t, "c": cf[fam], "m": mmm, "d790_cap": d790,
                        "how": "g(c, m) %s, the %s family's c" % ("Zt" if upright else "Xt", fam)}
    return out


def freeze(path, body):
    path.write_text(json.dumps(body, indent=1, sort_keys=True) + "\n")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cmd_predict():
    cal = json.loads((G20 / "break-calibration.json").read_text())
    stamp = time.strftime("%Y%m%d-%H%M%S")
    tool = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    lib = hashlib.sha256((ROOT / "src/mech/breakest.c").read_bytes()).hexdigest()
    test, refused = predict_test(cal)
    p1 = G20 / ("predictions-break-%s.json" % stamp)
    h1 = freeze(p1, {"made": stamp, "calibration": cal, "tool_sha256": tool, "breakest_sha256": lib, "predictions": test, "not_predicted": refused,
                     "sealed_opened": False})
    pub = predict_published(cal)
    p2 = Path.home() / ".openphysicsai" / "g17-published" / ("predictions-break-v2-%s.json" % stamp)
    h2 = freeze(p2, {"made": stamp, "calibration": cal, "tool_sha256": tool, "breakest_sha256": lib, "predictions": pub, "sealed_opened": False})
    for k, v in sorted(test.items()):
        print("  %-24s %s" % (k, ", ".join("%s %.4g" % (kk, vv) for kk, vv in v.items() if isinstance(vv, float))))
    for k, v in sorted(refused.items()):
        print("  %-24s not predicted: %s" % (k, v))
    for k, v in sorted(pub.items()):
        if "predicted_nominal_strength_mpa" in v:
            print("  %-40s %8.3f MPa  (%s)" % (k, v["predicted_nominal_strength_mpa"], v["how"]))
    print("frozen: %s\n  SHA-256 %s\nfrozen: %s\n  SHA-256 %s" % (p1, h1, p2, h2))


def checked(path, sha):
    got = hashlib.sha256(Path(path).read_bytes()).hexdigest()
    if got != sha:
        raise SystemExit("%s: SHA-256 %s, not the recorded %s: not scored" % (path, got, sha))
    if sha not in (ROOT / "GOALS.md").read_text():
        raise SystemExit("%s: its hash is not recorded in GOALS.md: record it before the sealed file is opened" % path)
    return json.loads(Path(path).read_text())


def med(v):
    v = sorted(v)
    return (v[(len(v) - 1) // 2] + v[len(v) // 2]) / 2 if v else float("nan")


def cmd_score(test_path, test_sha, pub_path, pub_sha):
    test = checked(test_path, test_sha)["predictions"]
    pub = checked(pub_path, pub_sha)["predictions"]
    if hashlib.sha256(SEALED.read_bytes()).hexdigest() != "3fc3a79cbebb2ef68e374c07123c23e3ef001278b291aa7adb6e46f15ae565ea":
        raise SystemExit("the step 1 sealed file is not the one sealed")
    sealed = {g["group_id"]: g for st in json.loads(SEALED.read_text())["studies"] for g in st.get("groups", [])}
    rows = []
    for gid, pr in sorted(test.items()):
        o = sealed[gid]
        if gid.startswith("TST-01"):
            meas, pred, kind = o["KmatN_group_mean_MPa_sqrt_m"], pr["apparent_toughness_MPa_sqrt_m"], "notched bend bar, apparent toughness"
            load = statistics.mean(x["Pcrit_N"] for x in o["specimens"] if x.get("Pcrit_N") is not None)  # a specimen without a load is left out
            extra = {"largest_load_predicted_N": pr["largest_load_N"], "largest_load_measured_N": load,
                     "largest_load_error": pr["largest_load_N"] / load - 1}
        elif gid.startswith("TST-04"):
            meas, pred, kind, extra = o["flexural_ultimate_strength_MPa"]["mean"], pr["flexural_strength_MPa"], "bending", {}
        else:
            meas, pred, kind, extra = o["flexural_strength_MPa"]["mean"], pr["flexural_strength_MPa"], "bending", {}
        rows.append(dict({"id": gid, "kind": kind, "predicted": pred, "measured": meas, "error": pred / meas - 1}, **extra))
    print("== the step 1 test set")
    for r in rows:
        print("  %-24s %-38s %8.3f against %8.3f (%+6.1f %%)%s" % (r["id"], r["kind"], r["predicted"], r["measured"], 100 * r["error"],
              "  load %.1f against %.1f N (%+.1f %%)" % (r["largest_load_predicted_N"], r["largest_load_measured_N"], 100 * r["largest_load_error"])
              if "largest_load_error" in r else ""))
    by = {}
    for r in rows:
        by.setdefault(r["kind"], []).append(abs(r["error"]))
    for k, v in by.items():
        print("  %-38s %2d groups: median absolute error %.1f %%" % (k, len(v), 100 * med(v)))
    all1 = med([abs(r["error"]) for r in rows])
    print("  the whole set, %d groups: median absolute error %.1f %% (criterion 25 %%)" % (len(rows), 100 * all1))
    # the 26 published groups, beside the first-failure predictions frozen for G17 step 5
    sys.path.insert(0, str(ROOT / "tools"))
    import publishedbreaks as pb
    if hashlib.sha256(pb.SEALED.read_bytes()).hexdigest() != pb.SEALED_SHA256:
        raise SystemExit("the published sealed file is not the one sealed")
    answers = json.loads(pb.SEALED.read_text())["answers"]
    first = json.loads(pb.PREDICTIONS.read_text())["predictions"]
    prow = []
    for tid, pr in sorted(pub.items()):
        if "predicted_nominal_strength_mpa" not in pr:
            continue
        a = answers[tid]
        e = pr["predicted_nominal_strength_mpa"] / a["mean_MPa"] - 1
        f0 = first.get(tid, {}).get("predicted_nominal_strength_mpa")
        prow.append({"id": tid, "kind": pb.kind_of(tid), "predicted": pr["predicted_nominal_strength_mpa"], "measured": a["mean_MPa"], "error": e,
                     "first_failure_error": (f0 / a["mean_MPa"] - 1) if f0 else None})
    print("== the 26 published groups (G17 step 5), the break estimate beside the first-failure estimate")
    for r in prow:
        print("  %-40s %-34s %8.3f against %8.3f MPa (%+6.1f %%; first failure %s)" % (r["id"], r["kind"], r["predicted"], r["measured"],
              100 * r["error"], "%+.1f %%" % (100 * r["first_failure_error"]) if r["first_failure_error"] is not None else "n/a"))
    byk = {}
    for r in prow:
        byk.setdefault(r["kind"], []).append(r)
    for k, v in byk.items():
        print("  %-38s %2d groups: median absolute error %.1f %% (first failure %.1f %%)" % (
            k, len(v), 100 * med([abs(x["error"]) for x in v]), 100 * med([abs(x["first_failure_error"]) for x in v if x["first_failure_error"] is not None])))
    all2 = med([abs(r["error"]) for r in prow])
    ff = med([abs(r["first_failure_error"]) for r in prow if r["first_failure_error"] is not None])
    print("  all %d groups: median absolute error %.1f %% (criterion 25 %%); first failure %.1f %%; under the measured mean in %d of %d" % (
        len(prow), 100 * all2, 100 * ff, sum(r["error"] < 0 for r in prow), len(prow)))
    out = {"test_set": {"predictions": str(test_path), "sha256": test_sha, "rows": rows, "median_abs_error": all1,
                        "by_kind": {k: med(v) for k, v in by.items()}},
           "published": {"predictions": str(pub_path), "sha256": pub_sha, "rows": prow, "median_abs_error": all2, "first_failure_median": ff}}
    (G20 / ("score-" + Path(test_path).stem + ".json")).write_text(json.dumps(out, indent=1) + "\n")
    print("  written: %s" % (G20 / ("score-" + Path(test_path).stem + ".json")))


def main():
    a = sys.argv[1:]
    if a[:1] == ["score"]:
        cmd_score(*a[1:5])
        return 0
    if a[:1] == ["calibrate"]:
        cmd_calibrate()
        return 0
    if a[:1] == ["predict"]:
        cmd_predict()
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
