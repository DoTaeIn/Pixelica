# UI is bundled with every base SDK. It stays a separate static archive, so a
# game that never calls UI functions does not gain UI code or frame-time cost.
add_library(pixelica_ui STATIC ui.c ui_rgb565.c ui_font.c ui_presenter.c ui_debug.c)
add_library(Pixelica::ui ALIAS pixelica_ui)
set_target_properties(pixelica_ui PROPERTIES EXPORT_NAME ui DEBUG_POSTFIX d)
target_compile_features(pixelica_ui PUBLIC c_std_11)
target_include_directories(pixelica_ui PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(pixelica_ui PUBLIC
  "$<BUILD_INTERFACE:pixelica_common>" "$<BUILD_INTERFACE:pixelica_options>")
if(PIXELICA_INSTALL)
  include(CMakePackageConfigHelpers)
  set(ui_cmakedir "${CMAKE_INSTALL_LIBDIR}/cmake/PixelicaUI")
  configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/PixelicaUIConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaUIConfig.cmake"
    INSTALL_DESTINATION "${ui_cmakedir}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaUIConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
  install(TARGETS pixelica_ui EXPORT PixelicaUITargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT common
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  # core alone is a complete 3D SDK, just as common is shared by both profiles.
  if(PIXELICA_BUILD_3D)
    install(TARGETS pixelica_ui
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT core)
  endif()
  set(ui_components common)
  if(PIXELICA_BUILD_3D)
    list(APPEND ui_components core)
  endif()
  foreach(component IN LISTS ui_components)
    install(FILES include/pixelica/ui.h
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pixelica" COMPONENT ${component})
    install(EXPORT PixelicaUITargets NAMESPACE Pixelica::
      DESTINATION "${ui_cmakedir}" COMPONENT ${component})
    install(FILES
      "${CMAKE_CURRENT_BINARY_DIR}/PixelicaUIConfig.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/PixelicaUIConfigVersion.cmake"
      DESTINATION "${ui_cmakedir}" COMPONENT ${component})
    install(FILES "${PROJECT_SOURCE_DIR}/docs/ui.md"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/Pixelica/docs" COMPONENT ${component})
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/examples/hello_ui/"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/Pixelica/examples/hello_ui"
      COMPONENT ${component} PATTERN "__pycache__" EXCLUDE)
  endforeach()
endif()
