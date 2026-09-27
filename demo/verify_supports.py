#!/usr/bin/env python3
"""verify_supports.py - the verification cases S5, S5b and S6 of docs/contracts/supports.md: explicitly meshed support
walls against the homogenised block support of the same walls, at 0.25 mm voxels and layers.

    python3 demo/verify_supports.py s5 s5b s6        # about 40 minutes on the M2 Air; results in the terminal and
                                                    # in validation/supports/verification_<date>.json (a new file each run)

S5: the bridge on two legs (LPBF eigenstrain build), largest displacement on the plate and after removal.
S5b: a slab standing on its supports alone (the amendment of 2026-09-19), the same two quantities, with the gap filled
     solid beside it to show the criterion can fail.
S6: the bridge in the FFF heat model (generic PLA): the deck's temperature rise above the bed at mid-span at the end of
    printing, and the heat into the bed during the print.
"""
import datetime
import json
import pathlib
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import lpbfflow as F  # noqa: E402
from mcptest import Client  # noqa: E402

H = 0.25
N = lambda mm: int(round(mm / H))  # noqa: E731


def inwall(u, lo, pitch, t):
    x = (u + 0.5) * H - lo
    return abs((x % pitch) - pitch / 2) < t / 2


BRIDGE = {(i, j, k) for i in range(N(12)) for j in range(N(4)) for k in range(N(6)) if k >= N(4) or i < N(3) or i >= N(9)}
BRIDGE_WALLS = {(i, j, k) for i in range(N(3), N(9)) for j in range(N(4)) for k in range(N(4)) if inwall(i, 3, 2, 0.5) or inwall(j, 0, 2, 0.5)}
SLAB = {(i, j, k) for i in range(N(12)) for j in range(N(4)) for k in range(N(4), N(6))}
SLAB_WALLS = {(i, j, k) for i in range(N(12)) for j in range(N(4)) for k in range(N(4)) if inwall(i, 0, 2, 0.5) or inwall(j, 0, 2, 0.5)}
BLOCK = {"type": "block", "wall_thickness": "0.5 mm", "spacing": "2 mm", "provenance": "user",
         "source": "verification: the same walls as a homogenised block support"}
PROCESS = {"layer_height": f"{H} mm", "printed_layer_height": "0.2 mm", "nozzle_temperature": "210 degC", "bed_temperature": "60 degC",
           "ambient_temperature": "30 degC", "deposition_rate": "8 mm^3/s", "min_layer_time": "8 s", "cooldown_bed_on": "120 s",
           "cooldown_bed_off": "300 s", "thermal_substeps": 4, "provenance": "user"}


def setup(c, tmp, name, cells, lift=0.0, material=None):
    F.voxel_stl(tmp / f"{name}.stl", cells, H)
    F.call_ok(c, "project_create", {"name": name, "description": name})
    F.call_ok(c, "geometry_import", {"path": str(tmp / f"{name}.stl"), "units": "mm", "name": "b"})
    if lift:
        F.call_ok(c, "geometry_place", {"body": "b", "z_offset": f"{lift} mm"})
    if material is None:
        F.call_ok(c, "material_define", {"material": {"id": "m", "name": "m", "family": "metal", "status": "user_supplied",
                  "processes": ["lpbf"], "provenance": "test values only", "density_kg_m3": {"value": 2680},
                  "youngs_modulus_pa": {"value": 70e9}, "poisson_ratio": {"value": 0.33}}})
        material = "m"
    F.call_ok(c, "material_assign", {"body": "b", "material": material, "source": "user"})
    F.call_ok(c, "mesh_generate", {"element_size": f"{H} mm"})


def lpbf(c, sup):
    args = {"body": "b", "build_orientation": "X", "layer_thickness_sim": f"{H} mm", "inherent_strain": F.STRAIN,
            "material": F.MATERIAL, "supports": dict(sup, remove=True)}
    t0 = time.time()
    v = F.call_ok(c, "lpbf_build_run", args)
    st = F.wait_job(c, v["job_id"], tries=2000)
    r = st["summary"]["results"]
    return {"on_plate_mm": r["max_displacement_before_cut_mm"], "after_removal_mm": r["supports"]["max_displacement_after_support_removal_mm"],
            "support_elements": r["supports"]["elements"], "volume_mm3": r["supports"]["volume_mm3"], "wall_s": round(time.time() - t0, 1)}


