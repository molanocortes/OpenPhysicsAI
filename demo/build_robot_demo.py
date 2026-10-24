#!/usr/bin/env python3
"""build_robot_demo.py - the complete mechanics + additive-manufacturing demonstration, driven over MCP.

A printed robot upper-arm link (yoke with two bearing bores, I-section web with lightening holes, elbow bearing boss and
a NEMA-17 motor flange) carries an elbow motor, a forearm and a 1 kg payload. Two geared DC motors with encoders and
sampled PID controllers lift and fold the arm. The script then:

  1. runs the rigid dynamics and reads the joint load envelope
  2. assesses the printed link with the finite-element core at the peak shoulder moment, with the loads entering through
     the shoulder bores, the elbow bore and the four motor bolt holes
  3. repeats the assessment with an orthotropic printed material for two build directions and a strength criterion
  4. computes the natural modes of the link held at its shoulder bores
  5. reduces the link to a flexible body with two interfaces (elbow bore, motor flange), runs the same lift with the
     flexible link coupled to the motion, and recovers stresses from the elastic coordinates
  6. compares two designs of the link (solid web against lightened I-section) with the comparison-study feature:
     equal mounting and load, two mesh levels, sensitivities and an evidence record

Nothing here is measured: every material value is demonstration data and every contact or damping parameter is declared
as assumed. Run from the repository root after `make am` and the geometry build:

    clang -O2 demo/make_link_geometry.c -o build/linkgen -lm
    ./build/linkgen a demo/geometry/link_a_solid.stl 0.6
    ./build/linkgen b demo/geometry/link_b_lightened.stl 0.6
    python3 demo/build_robot_demo.py [workspace_dir]
"""
import json
import math
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402

LINK = 170.0  # shoulder axis to elbow axis, mm, in the part's file frame
FLANGE_Y = 20.0


