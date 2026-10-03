#!/usr/bin/env python3
"""Compute the three numerical gallery demonstrations through native MCP.

First regenerate the designed geometry with tools/art_gallery.py. Then run one
case, for example:
  python3 tools/art_gallery_compute.py thermal --output-dir /tmp/art-gallery

Acceptance was fixed before the original runs: <=12000 hexes, successful job
within 480 seconds, finite complete result arrays, equilibrium/heat diagnostics
<=1e-6, and thermal enthalpy mismatch <=1e-9. Passing these numerical checks is
not measurement validation. Materials, process parameters and loads are explicit
demonstration inputs. Voxel volume and surface-area bias are recorded; neither a
smooth display nor a closed energy balance establishes geometric convergence.
"""
import argparse
import array
import hashlib
import json
import math
import struct
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ACCEPTANCE = {
    "max_elements": 12000,
    "residual_or_heat_balance": 1e-6,
    "thermal_enthalpy_mismatch": 1e-9,
    "finite_arrays": True,
    "runtime_budget_seconds": 480,
}
MODELS = {
    "fdm": ("helical_flute_vase_mm", "Helical_Flute", "pla_generic_demo"),
    "lpbf": ("helical_window_column_mm", "Helical_Lantern", "ss316l_lpbf_demo"),
    "thermal": ("thermal_corolla_mm", "Thermal_Corolla", "corolla_demo"),
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_results(path):
    """Check the actual result file, including every stored floating array."""
    finite = {}
    with path.open("rb") as stream:
        require(stream.read(8) == b"NVTHR001", "unrecognized result format")
        raw = stream.read(8)
        require(len(raw) == 8, "truncated result header length")
        length = struct.unpack("<Q", raw)[0]
        data = stream.read(length)
        require(len(data) == length, "truncated result header")
        header = json.loads(data)
        for item in header["arrays"]:
            typ, count = item["type"], item["count"]
            size = {"d": 8, "i": 4, "b": 1, "c": 1}[typ]
            require(count >= 0, "negative array count")
            data = stream.read(size * count)
            require(len(data) == size * count, "truncated array: " + item["name"])
            if typ == "d":
                values = array.array("d")
                values.frombytes(data)
                if sys.byteorder != "little":
                    values.byteswap()
                require(all(map(math.isfinite, values)), "nonfinite array: " + item["name"])
                if item["name"] in ("T", "mech_u", "mech_vm", "times"):
                    require(bool(values), "empty result array: " + item["name"])
                    finite[item["name"]] = {"count": len(values), "min": min(values), "max": max(values)}
    return header, finite


def compute(kind, repo, out):
    repo, out = repo.expanduser().resolve(), out.expanduser().resolve()
    model, name, material = MODELS[kind]
    geometry = out / (model + ".stl")
    require(geometry.is_file(), "Generate geometry first: python3 tools/art_gallery.py --output-dir " + str(out))
    require((repo / "navier-mcp").is_file(), "Build the native binaries in " + str(repo) + " first")
    require((repo / "tools" / "mcptest.py").is_file(), "Repository lacks tools/mcptest.py")
    sys.path.insert(0, str(repo / "tools"))
    import mcptest
    # --repo selects its binaries even if another shell session set NAVIER_BIN.
    mcptest.MCP = repo / "navier-mcp"
    workspace = out / "workspace"
    workspace.mkdir(parents=True, exist_ok=True)
    record = {
        "kind": kind,
        "title": name.replace("_", " "),
        "scope": "Numerical demonstration with inferred inputs; no measurement comparison",
        "geometry": geometry.name,
        "geometry_sha256": sha256(geometry),
        "compute_script_sha256": sha256(Path(__file__).resolve()),
        "repository": str(repo),
        "mesh_spacing_mm": 0.75,
        "material": {"id": material, "provenance": "inferred", "calibrated": False},
        "acceptance": dict(ACCEPTANCE),
        "acceptance_passed": False,
        "geometry_caveat": (
            "The solver uses voxel boundary areas. Curved-surface area and volume errors are reported, "
            "not corrected or hidden. No mesh-convergence study or measurement validation is claimed."
        ),
    }
    record_path = out / (kind + "-run.json")
    if record_path.exists():
        previous = record_path.read_bytes()
        history = out / "evidence-history"
        history.mkdir(exist_ok=True)
        digest = hashlib.sha256(previous).hexdigest()
        backup = history / f"{kind}-run-{time.time_ns()}-{digest[:12]}.json"
        with backup.open("xb") as stream:
            stream.write(previous)
        record["prior_record_backup"] = {"path": str(backup.relative_to(out)), "sha256": digest}

    def save():
        record_path.write_text(json.dumps(record, indent=2) + "\n")

    client = mcptest.Client(["--embedded", "--workspace", str(workspace), "--allow-read", str(out)])

    def call(operation, arguments):
        reply = client.call(operation, arguments).get("result", {}).get("structuredContent", {})
        require(reply.get("ok"), operation + ": " + json.dumps(reply))
        return reply["value"]

    save()
    try:
        client.initialize()
        if (workspace / name).exists():
            project = call("project_open", {"path": str(workspace / name)})
        else:
            project = call("project_create", {"name": name, "description": record["scope"]})
        record["project"] = project
        record["import"] = call("geometry_import", {
            "path": str(geometry), "units": "mm", "name": "sculpture", "replace": True,
        })
        if kind == "thermal":
            properties = {
                "id": "corolla_demo", "name": "Thermal corolla demonstration", "family": "metal",
                "status": "demonstration",
                "provenance": "Assumed constant properties for a numerical gallery demonstration",
                "conductivity_w_per_mk": {"value": 10}, "density_kg_m3": {"value": 7800},
                "specific_heat_j_per_kgk": {"value": 500},
            }
            record["material"]["properties"] = properties
            call("material_define", {"material": properties, "replace": True})
        call("material_assign", {"body": "sculpture", "material": material, "source": "inferred"})
        record["mesh"] = call("mesh_generate", {"element_size": ".75 mm", "max_elements": ACCEPTANCE["max_elements"]})
        require(0 < record["mesh"]["mesh"]["elements"] <= ACCEPTANCE["max_elements"], "element-count limit exceeded")
        record["inspection"] = call("mesh_inspect", {})
        bodies = record["inspection"].get("mesh", {}).get("bodies", [])
        record["geometry_approximation"] = bodies
        print(kind, "mesh:", record["mesh"]["mesh"]["elements"], "elements", flush=True)
        for body in bodies:
            print(kind, "geometry:", json.dumps(body), flush=True)

        if kind == "fdm":
            operation = "mech_print_run"
            arguments = {
                "body": "sculpture", "label": "Helical Flute: FDM numerical demonstration",
                "process": {
                    "layer_height": "1.5 mm", "printed_layer_height": ".2 mm",
                    "nozzle_temperature": "210 degC", "bed_temperature": "60 degC", "ambient_temperature": "25 degC",
                    "deposition_rate": "8 mm^3/s", "min_layer_time": "8 s", "cooldown_bed_on": "120 s",
                    "cooldown_bed_off": "240 s", "thermal_substeps": 4, "provenance": "inferred",
                },
            }
        elif kind == "lpbf":
            operation = "lpbf_build_run"
            arguments = {
                "body": "sculpture", "label": "Helical Lantern: inherent-strain numerical demonstration",
                "build_orientation": "X", "layer_thickness_sim": "1.5 mm",
                "inherent_strain": {
                    "exx": -.0004, "eyy": -.0006, "ezz": -.002, "provenance": "inferred",
                    "source": "Demonstration eigenstrain tensor; not a calibrated machine or material",
                },
                "material": {"youngs_modulus": "215000 MPa", "poissons_ratio": .3, "provenance": "inferred"},
                "cut": {"height": "1.5 mm", "kerf": ".75 mm", "from_x": "0 mm", "provenance": "assumed"},
            }
        else:
            selections = [
                {"name": "bottom", "body": "sculpture", "query": {"plane": {"axis": "z", "at": "min"}}, "source": "default"},
                {"name": "outer", "body": "sculpture", "query": {"facing": {"direction": "up", "max_angle_deg": 180}}, "source": "default"},
            ]
            boundaries = [
                {"name": "bottom_pulse", "kind": "heat_flux", "selection": "bottom", "heat_flux": "60000 W/m^2",
                 "schedule": [{"time": "0 s", "factor": 1}, {"time": "25 s", "factor": 0}], "source": "default"},
                {"name": "ambient_convection", "kind": "convection", "selection": "outer",
                 "convection": {"coefficient": 40, "ambient": "20 degC"}, "source": "default"},
                {"name": "ambient_radiation", "kind": "radiation", "selection": "outer",
                 "radiation": {"emissivity": .7, "ambient": "20 degC"}, "source": "default"},
            ]
            record["thermal_setup"] = {"selections": selections, "boundaries": boundaries}
            for selection in selections:
                call("selection_create", dict(selection, replace=True))
            for boundary in boundaries:
                call("boundary_apply", dict(boundary, replace=True))
            operation = "analysis_run"
            arguments = {
                "analysis": "transient_thermal", "end_time": "150 s", "time_step": "1.25 s", "theta": 1,
                "capacity": "lumped", "initial_temperature": "20 degC", "output_times": [2.5 * i for i in range(1, 61)],
            }
            record["setup_validation"] = call("setup_validate", arguments)
            require(record["setup_validation"]["ready"], "thermal setup is not ready")

        call("project_save", {})
        record["operation"], record["inputs"] = operation, arguments
        save()
        start = time.monotonic()
        job = call(operation, arguments)
        record["run"] = job
        save()
        while True:
            status = call("job_status", {"job_id": job["job_id"], "wait_seconds": 10})
            record["status"] = status
            record["elapsed_seconds"] = time.monotonic() - start
            save()
            if status["state"] not in ("queued", "running"):
                break
            print(kind, status.get("progress"), status.get("stage"), round(record["elapsed_seconds"], 1), flush=True)
            if record["elapsed_seconds"] > ACCEPTANCE["runtime_budget_seconds"]:
                call("job_cancel", {"job_id": job["job_id"]})
                raise RuntimeError("480-second compute budget exceeded")
        require(record["elapsed_seconds"] <= ACCEPTANCE["runtime_budget_seconds"], "480-second compute budget exceeded")
        require(status["state"] == "succeeded", json.dumps(status))
        summary = status["summary"]
        results = summary.get("results", {})
        tolerance = ACCEPTANCE["residual_or_heat_balance"]
        if kind == "fdm":
            require(results["worst_heat_balance_relative"] <= tolerance, "FDM heat-balance criterion failed")
            require(results["equilibrium_error_last_solve"] <= tolerance, "FDM equilibrium criterion failed")
        elif kind == "lpbf":
            require(results["equilibrium_error_last_solve"] <= tolerance, "LPBF equilibrium criterion failed")
        else:
            energy = summary["energy"]
            require(energy["closure_error"] <= tolerance, "thermal closure criterion failed")
            require(energy["worst_step_balance_error"] <= tolerance, "thermal step-balance criterion failed")
            require(energy["enthalpy_mismatch"] <= ACCEPTANCE["thermal_enthalpy_mismatch"], "thermal enthalpy criterion failed")
        path = Path(job["run_directory"]) / "results.nvt"
        record["result_header"], record["finite_arrays"] = inspect_results(path)
        record["results_sha256"] = sha256(path)
        record["acceptance_passed"] = True
        call("project_save", {})
        save()
        print(kind, "PASS", json.dumps({"elapsed_seconds": record["elapsed_seconds"], "results": results,
                                         "finite": record["finite_arrays"]}), flush=True)
    except BaseException as error:
        record["acceptance_passed"] = False
        record["error"] = str(error)
        save()
        raise
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=MODELS)
    parser.add_argument("--repo", type=Path, default=REPO, help="Repository containing the native binaries (default: this script's repository)")
    parser.add_argument("--output-dir", "--out", dest="output_dir", type=Path, required=True,
                        help="Directory containing the regenerated STL files; also receives projects and evidence")
    arguments = parser.parse_args()
    compute(arguments.kind, arguments.repo, arguments.output_dir)


if __name__ == "__main__":
    main()
