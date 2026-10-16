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
Pixelica::common    src/math/{mat4,quat}.c, input/input.c, animation/{animation,anim_clip}.c,
                    assets/container.c, assets/texture_asset.c, src/game/* (lifecycle, storage, ...)
                    headers: status input platform math animation anim_clip image assets_common ...
                                                                              (no renderer, no context)
Pixelica::ui        modules/ui, always built/bundled; links common, no renderer/SDL/heap
Pixelica::core      the other src/ files (renderer, context, scene, mesh/cart)    links common PUBLIC  (alias renderer3d)
Pixelica::renderer2d  modules/renderer2d  (alias Pixelica2D::core)               links common, NO 3D code, NO 3D headers
Pixelica::hybrid    modules/hybrid        needs core AND renderer2d (built only when both engines are on)
skeleton, ik        modules/*             link common (mathematical, still default OFF)
input_manager       modules/input_manager links common only, default OFF, shared by 2D and 3D (no renderer, no SDL)
audio               modules/audio        links common only, default OFF, caller-owned mixer and borrowed PCM/WAV
audio_sdl3          platforms/sdl3        optional audio output, needs audio + SDL3; game-thread queue pump
pixelica_platform_sdl3   PAL only; links common, so it also serves 2D-only builds
```

- **Rule 1 changes for the new modules**: `modules/renderer2d` and `modules/hybrid` follow the same rule (no SDL/ESP-IDF includes). `renderer2d`
  includes no 3D header. `hybrid.c` is the single place that includes a private core header (`src/core/context_internal.h`), read-only, to get the
  frame state, the color/depth pointers and the extents of the context's allocation (for the disjointness check); nothing casts away `const` in a public API.
- **2D frame loop** (examples/common2d/main.c): `update` at a fixed 60 Hz, `render` into a caller-owned `PIXELICA2D_Surface`, exactly one PAL
  `present_rgb565` per frame. There is no `PIXELICA_Context`, no begin/end-frame protocol and no hidden draw queue in 2D.
- **2.5D frame loop**: `pixelica_begin_frame` -> meshes -> `pixelica2d_bind_3d` -> `pixelica25d_draw_billboard` / HUD -> `pixelica_end_frame` (the only present).
  The 2D view is a borrow of the 3D buffers and must not outlive the frame; its output struct must be disjoint from the whole 3D engine
  allocation (context, framebuffers, arena, coverage bitmap, guards) or the bind is rejected without writes. Statistics stay separate: 3D `PIXELICA_FrameStats` vs `PIXELICA2D_Stats`.
  A billboard has one depth for the whole sprite and mirrors by the sign of its appearance `scale.x/y`; mesh and billboard depths share the
  nominal 0..65534 quantization but can differ by about a unit at boundaries (float vs double), so exact coplanar equality is not guaranteed.
- Build profiles: `PIXELICA_BUILD_3D` (default ON) and `PIXELICA_BUILD_2D` (ON at top level, OFF as a subproject) select which pieces exist; both OFF
  builds common and UI. SDK archives (`tools/package_sdk.py`): Pixelica2D (common + UI + renderer2d) and Pixelica3D (core + hybrid, which includes common + UI); extracting
  both into one prefix enables `hybrid` when version, compiler, architecture and configuration ABI match. Skeleton and IK are independent, default OFF
  and not in either base archive. On ESP32-P4 the 2D path is CPU software rendering that is compiled only; no board run and no frame rate exists.
- Demos: `examples/common2d` (runner `main.c`, `demo2d.h` interface, `demo_art.h` generated pixel art) for `hello_2d` and `hello_isometric`;
  `hello_25d` uses the existing `PIXELICA_Game` runner plus the hybrid module.

## CMake targets

| Target | Contents | Depends on |
|---|---|---|
| `pixelica_common` (`Pixelica::common`) | 5 sources listed in `PIXELICA_COMMON_SOURCE_NAMES` | libm on Unix; build-only warning flags |
| `pixelica_ui` (`Pixelica::ui`), always built | `modules/ui/`: input state, layout, RGB565 widgets, font, PAL presenter | `pixelica_common` |
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
- Optional audio supports PCM WAV (8/16-bit mono/stereo), 16 voices, resampling, buses, pan and sample-timed fades.
  The portable mixer allocates nothing; the SDL output adapter owns device/file resources and queues audio on the game thread.
  Call its update each frame; long stalls may underrun and controls affect future mixed samples (queued output has latency).
  Compressed formats, disk streaming, spatial acoustics and board I2S drivers are not implemented. See [`audio.md`](audio.md).
- No editor, glTF, GPU renderer, or dynamic game loading.

## Verification

Verification results live in the audit records (`G0_AUDIT.md` (local-only record) for G0;
`REMAINING_AUDIT.md` (local-only record) for the rest) and are summarized in the README.
The images in `docs/verification/` are exports of the engine's framebuffer, not window screenshots.
The 2D / 2.5D extension is recorded in `DIMENSIONS_AUDIT.md` (local-only record); the statements above about earlier 3D work are unchanged.

### 선택 파티클

`modules/particles/particles.c`는 common만 의존하는 호출자 소유 고정 풀 시뮬레이션입니다.
`particles2d.c`는 renderer2d, `particles25d.c`는 hybrid 위의 출력 어댑터이며 각각 별도 타깃·컴포넌트입니다.
기본값 OFF이며 기본 엔진 헤더·아카이브로 편입하지 않습니다. 3D 빌보드는 기존의
깊이 버퍼와 RGB565 알파 합성을 재사용합니다. GPU나 추가 렌더링 파이프라인은 필요하지 않습니다.
설치 ZIP과 소스 ZIP은 `tools/package_particles.py`가 기본 SDK와 별도로 생성합니다.

## 선택 CPU 쉐이더

`modules/shaders`는 RGB565 표면을 처리하는 렌더러 독립 모듈입니다. `Pixelica::common`만 연결하며
기본 옵션 `PIXELICA_BUILD_SHADERS=OFF`에서는 헤더·라이브러리·설치 설정을 만들지 않습니다.
포인트 효과는 제자리 연산, 이웃 픽셀을 읽는 효과는 호출자가 제공한 프레임 크기의 스냅샷을 사용합니다.
사용자 콜백도 변경되지 않는 원본을 읽으므로 픽셀 순회 순서가 결과를 바꾸지 않습니다.

2D는 소유한 표면에 직접 적용합니다. 3D·하이브리드는 `PIXELICA_ShaderPresenter`가 PAL 출력 앞에서
프레임을 별도 출력 버퍼에 복사하여 처리하고 실제 플랫폼으로 전달합니다. 엔진 소유 색·깊이 버퍼와
렌더링 통계는 그대로이며, PAL의 시간·입력·로그 호출은 원래 userdata로 전달합니다.
이 모듈은 GPU 파이프라인이나 셰이더 언어 컴파일러가 아닙니다.

설치는 독립된 `PixelicaShaders` CMake 패키지와 `Pixelica::shaders` 타깃을 사용합니다.
기존 SDK 로더를 교체하지 않아 이전 파티클 추가 ZIP과 병합해도 충돌하지 않습니다.
상세 계약은 [shaders.md](shaders.md), 패키징은 `tools/package_shaders.py`를 참조하세요.

## 기본 UI

`modules/ui`는 항상 빌드하고 두 기본 SDK에 포함하는 별도 정적 라이브러리입니다.
위젯 배열·텍스트·컨텍스트·출력은 호출자 소유이며 힙 할당이나 SDL·2D·3D 의존성이 없습니다.
입력은 프레임당 한 번 갱신합니다. 버튼·체크박스·슬라이더의 포커스, 상승 에지, 포인터 캡처를 처리하고
장식 위젯은 입력을 통과시킵니다. UI 입력을 게임에 전달할지는 `wants_pointer/buttons`로 결정합니다.

2D는 장면 뒤에 RGB565 위젯을 직접 그립니다. 3D·2.5D는 PAL presenter가 엔진의 읽기 전용 프레임을
호출자 출력에 복사한 뒤 UI를 그려 실제 플랫폼으로 보냅니다. 깊이·카메라·렌더링 통계는 바꾸지 않습니다.
셰이더와 함께 쓸 때는 `엔진 → 셰이더 → UI → 플랫폼` 순서로 연결하면 글자가 후처리로 흐려지지 않습니다.
독립 `PixelicaUI` CMake 패키지로 찾기 때문에 이전 선택 모듈 ZIP이 기본 컴포넌트 로더를 교체해도
UI를 찾을 수 있습니다. 기본 글꼴은 ASCII이며 한글은 사용자 비트맵 콜백으로 공급합니다.
상세 사용법과 소유권·오류 규칙은 [ui.md](ui.md)에 있습니다.

## 수명주기·저장·화면·번역 공통 기능

`src/game`의 수명주기·저장·화면 전환·다국어는 `Pixelica::common`에 포함되어 렌더러 없이도 사용합니다.
수명주기(`app.c`)는 초기화 / pause·resume / suspend·resume / shutdown 상태 기계이며 시계도 전역 상태도 없습니다.
전환은 드라이버가 일으키고, 모듈은 합법성·콜백 순서·체크포인트 시점만 강제합니다.
체크포인트는 게임의 `save_checkpoint` 콜백이 만든 바이트이고 프로세스 스냅샷이 아닙니다.
저장소(`storage.c`)는 게임 ID와 슬롯을 봉투에 묶어 플랫폼 블롭 저장소(`PIXELICA_StorageOps`)에 넘기므로
PC 파일시스템 어댑터(`storage_fs.c`)든 임베디드 NVS든 같은 버전·CRC 규칙을 공유합니다.
`storage_fs.c`는 `save.c`와 원자적 임시 파일 쓰기 코드(`src/game/save_internal.h`)를 공유합니다.
저장은 게임이 직렬화한 바이트를 버전·길이·CRC가 있는 컨테이너로 감싸며, PC 파일 도우미는 임시 파일 작성 후 교체합니다.
임베디드 저장 장치는 컨테이너 인코딩·디코딩에 게임의 NVS·SD 입출력을 연결합니다.
화면 관리자는 기존 3D `scene`과 별개로 게임 진행 상태를 관리하며, 콜백 중 요청한 전환을 안전한 경계에서 처리합니다.
번역표와 문자열은 호출자가 소유합니다. 언어를 바꾸면 UI 위젯의 문자열도 갱신합니다.
폰트 도구는 게임에서 사용하는 유니코드 글자만 BDF에서 추출해 UI 콜백으로 변환합니다.
자세한 계약은 [lifecycle.md](lifecycle.md), [storage.md](storage.md), [save.md](save.md),
[screens.md](screens.md), [i18n.md](i18n.md)에 있습니다.
