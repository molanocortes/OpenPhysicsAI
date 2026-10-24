#!/usr/bin/env python3
"""make_three_domain.py - the fluid-thermal-solid demonstration study, driven over MCP exactly as an AI host would drive it.

A steel plate heated by 0.1 W sits under a 10 mm air channel. The air flow (lattice Boltzmann, steady, laminar) carries the
plate's heat away, the plate and the air exchange heat through their resolved interface (no convection coefficient), and
the plate expands thermally (Solid Sim, one-way). The script leaves behind, in "Thermal Sim/examples/04_three_domain_cht":

  workspace/plate_in_air/project.json     the saved project
  workspace/plate_in_air/runs/<job>/spec.json, summary.json, results.nvt   per run: hashed specification, budgets, fields
  export/*.vtu, *.pvd, *.csv               temperature, air velocity and displacement over time, histories
  study.json                               every run's settings, cost and outcomes
  README.md                                what the study shows, its convergence evidence and what it does not model

Runs (all through MCP):
  reference   partitioned, adaptive (1e-3 / 0.05 K), 2 mm cells, paused at about half time and resumed from its checkpoint
  temporal    the same at tolerances 1e-4 / 0.005 K and 1e-5 / 0.0005 K (monolithic: identical to partitioned within the
              coupling tolerance, and cheaper)
  spatial     2 mm, 1 mm and 0.5 mm cells at 1e-4 / 0.005 K (monolithic), and again at one fixed 60 s step (--spatial-fixed)
  cancellation  the fixed-step study's meshes at 30 s, the 2 mm and 1 mm meshes at 15 s (--temporal-check): the temporal
              order, how much the temporal error depends on the mesh, and the spatial orders on time-extrapolated values

Run from the repository root after `make am` (about an hour on an 8 GB M2; the 0.5 mm levels dominate):
    python3 "Thermal Sim/examples/make_three_domain.py"
    python3 "Thermal Sim/examples/make_three_domain.py" --report-only   (re-export and rebuild study.json and README.md from
                                                                          the finished runs, without solving again)
    python3 "Thermal Sim/examples/make_three_domain.py" --spatial-fixed --temporal-check   (only the fixed-step mesh study and
                                                                          its cancellation check, on the saved project)
"""
import json
import math
import shutil
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402

OUT = Path(__file__).resolve().parent / "04_three_domain_cht"
PROBLEMS = []


def problem(msg):
    PROBLEMS.append(msg)
    print(f"  PROBLEM: {msg}")


def call(c, name, args):
    r = c.call(name, args)
    res = r.get("result", {}) or {}
    sc = res.get("structuredContent") or {}
    if res.get("isError") or not sc.get("ok"):
        problem(f"{name}: {json.dumps(sc.get('error'))[:400]}")
        return {}
    return sc.get("value", {})


def box_stl(path, lx, ly, lz, label):
    p = [(0, 0, 0), (lx, 0, 0), (lx, ly, 0), (0, ly, 0), (0, 0, lz), (lx, 0, lz), (lx, ly, lz), (0, ly, lz)]
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


def status(c, job_id):
    r = c.call("job_status", {"job_id": job_id})
    return (r.get("result", {}).get("structuredContent") or {}).get("value", {})


def wait(c, job_id, states=("succeeded", "failed", "cancelled", "paused")):
    while True:
        st = status(c, job_id)
        if st.get("state") in states:
            return st
        time.sleep(0.2)


PLATE_Q = 0.1 / (40e-3 * 20e-3 * 4e-3)
BASE = {"analysis": "conjugate_heat_transfer", "end_time": "7200 s", "time_stepping": "adaptive", "initial_temperature": "20 degC",
        "output_interval": "300 s", "flow": {"fluid_body": "air", "inlet_velocity": "0.02 m/s", "inlet_temperature": "20 degC",
                                             "walls": {"y_min": "periodic", "y_max": "periodic", "z_min": "no_slip", "z_max": "no_slip"}},
        "structural_response": True, "stress_free_temperature": "20 degC", "checkpoint_wall_interval": 0, "allow_duplicate": True}
TOL = {"1e-3": {"temporal_relative_tolerance": 1e-3, "temporal_temperature_tolerance": "0.05 K"},
       "1e-4": {"temporal_relative_tolerance": 1e-4, "temporal_temperature_tolerance": "0.005 K"},
       "1e-5": {"temporal_relative_tolerance": 1e-5, "temporal_temperature_tolerance": "0.0005 K"}}


def outcomes(c, job_id, summary):
    peak = call(c, "results_query", {"job_id": job_id, "quantity": "temperature"}).get("statistics", {}).get("max")
    outlet = call(c, "results_query", {"job_id": job_id, "quantity": "temperature", "selection": "outlet"}).get("statistics", {}).get("mean")
    return summary_outcomes(summary, peak, outlet)


