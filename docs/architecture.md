# Pixelica architecture

Structure of the repository as implemented. The design target is in `Pixelica_Plan.md`; where this
file and the public headers in `include/pixelica/` differ, the headers are authoritative for API
behaviour. Historical G0-only notes are preserved in `G0_AUDIT.md` (local-only record). Verification
results are in `REMAINING_AUDIT.md` (local-only record) and summarized in the README.

## Layers

```text
examples/hello_*, pixel_room     game callbacks: init / update / render / shutdown
sdk/template                     standalone starter game (installed core or source-tree SDL3)
        |  (public pixelica API only)
examples/common + platform main  main(), argument parsing, frame loop (pixelica_game_run)
        v
include/pixelica + src/               pixelica_core: renderer, context, scene/AABB, mesh/cart
        |                        links pixelica_common: math, input, animation, texture assets
        |  (optional, default OFF) modules/skeleton, modules/ik: separate libraries that
        |  link pixelica_common; the core never includes or links them (docs/rigging.md)
        |  (optional, default OFF) modules/input_manager: snapshots -> bindings -> actions,
        |  links pixelica_common only; no renderer includes it (docs/input_manager.md)
        |  PIXELICA_PlatformOps
        v
platforms/sdl3                   pixelica_platform_sdl3: clock, keyboard, pixel display (Windows/Linux)
platforms/esp32p4                ESP-IDF platform: esp_timer, GPIO buttons, esp_lcd via a
                                 DMA-lifetime-safe transport (ESP-IDF v6.0; compiles, not run on a board)
```

Rules:

1. `src/` includes public headers, private core headers and standard C headers. It never includes SDL or ESP-IDF.
2. SDL3 dependencies stay in `platforms/sdl3/` and the SDK's `main_sdl3.c` driver; ESP-IDF dependencies stay in
   `platforms/esp32p4/` and `firmware/esp32p4/`. The pure-C `lcd_transport.c` has no ESP-IDF includes.
3. Examples and the SDK template call only the public pixelica API (plus `examples/common/pixelica_game.h` for
   examples). No `#ifdef` on the platform inside game code.
4. Game code reads logical buttons (`PIXELICA_BTN_*`), never keys or pins.
5. The asset tool (`tools/pixelica_pack`, Python) runs on the desktop and is not part of firmware.

## 2D / 2.5D split

The tree is split into independently usable pieces (see `2D_25D_PLAN.md` (local-only record) for the plan and dependency graph, [`2d.md`](2d.md) for
usage and `DIMENSIONS_AUDIT.md` (local-only record) for the verification record):

```text
Pixelica::common    src/math/mat4.c, input/input.c, animation/animation.c, assets/container.c, assets/texture_asset.c
                    headers: status input platform math animation image assets_common      (no renderer, no context)
Pixelica::core      the other src/ files (renderer, context, scene, mesh/cart)    links common PUBLIC  (alias renderer3d)
Pixelica::renderer2d  modules/renderer2d  (alias Pixelica2D::core)               links common, NO 3D code, NO 3D headers
Pixelica::hybrid    modules/hybrid        needs core AND renderer2d (built only when both engines are on)
skeleton, ik        modules/*             link common (mathematical, still default OFF)
input_manager       modules/input_manager links common only, default OFF, shared by 2D and 3D (no renderer, no SDL)
pixelica_platform_sdl3   PAL only; links common, so it also serves 2D-only builds
```

- **Rule 1 changes for the new modules**: `modules/renderer2d` and `modules/hybrid` follow the same rule (no SDL/ESP-IDF includes). `renderer2d`
  includes no 3D header. `hybrid.c` is the single place that includes a private core header (`src/core/context_internal.h`), read-only, to get the
  frame state, the color/depth pointers and the extents of the context's allocation (for the disjointness check); nothing casts away `const` in a public API.
- **2D frame loop** (examples/common2d/main.c): `update` at a fixed 60 Hz, `render` into a caller-owned `PIXELICA2D_Surface`, exactly one PAL
  `present_rgb565` per frame. There is no `PIXELICA_Context`, no begin/end-frame protocol and no hidden draw queue in 2D.
- **2.5D frame loop**: `pixelica_begin_frame` -> meshes -> `pixelica2d_bind_3d` -> `pixelica25d_draw_billboard` / HUD -> `pixelica_end_frame` (the only present).
  The 2D view is a borrow of the 3D buffers and must not outlive the frame; its output struct must be disjoint from the whole 3D engine
  allocation (context, framebuffers, arena, guards) or the bind is rejected without writes. Statistics stay separate: 3D `PIXELICA_FrameStats` vs `PIXELICA2D_Stats`.
  A billboard has one depth for the whole sprite and mirrors by the sign of its appearance `scale.x/y`; mesh and billboard depths share the
  nominal 0..65534 quantization but can differ by about a unit at boundaries (float vs double), so exact coplanar equality is not guaranteed.
