# PS1B-574: a GCC PGO build from a deep folder wrote no profile. GCC named
# each .gcda <profile dir>/<absolute object path, mangled>; past 260
# characters on Windows libgcov could not create it, and the use build hid the
# loss behind -Wno-missing-profile.
#
# This script builds a two-file program with psx_gcc_pgo_options() from a
# build folder of 180 characters, trains it, and requires one .gcda per
# object. It then rebuilds in use mode with -Werror=missing-profile, which
# fails when a profile is missing.
#
# cmake -DPSXRECOMP_ROOT=<repo> -DCC=<gcc> -DGEN=<generator>
#       [-DMAKE=<make program>] -DWORK=<scratch folder> -P <this file>

foreach(_v PSXRECOMP_ROOT CC GEN WORK)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "gcc_pgo_deep_folder_test: -D${_v} is required")
    endif()
endforeach()

set(_build_length 180)
string(LENGTH "${WORK}" _work_length)
if(_work_length GREATER 140)
    # The test needs room below WORK for a build folder of 180 characters.
    message("SKIP: the scratch folder is ${_work_length} characters; this test needs 140 or fewer")
    cmake_language(EXIT 77)
endif()

file(REMOVE_RECURSE "${WORK}")
set(_src "${WORK}/src")
file(MAKE_DIRECTORY "${_src}/nested/folder")
file(WRITE "${_src}/nested/folder/a.c" "int pgo_a(int x) { return x > 3 ? x * 2 : x + 1; }\n")
file(WRITE "${_src}/nested/folder/b.c"
"int pgo_a(int);
int main(void) { int s = 0; for (int i = 0; i < 100; ++i) s += pgo_a(i); return s == 9898 ? 0 : 1; }
")
file(WRITE "${_src}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.16)
project(pgo_deep_folder C)
include(\"${PSXRECOMP_ROOT}/cmake/psx_gcc_pgo.cmake\")
add_executable(pgo_deep nested/folder/a.c nested/folder/b.c)
psx_gcc_pgo_options(\${PSX_PGO} \"\${CMAKE_BINARY_DIR}/pgo\" _compile _link)
target_compile_options(pgo_deep PRIVATE \${_compile} -Werror=missing-profile)
target_link_options(pgo_deep PRIVATE \${_link})
")

# A build folder of exactly 180 characters.
set(_build "${WORK}/b")
string(LENGTH "${_build}" _len)
while(_len LESS ${_build_length})
    math(EXPR _pad "${_build_length} - ${_len} - 1")
    if(_pad GREATER 40)
        set(_pad 40)
    endif()
    if(_pad LESS 1)
        set(_build "${_build}x")
    else()
        string(REPEAT "p" ${_pad} _seg)
        set(_build "${_build}/${_seg}")
    endif()
    string(LENGTH "${_build}" _len)
endwhile()
message("build folder: ${_len} characters")

set(_make_arg)
if(DEFINED MAKE AND NOT MAKE STREQUAL "")
    set(_make_arg "-DCMAKE_MAKE_PROGRAM=${MAKE}")
endif()

function(run_step what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "FAIL: ${what} (exit ${_rc})\n${_out}")
    endif()
endfunction()

run_step("configure the instrumented build"
    "${CMAKE_COMMAND}" -S "${_src}" -B "${_build}" -G "${GEN}" ${_make_arg}
    "-DCMAKE_C_COMPILER=${CC}" -DCMAKE_BUILD_TYPE=Release -DPSX_PGO=generate)
run_step("build the instrumented program" "${CMAKE_COMMAND}" --build "${_build}")
file(GLOB_RECURSE _exe "${_build}/pgo_deep" "${_build}/pgo_deep.exe")
list(LENGTH _exe _n_exe)
if(NOT _n_exe EQUAL 1)
    message(FATAL_ERROR "FAIL: the instrumented program was not found under ${_build}")
endif()
run_step("run the instrumented program" "${_exe}")

# -fprofile-generate alone writes no .gcno, so the objects are the count.
file(GLOB_RECURSE _objs "${_build}/CMakeFiles/pgo_deep.dir/*.obj" "${_build}/CMakeFiles/pgo_deep.dir/*.o")
file(GLOB_RECURSE _gcda "${_build}/*.gcda")
list(LENGTH _objs _n_objs)
list(LENGTH _gcda _n_gcda)
message("profiles: ${_n_gcda} .gcda for ${_n_objs} instrumented objects")
# On a host where long paths are on (Eagle), libgcov can write a name past
# 260 characters, so the count alone does not show the fault there. The
# length does: a host with long paths off (Athena, LongPathsEnabled 0) cannot
# create any name of 260 characters or more.
set(_longest 0)
foreach(_f IN LISTS _gcda)
    file(TO_NATIVE_PATH "${_f}" _native)
    string(LENGTH "${_native}" _fl)
    message("  ${_fl} characters: ${_f}")
    if(_fl GREATER _longest)
        set(_longest ${_fl})
    endif()
endforeach()
if(NOT _n_objs EQUAL 2 OR NOT _n_gcda EQUAL 2)
    message(FATAL_ERROR "FAIL: training wrote ${_n_gcda} of ${_n_objs} profiles from a ${_len}-character build folder")
endif()
if(NOT _longest LESS 260)
    message(FATAL_ERROR "FAIL: the longest profile name is ${_longest} characters; a host without long paths cannot write it")
endif()

run_step("configure the use build" "${CMAKE_COMMAND}" -S "${_src}" -B "${_build}" -DPSX_PGO=use)
run_step("build with -fprofile-use and -Werror=missing-profile" "${CMAKE_COMMAND}" --build "${_build}" --clean-first)
run_step("run the optimised program" "${_exe}")

file(REMOVE_RECURSE "${WORK}")
message("gcc_pgo_deep_folder_test: 2 of 2 profiles written and used")
