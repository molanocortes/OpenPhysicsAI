#!/usr/bin/env python3
"""thermflow.py - the complete thermal workflow driven over MCP stdio exactly as an AI host would drive it:

  STL import -> units -> material (including an anisotropic and a melting one) -> volume mesh -> surface selections ->
  thermal boundary conditions -> validation -> transient thermal job -> energy balance -> temperature query, probe and
  image -> VTU/CSV export -> thermomechanical run with the thermal deformation

and three physics checks made through the operation layer rather than inside the solver:

  1. a heated bar cooled by convection reaches the analytical steady state, and the run's energy balance closes;
  2. an anisotropic material transports a different amount of heat than the isotropic one, in the predicted ratio;
  3. a melting material reports a molten volume and the latent heat appears in the enthalpy of the run.

This is a protocol-level test with a scripted client, NOT a test with a real AI model; see docs/mcp-clients.md.
Run from the repository root after `make am`:
    python3 tools/thermflow.py
"""
import json
import math
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcptest import MCP, Client  # noqa: E402

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
    check(ok, f"{what or name}: {json.dumps(sc.get('error'))[:400] if not ok else ''}")
    return sc.get("value", {}), r


def call_err(c, name, args):
    r = c.call(name, args)
    sc = (r.get("result", {}) or {}).get("structuredContent") or {}
    return sc.get("error") or {}


