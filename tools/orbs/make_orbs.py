#!/usr/bin/env python3
"""Orb icons: original pixel art on a 64x64 grid, drawn at 2x so the files are 128x128,
with a thick (3 px on the grid, 6 px in the file) black outline and a transparent background.

Every design here is original. No Grid Games artwork is traced, copied or
reproduced, and the shapes are generic (sphere, crystal, nugget, crown), so
nothing here depends on their assets.

Each orb has its own embossed face, a different expression per orb:
  transmute  small pale blue crystal, serene closed eyes
  augment    cobalt sphere, bright ring, long and short faceted spikes, loose shards, furious
  alchemy    amber glass, four bubbles popping off it (one per modifier), startled
  exalt      faceted gold nugget under a crown, stern
  chaos      tarnished bronze with a gold rune rim, unhinged (mismatched eyes)
  scour      grey-white stone slashed right across, drowsy
  divine     gold with sapphire accents, a halo round the head, face tipped up as if ascending
  vaal       angular blood-red rock with bright bevelled edges, menacing
  regal      sphere split down the middle, cobalt and gold, gold crown, dignified

Writes, for each orb (roll::Orb order, ESP FormID 0x801 + index):
  tools/orbs/master/<key>.png                                       64x64 master
  data/GridInventory_fallback/item/DiabloInSkyrim.esp_0x000801.png  128x128, nearest-neighbour
  data/textures/DiabloInSkyrim/orbs/<key>.dds                       128x128 uncompressed RGBA
  tools/orbs/sheet.png                                              every orb side by side (preview)

Deterministic (a fixed hash, no randomness). Run from anywhere:
  python tools/orbs/make_orbs.py   (needs Pillow)
"""
import math, os, struct
from PIL import Image

N, SCALE = 64, 2
C = 31.5
BLACK = (8, 6, 10, 255)
OUTLINE = 3.4          # outline radius in grid pixels (6-7 px in the 128 px file)

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]

def bay(x, y):
    return (BAYER[y % 4][x % 4] + 0.5) / 16.0 - 0.5

def hash01(x, y, salt):
    h = (x * 374761393 + y * 668265263 + salt * 2147483647) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65536.0

def blocknoise(x, y, size, salt):
    """Value noise on a coarse grid, bilinear, 0..1."""
    gx, gy = x / size, y / size
    x0, y0 = int(math.floor(gx)), int(math.floor(gy))
    fx, fy = gx - x0, gy - y0
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a = hash01(x0, y0, salt); b = hash01(x0 + 1, y0, salt)
    c = hash01(x0, y0 + 1, salt); d = hash01(x0 + 1, y0 + 1, salt)
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy

def pick(ramp, t, x, y, dither=0.85):
    """Quantise t (0..1) to the ramp with an ordered dither, so shading reads as pixel art."""
    v = max(0.0, min(1.0, t)) * (len(ramp) - 1) + bay(x, y) * dither
    i = max(0, min(len(ramp) - 1, int(round(v))))
    return ramp[i] + (255,)

def sphere(x, y, cx, cy, r):
    nx, ny = (x - cx) / r, (y - cy) / r
    d = nx * nx + ny * ny
    if d > 1.0:
        return None
    return nx, ny, math.sqrt(1.0 - d)

def lit(nx, ny, nz):
    """0 shadow .. 1 lit: a key light from the top left, a weak bounce from the bottom right."""
    key = -0.52 * nx - 0.62 * ny + 0.60 * nz
    bounce = max(0.0, 0.55 * nx + 0.6 * ny - 0.2) * 0.35
    return 0.22 + 0.62 * key + bounce

def poly_contains(poly, x, y):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]; xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside

def seg_dist(x, y, a, b):
    ax, ay = a; bx, by = b
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(x - (ax + t * dx), y - (ay + t * dy))

def ellipse(x, y, cx, cy, rx, ry):
    return ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0

def ang(x, y, cx=C, cy=C):
    return math.atan2(y - cy, x - cx)

# ------------------------------------------------------------------ faces
# Every orb carries its own embossed face: eyes, brow, nose and mouth are drawn as
# raised and sunken shading on the surface (a lighting offset) or as dark hollows.
# The faces are original: a different expression and different proportions per orb.

