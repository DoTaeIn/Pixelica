# Porting Pixelica

> **No physical board has been used.** The ESP32-P4 port compiles with official ESP-IDF v6.0
> (`hello_triangle` and Pixel Room TINY, RISC-V GCC 15.2, ELF/bin produced) but has **never been flashed
> or run on hardware**. Raspberry Pi has not been tested either. Every board fact below is **N/A** until
> someone fills it in from a real board. A passing compile or host test does not verify LCD, GPIO, PSRAM
> placement, cache/DMA behavior or performance; the hardware bring-up gate remains open.

## The porting surface

The engine core (`src/`, `include/pixelica/`) never includes SDL or ESP-IDF headers. A port supplies
one `PIXELICA_PlatformOps` table (`include/pixelica/platform.h`):

| Callback | Contract |
|---|---|
| `time_us` | monotonic 64-bit microseconds |
| `read_buttons` | logical `PIXELICA_BTN_*` mask, called once per `pixelica_update` |
| `present_rgb565` | **synchronous**: when it returns, the engine may overwrite `pixels` |
| `log` | optional text sink |

plus a main loop (see `examples/common/game_runner.c` and `firmware/esp32p4/main/app_main.c`).
Game code is shared: no `#ifdef ESP_PLATFORM` in examples (the pixel_room and hello_* sources are the
same files for desktop and firmware).

## Raspberry Pi / arm64 Linux

Intended path: the SDL3 platform on a 64-bit Raspberry Pi OS, built natively (`cmake -S . -B build
-DPIXELICA_PLATFORM=SDL3 -DPIXELICA_FETCH_SDL3=ON`) or headless. CI offers an optional **arm64 cross-compile**
(`cmake/toolchains/aarch64-linux-gnu.cmake`, tests run under qemu-user):

```sh
sudo apt-get install gcc-aarch64-linux-gnu qemu-user ninja-build
cmake -S . -B build-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
      -DPIXELICA_PLATFORM=HEADLESS -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm64 && ctest --test-dir build-arm64 --output-on-failure
```

The ARM64 Release cross build and its 19 headless tests ran under QEMU 8.2.2 (aarch64-linux-gnu GCC 13.3), and
the TINY and STANDARD initial-state captures were pixel-identical to x64 (see
`benchmarks/cross-architecture-images.json` (로컬 검증 자료); evidence for
those scenes and builds, not a general cross-libm guarantee). This says nothing about a real Pi (SDL video
drivers, timing, performance). Pi hardware test: **not done**.

## ESP32-P4 (ESP-IDF v6.0)

### Pinned SDK and scope

- ESP-IDF release tag **v6.0**, target **esp32p4** only. `firmware/esp32p4/CMakeLists.txt` refuses
  other targets and other IDF versions (v6.0.x only warns; `-DPIXELICA_ALLOW_OTHER_IDF=ON` overrides, unsupported).
- Supported display transport: an **SPI or I80** `esp_lcd` panel with its panel IO, initialized by the board
  code. **RGB and MIPI-DSI panels are not supported** (continuous scan-out has different buffer rules and
  no completion event is assumed for them); adding them needs a real board.
- Compiled language mode: ESP-IDF v6.0 builds C with its native `gnu23`; the engine and examples are portable
  C11 and compile unchanged. No `-std` override is used.
- No LCD model, pin, clock, PSRAM size, resolution or board is chosen anywhere in this repository.

### Layout

```text
platforms/esp32p4/
  include/pixelica_esp32p4.h     public config and API (esp_lcd handles, GPIO table, memory)
  lcd_transport.h/.c        pure-C DMA lifetime contract (no ESP-IDF includes)
  display_esp_lcd.c         esp_lcd glue: draw_bitmap, done callback, semaphore, heap_caps staging
  input_gpio.c              GPIO validation and logical-button mapping
  platform_esp32p4.c        PAL (esp_timer, buttons, present), engine memory, lifecycle, stats
  host_test/                host-only test of lcd_transport.c with a mocked port
firmware/esp32p4/
  CMakeLists.txt            ESP-IDF project (esp32p4, v6.0 guards)
  sdkconfig.defaults        target + generic options only (no board data)
  components/pixelica/      ESP-IDF component wrapping src/**/*.c and platforms/esp32p4 by repo-relative path
  main/                     app_main.c, board_config.c (the unimplemented board hook), Kconfig.projbuild
```

The component directory is called `pixelica` on purpose: a directory named `esp32p4` would collide with
ESP-IDF's own chip component. Source paths are computed from the component's location (or the
`PIXELICA_ROOT` environment variable) and checked, so a wrong checkout fails at configure time with a
message.

### Build instructions

