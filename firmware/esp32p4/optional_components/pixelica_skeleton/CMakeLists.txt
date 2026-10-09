# OPTIONAL ESP-IDF component "pixelica_skeleton": the Pixelica skeleton module
# (modules/skeleton). It is only part of a firmware build when the project is
# configured with -DPIXELICA_BUILD_SKELETON=ON (see firmware/esp32p4/CMakeLists.txt,
# which then appends this directory to EXTRA_COMPONENT_DIRS). The sources stay
# in the repository's modules/ tree; this wrapper only points at them. They do
# NOT enter the "pixelica" component's core source list or its src/ drift check.
#
# Like components/pixelica, this file runs twice: first in CMake *script* mode
# to read REQUIRES (idf_component_register() is a macro that return()s), then in
# the real build. Everything before idf_component_register() is therefore valid
# in script mode: no add_*/target_* commands, no file(GLOB ... CONFIGURE_DEPENDS).
#
# STATUS: build prep only. Compiler audit recorded in docs/RIGGING_AUDIT.md;
# no hardware execution.

# Repository root: four levels above this file, unless overridden.
if(DEFINED ENV{PIXELICA_ROOT} AND NOT "$ENV{PIXELICA_ROOT}" STREQUAL "")
  get_filename_component(PIXELICA_ROOT "$ENV{PIXELICA_ROOT}" ABSOLUTE)
else()
  get_filename_component(PIXELICA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
endif()
if(NOT EXISTS "${PIXELICA_ROOT}/modules/skeleton/skeleton.c"
   OR NOT EXISTS "${PIXELICA_ROOT}/modules/skeleton/include/pixelica/skeleton.h")
  message(FATAL_ERROR
    "pixelica_skeleton component: '${PIXELICA_ROOT}' does not contain modules/skeleton "
    "(expected skeleton.c and include/pixelica/skeleton.h). "
    "Set the PIXELICA_ROOT environment variable to the repository root.")
endif()

idf_component_register(
  SRCS "${PIXELICA_ROOT}/modules/skeleton/skeleton.c"
  INCLUDE_DIRS "${PIXELICA_ROOT}/modules/skeleton/include"
  # pixelica/math.h and pixelica/status.h come from the core component.
  REQUIRES pixelica)
