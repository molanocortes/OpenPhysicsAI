"""topofilm.py: a generative-design run filmed in Blender's Cycles (tools/render/README.md, "Generative design").

    blender -b --factory-startup -noaudio -P tools/render/topofilm.py -- PLYDIR OUTDIR [options]

PLYDIR is what `build/gendesign ply` wrote: manifest.json, it_NNNN.ply (the isosurface at density 0.5 of every kept
iteration of the optimiser), final_vm.ply (the final design with the von Mises stress the engine computed for it). Every
vertex comes from the optimiser's density field, mirrored about y = 0 when half the bracket was computed; materials,
light, the studio, the camera and the arrow that marks the load are presentation, chosen here.

The film, shot by shot (OUTDIR/shots.json lists every frame: its shot, the iteration it shows, the turn of the part):
    brief    the design space at the start, solid titanium, with the load drawn as an arrow   [--brief 36 frames]
    carve    the iterations: every kept iteration up to --dense-to, then every --sparse-th      [one frame each]
    turn     the final design turning on the turntable                                          [--turn 120 frames]
    stress   the final design coloured by its von Mises stress, still turning                   [--stress 72 frames]
The part turns about the vertical axis at a speed that rises smoothly from shot to shot (--deg BRIEF CARVE TURN STRESS,
degrees per second), the camera stands still in the room and moves in a little; the lights stay with the room.

Options (defaults in brackets):
    --frames A B        render the film's frames A to B [all]; --every K [1]; --still K one frame as still_KKKK.png
    --size WxH [1920x1080]; --percent P [100]; --samples N [24]; --noise T [0.05]; --min-samples N [8]
    --threads N [6]; --denoise atrous|none [atrous]; --fps F [24]
    --azim A [-58] camera azimuth in degrees; --elev E0 E1 [25 19]; --dist D0 D1 [6.4 5.6] in object radii
    --lens MM [70]; --fstop F [3.5] (0: no depth of field); --exposure EV [0]
    --look titanium|printed [titanium]: bead-blasted titanium, or the grainier surface of a part as printed
    --dense-to K [the iteration where the optimiser reached its target volume, plus 20]; --sparse K [2]
    --story K           every K frames a 960 px picture and the progress to ~/bin/story (the owner's phone)
    --preview N         N frames spread over the film (one in each shot at least), as preview_FFFF.png
    --run RUNDIR        build a surface that PLYDIR lacks from the run's densities just before its first frame
                        (`build/gendesign ply --only`, checked closed), so that PLYDIR may hold the manifest alone;
                        --prune deletes each surface after its last frame (the node's disk is the tight resource);
                        --wait waits for a density the run has not written (or sent) yet, so that the first frames can
                        be rendered while the optimisation goes on
    --camera-frames N   the camera's move spans N frames [the film's length]: give the expected length when frames are
                        rendered before the film's length is known, so that they keep their camera
Frames already in OUTDIR are skipped, so a stopped film resumes.
"""
import json
import math
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import beauty  # noqa: E402  the studio, the lights, the PLY reader and the denoiser of the lab's beauty renders

