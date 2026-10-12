# Standalone build of the editor's native bridge:
#   cmake -S tools/editor/native -B build-editor
#   cmake --build build-editor --config Release
#   ctest --test-dir build-editor -C Release
#
# Builds the repository's engine through add_subdirectory with only the 2D
# renderer (headless, no examples/tests/install, position-independent code) and
# links it into one shared library, pixelica_editor_bridge, exporting only the
# pe_* C ABI from bridge.h. The engine sources and root CMakeLists.txt are used
# unchanged.
cmake_minimum_required(VERSION 3.21)
project(PixelicaEditorBridge VERSION 0.1.0 LANGUAGES C)

option(PIXELICA_EDITOR_WARNINGS_AS_ERRORS "Treat warnings in the bridge and engine as errors" OFF)

get_filename_component(PIXELICA_EDITOR_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
if(NOT EXISTS "${PIXELICA_EDITOR_REPO_ROOT}/modules/renderer2d/include/pixelica/renderer2d.h")
  message(FATAL_ERROR "Pixelica repository not found at ${PIXELICA_EDITOR_REPO_ROOT}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# The engine's static libraries end up inside a shared library.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Engine profile for the editor. FORCE: these are requirements, not defaults.
set(PIXELICA_PLATFORM HEADLESS CACHE STRING "" FORCE)
set(PIXELICA_BUILD_2D ON CACHE BOOL "" FORCE)
set(PIXELICA_BUILD_3D OFF CACHE BOOL "" FORCE)
set(PIXELICA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PIXELICA_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(PIXELICA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PIXELICA_INSTALL OFF CACHE BOOL "" FORCE)
set(PIXELICA_SANITIZE OFF CACHE BOOL "" FORCE)
set(PIXELICA_WARNINGS_AS_ERRORS ${PIXELICA_EDITOR_WARNINGS_AS_ERRORS} CACHE BOOL "" FORCE)

add_subdirectory("${PIXELICA_EDITOR_REPO_ROOT}" pixelica EXCLUDE_FROM_ALL)
if(NOT TARGET pixelica_renderer2d)
  message(FATAL_ERROR "The Pixelica 2D renderer was not configured; the editor bridge needs it")
endif()

# Outputs land in build-editor/ (single-config) or build-editor/<Config>/
# (multi-config), where runtime.py and the launcher look for them. The test
# executable sits next to the DLL so Windows can load it.
set(PIXELICA_EDITOR_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

add_library(pixelica_editor_bridge SHARED bridge.c bridge.h)
target_compile_definitions(pixelica_editor_bridge PRIVATE PE_BRIDGE_BUILD=1)
target_include_directories(pixelica_editor_bridge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pixelica_editor_bridge PRIVATE pixelica_renderer2d PUBLIC pixelica_common)
set_target_properties(pixelica_editor_bridge PROPERTIES
  PREFIX ""
  DEBUG_POSTFIX ""
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  RUNTIME_OUTPUT_DIRECTORY "${PIXELICA_EDITOR_OUTPUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${PIXELICA_EDITOR_OUTPUT_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${PIXELICA_EDITOR_OUTPUT_DIR}/lib")
if(MSVC)
  target_compile_options(pixelica_editor_bridge PRIVATE /W4)
else()
  target_compile_options(pixelica_editor_bridge PRIVATE -Wall -Wextra -Wconversion)
endif()
if(PIXELICA_EDITOR_WARNINGS_AS_ERRORS)
  if(MSVC)
    target_compile_options(pixelica_editor_bridge PRIVATE /WX)
  else()
    target_compile_options(pixelica_editor_bridge PRIVATE -Werror)
  endif()
endif()

enable_testing()
add_executable(test_bridge test_bridge.c)
target_link_libraries(test_bridge PRIVATE pixelica_editor_bridge)
set_target_properties(test_bridge PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PIXELICA_EDITOR_OUTPUT_DIR}")
if(PIXELICA_EDITOR_WARNINGS_AS_ERRORS)
  if(MSVC)
    target_compile_options(test_bridge PRIVATE /W4 /WX)
  else()
    target_compile_options(test_bridge PRIVATE -Wall -Wextra -Werror)
  endif()
endif()
if(UNIX)
  target_link_libraries(test_bridge PRIVATE m)
endif()
add_test(NAME editor_bridge COMMAND test_bridge)
set_tests_properties(editor_bridge PROPERTIES LABELS "editor;unit" TIMEOUT 60)
