#!/usr/bin/env python3
"""g20_warp.py - GOALS.md G20 step 4: printed plastic parts' warp and residual strain predicted blind by the lab's
fff_print simulation, through the MCP server as an agent drives it.

The rules are in GOALS.md (written before any run): one relaxation offset per study fixed on its calibration case,
materials from the library and the studies' own values, the quantities as each study measured them. The set-ups and the
calibration data are read from ~/.openphysicsai/g20/fff-process/; the test outcomes stay sealed until the predictions
are frozen and their SHA-256 recorded in GOALS.md.

    make openphysics-mcp
    python3 tools/g20_warp.py run CASE_ID [--offset K]      # one case, its quantity
    python3 tools/g20_warp.py calibrate                      # the offset of each study on its calibration case
    python3 tools/g20_warp.py predict                        # every predicted test case, frozen with its SHA-256
    python3 tools/g20_warp.py score PREDICTIONS.json SHA256  # only once the hash is recorded in GOALS.md
"""
import hashlib
import json
import math
import shutil
import statistics
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402
from breaks import arc, extrude_stl  # noqa: E402

G20 = Path.home() / ".openphysicsai" / "g20" / "fff-process"
SEALED = Path.home() / ".openphysicsai" / "sealed" / "g20-fff-process-outcomes.json"
G20R2 = Path.home() / ".openphysicsai" / "g20" / "fff-process-r2"  # round two (GOALS.md G20 steps 7 and 8)
SEALED_R2 = Path.home() / ".openphysicsai" / "sealed" / "g20-fff-process-r2-outcomes.json"
SEALED_R2_SHA = "068a7e81cac173896f1a290363d674dd18dab00665aafd3e1645accf433a7803"
ARM18_SPEED_MM_S, ARM18_ROAD_MM = 20.0, 0.5  # not stated by the source: assumptions, labelled with the predictions
SEALED_SHA = "a1235964ad5978ed352b3609bf84f0ad89b529935bc30f747aed21475758ab88"
PREDICTED_STUDIES = ("SYR21", "ALR22", "MAT24", "FAR25")
NOT_PREDICTED = {
    "RAM21": "printed with 20 % infill: the process simulation models solid parts (GOALS.md G20 step 4)",
    "FAN20": "polycarbonate: no sourced expansion or thermal data in the set or the library",
    "MAT24-PA": "PA: no specific heat or conductivity in the set or the library",
    "MAT24-CFPA": "carbon-filled PA: no specific heat or conductivity in the set or the library",
    "MAT24-PEBA": "PEBA: no modulus, specific heat or conductivity in the set or the library",
    "SYR21-MM": "ABS and HIPS in one part: the process simulation prints one material",
}


def load():
    return json.loads((G20 / "setups.json").read_text()), json.loads((G20 / "calibration.json").read_text())


def lib(mid):
    d = json.loads((ROOT / "src/ctl/materials.json").read_text())
    return next(r for r in (d["materials"] if isinstance(d, dict) else d) if r["id"] == mid)


def refusal(case):
    cid = case["case_id"]
    for key, why in NOT_PREDICTED.items():
        if cid.startswith(key):
            return why
    return None


# ------------------------------------------------------------------------------------------------ geometry (mm)

def rect(L, W):
    return [(0, 0), (L, 0), (L, W), (0, W)]


def _half_outline(L, wg, wn, ln, arcs):
    """the upper edge of a flat tensile specimen, left to right: grip, transition, narrow section, transition, grip.
    arcs: the transition as (radius, sign) pieces from the grip down to the narrow edge, all tangent; sign +1 turns down
    (centre inside the part), -1 turns back to level (centre outside). Returns the points and the transition's length."""
    dw = 0.5 * (wg - wn)
    rs = sum(r for r, _ in arcs)
    t = math.acos(1 - dw / rs) if len(arcs) > 1 else None
    pts = []
    if len(arcs) == 1:  # one fillet, tangent to the narrow edge, meeting the grip edge at an angle
        R = arcs[0][0]
        fx = math.sqrt(R * R - (R - dw) ** 2)
        xs = 0.5 * (L - ln)
        cx, cy = xs, 0.5 * wn + R
        a0 = math.atan2(0.5 * wg - cy, (xs - fx) - cx)
        left = [(0.0, 0.5 * wg)] + [(cx + R * math.cos(a0 + (-0.5 * math.pi - a0) * k / 16), cy + R * math.sin(a0 + (-0.5 * math.pi - a0) * k / 16))
                                    for k in range(17)]
        trans = fx
    else:  # a reverse curve: R2 (centre inside) from the grip edge, then R1 (centre outside) onto the narrow edge
        R2, R1 = arcs[0][0], arcs[1][0]
        trans = rs * math.sin(t)
        xs = 0.5 * (L - ln)
        xg = xs - trans
        left = [(0.0, 0.5 * wg), (xg, 0.5 * wg)]
        left += [(xg + R2 * math.sin(t * k / 12), 0.5 * wg - R2 + R2 * math.cos(t * k / 12)) for k in range(1, 13)]
        left += [(xs + R1 * math.cos(-0.5 * math.pi - t * (1 - k / 12)), 0.5 * wn + R1 + R1 * math.sin(-0.5 * math.pi - t * (1 - k / 12)))
                 for k in range(1, 13)]
    right = [(L - x, y) for x, y in reversed(left)]
    return left + right, trans


def tensile_outline(L, wg, wn, ln, arcs):
    """the whole outline, counter-clockwise, y from 0 to wg"""
    upper, trans = _half_outline(L, wg, wn, ln, arcs)
    lower = [(x, -y) for x, y in reversed(upper)]
    poly = [(x, y + 0.5 * wg) for x, y in lower + upper]
    out = []
    for q in poly:  # drop repeated points where the pieces meet
        if not out or abs(q[0] - out[-1][0]) > 1e-9 or abs(q[1] - out[-1][1]) > 1e-9:
            out.append(q)
    return out, trans


def astm_d638_type1():
    """ASTM D638 type I: 165 long, grips 19 wide, the narrow section 13 wide and 57 long, R76 fillets"""
    return tensile_outline(165.0, 19.0, 13.0, 57.0, [(76.0, 1)])[0]


def syr21_dogbone():
    """SYR21's specimen as its set-up states it (Figure 5a): 120 long, grips 19 wide, the narrow section 6 wide and 33
    long, joined by arcs of 14 (from the grip) and 25 mm (onto the narrow section)"""
    return tensile_outline(120.0, 19.0, 6.0, 33.0, [(14.0, 1), (25.0, -1)])[0]


def polygon_area(poly):
    return 0.5 * abs(sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1] for i in range(len(poly))))


