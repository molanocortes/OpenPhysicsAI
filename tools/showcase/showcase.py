#!/usr/bin/env python3
"""showcase.py - the capture preset of the repository's media: one renderer, one frame, one typography.

Every showcase picture is a stored state of a finished run, drawn by the simulator's own software renderer
(results_render / view_render with style "showcase": dark neutral background, none of the renderer's bitmap
annotations) and then framed by compose(): phenomenon and state at the top left, the colour bar with field name and
unit at the right, deformation scale and the run at the bottom. The colour bar is evaluated from the renderer's own
colour map formulas, read from src/render/swrender.c, and its range is the range the renderer reports, so the legend
cannot drift from the picture. Nothing in the rendered result is altered: compose() only draws around it.

Preset (PRESET below): canvas 1600 x 900, picture area 1380 x 900 at the left, films 960 x 540. Pillow draws the text.
"""
import base64, io, json, math, re, struct, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402

PRESET = {
    "canvas": [1600, 900], "picture": [1380, 900], "film": [960, 540],
    "background_top": [20, 22, 27], "background_bottom": [13, 14, 18],      # SHOW_TOP / SHOW_BOTTOM of viewrender.c
    "ink": (232, 234, 238), "soft": (158, 164, 173), "faint": (104, 110, 120), "accent": (120, 190, 235),
    "font": "/System/Library/Fonts/HelveticaNeue.ttc",
}


def font(size, weight="regular"):
    index = {"regular": 0, "bold": 1, "medium": 10}.get(weight, 0)
    try:
        return ImageFont.truetype(PRESET["font"], size, index=index)
    except Exception:
        return ImageFont.truetype(PRESET["font"], size, index=0)


class Engine(Client):
    """the MCP client, patient enough for a stored run of a real part"""
    def __init__(self, workspace, allow_read=(), allow_write=()):
        args = ["--embedded", "--workspace", str(workspace)]
        for d in allow_read: args += ["--allow-read", str(d)]
        for d in allow_write: args += ["--allow-write", str(d)]
        super().__init__(args)
        self.initialize()

    def recv(self, timeout=1800.0):
        return super().recv(timeout)

    def op(self, name, args):
        r = self.call(name, args)["result"]
        sc = r.get("structuredContent") or {}
        if sc.get("ok") is not True:
            raise SystemExit(f"{name} refused: {json.dumps(sc.get('error'))[:900]}")
        img = [x for x in r.get("content", []) if x.get("type") == "image"]
        return sc.get("value", {}), (Image.open(io.BytesIO(base64.b64decode(img[0]["data"]))).convert("RGB") if img else None)

    def wait(self, job, every=10):
        while True:
            st, _ = self.op("job_status", {"job_id": job, "wait_seconds": every})
            if st.get("state") not in ("queued", "running"):
                if st.get("state") != "succeeded":
                    raise SystemExit(f"job {job}: {st.get('state')} {json.dumps(st.get('error'))[:500]}")
                return st

    def picture(self, op, args):
        w, h = PRESET["picture"]
        return self.op(op, dict(args, style="showcase", width=w, height=h))


