# CTest driver for "sdk_installed_water" (run with cmake -P).
#
# Proves the BINARY water add-on works the way a user receives it:
#   1. install the base SDK components (common, and core when the 3D renderer
#      is built) into a fresh prefix,
#   2. install the water components (water, and water3d when it exists) into
#      the SAME prefix, as unpacking the add-on ZIP over the base SDK does,
#   3. check the add-on replaced no base SDK file (the base loader, the common
#      library and the common headers must be byte-identical afterwards),
#   4. configure, build and run cmake/WaterConsumer against that prefix.
#
# Nothing is ever deleted; every run gets a fresh directory below the work
# directory (cmake/PixelicaWorkDir.cmake).
#
# Input (-D): the PIXELICA_SDK_CHECK_COMMON_ARGS of the root CMakeLists.txt,
#             PIXELICA_SDK_WORK_DIR and PIXELICA_WATER_HAS_3D.
cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_LIST_DIR}/PixelicaWorkDir.cmake")
pixelica_make_run_dir("${PIXELICA_BINARY_DIR}" "${PIXELICA_SOURCE_DIR}"
                      "${PIXELICA_SDK_WORK_DIR}" water_work)

set(water_config "${PIXELICA_CONFIG}")
if(water_config STREQUAL "")
  set(water_config Release)
endif()

function(water_step)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "[water-sdk] step failed (${rc}): ${ARGV}\n${out}\n${err}")
  endif()
endfunction()

set(water_prefix "${water_work}/prefix")
# lib/cmake/Pixelica -> lib/cmake/PixelicaWater (the add-on's own package dir).
get_filename_component(water_cmake_parent "${PIXELICA_INSTALL_CMAKEDIR}" DIRECTORY)
set(water_cmakedir "${water_cmake_parent}/PixelicaWater")

# -- 1. the base SDK, with no water in it ------------------------------------
set(water_base_components common)
if(PIXELICA_EXPECT_3D)
  list(APPEND water_base_components core)
endif()
foreach(component IN LISTS water_base_components)
  water_step("${CMAKE_COMMAND}" --install "${PIXELICA_BINARY_DIR}" --config "${water_config}"
    --prefix "${water_prefix}" --component "${component}")
endforeach()

# The base SDK must not contain a single water file.
file(GLOB_RECURSE water_leaked
  "${water_prefix}/include/pixelica/water*.h"
  "${water_prefix}/*water*.a" "${water_prefix}/*water*.lib"
  "${water_prefix}/${water_cmakedir}/*")
if(water_leaked)
  list(JOIN water_leaked "\n  " joined)
  message(FATAL_ERROR "[water-sdk] the BASE SDK contains water files:\n  ${joined}")
endif()

# Fingerprint the base files the add-on must leave alone.
set(water_guarded "")
file(GLOB_RECURSE water_guarded
  "${water_prefix}/${PIXELICA_INSTALL_CMAKEDIR}/PixelicaPackage.cmake"
  "${water_prefix}/${PIXELICA_INSTALL_CMAKEDIR}/PixelicaConfig.cmake"
  "${water_prefix}/include/pixelica/math.h"
  "${water_prefix}/include/pixelica/status.h")
set(water_before "")
foreach(file IN LISTS water_guarded)
  file(SHA256 "${file}" hash)
  list(APPEND water_before "${hash}")
endforeach()
if(NOT water_before)
  message(FATAL_ERROR "[water-sdk] the base install produced none of the guarded files")
endif()

# -- 2. the add-on, into the same prefix -------------------------------------
set(water_components water)
if(PIXELICA_WATER_HAS_3D)
  list(APPEND water_components water3d)
endif()
foreach(component IN LISTS water_components)
  water_step("${CMAKE_COMMAND}" --install "${PIXELICA_BINARY_DIR}" --config "${water_config}"
    --prefix "${water_prefix}" --component "${component}")
endforeach()

# -- 3. nothing of the base SDK changed --------------------------------------
list(LENGTH water_guarded water_guarded_count)
math(EXPR water_last "${water_guarded_count} - 1")
foreach(index RANGE ${water_last})
  list(GET water_guarded ${index} file)
  list(GET water_before ${index} expected)
  file(SHA256 "${file}" hash)
  if(NOT hash STREQUAL expected)
    message(FATAL_ERROR "[water-sdk] installing the add-on REPLACED a base SDK file: ${file}")
  endif()
endforeach()
foreach(required
    "include/pixelica/water.h"
    "${water_cmakedir}/PixelicaWaterConfig.cmake"
    "${water_cmakedir}/PixelicaWaterTargets.cmake")
  if(NOT EXISTS "${water_prefix}/${required}")
    message(FATAL_ERROR "[water-sdk] the add-on did not install ${required}")
  endif()
endforeach()
if(PIXELICA_WATER_HAS_3D AND NOT EXISTS "${water_prefix}/include/pixelica/water3d.h")
  message(FATAL_ERROR "[water-sdk] the water3d component did not install its header")
endif()

# -- 4. a real external consumer ---------------------------------------------
file(COPY "${PIXELICA_SOURCE_DIR}/cmake/WaterConsumer/" DESTINATION "${water_work}/consumer")
set(water_args -G "${PIXELICA_SDK_GENERATOR}" "-DCMAKE_BUILD_TYPE=${water_config}"
  "-DCMAKE_PREFIX_PATH=${water_prefix}"
  "-DPIXELICA_WATER_EXPECT_3D=${PIXELICA_WATER_HAS_3D}"
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
water_step("${CMAKE_COMMAND}" -S "${water_work}/consumer" -B "${water_work}/build" ${water_args})
water_step("${CMAKE_COMMAND}" --build "${water_work}/build" --config "${water_config}"
  --parallel 2)
water_step("${CMAKE_CTEST_COMMAND}" --test-dir "${water_work}/build" -C "${water_config}"
  --output-on-failure)
message(STATUS "[water-sdk] OK: binary add-on installs beside the base SDK and an external "
               "consumer builds and runs (components: ${water_components})")
