"""Particle add-on isolation and source archive tests; no compiler needed."""
import importlib.util
import os
import pathlib
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(os.environ.get("PIXELICA_TOOLS_DIR") or pathlib.Path(__file__).resolve().parent.parent)
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("package_particles", TOOLS / "package_particles.py")
pp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pp)


class ParticlePackageTests(unittest.TestCase):
    def test_source_bundle_uses_allowlist(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, stage = pathlib.Path(tmp) / "repo", pathlib.Path(tmp) / "stage"
            paths = ("sdk/particles-addon/CMakeLists.txt", "cmake/PixelicaPackage.cmake",
                     "docs/particles.md", "examples/hello_particles/main.c",
                     "modules/particles/CMakeLists.txt", "modules/particles/particles.c",
                     "modules/particles/include/pixelica/particles.h",
                     "modules/particles/private.log", "modules/audio/audio.c", ".git/config")
            for name in paths:
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name, encoding="utf-8")
            pp.stage_source(source, stage)
            files = pp.sdk.list_files(stage)
            self.assertIn("modules/particles/particles.c", files)
            self.assertEqual((stage / "CMakeLists.txt").read_text(), "sdk/particles-addon/CMakeLists.txt")
            self.assertFalse(any("audio" in p or ".git" in p or p.endswith(".log") for p in files))

    def test_addon_rejects_base_library_and_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = pathlib.Path(tmp)
            files = ["lib/cmake/Pixelica/PixelicaPackage.cmake"]
            for component, export in zip(pp.COMPONENTS, ("Particles", "Particles2D", "Particles25D")):
                files += ["include/pixelica/%s.h" % component, "lib/libpixelica_%s.a" % component]
                files += ["lib/cmake/Pixelica/Pixelica%sTargets-%s.cmake" % (export, config)
                          for config in ("debug", "release")]
            for name in files:
                path = stage / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("test")
            self.assertEqual(pp.check_binary(stage), sorted(files))
            for unrelated in ("lib/libpixelica_common.a", "include/pixelica/renderer.h"):
                path = stage / unrelated
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("unrelated")
                with self.assertRaises(pp.sdk.PackagingError):
                    pp.check_binary(stage)
                path.unlink()

    def test_base_packaging_explicitly_disables_particles(self):
        self.assertIn("-DPIXELICA_BUILD_PARTICLES=OFF", pp.sdk.configure_options("Release"))


if __name__ == "__main__":
    unittest.main()