def summary_outcomes(summary, peak, outlet):
    en, ci, ti = summary.get("energy", {}), summary.get("conjugate_interface", {}), summary.get("time_integration", {})
    src = en.get("sources_j", 0)
    iface = ci.get("heat_into_fluid_j")
    if iface is None:  # monolithic: not a separate measurement; the air body's balance residual is the heat it received
        air = [b for b in summary.get("bodies", []) if b.get("name") == "air"]
        iface = air[0].get("residual_j") if air else None
    return {"plate_peak_c": peak, "outlet_face_mean_c": outlet, "interface_heat_j": iface,
            "interface_heat_source": "measured (partitioned reactions)" if ci.get("heat_into_fluid_j") is not None else "air body balance residual (monolithic)",
            "stored_j": en.get("stored_j"), "enthalpy_out_minus_in_j": en.get("enthalpy_out_j", 0) - en.get("enthalpy_in_j", 0),
            "discrete_closure": en.get("closure_error"), "enthalpy_flux_closure": abs(src - en.get("stored_j", 0) - (en.get("enthalpy_out_j", 0) - en.get("enthalpy_in_j", 0))) / max(src, 1e-300),
            "unexplained_advective_j": en.get("unexplained_advective_j"), "accepted_steps": ti.get("accepted_steps"),
            "rejected_steps": ti.get("rejected_steps"), "solves": ti.get("work", {}).get("solves"),
            "coupling_iterations": summary.get("coupling", {}).get("statistics", {}).get("coupling_iterations"),
            "flow": {k: summary.get("flow", {}).get(k) for k in ("relaxation_time", "lattice_mach", "reynolds_cell", "reynolds_hydraulic", "lattice_steps",
                                                                 "physical_time_to_steady_s", "density_min", "density_max", "section_scale_min",
                                                                 "section_scale_max", "wall_seconds")},
            "max_displacement_mm": max((r.get("max_displacement_mm", 0) for r in summary.get("history", [])), default=None)}


def run(c, label, args):
    t0 = time.time()
    v = call(c, "analysis_run", dict(args, label=label))
    if not v.get("job_id"):
        return None
    st = wait(c, v["job_id"])
    wall = time.time() - t0
    if st.get("state") != "succeeded":
        problem(f"{label}: {st.get('state')} {json.dumps(st.get('error'))[:300]}")
        return None
    o = outcomes(c, v["job_id"], st.get("summary", {}))
    o.update({"label": label, "job_id": v["job_id"], "wall_s": wall, "spec_hash": v.get("spec_hash")})
    print(f"  {label}: peak {o['plate_peak_c']:.4f} degC, outlet {o['outlet_face_mean_c']:.4f} degC, interface {o['interface_heat_j']} J, "
          f"{o['accepted_steps']} steps, {wall:.1f} s")
    return o


def richardson(values, ratio=2.0):
    """observed order and extrapolated value from three levels (coarse, medium, fine). No extrapolation is offered unless
    the sequence is monotone with an order in [0.5, 4]: outside that range the levels are not in the asymptotic range and
    an extrapolated number would only look like evidence."""
    f1, f2, f3 = values
    d1, d2 = f1 - f2, f2 - f3
    if d1 == 0 or d2 == 0 or d1 * d2 < 0:
        return None, None, "not monotone: no order can be estimated"
    p = math.log(abs(d1 / d2)) / math.log(ratio)
    if not 0.5 <= p <= 4:
        return p, None, f"observed order {p:.2f} is outside [0.5, 4]: not in the asymptotic range, no extrapolation"
    return p, f3 + (f3 - f2) / (ratio ** p - 1), "monotone, asymptotic range plausible"


