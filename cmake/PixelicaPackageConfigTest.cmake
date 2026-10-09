# CTest driver for "cmake_package_config" (run with cmake -P).
#
# Tests the find_package() behavior of the installed packages WITHOUT building
# or installing Pixelica: it configures the real PixelicaConfig.cmake.in and
# Pixelica2DConfig.cmake.in, copies the real PixelicaPackage.cmake, and puts them
# into fake install prefixes whose *Targets.cmake files are stubs that only
# define an INTERFACE IMPORTED target and log that they were loaded. Each case
# then configures cmake/PackageConfigConsumer against one prefix.
#
# Covered: default component choice (3D, else 2D, else common), explicit
# components import only what was asked for plus dependencies, hybrid needs both
# engines, REQUIRED unknown/missing components fail while OPTIONAL ones do not,
# repeated find_package calls load each export once, skeleton/ik need only the
# common library, the Pixelica2D wrapper resolves the Pixelica package beside
# itself (not an unrelated Pixelica_DIR) and fails cleanly without it, and a
# copied prefix still works (relocatable).
#
# Inputs (-D): PIXELICA_SOURCE_DIR PIXELICA_BINARY_DIR PIXELICA_SDK_GENERATOR [PIXELICA_SDK_PLATFORM]
#   PIXELICA_INSTALL_CMAKEDIR PIXELICA_INSTALL_CMAKEDIR_2D PIXELICA_INSTALL_SHAREDIR PIXELICA_WORK_DIR
#
# Nothing is ever deleted: PIXELICA_WORK_DIR must be inside PIXELICA_BINARY_DIR and every
# run uses a fresh child directory (PixelicaWorkDir.cmake).
cmake_minimum_required(VERSION 3.21)

foreach(var PIXELICA_SOURCE_DIR PIXELICA_BINARY_DIR PIXELICA_SDK_GENERATOR PIXELICA_INSTALL_CMAKEDIR
            PIXELICA_INSTALL_CMAKEDIR_2D PIXELICA_INSTALL_SHAREDIR PIXELICA_WORK_DIR)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "PixelicaPackageConfigTest: ${var} is required")
  endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/PixelicaWorkDir.cmake")
file(TO_CMAKE_PATH "${PIXELICA_SOURCE_DIR}" source_dir)
pixelica_make_run_dir("${PIXELICA_BINARY_DIR}" "${PIXELICA_SOURCE_DIR}" "${PIXELICA_WORK_DIR}" work_dir)
message(STATUS "[package-config] run directory ${work_dir}")

set(platform_args "")
if(NOT "${PIXELICA_SDK_PLATFORM}" STREQUAL "")
  set(platform_args -A "${PIXELICA_SDK_PLATFORM}")
endif()

# 1. Generate the real configs ------------------------------------------------
set(generated "${work_dir}/generated")
file(MAKE_DIRECTORY "${generated}")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -S "${source_dir}/cmake/PackageConfigGen" -B "${work_dir}/generate-build"
    -G "${PIXELICA_SDK_GENERATOR}" ${platform_args}
    "-DPIXELICA_SRC=${source_dir}"
    "-DPIXELICA_OUT=${generated}"
    "-DPIXELICA_INSTALL_CMAKEDIR=${PIXELICA_INSTALL_CMAKEDIR}"
    "-DPIXELICA_INSTALL_CMAKEDIR_2D=${PIXELICA_INSTALL_CMAKEDIR_2D}"
    "-DPIXELICA_INSTALL_SHAREDIR=${PIXELICA_INSTALL_SHAREDIR}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "[package-config] generating the configs failed (exit ${rc})\n${out}\n${err}")
endif()
foreach(name PixelicaConfig.cmake Pixelica2DConfig.cmake)
  if(NOT EXISTS "${generated}/${name}")
    message(FATAL_ERROR "[package-config] ${name} was not generated")
  endif()
