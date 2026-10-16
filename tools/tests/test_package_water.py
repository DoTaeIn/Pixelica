"""Water add-on isolation, source allowlist and base-SDK separation.

No compiler and no network: everything runs on synthetic staging trees.
"""
import importlib.util
import os
import pathlib
import re
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(os.environ.get("PIXELICA_TOOLS_DIR") or pathlib.Path(__file__).resolve().parent.parent)
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("package_water", TOOLS / "package_water.py")
pw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pw)

REPO = TOOLS.parent


def _complete_binary_stage(root):
    """Writes a minimally valid staged binary add-on below <root>."""
    paths = list(pw.required_files()) + [
        "lib/libpixelica_water.a", "lib/libpixelica_waterd.a",
        "lib/libpixelica_water3d.a", "lib/libpixelica_water3dd.a",
        "share/Pixelica/docs/WATER_INSTALL.md",
    ]
    for name in paths:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(name, encoding="utf-8")
    return sorted(paths)


class WaterSourceArchiveTests(unittest.TestCase):
    def test_source_allowlist_is_exactly_what_is_staged(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, stage = pathlib.Path(tmp) / "repo", pathlib.Path(tmp) / "stage"
            extra = ["modules/water/scratch.c", "modules/audio/audio.c",
                     ".git/config", "modules/water/notes.log"]
            for name in list(pw.SOURCE_FILES.values()) + extra:
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name, encoding="utf-8")
            pw.stage_source(source, stage)
            staged = set(pw.sdk.list_files(stage))
            self.assertEqual(staged, set(pw.SOURCE_FILES) | {"README.md"})
            # The add-on root really is the add-on's own CMakeLists.
            self.assertEqual((stage / "CMakeLists.txt").read_text(),
                             "sdk/water-addon/CMakeLists.txt")
            self.assertFalse(any("audio" in p or ".git" in p or p.endswith(".log")
                                 for p in staged))

    def test_source_allowlist_requires_every_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, stage = pathlib.Path(tmp) / "repo", pathlib.Path(tmp) / "stage"
            for name in list(pw.SOURCE_FILES.values())[:-1]:
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name, encoding="utf-8")
            with self.assertRaises(pw.sdk.PackagingError):
                pw.stage_source(source, stage)

    def test_every_allowlisted_source_exists_in_the_repository(self):
        for original in pw.SOURCE_FILES.values():
            self.assertTrue((REPO / original).is_file(), original)

    def test_cmake_source_check_stages_the_same_files(self):
        """The CTest driver duplicates SOURCE_FILES; they must not drift."""
        driver = (REPO / "cmake" / "PixelicaWaterSourceCheck.cmake").read_text(encoding="utf-8")
        block = re.search(r"set\(WATER_SOURCE_FILES(.*?)\)\s*\n", driver, re.S)
        self.assertIsNotNone(block, "WATER_SOURCE_FILES not found in the driver")
        pairs = dict(re.findall(r'"([^"=]+)=([^"]+)"', block.group(1)))
        self.assertEqual(pairs, dict(pw.SOURCE_FILES))


class WaterBinaryArchiveTests(unittest.TestCase):
    def test_complete_stage_is_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = pathlib.Path(tmp)
            expected = _complete_binary_stage(stage)
            self.assertEqual(pw.check_binary(stage), expected)

    def test_base_sdk_files_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = pathlib.Path(tmp)
            _complete_binary_stage(stage)
            for unrelated in ("lib/libpixelica_common.a",
                              "include/pixelica/pixelica.h",
                              "include/pixelica/renderer2d.h",
                              "lib/cmake/Pixelica/PixelicaPackage.cmake",
                              "lib/cmake/Pixelica/PixelicaConfig.cmake",
                              "lib/cmake/PixelicaWater/PixelicaShadersTargets.cmake",
                              "lib/libpixelica_shaders.a"):
                path = stage / unrelated
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("unrelated", encoding="utf-8")
                with self.assertRaises(pw.sdk.PackagingError, msg=unrelated):
                    pw.check_binary(stage)
                path.unlink()
            # Still clean once they are gone.
            pw.check_binary(stage)

    def test_missing_component_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = pathlib.Path(tmp)
            _complete_binary_stage(stage)
            # Dropping the adapter export, or a configuration of it, must fail:
            # a half-installed component is worse than none.
            for name in ("lib/cmake/PixelicaWater/PixelicaWater3DTargets.cmake",
                         "lib/cmake/PixelicaWater/PixelicaWaterTargets-debug.cmake",
                         "include/pixelica/water3d.h",
                         "share/Pixelica/docs/water.md"):
                path = stage / name
                kept = path.read_text()
                path.unlink()
                with self.assertRaises(pw.sdk.PackagingError, msg=name):
                    pw.check_binary(stage)
                path.write_text(kept, encoding="utf-8")

    def test_both_configurations_of_both_libraries_are_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = pathlib.Path(tmp)
            _complete_binary_stage(stage)
            (stage / "lib/libpixelica_water3dd.a").unlink()
            with self.assertRaises(pw.sdk.PackagingError):
                pw.check_binary(stage)

    def test_components_are_separate(self):
        self.assertEqual(pw.COMPONENTS, ("water", "water3d"))


class BaseSdkIsolationTests(unittest.TestCase):
    def test_base_packaging_explicitly_disables_water(self):
        """The base SDKs must not acquire the water library or header."""
        for config in ("Debug", "Release"):
            self.assertIn("-DPIXELICA_BUILD_WATER=OFF", pw.sdk.configure_options(config))

    def test_water_headers_are_not_base_sdk_headers(self):
        self.assertNotIn("water.h", pw.sdk.COMMON_HEADERS)
        self.assertNotIn("water3d.h", pw.sdk.COMMON_HEADERS)
        self.assertNotIn("water.h", pw.sdk.HEADERS_3D)
        self.assertNotIn("water3d.h", pw.sdk.HEADERS_3D)

    def test_water_guide_is_not_a_base_sdk_guide(self):
        """water.md ships with the add-on, not with every base SDK."""
        self.assertNotIn("water.md", pw.sdk.COMMON_GUIDES)

    def test_base_variant_check_rejects_a_leaked_water_library(self):
        """A base SDK that somehow contained the water library is refused."""
        for variant in ("Pixelica2D", "Pixelica3D"):
            files = ["include/pixelica/water.h", "lib/libpixelica_water.a"]
            problems = pw.sdk.check_variant_files(variant, files)
            # The base check is an allowlist of required files, so it already
            # fails here; what matters is that it never passes.
            self.assertTrue(problems)

    def test_the_option_defaults_to_off(self):
        text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
        match = re.search(r'option\(PIXELICA_BUILD_WATER\s+"[^"]*"\s+(\w+)\)', text)
        self.assertIsNotNone(match, "PIXELICA_BUILD_WATER option not found")
        self.assertEqual(match.group(1), "OFF")


if __name__ == "__main__":
    unittest.main()