```sh
# One-time ESP-IDF v6.0 setup (needs network)
git clone -b v6.0 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32p4 && . ./export.sh      # Windows: install.ps1 esp32p4 / export.ps1

cd <Pixelica>/firmware/esp32p4
idf.py set-target esp32p4
idf.py menuconfig        # "Pixelica firmware": run mode, game, Pixel Room profile
idf.py build
```

- **Headless run mode (default)**: no LCD and no buttons. Renders `CONFIG_PIXELICA_FW_HEADLESS_FRAMES` frames
  with a fixed 16667 us step and logs `headless: presented=N last_frame_fnv1a=XXXXXXXX` plus memory and
  present counters. It compiles without any board data (observed: app sizes 195664 bytes for
  `hello_triangle`, 264096 bytes for Pixel Room TINY). Flashing and running it requires a real ESP32-P4 chip
  (`idf.py -p <port> flash monitor`) and has not been done. Whether its hash equals the desktop headless hash
  is **unverified** (different libm).
- **Board run mode**: calls `pixelica_board_configure()` in `main/board_config.c`, which currently returns
  `ESP_ERR_NOT_SUPPORTED` and logs what is missing. Implement it for a real board (checklist below).
- **Pixel Room**: select it in menuconfig. It needs `assets/compiled/pixel_room_assets.h`
  (`python tools/pixelica_pack/generate_demo_assets.py`, included in this workspace) and builds the same
  `examples/pixel_room/*.c` as the desktop with `PIXEL_ROOM_WIDTH/HEIGHT` for TINY 160x120 or STANDARD
  256x192. `hello_triangle` is fixed at 256x192.
- Enable PSRAM in menuconfig for your board if the engine block should live there. Without
  `CONFIG_SPIRAM` the firmware falls back to internal RAM for the engine block.
- The optional CI job `esp-idf-compile` tries the same build in Espressif's `espressif/idf:v6.0` image. The
  compile result above comes from a local official ESP-IDF v6.0 checkout, not from that Docker image or a
  GitHub Actions run.

### The LCD transport and buffer-lifetime contract

`esp_lcd_panel_draw_bitmap()` on SPI/I80 panels only **queues** an asynchronous DMA transfer. Its return
value is never treated as completion. The engine's pixel buffer is therefore **never given to the LCD
driver**. Instead (`lcd_transport.h`, enforced by `lcd_transport.c`):

1. At create time the PAL allocates one permanent, aligned, DMA-capable **staging** buffer in internal RAM
   (`heap_caps_aligned_alloc`, `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`). Its size is
   `staging_rows * width * 2` bytes rounded up to the alignment (default 128 B, a conservative cache-line
   guess to be confirmed on the board). `staging_rows = 0` stages the whole frame; a smaller value trades
   internal RAM for more transfers. Nothing is allocated per frame.
2. `present_rgb565` copies a band of rows (row stride honoured, optionally byte-swapped) into staging,
   calls `esp_lcd_panel_draw_bitmap` for that band, and **waits for the `on_color_trans_done` event**
   registered with `esp_lcd_panel_io_register_event_callbacks` (a binary semaphore given from the
   callback, `esp_lcd_panel_io_color_trans_done_cb_t(io, edata, user_ctx)`), then repeats for the next band.
   It returns `true` only when no transfer is outstanding. A failure may return `false`
   while staging is still owned by the outstanding transfer, as described below.
3. The transport is **pessimistic**: it is `POISONED` before every draw and returns to `IDLE` only after the
   matching completion is counted. Completion is tracked by counters (draws issued vs. done events), so a
   spurious semaphore wake-up is not completion and an extra or stray event is a detected protocol error.
4. A **timeout, a failing `draw_bitmap`, or an event/draw mismatch leaves the transport poisoned**: staging
   is neither rewritten nor freed, no new draw is submitted, and `present_rgb565` returns `false`
   (`pixelica_end_frame` then reports `PIXELICA_ERR_PLATFORM`). The next present may proceed only when the counted
   completions match the submitted draws, such as after a late completion following a timeout or a draw
   error that nevertheless queued a transfer. An extra completion (protocol error), or a draw error that
   queued nothing and will never complete, cannot recover merely by waiting: the board must quiesce the
   bus or reset it. `pixelica_esp32p4_display_recover()` waits for completion, or uses the board's optional
   `quiesce()` hook, which must return true only if the bus is provably idle and no further done event will
   arrive for the abandoned draws.
5. `pixelica_esp32p4_destroy()` frees staging only after the DMA is proven idle. Otherwise it returns
   `ESP_ERR_INVALID_STATE` and **deliberately keeps the staging buffer and the platform object allocated**
   (a leak is preferred over a DMA write into freed memory); the handle stays valid and destroy can be
   retried. The engine block is never visible to the DMA and is freed regardless.

