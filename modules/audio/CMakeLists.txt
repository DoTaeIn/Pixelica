# Optional module: portable PCM audio mixer + WAV parser (shared by the 2D and
# 3D profiles). Added by the top-level CMakeLists.txt only when
# PIXELICA_BUILD_AUDIO=ON, after pixelica_common, GNUInstallDirs and PIXELICA_INSTALL_CMAKEDIR
# exist. Allocation-free portable C with no file I/O or SDL dependency, so it
# needs only the common library (Pixelica::common): it works in 2D-only,
# 3D-only and common-only builds. Exported as Pixelica::audio in its OWN export
# set (PixelicaAudioTargets.cmake, component "audio"). The SDL3 output adapter
# (platforms/sdl3/audio_sdl3.*) is a separate component.

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/audio.c")
  message(FATAL_ERROR "Pixelica audio module enabled but ${CMAKE_CURRENT_SOURCE_DIR}/audio.c does not exist")
endif()

add_library(pixelica_audio STATIC "${CMAKE_CURRENT_SOURCE_DIR}/audio.c")
add_library(Pixelica::audio ALIAS pixelica_audio)
set_target_properties(pixelica_audio PROPERTIES
  EXPORT_NAME audio
  DEBUG_POSTFIX d)
target_compile_features(pixelica_audio PUBLIC c_std_11)
target_include_directories(pixelica_audio PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# Same build-only warning/sanitizer flags as the other libraries; the installed
# target references neither pixelica_options nor any build-tree path. Its link to
# Pixelica::common (pixelica/status.h) is added by the installed package config,
# so the export set stays self-contained.
target_link_libraries(pixelica_audio PUBLIC
  "$<BUILD_INTERFACE:pixelica_common>"
  "$<BUILD_INTERFACE:pixelica_options>")

if(PIXELICA_INSTALL)
  install(TARGETS pixelica_audio
    EXPORT PixelicaAudioTargets
    COMPONENT audio
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT audio
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT audio
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT audio
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(DIRECTORY include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT audio
    FILES_MATCHING PATTERN "*.h")
  install(EXPORT PixelicaAudioTargets
    NAMESPACE Pixelica::
    FILE PixelicaAudioTargets.cmake
    DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
    COMPONENT audio)
endif()