def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / "geometry").mkdir(parents=True)
    box_stl(OUT / "geometry" / "plate.stl", 40, 20, 4, b"three-domain study: steel plate")
    box_stl(OUT / "geometry" / "air.stl", 60, 20, 10, b"three-domain study: air channel")
    ws = OUT / "workspace"
    c = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(OUT), "--allow-write", str(OUT)])
    c.initialize()
    print("== setup through MCP")
    call(c, "project_create", {"name": "plate_in_air", "description": "steel plate heated by 0.1 W under a 10 mm air channel (three-domain demonstration)"})
    call(c, "geometry_import", {"path": str(OUT / "geometry" / "plate.stl"), "units": "mm", "name": "plate"})
    call(c, "geometry_import", {"path": str(OUT / "geometry" / "air.stl"), "units": "mm", "name": "air"})
    call(c, "geometry_place", {"body": "plate", "position": [0, 0], "z_offset": 0})
    call(c, "geometry_place", {"body": "air", "position": [0, 0], "z_offset": 4})
    call(c, "material_assign", {"body": "plate", "material": "steel_plate_demo", "source": "user", "note": "demonstration steel"})
    call(c, "material_assign", {"body": "air", "material": "air_demo", "source": "user", "note": "demonstration air, constant properties"})
    call(c, "mesh_generate", {"element_size": "2 mm"})
    call(c, "selection_create", {"name": "plate_end", "query": {"plane": {"axis": "x", "at": "min"}}, "body": "plate"})
    call(c, "selection_create", {"name": "outlet", "query": {"plane": {"axis": "x", "at": "max"}}, "body": "air"})
    call(c, "boundary_apply", {"name": "heater", "kind": "heat_source", "body": "plate", "power_density": f"{PLATE_Q} W/m^3", "source": "user",
                               "description": "0.1 W dissipated uniformly in the plate"})
    call(c, "boundary_apply", {"name": "held", "kind": "fixed", "selection": "plate_end", "source": "user", "description": "plate clamped at its upstream end"})
    val = call(c, "setup_validate", {k: v for k, v in dict(BASE, **TOL["1e-3"]).items() if k != "allow_duplicate"})
    print(f"  ready: {val.get('ready')}; warnings: {sorted({w.get('code') for w in val.get('warnings', [])})}")
    call(c, "project_save", {})
    study = {"setup": val.get("model"), "runs": {}}

    print("== reference: partitioned, paused and resumed")
    t0 = time.time()
    v = call(c, "analysis_run", dict(BASE, **TOL["1e-3"], label="reference"))
    ref_id = v.get("job_id")
    while True:
        st = status(c, ref_id)
        if st.get("progress", 0) >= 0.5 or st.get("state") not in ("running", "queued"):
            break
        time.sleep(0.01)
    call(c, "job_pause", {"job_id": ref_id})
    st = wait(c, ref_id)
    ck = st.get("checkpoint", {})
    print(f"  paused: {st.get('state')} at t = {ck.get('time_s')} s (checkpoint {ck.get('sequence')})")
    call(c, "job_resume", {"job_id": ref_id})
    st = wait(c, ref_id)
    if st.get("state") != "succeeded":
        problem(f"reference: {st.get('state')}")
    ref = outcomes(c, ref_id, st.get("summary", {}))
    ref.update({"label": "reference (partitioned, paused and resumed)", "job_id": ref_id, "wall_s": time.time() - t0,
                "paused_at_s": ck.get("time_s"), "resumed_from": st.get("summary", {}).get("restart", {}).get("resumed_from_s")})
    study["runs"]["reference"] = ref
    study["reference_summary"] = st.get("summary")
    print(f"  reference: peak {ref['plate_peak_c']:.4f} degC, interface {ref['interface_heat_j']:.4f} J, {ref['accepted_steps']} steps, "
          f"displacement {ref['max_displacement_mm']:.5f} mm")
    exp = call(c, "results_export", {"job_id": ref_id, "formats": ["vtu", "csv"], "directory": str(OUT / "export")})
    study["export"] = exp
    probe = call(c, "results_probe", {"job_id": ref_id, "points_mm": [[0, 0, 2], [19, 0, 2], [29, 0, 9]], "history": True})
    study["probes"] = probe.get("probes")

    print("== partitioned against monolithic at the reference settings")
    mono = run(c, "monolithic 2 mm 1e-3", dict(BASE, **TOL["1e-3"], coupling={"scheme": "monolithic"}))
    study["runs"]["monolithic_2mm_1e-3"] = mono

    print("== temporal study (2 mm, monolithic)")
    for key in ("1e-4", "1e-5"):
        study["runs"][f"monolithic_2mm_{key}"] = run(c, f"monolithic 2 mm {key}", dict(BASE, **TOL[key], coupling={"scheme": "monolithic"}))

    print("== spatial study (tolerance 1e-4, monolithic)")
    levels = {}
    for h in ("2 mm", "1 mm", "0.5 mm"):
        if h != "2 mm":
            call(c, "mesh_generate", {"element_size": h})
            key = f"monolithic_{h.replace(' ', '')}_1e-4"
            study["runs"][key] = run(c, f"monolithic {h} 1e-4", dict(BASE, **TOL["1e-4"], coupling={"scheme": "monolithic"}))
        else:
            key = "monolithic_2mm_1e-4"
        levels[h] = study["runs"][key]
    call(c, "mesh_generate", {"element_size": "2 mm"})
    call(c, "project_save", {})
    c.close()
    finish(study)
    print(f"\n{'STUDY FINISHED WITH PROBLEMS' if PROBLEMS else 'STUDY FINISHED'}: {len(PROBLEMS)} problem(s); artefacts in {OUT}")
    return 1 if PROBLEMS else 0


