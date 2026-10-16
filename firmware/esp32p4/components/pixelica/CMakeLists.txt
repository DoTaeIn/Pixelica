# ESP-IDF component "pixelica": the portable Pixelica core (src/**/*.c) plus the
# ESP32-P4 platform (platforms/esp32p4/). The sources stay where they are in the
# repository; this wrapper only points at them.
#
# The source list below is the host build's common files (pixelica_common) PLUS its
# 3D-renderer files (pixelica_core), compiled into this one component so the original
# firmware keeps working unchanged. The authoritative lists live in
# cmake/PixelicaSources.cmake and the cross-check further down fails the configure
# step if the two ever disagree. Do NOT register the standalone 2D component
# (optional_components/pixelica2d) in the same project: it compiles the same common
# files again. A 2D renderer next to this component is optional_components/
# pixelica_renderer2d (REQUIRES pixelica), enabled with -DPIXELICA_BUILD_2D=ON.
#
# The component lives in a directory named "pixelica" on purpose: a directory
# named "esp32p4" would collide with ESP-IDF's own chip component of that name.
#
# This file is executed twice by ESP-IDF: first in CMake *script* mode
# (tools/cmake/scripts/component_get_requirements.cmake) to read REQUIRES, where
# idf_component_register() is a macro that return()s, then in the real build.
# Everything before idf_component_register() must therefore be valid in script
# mode: no add_*/target_* commands and no file(GLOB ... CONFIGURE_DEPENDS).
#
# STATUS: compiled with ESP-IDF v6.0; not flashed or run on a board.

# Repository root: four levels above this file, unless overridden.
if(DEFINED ENV{PIXELICA_ROOT} AND NOT "$ENV{PIXELICA_ROOT}" STREQUAL "")
  get_filename_component(PIXELICA_ROOT "$ENV{PIXELICA_ROOT}" ABSOLUTE)
