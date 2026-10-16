#!/usr/bin/env python3
"""Build Pixelica once and package TWO separate SDK archives.

    Pixelica2D-<version>-<platform>.zip   install components: common + renderer2d
        2D renderer + the shared common library. No 3D renderer, context,
        scene, mesh/cart code, no 3D header, no pixelica_core archive, no hybrid.
    Pixelica3D-<version>-<platform>.zip   install components: core + hybrid
        3D renderer (the original Pixelica::core SDK, which includes the
        common library) plus the opt-in hybrid 2.5D module. Hybrid needs the
        2D SDK in the same prefix; the 3D core does not.

Both always include the UI library, headers, independent PixelicaUI CMake
package, guide and menu example. UI has no build switch or separate ZIP.
Both archives have the same top-level directory (Pixelica-<version>/), so
extracting both into one directory MERGES them into one prefix. Files that
exist in both (package configs, common headers and library) are byte-identical;
this is checked before the archives are written.

Build recipe (same source tree, one configuration of CMake per tree):
    PIXELICA_PLATFORM=HEADLESS, PIXELICA_BUILD_3D=ON, PIXELICA_BUILD_2D=ON, examples / tests /
    benchmarks OFF, optional skeleton / IK OFF, strict warnings (as errors),
    Debug and Release (one multi-config build tree, or one tree per
    configuration for single-config generators; DEBUG_POSTFIX 'd' lets both
    share one prefix).

Safety rules
    * Nothing is ever deleted. There is no recursive delete anywhere in this
      script. Staging directories are created fresh (<build-dir>/stage/<run-id>,
      a new run id every run) and the script refuses to use an existing,
      populated one. A build tree is only reused when it is empty or already
      configured from this very source directory. Existing archives are never
      overwritten.
    * Every external program is started from an argument list, never through a
      shell. Option values are validated before anything runs.
    * No network access, no downloads, no publishing.

Usage:
    python tools/package_sdk.py --build-dir build-sdk --output-dir dist
    python tools/package_sdk.py --cmake /opt/cmake/bin/cmake --generator Ninja \\
        --c-compiler /usr/bin/gcc --build-dir /tmp/pixelica-build --output-dir /tmp/pixelica-dist
    python tools/package_sdk.py --dry-run          # print the plan, run nothing
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime
import hashlib
import json
import os
import pathlib
import platform
import re
import secrets
import shlex
import shutil
import subprocess
import sys
import zipfile
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
DEFAULT_SOURCE_DIR = SCRIPT_DIR.parent

CONFIGS = ("Debug", "Release")
MIN_CMAKE = (3, 21)

GENERATOR_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 .+()_-]{0,63}$")
PLATFORM_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_]{0,31}$")
TOOLSET_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.,=+-]{0,63}$")
RUN_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")
VERSION_RE = re.compile(r"project\s*\(\s*Pixelica\s+VERSION\s+(\d+\.\d+\.\d+)", re.IGNORECASE)

COMMON_HEADERS = ("status.h", "input.h", "platform.h", "math.h", "animation.h", "anim_clip.h",
                  "image.h", "draw_options.h", "assets_common.h", "save.h", "storage.h", "app.h", "screens.h", "i18n.h", "camera.h", "debug.h", "chunks.h", "framebuffer.h", "light.h", "collision2d.h")
HEADERS_3D = ("pixelica.h", "renderer.h", "assets.h", "scene.h")

# Reference guides installed under share/Pixelica/docs/ by every SDK profile
# (the `common` and `core` install components). Keep in sync with the
# install(FILES docs/...) list in CMakeLists.txt; check_variant_files() below
# refuses to package an SDK that is missing any of them.
COMMON_GUIDES = ("save.md", "storage.md", "lifecycle.md", "screens.md", "i18n.md",
                 "texture_options.md", "camera.md", "debug.md", "chunks.md",
                 "material_layers.md", "framebuffer.md", "render_effects.md",
                 "lighting.md", "blending.md", "anim_clips.md", "pixel_light.md")

CMAKE_DIR_3D = "cmake/Pixelica"  # below the library directory (lib, lib64, ...)
CMAKE_DIR_2D = "cmake/Pixelica2D"
CMAKE_DIR_UI = "cmake/PixelicaUI"

EPOCH_FLOOR = 315532800  # 1980-01-01, the earliest ZIP timestamp


class PackagingError(Exception):
    """A user-facing failure; main() prints it and exits with status 1."""


# --------------------------------------------------------------------------
# Variants
# --------------------------------------------------------------------------
@dataclasses.dataclass(frozen=True)
class Variant:
    name: str                     # Pixelica2D / Pixelica3D
    components: Tuple[str, ...]   # cmake --install --component, in order
    docs: Tuple[str, ...]         # source-relative documents to copy (missing ones are reported)


VARIANTS = (
    Variant('Pixelica2D', ('common', 'renderer2d'), ('README.md', 'docs/sdk.md', 'docs/2d.md', 'docs/api.md', 'docs/architecture.md', 'docs/DEVELOPMENT.md', 'docs/asset_format.md', 'docs/animation.md', 'docs/rigging.md', 'docs/input_manager.md', 'docs/online.md', 'docs/audio.md', 'docs/particles.md', 'docs/shaders.md', 'docs/porting.md', 'docs/art_style.md', 'docs/ui.md')),
    Variant('Pixelica3D', ('core', 'hybrid'), ('README.md', 'docs/sdk.md', 'docs/2d.md', 'docs/api.md', 'docs/architecture.md', 'docs/DEVELOPMENT.md', 'docs/asset_format.md', 'docs/animation.md', 'docs/rigging.md', 'docs/input_manager.md', 'docs/online.md', 'docs/audio.md', 'docs/particles.md', 'docs/shaders.md', 'docs/porting.md', 'docs/art_style.md', 'docs/ui.md')),
)


# --------------------------------------------------------------------------
# Argument validation
# --------------------------------------------------------------------------
def validate_generator(value: str) -> str:
    if not GENERATOR_RE.fullmatch(value):
        raise argparse.ArgumentTypeError(
            "generator must be a plain CMake generator name such as 'Ninja' or "
            "'Visual Studio 17 2022' (letters, digits, space, . + ( ) _ -; no leading '-')")
    return value


def validate_platform(value: str) -> str:
    if not PLATFORM_RE.fullmatch(value):
        raise argparse.ArgumentTypeError(
            "platform must be a simple architecture name such as x64, Win32 or ARM64")
    return value


def validate_toolset(value: str) -> str:
    if not TOOLSET_RE.fullmatch(value):
        raise argparse.ArgumentTypeError("toolset must be a simple CMake toolset spec such as v143")
    return value


def validate_run_id(value: str) -> str:
    if not RUN_ID_RE.fullmatch(value):
        raise argparse.ArgumentTypeError(
            "run id must start with a letter or digit and contain only letters, digits, . _ -")
    return value


def validate_jobs(value: str) -> int:
    try:
        jobs = int(value, 10)
    except ValueError:
        raise argparse.ArgumentTypeError("jobs must be an integer in 1..256") from None
    if not 1 <= jobs <= 256:
        raise argparse.ArgumentTypeError("jobs must be an integer in 1..256")
    return jobs


def _reject_nul_and_empty(value: str, what: str) -> None:
    if value == "" or "\0" in value:
        raise argparse.ArgumentTypeError("%s must be a non-empty path" % what)


def validate_executable(value: str) -> str:
    """A path to an existing file, or a bare program name found on PATH."""
    _reject_nul_and_empty(value, "executable")
    if os.sep in value or (os.altsep and os.altsep in value) or value.startswith("."):
        candidate = pathlib.Path(value)
        if not candidate.is_file():
            raise argparse.ArgumentTypeError("executable '%s' does not exist" % value)
        return str(candidate.resolve())
    found = shutil.which(value)
    if found is None:
        raise argparse.ArgumentTypeError("'%s' was not found on PATH" % value)
    return str(pathlib.Path(found).resolve())


def validate_dir_path(value: str) -> pathlib.Path:
    """An absolute-ized directory path that is not a filesystem root."""
    _reject_nul_and_empty(value, "directory")
    path = pathlib.Path(value).expanduser()
    path = pathlib.Path(os.path.abspath(str(path)))
    if path.parent == path:
        raise argparse.ArgumentTypeError("'%s' is a filesystem root" % value)
    return path


def is_multi_config(generator: Optional[str]) -> bool:
    """Visual Studio, Xcode and every '... Multi-Config' generator."""
    if generator is None:
        return os.name == "nt"  # CMake picks Visual Studio by default on Windows
    return bool(re.search(r"Visual Studio|Xcode|Multi-Config", generator))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="package_sdk.py",
        description="Build Pixelica and write the separate Pixelica2D and Pixelica3D SDK ZIP archives "
                    "(plus a SHA256 manifest). See the module docstring for the full recipe.")
    parser.add_argument("--cmake", type=validate_executable, default=None,
                        help="CMake executable (path or name on PATH; default: cmake on PATH)")
    parser.add_argument("--generator", type=validate_generator, default=None,
                        help="CMake generator name (default: CMake's own default)")
    parser.add_argument("--platform", type=validate_platform, default=None,
                        help="CMake generator platform (-A), e.g. x64 for Visual Studio")
    parser.add_argument("--toolset", type=validate_toolset, default=None,
                        help="CMake generator toolset (-T), e.g. v143")
    parser.add_argument("--c-compiler", type=validate_executable, default=None,
                        help="C compiler for single-config generators (CMAKE_C_COMPILER)")
    parser.add_argument("--make-program", type=validate_executable, default=None,
                        help="build program for single-config generators (CMAKE_MAKE_PROGRAM)")
    parser.add_argument("--source-dir", type=validate_dir_path, default=DEFAULT_SOURCE_DIR,
                        help="Pixelica source tree (default: the repository this script is in)")
    parser.add_argument("--build-dir", type=validate_dir_path, default=None,
                        help="build + staging directory (default: <source>/build-sdk); never deleted")
    parser.add_argument("--output-dir", type=validate_dir_path, default=None,
                        help="where the archives are written (default: <build-dir>/archives)")
    parser.add_argument("--run-id", type=validate_run_id, default=None,
                        help="name of this run's fresh staging directory below <build-dir>/stage "
                             "(default: UTC time + random suffix)")
    parser.add_argument("--jobs", type=validate_jobs, default=None,
                        help="parallel build jobs (default: the build tool's default)")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate the arguments and print every command and file operation "
                             "without executing or creating anything")
    return parser


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    """Parses and cross-checks the command line (SystemExit(2) on errors)."""
    parser = build_parser()
    args = parser.parse_args(argv)

    source = args.source_dir
    if not (source / "CMakeLists.txt").is_file() or not (source / "cmake" / "PixelicaSources.cmake").is_file():
        parser.error("--source-dir '%s' is not a Pixelica source tree "
                     "(CMakeLists.txt and cmake/PixelicaSources.cmake expected)" % source)
    if args.build_dir is None:
        args.build_dir = source / "build-sdk"
    if args.output_dir is None:
        args.output_dir = args.build_dir / "archives"

    def inside_or_same(child: pathlib.Path, parent: pathlib.Path) -> bool:
        child_key = os.path.normcase(str(child))
        parent_key = os.path.normcase(str(parent))
        return child_key == parent_key or child_key.startswith(parent_key.rstrip(os.sep) + os.sep)

    if inside_or_same(source, args.build_dir):
        parser.error("--build-dir must not be the source directory or contain it")
    if inside_or_same(args.build_dir, source / "src") or inside_or_same(args.build_dir, source / "include"):
        parser.error("--build-dir must not be inside the source tree's src/ or include/")
    if inside_or_same(source, args.output_dir):
        parser.error("--output-dir must not be the source directory or contain it")
    if args.output_dir.exists() and not args.output_dir.is_dir():
        parser.error("--output-dir '%s' exists and is not a directory" % args.output_dir)
    if args.build_dir.exists() and not args.build_dir.is_dir():
        parser.error("--build-dir '%s' exists and is not a directory" % args.build_dir)
    if args.platform is not None and args.generator is not None and \
            not re.search(r"Visual Studio|Xcode|Green Hills", args.generator):
        parser.error("--platform is only meaningful for Visual Studio / Xcode generators")
    return args


# --------------------------------------------------------------------------
# Paths: fresh directories, never deleted
# --------------------------------------------------------------------------
def ensure_fresh_dir(path: pathlib.Path) -> None:
    """Creates <path> (and parents). An existing directory must be empty.

    Nothing is deleted: a populated directory is a hard error, so a stale or
    foreign tree can never end up inside an archive.
    """
    if path.exists():
        if not path.is_dir() or path.is_symlink():
            raise PackagingError("refusing to use '%s': it exists and is not a plain directory" % path)
        if any(path.iterdir()):
            raise PackagingError(
                "refusing to use '%s': the directory exists and is not empty. Nothing is "
                "deleted; pass a new --run-id / --build-dir or empty it yourself." % path)
        return
    path.mkdir(parents=True)


def _cache_home_directory(cache_file: pathlib.Path) -> Optional[str]:
    try:
        with cache_file.open("r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if line.startswith("CMAKE_HOME_DIRECTORY:"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        return None
    return None


def check_build_tree(tree: pathlib.Path, source: pathlib.Path) -> None:
    """A build tree may be new, empty, or already configured from <source>."""
    if not tree.exists():
        return
    if not tree.is_dir() or tree.is_symlink():
        raise PackagingError("refusing to use build tree '%s': not a plain directory" % tree)
    if not any(tree.iterdir()):
        return
    cache = tree / "CMakeCache.txt"
    home = _cache_home_directory(cache) if cache.is_file() else None
    if home is None or os.path.normcase(os.path.abspath(home)) != os.path.normcase(str(source)):
        raise PackagingError(
            "refusing to use build tree '%s': it is not empty and is not a CMake build of '%s'. "
            "Nothing is deleted; choose another --build-dir." % (tree, source))


def default_run_id() -> str:
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return "run-%s-%s" % (stamp, secrets.token_hex(4))


# --------------------------------------------------------------------------
# Plan: the exact external commands
# --------------------------------------------------------------------------
@dataclasses.dataclass(frozen=True)
class Step:
    description: str
    argv: Tuple[str, ...]


def read_version(source: pathlib.Path) -> str:
    text = (source / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    match = VERSION_RE.search(text)
    if match is None:
        raise PackagingError("could not read 'project(Pixelica VERSION x.y.z)' from %s/CMakeLists.txt" % source)
    return match.group(1)


def configure_options(single_config: Optional[str]) -> List[str]:
    """-D options of the packaging build (the build profile of both SDKs)."""
    options = [
        "-DPIXELICA_PLATFORM=HEADLESS",
        "-DPIXELICA_BUILD_3D=ON",
        "-DPIXELICA_BUILD_2D=ON",
        "-DPIXELICA_BUILD_EXAMPLES=OFF",
        "-DPIXELICA_BUILD_TESTS=OFF",
        "-DPIXELICA_BUILD_BENCHMARKS=OFF",
        "-DPIXELICA_BUILD_SKELETON=OFF",
        "-DPIXELICA_BUILD_IK=OFF",
        "-DPIXELICA_BUILD_INPUT_MANAGER=OFF",
        "-DPIXELICA_BUILD_ONLINE=OFF",
        "-DPIXELICA_BUILD_AUDIO=OFF",
        "-DPIXELICA_BUILD_PARTICLES=OFF",
        "-DPIXELICA_BUILD_SHADERS=OFF",
        "-DPIXELICA_BUILD_WATER=OFF",
        "-DPIXELICA_INSTALL=ON",
        "-DPIXELICA_SANITIZE=OFF",
        "-DPIXELICA_WARNINGS_AS_ERRORS=ON",
    ]
    if single_config is not None:
        options.append("-DCMAKE_BUILD_TYPE=%s" % single_config)
    return options


def build_trees(args: argparse.Namespace) -> List[Tuple[pathlib.Path, Tuple[str, ...]]]:
    """[(build tree, configurations built in it)]."""
    if is_multi_config(args.generator):
        return [(args.build_dir / "tree", CONFIGS)]
    return [(args.build_dir / ("tree-" + config.lower()), (config,)) for config in CONFIGS]


def plan_steps(args: argparse.Namespace, cmake: str, stage_root: pathlib.Path) -> List[Step]:
    """Every external command, in execution order (pure; nothing is executed)."""
    steps: List[Step] = []
    multi = is_multi_config(args.generator)
    source = str(args.source_dir)
    for tree, configs in build_trees(args):
        argv = [cmake, "-S", source, "-B", str(tree)]
        if args.generator is not None:
            argv += ["-G", args.generator]
        if args.platform is not None:
            argv += ["-A", args.platform]
        if args.toolset is not None:
            argv += ["-T", args.toolset]
        argv += configure_options(None if multi else configs[0])
        if not multi:
            if args.c_compiler is not None:
                argv.append("-DCMAKE_C_COMPILER=%s" % args.c_compiler)
            if args.make_program is not None:
                argv.append("-DCMAKE_MAKE_PROGRAM=%s" % args.make_program)
        steps.append(Step("configure %s" % tree.name, tuple(argv)))
        for config in configs:
            build = [cmake, "--build", str(tree), "--config", config]
            if args.jobs is not None:
                build += ["--parallel", str(args.jobs)]
            steps.append(Step("build %s (%s)" % (tree.name, config), tuple(build)))
    for variant in VARIANTS:
        prefix = str(stage_root / variant.name)
        for tree, configs in build_trees(args):
            for config in configs:
                for component in variant.components:
                    steps.append(Step(
                        "install %s: component %s (%s)" % (variant.name, component, config),
                        (cmake, "--install", str(tree), "--config", config,
                         "--component", component, "--prefix", prefix)))
    return steps


def format_command(argv: Sequence[str]) -> str:
    """For display only; commands are never run through a shell."""
    if os.name == "nt":
        return subprocess.list2cmdline(list(argv))
    return " ".join(shlex.quote(part) for part in argv)


def run_step(step: Step, dry_run: bool) -> None:
    print("==> %s\n    %s" % (step.description, format_command(step.argv)), flush=True)
    if dry_run:
        return
    try:
        completed = subprocess.run(list(step.argv), shell=False, check=False)
    except OSError as error:
        raise PackagingError("could not start %s: %s" % (step.argv[0], error)) from error
    if completed.returncode != 0:
        raise PackagingError("%s failed (exit status %d)" % (step.description, completed.returncode))


def check_cmake_version(cmake: str) -> None:
    try:
        completed = subprocess.run([cmake, "--version"], shell=False, check=False,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as error:
        raise PackagingError("could not run '%s --version': %s" % (cmake, error)) from error
    first = completed.stdout.decode("utf-8", "replace").splitlines()[:1]
    match = re.search(r"cmake version (\d+)\.(\d+)", first[0]) if first else None
    if completed.returncode != 0 or match is None:
        raise PackagingError("'%s --version' did not report a CMake version" % cmake)
    found = (int(match.group(1)), int(match.group(2)))
    if found < MIN_CMAKE:
        raise PackagingError("CMake %d.%d or newer is required (found %d.%d)" % (MIN_CMAKE + found))


# --------------------------------------------------------------------------
# Archive content policy
# --------------------------------------------------------------------------
def _is_library_file(path: str) -> bool:
    parts = path.split("/")
    return len(parts) >= 2 and parts[0].startswith("lib") and "cmake" not in parts[:-1]


def _lib_basenames(files: Iterable[str]) -> List[str]:
    return [f.rsplit("/", 1)[-1] for f in files if _is_library_file(f)]


def _config_suffix_files(files: Iterable[str], directory: str, name: str) -> List[str]:
    """Files named exactly <name> inside any '<libdir>/<directory>/'."""
    wanted = "/%s/%s" % (directory, name)
    return [f for f in files if f.endswith(wanted)]


def check_variant_files(variant: str, files: Iterable[str], configs: Sequence[str] = CONFIGS) -> List[str]:
    """Returns the policy violations of one SDK (empty list = acceptable).

    <files> are POSIX paths relative to the install prefix. The 2D SDK must hold
    no 3D header, 3D library, 3D export or hybrid module; the 3D SDK must hold
    no 2D renderer. Both must hold the common library, the package configs and
    the per-configuration export files of every library they ship.
    """
    problems: List[str] = []
    file_list = sorted(set(files))
    present = set(file_list)
    libs = _lib_basenames(file_list)

    def need(path: str) -> None:
        if path not in present:
            problems.append("missing %s" % path)

    def need_suffix(directory: str, name: str) -> None:
        if not _config_suffix_files(file_list, directory, name):
            problems.append("missing <libdir>/%s/%s" % (directory, name))

    def need_lib(stem: str) -> None:
        if not any(stem in base for base in libs):
            problems.append("missing a library file containing '%s' under lib*/" % stem)

    def forbid_path(path: str) -> None:
        if path in present:
            problems.append("must not contain %s" % path)

    def forbid_lib(stem: str) -> None:
        for base in libs:
            if stem in base:
                problems.append("must not contain library %s" % base)

    def forbid_name(prefix: str) -> None:
        for path in file_list:
            if path.rsplit("/", 1)[-1].startswith(prefix):
                problems.append("must not contain %s" % path)

    def forbid_dir(prefix: str) -> None:
        for path in file_list:
            if path.startswith(prefix):
                problems.append("must not contain %s" % path)

    exports_common = ["PixelicaCommonTargets.cmake"] + \
        ["PixelicaCommonTargets-%s.cmake" % c.lower() for c in configs]
    base_config = ["PixelicaConfig.cmake", "PixelicaConfigVersion.cmake", "PixelicaPackage.cmake"]

    for header in COMMON_HEADERS:
        need("include/pixelica/" + header)
    for name in base_config + exports_common:
        need_suffix(CMAKE_DIR_3D, name)
    need_lib("pixelica_common")
    # UI is a standard part of both SDKs, with independent package discovery.
    need("include/pixelica/ui.h")
    need_lib("pixelica_ui")
    for name in ["PixelicaUIConfig.cmake", "PixelicaUIConfigVersion.cmake",
                 "PixelicaUITargets.cmake"] + [
                     "PixelicaUITargets-%s.cmake" % c.lower() for c in configs]:
        need_suffix(CMAKE_DIR_UI, name)
    need("share/Pixelica/docs/ui.md")
    need("share/Pixelica/examples/hello_ui/CMakeLists.txt")
    need("share/Pixelica/examples/hello_ui/main.c")
    for guide in COMMON_GUIDES:
        need("share/Pixelica/docs/" + guide)
    need("share/Pixelica/tools/pixelica_font.py")

    if variant == "Pixelica2D":
        need("include/pixelica/renderer2d.h")
        for name in ["PixelicaRenderer2DTargets.cmake"] + \
                ["PixelicaRenderer2DTargets-%s.cmake" % c.lower() for c in configs]:
            need_suffix(CMAKE_DIR_3D, name)
        need_suffix(CMAKE_DIR_2D, "Pixelica2DConfig.cmake")
        need_suffix(CMAKE_DIR_2D, "Pixelica2DConfigVersion.cmake")
        need_lib("pixelica_renderer2d")
        for header in HEADERS_3D + ("hybrid.h",):
            forbid_path("include/pixelica/" + header)
        for stem in ("pixelica_core", "pixelica_hybrid", "renderer3d"):
            forbid_lib(stem)
        forbid_name("PixelicaTargets")
        forbid_name("PixelicaHybridTargets")
        forbid_dir("share/Pixelica/template/")
        forbid_dir("share/Pixelica/tools/pixelica_pack/")
    elif variant == "Pixelica3D":
        for header in HEADERS_3D + ("hybrid.h",):
            need("include/pixelica/" + header)
        for name in ["PixelicaTargets.cmake", "PixelicaHybridTargets.cmake"] + \
                ["PixelicaTargets-%s.cmake" % c.lower() for c in configs] + \
                ["PixelicaHybridTargets-%s.cmake" % c.lower() for c in configs]:
            need_suffix(CMAKE_DIR_3D, name)
        need_lib("pixelica_core")
        need_lib("pixelica_hybrid")
        forbid_path("include/pixelica/renderer2d.h")
        forbid_lib("pixelica_renderer2d")
        forbid_name("PixelicaRenderer2DTargets")
        forbid_name("Pixelica2DConfig")
        forbid_dir("share/Pixelica/template2d/")
    else:
        problems.append("unknown variant '%s'" % variant)
    return problems


def list_files(root: pathlib.Path) -> List[str]:
    """Sorted POSIX relative paths of every file below <root>. Symlinks and
    anything that is neither a file nor a directory are an error."""
    result: List[str] = []
    for current, dirs, names in os.walk(str(root), followlinks=False):
        for name in list(dirs) + list(names):
            full = os.path.join(current, name)
            if os.path.islink(full):
                raise PackagingError("refusing to package a symbolic link: %s" % full)
        for name in names:
            full = os.path.join(current, name)
            if not os.path.isfile(full):
                raise PackagingError("refusing to package a non-regular file: %s" % full)
            result.append(pathlib.PurePath(os.path.relpath(full, str(root))).as_posix())
    result.sort()
    return result


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_shared_files_identical(stage_a: pathlib.Path, stage_b: pathlib.Path) -> List[str]:
    """Every relative path present in both stages must be byte-identical, so
    that extracting both archives into one prefix cannot conflict."""
    problems: List[str] = []
    files_a = set(list_files(stage_a))
    for rel in sorted(files_a.intersection(list_files(stage_b))):
        if sha256_file(stage_a / rel) != sha256_file(stage_b / rel):
            problems.append("%s differs between the two SDKs" % rel)
    return problems


# --------------------------------------------------------------------------
# Manifest and ZIP
# --------------------------------------------------------------------------
def archive_root(version: str) -> str:
    return "Pixelica-%s" % version


def platform_tag() -> str:
    raw = "%s-%s" % (platform.system().lower() or "unknown", platform.machine() or "unknown")
    return re.sub(r"[^A-Za-z0-9._-]", "_", raw)


def build_manifest(variant: Variant, version: str, stage: pathlib.Path, files: Sequence[str],
                   generator: Optional[str], missing_docs: Sequence[str]) -> Dict[str, object]:
    """Deterministic description of an SDK: no timestamps, sorted entries. The
    manifest file itself is not listed (it cannot contain its own hash)."""
    entries = []
    for rel in sorted(files):
        full = stage / rel
        entries.append({"path": rel, "size": full.stat().st_size, "sha256": sha256_file(full)})
    return {
        "name": variant.name,
        "version": version,
        "archive_root": archive_root(version),
        "install_components": list(variant.components),
        "configurations": list(CONFIGS),
        "generator": generator or "default",
        "package_configs": ["find_package(Pixelica CONFIG)", "find_package(PixelicaUI CONFIG)"] +
                           (["find_package(Pixelica2D CONFIG)"] if variant.name == "Pixelica2D" else []),
        "missing_documents": sorted(missing_docs),
        "files": entries,
    }


def manifest_path(variant: Variant) -> str:
    return "share/doc/%s/MANIFEST.json" % variant.name


def write_manifest(stage: pathlib.Path, variant: Variant, manifest: Dict[str, object]) -> str:
    rel = manifest_path(variant)
    target = stage / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("x", encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")
    return rel


def zip_timestamp() -> Tuple[int, int, int, int, int, int]:
    epoch = EPOCH_FLOOR
    raw = os.environ.get("SOURCE_DATE_EPOCH")
    if raw is not None:
        try:
            epoch = max(int(raw, 10), EPOCH_FLOOR)
        except ValueError:
            raise PackagingError("SOURCE_DATE_EPOCH must be an integer (got '%s')" % raw) from None
    stamp = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
    return (stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second & ~1)


def safe_arcname(root: str, rel: str) -> str:
    """root/rel with forward slashes; rejects absolute paths and '..'."""
    parts = rel.split("/")
    if rel.startswith("/") or "\\" in rel or any(p in ("", ".", "..") for p in parts) or ":" in parts[0]:
        raise PackagingError("unsafe archive member path '%s'" % rel)
    return "%s/%s" % (root, rel)


def write_zip(stage: pathlib.Path, root: str, out_path: pathlib.Path) -> List[str]:
    """Writes <stage>'s files as <root>/<relative path> in sorted order with
    fixed timestamps (reproducible). Refuses to overwrite <out_path>. Returns
    the member names."""
    if out_path.exists():
        raise PackagingError("refusing to overwrite existing archive '%s'" % out_path)
    files = list_files(stage)
    stamp = zip_timestamp()
    temp = out_path.with_name(out_path.name + ".partial")
    names: List[str] = []
    try:
        with zipfile.ZipFile(str(temp), mode="x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for rel in files:
                arcname = safe_arcname(root, rel)
                source = stage / rel
                info = zipfile.ZipInfo(arcname, stamp)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3  # Unix: the permission bits below are honored
                executable = os.stat(str(source)).st_mode & 0o111
                info.external_attr = (0o100755 if executable else 0o100644) << 16
                with source.open("rb") as handle, archive.open(info, "w") as member:
                    shutil.copyfileobj(handle, member, 1 << 20)
                names.append(arcname)
        os.replace(str(temp), str(out_path))
    except BaseException:
        # Remove only the single partial file this call created (no directories).
        try:
            if temp.is_file():
                temp.unlink()
        except OSError:
            pass
        raise
    return names


def write_sha256sums(output_dir: pathlib.Path, archives: Sequence[pathlib.Path]) -> pathlib.Path:
    target = output_dir / "SHA256SUMS.txt"
    lines = ["%s  %s\n" % (sha256_file(path), path.name) for path in sorted(archives, key=lambda p: p.name)]
    with target.open("x", encoding="utf-8", newline="\n") as handle:
        handle.writelines(lines)
    return target


# --------------------------------------------------------------------------
# Orchestration
# --------------------------------------------------------------------------
def copy_documents(source: pathlib.Path, stage: pathlib.Path, variant: Variant) -> List[str]:
    """Copies the variant's documents to share/doc/<name>/; returns the missing ones."""
    missing: List[str] = []
    destination = stage / "share" / "doc" / variant.name
    wanted = list(variant.docs)
    for license_name in ("LICENSE", "LICENSE.md", "LICENSE.txt"):
        if (source / license_name).is_file():
            wanted.append(license_name)
    for rel in wanted:
        origin = source / rel
        if not origin.is_file():
            missing.append(rel)
            continue
        # Preserve README -> docs/ and guide -> verification/ relative links.
        target = destination / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            raise PackagingError("document name collision: %s" % target)
        shutil.copyfile(str(origin), str(target))
    return missing


