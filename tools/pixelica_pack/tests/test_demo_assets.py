import contextlib
import io
import os
import re
import tempfile
import unittest

import support

import demo_models
import generate_demo_assets as gen
import pixelica_format
import ppm
import vox

NAMES = ("desk", "chair", "book", "cup", "plant")


def components(cells):
    """4-connected component count of a set of (x, y) cells."""
    remaining = set(cells)
    count = 0
    while remaining:
        count += 1
        stack = [remaining.pop()]
        while stack:
            x, y = stack.pop()
            for n in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                if n in remaining:
                    remaining.remove(n)
                    stack.append(n)
    return count


class DemoAssetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.outputs = gen.build_outputs()
        cls.files = dict(cls.outputs)

    def test_expected_file_set(self):
        expected = set()
        for n in NAMES:
            expected |= {"assets/source/vox/%s.vox" % n,
                         "assets/compiled/%s.p3mesh" % n}
        expected |= {"assets/source/textures/checker.ppm",
                     "assets/compiled/checker.p3tex",
                     "assets/compiled/room.p3cart",
                     "assets/compiled/pixel_room_assets.h"}
        self.assertEqual(set(self.files), expected)
        self.assertEqual(len(self.files), len(self.outputs))

    def test_generation_is_reproducible(self):
        self.assertEqual(gen.build_outputs(), self.outputs)

    def test_compiled_meshes_come_from_the_real_importer(self):
        for n in NAMES:
            source = self.files["assets/source/vox/%s.vox" % n]
            self.assertEqual(self.files["assets/compiled/%s.p3mesh" % n],
                             vox.vox_to_p3mesh(source))

    def test_meshes_are_valid_and_within_budget(self):
        for n in NAMES:
            mesh = pixelica_format.decode_mesh(self.files["assets/compiled/%s.p3mesh" % n])
            self.assertGreater(len(mesh.indices) // 3, 12, n)
            self.assertLess(len(mesh.indices) // 3, 20000, n)
            self.assertGreaterEqual(len(set(mesh.colors)), 3, n)
            self.assertEqual(len(set(mesh.positions)), len(mesh.positions))

    def test_sources_fit_the_art_grid(self):
        for n in NAMES:
            model = vox.parse_vox(self.files["assets/source/vox/%s.vox" % n])
            self.assertTrue(model.custom_palette)
            self.assertTrue(8 <= max(model.size) <= 64, (n, model.size))
            self.assertGreater(len(model.voxels), 100, n)

    def test_checker_texture(self):
        source = self.files["assets/source/textures/checker.ppm"]
        tex = pixelica_format.decode_texture(self.files["assets/compiled/checker.p3tex"])
        self.assertEqual((tex.width, tex.height), (32, 32))
        self.assertEqual(self.files["assets/compiled/checker.p3tex"],
                         ppm.ppm_to_p3tex(source))
        self.assertEqual(len(set(tex.pixels)), 3)

    def test_cart_contents_and_ids(self):
        entries = pixelica_format.decode_cart(self.files["assets/compiled/room.p3cart"])
        self.assertEqual([e.name for e in entries],
                         ["book", "chair", "checker", "cup", "desk", "plant"])
        self.assertEqual([e.resource_id for e in entries], [1, 2, 3, 4, 5, 6])
        for e in entries:
            ext = ".p3tex" if e.kind == b"P3TX" else ".p3mesh"
            self.assertEqual(e.data, self.files["assets/compiled/%s%s" % (e.name, ext)])

    def header_arrays(self):
        text = self.files["assets/compiled/pixel_room_assets.h"].decode("ascii")
        arrays = {}
        for match in re.finditer(r"static const uint8_t (\w+)\[\] = \{(.*?)\};",
                                 text, re.DOTALL):
            arrays[match.group(1)] = bytes(
                int(h, 16) for h in re.findall(r"0x([0-9a-f]{2})", match.group(2)))
        return text, arrays

    def test_header_embeds_exact_file_bytes_and_symbols(self):
        text, arrays = self.header_arrays()
        expected = {
            "pixelica_asset_desk": "desk.p3mesh", "pixelica_asset_chair": "chair.p3mesh",
            "pixelica_asset_book": "book.p3mesh", "pixelica_asset_cup": "cup.p3mesh",
            "pixelica_asset_plant": "plant.p3mesh", "pixelica_asset_checker": "checker.p3tex",
            "pixelica_asset_room_cart": "room.p3cart",
        }
        self.assertEqual(set(arrays), set(expected))
        for symbol, filename in expected.items():
            self.assertEqual(arrays[symbol], self.files["assets/compiled/" + filename])
            self.assertIn("static const size_t %s_size = sizeof %s;" % (symbol, symbol),
                          text)
        for ident in ("#include <stddef.h>", "#include <stdint.h>",
                      "#ifndef PIXELICA_PIXEL_ROOM_ASSETS_H"):
            self.assertIn(ident, text)

    def test_header_id_macros_match_cart(self):
        text, _arrays = self.header_arrays()
        for name, rid in (("BOOK", 1), ("CHAIR", 2), ("CHECKER", 3), ("CUP", 4),
                          ("DESK", 5), ("PLANT", 6)):
            self.assertIn("#define PIXELICA_ROOM_CART_ID_%s %du" % (name, rid), text)

    def test_header_has_no_nondeterministic_content(self):
        text = self.files["assets/compiled/pixel_room_assets.h"].decode("ascii")
        self.assertNotIn("\r", text)
        self.assertTrue(text.endswith("\n"))
        self.assertNotRegex(text, r"20\d\d-\d\d-\d\d")

    # ---- recognizable shapes ----
    def cells(self, name):
        model = vox.parse_vox(self.files["assets/source/vox/%s.vox" % name])
        return model, {(x, y, z) for x, y, z, _c in model.voxels}

    def test_desk_has_four_legs_and_a_top(self):
        model, cells = self.cells("desk")
        self.assertEqual(components({(x, y) for x, y, z in cells if z == 0}), 4)
        top = {(x, y) for x, y, z in cells if z == model.size[2] - 1}
        self.assertGreater(len(top), 0.9 * model.size[0] * model.size[1])

    def test_chair_has_four_legs_seat_and_taller_back(self):
        model, cells = self.cells("chair")
        self.assertEqual(components({(x, y) for x, y, z in cells if z == 0}), 4)
        seat = [c for c in cells if c[2] == 21]
        self.assertGreater(len(seat), 300)
        back = [c for c in cells if c[2] >= 30]
        self.assertTrue(back)
        self.assertGreater(min(y for _x, y, _z in back), model.size[1] // 2)

    def test_book_is_flat(self):
        model, cells = self.cells("book")
        sx, sy, sz = model.size
        self.assertLessEqual(sz * 2, min(sx, sy))
        self.assertGreater(len(cells), 0.6 * sx * sy * sz)

    def test_cup_is_hollow_with_handle(self):
        model, cells = self.cells("cup")
        self.assertNotIn((3, 3, 8), cells)       # open top
        self.assertIn((3, 3, 6), cells)          # coffee surface
        self.assertIn((0, 3, 8), cells)          # rim
        self.assertIn((9, 3, 4), cells)          # handle on +x
        self.assertGreater(model.size[0], model.size[1])

    def test_plant_has_pot_stem_leaves_and_flower(self):
        model, cells = self.cells("plant")
        pot_floor = {(x, y) for x, y, z in cells if z == 0}
        self.assertEqual(components(pot_floor), 1)
        self.assertGreater(len(pot_floor), 60)
        self.assertEqual(max(z for _x, _y, z in cells), model.size[2] - 1)
        greens = [c for c in model.palette[1:] if c[1] > c[0] and c[1] > c[2]]
        used = {c for _x, _y, _z, c in model.voxels}
        self.assertGreaterEqual(len([i for i in used if model.palette[i] in greens]), 3)
        stem_top = {(x, y) for x, y, z in cells if z == 33}
        self.assertLess(len(stem_top), 60)

    def test_every_prop_is_connected_in_3d(self):
        for n in NAMES:
            _model, cells = self.cells(n)
            remaining = set(cells)
            stack = [remaining.pop()]
            while stack:
                x, y, z = stack.pop()
                for d in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0),
                          (0, 0, 1), (0, 0, -1)):
                    nb = (x + d[0], y + d[1], z + d[2])
                    if nb in remaining:
                        remaining.remove(nb)
                        stack.append(nb)
            self.assertEqual(remaining, set(), "%s has floating voxels" % n)


class GeneratorCliTests(unittest.TestCase):
    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = gen.main(list(argv))
        return code, out.getvalue()

    def test_write_and_check_cycle(self):
        with tempfile.TemporaryDirectory() as root:
            code, out = self.run_main("--output-root", root)
            self.assertEqual(code, 0)
            self.assertIn("pixel_room_assets.h", out)
            code, out = self.run_main("--output-root", root, "--check")
            self.assertEqual((code, out.strip()), (0, "assets up to date"))
            target = os.path.join(root, "assets", "compiled", "desk.p3mesh")
            with open(target, "ab") as handle:
                handle.write(b"x")
            code, out = self.run_main("--output-root", root, "--check")
            self.assertEqual(code, 1)
            self.assertIn("differs: assets/compiled/desk.p3mesh", out)
            os.remove(os.path.join(root, "assets", "compiled", "room.p3cart"))
            code, out = self.run_main("--output-root", root, "--check")
            self.assertIn("missing: assets/compiled/room.p3cart", out)

    def test_two_roots_are_byte_identical(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.run_main("--output-root", a)
            self.run_main("--output-root", b)
            for rel, _data in gen.build_outputs():
                path = os.path.join(*rel.split("/"))
                with open(os.path.join(a, path), "rb") as fa, \
                        open(os.path.join(b, path), "rb") as fb:
                    self.assertEqual(fa.read(), fb.read(), rel)

    def test_default_root_is_the_repository_not_the_cwd(self):
        self.assertTrue(os.path.isdir(os.path.join(gen.REPO_ROOT, "tools", "pixelica_pack")))


@unittest.skipUnless(
    os.path.isfile(os.path.join(gen.REPO_ROOT, "assets", "compiled",
                                "pixel_room_assets.h")),
    "workspace assets have not been generated yet")
class CommittedAssetsTests(unittest.TestCase):
    def test_workspace_assets_match_the_generator(self):
        self.assertEqual(gen.check_outputs(gen.REPO_ROOT, gen.build_outputs()), [])


class ModelHelperTests(unittest.TestCase):
    def test_model_rejects_out_of_range_cells(self):
        m = demo_models.Model(2, 2, 2)
        with self.assertRaises(ValueError):
            m.set(2, 0, 0, (1, 2, 3))

    def test_palette_indices_follow_first_use(self):
        m = demo_models.Model(2, 1, 1)
        m.set(0, 0, 0, (10, 20, 30))
        m.set(1, 0, 0, (40, 50, 60))
        self.assertEqual(m.cells[(0, 0, 0)], 1)
        self.assertEqual(m.cells[(1, 0, 0)], 2)
        self.assertEqual(vox.parse_vox(m.to_vox()).palette[2], (40, 50, 60, 255))


if __name__ == "__main__":
    unittest.main()
