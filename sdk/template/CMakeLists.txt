# Pixelica starter game: a standalone project that uses Pixelica only through
# its public API and CMake package. Copy this directory anywhere and build it.
#
#   Installed package (default):
#     cmake -S . -B build -DCMAKE_PREFIX_PATH=<pixelica install prefix>
#     cmake --build build
#     ./build/bin/pixelica_game --frames 60
#
#   Source checkout (optional, also gives access to the SDL3 window front end):
#     cmake -S . -B build -DPIXELICA_GAME_USE_SOURCE_TREE=ON \
#           -DPIXELICA_GAME_PIXELICA_SOURCE_DIR=<pixelica checkout> [-DPIXELICA_GAME_WITH_SDL3=ON -DPIXELICA_FETCH_SDL3=ON]
cmake_minimum_required(VERSION 3.21)

project(PixelicaGame VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(PIXELICA_GAME_STRICT_WARNINGS "Compile the game with strict warnings and treat them as errors" OFF)
option(PIXELICA_GAME_USE_SOURCE_TREE "Build Pixelica from a source checkout instead of find_package" OFF)
set(PIXELICA_GAME_PIXELICA_SOURCE_DIR "" CACHE PATH "Pixelica checkout used when PIXELICA_GAME_USE_SOURCE_TREE=ON")
option(PIXELICA_GAME_WITH_SDL3 "Source-tree mode only: also build the SDL3 window front end" OFF)

if(PIXELICA_GAME_USE_SOURCE_TREE)
  if(NOT EXISTS "${PIXELICA_GAME_PIXELICA_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "PIXELICA_GAME_PIXELICA_SOURCE_DIR must point at a Pixelica checkout")
  endif()
  set(PIXELICA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(PIXELICA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(PIXELICA_INSTALL OFF CACHE BOOL "" FORCE)
  # This template is a 3D game: build the 3D renderer and skip the 2D one.
  set(PIXELICA_BUILD_3D ON CACHE BOOL "" FORCE)
  set(PIXELICA_BUILD_2D OFF CACHE BOOL "" FORCE)
  if(PIXELICA_GAME_WITH_SDL3)
    set(PIXELICA_PLATFORM SDL3 CACHE STRING "" FORCE)
  else()
    set(PIXELICA_PLATFORM HEADLESS CACHE STRING "" FORCE)
  endif()
  add_subdirectory("${PIXELICA_GAME_PIXELICA_SOURCE_DIR}" pixelica) # provides Pixelica::core
else()
  if(PIXELICA_GAME_WITH_SDL3)
    message(FATAL_ERROR "PIXELICA_GAME_WITH_SDL3 needs PIXELICA_GAME_USE_SOURCE_TREE=ON: the installed package is headless only")
  endif()
  # Ask for the 3D core explicitly: an SDK prefix may also hold the 2D renderer
  # (merged 2D + 3D SDKs), and only the requested components are imported.
  find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS core)
endif()

function(pixelica_game_target_options target)
  if(PIXELICA_GAME_STRICT_WARNINGS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
    else()
      target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
        -Wmissing-prototypes -Werror)
    endif()
  endif()
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()

# Portable game logic (no platform code) + a headless driver.
add_library(game_logic STATIC src/game.c)
target_include_directories(game_logic PUBLIC src)
target_link_libraries(game_logic PUBLIC Pixelica::core)
pixelica_game_target_options(game_logic)

add_executable(pixelica_game src/main_headless.c)
target_link_libraries(pixelica_game PRIVATE game_logic)
pixelica_game_target_options(pixelica_game)

if(PIXELICA_GAME_USE_SOURCE_TREE AND PIXELICA_GAME_WITH_SDL3)
  add_executable(pixelica_game_sdl3 src/main_sdl3.c)
  target_link_libraries(pixelica_game_sdl3 PRIVATE game_logic pixelica_platform_sdl3)
  pixelica_game_target_options(pixelica_game_sdl3)
endif()

enable_testing()
add_test(NAME game_runs COMMAND pixelica_game --frames 30)
set_tests_properties(game_runs PROPERTIES PASS_REGULAR_EXPRESSION "frames=30 presented=30")
add_test(NAME game_rejects_bad_frames COMMAND pixelica_game --frames abc)
set_tests_properties(game_rejects_bad_frames PROPERTIES WILL_FAIL TRUE)
