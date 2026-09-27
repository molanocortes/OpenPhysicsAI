#!/usr/bin/env python3
"""ccx_crosscheck.py - check one NAVIER static run against CalculiX (ccx) on the same assembled problem

    python3 tools/ccx_crosscheck.py export RUN_DIR OUT_DIR [--elements C3D8I|C3D8] [--probe X,Y,Z] [--all-nodes]
    (cd OUT_DIR && ccx -i model)
    python3 tools/ccx_crosscheck.py compare RUN_DIR OUT_DIR

export  reads RUN_DIR/results.nvr: the model NAVIER solved (nodes, hex8 connectivity, prescribed displacement components,
        consistent nodal loads, materials, the node sets of its selections). It writes OUT_DIR/model.inp with:
          - the same mesh as C3D8I (incompatible modes) or C3D8 (standard trilinear, 2x2x2 Gauss integration);
          - the same supports (*BOUNDARY) and the same nodal forces (*CLOAD);
          - linear isotropic *ELASTIC per material, and one node set per NAVIER selection;
          - an element set PROBE holding the element whose centroid is nearest to --probe (mm; default the model centre).
        Requested output: displacements of the load set, reaction forces with totals of the support set, total strain
        energy, the integration-point stresses of PROBE, and with --all-nodes the displacements of every node.
        navier_reference.json receives the same quantities from NAVIER's stored result.
compare parses OUT_DIR/model.dat and writes OUT_DIR/comparison.json. Every quantity uses the same definition on both
        sides:
          - load-region displacement: unweighted mean over the load set's nodes of u . d, d the unit load direction;
          - reaction force and moment: sum over the support set, moment about that set's nodal centroid;
          - strain energy: the total internal energy;
          - stress: the mean over the 8 integration points of the probe element.
        Integration-point order differs between codes, which the mean removes.

Units in the deck: mm, N, MPa (energies in N mm). What agrees by construction: geometry, mesh, supports, nodal loads,
material. What is tested: element formulation and implementation, assembly, solution, reaction and energy recovery,
stress recovery. Setup errors made before the export (wrong region, wrong load) are carried over and not detected here:
that needs an independently built model. Requires NumPy.
"""
import json
import re
import struct
import sys
from pathlib import Path

import numpy as np

TYPES = {"d": (np.float64, 8), "i": (np.int32, 4), "b": (np.uint8, 1), "c": (np.int8, 1)}


def load_nvr(path):
    with open(path, "rb") as f:
        if f.read(8) != b"NVRES001":
            raise SystemExit(f"{path}: not a NAVIER results file")
        (hlen,) = struct.unpack("<Q", f.read(8))
        header = json.loads(f.read(hlen))
        arrays = {}
        for a in header["arrays"]:
            dt, size = TYPES[a["type"]]
            n = int(a["count"])
            arrays[a["name"]] = np.frombuffer(f.read(n * size), dtype=dt, count=n) if n else np.zeros(0, dtype=dt)
    return header, arrays


def model(run_dir):
    header, A = load_nvr(Path(run_dir) / "results.nvr")
    nn, ne = header["nnodes"], header["nelems"]
    m = {"header": header, "nn": nn, "ne": ne,
         "xyz": A["xyz"].reshape(nn, 3) * 1e3,
         "conn": A["conn"].reshape(ne, 8),
         "emat": A["elem_mat"],
         "fixed": A["fixed"].reshape(nn, 3),
         "fval": A["fixed_value"].reshape(nn, 3) * 1e3,
         "force": A["nodal_force"].reshape(nn, 3),
         "u": A["u"].reshape(nn, 3) * 1e3,
         "reaction": A["reaction"].reshape(nn, 3),
         "gp_stress": A["gp_stress"].reshape(ne, 8, 6) * 1e-6,
         "sets": {}}
    for s, st in enumerate(header.get("sets", [])):
        nodes = A.get(f"set{s}_nodes")
        if nodes is not None and len(nodes):
            m["sets"][st["name"]] = np.array(nodes, dtype=np.int64)
    bcs = header.get("bcs", [])
    load = next((b for b in bcs if b["kind"] in ("force", "traction")), None)
    support = next((b for b in bcs if b["kind"] in ("fixed", "displacement", "frictionless_support")), None)
    if not load or not support:
        raise SystemExit("the run needs one force or traction load and one support")
    m["load_set"], m["support_set"] = load["selection"], support["selection"]
    F = np.array(load["applied"], dtype=float)
    m["direction"] = F / np.linalg.norm(F)
    m["load_total_n"] = F
    return m