class Face:
    def __init__(self, cx, cy, size, eyes="round", mouth="smile", brow=0.0, pupil=(255, 255, 255),
                 eye_w=0.17, eye_h=0.13, eye_dx=0.36, eye_dy=-0.12, nose=0.30, mouth_y=0.46,
                 mouth_w=0.40, asym=False, socket=(16, 8, 14), look=0.0):
        self.cx, self.cy, self.size = cx, cy, size
        self.eyes, self.mouth, self.brow = eyes, mouth, brow
        self.pupil = pupil + (255,)
        self.eye_w, self.eye_h, self.eye_dx, self.eye_dy = eye_w, eye_h, eye_dx, eye_dy
        self.nose, self.mouth_y, self.mouth_w, self.asym = nose, mouth_y, mouth_w, asym
        self.socket = socket + (255,)
        self.look = look                    # 0 level .. 1 eyes rolled upward

    def _eye(self, x, y, ex, ey, ew, eh, side):
        d = ((x - ex) / ew) ** 2 + ((y - ey) / eh) ** 2
        if self.eyes == "arc":                      # a closed, curved eye
            u = (x - ex) / ew
            if abs(u) <= 1.0 and abs(y - (ey - eh * 0.7 * (1 - u * u))) < 1.15:
                return 0.0, self.socket
            return None
        if self.eyes == "slit":
            d = ((x - ex) / (ew * 1.15)) ** 2 + ((y - ey) / (eh * 0.5)) ** 2
            if d <= 1.0:
                if abs(x - ex) < 1.2 and abs(y - ey) < eh * 0.5:
                    return 0.0, self.pupil
                return 0.0, self.socket
            if d <= 2.4:
                return (0.2 if y < ey else -0.2), None
            return None
        if d <= 1.0:
            if self.eyes == "half" and y < ey - eh * 0.15:
                return 0.0, self.socket              # the lid
            if (x - ex - side * 0.2) ** 2 / (ew * 0.55) ** 2 + (y - ey + 0.2 + self.look * eh * 0.7) ** 2 / (eh * 0.75) ** 2 <= 1.0:
                if abs(x - (ex - ew * 0.25)) < 0.8 and abs(y - (ey - eh * 0.35)) < 0.8:
                    return 0.0, (255, 255, 255, 255)   # glint
                return 0.0, self.pupil
            return 0.0, self.socket
        if d <= 2.1:                                 # the raised rim round the socket
            return (0.22 if (x < ex or y < ey) else -0.22), None
        return None

    def __call__(self, x, y):
        s = self.size
        # eyes
        for side in (-1, 1):
            big = 1.3 if (self.asym and side > 0) else 1.0
            ew, eh = self.eye_w * s * big, self.eye_h * s * big
            ex = self.cx + side * self.eye_dx * s
            ey = self.cy + self.eye_dy * s - (0.05 * s if (self.asym and side > 0) else 0.0)
            r = self._eye(x, y, ex, ey, ew, eh, side)
            if r:
                return r
            if self.brow is not None:                # brow ridge: a dark line, angled by mood
                eh0 = self.eye_h * s
                a = (ex - side * ew * 1.0, ey - eh0 * (2.1 - self.brow * 1.3))
                b = (ex + side * ew * 1.5, ey - eh0 * (2.1 + self.brow * 0.9))
                dd = seg_dist(x, y, a, b)
                if dd <= 1.15:
                    return 0.0, self.socket
                if dd <= 2.3 and y < min(a[1], b[1]) + 2:
                    return 0.12, None
        # nose: a raised ridge, lit on the left, shadowed on the right, two nostrils
        n0 = self.cy + (self.eye_dy + 0.04) * s
        n1 = self.cy + self.nose * s
        if n0 <= y <= n1 and abs(x - self.cx) <= 1.7:
            return (0.26 if x < self.cx else -0.3), None
        for sd in (-1, 1):
            if abs(x - (self.cx + sd * 0.1 * s)) < 0.8 and abs(y - (n1 + 0.5)) < 0.8:
                return 0.0, self.socket
        # mouth
        my = self.cy + self.mouth_y * s
        w = self.mouth_w * s
        u = (x - self.cx) / w
        if abs(u) <= 1.0:
            m = self.mouth
            if m == "smile" and abs(y - (my + 0.12 * s * (1 - u * u))) < 1.1:
                return 0.0, self.socket
            if m == "frown" and abs(y - (my - 0.1 * s * (1 - u * u) + 0.05 * s)) < 1.1:
                return 0.0, self.socket
            if m == "grim" and abs(y - my) < 1.1:
                return 0.0, self.socket
            if m == "crook" and abs(y - (my - u * 0.1 * s)) < 1.1:
                return 0.0, self.socket
            if m == "open":
                if ((x - self.cx) / (w * 0.55)) ** 2 + ((y - my) / (0.13 * s)) ** 2 <= 1.0:
                    return 0.0, (120, 20, 30, 255) if y > my else self.socket
            if m == "teeth":                         # a wide grin full of points
                top = my - 0.14 * s * (1 - u * u * 0.6)
                bot = my + 0.16 * s * (1 - u * u * 0.6)
                if top - 1.0 <= y <= bot + 1.0:
                    tooth = (int((x - self.cx) / 2.2) % 2 == 0)
                    if y <= top + 3.4 and tooth:
                        return 0.0, (245, 240, 225, 255)
                    if y >= bot - 3.2 and not tooth:
                        return 0.0, (245, 240, 225, 255)
                    return 0.0, self.socket
        return None