def colormap(name, t):
    """the renderer's colour map, from its source: no second definition to drift"""
    src = (ROOT / "src/render/swrender.c").read_text()
    t = min(max(t, 0.0), 1.0)
    num = r"(-?[0-9.]+(?:e-?\d+)?)"
    if name == "viridis":
        body = src[src.index("case SW_CMAP_VIRIDIS"):src.index("case SW_CMAP_HEAT")]
        c = [float(x) for x in re.findall(num, body[body.index("C[7][3]") + 7:body.index("double v[3]")])]
        rows = [c[i:i + 3] for i in range(0, 21, 3)]
        rgb = [sum(rows[i][k] * t ** i for i in range(7)) for k in range(3)]
    elif name == "heat":
        body = src[src.index("case SW_CMAP_HEAT"):src.index("case SW_CMAP_COOLWARM")]
        ts = [float(x) for x in re.findall(num, body[body.index("HEAT_T[5]") + 9:body.index("HEAT_C[5][3]")])]
        cs = [float(x) for x in re.findall(num, body[body.index("HEAT_C[5][3]") + 12:body.index("int i = 0")])]
        cs = [cs[i:i + 3] for i in range(0, 15, 3)]
        i = 0
        while i < 3 and t > ts[i + 1]: i += 1
        u = (t - ts[i]) / (ts[i + 1] - ts[i])
        rgb = [cs[i][k] + u * (cs[i + 1][k] - cs[i][k]) for k in range(3)]
    elif name == "coolwarm":
        a, m, b = (0.230, 0.299, 0.754), (0.865, 0.865, 0.865), (0.706, 0.016, 0.150)
        u, p, q = (t * 2, a, m) if t < 0.5 else ((t - 0.5) * 2, m, b)
        rgb = [p[k] + u * (q[k] - p[k]) for k in range(3)]
    else:
        raise SystemExit(f"colour map {name}: not part of the showcase preset")
    return tuple(int(round(255 * min(1, max(0, v)))) for v in rgb)


def nice_ticks(lo, hi, n=5):
    span = hi - lo
    if span <= 0: return [lo]
    raw = span / n
    mag = 10 ** math.floor(math.log10(raw))
    step = min((s for s in (1, 2, 2.5, 5, 10) if s * mag >= raw), default=10) * mag
    t, out = math.ceil(lo / step - 1e-9) * step, []
    while t <= hi + 1e-9 * span:
        out.append(0.0 if abs(t) < step * 1e-9 else t)
        t += step
    return out


def fmt(v, step):
    d = max(0, -int(math.floor(math.log10(step))) if step < 1 else 0)
    if step < 1 and abs(step / 10 ** math.floor(math.log10(step)) - 2.5) < 1e-9: d += 1
    return f"{v:.{d}f}"


def compose(picture, title, state, field=None, unit="", lo=0.0, hi=1.0, cmap="viridis", note="", run=""):
    """frame one rendered picture. field None: a geometry picture, no colour bar."""
    W, H = PRESET["canvas"]
    top, bot = PRESET["background_top"], PRESET["background_bottom"]
    canvas = Image.new("RGB", (W, H))
    px = canvas.load()
    for y in range(H):
        c = tuple(int(round(top[k] + (bot[k] - top[k]) * y / (H - 1))) for k in range(3))
        for x in range(PRESET["picture"][0], W): px[x, y] = c
    canvas.paste(picture, (0, 0))
    d = ImageDraw.Draw(canvas)
    d.text((56, 44), title, font=font(38, "medium"), fill=PRESET["ink"])
    d.text((57, 96), state, font=font(23), fill=PRESET["soft"])
    if field:
        bx, by, bw, bh = W - 178, 250, 20, 430
        for i in range(bh):
            d.line([(bx, by + bh - 1 - i), (bx + bw - 1, by + bh - 1 - i)], fill=colormap(cmap, i / (bh - 1)))
        d.rectangle([bx - 1, by - 1, bx + bw, by + bh], outline=PRESET["faint"])
        ticks = nice_ticks(lo, hi)
        step = ticks[1] - ticks[0] if len(ticks) > 1 else 1
        for t in ticks:
            y = by + bh - 1 - (t - lo) / (hi - lo) * (bh - 1)
            d.line([(bx + bw + 2, y), (bx + bw + 8, y)], fill=PRESET["soft"])
            d.text((bx + bw + 14, y), fmt(t, step), font=font(20), fill=PRESET["ink"], anchor="lm")
        words = field.split(" ")
        lines, cur = [], ""
        for w_ in words:
            if d.textlength((cur + " " + w_).strip(), font=font(20, "medium")) > 150 and cur:
                lines.append(cur); cur = w_
            else:
                cur = (cur + " " + w_).strip()
        lines.append(cur + (f", {unit}" if unit else ""))
        for i, ln in enumerate(lines):
            d.text((bx, by - 22 - 26 * (len(lines) - 1 - i)), ln, font=font(20, "medium"), fill=PRESET["ink"], anchor="ls")
    if note:
        brand_w = d.textlength("OpenPhysicsAI" + (f"  ·  {run}" if run else ""), font=font(17))
        room = W - 56 - brand_w - 40 - 57
        size = next((z for z in (21, 20, 19, 18) if d.textlength(note, font=font(z)) <= room), None)
        if size is None:
            raise SystemExit(f"the note does not fit in {room:.0f} px even at 18 px: shorten it ({note!r})")
        d.text((57, H - 50), note, font=font(size), fill=PRESET["soft"], anchor="ls")
    d.text((W - 56, H - 50), ("OpenPhysicsAI" + (f"  ·  {run}" if run else "")), font=font(17), fill=PRESET["faint"], anchor="rs")
    return canvas


