# Optional module: skeleton (hierarchical pose, matrix palette, linear-blend
# skinning). Added by the top-level CMakeLists.txt only when
# PIXELICA_BUILD_SKELETON=ON, after pixelica_common, GNUInstallDirs and
# PIXELICA_INSTALL_CMAKEDIR exist. Independent of the IK module; no library links it
# and neither renderer includes it. It is pure math, so it needs only the common
# library (Pixelica::common), not a renderer: it works in 2D-only and common-only
# builds. Exported as Pixelica::skeleton in its OWN export set
# (PixelicaSkeletonTargets.cmake, component "skeleton").

add_library(pixelica_skeleton STATIC "${CMAKE_CURRENT_SOURCE_DIR}/skeleton.c")
add_library(Pixelica::skeleton ALIAS pixelica_skeleton)
set_target_properties(pixelica_skeleton PROPERTIES
  EXPORT_NAME skeleton
  DEBUG_POSTFIX d)
target_compile_features(pixelica_skeleton PUBLIC c_std_11)
target_include_directories(pixelica_skeleton PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# Same build-only warning/sanitizer flags as the other libraries; the installed
# target references neither pixelica_options nor any build-tree path. Its link to
# Pixelica::common is added by the installed package config, so the export set
# stays self-contained.
target_link_libraries(pixelica_skeleton PUBLIC
  "$<BUILD_INTERFACE:pixelica_common>"
  "$<BUILD_INTERFACE:pixelica_options>")

if(PIXELICA_INSTALL)
  install(TARGETS pixelica_skeleton
    EXPORT PixelicaSkeletonTargets
    COMPONENT skeleton
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT skeleton
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT skeleton
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT skeleton
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(DIRECTORY include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT skeleton
    FILES_MATCHING PATTERN "*.h")
  install(EXPORT PixelicaSkeletonTargets
    NAMESPACE Pixelica::
    FILE PixelicaSkeletonTargets.cmake
    DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
    COMPONENT skeleton)
endif()
