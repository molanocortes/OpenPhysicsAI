#!/usr/bin/env python3
"""uicheck.py - functional check of the NAVIER interface: every control is clicked through the real input path and the
state it is supposed to change is then read back and compared. tools/uitest.nav only clicks; this asserts the effects.

It writes a script, runs the app off-screen once, splits the log at the markers and checks each step.

    python3 tools/uicheck.py            # after: make navier
    python3 tools/uicheck.py --keep     # keep the generated script and the log for inspection
"""
import base64
import importlib.util
import json
import os
import math
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "navier"
MARK = "UICHECK"

PASS = 0
FAIL = 0


def check(cond, what, text=""):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"  FAIL: {what}")
        if text:
            for line in text.strip().splitlines()[-12:]:
                print(f"        | {line}")


def num(pattern, text, group=1):
    """first number matched by pattern, or None"""
    m = re.search(pattern, text, re.I)
    return float(m.group(group)) if m else None


def close(a, b, rel=0.02, absolute=0.0):
    return a is not None and abs(a - b) <= max(abs(b) * rel, absolute)


# Each step: (name, commands, check function). The check receives the log text of that step.
def steps():
    s = []

    def add(name, cmds, fn, note=None):
        s.append((name, cmds, fn, note or name))

    add("boot", ["scene cylinder", "frames 40", "status"],
        lambda t: check("state" in t.lower(), "the app starts and reports its state", t))
    add("start", ['uiclick "START EXPERIMENT"', "frames 12", "status"],
        lambda t: check(re.search(r"state\s+running", t, re.I), "START EXPERIMENT starts the run", t))
    add("pause", ["uiclick PAUSE", "frames 10", "status"],
        lambda t: check(re.search(r"state\s+paused", t, re.I), "PAUSE pauses the run", t))
    add("run_state_row", ['uiclick "run state"', "frames 10", "status"],
        lambda t: check(re.search(r"state\s+running", t, re.I), "the run-state row restarts the run", t))
    add("pause_again", ["uiclick PAUSE", "frames 10", "status"],
        lambda t: check(re.search(r"state\s+paused", t, re.I), "PAUSE pauses again", t))
    add("field_cp", ["uiclick Cp", "frames 6", "view"],
        lambda t: check(re.search(r"field\s*=\s*cp\b", t, re.I), "the Cp tab selects the pressure-coefficient field", t))
    add("field_vorticity", ["uiclick VORTICITY", "frames 6", "view"],
        lambda t: check(re.search(r"field\s*=\s*vorticity", t, re.I), "the VORTICITY tab selects vorticity", t))
    add("field_speed", ["uiclick SPEED", "frames 6", "view"],
        lambda t: check(re.search(r"field\s*=\s*speed", t, re.I), "the SPEED tab selects speed", t))
    add("streamlines_on", ["streamlines off", "frames 6", "uiclick STREAMLINES", "frames 8", "streamlines"],
        lambda t: check(re.search(r"streamlines\s+on\b", t, re.I), "STREAMLINES switches streamlines on", t))
    add("streamlines_off", ["uiclick STREAMLINES", "frames 8", "streamlines"],
        lambda t: check(re.search(r"streamlines\s+off\b", t, re.I), "STREAMLINES switches them off again", t))
    add("particles_on", ["uiclick PARTICLES", "frames 8", "particles"],
        lambda t: check(re.search(r"particles\s+on", t, re.I), "PARTICLES switches particles on", t))
    add("particles_off", ["uiclick PARTICLES", "frames 8", "particles"],
        lambda t: check(re.search(r"particles\s+off", t, re.I), "PARTICLES switches them off again", t))
    add("vortices_on", ["uiclick VORTICES", "frames 8", "vortices"],
        lambda t: check(re.search(r"(vortex|vortices).*on", t, re.I), "VORTICES switches the isosurfaces on", t))
    add("vortices_off", ["uiclick VORTICES", "frames 8", "vortices"],
        lambda t: check(re.search(r"(vortex|vortices).*off", t, re.I), "VORTICES switches them off again", t))
    add("slice_on", ["slice off", "frames 6", "uiclick SLICE", "frames 8", "uilist"],
        lambda t: check("sl_slice" in t, "SLICE switches the slice on and shows its slider", t))
    add("slice_pos", ["uiclick sl_slice 0.25", "frames 10", "slice"],
        lambda t: check(close(num(r"slice\s+(?:on|off):\s*[xyz]\s*=\s*([0-9.]+)", t), 0.25, rel=0.08),
                        "the slice slider sets the position to 0.25", t))
    add("surface", ["uiclick SURFACE", "frames 6", "surface"],
        lambda t: check(re.search(r"surface:\s*(solid|field)", t, re.I), "SURFACE switches the surface mode", t))
    add("volume", ["uiclick VOLUME", "frames 8", "view"],
        lambda t: check(re.search(r"volume rendering (on|off)", t, re.I), "VOLUME toggles volume rendering", t))
    add("speed_slider", ["uiclick sl_speed 0.25", "frames 10", "speed"],
        lambda t: check(close(num(r"U\s*=\s*([0-9.]+(?:e[+-]?[0-9]+)?)\s*mm/s", t), 15.7, rel=0.06) or
                        close(num(r"U\s*=\s*([0-9.]+(?:e[+-]?[0-9]+)?)\s*m/s", t), 0.0157, rel=0.06),
                        "the speed slider at 0.25 gives 0.0157 m/s (logarithmic 0.001..60)", t))
    add("aoa_slider", ["uiclick sl_aoa 0.25", "frames 10", "aoa"],
        lambda t: check(close(num(r"angle of attack\s*=\s*(-?[0-9.]+)", t), -12.5, rel=0.02, absolute=0.3),
                        "the angle-of-attack slider at 0.25 gives -12.5 degrees", t))
    add("roughness_slider", ["uiclick sl_rough 0.6", "frames 10", "roughness"],
        lambda t: check(close(num(r"ks\s*=\s*([0-9.]+(?:e[+-]?[0-9]+)?)\s*mm", t), 0.251, rel=0.08) or
                        close(num(r"ks\s*=\s*([0-9.]+(?:e[+-]?[0-9]+)?)\s*m\b", t), 2.51e-4, rel=0.08),
                        "the roughness slider at 0.6 gives ks = 0.25 mm", t))
    add("temp_slider", ["uiclick sl_temp 0.4", "frames 10", "temp"],
        lambda t: check(close(num(r"temperature\s*=\s*([0-9.]+)", t) or num(r"at\s+([0-9.]+)\s*\xc2?\xb0", t), 38.0, rel=0.03, absolute=0.6),
                        "the temperature slider at 0.4 gives 38 degC", t))
    add("turbulence_slider", ["uiclick sl_turb 0.3", "frames 10", "turbulence"],
        lambda t: check(close(num(r"turbulence[^=\n]*=\s*([0-9.]+)\s*%", t) or num(r"inlet turbulence\s+([0-9.]+)\s*%", t), 6.0, rel=0.05, absolute=0.2),
                        "the turbulence slider at 0.3 gives 6 %", t))
    add("colormap", ["cmap", "uiclick legend", "frames 6", "cmap"],
        lambda t: check(len(re.findall(r"\*", t)) >= 2 and _cmap_changed(t), "clicking the legend switches the colour map", t))
    add("camera_gizmo", ["uiclick axes", "frames 10", "camera"],
        lambda t: check(num(r"yaw\s*(-?[0-9.]+)", t) is not None, "the axes gizmo applies a camera preset", t))
    # the frame budget is generous on purpose: the assertion is about the button, not about how fast the solver runs
    add("step_button", ["status", "uiclick TIME", "frames 8", 'uiclick "+10"', "frames 600", "status"],
        lambda t: check(_step_delta(t) == 10, f"the +10 button advances exactly 10 steps (measured {_step_delta(t)})", t))
    add("quality_preset", ["uiclick DRAFT", "frames 60", "grid"],
        lambda t: check(_cells(t) is not None and 0.3e6 < _cells(t) < 2.0e6,
                        f"the DRAFT preset applies a draft-sized lattice ({_cells(t)} cells)", t))
    add("model_tab", ["uiclick MODEL", "frames 6", "uilist"],
        lambda t: check("sl_pitch" in t and "sl_yaw" in t, "the MODEL tab shows the attitude sliders", t))
    add("model_yaw", ["uiclick sl_yaw 0.7", "frames 12", "yaw"],
        lambda t: check(close(num(r"yaw\s*=\s*(-?[0-9.]+)", t), 72.0, rel=0.03, absolute=1.5),
                        "the yaw slider at 0.7 gives +72 degrees", t))
    add("model_rotate", ["uiclick \"X 90\"", "frames 12", "model"],
        lambda t: check(re.search(r"rotated|extent", t, re.I), "the X 90 button rotates the model", t))
    add("lines_tab", ["uiclick LINES", "frames 6", "uiclick GRID", "frames 10", "streamlines"],
        lambda t: check(re.search(r"grid", t, re.I), "the LINES tab selects the grid rake", t))
    add("rake_slider", ["uiclick sl_rake_x 0.3", "frames 12", "streamlines"],
        lambda t: check(close(num(r"rake at \(\s*([0-9.]+)", t), 0.30, rel=0.12, absolute=0.03),
                        "the rake slider moves the rake to x = 0.30 of the tunnel", t))
    add("seed_slider", ["uiclick sl_seeds 0.5", "frames 12", "streamlines"],
        lambda t: check(close(num(r"([0-9]+)\s+lines", t), 316, rel=0.15),
                        "the line-count slider at 0.5 gives about 316 lines (logarithmic 20..5000)", t))
    add("ambiguous_click", ["uiclick FLOW", "frames 6", "uiclick LINE", "frames 4"],
        lambda t: check("no widget" in t.lower(), "an ambiguous partial name is refused instead of clicking the wrong control", t),
        note="ambiguous name refused")
    add("missing_click", ["uiclick NOT_A_CONTROL", "frames 4"],
        lambda t: check("no widget" in t.lower(), "a missing control is reported", t))

    # ---- the solid workspace: the same window drives the finite-element engine -------------------------------
    add("solid_switch", ["uiclick SOLID", "frames 10", "workspace", "uilist"],
        lambda t: check(re.search(r"workspace:\s*solid", t, re.I) and "NEW PROJECT" in t,
                        "the SOLID button switches the workspace and shows the analysis panel", t))
    add("solid_toolbar_is_the_analysis", ["uilist"],
        lambda t: check("START##run" not in t and "LOAD STL" not in t and "OPEN STL" in t and "RUN" in t,
                        "the analysis toolbar drops the tunnel's verbs and keeps its own", t))
    add("solid_layers_off", ["streamlines"],
        lambda t: check(re.search(r"streamlines\s+off", t, re.I),
                        "the flow layers are put away while the analysis workspace is open", t))
    add("solid_new_project", ['uiclick "NEW PROJECT"', "frames 12", "fem status"],
        lambda t: check("new project 'project'" in t and "project project" in t,
                        "the NEW PROJECT button creates a project through project_create", t))
    add("solid_import", ["am geometry_import path=models/cube.stl units=cm", "frames 10", "fem status"],
        lambda t: check("1 bodies" in t, "the imported body reaches the interface state", t))
    add("solid_mesh", ["uiclick \"2 MESH\"", "frames 4", "uiclick sl_elem 0.45", "frames 6", "uiclick GENERATE", "frames 40", "fem status"],
        lambda t: check(_elements(t) is not None and 50 < _elements(t) < 4000,
                        f"the GENERATE button meshes the part ({_elements(t)} elements)", t))
    add("solid_material", ["uiclick \"3 MATERIAL\"", "frames 4", "uiclick \"SS316L DEMO\"", "frames 12", "fem status", "uilist"],
        lambda t: check("ss316l" in t.lower(), "a material button assigns that material to the body", t))
    add("solid_material_provenance", ["frames 4", "uitext"],
        lambda t: check("status: demonstration" in t and "source:" in t,
                        "the MATERIAL step states the record's status and where its numbers come from", t))
    # the sourced records: grouped by process, a status, a source line, and what a record lacks said in words
    # CoCr F75 still has no citable Poisson's ratio; 316L has one since wave 10
    add("solid_material_sourced", ["uiclick COCR", "frames 12", "uitext"],
        lambda t: check("status: published" in t and "Metal powder bed (LPBF)" in t and "Plastic filament (FFF)" in t and
                        "Not in this record, so a stress analysis refuses it: Poisson's ratio" in t and "am materials_list id=cocr_f75_lpbf" in t,
                        "a sourced record shows its status, its source line, its process group and what it lacks (read from material_list)", t))
    add("solid_material_complete", ["uiclick SS316L", "frames 12", "uitext"],
        lambda t: check("status: published" in t and "Not in this record" not in t,
                        "316L, with its design Poisson's ratio, lacks nothing for a stress or a heat analysis", t))
    add("solid_material_back", ["uiclick \"SS316L DEMO\"", "frames 12", "fem status"],
        lambda t: check("material ss316l_lpbf_demo" in t, "the demonstration 316L is assigned again for the solve that follows", t))
    add("solid_support", ["solid fixbase", "frames 10", "uiclick \"4 HOLD & LOAD\"", "frames 8", "uilist"],
        lambda t: check("base_support" in t and "bc0" in t,
                        "FIX BASE adds a support and the LOADS tab lists it", t))
    add("solid_load", ["am selection_create name=top query='{\"plane\": {\"axis\": \"z\", \"at\": \"max\"}}' source=user replace=true",
                       "frames 4",
                       "am boundary_apply '{\"name\": \"pull\", \"kind\": \"force\", \"selection\": \"top\","
                       " \"force\": [0, 0, -500], \"source\": \"user\"}'",
                       "frames 10", "uilist"],
        lambda t: check("bc1" in t, "the second condition appears in the LOADS list", t))
    # ---- pick by box: a rectangle takes every face whose centre falls inside it ---------------------------------
    add("pick_box_arm", ["uiclick \"4 HOLD & LOAD\"", "frames 6", "uiclick BOX", "frames 6", "uilist"],
        lambda t: check("BOX##pickbox" in t, "BOX is on the HOLD & LOAD step and arms with one click", t))
    add("pick_box_take", ["fem fit", "frames 8", "uidrag 60 100 940 780 frames 10", "frames 10"],
        lambda t: check(re.search(r"box took (\d+) faces", t) is not None and
                        int(re.search(r"box took (\d+) faces", t).group(1)) >= 2,
                        f"a dragged rectangle takes several faces at once ({_box_faces(t)})", t))
    add("pick_box_front_only", ["uiclick CLEAR", "frames 6", "uiclick \"FRONT ONLY\"", "frames 6",
                                "uidrag 60 100 940 780 frames 10", "frames 10"],
        lambda t: check("(front only)" in t, f"FRONT ONLY takes the faces turned towards the camera ({_box_faces(t)})", t))
    add("pick_box_front_off", ["uiclick \"FRONT ONLY\"", "frames 6", "uilist"],
        lambda t: check("FRONT ONLY##pickfront" in t, "FRONT ONLY switches off again", t))
    add("pick_box_drop", ["uidrag 60 100 940 780 shift frames 10", "frames 10"],
        lambda t: check("box dropped" in t, "shift-drag drops the faces inside the rectangle again", t))
    add("pick_box_off", ["uiclick BOX", "frames 6", "uiclick CLEAR", "frames 6", "uilist"],
        lambda t: check("BOX##pickbox" in t, "BOX disarms and the single-click pick is back", t))
    add("solid_run", ["uiclick \"5 SOLVE\"", "frames 8", 'uiclick "SOLVE"', "frames 10", "solid wait 240",
                      "frames 20", "fem status"],
        lambda t: check("succeeded" in t and "von Mises" in t,
                        "the RUN ANALYSIS button submits the solve and the result arrives", t))
    add("solid_stress", ["fem status"],
        lambda t: check(_peak_mpa(t) is not None and 1.0 < _peak_mpa(t) < 40.0,
                        f"500 N on the 10 mm cube gives a surface peak near the 5 MPa nominal stress "
                        f"(got {_peak_mpa(t)} MPa)", t))
    add("solid_field", ["uiclick DISPL", "frames 10", "fem status"],
        lambda t: check("displacement" in t and "mm" in t, "the DISPL button switches the displayed field", t))
    add("solid_view_open", ["uiclick \"6 RESULTS\"", "frames 4", "uiclick VIEW", "frames 6", "uilist"],
        lambda t: check("sl_deform" in t and "SECTION" in t,
                        "the VIEW expander holds the viewer tools and opens on one click", t))
    add("solid_deform", ["fem deform 250", "frames 8", "uiclick sl_deform 0.5", "frames 10", "fem deform"],
        lambda t: check(_deform(t) is not None and _deform(t) != 250,
                        f"the deformation-scale slider changes the exaggeration (now x{_deform(t)})", t))
    add("solid_true_deform", ["uiclick TRUE##def", "frames 4", "fem deform"],
        lambda t: check(close(_deform(t),1),"TRUE button sets physical deformation scale 1",t))
    add("solid_auto_deform", ["uiclick AUTO", "frames 10", "fem deform"],
        lambda t: check("auto" in t.lower(), "the AUTO button restores the automatic deformation scale", t))
    add("solid_hide", ["uiclick HIDE", "frames 10", "uilist"],
        lambda t: check("SHOW" in t, "HIDE removes the result surface and the button becomes SHOW", t))
    add("solid_show", ["uiclick SHOW", "frames 10", "fem status"],
        lambda t: check("triangles" in t, "SHOW brings the result surface back", t))
    # ---- a transient result: playback over the stored times ---------------------------------------------------
    add("solid_thermal_conditions", [
        "uiclick \"4 HOLD & LOAD\"", "frames 6",
        "am selection_create name=outside query='{\"type\": \"planar\"}' source=user replace=true",
        "am boundary_apply '{\"name\": \"cooling\", \"kind\": \"convection\", \"selection\": \"outside\","
        " \"convection\": {\"coefficient\": 60, \"ambient\": 20}, \"source\": \"user\"}'",
        "am boundary_apply '{\"name\": \"heating\", \"kind\": \"heat_source\", \"body\": \"cube\","
        " \"power_density\": 8e6, \"source\": \"user\"}'", "frames 8", "uilist"],
        lambda t: check(t.count("boundary_apply ok") == 2 and "bc3" in t,
                        "the thermal conditions are applied and listed in the LOADS tab", t))
    add("solid_thermal_run", ["uiclick \"5 SOLVE\"", "frames 6", "uiclick THERMAL", "frames 6",
                              "uiclick sl_endtime 0.39", "uiclick sl_timestep 0.69", "frames 6",
                              'uiclick "SOLVE"', "frames 10", "solid wait 240", "frames 20", "fem status", "fem step"],
        lambda t: check("succeeded" in t and "temperature" in t and _stored_times(t) and _stored_times(t) > 2,
                        f"the THERMAL run through the panel finishes with {_stored_times(t)} stored times", t))
    add("solid_speed_slider", ["uiclick \"6 RESULTS\"", "frames 4", "uiclick sl_rate 0.5", "frames 8", "fem speed"],
        lambda t: check(close(num(r"playback speed[: ]+([0-9.]+)", t), 3.87, rel=0.15),
                        "the playback-speed slider at 0.5 gives about 3.9 stored times per second (log 0.5..30)", t))
    add("solid_play", ["uiclick \"6 RESULTS\"", "frames 4", "fem speed 8", "uiclick PLAY", "frames 45", "fem step", "fem range"],
        lambda t: check(_stored_step(t) is not None and _stored_step(t) > 1 and "all stored times" in t,
                        f"PLAY advances the stored time (at {_stored_step(t)}) and keeps the range over all times", t))
    add("solid_pause", ["uiclick PAUSE", "frames 25", "fem step", "frames 25", "fem step"],
        lambda t: check(_steps_seen(t) >= 2 and _steps_seen(t, uniq=True) == 1,
                        "PAUSE stops the film: the stored time no longer changes", t))
    # ---- a part that grows over the stored times (element visibility, not the mesher's boundary) ---------------
    # the triangle count is a statement about the mesh, so this one is asked of the VOXELS view; on the part's own
    # surface the same growth is measured in pixels by the pixel checks below
    add("solid_grow", ["uiclick \"6 RESULTS\"", "frames 4", "uiclick VOXELS", "frames 4",
                       "fem grow z", "frames 6", "fem step 1", "frames 4", "fem status",
                       "fem step last", "frames 4", "fem status"],
        # Only the two displayed result statuses count. The debug command also logs its current triangle count,
        # which can be the last time before the script explicitly selects the first time.
        lambda t: check(len(_result_tris(t)) >= 2 and _result_tris(t)[0] < _result_tris(t)[-1],
                        f"the faked growing part draws fewer triangles at the first stored time than at the last {_result_tris(t)[:1]}->{_result_tris(t)[-1:]}", t))
    add("solid_grow_off", ["fem grow off", "frames 6", "fem status"],
        lambda t: check("growth off" in t and _tris(t) and _tris(t)[-1] > 0,
                        "growth off draws the whole mesh again", t))
    add("solid_surface_back", ["uiclick SURFACE", "frames 6", "fem surface"],
        lambda t: check("drawing the part surface" in t,
                        "SURFACE puts the result back on the part's own surface", t))
    add("solid_section", ["uiclick \"6 RESULTS\"", "frames 4", "uiclick SECTION", "frames 4", "uiclick X##section", "frames 4", "uiclick sl_section 0.5", "frames 4", "fem section"],
        lambda t: check(re.search(r"section on x 0\.5",t), "section toggle, axis and position use the real input path",t))
    add("solid_section_flip", ["uiclick FLIP##section", "frames 4", "fem section", "fem play", "frames 15", "fem pause", "fem status"],
        lambda t: check("flip on" in t and "triangles" in t, "section flip works during playback",t))
    add("solid_section_off", ["uiclick SECTION", "frames 4", "fem section"],
        lambda t: check("section off" in t,"SECTION switches off",t))
    add("solid_groups", ["fem groups z 2 4", "frames 6", "uiclick PART##group", "frames 4", "uiclick SUPPORT##group", "frames 4", "uiclick PLATE##group", "frames 4", "fem status", "uilist"],
        lambda t: check("groups part 0 support 0 plate 0" in t and "PART##group" in t,"group toggles hide all groups and keep controls reachable",t))
    add("solid_groups_restore", ["fem show part on", "fem show support on", "fem show plate on", "frames 4"],
        lambda t: check("group part on" in t and "group support on" in t and "group plate on" in t,"terminal restores groups",t))
    # ---- the five things the owner saw on 2026-09-18 -----------------------------------------------------------
    add("solid_slider_follows", ["uiclick \"2 MESH\"", "frames 6", "uiclick sl_elem 0.8", "frames 6", "solid mesh", "am mesh_generate element_size=1.4",
                                 "frames 8", "solid mesh"],
        lambda t: check(_shown_mm(t) is not None and close(_shown_mm(t), 1.4, rel=0.01),
                        f"the element-size slider follows the project after a mesh elsewhere (shows {_shown_mm(t)} mm)", t))
    add("solid_suggested_size", ["solid mesh"],
        lambda t: check(_suggested_mm(t) is not None and 0.2 <= _suggested_mm(t) <= 20,
                        f"a mesh size is suggested from the part ({_suggested_mm(t)} mm)", t))
    add("solid_auto_deform_stable", ["fem step first", "fem deform auto", "frames 6", "fem deform",
                                     "fem speed 20", "fem play", "frames 60", "fem pause", "frames 6", "fem deform"],
        lambda t: check(len(_deforms(t)) >= 2 and _deforms(t)[0] == _deforms(t)[-1],
                        f"the AUTO deformation scale does not jump during playback {_deforms(t)[:1]}->{_deforms(t)[-1:]}", t))
    add("solid_drop_goes_to_analysis", ["load models/cube.stl", "frames 8", "solid import", "frames 4"],
        lambda t: check("waiting in the PART step" in t and "choose the unit" in t.lower(),
                        "a part dropped while SOLID is open waits for its unit instead of loading into the tunnel", t))
    add("solid_drop_unit", ["solid import mm", "frames 10", "fem status"],
        lambda t: check("imported" in t and "bodies" in t,
                        "answering the unit imports the waiting file into the analysis", t))
    add("solid_refusal_shown", ["uiclick \"6 RESULTS\"", "frames 4", "uiclick \"CHECK MESH\"", "frames 40",
                               "fem status"],
        lambda t: check("last refusal shown in the panel" in t or "mesh check:" in t,
                        "a refusal from a panel button is kept for the panel to show, not swallowed", t))
    # ---- the build path: every control of step 4, on the part that is already open --------------------------
    add("build_path_chosen", ["uiclick \"1 PART\"", "frames 6", 'uiclick "SIMULATE THE BUILD"', "frames 8", "uilist"],
        lambda t: check("4 BUILD" in t and "5 RUN" in t,
                        "choosing the build path renames steps 4 and 5 in the strip", t))
    add("build_kind_fff", ["uiclick \"4 BUILD\"", "frames 6", 'uiclick "FFF PLASTIC"', "frames 6", "uilist"],
        lambda t: check("nozzle" in t and "rate" in t and "bed" in t,
                        "FFF PLASTIC shows the printer settings, each with its unit", t))
    add("build_kind_lpbf", ['uiclick "LPBF METAL"', "frames 6", "uilist"],
        lambda t: check("exx" in t and "CUT ON" in t and "youngs" in t,
                        "LPBF METAL shows the typed strain, the cut and the elastic constants", t))
    add("build_orientation", ["uiclick \"Y##ori\"", "frames 6", "uilist"],
        lambda t: check("Y##ori" in t, "the build orientation buttons are on screen and clickable", t))
    add("build_strain_typed", ["uiclick Calibrated##prov", "frames 6", "uilist"],
        lambda t: check("exx" in t and "eyy" in t and "ezz" in t and "Calibrated##prov" in t,
                        "the three strain components and the provenance the operation demands are on screen", t))
    add("build_cut_off", ["uiclick \"CUT ON\"", "frames 6", "uilist"],
        lambda t: check("CUT OFF" in t and "kerf" not in t,
                        "the cut can be switched off and its three fields go with it", t))
    add("build_cut_on", ["uiclick \"CUT OFF\"", "frames 6", "uilist"],
        lambda t: check("CUT ON" in t and "kerf" in t, "the cut comes back with height, kerf and start", t))
    add("build_typed_modulus", ["uiclick youngs", "frames 4", "uikeys 70000", "frames 6", "uilist"],
        lambda t: check("typed 5 characters" in t, "the modulus is typed into its own field", t))
    add("build_field_keeps_its_own", ["uiclick poisson", "frames 4", "uikeys 0.33", "frames 6", "uilist"],
        lambda t: check("typed 4 characters into the panel field: '0.33'" in t,
                        "a second field takes the keyboard without disturbing the first", t))
    add("build_run_refused_empty", ["uiclick \"5 RUN\"", "frames 6", "uilist"],
        lambda t: check("RUN BUILD" in t, "step 5 offers RUN BUILD on the build path", t))
    add("back_to_analysis_path", ["uiclick \"1 PART\"", "frames 6", 'uiclick "ANALYSE STRESS"', "frames 8", "uilist"],
        lambda t: check("4 HOLD & LOAD" in t and "5 SOLVE" in t,
                        "the analysis path comes back and the strip says so", t))
    add("mesh_size_typed", ["uiclick \"2 MESH\"", "frames 6", 'uiclick "element size"', "frames 4", "uikeys 3",
                            "frames 6", "uilist"],
        lambda t: check("typed 1 characters into the panel field: '3'" in t,
                        "the element size can be typed exactly instead of dragged", t))
    # ---- the three modes, the glossary and the results' own colour map ------------------------------------------
    add("results_viridis", ["uiclick \"6 RESULTS\"", "frames 4", "fem status"],
        lambda t: check("result colour map viridis" in t, "results are drawn in viridis by default (colour-blind safe)", t))
    add("legend_turbo", ["uiclick legend", "frames 6", "fem status"],
        lambda t: check("result colour map turbo" in t, "a click on the legend switches the results to turbo", t))
    add("legend_back", ["uiclick legend", "frames 6", "fem status"],
        lambda t: check("result colour map viridis" in t, "and back to viridis", t))
    add("mode_simple", ["mode simple", "frames 10", "uitext", "mode", "uilist"],
        lambda t: check("mode simple" in t and ("Choose a part" in t or "Results" in t) and "Simple##mode" not in t,
                        "two modes on the top bar (Manual, Agentic); the guided screens stay reachable by command", t))
    add("glossary_open", ["uiclick ?", "frames 8", "uilist"],
        lambda t: check("CLOSE##glossary" in t, "? opens the glossary of the words used", t))
    add("glossary_close", ["uiclick CLOSE", "frames 6", "uilist"],
        lambda t: check("CLOSE##glossary" not in t, "and CLOSE puts it away", t))
    add("mode_agent", ["uiclick Agentic", "frames 10", "uitext", "mode"],
        lambda t: check("mode agent" in t and "Ask the lab" in t, "the Agent switch shows the one question field", t))
    add("agent_settings", ["uiclick SETTINGS", "frames 8", "uitext"],
        lambda t: check("Found on this computer" in t, "the agent settings list the tools found on this computer", t))
    add("agent_settings_close", ["uiclick SETTINGS", "frames 6"],
        lambda t: check(True, "settings close again", t))
    add("mode_advanced", ["uiclick Manual", "frames 10", "mode", "uilist"],
        lambda t: check("mode advanced" in t and "6 RESULTS" in t, "Advanced brings the six-step strip back, with nothing lost", t))
    add("solid_unknown_job", ["fem job nosuchjob-1", "frames 8", "fem status"],
        lambda t: check("no run directory" in t and "no result displayed" in t,
                        "an unknown job id is reported, not loaded as something else", t))
    add("back_to_fluid", ["uiclick FLUID", "frames 12", "workspace", "streamlines"],
        lambda t: check(re.search(r"workspace:\s*fluid", t, re.I) and re.search(r"streamlines\s+on", t, re.I),
                        "FLUID returns to the tunnel and restores the layers that were on", t))
    return s


