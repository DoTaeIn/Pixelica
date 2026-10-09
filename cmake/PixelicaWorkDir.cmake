# Safe working directories for the CTest drivers that run in "cmake -P" mode
# (PixelicaAssetRepro.cmake, PixelicaSdkCheck.cmake). include() this file; it defines
# functions only.
#
# Rule: a driver never deletes anything. It validates the directory it was told
# to use and then creates a FRESH, uniquely named child below it for each run.
# Old run directories are left alone; remove the build directory to reclaim the
# space. The validation (pixelica_check_work_dir) accepts a work directory only when,
# after resolving ".." and existing symlinks,
#   - the build directory is an existing CMake build tree (has CMakeCache.txt or
#     CMakeFiles/), is not a filesystem root, is not the source directory and
#     does not contain the source directory, and
#   - the work directory is strictly inside the build directory.
# So a root, the source tree, the build directory itself, a parent of the build
# directory, a sibling that only shares a name prefix, and a symlink that leads
# outside the build directory are all refused.
#
# Needs CMake >= 3.21 (file(REAL_PATH), cmake_path).

# Absolute input -> <out_path> (lexically normalized, symlinks resolved, no
# trailing slash) and <out_key> (the same, lower-cased on a Windows host, for
# comparisons). Both are empty for an empty or relative input or one that passes
# through a dangling symlink. The not-yet-existing tail of a path is kept as is.
function(pixelica_resolve_path input out_path out_key)
  file(TO_CMAKE_PATH "${input}" resolved)
  if("${resolved}" STREQUAL "" OR NOT IS_ABSOLUTE "${resolved}")
    set(${out_path} "" PARENT_SCOPE)
    set(${out_key} "" PARENT_SCOPE)
    return()
  endif()
  cmake_path(SET resolved NORMALIZE "${resolved}")
  # Resolve symlinks of the longest EXISTING prefix (the tail may not exist yet)
  # and append the rest. A dangling symlink cannot be resolved: refuse it.
  set(existing "${resolved}")
  set(tail "")
  while(NOT EXISTS "${existing}")
    if(IS_SYMLINK "${existing}")
      set(${out_path} "" PARENT_SCOPE)
      set(${out_key} "" PARENT_SCOPE)
      return()
    endif()
    get_filename_component(component "${existing}" NAME)
    get_filename_component(parent "${existing}" DIRECTORY)
    if("${component}" STREQUAL "" OR "${parent}" STREQUAL "${existing}")
      break()
    endif()
    set(tail "/${component}${tail}")
    set(existing "${parent}")
  endwhile()
  file(REAL_PATH "${existing}" existing)
  set(resolved "${existing}${tail}")
  cmake_path(SET resolved NORMALIZE "${resolved}")
  string(REGEX REPLACE "([^/:])/+$" "\\1" resolved "${resolved}")
  set(key "${resolved}")
  if(CMAKE_HOST_WIN32)
    string(TOLOWER "${key}" key)
  endif()
  set(${out_path} "${resolved}" PARENT_SCOPE)
  set(${out_key} "${key}" PARENT_SCOPE)
endfunction()

# <out_var> = TRUE when <child_key> is strictly below <parent_key> (keys from
# pixelica_resolve_path). "/a/b-evil" is not below "/a/b".
function(pixelica_is_strictly_inside child_key parent_key out_var)
  set(${out_var} FALSE PARENT_SCOPE)
  if("${parent_key}" STREQUAL "" OR "${child_key}" STREQUAL "")
    return()
  endif()
  string(LENGTH "${parent_key}" parent_length)
  string(LENGTH "${child_key}" child_length)
  math(EXPR shortest_child "${parent_length} + 1") # "<parent>/" with nothing after it is not inside
  string(FIND "${child_key}" "${parent_key}/" position)
  if(position EQUAL 0 AND child_length GREATER shortest_child)
    set(${out_var} TRUE PARENT_SCOPE)
  endif()
endfunction()

