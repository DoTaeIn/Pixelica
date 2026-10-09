# Hybrid 2.5D module (pixelica/hybrid.h): 2D sprites/HUD drawn into the 3D context's
# color + depth buffers. Added by the top-level CMakeLists.txt only when BOTH
# PIXELICA_BUILD_3D and PIXELICA_BUILD_2D are ON. Exported as Pixelica::hybrid in its OWN
# export set (PixelicaHybridTargets.cmake, component "hybrid").
#
# Opt-in: nothing else links it, and the core / renderer2d targets and archives
# never contain it. An installed hybrid needs BOTH the 3D SDK (core) and the 2D
# SDK (renderer2d) in the same prefix; find_package(Pixelica COMPONENTS hybrid)
# fails if either is missing and the 3D-only consumer is unaffected.

# The .c file names belong to the hybrid code; every .c directly in this
# directory or in src/ is part of the library. CONFIGURE_DEPENDS re-runs CMake
# when one is added.
file(GLOB PIXELICA_HYBRID_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(SORT PIXELICA_HYBRID_SOURCES)
if(NOT PIXELICA_HYBRID_SOURCES)
  message(FATAL_ERROR
    "modules/hybrid: no .c source found (expected modules/hybrid/*.c or "
    "modules/hybrid/src/*.c). The public header exists but the implementation "
    "does not yet: configure with -DPIXELICA_BUILD_2D=OFF (or -DPIXELICA_BUILD_3D=OFF) "
    "until it does.")
endif()

add_library(pixelica_hybrid STATIC ${PIXELICA_HYBRID_SOURCES})
add_library(Pixelica::hybrid ALIAS pixelica_hybrid)
set_target_properties(pixelica_hybrid PROPERTIES
  EXPORT_NAME hybrid
  DEBUG_POSTFIX d)
target_compile_features(pixelica_hybrid PUBLIC c_std_11)
target_include_directories(pixelica_hybrid
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  # hybrid.c may include src/core/context_internal.h privately to bind the
  # context's mutable buffers (as "core/context_internal.h" or, from src/core
  # being on the path too, "context_internal.h"); nothing of src/ is exposed to
  # consumers.
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/core")
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_include_directories(pixelica_hybrid PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
# In-tree the dependencies are real targets; the installed target gets its links
# to Pixelica::core and Pixelica::renderer2d from the installed package config.
target_link_libraries(pixelica_hybrid PUBLIC
  "$<BUILD_INTERFACE:pixelica_core>"
  "$<BUILD_INTERFACE:pixelica_renderer2d>"
  "$<BUILD_INTERFACE:pixelica_options>")

if(PIXELICA_INSTALL)
  install(TARGETS pixelica_hybrid
    EXPORT PixelicaHybridTargets
    COMPONENT hybrid
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT hybrid
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT hybrid
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT hybrid
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(DIRECTORY include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT hybrid
    FILES_MATCHING PATTERN "*.h")
  install(EXPORT PixelicaHybridTargets
    NAMESPACE Pixelica::
    FILE PixelicaHybridTargets.cmake
    DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
    COMPONENT hybrid)
endif()
