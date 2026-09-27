#!/usr/bin/env python3
"""transientflow.py - adaptive transient thermal runs driven over MCP stdio, as an AI host drives them.

Stage A (adaptive integration) through the real application path:
  * capabilities advertise adaptive stepping, schedules and output times, and each is actually reachable;
  * a heater switched on and off by a schedule, with explicit output times, run adaptively: stored times are exact,
    the deposited energy is exact, and the summary separates execution, convergence, conservation, discretisation
    evidence, material provenance and validation;
  * schema-to-solver wiring: every adaptive setting that is accepted changes what the solver does (tolerance ->
    step count, error measure -> summary and spec hash, min_time_step -> a MIN_STEP failure with structured details);
  * invalid combinations are rejected (adaptive keys with fixed steps, malformed schedules, schedules on mechanical
    conditions) and schedules persist across save and reopen.

Run from the repository root after `make am`:
    python3 tools/transientflow.py
"""
import json
import struct
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcptest import Client  # noqa: E402

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
    check(ok, f"{what or name}: {json.dumps(sc.get('error'))[:500] if not ok else ''}")
    return sc.get("value", {}), r


def call_err(c, name, args):
    r = c.call(name, args)
    sc = (r.get("result", {}) or {}).get("structuredContent") or {}
    return sc.get("error") or {}