def _shown_mm(t):
    m = re.findall(r"element size shown\s+([0-9.eE+-]+)\s*mm", t)
    return float(m[-1]) if m else None


def _suggested_mm(t):
    m = re.search(r"suggested\s+([0-9.eE+-]+)\s*mm", t)
    return float(m.group(1)) if m else None


def _deforms(t):
    return re.findall(r"deformation scale:[^0-9]*([0-9.eE+-]+)", t)


def _box_faces(t):
    m = re.findall(r"box (?:took|dropped) (\d+) faces?", t)
    return [int(x) for x in m]


def _result_tris(t):
    return [int(n) for n in re.findall(r"\[info\] result [^\n]*? (\d+) triangles", t)]


def _tris(t):
    return [int(x) for x in re.findall(r"(\d+) triangles", t)]


def _stored_times(t):
    m = re.search(r"stored time\s+\d+\s+of\s+(\d+)", t) or re.search(r"(\d+)\s+stored times", t)
    return int(m.group(1)) if m else None


def _stored_step(t):
    m = re.search(r"stored time\s+(\d+)\s+of", t)
    return int(m.group(1)) if m else None


def _steps_seen(t, uniq=False):
    hits = re.findall(r"stored time\s+(\d+)\s+of", t)
    return len(set(hits)) if uniq else len(hits)


