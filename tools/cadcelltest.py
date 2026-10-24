#!/usr/bin/env python3
"""cadcelltest.py - GOALS.md G23 step 5a (H3 to H7) and 5b: a lattice core's cell homogenised, through build/openphysics-cad
and openphysics-mcp, and its prediction for printed gyroids against measurement. H1 and H2 are tools/cellstifftest.c.

    make build/openphysics-cad openphysics-mcp && python3 tools/cadcelltest.py      (make test-cad runs it; about six minutes)

Criteria, written 2026-10-08 before the first run (GOALS.md G23 step 5), amended only openly below them:
H3 cubic symmetry of the gyroid, Schwarz P and diamond, each as a band sheet and as a network at a solid fraction of
   0.3: the three normal terms, the three normal-normal terms and the three shear terms each within 1 per cent of their
   means, every other term below 1 per cent of C11
H4 the band gyroid's cube-axis modulus at 32, 48 and 64 voxels per cell, the change from 48 to 64 below 2 per cent
H5 every cell at or below the Voigt bound: f C_s - C positive semidefinite (its least eigenvalue at least -1e-9 C11_s),
   with f the voxels' mean solid fraction; the Hashin-Shtrikman upper bound of a porous solid on the bulk modulus,
   4 G K f / (4 G + 3 K (1 - f)), reported beside each, not a criterion
H6 against an independent solution: Li, Liao, Dai and Xie (Materials 12:2183, 2019, doi 10.3390/ma12132183, CC BY 4.0)
   homogenised in ABAQUS the band sheet gyroid (G^2 < t^2) and the network gyroid (G < t) at a solid fraction of 0.3
   with nu 0.33: E_H = 63.67 and 40.56 MPa on E_s = 850.1 MPa (Table 1), so E_H / E_s = 0.07490 and 0.04771; ours
   within 5 per cent of each
H7 cad_cell through openphysics-mcp gives the program's numbers
5b the same paper's SLA specimens (Formlabs Form 2, 36 mm cubes of 6 x 6 x 6 cells of 6 mm at a solid fraction of 0.3,
   built and compressed along z at 10 mm/min, ASTM D695-15; the resin measured at E_s = 850.1 +- 15.2 MPa, nu 0.33)
   measured 62.51 +- 0.44 MPa (sheet) and 39.21 +- 0.23 MPa (network) (Table 1). Prediction: E_s times our cube-axis
   modulus fraction at 64 voxels, an infinite lattice with no correction for the cubes' edges, written down before it
   is compared, then scored as the signed error against each measurement; criterion: within 10 per cent of both

First run, 2026-10-08: H3, H4, H5 and H7 passed; H6 failed (+57.8 and +46.7 per cent against Li et al.'s ABAQUS values)
and 5b failed (+60.7 and +51.7 per cent against the measured cubes). The criteria stand; GOALS.md G23 5c follows it up.
"""
import json, math, os, pathlib, shutil, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor

ROOT = pathlib.Path(__file__).resolve().parents[1]
CAD = ROOT / "build" / "openphysics-cad"
MCP = ROOT / "openphysics-mcp"
NU = 0.33
failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)
    return ok


def cell(tmp, kind, form, voxels, fraction=0.3, cell_mm=6):
    req = {"core": {"kind": kind, "form": form, "cell_mm": cell_mm, "solid_fraction": fraction}, "poisson_ratio": NU, "voxels": voxels}
    f = tmp / ("%s-%s-%d.json" % (kind, form, voxels))
    f.write_text(json.dumps(req))
    t0 = time.time()
    r = subprocess.run([str(CAD), "cell", str(f)], capture_output=True, text=True, timeout=1800)
    lines = [l for l in r.stdout.splitlines() if l.startswith("{")]
    rec = json.loads(lines[-1]) if lines else {"ok": False, "error": {"message": (r.stderr or r.stdout)[-300:]}}
    rec["_wall_s"] = time.time() - t0
    rec["_request"] = req
    return rec


def symmetric_ok(C):
    """the three normal, normal-normal and shear terms each within 1 % of their means, the rest below 1 % of C11
    (the solver's order: xx, yy, zz, xy, yz, zx)"""
    n = [C[0], C[7], C[14]]
    nn = [C[1], C[2], C[8]]
    sh = [C[21], C[28], C[35]]
    worst = 0.0
    for g in (n, nn, sh):
        m = sum(g) / 3
        worst = max(worst, max(abs(x - m) / abs(m) for x in g))
    named = {0, 7, 14, 1, 2, 8, 6, 12, 13, 21, 28, 35}
    rest = max(abs(C[i]) for i in range(36) if i not in named)
    return worst <= 0.01 and rest <= 0.01 * C[0], worst, rest / C[0]


class Mcp:
    def __init__(self, tmp, lean):
        args = [str(MCP), "--embedded", "--workspace", str(tmp / "ws"), "--lab", str(ROOT), "--lab-results", str(tmp / "res")]
        self.p = subprocess.Popen(args + (["--lean"] if lean else []), cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True)
        self.n = 0
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "cadcelltest", "version": "1"}})
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.p.stdin.flush()

    def rpc(self, method, params=None):
        self.n += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params or {}}) + "\n")
        self.p.stdin.flush()
        while True:
            m = json.loads(self.p.stdout.readline())
            if m.get("id") == self.n:
                return m

    def call(self, name, args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})["result"]
        return r.get("structuredContent") or json.loads(r["content"][-1]["text"])

    def stop(self):
        self.p.kill()
        self.p.wait()