endforeach()
# The configs must be build-independent: no @VAR@ left over, no absolute path.
foreach(name PixelicaConfig.cmake Pixelica2DConfig.cmake)
  file(READ "${generated}/${name}" content)
  # (PACKAGE_INIT leaves a literal "@PACKAGE_INIT@" in its own banner comment.)
  if(content MATCHES "@PIXELICA_[A-Za-z_]+@" OR content MATCHES "@PACKAGE_PIXELICA_[A-Za-z_]+@")
    message(FATAL_ERROR "[package-config] ${name} still contains an unsubstituted @PIXELICA_...@ variable")
  endif()
  string(TOLOWER "${content}" content_lower)
  string(TOLOWER "${source_dir}" source_lower)
  string(FIND "${content_lower}" "${source_lower}" position)
  if(NOT position EQUAL -1)
    message(FATAL_ERROR "[package-config] ${name} contains the source directory")
  endif()
  string(TOLOWER "${PIXELICA_BINARY_DIR}" binary_lower)
  string(FIND "${content_lower}" "${binary_lower}" position)
  if(NOT position EQUAL -1)
    message(FATAL_ERROR "[package-config] ${name} contains the build directory")
  endif()
endforeach()

# 2. Fake prefixes ------------------------------------------------------------
# <component> -> stub file name and imported target
function(pixelica_stub_info component out_file out_target)
  if(component STREQUAL "common")
    set(file "PixelicaCommonTargets.cmake")
    set(target "Pixelica::common")
  elseif(component STREQUAL "core")
    set(file "PixelicaTargets.cmake")
    set(target "Pixelica::core")
  elseif(component STREQUAL "renderer2d")
    set(file "PixelicaRenderer2DTargets.cmake")
    set(target "Pixelica::renderer2d")
  elseif(component STREQUAL "hybrid")
    set(file "PixelicaHybridTargets.cmake")
    set(target "Pixelica::hybrid")
  elseif(component STREQUAL "skeleton")
    set(file "PixelicaSkeletonTargets.cmake")
    set(target "Pixelica::skeleton")
  elseif(component STREQUAL "ik")
    set(file "PixelicaIKTargets.cmake")
    set(target "Pixelica::ik")
  else()
    message(FATAL_ERROR "[package-config] unknown stub component ${component}")
  endif()
  set(${out_file} "${file}" PARENT_SCOPE)
  set(${out_target} "${target}" PARENT_SCOPE)
endfunction()