def report_only():
    """re-export the reference run and rebuild study.json and README.md from the finished runs' summaries"""
    study = json.loads((OUT / "study.json").read_text())
    runs_dir = OUT / "workspace" / "plate_in_air" / "runs"
    for key, r in study["runs"].items():
        if not r:
            continue
        sm = json.loads((runs_dir / r["job_id"] / "summary.json").read_text())
        keep = {k: r[k] for k in ("label", "job_id", "wall_s", "spec_hash", "paused_at_s", "resumed_from") if k in r}
        r.clear()
        r.update(summary_outcomes(sm, None, None))  # the field extremes are queried below
        r.update(keep)
    c = Client(["--embedded", "--workspace", str(OUT / "workspace"), "--allow-read", str(OUT), "--allow-write", str(OUT)])
    c.initialize()
    call(c, "project_open", {"path": str(OUT / "workspace" / "plate_in_air")})
    for key, r in study["runs"].items():
        if r:
            r["plate_peak_c"] = call(c, "results_query", {"job_id": r["job_id"], "quantity": "temperature"}).get("statistics", {}).get("max")
            r["outlet_face_mean_c"] = call(c, "results_query", {"job_id": r["job_id"], "quantity": "temperature", "selection": "outlet"}).get("statistics", {}).get("mean")
    ref_id = study["runs"]["reference"]["job_id"]
    export = OUT / "export"
    if export.exists():
        shutil.rmtree(export)
    study["export"] = call(c, "results_export", {"job_id": ref_id, "formats": ["vtu", "csv"], "directory": str(export)})
    study["probes"] = call(c, "results_probe", {"job_id": ref_id, "points_mm": [[0, 0, 2], [19, 0, 2], [29, 0, 9]], "history": True}).get("probes")
    c.close()
    finish(study)
    print(f"\n{'REPORT FINISHED WITH PROBLEMS' if PROBLEMS else 'REPORT REBUILT'}: {len(PROBLEMS)} problem(s)")
    return 1 if PROBLEMS else 0


def convergence(study, keys):
    levels = {h: study["runs"].get(k) for h, k in zip(("2 mm", "1 mm", "0.5 mm"), keys)}
    conv = {}
    for q in ("plate_peak_c", "outlet_face_mean_c", "interface_heat_j", "max_displacement_mm"):
        vals = [levels[h][q] if levels[h] else None for h in ("2 mm", "1 mm", "0.5 mm")]
        if None in vals:
            conv[q] = {"values_2mm_1mm_05mm": vals, "note": "a level is missing"}
            continue
        p, ext, note = richardson(vals)
        conv[q] = {"values_2mm_1mm_05mm": vals, "observed_order": p, "extrapolated": ext, "note": note,
                   "fine_minus_extrapolated": (vals[2] - ext) if ext is not None else None}
    # the enthalpy-flux closure has the exact limit 0: orders from successive ratios
    vals = [levels[h]["enthalpy_flux_closure"] if levels[h] else None for h in ("2 mm", "1 mm", "0.5 mm")]
    if None not in vals and all(v > 0 for v in vals):
        conv["enthalpy_flux_closure"] = {"values_2mm_1mm_05mm": vals, "orders_from_ratios": [math.log2(vals[0] / vals[1]), math.log2(vals[1] / vals[2])],
                                         "note": "exact limit 0"}
    return conv


def finish(study):
    study["spatial_convergence_adaptive"] = convergence(study, ("monolithic_2mm_1e-4", "monolithic_1mm_1e-4", "monolithic_0.5mm_1e-4"))
    if study["runs"].get("fixed60_2mm"):
        study["spatial_convergence"] = convergence(study, ("fixed60_2mm", "fixed60_1mm", "fixed60_0.5mm"))
    study.pop("spatial_convergence_note", None)
    tc = temporal_cancellation(study)
    if tc:
        study["temporal_cancellation"] = tc
    tvals = [study["runs"].get(k) for k in ("monolithic_2mm_1e-3", "monolithic_2mm_1e-4", "monolithic_2mm_1e-5")]
    study["temporal_convergence"] = {q: [r[q] if r else None for r in tvals] for q in ("plate_peak_c", "outlet_face_mean_c", "interface_heat_j")}
    (OUT / "study.json").write_text(json.dumps(study, indent=2, sort_keys=True, default=str))
    write_readme(study)


