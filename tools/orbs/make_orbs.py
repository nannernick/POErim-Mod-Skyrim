#!/usr/bin/env python3
"""Orb icons: original 16x16 pixel art with black outlines, one palette per orb.

Writes, for each orb (roll::Orb order, ESP FormID 0x801 + index):
  tools/orbs/master/<key>.png                                   16x16 master
  data/GridInventory_fallback/item/DiabloInSkyrim.esp_0x000801.png  128x128, nearest-neighbour
  data/textures/DiabloInSkyrim/orbs/<key>.dds                   128x128 uncompressed RGBA

Run from the repo root:  python tools/orbs/make_orbs.py   (needs Pillow)
"""
import math, os, struct
from PIL import Image

SIZE, SCALE = 16, 8
BLACK = (12, 10, 14, 255)

# key, ramp (darkest -> highlight), motif
ORBS = [
    ("transmute", [(18, 40, 110), (40, 90, 200), (90, 160, 245), (200, 235, 255)], "shards"),
    ("augment",   [(16, 22, 90), (35, 60, 175), (75, 120, 235), (175, 205, 255)], "swirl"),
    ("alchemy",   [(110, 50, 10), (200, 110, 25), (245, 170, 60), (255, 235, 170)], "bubbles"),
    ("exalt",     [(95, 70, 15), (175, 135, 35), (230, 195, 70), (255, 245, 190)], "crown"),
    ("chaos",     [(70, 45, 20), (145, 100, 40), (205, 160, 75), (250, 225, 150)], "faces"),
    ("scour",     [(55, 55, 60), (130, 130, 138), (200, 200, 205), (250, 250, 252)], "yinyang"),
    ("divine",    [(80, 50, 60), (150, 105, 85), (205, 160, 140), (250, 215, 235)], "face"),
    ("vaal",      [(70, 8, 8), (150, 25, 20), (215, 60, 40), (255, 160, 120)], "faces"),
]

def shape(motif):
    """Filled pixels: a round body, lumpy for the face orbs."""
    pts = set()
    for y in range(SIZE):
        for x in range(SIZE):
            dx, dy = x - 7.5, y - 7.5
            r = 6.3
            if motif == "faces":   # three lobes
                a = math.atan2(dy, dx)
                r = 5.6 + 0.9 * max(0.0, math.cos(3 * a + 0.6))
            elif motif == "crown":  # rough nugget
                a = math.atan2(dy, dx)
                r = 5.9 + 0.5 * math.sin(5 * a) + 0.3 * math.cos(7 * a)
            if dx * dx + dy * dy <= r * r:
                pts.add((x, y))
    return pts

def shade(px, x, y, ramp):
    # light from the top-left: the further toward it, the brighter
    lx, ly = x - 4.5, y - 4.5
    d = math.sqrt(lx * lx + ly * ly)
    i = 3 if d < 1.6 else 2 if d < 4.5 else 1 if d < 8.0 else 0
    px[x, y] = ramp[i] + (255,)

def motif_pixels(px, filled, motif, ramp):
    dark = ramp[0] + (255,)
    hi = ramp[3] + (255,)
    put = lambda pts, c: [px.__setitem__(p, c) for p in pts if p in filled]
    if motif == "shards":
        put([(9, 4), (10, 5), (11, 6), (5, 10), (6, 11), (10, 10), (11, 11)], hi)
        put([(8, 8), (9, 9), (7, 9), (6, 6)], dark)
    elif motif == "swirl":
        put([(6, 5), (7, 5), (8, 5), (9, 6), (10, 7), (10, 8), (9, 9), (8, 9), (7, 8), (7, 7), (8, 7)], dark)
        put([(5, 9), (5, 10), (6, 11), (7, 11), (8, 11)], hi)
    elif motif == "bubbles":
        put([(6, 9), (9, 11), (10, 8), (8, 6)], hi)
        put([(7, 10), (10, 9), (9, 7)], dark)
    elif motif == "crown":
        put([(5, 9), (6, 8), (7, 9), (8, 8), (9, 9), (10, 8), (11, 9), (5, 10), (11, 10)], dark)
        put([(6, 10), (7, 10), (8, 10), (9, 10), (10, 10)], hi)
    elif motif == "faces":
        # three little faces: eye pairs and a mouth
        put([(5, 6), (7, 6), (6, 8), (9, 5), (11, 5), (10, 7), (8, 10), (10, 10), (9, 12)], dark)
    elif motif == "yinyang":
        for (x, y) in filled:
            if x + (y - 7.5) * 0.15 > 8:   # dark right half
                px[x, y] = ramp[0] + (255,) if (x - 9) ** 2 + (y - 5) ** 2 > 3 else ramp[2] + (255,)
        put([(6, 10), (5, 10)], dark)
        put([(10, 5)], hi)
    elif motif == "face":
        put([(6, 6), (9, 6), (7, 9), (8, 9), (7, 11), (8, 11)], dark)
        put([(6, 5), (9, 5)], hi)

def make(key, ramp, motif):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    px = img.load()
    filled = shape(motif)
    for (x, y) in filled:
        shade(px, x, y, ramp)
    motif_pixels(px, filled, motif, ramp)
    # black outline: every empty pixel touching the body
    for y in range(SIZE):
        for x in range(SIZE):
            if (x, y) in filled:
                continue
            if any((x + dx, y + dy) in filled for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                px[x, y] = BLACK
    return img

def write_dds(path, img):
    w, h = img.size
    header = struct.pack("<4s7I44x", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x8, h, w, w * 4, 0, 1)
    pf = struct.pack("<2I4s5I", 32, 0x41, b"\0\0\0\0", 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    caps = struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    raw = img.tobytes()   # RGBA rows, top first
    data = bytearray(len(raw))
    data[0::4], data[1::4], data[2::4], data[3::4] = raw[2::4], raw[1::4], raw[0::4], raw[3::4]
    with open(path, "wb") as f:
        f.write(header + pf + caps + bytes(data))

def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    master = os.path.join(root, "tools", "orbs", "master")
    grid = os.path.join(root, "data", "GridInventory_fallback", "item")
    dds = os.path.join(root, "data", "textures", "DiabloInSkyrim", "orbs")
    for d in (master, grid, dds):
        os.makedirs(d, exist_ok=True)
    for i, (key, ramp, motif) in enumerate(ORBS):
        small = make(key, ramp, motif)
        small.save(os.path.join(master, key + ".png"))
        big = small.resize((SIZE * SCALE, SIZE * SCALE), Image.NEAREST)
        big.save(os.path.join(grid, "DiabloInSkyrim.esp_0x%06X.png" % (0x801 + i)))
        write_dds(os.path.join(dds, key + ".dds"), big)
        print("orb", key)

if __name__ == "__main__":
    main()
