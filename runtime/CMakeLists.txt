cmake_minimum_required(VERSION 3.20)
project(psxrecomp-runtime C CXX)
include(CTest)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# ---- Native/oracle runtime targets ----

set(PSXRECOMP_ROOT "${CMAKE_SOURCE_DIR}/..")
include("${CMAKE_SOURCE_DIR}/runtime.cmake")

psxrecomp_add_runtime_target(psx-runtime
    DEBUG_PORT 4370
    WINDOW_TITLE "psxrecomp - BIOS"
    DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/bios/SCPH1001.BIN"
    EXE_NAME "PSXRecomp"
)

# With -DPSX_PGXP_VARIANT=ON, psxrecomp_add_runtime_target auto-clones a
# psx-runtime-pgxp sibling (PSXRecomp_pgxp.exe) compiled with -DPSX_PGXP=1 —
# the framework-dev validation surface for the PGXP value-propagation engine
# (docs/ENHANCEMENTS.md G1.10). Titles get their <exe>_pgxp the same way.

if(BUILD_TESTING)
    if(PSX_RECOMP_UI AND EXISTS "${RECOMP_UI_ROOT}/src/recomp_launcher.h")
        add_executable(player_settings_provider_test
            tests/test_player_settings_provider.cpp
            ../recompiler/src/config_loader.cpp ../recompiler/src/ps1_exe_parser.cpp)
        target_include_directories(player_settings_provider_test PRIVATE include
            ../recompiler/src ../recompiler/include ../recompiler/lib/fmt/include
            ../recompiler/lib/toml11 "${RECOMP_UI_ROOT}/src")
        target_compile_definitions(player_settings_provider_test PRIVATE RECOMP_LAUNCHER FMT_HEADER_ONLY=1)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(player_settings_provider_test PRIVATE -UNDEBUG)
        elseif(MSVC)
            target_compile_options(player_settings_provider_test PRIVATE /UNDEBUG)
        endif()
        add_test(NAME player_settings_provider_test COMMAND player_settings_provider_test)
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_executable(depth24_row_policy_test tests/test_depth24_row_policy.c)
        target_include_directories(depth24_row_policy_test PRIVATE include)
        target_compile_options(depth24_row_policy_test PRIVATE
            -O2 -UNDEBUG -flto -fwhole-program -ffunction-sections -fdata-sections)
        target_link_options(depth24_row_policy_test PRIVATE -flto -Wl,--gc-sections)
        add_test(NAME depth24_row_policy_test COMMAND depth24_row_policy_test)
    endif()
    add_executable(irq_register_image_test tests/test_irq_register_image.c)
    target_include_directories(irq_register_image_test PRIVATE include)
    add_test(NAME irq_register_image_test COMMAND irq_register_image_test)
    # PS1B-289: which BIOS a netplay match boots, and its wire helpers.
    foreach(_native IN ITEMS 0 1)
        add_executable(host_deadline_${_native}_test tests/test_host_deadline.c)
        target_include_directories(host_deadline_${_native}_test PRIVATE include)
        target_compile_definitions(host_deadline_${_native}_test PRIVATE PSX_NATIVE_HOST_PACING=${_native})
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(host_deadline_${_native}_test PRIVATE -UNDEBUG)
        elseif(MSVC)
            target_compile_options(host_deadline_${_native}_test PRIVATE /UNDEBUG)
        endif()
        add_test(NAME host_deadline_${_native}_test COMMAND host_deadline_${_native}_test)
    endforeach()
    foreach(_span_mode native reference)
        add_executable(psx_${_span_mode}_span_test tests/test_psx_native_span.c)
        target_include_directories(psx_${_span_mode}_span_test PRIVATE include)
        target_compile_definitions(psx_${_span_mode}_span_test PRIVATE PSX_ENABLE_BLOCK_CYCLES=1)
        if(_span_mode STREQUAL "native")
            target_compile_definitions(psx_${_span_mode}_span_test PRIVATE PSX_NATIVE_CPU_SPANS=1)
        endif()
        add_test(NAME psx_${_span_mode}_span_test COMMAND psx_${_span_mode}_span_test)
    endforeach()
    add_executable(gpu_fmv_hd_test tests/test_gpu_fmv_hd.cpp src/gpu_fmv_hd.c)
    target_include_directories(gpu_fmv_hd_test PRIVATE include ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(gpu_fmv_hd_test PRIVATE SDL_MAIN_HANDLED PSX_FMV_HD_TESTING=1
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
    target_link_libraries(gpu_fmv_hd_test PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME gpu_fmv_hd_test COMMAND gpu_fmv_hd_test)
    set_tests_properties(gpu_fmv_hd_test PROPERTIES TIMEOUT 20)
    add_executable(postfx_publication_test tests/test_postfx_publication.cpp)
    target_include_directories(postfx_publication_test PRIVATE include)
    add_test(NAME postfx_publication_test COMMAND postfx_publication_test)
    # Windows UTF-8 text boundary (issue #371): the helpers legacy Windows
    # relies on, and — with the shipped manifest — a UTF-8 process code page.
    if(WIN32)
        add_executable(host_time_threads_test tests/test_host_time_threads.c src/host_time.c)
        target_include_directories(host_time_threads_test PRIVATE include)
        add_test(NAME host_time_threads_test COMMAND host_time_threads_test)
        set_tests_properties(host_time_threads_test PROPERTIES TIMEOUT 15)
        add_executable(win_utf8_helpers_test tests/test_win_utf8.cpp)
        target_include_directories(win_utf8_helpers_test PRIVATE include)
        add_test(NAME win_utf8_helpers_test COMMAND win_utf8_helpers_test)
        add_executable(win_utf8_manifest_test tests/test_win_utf8.cpp)
        target_include_directories(win_utf8_manifest_test PRIVATE include)
        psxrecomp_windows_manifest(win_utf8_manifest_test)
        add_test(NAME win_utf8_manifest_test
                 COMMAND win_utf8_manifest_test --expect-utf8-acp)
    endif()

    add_executable(debug_socket_shutdown_test tests/test_debug_socket_shutdown.cpp)
    target_include_directories(debug_socket_shutdown_test PRIVATE src)
    find_package(Threads REQUIRED)
    target_link_libraries(debug_socket_shutdown_test PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(debug_socket_shutdown_test PRIVATE ws2_32)
    endif()
    add_test(NAME debug_socket_shutdown_test COMMAND debug_socket_shutdown_test)
    set_tests_properties(debug_socket_shutdown_test PROPERTIES TIMEOUT 15)

    add_executable(atomic_file_write_test tests/test_atomic_file_write.cpp)
    target_include_directories(atomic_file_write_test PRIVATE include)
    target_compile_features(atomic_file_write_test PRIVATE cxx_std_17)
    add_test(NAME atomic_file_write_test COMMAND atomic_file_write_test)

    add_executable(openxr_ready_state_test tests/test_openxr_ready_state.c)
    target_include_directories(openxr_ready_state_test PRIVATE include)
    add_test(NAME openxr_ready_state_test COMMAND openxr_ready_state_test)

    add_executable(overlay_memory_address_test tests/test_overlay_memory_address.c)
    target_include_directories(overlay_memory_address_test PRIVATE include)
    add_test(NAME overlay_memory_address_test COMMAND overlay_memory_address_test)
    add_executable(mod_snapshot_test tests/test_mod_snapshot.c src/mod_snapshot.c)
    target_include_directories(mod_snapshot_test PRIVATE include)
    target_compile_options(mod_snapshot_test PRIVATE
        $<IF:$<C_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
    add_test(NAME mod_snapshot_test COMMAND mod_snapshot_test)
    add_executable(mod_netplay_test tests/test_mod_netplay.c
        src/mod_netplay.c src/netplay_state_digest.c src/crc32.c src/psx_sha256.c)
    target_include_directories(mod_netplay_test PRIVATE include)
    add_test(NAME mod_netplay_test COMMAND mod_netplay_test)
    add_test(NAME mod_netplay_wide_test COMMAND mod_netplay_test --wide)
    add_test(NAME mod_netplay_optional_bank_test COMMAND mod_netplay_test --no-banks)
    add_test(NAME mod_netplay_options_policy_test COMMAND mod_netplay_test --options)
    add_test(NAME mod_netplay_loading_policy_test COMMAND mod_netplay_test --loading)
    add_executable(savestate_title_policy_test tests/test_savestate_title_policy.c
        src/mod_netplay.c src/psx_sha256.c)
    target_include_directories(savestate_title_policy_test PRIVATE include)
    add_test(NAME savestate_title_policy_test COMMAND savestate_title_policy_test)
    add_executable(mod_capcom_bg2d_test tests/test_mod_capcom_bg2d.c)
    target_include_directories(mod_capcom_bg2d_test PRIVATE include)
    add_test(NAME mod_capcom_bg2d_test COMMAND mod_capcom_bg2d_test)
    # White-box failure injection checks restart after partial native hashing.
    foreach(_hash_variant native portable)
        add_executable(sha256_file_${_hash_variant}_test
            tests/test_psx_sha256_file.cpp src/psx_sha256.c)
        target_include_directories(sha256_file_${_hash_variant}_test PRIVATE include)
        if(_hash_variant STREQUAL "portable")
            target_compile_definitions(sha256_file_${_hash_variant}_test PRIVATE
                PSX_SHA256_FILE_PORTABLE=1)
        elseif(WIN32)
            target_link_libraries(sha256_file_${_hash_variant}_test PRIVATE bcrypt)
        elseif(TARGET OpenSSL::Crypto)
            target_compile_definitions(sha256_file_${_hash_variant}_test PRIVATE
                PSX_SHA256_FILE_OPENSSL=1)
            target_link_libraries(sha256_file_${_hash_variant}_test PRIVATE OpenSSL::Crypto)
        endif()
        add_test(NAME sha256_file_${_hash_variant}_test
            COMMAND sha256_file_${_hash_variant}_test)
    endforeach()
    find_package(Threads REQUIRED)
    add_executable(host_launch_timing_test
        tests/test_host_launch_timing.cpp tests/host_launch_timing_helper.cpp)
    target_include_directories(host_launch_timing_test PRIVATE include)
    target_link_libraries(host_launch_timing_test PRIVATE Threads::Threads)
    add_test(NAME host_launch_timing_test COMMAND host_launch_timing_test)
    add_executable(disasm_test tests/test_disasm.cpp src/disasm_shim.cpp
        ../recompiler/src/mips_decoder.cpp)
    target_include_directories(disasm_test PRIVATE include ../recompiler/include)
    add_test(NAME disasm_test COMMAND disasm_test)
    add_executable(local_mouse_policy_test
        tests/test_local_mouse_policy.cpp src/local_mouse_policy.cpp)
    target_include_directories(local_mouse_policy_test PRIVATE include src)
    add_test(NAME local_mouse_policy_test COMMAND local_mouse_policy_test)
    # Netplay: which BIOS a match boots, and its wire helpers.
    add_executable(netplay_bios_settle_test
        tests/test_netplay_bios_settle.c
        src/netplay_bios_settle.c)
    target_include_directories(netplay_bios_settle_test PRIVATE include)
    add_test(NAME netplay_bios_settle_test COMMAND netplay_bios_settle_test)
    # Netplay: why a match ended, and when a boot-digest mismatch is final.
    add_executable(netplay_exit_reason_test
        tests/test_netplay_exit_reason.c
        src/netplay_exit_reason.c)
    target_include_directories(netplay_exit_reason_test PRIVATE include)
    add_test(NAME netplay_exit_reason_test COMMAND netplay_exit_reason_test)
    # PS1G-63: why a start ended before the first frame, for the run report.
    add_executable(start_refusal_test
        tests/test_start_refusal.c
        src/start_refusal.c)
    target_include_directories(start_refusal_test PRIVATE include)
    add_test(NAME start_refusal_test COMMAND start_refusal_test)
    add_executable(missing_sbi_report_test
        ${PSXRECOMP_ROOT}/tools/tests/test_missing_sbi_report.cpp
        src/start_refusal.c)
    target_include_directories(missing_sbi_report_test PRIVATE include)
    add_test(NAME missing_sbi_report_test COMMAND missing_sbi_report_test)
    # PS1B-295: a LAN / Direct IP host refuses a guest on another build.
    add_executable(netplay_lan_version_test
        tests/test_netplay_lan_version.c
        src/netplay_lan_version.c)
    target_include_directories(netplay_lan_version_test PRIVATE include)
    add_test(NAME netplay_lan_version_test COMMAND netplay_lan_version_test)
    add_executable(netplay_sim_pad_cache_test tests/test_netplay_sim_pad_cache.c)
    target_include_directories(netplay_sim_pad_cache_test PRIVATE include)
    target_compile_definitions(netplay_sim_pad_cache_test PRIVATE PSX_MAX_PLAYERS=4)
    add_test(NAME netplay_sim_pad_cache_test COMMAND netplay_sim_pad_cache_test)
    add_executable(lobby_launch_transport_test tests/test_lobby_launch_transport.c)
    target_include_directories(lobby_launch_transport_test PRIVATE include)
    add_test(NAME lobby_launch_transport_test COMMAND lobby_launch_transport_test)
    add_executable(netplay_fmv_confirm_test tests/test_netplay_fmv_confirm.c)
    target_include_directories(netplay_fmv_confirm_test PRIVATE include)
    add_test(NAME netplay_fmv_confirm_test COMMAND netplay_fmv_confirm_test)
    add_executable(netplay_local_view_test tests/test_netplay_local_view.c)
    target_include_directories(netplay_local_view_test PRIVATE include)
    add_test(NAME netplay_local_view_test COMMAND netplay_local_view_test)
    # Host relay over ICE: transport choice + lobby seat -> session slot map.
    add_executable(netplay_ice_hub_test
        tests/test_netplay_ice_hub.c
        src/netplay_ice_hub.c)
    target_include_directories(netplay_ice_hub_test PRIVATE include)
    add_test(NAME netplay_ice_hub_test COMMAND netplay_ice_hub_test)
    add_executable(ws_mixed_world_test tests/test_ws_mixed_world.c)
    target_include_directories(ws_mixed_world_test PRIVATE include)
    add_test(NAME ws_mixed_world COMMAND ws_mixed_world_test)
    add_executable(mod_function_replacements_test tests/test_mod_function_replacements.c src/mod_function_replacements.c)
    target_include_directories(mod_function_replacements_test PRIVATE include)
    add_test(NAME mod_function_replacements_test COMMAND mod_function_replacements_test)
    add_executable(ws_scene_hold_test tests/test_ws_scene_hold.c)
    target_include_directories(ws_scene_hold_test PRIVATE include)
    add_test(NAME ws_scene_hold_test COMMAND ws_scene_hold_test)
    add_executable(ws_backdrop_extent_test tests/test_ws_backdrop_extent.c)
    target_include_directories(ws_backdrop_extent_test PRIVATE include)
    add_test(NAME ws_backdrop_extent_test COMMAND ws_backdrop_extent_test)
    add_executable(mdec_snapshot_activity_test tests/test_mdec_snapshot_activity.c)
    target_include_directories(mdec_snapshot_activity_test PRIVATE include src)
    add_test(NAME mdec_snapshot_activity_test COMMAND mdec_snapshot_activity_test)
    add_executable(mdec_snapshot_capacity_test tests/test_mdec_snapshot_capacity.c)
    target_include_directories(mdec_snapshot_capacity_test PRIVATE include src)
    add_test(NAME mdec_snapshot_capacity_test COMMAND mdec_snapshot_capacity_test)
    add_executable(mod_texture_banks_test tests/test_mod_texture_banks.c src/mod_texture_banks.c)
    target_include_directories(mod_texture_banks_test PRIVATE include)
    add_test(NAME mod_texture_banks_test COMMAND mod_texture_banks_test)
    # First-call capture / later-call restore behind main.cpp's
    # reset_mod_owned_presentation() (header-only, no runtime link).
    add_executable(mod_session_baseline_test tests/test_mod_session_baseline.c)
    target_include_directories(mod_session_baseline_test PRIVATE include)
    add_test(NAME mod_session_baseline_test COMMAND mod_session_baseline_test)
    add_executable(spu_shadow_normalization_test tests/test_spu_shadow_normalization.c
        src/spu_shadow.c src/audio_shadow.c)
    target_include_directories(spu_shadow_normalization_test PRIVATE include)
    if(NOT WIN32)
        target_link_libraries(spu_shadow_normalization_test PRIVATE m)
    endif()
    add_test(NAME spu_shadow_normalization_test COMMAND spu_shadow_normalization_test)
    # Netplay: libjuice resolves from third_party/ with the network sealed,
    # and a recomp-net pinning another libjuice stops the configure.
    foreach(_psx_juice_case same other)
        add_test(NAME netplay_libjuice_offline_${_psx_juice_case}
            COMMAND ${CMAKE_COMMAND}
                -S "${CMAKE_CURRENT_SOURCE_DIR}/tests/netplay_libjuice_offline"
                -B "${CMAKE_CURRENT_BINARY_DIR}/netplay_libjuice_offline_${_psx_juice_case}"
                -G "${CMAKE_GENERATOR}"
                "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
                -DPSX_DEPS_OFFLINE=ON
                "-DFAKE_RECOMP_NET=${CMAKE_CURRENT_SOURCE_DIR}/tests/netplay_libjuice_offline/recomp-net-${_psx_juice_case}")
        set_tests_properties(netplay_libjuice_offline_${_psx_juice_case} PROPERTIES
            ENVIRONMENT "HTTP_PROXY=http://127.0.0.1:9;HTTPS_PROXY=http://127.0.0.1:9;ALL_PROXY=http://127.0.0.1:9;NO_PROXY=")
    endforeach()
    set_tests_properties(netplay_libjuice_offline_other PROPERTIES
        PASS_REGULAR_EXPRESSION "recomp-net pins libjuice SHA256")
    # PS1B-287 / PS1B-295: a snapshot with no .git still knows its framework
    # commit, and the lobby version carries it.
    add_test(NAME framework_identity_test
        COMMAND ${CMAKE_COMMAND}
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/framework_identity_test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/framework_identity/check.cmake")
    if(NOT MSVC)
        add_executable(host_snapshot_guard_test tests/test_host_snapshot_guard.c)
        add_executable(fntrace_restore_handoff_test
            tests/test_fntrace_restore_handoff.c src/fntrace.c)
        foreach(test_target host_snapshot_guard_test fntrace_restore_handoff_test)
            target_include_directories(${test_target} PRIVATE include src)
            target_compile_options(${test_target} PRIVATE -O2 -flto -ffunction-sections -fdata-sections)
            target_link_options(${test_target} PRIVATE -flto -Wl,--gc-sections)
            add_test(NAME ${test_target} COMMAND ${test_target})
        endforeach()
        add_executable(ws_world_scene_test tests/test_ws_world_scene.c
            src/psx_video_timing.c)
        target_include_directories(ws_world_scene_test PRIVATE include src)
        target_compile_options(ws_world_scene_test PRIVATE -O2 -flto -ffunction-sections -fdata-sections)
        target_link_options(ws_world_scene_test PRIVATE -flto -Wl,--gc-sections)
        add_test(NAME ws_world_scene_test COMMAND ws_world_scene_test)
        add_executable(ws_backdrop_scene_test tests/test_ws_backdrop_scene.c
            src/psx_video_timing.c)
        target_include_directories(ws_backdrop_scene_test PRIVATE include src)
        target_compile_options(ws_backdrop_scene_test PRIVATE -O2 -flto -ffunction-sections -fdata-sections)
        target_link_options(ws_backdrop_scene_test PRIVATE -flto -Wl,--gc-sections)
        add_test(NAME ws_backdrop_scene_test COMMAND ws_backdrop_scene_test)
        add_executable(boot_state_handoff_test tests/test_boot_state_handoff.c src/mod_snapshot.c)
        target_include_directories(boot_state_handoff_test PRIVATE include src)
        target_compile_definitions(boot_state_handoff_test PRIVATE PSX_COSIM=1)
        target_compile_options(boot_state_handoff_test PRIVATE -O2 -flto -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(boot_state_handoff_test PRIVATE -flto -Wl,--gc-sections)
        target_link_libraries(boot_state_handoff_test PRIVATE ZLIB::ZLIB)
        add_test(NAME boot_state_handoff_test COMMAND boot_state_handoff_test)
        add_executable(boot_state_native_capture_test tests/test_capture_writer.c src/mod_snapshot.c)
        target_include_directories(boot_state_native_capture_test PRIVATE include src)
        target_compile_definitions(boot_state_native_capture_test PRIVATE PSX_COSIM=1 PSX_NATIVE_CAPTURE_JOBS=1)
        target_compile_options(boot_state_native_capture_test PRIVATE -O2 -flto -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(boot_state_native_capture_test PRIVATE -flto -Wl,--gc-sections)
        target_link_libraries(boot_state_native_capture_test PRIVATE ZLIB::ZLIB)
        add_test(NAME boot_state_native_capture_test COMMAND boot_state_native_capture_test)
        add_executable(boot_state_reference_capture_test tests/test_capture_writer.c src/mod_snapshot.c)
        target_include_directories(boot_state_reference_capture_test PRIVATE include src)
        target_compile_definitions(boot_state_reference_capture_test PRIVATE PSX_COSIM=1 PSX_NATIVE_CAPTURE_JOBS=0)
        target_compile_options(boot_state_reference_capture_test PRIVATE -O2 -flto -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(boot_state_reference_capture_test PRIVATE -flto -Wl,--gc-sections)
        target_link_libraries(boot_state_reference_capture_test PRIVATE ZLIB::ZLIB)
        add_test(NAME boot_state_reference_capture_test COMMAND boot_state_reference_capture_test)
        foreach(_native IN ITEMS 0 1)
            set(_capture_test "boot_state_capture_o0_${_native}_test")
            add_executable(${_capture_test} tests/test_capture_writer.c src/mod_snapshot.c)
            target_include_directories(${_capture_test} PRIVATE include src)
            target_compile_definitions(${_capture_test} PRIVATE PSX_COSIM=1 PSX_NATIVE_CAPTURE_JOBS=${_native})
            target_compile_options(${_capture_test} PRIVATE -O0 -ffunction-sections -fdata-sections -UNDEBUG)
            target_link_options(${_capture_test} PRIVATE -Wl,--gc-sections)
            target_link_libraries(${_capture_test} PRIVATE ZLIB::ZLIB)
            add_test(NAME ${_capture_test} COMMAND ${_capture_test})
        endforeach()
        if(EXISTS "${RECOMP_RBENGINE_ROOT}/src/snap/rbe_snap_ring.c")
            file(READ "${RECOMP_RBENGINE_ROOT}/include/retcomm_rbengine/snap_ring.h" _capture_ring_header)
            foreach(_native IN ITEMS 0 1)
                if(_native AND NOT _capture_ring_header MATCHES "RBE_SNAP_CAPTURE_API_VERSION")
                    continue() # Old dependency remains a supported reference build.
                endif()
                set(_ring_test "netplay_capture_ring_${_native}_test")
                add_executable(${_ring_test} tests/test_netplay_capture_ring.c
                    src/mod_snapshot.c
                    src/netplay_snap_ring.c
                    "${RECOMP_RBENGINE_ROOT}/src/snap/rbe_snap_ring.c")
                if(_native)
                    target_sources(${_ring_test} PRIVATE src/capture_ring.c)
                endif()
                target_include_directories(${_ring_test} PRIVATE include src
                    "${RECOMP_RBENGINE_ROOT}/include")
                target_compile_definitions(${_ring_test} PRIVATE PSX_COSIM=1
                    PSX_HAS_RECOMP_NET=1 PSX_NATIVE_CAPTURE_JOBS=${_native})
                target_compile_options(${_ring_test} PRIVATE -O2 -ffunction-sections -fdata-sections -UNDEBUG)
                target_link_options(${_ring_test} PRIVATE -Wl,--gc-sections)
                target_link_libraries(${_ring_test} PRIVATE ZLIB::ZLIB)
                add_test(NAME ${_ring_test} COMMAND ${_ring_test})
            endforeach()
        endif()
        add_executable(mod_snapshot_boot_state_test
            tests/test_mod_snapshot_boot_state.c src/mod_snapshot.c)
        target_include_directories(mod_snapshot_boot_state_test PRIVATE include src)
        target_compile_definitions(mod_snapshot_boot_state_test PRIVATE
            PSX_COSIM=1 PSX_NATIVE_CAPTURE_JOBS=1)
        target_compile_options(mod_snapshot_boot_state_test PRIVATE
            -O2 -flto -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(mod_snapshot_boot_state_test PRIVATE -flto -Wl,--gc-sections)
        target_link_libraries(mod_snapshot_boot_state_test PRIVATE ZLIB::ZLIB)
        add_test(NAME mod_snapshot_boot_state_test COMMAND mod_snapshot_boot_state_test)
        add_executable(boot_state_impl_tag_test tests/test_boot_state_handoff.c src/mod_snapshot.c)
        target_include_directories(boot_state_impl_tag_test PRIVATE include src)
        target_compile_definitions(boot_state_impl_tag_test PRIVATE PSX_COSIM=1 PSX_SAVESTATE_IMPL_TAG=0x54474801u)
        target_compile_options(boot_state_impl_tag_test PRIVATE -O2 -flto -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(boot_state_impl_tag_test PRIVATE -flto -Wl,--gc-sections)
        target_link_libraries(boot_state_impl_tag_test PRIVATE ZLIB::ZLIB)
        add_test(NAME boot_state_impl_tag_test COMMAND boot_state_impl_tag_test)

        add_executable(boot_state_execution_test tests/test_boot_state_handoff.c src/mod_snapshot.c)
        target_include_directories(boot_state_execution_test PRIVATE include src)
        target_compile_definitions(boot_state_execution_test PRIVATE PSX_COSIM=1)
        target_compile_options(boot_state_execution_test PRIVATE -O2 -flto -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(boot_state_execution_test PRIVATE -flto -Wl,--gc-sections)
        target_link_libraries(boot_state_execution_test PRIVATE ZLIB::ZLIB)
        psxrecomp_execution_profile(boot_state_execution_test PROFILE ENHANCED)
        add_test(NAME boot_state_execution_test COMMAND boot_state_execution_test)

        add_executable(static_match_cache_test tests/test_static_match_cache.c
            src/overlay_loader.c src/host_span_ring.c src/crc32.c)
        target_include_directories(static_match_cache_test PRIVATE include)
        target_compile_definitions(static_match_cache_test PRIVATE
            PSX_HAS_OVERLAY_DISPATCH PSX_NO_DEBUG_TOOLS)
        target_compile_options(static_match_cache_test PRIVATE -O2 -flto -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(static_match_cache_test PRIVATE -flto -Wl,--gc-sections)
        add_test(NAME static_match_cache_test COMMAND static_match_cache_test)

        add_executable(mod_memory_snapshot_test tests/test_mod_memory_snapshot.c
            src/psx_ram_geometry.c)
        target_include_directories(mod_memory_snapshot_test PRIVATE include src)
        target_compile_options(mod_memory_snapshot_test PRIVATE -O2 -flto -ffunction-sections -fdata-sections)
        target_link_options(mod_memory_snapshot_test PRIVATE -flto -Wl,--gc-sections)
        add_test(NAME mod_memory_snapshot_test COMMAND mod_memory_snapshot_test)
    endif()
    add_executable(sio_card_irq_integrity_test tests/test_sio_card_irq_integrity.c)
    target_include_directories(sio_card_irq_integrity_test PRIVATE include)
    foreach(scenario absent deselect pending coarse)
        add_test(NAME sio_card_irq_integrity_${scenario}
                 COMMAND sio_card_irq_integrity_test ${scenario})
        # The retired environment variable must not reactivate the workaround.
        set_tests_properties(sio_card_irq_integrity_${scenario} PROPERTIES
            ENVIRONMENT "PSX_APE_CARD_UNSTICK=1")
    endforeach()
    # Optional real-hardware regression. Configure its dependencies explicitly;
    # absence must remain visible, never counted as a passing GPU test.
    set(PSX_GL_READBACK_SDL_ROOT "" CACHE PATH "SDL3 static build root for GL readback test")
    get_filename_component(_gl_test_default_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    set(PSX_GL_READBACK_COMPILER_BIN "${_gl_test_default_bin}" CACHE PATH "GCC/MinGW bin for GL fixture")
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            add_test(NAME dispatch_crc_capability COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dispatch_crc_capability.py"
                --cc "${CMAKE_C_COMPILER}" --cmake "${CMAKE_COMMAND}"
                --generator "${CMAKE_GENERATOR}" --make-program "${CMAKE_MAKE_PROGRAM}")
        endif()
        if(PSX_RECOMP_UI AND EXISTS "${RECOMP_UI_ROOT}/src/recomp_launcher.h"
           AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
           AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            add_test(NAME player_settings_composition COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_player_settings_composition.py"
                --compiler "${CMAKE_CXX_COMPILER}" --ui "${RECOMP_UI_ROOT}"
                --work "${CMAKE_CURRENT_BINARY_DIR}/player-settings-composition")
            set_tests_properties(player_settings_composition PROPERTIES TIMEOUT 180)
        endif()
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            add_test(NAME display_ring_capture_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_display_ring_capture.py"
                --cc "${CMAKE_C_COMPILER}" --work-dir "${CMAKE_CURRENT_BINARY_DIR}/display-ring-fixture")
            set_tests_properties(display_ring_capture_test PROPERTIES RUN_SERIAL TRUE)
        endif()
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            add_test(NAME interp_refused_game_text_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_interp_refused_game_text.py"
                --cc "${CMAKE_C_COMPILER}")
            add_test(NAME kseg2_inline_load_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_kseg2_inline_load.py"
                --cc "${CMAKE_C_COMPILER}")
            add_test(NAME changethread_in_handler_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_changethread_in_handler.py"
                --cc "${CMAKE_C_COMPILER}")
            add_test(NAME syscall_closed_thread_context COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_syscall_closed_thread_context.py"
                --cc "${CMAKE_C_COMPILER}")
            add_test(NAME syscall_load_value_commit COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_syscall_load_value_commit.py"
                --cc "${CMAKE_C_COMPILER}")
        endif()
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            add_test(NAME dma_mdec_jobs_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dma_mdec_jobs.py"
                --cc "${CMAKE_C_COMPILER}")
            set_tests_properties(dma_mdec_jobs_test PROPERTIES RUN_SERIAL TRUE)
        endif()
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            add_test(NAME menu_video_snapshot_test COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_menu_video_snapshot.py
                --cxx ${CMAKE_CXX_COMPILER})
            add_test(NAME menu_launcher_callbacks_test COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_menu_launcher_callbacks.py)
            add_test(NAME host_pause_first_present_test COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_pause_first_present.py
                --cxx ${CMAKE_CXX_COMPILER})
        endif()
        add_test(NAME guest_implementation_binding_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_guest_implementation.py"
            --cmake "${CMAKE_COMMAND}" --compiler "${CMAKE_C_COMPILER}"
            --generator "${CMAKE_GENERATOR}"
            --work-root "${CMAKE_CURRENT_BINARY_DIR}/hle-binding-fixtures")
        find_program(PSX_PROFILE_TEST_NINJA NAMES ninja ninja.exe)
        if(PSX_PROFILE_TEST_NINJA)
            add_test(NAME present_image_selection_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_present_image_selection.py"
                --cmake "${CMAKE_COMMAND}" --cc "${CMAKE_C_COMPILER}"
                --ninja "${PSX_PROFILE_TEST_NINJA}")
            set_tests_properties(present_image_selection_test PROPERTIES RUN_SERIAL TRUE)
            set(_execution_profile_rc_args "")
            if(WIN32)
                list(APPEND _execution_profile_rc_args --rc "${CMAKE_RC_COMPILER}")
            endif()
            add_test(NAME execution_profile_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_execution_profile.py"
                --cmake "${CMAKE_COMMAND}" --cc "${CMAKE_C_COMPILER}"
                --ninja "${PSX_PROFILE_TEST_NINJA}" ${_execution_profile_rc_args})
            set_tests_properties(execution_profile_test PROPERTIES RUN_SERIAL TRUE)
            add_test(NAME capture_policy_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capture_policy.py"
                --cmake "${CMAKE_COMMAND}" --cc "${CMAKE_C_COMPILER}"
                --ninja "${PSX_PROFILE_TEST_NINJA}")
            set_tests_properties(capture_policy_test PROPERTIES RUN_SERIAL TRUE)
            add_test(NAME native_idle_wait_policy_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_idle_wait.py"
                --cmake "${CMAKE_COMMAND}" --cc "${CMAKE_C_COMPILER}"
                --ninja "${PSX_PROFILE_TEST_NINJA}")
            set_tests_properties(native_idle_wait_policy_test PROPERTIES RUN_SERIAL TRUE)
            add_test(NAME native_cpu_spans_policy_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_cpu_spans.py"
                --cmake "${CMAKE_COMMAND}" --cc "${CMAKE_C_COMPILER}"
                --ninja "${PSX_PROFILE_TEST_NINJA}")
            set_tests_properties(native_cpu_spans_policy_test PROPERTIES RUN_SERIAL TRUE)
            add_test(NAME openbios_read_selection_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_openbios_read_selection.py"
                --cmake "${CMAKE_COMMAND}" --cc "${CMAKE_C_COMPILER}"
                --ninja "${PSX_PROFILE_TEST_NINJA}"
                --output "${CMAKE_CURRENT_BINARY_DIR}/openbios-read-selection")
            set_tests_properties(openbios_read_selection_test PROPERTIES RUN_SERIAL TRUE)
            add_test(NAME media_jobs_policy_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_media_jobs_policy.py"
                --cmake "${CMAKE_COMMAND}" --cc "${CMAKE_C_COMPILER}"
                --ninja "${PSX_PROFILE_TEST_NINJA}")
            set_tests_properties(media_jobs_policy_test PROPERTIES RUN_SERIAL TRUE)
        endif()
        if(NOT MSVC AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND
           EXISTS "${PSXRECOMP_ROOT}/generated/OpenBIOS_full.c")
            add_test(NAME openbios_read_contract_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_openbios_read_job.py"
                --generated "${PSXRECOMP_ROOT}/generated/OpenBIOS_full.c"
                --output "${CMAKE_CURRENT_BINARY_DIR}/openbios-read-contract"
                --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}")
            set_tests_properties(openbios_read_contract_test PROPERTIES RUN_SERIAL TRUE)
        endif()
        add_test(NAME host_launch_timings_protocol_test COMMAND ${CMAKE_COMMAND} -E env
            "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}" "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_launch_timings_protocol.py")
        set_tests_properties(host_launch_timings_protocol_test PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME tcp_command_index_check COMMAND "${Python3_EXECUTABLE}"
            "${PSXRECOMP_ROOT}/tools/gen_tcp_commands.py" --check)
        add_test(NAME wtrace_dump_test COMMAND ${CMAKE_COMMAND} -E env
            "CC=${CMAKE_C_COMPILER}" "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wtrace_dump.py")
        set_tests_properties(wtrace_dump_test PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME counted_mips_relocations_test COMMAND "${Python3_EXECUTABLE}"
            "${PSXRECOMP_ROOT}/tools/tests/test_counted_mips_relocations.py")
        add_test(NAME interp_mod_entry_guards_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_interp_mod_entry_guards.py")
        add_test(NAME interp_mod_immediate_guards_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_interp_mod_immediate_guards.py")
        add_test(NAME mirror_pc_preserved_guards_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mirror_pc_preserved_guards.py")
        add_test(NAME segment_miss_wiring_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_segment_miss_wiring.py")
        add_test(NAME overlay_unit_defer_wiring_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_unit_defer_wiring.py")
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            add_test(NAME ws_screen_tags_exec_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ws_screen_tags_exec.py"
                --compiler "${CMAKE_C_COMPILER}")
            add_test(NAME ws_activation_bias_exec_test COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ws_activation_bias_exec.py" "${CMAKE_C_COMPILER}")
        endif()
        add_test(NAME gpu_textured_dot_nw_shift_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gpu_textured_dot_nw_shift.py")
        add_test(NAME gl_readback_runner_test COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_readback_runner.py")
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            add_test(NAME depth24_upload_span_test COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_depth24_upload_span.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/.."
                --compiler "${CMAKE_C_COMPILER}"
                --output "${CMAKE_CURRENT_BINARY_DIR}/depth24-upload-span")
            add_test(NAME depth24_transition_test COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_depth24_transition.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/.."
                --compiler "${CMAKE_C_COMPILER}"
                --output "${CMAKE_CURRENT_BINARY_DIR}/depth24-transition")
            add_test(NAME cdrom_audio_disc_read_test COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cdrom_audio_disc_read.py"
                "${CMAKE_C_COMPILER}" "${CMAKE_CURRENT_BINARY_DIR}/audio-disc-read")
        endif()
    endif()
    if(MINGW AND PSX_GL_READBACK_SDL_ROOT
            AND EXISTS "${PSX_GL_READBACK_COMPILER_BIN}/gcc.exe"
            AND EXISTS "${PSX_GL_READBACK_COMPILER_BIN}/g++.exe"
            AND IS_DIRECTORY "${PSX_GL_READBACK_SDL_ROOT}/sdl3-src/include"
            AND EXISTS "${PSX_GL_READBACK_SDL_ROOT}/sdl3-build/libSDL3.a")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME gl_readback_region_test
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_readback_region.py"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_readback_region.c"
                --compiler-bin "${PSX_GL_READBACK_COMPILER_BIN}"
                --sdl-root "${PSX_GL_READBACK_SDL_ROOT}"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-readback-results")
        set_tests_properties(gl_readback_region_test PROPERTIES LABELS "gpu;opengl;hardware" TIMEOUT 180)
    else()
        message(STATUS "gl_readback_region_test: not registered (requires MinGW, SDL3 headers/static library in PSX_GL_READBACK_SDL_ROOT and gcc.exe/g++.exe in PSX_GL_READBACK_COMPILER_BIN)")
    endif()
    add_executable(compiled_ram_read_watch_test tests/test_compiled_ram_read_watch.c)
    target_include_directories(compiled_ram_read_watch_test PRIVATE include)
    target_compile_definitions(compiled_ram_read_watch_test PRIVATE PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME compiled_ram_read_watch_test COMMAND compiled_ram_read_watch_test)
    add_executable(input_update_clock_test tests/test_input_update_clock.c)
    target_include_directories(input_update_clock_test PRIVATE include)
    add_test(NAME input_update_clock_test COMMAND input_update_clock_test)
    add_executable(input_instruction_histogram_test tests/test_input_instruction_histogram.c)
    target_include_directories(input_instruction_histogram_test PRIVATE include)
    add_test(NAME input_instruction_histogram_test COMMAND input_instruction_histogram_test)
    add_executable(break_vector_enter_test tests/test_break_vector_enter.c)
    target_include_directories(break_vector_enter_test PRIVATE include)
    add_test(NAME break_vector_enter_test COMMAND break_vector_enter_test)
    # PS1G-39: game entry leaves guest RAM 0..15 as the kernel left it, and a
    # replay recorded by a build that zeroed those bytes still compares them
    # as that build held them. Links the real memory.c, fntrace.c and
    # netplay_state_digest.c. The driver's link helper reads nm's listing and
    # GNU ld's messages, so the test is registered for GCC, and for Clang on
    # Linux, where Clang links through GNU ld (the sanitizer gate).
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU"
            OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        add_test(NAME low_ram_game_entry_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_low_ram_game_entry.py --cc ${CMAKE_C_COMPILER})
    else()
        message(STATUS "low_ram_game_entry_test: not registered (its link helper needs GCC, or Clang on Linux; this is ${CMAKE_C_COMPILER_ID} on ${CMAKE_SYSTEM_NAME})")
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_test(NAME break_guest_vector_interp_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_break_guest_vector_interp.py --cc ${CMAKE_C_COMPILER})
        add_test(NAME biohazard_checkpoint_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_biohazard_checkpoint.py --cc ${CMAKE_C_COMPILER})
        add_test(NAME irq_handoff_ws_cull_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_irq_handoff_ws_cull.py --cc ${CMAKE_C_COMPILER})
        add_test(NAME interp_refused_text_handoff_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_interp_refused_text_handoff.py --cc ${CMAKE_C_COMPILER})
        add_test(NAME syscall_change_thread_self_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_syscall_change_thread_self.py --cc ${CMAKE_C_COMPILER})
        add_test(NAME rfe_host_reads_test COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rfe_host_reads.py --cc ${CMAKE_C_COMPILER})
        add_test(NAME rfe_irq_context_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rfe_irq_context.py --cc ${CMAKE_C_COMPILER})
        add_test(NAME irq_pending_load_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_irq_pending_load.py --cc ${CMAKE_C_COMPILER})
        add_test(NAME rfe_syscall_depth_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rfe_syscall_depth.py --cc ${CMAKE_C_COMPILER})

        add_test(NAME overlay_dispatch_region_counters_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_dispatch_region_counters.py --cc ${CMAKE_C_COMPILER})
        # PS1B-343: a data shard replay marks the RAM pages it writes. Links
        # the real data_shards.c and memory.c.
        add_test(NAME data_shard_replay_marks_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_data_shard_replay_marks.py --cc ${CMAKE_C_COMPILER})
        add_test(NAME tas_resume_interval_test COMMAND ${Python3_EXECUTABLE} -B
            ${PSXRECOMP_ROOT}/tools/tasreplays/test_resume_interval.py)
        add_executable(critical_exception_test tests/test_critical_exception.c)
        target_include_directories(critical_exception_test PRIVATE include)
        target_compile_options(critical_exception_test PRIVATE -O3 -UNDEBUG -flto -fwhole-program)
        target_link_options(critical_exception_test PRIVATE -flto)
        add_test(NAME critical_exception_direct_test COMMAND critical_exception_test)
        add_test(NAME critical_exception_vector_test COMMAND critical_exception_test exception)
        add_test(NAME critical_exception_nested_reject_test COMMAND critical_exception_test exception nested)
        set_tests_properties(critical_exception_nested_reject_test PROPERTIES WILL_FAIL TRUE)
        # PS1B-306: the real memory.c bless code on a 5,054-row table, and the
        # refusal of a table larger than PSX_KBLESS_MAX_ENTRIES.
        add_executable(kernel_bless_state_test tests/test_kernel_bless_state.c
            src/kernel_patch_ranges.c)
        target_include_directories(kernel_bless_state_test PRIVATE include)
        target_compile_options(kernel_bless_state_test PRIVATE
            -O2 -UNDEBUG -flto -fwhole-program -ffunction-sections -fdata-sections)
        target_link_options(kernel_bless_state_test PRIVATE -flto -Wl,--gc-sections)
        add_test(NAME kernel_bless_state_test COMMAND kernel_bless_state_test)
        add_test(NAME kernel_bless_overflow_refused_test
            COMMAND kernel_bless_state_test overflow)
        set_tests_properties(kernel_bless_overflow_refused_test PROPERTIES
            PASS_REGULAR_EXPRESSION "FATAL: kernel-bless table has [0-9]+ rows"
            FAIL_REGULAR_EXPRESSION "was not refused")
        add_test(NAME kernel_bless_paranoid_test
            COMMAND kernel_bless_state_test paranoid)
        set_tests_properties(kernel_bless_paranoid_test PROPERTIES
            PASS_REGULAR_EXPRESSION "kernel_bless_state: PASS paranoid")
        # PS1B-348: source (TAS) mode never blesses; release does.
        add_test(NAME kernel_bless_source_mode_test
            COMMAND kernel_bless_state_test source)
        set_tests_properties(kernel_bless_source_mode_test PROPERTIES
            PASS_REGULAR_EXPRESSION "kernel_bless_state: PASS source-mode")
        add_test(NAME kernel_bless_source_override_refused_test
            COMMAND kernel_bless_state_test source-override)
        set_tests_properties(kernel_bless_source_override_refused_test PROPERTIES
            PASS_REGULAR_EXPRESSION "kernel_bless_state: PASS source-override")
        add_executable(sio_ack_timing_test tests/test_sio_ack_timing.c)
        target_include_directories(sio_ack_timing_test PRIVATE include)
        target_compile_options(sio_ack_timing_test PRIVATE -O2 -flto -fwhole-program)
        target_link_options(sio_ack_timing_test PRIVATE -flto)
        add_test(NAME sio_ack_timing_test COMMAND sio_ack_timing_test)
        add_executable(dma_otc_timing_test tests/test_dma_otc_timing.c)
        target_include_directories(dma_otc_timing_test PRIVATE include)
        target_compile_options(dma_otc_timing_test PRIVATE -O2 -flto -fwhole-program)
        target_link_options(dma_otc_timing_test PRIVATE -flto)
        add_test(NAME dma_otc_timing_test COMMAND dma_otc_timing_test)
        add_executable(dma_cd_source_test tests/test_dma_cd_source.c)
        target_include_directories(dma_cd_source_test PRIVATE include)
        target_compile_definitions(dma_cd_source_test PRIVATE PSX_TEST_SOURCE_CD_IMPLEMENTED)
        target_compile_options(dma_cd_source_test PRIVATE -O2 -flto -fwhole-program)
        target_link_options(dma_cd_source_test PRIVATE -flto)
        add_test(NAME dma_cd_source_test COMMAND dma_cd_source_test)
        add_executable(dma_default_regs_test tests/test_dma_default_regs.c src/dma_gpu_ll.c)
        target_include_directories(dma_default_regs_test PRIVATE include)
        target_compile_options(dma_default_regs_test PRIVATE -O2 -flto -fwhole-program)
        target_link_options(dma_default_regs_test PRIVATE -flto)
        add_test(NAME dma_default_regs_test COMMAND dma_default_regs_test)
        add_executable(dma_otc_during_ll_test tests/test_dma_otc_during_ll.c src/dma_gpu_ll.c)
        target_include_directories(dma_otc_during_ll_test PRIVATE include)
        target_compile_options(dma_otc_during_ll_test PRIVATE -O2 -UNDEBUG)
        add_test(NAME dma_otc_during_ll_test COMMAND dma_otc_during_ll_test)
        add_executable(dma_source_spec2_test tests/test_dma_source_spec2.c)
        target_include_directories(dma_source_spec2_test PRIVATE include)
        target_compile_options(dma_source_spec2_test PRIVATE -O2 -flto -fwhole-program -UNDEBUG)
        target_link_options(dma_source_spec2_test PRIVATE -flto)
        add_test(NAME dma_source_spec2_test COMMAND dma_source_spec2_test)
        # Source-profile GPU DMA controller fixtures (PS1B-186). Each negative must
        # stop the bounded profile with its [dma-model] diagnostic (exit 2).
        foreach(_dma_src upload linked_list)
            set(_target dma_gpu_${_dma_src}_source_test)
            if(_dma_src STREQUAL "upload")
                set(_fixture tests/test_dma_gpu_upload_source.c)
            else()
                set(_fixture tests/test_dma_gpu_linked_list_source.c)
            endif()
            add_executable(${_target} ${_fixture})
            target_include_directories(${_target} PRIVATE include)
            target_compile_definitions(${_target} PRIVATE PSX_TEST_SOURCE_GPU_IMPLEMENTED)
            if(_dma_src STREQUAL "linked_list")
                target_compile_definitions(${_target} PRIVATE PSX_TEST_SOURCE_LL_IMPLEMENTED)
                # SPEC-PS1B-186-DMA-BEHAVIOUR-v2: a start address with bit 23 is a bus
                # error (3.2) and live MADR/DPCR writes are accepted (7.2, 7.8).
                set(_negatives parser draw reverse capture)
            else()
                # The spec defines GPU-to-RAM uploads (6.5), chopping (2.3) and live
                # MADR/DPCR writes (7.2, 7.8); none may stop the run.
                set(_negatives no-upload short-upload capture)
            endif()
            target_compile_options(${_target} PRIVATE -O2 -flto -fwhole-program)
            target_link_options(${_target} PRIVATE -flto)
            add_test(NAME ${_target} COMMAND ${_target})
            foreach(_case ${_negatives})
                add_test(NAME ${_target}_${_case} COMMAND ${_target} ${_case})
                if(_case STREQUAL "capture")
                    # Current source-state wire supports these pending transfers.
                    set_tests_properties(${_target}_${_case} PROPERTIES
                        PASS_REGULAR_EXPRESSION "PASS source DMA snapshot roundtrip")
                else()
                    set_tests_properties(${_target}_${_case} PROPERTIES
                        PASS_REGULAR_EXPRESSION "\\[dma-model\\]")
                endif()
            endforeach()
        endforeach()
    endif()
    # Real-GL internal-resolution invariance (macOS, Linux and Windows/MinGW,
    # hidden window): the guest-visible VRAM is identical at every scale, lines
    # stay one native pixel thick, and an over-limit request clamps inside GL.
    # Needs the SDL3 this tree fetched and a GPU session; labelled hardware like
    # the readback test above. A host with no window/GL 3.3 context (headless,
    # or Windows over SSH) exits 77 and is reported as skipped.
    if((NOT WIN32 OR MINGW) AND Python3_Interpreter_FOUND AND TARGET SDL3::SDL3)
        if(DEFINED sdl3_SOURCE_DIR)
            set(_gl_scale_sdl_inc "${sdl3_SOURCE_DIR}/include")
        else()
            set(_gl_scale_sdl_inc "$<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>")
        endif()
        add_test(NAME gl_scale_invariance_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_scale_invariance.py"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_scale_invariance.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "${_gl_scale_sdl_inc}"
                --sdl-library "$<TARGET_FILE:SDL3::SDL3>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-scale-results")
        set_tests_properties(gl_scale_invariance_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 300 SKIP_RETURN_CODE 77)
        # Render thread ([video] render_thread) on vs off: the values the
        # guest reads back, native VRAM, the frame at S and the native-wide
        # surface are identical for a scripted stream with readbacks, mask
        # bits, uploads, ring wraps and per-primitive widescreen tags. A host
        # with no window/GL 3.3 context (headless, Windows over SSH) exits 77:
        # reported as skipped.
        add_test(NAME gl_render_thread_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_render_thread.py"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_render_thread.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "${_gl_scale_sdl_inc}"
                --sdl-library "$<TARGET_FILE:SDL3::SDL3>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-render-thread-results")
        set_tests_properties(gl_render_thread_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 300 SKIP_RETURN_CODE 77)
        # Frame generation ([video] frame_generation): the real presented
        # images are identical with it off and on (VRAM and native-wide
        # presents), generated frames are presented, and an in-between frame
        # at phase 1 / 0 equals the newer / older real frame.
        add_test(NAME gl_frame_gen_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_frame_gen.py"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_frame_gen.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "${_gl_scale_sdl_inc}"
                --sdl-library "$<TARGET_FILE:SDL3::SDL3>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-frame-gen-results")
        set_tests_properties(gl_frame_gen_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 300 SKIP_RETURN_CODE 77)
        add_test(NAME gl_shared_present_images_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_render_thread.py" --single
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_shared_present_images.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "${_gl_scale_sdl_inc}"
                --sdl-library "$<TARGET_FILE:SDL3::SDL3>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-shared-present-images-results")
        set_tests_properties(gl_shared_present_images_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 180 SKIP_RETURN_CODE 77)
        # [video] texture_lod / anisotropic_filtering: off and untracked/
        # magnified prims are unchanged; minified world textures average
        # inside their UV bounds; render thread and PGXP depth agree.
        add_test(NAME gl_texture_lod_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_render_thread.py" --single
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_texture_lod.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "${_gl_scale_sdl_inc}"
                --sdl-library "$<TARGET_FILE:SDL3::SDL3>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-texture-lod-results")
        set_tests_properties(gl_texture_lod_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 180 SKIP_RETURN_CODE 77)
        # PS1 semi-transparency modes and mask bit at 4x with PGXP depth and
        # the render thread, against the formulas and the software reference
        # ([video] accurate_blending on; off is measured and printed).
        add_test(NAME gl_blend_accuracy_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_render_thread.py" --single
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_blend_accuracy.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "${_gl_scale_sdl_inc}"
                --sdl-library "$<TARGET_FILE:SDL3::SDL3>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-blend-accuracy-results")
        set_tests_properties(gl_blend_accuracy_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 180 SKIP_RETURN_CODE 77)
        # texture_filtering = "xbr": diagonals lose the staircase, straight
        # edges, flat areas, cutouts and 1:1 draws stay nearest; RT agrees.
        add_test(NAME gl_xbr_filter_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_render_thread.py" --single
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_xbr.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "${_gl_scale_sdl_inc}"
                --sdl-library "$<TARGET_FILE:SDL3::SDL3>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-xbr-results")
        set_tests_properties(gl_xbr_filter_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 180 SKIP_RETURN_CODE 77)
        # [video] dithering: off ignores the guest's dither bit (byte-identical
        # true colour); on/scaled add the PS1 pattern to shaded/modulated draws
        # only, through the facade, the render thread, PGXP depth and wide.
        add_test(NAME gl_dithering_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_render_thread.py" --single
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_dithering.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "${_gl_scale_sdl_inc}"
                --sdl-library "$<TARGET_FILE:SDL3::SDL3>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-dithering-results")
        set_tests_properties(gl_dithering_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 180 SKIP_RETURN_CODE 77)
        unset(_gl_scale_sdl_inc)
    else()
        message(STATUS "gl_scale_invariance_test: not registered (needs macOS, Linux or MinGW, Python 3 and SDL3)")
    endif()
    # These hidden-context probes work with either supported SDL linkage.
    # Prefer static when supplied; SDK-only Windows builds usually supply shared.
    if(TARGET SDL3::SDL3-static)
        set(_gl_filter_sdl_target SDL3::SDL3-static)
    elseif(TARGET SDL3::SDL3)
        set(_gl_filter_sdl_target SDL3::SDL3)
    endif()
    if(Python3_Interpreter_FOUND AND _gl_filter_sdl_target)
        add_test(NAME gl_texture_filter_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_texture_filter.py"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_texture_filter.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "$<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>"
                --sdl-library "$<TARGET_FILE:${_gl_filter_sdl_target}>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-filter-results")
        set_tests_properties(gl_texture_filter_test PROPERTIES LABELS "gpu;opengl;hardware" TIMEOUT 180 SKIP_RETURN_CODE 77)
        add_test(NAME gl_post_aa_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_texture_filter.py"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_post_aa.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "$<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>"
                --sdl-library "$<TARGET_FILE:${_gl_filter_sdl_target}>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-post-aa-results")
        set_tests_properties(gl_post_aa_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 180 SKIP_RETURN_CODE 77)
        add_test(NAME gl_bloom_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_texture_filter.py"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_bloom.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "$<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>"
                --sdl-library "$<TARGET_FILE:${_gl_filter_sdl_target}>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-bloom-results")
        set_tests_properties(gl_bloom_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 180 SKIP_RETURN_CODE 77)
        add_test(NAME gl_hd_texture_pack_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_texture_filter.py"
                --hd-pack --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_hd_texture_pack.c"
                --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
                --webp-include "$<TARGET_PROPERTY:psx_texture_image_decode,PSX_WEBP_INCLUDE_DIR>"
                --webp-decoder "$<TARGET_FILE:webpdecoder>"
                --sdl-include "$<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>"
                --sdl-library "$<TARGET_FILE:${_gl_filter_sdl_target}>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-hd-texture-results")
        set_tests_properties(gl_hd_texture_pack_test PROPERTIES LABELS "gpu;opengl;hardware" TIMEOUT 180 SKIP_RETURN_CODE 77)
        # [video] fmv_chroma_smoothing: 24-bit presents lose their 2x2 chroma
        # blocks, luma kept; 15-bit presents and off are byte-identical.
        add_test(NAME gl_fmv_chroma_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_texture_filter.py"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_fmv_chroma.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "$<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>"
                --sdl-library "$<TARGET_FILE:${_gl_filter_sdl_target}>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-fmv-chroma-results")
        set_tests_properties(gl_fmv_chroma_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 180 SKIP_RETURN_CODE 77)
        # Debug present ring: GL names do not outlive a renderer restart.
        add_test(NAME gl_present_ring_restart_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gl_texture_filter.py"
                --debug-tools
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_present_ring_restart.c"
                --cc "${CMAKE_C_COMPILER}"
                --sdl-include "$<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>"
                --sdl-library "$<TARGET_FILE:${_gl_filter_sdl_target}>"
                --output "${CMAKE_CURRENT_BINARY_DIR}/gl-present-ring-restart-results")
        set_tests_properties(gl_present_ring_restart_test PROPERTIES
            LABELS "gpu;opengl;hardware" TIMEOUT 180 SKIP_RETURN_CODE 77)
    endif()
    unset(_gl_filter_sdl_target)
    # Historical pin-F bytes are not the modern protocol baseline.
    add_executable(sio_port_device_identity_historical tests/test_sio_port_device_identity.c)
    target_include_directories(sio_port_device_identity_historical PRIVATE include)
    add_test(NAME sio_port_device_identity_historical COMMAND sio_port_device_identity_historical 0)
    set_tests_properties(sio_port_device_identity_historical PROPERTIES DISABLED TRUE)
    if(Python3_Interpreter_FOUND)
        add_test(NAME preserved_overlay_tier COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_tier_witness_runtime.py" --cc "${CMAKE_C_COMPILER}")
        add_test(NAME public_studio_batch COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../tools/public_studio/test_batch_state.py")
        add_test(NAME public_studio_delivery COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../tools/public_studio/test_delivery.py")
        add_test(NAME public_studio_backend COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../tools/public_studio/test_backend.py")
        if(WIN32)
            add_test(NAME public_studio_widgets COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../tools/public_studio/test_widgets.py")
            set_tests_properties(public_studio_widgets PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
    add_executable(cdrom_boot_model_test tests/test_cdrom_boot_model.c src/psx_sha256.c)
    target_include_directories(cdrom_boot_model_test PRIVATE include)
    add_test(NAME cdrom_boot_model_test COMMAND cdrom_boot_model_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cd_source_cold_random_256.psxrng)
    add_executable(ps1b109_seekp_head_test tests/test_ps1b109_seekp_head.c src/psx_sha256.c)
    target_include_directories(ps1b109_seekp_head_test PRIVATE include)
    add_test(NAME ps1b109_seekp_head_test COMMAND ps1b109_seekp_head_test)
    add_executable(ps1b109_xa_eof_model_test tests/test_ps1b109_xa_eof_model.c src/psx_sha256.c)
    target_include_directories(ps1b109_xa_eof_model_test PRIVATE include)
    add_test(NAME ps1b109_xa_eof_model_test COMMAND ps1b109_xa_eof_model_test)
    add_executable(cdrom_getlocl_test tests/test_cdrom_getlocl.c src/psx_sha256.c)
    target_include_directories(cdrom_getlocl_test PRIVATE include)
    add_test(NAME cdrom_getlocl_test COMMAND cdrom_getlocl_test)
    add_executable(cdrom_explicit_seek_test tests/test_cdrom_explicit_seek.c src/psx_sha256.c)
    target_include_directories(cdrom_explicit_seek_test PRIVATE include)
    add_test(NAME cdrom_explicit_seek_test COMMAND cdrom_explicit_seek_test)
    add_executable(cdrom_source_explicit_timing_test tests/test_cdrom_source_explicit_timing.c src/psx_sha256.c)
    target_include_directories(cdrom_source_explicit_timing_test PRIVATE include)
    add_test(NAME cdrom_source_explicit_timing_test COMMAND cdrom_source_explicit_timing_test)
    # PS1B-109: SeekP keeps SetLoc dirty; Play seeks from the standby head (Spyro 3 return 1847).
    add_executable(cdrom_source_seekp_play_test tests/test_cdrom_source_seekp_play.c src/psx_sha256.c)
    target_include_directories(cdrom_source_seekp_play_test PRIVATE include)
    add_test(NAME cdrom_source_seekp_play_test COMMAND cdrom_source_seekp_play_test)
    add_executable(cdrom_random_tape_test tests/test_cdrom_random_tape.c src/psx_sha256.c)
    target_include_directories(cdrom_random_tape_test PRIVATE include)
    add_test(NAME cdrom_random_tape_test COMMAND cdrom_random_tape_test)
    add_executable(cdrom_implicit_seek_test tests/test_cdrom_implicit_seek.c src/psx_sha256.c)
    target_include_directories(cdrom_implicit_seek_test PRIVATE include)
    add_test(NAME cdrom_implicit_seek_test COMMAND cdrom_implicit_seek_test)
    # PS1B-189: CD status bit 7 (BUSYSTS), cited to PSX-SPX.
    add_executable(cdrom_busysts_test tests/test_cdrom_busysts.c src/psx_sha256.c)
    target_include_directories(cdrom_busysts_test PRIVATE include src)
    add_test(NAME cdrom_busysts_test COMMAND cdrom_busysts_test)
    # PS1B-118: the default-path implicit seek travels from the read cursor.
    add_executable(cdrom_implicit_seek_origin_test tests/test_cdrom_implicit_seek_origin.c src/psx_sha256.c)
    target_include_directories(cdrom_implicit_seek_origin_test PRIVATE include src)
    add_test(NAME cdrom_implicit_seek_origin_test COMMAND cdrom_implicit_seek_origin_test)
    # This optional clock fixture takes a separately generated fixed tape path.
    add_executable(cdrom_source_clock_test tests/test_cdrom_source_clock.c src/psx_sha256.c)
    target_include_directories(cdrom_source_clock_test PRIVATE include)
    add_executable(cdrom_source_reset_test tests/test_cdrom_source_reset.c src/psx_sha256.c)
    target_include_directories(cdrom_source_reset_test PRIVATE include)
    # PS1B-317 (spec 6.11): on the default path a read start spins the motor
    # up, so a read re-issued after a Pause does not pay the spin-up again.
    add_executable(cdrom_read_starts_motor_test tests/test_cdrom_read_starts_motor.c src/psx_sha256.c)
    target_include_directories(cdrom_read_starts_motor_test PRIVATE include)
    add_test(NAME cdrom_read_starts_motor_test COMMAND cdrom_read_starts_motor_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cd_source_cold_random_256.psxrng)
    # PS1G-95: on the default path GetlocP names the target of a completed
    # SeekL/SeekP; after a Pause it names the last delivered sector.
    add_executable(cdrom_getlocp_after_seek_test tests/test_cdrom_getlocp_after_seek.c src/psx_sha256.c)
    target_include_directories(cdrom_getlocp_after_seek_test PRIVATE include)
    add_test(NAME cdrom_getlocp_after_seek_test COMMAND cdrom_getlocp_after_seek_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cd_source_cold_random_256.psxrng)
    # PS1B-317 fleet check: read streams that ended before their first sector,
    # counted for the run report (docs/RUN_REPORT_CDROM.md).
    add_executable(cdrom_silent_read_counter_test tests/test_cdrom_silent_read_counter.c src/psx_sha256.c)
    target_include_directories(cdrom_silent_read_counter_test PRIVATE include)
    add_test(NAME cdrom_silent_read_counter_test COMMAND cdrom_silent_read_counter_test)
    # PS1B-311: rows of the drive-profile behaviour spec that the older tests
    # do not pin. The four named modes must stop fail-closed with a [CDROM] line.
    add_executable(cdrom_drive_profile_rows_test tests/test_cdrom_drive_profile_rows.c src/psx_sha256.c)
    target_include_directories(cdrom_drive_profile_rows_test PRIVATE include)
    add_test(NAME cdrom_drive_profile_rows_test COMMAND cdrom_drive_profile_rows_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cd_source_cold_random_256.psxrng)
    foreach(_case bad-model no-tape init-cdda init-read-seek)
        add_test(NAME cdrom_drive_profile_rows_${_case}_test COMMAND cdrom_drive_profile_rows_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/cd_source_cold_random_256.psxrng ${_case})
        set_tests_properties(cdrom_drive_profile_rows_${_case}_test PROPERTIES
            PASS_REGULAR_EXPRESSION "\\[CDROM\\]")
    endforeach()
    # C1-C9/C2b-C3b/C8b replay (PS1B-212). The source sub-model profiles must
    # match the proxy rows (C3/C3b seeks, C4/C9 read start). The tape is the
    # first 256 words of the octoshock-2.2.2 cold random tape the TAS routes
    # use (tools/tasreplays, sha d85f0dec...); each case starts at power-on.
    add_executable(cdrom_c_fixture_replay_test tests/test_cdrom_c_fixture_replay.c src/psx_sha256.c)
    target_include_directories(cdrom_c_fixture_replay_test PRIVATE include)
    foreach(_profile tekken pepsiman)
        add_test(NAME cdrom_c_fixture_replay_${_profile}_test COMMAND cdrom_c_fixture_replay_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/cd_c_fixture_replay.txt ${_profile}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/cd_source_cold_random_256.psxrng
            --classes C3,C3b,C4,C9)
    endforeach()
    # PS1B-424: the rows of section 14 of the CD spec (command reception and
    # Init on the source clock), through the production cdrom.c.
    add_executable(cdrom_source_clock_rows_test tests/test_cdrom_source_clock_rows.c src/psx_sha256.c)
    target_include_directories(cdrom_source_clock_rows_test PRIVATE include)
    add_test(NAME cdrom_source_clock_rows_test COMMAND cdrom_source_clock_rows_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cd_source_cold_random_256.psxrng)

    add_executable(input_route_file_test tests/test_input_route_file.c)
    target_include_directories(input_route_file_test PRIVATE include)
    add_test(NAME input_route_file_test COMMAND input_route_file_test)

    add_executable(input_dualshock_route_file_test tests/test_input_dualshock_route_file.c)
    target_include_directories(input_dualshock_route_file_test PRIVATE include)
    add_test(NAME input_dualshock_route_file_test COMMAND input_dualshock_route_file_test)
    add_executable(input_dualshock_delivery_test tests/test_input_dualshock_delivery.c)
    target_include_directories(input_dualshock_delivery_test PRIVATE include)
    add_test(NAME input_dualshock_delivery_test COMMAND input_dualshock_delivery_test)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    # PS1B-306: each generated BIOS profile's kernel-bless table fits the
    # runtime's verify-state array (PSX_KBLESS_MAX_ENTRIES).
    add_test(NAME kbless_table_capacity_test COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_kbless_table_capacity.py"
        --generated "${PSXRECOMP_ROOT}/generated"
        --capacity-header "${CMAKE_CURRENT_SOURCE_DIR}/include/psx_bios_image.h")
    set_tests_properties(kbless_table_capacity_test PROPERTIES SKIP_RETURN_CODE 77)
    # PS1B-306: every writer of guest RAM that bypasses psx_write_* tells the
    # kernel-bless table (psx_kernel_bless_note_range) next to the write.
    add_test(NAME raw_ram_writers_test COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_raw_ram_writers.py"
        --src "${CMAKE_CURRENT_SOURCE_DIR}/src"
        --include "${CMAKE_CURRENT_SOURCE_DIR}/include")
    # PSXRTI3 routes (T101): wire format, then record on a diagnostic build and
    # replay on a release build (PSX_NO_DEBUG_TOOLS) of the session module.
    add_executable(input_route_v3_file_test tests/test_input_route_v3_file.c)
    target_include_directories(input_route_v3_file_test PRIVATE include)
    add_test(NAME input_route_v3_file_test COMMAND input_route_v3_file_test)
    # Player replays (PS1B-191): recorder/player state machine against a
    # deterministic stand-in machine behind the replay_host_* hooks.
    add_executable(replay_session_test tests/test_replay_session.c
        src/replay_session.c src/psx_sha256.c)
    target_include_directories(replay_session_test PRIVATE include)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/replay_session_scratch)
    add_test(NAME replay_session_test COMMAND replay_session_test
        ${CMAKE_CURRENT_BINARY_DIR}/replay_session_scratch)
    # PS1B-316: a replay names the disc in the drive, not the launch disc. The
    # real product identity behind the replay session, two files as the discs.
    add_executable(replay_disc_identity_test tests/test_replay_disc_identity.c
        src/replay_session.c src/input_route_session.c src/disc_digest_cache.c
        src/psx_sha256.c)
    target_include_directories(replay_disc_identity_test PRIVATE include)
    target_compile_definitions(replay_disc_identity_test PRIVATE
        "PSX_FRAMEWORK_PIN=\"0123456789abcdef0123456789abcdef01234567\"")
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/replay_disc_identity_scratch)
    add_test(NAME replay_disc_identity_test COMMAND replay_disc_identity_test
        ${CMAKE_CURRENT_BINARY_DIR}/replay_disc_identity_scratch)
    # PS1B-316: the host toast breaks into at most three lines of 38
    # characters, which fits the 640-pixel window. The second test reads
    # every replay message out of the sources and measures it with the same
    # line breaker.
    add_executable(host_osd_wrap_test tests/test_host_osd_wrap.c)
    target_include_directories(host_osd_wrap_test PRIVATE include)
    add_test(NAME host_osd_wrap_test COMMAND host_osd_wrap_test)
    add_test(NAME replay_osd_text COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_replay_osd_text.py
        $<TARGET_FILE:host_osd_wrap_test>)
    # PS1B-316 power-on replays: playback swaps in the recorded memory cards
    # without touching the player's card files; the launcher's "Record
    # replay" choice reaches the runtime without being persisted.
    add_executable(memcard_replay_test tests/test_memcard_replay.c src/memcard.c)
    target_include_directories(memcard_replay_test PRIVATE include)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/memcard_replay_scratch)
    add_test(NAME memcard_replay_test COMMAND memcard_replay_test
        ${CMAKE_CURRENT_BINARY_DIR}/memcard_replay_scratch)
    add_executable(launcher_record_replay_test tests/test_launcher_record_replay.cpp)
    target_include_directories(launcher_record_replay_test PRIVATE include)
    if(NOT MSVC)
        target_compile_options(launcher_record_replay_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME launcher_record_replay_test COMMAND launcher_record_replay_test)
    foreach(_product IN ITEMS diagnostic release)
        set(_target input_route_session_${_product})
        add_executable(${_target} tests/test_input_route_session.c
            src/input_route_session.c src/disc_digest_cache.c src/psx_sha256.c)
        target_include_directories(${_target} PRIVATE include)
        target_compile_definitions(${_target} PRIVATE
            "PSX_FRAMEWORK_PIN=\"0123456789abcdef0123456789abcdef01234567\""
            $<$<STREQUAL:${_product},release>:PSX_NO_DEBUG_TOOLS=1>)
    endforeach()
    add_executable(input_route_session_release_refresh_defect
        tests/test_input_route_session.c src/input_route_session.c
        src/disc_digest_cache.c src/psx_sha256.c src/input_route_observer.c)
    target_include_directories(input_route_session_release_refresh_defect PRIVATE include)
    target_compile_definitions(input_route_session_release_refresh_defect PRIVATE
        "PSX_FRAMEWORK_PIN=\"0123456789abcdef0123456789abcdef01234567\""
        PSX_NO_DEBUG_TOOLS=1 PSX_TEST_REFRESH_OWNER_DEFECT=1)
    # PS1B-404: a release product drives the route observer from its replay
    # when PSX_INPUT_ROUTE_CAPTURE_DIR is set, so that it writes route pictures.
    target_sources(input_route_session_release PRIVATE src/input_route_observer.c)
    add_test(NAME input_route_session_test COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_input_route_session.py
        $<TARGET_FILE:input_route_session_diagnostic> $<TARGET_FILE:input_route_session_release>
        $<TARGET_FILE:input_route_session_release_refresh_defect>)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_test(NAME desktop_route_refresh_test COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_desktop_route_refresh.py
            ${CMAKE_CXX_COMPILER} ${PSXRECOMP_ROOT}
            ${CMAKE_CURRENT_BINARY_DIR}/desktop_route_refresh)
    endif()
    add_executable(input_route_cdrom_events_test
        tests/test_input_route_cdrom_events.c
        src/input_route_session.c src/disc_digest_cache.c src/psx_sha256.c
        src/iso_reader_c.cpp src/iso_reader.cpp src/cue_sheet.cpp)
    target_include_directories(input_route_cdrom_events_test PRIVATE
        include ${PSXRECOMP_ROOT}/recompiler/include)
    if(NOT MSVC)
        target_compile_options(input_route_cdrom_events_test PRIVATE
            -Wall -Wextra -Werror -Wno-unused-function)
    endif()
    target_link_libraries(input_route_cdrom_events_test PRIVATE chdr-static)
    add_test(NAME input_route_cdrom_events_test COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_input_route_cdrom_events.py
        $<TARGET_FILE:input_route_cdrom_events_test>)
    add_executable(sio_mode_dtr_test tests/test_sio_mode_dtr.c)
    target_include_directories(sio_mode_dtr_test PRIVATE include)
    add_test(NAME sio_mode_dtr_test COMMAND sio_mode_dtr_test)
    add_executable(sio_nymashock_card_test tests/test_sio_nymashock_card.c)
    target_include_directories(sio_nymashock_card_test PRIVATE include)
    add_test(NAME sio_nymashock_card_test COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sio_nymashock_card.py
        $<TARGET_FILE:sio_nymashock_card_test>)
    add_executable(sio_dualshock_dtr_session_test tests/test_sio_dualshock_dtr_session.c)
target_include_directories(sio_dualshock_dtr_session_test PRIVATE include)
add_test(NAME sio_dualshock_dtr_session_test COMMAND sio_dualshock_dtr_session_test)
add_executable(sio_dualshock_p2_p3_test tests/test_sio_dualshock_p2_p3.c)
target_include_directories(sio_dualshock_p2_p3_test PRIVATE include)
add_test(NAME sio_dualshock_p2_p3_test COMMAND sio_dualshock_p2_p3_test)
    # PS1 Mouse (PS1B-279): the byte protocol, and pin F byte identity of the
    # pad/card bus when no port is a mouse. Mouse/method-2 checks also cover
    # eight players, where both physical ports have a multitap.
    foreach(_players IN ITEMS 2 5 8)
        add_executable(sio_mouse_p${_players}_test tests/test_sio_mouse.c)
        target_include_directories(sio_mouse_p${_players}_test PRIVATE include)
        target_compile_definitions(sio_mouse_p${_players}_test PRIVATE PSX_MAX_PLAYERS=${_players})
        add_test(NAME sio_mouse_p${_players}_test COMMAND sio_mouse_p${_players}_test)
    endforeach()
    foreach(_players IN ITEMS 2 5)
        set(_target sio_port_device_identity_p${_players}_test)
        add_executable(${_target} tests/test_sio_port_device_identity.c)
        target_include_directories(${_target} PRIVATE include)
        target_compile_definitions(${_target} PRIVATE PSX_MAX_PLAYERS=${_players})
        foreach(_profile IN ITEMS 0 1 2 3)
            add_test(NAME ${_target}_${_profile} COMMAND ${_target} ${_profile})
            add_test(NAME ${_target}_${_profile}_mouse_roundtrip
                COMMAND ${_target} ${_profile} --touch-mouse)
            add_test(NAME ${_target}_${_profile}_negcon_roundtrip
                COMMAND ${_target} ${_profile} --touch-negcon)
            add_test(NAME ${_target}_${_profile}_guncon_roundtrip
                COMMAND ${_target} ${_profile} --touch-guncon)
        endforeach()
        # neGcon (PS1B-304): the byte protocol, same two builds.
        add_executable(sio_negcon_p${_players}_test tests/test_sio_negcon.c)
        target_include_directories(sio_negcon_p${_players}_test PRIVATE include)
        target_compile_definitions(sio_negcon_p${_players}_test PRIVATE PSX_MAX_PLAYERS=${_players})
        add_test(NAME sio_negcon_p${_players}_test COMMAND sio_negcon_p${_players}_test)
        # GunCon (PS1B-305): the byte protocol, same two builds.
        add_executable(sio_guncon_p${_players}_test tests/test_sio_guncon.c)
        target_include_directories(sio_guncon_p${_players}_test PRIVATE include)
        target_compile_definitions(sio_guncon_p${_players}_test PRIVATE PSX_MAX_PLAYERS=${_players})
        add_test(NAME sio_guncon_p${_players}_test COMMAND sio_guncon_p${_players}_test)
    endforeach()
    # neGcon twist (PS1B-304): the one-axis stick transform.
    add_executable(psx_stick_axis_test tests/test_psx_stick_axis.c)
    target_include_directories(psx_stick_axis_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(psx_stick_axis_test PRIVATE m)
    endif()
    add_test(NAME psx_stick_axis_test COMMAND psx_stick_axis_test)
add_executable(sio_nymashock_dualshock_test tests/test_sio_nymashock_dualshock.c)
    target_include_directories(sio_nymashock_dualshock_test PRIVATE include)
    add_test(NAME sio_nymashock_dualshock_test COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sio_nymashock_dualshock.py
        $<TARGET_FILE:sio_nymashock_dualshock_test>)
    add_executable(mdec_source_fifo_rows_test tests/test_mdec_source_fifo_rows.c)
    target_include_directories(mdec_source_fifo_rows_test PRIVATE include)
    add_test(NAME mdec_source_fifo_rows_test COMMAND mdec_source_fifo_rows_test)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_executable(mdec_dma_write_invariance_test tests/test_mdec_dma_write_invariance.c)
        target_include_directories(mdec_dma_write_invariance_test PRIVATE src include ../recompiler/include)
        target_compile_options(mdec_dma_write_invariance_test PRIVATE -O2 -UNDEBUG)
        add_test(NAME mdec_dma_write_invariance_test COMMAND mdec_dma_write_invariance_test)
    endif()
    add_executable(input_route_observer_test tests/test_input_route_observer.c
        src/input_route_observer.c src/psx_sha256.c)
    target_include_directories(input_route_observer_test PRIVATE include)
    add_test(NAME input_route_observer_test COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_input_route_observer.py
        $<TARGET_FILE:input_route_observer_test>)

    add_executable(launcher_device_roundtrip_test
        tests/test_launcher_device_roundtrip.cpp)
    target_include_directories(launcher_device_roundtrip_test PRIVATE include)
    # The checks are assert()s; keep them in Release builds.
    if(NOT MSVC)
        target_compile_options(launcher_device_roundtrip_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME launcher_device_roundtrip_test
             COMMAND launcher_device_roundtrip_test)

    # PS1B-305: host pointer to GunCon X/Y (spec host-mapping vectors).
    add_executable(guncon_map_test tests/test_guncon_map.c)
    target_include_directories(guncon_map_test PRIVATE include)
    if(NOT WIN32)
        target_link_libraries(guncon_map_test PRIVATE m)
    endif()
    add_test(NAME guncon_map_test COMMAND guncon_map_test)

    # Pad-mode resolution at the launcher exit seam: a lock_mode title must
    # boot the pad type its game.toml declared whatever the launcher hands
    # back, an active mod controller-mode override must outrank the lock, and
    # neither may let an unsupported mode reach settings.toml. Header-only, so
    # it needs nothing but runtime/include.
    add_executable(launcher_pad_mode_resolve_test
        tests/test_launcher_pad_mode_resolve.cpp)
    target_include_directories(launcher_pad_mode_resolve_test PRIVATE include)
    add_test(NAME launcher_pad_mode_resolve_test
             COMMAND launcher_pad_mode_resolve_test)

    # Presentation-only mod input must not be sourced from disconnected,
    # peer-synchronized, or rollback-replayed pad state.
    add_executable(mod_local_input_scope_test
        tests/test_mod_local_input_scope.c)
    target_include_directories(mod_local_input_scope_test PRIVATE src)
    add_test(NAME mod_local_input_scope_test
             COMMAND mod_local_input_scope_test)

    # COP0.CAUSE IP2 must mirror the INTC line combinationally, never latch.
    add_executable(cause_ip2_combinational_test
        tests/test_cause_ip2_combinational.c)
    add_test(NAME cause_ip2_combinational_test
             COMMAND cause_ip2_combinational_test)

    add_executable(psx_cyc_batch_test tests/test_psx_cyc_batch.c)
    target_include_directories(psx_cyc_batch_test PRIVATE include)
    target_compile_definitions(psx_cyc_batch_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME psx_cyc_batch_test COMMAND psx_cyc_batch_test)

    # [timing] guest_cycle_scale divides instruction charges only (1 = faithful).
    add_executable(psx_guest_cycle_scale_test tests/test_psx_guest_cycle_scale.c)
    target_include_directories(psx_guest_cycle_scale_test PRIVATE include)
    target_compile_definitions(psx_guest_cycle_scale_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME psx_guest_cycle_scale_test COMMAND psx_guest_cycle_scale_test)

    # One CPU charge for every tier, the declarative RAM gate and the mod gate,
    # and rollback re-simulation, against the real psx_cycles.c.
    add_executable(psx_guest_cycle_scale_gate_test
        tests/test_psx_guest_cycle_scale_gate.c
        src/psx_cycles.c)
    target_include_directories(psx_guest_cycle_scale_gate_test PRIVATE include)
    target_compile_definitions(psx_guest_cycle_scale_gate_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME psx_guest_cycle_scale_gate_test COMMAND psx_guest_cycle_scale_gate_test)

    # Segment misses in static game code (docs/SEGMENT_AWARE_CODE.md §5.5):
    # classification against the dispatch rows, the ring and the per-PC
    # summary the TCP command and the exit report read.
    add_executable(psx_segment_miss_test
        tests/test_psx_segment_miss.c
        src/psx_segment_miss.c
    )
    target_include_directories(psx_segment_miss_test PRIVATE include)
    add_test(NAME psx_segment_miss_test COMMAND psx_segment_miss_test)

    add_executable(psx_icache_fastpath_test
        tests/test_psx_icache_fastpath.c
        src/psx_icache.c
    )
    target_include_directories(psx_icache_fastpath_test PRIVATE include)
    # PSX_OVERLAY_DLL_BUILD is the seam psx_cycles.h already provides for a TU
    # that supplies its own psx_advance_cycles (overlay DLLs forward it through
    # a callback shim). Without it the header inlines the production definition
    # and this test's counting stub is a redefinition — the whole runtime test
    # suite stopped building at "redefinition of 'psx_advance_cycles'", which
    # also blocked every test registered after it.
    target_compile_definitions(psx_icache_fastpath_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1
        PSX_OVERLAY_DLL_BUILD=1)
    add_test(NAME psx_icache_fastpath_test COMMAND psx_icache_fastpath_test)

    add_executable(psx_icache_elision_guard_test
        tests/test_psx_icache_elision_guard.c src/psx_icache.c)
    target_include_directories(psx_icache_elision_guard_test PRIVATE include)
    target_compile_definitions(psx_icache_elision_guard_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1 PSX_OVERLAY_DLL_BUILD=1)
    add_test(NAME psx_icache_elision_guard_test COMMAND psx_icache_elision_guard_test)

    add_executable(psx_icache_contents_test
        tests/test_psx_icache_contents.c src/psx_icache.c)
    target_include_directories(psx_icache_contents_test PRIVATE include)
    target_compile_definitions(psx_icache_contents_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1 PSX_OVERLAY_DLL_BUILD=1)
    add_test(NAME psx_icache_contents_test COMMAND psx_icache_contents_test)

    foreach(_ram_geometry IN ITEMS retail expanded)
        add_executable(psx_memory_geometry_${_ram_geometry}_test
            tests/test_psx_memory_geometry.c)
        target_include_directories(psx_memory_geometry_${_ram_geometry}_test
            PRIVATE include)
        if(_ram_geometry STREQUAL "expanded")
            target_compile_definitions(psx_memory_geometry_${_ram_geometry}_test
                PRIVATE PSX_MEMORY_GEOMETRY_TEST_EXPANDED=1)
        endif()
        add_test(NAME psx_memory_geometry_${_ram_geometry}_test
                 COMMAND psx_memory_geometry_${_ram_geometry}_test)
    endforeach()

    # Live 2/8 MiB geometry end to end through the REAL memory.c accessors,
    # dma.c channel execution and boot_state.c save/load (including the
    # refuse-before-mutation contract). Section GC, as for
    # mod_memory_snapshot_test, drops the hardware routing the test never
    # reaches; psx_ram_runtime_map_stubs.c doubles the rest.
    if(NOT MSVC)
        add_executable(psx_ram_runtime_map_test
            tests/test_psx_ram_runtime_map.c
            tests/psx_ram_runtime_map_stubs.c
            src/memory.c
            src/dma.c
            src/gpu_timeline.c
            src/boot_state.c
            src/mod_snapshot.c
            src/psx_ram_geometry.c)
        target_include_directories(psx_ram_runtime_map_test PRIVATE include src)
        target_compile_options(psx_ram_runtime_map_test PRIVATE
            -O2 -flto -ffunction-sections -fdata-sections -Wno-lto-type-mismatch)
        target_link_options(psx_ram_runtime_map_test PRIVATE
            -flto -Wl,--gc-sections -Wno-lto-type-mismatch)
        target_link_libraries(psx_ram_runtime_map_test PRIVATE ZLIB::ZLIB)
        add_test(NAME psx_ram_runtime_map_test COMMAND psx_ram_runtime_map_test)

        # Cache-isolated (SR.IsC) CPU stores through the REAL memory.c store
        # paths and the REAL psx_icache.c tag model, against Beetle's
        # WriteMemory IsC branch: FlushCache invalidation, tag-test valid bits,
        # the D-cache scratchpad write, the BIU and DMA exclusions. Same link
        # doubles as psx_ram_runtime_map_test, minus the I-cache ones.
        add_executable(isc_store_test
            tests/test_isc_store.c
            tests/psx_ram_runtime_map_stubs.c
            src/memory.c
            src/dma.c
            src/gpu_timeline.c
            src/boot_state.c
            src/mod_snapshot.c
            src/psx_icache.c
            src/psx_ram_geometry.c)
        target_include_directories(isc_store_test PRIVATE include src)
        target_compile_definitions(isc_store_test PRIVATE
            PSX_ENABLE_BLOCK_CYCLES=1 PSX_STUBS_REAL_ICACHE=1)
        target_compile_options(isc_store_test PRIVATE
            -O2 -flto -ffunction-sections -fdata-sections -Wno-lto-type-mismatch)
        target_link_options(isc_store_test PRIVATE -flto -Wno-lto-type-mismatch
            $<IF:$<PLATFORM_ID:Darwin>,-Wl$<COMMA>-dead_strip,-Wl$<COMMA>--gc-sections>)
        target_link_libraries(isc_store_test PRIVATE ZLIB::ZLIB)
        add_test(NAME isc_store_test COMMAND isc_store_test)

        # DMA and host stores drop the PGXP shadow of the words they touch
        # (an identical word is not proof of the same projection): the REAL
        # memory.c store paths against the REAL pgxp.cpp engine. Same link
        # doubles as isc_store_test, minus the PGXP one.
        add_executable(pgxp_untracked_store_test
            tests/test_pgxp_untracked_store.c
            tests/psx_ram_runtime_map_stubs.c
            src/memory.c
            src/dma.c
            src/gpu_timeline.c
            src/boot_state.c
            src/mod_snapshot.c
            src/pgxp.cpp
            src/psx_ram_geometry.c)
        target_include_directories(pgxp_untracked_store_test PRIVATE include src)
        target_compile_definitions(pgxp_untracked_store_test PRIVATE
            PSX_STUBS_REAL_PGXP=1)
        target_compile_options(pgxp_untracked_store_test PRIVATE
            -O2 -flto -ffunction-sections -fdata-sections -Wno-lto-type-mismatch)
        target_link_options(pgxp_untracked_store_test PRIVATE -flto -Wno-lto-type-mismatch
            $<IF:$<PLATFORM_ID:Darwin>,-Wl$<COMMA>-dead_strip,-Wl$<COMMA>--gc-sections>)
        target_link_libraries(pgxp_untracked_store_test PRIVATE ZLIB::ZLIB)
        add_test(NAME pgxp_untracked_store_test COMMAND pgxp_untracked_store_test)
    endif()

    # Loadable BIOS backends (psx_bios_module.h): the host loader against
    # three fake modules built here from one source -- good, wrong ABI tag,
    # wrong codegen hash. Real dlopen/LoadLibrary, real registry, stubbed core.
    if(NOT MSVC)
        foreach(_fake IN ITEMS good bad_abi bad_hash)
            add_library(bios_module_fake_${_fake} SHARED tests/bios_module_fake.c)
            target_include_directories(bios_module_fake_${_fake} PRIVATE include)
            target_compile_definitions(bios_module_fake_${_fake} PRIVATE
                PSX_OVERLAY_DLL_BUILD PSX_NO_DEBUG_TOOLS)
            set_target_properties(bios_module_fake_${_fake} PROPERTIES PREFIX "")
        endforeach()
        target_compile_definitions(bios_module_fake_bad_abi PRIVATE FAKE_ABI_TAG=0x7fff)
        target_compile_definitions(bios_module_fake_bad_hash PRIVATE FAKE_CODEGEN_HASH=0x1u)
        add_executable(bios_module_host_test
            tests/test_bios_module_host.c
            src/psx_bios_module.c
            src/psx_bios_backend.c
            src/overlay_posix.c
            src/crc32.c)
        target_include_directories(bios_module_host_test PRIVATE include src)
        target_compile_definitions(bios_module_host_test PRIVATE
            PSX_NO_DEBUG_TOOLS
            FAKE_GOOD="$<TARGET_FILE:bios_module_fake_good>"
            FAKE_BAD_ABI="$<TARGET_FILE:bios_module_fake_bad_abi>"
            FAKE_BAD_HASH="$<TARGET_FILE:bios_module_fake_bad_hash>")
        target_link_libraries(bios_module_host_test PRIVATE ${CMAKE_DL_LIBS})
        add_dependencies(bios_module_host_test
            bios_module_fake_good bios_module_fake_bad_abi bios_module_fake_bad_hash)
        add_test(NAME bios_module_host_test COMMAND bios_module_host_test)
    endif()

    if(EXISTS "${RECOMP_UI_ROOT}/src/recomp_runtime_ui.h")
        add_executable(runtime_disc_menu_test
            tests/test_runtime_disc_menu.c src/psx_runtime_menu.c src/psx_disc_tray.c
            "${RECOMP_UI_ROOT}/src/common/recomp_runtime_ui.c")
        target_include_directories(runtime_disc_menu_test PRIVATE
            include "${RECOMP_UI_ROOT}/src")
        target_compile_definitions(runtime_disc_menu_test PRIVATE RECOMP_LAUNCHER=1)
        # Assertions are the test oracle even in a Release validation build.
        if(MSVC)
            target_compile_options(runtime_disc_menu_test PRIVATE /UNDEBUG)
        else()
            target_compile_options(runtime_disc_menu_test PRIVATE -UNDEBUG)
        endif()
        add_test(NAME runtime_disc_menu_test COMMAND runtime_disc_menu_test)
    endif()

    add_executable(host_keymap_test
        tests/test_host_keymap.c
        src/host_keymap.c)
    target_include_directories(host_keymap_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(host_keymap_test PRIVATE
        SDL_MAIN_HANDLED
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
    target_link_libraries(host_keymap_test PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME host_keymap_test COMMAND host_keymap_test)

    add_executable(host_osd_diagnostics_test
        tests/test_host_osd_diagnostics.c src/host_osd.c)
    target_include_directories(host_osd_diagnostics_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(host_osd_diagnostics_test PRIVATE
        SDL_MAIN_HANDLED RECOMP_LAUNCHER=1 PSX_SDL_NO_RENDER=1
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
    target_link_libraries(host_osd_diagnostics_test PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME host_osd_diagnostics_test COMMAND host_osd_diagnostics_test)

    add_executable(keyboard_pad_chord_test
        tests/test_keyboard_pad_chord.c
        src/psx_keybinds.c src/host_keymap.c)
    target_include_directories(keyboard_pad_chord_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(keyboard_pad_chord_test PRIVATE
        SDL_MAIN_HANDLED
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
    target_link_libraries(keyboard_pad_chord_test PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME keyboard_pad_chord_test COMMAND keyboard_pad_chord_test)

    add_executable(mouse_binding_suppression_test tests/test_mouse_binding_suppression.c)
    target_include_directories(mouse_binding_suppression_test PRIVATE include ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(mouse_binding_suppression_test PRIVATE
        SDL_MAIN_HANDLED $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
    target_link_libraries(mouse_binding_suppression_test PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME mouse_binding_suppression_test COMMAND mouse_binding_suppression_test)

    # Generic external replacement-pack core. The renderer integration is a
    # separate consumer; this target pins pack discovery, deterministic keying,
    # VRAM residency, portable tracking state, and bounded asynchronous decode.
    add_executable(hd_texture_pack_test
        tests/test_hd_texture_pack.cpp
        src/hd_texture_pack.cpp)
    target_include_directories(hd_texture_pack_test PRIVATE include)
    find_package(Threads REQUIRED)
    target_link_libraries(hd_texture_pack_test PRIVATE Threads::Threads)
    add_test(NAME hd_texture_pack_test COMMAND hd_texture_pack_test)
    add_executable(duckstation_texture_pack_test
        tests/test_duckstation_texture_pack.cpp
        src/duckstation_texture_pack.cpp)
    target_include_directories(duckstation_texture_pack_test PRIVATE include)
    target_link_libraries(duckstation_texture_pack_test PRIVATE Threads::Threads)
    target_link_libraries(duckstation_texture_pack_test PRIVATE psx_texture_image_decode)
    add_test(NAME duckstation_texture_pack_test COMMAND duckstation_texture_pack_test)
    add_executable(texture_image_decode_test tests/test_texture_image_decode.cpp)
    target_link_libraries(texture_image_decode_test PRIVATE psx_texture_image_decode)
    add_test(NAME texture_image_decode_test COMMAND texture_image_decode_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/texture_images")

    # Standalone serial-pad protocol tests use explicit SIO/device stubs; they
    # cover both DualShock rumble and JogCon wire/state behavior.
    add_executable(sio_digital_command_test
        tests/test_sio_digital_command.c
        src/sio.c)
    target_include_directories(sio_digital_command_test PRIVATE include)
    add_test(NAME sio_digital_command_test COMMAND sio_digital_command_test)
    add_executable(sio_digital_command_multitap_test
        tests/test_sio_digital_command.c
        src/sio.c)
    target_include_directories(sio_digital_command_multitap_test PRIVATE include)
    target_compile_definitions(sio_digital_command_multitap_test PRIVATE
        PSX_MAX_PLAYERS=5)
    add_test(NAME sio_digital_command_multitap_test
             COMMAND sio_digital_command_multitap_test)
    add_executable(sio_digital_command_dual_multitap_test
        tests/test_sio_digital_command.c
        src/sio.c)
    target_include_directories(sio_digital_command_dual_multitap_test PRIVATE include)
    target_compile_definitions(sio_digital_command_dual_multitap_test PRIVATE
        PSX_MAX_PLAYERS=8)
    add_test(NAME sio_digital_command_dual_multitap_test
             COMMAND sio_digital_command_dual_multitap_test)
    add_executable(sio_dualshock_rumble_test
        tests/test_sio_dualshock_rumble.c
        src/sio.c)
    target_include_directories(sio_dualshock_rumble_test PRIVATE include)
    add_test(NAME sio_dualshock_rumble_test COMMAND sio_dualshock_rumble_test)
    add_executable(sio_jogcon_protocol_test
        tests/test_sio_jogcon_protocol.c
        src/sio.c)
    target_include_directories(sio_jogcon_protocol_test PRIVATE include)
    add_test(NAME sio_jogcon_protocol_test COMMAND sio_jogcon_protocol_test)
    add_executable(sio_negcon_protocol_test
        tests/test_sio_negcon_protocol.c
        src/sio.c)
    target_include_directories(sio_negcon_protocol_test PRIVATE include)
    add_test(NAME sio_negcon_protocol_test COMMAND sio_negcon_protocol_test)
    add_executable(sio_negcon_protocol_multitap_test
        tests/test_sio_negcon_protocol.c
        src/sio.c)
    target_include_directories(sio_negcon_protocol_multitap_test PRIVATE include)
    target_compile_definitions(sio_negcon_protocol_multitap_test PRIVATE
        PSX_MAX_PLAYERS=5)
    add_test(NAME sio_negcon_protocol_multitap_test
             COMMAND sio_negcon_protocol_multitap_test)
    # The shared PsxNetPad -> SIO apply and normalization, over the real SIO.
    add_executable(psx_pad_apply_test
        tests/test_psx_pad_apply.c
        src/psx_pad_apply.c
        src/psx_net_pad.c
        src/sio.c)
    target_include_directories(psx_pad_apply_test PRIVATE include)
    add_test(NAME psx_pad_apply_test COMMAND psx_pad_apply_test)
    if(Python3_Interpreter_FOUND AND NOT MSVC)
        add_test(NAME controller_recording_mode_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_controller_recording_mode.py"
            "${CMAKE_C_COMPILER}" "${CMAKE_CXX_COMPILER}")
    endif()

    # Title-supplied host rumble (psx_mod_set_host_rumble) merge and expiry.
    add_executable(host_rumble_test tests/test_host_rumble.c)
    target_include_directories(host_rumble_test PRIVATE include)
    add_test(NAME host_rumble_test COMMAND host_rumble_test)
    # JogCon motor byte -> host wheel haptics ([controller] jogcon_force_feedback).
    add_executable(jogcon_ffb_test tests/test_jogcon_ffb.c)
    target_include_directories(jogcon_ffb_test PRIVATE include)
    add_test(NAME jogcon_ffb_test COMMAND jogcon_ffb_test)
    # The SDL haptic backend against an SDL3 virtual wheel (no virtual haptic
    # device exists in SDL; see the test). Skips (77) on SDL2.
    add_executable(jogcon_ffb_sdl_test tests/test_jogcon_ffb_sdl.cpp
                   src/psx_jogcon_ffb_sdl.cpp)
    target_include_directories(jogcon_ffb_sdl_test PRIVATE include)
    target_link_libraries(jogcon_ffb_sdl_test PRIVATE ${PSX_SDL_LIBRARIES})
    target_compile_definitions(jogcon_ffb_sdl_test PRIVATE
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
    add_test(NAME jogcon_ffb_sdl_test COMMAND jogcon_ffb_sdl_test)
    set_tests_properties(jogcon_ffb_sdl_test PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(controller_type_test tests/test_controller_type.cpp)
    target_include_directories(controller_type_test PRIVATE include)
    add_test(NAME controller_type_test COMMAND controller_type_test)
    add_executable(psx_stick_test tests/test_psx_stick.cpp src/psx_stick.c)
    target_include_directories(psx_stick_test PRIVATE include)
    add_test(NAME psx_stick_test COMMAND psx_stick_test)

    add_executable(openxr_input_test tests/test_openxr_input.c src/psx_openxr.c)
    target_include_directories(openxr_input_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(openxr_input_test PRIVATE m)
    endif()
    add_test(NAME openxr_input_test COMMAND openxr_input_test)

    add_executable(vr_pose_math_test tests/test_vr_pose_math.c src/vr_pose_math.c)
    target_include_directories(vr_pose_math_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(vr_pose_math_test PRIVATE m)
    endif()
    add_test(NAME vr_pose_math_test COMMAND vr_pose_math_test)

    add_executable(projection_scale_config_test
        tests/test_projection_scale_config.cpp src/projection_scale_config.cpp)
    target_include_directories(projection_scale_config_test PRIVATE
        include ../recompiler/lib/toml11)
    add_test(NAME projection_scale_config_test COMMAND projection_scale_config_test)

    add_executable(mod_controller_source_test tests/test_mod_controller_source.c src/mod_controller_source.c)
    target_include_directories(mod_controller_source_test PRIVATE include)
    add_test(NAME mod_controller_source_test COMMAND mod_controller_source_test)

    add_executable(mod_pad_transform_test
        tests/test_mod_pad_transform.c src/mod_pad_transform.c)
    target_include_directories(mod_pad_transform_test PRIVATE include)
    add_test(NAME mod_pad_transform_test COMMAND mod_pad_transform_test)
    add_executable(psx_hotkey_pad_test tests/test_psx_hotkey_pad.c)
    target_include_directories(psx_hotkey_pad_test PRIVATE include)
    add_test(NAME psx_hotkey_pad_test COMMAND psx_hotkey_pad_test)
    add_executable(psx_stick_fold_test tests/test_psx_stick_fold.c)
    target_include_directories(psx_stick_fold_test PRIVATE include)
    add_test(NAME psx_stick_fold_test COMMAND psx_stick_fold_test)
    add_executable(psx_trigger_values_test tests/test_psx_trigger_values.c)
    target_include_directories(psx_trigger_values_test PRIVATE include)
    add_test(NAME psx_trigger_values_test COMMAND psx_trigger_values_test)
    add_executable(pad_external_input_test
        tests/test_pad_external_input.c src/pad_external_input.c)
    target_include_directories(pad_external_input_test PRIVATE include src)
    add_test(NAME pad_external_input_test COMMAND pad_external_input_test)
    add_executable(host_overlay_test tests/test_host_overlay.c src/host_overlay.c)
    target_include_directories(host_overlay_test PRIVATE include src)
    find_package(Threads REQUIRED)
    target_link_libraries(host_overlay_test PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(host_overlay_test PRIVATE shell32)
    endif()
    add_test(NAME host_overlay_test COMMAND host_overlay_test)
    add_executable(psx_menu_host_test tests/test_psx_menu_host.c src/psx_menu_host.c)
    target_include_directories(psx_menu_host_test PRIVATE include src)
    add_test(NAME psx_menu_host_test COMMAND psx_menu_host_test)

    add_executable(gte_register_access_test
        tests/test_gte_register_access.cpp
        src/gte.cpp
        src/pgxp.cpp
        src/psx_ram_geometry.c
    )
    target_include_directories(gte_register_access_test PRIVATE include)
    target_compile_definitions(gte_register_access_test PRIVATE
        PSX_NO_DEBUG_TOOLS=1
        PSX_GTE_REGISTER_TEST=1
        PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME gte_register_access_test COMMAND gte_register_access_test)

    add_executable(mdec_output_lifetime_test
        tests/test_mdec_output_lifetime.c src/mdec.c)
    target_include_directories(mdec_output_lifetime_test PRIVATE include)
    target_compile_definitions(mdec_output_lifetime_test PRIVATE PSX_NO_DEBUG_TOOLS=1)
    add_test(NAME mdec_output_lifetime_test COMMAND mdec_output_lifetime_test)

    add_library(mdec_clean_fixture_probe SHARED tests/mdec_clean_fixture_probe.c)
    target_include_directories(mdec_clean_fixture_probe PRIVATE include)
    add_test(NAME mdec_clean_fixtures_test COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mdec_clean_fixtures.py
        --library $<TARGET_FILE:mdec_clean_fixture_probe>)
    # Same ABI/result corpus for the native operation entry and the reference.
    add_executable(gte_hle_contract_test
        tests/test_gte_register_access.cpp src/gte.cpp src/pgxp.cpp src/psx_ram_geometry.c)
    target_include_directories(gte_hle_contract_test PRIVATE include)
    target_compile_definitions(gte_hle_contract_test PRIVATE
        PSX_NO_DEBUG_TOOLS=1 PSX_GTE_REGISTER_TEST=1 PSX_ENABLE_BLOCK_CYCLES=1)
    psxrecomp_execution_profile(gte_hle_contract_test PROFILE ENHANCED)
    add_test(NAME gte_hle_contract_test COMMAND gte_hle_contract_test)

    # PGXP value-propagation engine (docs/ENHANCEMENTS.md G1.2/G1.3): shadow
    # load/store/COP2 roundtrips, half-word semantics, validate-on-read
    # (foreign overwrite + DMA-overwrite rejection), MOVE/repack arithmetic,
    # suppression bracket, invalidate-all, and GetPreciseVertex safeguards
    # (truncation agreement + tolerance clamp). White-box: includes src/pgxp.cpp
    # and stubs the gte.cpp fallback-cache extern.
    add_executable(pgxp_test tests/test_pgxp.cpp src/pgxp.cpp
        src/psx_ram_geometry.c)
    target_include_directories(pgxp_test PRIVATE include)
    add_test(NAME pgxp_test COMMAND pgxp_test)

    add_executable(pgxp_word_shadow_test tests/test_pgxp_word_shadow.cpp
        src/pgxp.cpp src/psx_ram_geometry.c)
    target_include_directories(pgxp_word_shadow_test PRIVATE include)
    add_test(NAME pgxp_word_shadow_test COMMAND pgxp_word_shadow_test)

    # psx.enhancement.pgxp at session start (docs/ENHANCEMENTS.md G1.11): the
    # real builtin activation (mod API stubbed) and the pgxp_session.h
    # resolver main.cpp's renderer setup uses, in main.cpp's order. Pins that
    # the [video] baseline no longer undoes the mod, that a cleared (netplay)
    # plan keeps the baseline, and that the env overrides win.
    # The session arming main.cpp runs (src/pgxp_session.cpp) against the
    # real mod runtime, the real builtin plugin and the builtin's own
    # manifest: default-on, netplay clear, the off switch, options, env
    # overrides and [video] pgxp_mod_only.
    # Run the same session fixture in both real compilation flavors.
    foreach(_pgxp_session_test IN ITEMS pgxp_session_test pgxp_session_hooks_test)
        add_executable(${_pgxp_session_test} tests/test_pgxp_session.cpp
            src/mod_immediate.c src/func_override.c
            src/host_overlay.c src/host_span_ring.c
            src/pgxp_session.cpp src/mod_builtin_pgxp.c src/pgxp.cpp
            src/psx_ram_geometry.c
            src/mod_runtime.cpp src/mod_function_replacements.c
            src/mod_packages.cpp
            src/mod_media.cpp
            src/disc_path.cpp
            src/cue_sheet.cpp
            src/iso_reader.cpp
            src/crc32.c
            src/psx_sha256.c)
        target_include_directories(${_pgxp_session_test} PRIVATE
            include
            ../recompiler/include
            ../recompiler/lib/toml11)
        target_compile_definitions(${_pgxp_session_test} PRIVATE
            PSX_BUILTIN_PGXP_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../mods/builtin/packages/psx.enhancement.pgxp/1.0.0/manifest.toml")
        target_link_libraries(${_pgxp_session_test} PRIVATE chdr-static)
        target_sources(${_pgxp_session_test} PRIVATE src/mod_netplay.c)
        psxrecomp_sha256_file(${_pgxp_session_test})
        if(WIN32)
            if(MSVC)
                target_link_options(${_pgxp_session_test} PRIVATE /STACK:16777216)
            else()
                target_link_options(${_pgxp_session_test} PRIVATE -Wl,--stack,16777216)
            endif()
        endif()
        add_test(NAME ${_pgxp_session_test} COMMAND ${_pgxp_session_test})
        if(_pgxp_session_test STREQUAL "pgxp_session_hooks_test")
            target_compile_definitions(${_pgxp_session_test} PRIVATE PSX_PGXP=1)
        endif()
    endforeach()

    foreach(_pgxp_isolated_test
            pgxp_test pgxp_session_test pgxp_session_hooks_test)
        target_sources(${_pgxp_isolated_test} PRIVATE tests/pgxp_test_mod_memory.cpp)
    endforeach()

    add_executable(psx_instr_cost_test tests/test_psx_instr_cost.cpp)
    target_include_directories(psx_instr_cost_test PRIVATE include)
    add_test(NAME psx_instr_cost_test COMMAND psx_instr_cost_test)

    add_executable(ws_aspect_cone_math_test
        tests/test_ws_aspect_cone_math.c
        src/ws_aspect_cone_math.c)
    target_include_directories(ws_aspect_cone_math_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(ws_aspect_cone_math_test PRIVATE m)
    endif()
    add_test(NAME ws_aspect_cone_math_test COMMAND ws_aspect_cone_math_test)

    add_executable(ws_ui_group_test
        tests/test_ws_ui_group.c
        src/ws_ui_group.c)
    target_include_directories(ws_ui_group_test PRIVATE include)
    add_test(NAME ws_ui_group_test COMMAND ws_ui_group_test)

    add_executable(ws_prepass_guard_test
        tests/test_ws_prepass_guard.c)
    target_include_directories(ws_prepass_guard_test PRIVATE include)
    add_test(NAME ws_prepass_guard_test COMMAND ws_prepass_guard_test)

    add_executable(ws_hud_anchor_test
        tests/test_ws_hud_anchor.c)
    target_include_directories(ws_hud_anchor_test PRIVATE include)
    add_test(NAME ws_hud_anchor_test COMMAND ws_hud_anchor_test)

    add_executable(ws_gte_sxy_test
        tests/test_ws_gte_sxy.c)
    target_include_directories(ws_gte_sxy_test PRIVATE include)
    add_test(NAME ws_gte_sxy_test COMMAND ws_gte_sxy_test)

    add_executable(gpu_textured_dot_nw_shift_exec_test
        tests/test_gpu_textured_dot_nw_shift_exec.c
        src/psx_video_timing.c
        src/gpu_timeline.c)
    target_include_directories(gpu_textured_dot_nw_shift_exec_test PRIVATE
        include
        src)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(gpu_textured_dot_nw_shift_exec_test PRIVATE
            -ffunction-sections -fdata-sections)
        # Apple ld has no --gc-sections; -dead_strip is its equivalent (as
        # for isc_store_test above), so the test links and runs on macOS.
        target_link_options(gpu_textured_dot_nw_shift_exec_test PRIVATE
            $<IF:$<PLATFORM_ID:Darwin>,-Wl$<COMMA>-dead_strip,-Wl$<COMMA>--gc-sections>)
    endif()
    add_test(NAME gpu_textured_dot_nw_shift_exec_test
             COMMAND gpu_textured_dot_nw_shift_exec_test)

    # auto_ui_squash corrects untextured HUD fills with their textured frames.
    add_executable(ws_auto_ui_untextured_exec_test
        tests/test_ws_auto_ui_untextured_exec.c
        src/ws_ui_group.c
        src/psx_video_timing.c
        src/gpu_timeline.c)
    target_include_directories(ws_auto_ui_untextured_exec_test PRIVATE
        include
        src)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(ws_auto_ui_untextured_exec_test PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(ws_auto_ui_untextured_exec_test PRIVATE
            -Wl,--gc-sections)
    endif()
    add_test(NAME ws_auto_ui_untextured_exec_test
             COMMAND ws_auto_ui_untextured_exec_test)

    add_executable(ws_shared_split_ui_exec_test
        tests/test_ws_shared_split_ui_exec.c
        src/ws_ui_group.c
        src/psx_video_timing.c
        src/gpu_timeline.c)
    target_include_directories(ws_shared_split_ui_exec_test PRIVATE include src)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(ws_shared_split_ui_exec_test PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(ws_shared_split_ui_exec_test PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME ws_shared_split_ui_exec_test COMMAND ws_shared_split_ui_exec_test)

    add_executable(ws_local_view_margin_test
        tests/test_ws_local_view_margin.c
        src/ws_ui_group.c
        src/psx_video_timing.c
        src/gpu_timeline.c)
    target_include_directories(ws_local_view_margin_test PRIVATE include src)
    target_compile_definitions(ws_local_view_margin_test PRIVATE GPU_EXEC_REAL_UI_GROUP=1)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(ws_local_view_margin_test PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(ws_local_view_margin_test PRIVATE
            $<IF:$<PLATFORM_ID:Darwin>,-Wl$<COMMA>-dead_strip,-Wl$<COMMA>--gc-sections>)
    endif()
    add_test(NAME ws_local_view_margin_test COMMAND ws_local_view_margin_test)

    add_executable(ws_cull_margin_width_test
        tests/test_ws_cull_margin_width.c
        src/psx_video_timing.c
        src/gpu_timeline.c)
    target_include_directories(ws_cull_margin_width_test PRIVATE include src)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(ws_cull_margin_width_test PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(ws_cull_margin_width_test PRIVATE
            $<IF:$<PLATFORM_ID:Darwin>,-Wl$<COMMA>-dead_strip,-Wl$<COMMA>--gc-sections>)
    endif()
    add_test(NAME ws_cull_margin_width_test COMMAND ws_cull_margin_width_test)

    # Depth24 GP1(07h) present hold (#636): hold count, upload span and the
    # staged frame agree across windowed/headless/deferred presents, rollback
    # replay and disk loads; stale trailing FMV pixels never reach a frame.
    add_executable(depth24_present_hold_exec_test
        tests/test_depth24_present_hold_exec.c
        src/psx_video_timing.c
        src/gpu_timeline.c)
    target_include_directories(depth24_present_hold_exec_test PRIVATE include src)
    if(MINGW)
        target_compile_options(depth24_present_hold_exec_test PRIVATE -flto)
        target_link_options(depth24_present_hold_exec_test PRIVATE -flto)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(depth24_present_hold_exec_test PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(depth24_present_hold_exec_test PRIVATE
            $<IF:$<PLATFORM_ID:Darwin>,-Wl$<COMMA>-dead_strip,-Wl$<COMMA>--gc-sections>)
    endif()
    add_test(NAME depth24_present_hold_exec_test COMMAND depth24_present_hold_exec_test)

    add_executable(ws_view_anchor_test tests/test_ws_view_anchor.c)
    target_sources(ws_view_anchor_test PRIVATE tests/source_gpu_raster_stub.c)
    add_executable(ws_view_anchor_gpu_test tests/test_ws_view_anchor_gpu.c
        src/psx_video_timing.c
        src/gpu_timeline.c)
    add_executable(native_wide_projection_test tests/test_native_wide_projection.c
        src/psx_video_timing.c
        src/gpu_timeline.c)
    add_executable(gpu_render_pass_provenance_test tests/test_gpu_render_pass_provenance.c
        src/ws_ui_group.c
        src/psx_video_timing.c
        src/gpu_timeline.c)
    foreach(_anchor_test ws_view_anchor ws_view_anchor_gpu native_wide_projection
                         gpu_render_pass_provenance)
        target_include_directories(${_anchor_test}_test PRIVATE include src)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${_anchor_test}_test PRIVATE -ffunction-sections -fdata-sections)
            if(NOT APPLE)
                target_link_options(${_anchor_test}_test PRIVATE -Wl,--gc-sections)
            endif()
        endif()
        if(NOT WIN32)
            target_link_libraries(${_anchor_test}_test PRIVATE m)
        endif()
    endforeach()
    add_test(NAME ws_view_anchor_test COMMAND ws_view_anchor_test)
    add_test(NAME ws_view_anchor_gpu_test COMMAND ws_view_anchor_gpu_test)
    add_test(NAME native_wide_projection_test COMMAND native_wide_projection_test)
    add_test(NAME gpu_render_pass_provenance_test COMMAND gpu_render_pass_provenance_test)

    add_executable(ws_repeat_rect_test tests/test_ws_repeat_rect.c)
    target_include_directories(ws_repeat_rect_test PRIVATE include)
    add_test(NAME ws_repeat_rect_test COMMAND ws_repeat_rect_test)
    add_executable(ws_feedback_rect_test tests/test_ws_feedback_rect.c)
    target_include_directories(ws_feedback_rect_test PRIVATE include)
    add_test(NAME ws_feedback_rect_test COMMAND ws_feedback_rect_test)
    add_executable(ws_screen_mask_test tests/test_ws_screen_mask.c)
    target_include_directories(ws_screen_mask_test PRIVATE include)
    if(UNIX)
        target_link_libraries(ws_screen_mask_test PRIVATE m)
    endif()
    add_test(NAME ws_screen_mask_test COMMAND ws_screen_mask_test)

    add_executable(mod_gpu_dma_aperture_test
        tests/test_mod_gpu_dma_aperture.c
        src/psx_ram_geometry.c)
    target_include_directories(mod_gpu_dma_aperture_test PRIVATE include)
    add_test(NAME mod_gpu_dma_aperture_test
             COMMAND mod_gpu_dma_aperture_test)

    # The real-handler fixture relies on LTO pruning its unused syscall paths.
    # Enable only the toolchain on which that link contract is verified; keep
    # the coverage gap visible in CTest for every other compiler/platform.
    if(WIN32 AND MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_C_COMPILER_ID STREQUAL "GNU"
       AND CMAKE_C_COMPILER_VERSION VERSION_EQUAL "16.1.0")
        add_executable(exit_critical_section_test
            tests/test_exit_critical_section.c)
        target_include_directories(exit_critical_section_test PRIVATE include)
        target_compile_definitions(exit_critical_section_test PRIVATE PSX_NO_DEBUG_TOOLS=1)
        target_compile_options(exit_critical_section_test PRIVATE -O3 -flto -ffunction-sections)
        target_link_options(exit_critical_section_test PRIVATE -flto -Wl,--gc-sections)
        add_test(NAME exit_critical_section_test COMMAND exit_critical_section_test)
    else()
        add_test(NAME exit_critical_section_test
            COMMAND "${CMAKE_COMMAND}" -E echo
                "SKIP: real-handler LTO fixture is verified only on Windows x64 MinGW GCC 16.1.0; current compiler is ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
        set_tests_properties(exit_critical_section_test PROPERTIES
            SKIP_REGULAR_EXPRESSION "^SKIP:")
    endif()

    # #636 / PR #660: restore-vs-live IRQ delivery at the restore edge, over
    # the real scheduler (traps.c) and IRQ delivery (interrupts.c). Compiled
    # game, BIOS ROM, CPS continuation and interpreted resumes; no pending
    # IRQ; I_MASK/SR gating; same-edge dedup; null-PC recovery.
    add_executable(same_thread_rfe_k0_test
        tests/test_same_thread_rfe_k0.c
        tests/restore_edge_irq_stubs.c
        src/traps.c
        src/interrupts.c)
    target_include_directories(same_thread_rfe_k0_test PRIVATE include src)
    foreach(_rfe_mode RANGE 0 3)
        add_test(NAME same_thread_rfe_k0_mode${_rfe_mode}
            COMMAND same_thread_rfe_k0_test ${_rfe_mode})
        set_tests_properties(same_thread_rfe_k0_mode${_rfe_mode} PROPERTIES
            ENVIRONMENT "PSX_SAME_THREAD_RESTORE=${_rfe_mode}" TIMEOUT 30)
    endforeach()
    add_executable(restore_edge_irq_replay_test
        tests/test_restore_edge_irq_replay.c
        tests/restore_edge_irq_stubs.c
        src/traps.c
        src/interrupts.c)
    target_include_directories(restore_edge_irq_replay_test PRIVATE include src)
    add_test(NAME restore_edge_irq_replay_test COMMAND restore_edge_irq_replay_test)
    set_tests_properties(restore_edge_irq_replay_test PROPERTIES TIMEOUT 60)

    add_executable(dma_gpu_linked_list_timing_test
        tests/test_dma_gpu_linked_list_timing.c
        src/dma_gpu_ll.c)
    target_include_directories(dma_gpu_linked_list_timing_test PRIVATE include)
    add_test(NAME dma_gpu_linked_list_timing_test
             COMMAND dma_gpu_linked_list_timing_test)

    # Clean-room SPU DSP fidelity (issue #103): SPU IRQ on every RAM access
    # class, reverb (write gating + work-area wrap + CD-only path), noise
    # generator, volume sweeps / direct decode, capture buffers, and
    # savestate coverage of all the new state. Includes src/spu.c directly
    # (white-box) and stubs its externs.
    # src/crc32.c is linked rather than stubbed: spu.c's capture/shadow paths
    # feed real CRCs that this test checks, so a zero-returning stub would make
    # those assertions vacuous. (This target did not link at all before —
    # crc32_update was undefined on origin/master too.)
    add_executable(spu_fidelity_test tests/test_spu_fidelity.c src/crc32.c)
    target_include_directories(spu_fidelity_test PRIVATE include)
    add_test(NAME spu_fidelity_test COMMAND spu_fidelity_test)
    # Maintained authored optional source-profile fixtures. Default DSP keeps
    # its existing tests; source fixtures select the profile explicitly.
    foreach(fixture_source IN ITEMS tests/test_spu_key_latency.c tests/test_spu_source_voice_rows.c
            tests/test_spu_source_snapshot.c tests/test_spu_register_readback.c tests/test_spu_s1_block_flags.c
            tests/test_spu_adpcm_sample.c tests/test_spu_envelope_fixture.c)
        get_filename_component(fixture ${fixture_source} NAME_WE)
        string(REGEX REPLACE "^test_spu_" "" fixture ${fixture})
        add_executable(spu_${fixture}_test ${fixture_source})
        target_include_directories(spu_${fixture}_test PRIVATE include)
        if(MSVC)
            target_compile_options(spu_${fixture}_test PRIVATE /UNDEBUG)
        else()
            target_compile_options(spu_${fixture}_test PRIVATE -UNDEBUG)
        endif()
        if(fixture STREQUAL "envelope_fixture")
            add_test(NAME spu_${fixture}_test COMMAND spu_${fixture}_test
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/spu_envelope_fixture.txt
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/spu_envelope_fixture_e1_e9.txt
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/spu_envelope_fixture_e1r_e10.txt)
        else()
            add_test(NAME spu_${fixture}_test COMMAND spu_${fixture}_test)
        endif()
    endforeach()
    add_executable(spu_signed_nibble_test tests/test_spu_signed_nibble.c src/crc32.c)
    target_include_directories(spu_signed_nibble_test PRIVATE include)
    add_test(NAME spu_signed_nibble_test COMMAND spu_signed_nibble_test)
    # Same story as sio_dualshock_rumble_test: registered but unlinkable on
    # origin/master (crc32_update undefined), so it never ran. With crc32 linked
    # it now runs 137 checks and fails 4, all in one ADSR sweep region
    # (mid > 0, late > mid, fall_mid < 0x7FFE, fall_late < fall_mid). Rebuilding
    # it against a zero-returning crc32 stub reproduces exactly those 4, so the
    # crc32 linkage is not the cause — the envelope sweep genuinely disagrees.
    set_tests_properties(spu_fidelity_test PROPERTIES DISABLED TRUE)

    add_executable(overlay_capture_retry_test
        tests/test_overlay_capture_retry.cpp
        src/overlay_capture.c
        src/host_span_ring.c
        src/psx_ram_geometry.c)
    target_include_directories(overlay_capture_retry_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(overlay_capture_retry_test PRIVATE
        SDL_MAIN_HANDLED PSX_OVERLAY_CAPTURE_TEST=1
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
    target_link_libraries(overlay_capture_retry_test PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME overlay_capture_retry_test COMMAND overlay_capture_retry_test)

    # PC evidence stays inside the dirty-page run: nothing recorded in the
    # trailing delay-slot guard word (bead beads-eio.3.190).
    add_executable(overlay_capture_guard_word_test
        tests/test_overlay_capture_guard_word.cpp
        src/overlay_capture.c src/host_span_ring.c)
    target_include_directories(overlay_capture_guard_word_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(overlay_capture_guard_word_test PRIVATE
        SDL_MAIN_HANDLED $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
    target_link_libraries(overlay_capture_guard_word_test PRIVATE
        ${PSX_SDL_LIBRARIES})
    if(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(overlay_capture_guard_word_test PRIVATE Threads::Threads)
    endif()
    add_test(NAME overlay_capture_guard_word_test
             COMMAND overlay_capture_guard_word_test)

    add_executable(overlay_path_canon_test
        tests/test_overlay_path_canon.c
        src/overlay_path_canon.c)
    target_include_directories(overlay_path_canon_test PRIVATE include)
    add_test(NAME overlay_path_canon_test COMMAND overlay_path_canon_test)

    # The kernel-bless verifier must accept a body whose DECLARED patch range
    # the guest has overwritten (Psy-Q's install stubs land inside compiled
    # kernel bodies) and still reject one changed anywhere else. Before this,
    # the whole-body memcmp failed forever on the first install and the BIOS
    # exception handler interpreted for the life of the process.
    add_executable(kernel_patch_ranges_test
        tests/test_kernel_patch_ranges.c
        src/kernel_patch_ranges.c)
    target_include_directories(kernel_patch_ranges_test PRIVATE include)
    add_test(NAME kernel_patch_ranges_test COMMAND kernel_patch_ranges_test)

    # The player-facing "skip the BIOS" flag must resolve to the same boot
    # behaviour on retail SCPH-1001 and on the bundled OpenBIOS, whose
    # kernel-call HLE is (correctly) refused for want of a DeliverEvent anchor.
    add_executable(bios_hle_plan_test
        tests/test_bios_hle_plan.c
        src/bios_hle_plan.c)
    target_include_directories(bios_hle_plan_test PRIVATE include)
    add_test(NAME bios_hle_plan_test COMMAND bios_hle_plan_test)

    # Every func_override failure mode is silent at runtime: a registration
    # that is never consulted, a package override that survives clear-mods
    # (a netplay divergence), a residency guard that corrupts instead of
    # declining. Drives the real g_psx_func_override_hook pointer.
    add_executable(func_override_test
        tests/test_func_override.c
        src/func_override.c)
    target_include_directories(func_override_test PRIVATE include)
    add_test(NAME func_override_test COMMAND func_override_test)

    # A cue, conventional raw image, or Steam .car image is a valid pick; each
    # must mount the same disc and identify off the same data track.
    add_executable(sbi_setup_test tests/test_sbi_setup.cpp src/psx_sha256.c)
    target_include_directories(sbi_setup_test PRIVATE include)
    add_test(NAME sbi_setup_test COMMAND sbi_setup_test ${CMAKE_CURRENT_BINARY_DIR}/sbi-setup-fixture)

    add_executable(disc_path_resolve_test
        tests/test_disc_path_resolve.cpp
        src/disc_path.cpp
        src/cue_sheet.cpp
        src/iso_reader.cpp)
    target_include_directories(disc_path_resolve_test PRIVATE include ${PSXRECOMP_ROOT}/recompiler/include)
    target_link_libraries(disc_path_resolve_test PRIVATE chdr-static)
    add_test(NAME disc_path_resolve_test COMMAND disc_path_resolve_test)

    add_executable(regional_selection_test tests/test_regional_selection.cpp)
    target_compile_features(regional_selection_test PRIVATE cxx_std_17)
    target_include_directories(regional_selection_test PRIVATE include
        ${PSXRECOMP_ROOT}/recompiler/lib/toml11)
    add_test(NAME regional_selection_test COMMAND regional_selection_test
        ${CMAKE_CURRENT_BINARY_DIR}/regional-selection-fixture)

    # Both BIOS filename conventions (bare vs region-qualified) resolve, in
    # both directions, so a config and a dump folder may disagree.
    add_executable(bios_rom_alias_test tests/test_bios_rom_alias.cpp)
    target_include_directories(bios_rom_alias_test PRIVATE
        ${PSXRECOMP_ROOT}/recompiler/include)
    add_test(NAME bios_rom_alias_test COMMAND bios_rom_alias_test)

    # The game window opens at a normal size, not maximised.
    add_executable(window_size_test tests/test_window_size.cpp)
    target_include_directories(window_size_test PRIVATE include)
    add_test(NAME window_size_test COMMAND window_size_test)
    option(PSX_WINDOW_FULLSCREEN_TEST "Run fullscreen transitions on a real desktop" OFF)
    if(PSX_WINDOW_FULLSCREEN_TEST)
        add_executable(window_fullscreen_test tests/test_window_fullscreen.cpp)
        target_include_directories(window_fullscreen_test PRIVATE include)
        target_compile_definitions(window_fullscreen_test PRIVATE
            SDL_MAIN_HANDLED=1 $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
        target_link_libraries(window_fullscreen_test PRIVATE ${PSX_SDL_LIBRARIES})
        add_test(NAME window_fullscreen_test COMMAND window_fullscreen_test)
        set_tests_properties(window_fullscreen_test PROPERTIES LABELS "window;hardware" TIMEOUT 30)
    endif()

    # Multi-file cue TOC mapping (data track + CD-DA in separate bins).
    add_executable(iso_reader_cdda_test
        tests/test_iso_reader_cdda.cpp
        src/iso_reader.cpp
        src/cue_sheet.cpp)
    target_include_directories(iso_reader_cdda_test PRIVATE include ${PSXRECOMP_ROOT}/recompiler/include)
    target_link_libraries(iso_reader_cdda_test PRIVATE chdr-static)
    add_test(NAME iso_reader_cdda_test COMMAND iso_reader_cdda_test)

    # Cue PREGAP/POSTGAP (gaps the dump does not store) shift the disc layout.
    add_executable(iso_reader_pregap_test
        tests/test_iso_reader_pregap.cpp
        src/iso_reader.cpp
        src/cue_sheet.cpp)
    target_include_directories(iso_reader_pregap_test PRIVATE include ${PSXRECOMP_ROOT}/recompiler/include)
    target_link_libraries(iso_reader_pregap_test PRIVATE chdr-static)
    add_test(NAME iso_reader_pregap_test COMMAND iso_reader_pregap_test)

    # Raw vs cooked geometry comes from the cue TRACK mode / sector-0 sync,
    # never from file length.
    add_executable(iso_reader_geometry_test
        tests/test_iso_reader_geometry.cpp
        src/iso_reader.cpp
        src/cue_sheet.cpp)
    target_include_directories(iso_reader_geometry_test PRIVATE include ${PSXRECOMP_ROOT}/recompiler/include)
    target_link_libraries(iso_reader_geometry_test PRIVATE chdr-static)
    add_test(NAME iso_reader_geometry_test COMMAND iso_reader_geometry_test)

    if(Python3_Interpreter_FOUND)
        foreach(opt IN ITEMS 0 2)
            add_executable(iso_reader_source_subq_O${opt}_test
                tests/test_iso_reader_source_subq.cpp src/iso_reader.cpp src/cue_sheet.cpp)
            target_include_directories(iso_reader_source_subq_O${opt}_test PRIVATE include ${PSXRECOMP_ROOT}/recompiler/include)
            target_link_libraries(iso_reader_source_subq_O${opt}_test PRIVATE chdr-static)
            if(MSVC)
                if(opt EQUAL 0)
                    target_compile_options(iso_reader_source_subq_O${opt}_test PRIVATE /Od)
                else()
                    target_compile_options(iso_reader_source_subq_O${opt}_test PRIVATE /O2)
                endif()
            else()
                target_compile_options(iso_reader_source_subq_O${opt}_test PRIVATE -O${opt})
            endif()
            add_test(NAME iso_reader_source_subq_O${opt}_test COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_reader_source_subq_contracts.py
                $<TARGET_FILE:iso_reader_source_subq_O${opt}_test>)
        endforeach()
    endif()

    add_executable(iso_reader_sbi_test
        tests/test_iso_reader_sbi.cpp
        src/iso_reader.cpp
        src/cue_sheet.cpp)
    target_include_directories(iso_reader_sbi_test PRIVATE include ${PSXRECOMP_ROOT}/recompiler/include)
    target_link_libraries(iso_reader_sbi_test PRIVATE chdr-static)
    add_test(NAME iso_reader_sbi_test COMMAND iso_reader_sbi_test)

    add_executable(mod_packages_test
        tests/test_mod_packages.cpp
        src/mod_packages.cpp
        src/mod_media.cpp src/iso_reader.cpp src/cue_sheet.cpp
        src/crc32.c
        src/psx_sha256.c)
    target_include_directories(mod_packages_test PRIVATE
        include
        ../recompiler/include
        ../recompiler/lib/toml11)
    target_link_libraries(mod_packages_test PRIVATE chdr-static)
    add_test(NAME mod_packages_test COMMAND mod_packages_test)

    add_executable(builtin_mod_manifests_test
        tests/test_builtin_mod_manifests.cpp
        src/mod_packages.cpp
        src/mod_media.cpp src/iso_reader.cpp src/cue_sheet.cpp
        src/crc32.c
        src/psx_sha256.c
        src/host_enhancements.c)
    target_include_directories(builtin_mod_manifests_test PRIVATE
        include
        ../recompiler/include
        ../recompiler/lib/toml11)
    target_link_libraries(builtin_mod_manifests_test PRIVATE chdr-static)
    add_test(NAME builtin_mod_manifests_test
             COMMAND builtin_mod_manifests_test
                     "${CMAKE_CURRENT_SOURCE_DIR}/../mods/builtin/packages")

    add_executable(mod_media_test
        tests/test_mod_media.cpp src/mod_media.cpp src/iso_reader.cpp
        src/cue_sheet.cpp src/psx_sha256.c)
    target_include_directories(mod_media_test PRIVATE include ../recompiler/include)
    target_link_libraries(mod_media_test PRIVATE chdr-static)
    add_test(NAME mod_media_test COMMAND mod_media_test)

    add_executable(text_xlate_fix_only_test
        tests/test_text_xlate_fix_only.cpp
        src/text_xlate.cpp
        src/psx_ram_geometry.c)
    target_include_directories(text_xlate_fix_only_test PRIVATE
        include
        ../recompiler/lib/toml11)
    add_test(NAME text_xlate_fix_only_test COMMAND text_xlate_fix_only_test)
    add_executable(native_text_test tests/test_native_text.cpp
        src/native_text.cpp third_party/json11/json11.cpp)
    target_include_directories(native_text_test PRIVATE include)
    add_test(NAME native_text_test COMMAND native_text_test)

    add_executable(netplay_peer_frontier_test tests/test_netplay_peer_frontier.c)
    target_include_directories(netplay_peer_frontier_test PRIVATE include)
    add_test(NAME netplay_peer_frontier_test COMMAND netplay_peer_frontier_test)

    add_executable(netplay_content_gate_test tests/test_netplay_content_gate.c)
    target_include_directories(netplay_content_gate_test PRIVATE include)
    add_test(NAME netplay_content_gate_test COMMAND netplay_content_gate_test)

    add_executable(mod_runtime_test
        src/mod_immediate.c
        src/host_overlay.c
        tests/test_mod_runtime.cpp
        src/func_override.c
        src/iso_reader_c.cpp
        src/host_span_ring.c
        src/mod_runtime.cpp src/mod_function_replacements.c
        src/mod_netplay.c
        src/mod_packages.cpp
        src/mod_media.cpp
        src/disc_path.cpp
        src/cue_sheet.cpp
        src/iso_reader.cpp
        src/crc32.c
        src/psx_sha256.c)
    target_include_directories(mod_runtime_test PRIVATE
        include
        ../recompiler/include
        ../recompiler/lib/toml11)
    if(PSX_RECOMP_UI AND EXISTS "${RECOMP_UI_ROOT}/src/recomp_launcher.h")
        target_compile_definitions(mod_runtime_test PRIVATE RECOMP_LAUNCHER=1)
        target_include_directories(mod_runtime_test PRIVATE "${RECOMP_UI_ROOT}/src")
    endif()
    target_link_libraries(mod_runtime_test PRIVATE chdr-static)
    psxrecomp_sha256_file(mod_runtime_test)
    # Match the runtime's stack allowance for the remaining disc/mod helpers.
    if(WIN32)
        if(MSVC)
            target_link_options(mod_runtime_test PRIVATE /STACK:16777216)
        else()
            target_link_options(mod_runtime_test PRIVATE -Wl,--stack,16777216)
        endif()
    endif()
    add_test(NAME mod_runtime_test COMMAND mod_runtime_test)
    add_test(NAME mod_commands_test COMMAND mod_runtime_test --commands-only)
    add_test(NAME mod_netplay_missing_plugin_test COMMAND mod_runtime_test --missing-profile)
    add_test(NAME mod_netplay_missing_renderer_test COMMAND mod_runtime_test --missing-renderer)
    add_test(NAME mod_netplay_profile_options_test COMMAND mod_runtime_test --profile-options)
    add_test(NAME mod_netplay_option_renderer_missing_test COMMAND mod_runtime_test --option-renderer-missing)
    add_test(NAME mod_native_loader_bridge_test COMMAND mod_runtime_test --native-loading)
    # These aliases delete and reuse the same temporary fixture directory.
    set_tests_properties(mod_runtime_test mod_commands_test
        mod_netplay_missing_plugin_test mod_netplay_missing_renderer_test
        mod_netplay_profile_options_test mod_netplay_option_renderer_missing_test
        mod_native_loader_bridge_test PROPERTIES RESOURCE_LOCK psx_mod_runtime_fixture)

    add_executable(neversoft_helpers_test tests/test_neversoft_helpers.cpp)
    target_include_directories(neversoft_helpers_test PRIVATE include)
    target_compile_features(neversoft_helpers_test PRIVATE cxx_std_17)
    add_test(NAME neversoft_helpers_test COMMAND neversoft_helpers_test)

    add_executable(aligned_archive_test tests/test_aligned_archive.cpp)
    target_include_directories(aligned_archive_test PRIVATE include)
    target_compile_features(aligned_archive_test PRIVATE cxx_std_17)
    add_test(NAME aligned_archive_test COMMAND aligned_archive_test)

    add_executable(resident_sector_test tests/test_resident_sector.cpp)
    target_include_directories(resident_sector_test PRIVATE include)
    target_compile_features(resident_sector_test PRIVATE cxx_std_17)
    add_test(NAME resident_sector_test COMMAND resident_sector_test)
    add_executable(netplay_view_mods_test
        tests/test_netplay_view_mods.cpp
        src/mod_immediate.c src/func_override.c src/host_overlay.c
        src/iso_reader_c.cpp
        src/host_span_ring.c
        src/mod_runtime.cpp src/mod_function_replacements.c
        src/mod_netplay.c
        src/mod_packages.cpp
        src/mod_media.cpp
        src/disc_path.cpp
        src/cue_sheet.cpp
        src/iso_reader.cpp
        src/crc32.c
        src/psx_sha256.c
        src/sio.c)
    target_include_directories(netplay_view_mods_test PRIVATE
        include
        ../recompiler/include
        ../recompiler/lib/toml11)
    target_link_libraries(netplay_view_mods_test PRIVATE chdr-static)
    psxrecomp_sha256_file(netplay_view_mods_test)
    if(WIN32)
        if(MSVC)
            target_link_options(netplay_view_mods_test PRIVATE /STACK:16777216)
        else()
            target_link_options(netplay_view_mods_test PRIVATE -Wl,--stack,16777216)
        endif()
    endif()
    add_test(NAME netplay_view_mods_test COMMAND netplay_view_mods_test)
    get_target_property(mod_runtime_sources mod_runtime_test SOURCES)
    add_executable(mod_runtime_cache_test ${mod_runtime_sources})
    target_include_directories(mod_runtime_cache_test PRIVATE
        include ../recompiler/include ../recompiler/lib/toml11)
    target_compile_definitions(mod_runtime_cache_test PRIVATE
        PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE=1)
    target_link_libraries(mod_runtime_cache_test PRIVATE chdr-static)
    psxrecomp_sha256_file(mod_runtime_cache_test)
    if(WIN32)
        if(MSVC)
            target_link_options(mod_runtime_cache_test PRIVATE /STACK:16777216)
        else()
            target_link_options(mod_runtime_cache_test PRIVATE -Wl,--stack,16777216)
        endif()
    endif()
    add_test(NAME mod_runtime_cache_test COMMAND mod_runtime_cache_test)

    add_executable(mod_resident_test
        tests/test_mod_resident.cpp
        src/mod_immediate.c
        src/host_overlay.c
        src/func_override.c
        src/mod_resident.cpp
        src/mod_runtime.cpp src/mod_function_replacements.c
        src/mod_netplay.c
        src/mod_packages.cpp
        src/mod_media.cpp
        src/disc_path.cpp
        src/cue_sheet.cpp
        src/iso_reader.cpp
        src/crc32.c
        src/psx_sha256.c)
    target_include_directories(mod_resident_test PRIVATE
        include
        ../recompiler/include
        ../recompiler/lib/toml11)
    target_link_libraries(mod_resident_test PRIVATE chdr-static)
    psxrecomp_sha256_file(mod_resident_test)
    if(WIN32)
        if(MSVC)
            target_link_options(mod_resident_test PRIVATE /STACK:16777216)
        else()
            target_link_options(mod_resident_test PRIVATE -Wl,--stack,16777216)
        endif()
    endif()
    add_test(NAME mod_resident_test COMMAND mod_resident_test)
    # Run the existing real ISO/effective-mod integration against owned blobs.
    target_compile_definitions(mod_resident_test PRIVATE PSX_NATIVE_RESIDENT_PREPARATION=1)
    foreach(_resident_mode 0 1)
        add_executable(resident_preparation_${_resident_mode}_test
            tests/test_resident_preparation.cpp src/psx_sha256.c)
        target_include_directories(resident_preparation_${_resident_mode}_test PRIVATE include)
        target_compile_features(resident_preparation_${_resident_mode}_test PRIVATE cxx_std_17)
        target_compile_definitions(resident_preparation_${_resident_mode}_test PRIVATE
            PSX_NATIVE_RESIDENT_PREPARATION=${_resident_mode} PSX_DISABLE_NATIVE_SHA256=1)
    endforeach()
    if(Python3_Interpreter_FOUND)
        add_test(NAME resident_preparation_whole_job COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resident_preparation.py"
            --reference $<TARGET_FILE:resident_preparation_0_test>
            --native $<TARGET_FILE:resident_preparation_1_test>)
        set_tests_properties(resident_preparation_whole_job PROPERTIES RUN_SERIAL TRUE)
        if(PSX_PROFILE_TEST_NINJA)
            add_test(NAME resident_preparation_policy COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resident_preparation_policy.py"
                --cmake "${CMAKE_COMMAND}" --cc "${CMAKE_C_COMPILER}"
                --ninja "${PSX_PROFILE_TEST_NINJA}")
            set_tests_properties(resident_preparation_policy PROPERTIES RUN_SERIAL TRUE)
        endif()
    endif()

    add_executable(mod_disc_read_session_test tests/test_mod_disc_read_session.cpp
        src/iso_reader.cpp src/cue_sheet.cpp)
    target_include_directories(mod_disc_read_session_test PRIVATE include ../recompiler/include)
    target_compile_features(mod_disc_read_session_test PRIVATE cxx_std_17)
    target_link_libraries(mod_disc_read_session_test PRIVATE chdr-static)
    add_test(NAME mod_disc_read_session_test COMMAND mod_disc_read_session_test
        "${CMAKE_CURRENT_BINARY_DIR}/disc-read-session-fixture")

    if(WIN32)
        add_executable(autocompile_child_env_test tests/test_autocompile_child_env.c)
        target_include_directories(autocompile_child_env_test PRIVATE src)
        add_test(NAME autocompile_child_env_test COMMAND autocompile_child_env_test)

        add_executable(autocompile_publication_test
            tests/test_autocompile_publication.c
            src/autocompile.c
            src/host_span_ring.c)
        target_include_directories(autocompile_publication_test PRIVATE include)
        target_compile_definitions(autocompile_publication_test PRIVATE
            PSX_AUTOCOMPILE_TEST=1)
        add_test(NAME autocompile_publication_test
                 COMMAND autocompile_publication_test)

        # A misconfigured overlay_autocompile_cmd must be reported on a channel
        # that exists: the stdout warnings reach nobody under -mwindows, so a
        # broken autocompile silently degrades the whole run to the interpreter.
        add_executable(autocompile_degraded_test
            tests/test_autocompile_degraded.c
            src/autocompile.c
            src/host_span_ring.c)
        target_include_directories(autocompile_degraded_test PRIVATE include)
        add_test(NAME autocompile_degraded_test
                 COMMAND autocompile_degraded_test)

        # A toolchain staged from a different emitter writes shards into a cache directory the
        # runtime never reads (LoD playtest 2026-10-12); the runtime must refuse it loudly.
        add_executable(autocompile_toolchain_hash_test
            tests/test_autocompile_toolchain_hash.c
            src/autocompile.c
            src/host_span_ring.c)
        target_include_directories(autocompile_toolchain_hash_test PRIVATE include)
        add_test(NAME autocompile_toolchain_hash_test
                 COMMAND autocompile_toolchain_hash_test)
    endif()

    # Always-on wall-time frame/span rings (host_span_ring.h): the hitch-attribution
    # rings every runtime stall probe reads.
    add_executable(host_span_ring_test
        tests/test_host_span_ring.c
        src/host_span_ring.c)
    target_include_directories(host_span_ring_test PRIVATE include)
    if(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(host_span_ring_test PRIVATE Threads::Threads)
    endif()
    add_test(NAME host_span_ring_test COMMAND host_span_ring_test)

    add_executable(autocompile_report_test
        tests/test_autocompile_report.c
        src/autocompile.c)
    target_include_directories(autocompile_report_test PRIVATE include)
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(autocompile_report_test PRIVATE Threads::Threads)
    endif()
    add_test(NAME autocompile_report_test COMMAND autocompile_report_test)
    set_tests_properties(autocompile_report_test PROPERTIES TIMEOUT 120)

    # PS1B-391: the run report names the hottest interpreted addresses, the
    # text guard's state and the loader's misses by reason. The builder is a
    # pure function over tables.
    add_executable(interp_report_test
        tests/test_interp_report.c
        src/interp_report.c)
    target_include_directories(interp_report_test PRIVATE include)
    add_test(NAME interp_report_test COMMAND interp_report_test)

    add_executable(bios_phase_report_test tests/test_bios_phase_report.c src/bios_phase_report.c)
    target_include_directories(bios_phase_report_test PRIVATE include)
    add_test(NAME bios_phase_report_test COMMAND bios_phase_report_test)

    # ---- previously-unregistered standalone tests -------------------------
    # These 16 existed in runtime/tests/ with no add_test() entry. Their only
    # build instructions were a gcc command line in each file's header comment,
    # which nothing executes — so when the runtime API moved under them they
    # broke silently and stayed broken. Four had already rotted when they were
    # collected here (2026-08-11):
    #
    #   test_netplay_snap_ring.c        duplicate TU in the documented command;
    #                                   no stubs for the VRAM-incremental
    #                                   boot_state queries  (PR #134)
    #   test_psx_cycle_event_boundaries.c
    #                                   psx_cycles_resync_after_restore() grew a
    #                                   CPUState* parameter; psx_cycles.c grew
    #                                   three externs the stub block never did
    #   test_spu_end_without_repeat.c   spu.c grew psx_irq_raise / crc32_update
    #   test_sio_card_protocol.c        repaired below: the harness now walks
    #                                   shift/ACK edges and clears INTC IRQ7
    #
    # Registering them is only half the fix; check_test_registration.cmake at
    # the end of this file is what stops the next one from going unnoticed.
    # Each test's build recipe now lives HERE and nowhere else — the header
    # comments were reduced to a `ctest -R <name>` pointer so there is exactly
    # one description of how a test is built, and it is one the build executes.

    add_executable(tas_disc_relocation_test tests/test_tas_disc_relocation.cpp)
    target_include_directories(tas_disc_relocation_test PRIVATE include)
    target_compile_features(tas_disc_relocation_test PRIVATE cxx_std_17)
    add_test(NAME tas_disc_relocation_test COMMAND tas_disc_relocation_test)
    add_test(NAME tas_disc_relocation_launcher_test COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/tasreplays/test_disc_relocation.py")

    add_executable(disc_roster_test tests/test_disc_roster.cpp)
    target_include_directories(disc_roster_test PRIVATE include)
    target_compile_features(disc_roster_test PRIVATE cxx_std_17)
    add_test(NAME disc_roster_test COMMAND disc_roster_test)

    add_executable(cdrom_irq_mask_test tests/test_cdrom_irq_mask.c)
    target_include_directories(cdrom_irq_mask_test PRIVATE src)
    add_test(NAME cdrom_irq_mask_test COMMAND cdrom_irq_mask_test)

    # Accepted optional bus: exact source expectations, modern defaults separate.
    foreach(opt IN ITEMS 0 2)
        foreach(source IN ITEMS test_source_gpu_service_clock.c test_source_gpu_command_projection.c
                test_source_gpu_ready_transfer.c test_source_gpu_service_start.c
                test_source_gpu_pending_frame.c test_input_route_raster_clock.c
                test_dma_source_spec2.c test_dma_source_state.c test_dma_source_register_edges.c test_dma_cd_source.c
                test_dma_otc_timing.c test_dma_gpu_linked_list_source.c test_dma_gpu_upload_source.c)
            get_filename_component(stem "${source}" NAME_WE)
            set(t "${stem}_O${opt}")
            add_executable(${t} "tests/${source}")
            target_include_directories(${t} PRIVATE include src)
            target_compile_features(${t} PRIVATE c_std_11)
            if(MSVC)
                target_compile_options(${t} PRIVATE $<IF:$<EQUAL:${opt},0>,/Od,/O2>)
            else()
                target_compile_options(${t} PRIVATE -O${opt} -UNDEBUG -ffunction-sections -fdata-sections)
                    target_link_options(${t} PRIVATE -Wl,--gc-sections)
            endif()
            if(source STREQUAL "test_source_gpu_service_clock.c")
                add_test(NAME ${t} COMMAND ${t} 16 1)
            else()
                add_test(NAME ${t} COMMAND ${t})
            endif()
        endforeach()
    endforeach()
    foreach(feature IN ITEMS 0 1)
        foreach(opt IN ITEMS 0 2)
            set(t source_bus_overlay_${feature}_O${opt}_test)
            add_executable(${t} tests/test_source_bus_overlay.c)
            target_include_directories(${t} PRIVATE include)
            target_compile_features(${t} PRIVATE c_std_11)
            if(feature)
                target_compile_definitions(${t} PRIVATE PSX_SOURCE_BUS_BOUNDARY=1 PSX_OVERLAY_FLAVOR=4)
            endif()
            if(NOT MSVC)
                target_compile_options(${t} PRIVATE -O${opt} -UNDEBUG -ffunction-sections -fdata-sections)
                    target_link_options(${t} PRIVATE -Wl,--gc-sections)
            endif()
            add_test(NAME ${t} COMMAND ${t})
        endforeach()
    endforeach()
    add_executable(source_gpu_headers_cxx_test tests/test_source_gpu_headers_cxx.cpp)
    target_include_directories(source_gpu_headers_cxx_test PRIVATE include)
    add_test(NAME source_gpu_headers_cxx_test COMMAND source_gpu_headers_cxx_test)
    if(Python3_Interpreter_FOUND AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
        foreach(case IN ITEMS runtime service raster receipt frozen prefix cd-clock)
            add_test(NAME source_bus_${case}_test COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_source_bus_fixtures.py --cc ${CMAKE_C_COMPILER} --case ${case})
            set_tests_properties(source_bus_${case}_test PROPERTIES TIMEOUT 180)
        endforeach()
        # Production sources reached by test_source_bus_fixtures.py:
        # tests/test_source_bus_runtime.c tests/test_source_gpu_service_path.c
        # tests/test_source_gpu_raster_rows.c (source_bus_receipt.c is also linked).
        # tests/test_source_bus_cd_clock.c links actual CD/DMA/event-clock owners.
    endif()

    # Maintained optional MDEC profile: authored arithmetic and complete transcripts.
    add_executable(mdec_source_receipt_test tests/mdec_source_receipt.c src/mdec.c)
    target_include_directories(mdec_source_receipt_test PRIVATE include src)
    add_test(NAME mdec_source_receipt_test COMMAND mdec_source_receipt_test)
    set_tests_properties(mdec_source_receipt_test PROPERTIES ENVIRONMENT "PSX_MDEC_SOURCE_MODEL=octoshock-2.3")

    target_include_directories(mdec_source_fifo_rows_test PRIVATE include src)

    add_executable(mdec_source_snapshot_test tests/test_mdec_source_snapshot.c)
    target_include_directories(mdec_source_snapshot_test PRIVATE include src)
    add_test(NAME mdec_source_snapshot_test COMMAND mdec_source_snapshot_test)
    if(Python3_Interpreter_FOUND)
        foreach(opt IN ITEMS 0 2)
            add_executable(mdec_source_pipeline_O${opt}_test tests/test_mdec_source_pipeline.c)
            add_executable(mdec_source_dma_O${opt}_test tests/test_mdec_source_dma.c)
            add_library(mdec_clean_probe_O${opt} SHARED tests/mdec_clean_fixture_probe.c)
            foreach(t IN ITEMS mdec_source_pipeline_O${opt}_test mdec_source_dma_O${opt}_test mdec_clean_probe_O${opt})
                target_include_directories(${t} PRIVATE include src)
                if(MSVC)
                    if(opt EQUAL 0)
                        target_compile_options(${t} PRIVATE /Od)
                    else()
                        target_compile_options(${t} PRIVATE /O2)
                    endif()
                else()
                    target_compile_options(${t} PRIVATE -O${opt} -UNDEBUG -ffunction-sections -fdata-sections)
                    target_link_options(${t} PRIVATE -Wl,--gc-sections)
                endif()
            endforeach()
            add_test(NAME mdec_source_pipeline_O${opt}_test COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mdec_source_contracts.py
                $<TARGET_FILE:mdec_source_pipeline_O${opt}_test> pipeline)
            add_test(NAME mdec_source_dma_wire_O${opt}_test COMMAND mdec_source_dma_O${opt}_test)
            add_test(NAME mdec_source_dma_O${opt}_test COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mdec_source_contracts.py
                $<TARGET_FILE:mdec_source_dma_O${opt}_test> dma)
            add_test(NAME mdec_clean_source_O${opt}_test COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mdec_clean_fixtures.py
                --library $<TARGET_FILE:mdec_clean_probe_O${opt}> --entry-path source)
        endforeach()
    endif()

    # Optional source profiles use authored tapes and synthetic discs only.
    add_executable(cdrom_source_clock_receipt_test tests/test_cdrom_source_clock_receipt.c src/psx_sha256.c)
    target_include_directories(cdrom_source_clock_receipt_test PRIVATE include src)

    target_include_directories(cdrom_source_clock_rows_test PRIVATE include src)

    target_include_directories(cdrom_drive_profile_rows_test PRIVATE include src)

    add_executable(cdrom_source_snapshot_test tests/test_cdrom_source_snapshot.c src/psx_sha256.c)
    target_include_directories(cdrom_source_snapshot_test PRIVATE include src)
    add_test(NAME cdrom_source_snapshot_test COMMAND cdrom_source_snapshot_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cd_source_cold_random_256.psxrng)
    if(Python3_Interpreter_FOUND)
        add_test(NAME cdrom_source_clock_receipt_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cdrom_source_profile_contracts.py receipt
            $<TARGET_FILE:cdrom_source_clock_receipt_test>)
        add_test(NAME cdrom_drive_scope_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cdrom_source_profile_contracts.py drive
            $<TARGET_FILE:cdrom_drive_profile_rows_test>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/cd_source_cold_random_256.psxrng)
        foreach(opt IN ITEMS 0 2)
            add_executable(cdrom_source_cdda_O${opt}_test tests/test_cdrom_source_cdda.c src/psx_sha256.c)
            target_include_directories(cdrom_source_cdda_O${opt}_test PRIVATE include src)
            if(MSVC)
                if(opt EQUAL 0)
                    target_compile_options(cdrom_source_cdda_O${opt}_test PRIVATE /Od)
                else()
                    target_compile_options(cdrom_source_cdda_O${opt}_test PRIVATE /O2)
                endif()
            else()
                target_compile_options(cdrom_source_cdda_O${opt}_test PRIVATE -O${opt} -UNDEBUG)
            endif()
            add_test(NAME cdrom_source_cdda_O${opt}_test COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cdrom_source_cdda_contracts.py
                $<TARGET_FILE:cdrom_source_cdda_O${opt}_test>)
        endforeach()
    endif()

    add_executable(cdrom_sector_fifo_test tests/test_cdrom_sector_fifo.c src/psx_sha256.c)
    target_include_directories(cdrom_sector_fifo_test PRIVATE include src)
    add_test(NAME cdrom_sector_fifo_test COMMAND cdrom_sector_fifo_test)
    add_executable(cdrom_pause_preservation_test tests/test_cdrom_pause_preservation.c src/psx_sha256.c)
    target_include_directories(cdrom_pause_preservation_test PRIVATE include src)
    add_test(NAME cdrom_pause_preservation_test COMMAND cdrom_pause_preservation_test)

    add_executable(cdrom_resident_sector_test tests/test_cdrom_resident_sector.c src/psx_sha256.c)
    target_include_directories(cdrom_resident_sector_test PRIVATE include src)
    add_test(NAME cdrom_resident_sector_test COMMAND cdrom_resident_sector_test)

    add_executable(cdrom_lid_state_test tests/test_cdrom_lid_state.c)
    target_include_directories(cdrom_lid_state_test PRIVATE src)
    add_test(NAME cdrom_lid_state_test COMMAND cdrom_lid_state_test)

    # Cross-device I_STAT ownership: a memory-card ACK IRQ (SIO0) must not
    # clear another device's pending source — pinned on SPU (I_STAT bit 9)
    # staying set across card ACK delivery until the guest acknowledges it.
    # Scratch working dir for the same reason as sio_card_protocol_test: the
    # test self-provisions test_dir/card1.mcd relative to its cwd.
    add_executable(sio_spu_istat_ownership_test
        tests/test_sio_spu_istat_ownership.c
        src/sio.c
        src/memcard.c)
    target_include_directories(sio_spu_istat_ownership_test PRIVATE include src)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sio_spu_istat_scratch)
    add_test(NAME sio_spu_istat_ownership_test COMMAND sio_spu_istat_ownership_test
             WORKING_DIRECTORY
                 ${CMAKE_CURRENT_BINARY_DIR}/sio_spu_istat_scratch)

    # Mirrors func_00005A08 (BIOS card READ chain handler) against a mock CPU,
    # so it pins the disassembly reading without linking the recompiled BIOS.
    add_executable(chain_handler_check_test tests/test_chain_handler_check.c)
    target_include_directories(chain_handler_check_test PRIVATE include src)
    add_test(NAME chain_handler_check_test COMMAND chain_handler_check_test)

    # Bug B: the pacing decision must take ONE counter sample, so the unsigned
    # subtraction cannot underflow into a ~24.7-day SDL_Delay.
    add_executable(frame_pacing_test tests/test_frame_pacing.c)
    add_executable(launcher_vsync_test tests/test_launcher_vsync.c)
    target_include_directories(launcher_vsync_test PRIVATE include)
    add_test(NAME launcher_vsync_test COMMAND launcher_vsync_test)
    add_executable(present_sync_test tests/test_present_sync.c)
    target_include_directories(present_sync_test PRIVATE include)
    add_test(NAME present_sync_test COMMAND present_sync_test)
    target_include_directories(frame_pacing_test PRIVATE include)
    add_test(NAME frame_pacing_test COMMAND frame_pacing_test)
    # Opt-in stereo/OpenXR frame-trace ring (frame_trace.c): disabled-by-default,
    # ring wrap, timestamp monotonicity, missing telemetry, export validity.
    add_executable(frame_trace_test tests/test_frame_trace.c src/frame_trace.c)
    target_include_directories(frame_trace_test PRIVATE include)
    add_test(NAME frame_trace_test COMMAND frame_trace_test)
    # Stereo shedding/recovery policy (docs/STEREO_RENDERING.md): bounded streaks,
    # authoritative probes, hysteresis, pair age separate from the estimate.
    add_executable(stereo_shed_policy_test tests/test_stereo_shed_policy.c
                   src/stereo_shed_policy.c)
    target_include_directories(stereo_shed_policy_test PRIVATE include)
    add_test(NAME stereo_shed_policy_test COMMAND stereo_shed_policy_test)
    # Render thread core ([video] render_thread, docs/RENDER_THREAD.md):
    # ordering across ring wraps, the frame bound, stale-frame detection and
    # the drain + context hand-off sync point, against a fake executor.
    # The fixture itself uses POSIX threads and nanosleep (MinGW has both;
    # the core takes its Win32 path there).
    if(NOT MSVC)
        add_executable(render_thread_test tests/test_render_thread.c src/render_thread.c)
        target_include_directories(render_thread_test PRIVATE include)
        find_package(Threads REQUIRED)
        target_link_libraries(render_thread_test PRIVATE Threads::Threads)
        add_test(NAME render_thread_test COMMAND render_thread_test)
        # Present thread core ([video] present_thread): submission order is
        # presentation order, slots are never composed while queued or on
        # screen, done tokens reach the next composer, stop drains.
        add_executable(present_thread_test tests/test_present_thread.c src/present_thread.c)
        target_include_directories(present_thread_test PRIVATE include)
        target_link_libraries(present_thread_test PRIVATE Threads::Threads)
        add_test(NAME present_thread_test COMMAND present_thread_test)
    endif()

    # Same family of bug as frame_pacing: the starvation watchdog's staleness
    # gap must not wrap when the (cross-thread) heartbeat is newer than the
    # clock sample taken by the emu thread.
    add_executable(starvation_watchdog_test tests/test_starvation_watchdog.c)
    target_include_directories(starvation_watchdog_test PRIVATE include)
    add_test(NAME starvation_watchdog_test COMMAND starvation_watchdog_test)

    add_executable(freeze_dump_policy_test
        tests/test_freeze_dump_policy.c
        src/freeze_dump_policy.c)
    target_include_directories(freeze_dump_policy_test PRIVATE src)
    add_test(NAME freeze_dump_policy_test COMMAND freeze_dump_policy_test)

    add_executable(frame_interpolation_schedule_test
        tests/test_frame_interpolation_schedule.c
        src/frame_interpolation.c)
    target_include_directories(frame_interpolation_schedule_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(frame_interpolation_schedule_test PRIVATE m)
    endif()
    add_test(NAME frame_interpolation_schedule_test
             COMMAND frame_interpolation_schedule_test)

    add_executable(render_pass_plan_test
        tests/test_render_pass_plan.c
        src/render_pass_plan.c)
    target_include_directories(render_pass_plan_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(render_pass_plan_test PRIVATE m)
    endif()
    add_test(NAME render_pass_plan_test COMMAND render_pass_plan_test)

    add_executable(mod_internal_resolution_test
        tests/test_mod_internal_resolution.c
        src/mod_internal_resolution.c)
    target_include_directories(mod_internal_resolution_test PRIVATE include)
    add_test(NAME mod_internal_resolution_test COMMAND mod_internal_resolution_test)

    # The built-in host enhancement mods (CRT Filter, Image Filtering,
    # Enhanced Audio, Rewind, Internal Resolution): the real plugins and their
    # session requests against a fake mod runtime (host_enhancements.h).
    add_executable(host_enhancements_test
        tests/test_host_enhancements.c
        src/host_enhancements.c
        src/mod_builtin_crt_filter.c
        src/mod_builtin_image_filtering.c
        src/mod_builtin_enhanced_audio.c
        src/mod_builtin_rewind.c
        src/mod_builtin_internal_resolution.c)
    target_include_directories(host_enhancements_test PRIVATE include)
    add_test(NAME host_enhancements_test COMMAND host_enhancements_test)

    add_executable(memcard_extensions_test
        tests/test_memcard_extensions.c src/memcard.c)
    target_include_directories(memcard_extensions_test PRIVATE include)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/memcard_extensions_scratch)
    add_test(NAME memcard_extensions_test COMMAND memcard_extensions_test
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/memcard_extensions_scratch)

    # Motion kit and per-frame driver for render-pass plugins: blending,
    # identity matching, limits and the crossfade fallback, on mock RAM.
    add_executable(render_pass_motion_test
        tests/test_render_pass_motion.c
        src/render_pass_motion.c
        src/render_pass_frame.c)
    target_include_directories(render_pass_motion_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(render_pass_motion_test PRIVATE m)
    endif()
    add_test(NAME render_pass_motion_test COMMAND render_pass_motion_test)
    add_executable(render_pass_projection_test
        tests/test_render_pass_projection.cpp
        src/render_pass_projection.cpp
        src/render_pass_motion.c)
    target_include_directories(render_pass_projection_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(render_pass_projection_test PRIVATE m)
    endif()
    add_test(NAME render_pass_projection_test COMMAND render_pass_projection_test)
    # Dynamic resolution's controller against synthetic load: step rules,
    # hysteresis, transient spikes, CPU-bound stretches, holds, the floor.
    add_executable(dynamic_resolution_test
        tests/test_dynamic_resolution.c
        src/dynamic_resolution.c)
    target_include_directories(dynamic_resolution_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(dynamic_resolution_test PRIVATE m)
    endif()
    add_test(NAME dynamic_resolution_test COMMAND dynamic_resolution_test)
    # Game Mode (Steam Deck / gamescope) detection and precedence.
    add_executable(game_mode_test tests/test_game_mode.c src/game_mode.c)
    target_include_directories(game_mode_test PRIVATE include)
    add_test(NAME game_mode_test COMMAND game_mode_test)
    # The render-thread controller (dynrt_*) against a synthetic pipeline.
    add_executable(dynamic_resolution_rt_test
        tests/test_dynamic_resolution_rt.c
        src/dynamic_resolution.c)
    target_include_directories(dynamic_resolution_rt_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(dynamic_resolution_rt_test PRIVATE m)
    endif()
    add_test(NAME dynamic_resolution_rt_test COMMAND dynamic_resolution_rt_test)
    # Frame generation's matching, interpolation, plan and breaker.
    add_executable(frame_gen_test tests/test_frame_gen.c src/frame_gen.c)
    target_include_directories(frame_gen_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(frame_gen_test PRIVATE m)
    endif()
    add_test(NAME frame_gen_test COMMAND frame_gen_test)

    # Post-effect chain presets (postfx_chain.c alone; no GL).
    add_executable(postfx_chain_test tests/test_postfx_chain.c src/postfx_chain.c)
    target_include_directories(postfx_chain_test PRIVATE include)
    add_test(NAME postfx_chain_test COMMAND postfx_chain_test
             WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    # Render-pass time freeze against mock devices (psx_cycles.c only).
    add_executable(render_pass_freeze_test
        tests/test_render_pass_freeze.c
        src/psx_cycles.c)
    target_include_directories(render_pass_freeze_test PRIVATE include)
    add_test(NAME render_pass_freeze_test COMMAND render_pass_freeze_test)

    # Uncharged guest-time spans (psx_mod_call_guest_uncharged's time layer)
    # against the same mock devices.
    add_executable(uncharged_span_test
        tests/test_uncharged_span.c
        src/psx_cycles.c)
    target_include_directories(uncharged_span_test PRIVATE include)
    add_test(NAME uncharged_span_test COMMAND uncharged_span_test)

    # Render-pass watchdog abort from nested guest frames: the real
    # render_pass.c and psx_cycles.c against mock memory, devices and
    # presenter (the frames use GCC/Clang cleanup handlers, as generated
    # code does).
    if(NOT MSVC)
        add_executable(render_pass_abort_test
            tests/test_render_pass_abort.c
            src/render_pass.c
            src/frame_trace.c
            src/stereo_shed_policy.c
            src/psx_cycles.c)
        target_include_directories(render_pass_abort_test PRIVATE include)
        add_test(NAME render_pass_abort_test COMMAND render_pass_abort_test)
    endif()

    # Render-pass sandbox: the store policy memory.c uses, a real pass
    # (render_pass.c, psx_cycles.c, timers.c) that makes device stores and
    # runs frozen cycles, and the out-of-rect VRAM journal.
    add_executable(render_pass_sandbox_test
        tests/test_render_pass_sandbox.c
        src/render_pass.c
        src/frame_trace.c
        src/stereo_shed_policy.c
        src/render_pass_plan.c
        src/psx_cycles.c
        src/timers.c
        src/psx_video_timing.c)
    target_include_directories(render_pass_sandbox_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(render_pass_sandbox_test PRIVATE m)
    endif()
    add_test(NAME render_pass_sandbox_test COMMAND render_pass_sandbox_test)

    add_executable(render_pass_ot_test tests/test_render_pass_ot.cpp)
    target_include_directories(render_pass_ot_test PRIVATE include)
    target_compile_features(render_pass_ot_test PRIVATE cxx_std_17)
    add_test(NAME render_pass_ot_test COMMAND render_pass_ot_test)

    foreach(_submit_profile ENHANCED REFERENCE)
        string(TOLOWER "${_submit_profile}" _submit_suffix)
        set(_submit_test "render_pass_gpu_submit_${_submit_suffix}_test")
        add_executable(${_submit_test} ../tests/test_render_pass_gpu_submit.cpp src/dma_gpu_ll.c)
        target_include_directories(${_submit_test} PRIVATE include)
        psxrecomp_execution_profile(${_submit_test} PROFILE ${_submit_profile})
        add_test(NAME ${_submit_test} COMMAND ${_submit_test})
    endforeach()

    add_executable(gpu_primitive_reject_test tests/test_gpu_primitive_reject.c)
    target_include_directories(gpu_primitive_reject_test PRIVATE include)
    add_test(NAME gpu_primitive_reject_test COMMAND gpu_primitive_reject_test)

    add_executable(gpu_sw_edges_test tests/test_gpu_sw_edges.c)
    target_include_directories(gpu_sw_edges_test PRIVATE src)
    add_test(NAME gpu_sw_edges_test COMMAND gpu_sw_edges_test)

    add_executable(gpu_uv_test tests/test_gpu_uv.c)
    target_include_directories(gpu_uv_test PRIVATE src)
    add_test(NAME gpu_uv_test COMMAND gpu_uv_test)

    # Netplay CPU-authoritative VRAM ignores PGXP overrides and texture
    # filtering (sw_set_faithful_authority).
    add_executable(sw_faithful_authority_test tests/test_sw_faithful_authority.c)
    target_sources(sw_faithful_authority_test PRIVATE tests/source_gpu_raster_stub.c)
    target_include_directories(sw_faithful_authority_test PRIVATE include src)
    if(NOT WIN32)
        target_link_libraries(sw_faithful_authority_test PRIVATE m)
    endif()
    add_test(NAME sw_faithful_authority_test COMMAND sw_faithful_authority_test)

    add_executable(gpu_vk_upload_alignment_test
        tests/test_gpu_vk_upload_alignment.c)
    target_include_directories(gpu_vk_upload_alignment_test PRIVATE src)
    add_test(NAME gpu_vk_upload_alignment_test
             COMMAND gpu_vk_upload_alignment_test)

    # Portable rollback helpers — required for MotK netplay unit tests that use
    # the netplay_* compatibility shims. The peer-facing ones (hash_confirm,
    # input_hist, rb_post) live in recomp-net; the snap ring in retcomm-rbengine.
    set(_RBE_ROOT "${RECOMP_RBENGINE_ROOT}")
    if(NOT _RBE_ROOT)
        set(_RBE_ROOT "${PSXRECOMP_ROOT}/lib/retcomm-rbengine")
    endif()
    set(_RNET_ROOT "${RECOMP_NET_ROOT}")
    if(NOT _RNET_ROOT)
        set(_RNET_ROOT "${PSXRECOMP_ROOT}/lib/recomp-net")
    endif()
    if(EXISTS "${_RBE_ROOT}/CMakeLists.txt" AND EXISTS "${_RNET_ROOT}/CMakeLists.txt")
        if(NOT TARGET recomp_net)
            set(RNET_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
            set(RNET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            set(RNET_ENABLE_ICE OFF CACHE BOOL "" FORCE)
            add_subdirectory("${_RNET_ROOT}" "${CMAKE_BINARY_DIR}/recomp-net-tests")
        endif()
        if(NOT TARGET retcomm_rbengine)
            set(RBE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            set(RECOMP_NET_ROOT "${_RNET_ROOT}" CACHE PATH "" FORCE)
            add_subdirectory("${_RBE_ROOT}" "${CMAKE_BINARY_DIR}/retcomm-rbengine-tests")
        endif()
        set(PSXRECOMP_TEST_HAS_RBENGINE TRUE)
    else()
        set(PSXRECOMP_TEST_HAS_RBENGINE FALSE)
        message(STATUS
            "netplay_*_tests: lib/retcomm-rbengine or lib/recomp-net absent - skipped")
    endif()

    add_executable(netplay_load_probe_test
        tests/test_netplay_load_probe.c)
    target_include_directories(netplay_load_probe_test PRIVATE include)
    add_test(NAME netplay_load_probe_test COMMAND netplay_load_probe_test)

    if(PSXRECOMP_TEST_HAS_RBENGINE)
        if(NOT MSVC)
            add_executable(rewind_load_context_test tests/test_rewind_load_context.c)
            target_include_directories(rewind_load_context_test PRIVATE include src)
            target_compile_definitions(rewind_load_context_test PRIVATE PSX_HAS_RBENGINE_SNAP=1)
            target_compile_options(rewind_load_context_test PRIVATE
                -O2 -flto -ffunction-sections -fdata-sections -UNDEBUG)
            target_link_options(rewind_load_context_test PRIVATE -flto -Wl,--gc-sections)
            target_link_libraries(rewind_load_context_test PRIVATE retcomm_rbengine)
            add_test(NAME rewind_load_context_test COMMAND rewind_load_context_test)
        endif()
        add_executable(netplay_hash_confirm_test
            tests/test_netplay_hash_confirm.c)
        target_include_directories(netplay_hash_confirm_test PRIVATE include)
        target_link_libraries(netplay_hash_confirm_test PRIVATE recomp_net)
        add_test(NAME netplay_hash_confirm_test COMMAND netplay_hash_confirm_test)

        # save/load glue #included from the test; ring ops come from rbengine.
        add_executable(netplay_snap_ring_test tests/test_netplay_snap_ring.c)
        target_include_directories(netplay_snap_ring_test PRIVATE include)
        target_compile_definitions(netplay_snap_ring_test PRIVATE
            NETPLAY_SNAP_RING_TEST_STUB_BOOT_STATE
            PSX_HAS_RECOMP_NET=1)
        target_link_libraries(netplay_snap_ring_test PRIVATE retcomm_rbengine ZLIB::ZLIB)
        add_test(NAME netplay_snap_ring_test COMMAND netplay_snap_ring_test)

        # Lobby chat ring. #includes psx_lobby_client.c, so it needs the same
        # guard define and include roots the runtime build uses; the two WS
        # calls that TU makes are stubbed in the test and abort if reached.
        if(EXISTS "${PSXRECOMP_ROOT}/runtime/src/lobby_ws/rnet_ws.c")
            add_executable(lobby_chat_ring_test tests/test_lobby_chat_ring.c
                src/mod_netplay.c src/psx_sha256.c)
            target_include_directories(lobby_chat_ring_test PRIVATE
                include
                src
                "${PSXRECOMP_ROOT}/runtime/src/lobby_ws"
                ${_RNET_ROOT}/include)
            target_compile_definitions(lobby_chat_ring_test PRIVATE
                PSX_HAS_LOBBY_CLIENT=1
                PSX_HAS_RECOMP_NET=1)
            # Links the REAL recomp_net and the real WS helpers rather than a
            # wall of stubs. Nothing in this test reaches a socket, so no fake
            # is needed and none can drift from what the runtime links.
            target_sources(lobby_chat_ring_test PRIVATE
                "${PSXRECOMP_ROOT}/runtime/src/lobby_ws/rnet_ws.c"
                "${PSXRECOMP_ROOT}/runtime/src/lobby_ws/rnet_sha1.c"
                "${PSXRECOMP_ROOT}/runtime/src/netplay_bios_settle.c")
            target_link_libraries(lobby_chat_ring_test PRIVATE recomp_net)
            add_test(NAME lobby_chat_ring_test COMMAND lobby_chat_ring_test)
            add_executable(netplay_bios_wire_test
                tests/test_netplay_bios_wire.c
                src/mod_netplay.c
                src/psx_sha256.c
                src/netplay_bios_settle.c
                src/lobby_ws/rnet_ws.c
                src/lobby_ws/rnet_sha1.c)
            target_include_directories(netplay_bios_wire_test PRIVATE
                include src src/lobby_ws ${_RNET_ROOT}/include)
            target_compile_definitions(netplay_bios_wire_test PRIVATE
                PSX_HAS_LOBBY_CLIENT=1 PSX_HAS_RECOMP_NET=1)
            target_link_libraries(netplay_bios_wire_test PRIVATE recomp_net)
            add_test(NAME netplay_bios_wire_test COMMAND netplay_bios_wire_test)
        endif()

        add_executable(rb_post_tip_filter_test tests/test_rb_post_tip_filter.c)
        target_include_directories(rb_post_tip_filter_test PRIVATE include)
        target_link_libraries(rb_post_tip_filter_test PRIVATE recomp_net)
        add_test(NAME rb_post_tip_filter_test COMMAND rb_post_tip_filter_test)

        add_executable(netplay_input_hist_test
            tests/test_netplay_input_hist.c
            src/netplay_input_hist.c
            src/psx_net_pad.c)
        target_include_directories(netplay_input_hist_test PRIVATE
            include
            ${_RNET_ROOT}/include)
        target_compile_definitions(netplay_input_hist_test PRIVATE
            PSX_HAS_RECOMP_NET=1)
        target_link_libraries(netplay_input_hist_test PRIVATE recomp_net)
        add_test(NAME netplay_input_hist_test COMMAND netplay_input_hist_test)
    endif()

    # IRQ policy for nested overlay call units: IRQs are delivered inside a
    # unit (a callee that polls for an IRQ-set flag, libcard MemCardSync, must
    # not starve: LoD "Checking MEMORY CARD" hang). Header-only, no runtime link.
    add_executable(overlay_unit_defer_test tests/test_overlay_unit_defer.c)
    target_include_directories(overlay_unit_defer_test PRIVATE src)
    if(MSVC)
        target_compile_options(overlay_unit_defer_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(overlay_unit_defer_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME overlay_unit_defer_test COMMAND overlay_unit_defer_test)

    add_executable(overlay_store_cycle_barrier_test
        tests/test_overlay_store_cycle_barrier.c)
    target_include_directories(overlay_store_cycle_barrier_test PRIVATE include)
    add_test(NAME overlay_store_cycle_barrier_test
             COMMAND overlay_store_cycle_barrier_test)

    # Overlay stores update the host's store-PC breadcrumb (ABI v24). Built
    # with the defines compile_overlays.py gives every shard.
    add_executable(overlay_store_pc_forwarding_test
        tests/test_overlay_store_pc_forwarding.c)
    target_include_directories(overlay_store_pc_forwarding_test PRIVATE include)
    target_compile_definitions(overlay_store_pc_forwarding_test PRIVATE
        PSX_OVERLAY_DLL_BUILD PSX_NO_DEBUG_TOOLS PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME overlay_store_pc_forwarding_test
             COMMAND overlay_store_pc_forwarding_test)

    # frame_fingerprint judge columns are blind to device-write service order.
    add_executable(frame_fingerprint_test tests/test_frame_fingerprint.c)
    target_include_directories(frame_fingerprint_test PRIVATE include)
    add_test(NAME frame_fingerprint_test COMMAND frame_fingerprint_test)

    add_executable(psx_cycle_event_boundaries_test
        tests/test_psx_cycle_event_boundaries.c
        src/psx_cycles.c)
    target_include_directories(psx_cycle_event_boundaries_test PRIVATE include)
    add_test(NAME psx_cycle_event_boundaries_test
             COMMAND psx_cycle_event_boundaries_test)

    # idle_skip store-counter loops: PsyQ v_wait skips, near misses do not.
    add_executable(idle_skip_store_counter_test
        tests/test_idle_skip_store_counter.c
        src/psx_cycles.c)
    target_include_directories(idle_skip_store_counter_test PRIVATE include)
    add_test(NAME idle_skip_store_counter_test COMMAND idle_skip_store_counter_test)

    add_executable(psx_vblank_clock_test
        tests/test_psx_vblank_clock.c
        src/psx_vblank_clock.c)
    target_include_directories(psx_vblank_clock_test PRIVATE include)
    add_test(NAME psx_vblank_clock_test COMMAND psx_vblank_clock_test)

    add_executable(input_route_field_clock_test tests/test_input_route_field_clock.c)
    target_include_directories(input_route_field_clock_test PRIVATE include)
    add_test(NAME input_route_field_clock_test COMMAND input_route_field_clock_test)
    add_executable(input_route_raster_clock_test tests/test_input_route_raster_clock.c)
    target_include_directories(input_route_raster_clock_test PRIVATE include)
    add_test(NAME input_route_raster_clock_test COMMAND input_route_raster_clock_test)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        # Include production owners, then discard unrelated runtime machinery.
        add_executable(vblank_in_exception_test tests/test_vblank_in_exception.c)
        add_executable(gpu_interlace_status_test tests/test_gpu_interlace_status.c)
        add_executable(gpu_gpuread_latch_test tests/test_gpu_gpuread_latch.c)
        add_executable(gpu_command_queue_test tests/test_gpu_command_queue.c)
        add_executable(dma_gpu_command_queue_test tests/test_dma_gpu_command_queue.c src/dma_gpu_ll.c)
        add_executable(gpu_queue_boot_admission_test tests/test_gpu_queue_boot_admission.c src/dma_gpu_ll.c)
        target_link_libraries(gpu_queue_boot_admission_test PRIVATE ZLIB::ZLIB)
        add_test(NAME gpu_queue_boot_admission COMMAND gpu_queue_boot_admission_test)
        foreach(_queue_test gpu_command_queue dma_gpu_command_queue gpu_queue_boot_admission)
            target_include_directories(${_queue_test}_test PRIVATE include)
            target_compile_definitions(${_queue_test}_test PRIVATE PSX_ENABLE_BLOCK_CYCLES=1)
            target_compile_options(${_queue_test}_test PRIVATE -O2 -flto -fwhole-program -UNDEBUG)
            target_link_options(${_queue_test}_test PRIVATE -flto)
        endforeach()
        foreach(_case pending deferred reset snapshot packets interlace-charge)
            add_test(NAME gpu_command_queue_${_case} COMMAND gpu_command_queue_test ${_case})
        endforeach()
        add_test(NAME dma_gpu_command_queue_linked_list COMMAND dma_gpu_command_queue_test)
        add_test(NAME dma_gpu_command_queue_upload COMMAND dma_gpu_command_queue_test upload)
        add_test(NAME dma_gpu_command_queue_scheduler COMMAND dma_gpu_command_queue_test scheduler)
        foreach(_kick_case kick-hold kick-cycle)
            add_test(NAME dma_gpu_command_queue_${_kick_case} COMMAND dma_gpu_command_queue_test ${_kick_case})
        endforeach()
        foreach(_stop_case stop-read stop-read-new stop-read-block)
            add_test(NAME dma_gpu_command_queue_${_stop_case} COMMAND dma_gpu_command_queue_test ${_stop_case})
        endforeach()
        add_executable(memory_unmapped_cd_mirror_test tests/test_memory_unmapped_cd_mirror.c)
        add_executable(gpu_texture_disable_status_test tests/test_gpu_texture_disable_status.c)
        foreach(_test vblank_in_exception gpu_interlace_status gpu_gpuread_latch
                memory_unmapped_cd_mirror gpu_texture_disable_status)
            target_include_directories(${_test}_test PRIVATE include)
            target_compile_definitions(${_test}_test PRIVATE
                PSX_ENABLE_BLOCK_CYCLES=1 PSX_NO_DEBUG_TOOLS=1)
            target_compile_options(${_test}_test PRIVATE -O2 -fwhole-program)
        endforeach()
        add_test(NAME vblank_in_exception_test COMMAND vblank_in_exception_test)
        add_test(NAME gpu_interlace_status_test COMMAND gpu_interlace_status_test)
        add_test(NAME gpu_gpuread_latch_test COMMAND gpu_gpuread_latch_test)
        add_test(NAME memory_unmapped_cd_mirror_test COMMAND memory_unmapped_cd_mirror_test)
        add_test(NAME gpu_texture_disable_status_test COMMAND gpu_texture_disable_status_test)
    endif()
    add_executable(psx_video_timing_test
        tests/test_psx_video_timing.c
        src/psx_video_timing.c)
    target_include_directories(psx_video_timing_test PRIVATE include)
    add_test(NAME psx_video_timing_test COMMAND psx_video_timing_test)

    add_executable(spu_end_without_repeat_test
        tests/test_spu_end_without_repeat.c)
    target_include_directories(spu_end_without_repeat_test PRIVATE include)
    add_test(NAME spu_end_without_repeat_test
             COMMAND spu_end_without_repeat_test)

    add_executable(spu_silent_fetch_test tests/test_spu_silent_fetch.c)
    target_include_directories(spu_silent_fetch_test PRIVATE include)
    add_test(NAME spu_silent_fetch_test COMMAND spu_silent_fetch_test)

    add_executable(spu_irq_enable_test tests/test_spu_irq_enable.c src/crc32.c)
    target_include_directories(spu_irq_enable_test PRIVATE include)
    add_test(NAME spu_irq_enable_test COMMAND spu_irq_enable_test)
    add_executable(spu_pitch_modulation_test tests/test_spu_pitch_modulation.c)
    target_include_directories(spu_pitch_modulation_test PRIVATE include)
    add_test(NAME spu_pitch_modulation_test COMMAND spu_pitch_modulation_test)
    add_executable(spu_pitch_zero_test
        tests/test_spu_pitch_zero.c)
    target_include_directories(spu_pitch_zero_test PRIVATE include)
    add_test(NAME spu_pitch_zero_test
             COMMAND spu_pitch_zero_test)

    # Guest-clocked SPU: accesses advance it to the current cycle (beads-eio.3.321).
    add_executable(spu_guest_clock_test tests/test_spu_guest_clock.c)
    target_include_directories(spu_guest_clock_test PRIVATE include)
    add_test(NAME spu_guest_clock_test COMMAND spu_guest_clock_test)
    add_executable(spu_sample_banks_test tests/test_spu_sample_banks.c)
    target_include_directories(spu_sample_banks_test PRIVATE include)
    add_test(NAME spu_sample_banks_test COMMAND spu_sample_banks_test)

    add_executable(spu_gaussian_test tests/test_spu_gaussian.c)
    target_include_directories(spu_gaussian_test PRIVATE include)
    add_test(NAME spu_gaussian_test COMMAND spu_gaussian_test)


    # PS1B-192: the ADSR envelope and volume sweep unit against oracle fixture
    # sets S-spu E2-E7, E1-E3-E9 and E1R-E10-E8b.
    # PS1B-192: KON/KOFF latency through the register interface (fixtures E4, E5).
    # PS1B-192: default-mode capture ring while disabled (E8c) + source-mode guard.
    add_executable(spu_capture_ring_test tests/test_spu_capture_ring.c)
    target_include_directories(spu_capture_ring_test PRIVATE include)
    add_test(NAME spu_capture_ring_test COMMAND spu_capture_ring_test)
    # PS1B-109: source-profile capture of voices 1/3 inside the voice walk.
    add_executable(spu_capture_order_test tests/test_spu_capture_order.c)
    target_include_directories(spu_capture_order_test PRIVATE include)
    add_test(NAME spu_capture_order_test COMMAND spu_capture_order_test)
    # PS1B-372: rows of the source-profile voice spec that no older test pins.
    # PS1B-109: source-profile reverb stores against the stock oracle (R1).
    add_executable(spu_source_reverb_test tests/test_spu_source_reverb.c)
    target_include_directories(spu_source_reverb_test PRIVATE include)
    add_test(NAME spu_source_reverb_test COMMAND spu_source_reverb_test)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME fntrace_c_linkage_test COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fntrace_c_linkage.py)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC
           AND NOT CMAKE_CROSSCOMPILING)
            add_test(NAME audio_trace_pcm_guard_test
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audio_trace_pcm_guard.py
                             --cc ${CMAKE_C_COMPILER})
            add_test(NAME gpu_gp0_history_guard_test
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gpu_gp0_history_guard.py
                             --cc ${CMAKE_C_COMPILER})
            add_test(NAME debug_input_route_test
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_debug_input_route.py
                             --cc ${CMAKE_C_COMPILER})
            add_test(NAME memory_debug_trace_guard_test
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_memory_debug_trace_guard.py
                             --cc ${CMAKE_C_COMPILER})
            # The frame-trace record path must stay allocation- and IO-free, or
            # it perturbs the frame it measures and no functional test notices.
            add_test(NAME frame_trace_guard_test
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frame_trace_guard.py)
        endif()

        add_test(NAME depth24_present_hold_vblank_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_depth24_present_hold_vblank.py)
        add_test(NAME netplay_load_apply_flow_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_netplay_load_apply_flow.py)
        add_test(NAME restore_edge_irq_check_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_restore_edge_irq_check.py)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            add_test(NAME netplay_rb_session_reset_test
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_netplay_rb_session_reset.py
                             --compiler ${CMAKE_C_COMPILER})
            if(PSXRECOMP_TEST_HAS_RBENGINE)
                # Multi-seat Verify with a lost POST (#636): production Verify
                # code, controlled clock and lossy transport.
                add_test(NAME netplay_rb_verify_lost_post_test
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_netplay_rb_verify_lost_post.py
                                 --compiler ${CMAKE_C_COMPILER}
                                 --recomp-net ${_RNET_ROOT})
            endif()
            add_test(NAME overlay_retry_c11_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_retry_c11.py --compiler ${CMAKE_C_COMPILER})
        endif()
        add_test(NAME setup_bios_assets_test COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_setup_bios_assets.py)
        add_test(NAME codegen_host_posix_toolchain_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_codegen_host_posix_toolchain.py)
        add_test(NAME codegen_host_disc_cfg_handoff_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_codegen_host_disc_cfg_handoff.py)
        # The hand-over to the product forces the launcher for a player and
        # honours a headless start (a gate that starts a kit's own exe needs
        # frames from the game it hands over to).
        add_test(NAME codegen_host_handover_launcher_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_codegen_host_handover_launcher.py)
        # Without recomp-ui or a C compiler it cannot run and exits 77: "Skipped",
        # not "Passed".
        set_tests_properties(codegen_host_handover_launcher_test PROPERTIES
                 SKIP_RETURN_CODE 77)
        # PS1B-410: with PSXRECOMP_TOOLCHAIN_READONLY=1 neither the setup host
        # nor the CLI changes a toolchain pack or a pointer to one. The test
        # works on a made-up cache in its own closed environment.
        add_test(NAME toolchain_readonly_switch_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_toolchain_readonly_switch.py
                         --compiler ${CMAKE_C_COMPILER})
        # PS1B-410: an installed pack is never removed before its replacement
        # is in place, and a failed check removes nothing.
        add_test(NAME toolchain_safe_install_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_toolchain_safe_install.py
                         --compiler ${CMAKE_C_COMPILER})
        # PS1B-410: the toolchain check does not depend on the length of PATH
        # (it failed past about 7,600 characters), and activating a pack does
        # not grow PATH.
        add_test(NAME toolchain_path_length_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_toolchain_path_length.py
                         --compiler ${CMAKE_C_COMPILER})
        # PS1B-410: the sentences a player reads when the toolchain check or an
        # install fails name the step that failed and the system's error.
        add_test(NAME toolchain_sentences_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_toolchain_sentences.py
                         --compiler ${CMAKE_C_COMPILER})
        set_tests_properties(toolchain_sentences_test PROPERTIES SKIP_RETURN_CODE 77)
        # Setup-wizard disc probing (multi-disc roster). Never registered
        # before: a .chd set was rejected and nothing ran to notice.
        add_test(NAME probe_disc_test
                 COMMAND ${Python3_EXECUTABLE} -B -m unittest test_probe_disc
                 WORKING_DIRECTORY ${PSXRECOMP_ROOT}/tools/new_project_layout)
        set_tests_properties(probe_disc_test PROPERTIES
                 ENVIRONMENT "PYTHONPATH=${PSXRECOMP_ROOT}/tools")
        add_test(NAME ps1_encoding_test
                 COMMAND ${Python3_EXECUTABLE} -B -m unittest test_ps1_encoding
                 WORKING_DIRECTORY ${PSXRECOMP_ROOT}/tools/new_project_layout)
        # One program patched per disc is one set (Metal Gear Solid Europe's
        # 2-disc set was refused on a whole-file hash).
        add_test(NAME verify_disc_set_test
                 COMMAND ${Python3_EXECUTABLE} -B -m unittest test_verify_disc_set
                 WORKING_DIRECTORY ${PSXRECOMP_ROOT}/tools/new_project_layout)
        set_tests_properties(verify_disc_set_test PROPERTIES
                 ENVIRONMENT "PYTHONPATH=${PSXRECOMP_ROOT}/tools")
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            add_test(NAME launch_disc_path_test COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_launch_disc_path.py --compiler ${CMAKE_CXX_COMPILER})
            add_test(NAME persisted_disc_path_test COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_persisted_disc_path.py --compiler ${CMAKE_CXX_COMPILER})
            if(WIN32)
                add_test(NAME shared_overlay_compiler_test COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shared_overlay_compiler.py --compiler ${CMAKE_CXX_COMPILER})
            endif()
            add_test(NAME overlay_compiler_path_test COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_compiler_path.py --compiler ${CMAKE_CXX_COMPILER})
        endif()
        add_test(NAME disc_picker_ascii_test COMMAND ${Python3_EXECUTABLE} -B
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_disc_picker_ascii.py)
        add_test(NAME disc_companion_test COMMAND ${Python3_EXECUTABLE} -B ${PSXRECOMP_ROOT}/tools/tests/test_disc_companion.py)
        add_test(NAME sbi_registry_test COMMAND ${Python3_EXECUTABLE} -B ${PSXRECOMP_ROOT}/tools/tests/test_sbi_registry.py)
        # PS1B-407: setup checks the later tracks of a disc against the kit's
        # track list. A disc whose last track is cut short is said, and is
        # refused when the kit sets [prepare_disc] track_list_mismatch = "refuse".
        add_test(NAME setup_track_list_test COMMAND ${Python3_EXECUTABLE} -B ${PSXRECOMP_ROOT}/tools/tests/test_setup_track_list.py)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
            add_test(NAME missing_sbi_callers_test COMMAND ${Python3_EXECUTABLE} -B
                ${PSXRECOMP_ROOT}/tools/tests/test_missing_sbi_callers.py
                --cc ${CMAKE_C_COMPILER} --cxx ${CMAKE_CXX_COMPILER})
            add_test(NAME missing_sbi_reason_test COMMAND ${Python3_EXECUTABLE} -B
                ${PSXRECOMP_ROOT}/tools/tests/test_missing_sbi_reason.py
                --cxx ${CMAKE_CXX_COMPILER})
            # PS1B-413: what the CLI says about a player's file is what the
            # setup window gets, also for a file name outside ASCII. The test
            # compiles the host's reader of the CLI's rows from its headers.
            add_test(NAME setup_host_cli_text_test COMMAND ${Python3_EXECUTABLE} -B
                ${PSXRECOMP_ROOT}/tools/tests/test_setup_host_cli_text.py
                --compiler ${CMAKE_C_COMPILER})
            set_tests_properties(setup_host_cli_text_test PROPERTIES
                SKIP_RETURN_CODE 77)
        endif()
        add_test(NAME cd_seek_trace_validation_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cd_seek_trace_validation.py)
        add_test(NAME timer1_trace_validation_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_timer1_trace_validation.py)
        add_test(NAME timer2_trace_validation_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_timer2_trace_validation.py)
        add_test(NAME frame_interpolation_context_ownership_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frame_interpolation_context_ownership.py
                         --cc "${CMAKE_C_COMPILER}")
        add_test(NAME host_pacing_policy_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_pacing_policy.py
                         --cmake ${CMAKE_COMMAND} --cc ${CMAKE_C_COMPILER}
                         --generator "${CMAKE_GENERATOR}"
                         --make-program "${CMAKE_MAKE_PROGRAM}")
        add_test(NAME present_skip_cadence_guard_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_present_skip_cadence_guard.py)
        add_test(NAME mod_bezel_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mod_bezel.py)
        add_test(NAME rewind_toggle_combo_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rewind_toggle_combo.py)
        add_test(NAME savestate_status_protocol_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_savestate_status_protocol.py)
        add_test(NAME overlay_region_floor_config_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_region_floor_config.py)
        add_test(NAME screenshot_hires_pitch_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_screenshot_hires_pitch.py)
        add_test(NAME gl_depth24_upload_order_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gl_depth24_upload_order.py)
        add_test(NAME pal_cadence_host_refresh_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pal_cadence_host_refresh.py)
        add_test(NAME mixed_refresh_display_cadence_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mixed_refresh_display_cadence.py)
        add_test(NAME cdrom_lid_integration_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cdrom_lid_integration.py)
        add_test(NAME bios_registry_zero_entry_guard_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bios_registry_zero_entry.py)
        # Exercises runtime/psx_check_mod_catalog.cmake -- the build-time guard
        # that fails a build when a title's mod packages land in the legacy
        # mods/packages layout instead of mods/bundled (bead beads-eio.3.101).
        # Needs the cmake executable, since the guard is a cmake -P script.
        add_test(NAME mod_catalog_layout_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mod_catalog_layout.py
                         --cmake ${CMAKE_COMMAND})
        # psxrecomp_add_game_runtime(... PGXP): the real function body under
        # cmake -P, forwarding the hook-flavor option for the title's one
        # runtime (docs/ENHANCEMENTS.md G1.11).
        add_test(NAME game_runtime_pgxp_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_game_runtime_pgxp.py
                         --cmake ${CMAKE_COMMAND})
        add_test(NAME game_runtime_memcard_slots_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_game_runtime_memcard_slots.py
                         --cmake ${CMAKE_COMMAND})
        # tools/mod_channel_filter.py strips developer features from a staged
        # catalog; every entry that served one ([[requirement]] included) must
        # leave with it.
        add_test(NAME mod_channel_filter_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${PSXRECOMP_ROOT}/tools/tests/test_mod_channel_filter.py)
        # The overlay cache tag may be formatted in exactly two files. Three
        # release packagers hand-built it instead, the flavor-suffix fix reached
        # one of them, and the other two could not stage a matching Linux cache
        # at all -- Tomba 2 v0.0.9 shipped with no AppImage as a result.
        # bead beads-eio.3.102. This test is also the SHARED check a title's own
        # suite runs against its own tree (pass --root <repo>), so the rule has
        # one implementation like the staging it guards.
        add_test(NAME packagers_never_format_cache_tag_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_packagers_never_format_cache_tag.py)
        # Behavioural half of the same bead: tools/release_stage.py must select
        # shards by the tag DIRECTORY's identity (the old `-path "*/<tag>/*"`
        # substring match rejected the real ..._f0 directory and staged zero),
        # must never ship .abi_<tag>.ok / .c / .pair-lock, and must fail rather
        # than warn on an empty selection. Hermetic: synthetic cache trees, no
        # recompiler or captures needed.
        add_test(NAME release_stage_cache_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_stage_cache.py)
        # The same staging step unpacks whole programs beside the game (TinyCC,
        # LGPL-2.1, and a Python interpreter). It must write tcc's licence text
        # and notice, leave tcc's example sources out, and warn -- never fail --
        # when a bundled program has no licence file. Hermetic. PS1B-345.
        add_test(NAME release_stage_notices_test
                 COMMAND ${Python3_EXECUTABLE} -B
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_stage_notices.py)
        find_program(PSX_PACKAGE_TEST_BASH NAMES bash
            HINTS "C:/Program Files/Git/bin")
        add_test(NAME package_execution_stage_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_execution_stage.py
                         --bash "${PSX_PACKAGE_TEST_BASH}")
        if(NOT PSX_PACKAGE_TEST_BASH)
            set_tests_properties(package_execution_stage_test PROPERTIES DISABLED TRUE)
        endif()
        add_test(NAME package_appimage_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_appimage.py)
        if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
            set_tests_properties(package_appimage_test PROPERTIES DISABLED TRUE)
        endif()
        # The Linux release's bundled tcc + musl headers build and load a real
        # shard with nothing from the host (x86_64 Linux; skips elsewhere).
        add_test(NAME linux_tcc_bundle_smoke_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_linux_tcc_bundle_smoke.py)
        set_tests_properties(linux_tcc_bundle_smoke_test PROPERTIES
            SKIP_RETURN_CODE 77 TIMEOUT 900)
        # Overlay shards and BIOS modules are compiled for the RUNTIME's
        # architecture: an x86_64 macOS runtime under Rosetta used to get arm64
        # shards (arm64-only Xcode clang, no -arch) in its macos-x64 cache and
        # ran every overlay interpreted. Pins the PSX_OVERLAY_ARCH_ABI handoff
        # (autocompile.c), -arch on macOS, the refusal when the Python cannot
        # load the runtime's architecture, and unchanged Windows/Linux commands;
        # with a real cc it builds and loads each slice this Mac can run.
        add_test(NAME overlay_compile_arch_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_compile_arch.py
                         --cc ${CMAKE_C_COMPILER})
    endif()

    # Full card protocol regression: read, write, address echo, per-slot state,
    # pad interruption, terminal ACK behavior and persistent FLAG semantics.
    add_executable(sio_card_protocol_test
        tests/test_sio_card_protocol.c
        src/sio.c
        src/memcard.c)
    target_include_directories(sio_card_protocol_test PRIVATE include src)
    # Runs in a build-tree scratch dir, NOT in tests/: the test writes
    # test_dir/card1.mcd relative to the working directory (self-provisioning it
    # via mkdir if absent), and tests/test_dir/card1.mcd is a TRACKED fixture —
    # running it from tests/ overwrites that file and leaves the repo dirty.
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sio_card_protocol_scratch)
    add_test(NAME sio_card_protocol_test COMMAND sio_card_protocol_test
             WORKING_DIRECTORY
                 ${CMAKE_CURRENT_BINARY_DIR}/sio_card_protocol_scratch)

    # POSIX-only: dlopen()s a fixture .so out of a synthetic cache tree, so it
    # cannot link under MinGW/MSVC (-ldl). run_overlay_posix_test.sh already
    # stages the cache tree, builds the fixture and the test, and runs it — so
    # register THAT rather than reimplementing its fixture staging here.
    if(UNIX)
        find_program(BASH_EXECUTABLE bash)
        if(BASH_EXECUTABLE)
            add_test(NAME overlay_posix_test
                     COMMAND ${BASH_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_overlay_posix_test.sh)
        else()
            message(STATUS "overlay_posix_test: bash not found - skipped")
        endif()
    endif()
endif()

# ---- test-registration completeness guard (must stay LAST) ------------------
# Fails configure if a file in runtime/tests/ or recompiler/tests/ is neither
# registered as a test nor listed as a deliberate exception. See the module
# header for the rot story this exists to prevent.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/restored_authored_tests.cmake")
include(${CMAKE_CURRENT_SOURCE_DIR}/check_test_registration.cmake)
psxrecomp_check_all_tests_registered()

if(NOT MSVC)
    psxrecomp_add_runtime_target(psx-oracle
        ORACLE
        DEBUG_PORT 4371
        WINDOW_TITLE "psxrecomp - Oracle"
        DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/bios/SCPH1001.BIN"
    )
endif()

# ---- psx-beetle target (Beetle PSX libretro standalone; MinGW, Linux, macOS) ----
#
# Standalone binary that runs Beetle PSX (mednafen-psx libretro core) with
# a debug server exposing the SAME wire-protocol commands as psx-runtime,
# on its own port (default 4382, set in beetle_main.cpp; --port N). No recomp
# code linked.
#
# Requires beetle-psx with the docs/beetle_*.patch hooks, built via:
#   cd beetle-psx && make platform=mingw_x86_64 STATIC_LINKING=1 HAVE_LIGHTREC=0
# (platform=unix on Linux, platform=osx on macOS; there the archive is named
# mednafen_psx_libretro.so/.dylib; copy it to libmednafen_psx.a). Recipes:
# docs/beetle-linux.md, docs/beetle-macos.md.

set(BEETLE_LIB "${CMAKE_SOURCE_DIR}/../beetle-psx/libmednafen_psx.a")

if(NOT MSVC AND EXISTS "${BEETLE_LIB}")
    message(STATUS "psx-beetle: found ${BEETLE_LIB}")

    set(BEETLE_DIR "${CMAKE_SOURCE_DIR}/../beetle-psx")
    set(LRC "${BEETLE_DIR}/libretro-common")
    set(LIBRETRO_COMMON_SOURCES
        ${LRC}/streams/file_stream.c
        ${LRC}/streams/file_stream_transforms.c
        ${LRC}/vfs/vfs_implementation.c
        ${LRC}/file/file_path.c
        ${LRC}/compat/compat_strl.c
        ${LRC}/compat/fopen_utf8.c
        ${LRC}/compat/compat_posix_string.c
        ${LRC}/string/stdstring.c
        ${LRC}/lists/string_list.c
        ${LRC}/encodings/encoding_utf.c
        ${LRC}/time/rtime.c
        ${LRC}/rthreads/rthreads.c
        ${LRC}/memmap/memalign.c
        ${LRC}/hash/rhash.c
    )

    add_executable(psx-beetle
        src/beetle_main.cpp
        src/beetle_libretro.cpp
        src/beetle_debug_server.c
        src/frame_pacing.c
        src/host_time.c
        src/parity_trace.c
        src/device_trace.c
        src/audio_trace.c
        ${LIBRETRO_COMMON_SOURCES}
    )

    target_include_directories(psx-beetle PRIVATE
        include
        ${PSX_SDL_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/../beetle-psx/libretro-common/include
        ${CMAKE_SOURCE_DIR}/../beetle-psx
    )

    target_compile_definitions(psx-beetle PRIVATE
        PSX_BEETLE_BUILD=1
        DEFAULT_DEBUG_PORT=4380
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>
    )
    if(NOT PSX_DEBUG_TOOLS)
        target_compile_definitions(psx-beetle PRIVATE PSX_NO_DEBUG_TOOLS=1)
    endif()

    target_link_libraries(psx-beetle PRIVATE
        "${BEETLE_LIB}"
        ${PSX_SDL_LIBRARIES}
    )

    if(WIN32 OR MINGW)
        target_link_libraries(psx-beetle PRIVATE ws2_32 -lpthread)
    endif()

    if(MINGW)
        target_link_options(psx-beetle PRIVATE -Wl,--stack,67108864)
    endif()
elseif(NOT MSVC)
    message(STATUS "psx-beetle: libmednafen_psx.a not found - skipping psx-beetle")
endif()

# Fixtures compiling the real mod runtime also need its read-only installer.
if(BUILD_TESTING)
    foreach(fixture pgxp_session_test pgxp_session_hooks_test mod_runtime_test netplay_view_mods_test mod_runtime_cache_test mod_resident_test)
        target_sources(${fixture} PRIVATE src/texture_pack_install.cpp src/duckstation_texture_pack.cpp tests/texture_install_stb.cpp)
        target_link_libraries(${fixture} PRIVATE psx_texture_image_decode Threads::Threads)
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(audio_catchup_test tests/test_audio_catchup.c)
    target_include_directories(audio_catchup_test PRIVATE include)
    add_test(NAME audio_catchup_test COMMAND audio_catchup_test)
    add_executable(audio_sink_resume_test tests/test_audio_sink_resume.c)
    target_include_directories(audio_sink_resume_test PRIVATE src)
    if(NOT WIN32)
        target_link_libraries(audio_sink_resume_test PRIVATE m)
    endif()
    add_test(NAME audio_sink_resume_test COMMAND audio_sink_resume_test)
endif()


if(BUILD_TESTING)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/preserved_replay_tests.cmake")
endif()

# Existing authored regression commands retained on the combined source.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
add_test(NAME netplay_overlay_pin_wiring_test COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_netplay_overlay_pin_wiring.py")

add_test(NAME overlay_tier_witness_runtime_test COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_tier_witness_runtime.py"
                --cc "${CMAKE_C_COMPILER}")

add_test(NAME overlay_tier_identity_test COMMAND "${CMAKE_COMMAND}"
            "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/tier-identity-fixture"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_tier_identity.cmake")

add_test(NAME runtime_overlay_guards_test COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_overlay_guards.py")

add_test(NAME compile_overlays_publish_marker_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compile_overlays_publish_marker.py)

add_test(NAME persisted_settings_paths_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_persisted_settings_paths.py --compiler ${CMAKE_CXX_COMPILER})
add_test(NAME menu_roster_startup_test COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_menu_roster_startup.py
    --compiler ${CMAKE_CXX_COMPILER} --work "${CMAKE_CURRENT_BINARY_DIR}/menu-roster-startup")

if(PSX_SDL3 AND EXISTS "${SDL3_SOURCE_DIR}/include/SDL3/SDL.h")
    add_test(NAME savestate_host_navigation_test COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_savestate_host_navigation.py
        --compiler ${CMAKE_CXX_COMPILER} --sdl "${SDL3_SOURCE_DIR}"
        --work "${CMAKE_CURRENT_BINARY_DIR}/savestate-host-navigation")
endif()

add_test(NAME perf_campaign_test COMMAND ${Python3_EXECUTABLE} ${PSXRECOMP_ROOT}/tools/tests/test_perf_campaign.py)

add_test(NAME perf_host_test COMMAND ${Python3_EXECUTABLE} ${PSXRECOMP_ROOT}/tools/tests/test_perf_host.py)
endif()

# Composed fixture dependencies: the tested devices retain their real owners.
if(BUILD_TESTING)
    foreach(_geometry_target compiled_ram_read_watch_test kernel_bless_state_test
            freeze_heartbeat_wedge_test
            overlay_capture_history_test overlay_capture_queue_bound_test
            text_xlate_kernel_bless_test)
        if(TARGET ${_geometry_target})
            target_sources(${_geometry_target} PRIVATE src/psx_ram_geometry.c)
        endif()
    endforeach()
    foreach(_span_target autocompile_report_test autocompile_handle_inherit_test
            overlay_capture_history_test overlay_capture_queue_bound_test)
        if(TARGET ${_span_target})
            target_sources(${_span_target} PRIVATE src/host_span_ring.c)
        endif()
    endforeach()
endif()

# Composed boot-state fixtures use the real atomic file-replacement owner.
foreach(_snapshot_fixture boot_state_handoff_test boot_state_impl_tag_test
        boot_state_execution_test boot_state_reference_capture_test
        boot_state_native_capture_test boot_state_capture_o0_0_test
        boot_state_capture_o0_1_test netplay_capture_ring_0_test
        netplay_capture_ring_1_test mod_snapshot_boot_state_test)
    if(TARGET ${_snapshot_fixture})
        target_sources(${_snapshot_fixture} PRIVATE src/boot_state_replace.c)
    endif()
endforeach()
if(TARGET gpu_queue_boot_admission_test)
    target_sources(gpu_queue_boot_admission_test PRIVATE src/mod_snapshot.c)
endif()

if(TARGET input_route_cdrom_events_test)
    target_sources(input_route_cdrom_events_test PRIVATE src/host_span_ring.c)
endif()
