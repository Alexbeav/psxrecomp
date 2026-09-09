# BIOS/disc-free controller, cycle and input regressions from the TAS work.
set(_tas_runtime "${CMAKE_CURRENT_LIST_DIR}/../../../runtime")
add_test(NAME tas_dualshock_route_export COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/../test_dualshock_route.py")
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(STATUS "TAS model fixtures currently require GCC")
    return()
endif()
foreach(_mode IN ITEMS O0 O2)
    foreach(_model IN ITEMS
            input_route_file input_dualshock_route_file input_dualshock_delivery input_route_field_clock input_route_raster_clock
            input_instruction_histogram input_update_clock compiled_ram_read_watch
            spu_status_visibility cdrom_boot_model cdrom_explicit_seek
            cdrom_implicit_seek cdrom_source_explicit_timing cdrom_random_tape)
        set(_target "tas_${_model}_${_mode}")
        add_executable(${_target} "${_tas_runtime}/tests/test_${_model}.c")
        target_include_directories(${_target} PRIVATE "${_tas_runtime}/include")
        target_compile_options(${_target} PRIVATE -${_mode} -UNDEBUG)
        if(_model MATCHES "^cdrom_")
            target_sources(${_target} PRIVATE "${_tas_runtime}/src/psx_sha256.c")
        endif()
        if(_model STREQUAL "compiled_ram_read_watch")
            target_compile_definitions(${_target} PRIVATE PSX_ENABLE_BLOCK_CYCLES=1)
        endif()
        target_link_libraries(${_target} PRIVATE m)
        add_test(NAME ${_target} COMMAND ${_target})
    endforeach()
endforeach()

foreach(_mode IN ITEMS O0 O2)
    set(_target "tas_input_route_observer_${_mode}")
    add_executable(${_target} "${_tas_runtime}/tests/test_input_route_observer.c"
        "${_tas_runtime}/src/input_route_observer.c" "${_tas_runtime}/src/psx_sha256.c")
    target_include_directories(${_target} PRIVATE "${_tas_runtime}/include")
    target_compile_options(${_target} PRIVATE -${_mode} -UNDEBUG)
    add_test(NAME ${_target} COMMAND ${Python3_EXECUTABLE}
        "${_tas_runtime}/tests/test_input_route_observer.py" $<TARGET_FILE:${_target}>)
endforeach()

foreach(_mode IN ITEMS O0 O2)
    set(_target "tas_sio_nymashock_card_${_mode}")
    add_executable(${_target} "${_tas_runtime}/tests/test_sio_nymashock_card.c")
    target_include_directories(${_target} PRIVATE "${_tas_runtime}/include")
    target_compile_options(${_target} PRIVATE -${_mode} -UNDEBUG -Werror=incompatible-pointer-types)
    add_test(NAME ${_target} COMMAND ${Python3_EXECUTABLE}
        "${_tas_runtime}/tests/test_sio_nymashock_card.py" $<TARGET_FILE:${_target}>)
    set(_target "tas_sio_nymashock_dualshock_${_mode}")
    add_executable(${_target} "${_tas_runtime}/tests/test_sio_nymashock_dualshock.c")
    target_include_directories(${_target} PRIVATE "${_tas_runtime}/include")
    target_compile_options(${_target} PRIVATE -${_mode} -UNDEBUG)
    add_test(NAME ${_target} COMMAND ${Python3_EXECUTABLE}
        "${_tas_runtime}/tests/test_sio_nymashock_dualshock.py" $<TARGET_FILE:${_target}>)
    set(_target "tas_sio_ack_timing_${_mode}")
    add_executable(${_target} "${_tas_runtime}/tests/test_sio_ack_timing.c")
    target_include_directories(${_target} PRIVATE "${_tas_runtime}/include")
    target_compile_options(${_target} PRIVATE -${_mode} -UNDEBUG)
    add_test(NAME ${_target} COMMAND ${_target})
endforeach()

foreach(_kind IN ITEMS pipeline dma)
    foreach(_mode IN ITEMS O0 O2)
        set(_target "tas_mdec_source_${_kind}_${_mode}")
        add_executable(${_target} "${_tas_runtime}/tests/test_mdec_source_${_kind}.c")
        target_include_directories(${_target} PRIVATE "${_tas_runtime}/src"
            "${_tas_runtime}/include" "${CMAKE_CURRENT_LIST_DIR}/../../../recompiler/include")
        target_compile_options(${_target} PRIVATE -${_mode} -UNDEBUG -ffunction-sections -fdata-sections)
        target_link_options(${_target} PRIVATE -Wl,--gc-sections)
        add_test(NAME ${_target} COMMAND ${Python3_EXECUTABLE}
            "${_tas_runtime}/tests/test_mdec_source_contracts.py" $<TARGET_FILE:${_target}> ${_kind})
    endforeach()
endforeach()

foreach(_mode IN ITEMS O0 O2)
    set(_target "tas_cdrom_source_cdda_${_mode}")
    add_executable(${_target} "${_tas_runtime}/tests/test_cdrom_source_cdda.c" "${_tas_runtime}/src/psx_sha256.c")
    target_include_directories(${_target} PRIVATE "${_tas_runtime}/include")
    target_compile_options(${_target} PRIVATE -${_mode} -UNDEBUG)
    add_test(NAME ${_target} COMMAND ${Python3_EXECUTABLE}
        "${_tas_runtime}/tests/test_cdrom_source_cdda_contracts.py" $<TARGET_FILE:${_target}>)
endforeach()