def write_film(frames, path, ms=110, hold_last=1600, hold_first=500, colors=255, start=None, cmap=None):
    """a GIF at the preset's film size with one palette for the whole film (no flicker), and its poster frame"""
    fw, fh = PRESET["film"]
    small = [f.resize((fw, fh), Image.LANCZOS) for f in frames]
    dur0 = [ms] * len(small)
    dur0[0], dur0[-1] = hold_first, hold_last
    if start is not None:
        # a loop is a cycle: the file may begin at its most informative frame, which is what is shown wherever
        # animation is off, without changing the sequence a viewer sees
        k = start % len(small)
        small, dur0 = small[k:] + small[:k], dur0[k:] + dur0[:k]
    n = len(small)
    pick = sorted(set([0, n - 1] + [round(i * (n - 1) / 7) for i in range(8)]))      # first, last and six between
    strip = Image.new("RGB", (fw * (len(pick) + (2 if cmap else 0)), fh))
    for i, k in enumerate(pick): strip.paste(small[k], (i * fw, 0))
    if cmap:
        # a film has few pixels of the top of its scale, and a palette learnt from the frames alone would give the legend
        # the wrong colours: the colour map itself takes two frames' worth of the sample
        sw = ImageDraw.Draw(strip)
        x0, wide = fw * len(pick), 2 * fw
        for x in range(wide): sw.line([(x0 + x, 0), (x0 + x, fh)], fill=colormap(cmap, x / (wide - 1)))
    pal = strip.quantize(colors=colors, method=Image.MEDIANCUT, dither=Image.NONE)
    q = [f.quantize(palette=pal, dither=Image.NONE) for f in small]
    dur = dur0
    q[0].save(path, save_all=True, append_images=q[1:], duration=dur, loop=0, optimize=True, disposal=1)
    frames[-1].save(Path(path).with_name(Path(path).stem + "-poster.png"), optimize=True)
    return Path(path).stat().st_size


# ---- voxel cells to a closed binary STL (the boundary faces of a cell set) -------------------------------------

FACES = {  # (di, dj, dk): the four corners of the outward face, counter-clockwise seen from outside
    (-1, 0, 0): ((0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)),
    (1, 0, 0): ((1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1)),
    (0, -1, 0): ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)),
    (0, 1, 0): ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)),
    (0, 0, -1): ((0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)),
    (0, 0, 1): ((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)),
}


def write_stl(path, cells, h, title):
    tris = []
    for (i, j, k) in sorted(cells):
        for (d, corners) in FACES.items():
            if (i + d[0], j + d[1], k + d[2]) in cells:
                continue  # an interior face
            p = [((i + c[0]) * h, (j + c[1]) * h, (k + c[2]) * h) for c in corners]
            n = tuple(float(x) for x in d)
            tris.append((n, p[0], p[1], p[2]))
            tris.append((n, p[0], p[2], p[3]))
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(title.encode()[:79].ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for n, a, b, c in tris:
            f.write(struct.pack("<3f", *n))
            for v in (a, b, c):
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")
    return len(tris)