def package(args: argparse.Namespace) -> int:
    cmake = args.cmake or shutil.which("cmake") or ("cmake" if args.dry_run else None)
    if cmake is None:
        raise PackagingError("cmake was not found on PATH; pass --cmake")
    source: pathlib.Path = args.source_dir
    version = read_version(source)
    run_id = args.run_id or default_run_id()
    stage_root = args.build_dir / "stage" / run_id
    output_dir: pathlib.Path = args.output_dir
    root = archive_root(version)
    tag = platform_tag()
    archive_paths = {v.name: output_dir / ("%s-%s-%s.zip" % (v.name, version, tag)) for v in VARIANTS}

    steps = plan_steps(args, cmake, stage_root)
    print("Pixelica SDK packaging %s (%s)" % (version, "dry run: nothing is executed" if args.dry_run else "live"))
    print("  source     : %s" % source)
    print("  build dir  : %s" % args.build_dir)
    print("  staging    : %s" % stage_root)
    print("  output dir : %s" % output_dir)
    for name, path in archive_paths.items():
        print("  archive    : %s" % path)

    if args.dry_run:
        for step in steps:
            run_step(step, True)
        for variant in VARIANTS:
            print("==> %s: copy documents %s, verify content policy, write %s, zip as %s/..." %
                  (variant.name, ", ".join(variant.docs), manifest_path(variant), root))
        print("==> verify files shared by both SDKs are identical; write SHA256SUMS.txt")
        return 0

    # Preconditions: everything is checked before the first byte is written.
    check_cmake_version(cmake)
    for tree, _configs in build_trees(args):
        check_build_tree(tree, source)
    ensure_fresh_dir(stage_root)
    for path in archive_paths.values():
        if path.exists():
            raise PackagingError("refusing to overwrite existing archive '%s'" % path)
    if (output_dir / "SHA256SUMS.txt").exists():
        raise PackagingError("refusing to overwrite '%s'" % (output_dir / "SHA256SUMS.txt"))
    output_dir.mkdir(parents=True, exist_ok=True)

    for step in steps:
        run_step(step, False)

    manifests = {}
    for variant in VARIANTS:
        stage = stage_root / variant.name
        if not stage.is_dir():
            raise PackagingError("install produced no %s staging directory" % variant.name)
        missing_docs = copy_documents(source, stage, variant)
        if missing_docs:
            print("warning: %s: documents not found and not packaged: %s" %
                  (variant.name, ", ".join(missing_docs)), file=sys.stderr)
        files = list_files(stage)
        problems = check_variant_files(variant.name, files)
        if problems:
            raise PackagingError("%s SDK content check failed:\n  %s" % (variant.name, "\n  ".join(problems)))
        manifests[variant.name] = (files, missing_docs)

    shared_problems = check_shared_files_identical(stage_root / "Pixelica2D", stage_root / "Pixelica3D")
    if shared_problems:
        raise PackagingError("the two SDKs conflict when merged:\n  " + "\n  ".join(shared_problems))

    for variant in VARIANTS:
        stage = stage_root / variant.name
        files, missing_docs = manifests[variant.name]
        manifest = build_manifest(variant, version, stage, files, args.generator, missing_docs)
        write_manifest(stage, variant, manifest)
        members = write_zip(stage, root, archive_paths[variant.name])
        print("wrote %s (%d files)" % (archive_paths[variant.name], len(members)))

    sums = write_sha256sums(output_dir, list(archive_paths.values()))
    print("wrote %s" % sums)
    for name, path in archive_paths.items():
        print("  %s  %s" % (sha256_file(path), path.name))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    try:
        return package(args)
    except PackagingError as error:
        print("error: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
