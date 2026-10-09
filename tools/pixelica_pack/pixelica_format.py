"""Pixelica v1 binary asset formats: P3MS (.p3mesh), P3TX (.p3tex), P3CT (.p3cart).

Pure standard library. Everything is little endian, IEEE-754 binary32 floats,
and serialized field by field (no struct dumps). The byte layout is specified
in docs/asset_format.md and must match src/assets in the C engine.

Encoders are deterministic: the same input always yields the same bytes.
Decoders validate the whole file (header, table arithmetic, CRC32, payload
semantics) and raise PixelicaPackError on the first problem; they never return a
partially valid result.
"""

import array
import dataclasses
import hashlib
import math
import os
import re
import struct
import sys
import tempfile
import zlib

VERSION = 1

MAGIC_MESH = b"P3MS"
MAGIC_TEXTURE = b"P3TX"
MAGIC_CART = b"P3CT"

COMMON_HEADER_SIZE = 16
CHUNK_ENTRY_SIZE = 16
CART_RECORD_SIZE = 48
CART_NAME_BYTES = 32

# Hard defaults; the C loaders expose the same numbers as compile-time limits.
MAX_FILE_BYTES = 16 * 1024 * 1024
MAX_CHUNKS = 16
MAX_VERTICES = 65535
MAX_INDICES = 196608
MAX_TEXTURE_DIM = 4096
MAX_CART_ENTRIES = 1024  # PIXELICA_CART_MAX_ENTRIES

MESH_REQUIRED = (b"VTX0", b"IDX0", b"COL0")
MESH_OPTIONAL = (b"UV00",)
TEXTURE_REQUIRED = (b"INFO", b"DATA")
CART_REQUIRED = (b"ENTR", b"DATA")

CART_KINDS = (MAGIC_MESH, MAGIC_TEXTURE)

_NAME_RE = re.compile(r"[A-Za-z0-9_.-]{1,31}")
_U32_MAX = 0xFFFFFFFF


class PixelicaPackError(Exception):
    """A user-facing error: bad input, unsupported feature or limit exceeded."""


# --------------------------------------------------------------------------
# Small helpers
# --------------------------------------------------------------------------

