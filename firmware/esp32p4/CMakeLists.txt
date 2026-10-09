# Pixelica ESP32-P4 firmware project (ESP-IDF v6.0, target esp32p4 only).
#
#   idf.py set-target esp32p4
#   idf.py menuconfig        # "Pixelica firmware": game, profile, headless/board
#   idf.py build
#
# STATUS: hello_triangle and Pixel Room TINY compiled with ESP-IDF v6.0.
# Not flashed or run on a board. See docs/porting.md.
cmake_minimum_required(VERSION 3.22)

# Component "pixelica" wraps the core and platform sources from the repo root.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# Optional modules (default OFF, same option names as the host build). Only the
# selected wrapper directory is added, so a default firmware contains neither
# module. Enable with e.g. `idf.py -DPIXELICA_BUILD_SKELETON=ON -DPIXELICA_BUILD_IK=ON
# build`, then list pixelica_skeleton / pixelica_ik in your own component's REQUIRES
# (docs/rigging.md). Must be decided before project.cmake is included.
option(PIXELICA_BUILD_SKELETON "Add the optional pixelica_skeleton component (modules/skeleton)" OFF)
option(PIXELICA_BUILD_IK "Add the optional pixelica_ik component (modules/ik)" OFF)
# 2D renderer next to the 3D core (REQUIRES pixelica; default OFF in firmware,
# unlike the host build's top-level default). It adds ONLY the 2D renderer
# sources: the common sources are already part of "pixelica". The standalone
# optional_components/pixelica2d (common + 2D, no pixelica) is for OTHER projects
# and is never added here, because registering both would duplicate symbols.
option(PIXELICA_BUILD_2D "Add the optional pixelica_renderer2d component (modules/renderer2d)" OFF)
if(PIXELICA_BUILD_2D)
  list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/optional_components/pixelica_renderer2d")
endif()
if(PIXELICA_BUILD_SKELETON)
  list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/optional_components/pixelica_skeleton")
endif()
if(PIXELICA_BUILD_IK)
  list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/optional_components/pixelica_ik")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pixelica_esp32p4)

idf_build_get_property(pixelica_idf_target IDF_TARGET)
if(NOT pixelica_idf_target STREQUAL "esp32p4")
  message(FATAL_ERROR "This project supports IDF_TARGET esp32p4 only (found '${pixelica_idf_target}'). "
                      "Run: idf.py set-target esp32p4")
endif()

# Pinned SDK: ESP-IDF 6.0. The version comes from tools/cmake/version.cmake, which
# exists in every checkout; the IDF_VER build property comes from git describe (or
# version.txt) and is empty for a checkout without tags, so it is only reported.
# A 6.0.x patch release is accepted with a warning; anything else is refused unless
# explicitly allowed.
idf_build_get_property(pixelica_idf_ver IDF_VER)
include("$ENV{IDF_PATH}/tools/cmake/version.cmake")
set(pixelica_idf_numeric "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
message(STATUS "Pixelica: ESP-IDF ${pixelica_idf_numeric} (IDF_VER='${pixelica_idf_ver}')")
if(NOT pixelica_idf_numeric STREQUAL "6.0.0")
  if(IDF_VERSION_MAJOR EQUAL 6 AND IDF_VERSION_MINOR EQUAL 0)
    message(WARNING "Pixelica targets ESP-IDF 6.0.0; found ${pixelica_idf_numeric} (a 6.0 patch release).")
  elseif(NOT PIXELICA_ALLOW_OTHER_IDF)
    message(FATAL_ERROR "Pixelica targets ESP-IDF 6.0; found ${pixelica_idf_numeric}. "
                        "Pass -DPIXELICA_ALLOW_OTHER_IDF=ON to try anyway (unsupported).")
  endif()
endif()
