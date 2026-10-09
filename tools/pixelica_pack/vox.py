"""MagicaVoxel .vox reader, deterministic mesher and minimal .vox writer.

Scope (see docs/asset_format.md): one model. A file is
    'VOX ' version(150|200), MAIN { [PACK=1] SIZE XYZI [RGBA] }
and anything else (scene graph, layers, materials, multiple models, unknown
chunks) is rejected with an explicit "unsupported" error instead of being
silently mangled.

Coordinates: VOX is right handed with +z up. The importer maps
VOX (x, y, z) -> engine (x, z, -y), which is a pure rotation (determinant +1),
so right-handedness and counter-clockwise outward faces are preserved.
"""

import hashlib
import os
import re
import struct
import sys

# Allow `python -I tools/pixelica_pack/vox.py ...` (isolated mode drops the script dir).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pixelica_format  # noqa: E402
from pixelica_format import PixelicaPackError, MeshData, rgb565  # noqa: E402

SUPPORTED_VERSIONS = (150, 200)
MAX_DIM = 128
MAX_VOXELS = 1000000
DEFAULT_SCALE = 1.0 / 16.0
PIVOTS = ("floor", "center")

_HINTS = {
    b"nTRN": "scene graph transform",
    b"nGRP": "scene graph group",
    b"nSHP": "scene graph shape",
    b"LAYR": "layers",
    b"MATL": "materials",
    b"MATT": "materials",
    b"rOBJ": "render object settings",
    b"rCAM": "render camera settings",
    b"NOTE": "color notes",
    b"IMAP": "palette index map",
}
_RANK = {b"PACK": 0, b"SIZE": 1, b"XYZI": 2, b"RGBA": 3}


# --------------------------------------------------------------------------
# Default palette
# --------------------------------------------------------------------------

