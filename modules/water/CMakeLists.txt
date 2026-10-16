# Optional module: analytic ocean surface + sample-point buoyancy, plus a
# separate 3D render adapter. Added by the top-level CMakeLists.txt only when
# PIXELICA_BUILD_WATER=ON, after pixelica_common, the renderer modules,
# GNUInstallDirs and PIXELICA_INSTALL_CMAKEDIR exist.
#
#   Pixelica::water    portable core (water.c, pixelica/water.h). Needs only
#                      Pixelica::common; always built with the module. Never
#                      includes or links a renderer.
#   Pixelica::water3d  3D adapter (water3d.c, pixelica/water3d.h), only when
#                      pixelica_core exists. Tessellates and draws the surface.
#
# Each library has its OWN export set and install component (water / water3d)
# and installs only its own header, so a core-only install contains no adapter
# header. Its independent installed config (PixelicaWater) never overwrites the
# base SDK loader, so the add-on and either base SDK can be installed in any
# order; the config imports water3d only when that export file is present AND
# the prefix really has the 3D core, so a 2D-only prefix degrades to the core
# library instead of failing.

set(PIXELICA_WATER_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(PIXELICA_WATER_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/PixelicaWater")

function(pixelica_water_require file)
  if(NOT EXISTS "${PIXELICA_WATER_DIR}/${file}")
    message(FATAL_ERROR "Pixelica water module enabled but ${PIXELICA_WATER_DIR}/${file} does not exist")
  endif()
endfunction()

# Common target setup. deps are build-tree targets wrapped in BUILD_INTERFACE,
# so neither pixelica_options nor a build-tree path reaches the installed target.
function(pixelica_water_library target export_name source header component export_file)
  pixelica_water_require("${source}")
  pixelica_water_require("include/pixelica/${header}")
  add_library(${target} STATIC "${PIXELICA_WATER_DIR}/${source}")
  add_library(Pixelica::${export_name} ALIAS ${target})
  set_target_properties(${target} PROPERTIES
    EXPORT_NAME ${export_name}
    DEBUG_POSTFIX d)
  target_compile_features(${target} PUBLIC c_std_11)
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${PIXELICA_WATER_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  set(links "")
  foreach(dep IN LISTS ARGN)
    list(APPEND links "$<BUILD_INTERFACE:${dep}>")
  endforeach()
  target_link_libraries(${target} PUBLIC ${links} "$<BUILD_INTERFACE:pixelica_options>")

  if(PIXELICA_INSTALL)
    install(TARGETS ${target}
      EXPORT ${export_file}
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${component}
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${component}
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${component}
      INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    # One header per component: never install the whole include/ directory.
    install(FILES "${PIXELICA_WATER_DIR}/include/pixelica/${header}"
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pixelica" COMPONENT ${component})
    install(EXPORT ${export_file}
      NAMESPACE Pixelica::
      FILE ${export_file}.cmake
      DESTINATION "${PIXELICA_WATER_CMAKEDIR}" COMPONENT ${component})
  endif()
endfunction()

pixelica_water_library(pixelica_water water
  water.c water.h water PixelicaWaterTargets
  pixelica_common)

if(TARGET pixelica_core)
  pixelica_water_library(pixelica_water3d water3d
    water3d.c water3d.h water3d PixelicaWater3DTargets
    pixelica_water pixelica_core pixelica_common)
endif()

if(PIXELICA_INSTALL)
  include(CMakePackageConfigHelpers)
  configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/PixelicaWaterConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaWaterConfig.cmake"
    INSTALL_DESTINATION "${PIXELICA_WATER_CMAKEDIR}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaWaterConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
  # The config belongs to the `water` component: the core is what every water
  # install has, and the adapter only adds its own export file next to it.
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaWaterConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaWaterConfigVersion.cmake"
    DESTINATION "${PIXELICA_WATER_CMAKEDIR}" COMPONENT water)
  install(FILES "${PROJECT_SOURCE_DIR}/docs/water.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/Pixelica/docs" COMPONENT water)
endif()
