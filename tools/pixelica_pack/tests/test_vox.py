import itertools
import math
import os
import random
import struct
import unittest

import support
from support import (make_file, vox_chunk, vox_file, vox_rgba, vox_size,
                     vox_xyzi)

import pixelica_format
import vox
from pixelica_format import PixelicaPackError, rgb565

RED = (255, 0, 0, 255)
GREEN = (0, 255, 0, 255)
BLUE = (0, 0, 255, 255)
RGB_PALETTE = [RED, GREEN, BLUE]


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def build(size, voxels, palette=RGB_PALETTE, scale=1.0, pivot="floor"):
    data = vox.encode_vox(size, voxels, palette)
    return vox.build_mesh(vox.parse_vox(data), scale, pivot)


def check_geometry(case, size, voxels, palette, mesh):
    """Independent oracle (scale 1, pivot floor): every triangle lies on an
    exposed face with the right winding and color, and total area equals the
    number of exposed voxel faces."""
    sx, sy, sz = size
    occupied = {(x, y, z): c for x, y, z, c in voxels}

    def at(point):
        i = math.floor(point[0] + sx / 2.0)
        j = math.floor(point[1])
        k = math.floor(point[2] + sy / 2.0)
        return occupied.get((i, sy - 1 - k, j))  # engine (x,y,z) = vox (x,z,-y)

    exposed = 0
    for (x, y, z) in occupied:
        for d in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
            if (x + d[0], y + d[1], z + d[2]) not in occupied:
                exposed += 1

    case.assertEqual(len(set(mesh.positions)), len(mesh.positions),
                     "vertices must be shared by position")
    case.assertEqual(len(mesh.colors) * 3, len(mesh.indices))
    area = 0.0
    for t in range(len(mesh.indices) // 3):
        a, b, c = (mesh.positions[mesh.indices[3 * t + n]] for n in range(3))
        normal = cross(sub(b, a), sub(c, a))
        length = math.sqrt(sum(v * v for v in normal))
        case.assertGreater(length, 0.0)
        unit = tuple(v / length for v in normal)
        case.assertEqual(sorted(abs(v) for v in unit), [0.0, 0.0, 1.0],
                         "face must be axis aligned")
        area += length / 2.0
        samples = [tuple((a[n] + b[n] + c[n]) / 3.0 for n in range(3))]
        for corner in (a, b, c):
            samples.append(tuple(
                (corner[n] * 2 + a[n] + b[n] + c[n] - corner[n]) / 4.0
                for n in range(3)))
        for s in samples:
            inside = at(tuple(s[n] - 0.25 * unit[n] for n in range(3)))
            outside = at(tuple(s[n] + 0.25 * unit[n] for n in range(3)))
            case.assertIsNotNone(inside, "winding must face outward")
            case.assertIsNone(outside, "face must be exposed")
            rgb = palette[inside - 1]
            case.assertEqual(mesh.colors[t], rgb565(*rgb[:3]))
    case.assertEqual(area, float(exposed))


class AxisAndPlacementTests(unittest.TestCase):
    def test_axis_map_and_floor_pivot(self):
        mesh = build((4, 5, 6), [(1, 2, 3, 1)])
        xs, ys, zs = zip(*mesh.positions)
        self.assertEqual((min(xs), max(xs)), (-1.0, 0.0))   # x - 4/2
        self.assertEqual((min(ys), max(ys)), (3.0, 4.0))    # vox z, floor at 0
        self.assertEqual((min(zs), max(zs)), (-0.5, 0.5))   # -y, centered on SIZE

    def test_center_pivot(self):
        mesh = build((4, 5, 6), [(1, 2, 3, 1)], pivot="center")
        ys = [p[1] for p in mesh.positions]
        self.assertEqual((min(ys), max(ys)), (0.0, 1.0))    # 3 - 6/2

    def test_vox_front_minus_y_faces_plus_z(self):
        mesh = build((1, 3, 1), [(0, 0, 0, 1)])
        self.assertEqual(max(p[2] for p in mesh.positions), 1.5)  # y=0 -> +z side

    def test_scale(self):
        mesh = build((4, 5, 6), [(1, 2, 3, 1)], scale=0.5)
        xs = [p[0] for p in mesh.positions]
        self.assertEqual((min(xs), max(xs)), (-0.5, 0.0))

    def test_default_scale_is_one_sixteenth(self):
        data = vox.encode_vox((2, 2, 2), [(0, 0, 0, 1)], RGB_PALETTE)
        mesh = vox.build_mesh(vox.parse_vox(data))
        xs = [p[0] for p in mesh.positions]
        self.assertEqual((min(xs), max(xs)), (-1 / 16, 0.0))

    def test_importer_output_is_valid_p3mesh(self):
        data = vox.encode_vox((2, 2, 2), [(0, 0, 0, 1)], RGB_PALETTE)
        blob = vox.vox_to_p3mesh(data)
        mesh = pixelica_format.decode_mesh(blob)
        self.assertEqual(len(mesh.positions), 8)
        self.assertEqual(len(mesh.indices), 36)
        self.assertEqual(mesh.colors, [0xF800] * 12)
        self.assertIsNone(mesh.uvs)

    def test_bad_scale_and_pivot(self):
        model = vox.parse_vox(vox.encode_vox((1, 1, 1), [(0, 0, 0, 1)], RGB_PALETTE))
        for scale in (0, -1, float("nan"), float("inf"), "abc", None):
            with self.assertRaises(PixelicaPackError, msg=repr(scale)):
                vox.build_mesh(model, scale)
        with self.assertRaises(PixelicaPackError):
            vox.build_mesh(model, 1.0, "corner")

    def test_scale_that_overflows_binary32_is_rejected(self):
        data = vox.encode_vox((128, 1, 1), [(127, 0, 0, 1)], RGB_PALETTE)
        with self.assertRaises(PixelicaPackError):
            vox.vox_to_p3mesh(data, scale=1e37)

    def test_largest_model_dimensions(self):
        data = vox.encode_vox((128, 128, 128), [(127, 127, 127, 1)], RGB_PALETTE)
        mesh = vox.build_mesh(vox.parse_vox(data), 1.0)
        self.assertEqual(max(p[0] for p in mesh.positions), 64.0)
        self.assertEqual(max(p[1] for p in mesh.positions), 128.0)
        self.assertEqual(min(p[2] for p in mesh.positions), -64.0)


class MeshingTests(unittest.TestCase):
    def test_single_voxel(self):
        mesh = build((1, 1, 1), [(0, 0, 0, 1)])
        self.assertEqual(len(mesh.positions), 8)
        self.assertEqual(len(mesh.indices) // 3, 12)
        check_geometry(self, (1, 1, 1), [(0, 0, 0, 1)], RGB_PALETTE, mesh)

    def test_flat_plate_merges_to_six_quads(self):
        voxels = [(x, y, 0, 1) for x in range(4) for y in range(4)]
        mesh = build((4, 4, 1), voxels)
        self.assertEqual(len(mesh.indices) // 3, 12)
        self.assertEqual(len(mesh.positions), 8)
        check_geometry(self, (4, 4, 1), voxels, RGB_PALETTE, mesh)

    def test_different_palette_index_never_merges(self):
        voxels = [(0, 0, 0, 1), (1, 0, 0, 1), (2, 0, 0, 2)]
        mesh = build((3, 1, 1), voxels)
        # top, bottom, front, back are split in two, plus two end caps
        self.assertEqual(len(mesh.indices) // 3, 20)
        check_geometry(self, (3, 1, 1), voxels, RGB_PALETTE, mesh)

    def test_same_rgb565_but_different_index_is_not_merged(self):
        palette = [(255, 0, 0, 255), (255, 1, 1, 255)]
        self.assertEqual(rgb565(255, 0, 0), rgb565(255, 1, 1))
        voxels = [(0, 0, 0, 1), (1, 0, 0, 2)]
        mesh = build((2, 1, 1), voxels, palette)
        self.assertEqual(len(mesh.indices) // 3, 20)
        self.assertEqual(set(mesh.colors), {0xF800})

    def test_internal_faces_removed_across_colors(self):
        voxels = [(0, 0, 0, 1), (1, 0, 0, 2)]
        mesh = build((2, 1, 1), voxels)
        for t in range(len(mesh.indices) // 3):
            xs = {mesh.positions[mesh.indices[3 * t + n]][0] for n in range(3)}
            self.assertNotEqual(xs, {0.0}, "internal face between the voxels")
        self.assertEqual(len(mesh.indices) // 3, 20)
        check_geometry(self, (2, 1, 1), voxels, RGB_PALETTE, mesh)

    def test_hollow_cube_keeps_cavity_faces(self):
        voxels = [(x, y, z, 1) for x in range(3) for y in range(3) for z in range(3)
                  if (x, y, z) != (1, 1, 1)]
        mesh = build((3, 3, 3), voxels)
        check_geometry(self, (3, 3, 3), voxels, RGB_PALETTE, mesh)

    def test_random_models_against_oracle(self):
        for seed in range(25):
            rng = random.Random(seed)
            size = (rng.randint(1, 6), rng.randint(1, 5), rng.randint(1, 5))
            voxels = [(x, y, z, rng.randint(1, 3))
                      for x in range(size[0]) for y in range(size[1])
                      for z in range(size[2]) if rng.random() < 0.55]
            if not voxels:
                voxels = [(0, 0, 0, 1)]
            mesh = build(size, voxels)
            check_geometry(self, size, voxels, RGB_PALETTE, mesh)

    def test_output_does_not_depend_on_voxel_order(self):
        rng = random.Random(7)
        voxels = [(x, y, z, rng.randint(1, 3)) for x in range(5) for y in range(4)
                  for z in range(3) if rng.random() < 0.6]
        outputs = set()
        for _ in range(4):
            rng.shuffle(voxels)
            data = vox_file([vox_size(5, 4, 3), vox_xyzi(voxels),
                             vox_rgba({0: RED, 1: GREEN, 2: BLUE})])
            outputs.add(vox.vox_to_p3mesh(data))
        self.assertEqual(len(outputs), 1)

    def test_same_input_same_bytes(self):
        data = vox.encode_vox((3, 3, 3), [(1, 1, 1, 1), (0, 0, 0, 2)], RGB_PALETTE)
        self.assertEqual(vox.vox_to_p3mesh(data), vox.vox_to_p3mesh(data))

    def test_triangle_limit(self):
        voxels = [(x, y, z, 1) for x in range(26) for y in range(26)
                  for z in range(26) if (x + y + z) % 2 == 0]
        data = vox.encode_vox((26, 26, 26), voxels, RGB_PALETTE)
        with self.assertRaises(PixelicaPackError) as ctx:
            vox.vox_to_p3mesh(data)
        self.assertIn("triangles", str(ctx.exception))


class PaletteTests(unittest.TestCase):
    def test_xyzi_index_maps_to_rgba_entry_minus_one(self):
        rgba = {0: RED, 254: GREEN, 255: (9, 9, 9, 9)}
        data = vox_file([vox_size(2, 1, 1), vox_xyzi([(0, 0, 0, 1), (1, 0, 0, 255)]),
                         vox_rgba(rgba)])
        model = vox.parse_vox(data)
        self.assertTrue(model.custom_palette)
        self.assertEqual(model.palette[1], RED)
        self.assertEqual(model.palette[255], GREEN)
        self.assertEqual(model.palette[0], (0, 0, 0, 0))
        mesh = vox.build_mesh(model, 1.0)
        self.assertEqual(set(mesh.colors), {0xF800, 0x07E0})

    def test_default_palette_used_when_rgba_absent(self):
        data = vox_file([vox_size(1, 1, 1), vox_xyzi([(0, 0, 0, 2)])])
        model = vox.parse_vox(data)
        self.assertFalse(model.custom_palette)
        mesh = vox.build_mesh(model, 1.0)
        self.assertEqual(set(mesh.colors), {rgb565(255, 255, 204)})

    def test_default_palette_structure(self):
        pal = vox.DEFAULT_PALETTE
        self.assertEqual(len(pal), 256)
        self.assertEqual(pal[0], (0, 0, 0, 0))
        self.assertEqual(pal[1], (255, 255, 255, 255))
        self.assertEqual(pal[2], (255, 255, 204, 255))   # 0xffccffff
        self.assertEqual(pal[7], (255, 204, 255, 255))   # 0xffffccff
        self.assertEqual(pal[36], (255, 0, 0, 255))      # 0xff0000ff
        self.assertEqual(pal[37], (204, 255, 255, 255))  # 0xffffffcc
        self.assertTrue(all(c[3] == 255 for c in pal[1:]))
        self.assertEqual(len(set(pal[1:])), 255)
        self.assertEqual(vox.default_palette_words()[2], 0xFFCCFFFF)

    @unittest.skipUnless(os.environ.get("PIXELICA_PACK_VOX_REFERENCE"),
                         "set PIXELICA_PACK_VOX_REFERENCE to MagicaVoxel-file-format-vox.txt")
    def test_default_palette_matches_author_reference(self):
        with open(os.environ["PIXELICA_PACK_VOX_REFERENCE"], "r", encoding="utf-8",
                  errors="replace") as handle:
            problems = vox.check_default_palette(handle.read())
        self.assertEqual(problems, [])

    def test_reference_checker_detects_differences(self):
        words = vox.default_palette_words()
        text = "static unsigned int default_palette[256] = {\n%s\n};" % ",\n".join(
            "0x%08x" % w for w in words)
        self.assertEqual(vox.check_default_palette(text), [])
        words[5] ^= 1
        bad = text.replace("0x%08x" % vox.default_palette_words()[5], "0x%08x" % words[5], 1)
        self.assertEqual(len(vox.check_default_palette(bad)), 1)
        self.assertTrue(vox.check_default_palette("nothing here"))

    def test_nonopaque_used_color_fails_unused_is_fine(self):
        bad = vox_file([vox_size(1, 1, 1), vox_xyzi([(0, 0, 0, 1)]),
                        vox_rgba({0: (1, 2, 3, 128)})])
        with self.assertRaises(PixelicaPackError) as ctx:
            vox.parse_vox(bad)
        self.assertIn("alpha", str(ctx.exception))
        ok = vox_file([vox_size(1, 1, 1), vox_xyzi([(0, 0, 0, 2)]),
                       vox_rgba({0: (1, 2, 3, 0), 1: RED})])
        vox.parse_vox(ok)

    def test_rgba_length(self):
        for length in (0, 1020, 1028):
            data = vox_file([vox_size(1, 1, 1), vox_xyzi([(0, 0, 0, 1)]),
                             vox_chunk(b"RGBA", bytes(length))])
            with self.assertRaises(PixelicaPackError):
                vox.parse_vox(data)


def valid_parts():
    return [vox_size(2, 2, 2), vox_xyzi([(0, 0, 0, 1), (1, 1, 1, 2)])]


class ParserRejectionTests(unittest.TestCase):
    def reject(self, data, text=None):
        with self.assertRaises(PixelicaPackError) as ctx:
            vox.parse_vox(data)
        if text:
            self.assertIn(text, str(ctx.exception))

    def test_valid_file_parses(self):
        model = vox.parse_vox(vox_file(valid_parts()))
        self.assertEqual(model.size, (2, 2, 2))
        self.assertEqual(len(model.voxels), 2)

    def test_encoder_matches_independent_builder(self):
        self.assertEqual(vox.encode_vox((2, 2, 2), [(0, 0, 0, 1)]),
                         vox_file([vox_size(2, 2, 2), vox_xyzi([(0, 0, 0, 1)])]))

    def test_header(self):
        good = vox_file(valid_parts())
        self.reject(b"VOX!" + good[4:], "magic")
        self.reject(b"", "truncated")
        self.reject(b"VOX ", "truncated")
        for version in (0, 149, 151, 199, 201, -1):
            self.reject(vox_file(valid_parts(), version=version), "version")
        vox.parse_vox(vox_file(valid_parts(), version=200))
        vox.parse_vox(vox_file(valid_parts(), version=150))

    def test_main_chunk(self):
        self.reject(b"VOX " + struct.pack("<i", 150) + vox_size(1, 1, 1), "MAIN")
        bad = (b"VOX " + struct.pack("<i", 150)
               + struct.pack("<4sii", b"MAIN", 4, 0) + b"abcd")
        self.reject(bad, "no content")
        self.reject(vox_file(valid_parts(), main_children=5), "child bytes")
        total = sum(len(p) for p in valid_parts())
        self.reject(vox_file(valid_parts(), main_children=total + 1), "child bytes")
        self.reject(vox_file(valid_parts(), main_children=-1), "child bytes")

    def test_every_truncation_and_trailing_byte(self):
        data = vox_file(valid_parts() + [vox_rgba({0: RED})])
        for cut in range(len(data)):
            with self.assertRaises(PixelicaPackError, msg="prefix %d" % cut):
                vox.parse_vox(data[:cut])
        self.reject(data + b"\0", "child bytes")
        extra = b"abcde"
        self.reject(vox_file(valid_parts() + [extra]), "truncated chunk header")

    def test_unsupported_chunks_are_named(self):
        for tag in (b"nTRN", b"nGRP", b"nSHP", b"LAYR", b"MATL", b"MATT",
                    b"rOBJ", b"rCAM", b"NOTE", b"IMAP", b"ABCD"):
            for position in (0, 1, 2):
                parts = valid_parts()
                parts.insert(position, vox_chunk(tag, b"\0\0\0\0"))
                with self.assertRaises(PixelicaPackError) as ctx:
                    vox.parse_vox(vox_file(parts))
                text = str(ctx.exception)
                self.assertIn("unsupported", text)
                self.assertIn(tag.decode(), text)

    def test_unprintable_chunk_id(self):
        parts = valid_parts() + [vox_chunk(b"\xff\x00\x01\x02", b"")]
        self.reject(vox_file(parts), "unsupported")

    def test_chunks_with_children_are_rejected(self):
        parts = [vox_chunk(b"SIZE", struct.pack("<iii", 1, 1, 1), b"xxxx"),
                 vox_xyzi([(0, 0, 0, 1)])]
        self.reject(vox_file(parts), "child")

    def test_bad_chunk_lengths(self):
        negative = struct.pack("<4sii", b"SIZE", -1, 0)
        self.reject(vox_file([negative]), "invalid lengths")
        negative_kids = struct.pack("<4sii", b"SIZE", 12, -1) + bytes(12)
        self.reject(vox_file([negative_kids]), "invalid lengths")
        too_long = struct.pack("<4sii", b"SIZE", 1000, 0) + bytes(12)
        self.reject(vox_file([too_long]), "invalid lengths")
        for content in (bytes(8), bytes(16), b""):
            self.reject(vox_file([vox_chunk(b"SIZE", content),
                                  vox_xyzi([(0, 0, 0, 1)])]), "SIZE content")

    def test_ordering_and_duplicates(self):
        size, xyzi = valid_parts()
        self.reject(vox_file([xyzi, size]), "before SIZE")
        self.reject(vox_file([xyzi]), "before SIZE")
        self.reject(vox_file([size]), "missing XYZI")
        self.reject(vox_file([xyzi, vox_rgba({})]), "before SIZE")
        self.reject(vox_file([size, vox_rgba({}), xyzi]), "out of order")
        self.reject(vox_file([size, size, xyzi]), "duplicate")
        self.reject(vox_file([size, xyzi, xyzi]), "duplicate")
        self.reject(vox_file([size, xyzi, vox_rgba({}), vox_rgba({})]), "duplicate")
        self.reject(vox_file([]), "missing SIZE")

    def test_pack_chunk(self):
        pack = lambda n: vox_chunk(b"PACK", struct.pack("<i", n))
        size, xyzi = valid_parts()
        vox.parse_vox(vox_file([pack(1), size, xyzi]))
        self.reject(vox_file([pack(2), size, xyzi]), "only 1")
        self.reject(vox_file([pack(0), size, xyzi]), "only 1")
        self.reject(vox_file([size, pack(1), xyzi]), "out of order")
        self.reject(vox_file([vox_chunk(b"PACK", bytes(8)), size, xyzi]), "PACK")

    def test_size_limits(self):
        xyzi = vox_xyzi([(0, 0, 0, 1)])
        for dims in ((0, 1, 1), (1, 0, 1), (1, 1, 0), (-1, 1, 1), (129, 1, 1),
                     (1, 129, 1), (1, 1, 129), (2 ** 31 - 1, 1, 1)):
            self.reject(vox_file([vox_size(*dims), xyzi]), "SIZE")
        vox.parse_vox(vox_file([vox_size(128, 128, 128), xyzi]))

    def test_voxel_validation(self):
        size = vox_size(4, 3, 2)
        self.reject(vox_file([size, vox_xyzi([])]), "empty")
        self.reject(vox_file([size, vox_xyzi([(0, 0, 0, 0)])]), "color index 0")
        self.reject(vox_file([size, vox_xyzi([(1, 1, 1, 3), (1, 1, 1, 4)])]),
                    "duplicate voxel")
        self.reject(vox_file([size, vox_xyzi([(4, 0, 0, 1)])]), "outside SIZE")
        self.reject(vox_file([size, vox_xyzi([(0, 3, 0, 1)])]), "outside SIZE")
        self.reject(vox_file([size, vox_xyzi([(0, 0, 2, 1)])]), "outside SIZE")
        self.reject(vox_file([size, vox_xyzi([(255, 255, 255, 1)])]), "outside SIZE")
        vox.parse_vox(vox_file([size, vox_xyzi([(3, 2, 1, 255)])]))

    def test_xyzi_content_length_must_match_count(self):
        size = vox_size(4, 4, 4)
        body = struct.pack("<i", 2) + bytes((0, 0, 0, 1))
        self.reject(vox_file([size, vox_chunk(b"XYZI", body)]), "XYZI content")
        body = struct.pack("<i", 1) + bytes((0, 0, 0, 1)) + b"\0"
        self.reject(vox_file([size, vox_chunk(b"XYZI", body)]), "XYZI content")
        self.reject(vox_file([size, vox_chunk(b"XYZI", b"\1")]), "too short")
        negative = struct.pack("<i", -1)
        self.reject(vox_file([size, vox_chunk(b"XYZI", negative)]), "voxel count")

    def test_voxel_count_limit(self):
        size = vox_size(128, 128, 128)
        over = struct.pack("<i", 1000001) + bytes(4 * 1000001)
        self.reject(vox_file([size, vox_chunk(b"XYZI", over)]), "voxel count")
        huge = struct.pack("<i", 2 ** 31 - 1)
        self.reject(vox_file([size, vox_chunk(b"XYZI", huge)]), "voxel count")

    def test_one_million_voxels_is_accepted(self):
        cells = itertools.islice(
            ((x, y, z, 1) for z in range(128) for y in range(128) for x in range(128)),
            1000000)
        body = bytearray(struct.pack("<i", 1000000))
        for cell in cells:
            body += bytes(cell)
        data = vox_file([vox_size(128, 128, 128), vox_chunk(b"XYZI", bytes(body))])
        self.assertEqual(len(vox.parse_vox(data).voxels), 1000000)

    def test_file_over_16mib(self):
        self.reject(b"VOX " + bytes(pixelica_format.MAX_FILE_BYTES), "limit")

    def test_fuzz_only_raises_pixelica_packerror(self):
        rng = random.Random(99)
        good = vox_file(valid_parts() + [vox_rgba({0: RED, 1: GREEN})])
        for _ in range(500):
            data = bytearray(good)
            for _k in range(rng.randint(1, 3)):
                data[rng.randrange(len(data))] = rng.randrange(256)
            try:
                vox.vox_to_p3mesh(bytes(data))
            except PixelicaPackError:
                pass


class DescribeTests(unittest.TestCase):
    def test_describe(self):
        data = vox_file(valid_parts())
        info = vox.describe_vox(data)
        self.assertEqual(info["format"], "VOX")
        self.assertEqual(info["size"], [2, 2, 2])
        self.assertEqual(info["voxel_count"], 2)
        self.assertEqual(info["palette"], "default")
        self.assertEqual(info["used_colors"], 2)
        self.assertEqual(info["version"], 150)


if __name__ == "__main__":
    unittest.main()