# Non-fatal validation. Sets <prefix>_ERROR (empty when accepted) and, when
# accepted, <prefix>_BINARY and <prefix>_WORK to the resolved paths to use.
function(pixelica_check_work_dir prefix binary_dir source_dir work_dir)
  set(${prefix}_ERROR "" PARENT_SCOPE)
  set(${prefix}_BINARY "" PARENT_SCOPE)
  set(${prefix}_WORK "" PARENT_SCOPE)

  pixelica_resolve_path("${binary_dir}" binary binary_key)
  pixelica_resolve_path("${source_dir}" source source_key)
  pixelica_resolve_path("${work_dir}" work work_key)

  set(error "")
  if(binary STREQUAL "")
    set(error "the build directory '${binary_dir}' is empty or not an absolute path")
  elseif(source STREQUAL "")
    set(error "the source directory '${source_dir}' is empty or not an absolute path")
  elseif(work STREQUAL "")
    set(error "the work directory '${work_dir}' is empty, relative, or passes through a dangling symlink")
  elseif(NOT IS_DIRECTORY "${binary}")
    set(error "the build directory '${binary}' does not exist")
  elseif(NOT IS_DIRECTORY "${source}")
    set(error "the source directory '${source}' does not exist")
  else()
    get_filename_component(binary_parent "${binary}" DIRECTORY)
    pixelica_is_strictly_inside("${source_key}" "${binary_key}" source_in_binary)
    pixelica_is_strictly_inside("${work_key}" "${binary_key}" work_in_binary)
    if("${binary_parent}" STREQUAL "${binary}" OR "${binary}" MATCHES "^[A-Za-z]:/?$")
      set(error "the build directory '${binary}' is a filesystem root")
    elseif(NOT EXISTS "${binary}/CMakeCache.txt" AND NOT IS_DIRECTORY "${binary}/CMakeFiles")
      set(error "'${binary}' does not look like a CMake build directory (no CMakeCache.txt or CMakeFiles/)")
    elseif(binary_key STREQUAL source_key)
      set(error "the build directory '${binary}' is the source directory")
    elseif(source_in_binary)
      set(error "the source directory '${source}' is inside the build directory '${binary}'")
    elseif(work_key STREQUAL binary_key)
      set(error "the work directory is the build directory itself ('${work}')")
    elseif(NOT work_in_binary)
      set(error "the work directory '${work}' (resolved from '${work_dir}') is not inside the build directory '${binary}'")
    endif()
  endif()

  set(${prefix}_ERROR "${error}" PARENT_SCOPE)
  if(error STREQUAL "")
    set(${prefix}_BINARY "${binary}" PARENT_SCOPE)
    set(${prefix}_WORK "${work}" PARENT_SCOPE)
  endif()
endfunction()

# Validates (fatal on refusal), then creates and returns a fresh, empty,
# uniquely named run directory below <work_dir> in <out_var>. Nothing is deleted.
function(pixelica_make_run_dir binary_dir source_dir work_dir out_var)
  pixelica_check_work_dir(check "${binary_dir}" "${source_dir}" "${work_dir}")
  if(NOT check_ERROR STREQUAL "")
    message(FATAL_ERROR "[work-dir] refusing to use '${work_dir}': ${check_ERROR}")
  endif()
  file(MAKE_DIRECTORY "${check_WORK}")
  # Validate again now that the directory exists (symlink resolution is exact).
  pixelica_check_work_dir(check "${binary_dir}" "${source_dir}" "${check_WORK}")
  if(NOT check_ERROR STREQUAL "")
    message(FATAL_ERROR "[work-dir] refusing to use '${work_dir}': ${check_ERROR}")
  endif()

  string(TIMESTAMP stamp "%Y%m%dT%H%M%SZ" UTC)
  foreach(attempt RANGE 1 100)
    string(RANDOM LENGTH 8 ALPHABET "0123456789abcdef" suffix)
    set(candidate "${check_WORK}/run-${stamp}-${suffix}")
    if(NOT EXISTS "${candidate}")
      file(MAKE_DIRECTORY "${candidate}")
      if(IS_DIRECTORY "${candidate}")
        set(${out_var} "${candidate}" PARENT_SCOPE)
        return()
      endif()
    endif()
  endforeach()
  message(FATAL_ERROR "[work-dir] could not create a fresh run directory below '${check_WORK}'")
endfunction()
