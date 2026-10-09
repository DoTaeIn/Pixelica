# Pixelica2D starter game

A standalone project that depends on Pixelica2D only through its installed CMake package and public
headers (`pixelica/renderer2d.h`). It does not use a 3D renderer, a `PIXELICA_Context` or any 3D header, and it
links nothing but `Pixelica2D::core`. Copy this directory anywhere.

It draws a tilemap background, four spinning color-keyed gems (a 4-frame atlas), a player sprite and
a translucent score bar. The player collects gems with the arrow keys.

```text
CMakeLists.txt               find_package(Pixelica2D CONFIG REQUIRED) + Pixelica2D::core, CTest cases
cmake/check_deterministic.cmake   CTest helper: same input -> same frame hash
src/game.h/.c                portable game logic: init, fixed 60 Hz update, render
src/main_headless.c          driver: --frames N, --capture PATH, scripted buttons, one PAL present per frame
```

## Build against an installed Pixelica2D

`<prefix>` is the directory that contains `lib/cmake/Pixelica2D` and `lib/cmake/Pixelica` (the 2D SDK
archive extracted anywhere; both configs are relocatable and resolve each other beside themselves).

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=<prefix> -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/bin/pixelica2d_game --frames 60 --capture frame.ppm   # Windows (Visual Studio): build\bin\Release\pixelica2d_game.exe
```

`-DPIXELICA2D_GAME_STRICT_WARNINGS=ON` compiles the game with strict warnings as errors.

## Command line

```text
pixelica2d_game [--frames N] [--capture PATH]
```

* `--frames N` runs exactly N frames (1..1000000, default 60). Anything else exits with status 2.
* `--capture PATH` writes the last frame as a binary PPM (open it with any image viewer).
* On success it prints `pixelica2d-template: frames=N presented=N fnv1a=XXXXXXXX`.

The hash proves the run is deterministic, not what it looks like. Look at the capture for that.

## CTest cases

| Test | What it checks |
| --- | --- |
| `game_runs` | 30 frames render and present, summary line matches |
| `game_is_deterministic` | two identical runs print identical output; a 60-frame run differs from a 30-frame run |
| `game_captures_ppm` | `--capture` writes a file and the run still succeeds |
| `game_rejects_bad_frames`, `game_rejects_zero_frames`, `game_rejects_missing_capture_path` | invalid arguments exit non-zero |
| `game_bad_frames_message` | the error message names `--frames` |

## Porting to a device or a window

`game.c` reads logical buttons (`PIXELICA_BTN_*`), allocates nothing and contains no platform `#ifdef`.
Replace `main_headless.c` with your own driver: call `template2d_game_update` at a fixed 60 Hz, then
`template2d_game_render` once per frame, then hand `surface.pixels` to your display exactly once
(the PAL contract in `pixelica/platform.h`: `present_rgb565` is synchronous and must not keep the
pointer). This template deliberately ships no SDL front end; the source repository's
`examples/common2d` has an SDL3 driver for source-tree builds.
