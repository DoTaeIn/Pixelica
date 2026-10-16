# CTest driver for "water_source_addon" (run with cmake -P).
#
# Proves the PORTABLE SOURCE add-on is really usable by someone who has only an
# installed base SDK and the -source ZIP:
#   1. install the base SDK components (common, plus core when the 3D renderer
#      is built) into a fresh prefix, and verify it contains NO water file,
#   2. stage exactly the files the source ZIP ships, in the ZIP's own layout,
#      into a directory that is NOT the repository (so nothing can reach back
#      into the source tree),
#   3. configure and build that staged tree against the installed base SDK
#      alone, with the raft demo on when the prefix has the 3D core,
#   4. run the add-on's own test (the demo must report ok=1),
#   5. install the components it produces into the base SDK's prefix and check
#      no base file was replaced,
#   6. configure, build and run cmake/WaterConsumer - a real external consumer -
#      against the resulting prefix.
#
# WATER_SOURCE_FILES below is the same allowlist as SOURCE_FILES in
# tools/package_water.py; tools/tests/test_package_water.py fails if the two
# ever drift apart.
#
# Nothing is ever deleted; every run gets a fresh directory below the work
# directory (cmake/PixelicaWorkDir.cmake).
#
# Input (-D): the PIXELICA_SDK_CHECK_COMMON_ARGS of the root CMakeLists.txt and
#             PIXELICA_SDK_WORK_DIR.
cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_LIST_DIR}/PixelicaWorkDir.cmake")
pixelica_make_run_dir("${PIXELICA_BINARY_DIR}" "${PIXELICA_SOURCE_DIR}"
                      "${PIXELICA_SDK_WORK_DIR}" water_work)

set(water_config "${PIXELICA_CONFIG}")
if(water_config STREQUAL "")
  set(water_config Release)
endif()

# "<destination in the ZIP>=<path in the repository>"
set(WATER_SOURCE_FILES
  "CMakeLists.txt=sdk/water-addon/CMakeLists.txt"
  "cmake/PixelicaWaterConfig.cmake.in=cmake/PixelicaWaterConfig.cmake.in"
  "docs/water.md=docs/water.md"
  "examples/hello_water/hello_water.c=examples/hello_water/hello_water.c"
  "modules/water/CMakeLists.txt=modules/water/CMakeLists.txt"
  "modules/water/water.c=modules/water/water.c"
  "modules/water/water3d.c=modules/water/water3d.c"
  "modules/water/include/pixelica/water.h=modules/water/include/pixelica/water.h"
  "modules/water/include/pixelica/water3d.h=modules/water/include/pixelica/water3d.h")

function(water_step)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "[water-source] step failed (${rc}): ${ARGV}\n${out}\n${err}")
  endif()
  set(water_last_output "${out}" PARENT_SCOPE)
endfunction()

set(water_prefix "${water_work}/prefix")
set(water_addon "${water_work}/addon")
get_filename_component(water_cmake_parent "${PIXELICA_INSTALL_CMAKEDIR}" DIRECTORY)
set(water_cmakedir "${water_cmake_parent}/PixelicaWater")

# -- 1. a base SDK with no water in it ---------------------------------------
set(water_base_components common)
if(PIXELICA_EXPECT_3D)
  list(APPEND water_base_components core)
endif()
foreach(component IN LISTS water_base_components)
  water_step("${CMAKE_COMMAND}" --install "${PIXELICA_BINARY_DIR}" --config "${water_config}"
    --prefix "${water_prefix}" --component "${component}")
endforeach()
file(GLOB_RECURSE water_leaked
  "${water_prefix}/include/pixelica/water*.h"
  "${water_prefix}/*water*.a" "${water_prefix}/*water*.lib"
  "${water_prefix}/${water_cmakedir}/*")
if(water_leaked)
  list(JOIN water_leaked "\n  " joined)
  message(FATAL_ERROR "[water-source] the BASE SDK contains water files:\n  ${joined}")
endif()
set(water_guarded
  "${water_prefix}/${PIXELICA_INSTALL_CMAKEDIR}/PixelicaPackage.cmake"
  "${water_prefix}/include/pixelica/math.h")
set(water_before "")
foreach(file IN LISTS water_guarded)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "[water-source] the base install is missing ${file}")
  endif()
  file(SHA256 "${file}" hash)
  list(APPEND water_before "${hash}")
endforeach()