def bridge_profile():
    """TRO22's bridge in its side view (x along the deck, h up): 15 supports 2 mm long with 4 mm gaps under a 2 mm deck"""
    pts = []
    for i in range(15):
        x0 = 6.0 * i
        pts += [(x0, 0.0), (x0 + 2.0, 0.0)]
        if i < 14:
            pts += [(x0 + 2.0, 4.0), (x0 + 6.0, 4.0)]
    pts += [(86.0, 6.0), (0.0, 6.0)]
    return pts


def write_side_extrusion(path, prof, width, name):
    """an STL of a side profile (x, h) extruded across y from 0 to width, outward facets"""
    import struct
    from breaks import area2, clean, triangulate
    prof = clean(prof)
    if area2(prof) < 0:
        prof = prof[::-1]
    tris = []
    for i in range(len(prof)):
        (x0, h0), (x1, h1) = prof[i], prof[(i + 1) % len(prof)]
        a, b, c, d = (x0, 0, h0), (x1, 0, h1), (x1, width, h1), (x0, width, h0)
        tris += [(a, c, b), (a, d, c)]  # the profile's walls: (x, h) counter-clockwise seen from -y, so wound for outward normals
    for i0, i1, i2 in triangulate(prof):
        p0, p1, p2 = prof[i0], prof[i1], prof[i2]
        tris.append(((p0[0], 0, p0[1]), (p1[0], 0, p1[1]), (p2[0], 0, p2[1])))          # the face at y = 0 (normal -y)
        tris.append(((p0[0], width, p0[1]), (p2[0], width, p2[1]), (p1[0], width, p1[1])))  # at y = width (normal +y)
    with open(path, "wb") as f:
        f.write(name.encode()[:80].ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            u = [t[1][k] - t[0][k] for k in range(3)]
            v = [t[2][k] - t[0][k] for k in range(3)]
            n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            ln = math.sqrt(sum(q * q for q in n)) or 1
            f.write(struct.pack("<3f", *[q / ln for q in n]))
            for q in t:
                f.write(struct.pack("<3f", *q))
            f.write(b"\0\0")


def outline(case):
    g = case["geometry"]
    cid = case["case_id"]
    if cid.startswith("SYR21"):
        return syr21_dogbone(), g["thickness_mm"]
    if cid.startswith("ALR22") or cid.startswith("FAR25"):
        return astm_d638_type1(), g["thickness_mm"]
    if cid.startswith("MAT24"):
        return rect(g["length_mm"], g["width_mm"]), g["height_mm"]
    if cid.startswith("TRO22"):
        return rect(g["deck_length_x2_mm"], g["deck_width_x1_mm"]), g["overall_height_x3_mm"]  # the footprint; the STL is the bridge
    if cid.startswith("ARM18"):
        return rect(g["length_l_mm"], g["width_w_mm"]), g["height_h_mm"]
    raise SystemExit("%s: no geometry" % cid)
# ------------------------------------------------------------------------------------------------ materials (SI)

def abs_record(case, setups, calib):
    """ABS as GOALS.md G20 step 4 states it: the library's abs_fff, the study's own values where it gives them, Sun et
    al.'s specific heat via KOU17, STR24's modulus curve of a printed bar"""
    L = lib("abs_fff")
    mp = calib["material_properties"]
    rho, nu, tg, alpha = L["density_kg_m3"]["value"], 0.35, L["glass_transition_c"]["value"], L["expansion_1_per_k"]["value"]
    src = ["abs_fff (density, conductivity, expansion, glass transition)"]
    sid = case["source_id"]
    if sid == "SYR21":
        own = {e["property"]: e["value"] for e in mp["ABS_Bestfilament"] if "value" in e}
        rho, nu, tg, alpha = own["density_kg_per_m3"], own["poisson_ratio"], own["glass_transition_C"], own["cte_per_C"]
        src.append("SYR21 Table 1: density, Poisson's ratio, glass transition, expansion")
    if sid == "ARM18":  # round two: ABSplus-P430, its data sheet's glass transition and expansion (round two's calibration.json)
        r2 = json.loads((G20R2 / "calibration.json").read_text())["materials"]["ABSplus_P430"]["properties"]
        own = {e["property"]: e["value"] for e in r2}
        tg, alpha = own["glass_transition_C"], own["cte_per_C"]
        src.append("ABSplus-P430 data sheet via ARM18's set-up: glass transition 108 C, expansion 8.82e-5 /K")
    if sid == "MAT24":
        e = mp["ABS_eSUN_gray_printed"]
        ctes = [x for x in e if x["property"] == "cte_per_C"]
        raster = "90" if case["case_id"].endswith("-90") else "0"
        alpha = next(x["value"] for x in ctes if x.get("raster", "").startswith(raster))
        tg = next(x["value"] for x in e if x["property"] == "softening_point_as_glass_transition_C")
        src.append("MAT24 Table 2: expansion along the fibre (raster %s), softening point as the glass transition" % raster)
    k = statistics.mean(L["conductivity_w_per_mk"]["value"]) if isinstance(L["conductivity_w_per_mk"]["value"], list) else L["conductivity_w_per_mk"]["value"]
    cp = next(x["value"] for x in mp["ABS_generic_Sun2008_via_KOU17"] if x["property"] == "specific_heat_J_per_kgK")
    curve = next(x for x in mp["ABS_PrusaResearch_white_printed"] if x["property"] == "storage_modulus_vs_temperature_MPa")["data_C_MPa"]
    tc = [t for t, _ in curve] + [150.0, 260.0]
    E = [1e6 * v for _, v in curve] + [1e6 * curve[-1][1] * 0.01, 1e6 * curve[-1][1] * 0.01]  # above STR24's 112 C: 1 % of its last value
    src.append("specific heat 2080 J/(kg K) (Sun et al. 2008 via KOU17); modulus against temperature: STR24 Figure 20 to 112 C, then 1 %% of "
               "its last value (molten)")
    return {
        "id": "g20_abs_%s" % case["case_id"].lower().replace("-", "_"), "name": "ABS for G20 step 4 (%s)" % case["case_id"], "family": "polymer",
        "status": "user_supplied", "provenance": "; ".join(src),
        "density_kg_m3": {"value": rho}, "poisson_ratio": {"value": nu}, "expansion_1_per_k": {"value": alpha},
        "conductivity_w_per_mk": {"value": k}, "specific_heat_j_per_kgk": {"value": cp}, "emissivity": {"value": 0.9},
        "youngs_modulus_pa": {"t_c": tc, "value": E}, "glass_transition_c": {"value": tg},
    }


def pla_record(case, setups, calib):
    """PLA from FAR25's temperature table, with MAT24's expansion and softening point for its blocks"""
    mp = calib["material_properties"]
    t = next(x for x in mp["PLA_Prusament_literature_table_FAR25"] if x["property"] == "temperature_dependent_properties")
    rows = t["rows"]
    tc = [r[0] for r in rows]
    src = ["FAR25 Table 3 (modulus, Poisson's ratio, expansion, conductivity, specific heat, density against temperature)"]
    alpha = {"t_c": tc, "value": [r[4] * 1e-5 for r in rows]}
    tg = lib("pla_fff")["glass_transition_c"]["value"]  # FAR25's table gives no glass transition
    src.append("glass transition 62.8 C (library pla_fff)")
    if case["source_id"] == "TRO22":
        tg = 65.0
        src.append("glass transition about 65 C, the literature value of Trofimov et al. 2022 (Table 1)")
    if case["source_id"] == "MAT24":
        e = mp["PLA_PM_white_printed"]
        raster = "90" if case["case_id"].endswith("-90") else "0"
        a = next(x["value"] for x in e if x["property"] == "cte_per_C" and x.get("raster", "").startswith(raster))
        alpha = {"value": a}
        sp = next(x["value"] for x in e if x["property"] == "softening_point_as_glass_transition_C")
        tg = sp["%sdeg" % raster] if isinstance(sp, dict) else sp
        src.append("MAT24 Table 2: expansion along the fibre (raster %s) and its softening point as the glass transition" % raster)
    k = [max(r[5], 0.11) for r in rows]  # FAR25 prints k = 0 above 200 C; its last nonzero value is kept there
    return {
        "id": "g20_pla_%s" % case["case_id"].lower().replace("-", "_"), "name": "PLA for G20 step 4 (%s)" % case["case_id"], "family": "polymer",
        "status": "user_supplied", "provenance": "; ".join(src) + "; conductivity above 200 C held at 0.11 W/(m K) where FAR25 prints 0",
        "density_kg_m3": {"t_c": tc, "value": [r[7] for r in rows]}, "poisson_ratio": {"value": rows[0][2]},
        "expansion_1_per_k": alpha, "conductivity_w_per_mk": {"t_c": tc, "value": k},
        "specific_heat_j_per_kgk": {"t_c": tc, "value": [r[6] for r in rows]}, "emissivity": {"value": 0.9},
        "youngs_modulus_pa": {"t_c": tc, "value": [r[1] * 1e6 for r in rows]}, "glass_transition_c": {"value": tg},
    }


def material(case, setups, calib):
    return pla_record(case, setups, calib) if "PLA" in case.get("material_id", "PLA") else abs_record(case, setups, calib)


# ------------------------------------------------------------------------------------------------ one print through MCP

LAYER_MODE = "rule"  # "printed": every printed layer a simulation layer (a calibration exploration, GOALS.md G20 step 4)


def layers(case):
    """the cells and the simulation layer: cells at most 0.5 mm high, a whole number of them through the part; the
    simulation layer the multiple of the cell nearest 1 mm, or the printed layer when the part has fewer than five"""
    _, t = outline(case)
    lh = case["process"].get("layer_height_mm") or 0.2
    if t / lh < 5 or LAYER_MODE == "printed":
        n = max(1, round(t / lh))
        return t / n, t / n
    n = math.ceil(t / 0.5 - 1e-9)
    hz = t / n
    k = max(1, round(1.0 / hz))
    return hz, k * hz


def run_case(case, setups, calib, offset, keep=False, tau_g=0.0, t_act=0.0, gc=0.0, hot=False, consistent=False):
    """the print of one case: returns the released shape's bottom heights along the centre line and the run's summary"""
    poly, t = outline(case)
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-g20warp-", dir="/tmp"))
    stl = tmp / "part.stl"
    if case["case_id"].startswith("TRO22"):
        write_side_extrusion(stl, bridge_profile(), case["geometry"]["deck_width_x1_mm"], "g20 %s" % case["case_id"])
    else:
        extrude_stl(stl, poly, t, "g20 %s" % case["case_id"])
    c = Client(["--embedded", "--workspace", str(tmp / "ws"), "--allow-read", str(tmp)])
    c.initialize()

    def call(op, args):
        r = c.call(op, args)["result"]["structuredContent"]
        if not r.get("ok"):
            raise SystemExit("%s %s: %s" % (case["case_id"], op, json.dumps(r.get("error"))[:600]))
        return r.get("value", {})
    try:
        call("project_create", {"name": "g20_" + case["case_id"].lower().replace("-", "_")})
        body = call("geometry_import", {"path": str(stl), "units": "mm", "name": "part"})["body"]
        tr = body["placement"]["translation_mm"]
        rec = material(case, setups, calib)
        call("material_define", {"material": rec})
        call("material_assign", {"body": "part", "material": rec["id"], "source": "user"})
        hz, layer = layers(case)
        call("mesh_generate", {"element_size": "0.5 mm" if case["source_id"] == "TRO22" else "1 mm", "element_size_z": "%.6g mm" % hz})
        pr = case["process"]
        if case["source_id"] == "ARM18":  # round two: the 75 C chamber is the bed and the ambient; speed and road assumed
            pr = dict(pr, nozzle_temperature_C=pr["extrusion_temperature_C"], bed_temperature_C=pr["chamber_temperature_C"],
                      layer_height_mm=pr["layer_thickness_mm"], line_width_mm=ARM18_ROAD_MM, print_speed_mm_per_s=ARM18_SPEED_MM_S)
        lw = pr.get("line_width_mm") or 0.4
        lh = pr.get("layer_height_mm") or 0.2
        amb = {"SYR21": 22.0, "ALR22": 25.0, "MAT24": 23.0, "FAR25": 20.0, "TRO22": 60.0,
               "ARM18": pr.get("chamber_temperature_C")}[case["source_id"]]
        speed = pr.get("print_speed_mm_per_s") or {"TRO22": 47.0}[case["source_id"]]  # TRO22: from its calibration infrared histories
        proc = {"layer_height": "%.6g mm" % layer, "printed_layer_height": "%.6g mm" % lh,
                "nozzle_temperature": "%g degC" % pr["nozzle_temperature_C"], "bed_temperature": "%g degC" % pr["bed_temperature_C"],
                "ambient_temperature": "%g degC" % amb, "convection": "15 W/(m^2 K)",
                "deposition_rate": "%.6g mm^3/s" % (speed * lh * lw), "relaxation_offset": "%g K" % offset,
                "relaxation_time_at_tg": "%.9g s" % tau_g, "provenance": "inferred"}
        if t_act > 0:  # round two (GOALS.md G20 step 8): the glass's Arrhenius shift below Tg
            proc["relaxation_activation_temperature"] = "%.9g K" % t_act
        if gc > 0:  # round two: the bed that lets go
            proc["bed_toughness"] = "%.9g J/m^2" % gc
        if consistent:  # round two: the consistent stress update
            proc["stress_update"] = "consistent"
        if case["source_id"] == "TRO22":  # one minute on the bed, then released (GOALS.md G20 step 4, the bridge)
            proc.update({"cooldown_bed_on": "60 s", "cooldown_bed_off": "0 s"})
        if hot:  # round two's check of the removal: released straight after the print, then an hour off the bed
            proc.update({"cooldown_bed_on": "0 s", "cooldown_bed_off": "0 s", "cooldown_released": "3600 s"})
        if case["source_id"] == "ARM18":  # extracted hot: five minutes at the chamber temperature, then released
            proc.update({"cooldown_bed_on": "300 s", "cooldown_bed_off": "0 s"})
        run = call("mech_print_run", {"process": proc, "label": case["case_id"]})
        job = run["job_id"]
        st = {}
        for _ in range(1080):  # up to three hours: a peel near the bond's toughness advances one cell per solve
            st = call("job_status", {"job_id": job, "wait_seconds": 10})
            if st.get("state") not in ("queued", "running"):
                break
        if st.get("state") != "succeeded":
            raise SystemExit("%s: the print did not succeed: %s" % (case["case_id"], json.dumps(st)[:600]))
        # the bottom face along the centre line, 41 points, at the released (last) stored time
        xs = [min(p[0] for p in poly), max(p[0] for p in poly)]
        yc = 0.5 * (min(p[1] for p in poly) + max(p[1] for p in poly))
        npts = 41
        z_probe = (t - 0.25 * hz) if case["source_id"] == "TRO22" else 0.25 * hz  # TRO22: the deck's top surface, as scanned
        pts = [[tr[0] + xs[0] + 0.5 + (xs[1] - xs[0] - 1.0) * i / (npts - 1), tr[1] + yc, tr[2] + z_probe] for i in range(npts)]
        pv = call("results_probe", {"job_id": job, "points_mm": pts})
        out = {"probe": pv, "points_mm": pts, "summary": st.get("summary"), "layer_mm": layer, "cell_z_mm": hz, "material": rec,
               "process": proc, "translation_mm": tr}
        if case["source_id"] == "ARM18":  # the top face on a grid, 1 mm inside its edges, for its flatness
            x0, x1 = min(p[0] for p in poly), max(p[0] for p in poly)
            y0, y1 = min(p[1] for p in poly), max(p[1] for p in poly)
            nx, ny = 29, max(5, int(round(28 * (y1 - y0) / (x1 - x0))) + 1)
            top = [[tr[0] + x0 + 1 + (x1 - x0 - 2) * i / (nx - 1), tr[1] + y0 + 1 + (y1 - y0 - 2) * j / (ny - 1), tr[2] + t - 0.25 * hz]
                   for i in range(nx) for j in range(ny)]
            tq = []
            for k in range(0, len(top), 64):  # the probe takes 64 points at a time
                tq += call("results_probe", {"job_id": job, "points_mm": top[k:k + 64]})["probes"]
            out["top"] = [(p[0], p[1], p[2] + q["displacement_mm"]["z"]) for p, q in zip(top, tq) if q.get("inside")]
        if case["source_id"] == "MAT24":
            # the fibre on top of layer 21 along the 40 mm axis, at the block's middle; its 3 mm grating centred (an assumption)
            g = case["geometry"]
            zf = 21 * (pr.get("layer_height_mm") or 0.2)
            cx, cy = 0.5 * g["length_mm"], 0.5 * g["width_mm"]
            centre = [tr[0] + cx, tr[1] + cy, tr[2] + zf]
            ends = [[tr[0] + cx - 1.5, tr[1] + cy, tr[2] + zf], [tr[0] + cx + 1.5, tr[1] + cy, tr[2] + zf]]
            h = call("results_probe", {"job_id": job, "points_mm": [centre], "history": True})["probes"][0]["history"]
            trelax_c = rec["glass_transition_c"]["value"] + offset
            hot = [i for i, row in enumerate(h) if row.get("temperature_c", -1e9) >= trelax_c]
            i_bond = min(hot[-1] + 1, len(h) - 1) if hot else 0
            a = call("results_probe", {"job_id": job, "points_mm": ends, "time_index": i_bond})["probes"]
            b = call("results_probe", {"job_id": job, "points_mm": ends, "time_index": len(h) - 1})["probes"]
            dux = [b[k]["displacement_mm"]["x"] - a[k]["displacement_mm"]["x"] for k in range(2)]
            out["fbg"] = {"bond_index": i_bond, "bond_time_s": h[i_bond]["time_s"], "relaxation_c": trelax_c,
                          "strain": (dux[1] - dux[0]) / 3.0, "centre_mm": centre}
        return out
    finally:
        c.close()
        if not keep:
            shutil.rmtree(tmp, ignore_errors=True)


def bottom_profile(r):
    """the released bottom face's heights along the centre line (mm): z + u_z at each probe that lies in the part"""
    out = []
    for pt, q in zip(r["points_mm"], r["probe"]["probes"]):
        if q.get("inside"):
            out.append((pt[0], pt[2] + q["displacement_mm"]["z"]))
    return out


def end_lift_mm(prof):
    """a bar lying on a flat surface: the mean height of its two ends above its lowest point"""
    zmin = min(z for _, z in prof)
    return 0.5 * (prof[0][1] + prof[-1][1]) - zmin


def largest_lift_mm(prof):
    zmin = min(z for _, z in prof)
    return max(z for _, z in prof) - zmin


def quantity(case, r):
    """the predicted value in the unit of the case's measurement"""
    prof = bottom_profile(r)
    unit = case["measurement"]["unit"]
    if case["source_id"] in ("SYR21", "ALR22"):
        v = end_lift_mm(prof)
        return v * 1e3 if unit == "um" else v
    if case["source_id"] == "FAR25":
        return largest_lift_mm(prof)
    if case["source_id"] == "MAT24":
        return r["fbg"]["strain"] * 1e6  # in 1e-6, contraction negative
    if case["source_id"] == "ARM18":  # the top face's flatness: the range of its heights about their least-squares plane
        P = r["top"]
        A = [[0.0] * 4 for _ in range(3)]
        for x, y, z in P:
            f = (1.0, x, y)
            for i in range(3):
                for j in range(3):
                    A[i][j] += f[i] * f[j]
                A[i][3] += f[i] * z
        for c in range(3):
            for i in range(3):
                if i != c:
                    q = A[i][c] / A[c][c]
                    for j in range(4):
                        A[i][j] -= q * A[c][j]
        a, b, cc = (A[i][3] / A[i][i] for i in range(3))
        res = [z - (a + b * x + cc * y) for x, y, z in P]
        return max(res) - min(res)
    if case["source_id"] == "TRO22":  # the camber of the deck's middle line: from the chord through its ends, middle above positive
        (x0, z0), (x1, z1) = prof[0], prof[-1]
        dev = [z - (z0 + (z1 - z0) * (x - x0) / (x1 - x0)) for x, z in prof]
        return max(dev, key=abs)
    raise SystemExit("%s: no quantity yet" % case["case_id"])


def calibration_value(calib, cid):
    """the measured value of a calibration case, in its measurement's unit"""
    c = calib["calibration_cases"][cid]
    if "value" in c and c["value"] is not None:
        return c["value"]
    for k, v in c.items():
        if isinstance(v, dict) and "value" in v and k.startswith(("warpage", "end_lift", "lift", "residual_strain", "curvature")):
            return v["value"]
    raise SystemExit("%s: no measured value found in calibration.json" % cid)


def cmd_calibrate():
    """each study's relaxation time at the glass transition, on its calibration case, by bisection in its logarithm
    (the predicted warp falls as the time grows); the relaxation offset at the operation's default 10 K"""
    setups, calib = load()
    out = {"made_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "offset_K": 10.0, "studies": {},
           "rule": "GOALS.md G20 step 4 as amended before any test prediction: one relaxation time at the glass transition per study, on its "
                   "calibration case"}
    for sid in ("SYR21", "ALR22"):
        cid = next(c["case_id"] for c in setups["cases"] if c["source_id"] == sid and c["split"] == "calibration")
        case = next(c for c in setups["cases"] if c["case_id"] == cid)
        want = calibration_value(calib, cid)
        lo, hi = -14.0, 6.0  # log10 of the time in s: short times warp more
        runs = []

        def H(lt):
            r = run_case(case, setups, calib, 10.0, tau_g=10 ** lt)
            v = quantity(case, r)
            runs.append((lt, v))
            print("  %s log10 tau_g %+7.3f: %s %.5g %s (measured %.5g)" % (cid, lt, case["measurement"]["quantity"], v, case["measurement"]["unit"], want),
                  flush=True)
            return v
        h_lo, h_hi = H(lo), H(hi)
        if not (h_lo >= want >= h_hi):
            out["studies"][sid] = {"case": cid, "measured": want, "reached": False, "runs": runs,
                                   "note": "no relaxation time between 1e-14 and 1e6 s reproduces the calibration case"}
            print("  %s: the measurement lies outside what the model reaches (%.4g to %.4g)" % (sid, h_hi, h_lo))
            continue
        for _ in range(9):
            mid = 0.5 * (lo + hi)
            if H(mid) > want:
                lo = mid
            else:
                hi = mid
        lt = 0.5 * (lo + hi)
        out["studies"][sid] = {"case": cid, "measured": want, "reached": True, "log10_tau_g_s": lt, "tau_g_s": 10 ** lt, "runs": runs}
        print("  %s: tau_g = %.3g s (log10 %+.3f)" % (sid, 10 ** lt, lt))
    got = [v["log10_tau_g_s"] for v in out["studies"].values() if v.get("reached")]
    if got:
        out["median_log10_tau_g_s"] = statistics.median(got)
    (G20 / "warp-calibration.json").write_text(json.dumps(out, indent=1) + "\n")
    print("  written: %s" % (G20 / "warp-calibration.json"))


VARIABLES = ("layer_height_mm", "bed_temperature_C", "nozzle_temperature_C", "chamber_or_ambient_temperature_C", "raster_angles_deg")


def signature(case):
    """the stated variables a pair may differ in: the process's, the geometry's dimensions and the material"""
    pr = case["process"]
    sig = {k: json.dumps(pr.get(k)) for k in VARIABLES}
    sig.update({"geometry:" + k: json.dumps(v) for k, v in case["geometry"].items() if k.endswith("_mm")})
    sig["material"] = case["material_id"]
    return sig


def cmd_predict():
    setups, calib = load()
    cal = json.loads((G20 / "warp-calibration.json").read_text())
    med = cal.get("median_log10_tau_g_s")
    preds, refused = {}, {}
    for case in setups["cases"]:
        if case["split"] != "test":
            continue
        why = refusal(case)
        if why or case["source_id"] not in PREDICTED_STUDIES:
            refused[case["case_id"]] = why or "not a predicted study"
            continue
        st = cal["studies"].get(case["source_id"])
        lt = st["log10_tau_g_s"] if st and st.get("reached") else med
        src = "its calibration case" if st and st.get("reached") else "the median of the studies calibrated"
        t0 = time.time()
        r = run_case(case, setups, calib, 10.0, tau_g=10 ** lt)
        v = quantity(case, r)
        preds[case["case_id"]] = {"value": v, "unit": case["measurement"]["unit"], "quantity": case["measurement"]["quantity"],
                                  "log10_tau_g_s": lt, "tau_g_from": src, "material": r["material"]["provenance"],
                                  "process": r["process"], "fbg": r.get("fbg"), "seconds": time.time() - t0}
        print("  %-14s %s %.5g %s (tau_g from %s)  [%.0f s]" % (case["case_id"], case["measurement"]["quantity"], v, case["measurement"]["unit"], src,
                                                               time.time() - t0), flush=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    body = {"made": stamp, "calibration": cal, "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "fffprint_sha256": hashlib.sha256((ROOT / "src/mech/fffprint.c").read_bytes()).hexdigest(), "predictions": preds,
            "not_predicted": refused, "sealed_opened": False}
    path = G20 / ("predictions-warp-%s.json" % stamp)
    path.write_text(json.dumps(body, indent=1, sort_keys=True) + "\n")
    print("frozen: %s\n  SHA-256 %s" % (path, hashlib.sha256(path.read_bytes()).hexdigest()))


def cmd_score(path, sha):
    got = hashlib.sha256(Path(path).read_bytes()).hexdigest()
    if got != sha:
        raise SystemExit("the predictions' SHA-256 is %s, not the recorded %s: not scored" % (got, sha))
    if sha not in (ROOT / "GOALS.md").read_text():
        raise SystemExit("the hash is not recorded in GOALS.md: record it before the sealed file is opened")
    if hashlib.sha256(SEALED.read_bytes()).hexdigest() != SEALED_SHA:
        raise SystemExit("the sealed file is not the one sealed")
    setups, _ = load()
    preds = json.loads(Path(path).read_text())["predictions"]
    sealed = json.loads(SEALED.read_text())
    outcomes = sealed.get("outcomes") or sealed.get("cases") or sealed
    if isinstance(outcomes, list):
        outcomes = {o.get("case_id") or o.get("id"): o for o in outcomes}

    def to_unit(v, unit, want, cid):
        """a measured value in the prediction's unit: lengths in mm or um, strains in 1e-6"""
        norm = {"mm": ("mm", 1.0), "um": ("mm", 1e-3), "\u00b5m": ("mm", 1e-3), "\u03bcm": ("mm", 1e-3)}
        a = norm.get(unit.strip()) if unit else None
        b = norm.get(want.strip()) if want else None
        if a and b and a[0] == b[0]:
            return v * a[1] / b[1]
        micro = ("1e-6", "microstrain", "\u00b5\u03b5", "\u03bc\u03b5", "ue", "um/m")
        if any(m in (unit or "").lower() for m in micro) and any(m in (want or "").lower() for m in micro):
            return v
        if unit == want:
            return v
        raise SystemExit("%s: the sealed unit %r and the prediction's %r are not known to agree: not scored" % (cid, unit, want))

    def measured(cid, want):
        o = outcomes[cid]
        if cid.startswith("FAR25"):
            # the largest lift of the release profile (GOALS.md G20 step 4, the rule recorded before scoring): the
            # largest of the values the record gives, its labelled end values and the profile read from the figure
            vals = list((o.get("end_values_labelled_in_figure_7a_mm") or {}).values())
            vals += [q["value_mm"] for q in o.get("profile_read_from_figure_7c") or []]
            vals = [v for v in vals if isinstance(v, (int, float))]
            if not vals:
                raise SystemExit("%s: no profile value in the sealed record" % cid)
            return to_unit(max(vals), "mm", want, cid)
        if isinstance(o.get("value"), (int, float)):
            return to_unit(o["value"], o.get("unit"), want, cid)
        raise SystemExit("%s: no measured value in the sealed record" % cid)
    rows = []
    for cid, pr in sorted(preds.items()):
        m = measured(cid, pr["unit"])
        rows.append({"id": cid, "predicted": pr["value"], "measured": m, "unit": pr["unit"],
                     "error": pr["value"] / m - 1 if m else float("nan")})
    cases = {c["case_id"]: c for c in setups["cases"]}
    pairs = right = 0
    for i, ra in enumerate(rows):
        for rb in rows[i + 1:]:
            ca, cb = cases[ra["id"]], cases[rb["id"]]
            if ca["source_id"] != cb["source_id"]:
                continue
            sa, sb = signature(ca), signature(cb)
            diff = [k for k in sa if sa.get(k) != sb.get(k)]
            if len(diff) != 1:
                continue
            pairs += 1
            right += (rb["predicted"] - ra["predicted"]) * (rb["measured"] - ra["measured"]) > 0
    errs = [abs(r["error"]) for r in rows if r["error"] == r["error"]]
    med_err = statistics.median(errs) if errs else float("nan")
    for r in rows:
        print("  %-14s predicted %10.4g against %10.4g %s (%+7.1f %%)" % (r["id"], r["predicted"], r["measured"], r["unit"], 100 * r["error"]))
    print("  %d cases: median absolute error %.1f %% (criterion 25 %%); order right in %d of %d pairs that differ in one stated variable "
          "(%.0f %%; criterion 80 %%)" % (len(rows), 100 * med_err, right, pairs, 100 * right / pairs if pairs else float("nan")))
    out = {"predictions": str(path), "sha256": sha, "rows": rows, "median_abs_error": med_err, "pairs": pairs, "pairs_right": right}
    (G20 / ("score-" + Path(path).stem + ".json")).write_text(json.dumps(out, indent=1) + "\n")


def cmd_predict_tro22():
    setups, calib = load()
    tro = json.loads((G20 / "trofimov2022-setups.json").read_text())
    case = dict(tro["cases"][0])
    case.setdefault("material_id", "PLA_Raise3D_Premium_TRO22")
    case["measurement"] = {"quantity": "camber of the deck's middle line", "unit": "mm"}
    cal = json.loads((G20 / "warp-calibration.json").read_text())
    lt = cal["median_log10_tau_g_s"]
    t0 = time.time()
    r = run_case(case, setups, calib, 10.0, tau_g=10 ** lt)
    v = quantity(case, r)
    print("  TRO22-BRIDGE: camber %.5g mm (tau_g 10^%.3f s, the median)  [%.0f s]" % (v, lt, time.time() - t0))
    stamp = time.strftime("%Y%m%d-%H%M%S")
    body = {"made": stamp, "case": "TRO22-BRIDGE", "camber_mm": v, "log10_tau_g_s": lt, "profile_mm": bottom_profile(r),
            "material": r["material"]["provenance"], "process": r["process"],
            "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "sealed_opened": False}
    path = G20 / ("predictions-warp-tro22-%s.json" % stamp)
    path.write_text(json.dumps(body, indent=1, sort_keys=True) + "\n")
    print("frozen: %s\n  SHA-256 %s" % (path, hashlib.sha256(path.read_bytes()).hexdigest()))


R2_FIT_CASES = ("SYR21-R1", "SYR21-R3", "ALR22-T210", "ALR22-T260")  # GOALS.md G20 step 8, as amended


def r1_measured(setups, calib):
    """round one's ABS cases' measured quantities (round two's calibration data: the calibration cases and round one's
    test cases, open since round one's score)"""
    meas = {cid: calibration_value(calib, cid) for cid in ("SYR21-R1", "ALR22-T210")}
    sealed = json.loads(SEALED.read_text())["outcomes"]
    for c in setups["cases"]:
        cid = c["case_id"]
        if c["source_id"] in ("SYR21", "ALR22") and cid in sealed and isinstance(sealed[cid].get("value"), (int, float)):
            meas[cid] = sealed[cid]["value"]
    return meas


def cmd_calibrate_r2():
    """ABS's lock-in offset and bond toughness (GOALS.md G20 step 8, as amended): a grid on four cases, refined, then the
    chosen pair on all of round one's ABS bars, every evaluation logged as it comes"""
    setups, calib = load()
    cases = {c["case_id"]: c for c in setups["cases"]}
    meas = r1_measured(setups, calib)
    log = G20R2 / "calibration-r2.jsonl"

    def one(cid, off, gc):
        t0 = time.time()
        r = run_case(cases[cid], setups, calib, off, gc=gc, consistent=True)
        v = quantity(cases[cid], r)
        pe = ((r["summary"] or {}).get("results") or {}).get("bed_peel") or {}
        return cid, {"value": v, "measured": meas[cid], "error": v / meas[cid] - 1, "peeled": pe.get("peeled_fraction"),
                     "came_off": pe.get("came_off_during_print"), "seconds": round(time.time() - t0, 1)}

    def evaluate(off, gc, ids):
        from concurrent.futures import ThreadPoolExecutor  # each case runs its own engine process
        with ThreadPoolExecutor(max_workers=4) as ex:
            rows = dict(ex.map(lambda cid: one(cid, off, gc), ids))
        med = statistics.median(abs(x["error"]) for x in rows.values())
        rec = {"offset_K": off, "toughness_J_m2": gc, "median_abs_error": med, "cases": rows}
        with open(log, "a") as f:
            f.write(json.dumps(rec) + "\n")
        print("  offset %5.1f K  toughness %7.1f J/m^2: median %5.1f %%  %s" % (off, gc, 100 * med, "  ".join(
            "%s %.4g/%.4g" % (cid.split("-")[1], x["value"], x["measured"]) for cid, x in rows.items())), flush=True)
        return med
    seen = {}
    if log.exists():  # resume: the evaluations already logged on the four cases
        for line in log.read_text().splitlines():
            rec = json.loads(line)
            if sorted(rec["cases"]) == sorted(R2_FIT_CASES):
                seen[(round(rec["offset_K"], 3), round(rec["toughness_J_m2"], 3))] = rec["median_abs_error"]

    def ev(off, gc):
        key = (round(off, 3), round(gc, 3))
        if key not in seen:
            seen[key] = evaluate(off, gc, R2_FIT_CASES)
        return seen[key]
    # the grid: 30 and 100 J/m^2 at 50 K bracketed the measured bars (far too curled, then far too flat); at 45 J/m^2
    # SYR21's bars came out too flat and ALR22's still curled by millimetres, and an ALR22 peel near its threshold outran
    # an hour; so the search moves to the stronger bonds and higher lock-in offsets, which are also the cheaper runs
    for off in (60.0, 70.0):
        for gc in (100.0, 150.0):
            ev(off, gc)
    best = min(seen, key=seen.get)
    step_off, fac = 5.0, 1.7
    for _ in range(1):  # refine: the four neighbours of the best
        off, gc = best
        cand = [(off - step_off, gc), (off + step_off, gc), (off, gc / fac), (off, gc * fac)]
        for c in cand:
            ev(*c)
        nb = min(seen, key=seen.get)
        if nb == best:
            step_off, fac = step_off / 2, fac ** 0.5
        best = nb
    off, gc = best
    print("chosen: offset %.2f K, toughness %.3g J/m^2 (median %.1f %% on the four cases)" % (off, gc, 100 * seen[best]), flush=True)
    out = {"made_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "rule": "GOALS.md G20 step 8 as amended",
           "offset_K": off, "toughness_J_m2": gc, "fit_cases": list(R2_FIT_CASES), "median_on_fit_cases": seen[best],
           "evaluated": [{"offset_K": k[0], "toughness_J_m2": k[1], "median_abs_error": v} for k, v in sorted(seen.items())],
           "median_on_all_round_one_ABS_bars": None, "log": str(log)}
    (G20R2 / "calibration-r2.json").write_text(json.dumps(out, indent=1) + "\n")
    print("written %s (the pair on all of round one's ABS bars follows the predictions: calibrate-r2-report)" % (G20R2 / "calibration-r2.json"),
          flush=True)


def cmd_calibrate_r2_pick():
    """the best pair among those evaluated on the four cases, when the search is stopped early (GOALS.md G20 step 8 says why)"""
    log = G20R2 / "calibration-r2.jsonl"
    recs = [json.loads(l) for l in log.read_text().splitlines()]
    recs = [r for r in recs if sorted(r["cases"]) == sorted(R2_FIT_CASES)]
    best = min(recs, key=lambda r: r["median_abs_error"])
    out = {"made_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "rule": "GOALS.md G20 step 8 as amended (search stopped early)",
           "offset_K": best["offset_K"], "toughness_J_m2": best["toughness_J_m2"], "fit_cases": list(R2_FIT_CASES),
           "median_on_fit_cases": best["median_abs_error"],
           "evaluated": [{"offset_K": r["offset_K"], "toughness_J_m2": r["toughness_J_m2"], "median_abs_error": r["median_abs_error"]} for r in recs],
           "median_on_all_round_one_ABS_bars": None, "log": str(log)}
    (G20R2 / "calibration-r2.json").write_text(json.dumps(out, indent=1) + "\n")
    print("picked offset %.1f K, toughness %.3g J/m^2 (median %.1f %% on the four cases) of %d evaluated" % (
        best["offset_K"], best["toughness_J_m2"], 100 * best["median_abs_error"], len(recs)), flush=True)


def cmd_calibrate_r2_report():
    """the chosen pair on all of round one's ABS bars, so that the calibration's own error is on record"""
    setups, calib = load()
    cases = {c["case_id"]: c for c in setups["cases"]}
    meas = r1_measured(setups, calib)
    cal = json.loads((G20R2 / "calibration-r2.json").read_text())
    off, gc = cal["offset_K"], cal["toughness_J_m2"]
    ids = [c["case_id"] for c in setups["cases"] if c["source_id"] in ("SYR21", "ALR22") and c["case_id"] in meas]

    def one(cid):
        r = run_case(cases[cid], setups, calib, off, gc=gc, consistent=True)
        v = quantity(cases[cid], r)
        print("  %-12s %.4g against %.4g (%+.1f %%)" % (cid, v, meas[cid], 100 * (v / meas[cid] - 1)), flush=True)
        return cid, {"value": v, "measured": meas[cid], "error": v / meas[cid] - 1}
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=4) as ex:
        rows = dict(ex.map(one, ids))
    cal["median_on_all_round_one_ABS_bars"] = statistics.median(abs(x["error"]) for x in rows.values())
    cal["all_round_one_ABS_bars"] = rows
    (G20R2 / "calibration-r2.json").write_text(json.dumps(cal, indent=1) + "\n")
    print("median on all %d of round one's ABS bars %.1f %%" % (len(rows), 100 * cal["median_on_all_round_one_ABS_bars"]), flush=True)


R2_REFUSED = {
    "NAG25": "a PLA strip printed on a clamped polystyrene cantilever whose clamped length is not stated: two materials and an unstated "
             "mounting (GOALS.md G20 step 8)",
}
R2_DEFERRED = {
    "CHO16": "blocks of 31,250 cells with a peeling bed, beyond this session's budget on the shared laptop; frozen and scored later, apart "
             "(GOALS.md G20 step 8 as amended)",
}


def load_r2():
    return json.loads((G20R2 / "setups.json").read_text())


def cmd_predict_r2():
    """round two's fresh plastic cases (GOALS.md G20 step 8): ABS's calibrated pair, frozen with their SHA-256"""
    setups, calib = load()
    r2 = load_r2()
    cal = json.loads((G20R2 / "calibration-r2.json").read_text())
    off, gc = cal["offset_K"], cal["toughness_J_m2"]
    preds, refused = {}, {}
    for case in r2["cases"]:
        sid = case["source_id"]
        if sid in R2_REFUSED or sid in R2_DEFERRED:
            refused[case["case_id"]] = R2_REFUSED.get(sid) or R2_DEFERRED[sid]
            continue
        t0 = time.time()
        r = run_case(case, setups, calib, off, gc=gc, consistent=True)
        v = quantity(case, r)
        pe = ((r["summary"] or {}).get("results") or {}).get("bed_peel") or {}
        preds[case["case_id"]] = {"value": v, "unit": "mm", "quantity": case["measurement"]["quantity"], "peeled_fraction": pe.get("peeled_fraction"),
                                  "came_off_during_print": pe.get("came_off_during_print"), "seconds": round(time.time() - t0, 1)}
        print("  %-24s flatness %.4f mm  (peeled %s, came off %s)  [%.0f s]" % (case["case_id"], v, pe.get("peeled_fraction"),
              pe.get("came_off_during_print"), time.time() - t0), flush=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    body = {"made": stamp, "rule": "GOALS.md G20 step 8 as amended", "calibration": cal, "predictions": preds, "not_predicted": refused,
            "assumptions": ["ABS's lock-in offset and bond toughness from round one's ABS bars stand for ABSplus-P430 on its soluble support",
                            "ARM18's print speed %g mm/s and road %g mm, which the source does not state" % (ARM18_SPEED_MM_S, ARM18_ROAD_MM),
                            "the 75 C chamber as bed and ambient; released after five minutes at the chamber temperature; the 75 C bath not modelled",
                            "the flatness as the range of the top face's heights about their least-squares plane; scanner noise not added"],
            "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "fffprint_sha256": hashlib.sha256((ROOT / "src/mech/fffprint.c").read_bytes()).hexdigest(), "sealed_opened": False}
    path = G20R2 / ("predictions-r2-%s.json" % stamp)
    path.write_text(json.dumps(body, indent=1, sort_keys=True) + "\n")
    print("frozen: %s\n  SHA-256 %s" % (path, hashlib.sha256(path.read_bytes()).hexdigest()))


def cmd_score_r2(path, sha):
    got = hashlib.sha256(Path(path).read_bytes()).hexdigest()
    if got != sha:
        raise SystemExit("the predictions' SHA-256 is %s, not the recorded %s: not scored" % (got, sha))
    if sha not in (ROOT / "GOALS.md").read_text():
        raise SystemExit("the hash is not recorded in GOALS.md: record it before the sealed file is opened")
    if hashlib.sha256(SEALED_R2.read_bytes()).hexdigest() != SEALED_R2_SHA:
        raise SystemExit("the sealed file is not the one sealed")
    preds = json.loads(Path(path).read_text())["predictions"]
    oc = json.loads(SEALED_R2.read_text())["outcomes"]
    cases = {c["case_id"]: c for c in load_r2()["cases"]}
    rows = []
    for cid, pr in sorted(preds.items()):
        o = oc[cid]
        if o.get("unit") != "mm":
            raise SystemExit("%s: the sealed unit is %r, not mm" % (cid, o.get("unit")))
        m, m5 = o["value_mm"], o.get("table5_value_mm", o["value_mm"])
        rows.append({"id": cid, "predicted": pr["value"], "measured": m, "measured_table5": m5, "error": pr["value"] / m - 1,
                     "error_table5": pr["value"] / m5 - 1})
    def sig(c):
        g, q = c["geometry"], c["process"]
        return (g["length_l_mm"], g["width_w_mm"], g["height_h_mm"], q["layer_thickness_mm"])
    pairs = right = 0
    for i, a in enumerate(rows):
        for b in rows[i + 1:]:
            sa, sb = sig(cases[a["id"]]), sig(cases[b["id"]])
            if sum(x != y for x, y in zip(sa, sb)) != 1:
                continue
            pairs += 1
            right += (b["predicted"] - a["predicted"]) * (b["measured"] - a["measured"]) > 0
    med = statistics.median(abs(r["error"]) for r in rows)
    med5 = statistics.median(abs(r["error_table5"]) for r in rows)
    big = [r for r in rows if r["predicted"] > 0.18]
    medbig = statistics.median(abs(r["error"]) for r in big) if big else float("nan")
    for r in rows:
        print("  %-24s predicted %7.4f against %7.4f mm (%+7.1f %%)" % (r["id"], r["predicted"], r["measured"], 100 * r["error"]))
    print("  %d cases: median absolute error %.1f %% (criterion 25 %%; with Table 5's readings %.1f %%); order right in %d of %d pairs "
          "(%.0f %%; criterion 80 %%); the %d the model puts above 0.18 mm: median %.1f %%" % (
              len(rows), 100 * med, 100 * med5, right, pairs, 100 * right / pairs if pairs else float("nan"), len(big), 100 * medbig))
    out = {"predictions": str(path), "sha256": sha, "rows": rows, "median_abs_error": med, "median_abs_error_table5": med5,
           "pairs": pairs, "pairs_right": right, "above_0_18_mm": len(big), "median_above_0_18_mm": medbig}
    (G20R2 / ("score-" + Path(path).stem + ".json")).write_text(json.dumps(out, indent=1) + "\n")


def main():
    a = sys.argv[1:]
    if a[:1] == ["calibrate-r2"]:
        cmd_calibrate_r2()
        return 0
    if a[:1] == ["calibrate-r2-pick"]:
        cmd_calibrate_r2_pick()
        return 0
    if a[:1] == ["calibrate-r2-report"]:
        cmd_calibrate_r2_report()
        return 0
    if a[:1] == ["predict-r2"]:
        cmd_predict_r2()
        return 0
    if a[:1] == ["score-r2"]:
        cmd_score_r2(a[1], a[2])
        return 0
    if a[:1] == ["predict-tro22"]:
        cmd_predict_tro22()
        return 0
    if a[:1] == ["predict"]:
        cmd_predict()
        return 0
    if a[:1] == ["score"]:
        cmd_score(a[1], a[2])
        return 0
    if a[:1] == ["calibrate"]:
        cmd_calibrate()
        return 0
    if a[:1] == ["run"]:
        setups, calib = load()
        case = next(x for x in setups["cases"] if x["case_id"] == a[1])
        off = float(a[a.index("--offset") + 1]) if "--offset" in a else 10.0
        global LAYER_MODE
        if "--printed-layers" in a:
            LAYER_MODE = "printed"
        t0 = time.time()
        tau = float(a[a.index("--tau") + 1]) if "--tau" in a else 0.0
        tact = float(a[a.index("--tact") + 1]) if "--tact" in a else 0.0
        gc = float(a[a.index("--gc") + 1]) if "--gc" in a else 0.0
        r = run_case(case, setups, calib, off, keep="--keep" in a, tau_g=tau, t_act=tact, gc=gc)
        prof = bottom_profile(r)
        print("  layer %.3g mm, cells %.3g mm high; bottom heights along the bar (mm):" % (r["layer_mm"], r["cell_z_mm"]))
        print("   " + " ".join("%.4f" % z for _, z in prof))
        res = (r["summary"] or {}).get("results", {})
        print("  warp range %.4f to %.4f mm (summary); %s %s" % (res.get("warp_z_min_mm", float("nan")), res.get("warp_z_max_mm", float("nan")),
                                                                case["measurement"]["quantity"], "%.4g %s" % (quantity(case, r), case["measurement"]["unit"])))
        print("  %.0f s" % (time.time() - t0))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