def probe_element(m, probe_mm):
    cent = m["xyz"][m["conn"]].mean(axis=1)
    p = np.array(probe_mm, dtype=float) if probe_mm is not None else 0.5 * (m["xyz"].min(axis=0) + m["xyz"].max(axis=0))
    e = int(np.argmin(np.linalg.norm(cent - p, axis=1)))
    return e, cent[e]


def navier_quantities(m, probe):
    d = m["direction"]
    load_nodes, sup = m["sets"][m["load_set"]], m["sets"][m["support_set"]]
    c = m["xyz"][sup].mean(axis=0)
    rf = m["reaction"][sup]
    arm = m["xyz"][sup] - c
    e = probe
    s = m["gp_stress"][e].mean(axis=0)  # Voigt xx yy zz xy yz zx
    return {"load_region_nodal_mean_mm": float(np.mean(m["u"][load_nodes] @ d)),
            "reaction_force_n": rf.sum(axis=0).tolist(),
            "reaction_moment_nmm": np.cross(arm, rf).sum(axis=0).tolist(),
            "moment_point_mm": c.tolist(),
            "strain_energy_nmm": 1e3 * float(m["header"]["solution"]["strain_energy"]),
            "probe_element": e + 1,
            "probe_stress_mean_mpa": {"xx": float(s[0]), "yy": float(s[1]), "zz": float(s[2]), "xy": float(s[3]), "yz": float(s[4]), "zx": float(s[5])}}


def export(run_dir, out_dir, element="C3D8I", probe_mm=None, all_nodes=False):
    m = model(run_dir)
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    h = m["header"]
    e_probe, c_probe = probe_element(m, probe_mm)
    L = ["** NAVIER run exported for an independent CalculiX check (units: mm, N, MPa)", "*HEADING", f"NAVIER job {h.get('job_id', '')} as {element}",
         "*NODE, NSET=NALL"]
    L += [f"{i + 1}, {x[0]:.15g}, {x[1]:.15g}, {x[2]:.15g}" for i, x in enumerate(m["xyz"])]
    for k, mat in enumerate(h["materials"]):
        ids = np.nonzero(m["emat"] == k)[0]
        if not len(ids):
            continue
        L.append(f"*ELEMENT, TYPE={element}, ELSET=MAT{k}")
        L += [f"{e + 1}, " + ", ".join(str(int(v) + 1) for v in m["conn"][e]) for e in ids]
    L.append("*ELSET, ELSET=EALL")
    L += [f"MAT{k}" for k in range(len(h["materials"])) if np.any(m["emat"] == k)]
    L += ["*ELSET, ELSET=PROBE", str(e_probe + 1)]
    for name, nodes in m["sets"].items():
        L.append(f"*NSET, NSET={name.upper()}")
        ids = [str(int(v) + 1) for v in nodes]
        L += [", ".join(ids[k:k + 16]) for k in range(0, len(ids), 16)]
    for k, mat in enumerate(h["materials"]):
        if not np.any(m["emat"] == k):
            continue
        L += [f"*MATERIAL, NAME=M{k}", "*ELASTIC", f"{mat['E'] * 1e-6:.15g}, {mat['nu']:.15g}", f"*SOLID SECTION, ELSET=MAT{k}, MATERIAL=M{k}"]
    L += ["*STEP", "*STATIC", "*BOUNDARY"]
    for i in range(m["nn"]):
        for k in range(3):
            if m["fixed"][i, k]:
                L.append(f"{i + 1}, {k + 1}, {k + 1}, {m['fval'][i, k]:.15g}")
    if any(abs(g) > 0 for g in h.get("gravity", [0, 0, 0])):
        raise SystemExit("this run has gravity: body forces are not exported; compare runs without self-weight")
    L.append("*CLOAD")
    for i in range(m["nn"]):
        for k in range(3):
            if m["force"][i, k] != 0:
                L.append(f"{i + 1}, {k + 1}, {m['force'][i, k]:.17g}")
    L += [f"*NODE PRINT, NSET={m['load_set'].upper()}", "U",
          f"*NODE PRINT, NSET={m['support_set'].upper()}, TOTALS=YES", "RF",
          "*EL PRINT, ELSET=EALL, TOTALS=ONLY", "ELSE",
          "*EL PRINT, ELSET=PROBE", "S"]
    if all_nodes:
        L += ["*NODE PRINT, NSET=NALL", "U"]
    L += ["*END STEP", ""]
    (out / "model.inp").write_text("\n".join(L))
    ref = {"run_directory": str(Path(run_dir).resolve()), "job_id": h.get("job_id"), "navier_formulation": h["settings"]["formulation"], "ccx_element": element,
           "nodes": m["nn"], "elements": m["ne"], "element_size_mm": [1e3 * v for v in h["h_m"]], "direction": m["direction"].tolist(),
           "load_set": m["load_set"], "support_set": m["support_set"], "load_total_n": m["load_total_n"].tolist(),
           "probe_request_mm": probe_mm, "probe_centroid_mm": c_probe.tolist(), "all_nodes": all_nodes,
           "navier": navier_quantities(m, e_probe)}
    (out / "navier_reference.json").write_text(json.dumps(ref, indent=1))
    print(f"wrote {out / 'model.inp'}: {m['nn']} nodes, {m['ne']} {element} elements, probe element {e_probe + 1} at {np.round(c_probe, 3).tolist()} mm")


