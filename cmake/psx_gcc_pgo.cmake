# GCC profile-guided optimisation options, shared by the whole-runtime
# PSX_PGO build (runtime.cmake) and the selective hot-path PGO
# (psx_runtime_hot_pgo.cmake) (PS1B-574).
#
# With -fprofile-generate=<dir>, GCC names each profile <dir>/<the object's
# absolute path, mangled>. From a deep build folder on Windows that name passes
# 260 characters, and the instrumented program cannot create it: libgcov
# writes no .gcda at all, and the use build then compiles without a profile.
# -fprofile-prefix-path drops the build folder from the mangled part, so the
# name is <dir>/<path below the build folder>. MinGW records the object path
# with native separators, and GCC compares the prefix literally, so the prefix
# is given in native form.
#
# psx_gcc_pgo_options(<generate|use> <profile dir> <compile var> <link var>)
function(psx_gcc_pgo_options mode profile_dir out_compile out_link)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _prefix)
    if(mode STREQUAL "generate")
        set(_compile "-fprofile-generate=${profile_dir}" "-fprofile-prefix-path=${_prefix}")
        set(_link "-fprofile-generate=${profile_dir}")
    elseif(mode STREQUAL "use")
        set(_compile "-fprofile-use=${profile_dir}" "-fprofile-prefix-path=${_prefix}")
        set(_link "-fprofile-use=${profile_dir}")
    else()
        message(FATAL_ERROR "psx_gcc_pgo_options: mode must be generate or use (got '${mode}')")
    endif()
    set(${out_compile} "${_compile}" PARENT_SCOPE)
    set(${out_link} "${_link}" PARENT_SCOPE)
endfunction()
