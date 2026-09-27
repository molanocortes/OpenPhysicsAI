#!/usr/bin/env python3
"""build_arm_demo.py - builds the printed-arm mechanics demonstration project through MCP, for viewing in the NAVIER app.

A printed PLA arm (STL) on a geared DC motor with an encoder and a PID lifts a 150 g payload by 60 degrees. The project gets:
rigid dynamics run, FEM load assessment at the peak hinge moment, the arm reduced to a flexible body (Craig-Bampton),
a coupled flexible run, stresses from the coupled elastic coordinates, and a static structural case (hinge held, payload
weight on the tip) that the app draws as a stress field.

All material values are demonstration data; nothing here is measured. Run from the repository root after `make am`:
    python3 demo/build_arm_demo.py [workspace_dir]          (default: demo/workspace)
then open it:
    ./navier --workspace demo/workspace --exec "workspace solid; solid open arm_demo"
"""
import json
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402


def box_stl(path, lo, hi):
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    v = [(x0, y0, z0), (x1, y0, z0), (x0, y1, z0), (x1, y1, z0), (x0, y0, z1), (x1, y0, z1), (x0, y1, z1), (x1, y1, z1)]
    faces = [(0, 2, 3), (0, 3, 1), (4, 5, 7), (4, 7, 6), (0, 1, 5), (0, 5, 4), (2, 6, 7), (2, 7, 3), (0, 4, 6), (0, 6, 2), (1, 3, 7), (1, 7, 5)]
    with open(path, "wb") as f:
        f.write(b"arm demo box".ljust(80, b" "))
        f.write(struct.pack("<I", len(faces)))
        for a, b, c in faces:
            f.write(struct.pack("<3f", 0, 0, 0))
            for i in (a, b, c):
                f.write(struct.pack("<3f", *v[i]))
            f.write(b"\0\0")


