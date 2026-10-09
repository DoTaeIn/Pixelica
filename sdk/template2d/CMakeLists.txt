# Pixelica2D starter game: a standalone project that uses Pixelica2D only through
# its installed CMake package and public headers (pixelica/renderer2d.h). It needs
# no 3D renderer, no 3D headers and no Pixelica core archive. Copy this
# directory anywhere and build it.
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<pixelica2d install prefix>
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#   ./build/bin/pixelica2d_game --frames 60 --capture frame.ppm
#
# <prefix> is the directory that contains lib/cmake/Pixelica2D and
# lib/cmake/Pixelica (the 2D SDK archive extracted anywhere).
cmake_minimum_required(VERSION 3.21)

project(Pixelica2DGame VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(PIXELICA2D_GAME_STRICT_WARNINGS "Compile the game with strict warnings and treat them as errors" OFF)

find_package(Pixelica2D 0.1 CONFIG REQUIRED)

function(pixelica2d_game_target_options target)
  if(PIXELICA2D_GAME_STRICT_WARNINGS)
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

# Portable game logic (no platform code, no 3D).
add_library(game_logic STATIC src/game.c)
target_include_directories(game_logic PUBLIC src)
target_link_libraries(game_logic PUBLIC Pixelica2D::core)
pixelica2d_game_target_options(game_logic)

# Headless driver: scripted input, one PAL present per frame, optional PPM capture.
add_executable(pixelica2d_game src/main_headless.c)
target_link_libraries(pixelica2d_game PRIVATE game_logic)
pixelica2d_game_target_options(pixelica2d_game)

enable_testing()

# Success: a finite run renders and presents every frame.
add_test(NAME game_runs COMMAND pixelica2d_game --frames 30)
set_tests_properties(game_runs PROPERTIES PASS_REGULAR_EXPRESSION "frames=30 presented=30")

# The same input must give the same frame hash (deterministic simulation).
add_test(NAME game_is_deterministic
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:pixelica2d_game>
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_deterministic.cmake")

# Capture writes a PPM that can be opened to look at the frame.
add_test(NAME game_captures_ppm
  COMMAND pixelica2d_game --frames 20 --capture "${CMAKE_CURRENT_BINARY_DIR}/capture.ppm")
set_tests_properties(game_captures_ppm PROPERTIES PASS_REGULAR_EXPRESSION "frames=20 presented=20")

# Invalid arguments: non-zero exit (WILL_FAIL) and a useful message.
add_test(NAME game_rejects_bad_frames COMMAND pixelica2d_game --frames abc)
set_tests_properties(game_rejects_bad_frames PROPERTIES WILL_FAIL TRUE)
add_test(NAME game_rejects_zero_frames COMMAND pixelica2d_game --frames 0)
set_tests_properties(game_rejects_zero_frames PROPERTIES WILL_FAIL TRUE)
add_test(NAME game_rejects_missing_capture_path COMMAND pixelica2d_game --capture)
set_tests_properties(game_rejects_missing_capture_path PROPERTIES WILL_FAIL TRUE)
add_test(NAME game_bad_frames_message COMMAND pixelica2d_game --frames abc)
set_tests_properties(game_bad_frames_message PROPERTIES
  PASS_REGULAR_EXPRESSION "--frames needs an integer")
