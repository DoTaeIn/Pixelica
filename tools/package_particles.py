#!/usr/bin/env python3
"""Build the optional particle add-on (Debug + Release) and portable source ZIP.

Uses the base SDK packager's path, archive and SHA helpers. Never deletes a
directory or overwrites an archive. The binary ZIP merges into Pixelica-x.y.z/;
the source ZIP is a standalone CMake project against an installed base SDK.
"""
import argparse
import json
import pathlib
import shutil
import sys

import package_sdk as sdk


COMPONENTS = ("particles", "particles2d", "particles25d")


def stage_source(source, stage):
    """Explicit allowlist: no private logs, engine files, build outputs or Git."""
    files = {
        "CMakeLists.txt": source / "sdk/particles-addon/CMakeLists.txt",
        "cmake/PixelicaPackage.cmake": source / "cmake/PixelicaPackage.cmake",
        "docs/particles.md": source / "docs/particles.md",
        "examples/hello_particles/main.c": source / "examples/hello_particles/main.c",
    }
    module = source / "modules/particles"
    for rel in sdk.list_files(module):
        if pathlib.PurePosixPath(rel).suffix in (".c", ".h") or rel == "CMakeLists.txt":
            files["modules/particles/" + rel] = module / rel
    for rel, path in files.items():
        if not path.is_file() or path.is_symlink():
            raise sdk.PackagingError("missing or unsafe source file: %s" % path)
        dest = stage / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
    (stage / "README.md").write_text(
        "# Pixelica Particles source add-on\n\n"
        "Requires the matching Pixelica 0.1.x base SDK and a C11 compiler.\n\n"
        "```sh\ncmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Pixelica-0.1.0 "
        "-DPIXELICA_PARTICLES_BUILD_2D=ON\n"
        "cmake --build build --config Release\n"
        "cmake --install build --config Release --prefix /path/to/Pixelica-0.1.0\n```\n\n"
        "Use the same compiler, architecture and configuration as the base SDK. "
        "For 3D billboards merge both base SDKs first and also set "
        "`PIXELICA_PARTICLES_BUILD_25D=ON`. See [the guide](docs/particles.md).\n",
        encoding="utf-8")


def check_binary(stage):
    files = sdk.list_files(stage)
    problems = []
    for component, export in zip(COMPONENTS, ("Particles", "Particles2D", "Particles25D")):
        if "include/pixelica/%s.h" % component not in files:
            problems.append("missing %s header" % component)
        for config in ("debug", "release"):
            if not any(p.endswith("/Pixelica%sTargets-%s.cmake" % (export, config)) for p in files):
                problems.append("missing %s %s export" % (component, config))
        if not any(sdk._is_library_file(p) and pathlib.PurePosixPath(p).name.startswith(
                ("pixelica_" + component, "libpixelica_" + component)) for p in files):
            problems.append("missing %s library" % component)
    if not any(p.endswith("/PixelicaPackage.cmake") for p in files):
        problems.append("missing updated component loader")
    for p in files:
        if sdk._is_library_file(p):
            name = pathlib.PurePosixPath(p).name
            if not name.startswith(("pixelica_particles", "libpixelica_particles")):
                problems.append("unrelated library: %s" % p)
        if p.startswith("include/") and p not in {
            "include/pixelica/%s.h" % component for component in COMPONENTS
        }:
            problems.append("unrelated header: %s" % p)
    if problems:
        raise sdk.PackagingError("invalid particle add-on:\n" + "\n".join(problems))
    return files


def package(args):
    source = args.source_dir.resolve()
    output = args.output_dir.resolve()
    build = args.build_dir.resolve()
    version = sdk.read_version(source)
    names = ("PixelicaParticles-%s-%s.zip" % (version, sdk.platform_tag()),
             "PixelicaParticles-%s-source.zip" % version)
    output.mkdir(parents=True, exist_ok=True)
    archives = [output / name for name in names]
    if any(p.exists() for p in archives) or (output / "SHA256SUMS.txt").exists():
        raise sdk.PackagingError("output archives/checksums already exist; choose a fresh output directory")
    sdk.ensure_fresh_dir(build)
    stage = build / "stage"
    binary = stage / "binary"
    portable = stage / "source"
    sdk.check_cmake_version(args.cmake)
    for config in sdk.CONFIGS:
        # Separate trees work with both single-configuration and VS generators.
        tree = build / config.lower()
        configure = [args.cmake, "-S", str(source), "-B", str(tree)]
        if args.generator:
            configure += ["-G", args.generator]
        configure += [p for p in sdk.configure_options(config)
                      if not p.startswith("-DPIXELICA_BUILD_PARTICLES=")]
        configure += ["-DPIXELICA_BUILD_PARTICLES=ON"]
        sdk.run_step(sdk.Step("configure particles " + config, tuple(configure)), False)
        sdk.run_step(sdk.Step("build particles " + config, (
            args.cmake, "--build", str(tree), "--config", config, "--parallel", str(args.jobs))), False)
        for component in COMPONENTS:
            sdk.run_step(sdk.Step("install " + component, (
                args.cmake, "--install", str(tree), "--config", config,
                "--prefix", str(binary), "--component", component)), False)
    check_binary(binary)
    stage_source(source, portable)
    (binary / "share/Pixelica/docs").mkdir(parents=True, exist_ok=True)
    (binary / "share/Pixelica/docs/PARTICLES_INSTALL.md").write_text(
        "# Optional Pixelica Particles add-on\n\n"
        "Extract into the SAME parent as the matching Pixelica-" + version +
        " base SDK, after extracting the base SDK(s). Allow replacing only "
        "lib/cmake/Pixelica/PixelicaPackage.cmake. No base library is replaced.\n\n"
        "Platform: " + sdk.platform_tag() + ". Generator: " + (args.generator or "CMake default") +
        ". Debug and Release are included. Match the base SDK's compiler and architecture; "
        "other compilers/platforms can build the separate source ZIP.\n\n"
        "particles needs common; particles2d also needs the 2D SDK; particles25d "
        "needs BOTH the 2D and 3D SDKs. Only requested components are loaded.\n",
        encoding="utf-8")
    files = check_binary(binary)
    manifest = {"name": "PixelicaParticles", "version": version,
                "components": list(COMPONENTS), "files": files,
                "dependencies": {"particles": ["common"], "particles2d": ["particles", "renderer2d"],
                                 "particles25d": ["particles", "hybrid"]},
                "sha256": {p: sdk.sha256_file(binary / p) for p in files}}
    (binary / "share/Pixelica/particles-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    sdk.write_zip(binary, sdk.archive_root(version), archives[0])
    sdk.write_zip(portable, "PixelicaParticles-%s-source" % version, archives[1])
    sdk.write_sha256sums(output, archives)
    for p in archives:
        print("wrote %s (%d bytes)" % (p, p.stat().st_size))
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
