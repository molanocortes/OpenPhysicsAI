#!/usr/bin/env python3
"""chtflow.py - a fluid-thermal-solid study (heated plate in an air channel) configured, solved and checked through MCP only.

Criteria were fixed before the first run: "Thermal Sim/verification/stageE-criteria.md" (E5a..E5h, with the recorded
E5b amendment).

Run from the repository root after `make am`:
    python3 tools/chtflow.py            (add --quick to skip the 1 mm and tight-tolerance runs of E5h)
"""
import json
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcptest import Client  # noqa: E402
from contactflow import box_stl  # noqa: E402
from restartflow import read_nvt  # noqa: E402

PASS = 0
FAIL = 0
QUICK = "--quick" in sys.argv


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


def poll(c, job_id):
    r = c.call("job_status", {"job_id": job_id})
    return (r.get("result", {}).get("structuredContent") or {}).get("value", {})


def wait(c, job_id, limit_s=1800):
    t0 = time.time()
    while time.time() - t0 < limit_s:
        st = poll(c, job_id)
        if st.get("state") in ("succeeded", "failed", "cancelled", "paused"):
            return st
        time.sleep(0.1)
    check(False, f"job {job_id} did not finish in {limit_s} s")
    return {}


PLATE_Q = 0.1 / (40e-3 * 20e-3 * 4e-3)  # 0.1 W in the plate, W/m^3
STUDY = {"analysis": "conjugate_heat_transfer", "end_time": "7200 s", "time_stepping": "adaptive", "initial_temperature": "20 degC",
         "temporal_relative_tolerance": 1e-3, "temporal_temperature_tolerance": "0.05 K", "output_interval": "600 s",
         "flow": {"fluid_body": "air", "inlet_velocity": "0.02 m/s", "inlet_temperature": "20 degC",
                  "walls": {"y_min": "periodic", "y_max": "periodic", "z_min": "no_slip", "z_max": "no_slip"}},
         "structural_response": True, "stress_free_temperature": "20 degC", "checkpoint_wall_interval": 0, "allow_duplicate": True}


def setup(c, name, tmp, element="2 mm"):
    plate, air = tmp / "plate.stl", tmp / "air.stl"
    box_stl(plate, 0, 0, 0, 40, 20, 4, b"chtflow plate")
    box_stl(air, 0, 0, 0, 60, 20, 10, b"chtflow air channel")
    call_ok(c, "project_create", {"name": name, "description": "steel plate heated in an air channel (conjugate heat transfer)"})
    call_ok(c, "geometry_import", {"path": str(plate), "units": "mm", "name": "plate"})
    call_ok(c, "geometry_import", {"path": str(air), "units": "mm", "name": "air"})
    call_ok(c, "geometry_place", {"body": "plate", "position": [0, 0], "z_offset": 0})
    call_ok(c, "geometry_place", {"body": "air", "position": [0, 0], "z_offset": 4})
    call_ok(c, "material_assign", {"body": "plate", "material": "steel_plate_demo", "source": "user"})
    call_ok(c, "material_assign", {"body": "air", "material": "air_demo", "source": "user"})
    call_ok(c, "mesh_generate", {"element_size": element})
    call_ok(c, "selection_create", {"name": "plate_end", "query": {"plane": {"axis": "x", "at": "min"}}, "body": "plate"})
    call_ok(c, "selection_create", {"name": "outlet", "query": {"plane": {"axis": "x", "at": "max"}}, "body": "air"})
    call_ok(c, "boundary_apply", {"name": "heater", "kind": "heat_source", "body": "plate", "power_density": f"{PLATE_Q} W/m^3", "source": "user"})
    call_ok(c, "boundary_apply", {"name": "held", "kind": "fixed", "selection": "plate_end", "source": "user"})


def run(c, args, what):
    v, _ = call_ok(c, "analysis_run", args, what)
    if not v.get("job_id"):
        return {}, {}
    t0 = time.time()
    st = wait(c, v["job_id"])
    check(st.get("state") == "succeeded", f"{what}: {st.get('state')} {json.dumps(st.get('error'))[:500]}")
    st["_wall"] = time.time() - t0
    return v, st


