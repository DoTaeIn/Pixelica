cmake_minimum_required(VERSION 3.21)

project(Pixelica VERSION 0.1.0 LANGUAGES C)

# Defaults differ when Pixelica is the top-level project and when it is pulled
# into another project with add_subdirectory (see docs/sdk.md).
if(PROJECT_IS_TOP_LEVEL)
  set(PIXELICA_IS_TOP_LEVEL ON)
  set(PIXELICA_DEFAULT_PLATFORM "SDL3")
else()
  set(PIXELICA_IS_TOP_LEVEL OFF)
  set(PIXELICA_DEFAULT_PLATFORM "HEADLESS")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(PIXELICA_PLATFORM "${PIXELICA_DEFAULT_PLATFORM}" CACHE STRING "Platform backend: SDL3 or HEADLESS (no SDL needed)")
set_property(CACHE PIXELICA_PLATFORM PROPERTY STRINGS SDL3 HEADLESS)
# Engine profiles. The shared common library (Pixelica::common: math, input,
# animation, asset container, texture loader) is always built. Both OFF is valid
# and gives a common-only build.
option(PIXELICA_BUILD_3D "Build the 3D renderer core (Pixelica::core / Pixelica::renderer3d, pixelica.h API)" ON)
option(PIXELICA_BUILD_2D "Build the 2D renderer (Pixelica::renderer2d / Pixelica2D::core, pixelica/renderer2d.h API)" ${PIXELICA_IS_TOP_LEVEL})
option(PIXELICA_BUILD_EXAMPLES "Build the examples (3D ones need PIXELICA_BUILD_3D, 2D ones PIXELICA_BUILD_2D)" ${PIXELICA_IS_TOP_LEVEL})
option(PIXELICA_BUILD_BENCHMARKS "Build the capture / wall-clock benchmark tool (pixel_room_capture); only used when PIXELICA_BUILD_EXAMPLES and PIXELICA_BUILD_3D are ON" ON)
option(PIXELICA_BUILD_TESTS "Build and register Pixelica's own tests" ${PIXELICA_IS_TOP_LEVEL})
option(PIXELICA_INSTALL "Generate install rules and the find_package(Pixelica) / find_package(Pixelica2D) packages" ${PIXELICA_IS_TOP_LEVEL})
option(PIXELICA_REQUIRE_PYTHON "Fail configuration when Python 3 is missing instead of skipping the Python/asset tests" OFF)
option(PIXELICA_SANITIZE "Enable AddressSanitizer (+ UBSan on GCC/Clang). Not for MinGW." OFF)
option(PIXELICA_WARNINGS_AS_ERRORS "Treat compiler warnings in Pixelica targets as errors" OFF)
# Optional modules live in modules/ (outside src/ and include/): a default build,
# install and package contain none of their headers, libraries or symbols.
# They are independent of each other (docs/rigging.md) and only need the common
# library, not a renderer.
option(PIXELICA_BUILD_SKELETON "Build the optional skeleton module (Pixelica::skeleton: hierarchical pose, palette, linear-blend skinning)" OFF)
option(PIXELICA_BUILD_IK "Build the optional IK module (Pixelica::ik: analytic two-bone IK)" OFF)
option(PIXELICA_BUILD_INPUT_MANAGER "Build the optional input manager module (Pixelica::input_manager: device snapshots -> actions, shared by 2D and 3D; with PIXELICA_PLATFORM=SDL3 also the build-only SDL3 adapter)" OFF)
option(PIXELICA_BUILD_ONLINE "Build optional room-code multiplayer and record boards (Pixelica::online; Windows WinHTTP / Linux libcurl)" OFF)
option(PIXELICA_BUILD_AUDIO "Build optional WAV/PCM audio mixer (Pixelica::audio; with SDL3 also Pixelica::audio_sdl3 output)" OFF)
option(PIXELICA_BUILD_PARTICLES "Build optional fixed-pool particles and adapters for enabled renderers" OFF)
option(PIXELICA_BUILD_SHADERS "Build optional CPU RGB565 pixel shaders and framebuffer effects" OFF)

if(NOT PIXELICA_PLATFORM STREQUAL "SDL3" AND NOT PIXELICA_PLATFORM STREQUAL "HEADLESS")
  message(FATAL_ERROR "PIXELICA_PLATFORM must be SDL3 or HEADLESS (got '${PIXELICA_PLATFORM}')")
endif()

