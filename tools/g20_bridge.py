#!/usr/bin/env python3
"""g20_bridge.py - GOALS.md G20 step 3: the AM-Bench bridges' ridge deflection after the cut, predicted blind by the
lab's inherent-strain LPBF build (lpbf_build_run) through the MCP server as an agent drives it.

The rules are in GOALS.md (written before any bridge run): the 2018 IN625 bridge's ridge 1 fixes one isotropic inherent
strain (exx = eyy = ezz), read from the sealed file by a function that returns that entry alone; the 2022 IN718 bridge's
six ridges are the test, predicted with that strain. The elastic build is linear in the inherent strain (layers born
stress-free on the deformed part, the strain an eigenstrain, the cut a removal of elements), so the calibration needs
one run at a unit strain; the calibration then builds again at the calibrated strain and checks that it reproduces the
measurement to 1e-6, which is the linearity verified on the bridge itself.

Geometry: NIST's STL files. The cut keeps the part on the plate "before from_x", measured from the part's minimum x,
so the part is mirrored in x to put its attached end (the pointed base) at the low end; the ridges are found on the STL
(the facets at the top of the part) and numbered from the layout file a curator transcribed from NIST's figures
(~/.openphysicsai/g20/metal/bridge-layout.json, no result read).

    make openphysics-mcp
    python3 tools/g20_bridge.py geometry [STL]            # the profile and the ridges the driver sees
    python3 tools/g20_bridge.py smoke                     # one build at the unit strain, timed; no outcome read
    python3 tools/g20_bridge.py calibrate                 # the strain from the 2018 IN625 ridge 1, linearity checked
    python3 tools/g20_bridge.py predict                   # the 2022 IN718 ridges, frozen with their SHA-256
    python3 tools/g20_bridge.py score PREDICTIONS SHA256  # only once the hash is recorded in GOALS.md
"""
import hashlib
import json
import math
import shutil
import statistics
import struct
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402

G20 = Path.home() / ".openphysicsai" / "g20" / "metal"
SEALED = Path.home() / ".openphysicsai" / "sealed" / "g20-metal-outcomes.json"
SEALED_SHA = "694ae58a64cd3a6e6abf107666d88a94eb00f575fb3a8232054be1a11c9b4ec1"
STL_2018 = G20 / "sources" / "amb2018-01-cad" / "AMB2018_01_Part.STL"
STL_2022 = G20 / "sources" / "amb2022-01-cad" / "AMB2022-01-AMMT-PartCAD.STL"
LAYOUT = G20 / "bridge-layout.json"
CAL_CASE = "AMB2018-01-IN625-CBM-B1-P3"
TEST_CASE = "AMB2022-01-IN718-AMMT-B7-P2"
UNIT = -1e-3  # the unit inherent strain, isotropic (a contraction, the sign every LPBF calibration has)
LAYER_MM = 0.5  # the simulation layer and the element size (GOALS.md G20 step 3)
YS_2018 = (1.2, 2.5, 3.8)  # the results page's three CMM points across each ridge (its Figure 1, read by the curator)

# room-temperature elastic constants, each alloy's datasheet (setups.json materials, Special Metals' tables)
ELASTIC = {
    "IN625": {"E_pa": 207.5e9, "nu": 0.278, "density": 8440.0,
              "source": "Special Metals INCONEL 625 datasheet, annealed, 21 C (setups.json materials.IN625)"},
    "IN718": {"E_pa": 29.0e6 * 6894.757, "nu": 0.294, "density": 8190.0,
              "source": "Special Metals INCONEL 718 datasheet, 70 F (setups.json materials.IN718)"},
}


# ------------------------------------------------------------------------------------------------ geometry

def read_stl(path):
    """triangles of an ASCII or binary STL, as ((x, y, z) * 3) tuples in the file's units"""
    b = Path(path).read_bytes()
    tris = []
    if b[:5] == b"solid" and b"facet" in b[:400]:
        v = []
        for line in b.decode(errors="replace").splitlines():
            w = line.split()
            if w and w[0] == "vertex":
                v.append(tuple(float(x) for x in w[1:4]))
                if len(v) == 3:
                    tris.append(tuple(v))
                    v = []
        return tris
    n = struct.unpack("<I", b[80:84])[0]
    for i in range(n):
        o = 84 + 50 * i
        f = struct.unpack("<12f", b[o:o + 48])
        tris.append((f[3:6], f[6:9], f[9:12]))
    return tris


