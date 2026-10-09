# 2D renderer (pixelica/renderer2d.h). Added by the top-level CMakeLists.txt when
# PIXELICA_BUILD_2D=ON, after pixelica_common, GNUInstallDirs and PIXELICA_INSTALL_CMAKEDIR
# exist. Exported as Pixelica::renderer2d in its OWN export set
# (PixelicaRenderer2DTargets.cmake, component "renderer2d"); the installed
# find_package(Pixelica2D) wrapper also names it Pixelica2D::core.
#
# It links ONLY the common library: no 3D renderer, context, scene, mesh or cart
# code and no 3D header. The common library's in-tree include directory holds
# just the common headers, so a stray #include "pixelica/pixelica.h" fails to compile.

# The .c file names belong to the engine code; every .c directly in this
# directory or in src/ is part of the library. CONFIGURE_DEPENDS re-runs CMake
# when one is added.
file(GLOB PIXELICA_RENDERER2D_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(SORT PIXELICA_RENDERER2D_SOURCES)
if(NOT PIXELICA_RENDERER2D_SOURCES)
  message(FATAL_ERROR
    "modules/renderer2d: no .c source found (expected modules/renderer2d/*.c or "
    "modules/renderer2d/src/*.c). The public header exists but the implementation "
    "does not yet: configure with -DPIXELICA_BUILD_2D=OFF until it does.")
endif()

add_library(pixelica_renderer2d STATIC ${PIXELICA_RENDERER2D_SOURCES})
add_library(Pixelica::renderer2d ALIAS pixelica_renderer2d)
set_target_properties(pixelica_renderer2d PROPERTIES
  EXPORT_NAME renderer2d
  DEBUG_POSTFIX d)
target_compile_features(pixelica_renderer2d PUBLIC c_std_11)
target_include_directories(pixelica_renderer2d
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_include_directories(pixelica_renderer2d PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
# Same build-only warning/sanitizer flags as the other libraries; the installed
# target references neither pixelica_options nor any build-tree path, and its link to
# Pixelica::common is added by the installed package config.
target_link_libraries(pixelica_renderer2d PUBLIC
  "$<BUILD_INTERFACE:pixelica_common>"
  "$<BUILD_INTERFACE:pixelica_options>")

if(PIXELICA_INSTALL)
  install(TARGETS pixelica_renderer2d
    EXPORT PixelicaRenderer2DTargets
    COMPONENT renderer2d
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT renderer2d
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT renderer2d
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT renderer2d
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(DIRECTORY include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT renderer2d
    FILES_MATCHING PATTERN "*.h")
  install(EXPORT PixelicaRenderer2DTargets
    NAMESPACE Pixelica::
    FILE PixelicaRenderer2DTargets.cmake
    DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
    COMPONENT renderer2d)

  # Copyable 2D starter project (added by the demo owner; absent until then).
  if(IS_DIRECTORY "${PROJECT_SOURCE_DIR}/sdk/template2d")
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/sdk/template2d/"
      DESTINATION "${PIXELICA_INSTALL_SHAREDIR}/template2d"
      COMPONENT renderer2d
      PATTERN "__pycache__" EXCLUDE)
  endif()
endif()
