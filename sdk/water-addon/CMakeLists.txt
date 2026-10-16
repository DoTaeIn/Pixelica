cmake_minimum_required(VERSION 3.21)
project(PixelicaWater VERSION 0.1.0 LANGUAGES C)
# Root of the PORTABLE SOURCE add-on (the layout of the -source ZIP). It builds
# modules/water against an ALREADY INSTALLED base SDK and installs the same
# `water` / `water3d` components the binary add-on ships, into the prefix the
# user chooses - normally the base SDK's own prefix.
include(GNUInstallDirs)
option(PIXELICA_INSTALL "Install the optional water module" ON)
option(PIXELICA_WATER_BUILD_EXAMPLE "Build the raft demo (needs the 3D core)" OFF)

find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS common)
if(NOT TARGET pixelica_common)
  add_library(pixelica_common ALIAS Pixelica::common)
endif()
# The in-tree build adds warning/sanitizer flags through this target; an
# external build of the add-on simply has none.
if(NOT TARGET pixelica_options)
  add_library(pixelica_options INTERFACE)
endif()

# The draw adapter is optional: it is built only when the SAME prefix also
# offers the 3D core, so this add-on works on a common-only or 2D-only SDK too.
find_package(Pixelica 0.1 CONFIG QUIET COMPONENTS core)
if(TARGET Pixelica::core AND NOT TARGET pixelica_core)
  add_library(pixelica_core ALIAS Pixelica::core)
endif()

add_subdirectory(modules/water)

if(PIXELICA_WATER_BUILD_EXAMPLE)
  if(NOT TARGET pixelica_water3d)
    message(FATAL_ERROR
      "PIXELICA_WATER_BUILD_EXAMPLE=ON needs the 3D core in CMAKE_PREFIX_PATH "
      "(the raft demo draws the surface); install the 3D SDK or leave it OFF")
  endif()
  add_executable(hello_water examples/hello_water/hello_water.c)
  target_link_libraries(hello_water PRIVATE pixelica_water3d)
  if(MSVC)
    target_compile_definitions(hello_water PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  enable_testing()
  add_test(NAME smoke_hello_water COMMAND hello_water --frames 240)
  set_tests_properties(smoke_hello_water PROPERTIES
    PASS_REGULAR_EXPRESSION "hello_water: ok=1")
endif()
