# CTest driver for "asset_reproducibility" (run with cmake -P).
#
# Runs tools/pixelica_pack/generate_demo_assets.py twice into two empty temporary
# roots (fresh directories; from a neutral working directory, so there is no
# dependency on the current directory) and checks that
#   - both runs produce the same set of files with identical bytes, and
#   - every generated file is byte-identical to the one committed under assets/.
#
# Inputs (-D): PIXELICA_PYTHON PIXELICA_SOURCE_DIR PIXELICA_BINARY_DIR PIXELICA_WORK_DIR
#
# Nothing is ever deleted. PIXELICA_WORK_DIR must resolve (".." and symlinks
# resolved) to a directory strictly inside PIXELICA_BINARY_DIR, which must be a CMake
# build directory; each run then uses a fresh run-<time>-<random> child of it
# (see PixelicaWorkDir.cmake). Anything else is refused before any file is written.
cmake_minimum_required(VERSION 3.21)

foreach(var PIXELICA_PYTHON PIXELICA_SOURCE_DIR PIXELICA_BINARY_DIR PIXELICA_WORK_DIR)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "PixelicaAssetRepro: ${var} is required")
  endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/PixelicaWorkDir.cmake")

file(TO_CMAKE_PATH "${PIXELICA_SOURCE_DIR}" source_dir)
set(generator "${source_dir}/tools/pixelica_pack/generate_demo_assets.py")
if(NOT EXISTS "${generator}")
  message(FATAL_ERROR "[asset-repro] missing ${generator}")
endif()

pixelica_make_run_dir("${PIXELICA_BINARY_DIR}" "${PIXELICA_SOURCE_DIR}" "${PIXELICA_WORK_DIR}" work_dir)
message(STATUS "[asset-repro] run directory ${work_dir}")
file(MAKE_DIRECTORY "${work_dir}/cwd")

foreach(run a b)
  message(STATUS "[asset-repro] generate run ${run}")
  file(MAKE_DIRECTORY "${work_dir}/${run}")
  execute_process(
    COMMAND "${PIXELICA_PYTHON}" -B "${generator}" --output-root "${work_dir}/${run}"
    WORKING_DIRECTORY "${work_dir}/cwd"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "[asset-repro] generator failed on run ${run} (exit ${rc})\n${out}\n${err}")
  endif()
endforeach()

# The generator may write <root>/assets/... or directly <root>/...
function(pixelica_resolve_root run out_var)
  if(IS_DIRECTORY "${work_dir}/${run}/assets")
    set(${out_var} "${work_dir}/${run}/assets" PARENT_SCOPE)
  else()
    set(${out_var} "${work_dir}/${run}" PARENT_SCOPE)
  endif()
endfunction()
pixelica_resolve_root(a root_a)
pixelica_resolve_root(b root_b)

file(GLOB_RECURSE files_a RELATIVE "${root_a}" "${root_a}/*")
file(GLOB_RECURSE files_b RELATIVE "${root_b}" "${root_b}/*")
list(SORT files_a)
list(SORT files_b)
list(LENGTH files_a count_a)
if(count_a EQUAL 0)
  message(FATAL_ERROR "[asset-repro] the generator produced no files under ${root_a}")
endif()
if(NOT "${files_a}" STREQUAL "${files_b}")
  message(FATAL_ERROR "[asset-repro] the two runs produced different file sets:\nA: ${files_a}\nB: ${files_b}")
endif()

set(committed_root "${source_dir}/assets")
set(mismatches "")
foreach(relative IN LISTS files_a)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
    "${root_a}/${relative}" "${root_b}/${relative}" RESULT_VARIABLE same_runs)
  if(NOT same_runs EQUAL 0)
    string(APPEND mismatches "  run A != run B: ${relative}\n")
  endif()
  if(NOT EXISTS "${committed_root}/${relative}")
    string(APPEND mismatches "  not committed under assets/: ${relative}\n")
  else()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
      "${root_a}/${relative}" "${committed_root}/${relative}" RESULT_VARIABLE same_committed)
    if(NOT same_committed EQUAL 0)
      string(APPEND mismatches "  differs from committed assets/${relative}\n")
    endif()
  endif()
endforeach()

if(NOT mismatches STREQUAL "")
  message(FATAL_ERROR "[asset-repro] reproducibility failures:\n${mismatches}")
endif()
# The generator's own --check (in-memory regeneration vs the files on disk).
execute_process(
  COMMAND "${PIXELICA_PYTHON}" -B "${generator}" --output-root "${source_dir}" --check
  WORKING_DIRECTORY "${work_dir}/cwd"
  RESULT_VARIABLE check_rc OUTPUT_VARIABLE check_out ERROR_VARIABLE check_err)
if(NOT check_rc EQUAL 0)
  message(FATAL_ERROR "[asset-repro] generate_demo_assets.py --check failed (exit ${check_rc})\n${check_out}\n${check_err}")
endif()

list(LENGTH files_a total)
message(STATUS "[asset-repro] OK: ${total} files identical across two runs and to the committed assets")
