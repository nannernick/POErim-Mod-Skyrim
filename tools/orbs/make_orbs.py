#!/usr/bin/env python3
"""Orb icons: original 16x16 pixel art, thick (2 px) black outline, one look per orb.

2.1.0 redraw: closer to the weight of Path of Exile's currency -- metallic,
lit from the top left, a strong rim -- but every design here is original: no
GGG artwork is traced or reproduced, and none of their sculpted faces.

  transmute  pale blue faceted crystal
  augment    cobalt sphere with bright shard spikes
  alchemy    amber glass with bubbles
  exalt      faceted gold nugget with a small crown
  chaos      tarnished bronze with a gold rune ring
  scour      grey-white mottled stone
  divine     bronze with a rose-gold sheen and a halo
  vaal       lumpy blood-red metal, dark core
  regal      split down the middle, cobalt and gold, gold crown

Writes, for each orb (roll::Orb order, ESP FormID 0x801 + index):
  tools/orbs/master/<key>.png                                   16x16 master
  data/GridInventory_fallback/item/DiabloInSkyrim.esp_0x000801.png  128x128, nearest-neighbour
  data/textures/DiabloInSkyrim/orbs/<key>.dds                   128x128 uncompressed RGBA
  tools/orbs/sheet.png                                          every orb side by side (preview)

Deterministic (no randomness beyond a fixed hash). Run from anywhere:
  python tools/orbs/make_orbs.py   (needs Pillow)
"""
import math, os, struct
from PIL import Image

SIZE, SCALE = 16, 8
BLACK = (8, 6, 10, 255)
C = 7.5  # centre

def hash01(x, y, salt):
    """Stable per-pixel noise in [0, 1)."""
    h = (x * 374761393 + y * 668265263 + salt * 2147483647) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65536.0

def disc(r, cx=C, cy=C):
    return {(x, y) for y in range(SIZE) for x in range(SIZE)
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r}

def light(x, y, cx=C, cy=C, r=5.5):
    """0 (shadow) .. 1 (lit), a sphere lit from the top left."""
    nx, ny = (x - cx) / r, (y - cy) / r
    nz = math.sqrt(max(0.0, 1.0 - nx * nx - ny * ny))
    l = (-0.55 * nx - 0.6 * ny + 0.58 * nz)
    return max(0.0, min(0.999, 0.3 + 0.5 * l))

def ramp_pick(ramp, t):
    i = min(len(ramp) - 1, max(0, int(t * len(ramp))))
    return ramp[i] + (255,)

# ---------------------------------------------------------------- the orbs

def transmute():
    blue = [(20, 45, 110), (40, 95, 185), (95, 160, 235), (170, 215, 255), (235, 250, 255)]
    pts = {(x, y) for y in range(SIZE) for x in range(SIZE)
           if abs(x - C) * 1.0 + abs(y - C) * 0.82 <= 6.0}   # a cut stone, taller than wide
    def col(x, y):
        # facets: four quadrants, lit by which way they face
        q = (x >= C) * 1 + (y >= C) * 2
        base = [0.7, 0.45, 0.5, 0.2][q]
        if abs(x - C) < 1.0 or abs(y - C) < 0.9:
            base += 0.15                       # the ridge lines catch the light
        return ramp_pick(blue, base)
    hi = {(5, 4), (6, 4), (5, 5)}
    return pts, col, hi, blue

def augment():
    blue = [(10, 20, 80), (25, 55, 160), (55, 105, 225), (130, 175, 255), (220, 240, 255)]
    body = disc(4.6)
    spikes = {(7, 1), (8, 2), (2, 7), (3, 8), (13, 7), (12, 6), (7, 13), (8, 12), (3, 3), (4, 4),
              (12, 12), (11, 11), (12, 3), (11, 4), (3, 12), (4, 11)}
    pts = body | spikes
    def col(x, y):
        if (x, y) in spikes:
            return blue[4] + (255,) if (x + y) % 2 else blue[3] + (255,)
        return ramp_pick(blue, light(x, y, r=4.6))
    hi = {(6, 5), (5, 6)}
    return pts, col, hi, blue

