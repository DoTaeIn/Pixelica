# Pixelica SDK (preview)

How to build a game of your own against Pixelica without touching this repository's sources. The
SDK is the portable engine core plus its public headers, a CMake package, a copyable starter
project and the offline asset tool. It is a software renderer: no GPU API is used anywhere, and
there is no audio, editor, ECS, glTF import or GPU game logic.

**Verification status.** The install/export rules, the starter template and the SDK check script run in
CTest (`sdk_installed_template`). Observed results (see `docs/REMAINING_AUDIT.md`):

- A copied standalone template built against only the installed package, ran and produced deterministic
  output on Windows and Linux.
- The source-tree template with optional SDL3 built and ran (hidden 60-frame run).
- A minimal Linux SDK build (examples and benchmarks off) passed its 13 tests.

The CI file has not been run on GitHub Actions. On the audited Windows machine, `out/sdk-win64` is an installed
prefix containing both Debug and Release libraries (ignored generated artifact); use it with
`-DCMAKE_PREFIX_PATH=<absolute path to out/sdk-win64>`.

## What gets installed

The table describes the 3D SDK (see "Two SDKs" below for the 2D split and the extra headers/libraries that a 2D or
hybrid install adds). `cmake --install` puts this under the prefix:

| Path | Content |
|---|---|
| `include/pixelica/*.h` | common and 3D public headers (`pixelica/pixelica.h` exposes the 3D API; module headers are included explicitly) |
| `lib/pixelica_common.lib` / `lib/libpixelica_common.a` (`pixelica_commond.*` for Debug) | shared math, input, animation, container and texture assets |
| `lib/pixelica_core.lib` / `lib/libpixelica_core.a` (`pixelica_cored.*` for Debug) | static core library |
| `lib/cmake/Pixelica/` | package config, shared loader, `PixelicaCommonTargets*.cmake`, `PixelicaTargets*.cmake` and requested module exports |
| `share/Pixelica/template/` | the starter project (below) |
| `share/Pixelica/tools/pixelica_pack/` | the asset tool (`pixelica_pack.py`, no third-party packages) |

The base package is **headless-capable and has no dependencies besides the C library** (and libm on
Unix). SDL3 is not part of it. The package carries no references to the build or source tree and no
warning flags: those are applied only inside this repository's own build (`pixelica_options` is
linked through `$<BUILD_INTERFACE:...>`). The version check is `SameMinorVersion` (pre-1.0: a new
minor version may change the API). Debug libraries get a `d` suffix so Debug and Release can share
one prefix.

The optional `online` component is an exception: it uses Windows WinHTTP or Linux libcurl and threads.
It is OFF by default and is loaded only when requested. It needs no renderer or SDL3.

## Install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_EXAMPLES=OFF -DPIXELICA_BUILD_TESTS=OFF
cmake --build build --config Release
cmake --install build --config Release --prefix /path/to/pixelica-prefix
# multi-config generators (Visual Studio): repeat build/install with --config Debug to add Debug libraries
```

`PIXELICA_INSTALL` is ON when Pixelica is the top-level project (`-DPIXELICA_INSTALL=OFF` disables it).
Builds with `-DPIXELICA_SANITIZE=ON` should not be installed for use by a game that is not built with the
same sanitizer; CTest does not register the SDK check in that case.

## Use the package

```cmake
cmake_minimum_required(VERSION 3.21)
project(MyGame LANGUAGES C)
set(CMAKE_C_STANDARD 11)