# -- 2. stage the source ZIP's layout ----------------------------------------
foreach(entry IN LISTS WATER_SOURCE_FILES)
  string(FIND "${entry}" "=" split)
  string(SUBSTRING "${entry}" 0 ${split} destination)
  math(EXPR after "${split} + 1")
  string(SUBSTRING "${entry}" ${after} -1 origin)
  if(NOT EXISTS "${PIXELICA_SOURCE_DIR}/${origin}")
    message(FATAL_ERROR "[water-source] the add-on needs ${origin}, which does not exist")
  endif()
  if(IS_SYMLINK "${PIXELICA_SOURCE_DIR}/${origin}")
    message(FATAL_ERROR "[water-source] refusing to stage a symbolic link: ${origin}")
  endif()
  configure_file("${PIXELICA_SOURCE_DIR}/${origin}" "${water_addon}/${destination}" COPYONLY)
endforeach()

# -- 3. build it against the installed base SDK alone ------------------------
set(water_args -G "${PIXELICA_SDK_GENERATOR}" "-DCMAKE_BUILD_TYPE=${water_config}"
  "-DCMAKE_PREFIX_PATH=${water_prefix}"
  "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF" "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF")
if(NOT "${PIXELICA_SDK_PLATFORM}" STREQUAL "")
  list(APPEND water_args -A "${PIXELICA_SDK_PLATFORM}")
endif()
if(NOT "${PIXELICA_SDK_TOOLSET}" STREQUAL "")
  list(APPEND water_args -T "${PIXELICA_SDK_TOOLSET}")
endif()
if(NOT PIXELICA_SDK_GENERATOR MATCHES "Visual Studio|Xcode|Multi-Config")
  if(NOT "${PIXELICA_SDK_C_COMPILER}" STREQUAL "")
    list(APPEND water_args "-DCMAKE_C_COMPILER=${PIXELICA_SDK_C_COMPILER}")
  endif()
  if(NOT "${PIXELICA_SDK_MAKE_PROGRAM}" STREQUAL "")
    list(APPEND water_args "-DCMAKE_MAKE_PROGRAM=${PIXELICA_SDK_MAKE_PROGRAM}")
  endif()
endif()
set(water_addon_args ${water_args})
if(PIXELICA_EXPECT_3D)
  list(APPEND water_addon_args "-DPIXELICA_WATER_BUILD_EXAMPLE=ON")
endif()
water_step("${CMAKE_COMMAND}" -S "${water_addon}" -B "${water_work}/addon-build"
  ${water_addon_args})
water_step("${CMAKE_COMMAND}" --build "${water_work}/addon-build" --config "${water_config}"
  --parallel 2)

# -- 4. the add-on's own test (the raft demo) --------------------------------
if(PIXELICA_EXPECT_3D)
  water_step("${CMAKE_CTEST_COMMAND}" --test-dir "${water_work}/addon-build"
    -C "${water_config}" --output-on-failure)
endif()

# -- 5. install it into the base SDK's prefix --------------------------------
water_step("${CMAKE_COMMAND}" --install "${water_work}/addon-build" --config "${water_config}"
  --prefix "${water_prefix}")
foreach(required
    "include/pixelica/water.h"
    "${water_cmakedir}/PixelicaWaterConfig.cmake"
    "${water_cmakedir}/PixelicaWaterTargets.cmake")
  if(NOT EXISTS "${water_prefix}/${required}")
    message(FATAL_ERROR "[water-source] the source add-on did not install ${required}")
  endif()
endforeach()
if(PIXELICA_EXPECT_3D AND NOT EXISTS "${water_prefix}/include/pixelica/water3d.h")
  message(FATAL_ERROR "[water-source] the source add-on did not install the water3d header")
endif()
list(LENGTH water_guarded water_guarded_count)
math(EXPR water_last "${water_guarded_count} - 1")
foreach(index RANGE ${water_last})
  list(GET water_guarded ${index} file)
  list(GET water_before ${index} expected)
  file(SHA256 "${file}" hash)
  if(NOT hash STREQUAL expected)
    message(FATAL_ERROR "[water-source] installing the add-on REPLACED a base SDK file: ${file}")
  endif()
endforeach()

# -- 6. a real external consumer of what was just installed ------------------
file(COPY "${PIXELICA_SOURCE_DIR}/cmake/WaterConsumer/" DESTINATION "${water_work}/consumer")
water_step("${CMAKE_COMMAND}" -S "${water_work}/consumer" -B "${water_work}/consumer-build"
  ${water_args} "-DPIXELICA_WATER_EXPECT_3D=${PIXELICA_EXPECT_3D}")
water_step("${CMAKE_COMMAND}" --build "${water_work}/consumer-build" --config "${water_config}"
  --parallel 2)
water_step("${CMAKE_CTEST_COMMAND}" --test-dir "${water_work}/consumer-build"
  -C "${water_config}" --output-on-failure)

message(STATUS "[water-source] OK: the portable source add-on builds against an installed "
               "base SDK, installs a usable component and an external consumer links it")
