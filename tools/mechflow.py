#!/usr/bin/env python3
"""mechflow.py - the first complete mechanical path, driven over MCP stdio exactly as an AI host would drive it:

  printed part (STL) -> rigid bodies with declared fill -> joints -> geared DC motor, encoder and sampled PID ->
  validation -> dynamics job (with time-step check) -> histories, load envelope and snapshots ->
  selections, material and mesh of the part -> FEM assessment at the peak hinge moment -> stress query

plus the refusals that keep results honest: plain numbers without units, joints without frames, URDF without a root
connection, and a load assessment that does not say where a joint's load enters the part.

A second project drives contact: two printed fingers (STL parts) squeeze a block through open-loop effort actuators, with
collision shapes declared per body, a static floor, contact enabled in the study, the grasp forces, impacts reported as
impulses, and a FEM assessment of one finger with its joint load and its contact load on separate selections.

This is a protocol-level test with a scripted client, NOT a test with a real AI model.
Run from the repository root after `make am`:
    python3 tools/mechflow.py
"""
import json
import math
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcptest import MCP, Client  # noqa: E402

PASS = 0
FAIL = 0
FLEX_CUT_HZ = 2000


def check(cond, what):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"  FAIL: {what}")


def box_stl(path, lo, hi):
    """closed box between corners lo and hi (mm), outward winding"""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    v = [(x0, y0, z0), (x1, y0, z0), (x0, y1, z0), (x1, y1, z0), (x0, y0, z1), (x1, y0, z1), (x0, y1, z1), (x1, y1, z1)]
    faces = [(0, 2, 3), (0, 3, 1), (4, 5, 7), (4, 7, 6), (0, 1, 5), (0, 5, 4), (2, 6, 7), (2, 7, 3), (0, 4, 6), (0, 6, 2), (1, 3, 7), (1, 7, 5)]
    with open(path, "wb") as f:
        f.write(b"mechflow box".ljust(80, b" "))
        f.write(struct.pack("<I", len(faces)))
        for a, b, c in faces:
            f.write(struct.pack("<3f", 0, 0, 0))
            for i in (a, b, c):
                f.write(struct.pack("<3f", *v[i]))
            f.write(b"\0\0")


def call(c, name, args):
    r = c.call(name, args)
    res = r.get("result", {})
    return res.get("structuredContent") or {}


def call_ok(c, name, args, what=None):
    sc = call(c, name, args)
    ok = sc.get("ok") is True
    check(ok, f"{what or name}: {json.dumps(sc.get('error'))[:600] if not ok else ''}")
    return sc.get("value", {})


def call_err(c, name, args, code, what):
    sc = call(c, name, args)
    err = sc.get("error") or {}
    check(sc.get("ok") is False and err.get("code") == code, f"{what}: expected {code}, got {json.dumps(err)[:400]}")
    return err


def wait_job(c, job):
    status = {}
    for _ in range(120):  # the test client waits 20 s for a response, so each poll waits less
        status = call_ok(c, "job_status", {"job_id": job, "wait_seconds": 10}, "job_status")
        if status.get("state") not in ("queued", "running"):
            break
    return status