import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Vector  # noqa: E402


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    o = dict(frames=None, every=1, still=None, size=(1920, 1080), percent=100, samples=24, noise=0.05, min_samples=8,
             threads=6, denoise="atrous", fps=24, azim=-58.0, elev=(25.0, 19.0), dist=(6.4, 5.6), lens=70.0, fstop=3.5,
             exposure=0.0, look="titanium", brief=36, turn=120, stress=72, dense_to=None, sparse=2,
             deg=(4.0, 7.0, 30.0, 30.0), story=0, arrow=True, preview=0, run=None, gendesign="build/gendesign",
             prune=False, camera_frames=0, wait=False)
    pos = []
    i = 0
    while i < len(argv):
        a = argv[i]

        def nums(k):
            return [float(x) for x in argv[i + 1:i + 1 + k]]
        if a == "--frames": o["frames"] = tuple(int(x) for x in argv[i + 1:i + 3]); i += 2
        elif a == "--every": o["every"] = int(argv[i + 1]); i += 1
        elif a == "--still": o["still"] = int(argv[i + 1]); i += 1
        elif a == "--size": o["size"] = tuple(int(x) for x in argv[i + 1].lower().split("x")); i += 1
        elif a == "--percent": o["percent"] = int(argv[i + 1]); i += 1
        elif a == "--samples": o["samples"] = int(argv[i + 1]); i += 1
        elif a == "--noise": o["noise"] = float(argv[i + 1]); i += 1
        elif a == "--min-samples": o["min_samples"] = int(argv[i + 1]); i += 1
        elif a == "--threads": o["threads"] = int(argv[i + 1]); i += 1
        elif a == "--denoise": o["denoise"] = argv[i + 1]; i += 1
        elif a == "--fps": o["fps"] = int(argv[i + 1]); i += 1
        elif a == "--azim": o["azim"] = float(argv[i + 1]); i += 1
        elif a == "--elev": o["elev"] = tuple(nums(2)); i += 2
        elif a == "--dist": o["dist"] = tuple(nums(2)); i += 2
        elif a == "--lens": o["lens"] = float(argv[i + 1]); i += 1
        elif a == "--fstop": o["fstop"] = float(argv[i + 1]); i += 1
        elif a == "--exposure": o["exposure"] = float(argv[i + 1]); i += 1
        elif a == "--look": o["look"] = argv[i + 1]; i += 1
        elif a == "--brief": o["brief"] = int(argv[i + 1]); i += 1
        elif a == "--turn": o["turn"] = int(argv[i + 1]); i += 1
        elif a == "--stress": o["stress"] = int(argv[i + 1]); i += 1
        elif a == "--dense-to": o["dense_to"] = int(argv[i + 1]); i += 1
        elif a == "--sparse": o["sparse"] = int(argv[i + 1]); i += 1
        elif a == "--deg": o["deg"] = tuple(nums(4)); i += 4
        elif a == "--story": o["story"] = int(argv[i + 1]); i += 1
        elif a == "--no-arrow": o["arrow"] = False
        elif a == "--preview": o["preview"] = int(argv[i + 1]); i += 1
        elif a == "--run": o["run"] = argv[i + 1]; i += 1
        elif a == "--gendesign": o["gendesign"] = argv[i + 1]; i += 1
        elif a == "--prune": o["prune"] = True
        elif a == "--camera-frames": o["camera_frames"] = int(argv[i + 1]); i += 1
        elif a == "--wait": o["wait"] = True
        elif a.startswith("--"): sys.exit("topofilm.py: unknown option %s" % a)
        else: pos.append(a)
        i += 1
    if len(pos) != 2:
        sys.exit("usage: blender -b -P tools/render/topofilm.py -- PLYDIR OUTDIR [options] (see the file's head)")
    o["plydir"], o["out"] = pos
    return o


# ---- the shot list ---------------------------------------------------------------------------------------------------

def smooth_angles(speeds, fps, ramp):
    """the part's turn at every frame from a speed per frame (degrees per second), the speed blended between shots by
    a moving average over `ramp` frames so that it never jumps, and eased in from rest at the start"""
    v = np.asarray(speeds, float)
    k = max(1, int(ramp))
    pad = np.concatenate([np.full(k, v[0]), v, np.full(k, v[-1])])
    ker = np.hanning(2 * k + 1)
    ker /= ker.sum()
    vs = np.convolve(pad, ker, mode="same")[k:-k]
    start = np.clip(np.arange(len(v)) / float(k), 0, 1)
    vs *= start * start * (3 - 2 * start)
    return np.concatenate([[0.0], np.cumsum(vs[:-1] / fps)])