else()
  get_filename_component(PIXELICA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
endif()
if(NOT EXISTS "${PIXELICA_ROOT}/include/pixelica/pixelica.h" OR NOT IS_DIRECTORY "${PIXELICA_ROOT}/src"
   OR NOT EXISTS "${PIXELICA_ROOT}/platforms/esp32p4/lcd_transport.c")
  message(FATAL_ERROR
    "pixelica component: '${PIXELICA_ROOT}' is not a Pixelica checkout "
    "(expected include/pixelica/pixelica.h, src/ and platforms/esp32p4/). "
    "Set the PIXELICA_ROOT environment variable to the repository root.")
endif()

# Portable core: explicit list, relative to src/. Keep in sync with the lists in
# cmake/PixelicaSources.cmake; the check below fails the configure step when a
# file under src/ is missing here, so a new core file cannot be silently left out
# of the firmware.
set(PIXELICA_CORE_SOURCE_NAMES
  animation/animation.c
  animation/anim_clip.c
  assets/cart.c
  assets/container.c
  assets/mesh_asset.c
  assets/texture_asset.c
  core/context.c
  core/framebuffer.c
  game/app.c
  game/save.c
  game/storage.c
  game/storage_fs.c
  game/screens.c
  game/i18n.c
  game/camera.c
  game/debug.c
  game/chunks.c
  game/framebuffer.c
  game/collision2d.c
  input/input.c
  light/bloom.c
  light/light_mask.c
  math/mat4.c
  math/quat.c
  renderer/light_draw.c
  renderer/pipeline.c
  renderer/rasterizer.c
  scene/aabb.c
  scene/scene.c)

set(PIXELICA_CORE_SOURCES "")
foreach(pixelica_name IN LISTS PIXELICA_CORE_SOURCE_NAMES)
  if(NOT EXISTS "${PIXELICA_ROOT}/src/${pixelica_name}")
    message(FATAL_ERROR "pixelica component: listed core source src/${pixelica_name} does not exist")
  endif()
  list(APPEND PIXELICA_CORE_SOURCES "${PIXELICA_ROOT}/src/${pixelica_name}")
endforeach()

# Plain GLOB_RECURSE (no CONFIGURE_DEPENDS) is valid in script mode. It is only
# used to detect drift, never to choose the sources.
file(GLOB_RECURSE pixelica_found_sources RELATIVE "${PIXELICA_ROOT}/src" "${PIXELICA_ROOT}/src/*.c")
foreach(pixelica_name IN LISTS pixelica_found_sources)
  if(NOT pixelica_name IN_LIST PIXELICA_CORE_SOURCE_NAMES)
    message(FATAL_ERROR
      "pixelica component: src/${pixelica_name} exists but is not listed in "
      "firmware/esp32p4/components/pixelica/CMakeLists.txt (PIXELICA_CORE_SOURCE_NAMES)")
  endif()
endforeach()

# Cross-check with the host build's explicit lists (cmake/PixelicaSources.cmake:
# common + 3D-renderer files): both must name exactly the same files, so the
# firmware and the host libraries cannot silently disagree.
if(NOT EXISTS "${PIXELICA_ROOT}/cmake/PixelicaSources.cmake")
  message(FATAL_ERROR "pixelica component: ${PIXELICA_ROOT}/cmake/PixelicaSources.cmake is missing")
endif()
include("${PIXELICA_ROOT}/cmake/PixelicaSources.cmake")
set(pixelica_host_source_names ${PIXELICA_COMMON_SOURCE_NAMES} ${PIXELICA_RENDERER3D_SOURCE_NAMES})
foreach(pixelica_name IN LISTS pixelica_host_source_names)
  if(NOT pixelica_name IN_LIST PIXELICA_CORE_SOURCE_NAMES)
    message(FATAL_ERROR
      "pixelica component: cmake/PixelicaSources.cmake lists src/${pixelica_name} but "
      "firmware/esp32p4/components/pixelica/CMakeLists.txt (PIXELICA_CORE_SOURCE_NAMES) does not")
  endif()
endforeach()
foreach(pixelica_name IN LISTS PIXELICA_CORE_SOURCE_NAMES)
  if(NOT pixelica_name IN_LIST pixelica_host_source_names)
    message(FATAL_ERROR
      "pixelica component: PIXELICA_CORE_SOURCE_NAMES lists src/${pixelica_name} which "
      "cmake/PixelicaSources.cmake does not")
  endif()
endforeach()

set(PIXELICA_PORT_DIR "${PIXELICA_ROOT}/platforms/esp32p4")
set(PIXELICA_PORT_SOURCES
  "${PIXELICA_PORT_DIR}/platform_esp32p4.c"
  "${PIXELICA_PORT_DIR}/display_esp_lcd.c"
  "${PIXELICA_PORT_DIR}/input_gpio.c"
  "${PIXELICA_PORT_DIR}/lcd_transport.c")

idf_component_register(
  SRCS ${PIXELICA_CORE_SOURCES} ${PIXELICA_PORT_SOURCES}
    "${PIXELICA_ROOT}/modules/ui/ui.c" "${PIXELICA_ROOT}/modules/ui/ui_rgb565.c"
    "${PIXELICA_ROOT}/modules/ui/ui_font.c" "${PIXELICA_ROOT}/modules/ui/ui_presenter.c" "${PIXELICA_ROOT}/modules/ui/ui_debug.c"
  INCLUDE_DIRS "${PIXELICA_ROOT}/include" "${PIXELICA_PORT_DIR}/include" "${PIXELICA_ROOT}/modules/ui/include"
  PRIV_INCLUDE_DIRS "${PIXELICA_ROOT}/src" "${PIXELICA_PORT_DIR}"
  # pixelica_esp32p4.h includes esp_err.h and the esp_lcd panel / panel IO headers.
  REQUIRES esp_lcd
  PRIV_REQUIRES esp_timer esp_driver_gpio esp_hw_support heap freertos log)

# Nothing below runs in script mode (idf_component_register() returned above).
#
# The standalone 2D component compiles the same common sources: refuse the mix.
idf_build_get_property(pixelica_build_components BUILD_COMPONENTS)
if("pixelica2d" IN_LIST pixelica_build_components)
  message(FATAL_ERROR
    "pixelica component: the standalone 'pixelica2d' component is part of this project too. "
    "Both compile the 5 common sources (duplicate symbols). Use pixelica_renderer2d next to pixelica.")
endif()
#
# No -std override on purpose: ESP-IDF v6.0 builds C with -std=gnu23
# (tools/cmake/build.cmake) and its headers are validated with that. The core is
# plain C11 and is checked with -std=c11 -pedantic on the host.
