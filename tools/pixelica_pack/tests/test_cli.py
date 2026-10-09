import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest

import support
from support import vox_file, vox_size, vox_xyzi

import pixelica_format
import pixelica_pack
import vox

SCRIPT = os.path.join(support.ROOT, "pixelica_pack.py")
PALETTE = [(255, 0, 0, 255), (0, 255, 0, 255)]


def run(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = pixelica_pack.main(list(argv))
        except SystemExit as exc:
            code = exc.code
    return code, out.getvalue(), err.getvalue()


class CliTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = self._tmp.name

    def path(self, name):
        return os.path.join(self.dir, name)

    def write(self, name, data):
        with open(self.path(name), "wb") as handle:
            handle.write(data)
        return self.path(name)

    def read(self, name):
        with open(self.path(name), "rb") as handle:
            return handle.read()

    def listing(self):
        return sorted(os.listdir(self.dir))

    def good_vox(self, name="a.vox"):
        return self.write(name, vox.encode_vox(
            (4, 3, 2), [(0, 0, 0, 1), (1, 0, 0, 1), (3, 2, 1, 2)], PALETTE))


class ImportTests(CliTestCase):
    def test_import_success(self):
        src = self.good_vox()
        code, out, err = run("import", "vox", src, "-o", self.path("a.p3mesh"))
        self.assertEqual((code, err), (0, ""))
        self.assertIn("wrote", out)
        mesh = pixelica_format.decode_mesh(self.read("a.p3mesh"))
        self.assertGreater(len(mesh.positions), 8)
        self.assertEqual(self.listing(), ["a.p3mesh", "a.vox"])

    def test_reproducible_regardless_of_time_and_location(self):
        src = self.good_vox()
        run("import", "vox", src, "-o", self.path("1.p3mesh"))
        later = time.time() + 1000
        os.utime(src, (later, later))
        os.makedirs(self.path("sub"))
        run("import", "vox", src, "-o", self.path(os.path.join("sub", "2.p3mesh")))
        self.assertEqual(self.read("1.p3mesh"), self.read(os.path.join("sub", "2.p3mesh")))

    def test_scale_and_pivot_options(self):
        src = self.good_vox()
        run("import", "vox", src, "-o", self.path("def.p3mesh"))
        run("import", "vox", src, "-o", self.path("big.p3mesh"), "--scale", "1",
            "--pivot", "center")
        small = pixelica_format.decode_mesh(self.read("def.p3mesh"))
        big = pixelica_format.decode_mesh(self.read("big.p3mesh"))
        self.assertEqual(max(p[0] for p in small.positions), 2 / 16)
        self.assertEqual(max(p[0] for p in big.positions), 2.0)
        self.assertEqual(min(p[1] for p in small.positions), 0.0)
        self.assertEqual(min(p[1] for p in big.positions), -1.0)

    def test_invalid_input_writes_nothing(self):
        src = self.write("bad.vox", b"VOX " + bytes(40))
        code, out, err = run("import", "vox", src, "-o", self.path("x.p3mesh"))
        self.assertEqual(code, 1)
        self.assertEqual(out, "")
        self.assertTrue(err.startswith("pixelica_pack: error:"))
        self.assertEqual(self.listing(), ["bad.vox"])

    def test_failure_keeps_existing_output(self):
        self.write("keep.p3mesh", b"OLD CONTENT")
        src = self.write("bad.vox", b"not a vox file")
        code, _out, _err = run("import", "vox", src, "-o", self.path("keep.p3mesh"))
        self.assertEqual(code, 1)
        self.assertEqual(self.read("keep.p3mesh"), b"OLD CONTENT")
        self.assertEqual(self.listing(), ["bad.vox", "keep.p3mesh"])

    def test_success_replaces_existing_output(self):
        self.write("o.p3mesh", b"OLD")
        run("import", "vox", self.good_vox(), "-o", self.path("o.p3mesh"))
        pixelica_format.decode_mesh(self.read("o.p3mesh"))

    def test_unsupported_scene_graph_message(self):
        data = vox_file([vox_size(1, 1, 1), vox_xyzi([(0, 0, 0, 1)]),
                         support.vox_chunk(b"nTRN", bytes(8))])
        code, _out, err = run("import", "vox", self.write("s.vox", data),
                              "-o", self.path("s.p3mesh"))
        self.assertEqual(code, 1)
        self.assertIn("unsupported chunk 'nTRN'", err)

    def test_missing_input_and_missing_output_directory(self):
        code, _o, err = run("import", "vox", self.path("nope.vox"),
                            "-o", self.path("x.p3mesh"))
        self.assertEqual(code, 1)
        self.assertIn("cannot read", err)
        src = self.good_vox()
        code, _o, err = run("import", "vox", src,
                            "-o", self.path(os.path.join("missing", "x.p3mesh")))
        self.assertEqual(code, 1)
        self.assertIn("directory", err)

    def test_input_is_never_overwritten(self):
        src = self.good_vox()
        before = self.read("a.vox")
        code, _o, err = run("import", "vox", src, "-o", src)
        self.assertEqual(code, 1)
        self.assertIn("overwrite", err)
        self.assertEqual(self.read("a.vox"), before)

    def test_output_path_is_directory(self):
        os.makedirs(self.path("d"))
        code, _o, _e = run("import", "vox", self.good_vox(), "-o", self.path("d"))
        self.assertEqual(code, 1)

    def test_usage_errors_exit_2(self):
        src = self.good_vox()
        out = self.path("x.p3mesh")
        for argv in ((), ("import",), ("import", "vox"), ("import", "vox", src),
                     ("import", "vox", src, "-o", out, "--scale", "0"),
                     ("import", "vox", src, "-o", out, "--scale", "-1"),
                     ("import", "vox", src, "-o", out, "--scale", "nan"),
                     ("import", "vox", src, "-o", out, "--scale", "inf"),
                     ("import", "vox", src, "-o", out, "--scale", "abc"),
                     ("import", "vox", src, "-o", out, "--pivot", "corner"),
                     ("frobnicate",)):
            code, _o, _e = run(*argv)
            self.assertEqual(code, 2, argv)
        self.assertFalse(os.path.exists(out))

    def test_triangle_limit_error_is_friendly(self):
        voxels = [(x, y, z, 1) for x in range(26) for y in range(26)
                  for z in range(26) if (x + y + z) % 2 == 0]
        src = self.write("big.vox", vox.encode_vox((26, 26, 26), voxels, PALETTE))
        code, _o, err = run("import", "vox", src, "-o", self.path("big.p3mesh"))
        self.assertEqual(code, 1)
        self.assertIn("triangles", err)
        self.assertNotIn("Traceback", err)
        self.assertFalse(os.path.exists(self.path("big.p3mesh")))


class TextureTests(CliTestCase):
    def test_texture(self):
        src = self.write("t.ppm", b"P6\n2 1\n255\n\xff\x00\x00\x00\x00\xff")
        code, out, _e = run("texture", src, "-o", self.path("t.p3tex"))
        self.assertEqual(code, 0)
        self.assertIn("wrote", out)
        tex = pixelica_format.decode_texture(self.read("t.p3tex"))
        self.assertEqual(list(tex.pixels), [0xF800, 0x001F])

    def test_bad_ppm_writes_nothing(self):
        src = self.write("t.ppm", b"P6\n2 1\n255\n\xff")
        code, _o, err = run("texture", src, "-o", self.path("t.p3tex"))
        self.assertEqual(code, 1)
        self.assertIn("PPM", err)
        self.assertEqual(self.listing(), ["t.ppm"])


class BundleTests(CliTestCase):
    def make_resources(self):
        mesh = pixelica_format.encode_mesh(vox.build_mesh(vox.parse_vox(
            vox.encode_vox((1, 1, 1), [(0, 0, 0, 1)], PALETTE))))
        tex = pixelica_format.encode_texture(pixelica_format.TextureData(1, 1, [5]))
        self.write("m.p3mesh", mesh)
        self.write("t.p3tex", tex)
        return mesh, tex

    def test_bundle_sorted_ids(self):
        mesh, tex = self.make_resources()
        code, out, err = run("bundle", "-o", self.path("c.p3cart"),
                             "zebra=" + self.path("t.p3tex"),
                             "apple=" + self.path("m.p3mesh"))
        self.assertEqual((code, err), (0, ""))
        self.assertIn("2 resources", out)
        entries = pixelica_format.decode_cart(self.read("c.p3cart"))
        self.assertEqual([(e.resource_id, e.name) for e in entries],
                         [(1, "apple"), (2, "zebra")])
        self.assertEqual(entries[0].data, mesh)
        self.assertEqual(entries[1].data, tex)

    def test_bundle_is_reproducible_in_any_argument_order(self):
        self.make_resources()
        a = ["x=" + self.path("m.p3mesh"), "y=" + self.path("t.p3tex")]
        run("bundle", "-o", self.path("1.p3cart"), *a)
        run("bundle", "-o", self.path("2.p3cart"), *reversed(a))
        self.assertEqual(self.read("1.p3cart"), self.read("2.p3cart"))

    def test_bundle_errors_write_nothing(self):
        self.make_resources()
        self.write("junk.bin", b"junk data here")
        good = "g=" + self.path("m.p3mesh")
        cases = [
            ["noequals"],
            ["=" + self.path("m.p3mesh")],
            ["name="],
            ["bad name=" + self.path("m.p3mesh")],
            [good, "g=" + self.path("t.p3tex")],
            ["j=" + self.path("junk.bin")],
            ["n=" + self.path("missing.p3mesh")],
        ]
        for resources in cases:
            code, out, err = run("bundle", "-o", self.path("c.p3cart"), *resources)
            self.assertEqual(code, 1, resources)
            self.assertEqual(out, "")
            self.assertIn("pixelica_pack: error:", err)
        self.assertEqual(self.listing(), ["junk.bin", "m.p3mesh", "t.p3tex"])

    def test_bundle_rejects_corrupt_resource(self):
        mesh, _tex = self.make_resources()
        self.write("bad.p3mesh", mesh[:-1] + bytes([mesh[-1] ^ 1]))
        code, _o, err = run("bundle", "-o", self.path("c.p3cart"),
                            "bad=" + self.path("bad.p3mesh"))
        self.assertEqual(code, 1)
        self.assertIn("invalid", err)

    def test_bundle_without_resources_is_usage_error(self):
        code, _o, _e = run("bundle", "-o", self.path("c.p3cart"))
        self.assertEqual(code, 2)


class InspectTests(CliTestCase):
    def inspect(self, name):
        code, out, err = run("inspect", self.path(name))
        self.assertEqual((code, err), (0, ""))
        return out, json.loads(out)

    def test_inspect_all_kinds(self):
        self.good_vox("a.vox")
        run("import", "vox", self.path("a.vox"), "-o", self.path("a.p3mesh"))
        self.write("t.ppm", b"P3\n1 1\n255\n255 255 255\n")
        run("texture", self.path("t.ppm"), "-o", self.path("t.p3tex"))
        run("bundle", "-o", self.path("c.p3cart"), "mesh=" + self.path("a.p3mesh"),
            "tex=" + self.path("t.p3tex"))

        _t, info = self.inspect("a.vox")
        self.assertEqual((info["format"], info["voxel_count"]), ("VOX", 3))
        _t, info = self.inspect("a.p3mesh")
        self.assertEqual(info["format"], "P3MS")
        self.assertEqual(info["unique_colors"], 2)
        _t, info = self.inspect("t.p3tex")
        self.assertEqual((info["width"], info["height"]), (1, 1))
        text, info = self.inspect("c.p3cart")
        self.assertEqual(info["entry_count"], 2)
        self.assertEqual(info["entries"][0]["asset"]["format"], "P3MS")
        self.assertEqual(text, run("inspect", self.path("c.p3cart"))[1])
        self.assertEqual(json.dumps(info, indent=2, sort_keys=True) + "\n", text)

    def test_inspect_rejects_invalid(self):
        self.write("bad.bin", b"P3MS" + bytes(40))
        code, out, err = run("inspect", self.path("bad.bin"))
        self.assertEqual((code, out), (1, ""))
        self.assertIn("pixelica_pack: error:", err)

    def test_inspect_detects_corruption_inside_a_cart(self):
        mesh = pixelica_format.encode_mesh(vox.build_mesh(vox.parse_vox(
            vox.encode_vox((1, 1, 1), [(0, 0, 0, 1)], PALETTE))))
        cart = bytearray(pixelica_format.bundle_named([("m", mesh)]))
        cart[-5] ^= 0xFF  # a byte inside DATA: the outer CRC32 must catch it
        self.write("c.p3cart", bytes(cart))
        code, _o, err = run("inspect", self.path("c.p3cart"))
        self.assertEqual(code, 1)
        self.assertIn("CRC32", err)


class SubprocessTests(CliTestCase):
    def call(self, *args):
        return subprocess.run([sys.executable, "-I", SCRIPT] + list(args),
                              capture_output=True, text=True, timeout=120)

    def test_script_entry_point_in_isolated_mode(self):
        src = self.good_vox()
        done = self.call("import", "vox", src, "-o", self.path("a.p3mesh"))
        self.assertEqual(done.returncode, 0, done.stderr)
        done = self.call("inspect", self.path("a.p3mesh"))
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(json.loads(done.stdout)["format"], "P3MS")

    def test_script_failure_exit_status(self):
        bad = self.write("bad.vox", b"nope")
        done = self.call("import", "vox", bad, "-o", self.path("x.p3mesh"))
        self.assertEqual(done.returncode, 1)
        self.assertIn("pixelica_pack: error:", done.stderr)
        self.assertNotIn("Traceback", done.stderr)
        done = self.call()
        self.assertEqual(done.returncode, 2)


if __name__ == "__main__":
    unittest.main()