def plan(o, man):
    its = sorted((s for s in man["surfaces"] if s["iteration"] >= 0), key=lambda s: s["iteration"])
    if not its:
        sys.exit("topofilm.py: no iterations in %s" % o["plydir"])
    last = its[-1]["iteration"]
    reached = man.get("target_reached_at") or 0
    dense_to = o["dense_to"] if o["dense_to"] is not None else (reached + 20 if reached else last)
    shown = [s for s in its if s["iteration"] <= dense_to or (s["iteration"] - dense_to) % o["sparse"] == 0]  # as `gendesign ply`
    if shown[-1]["iteration"] != last:
        shown.append(its[-1])
    final = next((s for s in man["surfaces"] if s["name"] == "final_vm"), its[-1])
    frames = []
    for _ in range(o["brief"]):
        frames.append(dict(shot="brief", surface=its[0]["name"], iteration=its[0]["iteration"], stress=0.0))
    for s in shown[1:]:
        frames.append(dict(shot="carve", surface=s["name"], iteration=s["iteration"], stress=0.0))
    for _ in range(o["turn"]):
        frames.append(dict(shot="turn", surface=final["name"], iteration=last, stress=0.0))
    for j in range(o["stress"]):
        u = min(1.0, j / max(1.0, 0.3 * o["stress"]))
        frames.append(dict(shot="stress", surface=final["name"], iteration=last, stress=u * u * (3 - 2 * u)))
    deg = dict(zip(("brief", "carve", "turn", "stress"), o["deg"]))
    turn = smooth_angles([deg[f["shot"]] for f in frames], o["fps"], ramp=0.8 * o["fps"])
    n = len(frames)
    # the arrow that marks the load: there in the brief, gone over the first second of the carving
    span = o["camera_frames"] if o["camera_frames"] > 0 else n
    for j, f in enumerate(frames):
        f["frame"] = j
        f["turn_deg"] = float(turn[j])
        f["t"] = min(1.0, j / max(1, span - 1))
        k = j - o["brief"]
        f["arrow"] = 1.0 if k < 0 else max(0.0, 1.0 - k / float(o["fps"]))
    return frames


# ---- materials --------------------------------------------------------------------------------------------------------

def titanium_material(look):
    """Ti-6Al-4V, bead blasted (or as printed): a grey metal a little warm, its roughness broken by a fine noise that
    rides with the part; the stress colours of the engine's von Mises field mixed in by the node 'stress' (0..1)"""
    m = bpy.data.materials.new("titanium")
    m.use_nodes = True
    if hasattr(m, "cycles") and hasattr(m.cycles, "emission_sampling"):
        m.cycles.emission_sampling = "NONE"
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    printed = look == "printed"
    base = (0.40, 0.38, 0.355) if not printed else (0.36, 0.345, 0.33)   # linear; titanium's grey, a little warm
    rough = 0.40 if not printed else 0.52
    ti = nt.nodes.new("ShaderNodeBsdfPrincipled")
    beauty.set_input(ti, "Base Color", base + (1.0,))
    beauty.set_input(ti, "Metallic", 1.0)
    coord = nt.nodes.new("ShaderNodeTexCoord")
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 90.0 if not printed else 260.0
    noise.inputs["Detail"].default_value = 4.0
    nt.links.new(coord.outputs["Object"], noise.inputs["Vector"])
    rr = nt.nodes.new("ShaderNodeMapRange")
    rr.inputs["To Min"].default_value, rr.inputs["To Max"].default_value = rough * 0.82, rough * 1.18
    nt.links.new(noise.outputs["Fac"], rr.inputs["Value"])
    nt.links.new(rr.outputs["Result"], beauty.inp(ti, "Roughness"))
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.05 if not printed else 0.10
    bump.inputs["Distance"].default_value = 0.0004
    nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], beauty.inp(ti, "Normal"))
    # the stress look: the ember ramp of the beauty renders on the field mapped to 0..1, a tinted, less mirror-like metal
    st = nt.nodes.new("ShaderNodeBsdfPrincipled")
    attr = nt.nodes.new("ShaderNodeAttribute")
    attr.attribute_type, attr.attribute_name = "GEOMETRY", "value01"
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    els = ramp.color_ramp.elements
    stops = [(0.0, (0.40, 0.46, 0.56)), (0.14, (0.62, 0.66, 0.70)), (0.32, (0.95, 0.74, 0.30)), (0.55, (0.97, 0.45, 0.10)),
             (0.80, (0.82, 0.13, 0.05)), (1.0, (0.50, 0.03, 0.05))]
    els[0].position, els[0].color = stops[0][0], beauty.srgb_to_linear(stops[0][1]) + (1.0,)
    els[1].position, els[1].color = stops[-1][0], beauty.srgb_to_linear(stops[-1][1]) + (1.0,)
    for p, c in stops[1:-1]:
        e = els.new(p)
        e.color = beauty.srgb_to_linear(c) + (1.0,)
    nt.links.new(attr.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], beauty.inp(st, "Base Color"))
    beauty.set_input(st, "Metallic", 0.35)
    beauty.set_input(st, "Roughness", 0.38)
    nt.links.new(bump.outputs["Normal"], beauty.inp(st, "Normal"))
    mix = nt.nodes.new("ShaderNodeMixShader")
    mix.name = "stress"
    mix.inputs[0].default_value = 0.0
    nt.links.new(ti.outputs["BSDF"], mix.inputs[1])
    nt.links.new(st.outputs["BSDF"], mix.inputs[2])
    nt.links.new(mix.outputs["Shader"], out.inputs["Surface"])
    return m


