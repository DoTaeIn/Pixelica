"""Add-on isolation: never ship or replace base SDK files."""
import importlib.util
import os
import pathlib
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(os.environ.get("PIXELICA_TOOLS_DIR") or pathlib.Path(__file__).resolve().parent.parent)
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("package_shaders", TOOLS / "package_shaders.py")
ps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ps)


class ShaderPackageTests(unittest.TestCase):
    def test_source_allowlist(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, stage = pathlib.Path(tmp) / "repo", pathlib.Path(tmp) / "stage"
            for name in list(ps.SOURCE_FILES.values()) + ["modules/shaders/private.c", ".git/config"]:
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name)
            ps.stage_source(source, stage)
            self.assertEqual(set(ps.sdk.list_files(stage)), set(ps.SOURCE_FILES) | {"README.md"})

    def test_no_shared_loader_or_base_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = pathlib.Path(tmp)
            paths = ["include/pixelica/shaders.h", "lib/pixelica_shaders.lib", "lib/pixelica_shadersd.lib"]
            paths += ["lib/cmake/PixelicaShaders/PixelicaShaders%s.cmake" % suffix for suffix in
                      ("Config", "ConfigVersion", "Targets", "Targets-debug", "Targets-release")]
            for name in paths:
                path = stage / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("test")
            self.assertEqual(set(ps.check_binary(stage)), set(paths))
            for name in ("lib/pixelica_common.lib", "include/pixelica/pixelica.h",
                         "lib/cmake/Pixelica/PixelicaPackage.cmake"):
                path = stage / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("unrelated")
                with self.assertRaises(ps.sdk.PackagingError):
                    ps.check_binary(stage)
                path.unlink()

    def test_base_sdk_keeps_shaders_off(self):
        self.assertIn("-DPIXELICA_BUILD_SHADERS=OFF", ps.sdk.configure_options("Release"))


if __name__ == "__main__":
    unittest.main()
