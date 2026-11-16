# Exact authored e841 recipes restored during Pin I composition.

if(BUILD_TESTING AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
foreach(_mode IN ITEMS O0 O2)
        set(_target "muldiv_giveback_fixture_${_mode}")
        add_executable(${_target}
            "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_muldiv_giveback_fixture.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/psx_cycles.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/psx_ram_geometry.c")
        target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include")
        target_compile_definitions(${_target} PRIVATE PSX_ENABLE_BLOCK_CYCLES=1)
        target_compile_options(${_target} PRIVATE -${_mode} -UNDEBUG -ffunction-sections -fdata-sections)
        target_link_options(${_target} PRIVATE -Wl,--gc-sections)
        add_test(NAME ${_target} COMMAND ${_target}
            "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/muldiv_f5_giveback_fixture.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/muldiv_f6_second_read_fixture.txt")
    endforeach()
endif()

if(BUILD_TESTING)
if(Python3_Interpreter_FOUND)
foreach(_t IN ITEMS
    cli_split_prepass_report
    codegen_hash_line_endings
    deferred_switch_interp_poll
    gl_swap_self_heal
    host_input_focus_guards
    netplay_boot_mode_wiring
    netplay_pad_seat_wiring
    netplay_test_input_wiring
    overlay_abi_gate_runtime
    overlay_abi_v26_guards
    overlay_call_unit_irq_guards
    overlay_step_boundary
    savestate_load_preflight
    settings_boot_keys_wiring
    setup_install_card_path_wiring
    setup_refusal_report
    setup_refusal_verdict
    windows_rebuild_helper)
            add_test(
                NAME ${_t}
                COMMAND ${Python3_EXECUTABLE} -B
                        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_${_t}.py
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        endforeach()
endif()
endif()
