# CTest driver for the "sdk_installed_template" / "sdk_installed_template2d" tests
# (run with cmake -P).
#
# 1. Installs the already built Pixelica into a fresh prefix (new run directory).
# 2. Checks that the installed packages do not point back into the source or
#    build tree and that the expected files exist.
# 3. Copies the starter template of the profile (sdk/template for 3D,
#    sdk/template2d for 2D) to a directory outside the source tree, configures it
#    against the installed package (Pixelica_DIR / Pixelica2D_DIR forced to the
#    temporary prefix), builds it and runs its tests.
# 4. 3D profile only: runs the template executable and checks its output.
# 5. 3D profile, when both engines are built: installs the SPLIT SDKs
#    (component common + renderer2d -> 2D prefix, component core -> 3D prefix,
#    all four -> merged prefix) and checks that the 2D SDK contains no 3D header,
#    library or export, that the 3D SDK contains no 2D one, that shared config
#    files are byte-identical, and that a tiny consumer links in each of them
#    (find_package(Pixelica2D), find_package(Pixelica), hybrid on the merged
#    prefix).
#
# Inputs (-D): PIXELICA_SOURCE_DIR PIXELICA_BINARY_DIR PIXELICA_CONFIG PIXELICA_SDK_GENERATOR
#   PIXELICA_SDK_PLATFORM PIXELICA_SDK_TOOLSET PIXELICA_SDK_C_COMPILER PIXELICA_SDK_MAKE_PROGRAM
#   PIXELICA_SDK_WORK_DIR PIXELICA_INSTALL_CMAKEDIR [PIXELICA_INSTALL_CMAKEDIR_2D]
#   [PIXELICA_SDK_PROFILE = 3D (default) | 2D] [PIXELICA_EXPECT_3D] [PIXELICA_EXPECT_2D]
# PIXELICA_CONFIG may be empty (single-config generator without CMAKE_BUILD_TYPE:
# Release is used).
#
# Nothing is ever deleted. PIXELICA_SDK_WORK_DIR must resolve (".." and symlinks
# resolved) to a directory strictly inside PIXELICA_BINARY_DIR; each run then uses a
# fresh run-<time>-<random> child of it (see PixelicaWorkDir.cmake). Anything else is
# refused before any file is written.
cmake_minimum_required(VERSION 3.21)

foreach(var PIXELICA_SOURCE_DIR PIXELICA_BINARY_DIR PIXELICA_SDK_GENERATOR PIXELICA_SDK_WORK_DIR PIXELICA_INSTALL_CMAKEDIR)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "PixelicaSdkCheck: ${var} is required")
  endif()
endforeach()
if(NOT DEFINED PIXELICA_SDK_PROFILE OR "${PIXELICA_SDK_PROFILE}" STREQUAL "")
  set(PIXELICA_SDK_PROFILE 3D)
endif()
if(NOT PIXELICA_SDK_PROFILE STREQUAL "3D" AND NOT PIXELICA_SDK_PROFILE STREQUAL "2D")
  message(FATAL_ERROR "PixelicaSdkCheck: PIXELICA_SDK_PROFILE must be 3D or 2D (got '${PIXELICA_SDK_PROFILE}')")
endif()
if(NOT DEFINED PIXELICA_INSTALL_CMAKEDIR_2D OR "${PIXELICA_INSTALL_CMAKEDIR_2D}" STREQUAL "")
  string(REGEX REPLACE "Pixelica$" "Pixelica2D" PIXELICA_INSTALL_CMAKEDIR_2D "${PIXELICA_INSTALL_CMAKEDIR}")
endif()
if(NOT DEFINED PIXELICA_INSTALL_SHAREDIR OR "${PIXELICA_INSTALL_SHAREDIR}" STREQUAL "")
  set(PIXELICA_INSTALL_SHAREDIR "share/Pixelica")
endif()
# Without the flags assume the profile under test is built.
if(NOT DEFINED PIXELICA_EXPECT_3D)
  set(PIXELICA_EXPECT_3D ON)
endif()
if(NOT DEFINED PIXELICA_EXPECT_2D)
  if(PIXELICA_SDK_PROFILE STREQUAL "2D")
    set(PIXELICA_EXPECT_2D ON)
  else()
    set(PIXELICA_EXPECT_2D OFF)
  endif()
endif()
if(NOT DEFINED PIXELICA_EXPECT_HYBRID)
  set(PIXELICA_EXPECT_HYBRID OFF)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/PixelicaWorkDir.cmake")