def temporal_check():
    """whether the temporal error of the fixed-step spatial study cancels between meshes. The same meshes at half the step
    (30 s), and the 2 mm and 1 mm meshes at a quarter (15 s): the measured temporal order justifies extrapolating each mesh
    in time, and T(30 s) - T(60 s) shows how much the temporal error depends on the mesh. Fields are stored hourly to save
    disk; the step sequence is unchanged, so the solution is too (checked bitwise on 2 mm at 60 s)."""
    study = json.loads((OUT / "study.json").read_text())
    c = Client(["--embedded", "--workspace", str(OUT / "workspace"), "--allow-read", str(OUT), "--allow-write", str(OUT)])
    c.initialize()
    call(c, "project_open", {"path": str(OUT / "workspace" / "plate_in_air")})
    base = {k: v for k, v in BASE.items() if k != "time_stepping"}
    base.update(time_stepping="fixed", output_interval="3600 s", coupling={"scheme": "monolithic"})
    print("== temporal cancellation check (monolithic, fixed steps, fields stored hourly)")
    for h, steps in (("2 mm", ("60 s", "30 s", "15 s")), ("1 mm", ("30 s", "15 s")), ("0.5 mm", ("30 s",))):
        call(c, "mesh_generate", {"element_size": h})
        for dt in steps:
            key = f"fixed{dt.split()[0]}_{h.replace(' ', '')}" + ("_hourly" if dt == "60 s" else "")
            study["runs"][key] = run(c, f"monolithic {h} fixed {dt} (hourly fields)", dict(base, time_step=dt))
    call(c, "mesh_generate", {"element_size": "2 mm"})
    call(c, "project_save", {})
    c.close()
    finish(study)
    print(f"\n{'TEMPORAL CHECK FINISHED WITH PROBLEMS' if PROBLEMS else 'TEMPORAL CHECK FINISHED'}: {len(PROBLEMS)} problem(s)")
    return 1 if PROBLEMS else 0


QUANTITIES = ("plate_peak_c", "outlet_face_mean_c", "interface_heat_j", "max_displacement_mm")


def temporal_cancellation(study):
    """the evidence behind the fixed-step spatial study, from the runs of temporal_check()"""
    runs = study["runs"]
    if not all(runs.get(k) for k in ("fixed30_2mm", "fixed30_1mm", "fixed30_0.5mm", "fixed15_2mm", "fixed15_1mm", "fixed60_2mm_hourly")):
        return None
    out = {"output_interval_invariance": {q: runs["fixed60_2mm_hourly"][q] - runs["fixed60_2mm"][q] for q in QUANTITIES},
           "temporal_order": {}, "t30_minus_t60": {}, "spatial_convergence_30s": convergence(study, ("fixed30_2mm", "fixed30_1mm", "fixed30_0.5mm"))}
    for h in ("2mm", "1mm"):
        # order from 60 / 30 / 15 s, and the step-halving differences it rests on
        out["temporal_order"][h] = {q: richardson([runs[f"fixed{s}_{h}"][q] for s in (60, 30, 15)])[0] for q in QUANTITIES}
    for h in ("2mm", "1mm", "0.5mm"):
        out["t30_minus_t60"][h] = {q: runs[f"fixed30_{h}"][q] - runs[f"fixed60_{h}"][q] for q in QUANTITIES}
    # the 60 s error of a mesh is -2 [T(30 s) - T(60 s)]; what does not cancel in a mesh difference is its change between the
    # two meshes, given as a fraction of that difference (2 -> 1 mm, 1 -> 0.5 mm)
    out["contamination_of_mesh_differences"] = {}
    for q in QUANTITIES:
        d = [out["t30_minus_t60"][h][q] for h in ("2mm", "1mm", "0.5mm")]
        t60 = [runs[f"fixed60_{h}"][q] for h in ("2mm", "1mm", "0.5mm")]
        out["contamination_of_mesh_differences"][q] = [2 * abs(d[i] - d[i + 1]) / abs(t60[i] - t60[i + 1]) if t60[i] != t60[i + 1] else None
                                                       for i in (0, 1)]
    # backward Euler is first order (confirmed by temporal_order): 2 T(30 s) - T(60 s) removes the leading temporal error of
    # each mesh; the spatial study is then repeated on those values
    ext = {h: {q: 2 * runs[f"fixed30_{h}"][q] - runs[f"fixed60_{h}"][q] for q in QUANTITIES} for h in ("2mm", "1mm", "0.5mm")}
    out["time_extrapolated_values"] = ext
    conv = {}
    for q in QUANTITIES:
        vals = [ext[h][q] for h in ("2mm", "1mm", "0.5mm")]
        p, e, note = richardson(vals)
        conv[q] = {"values_2mm_1mm_05mm": vals, "observed_order": p, "extrapolated": e, "note": note,
                   "fine_minus_extrapolated": (vals[2] - e) if e is not None else None}
    out["spatial_convergence_time_extrapolated"] = conv
    return out


def spatial_fixed():
    """the spatial study at one fixed time step for every mesh, so that the temporal error largely cancels in the
    differences between levels (the adaptive runs take different step sequences on different meshes)"""
    study = json.loads((OUT / "study.json").read_text())
    c = Client(["--embedded", "--workspace", str(OUT / "workspace"), "--allow-read", str(OUT), "--allow-write", str(OUT)])
    c.initialize()
    call(c, "project_open", {"path": str(OUT / "workspace" / "plate_in_air")})
    fixed = {k: v for k, v in BASE.items() if k != "time_stepping"}
    fixed.update(time_stepping="fixed", time_step="60 s", coupling={"scheme": "monolithic"})
    print("== spatial study at a fixed 60 s step (monolithic)")
    for h in ("2 mm", "1 mm", "0.5 mm"):
        call(c, "mesh_generate", {"element_size": h})
        study["runs"][f"fixed60_{h.replace(' ', '')}"] = run(c, f"monolithic {h} fixed 60 s", fixed)
    call(c, "mesh_generate", {"element_size": "2 mm"})
    call(c, "project_save", {})
    c.close()
    finish(study)
    print(f"\n{'SPATIAL STUDY FINISHED WITH PROBLEMS' if PROBLEMS else 'SPATIAL STUDY FINISHED'}: {len(PROBLEMS)} problem(s)")
    return 1 if PROBLEMS else 0