def outcomes(c, job_id):
    """final plate peak temperature and mean outlet-face air temperature"""
    v, _ = call_ok(c, "results_query", {"job_id": job_id, "quantity": "temperature"})
    peak = v.get("statistics", {}).get("max")
    o, _ = call_ok(c, "results_query", {"job_id": job_id, "quantity": "temperature", "selection": "outlet"})
    return peak, o.get("statistics", {}).get("mean")


def main():
    tmp = Path(tempfile.mkdtemp(prefix="nvcht"))
    ws = tmp / "ws"
    common = ["--workspace", str(ws), "--allow-read", str(tmp)]
    c = Client(["--embedded"] + common)
    c.initialize()

    print("== capabilities")
    cap, _ = call_ok(c, "capabilities_get", {})
    analyses = {a["name"]: a for a in cap.get("analyses", [])}
    ch = analyses.get("conjugate_heat_transfer", {})
    check(ch.get("status") == "available", f"conjugate_heat_transfer is advertised ({ch.get('status')})")
    check("one-way" in json.dumps(ch) and "laminar" in json.dumps(ch), "its declaration states the laminar, one-way flow")

    print("== E5a setup and refusals")
    setup(c, "cht_plate", tmp)
    val = {k: v for k, v in STUDY.items() if k not in ("allow_duplicate",)}
    v, _ = call_ok(c, "setup_validate", val, "setup_validate conjugate study")
    codes = {w.get("code") for w in v.get("warnings", [])}
    check(v.get("ready") is True and {"ONE_WAY_FLOW", "PERIODIC_FLOW_SIDES"} <= codes, f"ready with the flow warnings ({v.get('ready')}, {sorted(codes)})")
    check("UNDECLARED_CONTACT" not in codes, "the resolved fluid-solid interface is not reported as an undeclared bond")
    fl = v.get("model", {}).get("flow", {})
    check(fl.get("lattice_cells") == [30, 10, 5] and abs(fl.get("conjugate_interface_area_mm2", 0) - 800) < 1e-6,
          f"the model describes the lattice and the interface ({json.dumps(fl)[:300]})")
    e = call_err(c, "analysis_run", dict(STUDY, flow=dict(STUDY["flow"], walls={"y_min": "periodic"})))
    check(e.get("code") == "INVALID_PARAMS" and "periodic" in e.get("message", ""), f"a one-sided periodic flow is refused ({e.get('message')})")
    e = call_err(c, "analysis_run", {"analysis": "transient_thermal", "end_time": "10 s", "time_step": "1 s", "flow": STUDY["flow"]})
    check(e.get("code") == "INVALID_PARAMS" and "conjugate_heat_transfer" in e.get("message", ""), f"flow settings on transient_thermal are refused ({e.get('message')})")
    call_ok(c, "material_assign", {"body": "air", "material": "steel_plate_demo", "source": "user"})
    e = call_err(c, "analysis_run", STUDY)
    check("not a fluid" in json.dumps(e) and "air_demo" in json.dumps(e), f"a non-fluid material in the flow domain is refused ({json.dumps(e)[:300]})")
    call_ok(c, "material_assign", {"body": "air", "material": "air_demo", "source": "user"})
    call_ok(c, "selection_create", {"name": "inlet", "query": {"plane": {"axis": "x", "at": "min"}}, "body": "air"})
    call_ok(c, "boundary_apply", {"name": "inlet_conv", "kind": "convection", "selection": "inlet", "source": "user",
                                  "convection": {"coefficient": 10, "ambient": "20 degC"}})
    e = call_err(c, "analysis_run", STUDY)
    check("inlet plane" in json.dumps(e), f"a condition on the inlet plane is refused ({json.dumps(e)[:300]})")
    call_ok(c, "boundary_remove", {"name": "inlet_conv"})
    call_ok(c, "mesh_generate", {"element_size": "2 mm", "element_size_z": "1 mm"})
    e = call_err(c, "analysis_run", STUDY)
    check("cubic" in json.dumps(e), f"a mesh with non-cubic cells is refused ({json.dumps(e)[:300]})")
    call_ok(c, "mesh_generate", {"element_size": "2 mm"})
    call_ok(c, "project_save", {})
    # a flow domain that touches no solid
    lone = tmp / "lone"
    lone.mkdir()
    c3 = Client(["--embedded", "--workspace", str(lone), "--allow-read", str(tmp)])
    c3.initialize()
    call_ok(c3, "project_create", {"name": "lonely_air", "description": "air box without a solid"})
    box_stl(tmp / "air2.stl", 0, 0, 0, 60, 20, 10)
    box_stl(tmp / "block.stl", 0, 0, 0, 10, 10, 10)
    call_ok(c3, "geometry_import", {"path": str(tmp / "air2.stl"), "units": "mm", "name": "air"})
    call_ok(c3, "geometry_import", {"path": str(tmp / "block.stl"), "units": "mm", "name": "block"})
    call_ok(c3, "geometry_place", {"body": "block", "position": [80, 0]})
    call_ok(c3, "material_assign", {"body": "air", "material": "air_demo", "source": "user"})
    call_ok(c3, "material_assign", {"body": "block", "material": "steel_plate_demo", "source": "user"})
    call_ok(c3, "mesh_generate", {"element_size": "2 mm"})
    e = call_err(c3, "analysis_run", dict(STUDY, structural_response=False))
    check("no solid body touches" in json.dumps(e), f"a fluid body touching no solid is refused ({json.dumps(e)[:300]})")
    c3.close()

    print("== E5b partitioned adaptive study")
    v, st = run(c, dict(STUDY, label="partitioned"), "partitioned study")
    part_id, part_dir, sm = v.get("job_id"), v.get("run_directory"), st.get("summary", {})
    flow, ci, en = sm.get("flow", {}), sm.get("conjugate_interface", {}), sm.get("energy", {})
    cp, asm = sm.get("coupling", {}), sm.get("assessment", {})
    dens = flow.get("density_max", 9) - flow.get("density_min", 0)
    print(f"  {sm.get('time_integration', {}).get('accepted_steps')} steps, {st.get('_wall', 0):.1f} s wall; flow: tau {flow.get('relaxation_time'):.4f}, "
          f"Mach {flow.get('lattice_mach'):.4f}, Re_h {flow.get('reynolds_hydraulic'):.3g}, density range {dens:.4f}, section scale "
          f"{flow.get('section_scale_min'):.4f}..{flow.get('section_scale_max'):.4f}")
    check(flow.get("computed") is True and flow.get("relaxation_time", 0) >= 0.52 and flow.get("lattice_mach", 1) <= 0.11 and dens <= 0.03,
          f"the flow is reported within its limits ({json.dumps(flow)[:300]})")
    check(isinstance(flow.get("section_scale_min"), (int, float)) and isinstance(flow.get("section_scale_max"), (int, float)), "section scale factors reported")
    check(en.get("closure_error", 1) <= 1e-6, f"energy closure {en.get('closure_error')}")
    check(ci.get("conservation_error", 1) <= 1e-9, f"interface heat conserved ({ci.get('conservation_error')})")
    tmax_k = sm.get("temperature", {}).get("max_c", 1000) + 273.15
    jump_bound = 1e-8 * tmax_k  # E5b amended: coupling relative tolerance times the largest interface temperature
    check(ci.get("largest_temperature_jump_k", 1) <= jump_bound, f"interface temperature jump {ci.get('largest_temperature_jump_k')} K <= {jump_bound:.3g} K")
    check(asm.get("coupling_convergence", {}).get("converged_every_step") is True, "coupling converged every step")
    check("overshoot" in sm.get("fluid_energy", {}), "overshoot diagnostics reported")
    text = json.dumps(cp) + json.dumps(asm) + json.dumps(sm.get("interpretation"))
    check("one-way" in cp.get("summary", "") and "strongly coupled" in cp.get("summary", "") and "one-way" in cp.get("structure", ""),
          f"the coupling is labelled: one-way flow, strongly coupled fluid and solids, one-way structure ({cp.get('summary', '')[:200]})")
    check(all(m.get("status") == "demonstration" for m in asm.get("material_provenance", [])) and "Demonstration materials" in text,
          "demonstration materials are stated")
    ci_h = ci.get("heat_into_fluid_j", 0)
    print(f"  interface: {ci_h:.4f} J into the air, jump {ci.get('largest_temperature_jump_k'):.3g} K, conservation {ci.get('conservation_error'):.2g}; "
          f"closure {en.get('closure_error'):.2g}; coupling {cp.get('statistics', {}).get('iterations_per_accepted_step', 0):.2f} iterations per step")

    print("== schema-to-solver wiring of the coupling and flow settings (600 s runs)")
    short = dict(STUDY, end_time="600 s", output_interval="300 s", structural_response=False)
    _, w0 = run(c, dict(short, label="wiring default"), "wiring: default coupling")
    _, w1 = run(c, dict(short, label="wiring loose", coupling={"relative_tolerance": 1e-4}), "wiring: coupling.relative_tolerance 1e-4")
    s0, s1 = w0.get("summary", {}), w1.get("summary", {})
    it0 = s0.get("coupling", {}).get("statistics", {}).get("coupling_iterations", 0)
    it1 = s1.get("coupling", {}).get("statistics", {}).get("coupling_iterations", 0)
    j1 = s1.get("conjugate_interface", {}).get("largest_temperature_jump_k", 1)
    check(it1 < it0 and j1 <= 1e-4 * (s1.get("temperature", {}).get("max_c", 1e9) + 273.15),
          f"coupling.relative_tolerance reaches the iteration ({it0} -> {it1} coupling iterations, jump {j1:.3g} K)")
    vcap, _ = call_ok(c, "analysis_run", dict(short, label="wiring capped", coupling={"relative_tolerance": 1e-12, "max_iterations": 2}),
                      "wiring: coupling.max_iterations 2")
    w2 = wait(c, vcap.get("job_id", "")) if vcap.get("job_id") else {}
    rej = w2.get("summary", {}).get("coupling", {}).get("statistics", {}).get("rejected_for_coupling", 0)
    detail = json.dumps(w2.get("error") or {})
    check(rej > 0 or (w2.get("state") == "failed" and "coupl" in detail),
          f"coupling.max_iterations reaches the orchestrator ({w2.get('state')}: {rej} steps rejected for coupling {detail[:200]})")
    _, w3 = run(c, dict(short, label="wiring flow", flow=dict(STUDY["flow"], steady_tolerance=1e-4)), "wiring: flow.steady_tolerance 1e-4")
    l0, l3 = s0.get("flow", {}).get("lattice_steps", 0), w3.get("summary", {}).get("flow", {}).get("lattice_steps", 0)
    check(0 < l3 < l0, f"flow.steady_tolerance reaches the lattice run ({l0} -> {l3} lattice steps)")

    print("== E5c conservation of the whole study (criterion amended, see stageE-criteria.md)")
    source = en.get("sources_j", 0)
    carried = en.get("enthalpy_out_j", 0) - en.get("enthalpy_in_j", 0)
    advected = en.get("advected_into_fluid_j", 0)
    closure_c = abs(source - en.get("stored_j", 0) - carried) / max(abs(source), 1e-300)
    discrete_c = abs(source + advected - en.get("stored_j", 0)) / max(abs(source), 1e-300)
    print(f"  source {source:.6g} J = stored {en.get('stored_j'):.6g} J - advected into the fluid {advected:.6g} J (discrete closure {discrete_c:.2g})")
    print(f"  enthalpy-flux form: stored + (out - in) = {en.get('stored_j', 0) + carried:.6g} J, closure {closure_c:.3g} "
          f"(original E5c criterion 1e-6: {'met' if closure_c <= 1e-6 else 'NOT met'})")
    check(discrete_c <= 1e-6, f"(a) discrete balance source = stored + advected closes within 1e-6 ({discrete_c:.3g})")
    check(isinstance(en.get("unexplained_advective_j"), (int, float)), "(b) the unexplained advective energy is reported")

    print("== E5g results through MCP")
    v, _ = call_ok(c, "results_query", {"job_id": part_id, "quantity": "temperature"})
    check(isinstance(v.get("statistics", {}).get("max"), (int, float)), "final temperature field queried")
    v, _ = call_ok(c, "results_probe", {"job_id": part_id, "points_mm": [[0, 0, 2], [19, 0, 2]], "history": True})
    probes = v.get("probes", [])
    check(len(probes) == 2 and len(probes[0].get("history", [])) > 2, "plate probe history returned")
    dx = probes[1].get("displacement_mm", {}).get("x", 0) if len(probes) == 2 else 0
    v, _ = call_ok(c, "results_query", {"job_id": part_id, "quantity": "displacement"})
    check((v.get("statistics", {}).get("max") or 0) > 0 and dx > 0, f"thermal expansion moves the free end towards +x ({dx:.4g} mm)")
    out = ws / "export"
    call_ok(c, "results_export", {"job_id": part_id, "formats": ["vtu"], "directory": str(out)})
    vtus = sorted(out.rglob("*.vtu"))
    check(bool(vtus) and b"velocity_m_s" in vtus[-1].read_bytes()[:4000], "the VTU export carries velocity_m_s")

    print("== E5d monolithic against partitioned")
    v, stm = run(c, dict(STUDY, label="monolithic", coupling={"scheme": "monolithic"}), "monolithic study")
    mono_dir = v.get("run_directory")
    _, ap = read_nvt(Path(part_dir) / "results.nvt")
    _, am = read_nvt(Path(mono_dir) / "results.nvt")
    dT = max((abs(x - y) for x, y in zip(ap["T"], am["T"])), default=float("inf")) if len(ap["T"]) == len(am["T"]) else float("inf")
    sp_, sm_ = sm.get("time_integration", {}).get("accepted_steps"), stm.get("summary", {}).get("time_integration", {}).get("accepted_steps")
    print(f"  partitioned {sp_} steps ({st.get('_wall', 0):.1f} s), monolithic {sm_} steps ({stm.get('_wall', 0):.1f} s); max stored-temperature difference {dT:.3g} K")
    check(sp_ == sm_, f"the same number of accepted steps ({sp_}, {sm_})")
    check(dT <= 1e-5, f"stored temperatures agree within 1e-5 K ({dT:.3g})")

    print("== E5e pause and resume")
    v, _ = call_ok(c, "analysis_run", dict(STUDY, label="paused"), "study to pause")
    pid, pdir = v.get("job_id"), v.get("run_directory")
    for _ in range(20000):
        s = poll(c, pid)
        if s.get("progress", 0) >= 0.4 or s.get("state") not in ("running", "queued"):
            break
        time.sleep(0.005)
    call_ok(c, "job_pause", {"job_id": pid})
    s = wait(c, pid)
    ck = s.get("checkpoint", {})
    check(s.get("state") == "paused" and ck.get("available"), f"paused with a checkpoint ({s.get('state')}, {ck.get('time_s')})")
    v, _ = call_ok(c, "job_resume", {"job_id": pid})
    s = wait(c, pid)
    check(s.get("state") == "succeeded", f"the resumed study completes ({s.get('state')})")
    _, ar = read_nvt(Path(pdir) / "results.nvt")
    same = ar["T"] == ap["T"] and ar["mech_u"] == ap["mech_u"] and ar["times"] == ap["times"]
    e1, e2 = sm.get("energy", {}), s.get("summary", {}).get("energy", {})
    same_e = all(e1.get(k) == e2.get(k) for k in ("sources_j", "stored_j", "enthalpy_in_j", "enthalpy_out_j", "unexplained_advective_j"))
    same_i = sm.get("conjugate_interface", {}).get("heat_into_fluid_j") == s.get("summary", {}).get("conjugate_interface", {}).get("heat_into_fluid_j")
    print(f"  paused at t = {ck.get('time_s')} s; frames {'bitwise identical' if same else 'DIFFERENT'}, energies {same_e}, interface heat {same_i}")
    check(same and same_e and same_i, "the resumed study is bitwise identical to the uninterrupted one (temperatures, displacements, energies, interface heat)")
    check(s.get("summary", {}).get("restart", {}).get("resumed_from_s") == [ck.get("time_s")], "the summary records the resume")

    print("== E5f save, reopen in a new server process, reproduce")
    c.close()
    c2 = Client(["--embedded"] + common)
    c2.initialize()
    call_ok(c2, "project_open", {"path": str(ws / "cht_plate")})
    call_ok(c2, "mesh_generate", {"element_size": "2 mm"})
    v, s2 = run(c2, dict(STUDY, label="reopened"), "reopened study")
    _, a2 = read_nvt(Path(v.get("run_directory")) / "results.nvt")
    scale = max(abs(x) for x in ap["T"])
    d2 = max((abs(x - y) for x, y in zip(ap["T"], a2["T"])), default=float("inf")) if len(ap["T"]) == len(a2["T"]) else float("inf")
    check(d2 <= 1e-12 * scale, f"the reopened study reproduces the stored temperatures ({d2:.3g} K)")

    print("== E5h discretization evidence")
    base_peak, base_out = outcomes(c2, v.get("job_id"))
    rows = [("2 mm, tolerance 1e-3 / 0.05 K", base_peak, base_out, s2)]
    if not QUICK:
        v, st_t = run(c2, dict(STUDY, label="tight", temporal_relative_tolerance=1e-4, temporal_temperature_tolerance="0.005 K"), "tolerance / 10")
        pk, oa = outcomes(c2, v.get("job_id"))
        rows.append(("2 mm, tolerance 1e-4 / 0.005 K", pk, oa, st_t))
        call_ok(c2, "mesh_generate", {"element_size": "1 mm"})
        v, st_f = run(c2, dict(STUDY, label="fine"), "1 mm cells")
        pk, oa = outcomes(c2, v.get("job_id"))
        rows.append(("1 mm, tolerance 1e-3 / 0.05 K", pk, oa, st_f))
    for label, pk, oa, stx in rows:
        smx = stx.get("summary", {})
        enx, cix = smx.get("energy", {}), smx.get("conjugate_interface", {})
        print(f"  {label}: plate peak {pk:.4f} degC, mean outlet face {oa:.4f} degC; {smx.get('time_integration', {}).get('accepted_steps')} steps, "
              f"{stx.get('_wall', 0):.1f} s; closure {enx.get('closure_error', 1):.2g}, interface conservation {cix.get('conservation_error', 1):.2g}")
        srcx = enx.get("sources_j", 0)
        closx = abs(srcx - enx.get("stored_j", 0) - (enx.get("enthalpy_out_j", 0) - enx.get("enthalpy_in_j", 0))) / max(abs(srcx), 1e-300)
        print(f"    enthalpy-flux closure {closx:.3g}; unexplained advective energy {enx.get('unexplained_advective_j', 0):.4g} J "
              f"({abs(enx.get('unexplained_advective_j', 0)) / max(abs(srcx), 1e-300):.3g} of the source)")
        discx = abs(srcx + enx.get("advected_into_fluid_j", 0) - enx.get("stored_j", 0)) / max(abs(srcx), 1e-300)
        stx["_flux_closure"] = closx
        check(enx.get("closure_error", 1) <= 1e-6 and cix.get("conservation_error", 1) <= 1e-9 and discx <= 1e-6, f"{label}: conservation holds")
    if len(rows) == 3:
        f2, f1 = rows[0][3].get("_flux_closure", 1), rows[2][3].get("_flux_closure", 0)
        check(f1 < f2, f"(E5c b) the enthalpy-flux closure decreases with refinement ({f2:.3g} at 2 mm, {f1:.3g} at 1 mm)")
        print(f"  temporal: tolerance / 10 changes the plate peak by {rows[1][1] - rows[0][1]:+.4f} K and the outlet mean by {rows[1][2] - rows[0][2]:+.4f} K")
        print(f"  spatial: 1 mm cells change the plate peak by {rows[2][1] - rows[0][1]:+.4f} K and the outlet mean by {rows[2][2] - rows[0][2]:+.4f} K "
              f"(two levels: no order is claimed)")
    c2.close()
    print(f"\n{'CONJUGATE FLOW TESTS FAILED' if FAIL else 'ALL CONJUGATE FLOW TESTS PASSED'}: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