# The 2D / hybrid modules are implemented separately from this build file. While
# an implementation does not exist yet, the corresponding profile is switched off
# for this configure run with a loud warning (instead of failing the whole
# configure step); CONFIGURE_DEPENDS re-runs CMake when the sources appear. A
# tree that has the sources is unaffected.
if(PIXELICA_BUILD_2D)
  file(GLOB PIXELICA_RENDERER2D_PROBE CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/renderer2d/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/renderer2d/src/*.c")
  if(NOT PIXELICA_RENDERER2D_PROBE)
    message(WARNING "Pixelica: PIXELICA_BUILD_2D=ON but modules/renderer2d has no .c source yet; "
                    "configuring WITHOUT the 2D renderer (and without hybrid, 2D tests and 2D examples)")
    set(PIXELICA_BUILD_2D OFF)
  endif()
endif()

# The hybrid (2.5D) module mixes both renderers, so it exists only when both are
# built. It is not a separate option.
set(PIXELICA_ENABLE_HYBRID OFF)
if(PIXELICA_BUILD_3D AND PIXELICA_BUILD_2D)
  file(GLOB PIXELICA_HYBRID_PROBE CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/hybrid/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/hybrid/src/*.c")
  if(PIXELICA_HYBRID_PROBE)
    set(PIXELICA_ENABLE_HYBRID ON)
  else()
    message(WARNING "Pixelica: both engines are ON but modules/hybrid has no .c source yet; "
                    "configuring WITHOUT the hybrid module (hybrid test and hello_25d)")
  endif()
endif()
message(STATUS "Pixelica profiles: 3D=${PIXELICA_BUILD_3D} 2D=${PIXELICA_BUILD_2D} hybrid=${PIXELICA_ENABLE_HYBRID} (common is always built)")

if(PIXELICA_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(PIXELICA_IS_TOP_LEVEL)
  include(CTest) # defines BUILD_TESTING (default ON)
endif()
# Tests run only when Pixelica owns the build (or the parent already enabled
# testing) and PIXELICA_BUILD_TESTS is on. examples/ and platforms/ test the same
# variable, so shadow it for this directory tree.
if(PIXELICA_BUILD_TESTS AND (BUILD_TESTING OR NOT PIXELICA_IS_TOP_LEVEL))
  set(PIXELICA_TESTS_ENABLED ON)
else()
  set(PIXELICA_TESTS_ENABLED OFF)
endif()
set(BUILD_TESTING ${PIXELICA_TESTS_ENABLED})

# ---------------------------------------------------------------------------
# Shared compile options (warnings, sanitizers) for Pixelica's own targets.
# Never part of the installed package: every library links it through
# $<BUILD_INTERFACE:...>, so the exported targets carry no reference to it.
# ---------------------------------------------------------------------------
add_library(pixelica_options INTERFACE)

if(MSVC)
  target_compile_options(pixelica_options INTERFACE /W4 /permissive-)
  if(PIXELICA_WARNINGS_AS_ERRORS)
    target_compile_options(pixelica_options INTERFACE /WX)
  endif()
else()
  target_compile_options(pixelica_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes)
  if(PIXELICA_WARNINGS_AS_ERRORS)
    target_compile_options(pixelica_options INTERFACE -Werror)
  endif()
endif()

if(PIXELICA_SANITIZE)
  if(MSVC)
    # MSVC ASan (VS 2019 16.9+). Incompatible with /RTC1 and incremental linking.
    foreach(var CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELWITHDEBINFO)
      string(REGEX REPLACE "/RTC[1csu]+" "" ${var} "${${var}}")
    endforeach()
    target_compile_options(pixelica_options INTERFACE /fsanitize=address)
    target_link_options(pixelica_options INTERFACE /INCREMENTAL:NO)
    message(STATUS "PIXELICA_SANITIZE: MSVC AddressSanitizer (no UBSan on MSVC)")
  elseif(WIN32)
    message(FATAL_ERROR "PIXELICA_SANITIZE is not supported with MinGW; use MSVC or Linux GCC/Clang")
  else()
    target_compile_options(pixelica_options INTERFACE
      -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(pixelica_options INTERFACE -fsanitize=address,undefined)
    message(STATUS "PIXELICA_SANITIZE: AddressSanitizer + UndefinedBehaviorSanitizer")
  endif()
endif()

include(GNUInstallDirs)
# Install locations are shared by the rules at the bottom and by the modules
# (modules/*/CMakeLists.txt), so they are defined first.
set(PIXELICA_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/Pixelica")
set(PIXELICA_INSTALL_CMAKEDIR_2D "${CMAKE_INSTALL_LIBDIR}/cmake/Pixelica2D")
set(PIXELICA_INSTALL_SHAREDIR "${CMAKE_INSTALL_DATADIR}/Pixelica")

# ---------------------------------------------------------------------------
# Explicit source lists (cmake/PixelicaSources.cmake) + drift check. A file under
# src/ or include/pixelica/ that is in neither list stops the configure step.
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaSources.cmake")
pixelica_check_source_lists("${CMAKE_CURRENT_SOURCE_DIR}")
list(TRANSFORM PIXELICA_COMMON_SOURCE_NAMES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/"
     OUTPUT_VARIABLE PIXELICA_COMMON_SOURCES)
list(TRANSFORM PIXELICA_RENDERER3D_SOURCE_NAMES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/"
     OUTPUT_VARIABLE PIXELICA_RENDERER3D_SOURCES)
list(TRANSFORM PIXELICA_COMMON_HEADER_NAMES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/include/pixelica/"
     OUTPUT_VARIABLE PIXELICA_COMMON_HEADERS)
list(TRANSFORM PIXELICA_RENDERER3D_HEADER_NAMES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/include/pixelica/"
     OUTPUT_VARIABLE PIXELICA_RENDERER3D_HEADERS)

# ---------------------------------------------------------------------------
# pixelica_common: portable C11 code shared by every profile (math, input, animation,
# asset container + texture loader). No SDL, no ESP-IDF, no 3D renderer.
# Exported as Pixelica::common.
#
# Its in-tree include directory is a staged copy holding ONLY the common
# headers, so code that links just the common library (the 2D renderer, the
# common tests, the SDL platform layer) cannot include a 3D header by accident:
# it fails to compile instead.
# ---------------------------------------------------------------------------
set(PIXELICA_COMMON_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/pixelica_common_include")
foreach(pixelica_header IN LISTS PIXELICA_COMMON_HEADER_NAMES)
  # configure_file also makes CMake re-run when the header changes.
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/include/pixelica/${pixelica_header}"
                 "${PIXELICA_COMMON_INCLUDE_DIR}/pixelica/${pixelica_header}" COPYONLY)
endforeach()

add_library(pixelica_common STATIC ${PIXELICA_COMMON_SOURCES})
add_library(Pixelica::common ALIAS pixelica_common)
set_target_properties(pixelica_common PROPERTIES
  EXPORT_NAME common
  DEBUG_POSTFIX d) # Debug and Release can then share one install prefix
target_compile_features(pixelica_common PUBLIC c_std_11)
target_include_directories(pixelica_common PUBLIC
  "$<BUILD_INTERFACE:${PIXELICA_COMMON_INCLUDE_DIR}>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# In-tree consumers keep inheriting the warning/sanitizer flags; the installed
# package does not.
target_link_libraries(pixelica_common PUBLIC "$<BUILD_INTERFACE:pixelica_options>")
if(UNIX)
  target_link_libraries(pixelica_common PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# pixelica_core: the 3D renderer core (context, scene, raster pipeline, mesh/cart
# loaders). Exported as Pixelica::core, the original and still supported name;
# Pixelica::renderer3d is the same target under its new name.
#
# Dependencies between exported libraries (core -> common, hybrid -> core +
# renderer2d, ...) are written $<BUILD_INTERFACE:...>: they bind targets inside
# this tree only. Each export set therefore stands alone, and the installed
# package config (cmake/PixelicaPackage.cmake) connects the imported targets
# from the export files that actually exist. That is what lets two SDKs be
# merged into one prefix.
# ---------------------------------------------------------------------------
add_subdirectory(modules/ui)

if(PIXELICA_BUILD_3D)
  add_library(pixelica_core STATIC ${PIXELICA_RENDERER3D_SOURCES})
  add_library(Pixelica::core ALIAS pixelica_core)
  add_library(Pixelica::renderer3d ALIAS pixelica_core)
  set_target_properties(pixelica_core PROPERTIES
    EXPORT_NAME core
    DEBUG_POSTFIX d)
  target_compile_features(pixelica_core PUBLIC c_std_11)
  target_include_directories(pixelica_core
    PUBLIC
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(pixelica_core PUBLIC
    "$<BUILD_INTERFACE:pixelica_common>"
    "$<BUILD_INTERFACE:pixelica_options>")
  if(UNIX)
    target_link_libraries(pixelica_core PUBLIC m)
  endif()
endif()

# ---------------------------------------------------------------------------
# Optional modules and the 2D / hybrid renderers. Added after the libraries they
# link and before the tests and examples so those can link them. None of them is
# part of pixelica_common, pixelica_core, their source lists or their export sets.
# ---------------------------------------------------------------------------
if(PIXELICA_BUILD_2D)
  add_subdirectory(modules/renderer2d)
endif()
if(PIXELICA_ENABLE_HYBRID)
  add_subdirectory(modules/hybrid)
endif()
if(PIXELICA_BUILD_SKELETON)
  add_subdirectory(modules/skeleton)
endif()
if(PIXELICA_BUILD_IK)
  add_subdirectory(modules/ik)
endif()
# Before the platform backend: platforms/sdl3 builds its input adapter on top of
# pixelica_input_manager when the target exists.
if(PIXELICA_BUILD_INPUT_MANAGER)
  add_subdirectory(modules/input_manager)
endif()
if(PIXELICA_BUILD_AUDIO)
  add_subdirectory(modules/audio)
  if(PIXELICA_INSTALL)
    install(FILES docs/audio.md
      DESTINATION "${PIXELICA_INSTALL_SHAREDIR}/docs" COMPONENT audio)
  endif()
endif()
if(PIXELICA_BUILD_SHADERS)
  add_subdirectory(modules/shaders)
endif()
if(PIXELICA_BUILD_PARTICLES)
  add_subdirectory(modules/particles)
  if(PIXELICA_INSTALL)
    install(FILES docs/particles.md
      DESTINATION "${PIXELICA_INSTALL_SHAREDIR}/docs" COMPONENT particles)
    # An add-on updates the loader in an older matching base SDK, without
    # replacing any renderer or common library.
    install(CODE "file(COPY_FILE \"${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaPackage.cmake\"
      \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${PIXELICA_INSTALL_CMAKEDIR}/PixelicaPackage.cmake\")"
      COMPONENT particles)
  endif()
endif()
if(PIXELICA_BUILD_ONLINE)
  add_subdirectory(modules/online)
  if(PIXELICA_INSTALL)
    install(DIRECTORY tools/online_server/
      DESTINATION "${PIXELICA_INSTALL_SHAREDIR}/tools/online_server" COMPONENT online
      PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE PATTERN "*.sqlite3*" EXCLUDE)
    install(DIRECTORY templates/online/
      DESTINATION "${PIXELICA_INSTALL_SHAREDIR}/template_online" COMPONENT online
      PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
    install(FILES docs/online.md
      DESTINATION "${PIXELICA_INSTALL_SHAREDIR}/docs" COMPONENT online)
  endif()
endif()

# ---------------------------------------------------------------------------
# Platform backend
# ---------------------------------------------------------------------------
if(PIXELICA_PLATFORM STREQUAL "SDL3")
  add_subdirectory(platforms/sdl3)
endif()

# ---------------------------------------------------------------------------
# Tests: one executable per tests/test_*.c, no SDL required. Registered BEFORE
# add_subdirectory(examples) so examples/CMakeLists.txt can add link
# dependencies to a test target (test_pixel_room) that already exists.
# Built with the same build type; tests must not depend on assert() (Release
# defines NDEBUG).
#
# Which library a test links decides which profile it needs:
#   common tests        link pixelica_common only and never include pixelica.h
#   mixed common tests  (test_input, test_animation) have 3D integration
#                       sections. With PIXELICA_BUILD_3D they link pixelica_core and see
#                       PIXELICA_TEST_WITH_3D; without it they must guard those
#                       sections with #ifdef PIXELICA_TEST_WITH_3D (see test_util.h)
#   2D / hybrid tests   need PIXELICA_BUILD_2D / both profiles
#   every other test    is a 3D test and is omitted without PIXELICA_BUILD_3D
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  file(GLOB PIXELICA_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.c")
  if(NOT PIXELICA_TEST_SOURCES)
    message(WARNING "BUILD_TESTING is ON but no tests/test_*.c files were found")
  endif()
  set(PIXELICA_COMMON_TESTS test_math test_save test_screens test_i18n test_camera test_chunks test_framebuffer test_collision2d)
  set(PIXELICA_COMMON_MIXED_TESTS test_input test_animation)
  set(PIXELICA_RENDERER2D_TESTS test_renderer2d test_texture_options2d)
  set(PIXELICA_HYBRID_TESTS test_hybrid)
  # Tests that need code which only examples/ builds. With PIXELICA_BUILD_EXAMPLES=OFF
  # that code does not exist, so the test would not link: it is omitted.
  set(PIXELICA_EXAMPLE_TESTS test_pixel_room)
  foreach(test_source IN LISTS PIXELICA_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    set(test_libs "")
    set(test_with_3d OFF)
    set(test_omit_reason "")

    if(test_name IN_LIST PIXELICA_COMMON_TESTS)
      set(test_libs pixelica_common)
    elseif(test_name IN_LIST PIXELICA_COMMON_MIXED_TESTS)
      if(PIXELICA_BUILD_3D)
        set(test_libs pixelica_core) # PUBLIC-links pixelica_common
        set(test_with_3d ON)
      else()
        set(test_libs pixelica_common)
      endif()
    elseif(test_name STREQUAL "test_skeleton")
      if(NOT PIXELICA_BUILD_SKELETON)
        set(test_omit_reason "PIXELICA_BUILD_SKELETON=OFF")
      endif()
      set(test_libs pixelica_skeleton)
    elseif(test_name STREQUAL "test_ik")
      if(NOT PIXELICA_BUILD_IK)
        set(test_omit_reason "PIXELICA_BUILD_IK=OFF")
      endif()
      set(test_libs pixelica_ik)
    elseif(test_name STREQUAL "test_input_manager")
      # Common-only module: needs neither renderer, so it is omitted only when
      # the module itself is off (also with PIXELICA_BUILD_3D=OFF).
      if(NOT PIXELICA_BUILD_INPUT_MANAGER)
        set(test_omit_reason "PIXELICA_BUILD_INPUT_MANAGER=OFF")
      endif()
      set(test_libs pixelica_input_manager)
    elseif(test_name STREQUAL "test_debug" OR test_name MATCHES "^test_ui($|_)")
      set(test_libs pixelica_ui)
      if(test_name STREQUAL "test_ui_integration")
        if(TARGET pixelica_renderer2d)
          list(APPEND test_libs pixelica_renderer2d)
        endif()
        if(TARGET pixelica_core)
          list(APPEND test_libs pixelica_core)
          set(test_with_3d ON)
        endif()
      endif()
    elseif(test_name MATCHES "^test_shaders($|_)")
      if(NOT PIXELICA_BUILD_SHADERS)
        set(test_omit_reason "PIXELICA_BUILD_SHADERS=OFF")
      endif()
      set(test_libs pixelica_shaders)
      if(test_name STREQUAL "test_shaders_render")
        if(TARGET pixelica_renderer2d)
          list(APPEND test_libs pixelica_renderer2d)
        endif()
        if(TARGET pixelica_core)
          list(APPEND test_libs pixelica_core)
          set(test_with_3d ON)
        endif()
      endif()
    elseif(test_name STREQUAL "test_particles")
      if(NOT PIXELICA_BUILD_PARTICLES)
        set(test_omit_reason "PIXELICA_BUILD_PARTICLES=OFF")
      endif()
      set(test_libs pixelica_particles)
    elseif(test_name STREQUAL "test_particles_render" OR test_name STREQUAL "test_particles_safety")
      if(NOT TARGET pixelica_particles2d)
        set(test_omit_reason "particles rendering needs particles and the 2D renderer")
      endif()
      set(test_libs pixelica_particles2d)
      if(TARGET pixelica_particles25d)
        list(APPEND test_libs pixelica_particles25d)
      endif()
    elseif(test_name STREQUAL "test_audio")
      if(NOT PIXELICA_BUILD_AUDIO)
        set(test_omit_reason "PIXELICA_BUILD_AUDIO=OFF")
      endif()
      set(test_libs pixelica_audio)
    elseif(test_name STREQUAL "test_online")
      if(NOT PIXELICA_BUILD_ONLINE)
        set(test_omit_reason "PIXELICA_BUILD_ONLINE=OFF")
      endif()
      set(test_libs pixelica_online)
    elseif(test_name IN_LIST PIXELICA_RENDERER2D_TESTS)
      if(NOT PIXELICA_BUILD_2D)
        set(test_omit_reason "PIXELICA_BUILD_2D=OFF")
      endif()
      set(test_libs pixelica_renderer2d)
    elseif(test_name IN_LIST PIXELICA_HYBRID_TESTS)
      if(NOT PIXELICA_ENABLE_HYBRID)
        set(test_omit_reason "the hybrid module needs PIXELICA_BUILD_3D=ON and PIXELICA_BUILD_2D=ON")
      endif()
      set(test_libs pixelica_hybrid pixelica_renderer2d pixelica_core)
      set(test_with_3d ON)
    else()
      if(NOT PIXELICA_BUILD_3D)
        set(test_omit_reason "PIXELICA_BUILD_3D=OFF")
      endif()
      set(test_libs pixelica_core)
      set(test_with_3d ON)
    endif()
    if(test_name IN_LIST PIXELICA_EXAMPLE_TESTS AND NOT PIXELICA_BUILD_EXAMPLES)
      set(test_omit_reason "PIXELICA_BUILD_EXAMPLES=OFF (it links example code)")
    endif()

    if(NOT test_omit_reason STREQUAL "")
      if(test_omit_reason MATCHES "not guarded")
        message(WARNING "Pixelica: ${test_name} omitted because ${test_omit_reason}")
      else()
        message(STATUS "Pixelica: ${test_name} omitted because ${test_omit_reason}")
      endif()
      continue()
    endif()

    add_executable(${test_name} "${test_source}")
    if(test_name STREQUAL "test_ui_integration" AND TARGET pixelica_renderer2d)
      target_compile_definitions(${test_name} PRIVATE PIXELICA_TEST_UI2D=1)
    endif()
    if(test_name STREQUAL "test_shaders_render" AND TARGET pixelica_renderer2d)
      target_compile_definitions(${test_name} PRIVATE PIXELICA_TEST_SHADERS2D=1)
    endif()
    if((test_name STREQUAL "test_particles_render" OR test_name STREQUAL "test_particles_safety")
       AND TARGET pixelica_particles25d)
      target_compile_definitions(${test_name} PRIVATE PIXELICA_TEST_PARTICLES25D=1)
    endif()
    target_link_libraries(${test_name} PRIVATE ${test_libs} pixelica_options)
    target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    if(test_with_3d)
      target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
      target_compile_definitions(${test_name} PRIVATE PIXELICA_TEST_WITH_3D=1)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "unit")
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(PIXELICA_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# ESP32-P4 transport ownership test. Pure host C: compiles
# platforms/esp32p4/lcd_transport.c against a mocked port. It does NOT use
# ESP-IDF and says nothing about real hardware. Lives outside tests/ so the
# tests/test_*.c glob above never picks it up.
# ---------------------------------------------------------------------------
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/platforms/esp32p4/host_test/test_lcd_transport.c")
  add_executable(test_esp32p4_lcd_transport
    platforms/esp32p4/host_test/test_lcd_transport.c
    platforms/esp32p4/lcd_transport.c)
  target_include_directories(test_esp32p4_lcd_transport PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/platforms/esp32p4")
  target_link_libraries(test_esp32p4_lcd_transport PRIVATE pixelica_options)
  add_test(NAME esp32p4_lcd_transport COMMAND test_esp32p4_lcd_transport)
  set_tests_properties(esp32p4_lcd_transport PROPERTIES LABELS "unit;port")
endif()

# ---------------------------------------------------------------------------
# Guard used by the cmake -P test drivers below: they never delete anything and
# only create fresh run directories inside the build directory. This test proves
# the guard refuses roots, the source tree, escapes and symlinks. Pure CMake.
# ---------------------------------------------------------------------------
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaWorkDirSelfTest.cmake")
  add_test(NAME cmake_work_dir_safety
    COMMAND "${CMAKE_COMMAND}"
      "-DPIXELICA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DPIXELICA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaWorkDirSelfTest.cmake")
  set_tests_properties(cmake_work_dir_safety PROPERTIES LABELS "unit;cmake" TIMEOUT 120)
endif()

# ---------------------------------------------------------------------------
# Package-boundary check (pure CMake, no compiler): the explicit source/header
# lists match the tree, and common headers/sources, the 2D module and the SDL
# platform layer include no 3D header or 3D-internal file.
# ---------------------------------------------------------------------------
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaBoundaryCheck.cmake")
  add_test(NAME cmake_package_boundary
    COMMAND "${CMAKE_COMMAND}"
      "-DPIXELICA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaBoundaryCheck.cmake")
  set_tests_properties(cmake_package_boundary PROPERTIES LABELS "unit;cmake" TIMEOUT 120)
endif()

# ---------------------------------------------------------------------------
# Python tooling tests and asset reproducibility (tools/pixelica_pack, assets/), plus
# the SDK packaging script tests (tools/package_sdk.py). Skipped with a status
# message when Python 3 or the tools are absent, unless -DPIXELICA_REQUIRE_PYTHON=ON
# (CI does that). They do not depend on the 3D or 2D profile.
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  if(PIXELICA_REQUIRE_PYTHON)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
  else()
    find_package(Python3 3.8 QUIET COMPONENTS Interpreter)
  endif()

  if(Python3_Interpreter_FOUND)
    if(PIXELICA_BUILD_ONLINE AND Python3_VERSION VERSION_GREATER_EQUAL 3.10)
      add_executable(online_client_check tests/online_client.c)
      target_link_libraries(online_client_check PRIVATE pixelica_online pixelica_options)
      add_test(NAME online_client_integration
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/online_integration.py"
          --server "${CMAKE_CURRENT_SOURCE_DIR}/tools/online_server/server.py"
          --client "$<TARGET_FILE:online_client_check>")
      set_tests_properties(online_client_integration PROPERTIES LABELS "online;integration" TIMEOUT 100)
      add_test(NAME online_protocol_validation
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/online_protocol.py"
          --client "$<TARGET_FILE:online_client_check>")
      set_tests_properties(online_protocol_validation PROPERTIES LABELS "online;unit" TIMEOUT 120)
      add_test(NAME online_server_unittest
        COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
          -s "${CMAKE_CURRENT_SOURCE_DIR}/tools/online_server" -p "test*.py" -v)
      set_tests_properties(online_server_unittest PROPERTIES LABELS "online;python" TIMEOUT 120)
      if(TARGET hello_online_headless)
        add_test(NAME online_launcher_unittest
          COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/examples/hello_online/test_launch_local.py")
        set_tests_properties(online_launcher_unittest PROPERTIES LABELS "online;python" TIMEOUT 30)
        add_test(NAME online_demo_coop
          COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/examples/hello_online/launch_local.py"
            --exe "$<TARGET_FILE:hello_online_headless>" --server "${CMAKE_CURRENT_SOURCE_DIR}/tools/online_server/server.py"
            --smoke --mode coop --timeout 40)
        set_tests_properties(online_demo_coop PROPERTIES LABELS "online;integration" TIMEOUT 50)
        add_test(NAME online_score_retry
          COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/online_retry.py"
            --server "${CMAKE_CURRENT_SOURCE_DIR}/tools/online_server/server.py"
            --launcher "${CMAKE_CURRENT_SOURCE_DIR}/examples/hello_online/launch_local.py"
            --client "$<TARGET_FILE:hello_online_headless>")
        set_tests_properties(online_score_retry PROPERTIES LABELS "online;integration" TIMEOUT 55)
      endif()
    endif()
    set(PIXELICA_PY_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/pixelica_pack")
    set(PIXELICA_PY_TEST_DIR "" CACHE PATH "Directory unittest discovers tests in (empty = auto-detect)")
    if(NOT PIXELICA_PY_TEST_DIR)
      foreach(candidate "${PIXELICA_PY_TOOLS_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}/tests/python")
        if(IS_DIRECTORY "${candidate}")
          set(PIXELICA_PY_TEST_DIR_RESOLVED "${candidate}")
          break()
        endif()
      endforeach()
    else()
      set(PIXELICA_PY_TEST_DIR_RESOLVED "${PIXELICA_PY_TEST_DIR}")
    endif()

    if(PIXELICA_PY_TEST_DIR_RESOLVED)
      add_test(NAME python_unittest
        COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
                -s "${PIXELICA_PY_TEST_DIR_RESOLVED}" -p "test*.py" -v)
      set_tests_properties(python_unittest PROPERTIES
        LABELS "python"
        TIMEOUT 600
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${PIXELICA_PY_TOOLS_DIR}")
    else()
      message(STATUS "Pixelica: no Python test directory found (tools/pixelica_pack/tests or tests/python); python_unittest not registered")
      if(PIXELICA_REQUIRE_PYTHON)
        message(FATAL_ERROR "PIXELICA_REQUIRE_PYTHON=ON but no Python unittest directory exists")
      endif()
    endif()

    if(EXISTS "${PIXELICA_PY_TOOLS_DIR}/generate_demo_assets.py")
      add_test(NAME asset_reproducibility
        COMMAND "${CMAKE_COMMAND}"
          "-DPIXELICA_PYTHON=${Python3_EXECUTABLE}"
          "-DPIXELICA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
          "-DPIXELICA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
          "-DPIXELICA_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/asset-repro"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaAssetRepro.cmake")
      set_tests_properties(asset_reproducibility PROPERTIES LABELS "python;assets" TIMEOUT 600)
    else()
      message(STATUS "Pixelica: tools/pixelica_pack/generate_demo_assets.py not present; asset_reproducibility not registered")
      if(PIXELICA_REQUIRE_PYTHON)
        message(FATAL_ERROR "PIXELICA_REQUIRE_PYTHON=ON but tools/pixelica_pack/generate_demo_assets.py is missing")
      endif()
    endif()

    # tools/package_sdk.py: argument validation, archive members, manifest and
    # fresh-directory policy, on synthetic trees. No build, no network.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/package_sdk.py"
       AND IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/tests")
      add_test(NAME package_sdk_unittest
        COMMAND "${Python3_EXECUTABLE}" -B -I -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/tools/tests" -p "test_package*.py" -v
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
      set_tests_properties(package_sdk_unittest PROPERTIES
        LABELS "python;sdk"
        TIMEOUT 300
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PIXELICA_TOOLS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tools")
      add_test(NAME font_tool_unittest
        COMMAND "${Python3_EXECUTABLE}" -B -I -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/tools/tests" -p "test_pixelica_font.py" -v
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
      set_tests_properties(font_tool_unittest PROPERTIES LABELS "python;font" TIMEOUT 60)
    endif()
  else()
    message(STATUS "Pixelica: Python 3 not found; python_unittest, asset_reproducibility and package_sdk_unittest not registered")
  endif()
endif()

# ---------------------------------------------------------------------------
# Install / package export.
#
#   3D:  find_package(Pixelica CONFIG REQUIRED)       -> Pixelica::core (renderer3d)
#   2D:  find_package(Pixelica2D CONFIG REQUIRED)       -> Pixelica2D::core (renderer2d)
#   explicit: find_package(Pixelica CONFIG REQUIRED COMPONENTS renderer2d hybrid ...)
#
# One export set (and one *Targets.cmake file) per library; nothing is gated on
# build options inside the installed files, so a 2D-only and a 3D-only SDK can
# be unpacked into the same prefix and merged. Install components:
#   common      common library, common headers, package configs
#   core        COMPLETE 3D SDK: common + pixelica_core + all headers + configs +
#               starter template + asset tool (the original single component)
#   renderer2d  2D renderer, renderer2d.h, Pixelica2D package (needs `common`)
#   hybrid      2.5D module (opt-in; its use needs both SDKs in one prefix)
#   skeleton, ik, input_manager, online  optional modules (each needs `common`)
# `renderer3d` is not a separate install component: it is part of `core`, and
# the find_package component `renderer3d` is the same target as `core`.
# The SDL3 backend is never part of a package (docs/sdk.md).
# ---------------------------------------------------------------------------
if(PIXELICA_INSTALL)
  include(CMakePackageConfigHelpers)

  # -- common -----------------------------------------------------------------
  # The common export is installed by `common` AND (duplicated) by `core`, so
  # `cmake --install --component core` alone stays a working 3D SDK.
  install(TARGETS pixelica_common
    EXPORT PixelicaCommonTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT common
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT common
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT common
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(FILES ${PIXELICA_COMMON_HEADERS}
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pixelica"
    COMPONENT common)
  install(EXPORT PixelicaCommonTargets
    NAMESPACE Pixelica::
    FILE PixelicaCommonTargets.cmake
    DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
    COMPONENT common)
  foreach(pixelica_common_component common core)
    if(pixelica_common_component STREQUAL "core" AND NOT PIXELICA_BUILD_3D)
      continue()
    endif()
    install(FILES docs/save.md docs/screens.md docs/i18n.md docs/texture_options.md docs/camera.md docs/debug.md docs/chunks.md docs/material_layers.md docs/framebuffer.md docs/render_effects.md
      DESTINATION "${PIXELICA_INSTALL_SHAREDIR}/docs"
      COMPONENT ${pixelica_common_component})
    install(FILES tools/pixelica_font.py
      DESTINATION "${PIXELICA_INSTALL_SHAREDIR}/tools"
      COMPONENT ${pixelica_common_component})
  endforeach()
  if(PIXELICA_BUILD_3D)
    install(TARGETS pixelica_common
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT core
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT core
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT core)
    install(FILES ${PIXELICA_COMMON_HEADERS}
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pixelica"
      COMPONENT core)
    install(EXPORT PixelicaCommonTargets
      NAMESPACE Pixelica::
      FILE PixelicaCommonTargets.cmake
      DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
      COMPONENT core)
  endif()

  # -- 3D core (component core) -----------------------------------------------
  if(PIXELICA_BUILD_3D)
    install(TARGETS pixelica_core
      EXPORT PixelicaTargets
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT core
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT core
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT core
      INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(FILES ${PIXELICA_RENDERER3D_HEADERS}
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pixelica"
      COMPONENT core)
    install(EXPORT PixelicaTargets
      NAMESPACE Pixelica::
      FILE PixelicaTargets.cmake
      DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
      COMPONENT core)

    # Copyable starter game and (when present) the asset tool.
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/sdk/template")
      install(DIRECTORY sdk/template/
        DESTINATION "${PIXELICA_INSTALL_SHAREDIR}/template"
        COMPONENT core
        PATTERN "__pycache__" EXCLUDE)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/pixelica_pack/pixelica_pack.py")
      install(DIRECTORY tools/pixelica_pack
        DESTINATION "${PIXELICA_INSTALL_SHAREDIR}/tools"
        COMPONENT core
        PATTERN "__pycache__" EXCLUDE
        PATTERN "tests" EXCLUDE)
    endif()
  endif()

  # -- package configs --------------------------------------------------------
  # Identical for every build profile: they list no build options, only the
  # layout. The loader (PixelicaPackage.cmake) decides what a prefix offers from
  # which export files exist next to it.
  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaConfig.cmake"
    INSTALL_DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}")
  # Pre-1.0: a new minor version may change the API.
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
  set(PIXELICA_PACKAGE_CONFIG_FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PixelicaConfigVersion.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaPackage.cmake")
  install(FILES ${PIXELICA_PACKAGE_CONFIG_FILES}
    DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
    COMPONENT common)
  if(PIXELICA_BUILD_3D)
    install(FILES ${PIXELICA_PACKAGE_CONFIG_FILES}
      DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
      COMPONENT core)
  endif()

  # find_package(Pixelica2D): a thin wrapper that resolves the Pixelica package
  # beside itself in the same prefix (so it never picks an unrelated global
  # install). Part of the renderer2d component, which needs `common`.
  if(PIXELICA_BUILD_2D)
    configure_package_config_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Pixelica2DConfig.cmake.in"
      "${CMAKE_CURRENT_BINARY_DIR}/Pixelica2DConfig.cmake"
      INSTALL_DESTINATION "${PIXELICA_INSTALL_CMAKEDIR_2D}")
    write_basic_package_version_file(
      "${CMAKE_CURRENT_BINARY_DIR}/Pixelica2DConfigVersion.cmake"
      VERSION ${PROJECT_VERSION}
      COMPATIBILITY SameMinorVersion)
    install(FILES
      "${CMAKE_CURRENT_BINARY_DIR}/Pixelica2DConfig.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/Pixelica2DConfigVersion.cmake"
      DESTINATION "${PIXELICA_INSTALL_CMAKEDIR_2D}"
      COMPONENT renderer2d)
  endif()

  # Prove the installed packages work for copied, standalone projects. Needs an
  # unsanitized build (the templates are not built with the sanitizer runtime).
  set(PIXELICA_SDK_CHECK_COMMON_ARGS
    "-DPIXELICA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DPIXELICA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    "-DPIXELICA_CONFIG=$<CONFIG>"
    "-DPIXELICA_SDK_GENERATOR=${CMAKE_GENERATOR}"
    "-DPIXELICA_SDK_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DPIXELICA_SDK_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
    "-DPIXELICA_SDK_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DPIXELICA_SDK_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    "-DPIXELICA_INSTALL_CMAKEDIR=${PIXELICA_INSTALL_CMAKEDIR}"
    "-DPIXELICA_INSTALL_CMAKEDIR_2D=${PIXELICA_INSTALL_CMAKEDIR_2D}"
    "-DPIXELICA_INSTALL_SHAREDIR=${PIXELICA_INSTALL_SHAREDIR}"
    "-DPIXELICA_EXPECT_3D=${PIXELICA_BUILD_3D}"
    "-DPIXELICA_EXPECT_2D=${PIXELICA_BUILD_2D}"
    "-DPIXELICA_EXPECT_HYBRID=${PIXELICA_ENABLE_HYBRID}")

  if(BUILD_TESTING)
    if(PIXELICA_SANITIZE)
      message(STATUS "Pixelica: sdk_installed_template* not registered (PIXELICA_SANITIZE=ON)")
    elseif(CMAKE_CROSSCOMPILING)
      message(STATUS "Pixelica: sdk_installed_template* not registered (cross-compiling: the template build would need the same toolchain and an emulator)")
    else()
      add_test(NAME sdk_installed_ui
        COMMAND "${CMAKE_COMMAND}"
          ${PIXELICA_SDK_CHECK_COMMON_ARGS}
          "-DPIXELICA_SDK_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/sdk-check-ui"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaUISdkCheck.cmake")
      set_tests_properties(sdk_installed_ui PROPERTIES LABELS "sdk;ui" TIMEOUT 180)
      if(PIXELICA_BUILD_SHADERS)
        add_test(NAME sdk_installed_shaders
          COMMAND "${CMAKE_COMMAND}"
            ${PIXELICA_SDK_CHECK_COMMON_ARGS}
            "-DPIXELICA_SDK_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/sdk-check-shaders"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaShadersSdkCheck.cmake")
        set_tests_properties(sdk_installed_shaders PROPERTIES LABELS "sdk;shaders" TIMEOUT 180)
      endif()
      if(PIXELICA_BUILD_PARTICLES)
        add_test(NAME sdk_installed_particles
          COMMAND "${CMAKE_COMMAND}"
            ${PIXELICA_SDK_CHECK_COMMON_ARGS}
            "-DPIXELICA_SDK_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/sdk-check-particles"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaParticlesSdkCheck.cmake")
        set_tests_properties(sdk_installed_particles PROPERTIES LABELS "sdk;particles" TIMEOUT 180)
      endif()
      if(PIXELICA_BUILD_AUDIO)
        add_test(NAME sdk_installed_audio
          COMMAND "${CMAKE_COMMAND}"
            ${PIXELICA_SDK_CHECK_COMMON_ARGS}
            "-DPIXELICA_SDK_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/sdk-check-audio"
            "-DPIXELICA_AUDIO_SDL3_DIR=${SDL3_DIR}"
            "-DPIXELICA_AUDIO_HAS_SDL3=$<TARGET_EXISTS:pixelica_audio_sdl3>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaAudioSdkCheck.cmake")
        set_tests_properties(sdk_installed_audio PROPERTIES LABELS "sdk;audio" TIMEOUT 180)
      endif()
      # 3D starter template, only when the 3D renderer is built.
      if(PIXELICA_BUILD_3D AND IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/sdk/template")
        add_test(NAME sdk_installed_template
          COMMAND "${CMAKE_COMMAND}"
            ${PIXELICA_SDK_CHECK_COMMON_ARGS}
            "-DPIXELICA_SDK_PROFILE=3D"
            "-DPIXELICA_SDK_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/sdk-check"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaSdkCheck.cmake")
        set_tests_properties(sdk_installed_template PROPERTIES LABELS "sdk" TIMEOUT 900)
      elseif(NOT PIXELICA_BUILD_3D)
        message(STATUS "Pixelica: sdk_installed_template not registered (PIXELICA_BUILD_3D=OFF)")
      endif()
      # 2D starter template: registered as soon as sdk/template2d exists.
      if(PIXELICA_BUILD_2D AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdk/template2d/CMakeLists.txt")
        add_test(NAME sdk_installed_template2d
          COMMAND "${CMAKE_COMMAND}"
            ${PIXELICA_SDK_CHECK_COMMON_ARGS}
            "-DPIXELICA_SDK_PROFILE=2D"
            "-DPIXELICA_SDK_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/sdk-check-2d"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaSdkCheck.cmake")
        set_tests_properties(sdk_installed_template2d PROPERTIES LABELS "sdk" TIMEOUT 900)
      elseif(PIXELICA_BUILD_2D)
        message(STATUS "Pixelica: sdk_installed_template2d not registered (sdk/template2d/CMakeLists.txt does not exist yet; re-configure once it does)")
      endif()
    endif()
  endif()
endif()

# Package config semantics (component selection, required/optional handling,
# repeat calls, merged prefixes) against stub export files. The driver configures
# the real PixelicaConfig.cmake.in / Pixelica2DConfig.cmake.in templates itself in
# a scratch project, so it does not depend on the build profile or an install.
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaPackageConfigTest.cmake")
  add_test(NAME cmake_package_config
    COMMAND "${CMAKE_COMMAND}"
      "-DPIXELICA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DPIXELICA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPIXELICA_SDK_GENERATOR=${CMAKE_GENERATOR}"
      "-DPIXELICA_SDK_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
      "-DPIXELICA_INSTALL_CMAKEDIR=${PIXELICA_INSTALL_CMAKEDIR}"
      "-DPIXELICA_INSTALL_CMAKEDIR_2D=${PIXELICA_INSTALL_CMAKEDIR_2D}"
      "-DPIXELICA_INSTALL_SHAREDIR=${PIXELICA_INSTALL_SHAREDIR}"
      "-DPIXELICA_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-config-check"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PixelicaPackageConfigTest.cmake")
  set_tests_properties(cmake_package_config PROPERTIES LABELS "unit;cmake" TIMEOUT 300)
endif()
