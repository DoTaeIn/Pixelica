# Shared component loader of the Pixelica / Pixelica2D packages. It is include()d
# by PixelicaConfig.cmake and by the Pixelica2DConfig.cmake wrapper and is
# installed verbatim next to the *Targets.cmake files. It is NOT configured: it
# contains no build-time value, so every SDK variant installs the same bytes and
# SDKs built with different profiles (2D-only, 3D-only) can be merged into one
# prefix.
#
# Inputs, set by the including config file:
#   _pixelica_pkg_name    "Pixelica" or "Pixelica2D": the name find_package() was called
#                    with (selects <name>_FIND_COMPONENTS, <name>_<c>_FOUND, ...)
#   _pixelica_pkg_dir     directory holding the Pixelica *Targets.cmake files
#   _pixelica_pkg_prefix  install prefix (for the tools / template paths)
#   _pixelica_pkg_share   <prefix>/share/Pixelica
#
# What a prefix offers is decided ONLY by which export files exist, never by the
# options the library was built with:
#
#   component   export file                       imported target(s)
#   common      PixelicaCommonTargets.cmake       Pixelica::common
#   core        PixelicaTargets.cmake             Pixelica::core, ::renderer3d
#   renderer3d  (same file as core)               Pixelica::core, ::renderer3d
#   renderer2d  PixelicaRenderer2DTargets.cmake   Pixelica::renderer2d, Pixelica2D::core
#   hybrid      PixelicaHybridTargets.cmake       Pixelica::hybrid (needs core + renderer2d)
#   skeleton    PixelicaSkeletonTargets.cmake     Pixelica::skeleton (needs common)
#   ik          PixelicaIKTargets.cmake           Pixelica::ik (needs common)
#   input_manager  PixelicaInputManagerTargets.cmake  Pixelica::input_manager (needs common)
#   online      PixelicaOnlineTargets.cmake       Pixelica::online (common + OS HTTPS/threads)
#
# Pixelica with no COMPONENTS: core if its export exists, else renderer2d, else
# common. Pixelica2D with no COMPONENTS: renderer2d (Pixelica2D::core); its
# COMPONENTS may be core (= renderer2d), renderer2d, common and, only when
# requested explicitly, input_manager and online (the same Pixelica:: targets
# as through find_package(Pixelica); shared by the 2D and 3D profiles).
# Only the requested components and their dependencies are imported.
# A required component that is missing or unknown makes find_package() fail; an
# OPTIONAL_COMPONENTS entry that is missing does not. Calling find_package()
# again, also with other components, only imports what is still missing.

# component -> list: <export file>;<target>;<dependency components>... (the
# dependencies are the list elements from index 2 on; empty result = unknown).
function(_pixelica_pkg_describe comp out_var)
  if(comp STREQUAL "common")
    set(${out_var} "PixelicaCommonTargets.cmake;Pixelica::common" PARENT_SCOPE)
  elseif(comp STREQUAL "core" OR comp STREQUAL "renderer3d")
    set(${out_var} "PixelicaTargets.cmake;Pixelica::core;common" PARENT_SCOPE)
  elseif(comp STREQUAL "renderer2d")
    set(${out_var} "PixelicaRenderer2DTargets.cmake;Pixelica::renderer2d;common" PARENT_SCOPE)
  elseif(comp STREQUAL "hybrid")
    set(${out_var} "PixelicaHybridTargets.cmake;Pixelica::hybrid;core;renderer2d" PARENT_SCOPE)
  elseif(comp STREQUAL "skeleton")
    set(${out_var} "PixelicaSkeletonTargets.cmake;Pixelica::skeleton;common" PARENT_SCOPE)
  elseif(comp STREQUAL "ik")
    set(${out_var} "PixelicaIKTargets.cmake;Pixelica::ik;common" PARENT_SCOPE)
  elseif(comp STREQUAL "input_manager")
    set(${out_var} "PixelicaInputManagerTargets.cmake;Pixelica::input_manager;common" PARENT_SCOPE)
  elseif(comp STREQUAL "online")
    set(${out_var} "PixelicaOnlineTargets.cmake;Pixelica::online;common" PARENT_SCOPE)
  else()
    set(${out_var} "" PARENT_SCOPE)
  endif()
endfunction()

# alias <- real, as an ALIAS where CMake allows aliases of imported targets and
# as an INTERFACE wrapper otherwise. Does nothing when <alias> already exists.
function(_pixelica_pkg_alias alias real)
  if(TARGET ${alias} OR NOT TARGET ${real})
    return()
  endif()
  if(NOT CMAKE_VERSION VERSION_LESS 3.18)
    add_library(${alias} ALIAS ${real})
  else()
    add_library(${alias} INTERFACE IMPORTED)
    set_property(TARGET ${alias} PROPERTY INTERFACE_LINK_LIBRARIES ${real})
  endif()
endfunction()

