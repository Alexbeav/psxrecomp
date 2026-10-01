# Which psxrecomp commit a build was made from, and the netplay lobby key that
# follows from it (PS1B-287, PS1B-295).
#
# A release is built from a `git archive` snapshot with no .git, on the build
# host and again on the player's machine by first-run setup. `git rev-parse`
# finds nothing there, so those builds carried an empty pin: replays could not
# warn about a different build, and the lobby seated builds of different
# commits in one room, where they desync.
#
# runtime/FRAMEWORK_PIN is marked export-subst in .gitattributes, so
# `git archive` writes the commit into it. tools/stage_framework_tree.sh does
# the same when it stages a checkout. In a checkout the file holds its
# placeholder and git answers instead, as before.

# psxrecomp_framework_identity(<framework root> <out pin> <out rev> <out source>)
#
# pin    - the full commit, or "" when nothing names one.
# rev    - a short build id for crash reports; "unknown" when nothing names one.
# source - caller | archive | git | none: where the pin came from.
#
# A caller-supplied -DPSX_FRAMEWORK_PIN=<40 hex> or -DPSX_GIT_REV=<text> wins.
function(psxrecomp_framework_identity root out_pin out_rev out_source)
    set(_pin "")
    set(_source "none")
    if(DEFINED PSX_FRAMEWORK_PIN AND NOT "${PSX_FRAMEWORK_PIN}" STREQUAL "")
        if(NOT "${PSX_FRAMEWORK_PIN}" MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR
                "PSX_FRAMEWORK_PIN must be a full lowercase commit hash "
                "(got \"${PSX_FRAMEWORK_PIN}\").")
        endif()
        string(LENGTH "${PSX_FRAMEWORK_PIN}" _len)
        if(NOT _len EQUAL 40)
            message(FATAL_ERROR
                "PSX_FRAMEWORK_PIN must be a full lowercase commit hash "
                "(got \"${PSX_FRAMEWORK_PIN}\").")
        endif()
        set(_pin "${PSX_FRAMEWORK_PIN}")
        set(_source "caller")
    endif()
    if(_pin STREQUAL "" AND EXISTS "${root}/runtime/FRAMEWORK_PIN")
        file(STRINGS "${root}/runtime/FRAMEWORK_PIN" _lines LIMIT_COUNT 1)
        string(STRIP "${_lines}" _line)
        string(LENGTH "${_line}" _len)
        if(_len EQUAL 40 AND "${_line}" MATCHES "^[0-9a-f]+$")
            set(_pin "${_line}")
            set(_source "archive")
        endif()
    endif()
    if(_pin STREQUAL "")
        execute_process(
            COMMAND git -C "${root}" rev-parse HEAD
            OUTPUT_VARIABLE _git_pin OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT "${_git_pin}" STREQUAL "")
            set(_pin "${_git_pin}")
            set(_source "git")
        endif()
    endif()

    set(_rev "")
    if(DEFINED PSX_GIT_REV AND NOT "${PSX_GIT_REV}" STREQUAL "")
        set(_rev "${PSX_GIT_REV}")
    elseif(_source STREQUAL "git")
        execute_process(
            COMMAND git -C "${root}" describe --always --dirty --tags
            OUTPUT_VARIABLE _rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if("${_rev}" STREQUAL "" AND NOT _pin STREQUAL "")
        string(SUBSTRING "${_pin}" 0 9 _rev)
    endif()
    if("${_rev}" STREQUAL "")
        set(_rev "unknown")
    endif()

    set(${out_pin} "${_pin}" PARENT_SCOPE)
    set(${out_rev} "${_rev}" PARENT_SCOPE)
    set(${out_source} "${_source}" PARENT_SCOPE)
endfunction()

# psxrecomp_lobby_version(<game version> <framework pin> <out>)
#
# The string the lobby matches rooms on. Peers must run the same emulation
# code, and the kit VERSION alone does not say that: two builds of one kit on
# different framework commits share a VERSION. So a build whose pin is known
# matches on "<version>-<first 8 of the pin>".
#
# "dev" stays "dev": a non-release build lists every room of its title.
# -DPSX_NET_BUILD_KEY=<text> replaces the pin part; =off drops it.
# VERSION and psx_game_version.txt do not change.
function(psxrecomp_lobby_version version pin out)
    set(_key "")
    if(DEFINED PSX_NET_BUILD_KEY AND NOT "${PSX_NET_BUILD_KEY}" STREQUAL "")
        string(TOLOWER "${PSX_NET_BUILD_KEY}" _key_lower)
        if(NOT _key_lower STREQUAL "off")
            set(_key "${PSX_NET_BUILD_KEY}")
        endif()
    elseif(NOT "${pin}" STREQUAL "")
        string(SUBSTRING "${pin}" 0 8 _key)
    endif()
    if(_key STREQUAL "" OR "${version}" STREQUAL "dev")
        set(${out} "${version}" PARENT_SCOPE)
        return()
    endif()
    if(NOT "${_key}" MATCHES "^[A-Za-z0-9._-]+$")
        message(FATAL_ERROR
            "PSX_NET_BUILD_KEY may hold only letters, digits, '.', '_' and '-' "
            "(got \"${_key}\").")
    endif()
    if("${version}" MATCHES "\\$<")
        # Multi-config: the version is "$<IF:$<CONFIG:Release>,<version>,dev>".
        set(${out} "${version}$<$<CONFIG:Release>:-${_key}>" PARENT_SCOPE)
        return()
    endif()
    set(_lobby "${version}-${_key}")
    string(LENGTH "${_lobby}" _len)
    # PSX_LOBBY_VERSION_LEN is 32 with its terminator. A longer string would be
    # cut, and two different keys could then compare equal.
    if(_len GREATER 31)
        message(FATAL_ERROR
            "The lobby version \"${_lobby}\" is ${_len} characters; the limit "
            "is 31. Shorten VERSION or PSX_NET_BUILD_KEY.")
    endif()
    set(${out} "${_lobby}" PARENT_SCOPE)
endfunction()
