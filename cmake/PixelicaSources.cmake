# Explicit source and header lists for the 2D / 3D package split.
#
# include() this file; it defines variables and functions only, so it is valid in
# CMake script mode too (the ESP-IDF component wrappers read it while IDF scans
# component requirements). Everything is relative to src/ or include/pixelica/.
#
# Library layout (one definition of every function in a link line):
#   pixelica_common   (Pixelica::common)  the 5 files below, no 3D renderer code
#   pixelica_core     (Pixelica::core, ::renderer3d)  the remaining 7 files
#   modules/renderer2d, modules/hybrid, modules/skeleton, modules/ik live
#   outside src/ and are listed by their own CMakeLists.txt.

set(PIXELICA_COMMON_SOURCE_NAMES
  animation/animation.c
  animation/anim_clip.c
  assets/container.c
  assets/texture_asset.c
  game/app.c
  game/save.c
  game/storage.c
  game/storage_fs.c
  game/screens.c
  game/i18n.c
  game/font.c
  game/font_korean.c
  game/camera.c
  game/debug.c
  game/chunks.c
  game/framebuffer.c
  game/collision2d.c
  input/input.c
  light/bloom.c
  light/light_mask.c
  math/mat4.c
  math/quat.c)

set(PIXELICA_RENDERER3D_SOURCE_NAMES
  assets/cart.c
  assets/mesh_asset.c
  core/context.c
  core/framebuffer.c
  renderer/light_draw.c
  renderer/pipeline.c
  renderer/rasterizer.c
  scene/aabb.c
  scene/scene.c)

# Public headers in include/pixelica/. Common headers are installed by every SDK
# profile; the 3D headers only by the 3D profile. The 2D and hybrid headers stay
# in their module include directories.
set(PIXELICA_COMMON_HEADER_NAMES
  status.h
  input.h
  platform.h
  math.h
  animation.h
  anim_clip.h
  image.h
  draw_options.h
  assets_common.h
  app.h
  save.h
  storage.h
  screens.h
  i18n.h
  font.h
  camera.h
  debug.h
  chunks.h
  framebuffer.h
  light.h
  collision2d.h)

set(PIXELICA_RENDERER3D_HEADER_NAMES
  pixelica.h
  renderer.h
  assets.h
  scene.h)

# Fails (FATAL_ERROR) when the lists and the tree disagree:
#   - a listed file does not exist,
#   - a file is in both lists of one kind,
#   - a src/**/*.c or include/pixelica/*.h file is in neither list (so a new file can
#     not silently be left out of, or duplicated into, a library).
# Plain GLOB (no CONFIGURE_DEPENDS) so it also works in script mode; the callers
# that need re-checks on file addition re-run it on every configure.
function(pixelica_check_source_lists root)
  foreach(name IN LISTS PIXELICA_COMMON_SOURCE_NAMES PIXELICA_RENDERER3D_SOURCE_NAMES)
    if(NOT EXISTS "${root}/src/${name}")
      message(FATAL_ERROR "Pixelica: listed source src/${name} does not exist (cmake/PixelicaSources.cmake)")
    endif()
  endforeach()
  foreach(name IN LISTS PIXELICA_COMMON_HEADER_NAMES PIXELICA_RENDERER3D_HEADER_NAMES)
    if(NOT EXISTS "${root}/include/pixelica/${name}")
      message(FATAL_ERROR "Pixelica: listed header include/pixelica/${name} does not exist (cmake/PixelicaSources.cmake)")
    endif()
  endforeach()

  foreach(name IN LISTS PIXELICA_COMMON_SOURCE_NAMES)
    if(name IN_LIST PIXELICA_RENDERER3D_SOURCE_NAMES)
      message(FATAL_ERROR "Pixelica: src/${name} is listed as both common and 3D (duplicate symbols in the link)")
    endif()
  endforeach()
  foreach(name IN LISTS PIXELICA_COMMON_HEADER_NAMES)
    if(name IN_LIST PIXELICA_RENDERER3D_HEADER_NAMES)
      message(FATAL_ERROR "Pixelica: include/pixelica/${name} is listed as both common and 3D")
    endif()
  endforeach()

  file(GLOB_RECURSE found_sources RELATIVE "${root}/src" "${root}/src/*.c")
  foreach(name IN LISTS found_sources)
    if(NOT name IN_LIST PIXELICA_COMMON_SOURCE_NAMES AND NOT name IN_LIST PIXELICA_RENDERER3D_SOURCE_NAMES)
      message(FATAL_ERROR
        "Pixelica: src/${name} exists but is in neither PIXELICA_COMMON_SOURCE_NAMES nor "
        "PIXELICA_RENDERER3D_SOURCE_NAMES (cmake/PixelicaSources.cmake)")
    endif()
  endforeach()
  file(GLOB found_headers RELATIVE "${root}/include/pixelica" "${root}/include/pixelica/*.h")
  foreach(name IN LISTS found_headers)
    if(NOT name IN_LIST PIXELICA_COMMON_HEADER_NAMES AND NOT name IN_LIST PIXELICA_RENDERER3D_HEADER_NAMES)
      message(FATAL_ERROR
        "Pixelica: include/pixelica/${name} exists but is in neither PIXELICA_COMMON_HEADER_NAMES nor "
        "PIXELICA_RENDERER3D_HEADER_NAMES (cmake/PixelicaSources.cmake)")
    endif()
  endforeach()
endfunction()

# <out_var> = the .c files directly in <module_dir> and <module_dir>/src, sorted,
# absolute. Used for modules/renderer2d and modules/hybrid, whose file names are
# owned by the engine code, not by the build files. Empty when there are none.
function(pixelica_collect_module_sources out_var module_dir)
  file(GLOB module_sources "${module_dir}/*.c" "${module_dir}/src/*.c")
  list(SORT module_sources)
  set(${out_var} "${module_sources}" PARENT_SCOPE)
endfunction()