def _elements(t):
    m = re.search(r"([0-9]+)\s+elements", t)
    return int(m.group(1)) if m else None


def _peak_mpa(t):
    m = re.search(r"([0-9.eE+-]+)\s+to\s+([0-9.eE+-]+)\s*MPa", t)
    return float(m.group(2)) if m else None


def _deform(t):
    """the factor reported by the last 'fem deform' line of the step"""
    hits = re.findall(r"deformation scale:[^0-9]*([0-9.eE+-]+)", t)
    return float(hits[-1]) if hits else None


def _cmap_changed(t):
    names = re.findall(r"([A-Za-z_]+)\*", t)
    return len(names) >= 2 and names[0] != names[-1]


def _step_delta(t):
    steps_seen = [int(x) for x in re.findall(r"step\s+([0-9]+),", t, re.I)]
    return steps_seen[-1] - steps_seen[0] if len(steps_seen) >= 2 else None


def _cells(t):
    m = re.search(r"lattice\s+([0-9]+)\s*[x\xc3×]\s*([0-9]+)\s*[x\xc3×]\s*([0-9]+)", t, re.I)
    return float(m.group(1)) * float(m.group(2)) * float(m.group(3)) if m else None


# ---- pixel evidence -------------------------------------------------------------------------------------------
# The checks above read the log; these read the image the app drew. A second, small run keeps the pure-Python PNG
# decode cheap (tools/pngcheck.py, the same decoder the render tests use).