def arrow_material():
    """the load's arrow: a flat signal red that glows a little, faded out by the node 'fade' (1 shown, 0 gone)"""
    m = bpy.data.materials.new("arrow")
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    red = beauty.srgb_to_linear((0.86, 0.16, 0.10))
    beauty.set_input(b, "Base Color", red + (1.0,))
    beauty.set_input(b, "Roughness", 0.45)
    beauty.set_input(b, ["Emission Color", "Emission"], red + (1.0,))
    beauty.set_input(b, "Emission Strength", 0.6)
    tr = nt.nodes.new("ShaderNodeBsdfTransparent")
    mix = nt.nodes.new("ShaderNodeMixShader")
    mix.name = "fade"
    mix.inputs[0].default_value = 1.0
    nt.links.new(tr.outputs["BSDF"], mix.inputs[1])
    nt.links.new(b.outputs["BSDF"], mix.inputs[2])
    nt.links.new(mix.outputs["Shader"], out.inputs["Surface"])
    if hasattr(m, "blend_method"):
        m.blend_method = "HASHED"
    return m


# ---- the film ---------------------------------------------------------------------------------------------------------

class TopoFilm:
    def __init__(self, o):
        self.o = o
        with open(os.path.join(o["plydir"], "manifest.json")) as f:
            self.man = json.load(f)
        lo, hi = np.array(self.man["bounds"][0]), np.array(self.man["bounds"][1])
        self.center = 0.5 * (lo + hi)
        self.center[2] = lo[2]                           # the part stands on the floor of the studio, turning in place
        self.radius = 0.5 * float(np.linalg.norm(hi - lo))
        self.scale = 1.0 / self.radius                   # the scene in object radii, as in beauty.py
        self.height = (hi[2] - lo[2]) * self.scale
        self.part = None
        self.loaded = None

    def place(self, xyz):
        return (xyz - self.center) * self.scale

    def build(self):
        o, sc = self.o, bpy.context.scene
        for ob in list(bpy.data.objects):
            bpy.data.objects.remove(ob, do_unlink=True)
        sc.render.engine = "CYCLES"
        cy = sc.cycles
        cy.device = "CPU"
        cy.samples, cy.use_adaptive_sampling = o["samples"], True
        cy.adaptive_threshold, cy.adaptive_min_samples = o["noise"], o["min_samples"]
        cy.use_denoising = False
        cy.max_bounces, cy.diffuse_bounces, cy.glossy_bounces = 6, 2, 4
        cy.transmission_bounces, cy.transparent_max_bounces, cy.volume_bounces = 0, 4, 0
        cy.use_fast_gi, cy.ao_bounces_render = True, 2
        if hasattr(cy, "fast_gi_method"):
            cy.fast_gi_method = "REPLACE"
        cy.caustics_reflective = cy.caustics_refractive = False
        cy.sample_clamp_direct, cy.sample_clamp_indirect, cy.blur_glossy = 10.0, 3.0, 0.8
        cy.use_light_tree = True
        sc.render.threads_mode, sc.render.threads = "FIXED", o["threads"]
        sc.render.use_persistent_data = True
        sc.render.resolution_x, sc.render.resolution_y = o["size"]
        sc.render.resolution_percentage = o["percent"]
        sc.render.film_transparent = False
        sc.render.image_settings.file_format, sc.render.image_settings.color_mode = "PNG", "RGB"
        sc.render.image_settings.color_depth, sc.render.image_settings.compression = "8", 15
        sc.view_settings.view_transform = "AgX"
        try:
            sc.view_settings.look = "AgX - Medium High Contrast"
        except TypeError:
            pass
        sc.view_settings.exposure = o["exposure"]
        w = bpy.data.worlds.new("studio sky")
        sc.world = w
        w.light_settings.ao_factor, w.light_settings.distance = 1.0, 0.5
        w.use_nodes = True
        bg = w.node_tree.nodes["Background"]
        bg.inputs["Color"].default_value = (0.80, 0.82, 0.85, 1)
        bg.inputs["Strength"].default_value = 0.30
        paper = beauty.plain_material("seamless", (0.55, 0.57, 0.61), 0.88)
        beauty.cyclorama("studio", -0.0015, 14.0, 5.0, 16.0, paper)
        # the lights, fixed in the room around the camera's side: a large key softbox high on the left, a fill low on
        # the right, two strip rims behind for the edges of the members, a softbox overhead, a band behind to mirror
        az = o["azim"]
        tgt = Vector((0.0, 0.0, 0.42 * self.height))
        k = 0.36
        beauty.area_light("key", beauty.sph(az - 50, 50, 6.0) + tgt, tgt, 4.0, 3.0, 640 * k, spread=80)
        beauty.area_light("fill", beauty.sph(az + 75, 14, 7.0) + tgt, tgt, 5.0, 3.5, 110 * k, spread=90)
        beauty.area_light("rim left", beauty.sph(az + 145, 16, 6.0) + tgt, tgt, 0.8, 4.0, 520 * k, spread=70)
        beauty.area_light("rim right", beauty.sph(az - 145, 16, 6.0) + tgt, tgt, 0.8, 4.0, 420 * k, spread=70)
        beauty.area_light("top", Vector((0, 0, 7.0)), tgt, 7.0, 7.0, 200 * k, spread=90)
        beauty.area_light("back", beauty.sph(az + 180, 32, 6.5) + tgt, tgt, 7.0, 1.6, 100 * k, spread=60)
        cam = bpy.data.cameras.new("camera")
        cam.lens, cam.sensor_width = o["lens"], 36.0
        cam.clip_start, cam.clip_end = 0.05, 200.0
        cam.dof.use_dof = o["fstop"] > 0
        cam.dof.aperture_fstop = max(o["fstop"], 0.01)
        cam.dof.aperture_blades = 7
        self.cam = bpy.data.objects.new("camera", cam)
        sc.collection.objects.link(self.cam)
        sc.camera = self.cam
        self.mat = titanium_material(o["look"])
        self.arrow = self.make_arrow() if o["arrow"] else None

    def make_arrow(self):
        """the load through the clevis pin, drawn as an arrow along its line from just outside the lug: presentation,
        placed from the case's pin and angle (tools/gendesign.c)"""
        case = self.man.get("case", {})
        pin = np.array([0.0, 0.0, case.get("pin_z_m", 0.054)])
        a = math.radians(case.get("angle_deg", 45.0))
        d = np.array([math.sin(a), 0.0, math.cos(a)])
        start = self.place(pin + d * case.get("arrow_start_m", 0.024))
        length = 0.034 * self.scale
        r = 0.0034 * self.scale
        mat = arrow_material()
        bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=r, depth=length * 0.72)
        shaft = bpy.context.active_object
        bpy.ops.mesh.primitive_cone_add(vertices=32, radius1=2.4 * r, radius2=0.0, depth=length * 0.28)
        head = bpy.context.active_object
        for ob, off in ((shaft, 0.36 * length), (head, 0.72 * length + 0.14 * length)):
            ob.data.materials.append(mat)
            ob.rotation_mode = "QUATERNION"
            ob.rotation_quaternion = Vector(tuple(d)).to_track_quat("Z", "Y")
            ob.location = Vector(tuple(start + d * off))
            for p in ob.data.polygons:
                p.use_smooth = True
        holder = bpy.data.objects.new("load", None)
        bpy.context.scene.collection.objects.link(holder)
        for ob in (shaft, head):
            ob.parent = holder
        self.arrow_mat = mat
        return holder

    def ensure(self, name):
        """the surface's PLY, built from the run's densities when it is missing (and checked: closed, oriented)"""
        path = os.path.join(self.o["plydir"], name + ".ply")
        if os.path.exists(path) or not self.o["run"]:
            return path
        if self.o["wait"]:  # the run's density for this surface, which may still be on its way
            need = os.path.join(self.o["run"], "final_vm.f32" if name == "final_vm" else
                                "snap/%s.f32" % name if name.startswith("it_") else "cells.u8")
            t0 = time.time()
            while not os.path.exists(need):
                if int(time.time() - t0) % 600 < 60:
                    print("topofilm.py: waiting for %s (%.0f min)" % (need, (time.time() - t0) / 60), flush=True)
                time.sleep(60)
        taubin = str(self.man.get("taubin", 12))
        cmd = [self.o["gendesign"], "ply", self.o["run"], self.o["plydir"], "--only", name, "--taubin", taubin]
        if name in ("domain", "held"):
            cmd += ["--domain", "--to", "-1"]
        elif name == "final_vm":
            cmd += ["--final", "--to", "-1"]
        else:
            it = int(name.split("_")[1])
            cmd += ["--from", str(it), "--to", str(it)]
        r = subprocess.run(["nice", "-n", "19"] + cmd, capture_output=True, text=True)
        line = next((l for l in r.stdout.splitlines() if l.startswith(name + ":")), "")
        if r.returncode or " 0 open, 0 non-manifold, 0 misoriented" not in line:
            sys.exit("topofilm.py: the surface %s is not sound or was not built: %s %s" % (name, line, r.stderr[-500:]))
        print("topofilm.py: built %s" % line, flush=True)
        return path

    def load(self, name):
        if self.loaded == name:
            return
        v, faces = beauty.read_ply(self.ensure(name))
        me = bpy.data.meshes.new(name)
        nv, nf = len(v), len(faces)
        xyz = np.stack([v["x"], v["y"], v["z"]], 1).astype(np.float64)
        me.vertices.add(nv)
        me.vertices.foreach_set("co", self.place(xyz).astype(np.float32).ravel())
        me.loops.add(3 * nf)
        me.polygons.add(nf)
        me.loops.foreach_set("vertex_index", faces.astype(np.int32).ravel())
        me.polygons.foreach_set("loop_start", np.arange(0, 3 * nf, 3, dtype=np.int32))
        try:
            me.polygons.foreach_set("loop_total", np.full(nf, 3, np.int32))
        except (AttributeError, TypeError, RuntimeError):
            pass
        me.polygons.foreach_set("use_smooth", np.ones(nf, bool))
        me.update(calc_edges=True)
        for a in ("value01", "value"):
            at = me.attributes.new(a, "FLOAT", "POINT")
            at.data.foreach_set("value", np.ascontiguousarray(v[a], np.float32))
        n = np.stack([v["nx"], v["ny"], v["nz"]], 1).astype(np.float32)
        try:
            if hasattr(me, "use_auto_smooth"):
                me.use_auto_smooth = True
            me.normals_split_custom_set_from_vertices(n)
        except Exception as ex:
            print("topofilm.py: custom normals not set (%s)" % ex)
        me.materials.append(self.mat)
        if self.part is None:
            self.part = bpy.data.objects.new("bracket", me)
            bpy.context.scene.collection.objects.link(self.part)
            self.part.rotation_mode = "XYZ"
        else:
            old = self.part.data
            self.part.data = me
            bpy.data.meshes.remove(old)
        self.loaded = name

    def set_frame(self, f):
        o = self.o
        t = f["t"]
        e = t * t * (3 - 2 * t)
        el = o["elev"][0] + (o["elev"][1] - o["elev"][0]) * e
        dist = o["dist"][0] + (o["dist"][1] - o["dist"][0]) * e
        tgt = Vector((0.0, 0.0, 0.36 * self.height))
        pos = tgt + beauty.sph(o["azim"], el, dist)
        self.cam.location = pos
        self.cam.rotation_mode = "QUATERNION"
        self.cam.rotation_quaternion = (tgt - pos).to_track_quat("-Z", "Y")
        self.cam.data.dof.focus_distance = (tgt - pos).length
        rot = math.radians(f["turn_deg"])
        self.part.rotation_euler = (0.0, 0.0, rot)
        self.mat.node_tree.nodes["stress"].inputs[0].default_value = f["stress"]
        if self.arrow is not None:
            self.arrow.rotation_euler = (0.0, 0.0, rot)
            self.arrow_mat.node_tree.nodes["fade"].inputs[0].default_value = f["arrow"]
            for ob in self.arrow.children:
                ob.hide_render = f["arrow"] <= 0.0


