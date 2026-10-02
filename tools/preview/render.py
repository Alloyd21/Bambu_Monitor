"""
Preview the e-paper screens on a PC.

Compiles Bambu_Monitor_ESP32/screen.h together with the LilyGo library's font code,
renders every scenario in preview.cpp and writes PNGs to tools/preview/out,
tinted like the real panel (warm grey paper, soft black ink).

Usage:  python tools/preview/render.py [plate.png]
Needs:  g++/gcc on PATH, Pillow, numpy, the LilyGo-EPD47 library.
"""
import glob
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
LIB = os.path.expanduser(os.environ.get(
    "EPD47_LIB", "~/Documents/Arduino/libraries/LilyGo-EPD47/src"))
OUT = os.path.join(HERE, "out")

PAPER = np.array([226, 224, 216], np.float32)   # how the panel's white looks
INK = np.array([34, 34, 38], np.float32)


def constant(name):
    src = open(os.path.join(ROOT, "Bambu_Monitor_ESP32", "screen.h")).read()
    return int(re.search(rf"const int {name}\s*=\s*(\d+);", src).group(1))


def make_thumb(path, w, h):
    """Approximation of renderGreyPreview() in the sketch."""
    img = Image.open(path).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    g = np.asarray(Image.alpha_composite(bg, img).convert("L"), np.float32)
    ys, xs = np.nonzero(g < 240)
    if len(xs):
        m = 8
        g = g[max(ys.min() - m, 0):ys.max() + m + 1, max(xs.min() - m, 0):xs.max() + m + 1]
    ch, cw = g.shape
    dw, dh = w, int(ch * w / cw)
    if dh > h:
        dh, dw = h, int(cw * h / ch)
    small = np.asarray(Image.fromarray(g.astype(np.uint8)).resize((dw, dh), Image.BOX), np.float32)
    content = small[small < 240]
    lo, hi = (np.percentile(content, 1), np.percentile(content, 99)) if content.size else (0, 239)
    lo = min(lo, 215)
    hi = max(hi, lo + 24)
    curve = 50 + (np.arange(256) / 255.0) ** 0.5 * (235 - 50)
    v = np.where(small >= 245, 255, curve[np.clip((small - lo) * 255 / (hi - lo), 0, 255).astype(int)])
    out = np.full((h, w), 255, np.uint8)
    err = v.copy()
    for y in range(dh):
        for x in range(dw):
            q = min(int((err[y, x] + 8) // 17), 15) * 17
            e = err[y, x] - q
            err[y, x] = q
            if x + 1 < dw: err[y, x + 1] += e * 7 / 16
            if y + 1 < dh:
                if x > 0: err[y + 1, x - 1] += e * 3 / 16
                err[y + 1, x] += e * 5 / 16
                if x + 1 < dw: err[y + 1, x + 1] += e / 16
    ox, oy = (w - dw) // 2, (h - dh) // 2
    out[oy:oy + dh, ox:ox + dw] = np.clip(err, 0, 255).astype(np.uint8)
    return out


def build():
    exe = os.path.join(OUT, "preview.exe")
    objs = []
    zsrc = ["adler32", "crc32", "inflate", "inffast", "inftrees", "zutil", "uncompr"]
    csrc = [os.path.join(LIB, "font.c"), os.path.join(HERE, "gfx.c")] + \
           [os.path.join(LIB, "zlib", f + ".c") for f in zsrc]
    inc = ["-I", os.path.join(HERE, "stubs"), "-I", LIB, "-I", os.path.join(ROOT, "Bambu_Monitor_ESP32")]
    for c in csrc:
        o = os.path.join(OUT, os.path.basename(c) + ".o")
        subprocess.check_call(["gcc", "-O2", "-w", "-c", c, "-o", o] + inc)
        objs.append(o)
    subprocess.check_call(["g++", "-std=c++17", "-O2", "-Wall", "-Wno-unused-function",
                           os.path.join(HERE, "preview.cpp"), "-o", exe] + inc + objs)
    return exe


def main():
    os.makedirs(OUT, exist_ok=True)
    plate = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "plate_1.png")
    w, h = constant("PREVIEW_W"), constant("PREVIEW_H")
    make_thumb(plate, w, h).tofile(os.path.join(OUT, "thumb.raw"))

    exe = build()
    subprocess.check_call([exe], cwd=HERE)

    pngs = []
    for pgm in sorted(glob.glob(os.path.join(OUT, "*.pgm")),
                      key=lambda p: int(re.match(r"\d+", os.path.basename(p)).group())):
        a = np.asarray(Image.open(pgm).convert("L"), np.float32) / 255.0   # 0 black .. 1 white
        rgb = INK + a[..., None] * (PAPER - INK)
        png = pgm[:-4] + ".png"
        Image.fromarray(rgb.astype(np.uint8)).save(png)
        os.remove(pgm)
        pngs.append(png)
        print("wrote", os.path.relpath(png, ROOT))

    # All screens on one sheet, two per row.
    imgs = [Image.open(p) for p in pngs]
    w, h = imgs[0].size
    gap, cols = 24, 2
    rows = (len(imgs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * w + (cols + 1) * gap, rows * h + (rows + 1) * gap), (255, 255, 255))
    for n, img in enumerate(imgs):
        sheet.paste(img, (gap + (n % cols) * (w + gap), gap + (n // cols) * (h + gap)))
    allpng = os.path.join(OUT, "_all.png")
    sheet.save(allpng)
    print("wrote", os.path.relpath(allpng, ROOT))


if __name__ == "__main__":
    main()
