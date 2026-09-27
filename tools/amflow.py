#!/usr/bin/env python3
"""amflow.py - the first complete AI-control path, driven over MCP stdio exactly as an AI host would drive it:

  STL import -> units and placement -> surface discovery -> selections -> material -> volume mesh ->
  supports and loads -> validation -> analysis job -> result query -> result image -> exports

on an L-shaped bracket, then a disconnect/reconnect check against navier-server (a job started by one MCP client is
finished and visible to the next client, and an idempotent retry does not start a duplicate run).

The exported VTU file is checked independently here (XML header, appended-block offsets and sizes, cell types,
connectivity range, and displacements compared with the CSV export).

This is a protocol-level test with a scripted client, NOT a test with a real AI model; see docs/mcp-clients.md.
Run from the repository root after `make am`:
    python3 tools/amflow.py
"""
import base64
import json
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcptest import CTL, MCP, SERVER, Client, structured  # noqa: E402

PASS = 0
FAIL = 0


def check(cond, what):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"  FAIL: {what}")


def bracket_stl(path):
    """closed L-bracket: profile in the XZ plane (mm), extruded 30 mm along +y, outward winding"""
    prof = [(0, 0), (60, 0), (60, 6), (6, 6), (6, 40), (0, 40)]
    width = 30.0
    tris = []
    for a, b, c in [(0, 1, 2), (0, 2, 3), (0, 3, 4), (0, 4, 5)]:  # fan triangulation of the L, counter-clockwise from -y
        pa, pb, pc = prof[a], prof[b], prof[c]
        tris.append(((pa[0], 0, pa[1]), (pb[0], 0, pb[1]), (pc[0], 0, pc[1])))                  # y = 0 cap, normal -y
        tris.append(((pa[0], width, pa[1]), (pc[0], width, pc[1]), (pb[0], width, pb[1])))      # y = 30 cap, normal +y
    for i in range(len(prof)):
        p, q = prof[i], prof[(i + 1) % len(prof)]
        a, b, c, d = (p[0], 0, p[1]), (p[0], width, p[1]), (q[0], width, q[1]), (q[0], 0, q[1])
        tris.append((a, b, c))
        tris.append((a, c, d))
    with open(path, "wb") as f:
        f.write(b"L bracket for amflow".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


def call_ok(c, name, args, what=None):
    r = c.call(name, args)
    res = r.get("result", {})
    sc = res.get("structuredContent") or {}
    ok = res.get("isError") is False and sc.get("ok") is True
    check(ok, f"{what or name}: {json.dumps(sc.get('error'))[:300] if not ok else ''}")
    return sc.get("value", {}), r


def check_vtu(vtu: Path, csv: Path):
    data = vtu.read_bytes()
    marker = data.index(b'<AppendedData encoding="raw">')
    start = data.index(b"_", marker) + 1
    header = data[:marker].decode()
    check('byte_order="LittleEndian"' in header and 'header_type="UInt64"' in header, "VTU declares little-endian UInt64 block headers")
    npts = int(re.search(r'NumberOfPoints="(\d+)"', header).group(1))
    ncells = int(re.search(r'NumberOfCells="(\d+)"', header).group(1))
    sections = {"PointData": (header.index("<PointData"), header.index("</PointData>")),
                "CellData": (header.index("<CellData"), header.index("</CellData>")),
                "Points": (header.index("<Points>"), header.index("</Points>")),
                "Cells": (header.index("<Cells>"), header.index("</Cells>"))}
    size = {"Float64": 8, "Int32": 4, "UInt8": 1}
    blocks = {}
    offsets_seen = []
    for m in re.finditer(r'<DataArray type="(\w+)" Name="([^"]+)" NumberOfComponents="(\d+)" format="appended" offset="(\d+)"/>', header):
        typ, name, ncomp, off = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        sec = next(s for s, (a, b) in sections.items() if a < m.start() < b)
        nbytes = struct.unpack_from("<Q", data, start + off)[0]
        if sec == "PointData":
            expect = npts * ncomp
        elif sec == "CellData":
            expect = ncells * ncomp
        elif sec == "Points":
            expect = npts * 3
        else:
            expect = {"connectivity": 8 * ncells, "offsets": ncells, "types": ncells}[name]
        check(nbytes == expect * size[typ], f"VTU block {name}: {nbytes} bytes, expected {expect * size[typ]}")
        blocks[name] = (typ, ncomp, start + off + 8, nbytes)
        offsets_seen.append((off, nbytes))
    offsets_seen.sort()
    contiguous = all(offsets_seen[i][0] + 8 + offsets_seen[i][1] == offsets_seen[i + 1][0] for i in range(len(offsets_seen) - 1))
    end = start + offsets_seen[-1][0] + 8 + offsets_seen[-1][1]
    check(contiguous and data[end:].strip().startswith(b"</AppendedData>"), "VTU appended blocks are contiguous and end at </AppendedData>")
    _, _, p, n = blocks["types"]
    check(set(data[p:p + n]) == {12}, "all VTU cells are VTK_HEXAHEDRON (12)")
    _, _, p, n = blocks["connectivity"]
    conn = struct.unpack_from(f"<{n // 4}i", data, p)
    check(min(conn) >= 0 and max(conn) < npts, "VTU connectivity indices within the point range")
    _, _, p, n = blocks["offsets"]
    offs = struct.unpack_from(f"<{n // 4}i", data, p)
    check(offs[0] == 8 and offs[-1] == 8 * ncells, "VTU offsets count 8 nodes per cell")
    _, _, p, n = blocks["displacement_m"]
    disp = struct.unpack_from(f"<{n // 8}d", data, p)
    rows = csv.read_text().splitlines()
    check(len(rows) == npts + 1, f"CSV has {len(rows) - 1} node rows for {npts} VTU points")
    hdr = rows[0].split(",")
    iu = [hdr.index(k) for k in ("ux_mm", "uy_mm", "uz_mm")]
    worst = 0.0
    for row in rows[1:]:
        cols = row.split(",")
        node = int(cols[0])
        for k in range(3):
            a, b = float(cols[iu[k]]), 1e3 * disp[3 * node + k]
            worst = max(worst, abs(a - b) / max(abs(b), 1e-9))
    check(worst < 1e-8, f"VTU displacements agree with the CSV export (worst relative difference {worst:.2e})")


def main():
    if not MCP.exists() or not SERVER.exists():
        print("binaries not built (run: make am)")
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="nvamflow"))
    ws = tmp / "ws"
    stl = tmp / "bracket.stl"
    bracket_stl(stl)
    common = ["--workspace", str(ws), "--allow-read", str(tmp)]

    print("== setup through MCP tools")
    c = Client(["--embedded"] + common)
    c.initialize()
    tools = {t["name"]: t for t in c.request("tools/list")["result"]["tools"]}
    needed = {"surfaces_list", "selection_create", "view_render", "view_pick", "materials_list", "material_assign", "mesh_generate", "boundary_apply",
              "setup_validate", "analysis_run", "job_status", "job_cancel", "results_query", "results_probe", "results_render", "results_export"}
    check(needed <= set(tools), f"analysis tools listed (missing: {sorted(needed - set(tools))})")
    check(tools["job_status"]["annotations"]["readOnlyHint"] is True and tools["analysis_run"]["annotations"]["readOnlyHint"] is False,
          "read-only hints distinguish queries from actions")
    call_ok(c, "project_create", {"name": "bracket_demo", "description": "L bracket pushed sideways at the top"})
    v, _ = call_ok(c, "geometry_import", {"path": str(stl), "units": "mm", "name": "bracket"})
    check(v.get("body", {}).get("closed_solid") is True and abs(v["body"]["volume_mm3"] - 16920) < 1e-6, f"bracket imported as a closed solid ({v.get('body', {}).get('volume_mm3')})")
    v, _ = call_ok(c, "surfaces_list", {"body": "bracket"})
    check(v.get("patches_total") == 8, f"8 faces found ({v.get('patches_total')})")
    v, _ = call_ok(c, "selection_create", {"name": "base_bottom", "query": {"all": [{"facing": {"direction": "down"}}, {"plane": {"axis": "z", "at": "min"}}]},
                                          "description": "underside bolted to the table", "source": "user"})
    check(abs(v.get("selection", {}).get("area_mm2", 0) - 1800) < 1e-6, "base underside 1800 mm2")
    v, _ = call_ok(c, "selection_create", {"name": "upright_top", "query": {"extreme": {"direction": "+z"}}, "description": "top of the upright", "source": "user"})
    check(abs(v.get("selection", {}).get("area_mm2", 0) - 180) < 1e-6, "upright top 180 mm2")
    v, r = call_ok(c, "view_render", {"highlight": ["base_bottom", "upright_top"], "width": 640, "height": 420})
    images = [item for item in r["result"]["content"] if item.get("type") == "image"]
    check(len(images) == 1 and images[0]["mimeType"] == "image/png" and base64.b64decode(images[0]["data"])[:8] == b"\x89PNG\r\n\x1a\n",
          "view image delivered as MCP image content")
    check(v.get("camera", {}).get("pixel_convention", "").startswith("origin at the top-left"), "camera metadata with the pixel convention")
    v, _ = call_ok(c, "materials_list", {"process": "lpbf"})
    # demonstration data, or measured / published, which the library accepts only with a source on every value
    check(all(m["status"] in ("demonstration", "measured", "published") for m in v.get("materials", [])) and
          any(m["status"] == "demonstration" for m in v.get("materials", [])),
          "library materials are labelled demonstration, or measured or published with their sources")
    call_ok(c, "material_assign", {"body": "bracket", "material": "ss316l_lpbf_demo", "source": "user", "note": "user chose the 316L demonstration entry"})
    v, _ = call_ok(c, "mesh_generate", {"element_size": "2 mm"})
    mesh = v.get("mesh", {})
    check(mesh.get("elements") == 30 * 15 * 3 + 3 * 15 * 17, f"voxel mesh of {mesh.get('elements')} elements")
    call_ok(c, "boundary_apply", {"name": "bolted", "kind": "fixed", "selection": "base_bottom", "source": "user", "description": "bolted flat to a rigid table"})
    call_ok(c, "boundary_apply", {"name": "push", "kind": "force", "selection": "upright_top", "force": ["0.5 kN", 0, 0], "source": "user"})
    v, _ = call_ok(c, "setup_validate", {})
    codes = {w.get("code") for w in v.get("warnings", [])}
    check(v.get("ready") is True and "DEMONSTRATION_MATERIAL" in codes, f"setup ready, demonstration material warned ({sorted(codes)})")

    print("== analysis job and results")
    v, _ = call_ok(c, "analysis_run", {"label": "push test", "idempotency_key": "bracket-run-1"})
    job = v.get("job_id", "")
    status = {}
    for _ in range(10):
        status, _ = call_ok(c, "job_status", {"job_id": job, "wait_seconds": 30}, "job_status")
        if status.get("state") not in ("queued", "running"):
            break
    check(status.get("state") == "succeeded", f"job {job} succeeded ({status.get('state')}: {status.get('error')})")
    checks = status.get("summary", {}).get("checks", {})
    check(checks.get("equilibrium_ok") is True and checks.get("energy_ok") is True, f"equilibrium and energy checks pass ({checks.get('equilibrium_error')}, {checks.get('energy_ratio')})")
    check(abs(checks.get("reaction_total_n", [0])[0] + 500) < 1e-6, f"support reaction -500 N in x ({checks.get('reaction_total_n')})")
    rw = [w.get("code") for w in status.get("summary", {}).get("result_warnings", [])]
    check("PEAK_AT_REENTRANT_CORNER" in rw, f"the stress peak at the inside corner is flagged as a singularity ({rw})")
    v, _ = call_ok(c, "results_query", {"job_id": job, "quantity": "displacement"})
    top = v.get("largest", [{}])[0]
    check(top.get("location_mm", [0, 0, 0])[2] > 39, f"largest displacement at the top of the upright ({top.get('location_mm')})")
    v, r = call_ok(c, "results_render", {"job_id": job, "quantity": "von_mises", "width": 640, "height": 420})
    images = [item for item in r["result"]["content"] if item.get("type") == "image"]
    check(len(images) == 1 and base64.b64decode(images[0]["data"])[:4] == b"\x89PNG", "result image delivered as MCP image content")
    check(v.get("legend", {}).get("unit") == "MPa" and "caution" in v, "legend units and the correctness caution are reported")
    v, _ = call_ok(c, "results_export", {"job_id": job})
    files = {f["format"]: Path(f["path"]) for f in v.get("files", [])}
    check(set(files) == {"vtu", "csv", "summary"} and all(p.exists() for p in files.values()), "VTU, CSV and summary exported")
    if "vtu" in files and "csv" in files:
        check_vtu(files["vtu"], files["csv"])
    run_dir = Path(status.get("run_directory", "/nonexistent"))
    spec = json.loads((run_dir / "spec.json").read_text()) if (run_dir / "spec.json").exists() else {}
    check(spec.get("spec_hash") == status.get("spec_hash") and spec.get("materials_resolved", [{}])[0].get("status") == "demonstration",
          "spec.json records the hashed setup with the resolved (demonstration) material")
    call_ok(c, "project_save", {})
    code, _ = c.close()
    check(code == 0, "embedded session exits cleanly")

    print("== disconnect and reconnect through navier-server")
    sock = tmp / "run" / "ctl.sock"
    ready = tmp / "ready"
    srv = subprocess.Popen([str(SERVER), "--socket", str(sock), "--ready-file", str(ready)] + common, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for _ in range(100):
        if ready.exists():
            break
        time.sleep(0.05)
    check(ready.exists(), "navier-server ready")
    a = Client(["--connect", str(sock)])
    a.initialize()
    call_ok(a, "project_open", {"path": str(ws / "bracket_demo")}, "project_open through the bridge")
    v, _ = call_ok(a, "mesh_generate", {}, "mesh_generate with the stored settings")
    check(v.get("element_size_source") == "stored" and v.get("reproduces_stored_mesh") is True,
          f"the reopened project regenerates the identical mesh ({v.get('element_size_source')}, {v.get('reproduces_stored_mesh')})")
    v, _ = call_ok(a, "analysis_run", {"idempotency_key": "after-reconnect", "formulation": "full_integration"})
    job2 = v.get("job_id", "")
    a.close()  # the client goes away while the job may still be running
    b = Client(["--connect", str(sock)])
    b.initialize()
    status = {}
    for _ in range(10):
        status, _ = call_ok(b, "job_status", {"job_id": job2, "wait_seconds": 30}, "job_status from a new client")
        if status.get("state") not in ("queued", "running"):
            break
    check(status.get("state") == "succeeded", f"the job finished after its client disconnected ({status.get('state')})")
    v, r = call_ok(b, "analysis_run", {"idempotency_key": "after-reconnect", "formulation": "full_integration"}, "retry of analysis_run")
    check(v.get("job_id") == job2 and r["result"]["structuredContent"].get("replayed") is True, "a retried request returns the original job instead of a duplicate")
    v, _ = call_ok(b, "job_list", {})
    check(sum(1 for j in v.get("jobs", []) if j.get("job_id") == job2) == 1, "exactly one job exists for the retried request")
    b.close()
    out = subprocess.run([str(CTL), "--socket", str(sock), "call", "results_query", json.dumps({"job_id": job, "quantity": "von_mises"})],
                         capture_output=True, text=True, timeout=30)
    check(out.returncode == 0 and '"largest"' in out.stdout, "results of the earlier embedded session are reloaded from the run directory")
    srv.send_signal(signal.SIGTERM)
    try:
        srv.wait(timeout=20)
    except subprocess.TimeoutExpired:
        srv.kill()
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{'AM FLOW TESTS FAILED' if FAIL else 'ALL AM FLOW TESTS PASSED'}: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