def box_stl(path, lx, ly, lz, name=b"thermflow bar"):
    """closed axis-aligned box from (0,0,0) to (lx,ly,lz) in mm, outward winding"""
    p = [(0, 0, 0), (lx, 0, 0), (lx, ly, 0), (0, ly, 0), (0, 0, lz), (lx, 0, lz), (lx, ly, lz), (0, ly, lz)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, cc, d in quads:
        tris.append((p[a], p[b], p[cc]))
        tris.append((p[a], p[cc], p[d]))
    with open(path, "wb") as f:
        f.write(name.ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


def wait_job(c, job_id, limit=1200):
    import time

    for _ in range(limit):
        v, _ = call_ok(c, "job_status", {"job_id": job_id}, "job_status")
        if v.get("state") in ("succeeded", "failed", "cancelled"):
            return v
        time.sleep(0.25)
    check(False, f"job {job_id} did not finish")
    return {}


def run_thermal(c, args, what):
    v, _ = call_ok(c, "analysis_run", args, what)
    if not v.get("job_id"):
        return {}, {}
    st = wait_job(c, v["job_id"])
    check(st.get("state") == "succeeded", f"{what} finished: {json.dumps(st.get('error'))[:300]}")
    return v, st


def main():
    tmp = Path(tempfile.mkdtemp(prefix="nvtherm"))
    ws = tmp / "ws"
    bar = tmp / "bar.stl"
    # 100 x 10 x 10 mm bar: a 1-D conduction problem whose steady state is known in closed form
    box_stl(bar, 100.0, 10.0, 10.0)
    common = ["--workspace", str(ws), "--allow-read", str(tmp)]

    print("== capabilities and setup through MCP tools")
    c = Client(["--embedded"] + common)
    c.initialize()
    tools = {t["name"] for t in c.request("tools/list")["result"]["tools"]}
    needed = {"capabilities_get", "material_define", "material_assign", "mesh_generate", "boundary_apply", "setup_validate",
              "analysis_run", "job_status", "results_query", "results_probe", "results_render", "results_export"}
    check(needed <= tools, f"thermal tools listed (missing: {sorted(needed - tools)})")

    cap, _ = call_ok(c, "capabilities_get", {})
    analyses = {a["name"]: a for a in cap.get("analyses", [])}
    check(analyses.get("transient_thermal", {}).get("status") == "available", "transient_thermal is advertised as available")
    check(analyses.get("thermomechanical", {}).get("status") == "available", "thermomechanical is advertised as available")
    notinc = " ".join(analyses.get("transient_thermal", {}).get("not_included", []))
    check("surface-to-surface" in notinc and "advection" in notinc and "global temporal error bound" in notinc,
          "the unsupported regimes are declared (enclosure radiation, advection outside a conjugate study, a global temporal error bound)")
    tsx = analyses.get("transient_thermal", {}).get("time_stepping", {})
    check("available" in tsx.get("adaptive", "") and "available" in tsx.get("fixed", ""),
          "adaptive and fixed time stepping are advertised as available")
    cpx = analyses.get("transient_thermal", {}).get("coupling", {})
    check("available" in cpx.get("orchestration", "") and "one-way" in cpx.get("thermomechanical", "") and
          "conjugate_heat_transfer" in cpx.get("strong_coupling", ""),
          "the coupling capabilities are declared: shared orchestration, one-way thermomechanics, strong coupling through the conjugate analysis")
    lims = " ".join(cap.get("model_limitations", []))
    check("view factors" in lims and "only in a conjugate_heat_transfer analysis" in lims,
          "model_limitations name the radiation and fluid-coupling limits")

    call_ok(c, "project_create", {"name": "thermal_bar", "description": "heated bar cooled by convection"})
    v, _ = call_ok(c, "geometry_import", {"path": str(bar), "units": "mm", "name": "bar"})
    check(v.get("body", {}).get("closed_solid") is True, "bar imported as a closed solid")

    # an explicit material with constant properties: the analytical steady state must be reproducible
    K, RHO, CP = 20.0, 8000.0, 500.0
    call_ok(c, "material_define", {"material": {
        "id": "test_alloy", "name": "Verification alloy", "family": "metal", "status": "user_supplied",
        "provenance": "constant properties chosen so that the steady state has a closed-form solution",
        "density_kg_m3": {"value": RHO}, "conductivity_w_per_mk": {"value": K}, "specific_heat_j_per_kgk": {"value": CP},
        "youngs_modulus_pa": {"value": 200e9}, "poisson_ratio": {"value": 0.3}, "expansion_1_per_k": {"value": 1.2e-5}}},
        "material_define (isotropic)")
    call_ok(c, "material_assign", {"body": "bar", "material": "test_alloy", "source": "user"})
    v, _ = call_ok(c, "mesh_generate", {"element_size": "2.5 mm"})
    nel = v.get("mesh", {}).get("elements")
    check(nel == 40 * 4 * 4, f"40 x 4 x 4 elements ({nel})")

    call_ok(c, "selection_create", {"name": "hot_end", "query": {"plane": {"axis": "x", "at": "min"}}, "body": "bar"})
    call_ok(c, "selection_create", {"name": "cold_end", "query": {"plane": {"axis": "x", "at": "max"}}, "body": "bar"})

    print("== thermal boundary conditions")
    THOT, TAMB, HCONV = 300.0, 20.0, 60.0
    call_ok(c, "boundary_apply", {"name": "hot", "kind": "temperature", "selection": "hot_end",
                                  "temperature": f"{THOT} degC", "source": "user"})
    call_ok(c, "boundary_apply", {"name": "cooled", "kind": "convection", "selection": "cold_end",
                                  "convection": {"coefficient": HCONV, "ambient": f"{TAMB} degC"}, "source": "user"})
    v, _ = call_ok(c, "boundary_list", {})
    kinds = sorted(b.get("kind") for b in v.get("boundary_conditions", []))
    check(kinds == ["convection", "temperature"], f"both thermal conditions are listed ({kinds})")

    e = call_err(c, "boundary_apply", {"name": "bad", "kind": "convection", "selection": "cold_end",
                                       "convection": {"coefficient": -5, "ambient": "20 degC"}, "source": "user"})
    check(e.get("code") in ("INVALID_PARAMS", "OUT_OF_RANGE"), f"a negative heat-transfer coefficient is rejected ({e.get('code')})")

    v, _ = call_ok(c, "setup_validate", {"analysis": "transient_thermal", "end_time": "600 s", "time_step": "5 s"})
    check(v.get("ready") is True, f"the thermal setup validates: {json.dumps(v.get('errors'))[:300]}")

    print("== transient thermal job and energy balance")
    # the diffusion time of the bar is L^2 rho cp / k = 2000 s, so 20000 s is ten diffusion times: essentially steady
    END, DT = 20000.0, 50.0
    run, st = run_thermal(c, {"analysis": "transient_thermal", "end_time": f"{END} s", "time_step": f"{DT} s",
                              "initial_temperature": f"{TAMB} degC", "output_every": 10}, "analysis_run (transient_thermal)")
    summary = st.get("summary", {})
    en = summary.get("energy", {})
    check(en.get("closure_error", 1) < 1e-6, f"the run's energy balance closes (closure error {en.get('closure_error')})")
    check(en.get("worst_step_balance_error", 1) < 1e-6, f"every step balances (worst {en.get('worst_step_balance_error')})")
    check(en.get("enthalpy_mismatch", 1) < 1e-9,
          f"the stored energy equals the independently integrated enthalpy change (mismatch {en.get('enthalpy_mismatch')})")
    cps = summary.get("coupling", {})
    check([p.get("name") for p in cps.get("participants", [])] == ["thermal"] and "nothing is coupled" in cps.get("summary", ""),
          f"a thermal run is one transient participant with nothing coupled ({json.dumps(cps)[:200]})")

    # analytical steady state of a bar with one end held at THOT and the far face cooled by convection.
    # A 10 x 10 mm section, 100 mm long: R_cond = L/(k A), R_conv = 1/(h A); the far-face temperature follows.
    L, A = 0.1, 0.01 * 0.01
    q = (THOT - TAMB) / (L / (K * A) + 1.0 / (HCONV * A))
    t_cold_exact = TAMB + q / (HCONV * A)
    v, _ = call_ok(c, "results_query", {"job_id": run["job_id"], "quantity": "temperature"})
    tmin = v.get("statistics", {}).get("min")
    # 3600 s is long compared with the diffusion time L^2 rho cp / k = 200 s, so the field is essentially steady
    check(tmin is not None and abs(tmin - t_cold_exact) < 0.5,
          f"cold face {tmin:.3f} degC against the analytical steady state {t_cold_exact:.3f} degC")
    check(abs(v.get("statistics", {}).get("max", 0) - THOT) < 1e-6, "the held end stays at the prescribed temperature")
    print(f"  bar: {q:.4f} W removed, cold face {tmin:.3f} degC (analytic {t_cold_exact:.3f} degC), "
          f"energy closure {en.get('closure_error'):.2e}, enthalpy mismatch {en.get('enthalpy_mismatch'):.2e}")

    v, _ = call_ok(c, "results_probe", {"job_id": run["job_id"], "points_mm": [[50, 5, 5]], "history": True})
    pts = v.get("probes", [])
    check(len(pts) == 1 and isinstance(pts[0].get("temperature_c"), (int, float)) and len(pts[0].get("history", [])) > 1,
          f"probe at mid-span returns a temperature and its history ({json.dumps(pts)[:200]})")
    _, r = call_ok(c, "results_render", {"job_id": run["job_id"], "quantity": "temperature"})
    imgs = [m for m in r["result"]["content"] if m.get("type") == "image"]
    check(len(imgs) == 1 and imgs[0]["mimeType"] == "image/png", "a temperature image comes back as PNG")

    out = ws / "export"
    out.mkdir(exist_ok=True)
    v, _ = call_ok(c, "results_export", {"job_id": run["job_id"], "formats": ["vtu", "csv"], "directory": str(out)})
    files = sorted(out.rglob("*.vtu"))
    check(len(files) >= 1, f"VTU series exported ({len(files)} files)")
    check(len(sorted(out.rglob("*.pvd"))) == 1, "a .pvd collection carries the times of the series")
    csvs = sorted(out.rglob("*.csv"))
    rows = csvs[0].read_text().strip().splitlines() if csvs else []
    check(len(rows) > 2 and "time" in rows[0].lower(), f"CSV time history exported ({len(rows)} rows)")

    print("== anisotropic conductivity through the operation layer")
    # the same bar with the second principal conductivity along x: heat flow must drop in the predicted ratio
    K2 = 5.0
    call_ok(c, "material_define", {"material": {
        "id": "test_alloy_aniso", "name": "Verification alloy, anisotropic", "family": "metal", "status": "user_supplied",
        "provenance": "the isotropic verification alloy with a second principal conductivity, for the frame-transform check",
        "density_kg_m3": {"value": RHO}, "specific_heat_j_per_kgk": {"value": CP},
        "conductivity_w_per_mk": {"value": K}, "conductivity_w_per_mk_2": {"value": K2},
        "conductivity_w_per_mk_3": {"value": K},
        # rows are the principal directions in the body frame: a 90 degree rotation about z puts k2 along x
        "conductivity_axes": [[0, -1, 0], [1, 0, 0], [0, 0, 1]]}}, "material_define (anisotropic)")
    e = call_err(c, "material_define", {"material": {
        "id": "bad_axes", "name": "not orthonormal", "family": "metal", "status": "user_supplied",
        "provenance": "a deliberately invalid frame",
        "density_kg_m3": {"value": RHO}, "specific_heat_j_per_kgk": {"value": CP},
        "conductivity_w_per_mk": {"value": K}, "conductivity_w_per_mk_2": {"value": K2},
        "conductivity_axes": [[1, 0, 0], [1, 0, 0], [0, 0, 1]]}})
    check(e.get("code") is not None and "orthonormal" in json.dumps(e), f"non-orthonormal conductivity axes are rejected ({e.get('code')})")

    call_ok(c, "material_assign", {"body": "bar", "material": "test_alloy_aniso", "source": "user"})
    run2, st2 = run_thermal(c, {"analysis": "transient_thermal", "end_time": f"{END} s", "time_step": f"{DT} s",
                                "initial_temperature": f"{TAMB} degC", "output_every": 10}, "analysis_run (anisotropic)")
    warns = json.dumps(st2.get("summary", {}).get("setup_warnings", [])) + json.dumps(st2.get("warnings", []))
    check("ANISOTROPIC_CONDUCTIVITY" in warns, "the run warns that the material conducts anisotropically")
    v2, _ = call_ok(c, "results_query", {"job_id": run2["job_id"], "quantity": "temperature"})
    q2 = (THOT - TAMB) / (L / (K2 * A) + 1.0 / (HCONV * A))
    t_cold_exact2 = TAMB + q2 / (HCONV * A)
    tmin2 = v2.get("statistics", {}).get("min")
    check(tmin2 is not None and abs(tmin2 - t_cold_exact2) < 0.5,
          f"anisotropic cold face {tmin2:.3f} degC against the analytical {t_cold_exact2:.3f} degC")
    check(tmin2 < tmin - 10, "the low principal conductivity along the bar transports visibly less heat")
    print(f"  anisotropic: k_x = {K2} W/(m K) gives {q2:.4f} W and a {tmin2:.3f} degC cold face "
          f"(isotropic k = {K}: {q:.4f} W, {tmin:.3f} degC)")

    # save before moving on: the thermomechanical run below reopens this project in a second MCP client, which also
    # checks that a saved project carries its mesh, materials and conditions across a disconnect
    call_ok(c, "material_assign", {"body": "bar", "material": "test_alloy", "source": "user"})
    call_ok(c, "project_save", {})

    print("== latent heat of melting through the operation layer")
    # a small block heated hard enough to melt: the run must report a molten volume, and switching the capacity
    # model off must change the answer (which is what proves the latent heat is actually being carried)
    call_ok(c, "project_create", {"name": "melt_block", "description": "block driven above its solidus"})
    blk = tmp / "block.stl"
    box_stl(blk, 10.0, 10.0, 10.0, b"melt block")
    call_ok(c, "geometry_import", {"path": str(blk), "units": "mm", "name": "block"})
    call_ok(c, "material_assign", {"body": "block", "material": "ss316l_lpbf_demo", "source": "default"})
    call_ok(c, "mesh_generate", {"element_size": "2.5 mm"})
        # 1e-6 m^3 of 316L needs roughly 8000 * 500 * 1350 = 5.4e9 J/m^3 of sensible heat to reach the solidus
    # plus 8000 * 2.7e5 = 2.2e9 J/m^3 of latent heat: a 4e8 W/m^3 source crosses both within 20 s.
    call_ok(c, "boundary_apply", {"name": "heating", "kind": "heat_source", "body": "block",
                                  "power_density": "4e8 W/m^3", "source": "user"})
    runm, stm = run_thermal(c, {"analysis": "transient_thermal", "end_time": "20 s", "time_step": "0.25 s",
                                "initial_temperature": "20 degC", "max_iterations": 1500,
                                "temperature_tolerance": 1e-9, "output_every": 10}, "analysis_run (melting)")
    sm = stm.get("summary", {})
    ph = sm.get("phase_change", {})
    warns = json.dumps(sm.get("setup_warnings", [])) + json.dumps(stm.get("warnings", []))
    check("PHASE_CHANGE_ACTIVE" in warns, "the run states that the latent heat of melting is applied")
    check(ph.get("largest_molten_volume_mm3", 0) > 0, f"a molten volume is reported ({ph.get('largest_molten_volume_mm3')} mm^3)")
    check(sm.get("energy", {}).get("enthalpy_mismatch", 1) < 1e-9,
          f"stored energy still equals the enthalpy change while melting (mismatch {sm.get('energy', {}).get('enthalpy_mismatch')})")
    vm, _ = call_ok(c, "results_query", {"job_id": runm["job_id"], "quantity": "temperature"})
    t_latent = vm.get("statistics", {}).get("max")

    runs, sts = run_thermal(c, {"analysis": "transient_thermal", "end_time": "20 s", "time_step": "0.25 s",
                                "initial_temperature": "20 degC", "phase_change": False,
                                "output_every": 10}, "analysis_run (no phase change)")
    vs, _ = call_ok(c, "results_query", {"job_id": runs["job_id"], "quantity": "temperature"})
    t_nolatent = vs.get("statistics", {}).get("max")
    check("PHASE_CHANGE_DISABLED" in (json.dumps(sts.get("summary", {}).get("setup_warnings", [])) + json.dumps(sts.get("warnings", []))),
          "turning the phase change off is reported as a modelling choice, not silently applied")
    check(t_latent is not None and t_nolatent is not None and t_nolatent > t_latent + 20,
          f"absorbing the latent heat holds the block back: {t_latent:.1f} degC with melting, {t_nolatent:.1f} degC without")
    print(f"  melting: {ph.get('largest_molten_volume_mm3'):.3f} mm^3 molten, peak {t_latent:.1f} degC with latent heat "
          f"and {t_nolatent:.1f} degC without it")

    print("== thermomechanical run: temperature history drives the deformation")
    c2 = Client(["--embedded"] + common)
    c2.initialize()
    v, _ = call_ok(c2, "project_open", {"path": str(ws / "thermal_bar")})
    check(v.get("project", {}).get("bodies") or v.get("bodies"), f"the reopened project carries its body ({json.dumps(v)[:200]})")
    # the stored mesh settings are reloaded but the mesh itself is rebuilt on demand
    call_ok(c2, "mesh_generate", {"element_size": "2.5 mm"})
    call_ok(c2, "selection_create", {"name": "anchor", "query": {"plane": {"axis": "x", "at": "min"}}, "body": "bar"})
    call_ok(c2, "boundary_apply", {"name": "held", "kind": "fixed", "selection": "anchor", "source": "user"})
    runtm, sttm = run_thermal(c2, {"analysis": "thermomechanical", "end_time": "1800 s", "time_step": "30 s",
                                   "initial_temperature": f"{TAMB} degC", "stress_free_temperature": f"{TAMB} degC",
                                   "output_every": 10}, "analysis_run (thermomechanical)")
    v, _ = call_ok(c2, "results_query", {"job_id": runtm["job_id"], "quantity": "displacement"})
    umax = v.get("statistics", {}).get("max")
    check(umax is not None and umax > 0, f"the thermomechanical run reports a displacement ({umax})")
    note = json.dumps(sttm.get("summary", {}))
    check("one-way" in note or "sequential" in note or "residual" in note,
          "the summary states that the coupling is one-way and not a residual-stress prediction")
    # Stage D6: the job runs through the shared orchestrator, and its summary says how the participants were coupled
    cp = sttm.get("summary", {}).get("coupling", {})
    parts = {p.get("name"): p for p in cp.get("participants", [])}
    check(parts.get("thermal", {}).get("kind") == "transient" and parts.get("structure", {}).get("kind") == "quasi-static",
          f"the thermomechanical run has a transient thermal and a quasi-static structural participant ({json.dumps(cp)[:240]})")
    check("one-way" in cp.get("character", "") and "stored times" in parts.get("structure", {}).get("evaluated", ""),
          "the coupling is declared one-way, with the structure evaluated at the stored times")
    nstored = sttm.get("summary", {}).get("time_integration", {}).get("events", {}).get("stored_times")
    check(nstored and cp.get("stored_times_with_structural_results") == nstored + 1,
          f"structural results exist for the initial time and every stored time ({cp.get('stored_times_with_structural_results')} for {nstored} + 1)")
    print(f"  thermomechanical: largest displacement {umax:.4f} mm from the thermal expansion of the heated bar")

    c.close()
    c2.close()
    print(f"\n{'THERMAL FLOW TESTS FAILED' if FAIL else 'ALL THERMAL FLOW TESTS PASSED'}: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
