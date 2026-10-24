"""printfilm.py: the metal printing film, a part growing layer by layer on its build plate and cut from it, filmed in
Blender's Cycles (tools/render/README.md, "The metal printing film").

    blender -b --factory-startup -noaudio -P tools/render/printfilm.py -- PLYDIR OUTDIR [options]

PLYDIR is what tools/render/nvt2ply.c wrote from a build's results.nvt: manifest.json and state_NNNN/GROUP.ply. Every
vertex is where the build put its node (the displacement multiplied by the factor the manifest states) and every colour
is the computed von Mises stress at that node through one fixed scale. The plate, the studio, the light, the materials,
the camera, the timing and the turntable are presentation, and so is every frame between the stored state before the
cut and the one after (the exporter's blends). Nothing here suggests a temperature: the build computes none, so the
stress scale is a cool one (a warm scale on a fresh layer reads as heat) and nothing glows.

The film, in frames at --fps:
    intro       the empty plate                                                      --intro [18]
    build       every stored layer, each held --layer-frames [8]
    hold        the finished part                                                    --hold [24]
    cut         the stored states after the build (the cut, the supports' removal), each reached from the one before
                through the exporter's blends over --cut-frames [60], eased, then held --hold
    turntable   the final part turning --turn-deg [360] over --turn-frames [240], eased
Options (defaults in brackets):
    --frames A B / --every K / --list I,J,K   which film frames to render [all]; --still K one, as still_KKKK.png
    --size WxH [1920x1080]; --percent P [100]; --samples N [24]; --noise T [0.05]; --min-samples N [12]; --threads N [4]
    --plate MX MY T     the plate's margin around the part in x and y and its thickness, mm [6 13 12]
    --azim A0 A1 [-74 -50] --elev E0 E1 [22 17] --dist D0 D1 [3.9 3.6]   the camera over intro, build and cut (degrees;
                        object radii); it then holds while the part turns
    --lens MM [70]; --fstop F [2.0], 0 for none; --target X Y Z [0 0 0.03] (radii, from the part's centre)
    --exposure EV [0]; --light X [1]; --bounces M D G [3 1 2]; --fast-gi N [1] (ambient occlusion after N bounces)
    --story K           every K frames a picture and the progress to ~/bin/story (the owner's phone)
    --blend PATH        also save the scene as a .blend file
Frames already in OUTDIR are kept, so a stopped film resumes. OUTDIR/timeline.json lists every film frame: its state,
phase and turntable angle, for the captions the encoder times.
"""
import json
import math
import os
import sys
import time

import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import beauty as B  # noqa: E402  the studio, the lights, the PLY reader and the denoiser of the first films

# The stress scale: the part's own metal where the stress is low, then a cool sequence (steel blue, blue, indigo,
# violet), lightness falling as the stress rises, the metal staying metal. No red, orange or yellow: on a fresh layer
# they would read as heat.
RAMP = [(0.00, (0.46, 0.47, 0.49)), (0.10, (0.46, 0.47, 0.49)), (0.35, (0.40, 0.52, 0.63)),
        (0.55, (0.22, 0.40, 0.69)), (0.78, (0.25, 0.20, 0.60)), (1.00, (0.46, 0.10, 0.48))]
RAMP_START = 0.10



def render_device(cy):
    """CPU unless OPENPHYSICS_RENDER_DEVICE names a GPU backend (CUDA, OPTIX, HIP, METAL, ONEAPI): then every device of
    that backend renders. Same scene, samples and denoise either way; only where Cycles runs changes."""
    import os
    kind = os.environ.get("OPENPHYSICS_RENDER_DEVICE", "CPU").upper()
    cy.device = "CPU"
    if kind == "CPU":
        return
    p = bpy.context.preferences.addons["cycles"].preferences
    p.compute_device_type = kind
    p.get_devices()
    gpus = [d for d in p.devices if d.type == kind]
    if not gpus:
        raise SystemExit("no %s device for Cycles here" % kind)
    for d in p.devices:
        d.use = d.type == kind
    cy.device = "GPU"
    print("render device: %s (%s)" % (kind, ", ".join(d.name for d in gpus)), flush=True)

