# CTest driver for "cmake_work_dir_safety" (run with cmake -P).
#
# Checks that the working-directory guard used by PixelicaAssetRepro.cmake and
# PixelicaSdkCheck.cmake (PixelicaWorkDir.cmake) refuses roots, the source tree, the build
# directory itself, its parent, a name-prefix sibling, ".." escapes, relative
# paths and (when the platform lets this process create one) a symlink that
# leads outside the build directory, and accepts a normal child of the build
# directory. It also runs both real drivers with unsafe input and requires them
# to fail without touching the source tree.
#
# Nothing is deleted. The test creates (and leaves) one small run directory
# below <build>/work-dir-safety; remove the build directory to reclaim it.
#
# Inputs (-D): PIXELICA_SOURCE_DIR PIXELICA_BINARY_DIR
cmake_minimum_required(VERSION 3.21)

foreach(var PIXELICA_SOURCE_DIR PIXELICA_BINARY_DIR)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "PixelicaWorkDirSelfTest: ${var} is required")
  endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/PixelicaWorkDir.cmake")

set(source "${PIXELICA_SOURCE_DIR}")
set(binary "${PIXELICA_BINARY_DIR}")
set(failures "")
set(checks 0)

function(pixelica_expect_reject label binary_dir work_dir)
  pixelica_check_work_dir(r "${binary_dir}" "${source}" "${work_dir}")
  if(r_ERROR STREQUAL "")
    string(APPEND failures "  accepted but must be refused: ${label} (work='${work_dir}', build='${binary_dir}')\n")
    set(failures "${failures}" PARENT_SCOPE)
  else()
    message(STATUS "[work-dir-test] refused ${label}: ${r_ERROR}")
  endif()
  math(EXPR checks "${checks} + 1")
  set(checks "${checks}" PARENT_SCOPE)
endfunction()

function(pixelica_expect_accept label binary_dir work_dir)
  pixelica_check_work_dir(r "${binary_dir}" "${source}" "${work_dir}")
  if(NOT r_ERROR STREQUAL "")
    string(APPEND failures "  refused but must be accepted: ${label}: ${r_ERROR}\n")
    set(failures "${failures}" PARENT_SCOPE)
  else()
    message(STATUS "[work-dir-test] accepted ${label}: ${r_WORK}")
  endif()
  math(EXPR checks "${checks} + 1")
  set(checks "${checks}" PARENT_SCOPE)
endfunction()

# The build directory handed to the test must itself pass the guard.
cmake_path(GET binary ROOT_PATH root)
get_filename_component(binary_parent "${binary}" DIRECTORY)
get_filename_component(source_parent "${source}" DIRECTORY)

# --- accepted ---------------------------------------------------------------
pixelica_expect_accept("child of the build directory" "${binary}" "${binary}/work-dir-safety")
pixelica_expect_accept("deeper child, not yet existing" "${binary}" "${binary}/a/b/c")
pixelica_expect_accept("child written with a '..' that stays inside" "${binary}" "${binary}/x/../work-dir-safety")

# --- refused work directories ------------------------------------------------
pixelica_expect_reject("filesystem root as work dir" "${binary}" "${root}")
pixelica_expect_reject("source directory as work dir" "${binary}" "${source}")
pixelica_expect_reject("child of the source directory (outside the build directory)" "${binary}" "${source}/assets")
pixelica_expect_reject("build directory itself" "${binary}" "${binary}")
pixelica_expect_reject("parent of the build directory" "${binary}" "${binary_parent}")
pixelica_expect_reject("sibling sharing the build directory's name prefix" "${binary}" "${binary}-evil/work")
pixelica_expect_reject("'..' escape out of the build directory" "${binary}" "${binary}/../escaped-work")
pixelica_expect_reject("relative path" "${binary}" "work-dir-safety")
pixelica_expect_reject("empty path" "${binary}" "")

