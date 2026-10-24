#!/usr/bin/env python3
"""publishedbreaks.py - GOALS.md G17 step 5's digital stand-in: the model against published breaks, blind.

Criterion, written in GOALS.md (step 5) before the first run. An agent that makes no prediction read three open sources
into ~/.openphysicsai/g17-published/setups.json (set-ups and tensile coupons) and sealed the outcomes of the scored tests
in ~/.openphysicsai/sealed/g17-published-breaks.json, outside the repository. `predict` builds every prediction from the
set-ups and the coupons alone and writes it, with its SHA-256, before anything reads the sealed file; `score` then checks
the sealed file against the hash recorded at sealing, and compares.

The model is G17's: a transversely isotropic record with the in-layer modulus and strength of the flat coupon and the
across-layer ones of the upright coupon (PC-ISO: of the same raster angle), Poisson 0.35, compression equal to tension and
shear strengths from tension (S12 = Xt / sqrt 3, S23 = S31 = Zt / sqrt 3), Tsai-Wu with interaction -1/2. A prediction is
the standard's nominal strength at the model's first failure:
  - open-hole tension (gross section): a quarter plate with symmetry planes, solved through MCP on TET10 at 0.7 mm, the
    strength ratio as step 4 states it times the applied 10 MPa;
  - three-point bending and the 45-degree tension bar: the same criterion at beam theory's or the uniform stress, which
    for a linear-elastic first failure is the strength along the bar's axis (Xt in the layers, Zt across them, Tsai-Wu's
    at 45 degrees: 2 / sqrt(1/X^2 + 4/Z^2 - 1/(X Z))).
Not modelled: the open-hole bar at 45 degrees and the V-notched shear groups (secondary).

    make openphysics-mcp && python3 tools/publishedbreaks.py predict [--keep]
    python3 tools/publishedbreaks.py diagnose    # the open-hole closed form, frozen before the sealed file is opened
    python3 tools/publishedbreaks.py score
"""
import hashlib
import json
import math
import shutil
import struct
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOME = Path.home() / ".openphysicsai"
SETUPS = HOME / "g17-published" / "setups.json"
SEALED = HOME / "sealed" / "g17-published-breaks.json"
PREDICTIONS = HOME / "g17-published" / "predictions.json"
DIAGNOSTIC = HOME / "g17-published" / "diagnostic.json"
SCORE = HOME / "g17-published" / "score.json"
PREDICTIONS_SHA256 = "3cc5bee89844045aa51e420ec841411ebda77fbd1592b7ad5da57e3c94778025"  # as frozen on 2026-10-08 at 00:48
SEALED_SHA256 = "db3a30ca6054a8f0d24ee1055023afd6dd19e5fc31dde415b0ed3aa9840ab0f1"  # recorded at sealing (GOALS.md G17 step 5)
DIAGNOSTIC_SHA256 = "4b48f0806235db1051a7528ad5fcc08e22590b2f12340578a4a93106b9506de3"  # frozen on 2026-10-08 at 01:10, sealed file unopened
NU = 0.35
SIGMA_APPLIED = 10.0  # MPa, gross, on the open-hole plates (linear: the ratio scales it)


def record(e_in, x_t, e_3, z_t):
    """the model's material from two coupons, with the library's rules for what they do not give (MPa)"""
    g12 = e_in / (2 * (1 + NU))
    g13 = e_in * e_3 / (e_in + e_3 + 2 * NU * e_3)
    return {"E1": e_in, "E3": e_3, "G12": g12, "G13": g13, "Xt": x_t, "Zt": z_t}


def tsai_wu_45(m):
    x, z = m["Xt"], m["Zt"]
    return 2 / math.sqrt(1 / x**2 + 4 / z**2 - 1 / (x * z))