def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    o = dict(fps=30, intro=18, layer_frames=8, hold=24, cut_frames=60, turn_frames=240, turn_deg=360.0,
             frames=None, every=1, list=None, still=None, size=(1920, 1080), percent=100, samples=24, noise=0.05,
             min_samples=12, threads=4, plate=(6.0, 13.0, 12.0), azim=(-74.0, -50.0), elev=(22.0, 17.0), dist=(3.9, 3.6),
             lens=70.0, fstop=2.0, target=(0.0, 0.0, 0.03), exposure=0.0, light=1.0, bounces=(3, 1, 2), fast_gi=1,
             story=0, blend=None)
    pos, i = [], 0
    while i < len(argv):
        a = argv[i]

        def nums(k, f=float):
            return tuple(f(x) for x in argv[i + 1:i + 1 + k])
        if a in ("--fps", "--intro", "--layer-frames", "--hold", "--cut-frames", "--turn-frames", "--every", "--still",
                 "--percent", "--samples", "--min-samples", "--threads", "--story", "--fast-gi"):
            o[a[2:].replace("-", "_")] = int(argv[i + 1]); i += 1
        elif a in ("--turn-deg", "--noise", "--lens", "--fstop", "--exposure", "--light"):
            o[a[2:].replace("-", "_")] = float(argv[i + 1]); i += 1
        elif a == "--frames": o["frames"] = nums(2, int); i += 2
        elif a == "--list": o["list"] = [int(x) for x in argv[i + 1].split(",")]; i += 1
        elif a == "--size": o["size"] = tuple(int(x) for x in argv[i + 1].lower().split("x")); i += 1
        elif a == "--plate": o["plate"] = nums(3); i += 3
        elif a in ("--azim", "--elev", "--dist"): o[a[2:]] = nums(2); i += 2
        elif a == "--target": o["target"] = nums(3); i += 3
        elif a == "--bounces": o["bounces"] = nums(3, int); i += 3
        elif a == "--blend": o["blend"] = argv[i + 1]; i += 1
        elif a.startswith("--"): sys.exit("printfilm.py: unknown option %s" % a)
        else: pos.append(a)
        i += 1
    if len(pos) != 2:
        sys.exit("usage: blender -b -P tools/render/printfilm.py -- PLYDIR OUTDIR [options] (see the file's head)")
    o["plydir"], o["out"] = pos
    return o