def apply_face(face, ramp, t, x, y):
    """Colour for a surface pixel at brightness t, with the face drawn over it."""
    r = face(x, y)
    if r:
        dt, col = r
        if col is not None:
            return col
        t += dt
    return pick(ramp, t, x, y)

def specular(x, y, spots):
    for (cx, cy, rx, ry) in spots:
        if ellipse(x, y, cx, cy, rx, ry):
            return (255, 255, 255, 255)
    return None

CROWN = [(18, 21), (17, 8), (25, 15), (32, 5), (39, 15), (47, 8), (46, 21)]
GOLD = [(70, 45, 8), (130, 88, 20), (190, 140, 40), (235, 190, 75), (255, 233, 140), (255, 252, 215)]
SAPPHIRE = [(8, 20, 80), (20, 50, 150), (40, 100, 225), (110, 170, 255), (200, 228, 255), (245, 252, 255)]

def crown_color(x, y):
    if (x - 32) ** 2 + (y - 16.5) ** 2 <= 2.6 ** 2:
        return (230, 60, 60, 255) if x + y < 49 else (150, 20, 35, 255)
    t = 0.55 + (32 - x) * 0.018 - (y - 5) * 0.012
    if 18.5 <= y <= 21:
        t -= 0.22
    return pick(GOLD, t, x, y)

def crown_has(x, y):
    return poly_contains(CROWN, x, y)

# ------------------------------------------------------------------ the orbs
# Each returns f(x, y) -> RGBA, or None where there is no body.

def transmute():
    # a smaller, paler crystal than the other orbs
    ramp = [(50, 98, 184), (104, 160, 236), (162, 206, 255), (212, 236, 255), (240, 250, 255), (255, 255, 255)]
    k = 0.78
    hexa = [(32 + (px_ - 32) * k, 32 + (py_ - 32) * k)
            for px_, py_ in [(32, 4), (53, 20), (53, 44), (32, 60), (11, 44), (11, 20)]]
    # serene: closed arched eyes, a small smile, no brow
    face = Face(32, 33, 12, eyes="arc", mouth="smile", brow=None, eye_w=0.2, eye_h=0.16,
                eye_dx=0.4, eye_dy=-0.08, nose=0.28, mouth_y=0.5, mouth_w=0.32, socket=(40, 80, 160))
    def f(x, y):
        if not poly_contains(hexa, x, y):
            return None
        a = ang(x, y, 32, 32)
        face_idx = (a + math.pi) / (2 * math.pi) * 6
        centre_a = (int(face_idx) + 0.5) / 6 * 2 * math.pi - math.pi
        base = 0.55 + 0.42 * math.cos(centre_a - (-2.3))
        if abs(face_idx - round(face_idx)) < 0.05:
            base += 0.18
        if math.hypot((x - 32) * 0.9, (y - 33) * 0.85) < 12.5:
            base = base * 0.3 + 0.66                     # a flat polished plate for the face
            fr = face(x, y)
            if fr:
                dt, col = fr
                return col if col is not None else pick(ramp, base + dt, x, y)
        s = specular(x, y, [(25, 19, 2.8, 2.0)])
        return s or pick(ramp, base, x, y)
    return f

