#!/usr/bin/env python3
"""cadspecimentest.py - GOALS.md G23 step 5c, S3 and S4: the printed test cubes of Li et al. themselves, through
build/openphysics-cad specimen. S1 and S2 are tools/specimentest.cpp. Longer than a session step: run it detached,

    nohup python3 tools/cadspecimentest.py > specimen.log 2>&1 &

Criteria, written 2026-10-08 before the first run (GOALS.md G23 step 5c), amended only openly below them:
S3 the band sheet and network gyroid cubes at a solid fraction of 0.3, at 8 and at 10 voxels per cell (the most this
   laptop's memory holds): the size factor, the cube's apparent modulus over the periodic cell's at the same voxels, at
   8 and at 10 within 5 per cent of each other
S4 the prediction for Li et al.'s cubes, E_s times 5a's periodic fraction at 64 voxels times the size factor at 10
   voxels, frictionless and bonded, written down before the comparison: each measured modulus (62.51 and 39.21 MPa)
   between 0.9 times the frictionless prediction and 1.1 times the bonded one

First run, 2026-10-08: S3 passed (size factors within 0.2 to 1.8 per cent); S4 failed (+53 to +66 per cent): the
cubes are as stiff as the infinite lattice within a few per cent, so their size is not why the printed ones measure
softer (GOALS.md G23 5c).
"""
import json, pathlib, shutil, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor

ROOT = pathlib.Path(__file__).resolve().parents[1]
CAD = ROOT / "build" / "openphysics-cad"
NU, ES = 0.33, 850.1
MEASURED = {"band": (62.51, 0.44), "network": (39.21, 0.23)}
failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what, flush=True)
    if not ok:
        failures.append(what)
    return ok


def run(tmp, command, req, name):
    f = tmp / (name + ".json")
    f.write_text(json.dumps(req))
    t0 = time.time()
    r = subprocess.run([str(CAD), command, str(f)], capture_output=True, text=True, timeout=7200)
    lines = [l for l in r.stdout.splitlines() if l.startswith("{")]
    rec = json.loads(lines[-1]) if lines else {"ok": False, "error": {"message": (r.stderr or r.stdout)[-300:]}}
    rec["_wall_s"] = time.time() - t0
    return rec


def main():
    if not CAD.exists():
        print("cadspecimentest: build/openphysics-cad is not built (make build/openphysics-cad)")
        return 2
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-cadspecimen-"))
    try:
        core = lambda form: {"kind": "gyroid", "form": form, "cell_mm": 6, "solid_fraction": 0.3}
        print("the periodic cells at 64 voxels (5a's fraction)", flush=True)
        with ThreadPoolExecutor(max_workers=2) as pool:
            cells = dict(zip(("band", "network"), pool.map(
                lambda form: run(tmp, "cell", {"core": core(form), "poisson_ratio": NU, "voxels": 64}, "cell-" + form), ("band", "network"))))
        for form, r in cells.items():
            print("    %s: E/Es %s at fraction %s (%.0f s) %s" % (form, (r.get("cubic") or {}).get("modulus_over_Es"), r.get("solid_fraction"),
                                                            r["_wall_s"], (r.get("error") or {}).get("message", "")), flush=True)
        spec = {}
        for form in ("band", "network"):
            for n in (8, 10):
                print("the %s cube, 6 x 6 x 6 cells, %d voxels per cell" % (form, n), flush=True)
                r = run(tmp, "specimen", {"core": core(form), "poisson_ratio": NU, "cells": [6, 6, 6], "voxels_per_cell": n, "platens": "both"},
                        "spec-%s-%d" % (form, n))
                spec[(form, n)] = r
                if not check(r.get("ok"), "%s at %d: made (%.0f s) %s" % (form, n, r["_wall_s"], (r.get("error") or {}).get("message", ""))):
                    continue
                for mode in ("frictionless", "bonded"):
                    m = r[mode]
                    print("    %s: E/Es %.5f, periodic %.5f, size factor %.4f; %d elements (%d voxels dropped), %d unknowns, %d iterations, %.0f s"
                          % (mode, m["modulus_over_Es"], r["periodic_modulus_over_Es"], m["size_factor"], m["elements"], m["voxels_dropped"],
                             m["unknowns"], m["iterations"], m["seconds"]), flush=True)

        print("S3 the size factor at 8 and 10 voxels per cell")
        for form in ("band", "network"):
            a, b = spec.get((form, 8), {}), spec.get((form, 10), {})
            if not (a.get("ok") and b.get("ok")):
                check(False, "%s: both resolutions needed" % form)
                continue
            for mode in ("frictionless", "bonded"):
                fa, fb = a[mode]["size_factor"], b[mode]["size_factor"]
                check(abs(fa - fb) / fb <= 0.05, "%s %s: %.4f at 8, %.4f at 10 (%.1f %%)" % (form, mode, fa, fb, 100 * abs(fa - fb) / fb))

        print("S4 the prediction for the printed cubes")
        pred = {}
        for form in ("band", "network"):
            e64 = (cells[form].get("cubic") or {}).get("modulus_over_Es")
            b = spec.get((form, 10), {})
            if not (e64 and b.get("ok")):
                check(False, "%s: the prediction needs the cell at 64 and the cube at 10" % form)
                continue
            pred[form] = {mode: ES * e64 * b[mode]["size_factor"] for mode in ("frictionless", "bonded")}
            print("    prediction, written before the comparison: %s cube %.2f MPa (frictionless) to %.2f MPa (bonded)"
                  % (form, pred[form]["frictionless"], pred[form]["bonded"]), flush=True)
        for form, p in pred.items():
            meas, sd = MEASURED[form]
            lo, hi = 0.9 * p["frictionless"], 1.1 * p["bonded"]
            check(lo <= meas <= hi, "%s: measured %.2f +- %.2f MPa against %.2f to %.2f (frictionless %+.1f %%, bonded %+.1f %%)"
                  % (form, meas, sd, lo, hi, 100 * (p["frictionless"] - meas) / meas, 100 * (p["bonded"] - meas) / meas))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("cadspecimentest: %s" % ("all checks passed" if not failures else "%d failed" % len(failures)), flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