NUM = r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?"


def parse_dat(path):
    """sections of a ccx .dat file: title line -> list of numeric rows"""
    sections, current = {}, None
    for line in Path(path).read_text(errors="replace").splitlines():
        s = line.strip()
        if not s:
            continue
        if re.match(r"^[a-z]", s) and "for set" in s:
            current = s
            sections[current] = []
            continue
        if current is None:
            continue
        nums = re.findall(NUM, s)
        if nums and len(nums) == len(s.split()):
            sections[current].append([float(v) for v in nums])
    return sections


def find(sections, *words):
    """the first section whose title contains every word (case-insensitive: ccx prints set names in upper case)"""
    for title, rows in sections.items():
        if all(w.lower() in title.lower() for w in words):
            return title, rows
    return None, []


def rel(a, b):
    a, b = np.asarray(a, dtype=float), np.asarray(b, dtype=float)
    scale = max(np.linalg.norm(a), np.linalg.norm(b), 1e-300)
    return float(np.linalg.norm(a - b) / scale)


def compare(run_dir, out_dir):
    out = Path(out_dir)
    ref = json.loads((out / "navier_reference.json").read_text())
    m = model(run_dir)
    dat = out / "model.dat"
    if not dat.exists():
        raise SystemExit(f"{dat} not found: run CalculiX first (ccx -i model)")
    sec = parse_dat(dat)
    d = m["direction"]
    t_u, rows_u = find(sec, "displacements", f"set {ref['load_set']} ")
    t_f, rows_f = find(sec, "forces (fx", f"set {ref['support_set']} ")
    t_e, rows_e = find(sec, "energy")
    t_s, rows_s = find(sec, "stresses", "PROBE")
    load_nodes, sup = m["sets"][m["load_set"]], m["sets"][m["support_set"]]
    u = {int(r[0]) - 1: r[1:4] for r in rows_u if len(r) == 4}
    uc = np.array([u[i] for i in load_nodes])
    ccx_disp = float(np.mean(uc @ d))
    rf_nodes = {int(r[0]) - 1: r[1:4] for r in rows_f if len(r) == 4}
    rfc = np.array([rf_nodes.get(i, [0, 0, 0]) for i in sup])
    c = m["xyz"][sup].mean(axis=0)
    ccx_rf = rfc.sum(axis=0)
    ccx_rm = np.cross(m["xyz"][sup] - c, rfc).sum(axis=0)
    ccx_energy = float(rows_e[-1][-1]) if rows_e else float("nan")
    st = np.array([r[2:8] for r in rows_s if len(r) == 8]) if rows_s else np.zeros((0, 6))  # sxx syy szz sxy sxz syz
    nav = ref["navier"]
    load = np.array(ref["load_total_n"])
    lever = float(np.max(np.linalg.norm(m["xyz"][load_nodes] - c, axis=1)))
    res = {"case": ref, "ccx_sections": [t_u, t_f, t_e, t_s],
           "load_region_nodal_mean_mm": {"navier": nav["load_region_nodal_mean_mm"], "ccx": ccx_disp,
                                         "relative_difference": (ccx_disp - nav["load_region_nodal_mean_mm"]) / nav["load_region_nodal_mean_mm"]},
           "reaction_force_n": {"navier": nav["reaction_force_n"], "ccx": ccx_rf.tolist(), "relative_difference": rel(nav["reaction_force_n"], ccx_rf),
                                "navier_balance": rel(np.array(nav["reaction_force_n"]), -load), "ccx_balance": rel(ccx_rf, -load)},
           "reaction_moment_nmm": {"navier": nav["reaction_moment_nmm"], "ccx": ccx_rm.tolist(),
                                   "relative_difference": float(np.linalg.norm(np.array(nav["reaction_moment_nmm"]) - ccx_rm) / (np.linalg.norm(load) * lever)),
                                   "scale_note": "difference divided by |load| x largest distance from the moment point to a load node"},
           "strain_energy_nmm": {"navier": nav["strain_energy_nmm"], "ccx": ccx_energy,
                                 "relative_difference": (ccx_energy - nav["strain_energy_nmm"]) / nav["strain_energy_nmm"],
                                 "work_check_ccx": ccx_energy / (0.5 * float(np.linalg.norm(load)) * ccx_disp) if ccx_disp else None},
           "probe_stress_mean_mpa": {}}
    if len(st):
        mean = st.mean(axis=0)
        for key, idx in (("xx", 0), ("yy", 1), ("zz", 2), ("xy", 3)):
            nv = nav["probe_stress_mean_mpa"][key]
            res["probe_stress_mean_mpa"][key] = {"navier": nv, "ccx": float(mean[idx]), "relative_difference": (float(mean[idx]) - nv) / nv if abs(nv) > 1e-12 else None}
        res["probe_stress_mean_mpa"]["integration_points_ccx"] = int(len(st))
    if ref.get("all_nodes"):
        _, rows_all = find(sec, "displacements", "set NALL ")
        ua = {int(r[0]) - 1: r[1:4] for r in rows_all if len(r) == 4}
        if len(ua) == m["nn"]:
            ucx = np.array([ua[i] for i in range(m["nn"])])
            res["all_nodes"] = {"max_displacement_difference_mm": float(np.max(np.linalg.norm(ucx - m["u"], axis=1))),
                                "max_displacement_navier_mm": float(np.max(np.linalg.norm(m["u"], axis=1)))}
    warn = [l.strip() for l in (out / "ccx.log").read_text(errors="replace").splitlines() if re.search(r"warning|error|singular|negative|zero pivot", l, re.I)] \
        if (out / "ccx.log").exists() else ["ccx.log not found"]
    res["ccx_messages"] = warn
    (out / "comparison.json").write_text(json.dumps(res, indent=1))
    return res


if __name__ == "__main__":
    if len(sys.argv) < 4 or sys.argv[1] not in ("export", "compare"):
        print(__doc__)
        sys.exit(2)
    if sys.argv[1] == "export":
        el = sys.argv[sys.argv.index("--elements") + 1] if "--elements" in sys.argv else "C3D8I"
        probe = [float(v) for v in sys.argv[sys.argv.index("--probe") + 1].split(",")] if "--probe" in sys.argv else None
        export(sys.argv[2], sys.argv[3], el, probe, "--all-nodes" in sys.argv)
    else:
        print(json.dumps(compare(sys.argv[2], sys.argv[3]), indent=1))
