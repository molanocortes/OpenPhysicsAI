"""beauty.py: a physics-lab result filmed in Blender's Cycles like a product shot (tools/render/README.md).

    blender -b --factory-startup -noaudio -P tools/render/beauty.py -- PLYDIR OUTDIR [options]

PLYDIR is what tools/render/lab2ply.c wrote (manifest.json and frame_NNNN/PART.ply). The geometry and the field come
from the result, untouched: every vertex where the solver put it, every colour from the field's value at that vertex.
Materials, light, the studio and the camera are presentation and are chosen here.

Options (defaults in brackets):
    --frames A B        the manifest's frames A to B [all]; --every K [1]
    --still K           one frame only, K, written as OUTDIR/still_KKKK.png (the camera where the film has it then)
    --size WxH          [1920x1080]; --percent P renders at P per cent of it [100]
    --samples N         the most samples per pixel [32]; --noise T the adaptive threshold [0.05]; --min-samples [12]
    --threads N         [6]
    --azim A0 A1        the camera's azimuth over the film, degrees about the z axis [-95 -40]
    --elev E0 E1        its elevation [27 22]; --dist D0 D1 its distance in object radii [5.3 4.6]
    --lens MM           focal length [70]; --fstop F depth of field, 0 for none [1.6]; --focus X Y Z (object units)
    --target X Y Z      what the camera looks at, relative to the centre, in object radii [0 0 -0.08]
    --look NAME         studio: light (a pale grey seamless studio) or dark [light]
    --exposure EV       [0]; --light X scales every light [1]
    --bounces M D G     the most bounces in all, diffuse and glossy [4 1 3]
    --fast-gi N         after N bounces indirect light is ambient occlusion (Cycles' fast GI approximation), 0: off [1]
    --grain             a fine ground grain on the steel (noise on its roughness and a faint bump, riding with the part)
    --story K           every K frames, a 960 px picture and the progress to ~/bin/story (the owner's phone)
    --blend PATH        also save the scene as a .blend file (the first frame loaded), for looking at it in Blender
    --denoise atrous|nlm|none  [atrous] this Blender (Ubuntu's 4.0.2) is built without OpenImageDenoise, so Cycles has
                        no denoiser. atrous: the edge-avoiding a-trous wavelet filter of Dammertz, Sewtz, Hanika and
                        Lensch (HPG 2010) on Cycles' own passes, guided by the normal, depth and albedo of every pixel,
                        its colour weights scaled by the local noise as in Schied et al.'s SVGF (HPG 2017), on the light
                        divided by the albedo so that the field's colours and every edge stay sharp. nlm: ffmpeg's
                        non-local means on the finished image. none: Cycles' samples as they are
Frames that already exist in OUTDIR are skipped, so a stopped film resumes where it stopped.
"""
import json
import math
import os
import shutil
import subprocess
import sys
import time

import bpy
import numpy as np
from mathutils import Vector


# ---- arguments ---------------------------------------------------------------------------------------------------


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
    o = dict(frames=None, every=1, still=None, size=(1920, 1080), percent=100, samples=32, noise=0.05, min_samples=12,
             threads=6, azim=(-95.0, -40.0), elev=(27.0, 22.0), dist=(5.3, 4.6), lens=70.0, fstop=1.6, focus=None,
             target=(0.0, 0.0, -0.08), look="light", exposure=0.0, story=0, blend=None,
             denoise="atrous", light=1.0, fast_gi=1, bounces=(4, 1, 3), grain=False)
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
        elif a == "--azim": o["azim"] = tuple(nums(2)); i += 2
        elif a == "--elev": o["elev"] = tuple(nums(2)); i += 2
        elif a == "--dist": o["dist"] = tuple(nums(2)); i += 2
        elif a == "--lens": o["lens"] = float(argv[i + 1]); i += 1
        elif a == "--fstop": o["fstop"] = float(argv[i + 1]); i += 1
        elif a == "--focus": o["focus"] = tuple(nums(3)); i += 3
        elif a == "--target": o["target"] = tuple(nums(3)); i += 3
        elif a == "--look": o["look"] = argv[i + 1]; i += 1
        elif a == "--exposure": o["exposure"] = float(argv[i + 1]); i += 1
        elif a == "--story": o["story"] = int(argv[i + 1]); i += 1
        elif a == "--blend": o["blend"] = argv[i + 1]; i += 1
        elif a == "--denoise": o["denoise"] = argv[i + 1]; i += 1
        elif a == "--light": o["light"] = float(argv[i + 1]); i += 1
        elif a == "--fast-gi": o["fast_gi"] = int(argv[i + 1]); i += 1
        elif a == "--bounces": o["bounces"] = tuple(int(x) for x in argv[i + 1:i + 4]); i += 3
        elif a == "--grain": o["grain"] = True
        elif a.startswith("--"): sys.exit("beauty.py: unknown option %s" % a)
        else: pos.append(a)
        i += 1
    if len(pos) != 2:
        sys.exit("usage: blender -b -P tools/render/beauty.py -- PLYDIR OUTDIR [options] (see the file's head)")
    o["plydir"], o["out"] = pos
    return o