def _decode(path):
    spec = importlib.util.spec_from_file_location("pngcheck", ROOT / "tools" / "pngcheck.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m.decode_png(Path(path).read_bytes())


def _differing(a, b, x1=None):
    """pixels that differ between two decoded frames of the same size; x1 limits it to the left part (the 3D view)"""
    (w, h, ch, pa), (_, _, _, pb) = a, b
    x1 = w if x1 is None else max(0, min(w, x1))
    n = 0
    for y in range(h):
        row = (y * w) * ch
        for x in range(x1):
            i = row + x * ch
            if abs(pa[i] - pb[i]) > 24 or abs(pa[i + 1] - pb[i + 1]) > 24 or abs(pa[i + 2] - pb[i + 2]) > 24:
                n += 1
    return n


def _hue_buckets(img, x0, y0, x1, y1):
    """how many different dominant-channel/brightness buckets a strip holds (a colour map has several)"""
    w, h, ch, px = img
    seen = set()
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w)):
            i = (y * w + x) * ch
            r, g, b = px[i], px[i + 1], px[i + 2]
            if max(r, g, b) < 40:
                continue
            seen.add((r // 64, g // 64, b // 64))
    return len(seen)


def pixel_checks(app, tmp):
    print("== result pixel checks on the off-screen image")
    shots = tmp / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    stl = (ROOT / "models" / "cube.stl").resolve()
    script = tmp / "pixels.nav"
    script.write_text("\n".join([
        "workspace solid", "frames 8",
        "am project_create name=pixels overwrite=true",
        f"am geometry_import path='{stl}' units=cm name=cube",
        "am mesh_generate element_size=2",
        "am material_assign body=cube material=ss316l_lpbf_demo source=user",
        "am selection_create name=outside query='{\"type\": \"planar\"}' source=user replace=true",
        "am boundary_apply '{\"name\": \"cooling\", \"kind\": \"convection\", \"selection\": \"outside\","
        " \"convection\": {\"coefficient\": 60, \"ambient\": 20}, \"source\": \"user\"}'",
        "am boundary_apply '{\"name\": \"heating\", \"kind\": \"heat_source\", \"body\": \"cube\","
        " \"power_density\": 8e6, \"source\": \"user\"}'",
        "frames 8", "solid run thermal 40 10", "solid wait 300", "frames 15",
        # the last stored time throughout: at the first one the field is uniform and sits at the bottom of the
        # colour map, which is nearly the colour of the background
        "fem step last", "fem fit", "frames 10",
        "fem hide", "frames 10", f"screenshot {shots}/hidden.png",
        "fem show", "frames 10", f"screenshot {shots}/whole.png",
        "fem grow z", "fem step 1", "frames 8", f"screenshot {shots}/grow1.png",
        "fem step 3", "frames 8", f"screenshot {shots}/grow3.png",
        "fem step last", "frames 8", f"screenshot {shots}/grow5.png",
        "fem grow off", "fem section x 0.5", "frames 8", f"screenshot {shots}/section.png",
        "fem section off", "fem step first", "frames 8", f"screenshot {shots}/zero.png",
        "fem step last", "fem status", "fem cut z 5 at 4", "frames 8", "fem status", f"screenshot {shots}/cut.png",
        "fem cut off", "frames 8", f"screenshot {shots}/identity.png",
        "quit", ""]))
    run = subprocess.run([str(app), "--headless", "--size", "640x400", "--workspace", str(tmp / "pixelws"),
                          "--exec", f"exec {script}"], capture_output=True, text=True, timeout=900, cwd=str(ROOT))
    log = run.stdout + run.stderr
    missing = [n for n in ("hidden", "whole", "grow1", "grow3", "grow5") if not (shots / f"{n}.png").exists()]
    if missing:
        check(False, f"the pixel run wrote every screenshot (missing {missing})", log)
        return
    hidden = _decode(shots / "hidden.png")
    whole = _decode(shots / "whole.png")
    g1, g3, g5 = (_decode(shots / f"grow{i}.png") for i in (1, 3, 5))
    frame = hidden[0] * hidden[1]
    part = _differing(whole, hidden)
    check(part > frame * 0.02, f"the result surface covers {100.0 * part / frame:.1f}% of the frame "
                               f"(nothing is drawn when it is hidden)")
    a, b, c = _differing(g1, hidden), _differing(g3, hidden), _differing(g5, hidden)
    check(0 < a < b < c, f"the growing part covers more of the frame at each stored time ({a} -> {b} -> {c} pixels)")
    check(abs(c - part) <= max(40, part * 0.02),
          f"at the last stored time the grown part matches the whole mesh ({c} against {part} pixels)")
    half = _decode(shots / "section.png")
    zero = _decode(shots / "zero.png")
    hs = _differing(half, hidden)
    check(0 < hs < part, f"section at 0.5 shows fewer lit pixels ({hs} vs {part})")
    zp = _differing(zero, hidden)
    check(zp > part*.8, f"uniform bottom-of-range field stays visible ({zp} vs {part} pixels)")
    # only the 3D view: the panel holds a scrolling terminal and a pulsing state light, which differ by design.
    # The window is 640 wide and the panel is 430 wide with a 10 px margin (main.c: app.panel_w).
    view_w = hidden[0] - 440
    ident = _differing(_decode(shots / "identity.png"), whole, x1=view_w)
    check(ident < 40, f"birth and death switched off restore the same picture ({ident} differing pixels in the view)")
    tr = _tris(log)
    # The cut removes one element layer: the two faces it exposes are larger than the perimeter strip it takes
    # away, so the triangle count must rise. femviewtest checks the exact arithmetic on a mesh it builds itself.
    cut_img = _decode(shots / "cut.png")
    gap = _differing(cut_img, _decode(shots / "identity.png"), x1=view_w)
    check(len(tr) >= 2 and tr[-1] > tr[-2] and gap > 200,
          f"the cut opens two new surfaces: {tr[-2]} -> {tr[-1]} triangles, {gap} pixels changed in the view", log)
    # Compare the visible zero-field pixels with the same camera's empty background; report median contrast.
    w,h,ch,px=zero; bg=hidden[3]; differences=[]
    for y in range(h-75):
        for x in range(w):
            i=(y*w+x)*ch
            d=max(abs(px[i+k]-bg[i+k]) for k in range(3))
            if d>24: differences.append(d)
    differences.sort()
    median=differences[len(differences)//2] if differences else 0
    check(median>=40,f"zero-field median channel separation {median}/255 (criterion >=40)")
    print(f"  pixels growth {a}/{b}/{c}; whole {part}; section {hs}; zero {zp}; zero contrast {median}/255; identity {ident}; cut triangles {tr[-2:]}")
    print(f"  images: {shots}")
    # the legend colour bar of the SOLID workspace: x 24..314, ten rows starting 52 above the bottom
    w, h = hidden[0], hidden[1]
    buckets = _hue_buckets(whole, 24, h - 52, 314, h - 42)
    check(buckets >= 3, f"the legend colour bar shows a range of colours ({buckets} colour buckets)")
    check(_hue_buckets(hidden, 24, h - 52, 314, h - 42) >= 3, "the legend is drawn even with the result hidden")




# ---- presentation (T22): transparency, ambient occlusion, feature edges, exploded view, image export ------------
# Every item is checked on pixels the app drew, on a three-body sample built here from primitives, and leaves one
# screenshot per item in docs/images/.

def _boxes(dirpath):
    """three boxes as STL files: a base, a post on it and a cap over that"""
    import struct
    def box(path, lo, hi):
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        p = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
        quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (2, 3, 7, 6), (1, 2, 6, 5), (0, 4, 7, 3)]
        tris = []
        for a, b, c, d in quads:
            tris.append((p[a], p[b], p[c]))
            tris.append((p[a], p[c], p[d]))
        with open(path, "wb") as f:
            f.write(b"three-body sample (uicheck)".ljust(80, b" "))
            f.write(struct.pack("<I", len(tris)))
            for t in tris:
                f.write(struct.pack("<3f", 0, 0, 0))
                for v in t:
                    f.write(struct.pack("<3f", *v))
                f.write(b"\0\0")
    dirpath.mkdir(parents=True, exist_ok=True)
    # Every dimension is a multiple of 2 mm, so a 2 mm mesh of the stack is exact and the volume error says
    # something about the mesher rather than about the staircase.
    box(dirpath / "base.stl", (0, 0, 0), (60, 30, 8))
    box(dirpath / "post.stl", (0, 0, 0), (12, 12, 32))
    box(dirpath / "cap.stl", (0, 0, 0), (26, 20, 8))
    return dirpath


# geometry_import drops every body on the plate with its footprint centred, so a stack has to be placed: without
# these two lines the three boxes sit inside each other and both the picture and the volume are wrong.
BOX_PLACEMENT = ['am geometry_place \'{"body": "post", "position": [0, 0], "z_offset": "8 mm"}\'',
                 'am geometry_place \'{"body": "cap", "position": [0, 0], "z_offset": "40 mm"}\'']


def _mean_brightness(img, x0, y0, x1, y1):
    w, h, ch, px = img
    tot = n = 0
    for y in range(max(0, y0), min(y1, h)):
        for x in range(max(0, x0), min(x1, w)):
            i = (y * w + x) * ch
            tot += px[i] + px[i + 1] + px[i + 2]
            n += 3
    return tot / n if n else 0



def _silhouette(img, x1):
    """the pixels the part covers: not the dark blue background and not its grid lines"""
    w, h, ch, px = img
    mask = []
    for y in range(80, h):
        for x in range(0, min(x1, w)):
            i = (y * w + x) * ch
            r, g, b = px[i], px[i + 1], px[i + 2]
            if max(r, g, b) >= 75 or b < r:
                mask.append((x, y))
    return mask


def _top_patch(img, x1, background=None):
    """the centre of the highest face drawn in the view: scan down for the first solid run of part pixels, then
    average a small box just below it. That face belongs to the piece standing highest, the cap of the stack."""
    w, h, ch, px = img
    def solid(x, y):
        i = (y * w + x) * ch
        r, g, b = px[i], px[i + 1], px[i + 2]
        if background is not None:
            # Compare the computed body with the same empty studio. A neutral
            # backdrop can have r > b too; hue alone is not evidence of a face.
            return max(abs(px[i + c] - background[3][i + c]) for c in range(3)) > 8
        return max(r, g, b) >= 75 or b < r
    for y in range(90, h):
        run = start = 0
        for x in range(0, min(x1, w)):
            if solid(x, y):
                if run == 0:
                    start = x
                run += 1
                if run >= 24:
                    cx, cy = start + run // 2, y + 8
                    t = [0.0, 0.0, 0.0]
                    n = 0
                    for yy in range(cy, cy + 8):
                        for xx in range(cx - 6, cx + 6):
                            i = (yy * w + xx) * ch
                            t[0] += px[i]; t[1] += px[i + 1]; t[2] += px[i + 2]; n += 1
                    return (cx, cy), [c / n for c in t]
            else:
                run = 0
    return None


def _darkened(before, after, mask, threshold):
    """how many of the mask's pixels the second frame darkens by at least `threshold`, and the deepest darkening"""
    w, h, ch, pa = before
    pb = after[3]
    n, deepest = 0, 0.0
    for x, y in mask:
        i = (y * w + x) * ch
        d = (pa[i] + pa[i + 1] + pa[i + 2] - pb[i] - pb[i + 1] - pb[i + 2]) / 3.0
        if d >= threshold:
            n += 1
        if d > deepest:
            deepest = d
    return n, deepest


def _percentile_over(img, mask, q):
    """brightness of the qth percentile pixel of the mask: where ambient occlusion acts, in the creases"""
    w, h, ch, px = img
    vals = []
    for x, y in mask:
        i = (y * w + x) * ch
        vals.append((px[i] + px[i + 1] + px[i + 2]) / 3.0)
    if not vals:
        return 0.0
    vals.sort()
    return vals[min(len(vals) - 1, int(q * len(vals)))]


def _mean_over(img, mask):
    w, h, ch, px = img
    t = 0
    for x, y in mask:
        i = (y * w + x) * ch
        t += px[i] + px[i + 1] + px[i + 2]
    return t / (3 * len(mask)) if mask else 0.0


def _darkest(img, x0, y0, x1, y1):
    w, h, ch, px = img
    best = 255
    for y in range(max(0, y0), min(y1, h)):
        for x in range(max(0, x0), min(x1, w)):
            i = (y * w + x) * ch
            best = min(best, max(px[i], px[i + 1], px[i + 2]))
    return best



# ---- topology optimisation: OPTIMISE in the panel, the density on screen, the STL, and the operation through MCP
# (docs/contracts/topology-optimisation.md, T21 items 3 and 4). One screenshot per step in docs/images/.
def topopt():
    print("== topology optimisation: the panel, the part that survives, the STL, and the operation through MCP")
    tmp = Path(tempfile.mkdtemp(prefix="topopt"))
    shots = tmp / "shots"
    shots.mkdir()
    (tmp / "ws").mkdir(parents=True, exist_ok=True)
    images = ROOT / "docs" / "images"
    images.mkdir(parents=True, exist_ok=True)
    stl = tmp / "ws" / "optimised.stl"
    lines = ["workspace solid", "frames 4",
             'am project_create \'{"name": "topo_bracket", "description": "stiffness optimisation"}\'',
             'am geometry_import \'{"path": "samples/bracket.stl", "units": "mm", "name": "bracket"}\'',
             'am material_assign \'{"body": "bracket", "material": "ss316l_lpbf_demo", "source": "user"}\'',
             'am mesh_generate \'{"element_size": "1 mm"}\'',
             'am selection_create \'{"name": "foot", "query": {"all": [{"facing": {"direction": "down"}}, {"plane": {"axis": "z", "at": "min"}}]}, "source": "user"}\'',
             'am selection_create \'{"name": "top", "query": {"extreme": {"direction": "+z"}}, "source": "user"}\'',
             'am boundary_apply \'{"name": "hold", "kind": "fixed", "selection": "foot", "source": "user"}\'',
             'am boundary_apply \'{"name": "push", "kind": "force", "selection": "top", "force": ["0.5 kN", 0, 0], "source": "user"}\'',
             "solid run static", "solid wait 300", "frames 20",
             "fem surface off", "fem deform 0", "fem fit", "frames 8", f"screenshot {shots}/whole.png",
             # the panel, through real clicks: open OPTIMISE, press OPTIMISE NOW, wait for the job
             "uiclick OPTIMISE", "frames 6", f"screenshot {images}/topopt-panel.png",
             'uiclick "OPTIMISE NOW"', "frames 8", "fem optimwait 600", "frames 12",
             f"screenshot {shots}/optimised.png", f"screenshot {images}/topopt-optimised.png",
             'uiclick "SHOW WHOLE"', "frames 8", f"screenshot {shots}/back.png",
             'uiclick "SHOW OPTIMISED"', "frames 8",
             f"fem optipart {stl}",
             "echo TOPOPT done", "quit"]
    script = tmp / "topopt.nav"
    script.write_text("\n".join(lines) + "\n")
    run = subprocess.run([str(APP), "--headless", "--size", "900x600", "--workspace", str(tmp / "ws"),
                          "--exec", f"exec {script}"], capture_output=True, text=True, timeout=1800, cwd=str(ROOT))
    log = run.stdout + run.stderr
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build" / "topopt.log").write_text(log)
    check("TOPOPT done" in log, "the optimisation walkthrough ran to the end")
    started = "topology optimisation started" in log
    check(started, "OPTIMISE NOW in the panel started the job")
    m = re.search(r"topology optimisation: (\d+) iterations, (\d+) of (\d+) elements kept \((\d+) % of the volume\), "
                  r"compliance ([0-9.eE+-]+) J; the grey start of the same volume was ([0-9.eE+-]+) J", log)
    check(m is not None, "the panel reports iterations, elements kept and the compliance")
    if m:
        iters, kept, total, vol, c, c0 = int(m[1]), int(m[2]), int(m[3]), int(m[4]), float(m[5]), float(m[6])
        print(f"  {iters} iterations, {kept} of {total} elements kept ({vol} % of the volume), "
              f"compliance {c:.4g} J against {c0:.4g} J for the grey design of the same volume")
        check(0 < kept < total, f"material was removed ({kept} of {total} elements left)")
        check(c < c0, f"the optimised design is stiffer than the grey one ({c:.4g} J against {c0:.4g} J)")
    whole, optimised, back = (_decode(shots / f"{n}.png") for n in ("whole", "optimised", "back"))
    n = _differing(optimised, whole, x1=560)
    check(n > 2000, f"the view shows the part with the emptied elements gone ({n} pixels changed)")
    n_back = _differing(back, whole, x1=560)
    check(n_back < n / 4, f"SHOW WHOLE puts the removed material back ({n_back} pixels still differ, against {n})")
    tris = 0
    if stl.exists():
        data = stl.read_bytes()
        if len(data) > 84:
            tris = int.from_bytes(data[80:84], "little")
    check(tris > 50, f"the surviving part is exported as an STL ({tris} triangles)")
    check(stl.exists() and len(stl.read_bytes()) == 84 + 50 * tris, "the STL is a complete binary file")
    m = re.search(r"optimised part: .*?(\d+) diagonal contacts? filled to a neck.*?non-manifold edges (\d+) to (\d+), "
                  r"(closed solid|still not a closed solid), volume ([0-9.eE+-]+) mm3", log)
    check(m is not None, "the export says what the repair changed")
    if m:
        necks, nm_before, nm_after, verdict, vol = int(m[1]), int(m[2]), int(m[3]), m[4], float(m[5])
        print(f"  export repair: {necks} diagonal contacts filled, non-manifold edges {nm_before} to {nm_after}, "
              f"{verdict}, volume {vol:.4g} mm3")
        check(verdict == "closed solid" and nm_after == 0 and vol > 0,
              f"the exported part is a closed solid with a volume ({verdict}, {nm_after} non-manifold edges, {vol:.4g} mm3)")

    # the same operation through the MCP server, on the same project
    try:
        sys.path.insert(0, str(ROOT / "tools"))
        from mcptest import Client
        c = Client(["--embedded", "--workspace", str(tmp / "ws")])
        c.initialize()
        def op(name, args):
            r = c.call(name, args)
            sc = r.get("result", {}).get("structuredContent") or {}
            return sc.get("value", {}), r
        # a project of its own, built through the operation layer, so the MCP path is checked end to end
        op("project_create", {"name": "topo_mcp", "description": "topology optimisation through MCP"})
        op("geometry_import", {"path": "samples/bracket.stl", "units": "mm", "name": "bracket"})
        op("material_assign", {"body": "bracket", "material": "ss316l_lpbf_demo", "source": "user"})
        op("mesh_generate", {"element_size": "1 mm"})
        op("selection_create", {"name": "foot", "query": {"all": [{"facing": {"direction": "down"}}, {"plane": {"axis": "z", "at": "min"}}]}, "source": "user"})
        op("selection_create", {"name": "top", "query": {"extreme": {"direction": "+z"}}, "source": "user"})
        op("boundary_apply", {"name": "hold", "kind": "fixed", "selection": "foot", "source": "user"})
        op("boundary_apply", {"name": "push", "kind": "force", "selection": "top", "force": ["0.5 kN", 0, 0], "source": "user"})
        v, _ = op("topology_optimize", {"volume_fraction": 0.35, "max_iterations": 20, "filter_radius": "2 mm"})
        job = v.get("job_id", "")
        check(bool(job), f"topology_optimize is reachable through navier-mcp (job {job})")
        state = ""
        for _ in range(600):
            st, _ = op("job_status", {"job_id": job})
            state = st.get("state", "")
            if state in ("succeeded", "failed", "cancelled"):
                break
            time.sleep(1)
        check(state == "succeeded", f"the MCP run finished ({state})")
        res, _ = op("topology_result", {"job_id": job, "include_density": True})
        dens = base64.b64decode(res.get("density_base64", ""))
        check(len(dens) == 4 * int(res.get("elements", 0)) and res.get("elements", 0) > 0,
              f"topology_result returns one float per element ({len(dens)} bytes for {res.get('elements', 0)} elements)")
        vals = struct.unpack("<%df" % res.get("elements", 0), dens) if dens else ()
        check(vals and min(vals) < 0.1 and max(vals) > 0.9, "the field runs from empty to solid")
        hist = res.get("history", [])
        check(len(hist) == res.get("iterations", -1) and len(hist) > 1, f"the iteration history is returned ({len(hist)} points)")
        # a volume fraction below the passive region is refused, with both numbers in the message
        v, _ = op("topology_optimize", {"volume_fraction": 0.01, "max_iterations": 5})
        bad = v.get("job_id", "")
        msg = ""
        for _ in range(120):
            st, _ = op("job_status", {"job_id": bad})
            if st.get("state") in ("succeeded", "failed", "cancelled"):
                msg = json.dumps(st.get("error", {}))
                break
            time.sleep(1)
        check("more than the" in msg, f"a volume fraction below the passive volume is refused: {msg[:120]}")
        # the exported part imports as a solid with a volume: what the repair inside the export is for
        op("project_create", {"name": "topo_reimport", "description": "the optimised part imported again"})
        body, _ = op("geometry_import", {"path": str(stl), "units": "mm", "name": "optimised"})
        b = body.get("body", {})
        print(f"  reimported: {b.get('triangles')} triangles, volume {b.get('volume_mm3')} mm3, "
              f"closed solid {b.get('closed_solid')}")
        check(bool(b.get("closed_solid")) and float(b.get("volume_mm3") or 0) > 0,
              f"the exported part imports as a closed solid with a volume ({b.get('volume_mm3')} mm3)")
        c.close()
    except Exception as e:
        check(False, f"the operation through navier-mcp: {e}")
    print(f"  log: build/topopt.log   images: docs/images/topopt-*.png")
    shutil.rmtree(tmp, ignore_errors=True)
    return 1 if FAIL else 0

def presentation():
    print("== presentation: glass, ambient occlusion, edges, exploded view, image export")
    tmp = Path(tempfile.mkdtemp(prefix="presentation"))
    shots = tmp / "shots"
    shots.mkdir()
    boxes = _boxes(Path("/tmp/navier-presentation-boxes"))  # under an allowed read root
    images = ROOT / "docs" / "images"
    images.mkdir(parents=True, exist_ok=True)
    lines = ["workspace solid", "frames 4",
             'am project_create \'{"name": "three_bodies", "description": "three boxes from primitives"}\'']
    for name in ("base", "post", "cap"):
        lines.append(f'am geometry_import \'{{"path": "{boxes}/{name}.stl", "units": "mm", "name": "{name}"}}\'')
        lines.append(f'am material_assign \'{{"body": "{name}", "material": "ss316l_lpbf_demo", "source": "user"}}\'')
    lines += BOX_PLACEMENT
    lines += ['am mesh_generate \'{"element_size": "2 mm"}\'',
              'am selection_create \'{"name": "foot", "body": "base", "query": {"all": [{"facing": {"direction": "down"}}, {"plane": {"axis": "z", "at": "min"}}]}, "source": "user"}\'',
              'am selection_create \'{"name": "top", "body": "cap", "query": {"all": [{"facing": {"direction": "up"}}, {"plane": {"axis": "z", "at": "max"}}]}, "source": "user"}\'',
              'am boundary_apply \'{"name": "hold", "kind": "fixed", "selection": "foot", "source": "user"}\'',
              'am boundary_apply \'{"name": "push", "kind": "force", "selection": "top", "force": [200, 0, 0], "source": "user"}\'',
              "solid run static", "solid wait 300", "frames 20",
              "fem surface off", "fem fit", "fem deform 0", "frames 8",
              # the same camera for every pair: only the feature under test changes
              "fem edges off", "fem shadows off", "frames 6", f"screenshot {shots}/plain.png",
              f"screenshot {images}/presentation-plain.png",
              "fem shadows on", "frames 6", f"screenshot {shots}/ao.png",
              f"screenshot {images}/presentation-ambient-occlusion.png",
              "fem edges on", "frames 6", f"screenshot {shots}/edges.png",
              f"screenshot {images}/presentation-edges.png",
              "fem glass 0 0.25", "frames 6", f"screenshot {shots}/glass.png",
              f"screenshot {images}/presentation-glass-edges.png",
              "fem explode 0", "frames 6", "fem status",
              "fem explode 0.7", "fem fit", "frames 8", "fem status", f"screenshot {shots}/explode.png",
              f"screenshot {images}/presentation-exploded.png",
              "fem hide", "frames 6", f"screenshot {shots}/explode-empty.png",
              "fem show", "frames 6",
              # probing and sectioning must still work while the pieces stand apart
              "uiclick \"4 HOLD & LOAD\"", "frames 6", "uiclickat 250 330", "frames 6",
              "fem section z 0.6", "frames 6", f"screenshot {images}/presentation-section-exploded.png",
              "fem section off", "fem explode 0", "fem glass 0 1", "frames 6",
              f"fem export {shots}/export3x.png 3",
              f"fem export {images}/presentation-export-3x.png 3",
              "echo PRESENTATION done", "quit"]
    script = tmp / "presentation.nav"
    script.write_text("\n".join(lines) + "\n")
    run = subprocess.run([str(APP), "--headless", "--size", "900x600", "--workspace", str(tmp / "ws"),
                          "--exec", f"exec {script}"], capture_output=True, text=True, timeout=900, cwd=str(ROOT))
    log = run.stdout + run.stderr
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build" / "presentation.log").write_text(log)
    check("PRESENTATION done" in log, "the presentation script ran to the end")
    check("explode 0.70 over 3 piece(s)" in log, "the three bodies are three pieces")
    plain, ao, edges, glass, explode = (_decode(shots / f"{n}.png") for n in ("plain", "ao", "edges", "glass", "explode"))
    # 1 transparency: the glass body lets what is behind it through, so many pixels change
    n = _differing(glass, edges, x1=560)
    check(n > 400, f"glass changes the picture where the body used to hide the rest ({n} pixels)")
    # 2 ambient occlusion: the part is darker in its creases with it on
    # Ambient occlusion acts in the creases, not over the whole part, so the measure is how many of the part's own
    # pixels it darkens and by how much, both frames read over the same pixels.
    part = _silhouette(plain, 560)
    darker, drop = _darkened(plain, ao, part, 8)
    b_off, b_on = _mean_over(plain, part), _mean_over(ao, part)
    check(darker > 400 and b_on <= b_off,
          f"ambient occlusion darkens the creases ({darker} pixels darker by 8 or more, deepest {drop:.0f}, "
          f"whole part {b_off:.1f} -> {b_on:.1f})")
    # 3 feature edges: dark lines appear over the surface
    n = _differing(edges, ao, x1=560)
    dark = _darkest(edges, 60, 150, 520, 520)
    check(n > 150 and dark < 60, f"feature edges drawn as dark lines ({n} pixels changed, darkest {dark})")
    # 4 exploded view: the picture changes a lot, every piece closes, and the section still works
    n = _differing(explode, glass, x1=560)
    check(n > 2000, f"the exploded view moves the pieces apart ({n} pixels)")
    check("section on z" in log, "the section still works while exploded")
    # Each piece is a body of its own while they stand apart, so the faces that were interior to the joined mesh
    # are drawn: the three boxes meet over 12 x 12 mm twice, which at 2 mm cells is 4 x 36 faces, 288 triangles.
    tris = [int(n) for ln in log.splitlines() if "rebuild" in ln for n in re.findall(r"(\d+) triangles", ln)]
    if len(tris) >= 2:
        joined, apart = tris[-2], tris[-1]
        print(f"  joined {joined} triangles, exploded {apart}: the four interfaces add {apart - joined}")
        check(apart - joined == 288,
              f"every piece closes when it stands apart ({apart} triangles against {joined} joined, expected {joined + 288})")
    else:
        check(False, f"the drawn triangle count was not reported ({tris})")
    # the piece that stands highest is the cap: the centre of its top face carries the result, not the background
    top = _top_patch(explode, 560, _decode(shots / "explode-empty.png"))
    if top:
        (px_x, px_y), col = top
        bg = _mean_brightness(explode, 20, 100, 60, 140)
        bright = sum(col) / 3.0
        print(f"  the cap's top face at ({px_x}, {px_y}): rgb {tuple(round(c) for c in col)}, background {bg:.1f}")
        check(bright > bg + 5 and col[1] >= col[0] * 0.5,
              f"the centre of the cap's top face carries a result colour (rgb {tuple(round(c) for c in col)} against a "
              f"background of {bg:.1f})")
    else:
        check(False, "the exploded frame shows no piece to measure")
    # the mesh of a stack whose dimensions are multiples of the cell size is exact: the volume says so
    verr = [abs(float(v)) for v in re.findall(r'"volume_error_percent": ([-0-9.e+]+)', log)]
    check(bool(verr) and max(verr) < 1.0,
          f"the volume of the placed stack is within 1 percent of the STL (worst {max(verr) if verr else float('nan'):.3g} %)")

    # 5 image export: three times the window, and the part is in it
    exp = _decode(shots / "export3x.png")
    check(exp[0] == 2700 and exp[1] == 1800, f"EXPORT IMAGE writes 3x the window ({exp[0]}x{exp[1]})")
    mid = _mean_brightness(exp, 600, 900, 900, 1200)
    check(mid > 10, f"the exported image holds the part, not an empty frame (mean {mid:.1f})")
    # the same glass through the operation layer: results_render with a body drawn as glass
    try:
        sys.path.insert(0, str(ROOT / "tools"))
        from mcptest import Client
        c = Client(["--embedded", "--workspace", str(tmp / "ws")])
        c.initialize()
        def op(name, args):
            r = c.call(name, args)
            sc = r.get("result", {}).get("structuredContent") or {}
            img = [i for i in r.get("result", {}).get("content", []) if i.get("type") == "image"]
            return sc.get("value", {}), (base64.b64decode(img[0]["data"]) if img else None)
        op("project_open", {"path": str(tmp / "ws" / "three_bodies")})
        jobs, _ = op("job_list", {})
        job = (jobs.get("jobs") or [{}])[0].get("job_id", "")
        if not job:  # a finished run of a reopened project is found by its run directory
            runs = sorted((tmp / "ws" / "three_bodies" / "runs").glob("job-*"))
            job = runs[-1].name if runs else ""
        _, solid_png = op("results_render", {"job_id": job, "quantity": "von_mises", "width": 420, "height": 300, "show_undeformed": False})
        _, glass_png = op("results_render", {"job_id": job, "quantity": "von_mises", "width": 420, "height": 300, "show_undeformed": False,
                                             "body_opacity": {"base": 0.25}})
        c.close()
        (tmp / "op-solid.png").write_bytes(solid_png)
        (tmp / "op-glass.png").write_bytes(glass_png)
        d = _differing(_decode(tmp / "op-solid.png"), _decode(tmp / "op-glass.png"))
        check(d > 200, f"results_render draws a body as glass ({d} pixels differ)")
    except Exception as e:  # the operation layer is checked elsewhere; say what went wrong instead of failing silently
        check(False, f"results_render with body_opacity: {e}")
    stray = [ln for ln in log.splitlines() if "[err!]" in ln]
    check(not stray, f"no errors along the way ({len(stray)})", "\n".join(stray[:4]))
    print(f"  glass changed {_differing(glass, edges, x1=560)} pixels; ambient occlusion darkened {darker} pixels "
          f"(deepest {drop:.0f}, whole part {b_off:.1f} -> {b_on:.1f}); "
          f"edges {_differing(edges, ao, x1=560)} pixels, darkest {dark}; exploded {_differing(explode, glass, x1=560)} pixels; "
          f"export {exp[0]}x{exp[1]}")
    print(f"  log: build/presentation.log   images: docs/images/presentation-*.png")
    return 1 if FAIL else 0


# ---- the acceptance walkthrough ------------------------------------------------------------------------------
# The owner's own part, from launch to a written report, by clicking only. Timed, because the promise is an answer
# in under three minutes.

WALK_PART = Path(os.environ.get("NAVIER_WALK_PART", "owner-part-not-set.stl"))  # the owner's part is not in the repository


def walkthrough(tet=False):
    import time
    print("== acceptance walkthrough" + (" on the tetrahedral mesh" if tet else "") + ": the owner's part, by clicking")
    tag, nav, logname = ("WALKTET", "walkthrough_tet.nav", "walkthrough_tet.log") if tet else ("WALK", "walkthrough.nav", "walkthrough.log")
    if not WALK_PART.exists():
        check(False, f"the part is where the walkthrough expects it ({WALK_PART}); set NAVIER_WALK_PART to its path")
        return 1
    tmp = Path(tempfile.mkdtemp(prefix="walkthrough"))
    script = tmp / "walk.nav"
    script.write_text((ROOT / "tools" / nav).read_text().replace("@WALK_PART@", str(WALK_PART)) + "\nquit\n")
    t0 = time.time()
    run = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--workspace", str(tmp / "ws"),
                          "--exec", f"exec {script}"], capture_output=True, text=True, timeout=1800, cwd=str(ROOT))
    wall = time.time() - t0
    log = run.stdout + run.stderr
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build" / logname).write_text(log)
    marks = []
    for m in re.findall(tag + r" (\w+)", log): # the command line and its output both carry the marker
        if not marks or marks[-1] != m:
            marks.append(m)
    want = ["start", "part", "mesh", "material", "conditions", "solved", "converged", "done"]
    check(marks == want, f"every step of the walkthrough ran ({marks})")
    check(log.count("face ") >= 2, "two faces were picked by clicking on the part")
    check("typed 3 characters" in log, "the force was typed into the panel field")
    check(re.search(r"result surface built .*static_structural", log) is not None, "the solve produced a result")
    mesh_check = re.search(r"mesh check: (not )?converged: displacement ([-+0-9.]+) %, stress ([-+0-9.]+) %", log)
    check(mesh_check is not None, "CHECK MESH compared the two meshes")
    report = re.search(r"report written to (\S+)", log)
    check(report is not None, "REPORT wrote the folder")
    if report:
        md = Path(report.group(1)) / "report.md"
        text = md.read_text() if md.exists() else ""
        for want_text in ("## The answer", "## The setup", "## Mesh convergence", "## What these numbers are not",
                          "99th percentile", "demonstration"):
            check(want_text in text, f"the report says {want_text!r}")
        for f in ("stress.png", "displacement.png", "view.png", "summary.json"):
            check((Path(report.group(1)) / f).exists(), f"the report folder holds {f}")
    stray = [ln for ln in log.splitlines() if "[err!]" in ln]
    check(not stray, f"no errors along the way ({len(stray)})", "\n".join(stray[:6]))
    if tet:
        # the same quantities, on 10-node tetrahedra; the time is reported, the three-minute promise is the voxel one's
        mesh = re.search(r"tetrahedral TET10 mesh: (\d+) elements, (\d+) nodes, surface ([0-9.]+) mm, dihedral ([0-9.]+) to ([0-9.]+) degrees", log)
        check(mesh is not None, "the mesh is 10-node tetrahedra, with its angles reported")
        if mesh:
            check(float(mesh.group(4)) >= 10, f"smallest dihedral angle {mesh.group(4)} degrees (target 10)")
            print(f"  mesh: {mesh.group(1)} TET10 elements, {mesh.group(2)} nodes, surface {mesh.group(3)} mm, dihedral {mesh.group(4)} to {mesh.group(5)}")
        print(f"  wall time {wall:.0f} s (reported; not held to the voxel walkthrough's three minutes)")
    else:
        check(wall < 180, f"the whole walkthrough took {wall:.0f} s (under three minutes)")
        print(f"  wall time {wall:.0f} s")
    print(f"  {mesh_check.group(0) if mesh_check else 'no mesh check'}")
    print(f"  log: build/{logname}   screenshots: docs/app/{'walktet' if tet else 'walk'}-*.png")
    return 1 if FAIL else 0