def augment():
    ramp = [(8, 16, 70), (20, 44, 140), (46, 96, 218), (118, 168, 252), (205, 230, 255), (245, 250, 255)]
    R = 15.5
    # eight faceted spikes, long and short in turn, a bright energy ring round the body,
    # and four loose diamond shards floating between the long spikes
    spikes = [(k * math.pi / 4 + math.pi / 8, 29.5 if k % 2 == 0 else 23.5, 6.6 if k % 2 == 0 else 5.0)
              for k in range(8)]
    shards = [(C + 25.0 * math.cos(k * math.pi / 2), C + 25.0 * math.sin(k * math.pi / 2)) for k in range(4)]
    # furious: slanted brows, glowing white eyes, a toothy snarl
    face = Face(C, 33, 13.5, eyes="round", mouth="teeth", brow=1.0, pupil=(255, 255, 255),
                eye_w=0.19, eye_h=0.14, eye_dx=0.36, eye_dy=-0.12, nose=0.22, mouth_y=0.5, mouth_w=0.46,
                socket=(8, 14, 60))
    def f(x, y):
        d = math.hypot(x - C, y - C)
        sp = sphere(x, y, C, C, R)
        if sp:
            s = specular(x, y, [(25, 21, 2.8, 2.0)])
            return s or apply_face(face, ramp, lit(*sp), x, y)
        if d <= R + 2.3:                                               # the bright ring
            return pick(ramp, 0.95 - 0.5 * ((d - R) / 2.3) ** 2 * 0.3 - 0.15 * (x > C), x, y)
        dx, dy = x - C, y - C
        for a, length, width in spikes:
            ca, sa = math.cos(a), math.sin(a)
            along, perp = dx * ca + dy * sa, -dx * sa + dy * ca
            if R <= along <= length and abs(perp) <= width * (1 - (along - R) / (length - R)):
                lit_side = math.cos(a - (-2.3)) > 0                    # which half of the spike faces the light
                bright = (perp < 0) == lit_side
                t = 0.88 if bright else 0.3
                if abs(perp) < 0.9:
                    t = 0.98                                           # a bright ridge down the middle
                return pick(ramp, t, x, y)
        for sx, sy in shards:
            u, v = x - sx, y - sy
            if abs(u) * 0.8 + abs(v) <= 3.4:
                return pick(ramp, 0.95 if (u + v) < 0 else 0.55, x, y)
        return None
    return f

def alchemy():
    ramp = [(80, 30, 4), (150, 66, 10), (215, 118, 24), (250, 172, 60), (255, 218, 128), (255, 246, 200)]
    cy, R = 36.0, 21.5
    # four bubbles pop out of the orb, one per modifier, each a different size
    bubbles = [(34, 9, 3.6), (46, 11, 4.8), (55, 21, 4.0), (57, 33, 3.1)]
    # wide-eyed and startled: big round eyes, a small open mouth, brows up
    face = Face(C, 37, 19, eyes="round", mouth="open", brow=-0.5, pupil=(120, 50, 10),
                eye_w=0.2, eye_h=0.19, eye_dx=0.34, eye_dy=-0.1, nose=0.26, mouth_y=0.5, mouth_w=0.3,
                socket=(70, 24, 4))
    def f(x, y):
        for bx, by, br in bubbles:
            d = math.hypot(x - bx, y - by)
            if d <= br:
                if d > br - 1.3:
                    return (255, 246, 205, 255)                       # the bubble's bright rim
                if (x - bx) < -br * 0.2 and (y - by) < -br * 0.2 and d < br * 0.7:
                    return (255, 255, 255, 255)                       # glint
                return pick(ramp, 0.7 - 0.12 * ((y - by) / br), x, y)
        sp = sphere(x, y, C, cy, R)
        if not sp:
            return None
        nx, ny, nz = sp
        t = lit(*sp) + 0.08
        if nz < 0.42 and nx + ny > 0.2:
            t = max(t, 0.62)
        s = specular(x, y, [(21, 24, 4.2, 2.8)])
        return s or apply_face(face, ramp, t, x, y)
    return f

