# Optional module: online client (room-code co-op, casual real-time versus and
# best-score record boards over the relay in tools/online_server). Added by the
# top-level CMakeLists.txt only when the online option is ON, after pixelica_options,
# GNUInstallDirs and PIXELICA_INSTALL_CMAKEDIR exist. Portable C11 that links no
# renderer; it shares the common SDK dependency, so it works in 2D-only, 3D-only and
# common-only builds.
#
# Windows: native WinHTTP (system TLS store). Linux: libcurl + pthreads. Any
# other platform stops at configure time.
#
# Exported as Pixelica::online in its OWN export set
# (PixelicaOnlineTargets.cmake, component "online"). The static library's
# private dependencies are exported as $<LINK_ONLY:...>: winhttp on Windows
# (a system library, nothing to find), and CURL::libcurl + Threads::Threads on
# Linux, so the installed package config must find_dependency(CURL) and
# find_dependency(Threads) before it includes PixelicaOnlineTargets.cmake.
#
# Tests may include the private online_internal.h (parsers, transport seam)
# through PIXELICA_ONLINE_INTERNAL_INCLUDE_DIR; it is never installed.

set(PIXELICA_ONLINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/online.c")
if(WIN32)
  list(APPEND PIXELICA_ONLINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/online_win32.c")
elseif(UNIX AND NOT APPLE)
  find_package(CURL 7.68 REQUIRED)
  find_package(Threads REQUIRED)
  list(APPEND PIXELICA_ONLINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/online_posix.c")
else()
  message(FATAL_ERROR "The online module supports Windows (WinHTTP) and Linux (libcurl) only")
endif()
foreach(pixelica_online_source IN LISTS PIXELICA_ONLINE_SOURCES)
  if(NOT EXISTS "${pixelica_online_source}")
    message(FATAL_ERROR "online module enabled but ${pixelica_online_source} does not exist")
  endif()
endforeach()

add_library(pixelica_online STATIC ${PIXELICA_ONLINE_SOURCES})
add_library(Pixelica::online ALIAS pixelica_online)
set_target_properties(pixelica_online PROPERTIES
  EXPORT_NAME online
  DEBUG_POSTFIX d)
target_compile_features(pixelica_online PUBLIC c_std_11)
target_include_directories(pixelica_online
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# Same build-only warning/sanitizer flags as the other libraries; the installed
# target references neither pixelica_options nor any build-tree path.
target_link_libraries(pixelica_online PUBLIC "$<BUILD_INTERFACE:pixelica_common>" "$<BUILD_INTERFACE:pixelica_options>")
if(WIN32)
  target_link_libraries(pixelica_online PRIVATE winhttp)
else()
  target_link_libraries(pixelica_online PRIVATE CURL::libcurl Threads::Threads)
endif()

set(PIXELICA_ONLINE_INTERNAL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" PARENT_SCOPE)

if(PIXELICA_INSTALL)
  install(TARGETS pixelica_online
    EXPORT PixelicaOnlineTargets
    COMPONENT online
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT online
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT online
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT online
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/pixelica/online.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pixelica"
    COMPONENT online)
  install(EXPORT PixelicaOnlineTargets
    NAMESPACE Pixelica::
    FILE PixelicaOnlineTargets.cmake
    DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
    COMPONENT online)
endif()