def alchemy():
    amber = [(95, 40, 5), (175, 85, 15), (235, 140, 35), (255, 195, 90), (255, 240, 190)]
    pts = disc(5.6)
    bubbles = {(9, 10), (7, 11), (10, 8), (6, 9)}
    def col(x, y):
        if (x, y) in bubbles:
            return amber[4] + (255,)
        t = light(x, y)
        # glass: brighter rim on the shadow side
        if (x - C) ** 2 + (y - C) ** 2 > 4.6 ** 2 and x + y > 15:
            t = max(t, 0.55)
        return ramp_pick(amber, t + 0.08)
    hi = {(5, 4), (6, 4), (4, 5)}
    return pts, col, hi, amber

# Three spikes on a band: the crown Exalted and Regal both wear.
CROWN_LOW = {(5, 1), (5, 2), (7, 1), (8, 1), (7, 2), (8, 2), (10, 1), (10, 2),
             (5, 3), (6, 3), (7, 3), (8, 3), (9, 3), (10, 3)}

def exalt():
    gold = [(90, 60, 10), (165, 120, 30), (220, 175, 55), (250, 220, 110), (255, 248, 200)]
    pts = set()
    for y in range(SIZE):
        for x in range(SIZE):
            a = math.atan2(y - C, x - C)
            r = 5.2 + 0.6 * math.cos(6 * a)        # a faceted nugget
            if (x - C) ** 2 + (y - C - 0.8) ** 2 <= r * r and y >= 4:
                pts.add((x, y))
    crown = CROWN_LOW
    pts |= crown
    def col(x, y):
        if (x, y) in crown:
            return gold[4] + (255,) if y <= 2 else gold[2] + (255,)
        facet = 0.12 if int(math.atan2(y - C, x - C) * 3) % 2 else -0.08
        return ramp_pick(gold, light(x, y, cy=8.3) + facet)
    hi = {(5, 6), (6, 6)}
    return pts, col, hi, gold

def chaos():
    bronze = [(40, 25, 12), (80, 52, 25), (120, 85, 40), (160, 120, 60), (200, 165, 100)]
    pts = disc(5.6)
    def col(x, y):
        d = math.hypot(x - C, y - C)
        if 3.0 <= d <= 4.1:                                  # the rune ring, notched like glyphs
            return (255, 215, 90, 255) if (int(math.atan2(y - C, x - C) * 4) % 3) else (120, 85, 30, 255)
        t = light(x, y) - 0.12 * hash01(x, y, 3)            # tarnish
        return ramp_pick(bronze, t)
    hi = {(5, 4)}
    return pts, col, hi, bronze

