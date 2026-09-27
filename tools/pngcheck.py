#!/usr/bin/env python3
"""pngcheck.py - independent decoder check of the C PNG/zlib encoder (src/core/png.c).

Parses every chunk, verifies CRCs and the zlib stream with Python's zlib (a different deflate implementation),
reverses the scanline filters and compares the pixels with the raw files written by build/rendertest.
    python3 tools/pngcheck.py build/rendertest_out
"""
import struct
import sys
import zlib
from pathlib import Path


def decode_png(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "bad signature"
    pos, idat, ihdr, seen_end = 8, b"", None, False
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])
        assert zlib.crc32(ctype + body) & 0xFFFFFFFF == crc, f"CRC mismatch in {ctype}"
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            seen_end = True
        pos += 12 + length
    assert ihdr and seen_end, "missing IHDR or IEND"
    w, h, depth, ctype, comp, filt, inter = ihdr
    assert depth == 8 and comp == 0 and filt == 0 and inter == 0 and ctype in (2, 6)
    ch = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * ch
    assert len(raw) == h * (stride + 1), "decompressed size mismatch"
    out = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 0:
                pred = 0
            elif f == 1:
                pred = a
            elif f == 2:
                pred = b
            elif f == 3:
                pred = (a + b) // 2
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            else:
                raise AssertionError(f"bad filter {f}")
            line[x] = (line[x] + pred) & 0xFF
        out += line
        prev = line
    return w, h, ch, bytes(out)


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else "build/rendertest_out")
    fails = 0
    for png in sorted(d.glob("*.png")):
        raw = png.with_suffix(".raw")
        try:
            w, h, ch, pixels = decode_png(png.read_bytes())
            if raw.exists():
                assert pixels == raw.read_bytes(), "pixels differ from the raw reference"
            print(f"  ok   {png.name}: {w}x{h}, {ch} channels, {png.stat().st_size} bytes")
        except AssertionError as e:
            fails += 1
            print(f"  FAIL {png.name}: {e}")
    z = d / "text.zlib"
    if z.exists():
        if zlib.decompress(z.read_bytes()) == (d / "text.raw").read_bytes():
            print("  ok   text.zlib round trip")
        else:
            fails += 1
            print("  FAIL text.zlib round trip")
    print("PNG CHECK", "FAILED" if fails else "PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