**Assumption that must be verified on a real board (currently unverified):** the panel IO raises exactly one
`on_color_trans_done` per successful `esp_lcd_panel_draw_bitmap()` call, after the last byte of that call's
color data was read, and the callback runs where `xSemaphoreGiveFromISR` is allowed (it is placed in IRAM).
A driver that reports nothing shows up as repeated timeouts; one that reports more than once per draw
shows up as `PIXELICA_LCD_ERR_PROTOCOL`.

`platforms/esp32p4/host_test/test_lcd_transport.c` (CTest `esp32p4_lcd_transport`, built on every
host) runs `lcd_transport.c` against a mocked asynchronous DMA: pixels are read from staging only when the
mock "hardware" completes, and the test counts any rewrite of staging, overlapping draw, or free while a transfer
is pending (all must be zero). It covers byte swap, offsets, stride, banding, timeout, partial draw
failure, duplicate and stray events, late completion, recovery, double deinit and counter wrap-around. It
proves the ownership logic only; it uses no ESP-IDF and no hardware.

### Memory

- **Engine block**: `pixelica_memory_requirement(cfg)` bytes (color + depth + frame arena + guards) allocated once with
  `heap_caps_malloc` (`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` by default, overridable with
  `engine_heap_caps`) and passed to `pixelica_create_in_buffer`; `pixelica_esp32p4_destroy` frees it. The pure framebuffer
  arithmetic from the plan is 75 KiB at 160x120 and 192 KiB at 256x192 (color + depth), excluding arena, staging,
  textures, game state and stack.
- **Staging**: internal DMA RAM as above.
- `pixelica_esp32p4_get_stats()` reports block sizes, `heap_caps` free/minimum-free for internal RAM and PSRAM
  (values exist only at run time on a board) and present counters/times.

### Input and time

`PIXELICA_Esp32P4Config.buttons[7]` maps LEFT, RIGHT, UP, DOWN, A, START, QUIT to user-chosen GPIOs. `gpio_num = -1`
(`PIXELICA_ESP_GPIO_UNASSIGNED`) means unassigned; call `pixelica_esp32p4_config_defaults()` first because a zeroed
table would mean GPIO 0. All pins are validated before any hardware is touched: valid GPIO for the chip,
no duplicates, valid pull mode. Polarity (`active_low`) and the internal pull are per button. There is no
debouncing. Time is `esp_timer_get_time()`.

### Board integration checklist

Every row is **N/A** (not decided, not measured) in this repository. Fill it in when a board exists.

| Item | Status |
|---|---|
| Board / module and ESP32-P4 chip revision | N/A |
| CPU clock configuration | N/A |
| Flash size and mode | N/A |
| PSRAM present, mode, speed, size (`CONFIG_SPIRAM*`) | N/A |
| LCD controller model, interface (SPI or I80), bus width | N/A |
| LCD bus pins, clock frequency, command/data pin, reset, backlight | N/A |
| Panel width x height; must equal the game's render size (160x120 / 256x192), x/y offset | N/A |
| Pixel format and byte order (`swap_bytes`), rotation/mirror | N/A |
| `on_color_trans_done` verified: exactly one event per `draw_bitmap`, ISR-safe | N/A (unverified assumption) |
| Staging alignment (cache line) and `staging_rows` chosen | N/A |
| `quiesce()` hook implemented, or deliberately absent | N/A |
| Button GPIOs, polarity, pulls (LEFT/RIGHT/UP/DOWN/A/START/QUIT) | N/A |
| `pixelica_board_configure()` implemented in `firmware/esp32p4/main/board_config.c` | N/A |
| Engine block placement (PSRAM or internal) | N/A |
| 30-minute run without leak or memory-guard failure | N/A |

Metrics template (do not fill with guesses):

```text
board: N/A
cpu_clock: N/A          psram: N/A          lcd: N/A
profile/resolution: N/A scene: N/A
render_ms_p50/p95: N/A  present_ms_p50/p95: N/A  frame_fps_avg: N/A
heap_free_bytes: N/A    heap_min_free_bytes: N/A
```

### Known limitations and risks

- Everything that touches hardware is unverified: pin and peripheral setup, panel driver initialization, byte
  order, actual `on_color_trans_done` semantics, cache/DMA behavior, PSRAM placement and timing. Board
  differences may need changes in `board_config.c` and `sdkconfig`.
- One in-flight transfer at a time: simple and safe, not the fastest possible (no overlap of rendering and
  transfer; a double-buffered staging variant is future work after measurement).
- Pixel Room's own state is allocated with `malloc`; place it in PSRAM with `CONFIG_SPIRAM_USE_MALLOC` or a
  custom allocator if internal RAM is tight.