# ---- PLY as lab2ply writes it (binary little endian; float vertex properties; triangles) -------------------------

def read_ply(path):
    with open(path, "rb") as f:
        head = []
        while True:
            line = f.readline()
            if not line:
                raise ValueError("%s: no end_header" % path)
            line = line.decode("ascii").strip()
            head.append(line)
            if line == "end_header":
                break
        data = f.read()
    nv = nf = 0
    props, el = [], None
    for line in head:
        w = line.split()
        if w[0] == "element":
            el = w[1]
            if el == "vertex": nv = int(w[2])
            elif el == "face": nf = int(w[2])
        elif w[0] == "property" and el == "vertex":
            if w[1] != "float": raise ValueError("%s: vertex property %s is not float" % (path, w[2]))
            props.append(w[2])
    vdt = np.dtype([(p, "<f4") for p in props])
    v = np.frombuffer(data, dtype=vdt, count=nv)
    faces = np.zeros((0, 3), np.int32)
    if nf:
        fdt = np.dtype([("n", "u1"), ("i", "<i4", (3,))])
        f = np.frombuffer(data, dtype=fdt, count=nf, offset=nv * vdt.itemsize)
        if np.any(f["n"] != 3): raise ValueError("%s: faces other than triangles" % path)
        faces = f["i"]
    return v, faces


# ---- materials -----------------------------------------------------------------------------------------------------

def principled(nt):
    b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    return b


def set_input(node, names, value):
    for n in names if isinstance(names, (list, tuple)) else [names]:
        if n in node.inputs:
            node.inputs[n].default_value = value
            return node.inputs[n]
    return None


def inp(node, names):
    for n in names if isinstance(names, (list, tuple)) else [names]:
        if n in node.inputs:
            return node.inputs[n]
    raise KeyError(names)


# The field's colours: the steel's own colour where the field is low, then the house's "ember" sequence (labfilm's
# cmap 102: metal grey at zero, yellow, orange, red, dark red), so a stressed tooth reads as stressed steel.
RAMP = [(0.00, (0.62, 0.63, 0.65)), (0.05, (0.62, 0.63, 0.65)), (0.16, (0.96, 0.80, 0.42)),
        (0.36, (0.98, 0.50, 0.12)), (0.66, (0.85, 0.15, 0.05)), (1.00, (0.50, 0.03, 0.05))]
RAMP_START = 0.05  # below it the steel keeps its own colour


def srgb_to_linear(c):
    return tuple(((x + 0.055) / 1.055) ** 2.4 if x > 0.04045 else x / 12.92 for x in c)


