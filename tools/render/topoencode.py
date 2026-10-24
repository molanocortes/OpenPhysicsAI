#!/usr/bin/env python3
"""topoencode.py: the generative-design film's frames encoded with their captions (tools/render/README.md,
"Generative design"): an H.264 MP4, a GIF for the README and a still.

    python3 tools/render/topoencode.py RUNDIR PLYDIR FILMDIR [options]

RUNDIR is what `build/gendesign run` wrote (history.csv, summary.json), PLYDIR what `build/gendesign ply` wrote
(manifest.json: the stress range), FILMDIR what tools/render/topofilm.py rendered (frame_NNNN.png, shots.json). Every
number in a caption is read from the run's own files: the iteration, the volume and the compliance of that frame's
design, the stiffness against the grey design, the stress range. The captions say what is computed and what is
Blender's presentation.

Options: --fps F [24]; --name PREFIX [generative_design]; --gif-width W [800]; --gif-fps F [10]; --gif-shots
"brief:K,carve:K,turn:K,stress:K" every K-th frame of each shot, 0 leaves it out [brief:12,carve:4,turn:6,stress:0];
--gif-colors N [128]; --gif-dither none|bayer [none]; --still K [the last frame of the turn]; --still-width W [1200];
--crf N [18]; --width W the MP4's width [the frames']; --threads N [4].
Writes FILMDIR/PREFIX.mp4, PREFIX.gif, PREFIX_still.png and the caption scripts beside them.
"""
import csv
import json
import os
import subprocess
import sys

FONT = "DejaVu Sans"
INK = "&H00362F2B"      # #2b2f36 as ASS writes colours (&HAABBGGRR)
SOFT = "&H50362F2B"     # the same ink, partly transparent
STUDIO = "0xd8dade"     # the studio's grey, for the fades


def arg(argv, name, default, kind=str, n=1):
    if name in argv:
        i = argv.index(name)
        vals = argv[i + 1:i + 1 + n]
        return [kind(v) for v in vals] if n > 1 else kind(vals[0])
    return default