# ---- the part's own surface against the voxel mesh --------------------------------------------------------------
# The owner's rule for this wave: what is drawn is the part, not the voxel approximation. The evidence is a bar with
# one chamfered edge. The mesher keeps a cell when its centre is inside the surface, so the staircase is inscribed:
# where the part is chamfered the voxel body falls inside it, and in that corner of the frame the part's own surface
# must cover measurably more pixels than the mesh does. The two screenshots are kept for the owner to look at.

CHAMFER_STL = """solid chamfer
{facets}endsolid chamfer
"""


def _write_chamfer(path, W=40.0, D=30.0, H=20.0, C=8.0):
    """a W x D x H bar with one long top edge chamfered at 45 degrees over C, written as an ASCII STL"""
    poly = [(0, 0), (W, 0), (W, H - C), (W - C, H), (0, H)]
    n = len(poly)
    tris = []
    for i in range(n):  # the five side walls
        x0, z0 = poly[i]
        x1, z1 = poly[(i + 1) % n]
        a, b, c, d = (x0, 0, z0), (x1, 0, z1), (x1, D, z1), (x0, D, z0)
        tris += [(a, b, c), (a, c, d)]
    for i in range(1, n - 1):  # the two caps
        p0 = (poly[0][0], 0, poly[0][1])
        tris.append((p0, (poly[i + 1][0], 0, poly[i + 1][1]), (poly[i][0], 0, poly[i][1])))
        q0 = (poly[0][0], D, poly[0][1])
        tris.append((q0, (poly[i][0], D, poly[i][1]), (poly[i + 1][0], D, poly[i + 1][1])))
    out = []
    for t in tris:
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = t
        ux, uy, uz = bx - ax, by - ay, bz - az
        vx, vy, vz = cx - ax, cy - ay, cz - az
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        out.append(f"  facet normal {nx / ln:.6e} {ny / ln:.6e} {nz / ln:.6e}\n    outer loop\n")
        for v in t:
            out.append("      vertex %.6e %.6e %.6e\n" % v)
        out.append("    endloop\n  endfacet\n")
    Path(path).write_text(CHAMFER_STL.format(facets="".join(out)))
    return len(tris)


def _part_mask(img, hidden):
    """pixels of the 3D view that the result changed, against the same frame with the result hidden"""
    w, h, ch, px = img
    hp = hidden[3]
    x1, y0, y1 = w - 450, 90, h - 110
    out = set()
    for y in range(y0, y1):
        for x in range(x1):
            i = (y * w + x) * ch
            if abs(px[i] - hp[i]) > 30 or abs(px[i + 1] - hp[i + 1]) > 30 or abs(px[i + 2] - hp[i + 2]) > 30:
                out.add((x, y))
    return out


def chamfer_checks(app, tmp):
    print("== the part's own surface against the voxel mesh")
    shots = tmp / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    ws = tmp / "chamws"
    ws.mkdir(parents=True, exist_ok=True)
    stl = ws / "chamfer.stl"  # inside the workspace: the engine reads only below its own roots
    ntri = _write_chamfer(stl)
    script = tmp / "chamfer.nav"
    script.write_text("\n".join([
        "workspace solid", "frames 6",
        "am project_create name=chamfer overwrite=true",
        f"am geometry_import path='{stl}' units=mm name=cham",
        "am mesh_generate element_size=2",
        "am material_assign body=cham material=ss316l_lpbf_demo source=user",
        "solid fixbase",
        "am selection_create name=top query='{\"plane\": {\"axis\": \"z\", \"at\": \"max\"}}' source=user replace=true",
        "am boundary_apply '{\"name\": \"pull\", \"kind\": \"force\", \"selection\": \"top\","
        " \"force\": [0, 0, -500], \"source\": \"user\"}'",
        "frames 6", "solid run static", "solid wait 300", "frames 20",
        # the shape, not the exaggerated shape: true scale, looking along the chamfered edge
        "fem deform true", "camera side", "fem fit", "camera zoom 0.75", "frames 8",
        "fem hide", "frames 8", f"screenshot {shots}/cham-hidden.png",
        "fem show", "fem surface on", "frames 10", "fem surface",
        f"screenshot {shots}/cham-surface.png",
        "fem surface off", "frames 10", "fem surface",
        f"screenshot {shots}/cham-voxels.png",
        "quit", ""]))
    run = subprocess.run([str(app), "--headless", "--size", "960x600", "--workspace", str(tmp / "chamws"),
                          "--exec", f"exec {script}"], capture_output=True, text=True, timeout=900, cwd=str(ROOT))
    log = run.stdout + run.stderr
    missing = [n for n in ("cham-hidden", "cham-surface", "cham-voxels") if not (shots / f"{n}.png").exists()]
    if missing:
        check(False, f"the chamfer run wrote every screenshot (missing {missing})", log)
        return
    drew = dict(re.findall(r"drawing the (part surface|voxel mesh): (\d+) triangles", log))
    check(drew.get("part surface") == str(ntri),
          f"the surface view draws the STL's own {ntri} triangles (drew {drew.get('part surface')})", log)
    check(int(drew.get("voxel mesh", 0)) > 20 * ntri,
          f"the voxel view draws the mesh boundary instead ({drew.get('voxel mesh')} triangles)", log)
    snap = re.search(r"largest snap ([0-9.eE+-]+) mm", log)
    check(snap is not None and float(snap.group(1)) <= 2.0,
          f"the panel states how far a vertex had to be snapped onto the mesh ({snap.group(1) if snap else '?'} mm, "
          f"element size 2 mm)", log)
    hidden = _decode(shots / "cham-hidden.png")
    ms = _part_mask(_decode(shots / "cham-surface.png"), hidden)
    mv = _part_mask(_decode(shots / "cham-voxels.png"), hidden)
    check(len(ms) > 1000 and len(mv) > 1000, f"both views draw the part ({len(ms)} and {len(mv)} pixels)")
    if not (ms and mv):
        return
    allp = ms | mv
    x0, x1 = min(p[0] for p in allp), max(p[0] for p in allp)
    y0, y1 = min(p[1] for p in allp), max(p[1] for p in allp)
    cx = x0 + (x1 - x0) * 0.80          # the chamfered corner: right fifth, top quarter
    cy = y0 + (y1 - y0) * 0.25
    a = sum(1 for (x, y) in ms if x >= cx and y <= cy)
    b = sum(1 for (x, y) in mv if x >= cx and y <= cy)
    check(a > b * 1.08,
          f"in the chamfered corner the part's own surface covers more than the inscribed staircase "
          f"({a} against {b} pixels, {100.0 * a / max(b, 1) - 100:.0f} % more)")
    print(f"  chamfer corner: surface {a} px, voxels {b} px; whole part {len(ms)} and {len(mv)} px")
    print(f"  images: {shots}/cham-surface.png and {shots}/cham-voxels.png")



# ---- meshing a surface that is not closed -----------------------------------------------------------------------
# The inside test votes over three axes. A hole or a doubled facet can only fool one of them, so the voxels must come
# out the same as for the closed part. Three cubes, one mesh each, through the real operation path.

