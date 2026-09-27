#!/usr/bin/env python3
"""restartflow.py - durable checkpoint and restart of transient runs, driven over MCP as an AI host drives them.

Criteria were fixed before the first run: "Thermal Sim/verification/stageB-criteria.md" (B1..B8). Every resumed run
is compared with an uninterrupted run of the identical specification: stored frames, energy budget, step counts and
the accepted-step time history read back from results.nvt.

Run from the repository root after `make am`:
    python3 tools/restartflow.py
"""
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcptest import SERVER, Client  # noqa: E402

PASS = 0
FAIL = 0
TOL = 1e-12


def check(cond, what):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"  FAIL: {what}")


def call_ok(c, name, args, what=None):
    r = c.call(name, args)
    res = r.get("result", {})
    sc = res.get("structuredContent") or {}
    ok = res.get("isError") is False and sc.get("ok") is True
    check(ok, f"{what or name}: {json.dumps(sc.get('error'))[:600] if not ok else ''}")
    return sc.get("value", {}), r


def poll(c, name, args):
    """a status poll: not a check, so it does not inflate the count"""
    r = c.call(name, args)
    return (r.get("result", {}).get("structuredContent") or {}).get("value", {})


def call_err(c, name, args):
    r = c.call(name, args)
    sc = (r.get("result", {}) or {}).get("structuredContent") or {}
    return sc.get("error") or {}


