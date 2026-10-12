#!/usr/bin/env python3
"""Package optional CPU shaders separately from either base SDK.

Build Debug + Release libraries and a portable standalone source ZIP. Never
delete directories, overwrite archives or replace the base SDK's files.
"""
import argparse
import json
import pathlib
import shutil
import sys

import package_sdk as sdk


SOURCE_FILES = {
    "CMakeLists.txt": "sdk/shaders-addon/CMakeLists.txt",
    "cmake/PixelicaShadersConfig.cmake.in": "cmake/PixelicaShadersConfig.cmake.in",
    "docs/shaders.md": "docs/shaders.md",
    "examples/hello_shaders/main.c": "examples/hello_shaders/main.c",
    "modules/shaders/CMakeLists.txt": "modules/shaders/CMakeLists.txt",
    "modules/shaders/shaders.c": "modules/shaders/shaders.c",
    "modules/shaders/include/pixelica/shaders.h": "modules/shaders/include/pixelica/shaders.h",
}


def stage_source(source, stage):
    for rel, original in SOURCE_FILES.items():
        path = source / original
        if not path.is_file() or path.is_symlink():
            raise sdk.PackagingError("missing or unsafe shader source: %s" % path)
        dest = stage / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
    (stage / "README.md").write_text(
        "# Pixelica Shaders source add-on\n\n"
        "Requires a matching Pixelica 0.1.x base SDK and C11 compiler.\n\n"
        "```sh\ncmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Pixelica-0.1.0 "
        "-DPIXELICA_SHADERS_BUILD_EXAMPLE=ON\n"
        "cmake --build build --config Release\n"
        "cmake --install build --config Release --prefix /path/to/Pixelica-0.1.0\n```\n\n"
        "Match the base SDK compiler, architecture and configuration. "
        "See [the guide](docs/shaders.md) for APIs and memory ownership.\n",
        encoding="utf-8")


def check_binary(stage):
    files = sdk.list_files(stage)
    required = {
        "include/pixelica/shaders.h",
        "lib/cmake/PixelicaShaders/PixelicaShadersConfig.cmake",
        "lib/cmake/PixelicaShaders/PixelicaShadersConfigVersion.cmake",
        "lib/cmake/PixelicaShaders/PixelicaShadersTargets.cmake",
        "lib/cmake/PixelicaShaders/PixelicaShadersTargets-debug.cmake",
        "lib/cmake/PixelicaShaders/PixelicaShadersTargets-release.cmake",
    }
    problems = ["missing " + p for p in sorted(required.difference(files))]
    libs = [p for p in files if sdk._is_library_file(p)]
    allowed_libs = {"pixelica_shaders.lib", "pixelica_shadersd.lib",
                    "libpixelica_shaders.a", "libpixelica_shadersd.a"}
    if len(libs) != 2:
        problems.append("expected exactly Debug and Release shader libraries")
    for p in files:
        if sdk._is_library_file(p) and pathlib.PurePosixPath(p).name not in allowed_libs:
            problems.append("unrelated library: " + p)
        if p.startswith("include/") and p != "include/pixelica/shaders.h":
            problems.append("unrelated header: " + p)
        if "/cmake/" in p and p not in required:
            problems.append("unrelated CMake file: " + p)
    if problems:
        raise sdk.PackagingError("invalid shader add-on:\n" + "\n".join(problems))
    return files


def package(args):
    source, output, build = (p.resolve() for p in (args.source_dir, args.output_dir, args.build_dir))
    version = sdk.read_version(source)
    names = ("PixelicaShaders-%s-%s.zip" % (version, sdk.platform_tag()),
             "PixelicaShaders-%s-source.zip" % version)
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
        configure += [p for p in sdk.configure_options(config)
                      if not p.startswith("-DPIXELICA_BUILD_SHADERS=")]
        configure += ["-DPIXELICA_BUILD_SHADERS=ON"]
        sdk.run_step(sdk.Step("configure shaders " + config, tuple(configure)), False)
        sdk.run_step(sdk.Step("build shaders " + config, (
            args.cmake, "--build", str(tree), "--config", config,
            "--target", "pixelica_shaders", "--parallel", str(args.jobs))), False)
        sdk.run_step(sdk.Step("install shaders " + config, (
            args.cmake, "--install", str(tree), "--config", config,
            "--prefix", str(binary), "--component", "shaders")), False)
    stage_source(source, portable)
    (binary / "share/Pixelica/docs/SHADERS_INSTALL.md").write_text(
        "# Optional Pixelica Shaders add-on\n\n"
        "Extract after either matching base SDK into its SAME parent directory; "
        "both use the Pixelica-" + version + "/ root. No base file is replaced.\n\n"
        "Platform: " + sdk.platform_tag() + ". Generator: " + (args.generator or "CMake default") +
        ". Debug and Release included. Match the base SDK compiler and architecture. "
        "Other platforms can build the separate source ZIP.\n\n"
        "Use find_package(PixelicaShaders CONFIG REQUIRED), then link Pixelica::shaders. "
        "Needs only Pixelica::common; compatible with either renderer. See shaders.md.\n",
        encoding="utf-8")
    files = check_binary(binary)
    manifest = {"name": "PixelicaShaders", "version": version,
                "package": "PixelicaShaders", "target": "Pixelica::shaders",
                "dependencies": ["Pixelica::common"], "files": files,
                "sha256": {p: sdk.sha256_file(binary / p) for p in files}}
    (binary / "share/Pixelica/shaders-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    sdk.write_zip(binary, sdk.archive_root(version), archives[0])
    sdk.write_zip(portable, "PixelicaShaders-%s-source" % version, archives[1])
    sdk.write_sha256sums(output, archives)
    for path in archives:
        print("wrote %s (%d bytes)" % (path, path.stat().st_size))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent)
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
