#!/usr/bin/env python3
"""printmesh_ccx.py - an independent solution of printmesh_study's C-ring (GOALS.md G17 step 4), with CalculiX.

The same ring (inner radius 10 mm, outer 14 mm, 4 mm wide, a gap of 0.25 rad), one tip held and 10 N pulling the other
along the line through the two tips, on a structured mesh of 20-node hexahedra (C3D20R) in (r, theta, z) whose mid-side
nodes lie on the true circles, solved by CalculiX (ccx 2.23, used as a cross-check; nothing of it is copied into the
repository). The material is pla_fff as the library's rules build it (E1 = E2 along the layers, E3 across them, Poisson
from the record, Saint-Venant's G13); its layer plane is isotropic, so the ring needs no turn. The script prints the hoop
stress across the apex section (at mid-width and at the faces) and the Tsai-Wu strength ratio at every node, read from
the nodal stresses CalculiX extrapolates and averages, beside the curved-beam closed form.

    python3 tools/printmesh_ccx.py [--nr 8 --ntheta 160 --nz 8] [--iso]
"""
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CCX = Path(os.environ.get("CCX") or next((p for p in (Path.home() / ".openphysicsai/eval-tools/ccx-env/bin/ccx",
                                                       Path.home() / ".navier-eval-tools/ccx-env/bin/ccx") if p.exists()),
                                           Path.home() / ".openphysicsai/eval-tools/ccx-env/bin/ccx"))  # the second: before 2026-10-21
RI, RO, WIDTH, HALF_GAP, FORCE = 10.0, 14.0, 4.0, 0.125, 10.0


