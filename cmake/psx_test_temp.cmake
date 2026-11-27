include_guard(GLOBAL)

# Give every test in the calling directory its own temporary folder.
#
# Many tests write fixed names under std::filesystem::temp_directory_path(),
# tempfile.gettempdir() or $TMPDIR (psxrecomp_mouse_roundtrip.toml and so on).
# Two test runs that share one TEMP -- two build trees tested at once, or ctest
# -j with two tests that pick the same name -- then remove or rewrite each
# other's files (PS1B-434: mouse_settings_test exited 0xc0000409 when
# std::filesystem::remove met a file another suite held open).
#
# Call this once at the end of a CMakeLists.txt, after its last add_test. Each
# test gets TEMP, TMP and TMPDIR set to <build>/tt/<n>, created at configure
# time, where n is its place in the directory's test list; tt/index.txt maps n
# to the test name. The folder name is a number, not the test name, because
# some tests build deep trees under TEMP and a Windows path of 260 characters
# or more fails in tools such as MSYS zip (package_setup_program_set_zip did).
# An ENVIRONMENT a test already sets is kept.
function(psx_isolate_test_temp)
    get_property(_tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
    set(_root "${CMAKE_CURRENT_BINARY_DIR}/tt")
    set(_index "")
    set(_n 0)
    foreach(_test IN LISTS _tests)
        math(EXPR _n "${_n} + 1")
        set(_dir "${_root}/${_n}")
        file(MAKE_DIRECTORY "${_dir}")
        file(TO_NATIVE_PATH "${_dir}" _native)
        string(APPEND _index "${_n} ${_test}\n")
        set_property(TEST "${_test}" APPEND PROPERTY ENVIRONMENT
            "TEMP=${_native}" "TMP=${_native}" "TMPDIR=${_dir}")
    endforeach()
    file(WRITE "${_root}/index.txt" "${_index}")
endfunction()
