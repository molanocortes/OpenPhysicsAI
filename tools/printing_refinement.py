#!/usr/bin/env python3
"""Independent MCP printing refinement study, criteria declared before first run.

An aligned 8 x 2 x 4 mm wall has fixed geometry and 1 mm simulation layers.
Spatial levels are h = 1, 0.5, 0.25 mm (64, 512, 4096 HEX8 elements).
The FDM process uses demonstration PLA and inferred process inputs; LPBF is an
elastic inherent-strain model with inferred coefficients, not a melt-pool model.
Spatial runs keep thermal_substeps=8. Temporal FDM runs use 4, 8, 16 substeps
on the SAME 0.5 mm mesh. All other physical and numerical inputs stay fixed.

Acceptance, not silently amended after execution:
* Every job succeeds within 480 seconds; timed-out jobs are cancelled.
* FDM step/deposition/whole-print heat balance and an independently reconstructed
  full heat ledger are <= 1e-6 relative. FDM normalized recovered free-equation
  residuals at bed release and the last actual solve are <= 1e-6.
* LPBF last-solve equilibrium residual is <= 1e-6. These equilibrium residuals
  use solid_solve's normalization: applied nodal load norm + individual prescribed
  DOF reaction norm + assembled free RHS norm (including eigenstrain). LPBF does
  NOT compute a thermal energy balance.
* Successive quantities are FDM released vertical warp span (max u_z - min u_z)
  and LPBF maximum u_z over the largest-x face before any cut. Their finest
  spatial changes, and the FDM finest temporal change, must be <= 5 percent of
  the finer value. An observable <= 1e-12 mm is inconclusive, not a pass.
* Report both successive differences and monotonicity; do not infer an accuracy
  percentage or a convergence order from this small study. Max stress at fixed
  edges is not a convergence observable.

EXPLICIT CRITERION AMENDMENT 1, after the first run and before the amended run:
The first criterion divided FDM released support reaction by the exported
largest_bed_reaction_n and failed. Source inspection established that the latter
is a NET plate force, theoretically zero for this wall's self-equilibrated
thermal eigenstrains. Dividing roundoff by roundoff is not an equilibrium test.
Replace that invalid ratio with the solver's existing recovered-free-residual
normalization at release and last solve, retaining the SAME 1e-6 threshold.
The complete initial FAIL is preserved unchanged in
validation/printing-refinement/initial-invalid-normalization.json. Its original
ratio and the reason it is invalid remain in each report. This amendment fixes
the diagnostic definition; it does not improve or relax the physical model.

This verifies discretization sensitivity of these implemented models only. It
does not establish experimental accuracy, polymer bead bonding, anisotropy,
crystallization, a resolved LPBF thermal history, or calibrated material inputs.
Uses stdlib plus existing repository MCP client/box generator. Run after make am:
    python3 tools/printing_refinement.py --output /tmp/printing-refinement.json
Raw specs, summaries, result hashes and engine logs remain in its reported
temporary workspace. JSON checkpoints are written after every completed job.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from mcptest import MCP, Client
from printflow import box_stl, PROCESS

BALANCE_LIMIT = 1e-6
REFINEMENT_LIMIT = 0.05
JOB_SECONDS = 480
SPACINGS = (1.0, 0.5, 0.25)
SUBSTEPS = (4, 8, 16)
INITIAL_EVIDENCE = Path(__file__).resolve().parent.parent / "validation/printing-refinement/initial-invalid-normalization.json"
NUMERICS = {"solver": "iterative", "tolerance": 1e-10}
FDM_PROCESS = dict(PROCESS, provenance="inferred", thermal_substeps=8)
FDM_INPUT = {"body": "wall", "process": FDM_PROCESS,
             "numerics": dict(NUMERICS, skip_increment_below="0 K")}
LPBF_INPUT = {"body": "wall", "build_orientation": "X", "layer_thickness_sim": "1 mm",
              "inherent_strain": {"exx": -0.001, "eyy": -0.002, "ezz": 0,
                                  "provenance": "inferred", "source": "Refinement study tensor, not a calibration"},
              "material": {"youngs_modulus": "200000 MPa", "poissons_ratio": 0.3,
                           "provenance": "inferred"}, "numerics": NUMERICS}


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as src:
        for block in iter(lambda: src.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def checkpoint(path, report):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    os.replace(temporary, path)


def call(client, name, arguments):
    response = client.call(name, arguments)
    result = response.get("result", {}).get("structuredContent", {})
    if result.get("ok") is not True:
        raise RuntimeError(f"{name}: {json.dumps(result or response)}")
    return result.get("value", {})


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def ratio(numerator, denominator):
    if not finite(numerator) or not finite(denominator) or abs(denominator) <= 1e-12:
        return None
    return abs(numerator) / abs(denominator)


def checks(kind, summary):
    values = summary.get("results", {})
    if kind == "lpbf":
        metrics = {"equilibrium_relative": values.get("equilibrium_error_last_solve")}
        quantity = values.get("tip_uz_before_cut_mm")
    else:
        names = ("supplied_nozzle_enthalpy_above_ambient_j", "stored_enthalpy_above_ambient_j",
                 "heat_into_bed_j", "heat_into_air_j", "enthalpy_removed_with_supports_j")
        heat = [values.get(name) for name in names]
        metrics = {"step_heat_balance_relative": values.get("worst_heat_balance_relative"),
                   "deposition_heat_balance_relative": values.get("worst_deposition_balance_relative"),
                   "whole_print_heat_balance_relative": values.get("whole_print_heat_balance_relative"),
                   "independent_whole_print_heat_balance_relative": None,
                   "equilibrium_relative_at_release": values.get("equilibrium_error_at_release"),
                   "equilibrium_relative_last_solve": values.get("equilibrium_error_last_solve")}
        if all(finite(v) for v in heat):
            supplied, stored, bed, air, removed = heat
            metrics["independent_whole_print_heat_balance_relative"] = ratio(stored + bed + air + removed - supplied,
                                                                            max(abs(v) for v in heat))
        lo, hi = values.get("warp_z_min_mm"), values.get("warp_z_max_mm")
        quantity = hi - lo if finite(lo) and finite(hi) else None
    accepted = all(finite(v) and 0 <= v <= BALANCE_LIMIT for v in metrics.values())
    return quantity, metrics, accepted


def comparison(records, labels):
    quantities = [r.get("observable_mm") for r in records]
    meaningful = len(quantities) == 3 and all(finite(q) and abs(q) > 1e-12 for q in quantities)
    differences, signed = [], []
    if meaningful:
        for i in range(1, 3):
            delta = quantities[i] - quantities[i - 1]
            signed.append(delta)
            differences.append({"from": labels[i - 1], "to": labels[i], "absolute_mm": abs(delta),
                                "relative_to_finer": abs(delta) / abs(quantities[i])})
    monotone = meaningful and signed[0] * signed[1] > 0 and abs(signed[1]) < abs(signed[0])
    return {"levels": labels, "observables_mm": quantities, "successive_differences": differences,
            "decreasing_same_sign_differences": bool(monotone), "convergence_order": None,
            "meaningful_observable": meaningful,
            "finest_change_accepted": meaningful and differences[-1]["relative_to_finer"] <= REFINEMENT_LIMIT,
            "interpretation": "discretization sensitivity only, not physical validation or an accuracy estimate"}


def run(client, report, output, kind, spacing, substeps=8):
    record = {"kind": kind, "element_size_mm": spacing, "thermal_substeps": substeps if kind == "fdm" else None,
              "state": "setting_up", "criteria_passed": False}
    report["runs"].append(record)
    checkpoint(output, report)
    started = time.monotonic()
    job = None
    try:
        call(client, "material_assign", {"body": "wall", "material": "pla_generic_demo" if kind == "fdm" else
                                        "ss316l_lpbf_demo", "source": "inferred"})
        mesh = call(client, "mesh_generate", {"element_size": f"{spacing:g} mm"})
        record["mesh"] = mesh
        expected = int(64 / spacing ** 3)
        if mesh.get("mesh", {}).get("elements") != expected:
            raise RuntimeError(f"aligned mesh does not have {expected} elements")
        inputs = json.loads(json.dumps(FDM_INPUT if kind == "fdm" else LPBF_INPUT))
        if kind == "fdm":
            inputs["process"]["thermal_substeps"] = substeps
        record["inputs"] = inputs
        launched = call(client, "mech_print_run" if kind == "fdm" else "lpbf_build_run", inputs)
        record["launch"] = launched
        job = launched["job_id"]
        record["state"] = launched.get("state", "queued")
        deadline = time.monotonic() + JOB_SECONDS
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                call(client, "job_cancel", {"job_id": job})
                record["state"] = "timed_out_cancel_requested"
                raise TimeoutError(f"job exceeded predeclared {JOB_SECONDS} seconds")
            status = call(client, "job_status", {"job_id": job, "wait_seconds": min(10, remaining)})
            if status.get("state") not in ("queued", "running"):
                break
        record["state"] = status.get("state")
        record["status"] = status
        if status.get("state") != "succeeded":
            raise RuntimeError(f"job did not succeed: {json.dumps(status.get('error'))}")
        summary = status.get("summary", {})
        record["observable_mm"], record["balance_checks"], accepted = checks(kind, summary)
        if kind == "fdm":
            values = summary.get("results", {})
            record["original_invalid_reaction_diagnostic"] = {
                "release_support_reaction_n": values.get("support_reaction_after_release_n"),
                "net_bed_reaction_n": values.get("largest_bed_reaction_n"),
                "ratio": ratio(values.get("support_reaction_after_release_n"), values.get("largest_bed_reaction_n")),
                "used_for_acceptance": False,
                "reason": "self-equilibrated thermal loads have zero NET plate reaction; this ratio divides roundoff by roundoff"}
        record["failed_balance_checks"] = [key for key, value in record["balance_checks"].items()
                                           if not finite(value) or value < 0 or value > BALANCE_LIMIT]
        record["criteria_passed"] = accepted and finite(record["observable_mm"])
        run_dir = Path(launched["run_directory"])
        record["result_sha256"] = digest(run_dir / "results.nvt")
        record["spec"] = json.loads((run_dir / "spec.json").read_text())
    except Exception as exc:
        record["criteria_passed"] = False
        record["error"] = str(exc)
        if job and record["state"] not in ("succeeded", "failed", "cancelled", "timed_out_cancel_requested"):
            try:
                call(client, "job_cancel", {"job_id": job})
            except Exception as cancel_error:
                record["cancel_error"] = str(cancel_error)
        if record["state"] == "setting_up":
            record["state"] = "error"
    record["wall_seconds"] = time.monotonic() - started
    checkpoint(output, report)
    observable = record.get("observable_mm")
    print(f"{kind} h={spacing:g} mm substeps={record['thermal_substeps']}: {record['state']}; "
          f"observable={observable} mm; checks={record['criteria_passed']}; {record['wall_seconds']:.3f} s", flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("/tmp/printing-refinement.json"))
    args = parser.parse_args()
    output = args.output.resolve()
    if not MCP.is_file():
        parser.error("make am must build navier-mcp first")
    output.parent.mkdir(parents=True, exist_ok=True)
    workspace = Path(tempfile.mkdtemp(prefix="printing-refinement-"))
    stl = workspace / "wall.stl"
    box_stl(stl, (0, 0, 0), (8, 2, 4), name=b"printing refinement wall")
    report = {"criterion_declared_in": "tools/printing_refinement.py module docstring",
              "scope": "model discretization sensitivity; no measured-print validation",
              "geometry_mm": {"minimum": [0, 0, 0], "maximum": [8, 2, 4]},
              "geometry_sha256": digest(stl), "input_provenance": "inferred",
              "material_status": "demonstration", "balance_limit": BALANCE_LIMIT,
              "finest_change_limit": REFINEMENT_LIMIT, "per_job_seconds_limit": JOB_SECONDS,
              "observables": {"fdm": "released max u_z - min u_z, mm",
                              "lpbf": "maximum u_z on largest-x face before cut, mm; no cut requested"},
              "metric_definitions": {"independent_fdm_energy": "absolute heat-ledger residual / largest absolute ledger term, J/J",
                                     "equilibrium": "recovered free-equation residual norm / (applied nodal load norm + individual prescribed-DOF reaction norm + assembled free RHS norm including eigenstrain), dimensionless"},
              "workspace": str(workspace), "output": str(output), "runs": [], "accepted": False}
    original = json.loads(INITIAL_EVIDENCE.read_text())
    report["criterion_amendments"] = [{
        "number": 1, "initial_report": str(INITIAL_EVIDENCE), "initial_report_sha256": digest(INITIAL_EVIDENCE),
        "original_accepted": original["accepted"], "original_threshold": original["balance_limit"],
        "original_failed_checks": [{"element_size_mm": r["element_size_mm"], "thermal_substeps": r["thermal_substeps"],
                                    "release_equilibrium_relative": r.get("balance_checks", {}).get("release_equilibrium_relative")}
                                   for r in original["runs"] if r["kind"] == "fdm"],
        "reason": "largest_bed_reaction_n is the NET plate force, theoretically zero for self-equilibrated eigenstrain; normalization was invalid",
        "replacement": "normalized recovered free residual at release and last actual solve",
        "replacement_threshold": BALANCE_LIMIT,
        "physical_model_changed_for_this_amendment": False}]
    checkpoint(output, report)
    client = Client(["--embedded", "--workspace", str(workspace / "projects"), "--allow-read", str(workspace)])
    # Drain diagnostics while jobs run so a full stderr pipe cannot stall a solve.
    def drain():
        with open(workspace / "engine.log", "wb") as log:
            for data in iter(lambda: client.proc.stderr.read(65536), b""):
                log.write(data)
                log.flush()
    logger = threading.Thread(target=drain, daemon=True)
    logger.start()
    try:
        initialized = client.initialize()
        report["engine_server_info"] = initialized.get("result", {}).get("serverInfo", {})
        report["engine_binary_sha256"] = digest(MCP)
        report["capabilities"] = call(client, "capabilities_get", {})
        project = call(client, "project_create", {"name": "refinement", "description": report["scope"]})
        report["project"] = project
        call(client, "geometry_import", {"path": str(stl), "units": "mm", "name": "wall"})
        spatial = {}
        for kind in ("lpbf", "fdm"):
            spatial[kind] = [run(client, report, output, kind, h) for h in SPACINGS]
        temporal = []
        for substeps in SUBSTEPS:
            if substeps == 8:
                temporal.append(spatial["fdm"][1])
            else:
                temporal.append(run(client, report, output, "fdm", 0.5, substeps))
        report["comparisons"] = {"lpbf_spatial": comparison(spatial["lpbf"], list(SPACINGS)),
                                 "fdm_spatial": comparison(spatial["fdm"], list(SPACINGS)),
                                 "fdm_temporal": comparison(temporal, list(SUBSTEPS))}
        temporal_hashes = [r.get("launch", {}).get("model", {}).get("mesh_hash") for r in temporal]
        report["temporal_mesh_hashes"] = temporal_hashes
        report["temporal_mesh_identical"] = bool(temporal_hashes[0]) and len(set(temporal_hashes)) == 1
        report["accepted"] = all(r["criteria_passed"] for r in report["runs"]) and all(
            c["finest_change_accepted"] for c in report["comparisons"].values()) and report["temporal_mesh_identical"]
    except Exception as exc:
        report["error"] = str(exc)
    finally:
        try:
            client.proc.stdin.close()
            client.proc.wait(timeout=5)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            client.proc.kill()
            client.proc.wait()
        logger.join(timeout=1)
        checkpoint(output, report)
    for label, compared in report.get("comparisons", {}).items():
        differences = compared["successive_differences"]
        percentages = [100 * d["relative_to_finer"] for d in differences]
        print(f"{label}: changes={percentages} percent; accepted={compared['finest_change_accepted']}")
    print(f"PRINTING REFINEMENT: {'PASS' if report['accepted'] else 'FAIL'}; evidence {output}; workspace {workspace}")
    return 0 if report["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
