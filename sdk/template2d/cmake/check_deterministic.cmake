# CTest helper: the headless game must be deterministic.
#   cmake -DEXE=<pixelica2d_game> -P check_deterministic.cmake
# Two runs with identical arguments print identical output (including the
# frame hash); a run of a different length ends in a different frame.
if(NOT EXE)
  message(FATAL_ERROR "pass -DEXE=<path to pixelica2d_game>")
endif()

function(run_game frames out_var)
  execute_process(COMMAND "${EXE}" --frames ${frames}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "pixelica2d_game --frames ${frames} failed (${rc}): ${err}")
  endif()
  if(NOT out MATCHES "frames=${frames} presented=${frames} fnv1a=[0-9a-f]+")
    message(FATAL_ERROR "unexpected output for --frames ${frames}: ${out}")
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

run_game(30 first)
run_game(30 second)
run_game(60 longer)
if(NOT first STREQUAL second)
  message(FATAL_ERROR "same input, different output:\n${first}\n${second}")
endif()
if(first STREQUAL longer)
  message(FATAL_ERROR "30 and 60 frames ended in the same output; the game is not advancing:\n${first}")
endif()
message(STATUS "deterministic: ${first}")
