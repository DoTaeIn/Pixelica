#!/usr/bin/env python3
"""Bootstrap for the Pixelica desktop editor.

    python tools/editor/launch_editor.py            # build bridge if missing, open editor
    python tools/editor/launch_editor.py --build    # (re)build the native bridge first
    python tools/editor/launch_editor.py --test     # build, run native CTest + Python unit tests

The native bridge is built with CMake from tools/editor/native into
build-editor/ at the repository root. CMake is taken from PATH or, on Windows,
from the Visual Studio 2022 bundled copy (found via vswhere or the standard
install folders). Nothing here runs code from a project file.
"""

from __future__ import annotations

import argparse
import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

EDITOR_DIR = Path(__file__).resolve().parent
REPO_ROOT = EDITOR_DIR.parents[1]
NATIVE_DIR = EDITOR_DIR / "native"
TESTS_DIR = EDITOR_DIR / "tests"
DEFAULT_BUILD_DIR = REPO_ROOT / "build-editor"
VS_CMAKE_SUFFIX = Path("Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")


def library_filename() -> str:
    if sys.platform == "win32":
        return "pixelica_editor_bridge.dll"
    if sys.platform == "darwin":
        return "pixelica_editor_bridge.dylib"
    return "pixelica_editor_bridge.so"


def find_built_library(build_dir: Path, config: str) -> Path | None:
    name = library_filename()
    for candidate in (build_dir / config / name, build_dir / name):
        if candidate.is_file():
            return candidate
    return None


def _vswhere_installations() -> list[Path]:
    roots = [os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")]
    for root in filter(None, roots):
        vswhere = Path(root) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if not vswhere.is_file():
            continue
        try:
            out = subprocess.run(
                [str(vswhere), "-products", "*", "-version", "[17.0,18.0)", "-property", "installationPath"],
                capture_output=True, text=True, timeout=30, check=False)
        except (OSError, subprocess.SubprocessError):
            continue
        return [Path(line.strip()) for line in out.stdout.splitlines() if line.strip()]
    return []


def find_cmake() -> Path | None:
    found = shutil.which("cmake")
    if found:
        return Path(found)
    if sys.platform != "win32":
        return None
    for install in _vswhere_installations():
        candidate = install / VS_CMAKE_SUFFIX
        if candidate.is_file():
            return candidate
    for root in filter(None, (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)"))):
        pattern = str(Path(root) / "Microsoft Visual Studio" / "2022" / "*" / VS_CMAKE_SUFFIX)
        matches = sorted(glob.glob(pattern))
        if matches:
            return Path(matches[0])
    return None


def find_ctest(cmake: Path) -> Path | None:
    name = "ctest.exe" if sys.platform == "win32" else "ctest"
    sibling = cmake.with_name(name)
    if sibling.is_file():
        return sibling
    found = shutil.which("ctest")
    return Path(found) if found else None


def run(cmd: list[str], **kwargs) -> int:
    print("+ " + " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    return subprocess.run(cmd, check=False, **kwargs).returncode


def build_bridge(build_dir: Path, config: str, werror: bool) -> Path:
    cmake = find_cmake()
    if cmake is None:
        raise SystemExit("CMake not found. Install CMake (or Visual Studio 2022 with C++ CMake tools) "
                         "and make sure 'cmake' is on PATH.")
    configure = [str(cmake), "-S", str(NATIVE_DIR), "-B", str(build_dir),
                 f"-DCMAKE_BUILD_TYPE={config}",
                 f"-DPIXELICA_EDITOR_WARNINGS_AS_ERRORS={'ON' if werror else 'OFF'}"]
    if run(configure) != 0:
        raise SystemExit("CMake configure failed")
    if run([str(cmake), "--build", str(build_dir), "--config", config]) != 0:
        raise SystemExit("native bridge build failed")
    library = find_built_library(build_dir, config)
    if library is None:
        raise SystemExit(f"build finished but {library_filename()} was not found in {build_dir}")
    return library


def check_python_deps() -> None:
    if sys.version_info < (3, 10):
        raise SystemExit("Pixelica editor needs Python 3.10 or newer")
    try:
        import tkinter  # noqa: F401
        from tkinter import ttk  # noqa: F401
    except ImportError as exc:
        raise SystemExit(f"tkinter is not available ({exc}); install Python with Tk support") from exc
    try:
        import PIL  # noqa: F401
        from PIL import Image  # noqa: F401
    except ImportError as exc:
        raise SystemExit(f"Pillow is not available ({exc}); run: {sys.executable} -m pip install Pillow") from exc


def run_tests(build_dir: Path, config: str, library: Path) -> int:
    status = 0
    cmake = find_cmake()
    ctest = find_ctest(cmake) if cmake else None
    if ctest is None:
        print("ctest not found; native C tests not run", file=sys.stderr)
        status = 1
    elif run([str(ctest), "--test-dir", str(build_dir), "-C", config, "--output-on-failure"]) != 0:
        status = 1
    env = dict(os.environ)
    env["PIXELICA_EDITOR_LIBRARY"] = str(library)
    env["PIXELICA_EDITOR_REQUIRE_NATIVE"] = "1"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONPATH"] = os.pathsep.join(filter(None, [str(EDITOR_DIR), env.get("PYTHONPATH")]))
    unit = [sys.executable, "-B", "-m", "unittest", "discover", "-s", str(TESTS_DIR), "-p", "test*.py", "-v"]
    if run(unit, env=env, cwd=str(EDITOR_DIR)) != 0:
        status = 1
    return status


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Pixelica desktop editor launcher")
    parser.add_argument("--build", action="store_true", help="(re)build the native bridge before starting")
    parser.add_argument("--test", action="store_true", help="build, then run native and Python tests (no GUI)")
    parser.add_argument("--config", default="Release", choices=["Release", "Debug", "RelWithDebInfo"],
                        help="CMake build configuration (default Release)")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR,
                        help="CMake binary directory (default <repo>/build-editor)")
    parser.add_argument("--werror", action="store_true", help="treat C warnings as errors")
    args = parser.parse_args(argv)

    check_python_deps()
    build_dir = args.build_dir.resolve()
    library = None
    if not (args.build or args.test):
        # Same precedence as runtime.find_library: environment, build dir, bundled copy.
        env_library = os.environ.get("PIXELICA_EDITOR_LIBRARY")
        if env_library:
            library = Path(env_library)
            if not library.is_file():
                raise SystemExit(f"PIXELICA_EDITOR_LIBRARY points to a missing file: {library}")
        else:
            library = find_built_library(build_dir, args.config)
            bundled = NATIVE_DIR / library_filename()
            if library is None and bundled.is_file():
                library = bundled
    if library is None:
        library = build_bridge(build_dir, args.config, args.werror)
    print(f"native bridge: {library}", flush=True)

    if args.test:
        return run_tests(build_dir, args.config, library)

    os.environ["PIXELICA_EDITOR_LIBRARY"] = str(library)
    if str(EDITOR_DIR) not in sys.path:
        sys.path.insert(0, str(EDITOR_DIR))
    from pixelica_editor.app import main as app_main
    app_main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