def ease(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


# ---- the timeline ----------------------------------------------------------------------------------------------------

def timeline(man, o):
    """every film frame: the state drawn (None: the plate alone), its phase, the camera's progress and the turn"""
    states = man["frames"]
    stored = {f["a"]: f["index"] for f in states if f["s"] == 0}
    n = man["stored_times"]
    summ = (man.get("header") or {}).get("summary") or {}
    layers = int((summ.get("results") or {}).get("layers") or n - 1)
    rows = [("intro", None)] * o["intro"]
    for k in range(layers):
        rows += [("build", stored[k])] * o["layer_frames"]
    rows += [("hold", stored[layers - 1])] * o["hold"]
    for a in range(layers - 1, n - 1):  # each stored state after the build, reached through the blends from the one before
        bl = sorted((f for f in states if f["a"] == a and f["b"] == a + 1 and f["s"] > 0), key=lambda f: f["s"])
        ss = [0.0] + [f["s"] for f in bl] + [1.0]
        ids = [stored[a]] + [f["index"] for f in bl] + [stored[a + 1]]
        for k in range(o["cut_frames"]):
            want = ease(k / max(o["cut_frames"] - 1, 1))
            rows.append(("cut", ids[int(np.argmin([abs(x - want) for x in ss]))]))
        rows += [("hold", stored[a + 1])] * o["hold"]
    move = len(rows)  # the camera moves until here, then holds while the part turns
    final = stored[n - 1]
    rows += [("turntable", final)] * o["turn_frames"]
    out = []
    for f, (phase, st) in enumerate(rows):
        cam = ease(f / max(move - 1, 1)) if f < move else 1.0
        turn = o["turn_deg"] * ease((f - move) / max(o["turn_frames"] - 1, 1)) if f >= move else 0.0
        out.append({"frame": f, "phase": phase, "state": st, "camera": cam, "turn_deg": turn,
                    "time_s": f / o["fps"], "stored": None if st is None else [states[st]["a"], states[st]["b"], states[st]["s"]]})
    return out, layers


# ---- materials -----------------------------------------------------------------------------------------------------

def printed_metal(name, grain=True):
    """the part as printed: a satin, slightly rough nickel alloy, with the stress as a cool tint blended into it; where
    the tint shows, the metal turns from a mirror into a tinted satin so the colour reads in any light. No emission."""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(b.outputs["BSDF"], out.inputs["Surface"])
    base = B.srgb_to_linear(RAMP[0][1])
    rough = 0.40
    B.set_input(b, "Metallic", 1.0)
    B.set_input(b, "Roughness", rough)
    if grain:  # the powder-blasted grain of a printed surface, in the rest position so it rides with the part
        rest = nt.nodes.new("ShaderNodeAttribute")
        rest.attribute_type, rest.attribute_name = "GEOMETRY", "rest"
        noise = nt.nodes.new("ShaderNodeTexNoise")
        noise.inputs["Scale"].default_value = 420.0
        noise.inputs["Detail"].default_value = 2.0
        nt.links.new(rest.outputs["Vector"], noise.inputs["Vector"])
        rr = nt.nodes.new("ShaderNodeMapRange")
        rr.inputs["To Min"].default_value, rr.inputs["To Max"].default_value = rough * 0.85, rough * 1.25
        nt.links.new(noise.outputs["Fac"], rr.inputs["Value"])
        nt.links.new(rr.outputs["Result"], B.inp(b, "Roughness"))
        bump = nt.nodes.new("ShaderNodeBump")
        bump.inputs["Strength"].default_value = 0.06
        bump.inputs["Distance"].default_value = 0.0004
        nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
        nt.links.new(bump.outputs["Normal"], B.inp(b, "Normal"))
    attr = nt.nodes.new("ShaderNodeAttribute")
    attr.attribute_type, attr.attribute_name = "GEOMETRY", "value01"
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    els = ramp.color_ramp.elements
    els[0].position, els[0].color = RAMP[0][0], base + (1.0,)
    els[1].position, els[1].color = RAMP[-1][0], B.srgb_to_linear(RAMP[-1][1]) + (1.0,)
    for p, col in RAMP[1:-1]:
        e = els.new(p)
        e.color = B.srgb_to_linear(col) + (1.0,)
    ramp.color_ramp.interpolation = "LINEAR"
    nt.links.new(attr.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], B.inp(b, "Base Color"))
    mr = nt.nodes.new("ShaderNodeMapRange")
    mr.inputs["From Min"].default_value, mr.inputs["From Max"].default_value = RAMP_START, 0.55
    mr.inputs["To Min"].default_value, mr.inputs["To Max"].default_value = 1.0, 0.72
    nt.links.new(attr.outputs["Fac"], mr.inputs["Value"])
    nt.links.new(mr.outputs["Result"], B.inp(b, "Metallic"))
    return m


def plate_steel():
    """the build plate: surface-ground steel, the grinding along x (anisotropic) with fine scratches"""
    m = bpy.data.materials.new("plate steel")
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes.get("Principled BSDF")
    B.set_input(b, "Base Color", B.srgb_to_linear((0.53, 0.54, 0.56)) + (1.0,))
    B.set_input(b, "Metallic", 1.0)
    B.set_input(b, "Roughness", 0.32)
    B.set_input(b, ["Anisotropic", "Anisotropy"], 0.5)
    tc = nt.nodes.new("ShaderNodeTexCoord")
    sep = nt.nodes.new("ShaderNodeMapping")
    sep.inputs["Scale"].default_value = (2.0, 900.0, 2.0)  # long streaks along x: the grinding marks
    nt.links.new(tc.outputs["Object"], sep.inputs["Vector"])
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 1.0
    noise.inputs["Detail"].default_value = 6.0
    nt.links.new(sep.outputs["Vector"], noise.inputs["Vector"])
    rr = nt.nodes.new("ShaderNodeMapRange")
    rr.inputs["To Min"].default_value, rr.inputs["To Max"].default_value = 0.26, 0.40
    nt.links.new(noise.outputs["Fac"], rr.inputs["Value"])
    nt.links.new(rr.outputs["Result"], B.inp(b, "Roughness"))
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.03
    bump.inputs["Distance"].default_value = 0.0005
    nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], B.inp(b, "Normal"))
    tan = nt.nodes.new("ShaderNodeTangent")
    tan.direction_type, tan.uv_map = "UV_MAP", "grind"  # the plate's UVs run along x and y: the tangent along x
    nt.links.new(tan.outputs["Tangent"], B.inp(b, "Tangent"))
    return m


def support_grey():
    return B.plain_material("support grey", (0.46, 0.47, 0.49), 0.72, metallic=0.25)


# ---- the scene -------------------------------------------------------------------------------------------------------

