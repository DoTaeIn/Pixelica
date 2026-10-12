cmake_minimum_required(VERSION 3.21)
project(PixelicaHelloUI LANGUAGES C)
find_package(PixelicaUI 0.1 CONFIG REQUIRED)
add_executable(hello_ui main.c)
target_link_libraries(hello_ui PRIVATE Pixelica::ui)
option(PIXELICA_UI_DEMO_SDL3 "Show the interactive menu using installed SDL3" OFF)
if(PIXELICA_UI_DEMO_SDL3)
  find_package(SDL3 3.4.18 CONFIG REQUIRED)
  target_compile_definitions(hello_ui PRIVATE HELLO_UI_SDL=1)
  target_link_libraries(hello_ui PRIVATE SDL3::SDL3)
  if(WIN32)
    add_custom_command(TARGET hello_ui POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E
        $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:hello_ui>>,copy_if_different,true>
        $<TARGET_RUNTIME_DLLS:hello_ui> $<TARGET_FILE_DIR:hello_ui>
      COMMAND_EXPAND_LISTS)
  endif()
endif()
enable_testing()
add_test(NAME hello_ui COMMAND hello_ui --smoke)
if(PIXELICA_UI_DEMO_SDL3)
  add_test(NAME hello_ui_sdl COMMAND hello_ui --hidden --frames 3)
  set_tests_properties(hello_ui_sdl PROPERTIES TIMEOUT 30)
endif()