file(TO_CMAKE_PATH "${PIXELICA_SOURCE_DIR}" source_dir)
file(TO_CMAKE_PATH "${PIXELICA_BINARY_DIR}" binary_dir)
pixelica_make_run_dir("${PIXELICA_BINARY_DIR}" "${PIXELICA_SOURCE_DIR}" "${PIXELICA_SDK_WORK_DIR}" work_dir)
message(STATUS "[sdk-check] run directory ${work_dir}")
set(prefix "${work_dir}/prefix")
set(config "${PIXELICA_CONFIG}")
if(config STREQUAL "")
  set(config Release)
endif()

if(CMAKE_HOST_WIN32)
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()

function(pixelica_step description)
  cmake_parse_arguments(S "" "" "COMMAND" ${ARGN})
  message(STATUS "[sdk-check] ${description}")
  execute_process(COMMAND ${S_COMMAND}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "[sdk-check] FAILED: ${description} (exit ${rc})\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
  endif()
endfunction()

# <out_var> = the arguments that configure <src> into <bin> with the same
# generator / compiler as the Pixelica build under test.
function(pixelica_configure_args out_var src bin)
  set(args -S "${src}" -B "${bin}" -G "${PIXELICA_SDK_GENERATOR}")
  if(NOT "${PIXELICA_SDK_PLATFORM}" STREQUAL "")
    list(APPEND args -A "${PIXELICA_SDK_PLATFORM}")
  endif()
  if(NOT "${PIXELICA_SDK_TOOLSET}" STREQUAL "")
    list(APPEND args -T "${PIXELICA_SDK_TOOLSET}")
  endif()
  list(APPEND args
    "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"
    "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF")
  if(NOT PIXELICA_SDK_GENERATOR MATCHES "Visual Studio|Xcode|Multi-Config")
    list(APPEND args "-DCMAKE_BUILD_TYPE=${config}")
    if(NOT "${PIXELICA_SDK_C_COMPILER}" STREQUAL "")
      list(APPEND args "-DCMAKE_C_COMPILER=${PIXELICA_SDK_C_COMPILER}")
    endif()
    if(NOT "${PIXELICA_SDK_MAKE_PROGRAM}" STREQUAL "")
      list(APPEND args "-DCMAKE_MAKE_PROGRAM=${PIXELICA_SDK_MAKE_PROGRAM}")
    endif()
  endif()
  set(${out_var} "${args}" PARENT_SCOPE)
endfunction()

function(pixelica_install_component component dest_prefix)
  pixelica_step("install component '${component}' (${config}) into ${dest_prefix}"
    COMMAND "${CMAKE_COMMAND}" --install "${binary_dir}" --config "${config}"
            --component "${component}" --prefix "${dest_prefix}")
endfunction()

function(pixelica_expect_present root)
  foreach(rel IN LISTS ARGN)
    if(NOT EXISTS "${root}/${rel}")
      message(FATAL_ERROR "[sdk-check] expected installed file is missing: ${root}/${rel}")
    endif()
  endforeach()
endfunction()

function(pixelica_expect_absent root)
  foreach(rel IN LISTS ARGN)
    if(EXISTS "${root}/${rel}")
      message(FATAL_ERROR "[sdk-check] this SDK must not contain: ${root}/${rel}")
    endif()
  endforeach()
endfunction()

# Fails when any file matches one of the glob patterns (relative to root).
function(pixelica_expect_no_match root)
  foreach(pattern IN LISTS ARGN)
    file(GLOB found "${root}/${pattern}")
    if(found)
      message(FATAL_ERROR "[sdk-check] this SDK must not contain files matching ${pattern}: ${found}")
    endif()
  endforeach()
endfunction()

# Fails unless some file matches the glob pattern (relative to root).
function(pixelica_expect_match root pattern)
  file(GLOB found "${root}/${pattern}")
  if(NOT found)
    message(FATAL_ERROR "[sdk-check] expected a file matching ${pattern} under ${root}")
  endif()
endfunction()

# Locates <bin>/[<config>/]<name>[.exe]; <out_var> empty when not built.
function(pixelica_find_exe bin name out_var)
  set(${out_var} "" PARENT_SCOPE)
  foreach(candidate
      "${bin}/${config}/${name}${exe_suffix}"
      "${bin}/bin/${config}/${name}${exe_suffix}"
      "${bin}/bin/${name}${exe_suffix}"
      "${bin}/${name}${exe_suffix}")
    if(EXISTS "${candidate}")
      set(${out_var} "${candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()

# Package files must not reference the source or build tree and must not leak
# the build-only pixelica_options target.
function(pixelica_check_package_files root)
  string(TOLOWER "${source_dir}" source_dir_lower)
  string(TOLOWER "${binary_dir}" binary_dir_lower)
  file(GLOB package_files
    "${root}/${PIXELICA_INSTALL_CMAKEDIR}/*.cmake"
    "${root}/${PIXELICA_INSTALL_CMAKEDIR_2D}/*.cmake")
  foreach(package_file IN LISTS package_files)
    file(READ "${package_file}" content)
    string(REPLACE "\\" "/" content "${content}")
    string(TOLOWER "${content}" content_lower)
    foreach(forbidden "${source_dir_lower}" "${binary_dir_lower}")
      string(FIND "${content_lower}" "${forbidden}" position)
      if(NOT position EQUAL -1)
        message(FATAL_ERROR "[sdk-check] ${package_file} references the source/build tree (${forbidden})")
      endif()
    endforeach()
    if(content MATCHES "pixelica_options")
      message(FATAL_ERROR "[sdk-check] ${package_file} leaks the build-only pixelica_options target")
    endif()
  endforeach()
endfunction()

# 1. Install -----------------------------------------------------------------
pixelica_step("install Pixelica (${config}) into ${prefix}"
  COMMAND "${CMAKE_COMMAND}" --install "${binary_dir}" --config "${config}" --prefix "${prefix}")

set(pkg_dir "${prefix}/${PIXELICA_INSTALL_CMAKEDIR}")
pixelica_expect_present("${prefix}"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaConfig.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaConfigVersion.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaPackage.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaCommonTargets.cmake"
  "include/pixelica/math.h"
  "include/pixelica/image.h"
  "include/pixelica/assets_common.h")
if(PIXELICA_EXPECT_3D)
  pixelica_expect_present("${prefix}"
    "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaTargets.cmake"
    "include/pixelica/pixelica.h")
endif()
if(PIXELICA_EXPECT_2D)
  pixelica_expect_present("${prefix}"
    "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaRenderer2DTargets.cmake"
    "${PIXELICA_INSTALL_CMAKEDIR_2D}/Pixelica2DConfig.cmake"
    "${PIXELICA_INSTALL_CMAKEDIR_2D}/Pixelica2DConfigVersion.cmake"
    "include/pixelica/renderer2d.h")
endif()

# 2. The packages must be relocatable: no absolute source/build paths. ---------
pixelica_check_package_files("${prefix}")

# 3. Standalone project built against the installed package. --------------------
if(PIXELICA_SDK_PROFILE STREQUAL "3D")
  set(template_name template)
  set(package_dir_var Pixelica_DIR)
  set(package_dir "${pkg_dir}")
  set(strict_option "-DPIXELICA_GAME_STRICT_WARNINGS=ON")
else()
  set(template_name template2d)
  set(package_dir_var Pixelica2D_DIR)
  set(package_dir "${prefix}/${PIXELICA_INSTALL_CMAKEDIR_2D}")
  set(strict_option "")
endif()
set(game_src "${work_dir}/game")
set(game_bin "${work_dir}/game-build")
file(COPY "${source_dir}/sdk/${template_name}/" DESTINATION "${game_src}")

pixelica_configure_args(configure_args "${game_src}" "${game_bin}")
list(APPEND configure_args
  "-D${package_dir_var}=${package_dir}"
  "-DCMAKE_PREFIX_PATH=${prefix}")
if(NOT strict_option STREQUAL "")
  list(APPEND configure_args "${strict_option}")
endif()

pixelica_step("configure the copied ${template_name} against the installed package"
  COMMAND "${CMAKE_COMMAND}" ${configure_args})
pixelica_step("build the ${template_name} (${config})"
  COMMAND "${CMAKE_COMMAND}" --build "${game_bin}" --config "${config}")

if(CMAKE_CTEST_COMMAND)
  set(ctest_command "${CMAKE_CTEST_COMMAND}")
else()
  get_filename_component(cmake_bin_dir "${CMAKE_COMMAND}" DIRECTORY)
  set(ctest_command "${cmake_bin_dir}/ctest")
endif()
pixelica_step("run the ${template_name}'s own CTest tests"
  COMMAND "${ctest_command}" --test-dir "${game_bin}" -C "${config}" --output-on-failure)

if(PIXELICA_SDK_PROFILE STREQUAL "2D")
  message(STATUS "[sdk-check] OK: 2D starter template configured, built and tested against the installed Pixelica2D package")
  return()
endif()

# 4. Run the 3D template executable and check its output. ----------------------
pixelica_find_exe("${game_bin}" pixelica_game game_exe)
if(game_exe STREQUAL "")
  message(FATAL_ERROR "[sdk-check] pixelica_game was not produced under ${game_bin}")
endif()

function(pixelica_run_game frames out_var)
  execute_process(COMMAND "${game_exe}" --frames ${frames}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "[sdk-check] pixelica_game --frames ${frames} exited ${rc}\n${out}\n${err}")
  endif()
  string(STRIP "${out}" out)
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

message(STATUS "[sdk-check] run the template executable")
pixelica_run_game(30 run_a)
pixelica_run_game(30 run_b)
pixelica_run_game(31 run_c)
if(NOT run_a MATCHES "^pixelica-template: frames=30 presented=30 fnv1a=[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]$")
  message(FATAL_ERROR "[sdk-check] unexpected output: '${run_a}'")
endif()
if(NOT run_a STREQUAL run_b)
  message(FATAL_ERROR "[sdk-check] two identical runs differ:\n${run_a}\n${run_b}")
endif()
if(run_a STREQUAL run_c)
  message(FATAL_ERROR "[sdk-check] 30 and 31 frames produced the same output; the frame hash does not follow rendering")
endif()

foreach(bad_args "--frames;0" "--frames;abc" "--bogus")
  execute_process(COMMAND "${game_exe}" ${bad_args}
    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(rc EQUAL 0)
    message(FATAL_ERROR "[sdk-check] pixelica_game ${bad_args} should have failed")
  endif()
endforeach()

# 5. Split SDKs (both engines built) ------------------------------------------
if(NOT (PIXELICA_EXPECT_3D AND PIXELICA_EXPECT_2D))
  message(STATUS "[sdk-check] OK: ${run_a}")
  return()
endif()

set(common_headers status.h input.h platform.h math.h animation.h image.h assets_common.h)
set(only_3d_headers pixelica.h renderer.h assets.h scene.h)
list(TRANSFORM common_headers PREPEND "include/pixelica/" OUTPUT_VARIABLE common_header_paths)
list(TRANSFORM only_3d_headers PREPEND "include/pixelica/" OUTPUT_VARIABLE only_3d_header_paths)

# A tiny consumer: writes the project, configures it against <install_prefix>,
# builds and runs it. <cmake_lists> and <main_c> are the file contents.
function(pixelica_run_consumer label install_prefix cmake_lists main_c)
  set(consumer_src "${work_dir}/consumer-${label}")
  set(consumer_bin "${work_dir}/consumer-${label}-build")
  file(MAKE_DIRECTORY "${consumer_src}")
  file(WRITE "${consumer_src}/CMakeLists.txt" "${cmake_lists}")
  file(WRITE "${consumer_src}/main.c" "${main_c}")
  pixelica_configure_args(consumer_args "${consumer_src}" "${consumer_bin}")
  list(APPEND consumer_args "-DCMAKE_PREFIX_PATH=${install_prefix}")
  pixelica_step("[${label}] configure a consumer against the installed prefix"
    COMMAND "${CMAKE_COMMAND}" ${consumer_args})
  pixelica_step("[${label}] build the consumer"
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_bin}" --config "${config}")
  pixelica_find_exe("${consumer_bin}" consumer consumer_exe)
  if(consumer_exe STREQUAL "")
    message(FATAL_ERROR "[sdk-check] [${label}] consumer executable was not produced under ${consumer_bin}")
  endif()
  pixelica_step("[${label}] run the consumer" COMMAND "${consumer_exe}")
endfunction()

set(consumer_2d_cmake [=[
cmake_minimum_required(VERSION 3.21)
project(Consumer2D C)
find_package(Pixelica2D 0.1 CONFIG REQUIRED)
find_package(Pixelica 0.1 CONFIG REQUIRED)
if(NOT TARGET Pixelica2D::core OR NOT TARGET Pixelica::renderer2d OR NOT TARGET Pixelica::common)
  message(FATAL_ERROR "the 2D SDK must provide Pixelica2D::core, Pixelica::renderer2d and Pixelica::common")
endif()
if(TARGET Pixelica::core OR TARGET Pixelica::renderer3d)
  message(FATAL_ERROR "a 2D-only SDK must not provide the 3D core")
endif()
find_package(Pixelica 0.1 CONFIG QUIET COMPONENTS core)
if(Pixelica_core_FOUND OR TARGET Pixelica::core)
  message(FATAL_ERROR "component core must not be found in a 2D-only SDK")
endif()
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE Pixelica2D::core)
]=])
set(consumer_2d_main [=[
#include <stddef.h>
#include <stdint.h>
#include "pixelica/renderer2d.h"
int main(void)
{
    uint16_t px[16];
    PIXELICA2D_Surface surface = {px, 4, 4, 4, NULL, 0};
    PIXELICA2D_Context ctx;
    float m[16];
    if (pixelica2d_init(&ctx, &surface) != PIXELICA_OK) return 1;
    if (pixelica2d_clear(&ctx, 0x1234u) != PIXELICA_OK) return 2;
    if (px[15] != 0x1234u) return 3;
    pixelica_mat4_identity(m);
    if (m[0] != 1.0f) return 4;
    return pixelica_crc32(NULL, 0) == 0u ? 0 : 5;
}
]=])

set(consumer_3d_cmake [=[
cmake_minimum_required(VERSION 3.21)
project(Consumer3D C)
find_package(Pixelica 0.1 CONFIG REQUIRED)
if(NOT TARGET Pixelica::core OR NOT TARGET Pixelica::renderer3d OR NOT TARGET Pixelica::common)
  message(FATAL_ERROR "the 3D SDK must provide Pixelica::core, ::renderer3d and ::common")
endif()
if(TARGET Pixelica::renderer2d OR TARGET Pixelica2D::core)
  message(FATAL_ERROR "a 3D-only SDK must not provide the 2D renderer")
endif()
find_package(Pixelica 0.1 CONFIG QUIET COMPONENTS renderer2d hybrid)
if(Pixelica_renderer2d_FOUND OR Pixelica_hybrid_FOUND)
  message(FATAL_ERROR "renderer2d / hybrid must not be found in a 3D-only SDK")
endif()
find_package(Pixelica2D 0.1 CONFIG QUIET)
if(Pixelica2D_FOUND)
  message(FATAL_ERROR "Pixelica2D must not be found in a 3D-only SDK")
endif()
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE Pixelica::core)
]=])
set(consumer_3d_main [=[
#include <stddef.h>
#include "pixelica/pixelica.h"
int main(void)
{
    PIXELICA_Config cfg = {8, 8, 1, 1, 64};
    size_t bytes = 0;
    float m[16];
    if (pixelica_memory_requirement(&cfg, &bytes) != PIXELICA_OK || bytes == 0) return 1;
    pixelica_mat4_identity(m);
    return (m[0] == 1.0f && pixelica_crc32(NULL, 0) == 0u) ? 0 : 2;
}
]=])

set(consumer_merged_cmake [=[
cmake_minimum_required(VERSION 3.21)
project(ConsumerHybrid C)
find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS hybrid)
foreach(t Pixelica::hybrid Pixelica::core Pixelica::renderer2d Pixelica::common)
  if(NOT TARGET ${t})
    message(FATAL_ERROR "merged SDK: ${t} is missing after COMPONENTS hybrid")
  endif()
endforeach()
# Repeat calls are safe and import more only on request.
find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS core renderer2d hybrid)
find_package(Pixelica2D 0.1 CONFIG REQUIRED)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE Pixelica::hybrid)
]=])
set(consumer_merged_main [=[
#include <stdbool.h>
#include "pixelica/hybrid.h"
int main(void)
{
    float vp[16];
    PIXELICA_Vec3 origin = {0.0f, 0.0f, 0.0f};
    PIXELICA25D_Projection projection;
    pixelica_mat4_identity(vp);
    if (pixelica25d_project(vp, origin, 16, 16, &projection) != PIXELICA_OK) return 1;
    return projection.drawable ? 0 : 2;
}
]=])

