# OPTIONAL ESP-IDF component "pixelica_renderer2d": the Pixelica 2D renderer
# (modules/renderer2d) for a project that already uses the legacy "pixelica"
# component. It is only part of a firmware build when the project is configured
# with -DPIXELICA_BUILD_2D=ON (see firmware/esp32p4/CMakeLists.txt, which then appends
# this directory to EXTRA_COMPONENT_DIRS; default OFF, like the host build option
# PIXELICA_BUILD_2D at its subproject default). Only the 2D renderer sources are
# compiled here; the 5 common sources are already part of "pixelica", so this
# wrapper must NOT be combined with the standalone "pixelica2d" component (it would
# duplicate them and fail the build).
#
# Which component to use:
#   3D firmware (original project)          pixelica
#   3D firmware + 2D sprites/HUD            pixelica + pixelica_renderer2d  (this file)
#   2D-only firmware in another project     pixelica2d (standalone, no pixelica)
#
# Like components/pixelica, this file runs twice: first in CMake *script* mode
# to read REQUIRES (idf_component_register() is a macro that return()s), then in
# the real build. Everything before idf_component_register() is valid in script
# mode: no add_*/target_* commands, no file(GLOB ... CONFIGURE_DEPENDS).
#
# STATUS: compiled with ESP-IDF v6.0 alongside the legacy pixelica component.
# Image generated; no physical board run. See docs/DIMENSIONS_AUDIT.md.

# Repository root: four levels above this file, unless overridden.
if(DEFINED ENV{PIXELICA_ROOT} AND NOT "$ENV{PIXELICA_ROOT}" STREQUAL "")
  get_filename_component(PIXELICA_ROOT "$ENV{PIXELICA_ROOT}" ABSOLUTE)
else()
  get_filename_component(PIXELICA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
endif()
if(NOT EXISTS "${PIXELICA_ROOT}/cmake/PixelicaSources.cmake"
   OR NOT EXISTS "${PIXELICA_ROOT}/modules/renderer2d/include/pixelica/renderer2d.h")
  message(FATAL_ERROR
    "pixelica_renderer2d component: '${PIXELICA_ROOT}' does not contain modules/renderer2d "
    "(expected cmake/PixelicaSources.cmake and include/pixelica/renderer2d.h). "
    "Set the PIXELICA_ROOT environment variable to the repository root.")
endif()

include("${PIXELICA_ROOT}/cmake/PixelicaSources.cmake")
pixelica_collect_module_sources(PIXELICA_RENDERER2D_SOURCES "${PIXELICA_ROOT}/modules/renderer2d")
if(NOT PIXELICA_RENDERER2D_SOURCES)
  message(FATAL_ERROR
    "pixelica_renderer2d component: no .c source in ${PIXELICA_ROOT}/modules/renderer2d "
    "(the 2D renderer implementation is missing)")
endif()

idf_component_register(
  SRCS ${PIXELICA_RENDERER2D_SOURCES}
  INCLUDE_DIRS "${PIXELICA_ROOT}/modules/renderer2d/include"
  PRIV_INCLUDE_DIRS "${PIXELICA_ROOT}/modules/renderer2d"
  # pixelica/math.h, status.h, image.h, platform.h, animation.h ... come from the
  # pixelica component (its include directory is the whole include/ tree).
  REQUIRES pixelica)

# Nothing below runs in script mode (idf_component_register() returned above).
idf_build_get_property(pixelica_build_components BUILD_COMPONENTS)
if("pixelica2d" IN_LIST pixelica_build_components)
  message(FATAL_ERROR
    "pixelica_renderer2d component: the standalone 'pixelica2d' component is part of this project "
    "too; it duplicates the common sources of 'pixelica'. Use only one of them.")
endif()