def main():
    ws = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "demo" / "workspace"
    ws.mkdir(parents=True, exist_ok=True)
    if (ws / "robot_arm").exists():
        print(f"{ws / 'robot_arm'} already exists: remove it to rebuild")
        return 1
    c = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(ROOT), "--allow-write", str(ws)])
    c.initialize()
    t0 = time.time()

    def op(name, args, what=None):
        r = c.call(name, args).get("result", {}).get("structuredContent") or {}
        if r.get("ok") is not True:
            raise SystemExit(f"{what or name} failed: {json.dumps(r.get('error'))[:900]}")
        return r.get("value", {})

    def wait(job, what=""):
        for _ in range(400):
            st = op("job_status", {"job_id": job, "wait_seconds": 10})
            if st.get("state") not in ("queued", "running"):
                if st.get("state") != "succeeded":
                    raise SystemExit(f"{what} job {job} {st.get('state')}: {json.dumps(st.get('error'))[:700]}")
                return st
        raise SystemExit(f"job {job} did not finish")

    try:
        print("== printed link: import, regions, material, mesh")
        op("project_create", {"name": "robot_arm", "description": "printed robot arm: two motors lift a 1 kg payload (demonstration data)"})
        v = op("geometry_import", {"path": str(ROOT / "demo/geometry/link_b_lightened.stl"), "units": "mm", "name": "upper_arm_part"})
        body = v["body"]
        t = body["placement"]["translation_mm"]  # x_build = x_file + t (the importer centres the part on the plate)
        print(f"  {body['triangles']} triangles, {body['volume_mm3'] / 1e3:.1f} cm^3, "
              f"{'closed solid' if body['closed_solid'] else 'NOT CLOSED'}, {body['surface_patches']} surface patches")

        def B(x, y, z):  # part frame (mm) -> build frame (mm)
            return [x + t[0], y + t[1], z + t[2]]

        def bore(x0, z0, r, y0, y1, pad=1.0):
            """the wall of a bore along y: a tight box around it, without the faces that look along the bore axis"""
            return {"all": [{"box": {"min": B(x0 - r - pad, y0, z0 - r - pad), "max": B(x0 + r + pad, y1, z0 + r + pad)}},
                            {"not": {"facing": {"direction": [0, 1, 0], "max_angle_deg": 50}}},
                            {"not": {"facing": {"direction": [0, -1, 0], "max_angle_deg": 50}}}]}

        sels = {
            "shoulder_bores": (bore(0, 0, 10, -22, 22), "the two 20 mm bearing bores of the shoulder yoke"),
            "elbow_bore": (bore(LINK, 0, 8, -15, 15), "the 16 mm elbow bearing bore"),
            "motor_bolts": ({"any": [bore(LINK + sx * 15.5, sz * 15.5, 2.1, FLANGE_Y - 4, FLANGE_Y + 4, pad=0.6) for sx in (-1, 1) for sz in (-1, 1)]},
                            "the four M4 bolt holes of the NEMA-17 motor flange"),
        }
        for name, (q, what) in sels.items():
            r = op("selection_create", {"name": name, "body": "upper_arm_part", "query": q, "mode": "triangle",
                                        "description": what, "source": "user", "replace": True})
            sel = r.get("selection", r)
            print(f"  selection {name}: {sel.get('triangles')} triangles, {sel.get('area_mm2', 0):.0f} mm^2 at {[round(x, 1) for x in sel.get('centroid_mm', [])]}")
        op("material_assign", {"body": "upper_arm_part", "material": "pla_generic_demo", "source": "user"})
        v = op("mesh_generate", {"element_size": "2 mm"})
        mesh = v.get("mesh", v)
        print(f"  hex mesh: {mesh.get('elements')} elements, {mesh.get('nodes')} nodes at {mesh.get('element_size_mm', 2.5)} mm")

        print("== robot: bodies, joints, motors, encoders, controllers")
        op("mech_assembly_import", {"name": "printed_arm", "parts": [{"body": "upper_arm_part", "name": "upper_arm",
                                                                      "fill": {"model": "solid", "density": "1240 kg/m^3", "source": "user"}}]})
        op("mech_body_define", {"name": "base", "mass_properties": {"mass": "2.4 kg", "com": ["0 mm", "0 mm", "-40 mm"],
                                                                     "inertia": {"ixx": "4000 kg*mm^2", "iyy": "4000 kg*mm^2", "izz": "3000 kg*mm^2"},
                                                                     "provenance": "user"}})
        op("mech_body_define", {"name": "elbow_motor", "mass_properties": {"mass": "280 g", "com": ["0 mm", "17 mm", "0 mm"],
                                                                            "inertia": {"ixx": "70 kg*mm^2", "iyy": "50 kg*mm^2", "izz": "70 kg*mm^2",
                                                                                        "ixy": 0, "ixz": 0, "iyz": 0},
                                                                            "provenance": "user"}})
        op("mech_body_define", {"name": "forearm", "mass_properties": {"mass": "420 g", "com": ["70 mm", "0 mm", "0 mm"],
                                                                        "inertia": {"ixx": "300 kg*mm^2", "iyy": "2600 kg*mm^2", "izz": "2600 kg*mm^2",
                                                                                    "ixy": 0, "ixz": 0, "iyz": 0},
                                                                        "provenance": "user"}})
        op("mech_body_define", {"name": "payload", "mass_properties": {"mass": "1 kg", "com": ["0 mm", "0 mm", "0 mm"],
                                                                        "inertia": {"ixx": "900 kg*mm^2", "iyy": "900 kg*mm^2", "izz": "900 kg*mm^2",
                                                                                    "ixy": 0, "ixz": 0, "iyz": 0},
                                                                        "provenance": "user"}})
        op("mech_joint_define", {"joint": {"name": "column", "type": "fixed", "parent": "world", "child": "base", "parent_frame": {}, "child_frame": {}}})
        op("mech_joint_define", {"joint": {"name": "shoulder", "type": "revolute", "parent": "base", "child": "upper_arm",
                                           "parent_frame": {"position": ["0 mm", "0 mm", "0 mm"]}, "child_frame": {"position": ["0 mm", "0 mm", "0 mm"]},
                                           "axis": [0, -1, 0], "motion": "actuated",
                                           "limits": {"lower": "-20 deg", "upper": "110 deg", "restitution": 0},
                                           "friction": {"coulomb": "0.01 N*m", "regularization_velocity": "0.5 deg/s"}}})
        op("mech_joint_define", {"joint": {"name": "motor_mount", "type": "fixed", "parent": "upper_arm", "child": "elbow_motor",
                                           "parent_frame": {"position": [f"{LINK} mm", f"{FLANGE_Y} mm", "0 mm"]}, "child_frame": {}}})
        op("mech_joint_define", {"joint": {"name": "elbow", "type": "revolute", "parent": "upper_arm", "child": "forearm",
                                           "parent_frame": {"position": [f"{LINK} mm", "0 mm", "0 mm"]}, "child_frame": {"position": ["0 mm", "0 mm", "0 mm"]},
                                           "axis": [0, -1, 0], "motion": "actuated",
                                           "limits": {"lower": "-130 deg", "upper": "10 deg", "restitution": 0},
                                           "friction": {"coulomb": "0.008 N*m", "regularization_velocity": "0.5 deg/s"}}})
        op("mech_joint_define", {"joint": {"name": "wrist", "type": "fixed", "parent": "forearm", "child": "payload",
                                           "parent_frame": {"position": ["150 mm", "0 mm", "0 mm"]}, "child_frame": {}}})
        for name, joint, ratio, kp, ki, kd, to, dur in (("shoulder_drive", "shoulder", 150, 38, 30, 0.45, "75 deg", "1.2 s"),
                                                        ("elbow_drive", "elbow", 120, 26, 22, 0.30, "-100 deg", "1.2 s")):
            op("mech_component_define", {"kind": "actuator", "definition": {
                "name": name, "type": "dc_motor", "joint": joint, "gear_ratio": ratio, "efficiency": 0.65, "rotor_inertia": "2.5 g*cm^2",
                "resistance": "3.2 ohm", "torque_constant": "18 mN*m/A", "back_emf_constant": "0.018 V*s/rad", "voltage_limit": "24 V",
                "current_limit": "2.5 A", "motor_viscous": "2e-7 N*m*s/rad", "motor_coulomb": "0.8 mN*m", "motor_coulomb_vreg": "10 rad/s",
                "rated_effort": "4 N*m", "source": "demonstration values, not a datasheet"}})
            op("mech_component_define", {"kind": "sensor", "definition": {
                "name": f"{joint}_encoder", "type": "joint_position", "joint": joint, "period": "1 ms", "latency_samples": 1,
                "resolution": "0.018 deg", "noise_std": "0.002 deg", "seed": 7}})
            op("mech_component_define", {"kind": "controller", "definition": {
                "name": f"{joint}_pid", "type": "pid", "actuator": name, "feedback": f"{joint}_encoder", "loop": "position", "period": "1 ms",
                "delay_samples": 1, "kp": kp, "ki": ki, "kd": kd, "derivative_filter": "10 ms", "output_limits": ["-24 V", "24 V"],
                "antiwindup": "clamp", "reference": {"type": "min_jerk", "start": "0.1 s", "from": "0 deg", "to": to, "duration": dur}}})
        op("mech_study_settings", {"settings": {"end_time": "2 s", "max_step": "0.5 ms", "record_period": "2 ms"}, "snapshots": ["0.5 s", "1.9 s"]})
        v = op("mech_study_validate", {})
        print(f"  study valid: {v.get('valid')}, fidelity level {v.get('fidelity', {}).get('level')}")

        print("== rigid dynamics of the lift")
        run = op("mech_dynamics_run", {"time_step_check": True, "label": "lift and fold, rigid link"})["job_id"]
        wait(run, "rigid dynamics")
        s = op("mech_results_query", {"job_id": run, "what": "summary"}).get("summary", {})
        env = op("mech_results_query", {"job_id": run, "what": "envelope"}).get("envelope", [])
        peaks = {e["joint"]: e for e in env}
        for j in ("shoulder", "elbow", "motor_mount"):
            e = peaks[j]
            print(f"  {j}: peak moment {e['peak_moment']['magnitude_Nm']:.3f} N m at {e['peak_moment']['time_s']:.3f} s, "
                  f"peak force {e['peak_force']['magnitude_N']:.2f} N")
        acts = {a["name"]: a for a in s.get("actuators", [])}
        for a in acts.values():
            print(f"  {a['name']}: peak effort {a.get('peak_effort', 0):.3f} N m, peak current {a.get('peak_current_A', 0):.2f} A")
        ts = s.get("time_step_sensitivity", {})
        print(f"  energy balance residual {s.get('energy_J', {}).get('balance_residual', 0):.2e} J; "
              f"time-step check {ts.get('largest_relative_change', -1):.2e}")
        for item in sorted(ts.get("changes", []), key=lambda x: -x["relative_change"])[:3]:
            print(f"    {item['quantity']}: {item['value']:.5g} -> {item['value_half_step']:.5g} ({item['relative_change']:.1%})")

        print("== finite-element assessment of the printed link: peak shoulder moment, and the steady hold")
        att = [{"joint": "shoulder", "selection": "shoulder_bores"}, {"joint": "elbow", "selection": "elbow_bore"},
               {"joint": "motor_mount", "selection": "motor_bolts"}]
        fem = {}
        for label, snap in (("peak shoulder moment", "peak_moment:shoulder"), ("steady hold at 1.9 s", "t=1.9")):
            v = op("mech_fem_assess", {"job_id": run, "snapshot": snap, "body": "upper_arm", "attachments": att})
            a = v.get("assessment", {})
            job = v["job_id"]
            errs = [t.get("force_reproduction_error", 1) for t in a.get("joint_loads", [])]
            st = wait(job, "fem assessment")
            ma = (st.get("summary") or {}).get("mechanical_assessment", {})
            q = op("results_query", {"job_id": job, "quantity": "von_mises"})
            top = (q.get("largest") or [{}])[0]
            fem[label] = job
            print(f"  {label} (t = {a.get('time_s', 0):.3f} s): 3 joint wrenches placed on their regions (largest reproduction error {max(errs):.0e}), "
                  f"mesh mass {a.get('mass_consistency', {}).get('relative_difference', 0):+.2%} of the rigid model")
            print(f"    peak von Mises {float(top.get('value_mpa', top.get('value', 0))):.3f} MPa at {top.get('location_mm')}; "
                  f"support reactions {ma.get('support_reactions', {}).get('relative_to_load_scale', -1):.1e} of the load scale")
        print("== the same load with an orthotropic printed material, two build directions")
        op("mech_material_define", {"material": {
            "id": "pla_printed_demo", "name": "PLA, printed, demonstration values",
            "elastic": {"E1": "3.2 GPa", "E2": "3.0 GPa", "E3": "2.4 GPa", "nu12": 0.35, "nu13": 0.30, "nu23": 0.32,
                        "G12": "1.15 GPa", "G23": "0.85 GPa", "G13": "0.9 GPa", "source": "assumed",
                        "reference": "illustrative values for this demonstration, not measured"},
            "density": "1240 kg/m^3",
            "strength": {"Xt": "45 MPa", "Xc": "60 MPa", "Yt": "42 MPa", "Yc": "58 MPa", "Zt": "22 MPa", "Zc": "55 MPa",
                         "S12": "30 MPa", "S23": "18 MPa", "S31": "20 MPa", "source": "assumed",
                         "reference": "illustrative interlayer-weak values for this demonstration, not measured"}}})
        ortho = {}
        for name, build_dir in (("upright (layers across the link)", [1, 0, 0]), ("flat (layers across the load)", [0, 0, 1])):
            v = op("mech_fem_assess", {"job_id": run, "snapshot": "t=1.9", "body": "upper_arm", "attachments": att,
                                       "material_model": {"type": "orthotropic", "material": "pla_printed_demo", "build_direction": build_dir,
                                                          "raster_reference": [0, 1, 0] if build_dir[0] else [1, 0, 0], "raster_angle": "45 deg",
                                                          "criterion": "tsai_wu"}})
            j = v["job_id"]
            wait(j, f"orthotropic {name}")
            f = op("mech_structure_query", {"job_id": j, "quantity": "failure_index", "largest": 3})
            worst = (f.get("largest") or [{}])[0]
            ortho[name] = (j, worst)
            print(f"  {name}: failure index {worst.get('failure_index', 0):.4f} (strength ratio {worst.get('strength_ratio', 0):.1f}), "
                  f"mode {worst.get('governing_mode')} at {worst.get('location_mm')}")

        print("== natural modes of the link held at its shoulder bores")
        mj = op("mech_modal_run", {"part": "upper_arm_part", "modes": 6, "supports": {"type": "fixed", "selections": ["shoulder_bores"]}})["job_id"]
        wait(mj, "modal")
        ms = op("mech_vibration_query", {"job_id": mj}).get("summary", {})
        freqs = [m["frequency_hz"] for m in ms.get("modes", []) if m.get("kind") == "elastic"]
        print(f"  first modes {[round(f, 1) for f in freqs[:5]]} Hz, residual {ms.get('checks', {}).get('largest_elastic_residual', -1):.1e}")

        print("== flexible link: reduction with two interfaces, coupled run, stresses")
        red = op("mech_flexible_reduce", {"body": "upper_arm", "root_selection": "shoulder_bores",
                                          "interfaces": [{"joint": "elbow", "selection": "elbow_bore"},
                                                         {"joint": "motor_mount", "selection": "motor_bolts"}],
                                          "fixed_interface_modes": 6, "max_frequency_hz": 1500,
                                          "damping": {"ratio": 0.02, "source": "assumed"}})["job_id"]
        rs = wait(red, "reduction").get("summary", {})
        rr = rs.get("reduction", {})
        print(f"  {rr.get('elastic_coordinates')} coordinates kept of {rr.get('constraint_modes')} constraint + {rr.get('fixed_interface_modes')} fixed modes "
              f"({rr.get('dropped_above_cutoff')} dropped above 1500 Hz), stability step {rr.get('stability_step_s', 0):.2e} s")
        for i in rs.get("interfaces", []):
            ct = i.get("static_compliance_translation_m_per_n", [0, 0, 0])
            print(f"  interface at {i.get('joint')}: static compliance {ct[2]:.3e} m/N along z ({1e-3 / ct[2]:.1f} N/mm)")
        op("mech_flexible_attach", {"job_id": red, "body": "upper_arm"})
        v = op("mech_study_validate", {})
        print(f"  study now at fidelity level {v.get('fidelity', {}).get('level')}: {v.get('fidelity', {}).get('name')}")
        frun = op("mech_dynamics_run", {"time_step_check": False, "label": "lift and fold, flexible link"})["job_id"]
        wait(frun, "flexible dynamics")
        fs = op("mech_results_query", {"job_id": frun, "what": "summary"}).get("summary", {})
        fb = (fs.get("flexible_bodies") or [{}])[0]
        print(f"  {fs.get('steps')} steps at the {fs.get('flexible_step_limit_s', 0):.2e} s limit; peak strain energy {fb.get('peak_strain_energy_j', 0):.3e} J")
        for i in fb.get("interfaces", []):
            print(f"  interface {i.get('joint')}: peak deflection {i.get('peak_deflection_m', 0) * 1e3:.3f} mm at {i.get('peak_deflection_time_s', 0):.3f} s, "
                  f"peak rotation {math.degrees(i.get('peak_rotation_rad', 0)):.3f} deg")
        fenv = {e["joint"]: e for e in op("mech_results_query", {"job_id": frun, "what": "envelope"}).get("envelope", [])}
        print(f"  shoulder peak moment flexible {fenv['shoulder']['peak_moment']['magnitude_Nm']:.3f} N m "
              f"vs rigid {peaks['shoulder']['peak_moment']['magnitude_Nm']:.3f} N m")
        sj = op("mech_flexible_stress", {"job_id": frun, "body": "upper_arm"})["job_id"]
        ss = wait(sj, "coupled stress").get("summary", {})
        pk = ss.get("response", {}).get("peak_von_mises", {})
        print(f"  coupled peak von Mises {pk.get('value_mpa', 0):.3f} MPa at {pk.get('time_s', 0):.3f} s, {pk.get('location_mm')}")

        print("== static case for the app view: shoulder bores held, elbow load")
        op("boundary_apply", {"name": "shoulder_support", "kind": "fixed", "selection": "shoulder_bores",
                              "description": "both bearing bores held (demonstration idealisation)", "source": "user"})
        op("boundary_apply", {"name": "elbow_load", "kind": "force", "selection": "elbow_bore",
                              "force": ["0 N", "0 N", f"{-9.80665 * (0.42 + 1.0):.4f} N"],
                              "description": "weight of forearm and payload carried at the elbow bore", "source": "user"})
        op("project_save", {})
        jobs = {"rigid_run": run, "fem_assessment": fem, "orthotropic": {k: v[0] for k, v in ortho.items()},
                "modal": mj, "reduction": red, "flexible_run": frun, "flexible_stress": sj}
        (ws / "robot_arm" / "DEMO_JOBS.json").write_text(json.dumps(jobs, indent=2))
        print(f"done in {time.time() - t0:.1f} s: {ws / 'robot_arm'}")
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