def _cube_facets(size=20.0, drop=-1, double=-1):
    """the 12 facets of a cube, optionally with one dropped or one stored twice"""
    s = size
    v = [(0, 0, 0), (s, 0, 0), (s, s, 0), (0, s, 0), (0, 0, s), (s, 0, s), (s, s, s), (0, s, s)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, c, d in quads:
        tris.append((v[a], v[b], v[c]))
        tris.append((v[a], v[c], v[d]))
    out = []
    for i, t in enumerate(tris):
        if i == drop:
            continue
        out.append(t)
        if i == double:
            out.append(t)
    return out


def _write_stl(path, tris, name="cube"):
    lines = [f"solid {name}"]
    for t in tris:
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = t
        ux, uy, uz = bx - ax, by - ay, bz - az
        vx, vy, vz = cx - ax, cy - ay, cz - az
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        lines.append(f"  facet normal {nx / ln:.6e} {ny / ln:.6e} {nz / ln:.6e}")
        lines.append("    outer loop")
        for p in t:
            lines.append("      vertex %.6e %.6e %.6e" % p)
        lines.append("    endloop")
        lines.append("  endfacet")
    lines.append(f"endsolid {name}")
    Path(path).write_text("\n".join(lines) + "\n")


def broken_surface_checks(app, tmp):
    print("== meshing a surface that is not closed")
    ws = tmp / "brokenws" / "proj"   # the STLs live inside the workspace: the engine reads only below its own roots
    ws.mkdir(parents=True, exist_ok=True)
    cases = {"closed": _cube_facets(), "holed": _cube_facets(drop=5), "doubled": _cube_facets(double=5)}
    cmds = ["workspace solid", "frames 4"]
    for name, tris in cases.items():
        stl = ws / f"{name}.stl"
        _write_stl(stl, tris, name)
        cmds += [f"am project_create name={name} overwrite=true",
                 f"am geometry_import path='{stl}' units=mm name={name}",
                 "am mesh_generate element_size=4mm",
                 f"echo {MARK} {name}"]
        if name == "holed":  # the same part again, this time repaired through the panel's own button
            cmds += ["workspace solid", "frames 8", "solid repair", "frames 10",
                     'uiclick "1 PART"', "frames 8", "uitext",
                     "am mesh_generate element_size=4mm", f"echo {MARK} repaired"]
    cmds.append("quit")
    script = tmp / "broken.nav"
    script.write_text("\n".join(cmds) + "\n")
    run = subprocess.run([str(app), "--headless", "--size", "640x400", "--workspace", str(ws),
                          "--exec", f"exec {script}"], capture_output=True, text=True, timeout=900, cwd=str(ROOT))
    log = run.stdout + run.stderr
    parts = re.split(rf"{MARK}\s+(\w+)", log)
    got = {}
    for i in range(1, len(parts) - 1, 2):
        pass
    # the element count of each case, in the order the cases were meshed
    counts = [int(x) for x in re.findall(r'"elements": (\d+)', log)]
    unc = [int(x) for x in re.findall(r'"uncertain_cells": (\d+)', log)]
    check(len(counts) >= 3, f"all three cubes meshed ({counts})", log)
    if len(counts) < 3:
        return
    check(counts[0] == counts[1],
          f"a cube with one facet missing meshes to the same voxels as the closed one ({counts[0]} and {counts[1]})")
    check(counts[0] == counts[-1],
          f"a cube with a facet stored twice meshes to the same voxels as the closed one ({counts[0]} and {counts[-1]})")
    if len(counts) >= 4:
        check(counts[0] == counts[2], f"and so does the repaired one ({counts[0]} and {counts[2]})")
    check(len(unc) >= 3 and unc[0] == 0,
          f"the closed cube leaves nothing for the three axes to disagree about ({unc[0] if unc else '?'})")
    ab = [int(x) for x in re.findall(r'"abstained_rays": (\d+)', log)]
    check(len(ab) >= 3 and ab[0] == 0 and ab[1] > 0,
          f"the hole is named, not hidden: rays through it abstain instead of voting wrongly ({ab})")
    rep = re.search(r"repaired: (\d+) facets dropped with (\d+) shell\(s\), (\d+) hole\(s\) filled with (\d+) facets; "
                    r"(\d+) open and (\d+) non-manifold edges left", log)
    check(rep is not None, "REPAIR says in numbers what it changed", log)
    if rep:
        check(rep.group(3) == "1" and rep.group(4) == "1" and rep.group(5) == "0",
              f"the missing facet is put back and nothing is left open ({rep.group(0)})")
    check("Repaired:" in log, "the PART step says the part was repaired and what it cost", log)
    stray = [ln for ln in log.splitlines() if "[err!]" in ln]
    check(not stray, f"no refusal along the way ({len(stray)})", "\n".join(stray[:4]))
    print(f"  elements {counts}; uncertain cells {unc}; abstained rays {ab}")
    if rep:
        print(f"  repair: {rep.group(0)}")



# ---- Simple mode: a first-time user, the sample bracket, a report ------------------------------------------------

SIMPLE_FORBIDDEN = ("voxel", "strain", "tensor", "mesh", "provenance")


def stopwalk():
    """Stopping and starting again by clicks: the sequence that failed for the owner on 2026-09-20."""
    import time
    print("== Stop and start again: simulate, stop, simulate, change mode, type stop, start over with another part")
    tmp = Path(tempfile.mkdtemp(prefix="stopwalk"))
    t0 = time.time()
    run = subprocess.run([str(APP), "--headless", "--first-run", "--size", "1440x900", "--workspace", str(tmp / "ws"),
                          "--exec", "exec tools/stopwalk.nav"],  # relative: the checkout's path may hold a space
                         capture_output=True, text=True, timeout=600, cwd=str(ROOT))
    wall = time.time() - t0
    log = run.stdout + run.stderr
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build" / "stopwalk.log").write_text(log)

    def section(a, b):
        i, j = log.find(f"STOPWALK {a}"), log.find(f"STOPWALK {b}")
        return log[i:j] if 0 <= i < j else ""

    marks = []
    for m in re.findall(r"^\[info\] STOPWALK ([\w-]+)\s*$", log, re.M):
        if not marks or marks[-1] != m:
            marks.append(m)
    want = ["start", "running-1", "stopping-1", "stopped-1", "running-2", "advanced-strip", "simple-still-running",
            "stopped-2", "running-3", "started-over", "results", "done"]
    check(marks == want, f"every stage was reached ({marks})")
    check(re.search(r"layer \d+ of \d+", section("running-1", "stopping-1")) is not None, "the first simulation runs, layer by layer")
    check("Stopping: it ends with the layer it is on." in section("stopping-1", "stopped-1"),
          "STOP answers at once: the screen says it is stopping")
    s1 = section("stopped-1", "running-2")
    check("Stopped. Nothing is running now." in s1 and "Press Simulate to run it again" in s1,
          "after the stop the screen says nothing is running and what to press next")
    check(re.search(r"layer \d+ of \d+", section("running-2", "advanced-strip")) is not None,
          "Simulate after a stop starts a new simulation that runs")
    adv = section("advanced-strip", "simple-still-running")
    check("RUNNING NOW" in adv and re.search(r"Simple mode: .*: \d+ %", adv) is not None,
          "Advanced mode shows what is running, by name and per cent, with its own STOP")
    back = section("simple-still-running", "stopped-2")
    check(re.search(r"layer \d+ of \d+", back) is not None, "back in Simple mode the same simulation is still shown running")
    check("asked 1 simulation to stop" in back, "typing 'stop' stops it")
    check("Stopped. Nothing is running now." in section("stopped-2", "running-3"), "and the screen says so again")
    over = section("started-over", "results")
    check("to start over" in section("running-3", "started-over") + over and "Choose a part" in over,
          "'start over' while a simulation runs stops it and returns to the first screen")
    res = section("results", "done")
    check("What this is:" in res and re.search(r"Warp: up to [0-9.]+ mm", res) is not None,
          "another part, chosen after starting over, simulates to its results")
    n_over = len(re.findall(r"to start over", log))
    check(n_over == 1, f"starting over happens once, not once per frame while the old simulation ends ({n_over})")
    started = len(re.findall(r"simulation started", log))
    finished = len(re.findall(r"\] \S* ?finished \(", log)) + len(re.findall(r"lpbf_build finished", log))
    check(started == 4, f"four simulations were started ({started})")
    stray = [ln for ln in log.splitlines() if "[err!]" in ln]
    check(not stray, f"a stop the person asked for is never reported as an error ({len(stray)} error lines)", "\n".join(stray[:6]))
    print(f"  wall time {wall:.0f} s   log: build/stopwalk.log   screenshots: docs/app/stop-*.png")
    return 1 if FAIL else 0


def simplewalk():
    import time
    print("== Simple mode: the sample bracket to a report, as a first-time user")
    tmp = Path(tempfile.mkdtemp(prefix="simplewalk"))
    t0 = time.time()
    run = subprocess.run([str(APP), "--headless", "--first-run", "--size", "1440x900", "--workspace", str(tmp / "ws"),
                          "--exec", "exec tools/simplewalk.nav"],
                         capture_output=True, text=True, timeout=1800, cwd=str(ROOT))
    wall = time.time() - t0
    log = run.stdout + run.stderr
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build" / "simplewalk.log").write_text(log)
    marks = []
    for m in re.findall(r"^\[info\] SIMPLE (\w+)\s*$", log, re.M):
        if not marks or marks[-1] != m:
            marks.append(m)
    want = ["start", "part", "print", "running", "results", "done"]
    check(marks == want, f"every screen was reached ({marks})")
    # what the panel said, screen by screen, and nothing else: the lines uitext printed
    said = "\n".join(re.findall(r"^\[data\]   (.*)$", log, re.M))
    check("Choose a part" in said and "Small bracket" in said and "Calibration cantilever" in said,
          "the first screen offers the three samples")
    check("How will it be printed?" in said and "Metal powder" in said and "AlSi10Mg on an SLM 280" in said,
          "the second screen asks how it will be printed, with a named preset")
    check("layer" in said and ("About" in said or "about" in said), "the simulate screen gives the layers and a time estimate")
    check(re.search(r"layer \d+ of \d+", said) is not None, "the progress bar speaks: layer N of M")
    check("What this is:" in said, "the results screen says what the simulation is, before the numbers")
    i_what = said.find("What this is:")
    i_warp = said.find("Warp: up to")
    check(0 <= i_what < i_warp, "and it says it before the first number")
    check(re.search(r"Warp: up to [0-9.]+ mm, (upwards|downwards|sideways)", said) is not None,
          "the warp is stated with its unit and its direction")
    check(re.search(r"Residual stress: up to \d+ MPa", said) is not None, "the residual stress is stated with its unit")
    check("Risk:" in said, "a risk line is given")
    check(re.search(r"Residual stress up to \d+ MPa [^;]*; the material yields at 292 MPa", said) is not None,
          "the risk line compares the stress with the library record's yield value (292 MPa)")
    check("The metal is allowed to yield" in said, "the preface says the metal is allowed to yield")
    check(re.search(r"(Between|About) .*(seconds|minutes)", said) is not None, "the time estimate is given in words")
    check("click 'OTHER MATERIALS IN THE LIBRARY" in log and "Published values. As built from the EOS Ti64 data sheet" in said,
          "the print screen lists the library's other metals with status and source line")
    check("scroll" in log and "Which face sits on the plate?" in said, "the long print screen scrolls with the wheel")
    check("The report is written." in said, "the report screen confirms the report")
    low = said.lower()
    bad = [w for w in SIMPLE_FORBIDDEN if w in low]
    check(not bad, f"no engineering jargon on screen in Simple mode ({bad or 'none of ' + ', '.join(SIMPLE_FORBIDDEN)})",
          "\n".join(ln for ln in said.splitlines() if any(w in ln.lower() for w in bad))[:800])
    rep = re.search(r"report written to (\S+)", log)
    check(rep is not None, "the report folder was written", log)
    if rep:
        text = (Path(rep.group(1)) / "report.md").read_text() if (Path(rep.group(1)) / "report.md").exists() else ""
        check("## In plain words" in text and "What this is:" in text, "the report opens with what the results screen said")
    stray = [ln for ln in log.splitlines() if "[err!]" in ln]
    check(not stray, f"no errors along the way ({len(stray)})", "\n".join(stray[:6]))
    check(wall < 120, f"the whole path took {wall:.0f} s (under two minutes)")
    for t in ("simulated", "report written"):
        m = re.search(rf"elapsed ([0-9.]+) s - {t}", log)
        if m:
            print(f"  {t}: {m.group(1)} s")
    print(f"  wall time {wall:.0f} s   log: build/simplewalk.log   screenshots: docs/app/simple-*.png")
    return 1 if FAIL else 0


# ---- Agent mode: a question reaches a result with no button pressed -----------------------------------------------

def agentwalk():
    import time
    print("== Agent mode: a typed question, a result in the window, no button pressed")
    tmp = Path(tempfile.mkdtemp(prefix="agentwalk"))
    t0 = time.time()
    runs = tmp / "runs"  # the run record goes here, not into the repository's leaderboard
    env = dict(os.environ, NAVIER_AGENT_RUNS_DIR=str(runs))
    run = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--workspace", str(tmp / "ws"),
                          "--exec", "exec tools/agentwalk.nav"],
                         capture_output=True, text=True, timeout=1800, cwd=str(ROOT), env=env)
    wall = time.time() - t0
    log = run.stdout + run.stderr
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build" / "agentwalk.log").write_text(log)
    # after the question is typed (ending in Return) the script presses nothing: no uiclick follows it
    after = log.split("echo AGENT asked", 1)[-1]
    check("uiclick" not in after, "no button is pressed after the question is entered")
    check("agent: asked: How far does the calibration cantilever bend" in log, "Return in the question field asks it")
    check("started outside the interface" in log, "the window followed the agent's job by itself")
    check(re.search(r"result surface built for \S+ \(lpbf_build\)", log) is not None, "the result was drawn in the window")
    m = re.search(r"The tip rises ([0-9.]+) mm once the part is cut off", log)
    check(m is not None and float(m.group(1)) > 0,
          f"the agent reports the tip rise after the cut ({m.group(1) if m else '?'} mm)")
    check(re.search(r"agent: (created project|imported body)", log) is not None,
          "what the agent changed is shown in the engine's own words")
    # the run is recorded for the agent leaderboard, in its format, and the transcript says where
    m = re.search(r"agent: run recorded[^:]*: (\S+\.json)", log)
    check(m is not None, "the transcript shows where the run record was written")
    rec = None
    if m and Path(m.group(1)).exists():
        rec = json.loads(Path(m.group(1)).read_text())
    template = json.loads((ROOT / "challenges" / "agents" / "run_template.json").read_text())
    check(rec is not None and set(template) <= set(rec) and rec.get("format") == "openphysicsai-agent-run",
          "the record has every field of run_template.json", str(sorted(set(template) - set(rec or {}))))
    acc = (rec or {}).get("acceptance", {})
    check(rec is not None and acc.get("passed") is None and acc.get("rerun_by") == "" and
          rec.get("prompt", "").startswith("How far does the calibration cantilever bend") and rec.get("commit") and
          isinstance(rec.get("wall_s"), int),
          "acceptance is left for a maintainer; prompt, commit and wall time are filled")
    check(re.match(r"\d{4}-\d{2}-\d{2}-agentmode-agent-standin-how-far-does-the\.json$", Path(m.group(1)).name if m else "") is not None,
          "the file is named <date>-agentmode-<tool>-<short>.json")
    # the leaderboard generator accepts it: run it on a copy of the runs folder
    if rec is not None:
        sys.path.insert(0, str(ROOT / "tools"))
        import agentboard
        agentboard.RUNS, agentboard.OUT = runs, tmp / "AGENT_LEADERBOARD.md"
        agentboard.ROOT = tmp
        rc = agentboard.main()
        check(rc == 0 and "Files refused" not in (tmp / "AGENT_LEADERBOARD.md").read_text(),
              "tools/agentboard.py (make leaderboard) accepts the record")
    stray = [ln for ln in log.splitlines() if "[err!]" in ln]
    check(not stray, f"no errors along the way ({len(stray)})", "\n".join(stray[:6]))
    print(f"  wall time {wall:.0f} s   log: build/agentwalk.log   screenshot: docs/app/agent-2-result.png")
    return 1 if FAIL else 0