# --- refused build directories ------------------------------------------------
pixelica_expect_reject("filesystem root as build dir" "${root}" "${root}/work")
pixelica_expect_reject("source directory as build dir (in-source)" "${source}" "${source}/work")
pixelica_expect_reject("parent of the source directory as build dir" "${source_parent}" "${source_parent}/work")
pixelica_expect_reject("directory that is not a CMake build tree" "${source}/tools" "${source}/tools/work")
pixelica_expect_reject("missing build directory" "${binary}/no-such-build-dir" "${binary}/no-such-build-dir/work")

# --- symlink escape ---------------------------------------------------------
# A real run directory is created through the production helper, and a symbolic
# link to the source directory is placed inside it. Resolving the link must show
# the work directory is outside the build directory.
pixelica_make_run_dir("${binary}" "${source}" "${binary}/work-dir-safety" run_dir)
file(CREATE_LINK "${source}" "${run_dir}/link-to-source" SYMBOLIC RESULT link_result)
if(link_result STREQUAL "0" AND IS_SYMLINK "${run_dir}/link-to-source")
  pixelica_expect_reject("symlink to the source directory" "${binary}" "${run_dir}/link-to-source")
  pixelica_expect_reject("path below a symlink to the source directory" "${binary}" "${run_dir}/link-to-source/new-child")
  pixelica_expect_accept("the real run directory" "${binary}" "${run_dir}")
else()
  message(STATUS "[work-dir-test] SKIPPED symlink cases: could not create a symbolic link (${link_result})")
endif()

# Run directories are unique and fresh.
pixelica_make_run_dir("${binary}" "${source}" "${binary}/work-dir-safety" run_dir_2)
if(run_dir STREQUAL run_dir_2)
  string(APPEND failures "  two calls returned the same run directory ${run_dir}\n")
endif()

# --- the real drivers must refuse unsafe input before writing anything -------
set(sentinel "${source}/CMakeLists.txt")
if(NOT EXISTS "${sentinel}")
  message(FATAL_ERROR "PixelicaWorkDirSelfTest: ${sentinel} not found; PIXELICA_SOURCE_DIR is not the Pixelica source")
endif()
set(bad_work_dirs "${source}" "${root}" "${binary_parent}" "${binary}")
foreach(bad IN LISTS bad_work_dirs)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DPIXELICA_PYTHON=python-is-never-started" "-DPIXELICA_SOURCE_DIR=${source}"
      "-DPIXELICA_BINARY_DIR=${binary}" "-DPIXELICA_WORK_DIR=${bad}"
      -P "${CMAKE_CURRENT_LIST_DIR}/PixelicaAssetRepro.cmake"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0 OR NOT "${err}" MATCHES "refusing to use")
    string(APPEND failures "  PixelicaAssetRepro.cmake did not refuse work dir '${bad}' (exit ${rc}): ${err}\n")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DPIXELICA_SOURCE_DIR=${source}" "-DPIXELICA_BINARY_DIR=${binary}" "-DPIXELICA_CONFIG=Release"
      "-DPIXELICA_SDK_GENERATOR=unused" "-DPIXELICA_INSTALL_CMAKEDIR=lib/cmake/Pixelica"
      "-DPIXELICA_SDK_WORK_DIR=${bad}"
      -P "${CMAKE_CURRENT_LIST_DIR}/PixelicaSdkCheck.cmake"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0 OR NOT "${err}" MATCHES "refusing to use")
    string(APPEND failures "  PixelicaSdkCheck.cmake did not refuse work dir '${bad}' (exit ${rc}): ${err}\n")
  endif()
  math(EXPR checks "${checks} + 2")
endforeach()
if(NOT EXISTS "${sentinel}")
  string(APPEND failures "  ${sentinel} disappeared\n")
endif()

if(NOT failures STREQUAL "")
  message(FATAL_ERROR "[work-dir-test] FAILED:\n${failures}")
endif()
message(STATUS "[work-dir-test] OK: ${checks} checks")
