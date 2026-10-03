#!/usr/bin/env python3
"""lpbfflow.py - the inherent-strain LPBF build driven over MCP stdio exactly as an AI host would drive it:

  block on the plate (STL) -> material -> voxel mesh -> lpbf_build_run (job kind lpbf_build) -> job_status ->
  the stored times through the ordinary result operations -> the cut, and what elem_birth and elem_death say about it

plus the refusals that keep the answer honest: an inherent strain without provenance, elastic constants without
provenance, a cut without provenance, a cut that crosses no element, a part lifted off the plate, a stale mesh; the
duplicate detection of an identical build; a second engine session that reopens the run directory and reads the same
numbers; and the result file's element birth and death, read back through the file itself.

This is a protocol-level test with a scripted client, NOT a test with a real AI model.
Run from the repository root after `make`:
    python3 tools/lpbfflow.py
"""
import json
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcptest import MCP, Client  # noqa: E402

PASS = 0
FAIL = 0


def check(cond, what):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"  FAIL: {what}")


def box_stl(path, lo, hi):
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    v = [(x0, y0, z0), (x1, y0, z0), (x0, y1, z0), (x1, y1, z0), (x0, y0, z1), (x1, y0, z1), (x0, y1, z1), (x1, y1, z1)]
    faces = [(0, 2, 3), (0, 3, 1), (4, 5, 7), (4, 7, 6), (0, 1, 5), (0, 5, 4), (2, 6, 7), (2, 7, 3), (0, 4, 6), (0, 6, 2), (1, 3, 7), (1, 7, 5)]
    with open(path, "wb") as f:
        f.write(b"lpbfflow block".ljust(80, b" "))
        f.write(struct.pack("<I", len(faces)))
        for a, b, c in faces:
            f.write(struct.pack("<3f", 0, 0, 0))
            for i in (a, b, c):
                f.write(struct.pack("<3f", *v[i]))
            f.write(b"\0\0")


def call(c, name, args):
    return (c.call(name, args).get("result", {}).get("structuredContent")) or {}


def call_ok(c, name, args, what=None):
    sc = call(c, name, args)
    ok = sc.get("ok") is True
    check(ok, f"{what or name}: {json.dumps(sc.get('error'))[:600] if not ok else ''}")
    return sc.get("value", {})


def call_err(c, name, args, code, what):
    sc = call(c, name, args)
    err = sc.get("error") or {}
    check(sc.get("ok") is False and err.get("code") == code, f"{what}: expected {code}, got {json.dumps(err)[:400]}")
    return err


def wait_job(c, job, tries=120):
    st = {}
    for _ in range(tries):
        st = call_ok(c, "job_status", {"job_id": job, "wait_seconds": 10}, "job_status")
        if st.get("state") not in ("queued", "running"):
            break
    return st


STRAIN = {"exx": -0.001, "eyy": -0.002, "ezz": -0.01, "provenance": "user", "source": "a test tensor, not a calibration"}
MATERIAL = {"youngs_modulus": "70000 MPa", "poissons_ratio": 0.33, "provenance": "user"}
CUT = {"height": "2 mm", "kerf": "1 mm", "from_x": "4 mm", "provenance": "assumed"}


def read_results_header(path):
    """the JSON header of results.nvt: magic, 8-byte length, header"""
    with open(path, "rb") as f:
        magic = f.read(8)
        if magic != b"NVTHR001":
            return None
        n = struct.unpack("<Q", f.read(8))[0]
        return json.loads(f.read(n))


def read_results_array(path, name):
    """one array of results.nvt, read by walking the arrays in the order the header lists them (no padding)"""
    size = {"d": 8, "i": 4, "b": 1, "c": 1}
    fmt = {"d": "d", "i": "i", "b": "B", "c": "b"}
    with open(path, "rb") as f:
        if f.read(8) != b"NVTHR001":
            return None
        n = struct.unpack("<Q", f.read(8))[0]
        header = json.loads(f.read(n))
        for a in header.get("arrays", []):
            nbytes = size[a["type"]] * a["count"]
            if a["name"] == name:
                return list(struct.unpack(f"<{a['count']}{fmt[a['type']]}", f.read(nbytes)))
            f.seek(nbytes, 1)
    return None


def voxel_stl(path, cells, h=1.0):
    """the closed boundary of a set of unit cells (i, j, k), as a binary STL in mm"""
    faces = {(-1, 0, 0): ((0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)), (1, 0, 0): ((1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1)),
             (0, -1, 0): ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)), (0, 1, 0): ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)),
             (0, 0, -1): ((0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)), (0, 0, 1): ((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1))}
    tris = []
    for (i, j, k) in sorted(cells):
        for d, cs in faces.items():
            if (i + d[0], j + d[1], k + d[2]) in cells:
                continue
            p = [((i + c[0]) * h, (j + c[1]) * h, (k + c[2]) * h) for c in cs]
            tris += [(d, p[0], p[1], p[2]), (d, p[0], p[2], p[3])]
    with open(path, "wb") as f:
        f.write(b"voxel test part".ljust(80, b" ") + struct.pack("<I", len(tris)))
        for n, a, b, c in tris:
            f.write(struct.pack("<3f", *map(float, n)))
            for v in (a, b, c):
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


