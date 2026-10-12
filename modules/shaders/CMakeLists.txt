# Portable optional module. Its independent installed config never overwrites
# the base SDK loader, so add-ons can be installed in either order.
add_library(pixelica_shaders STATIC shaders.c)
add_library(Pixelica::shaders ALIAS pixelica_shaders)
set_target_properties(pixelica_shaders PROPERTIES EXPORT_NAME shaders DEBUG_POSTFIX d)
target_compile_features(pixelica_shaders PUBLIC c_std_11)
target_include_directories(pixelica_shaders PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(pixelica_shaders PUBLIC
  "$<BUILD_INTERFACE:pixelica_common>" "$<BUILD_INTERFACE:pixelica_options>")

if(PIXELICA_INSTALL)
  include(CMakePackageConfigHelpers)
  set(shader_cmakedir "${CMAKE_INSTALL_LIBDIR}/cmake/PixelicaShaders")
  configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/PixelicaShadersConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaShadersConfig.cmake"
    INSTALL_DESTINATION "${shader_cmakedir}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaShadersConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
  install(TARGETS pixelica_shaders EXPORT PixelicaShadersTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT shaders
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(FILES include/pixelica/shaders.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pixelica" COMPONENT shaders)
  install(EXPORT PixelicaShadersTargets NAMESPACE Pixelica::
    DESTINATION "${shader_cmakedir}" COMPONENT shaders)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaShadersConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaShadersConfigVersion.cmake"
    DESTINATION "${shader_cmakedir}" COMPONENT shaders)
  install(FILES "${PROJECT_SOURCE_DIR}/docs/shaders.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/Pixelica/docs" COMPONENT shaders)
endif()
