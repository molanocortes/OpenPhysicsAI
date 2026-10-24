#!/usr/bin/env python3
"""nvt2ply_check.py - checks what tools/render/nvt2ply.c wrote against the result file it read, by a second
implementation: the .nvt read here with numpy, the alive elements and their outer faces found again, every PLY vertex
compared with the node it names.

    python3 tools/render/nvt2ply_check.py RESULTS.nvt PLYDIR [--states LIST] [--probe NODE]

Criteria, written on 2026-10-22 before the exporter's first run and not to be changed after it:
    C1  for every state and element group: the PLY's triangles are exactly twice the outer faces of that group's alive
        elements (alive: not i < elem_birth and not 0 <= elem_death <= i, at stored time b when s > 0, else a; outer:
        a face that no other alive element shares)
    C2  its vertices are exactly the distinct (node, direction) pairs of those faces, the direction being the axis and
        sign of the face's outward normal at rest; the nodes the PLY names are exactly the faces' nodes
    C3  every vertex: rest = the node's position, u = the node's displacement blended as the state says
        ((1 - s) u_a + s u_b), position = rest + F u, value = the node's von Mises stress blended the same way, value01 =
        (value - lo) / (hi - lo) clamped to 0..1; to float32 rounding: 1e-6 of the largest |x|, |u|, (|x| + F|u|) and
        von Mises of the state
    C4  the union of all groups' triangles, as node indices, is closed and consistently oriented: every directed edge
        a -> b is used as often as b -> a
    C5  the volume inside that union (divergence theorem, at rest, from the nodes' double positions) equals the alive
        elements' volume to 1e-9 relative, and with the triangles reversed it would be negative: outward
    C6  at a probe node on the part's surface (the node of largest u_z at the last stored time, or --probe NODE) the PLY's
        displacement and stress are printed beside the .nvt's, and equal them as C3 says
Exit 0 when every criterion holds in every state checked, 1 otherwise.
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np

HF = np.array([[0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4], [1, 2, 6, 5], [2, 3, 7, 6], [3, 0, 4, 7]])
DT = {"d": "<f8", "i": "<i4", "b": "u1", "c": "i1"}


class Nvt:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            if f.read(8) != b"NVTHR001":
                raise SystemExit("%s: not a results.nvt" % path)
            hl = struct.unpack("<Q", f.read(8))[0]
            self.h = json.loads(f.read(hl))
        self.nn, self.ne, self.no = self.h["nnodes"], self.h["nelems"], self.h["noutputs"]
        self.arr, off = {}, 16 + hl
        for a in self.h["arrays"]:
            self.arr[a["name"]] = (DT[a["type"]], a["count"], off)
            off += a["count"] * np.dtype(DT[a["type"]]).itemsize
        self.mm = np.memmap(path, dtype="u1", mode="r")

    def get(self, name, first=0, n=None):
        if name not in self.arr:
            return None
        dt, count, off = self.arr[name]
        size = np.dtype(dt).itemsize
        n = count - first if n is None else n
        return np.frombuffer(self.mm[off + first * size: off + (first + n) * size].tobytes(), dtype=dt)

    def u(self, i):
        return self.get("mech_u", 3 * i * self.nn, 3 * self.nn).reshape(-1, 3)

    def vm(self, i):
        return self.get("mech_vm", i * self.nn, self.nn)


def read_ply(path):
    with open(path, "rb") as f:
        props, nv, nf = [], 0, 0
        while True:
            line = f.readline().decode("ascii").strip()
            w = line.split()
            if w[:2] == ["element", "vertex"]:
                nv = int(w[2])
            elif w[:2] == ["element", "face"]:
                nf = int(w[2])
            elif w and w[0] == "property" and w[1] == "float":
                props.append(w[2])
            if line == "end_header":
                break
        data = f.read()
    vdt = np.dtype([(p, "<f4") for p in props])
    v = np.frombuffer(data, dtype=vdt, count=nv)
    fdt = np.dtype([("n", "u1"), ("i", "<i4", (3,))])
    t = np.frombuffer(data, dtype=fdt, count=nf, offset=nv * vdt.itemsize)
    if np.any(t["n"] != 3):
        raise SystemExit("%s: faces other than triangles" % path)
    return v, t["i"]


def outer_faces(conn, X, alive):
    """the outer faces of the alive elements: (element, the four nodes oriented away from it, direction 0..5)"""
    el = np.nonzero(alive)[0]
    q = conn[el][:, HF].reshape(-1, 4)                     # every face of every alive element
    e = np.repeat(el, 6)
    k = np.sort(q, axis=1)
    order = np.lexsort((k[:, 3], k[:, 2], k[:, 1], k[:, 0]))
    ks = k[order]
    same_prev = np.zeros(len(ks), bool)
    same_prev[1:] = np.all(ks[1:] == ks[:-1], axis=1)
    same_next = np.zeros(len(ks), bool)
    same_next[:-1] = same_prev[1:]
    once = order[~same_prev & ~same_next]
    q, e = q[once], e[once]
    R = X[q]                                               # rest positions of the four corners
    n = 0.5 * np.cross(R[:, 2] - R[:, 0], R[:, 3] - R[:, 1])   # the area vector of a quadrilateral
    fc = R.mean(axis=1)
    ec = X[conn[e]].mean(axis=1)
    flip = np.einsum("ij,ij->i", n, fc - ec) < 0
    q[flip] = q[flip][:, [0, 3, 2, 1]]
    n[flip] = -n[flip]
    ax = np.argmax(np.abs(n), axis=1)
    d = 2 * ax + (n[np.arange(len(n)), ax] < 0)
    return e, q, d


def main():
    a = sys.argv[1:]
    states, probe = None, None
    if "--states" in a:
        i = a.index("--states")
        states = [int(x) for x in a[i + 1].split(",")]
        del a[i:i + 2]
    if "--probe" in a:
        i = a.index("--probe")
        probe = int(a[i + 1])
        del a[i:i + 2]
    if len(a) != 2:
        print(__doc__)
        return 2
    r = Nvt(a[0])
    plydir = Path(a[1])
    man = json.loads((plydir / "manifest.json").read_text())
    F, (lo, hi) = man["scale"], man["range"]
    X = r.get("xyz").reshape(-1, 3)
    conn = r.get("conn").reshape(-1, 8).astype(np.int64)
    birth, death, group = r.get("elem_birth"), r.get("elem_death"), r.get("elem_group")
    group = np.zeros(r.ne, np.uint8) if group is None else group
    vol_e = None
    xmax = float(np.abs(X).max())
    if probe is None:  # the part's surface node that rises most at the last stored time
        alive = np.ones(r.ne, bool)
        if birth is not None:
            alive &= ~(r.no - 1 < birth)
        if death is not None:
            alive &= ~((death >= 0) & (r.no - 1 >= death))
        e, q, d = outer_faces(conn, X, alive & (group == 0))
        nodes = np.unique(q)
        probe = int(nodes[np.argmax(r.u(r.no - 1)[nodes, 2])])
    fails, checked = 0, 0
    print("nvt2ply_check: %s, %d nodes, %d elements, %d stored times; %s, %d states, displacement x %g, range %.6g to %.6g Pa; "
          "probe node %d at %s m" % (a[0], r.nn, r.ne, r.no, plydir, len(man["frames"]), F, lo, hi, probe, X[probe].tolist()))
    for fr in man["frames"]:
        j = fr["index"]
        if states is not None and j not in states:
            continue
        A, B, s = fr["a"], fr["b"], fr["s"]
        t = B if s > 0 else A
        alive = np.ones(r.ne, bool)
        if birth is not None:
            alive &= ~(t < birth)
        if death is not None:
            alive &= ~((death >= 0) & (t >= death))
        u = (1 - s) * r.u(A) + (s * r.u(B) if s > 0 else 0)
        vm = (1 - s) * r.vm(A) + (s * r.vm(B) if s > 0 else 0)
        e, q, d = outer_faces(conn, X, alive)
        g = group[e]
        umax, vmax = float(np.abs(u).max()), float(np.abs(vm).max())
        tol_x, tol_u, tol_p, tol_v = 1e-6 * xmax, 1e-6 * umax + 1e-300, 1e-6 * (xmax + F * umax), 1e-6 * vmax + 1e-300
        files = {p["group"]: p for p in fr["parts"]}
        bad = []
        all_tris = []
        for gi in sorted(set(g.tolist()) | set(files)):
            sel = g == gi
            nq = int(sel.sum())
            if gi not in files:
                bad.append("group %d has %d outer faces and no PLY" % (gi, nq))
                continue
            v, tri = read_ply(plydir / files[gi]["file"])
            node = v["node"].astype(np.int64)
            all_tris.append(node[tri])
            # C1, C2
            if len(tri) != 2 * nq:
                bad.append("C1 group %d: %d triangles, %d outer faces" % (gi, len(tri), nq))
            pairs = np.unique(q[sel].ravel() * 6 + np.repeat(d[sel], 4))
            if len(v) != len(pairs):
                bad.append("C2 group %d: %d vertices, %d (node, direction) pairs" % (gi, len(v), len(pairs)))
            if not np.array_equal(np.unique(node), np.unique(q[sel])):
                bad.append("C2 group %d: the PLY's nodes are not the outer faces' nodes" % gi)
            # each vertex's direction, from the triangles that use it (at rest): one direction per vertex, and the
            # (node, direction) pairs of the vertices are the faces' pairs, each once
            Rt = X[node[tri]]
            nt = np.cross(Rt[:, 1] - Rt[:, 0], Rt[:, 2] - Rt[:, 0])
            axt = np.argmax(np.abs(nt), axis=1)
            dt = 2 * axt + (nt[np.arange(len(nt)), axt] < 0)
            dmin, dmax = np.full(len(v), 99), np.full(len(v), -1)
            for c in range(3):
                np.minimum.at(dmin, tri[:, c], dt)
                np.maximum.at(dmax, tri[:, c], dt)
            if np.any(dmax < 0) or np.any(dmin != dmax):
                bad.append("C2 group %d: %d vertices unused or shared by faces of two directions" % (gi, int(np.sum((dmax < 0) | (dmin != dmax)))))
            else:
                vp = node * 6 + dmin
                if len(np.unique(vp)) != len(vp) or not np.array_equal(np.unique(vp), pairs):
                    bad.append("C2 group %d: the vertices' (node, direction) pairs are not the faces' pairs, each once" % gi)
            # C3
            rest = np.stack([v["rest_x"], v["rest_y"], v["rest_z"]], 1).astype(np.float64)
            uu = np.stack([v["ux"], v["uy"], v["uz"]], 1).astype(np.float64)
            pos = np.stack([v["x"], v["y"], v["z"]], 1).astype(np.float64)
            e3 = [("rest", np.abs(rest - X[node]).max(), tol_x), ("u", np.abs(uu - u[node]).max(), tol_u),
                  ("position", np.abs(pos - (X[node] + F * u[node])).max(), tol_p),
                  ("value", np.abs(v["value"].astype(np.float64) - vm[node]).max(), tol_v),
                  ("value01", np.abs(v["value01"] - np.clip((vm[node] - lo) / (hi - lo), 0, 1)).max(), 1e-6)]
            for name, err, tol in e3:
                if not err <= tol:
                    bad.append("C3 group %d: %s differs by %.3g (tolerance %.3g)" % (gi, name, err, tol))
            if probe in set(node.tolist()) and gi == 0:
                k = int(np.nonzero(node == probe)[0][0])
                print("  state %d probe node %d: PLY u = %s m, von Mises %.9g Pa; nvt u = %s m, von Mises %.9g Pa" % (
                    j, probe, ["%.9g" % x for x in uu[k]], v["value"][k], ["%.9g" % x for x in u[probe]], vm[probe]))
        # C4, C5 over the union of the groups
        if all_tris:
            T = np.concatenate(all_tris)
            ed = np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]])
            key_f = ed[:, 0] * r.nn + ed[:, 1]
            key_b = ed[:, 1] * r.nn + ed[:, 0]
            uf, cf = np.unique(key_f, return_counts=True)
            ub_, cb = np.unique(key_b, return_counts=True)
            if not (np.array_equal(uf, ub_) and np.array_equal(cf, cb)):
                bad.append("C4: the union is not closed and consistently oriented")
            P = X[T]
            vol = float(np.einsum("ij,ij->i", P[:, 0], np.cross(P[:, 1], P[:, 2])).sum() / 6.0)
            if vol_e is None:
                he = X[conn[0]].max(axis=0) - X[conn[0]].min(axis=0)
                vol_e = float(np.prod(he))
            want = vol_e * int(alive.sum())
            if not abs(vol - want) <= 1e-9 * want:
                bad.append("C5: volume inside %.12g m^3, alive elements %.12g m^3" % (vol, want))
        checked += 1
        status = "pass" if not bad else "FAIL"
        fails += bool(bad)
        if bad or j == 0 or j == len(man["frames"]) - 1 or s > 0 or j % 10 == 0:
            print("state %3d (a %d, b %d, s %.3f): %d alive elements, %d outer faces, groups %s: %s%s" % (
                j, A, B, s, int(alive.sum()), len(q), sorted(set(g.tolist())), status, "" if not bad else "\n    " + "\n    ".join(bad)))
    print("nvt2ply_check: %d states checked, %d failed" % (checked, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
