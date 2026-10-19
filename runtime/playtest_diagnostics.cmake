include_guard(GLOBAL)

# One implementation shared by every title; the mod manifest owns its defaults.
# Call after psxrecomp_add_game_runtime. This also covers its PGXP sibling.
function(psxrecomp_add_playtest_diagnostics target)
    cmake_parse_arguments(DIAG "" "GAME_ID;MOD_ID" "" ${ARGN})
    if(DIAG_UNPARSED_ARGUMENTS OR NOT DIAG_GAME_ID MATCHES "^[A-Za-z0-9_-]+$"
       OR NOT DIAG_MOD_ID MATCHES "^[a-z0-9._-]+$")
        message(FATAL_ERROR "playtest diagnostics requires GAME_ID and MOD_ID")
    endif()
    set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    set(_version "development")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _version LIMIT_COUNT 1)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
    endif()
    include("${_root}/cmake/psx_build_revision.cmake")
    foreach(_component game framework)
        set(_revision_${_component} "nogit")
        if(_component STREQUAL "game")
            set(_directory "${CMAKE_CURRENT_SOURCE_DIR}")
        else()
            set(_directory "${_root}")
        endif()
        psxrecomp_build_revision(_revision_${_component} "${_directory}")
    endforeach()
    set(_identity "${DIAG_GAME_ID}/${_version}+${_revision_game}/psxrecomp-${_revision_framework}")
    string(REGEX REPLACE "[^A-Za-z0-9._/+:-]" "_" _identity "${_identity}")
    # Restrict changing build identities to the recorder translation unit.
    # Generated game units can be enormous and do not use these definitions.
    set_property(SOURCE "${_root}/runtime/src/playtest_diagnostics.cpp" APPEND PROPERTY
        COMPILE_DEFINITIONS
        "PSX_DIAGNOSTIC_GAME_ID=\"${DIAG_GAME_ID}\""
        "PSX_DIAGNOSTIC_MOD_ID=\"${DIAG_MOD_ID}\""
        "PSX_DIAGNOSTIC_BUILD_ID=\"${_identity}\"")
    set(_targets ${target})
    if(TARGET ${target}-pgxp)
        list(APPEND _targets ${target}-pgxp)
    endif()
    find_package(Threads REQUIRED)
    foreach(_target IN LISTS _targets)
        if(NOT TARGET ${_target})
            message(FATAL_ERROR "playtest diagnostics target ${_target} does not exist")
        endif()
        get_target_property(_already ${_target} PSX_PLAYTEST_DIAGNOSTICS)
        if(_already)
            message(FATAL_ERROR "playtest diagnostics already configured for ${_target}")
        endif()
        set_property(TARGET ${_target} PROPERTY PSX_PLAYTEST_DIAGNOSTICS TRUE)
        target_sources(${_target} PRIVATE "${_root}/runtime/src/playtest_diagnostics.cpp")
        target_include_directories(${_target} PRIVATE "${_root}/runtime/include")
        target_compile_features(${_target} PRIVATE cxx_std_17)
        target_link_libraries(${_target} PRIVATE Threads::Threads)
        add_custom_command(TARGET ${_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_root}/tools/replay_diagnostics.py"
                "$<TARGET_FILE_DIR:${_target}>/replay-diagnostics.py" VERBATIM)
    endforeach()
endfunction()
