# ctest -R framework_identity_test
#
# PS1B-287 / PS1B-295: a build knows its framework commit without a .git, and
# the lobby version carries it. Run as:
#   cmake -DWORK=<scratch dir> -P check.cmake
# A CASE value runs one configuration that must stop with an error.
cmake_minimum_required(VERSION 3.20)

get_filename_component(PSXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${PSXRECOMP_ROOT}/runtime/framework_identity.cmake")

set(PIN_A "0123456789abcdef0123456789abcdef01234567")
set(PIN_B "89abcdef0123456789abcdef0123456789abcdef")

if(DEFINED CASE)
    if(CASE STREQUAL "bad_pin")
        set(PSX_FRAMEWORK_PIN "v1.2.3")
        psxrecomp_framework_identity("${WORK}" pin rev source)
    elseif(CASE STREQUAL "short_pin")
        set(PSX_FRAMEWORK_PIN "0123456")
        psxrecomp_framework_identity("${WORK}" pin rev source)
    elseif(CASE STREQUAL "long_lobby")
        set(PSX_NET_BUILD_KEY "a-release-key-that-is-far-too-long")
        psxrecomp_lobby_version("0.1.2" "${PIN_A}" lobby)
    elseif(CASE STREQUAL "bad_key")
        set(PSX_NET_BUILD_KEY "two words")
        psxrecomp_lobby_version("0.1.2" "${PIN_A}" lobby)
    else()
        message(FATAL_ERROR "unknown CASE ${CASE}")
    endif()
    return()
endif()

if(NOT WORK)
    message(FATAL_ERROR "pass -DWORK=<scratch dir>")
endif()

set(failures 0)
function(expect what got want)
    if(NOT "${got}" STREQUAL "${want}")
        message(SEND_ERROR "${what}: got \"${got}\", want \"${want}\"")
        math(EXPR n "${failures} + 1")
        set(failures ${n} PARENT_SCOPE)
    endif()
endfunction()

# A snapshot: no .git, and git archive wrote the commit into FRAMEWORK_PIN.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/snapshot/runtime")
file(WRITE "${WORK}/snapshot/runtime/FRAMEWORK_PIN" "${PIN_A}\n")
psxrecomp_framework_identity("${WORK}/snapshot" pin rev source)
expect("snapshot pin" "${pin}" "${PIN_A}")
expect("snapshot source" "${source}" "archive")
expect("snapshot rev" "${rev}" "012345678")

# A caller's value wins over the file.
set(PSX_FRAMEWORK_PIN "${PIN_B}")
set(PSX_GIT_REV "release-7")
psxrecomp_framework_identity("${WORK}/snapshot" pin rev source)
expect("caller pin" "${pin}" "${PIN_B}")
expect("caller source" "${source}" "caller")
expect("caller rev" "${rev}" "release-7")
unset(PSX_FRAMEWORK_PIN)
unset(PSX_GIT_REV)

# A checkout: the file still holds its placeholder, so it names nothing. git
# may or may not answer for this scratch folder; the placeholder must not leak.
file(MAKE_DIRECTORY "${WORK}/checkout/runtime")
file(WRITE "${WORK}/checkout/runtime/FRAMEWORK_PIN" "$Format:%H$\n")
psxrecomp_framework_identity("${WORK}/checkout" pin rev source)
if(source STREQUAL "archive" OR pin MATCHES "Format")
    message(SEND_ERROR "placeholder was read as a pin: \"${pin}\" (${source})")
    math(EXPR failures "${failures} + 1")
endif()
if(pin STREQUAL "" AND NOT rev STREQUAL "unknown")
    message(SEND_ERROR "no pin, but rev is \"${rev}\"")
    math(EXPR failures "${failures} + 1")
endif()

# The tree this test runs from carries the file and its attribute.
if(NOT EXISTS "${PSXRECOMP_ROOT}/runtime/FRAMEWORK_PIN")
    message(SEND_ERROR "runtime/FRAMEWORK_PIN is missing")
    math(EXPR failures "${failures} + 1")
endif()
if(EXISTS "${PSXRECOMP_ROOT}/.gitattributes")
    file(READ "${PSXRECOMP_ROOT}/.gitattributes" attributes)
    if(NOT attributes MATCHES "runtime/FRAMEWORK_PIN[^\n]*export-subst")
        message(SEND_ERROR ".gitattributes does not mark runtime/FRAMEWORK_PIN export-subst")
        math(EXPR failures "${failures} + 1")
    endif()
endif()

# The lobby version.
psxrecomp_lobby_version("0.1.2" "${PIN_A}" lobby)
expect("release with a pin" "${lobby}" "0.1.2-01234567")
psxrecomp_lobby_version("0.1.2" "${PIN_B}" lobby)
expect("another pin" "${lobby}" "0.1.2-89abcdef")
psxrecomp_lobby_version("0.1.2" "" lobby)
expect("no pin" "${lobby}" "0.1.2")
psxrecomp_lobby_version("dev" "${PIN_A}" lobby)
expect("dev build" "${lobby}" "dev")
set(PSX_NET_BUILD_KEY "off")
psxrecomp_lobby_version("0.1.2" "${PIN_A}" lobby)
expect("key off" "${lobby}" "0.1.2")
set(PSX_NET_BUILD_KEY "wave1.SLUS-00605")
psxrecomp_lobby_version("0.1.2" "${PIN_A}" lobby)
expect("caller key" "${lobby}" "0.1.2-wave1.SLUS-00605")
psxrecomp_lobby_version("dev" "${PIN_A}" lobby)
expect("caller key, dev build" "${lobby}" "dev")
unset(PSX_NET_BUILD_KEY)
psxrecomp_lobby_version("$<IF:$<CONFIG:Release>,0.1.2,dev>" "${PIN_A}" lobby)
expect("multi-config" "${lobby}"
    "$<IF:$<CONFIG:Release>,0.1.2,dev>$<$<CONFIG:Release>:-01234567>")

# Configurations that must stop.
foreach(case bad_pin short_pin long_lobby bad_key)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DWORK=${WORK}/snapshot" "-DCASE=${case}"
                -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    if(result EQUAL 0)
        message(SEND_ERROR "${case}: expected the configure to stop")
        math(EXPR failures "${failures} + 1")
    endif()
endforeach()

if(failures)
    message(FATAL_ERROR "framework_identity_test: ${failures} failure(s)")
endif()
message(STATUS "framework_identity_test: passed")