def models(setups):
    """per study: the coupons the model is built from"""
    st = {s["study_id"]: s for s in setups["studies"]}
    nc = {e["group"].split()[0]: e for e in st["ncamp-ultem9085"]["model_inputs_A"]}
    out = {"ncamp": record(nc["XT"]["tensile_modulus"]["mean_GPa"] * 1e3, nc["XT"]["tensile_strength"]["mean_MPa"],
                           nc["ZT"]["tensile_modulus"]["mean_GPa"] * 1e3, nc["ZT"]["tensile_strength"]["mean_MPa"])}
    pc = {e["label"]: e for e in st["gomezgras2021-pciso"]["model_inputs_A"]}
    for raster in ("0", "+/- 45", "90"):
        f, u = pc["X-Flat %s°" % raster], pc["Z-Flat %s°" % raster]
        out["pciso " + raster] = record(f["tensile_modulus_MPa"]["mean"], f["tensile_strength_MPa"]["mean"],
                                        u["tensile_modulus_MPa"]["mean"], u["tensile_strength_MPa"]["mean"])
    ca = {(e["orientation"], e["material"]): e for e in st["caminero2019-pla"]["model_inputs_A"]}
    for mat in ("PLA", "PLA 3D850", "PLA-Graphene"):
        f, u = ca[("Flat", mat)], ca[("Upright", mat)]
        out["caminero " + mat] = record(f["tensile_modulus_GPa"]["mean"] * 1e3, f["tensile_strength_MPa"]["mean"],
                                        u["tensile_modulus_GPa"]["mean"] * 1e3, u["tensile_strength_MPa"]["mean"])
    return out