def fff(c, sup):
    args = {"process": PROCESS, "probes": [{"at": ["0 mm", "0 mm", "6 mm"], "label": "deck top mid-span"}]}
    if sup:
        args["supports"] = sup
    t0 = time.time()
    v = F.call_ok(c, "mech_print_run", args)
    st = F.wait_job(c, v["job_id"], tries=4000)
    s = st["summary"]
    tp = s["timing"]["print_time_h"] * 3600
    pr = s["probes"][0]
    i = max(k for k, t in enumerate(pr["time_s"]) if t <= tp + 1e-6)
    return {"rise_above_bed_k": pr["temperature_c"][i] - 60.0, "heat_into_bed_during_print_j": s["results"]["heat_into_bed_during_print_j"],
            "wall_s": round(time.time() - t0, 1)}


def main():
    cases = sys.argv[1:] or ["s5", "s5b", "s6"]
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="nvsupverify"))
    c = Client(["--embedded", "--workspace", str(tmp / "ws"), "--allow-read", str(tmp)])
    c.initialize()
    out = {"date": datetime.date.today().isoformat(), "voxel_mm": H}
    try:
        if "s5" in cases:
            setup(c, tmp, "s5_explicit", BRIDGE | BRIDGE_WALLS)
            a = lpbf(c, {"type": "explicit", "region_mm": [-3, -2, 0, 3, 2, 4], "provenance": "user", "source": "S5: the walls meshed"})
            setup(c, tmp, "s5_homogenised", BRIDGE)
            b = lpbf(c, BLOCK)
            out["S5"] = {"explicit": a, "homogenised": b, "criterion_pct": 10}
        if "s5b" in cases:
            setup(c, tmp, "s5b_explicit", SLAB | SLAB_WALLS)
            a = lpbf(c, {"type": "explicit", "region_mm": [-6, -2, 0, 6, 2, 4], "provenance": "user", "source": "S5b: the walls meshed"})
            setup(c, tmp, "s5b_homogenised", SLAB, lift=4.0)
            b = lpbf(c, BLOCK)
            setup(c, tmp, "s5b_solid", SLAB, lift=4.0)
            d = lpbf(c, {"type": "homogeneous", "stiffness_fraction": 1.0, "provenance": "user", "source": "S5b: the gap filled solid"})
            out["S5b"] = {"explicit": a, "homogenised": b, "solid_fill": d, "criterion_pct": 10}
        if "s6" in cases:
            setup(c, tmp, "s6_explicit", BRIDGE | BRIDGE_WALLS, material="pla_generic_demo")
            a = fff(c, None)
            setup(c, tmp, "s6_homogenised", BRIDGE, material="pla_generic_demo")
            b = fff(c, BLOCK)
            out["S6"] = {"explicit": a, "homogenised": b, "criterion_pct": 5}
    finally:
        c.close()
    for k, v in out.items():
        if isinstance(v, dict) and "explicit" in v:
            e, h = v["explicit"], v["homogenised"]
            for q in e:
                if q in h and isinstance(e[q], (int, float)) and q.endswith(("_mm", "_k", "_j")) and e[q]:
                    d = 100 * (h[q] / e[q] - 1)
                    print(f"{k} {q}: explicit {e[q]:.6g}, homogenised {h[q]:.6g} ({d:+.1f} %, {'passes' if abs(d) <= v['criterion_pct'] else 'fails'})")
    dest = ROOT / "validation/supports" / f"verification_{out['date']}.json"
    dest.parent.mkdir(parents=True, exist_ok=True)
    k = 1
    while dest.exists():
        k += 1
        dest = dest.with_name(f"verification_{out['date']}_{k}.json")
    dest.write_text(json.dumps(out, indent=2) + "\n")
    print(f"-> {dest.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