class Scene:
    def __init__(self, o):
        self.o = o
        with open(os.path.join(o["plydir"], "manifest.json")) as f:
            self.man = json.load(f)
        rlo, rhi = (np.array(v) for v in self.man["rest_bounds"])
        mx, my, th = (1e-3 * v for v in o["plate"])
        self.plate_lo = np.array([rlo[0] - mx, rlo[1] - my, -th])
        self.plate_hi = np.array([rhi[0] + mx, rhi[1] + my, 0.0])
        blo, bhi = (np.array(v) for v in self.man["bounds"])
        lo, hi = np.minimum(self.plate_lo, blo), np.maximum(self.plate_hi, bhi)
        self.center = 0.5 * (lo + hi)
        self.radius = 0.5 * float(np.linalg.norm(hi - lo))
        self.scale = 1.0 / self.radius  # the scene in object radii, so the studio does not depend on the part's size
        self.part_center = 0.5 * (rlo + rhi)
        self.objs, self.mats, self.loaded = {}, {}, None

    def place(self, xyz):
        return (xyz - self.center) * self.scale

    def build(self):
        o, sc = self.o, bpy.context.scene
        for ob in list(bpy.data.objects):
            bpy.data.objects.remove(ob, do_unlink=True)
        sc.render.engine = "CYCLES"
        cy = sc.cycles
        render_device(cy)
        cy.samples, cy.use_adaptive_sampling = o["samples"], True
        cy.adaptive_threshold, cy.adaptive_min_samples = o["noise"], o["min_samples"]
        cy.use_denoising = False
        cy.max_bounces, cy.diffuse_bounces, cy.glossy_bounces = o["bounces"]
        if o["fast_gi"] > 0:  # ambient occlusion in place of the deeper bounces: soft crevices, less noise, faster
            cy.use_fast_gi, cy.ao_bounces_render = True, o["fast_gi"]
            if hasattr(cy, "fast_gi_method"): cy.fast_gi_method = "REPLACE"
        cy.transmission_bounces, cy.transparent_max_bounces, cy.volume_bounces = 0, 2, 0
        cy.caustics_reflective = cy.caustics_refractive = False
        cy.sample_clamp_direct, cy.sample_clamp_indirect, cy.blur_glossy = 12.0, 3.0, 1.0
        cy.use_light_tree = True
        sc.render.threads_mode, sc.render.threads = "FIXED", o["threads"]
        sc.render.use_persistent_data = True
        sc.render.resolution_x, sc.render.resolution_y = o["size"]
        sc.render.resolution_percentage = o["percent"]
        sc.render.film_transparent = False
        sc.render.image_settings.file_format, sc.render.image_settings.color_mode = "PNG", "RGB"
        sc.render.image_settings.color_depth, sc.render.image_settings.compression = "8", 15
        sc.view_settings.view_transform = "AgX"
        try: sc.view_settings.look = "AgX - Medium High Contrast"
        except TypeError: pass
        sc.view_settings.exposure = o["exposure"]
        w = bpy.data.worlds.new("studio sky")
        sc.world = w
        w.light_settings.ao_factor, w.light_settings.distance = 1.0, 0.6  # the fast GI's occlusion, in object radii
        w.use_nodes = True
        bg = w.node_tree.nodes["Background"]
        bg.inputs["Color"].default_value = (0.8, 0.82, 0.85, 1)
        bg.inputs["Strength"].default_value = 0.30
        floor_z = self.place(self.plate_lo)[2]
        paper = B.plain_material("seamless", (0.52, 0.54, 0.58), 0.9)
        B.cyclorama("studio", floor_z - 0.001, 14.0, 5.0, 16.0, paper)
        # the turntable: the plate and the part ride on one empty that turns about the part's vertical axis
        self.pivot = bpy.data.objects.new("turntable", None)
        sc.collection.objects.link(self.pivot)
        pc = self.place(np.array([self.part_center[0], self.part_center[1], 0.0]))
        self.pivot.location = Vector((pc[0], pc[1], 0.0))
        self.pivot_inv = Matrix.Translation(-self.pivot.location)
        self.plate()
        # the lights, fixed in the room around the camera's path (as the first films', tools/render/beauty.py)
        az = 0.5 * (o["azim"][0] + o["azim"][1])
        tgt = Vector(self.target())
        k = 0.35 * o["light"]
        B.area_light("key", B.sph(az - 45, 50, 6.5), tgt, 3.5, 2.6, 560 * k, spread=80)
        B.area_light("fill", B.sph(az + 70, 15, 7.0), tgt, 5.0, 3.5, 70 * k, spread=90)
        B.area_light("rim left", B.sph(az + 140, 14, 6.5), tgt, 0.7, 4.0, 440 * k, spread=70)
        B.area_light("rim right", B.sph(az - 140, 14, 6.5), tgt, 0.7, 4.0, 340 * k, spread=70)
        B.area_light("top", Vector((0, 0, 7.0)), tgt, 7.0, 7.0, 70 * k, spread=90)
        B.area_light("back", B.sph(az + 180, 30, 6.5), tgt, 7.0, 1.6, 60 * k, spread=60)
        cam = bpy.data.cameras.new("camera")
        cam.lens, cam.sensor_width = o["lens"], 36.0
        cam.clip_start, cam.clip_end = 0.02, 200.0
        cam.dof.use_dof = o["fstop"] > 0
        cam.dof.aperture_fstop = max(o["fstop"], 0.01)
        cam.dof.aperture_blades = 7
        self.cam = bpy.data.objects.new("camera", cam)
        sc.collection.objects.link(self.cam)
        sc.camera = self.cam

    def target(self):
        pc = self.place(self.part_center)
        t = self.o["target"]
        return (pc[0] + t[0], pc[1] + t[1], pc[2] + t[2])

    def plate(self):
        """the build plate under the part, its top a hair below z = 0 so the part's foot never shares a plane with it"""
        lo, hi = self.place(self.plate_lo), self.place(self.plate_hi)
        hi = hi.copy()
        hi[2] -= 2e-6 * self.scale
        me = bpy.data.meshes.new("plate")
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
        f = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
        me.from_pydata(v, [], f)
        uv = me.uv_layers.new(name="grind")
        for lp in me.loops:  # u along x, v along y, so the grinding runs along x on the top
            x = me.vertices[lp.vertex_index].co
            uv.data[lp.index].uv = (x[0] + 0.3 * x[2], x[1] + 0.3 * x[2])
        me.update()
        ob = bpy.data.objects.new("plate", me)
        bpy.context.scene.collection.objects.link(ob)
        bev = ob.modifiers.new("bevel", "BEVEL")
        bev.width = 0.8e-3 * self.scale
        bev.segments = 4
        bev.limit_method = "NONE"
        ob.data.materials.append(plate_steel())
        ob.parent = self.pivot
        ob.matrix_parent_inverse = self.pivot_inv
        self.plate_ob = ob

    def material(self, name):
        if name not in self.mats:
            self.mats[name] = printed_metal("printed metal") if name == "part" else (
                support_grey() if name == "support" else B.plain_material("plate part", (0.5, 0.5, 0.52), 0.3, 1.0))
        return self.mats[name]

    def load(self, state):
        """the state's surfaces on the turntable (the same objects, new meshes); None draws the plate alone"""
        if state == self.loaded:
            return
        self.loaded = state
        seen = set()
        if state is not None:
            for p in self.man["frames"][state]["parts"]:
                if p["triangles"] == 0:
                    continue
                v, faces = B.read_ply(os.path.join(self.o["plydir"], p["file"]))
                name = p["name"]
                seen.add(name)
                me = bpy.data.meshes.new(name)
                nv, nf = len(v), len(faces)
                xyz = np.stack([v["x"], v["y"], v["z"]], 1).astype(np.float64)
                me.vertices.add(nv)
                me.vertices.foreach_set("co", self.place(xyz).astype(np.float32).ravel())
                me.loops.add(3 * nf)
                me.polygons.add(nf)
                me.loops.foreach_set("vertex_index", faces.astype(np.int32).ravel())
                me.polygons.foreach_set("loop_start", np.arange(0, 3 * nf, 3, dtype=np.int32))
                try: me.polygons.foreach_set("loop_total", np.full(nf, 3, np.int32))
                except (AttributeError, TypeError, RuntimeError): pass
                me.polygons.foreach_set("use_smooth", np.ones(nf, bool))
                me.update(calc_edges=True)
                at = me.attributes.new("value01", "FLOAT", "POINT")
                at.data.foreach_set("value", np.ascontiguousarray(v["value01"], np.float32))
                rest = self.place(np.stack([v["rest_x"], v["rest_y"], v["rest_z"]], 1).astype(np.float64))
                at = me.attributes.new("rest", "FLOAT_VECTOR", "POINT")
                at.data.foreach_set("vector", rest.astype(np.float32).ravel())
                n = np.stack([v["nx"], v["ny"], v["nz"]], 1).astype(np.float32)
                try:
                    if hasattr(me, "use_auto_smooth"): me.use_auto_smooth = True
                    me.normals_split_custom_set_from_vertices(n)
                except Exception as ex:
                    print("printfilm.py: custom normals not set (%s)" % ex)
                me.materials.append(self.material(name))
                ob = self.objs.get(name)
                if ob is None:
                    ob = bpy.data.objects.new(name, me)
                    bpy.context.scene.collection.objects.link(ob)
                    ob.parent = self.pivot
                    ob.matrix_parent_inverse = self.pivot_inv
                    self.objs[name] = ob
                else:
                    old = ob.data
                    ob.data = me
                    bpy.data.meshes.remove(old)
        for name, ob in self.objs.items():
            ob.hide_render = name not in seen

    def set_frame(self, row):
        o = self.o
        e = row["camera"]
        az = o["azim"][0] + (o["azim"][1] - o["azim"][0]) * e
        el = o["elev"][0] + (o["elev"][1] - o["elev"][0]) * e
        d = o["dist"][0] + (o["dist"][1] - o["dist"][0]) * e
        tgt = Vector(self.target())
        pos = tgt + B.sph(az, el, d)
        self.cam.location = pos
        self.cam.rotation_mode = "QUATERNION"
        self.cam.rotation_quaternion = (tgt - pos).to_track_quat("-Z", "Y")
        self.cam.data.dof.focus_distance = (tgt - pos).length
        self.pivot.rotation_euler = (0.0, 0.0, math.radians(row["turn_deg"]))


