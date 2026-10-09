import json
import random
import struct
import unittest

import support
from support import (TRI_COL, TRI_IDX, TRI_VTX, cart_file, cart_record, f32s,
                     make_file, mesh_chunks, mesh_file, tex_chunks, tex_file,
                     u16s)

import pixelica_format
from pixelica_format import (CartEntry, MeshData, PixelicaPackError, TextureData,
                      bundle_named, decode_cart, decode_mesh, decode_texture,
                      encode_cart, encode_mesh, encode_texture, inspect_asset)


def tri_mesh(uvs=None):
    return MeshData([(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
                    [0, 1, 2], [0xF800], uvs)


class MeshFormatTests(unittest.TestCase):
    def test_encoder_matches_independent_layout(self):
        self.assertEqual(encode_mesh(tri_mesh()), mesh_file())

    def test_encoder_with_uv_matches_independent_layout(self):
        uv = [(0.0, 0.0), (1.0, 0.0), (0.5, 1.0)]
        expected = make_file(b"P3MS", mesh_chunks(uv=f32s(0, 0, 1, 0, 0.5, 1)))
        self.assertEqual(encode_mesh(tri_mesh(uv)), expected)

    def test_header_fields(self):
        data = encode_mesh(tri_mesh())
        magic, version, header_size, file_size, count = struct.unpack_from(
            "<4sHHII", data, 0)
        self.assertEqual((magic, version, header_size, count),
                         (b"P3MS", 1, 16 + 16 * 3, 3))
        self.assertEqual(file_size, len(data))
        types = [struct.unpack_from("<4s", data, 16 + 16 * i)[0] for i in range(3)]
        self.assertEqual(types, [b"VTX0", b"IDX0", b"COL0"])

    def test_roundtrip_is_byte_identical(self):
        uv = [(0.0, 0.25), (1.0, 0.5), (0.5, 1.0)]
        data = encode_mesh(tri_mesh(uv))
        mesh = decode_mesh(data)
        self.assertEqual(mesh.indices, [0, 1, 2])
        self.assertEqual(mesh.colors, [0xF800])
        self.assertEqual(mesh.positions[1], (1.0, 0.0, 0.0))
        self.assertEqual(mesh.uvs, [(0.0, 0.25), (1.0, 0.5), (0.5, 1.0)])
        self.assertEqual(encode_mesh(mesh), data)

    def test_decode_accepts_bytearray_and_memoryview(self):
        data = mesh_file()
        decode_mesh(bytearray(data))
        decode_mesh(memoryview(data))

    # ---- container corruption ----
    def assertRejected(self, data, text=None, decoder=decode_mesh):
        with self.assertRaises(PixelicaPackError) as ctx:
            decoder(data)
        if text:
            self.assertIn(text, str(ctx.exception))

    def test_bad_magic(self):
        self.assertRejected(b"P3MX" + mesh_file()[4:], "magic")

    def test_bad_version(self):
        for version in (0, 2, 0xFFFF):
            self.assertRejected(mesh_file(version=version), "version")

    def test_bad_chunk_count(self):
        self.assertRejected(mesh_file(count=0), "chunk count")
        self.assertRejected(mesh_file(count=17), "chunk count")
        self.assertRejected(mesh_file(count=0xFFFFFFFF), "chunk count")
        # table says 2 chunks while the header_size claims 3
        self.assertRejected(mesh_file(count=2), "header_size")

    def test_bad_header_size(self):
        self.assertRejected(mesh_file(header_size=48), "header_size")
        self.assertRejected(mesh_file(header_size=0xFFFF), "header_size")

    def test_file_size_must_be_exact(self):
        actual = len(mesh_file())
        self.assertRejected(mesh_file(file_size=actual + 1), "truncated")
        self.assertRejected(mesh_file(file_size=actual - 1), "trailing")
        self.assertRejected(mesh_file(file_size=0xFFFFFFFF), "truncated")

    def test_trailing_bytes(self):
        self.assertRejected(mesh_file(trailing=b"\0"), "after the last chunk")

    def test_every_truncation_is_rejected(self):
        data = encode_mesh(tri_mesh([(0, 0), (1, 0), (0, 1)]))
        for cut in range(len(data)):
            with self.assertRaises(PixelicaPackError, msg="prefix %d" % cut):
                decode_mesh(data[:cut])

    def test_crc_mismatch(self):
        self.assertRejected(mesh_file(crcs=[None, 1, None]), "CRC32")
        data = bytearray(mesh_file())
        data[-1] ^= 0x01  # flip a payload bit
        self.assertRejected(bytes(data), "CRC32")

    def test_offset_arithmetic(self):
        base = 16 + 16 * 3
        self.assertRejected(mesh_file(offsets=[base + 1, None, None]), "offset")
        self.assertRejected(mesh_file(offsets=[None, base, None]), "offset")
        self.assertRejected(mesh_file(offsets=[None, None, 0xFFFFFFFF]), "offset")
        self.assertRejected(mesh_file(offsets=[0, None, None]), "offset")

    def test_length_overflow(self):
        self.assertRejected(mesh_file(lengths=[0xFFFFFFFF, None, None]), "exceeds")
        self.assertRejected(mesh_file(lengths=[None, None, 1000]), "exceeds")

    def test_shortened_length_leaves_bytes_uncovered(self):
        # CRC computed over the real payload is checked against the shortened
        # slice, so this fails on CRC or on coverage; either way it is rejected.
        self.assertRejected(mesh_file(lengths=[None, None, 0]))

    def test_duplicate_chunk(self):
        data = make_file(b"P3MS", mesh_chunks() + [(b"VTX0", TRI_VTX)])
        self.assertRejected(data, "duplicate")

    def test_unknown_chunk(self):
        data = make_file(b"P3MS", mesh_chunks() + [(b"XTRA", b"\1\2\3\4")])
        self.assertRejected(data, "unknown chunk")

    def test_missing_required_chunk(self):
        for drop in range(3):
            chunks = mesh_chunks()
            del chunks[drop]
            self.assertRejected(make_file(b"P3MS", chunks), "missing")

    def test_file_over_16mib(self):
        data = b"P3MS" + bytes(pixelica_format.MAX_FILE_BYTES)
        self.assertRejected(data, "limit")

    def test_other_magic_rejected(self):
        self.assertRejected(tex_file(), "magic")

    # ---- mesh semantics ----
    def test_vertex_payload_lengths(self):
        for bad in (b"", TRI_VTX[:-1], TRI_VTX + b"\0"):
            self.assertRejected(make_file(b"P3MS", mesh_chunks(vtx=bad)))

    def test_index_payload(self):
        self.assertRejected(make_file(b"P3MS", mesh_chunks(idx=b"")), "no indices")
        self.assertRejected(make_file(b"P3MS", mesh_chunks(idx=TRI_IDX + b"\0")),
                            "multiple of 2")
        self.assertRejected(
            make_file(b"P3MS", mesh_chunks(idx=u16s(0, 1, 2, 0), col=u16s(1))),
            "multiple of 3")
        self.assertRejected(make_file(b"P3MS", mesh_chunks(idx=u16s(0, 1, 3))),
                            "out of range")
        self.assertRejected(make_file(b"P3MS", mesh_chunks(idx=u16s(0, 1, 0xFFFF))),
                            "out of range")

    def test_color_count_must_match_triangles(self):
        self.assertRejected(make_file(b"P3MS", mesh_chunks(col=b"")), "COL0")
        self.assertRejected(make_file(b"P3MS", mesh_chunks(col=u16s(1, 2))), "COL0")

    def test_non_finite_positions(self):
        for bad in (float("nan"), float("inf"), float("-inf")):
            vtx = f32s(0, 0, 0, bad, 0, 0, 0, 1, 0)
            self.assertRejected(make_file(b"P3MS", mesh_chunks(vtx=vtx)),
                                "non-finite")

    def test_uv_validation(self):
        for bad in (1.5, -0.01, float("nan"), float("inf")):
            uv = f32s(0, 0, bad, 0, 0, 1)
            self.assertRejected(make_file(b"P3MS", mesh_chunks(uv=uv)), "UV00")
        self.assertRejected(make_file(b"P3MS", mesh_chunks(uv=f32s(0, 0, 1, 1))),
                            "UV00")
        self.assertRejected(
            make_file(b"P3MS", mesh_chunks(uv=f32s(0, 0, 1, 1, 0, 0) + b"\0")),
            "multiple of 8")
        # boundary values are fine
        decode_mesh(make_file(b"P3MS", mesh_chunks(uv=f32s(0, 0, 1, 1, 1, 0))))

    def test_vertex_count_boundary(self):
        def with_vertices(n):
            return make_file(b"P3MS", mesh_chunks(vtx=bytes(12 * n)))
        self.assertEqual(len(decode_mesh(with_vertices(65535)).positions), 65535)
        self.assertRejected(with_vertices(65536), "limit")

    def test_index_count_boundary(self):
        def with_triangles(n):
            return make_file(b"P3MS", mesh_chunks(idx=u16s(*([0, 1, 2] * n)),
                                                  col=u16s(*([0] * n))))
        self.assertEqual(len(decode_mesh(with_triangles(65536)).indices), 196608)
        self.assertRejected(with_triangles(65537), "limit")

    def test_decoder_never_fails_other_than_pixelica_packerror(self):
        rng = random.Random(20240607)
        good = encode_mesh(tri_mesh([(0, 0), (1, 0), (0, 1)]))
        for _ in range(400):
            data = bytearray(good)
            for _k in range(rng.randint(1, 4)):
                data[rng.randrange(len(data))] = rng.randrange(256)
            try:
                decode_mesh(bytes(data))
            except PixelicaPackError:
                pass

    # ---- encoder validation ----
    def test_encoder_rejects_bad_meshes(self):
        good = tri_mesh()
        cases = [
            MeshData([], [], []),
            MeshData(good.positions, [0, 1, 3], [1]),
            MeshData(good.positions, [0, 1, 2], []),
            MeshData(good.positions, [0, 1], []),
            MeshData([(float("nan"), 0, 0)] * 3, [0, 1, 2], [1]),
            MeshData([(1e39, 0, 0)] * 3, [0, 1, 2], [1]),
            MeshData(good.positions, [0, 1, 2], [0x10000]),
            MeshData(good.positions, [0, 1, 2], [1], [(0, 0)] * 2),
            MeshData(good.positions, [0, 1, 2], [1], [(0, 2)] * 3),
            MeshData([(0, 0, 0)] * 65536, [0, 1, 2], [1]),
            MeshData(good.positions, [0, 1, 2] * 65537, [1] * 65537),
        ]
        for case in cases:
            with self.assertRaises(PixelicaPackError):
                encode_mesh(case)


class TextureFormatTests(unittest.TestCase):
    def test_encoder_matches_independent_layout(self):
        tex = TextureData(2, 2, [0, 1, 2, 3])
        self.assertEqual(encode_texture(tex), tex_file())

    def test_roundtrip(self):
        tex = TextureData(3, 1, [0xF800, 0x07E0, 0x001F])
        data = encode_texture(tex)
        back = decode_texture(data)
        self.assertEqual((back.width, back.height), (3, 1))
        self.assertEqual(list(back.pixels), [0xF800, 0x07E0, 0x001F])
        self.assertEqual(encode_texture(back), data)

    def check(self, **kw):
        with self.assertRaises(PixelicaPackError):
            decode_texture(make_file(b"P3TX", tex_chunks(**kw)))

    def test_rejections(self):
        self.check(reserved=1)
        self.check(width=0, height=2, pixels=b"")
        self.check(width=2, height=0, pixels=b"")
        self.check(width=4097, height=1, pixels=bytes(2 * 4097))
        self.check(width=1, height=4097, pixels=bytes(2 * 4097))
        self.check(width=2, height=2, pixels=bytes(7))
        self.check(width=2, height=2, pixels=bytes(9))

    def test_info_length(self):
        data = make_file(b"P3TX", [(b"INFO", bytes(9)), (b"DATA", bytes(2))])
        with self.assertRaises(PixelicaPackError):
            decode_texture(data)

    def test_max_dimension_boundary(self):
        data = make_file(b"P3TX", tex_chunks(4096, 1, pixels=bytes(2 * 4096)))
        self.assertEqual(decode_texture(data).width, 4096)

    def test_missing_and_unknown_chunks(self):
        with self.assertRaises(PixelicaPackError):
            decode_texture(make_file(b"P3TX", tex_chunks()[:1]))
        with self.assertRaises(PixelicaPackError):
            decode_texture(make_file(b"P3TX", tex_chunks() + [(b"UV00", b"")]))

    def test_encoder_validation(self):
        for tex in (TextureData(0, 1, []), TextureData(2, 2, [0] * 3),
                    TextureData(4097, 1, [0] * 4097),
                    TextureData(1, 1, [70000])):
            with self.assertRaises(PixelicaPackError):
                encode_texture(tex)

    def test_truncations(self):
        data = tex_file()
        for cut in range(len(data)):
            with self.assertRaises(PixelicaPackError):
                decode_texture(data[:cut])


MESH = encode_mesh(tri_mesh())
TEX = encode_texture(TextureData(2, 2, [1, 2, 3, 4]))


def good_cart_parts():
    records = [cart_record(1, b"P3MS", "alpha", 0, len(MESH)),
               cart_record(2, b"P3TX", "zeta", len(MESH), len(TEX))]
    return records, MESH + TEX


class CartFormatTests(unittest.TestCase):
    def test_bundle_matches_independent_layout(self):
        records, blob = good_cart_parts()
        data = bundle_named([("zeta", TEX), ("alpha", MESH)])
        self.assertEqual(data, cart_file(records, blob))

    def test_bundle_is_order_independent_and_ids_follow_names(self):
        a = bundle_named([("zeta", TEX), ("alpha", MESH), ("mid", MESH)])
        b = bundle_named([("mid", MESH), ("alpha", MESH), ("zeta", TEX)])
        self.assertEqual(a, b)
        entries = decode_cart(a)
        self.assertEqual([(e.resource_id, e.name, e.kind) for e in entries],
                         [(1, "alpha", b"P3MS"), (2, "mid", b"P3MS"),
                          (3, "zeta", b"P3TX")])
        self.assertEqual(entries[2].data, TEX)

    def test_decode_returns_nested_bytes(self):
        records, blob = good_cart_parts()
        entries = decode_cart(cart_file(records, blob))
        self.assertEqual(entries[0].data, MESH)
        self.assertEqual(entries[1].data, TEX)

    def test_valid_names(self):
        for name in ("a", "Desk_1.v2-b", "x" * 31):
            decode_cart(bundle_named([(name, MESH)]))

    def test_invalid_names(self):
        for name in ("", "has space", "x" * 32, "café", "a/b", "a\0b", "a=b"):
            with self.assertRaises(PixelicaPackError, msg=repr(name)):
                bundle_named([(name, MESH)])

    def test_bundle_rejects_duplicates_and_bad_resources(self):
        with self.assertRaises(PixelicaPackError):
            bundle_named([("a", MESH), ("a", TEX)])
        with self.assertRaises(PixelicaPackError):
            bundle_named([])
        with self.assertRaises(PixelicaPackError):
            bundle_named([("a", b"not an asset")])
        with self.assertRaises(PixelicaPackError):
            bundle_named([("a", encode_cart([CartEntry(1, b"P3MS", "m", MESH)]))])
        corrupt = bytearray(MESH)
        corrupt[-1] ^= 0xFF
        with self.assertRaises(PixelicaPackError):
            bundle_named([("a", bytes(corrupt))])

    def test_encode_cart_rejections(self):
        bad_sets = [
            [CartEntry(0, b"P3MS", "a", MESH)],
            [CartEntry(1, b"P3MS", "a", MESH), CartEntry(1, b"P3MS", "b", MESH)],
            [CartEntry(1, b"P3MS", "a", MESH), CartEntry(2, b"P3MS", "a", MESH)],
            [CartEntry(1, b"P3TX", "a", MESH)],
            [CartEntry(1, b"P3CT", "a", MESH)],
            [CartEntry(1, b"P3MS", "a", b"")],
            [CartEntry(2 ** 32, b"P3MS", "a", MESH)],
        ]
        for entries in bad_sets:
            with self.assertRaises(PixelicaPackError):
                encode_cart(entries)

    def reject(self, data, text=None):
        with self.assertRaises(PixelicaPackError) as ctx:
            decode_cart(data)
        if text:
            self.assertIn(text, str(ctx.exception))

    def test_record_rejections(self):
        n = len(MESH)
        t = len(TEX)
        blob = MESH + TEX
        r = cart_record
        cases = {
            "zero id": ([r(0, b"P3MS", "a", 0, n), r(2, b"P3TX", "b", n, t)], "id 0"),
            "unsorted": ([r(2, b"P3MS", "a", 0, n), r(1, b"P3TX", "b", n, t)], "ascending"),
            "duplicate id": ([r(1, b"P3MS", "a", 0, n), r(1, b"P3TX", "b", n, t)], "ascending"),
            "duplicate name": ([r(1, b"P3MS", "a", 0, n), r(2, b"P3TX", "a", n, t)], "duplicate"),
            "kind mismatch": ([r(1, b"P3TX", "a", 0, n), r(2, b"P3TX", "b", n, t)], "magic"),
            "cart in cart": ([r(1, b"P3CT", "a", 0, n), r(2, b"P3TX", "b", n, t)], "not allowed"),
            "unknown kind": ([r(1, b"ABCD", "a", 0, n), r(2, b"P3TX", "b", n, t)], "not allowed"),
            "no nul": ([r(1, b"P3MS", b"a" * 32, 0, n), r(2, b"P3TX", "b", n, t)], "NUL"),
            "empty name": ([r(1, b"P3MS", b"", 0, n), r(2, b"P3TX", "b", n, t)], "empty"),
            "dirty padding": ([r(1, b"P3MS", b"a\0junk", 0, n), r(2, b"P3TX", "b", n, t)], "padding"),
            "bad char": ([r(1, b"P3MS", "a b", 0, n), r(2, b"P3TX", "b", n, t)], "invalid resource name"),
            "gap": ([r(1, b"P3MS", "a", 1, n - 1), r(2, b"P3TX", "b", n, t)], "offset"),
            "overlap": ([r(1, b"P3MS", "a", 0, n), r(2, b"P3TX", "b", n - 1, t)], "offset"),
            "zero length": ([r(1, b"P3MS", "a", 0, 0), r(2, b"P3TX", "b", 0, n + t)], "empty"),
            "beyond data": ([r(1, b"P3MS", "a", 0, n), r(2, b"P3TX", "b", n, t + 1)], "exceeds"),
            "huge length": ([r(1, b"P3MS", "a", 0, n), r(2, b"P3TX", "b", n, 0xFFFFFFFF)], "exceeds"),
            "uncovered data": ([r(1, b"P3MS", "a", 0, n), r(2, b"P3TX", "b", n, t - 1)], None),
        }
        for label, (records, text) in cases.items():
            with self.subTest(label):
                self.reject(cart_file(records, blob), text)

    def test_unreferenced_trailing_data(self):
        records, blob = good_cart_parts()
        self.reject(cart_file(records, blob + b"\0"), "not covered")

    def test_entry_count_field(self):
        records, blob = good_cart_parts()
        self.reject(cart_file(records, blob, count=3), "entries")
        self.reject(cart_file(records, blob, count=1), "does not match")
        self.reject(cart_file(records, blob, count=0xFFFFFFFF), "entries")
        self.reject(cart_file([], blob, count=0), "no entries")
        short = make_file(b"P3CT", [(b"ENTR", b"\1\0"), (b"DATA", blob)])
        self.reject(short, "too short")

    def many_cart(self, count):
        """Hand-built cart with count valid textures (independent layout)."""
        records = [cart_record(i + 1, b"P3TX", "t%d" % i, i * len(TEX), len(TEX))
                   for i in range(count)]
        return cart_file(records, TEX * count)

    def test_entry_limit_matches_c_loader(self):
        self.assertEqual(pixelica_format.MAX_CART_ENTRIES, 1024)  # PIXELICA_CART_MAX_ENTRIES

    def test_1024_entries_succeed_everywhere(self):
        data = self.many_cart(1024)
        entries = decode_cart(data)
        self.assertEqual(len(entries), 1024)
        self.assertEqual(entries[-1].resource_id, 1024)
        self.assertEqual(inspect_asset(data)["entry_count"], 1024)
        named = [("t%d" % i, TEX) for i in range(1024)]
        bundled = bundle_named(named)
        self.assertEqual(len(decode_cart(bundled)), 1024)
        self.assertEqual(
            encode_cart([CartEntry(i + 1, b"P3TX", "t%d" % i, TEX)
                         for i in range(1024)]),
            encode_cart([CartEntry(i + 1, b"P3TX", "t%d" % i, TEX)
                         for i in reversed(range(1024))]))

    def test_1025_entries_fail_explicitly(self):
        self.reject(self.many_cart(1025), "limit is 1024")
        with self.assertRaises(PixelicaPackError) as ctx:
            inspect_asset(self.many_cart(1025))
        self.assertIn("limit is 1024", str(ctx.exception))
        with self.assertRaises(PixelicaPackError) as ctx:
            bundle_named([("t%d" % i, TEX) for i in range(1025)])
        self.assertIn("limit is 1024", str(ctx.exception))
        with self.assertRaises(PixelicaPackError) as ctx:
            encode_cart([CartEntry(i + 1, b"P3TX", "t%d" % i, TEX)
                         for i in range(1025)])
        self.assertIn("limit is 1024", str(ctx.exception))

    def test_entry_limit_is_checked_before_per_entry_work(self):
        # Invalid names/resources would fail later with a different message,
        # so the count message proves the bound came first.
        with self.assertRaises(PixelicaPackError) as ctx:
            bundle_named([("bad name", b"junk")] * 1025)
        self.assertIn("limit is 1024", str(ctx.exception))
        with self.assertRaises(PixelicaPackError) as ctx:
            encode_cart([CartEntry(0, b"XXXX", "", b"")] * 1025)
        self.assertIn("limit is 1024", str(ctx.exception))
        # A huge declared count is rejected from ENTR without iterating.
        self.reject(cart_file([], b"\0", count=0x00FFFFFF), "entries")

    def test_corrupt_nested_resource_is_found_on_open(self):
        records, blob = good_cart_parts()
        bad = bytearray(blob)
        bad[len(MESH) - 1] ^= 0xFF  # inside the mesh, outer CRC recomputed
        self.reject(cart_file(records, bytes(bad)), "invalid")

    def test_missing_unknown_and_duplicate_chunks(self):
        records, blob = good_cart_parts()
        entr = struct.pack("<I", 2) + b"".join(records)
        self.reject(make_file(b"P3CT", [(b"ENTR", entr)]), "missing")
        self.reject(make_file(b"P3CT", [(b"ENTR", entr), (b"DATA", blob),
                                        (b"EXTR", b"")]), "unknown")
        self.reject(make_file(b"P3CT", [(b"ENTR", entr), (b"DATA", blob),
                                        (b"DATA", blob)]), "duplicate")

    def test_every_truncation_is_rejected(self):
        data = bundle_named([("a", MESH), ("b", TEX)])
        for cut in range(0, len(data), 7):
            with self.assertRaises(PixelicaPackError):
                decode_cart(data[:cut])

    def test_odd_sized_payloads_pack_tightly(self):
        # A 1x1 texture has a 2-byte DATA payload; the cart must not pad.
        one = encode_texture(TextureData(1, 1, [7]))
        entries = decode_cart(bundle_named([("a", one), ("b", MESH)]))
        self.assertEqual([len(e.data) for e in entries], [len(one), len(MESH)])


class InspectTests(unittest.TestCase):
    def test_mesh_info(self):
        info = inspect_asset(encode_mesh(tri_mesh([(0, 0), (1, 0), (0, 1)])))
        self.assertEqual(info["format"], "P3MS")
        self.assertEqual((info["vertex_count"], info["index_count"],
                          info["triangle_count"]), (3, 3, 1))
        self.assertTrue(info["has_uv"])
        self.assertEqual(info["bounds_min"], [0.0, 0.0, 0.0])
        self.assertEqual(info["bounds_max"], [1.0, 1.0, 0.0])
        self.assertEqual([c["type"] for c in info["chunks"]],
                         ["VTX0", "IDX0", "COL0", "UV00"])

    def test_texture_and_cart_info(self):
        info = inspect_asset(TEX)
        self.assertEqual((info["width"], info["height"]), (2, 2))
        cart = inspect_asset(bundle_named([("m", MESH), ("t", TEX)]))
        self.assertEqual(cart["entry_count"], 2)
        self.assertEqual([e["name"] for e in cart["entries"]], ["m", "t"])
        self.assertEqual(cart["entries"][0]["asset"]["format"], "P3MS")
        self.assertEqual(cart["entries"][1]["offset"], len(MESH))

    def test_json_is_stable(self):
        data = bundle_named([("m", MESH), ("t", TEX)])
        first = json.dumps(inspect_asset(data), sort_keys=True)
        second = json.dumps(inspect_asset(data), sort_keys=True)
        self.assertEqual(first, second)

    def test_unknown_and_invalid(self):
        with self.assertRaises(PixelicaPackError):
            inspect_asset(b"hello world, definitely not an asset")
        with self.assertRaises(PixelicaPackError):
            inspect_asset(MESH[:-1])


class HelperTests(unittest.TestCase):
    def test_rgb565_matches_c_macro(self):
        self.assertEqual(pixelica_format.rgb565(255, 255, 255), 0xFFFF)
        self.assertEqual(pixelica_format.rgb565(0, 0, 0), 0)
        self.assertEqual(pixelica_format.rgb565(255, 0, 0), 0xF800)
        self.assertEqual(pixelica_format.rgb565(0, 255, 0), 0x07E0)
        self.assertEqual(pixelica_format.rgb565(0, 0, 255), 0x001F)
        self.assertEqual(pixelica_format.rgb565(8, 4, 8), 0x0821)
        self.assertEqual(pixelica_format.rgb565(7, 3, 7), 0)

    def test_crc32_is_ieee(self):
        self.assertEqual(support.crc(b"123456789"), 0xCBF43926)
        data = mesh_file()
        crc = struct.unpack_from("<I", data, 16 + 12)[0]
        self.assertEqual(crc, support.crc(TRI_VTX))


if __name__ == "__main__":
    unittest.main()