def arg(name, default):
    return type(default)(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv[:-1] else default


def library(mid):
    data = json.loads((ROOT / "src/ctl/materials.json").read_text())
    rec = next(r for r in (data["materials"] if isinstance(data, dict) else data) if r["id"] == mid)
    v = lambda k: rec[k]["value"] if isinstance(rec[k], dict) else rec[k]
    e1, e3 = v("youngs_modulus_along_layers_pa") / 1e6, v("youngs_modulus_across_layers_pa") / 1e6
    nu = v("poisson_ratio") if "poisson_ratio" in rec else 0.35
    xt, zt = v("tensile_strength_along_layers_pa") / 1e6, v("tensile_strength_across_layers_pa") / 1e6
    g12, g13 = e1 / (2 * (1 + nu)), e1 * e3 / (e1 + e3 + 2 * nu * e3)
    return {"E1": e1, "E3": e3, "nu": nu, "G12": g12, "G13": g13, "Xt": xt, "Zt": zt}


def tsai_wu_ratio(s, m):
    """the library's rules: compression equal to tension, S12 = Xt / sqrt 3, S23 = S31 = Zt / sqrt 3, interaction -1/2"""
    x, z = m["Xt"], m["Zt"]
    f11, f33, f66, f44 = 1 / x**2, 1 / z**2, 3 / x**2, 3 / z**2
    f12, f13 = -0.5 / x**2, -0.5 / (x * z)
    s11, s22, s33, s12, s13, s23 = s
    q = (f11 * (s11**2 + s22**2) + f33 * s33**2 + 2 * f12 * s11 * s22 + 2 * f13 * (s11 + s22) * s33 + f66 * s12**2 + f44 * (s13**2 + s23**2))
    return 1 / math.sqrt(q) if q > 0 else math.inf


def closed_form(xt):
    area, rbar = (RO - RI) * WIDTH, 0.5 * (RI + RO)
    rn = (RO - RI) / math.log(RO / RI)
    e, moment = rbar - rn, FORCE * rbar * (1 + math.cos(HALF_GAP))
    hoop = lambda r: FORCE / area + moment * (rn - r) / (area * e * r)
    return hoop, xt / hoop(RI)


def main():
    nr, nth, nz = arg("--nr", 8), arg("--ntheta", 160), arg("--nz", 8)
    nth += nth % 2  # the apex is then a row of corner nodes
    iso = "--iso" in sys.argv
    m = library("pla_fff")
    t0, t1 = -math.pi / 2 + HALF_GAP, 1.5 * math.pi - HALF_GAP
    ids, coords = {}, []
    for i2 in range(2 * nr + 1):
        for j2 in range(2 * nth + 1):
            for k2 in range(2 * nz + 1):
                if (i2 % 2) + (j2 % 2) + (k2 % 2) > 1:
                    continue
                r, th, z = RI + (RO - RI) * i2 / (2 * nr), t0 + (t1 - t0) * j2 / (2 * nth), WIDTH * k2 / (2 * nz)
                ids[(i2, j2, k2)] = len(coords) + 1
                coords.append((r * math.cos(th), r * math.sin(th), z))
    elems = []
    for i in range(nr):
        for j in range(nth):
            for k in range(nz):
                I, J, K = 2 * i, 2 * j, 2 * k
                local = [(I, J, K), (I + 2, J, K), (I + 2, J + 2, K), (I, J + 2, K), (I, J, K + 2), (I + 2, J, K + 2), (I + 2, J + 2, K + 2),
                         (I, J + 2, K + 2), (I + 1, J, K), (I + 2, J + 1, K), (I + 1, J + 2, K), (I, J + 1, K), (I + 1, J, K + 2),
                         (I + 2, J + 1, K + 2), (I + 1, J + 2, K + 2), (I, J + 1, K + 2), (I, J, K + 1), (I + 2, J, K + 1), (I + 2, J + 2, K + 1),
                         (I, J + 2, K + 1)]
                elems.append([ids[q] for q in local])
    # the pulled tip (j2 = 0): consistent loads of a uniform traction on each flat 8-node face, -1/12 at corners, 1/3 at mid-sides
    load = {}
    dr, dz = (RO - RI) / nr, WIDTH / nz
    traction = FORCE / ((RO - RI) * WIDTH)
    for i in range(nr):
        for k in range(nz):
            I, K = 2 * i, 2 * k
            fa = traction * dr * dz
            for q in [(I, 0, K), (I + 2, 0, K), (I + 2, 0, K + 2), (I, 0, K + 2)]:
                load[ids[q]] = load.get(ids[q], 0) - fa / 12
            for q in [(I + 1, 0, K), (I + 2, 0, K + 1), (I + 1, 0, K + 2), (I, 0, K + 1)]:
                load[ids[q]] = load.get(ids[q], 0) + fa / 3
    held = [n for (i2, j2, k2), n in ids.items() if j2 == 2 * nth]
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-printmesh-ccx-"))
    lines = ["*NODE, NSET=NALL"] + ["%d, %.12g, %.12g, %.12g" % (n + 1, *c) for n, c in enumerate(coords)]
    lines += ["*ELEMENT, TYPE=C3D20R, ELSET=EALL"]
    for e, nodes in enumerate(elems):
        lines += ["%d, %s," % (e + 1, ", ".join(str(n) for n in nodes[:15])), ", ".join(str(n) for n in nodes[15:])]
    lines += ["*NSET, NSET=HELD"] + [", ".join(str(n) for n in held[a:a + 12]) for a in range(0, len(held), 12)]
    lines += ["*MATERIAL, NAME=PRINTED"]
    if iso:
        lines += ["*ELASTIC", "%.9g, %.9g" % (m["E1"], m["nu"])]
    else:  # axis 1 = x, 2 = y, 3 = z (the build direction): a transversely isotropic solid about z
        lines += ["*ELASTIC, TYPE=ENGINEERING CONSTANTS", "%.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g" %
                  (m["E1"], m["E1"], m["E3"], m["nu"], m["nu"], m["nu"], m["G12"], m["G13"]), "%.9g" % m["G13"]]
    lines += ["*SOLID SECTION, ELSET=EALL, MATERIAL=PRINTED", "*STEP", "*STATIC", "*BOUNDARY", "HELD, 1, 3, 0", "*CLOAD"]
    lines += ["%d, 1, %.12g" % (n, f) for n, f in sorted(load.items())]
    lines += ["*NODE FILE", "S", "*END STEP", ""]
    (tmp / "ring.inp").write_text("\n".join(lines))
    run = subprocess.run([str(CCX), "-i", "ring"], cwd=tmp, capture_output=True, text=True, timeout=1800)
    if run.returncode or not (tmp / "ring.frd").exists():
        print(run.stdout[-2000:], run.stderr[-2000:])
        print("ccx failed; workspace kept: %s" % tmp)
        return 2
    stress, block = {}, None
    for line in (tmp / "ring.frd").read_text().splitlines():  # the nodal stresses CalculiX extrapolates and averages
        if line.startswith(" -4"):
            block = line.split()[1]
        elif line.startswith(" -1") and block == "STRESS":
            v = [float(line[13 + 12 * k:25 + 12 * k]) for k in range(6)]  # SXX SYY SZZ SXY SYZ SZX
            stress[int(line[3:13])] = [v[0], v[1], v[2], v[3], v[5], v[4]]  # as sxx syy szz sxy sxz syz
    hoop_cf, r_cf = closed_form(m["Xt"])
    apex = nth  # j2 of the apex section: theta = 90 degrees
    print("C-ring in CalculiX: %d C3D20R elements, %d nodes, %s material (E1 %.4g MPa, E3 %.4g MPa, nu %.3g)" %
          (len(elems), len(coords), "isotropic" if iso else "pla_fff printed", m["E1"], m["E3"], m["nu"]))
    print("hoop stress across the apex section (MPa), CalculiX / Winkler-Bach:")
    for k2 in (0, nz // 2, nz, 3 * nz // 2, 2 * nz):
        row = []
        for i2 in (0, 1, 2, nr, 2 * nr):
            s = stress[ids[(i2, apex, k2)]]
            r = RI + (RO - RI) * i2 / (2 * nr)
            row.append("r %.2f: %.2f/%.2f" % (r, s[0], hoop_cf(r)))  # at theta = 90 degrees the hoop direction is x (sign free)
        print("  z %.2f  %s" % (WIDTH * k2 / (2 * nz), "  ".join(row)))
    best, where = math.inf, None
    for (i2, j2, k2), n in ids.items():
        s = stress.get(n)
        if s is None:
            continue
        rr = tsai_wu_ratio([s[0], s[1], s[2], s[3], s[4], s[5]], m)
        if rr < best and j2 not in (0, 2 * nth):  # the two tip faces carry the load and the support
            best, where = rr, (i2, j2, k2)
    i2, j2, k2 = where
    th = t0 + (t1 - t0) * j2 / (2 * nth)
    print("smallest Tsai-Wu strength ratio at a node: %.5g at r %.3f mm, %.2f deg from the apex, z %.3f mm; closed form %.5g (%+.1f %%)" %
          (best, RI + (RO - RI) * i2 / (2 * nr), math.degrees(th) - 90, WIDTH * k2 / (2 * nz), r_cf, 100 * (best / r_cf - 1)))
    mid = min(tsai_wu_ratio(stress[ids[(0, j, nz)]], m) for j in range(2, 2 * nth - 1, 2))
    print("smallest at mid-width of the inner fibre: %.5g (%+.1f %% from the closed form)" % (mid, 100 * (mid / r_cf - 1)))
    if "--keep" not in sys.argv:
        shutil.rmtree(tmp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
