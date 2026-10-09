"""Procedural voxel sources for the five Pixel Room props.

Coordinates are VOX coordinates: x right, y depth, z up. The front of every
prop faces -y (VOX), which the importer maps to +z in engine space, toward
the default camera. All shapes are half-open boxes / discs painted in order,
later paints overwrite earlier ones; palette indices follow first use, so the
output is fully deterministic. Dimensions are chosen for the default import
scale of 1/16 world unit per voxel (see docs/art_style.md).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vox  # noqa: E402


class Model:
    def __init__(self, sx, sy, sz):
        self.size = (sx, sy, sz)
        self.cells = {}
        self.palette = []

    def color(self, rgb):
        rgba = (rgb[0], rgb[1], rgb[2], 255)
        if rgba not in self.palette:
            if len(self.palette) >= 255:
                raise ValueError("palette overflow")
            self.palette.append(rgba)
        return self.palette.index(rgba) + 1

    def set(self, x, y, z, rgb):
        sx, sy, sz = self.size
        if not (0 <= x < sx and 0 <= y < sy and 0 <= z < sz):
            raise ValueError("voxel (%d,%d,%d) outside %r" % (x, y, z, self.size))
        self.cells[(x, y, z)] = self.color(rgb)

    def box(self, x0, y0, z0, x1, y1, z1, rgb):
        for z in range(z0, z1):
            for y in range(y0, y1):
                for x in range(x0, x1):
                    self.set(x, y, z, rgb)

    def disc(self, cx, cy, r_in2, r_out2, z0, z1, rgb):
        """Cells whose center lies in r_in2 <= d^2 < r_out2 around (cx, cy)."""
        sx, sy, _sz = self.size
        for z in range(z0, z1):
            for y in range(sy):
                for x in range(sx):
                    d2 = (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2
                    if r_in2 <= d2 < r_out2:
                        self.set(x, y, z, rgb)

    def ball(self, cx, cy, cz, r2, rgb):
        sx, sy, sz = self.size
        for z in range(sz):
            for y in range(sy):
                for x in range(sx):
                    d2 = ((x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2
                          + (z + 0.5 - cz) ** 2)
                    if d2 < r2:
                        self.set(x, y, z, rgb)

    def to_vox(self):
        voxels = [(x, y, z, c) for (x, y, z), c in self.cells.items()]
        return vox.encode_vox(self.size, voxels, self.palette)


def make_desk():
    wood_top = (198, 150, 98)
    wood_edge = (160, 112, 70)
    wood_dark = (112, 76, 48)
    body = (146, 102, 64)
    drawer = (182, 134, 88)
    metal = (206, 206, 212)
    m = Model(64, 32, 35)
    # legs (the right pair is mostly hidden by the drawer cabinet)
    for lx, ly in ((2, 2), (58, 2), (2, 26), (58, 26)):
        m.box(lx, ly, 0, lx + 4, ly + 4, 31, wood_dark)
    # rails under the top and a modesty panel at the back
    m.box(6, 2, 27, 38, 4, 31, wood_dark)
    m.box(6, 28, 12, 38, 30, 31, wood_dark)
    # drawer cabinet on the right, two drawer fronts with a groove between
    m.box(38, 2, 3, 62, 30, 31, body)
    m.box(40, 1, 5, 60, 2, 16, drawer)
    m.box(40, 1, 18, 60, 2, 29, drawer)
    m.box(48, 0, 10, 52, 1, 11, metal)
    m.box(48, 0, 23, 52, 1, 24, metal)
    # table top: lipped edge, lighter inset surface
    m.box(0, 0, 31, 64, 32, 34, wood_edge)
    m.box(0, 0, 34, 64, 32, 35, wood_edge)
    m.box(1, 1, 34, 63, 31, 35, wood_top)
    return m


def make_chair():
    wood = (172, 122, 76)
    dark = (120, 80, 50)
    cush = (186, 62, 62)
    cush_dark = (140, 42, 48)
    m = Model(24, 24, 46)
    for lx, ly in ((2, 2), (19, 2), (2, 19), (19, 19)):
        m.box(lx, ly, 0, lx + 3, ly + 3, 20, dark)
    # stretchers between the legs
    m.box(5, 3, 8, 19, 4, 10, dark)
    m.box(5, 20, 8, 19, 21, 10, dark)
    m.box(3, 5, 8, 4, 19, 10, dark)
    m.box(20, 5, 8, 21, 19, 10, dark)
    # seat and cushion
    m.box(2, 2, 20, 22, 22, 24, wood)
    m.box(3, 3, 24, 21, 21, 25, cush_dark)
    m.box(3, 3, 25, 21, 21, 26, cush)
    # backrest: posts, top rail, three slats
    m.box(2, 19, 24, 5, 22, 46, dark)
    m.box(19, 19, 24, 22, 22, 46, dark)
    m.box(2, 19, 40, 22, 22, 46, wood)
    for sx0 in (7, 11, 15):
        m.box(sx0, 20, 26, sx0 + 2, 21, 40, wood)
    return m


def make_book():
    cover = (150, 40, 50)
    cover_dark = (108, 28, 38)
    pages = (240, 232, 210)
    shade = (212, 202, 176)
    gold = (230, 190, 60)
    ribbon = (60, 100, 170)
    m = Model(14, 10, 5)
    m.box(0, 0, 0, 14, 10, 1, cover_dark)
    m.box(1, 1, 1, 13, 9, 2, pages)
    m.box(1, 1, 2, 13, 9, 3, shade)
    m.box(1, 1, 3, 13, 9, 4, pages)
    m.box(0, 0, 4, 14, 10, 5, cover)
    m.box(0, 0, 0, 1, 10, 5, cover_dark)  # spine
    m.box(4, 3, 4, 11, 4, 5, gold)
    m.box(4, 6, 4, 11, 7, 5, gold)
    m.box(10, 0, 1, 11, 1, 4, ribbon)
    return m


def make_cup():
    ceramic = (240, 240, 234)
    stripe = (60, 110, 190)
    coffee = (92, 54, 32)
    m = Model(10, 8, 9)
    m.disc(4, 4, 0.0, 15.5, 0, 1, ceramic)
    m.disc(4, 4, 7.3, 15.5, 1, 9, ceramic)
    m.disc(4, 4, 7.3, 15.5, 3, 5, stripe)
    m.disc(4, 4, 0.0, 7.3, 1, 7, coffee)
    # handle on +x: a small vertical loop two voxels wide
    for y in (3, 4):
        for x in (8, 9):
            m.set(x, y, 6, ceramic)
            m.set(x, y, 2, ceramic)
        for z in (3, 4, 5):
            m.set(9, y, z, ceramic)
    return m


def _leaf(m, dx, dy, z0, length, leaf, tip):
    """A rising 2-wide leaf blade leaving the 2x2 stem (cells 8..9)."""
    low = z0
    for t in range(length):
        z = z0 + (t * 2) // 3
        color = tip if t >= length - 2 else leaf
        # each step also fills down to the previous height so the blade stays
        # face-connected (no diagonal-only steps)
        if dx and dy:
            x0 = 10 + t if dx > 0 else 6 - t
            y0 = 10 + t if dy > 0 else 6 - t
            m.box(x0, y0, low, x0 + 2, y0 + 2, z + 1, color)
            if t == 0:  # face-connect the blade to the stem
                m.set(10 if dx > 0 else 7, 9 if dy > 0 else 8, z, color)
        elif dx:
            x0 = 10 + t if dx > 0 else 7 - t
            m.box(x0, 8, low, x0 + 1, 10, z + 1, color)
        else:
            y0 = 10 + t if dy > 0 else 7 - t
            m.box(8, y0, low, 10, y0 + 1, z + 1, color)
        low = z


def make_plant():
    terra = (190, 92, 58)
    terra_dark = (150, 66, 42)
    rim = (214, 116, 76)
    soil = (70, 46, 32)
    stem = (52, 120, 56)
    leaf = (70, 160, 70)
    light = (120, 198, 98)
    flower = (240, 120, 160)
    center = (250, 222, 90)
    m = Model(18, 18, 40)
    cx = cy = 9.0
    m.disc(cx, cy, 0.0, 5.4 ** 2, 0, 1, terra_dark)
    for z in range(1, 10):
        r_out = 5.4 + 0.3 * z
        r_in = r_out - 1.6
        m.disc(cx, cy, r_in ** 2, r_out ** 2, z, z + 1,
               terra_dark if z in (4, 5) else terra)
        m.disc(cx, cy, 0.0, r_in ** 2, z, z + 1, soil)
    m.disc(cx, cy, 0.0, 7.0 ** 2, 10, 11, soil)
    m.disc(cx, cy, 7.0 ** 2, 8.7 ** 2, 10, 12, rim)
    m.box(8, 8, 11, 10, 10, 32, stem)
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        _leaf(m, dx, dy, 14, 7, leaf, light)
    for dx, dy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        _leaf(m, dx, dy, 22, 6, leaf, light)
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        _leaf(m, dx, dy, 28, 4, light, light)
    m.ball(9.0, 9.0, 34.0, 12.0, light)
    m.box(7, 8, 37, 11, 10, 38, flower)
    m.box(8, 7, 37, 10, 11, 38, flower)
    m.box(8, 8, 38, 10, 10, 40, center)
    return m


# name -> builder, in a fixed order
PROPS = (
    ("desk", make_desk),
    ("chair", make_chair),
    ("book", make_book),
    ("cup", make_cup),
    ("plant", make_plant),
)