def main():
    o = parse_args()
    os.makedirs(o["out"], exist_ok=True)
    film = TopoFilm(o)
    frames = plan(o, film.man)
    with open(os.path.join(o["out"], "shots.json"), "w") as fh:
        json.dump(dict(fps=o["fps"], frames=frames), fh, indent=0)
    film.build()
    n = len(frames)
    a, b = o["frames"] if o["frames"] else (0, n - 1)
    todo = [o["still"]] if o["still"] is not None else list(range(a, min(b, n - 1) + 1, o["every"]))
    if o["preview"] > 0:  # evenly over the film, and the middle of every shot
        picks = {int(round(k * (n - 1) / max(1, o["preview"] - 1))) for k in range(o["preview"])}
        for shot in ("brief", "carve", "turn", "stress"):
            idx = [f["frame"] for f in frames if f["shot"] == shot]
            if idx:
                picks.add(idx[len(idx) // 2])
        todo = sorted(picks)
    sc = bpy.context.scene
    tmp = os.path.join(o["out"], ".passes")
    if o["denoise"] == "atrous":
        os.makedirs(tmp, exist_ok=True)
        beauty.passes_setup(tmp)
    print("topofilm.py: %d frames in the film (%d brief, %d carving, %d turning, %d with the stress); rendering %d" % (
        n, o["brief"], n - o["brief"] - o["turn"] - o["stress"], o["turn"], o["stress"], len(todo)), flush=True)
    t_all = time.time()
    done = 0
    for j in todo:
        name = ("still_%04d.png" if o["still"] is not None else "preview_%04d.png" if o["preview"] else "frame_%04d.png") % j
        final = os.path.join(o["out"], name)
        if os.path.exists(final) and o["still"] is None and not o["preview"]:
            continue
        t0 = time.time()
        f = frames[j]
        film.load(f["surface"])
        film.set_frame(f)
        if o["denoise"] == "atrous":
            td = beauty.render_atrous(final, tmp, j + 1)
        else:
            sc.render.filepath = final
            bpy.ops.render.render(write_still=True)
            td = 0.0
        done += 1
        print("topofilm.py: %s, %s, iteration %d, turn %.1f deg, %.1f s (denoise %.1f s)" % (
            name, f["shot"], f["iteration"], f["turn_deg"], time.time() - t0, td), flush=True)
        if o["prune"] and o["run"]:  # the surface is not needed by any later frame of this pass: delete it
            later = {frames[k]["surface"] for k in todo if k > j}
            if f["surface"] not in later:
                try:
                    os.remove(os.path.join(o["plydir"], f["surface"] + ".ply"))
                except OSError:
                    pass
        if o["story"] and (done == 1 or done % o["story"] == 0):
            small = os.path.join(o["out"], "story.png")
            beauty.small_copy(final, small)
            beauty.story(["frame", small, "--caption", "film frame %d of %d (%s, iteration %d)" % (j + 1, n, f["shot"], f["iteration"])])
            beauty.story(["progress", str(j + 1), str(n), "--unit", "frames"])
    print("topofilm.py: %d frames in %.0f s" % (done, time.time() - t_all))


if __name__ == "__main__":
    main()