- Build profiles: `PIXELICA_BUILD_3D` (default ON) and `PIXELICA_BUILD_2D` (ON at top level, OFF as a subproject) select which pieces exist; both OFF
  builds only common. SDK archives (`tools/package_sdk.py`): Pixelica2D (common + renderer2d) and Pixelica (core + hybrid, which includes common); extracting
  both into one prefix enables `hybrid` when version, compiler, architecture and configuration ABI match. Skeleton and IK are independent, default OFF
  and not in either base archive. On ESP32-P4 the 2D path is CPU software rendering that is compiled only; no board run and no frame rate exists.
- Demos: `examples/common2d` (runner `main.c`, `demo2d.h` interface, `demo_art.h` generated pixel art) for `hello_2d` and `hello_isometric`;
  `hello_25d` uses the existing `PIXELICA_Game` runner plus the hybrid module.

## CMake targets

| Target | Contents | Depends on |
|---|---|---|
| `pixelica_common` (`Pixelica::common`) | 5 sources listed in `PIXELICA_COMMON_SOURCE_NAMES` | libm on Unix; build-only warning flags |
| `pixelica_core` (`Pixelica::core`, `Pixelica::renderer3d`) | 7 sources listed in `PIXELICA_RENDERER3D_SOURCE_NAMES` | `pixelica_common` |
| `pixelica_renderer2d` (`Pixelica::renderer2d`, installed `Pixelica2D::core`) | `modules/renderer2d/renderer2d.c` | `pixelica_common` |
| `pixelica_hybrid` (`Pixelica::hybrid`) | `modules/hybrid/hybrid.c` | `pixelica_core`, `pixelica_renderer2d` |
| `pixelica_platform_sdl3` | `platform_sdl3.c`, `letterbox.c` | `pixelica_common`, SDL3 |
| `pixelica_game_runner` | `examples/common/game_runner.c` | `pixelica_core` |
| `pixelica_example_driver` (object) | `main_sdl3.c` (SDL3) or `main_headless.c` (HEADLESS) | runner, platform |
| `pixel_room_core` | `pixel_room.c`, `pixel_room_world.c`; portable game logic | `pixelica_core` |
| `pixel_room_embedded` | `room_embedded.c` and generated asset arrays | `pixel_room_core` |
| `hello_triangle`, `hello_cube`, `hello_animation`, `pixel_room`, `pixel_room_tiny`, `pixel_room_capture` | examples and the capture/benchmark driver | driver / room libraries |
| `pixelica_skeleton` (`Pixelica::skeleton`), only with `PIXELICA_BUILD_SKELETON=ON` | `modules/skeleton/skeleton.c` | `pixelica_common` |
| `pixelica_ik` (`Pixelica::ik`), only with `PIXELICA_BUILD_IK=ON` | `modules/ik/ik.c` | `pixelica_common` (not on `pixelica_skeleton`) |
| `pixelica_input_manager` (`Pixelica::input_manager`), only with `PIXELICA_BUILD_INPUT_MANAGER=ON` | `modules/input_manager/input_manager.c` | `pixelica_common` (no renderer, no SDL) |
| `pixelica_online` (`Pixelica::online`), only with `PIXELICA_BUILD_ONLINE=ON` | `modules/online/` | `pixelica_common`; Windows WinHTTP / Linux libcurl + threads; no renderer |
| `pixelica_input_sdl3` (`Pixelica::input_sdl3`), only with `PIXELICA_BUILD_INPUT_MANAGER=ON` and `PIXELICA_PLATFORM=SDL3`; build-only, never installed | `platforms/sdl3/input_sdl3.c` | `pixelica_input_manager`, SDL3 |
| `hello_rig`, only when both optional modules are ON | skinned-ribbon + two-bone IK demo | driver, `pixelica_skeleton`, `pixelica_ik` |
| `test_*` | one executable per enabled `tests/test_*.c` | common, 2D, 3D or the tested module, according to the profile |
| `test_esp32p4_lcd_transport` | host test of `lcd_transport.c` with a mocked port | none |

`PIXELICA_PLATFORM=HEADLESS` skips `platforms/sdl3` entirely, so SDL is not searched for or required.
`cmake --install --component core` exports the complete 3D SDK, including common (see `docs/sdk.md`). Other components are
`common`, `renderer2d`, `hybrid`, `skeleton`, `ik` and `input_manager`, each with a separate export (`PixelicaInputManagerTargets.cmake` for the optional input
manager, default OFF, [`input_manager.md`](input_manager.md); the SDL3 input adapter is a build-tree library and is not installed). The optional rig modules live under `modules/`,
are added only when their option is ON, never enter the core source list or `PixelicaTargets` export set, and install as `skeleton` and
`ik` with their own export files (`PixelicaSkeletonTargets.cmake`, `PixelicaIKTargets.cmake`); `find_package(Pixelica
COMPONENTS ...)` loads them on request (`docs/rigging.md`). The root file registers
the tests *before* `add_subdirectory(examples)` so that `examples/CMakeLists.txt` can link
`test_pixel_room`; with `PIXELICA_BUILD_EXAMPLES=OFF` that one test is omitted because it needs example code.