def legend_png(path, w=512, h=16):
    img = bpy.data.images.new("legend", w, h, alpha=False)
    xs = np.linspace(0, 1, w)
    cols = np.zeros((w, 3))
    for k in range(3):
        cols[:, k] = np.interp(xs, [p for p, _ in RAMP], [c[k] for _, c in RAMP])
    px = np.ones((h, w, 4), np.float32)
    px[:, :, :3] = cols[None, :, :]
    img.pixels.foreach_set(px.ravel())
    img.filepath_raw, img.file_format = path, "PNG"
    img.save()


def main():
    o = parse_args()
    os.makedirs(o["out"], exist_ok=True)
    S = Scene(o)
    S.build()
    rows, layers = timeline(S.man, o)
    with open(os.path.join(o["out"], "timeline.json"), "w") as f:
        json.dump({"fps": o["fps"], "frames": len(rows), "layers": layers, "scale": S.man["scale"], "range_pa": S.man["range"],
                   "rows": rows}, f, indent=0)
    legend_png(os.path.join(o["out"], "legend.png"))
    n = len(rows)
    if o["still"] is not None: todo = [o["still"]]
    elif o["list"]: todo = o["list"]
    else:
        a, b = o["frames"] if o["frames"] else (0, n - 1)
        todo = list(range(a, min(b, n - 1) + 1, o["every"]))
    print("printfilm.py: %d film frames (%.1f s at %d fps), %d stored layers; rendering %d" % (n, n / o["fps"], o["fps"], layers, len(todo)), flush=True)
    tmp = os.path.join(o["out"], ".passes")
    os.makedirs(tmp, exist_ok=True)
    B.passes_setup(tmp)
    t_all, done = time.time(), 0
    for j in todo:
        name = ("still_%04d.png" if o["still"] is not None else "frame_%04d.png") % j
        final = os.path.join(o["out"], name)
        if os.path.exists(final) and o["still"] is None:
            continue
        t0 = time.time()
        S.load(rows[j]["state"])
        S.set_frame(rows[j])
        if o["blend"] and done == 0:
            bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(o["blend"]))
        td = B.render_atrous(final, tmp, j + 1)
        done += 1
        print("printfilm.py: %s (%s, state %s), %.1f s (denoise %.1f s)" % (name, rows[j]["phase"], rows[j]["state"], time.time() - t0, td), flush=True)
        if o["story"] and (done == 1 or done % o["story"] == 0):
            small = os.path.join(o["out"], "story.png")
            B.small_copy(final, small)
            B.story(["frame", small, "--caption", "film frame %d of %d: %s" % (j + 1, n, rows[j]["phase"])])
            B.story(["progress", str(j + 1), str(n), "--unit", "frames"])
    print("printfilm.py: %d frames in %.0f s" % (done, time.time() - t_all))


if __name__ == "__main__":
    main()
