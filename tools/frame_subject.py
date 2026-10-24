#!/usr/bin/env python3
"""frame_subject.py - crop a rendered picture so its subject sits in the centre, at a given aspect ratio.

    python3 tools/frame_subject.py in.png out.png [--aspect 1.6] [--margin 0.10] [--width 1600]

The application frames a part in the area left of its side panel, so an exported picture has its subject off centre.
The subject is found as the pixels that are clearly brighter or more colourful than the dark floor grid; the crop is
the smallest box of the asked aspect ratio around them plus a margin, kept inside the picture.
"""
import sys
import numpy as np
from PIL import Image


def main():
    a = sys.argv
    src, dst = a[1], a[2]
    opt = lambda k, d: float(a[a.index(k) + 1]) if k in a else d
    aspect, margin, width = opt("--aspect", 1.6), opt("--margin", 0.10), int(opt("--width", 1600))
    im = Image.open(src).convert("RGB")
    px = np.asarray(im).astype(np.int32)
    sat = px.max(axis=2) - px.min(axis=2)
    val = px.max(axis=2)
    # the coloured field, dark violet end of the colour map included; the grid lines are pale and thin, the floor grey
    mask = ((sat > 70) & (val > 95)) | ((sat > 45) & (px[:, :, 2] > px[:, :, 1] + 25) & (val > 55))
    # thin lines do not survive a coarse vote: keep 8x8 blocks that are at least a third subject
    h, w = mask.shape
    b = 8
    blocks = mask[: h // b * b, : w // b * b].reshape(h // b, b, w // b, b).mean(axis=(1, 3)) > 0.33
    ys, xs = np.where(blocks)
    if len(xs) == 0:
        raise SystemExit("no subject found")
    x0, x1, y0, y1 = xs.min() * b, (xs.max() + 1) * b, ys.min() * b, (ys.max() + 1) * b
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    bw, bh = (x1 - x0) * (1 + 2 * margin), (y1 - y0) * (1 + 2 * margin)
    if bw / bh < aspect: bw = bh * aspect
    else: bh = bw / aspect
    if bw > w: bw, bh = w, w / aspect
    if bh > h: bh, bw = h, h * aspect
    left = min(max(cx - bw / 2, 0), w - bw)
    top = min(max(cy - bh / 2, 0), h - bh)
    out = im.crop((int(left), int(top), int(left + bw), int(top + bh)))
    out = out.resize((width, int(round(width / aspect))), Image.LANCZOS)
    out.save(dst, optimize=True)
    off = ((cx - left) / bw - 0.5, (cy - top) / bh - 0.5)
    print(f"{dst}: subject box {x1 - x0}x{y1 - y0} px, centre offset after crop {off[0]:+.3f}, {off[1]:+.3f} of the frame")


if __name__ == "__main__":
    main()