# Imports <comp> and its dependencies. <out_var> = TRUE when the component's
# target exists afterwards. Safe to call repeatedly.
function(_pixelica_pkg_load comp out_var)
  set(${out_var} FALSE PARENT_SCOPE)
  _pixelica_pkg_describe("${comp}" info)
  if(info STREQUAL "")
    return() # unknown component
  endif()
  list(GET info 0 export_file)
  list(GET info 1 target)
  list(LENGTH info info_length)
  set(deps "")
  if(info_length GREATER 2)
    list(SUBLIST info 2 -1 deps)
  endif()

  foreach(dep IN LISTS deps)
    _pixelica_pkg_load("${dep}" dep_ok)
    if(NOT dep_ok)
      return()
    endif()
  endforeach()

  if(NOT TARGET ${target})
    if(NOT EXISTS "${_pixelica_pkg_dir}/${export_file}")
      return()
    endif()
    if(comp STREQUAL "online" AND NOT WIN32)
      # Dependencies belong only to this explicitly requested optional module.
      find_package(Threads QUIET)
      find_package(CURL 7.68 QUIET)
      if(NOT Threads_FOUND OR NOT CURL_FOUND)
        return()
      endif()
    endif()
    include("${_pixelica_pkg_dir}/${export_file}")
    if(NOT TARGET ${target})
      return()
    endif()
    # The export files deliberately do not name other export sets (that is what
    # keeps each one standalone): connect the imported target to its
    # dependencies here, once, right after it was created.
    foreach(dep IN LISTS deps)
      _pixelica_pkg_describe("${dep}" dep_info)
      list(GET dep_info 1 dep_target)
      set_property(TARGET ${target} APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${dep_target}")
    endforeach()
  endif()

  if(target STREQUAL "Pixelica::core")
    _pixelica_pkg_alias(Pixelica::renderer3d Pixelica::core)
  elseif(target STREQUAL "Pixelica::renderer2d")
    _pixelica_pkg_alias(Pixelica2D::core Pixelica::renderer2d)
  endif()
  set(${out_var} TRUE PARENT_SCOPE)
endfunction()

set(_pixelica_requested "${${_pixelica_pkg_name}_FIND_COMPONENTS}")
set(_pixelica_selected "")

if(NOT _pixelica_requested)
  # Default selection.
  if(_pixelica_pkg_name STREQUAL "Pixelica2D")
    set(_pixelica_selected renderer2d)
  elseif(EXISTS "${_pixelica_pkg_dir}/PixelicaTargets.cmake")
    set(_pixelica_selected core)
  elseif(EXISTS "${_pixelica_pkg_dir}/PixelicaRenderer2DTargets.cmake")
    set(_pixelica_selected renderer2d)
  else()
    set(_pixelica_selected common)
  endif()
  _pixelica_pkg_load(${_pixelica_selected} _pixelica_default_ok)
  if(_pixelica_default_ok)
    set(${_pixelica_pkg_name}_${_pixelica_selected}_FOUND TRUE)
    if(_pixelica_pkg_name STREQUAL "Pixelica2D")
      set(Pixelica2D_core_FOUND TRUE)
    endif()
  else()
    set(${_pixelica_pkg_name}_FOUND FALSE)
    set(${_pixelica_pkg_name}_NOT_FOUND_MESSAGE
      "${_pixelica_pkg_name}: the install prefix of ${CMAKE_CURRENT_LIST_FILE} has no usable '${_pixelica_selected}' export (install the matching SDK or component)")
  endif()
  unset(_pixelica_default_ok)
else()
  foreach(_pixelica_component IN LISTS _pixelica_requested)
    set(_pixelica_actual "${_pixelica_component}")
    if(_pixelica_pkg_name STREQUAL "Pixelica2D")
      # The Pixelica2D package offers the 2D renderer as "core" plus "common", and
      # the shared optional input_manager module when asked for by name.
      if(_pixelica_component STREQUAL "core")
        set(_pixelica_actual renderer2d)
      elseif(NOT _pixelica_component STREQUAL "renderer2d" AND NOT _pixelica_component STREQUAL "common"
             AND NOT _pixelica_component STREQUAL "input_manager"
             AND NOT _pixelica_component STREQUAL "online")
        set(_pixelica_actual "")
      endif()
    endif()
    set(${_pixelica_pkg_name}_${_pixelica_component}_FOUND FALSE)
    if(NOT _pixelica_actual STREQUAL "")
      _pixelica_pkg_load(${_pixelica_actual} _pixelica_component_ok)
      if(_pixelica_component_ok)
        set(${_pixelica_pkg_name}_${_pixelica_component}_FOUND TRUE)
      endif()
    endif()
  endforeach()
  unset(_pixelica_component)
  unset(_pixelica_actual)
  unset(_pixelica_component_ok)
endif()

# Legacy helper variables (empty when the file or directory is not installed).
set(Pixelica_PACK "")
if(EXISTS "${_pixelica_pkg_share}/tools/pixelica_pack/pixelica_pack.py")
  set(Pixelica_PACK "${_pixelica_pkg_share}/tools/pixelica_pack/pixelica_pack.py")
endif()
set(Pixelica_TEMPLATE_DIR "")
if(IS_DIRECTORY "${_pixelica_pkg_share}/template")
  set(Pixelica_TEMPLATE_DIR "${_pixelica_pkg_share}/template")
endif()
set(Pixelica2D_TEMPLATE_DIR "")
if(IS_DIRECTORY "${_pixelica_pkg_share}/template2d")
  set(Pixelica2D_TEMPLATE_DIR "${_pixelica_pkg_share}/template2d")
endif()
set(Pixelica_ONLINE_TEMPLATE_DIR "")
set(Pixelica_ONLINE_SERVER "")
if(IS_DIRECTORY "${_pixelica_pkg_share}/template_online")
  set(Pixelica_ONLINE_TEMPLATE_DIR "${_pixelica_pkg_share}/template_online")
endif()
if(EXISTS "${_pixelica_pkg_share}/tools/online_server/server.py")
  set(Pixelica_ONLINE_SERVER "${_pixelica_pkg_share}/tools/online_server/server.py")
endif()

check_required_components(${_pixelica_pkg_name})

unset(_pixelica_requested)
unset(_pixelica_selected)
