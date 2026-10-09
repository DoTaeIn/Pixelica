# Pixelica online starter: a standalone project that uses the optional "online"
# component of an installed Pixelica SDK (pixelica/online.h) and nothing else - no
# renderer, no window, no other Pixelica library. Copy this directory anywhere.
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<sdk install prefix>
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure
#   python launch_local.py --exe build/bin/online_game --smoke      # relay + 2 bots
#
# <prefix> is the directory that contains lib/cmake/Pixelica (the SDK archive
# extracted anywhere). The SDK must have been built with the online module
# (PIXELICA_BUILD_ONLINE=ON); otherwise find_package reports that the "online"
# component was not found.
#
# Platform libraries are rediscovered by the package config on your machine:
# WinHTTP on Windows, libcurl (+ Threads) on Linux. You never list them.
cmake_minimum_required(VERSION 3.21)

project(PixelicaOnlineGame VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(ONLINEGAME_STRICT_WARNINGS "Compile with strict warnings and treat them as errors" OFF)
option(ONLINEGAME_NETWORK_TESTS
  "Add a CTest case that starts the local relay and two bots through launch_local.py" OFF)
set(ONLINEGAME_SERVER_SCRIPT "" CACHE FILEPATH
  "online_server/server.py for the network test (default: searched by launch_local.py)")

find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS online)

add_executable(online_game src/main.c)
target_link_libraries(online_game PRIVATE Pixelica::online)
set_target_properties(online_game PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(ONLINEGAME_STRICT_WARNINGS)
  if(MSVC)
    target_compile_options(online_game PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(online_game PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
      -Wmissing-prototypes -Werror)
  endif()
endif()

enable_testing()

# Offline cases: no relay is needed for these.
add_test(NAME online_game_help COMMAND online_game --help)
set_tests_properties(online_game_help PROPERTIES PASS_REGULAR_EXPRESSION "usage:")

add_test(NAME online_game_needs_a_role COMMAND online_game)
set_tests_properties(online_game_needs_a_role PROPERTIES
  PASS_REGULAR_EXPRESSION "give exactly one of --host")

add_test(NAME online_game_rejects_bad_mode COMMAND online_game --host cooperative)
set_tests_properties(online_game_rejects_bad_mode PROPERTIES
  PASS_REGULAR_EXPRESSION "--host needs coop or versus")

# Nothing listens on the discard port: creating the room must FAIL cleanly
# (exit 1 with a message), not hang. Needs the library's error path only.
add_test(NAME online_game_unreachable_relay
  COMMAND online_game --host coop --server http://127.0.0.1:9 --timeout-ms 300)
set_tests_properties(online_game_unreachable_relay PROPERTIES
  PASS_REGULAR_EXPRESSION "cannot create the room" TIMEOUT 30)

# Optional end-to-end case (relay + host bot + guest bot).
if(ONLINEGAME_NETWORK_TESTS)
  find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
  set(_online_smoke_args
    "${CMAKE_CURRENT_SOURCE_DIR}/launch_local.py"
    --exe "$<TARGET_FILE:online_game>" --smoke)
  # find_package(Pixelica) defines Pixelica_ONLINE_SERVER when the SDK ships the relay.
  if(ONLINEGAME_SERVER_SCRIPT)
    list(APPEND _online_smoke_args --server "${ONLINEGAME_SERVER_SCRIPT}")
  elseif(Pixelica_ONLINE_SERVER)
    list(APPEND _online_smoke_args --server "${Pixelica_ONLINE_SERVER}")
  endif()
  add_test(NAME online_game_smoke COMMAND Python3::Interpreter ${_online_smoke_args})
  set_tests_properties(online_game_smoke PROPERTIES LABELS "network" TIMEOUT 120)
endif()
