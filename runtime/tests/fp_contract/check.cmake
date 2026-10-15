# PS1B-490: the runtime target must be built with float contraction off, or an
# arm64 Mac fuses a*b+c and picks other texels than a PC (VRAM is part of the
# compared netplay state). On x86-64 GCC the flag changes no machine code, so no
# replay or unit test notices if a merge drops it. This test does.
#
# Inputs (all -D, passed by runtime/CMakeLists.txt):
#   OPTIONS_FILE  one COMPILE_OPTIONS entry of the psx-runtime target per line,
#                 as written at configure time (generator expressions unexpanded)
#   C_ID, C_FRONTEND, CXX_ID, CXX_FRONTEND  compiler id and frontend variant
#
# The expected spelling is derived here from the compiler, not read from
# runtime.cmake: clang-cl (frontend MSVC) ignores a plain -ffp-contract=off with
# only a warning, so it needs /clang:-ffp-contract=off. MSVC cl.exe does not
# contract by default and gets no flag. Standard CMake only.

foreach(var OPTIONS_FILE C_ID CXX_ID)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "fp_contract_flag_test: -D${var}= not given")
    endif()
endforeach()
if(NOT EXISTS "${OPTIONS_FILE}")
    message(FATAL_ERROR "fp_contract_flag_test: ${OPTIONS_FILE} does not exist")
endif()
file(READ "${OPTIONS_FILE}" _options)

set(_checked 0)
set(_failed "")
foreach(lang C CXX)
    if(NOT ${lang}_ID MATCHES "Clang|GNU")
        message(STATUS "fp_contract_flag_test: ${lang} compiler '${${lang}_ID}' takes no flag, skipped")
        continue()
    endif()
    if(${lang}_FRONTEND STREQUAL "MSVC")
        set(_flag "/clang:-ffp-contract=off")
    else()
        set(_flag "-ffp-contract=off")
    endif()
    set(_entry "$<$<COMPILE_LANGUAGE:${lang}>:${_flag}>")
    math(EXPR _checked "${_checked} + 1")
    string(FIND "${_options}" "${_entry}" _pos)
    if(_pos EQUAL -1)
        string(APPEND _failed "  ${lang}: psx-runtime COMPILE_OPTIONS lacks ${_entry}\n")
    else()
        message(STATUS "fp_contract_flag_test: ${lang} has ${_flag}")
    endif()
endforeach()

if(_failed)
    message(FATAL_ERROR
        "fp_contract_flag_test: float contraction is not off in the runtime build (PS1B-490).\n"
        "${_failed}"
        "Without it an arm64 build fuses a*b+c and netplay VRAM differs from a PC's.\n"
        "options seen:\n${_options}")
endif()
message(STATUS "fp_contract_flag_test: ${_checked} language(s) checked")
