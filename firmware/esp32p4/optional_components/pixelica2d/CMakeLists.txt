# OPTIONAL, STANDALONE ESP-IDF component "pixelica2d": the Pixelica2D engine without
# any 3D code. It contains
#   - the 5 common sources  (math, input, animation, asset container, texture
#     loader: src/math/mat4.c, src/input/input.c, src/animation/animation.c,
#     src/assets/container.c, src/assets/texture_asset.c), and
#   - the 2D renderer sources of modules/renderer2d.
# It does NOT depend on, include or compile anything of the "pixelica" component
# (no context, scene, rasterizer, mesh/cart loader, no pixelica.h) and it ships NO
# board port: the application provides its own PIXELICA_PlatformOps (pixelica/platform.h)
# and presents the RGB565 surface itself. Hardware integration is board-unknown.
#
# For OTHER firmware projects that want 2D only:
#   set(EXTRA_COMPONENT_DIRS "<repo>/firmware/esp32p4/optional_components/pixelica2d")
#   ... and list `pixelica2d` in your component's REQUIRES.
#
# NEVER register this component together with the legacy "pixelica" component
# (components/pixelica) in one project: both would compile the same 5 common
# sources and the link would see duplicate symbols. The legacy firmware project
# (firmware/esp32p4/CMakeLists.txt) therefore never adds this directory; it adds
# optional_components/pixelica_renderer2d (REQUIRES pixelica) for its 2D option
# instead. This file fails the build if both are part of the same project.
#
# Like components/pixelica, this file runs twice: first in CMake *script* mode
# to read REQUIRES (idf_component_register() is a macro that return()s), then in
# the real build. Everything before idf_component_register() is valid in script
# mode: no add_*/target_* commands, no file(GLOB ... CONFIGURE_DEPENDS).
#
# STATUS: compiled and linked with ESP-IDF v6.0 for esp32p4, image generated.
# No physical board run or performance measurement; see docs/DIMENSIONS_AUDIT.md.

# Repository root: four levels above this file, unless overridden.
if(DEFINED ENV{PIXELICA_ROOT} AND NOT "$ENV{PIXELICA_ROOT}" STREQUAL "")
  get_filename_component(PIXELICA_ROOT "$ENV{PIXELICA_ROOT}" ABSOLUTE)
else()
  get_filename_component(PIXELICA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
endif()
if(NOT EXISTS "${PIXELICA_ROOT}/cmake/PixelicaSources.cmake"
   OR NOT EXISTS "${PIXELICA_ROOT}/modules/renderer2d/include/pixelica/renderer2d.h")
  message(FATAL_ERROR
    "pixelica2d component: '${PIXELICA_ROOT}' is not a Pixelica checkout with the 2D renderer "
    "(expected cmake/PixelicaSources.cmake and modules/renderer2d/include/pixelica/renderer2d.h). "
    "Set the PIXELICA_ROOT environment variable to the repository root.")
endif()

# One source of truth for the common file list (shared with the host build);
# pixelica_check_source_lists() also fails when a file under src/ is in neither list.
include("${PIXELICA_ROOT}/cmake/PixelicaSources.cmake")
pixelica_check_source_lists("${PIXELICA_ROOT}")

set(PIXELICA2D_SOURCES "")
foreach(pixelica_name IN LISTS PIXELICA_COMMON_SOURCE_NAMES)
  list(APPEND PIXELICA2D_SOURCES "${PIXELICA_ROOT}/src/${pixelica_name}")
endforeach()

# Every .c directly in modules/renderer2d (and its src/) is part of the 2D
# renderer; the names belong to the engine code.
pixelica_collect_module_sources(PIXELICA2D_RENDERER2D_SOURCES "${PIXELICA_ROOT}/modules/renderer2d")
if(NOT PIXELICA2D_RENDERER2D_SOURCES)
  message(FATAL_ERROR
    "pixelica2d component: no .c source in ${PIXELICA_ROOT}/modules/renderer2d "
    "(the 2D renderer implementation is missing)")
endif()
list(APPEND PIXELICA2D_SOURCES ${PIXELICA2D_RENDERER2D_SOURCES})

# Public include directories: ONLY the common headers (staged copies, so pixelica.h,
# renderer.h, assets.h and scene.h are not on the include path) plus the 2D
# module's own header directory. The staging directory is created in the real
# build only; in script mode the path is never used.
set(PIXELICA2D_STAGED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/pixelica2d_include")
if(NOT CMAKE_SCRIPT_MODE_FILE)
  foreach(pixelica_header IN LISTS PIXELICA_COMMON_HEADER_NAMES)
    configure_file("${PIXELICA_ROOT}/include/pixelica/${pixelica_header}"
                   "${PIXELICA2D_STAGED_INCLUDE}/pixelica/${pixelica_header}" COPYONLY)
  endforeach()
endif()

idf_component_register(
  SRCS ${PIXELICA2D_SOURCES}
  INCLUDE_DIRS "${PIXELICA2D_STAGED_INCLUDE}" "${PIXELICA_ROOT}/modules/renderer2d/include"
  PRIV_INCLUDE_DIRS "${PIXELICA_ROOT}/modules/renderer2d")

# Nothing below runs in script mode (idf_component_register() returned above).
idf_build_get_property(pixelica_build_components BUILD_COMPONENTS)
if("pixelica" IN_LIST pixelica_build_components)
  message(FATAL_ERROR
    "pixelica2d component: the legacy 'pixelica' component is part of this project too. "
    "Both compile the 5 common sources (duplicate symbols). Use optional_components/"
    "pixelica_renderer2d (REQUIRES pixelica) next to pixelica, or pixelica2d alone.")
endif()