def steel_material(name, base, rough, field=True, glow=0.0, grain=True):
    """machined steel: metallic, slightly rough, and the field as a colour ramp blended into the base colour; with
    grain, a fine ground texture that turns with the part (it is drawn in lab2ply's rest position)"""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    if hasattr(m, "cycles") and hasattr(m.cycles, "emission_sampling"):
        m.cycles.emission_sampling = "NONE"  # the glow is not a light: thousands of glowing triangles would be sampled
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    b = principled(nt)
    nt.links.new(b.outputs["BSDF"], out.inputs["Surface"])
    set_input(b, "Metallic", 1.0)
    set_input(b, "Roughness", rough)
    if grain:  # noise in the rest position (so it rides with the part), on the roughness and as a faint bump
        rest = nt.nodes.new("ShaderNodeAttribute")
        rest.attribute_type, rest.attribute_name = "GEOMETRY", "rest"
        noise = nt.nodes.new("ShaderNodeTexNoise")
        noise.inputs["Scale"].default_value = 160.0
        noise.inputs["Detail"].default_value = 3.0
        nt.links.new(rest.outputs["Vector"], noise.inputs["Vector"])
        rr = nt.nodes.new("ShaderNodeMapRange")
        rr.inputs["To Min"].default_value, rr.inputs["To Max"].default_value = rough * 0.8, rough * 1.2
        nt.links.new(noise.outputs["Fac"], rr.inputs["Value"])
        nt.links.new(rr.outputs["Result"], inp(b, "Roughness"))
        bump = nt.nodes.new("ShaderNodeBump")
        bump.inputs["Strength"].default_value = 0.025
        bump.inputs["Distance"].default_value = 0.0005
        nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
        nt.links.new(bump.outputs["Normal"], inp(b, "Normal"))
    base_lin = srgb_to_linear(base)
    if not field:
        set_input(b, "Base Color", base_lin + (1.0,))
        return m
    attr = nt.nodes.new("ShaderNodeAttribute")
    attr.attribute_type, attr.attribute_name = "GEOMETRY", "value01"
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    els = ramp.color_ramp.elements
    els[0].position, els[0].color = RAMP[0][0], base_lin + (1.0,)
    els[1].position, els[1].color = RAMP[-1][0], srgb_to_linear(RAMP[-1][1]) + (1.0,)
    for pos, col in RAMP[1:-1]:
        e = els.new(pos)
        e.color = (base_lin if pos <= RAMP_START else srgb_to_linear(col)) + (1.0,)
    ramp.color_ramp.interpolation = "LINEAR"
    nt.links.new(attr.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], inp(b, "Base Color"))
    # where the field shows, the steel turns from a mirror into a tinted surface so the colour reads in any light
    mr = nt.nodes.new("ShaderNodeMapRange")
    mr.inputs["From Min"].default_value, mr.inputs["From Max"].default_value = RAMP_START, 0.36
    mr.inputs["To Min"].default_value, mr.inputs["To Max"].default_value = 1.0, 0.25
    nt.links.new(attr.outputs["Fac"], mr.inputs["Value"])
    nt.links.new(mr.outputs["Result"], inp(b, "Metallic"))
    if glow > 0:  # a faint glow where the field is highest, so a loaded contact in the shade still shows
        gl = nt.nodes.new("ShaderNodeMapRange")
        gl.inputs["From Min"].default_value, gl.inputs["From Max"].default_value = 0.16, 1.0
        gl.inputs["To Min"].default_value, gl.inputs["To Max"].default_value = 0.0, glow
        nt.links.new(attr.outputs["Fac"], gl.inputs["Value"])
        nt.links.new(ramp.outputs["Color"], inp(b, ["Emission Color", "Emission"]))
        nt.links.new(gl.outputs["Result"], inp(b, "Emission Strength"))
    return m