# 5a. 2D SDK: components common + renderer2d.
set(prefix_2d "${work_dir}/prefix-2d")
pixelica_install_component(common "${prefix_2d}")
pixelica_install_component(renderer2d "${prefix_2d}")
pixelica_expect_present("${prefix_2d}"
  ${common_header_paths}
  "include/pixelica/renderer2d.h"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaConfig.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaPackage.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaCommonTargets.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaRenderer2DTargets.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR_2D}/Pixelica2DConfig.cmake")
pixelica_expect_absent("${prefix_2d}"
  ${only_3d_header_paths}
  "include/pixelica/hybrid.h"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaTargets.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaHybridTargets.cmake"
  "${PIXELICA_INSTALL_SHAREDIR}/template")
pixelica_expect_match("${prefix_2d}" "lib*/*pixelica_common*")
pixelica_expect_match("${prefix_2d}" "lib*/*pixelica_renderer2d*")
pixelica_expect_no_match("${prefix_2d}" "lib*/*pixelica_core*" "lib*/*pixelica_hybrid*")
pixelica_check_package_files("${prefix_2d}")
pixelica_run_consumer(2d "${prefix_2d}" "${consumer_2d_cmake}" "${consumer_2d_main}")

# 5b. 3D SDK: component core alone is complete (common is part of it).
set(prefix_3d "${work_dir}/prefix-3d")
pixelica_install_component(core "${prefix_3d}")
pixelica_expect_present("${prefix_3d}"
  ${common_header_paths}
  ${only_3d_header_paths}
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaConfig.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaPackage.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaCommonTargets.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaTargets.cmake")
pixelica_expect_absent("${prefix_3d}"
  "include/pixelica/renderer2d.h"
  "include/pixelica/hybrid.h"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaRenderer2DTargets.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaHybridTargets.cmake"
  "${PIXELICA_INSTALL_CMAKEDIR_2D}/Pixelica2DConfig.cmake")
pixelica_expect_match("${prefix_3d}" "lib*/*pixelica_common*")
pixelica_expect_match("${prefix_3d}" "lib*/*pixelica_core*")
pixelica_expect_no_match("${prefix_3d}" "lib*/*pixelica_renderer2d*" "lib*/*pixelica_hybrid*")
pixelica_check_package_files("${prefix_3d}")
pixelica_run_consumer(3d "${prefix_3d}" "${consumer_3d_cmake}" "${consumer_3d_main}")

# 5c. Files present in both SDKs must be byte-identical (so unpacking both into
# one prefix cannot conflict).
file(GLOB shared_names RELATIVE "${prefix_2d}/${PIXELICA_INSTALL_CMAKEDIR}" "${prefix_2d}/${PIXELICA_INSTALL_CMAKEDIR}/*")
foreach(name IN LISTS shared_names)
  if(EXISTS "${prefix_3d}/${PIXELICA_INSTALL_CMAKEDIR}/${name}")
    file(SHA256 "${prefix_2d}/${PIXELICA_INSTALL_CMAKEDIR}/${name}" sha_2d)
    file(SHA256 "${prefix_3d}/${PIXELICA_INSTALL_CMAKEDIR}/${name}" sha_3d)
    if(NOT sha_2d STREQUAL sha_3d)
      message(FATAL_ERROR "[sdk-check] ${PIXELICA_INSTALL_CMAKEDIR}/${name} differs between the 2D and the 3D SDK")
    endif()
  endif()
endforeach()
foreach(rel ${common_header_paths})
  file(SHA256 "${prefix_2d}/${rel}" sha_2d)
  file(SHA256 "${prefix_3d}/${rel}" sha_3d)
  if(NOT sha_2d STREQUAL sha_3d)
    message(FATAL_ERROR "[sdk-check] ${rel} differs between the 2D and the 3D SDK")
  endif()
endforeach()

# 5d. Opt-in hybrid on a merged prefix (3D SDK + hybrid + 2D SDK).
if(PIXELICA_EXPECT_HYBRID)
  set(prefix_merged "${work_dir}/prefix-merged")
  foreach(component common core renderer2d hybrid)
    pixelica_install_component(${component} "${prefix_merged}")
  endforeach()
  pixelica_expect_present("${prefix_merged}" "include/pixelica/hybrid.h" "${PIXELICA_INSTALL_CMAKEDIR}/PixelicaHybridTargets.cmake")
  pixelica_check_package_files("${prefix_merged}")
  pixelica_run_consumer(merged "${prefix_merged}" "${consumer_merged_cmake}" "${consumer_merged_main}")
  message(STATUS "[sdk-check] OK: ${run_a}; 2D, 3D and merged (hybrid) SDK profiles verified")
else()
  message(STATUS "[sdk-check] OK: ${run_a}; 2D and 3D SDK profiles verified (hybrid module not built: merged check skipped)")
endif()