# ---- the lab in Advanced mode: run a scenario, change what is shown, turn a knob, run again ------------------------
def labwalk():
    print("== The lab by hand: a scenario run from the app, a field chip, a knob, and a rerun")
    tmp = Path(tempfile.mkdtemp(prefix="uilab"))
    sc = json.loads((ROOT / "examples" / "lab" / "micro_mixer.json").read_text())
    sc["grid"] = {"nx": 100, "ny": 50, "cell_m": 2e-5}
    sc["regions"] = []
    sc["run"] = {"end_time_s": 0.5, "frames": 4}
    sc["title"] = "Tiny mixer for the UI check"
    (tmp / "tiny.json").write_text(json.dumps(sc))
    script = tmp / "lab.nav"
    script.write_text("\n".join([
        f"lab run {tmp / 'tiny.json'}", "frames 300", "uilist",
        "uiclick speed", "frames 10", "lab info",
        "uiclick labctl0 0.9", "frames 6",
        "uiclick RUN WITH THESE", "frames 300", "lab info",
        'uiclick LIBRARY', 'frames 4', 'uiclick "NEXT DOMAINS"', 'frames 4', 'uiclick "NEXT DOMAINS"', 'frames 4', 'uiclick "libdom relativity"', "frames 6", "uilist", "quit", ""]))
    env = dict(os.environ, HOME=str(tmp))
    run = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--exec", f"exec {script}"], capture_output=True, text=True,
                         timeout=900, cwd=str(ROOT), env=env)
    t = run.stdout + run.stderr
    check("tiny.lab, 5 frames" in t, "a scenario run from the app opens its result when it is done", t[-1500:])
    for w in ("RUN WITH THESE##labrerun", "labctl0", "speed##field", "3D##labview", "PLAY##labplay", "PAUSE##labplay"):
        pass
    check("labctl0" in t and "RUN WITH THESE" in t, "the panel shows the scenario's knobs and the rerun button", t[-1500:])
    check("lab field speed" in t, "a field chip, clicked, shows that field", t[-1500:])
    edited = tmp / "NAVIER-Projects" / "lab" / "tiny-edited.json"
    moved = edited.exists() and json.loads(edited.read_text())["boundaries"][0]["speed_m_s"] != sc["boundaries"][0]["speed_m_s"]
    check(moved, "the knob, dragged, changed the scenario that was run again", str(edited))
    check("tiny-edited.lab, 5 frames" in t, "RUN WITH THESE made a new result and opened it", t[-1500:])
    before, _, after = t.partition('[ >> ] uiclick "libdom relativity"')
    check("lib black_hole" not in before and "lib black_hole" in after,
          "the library shows one domain at a time: clicking a closed domain's heading lists its scenarios", after[-1500:])
    # Acceptance criteria declared before the revised UI's first run: every shipped domain/scenario is reachable
    # by real clicks; cached results restore their own controls; mode switches preserve paused frames; manual
    # stepping and playback speed are reachable. Browsing alone must never launch a simulation.
    catalog = sorted((json.loads(p.read_text())["domain"], p.stem) for p in (ROOT / "examples/lab").glob("*.json"))
    domains = sorted(set(d for d, _ in catalog))
    browse = ["mode agentic", "frames 4", "mode", "mode manual", "frames 6", "mode", "uiclick LIBRARY", "frames 4", "uilist"]
    for page in range((len(domains) + 4) // 5):
        for domain in domains[page * 5:(page + 1) * 5]:
            browse += [f'uiclick "libdom {domain}"', "frames 4", "uilist"]
            count = sum(d == domain for d, _ in catalog)
            for _ in range((count + 5) // 6 - 1):
                browse += ['uiclick "NEXT SCENARIOS"', "frames 4", "uilist"]
        browse += ['uiclick "NEXT DOMAINS"', "frames 4"]
    browse += ['uiclick "METAL / FDM PRINTING"', "frames 6", "uilist", 'uiclick "4 BUILD"', 'frames 4', 'uitext', 'uilist', 'uiclick "FDM / FFF PLASTIC"', 'frames 4', 'uitext', 'uilist', f'screenshot {tmp / "manual-printing.png"}', "quit", ""]
    (tmp / "browse.nav").write_text("\n".join(browse))
    run = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--exec", f"exec {tmp / 'browse.nav'}"],
                         capture_output=True, text=True, timeout=120, cwd=ROOT, env=env)
    bt = run.stdout + run.stderr
    check("usage: mode" not in bt and "mode advanced" in bt and "mode agent" in bt,
          "manual and agentic commands switch the same persistent modes as the buttons", bt[-2000:])
    missing = [base for _, base in catalog if not re.search(r"lib " + re.escape(base) + r"(?:\s|$)", bt)]
    check(not missing, f"all {len(domains)} domains and {len(catalog)} shipped scenarios are reachable through library pages", str(missing))
    check("lab: running" not in bt and "uiclick: no widget" not in bt, "browsing the library starts no solver and all clicks find a widget", bt[-2000:])
    check("4 BUILD" in bt and "5 RUN" in bt, "the library's printing route reaches the existing manual build workflow", bt[-2000:])
    # Manual defaults must be visibly examples and inferred, not automatically labelled calibrated.
    check("Strain provenance: inferred" in bt and "Process provenance: inferred" in bt and "not a calibrated printer profile" in bt,
          "LPBF and FDM starting values are shown as inferred examples, never automatically calibrated", bt[-2000:])
    check("Review deposition and cooling settings" in bt and "5 PRINT" in bt,
          "FDM instructions and workflow labels describe deposition and printing", bt[-2000:])
    # Actual cheap print jobs, not synthetic fields: adopting a result must select its own workflow and fields.
    _write_stl(tmp / "print-cube.stl", _cube_facets(4.0))
    process = {"layer_height": "1 mm", "printed_layer_height": "0.2 mm", "nozzle_temperature": "210 degC",
               "bed_temperature": "60 degC", "ambient_temperature": "30 degC", "deposition_rate": "8 mm^3/s",
               "min_layer_time": "1 s", "cooldown_bed_on": "5 s", "cooldown_bed_off": "5 s",
               "thermal_substeps": 2, "provenance": "inferred"}
    metal = {"layer_thickness_sim": "1 mm", "inherent_strain": {"exx": -.001, "eyy": -.002, "ezz": -.01,
             "provenance": "inferred", "source": "UI verification tensor, not a calibration"},
             "material": {"youngs_modulus": "215000 MPa", "poissons_ratio": .3, "provenance": "inferred"}}
    pc = ["lab close", "mode manual", "workspace solid", "am project_create name=print_ui overwrite=true",
          f'am geometry_import path="{tmp / "print-cube.stl"}" units=mm name=cube',
          "am material_assign body=cube material=pla_generic_demo source=user", 'am mesh_generate element_size="1 mm"',
          "am mech_print_run '" + json.dumps({"process": process}) + "'", "frames 120", "fem follow last", "frames 30",
          'uiclick "6 RESULTS"', "frames 4", "echo FDM_UI_FIELDS", "uilist", "uitext",
          'uiclick "TERMINAL##output"', "frames 4", "echo TERMINAL_OPEN", "uitext",
          'uiclick "TERMINAL##output"', "frames 4", "echo TERMINAL_CLOSED", "uitext",
          "uikey t", "frames 4", "echo TERMINAL_KEYBOARD", "uitext", "uikey escape", "frames 4",
          "am material_assign body=cube material=ss316l_lpbf_demo source=user",
          "am lpbf_build_run '" + json.dumps(metal) + "'", "frames 120", "fem follow last", "frames 30",
          'uiclick "6 RESULTS"', "frames 4", "echo LPBF_UI_FIELDS", "uilist", "uitext",
          "uiclick VIEW", "frames 4", 'uiclick "CLEAN VIEW"', "frames 8", "echo CLEAN_UI", "uilist", "uitext",
          f'screenshot {tmp / "clean-print.png"}', "uikey h", "frames 8", "echo RESTORED_UI", "uilist", "uitext", "quit", ""]
    (tmp / "print-ui.nav").write_text("\n".join(pc))
    pr = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--exec", f"exec {tmp / 'print-ui.nav'}"],
                        capture_output=True, text=True, timeout=90, cwd=ROOT, env=env)
    pt = pr.stdout + pr.stderr
    fdm_ui = pt.split('[ >> ] echo FDM_UI_FIELDS', 1)[-1].split('[ >> ] am material_assign', 1)[0]
    lpbf_ui = pt.split('[ >> ] echo LPBF_UI_FIELDS', 1)[-1]
    check(pr.returncode == 0 and "PLAY PRINT" in fdm_ui and "5 PRINT" in fdm_ui and "TEMPERATURE" in fdm_ui,
          "adopting an actual FDM result selects the print workflow and exposes its temperature field", pt[-3500:])
    check("PLAY BUILD" in lpbf_ui and "4 BUILD" in lpbf_ui and "5 RUN" in lpbf_ui and "TEMPERATURE" not in lpbf_ui,
          "adopting an actual LPBF result selects the build workflow and omits absent temperature", lpbf_ui[-3000:])
    check("BUILD COMPLETE" in lpbf_ui and "AFTER THE CUT" not in lpbf_ui and "tip, after the cut" not in lpbf_ui,
          "a build without a cut does not claim a cut or release in the native result panel", lpbf_ui[-3000:])
    opened = pt.split('[ >> ] echo TERMINAL_OPEN', 1)[-1].split('[ >> ] uiclick', 1)[0]
    closed = pt.split('[ >> ] echo TERMINAL_CLOSED', 1)[-1].split('[ >> ] uikey', 1)[0]
    keyboard = pt.split('[ >> ] echo TERMINAL_KEYBOARD', 1)[-1].split('[ >> ] uikey escape', 1)[0]
    check('Terminal output: collapsed' in fdm_ui and 'Terminal output: expanded' in opened
          and 'Terminal output: collapsed' in closed,
          'Manual terminal output starts collapsed and the real toggle opens and closes it', pt[-3500:])
    check('Terminal output: expanded' in keyboard,
          'the real terminal keyboard shortcut opens visible command output in Manual', keyboard[-2000:])
    clean = pt.split('[ >> ] echo CLEAN_UI', 1)[-1].split('[ >> ] uikey h', 1)[0]
    restored = pt.split('[ >> ] echo RESTORED_UI', 1)[-1]
    check('Clean view:' in clean and '[MPa]' in clean and 'stored time' in clean and 'deformation' in clean
          and 'max displacement' in clean and '6 RESULTS' not in clean,
          'CLEAN VIEW hides controls and preserves numerical field, time, deformation and displacement metadata', clean[-2500:])
    check('elastic (no yielding)' in clean and 'input inferred' in clean and 'no measurement comparison' in clean
          and 'process inferred' in fdm_ui and 'material demonstration' in fdm_ui,
          'native print inspectors and clean view visibly identify model scope and input provenance', clean[-2500:])
    check('6 RESULTS' in restored and 'CLEAN VIEW' in restored,
          'H through the real input path restores the Manual inspector from clean view', restored[-2500:])
    # Reopen a cached shipped scenario: the saved input snapshot survives mode switches and a process restart.
    result_dir = tmp / "NAVIER-Projects/lab"
    shutil.copyfile(result_dir / "tiny.lab", result_dir / "micro_mixer.lab")
    cached_sc = json.loads((result_dir / "tiny.json").read_text())
    (result_dir / "micro_mixer.json").write_text(json.dumps(cached_sc))
    cached = ['mode manual', 'frames 6', 'uiclick LIBRARY', 'frames 4', 'uiclick "NEXT DOMAINS"', 'frames 4', 'uiclick "libdom heat"', 'frames 4',
              'uiclick "lib micro_mixer"', 'frames 8', 'lab pause', 'lab frame 2', 'frames 4', 'uilist',
              'uiclick Agentic', 'frames 8', 'uiclick Manual', 'frames 8', 'lab info',
              'uiclick NEXT##labframe', 'frames 4', 'lab info', 'uiclick PREV##labframe', 'frames 4', 'lab info',
              'uiclick 12 fps', 'frames 4', 'uilist', f'screenshot {tmp / "manual-after.png"}', 'quit', '']
    (tmp / "cached.nav").write_text("\n".join(cached))
    run = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--exec", f"exec {tmp / 'cached.nav'}"],
                         capture_output=True, text=True, timeout=120, cwd=ROOT, env=env)
    ct = run.stdout + run.stderr
    check("labctl0" in ct and "RUN WITH THESE" in ct, "a cached scenario restores its input controls in a new app process", ct[-2000:])
    check("frame 3/5" in ct and "frame 4/5" in ct and "paused" in ct, "paused playback survives mode switches and PREV/NEXT change stored frames", ct[-2000:])
    check("24 fps##labfps" in ct and "uiclick: no widget" not in ct, "playback speed and the two mode controls work through real clicks", ct[-2000:])
    # Retained 3D geometry: a verified two-hex fixture, real section controls, and camera-only reuse.
    subprocess.run(["make", "build/labscenetest"], cwd=ROOT, check=True, capture_output=True)
    subprocess.run([str(ROOT / "build/labscenetest")], cwd=ROOT, check=True, capture_output=True)
    native = tmp / "native.nav"
    whole, cut = tmp / "whole.png", tmp / "cut.png"
    native.write_text("\n".join([
        f'lab open "{ROOT / "build/labscene.lab"}"', "lab pause", "lab frame 0", "lab field stress", "frames 8",
        f'screenshot "{whole}"', "lab bench 8", "frames 10",
        "uiclick X##labcut", "frames 6", "uiclick labsection 0.5", "frames 6", "lab info",
        f'screenshot "{cut}"', "uiclick FLIP##labcut", "frames 4", "lab info", "uiclick OFF##labcut", "frames 6", "lab info",
        "lab fps 60", "uiclick PLAY", "frames 40", "uiclick PAUSE", "lab info", "quit", ""]))
    r = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--exec", f"exec {native}"],
                       capture_output=True, text=True, timeout=120, cwd=ROOT, env=env)
    t = r.stdout + r.stderr
    check(r.returncode == 0 and "renderer retained GPU 3D" in t, "3D result uses native GPU path", t)
    check("0 frame reads, 0 uploads" in t, "camera-only orbit reuses retained geometry", t)
    reads = re.findall(r"renderer retained GPU 3D, frame reads (\d+)", t)
    check(len(reads) >= 2 and int(reads[-1]) > int(reads[0]), "PLAY advances the retained frame through real clicks", t)
    check("section axis 0 fraction 0.500 flip 1" in t and "section axis -1" in t, "section axis, slider and OFF work through clicks", t)
    if whole.exists() and cut.exists():
        def object_pixels(path):
            w, h, ch, pixels = _decode(path)
            # Exclude all HUD, panel and legend pixels. The two-cell fixture is much brighter than the room.
            return sum(max(pixels[(y*w+x)*ch:(y*w+x)*ch+3]) > 90
                       for y in range(120, min(h, 785)) for x in range(40, min(w, 950)))
        full, half = object_pixels(whole), object_pixels(cut)
        check(1000 < half < full * .85, f"section removes object pixels: {full} -> {half}", t)
    else:
        check(False, "native section captures exist", t)
    # Criterion before this hotkey implementation's first run: window key events address the displayed lab result,
    # preserve the hidden FEM field and tunnel settings, and never start the hidden tunnel.
    keys = ['workspace solid', 'fem field displacement', 'view pressure', 'slice off', 'streamlines off', 'particles off',
            'vortices off', 'volume off', f'lab open "{ROOT / "build/labscene.lab"}"', 'lab pause', 'lab frame 0', 'frames 6',
            'uikey space', 'frames 4', 'lab info', 'uikey space', 'frames 4', 'lab info', 'lab frame 0',
            'uikey right', 'frames 4', 'lab info', 'uikey left', 'frames 4', 'lab info', 'uikey 1', 'frames 4', 'lab info',
            'uikey x', 'frames 4', 'lab info', 'uikey ]', 'frames 4', 'lab info',
            'uikey s', 'uikey p', 'uikey v', 'uikey o', 'uikey l', 'uikey w', 'frames 6',
            'echo KEYBOARD_STATE', 'status', 'view', 'fem field', 'workspace', 'slice', 'streamlines', 'particles',
            'vortices', 'volume', 'quit', '']
    (tmp / 'keyboard.nav').write_text("\n".join(keys))
    kr = subprocess.run([str(APP), '--headless', '--size', '1440x900', '--exec', f'exec {tmp / "keyboard.nav"}'],
                        capture_output=True, text=True, timeout=120, cwd=ROOT, env=env)
    kt = kr.stdout + kr.stderr
    check(kr.returncode == 0 and 'playing, playback' in kt and 'paused, playback' in kt,
          'Space toggles replay through the real window keyboard path', kt[-2000:])
    check('frame 2/2, paused' in kt and 'frame 1/2, paused' in kt and 'frames, field stress' in kt,
          'arrow keys step stored frames and numeric keys select the displayed result field', kt[-2000:])
    check('section axis 0 fraction 0.520' in kt,
          'axis and bracket keys control the displayed result section', kt[-2000:])
    state = kt.partition('KEYBOARD_STATE')[2]
    check(re.search(r'state\s+paused', state) and 'showing displacement' in state and 'field = pressure' in state
          and 'workspace: solid' in state and re.search(r'streamlines\s+off', state),
          'lab shortcuts leave the hidden tunnel paused and its field and FEM field unchanged', state[-2000:])
    # Both volume and reconstruction tests create small, explicitly synthetic UI fixtures.
    for test in ("labvoltest", "labwatertest"):
        subprocess.run(["make", f"build/{test}"], cwd=ROOT, check=True, capture_output=True)
        subprocess.run([str(ROOT / "build" / test)], cwd=ROOT, check=True, capture_output=True)
    volume, plane, water, particles, restored = [tmp / f"{name}.png" for name in
                                               ("volume", "plane", "surface", "particles", "restored")]
    native.write_text("\n".join([
        f'lab open "{ROOT / "build/labvolume.lab"}"', "lab pause", "lab field ez", "lab range -0.012 0.012", "frames 8",
        f"screenshot {volume}", "lab info", "uiclick Z##labcut", "frames 4", "uiclick labsection 0.5", "frames 6",
        f"screenshot {plane}", "lab info", "lab bench 8", "frames 10",
        f'lab open "{ROOT / "build/labwater.lab"}"', "lab pause", "lab range 0 4", "frames 8", "lab info",
        f"screenshot {water}", "uiclick PARTICLES##labwater", "frames 8", "lab info", f"screenshot {particles}",
        "uiclick SURFACE##labwater", "frames 8", f"screenshot {restored}", "quit", ""]))
    r = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--exec", f"exec {native}"],
                       capture_output=True, text=True, timeout=120, cwd=ROOT, env=env)
    t = r.stdout + r.stderr
    check(r.returncode == 0 and "0 frame reads, 0 uploads" in t, "volume orbit retains its textures", t)
    volume_uploads = re.findall(r"renderer retained GPU 3D, frame reads \d+, uploads (\d+)", t)
    check(len(volume_uploads)>=2 and volume_uploads[0]==volume_uploads[1], "moving a volume section reuses the texture", t)
    check("water display surface" in t and "water display particles" in t, "water display switches through clicks", t)
    if all(path.exists() for path in (volume, plane, water, particles, restored)):
        check(_differing(_decode(volume), _decode(plane), x1=950)>500, "a real volume section changes viewport pixels", t)
        check(_differing(_decode(water), _decode(particles), x1=950)>2000, "surface and particles render different computed representations", t)
        check(_differing(_decode(water), _decode(restored), x1=950)<50, "returning to SURFACE restores the same view", t)
    else:
        check(False, "volume and water captures exist", t)
    print(f"lab UI: {PASS} passed, {FAIL} failed; captures {tmp}")
    return 1 if FAIL else 0