def rgb565(r, g, b):
    """Pack 8-bit channels exactly like the C macro PIXELICA_RGB565 (truncating)."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xFF) >> 3)


def _tag(raw):
    """Printable form of a 4-byte tag."""
    try:
        text = bytes(raw).decode("ascii")
        if text.isprintable():
            return "'%s'" % text
    except UnicodeDecodeError:
        pass
    return repr(bytes(raw))


def _crc32(payload):
    return zlib.crc32(payload) & _U32_MAX


def _u16_to_bytes(values, what):
    try:
        arr = array.array("H", values)
    except (OverflowError, TypeError) as exc:
        raise PixelicaPackError("%s: value outside 0..65535 (%s)" % (what, exc))
    if sys.byteorder == "big":
        arr.byteswap()
    return arr.tobytes()


def _u16_from_bytes(raw):
    arr = array.array("H")
    arr.frombytes(raw)
    if sys.byteorder == "big":
        arr.byteswap()
    return arr


def _f32_to_bytes(values, what):
    for v in values:
        if not math.isfinite(v):
            raise PixelicaPackError("%s: non-finite value %r" % (what, v))
    try:
        return struct.pack("<%df" % len(values), *values)
    except (OverflowError, struct.error) as exc:
        raise PixelicaPackError("%s: value does not fit binary32 (%s)" % (what, exc))


def _f32_from_bytes(raw):
    return struct.unpack("<%df" % (len(raw) // 4), raw)


# --------------------------------------------------------------------------
# Container
# --------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Chunk:
    type: bytes
    offset: int
    length: int
    crc32: int
    payload: bytes


@dataclasses.dataclass(frozen=True)
class Container:
    magic: bytes
    version: int
    file_size: int
    chunks: tuple

    def get(self, chunk_type):
        for chunk in self.chunks:
            if chunk.type == chunk_type:
                return chunk
        return None


def build_container(magic, chunks):
    """Serialize (type, payload) pairs, in the given order, into one file."""
    count = len(chunks)
    if not 1 <= count <= MAX_CHUNKS:
        raise PixelicaPackError("chunk count %d outside 1..%d" % (count, MAX_CHUNKS))
    seen = set()
    for ctype, _payload in chunks:
        if len(ctype) != 4:
            raise PixelicaPackError("chunk type must be 4 bytes: %r" % (ctype,))
        if ctype in seen:
            raise PixelicaPackError("duplicate chunk %s" % _tag(ctype))
        seen.add(ctype)
    header_size = COMMON_HEADER_SIZE + CHUNK_ENTRY_SIZE * count
    offset = header_size
    table = []
    for ctype, payload in chunks:
        table.append(struct.pack("<4sIII", ctype, offset, len(payload),
                                 _crc32(payload)))
        offset += len(payload)
    if offset > MAX_FILE_BYTES:
        raise PixelicaPackError(
            "%s asset would be %d bytes, over the %d byte limit"
            % (magic.decode("ascii"), offset, MAX_FILE_BYTES))
    head = struct.pack("<4sHHII", magic, VERSION, header_size, offset, count)
    return b"".join([head] + table + [p for _t, p in chunks])


def parse_container(data, magic, required, optional=()):
    """Validate the container structure and return a Container.

    Checks magic, version, counts, header size, exact file size, tightly
    packed in-order offsets, CRC32, duplicate/unknown/missing chunks.
    """
    data = bytes(data)
    size = len(data)
    label = magic.decode("ascii")
    if size > MAX_FILE_BYTES:
        raise PixelicaPackError("%s: file is %d bytes, over the %d byte limit"
                          % (label, size, MAX_FILE_BYTES))
    if size < COMMON_HEADER_SIZE:
        raise PixelicaPackError("%s: truncated header (%d bytes)" % (label, size))
    got, version, header_size, file_size, count = struct.unpack_from(
        "<4sHHII", data, 0)
    if got != magic:
        raise PixelicaPackError("%s: bad magic %s" % (label, _tag(got)))
    if version != VERSION:
        raise PixelicaPackError("%s: unsupported version %d (expected %d)"
                          % (label, version, VERSION))
    if not 1 <= count <= MAX_CHUNKS:
        raise PixelicaPackError("%s: chunk count %d outside 1..%d"
                          % (label, count, MAX_CHUNKS))
    if header_size != COMMON_HEADER_SIZE + CHUNK_ENTRY_SIZE * count:
        raise PixelicaPackError("%s: header_size %d does not match %d chunks"
                          % (label, header_size, count))
    if file_size != size:
        raise PixelicaPackError(
            "%s: header declares %d bytes but file has %d (%s)"
            % (label, file_size, size,
               "truncated" if size < file_size else "trailing bytes"))
    if size < header_size:
        raise PixelicaPackError("%s: file smaller than its chunk table" % label)

    allowed = tuple(required) + tuple(optional)
    expected = header_size
    seen = set()
    chunks = []
    for n in range(count):
        ctype, offset, length, crc = struct.unpack_from(
            "<4sIII", data, COMMON_HEADER_SIZE + CHUNK_ENTRY_SIZE * n)
        if ctype in seen:
            raise PixelicaPackError("%s: duplicate chunk %s" % (label, _tag(ctype)))
        seen.add(ctype)
        if ctype not in allowed:
            raise PixelicaPackError("%s: unknown chunk %s (v1 has no optional-chunk "
                              "mechanism)" % (label, _tag(ctype)))
        if offset != expected:
            raise PixelicaPackError(
                "%s: chunk %s offset %d, expected %d (payloads must be tightly "
                "packed in table order)" % (label, _tag(ctype), offset, expected))
        if length > size - offset:
            raise PixelicaPackError("%s: chunk %s [%d,+%d) exceeds file size %d"
                              % (label, _tag(ctype), offset, length, size))
        payload = data[offset:offset + length]
        if _crc32(payload) != crc:
            raise PixelicaPackError("%s: chunk %s CRC32 mismatch" % (label, _tag(ctype)))
        chunks.append(Chunk(ctype, offset, length, crc, payload))
        expected = offset + length
    if expected != size:
        raise PixelicaPackError("%s: %d bytes after the last chunk"
                          % (label, size - expected))
    for ctype in required:
        if ctype not in seen:
            raise PixelicaPackError("%s: missing required chunk %s"
                              % (label, _tag(ctype)))
    return Container(magic, version, size, tuple(chunks))


# --------------------------------------------------------------------------
# P3MS mesh
# --------------------------------------------------------------------------

@dataclasses.dataclass
class MeshData:
    """positions: (x,y,z) floats; indices: u16 triangle list; colors: one
    RGB565 per triangle; uvs: optional (u,v) per vertex in 0..1."""
    positions: list
    indices: list
    colors: list
    uvs: object = None


def _check_mesh_counts(vertex_count, index_count, color_count, uv_count):
    if vertex_count == 0:
        raise PixelicaPackError("mesh has no vertices")
    if vertex_count > MAX_VERTICES:
        raise PixelicaPackError("mesh has %d vertices, limit is %d"
                          % (vertex_count, MAX_VERTICES))
    if index_count == 0:
        raise PixelicaPackError("mesh has no indices")
    if index_count % 3:
        raise PixelicaPackError("index count %d is not a multiple of 3" % index_count)
    if index_count > MAX_INDICES:
        raise PixelicaPackError("mesh has %d indices (%d triangles), limit is %d "
                          "indices; split the model or lower its detail"
                          % (index_count, index_count // 3, MAX_INDICES))
    if color_count != index_count // 3:
        raise PixelicaPackError("COL0 holds %d colors for %d triangles"
                          % (color_count, index_count // 3))
    if uv_count is not None and uv_count != vertex_count:
        raise PixelicaPackError("UV00 holds %d uvs for %d vertices"
                          % (uv_count, vertex_count))


def encode_mesh(mesh):
    n = len(mesh.positions)
    uv_count = None if mesh.uvs is None else len(mesh.uvs)
    _check_mesh_counts(n, len(mesh.indices), len(mesh.colors), uv_count)
    for idx in mesh.indices:
        if not 0 <= idx < n:
            raise PixelicaPackError("index %d out of range for %d vertices" % (idx, n))
    flat = [c for p in mesh.positions for c in p]
    if len(flat) != 3 * n:
        raise PixelicaPackError("every position needs exactly 3 components")
    chunks = [
        (b"VTX0", _f32_to_bytes(flat, "positions")),
        (b"IDX0", _u16_to_bytes(mesh.indices, "indices")),
        (b"COL0", _u16_to_bytes(mesh.colors, "colors")),
    ]
    if mesh.uvs is not None:
        flat_uv = [c for uv in mesh.uvs for c in uv]
        if len(flat_uv) != 2 * n:
            raise PixelicaPackError("every uv needs exactly 2 components")
        for v in flat_uv:
            if not (math.isfinite(v) and 0.0 <= v <= 1.0):
                raise PixelicaPackError("uv component %r outside 0..1" % (v,))
        chunks.append((b"UV00", _f32_to_bytes(flat_uv, "uvs")))
    return build_container(MAGIC_MESH, chunks)


def _mesh_from_container(cont):
    vtx = cont.get(b"VTX0").payload
    idx = cont.get(b"IDX0").payload
    col = cont.get(b"COL0").payload
    uv = cont.get(b"UV00")
    if len(vtx) % 12:
        raise PixelicaPackError("VTX0 length %d is not a multiple of 12" % len(vtx))
    if len(idx) % 2:
        raise PixelicaPackError("IDX0 length %d is not a multiple of 2" % len(idx))
    if len(col) % 2:
        raise PixelicaPackError("COL0 length %d is not a multiple of 2" % len(col))
    if uv is not None and len(uv.payload) % 8:
        raise PixelicaPackError("UV00 length %d is not a multiple of 8"
                          % len(uv.payload))
    vertex_count = len(vtx) // 12
    index_count = len(idx) // 2
    # Counts are checked before any large unpack.
    _check_mesh_counts(vertex_count, index_count, len(col) // 2,
                       None if uv is None else len(uv.payload) // 8)

    flat = _f32_from_bytes(vtx)
    for v in flat:
        if not math.isfinite(v):
            raise PixelicaPackError("VTX0 contains a non-finite position")
    positions = [flat[i:i + 3] for i in range(0, len(flat), 3)]
    indices = list(_u16_from_bytes(idx))
    if max(indices) >= vertex_count:
        raise PixelicaPackError("index %d out of range for %d vertices"
                          % (max(indices), vertex_count))
    colors = list(_u16_from_bytes(col))
    uvs = None
    if uv is not None:
        flat_uv = _f32_from_bytes(uv.payload)
        for v in flat_uv:
            if not (math.isfinite(v) and 0.0 <= v <= 1.0):
                raise PixelicaPackError("UV00 component %r is not a finite value "
                                  "in 0..1" % (v,))
        uvs = [flat_uv[i:i + 2] for i in range(0, len(flat_uv), 2)]
    return MeshData([tuple(p) for p in positions], indices, colors,
                    None if uvs is None else [tuple(u) for u in uvs])


def decode_mesh(data):
    return _mesh_from_container(
        parse_container(data, MAGIC_MESH, MESH_REQUIRED, MESH_OPTIONAL))


# --------------------------------------------------------------------------
# P3TX texture
# --------------------------------------------------------------------------

@dataclasses.dataclass
class TextureData:
    """pixels: width*height RGB565 values, row-major, top row first."""
    width: int
    height: int
    pixels: object


def _check_texture_dims(width, height):
    if not (1 <= width <= MAX_TEXTURE_DIM and 1 <= height <= MAX_TEXTURE_DIM):
        raise PixelicaPackError("texture size %dx%d outside 1..%d"
                          % (width, height, MAX_TEXTURE_DIM))


def encode_texture(tex):
    _check_texture_dims(tex.width, tex.height)
    if len(tex.pixels) != tex.width * tex.height:
        raise PixelicaPackError("texture has %d pixels, expected %d"
                          % (len(tex.pixels), tex.width * tex.height))
    return build_container(MAGIC_TEXTURE, [
        (b"INFO", struct.pack("<HHI", tex.width, tex.height, 0)),
        (b"DATA", _u16_to_bytes(tex.pixels, "texture pixels")),
    ])


def _texture_from_container(cont):
    info = cont.get(b"INFO").payload
    if len(info) != 8:
        raise PixelicaPackError("INFO length %d, expected 8" % len(info))
    width, height, reserved = struct.unpack("<HHI", info)
    if reserved != 0:
        raise PixelicaPackError("INFO reserved field is %d, must be 0" % reserved)
    _check_texture_dims(width, height)
    payload = cont.get(b"DATA").payload
    if len(payload) != width * height * 2:
        raise PixelicaPackError("DATA length %d, expected %d for %dx%d"
                          % (len(payload), width * height * 2, width, height))
    return TextureData(width, height, _u16_from_bytes(payload))


def decode_texture(data):
    return _texture_from_container(
        parse_container(data, MAGIC_TEXTURE, TEXTURE_REQUIRED))


# --------------------------------------------------------------------------
# P3CT cart
# --------------------------------------------------------------------------

@dataclasses.dataclass
class CartEntry:
    resource_id: int
    kind: bytes
    name: str
    data: bytes


def _check_cart_count(count):
    if count > MAX_CART_ENTRIES:
        raise PixelicaPackError("cart has %d entries, limit is %d"
                          % (count, MAX_CART_ENTRIES))


def validate_resource_name(name):
    if not isinstance(name, str) or _NAME_RE.fullmatch(name) is None:
        raise PixelicaPackError(
            "invalid resource name %r: use 1..31 characters from "
            "[A-Za-z0-9_.-]" % (name,))


def validate_resource(kind, data):
    """Fully validate a nested resource and check its magic matches kind."""
    if kind not in CART_KINDS:
        raise PixelicaPackError("resource kind %s is not allowed in a cart "
                          "(P3MS or P3TX only)" % _tag(kind))
    if bytes(data[:4]) != kind:
        raise PixelicaPackError("resource declared as %s but its magic is %s"
                          % (_tag(kind), _tag(data[:4])))
    if kind == MAGIC_MESH:
        return decode_mesh(data)
    return decode_texture(data)


def encode_cart(entries):
    """Build a cart from CartEntry objects (sorted here by ascending id)."""
    entries = list(entries)
    if not entries:
        raise PixelicaPackError("a cart needs at least one resource")
    _check_cart_count(len(entries))
    entries.sort(key=lambda e: e.resource_id)
    ids = set()
    names = set()
    records = []
    blobs = []
    offset = 0
    for e in entries:
        if not 1 <= e.resource_id <= _U32_MAX:
            raise PixelicaPackError("resource id %r must be 1..2^32-1" % (e.resource_id,))
        if e.resource_id in ids:
            raise PixelicaPackError("duplicate resource id %d" % e.resource_id)
        validate_resource_name(e.name)
        if e.name in names:
            raise PixelicaPackError("duplicate resource name %r" % e.name)
        if len(e.data) == 0:
            raise PixelicaPackError("resource %r is empty" % e.name)
        validate_resource(e.kind, e.data)
        ids.add(e.resource_id)
        names.add(e.name)
        records.append(struct.pack(
            "<I4s32sII", e.resource_id, e.kind,
            e.name.encode("ascii").ljust(CART_NAME_BYTES, b"\0"),
            offset, len(e.data)))
        blobs.append(bytes(e.data))
        offset += len(e.data)
    entr = struct.pack("<I", len(entries)) + b"".join(records)
    return build_container(MAGIC_CART, [(b"ENTR", entr),
                                        (b"DATA", b"".join(blobs))])


def bundle_named(named):
    """Bundle (name, file bytes) pairs; ids 1..N follow ascending name order."""
    named = list(named)
    _check_cart_count(len(named))
    names = [n for n, _d in named]
    for n in names:
        validate_resource_name(n)
    if len(set(names)) != len(names):
        dup = sorted(n for n in set(names) if names.count(n) > 1)[0]
        raise PixelicaPackError("duplicate resource name %r" % dup)
    ordered = sorted(named, key=lambda item: item[0])
    entries = []
    for rid, (name, data) in enumerate(ordered, start=1):
        kind = bytes(data[:4])
        if kind not in CART_KINDS:
            raise PixelicaPackError("resource %r has magic %s; only .p3mesh (P3MS) "
                              "and .p3tex (P3TX) can be bundled"
                              % (name, _tag(kind)))
        try:
            validate_resource(kind, data)
        except PixelicaPackError as exc:
            raise PixelicaPackError("resource %r is invalid: %s" % (name, exc))
        entries.append(CartEntry(rid, kind, name, bytes(data)))
    return encode_cart(entries)


def _cart_from_container(cont):
    entr = cont.get(b"ENTR").payload
    blob = cont.get(b"DATA").payload
    if len(entr) < 4:
        raise PixelicaPackError("ENTR too short for its count field")
    (count,) = struct.unpack_from("<I", entr, 0)
    if count == 0:
        raise PixelicaPackError("cart has no entries")
    if count > (len(entr) - 4) // CART_RECORD_SIZE:
        raise PixelicaPackError("ENTR declares %d entries but holds only %d bytes "
                          "of records" % (count, len(entr) - 4))
    if len(entr) != 4 + CART_RECORD_SIZE * count:
        raise PixelicaPackError("ENTR length %d does not match %d entries"
                          % (len(entr), count))
    _check_cart_count(count)
    entries = []
    names = set()
    previous_id = 0
    expected_offset = 0
    for n in range(count):
        rid, kind, raw_name, offset, length = struct.unpack_from(
            "<I4s32sII", entr, 4 + CART_RECORD_SIZE * n)
        if rid == 0:
            raise PixelicaPackError("entry %d has resource id 0" % n)
        if rid <= previous_id:
            raise PixelicaPackError("entry %d: resource ids must be strictly "
                              "ascending (%d after %d)" % (n, rid, previous_id))
        previous_id = rid
        nul = raw_name.find(b"\0")
        if nul < 1:
            raise PixelicaPackError("entry %d: name is empty or not NUL-terminated" % n)
        if any(raw_name[nul:]):
            raise PixelicaPackError("entry %d: name padding is not zero" % n)
        try:
            name = raw_name[:nul].decode("ascii")
        except UnicodeDecodeError:
            raise PixelicaPackError("entry %d: name is not ASCII" % n)
        validate_resource_name(name)
        if name in names:
            raise PixelicaPackError("duplicate resource name %r" % name)
        names.add(name)
        if length == 0:
            raise PixelicaPackError("entry %r is empty" % name)
        if offset != expected_offset:
            raise PixelicaPackError("entry %r offset %d, expected %d (resources are "
                              "tightly packed in ENTR order)"
                              % (name, offset, expected_offset))
        if length > len(blob) - offset:
            raise PixelicaPackError("entry %r [%d,+%d) exceeds DATA (%d bytes)"
                              % (name, offset, length, len(blob)))
        data = blob[offset:offset + length]
        try:
            validate_resource(kind, data)
        except PixelicaPackError as exc:
            raise PixelicaPackError("entry %r is invalid: %s" % (name, exc))
        entries.append(CartEntry(rid, kind, name, data))
        expected_offset = offset + length
    if expected_offset != len(blob):
        raise PixelicaPackError("DATA has %d bytes not covered by ENTR"
                          % (len(blob) - expected_offset))
    return entries


def decode_cart(data):
    """Return the list of CartEntry (nested resources fully validated)."""
    return _cart_from_container(
        parse_container(data, MAGIC_CART, CART_REQUIRED))


# --------------------------------------------------------------------------
# Inspection
# --------------------------------------------------------------------------

def _chunk_table(cont):
    return [{"type": c.type.decode("ascii"), "offset": c.offset,
             "length": c.length, "crc32": "0x%08x" % c.crc32}
            for c in cont.chunks]


def inspect_asset(data):
    """Return a JSON-serializable dict describing a validated P3MS/P3TX/P3CT."""
    data = bytes(data)
    magic = data[:4]
    info = {"file_size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    if magic == MAGIC_MESH:
        cont = parse_container(data, MAGIC_MESH, MESH_REQUIRED, MESH_OPTIONAL)
        mesh = _mesh_from_container(cont)
        xs, ys, zs = zip(*mesh.positions)
        info.update({
            "format": "P3MS", "version": cont.version,
            "chunks": _chunk_table(cont),
            "vertex_count": len(mesh.positions),
            "index_count": len(mesh.indices),
            "triangle_count": len(mesh.indices) // 3,
            "has_uv": mesh.uvs is not None,
            "unique_colors": len(set(mesh.colors)),
            "bounds_min": [min(xs), min(ys), min(zs)],
            "bounds_max": [max(xs), max(ys), max(zs)],
        })
    elif magic == MAGIC_TEXTURE:
        cont = parse_container(data, MAGIC_TEXTURE, TEXTURE_REQUIRED)
        tex = _texture_from_container(cont)
        info.update({
            "format": "P3TX", "version": cont.version,
            "chunks": _chunk_table(cont),
            "width": tex.width, "height": tex.height,
            "unique_colors": len(set(tex.pixels)),
        })
    elif magic == MAGIC_CART:
        cont = parse_container(data, MAGIC_CART, CART_REQUIRED)
        entries = _cart_from_container(cont)
        offset = 0
        listing = []
        for e in entries:
            nested = inspect_asset(e.data)
            listing.append({
                "id": e.resource_id, "name": e.name,
                "kind": e.kind.decode("ascii"),
                "offset": offset, "length": len(e.data), "asset": nested,
            })
            offset += len(e.data)
        info.update({
            "format": "P3CT", "version": cont.version,
            "chunks": _chunk_table(cont),
            "entry_count": len(entries), "entries": listing,
        })
    else:
        raise PixelicaPackError("not a Pixelica asset (magic %s)" % _tag(magic))
    return info


# --------------------------------------------------------------------------
# File helpers
# --------------------------------------------------------------------------

def read_limited(path, limit=MAX_FILE_BYTES):
    """Read a whole file, refusing anything larger than limit bytes."""
    try:
        with open(path, "rb") as handle:
            data = handle.read(limit + 1)
    except OSError as exc:
        raise PixelicaPackError("cannot read %s: %s" % (path, exc.strerror or exc))
    if len(data) > limit:
        raise PixelicaPackError("%s is larger than the %d byte limit" % (path, limit))
    return data


def atomic_write(path, data):
    """Write data to path via a temp file in the same directory + rename, so a
    failure never leaves a half-written or damaged output."""
    directory = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(directory):
        raise PixelicaPackError("output directory does not exist: %s" % directory)
    if os.path.isdir(path):
        raise PixelicaPackError("output path is a directory: %s" % path)
    try:
        fd, tmp = tempfile.mkstemp(prefix=".pixelica_pack-", suffix=".tmp", dir=directory)
    except OSError as exc:
        raise PixelicaPackError("cannot create output in %s: %s"
                          % (directory, exc.strerror or exc))
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except OSError as exc:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise PixelicaPackError("cannot write %s: %s" % (path, exc.strerror or exc))
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
