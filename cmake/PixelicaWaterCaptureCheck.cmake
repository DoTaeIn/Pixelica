# CTest driver for "hello_water_captures" (run with cmake -P).
#
# Runs the raft demo twice into two FRESH directories and checks the framebuffer
# captures are real and time-separated:
#   1. three binary PPM files appear, with a valid P6 256x192 header and exactly
#      the payload that size implies,
#   2. the calm frame, the early wave frame and the late wave frame all differ
#      from each other (the sea moved between the captures),
#   3. a second run with the same arguments writes byte-identical files
#      (deterministic), while a run at a different time offset does not.
#
# Nothing is deleted; every run gets a fresh directory below the work directory.
#
# Input (-D): PIXELICA_SOURCE_DIR, PIXELICA_BINARY_DIR, PIXELICA_WORK_DIR,
#             PIXELICA_WATER_EXE
cmake_minimum_required(VERSION 3.21)

foreach(required PIXELICA_SOURCE_DIR PIXELICA_BINARY_DIR PIXELICA_WORK_DIR PIXELICA_WATER_EXE)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "PixelicaWaterCaptureCheck: ${required} must be set")
  endif()
endforeach()
if(NOT EXISTS "${PIXELICA_WATER_EXE}")
  message(FATAL_ERROR "PixelicaWaterCaptureCheck: '${PIXELICA_WATER_EXE}' does not exist")
endif()

include("${PIXELICA_SOURCE_DIR}/cmake/PixelicaWorkDir.cmake")
pixelica_make_run_dir("${PIXELICA_BINARY_DIR}" "${PIXELICA_SOURCE_DIR}"
                      "${PIXELICA_WORK_DIR}" run_dir)

set(capture_names calm waves_early waves_late)
# 256 x 192 at scale 1: the P6 header plus 3 bytes per pixel.
set(expected_header "P6\n256 192\n255\n")
string(LENGTH "${expected_header}" header_length)
math(EXPR expected_size "${header_length} + 256 * 192 * 3")

# Runs the demo into <run_dir>/<name> and returns the three file hashes.
function(water_capture_run name out_hashes)
  cmake_parse_arguments(ARG "" "" "ARGS" ${ARGN})
  set(dir "${run_dir}/${name}")

  file(MAKE_DIRECTORY "${dir}")
  execute_process(
    COMMAND "${PIXELICA_WATER_EXE}" --out-dir "${dir}" ${ARG_ARGS}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "[water-capture] ${name} failed (${rc})\n${out}\n${err}")
  endif()
  if(NOT out MATCHES "hello_water: ok=1")
    message(FATAL_ERROR "[water-capture] ${name} did not report ok=1\n${out}")
  endif()

  set(hashes "")
  foreach(capture IN LISTS capture_names)
    set(file "${dir}/${capture}.ppm")
    if(NOT EXISTS "${file}")
      message(FATAL_ERROR "[water-capture] ${name}: ${capture}.ppm was not written")
    endif()
    file(SIZE "${file}" size)
    if(NOT size EQUAL expected_size)
      message(FATAL_ERROR
        "[water-capture] ${name}/${capture}.ppm is ${size} bytes, expected ${expected_size}")
    endif()
    # A real binary PPM, not a text file or an OS screenshot.
    file(READ "${file}" head LIMIT ${header_length})
    if(NOT head STREQUAL expected_header)
      message(FATAL_ERROR "[water-capture] ${name}/${capture}.ppm has no P6 256x192 header")
    endif()
    file(SHA256 "${file}" hash)
    list(APPEND hashes "${hash}")
  endforeach()
  set(${out_hashes} "${hashes}" PARENT_SCOPE)
endfunction()

water_capture_run(first first_hashes ARGS --frames 420)

# The three frames must be different images: the sea moved between them.
list(GET first_hashes 0 hash_calm)
list(GET first_hashes 1 hash_early)
list(GET first_hashes 2 hash_late)
if(hash_calm STREQUAL hash_early OR hash_early STREQUAL hash_late
   OR hash_calm STREQUAL hash_late)
  message(FATAL_ERROR "[water-capture] the three captures are not distinct images")
endif()

# Same arguments -> byte-identical captures.
water_capture_run(repeat repeat_hashes ARGS --frames 420)
if(NOT repeat_hashes STREQUAL first_hashes)
  message(FATAL_ERROR "[water-capture] a repeated run produced different captures")
endif()

# A different point on the clock -> a different sea, and still a valid run.
# This is also the capture-level regression test for double time: with a float
# clock the offset run would be bit-identical to the first.
water_capture_run(offset offset_hashes ARGS --frames 420 --time-offset 1234.5)
list(GET offset_hashes 2 offset_late)
if(offset_late STREQUAL hash_late)
  message(FATAL_ERROR "[water-capture] the time offset did not change the late wave frame")
endif()

message(STATUS "[water-capture] OK: 3 distinct deterministic framebuffer PPMs per run "
               "(${expected_size} bytes each), and the clock offset changes the sea")