def main():
    ws = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "demo" / "workspace"
    ws.mkdir(parents=True, exist_ok=True)
    if (ws / "arm_demo").exists():
        print(f"{ws / 'arm_demo'} already exists: remove it to rebuild")
        return 1
    stl = ws / "arm.stl"
    box_stl(stl, (0, -8, -5), (120, 8, 5))
    c = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(ws)])
    c.initialize()

    def op(name, args, what=None):
        r = c.call(name, args).get("result", {}).get("structuredContent") or {}
        if r.get("ok") is not True:
            raise SystemExit(f"{what or name} failed: {json.dumps(r.get('error'))[:800]}")
        return r.get("value", {})

    def wait(job):
        for _ in range(200):
            st = op("job_status", {"job_id": job, "wait_seconds": 10})
            if st.get("state") not in ("queued", "running"):
                if st.get("state") != "succeeded":
                    raise SystemExit(f"job {job} {st.get('state')}: {json.dumps(st.get('error'))[:600]}")
                return st
        raise SystemExit(f"job {job} did not finish")

    try:
        print("project, part, assembly")
        op("project_create", {"name": "arm_demo", "description": "printed arm lifted by a geared motor (mechanics demonstration; demonstration data)"})
        op("geometry_import", {"path": str(stl), "units": "mm", "name": "arm_part"})
        op("mech_assembly_import", {"name": "arm_rig", "parts": [{"body": "arm_part", "name": "arm",
                                                                   "fill": {"model": "solid", "density": "1240 kg/m^3", "source": "user"}}]})
        op("mech_body_define", {"name": "base", "mass_properties": {"mass": "0.8 kg", "com": ["0 mm", "0 mm", "-20 mm"],
                                                                     "inertia": {"ixx": "400 kg*mm^2", "iyy": "400 kg*mm^2", "izz": "300 kg*mm^2"},
                                                                     "provenance": "user"}})
        op("mech_body_define", {"name": "payload", "mass_properties": {"mass": "150 g", "com": ["0 mm", "0 mm", "0 mm"],
                                                                        "inertia": {"ixx": "20 kg*mm^2", "iyy": "20 kg*mm^2", "izz": "20 kg*mm^2",
                                                                                    "ixy": 0, "ixz": 0, "iyz": 0}, "provenance": "user"}})
        op("mech_joint_define", {"joint": {"name": "mount", "type": "fixed", "parent": "world", "child": "base", "parent_frame": {}, "child_frame": {}}})
        op("mech_joint_define", {"joint": {"name": "shoulder", "type": "revolute", "parent": "base", "child": "arm",
                                           "parent_frame": {"position": ["0 mm", "0 mm", "0 mm"]}, "child_frame": {"position": ["0 mm", "0 mm", "0 mm"]},
                                           "axis": [0, -1, 0], "motion": "actuated", "limits": {"lower": "-30 deg", "upper": "120 deg", "restitution": 0},
                                           "friction": {"coulomb": "0.002 N*m", "regularization_velocity": "0.5 deg/s"}}})
        op("mech_joint_define", {"joint": {"name": "payload_mount", "type": "fixed", "parent": "arm", "child": "payload",
                                           "parent_frame": {"position": ["120 mm", "0 mm", "0 mm"]}, "child_frame": {}}})
        print("motor, encoder, PID, settings")
        op("mech_component_define", {"kind": "actuator", "definition": {
            "name": "motor", "type": "dc_motor", "joint": "shoulder", "gear_ratio": 150, "efficiency": 0.6, "rotor_inertia": "1.5 g*cm^2",
            "resistance": "4 ohm", "torque_constant": "10 mN*m/A", "back_emf_constant": "0.01 V*s/rad", "voltage_limit": "12 V", "current_limit": "1.5 A",
            "motor_viscous": "1e-7 N*m*s/rad", "motor_coulomb": "0.5 mN*m", "motor_coulomb_vreg": "10 rad/s", "rated_effort": "1 N*m",
            "source": "demonstration values, not a datasheet"}})
        op("mech_component_define", {"kind": "sensor", "definition": {
            "name": "encoder", "type": "joint_position", "joint": "shoulder", "period": "1 ms", "latency_samples": 1, "resolution": "0.09 deg",
            "noise_std": "0.01 deg", "seed": 11}})
        op("mech_component_define", {"kind": "controller", "definition": {
            "name": "pid", "type": "pid", "actuator": "motor", "feedback": "encoder", "loop": "position", "period": "1 ms", "delay_samples": 1,
            "kp": 40, "ki": 80, "kd": 0.5, "derivative_filter": "3 ms", "output_limits": ["-12 V", "12 V"], "antiwindup": "clamp",
            "reference": {"type": "min_jerk", "start": "0.1 s", "from": "0 deg", "to": "60 deg", "duration": "0.6 s"}}})
        op("mech_study_settings", {"settings": {"end_time": "2 s", "max_step": "0.5 ms", "record_period": "2 ms"}, "snapshots": ["0.4 s"]})
        print("rigid dynamics run")
        run = op("mech_dynamics_run", {"time_step_check": False, "label": "lift 60 deg, rigid arm"})["job_id"]
        wait(run)
        env = op("mech_results_query", {"job_id": run, "what": "envelope"}).get("envelope", [])
        sh = [e for e in env if e["joint"] == "shoulder"][0]
        print(f"  shoulder peak moment {sh['peak_moment']['magnitude_Nm']:.4f} N m at {sh['peak_moment']['time_s']:.3f} s")
        print("selections, material, mesh, FEM assessment at the peak hinge moment")
        op("selection_create", {"name": "hinge", "body": "arm_part", "query": {"plane": {"axis": "x", "at": "min"}}, "description": "arm end at the hinge", "source": "user"})
        op("selection_create", {"name": "tip", "body": "arm_part", "query": {"plane": {"axis": "x", "at": "max"}}, "description": "payload seat", "source": "user"})
        op("material_assign", {"body": "arm_part", "material": "pla_generic_demo", "source": "user"})
        op("mesh_generate", {"element_size": "2 mm"})
        fem = op("mech_fem_assess", {"job_id": run, "snapshot": "peak_moment:shoulder", "body": "arm",
                                     "attachments": [{"joint": "shoulder", "selection": "hinge"}, {"joint": "payload_mount", "selection": "tip"}]})["job_id"]
        wait(fem)
        print("flexible arm: reduction, attach, coupled run, coupled stresses")
        red = op("mech_flexible_reduce", {"body": "arm", "root_selection": "hinge", "interfaces": [{"joint": "payload_mount", "selection": "tip"}],
                                          "fixed_interface_modes": 4, "max_frequency_hz": 2000, "damping": {"ratio": 0.02, "source": "assumed"}})["job_id"]
        rs = wait(red).get("summary", {})
        print(f"  {rs.get('reduction', {}).get('elastic_coordinates')} elastic coordinates, "
              f"{[round(x['frequency_hz'], 1) for x in rs.get('coordinates', [])]} Hz")
        op("mech_flexible_attach", {"job_id": red, "body": "arm"})
        frun = op("mech_dynamics_run", {"time_step_check": False, "label": "lift 60 deg, flexible arm"})["job_id"]
        wait(frun)
        fs = op("mech_results_query", {"job_id": frun, "what": "summary"}).get("summary", {})
        fi = ((fs.get("flexible_bodies") or [{}])[0].get("interfaces") or [{}])[0]
        print(f"  peak tip deflection {fi.get('peak_deflection_m', 0) * 1e3:.3f} mm at {fi.get('peak_deflection_time_s', 0):.3f} s")
        stj = op("mech_flexible_stress", {"job_id": frun, "body": "arm"})["job_id"]
        ss = wait(stj).get("summary", {})
        pk = ss.get("response", {}).get("peak_von_mises", {})
        print(f"  coupled peak von Mises {pk.get('value_mpa', 0):.3f} MPa at {pk.get('time_s', 0):.3f} s")
        print("static structural case for the app: hinge held, payload weight on the tip")
        op("boundary_apply", {"name": "hinge_support", "kind": "fixed", "selection": "hinge", "description": "hinge held (demonstration)", "source": "user"})
        op("boundary_apply", {"name": "payload_weight", "kind": "force", "selection": "tip", "force": ["0 N", "0 N", "-1.471 N"],
                              "description": "150 g payload weight, arm horizontal (demonstration)", "source": "user"})
        op("project_save", {})
        (ws / "arm_demo" / "DEMO_JOBS.json").write_text(json.dumps(
            {"rigid_run": run, "fem_assessment": fem, "reduction": red, "flexible_run": frun, "flexible_stress": stj}, indent=2))
        print(f"done: {ws / 'arm_demo'}")
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