def scour():
    stone = [(70, 70, 76), (120, 120, 128), (170, 170, 176), (215, 215, 220), (245, 245, 248)]
    pts = disc(5.6)
    def col(x, y):
        t = light(x, y) + 0.35 * (hash01(x // 2, y // 2, 9) - 0.5) + 0.15 * (hash01(x, y, 4) - 0.5)
        return ramp_pick(stone, t)
    hi = {(5, 4), (6, 4)}
    return pts, col, hi, stone

def divine():
    rose = [(60, 35, 25), (115, 75, 50), (170, 115, 85), (215, 150, 140), (250, 205, 210)]
    halo_c = (255, 240, 175, 255)
    body = disc(4.8, cy=8.5)
    halo = {(x, y) for y in range(SIZE) for x in range(SIZE)
            if 3.0 <= math.hypot((x - C) * 0.75, (y - 3.0) * 1.6) <= 3.9 and y <= 4}
    pts = body | halo
    def col(x, y):
        if (x, y) in halo and (x, y) not in body:
            return halo_c
        return ramp_pick(rose, light(x, y, cy=8.5, r=4.8) + 0.08 * (x < C))
    hi = {(6, 6)}
    return pts, col, hi, rose

def vaal():
    red = [(45, 5, 8), (110, 15, 18), (175, 30, 30), (225, 70, 60), (255, 160, 140)]
    pts = set()
    for y in range(SIZE):
        for x in range(SIZE):
            a = math.atan2(y - C, x - C)
            r = 5.1 + 0.75 * max(0.0, math.cos(3 * a + 0.9)) - 0.3 * max(0.0, math.cos(5 * a))
            if (x - C) ** 2 + (y - C) ** 2 <= r * r:
                pts.add((x, y))
    def col(x, y):
        d = math.hypot(x - C - 0.8, y - C - 0.8)
        dark = 0.3 if d < 1.8 else (0.15 if d < 2.8 else 0.0)   # the dark core, shading in
        return ramp_pick(red, light(x, y) - dark + 0.1 * (hash01(x, y, 7) - 0.5))
    hi = {(5, 4), (6, 4), (4, 5)}
    return pts, col, hi, red

def regal():
    blue = [(10, 25, 85), (30, 65, 170), (70, 120, 230), (150, 190, 255), (225, 240, 255)]
    gold = [(95, 65, 10), (170, 125, 30), (225, 180, 60), (250, 220, 115), (255, 248, 200)]
    body = disc(5.0, cy=8.6)
    crown = CROWN_LOW
    pts = body | crown
    def col(x, y):
        if (x, y) in crown:
            return gold[4] + (255,) if y <= 2 else gold[2] + (255,)
        t = light(x, y, cy=8.6, r=5.0)
        if x == 7 or x == 8:
            # the seam: a thin dark line between the halves
            if (x == 8 and y % 3 == 0) or (x == 7 and y % 3 == 1):
                return (20, 15, 25, 255)
        return ramp_pick(blue if x < 8 else gold, t)
    hi = {(5, 6), (5, 7)}
    return pts, col, hi, blue

ORBS = [  # roll::Orb order -- 0x801 + index
    ("transmute", transmute),
    ("augment", augment),
    ("alchemy", alchemy),
    ("exalt", exalt),
    ("chaos", chaos),
    ("scour", scour),
    ("divine", divine),
    ("vaal", vaal),
    ("regal", regal),
]

def make(builder):
    pts, col, hi, ramp = builder()
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    px = img.load()
    for (x, y) in pts:
        px[x, y] = col(x, y)
    for (x, y) in hi:
        if (x, y) in pts:
            px[x, y] = (255, 255, 255, 255)
    # ★THICK OUTLINE: two rings of black around the body (4- then 8-connected),
    # clipped to the canvas.
    ring1 = {(x + dx, y + dy) for (x, y) in pts for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))} - pts
    grown = pts | ring1
    ring2 = {(x + dx, y + dy) for (x, y) in grown
             for dx in (-1, 0, 1) for dy in (-1, 0, 1)} - grown
    for (x, y) in ring1 | ring2:
        if 0 <= x < SIZE and 0 <= y < SIZE:
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
    sheet = Image.new("RGBA", (len(ORBS) * (SIZE * SCALE + 8) + 8, SIZE * SCALE + 16), (24, 22, 28, 255))
    for i, (key, builder) in enumerate(ORBS):
        small = make(builder)
        small.save(os.path.join(master, key + ".png"))
        big = small.resize((SIZE * SCALE, SIZE * SCALE), Image.NEAREST)
        big.save(os.path.join(grid, "DiabloInSkyrim.esp_0x%06X.png" % (0x801 + i)))
        write_dds(os.path.join(dds, key + ".dds"), big)
        sheet.alpha_composite(big, (8 + i * (SIZE * SCALE + 8), 8))
        print("orb", key)
    sheet.save(os.path.join(root, "tools", "orbs", "sheet.png"))

if __name__ == "__main__":
    main()
