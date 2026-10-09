# Firmware application: app_main + the board hook + the selected example game.
# The game sources are the SAME files the desktop builds use; nothing in them
# knows about ESP-IDF.
#
# Also executed once in ESP-IDF's early requirements script (CMake -P mode), where
# CONFIG_* values may be unset; only SRCS/REQUIRES-relevant logic lives above
# idf_component_register(), and none of it uses commands unavailable in script mode.
#
# STATUS: compiled with ESP-IDF v6.0; not flashed or run on a board.

if(DEFINED ENV{PIXELICA_ROOT} AND NOT "$ENV{PIXELICA_ROOT}" STREQUAL "")
  get_filename_component(PIXELICA_ROOT "$ENV{PIXELICA_ROOT}" ABSOLUTE)
else()
  get_filename_component(PIXELICA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()
if(NOT EXISTS "${PIXELICA_ROOT}/examples/common/pixelica_game.h")
  message(FATAL_ERROR "firmware main: '${PIXELICA_ROOT}' is not a Pixelica checkout "
                      "(examples/common/pixelica_game.h missing). Set PIXELICA_ROOT.")
endif()

set(PIXELICA_APP_SOURCES app_main.c board_config.c)
set(PIXELICA_APP_INCLUDES "${PIXELICA_ROOT}/examples/common")
set(PIXELICA_APP_DEFINES "")

if(CONFIG_PIXELICA_FW_GAME_PIXEL_ROOM)
  # Pixel Room: the desktop's own sources. pixel_room_game.c is the PIXELICA_Game; the
  # size comes from PIXEL_ROOM_WIDTH/HEIGHT (same mechanism as the pixel_room /
  # pixel_room_tiny desktop targets). Assets are embedded byte arrays.
  set(room_dir "${PIXELICA_ROOT}/examples/pixel_room")
  set(assets_dir "${PIXELICA_ROOT}/assets/compiled")
  foreach(room_file
      "${room_dir}/pixel_room.c" "${room_dir}/pixel_room_world.c"
      "${room_dir}/pixel_room_game.c" "${room_dir}/room_embedded.c")
    if(NOT EXISTS "${room_file}")
      message(FATAL_ERROR "firmware main: ${room_file} is missing")
    endif()
    list(APPEND PIXELICA_APP_SOURCES "${room_file}")
  endforeach()
  if(NOT EXISTS "${assets_dir}/pixel_room_assets.h")
    message(FATAL_ERROR "firmware main: ${assets_dir}/pixel_room_assets.h is missing. "
                        "Generate it first: python tools/pixelica_pack/generate_demo_assets.py")
  endif()
  list(APPEND PIXELICA_APP_INCLUDES "${room_dir}" "${assets_dir}")
  if(CONFIG_PIXELICA_FW_PROFILE_STANDARD)
    list(APPEND PIXELICA_APP_DEFINES PIXEL_ROOM_WIDTH=256 PIXEL_ROOM_HEIGHT=192)
  else()
    list(APPEND PIXELICA_APP_DEFINES PIXEL_ROOM_WIDTH=160 PIXEL_ROOM_HEIGHT=120)
  endif()
else()
  list(APPEND PIXELICA_APP_SOURCES "${PIXELICA_ROOT}/examples/hello_triangle/hello_triangle.c")
endif()

idf_component_register(
  SRCS ${PIXELICA_APP_SOURCES}
  INCLUDE_DIRS "." ${PIXELICA_APP_INCLUDES}
  PRIV_REQUIRES pixelica esp_timer esp_driver_gpio esp_hw_support heap freertos log)

# Not reached in ESP-IDF's early requirements script (idf_component_register()
# returns there), so target commands are safe from here on.
if(PIXELICA_APP_DEFINES)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ${PIXELICA_APP_DEFINES})
endif()
# No -std override: ESP-IDF v6.0 compiles C with -std=gnu23 and its headers are
# validated with that; the example sources are plain C11.
