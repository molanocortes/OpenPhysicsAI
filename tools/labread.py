#!/usr/bin/env python3
"""Read a physics-lab result (src/lab/labio.h) into Python: the header, and any frame as parts with NumPy fields.

    from labread import LabFile
    f = LabFile('run.lab')
    print(f.header['title'], len(f), f.times)
    fr = f.frame(-1)                         # the last frame
    gas = fr['gas']                          # a part: fr[name]
    rho = gas.fields['rho']                  # one value per cell (blocks), point or node
    x, y = gas.cell_centres()                # for block parts
    i = rho.argmax(); print(x[i], y[i])

Command line: python3 tools/labread.py RUN.lab [FIELD]  prints parts, fields and where each field peaks.
NumPy is the only requirement (as for the other tools that use it).
"""
import json
import struct
import sys

import numpy as np


class Part:
    def __init__(self, kind, name):
        self.kind, self.name = kind, name
        self.blocks = []      # (n[3], level, plane, origin[3], dx[3]) for block parts
        self.xyz = None       # nodes or points, (n, 3)
        self.conn = None      # cells, (ncells, nodes per cell)
        self.fields = {}

    def cell_centres(self):
        """x, y, z of every cell of a block part, in the order of its fields."""
        xs, ys, zs = [], [], []
        for n, level, plane, o, dx in self.blocks:
            i, j, k = np.meshgrid(np.arange(n[0]), np.arange(n[1]), np.arange(max(n[2], 1)), indexing='ij')
            i, j, k = (a.transpose(2, 1, 0).ravel() for a in (i, j, k))
            u, v, w = o[0] + (i + 0.5) * dx[0], o[1] + (j + 0.5) * dx[1], o[2] + (k + 0.5) * dx[2]
            if n[2] <= 1 and plane == 1:
                u, v, w = u, np.full_like(u, o[1]), o[2] + (j + 0.5) * dx[1]
            elif n[2] <= 1 and plane == 2:
                u, v, w = np.full_like(u, o[0]), o[1] + (i + 0.5) * dx[0], o[2] + (j + 0.5) * dx[1]
            xs.append(u), ys.append(v), zs.append(w)
        return np.concatenate(xs), np.concatenate(ys), np.concatenate(zs)

    def cell_levels(self):
        return np.concatenate([np.full(n[0] * n[1] * max(n[2], 1), level) for n, level, _, _, _ in self.blocks])


class LabFile:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        if self.data[:8] != b'OPLAB01\n':
            raise ValueError('%s is not a lab result' % path)
        hl = struct.unpack_from('<I', self.data, 8)[0]
        self.header = json.loads(self.data[12:12 + hl].decode())
        self.offsets, self.times = [], []
        at = 12 + hl
        while at + 12 <= len(self.data) and self.data[at:at + 4] == b'FRAM':
            n = struct.unpack_from('<Q', self.data, at + 4)[0]
            if at + 12 + n > len(self.data):
                break
            self.offsets.append((at + 12, n))
            self.times.append(struct.unpack_from('<d', self.data, at + 12)[0])
            at += 12 + n

    def __len__(self):
        return len(self.offsets)

    def frame(self, i):
        off, n = self.offsets[i]
        d, c, end = self.data, off + 8, off + n
        parts = {}
        while c < end:
            assert d[c:c + 4] == b'PART'
            kind = struct.unpack_from('<i', d, c + 4)[0]
            name = d[c + 8:c + 56].split(b'\0')[0].decode()
            c += 56
            p = Part(kind, name)
            if kind == 1:
                nb = struct.unpack_from('<i', d, c)[0]
                c += 4
                for _ in range(nb):
                    nx, ny, nz, lev, pl = struct.unpack_from('<5i', d, c)
                    o = struct.unpack_from('<3d', d, c + 20)
                    dx = struct.unpack_from('<3d', d, c + 44)
                    p.blocks.append(((nx, ny, nz), lev, pl, o, dx))
                    c += 68
            else:
                npts = struct.unpack_from('<i', d, c)[0]
                p.xyz = np.frombuffer(d, '<f8', 3 * npts, c + 4).reshape(npts, 3)
                c += 4 + 24 * npts
                if kind == 3:
                    ncells, ct = struct.unpack_from('<2i', d, c)
                    p.conn = np.frombuffer(d, '<i4', ncells * ct, c + 8).reshape(ncells, ct)
                    c += 8 + 4 * ncells * ct
            nf = struct.unpack_from('<i', d, c)[0]
            c += 4
            for _ in range(nf):
                fname = d[c + 4:c + 52].split(b'\0')[0].decode()
                cnt = struct.unpack_from('<Q', d, c + 56)[0]
                p.fields[fname] = np.frombuffer(d, '<f4', cnt, c + 64)
                c += 64 + 4 * cnt
            parts[name] = p
        return parts


if __name__ == '__main__':
    f = LabFile(sys.argv[1])
    fr = f.frame(-1)
    print('%s: %d frames, t = %.6g s at the last' % (f.header.get('title', ''), len(f), f.times[-1]))
    for name, p in fr.items():
        print('part %s (%s)' % (name, {1: 'blocks', 2: 'points', 3: 'cells'}[p.kind]))
        centres = p.cell_centres() if p.kind == 1 else None
        for fn, v in p.fields.items():
            if len(sys.argv) > 2 and fn != sys.argv[2]:
                continue
            i = int(np.argmax(v))
            where = (' at (%.4g, %.4g)' % (centres[0][i], centres[1][i])) if centres is not None else ''
            print('  %-10s %.6g to %.6g, largest%s' % (fn, v.min(), v.max(), where))