def plain_material(name, base, rough, metallic=0.0, coat=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes.get("Principled BSDF")
    set_input(b, "Base Color", srgb_to_linear(base) + (1.0,))
    set_input(b, "Roughness", rough)
    set_input(b, "Metallic", metallic)
    if coat > 0:
        set_input(b, ["Coat Weight", "Clearcoat"], coat)
        set_input(b, ["Coat Roughness", "Clearcoat Roughness"], 0.08)
    return m


# ---- the studio ----------------------------------------------------------------------------------------------------

def cyclorama(name, floor_z, radius, fillet, height, mat, segments=160):
    """a seamless studio: a round floor that curves up into a wall all around, so the camera may turn"""
    prof = [(0.0, 0.0)]
    n_floor, n_fil, n_wall = 12, 24, 8
    for i in range(1, n_floor + 1):
        prof.append(((radius - fillet) * i / n_floor, 0.0))
    for i in range(1, n_fil + 1):
        a = 0.5 * math.pi * i / n_fil
        prof.append((radius - fillet + fillet * math.sin(a), fillet * (1 - math.cos(a))))
    for i in range(1, n_wall + 1):
        prof.append((radius, fillet + (height - fillet) * i / n_wall))
    verts, faces = [], []
    for s in range(segments):
        t = 2 * math.pi * s / segments
        for r, z in prof:
            verts.append((r * math.cos(t), r * math.sin(t), floor_z + z))
    n = len(prof)
    for s in range(segments):
        s2 = (s + 1) % segments
        for k in range(n - 1):
            a, b, c, d = s * n + k, s * n + k + 1, s2 * n + k + 1, s2 * n + k
            faces.append((a, d, c, b))
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    me.polygons.foreach_set("use_smooth", [True] * len(me.polygons))
    me.update()
    ob = bpy.data.objects.new(name, me)
    ob.data.materials.append(mat)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def area_light(name, pos, target, size, size_y, energy, color=(1, 1, 1), spread=180.0):
    li = bpy.data.lights.new(name, "AREA")
    li.shape = "RECTANGLE"
    li.size, li.size_y, li.energy, li.color = size, size_y, energy, color
    if hasattr(li, "spread"): li.spread = math.radians(spread)
    ob = bpy.data.objects.new(name, li)
    ob.location = pos
    ob.rotation_mode = "QUATERNION"
    ob.rotation_quaternion = (Vector(target) - Vector(pos)).to_track_quat("-Z", "Y")
    bpy.context.scene.collection.objects.link(ob)
    return ob


def sph(az_deg, el_deg, d):
    a, e = math.radians(az_deg), math.radians(el_deg)
    return Vector((d * math.cos(e) * math.cos(a), d * math.cos(e) * math.sin(a), d * math.sin(e)))


# ---- the scene -------------------------------------------------------------------------------------------------------

class Film:
    def __init__(self, o):
        self.o = o
        with open(os.path.join(o["plydir"], "manifest.json")) as f:
            self.man = json.load(f)
        lo, hi = np.array(self.man["bounds"][0]), np.array(self.man["bounds"][1])
        self.center = 0.5 * (lo + hi)
        self.radius = 0.5 * float(np.linalg.norm(hi - lo)) or 1.0
        self.scale = 1.0 / self.radius        # the scene in object radii: the studio's sizes do not depend on the result
        self.zmin = (lo[2] - self.center[2]) * self.scale
        self.objs = {}
        self.mats = {}

    # world positions of a result's vertices in the scene
    def place(self, xyz):
        return (xyz - self.center) * self.scale

    def material_for(self, name, has_field):
        """the part's look, by what it is: gears in machined steel, rings a darker ground steel, the rest of the
        assembly (carriers, pins, shafts: drawn, not computed) in black anodised aluminium"""
        key = ("ring" if "ring" in name else "gear") if has_field else "fixture"
        if key not in self.mats:
            g = self.o["grain"]
            if key == "gear": self.mats[key] = steel_material("steel", (0.78, 0.79, 0.81), 0.30, True, glow=0.6, grain=g)
            elif key == "ring": self.mats[key] = steel_material("ring steel", (0.44, 0.45, 0.47), 0.32, True, glow=0.6, grain=g)
            else: self.mats[key] = plain_material("anodised", (0.16, 0.165, 0.18), 0.30, 0.0, coat=0.5)
        return self.mats[key]

    def build(self):
        o, sc = self.o, bpy.context.scene
        for ob in list(bpy.data.objects): bpy.data.objects.remove(ob, do_unlink=True)
        sc.render.engine = "CYCLES"
        cy = sc.cycles
        render_device(cy)
        cy.samples, cy.use_adaptive_sampling = o["samples"], True
        cy.adaptive_threshold, cy.adaptive_min_samples = o["noise"], o["min_samples"]
        cy.use_denoising = False  # no denoiser in this build (see --denoise)
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
        # the world: a soft grey sky above the studio's opening
        w = bpy.data.worlds.new("studio sky")
        sc.world = w
        w.light_settings.ao_factor, w.light_settings.distance = 1.0, 0.6  # the fast GI's occlusion, in object radii
        w.use_nodes = True
        bg = w.node_tree.nodes["Background"]
        light = o["look"] != "dark"
        bg.inputs["Color"].default_value = (0.8, 0.82, 0.85, 1) if light else (0.02, 0.022, 0.026, 1)
        bg.inputs["Strength"].default_value = 0.35 if light else 0.15
        # the studio
        paper = plain_material("seamless", (0.60, 0.62, 0.66) if light else (0.10, 0.105, 0.115), 0.9)
        cyclorama("studio", self.zmin - 0.002, 14.0, 5.0, 16.0, paper)
        # the lights, fixed in the room: a key over the camera's left shoulder at the middle of its path, a fill, two
        # strip rims behind for the teeth's edges, a softbox overhead and a band behind that the parts' faces mirror
        az = 0.5 * (o["azim"][0] + o["azim"][1])
        tgt = Vector(o["target"])
        k = 0.35 * (1.0 if light else 1.3) * o["light"]
        area_light("key", sph(az - 45, 52, 6.5), tgt, 3.5, 2.6, 620 * k, spread=80)
        area_light("fill", sph(az + 70, 15, 7.0), tgt, 5.0, 3.5, 90 * k, spread=90)
        area_light("rim left", sph(az + 140, 12, 6.5), tgt, 0.7, 4.0, 420 * k, spread=70)
        area_light("rim right", sph(az - 140, 12, 6.5), tgt, 0.7, 4.0, 320 * k, spread=70)
        area_light("top", Vector((0, 0, 7.0)), tgt, 7.0, 7.0, 180 * k, spread=90)
        area_light("back", sph(az + 180, 30, 6.5), tgt, 7.0, 1.6, 90 * k, spread=60)  # a band mirrored in the gears' faces
        # the camera
        cam = bpy.data.cameras.new("camera")
        cam.lens, cam.sensor_width = o["lens"], 36.0
        cam.clip_start, cam.clip_end = 0.05, 200.0
        cam.dof.use_dof = o["fstop"] > 0
        cam.dof.aperture_fstop = max(o["fstop"], 0.01)
        cam.dof.aperture_blades = 7
        self.cam = bpy.data.objects.new("camera", cam)
        sc.collection.objects.link(self.cam)
        sc.camera = self.cam

    def set_camera(self, t):
        """the camera at film time t in 0..1: a slow orbit with a push in, eased at both ends"""
        o = self.o
        e = t * t * (3 - 2 * t)
        az = o["azim"][0] + (o["azim"][1] - o["azim"][0]) * e
        el = o["elev"][0] + (o["elev"][1] - o["elev"][0]) * e
        d = o["dist"][0] + (o["dist"][1] - o["dist"][0]) * e
        tgt = Vector(o["target"])
        pos = tgt + sph(az, el, d)
        self.cam.location = pos
        self.cam.rotation_mode = "QUATERNION"
        self.cam.rotation_quaternion = (tgt - pos).to_track_quat("-Z", "Y")
        focus = Vector(o["focus"]) if o["focus"] else tgt
        self.cam.data.dof.focus_distance = (focus - pos).length

    def load(self, fr):
        """the frame's parts as meshes (positions, normals, the field, rest positions), on the same objects"""
        seen = set()
        for p in fr["parts"]:
            if p["kind"] != "surface" or p["triangles"] == 0:
                continue
            v, faces = read_ply(os.path.join(self.o["plydir"], p["file"]))
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
            except (AttributeError, TypeError, RuntimeError): pass  # derived from loop_start in newer Blenders
            me.polygons.foreach_set("use_smooth", np.ones(nf, bool))
            me.update(calc_edges=True)
            for a in ("value01", "value"):
                at = me.attributes.new(a, "FLOAT", "POINT")
                at.data.foreach_set("value", np.ascontiguousarray(v[a], np.float32))
            if "rest_x" in v.dtype.names:
                rest = self.place(np.stack([v["rest_x"], v["rest_y"], v["rest_z"]], 1).astype(np.float64))
                at = me.attributes.new("rest", "FLOAT_VECTOR", "POINT")
                at.data.foreach_set("vector", rest.astype(np.float32).ravel())
            if "nx" in v.dtype.names:  # the exporter's normals, split at its creases
                n = np.stack([v["nx"], v["ny"], v["nz"]], 1).astype(np.float32)
                try:
                    if hasattr(me, "use_auto_smooth"): me.use_auto_smooth = True
                    me.normals_split_custom_set_from_vertices(n)
                except Exception as ex:
                    print("beauty.py: custom normals not set (%s)" % ex)
            ob = self.objs.get(name)
            if ob is None:
                ob = bpy.data.objects.new(name, me)
                bpy.context.scene.collection.objects.link(ob)
                ob.data.materials.append(self.material_for(name, p["field"]))
                self.objs[name] = ob
            else:
                old = ob.data
                ob.data = me
                me.materials.append(self.material_for(name, p["field"]))
                bpy.data.meshes.remove(old)
        for name, ob in self.objs.items():
            ob.hide_render = name not in seen


# ---- denoising after the render ------------------------------------------------------------------------------------

def passes_setup(tmp):
    """the compositor writes the passes the filter needs as float EXR files: the noisy image, the emission (kept out of
    the filter), the albedo and normal Cycles keeps for denoisers, and the depth"""
    sc = bpy.context.scene
    vl = sc.view_layers[0]
    vl.use_pass_z = vl.use_pass_normal = vl.use_pass_emit = True
    vl.use_pass_diffuse_color = vl.use_pass_glossy_color = True
    try: vl.cycles.denoising_store_passes = True
    except AttributeError: pass
    sc.use_nodes = True
    sc.render.use_compositing = True
    tree = sc.node_tree
    tree.nodes.clear()
    rl = tree.nodes.new("CompositorNodeRLayers")
    comp = tree.nodes.new("CompositorNodeComposite")
    tree.links.new(rl.outputs["Image"], comp.inputs["Image"])
    fo = tree.nodes.new("CompositorNodeOutputFile")
    fo.base_path = tmp
    fo.format.file_format, fo.format.color_depth, fo.format.exr_codec = "OPEN_EXR", "32", "NONE"  # read back at once
    fo.format.color_mode = "RGB"
    fo.file_slots.clear()
    names = {"noisy": "Image", "emit": "Emit", "normal": ("Denoising Normal", "Normal"), "depth": "Depth",
             "albedo": ("Denoising Albedo",), "diffcol": "DiffCol", "glosscol": "GlossCol"}
    if "Denoising Albedo" in rl.outputs and rl.outputs["Denoising Albedo"].enabled:
        del names["diffcol"], names["glosscol"]  # the albedo Cycles keeps for denoisers is the better one
    for slot, outs in names.items():
        outs = outs if isinstance(outs, tuple) else (outs,)
        src = next((rl.outputs[n] for n in outs if n in rl.outputs and rl.outputs[n].enabled), None)
        if src is None:
            continue
        fo.file_slots.new(slot + "_")
        tree.links.new(src, fo.inputs[slot + "_"])
    return tmp


def load_pass(tmp, slot, frame):
    path = os.path.join(tmp, "%s_%04d.exr" % (slot, frame))
    if not os.path.exists(path):
        return None
    img = bpy.data.images.load(path, check_existing=False)
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    os.remove(path)
    return px.reshape(h, w, 4)[:, :, :3]


def box(a, r):
    """mean over a (2r+1) square, edges repeated"""
    k = 2 * r + 1
    p = np.pad(a, r, mode="edge")
    c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def atrous(noisy, emit, albedo, normal, depth, iters=5, sigma_l=4.0, sigma_n=64.0, sigma_z=1.0):
    """Dammertz et al. 2010, edge-avoiding a-trous wavelets (5 x 5 B3 spline, holes 1, 2, 4, 8, 16), on the light
    divided by the albedo; the edge stopping on normals (power sigma_n), depth (against its gradient) and luminance
    (against sigma_l standard deviations of the noise, estimated in 5 x 5 at the start and carried through the
    iterations with the squared weights, as in SVGF)"""
    H, W = noisy.shape[:2]
    A = np.maximum(albedo, 0.02).astype(np.float32)
    L = (np.maximum(noisy - emit, 0) / A).astype(np.float32)
    lw = np.array([0.2126, 0.7152, 0.0722], np.float32)
    lum = L @ lw
    # a lone sample far brighter than every neighbour (a firefly) is brought down to four times their median
    p1 = np.pad(lum, 1, mode="edge")
    nb = np.stack([p1[1 + dy:1 + dy + H, 1 + dx:1 + dx + W] for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dy or dx])
    cap = 4.0 * np.median(nb, axis=0) + 0.05
    f = np.where(lum > cap, cap / np.maximum(lum, 1e-6), 1.0).astype(np.float32)
    L *= f[:, :, None]
    lum *= f
    var = np.maximum(box(lum * lum, 2) - box(lum, 2) ** 2, 0).astype(np.float32)
    nrm = normal / np.maximum(np.linalg.norm(normal, axis=2, keepdims=True), 1e-6)
    z = depth.astype(np.float32)
    gz = np.maximum(np.abs(np.diff(z, axis=0, append=z[-1:, :])), np.abs(np.diff(z, axis=1, append=z[:, -1:])))
    h = np.array([1 / 16, 1 / 4, 3 / 8, 1 / 4, 1 / 16], np.float32)
    n_sq = int(round(math.log2(sigma_n)))
    for it in range(iters):
        s = 1 << it
        pad = 2 * s
        Lp = np.pad(L, ((pad, pad), (pad, pad), (0, 0)), mode="edge")
        lp, vp, zp = (np.pad(a, pad, mode="edge") for a in (lum, var, z))
        Np = np.pad(nrm, ((pad, pad), (pad, pad), (0, 0)), mode="edge")
        sd = sigma_l * np.sqrt(np.maximum(box(var, 1), 0)) + 1e-4
        acc = np.zeros_like(L)
        wsum = np.zeros((H, W), np.float32)
        vacc = np.zeros((H, W), np.float32)
        for j in range(5):
            for i in range(5):
                dy, dx = (j - 2) * s, (i - 2) * s
                sl = (slice(pad + dy, pad + dy + H), slice(pad + dx, pad + dx + W))
                k = h[i] * h[j]
                if dy == 0 and dx == 0:
                    w = np.full((H, W), k, np.float32)
                else:
                    nd = np.clip(np.einsum("ijk,ijk->ij", nrm, Np[sl]), 0, 1)
                    for _ in range(n_sq):  # nd ** sigma_n by squaring
                        nd *= nd
                    dist = math.hypot(dx, dy)
                    e = np.abs(lum - lp[sl]) / sd + np.abs(z - zp[sl]) / (sigma_z * gz * dist + 1e-5)
                    w = k * nd * np.exp(-e)
                acc += w[:, :, None] * Lp[sl]
                wsum += w
                vacc += w * w * vp[sl]
        L = acc / wsum[:, :, None]
        var = vacc / (wsum * wsum)
        lum = L @ lw
    return L * A + emit


def save_linear(rgb, path):
    """a linear image saved as the render is: the scene's view transform, look and exposure, then PNG"""
    h, w = rgb.shape[:2]
    img = bpy.data.images.new("denoised", w, h, alpha=False, float_buffer=True)
    px = np.ones((h, w, 4), np.float32)
    px[:, :, :3] = rgb
    img.pixels.foreach_set(px.ravel())
    img.save_render(path, scene=bpy.context.scene)
    bpy.data.images.remove(img)


def render_atrous(final, tmp, frame):
    if not hasattr(render_atrous, "first"): render_atrous.first = frame
    sc = bpy.context.scene
    sc.frame_current = frame
    bpy.ops.render.render(write_still=False)
    t = time.time()
    P = {k: load_pass(tmp, k, frame) for k in ("noisy", "emit", "normal", "depth", "albedo", "diffcol", "glosscol")}
    albedo = P["albedo"] if P["albedo"] is not None else P["diffcol"] + P["glosscol"]
    if frame == render_atrous.first:
        print("beauty.py: denoising with the %s as albedo" % ("denoising albedo pass" if P["albedo"] is not None else
                                                            "diffuse and glossy colour passes"), flush=True)
    out = atrous(P["noisy"], P["emit"], albedo, P["normal"], P["depth"][:, :, 0])
    save_linear(out, final)
    if os.environ.get("BEAUTY_KEEP_NOISY"):  # the samples before the filter, beside it, for comparing
        save_linear(P["noisy"], final[:-4] + "_noisy.png")
    return time.time() - t


def denoise_png(src, dst, strength):
    """ffmpeg's nlmeans (Buades, Coll, Morel 2005) on the rendered PNG"""
    cmd = ["ffmpeg", "-v", "error", "-y", "-i", src, "-vf", "nlmeans=s=%.2f:p=7:pc=5:r=11:rc=9" % strength, dst]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("beauty.py: ffmpeg nlmeans failed: %s" % r.stderr.strip()[:300])
        shutil.copyfile(src, dst)


def legend_png(path, w=512, h=16):
    """the field's colour ramp as a small image, for the caption"""
    img = bpy.data.images.new("legend", w, h, alpha=False)
    xs = np.linspace(0, 1, w)
    cols = np.zeros((w, 3))
    stops = RAMP  # sRGB, as the image is
    for k in range(3):
        cols[:, k] = np.interp(xs, [p for p, _ in stops], [c[k] for _, c in stops])
    px = np.ones((h, w, 4), np.float32)
    px[:, :, :3] = cols[None, :, :]
    img.pixels.foreach_set(px.ravel())
    img.filepath_raw, img.file_format = path, "PNG"
    img.save()


def story(args):
    exe = os.path.expanduser("~/bin/story")
    if os.path.exists(exe):
        subprocess.run([exe] + args, capture_output=True)


def small_copy(src, dst, width=960):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-vf", "scale=%d:-2" % width, dst], capture_output=True)


def main():
    o = parse_args()
    os.makedirs(o["out"], exist_ok=True)
    film = Film(o)
    film.build()
    frames = film.man["frames"]
    n = len(frames)
    a, b = o["frames"] if o["frames"] else (0, n - 1)
    todo = [o["still"]] if o["still"] is not None else list(range(a, b + 1, o["every"]))
    legend_png(os.path.join(o["out"], "legend.png"))
    sc = bpy.context.scene
    tmp = os.path.join(o["out"], ".passes")
    if o["denoise"] == "atrous":
        os.makedirs(tmp, exist_ok=True)
        passes_setup(tmp)
    t_all = time.time()
    done = 0
    for j in todo:
        name = ("still_%04d.png" if o["still"] is not None else "frame_%04d.png") % j
        final = os.path.join(o["out"], name)
        if os.path.exists(final) and o["still"] is None:
            continue
        t0 = time.time()
        film.load(frames[j])
        film.set_camera(j / (n - 1) if n > 1 else 0.0)
        if o["blend"] and done == 0:
            bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(o["blend"]))
        if o["denoise"] == "atrous":
            td = render_atrous(final, tmp, j + 1)
        else:
            raw = final if o["denoise"] == "none" else final[:-4] + ".raw.png"
            sc.render.filepath = raw
            bpy.ops.render.render(write_still=True)
            td = time.time()
            if o["denoise"] == "nlm":
                denoise_png(raw, final, 1.6)
                os.remove(raw)
            td = time.time() - td
        done += 1
        print("beauty.py: %s, input %.3f, %.1f s (denoise %.1f s)" % (name, frames[j]["time"], time.time() - t0, td), flush=True)
        if o["story"] and (done == 1 or done % o["story"] == 0):
            small = os.path.join(o["out"], "story.png")
            small_copy(final, small)
            story(["frame", small, "--caption", "frame %d of %d, input %.1f deg" % (j + 1, n, frames[j]["time"])])
            story(["progress", str(j + 1), str(n), "--unit", "frames"])
    print("beauty.py: %d frames in %.0f s" % (done, time.time() - t_all))


if __name__ == "__main__":
    main()
