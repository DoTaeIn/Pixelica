#!/usr/bin/env python3
"""Package the optional water add-on separately from either base SDK.

Writes THREE things into --output-dir, and never touches the base SDK:

    PixelicaWater-<version>-<platform>.zip   Debug + Release libraries,
        headers, the independent PixelicaWater CMake package and the guide,
        for the `water` and `water3d` install components.
    PixelicaWater-<version>-source.zip       the portable source add-on, which
        builds against an installed base SDK on any platform.
    SHA256SUMS.txt                           the checksums of both archives.

The binary archive uses the same Pixelica-<version>/ root as the base SDKs, so
extracting it into the SAME parent directory merges it into that prefix. It
contains no base SDK file at all - check_binary() refuses to write an archive
that holds one - so the merge can never replace anything.

Safety rules (inherited from package_sdk.py, which this module reuses):
    * Nothing is ever deleted. Staging happens in fresh directories and an
      existing, populated one is a hard error. Existing archives are never
      overwritten.
    * Every external program is started from an argument list, never a shell.
    * No network access, no downloads, no publishing.

Usage:
    python tools/package_water.py --build-dir build-water-pkg --output-dir dist/water
"""
import argparse
import json
import pathlib
import shutil
import sys

import package_sdk as sdk

# The install components the add-on ships, in install order.
COMPONENTS = ("water", "water3d")

# "<destination in the source ZIP>": "<path in the repository>".
# cmake/PixelicaWaterSourceCheck.cmake stages exactly this list, and
# tools/tests/test_package_water.py fails if the two ever drift apart.
SOURCE_FILES = {
    "CMakeLists.txt": "sdk/water-addon/CMakeLists.txt",
    "cmake/PixelicaWaterConfig.cmake.in": "cmake/PixelicaWaterConfig.cmake.in",
    "docs/water.md": "docs/water.md",
    "examples/hello_water/hello_water.c": "examples/hello_water/hello_water.c",
    "modules/water/CMakeLists.txt": "modules/water/CMakeLists.txt",
    "modules/water/water.c": "modules/water/water.c",
    "modules/water/water3d.c": "modules/water/water3d.c",
    "modules/water/include/pixelica/water.h": "modules/water/include/pixelica/water.h",
    "modules/water/include/pixelica/water3d.h": "modules/water/include/pixelica/water3d.h",
}

HEADERS = ("include/pixelica/water.h", "include/pixelica/water3d.h")

ALLOWED_LIBS = {
    "pixelica_water.lib", "pixelica_waterd.lib",
    "libpixelica_water.a", "libpixelica_waterd.a",
    "pixelica_water3d.lib", "pixelica_water3dd.lib",
    "libpixelica_water3d.a", "libpixelica_water3dd.a",
}

CMAKE_DIR = "lib/cmake/PixelicaWater"


def required_files():
    """The exact set of non-library files the binary add-on must contain."""
    names = ["PixelicaWaterConfig.cmake", "PixelicaWaterConfigVersion.cmake"]
    for export in ("PixelicaWaterTargets", "PixelicaWater3DTargets"):
        names += [export + ".cmake"] + [
            "%s-%s.cmake" % (export, config.lower()) for config in sdk.CONFIGS]
    return set(HEADERS) | {"%s/%s" % (CMAKE_DIR, name) for name in names} | {
        "share/Pixelica/docs/water.md"}


def stage_source(source, stage):
    """Copies exactly SOURCE_FILES into <stage>, then writes its README."""
    for rel, original in SOURCE_FILES.items():
        path = source / original
        if not path.is_file() or path.is_symlink():
            raise sdk.PackagingError("missing or unsafe water source: %s" % path)
        dest = stage / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
    (stage / "README.md").write_text(
        "# Pixelica Water source add-on\n\n"
        "Optional analytic ocean surface + sample-point buoyancy, plus a 3D draw\n"
        "adapter. Requires a matching Pixelica 0.1.x base SDK and a C11 compiler.\n"
        "The core (`Pixelica::water`) needs only `Pixelica::common`; the adapter\n"
        "(`Pixelica::water3d`) is built only when the same prefix also has the 3D\n"
        "core, so this add-on works on a 2D-only SDK too.\n\n"
        "```sh\ncmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Pixelica-0.1.0 \\\n"
        "      -DPIXELICA_WATER_BUILD_EXAMPLE=ON\n"
        "cmake --build build --config Release\n"
        "cmake --install build --config Release --prefix /path/to/Pixelica-0.1.0\n```\n\n"
        "`PIXELICA_WATER_BUILD_EXAMPLE=ON` also builds the raft demo, which needs\n"
        "the 3D core. Match the base SDK compiler, architecture and configuration.\n"
        "See [the guide](docs/water.md) for the API, the units and the limits.\n",
        encoding="utf-8")