def write_stl(path, tris, name):
    with open(path, "wb") as f:
        f.write(name.encode()[:80].ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for a, b, c in tris:
            u = [b[k] - a[k] for k in range(3)]
            v = [c[k] - a[k] for k in range(3)]
            n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            ln = math.sqrt(sum(x * x for x in n)) or 1
            f.write(struct.pack("<3f", *[x / ln for x in n]))
            for p in (a, b, c):
                f.write(struct.pack("<3f", *p))
            f.write(b"\0\0")


def bounds(tris):
    pts = [p for t in tris for p in t]
    return [min(p[k] for p in pts) for k in range(3)], [max(p[k] for p in pts) for k in range(3)]


def normalised(tris):
    """the part with its minimum corner at the origin"""
    lo, _ = bounds(tris)
    return [tuple(tuple(p[k] - lo[k] for k in range(3)) for p in t) for t in tris]


def mirrored_x(tris):
    """x -> L - x, with the winding reversed so that the facets still face outward"""
    _, hi = bounds(tris)
    L = hi[0]
    return [((L - a[0], a[1], a[2]), (L - c[0], c[1], c[2]), (L - b[0], b[1], b[2])) for a, b, c in tris]


def as_built(tris):
    """the part with its attached end (the pointed base, a solid foot 19 mm long) at x = 0. NIST's 2018 STL has the square
    end at x = 0 and is mirrored; the 2022 CAD has the pointed end there already. Decided on the geometry: at the foot,
    13 mm from an end lies inside the base but in the gap between legs L3 and L4 from the other end"""
    _, hi = bounds(tris)
    L, yc, z = hi[0], 0.5 * hi[1] - 0.13, 0.25
    near, far = inside(tris, 13.0, yc, z), inside(tris, L - 13.0, yc, z)
    if near and not far:
        return tris
    if far and not near:
        return mirrored_x(tris)
    raise SystemExit("cannot tell the bridge's attached end (inside at 13 mm from each end: %s, %s)" % (near, far))


def ridges(tris):
    """the ridges (tabs) on top of the part: the x intervals of the facets that lie at the part's top, merged"""
    _, hi = bounds(tris)
    top = hi[2]
    iv = []
    for t in tris:
        if all(abs(p[2] - top) < 1e-4 for p in t):
            iv.append((min(p[0] for p in t), max(p[0] for p in t)))
    iv.sort()
    out = []
    for a, b in iv:
        if out and a <= out[-1][1] + 1e-6:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return [(a, b) for a, b in out]


def inside(tris, x, y, z):
    """ray parity along +x (Moller-Trumbore); the probe points avoid facet edges"""
    c = 0
    for a, b, cc in tris:
        e1 = [b[k] - a[k] for k in range(3)]
        e2 = [cc[k] - a[k] for k in range(3)]
        p = [0.0, -e2[2], e2[1]]
        det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2]
        if abs(det) < 1e-12:
            continue
        s = [x - a[0], y - a[1], z - a[2]]
        u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) / det
        if u < 0 or u > 1:
            continue
        q = [s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]]
        v = q[0] / det
        if v < 0 or u + v > 1:
            continue
        if (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det > 1e-9:
            c += 1
    return c % 2 == 1


def profile_rows(tris, heights, step=0.5):
    _, hi = bounds(tris)
    yc = 0.5 * hi[1] - 0.13
    rows = []
    for z in heights:
        rows.append("z %5.2f %s" % (z, "".join("#" if inside(tris, (i + 0.5) * step, yc, z) else "."
                                                for i in range(int(round(hi[0] / step))))))
    return rows


# ------------------------------------------------------------------------------------------------ the layout

def layout():
    if not LAYOUT.exists():
        raise SystemExit("no %s: the curator's transcription of the cut and the ridge numbering is needed" % LAYOUT)
    return json.loads(LAYOUT.read_text())


def part_frame(case_id):
    """the part's triangles from its NIST file (minimum corner at the origin), what geometry stands for it, and its
    length; as_built() then puts the attached base at x = 0, so that a ridge at X_sq sits at L - X_sq as built"""
    if case_id == TEST_CASE and STL_2022.exists():
        tris, geom = normalised(read_stl(STL_2022)), "NIST's 2022 part CAD (AMB2022-01-AMMT-PartCAD.STL, leg 10 hollow)"
    else:
        tris = normalised(read_stl(STL_2018))
        geom = ("NIST's 2018 part STL (AMB2018_01_Part.STL)" if case_id == CAL_CASE else
                "NIST's 2018 part STL standing for the 2022 part's external geometry, leg 10 solid (the 2022 CAD could not be fetched)")
    lo, hi = bounds(tris)
    if hi[0] < hi[1] or hi[0] < hi[2]:
        raise SystemExit("%s: the part's long axis is not x (%s)" % (case_id, hi))
    return tris, geom, hi[0]


# ------------------------------------------------------------------------------------------------ one build

def run_build(case_id, alloy, strain, cut, ridge_x_build, keep=False, label="", ys=(2.5,)):
    """one layer-by-layer build of the bridge with an isotropic inherent strain, then the cut. ridge_x_build: the ridge
    centres along the build's x (attached end at 0); ys: the points across the width (mm) at which each ridge's top is
    read, averaged as the measurement averages them. Returns u_z at each ridge before and after the cut."""
    tris, geom, L = part_frame(case_id)
    build = as_built(tris)
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-g20bridge-", dir="/tmp"))
    stl = tmp / "bridge.stl"
    write_stl(stl, build, "g20 bridge %s" % case_id)
    c = Client(["--embedded", "--workspace", str(tmp / "ws"), "--allow-read", str(tmp)])
    c.initialize()

    def call(op, args):
        r = c.call(op, args)["result"]["structuredContent"]
        if not r.get("ok"):
            raise SystemExit("%s %s: %s" % (case_id, op, json.dumps(r.get("error"))[:800]))
        return r.get("value", {})
    t0 = time.time()
    try:
        call("project_create", {"name": "g20_bridge"})
        body = call("geometry_import", {"path": str(stl), "units": "mm", "name": "bridge"})["body"]
        tr = body["placement"]["translation_mm"]
        el = ELASTIC[alloy]
        mid = alloy.lower() + "_g20"
        call("material_define", {"material": {
            "id": mid, "name": "%s, room-temperature datasheet elastic constants" % alloy, "family": "metal",
            "status": "user_supplied", "processes": ["lpbf"], "provenance": el["source"],
            "density_kg_m3": {"value": el["density"]}, "youngs_modulus_pa": {"value": el["E_pa"]},
            "poisson_ratio": {"value": el["nu"]}}})
        call("material_assign", {"body": "bridge", "material": mid, "source": "user"})
        call("mesh_generate", {"element_size": "%g mm" % LAYER_MM, "element_size_z": "%g mm" % LAYER_MM})
        args = {"body": "bridge", "build_orientation": "X", "layer_thickness_sim": "%g mm" % LAYER_MM,
                "inherent_strain": {"exx": strain, "eyy": strain, "ezz": strain, "provenance": cut["strain_provenance"],
                                    "source": cut["strain_source"]},
                "material": {"youngs_modulus": "%.6g MPa" % (el["E_pa"] / 1e6), "poissons_ratio": el["nu"], "provenance": "user"},
                "cut": {"height": "%g mm" % cut["height_mm"], "kerf": "%g mm" % cut["kerf_mm"], "from_x": "%g mm" % cut["from_x_mm"],
                        "provenance": cut["provenance"]},
                "label": label or case_id}
        run = call("lpbf_build_run", args)
        job = run["job_id"]
        st = {}
        for _ in range(360):
            st = call("job_status", {"job_id": job, "wait_seconds": 10})
            if st.get("state") not in ("queued", "running"):
                break
        if st.get("state") != "succeeded":
            raise SystemExit("%s: the build did not succeed: %s" % (case_id, json.dumps(st)[:800]))
        res = (st.get("summary") or {}).get("results") or {}
        n = res.get("stored_times")
        _, hi = bounds(build)
        zt = hi[2] - 0.25 * LAYER_MM  # inside the ridge's top element
        pts = [[tr[0] + x, tr[1] + y, tr[2] + zt] for x in ridge_x_build for y in ys]
        before = call("results_probe", {"job_id": job, "points_mm": pts, "time_index": n - 2})["probes"]
        after = call("results_probe", {"job_id": job, "points_mm": pts, "time_index": n - 1})["probes"]
        if not all(q.get("inside", True) for q in before + after):
            raise SystemExit("%s: a ridge probe lies outside the mesh" % case_id)
        k = len(ys)

        def mean(qs):
            return [sum(q["displacement_mm"]["z"] for q in qs[i * k:(i + 1) * k]) / k for i in range(len(ridge_x_build))]
        out = {"case_id": case_id, "geometry": geom, "strain": strain, "cut": cut, "points_mm": pts, "ys_mm": list(ys),
               "translation_mm": tr, "uz_before_mm": mean(before), "uz_after_mm": mean(after),
               "uz_points_after_minus_before_mm": [a["displacement_mm"]["z"] - b["displacement_mm"]["z"] for a, b in zip(after, before)],
               "summary": {k: res.get(k) for k in ("stored_times", "elements_cut", "tip_uz_before_cut_mm", "tip_uz_after_cut_mm",
                                                   "max_displacement_before_cut_mm", "equilibrium_error", "solves", "elements")},
               "seconds": round(time.time() - t0, 1)}
        out["deflection_mm"] = [a - b for a, b in zip(out["uz_after_mm"], out["uz_before_mm"])]
        if keep:
            out["workspace"] = str(tmp)
        return out
    finally:
        c.close()
        if not keep:
            shutil.rmtree(tmp, ignore_errors=True)


# ------------------------------------------------------------------------------------------------ the commands

def cmd_geometry(path=None):
    tris = normalised(read_stl(path or STL_2018))
    lo, hi = bounds(tris)
    print("%s: %d facets, %.3f x %.3f x %.3f mm" % (path or STL_2018, len(tris), hi[0], hi[1], hi[2]))
    print("ridges in the STL frame (x from, x to, mm):", ", ".join("%.2f-%.2f" % r for r in ridges(tris)))
    zs = sorted(set(round(p[2], 3) for t in tris for p in t))
    print("facet heights:", zs)
    for row in profile_rows(tris, [0.25, 2.5, 5.25, 6.25, 6.75, 9.5, hi[2] - 0.25]):
        print(row)
    b = as_built(tris)
    print("as built (attached end at x = 0%s): ridges" % (", mirrored" if b is not tris else ""),
          ", ".join("%.2f-%.2f" % r for r in ridges(b)))


def cmd_smoke():
    """a build of the 2018 bridge at the unit strain with an assumed cut, to time it and see it run; no outcome is read"""
    tris = normalised(read_stl(STL_2018))
    rx = [0.5 * (a + b) for a, b in ridges(as_built(tris))]
    cut = {"height_mm": 0.25, "kerf_mm": 0.3, "from_x_mm": 20.0, "provenance": "assumed",
           "strain_provenance": "user", "strain_source": "a unit strain to time the build, not a calibration"}
    r = run_build(CAL_CASE, "IN625", UNIT, cut, rx, label="smoke")
    print(json.dumps({k: r[k] for k in ("summary", "seconds")}, indent=1))
    for x, b, a, d in zip(rx, r["uz_before_mm"], r["uz_after_mm"], r["deflection_mm"]):
        print("  ridge at x %.2f (build frame): u_z before %.5f, after %.5f, deflection %.5f mm" % (x, b, a, d))


def bridge_setup(case_id):
    """the cut and the measured ridges of a bridge, from the curator's layout (bridge-layout.json): the ridge centres
    along the build's x (the part mirrored, x = L - X_sq, so that the attached base is at x 0 to 19 mm)"""
    lay = layout()
    tris, _, L = part_frame(case_id)
    if abs(L - 75.0) > 1e-6:
        raise SystemExit("%s: the part is %.3f mm long, not the 75 mm of the layout" % (case_id, L))
    # the cut: "as close to base plate as possible" (description Figure 8), so the bottom row of elements of every leg;
    # all 12 legs cut, the base (foot X_sq 56 to 75, x 0 to 19 as built) left on the plate
    cut = {"height_mm": 0.5 * LAYER_MM, "kerf_mm": 0.3, "from_x_mm": 20.0, "provenance": "inferred",
           "note": "the bottom element row of the 12 legs (cut as close to the plate as possible, no height stated; a kerf "
                   "anywhere inside that row removes the same elements); the base, foot X_sq 56 to 75, stays on the plate"}
    if case_id == CAL_CASE:
        rows = lay["AMB2018-01"]["ridges"]
        rmap = {str(r["ridge"]): L - r["center_X_sq_mm"] for r in rows}
        num = lay.get("AMB2018-01-results-page-numbering")
        if not num:
            raise SystemExit("the results page's ridge numbering is not in the layout yet (which ridge the sealed 'ridge 1' is)")
        # the sealed "ridge 1" is the results page's text's count from the square end: the free-end ridge, X_sq 0 to 1
        rmap["calibration"] = L - num["q2_physical_ridge_of_quoted_IN625_value"]["center_X_sq_mm"]
        return cut, rmap
    rows = lay["AMB2022-01"]["measured_ridges"]
    return cut, {str(r["ridge"]): L - r["center_X_sq_mm"] for r in rows}


def sealed_bytes():
    b = SEALED.read_bytes()
    if hashlib.sha256(b).hexdigest() != SEALED_SHA:
        raise SystemExit("the sealed metal file does not match the hash recorded in GOALS.md")
    return b


def calibration_entry():
    """the 2018 IN625 bridge's ridge-1 deflection, that entry alone (GOALS.md G20 step 3: the calibration case)"""
    e = json.loads(sealed_bytes())["cases"][CAL_CASE]["outcomes"]["ridge_deflection_ridge_1"]
    scale = {"mm": 1.0, "um": 1e-3}[e["unit"]]
    return e, e["value"] * scale


def cmd_calibrate():
    cut, rmap = bridge_setup(CAL_CASE)
    xc = rmap.pop("calibration")
    entry, measured = calibration_entry()
    print("the calibration entry, alone:", json.dumps(entry))
    print("its ridge, as the results page numbers it: x %.2f mm as built (X_sq %.2f)" % (xc, 75.0 - xc))
    u = run_build(CAL_CASE, "IN625", UNIT, dict(cut, strain_provenance="user", strain_source="the unit strain of the calibration"),
                  [xc], label="2018 IN625, unit strain", ys=YS_2018)
    d_unit = u["deflection_mm"][0]
    if abs(d_unit) < 1e-9:
        raise SystemExit("the calibration ridge does not move at the unit strain (%.3g mm): no strain can be calibrated" % d_unit)
    e_cal = UNIT * measured / d_unit
    print("the ridge at the unit strain %.4g: %.6f mm; measured %.6f mm; calibrated isotropic strain %.6g  [%.0f s]" %
          (UNIT, d_unit, measured, e_cal, u["seconds"]))
    # the linearity the calibration rests on, criterion written before the run: the build at the calibrated strain
    # reproduces the measured ridge to 1e-6 relative
    keys = sorted(rmap, key=int)
    v = run_build(CAL_CASE, "IN625", e_cal, dict(cut, strain_provenance="calibrated",
                                                 strain_source="G20 step 3: isotropic, on the 2018 IN625 bridge's free-end ridge"),
                  [xc] + [rmap[k] for k in keys], label="2018 IN625, calibrated", ys=YS_2018)
    d1 = v["deflection_mm"][0]
    lin = abs(d1 / measured - 1)
    print("at the calibrated strain the ridge is %.6f mm (%.2g relative from the measurement; criterion 1e-6): %s  [%.0f s]" %
          (d1, lin, "pass" if lin <= 1e-6 else "FAIL", v["seconds"]))
    for k, d in zip(keys, v["deflection_mm"][1:]):
        print("  ridge %2s (description Figure 8 numbering) x %5.2f as built: %.5f mm" % (k, rmap[k], d))
    out = {"what": "GOALS.md G20 step 3 calibration: one isotropic inherent strain from the 2018 IN625 bridge's measured ridge",
           "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "measured_mm": measured,
           "calibration_entry": entry, "calibration_ridge_x_build_mm": xc, "unit_strain": UNIT, "ridge_at_unit_mm": d_unit,
           "inherent_strain": e_cal, "linearity_relative": lin, "linearity_pass": lin <= 1e-6,
           "ridges_figure8_at_calibrated_strain_mm": dict(zip(keys, v["deflection_mm"][1:])),
           "unit_run": u, "calibrated_run": v}
    path = G20 / "bridge-calibration.json"
    path.write_text(json.dumps(out, indent=1) + "\n")
    print("written", path)
    return 0 if lin <= 1e-6 else 1


def cmd_predict():
    cal = json.loads((G20 / "bridge-calibration.json").read_text())
    if not cal.get("linearity_pass"):
        raise SystemExit("the calibration did not pass its linearity check")
    e = cal["inherent_strain"]
    cut, rmap = bridge_setup(TEST_CASE)
    keys = sorted(rmap, key=int)
    r = run_build(TEST_CASE, "IN718", e, dict(cut, strain_provenance="calibrated",
                                              strain_source="G20 step 3: isotropic, on the 2018 IN625 bridge's free-end ridge"),
                  [rmap[k] for k in keys], label="2022 IN718, predicted")
    pred = {"what": "GOALS.md G20 step 3 predictions: the 2022 IN718 bridge's ridge deflections after the cut",
            "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "case_id": TEST_CASE,
            "model": "lpbf_build_run, elastic, layer by layer at %g mm, isotropic inherent strain %.6g calibrated on %s's free-end "
                     "ridge (X_sq 0 to 1 mm, sealed as ridge 1)" % (LAYER_MM, e, CAL_CASE),
            "assumptions": ["the strain calibrated on IN625 on the EOS M270 at 20 um layers stands for IN718 on the AMMT at 40 um "
                            "layers (another alloy, machine and layer thickness)", r["geometry"],
                            "on 0.5 mm cells the hollow leg 10 keeps its outer walls and its two double-track walls (one cell each) "
                            "and loses its two single-track walls", "the part on a rigid plate; the cut the bottom element row of the "
                            "twelve legs (as close to the plate as possible)", "elastic: no plasticity"],
            "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "layout_sha256": hashlib.sha256(LAYOUT.read_bytes()).hexdigest(),
            "calibration": {k: cal[k] for k in ("measured_mm", "ridge_at_unit_mm", "inherent_strain", "linearity_relative")},
            "sealed_opened": False,
            "ridge_deflection_mm": dict(zip(keys, r["deflection_mm"])), "run": r}
    path = G20 / ("predictions-bridge-%s.json" % time.strftime("%Y%m%d-%H%M%S"))
    b = (json.dumps(pred, indent=1) + "\n").encode()
    path.write_bytes(b)
    for k in keys:
        print("  ridge %s  %.5f mm" % (k, pred["ridge_deflection_mm"][k]))
    print("frozen %s SHA-256 %s" % (path, hashlib.sha256(b).hexdigest()))
    return 0


def cmd_score(path, sha):
    b = Path(path).read_bytes()
    if hashlib.sha256(b).hexdigest() != sha:
        raise SystemExit("the predictions do not match the hash given")
    if sha not in (ROOT / "GOALS.md").read_text():
        raise SystemExit("the predictions' hash is not recorded in GOALS.md")
    pred = json.loads(b)
    o = json.loads(sealed_bytes())["cases"][TEST_CASE]["outcomes"]["ridge_deflection"]
    scale = {"mm": 1.0, "um": 1e-3}[o["unit"]]
    rows = []
    for k, p in sorted(pred["ridge_deflection_mm"].items(), key=lambda kv: int(kv[0])):
        m = o["values_by_ridge"][k] * scale
        rel = (p - m) / abs(m) if m else float("inf")
        rows.append({"ridge": k, "predicted_mm": p, "measured_mm": m, "relative_error": rel})
        print("  ridge %s  predicted %.5f mm  measured %.5f mm  error %+.1f %%" % (k, p, m, 100 * rel))
    med = statistics.median(abs(r["relative_error"]) for r in rows)
    print("median absolute error %.1f %% (criterion 15 %%): %s; expanded uncertainty of a measurement (k = 2) %s mm" %
          (100 * med, "pass" if med <= 0.15 else "fail", o.get("expanded_uncertainty_mm_k2")))
    out = {"predictions": str(path), "predictions_sha256": sha, "rows": rows, "median_absolute_relative_error": med,
           "criterion": 0.15, "pass": med <= 0.15, "expanded_uncertainty_mm_k2": o.get("expanded_uncertainty_mm_k2"),
           "scored_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    sp = G20 / ("score-" + Path(path).name)
    sp.write_text(json.dumps(out, indent=1) + "\n")
    print("written", sp)
    return 0


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    if a[0] == "geometry":
        cmd_geometry(a[1] if len(a) > 1 else None)
    elif a[0] == "smoke":
        cmd_smoke()
    elif a[0] == "calibrate":
        return cmd_calibrate()
    elif a[0] == "predict":
        return cmd_predict()
    elif a[0] == "score" and len(a) == 3:
        return cmd_score(a[1], a[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
