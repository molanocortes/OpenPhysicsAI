#!/usr/bin/env python3
"""printskin.py - the print model's skin: convection and radiation on a turned wall's voxel steps (GOALS.md G22 step 4;
the case and its criterion were written before the first run, 2026-10-08).

G16's wall, 8 x 2 x 4 mm, standing on its 8 x 2 mm face, turned about the vertical by 0, 30 and 45 degrees and meshed
with cubes of 1, 0.5 and 0.25 mm, is printed in one simulation layer with the nozzle and the bed at 120 C in air at
20 C. It is born at the bed's temperature and stays there (its conductivity, a constant of the check, is
1000 W/(m K)), so the heat into the air over the run's time is the flux of a uniform surface. It is read with
convection alone and with radiation added, the radiation's share being the difference of the two runs. Closed forms
over the wall's surface less its bed face, A = 96 mm^2: h A dT = 0.144 W (h 15 W/(m^2 K)) and
eps sigma A (T_bed^4 - T_air^4) = 0.080865 W (eps 0.9).
Pass, at 0.25 mm: both within 2 % of their closed forms at each angle, with the whole wall printed. Recorded beside
them: the 1 and 0.5 mm results and the printed volume against 64 mm^3. The criterion also asks that the heat at 0
degrees be bitwise the same before and after the change: --compare reads two records and checks it.

The material is the check's own set of constants (conductivity 1000 W/(m K), 1000 kg/m^3, 1000 J/(kg K), 1 GPa, nu
0.3, expansion 1e-5 1/K, emissivity 0.9), labelled as such; it is not a library material.

Usage, from the repository root after `make openphysics-mcp` (OPENPHYSICS_BIN=DIR runs DIR/openphysics-mcp instead):
    python3 tools/printskin.py             every angle at 1, 0.5 and 0.25 mm
    python3 tools/printskin.py --quick     at 0.25 mm only (the criterion's runs)
    python3 tools/printskin.py --out DIR   also write printskin.json there; --keep keeps the workspaces
    python3 tools/printskin.py --compare BEFORE.json AFTER.json    the 0 degree runs' heat bitwise equal?
Exit status 0 when the criterion passes. The workspaces are removed when it passes and kept when it fails.
"""
import argparse
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcptest import Client  # noqa: E402

L_MM, W_MM, H_MM = 8.0, 2.0, 4.0
AREA_MM2 = L_MM * H_MM * 2 + W_MM * H_MM * 2 + L_MM * W_MM       # the surface less its bed face: 96
VOLUME_MM3 = L_MM * W_MM * H_MM
H_FILM, EMIS, SIGMA = 15.0, 0.9, 5.670374419e-8
T_BED_C, T_AIR_C = 120.0, 20.0
K_W, RHO, CP, E_PA, NU, ALPHA = 1000.0, 1000.0, 1000.0, 1e9, 0.3, 1e-5
ANGLES = (0.0, 30.0, 45.0)
LIMIT = 0.02


def closed_forms():
    tb, ta = T_BED_C + 273.15, T_AIR_C + 273.15
    a = AREA_MM2 * 1e-6
    return {"convection_w": H_FILM * a * (T_BED_C - T_AIR_C), "radiation_w": EMIS * SIGMA * a * (tb ** 4 - ta ** 4)}