def ts(seconds):
    cs = int(round(seconds * 100))
    return "%d:%02d:%02d.%02d" % (cs // 360000, (cs // 6000) % 60, (cs // 100) % 60, cs % 100)


def read_history(path):
    rows = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            rows[int(r["iteration"])] = r
    return rows


def ass_script(width, height, grow):
    s = lambda v: int(round(v * grow))
    head = ["[Script Info]", "ScriptType: v4.00+", "PlayResX: %d" % width, "PlayResY: %d" % height, "WrapStyle: 2",
            "ScaledBorderAndShadow: yes", "", "[V4+ Styles]",
            "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
            "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
            "MarginR, MarginV, Encoding"]
    m = s(width * 0.03)
    head.append("Style: Title,%s,%d,%s,&H000000FF,&H00FFFFFF,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,%d,%d,%d,1"
                % (FONT, s(height * 0.036), INK, m, m, s(height * 0.045)))
    head.append("Style: Sub,%s,%d,%s,&H000000FF,&H00FFFFFF,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,%d,%d,%d,1"
                % (FONT, s(height * 0.024), INK, m, m, s(height * 0.045) + s(height * 0.052)))
    head.append("Style: Credit,%s,%d,%s,&H000000FF,&H00FFFFFF,&H00000000,0,0,0,0,100,100,0,0,1,0,0,3,%d,%d,%d,1"
                % (FONT, s(height * 0.019), SOFT, m, m, s(height * 0.04)))
    head.append("Style: Legend,%s,%d,%s,&H000000FF,&H00FFFFFF,&H00000000,0,0,0,0,100,100,0,0,1,0,0,1,%d,%d,%d,1"
                % (FONT, s(height * 0.02), INK, m, m, s(height * 0.04)))
    head += ["", "[Events]", "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text"]
    return head


def captions(shots, hist, summ, stress_range, fps, width, height, grow, legend_box):
    """one caption per run of frames that say the same; the carving's captions change with every iteration"""
    frames = shots["frames"]
    vf = summ["settings"]["volume_fraction"]
    whole = summ["elements_whole"]
    stiff = summ["compliance_uniform_j"] / summ["compliance_final_j"]
    iters = summ["iterations"]
    lo, hi = stress_range
    texts = []
    for f in frames:
        it = f["iteration"]
        h = hist.get(it, {})
        vol = float(h.get("volume_fraction", 1.0))
        comp = float(h.get("compliance_j", 0.0))
        if f["shot"] == "brief":
            t1 = "A titanium engine-mount bracket: the design space"
            t2 = "160 x 100 x 72 mm of Ti-6Al-4V, %.0f kN through the clevis at %.0f degrees, four bolts" % (
                summ.get("load_n", 25000) / 1e3, summ.get("load_angle_deg", 30))
        elif f["shot"] == "carve":
            t1 = "Topology optimisation, iteration %d" % it
            t2 = "%.1f %% of the design space, compliance %.4g J" % (100 * vol, comp)
        elif f["shot"] == "turn":
            t1 = "The optimised bracket after %d iterations" % iters
            t2 = "%.0f %% of the design space, %.1f times as stiff as the same titanium spread evenly" % (100 * vf, stiff)
        else:
            t1 = "Von Mises stress under the %.0f kN load" % (summ.get("load_n", 25000) / 1e3)
            t2 = "computed by the engine on the final design (element values, %.0f to %.0f MPa)" % (lo / 1e6, hi / 1e6)
        texts.append((t1, t2, f["shot"] == "stress" and f["stress"] > 0.5))
    lines = ass_script(width, height, grow)
    j = 0
    while j < len(texts):
        k = j
        while k + 1 < len(texts) and texts[k + 1] == texts[j]:
            k += 1
        a, b = ts(j / fps), ts((k + 1) / fps)
        lines.append("Dialogue: 0,%s,%s,Title,,0,0,0,,%s" % (a, b, texts[j][0]))
        lines.append("Dialogue: 0,%s,%s,Sub,,0,0,0,,%s" % (a, b, texts[j][1]))
        j = k + 1
    credit = ("Computed by OpenPhysicsAI: SIMP topology optimisation of a %.1f million voxel design space (half computed, "
              "mirrored). Light, materials and camera: Blender Cycles" % (whole / 1e6))
    lines.append("Dialogue: 0,%s,%s,Credit,,0,0,0,,%s" % (ts(0), ts(len(frames) / fps), credit))
    # the stress legend's numbers, under the colour bar that is overlaid on the stress frames
    s0 = next((i for i, t in enumerate(texts) if t[2]), None)
    if s0 is not None:
        x, y, w, hh = legend_box
        a, b = ts(s0 / fps), ts(len(frames) / fps)
        lines.append("Dialogue: 0,%s,%s,Legend,,0,0,0,,{\\an7\\pos(%d,%d)}%.0f MPa" % (a, b, x, y + hh + 6 * grow, lo / 1e6))
        lines.append("Dialogue: 0,%s,%s,Legend,,0,0,0,,{\\an9\\pos(%d,%d)}%.0f MPa" % (a, b, x + w, y + hh + 6 * grow, hi / 1e6))
    return lines, s0


def legend_png(path, w, h):
    """the stress ramp of topofilm.py as an image (sRGB stops, linear in between)"""
    from PIL import Image
    stops = [(0.0, (0.40, 0.46, 0.56)), (0.14, (0.62, 0.66, 0.70)), (0.32, (0.95, 0.74, 0.30)), (0.55, (0.97, 0.45, 0.10)),
             (0.80, (0.82, 0.13, 0.05)), (1.0, (0.50, 0.03, 0.05))]
    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(w):
        t = i / max(1, w - 1)
        for k in range(len(stops) - 1):
            if stops[k][0] <= t <= stops[k + 1][0]:
                u = (t - stops[k][0]) / (stops[k + 1][0] - stops[k][0])
                c = tuple(int(round(255 * (stops[k][1][q] + u * (stops[k + 1][1][q] - stops[k][1][q])))) for q in range(3))
                break
        for j in range(h):
            px[i, j] = c
    img.save(path)


def run(cmd):
    print("topoencode.py: " + " ".join(cmd[:6]) + " ...", flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("topoencode.py: ffmpeg failed: %s" % r.stderr[-2000:])


def main():
    argv = sys.argv[1:]
    pos = [a for i, a in enumerate(argv) if not a.startswith("--") and (i == 0 or not argv[i - 1].startswith("--"))]
    if len(pos) < 3:
        sys.exit(__doc__)
    rundir, plydir, filmdir = pos[:3]
    fps = arg(argv, "--fps", 24, int)
    name = arg(argv, "--name", "generative_design")
    gif_w = arg(argv, "--gif-width", 800, int)
    gif_fps = arg(argv, "--gif-fps", 10, int)
    gif_shots = dict((k, int(v)) for k, v in (x.split(":") for x in arg(argv, "--gif-shots", "brief:12,carve:4,turn:6,stress:0").split(",")))
    gif_colors = arg(argv, "--gif-colors", 128, int)
    gif_dither = arg(argv, "--gif-dither", "none")
    still_w = arg(argv, "--still-width", 1200, int)
    crf = arg(argv, "--crf", 18, int)
    threads = arg(argv, "--threads", 4, int)
    shots = json.load(open(os.path.join(filmdir, "shots.json")))
    frames = shots["frames"]
    n = len(frames)
    picked = [f["frame"] for f in frames if gif_shots.get(f["shot"], 0) > 0 and
              sum(1 for g in frames[:f["frame"]] if g["shot"] == f["shot"]) % gif_shots[f["shot"]] == 0]
    turn_frames = [f["frame"] for f in frames if f["shot"] == "turn"]
    still = arg(argv, "--still", turn_frames[-1] if turn_frames else n - 1, int)
    width = arg(argv, "--width", 0, int)
    hist = read_history(os.path.join(rundir, "history.csv"))
    summ = json.load(open(os.path.join(rundir, "summary.json")))
    man = json.load(open(os.path.join(plydir, "manifest.json")))
    stress = man.get("stress_range_pa", [0, 1])
    present = [f for f in sorted(os.listdir(filmdir)) if f.startswith("frame_") and f.endswith(".png")]
    if len(present) < n:
        print("topoencode.py: %d of %d frames rendered; encoding what is there" % (len(present), n))
    from PIL import Image
    W, H = Image.open(os.path.join(filmdir, present[0])).size
    out = lambda s: os.path.join(filmdir, s)
    # the film
    lw, lh = int(W * 0.18), max(6, int(H * 0.012))
    lx, ly = int(W * 0.03), int(H - H * 0.04 - H * 0.02 - 2 * lh - H * 0.03)
    legend_png(out("legend_stress.png"), lw, lh)
    lines, s0 = captions(shots, hist, summ, stress, fps, W, H, 1.0, (lx, ly, lw, lh))
    open(out("captions_film.ass"), "w").write("\n".join(lines) + "\n")
    T = n / fps
    fade = "fade=t=in:st=0:d=0.5:color=%s,fade=t=out:st=%.3f:d=0.7:color=%s" % (STUDIO, max(0.0, T - 0.7), STUDIO)
    on = "between(n,%d,%d)" % (s0 if s0 is not None else n, n)
    scale = ",scale=%d:-2:flags=lanczos" % width if width else ""
    fc = "[0:v][1:v]overlay=x=%d:y=%d:enable='%s',ass=%s,%s%s" % (lx, ly, on, out("captions_film.ass"), fade, scale)
    run(["nice", "-n", "19", "ffmpeg", "-v", "error", "-y", "-framerate", str(fps), "-i", out("frame_%04d.png"), "-i",
         out("legend_stress.png"), "-filter_complex", fc, "-c:v", "libx264", "-preset", "slow", "-crf", str(crf),
         "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-threads", str(threads), out(name + ".mp4")])
    # the GIF: every gif_every-th frame of the chosen range, at gif_w wide, captions grown for the small picture
    grow = 1.6
    gh = int(round(H * gif_w / W / 2)) * 2
    glw, glh = int(gif_w * 0.18), max(4, int(gh * 0.012 * grow))
    glx, gly = int(gif_w * 0.03), int(gh - gh * 0.04 * grow - gh * 0.02 * grow - 2 * glh - gh * 0.03 * grow)
    legend_png(out("legend_stress_gif.png"), glw, glh)
    sub = dict(shots, frames=[dict(frames[k], frame=i) for i, k in enumerate(picked)])
    gl, gs0 = captions(sub, hist, summ, stress, gif_fps, gif_w, gh, grow, (glx, gly, glw, glh))
    open(out("captions_gif.ass"), "w").write("\n".join(gl) + "\n")
    m = len(sub["frames"])
    TG = m / gif_fps
    gfade = "fade=t=in:st=0:d=0.4:color=%s,fade=t=out:st=%.3f:d=0.5:color=%s" % (STUDIO, max(0.0, TG - 0.5), STUDIO)
    gon = "between(n,%d,%d)" % (gs0 if gs0 is not None else m, m)
    sel = "select='%s',setpts=N/(%d*TB)" % ("+".join("eq(n\\,%d)" % k for k in picked), gif_fps)
    dither = "bayer:bayer_scale=5" if gif_dither == "bayer" else "none"
    gfc = ("[0:v]%s,scale=%d:%d:flags=lanczos[v];[v][1:v]overlay=x=%d:y=%d:enable='%s',ass=%s,%s,split[a][b];"
           "[a]palettegen=max_colors=%d:stats_mode=diff[p];[b][p]paletteuse=dither=%s:diff_mode=rectangle"
           % (sel, gif_w, gh, glx, gly, gon, out("captions_gif.ass"), gfade, gif_colors, dither))
    run(["nice", "-n", "19", "ffmpeg", "-v", "error", "-y", "-framerate", str(fps), "-i", out("frame_%04d.png"), "-i",
         out("legend_stress_gif.png"), "-filter_complex", gfc, "-r", str(gif_fps), out(name + ".gif")])
    # the still: one frame of the turn, without captions, at still_w wide
    run(["nice", "-n", "19", "ffmpeg", "-v", "error", "-y", "-i", out("frame_%04d.png" % still), "-vf",
         "scale=%d:-2:flags=lanczos" % still_w, out(name + "_still.png")])
    for f in (name + ".mp4", name + ".gif", name + "_still.png"):
        print("topoencode.py: %s %.2f MB" % (out(f), os.path.getsize(out(f)) / 1e6))


if __name__ == "__main__":
    main()