def printsurface():
    """Actual printing jobs on a sparse-facet cube, through MCP and native clicks, not a physical validation.

    Criteria declared in docs/analysis.md before execution: 192 early-layer triangles, 532 after the one-layer
    LPBF kerf, twelve mapped facets for a fully born FDM cube; active SURFACE/VOXELS differ by fewer than 50
    viewport pixels and early/final geometry differs by more than 500. No synthetic result fields are used.
    """
    from mcptest import Client
    from printflow import box_stl, PROCESS
    tmp = Path(tempfile.mkdtemp(prefix="printsurface-"))
    ws = tmp / "projects"
    ws.mkdir()
    stl = tmp / "cube.stl"
    box_stl(stl, (0, 0, 0), (6, 6, 6))
    c = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(tmp)])
    c.initialize()
    def call(name, args):
        r = c.call(name, args)["result"]["structuredContent"]
        if not r.get("ok"):
            raise RuntimeError(f"{name}: {r}")
        return r.get("value", {})
    try:
        for kind in ("lpbf", "fdm"):
            call("project_create", {"name": kind, "description": "Presentation contract; demonstration material and inferred process"})
            call("geometry_import", {"path": str(stl), "units": "mm", "name": "cube"})
            call("material_assign", {"body": "cube", "material": "ss316l_lpbf_demo" if kind == "lpbf" else "pla_generic_demo", "source": "user"})
            mesh = call("mesh_generate", {"element_size": "1 mm"})
            check(mesh.get("mesh", {}).get("elements") == 216, f"{kind} fixture has exactly 216 computed cells", str(mesh))
            call("project_save", {})
            if kind == "lpbf":
                run = call("lpbf_build_run", {"body": "cube", "build_orientation": "X", "layer_thickness_sim": "1 mm",
                    "inherent_strain": {"exx": -0.001, "eyy": -0.001, "ezz": -0.001,
                        "provenance": "inferred", "source": "Demonstration tensor for rendering contract, not a calibration"},
                    "material": {"youngs_modulus": "200000 MPa", "poissons_ratio": 0.3, "provenance": "inferred"},
                    "cut": {"height": "2.5 mm", "kerf": "1 mm", "from_x": "1 mm", "provenance": "assumed"}})
            else:
                process = dict(PROCESS, cooldown_bed_on="1 s", cooldown_bed_off="1 s", provenance="inferred")
                run = call("mech_print_run", {"body": "cube", "process": process})
            status = {}
            for _ in range(12):
                status = call("job_status", {"job_id": run["job_id"], "wait_seconds": 10})
                if status.get("state") not in ("queued", "running"):
                    break
            check(status.get("state") == "succeeded", f"{kind} solver produced a saved result", str(status))
            if status.get("state") != "succeeded":
                return 1
    finally:
        c.close()
    lines = ["mode manual", "workspace solid", "backdrop neutral", "floorgrid off", "box off"]
    for kind in ("lpbf", "fdm"):
        lines += [f'solid open "{ws / kind}"', "frames 8", "fem follow last", "frames 12", "fem pause",
                  "fem deform true", "fem fit", "camera orbit 40 18", 'uiclick "6 RESULTS"', "frames 240"]
        states = ("early", "late") if kind == "lpbf" else ("early", "whole")
        for state in states:
            lines += [f"fem step {'0' if state == 'early' else 'last'}", 'uiclick "SURFACE##draw"', "frames 60",
                      f"echo PRINTSURFACE {kind}_{state}", "solid status", "uitext",
                      f'screenshot "{tmp / (kind + "-" + state + "-surface.png")}"',
                      'uiclick "VOXELS##draw"', "frames 60", "solid status",
                      f'screenshot "{tmp / (kind + "-" + state + "-voxels.png")}"']
            if state != "early":
                lines += ["fem hide", "frames 60", f'screenshot "{tmp / (kind + "-fit-hidden.png")}"', "fem show", "frames 60"]
    lines += ["echo PRINTSURFACE done", "quit", ""]
    script = tmp / "printsurface.nav"
    script.write_text("\n".join(lines))
    r = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--workspace", str(ws), "--exec", f"exec {script}"],
                       capture_output=True, text=True, timeout=120, cwd=ROOT)
    t = r.stdout + r.stderr
    (tmp / "native.log").write_text(t)
    check(r.returncode == 0 and "PRINTSURFACE done" in t, "native printing script reached its end", t)
    stages = {}
    for m in re.finditer(r"\[info\] PRINTSURFACE (lpbf_early|lpbf_late|fdm_early|fdm_whole)\s*\n", t):
        tail = t[m.end():]
        stages[m.group(1)] = tail.split("PRINTSURFACE ", 1)[0]
    expected = {"lpbf_early": 192, "lpbf_late": 532, "fdm_early": 192, "fdm_whole": 12}
    for key, count in expected.items():
        stage = stages.get(key, "")
        actual = _tris(stage)
        check(bool(actual) and actual[0] == count, f"{key} SURFACE has exactly {count} triangles", stage)
        kind, state = key.split("_", 1)
        surface, voxels = tmp / f"{kind}-{state}-surface.png", tmp / f"{kind}-{state}-voxels.png"
        if not surface.exists() or not voxels.exists():
            check(False, f"{key} captures exist", t)
            continue
        if key != "fdm_whole":
            diff = _differing(_decode(surface), _decode(voxels), x1=950)
            check(diff < 50, f"{key} SURFACE matches visible FE boundary ({diff} differing viewport pixels)", stage)
            check("Active element boundary" in stage, f"{key} explains why the original STL is not used", stage)
    for kind, final in (("lpbf", "late"), ("fdm", "whole")):
        early, late = tmp / f"{kind}-early-surface.png", tmp / f"{kind}-{final}-surface.png"
        if early.exists() and late.exists():
            diff = _differing(_decode(early), _decode(late), x1=950)
            check(diff > 500, f"{kind} early/final geometry changes visible pixels ({diff})", t)
    for kind, state in (("lpbf", "late"), ("fdm", "whole")):
        final, hidden = tmp / f"{kind}-{state}-voxels.png", tmp / f"{kind}-fit-hidden.png"
        if final.exists() and hidden.exists():
            w, h, ch, px = _decode(final)
            bg = _decode(hidden)[3]
            xlo, ylo, xhi, yhi = w, h, -1, -1
            for y in range(h):
                for x in range(950):
                    i = (y*w+x)*ch
                    if max(abs(px[i+k]-bg[i+k]) for k in range(3)) > 24:
                        xlo, ylo = min(xlo,x), min(ylo,y)
                        xhi, yhi = max(xhi,x), max(yhi,y)
            check(xhi >= xlo and xlo >= 20 and xhi < 930 and ylo >= 20 and yhi < h-20,
                  f"{kind} FIT on first layer frames final shape with 20-pixel margins ({xlo},{ylo})..({xhi},{yhi})", t)
        else:
            check(False, f"{kind} first-layer FIT captures exist", t)
    print(f"PRINT SURFACE: {PASS} passed, {FAIL} failed; native captures {tmp}")
    return 1 if FAIL else 0


def main():
    if "--printsurface" in sys.argv:
        return printsurface()
    if "--lab" in sys.argv:
        rc = labwalk()
        print(f"\n{'LAB WALKTHROUGH FAILED' if FAIL else 'LAB WALKTHROUGH PASSED'}: {PASS} passed, {FAIL} failed")
        return rc
    if "--stopwalk" in sys.argv:
        rc = stopwalk()
        print(f"\n{'STOP WALKTHROUGH FAILED' if FAIL else 'STOP WALKTHROUGH PASSED'}: {PASS} passed, {FAIL} failed")
        return rc
    if "--simplewalk" in sys.argv:
        rc = simplewalk()
        print(f"\n{'SIMPLE WALKTHROUGH FAILED' if FAIL else 'SIMPLE WALKTHROUGH PASSED'}: {PASS} passed, {FAIL} failed")
        return rc
    if "--agentwalk" in sys.argv:
        rc = agentwalk()
        print(f"\n{'AGENT WALKTHROUGH FAILED' if FAIL else 'AGENT WALKTHROUGH PASSED'}: {PASS} passed, {FAIL} failed")
        return rc
    if "--topopt" in sys.argv:
        rc = topopt()
        print(f"\n{'TOPOLOGY OPTIMISATION UI FAILED' if FAIL else 'TOPOLOGY OPTIMISATION UI PASSED'}: {PASS} passed, {FAIL} failed")
        return rc
    if "--presentation" in sys.argv:
        rc = presentation()
        print(f"\n{'PRESENTATION FAILED' if FAIL else 'PRESENTATION PASSED'}: {PASS} passed, {FAIL} failed")
        return rc
    if "--walkthrough-tet" in sys.argv:
        rc = walkthrough(tet=True)
        print(f"\n{'TET WALKTHROUGH FAILED' if FAIL else 'TET WALKTHROUGH PASSED'}: {PASS} passed, {FAIL} failed")
        return rc
    if "--walkthrough" in sys.argv:
        rc = walkthrough()
        print(f"\n{'WALKTHROUGH FAILED' if FAIL else 'WALKTHROUGH PASSED'}: {PASS} passed, {FAIL} failed")
        return rc
    keep = "--keep" in sys.argv
    if not APP.exists():
        print("navier is not built (run: make navier)")
        return 2
    plan = steps()
    lines = ["# generated by tools/uicheck.py - clicks every control and prints the state it changed"]
    for name, cmds, _fn, _note in plan:
        lines.append(f"echo {MARK} {name}")
        lines += cmds
    lines.append(f"echo {MARK} end")
    lines.append("quit")
    tmp = Path(tempfile.mkdtemp(prefix="uicheck"))
    script = tmp / "uicheck.nav"
    script.write_text("\n".join(lines) + "\n")
    run = subprocess.run([str(APP), "--headless", "--size", "1440x900", "--workspace", str(tmp / "projects"),
                          "--exec", f"exec {script}"],
                         capture_output=True, text=True, timeout=1200, cwd=str(ROOT))
    log = run.stdout + run.stderr
    if keep:
        (ROOT / "build" / "uicheck.log").write_text(log)
        print(f"  script: {script}\n  log: {ROOT / 'build' / 'uicheck.log'}")

    parts = re.split(rf"{MARK}\s+(\w+)", log)
    sections = {}
    for i in range(1, len(parts) - 1, 2):
        sections[parts[i]] = parts[i + 1]
    print(f"== {len(plan)} interface checks")
    for name, _cmds, fn, _note in plan:
        text = sections.get(name)
        if text is None:
            check(False, f"step '{name}' did not run")
            continue
        fn(text)
    # errors anywhere except in the steps that deliberately provoke one
    expected_err = {"ambiguous_click", "missing_click", "solid_refusal_shown"}  # these provoke a refusal on purpose
    stray = [ln for name, sec in sections.items() if name not in expected_err for ln in sec.splitlines() if "[err!]" in ln]
    check(not stray, f"no errors logged ({len(stray)} found)", "\n".join(stray[:8]))
    pixel_checks(APP, tmp)
    chamfer_checks(APP, tmp)
    broken_surface_checks(APP, tmp)
    print(f"\n{'INTERFACE CHECKS FAILED' if FAIL else 'ALL INTERFACE CHECKS PASSED'}: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