def main():
    if not CAD.exists():
        print("cadcelltest: build/openphysics-cad is not built (make build/openphysics-cad)")
        return 2
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-cadcell-"))
    try:
        jobs = [("gyroid", "band", 64), ("gyroid", "network", 64), ("gyroid", "band", 48), ("gyroid", "band", 32),
                ("gyroid", "network", 32), ("schwarz_p", "band", 32), ("schwarz_p", "network", 32), ("diamond", "band", 32),
                ("diamond", "network", 32)]
        t0 = time.time()
        with ThreadPoolExecutor(max_workers=4) as pool:
            recs = dict(zip(jobs, pool.map(lambda j: cell(tmp, *j), jobs)))
        print("(%d cells in %.0f s)" % (len(jobs), time.time() - t0))
        for j, r in recs.items():
            if not r.get("ok"):
                check(False, "%s %s at %d voxels: %s" % (j[0], j[1], j[2], (r.get("error") or {}).get("message")))

        print("H3 cubic symmetry, at 32 voxels")
        for kind in ("gyroid", "schwarz_p", "diamond"):
            for form in ("band", "network"):
                r = recs[(kind, form, 32)]
                if not r.get("ok"):
                    continue
                ok, worst, rest = symmetric_ok(r["stiffness_over_Es"])
                cu = r["cubic"]
                check(ok, "%s %s: fraction %.4f, E/Es %.5f, Zener %.3f; terms within %.2e of their means, the rest %.2e of C11"
                      % (kind, form, r["solid_fraction"], cu["modulus_over_Es"], cu["zener_ratio"], worst, rest))

        print("H4 convergence of the band gyroid")
        e = {n: recs[("gyroid", "band", n)].get("cubic", {}).get("modulus_over_Es", float("nan")) for n in (32, 48, 64)}
        change = abs(e[64] - e[48]) / e[64]
        check(change < 0.02, "E/Es %.5f, %.5f, %.5f at 32, 48, 64 voxels; 48 to 64 %.2f %%" % (e[32], e[48], e[64], 100 * change))

        print("H5 the Voigt bound; Hashin-Shtrikman's beside it")
        c11s = (1 - NU) / ((1 + NU) * (1 - 2 * NU))
        for j, r in recs.items():
            if not r.get("ok"):
                continue
            b = r["bounds"]
            check(b["voigt_least_eigenvalue_over_Es"] >= -1e-9 * c11s,
                  "%s %s %d: least eigenvalue of f Cs - C %.3e; bulk %.5f, Hashin-Shtrikman %.5f (%.0f %%), %.0f s"
                  % (j[0], j[1], j[2], b["voigt_least_eigenvalue_over_Es"], r["cubic"]["bulk_over_Es"], b["hashin_shtrikman_bulk_over_Es"],
                     100 * r["cubic"]["bulk_over_Es"] / b["hashin_shtrikman_bulk_over_Es"], r["_wall_s"]))

        print("H6 against Li, Liao, Dai and Xie 2019's homogenisation (ABAQUS)")
        sheet, net = recs[("gyroid", "band", 64)], recs[("gyroid", "network", 64)]
        for name, r, ref in (("band sheet", sheet, 63.67 / 850.1), ("network", net, 40.56 / 850.1)):
            v = r.get("cubic", {}).get("modulus_over_Es", float("nan"))
            check(abs(v - ref) / ref <= 0.05, "%s gyroid at %.4f: E/Es %.5f against %.5f (%+.1f %%)" % (name, r.get("solid_fraction", float("nan")), v, ref, 100 * (v - ref) / ref))

        print("5b against the printed specimens")
        es = 850.1
        pred = {"sheet": es * sheet.get("cubic", {}).get("modulus_over_Es", float("nan")),
                "network": es * net.get("cubic", {}).get("modulus_over_Es", float("nan"))}
        print("    prediction, written before the comparison: sheet %.2f MPa, network %.2f MPa (E_s 850.1 MPa times the infinite"
              " lattice's cube-axis modulus at 64 voxels)" % (pred["sheet"], pred["network"]))
        for name, meas, sd in (("sheet", 62.51, 0.44), ("network", 39.21, 0.23)):
            err = (pred[name] - meas) / meas
            check(abs(err) <= 0.10, "%s: predicted %.2f MPa against measured %.2f +- %.2f MPa: %+.1f %%" % (name, pred[name], meas, sd, 100 * err))

        print("H7 through openphysics-mcp")
        if MCP.exists():
            servers = []
            try:
                for lean in (False, True):
                    m = Mcp(tmp, lean)
                    servers.append(m)
                    names = {t["name"] for t in m.rpc("tools/list")["result"]["tools"]}
                    check("cad_cell" in names, "%s profile offers cad_cell" % ("lean" if lean else "default"))
                r = servers[0].call("cad_cell", recs[("gyroid", "band", 32)]["_request"])
                v = r.get("value") or {}
                same = r.get("ok") and v.get("stiffness_over_Es") == recs[("gyroid", "band", 32)]["stiffness_over_Es"]
                check(bool(same), "cad_cell: the band gyroid at 32 voxels, the same matrix as openphysics-cad's (%s)" % (r.get("error") or {}).get("message", ""))
            finally:
                for s in servers:
                    s.stop()
        else:
            check(False, "H7 needs openphysics-mcp (make openphysics-mcp)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("cadcelltest: %s" % ("all checks passed" if not failures else "%d failed" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
