# Examples use only the public API (include/pixelica and the module headers).
#
#   3D examples (PIXELICA_BUILD_3D)  hello_triangle, hello_cube, hello_animation,
#                               hello_rig (both rig modules), Pixel Room. Their
#                               platform lifecycle, main() and frame loop live in
#                               a driver library chosen by PIXELICA_PLATFORM.
#   2D examples (PIXELICA_BUILD_2D)  hello_2d, hello_isometric, driven by the small
#                               examples/common2d runner (no 3D renderer).
#   hybrid example (both)       hello_25d, on the same PIXELICA_Game runner as the 3D
#                               examples, using pixelica/hybrid.h.
#
# Adds an example whose sources are every .c file of examples/<name>/.
#   DRIVER  driver library providing main() (linked directly: an OBJECT library)
#   LIBS    further libraries
function(pixelica_add_guarded_example name)
  cmake_parse_arguments(ARG "" "" "DRIVER;LIBS" ${ARGN})
  file(GLOB example_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${name}/*.c")
  list(SORT example_sources)
  if(NOT example_sources)
    message(FATAL_ERROR "Pixelica: enabled example ${name} has no source in examples/${name}/")
  endif()
  add_executable(${name} ${example_sources})
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${name}")
  target_link_libraries(${name} PRIVATE ${ARG_DRIVER} ${ARG_LIBS} pixelica_options)
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
  if(BUILD_TESTING)
    add_test(NAME smoke_${name} COMMAND ${name} --frames 30 --hidden)
    set_tests_properties(smoke_${name} PROPERTIES LABELS "smoke" TIMEOUT 60)
  endif()
endfunction()

add_executable(hello_ui hello_ui/main.c)
target_link_libraries(hello_ui PRIVATE pixelica_ui pixelica_options)
if(PIXELICA_PLATFORM STREQUAL "SDL3")
  if(NOT TARGET SDL3::SDL3)
    find_package(SDL3 3.4.18 CONFIG REQUIRED)
  endif()
  target_compile_definitions(hello_ui PRIVATE HELLO_UI_SDL=1)
  target_link_libraries(hello_ui PRIVATE SDL3::SDL3)
  if(WIN32)
    add_custom_command(TARGET hello_ui POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E
        $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:hello_ui>>,copy_if_different,true>
        $<TARGET_RUNTIME_DLLS:hello_ui> $<TARGET_FILE_DIR:hello_ui>
      COMMAND_EXPAND_LISTS)
  endif()
endif()
set_target_properties(hello_ui PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
if(BUILD_TESTING)
  add_test(NAME smoke_hello_ui COMMAND hello_ui --smoke)
  set_tests_properties(smoke_hello_ui PROPERTIES LABELS "ui;smoke" TIMEOUT 30)
  if(PIXELICA_PLATFORM STREQUAL "SDL3")
    add_test(NAME smoke_hello_ui_sdl COMMAND hello_ui --hidden --frames 3)
    set_tests_properties(smoke_hello_ui_sdl PROPERTIES LABELS "ui;sdl3;smoke" TIMEOUT 30)
  endif()
endif()

# Lifecycle + save storage: common library only, so it exists in every profile
# (2D, 3D or neither) and needs no renderer, window or platform table. The
# cross-process check below is the integration test for pixelica/app.h and
# pixelica/storage.h: it runs this program several times against one save
# directory and verifies what each fresh process restores.
add_executable(hello_lifecycle hello_lifecycle/main.c)
target_link_libraries(hello_lifecycle PRIVATE pixelica_common pixelica_options)
set_target_properties(hello_lifecycle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
if(BUILD_TESTING)
  add_test(NAME smoke_hello_lifecycle
    COMMAND hello_lifecycle --dir "${CMAKE_CURRENT_BINARY_DIR}/hello-lifecycle-smoke"
            --steps 4 --exit shutdown --fresh)
  set_tests_properties(smoke_hello_lifecycle PROPERTIES LABELS "smoke" TIMEOUT 30)
  add_test(NAME lifecycle_checkpoint_restart
    COMMAND "${CMAKE_COMMAND}"
      "-DPIXELICA_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DPIXELICA_BINARY_DIR=${PROJECT_BINARY_DIR}"
      "-DPIXELICA_WORK_DIR=${PROJECT_BINARY_DIR}/lifecycle-check"
      "-DPIXELICA_LIFECYCLE_EXE=$<TARGET_FILE:hello_lifecycle>"
      -P "${PROJECT_SOURCE_DIR}/cmake/PixelicaLifecycleCheck.cmake")
  set_tests_properties(lifecycle_checkpoint_restart PROPERTIES LABELS "integration" TIMEOUT 300)
endif()

if(TARGET pixelica_shaders)
  add_executable(hello_shaders hello_shaders/main.c)
  target_link_libraries(hello_shaders PRIVATE pixelica_shaders pixelica_options)
  set_target_properties(hello_shaders PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
  if(BUILD_TESTING)
    add_test(NAME smoke_hello_shaders COMMAND hello_shaders)
    set_tests_properties(smoke_hello_shaders PROPERTIES LABELS "shaders;smoke" TIMEOUT 30)
  endif()
endif()

# hello_water: the raft demo of the optional water module. It owns its own
# context and fixed-step loop (no platform, no window) so the game drives the
# physics, and it is deliberately standalone - the same source builds inside
# the portable source add-on (sdk/water-addon).
if(TARGET pixelica_water3d)
  add_executable(hello_water hello_water/hello_water.c)
  target_link_libraries(hello_water PRIVATE pixelica_water3d pixelica_options)
  if(MSVC)
    # Only this translation unit uses plain fopen (C4996), as in the capture tool.
    target_compile_definitions(hello_water PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  set_target_properties(hello_water PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
  if(BUILD_TESTING)
    # The program checks itself: the calm raft settles to the equilibrium
    # draft and stays level, the waves tilt it, it never leaves the water or
    # sinks past its probes, the drawn surface and the sampled surface agree
    # at every corner, the three captures are distinct frames, and a
    # physics-only replay lands on the identical state. ok=1 means all of it.
    add_test(NAME smoke_hello_water COMMAND hello_water --frames 420)
    set_tests_properties(smoke_hello_water PROPERTIES
      LABELS "water;smoke" TIMEOUT 120
      PASS_REGULAR_EXPRESSION "hello_water: ok=1")
    # The same run a million seconds in: a float clock would have frozen the
    # sea solid here, so this is the regression test for double time.
    add_test(NAME smoke_hello_water_long_session
      COMMAND hello_water --frames 300 --time-offset 1e6)
    set_tests_properties(smoke_hello_water_long_session PROPERTIES
      LABELS "water;smoke" TIMEOUT 120
      PASS_REGULAR_EXPRESSION "hello_water: ok=1")
    # Captures into a fresh directory under the build tree (never the source
    # tree): proves --out-dir really writes the three framebuffer PPMs.
    add_test(NAME hello_water_captures COMMAND "${CMAKE_COMMAND}"
      "-DPIXELICA_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DPIXELICA_BINARY_DIR=${PROJECT_BINARY_DIR}"
      "-DPIXELICA_WORK_DIR=${PROJECT_BINARY_DIR}/water-captures"
      "-DPIXELICA_WATER_EXE=$<TARGET_FILE:hello_water>"
      -P "${PROJECT_SOURCE_DIR}/cmake/PixelicaWaterCaptureCheck.cmake")
    set_tests_properties(hello_water_captures PROPERTIES LABELS "water;integration" TIMEOUT 180)
  endif()
endif()

if(TARGET pixelica_particles2d)
  add_executable(hello_particles hello_particles/main.c)
  target_link_libraries(hello_particles PRIVATE pixelica_particles2d pixelica_options)
  set_target_properties(hello_particles PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
  if(BUILD_TESTING)
    add_test(NAME smoke_hello_particles COMMAND hello_particles --ticks 90)
    set_tests_properties(smoke_hello_particles PROPERTIES LABELS "particles;smoke" TIMEOUT 30)
  endif()
endif()

# Audio has no renderer dependency: the same example works in every profile.
if(PIXELICA_BUILD_AUDIO)
  add_executable(hello_audio hello_audio/main.c)
  target_link_libraries(hello_audio PRIVATE pixelica_audio pixelica_options)
  if(TARGET pixelica_audio_sdl3)
    target_compile_definitions(hello_audio PRIVATE HELLO_AUDIO_SDL3=1)
    target_link_libraries(hello_audio PRIVATE pixelica_audio_sdl3)
  endif()
  set_target_properties(hello_audio PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
  if(WIN32 AND TARGET pixelica_audio_sdl3)
    add_custom_command(TARGET hello_audio POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E
        $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:hello_audio>>,copy_if_different,true>
        $<TARGET_RUNTIME_DLLS:hello_audio> $<TARGET_FILE_DIR:hello_audio>
      COMMAND_EXPAND_LISTS)
  endif()
  if(BUILD_TESTING)
    add_test(NAME smoke_hello_audio COMMAND hello_audio --smoke)
    set_tests_properties(smoke_hello_audio PROPERTIES LABELS "audio;smoke" TIMEOUT 15
      ENVIRONMENT "SDL_AUDIO_DRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_VIDEODRIVER=dummy")
  endif()
endif()

# Same online module and game simulation in every profile. The optional 2D
# renderer supplies the arena; without it the demo is a console bot/client.
if(PIXELICA_BUILD_ONLINE)
  add_executable(hello_online hello_online/main.c hello_online/online_game.c hello_online/online_session.c)
  target_link_libraries(hello_online PRIVATE pixelica_online pixelica_options)
  if(PIXELICA_BUILD_2D)
    target_sources(hello_online PRIVATE hello_online/online_view.c)
    target_compile_definitions(hello_online PRIVATE HELLO_ONLINE_WITH_RENDERER2D=1)
    target_include_directories(hello_online PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common2d")
    target_link_libraries(hello_online PRIVATE pixelica_renderer2d)
    if(PIXELICA_PLATFORM STREQUAL "SDL3")
      target_compile_definitions(hello_online PRIVATE HELLO_ONLINE_SDL=1)
      target_link_libraries(hello_online PRIVATE pixelica_platform_sdl3)
    endif()
  endif()
  set_target_properties(hello_online PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
  add_executable(hello_online_headless hello_online/main.c hello_online/online_game.c hello_online/online_session.c)
  target_link_libraries(hello_online_headless PRIVATE pixelica_online pixelica_options)
  set_target_properties(hello_online_headless PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
  if(BUILD_TESTING)
    add_executable(hello_online_game_test hello_online/online_game_test.c hello_online/online_game.c)
    target_link_libraries(hello_online_game_test PRIVATE pixelica_common pixelica_options)
    add_test(NAME online_game_logic COMMAND hello_online_game_test)
    set_tests_properties(online_game_logic PROPERTIES LABELS "online;unit")
  endif()
endif()

# ---------------------------------------------------------------------------
# 3D examples
# ---------------------------------------------------------------------------
if(PIXELICA_BUILD_3D)
  add_library(pixelica_game_runner STATIC common/game_runner.c)
  target_link_libraries(pixelica_game_runner PUBLIC pixelica_core PRIVATE pixelica_options)

  # OBJECT library so main() is always linked into the executable.
  if(PIXELICA_PLATFORM STREQUAL "SDL3")
    add_library(pixelica_example_driver OBJECT
      "${PROJECT_SOURCE_DIR}/platforms/sdl3/main_sdl3.c")
    target_link_libraries(pixelica_example_driver
      PUBLIC pixelica_game_runner pixelica_platform_sdl3
      PRIVATE pixelica_options)
  else()
    add_library(pixelica_example_driver OBJECT common/main_headless.c)
    target_link_libraries(pixelica_example_driver
      PUBLIC pixelica_game_runner
      PRIVATE pixelica_options)
  endif()

  function(pixelica_add_example name)
    add_executable(${name} ${name}/${name}.c)
    target_link_libraries(${name} PRIVATE pixelica_example_driver pixelica_options)
    # Put executables at the top of the build tree (<build>/hello_cube, or
    # <build>/Debug/hello_cube.exe for multi-config generators, which append the
    # configuration name), as documented in the README.
    set_target_properties(${name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    if(BUILD_TESTING)
      # Smoke run: exits 0 after N frames. Under SDL3 this uses the dummy video
      # driver and software renderer, so it proves the program runs, not how it looks.
      add_test(NAME smoke_${name} COMMAND ${name} --frames 30 --hidden)
      set_tests_properties(smoke_${name} PROPERTIES LABELS "smoke" TIMEOUT 60)
    endif()
  endfunction()

  pixelica_add_example(hello_triangle)
  pixelica_add_example(hello_cube)
  pixelica_add_example(hello_animation)
  # Lighting + additive blending + animation clips in one program. It needs
  # no optional module: clips live in pixelica_common and lighting/additive
  # blending in the 3D core.
  pixelica_add_example(hello_lighting)
  if(BUILD_TESTING)
    # The 30-frame smoke run added above never reaches the clip's second
    # event, its loop point or the crossfade. This longer headless run does,
    # so the three features are exercised end to end, not just compiled.
    #
    # The expected counts follow from the clip timings: the walk clip
    # (120 ticks, events on 0 and 60) runs for ticks 1..179 and reports
    # LEFT, RIGHT, LOOP, LEFT; the crossfade at tick 180 restarts on the
    # run clip (48 ticks, events on 0 and 24), which then reports LEFT,
    # RIGHT, LOOP, LEFT, RIGHT over its 81 ticks. Totals: 4 left steps,
    # 3 right steps, 2 loops, and a fade that completed (weight 1).
    add_test(NAME smoke_hello_lighting_long
      COMMAND hello_lighting --frames 260 --hidden)
    set_tests_properties(smoke_hello_lighting_long PROPERTIES
      LABELS "smoke" TIMEOUT 60
      PASS_REGULAR_EXPRESSION
        "hello_lighting: ticks=260 clip=run fade_weight=1\\.00 steps=4/3 loops=2 crossfades=1 skipped=0 lit_pixels=[1-9][0-9]+ glow_pixels=[1-9][0-9]+")
  endif()

  # hello_rig combines BOTH optional modules (soft skinning + two-bone IK), so it
  # only exists when both are enabled; neither module requires the other.
  if(PIXELICA_BUILD_SKELETON AND PIXELICA_BUILD_IK)
    pixelica_add_example(hello_rig)
    target_link_libraries(hello_rig PRIVATE pixelica_skeleton pixelica_ik)
  endif()

  # hello_25d: 3D mesh + depth-tested 2D billboards/HUD through pixelica/hybrid.h, on
  # the same PIXELICA_Game runner as the examples above.
  if(PIXELICA_ENABLE_HYBRID)
    pixelica_add_guarded_example(hello_25d
      DRIVER pixelica_example_driver
      LIBS pixelica_hybrid pixelica_renderer2d pixelica_core)
  endif()

  # -------------------------------------------------------------------------
  # Pixel Room
  #
  #   pixel_room_core      portable game logic (no platform, no embedded data);
  #                        test_pixel_room links this and builds its own assets.
  #   pixel_room_embedded  the generated assets/compiled/pixel_room_assets.h as
  #                        byte arrays (no runtime filesystem dependency).
  #   pixel_room           STANDARD 256x192 game        (same source, -D sizes)
  #   pixel_room_tiny      TINY 160x120 game
  #   pixel_room_capture   headless screenshot / stats / wall-clock benchmark
  #
  # The embedded assets are produced by the asset generator, so the executables
  # only exist once assets/compiled/pixel_room_assets.h does; the glob below
  # re-runs CMake when it appears.
  # -------------------------------------------------------------------------
  add_library(pixel_room_core STATIC
    pixel_room/pixel_room.c
    pixel_room/pixel_room_world.c)
  target_include_directories(pixel_room_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/pixel_room")
  target_link_libraries(pixel_room_core PUBLIC pixelica_core PRIVATE pixelica_options)

  set(PIXELICA_ROOM_ASSET_DIR "${PROJECT_SOURCE_DIR}/assets/compiled")
  file(GLOB PIXELICA_ROOM_ASSET_HEADER CONFIGURE_DEPENDS "${PIXELICA_ROOM_ASSET_DIR}/pixel_room_assets.h")

  if(PIXELICA_ROOM_ASSET_HEADER)
    add_library(pixel_room_embedded STATIC pixel_room/room_embedded.c)
    target_include_directories(pixel_room_embedded PRIVATE "${PIXELICA_ROOM_ASSET_DIR}")
    target_link_libraries(pixel_room_embedded PUBLIC pixel_room_core PRIVATE pixelica_options)

    function(pixelica_add_pixel_room name width height)
      add_executable(${name} pixel_room/pixel_room_game.c)
      target_compile_definitions(${name} PRIVATE
        PIXEL_ROOM_WIDTH=${width} PIXEL_ROOM_HEIGHT=${height})
      target_link_libraries(${name} PRIVATE pixelica_example_driver pixel_room_embedded pixelica_options)
      set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
      if(BUILD_TESTING)
        add_test(NAME smoke_${name} COMMAND ${name} --frames 30 --hidden)
        set_tests_properties(smoke_${name} PROPERTIES LABELS "smoke" TIMEOUT 60)
      endif()
    endfunction()

    pixelica_add_pixel_room(pixel_room 256 192)
    pixelica_add_pixel_room(pixel_room_tiny 160 120)

    # hello_pixel_light: the Pixel Room scene lit with the reusable light
    # patches / contact shadows / selective bloom of pixelica/light.h. It owns
    # its own context and PAL-less frame loop (like pixel_room_capture), so the
    # light draws land inside the frame and the bloom composites into a
    # caller-owned snapshot. Pixel Room itself is untouched.
    add_executable(hello_pixel_light hello_pixel_light/hello_pixel_light.c)
    target_link_libraries(hello_pixel_light PRIVATE pixel_room_embedded pixelica_options)
    if(MSVC)
      # Only this translation unit uses plain fopen (C4996), as in the capture tool.
      target_compile_definitions(hello_pixel_light PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set_target_properties(hello_pixel_light PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    if(BUILD_TESTING)
      # Deterministic headless validation: the program itself checks that the
      # unlit and lit frames differ, that every patch/shadow/glow draw was
      # accepted, that the emissive mask is non-empty and that bloom only ever
      # ADDS to the frame. It prints ok=1 only when all of that held.
      add_test(NAME smoke_hello_pixel_light
        COMMAND hello_pixel_light --frames 4 --effects both)
      set_tests_properties(smoke_hello_pixel_light PROPERTIES
        LABELS "smoke" TIMEOUT 120
        PASS_REGULAR_EXPRESSION "hello_pixel_light: ok=1")
      add_test(NAME smoke_hello_pixel_light_tiny
        COMMAND hello_pixel_light --frames 2 --tiny --effects both)
      set_tests_properties(smoke_hello_pixel_light_tiny PROPERTIES
        LABELS "smoke" TIMEOUT 120
        PASS_REGULAR_EXPRESSION "hello_pixel_light: ok=1")
    endif()

    # The capture tool (screenshot / JSON / wall-clock benchmark) follows the root
    # PIXELICA_BUILD_BENCHMARKS option. A root that does not define it keeps the tool.
    if(NOT DEFINED PIXELICA_BUILD_BENCHMARKS OR PIXELICA_BUILD_BENCHMARKS)
      add_executable(pixel_room_capture pixel_room/pixel_room_capture.c)
      target_link_libraries(pixel_room_capture PRIVATE pixel_room_embedded pixelica_options)
      if(MSVC)
        # Only this translation unit uses plain fopen (C4996); warnings stay at
        # /W4 (and /WX when requested) for every other target and diagnostic.
        target_compile_definitions(pixel_room_capture PRIVATE _CRT_SECURE_NO_WARNINGS)
      endif()
      set_target_properties(pixel_room_capture PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
      if(BUILD_TESTING)
        # Quick, finite: renders one frame per profile and prints the JSON report.
        add_test(NAME smoke_pixel_room_capture_tiny
          COMMAND pixel_room_capture --profile tiny --script UP:30,A:2 --json -)
        add_test(NAME smoke_pixel_room_capture_standard
          COMMAND pixel_room_capture --profile standard --script UP:30,A:2 --bench 8 --warmup 2
                  --json -)
        set_tests_properties(smoke_pixel_room_capture_tiny smoke_pixel_room_capture_standard
          PROPERTIES LABELS "smoke" TIMEOUT 60)
      endif()
    endif()
  else()
    message(WARNING
      "Pixel Room executables are skipped: ${PIXELICA_ROOM_ASSET_DIR}/pixel_room_assets.h does not "
      "exist yet. Run tools/pixelica_pack/generate_demo_assets.py to create it (CMake re-configures "
      "automatically once it appears).")
  endif()

  # test_pixel_room (tests/test_pixel_room.c) is created by the root tests glob, which may run
  # before or after this directory is processed, so link it when the root file has finished.
  function(pixelica_link_pixel_room_test)
    if(TARGET test_pixel_room)
      target_link_libraries(test_pixel_room PRIVATE pixel_room_core)
      if(TARGET pixel_room_embedded)
        target_link_libraries(test_pixel_room PRIVATE pixel_room_embedded)
        target_compile_definitions(test_pixel_room PRIVATE PIXEL_ROOM_TEST_EMBEDDED=1)
      endif()
    endif()
  endfunction()
  cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL pixelica_link_pixel_room_test)
endif()

# ---------------------------------------------------------------------------
# 2D examples: hello_2d, hello_isometric
#
# Runner (examples/common2d): main.c is the driver and implements the
# `pixelica2d_example_demo` interface of demo2d.h; any further common2d/*.c is shared
# demo code (atlas data, helpers) built into one static library. With SDL3 the
# driver is compiled with PIXELICA2D_DEMO_SDL and links the platform layer (PAL only,
# no 3D); otherwise it is the plain headless driver. No example here links the 3D
# core.
# ---------------------------------------------------------------------------
if(PIXELICA_BUILD_2D)
  set(PIXELICA2D_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common2d")
  file(GLOB PIXELICA2D_COMMON_SOURCES CONFIGURE_DEPENDS "${PIXELICA2D_COMMON_DIR}/*.c")
  set(PIXELICA2D_DRIVER_SOURCE "${PIXELICA2D_COMMON_DIR}/main.c")
  list(REMOVE_ITEM PIXELICA2D_COMMON_SOURCES "${PIXELICA2D_DRIVER_SOURCE}")
  list(SORT PIXELICA2D_COMMON_SOURCES)
  # Also watch the header so adding it re-configures.
  file(GLOB PIXELICA2D_DEMO_HEADER CONFIGURE_DEPENDS "${PIXELICA2D_COMMON_DIR}/demo2d.h")

  if(EXISTS "${PIXELICA2D_DRIVER_SOURCE}" AND PIXELICA2D_DEMO_HEADER)
    if(PIXELICA2D_COMMON_SOURCES)
      add_library(pixelica2d_example_support STATIC ${PIXELICA2D_COMMON_SOURCES})
      target_include_directories(pixelica2d_example_support PUBLIC "${PIXELICA2D_COMMON_DIR}")
      target_link_libraries(pixelica2d_example_support
        PUBLIC pixelica_renderer2d pixelica_common
        PRIVATE pixelica_options)
    endif()

    # OBJECT library so main() is always linked into the executable.
    add_library(pixelica2d_example_driver OBJECT "${PIXELICA2D_DRIVER_SOURCE}")
    target_include_directories(pixelica2d_example_driver PUBLIC "${PIXELICA2D_COMMON_DIR}")
    target_link_libraries(pixelica2d_example_driver
      PUBLIC pixelica_renderer2d pixelica_common
      PRIVATE pixelica_options)
    if(TARGET pixelica2d_example_support)
      target_link_libraries(pixelica2d_example_driver PUBLIC pixelica2d_example_support)
    endif()
    if(PIXELICA_PLATFORM STREQUAL "SDL3")
      target_compile_definitions(pixelica2d_example_driver PRIVATE PIXELICA2D_DEMO_SDL=1)
      target_link_libraries(pixelica2d_example_driver PUBLIC pixelica_platform_sdl3)
    endif()

    pixelica_add_guarded_example(hello_2d DRIVER pixelica2d_example_driver LIBS pixelica_renderer2d)
    pixelica_add_guarded_example(hello_isometric DRIVER pixelica2d_example_driver LIBS pixelica_renderer2d)
  else()
    message(STATUS "Pixelica: 2D examples not built: examples/common2d/main.c and/or "
                   "examples/common2d/demo2d.h do not exist yet (CMake re-configures once they appear)")
  endif()
endif()
