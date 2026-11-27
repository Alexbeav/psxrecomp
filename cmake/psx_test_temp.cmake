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
# test gets TEMP, TMP and TMPDIR set to <build>/test-temp/<test name>, created at
# configure time; an ENVIRONMENT a test already sets is kept.
function(psx_isolate_test_temp)
    get_property(_tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
    foreach(_test IN LISTS _tests)
        string(MAKE_C_IDENTIFIER "${_test}" _leaf)
        set(_dir "${CMAKE_CURRENT_BINARY_DIR}/test-temp/${_leaf}")
        file(MAKE_DIRECTORY "${_dir}")
        file(TO_NATIVE_PATH "${_dir}" _native)
        set_property(TEST "${_test}" APPEND PROPERTY ENVIRONMENT
            "TEMP=${_native}" "TMP=${_native}" "TMPDIR=${_dir}")
    endforeach()
endfunction()
