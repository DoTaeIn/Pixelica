"""Shared test helpers. Builds files by hand (struct only) so the tests check
the encoders against an independent description of the byte layout."""

import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)


def crc(payload):
    return zlib.crc32(payload) & 0xFFFFFFFF


def f32s(*values):
    return struct.pack("<%df" % len(values), *values)


def u16s(*values):
    return struct.pack("<%dH" % len(values), *values)


def make_file(magic, chunks, version=1, header_size=None, file_size=None,
              count=None, offsets=None, lengths=None, crcs=None, trailing=b""):
    """Container with optional per-field overrides to build corrupt files."""
    n = len(chunks)
    base = 16 + 16 * n
    table = b""
    payloads = b""
    cursor = base
    for i, (ctype, payload) in enumerate(chunks):
        off = cursor if offsets is None or offsets[i] is None else offsets[i]
        length = len(payload) if lengths is None or lengths[i] is None else lengths[i]
        check = crc(payload) if crcs is None or crcs[i] is None else crcs[i]
        table += struct.pack("<4sIII", ctype, off, length, check)
        payloads += payload
        cursor += len(payload)
    total = base + len(payloads) + len(trailing)
    head = struct.pack(
        "<4sHHII", magic, version,
        base if header_size is None else header_size,
        total if file_size is None else file_size,
        n if count is None else count)
    return head + table + payloads + trailing


TRI_VTX = f32s(0, 0, 0, 1, 0, 0, 0, 1, 0)
TRI_IDX = u16s(0, 1, 2)
TRI_COL = u16s(0xF800)


def mesh_chunks(vtx=TRI_VTX, idx=TRI_IDX, col=TRI_COL, uv=None):
    chunks = [(b"VTX0", vtx), (b"IDX0", idx), (b"COL0", col)]
    if uv is not None:
        chunks.append((b"UV00", uv))
    return chunks


def mesh_file(**kw):
    return make_file(b"P3MS", mesh_chunks(), **kw)


def tex_chunks(width=2, height=2, reserved=0, pixels=None):
    if pixels is None:
        pixels = u16s(*range(width * height))
    return [(b"INFO", struct.pack("<HHI", width, height, reserved)),
            (b"DATA", pixels)]


def tex_file(**kw):
    return make_file(b"P3TX", tex_chunks(), **kw)


def cart_record(rid, kind, name, offset, length):
    if isinstance(name, str):
        name = name.encode("ascii")
    return struct.pack("<I4s32sII", rid, kind, name.ljust(32, b"\0"),
                       offset, length)


def cart_file(records, blob, count=None, **kw):
    entr = struct.pack("<I", len(records) if count is None else count) \
        + b"".join(records)
    return make_file(b"P3CT", [(b"ENTR", entr), (b"DATA", blob)], **kw)


# ---- VOX ----

def vox_chunk(tag, content, children=b""):
    return struct.pack("<4sii", tag, len(content), len(children)) + content + children


def vox_size(x, y, z):
    return vox_chunk(b"SIZE", struct.pack("<iii", x, y, z))


def vox_xyzi(voxels):
    body = struct.pack("<i", len(voxels)) + b"".join(bytes(v) for v in voxels)
    return vox_chunk(b"XYZI", body)


def vox_rgba(entries):
    """entries: dict chunk_slot -> (r,g,b,a); others opaque black."""
    out = b""
    for i in range(256):
        out += bytes(entries.get(i, (0, 0, 0, 255)))
    return vox_chunk(b"RGBA", out)


def vox_file(children, version=150, main_children=None):
    body = b"".join(children)
    declared = len(body) if main_children is None else main_children
    return (b"VOX " + struct.pack("<i", version)
            + struct.pack("<4sii", b"MAIN", 0, declared) + body)