find_package(Pixelica 0.1 CONFIG REQUIRED)
add_executable(my_game main.c game.c)
target_link_libraries(my_game PRIVATE Pixelica::core)
```

```sh
cmake -S my_game -B my_game/build -DCMAKE_PREFIX_PATH=/path/to/pixelica-prefix   # or -DPixelica_DIR=<prefix>/lib/cmake/Pixelica
cmake --build my_game/build --config Release
```

`find_package` also defines `Pixelica_PACK` (path of the installed `pixelica_pack.py`, or empty) and
`Pixelica_TEMPLATE_DIR`. `Pixelica::core` is the only target unless optional components are requested
(next section). A game links it, owns the main loop
(or reuses the pattern below), and provides a platform: a `PIXELICA_PlatformOps` table with a clock, a
button reader and a `present_rgb565` callback (`include/pixelica/platform.h`). Games read only logical
buttons (`PIXELICA_BTN_*`).

## Two SDKs: Pixelica2D and Pixelica (added 2026-10-10)

Everything above describes the **Pixelica SDK**, whose use is unchanged (`find_package(Pixelica)` + `Pixelica::core`; the old
`--component core` install is still a complete 3D SDK). The repository can now also produce a separate **Pixelica2D SDK** for
people who only need the 2D renderer. The authoritative verification record (builds, installed-SDK consumers, packaging, hashes) is
`DIMENSIONS_AUDIT.md` (local-only record); the Korean usage guide is [`2d.md`](2d.md), the plan `2D_25D_PLAN.md` (local-only record).
Nothing here has run on a physical board.

| | Pixelica2D SDK | Pixelica SDK |
|---|---|---|
| Components | `common`, `renderer2d` | `common`, `core` (= `renderer3d`), opt-in `hybrid`, optional `skeleton` / `ik` |
| Package | `find_package(Pixelica2D CONFIG REQUIRED)` -> `Pixelica2D::core` (also loads the matching `Pixelica` common config beside it) | `find_package(Pixelica CONFIG REQUIRED)` -> `Pixelica::core` |
| Headers | `status input platform math animation image assets_common renderer2d` | the same common headers plus `pixelica.h renderer.h assets.h scene.h` (and `hybrid.h` when installed) |
| 3D renderer, context, scene, mesh/cart code | **absent** | present |
| Starter project | `sdk/template2d`, installed as `share/Pixelica/template2d` (`Pixelica2D::core` only, no 3D header) | `sdk/template` |
| Asset converter `pixelica_pack` | not included | `share/Pixelica/tools/pixelica_pack` |

Both packages are relocatable: extract an archive anywhere and point `CMAKE_PREFIX_PATH` at it; the Pixelica2D config resolves
the Pixelica config in the same prefix rather than searching global installs. No SDL is shipped in either archive (the packaging
build is HEADLESS; supply a `PIXELICA_PlatformOps` display path). The optional skeleton and IK modules stay default OFF, build and install
independently, and are not part of the two base archives.

### Packaging the two ZIP archives

`tools/package_sdk.py` (Python standard library only, CMake 3.21+) configures one build, builds Debug and Release, installs the
components of each SDK into fresh staging directories and writes two ZIP archives (sorted members, fixed timestamps) plus `SHA256SUMS.txt`:

| Archive | Install components | Archive root |
|---|---|---|
| `Pixelica2D-<version>-<platform tag>.zip` | `common`, `renderer2d` | `Pixelica-<version>/` |
| `Pixelica3D-<version>-<platform tag>.zip` | `core`, `hybrid` | `Pixelica-<version>/` |

The default platform tag is the machine's `<os>-<architecture>`. Each archive carries `share/doc/<name>/MANIFEST.json` (paths, sizes,
SHA256). The script checks each SDK's content policy (the 2D archive must hold no 3D header, `pixelica_core` or hybrid) and that files present in both
SDKs are byte-identical before it writes anything.

```sh
python tools/package_sdk.py --build-dir build-sdk --output-dir dist
python tools/package_sdk.py --cmake /opt/cmake/bin/cmake --generator Ninja --build-dir /tmp/pixelica-build --output-dir /tmp/pixelica-dist --jobs 8
python tools/package_sdk.py --generator "Visual Studio 17 2022" --platform x64 --build-dir build-sdk --output-dir dist
python tools/package_sdk.py --dry-run          # print every command and file operation, run nothing
```

`--platform` is the CMake generator platform (`-A`), not the archive tag; `--c-compiler`, `--make-program`, `--toolset`, `--source-dir` and
`--run-id` exist too (`--help`). The script deletes nothing, never overwrites an existing archive and refuses a non-empty staging directory.
Exact verified results and archive hashes: `DIMENSIONS_AUDIT.md` (local-only record).

**2.5D.** Extract both archives into the same empty directory (they share the root `Pixelica-<version>/`, so they merge into one prefix). Merging
is supported only for archives of the same version, compiler, architecture and configuration ABI (the libraries are static). Archives from
one packaging run have byte-identical shared files; compare the SHA256 values in the manifests if you are unsure. The package config decides
what is available from which export files exist, so then `find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS core renderer2d hybrid)` works
and `Pixelica::hybrid` links.
`hybrid` needs both engines; a single archive gives a clear "component not found" for it. Without `COMPONENTS`, `find_package(Pixelica)`
defaults to 3D if its export is installed, otherwise 2D, otherwise common; it never imports every library by default.

**2D assets.** The 2D archive has no asset converter. RGB565 arrays work as they are; to produce `.p3tex`, run `tools/pixelica_pack/pixelica_pack.py texture`
from a repository checkout (the 3D archive carries a copy under `share/Pixelica/tools/pixelica_pack`).

## Optional modules: skeleton and IK

Skeletal posing/skinning and two-bone IK are **separate optional modules**, off by default, and are not part of
the package above (details in Korean: [`rigging.md`](rigging.md)). A default build/install contains none of their
headers, libraries, symbols or export files; `Pixelica::core` stays the only target.

| Option (default `OFF`) | Target | Install component | Export file |
|---|---|---|---|
| `PIXELICA_BUILD_SKELETON` | `Pixelica::skeleton` | `skeleton` | `PixelicaSkeletonTargets.cmake` |
| `PIXELICA_BUILD_IK` | `Pixelica::ik` | `ik` | `PixelicaIKTargets.cmake` |

```sh
cmake -S . -B build -DPIXELICA_BUILD_SKELETON=ON -DPIXELICA_BUILD_IK=ON -DPIXELICA_BUILD_EXAMPLES=OFF -DPIXELICA_BUILD_TESTS=OFF
cmake --build build --config Release
cmake --install build --config Release --prefix /path/to/prefix                       # everything configured
cmake --install build --config Release --prefix /path/to/prefix --component core      # core only
cmake --install build --config Release --prefix /path/to/prefix --component skeleton  # add one module
```

Every core install rule is `COMPONENT core`, so `--component core` installs a complete core package even when the
modules were configured. In a game:

```cmake
find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS skeleton ik)   # or OPTIONAL_COMPONENTS
target_link_libraries(my_game PRIVATE Pixelica::core Pixelica::skeleton Pixelica::ik)
```

Only the requested components load their export file; `Pixelica_skeleton_FOUND` / `Pixelica_ik_FOUND` report the
result, an unknown component is not found, and a missing optional component fails only when it was `REQUIRED`.
The starter template and its `sdk_installed_template` check use only the core. As in source-tree mode generally, the
options are plain cache variables for `add_subdirectory` users and stay `OFF` unless set.

## Optional module: input manager

Device snapshots -> bindings -> game actions, shared by the 2D and 3D profiles, is another **separate optional module**, off by default (details in Korean:
[`input_manager.md`](input_manager.md); verification record: `INPUT_MANAGER_AUDIT.md` (local-only record)). A default build/install contains none of its header,
library, symbols or export file.

| Option (default `OFF`) | Target | Install component | Export file |
|---|---|---|---|
| `PIXELICA_BUILD_INPUT_MANAGER` | `Pixelica::input_manager` | `input_manager` | `PixelicaInputManagerTargets.cmake` |

```sh
cmake -S . -B build -DPIXELICA_BUILD_INPUT_MANAGER=ON -DPIXELICA_BUILD_EXAMPLES=OFF -DPIXELICA_BUILD_TESTS=OFF
cmake --build build --config Release
cmake --install build --config Release --prefix /path/to/prefix --component common
cmake --install build --config Release --prefix /path/to/prefix --component input_manager
```

```cmake
find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS input_manager)   # needs only `common`; no renderer required
target_link_libraries(my_game PRIVATE Pixelica::input_manager)
```

- It depends on `Pixelica::common` only. The Pixelica2D package config accepts `input_manager` when requested by name and resolves the same `Pixelica::input_manager` target (see `cmake/PixelicaPackage.cmake`).
- **Not in the existing archives.** The Pixelica2D / Pixelica ZIP archives and `out/sdk-release` described above were **not rebuilt** for this module and do not contain it;
  `tools/package_sdk.py` explicitly keeps the optional modules off. It is available from a source-tree build, a direct install as above, or `add_subdirectory`.
- The SDL3 adapter (`Pixelica::input_sdl3`) exists only in source-tree builds with `PIXELICA_PLATFORM=SDL3`; it is not installed, like the SDL platform layer itself.

## Starter template (`sdk/template`)

A complete standalone project: copy it anywhere, point `CMAKE_PREFIX_PATH` at the install prefix
and build. It contains portable game logic (`src/game.c`: a spinning cube with per-face colors and a
textured checker floor drawn with `pixelica_draw_mesh_ex`, fixed 60 Hz update, logical buttons, no
allocation after init) and a headless driver (`src/main_headless.c`) with a finite `--frames N`
command that uses a fixed clock and a scripted button sequence and prints
`pixelica-template: frames=N presented=N fnv1a=XXXXXXXX`. The hash shows that the frames are
reproducible; it does not show what they look like.

```sh
cmake -S sdk/template -B build-game -DCMAKE_PREFIX_PATH=/path/to/pixelica-prefix -DCMAKE_BUILD_TYPE=Release
cmake --build build-game --config Release
ctest --test-dir build-game -C Release --output-on-failure
./build-game/bin/pixelica_game --frames 60        # Windows (Visual Studio): build-game\bin\Release\pixelica_game.exe
```

`-DPIXELICA_GAME_STRICT_WARNINGS=ON` compiles the game with strict warnings as errors (CTest does).

### What CTest checks (`sdk_installed_template`)

`cmake/PixelicaSdkCheck.cmake` installs the build into a temporary prefix, verifies that the installed
CMake files contain no source/build paths and no `pixelica_options`, copies `sdk/template` out of the
source tree, configures it with `Pixelica_DIR` forced to that prefix, builds it with strict warnings,
runs its own CTest, runs the executable twice for the same output, once with a different frame count
for a different hash, and checks that invalid `--frames` values fail. It needs the project built in
the configuration being tested (`ctest -C <config>`), and is not registered for sanitizer builds or
cross-compiles.

## Source-tree mode and the optional SDL3 window

To see a window, add the Pixelica checkout with `add_subdirectory`; `Pixelica::core` is an alias
of the same target, so the game's `CMakeLists.txt` is unchanged:

```sh
cmake -S sdk/template -B build-sdl -DPIXELICA_GAME_USE_SOURCE_TREE=ON -DPIXELICA_GAME_PIXELICA_SOURCE_DIR=/path/to/Pixelica \
      -DPIXELICA_GAME_WITH_SDL3=ON -DPIXELICA_FETCH_SDL3=ON