def wall_stl(path, deg):
    """binary STL of the wall centred on the vertical axis, turned by deg about it, outward winding; returns its volume"""
    c, s = (1.0, 0.0) if deg == 0 else (math.cos(math.radians(deg)), math.sin(math.radians(deg)))

    def p(x, y, z):
        return (x * c - y * s, x * s + y * c, z) if deg else (x, y, z)

    x0, x1, y0, y1, z0, z1 = -0.5 * L_MM, 0.5 * L_MM, -0.5 * W_MM, 0.5 * W_MM, 0.0, H_MM
    v = [p(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]   # index = 4 k + 2 j + i
    quads = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]   # -z +z -y +y -x +x
    tris = [t for q in quads for t in ((v[q[0]], v[q[1]], v[q[2]]), (v[q[0]], v[q[2]], v[q[3]]))]
    vol = sum((a[0] * (b[1] * d[2] - b[2] * d[1]) - a[1] * (b[0] * d[2] - b[2] * d[0]) + a[2] * (b[0] * d[1] - b[1] * d[0])) / 6
              for a, b, d in tris)
    if abs(vol - VOLUME_MM3) > 1e-9 * VOLUME_MM3:
        raise RuntimeError(f"the wall is not closed and outward ({vol:.6f} mm^3 against {VOLUME_MM3:g})")
    with open(path, "wb") as f:
        f.write(b"OpenPhysicsAI G22 step 4 wall".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for q in t:
                f.write(struct.pack("<3f", *q))
            f.write(b"\0\0")
    return vol


# ---- one print through the operation layer -----------------------------------------------------------------------

class Failed(RuntimeError):
    pass


def call(c, name, args):
    r = c.call(name, args)
    sc = (r.get("result") or {}).get("structuredContent") or {}
    if (r.get("result") or {}).get("isError") or sc.get("ok") is not True:
        raise Failed(f"{name}: {json.dumps(sc.get('error') or r.get('error'))[:600]}")
    return sc.get("value") or {}


def wait_job(c, job_id, limit_s=540):
    t0 = time.time()
    while time.time() - t0 < limit_s:
        v = call(c, "job_status", {"job_id": job_id})
        if v.get("state") in ("succeeded", "failed", "cancelled"):
            if v.get("state") != "succeeded":
                raise Failed(f"job {job_id} {v.get('state')}: {json.dumps(v.get('error'))[:600]}")
            return v
        time.sleep(0.25)
    raise Failed(f"job {job_id} did not finish within {limit_s} s")


def run(tmp, deg, h_mm):
    """the wall at one angle and cube size, printed twice (convection alone, then with radiation): returns its record"""
    name = f"wall_{deg:g}deg_{h_mm:g}mm".replace(".", "p")
    stl = tmp / f"{name}.stl"
    wall_stl(stl, deg)
    ws = tmp / f"ws_{name}"
    c = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(tmp)])
    t0 = time.time()
    try:
        c.initialize()
        call(c, "project_create", {"name": name, "description": "GOALS.md G22 step 4: the print model's skin on a turned wall"})
        call(c, "geometry_import", {"path": str(stl), "units": "mm", "name": "wall", "units_source": "user"})
        call(c, "material_define", {"material": {
            "id": "skin_check", "name": "constants of the G22 step 4 check", "family": "polymer", "status": "user_supplied",
            "provenance": "the verification case's stated constants, GOALS.md G22 step 4",
            "density_kg_m3": {"value": RHO}, "conductivity_w_per_mk": {"value": K_W}, "specific_heat_j_per_kgk": {"value": CP},
            "youngs_modulus_pa": {"value": E_PA}, "poisson_ratio": {"value": NU}, "expansion_1_per_k": {"value": ALPHA},
            "emissivity": {"value": EMIS}}})
        call(c, "material_assign", {"body": "wall", "material": "skin_check", "source": "user"})
        mesh = call(c, "mesh_generate", {"element_size": f"{h_mm} mm", "max_elements": 400000}).get("mesh", {})
        rec = {"name": name, "angle_deg": deg, "element_mm": h_mm, "elements": mesh.get("elements"), "nodes": mesh.get("nodes")}
        for key, radiation in (("convection", False), ("with_radiation", True)):
            proc = {"layer_height": f"{H_MM:g} mm", "printed_layer_height": "0.2 mm", "nozzle_temperature": f"{T_BED_C:g} degC",
                    "bed_temperature": f"{T_BED_C:g} degC", "ambient_temperature": f"{T_AIR_C:g} degC",
                    "convection": f"{H_FILM:g} W/(m^2 K)", "radiation": radiation, "deposition_rate": "10 mm^3/s",
                    "min_layer_time": "10 s", "cooldown_bed_on": "60 s", "cooldown_bed_off": "0 s", "provenance": "user"}
            job = call(c, "mech_print_run", {"process": proc, "label": f"{name} {key}"})
            st = wait_job(c, job["job_id"])
            s = st.get("summary") or {}
            res, dep, tim = s.get("results") or {}, s.get("deposition") or {}, s.get("timing") or {}
            rec[key] = {"air_heat_j": res.get("heat_into_air_j"), "total_time_s": 3600.0 * tim.get("total_machine_time_h", float("nan")),
                        "printed_volume_mm3": 1e3 * dep.get("printed_volume_cm3", float("nan")),
                        "elements_never_deposited": dep.get("elements_never_deposited"),
                        "elements_deposited_late": dep.get("elements_deposited_late"),
                        "worst_heat_balance_relative": res.get("worst_heat_balance_relative")}
        rec["seconds"] = round(time.time() - t0, 1)
        return rec
    finally:
        c.close()