## Frame loop

`pixelica_game_run` owns the order of calls; the driver owns the platform:

```text
pixelica_create -> game.init
loop:
    pump platform events        (SDL3: window close, Escape)
    elapsed = platform clock delta   (with --frames: fixed 16667 us)
    pixelica_update(ctx, elapsed, game.update, state)   -- 60 Hz fixed steps, max 4 per call
    game.render: pixelica_begin_frame, pixelica_set_view_projection, pixelica_draw_mesh*/pixelica_scene_render, pixelica_end_frame
game.shutdown -> pixelica_destroy
```

`pixelica_end_frame` presents exactly once through the PAL, synchronously. Rendering speed does not change
simulation speed: extra elapsed time beyond four steps per call is dropped (see `pixelica/pixelica.h`).

## Memory

`pixelica_create` makes the only heap allocation (context, RGB565 color buffer, `uint16` depth buffer, frame
arena); `pixelica_create_in_buffer` uses caller memory instead (the ESP32-P4 platform passes a `heap_caps`
block). Nothing is allocated per frame. Assets are decoded into caller storage; a cart is a zero-copy
view. The SDL3 platform allocates its state, and in the fallback case one 32-bit conversion buffer, at
creation only. The ESP32-P4 platform allocates a permanent DMA staging buffer once.

## Coordinates, depth and rasterization

Fixed by `include/pixelica/math.h` and `include/pixelica/renderer.h`; summarized here:

- Right-handed, +Y up, camera looks down -Z, radians.
- Column-major `float[16]` matrices acting on column vectors; `clip = P * V * M * p`.
- NDC depth in [-1, 1], stored as `uint16` with 0 at the near plane; cleared to `UINT16_MAX`;
  smaller is nearer; the test is strictly less.
- Counter-clockwise on screen is front-facing; back faces are culled by default.
- Clipping happens in homogeneous space (w and the six frustum planes) before the perspective divide;
  texture coordinates are clipped linearly with the position and interpolated perspective-correctly.
- Top-left fill rule on integer edge functions with fixed-point screen coordinates.

## Presentation contracts

- **SDL3**: `present_rgb565` copies the engine buffer into an SDL streaming texture before returning, so the
  engine can reuse its buffer immediately. The texture is RGB565 (fallback ARGB8888 with a persistent
  conversion buffer), drawn nearest-neighbor at the largest integer scale that fits, centered with black
  bars (`letterbox.c`). `--hidden` selects SDL's dummy video driver and the software renderer, so
  automated runs need no display.
- **ESP32-P4 SPI/I80 LCD**: the engine buffer is copied into PAL-owned staging and only staging is queued;
  `present_rgb565` returns after the panel IO's color-transfer-done event. Timeout or failure poisons the
  transport so staging is never reused or freed while a transfer may be pending. RGB and MIPI-DSI
  panels are not supported. Details and the hardware assumption that still needs a board:
  [`porting.md`](porting.md).

## Known limits

- Unlit meshes (flat color, per-triangle color or nearest-neighbor textures); no texture filtering or
  mipmaps, no lighting, no scene hierarchy, no ECS.
- VOX import takes a single flat model; scene graphs, multiple models, layers and material chunks are rejected.
- Keyboard input (SDL3) and GPIO buttons (ESP32-P4, never exercised on a board); no gamepad in the default PAL. The optional input manager
  ([`input_manager.md`](input_manager.md), default OFF) adds actions and a build-only SDL3 gamepad adapter without changing the default PAL or example key paths;
  no board driver exists and nothing was verified on physical input hardware.
- The engine renders at its configured internal resolution; SDL scales that buffer into the window.
  HiDPI scaling is left to the OS.
- ESP32-P4 and Raspberry Pi: the ESP32-P4 firmware compiles with ESP-IDF v6.0 but neither target has run on
  hardware, so there is no hardware performance figure. Desktop CPU render timings are in the README.
- No audio, editor, glTF, GPU renderer, or dynamic game loading.

## Verification

Verification results live in the audit records (`G0_AUDIT.md` (local-only record) for G0;
`REMAINING_AUDIT.md` (local-only record) for the rest) and are summarized in the README.
The images in `docs/verification/` are exports of the engine's framebuffer, not window screenshots.
The 2D / 2.5D extension is recorded in `DIMENSIONS_AUDIT.md` (local-only record); the statements above about earlier 3D work are unchanged.
