# Pixelica starter game

A standalone project that depends on Pixelica only through its installed CMake package and public
headers (`pixelica/pixelica.h`). Copy this directory anywhere. It renders a spinning cube (flat color per
face) above a checker-textured floor (`pixelica_draw_mesh_ex` with a procedural 8x8 texture) and is the
template that `docs/sdk.md` describes.

```text
CMakeLists.txt       find_package(Pixelica CONFIG REQUIRED) + Pixelica::core
src/game.h/.c        portable game logic: config, init, update (fixed 60 Hz), render
src/main_headless.c  driver: --frames N, scripted buttons, prints "frames=N presented=N fnv1a=XXXXXXXX"
src/main_sdl3.c      optional SDL3 window driver (source-tree mode only)
```

## Build against an installed Pixelica

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/pixelica-prefix -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/bin/pixelica_game --frames 60        # Windows (Visual Studio): build\bin\Release\pixelica_game.exe
```

`-DPIXELICA_GAME_STRICT_WARNINGS=ON` compiles the game with strict warnings as errors.

The headless run shows that the engine renders and presents frames deterministically; it does not
show what the frame looks like. To see it, use the SDL3 path below. Both paths are exercised by the
repository's tests (copied template against an installed package, and source-tree mode with SDL3).

## Build against a Pixelica source checkout (optional SDL3 window)

```sh
cmake -S . -B build-sdl -DPIXELICA_GAME_USE_SOURCE_TREE=ON -DPIXELICA_GAME_PIXELICA_SOURCE_DIR=/path/to/Pixelica \
      -DPIXELICA_GAME_WITH_SDL3=ON -DPIXELICA_FETCH_SDL3=ON
cmake --build build-sdl
./build-sdl/bin/pixelica_game_sdl3
```

This adds Pixelica with `add_subdirectory`, so `Pixelica::core` is the same target name as in the
installed package. SDL 3.4.18 is fetched by Pixelica's own pinned, SHA256-verified rule.

## Controls (SDL3 front end)

Arrows = rotate, Space = A (pause spin), Enter = START (reset), Escape = quit.

## Keep portable

Game code reads logical buttons (`PIXELICA_BTN_*`), allocates nothing per frame and contains no
platform `#ifdef`. A device build supplies its own `main`/driver and platform ops instead of
`main_headless.c`; `game.c` stays unchanged.
