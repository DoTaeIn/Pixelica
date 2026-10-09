# Optional module: IK (analytic two-bone solver). Added by the top-level
# CMakeLists.txt only when PIXELICA_BUILD_IK=ON, after pixelica_common, GNUInstallDirs and
# PIXELICA_INSTALL_CMAKEDIR exist. Independent of the skeleton module; no library
# links it and neither renderer includes it. It is pure math, so it needs only
# the common library (Pixelica::common), not a renderer. Exported as
# Pixelica::ik in its OWN export set (PixelicaIKTargets.cmake, component "ik").

add_library(pixelica_ik STATIC "${CMAKE_CURRENT_SOURCE_DIR}/ik.c")
add_library(Pixelica::ik ALIAS pixelica_ik)
set_target_properties(pixelica_ik PROPERTIES
  EXPORT_NAME ik
  DEBUG_POSTFIX d)
target_compile_features(pixelica_ik PUBLIC c_std_11)
target_include_directories(pixelica_ik PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# Same build-only warning/sanitizer flags as the other libraries; the installed
# target references neither pixelica_options nor any build-tree path. Its link to
# Pixelica::common is added by the installed package config, so the export set
# stays self-contained.
target_link_libraries(pixelica_ik PUBLIC
  "$<BUILD_INTERFACE:pixelica_common>"
  "$<BUILD_INTERFACE:pixelica_options>")

if(PIXELICA_INSTALL)
  install(TARGETS pixelica_ik
    EXPORT PixelicaIKTargets
    COMPONENT ik
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ik
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ik
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ik
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(DIRECTORY include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT ik
    FILES_MATCHING PATTERN "*.h")
  install(EXPORT PixelicaIKTargets
    NAMESPACE Pixelica::
    FILE PixelicaIKTargets.cmake
    DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
    COMPONENT ik)
endif()
