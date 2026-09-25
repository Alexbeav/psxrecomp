cmake_minimum_required(VERSION 3.20)
project(PSXRecomp C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)

# libchdr is needed by the user-facing `psxrecomp` media CLI and optional
# production disc-reader tests. It arrives via FetchContent + ExternalProject, which is by far the
# heaviest and most fragile thing in this configure — it needs the network on a
# cold cache, and ExternalProject cannot handle a source path containing a colon
# (a repo directory like "Marvel vs. Capcom: Clash of Super Heroes Recomp"
# truncates to "…/Marvel vs. Capcom.rule" and configure dies).
#
# Turning it off lets a caller that only wants an analysis or emitter binary
# configure this project without any of that. Nothing but the media CLI is
# affected apart from the conditional disc-reader tests; emitters and
# psxrecomp-analyze remain chd-free.
option(PSXRECOMP_ENABLE_CHD
       "Build the psxrecomp media CLI (requires libchdr via FetchContent)" ON)

if(PSXRECOMP_ENABLE_CHD)
    # The Python disc tools (psxrecomp_cli.py verify-disc, tools/prepare_disc.py,
    # tools/new_project_layout/probe_disc.py) read .chd through this same
    # decoder via ctypes, so libchdr is also built as a shared library, placed
    # next to the emitters where tools/psx_chd.py looks for it. BUILD_SHARED_LIBS
    # is scoped to that include: libchdr's codecs are declared STATIC and
    # chdr-static (what psxrecomp links) is unaffected; nothing else in this
    # project turns shared.
    set(_psx_saved_build_shared "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS ON)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../runtime/chd_dependency.cmake")
    set(BUILD_SHARED_LIBS "${_psx_saved_build_shared}")
    unset(_psx_saved_build_shared)
    if(TARGET chdr)
        set_target_properties(chdr PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    endif()
endif()

add_subdirectory(lib/fmt)

# Vendored fmt 9.1.0 enables its `consteval` format-string check on Apple
# clang via the `__cpp_consteval` clause in core.h, even though the same
# block tries to exclude Apple toolchains — and Apple clang 21 then rejects
# fmt's own literal format strings ("call to consteval function is not a
# constant expression"). Force FMT_CONSTEVAL empty to take fmt's
# non-consteval compile-time-check path. Scoped to non-MSVC so Windows is
# untouched.
if(NOT MSVC)
    target_compile_definitions(fmt PUBLIC FMT_CONSTEVAL=)
endif()

# Rabbitizer MIPS decoder
add_library(rabbitizer STATIC)
target_sources(rabbitizer PRIVATE
    lib/rabbitizer/cplusplus/src/analysis/LoPairingInfo.cpp
    lib/rabbitizer/cplusplus/src/analysis/RegistersTracker.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstrId.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstrIdType.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionBase.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionCpu.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionR3000GTE.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionR5900.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionRsp.cpp
    lib/rabbitizer/src/analysis/RabbitizerLoPairingInfo.c
    lib/rabbitizer/src/analysis/RabbitizerRegistersTracker.c
    lib/rabbitizer/src/analysis/RabbitizerTrackedRegisterState.c
    lib/rabbitizer/src/common/RabbitizerConfig.c
    lib/rabbitizer/src/common/RabbitizerVersion.c
    lib/rabbitizer/src/common/Utils.c
    lib/rabbitizer/src/instructions/RabbitizerInstrCategory.c
    lib/rabbitizer/src/instructions/RabbitizerInstrDescriptor.c
    lib/rabbitizer/src/instructions/RabbitizerInstrId.c
    lib/rabbitizer/src/instructions/RabbitizerInstrIdType.c
    lib/rabbitizer/src/instructions/RabbitizerInstrSuffix.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionCpu/RabbitizerInstructionCpu_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR3000GTE/RabbitizerInstructionR3000GTE.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR3000GTE/RabbitizerInstructionR3000GTE_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR3000GTE/RabbitizerInstructionR3000GTE_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR5900/RabbitizerInstructionR5900.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR5900/RabbitizerInstructionR5900_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR5900/RabbitizerInstructionR5900_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionRsp/RabbitizerInstructionRsp.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionRsp/RabbitizerInstructionRsp_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionRsp/RabbitizerInstructionRsp_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_Disassemble.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_Examination.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_Operand.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerRegister.c
    lib/rabbitizer/src/instructions/RabbitizerRegisterDescriptor.c
)
target_include_directories(rabbitizer PRIVATE
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/rabbitizer/tables
)

# PSXRecomp — MIPS→C recompiler tool
set(PSXRECOMP_GAME_SOURCES
    src/ps1_exe_parser.cpp
    src/mips_decoder.cpp
    src/control_flow.cpp
    src/function_analysis.cpp
    src/code_generator.cpp
    src/pgxp_hook_emitter.cpp
    src/bios_address_model.cpp
    src/annotations.cpp
    src/config_loader.cpp
    src/recompiler_patch.cpp
    src/main_psx.cpp
)

add_executable(psxrecomp-game ${PSXRECOMP_GAME_SOURCES})
target_include_directories(psxrecomp-game PRIVATE
    include
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
    lib/toml11
    lib/ELFIO
)
target_link_libraries(psxrecomp-game PRIVATE fmt rabbitizer)
if(MINGW)
    target_link_options(psxrecomp-game PRIVATE -Wl,--stack,16777216)
elseif(MSVC)
    target_link_options(psxrecomp-game PRIVATE /STACK:16777216)
endif()

# psxrecomp-toml — PS-X EXE → game.toml generator.
# Standalone tool that analyses a PS-X EXE and generates a complete game.toml
# config with auto-detected fields (load_address, entry_pc, text_size,
# stack_base) and optional JAL-target seed generation.
add_executable(psxrecomp-toml
    src/ps1_exe_parser.cpp
    src/main_toml.cpp
)
target_include_directories(psxrecomp-toml PRIVATE
    include
    lib/fmt/include
)
target_link_libraries(psxrecomp-toml PRIVATE fmt)

# psxrecomp — user-facing media-to-source project generator.
# The only libchdr consumer in this project; see PSXRECOMP_ENABLE_CHD above.
if(PSXRECOMP_ENABLE_CHD)
    add_executable(psxrecomp
        src/main_cli.cpp
        src/cli_boot_path.cpp
        src/ps1_exe_parser.cpp
        ../runtime/src/cue_sheet.cpp
        ../runtime/src/iso_reader.cpp
    )
    target_include_directories(psxrecomp PRIVATE
        include
        ../runtime/include
        lib/fmt/include
    )
    target_link_libraries(psxrecomp PRIVATE fmt chdr-static)
    if(MINGW)
        target_link_options(psxrecomp PRIVATE -Wl,--stack,16777216)
    elseif(MSVC)
        target_link_options(psxrecomp PRIVATE /STACK:16777216)
    endif()
endif()

# psxrecomp-analyze — developer-facing static function discovery.
#
# NOT part of the recompilation path. It turns the boundary/CFG/jump-table
# knowledge the emitters already derive into a queryable artifact set for
# modding and decomp work (see docs/FUNCTION_DISCOVERY.md).
#
# The source list is the guarantee: no runtime/ translation unit appears here
# and none may be added. The tool's whole claim is that everything it reports
# was proven from the executable image alone, with no trace, overlay-capture,
# or executed-PC feedback. Linking the runtime would quietly make that false.
add_executable(psxrecomp-analyze
    src/ps1_exe_parser.cpp
    src/mips_decoder.cpp
    src/function_analysis.cpp
    src/analysis_db.cpp
    src/bios_call_names.cpp
    src/analysis_export.cpp
    src/widescreen_scan.cpp
    src/main_analyze.cpp
)
target_include_directories(psxrecomp-analyze PRIVATE
    include
    src
    ../runtime/include   # ws_cull_detect.h — the shared auto_screen_x detector
    lib/fmt/include
    lib/toml11
)
target_link_libraries(psxrecomp-analyze PRIVATE fmt)
if(MINGW)
    target_link_options(psxrecomp-analyze PRIVATE -Wl,--stack,16777216)
elseif(MSVC)
    target_link_options(psxrecomp-analyze PRIVATE /STACK:16777216)
endif()

# ---- baked codegen hash (stale-binary guard) --------------------------------
# Bake the SAME emitter-source hash the runtime folds into its overlay cache
# tag (runtime/runtime.cmake -> hash_codegen.cmake, shared canonical list in
# runtime/codegen_hash_sources.cmake) into THIS binary, exposed via
# `psxrecomp-game --codegen-hash`. compile_overlays.py compares it against the
# hash it stamps on the cache dir (from --runtime-include) and HARD-FAILS on
# mismatch. Closes the 2026-07-01 Tomba pause-menu wedge class: a stale
# recompiler binary emitting shards that get filed under the CURRENT cg tag
# (read tag == write tag, content stale — invisible to the tag alone).
set(PSXRECOMP_CODEGEN_HASH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include(${CMAKE_CURRENT_SOURCE_DIR}/../runtime/codegen_hash_sources.cmake)
set(_baked_hash_hdr ${CMAKE_CURRENT_BINARY_DIR}/psxrecomp_baked_codegen_hash.h)
# Build-owned, never written into the source tree: runtime.cmake generates the
# same header into its own build dir, and both derive the hash from the shared
# PSXRECOMP_CODEGEN_HASH_SRCS list, so the two copies cannot drift.
set(_runtime_hash_hdr ${CMAKE_CURRENT_BINARY_DIR}/runtime/include/overlay_codegen_hash.h)
add_custom_command(
    OUTPUT  ${_baked_hash_hdr} ${_runtime_hash_hdr}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/runtime/include
    COMMAND ${CMAKE_COMMAND} -DOUT=${_baked_hash_hdr}
            "-DSRCS=${PSXRECOMP_CODEGEN_HASH_SRCS}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/hash_codegen.cmake
    COMMAND ${CMAKE_COMMAND} -DOUT=${_runtime_hash_hdr}
            "-DSRCS=${PSXRECOMP_CODEGEN_HASH_SRCS}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/hash_codegen.cmake
    DEPENDS ${PSXRECOMP_CODEGEN_HASH_SRCS}
            ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/hash_codegen.cmake
    COMMENT "Hashing codegen sources -> baked and runtime hash headers"
    VERBATIM)
# Overlay generation is usable immediately after a tools-only build, before
# configuring any runtime. Both consumers must have the same cache identity.
add_custom_target(psxrecomp_baked_codegen_hash
    DEPENDS ${_baked_hash_hdr} ${_runtime_hash_hdr})
add_dependencies(psxrecomp-game psxrecomp_baked_codegen_hash)
# Force main_psx.cpp to recompile whenever the hash header content changes
# (same OBJECT_DEPENDS hardening as runtime.cmake — __has_include depfile gap).
set_source_files_properties(src/main_psx.cpp PROPERTIES OBJECT_DEPENDS ${_baked_hash_hdr})
target_include_directories(psxrecomp-game PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# NOTE (2026-07-27): the legacy `PSXRecomp` target was REMOVED here.
# It built ${PSXRECOMP_GAME_SOURCES} — byte-for-byte the same source list as
# `psxrecomp-game` above — and was a leftover from before that target was
# introduced. Because Windows and macOS filesystems are case-insensitive, its
# output `PSXRecomp.exe` collided with the user-facing CLI `psxrecomp.exe`
# (the `psxrecomp` target above): both wrote the SAME file on disk and the
# last one linked won. In practice the CLI lost, so `psxrecomp.exe` shipped in
# `psxrecomp-cli-windows-x86_64.zip` (tools/build_cli.py) was actually the game
# recompiler, with a completely different argument grammar than the README
# documents. The bug is invisible on Linux, which is why it survived.
# The `PSXRecomp.exe` users are told to run in README's "Release Package"
# section is a DIFFERENT artifact — the BIOS-only runtime, whose name comes
# from OUTPUT_NAME in runtime/runtime.cmake — and is unaffected by this removal.
# See the collision guard at the bottom of this file.

# psxrecomp-bios — Phase 1a BIOS boot slice recompiler.
# Strict, fail-loud, ingests bios/SCPH1001.BIN as a flat ROM at 0xBFC00000.
# Independent of PSXRecomp's PS1-EXE entry point. Shares mips_decoder +
# ps1_exe_parser sources because mips_decoder.h depends on PS1Executable
# (used only by decode_executable, which psxrecomp-bios does not call).
add_executable(psxrecomp-bios
    src/ps1_exe_parser.cpp
    src/mips_decoder.cpp
    src/strict_translator.cpp
    src/pgxp_hook_emitter.cpp
    src/bios_slice_walker.cpp
    src/bios_address_model.cpp
    src/function_discovery.cpp
    src/full_function_emitter.cpp
    src/config_loader.cpp
    src/recompiler_patch.cpp
    src/main_bios.cpp
)
target_include_directories(psxrecomp-bios PRIVATE
    include
    src
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
    lib/toml11
)
target_link_libraries(psxrecomp-bios PRIVATE fmt rabbitizer)
if(MINGW)
    target_link_options(psxrecomp-bios PRIVATE -Wl,--stack,16777216)
elseif(MSVC)
    target_link_options(psxrecomp-bios PRIVATE /STACK:16777216)
endif()

# Released Windows CLI archives must run without a separate MinGW/MSYS2
# install. Prefer llvm-mingw (+ -static) in CI so emitters do not need
# libgcc/libstdc++ harvested from MSYS2. Keep opt-in for local debug builds.
option(PSXRECOMP_STATIC_CLI "Statically link Windows runtime into CLI tools" OFF)
if(PSXRECOMP_STATIC_CLI AND WIN32 AND (MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set(_cli_targets psxrecomp-game psxrecomp-toml psxrecomp-bios psxrecomp-analyze)
    if(PSXRECOMP_ENABLE_CHD)
        list(APPEND _cli_targets psxrecomp)
    endif()
    if(TARGET chdr)
        # The Python tools load this with ctypes from a plain python.exe, with
        # no toolchain bin/ on PATH, so it must not pull libgcc or libwinpthread.
        list(APPEND _cli_targets chdr)
    endif()
    foreach(_cli_target IN LISTS _cli_targets)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_link_options(${_cli_target} PRIVATE
                -static -static-libgcc -static-libstdc++)
        else()
            # llvm-mingw / Clang: fully static CRT + libc++ / libunwind / pthread.
            target_link_options(${_cli_target} PRIVATE -static)
        endif()
    endforeach()
endif()

# BIOS address-model equivalence test — sweeps every 4-byte address through
# the profile-driven BiosAddressModel and compares against verbatim copies of
# the pre-refactor hardcoded SCPH1001 helpers; also exercises the from_config
# invariants. Run from the framework root: recompiler/build/bios_address_model_test
add_executable(bios_address_model_test
    tests/bios_address_model_test.cpp
    src/bios_address_model.cpp
    src/config_loader.cpp
    src/ps1_exe_parser.cpp
)
target_include_directories(bios_address_model_test PRIVATE
    include
    src
    lib/fmt/include
    lib/toml11
)
target_link_libraries(bios_address_model_test PRIVATE fmt)

# L1 decoder conformance test — compares MipsDecoder against Rabbitizer
# oracle for every instruction in SCPH1001.BIN code regions.
add_executable(l1_decoder_test
    tests/l1_decoder_test.cpp
    src/mips_decoder.cpp
    src/ps1_exe_parser.cpp
)
target_include_directories(l1_decoder_test PRIVATE
    include
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
)
target_link_libraries(l1_decoder_test PRIVATE fmt rabbitizer)

# L2 structural codegen test — verifies StrictTranslator output patterns
# for each MIPS instruction form.
add_executable(l2_structural_test
    tests/l2_structural_test.cpp
    src/mips_decoder.cpp
    src/ps1_exe_parser.cpp
    src/strict_translator.cpp
    src/pgxp_hook_emitter.cpp
)
target_include_directories(l2_structural_test PRIVATE
    include
    src
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
)
target_link_libraries(l2_structural_test PRIVATE fmt rabbitizer)

# Emitter directive-line guard — pins the predicate that keeps same-line
# appenders off preprocessor-directive lines (the PR #171 hazard class).
add_executable(emitter_directive_line_test
    tests/emitter_directive_line_test.cpp
    src/pgxp_hook_emitter.cpp
)
target_include_directories(emitter_directive_line_test PRIVATE
    include
    src
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
)
target_link_libraries(emitter_directive_line_test PRIVATE fmt rabbitizer)

if(BUILD_TESTING)
    # Discover the interpreter before expanding any Python-backed TAS test
    # command. GNU's contract tests require it; non-GNU Python tests remain
    # optional, and BUILD_TESTING=OFF adds no interpreter dependency.
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    else()
        find_package(Python3 COMPONENTS Interpreter)
    endif()

    add_executable(control_flow_metadata_test
        tests/control_flow_metadata_test.cpp
        src/control_flow.cpp)
    target_include_directories(control_flow_metadata_test PRIVATE include)
    target_link_libraries(control_flow_metadata_test PRIVATE fmt)
    add_test(NAME control_flow_metadata_test COMMAND control_flow_metadata_test)
    if(PSXRECOMP_ENABLE_CHD)
        add_executable(disc_identity_test ../runtime/tests/test_disc_identity.cpp
            ../runtime/src/disc_identity.cpp ../runtime/src/disc_path.cpp
            ../runtime/src/iso_reader.cpp ../runtime/src/cue_sheet.cpp
            ../runtime/src/psx_sha256.c ../runtime/src/crc32.c)
        target_include_directories(disc_identity_test PRIVATE ../runtime/include include)
        target_compile_options(disc_identity_test PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
        target_link_libraries(disc_identity_test PRIVATE chdr-static)
        add_test(NAME disc_identity_test COMMAND disc_identity_test)
    endif()
    add_executable(host_path_test tests/host_path_test.cpp)
    target_include_directories(host_path_test PRIVATE include)
    add_test(NAME host_path_test COMMAND host_path_test)

    # PSXRTI3 route format and the record -> release replay round trip (T101).
    # BIOS-free, so they also run here where CI configures this tree.
    add_executable(input_route_v3_file_test ../runtime/tests/test_input_route_v3_file.c)
    target_include_directories(input_route_v3_file_test PRIVATE ../runtime/include)
    add_test(NAME input_route_v3_file_test COMMAND input_route_v3_file_test)
    if(Python3_Interpreter_FOUND)
        foreach(_product IN ITEMS diagnostic release)
            set(_target input_route_session_${_product})
            add_executable(${_target} ../runtime/tests/test_input_route_session.c
                ../runtime/src/input_route_session.c ../runtime/src/psx_sha256.c)
            target_include_directories(${_target} PRIVATE ../runtime/include)
            target_compile_definitions(${_target} PRIVATE
                "PSX_FRAMEWORK_PIN=\"0123456789abcdef0123456789abcdef01234567\""
                $<$<STREQUAL:${_product},release>:PSX_NO_DEBUG_TOOLS=1>)
        endforeach()
        add_test(NAME input_route_session_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_input_route_session.py
            $<TARGET_FILE:input_route_session_diagnostic>
            $<TARGET_FILE:input_route_session_release>)
    endif()

    # The source GPU/SPU headers must stay valid C++ (main.cpp includes them).
    # Compiling succeeds or the build fails; the test only runs the result.
    foreach(_std IN ITEMS 17 20)
        add_executable(source_gpu_headers_cxx${_std}_test
            ../runtime/tests/test_source_gpu_headers_cxx.cpp)
        target_include_directories(source_gpu_headers_cxx${_std}_test PRIVATE ../runtime/include)
        set_target_properties(source_gpu_headers_cxx${_std}_test PROPERTIES
            CXX_STANDARD ${_std} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        # GCC rejects C-only constructs outright; Clang accepts them as extensions.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(source_gpu_headers_cxx${_std}_test PRIVATE -Werror=c11-extensions)
        endif()
        add_test(NAME source_gpu_headers_cxx${_std}_test COMMAND source_gpu_headers_cxx${_std}_test)
    endforeach()

    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        foreach(_mode IN ITEMS O0 O2)
            add_executable(tas_cdrom_subq_position_${_mode}
                ../runtime/tests/test_cdrom_subq_position.c ../runtime/src/psx_sha256.c)
            target_include_directories(tas_cdrom_subq_position_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_cdrom_subq_position_${_mode} PRIVATE -${_mode} -UNDEBUG)
            target_link_libraries(tas_cdrom_subq_position_${_mode} PRIVATE m)
            add_test(NAME tas_cdrom_subq_position_${_mode} COMMAND tas_cdrom_subq_position_${_mode})
            add_executable(tas_cdrom_paused_subq_cycle_${_mode}
                ../runtime/tests/test_cdrom_paused_subq_cycle.c ../runtime/src/psx_sha256.c)
            target_include_directories(tas_cdrom_paused_subq_cycle_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_cdrom_paused_subq_cycle_${_mode} PRIVATE -${_mode} -UNDEBUG)
            target_link_libraries(tas_cdrom_paused_subq_cycle_${_mode} PRIVATE m)
            add_test(NAME tas_cdrom_paused_subq_cycle_${_mode} COMMAND tas_cdrom_paused_subq_cycle_${_mode})
            if(PSXRECOMP_ENABLE_CHD)
                foreach(_disc_test IN ITEMS source_subq sbi)
                    if(_disc_test STREQUAL "source_subq")
                        set(_disc_source ../runtime/tests/test_iso_reader_source_subq.cpp)
                    else()
                        set(_disc_source ../runtime/tests/test_iso_reader_sbi.cpp)
                    endif()
                    add_executable(tas_iso_${_disc_test}_${_mode}
                        ${_disc_source}
                        ../runtime/src/iso_reader.cpp ../runtime/src/cue_sheet.cpp)
                    target_include_directories(tas_iso_${_disc_test}_${_mode} PRIVATE ../runtime/include include)
                    target_compile_options(tas_iso_${_disc_test}_${_mode} PRIVATE -${_mode} -UNDEBUG)
                    target_link_libraries(tas_iso_${_disc_test}_${_mode} PRIVATE chdr-static)
                endforeach()
                add_test(NAME tas_iso_source_subq_${_mode} COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_iso_reader_source_subq_contracts.py
                    $<TARGET_FILE:tas_iso_source_subq_${_mode}>)
                file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sbi-${_mode})
                add_test(NAME tas_iso_sbi_${_mode} COMMAND tas_iso_sbi_${_mode})
                set_tests_properties(tas_iso_sbi_${_mode} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sbi-${_mode})
            endif()
            add_executable(tas_gpu_sprite_blend_${_mode}
                ../runtime/tests/test_source_gpu_sprite_blend.c
                ../runtime/src/gpu_sw_renderer.c ../runtime/tests/source_gpu_raster_stub.c
                ../runtime/src/gpu_vram_dirty.c)
            target_include_directories(tas_gpu_sprite_blend_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_sprite_blend_${_mode} PRIVATE -${_mode} -UNDEBUG)
            target_link_libraries(tas_gpu_sprite_blend_${_mode} PRIVATE m)
            add_test(NAME tas_gpu_sprite_blend_${_mode} COMMAND tas_gpu_sprite_blend_${_mode})
            add_executable(tas_gpu_sprite_fixed_${_mode}
                ../runtime/tests/test_source_gpu_sprite_fixed.c)
            target_include_directories(tas_gpu_sprite_fixed_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_sprite_fixed_${_mode} PRIVATE -${_mode} -UNDEBUG)
            add_test(NAME tas_gpu_sprite_fixed_${_mode} COMMAND tas_gpu_sprite_fixed_${_mode})
            add_executable(tas_gpu_textured_triangle_${_mode}
                ../runtime/tests/test_source_gpu_textured_triangle.c
                ../runtime/src/gpu_sw_renderer.c ../runtime/tests/source_gpu_raster_stub.c
                ../runtime/src/gpu_vram_dirty.c ../runtime/src/psx_sha256.c)
            target_include_directories(tas_gpu_textured_triangle_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_textured_triangle_${_mode} PRIVATE -${_mode} -UNDEBUG)
            target_link_libraries(tas_gpu_textured_triangle_${_mode} PRIVATE m)
            add_test(NAME tas_gpu_textured_triangle_${_mode} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_gpu_textured_triangle_contracts.py
                $<TARGET_FILE:tas_gpu_textured_triangle_${_mode}>)
            add_executable(tas_gpu_polygon_order_${_mode}
                ../runtime/tests/test_source_gpu_polygon_order.c
                ../runtime/src/gpu_sw_renderer.c ../runtime/tests/source_gpu_raster_stub.c
                ../runtime/src/gpu_vram_dirty.c ../runtime/src/psx_sha256.c)
            target_include_directories(tas_gpu_polygon_order_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_polygon_order_${_mode} PRIVATE -${_mode} -UNDEBUG)
            target_link_libraries(tas_gpu_polygon_order_${_mode} PRIVATE m)
            add_test(NAME tas_gpu_polygon_order_${_mode} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_gpu_polygon_order_contracts.py
                $<TARGET_FILE:tas_gpu_polygon_order_${_mode}>)
            add_executable(tas_gpu_shaded_texture_family_${_mode}
                ../runtime/tests/test_source_gpu_shaded_texture_family.c
                ../runtime/src/gpu_sw_renderer.c ../runtime/tests/source_gpu_raster_stub.c
                ../runtime/src/gpu_vram_dirty.c ../runtime/src/psx_sha256.c)
            target_include_directories(tas_gpu_shaded_texture_family_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_shaded_texture_family_${_mode} PRIVATE -${_mode} -UNDEBUG)
            target_link_libraries(tas_gpu_shaded_texture_family_${_mode} PRIVATE m)
            add_test(NAME tas_gpu_shaded_texture_family_${_mode} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_gpu_shaded_texture_family_contracts.py
                $<TARGET_FILE:tas_gpu_shaded_texture_family_${_mode}>)
            add_executable(tas_gpu_shaded_triangle_${_mode}
                ../runtime/tests/test_source_gpu_shaded_triangle.c
                ../runtime/src/gpu_sw_renderer.c ../runtime/tests/source_gpu_raster_stub.c
                ../runtime/src/gpu_vram_dirty.c ../runtime/src/psx_sha256.c)
            target_include_directories(tas_gpu_shaded_triangle_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_shaded_triangle_${_mode} PRIVATE -${_mode} -UNDEBUG)
            target_link_libraries(tas_gpu_shaded_triangle_${_mode} PRIVATE m)
            add_test(NAME tas_gpu_shaded_triangle_${_mode} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_gpu_shaded_triangle_contracts.py
                $<TARGET_FILE:tas_gpu_shaded_triangle_${_mode}>)
            add_executable(tas_gpu_shaded_quad_${_mode}
                ../runtime/tests/test_source_gpu_shaded_quad.c
                ../runtime/src/gpu_sw_renderer.c ../runtime/tests/source_gpu_raster_stub.c
                ../runtime/src/gpu_vram_dirty.c ../runtime/src/psx_sha256.c)
            target_include_directories(tas_gpu_shaded_quad_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_shaded_quad_${_mode} PRIVATE -${_mode} -UNDEBUG)
            target_link_libraries(tas_gpu_shaded_quad_${_mode} PRIVATE m)
            add_test(NAME tas_gpu_shaded_quad_${_mode} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_gpu_shaded_quad_contracts.py
                $<TARGET_FILE:tas_gpu_shaded_quad_${_mode}>)
            add_executable(tas_gpu_polygon_wrap_${_mode}
                ../runtime/tests/test_source_gpu_polygon_wrap.c
                ../runtime/src/gpu_sw_renderer.c ../runtime/tests/source_gpu_raster_stub.c
                ../runtime/src/gpu_vram_dirty.c ../runtime/src/psx_sha256.c)
            target_include_directories(tas_gpu_polygon_wrap_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_polygon_wrap_${_mode} PRIVATE -${_mode} -UNDEBUG)
            target_link_libraries(tas_gpu_polygon_wrap_${_mode} PRIVATE m)
            add_test(NAME tas_gpu_polygon_wrap_${_mode} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_gpu_polygon_wrap_contracts.py
                $<TARGET_FILE:tas_gpu_polygon_wrap_${_mode}>)
            # Disabled (PS1B-167): the harness expects the pre-T172 out-of-range reject;
            # hardware truncates vertex+offset to signed 11-bit (nocash GP0(E5h) note).
            # Rewrite harness + fixtures to the documented rule: follow-up PS1B-171.
            set_tests_properties(tas_gpu_polygon_wrap_${_mode} PROPERTIES DISABLED TRUE)
            add_executable(tas_gpu_flat_blend_${_mode}
                ../runtime/tests/test_source_gpu_flat_blend.c
                ../runtime/src/gpu_sw_renderer.c ../runtime/tests/source_gpu_raster_stub.c
                ../runtime/src/gpu_vram_dirty.c)
            target_include_directories(tas_gpu_flat_blend_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_flat_blend_${_mode} PRIVATE -${_mode} -UNDEBUG)
            target_link_libraries(tas_gpu_flat_blend_${_mode} PRIVATE m)
            add_test(NAME tas_gpu_flat_blend_${_mode} COMMAND tas_gpu_flat_blend_${_mode})
            # PS1B-180: the SCPH550x 480i logo quad draws with field skipping.
            add_executable(tas_gpu_interlace_draw_${_mode}
                ../runtime/tests/test_source_gpu_interlace_draw.c)
            target_include_directories(tas_gpu_interlace_draw_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_interlace_draw_${_mode} PRIVATE -${_mode} -UNDEBUG)
            add_test(NAME tas_gpu_interlace_draw_${_mode} COMMAND tas_gpu_interlace_draw_${_mode})
            # PS1B-180 (c)/(d): GPUSTAT.28 is "write FIFO empty" during A0h/C0h.
            add_executable(tas_gpu_ready_transfer_${_mode}
                ../runtime/tests/test_source_gpu_ready_transfer.c)
            target_include_directories(tas_gpu_ready_transfer_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_ready_transfer_${_mode} PRIVATE -${_mode} -UNDEBUG)
            add_test(NAME tas_gpu_ready_transfer_${_mode} COMMAND tas_gpu_ready_transfer_${_mode})
            # PS1B-182: oracle GPU logs replayed through the timing table. Skips
            # (77) when the logs (lab share, not committed) are absent.
            add_executable(tas_gpu_oracle_replay_${_mode}
                ../runtime/tests/test_source_gpu_oracle_replay.c)
            target_include_directories(tas_gpu_oracle_replay_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_oracle_replay_${_mode} PRIVATE -${_mode} -UNDEBUG)
            add_test(NAME tas_gpu_oracle_replay_${_mode} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_gpu_oracle_replay.py
                $<TARGET_FILE:tas_gpu_oracle_replay_${_mode}>)
            set_tests_properties(tas_gpu_oracle_replay_${_mode} PROPERTIES SKIP_RETURN_CODE 77)
            add_executable(tas_gpu_line_${_mode}
                ../runtime/tests/test_source_gpu_line.c ../runtime/src/gpu_sw_renderer.c ../runtime/tests/source_gpu_raster_stub.c
                ../runtime/src/gpu_vram_dirty.c ../runtime/src/psx_sha256.c)
            target_include_directories(tas_gpu_line_${_mode} PRIVATE ../runtime/include)
            target_compile_options(tas_gpu_line_${_mode} PRIVATE -${_mode} -UNDEBUG)
            target_link_libraries(tas_gpu_line_${_mode} PRIVATE m)
            add_test(NAME tas_gpu_line_${_mode} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_gpu_line_contracts.py
                $<TARGET_FILE:tas_gpu_line_${_mode}>)
        endforeach()
    endif()

    # TAS checkpoint state I/O and the v7 identity dimensions. Both drivers compile their own
    # C at O0 and O2 with -Werror, so they take no CMake target.
    if(Python3_EXECUTABLE AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_test(NAME tas_stateio_manifest COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_tas_stateio.py
            --cc ${CMAKE_C_COMPILER})
        add_test(NAME tas_stateio_identity COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_stateio_identity.py
            --cc ${CMAKE_C_COMPILER})
    endif()

    add_executable(source_cpu_block_bound_test
        ../runtime/tests/test_source_cpu_block_bound.c
        ../runtime/src/psx_icache.c)
    target_include_directories(source_cpu_block_bound_test PRIVATE ../runtime/include)
    target_compile_definitions(source_cpu_block_bound_test PRIVATE PSX_ENABLE_BLOCK_CYCLES=1)
    if(MSVC)
        target_compile_options(source_cpu_block_bound_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(source_cpu_block_bound_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME source_cpu_block_bound_test COMMAND source_cpu_block_bound_test)
    add_test(NAME emitter_directive_line_test
             COMMAND emitter_directive_line_test)
    add_executable(guest_tty_test
        ../runtime/tests/test_guest_tty.c
        ../runtime/src/guest_tty.c)
    target_include_directories(guest_tty_test PRIVATE ../runtime/include)
    add_test(NAME guest_tty_test COMMAND guest_tty_test)

    add_executable(debug_trace_ranges_test
        ../runtime/tests/test_debug_trace_ranges.c
        ../runtime/src/debug_trace_ranges.c
    )
    target_include_directories(debug_trace_ranges_test PRIVATE
        ../runtime/include)
    add_test(NAME debug_trace_ranges_test COMMAND debug_trace_ranges_test)

    add_executable(display_scanout_test
        ../runtime/tests/test_display_scanout.c)
    target_include_directories(display_scanout_test PRIVATE ../runtime/include)
    add_test(NAME display_scanout_test COMMAND display_scanout_test)

    add_executable(cli_boot_path_test
        tests/cli_boot_path_test.cpp
        src/cli_boot_path.cpp
    )
    target_include_directories(cli_boot_path_test PRIVATE src)
    add_test(NAME cli_boot_path_test COMMAND cli_boot_path_test)

    add_executable(ps1_exe_padding_test
        tests/ps1_exe_padding_test.cpp
        src/ps1_exe_parser.cpp
    )
    target_include_directories(ps1_exe_padding_test PRIVATE src include)
    target_link_libraries(ps1_exe_padding_test PRIVATE fmt)
    add_test(NAME ps1_exe_padding_test COMMAND ps1_exe_padding_test)

    add_executable(recompiler_patch_test
        tests/recompiler_patch_test.cpp
        src/ps1_exe_parser.cpp
        src/mips_decoder.cpp
        src/control_flow.cpp
        src/function_analysis.cpp
        src/code_generator.cpp
        src/pgxp_hook_emitter.cpp
        src/annotations.cpp
        src/bios_address_model.cpp
        src/config_loader.cpp
        src/recompiler_patch.cpp
    )
    target_include_directories(recompiler_patch_test PRIVATE
        include
        src
        lib/rabbitizer/include
        lib/rabbitizer/cplusplus/include
        lib/fmt/include
        lib/toml11
        lib/ELFIO
    )
    target_link_libraries(recompiler_patch_test PRIVATE fmt rabbitizer)
    add_test(NAME recompiler_patch_test COMMAND recompiler_patch_test)

    add_executable(full_function_emitter_test
        tests/full_function_emitter_test.cpp
        src/ps1_exe_parser.cpp
        src/mips_decoder.cpp
        src/strict_translator.cpp
        src/pgxp_hook_emitter.cpp
        src/bios_address_model.cpp
        src/function_discovery.cpp
        src/full_function_emitter.cpp
        src/config_loader.cpp
        src/recompiler_patch.cpp
    )
    target_include_directories(full_function_emitter_test PRIVATE
        include
        src
        lib/rabbitizer/include
        lib/rabbitizer/cplusplus/include
        lib/fmt/include
        lib/toml11
    )
    target_link_libraries(full_function_emitter_test PRIVATE fmt rabbitizer)
    add_test(NAME full_function_emitter_test COMMAND full_function_emitter_test)

    add_executable(signed_x_bound_codegen_test
        tests/signed_x_bound_codegen_test.cpp
        src/ps1_exe_parser.cpp
        src/mips_decoder.cpp
        src/control_flow.cpp
        src/function_analysis.cpp
        src/code_generator.cpp
        src/pgxp_hook_emitter.cpp
        src/annotations.cpp
        src/bios_address_model.cpp
        src/config_loader.cpp
        src/recompiler_patch.cpp
    )
    target_include_directories(signed_x_bound_codegen_test PRIVATE
        include
        src
        lib/rabbitizer/include
        lib/rabbitizer/cplusplus/include
        lib/fmt/include
        lib/toml11
        lib/ELFIO
    )
    target_link_libraries(signed_x_bound_codegen_test PRIVATE fmt rabbitizer)
    add_test(NAME signed_x_bound_codegen_test
             COMMAND signed_x_bound_codegen_test)

    add_executable(recompiler_patch_cxx17_header_test
        tests/recompiler_patch_cxx17_header_test.cpp
    )
    target_include_directories(recompiler_patch_cxx17_header_test PRIVATE src)
    set_target_properties(recompiler_patch_cxx17_header_test PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    add_test(NAME recompiler_patch_cxx17_header_test
             COMMAND recompiler_patch_cxx17_header_test)

    # [video] geometry_correction / perspective_texturing opt-in plumbing.
    add_executable(video_enhancement_settings_test
        tests/video_enhancement_settings_test.cpp
        src/config_loader.cpp
        src/ps1_exe_parser.cpp
    )
    target_include_directories(video_enhancement_settings_test PRIVATE
        include
        src
        lib/fmt/include
        lib/toml11
    )
    target_link_libraries(video_enhancement_settings_test PRIVATE fmt)
    add_test(NAME video_enhancement_settings_test
             COMMAND video_enhancement_settings_test)

    # [video] rewind opt-in plumbing (default off + settings round trip).
    add_executable(rewind_settings_test
        tests/rewind_settings_test.cpp
        src/config_loader.cpp
        src/ps1_exe_parser.cpp
    )
    target_include_directories(rewind_settings_test PRIVATE
        include
        src
        lib/fmt/include
        lib/toml11
    )
    target_link_libraries(rewind_settings_test PRIVATE fmt)
    add_test(NAME rewind_settings_test COMMAND rewind_settings_test)

    # Needs bios/SCPH1001.toml — run from the framework root.
    add_test(NAME bios_address_model_test
             COMMAND bios_address_model_test
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # These targets were already built by every test build, but without
    # add_test() entries ctest could neither run nor fail them.
    add_test(NAME l2_structural_test COMMAND l2_structural_test)
    add_test(NAME reachable_discovery_test COMMAND reachable_discovery_test)

    if(Python3_Interpreter_FOUND AND CMAKE_GENERATOR STREQUAL "Ninja")
        add_test(NAME runtime_ipo_configuration
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_ipo.py
                --cmake ${CMAKE_COMMAND} --cc ${CMAKE_C_COMPILER}
                --cxx ${CMAKE_CXX_COMPILER} --ninja ${CMAKE_MAKE_PROGRAM})
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME optional_netplay_auth
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_optional_netplay_auth.py
                --cxx ${CMAKE_CXX_COMPILER})
        add_test(NAME pgo_rebuild_flow
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pgo_rebuild_flow.py)
        add_test(
            NAME cli_project_packaging
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli_project_packaging.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

        # tools/coverage_vault.py's history compaction and streaming JSON
        # reader. Pure unit test — loads the module by path, needs no built
        # target and no generated BIOS. It sat in tools/ with no add_test()
        # entry anywhere, which is exactly the orphan class
        # runtime/check_test_registration.cmake exists to prevent; moving it
        # under tests/ puts it inside that guard's glob.
        add_test(
            NAME coverage_vault
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_coverage_vault.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

        # The runtime exe name must come from CMake, not be re-derived.
        add_test(
            NAME runtime_exe_name
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_exe_name.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

        # These two invoke psxrecomp-game, whose BIOS-profile resolution
        # probes fixed working-directory-relative paths (bios/<name>.toml,
        # then psxrecomp/bios/<name>.toml) -- run them from the framework
        # root so the shipped profiles are found.
        add_test(
            NAME overlay_cross_page_delay_codegen
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_cross_page_delay_codegen.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        # The overlay capture format's trailing delay-slot guard word: legal
        # to READ (a branch at the run's last word emits its slot from it),
        # illegal to ANALYSE (the word after it does not exist). Also pins
        # that the mandatory-delay-slot throw still fires on an untagged
        # image, and that no range manifest claims a byte past the image end.
        # Bead beads-eio.3.100.
        add_test(
            NAME overlay_guard_word_analysis_bound
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_guard_word_analysis_bound.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        # Producer half of the same contract: the capture writer declares its
        # guard bytes and compile_overlays puts them in the PS-EXE tag the
        # recompiler reads.
        add_test(
            NAME overlay_guard_bytes_declaration
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/test_overlay_guard_bytes_declaration.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        if(Python3_VERSION VERSION_GREATER_EQUAL "3.11")
            add_test(
                NAME aot_overlay_methods
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/test_aot_overlay_pipeline.py
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        endif()
        # --static must walk the whole of compile_overlays.main() and exit 0:
        # a DLL-only reconciliation at its tail once read an unbound cache_dir
        # and failed every static run after its output was written.
        add_test(
            NAME compile_overlays_static_tail
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/test_compile_overlays_static_tail.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME fmv_cycle_batch_codegen
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fmv_cycle_batch_codegen.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME kuseg_dispatch_lookup
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_kuseg_dispatch_lookup.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
                    --compiler ${CMAKE_C_COMPILER}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        # Every host edge into an independently registered split piece
        # (fallthrough, branch, jump, jal/jalr continuation, delay slot at the
        # cap) validates live text before native entry. Runs the emitted
        # bodies against mutated RAM; finding PSX-OVL-003.
        add_test(
            NAME split_piece_handoff_guards
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_split_piece_handoff_guards.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
                    --compiler ${CMAKE_C_COMPILER}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        # A shard compiled from resident text patched only at a control-flow
        # word (the 17e9bbaa0 fixture) is published, and the real loader plus
        # memory.c's exact-range text guard still never run stale native code
        # for it. The harness is built with GCC-style flags.
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            add_test(
                NAME overlay_resident_patch_runtime
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_overlay_resident_patch_runtime.py
                        --recompiler $<TARGET_FILE:psxrecomp-game>
                        --compiler ${CMAKE_C_COMPILER}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        endif()
        add_test(
            NAME overlay_shim_compile_contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_shim_compile.py)
        add_test(
            NAME lwlr_load_delay_forward
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lwlr_load_delay_forward.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME zero_run_compaction
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_zero_run_compaction.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME dirty_text_continuation_guards
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_dirty_text_continuation_guards.py)
        add_test(
            NAME dirty_text_admission_guards
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_dirty_text_admission_guards.py)
        add_test(
            NAME bios_backend_constant_codegen
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bios_backend_constant_codegen.py
                    --recompiler $<TARGET_FILE:psxrecomp-bios>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME stall_report
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/test_stall_report.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME prepare_disc_boot_case
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/test_prepare_disc_boot_case.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME package_setup_bios_hint
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/test_package_setup_bios_hint.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME setup_package_payload_filter
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_setup_package_payload_filter.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME setup_private_path_gate
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_setup_private_path_gate.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME app_icon_rc_spaces
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_app_icon_rc_spaces.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME cli_diagnostics
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_cli_diagnostics.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME cli_diagnostic_only
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_cli_diagnostic_only.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME cli_python_floor
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_cli_python_floor.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME codegen_hash_build_tree
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_codegen_hash_build_tree.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME bundle_mingw_dll_preference
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_bundle_mingw_dll_preference.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

        # ---- codegen tests that were written but never registered -----------
        # These existed in recompiler/tests/ with no add_test() entry, so they
        # had no way to run and no way to fail. Each was executed manually
        # before being wired here; all pass. They drive psxrecomp-game, so they
        # run from the framework root for BIOS-profile resolution, exactly like
        # the two above.
        foreach(_t IN ITEMS
                aot_base_recovery
                bios_resident_shard
                msvc_cps_codegen
                no_overlapping_bodies
                overlay_guard_codegen
                overlay_store_barrier_codegen
                reachable_discovery_codegen)
            add_test(
                NAME ${_t}
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${_t}.py
                        --recompiler $<TARGET_FILE:psxrecomp-game>
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        endforeach()

        # test_aot_overlay_discovery.py had no add_test() entry at all until
        # 2026-08-11, which registered it with a note blaming a missed cwd in
        # MOD.compile_fragment_batch ("no BIOS profile found"). That note was
        # already wrong when it was written: compile_fragment_batch has passed
        # cwd since b39387eb0 on 2026-07-13, a month earlier. The failure being
        # observed was two different things, neither of them that.
        #
        # On a Windows host the test prepended its gcc's bin to PATH, which put
        # a second toolchain's libstdc++-6.dll ahead of the one psxrecomp-game
        # was built against; the binary then failed to START (0xC0000139) with
        # empty output, and compile_overlays.py reported that as a binary too
        # old to support --codegen-hash. Separately, subprocess output was
        # decoded with the host ANSI codepage, so any byte the local codepage
        # could not map killed the reader thread (cp1253 here).
        #
        # Both are fixed at source: the test appends its toolchain instead of
        # prepending, and compile_overlays.py decodes as UTF-8 and distinguishes
        # a loader failure from a stale binary. Kept as a comment rather than
        # deleted because the shape recurs: a subprocess that could not run
        # should never be reported as a verdict about its contents, and a
        # known-failure note nobody re-checks will outlive the bug it names.
        add_test(
            NAME aot_overlay_discovery
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_aot_overlay_discovery.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

        # ---- runtime source-invariant guards --------------------------------
        # These live in runtime/tests/ but only READ runtime source, so they
        # need neither a generated BIOS nor a built runtime. Registering them
        # here (as dirty_text_continuation_guards already was) means they run
        # from a plain recompiler build, which is the only tree a newcomer can
        # configure before regenerating a BIOS. Each was run manually first;
        # all pass.
        #
        foreach(_t IN ITEMS
                bios_selection_guards
                capture_history
                cli_retail_bios_profile
                codegen_host_bios_stems
                default_renderer_guards
                deferred_switch_interp_poll
                mod_owned_display_controls
                fmv_quiet_guards
                fps_telemetry_opt_in
                frame_interpolation_display_rate
                gl_swap_self_heal
                hybrid_dev_any_input
                launcher_pad_mode_wiring
                jalr_link_register_guards
                launcher_vulkan_option
                mod_controller_override
                mod_load_acceleration
                overlay_call_unit_irq_guards
                overlay_candidate_capacity
                release_zip
                overlay_decodable_fallback
                overlay_init_guard
                recomp_ui_root_override
                pal_cadence_host_refresh
                savestate_load_preflight
                sdl3_main_single_include
                spu_sample_scheduler_default
                overlay_step_boundary
                vk_build_default
                vk_color_self_barrier
                vk_command_buffer_batching
                vk_draw_area_batch_boundary
                vk_mailbox_policy
                vk_present_timing
                vk_present_wait_stage
                vk_staging_cache)
            add_test(
                NAME ${_t}
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_${_t}.py
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        endforeach()

        # ---- known-failing guards: DISABLED, not de-registered ---------------
        # These three were previously left out of the list above with a comment
        # explaining that they FAIL and that wiring a red check would recreate
        # the problem that took CI off PRs. That reasoning is right; the remedy
        # was not. De-registering is precisely how a test stops being counted,
        # and it is the mechanism that let 16 C/C++ tests rot in runtime/tests/.
        #
        # ctest's DISABLED property is the intended tool: the test stays
        # registered, stays visible in `ctest -N`, and is reported as Disabled
        # rather than silently absent — without turning the run red. Delete the
        # matching set_tests_properties line to re-enable one once it is fixed.
        #
        #   interpreter_perf_guards    psx_devices_mmio_sync's recompute-deadline
        #                              branch never clears g_psx_cycle_fast_limit,
        #                              and the guard still names the pre-rename
        #                              s_next_service_cycle
        #   runtime_perf_diag_guards   asserts a substring no longer present in
        #                              the runtime source
        #   overlay_pair_dedup_runtime needs its companion harness built; not a
        #                              source-only guard
        foreach(_t IN ITEMS
                interpreter_perf_guards
                runtime_perf_diag_guards
                overlay_pair_dedup_runtime)
            add_test(
                NAME ${_t}
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_${_t}.py
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
            set_tests_properties(${_t} PROPERTIES DISABLED TRUE)
        endforeach()
    endif()
endif()

# Reachable main-EXE discovery test: parser-bound validation plus exact-entry
# analysis behavior for direct, indirect, seeded, and out-of-bound targets.
add_executable(reachable_discovery_test
    tests/reachable_discovery_test.cpp
    src/function_analysis.cpp
    src/ps1_exe_parser.cpp
)
target_include_directories(reachable_discovery_test PRIVATE
    include
    lib/fmt/include
)
target_link_libraries(reachable_discovery_test PRIVATE fmt)

# GTE deadline ownership includes pending CPU charges from both host sinks.
if(BUILD_TESTING AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    foreach(_mode IN ITEMS O0 O2)
        set(_target "tas_gte_deferred_${_mode}")
        add_executable(${_target}
            "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_gte_deferred.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/psx_cycles.c")
        target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include")
        target_compile_definitions(${_target} PRIVATE PSX_ENABLE_BLOCK_CYCLES=1)
        target_compile_options(${_target} PRIVATE -${_mode} -UNDEBUG -ffunction-sections -fdata-sections)
        target_link_options(${_target} PRIVATE -Wl,--gc-sections)
        add_test(NAME ${_target} COMMAND ${_target})
    endforeach()
endif()

# ---- test-registration completeness guard -----------------------------------
if(BUILD_TESTING AND Python3_Interpreter_FOUND AND NOT MSVC)
    add_test(NAME gpu_upload_history
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_gpu_upload_history.py"
            --cc ${CMAKE_C_COMPILER})
endif()
# The TAS replay tooling is developer infrastructure and is stripped from
# shipped setup packages, so a source tree may legitimately not have it.
if(BUILD_TESTING AND EXISTS
   "${CMAKE_CURRENT_SOURCE_DIR}/../tools/tasreplays/tests/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../tools/tasreplays/tests"
                     "${CMAKE_CURRENT_BINARY_DIR}/tas-models")
    if(Python3_Interpreter_FOUND)
        # The Python driver compiles test_scheduler_precise_escape.c and
        # test_source_gpu_pending_deadline.c at O0/O2.
        add_test(NAME tas_scheduler_precise_escape
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_scheduler_precise_escape.py"
                --cc ${CMAKE_C_COMPILER})
        add_test(NAME tas_bk2_intake
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/tasreplays/test_bk2_intake.py")
        add_test(NAME tas_replay_recipe
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/tasreplays/test_tekken3.py")
        add_test(NAME tas_pepsiman_admission
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/tasreplays/test_pepsiman.py")
        # The Python driver compiles test_source_ram_page_probe.c at O0/O2.
        add_test(NAME tas_ram_long_route
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_ram_page_probe.py"
                --cc ${CMAKE_C_COMPILER})
        add_test(NAME tas_cpu_long_route
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_cpu_return_probe.py"
                --cc ${CMAKE_C_COMPILER})
        # Both probe headers open their output with C11 fopen "wx". msvcrt
        # (MSYS2 MINGW64) rejects "x", so the probes abort under that CRT: a
        # runtime bug tracked as PSX Work T109, not a test defect. Keep the
        # tests registered and visible as Disabled there; UCRT and glibc run them.
        include(CheckCSourceRuns)
        check_c_source_runs([=[
#include <stdio.h>
int main(void) {
    FILE *f;
    remove("psx_fopen_x_probe.tmp");
    f = fopen("psx_fopen_x_probe.tmp", "wx");
    if (!f) return 1;
    fclose(f);
    remove("psx_fopen_x_probe.tmp");
    return 0;
}
]=] PSX_FOPEN_EXCLUSIVE_MODE)
        if(NOT PSX_FOPEN_EXCLUSIVE_MODE)
            set_tests_properties(tas_ram_long_route tas_cpu_long_route
                PROPERTIES DISABLED TRUE)
        endif()
        # tas_stateio_manifest and tas_stateio_identity are registered with the
        # foundation above. The Python driver compiles test_input_route_raster_wire.c at O0/O2.
        add_test(NAME tas_raster_wire
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_input_route_raster_wire.py"
                --cc ${CMAKE_C_COMPILER})
        # The Python driver links the four comparison-profile section codecs with
        # auto-derived link stubs and round-trips per-field sentinels (O0/O2).
        add_test(NAME tas_section_wire_sentinels
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_boot_state_section_wire.py"
                --cc ${CMAKE_C_COMPILER})
        add_test(NAME tas_cpu_state_wire
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_cpu_state_wire.py"
                --cc ${CMAKE_C_COMPILER})
        add_test(NAME tas_gpu_service_path
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_source_gpu_service_path.py"
                --cc ${CMAKE_C_COMPILER})
        # The Python driver compiles test_boot_state_replace.c with its own TU.
        add_test(NAME tas_state_replace
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_boot_state_replace.py"
                --cc ${CMAKE_C_COMPILER})
    endif()
endif()

# Reads both CMakeLists.txt files as text, so it catches an orphan in either
# project from whichever one is being configured. Placed before the collision
# guard, which must stay last because it needs every target declared; this one
# inspects files on disk and has no such ordering requirement.
include(${CMAKE_CURRENT_SOURCE_DIR}/../runtime/check_test_registration.cmake)
psxrecomp_check_all_tests_registered()

# ---- output-name collision guard (must stay LAST) ---------------------------
# Fails configure if any two targets here would write the same file on a
# case-insensitive filesystem. See the module header for the shipped-bug story.
include(${CMAKE_CURRENT_SOURCE_DIR}/../runtime/check_target_name_collisions.cmake)
psxrecomp_check_output_name_collisions()