def exalt():
    cy = 38
    # stern and proud: slit eyes, level brow, a straight mouth, a long nose
    face = Face(C, 40, 17, eyes="slit", mouth="grim", brow=0.35, pupil=(255, 250, 200),
                eye_w=0.2, eye_h=0.16, eye_dx=0.36, eye_dy=-0.12, nose=0.34, mouth_y=0.5, mouth_w=0.3,
                socket=(60, 38, 6))
    def f(x, y):
        if crown_has(x, y):
            return crown_color(x, y)
        a = ang(x, y, C, cy)
        r = 20.5 + 2.4 * math.cos(6 * a)
        if (x - C) ** 2 + (y - cy) ** 2 > r * r:
            return None
        face_idx = (a + math.pi) / (2 * math.pi) * 6
        facet = 0.1 if int(face_idx) % 2 else -0.07
        edge = 0.22 if abs(face_idx - round(face_idx)) < 0.05 else 0.0
        sp = sphere(x, y, C, cy, 23) or (0, 0, 0.3)
        t = lit(*sp) + 0.05
        if math.hypot(x - C, y - 40) < 15.5:
            facet = edge = 0.0
        else:
            t += facet + edge
        s = specular(x, y, [(22, 31, 3.2, 2.2)])
        return s or apply_face(face, GOLD, t, x, y)
    return f

def chaos():
    ramp = [(34, 20, 10), (74, 46, 22), (116, 80, 38), (160, 118, 58), (205, 165, 100), (240, 210, 150)]
    # unhinged: one big eye and one small, a crooked grin, brows at odds
    face = Face(C, 33, 14.5, eyes="round", mouth="crook", brow=0.5, pupil=(255, 225, 110),
                eye_w=0.18, eye_h=0.14, eye_dx=0.38, eye_dy=-0.1, nose=0.3, mouth_y=0.52, mouth_w=0.5,
                asym=True, socket=(28, 14, 6))
    def f(x, y):
        sp = sphere(x, y, C, C, 23)
        if not sp:
            return None
        d, a = math.hypot(x - C, y - C), ang(x, y)
        tarnish = 0.16 * (blocknoise(x, y, 5, 3) - 0.5) * 2
        if 18.0 <= d <= 22.0:                                          # rune ring round the rim
            sector = (a + math.pi) / (2 * math.pi) * 14
            if (sector % 1.0) < 0.62:
                return pick(GOLD, 0.62 + 0.25 * (hash01(int(sector), 0, 5) - 0.5) + (0.1 if d < 19.5 else -0.05), x, y)
            return pick(ramp, 0.12 + tarnish, x, y)
        s = specular(x, y, [(21, 19, 3.0, 2.0)])
        return s or apply_face(face, ramp, lit(*sp) + tarnish, x, y)
    return f

def scour():
    ramp = [(62, 62, 70), (108, 108, 118), (156, 156, 166), (200, 200, 208), (232, 232, 238), (252, 252, 254)]
    # the slashed stone: a long cut right across it, with branches and a few edge chips
    cracks = [((7, 30), (20, 25)), ((20, 25), (34, 20)), ((34, 20), (46, 18)), ((46, 18), (57, 11)),
              ((20, 25), (17, 17)), ((34, 20), (36, 13)), ((46, 18), (52, 24)),
              ((12, 43), (24, 49)), ((24, 49), (36, 53)), ((24, 49), (23, 56))]
    # drowsy and flat: half-lidded eyes, no brow, a blank line for a mouth
    face = Face(C, 33, 17.5, eyes="half", mouth="grim", brow=None, pupil=(40, 40, 52),
                eye_w=0.2, eye_h=0.15, eye_dx=0.36, eye_dy=-0.08, nose=0.28, mouth_y=0.5, mouth_w=0.26,
                socket=(26, 26, 34))
    def f(x, y):
        sp = sphere(x, y, C, C, 23)
        if not sp:
            return None
        fr = face(x, y)
        if fr and fr[1] is not None:                                   # eyes and mouth stay on top of the cuts
            return fr[1]
        dist = min(seg_dist(x, y, a, b) for a, b in cracks) if math.hypot(x - C, y - 33) > 1.0 else 99
        if dist < 1.5:
            return (30, 30, 38, 255)
        if dist < 2.7:
            return (88, 88, 98, 255) if (x + y) % 2 == 0 else (130, 130, 142, 255)   # the broken lip of the cut
        t = lit(*sp) + 0.22 * (blocknoise(x, y, 6, 9) - 0.5) + 0.1 * (hash01(x, y, 4) - 0.5) + 0.08
        if fr:
            t += fr[0]
        s = specular(x, y, [(21, 18, 3.4, 2.2)])
        return s or pick(ramp, t, x, y)
    return f

