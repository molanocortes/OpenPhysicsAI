#!/usr/bin/env python3
"""xr_check.py - is a headset scene a valid glTF 2.0 file of the right thing at true size?

    python3 tools/xr/xr_check.py scene.glb [--stl part.stl] [--tol-mm 3]       a finite-element result (the app's export)
    python3 tools/xr/xr_check.py frame.glb                                      a lab frame (build/labxr)

Criterion, written before the first run (2026-10-06; amended the same day when the colours became a colour-map
texture instead of vertex colours): the GLB header, chunk lengths and every accessor fit the binary chunk; POSITION,
NORMAL and TEXCOORD_0 (the value, read in the embedded colour-map image) have one entry per vertex, every index points
at a vertex and the image is a PNG; with --stl, the model's extent in each axis matches the STL's (build z up becomes
glTF Y up) within --tol-mm, and it rests on y = 0.
Added 2026-10-07 for lab frames and the values the headset reads (written before their first run): every primitive of
every mesh is checked the same way (a primitive may be particles or lines; a flat one has no NORMAL); every attribute,
_VALUE among them, has one entry per vertex; every image is a PNG; triangle and line index counts are multiples of 3 and
2; the extras say what the scene is (extras.result.kind fem or lab) and carry a 4 x 4 to_source matrix; a lab volume's
values hold two bytes per cell of its dims and its mask, when there is one, one byte per cell. The finite-element scene
still rests on y = 0 and matches its STL.
"""
import argparse, json, struct, sys

def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    return ok

def stl_extent_mm(path):
    data = open(path, "rb").read()
    n = struct.unpack_from("<I", data, 80)[0]
    if 84 + 50 * n == len(data):
        lo, hi = [1e30] * 3, [-1e30] * 3
        for i in range(n):
            vals = struct.unpack_from("<12f", data, 84 + 50 * i)
            for v in range(3):
                for k in range(3):
                    x = vals[3 + 3 * v + k]
                    lo[k], hi[k] = min(lo[k], x), max(hi[k], x)
        return [hi[k] - lo[k] for k in range(3)]
    raise SystemExit("only binary STL is read here")

SIZE = {5126: 4, 5125: 4, 5123: 2, 5121: 1}
COMPS = {"VEC4": 4, "VEC3": 3, "VEC2": 2, "SCALAR": 1}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("glb")
    ap.add_argument("--stl")
    ap.add_argument("--tol-mm", type=float, default=3.0)
    a = ap.parse_args()
    b = open(a.glb, "rb").read()
    good = True
    magic, version, length = struct.unpack_from("<III", b, 0)
    good &= check(magic == 0x46546C67 and version == 2 and length == len(b), "glTF 2.0 header, length %d" % len(b))
    jlen, jtype = struct.unpack_from("<II", b, 12)
    good &= check(jtype == 0x4E4F534A and jlen % 4 == 0, "JSON chunk")
    g = json.loads(b[20:20 + jlen])
    blen, btype = struct.unpack_from("<II", b, 20 + jlen)
    binc = b[28 + jlen:28 + jlen + blen]
    good &= check(btype == 0x004E4942 and len(binc) == blen == g["buffers"][0]["byteLength"], "BIN chunk of %d bytes" % blen)
    views, acc = g.get("bufferViews", []), g.get("accessors", [])
    fits = all(v["byteOffset"] + v["byteLength"] <= blen for v in views)
    for i, ac in enumerate(acc):
        bv = views[ac["bufferView"]]
        fits &= ac["count"] * COMPS[ac["type"]] * SIZE[ac["componentType"]] == bv["byteLength"]
    good &= check(fits, "%d views and %d accessors fit the binary chunk" % (len(views), len(acc)))
    for i, img in enumerate(g.get("images", [])):
        iv = views[img["bufferView"]]
        good &= check(img["mimeType"] == "image/png" and binc[iv["byteOffset"]:iv["byteOffset"] + 8] == b"\x89PNG\r\n\x1a\n",
                      "image %d (a colour map) is an embedded PNG" % i)
    tk = ((g.get("nodes") or [{}])[0].get("extras") or {}).get("result") or {}
    kind = tk.get("kind")
    good &= check(kind in ("fem", "lab") and len(tk.get("to_source") or []) == 16, "extras: a %s scene with its to_source matrix" % kind)
    lo, hi, ntri, npts, nseg = [1e30] * 3, [-1e30] * 3, 0, 0, 0
    for mesh in g.get("meshes", []):
        for p in mesh["primitives"]:
            at, mode = p["attributes"], p.get("mode", 4)
            nv = acc[at["POSITION"]]["count"]
            ok = all(acc[i]["count"] == nv for i in at.values())
            if mode == 4:
                ok &= "NORMAL" in at or (p.get("extras") or {}).get("flat") is True
            n = nv
            if "indices" in p:
                ia = acc[p["indices"]]
                iv = views[ia["bufferView"]]
                idx = struct.unpack_from("<%dI" % ia["count"], binc, iv["byteOffset"])
                ok &= (not idx or max(idx) < nv)
                n = len(idx)
            ok &= n % (3 if mode == 4 else 2 if mode == 1 else 1) == 0
            ntri, npts, nseg = ntri + (n // 3 if mode == 4 else 0), npts + (n if mode == 0 else 0), nseg + (n // 2 if mode == 1 else 0)
            pv = views[acc[at["POSITION"]]["bufferView"]]
            pos = struct.unpack_from("<%df" % (3 * nv), binc, pv["byteOffset"])
            for k in range(3):
                lo[k], hi[k] = min(lo[k], min(pos[k::3])), max(hi[k], max(pos[k::3]))
            name = {0: "particles", 1: "lines", 4: "triangles"}[mode]
            good &= check(ok, "%s (%s): %d vertices, every attribute one per vertex%s, every index a vertex" %
                          (name, (p.get("extras") or {}).get("key", "field"), nv, ", a normal each" if "NORMAL" in at else ", flat"))
    print("  %d triangles, %d particles, %d line segments" % (ntri, npts, nseg))
    vol = tk.get("volume")
    if kind == "lab" and vol:
        cells = vol["dims"][0] * vol["dims"][1] * vol["dims"][2]
        ok = views[vol["values"]]["byteLength"] == 2 * cells and (vol["mask"] < 0 or views[vol["mask"]]["byteLength"] == cells)
        good &= check(ok, "volume %s, %s: two bytes a cell, mask %s" % ("x".join(map(str, vol["dims"])), vol["mode"], vol["mask_kind"]))
    if lo[0] < 1e30:
        ext = [1000 * (hi[k] - lo[k]) for k in range(3)]
        print("  extent %.2f x %.2f x %.2f mm (x, y up, z)" % tuple(ext))
        if kind == "fem":
            good &= check(abs(lo[1]) < 1e-6, "rests on y = 0")
        if a.stl:
            s = stl_extent_mm(a.stl)
            want = [s[0], s[2], s[1]]  # build x, z (up), y
            for k, name in enumerate("xyz"):
                good &= check(abs(ext[k] - want[k]) <= a.tol_mm, "%s extent %.2f mm against the STL's %.2f mm" % (name, ext[k], want[k]))
    print("XR SCENE CHECK %s" % ("PASSED" if good else "FAILED"))
    return 0 if good else 1

if __name__ == "__main__":
    sys.exit(main())
