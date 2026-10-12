# CTest driver for "cmake_package_boundary" (run with cmake -P, no compiler).
#
# Static check of the 2D / 3D package split:
#   1. cmake/PixelicaSources.cmake lists every src/**/*.c and include/pixelica/*.h exactly
#      once (a new file cannot silently be missing from, or twice in, a library).
#   2. Code that must work without the 3D renderer includes no 3D header and no
#      3D-internal file:
#        - the common headers and the 5 common sources (+ container.h),
#        - the 2D renderer module,
#        - the optional skeleton / IK modules (pure math),
#        - the SDL platform layer (PAL only; main_sdl3.c is excluded because it is
#          the 3D example driver).
# It reads #include lines only; compiling the targets proves the rest.
#
# Input (-D): PIXELICA_SOURCE_DIR
cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED PIXELICA_SOURCE_DIR OR NOT IS_DIRECTORY "${PIXELICA_SOURCE_DIR}")
  message(FATAL_ERROR "PixelicaBoundaryCheck: PIXELICA_SOURCE_DIR must name the source directory")
endif()

include("${PIXELICA_SOURCE_DIR}/cmake/PixelicaSources.cmake")
pixelica_check_source_lists("${PIXELICA_SOURCE_DIR}")

set(pixelica_errors "")

# Appends an error for every #include in <file> that names a forbidden public
# header (pixelica/<name>) or matches a forbidden path regex.
macro(pixelica_scan_file file)
  file(STRINGS "${file}" pixelica_include_lines REGEX "^[ \t]*#[ \t]*include")
  foreach(pixelica_line IN LISTS pixelica_include_lines)
    if(pixelica_line MATCHES "[<\"]pixelica/([A-Za-z0-9_]+\\.h)[>\"]")
      set(pixelica_header "${CMAKE_MATCH_1}")
      if(pixelica_header IN_LIST pixelica_forbidden_headers)
        list(APPEND pixelica_errors "${file}: includes pixelica/${pixelica_header}")
      endif()
    endif()
    foreach(pixelica_pattern IN LISTS pixelica_forbidden_paths)
      if(pixelica_line MATCHES "${pixelica_pattern}")
        list(APPEND pixelica_errors "${file}: '${pixelica_line}' reaches a 3D-internal file")
      endif()
    endforeach()
  endforeach()
endmacro()

# Public headers a 3D-free file must not use, and internal-file patterns.
set(pixelica_forbidden_headers ${PIXELICA_RENDERER3D_HEADER_NAMES} hybrid.h)
set(pixelica_forbidden_paths
  "context_internal\\.h"
  "draw_internal\\.h"
  "(^|[/\"<])raster\\.h"
  "(\\.\\./|/)(core|renderer|scene)/")

# 2D renderer and the common code may not include hybrid.h either (hybrid is
# built on top of both); skeleton/IK/platform share the same rule set.
set(pixelica_common_files "")
foreach(name IN LISTS PIXELICA_COMMON_HEADER_NAMES)
  list(APPEND pixelica_common_files "${PIXELICA_SOURCE_DIR}/include/pixelica/${name}")
endforeach()
foreach(name IN LISTS PIXELICA_COMMON_SOURCE_NAMES)
  list(APPEND pixelica_common_files "${PIXELICA_SOURCE_DIR}/src/${name}")
endforeach()
list(APPEND pixelica_common_files "${PIXELICA_SOURCE_DIR}/src/assets/container.h")
foreach(file IN LISTS pixelica_common_files)
  pixelica_scan_file("${file}")
endforeach()

# Common headers also must not need the 2D module.
set(pixelica_forbidden_headers ${PIXELICA_RENDERER3D_HEADER_NAMES} hybrid.h renderer2d.h)
foreach(name IN LISTS PIXELICA_COMMON_HEADER_NAMES)
  pixelica_scan_file("${PIXELICA_SOURCE_DIR}/include/pixelica/${name}")
endforeach()

set(pixelica_forbidden_headers ${PIXELICA_RENDERER3D_HEADER_NAMES} hybrid.h)
foreach(module renderer2d skeleton ik input_manager audio)
  file(GLOB_RECURSE module_files "${PIXELICA_SOURCE_DIR}/modules/${module}/*.c" "${PIXELICA_SOURCE_DIR}/modules/${module}/*.h")
  foreach(file IN LISTS module_files)
    pixelica_scan_file("${file}")
  endforeach()
endforeach()

set(pixelica_forbidden_headers ${PIXELICA_RENDERER3D_HEADER_NAMES} hybrid.h renderer2d.h particles2d.h particles25d.h)
file(GLOB_RECURSE shader_files "${PIXELICA_SOURCE_DIR}/modules/shaders/*.c" "${PIXELICA_SOURCE_DIR}/modules/shaders/*.h"
  "${PIXELICA_SOURCE_DIR}/modules/ui/*.c" "${PIXELICA_SOURCE_DIR}/modules/ui/*.h")
foreach(file IN LISTS shader_files)
  pixelica_scan_file("${file}")
endforeach()
foreach(name particles.c include/pixelica/particles.h)
  if(EXISTS "${PIXELICA_SOURCE_DIR}/modules/particles/${name}")
    pixelica_scan_file("${PIXELICA_SOURCE_DIR}/modules/particles/${name}")
  endif()
endforeach()
set(pixelica_forbidden_headers ${PIXELICA_RENDERER3D_HEADER_NAMES} hybrid.h particles25d.h)
foreach(name particles2d.c include/pixelica/particles2d.h)
  if(EXISTS "${PIXELICA_SOURCE_DIR}/modules/particles/${name}")
    pixelica_scan_file("${PIXELICA_SOURCE_DIR}/modules/particles/${name}")
  endif()
endforeach()

foreach(name platform_sdl3.c platform_sdl3.h letterbox.c letterbox.h audio_sdl3.c audio_sdl3.h)
  if(EXISTS "${PIXELICA_SOURCE_DIR}/platforms/sdl3/${name}")
    pixelica_scan_file("${PIXELICA_SOURCE_DIR}/platforms/sdl3/${name}")
  endif()
endforeach()

if(pixelica_errors)
  list(JOIN pixelica_errors "\n  " joined)
  message(FATAL_ERROR "[boundary] 3D-free code depends on the 3D renderer:\n  ${joined}")
endif()
message(STATUS "[boundary] OK: source/header lists match the tree; common, 2D, skeleton, IK and SDL platform code include no 3D header")
