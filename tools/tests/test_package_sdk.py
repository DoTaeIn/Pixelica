"""Unit tests for tools/package_sdk.py.

They use synthetic trees in temporary directories: no CMake, compiler, network
or build is needed. A full packaging run (real build) is opt-in:

    PIXELICA_PACKAGE_SDK_E2E=1 python -m unittest discover -s tools/tests

Run from the repository root; the script is found through PIXELICA_TOOLS_DIR or, if
unset, relative to this file.
"""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

TOOLS_DIR = pathlib.Path(os.environ.get("PIXELICA_TOOLS_DIR") or pathlib.Path(__file__).resolve().parent.parent)


def load_module():
    spec = importlib.util.spec_from_file_location("package_sdk", str(TOOLS_DIR / "package_sdk.py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules["package_sdk"] = module  # dataclasses look the module up by name
    spec.loader.exec_module(module)
    return module


ps = load_module()


class DocumentLayoutTests(unittest.TestCase):
    def test_documents_preserve_relative_links(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = pathlib.Path(tmp) / "source"
            stage = pathlib.Path(tmp) / "stage"
            (source / "docs" / "verification").mkdir(parents=True)
            (source / "README.md").write_text("[Guide](docs/2d.md)", encoding="utf-8")
            (source / "docs" / "2d.md").write_text("![Preview](verification/preview.png)", encoding="utf-8")
            (source / "docs" / "verification" / "preview.png").write_bytes(b"image")
            variant = ps.Variant("Pixelica2D", (), ("README.md", "docs/2d.md", "docs/verification/preview.png"))
            self.assertEqual(ps.copy_documents(source, stage, variant), [])
            base = stage / "share" / "doc" / "Pixelica2D"
            self.assertTrue((base / "docs" / "2d.md").is_file())
            self.assertEqual((base / "docs" / "verification" / "preview.png").read_bytes(), b"image")
            self.assertFalse((base / "2d.md").exists())

# --- synthetic SDK file lists ------------------------------------------------
# The reference guides every SDK profile must ship under share/Pixelica/docs/.
# Written out here on purpose: CMakeLists.txt installs them and package_sdk.py
# requires them, and this third copy is what notices when one of those two is
# changed without the other.
EXPECTED_GUIDES = (
    "save.md", "storage.md", "lifecycle.md", "screens.md", "i18n.md",
    "texture_options.md", "camera.md", "debug.md", "chunks.md",
    "material_layers.md", "framebuffer.md", "render_effects.md",
    "lighting.md", "blending.md", "anim_clips.md", "pixel_light.md",
)

COMMON_FILES = (
    ["include/pixelica/" + h for h in ps.COMMON_HEADERS]
    # Spelled out rather than taken from ps.COMMON_GUIDES, so this is an
    # independent expectation of what an SDK must contain; the test below
    # compares the two lists and fails if either side drifts.
    + ["share/Pixelica/docs/" + name for name in EXPECTED_GUIDES]
    + ["share/Pixelica/tools/pixelica_font.py"]
    + ["include/pixelica/ui.h", "lib/libpixelica_ui.a", "lib/libpixelica_uid.a",
       "lib/cmake/PixelicaUI/PixelicaUIConfig.cmake",
       "lib/cmake/PixelicaUI/PixelicaUIConfigVersion.cmake",
       "lib/cmake/PixelicaUI/PixelicaUITargets.cmake",
       "lib/cmake/PixelicaUI/PixelicaUITargets-debug.cmake",
       "lib/cmake/PixelicaUI/PixelicaUITargets-release.cmake",
       "share/Pixelica/docs/ui.md", "share/Pixelica/examples/hello_ui/CMakeLists.txt",
       "share/Pixelica/examples/hello_ui/main.c",
       "lib/libpixelica_common.a", "lib/libpixelica_commond.a",
       "lib/cmake/Pixelica/PixelicaConfig.cmake",
       "lib/cmake/Pixelica/PixelicaConfigVersion.cmake",
       "lib/cmake/Pixelica/PixelicaPackage.cmake",
       "lib/cmake/Pixelica/PixelicaCommonTargets.cmake",
       "lib/cmake/Pixelica/PixelicaCommonTargets-debug.cmake",
       "lib/cmake/Pixelica/PixelicaCommonTargets-release.cmake"]
)
FILES_2D = COMMON_FILES + [
    "include/pixelica/renderer2d.h", "lib/libpixelica_renderer2d.a", "lib/libpixelica_renderer2dd.a",
    "lib/cmake/Pixelica/PixelicaRenderer2DTargets.cmake",
    "lib/cmake/Pixelica/PixelicaRenderer2DTargets-debug.cmake",
    "lib/cmake/Pixelica/PixelicaRenderer2DTargets-release.cmake",
    "lib/cmake/Pixelica2D/Pixelica2DConfig.cmake",
    "lib/cmake/Pixelica2D/Pixelica2DConfigVersion.cmake",
    "share/Pixelica/template2d/CMakeLists.txt",
]
FILES_3D = COMMON_FILES + [
    "include/pixelica/" + h for h in ps.HEADERS_3D + ("hybrid.h",)
] + [
    "lib/libpixelica_core.a", "lib/libpixelica_cored.a", "lib/libpixelica_hybrid.a", "lib/libpixelica_hybridd.a",
    "lib/cmake/Pixelica/PixelicaTargets.cmake",
    "lib/cmake/Pixelica/PixelicaTargets-debug.cmake",
    "lib/cmake/Pixelica/PixelicaTargets-release.cmake",
    "lib/cmake/Pixelica/PixelicaHybridTargets.cmake",
    "lib/cmake/Pixelica/PixelicaHybridTargets-debug.cmake",
    "lib/cmake/Pixelica/PixelicaHybridTargets-release.cmake",
    "share/Pixelica/template/CMakeLists.txt",
    "share/Pixelica/tools/pixelica_pack/pixelica_pack.py",
]


def fake_source(root):
    """A minimal 'Pixelica source tree' accepted by parse_args()."""
    (root / "cmake").mkdir(parents=True)
    (root / "CMakeLists.txt").write_text("project(Pixelica VERSION 0.1.0 LANGUAGES C)\n")
    (root / "cmake" / "PixelicaSources.cmake").write_text("# stub\n")
    return root


def parse(*argv):
    """parse_args() with stderr captured; returns (namespace, stderr text)."""
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        namespace = ps.parse_args(list(argv))
    return namespace, err.getvalue()


def parse_fails(testcase, *argv):
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        with testcase.assertRaises(SystemExit) as caught:
            ps.parse_args(list(argv))
    testcase.assertEqual(caught.exception.code, 2)
    return err.getvalue()


class ArgumentTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.source = fake_source(self.tmp / "src")
        self.base = ["--source-dir", str(self.source), "--build-dir", str(self.tmp / "build")]

    def test_defaults_come_from_source_dir(self):
        args, _ = parse("--source-dir", str(self.source))
        self.assertEqual(args.build_dir, self.source / "build-sdk")
        self.assertEqual(args.output_dir, self.source / "build-sdk" / "archives")
        self.assertFalse(args.dry_run)

    def test_generator_must_be_a_plain_name(self):
        for bad in ("-DEVIL", "Ninja; rm -rf /", "Ninja && echo", "$(whoami)", "a\nb", "Ninja\n", "",
                    "x" * 80):
            with self.subTest(generator=bad):
                parse_fails(self, *self.base, "--generator", bad)
        for good in ("Ninja", "Ninja Multi-Config", "Unix Makefiles", "Visual Studio 17 2022", "MinGW Makefiles"):
            with self.subTest(generator=good):
                args, _ = parse(*self.base, "--generator", good)
                self.assertEqual(args.generator, good)

    def test_platform_and_toolset_are_validated(self):
        parse_fails(self, *self.base, "--generator", "Visual Studio 17 2022", "--platform", "x64; calc")
        parse_fails(self, *self.base, "--generator", "Visual Studio 17 2022", "--platform", "-x64")
        parse_fails(self, *self.base, "--generator", "Visual Studio 17 2022", "--toolset", "v143 && dir")
        args, _ = parse(*self.base, "--generator", "Visual Studio 17 2022", "--platform", "x64", "--toolset", "v143")
        self.assertEqual((args.platform, args.toolset), ("x64", "v143"))

    def test_platform_needs_a_platform_generator(self):
        text = parse_fails(self, *self.base, "--generator", "Ninja", "--platform", "x64")
        self.assertIn("--platform", text)

    def test_executables_must_exist(self):
        missing = str(self.tmp / "no-such-cmake")
        parse_fails(self, *self.base, "--cmake", missing)
        parse_fails(self, *self.base, "--c-compiler", missing)
        parse_fails(self, *self.base, "--make-program", "definitely-not-a-program-0123456789")
        args, _ = parse(*self.base, "--cmake", sys.executable)  # any existing file
        self.assertEqual(args.cmake, str(pathlib.Path(sys.executable).resolve()))

    def test_jobs_range(self):
        for bad in ("0", "-1", "257", "x", "1.5", ""):
            with self.subTest(jobs=bad):
                parse_fails(self, *self.base, "--jobs", bad)
        args, _ = parse(*self.base, "--jobs", "8")
        self.assertEqual(args.jobs, 8)

    def test_run_id_is_a_plain_name(self):
        for bad in ("../x", "a/b", "-x", "a b", ""):
            with self.subTest(run_id=bad):
                parse_fails(self, *self.base, "--run-id", bad)
        args, _ = parse(*self.base, "--run-id", "try-1.2")
        self.assertEqual(args.run_id, "try-1.2")

    def test_directories_are_checked(self):
        root = pathlib.Path(self.tmp.anchor or "/")
        parse_fails(self, "--source-dir", str(self.source), "--build-dir", str(root))
        parse_fails(self, "--source-dir", str(self.source), "--build-dir", str(self.source))
        parse_fails(self, "--source-dir", str(self.source), "--build-dir", str(self.tmp))  # contains the source
        parse_fails(self, "--source-dir", str(self.source), "--build-dir", str(self.source / "src"))
        parse_fails(self, *self.base, "--output-dir", str(self.source))
        parse_fails(self, "--source-dir", str(self.tmp / "not-a-source-tree"))
        a_file = self.tmp / "file"
        a_file.write_text("x")
        parse_fails(self, "--source-dir", str(self.source), "--build-dir", str(a_file))
        parse_fails(self, *self.base, "--output-dir", str(a_file))

    def test_unknown_option_and_stray_value(self):
        parse_fails(self, *self.base, "--bogus")
        parse_fails(self, *self.base, "stray-positional")


class FreshDirectoryTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)

    def test_new_and_empty_directories_are_accepted(self):
        fresh = self.tmp / "a" / "b" / "fresh"
        ps.ensure_fresh_dir(fresh)
        self.assertTrue(fresh.is_dir())
        ps.ensure_fresh_dir(fresh)  # exists, still empty

    def test_populated_directory_is_refused_and_left_alone(self):
        stage = self.tmp / "stage"
        stage.mkdir()
        (stage / "keep.txt").write_text("precious")
        (stage / "sub").mkdir()
        (stage / "sub" / "deep.txt").write_text("also precious")
        with self.assertRaises(ps.PackagingError) as caught:
            ps.ensure_fresh_dir(stage)
        self.assertIn("not empty", str(caught.exception))
        self.assertEqual((stage / "keep.txt").read_text(), "precious")
        self.assertEqual((stage / "sub" / "deep.txt").read_text(), "also precious")

    def test_a_file_is_refused(self):
        target = self.tmp / "file"
        target.write_text("x")
        with self.assertRaises(ps.PackagingError):
            ps.ensure_fresh_dir(target)

    def test_script_never_deletes_recursively(self):
        text = (TOOLS_DIR / "package_sdk.py").read_text(encoding="utf-8")
        for forbidden in ("rmtree", "os.remove(", "os.removedirs", "shell=True", "os.system", "Popen"):
            self.assertNotIn(forbidden, text, forbidden)

    def test_build_tree_reuse_rules(self):
        source = fake_source(self.tmp / "src")
        tree = self.tmp / "tree"
        ps.check_build_tree(tree, source)  # does not exist yet
        tree.mkdir()
        ps.check_build_tree(tree, source)  # empty
        (tree / "junk").write_text("x")
        with self.assertRaises(ps.PackagingError):
            ps.check_build_tree(tree, source)  # populated, not a CMake build
        (tree / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=%s\n" % source)
        ps.check_build_tree(tree, source)  # configured from this source
        (tree / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=%s\n" % (self.tmp / "elsewhere"))
        with self.assertRaises(ps.PackagingError):
            ps.check_build_tree(tree, source)  # another project
        self.assertTrue((tree / "junk").exists())


class PolicyTests(unittest.TestCase):
    def test_valid_sdks_pass(self):
        self.assertEqual(ps.check_variant_files("Pixelica2D", FILES_2D), [])
        self.assertEqual(ps.check_variant_files("Pixelica3D", FILES_3D), [])

    def test_2d_sdk_rejects_every_3d_artifact(self):
        for extra in ["include/pixelica/pixelica.h", "include/pixelica/renderer.h", "include/pixelica/assets.h",
                      "include/pixelica/scene.h", "include/pixelica/hybrid.h", "lib/libpixelica_core.a",
                      "lib/pixelica_cored.lib", "lib/libpixelica_hybrid.a",
                      "lib/cmake/Pixelica/PixelicaTargets.cmake",
                      "lib/cmake/Pixelica/PixelicaTargets-release.cmake",
                      "lib/cmake/Pixelica/PixelicaHybridTargets.cmake",
                      "share/Pixelica/template/CMakeLists.txt",
                      "share/Pixelica/tools/pixelica_pack/pixelica_pack.py"]:
            with self.subTest(extra=extra):
                self.assertTrue(ps.check_variant_files("Pixelica2D", FILES_2D + [extra]))

    def test_3d_sdk_rejects_2d_artifacts(self):
        for extra in ["include/pixelica/renderer2d.h", "lib/libpixelica_renderer2d.a",
                      "lib/cmake/Pixelica/PixelicaRenderer2DTargets.cmake",
                      "lib/cmake/Pixelica2D/Pixelica2DConfig.cmake",
                      "share/Pixelica/template2d/CMakeLists.txt"]:
            with self.subTest(extra=extra):
                self.assertTrue(ps.check_variant_files("Pixelica3D", FILES_3D + [extra]))

    def test_missing_required_files_are_reported(self):
        for removed in ["include/pixelica/math.h", "include/pixelica/image.h",
                        "lib/cmake/Pixelica/PixelicaPackage.cmake",
                        "lib/cmake/Pixelica/PixelicaCommonTargets-debug.cmake",
                        "lib/cmake/Pixelica2D/Pixelica2DConfig.cmake",
                        "lib/cmake/Pixelica/PixelicaRenderer2DTargets-release.cmake"]:
            files = [f for f in FILES_2D if f != removed]
            with self.subTest(removed=removed):
                self.assertTrue(ps.check_variant_files("Pixelica2D", files), removed)
        # Both configurations of a library ship as separate files; dropping only
        # one of them is not detected by the library check, dropping both is.
        no_common_libs = [f for f in FILES_2D if "libpixelica_common" not in f]
        self.assertTrue(ps.check_variant_files("Pixelica2D", no_common_libs))
        self.assertTrue(ps.check_variant_files("Pixelica3D", [f for f in FILES_3D if "hybrid" not in f]))
        self.assertTrue(ps.check_variant_files("Pixelica3D", [f for f in FILES_3D if "pixelica_core" not in f]))

    def test_common_guides_are_required_in_both_sdks(self):
        # The inventory the packager enforces is the one this test expects.
        self.assertEqual(sorted(ps.COMMON_GUIDES), sorted(EXPECTED_GUIDES))
        self.assertEqual(len(set(ps.COMMON_GUIDES)), len(ps.COMMON_GUIDES))
        # Dropping any single guide must be reported, for either SDK.
        for variant, files in (("Pixelica2D", FILES_2D), ("Pixelica3D", FILES_3D)):
            self.assertEqual(ps.check_variant_files(variant, files), [])
            for guide in EXPECTED_GUIDES:
                removed = "share/Pixelica/docs/" + guide
                self.assertIn(removed, files)
                with self.subTest(variant=variant, missing=guide):
                    problems = ps.check_variant_files(
                        variant, [p for p in files if p != removed])
                    self.assertTrue(problems)
                    self.assertTrue(any(guide in p for p in problems), problems)

    def test_library_dirs_other_than_lib_are_understood(self):
        files = [f.replace("lib/", "lib64/", 1) if f.startswith("lib/") else f for f in FILES_2D]
        self.assertEqual(ps.check_variant_files("Pixelica2D", files), [])
        files.append("lib64/libpixelica_core.a")
        self.assertTrue(ps.check_variant_files("Pixelica2D", files))

    def test_unknown_variant(self):
        self.assertTrue(ps.check_variant_files("Pixelica4D", FILES_2D))

    def test_ui_is_required_in_both_base_sdks(self):
        for variant, files in (("Pixelica2D", FILES_2D), ("Pixelica3D", FILES_3D)):
            for removed in ("include/pixelica/ui.h", "lib/cmake/PixelicaUI/PixelicaUIConfig.cmake",
                            "share/Pixelica/examples/hello_ui/main.c"):
                with self.subTest(variant=variant, missing=removed):
                    self.assertTrue(ps.check_variant_files(variant, [p for p in files if p != removed]))
            self.assertTrue(ps.check_variant_files(variant, [p for p in files if "libpixelica_ui" not in p]))


def make_stage(root, files, content=lambda rel: ("data of %s\n" % rel).encode()):
    for rel in files:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content(rel))
    return root


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.variant = ps.VARIANTS[0]  # Pixelica2D

    def test_members_are_rooted_sorted_and_safe(self):
        stage = make_stage(self.tmp / "stage", ["b/two.txt", "a/one.txt", "include/pixelica/math.h"])
        out = self.tmp / "out.zip"
        names = ps.write_zip(stage, "Pixelica-0.1.0", out)
        with zipfile.ZipFile(str(out)) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(archive.namelist(), names)
            self.assertEqual(names, sorted(names))
            self.assertEqual(names, ["Pixelica-0.1.0/a/one.txt", "Pixelica-0.1.0/b/two.txt",
                                     "Pixelica-0.1.0/include/pixelica/math.h"])
            for name in names:
                self.assertFalse(name.startswith("/") or ".." in name.split("/") or "\\" in name)
            self.assertEqual(archive.read("Pixelica-0.1.0/a/one.txt"), b"data of a/one.txt\n")
        self.assertFalse(pathlib.Path(str(out) + ".partial").exists())

    def test_archive_is_reproducible_and_never_overwritten(self):
        stage = make_stage(self.tmp / "stage", ["x/y.txt", "z.txt"])
        first = self.tmp / "first.zip"
        second = self.tmp / "second.zip"
        ps.write_zip(stage, "Pixelica-0.1.0", first)
        ps.write_zip(stage, "Pixelica-0.1.0", second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        before = first.read_bytes()
        with self.assertRaises(ps.PackagingError):
            ps.write_zip(stage, "Pixelica-0.1.0", first)
        self.assertEqual(first.read_bytes(), before)

    def test_unsafe_names_are_rejected(self):
        for rel in ("../evil", "/abs", "a/../b", "a//b", "C:/x", "a\\b"):
            with self.subTest(rel=rel):
                with self.assertRaises(ps.PackagingError):
                    ps.safe_arcname("Pixelica-0.1.0", rel)

    @unittest.skipUnless(hasattr(os, "symlink"), "needs symlinks")
    def test_symbolic_links_are_refused(self):
        stage = make_stage(self.tmp / "stage", ["real.txt"])
        try:
            os.symlink(str(stage / "real.txt"), str(stage / "link.txt"))
        except (OSError, NotImplementedError):
            self.skipTest("cannot create symlinks here")
        with self.assertRaises(ps.PackagingError):
            ps.list_files(stage)
        self.assertFalse((self.tmp / "o.zip").exists())

    def test_manifest_lists_every_file_with_matching_hash(self):
        files = ["include/pixelica/math.h", "lib/libpixelica_common.a", "share/doc/Pixelica2D/README.md"]
        stage = make_stage(self.tmp / "stage", files)
        manifest = ps.build_manifest(self.variant, "0.1.0", stage, files, "Ninja", ["docs/2d.md"])
        self.assertEqual(manifest["name"], "Pixelica2D")
        self.assertEqual(manifest["version"], "0.1.0")
        self.assertEqual(manifest["archive_root"], "Pixelica-0.1.0")
        self.assertEqual(manifest["install_components"], ["common", "renderer2d"])
        self.assertIn("find_package(PixelicaUI CONFIG)", manifest["package_configs"])
        self.assertEqual(manifest["missing_documents"], ["docs/2d.md"])
        self.assertEqual([e["path"] for e in manifest["files"]], sorted(files))
        for entry in manifest["files"]:
            data = (stage / entry["path"]).read_bytes()
            self.assertEqual(entry["size"], len(data))
            self.assertEqual(entry["sha256"], hashlib.sha256(data).hexdigest())
        self.assertNotIn("time", json.dumps(manifest).lower())  # deterministic

    def test_embedded_manifest_matches_the_archive(self):
        files = ["include/pixelica/math.h", "lib/libpixelica_common.a"]
        stage = make_stage(self.tmp / "stage", files)
        manifest = ps.build_manifest(self.variant, "0.1.0", stage, files, None, [])
        rel = ps.write_manifest(stage, self.variant, manifest)
        self.assertEqual(rel, "share/doc/Pixelica2D/MANIFEST.json")
        out = self.tmp / "sdk.zip"
        ps.write_zip(stage, "Pixelica-0.1.0", out)
        with zipfile.ZipFile(str(out)) as archive:
            embedded = json.loads(archive.read("Pixelica-0.1.0/" + rel).decode("utf-8"))
            members = set(archive.namelist())
        self.assertEqual(embedded, manifest)
        listed = {"Pixelica-0.1.0/" + e["path"] for e in embedded["files"]}
        self.assertEqual(listed | {"Pixelica-0.1.0/" + rel}, members)  # everything but itself is listed
        with self.assertRaises(FileExistsError):
            ps.write_manifest(stage, self.variant, manifest)  # never overwrites

    def test_sha256sums_file(self):
        one = self.tmp / "Pixelica2D-0.1.0-x.zip"
        two = self.tmp / "Pixelica3D-0.1.0-x.zip"
        one.write_bytes(b"one")
        two.write_bytes(b"two")
        sums = ps.write_sha256sums(self.tmp, [two, one])
        lines = sums.read_text().splitlines()
        self.assertEqual(lines, ["%s  %s" % (hashlib.sha256(b"one").hexdigest(), one.name),
                                 "%s  %s" % (hashlib.sha256(b"two").hexdigest(), two.name)])
        with self.assertRaises(FileExistsError):
            ps.write_sha256sums(self.tmp, [one])

    def test_merging_two_sdks_detects_conflicts(self):
        a = make_stage(self.tmp / "a", ["include/pixelica/math.h", "only_a.txt"])
        b = make_stage(self.tmp / "b", ["include/pixelica/math.h", "only_b.txt"])
        self.assertEqual(ps.check_shared_files_identical(a, b), [])
        (b / "include/pixelica/math.h").write_bytes(b"different")
        problems = ps.check_shared_files_identical(a, b)
        self.assertEqual(len(problems), 1)
        self.assertIn("include/pixelica/math.h", problems[0])

    def test_source_date_epoch(self):
        saved = os.environ.get("SOURCE_DATE_EPOCH")
        self.addCleanup(lambda: os.environ.pop("SOURCE_DATE_EPOCH", None) if saved is None
                        else os.environ.__setitem__("SOURCE_DATE_EPOCH", saved))
        os.environ["SOURCE_DATE_EPOCH"] = "1700000000"
        self.assertEqual(ps.zip_timestamp()[0], 2023)
        os.environ["SOURCE_DATE_EPOCH"] = "0"
        self.assertEqual(ps.zip_timestamp()[0], 1980)  # clamped to the ZIP minimum
        os.environ["SOURCE_DATE_EPOCH"] = "soon"
        with self.assertRaises(ps.PackagingError):
            ps.zip_timestamp()


class PlanTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.source = fake_source(self.tmp / "src")
        self.build = self.tmp / "build"

    def args(self, *extra):
        namespace, _ = parse("--source-dir", str(self.source), "--build-dir", str(self.build), *extra)
        return namespace

    def test_multi_config_generator_uses_one_tree_and_both_configs(self):
        args = self.args("--generator", "Ninja Multi-Config")
        stage = self.tmp / "stage"
        steps = ps.plan_steps(args, "CMAKE", stage)
        configure = [s for s in steps if s.description.startswith("configure")]
        builds = [s for s in steps if s.description.startswith("build")]
        self.assertEqual(len(configure), 1)
        self.assertEqual([s.argv[s.argv.index("--config") + 1] for s in builds], ["Debug", "Release"])
        self.assertNotIn("-DCMAKE_BUILD_TYPE=Debug", configure[0].argv)

    def test_single_config_generator_uses_one_tree_per_configuration(self):
        compiler = sys.executable
        args = self.args("--generator", "Ninja", "--c-compiler", compiler, "--make-program", compiler)
        steps = ps.plan_steps(args, "CMAKE", self.tmp / "stage")
        configure = [s for s in steps if s.description.startswith("configure")]
        self.assertEqual(len(configure), 2)
        types = sorted(a for s in configure for a in s.argv if a.startswith("-DCMAKE_BUILD_TYPE="))
        self.assertEqual(types, ["-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_BUILD_TYPE=Release"])
        resolved = str(pathlib.Path(compiler).resolve())
        for step in configure:
            self.assertIn("-DCMAKE_C_COMPILER=" + resolved, step.argv)
            self.assertIn("-DCMAKE_MAKE_PROGRAM=" + resolved, step.argv)

    def test_build_profile_and_component_split(self):
        args = self.args("--generator", "Ninja Multi-Config", "--jobs", "3")
        stage = self.tmp / "stage"
        steps = ps.plan_steps(args, "CMAKE", stage)
        configure = [s for s in steps if s.description.startswith("configure")][0]
        for option in ("-DPIXELICA_PLATFORM=HEADLESS", "-DPIXELICA_BUILD_3D=ON", "-DPIXELICA_BUILD_2D=ON",
                       "-DPIXELICA_BUILD_EXAMPLES=OFF", "-DPIXELICA_BUILD_TESTS=OFF", "-DPIXELICA_BUILD_SKELETON=OFF",
                       "-DPIXELICA_BUILD_IK=OFF", "-DPIXELICA_BUILD_AUDIO=OFF", "-DPIXELICA_INSTALL=ON", "-DPIXELICA_WARNINGS_AS_ERRORS=ON"):
            self.assertIn(option, configure.argv)
        for step in steps:
            if step.description.startswith("build"):
                self.assertEqual(step.argv[-2:], ("--parallel", "3"))
        installs = [s for s in steps if s.description.startswith("install")]
        by_prefix = {}
        for step in installs:
            prefix = step.argv[step.argv.index("--prefix") + 1]
            component = step.argv[step.argv.index("--component") + 1]
            by_prefix.setdefault(pathlib.Path(prefix).name, set()).add(component)
        self.assertEqual(by_prefix, {"Pixelica2D": {"common", "renderer2d"}, "Pixelica3D": {"core", "hybrid"}})
        # every config of every component, into separate fresh prefixes
        self.assertEqual(len(installs), 2 * 2 * 2)

    def test_every_command_is_an_argument_list_of_strings(self):
        args = self.args("--generator", "Visual Studio 17 2022", "--platform", "x64", "--toolset", "v143")
        for step in ps.plan_steps(args, "CMAKE", self.tmp / "stage"):
            self.assertIsInstance(step.argv, tuple)
            self.assertTrue(all(isinstance(part, str) for part in step.argv))
        configure = ps.plan_steps(args, "CMAKE", self.tmp / "stage")[0].argv
        self.assertEqual(configure[configure.index("-G") + 1], "Visual Studio 17 2022")
        self.assertEqual(configure[configure.index("-A") + 1], "x64")
        self.assertEqual(configure[configure.index("-T") + 1], "v143")

    def test_dry_run_prints_the_plan_and_creates_nothing(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = ps.main(["--source-dir", str(self.source), "--build-dir", str(self.build),
                              "--output-dir", str(self.tmp / "dist"), "--cmake", sys.executable,
                              "--generator", "Ninja Multi-Config", "--run-id", "dry", "--dry-run"])
        text = out.getvalue()
        self.assertEqual(status, 0)
        self.assertIn("--component", text)
        self.assertIn("-DPIXELICA_BUILD_2D=ON", text)
        self.assertIn("SHA256SUMS.txt", text)
        self.assertFalse(self.build.exists())
        self.assertFalse((self.tmp / "dist").exists())

    def test_live_run_refuses_a_populated_stage_before_building(self):
        stage = self.build / "stage" / "taken"
        stage.mkdir(parents=True)
        (stage / "stale.txt").write_text("stale")
        err = io.StringIO()
        out = io.StringIO()
        executed = []
        # The CMake version probe is replaced (no CMake is needed) and every
        # external command is recorded instead of started.
        with mock.patch.object(ps, "check_cmake_version", lambda cmake: None), \
                mock.patch.object(ps, "run_step", lambda step, dry_run: executed.append(step)), \
                contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
            status = ps.main(["--source-dir", str(self.source), "--build-dir", str(self.build),
                              "--cmake", sys.executable, "--run-id", "taken"])
        self.assertEqual(status, 1)
        self.assertIn("not empty", err.getvalue())
        self.assertEqual(executed, [])  # refused before configuring or building anything
        self.assertEqual((stage / "stale.txt").read_text(), "stale")
        self.assertFalse((self.build / "tree").exists())

    def test_live_run_refuses_to_overwrite_an_archive(self):
        dist = self.tmp / "dist"
        dist.mkdir()
        version = ps.read_version(self.source)
        existing = dist / ("Pixelica2D-%s-%s.zip" % (version, ps.platform_tag()))
        existing.write_bytes(b"do not touch")
        err = io.StringIO()
        executed = []
        with mock.patch.object(ps, "check_cmake_version", lambda cmake: None), \
                mock.patch.object(ps, "run_step", lambda step, dry_run: executed.append(step)), \
                contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            status = ps.main(["--source-dir", str(self.source), "--build-dir", str(self.build),
                              "--output-dir", str(dist), "--cmake", sys.executable, "--run-id", "fresh"])
        self.assertEqual(status, 1)
        self.assertIn("overwrite", err.getvalue())
        self.assertEqual(executed, [])
        self.assertEqual(existing.read_bytes(), b"do not touch")


@unittest.skipUnless(os.environ.get("PIXELICA_PACKAGE_SDK_E2E") == "1",
                     "set PIXELICA_PACKAGE_SDK_E2E=1 to run a real build + packaging (needs CMake and a C compiler)")
class EndToEndTests(unittest.TestCase):
    def test_real_run_produces_two_valid_archives(self):
        source = TOOLS_DIR.parent
        with tempfile.TemporaryDirectory() as tmp:
            build = pathlib.Path(tmp) / "build"
            dist = pathlib.Path(tmp) / "dist"
            status = ps.main(["--source-dir", str(source), "--build-dir", str(build),
                              "--output-dir", str(dist)])
            self.assertEqual(status, 0)
            archives = sorted(p for p in dist.iterdir() if p.suffix == ".zip")
            self.assertEqual([p.name.split("-")[0] for p in archives], ["Pixelica2D", "Pixelica3D"])
            members = {}
            for path in archives:
                with zipfile.ZipFile(str(path)) as archive:
                    self.assertIsNone(archive.testzip())
                    members[path.name.split("-")[0]] = [n.split("/", 1)[1] for n in archive.namelist()]
            self.assertEqual(ps.check_variant_files("Pixelica2D", members["Pixelica2D"]), [])
            self.assertEqual(ps.check_variant_files("Pixelica3D", members["Pixelica3D"]), [])
            self.assertTrue((dist / "SHA256SUMS.txt").is_file())


if __name__ == "__main__":
    unittest.main()