def box_stl(path, lx, ly, lz):
    p = [(0, 0, 0), (lx, 0, 0), (lx, ly, 0), (0, ly, 0), (0, 0, lz), (lx, 0, lz), (lx, ly, lz), (0, ly, lz)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, cc, d in quads:
        tris += [(p[a], p[b], p[cc]), (p[a], p[cc], p[d])]
    with open(path, "wb") as f:
        f.write(b"restartflow bar".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


def read_nvt(path):
    """results.nvt: magic, uint64 header length, JSON header, then the arrays listed in the header"""
    data = Path(path).read_bytes()
    assert data[:8] == b"NVTHR001", "bad results magic"
    hl = struct.unpack("<Q", data[8:16])[0]
    header = json.loads(data[16:16 + hl])
    off = 16 + hl
    size = {"d": 8, "i": 4, "b": 1, "c": 1}
    fmt = {"d": "d", "i": "i", "b": "B", "c": "b"}
    arrays = {}
    for a in header["arrays"]:
        n = a["count"] * size[a["type"]]
        arrays[a["name"]] = struct.unpack(f"<{a['count']}{fmt[a['type']]}", data[off:off + n]) if n else ()
        off += n
    return header, arrays


def wait_state(c, job_id, states=("succeeded", "failed", "cancelled", "paused"), limit=6000):
    for _ in range(limit):
        v = poll(c, "job_status", {"job_id": job_id})
        if v.get("state") in states:
            return v
        time.sleep(0.05)
    check(False, f"job {job_id} did not reach {states}")
    return {}


def compare_runs(tag, dir_a, dir_b, sum_a, sum_b, mech=False):
    """B1/B8 equivalence of two finished runs of the same specification"""
    ha, aa = read_nvt(Path(dir_a) / "results.nvt")
    hb, ab = read_nvt(Path(dir_b) / "results.nvt")
    ta, tb = aa["times"], ab["times"]
    check(ta == tb, f"{tag}: identical stored times ({len(ta)} vs {len(tb)})")
    check(all(tb[i] < tb[i + 1] for i in range(len(tb) - 1)), f"{tag}: stored times strictly increasing (no duplicate records)")
    Ta, Tb = aa["T"], ab["T"]
    scale = max(abs(x) for x in Ta) if Ta else 1
    dT = max((abs(x - y) for x, y in zip(Ta, Tb)), default=0) if len(Ta) == len(Tb) else float("inf")
    check(dT <= TOL * scale, f"{tag}: every stored temperature frame within {TOL:g} relative (max difference {dT:.3g} K)")
    bitwise = Ta == Tb
    ea, eb = sum_a.get("energy", {}), sum_b.get("energy", {})
    for key in ("sources_j", "boundary_j", "prescribed_nodes_j", "stored_j", "enthalpy_change_j"):
        x, y = ea.get(key, 0), eb.get(key, 0)
        check(abs(x - y) <= TOL * max(abs(x), abs(y), 1e-300), f"{tag}: energy {key} {x!r} vs {y!r}")
    ia, ib = sum_a.get("time_integration", {}), sum_b.get("time_integration", {})
    for key in ("accepted_steps", "rejected_steps", "attempts"):
        check(ia.get(key) == ib.get(key), f"{tag}: {key} {ia.get(key)} vs {ib.get(key)}")
    check(aa.get("step_t") == ab.get("step_t") and aa.get("step_dt") == ab.get("step_dt"),
          f"{tag}: identical accepted-step history ({len(aa.get('step_t', ()))} vs {len(ab.get('step_t', ()))} steps)")
    if mech:
        ua, ub = aa["mech_u"], ab["mech_u"]
        du = max((abs(x - y) for x, y in zip(ua, ub)), default=0)
        umax = max((abs(x) for x in ua), default=1)
        check(len(ua) == len(ub) and du <= TOL * umax, f"{tag}: displacement frames within tolerance (max difference {du:.3g} m)")
        va, vb = aa["mech_vm"], ab["mech_vm"]
        dv = max((abs(x - y) for x, y in zip(va, vb)), default=0)
        check(len(va) == len(vb) and dv <= TOL * max((abs(x) for x in va), default=1), f"{tag}: von Mises frames within tolerance ({dv:.3g} Pa)")
        bitwise = bitwise and ua == ub and va == vb
    return bitwise


def main():
    tmp = Path(tempfile.mkdtemp(prefix="nvrestart"))
    ws = tmp / "ws"
    stl = tmp / "bar.stl"
    box_stl(stl, 100.0, 10.0, 10.0)
    common = ["--workspace", str(ws), "--allow-read", str(tmp)]
    c = Client(["--embedded"] + common)
    c.initialize()

    print("== setup")
    call_ok(c, "project_create", {"name": "restart_bar", "description": "heated bar for checkpoint and restart"})
    call_ok(c, "geometry_import", {"path": str(stl), "units": "mm", "name": "bar"})
    call_ok(c, "material_define", {"material": {
        "id": "test_alloy", "name": "Verification alloy", "family": "metal", "status": "user_supplied",
        "provenance": "constant properties for the restart test",
        "density_kg_m3": {"value": 8000.0}, "conductivity_w_per_mk": {"value": 20.0}, "specific_heat_j_per_kgk": {"value": 500.0},
        "youngs_modulus_pa": {"value": 200e9}, "poisson_ratio": {"value": 0.3}, "expansion_1_per_k": {"value": 1.2e-5}}})
    call_ok(c, "material_assign", {"body": "bar", "material": "test_alloy", "source": "user"})
    call_ok(c, "mesh_generate", {"element_size": "1.25 mm"})
    call_ok(c, "selection_create", {"name": "hot_end", "query": {"plane": {"axis": "x", "at": "min"}}, "body": "bar"})
    call_ok(c, "selection_create", {"name": "cold_end", "query": {"plane": {"axis": "x", "at": "max"}}, "body": "bar"})
    call_ok(c, "boundary_apply", {"name": "hot", "kind": "temperature", "selection": "hot_end", "temperature": "250 degC", "source": "user",
                                  "schedule": [{"time": 0, "factor": 1}, {"time": "900 s", "factor": 0}]})
    call_ok(c, "boundary_apply", {"name": "air", "kind": "convection", "selection": "cold_end", "source": "user",
                                  "convection": {"coefficient": 40, "ambient": "20 degC"}})
    call_ok(c, "boundary_apply", {"name": "heater", "kind": "heat_source", "body": "bar", "power_density": "3e5 W/m^3", "source": "user",
                                  "schedule": [{"time": 0, "factor": 0}, {"time": "300 s", "factor": 1}, {"time": "700 s", "factor": 0}]})
    call_ok(c, "project_save", {})
    RUN = {"analysis": "transient_thermal", "end_time": "1500 s", "time_stepping": "adaptive", "temporal_relative_tolerance": 1e-5,
           "temporal_temperature_tolerance": "0.001 K", "initial_temperature": "20 degC", "output_interval": "100 s",
           "checkpoint_wall_interval": 0, "allow_duplicate": True}

    print("== reference: uninterrupted run")
    t0 = time.time()
    v, _ = call_ok(c, "analysis_run", dict(RUN, label="uninterrupted"), "uninterrupted run")
    ref_id, ref_dir = v.get("job_id"), v.get("run_directory")
    ref = wait_state(c, ref_id)
    check(ref.get("state") == "succeeded", f"uninterrupted run succeeded ({ref.get('state')}: {json.dumps(ref.get('error'))[:300]})")
    ref_sum = ref.get("summary", {})
    wall = time.time() - t0
    print(f"  {ref_sum.get('time_integration', {}).get('accepted_steps')} accepted steps, {wall:.1f} s wall")

    print("== B1 pause mid-run and resume in the same session")
    v, _ = call_ok(c, "analysis_run", dict(RUN, label="paused"), "run to pause")
    pid, pdir = v.get("job_id"), v.get("run_directory")
    for _ in range(4000):
        st = poll(c, "job_status", {"job_id": pid})
        if st.get("progress", 0) >= 0.35 or st.get("state") != "running" and st.get("state") != "queued":
            break
        time.sleep(0.02)
    v, _ = call_ok(c, "job_pause", {"job_id": pid})
    check(v.get("requested") is True, f"pause requested while running ({v.get('state')})")
    st = wait_state(c, pid)
    ck = st.get("checkpoint", {})
    check(st.get("state") == "paused" and ck.get("available") and ck.get("time_s", 0) > 0,
          f"the job is paused with a checkpoint at t > 0 ({st.get('state')}, t = {ck.get('time_s')})")
    e = call_err(c, "results_query", {"job_id": pid, "quantity": "temperature"})
    check(e.get("code") == "PRECONDITION_FAILED" and "job_resume" in (e.get("hint") or ""), "a paused run publishes no results and names job_resume")
    v, _ = call_ok(c, "job_resume", {"job_id": pid})
    check(v.get("resumed_from_time_s") == ck.get("time_s") and v.get("frames_restored", 0) >= 1,
          f"resumed from the checkpoint time with its frames ({v.get('resumed_from_time_s')}, {v.get('frames_restored')} frames)")
    st = wait_state(c, pid)
    check(st.get("state") == "succeeded", f"the resumed run completes ({st.get('state')}: {json.dumps(st.get('error'))[:300]})")
    rs = st.get("summary", {}).get("restart", {})
    check(rs.get("resumed_from_s") == [ck.get("time_s")], f"the summary records the resume ({rs})")
    bw1 = compare_runs("B1", ref_dir, pdir, ref_sum, st.get("summary", {}))
    left = [n for n in ("frames.nvf", "checkpoint.nvc", "checkpoint.prev.nvc") if (Path(pdir) / n).exists()]
    check(not left, f"restart files are removed after completion ({left})")
    print(f"  paused at t = {ck.get('time_s'):.6g} s; resumed run {'bitwise identical' if bw1 else 'equal within tolerance'} to the uninterrupted run")

    print("== B7 checkpoint request on a running job")
    v, _ = call_ok(c, "analysis_run", dict(RUN, label="checkpointed"), "run to checkpoint")
    kid, kdir = v.get("job_id"), v.get("run_directory")
    for _ in range(4000):
        st = poll(c, "job_status", {"job_id": kid})
        if st.get("progress", 0) >= 0.2 or st.get("state") not in ("running", "queued"):
            break
        time.sleep(0.02)
    v, _ = call_ok(c, "job_checkpoint", {"job_id": kid})
    seen = False
    for _ in range(4000):
        st = poll(c, "job_status", {"job_id": kid})
        if st.get("checkpoint", {}).get("available"):
            seen = st.get("state") == "running" or st.get("state") == "succeeded"
            break
        if st.get("state") not in ("running", "queued"):
            break
        time.sleep(0.02)
    check(seen, "job_status reports the requested checkpoint")
    st = wait_state(c, kid)
    check(st.get("state") == "succeeded", f"a checkpoint does not stop the job ({st.get('state')})")
    bw = compare_runs("B7", ref_dir, kdir, ref_sum, st.get("summary", {}))
    print(f"  B7: {'bitwise identical' if bw else 'within tolerance'} to the uninterrupted run")

    print("== B4 cancellation keeps a resumable checkpoint")
    v, _ = call_ok(c, "analysis_run", dict(RUN, label="cancelled"), "run to cancel")
    cid, cdir = v.get("job_id"), v.get("run_directory")
    for _ in range(4000):
        st = poll(c, "job_status", {"job_id": cid})
        if st.get("progress", 0) >= 0.5 or st.get("state") not in ("running", "queued"):
            break
        time.sleep(0.02)
    call_ok(c, "job_cancel", {"job_id": cid})
    st = wait_state(c, cid)
    det = st.get("error", {}).get("details", {})
    check(st.get("state") == "cancelled" and det.get("checkpoint_available") is True and "partial" in det.get("partial_results_policy", ""),
          f"cancelled with a checkpoint and a stated partial-results policy ({st.get('state')}, {json.dumps(det)[:200]})")
    e = call_err(c, "results_query", {"job_id": cid, "quantity": "temperature"})
    check(e.get("code") == "PRECONDITION_FAILED" and "job_resume" in (e.get("hint") or ""), "results_query refuses the cancelled run and names job_resume")
    call_ok(c, "job_resume", {"job_id": cid})
    st = wait_state(c, cid)
    check(st.get("state") == "succeeded", f"the cancelled run is finished by job_resume ({st.get('state')})")
    bw = compare_runs("B4", ref_dir, cdir, ref_sum, st.get("summary", {}))
    print(f"  B4: {'bitwise identical' if bw else 'within tolerance'} to the uninterrupted run")

    print("== B3 thermomechanical pause and resume")
    call_ok(c, "boundary_apply", {"name": "clamp", "kind": "fixed", "selection": "hot_end", "source": "user"})
    TM = dict(RUN, analysis="thermomechanical", stress_free_temperature="20 degC", end_time="600 s", output_interval="150 s")
    v, _ = call_ok(c, "analysis_run", dict(TM, label="tm reference"), "thermomechanical reference")
    tref, trdir = v.get("job_id"), v.get("run_directory")
    tsum = wait_state(c, tref)
    check(tsum.get("state") == "succeeded", f"thermomechanical reference succeeded ({json.dumps(tsum.get('error'))[:200]})")
    v, _ = call_ok(c, "analysis_run", dict(TM, label="tm paused"), "thermomechanical run to pause")
    tp, tpdir = v.get("job_id"), v.get("run_directory")
    for _ in range(4000):
        st = poll(c, "job_status", {"job_id": tp})
        if st.get("progress", 0) >= 0.4 or st.get("state") not in ("running", "queued"):
            break
        time.sleep(0.02)
    call_ok(c, "job_pause", {"job_id": tp})
    st = wait_state(c, tp)
    check(st.get("state") == "paused", f"thermomechanical run paused ({st.get('state')})")
    call_ok(c, "job_resume", {"job_id": tp})
    st = wait_state(c, tp)
    check(st.get("state") == "succeeded", f"thermomechanical run resumed and completed ({st.get('state')})")
    bw = compare_runs("B3", trdir, tpdir, tsum.get("summary", {}), st.get("summary", {}), mech=True)
    print(f"  B3: temperature, displacement and von Mises frames {'bitwise identical' if bw else 'within tolerance'}")
    call_ok(c, "boundary_remove", {"name": "clamp"})
    call_ok(c, "project_save", {})

    print("== B6 changed specifications are refused")
    v, _ = call_ok(c, "analysis_run", dict(RUN, label="to change"), "run to pause before changes")
    mid, mdir = v.get("job_id"), v.get("run_directory")
    call_ok(c, "job_pause", {"job_id": mid})
    st = wait_state(c, mid)
    check(st.get("state") == "paused", f"paused before changing the project ({st.get('state')})")
    call_ok(c, "boundary_apply", {"name": "air", "kind": "convection", "selection": "cold_end", "source": "user", "replace": True,
                                  "convection": {"coefficient": 41, "ambient": "20 degC"}})
    e = call_err(c, "job_resume", {"job_id": mid})
    comps = [m.get("component") for m in e.get("details", {}).get("mismatches", [])]
    check(e.get("code") == "PRECONDITION_FAILED" and comps == ["conditions"], f"a changed convection coefficient is refused as 'conditions' ({comps})")
    call_ok(c, "boundary_apply", {"name": "air", "kind": "convection", "selection": "cold_end", "source": "user", "replace": True,
                                  "convection": {"coefficient": 40, "ambient": "20 degC"}})
    call_ok(c, "mesh_generate", {"element_size": "2.5 mm"})
    e = call_err(c, "job_resume", {"job_id": mid})
    comps = [m.get("component") for m in e.get("details", {}).get("mismatches", [])]
    check(e.get("code") == "PRECONDITION_FAILED" and "mesh" in comps, f"a changed mesh is refused and named ({comps})")
    call_ok(c, "mesh_generate", {"element_size": "1.25 mm"})
    call_ok(c, "material_define", {"material": {
        "id": "test_alloy", "name": "Verification alloy", "family": "metal", "status": "user_supplied",
        "provenance": "changed conductivity", "density_kg_m3": {"value": 8000.0}, "conductivity_w_per_mk": {"value": 21.0},
        "specific_heat_j_per_kgk": {"value": 500.0}, "youngs_modulus_pa": {"value": 200e9}, "poisson_ratio": {"value": 0.3},
        "expansion_1_per_k": {"value": 1.2e-5}}, "replace": True})
    e = call_err(c, "job_resume", {"job_id": mid})
    comps = [m.get("component") for m in e.get("details", {}).get("mismatches", [])]
    check(e.get("code") == "PRECONDITION_FAILED" and comps == ["materials"], f"a changed material is refused as 'materials' ({comps})")
    call_ok(c, "material_define", {"material": {
        "id": "test_alloy", "name": "Verification alloy", "family": "metal", "status": "user_supplied",
        "provenance": "constant properties for the restart test", "density_kg_m3": {"value": 8000.0}, "conductivity_w_per_mk": {"value": 20.0},
        "specific_heat_j_per_kgk": {"value": 500.0}, "youngs_modulus_pa": {"value": 200e9}, "poisson_ratio": {"value": 0.3},
        "expansion_1_per_k": {"value": 1.2e-5}}, "replace": True})
    spec_path = Path(mdir) / "spec.json"
    spec_text = spec_path.read_text()
    spec = json.loads(spec_text)
    spec["request"]["temporal_relative_tolerance"] = 2e-5
    spec_path.write_text(json.dumps(spec))
    e = call_err(c, "job_resume", {"job_id": mid})
    comps = [m.get("component") for m in e.get("details", {}).get("mismatches", [])]
    check(e.get("code") == "PRECONDITION_FAILED" and comps == ["settings"], f"a tampered physics setting is refused as 'settings' ({comps})")
    spec_path.write_text(spec_text)

    print("== B5 damaged checkpoints")
    # (a) the only checkpoint truncated: refused, naming the damage
    ckf, prevf = Path(mdir) / "checkpoint.nvc", Path(mdir) / "checkpoint.prev.nvc"
    check(ckf.exists() and not prevf.exists(), "the paused run has exactly one checkpoint")
    good = ckf.read_bytes()
    ckf.write_bytes(good[: len(good) - 13])
    e = call_err(c, "job_resume", {"job_id": mid})
    check(e.get("code") == "PRECONDITION_FAILED" and "truncated" in e.get("message", ""), f"a truncated only checkpoint is refused ({e.get('message')})")
    ckf.write_bytes(good)
    # (b) two checkpoints, the newest truncated: the previous one is used and the fallback reported
    v, _ = call_ok(c, "analysis_run", dict(RUN, label="two checkpoints", checkpoint_interval="200 s"), "run with periodic checkpoints")
    fid, fdir = v.get("job_id"), v.get("run_directory")
    fprev = Path(fdir) / "checkpoint.prev.nvc"
    for _ in range(40000):
        st = poll(c, "job_status", {"job_id": fid})
        if fprev.exists() or st.get("state") not in ("running", "queued"):
            break
        time.sleep(0.005)
    call_ok(c, "job_pause", {"job_id": fid})
    st = wait_state(c, fid)
    fck = Path(fdir) / "checkpoint.nvc"
    check(st.get("state") == "paused" and fck.exists() and fprev.exists(), f"paused with a newest and a previous checkpoint ({st.get('state')})")
    raw = fck.read_bytes()
    fck.write_bytes(raw[: len(raw) // 2])
    v, _ = call_ok(c, "job_resume", {"job_id": fid}, "resume from the previous checkpoint")
    rej = v.get("checkpoint", {}).get("newest_checkpoint_rejected", "")
    check("truncated" in rej, f"the truncated newest checkpoint is detected and the previous one used ({rej})")
    st = wait_state(c, fid)
    check(st.get("state") == "succeeded", f"the run completes from the previous checkpoint ({st.get('state')})")
    bw = compare_runs("B5 fallback", ref_dir, fdir, ref_sum, st.get("summary", {}))
    print(f"  fallback: resumed from t = {v.get('resumed_from_time_s')} s (previous checkpoint) after the newest was truncated; "
          f"{'bitwise identical' if bw else 'within tolerance'}")
    # a second paused run for the flipped-byte test
    v, _ = call_ok(c, "analysis_run", dict(RUN, label="to corrupt"), "run to corrupt")
    xid, xdir = v.get("job_id"), v.get("run_directory")
    for _ in range(4000):
        st = poll(c, "job_status", {"job_id": xid})
        if st.get("progress", 0) >= 0.2 or st.get("state") not in ("running", "queued"):
            break
        time.sleep(0.02)
    call_ok(c, "job_pause", {"job_id": xid})
    wait_state(c, xid)
    ck = Path(xdir) / "checkpoint.nvc"
    raw = bytearray(ck.read_bytes())
    raw[-100] ^= 0x01  # one bit in the payload
    ck.write_bytes(bytes(raw))
    for extra in ("checkpoint.prev.nvc",):
        if (Path(xdir) / extra).exists():
            (Path(xdir) / extra).unlink()
    e = call_err(c, "job_resume", {"job_id": xid})
    check(e.get("code") == "PRECONDITION_FAILED" and "SHA-256" in e.get("message", ""), f"a flipped payload bit is detected ({e.get('message')})")
    c.close()

    print("== B2 process killed after a checkpoint, resumed by a new server")
    sock = tmp / "run" / "ctl.sock"
    ready = tmp / "ready"

    def start_server():
        if ready.exists():
            ready.unlink()
        srv = subprocess.Popen([str(SERVER), "--socket", str(sock), "--ready-file", str(ready)] + common, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        for _ in range(200):
            if ready.exists():
                break
            time.sleep(0.05)
        return srv

    srv = start_server()
    check(ready.exists(), "navier-server ready")
    a = Client(["--connect", str(sock)])
    a.initialize()
    call_ok(a, "project_open", {"path": str(ws / "restart_bar")}, "project_open in the server")
    call_ok(a, "mesh_generate", {"element_size": "1.25 mm"})
    v, _ = call_ok(a, "analysis_run", dict(RUN, label="killed", checkpoint_interval="150 s"), "run to kill")
    kid2, kdir2 = v.get("job_id"), v.get("run_directory")
    ckpath = Path(kdir2) / "checkpoint.nvc"
    for _ in range(20000):
        if ckpath.exists():
            break
        time.sleep(0.005)
    killed_after = ckpath.exists()
    srv.send_signal(signal.SIGKILL)
    srv.wait(timeout=20)
    try:
        a.close()
    except Exception:
        pass
    check(killed_after, "the server was killed after the first checkpoint was published")
    check(not (Path(kdir2) / "results.nvt").exists(), "the killed run had not finished")
    srv = start_server()
    b = Client(["--connect", str(sock)])
    b.initialize()
    call_ok(b, "project_open", {"path": str(ws / "restart_bar")}, "project_open in the new server")
    st, _ = call_ok(b, "job_status", {"job_id": kid2}, "job_status of the killed run")
    check(st.get("state") == "interrupted" and st.get("checkpoint", {}).get("available") is True,
          f"the new process reports the run as interrupted with a checkpoint ({st.get('state')}, t = {st.get('checkpoint', {}).get('time_s')})")
    e = call_err(b, "job_resume", {"job_id": kid2})
    check(e.get("code") == "PRECONDITION_FAILED" and "mesh" in json.dumps(e.get("details", {})),
          "before the mesh is regenerated the resume is refused as a mesh change")
    call_ok(b, "mesh_generate", {"element_size": "1.25 mm"})
    v, _ = call_ok(b, "job_resume", {"job_id": kid2}, "job_resume in the new process")
    t_res = v.get("resumed_from_time_s")
    st = wait_state(b, kid2)
    check(st.get("state") == "succeeded", f"the killed run completes after resume ({st.get('state')}: {json.dumps(st.get('error'))[:300]})")
    bw2 = compare_runs("B2", ref_dir, kdir2, ref_sum, st.get("summary", {}))
    print(f"  killed after a checkpoint, resumed at t = {t_res} s in a new process: {'bitwise identical' if bw2 else 'within tolerance'}")
    b.close()
    srv.send_signal(signal.SIGTERM)
    try:
        srv.wait(timeout=20)
    except subprocess.TimeoutExpired:
        srv.kill()
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{'RESTART FLOW TESTS FAILED' if FAIL else 'ALL RESTART FLOW TESTS PASSED'}: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
