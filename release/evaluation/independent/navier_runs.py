#!/usr/bin/env python3
"""navier_runs.py - the NAVIER runs of the independent evaluation (baseline binaries, isolated workspace)

    python3 navier_runs.py RELEASE_DIR WORK_DIR

RELEASE_DIR is an extracted release (bin/, share/navier/tests/mcptest.py); WORK_DIR receives geometry, studies and
runs.json (the run directories used by the CalculiX comparisons and the beam hypothesis tests).
  cantilevers   C10 (100x10x10) and C20 (100x10x5), steel E 200 GPa nu 0.3, root fixed, 10 N -z over the tip face,
                2.5 / 1.25 / 0.625 mm, incompatible modes (the rc1 R1 case)
  t1_nu0        the same with nu = 0
  t2_t3         C10L (200x10x10, L doubled) and C10W (100x20x10, width doubled), nu 0.3
  full          full-integration runs of C10 at 1.25 and 0.625 mm and of brackets A and B at 1 mm
"""
import json
import os
import struct
import sys
import time
from pathlib import Path

REL, WORK = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
os.environ["NAVIER_BIN"] = str(REL / "bin")
sys.path.insert(0, str(REL / "share/navier/tests"))
from mcptest import Client  # noqa: E402


def box_stl(path, lx, ly, lz):
    p = [(0, 0, 0), (lx, 0, 0), (lx, ly, 0), (0, ly, 0), (0, 0, lz), (lx, 0, lz), (lx, ly, lz), (0, ly, lz)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, c, d in quads:
        tris += [(p[a], p[b], p[c]), (p[a], p[c], p[d])]
    with open(path, "wb") as f:
        f.write(b"evaluation box".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


def steel(nu, ident):
    return {"id": ident, "name": f"steel test values, nu {nu}", "family": "metal", "status": "user_supplied", "provenance": "evaluation test values",
            "youngs_modulus_pa": {"value": 200e9}, "poisson_ratio": {"value": nu}, "density_kg_m3": {"value": 7850}}


def cantilever_study(name, designs, nu, sizes):
    root = {"query": {"plane": {"axis": "x", "at": "min"}}, "description": "root face, fixed", "source": "user"}
    tip = {"query": {"plane": {"axis": "x", "at": "max"}}, "description": "tip face, loaded", "source": "user"}
    return {"name": name, "question": "evaluation: cantilever tip deflection", "designs": [
        {"name": n, "geometry": {"path": str(WORK / f"{n}.stl"), "units": "mm"}, "mounting_region": root, "load_region": tip} for n in designs],
        "material": {"record": steel(nu, f"steel_nu{str(nu).replace('.', '')}"), "source": "user"},
        "mounting": {"idealization": "fixed", "description": "fixed root", "source": "user"},
        "load": {"kind": "force", "force": "10 N", "direction": "-z", "source": "user"},
        "refinement": {"element_sizes": sizes}, "retain_results": "refinement",
        "accept": [{"id": "equivalence", "reason": "the cross-section or length is the difference being studied"}]}


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    for n, dims in {"C10": (100, 10, 10), "C20": (100, 10, 5), "C10L": (200, 10, 10), "C10W": (100, 20, 10)}.items():
        box_stl(WORK / f"{n}.stl", *dims)
    four = Path(os.environ.get("FOUR_LEVELS", "/nonexistent"))
    # one common root for everything this evaluation reads and writes (rc1 accepts at most 8 roots, 5 of them defaults)
    common = Path(os.path.commonpath([str(WORK), str(REL)] + ([str(four)] if four.exists() else [])))
    c = Client(["--embedded", "--workspace", str(WORK / "ws"), "--allow-read", str(common), "--allow-write", str(common)])
    c.initialize()

    def call(name, args):
        r = c.call(name, args)["result"]["structuredContent"]
        if not r.get("ok"):
            raise SystemExit(f"{name}: {json.dumps(r.get('error'))[:800]}")
        return r["value"]

    def wait(jid):
        while True:
            st = c.call("job_status", {"job_id": jid, "wait_seconds": 10})["result"]["structuredContent"]["value"]
            if st["state"] not in ("queued", "running"):
                return st

    runs = json.load(open(WORK / "runs.json")) if (WORK / "runs.json").exists() else {"studies": {}, "full_integration": {}}
    sizes = ["2.5 mm", "1.25 mm", "0.625 mm"]
    for name, designs, nu in (("cantilevers", ["C10", "C20"], 0.3), ("t1_nu0", ["C10", "C20"], 0.0), ("t2_t3", ["C10L", "C10W"], 0.3)):
        d = WORK / name
        if not (d / "evidence.json").exists():
            t0 = time.time()
            v = call("study_run", {"definition": cantilever_study(name, designs, nu, sizes), "directory": str(d)})
            st = wait(v["job_id"])
            print(f"{name}: {st['state']} in {time.time() - t0:.1f} s", flush=True)
        ev = json.load(open(d / "evidence.json"))
        runs["studies"][name] = {dd["name"]: [{"element_size_mm": lv["element_size_mm"], "run_directory": str(d / lv["run"]["run_directory"]),
                                               "load_region_displacement_mm": lv["quantities"]["load_region_displacement_mm"],
                                               "strain_energy_j": None} for lv in dd["levels"]] for dd in ev["designs"]}
    # full integration twins
    targets = [("C10", WORK / "cantilevers/designs/C10", [1.25, 0.625])]
    if four.exists():
        targets += [("A", four / "designs/A", [1.0]), ("B", four / "designs/B", [1.0, 2.0])]
    for name, proj, hs in targets:
        call("project_open", {"path": str(proj)})
        for h in hs:
            key = f"{name}@{h}"
            if runs["full_integration"].get(key, {}).get("state") == "succeeded":
                continue
            call("mesh_generate", {"element_size": f"{h} mm", "max_elements": 450000})
            v = call("analysis_run", {"analysis": "static_structural", "formulation": "full_integration", "allow_duplicate": True, "label": f"evaluation full integration {key}"})
            st = wait(v["job_id"])
            runs["full_integration"][key] = {"run_directory": v["run_directory"], "state": st["state"], "solver": st.get("summary", {}).get("solver", {}).get("method")}
            print(key, st["state"], v["run_directory"], flush=True)
            json.dump(runs, open(WORK / "runs.json", "w"), indent=1)
    json.dump(runs, open(WORK / "runs.json", "w"), indent=1)
    c.close()


if __name__ == "__main__":
    main()