cmake --build build-sdl
./build-sdl/bin/pixelica_game_sdl3 [--frames N] [--hidden]
```

Source-tree mode forces `PIXELICA_BUILD_EXAMPLES`, `PIXELICA_BUILD_TESTS` and `PIXELICA_INSTALL` off and sets
`PIXELICA_PLATFORM` to `SDL3` or `HEADLESS`. SDL stays pinned to 3.4.18 (SHA256-verified fetch or an exact
installed version, see the README). In this mode the warning flags of `pixelica_options` are inherited by
targets linking `Pixelica::core`. `src/main_sdl3.c` builds and runs in this mode (strict warnings, hidden
60-frame run).

## Assets

The asset tool and formats are documented in `docs/asset_format.md` and `docs/art_style.md`
(owned by the asset pipeline). Typical flow, Python 3.8+ standard library only:

```sh
python share/Pixelica/tools/pixelica_pack/pixelica_pack.py import vox model.vox -o model.p3mesh --scale 0.0625 --pivot floor
python share/Pixelica/tools/pixelica_pack/pixelica_pack.py texture floor.ppm -o floor.p3tex
python share/Pixelica/tools/pixelica_pack/pixelica_pack.py bundle -o game.p3cart model=model.p3mesh floor=floor.p3tex
python share/Pixelica/tools/pixelica_pack/pixelica_pack.py inspect game.p3cart
```

In a source checkout the same tool is `tools/pixelica_pack/pixelica_pack.py`. The Pixel Room demo assets are
regenerated with `python tools/pixelica_pack/generate_demo_assets.py` (the output is byte-reproducible;
`--check` compares without writing; CTest `asset_reproducibility` generates twice into temporary
directories and compares with the included `assets/`).

## Supported and not supported

The optional online module is documented in [`online.md`](online.md). Build with `PIXELICA_BUILD_ONLINE=ON`, install
components `common` and `online`, then request `COMPONENTS online` and link `Pixelica::online` through either
Pixelica or an existing Pixelica2D SDK. It installs its own export, relay, HTTPS deployment configuration and
standalone starter. It works without either renderer. Linux consumers need libcurl development files and threads;
Windows uses WinHTTP. `tools/package_sdk.py` keeps online OFF in the base 2D/3D archives.
The separate Windows online add-on contains Debug/Release libraries under the same `Pixelica-0.1.0/` archive root:
extract the base SDK first and the add-on last into the same folder. Existing rendering libraries are retained;
common package files are updated to recognize the new component. See `ONLINE_AUDIT.md` (local-only record).

Supported by the SDK: Windows and Linux x64, headless or SDL3 platform, C11. Linux arm64 builds and its
tests run under QEMU. ESP32-P4 through ESP-IDF v6.0 compiles (see `docs/porting.md`) but has not run on a
board. Raspberry Pi (arm64 Linux with SDL3) has not been tested on a Pi. Not provided: audio, editor, ECS,
scene hierarchy, glTF import, GPU rendering, dynamic game loading, physics beyond swept AABB.

Content limits: textures are sampled nearest-neighbor; the VOX importer accepts a single flat model
(`VOX ` 150/200) and rejects scene graphs, multiple models, layers and material chunks.
