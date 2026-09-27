#!/usr/bin/env python3
"""x2_ccx.py - X2 and T4: a cantilever built for CalculiX from the written specification (criteria: ../CRITERIA-independent.md)

    python3 x2_ccx.py NAVIER_WORK_DIR OUT_DIR

Nothing here reads NAVIER's mesh, supports or loads. The deck is generated from the text of the specification:
steel E = 200 GPa, nu = 0.3; L = 100 mm along x, b = 10 mm along y, h = 10 mm along z; root face x = 0; a total force of
10 N along -z as a uniform traction on the tip face x = L. Units in the deck: N, mm, MPa.

NAVIER's stored results are read only for the comparison: NAVIER_WORK_DIR/cantilevers/evidence.json (design C10).

Cases (pre-registered):
  clamped C3D20R 20x2x2, 40x4x4, 80x8x8 and C3D8I 160x16x16 (X2a, X2b, X2c; the "clamped C3D20R" of the beam reading)
  sliding C3D20R at the same three meshes (T4): u_x = 0 on the whole root face, u_z = 0 on the root nodes of the line
  z = h/2, u_y = 0 on the root nodes of the line y = b/2
Supplementary cases (added after the criteria were written; reported separately, not used for any criterion):
  s1_nu0      clamped C3D20R 80x8x8 with nu = 0
  s2_minimal  C3D20R 80x8x8, u_x = 0 on the root face, u_y = 0 on the root line y = b/2, u_z = 0 at the single root
              node (y, z) = (b/2, h/2): the root section may contract and curve freely in its plane
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

CCX = os.environ.get("CCX", str(Path.home() / ".navier-eval-tools/ccx-env/bin/ccx"))
L, B, H = 100.0, 10.0, 10.0
E_MPA, FORCE_N = 200e3, 10.0


def timoshenko(nu, kappa=None):
    g = E_MPA / (2 * (1 + nu))
    kappa = 5 / 6 if kappa is None else kappa
    return FORCE_N * L ** 3 / (3 * E_MPA * B * H ** 3 / 12) + FORCE_N * L / (kappa * g * B * H)


def cowper(nu):
    return 10 * (1 + nu) / (12 + 11 * nu)


def deck(path, nx, ny, nz, element, root, nu):
    """Structured brick mesh on a fine index grid (2nx+1)(2ny+1)(2nz+1); quadratic elements use nodes with at most one odd
    index, linear elements only the all-even nodes. Returns the tip weights for the area-weighted mean."""
    quadratic = element.startswith("C3D20")
    nxf, nyf = 2 * nx + 1, 2 * ny + 1

    def nid(i, j, k):
        return 1 + i + nxf * (j + nyf * k)

    def exists(i, j, k):
        odd = (i % 2) + (j % 2) + (k % 2)
        return odd <= 1 if quadratic else odd == 0

    lines = ["*HEADING", f"independent cantilever {element} {nx}x{ny}x{nz} root={root} nu={nu}", "*NODE"]
    nodes = []
    for k in range(2 * nz + 1):
        for j in range(2 * ny + 1):
            for i in range(2 * nx + 1):
                if exists(i, j, k):
                    nodes.append((i, j, k))
                    lines.append(f"{nid(i, j, k)}, {i * L / (2 * nx):.12g}, {j * B / (2 * ny):.12g}, {k * H / (2 * nz):.12g}")
    corner = [(0, 0, 0), (2, 0, 0), (2, 2, 0), (0, 2, 0), (0, 0, 2), (2, 0, 2), (2, 2, 2), (0, 2, 2)]
    mid = [(1, 0, 0), (2, 1, 0), (1, 2, 0), (0, 1, 0), (1, 0, 2), (2, 1, 2), (1, 2, 2), (0, 1, 2), (0, 0, 1), (2, 0, 1), (2, 2, 1), (0, 2, 1)]
    local = corner + (mid if quadratic else [])
    lines.append(f"*ELEMENT, TYPE={element}, ELSET=EALL")
    eid = 0
    for k in range(nz):
        for j in range(ny):
            for i in range(nx):
                eid += 1
                ids = [nid(2 * i + a, 2 * j + b, 2 * k + c) for a, b, c in local]
                # CalculiX accepts at most 16 entries per data line: continue on the next line
                row = [str(eid)] + [str(n) for n in ids]
                lines.append(", ".join(row[:16]) + ("," if len(row) > 16 else ""))
                if len(row) > 16:
                    lines.append(", ".join(row[16:]))
    root_nodes = [n for n in nodes if n[0] == 0]
    tip_nodes = [n for n in nodes if n[0] == 2 * nx]

    def nset(name, members):
        out = [f"*NSET, NSET={name}"]
        ids = [str(nid(*n)) for n in members]
        out += [", ".join(ids[s:s + 16]) for s in range(0, len(ids), 16)]
        return out

    lines += nset("NROOT", root_nodes) + nset("NTIP", tip_nodes)
    lines += ["*MATERIAL, NAME=STEEL", "*ELASTIC", f"{E_MPA:.12g}, {nu:.12g}", "*SOLID SECTION, ELSET=EALL, MATERIAL=STEEL", "*STEP", "*STATIC", "*BOUNDARY"]
    if root == "clamped":
        lines.append("NROOT, 1, 3")
    elif root == "sliding":
        lines.append("NROOT, 1, 1")
        lines += [f"{nid(*n)}, 3, 3" for n in root_nodes if n[2] == nz]
        lines += [f"{nid(*n)}, 2, 2" for n in root_nodes if n[1] == ny]
    elif root == "minimal":
        lines.append("NROOT, 1, 1")
        lines += [f"{nid(*n)}, 2, 2" for n in root_nodes if n[1] == ny]
        lines.append(f"{nid(0, ny, nz)}, 3, 3")
    else:
        raise ValueError(root)
    # consistent nodal forces of a uniform traction: 8-node faces corner -1/12, mid-side +1/3; 4-node faces 1/4 per corner
    weights = {}
    face_share = 1.0 / (ny * nz)
    for k in range(nz):
        for j in range(ny):
            j0, k0 = 2 * j, 2 * k
            corners = [(j0, k0), (j0 + 2, k0), (j0 + 2, k0 + 2), (j0, k0 + 2)]
            mids = [(j0 + 1, k0), (j0 + 2, k0 + 1), (j0 + 1, k0 + 2), (j0, k0 + 1)]
            for jj, kk in corners:
                weights[nid(2 * nx, jj, kk)] = weights.get(nid(2 * nx, jj, kk), 0.0) + face_share * (-1 / 12 if quadratic else 1 / 4)
            if quadratic:
                for jj, kk in mids:
                    weights[nid(2 * nx, jj, kk)] = weights.get(nid(2 * nx, jj, kk), 0.0) + face_share / 3
    lines.append("*CLOAD")
    lines += [f"{n}, 3, {-FORCE_N * w:.17g}" for n, w in sorted(weights.items())]
    lines += ["*NODE PRINT, NSET=NTIP", "U", "*NODE PRINT, NSET=NROOT, TOTALS=ONLY", "RF", "*EL PRINT, ELSET=EALL, TOTALS=ONLY", "ELSE", "*END STEP", ""]
    path.write_text("\n".join(lines))
    return {"nodes": len(nodes), "elements": eid, "tip_weights": weights, "weight_sum": sum(weights.values()),
            "tip_nodes": len(tip_nodes), "root_nodes": len(root_nodes)}


def parse_dat(path):
    sections, title = {}, None
    for line in path.read_text(errors="replace").splitlines():
        s = line.strip()
        if not s:
            continue
        if " for set " in s and " and time " in s:
            title = s.split(" and time ")[0]
            sections[title] = []
        elif title is not None:
            try:
                sections[title].append([float(x) for x in s.split()])
            except ValueError:
                title = None
    return sections


def run_case(out, name, nx, ny, nz, element, root, nu):
    d = out / name
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    info = deck(d / "model.inp", nx, ny, nz, element, root, nu)
    env = dict(os.environ, OMP_NUM_THREADS="4", CCX_NPROC_STIFFNESS="4", CCX_NPROC_EQUATION_SOLVER="4")
    t0 = time.time()
    with open(d / "ccx.log", "w") as log:
        rc = subprocess.run([CCX, "-i", "model"], cwd=d, stdout=log, stderr=subprocess.STDOUT, env=env).returncode
    secs = time.time() - t0
    row = {"case": name, "element": element, "mesh": [nx, ny, nz], "root": root, "nu": nu, "element_size_mm": L / nx,
           "nodes": info["nodes"], "elements": info["elements"], "ccx_rc": rc, "ccx_seconds": secs}
    if rc == 0:
        sec = parse_dat(d / "model.dat")
        disp = {int(r[0]): r[1:4] for r in sec["displacements (vx,vy,vz) for set NTIP"]}
        w = info["tip_weights"]
        assert set(disp) == set(w) or set(w) <= set(disp)
        u_w = sum(w[n] * -disp[n][2] for n in w)
        u_nodal = sum(-v[2] for v in disp.values()) / len(disp)
        rf = sec["total force (fx,fy,fz) for set NROOT"][0]
        energy = sec["total internal energy for set EALL"][0][0]
        messages = [ln.strip() for ln in (d / "ccx.log").read_text(errors="replace").splitlines()
                    if any(k in ln.lower() for k in ("warning", "error", "singular", "zero pivot", "not connected"))]
        row.update({"status": "solved", "tip_mean_uz_area_weighted_mm": u_w, "tip_mean_uz_nodal_mm": u_nodal,
                    "reaction_n": rf, "reaction_balance": abs(rf[2] - FORCE_N) / FORCE_N,
                    "reaction_print_resolution": 0.5e-6 * abs(rf[2]) / FORCE_N,
                    "strain_energy_nmm": energy, "energy_displacement_mm": 2 * energy / FORCE_N,
                    "energy_vs_work": 2 * energy / (FORCE_N * u_w), "weight_sum": info["weight_sum"], "ccx_messages": messages[:5]})
    else:
        row["status"] = "ccx_failed"
    inp, dat = d / "model.inp", d / "model.dat"
    hashes = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in (inp, dat) if f.exists()}
    with open(inp, "rb") as src, gzip.open(d / "model.inp.gz", "wb", compresslevel=9) as dst:
        shutil.copyfileobj(src, dst)
    (d / "SHA256.json").write_text(json.dumps(hashes, indent=1))
    for f in d.iterdir():
        if f.name not in ("model.inp.gz", "model.dat", "model.sta", "ccx.log", "SHA256.json"):
            f.unlink()
    (d / "result.json").write_text(json.dumps(row, indent=1))
    print(json.dumps({k: v for k, v in row.items() if k != "ccx_messages"}), flush=True)
    return row


def main():
    work, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for m in (20, 40, 80):
        rows.append(run_case(out, f"clamped_C3D20R_{m}x{m // 10}x{m // 10}", m, m // 10, m // 10, "C3D20R", "clamped", 0.3))
    rows.append(run_case(out, "clamped_C3D8I_160x16x16", 160, 16, 16, "C3D8I", "clamped", 0.3))
    for m in (20, 40, 80):
        rows.append(run_case(out, f"sliding_C3D20R_{m}x{m // 10}x{m // 10}", m, m // 10, m // 10, "C3D20R", "sliding", 0.3))
    rows.append(run_case(out, "s1_nu0_clamped_C3D20R_80x8x8", 80, 8, 8, "C3D20R", "clamped", 0.0))
    rows.append(run_case(out, "s2_minimal_C3D20R_80x8x8", 80, 8, 8, "C3D20R", "minimal", 0.3))

    ev = json.loads((work / "cantilevers/evidence.json").read_text())
    c10 = next(dd for dd in ev["designs"] if dd["name"] == "C10")
    nav = {lv["element_size_mm"]: lv["quantities"] for lv in c10["levels"]}
    h_fine = min(nav)
    nav_u = nav[h_fine]["load_region_displacement_mm"]
    nav_rf = nav[h_fine]["reaction_force_n"][2]
    t56, tcw = timoshenko(0.3), timoshenko(0.3, cowper(0.3))
    by = {r["case"]: r for r in rows}
    cl, sl = by["clamped_C3D20R_80x8x8"], by["sliding_C3D20R_80x8x8"]
    i8 = by["clamped_C3D8I_160x16x16"]

    def rel(a, b):
        return (a - b) / b

    crit = {
        "X2a": {"criterion": "finest C3D20R tip-face mean within 0.5 % of NAVIER's finest", "ccx_mm": cl.get("tip_mean_uz_area_weighted_mm"),
                "navier_mm": nav_u, "navier_element_size_mm": h_fine, "relative": rel(cl["tip_mean_uz_area_weighted_mm"], nav_u)},
        "X2b": {"criterion": "reaction equals 10 N within 1e-9 relative in both codes",
                "navier_balance": abs(nav_rf - FORCE_N) / FORCE_N,
                "ccx_balance_as_printed": {r["case"]: r.get("reaction_balance") for r in rows if r["root"] == "clamped" and r["nu"] == 0.3},
                "ccx_print_resolution": "CalculiX prints totals with 7 significant digits (resolution 5e-7 of 10 N); a 1e-9 balance cannot be read from its output"},
        "X2c": {"criterion": "independent C3D8I at 0.625 mm within 2 % of NAVIER at 0.625 mm", "ccx_mm": i8.get("tip_mean_uz_area_weighted_mm"),
                "navier_mm": nav.get(0.625, {}).get("load_region_displacement_mm"),
                "relative": rel(i8["tip_mean_uz_area_weighted_mm"], nav[0.625]["load_region_displacement_mm"]) if 0.625 in nav else None},
        "B_clamped": {"criterion": "clamped C3D20R within 0.2 % of NAVIER (H1 reading); > 0.5 % would indicate H2 or H3",
                      "relative": rel(cl["tip_mean_uz_area_weighted_mm"], nav_u)},
        "B_T4": {"criterion": "sliding-root C3D20R within 0.2 % of Timoshenko", "ccx_mm": sl.get("tip_mean_uz_area_weighted_mm"),
                 "timoshenko_kappa_5_6_mm": t56, "timoshenko_cowper_mm": tcw,
                 "relative_to_5_6": rel(sl["tip_mean_uz_area_weighted_mm"], t56), "relative_to_cowper": rel(sl["tip_mean_uz_area_weighted_mm"], tcw)},
    }
    table = []
    for r in rows:
        if r["status"] != "solved":
            table.append({"case": r["case"], "status": r["status"]})
            continue
        ref = timoshenko(r["nu"])
        table.append({"case": r["case"], "tip_mean_mm": r["tip_mean_uz_area_weighted_mm"], "vs_timoshenko_5_6": rel(r["tip_mean_uz_area_weighted_mm"], ref),
                      "vs_navier_finest": rel(r["tip_mean_uz_area_weighted_mm"], nav_u) if r["nu"] == 0.3 and r["root"] == "clamped" else None,
                      "nodal_mean_mm": r["tip_mean_uz_nodal_mm"], "energy_vs_work": r["energy_vs_work"], "reaction_balance_printed": r["reaction_balance"],
                      "ccx_seconds": r["ccx_seconds"], "ccx_messages": r["ccx_messages"]})
    summary = {"specification": {"L_mm": L, "b_mm": B, "h_mm": H, "E_mpa": E_MPA, "force_n": FORCE_N},
               "navier": {"study": str(work / "cantilevers"), "levels": {str(k): v["load_region_displacement_mm"] for k, v in sorted(nav.items())}},
               "timoshenko": {"nu_0.3_kappa_5_6": t56, "nu_0.3_cowper": tcw, "nu_0": timoshenko(0.0)},
               "ccx_version": subprocess.run([CCX, "-v"], capture_output=True, text=True).stdout.strip(),
               "cases": table, "criteria": crit}
    (out / "x2_summary.json").write_text(json.dumps(summary, indent=1))
    print(json.dumps(crit, indent=1))


if __name__ == "__main__":
    main()
