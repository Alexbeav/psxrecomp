# PS1B-434 regression test, run as: cmake -DTEST_NAME=<its ctest name> -P check_test_temp.cmake
# Passes only when ctest gave this test its own TEMP, TMP and TMPDIR under
# <build>/test-temp/<name>, and that folder exists.
cmake_minimum_required(VERSION 3.20)
string(MAKE_C_IDENTIFIER "${TEST_NAME}" _leaf)
foreach(_var IN ITEMS TEMP TMP TMPDIR)
    set(_value "$ENV{${_var}}")
    file(TO_CMAKE_PATH "${_value}" _value)
    if(NOT _value MATCHES "/test-temp/${_leaf}$")
        message(FATAL_ERROR "${_var} is '${_value}', not this test's own test-temp/${_leaf} folder")
    endif()
    if(NOT IS_DIRECTORY "${_value}")
        message(FATAL_ERROR "${_var} folder '${_value}' does not exist")
    endif()
endforeach()
message(STATUS "PASS: TEMP, TMP and TMPDIR are this test's own folder")
