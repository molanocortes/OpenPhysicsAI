#!/usr/bin/env python3
"""x1_ccx.py - X1: NAVIER's assembled problems solved again by CalculiX (criteria: ../CRITERIA-independent.md)

    python3 x1_ccx.py WORK_DIR FOUR_LEVELS_DIR OUT_DIR

Cases: C10 at 1.25 and 0.625 mm, brackets A and B at 1 mm; each with NAVIER full_integration against C3D8 and NAVIER
incompatible_modes against C3D8I. OUT_DIR/<case>/ keeps the exact deck (model.inp.gz), the NAVIER reference values,
the CalculiX log, an extract of model.dat without the all-node displacement block, and comparison.json.
If CalculiX's direct solver (SPOOLES) exits abnormally, the failure is recorded and the identical deck is solved again
with `*STATIC, SOLVER=ITERATIVE CHOLESKY` in <case>_iterative/ (only the solver line differs; its stopping limit is fixed
by CalculiX, so reactions balance less tightly than with the direct solver).
"""
import gzip
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import ccx_crosscheck as cc  # noqa: E402

CCX = os.environ.get("CCX", str(Path.home() / ".navier-eval-tools/ccx-env/bin/ccx"))
WORK, FOUR, OUT = (Path(p).resolve() for p in sys.argv[1:4])


def run_at(project, h, formulation, tol=1e-10):
    for spec in sorted((project / "runs").glob("*/spec.json")):
        s = json.load(open(spec))
        st = s["settings"]
        if abs(s["mesh"]["element_size_mm"][0] - h) < 1e-9 and st["formulation"] == formulation and abs(st["tolerance"] - tol) < 1e-20 \
                and (spec.parent / "results.nvr").exists():
            return spec.parent
    raise SystemExit(f"no {formulation} run at {h} mm with results in {project}")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cases = [("C10_1.25", WORK / "cantilevers/designs/C10", 1.25, [50.3, 5.3, 9.9]),
             ("C10_0.625", WORK / "cantilevers/designs/C10", 0.625, [50.3, 5.3, 9.9]),
             ("A_1", FOUR / "designs/A", 1.0, [40.3, 20.3, 59.9]),
             ("B_1", FOUR / "designs/B", 1.0, [40.3, 20.3, 59.9]),
             ("B_2", FOUR / "designs/B", 2.0, [40.3, 20.3, 59.9])]
    summary = []
    for name, project, h, probe in cases:
        for formulation, element in (("full_integration", "C3D8"), ("incompatible_modes", "C3D8I")):
            run = run_at(project, h, formulation)
            d = OUT / f"{name}_{element}"
            if d.exists():
                shutil.rmtree(d)
            cc.export(run, d, element, probe, all_nodes=True)
            row = solve_and_compare(run, d, name, element, formulation, "spooles")
            summary.append(row)
            print(json.dumps(row), flush=True)
            if row["status"] == "ccx_failed":
                di = OUT / f"{name}_{element}_iterative"
                if di.exists():
                    shutil.rmtree(di)
                di.mkdir()
                deck = gzip.decompress((d / "model.inp.gz").read_bytes()).decode()
                assert deck.count("\n*STATIC\n") == 1
                (di / "model.inp").write_text(deck.replace("\n*STATIC\n", "\n*STATIC, SOLVER=ITERATIVE CHOLESKY\n"))
                shutil.copy(d / "navier_reference.json", di / "navier_reference.json")
                row = solve_and_compare(run, di, name, element, formulation, "iterative_cholesky")
                summary.append(row)
                print(json.dumps(row), flush=True)
    (OUT / "x1_summary.json").write_text(json.dumps(summary, indent=1))