def fmt(x, d=4):
    return "-" if x is None else (f"{x:.{d}f}" if isinstance(x, float) else str(x))


def reading(study, tc):
    """what the cancellation check allows the fixed-step spatial study to say, with the numbers it rests on"""
    runs, conv = study["runs"], tc["spatial_convergence_time_extrapolated"]
    if any(conv[q].get("extrapolated") is None for q in ("plate_peak_c", "outlet_face_mean_c", "interface_heat_j")):
        return ["", "**Reading.** At least one quantity has no plausible order on the time-extrapolated values, so no converged "
                "estimate is offered."]
    cont = tc["contamination_of_mesh_differences"]
    worst = max(c for q in ("plate_peak_c", "outlet_face_mean_c", "interface_heat_j") for c in cont[q] if c is not None)
    worst_disp = max(c for c in cont["max_displacement_mm"] if c is not None)
    flows = [runs[f"fixed60_{h}"].get("flow") or {} for h in ("2mm", "1mm", "0.5mm")]
    tau = ", ".join(fmt(f.get("relaxation_time"), 3) for f in flows)
    scale = ", ".join(fmt(f.get("section_scale_max"), 3) for f in flows)
    pk, ih, ot = (conv[q]["extrapolated"] for q in ("plate_peak_c", "interface_heat_j", "outlet_face_mean_c"))
    ref = runs.get("reference") or {}
    out = ["", f"**Reading.** The temporal error of a 60 s step is the same on all three meshes to within {worst:.1%} of the mesh "
           f"differences it could contaminate ({worst_disp:.1%} for the displacement), and the observed spatial orders do not "
           "change once it is removed: the fixed-step study measures spatial error. That order is about 1 "
           f"({conv['plate_peak_c']['observed_order']:.2f} for the plate peak, {conv['interface_heat_j']['observed_order']:.2f} for the "
           f"interface heat, {conv['outlet_face_mean_c']['observed_order']:.2f} for the outlet face), not the 2 of smooth solutions on "
           "trilinear elements, and this study does not isolate why. Candidates: the plate starts at the inlet, where the uniform "
           "20 °C inflow meets the heated no-slip interface, and ends at an adiabatic floor, so gradients are singular at both "
           f"edges; and the flow is re-solved on each mesh with a different relaxation time ({tau}) and mapped to nodes with a "
           f"section-flux correction that shrinks only about as fast as the cell size (largest factor {scale}). The extrapolated "
           "values rest on three levels in that regime and are indicative, not established.", "",
           f"Best estimates, extrapolated in time and space: plate peak {pk:.3f} °C, interface heat {ih:.1f} J, outlet face "
           f"{ot:.3f} °C."]
    if ref.get("plate_peak_c") is not None and ref.get("interface_heat_j") is not None:
        rise = pk - 20.0
        out[-1] += (f" Against them the reference run (2 mm, tolerance 1e-3) is {ref['plate_peak_c'] - pk:+.3f} K on the plate peak "
                    f"({(ref['plate_peak_c'] - pk) / rise:+.1%} of the temperature rise) and {ref['interface_heat_j'] - ih:+.2f} J "
                    f"({(ref['interface_heat_j'] - ih) / ih:+.1%}) on the interface heat.")
    return out