def _build_default_palette():
    """The MagicaVoxel default palette as (r,g,b,a) for color indices 0..255.

    Layout (author table, 0xAABBGGRR): index 0 is empty; indices 1..215 are
    the 6x6x6 cube (R slowest, B fastest, each stepping 0xff,0xcc,0x99,0x66,
    0x33,0x00) without its final black entry 0xff000000, i.e. 215 colors;
    indices 216..255 are four 10-step ramps (red, green, blue, gray) of
    0xee,0xdd,0xbb,0xaa,0x88,0x77,0x55,0x44,0x22,0x11. That makes exactly 256
    entries, ending with gray 0xff111111 at index 255.

    Verify against the author's reference with
    `python tools/pixelica_pack/vox.py --check-default-palette MagicaVoxel-file-format-vox.txt`.
    """
    slots = []
    for k in range(215):  # the author table omits the cube's black corner
        r = 0xFF - 0x33 * (k // 36)
        g = 0xFF - 0x33 * ((k // 6) % 6)
        b = 0xFF - 0x33 * (k % 6)
        slots.append(0xFF000000 | (b << 16) | (g << 8) | r)
    ramp = (0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11)
    for shift in (0, 8, 16):  # red, green, blue
        slots.extend(0xFF000000 | (v << shift) for v in ramp)
    slots.extend(0xFF000000 | (v * 0x010101) for v in ramp)
    table = [0x00000000] + slots
    assert len(table) == 256
    return tuple((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF)
                 for v in table)


DEFAULT_PALETTE = _build_default_palette()


def default_palette_words():
    """The default palette as the author's 0xAABBGGRR words."""
    return [(a << 24) | (b << 16) | (g << 8) | r
            for (r, g, b, a) in DEFAULT_PALETTE]


def check_default_palette(reference_text):
    """Compare DEFAULT_PALETTE with the `default_palette` table in the
    author's MagicaVoxel-file-format-vox.txt. Returns a list of mismatch
    descriptions (empty means identical)."""
    start = reference_text.find("default_palette")
    if start < 0:
        return ["reference text has no 'default_palette' table"]
    end = reference_text.find("};", start)
    body = reference_text[start:end if end >= 0 else len(reference_text)]
    words = [int(w, 16) for w in re.findall(r"0x([0-9a-fA-F]{8})", body)]
    ours = default_palette_words()
    problems = []
    if len(words) != 256:
        problems.append("reference table has %d entries, expected 256"
                        % len(words))
    for i, (theirs, mine) in enumerate(zip(words, ours)):
        if theirs != mine:
            problems.append("index %d: reference 0x%08x, pixelica_pack 0x%08x"
                            % (i, theirs, mine))
    return problems


# --------------------------------------------------------------------------
# Parsing
# --------------------------------------------------------------------------

class VoxModel:
    """size: (sx, sy, sz); voxels: list of (x, y, z, color_index) in file
    order; palette: 256 (r,g,b,a) indexed by color index; custom_palette:
    whether an RGBA chunk was present."""

    def __init__(self, size, voxels, palette, custom_palette):
        self.size = size
        self.voxels = voxels
        self.palette = palette
        self.custom_palette = custom_palette


def _name(tag):
    return pixelica_format._tag(tag)


def parse_vox(data):
    data = bytes(data)
    size = len(data)
    if size > pixelica_format.MAX_FILE_BYTES:
        raise PixelicaPackError("VOX: file is %d bytes, over the %d byte limit"
                          % (size, pixelica_format.MAX_FILE_BYTES))
    if size < 8:
        raise PixelicaPackError("VOX: truncated header")
    if data[:4] != b"VOX ":
        raise PixelicaPackError("VOX: bad magic %s (not a MagicaVoxel file)"
                          % _name(data[:4]))
    (version,) = struct.unpack_from("<i", data, 4)
    if version not in SUPPORTED_VERSIONS:
        raise PixelicaPackError("VOX: unsupported version %d (supported: %s)"
                          % (version, ", ".join(map(str, SUPPORTED_VERSIONS))))
    pos = 8
    if size - pos < 12:
        raise PixelicaPackError("VOX: truncated, missing MAIN chunk")
    tag, content, children = struct.unpack_from("<4sii", data, pos)
    if tag != b"MAIN":
        raise PixelicaPackError("VOX: first chunk is %s, expected 'MAIN'" % _name(tag))
    if content != 0:
        raise PixelicaPackError("VOX: MAIN chunk must have no content bytes")
    pos += 12
    if children != size - pos:
        raise PixelicaPackError(
            "VOX: MAIN declares %d child bytes but the file has %d"
            % (children, size - pos))

    vox_size = None
    voxel_bytes = None
    voxel_count = 0
    palette = None
    last_rank = -1
    while pos < size:
        if size - pos < 12:
            raise PixelicaPackError("VOX: truncated chunk header at byte %d" % pos)
        tag, content, kids = struct.unpack_from("<4sii", data, pos)
        pos += 12
        if tag not in _RANK:
            what = _HINTS.get(tag, "unknown chunk")
            raise PixelicaPackError(
                "VOX: unsupported chunk %s (%s); pixelica_pack imports a single model "
                "with SIZE, XYZI and optional PACK/RGBA chunks only"
                % (_name(tag), what))
        if content < 0 or kids < 0 or content > size - pos:
            raise PixelicaPackError("VOX: chunk %s has invalid lengths "
                              "(content %d, children %d) for %d remaining bytes"
                              % (_name(tag), content, kids, size - pos))
        if kids != 0:
            raise PixelicaPackError("VOX: chunk %s has child chunks, unsupported"
                              % _name(tag))
        rank = _RANK[tag]
        if rank == last_rank:
            raise PixelicaPackError("VOX: duplicate %s chunk (multiple models are "
                              "unsupported)" % _name(tag))
        if rank < last_rank:
            raise PixelicaPackError("VOX: chunk %s is out of order (expected "
                              "[PACK] SIZE XYZI [RGBA])" % _name(tag))
        last_rank = rank
        body = data[pos:pos + content]
        pos += content

        if tag == b"PACK":
            if content != 4:
                raise PixelicaPackError("VOX: PACK content is %d bytes, expected 4"
                                  % content)
            (models,) = struct.unpack("<i", body)
            if models != 1:
                raise PixelicaPackError("VOX: PACK declares %d models; only 1 is "
                                  "supported" % models)
        elif tag == b"SIZE":
            if content != 12:
                raise PixelicaPackError("VOX: SIZE content is %d bytes, expected 12"
                                  % content)
            vox_size = struct.unpack("<iii", body)
            for axis, value in zip("xyz", vox_size):
                if not 1 <= value <= MAX_DIM:
                    raise PixelicaPackError("VOX: SIZE %s = %d outside 1..%d"
                                      % (axis, value, MAX_DIM))
        elif tag == b"XYZI":
            if vox_size is None:
                raise PixelicaPackError("VOX: XYZI before SIZE")
            if content < 4:
                raise PixelicaPackError("VOX: XYZI too short for its count")
            (voxel_count,) = struct.unpack_from("<i", body, 0)
            if voxel_count < 0 or voxel_count > MAX_VOXELS:
                raise PixelicaPackError("VOX: voxel count %d outside 0..%d"
                                  % (voxel_count, MAX_VOXELS))
            if voxel_count == 0:
                raise PixelicaPackError("VOX: model is empty (no voxels)")
            if content != 4 + 4 * voxel_count:
                raise PixelicaPackError("VOX: XYZI content is %d bytes, expected %d "
                                  "for %d voxels"
                                  % (content, 4 + 4 * voxel_count, voxel_count))
            voxel_bytes = body[4:]
        else:  # RGBA
            if content != 1024:
                raise PixelicaPackError("VOX: RGBA content is %d bytes, expected 1024"
                                  % content)
            entries = [tuple(body[i:i + 4]) for i in range(0, 1024, 4)]
            palette = ((0, 0, 0, 0),) + tuple(entries[:255])

    if vox_size is None:
        raise PixelicaPackError("VOX: missing SIZE chunk")
    if voxel_bytes is None:
        raise PixelicaPackError("VOX: missing XYZI chunk")

    sx, sy, sz = vox_size
    seen = bytearray(sx * sy * sz)
    voxels = []
    for off in range(0, 4 * voxel_count, 4):
        x, y, z, c = voxel_bytes[off:off + 4]
        if x >= sx or y >= sy or z >= sz:
            raise PixelicaPackError("VOX: voxel (%d,%d,%d) outside SIZE %dx%dx%d"
                              % (x, y, z, sx, sy, sz))
        if c == 0:
            raise PixelicaPackError("VOX: voxel (%d,%d,%d) uses color index 0 "
                              "(reserved for empty)" % (x, y, z))
        cell = (z * sy + y) * sx + x
        if seen[cell]:
            raise PixelicaPackError("VOX: duplicate voxel at (%d,%d,%d)" % (x, y, z))
        seen[cell] = 1
        voxels.append((x, y, z, c))

    custom = palette is not None
    if palette is None:
        palette = DEFAULT_PALETTE
    for c in sorted({v[3] for v in voxels}):
        if palette[c][3] != 255:
            raise PixelicaPackError(
                "VOX: used color index %d has alpha %d; the engine renders "
                "opaque colors only" % (c, palette[c][3]))
    return VoxModel(vox_size, voxels, palette, custom)


# --------------------------------------------------------------------------
# Meshing
# --------------------------------------------------------------------------

# (axis, sign) in fixed order: +X -X +Y -Y +Z -Z of the engine grid.
_DIRECTIONS = ((0, 1), (0, -1), (1, 1), (1, -1), (2, 1), (2, -1))


def build_mesh(model, scale=DEFAULT_SCALE, pivot="floor"):
    """Voxels -> MeshData (positions in engine space, scaled).

    * engine grid cell = (x, z, sy-1-y); dimensions (sx, sz, sy)
    * every face shared with another voxel is removed, whatever the colors
    * remaining faces are greedily merged per direction, plane and original
      palette index (never by quantized RGB565 value)
    * vertices are shared by lattice position, numbered by first use
    * quads are emitted counter-clockwise seen from outside
    """
    try:
        scale = float(scale)
    except (TypeError, ValueError):
        raise PixelicaPackError("scale must be a number")
    if not (scale > 0.0 and scale < float("inf")):
        raise PixelicaPackError("scale must be a positive finite number, got %r" % scale)
    if pivot not in PIVOTS:
        raise PixelicaPackError("pivot must be one of %s" % ", ".join(PIVOTS))

    sx, sy, sz = model.size
    dims = (sx, sz, sy)
    nx, ny, nz = dims
    stride = (1, nx, nx * ny)
    grid = bytearray(nx * ny * nz)
    for x, y, z, c in model.voxels:
        grid[((sy - 1 - y) * ny + z) * nx + x] = c

    # faces[(direction, plane)][(v, u)] = palette index
    faces = {}
    for k in range(nz):
        for j in range(ny):
            base = (k * ny + j) * nx
            for i in range(nx):
                c = grid[base + i]
                if not c:
                    continue
                cell = (i, j, k)
                for d, (axis, sign) in enumerate(_DIRECTIONS):
                    n = cell[axis] + sign
                    if 0 <= n < dims[axis] and grid[base + i + sign * stride[axis]]:
                        continue
                    plane = cell[axis] + (1 if sign > 0 else 0)
                    ua = (axis + 1) % 3
                    va = (axis + 2) % 3
                    faces.setdefault((d, plane), {})[(cell[va], cell[ua])] = c

    ox = sx / 2.0
    oz = sy / 2.0
    oy = 0.0 if pivot == "floor" else sz / 2.0

    vertex_ids = {}
    lattice = []
    indices = []
    colors = []

    def vertex(point):
        vid = vertex_ids.get(point)
        if vid is None:
            vid = len(lattice)
            if vid >= pixelica_format.MAX_VERTICES:
                raise PixelicaPackError("model needs more than %d vertices; split "
                                  "it or reduce its detail"
                                  % pixelica_format.MAX_VERTICES)
            vertex_ids[point] = vid
            lattice.append(point)
        return vid

    for key in sorted(faces):
        d, plane = key
        axis, sign = _DIRECTIONS[d]
        ua = (axis + 1) % 3
        va = (axis + 2) % 3
        mask = faces[key]
        for vu in sorted(mask):
            color = mask.get(vu)
            if color is None:
                continue
            v0, u0 = vu
            w = 1
            while mask.get((v0, u0 + w)) == color:
                w += 1
            h = 1
            while all(mask.get((v0 + h, u0 + t)) == color for t in range(w)):
                h += 1
            for dv in range(h):
                for du in range(w):
                    del mask[(v0 + dv, u0 + du)]

            def corner(du, dv):
                p = [0, 0, 0]
                p[axis] = plane
                p[ua] = u0 + du
                p[va] = v0 + dv
                return vertex(tuple(p))

            if sign > 0:
                quad = (corner(0, 0), corner(w, 0), corner(w, h), corner(0, h))
            else:
                quad = (corner(0, 0), corner(0, h), corner(w, h), corner(w, 0))
            if len(indices) + 6 > pixelica_format.MAX_INDICES:
                raise PixelicaPackError(
                    "model produces more than %d triangles (limit %d indices); "
                    "split it or reduce its detail"
                    % (pixelica_format.MAX_INDICES // 3, pixelica_format.MAX_INDICES))
            indices.extend((quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]))
            rgb = model.palette[color]
            packed = rgb565(rgb[0], rgb[1], rgb[2])
            colors.extend((packed, packed))

    positions = [((i - ox) * scale, (j - oy) * scale, (k - oz) * scale)
                 for (i, j, k) in lattice]
    return MeshData(positions, indices, colors)


def import_vox(data, scale=DEFAULT_SCALE, pivot="floor"):
    """VOX bytes -> (.p3mesh bytes, MeshData)."""
    mesh = build_mesh(parse_vox(data), scale, pivot)
    return pixelica_format.encode_mesh(mesh), mesh


def vox_to_p3mesh(data, scale=DEFAULT_SCALE, pivot="floor"):
    return import_vox(data, scale, pivot)[0]


def describe_vox(data):
    """JSON-serializable summary of a validated VOX file."""
    model = parse_vox(data)
    return {
        "format": "VOX",
        "file_size": len(data),
        "sha256": hashlib.sha256(bytes(data)).hexdigest(),
        "version": struct.unpack_from("<i", bytes(data), 4)[0],
        "size": list(model.size),
        "voxel_count": len(model.voxels),
        "palette": "custom" if model.custom_palette else "default",
        "used_colors": len({v[3] for v in model.voxels}),
    }


# --------------------------------------------------------------------------
# Writer (used by the demo generator and tests)
# --------------------------------------------------------------------------

def _chunk(tag, content, children=b""):
    return struct.pack("<4sii", tag, len(content), len(children)) + content + children


def encode_vox(size, voxels, palette=None):
    """Serialize a single-model VOX150 file, deterministically.

    voxels: iterable of (x, y, z, color_index); written sorted by (z, y, x).
    palette: optional sequence of (r,g,b,a) for color indices 1..N (N<=255);
    omit to rely on the default palette (no RGBA chunk).
    """
    sx, sy, sz = size
    if not all(1 <= v <= MAX_DIM for v in size):
        raise PixelicaPackError("VOX size %r outside 1..%d" % (tuple(size), MAX_DIM))
    ordered = sorted(voxels, key=lambda v: (v[2], v[1], v[0]))
    if not ordered:
        raise PixelicaPackError("cannot write an empty VOX model")
    body = bytearray(struct.pack("<i", len(ordered)))
    for x, y, z, c in ordered:
        if not (0 <= x < sx and 0 <= y < sy and 0 <= z < sz and 1 <= c <= 255):
            raise PixelicaPackError("voxel %r out of range" % ((x, y, z, c),))
        body += bytes((x, y, z, c))
    children = _chunk(b"SIZE", struct.pack("<iii", sx, sy, sz))
    children += _chunk(b"XYZI", bytes(body))
    if palette is not None:
        if len(palette) > 255:
            raise PixelicaPackError("palette has more than 255 colors")
        entries = list(palette) + [(0, 0, 0, 255)] * (255 - len(palette))
        entries.append((0, 0, 0, 0))
        children += _chunk(b"RGBA", b"".join(bytes(e) for e in entries))
    return b"VOX " + struct.pack("<i", 150) + _chunk(b"MAIN", b"", children)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--check-default-palette":
        with open(sys.argv[2], "r", encoding="utf-8", errors="replace") as fh:
            found = check_default_palette(fh.read())
        if found:
            print("default palette MISMATCH:")
            for line in found[:40]:
                print("  " + line)
            sys.exit(1)
        print("default palette matches the reference table (256 entries)")
    else:
        print("usage: vox.py --check-default-palette MagicaVoxel-file-format-vox.txt")
        sys.exit(2)