def solve_and_compare(run, d, name, element, formulation, solver):
    env = dict(os.environ, OMP_NUM_THREADS="4", CCX_NPROC_STIFFNESS="4", CCX_NPROC_EQUATION_SOLVER="4")
    t0 = time.time()
    with open(d / "ccx.log", "w") as log:
        rc = subprocess.run([CCX, "-i", "model"], cwd=d, stdout=log, stderr=subprocess.STDOUT, env=env).returncode
    secs = time.time() - t0
    if rc != 0:
        with open(inp := d / "model.inp", "rb") as src, gzip.open(d / "model.inp.gz", "wb", compresslevel=9) as dst:
            shutil.copyfileobj(src, dst)
        (d / "SHA256.json").write_text(json.dumps({"model.inp": hashlib.sha256(inp.read_bytes()).hexdigest()}, indent=1))
        for f in d.iterdir():
            if f.name not in ("model.inp.gz", "SHA256.json", "ccx.log", "navier_reference.json"):
                f.unlink()
        return {"case": name, "element": element, "navier_formulation": formulation, "ccx_solver": solver, "status": "ccx_failed", "ccx_rc": rc,
                "ccx_seconds": secs, "note": "CalculiX exited before writing results; the deck is preserved (model.inp.gz)"}
    res = cc.compare(run, d)
    res["ccx_seconds"] = secs
    res["ccx_return_code"] = rc
    res["ccx_version"] = subprocess.run([CCX, "-v"], capture_output=True, text=True).stdout.strip()
    (d / "comparison.json").write_text(json.dumps(res, indent=1))
    # preserve the exact deck compressed, an extract of the output and hashes of the full files
    inp, dat = d / "model.inp", d / "model.dat"
    hashes = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in (inp, dat) if f.exists()}
    with open(inp, "rb") as src, gzip.open(d / "model.inp.gz", "wb", compresslevel=9) as dst:
        shutil.copyfileobj(src, dst)
    if dat.exists():
        keep, skip = [], False
        for line in dat.read_text(errors="replace").splitlines():
            if "for set NALL" in line:
                skip = True
            elif skip and "for set" in line:
                skip = False
            if not skip:
                keep.append(line)
        (d / "model.dat.extract").write_text("\n".join(keep) + "\n")
    (d / "SHA256.json").write_text(json.dumps(hashes, indent=1))
    for f in d.iterdir():
        if f.name not in ("model.inp.gz", "model.dat.extract", "SHA256.json", "ccx.log", "comparison.json", "navier_reference.json", "model.sta"):
            f.unlink()
    return {"case": name, "element": element, "navier_formulation": formulation, "ccx_solver": solver, "status": "compared", "nodes": res["case"]["nodes"], "elements": res["case"]["elements"],
           "displacement_rel": res["load_region_nodal_mean_mm"]["relative_difference"],
           "navier_disp_mm": res["load_region_nodal_mean_mm"]["navier"], "ccx_disp_mm": res["load_region_nodal_mean_mm"]["ccx"],
           "energy_rel": res["strain_energy_nmm"]["relative_difference"], "reaction_force_rel": res["reaction_force_n"]["relative_difference"],
           "navier_force_balance": res["reaction_force_n"]["navier_balance"], "ccx_force_balance": res["reaction_force_n"]["ccx_balance"],
           "reaction_moment_rel": res["reaction_moment_nmm"]["relative_difference"],
           "stress_xx_rel": res["probe_stress_mean_mpa"].get("xx", {}).get("relative_difference"),
           "stress_xx_navier": res["probe_stress_mean_mpa"].get("xx", {}).get("navier"), "stress_xx_ccx": res["probe_stress_mean_mpa"].get("xx", {}).get("ccx"),
           "max_nodal_difference_mm": res.get("all_nodes", {}).get("max_displacement_difference_mm"),
           "max_nodal_displacement_mm": res.get("all_nodes", {}).get("max_displacement_navier_mm"),
           "ccx_seconds": secs, "ccx_rc": rc, "ccx_messages": [m for m in res["ccx_messages"] if not m.startswith("iteration=")][:5],
           "ccx_iterations": sum(1 for m in res["ccx_messages"] if m.startswith("iteration=")) or None}


if __name__ == "__main__":
    main()
