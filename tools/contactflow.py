#!/usr/bin/env python3
"""contactflow.py - thermal contact between two bodies, configured, solved and checked through MCP only.

Criteria were fixed before the first run: "Thermal Sim/verification/stageC-criteria.md" (C1..C8).

Run from the repository root after `make am`:
    python3 tools/contactflow.py
"""
import json
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcptest import SERVER, Client  # noqa: E402

PASS = 0
FAIL = 0


def check(cond, what):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"  FAIL: {what}")


def call_ok(c, name, args, what=None):
    r = c.call(name, args)
    res = r.get("result", {})
    sc = res.get("structuredContent") or {}
    ok = res.get("isError") is False and sc.get("ok") is True
    check(ok, f"{what or name}: {json.dumps(sc.get('error'))[:600] if not ok else ''}")
    return sc.get("value", {}), r


def call_err(c, name, args):
    r = c.call(name, args)
    sc = (r.get("result", {}) or {}).get("structuredContent") or {}
    return sc.get("error") or {}


def poll(c, name, args):
    r = c.call(name, args)
    return (r.get("result", {}).get("structuredContent") or {}).get("value", {})


def box_stl(path, x0, y0, z0, lx, ly, lz, label=b"contactflow box"):
    p = [(x0, y0, z0), (x0 + lx, y0, z0), (x0 + lx, y0 + ly, z0), (x0, y0 + ly, z0),
         (x0, y0, z0 + lz), (x0 + lx, y0, z0 + lz), (x0 + lx, y0 + ly, z0 + lz), (x0, y0 + ly, z0 + lz)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, cc, d in quads:
        tris += [(p[a], p[b], p[cc]), (p[a], p[cc], p[d])]
    with open(path, "wb") as f:
        f.write(label.ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


def run(c, args, what, expect="succeeded"):
    v, _ = call_ok(c, "analysis_run", args, what)
    if not v.get("job_id"):
        return {}, {}
    for _ in range(12000):
        st = poll(c, "job_status", {"job_id": v["job_id"]})
        if st.get("state") in ("succeeded", "failed", "cancelled", "paused"):
            break
        time.sleep(0.05)
    check(st.get("state") == expect, f"{what}: {st.get('state')} {json.dumps(st.get('error'))[:400]}")
    return v, st


MAT = lambda mid, k: {"material": {  # noqa: E731
    "id": mid, "name": mid, "family": "metal", "status": "user_supplied", "provenance": "verification values for the contact test",
    "density_kg_m3": {"value": 8000.0}, "conductivity_w_per_mk": {"value": k}, "specific_heat_j_per_kgk": {"value": 500.0},
    "youngs_modulus_pa": {"value": 200e9}, "poisson_ratio": {"value": 0.3}, "expansion_1_per_k": {"value": 1.2e-5}}}

RUN = {"analysis": "transient_thermal", "end_time": "30000 s", "time_stepping": "adaptive", "initial_temperature": "20 degC",
       "temporal_relative_tolerance": 1e-4, "temporal_temperature_tolerance": "0.001 K", "output_times": ["30000 s"], "allow_duplicate": True}
# series resistance (steady state)
Q = 0.5
T_BFAR, T_BI, T_AI, T_AFAR, JUMP = 70.0, 80.0, 82.5, 84.5, 2.5


def setup(c, ws_name, tmp, stl_a, stl_b):
    call_ok(c, "project_create", {"name": ws_name, "description": "two blocks with a contact conductance"})
    call_ok(c, "geometry_import", {"path": str(stl_a), "units": "mm", "name": "blockA"})
    call_ok(c, "geometry_import", {"path": str(stl_b), "units": "mm", "name": "blockB"})
    # every import is centred on the plate: place the assembly explicitly so that the blocks meet at x = 0
    call_ok(c, "geometry_place", {"body": "blockA", "position": [-10, 0]})
    call_ok(c, "geometry_place", {"body": "blockB", "position": [10, 0]})
    call_ok(c, "material_define", MAT("alloy_k50", 50.0))
    call_ok(c, "material_define", MAT("alloy_k10", 10.0))
    call_ok(c, "material_assign", {"body": "blockA", "material": "alloy_k50", "source": "user"})
    call_ok(c, "material_assign", {"body": "blockB", "material": "alloy_k10", "source": "user"})
    call_ok(c, "mesh_generate", {"element_size": "2.5 mm"})
    call_ok(c, "selection_create", {"name": "a_far", "query": {"plane": {"axis": "x", "at": "min"}}, "body": "blockA"})
    call_ok(c, "selection_create", {"name": "b_far", "query": {"plane": {"axis": "x", "at": "max"}}, "body": "blockB"})
    call_ok(c, "boundary_apply", {"name": "heating", "kind": "heat_flux", "selection": "a_far", "heat_flux": "5000 W/m^2", "source": "user"})
    call_ok(c, "boundary_apply", {"name": "cooling", "kind": "convection", "selection": "b_far", "source": "user",
                                  "convection": {"coefficient": 100, "ambient": "20 degC"}})


def interface_result(c, job_id, name="contact"):
    v, _ = call_ok(c, "results_interface", {"job_id": job_id, "interface": name}, "results_interface")
    ifs = v.get("interfaces", [{}])
    return (ifs[0] if ifs else {}), v


def far_temps(c, job_id):
    a, _ = call_ok(c, "results_probe", {"job_id": job_id, "points_mm": [[-20, 0, 5], [20, 0, 5]]}, "probe far faces")
    pr = a.get("probes", [])
    return (pr[0].get("temperature_c") if len(pr) > 0 else None), (pr[1].get("temperature_c") if len(pr) > 1 else None)


def main():
    tmp = Path(tempfile.mkdtemp(prefix="nvcontact"))
    ws = tmp / "ws"
    # the two blocks meet at x = 0 (placement centres bodies on the plate, so both are built in one frame)
    stl_a, stl_b = tmp / "a.stl", tmp / "b.stl"
    box_stl(stl_a, -20, -5, 0, 20, 10, 10, b"block A")
    box_stl(stl_b, 0, -5, 0, 20, 10, 10, b"block B")
    common = ["--workspace", str(ws), "--allow-read", str(tmp)]
    c = Client(["--embedded"] + common)
    c.initialize()

    cap, _ = call_ok(c, "capabilities_get", {})
    th = {a["name"]: a for a in cap.get("analyses", [])}.get("transient_thermal", {})
    check("available" in th.get("thermal_interfaces", {}).get("status", ""), "thermal interfaces are advertised as available")
    tools = {t["name"] for t in c.request("tools/list")["result"]["tools"]}
    for op in ("interface_define", "interface_list", "interface_preview", "interface_remove", "results_interface"):
        check(op in tools, f"{op} is a reachable tool")

    print("== setup through MCP")
    setup(c, "contact_pair", tmp, stl_a, stl_b)
    v, _ = call_ok(c, "project_inspect", {})
    bodies = {b.get("name"): b for b in v.get("bodies", [])}
    check(set(bodies) >= {"blockA", "blockB"}, f"both bodies imported ({sorted(bodies)})")
    lst, _ = call_ok(c, "interface_list", {})
    und = lst.get("touching_bodies_without_interface", [])
    check(len(und) == 1 and und[0].get("coverage", {}).get("faces") == 16,
          f"the touching pair is reported as perfectly bonded before an interface is declared ({json.dumps(und)[:200]})")

    print("== C5 refused topologies")
    e = call_err(c, "interface_define", {"name": "self", "body_a": "blockA", "body_b": "blockA", "model": "perfect", "source": "user"})
    check(e.get("code") == "INVALID_PARAMS", f"the same body twice is refused ({e.get('code')})")

    print("== C1/C2 conductance interface, series resistance")
    v, _ = call_ok(c, "interface_define", {"name": "contact", "body_a": "blockA", "body_b": "blockB", "model": "conductance",
                                           "conductance": "2000 W/(m^2*K)", "source": "user"})
    itf = v.get("interface", {})
    cov = itf.get("coverage", {})
    check(cov.get("faces") == 16 and abs(cov.get("area_mm2", 0) - 100) < 1e-9, f"coverage 16 faces, 100 mm2 ({cov})")
    check(abs(itf.get("interface_resistance_k_w", 0) - 5.0) < 1e-12, f"interface resistance 5 K/W ({itf.get('interface_resistance_k_w')})")
    e = call_err(c, "interface_define", {"name": "again", "body_a": "blockB", "body_b": "blockA", "model": "insulated", "source": "user"})
    check(e.get("code") == "CONFLICTING_BOUNDARY_CONDITIONS" and "contact" in e.get("message", ""), f"a second interface on the same pair is refused ({e.get('code')})")
    run1, st1 = run(c, RUN, "conductance run")
    r1, full1 = interface_result(c, run1["job_id"])
    sa, sb = r1.get("side_a", {}).get("mean_c"), r1.get("side_b", {}).get("mean_c")
    jump, rate = r1.get("temperature_jump_a_minus_b_k", {}).get("mean"), r1.get("heat_rate_b_to_a_w")
    fa, fb = far_temps(c, run1["job_id"])
    print(f"  side A {sa:.6f} degC (series {T_AI}), side B {sb:.6f} degC ({T_BI}), jump {jump:.6f} K ({JUMP}), "
          f"rate B->A {rate:.8f} W ({-Q}), far faces {fa:.6f} / {fb:.6f} degC ({T_AFAR} / {T_BFAR})")
    check(abs(sa - T_AI) < 1e-3 and abs(sb - T_BI) < 1e-3, f"interface side temperatures within 1e-3 K ({sa}, {sb})")
    check(abs(fa - T_AFAR) < 1e-3 and abs(fb - T_BFAR) < 1e-3, f"far-face temperatures within 1e-3 K ({fa}, {fb})")
    check(abs(jump - JUMP) < 1e-3, f"temperature jump within 1e-3 K ({jump})")
    check(abs(rate - (-Q)) < 1e-5, f"heat rate from B into A within 1e-5 W ({rate})")
    bal = {b.get("name"): b for b in full1.get("body_energy_balances", [])}
    for name in ("blockA", "blockB"):
        b = bal.get(name, {})
        check(b.get("independently_closed") is True and b.get("balance_error", 1) < 1e-9,
              f"{name}'s own energy balance closes independently ({b.get('balance_error')})")
    summ = st1.get("summary", {})
    si = {i.get("name"): i for i in summ.get("interfaces", [])}
    check(si.get("contact", {}).get("heat_b_to_a_j", 0) < 0, "the summary reports the cumulative interface heat")

    print("== C8 preview")
    v, r = call_ok(c, "interface_preview", {"name": "contact", "width": 640, "height": 400})
    imgs = [m for m in r["result"]["content"] if m.get("type") == "image"]
    check(len(imgs) == 1 and imgs[0].get("mimeType") == "image/png", "interface_preview returns a PNG")
    check(v.get("interface", {}).get("coverage", {}).get("faces") == 16, "the preview reports 16 faces")

    print("== C4 model variants")
    call_ok(c, "interface_define", {"name": "contact", "body_a": "blockA", "body_b": "blockB", "model": "thin_layer", "replace": True,
                                    "layer": {"thickness": "0.5 mm", "conductivity": "1 W/(m*K)"}, "source": "user"})
    run2, _ = run(c, RUN, "thin-layer run")
    r2, _ = interface_result(c, run2["job_id"])
    d2 = max(abs(r2.get("side_a", {}).get("mean_c", 0) - sa), abs(r2.get("side_b", {}).get("mean_c", 0) - sb))
    check(d2 < 1e-9, f"a thin layer of k/d = 2000 W/(m2 K) equals the conductance result ({d2:.3g} K)")
    call_ok(c, "interface_define", {"name": "contact", "body_a": "blockA", "body_b": "blockB", "model": "perfect", "replace": True, "source": "user"})
    run3, _ = run(c, RUN, "perfect-contact run")
    r3, _ = interface_result(c, run3["job_id"])
    fa3, _ = far_temps(c, run3["job_id"])
    check(r3.get("temperature_jump_k") == 0 and abs(fa3 - (T_AFAR - JUMP)) < 1e-3, f"perfect contact: no jump, far face {fa3} degC (82.00)")
    call_ok(c, "interface_define", {"name": "contact", "body_a": "blockA", "body_b": "blockB", "model": "insulated", "replace": True, "source": "user"})
    run4, _ = run(c, dict(RUN, end_time="600 s", output_times=["600 s"]), "insulated run")
    r4, _ = interface_result(c, run4["job_id"])
    _, fb4 = far_temps(c, run4["job_id"])
    check(r4.get("heat_rate_b_to_a_w") == 0 and r4.get("cumulative_heat_b_to_a_j") == 0, f"an insulated interface carries exactly 0 W ({r4.get('heat_rate_b_to_a_w')})")
    # criterion C4 as amended (see stageC-criteria.md): round-off drift below 1e-9 K, and B's stored energy is not heat from A
    check(abs(fb4 - 20.0) < 1e-9 and abs(r4.get("side_b", {}).get("max_c", 0) - 20.0) < 1e-9,
          f"B stays at 20 degC to round-off behind an insulated interface ({fb4!r})")
    st4 = poll(c, "job_status", {"job_id": run4["job_id"]})
    bb4 = {b.get("name"): b for b in st4.get("summary", {}).get("bodies", [])}.get("blockB", {})
    check(abs(bb4.get("stored_j", 1)) < 1e-9 and bb4.get("through_interfaces_j") == 0 and bb4.get("balance_error", 1) < 1e-9,
          f"B's stored energy is round-off (below 1e-9 J), none of it through the interface ({bb4})")
    check(r4.get("side_a", {}).get("mean_c", 0) > 20.5, "A heats up behind the insulated interface")

    print("== C6 exterior condition reaching the interface")
    call_ok(c, "interface_define", {"name": "contact", "body_a": "blockA", "body_b": "blockB", "model": "conductance", "replace": True,
                                    "conductance": 2000, "source": "user"})
    call_ok(c, "selection_create", {"name": "a_all", "query": {"any": [{"type": "planar"}, {"type": "cylindrical"}, {"type": "other"}]}, "body": "blockA"})
    call_ok(c, "boundary_apply", {"name": "a_skin", "kind": "convection", "selection": "a_all", "source": "user",
                                  "convection": {"coefficient": 5, "ambient": "20 degC"}})
    v, _ = call_ok(c, "interface_list", {})
    ext = (v.get("interfaces", [{}])[0]).get("exterior_conditions_reaching_the_interface", [])
    hit = [x for x in ext if x.get("condition") == "a_skin"]
    area = hit[0].get("area_on_interface_mm2", 0) if hit else 0
    check(hit and abs(area - 100) <= 1.0, f"the skin convection is reported on 100 mm2 of the interface ({area})")
    vv, _ = call_ok(c, "setup_validate", {k: v for k, v in RUN.items() if k != "allow_duplicate"})
    conds = {x.get("name"): x for x in vv.get("model", {}).get("thermal_conditions", [])}
    skin = conds.get("a_skin", {})
    check(skin and abs(skin.get("stl_area_mm2", 0) - skin.get("mesh_area_mm2", 0) - 100) < 1.0,
          f"the mesh area of the skin excludes the interface ({skin.get('stl_area_mm2')} STL, {skin.get('mesh_area_mm2')} mesh)")
    call_ok(c, "boundary_remove", {"name": "a_skin"})

    print("== C7 mechanical bonding kept across a split thermal interface")
    call_ok(c, "boundary_apply", {"name": "anchor", "kind": "fixed", "selection": "a_far", "source": "user"})
    run5, st5 = run(c, dict(RUN, analysis="thermomechanical", end_time="3000 s", output_times=["3000 s"], stress_free_temperature="20 degC"),
                    "thermomechanical run held only at A")
    umax = st5.get("summary", {}).get("history", [{}])[-1].get("max_displacement_mm")
    check(isinstance(umax, (int, float)) and umax > 0, f"B is carried by the bond and the structure solves ({umax} mm)")
    call_ok(c, "boundary_remove", {"name": "anchor"})

    print("== C5 gaps and overlaps")
    call_ok(c, "project_save", {})
    stl_gap, stl_ovl = tmp / "gap.stl", tmp / "ovl.stl"
    box_stl(stl_gap, 30, -5, 0, 10, 10, 10, b"separate block")
    box_stl(stl_ovl, 15, -5, 0, 10, 10, 10, b"overlapping block")
    # separate projects: an overlapping body takes cells from blockA, which would change the gap seen by a third body
    call_ok(c, "project_create", {"name": "gap_pair"})
    call_ok(c, "geometry_import", {"path": str(stl_a), "units": "mm", "name": "blockA"})
    call_ok(c, "geometry_import", {"path": str(stl_gap), "units": "mm", "name": "far"})
    call_ok(c, "geometry_place", {"body": "blockA", "position": [-10, 0]})
    call_ok(c, "geometry_place", {"body": "far", "position": [35, 0]})  # x 30..40: 30 mm from blockA
    for bname in ("blockA", "far"):
        call_ok(c, "material_assign", {"body": bname, "material": "steel_plate_demo", "source": "default"})
    call_ok(c, "mesh_generate", {"element_size": "2.5 mm"})
    e = call_err(c, "interface_define", {"name": "gap", "body_a": "blockA", "body_b": "far", "model": "conductance", "conductance": 1000, "source": "user"})
    check(e.get("code") == "GEOMETRY_INVALID" and "apart" in e.get("message", "") and "30" in e.get("message", ""),
          f"a gap is refused with its size ({e.get('message')})")
    call_ok(c, "project_create", {"name": "overlap_pair"})
    call_ok(c, "geometry_import", {"path": str(stl_a), "units": "mm", "name": "blockA"})
    call_ok(c, "geometry_import", {"path": str(stl_ovl), "units": "mm", "name": "overlap"})
    call_ok(c, "geometry_place", {"body": "blockA", "position": [-10, 0]})
    call_ok(c, "geometry_place", {"body": "overlap", "position": [-2, 0]})  # x -7..3: overlaps blockA over 7 mm
    for bname in ("blockA", "overlap"):
        call_ok(c, "material_assign", {"body": bname, "material": "steel_plate_demo", "source": "default"})
    call_ok(c, "mesh_generate", {"element_size": "2.5 mm"})
    e = call_err(c, "interface_define", {"name": "ovl", "body_a": "blockA", "body_b": "overlap", "model": "conductance", "conductance": 1000,
                                         "source": "user"})
    check(e.get("code") == "GEOMETRY_INVALID" and "overlap" in e.get("message", ""), f"overlapping bodies are refused as an overlap ({e.get('message')})")
    c.close()

    print("== C3 save, reopen in a new server process, reproduce")
    sock, ready = tmp / "run" / "ctl.sock", tmp / "ready"
    srv = subprocess.Popen([str(SERVER), "--socket", str(sock), "--ready-file", str(ready)] + common, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(200):
        if ready.exists():
            break
        time.sleep(0.05)
    d = Client(["--connect", str(sock)])
    d.initialize()
    call_ok(d, "project_open", {"path": str(ws / "contact_pair")})
    v, _ = call_ok(d, "interface_list", {})
    m = (v.get("interfaces", [{}])[0])
    check(m.get("model") == "conductance" and m.get("conductance_w_m2k") == 2000, f"the interface survives save and reopen ({m.get('model')})")
    call_ok(d, "mesh_generate", {"element_size": "2.5 mm"})
    run6, _ = run(d, RUN, "reproduced run")
    r6, _ = interface_result(d, run6["job_id"])
    rel = max(abs(r6.get("side_a", {}).get("mean_c", 0) - sa) / abs(sa), abs(r6.get("heat_rate_b_to_a_w", 0) - rate) / abs(rate))
    check(rel <= 1e-12, f"the reopened study reproduces the interface result ({rel:.3g} relative)")
    d.close()
    srv.send_signal(signal.SIGTERM)
    try:
        srv.wait(timeout=20)
    except subprocess.TimeoutExpired:
        srv.kill()
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{'CONTACT FLOW TESTS FAILED' if FAIL else 'ALL CONTACT FLOW TESTS PASSED'}: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
