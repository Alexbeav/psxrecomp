# Exact authored e841 recipes restored during Pin I composition.

if(BUILD_TESTING)
add_executable(autocompile_handle_inherit_test
        tests/test_autocompile_handle_inherit.c)
endif()

if(BUILD_TESTING)
if(WIN32)
target_sources(autocompile_handle_inherit_test PRIVATE src/autocompile.c)
endif()
endif()

if(BUILD_TESTING)
if(WIN32)
target_include_directories(autocompile_handle_inherit_test PRIVATE include)
endif()
endif()

if(BUILD_TESTING)
add_test(NAME autocompile_handle_inherit_test
             COMMAND autocompile_handle_inherit_test)
endif()

if(BUILD_TESTING)
set_tests_properties(autocompile_handle_inherit_test PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()

if(BUILD_TESTING)
add_executable(controller_port_route_test
        tests/test_controller_port_route.c
        src/controller_port_route.c)
endif()

if(BUILD_TESTING)
target_include_directories(controller_port_route_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()

if(BUILD_TESTING)
add_test(NAME controller_port_route_test COMMAND controller_port_route_test)
endif()

if(BUILD_TESTING)
add_executable(disc_boot_image_test
        tests/test_disc_boot_image.cpp
        src/disc_boot_image.cpp
        src/iso_reader.cpp
        src/cue_sheet.cpp)
endif()

if(BUILD_TESTING)
target_include_directories(disc_boot_image_test PRIVATE include ${PSXRECOMP_ROOT}/recompiler/include)
endif()

if(BUILD_TESTING)
target_link_libraries(disc_boot_image_test PRIVATE chdr-static)
endif()

if(BUILD_TESTING)
add_test(NAME disc_boot_image_test COMMAND disc_boot_image_test
        ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(BUILD_TESTING)
add_executable(disc_digest_cache_test tests/test_disc_digest_cache.c
        src/disc_digest_cache.c src/psx_sha256.c)
endif()

if(BUILD_TESTING)
target_include_directories(disc_digest_cache_test PRIVATE include)
endif()

if(BUILD_TESTING)
add_test(NAME disc_digest_cache_test COMMAND disc_digest_cache_test ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(BUILD_TESTING)
add_executable(freeze_heartbeat_wedge_test
        tests/test_freeze_heartbeat_wedge.c
        src/freeze_dump_policy.c)
endif()

if(BUILD_TESTING)
target_include_directories(freeze_heartbeat_wedge_test PRIVATE include src)
endif()

if(BUILD_TESTING)
if(WIN32)
target_link_libraries(freeze_heartbeat_wedge_test PRIVATE dbghelp)
endif()
endif()

if(BUILD_TESTING)
if(WIN32)
else()
target_link_libraries(freeze_heartbeat_wedge_test PRIVATE
            Threads::Threads ${CMAKE_DL_LIBS})
endif()
endif()

if(BUILD_TESTING)
foreach(_scenario IN ITEMS
            budget-after-startup dump-at-frame dump-at-frame-bad-value
            spin-ends-only spin-wait-ends spin-real spin-video
            spin-pause-restarts)
file(MAKE_DIRECTORY
            ${CMAKE_CURRENT_BINARY_DIR}/freeze_heartbeat_wedge_scratch/${_scenario})
endforeach()
endif()

if(BUILD_TESTING)
foreach(_scenario IN ITEMS
            budget-after-startup dump-at-frame dump-at-frame-bad-value
            spin-ends-only spin-wait-ends spin-real spin-video
            spin-pause-restarts)
add_test(NAME freeze_heartbeat_wedge_${_scenario}
                 COMMAND freeze_heartbeat_wedge_test ${_scenario}
                 WORKING_DIRECTORY
                     ${CMAKE_CURRENT_BINARY_DIR}/freeze_heartbeat_wedge_scratch/${_scenario})
endforeach()
endif()

if(BUILD_TESTING)
add_executable(gpu_interlace_render_test
        tests/test_gpu_interlace_render.c src/gpu_render.c
        src/gpu_sw_renderer.c src/gpu_vram_dirty.c)
endif()

if(BUILD_TESTING)
target_include_directories(gpu_interlace_render_test PRIVATE include)
endif()

if(BUILD_TESTING)
if(NOT MSVC)
target_link_libraries(gpu_interlace_render_test PRIVATE m)
endif()
endif()

if(BUILD_TESTING)
add_test(NAME gpu_interlace_render_test COMMAND gpu_interlace_render_test)
endif()

if(BUILD_TESTING)
add_executable(guncon_keybinds_test
        tests/test_guncon_keybinds.c
        src/psx_keybinds.c)
endif()

if(BUILD_TESTING)
target_include_directories(guncon_keybinds_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
endif()

if(BUILD_TESTING)
target_compile_definitions(guncon_keybinds_test PRIVATE
        SDL_MAIN_HANDLED
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
endif()

if(BUILD_TESTING)
target_link_libraries(guncon_keybinds_test PRIVATE ${PSX_SDL_LIBRARIES})
endif()

if(BUILD_TESTING)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/guncon_keybinds)
endif()

if(BUILD_TESTING)
add_test(NAME guncon_keybinds_test
        COMMAND guncon_keybinds_test ${CMAKE_CURRENT_BINARY_DIR}/guncon_keybinds)
endif()

if(BUILD_TESTING)
add_executable(host_osd_rec_test tests/test_host_osd_rec.c)
endif()

if(BUILD_TESTING)
target_include_directories(host_osd_rec_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
endif()

if(BUILD_TESTING)
target_compile_definitions(host_osd_rec_test PRIVATE
        SDL_MAIN_HANDLED
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
endif()

if(BUILD_TESTING)
target_link_libraries(host_osd_rec_test PRIVATE ${PSX_SDL_LIBRARIES})
endif()

if(BUILD_TESTING)
add_test(NAME host_osd_rec_test COMMAND host_osd_rec_test)
endif()

if(BUILD_TESTING)
add_executable(irq_cause_ip2_doc_test tests/test_irq_cause_ip2_doc.c)
endif()

if(BUILD_TESTING)
target_include_directories(irq_cause_ip2_doc_test PRIVATE include)
endif()

if(BUILD_TESTING)
add_test(NAME irq_cause_ip2_doc_test COMMAND irq_cause_ip2_doc_test)
endif()

if(BUILD_TESTING)
add_executable(offline_seats_test tests/test_offline_seats.c)
endif()

if(BUILD_TESTING)
target_include_directories(offline_seats_test PRIVATE include)
endif()

if(BUILD_TESTING)
add_test(NAME offline_seats_test COMMAND offline_seats_test)
endif()

if(BUILD_TESTING)
add_executable(overlay_abi_layout_test tests/test_overlay_abi_layout.c)
endif()

if(BUILD_TESTING)
target_include_directories(overlay_abi_layout_test PRIVATE include)
endif()

if(BUILD_TESTING)
add_test(NAME overlay_abi_layout_test COMMAND overlay_abi_layout_test)
endif()

if(BUILD_TESTING)
add_executable(overlay_capture_history_test
        tests/test_overlay_capture_history.cpp
        src/overlay_capture.c)
endif()

if(BUILD_TESTING)
target_include_directories(overlay_capture_history_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
endif()

if(BUILD_TESTING)
target_compile_definitions(overlay_capture_history_test PRIVATE
        SDL_MAIN_HANDLED PSX_OVERLAY_CAPTURE_TEST=1
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
endif()

if(BUILD_TESTING)
target_link_libraries(overlay_capture_history_test PRIVATE ${PSX_SDL_LIBRARIES})
endif()

if(BUILD_TESTING)
add_test(NAME overlay_capture_history_test COMMAND overlay_capture_history_test)
endif()

if(BUILD_TESTING)
add_executable(overlay_capture_queue_bound_test
        tests/test_overlay_capture_queue_bound.cpp
        src/overlay_capture.c)
endif()

if(BUILD_TESTING)
target_include_directories(overlay_capture_queue_bound_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
endif()

if(BUILD_TESTING)
target_compile_definitions(overlay_capture_queue_bound_test PRIVATE
        SDL_MAIN_HANDLED PSX_OVERLAY_CAPTURE_TEST=1
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
endif()

if(BUILD_TESTING)
target_link_libraries(overlay_capture_queue_bound_test PRIVATE ${PSX_SDL_LIBRARIES})
endif()

if(BUILD_TESTING)
add_test(NAME overlay_capture_queue_bound_test COMMAND overlay_capture_queue_bound_test)
endif()

if(BUILD_TESTING)
add_executable(program_set_lock_test
        tests/test_program_set_lock.c
        src/program_set_lock.c)
endif()

if(BUILD_TESTING)
target_include_directories(program_set_lock_test PRIVATE include)
endif()

if(BUILD_TESTING)
add_test(NAME program_set_lock_test COMMAND program_set_lock_test)
endif()

if(BUILD_TESTING)
add_executable(savestate_disc_scope_test
        tests/test_savestate_disc_scope.c
        src/savestate.c)
endif()

if(BUILD_TESTING)
target_include_directories(savestate_disc_scope_test PRIVATE include)
endif()

if(BUILD_TESTING)
add_test(NAME savestate_disc_scope_test
             COMMAND savestate_disc_scope_test ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(BUILD_TESTING)
add_executable(savestate_menu_replays_test tests/test_savestate_menu_replays.c)
endif()

if(BUILD_TESTING)
target_include_directories(savestate_menu_replays_test PRIVATE include)
endif()

if(BUILD_TESTING)
add_test(NAME savestate_menu_replays_test COMMAND savestate_menu_replays_test)
endif()

if(BUILD_TESTING)
add_executable(spu_status_visibility_test tests/test_spu_status_visibility.c)
endif()

if(BUILD_TESTING)
target_include_directories(spu_status_visibility_test PRIVATE include)
endif()

if(BUILD_TESTING)
add_test(NAME spu_status_visibility_test COMMAND spu_status_visibility_test)
endif()

if(BUILD_TESTING)
add_executable(text_source_guard_test
        tests/test_text_source_guard.c
        src/text_source_guard.c
        src/crc32.c)
endif()

if(BUILD_TESTING)
target_include_directories(text_source_guard_test PRIVATE include)
endif()

if(BUILD_TESTING)
add_test(NAME text_source_guard_test COMMAND text_source_guard_test)
endif()

if(BUILD_TESTING)
add_executable(text_xlate_kernel_bless_test
        tests/test_text_xlate_kernel_bless.cpp
        src/text_xlate.cpp)
endif()

if(BUILD_TESTING)
target_include_directories(text_xlate_kernel_bless_test PRIVATE
        include
        ../recompiler/lib/toml11)
endif()

if(BUILD_TESTING)
add_test(NAME text_xlate_kernel_bless_test COMMAND text_xlate_kernel_bless_test)
endif()

if(BUILD_TESTING)
add_executable(turbo_loads_gate_test tests/test_turbo_loads_gate.c src/turbo_loads_gate.c)
endif()

if(BUILD_TESTING)
target_include_directories(turbo_loads_gate_test PRIVATE include)
endif()

if(BUILD_TESTING)
add_test(NAME turbo_loads_gate_test COMMAND turbo_loads_gate_test)
endif()