def judge(rec, cf):
    """the uniform-surface fluxes of one record against the closed forms"""
    a, b = rec["convection"], rec["with_radiation"]
    t = a["total_time_s"]
    q_conv = a["air_heat_j"] / t
    q_rad = (b["air_heat_j"] - a["air_heat_j"]) / b["total_time_s"]
    rec["convection_w"], rec["radiation_w"] = q_conv, q_rad
    rec["convection_error_rel"] = q_conv / cf["convection_w"] - 1
    rec["radiation_error_rel"] = q_rad / cf["radiation_w"] - 1
    rec["volume_error_rel"] = a["printed_volume_mm3"] / VOLUME_MM3 - 1
    rec["whole_wall_printed"] = a["elements_never_deposited"] == 0 and b["elements_never_deposited"] == 0
    rec["pass"] = (abs(rec["convection_error_rel"]) <= LIMIT and abs(rec["radiation_error_rel"]) <= LIMIT and rec["whole_wall_printed"])
    return rec


def show(rec):
    print(f"== {rec['angle_deg']:g} degrees, cubes of {rec['element_mm']:g} mm: {rec['elements']} elements, printed volume "
          f"{rec['convection']['printed_volume_mm3']:.4f} mm^3 ({100 * rec['volume_error_rel']:+.2f} % of 64), {rec['seconds']} s", flush=True)
    print(f"   convection {rec['convection_w']:.6f} W ({100 * rec['convection_error_rel']:+.3f} %), radiation {rec['radiation_w']:.6f} W "
          f"({100 * rec['radiation_error_rel']:+.3f} %); heat into the air {rec['convection']['air_heat_j']!r} J over "
          f"{rec['convection']['total_time_s']:g} s{'' if rec['pass'] else '   [outside the criterion]'}", flush=True)


def compare(before, after):
    """the criterion's last clause: at 0 degrees the heat into the air bitwise the same before and after the change"""
    rb = {(r["angle_deg"], r["element_mm"]): r for r in json.loads(Path(before).read_text())["runs"] if "error" not in r}
    ra = {(r["angle_deg"], r["element_mm"]): r for r in json.loads(Path(after).read_text())["runs"] if "error" not in r}
    ok, n = True, 0
    for key in sorted(k for k in rb if k[0] == 0 and k in ra):
        for kind in ("convection", "with_radiation"):
            x, y = rb[key][kind]["air_heat_j"], ra[key][kind]["air_heat_j"]
            same = struct.pack("<d", x) == struct.pack("<d", y)
            ok &= same
            n += 1
            print(f"   0 degrees, {key[1]:g} mm, {kind}: {x!r} J before, {y!r} J after: {'bitwise equal' if same else 'DIFFERENT'}")
    print("PASS" if ok and n else "FAIL", f"({n} runs at 0 degrees compared)")
    return 0 if ok and n else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--quick", action="store_true", help="at 0.25 mm only, the criterion's runs")
    ap.add_argument("--out", help="directory for printskin.json")
    ap.add_argument("--keep", action="store_true", help="keep the workspaces")
    ap.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"), help="two records: the 0 degree heat bitwise equal?")
    a = ap.parse_args()
    if a.compare:
        return compare(*a.compare)
    root = Path(__file__).resolve().parent.parent
    try:
        commit = subprocess.run(["git", "-C", str(root), "describe", "--always", "--dirty"], capture_output=True, text=True).stdout.strip()
    except OSError:
        commit = ""
    cf = closed_forms()
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-printskin-"))
    sizes = (0.25,) if a.quick else (1.0, 0.5, 0.25)
    out = {"check": "GOALS.md G22 step 4: the print model's skin", "engine": commit, "closed_forms_w": cf, "area_mm2": AREA_MM2,
           "criterion": "at 0.25 mm and each of 0, 30 and 45 degrees, the uniform-surface convection and radiation within 2 % of "
                        "h A dT and eps sigma A (T_bed^4 - T_air^4), the whole wall printed; at 0 degrees the heat into the air "
                        "bitwise the same before and after the change (--compare)", "runs": []}
    print(f"closed forms over {AREA_MM2:g} mm^2: convection {cf['convection_w']:.6f} W, radiation {cf['radiation_w']:.6f} W", flush=True)
    ok = True
    for h in sizes:
        for deg in ANGLES:
            judged = h == 0.25
            try:
                rec = judge(run(tmp, deg, h), cf)
            except Failed as e:
                print(f"== {deg:g} degrees, {h:g} mm: FAILED TO RUN: {e}", flush=True)
                out["runs"].append({"angle_deg": deg, "element_mm": h, "error": str(e), "judged": judged})
                ok &= not judged
                continue
            rec["judged"] = judged
            show(rec)
            if judged:
                ok &= rec["pass"]
            out["runs"].append(rec)
    out["pass"] = ok
    if a.out:
        Path(a.out).mkdir(parents=True, exist_ok=True)
        (Path(a.out) / "printskin.json").write_text(json.dumps(out, indent=1))
    print("PASS" if ok else "FAIL", f"(engine {commit})")
    if ok and not a.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print(f"workspaces kept in {tmp}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