def prism_stl(path, lean_deg, h=10.0, w=10.0):
    """a block of w x w footprint and height h whose top is shifted in x by h tan(lean): one side face overhangs, at
    90 - lean degrees from the horizontal"""
    import math
    d = h * math.tan(math.radians(lean_deg))
    v = [(0, 0, 0), (w, 0, 0), (w, w, 0), (0, w, 0), (d, 0, h), (w + d, 0, h), (w + d, w, h), (d, w, h)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, c, e in quads:
        tris += [(v[a], v[b], v[c]), (v[a], v[c], v[e])]
    with open(path, "wb") as f:
        f.write(b"leaning prism".ljust(80, b" ") + struct.pack("<I", len(tris)))
        for A, B, C in tris:
            u = [B[k] - A[k] for k in range(3)]; t = [C[k] - A[k] for k in range(3)]
            n = [u[1] * t[2] - u[2] * t[1], u[2] * t[0] - u[0] * t[2], u[0] * t[1] - u[1] * t[0]]
            ln = math.sqrt(sum(x * x for x in n)) or 1
            f.write(struct.pack("<3f", *[x / ln for x in n]))
            for P in (A, B, C):
                f.write(struct.pack("<3f", *map(float, P)))
            f.write(b"\0\0")


def main():
    if not MCP.exists():
        print("navier-mcp not built (run: make)")
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="nvlpbfflow"))
    ws = tmp / "ws"
    common = ["--workspace", str(ws), "--allow-read", str(tmp)]
    c = Client(["--embedded"] + common)
    c.initialize()
    try:
        tools = {t["name"] for t in c.request("tools/list")["result"]["tools"]}
        check("lpbf_build_run" in tools, "lpbf_build_run is listed as a tool")

        stl = tmp / "block.stl"
        box_stl(stl, (0, 0, 0), (12, 4, 4))  # a small bar on the plate
        v = call_ok(c, "project_create", {"name": "lpbf_block", "description": "a small powder-bed block"})
        project_dir = (v.get("project") or {}).get("directory", "")
        call_ok(c, "geometry_import", {"path": str(stl), "units": "mm", "name": "block"})
        call_ok(c, "material_define", {"material": {
            "id": "alsi10mg_test", "name": "AlSi10Mg, test values", "family": "metal", "status": "user_supplied", "processes": ["lpbf"],
            "provenance": "the elastic constants of the validation data, used here only to exercise the operation",
            "density_kg_m3": {"value": 2680}, "youngs_modulus_pa": {"value": 70e9}, "poisson_ratio": {"value": 0.33}}})
        call_ok(c, "material_assign", {"body": "block", "material": "alsi10mg_test", "source": "user"})
        call_ok(c, "mesh_generate", {"element_size": "1 mm"})

        print("== refusals before any solve")
        err = call_err(c, "lpbf_build_run", {"layer_thickness_sim": "1 mm", "material": MATERIAL, "cut": CUT,
                                             "inherent_strain": {k: v for k, v in STRAIN.items() if k != "provenance"}},
                       "PRECONDITION_FAILED", "an inherent strain without provenance is refused")
        check("provenance" in json.dumps(err).lower(), "the refusal names the provenance of the strain")
        call_err(c, "lpbf_build_run", {"layer_thickness_sim": "1 mm", "inherent_strain": STRAIN, "cut": CUT,
                                       "material": {k: v for k, v in MATERIAL.items() if k != "provenance"}},
                 "INVALID_PARAMS", "elastic constants without provenance are refused by the schema")
        call_err(c, "lpbf_build_run", {"layer_thickness_sim": "1 mm", "inherent_strain": STRAIN, "material": MATERIAL,
                                       "cut": {k: v for k, v in CUT.items() if k != "provenance"}},
                 "INVALID_PARAMS", "a cut without provenance is refused by the schema")
        err = call_err(c, "lpbf_build_run", {"layer_thickness_sim": "1 mm", "inherent_strain": STRAIN, "material": MATERIAL,
                                             "cut": dict(CUT, height="20 mm")}, "PRECONDITION_FAILED", "a cut outside the part is refused")
        check("crosses no element" in json.dumps(err), "the refusal says the cut crosses nothing")
        err = call_err(c, "lpbf_build_run", {"layer_thickness_sim": "1 mm", "inherent_strain": {"strain_set": "P17"},
                                             "material": MATERIAL}, "INVALID_PARAMS",
                       "a named strain set is refused: none ships with the engine")
        check("exx" in json.dumps(err), "the refusal asks for the three typed components")

        print("== a part that does not rest on the plate, and a stale mesh")
        call_ok(c, "geometry_place", {"body": "block", "z_offset": "3 mm"})
        call_ok(c, "mesh_generate", {"element_size": "1 mm"})
        err = call_err(c, "lpbf_build_run", {"layer_thickness_sim": "1 mm", "inherent_strain": STRAIN, "material": MATERIAL},
                       "PRECONDITION_FAILED", "a lifted part is refused")
        check("plate" in json.dumps(err).lower(), "the refusal says the part is not on the plate")
        call_ok(c, "geometry_place", {"body": "block", "z_offset": "0 mm"})
        call_ok(c, "mesh_generate", {"element_size": "1 mm"})
        call_ok(c, "geometry_place", {"body": "block", "rotate_z": 90})
        err = call_err(c, "lpbf_build_run", {"layer_thickness_sim": "1 mm", "inherent_strain": STRAIN, "material": MATERIAL},
                       "PRECONDITION_FAILED", "a stale mesh is refused")
        check("stale" in json.dumps(err).lower(), "the refusal says the mesh is stale")
        call_ok(c, "geometry_place", {"body": "block", "rotate_z": 0})
        call_ok(c, "mesh_generate", {"element_size": "1 mm"})

        print("== the build")
        args = {"body": "block", "build_orientation": "X", "layer_thickness_sim": "1 mm", "inherent_strain": STRAIN,
                "material": MATERIAL, "cut": CUT, "label": "block build"}
        v = call_ok(c, "lpbf_build_run", args, "lpbf_build_run")
        job, run_dir = v.get("job_id", ""), v.get("run_directory", "")
        check(v.get("analysis") == "lpbf_build", "the job is an lpbf_build analysis")
        dup = call_ok(c, "lpbf_build_run", args, "an identical build")
        check(dup.get("deduplicated") is True and dup.get("job_id") == job, "an identical build returns the running job")
        st = wait_job(c, job)
        check(st.get("state") == "succeeded", f"the build succeeded ({st.get('state')}, {json.dumps(st.get('error'))[:200]})")
        s = st.get("summary") or {}
        res, model = s.get("results") or {}, s.get("model") or {}
        print(f"   {res.get('layers')} layers, before {res.get('tip_uz_before_cut_mm'):+.4f} mm, after {res.get('tip_uz_after_cut_mm'):+.4f} mm, "
              f"springback {res.get('springback_mm'):+.4f} mm, {model.get('cut', {}).get('elements_removed')} elements cut")
        check(res.get("layers") == 4, f"one simulation layer per millimetre of height ({res.get('layers')})")
        check(res.get("stored_times") == res.get("layers", 0) + 1, "one stored time per layer, plus the state after the cut")
        check(s.get("temperature") == "not modelled", "the summary says there is no temperature field")
        check((s.get("scope") or {}).get("is_forecast") is False and (s.get("scope") or {}).get("plasticity") is False,
              "the summary's scope says what the build is not")
        check(model.get("inherent_strain", {}).get("provenance") == "user", "the strain's provenance is recorded")
        check(model.get("cut", {}).get("provenance") == "assumed", "the cut's provenance is recorded literally")
        check(abs(res.get("largest_plate_reaction_n", 1e9)) < 1e9, "the plate reactions are reported")
        check("eight Gauss-point von Mises" in res.get("stress_field_definition", ""),
              "the build summary states the actual stress scalar definition")

        print("== the build through the ordinary result operations")
        err = call_err(c, "results_query", {"job_id": job, "quantity": "temperature"}, "UNSUPPORTED", "a temperature query on a build is refused")
        check("no temperature" in json.dumps(err).lower(), "the refusal says the analysis stores no temperature field")
        last = call_ok(c, "results_query", {"job_id": job, "quantity": "displacement", "component": "z"}, "results_query at the last stored time")
        check((last.get("statistics") or {}).get("max") is not None, f"the query returns statistics ({json.dumps(last)[:160]})")
        first = call_ok(c, "results_query", {"job_id": job, "quantity": "von_mises", "time_index": 0}, "results_query at the first stored time")
        check(first.get("time_index") == 0, "the first stored time can be queried")
        # the mesher centres the part in x and y, so the bar spans x -6..+6, y -2..+2, z 0..4: probe inside that
        pr = call_ok(c, "results_probe", {"job_id": job, "points_mm": [[5, 0, 3.5]], "history": True}, "results_probe with a history")
        check(json.dumps(pr).find('"inside": true') >= 0, "the probe point is inside the meshed volume")
        check("uz_mm" in json.dumps(pr), "the probe history carries the vertical displacement")

        print("== the result file's element birth and death")
        header = read_results_header(Path(run_dir) / "results.nvt")
        check(header is not None and header.get("version") == 4, f"the file is format version 4 ({header and header.get('version')})")
        check(header.get("has_elem_birth") and header.get("has_elem_death") and header.get("has_elem_group"),
              "the header declares element birth, death and group")
        check(header.get("has_temperature") is False, "the header says there is no temperature field")
        names = [a["name"] for a in header.get("arrays", [])]
        check("T" not in names and "elem_birth" in names and "elem_death" in names, f"the arrays are the ones the header declares ({names[:6]}...)")

        print("== a second session reads the same build")
        c2 = Client(["--embedded"] + common)
        c2.initialize()
        try:
            call_ok(c2, "project_open", {"path": project_dir}, "project_open in a second session")
            again = call_ok(c2, "results_query", {"job_id": job, "quantity": "displacement", "component": "z"}, "results_query in the second session")
            a = (again.get("statistics") or {}).get("max")
            b = (last.get("statistics") or {}).get("max")
            check(a is not None and b is not None and abs(a - b) <= 1e-9 * max(1.0, abs(b)), f"the reopened run gives the same displacement ({a} vs {b})")
        finally:
            c2.close()

        print("== the whole-part cross-check mode")
        v = call_ok(c, "lpbf_build_run", dict(args, mode="whole_part", label="whole part"), "whole_part build")
        st2 = wait_job(c, v.get("job_id", ""))
        check(st2.get("state") == "succeeded", f"the whole-part build succeeded ({st2.get('state')})")
        r2 = (st2.get("summary") or {}).get("results") or {}
        check(r2.get("layers") == 1, "the whole part is one layer")
        check(abs(r2.get("tip_uz_after_cut_mm", 0) - res.get("tip_uz_after_cut_mm", 0)) > 1e-6,
              "the whole part and the layer-by-layer build do not give the same answer, which is the point of the method")
        print(f"   layer by layer {res.get('tip_uz_after_cut_mm'):+.4f} mm against whole part {r2.get('tip_uz_after_cut_mm'):+.4f} mm")

        print("== plasticity")
        yield_ok = {"yield_strength": "292 MPa", "hardening_modulus": "1.95 GPa", "provenance": "published",
                    "source": "a test value, standing in for the library record"}
        call_err(c, "lpbf_build_run", dict(args, plasticity={k: v for k, v in yield_ok.items() if k != "source"}),
                 "INVALID_PARAMS", "a yield strength without a source is refused by the schema")
        call_err(c, "lpbf_build_run", dict(args, plasticity={"hardening_modulus": "1 GPa", "provenance": "user",
                                                             "source": "x"}),
                 "INVALID_PARAMS", "plasticity without a yield strength is refused by the schema")
        v = call_ok(c, "lpbf_build_run", dict(args, plasticity=dict(yield_ok, yield_strength="100 GPa"),
                                              label="yield far above any stress"), "plastic build that never yields")
        st5 = wait_job(c, v.get("job_id", ""))
        r5 = (st5.get("summary") or {}).get("results") or {}
        check(abs(r5.get("tip_uz_after_cut_mm", 9) - res.get("tip_uz_after_cut_mm", 0)) <= 1e-12,
              f"a yield stress nothing reaches gives the elastic answer to the last digit "
              f"({r5.get('tip_uz_after_cut_mm')} against {res.get('tip_uz_after_cut_mm')})")
        check((r5.get("plasticity") or {}).get("yielded") is False, "and reports that nothing yielded")
        v = call_ok(c, "lpbf_build_run", dict(args, plasticity=dict(yield_ok, yield_strength="60 MPa",
                                                                    hardening_modulus="2 GPa"),
                                              label="low yield, hardening"), "plastic build that yields")
        st6 = wait_job(c, v.get("job_id", ""))
        check(st6.get("state") == "succeeded", f"the plastic build succeeded ({st6.get('state')}: "
                                               f"{json.dumps(st6.get('error'))[:300]})")
        r6 = (st6.get("summary") or {}).get("results") or {}
        p6 = r6.get("plasticity") or {}
        check(p6.get("yielded") is True and p6.get("peak_equivalent_plastic_strain", 0) > 0,
              f"a low yield stress yields ({p6.get('peak_equivalent_plastic_strain')})")
        cap = 60.0 + 2000.0 * (p6.get("peak_equivalent_plastic_strain") or 0)
        check(r6.get("peak_von_mises_before_cut_mpa", 1e9) <= cap * (1 + 1e-6),
              f"the peak von Mises stays under the hardened yield stress sigma_y + H alpha "
              f"({r6.get('peak_von_mises_before_cut_mpa')} MPa against {cap:.2f})")
        check(p6.get("newton_iterations", 0) > 0, "the plastic build reports what the Newton loop cost")
        print(f"   elastic {res.get('tip_uz_after_cut_mm'):+.4f} mm, yielding {r6.get('tip_uz_after_cut_mm'):+.4f} mm, "
              f"peak plastic strain {p6.get('peak_equivalent_plastic_strain'):.5f}")

        print("== supports")
        SUP = {"stiffness_fraction": 0.3, "provenance": "assumed",
               "source": "a test value; no published default lattice stiffness is known to this project"}
        # a box standing on the plate has no downward-facing surface over empty space: no supports
        v = call_ok(c, "lpbf_build_run", dict(args, supports=SUP, label="box with supports"), "box with supports")
        stb = wait_job(c, v.get("job_id", ""))
        rb = (stb.get("summary") or {}).get("results") or {}
        check(stb.get("state") == "succeeded" and "supports" not in rb,
              "a part with no overhang gets no support")
        check(abs(rb.get("tip_uz_after_cut_mm", 9) - res.get("tip_uz_after_cut_mm", 0)) <= 1e-12,
              "and builds exactly as it does without the supports option")
        # a bridge on two legs: legs x 0-3 and 9-12, span x 0-12 at z 4-6, 4 wide; the gap under the span is
        # 6 x 4 x 4 cells, so exactly 96 support elements at 1 mm
        cells = {(i, j, k) for i in range(12) for j in range(4) for k in range(6)
                 if k >= 4 or i < 3 or i >= 9}
        bridge = tmp / "bridge.stl"
        voxel_stl(bridge, cells)
        call_ok(c, "project_create", {"name": "lpbf_bridge", "description": "a bridge on two legs"})
        call_ok(c, "geometry_import", {"path": str(bridge), "units": "mm", "name": "bridge"})
        call_ok(c, "material_define", {"material": {
            "id": "alsi10mg_test", "name": "AlSi10Mg, test values", "family": "metal", "status": "user_supplied",
            "processes": ["lpbf"], "provenance": "test values, only to exercise the operation",
            "density_kg_m3": {"value": 2680}, "youngs_modulus_pa": {"value": 70e9}, "poisson_ratio": {"value": 0.33}}})
        call_ok(c, "material_assign", {"body": "bridge", "material": "alsi10mg_test", "source": "user"})
        call_ok(c, "mesh_generate", {"element_size": "1 mm"})
        bargs = {"body": "bridge", "build_orientation": "X", "layer_thickness_sim": "1 mm", "inherent_strain": STRAIN,
                 "material": MATERIAL}
        v = call_ok(c, "lpbf_build_run", dict(bargs, label="bridge without supports"), "bridge without supports")
        s_no = wait_job(c, v.get("job_id", ""))
        r_no = (s_no.get("summary") or {}).get("results") or {}
        v = call_ok(c, "lpbf_build_run", dict(bargs, supports=dict(SUP, remove=True), label="bridge with supports"),
                    "bridge with supports")
        s_yes = wait_job(c, v.get("job_id", ""))
        check(s_yes.get("state") == "succeeded", f"the bridge builds on its supports ({s_yes.get('state')}: "
                                                 f"{json.dumps(s_yes.get('error'))[:200]})")
        r_yes = (s_yes.get("summary") or {}).get("results") or {}
        sp = r_yes.get("supports") or {}
        check(sp.get("elements") == 96, f"exactly the 96 cells under the span become support ({sp.get('elements')})")
        check(sp.get("part_elements") == len(cells), f"the part keeps its {len(cells)} elements ({sp.get('part_elements')})")
        d_no, d_yes = r_no.get("max_displacement_before_cut_mm", 0), r_yes.get("max_displacement_before_cut_mm", 0)
        check(abs(d_no - d_yes) > 1e-6 * max(d_no, 1e-9),
              f"the supports change how the bridge builds ({d_no:.5f} mm without, {d_yes:.5f} mm with)")
        nvt = Path(v.get("run_directory", "")) / "results.nvt"
        group = read_results_array(nvt, "elem_group") or []
        death = read_results_array(nvt, "elem_death") or []
        birth = read_results_array(nvt, "elem_birth") or []
        sup_idx = [i for i, g in enumerate(group) if g == 1]
        check(len(sup_idx) == 96, f"the result file marks the 96 supports as group 1 ({len(sup_idx)})")
        check(sup_idx and all(death[i] >= 0 for i in sup_idx) and all(death[i] > birth[i] for i in sup_idx),
              "every support is born with its layer and dies when it is removed")
        check(all(death[i] < 0 for i, g in enumerate(group) if g == 0), "no part element dies (there is no cut)")
        job_yes = (v or {}).get("job_id", "")
        q = call_ok(c, "results_query", {"job_id": job_yes, "quantity": "displacement"}, "results of the supported bridge")
        check(len(json.dumps(q)) > 50, "the supported build is readable through the ordinary result operations")
        print(f"   bridge: {sp.get('elements')} support elements; largest displacement {d_no:.5f} mm without, "
              f"{d_yes:.5f} mm with, {sp.get('max_displacement_after_support_removal_mm'):.5f} mm after removing them")

        print("== the support rule")
        def supports_of(stl_path, name, rule="overhang"):
            call_ok(c, "project_create", {"name": f"lpbf_{name}", "description": f"supports on {name}"})
            call_ok(c, "geometry_import", {"path": str(stl_path), "units": "mm", "name": name})
            call_ok(c, "material_define", {"material": {
                "id": "alsi10mg_test", "name": "AlSi10Mg, test values", "family": "metal", "status": "user_supplied",
                "processes": ["lpbf"], "provenance": "test values, only to exercise the operation",
                "density_kg_m3": {"value": 2680}, "youngs_modulus_pa": {"value": 70e9}, "poisson_ratio": {"value": 0.33}}})
            call_ok(c, "material_assign", {"body": name, "material": "alsi10mg_test", "source": "user"})
            call_ok(c, "mesh_generate", {"element_size": "1 mm"})
            vv = call_ok(c, "lpbf_build_run", {"body": name, "build_orientation": "X", "layer_thickness_sim": "1 mm",
                                               "inherent_strain": STRAIN, "material": MATERIAL,
                                               "supports": dict(SUP, rule=rule), "label": f"{name} {rule}"},
                         f"{name} with {rule} supports")
            ss = wait_job(c, vv.get("job_id", ""))
            return ((ss.get("summary") or {}).get("results") or {}).get("supports") or {}, ss.get("state")
        # a block with a channel through it along x, 4 x 4 mm, 3 mm above the plate: the channel's ceiling faces down,
        # and the column under it lands on the channel's floor, which is part
        chan = {(i, j, k) for i in range(16) for j in range(10) for k in range(10) if not (3 <= j < 7 and 3 <= k < 7)}
        chan_stl = tmp / "channel.stl"
        voxel_stl(chan_stl, chan)
        sp_ch, st_ch = supports_of(chan_stl, "channel")
        check(st_ch == "succeeded" and not sp_ch.get("elements"),
              f"a closed channel gets no support inside (overhang rule: {sp_ch.get('elements', 0)} elements)")
        sp_ch_old, _ = supports_of(chan_stl, "channel_old", "every_column")
        check(sp_ch_old.get("elements", 0) == 16 * 4 * 4,
              f"the wave-4 rule filled the channel (every_column: {sp_ch_old.get('elements')} elements, 256 expected)")
        sp_ch_all, _ = supports_of(chan_stl, "channel_all", "overhang_all")
        check(sp_ch_all.get("elements", 0) == 16 * 4 * 4 and sp_ch_all.get("rule") == "overhang_all",
              f"the overhang_all sensitivity supports the channel ceiling down to its floor ({sp_ch_all.get('elements')} "
              f"elements, 256 expected)")
        lean60 = tmp / "lean60.stl"
        prism_stl(lean60, 60)
        sp60, st60 = supports_of(lean60, "lean60")
        check(st60 == "succeeded" and sp60.get("elements", 0) > 0,
              f"a face 30 degrees from the horizontal gets support ({sp60.get('elements')} elements, "
              f"{sp60.get('supported_faces')} faces)")
        check(str(sp60.get("critical_angle_source", "")).startswith("assumed"),
              f"an angle not given is reported as assumed, not as given ({sp60.get('critical_angle_source', '')[:60]})")
        lean30 = tmp / "lean30.stl"
        prism_stl(lean30, 30)
        sp30, st30 = supports_of(lean30, "lean30")
        check(st30 == "succeeded" and not sp30.get("elements"),
              f"a face 60 degrees from the horizontal gets none (steeper than 45; "
              f"{sp30.get('downward_faces_steeper_than_the_angle', 0)} downward voxel faces judged steep)")
        print(f"   channel: {sp_ch.get('elements', 0)} support elements (wave-4 rule: {sp_ch_old.get('elements')}, "
              f"overhang_all: {sp_ch_all.get('elements')}); "
              f"lean 60: {sp60.get('elements')}; lean 30: {sp30.get('elements', 0)}")

        print("== typed supports (docs/contracts/supports.md S7 to S9)")
        # the bridge again: legs x 0-3 and 9-12, deck z 4-6; block supports with their assumed defaults
        call_ok(c, "project_create", {"name": "lpbf_bridge_typed", "description": "typed supports on the bridge"})
        call_ok(c, "geometry_import", {"path": str(bridge), "units": "mm", "name": "bridge"})
        call_ok(c, "material_define", {"material": {
            "id": "alsi10mg_test", "name": "AlSi10Mg, test values", "family": "metal", "status": "user_supplied",
            "processes": ["lpbf"], "provenance": "test values, only to exercise the operation",
            "density_kg_m3": {"value": 2680}, "youngs_modulus_pa": {"value": 70e9}, "poisson_ratio": {"value": 0.33}}})
        call_ok(c, "material_assign", {"body": "bridge", "material": "alsi10mg_test", "source": "user"})
        call_ok(c, "mesh_generate", {"element_size": "1 mm"})
        gen = {"body": "bridge", "layer_thickness_sim": "1 mm", "material": MATERIAL}
        g = call_ok(c, "lpbf_supports_generate", dict(gen, supports={"type": "block"}), "block supports generated")
        sp = g.get("supports") or {}
        check(g.get("support_elements") == 96, f"S7: block supports fill the bridge's span only ({g.get('support_elements')} of 96)")
        band = (sp.get("homogenised") or {}).get("bands") or [{}]
        check(abs(sp.get("volume_mm3", 0) - 96 * band[0].get("solid_fraction", -1)) < 1e-9,
              f"S7: the support volume is the counted voxels times their solid fraction ({sp.get('volume_mm3')} mm3)")
        check(set(sp.get("assumed") or []) >= {"wall_thickness", "spacing", "relative_density"},
              f"S8: every default used is listed as assumed ({sp.get('assumed')})")
        check(all(q.get("source") for q in sp.get("parameters") or []), "S8: every parameter carries its source")
        g2 = call_ok(c, "lpbf_supports_generate", dict(gen, supports={"type": "block", "offset_from_part": {
            "value": "1 mm", "provenance": "user", "source": "flow test"}}), "block supports with an offset")
        check(g2.get("support_elements") == 64 and (g2.get("supports") or {}).get("columns_dropped_for_offset") == 8,
              f"S7: an offset of 1 mm keeps the columns next to the legs free ({g2.get('support_elements')} of 64, "
              f"{(g2.get('supports') or {}).get('columns_dropped_for_offset')} columns dropped)")
        call_err(c, "lpbf_supports_generate", dict(gen, supports={"type": "block", "wall_thickness": "0.3 mm"}),
                 "PRECONDITION_FAILED", "S8: a parameter without a source is refused")
        call_err(c, "lpbf_supports_generate", dict(gen, supports={"type": "scaffold"}), "INVALID_PARAMS",
                 "S8: an unknown support type is refused")
        call_err(c, "lpbf_supports_generate", dict(gen, supports={"type": "block", "wall_thickness": "3 mm", "provenance": "user",
                 "source": "flow test"}), "PRECONDITION_FAILED", "S8: a wall thicker than its pitch is refused")
        for t in ("thin_wall", "cone", "tree", "lattice"):
            gt = call_ok(c, "lpbf_supports_generate", dict(gen, supports={"type": t}), f"{t} supports generated")
            bt = ((gt.get("supports") or {}).get("homogenised") or {}).get("bands") or [{}]
            check(gt.get("support_elements") == 96 and 0 < bt[0].get("stiffness_vertical", 0) < 1,
                  f"S7: {t} supports on the span, homogenised stiffness {bt[0].get('stiffness_vertical')}")
        v = call_ok(c, "lpbf_build_run", dict(bargs, supports={"type": "block", "remove": True}), "the bridge on block supports")
        stt = wait_job(c, v.get("job_id", ""))
        rt = ((stt.get("summary") or {}).get("results") or {}).get("supports") or {}
        check(stt.get("state") == "succeeded" and rt.get("elements") == 96,
              f"the build uses exactly the generated supports ({rt.get('elements')})")
        nvt = Path(v.get("run_directory", "")) / "results.nvt"
        grp, dth = read_results_array(nvt, "elem_group") or [], read_results_array(nvt, "elem_death") or []
        check(grp and all(dth[i] >= 0 for i, gg in enumerate(grp) if gg == 1), "S9: after removal no support element is active")
        # a slab standing on its supports alone: the forces they exert on it balance
        slab = {(i, j, k) for i in range(6) for j in range(4) for k in range(2)}
        slab_stl = tmp / "slab.stl"
        voxel_stl(slab_stl, slab)
        call_ok(c, "project_create", {"name": "lpbf_slab", "description": "a slab on supports alone"})
        call_ok(c, "geometry_import", {"path": str(slab_stl), "units": "mm", "name": "slab"})
        call_ok(c, "geometry_place", {"body": "slab", "z_offset": "3 mm"})
        call_ok(c, "material_define", {"material": {
            "id": "alsi10mg_test", "name": "AlSi10Mg, test values", "family": "metal", "status": "user_supplied",
            "processes": ["lpbf"], "provenance": "test values, only to exercise the operation",
            "density_kg_m3": {"value": 2680}, "youngs_modulus_pa": {"value": 70e9}, "poisson_ratio": {"value": 0.33}}})
        call_ok(c, "material_assign", {"body": "slab", "material": "alsi10mg_test", "source": "user"})
        call_ok(c, "mesh_generate", {"element_size": "1 mm"})
        v = call_ok(c, "lpbf_build_run", {"body": "slab", "build_orientation": "X", "layer_thickness_sim": "1 mm",
                                          "inherent_strain": STRAIN, "material": MATERIAL,
                                          "supports": {"type": "block", "remove": True,
                                                       "height_above_plate": "3 mm"}}, "the slab on block supports")
        sts = wait_job(c, v.get("job_id", ""))
        rs = ((sts.get("summary") or {}).get("results") or {}).get("supports") or {}
        to = rs.get("tear_off") or {}
        res_n = sum(x * x for x in to.get("resultant_n", [1, 1, 1])) ** 0.5
        check(sts.get("state") == "succeeded" and rs.get("held_3_2_1_after_removal") and to.get("largest_nodal_force_n", 0) > 0
              and res_n < 1e-6 * to.get("largest_nodal_force_n", 1),
              f"S9: a slab on its supports alone: tear-off largest {to.get('largest_nodal_force_n')} N, resultant {res_n:.2e} N")
        call_err(c, "lpbf_build_run", {"body": "slab", "build_orientation": "X", "layer_thickness_sim": "1 mm",
                                       "inherent_strain": STRAIN, "material": MATERIAL,
                                       "supports": {"type": "block", "height_above_plate": "5 mm"}},
                 "PRECONDITION_FAILED", "a support height that does not match the placement is refused")
        print(f"   block on the bridge: {g.get('support_elements')} elements, {sp.get('volume_mm3'):.2f} mm3, contact "
              f"{sp.get('contact_area_mm2'):.2f} mm2; with a 1 mm offset {g2.get('support_elements')}; slab tear-off "
              f"{to.get('largest_nodal_force_n'):.3f} N largest, resultant {res_n:.1e} N")

        print("== the 3-2-1 hold on an adaptive mesh")
        # a slab on supports alone, 24 x 24 x 5 mm at 1 mm: its underside rests on supports, so the adaptive mesh coarsens
        # it; the force the supports release there sits on hanging nodes no element uses any more and must reach their
        # masters (without that the separated slab moved 15.9 times too far), and the hold after removal must not take a
        # hanging node
        wide = {(i, j, k) for i in range(24) for j in range(24) for k in range(5)}
        wide_stl = tmp / "wide.stl"
        voxel_stl(wide_stl, wide)
        res321 = {}
        for mode in ("uniform", "adaptive"):
            call_ok(c, "project_create", {"name": f"lpbf_wide_{mode}", "description": "a wide slab on supports"})
            call_ok(c, "geometry_import", {"path": str(wide_stl), "units": "mm", "name": "slab"})
            call_ok(c, "geometry_place", {"body": "slab", "z_offset": "3 mm"})
            call_ok(c, "material_define", {"material": {
                "id": "alsi10mg_test", "name": "AlSi10Mg, test values", "family": "metal", "status": "user_supplied",
                "processes": ["lpbf"], "provenance": "test values, only to exercise the operation",
                "density_kg_m3": {"value": 2680}, "youngs_modulus_pa": {"value": 70e9}, "poisson_ratio": {"value": 0.33}}})
            call_ok(c, "material_assign", {"body": "slab", "material": "alsi10mg_test", "source": "user"})
            call_ok(c, "mesh_generate", {"element_size": "1 mm"})
            req = {"body": "slab", "build_orientation": "X", "layer_thickness_sim": "1 mm", "inherent_strain": STRAIN,
                   "material": MATERIAL, "supports": {"type": "block", "remove": True}}
            if mode == "adaptive":
                req["adaptive_mesh"] = {"max_element_size": "8 mm"}
            v = call_ok(c, "lpbf_build_run", req, f"the wide slab, {mode}")
            stw = wait_job(c, v.get("job_id", ""), tries=300)
            rw = (stw.get("summary") or {}).get("results") or {}
            res321[mode] = (stw.get("state"), rw.get("max_displacement_before_cut_mm"),
                            (rw.get("supports") or {}).get("max_displacement_after_support_removal_mm"),
                            (rw.get("adaptive_mesh") or {}).get("hanging_nodes"))
        (su, pu, au, _), (sa, pa, aa, ha) = res321["uniform"], res321["adaptive"]
        check(su == sa == "succeeded" and (ha or 0) > 0, f"the adaptive slab has hanging nodes ({ha}) and both builds run")
        check(pu and pa and abs(pa / pu - 1) < 0.03 and au and aa and abs(aa / au - 1) < 0.03,
              f"released forces reach the masters of unused hanging nodes: the adaptive slab separates like the uniform one "
              f"(on the plate {pu:.5f} / "
              f"{pa:.5f} mm, separated {au:.5f} / {aa:.5f} mm)")

        print("== supports in the FFF print (heat model and stress history)")
        call_ok(c, "project_create", {"name": "fff_bridge_supports", "description": "the bridge printed in PLA on block supports"})
        call_ok(c, "geometry_import", {"path": str(bridge), "units": "mm", "name": "bridge"})
        call_ok(c, "material_assign", {"body": "bridge", "material": "pla_generic_demo", "source": "user"})
        call_ok(c, "mesh_generate", {"element_size": "1 mm"})
        proc = {"layer_height": "1 mm", "nozzle_temperature": "210 degC", "bed_temperature": "60 degC", "ambient_temperature": "30 degC",
                "deposition_rate": "8 mm^3/s", "min_layer_time": "8 s", "cooldown_bed_on": "30 s", "cooldown_bed_off": "30 s",
                "thermal_substeps": 2, "provenance": "user"}
        v = call_ok(c, "mech_print_run", {"process": proc, "supports": {"type": "block"}}, "the bridge printed on block supports")
        stf = wait_job(c, v.get("job_id", ""), tries=600)
        rf = (stf.get("summary") or {}).get("results") or {}
        spf = rf.get("supports") or {}
        check(stf.get("state") == "succeeded" and spf.get("elements") == 96 and spf.get("removed_after_release") == 96,
              f"the print deposits the 96 support elements and removes them after the release ({spf.get('elements')}, "
              f"{spf.get('removed_after_release')})")
        check(rf.get("heat_into_bed_during_print_j", 0) > 0 and (spf.get("tear_off") or {}).get("largest_nodal_force_n", -1) >= 0,
              f"the print reports the heat into the bed ({rf.get('heat_into_bed_during_print_j')} J) and the tear-off")
        bf = ((spf.get("homogenised") or {}).get("bands") or [{}])[0]
        check(bf.get("surface_per_volume_per_mm", 0) > 0, f"the band carries its surface per volume ({bf.get('surface_per_volume_per_mm')} per mm)")

        print("== an adaptive mesh")
        # a block thick enough to have an inside: 20 x 12 x 6 mm at 1 mm, coarse up to 2 mm with a one-voxel band
        blk = tmp / "slab.stl"
        box_stl(blk, (0, 0, 0), (20, 12, 6))
        call_ok(c, "project_create", {"name": "lpbf_adaptive", "description": "a slab on an adaptive mesh"})
        call_ok(c, "geometry_import", {"path": str(blk), "units": "mm", "name": "slab"})
        call_ok(c, "material_define", {"material": {
            "id": "alsi10mg_test", "name": "AlSi10Mg, test values", "family": "metal", "status": "user_supplied",
            "processes": ["lpbf"], "provenance": "test values, only to exercise the operation",
            "density_kg_m3": {"value": 2680}, "youngs_modulus_pa": {"value": 70e9}, "poisson_ratio": {"value": 0.33}}})
        call_ok(c, "material_assign", {"body": "slab", "material": "alsi10mg_test", "source": "user"})
        call_ok(c, "mesh_generate", {"element_size": "1 mm"})
        sargs = {"body": "slab", "build_orientation": "X", "layer_thickness_sim": "1 mm", "inherent_strain": STRAIN,
                 "material": MATERIAL}
        v = call_ok(c, "lpbf_build_run", dict(sargs, adaptive_mesh={"max_element_size": "2 mm", "fine_band_voxels": 1},
                                              label="adaptive slab"), "adaptive build")
        model = v.get("model") or {}
        check(model.get("hanging_nodes", 0) > 0 and model.get("elements", 0) < model.get("uniform_elements", 0),
              f"the adaptive mesh has fewer elements than the uniform one and hanging nodes between them "
              f"({model.get('elements')} against {model.get('uniform_elements')}, {model.get('hanging_nodes')} hanging)")
        sta = wait_job(c, v.get("job_id", ""))
        check(sta.get("state") == "succeeded", f"the adaptive build succeeded ({sta.get('state')}: {json.dumps(sta.get('error'))[:200]})")
        nvt = Path(v.get("run_directory", "")) / "results.nvt"
        hdr = read_results_header(nvt) or {}
        counts = {a["name"]: a["count"] for a in hdr.get("arrays", [])}
        ne, nn = hdr.get("nelems") or counts.get("elem_group"), hdr.get("nnodes") or counts.get("fixed")
        check(counts.get("conn") == 8 * ne, f"the result file holds hex8 elements only ({counts.get('conn')} = 8 x {ne})")
        xyz = read_results_array(nvt, "xyz")
        conn = read_results_array(nvt, "conn")
        mu = read_results_array(nvt, "mech_u")
        last = len(mu) // (3 * nn) - 1
        u = mu[3 * nn * last:3 * nn * (last + 1)]
        # a node at the middle of a coarse element's top edge is governed by the constraint from its first use (it is laid
        # on that face), so it must carry the mean of the edge ends; a bottom-edge node belongs to the layer below and
        # moved on its own before the coarse element was laid on it, so it is not checked
        pos = {tuple(round(x * 1e6) for x in xyz[3 * n:3 * n + 3]): n for n in range(nn)}
        worst, checked = 0.0, 0
        for e in range(ne):
            nds = conn[8 * e:8 * e + 8]
            pts = [xyz[3 * n:3 * n + 3] for n in nds]
            if max(p[0] for p in pts) - min(p[0] for p in pts) < 1.5e-3:
                continue  # a voxel: no hanging node on it
            ztop = max(p[2] for p in pts)
            for a in range(8):
                for b in range(a + 1, 8):
                    if abs(pts[a][2] - ztop) > 1e-9 or abs(pts[b][2] - ztop) > 1e-9:
                        continue
                    if abs(pts[a][0] - pts[b][0]) > 1e-9 and abs(pts[a][1] - pts[b][1]) > 1e-9:
                        continue  # a diagonal, not an edge
                    mid = tuple(round((pts[a][k] + pts[b][k]) / 2 * 1e6) for k in range(3))
                    m = pos.get(mid)
                    if m is None:
                        continue
                    for k in range(3):
                        ref = 0.5 * (u[3 * nds[a] + k] + u[3 * nds[b] + k])
                        worst = max(worst, abs(u[3 * m + k] - ref))
                    checked += 1
        umax = max(abs(x) for x in u) or 1.0
        check(checked > 0 and worst <= 1e-12 * umax,
              f"the {checked} hanging nodes on coarse top edges carry the mean of their edge ends in the file (worst {worst / umax:.1e} of the largest displacement)")
        print(f"   adaptive slab: {model.get('elements')} elements against {model.get('uniform_elements')} uniform, "
              f"{model.get('hanging_nodes')} hanging nodes; {checked} top-edge hanging nodes checked in the file, worst "
              f"{worst / umax:.1e} of the largest displacement")
        q = call_ok(c, "results_query", {"job_id": v.get("job_id"), "quantity": "displacement"}, "results of the adaptive build")
        check(len(json.dumps(q)) > 50, "the adaptive build is readable through the ordinary result operations")

        print("== fitting an inherent strain of our own")
        # a bar long enough to bow: the 12 mm block's free-end face reaches the plate, where u_z is held at zero
        bar = tmp / "bar.stl"
        box_stl(bar, (0, 0, 0), (24, 4, 4))
        call_ok(c, "project_create", {"name": "lpbf_fit", "description": "a bar to fit an inherent strain on"})
        call_ok(c, "geometry_import", {"path": str(bar), "units": "mm", "name": "bar"})
        call_ok(c, "material_define", {"material": {
            "id": "alsi10mg_test", "name": "AlSi10Mg, test values", "family": "metal", "status": "user_supplied",
            "processes": ["lpbf"], "provenance": "the elastic constants of the validation data, used here only to "
                                                 "exercise the operation",
            "density_kg_m3": {"value": 2680}, "youngs_modulus_pa": {"value": 70e9}, "poisson_ratio": {"value": 0.33}}})
        call_ok(c, "material_assign", {"body": "bar", "material": "alsi10mg_test", "source": "user"})
        call_ok(c, "mesh_generate", {"element_size": "1 mm"})
        fit_cut = {"height": "1 mm", "kerf": "0.5 mm", "from_x": "6 mm", "provenance": "assumed"}
        cal = {"body": "bar", "strategy": "smoke", "layer_thickness_sim": "1 mm", "material": MATERIAL, "cut": fit_cut,
               "measured": {"tip_uz_after_cut_x": "0.02 mm", "tip_uz_after_cut_y": "0.05 mm",
                            "source": "invented for this test, not a measurement", "provenance": "assumed"},
               "fit": {"tolerance_pct": 1.0, "max_builds": 12}}
        err = call_err(c, "lpbf_calibrate", {k: v for k, v in cal.items() if k != "measured"},
                       "INVALID_PARAMS", "a fit without the measured deflections is refused by the schema")
        check("measured" in json.dumps(err).lower(), "the refusal says which measurement is missing")
        call_err(c, "lpbf_calibrate", dict(cal, measured=dict(cal["measured"], provenance="")), "PRECONDITION_FAILED",
                 "a measurement without a provenance is refused by the operation")
        call_err(c, "lpbf_calibrate", dict(cal, measured={k: v for k, v in cal["measured"].items() if k != "source"}),
                 "INVALID_PARAMS", "a measurement without a source is refused by the schema")
        call_err(c, "lpbf_calibrate", dict(cal, mode="whole_part"), "INVALID_PARAMS",
                 "fitting with the whole-part model is refused: it has no build history")
        call_err(c, "lpbf_calibrate", dict(cal, fit=dict(cal["fit"], max_builds=2)), "INVALID_PARAMS",
                 "a fit that is given too few builds is refused")
        v = call_ok(c, "lpbf_calibrate", cal, "lpbf_calibrate")
        st3 = wait_job(c, v.get("job_id", ""))
        check(st3.get("state") == "succeeded", f"the fit succeeded ({st3.get('state')})")
        s3 = st3.get("summary") or {}
        check(s3.get("converged") is True, "the fit converged")
        got, eps = s3.get("achieved") or {}, s3.get("inherent_strain") or {}
        check(abs(got.get("error_x_pct", 99)) < 1.0 and abs(got.get("error_y_pct", 99)) < 1.0,
              f"both deflections are within the tolerance ({got.get('error_x_pct'):+.2g} %, {got.get('error_y_pct'):+.2g} %)")
        check(eps.get("ezz") == -0.03, "ezz was held, not fitted")
        check("fitted by navier lpbf_calibrate" in eps.get("source", ""),
              "the fitted tensor's source names the fit and the case it was fitted on")
        check(len(s3.get("history") or []) >= 3, "the iteration history is in the summary")
        check((s3.get("fit") or {}).get("builds", 99) <= 12, "the fit stayed inside its build budget")
        print(f"   fitted exx {eps.get('exx'):+.6f}, eyy {eps.get('eyy'):+.6f} in {(s3.get('fit') or {}).get('builds')} builds")
        print("== the fitted strain reproduces the deflection it was fitted to")
        v = call_ok(c, "lpbf_build_run", dict(args, body="bar", cut=fit_cut, inherent_strain={
            "exx": eps["exx"], "eyy": eps["eyy"], "ezz": eps["ezz"], "provenance": "calibrated",
            "source": eps["source"]}, build_orientation="X", label="rebuild at the fitted strain"), "rebuild")
        st4 = wait_job(c, v.get("job_id", ""))
        r4 = (st4.get("summary") or {}).get("results") or {}
        check(abs(r4.get("tip_uz_after_cut_mm", 0) - 0.02) < 2e-4,
              f"the rebuild at the fitted strain gives the target deflection ({r4.get('tip_uz_after_cut_mm'):.5f} mm against 0.02)")
    finally:
        code, stderr = c.close()
        if FAIL:
            print(stderr[-3000:])
        shutil.rmtree(tmp, ignore_errors=True)
    if FAIL:
        print(f"\nLPBF FLOW TESTS FAILED: {PASS} passed, {FAIL} failed")
        return 1
    print(f"\nALL LPBF FLOW TESTS PASSED: {PASS} passed, 0 failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