def divine():
    cy = 41
    # ascending: the head tipped back, eyes rolled upward, a soft smile, brows raised;
    # gold with sapphire accents: sapphire eyes, a gem set in the halo, a blue halo edge
    face = Face(C, 39, 17, eyes="round", mouth="smile", brow=-0.4, pupil=(60, 120, 240),
                eye_w=0.18, eye_h=0.15, eye_dx=0.36, eye_dy=-0.08, nose=0.17, mouth_y=0.38, mouth_w=0.26,
                socket=(60, 40, 8), look=1.0)
    HY = 25.0
    def halo(x, y):
        out = ((x - C) / 21.0) ** 2 + ((y - HY) / 6.8) ** 2
        inn = ((x - C) / 17.2) ** 2 + ((y - HY) / 3.7) ** 2
        if out > 1.0 or inn < 1.0:
            return None
        if inn < 1.4:
            return (90, 160, 255, 255)                               # sapphire inner edge
        return pick(GOLD, 0.9 - 0.06 * (y - HY) + 0.05 * (x < C), x, y)
    def f(x, y):
        h = halo(x, y)
        sp = sphere(x, y, C, cy, 20.5)
        if (x - C) ** 2 + (y - 30.0) ** 2 <= 3.5 ** 2:                # the gem set in the halo
            return (235, 250, 255, 255) if (x < C - 0.6 and y < 29) else pick(SAPPHIRE, 0.62 - (y - 28) * 0.07 + (x < C) * 0.12, x, y)
        if h and (sp is None or y >= HY):                            # the back arc hides behind the head
            return h
        if sp is None:
            return None
        d = math.hypot(x - C, y - cy)
        if 18.0 <= d <= 20.5 and y > cy:
            if int((ang(x, y, C, cy) + math.pi) / (2 * math.pi) * 18) % 2 == 0:
                return pick(SAPPHIRE, 0.55, x, y)
        s = specular(x, y, [(23, 36, 3.2, 2.2)])
        return s or apply_face(face, GOLD, lit(*sp) + 0.05, x, y)
    return f

def vaal():
    ramp = [(40, 4, 8), (100, 12, 16), (166, 26, 28), (222, 64, 56), (252, 128, 108), (255, 214, 196)]
    # a rock: an angular outline with flat chipped facets and bright ridge edges
    nv = 13
    radii = [24.5, 20.0, 23.0, 17.5, 22.5, 24.5, 18.5, 23.5, 19.0, 24.0, 17.8, 22.0, 20.5]
    verts = [(C + radii[i] * math.cos(i * 2 * math.pi / nv - 1.9),
              C + radii[i] * math.sin(i * 2 * math.pi / nv - 1.9)) for i in range(nv)]
    # menacing: hollow glowing eyes under a heavy brow, a jagged grin
    face = Face(C, 33, 16.5, eyes="round", mouth="teeth", brow=0.9, pupil=(255, 190, 70),
                eye_w=0.2, eye_h=0.15, eye_dx=0.36, eye_dy=-0.1, nose=0.26, mouth_y=0.52, mouth_w=0.46,
                socket=(24, 2, 6))
    def f(x, y):
        if not poly_contains(verts, x, y):
            return None
        a = (ang(x, y) + 1.9) % (2 * math.pi)
        k = a / (2 * math.pi) * nv
        i = int(k)
        mid = (i + 0.5) / nv * 2 * math.pi - 1.9
        base = 0.45 + 0.4 * math.cos(mid - (-2.3))                    # each facet is one flat shade
        edge = abs(k - round(k))
        d = math.hypot(x - C, y - C)
        # distance from the polygon edge decides the bevel: a bright lip on the lit side, dark on the far side
        ex = min(seg_dist(x, y, verts[j], verts[(j + 1) % nv]) for j in range(nv))
        if ex < 2.3:
            base += 0.28 if math.cos(a - 1.9 - (-2.3)) > 0 else -0.12
        if edge < 0.04 and d > 11:
            base += 0.2                                               # ridge lines from the centre
        # chips and cracks
        for (a1, b1) in (((20, 40), (25, 47)), ((44, 22), (48, 27)), ((38, 48), (43, 45))):
            if seg_dist(x, y, a1, b1) < 0.9:
                return (30, 2, 6, 255)
        t = base + 0.12 * (blocknoise(x, y, 4, 7) - 0.5)
        if d < 15.5:
            t = 0.52 + 0.1 * (blocknoise(x, y, 4, 7) - 0.5)           # a flatter plate behind the face
            return apply_face(face, ramp, t, x, y)
        return pick(ramp, t, x, y)
    return f

