#!/usr/bin/env python3
"""make_examples.py - build the Thermal Sim demonstration studies and leave their artefacts on disk.

Each study is driven over MCP stdio exactly as an AI host would drive it, and each leaves behind:

  <study>/project.json      the saved project: geometry, material, mesh settings, conditions, assumptions
  <study>/runs/<job>/spec.json   the resolved, hashed run specification (reproducible without any AI)
  <study>/runs/<job>/summary.json  energy budget, enthalpy check, diagnostics
  <study>/export/*.vtu,*.pvd,*.csv  fields and time history
  <study>/README.md         what it shows, the analytical reference, and what it does not model

Run from the repository root after `make am`:
    python3 "Thermal Sim/examples/make_examples.py"
"""
import json
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402

OUT = Path(__file__).resolve().parent
FAIL = 0


def fail(msg):
    global FAIL
    FAIL += 1
    print(f"  FAIL: {msg}")


def call(c, name, args):
    r = c.call(name, args)
    sc = (r.get("result", {}) or {}).get("structuredContent") or {}
    if r.get("result", {}).get("isError"):
        fail(f"{name}: {json.dumps(sc.get('error'))[:300]}")
        return {}
    return sc.get("value", {})


def box_stl(path, lx, ly, lz, label=b"thermal sim example"):
    p = [(0, 0, 0), (lx, 0, 0), (lx, ly, 0), (0, ly, 0), (0, 0, lz), (lx, 0, lz), (lx, ly, lz), (0, ly, lz)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, cc, d in quads:
        tris += [(p[a], p[b], p[cc]), (p[a], p[cc], p[d])]
    with open(path, "wb") as f:
        f.write(label.ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


def wait(c, job_id):
    for _ in range(2400):
        st = call(c, "job_status", {"job_id": job_id})
        if st.get("state") in ("succeeded", "failed", "cancelled"):
            return st
        time.sleep(0.25)
    fail(f"job {job_id} did not finish")
    return {}


def study(name, title, build, readme):
    """build(c, ws) -> (job_id, notes); leaves the project and its exports under OUT/name"""
    print(f"== {title}")
    ws = OUT / name
    if ws.exists():
        import shutil
        shutil.rmtree(ws)
    ws.mkdir(parents=True)
    geo = ws / "geometry"
    geo.mkdir()
    c = Client(["--embedded", "--workspace", str(ws / "workspace"), "--allow-read", str(ws), "--allow-write", str(ws)])
    c.initialize()
    job_id, notes = build(c, ws, geo)
    if job_id:
        call(c, "project_save", {})
        exp = ws / "export"
        exp.mkdir(exist_ok=True)
        call(c, "results_export", {"job_id": job_id, "formats": ["vtu", "csv", "summary"], "directory": str(exp)})
    c.close()
    (ws / "README.md").write_text(readme(notes))
    files = sorted(p.relative_to(ws).as_posix() for p in ws.rglob("*") if p.is_file())
    print(f"  {len(files)} files under {ws.relative_to(ROOT)}")
    return notes


# ---------------------------------------------------------------- 1. heated bar cooled by convection
def build_bar(c, ws, geo):
    stl = geo / "bar.stl"
    box_stl(stl, 100.0, 10.0, 10.0, b"100x10x10 mm bar")
    call(c, "project_create", {"name": "heated_bar", "description": "bar held at 300 C at one end, air-cooled at the other"})
    call(c, "geometry_import", {"path": str(stl), "units": "mm", "name": "bar"})
    call(c, "material_define", {"material": {
        "id": "verification_alloy", "name": "Verification alloy", "family": "metal", "status": "user_supplied",
        "provenance": "constant properties chosen so that the steady state has a closed-form solution; not a real alloy",
        "density_kg_m3": {"value": 8000.0}, "conductivity_w_per_mk": {"value": 20.0},
        "specific_heat_j_per_kgk": {"value": 500.0}, "youngs_modulus_pa": {"value": 200e9},
        "poisson_ratio": {"value": 0.3}, "expansion_1_per_k": {"value": 1.2e-5}}})
    call(c, "material_assign", {"body": "bar", "material": "verification_alloy", "source": "user"})
    call(c, "mesh_generate", {"element_size": "2.5 mm"})
    call(c, "selection_create", {"name": "hot_end", "query": {"plane": {"axis": "x", "at": "min"}}, "body": "bar"})
    call(c, "selection_create", {"name": "cold_end", "query": {"plane": {"axis": "x", "at": "max"}}, "body": "bar"})
    call(c, "boundary_apply", {"name": "hot", "kind": "temperature", "selection": "hot_end", "temperature": "300 degC", "source": "user"})
    call(c, "boundary_apply", {"name": "cooled", "kind": "convection", "selection": "cold_end",
                               "convection": {"coefficient": 60.0, "ambient": "20 degC"}, "source": "user"})
    run = call(c, "analysis_run", {"analysis": "transient_thermal", "end_time": "20000 s", "time_step": "50 s",
                                   "initial_temperature": "20 degC", "output_every": 20})
    st = wait(c, run.get("job_id", ""))
    q = call(c, "results_query", {"job_id": run["job_id"], "quantity": "temperature"})
    L, A, k, h, Thot, Tamb = 0.1, 1e-4, 20.0, 60.0, 300.0, 20.0
    qw = (Thot - Tamb) / (L / (k * A) + 1 / (h * A))
    notes = {"cold_face_c": q.get("statistics", {}).get("min"), "cold_face_analytic_c": Tamb + qw / (h * A),
             "power_w": qw, "energy": st.get("summary", {}).get("energy", {}), "spec_hash": run.get("spec_hash"),
             "run_directory": run.get("run_directory")}
    return run.get("job_id"), notes


def readme_bar(n):
    e = n["energy"]
    return f"""# Study 1 — heated component cooled by a prescribed convection coefficient

A 100 x 10 x 10 mm bar held at 300 degC on one end face and cooled by `h = 60 W/(m^2 K)` to 20 degC air on the other,
run to 20 000 s (ten diffusion times `L^2 rho cp / k = 2000 s`), so the field is effectively steady.

## Reference

One-dimensional series resistance:

    q = (T_hot - T_amb) / (L/(k A) + 1/(h A)) = {n['power_w']:.4f} W
    T_cold_face = T_amb + q/(h A) = {n['cold_face_analytic_c']:.3f} degC

## Result

| quantity | value |
|---|---|
| cold-face temperature | **{n['cold_face_c']:.3f} degC** (analytic {n['cold_face_analytic_c']:.3f} degC) |
| energy closure error | {e.get('closure_error'):.2e} |
| worst per-step balance error | {e.get('worst_step_balance_error'):.2e} |
| stored vs integrated enthalpy | {e.get('enthalpy_mismatch'):.2e} |
| specification hash | `{n['spec_hash']}` |

The voxel mesh represents this box exactly (no staircase), so the agreement is a check of the discretisation and the
convection boundary term, not of the mesher.

## What this does not model

The cooling is a **prescribed heat-transfer coefficient**, not resolved airflow. No fluid is solved, so this is not
conjugate heat transfer and `h` carries all the uncertainty of whatever correlation it came from. The material is a
made-up constant-property alloy chosen for the closed-form reference; it is not a real material.

## Reproduce

```bash
make am
python3 "Thermal Sim/examples/make_examples.py"
```

The resolved setup is in `workspace/heated_bar/runs/<job>/spec.json` and can be replayed without any AI.
"""


# ---------------------------------------------------------------- 2. thermally loaded solid, deformation
def build_expansion(c, ws, geo):
    stl = geo / "bar.stl"
    box_stl(stl, 100.0, 10.0, 10.0, b"100x10x10 mm bar")
    call(c, "project_create", {"name": "thermal_expansion", "description": "anchored bar heated from one end; thermal deformation"})
    call(c, "geometry_import", {"path": str(stl), "units": "mm", "name": "bar"})
    call(c, "material_define", {"material": {
        "id": "verification_alloy", "name": "Verification alloy", "family": "metal", "status": "user_supplied",
        "provenance": "constant properties chosen so that the expansion has a closed-form estimate; not a real alloy",
        "density_kg_m3": {"value": 8000.0}, "conductivity_w_per_mk": {"value": 20.0},
        "specific_heat_j_per_kgk": {"value": 500.0}, "youngs_modulus_pa": {"value": 200e9},
        "poisson_ratio": {"value": 0.3}, "expansion_1_per_k": {"value": 1.2e-5}}})
    call(c, "material_assign", {"body": "bar", "material": "verification_alloy", "source": "user"})
    call(c, "mesh_generate", {"element_size": "2.5 mm"})
    call(c, "selection_create", {"name": "hot_end", "query": {"plane": {"axis": "x", "at": "min"}}, "body": "bar"})
    call(c, "selection_create", {"name": "cold_end", "query": {"plane": {"axis": "x", "at": "max"}}, "body": "bar"})
    call(c, "boundary_apply", {"name": "hot", "kind": "temperature", "selection": "hot_end", "temperature": "300 degC", "source": "user"})
    call(c, "boundary_apply", {"name": "cooled", "kind": "convection", "selection": "cold_end",
                               "convection": {"coefficient": 60.0, "ambient": "20 degC"}, "source": "user"})
    call(c, "boundary_apply", {"name": "anchor", "kind": "fixed", "selection": "hot_end", "source": "user"})
    run = call(c, "analysis_run", {"analysis": "thermomechanical", "end_time": "20000 s", "time_step": "50 s",
                                   "initial_temperature": "20 degC", "stress_free_temperature": "20 degC", "output_every": 20})
    st = wait(c, run.get("job_id", ""))
    u = call(c, "results_query", {"job_id": run["job_id"], "quantity": "displacement"})
    t = call(c, "results_query", {"job_id": run["job_id"], "quantity": "temperature"})
    notes = {"umax_mm": u.get("statistics", {}).get("max"), "tmin_c": t.get("statistics", {}).get("min"),
             "tmax_c": t.get("statistics", {}).get("max"), "spec_hash": run.get("spec_hash"),
             "energy": st.get("summary", {}).get("energy", {})}
    return run.get("job_id"), notes


def readme_expansion(n):
    tmean = 0.5 * (n["tmax_c"] + n["tmin_c"])
    est = 1.2e-5 * (tmean - 20.0) * 100.0
    return f"""# Study 2 — thermally loaded solid with deformation

The bar of study 1, anchored on the heated end face, solved as a **one-way thermomechanical** analysis: the
temperature history drives the thermal strain and the temperature-dependent stiffness, and the structure does not feed
back into the temperature field.

## Result

| quantity | value |
|---|---|
| temperature range | {n['tmin_c']:.2f} .. {n['tmax_c']:.2f} degC |
| largest displacement | **{n['umax_mm']:.4f} mm** |
| estimate from the mean temperature | `alpha * (T_mean - T_0) * L` = {est:.4f} mm |
| energy closure error | {n['energy'].get('closure_error'):.2e} |
| specification hash | `{n['spec_hash']}` |

The estimate uses the mean of the end temperatures; the real profile is not linear, so the two are close rather than
equal. It is a sanity bound, not a verification case — the verification of thermal strain is in `build/femtest`
(free and constrained expansion against closed-form values).

## What this does not model

**This is not a residual-stress prediction.** The solid is linear elastic: with no plasticity or creep the thermal
stresses vanish again when the part returns to a uniform temperature, so nothing is left behind. Residual stress needs
a history-dependent constitutive model, which is not implemented. There is also no two-way coupling: the deformation
does not move the thermal boundaries.
"""


# ---------------------------------------------------------------- 3. melting / solidification
def build_melt(c, ws, geo):
    stl = geo / "block.stl"
    box_stl(stl, 10.0, 10.0, 10.0, b"10 mm block")
    call(c, "project_create", {"name": "melting_block", "description": "316L block driven through its solidus by volumetric heating"})
    call(c, "geometry_import", {"path": str(stl), "units": "mm", "name": "block"})
    call(c, "material_assign", {"body": "block", "material": "ss316l_lpbf_demo", "source": "default"})
    call(c, "mesh_generate", {"element_size": "2.5 mm"})
    call(c, "boundary_apply", {"name": "heating", "kind": "heat_source", "body": "block",
                               "power_density": "4e8 W/m^3", "source": "user"})
    args = {"analysis": "transient_thermal", "end_time": "20 s", "time_step": "0.25 s", "initial_temperature": "20 degC",
            "max_iterations": 1500, "temperature_tolerance": 1e-9, "output_every": 8}
    run = call(c, "analysis_run", dict(args))
    st = wait(c, run.get("job_id", ""))
    t = call(c, "results_query", {"job_id": run["job_id"], "quantity": "temperature"})
    off = dict(args)
    off["phase_change"] = False
    off.pop("temperature_tolerance")
    off.pop("max_iterations")
    run2 = call(c, "analysis_run", off)
    wait(c, run2.get("job_id", ""))
    t2 = call(c, "results_query", {"job_id": run2["job_id"], "quantity": "temperature"})
    sm = st.get("summary", {})
    notes = {"molten_mm3": sm.get("phase_change", {}).get("largest_molten_volume_mm3"),
             "peak_c": t.get("statistics", {}).get("max"), "peak_no_latent_c": t2.get("statistics", {}).get("max"),
             "energy": sm.get("energy", {}), "spec_hash": run.get("spec_hash"),
             "warnings": [w.get("code") for w in sm.get("setup_warnings", []) if isinstance(w, dict)]}
    return run.get("job_id"), notes


def readme_melt(n):
    e = n["energy"]
    return f"""# Study 3 — melting and solidification

A 10 mm cube of the `ss316l_lpbf_demo` material (solidus 1375 degC, liquidus 1400 degC, latent heat 270 kJ/kg) heated
uniformly at 4e8 W/m^3 for 20 s, which carries it through the solidus and part way through the mushy zone.

## Result

| quantity | value |
|---|---|
| largest molten volume | **{n['molten_mm3']:.3f} mm^3** of 1000 mm^3 |
| peak temperature, latent heat applied | **{n['peak_c']:.1f} degC** |
| peak temperature, `phase_change: false` | {n['peak_no_latent_c']:.1f} degC |
| held back by the latent heat | {n['peak_no_latent_c'] - n['peak_c']:.1f} K |
| energy closure error | {e.get('closure_error'):.2e} |
| stored vs integrated enthalpy | {e.get('enthalpy_mismatch'):.2e} |
| setup warnings | {', '.join(n['warnings']) or '(none)'} |
| specification hash | `{n['spec_hash']}` |

The second run is the same study with the phase change switched off; the difference is the latent heat doing its job.
Both runs report which choice was made — the module never applies or drops latent heat silently.

The sharp verification of this model is in `build/thermtest`: a melt/re-freeze cycle returns to its starting
temperature with an enthalpy mismatch of 8.7e-14, and a one-phase Stefan problem matches the similarity solution
`s(t) = 2 lambda sqrt(alpha t)` to **-0.12 %**.

## What this does not model

The liquid fraction is **linear between solidus and liquidus and assumed to be at local equilibrium**. There is no
undercooling, no nucleation, no hysteresis (freezing retraces the melting curve) and no solute redistribution. The
mushy interval is a model parameter that changes the answer, so a study that depends on it should be repeated with a
different interval. Molten material conducts like a solid: there is no melt-pool convection. The material values are
demonstration data, not calibrated 316L.

## Solver note

Latent heat makes the step stiff: inside the mushy zone `dH/dT` is about 8.6e7 J/(m^3 K), so a temperature tolerance
is a much looser energy tolerance. This study uses `temperature_tolerance: 1e-9` and `max_iterations: 1500`. Without
Aitken relaxation the Picard iteration for this case **diverges** — the fixed-point map has derivative `-B/Q` with
`B` the latent heat still to be absorbed and `Q` the heat delivered in the step.
"""


def main():
    n1 = study("01_heated_bar_convection", "Study 1: heated bar cooled by convection", build_bar, readme_bar)
    n2 = study("02_thermal_expansion", "Study 2: thermally loaded solid with deformation", build_expansion, readme_expansion)
    n3 = study("03_melting_block", "Study 3: melting and solidification", build_melt, readme_melt)
    print()
    print(f"  bar:       cold face {n1['cold_face_c']:.3f} degC (analytic {n1['cold_face_analytic_c']:.3f}), "
          f"closure {n1['energy'].get('closure_error'):.1e}")
    print(f"  expansion: {n2['umax_mm']:.4f} mm over {n2['tmin_c']:.1f}..{n2['tmax_c']:.1f} degC")
    print(f"  melting:   {n3['molten_mm3']:.1f} mm^3 molten, peak {n3['peak_c']:.1f} degC "
          f"vs {n3['peak_no_latent_c']:.1f} degC without latent heat")
    print(f"\n{'EXAMPLES FAILED' if FAIL else 'ALL EXAMPLE STUDIES BUILT'}: {FAIL} failures")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
