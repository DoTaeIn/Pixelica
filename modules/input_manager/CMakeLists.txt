# Optional module: input manager (device snapshots -> actions, shared by the 2D
# and 3D profiles). Added by the top-level CMakeLists.txt only when
# PIXELICA_BUILD_INPUT_MANAGER=ON, after pixelica_common, GNUInstallDirs and
# PIXELICA_INSTALL_CMAKEDIR exist. Independent of the other modules; no library links
# it and neither renderer includes it. It is allocation-free portable C, so it
# needs only the common library (Pixelica::common), not a renderer: it works in
# 2D-only, 3D-only and common-only builds. Exported as Pixelica::input_manager in
# its OWN export set (PixelicaInputManagerTargets.cmake, component
# "input_manager"). The SDL3 adapter (platforms/sdl3/input_sdl3.*) is a separate,
# never-installed build-tree library.

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/input_manager.c")
  message(FATAL_ERROR "PIXELICA_BUILD_INPUT_MANAGER=ON but ${CMAKE_CURRENT_SOURCE_DIR}/input_manager.c does not exist")
endif()

add_library(pixelica_input_manager STATIC "${CMAKE_CURRENT_SOURCE_DIR}/input_manager.c")
add_library(Pixelica::input_manager ALIAS pixelica_input_manager)
set_target_properties(pixelica_input_manager PROPERTIES
  EXPORT_NAME input_manager
  DEBUG_POSTFIX d)
target_compile_features(pixelica_input_manager PUBLIC c_std_11)
target_include_directories(pixelica_input_manager PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# Same build-only warning/sanitizer flags as the other libraries; the installed
# target references neither pixelica_options nor any build-tree path. Its link to
# Pixelica::common is added by the installed package config, so the export set
# stays self-contained.
target_link_libraries(pixelica_input_manager PUBLIC
  "$<BUILD_INTERFACE:pixelica_common>"
  "$<BUILD_INTERFACE:pixelica_options>")

if(PIXELICA_INSTALL)
  install(TARGETS pixelica_input_manager
    EXPORT PixelicaInputManagerTargets
    COMPONENT input_manager
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT input_manager
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT input_manager
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT input_manager
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(DIRECTORY include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT input_manager
    FILES_MATCHING PATTERN "*.h")
  install(EXPORT PixelicaInputManagerTargets
    NAMESPACE Pixelica::
    FILE PixelicaInputManagerTargets.cmake
    DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
    COMPONENT input_manager)
endif()