def gripper_flow(c, tmp):
    print("== contact: printed fingers squeeze a block (collision shapes, environment, contact study)")
    fl_stl, fr_stl = tmp / "finger_l.stl", tmp / "finger_r.stl"
    box_stl(fl_stl, (-10, 21, 70), (10, 41, 130))  # CAD frame: pads 1 mm from a 40 mm block centred at z = 100 mm
    box_stl(fr_stl, (-10, -41, 70), (10, -21, 130))
    call_ok(c, "project_create", {"name": "gripper_demo", "description": "two printed fingers squeeze a block"})
    call_ok(c, "geometry_import", {"path": str(fl_stl), "units": "mm", "name": "finger_l_part"})
    call_ok(c, "geometry_import", {"path": str(fr_stl), "units": "mm", "name": "finger_r_part"})
    # the import centres each part on the plate; place them as assembled so their meshes do not overlap
    call_ok(c, "geometry_place", {"body": "finger_l_part", "position": [0, 31], "z_offset": 70})
    call_ok(c, "geometry_place", {"body": "finger_r_part", "position": [0, -31], "z_offset": 70})
    fill = {"model": "solid", "density": "1240 kg/m^3", "source": "user"}
    call_ok(c, "mech_assembly_import", {"name": "gripper", "parts": [{"body": "finger_l_part", "name": "finger_l", "fill": fill},
                                                                    {"body": "finger_r_part", "name": "finger_r", "fill": fill}]})
    block_mp = {"mass": "200 g", "com": ["0 mm", "0 mm", "0 mm"], "provenance": "measured",
                "inertia": {"ixx": "53.333 kg*mm^2", "iyy": "53.333 kg*mm^2", "izz": "53.333 kg*mm^2", "ixy": 0, "ixz": 0, "iyz": 0}}
    call_err(c, "mech_body_define", {"name": "block", "mass_properties": block_mp,
                                     "collision": [{"name": "block", "type": "box", "size": ["40 mm", "40 mm", "40 mm"], "friction": 0.6}]},
             "PRECONDITION_FAILED", "a friction coefficient without its source is a missing input")
    call_err(c, "mech_body_define", {"name": "block", "mass_properties": block_mp,
                                     "collision": [{"name": "block", "type": "box", "size": [40, 40, 40], "friction": 0.6, "friction_source": "user"}]},
             "INVALID_PARAMS", "shape sizes without units are refused")
    call_ok(c, "mech_body_define", {"name": "block", "mass_properties": block_mp,
                                    "collision": [{"name": "block", "type": "box", "size": ["40 mm", "40 mm", "40 mm"], "friction": 0.6, "friction_source": "user"}]})
    mf = 1240 * 20 * 20 * 60 * 1e-9
    for side, y in (("l", 31), ("r", -31)):
        v = call_ok(c, "mech_body_define", {"name": f"finger_{side}", "collision": [
            {"name": f"pad_{side}", "type": "box", "size": ["20 mm", "20 mm", "60 mm"], "pose": {"position": ["0 mm", f"{y} mm", "100 mm"]},
             "friction": 0.6, "friction_source": "user"}]})
    fingers = {b["name"]: b for b in v.get("assembly", {}).get("bodies", [])}
    check(abs(fingers.get("finger_r", {}).get("mass_kg", 0) - mf) < 1e-9 and fingers.get("finger_r", {}).get("part") == "finger_r_part",
          f"adding collision shapes keeps the part link and the computed mass ({fingers.get('finger_r')})")
    call_err(c, "mech_environment_define", {"shapes": [{"name": "floor", "type": "plane", "friction": 0.5}]}, "PRECONDITION_FAILED",
             "environment friction without its source")
    call_ok(c, "mech_environment_define", {"shapes": [{"name": "floor", "type": "plane", "friction": 0.5, "friction_source": "default"}]})
    call_ok(c, "mech_joint_define", {"joint": {"name": "float", "type": "free", "parent": "world", "child": "block",
                                               "parent_frame": {"position": ["0 mm", "0 mm", "100 mm"]}, "child_frame": {}}})
    for side, ax in (("l", -1), ("r", 1)):
        call_ok(c, "mech_joint_define", {"joint": {"name": f"slide_{side}", "type": "prismatic", "parent": "world", "child": f"finger_{side}",
                                                   "parent_frame": {}, "child_frame": {}, "axis": [0, ax, 0], "motion": "actuated"}})
        call_ok(c, "mech_component_define", {"kind": "actuator", "definition": {"name": f"act_{side}", "type": "effort", "joint": f"slide_{side}",
                                                                                "effort_limit": "5 N"}})
        call_ok(c, "mech_component_define", {"kind": "controller", "definition": {
            "name": f"squeeze_{side}", "type": "open_loop", "actuator": f"act_{side}", "period": "1 ms", "reference": {"type": "constant", "value": "2.5 N"}}})
    settings = {"end_time": "0.3 s", "max_step": "0.2 ms", "record_period": "1 ms"}
    snapshots = [f"{0.2 * k:.1f} ms" for k in range(15, 46)] + ["0.25 s"]  # every step from 3 to 9 ms: the pads land in this window
    call_ok(c, "mech_study_settings", {"settings": settings, "snapshots": snapshots})
    v = call_ok(c, "mech_study_validate", {})
    check(any(w.get("code") == "CONTACT_DISABLED" for w in v.get("warnings", [])), f"collision shapes with contact disabled are flagged ({v.get('warnings')})")
    check(any("pass through" in a for a in v.get("fidelity", {}).get("assumptions", [])), "without contact the fidelity says parts pass through each other")
    call_ok(c, "mech_study_settings", {"settings": dict(settings, contact={"enabled": True}), "snapshots": snapshots})
    v = call_ok(c, "mech_study_validate", {})
    notes = {n.get("code") for n in v.get("notes", [])} | {w.get("code") for w in v.get("warnings", [])}
    check(v.get("valid") is True and "CONTACT_PARAMETER_ASSUMED" in notes and "CONTACT_TIME_STEPPING" in notes,
          f"contact study validates, with the assumed floor friction and the time-stepping note ({sorted(notes)})")
    check(any("Coulomb" in a for a in v.get("fidelity", {}).get("assumptions", [])), "fidelity lists the contact assumptions")

    v = call_ok(c, "mech_dynamics_run", {"time_step_check": True, "label": "squeeze and hold"})
    job = v.get("job_id")
    status = wait_job(c, job)
    check(status.get("state") == "succeeded", f"contact job succeeded ({status.get('state')}: {json.dumps(status.get('error'))[:400]})")
    s = call_ok(c, "mech_results_query", {"job_id": job, "what": "summary"}).get("summary", {})
    co = s.get("contact", {})
    pairs = {frozenset((p["shape_a"], p["shape_b"])): p for p in co.get("pairs", [])}  # pair order follows the body order
    pl, pr = pairs.get(frozenset(("block", "pad_l")), {}), pairs.get(frozenset(("block", "pad_r")), {})
    pname = f"{pl.get('shape_a')}.{pl.get('shape_b')}"
    weight = 0.2 * 9.80665
    fric = pl.get("friction_force_at_end_N", 0) + pr.get("friction_force_at_end_N", 0)
    print(f"  end of hold: normal forces {pl.get('normal_force_at_end_N', 0):.9f} / {pr.get('normal_force_at_end_N', 0):.9f} N, friction {fric:.9f} N "
          f"(weight {weight:.9f} N); impacts: {co.get('impact_steps')} steps, largest impulse {pl.get('peak_impact_impulse_Ns', 0):.5f} N s; "
          f"max penetration {co.get('max_penetration_m', -1):.2e} m, solver sweeps {co.get('solver_max_sweeps')}, unconverged steps {co.get('steps_not_converged')}")
    check(abs(pl.get("normal_force_at_end_N", 0) - 2.5) < 1e-6 and abs(pr.get("normal_force_at_end_N", 0) - 2.5) < 1e-6, "normal force equals the squeeze force")
    check(abs(fric - weight) < 1e-6, "friction on the two pads carries the block's weight")
    mu_n = sum(p.get("friction_coefficient", 0) * p.get("normal_force_at_end_N", 0) for p in (pl, pr))
    used = sum(p.get("friction_utilization_at_end", 9) * p.get("friction_coefficient", 0) * p.get("normal_force_at_end_N", 0) for p in (pl, pr))
    print(f"  grasp friction utilisation {fric / mu_n if mu_n else -1:.4f} (slip margin {1 - fric / mu_n if mu_n else -1:.4f}); per pad "
          f"{pl.get('friction_utilization_at_end', -1):.4f} / {pr.get('friction_utilization_at_end', -1):.4f}")
    check(abs(used - weight) < 1e-6 and 0 < fric / mu_n < 1, "slip margins reported per pad and consistent with the grasp as a whole")
    check(co.get("impact_steps", 0) >= 1 and pl.get("peak_impact_impulse_Ns", 0) > 0 and "impact_steps_note" in co,
          "closing pads are reported as impacts with impulses, excluded from force peaks")
    check(co.get("steps_not_converged", 1) == 0, "contact solver converged in every step")
    ts = s.get("time_step_sensitivity", {})
    for item in sorted(ts.get("changes", []), key=lambda x: -x["relative_change"])[:5]:
        print(f"    {item['quantity']}: {item['value']:.6g} -> {item['value_half_step']:.6g} ({item['relative_change']:.2e})")
    changes = {i["quantity"]: i for i in ts.get("changes", [])}
    check(changes.get(f"contact.{pname}.normal_force_at_end_N", {}).get("relative_change", 1) < 1e-6,
          "time-step check covers the contact forces (the held force does not depend on the step)")
    h = call_ok(c, "mech_results_query", {"job_id": job, "what": "history", "channels": ["float.z", f"contact.{pname}.normal_force"], "max_points": 400})
    z = h.get("series", {}).get("float.z", [])
    check(len(z) > 10 and abs(z[-1]) < 1e-3 and abs(z[-1] - z[len(z) // 2]) < 1e-9, f"block held: drop {z[-1] * 1e3 if z else 0:.4f} mm, no creep in the hold")
    snaps = call_ok(c, "mech_results_query", {"job_id": job, "what": "snapshots"}).get("snapshots", [])
    by_label = {x["label"]: x for x in snaps}
    impulsive = [x["label"] for x in snaps if x.get("contact_impulsive")]
    check(f"peak_contact:{pname}" in by_label and not by_label[f"peak_contact:{pname}"].get("contact_impulsive") and "t=0.25" in by_label,
          f"peak-contact snapshot of persistent contact stored ({sorted(by_label)[:5]})")
    check(len(impulsive) >= 1 and all(not x.get("contact_impulsive") for x in snaps if x["kind"] != "requested"),
          f"requested snapshots at the landing are flagged impulsive ({impulsive}); peak snapshots never are")

    print("== FEM assessment of a finger: joint load and contact load on their own selections")
    for side, at_pad, at_mount in (("l", "min", "max"), ("r", "max", "min")):
        call_ok(c, "selection_create", {"name": f"{side}_pad", "body": f"finger_{side}_part", "query": {"plane": {"axis": "y", "at": at_pad}},
                                        "description": "pad face touching the block", "source": "user"})
        call_ok(c, "selection_create", {"name": f"{side}_mount", "body": f"finger_{side}_part", "query": {"plane": {"axis": "y", "at": at_mount}},
                                        "description": "back face driven by the slide", "source": "user"})
        call_ok(c, "material_assign", {"body": f"finger_{side}_part", "material": "pla_generic_demo", "source": "user"})
    call_ok(c, "mesh_generate", {"element_size": "2 mm"})
    if impulsive:
        call_err(c, "mech_fem_assess", {"job_id": job, "snapshot": impulsive[0], "body": "finger_l",
                                        "attachments": [{"joint": "slide_l", "selection": "l_mount"}, {"contact_shape": "pad_l", "selection": "l_pad"}]},
                 "UNSUPPORTED", "no structural load from an impact snapshot")
    call_err(c, "mech_fem_assess", {"job_id": job, "snapshot": "t=0.25", "body": "finger_l", "attachments": [{"joint": "slide_l", "selection": "l_mount"}]},
             "PRECONDITION_FAILED", "the contact load needs its own attachment selection")
    call_err(c, "mech_fem_assess", {"job_id": job, "snapshot": "t=0.25", "body": "finger_l",
                                    "attachments": [{"joint": "slide_l", "selection": "l_mount"}, {"contact_shape": "pad_r", "selection": "l_pad"}]},
             "NOT_FOUND", "a contact shape of another body is refused")
    v = call_ok(c, "mech_fem_assess", {"job_id": job, "snapshot": "t=0.25", "body": "finger_l",
                                       "attachments": [{"joint": "slide_l", "selection": "l_mount"}, {"contact_shape": "pad_l", "selection": "l_pad"}]})
    a = v.get("assessment", {})
    loads = {t.get("joint") or t.get("contact_shape"): t for t in a.get("joint_loads", [])}
    cf = loads.get("pad_l", {}).get("force_N", [0, 0, 0])
    print(f"  contact load on the pad {[round(x, 6) for x in cf]} N; joint load {[round(x, 6) for x in loads.get('slide_l', {}).get('force_N', [0, 0, 0])]} N")
    check(abs(cf[1] - 2.5) < 1e-6 and -weight < cf[2] < 0, "the block pushes the pad outward with the squeeze force and drags it down by friction")
    errs = [t.get("force_reproduction_error", 1) for t in a.get("joint_loads", [])]
    check(len(errs) == 2 and max(errs) < 1e-9, f"joint and contact loads reproduced on their selections ({errs})")
    check(any("tractions" in x for x in v.get("fidelity", {}).get("assumptions", [])), "assessment states how contact loads enter the part")
    off = a.get("mass_consistency", {}).get("mesh_alignment_offset_mm", [9, 9, 9])
    check(abs(abs(off[1]) - 1) < 1e-6 and abs(off[0]) < 1e-6 and abs(off[2]) < 1e-6 and any("offset from the placed part" in w for w in a.get("warnings", [])),
          f"the 1 mm voxel snapping of the finger is measured, reported and warned about ({off})")
    status = wait_job(c, v.get("job_id"))
    check(status.get("state") == "succeeded", f"finger FEM job succeeded ({status.get('state')}: {json.dumps(status.get('error'))[:400]})")
    sr = (status.get("summary") or {}).get("mechanical_assessment", {}).get("support_reactions", {})
    print(f"  isostatic support reactions {sr.get('largest_component_N', -1):.2e} N ({sr.get('relative_to_load_scale', -1):.2e} of the load scale)")
    ok_sr = 0 <= sr.get("relative_to_load_scale", 1) < 1e-6
    check(ok_sr, "joint load, contact load and body force balance: support reactions vanish")
    if not ok_sr:
        print(json.dumps({k: a.get(k) for k in ("balance", "mass_consistency", "joint_loads", "body_motion_fem_axes", "time_s")}, indent=1)[:4000])

    print("== orthotropic printed finger: material record with sources, print axes, strength criterion")
    elastic = {"E1": "3.2 GPa", "E2": "3.0 GPa", "E3": "2.4 GPa", "nu12": 0.35, "nu13": 0.30, "nu23": 0.32,
               "G12": "1.15 GPa", "G23": "0.85 GPa", "G13": "0.9 GPa"}
    strength = {"Xt": "50 MPa", "Xc": "60 MPa", "Yt": "45 MPa", "Yc": "55 MPa", "Zt": "25 MPa", "Zc": "50 MPa",
                "S12": "25 MPa", "S23": "15 MPa", "S31": "15 MPa"}
    call_err(c, "mech_material_define", {"material": {"id": "pla_demo", "elastic": elastic}}, "PRECONDITION_FAILED",
             "elastic constants without their source are a missing input")
    call_err(c, "mech_material_define", {"material": {"id": "pla_demo", "elastic": dict(elastic, E1=3.2e9, source="assumed")}}, "INVALID_PARAMS",
             "a modulus without a unit is refused")
    call_err(c, "mech_material_define", {"material": {"id": "pla_demo", "elastic": dict(elastic, E2="3.2 GPa", nu12=1.2, source="assumed")}}, "INVALID_PARAMS",
             "inadmissible Poisson ratios are refused")
    v = call_ok(c, "mech_material_define", {"material": {
        "id": "pla_demo", "density": "1240 kg/m^3",
        "elastic": dict(elastic, source="assumed", reference="illustrative values for this test, not measured"),
        "strength": dict(strength, source="assumed", reference="illustrative values for this test, not measured"),
        "print": {"process": "FFF", "layer_height": "0.2 mm", "raster": "0/90"}}})
    check(len(v.get("warnings", [])) == 2 and "Tsai-Hahn" in json.dumps(v.get("material", {})), f"assumed data and the default interaction are flagged ({v.get('warnings')})")
    base = {"job_id": job, "snapshot": "t=0.25", "body": "finger_l",
            "attachments": [{"joint": "slide_l", "selection": "l_mount"}, {"contact_shape": "pad_l", "selection": "l_pad"}]}
    call_err(c, "mech_fem_assess", dict(base, material_model={"type": "orthotropic", "material": "nylon_unknown"}), "NOT_FOUND", "an undefined material is refused")
    call_err(c, "mech_fem_assess", dict(base, material_model={"type": "orthotropic", "material": "pla_demo", "build_direction": "z", "raster_reference": "-z"}),
             "INVALID_PARAMS", "a raster reference along the build direction is refused")
    ratios = {}
    for bd in ("z", "y"):
        v = call_ok(c, "mech_fem_assess", dict(base, material_model={"type": "orthotropic", "material": "pla_demo", "build_direction": bd, "raster_reference": "x",
                                                                    "raster_angle": "0 deg", "criterion": "tsai_wu"}))
        ojob = v.get("job_id")
        check(any("orthotropic linear elastic" in x for x in v.get("fidelity", {}).get("assumptions", [])) and
              not any(x.startswith("isotropic linear elastic") for x in v.get("fidelity", {}).get("assumptions", [])),
              "fidelity names the orthotropic model instead of the isotropic one")
        status = wait_job(c, ojob)
        check(status.get("state") == "succeeded", f"orthotropic job succeeded ({status.get('state')}: {json.dumps(status.get('error'))[:400]})")
        if bd == "z":
            call_err(c, "results_query", {"job_id": ojob, "quantity": "von_mises"}, "UNSUPPORTED", "the shared isotropic result queries refuse orthotropic results")
        sq = call_ok(c, "mech_structure_query", {"job_id": ojob}).get("summary", {})
        st = sq.get("strength", {})
        osr = sq.get("mechanical_assessment", {}).get("support_reactions", {})
        ratios[bd] = st.get("smallest_strength_ratio", 0)
        print(f"  build direction {bd}: largest displacement {sq.get('largest_displacement', {}).get('value_mm', -1):.6f} mm; Tsai-Wu smallest strength ratio "
              f"{st.get('smallest_strength_ratio', -1):.1f} ({st.get('governing_mode')}) at {st.get('location_mm')}; support reactions "
              f"{osr.get('relative_to_load_scale', -1):.1e} of the load scale; equilibrium {sq.get('solver', {}).get('equilibrium_error', -1):.1e}")
        check(0 <= osr.get("relative_to_load_scale", 1) < 1e-6 and sq.get("solver", {}).get("equilibrium_error", 1) < 1e-9,
              "orthotropic solve: self-equilibrated loads, equilibrium closed")
        check(st.get("criterion") == "tsai_wu" and st.get("smallest_strength_ratio", 0) > 1 and st.get("data_source") == "assumed",
              "strength summary names the criterion, the ratio and the data source")
        fi = call_ok(c, "mech_structure_query", {"job_id": ojob, "quantity": "failure_index", "largest": 3}).get("largest", [])
        check(len(fi) == 3 and fi[0]["value"] >= fi[1]["value"] >= fi[2]["value"] and abs(fi[0]["value"] * st.get("smallest_strength_ratio", 0) - 1) < 1e-9
              and "governing_mode" in fi[0], "failure indices listed largest first with modes; the largest is the summary's")
        if bd == "z":
            ms = call_ok(c, "mech_structure_query", {"job_id": ojob, "quantity": "material_stress", "component": "33", "largest": 1}).get("largest", [])
            check(len(ms) == 1 and ms[0]["value"] != 0, "material-axes stress query")
            call_err(c, "mech_structure_query", {"job_id": ojob, "quantity": "material_stress", "component": "xx"}, "INVALID_PARAMS",
                     "material stresses use material components")
    check(ratios.get("z", 0) > 0 and ratios.get("y", 0) > 0 and abs(ratios["z"] / ratios["y"] - 1) > 1e-3,
          f"the print orientation changes the strength ratio ({ratios})")


def main():
    if not MCP.exists():
        print("navier-mcp not built (run: make am)")
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="nvmechflow"))
    ws = tmp / "ws"
    arm_stl = tmp / "arm.stl"
    box_stl(arm_stl, (0, -8, -5), (120, 8, 5))  # printed arm in the CAD frame: hinge axis along y through the origin
    common = ["--workspace", str(ws), "--allow-read", str(tmp)]
    c = Client(["--embedded"] + common)
    c.initialize()
    try:
        tools = {t["name"] for t in c.request("tools/list")["result"]["tools"]}
        needed = {"mech_assembly_import", "mech_body_define", "mech_joint_define", "mech_environment_define", "mech_material_define", "mech_structure_query",
                  "mech_modal_run", "mech_transient_assess", "mech_vibration_query", "mech_flexible_reduce", "mech_flexible_attach", "mech_flexible_detach", "mech_flexible_stress",
                  "mech_component_define", "mech_study_settings",
                  "mech_assembly_inspect", "mech_study_validate", "mech_dynamics_run", "mech_results_query", "mech_fem_assess"}
        check(needed <= tools, f"mechanics tools listed (missing {sorted(needed - tools)})")
        caps = call_ok(c, "capabilities_get", {})
        levels = {lv["level"]: lv for lv in caps.get("mechanics", {}).get("fidelity_levels", [])}
        check(levels.get(2, {}).get("status", "").startswith("available") and levels.get(4, {}).get("status", "").startswith("available")
              and levels.get(5, {}).get("status") == "not available", "capabilities state which fidelity levels exist")

        print("== assembly: printed arm part, explicit base and payload, joints")
        call_ok(c, "project_create", {"name": "arm_demo", "description": "printed arm lifted by a geared motor"})
        v = call_ok(c, "geometry_import", {"path": str(arm_stl), "units": "mm", "name": "arm_part"})
        check(v.get("body", {}).get("closed_solid") is True, "arm STL is a closed solid")
        call_err(c, "mech_assembly_import", {"parts": [{"body": "arm_part", "name": "arm", "fill": {"model": "solid", "density": 1240, "source": "user"}}]},
                 "INVALID_PARAMS", "a density without a unit is refused")
        v = call_ok(c, "mech_assembly_import", {"name": "arm_rig", "parts": [{"body": "arm_part", "name": "arm",
                                                                                "fill": {"model": "solid", "density": "1240 kg/m^3", "source": "user"}}]})
        arm = v["assembly"]["bodies"][0]
        mass = 1240 * 120 * 16 * 10 * 1e-9
        check(abs(arm["mass_kg"] - mass) < 1e-6 * mass and abs(arm["com_mm"][0] - 60) < 1e-4, f"arm mass {arm['mass_kg']:.6f} kg at x = {arm['com_mm'][0]:.4f} mm")
        check(any("gravity" in a["subject"] for a in v["assembly"]["assumptions"]), "default gravity recorded as an assumption")
        call_ok(c, "mech_body_define", {"name": "base", "mass_properties": {"mass": "0.8 kg", "com": ["0 mm", "0 mm", "-20 mm"],
                                                                           "inertia": {"ixx": "400 kg*mm^2", "iyy": "400 kg*mm^2", "izz": "300 kg*mm^2"},
                                                                           "provenance": "measured"}})
        call_ok(c, "mech_body_define", {"name": "payload", "mass_properties": {"mass": "150 g", "com": ["0 mm", "0 mm", "0 mm"],
                                                                              "inertia": {"ixx": "20 kg*mm^2", "iyy": "20 kg*mm^2", "izz": "20 kg*mm^2",
                                                                                          "ixy": 0, "ixz": 0, "iyz": 0},
                                                                              "provenance": "measured"}})
        call_err(c, "mech_joint_define", {"joint": {"name": "shoulder", "type": "revolute", "parent": "base", "child": "arm", "axis": [0, -1, 0]}},
                 "PRECONDITION_FAILED", "a joint without frames is a missing input")
        call_ok(c, "mech_joint_define", {"joint": {"name": "mount", "type": "fixed", "parent": "world", "child": "base", "parent_frame": {}, "child_frame": {}}})
        call_ok(c, "mech_joint_define", {"joint": {"name": "shoulder", "type": "revolute", "parent": "base", "child": "arm",
                                                   "parent_frame": {"position": ["0 mm", "0 mm", "0 mm"]}, "child_frame": {"position": ["0 mm", "0 mm", "0 mm"]},
                                                   "axis": [0, -1, 0], "motion": "actuated",
                                                   "limits": {"lower": "-30 deg", "upper": "120 deg", "restitution": 0},
                                                   "friction": {"coulomb": "0.002 N*m", "regularization_velocity": "0.5 deg/s"}}})
        call_ok(c, "mech_joint_define", {"joint": {"name": "payload_mount", "type": "fixed", "parent": "arm", "child": "payload",
                                                   "parent_frame": {"position": ["120 mm", "0 mm", "0 mm"]}, "child_frame": {}}})
        v = call_ok(c, "mech_assembly_inspect", {"analyze": True})
        model = v.get("model", {})
        check(model.get("compiles") and model.get("assembles") and model.get("coordinates") == 1, f"model compiles with one coordinate ({model})")

        print("== actuator, sensor, controller, settings")
        call_err(c, "mech_component_define", {"kind": "actuator", "definition": {"name": "m1", "type": "dc_motor", "joint": "shoulder", "gear_ratio": 150,
                                                                                 "efficiency": 0.6, "rotor_inertia": 1.5e-7, "resistance": "4 ohm",
                                                                                 "torque_constant": "0.01 N*m/A", "back_emf_constant": "0.01 V*s/rad",
                                                                                 "voltage_limit": "12 V"}},
                 "INVALID_PARAMS", "rotor inertia as a plain number is refused")
        call_ok(c, "mech_component_define", {"kind": "actuator", "definition": {
            "name": "motor", "type": "dc_motor", "joint": "shoulder", "gear_ratio": 150, "efficiency": 0.6, "rotor_inertia": "1.5 g*cm^2",
            "resistance": "4 ohm", "torque_constant": "10 mN*m/A", "back_emf_constant": "0.01 V*s/rad", "voltage_limit": "12 V", "current_limit": "1.5 A",
            "motor_viscous": "1e-7 N*m*s/rad", "motor_coulomb": "0.5 mN*m", "motor_coulomb_vreg": "10 rad/s", "rated_effort": "1 N*m",
            "source": "demonstration values, not a datasheet"}})
        call_ok(c, "mech_component_define", {"kind": "sensor", "definition": {
            "name": "encoder", "type": "joint_position", "joint": "shoulder", "period": "1 ms", "latency_samples": 1, "resolution": "0.09 deg",
            "noise_std": "0.01 deg", "seed": 11}})
        call_ok(c, "mech_component_define", {"kind": "controller", "definition": {
            "name": "pid", "type": "pid", "actuator": "motor", "feedback": "encoder", "loop": "position", "period": "1 ms", "delay_samples": 1,
            "kp": 40, "ki": 80, "kd": 0.5, "derivative_filter": "3 ms", "output_limits": ["-12 V", "12 V"], "antiwindup": "clamp",
            "reference": {"type": "min_jerk", "start": "0.1 s", "from": "0 deg", "to": "60 deg", "duration": "0.6 s"}}})
        call_ok(c, "mech_study_settings", {"settings": {"end_time": "2 s", "max_step": "0.5 ms", "record_period": "2 ms"}, "snapshots": ["0.4 s"]})
        v = call_ok(c, "mech_study_validate", {})
        check(v.get("valid") is True and len(v.get("spec_hash", "")) == 64, f"study validates ({json.dumps(v.get('errors'))[:300]} {json.dumps(v.get('missing_inputs'))[:300]})")
        check(v.get("fidelity", {}).get("level") == 2 and len(v["fidelity"].get("assumptions", [])) >= 3, "fidelity level and assumptions reported")

        print("== dynamics run")
        v = call_ok(c, "mech_dynamics_run", {"time_step_check": True, "label": "lift 60 deg"})
        job = v.get("job_id")
        status = wait_job(c, job)
        check(status.get("state") == "succeeded", f"dynamics job succeeded ({status.get('state')}: {json.dumps(status.get('error'))[:400]})")
        s = call_ok(c, "mech_results_query", {"job_id": job, "what": "summary"}).get("summary", {})
        ctl = s.get("controllers", [{}])[0]
        act = s.get("actuators", [{}])[0]
        en = s.get("energy_J", {})
        print(f"  tracking rms {math.degrees(ctl.get('rms_error', 0)):.3f} deg, final {math.degrees(ctl.get('final_error', 0)):.4f} deg; "
              f"peak torque {act.get('peak_effort', 0):.4f} N m, peak current {act.get('peak_current_A', 0):.3f} A; "
              f"electrical {act.get('electrical_energy_J', 0):.4f} J, copper {act.get('copper_loss_J', 0):.4f} J; balance residual {en.get('balance_residual', 1):.2e} J")
        check(abs(ctl.get("final_error", 1)) < math.radians(0.5), "arm settles within 0.5 deg of 60 deg")
        check(0.05 < act.get("peak_effort", 0) < 1.35, "peak joint torque between the gravity torque and the stall torque")
        check(abs(en.get("balance_residual", 1)) < 1e-3 * max(1e-9, en.get("input_work", 1)), "energy ledger closes to 0.1% of the input work")
        ts = s.get("time_step_sensitivity", {})
        print(f"  time-step check: largest relative change {ts.get('largest_relative_change', -1):.2e} when halving the step")
        for item in sorted(ts.get("changes", []), key=lambda x: -x["relative_change"])[:4]:
            print(f"    {item['quantity']}: {item['value']:.6g} -> {item['value_half_step']:.6g} ({item['relative_change']:.2e})")
        print(f"  energy: {json.dumps(en)}")
        check(0 <= ts.get("largest_relative_change", 1) < 0.05, "time-step check reports changes below 5% (peaks are sampled at step ends)")
        ch = call_ok(c, "mech_results_query", {"job_id": job, "what": "channels"}).get("channels", [])
        names = {x["name"] for x in ch}
        check({"shoulder.q", "motor.current", "pid.error", "encoder.measured", "shoulder.My"} <= names, "history channels for joint, motor, controller, sensor, reactions")
        h = call_ok(c, "mech_results_query", {"job_id": job, "what": "history", "channels": ["shoulder.q", "pid.reference"], "time_from": 1.4, "time_to": 2.0,
                                              "max_points": 50})
        q = h.get("series", {}).get("shoulder.q", [])
        check(len(q) <= 51 and len(q) > 10 and abs(q[-1] - math.radians(60)) < math.radians(0.5), f"decimated history window ({len(q)} points)")
        env = call_ok(c, "mech_results_query", {"job_id": job, "what": "envelope"}).get("envelope", [])
        sh = [e for e in env if e["joint"] == "shoulder"][0]
        print(f"  shoulder peak moment {sh['peak_moment']['magnitude_Nm']:.4f} N m at {sh['peak_moment']['time_s']:.3f} s, peak force {sh['peak_force']['magnitude_N']:.3f} N")
        check(sh["peak_force"]["magnitude_N"] > 9.81 * (mass + 0.15) * 0.9, "hinge carries at least the weight of arm and payload")
        snaps = call_ok(c, "mech_results_query", {"job_id": job, "what": "snapshots"}).get("snapshots", [])
        labels = {x["label"] for x in snaps}
        check("t=0.4" in labels and "peak_moment:shoulder" in labels, f"snapshots stored ({sorted(labels)[:6]})")

        print("== FEM assessment of the arm at the peak hinge moment")
        call_ok(c, "selection_create", {"name": "hinge", "body": "arm_part", "query": {"plane": {"axis": "x", "at": "min"}},
                                        "description": "arm end at the hinge", "source": "user"})
        call_ok(c, "selection_create", {"name": "tip", "body": "arm_part", "query": {"plane": {"axis": "x", "at": "max"}},
                                        "description": "payload seat", "source": "user"})
        call_ok(c, "material_assign", {"body": "arm_part", "material": "pla_generic_demo", "source": "user"})
        call_ok(c, "mesh_generate", {"element_size": "2 mm"})
        call_err(c, "mech_fem_assess", {"job_id": job, "snapshot": "peak_moment:shoulder", "body": "arm",
                                        "attachments": [{"joint": "shoulder", "selection": "hinge"}]},
                 "PRECONDITION_FAILED", "every joint loading the part needs an attachment selection")
        v = call_ok(c, "mech_fem_assess", {"job_id": job, "snapshot": "peak_moment:shoulder", "body": "arm",
                                           "attachments": [{"joint": "shoulder", "selection": "hinge"}, {"joint": "payload_mount", "selection": "tip"}]})
        a = v.get("assessment", {})
        t_snap = a.get("time_s", 0.248)
        errs = [t.get("force_reproduction_error", 1) for t in a.get("joint_loads", [])]
        check(len(errs) == 2 and max(errs) < 1e-9, f"both joint wrenches reproduced on their selections ({errs})")
        mc = a.get("mass_consistency", {})
        check(abs(mc.get("relative_difference", 1)) < 0.02, f"FEM and rigid-body masses agree ({mc})")
        fem_job = v.get("job_id")
        status = wait_job(c, fem_job)
        check(status.get("state") == "succeeded", f"FEM job succeeded ({status.get('state')}: {json.dumps(status.get('error'))[:400]})")
        summ = status.get("summary") or {}
        sr = summ.get("mechanical_assessment", {}).get("support_reactions", {})
        print(f"  isostatic support reactions {sr.get('largest_component_N', -1):.2e} N ({sr.get('relative_to_load_scale', -1):.2e} of the load scale); "
              f"FEM mass {mc.get('fem_mass_kg', 0) * 1e3:.2f} g vs rigid {mc.get('rigid_model_mass_kg', 0) * 1e3:.2f} g")
        check(0 <= sr.get("relative_to_load_scale", 1) < 1e-3, "support reactions negligible: loads self-equilibrated")
        check(summ.get("fidelity", {}).get("level") == 3, "result labelled as fidelity level 3 (quasi-static assessment)")
        rq = call_ok(c, "results_query", {"job_id": fem_job, "quantity": "von_mises"})
        top = (rq.get("largest") or [{}])[0]
        vm = top.get("value_mpa", top.get("value", 0))
        print(f"  largest von Mises {vm} MPa at {top.get('location_mm')}")
        check(0.2 < float(vm) < 20, "stress magnitude plausible for a 0.2-0.3 N m hinge moment on a 16 x 10 mm section")

        print("== vibration of the printed arm: natural modes and the transient elastic response during the lift")
        v = call_ok(c, "mech_modal_run", {"part": "arm_part", "modes": 6, "supports": {"type": "fixed", "selections": ["hinge"]}})
        mjob = v.get("job_id")
        status = wait_job(c, mjob)
        check(status.get("state") == "succeeded", f"clamped modal job succeeded ({status.get('state')}: {json.dumps(status.get('error'))[:300]})")
        ms = call_ok(c, "mech_vibration_query", {"job_id": mjob}).get("summary", {})
        elastic = [m for m in ms.get("modes", []) if m.get("kind") == "elastic"]
        E, rho, L, b, h = 3.0e9, 1240.0, 0.120, 0.016, 0.010
        f_eb = 1.87510407 ** 2 / (2 * math.pi * L * L) * math.sqrt(E * (b * h ** 3 / 12) / (rho * b * h))
        f1 = elastic[0]["frequency_hz"] if elastic else 0
        chkm = ms.get("checks", {})
        print(f"  clamped arm: first bending {f1:.2f} Hz (Euler-Bernoulli {f_eb:.2f} Hz, short beam L/h = 12), modes "
              f"{[round(m['frequency_hz'], 1) for m in elastic]}; {chkm.get('iterations')} iterations, residual {chkm.get('largest_elastic_residual', -1):.1e}")
        check(elastic and abs(f1 / f_eb - 1) < 0.05 and chkm.get("rigid_body_modes_found") == 0 and chkm.get("converged"),
              "clamped arm: first bending frequency within 5% of Euler-Bernoulli (shear and the clamped face lower it slightly)")
        shape = call_ok(c, "mech_vibration_query", {"job_id": mjob, "what": "mode_shape", "mode": 1})
        tipx = (shape.get("largest_nodes") or [{}])[0].get("location_mm", [0])[0]
        check(tipx > 55, f"mode 1 moves most at the free end (largest amplitude at x = {tipx} mm)")
        v = call_ok(c, "mech_modal_run", {"part": "arm_part", "modes": 4, "supports": {"type": "free"}})
        fjob = v.get("job_id")
        wait_job(c, fjob)
        fs = call_ok(c, "mech_vibration_query", {"job_id": fjob}).get("summary", {})
        fel = [m for m in fs.get("modes", []) if m.get("kind") == "elastic"]
        f_ff = 4.73004074 ** 2 / (2 * math.pi * L * L) * math.sqrt(E * (b * h ** 3 / 12) / (rho * b * h))
        print(f"  free-free arm: {fs.get('checks', {}).get('rigid_body_modes_found')} rigid-body modes, first elastic {fel[0]['frequency_hz'] if fel else 0:.1f} Hz "
              f"(Euler-Bernoulli bending {f_ff:.1f} Hz)")
        check(fs.get("checks", {}).get("rigid_body_modes_found") == 6 and fel and abs(fel[0]["frequency_hz"] / f_ff - 1) < 0.08,
              "free-free arm: six rigid-body modes found, first bending near Euler-Bernoulli")
        # a sample lands exactly on the instant of the static assessment (the peak-moment snapshot)
        tbase = {"job_id": job, "body": "arm", "attachments": [{"joint": "shoulder", "selection": "hinge"}, {"joint": "payload_mount", "selection": "tip"}],
                 "time_from": f"{t_snap - 0.05:.17g} s", "time_to": f"{t_snap + 0.05:.17g} s", "output_step": "2 ms", "modes": 12}
        call_err(c, "mech_transient_assess", tbase, "PRECONDITION_FAILED", "a transient assessment without damping and its source is a missing input")
        v = call_ok(c, "mech_transient_assess", dict(tbase, damping={"ratio": 0.02, "source": "assumed"}))
        samp = v.get("sampling", {})
        check(samp.get("pattern_reconstruction_error", 1) < 1e-12 and samp.get("net_force_relative_at_window_end", 1) < 1e-6 and samp.get("samples") == 51,
              f"load patterns reproduce the direct loads and balance the rigid motion ({samp.get('pattern_reconstruction_error')}, {samp.get('net_force_relative_at_window_end')})")
        tjob = v.get("job_id")
        status = wait_job(c, tjob)
        check(status.get("state") == "succeeded", f"transient job succeeded ({status.get('state')}: {json.dumps(status.get('error'))[:300]})")
        ts_ = call_ok(c, "mech_vibration_query", {"job_id": tjob}).get("summary", {})
        resp = ts_.get("response", {})
        daf = resp.get("dynamic_amplification_von_mises", 0)
        share = ts_.get("checks", {}).get("rigid_body_load_share", 1)
        print(f"  lift window {t_snap - 0.05:.4f}-{t_snap + 0.05:.4f} s: peak von Mises dynamic {resp.get('peak_von_mises_dynamic', {}).get('value_mpa', -1):.4f} MPa, quasi-static "
              f"{resp.get('peak_von_mises_quasi_static', {}).get('value_mpa', -1):.4f} MPa, amplification {daf:.4f}; rigid-body load share {share:.1e}; "
              f"lowest free-free mode {(ts_.get('modes', {}).get('elastic_frequencies_hz') or [0])[0]:.0f} Hz")
        check(share < 1e-6, "sampled loads excite no rigid-body mode (self-equilibrated)")
        check(abs(daf - 1) < 0.02, "a lift slow against the part's first mode shows no dynamic amplification")
        hist = call_ok(c, "mech_vibration_query", {"job_id": tjob, "what": "history", "max_points": 1000})
        rows = hist.get("rows", [])
        at = [r_ for r_ in rows if abs(r_[0] - t_snap) < 1e-9]
        gp = call_ok(c, "results_query", {"job_id": fem_job, "quantity": "von_mises", "value_kind": "gauss_point"})
        gtop = (gp.get("largest") or [{}])[0]
        gvm = float(gtop.get("value_mpa", gtop.get("value", 0))) * 1e6
        qs_at = at[0][2] if at else 0
        print(f"  quasi-static peak at t = {t_snap:.6f} s {qs_at / 1e6:.6f} MPa; static assessment of the peak-moment snapshot {gvm / 1e6:.6f} MPa (Gauss points)")
        check(at and gvm > 0 and abs(qs_at / gvm - 1) < 1e-5, "the transient's quasi-static solution equals the static assessment at the same instant")
        call_err(c, "mech_vibration_query", {"job_id": mjob, "what": "history"}, "INVALID_PARAMS", "a modal job has no history")


        print("== flexible arm: Craig-Bampton reduction of the printed part, coupled flexible dynamics of the lift")
        fbase = {"body": "arm", "root_selection": "hinge", "interfaces": [{"joint": "payload_mount", "selection": "tip"}], "fixed_interface_modes": 4}
        call_err(c, "mech_flexible_reduce", fbase, "PRECONDITION_FAILED", "a reduction without damping and its source is a missing input")
        call_err(c, "mech_flexible_reduce", dict(fbase, interfaces=[{"joint": "shoulder", "selection": "tip"}], damping={"ratio": 0.02, "source": "assumed"}),
                 "INVALID_PARAMS", "an interface must be a joint carried by the body")
        v = call_ok(c, "mech_flexible_reduce", dict(fbase, damping={"ratio": 0.02, "source": "assumed"}, max_frequency_hz=FLEX_CUT_HZ))
        xjob = v.get("job_id")
        status = wait_job(c, xjob)
        check(status.get("state") == "succeeded", f"reduction job succeeded ({status.get('state')}: {json.dumps(status.get('error'))[:300]})")
        xs = status.get("summary") or {}
        red = xs.get("reduction", {})
        freqs = [co["frequency_hz"] for co in xs.get("coordinates", [])]
        itf = (xs.get("interfaces") or [{}])[0]
        ct = itf.get("static_compliance_translation_m_per_n", [0, 0, 0])
        cr = itf.get("static_compliance_rotation_rad_per_n_m", [0, 0, 0])
        nu = 0.36
        EI = E * b * h ** 3 / 12
        c_zz = L ** 3 / (3 * EI) + L / (5 / 6 * E / (2 * (1 + nu)) * b * h)
        c_ry = L / EI
        mpx = xs.get("mass_properties", {})
        print(f"  reduced arm: {red.get('elastic_coordinates')} coordinates ({red.get('constraint_modes')} constraint + {red.get('fixed_interface_modes')} fixed-interface, "
              f"{red.get('dropped_above_cutoff')} dropped above {FLEX_CUT_HZ} Hz, interface compliance loss {red.get('largest_interface_compliance_loss', -1):.2e}); "
              f"frequencies {[round(f_, 1) for f_ in freqs]} Hz; stability step {red.get('stability_step_s', 0):.2e} s")
        print(f"  tip compliance z {ct[2]:.4e} m/N (Timoshenko {c_zz:.4e}), rotation y {cr[1]:.4e} rad/N m (L/EI {c_ry:.4e}); mesh mass {mpx.get('mesh_mass_kg', 0) * 1e3:.3f} g "
              f"vs body {mpx.get('body_mass_kg', 0) * 1e3:.3f} g")
        check(red.get("elastic_coordinates", 0) >= 3 and red.get("dropped_above_cutoff", 0) >= 1 and freqs == sorted(freqs), "coordinates kept below the cutoff, ascending")
        loss = itf.get("compliance_loss_translation", [1, 1, 1])
        rloss = itf.get("compliance_loss_rotation", [1, 1, 1])
        print(f"  flexibility lost by the cutoff: translation {[round(x, 4) for x in loss]}, rotation {[round(x, 4) for x in rloss]}; "
              f"before the cutoff: tip compliance z {ct[2] / (1 - loss[2]):.4e} m/N, rotation y {cr[1] / (1 - rloss[1]):.4e} rad/N m")
        check(abs(ct[2] / (1 - loss[2]) / c_zz - 1) < 0.03 and abs(cr[1] / (1 - rloss[1]) / c_ry - 1) < 0.03,
              "tip interface compliance of the reduction within 3% of beam theory (bending and rotation, before the cutoff)")
        check(abs(freqs[0] / f1 - 1) < 0.01, f"first reduced frequency {freqs[0]:.2f} Hz within 1% of the clamped modal run ({f1:.2f} Hz; the tip face is rigid)")
        check(loss[2] < 0.05 and loss[0] > 0.5, "the cutoff keeps the bending flexibility that matters for the lift and reports the axial flexibility it drops")
        shape = call_ok(c, "mech_vibration_query", {"job_id": xjob, "what": "mode_shape", "mode": 1})
        ftipx = (shape.get("largest_nodes") or [{}])[0].get("location_mm", [0])[0]
        check(abs(shape.get("frequency_hz", 0) - freqs[0]) < 1e-9 * freqs[0] and abs(ftipx - tipx) < 1e-9,
              f"the first elastic coordinate of the reduction moves the free end most, like the clamped mode (x = {ftipx} mm in the build frame)")
        v = call_ok(c, "mech_flexible_attach", {"job_id": xjob, "body": "arm"})
        fx = v.get("flexible", {})
        armb = [bb for bb in v.get("assembly", {}).get("bodies", []) if bb["name"] == "arm"][0]
        check(fx.get("elastic_coordinates") == red.get("elastic_coordinates") and armb.get("flexible", {}).get("interface_joints") == ["payload_mount"]
              and abs(armb.get("mass_kg", 0) - mpx.get("mesh_mass_kg", -1)) < 1e-12, "attached: the arm is flexible with the mesh's mass properties")
        v = call_ok(c, "mech_study_validate", {})
        check(v.get("valid") is True and v.get("fidelity", {}).get("level") == 4, f"study with a flexible arm validates at fidelity level 4 ({v.get('fidelity', {}).get('name')})")
        v = call_ok(c, "mech_dynamics_run", {"time_step_check": True, "label": "lift 60 deg, flexible arm"})
        xrun = v.get("job_id")
        status = wait_job(c, xrun)
        check(status.get("state") == "succeeded", f"flexible dynamics job succeeded ({status.get('state')}: {json.dumps(status.get('error'))[:400]})")
        fsum = call_ok(c, "mech_results_query", {"job_id": xrun, "what": "summary"}).get("summary", {})
        fb0 = (fsum.get("flexible_bodies") or [{}])[0]
        fi0 = (fb0.get("interfaces") or [{}])[0]
        fts = fsum.get("time_step_sensitivity", {})
        fctl = (fsum.get("controllers") or [{}])[0]
        print(f"  flexible lift: step limit {fsum.get('flexible_step_limit_s', 0):.2e} s, {fsum.get('steps')} steps; peak tip deflection {fi0.get('peak_deflection_m', 0) * 1e3:.4f} mm "
              f"at {fi0.get('peak_deflection_time_s', 0):.3f} s, peak strain energy {fb0.get('peak_strain_energy_j', 0):.3e} J; final tracking error "
              f"{math.degrees(fctl.get('final_error', 1)):.4f} deg; time-step check {fts.get('largest_relative_change', -1):.2e}")
        check(fsum.get("fidelity", {}).get("level") == 4 and fsum.get("flexible_step_limit_s", 1) < 5e-4, "run labelled fidelity 4; steps limited by the elastic coordinates")
        check(abs(fctl.get("final_error", 1)) < math.radians(0.5), "the flexible arm still settles within 0.5 deg of 60 deg")
        check(0 <= fts.get("largest_relative_change", 1) < 0.05, "time-step check (halving the step actually taken) below 5%, flexible peaks included")
        hist = call_ok(c, "mech_results_query", {"job_id": xrun, "what": "history", "channels": ["arm.payload_mount.dz", "arm.strain_energy", "shoulder.q"],
                                                 "time_from": 1.8, "time_to": 2.0, "max_points": 200}).get("series", {})
        dz = hist.get("arm.payload_mount.dz", [])
        dz_mean = sum(dz) / len(dz) if dz else 0
        q_end = (hist.get("shoulder.q") or [0])[-1]
        g0 = 9.80665
        dz_ref = -math.cos(q_end) * g0 * (0.150 * ct[2] + rho * b * h * L ** 4 / (8 * EI))
        print(f"  held at {math.degrees(q_end):.3f} deg: tip deflection normal to the arm {dz_mean * 1e3:.5f} mm; static estimate {dz_ref * 1e3:.5f} mm "
              f"(payload weight on the reduced compliance plus the arm's own weight, Euler-Bernoulli)")
        check(dz and abs(dz_mean / dz_ref - 1) < 0.05, "the held flexible arm sags as the static estimate (5%)")
        fenv = call_ok(c, "mech_results_query", {"job_id": xrun, "what": "envelope"}).get("envelope", [])
        fsh = [e_ for e_ in fenv if e_["joint"] == "shoulder"][0]
        m_rigid, m_flex = sh["peak_moment"]["magnitude_Nm"], fsh["peak_moment"]["magnitude_Nm"]
        hold = {}
        for jb, nm in ((job, "rigid"), (xrun, "flexible")):
            ser = call_ok(c, "mech_results_query", {"job_id": jb, "what": "history", "channels": ["motor.effort"], "time_from": 1.8, "time_to": 2.0,
                                                    "max_points": 200}).get("series", {}).get("motor.effort", [])
            hold[nm] = sum(ser) / len(ser) if ser else 0
        print(f"  shoulder peak moment flexible {m_flex:.5f} N m vs rigid {m_rigid:.5f} N m ({100 * (m_flex / m_rigid - 1):+.2f}%; the peaks ride on the controller's "
              f"quantisation ripple, which the 30 Hz arm mode filters); mean holding effort {hold['flexible']:.5f} vs {hold['rigid']:.5f} N m")
        check(abs(m_flex / m_rigid - 1) < 0.10 and abs(hold["flexible"] / hold["rigid"] - 1) < 0.02,
              "a stiff arm changes the peak hinge moment by less than 10% and the holding effort by less than 2%")
        call_err(c, "mech_flexible_stress", {"job_id": xrun, "body": "payload"}, "PRECONDITION_FAILED", "stresses of a rigid body from elastic coordinates are refused")
        v = call_ok(c, "mech_flexible_stress", {"job_id": xrun, "body": "arm"})
        sjob = v.get("job_id")
        status = wait_job(c, sjob)
        check(status.get("state") == "succeeded", f"flexible stress job succeeded ({status.get('state')}: {json.dumps(status.get('error'))[:300]})")
        ss = call_ok(c, "mech_vibration_query", {"job_id": sjob}).get("summary", {})
        pk = ss.get("response", {}).get("peak_von_mises", {})
        sh_ = call_ok(c, "mech_vibration_query", {"job_id": sjob, "what": "history", "max_points": 5000})
        srows = sh_.get("rows", [])
        s0 = srows[0][1] if srows else 0
        hold_rows = [r_[1] for r_ in srows if r_[0] >= 1.8]
        s_hold = sum(hold_rows) / len(hold_rows) if hold_rows else 0
        M0 = g0 * (0.150 * L + mpx.get("mesh_mass_kg", 0) * L / 2)
        s_beam = M0 * (h / 2) / (b * h ** 3 / 12)
        print(f"  coupled stresses of the arm: peak von Mises {pk.get('value_mpa', 0):.4f} MPa at {pk.get('time_s', 0):.3f} s, {pk.get('location_mm')}; "
              f"at t = 0 (static equilibrium, horizontal) {s0 / 1e6:.4f} MPa (beam bending at the root {s_beam / 1e6:.4f} MPa); held at 60 deg {s_hold / 1e6:.4f} MPa "
              f"(ratio {s_hold / s0 if s0 else 0:.4f}, cos {math.cos(q_end):.4f})")
        hinge_x = tipx - 120  # the free end is at tipx in the build frame and the arm is 120 mm long
        check(pk.get("value_mpa", 0) >= s0 / 1e6 and 0.85 < s0 / s_beam < 1.1 and abs(pk.get("location_mm", [1e9])[0] - hinge_x) < 4,
              "the largest coupled stress is next to the clamped hinge; at t = 0 it is the root bending stress at the Gauss points (inside the outer fibre)")
        check(s0 > 0 and abs(s_hold / s0 / math.cos(q_end) - 1) < 0.03, "held at 60 deg the stress scales with the gravity component normal to the arm")
        v = call_ok(c, "mech_flexible_detach", {"body": "arm"})
        armb = [bb for bb in v.get("assembly", {}).get("bodies", []) if bb["name"] == "arm"][0]
        check("flexible" not in armb, "detached: the arm is rigid again")
        call_ok(c, "mech_joint_define", {"joint": {"name": "payload_mount", "type": "fixed", "parent": "arm", "child": "payload",
                                                   "parent_frame": {"position": ["110 mm", "0 mm", "0 mm"]}, "child_frame": {}}})
        call_err(c, "mech_flexible_attach", {"job_id": xjob}, "PRECONDITION_FAILED", "a reduction whose interface joint has moved is refused as stale")
        call_ok(c, "mech_joint_define", {"joint": {"name": "payload_mount", "type": "fixed", "parent": "arm", "child": "payload",
                                                   "parent_frame": {"position": ["120 mm", "0 mm", "0 mm"]}, "child_frame": {}}})

        print("== URDF import asks for the root connection")
        urdf = tmp / "one.urdf"
        urdf.write_text('<robot name="one"><link name="a"><inertial><mass value="1"/><inertia ixx="1" ixy="0" ixz="0" iyy="1" iyz="0" izz="1"/>'
                        '</inertial></link></robot>')
        err = call_err(c, "mech_assembly_import", {"file": str(urdf)}, "PRECONDITION_FAILED", "URDF without root_joint")
        check("root" in json.dumps(err).lower(), "the missing input names the root connection")

        gripper_flow(c, tmp)
    finally:
        code, stderr = c.close()
        if FAIL:
            print(stderr[-2000:])
        shutil.rmtree(tmp, ignore_errors=True)
    if FAIL:
        print(f"\nMECH FLOW TESTS FAILED: {PASS} passed, {FAIL} failed")
        return 1
    print(f"\nALL MECH FLOW TESTS PASSED: {PASS} passed, 0 failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
