#!/usr/bin/env python3
"""printflow.py - the print analysis driven over MCP stdio exactly as an AI host would drive it:

  printed wall (STL) -> material with provenance -> voxel mesh -> mech_print_run (job kind fff_print) ->
  job_status -> the stored times through the ordinary result operations (results_query, results_probe,
  results_quantities, results_export) -> the summary's machine-readable scope

plus the refusals that keep the answer honest: a process without provenance, a part that does not rest on the build
plate, a stale mesh, a material without provenance, and a print of two bodies; the duplicate-detection of an identical
run; and a second engine session that reopens the run directory and reads the same numbers.

The energy balance of every step and the self-equilibrium of the released part are checked from the results, not from
the log.

This is a protocol-level test with a scripted client, NOT a test with a real AI model.
Run from the repository root after `make`:
    python3 tools/printflow.py
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


def check(cond, what):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"  FAIL: {what}")


def box_stl(path, lo, hi, name=b"printflow box"):
    """closed box between corners lo and hi (mm), outward winding"""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    v = [(x0, y0, z0), (x1, y0, z0), (x0, y1, z0), (x1, y1, z0), (x0, y0, z1), (x1, y0, z1), (x0, y1, z1), (x1, y1, z1)]
    faces = [(0, 2, 3), (0, 3, 1), (4, 5, 7), (4, 7, 6), (0, 1, 5), (0, 5, 4), (2, 6, 7), (2, 7, 3), (0, 4, 6), (0, 6, 2), (1, 3, 7), (1, 7, 5)]
    with open(path, "wb") as f:
        f.write(name.ljust(80, b" "))
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


def wait_job(c, job, tries=120):
    status = {}
    for _ in range(tries):
        status = call_ok(c, "job_status", {"job_id": job, "wait_seconds": 10}, "job_status")
        if status.get("state") not in ("queued", "running"):
            break
    return status


PROCESS = {
    "layer_height": "1 mm",
    "printed_layer_height": "0.2 mm",
    "nozzle_temperature": "210 degC",
    "bed_temperature": "60 degC",
    "ambient_temperature": "30 degC",
    "deposition_rate": "8 mm^3/s",
    "min_layer_time": "8 s",
    "cooldown_bed_on": "120 s",
    "cooldown_bed_off": "300 s",
    "thermal_substeps": 4,
    "provenance": "user",
}


def setup_wall(c, tmp, ws):
    """a 24 x 6 x 6 mm PLA wall on the plate, meshed at 2 mm; returns the project directory"""
    stl = tmp / "wall.stl"
    box_stl(stl, (0, -3, 0), (24, 3, 6))
    v = call_ok(c, "project_create", {"name": "print_wall", "description": "a small printed wall"})
    call_ok(c, "geometry_import", {"path": str(stl), "units": "mm", "name": "wall"})
    call_ok(c, "material_assign", {"body": "wall", "material": "pla_generic_demo", "source": "user"})
    call_ok(c, "mesh_generate", {"element_size": "2 mm"})
    return (v.get("project") or {}).get("directory", "")


def main():
    if not MCP.exists():
        print("navier-mcp not built (run: make)")
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="nvprintflow"))
    ws = tmp / "ws"
    common = ["--workspace", str(ws), "--allow-read", str(tmp)]
    c = Client(["--embedded"] + common)
    c.initialize()
    project_dir = None
    try:
        tools = {t["name"] for t in c.request("tools/list")["result"]["tools"]}
        check("mech_print_run" in tools, "mech_print_run is listed as a tool")

        print("== refusals before any solve")
        project_dir = setup_wall(c, tmp, ws)
        err = call_err(c, "mech_print_run", {"process": {k: v for k, v in PROCESS.items() if k != "provenance"}}, "INVALID_PARAMS",
                       "a process without provenance is refused by the schema")
        check("provenance" in json.dumps(err).lower(), "the refusal names provenance")
        err = call_err(c, "mech_print_run", {"process": {k: v for k, v in PROCESS.items() if k != "nozzle_temperature"}}, "INVALID_PARAMS",
                       "a process without the nozzle temperature is refused")
        check("nozzle_temperature" in json.dumps(err.get("details", {})), "the refusal lists the missing input")
        call_err(c, "mech_print_run", {"process": dict(PROCESS, deposition_rate="8 mm/s")}, "INVALID_UNIT", "a rate that is not a volume per time is refused")
        v = call_ok(c, "mech_print_run", {"process": dict(PROCESS, deposition_rate="480 mm3/min"), "label": "unit spellings", "allow_duplicate": True},
                    "a rate written as mm3/min is accepted")
        call_ok(c, "job_cancel", {"job_id": v.get("job_id", "")}, "cancel the unit-spelling print")

        print("== a part that does not rest on the build plate")
        call_ok(c, "geometry_place", {"body": "wall", "z_offset": "5 mm"})
        call_ok(c, "mesh_generate", {"element_size": "2 mm"})
        err = call_err(c, "mech_print_run", {"process": PROCESS}, "PRECONDITION_FAILED", "a lifted part is refused")
        check("plate" in json.dumps(err).lower(), "the refusal says the part is not on the plate")
        call_ok(c, "geometry_place", {"body": "wall", "z_offset": "0 mm"})
        call_ok(c, "mesh_generate", {"element_size": "2 mm"})

        print("== a stale mesh")
        call_ok(c, "geometry_place", {"body": "wall", "rotate_z": 90})
        err = call_err(c, "mech_print_run", {"process": PROCESS}, "PRECONDITION_FAILED", "a stale mesh is refused")
        check("stale" in json.dumps(err).lower() and "mesh_generate" in json.dumps(err), "the refusal says the mesh is stale and how to fix it")
        call_ok(c, "geometry_place", {"body": "wall", "rotate_z": 0})
        call_ok(c, "mesh_generate", {"element_size": "2 mm"})

        print("== a material whose provenance is a default")
        call_ok(c, "material_assign", {"body": "wall", "material": "pla_generic_demo", "source": "default"})
        err = call_err(c, "mech_print_run", {"process": PROCESS}, "PRECONDITION_FAILED", "a defaulted material is refused")
        check("provenance" in json.dumps(err).lower() or "default" in json.dumps(err).lower(), "the refusal names the material's provenance")
        call_ok(c, "material_assign", {"body": "wall", "material": "pla_generic_demo", "source": "user"})

        print("== the print")
        probes = [{"label": "wall middle", "at": ["12 mm", "0 mm", "3 mm"]}, {"label": "wall top", "at": ["12 mm", "0 mm", "6 mm"]}]
        v = call_ok(c, "mech_print_run", {"process": PROCESS, "probes": probes, "label": "wall print"}, "mech_print_run")
        job = v.get("job_id", "")
        run_dir = v.get("run_directory", "")
        check(v.get("analysis") == "fff_print", "the job is an fff_print analysis")
        check(bool(job) and bool(run_dir), "the print returns a job id and a run directory")
        # Thermal process strains can create stress without external mechanical loads.
        load_warnings = [w.get("message", "").lower() for w in v.get("warnings", []) if w.get("code") == "NO_LOADS"]
        check(bool(load_warnings) and all("without additional thermal or eigenstrain loading" in w for w in load_warnings),
              "the absent-mechanical-load warning qualifies its zero-state claim for process loading")
        spec_hash = v.get("spec_hash", "")
        dup = call_ok(c, "mech_print_run", {"process": PROCESS, "probes": probes}, "identical print")
        check(dup.get("deduplicated") is True and dup.get("job_id") == job, "an identical print returns the running job instead of a second one")
        status = wait_job(c, job)
        check(status.get("state") == "succeeded", f"the print job succeeded (state {status.get('state')}, error {json.dumps(status.get('error'))[:300]})")

        spec = json.loads((Path(run_dir) / "spec.json").read_text())
        check(spec.get("spec_hash") == spec_hash, "the run directory holds the specification with its hash")
        check(spec.get("spec_hash_covers", "").startswith("every field except"), "the specification says what the hash covers")

        summary = status.get("summary") or {}
        scope = summary.get("scope") or {}
        print(f"   stored times {summary.get('results', {}).get('stored_times')}, "
              f"peak on bed {summary.get('results', {}).get('peak_von_mises_on_bed_mpa', float('nan')):.3f} MPa, "
              f"released {summary.get('results', {}).get('peak_von_mises_released_mpa', float('nan')):.3f} MPa, "
              f"warp {summary.get('results', {}).get('warp_z_min_mm', float('nan')):.4f} to {summary.get('results', {}).get('warp_z_max_mm', float('nan')):.4f} mm")
        check(scope.get("is_forecast") is False, "the summary says the result is not a forecast")
        check(scope.get("compared_with_measurement") is False, "the summary says nothing was compared with a measurement")
        # Model-scope acceptance, declared before this correction's first run:
        # neglecting creep does not establish a local stress bound when stiffness
        # and strain redistribute through a heterogeneous printing temperature field.
        check(scope.get("bed_stresses_are_upper_bound") is False,
              "the summary does not assert an unproved bound on local bed stresses")
        check("no general local stress bound" in scope.get("bed_stresses_note", ""),
              "the summary explains why the approximation supplies no stress bound")
        check(scope.get("creep_below_relaxation_temperature") == "not modelled", "the summary says creep is not modelled")
        law = scope.get("constitutive_law", "")
        check("hypoelastic" in law and "no time-dependent viscoelastic law" in law,
              "the summary identifies the implemented incremental stress law and its polymer relaxation limit")
        check((scope.get("material") or {}).get("status") == "demonstration" and (scope.get("material") or {}).get("measured") is False,
              "the summary carries the material's own status")
        lump = scope.get("layer_lumping") or {}
        check(abs(lump.get("printed_layers_per_simulation_layer", 0) - 5.0) < 1e-9, "the summary states how many printed layers a simulation layer lumps")

        res = summary.get("results") or {}
        dep = summary.get("deposition") or {}
        check(dep.get("elements_never_deposited") == 0, "every element of the wall is printable")
        check(res.get("worst_heat_balance_relative", 1) <= 1e-6, f"every thermal step conserves energy ({res.get('worst_heat_balance_relative')})")
        # Criterion before first run: the independently reconstructed full-print
        # heat ledger must close within 1e-6, including physical deposited enthalpy.
        heat_keys = ("supplied_nozzle_enthalpy_above_ambient_j", "stored_enthalpy_above_ambient_j",
                     "heat_into_bed_j", "heat_into_air_j", "enthalpy_removed_with_supports_j")
        heat = [res.get(key, float('nan')) for key in heat_keys]
        check(all(math.isfinite(v) for v in heat), "the native summary exports every term of the physical heat ledger")
        supplied, stored_heat, bed_heat, air_heat, removed_heat = heat
        closure = abs(stored_heat + bed_heat + air_heat + removed_heat - supplied) / max(abs(v) for v in heat)
        check(closure <= 1e-6 and res.get("whole_print_heat_balance_relative", 1) <= 1e-6,
              f"the full print conserves deposited nozzle enthalpy (independent relative error {closure:.2e})")
        check(res.get("deposition_enthalpy_correction_j", 0) > 0 and res.get("worst_deposition_balance_relative", 1) <= 1e-6,
              "shared-node deposition correction is accounted for and conserves heat")
        check("eight Gauss-point von Mises" in res.get("stress_field_definition", ""), "the summary states the actual stress scalar definition")
        eq_last = res.get("equilibrium_error_last_solve", float('nan'))
        eq_release = res.get("equilibrium_error_at_release", float('nan'))
        check(math.isfinite(eq_last) and math.isfinite(eq_release) and 0 <= eq_last < 1e-6 and 0 <= eq_release < 1e-6,
              f"F17: last-solve and release normalized free-equation residuals close ({eq_last:.2e}, {eq_release:.2e})")
        definition = res.get("equilibrium_error_definition", "")
        check("free-equation residual" in definition and "individual" in definition and "RHS" in definition,
              "F17: the equilibrium definition states individual reactions and the eigenstrain RHS force scale")
        print(f"   nozzle enthalpy {supplied:.9g} J, shared-node correction {res.get('deposition_enthalpy_correction_j'):.9g} J, "
              f"whole-print closure {closure:.2e}")
        bed = abs(res.get("largest_bed_reaction_n", 0.0))
        rel = abs(res.get("support_reaction_after_release_n", 1.0))
        check(rel <= 1e-6 * (1.0 + bed), f"the released part has a small absolute support reaction ({rel:.2e} N; net bed resultant {bed:.2e} N)")
        print(f"   worst heat balance {res.get('worst_heat_balance_relative'):.2e}, support reaction after release {rel:.2e} N, "
              f"net bed resultant {bed:.2e} N; normalized equilibrium last {eq_last:.2e}, release {eq_release:.2e}")
        stored = res.get("stored_times", 0)
        laid = dep.get("layers_deposited", 0)
        check(laid > 0 and stored == 2 * laid + 3,
              f"key_times stores two times per deposited layer plus two cool-downs and the release ({stored} for {laid} layers)")
        probes_out = summary.get("probes") or []
        check(len(probes_out) == 2 and len(probes_out[0].get("temperature_c", [])) > 4, "the probe histories are in the summary")
        hot = [t for t in probes_out[0].get("temperature_c", []) if t is not None]
        check(hot and max(hot) > 100, f"the probe sees the hot material land on it (max {max(hot) if hot else float('nan'):.0f} C)")

        print("== the print through the ordinary result operations")
        err = call_err(c, "results_quantities", {"job_id": job}, "UNSUPPORTED", "results_quantities is for static results")
        check("results_query" in json.dumps(err), "the refusal points at the result operations a print answers to")
        last = call_ok(c, "results_query", {"job_id": job, "quantity": "von_mises"}, "results_query at the last stored time")
        first = call_ok(c, "results_query", {"job_id": job, "quantity": "von_mises", "time_index": 0}, "results_query at the first stored time")
        check(last.get("time_index") == stored - 1 or last.get("time_index") is None, "the default stored time is the last one (the released part)")
        released_peak = (last.get("statistics") or {}).get("max")
        check(released_peak is not None, f"the query returns a peak ({json.dumps(last)[:200]})")
        check(first.get("time_index") == 0 and (first.get("statistics") or {}).get("max") is not None, "the first stored time can be queried too")
        temp = call_ok(c, "results_query", {"job_id": job, "quantity": "temperature", "time_index": 0}, "temperature at the first stored time")
        tmax = (temp.get("statistics") or {}).get("max")
        check(tmax is not None and tmax > 100, f"the first stored time is hot ({tmax} {temp.get('unit')})")
        pr = call_ok(c, "results_probe", {"job_id": job, "points_mm": [[12, 0, 1]], "history": True}, "results_probe with a history")
        check(len(json.dumps(pr)) > 50, "the probe returns values over the stored times")
        img = call_ok(c, "results_render", {"job_id": job, "quantity": "temperature", "time_index": 1, "width": 320, "height": 240}, "results_render of a stored time")
        check(json.dumps(img).find("png") >= 0 or img.get("image") or img.get("path"), f"the render returns an image ({json.dumps(img)[:200]})")
        exp = call_ok(c, "results_export", {"job_id": job, "formats": ["csv"], "directory": str(ws / "export")}, "results_export csv")
        files = json.dumps(exp)
        check("csv" in files and any(Path(ws / "export").glob("*.csv")), f"the export writes a csv of the stored times ({files[:200]})")

        print("== a second session reads the same print from its run directory")
        c2 = Client(["--embedded"] + common)
        c2.initialize()
        try:
            call_ok(c2, "project_open", {"path": project_dir} if project_dir else {"name": "print_wall"}, "project_open in a second session")
            again = call_ok(c2, "results_query", {"job_id": job, "quantity": "von_mises"}, "results_query in the second session")
            a = (again.get("statistics") or {}).get("max")
            check(a is not None and released_peak is not None and abs(a - released_peak) <= 1e-9 * max(1.0, abs(released_peak)),
                  f"the reopened run gives the same peak von Mises ({a} against {released_peak})")
            t2 = call_ok(c2, "results_query", {"job_id": job, "quantity": "temperature", "time_index": 0}, "temperature in the second session")
            b = (t2.get("statistics") or {}).get("max")
            check(b is not None and abs(b - tmax) <= 1e-9 * max(1.0, abs(tmax)), f"the reopened run gives the same temperatures ({b} against {tmax})")
        finally:
            c2.close()

        print("== storing every thermal substep instead of the key times")
        v = call_ok(c, "mech_print_run", {"process": PROCESS, "store": "substeps", "label": "substeps"}, "print with store substeps")
        job4 = v.get("job_id", "")
        st4 = wait_job(c, job4)
        check(st4.get("state") == "succeeded", f"the substep print succeeded ({st4.get('state')})")
        r4 = (st4.get("summary") or {}).get("results") or {}
        d4 = (st4.get("summary") or {}).get("deposition") or {}
        expect = PROCESS["thermal_substeps"] * (d4.get("layers_deposited", 0) + 2) + 1  # layers and both cool-down phases, then the release 
        check(r4.get("stored_times") == expect,
              f"substeps stores one time per thermal substep of every layer and of both cool-downs, plus the release "
              f"({r4.get('stored_times')} for {d4.get('layers_deposited')} layers of {PROCESS['thermal_substeps']} substeps, expected {expect})")
        check(r4.get("stored_times", 0) > stored, "storing substeps keeps more times than the key times")
        a4 = r4.get("peak_von_mises_on_bed_mpa")
        print(f"   substeps: {r4.get('stored_times')} stored times, peak on bed {a4:.3f} MPa against {res.get('peak_von_mises_on_bed_mpa'):.3f} MPa at the key times")

        print("== the iterative solver, chosen explicitly")
        v = call_ok(c, "mech_print_run", {"process": PROCESS, "numerics": {"solver": "iterative", "tolerance": 1e-8, "skip_increment_below": "1 K"},
                                          "label": "iterative"}, "print with explicit numerics")
        job3 = v.get("job_id", "")
        check((v.get("model") or {}).get("numerics", {}).get("solver") == "iterative", "the model records the solver that was chosen")
        st3 = wait_job(c, job3)
        check(st3.get("state") == "succeeded", f"the iterative print succeeded ({st3.get('state')})")
        s3 = (st3.get("summary") or {}).get("results") or {}
        a, b = s3.get("peak_von_mises_on_bed_mpa"), res.get("peak_von_mises_on_bed_mpa")
        check(a is not None and abs(a - b) <= 1e-6 * abs(b), f"the iterative solver gives the same peak von Mises within 1e-6 ({a} against {b})")

        print("== cancelling a print")
        v = call_ok(c, "mech_print_run", {"process": dict(PROCESS, layer_height="0.5 mm"), "label": "to cancel"}, "second print to cancel")
        job2 = v.get("job_id", "")
        call_ok(c, "job_cancel", {"job_id": job2}, "job_cancel")
        st2 = wait_job(c, job2)
        check(st2.get("state") in ("cancelled", "succeeded"), f"a cancelled print ends cancelled (state {st2.get('state')})")
    finally:
        code, stderr = c.close()
        if FAIL:
            print(stderr[-3000:])
        shutil.rmtree(tmp, ignore_errors=True)
    if FAIL:
        print(f"\nPRINT FLOW TESTS FAILED: {PASS} passed, {FAIL} failed")
        return 1
    print(f"\nALL PRINT FLOW TESTS PASSED: {PASS} passed, 0 failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
