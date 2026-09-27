#!/usr/bin/env python3
"""make_geometry.py - the bracket geometries of the release example and of the evaluation set, as binary STL in mm.

Every bracket is a profile in the XZ plane extruded along +y. Profiles use only axis-aligned edges and 45-degree edges
between points of a 4 mm lattice, and every face is triangulated on that lattice. That gives three properties:

  * the surface is closed and manifold (shared edges carry the same vertices, so there are no T-junctions);
  * sub-regions of large faces (a load pad, bolt pads) are unions of whole lattice squares, so they can be selected
    with triangle-mode box queries;
  * the voxel mesher's grid (multiples of the element size from the origin) represents every axis-aligned face exactly
    at 4, 2 and 1 mm; only the 45-degree faces become staircases.

Frame (mm): x from the wall outwards along the arm, y across the bracket width, z up. The wall is the plane x = 0.
Run: python3 examples/bracket_comparison/make_geometry.py [output directory]
"""
import struct
import sys
from pathlib import Path

S = 4.0  # lattice spacing, mm


def point_in_polygon(px, pz, poly):
    inside = False
    n = len(poly)
    for i in range(n):
        x1, z1 = poly[i]
        x2, z2 = poly[(i + 1) % n]
        if (z1 > pz) != (z2 > pz):
            xc = x1 + (pz - z1) * (x2 - x1) / (z2 - z1)
            if px < xc:
                inside = not inside
    return inside


def extrude(poly, width):
    """closed triangulation of the profile poly (lattice units, counter-clockwise seen from -y) extruded over width"""
    tris = []
    ny = int(round(width / S))
    xs = [p[0] for p in poly]
    zs = [p[1] for p in poly]
    # caps: whole lattice squares, or the half on the inside of a 45-degree edge
    for i in range(min(xs), max(xs)):
        for k in range(min(zs), max(zs)):
            c = [(i, k), (i + 1, k), (i + 1, k + 1), (i, k + 1)]
            halves = [(c[0], c[1], c[2]), (c[0], c[2], c[3]), (c[0], c[1], c[3]), (c[1], c[2], c[3])]
            inside = [point_in_polygon(sum(p[0] for p in t) / 3, sum(p[1] for p in t) / 3, poly) for t in halves]
            if inside[0] and inside[1]:
                cell = [halves[0], halves[1]]
            elif inside[2] and inside[3]:
                cell = [halves[2], halves[3]]
            else:
                cell = [t for t, inn in zip(halves, inside) if inn][:1]
            for a, b, cc in cell:
                # counter-clockwise in (x, z) seen from -y means the outward normal of the y = 0 cap is -y
                area2 = (b[0] - a[0]) * (cc[1] - a[1]) - (cc[0] - a[0]) * (b[1] - a[1])
                if area2 < 0:
                    b, cc = cc, b
                tris.append(((a[0], 0, a[1]), (cc[0], 0, cc[1]), (b[0], 0, b[1])))          # y = 0, normal -y
                tris.append(((a[0], ny, a[1]), (b[0], ny, b[1]), (cc[0], ny, cc[1])))       # y = width, normal +y
    # side walls: each profile edge in lattice steps, each step extruded in lattice steps
    n = len(poly)
    for e in range(n):
        (x1, z1), (x2, z2) = poly[e], poly[(e + 1) % n]
        steps = max(abs(x2 - x1), abs(z2 - z1))
        dx, dz = (x2 - x1) // steps, (z2 - z1) // steps
        for s in range(steps):
            ax, az = x1 + s * dx, z1 + s * dz
            bx, bz = ax + dx, az + dz
            for j in range(ny):
                p0, p1, p2, p3 = (ax, j, az), (bx, j, bz), (bx, j + 1, bz), (ax, j + 1, az)
                tris.append((p0, p1, p2))
                tris.append((p0, p2, p3))
    return tris


def signed_volume(tris):
    v = 0.0
    for a, b, c in tris:
        v += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0
    return v


def write_stl(path, tris, label):
    # the side walls follow the profile order; orient the whole surface outward from its signed volume
    if signed_volume(tris) < 0:
        tris = [(a, c, b) for a, b, c in tris]
    with open(path, "wb") as f:
        f.write(label.encode("ascii")[:80].ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0.0, 0.0, 0.0))
            for p in t:
                f.write(struct.pack("<3f", p[0] * S, p[1] * S, p[2] * S))
            f.write(b"\0\0")
    return abs(signed_volume(tris)) * S ** 3


# profiles in lattice units (4 mm): counter-clockwise in the XZ plane
L_PLAIN = [(0, 0), (2, 0), (2, 13), (20, 13), (20, 15), (0, 15)]                 # 8 mm wall 60 mm high, 8 mm arm 80 mm long
L_CHAMFER = [(0, 0), (2, 0), (2, 7), (8, 13), (20, 13), (20, 15), (0, 15)]      # the same with a 24 mm inner chamfer
L_TIP_CHAMFER = [(0, 0), (2, 0), (2, 13), (19, 13), (20, 14), (20, 15), (0, 15)]  # plain, 4 mm chamfer on the free lower tip edge
L_THIN = [(0, 0), (1, 0), (1, 14), (20, 14), (20, 15), (0, 15)]                  # 4 mm plates (for the thin-feature case)

DESIGNS = {
    "bracket_a_plain.stl": (L_PLAIN, 40, "bracket A: L, 8 mm plates"),
    "bracket_b_chamfer.stl": (L_CHAMFER, 40, "bracket B: L, 8 mm plates, 24 mm inner chamfer"),
    "bracket_a_tip_chamfer.stl": (L_TIP_CHAMFER, 40, "bracket A with a 4 mm chamfer at the free tip"),
    "bracket_thin.stl": (L_THIN, 40, "bracket with 4 mm plates"),
}


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / "geometry"
    out.mkdir(parents=True, exist_ok=True)
    for name, (poly, width, label) in DESIGNS.items():
        tris = extrude(poly, width)
        vol = write_stl(out / name, tris, label)
        print(f"{name}: {len(tris)} triangles, volume {vol:.1f} mm3")


if __name__ == "__main__":
    main()