def check_binary(stage):
    """Returns the staged file list, or raises if it is not a clean add-on."""
    files = sdk.list_files(stage)
    present = set(files)
    required = required_files()
    problems = ["missing " + p for p in sorted(required.difference(present))]

    libs = [p for p in files if sdk._is_library_file(p)]
    if len(libs) != 4:
        problems.append(
            "expected exactly Debug and Release libraries for water and water3d, found %d"
            % len(libs))
    for path in files:
        name = pathlib.PurePosixPath(path).name
        if sdk._is_library_file(path) and name not in ALLOWED_LIBS:
            problems.append("unrelated library: " + path)
        if path.startswith("include/") and path not in HEADERS:
            problems.append("unrelated header: " + path)
        if "/cmake/" in path and path not in required:
            problems.append("unrelated CMake file: " + path)
        # The add-on must never carry, and therefore never replace, a base file.
        if name in ("PixelicaPackage.cmake", "PixelicaConfig.cmake",
                    "PixelicaConfigVersion.cmake", "Pixelica2DConfig.cmake"):
            problems.append("would replace a base SDK file: " + path)
    if problems:
        raise sdk.PackagingError("invalid water add-on:\n  " + "\n  ".join(problems))
    return files


def package(args):
    source, output, build = (p.resolve() for p in (args.source_dir, args.output_dir, args.build_dir))
    version = sdk.read_version(source)
    names = ("PixelicaWater-%s-%s.zip" % (version, sdk.platform_tag()),
             "PixelicaWater-%s-source.zip" % version)
    output.mkdir(parents=True, exist_ok=True)
    archives = [output / name for name in names]
    if any(p.exists() for p in archives) or (output / "SHA256SUMS.txt").exists():
        raise sdk.PackagingError("archives/checksums already exist; choose a fresh output directory")
    sdk.ensure_fresh_dir(build)
    binary, portable = build / "stage/binary", build / "stage/source"
    sdk.check_cmake_version(args.cmake)

    for config in sdk.CONFIGS:
        tree = build / config.lower()
        configure = [args.cmake, "-S", str(source), "-B", str(tree)]
        if args.generator:
            configure += ["-G", args.generator]
        # The base profile with water forced ON; everything else stays as the
        # base SDK builds it, so the libraries are ABI-compatible with it.
        configure += [p for p in sdk.configure_options(config)
                      if not p.startswith("-DPIXELICA_BUILD_WATER=")]
        configure += ["-DPIXELICA_BUILD_WATER=ON"]
        sdk.run_step(sdk.Step("configure water " + config, tuple(configure)), False)
        for target in ("pixelica_water", "pixelica_water3d"):
            sdk.run_step(sdk.Step("build %s %s" % (target, config), (
                args.cmake, "--build", str(tree), "--config", config,
                "--target", target, "--parallel", str(args.jobs))), False)
        for component in COMPONENTS:
            sdk.run_step(sdk.Step("install %s %s" % (component, config), (
                args.cmake, "--install", str(tree), "--config", config,
                "--prefix", str(binary), "--component", component)), False)

    stage_source(source, portable)
    (binary / "share/Pixelica/docs/WATER_INSTALL.md").write_text(
        "# Optional Pixelica Water add-on\n\n"
        "Extract after either matching base SDK into its SAME parent directory; "
        "both use the Pixelica-" + version + "/ root, and this add-on replaces no "
        "base file.\n\n"
        "Platform: " + sdk.platform_tag() + ". Generator: " + (args.generator or "CMake default") +
        ". Debug and Release included. Match the base SDK compiler and architecture. "
        "Other platforms can build the separate source ZIP.\n\n"
        "```cmake\nfind_package(PixelicaWater CONFIG REQUIRED)                   "
        "# Pixelica::water\nfind_package(PixelicaWater CONFIG REQUIRED COMPONENTS water3d) "
        "# + Pixelica::water3d\n```\n\n"
        "`Pixelica::water` needs only `Pixelica::common` and no renderer. "
        "`Pixelica::water3d` additionally needs the 3D core (`Pixelica::core`) in the "
        "same prefix; in a prefix without it the package still loads and reports the "
        "`water3d` component as not found. See water.md.\n",
        encoding="utf-8")

    files = check_binary(binary)
    manifest = {
        "name": "PixelicaWater",
        "version": version,
        "package": "PixelicaWater",
        "archive_root": sdk.archive_root(version),
        "platform": sdk.platform_tag(),
        "configurations": list(sdk.CONFIGS),
        "generator": args.generator or "default",
        "install_components": list(COMPONENTS),
        "targets": {"water": "Pixelica::water", "water3d": "Pixelica::water3d"},
        "dependencies": {"water": ["Pixelica::common"],
                         "water3d": ["Pixelica::water", "Pixelica::core"]},
        "files": files,
        "sha256": {p: sdk.sha256_file(binary / p) for p in files},
    }
    (binary / "share/Pixelica/water-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    sdk.write_zip(binary, sdk.archive_root(version), archives[0])
    sdk.write_zip(portable, "PixelicaWater-%s-source" % version, archives[1])
    sdk.write_sha256sums(output, archives)
    for path in archives:
        print("wrote %s (%d bytes)" % (path, path.stat().st_size))
    print("wrote %s" % (output / "SHA256SUMS.txt"))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent.parent)
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--cmake", default="cmake", type=sdk.validate_executable)
    parser.add_argument("--generator", type=sdk.validate_generator)
    parser.add_argument("--jobs", type=sdk.validate_jobs, default=4)
    try:
        return package(parser.parse_args(argv))
    except (sdk.PackagingError, OSError) as error:
        print("error: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
