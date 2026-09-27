#!/usr/bin/env python3
"""mosaic.py - the lab's films that the front page does not already show, in one light animated picture.

The README's gallery, its trials and its lab section each show films of their own; this picture is for the rest, one
film per subject (of two wings, two capsules or two dam breaks only one is shown anywhere). Each film is cut to its
scene (the app's panel, headers, titles and colour bars away), filled into a 4:3 tile and labelled; all play together
in one short loop. Nothing inside a scene is retouched. Kept small on purpose: a 4 x 4 grid, 40 frames, under 2 MB, so
that the page it closes stays quick to load and to scroll.

    python3 tools/showcase/mosaic.py            # writes docs/media/lab-all.webp, about a minute
    python3 tools/showcase/mosaic.py --frame    # one still in build/lab-all-check.png

When a film moves onto the front page, take it out of FILMS here.
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
M = ROOT / "docs" / "media"
OUT = M / "lab-all.webp"
COLS, TW, TH, GAP = 4, 236, 177, 6
FRAMES, FRAME_MS = 40, 120
FILMS = [  # file under docs/media, label; none of these is shown elsewhere on the README
    ("lab/flag-3d.gif", "A flag in the wind"), ("lab/whipple-shield.gif", "A Whipple shield at 6.5 km/s"),
    ("lab/double-mach.gif", "Mach 10 on a wedge"), ("lab/sloshing-tank.gif", "Water sloshes"),
    ("lab/battery-module.gif", "A battery under load"), ("lab/crater.gif", "A crater at 5 km/s"),
    ("lab/thruster-nozzle.gif", "A cold-gas thruster"), ("lab/wing-20.gif", "A wing stalls"),
    ("lab/room-fire.gif", "A fire in a room"), ("lab/taylor-copper.gif", "Copper hits a wall"),
    ("lab/laser-tracks.gif", "Laser tracks in steel"), ("lab/radar-volume.gif", "Radar in a 3D volume"),
    ("lab/data-centre.gif", "A data hall"), ("lab/sphere-plate.gif", "A sphere through a plate"),
    ("lab/office-hvac.gif", "An office's air"), ("lab/radar-pulse.gif", "A radar pulse meets two bodies"),
]


def scene_box(name, w, h):
    """Where the scene is in a film: the app's window, the older renderer's title band and colour bar, the showcase
    captions and the new films' one-line header are left out."""
    if (w, h) == (1200, 800):
        return (0, 84, 752, 760)
    if name.startswith("showcase-"):
        return (100, 74, 720, 478)
    if (w, h) in ((760, 700), (700, 560)):
        return (0, 22, w, h)
    if (w, h) == (752, 676):
        return (0, 0, w, h)
    return (0, int(h * 0.09), int(w * 0.86), h)


def cover(im, size):
    """Scaled to fill the tile and cropped at the centre."""
    w, h = im.size
    s = max(size[0] / w, size[1] / h)
    im = im.resize((max(size[0], round(w * s)), max(size[1], round(h * s))), Image.LANCZOS)
    x, y = (im.width - size[0]) // 2, (im.height - size[1]) // 2
    return im.crop((x, y, x + size[0], y + size[1]))


def font(size):
    try:
        f = ImageFont.truetype("/System/Library/Fonts/SFNS.ttf", size)
        f.set_variation_by_name("Semibold")
        return f
    except (OSError, ValueError):
        return ImageFont.load_default()


def main():
    rows = (len(FILMS) + COLS - 1) // COLS
    W, H = COLS * TW + (COLS - 1) * GAP, rows * TH + (rows - 1) * GAP
    f = font(16)
    shade = Image.new("L", (1, 44))
    for y in range(44):
        shade.putpixel((0, y), int(170 * (y / 43) ** 1.5))
    shade = shade.resize((TW, 44))
    mask = Image.new("L", (TW, TH), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, TW - 1, TH - 1), 8, fill=255)
    only = [FRAMES // 2] if "--frame" in sys.argv else range(FRAMES)
    tiles = []
    for name, label in FILMS:
        im = Image.open(M / name)
        box, n = scene_box(Path(name).name, *im.size), getattr(im, "n_frames", 1)
        frames = {}
        for k in only:  # decode only the frames the loop uses
            im.seek(k * n // FRAMES)
            frames[k] = cover(im.convert("RGB").crop(box), (TW, TH))
        tiles.append((frames, label))
    out = []
    for k in only:
        canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        for i, (frames, label) in enumerate(tiles):
            t = frames[k].copy()
            t.paste(Image.new("RGB", (TW, 44)), (0, TH - 44), shade)
            ImageDraw.Draw(t).text((10, TH - 10), label, font=f, fill=(245, 245, 247), anchor="ls")
            canvas.paste(t, ((i % COLS) * (TW + GAP), (i // COLS) * (TH + GAP)), mask)
        out.append(canvas)
    out[len(out) // 2].save(ROOT / "build" / "lab-all-check.png")
    if len(out) == 1:
        print("wrote build/lab-all-check.png")
        return 0
    out[0].save(OUT, save_all=True, append_images=out[1:], duration=FRAME_MS, loop=0, quality=62, method=6)
    print("wrote %s, %.1f MB, %d films, %d frames" % (OUT.relative_to(ROOT), OUT.stat().st_size / 1e6, len(FILMS), FRAMES))


if __name__ == "__main__":
    sys.exit(main())