def box_stl(path, lx, ly, lz):
    p = [(0, 0, 0), (lx, 0, 0), (lx, ly, 0), (0, ly, 0), (0, 0, lz), (lx, 0, lz), (lx, ly, lz), (0, ly, lz)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, cc, d in quads:
        tris += [(p[a], p[b], p[cc]), (p[a], p[cc], p[d])]
    with open(path, "wb") as f:
        f.write(b"transientflow block".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


def wait_job(c, job_id, limit=2400):
    for _ in range(limit):
        v, _ = call_ok(c, "job_status", {"job_id": job_id}, "job_status")
        if v.get("state") in ("succeeded", "failed", "cancelled"):
            return v
        time.sleep(0.1)
    check(False, f"job {job_id} did not finish")
    return {}


def run(c, args, what, expect="succeeded"):
    v, _ = call_ok(c, "analysis_run", args, what)
    if not v.get("job_id"):
        return {}, {}
    st = wait_job(c, v["job_id"])
    check(st.get("state") == expect, f"{what}: state {st.get('state')} (expected {expect}) {json.dumps(st.get('error'))[:400]}")
    return v, st


def main():
    tmp = Path(tempfile.mkdtemp(prefix="nvtransient"))
    ws = tmp / "ws"
    stl = tmp / "block.stl"
    box_stl(stl, 40.0, 10.0, 10.0)
    c = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(tmp)])
    c.initialize()

    print("== capabilities correspond to reachable settings")
    cap, _ = call_ok(c, "capabilities_get", {})
    th = {a["name"]: a for a in cap.get("analyses", [])}.get("transient_thermal", {})
    tsx = th.get("time_stepping", {})
    check("available" in tsx.get("adaptive", "") and "landed on exactly" in tsx.get("events", ""), "adaptive stepping and events are advertised")
    rsx = th.get("restart", {})
    check("available" in rsx.get("checkpoints", "") and "available" in rsx.get("resume", ""), "checkpoints and resume are advertised")
    for op in ("job_checkpoint", "job_pause", "job_resume"):
        check(op in {t["name"] for t in c.request("tools/list")["result"]["tools"]}, f"{op} is a reachable tool")
    tools = {t["name"]: t for t in c.request("tools/list")["result"]["tools"]}
    run_props = tools["analysis_run"]["inputSchema"]["properties"]
    for key in ("time_stepping", "temporal_relative_tolerance", "temporal_temperature_tolerance", "temporal_enthalpy_tolerance",
                "temporal_error_measure", "min_time_step", "max_time_step", "output_times", "output_interval", "max_rejections", "max_steps"):
        check(key in run_props, f"analysis_run declares {key}")
    check("schedule" in tools["boundary_apply"]["inputSchema"]["properties"], "boundary_apply declares schedule")

    print("== setup: a block with a scheduled heater and convection")
    K, RHO, CP = 20.0, 8000.0, 500.0
    call_ok(c, "project_create", {"name": "scheduled_heater", "description": "heater on from 5 s to 30 s, air-cooled end"})
    call_ok(c, "geometry_import", {"path": str(stl), "units": "mm", "name": "block"})
    call_ok(c, "material_define", {"material": {
        "id": "test_alloy", "name": "Verification alloy", "family": "metal", "status": "user_supplied",
        "provenance": "constant properties for the transient workflow test",
        "density_kg_m3": {"value": RHO}, "conductivity_w_per_mk": {"value": K}, "specific_heat_j_per_kgk": {"value": CP}}})
    call_ok(c, "material_assign", {"body": "block", "material": "test_alloy", "source": "user"})
    call_ok(c, "mesh_generate", {"element_size": "2.5 mm"})
    call_ok(c, "selection_create", {"name": "cooled_end", "query": {"plane": {"axis": "x", "at": "max"}}, "body": "block"})
    Q, T_ON, T_OFF = 2e6, 5.0, 30.0
    v, _ = call_ok(c, "boundary_apply", {"name": "heater", "kind": "heat_source", "body": "block", "power_density": f"{Q} W/m^3", "source": "user",
                                         "schedule": [{"time": "0 s", "factor": 0}, {"time": f"{T_ON} s", "factor": 1},
                                                      {"time": f"{T_OFF} s", "factor": 0}]})
    call_ok(c, "boundary_apply", {"name": "air", "kind": "convection", "selection": "cooled_end",
                                  "convection": {"coefficient": 50, "ambient": "20 degC"}, "source": "user"})

    print("== invalid schedules and settings are rejected")
    e = call_err(c, "boundary_apply", {"name": "bad1", "kind": "heat_source", "body": "block", "power_density": 1e5, "source": "user",
                                       "schedule": [{"time": "2 s", "factor": 1}]})
    check(e.get("code") == "INVALID_PARAMS" and "time 0" in e.get("message", ""), f"a schedule not starting at 0 is rejected ({e.get('message')})")
    e = call_err(c, "boundary_apply", {"name": "bad2", "kind": "convection", "selection": "cooled_end", "source": "user",
                                       "convection": {"coefficient": 10, "ambient": "20 degC"},
                                       "schedule": [{"time": 0, "factor": 1}, {"time": 5, "factor": 1}, {"time": 4, "factor": 0}]})
    check(e.get("code") == "INVALID_PARAMS" and "increase" in e.get("message", ""), f"non-increasing schedule times are rejected ({e.get('message')})")
    call_ok(c, "selection_create", {"name": "far_end", "query": {"plane": {"axis": "x", "at": "min"}}, "body": "block"})
    e = call_err(c, "boundary_apply", {"name": "bad3", "kind": "temperature", "selection": "far_end", "temperature": "50 degC", "source": "user",
                                       "schedule": [{"time": 0, "factor": 0.5}]})
    check(e.get("code") == "OUT_OF_RANGE", f"a prescribed temperature scaled by 0.5 is rejected ({e.get('code')})")
    e = call_err(c, "boundary_apply", {"name": "bad4", "kind": "fixed", "selection": "far_end", "source": "user",
                                       "schedule": [{"time": 0, "factor": 1}]})
    check(e.get("code") == "UNSUPPORTED", f"a schedule on a mechanical condition is unsupported ({e.get('code')})")
    e = call_err(c, "analysis_run", {"analysis": "transient_thermal", "end_time": "60 s", "time_step": "1 s", "temporal_relative_tolerance": 1e-4})
    check(e.get("code") == "INVALID_PARAMS" and "adaptive" in e.get("message", ""), "temporal tolerances with fixed steps are rejected, not ignored")

    print("== adaptive run with a switched heater and explicit output times")
    OUT = [1.0, 5.0, 10.0, 30.0, 45.0, 60.0]
    base = {"analysis": "transient_thermal", "end_time": "60 s", "time_stepping": "adaptive", "initial_temperature": "20 degC",
            "output_times": [f"{t} s" for t in OUT]}
    r1, s1 = run(c, dict(base, temporal_relative_tolerance=1e-3, temporal_temperature_tolerance="0.01 K"), "adaptive run, tolerance 1e-3")
    sm = s1.get("summary", {})
    tint = sm.get("time_integration", {})
    check(tint.get("stepping") == "adaptive" and tint.get("accepted_steps", 0) > 0, f"the summary reports adaptive stepping ({tint.get('accepted_steps')} steps)")
    check(tint.get("events", {}).get("condition_changes") == 2, f"both heater switches are events ({tint.get('events')})")
    times = [row["time_s"] for row in sm.get("history", [])]
    check(times == [0.0] + OUT, f"stored times are exactly the requested ones plus 0 ({times})")
    V = 0.040 * 0.010 * 0.010
    E = Q * V * (T_OFF - T_ON)
    en = sm.get("energy", {})
    check(abs(en.get("sources_j", 0) - E) <= 1e-9 * E, f"deposited energy {en.get('sources_j')} J equals Q V (t_off - t_on) = {E} J")
    check(en.get("closure_error", 1) < 1e-8 and en.get("enthalpy_mismatch", 1) < 1e-9, "the accepted budget closes and matches the enthalpy")
    asm = sm.get("assessment", {})
    for key in ("execution", "numerical_convergence", "conservation", "discretization_evidence", "material_provenance", "experimental_validation"):
        check(key in asm, f"the assessment reports {key}")
    check(asm.get("execution") == "COMPLETED" and "says nothing about accuracy" in asm.get("reading", ""),
          "completion is explicitly not presented as accuracy")
    check("step doubling" in tint.get("error_estimate", {}).get("method", ""), "the temporal error estimate method is named")
    check("not estimated" in asm.get("discretization_evidence", {}).get("spatial", ""), "the missing spatial error estimate is declared")
    q, _ = call_ok(c, "results_query", {"job_id": r1["job_id"], "quantity": "temperature", "time_s": 30})
    check(q.get("time_s") == 30.0, f"results_query selects the stored time 30 s exactly ({q.get('time_s')})")
    print(f"  tolerance 1e-3: {tint.get('accepted_steps')} accepted, {tint.get('rejected_steps')} rejected, "
          f"largest local estimate {tint.get('error_estimate', {}).get('largest_normalised_local_error')}, energy {en.get('sources_j')} J")

    print("== schema-to-solver wiring")
    r2, s2 = run(c, dict(base, temporal_relative_tolerance=1e-5, temporal_temperature_tolerance="0.0001 K"), "adaptive run, tolerance 1e-5")
    n1 = tint.get("accepted_steps", 0)
    n2 = s2.get("summary", {}).get("time_integration", {}).get("accepted_steps", 0)
    check(n2 > n1, f"a tighter tolerance reaches the solver: {n2} steps against {n1}")
    r3, s3 = run(c, dict(base, temporal_relative_tolerance=1e-3, temporal_temperature_tolerance="0.01 K", temporal_error_measure="temperature"),
                 "adaptive run, temperature-only measure")
    check(r3.get("spec_hash") != r1.get("spec_hash"), "the error measure is part of the specification hash")
    check(s3.get("summary", {}).get("time_integration", {}).get("tolerances", {}).get("measure") == "temperature only",
          "the summary reports the temperature-only measure")
    r4, s4 = run(c, dict(base, temporal_relative_tolerance=1e-6, temporal_temperature_tolerance="1e-6 K", min_time_step="5 s"),
                 "adaptive run with an impossible minimum step", expect="failed")
    er = s4.get("error", {})
    det = er.get("details", {})
    check(er.get("code") == "SOLVER_FAILED" and det.get("failure_class") == "MIN_STEP", f"MIN_STEP is reported as the failure class ({det.get('failure_class')})")
    check(isinstance(det.get("last_accepted_time_s"), (int, float)) and det.get("checkpoint_available") is True and len(det.get("recovery_options", [])) >= 2
          and "numerical" in det.get("checkpoint_note", ""),
          f"the failure names the last accepted time, the checkpoint it wrote (and why resuming alone will not fix a numerical failure) "
          f"and recovery options ({json.dumps(det)[:300]})")
    check("normalised error" in det.get("cause", ""), "the cause names the error measure")
    print(f"  wiring: {n1} -> {n2} steps when the tolerance tightens; MIN_STEP at t = {det.get('last_accepted_time_s')} s")

    print("== fixed stepping is unchanged and lands on schedule changes")
    r5, s5 = run(c, {"analysis": "transient_thermal", "end_time": "60 s", "time_step": "4 s", "initial_temperature": "20 degC",
                     "output_interval": "15 s"}, "fixed run with an output interval")
    sm5 = s5.get("summary", {})
    times5 = [row["time_s"] for row in sm5.get("history", [])]
    check(times5 == [0.0, 15.0, 30.0, 45.0, 60.0], f"output_interval stores 0, 15, 30, 45, 60 s ({times5})")
    en5 = sm5.get("energy", {})
    check(abs(en5.get("sources_j", 0) - E) <= 1e-9 * E, f"fixed 4 s steps still deposit exactly {E} J ({en5.get('sources_j')}): the 5 s switch is landed on")
    check(sm5.get("time_integration", {}).get("error_estimate", {}).get("method", "").startswith("none"), "fixed steps state that no error is estimated")

    print("== schedules persist")
    call_ok(c, "project_save", {})
    c.close()
    c2 = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(tmp)])
    c2.initialize()
    call_ok(c2, "project_open", {"path": str(ws / "scheduled_heater")})
    v, _ = call_ok(c2, "boundary_list", {})
    heater = [b for b in v.get("boundary_conditions", []) if b.get("name") == "heater"]
    sched = heater[0].get("schedule", []) if heater else []
    check([(e["time_s"], e["factor"]) for e in sched] == [(0, 0), (T_ON, 1), (T_OFF, 0)], f"the heater schedule survives save and reopen ({sched})")
    c2.close()

    print(f"\n{'TRANSIENT FLOW TESTS FAILED' if FAIL else 'ALL TRANSIENT FLOW TESTS PASSED'}: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
