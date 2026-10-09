# Helper project of cmake/PixelicaPackageConfigTest.cmake: configures the real
# PixelicaConfig.cmake.in / Pixelica2DConfig.cmake.in templates exactly like the
# root CMakeLists.txt does, into PIXELICA_OUT. No language is enabled, nothing is built.
#
# Inputs (-D): PIXELICA_SRC PIXELICA_OUT PIXELICA_INSTALL_CMAKEDIR PIXELICA_INSTALL_CMAKEDIR_2D
#              PIXELICA_INSTALL_SHAREDIR
cmake_minimum_required(VERSION 3.21)
project(PackageConfigGen NONE)

include(CMakePackageConfigHelpers)

foreach(var PIXELICA_SRC PIXELICA_OUT PIXELICA_INSTALL_CMAKEDIR PIXELICA_INSTALL_CMAKEDIR_2D PIXELICA_INSTALL_SHAREDIR)
  if("${${var}}" STREQUAL "")
    message(FATAL_ERROR "PackageConfigGen: ${var} is required")
  endif()
endforeach()

configure_package_config_file(
  "${PIXELICA_SRC}/cmake/PixelicaConfig.cmake.in"
  "${PIXELICA_OUT}/PixelicaConfig.cmake"
  INSTALL_DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}")
configure_package_config_file(
  "${PIXELICA_SRC}/cmake/Pixelica2DConfig.cmake.in"
  "${PIXELICA_OUT}/Pixelica2DConfig.cmake"
  INSTALL_DESTINATION "${PIXELICA_INSTALL_CMAKEDIR_2D}")