# pixelica_make_prefix(<prefix> <id> [WITH_2D_CONFIG] [NO_PACKAGE] COMPONENTS <c>...)
function(pixelica_make_prefix prefix id)
  cmake_parse_arguments(P "WITH_2D_CONFIG;NO_PACKAGE" "" "COMPONENTS" ${ARGN})
  set(cmake_dir "${prefix}/${PIXELICA_INSTALL_CMAKEDIR}")
  file(MAKE_DIRECTORY "${cmake_dir}")
  if(NOT P_NO_PACKAGE)
    file(COPY_FILE "${generated}/PixelicaConfig.cmake" "${cmake_dir}/PixelicaConfig.cmake")
    file(COPY_FILE "${source_dir}/cmake/PixelicaPackage.cmake" "${cmake_dir}/PixelicaPackage.cmake")
  endif()
  foreach(component IN LISTS P_COMPONENTS)
    pixelica_stub_info(${component} stub_file stub_target)
    file(WRITE "${cmake_dir}/${stub_file}"
"set_property(GLOBAL APPEND PROPERTY PIXELICA_STUB_LOADS \"${component}@${id}\")
if(TARGET ${stub_target})
  message(FATAL_ERROR \"stub export for ${stub_target} was included although the target already exists\")
endif()
add_library(${stub_target} INTERFACE IMPORTED)
")
  endforeach()
  if(P_WITH_2D_CONFIG)
    file(MAKE_DIRECTORY "${prefix}/${PIXELICA_INSTALL_CMAKEDIR_2D}")
    file(COPY_FILE "${generated}/Pixelica2DConfig.cmake"
         "${prefix}/${PIXELICA_INSTALL_CMAKEDIR_2D}/Pixelica2DConfig.cmake")
  endif()
endfunction()

set(prefixes "${work_dir}/prefixes")
pixelica_make_prefix("${prefixes}/common" pcommon COMPONENTS common)
pixelica_make_prefix("${prefixes}/3d" pixelica COMPONENTS common core)
pixelica_make_prefix("${prefixes}/2d" p2d WITH_2D_CONFIG COMPONENTS common renderer2d)
pixelica_make_prefix("${prefixes}/merged" pmerged WITH_2D_CONFIG
  COMPONENTS common core renderer2d hybrid skeleton ik)
pixelica_make_prefix("${prefixes}/other" pother COMPONENTS common core)
pixelica_make_prefix("${prefixes}/empty" pempty COMPONENTS)          # configs only
pixelica_make_prefix("${prefixes}/2d-no-package" pnopackage WITH_2D_CONFIG NO_PACKAGE
  COMPONENTS renderer2d)

# 3. Cases --------------------------------------------------------------------
# pixelica_case(<name> PASS|FAIL <prefix> [ID <build dir name>] [EXTRA -D...])
# <name> selects the consumer case; each run needs its own build directory
# (find_package caches Pixelica_DIR / Pixelica2D_DIR there), default <name>.
function(pixelica_case name expect prefix)
  cmake_parse_arguments(C "" "ID" "EXTRA" ${ARGN})
  if(NOT C_ID)
    set(C_ID "${name}")
  endif()
  set(bin "${work_dir}/cases/${C_ID}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      -S "${source_dir}/cmake/PackageConfigConsumer" -B "${bin}"
      -G "${PIXELICA_SDK_GENERATOR}" ${platform_args}
      "-DCASE=${name}"
      "-DCMAKE_PREFIX_PATH=${prefix}"
      "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"
      "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"
      ${C_EXTRA}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(expect STREQUAL "PASS")
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "[package-config] case '${name}' failed (exit ${rc})\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
    endif()
  else()
    if(rc EQUAL 0)
      message(FATAL_ERROR "[package-config] case '${name}' should have failed in find_package() but configured\n${out}")
    endif()
    # It must be find_package that refused, not one of the consumer's own checks.
    if(err MATCHES "\\[${name}\\]" OR err MATCHES "unknown CASE")
      message(FATAL_ERROR "[package-config] case '${name}' failed for the wrong reason\n${err}")
    endif()
    if(NOT err MATCHES "Pixelica(2D)?_FOUND")
      message(FATAL_ERROR "[package-config] case '${name}' failed without mentioning the package\n${err}")
    endif()
  endif()
  message(STATUS "[package-config] ${expect} ${name}")
endfunction()

pixelica_case(default-3d PASS "${prefixes}/3d")
pixelica_case(default-2d PASS "${prefixes}/2d")
pixelica_case(default-common PASS "${prefixes}/common")
pixelica_case(default-empty FAIL "${prefixes}/empty")
pixelica_case(explicit-2d-on-merged PASS "${prefixes}/merged")
pixelica_case(explicit-3d-on-merged PASS "${prefixes}/merged")
pixelica_case(hybrid-merged PASS "${prefixes}/merged")
pixelica_case(hybrid-requires-both FAIL "${prefixes}/3d")
pixelica_case(hybrid-optional PASS "${prefixes}/3d")
pixelica_case(unknown-required FAIL "${prefixes}/3d")
pixelica_case(unknown-optional PASS "${prefixes}/3d")
pixelica_case(repeat PASS "${prefixes}/merged")
pixelica_case(rig-modules-need-common-only PASS "${prefixes}/merged")
pixelica_case(pixelica2d-default PASS "${prefixes}/2d")
pixelica_case(pixelica2d-core-common PASS "${prefixes}/merged")
pixelica_case(pixelica2d-unknown-required FAIL "${prefixes}/2d")
pixelica_case(pixelica2d-needs-common-package FAIL "${prefixes}/2d-no-package")
pixelica_case(pixelica2d-ignores-other-pixelica PASS "${prefixes}/2d"
  EXTRA "-DPixelica_DIR=${prefixes}/other/${PIXELICA_INSTALL_CMAKEDIR}")

# Relocatable: the same prefix, copied elsewhere, still resolves everything
# beside itself. (A separate directory name so the loads show the copy works.)
file(COPY "${prefixes}/2d" DESTINATION "${work_dir}/relocated")
pixelica_case(pixelica2d-default PASS "${work_dir}/relocated/2d" ID pixelica2d-default-relocated)
