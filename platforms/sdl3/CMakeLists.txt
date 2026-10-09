# SDL3 platform backend. SDL3 only shows finished pixels, reads the keyboard
# and the clock; it implements the platform abstraction layer (pixelica/platform.h)
# and needs only the common library, so it works for 2D-only, 3D-only and
# common-only builds alike. All rendering stays in pixelica_core / pixelica_renderer2d.

set(PIXELICA_SDL3_VERSION 3.4.18)
set(PIXELICA_SDL3_URL
  "https://github.com/libsdl-org/SDL/releases/download/release-${PIXELICA_SDL3_VERSION}/SDL3-${PIXELICA_SDL3_VERSION}.tar.gz")
set(PIXELICA_SDL3_SHA256 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)

option(PIXELICA_FETCH_SDL3
  "Download and build SDL ${PIXELICA_SDL3_VERSION} from source (needs network) if no matching package is found" OFF)
option(PIXELICA_ALLOW_NEWER_SDL3
  "Accept an installed SDL3 newer than ${PIXELICA_SDL3_VERSION} (the pinned version is the tested one)" OFF)

# 1. Prefer an installed package.
if(PIXELICA_ALLOW_NEWER_SDL3)
  find_package(SDL3 ${PIXELICA_SDL3_VERSION} CONFIG QUIET)
else()
  find_package(SDL3 ${PIXELICA_SDL3_VERSION} EXACT CONFIG QUIET)
endif()

# 2. Optionally fetch the pinned release.
if(NOT SDL3_FOUND)
  if(NOT PIXELICA_FETCH_SDL3)
    message(FATAL_ERROR
      "SDL3 ${PIXELICA_SDL3_VERSION} was not found.\n"
      "  - Install it and point CMake at it (-DSDL3_DIR=<dir containing SDL3Config.cmake>,\n"
      "    or -DCMAKE_PREFIX_PATH=<prefix>), or\n"
      "  - re-run with -DPIXELICA_FETCH_SDL3=ON to build the pinned release from source, or\n"
      "  - use -DPIXELICA_PLATFORM=HEADLESS (no window, no SDL).")
  endif()

  include(FetchContent)
  if(POLICY CMP0135) # CMake >= 3.24: use extraction time for URL downloads
    cmake_policy(SET CMP0135 NEW)
  endif()
  message(STATUS "Fetching SDL ${PIXELICA_SDL3_VERSION} release archive (SHA256-verified)")
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL "${PIXELICA_SDL3_URL}"
    URL_HASH SHA256=${PIXELICA_SDL3_SHA256})
  FetchContent_MakeAvailable(SDL3)
else()
  message(STATUS "Using installed SDL3 ${SDL3_VERSION}")
endif()

if(TARGET SDL3::SDL3)
  set(PIXELICA_SDL3_TARGET SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
  set(PIXELICA_SDL3_TARGET SDL3::SDL3-static)
elseif(TARGET SDL3::SDL3-shared)
  set(PIXELICA_SDL3_TARGET SDL3::SDL3-shared)
else()
  message(FATAL_ERROR "SDL3 was found/fetched but no SDL3::SDL3 target exists")
endif()

# Window-free placement math; also built into the unit test below.
add_library(pixelica_platform_sdl3 STATIC
  platform_sdl3.c
  letterbox.c)
target_include_directories(pixelica_platform_sdl3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pixelica_platform_sdl3
  PUBLIC pixelica_common
  PRIVATE pixelica_options ${PIXELICA_SDL3_TARGET})

# main_sdl3.c (the common main() for the 3D examples) is built by
# examples/CMakeLists.txt because it calls the example game interface in
# examples/common/pixelica_game.h. The 2D examples have their own small driver
# (examples/common2d/main.c) that links this library under PIXELICA2D_DEMO_SDL.

# Optional input adapter: only with the input manager module (the top-level
# CMakeLists.txt adds modules/input_manager before this directory). Build-only,
# like the rest of the SDL layer: never installed, never exported. The header
# includes <SDL3/SDL.h>, so SDL is a PUBLIC dependency.
if(TARGET pixelica_input_manager)
  add_library(pixelica_input_sdl3 STATIC input_sdl3.c)
  add_library(Pixelica::input_sdl3 ALIAS pixelica_input_sdl3)
  target_include_directories(pixelica_input_sdl3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(pixelica_input_sdl3
    PUBLIC pixelica_input_manager ${PIXELICA_SDL3_TARGET}
    PRIVATE pixelica_options)

  # Headless: SDL virtual gamepads only, no window and no physical hardware.
  if(BUILD_TESTING)
    add_executable(pixelica_input_sdl3_test input_sdl3_test.c)
    target_link_libraries(pixelica_input_sdl3_test PRIVATE pixelica_input_sdl3 pixelica_options)
    if(WIN32)
      # A shared SDL3 needs its DLL next to the test executable.
      add_custom_command(TARGET pixelica_input_sdl3_test POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E
                $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:pixelica_input_sdl3_test>>,copy_if_different,true>
                $<TARGET_RUNTIME_DLLS:pixelica_input_sdl3_test>
                $<TARGET_FILE_DIR:pixelica_input_sdl3_test>
        COMMAND_EXPAND_LISTS)
    endif()
    add_test(NAME pixelica_input_sdl3 COMMAND pixelica_input_sdl3_test)
    set_tests_properties(pixelica_input_sdl3 PROPERTIES LABELS "unit;sdl3" TIMEOUT 60)
  endif()
endif()

if(BUILD_TESTING)
  add_executable(pixelica_letterbox_test letterbox_test.c letterbox.c)
  target_include_directories(pixelica_letterbox_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(pixelica_letterbox_test PRIVATE pixelica_options)
  add_test(NAME pixelica_letterbox COMMAND pixelica_letterbox_test)
  set_tests_properties(pixelica_letterbox PROPERTIES LABELS "unit")
endif()
