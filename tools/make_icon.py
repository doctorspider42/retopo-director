"""Writes packaging/app.ico: the faceted mark from the top bar, at every size
Windows asks an .ico for.

The same drawing as set_window_icon in src/ui/shell.cpp, on the same 64 unit
grid; run this again if that one changes. Needs Pillow.

    python tools/make_icon.py
"""

import math
import pathlib
import sys

from PIL import Image

ACCENT = (113, 126, 255)
TOP, UR, LR, BOT, LL, UL, MID = (32, 12), (49, 22), (49, 42), (32, 52), (15, 42), (15, 22), (32, 30)
FACETS = [(UL, TOP, MID, 0.85), (TOP, UR, MID, 0.55), (UL, MID, LL, 0.35),
          (UR, LR, MID, 0.20), (LL, MID, BOT, 0.15), (MID, LR, BOT, 0.08)]
EDGES = [(TOP, UR), (UR, LR), (LR, BOT), (BOT, LL), (LL, UL), (UL, TOP),
         (TOP, MID), (UL, MID), (UR, MID), (MID, BOT)]


def seg_distance(px, py, a, b):
    vx, vy = b[0] - a[0], b[1] - a[1]
    t = max(0.0, min(1.0, ((px - a[0]) * vx + (py - a[1]) * vy) / (vx * vx + vy * vy)))
    dx, dy = a[0] + vx * t - px, a[1] + vy * t - py
    return math.hypot(dx, dy)


def in_triangle(px, py, a, b, c):
    def side(p0, p1):
        return (p1[0] - p0[0]) * (py - p0[1]) - (p1[1] - p0[1]) * (px - p0[0])
    d0, d1, d2 = side(a, b), side(b, c), side(c, a)
    return (d0 >= 0 and d1 >= 0 and d2 >= 0) or (d0 <= 0 and d1 <= 0 and d2 <= 0)


def render(size, ss=4):
    img = Image.new("RGBA", (size, size))
    px = img.load()
    # Lines thicken as the icon shrinks, or they vanish at 16 px.
    line = max(1.6, 64.0 / size * 0.9)
    radius = 15.0
    for y in range(size):
        for x in range(size):
            r = g = b = a = 0.0
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + 0.5) / ss) * 64.0 / size
                    v = (y + (sy + 0.5) / ss) * 64.0 / size
                    qx = max(abs(u - 32.0) - (32.0 - radius), 0.0)
                    qy = max(abs(v - 32.0) - (32.0 - radius), 0.0)
                    if qx * qx + qy * qy > radius * radius:
                        continue
                    white = 0.0
                    for fa, fb, fc, alpha in FACETS:
                        if in_triangle(u, v, fa, fb, fc):
                            white = alpha
                    for ea, eb in EDGES:
                        if seg_distance(u, v, ea, eb) < line * 0.5:
                            white = 1.0
                    r += ACCENT[0] + (255 - ACCENT[0]) * white
                    g += ACCENT[1] + (255 - ACCENT[1]) * white
                    b += ACCENT[2] + (255 - ACCENT[2]) * white
                    a += 1.0
            if a:
                px[x, y] = (int(r / a + 0.5), int(g / a + 0.5), int(b / a + 0.5),
                            int(a / (ss * ss) * 255 + 0.5))
    return img


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "packaging/app.ico")
    out.parent.mkdir(parents=True, exist_ok=True)
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = {s: render(s, 4 if s <= 64 else 2) for s in sizes}
    images[256].save(out, format="ICO", sizes=[(s, s) for s in sizes],
                     append_images=[images[s] for s in sizes[:-1]])
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