def regal():
    blue = [(10, 24, 84), (28, 62, 166), (66, 118, 228), (146, 188, 255), (212, 234, 255), (246, 252, 255)]
    cy = 40
    # dignified: calm level eyes (one blue, one gold), a firm brow, a closed mouth
    face = Face(C, 42, 17, eyes="round", mouth="grim", brow=0.2, pupil=(255, 255, 255),
                eye_w=0.18, eye_h=0.14, eye_dx=0.36, eye_dy=-0.1, nose=0.28, mouth_y=0.5, mouth_w=0.28,
                socket=(24, 18, 30))
    def f(x, y):
        if crown_has(x, y):
            return crown_color(x, y)
        sp = sphere(x, y, C, cy, 21.0)
        if not sp:
            return None
        seam = x - 31.5 + 0.8 * math.sin(y * 0.5)
        ramp = blue if seam < 0 else GOLD
        r = face(x, y)
        if r:
            dt, col = r
            if col is not None:
                if col == (255, 255, 255, 255) or col == face.pupil:   # pupils take the half's colour
                    return pick(SAPPHIRE if seam < 0 else GOLD, 0.8, x, y)
                return col
            return pick(ramp, lit(*sp) + dt + 0.05, x, y)
        if abs(seam) < 1.1:
            return (22, 16, 28, 255)
        s = specular(x, y, [(23, 33, 3.2, 2.2)] if seam < 0 else [(40, 33, 2.0, 2.0)])
        return s or pick(ramp, lit(*sp) + 0.05, x, y)
    return f

ORBS = [  # roll::Orb order -- 0x801 + index
    ("transmute", transmute), ("augment", augment), ("alchemy", alchemy),
    ("exalt", exalt), ("chaos", chaos), ("scour", scour),
    ("divine", divine), ("vaal", vaal), ("regal", regal),
]

def make(builder):
    f = builder()
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    px = img.load()
    body = set()
    for y in range(N):
        for x in range(N):
            c = f(x, y)
            if c is not None:
                px[x, y] = c
                body.add((x, y))
    # thick outline: every empty pixel within OUTLINE of the body turns black
    reach = int(math.ceil(OUTLINE))
    offs = [(dx, dy) for dx in range(-reach, reach + 1) for dy in range(-reach, reach + 1)
            if dx * dx + dy * dy <= OUTLINE * OUTLINE]
    for (x, y) in list(body):
        for dx, dy in offs:
            xx, yy = x + dx, y + dy
            if 0 <= xx < N and 0 <= yy < N and (xx, yy) not in body:
                px[xx, yy] = BLACK
    return img

def write_dds(path, img):
    w, h = img.size
    header = struct.pack("<4s7I44x", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x8, h, w, w * 4, 0, 1)
    pf = struct.pack("<2I4s5I", 32, 0x41, b"\0\0\0\0", 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    caps = struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    raw = img.tobytes()
    data = bytearray(len(raw))
    data[0::4], data[1::4], data[2::4], data[3::4] = raw[2::4], raw[1::4], raw[0::4], raw[3::4]
    with open(path, "wb") as fh:
        fh.write(header + pf + caps + bytes(data))

def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    master = os.path.join(root, "tools", "orbs", "master")
    grid = os.path.join(root, "data", "GridInventory_fallback", "item")
    dds = os.path.join(root, "data", "textures", "DiabloInSkyrim", "orbs")
    for d in (master, grid, dds):
        os.makedirs(d, exist_ok=True)
    cell = N * SCALE
    sheet = Image.new("RGBA", (len(ORBS) * (cell + 8) + 8, cell + 16), (24, 22, 28, 255))
    for i, (key, builder) in enumerate(ORBS):
        small = make(builder)
        small.save(os.path.join(master, key + ".png"))
        big = small.resize((cell, cell), Image.NEAREST)
        big.save(os.path.join(grid, "DiabloInSkyrim.esp_0x%06X.png" % (0x801 + i)))
        write_dds(os.path.join(dds, key + ".dds"), big)
        sheet.alpha_composite(big, (8 + i * (cell + 8), 8))
        print("orb", key)
    sheet.save(os.path.join(root, "tools", "orbs", "sheet.png"))

if __name__ == "__main__":
    main()