def write_stl(path, tris):
    with open(path, "wb") as f:
        f.write(b"publishedbreaks quarter plate".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


def quarter_plate(path, half_length, half_width, thickness, radius, segments=24):
    """x >= 0, y >= 0 of a plate with a central hole: the arc, then the two symmetry faces, the end and the side"""
    arc = [(radius * math.cos(math.pi / 2 * k / segments), radius * math.sin(math.pi / 2 * k / segments)) for k in range(segments + 1)]
    outline = arc + [(0.0, half_width), (half_length, half_width), (half_length, 0.0)]  # (radius, 0) ... (0, radius), then round
    corner = (half_length, half_width)
    # the outline runs clockwise round the material seen from +z, so the material lies on the right of each edge a -> b
    tris = []
    for z, up in ((0.0, False), (thickness, True)):  # a fan from the far corner, which sees the whole outline
        for a, b in zip(outline, outline[1:] + outline[:1]):
            if a == corner or b == corner:
                continue
            c3, a3, b3 = (corner[0], corner[1], z), (a[0], a[1], z), (b[0], b[1], z)
            tris.append([c3, b3, a3] if up else [c3, a3, b3])  # +z on the top, -z on the bottom
    for a, b in zip(outline, outline[1:] + outline[:1]):  # the walls face left of a -> b, away from the material
        p0, p1, p2, p3 = (a[0], a[1], 0.0), (b[0], b[1], 0.0), (b[0], b[1], thickness), (a[0], a[1], thickness)
        tris += [[p0, p2, p1], [p0, p3, p2]]
    write_stl(path, tris)


def solve_open_hole(c, ws, name, test, m, build, raster):
    g = test["geometry"]
    thick = float(g["thickness"].split("(")[1].split(" mm")[0])
    stl = ws.parent / (name + ".stl")
    quarter_plate(stl, g["width_mm"], g["width_mm"] / 2, thick, g["hole_diameter_mm"] / 2)

    def call(op, args):
        r = c.call(op, args)["result"]["structuredContent"]
        if not r.get("ok"):
            raise RuntimeError("%s: %s" % (op, json.dumps(r)[:600]))
        return r.get("value", {})
    call("project_create", {"name": name, "description": "publishedbreaks: %s (GOALS.md G17 step 5)" % test["id"]})
    call("geometry_import", {"path": str(stl), "units": "mm", "name": "part"})
    call("material_define", {"material": {"id": "coupon_iso", "name": "isotropic placeholder of the study's flat coupon", "family": "polymer",
                                          "status": "user_supplied", "provenance": "publishedbreaks: the flat coupon's modulus, Poisson 0.35, for the static model's checks",
                                          "youngs_modulus_pa": {"value": m["E1"] * 1e6}, "poisson_ratio": {"value": NU}, "density_kg_m3": {"value": 1300}}})
    call("material_assign", {"body": "part", "material": "coupon_iso", "source": "user"})
    mpa = lambda v: "%.9g MPa" % v
    call("mech_material_define", {"material": {
        "id": "printed", "model": "orthotropic",
        "elastic": {"E1": mpa(m["E1"]), "E2": mpa(m["E1"]), "E3": mpa(m["E3"]), "nu12": NU, "nu13": NU, "nu23": NU, "G12": mpa(m["G12"]),
                    "G23": mpa(m["G13"]), "G13": mpa(m["G13"]), "source": "literature"},
        "strength": {"Xt": mpa(m["Xt"]), "Xc": mpa(m["Xt"]), "Yt": mpa(m["Xt"]), "Yc": mpa(m["Xt"]), "Zt": mpa(m["Zt"]), "Zc": mpa(m["Zt"]),
                     "S12": mpa(m["Xt"] / math.sqrt(3)), "S23": mpa(m["Zt"] / math.sqrt(3)), "S31": mpa(m["Zt"] / math.sqrt(3)), "source": "literature"},
        "note": "the study's flat and upright tensile coupons; Poisson, compression and shear by the library's rules (publishedbreaks)"}})
    call("mesh_generate", {"method": "tet", "surface_size": 0.7, "order": 2})
    for sel, query in (("sym_x", {"plane": {"axis": "x", "at": "min"}}), ("sym_y", {"plane": {"axis": "y", "at": "min"}}),
                       ("bottom", {"plane": {"axis": "z", "at": "min"}}), ("end", {"plane": {"axis": "x", "at": "max"}})):
        call("selection_create", {"name": sel, "query": query, "source": "user"})
    for nm, sel in (("symmetry_x", "sym_x"), ("symmetry_y", "sym_y"), ("bottom", "bottom")):
        call("boundary_apply", {"name": nm, "kind": "frictionless_support", "selection": sel, "source": "user"})
    force = SIGMA_APPLIED * (g["width_mm"] / 2) * thick
    call("boundary_apply", {"name": "pull", "kind": "force", "selection": "end", "force": ["%.9g N" % force, 0, 0], "source": "user"})
    run = call("analysis_run", {"label": name, "material_model": {"type": "orthotropic", "material": "printed", "build_direction": build,
                                                                  "raster_reference": raster, "criterion": "tsai_wu"}})
    status = {}
    for _ in range(int(6 * 45)):
        status = call("job_status", {"job_id": run["job_id"], "wait_seconds": 10})
        if status.get("state") not in ("queued", "running"):
            break
    if status.get("state") != "succeeded":
        raise RuntimeError("%s: %s" % (name, json.dumps(status)[:400]))
    summary = next(json.loads(p.read_text()) for p in (ws / name).rglob("summary.json"))
    st = summary["strength"]
    return st["stated_strength_ratio"] * SIGMA_APPLIED, {"stated_strength_ratio": st["stated_strength_ratio"], "peak_flag": st["peak_flag"],
                                                         "governing_mode": st.get("stated_governing_mode"), "where_mm": st.get("stated_location_mm"),
                                                         "elements": summary.get("model", {}).get("mesh", {}).get("elements")}


def predict():
    setups = json.loads(SETUPS.read_text())
    if SEALED.exists():
        print("  the sealed file exists and is not opened here")
    mod = models(setups)
    preds = {}
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-publishedbreaks-"))
    ws = tmp / "projects"
    ws.mkdir()
    sys.path.insert(0, str(ROOT / "tools"))
    from mcptest import Client
    c = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(tmp)])
    c.initialize()
    try:
        for t in setups["scored_tests"]:
            tid, kind = t["id"], t["test"]
            if t["tier"] != "primary":
                preds[tid] = {"not_modelled": "secondary group (V-notched shear), left out as declared"}
                continue
            if tid.startswith("ncamp"):
                m = mod["ncamp"]
            elif tid.startswith("gomezgras"):
                m = mod["pciso " + {"0": "0", "pm45": "+/- 45", "90": "90"}[tid.rsplit("-", 1)[1]]]
            else:
                m = mod["caminero " + {"pla": "PLA", "pla3d850": "PLA 3D850", "plagraphene": "PLA-Graphene"}[tid.rsplit("-", 1)[1]]]
            upright = "-z" in tid and "z45" not in tid or "zflat" in tid or "upright" in tid
            if "z45" in tid and "oht" in tid:
                preds[tid] = {"not_modelled": "open-hole bar at 45 degrees: its tilted layers couple extension and shear, and no load-free support fits a "
                                              "plate with a central hole"}
                continue
            t0 = time.time()
            if "oht" in tid:
                build, raster = {"x": ("z", "x"), "y": ("y", "x"), "z": ("x", "y")}[tid.rsplit("-", 1)[1]]
                value, extra = solve_open_hole(c, ws, tid.replace("-", "_"), t, m, build, raster)
                how = "quarter plate, TET10 0.7 mm, build direction %s, stated strength ratio x %g MPa" % (build, SIGMA_APPLIED)
            elif "z45" in tid:
                value, extra, how = tsai_wu_45(m), {}, "Tsai-Wu at 45 degrees between the bar's axis and the build direction"
            elif upright:
                value, extra, how = m["Zt"], {}, "bending stress across the layers: Zt"
            else:
                value, extra, how = m["Xt"], {}, "bending stress along the layers: Xt"
            preds[tid] = dict({"predicted_nominal_strength_mpa": value, "how": how, "model": m}, **extra)
            print("  %-38s predicted %8.3f MPa  (%s)  [%.1f s]" % (tid, value, how, time.time() - t0), flush=True)
    finally:
        c.close()
    body = {"made": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "setups_sha256": hashlib.sha256(SETUPS.read_bytes()).hexdigest(),
            "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "sealed_opened": False, "predictions": preds}
    PREDICTIONS.write_text(json.dumps(body, indent=1, sort_keys=True))
    print("predictions written: %s (SHA-256 %s)" % (PREDICTIONS, hashlib.sha256(PREDICTIONS.read_bytes()).hexdigest()))
    if "--keep" not in sys.argv:
        shutil.rmtree(tmp)
    return 0


def frozen(path, sha):
    """the file's content, refused unless it is the one whose SHA-256 was recorded"""
    body = path.read_bytes()
    got = hashlib.sha256(body).hexdigest()
    if got != sha:
        raise SystemExit("%s has SHA-256 %s, not the recorded %s: refused" % (path, got, sha))
    return json.loads(body)


def kt_open_hole(ex, ey, nu_xy, g_xy, d_over_w):
    """gross-section stress concentration at the hole of a strip pulled along x, its material axes along the strip:
    Lekhnitskii's infinite-plate factor 1 + sqrt(2 (sqrt(Ex / Ey) - nu_xy) + Ex / Gxy) times the isotropic finite-width
    factor (2 + (1 - d/W)^3) / (3 (1 - d/W)), which with the factor 3 of an isotropic plate is Heywood's gross Kt"""
    k_inf = 1 + math.sqrt(2 * (math.sqrt(ex / ey) - nu_xy) + ex / g_xy)
    q = 1 - d_over_w
    return k_inf * (2 + q**3) / (3 * q), k_inf


def diagnose():
    """Written and frozen after the predictions and before the sealed file is opened (2026-10-08): the open-hole groups
    in closed form for the same model, plane stress at the hole's edge. A diagnostic of the solve, labelled so; it never
    replaces a frozen prediction in the score."""
    preds = frozen(PREDICTIONS, PREDICTIONS_SHA256)["predictions"]
    tests = {t["id"]: t for t in json.loads(SETUPS.read_text())["scored_tests"]}
    out = {}
    for tid, p in sorted(preds.items()):
        if "oht" not in tid or "predicted_nominal_strength_mpa" not in p:
            continue
        m, g = p["model"], tests[tid]["geometry"]
        axis = tid.rsplit("-", 1)[1]  # the build direction of predict(): x -> z (the plate is the layer plane), y -> y, z -> x
        if axis == "x":
            ex, ey, nu, gxy, strength = m["E1"], m["E1"], NU, m["G12"], m["Xt"]
        elif axis == "y":
            ex, ey, nu, gxy, strength = m["E1"], m["E3"], NU, m["G13"], m["Xt"]
        else:  # pulled across the layers: nu_31 = nu_13 E3 / E1
            ex, ey, nu, gxy, strength = m["E3"], m["E1"], NU * m["E3"] / m["E1"], m["G13"], m["Zt"]
        kt, k_inf = kt_open_hole(ex, ey, nu, gxy, g["hole_diameter_mm"] / g["width_mm"])
        fe = p["predicted_nominal_strength_mpa"]
        out[tid] = {"closed_form_mpa": strength / kt, "kt_gross": kt, "kt_infinite_plate": k_inf, "fe_prediction_mpa": fe,
                    "fe_implied_kt_gross": strength / fe, "fe_over_closed_form": fe / (strength / kt) - 1}
        print("  %-20s closed form %7.3f MPa (Kt %.4f; infinite plate %.4f)  FE %7.3f MPa (implied Kt %.4f, %+.1f %%)" %
              (tid, strength / kt, kt, k_inf, fe, strength / fe, 100 * (fe / (strength / kt) - 1)))
    body = {"made": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "predictions_sha256": PREDICTIONS_SHA256, "sealed_opened": False,
            "what": "a labelled diagnostic: the open-hole groups in closed form (Lekhnitskii and Heywood) for the frozen predictions' "
                    "model, made before the sealed file was opened; the score uses the frozen predictions only",
            "open_hole": out}
    DIAGNOSTIC.write_text(json.dumps(body, indent=1, sort_keys=True))
    print("diagnostic written: %s (SHA-256 %s)" % (DIAGNOSTIC, hashlib.sha256(DIAGNOSTIC.read_bytes()).hexdigest()))
    return 0


def kind_of(tid):
    """the groups the score is read by"""
    if "oht" in tid:
        return "open-hole tension"
    if "z45" in tid:
        return "45 degrees to the layers"
    across = ("-z" in tid and "z45" not in tid) or "zflat" in tid or "upright" in tid
    return "bending, stress across the layers" if across else "bending, stress along the layers"


def median(v):
    v = sorted(v)
    return (v[(len(v) - 1) // 2] + v[len(v) // 2]) / 2 if v else math.nan


def score():
    """the frozen predictions against the sealed means, each file checked against its recorded SHA-256 first; the result
    goes to ~/.openphysicsai/g17-published/score.json, outside the repository, so that the measured values stay sealed"""
    preds = frozen(PREDICTIONS, PREDICTIONS_SHA256)["predictions"]
    diag = frozen(DIAGNOSTIC, DIAGNOSTIC_SHA256)["open_hole"]
    answers = frozen(SEALED, SEALED_SHA256)["answers"]
    rows = []
    for tid, p in sorted(preds.items()):
        if "predicted_nominal_strength_mpa" not in p:
            continue
        a = answers[tid]
        cens = a.get("censoring", {})
        censored = cens.get("specimens_marked_failure_not_observed", 0) + cens.get("specimens_marked_failure_after_5_percent_strain", 0)
        err = p["predicted_nominal_strength_mpa"] / a["mean_MPa"] - 1
        row = {"id": tid, "kind": kind_of(tid), "predicted_mpa": p["predicted_nominal_strength_mpa"], "measured_mean_mpa": a["mean_MPa"],
               "relative_error": err, "censored_specimens": censored}
        if tid in diag:
            row["diagnostic_closed_form_error"] = diag[tid]["closed_form_mpa"] / a["mean_MPa"] - 1
        rows.append(row)
        print("  %-40s %-34s %+6.1f %%%s%s" % (tid, row["kind"], 100 * err,
                                             "  (closed form, diagnostic: %+.1f %%)" % (100 * row["diagnostic_closed_form_error"]) if tid in diag else "",
                                             "  (%d specimen%s censored: a lower bound)" % (censored, "s" if censored > 1 else "") if censored else ""))
    errs = [r["relative_error"] for r in rows]
    by_kind = {}
    for r in rows:
        by_kind.setdefault(r["kind"], []).append(r["relative_error"])
    summary = {"groups": len(rows), "median_absolute_error": median([abs(e) for e in errs]), "within_25_percent": sum(abs(e) <= 0.25 for e in errs),
               "under": sum(e < 0 for e in errs), "range": [min(errs), max(errs)],
               "by_kind": {k: {"groups": len(v), "median_error": median(v), "range": [min(v), max(v)]} for k, v in sorted(by_kind.items())},
               "not_modelled": sorted(t for t, p in preds.items() if "predicted_nominal_strength_mpa" not in p)}
    body = {"made": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "predictions_sha256": PREDICTIONS_SHA256, "diagnostic_sha256": DIAGNOSTIC_SHA256,
            "sealed_sha256": SEALED_SHA256, "bar_for_information": 0.25, "summary": summary, "groups": rows}
    SCORE.write_text(json.dumps(body, indent=1, sort_keys=True))
    print("%d groups: median absolute error %.1f %% (bar, for information: 25 %%), %d within 25 %%, %d under the measured mean, range %+.1f to %+.1f %%" %
          (len(rows), 100 * summary["median_absolute_error"], summary["within_25_percent"], summary["under"], 100 * min(errs), 100 * max(errs)))
    for k, v in summary["by_kind"].items():
        print("  %-34s %2d groups, median %+.1f %%, range %+.1f to %+.1f %%" % (k, v["groups"], 100 * v["median_error"], 100 * v["range"][0],
                                                                            100 * v["range"][1]))
    print("score written: %s" % SCORE)
    return 0


if __name__ == "__main__":
    sys.exit({"predict": predict, "diagnose": diagnose, "score": score}.get((sys.argv[1:2] or [""])[0], lambda: 2)())
