# CTest driver for "lifecycle_checkpoint_restart" (run with cmake -P).
#
# The one thing a single-process unit test cannot show: a checkpoint written
# by one process is restored by a DIFFERENT process started later. The driver
# runs examples/hello_lifecycle several times against one save directory and
# checks what each fresh run sees.
#
#   1. first run, empty directory      -> restored=0, saves 10 points on suspend
#   2. second process                  -> restored=1, continues at 10, shuts down at 15
#   3. a run that is "killed"          -> restored=1, its 7 extra points are NOT stored
#   4. the next process                -> still 15, proving step 3 persisted nothing
#   5. pausing really stops the game   -> paused steps add nothing
#   6. a corrupted save file           -> restored=0 with bad_asset, the game starts over
#   7. a hostile game id               -> refused before anything touches the disk
#   8. a save root that is a file      -> the checkpoint fails and the run reports it
#
# Nothing is ever deleted: each run gets a fresh directory below the work
# directory (cmake/PixelicaWorkDir.cmake).
#
# Input (-D): PIXELICA_SOURCE_DIR, PIXELICA_BINARY_DIR, PIXELICA_WORK_DIR,
#             PIXELICA_LIFECYCLE_EXE
cmake_minimum_required(VERSION 3.21)

foreach(required PIXELICA_SOURCE_DIR PIXELICA_BINARY_DIR PIXELICA_WORK_DIR PIXELICA_LIFECYCLE_EXE)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "PixelicaLifecycleCheck: ${required} must be set")
  endif()
endforeach()
if(NOT EXISTS "${PIXELICA_LIFECYCLE_EXE}")
  message(FATAL_ERROR "PixelicaLifecycleCheck: '${PIXELICA_LIFECYCLE_EXE}' does not exist")
endif()

include("${PIXELICA_SOURCE_DIR}/cmake/PixelicaWorkDir.cmake")
pixelica_make_run_dir("${PIXELICA_BINARY_DIR}" "${PIXELICA_SOURCE_DIR}"
                      "${PIXELICA_WORK_DIR}" run_dir)

set(saves "${run_dir}/saves")