def write_readme(study):
    r, m = study["runs"].get("reference") or {}, study["runs"].get("monolithic_2mm_1e-3") or {}
    lines = ["# 04 — three-domain demonstration: heated plate in an air channel",
             "",
             "Built and run over MCP by `make_three_domain.py`. **Demonstration only:** both materials are uncalibrated "
             "`demonstration` records and nothing here is compared with a measurement.",
             "",
             "## What is modelled",
             "",
             "* **Flow:** steady laminar incompressible air (constant properties, no buoyancy) through a 60 × 20 × 10 mm channel at "
             "0.02 m/s, from the lattice Boltzmann solver. It is computed once and used from t = 0 (one-way: temperature does not "
             "change it). Periodic across y, no-slip in z.",
             "* **Energy:** the air's energy is advected (SUPG, BiCGSTAB/ILU(0)). The 40 × 20 × 4 mm steel plate conducts. The two "
             "exchange heat through their shared nodes, **with no convection coefficient**. Partitioned (iterated every step) or "
             "monolithic.",
             "* **Structure:** the plate's thermal expansion, clamped at its upstream end (one-way, linear elastic, stored times).",
             "",
             "## Reference run (partitioned, 2 mm, tolerance 1e-3 / 0.05 K, paused and resumed)",
             "",
             f"* paused at t = {fmt(r.get('paused_at_s'), 1)} s and resumed from its checkpoint; {r.get('accepted_steps')} accepted steps",
             f"* plate peak {fmt(r.get('plate_peak_c'))} °C, mean outlet face {fmt(r.get('outlet_face_mean_c'))} °C after 7200 s",
             f"* heat through the interface {fmt(r.get('interface_heat_j'))} J of the 720 J dissipated; largest displacement "
             f"{fmt(r.get('max_displacement_mm'), 5)} mm",
             f"* discrete energy closure {r.get('discrete_closure')}; enthalpy-flux closure {fmt(r.get('enthalpy_flux_closure'), 6)} "
             "(the mapped flow is not exactly divergence-free for the energy equation's elements; see stageE-evidence.md)",
             f"* flow: relaxation time {fmt((r.get('flow') or {}).get('relaxation_time'))}, lattice Mach {fmt((r.get('flow') or {}).get('lattice_mach'))}, "
             f"Re (channel) {fmt((r.get('flow') or {}).get('reynolds_hydraulic'), 2)}, density "
             f"{fmt((r.get('flow') or {}).get('density_min'))}–{fmt((r.get('flow') or {}).get('density_max'))}, section scale "
             f"{fmt((r.get('flow') or {}).get('section_scale_min'))}–{fmt((r.get('flow') or {}).get('section_scale_max'))}",
             f"* monolithic at the same settings: plate peak {fmt(m.get('plate_peak_c'))} °C, {m.get('accepted_steps')} steps, "
             f"{fmt(m.get('wall_s'), 1)} s wall against {fmt(r.get('wall_s'), 1)} s for the partitioned run (which includes the pause)",
             "",
             "## Temporal study (2 mm, monolithic)",
             "",
             "| tolerance | steps | plate peak °C | outlet face °C | interface heat J |",
             "|---|---|---|---|---|"]
    for key, tol in (("monolithic_2mm_1e-3", "1e-3 / 0.05 K"), ("monolithic_2mm_1e-4", "1e-4 / 0.005 K"), ("monolithic_2mm_1e-5", "1e-5 / 0.0005 K")):
        x = study["runs"].get(key) or {}
        lines.append(f"| {tol} | {x.get('accepted_steps')} | {fmt(x.get('plate_peak_c'))} | {fmt(x.get('outlet_face_mean_c'))} | {fmt(x.get('interface_heat_j'))} |")
    def spatial_table(title, keys, note):
        out = ["", f"## {title}", "", note, "",
               "| cells | steps | wall s | plate peak °C | outlet face °C | interface heat J | largest displacement mm | enthalpy-flux closure |",
               "|---|---|---|---|---|---|---|---|"]
        for key, h in zip(keys, ("2 mm", "1 mm", "0.5 mm")):
            x = study["runs"].get(key) or {}
            out.append(f"| {h} | {x.get('accepted_steps')} | {fmt(x.get('wall_s'), 1)} | {fmt(x.get('plate_peak_c'))} | {fmt(x.get('outlet_face_mean_c'))} | "
                       f"{fmt(x.get('interface_heat_j'))} | {fmt(x.get('max_displacement_mm'), 6)} | {fmt(x.get('enthalpy_flux_closure'), 6)} |")
        return out

    def order_table(conv):
        out = ["", "| quantity | observed order (2 → 1 → 0.5 mm) | extrapolated | reading |", "|---|---|---|---|"]
        for q, cv in conv.items():
            if "orders_from_ratios" in cv:
                o = cv["orders_from_ratios"]
                out.append(f"| {q} | {o[0]:.2f}, {o[1]:.2f} (from ratios; exact limit 0) | 0 | converging towards zero |")
            else:
                out.append(f"| {q} | {fmt(cv.get('observed_order'), 2)} | {fmt(cv.get('extrapolated'), 6 if q == 'max_displacement_mm' else 4)} | {cv.get('note')} |")
        return out

    if study["runs"].get("fixed60_2mm"):
        lines += spatial_table("Spatial study at a fixed 60 s step (monolithic)", ("fixed60_2mm", "fixed60_1mm", "fixed60_0.5mm"),
                               "Every mesh takes the same 120 backward-Euler steps, so the temporal error, large at 60 s, is nearly the same on "
                               "each and largely cancels in the differences between levels. These are the numbers to judge mesh convergence by.")
        lines += order_table(study.get("spatial_convergence", {}))
    tc = study.get("temporal_cancellation")
    if tc:
        runs = study["runs"]
        inv = max(abs(v) for v in tc["output_interval_invariance"].values())
        lines += ["", "## Does the temporal error of the 60 s step cancel between meshes?", "",
                  "The same three meshes at 30 s, and the 2 mm and 1 mm meshes at 15 s (monolithic, fields stored hourly; storing "
                  f"fewer fields leaves the solution unchanged: on 2 mm at 60 s the largest difference from the run stored every "
                  f"300 s is {inv:.1e}).", "",
                  "| cells | step s | wall s | plate peak °C | outlet face °C | interface heat J | largest displacement mm |",
                  "|---|---|---|---|---|---|---|"]
        for h, hk in (("2 mm", "2mm"), ("1 mm", "1mm"), ("0.5 mm", "0.5mm")):
            for step in (60, 30, 15):
                x = runs.get(f"fixed{step}_{hk}")
                if x:
                    lines.append(f"| {h} | {step} | {fmt(x.get('wall_s'), 1)} | {fmt(x.get('plate_peak_c'))} | {fmt(x.get('outlet_face_mean_c'))} | "
                                 f"{fmt(x.get('interface_heat_j'))} | {fmt(x.get('max_displacement_mm'), 6)} |")
        lines += ["", "Backward Euler's error is `C Δt`, so the 60 s error of each mesh is −2 [T(30 s) − T(60 s)]. What does not cancel in a "
                  "mesh difference is the change of that error between the two meshes; the last two columns give it as a fraction "
                  "of the mesh difference it contaminates.", "",
                  "| quantity | temporal order 2 mm, 1 mm | T(30 s) − T(60 s) at 2 / 1 / 0.5 mm | contamination 2 → 1 mm | contamination 1 → 0.5 mm |",
                  "|---|---|---|---|---|"]
        for q in QUANTITIES:
            d = [tc["t30_minus_t60"][h][q] for h in ("2mm", "1mm", "0.5mm")]
            c1, c2 = tc["contamination_of_mesh_differences"][q]
            o = [tc["temporal_order"][h][q] for h in ("2mm", "1mm")]
            lines.append(f"| {q} | {fmt(o[0], 2)}, {fmt(o[1], 2)} | {d[0]:.3g} / {d[1]:.3g} / {d[2]:.3g} | "
                         f"{'-' if c1 is None else f'{c1:.1%}'} | {'-' if c2 is None else f'{c2:.1%}'} |")
        lines += ["", "Spatial orders again, at 30 s and on each mesh's time-extrapolated value 2 T(30 s) − T(60 s):"]
        for title, conv in (("30 s", tc["spatial_convergence_30s"]), ("time-extrapolated", tc["spatial_convergence_time_extrapolated"])):
            lines += ["", f"| quantity ({title}) | observed order (2 → 1 → 0.5 mm) | extrapolated | fine mesh − extrapolated | reading |",
                      "|---|---|---|---|---|"]
            for q in QUANTITIES:
                cv = conv.get(q, {})
                lines.append(f"| {q} | {fmt(cv.get('observed_order'), 2)} | {fmt(cv.get('extrapolated'), 6 if q == 'max_displacement_mm' else 4)} | "
                             f"{'-' if cv.get('fine_minus_extrapolated') is None else format(cv['fine_minus_extrapolated'], '.3g')} | {cv.get('note')} |")
        lines += reading(study, tc)
    lines += spatial_table("Spatial study, adaptive (tolerance 1e-4 / 0.005 K, monolithic)", ("monolithic_2mm_1e-4", "monolithic_1mm_1e-4", "monolithic_0.5mm_1e-4"),
                           "Each mesh chose its own step sequence (100, 100 and 239 steps). The temporal study above shows that this tolerance "
                           "leaves errors of a few hundredths of a kelvin, the same size as the differences between meshes, so this table "
                           "mixes spatial and temporal error and is kept only as a record.")
    lines += order_table(study.get("spatial_convergence_adaptive", {}))
    lines += ["",
              "The mesh changes the flow as well as the energy equation: the lattice uses the same cells, so the flow's resolution "
              "(5, 10 and 20 cells across the channel), its relaxation time and its compressibility bound change with it. No "
              "extrapolation is offered unless the three values are monotone with an order between 0.5 and 4.",
              "",
              "## What this study does not show",
              "",
              "* agreement with an experiment: there is none, and the materials are demonstration values;",
              "* the flow start-up, buoyancy, temperature-dependent air properties or turbulence;",
              "* two-way coupling with the structure: the plate's deformation does not change the thermal problem.",
              ""]
    (OUT / "README.md").write_text("\n".join(lines))


if __name__ == "__main__":
    if "--report-only" in sys.argv:
        sys.exit(report_only())
    if "--spatial-fixed" in sys.argv or "--temporal-check" in sys.argv:  # either or both, in this order
        code = spatial_fixed() if "--spatial-fixed" in sys.argv else 0
        sys.exit(code or (temporal_check() if "--temporal-check" in sys.argv else 0))
    code = main()
    sys.exit(code or spatial_fixed() or temporal_check())
