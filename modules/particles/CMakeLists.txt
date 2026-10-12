# Optional module: fixed-pool particle simulation plus renderer adapters. Added
# by the top-level CMakeLists.txt only when PIXELICA_BUILD_PARTICLES=ON, after
# pixelica_common, the renderer modules, GNUInstallDirs and
# PIXELICA_INSTALL_CMAKEDIR exist.
#
#   Pixelica::particles    portable core (particles.c, pixelica/particles.h).
#                          Needs only Pixelica::common; always built. Never
#                          includes or links a renderer.
#   Pixelica::particles2d  2D adapter, only when pixelica_renderer2d exists.
#   Pixelica::particles25d 2.5D/3D adapter, only when pixelica_hybrid exists
#                          (which itself needs both the 3D and 2D renderers).
#
# Each library has its OWN export set and install component (particles /
# particles2d / particles25d) and installs only its own header, so a core-only
# install contains no adapter header. Dependencies are build-only links; the
# installed package config (cmake/PixelicaPackage.cmake) adds the links to
# Pixelica::common / Pixelica::particles / Pixelica::renderer2d /
# Pixelica::hybrid, so every export set stays self-contained.

set(PIXELICA_PARTICLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(pixelica_particles_require file)
  if(NOT EXISTS "${PIXELICA_PARTICLES_DIR}/${file}")
    message(FATAL_ERROR "Pixelica particles module enabled but ${PIXELICA_PARTICLES_DIR}/${file} does not exist")
  endif()
endfunction()

# Common target setup. deps are build-tree targets wrapped in BUILD_INTERFACE.
function(pixelica_particles_library target export_name source header component export_file)
  pixelica_particles_require("${source}")
  pixelica_particles_require("include/pixelica/${header}")
  add_library(${target} STATIC "${PIXELICA_PARTICLES_DIR}/${source}")
  add_library(Pixelica::${export_name} ALIAS ${target})
  set_target_properties(${target} PROPERTIES
    EXPORT_NAME ${export_name}
    DEBUG_POSTFIX d)
  target_compile_features(${target} PUBLIC c_std_11)
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${PIXELICA_PARTICLES_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  set(links "")
  foreach(dep IN LISTS ARGN)
    list(APPEND links "$<BUILD_INTERFACE:${dep}>")
  endforeach()
  # Same build-only warning/sanitizer flags as the other libraries; the
  # installed target references neither pixelica_options nor a build-tree path.
  target_link_libraries(${target} PUBLIC ${links} "$<BUILD_INTERFACE:pixelica_options>")

  if(PIXELICA_INSTALL)
    install(TARGETS ${target}
      EXPORT ${export_file}
      COMPONENT ${component}
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${component}
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${component}
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${component}
      INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    # One header per component: never install the whole include/ directory.
    install(FILES "${PIXELICA_PARTICLES_DIR}/include/pixelica/${header}"
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pixelica"
      COMPONENT ${component})
    install(EXPORT ${export_file}
      NAMESPACE Pixelica::
      FILE ${export_file}.cmake
      DESTINATION "${PIXELICA_INSTALL_CMAKEDIR}"
      COMPONENT ${component})
  endif()
endfunction()

pixelica_particles_library(pixelica_particles particles
  particles.c particles.h particles PixelicaParticlesTargets
  pixelica_common)

if(TARGET pixelica_renderer2d)
  pixelica_particles_library(pixelica_particles2d particles2d
    particles2d.c particles2d.h particles2d PixelicaParticles2DTargets
    pixelica_particles pixelica_renderer2d pixelica_common)
endif()

if(TARGET pixelica_hybrid)
  pixelica_particles_library(pixelica_particles25d particles25d
    particles25d.c particles25d.h particles25d PixelicaParticles25DTargets
    pixelica_particles pixelica_hybrid pixelica_common)
endif()