# Runs the example and stores its first stdout line in `last_line`.
# EXPECT_RC is the required exit code (default 0).
function(lifecycle_run label)
  cmake_parse_arguments(ARG "" "EXPECT_RC" "ARGS" ${ARGN})
  if(NOT DEFINED ARG_EXPECT_RC)
    set(ARG_EXPECT_RC 0)
  endif()
  execute_process(
    COMMAND "${PIXELICA_LIFECYCLE_EXE}" ${ARG_ARGS}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
  if(NOT rc EQUAL ARG_EXPECT_RC)
    message(FATAL_ERROR
      "[lifecycle] ${label}: exit code ${rc}, expected ${ARG_EXPECT_RC}\n"
      "  args:   ${ARG_ARGS}\n  stdout: ${out}\n  stderr: ${err}")
  endif()
  message(STATUS "[lifecycle] ${label}: ${out}")
  set(last_line "${out}" PARENT_SCOPE)
  set(last_err "${err}" PARENT_SCOPE)
endfunction()

# Fails unless `text` contains every given fragment.
function(lifecycle_expect label text)
  foreach(fragment IN LISTS ARGN)
    string(FIND "${text}" "${fragment}" position)
    if(position LESS 0)
      message(FATAL_ERROR "[lifecycle] ${label}: expected '${fragment}' in:\n  ${text}")
    endif()
  endforeach()
endfunction()

# 1. A cold start in an empty directory: nothing to restore, 10 steps, then the
#    host backgrounds the game and the process never comes back.
lifecycle_run("cold start, suspend" ARGS --dir "${saves}" --steps 10 --exit suspend)
lifecycle_expect("cold start, suspend" "${last_line}"
  "restored=0" "restore=not_found" "score=10" "runs=1"
  "state=suspended" "checkpoint=ok")

set(save_file "${saves}/hello-lifecycle/save00.pxsv")
if(NOT EXISTS "${save_file}")
  message(FATAL_ERROR "[lifecycle] the suspend did not create ${save_file}")
endif()
file(SIZE "${save_file}" save_size)
# 12-byte payload + the 20-byte envelope of pixelica/save.h.
if(NOT save_size EQUAL 32)
  message(FATAL_ERROR "[lifecycle] ${save_file} is ${save_size} bytes, expected 32")
endif()
if(EXISTS "${save_file}.tmp")
  message(FATAL_ERROR "[lifecycle] the atomic write left ${save_file}.tmp behind")
endif()

# 2. A SECOND PROCESS continues from the checkpoint and quits in an orderly way.
lifecycle_run("restart, shutdown" ARGS --dir "${saves}" --steps 5 --exit shutdown)
lifecycle_expect("restart, shutdown" "${last_line}"
  "restored=1" "restore=ok" "score=15" "runs=2" "state=stopped" "checkpoint=ok")

# 3. A run that is terminated without any lifecycle call stores nothing...
lifecycle_run("killed run" ARGS --dir "${saves}" --steps 7 --exit kill)
lifecycle_expect("killed run" "${last_line}" "restored=1" "score=22" "state=running")

# 4. ... which the next process proves: it still starts from 15 (and is the
#    third process to open this save).
lifecycle_run("after the kill" ARGS --dir "${saves}" --steps 0 --exit kill)
lifecycle_expect("after the kill" "${last_line}" "restored=1" "score=15" "runs=3")

# 5. Pausing stops the simulation: 6 steps with a pause at step 2 advance 4.
lifecycle_run("pause skips steps"
  ARGS --dir "${saves}" --steps 6 --pause-at 2 --exit shutdown)
lifecycle_expect("pause skips steps" "${last_line}"
  "restored=1" "score=19" "skipped=2" "state=stopped")

# 6. Corruption is caught by the envelope CRC/length, not trusted: a fresh
#    process starts from the game's defaults instead of loading garbage.
file(APPEND "${save_file}" "!")
lifecycle_run("corrupt save" ARGS --dir "${saves}" --steps 3 --exit shutdown)
lifecycle_expect("corrupt save" "${last_line}"
  "restored=0" "restore=bad_asset" "score=3" "runs=1" "checkpoint=ok")
# The orderly shutdown above replaced the corrupt file, so the next run loads.
lifecycle_run("after the repair" ARGS --dir "${saves}" --steps 1 --exit shutdown)
lifecycle_expect("after the repair" "${last_line}" "restored=1" "score=4")

# 7. Invalid game ids never reach the filesystem (exit code 2 = usage error).
foreach(bad_id "../../etc" ".." "a/b" "/abs" "Upper" "con")
  lifecycle_run("reject id '${bad_id}'" EXPECT_RC 2
    ARGS --dir "${saves}" --game-id "${bad_id}" --steps 1 --exit shutdown)
  lifecycle_expect("reject id '${bad_id}'" "${last_err}" "not a valid game id")
endforeach()
file(GLOB stray "${saves}/*")
list(LENGTH stray stray_count)
if(NOT stray_count EQUAL 1)
  message(FATAL_ERROR
    "[lifecycle] a rejected game id created something: ${stray}")
endif()

# 8. An I/O failure is reported, not hidden: a save root that is a regular file
#    cannot hold the game directory, so the checkpoint fails (exit code 4) while
#    the run itself still completes.
set(blocked "${run_dir}/not-a-directory")
file(WRITE "${blocked}" "this is a file\n")
lifecycle_run("unwritable root" EXPECT_RC 4
  ARGS --dir "${blocked}" --steps 2 --exit shutdown)
# (the exact restore status differs between platforms - ENOTDIR on Unix,
#  ENOENT on Windows - so only "nothing was restored" is asserted)
lifecycle_expect("unwritable root" "${last_line}"
  "restored=0" "checkpoint=platform" "state=stopped" "score=2")

# 9. A different slot is a different save in the same game directory.
lifecycle_run("slot 5 is empty" ARGS --dir "${saves}" --slot 5 --steps 2 --exit shutdown)
lifecycle_expect("slot 5 is empty" "${last_line}" "restored=0" "score=2")
if(NOT EXISTS "${saves}/hello-lifecycle/save05.pxsv")
  message(FATAL_ERROR "[lifecycle] slot 5 was not written to save05.pxsv")
endif()
lifecycle_run("slot 0 is unchanged" ARGS --dir "${saves}" --steps 0 --exit kill)
lifecycle_expect("slot 0 is unchanged" "${last_line}" "restored=1" "score=4")

message(STATUS "[lifecycle] OK: checkpoints survive process restarts, kills store "
               "nothing, corruption and bad ids are refused")
