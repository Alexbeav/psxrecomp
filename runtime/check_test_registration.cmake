# Guard against test files that no build system knows about.
#
# WHY THIS EXISTS
# ---------------
# Until 2026-08-11, 16 of the 41 C/C++ tests in runtime/tests/ had no add_test()
# entry anywhere. Their only build instructions were a gcc command line written
# into each file's header comment — and a comment cannot fail. So when the
# runtime API moved under them, they broke and nobody found out:
#
#   * test_netplay_snap_ring.c stopped linking when boot_state grew its
#     VRAM-incremental queries. Its documented recipe ALSO listed a .c file the
#     test already #includes, so the recipe produced duplicate symbols. Both
#     defects sat there until someone tried to run it by hand (PR #134).
#   * test_psx_cycle_event_boundaries.c called psx_cycles_resync_after_restore()
#     after that function grew a CPUState* parameter.
#   * test_spu_end_without_repeat.c never grew stubs for two externs spu.c
#     picked up.
#   * test_sio_card_protocol.c drifted until 147 of its 309 checks failed.
#
# Registering those 16 fixes the instance. It does not fix the class: the class
# is that adding an unregistered test is silent, and silence is the default.
# This check inverts that default. A new file under tests/ must either be
# registered or be listed below with a reason — there is no third option that
# configures successfully.
#
# HOW IT WORKS
# ------------
# The two projects here (recompiler/ and runtime/) are separate project()s with
# separate BUILD_TESTING gates, and runtime/ additionally needs a generated BIOS
# to configure — so there is no single tree in which CMake can see every
# registered target. Rather than force them together (which would make the
# BIOS-free recompiler tree unconfigurable for newcomers), this reads both
# CMakeLists.txt files as TEXT and compares the test files they mention against
# the test files on disk. That works identically from either project, so both
# call it and each independently catches an orphan in the other.
#
# One of the build files it reads — tools/tasreplays/tests/CMakeLists.txt — is a
# developer tree that source packages (game kits) deliberately do not ship, while
# runtime/tests/ IS shipped. Treating that absence as "nothing to read" made the
# guard unsound in exactly those trees: the three tests registered only there
# looked like orphans, and `psxrecomp_cli.py ensure-emitters` could not configure
# a shipped kit at all (2026-09-17, Parasite Eve macOS kit). The registrations
# such a tree cannot see are therefore declared below in
# PSXRECOMP_TESTS_REGISTERED_IN_DEV_TREES, and the declaration is verified in
# both directions whenever the dev tree IS present — so a full checkout still
# fails on a real orphan, on a stale declaration, and on a new dev-only
# registration that nobody declared.
#
# Usage: call at the END of a CMakeLists.txt.
#   include(${CMAKE_CURRENT_SOURCE_DIR}/check_test_registration.cmake)
#   psxrecomp_check_all_tests_registered()

set(_PSXRECOMP_TESTREG_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Files under tests/ that are deliberately not registered as their own test.
# Every entry needs a reason. An entry with no reason is a silent orphan with
# extra steps, which is the thing this file exists to prevent.
set(PSXRECOMP_TESTS_NOT_REGISTERED
    "test_irq_cache_block.c|Private TAS handoff work/check_irq_cache58.py links retained runtime objects and drives the unchanged original-core timer/branch controls; kernel76 and ROM85 variants share this driver"
    "test_irq_recognition.c|Authored copied-BIOS timer IRQ fixture driven by shared tas-replay-research/verify_irq_recognition.py; source-qualified bounded emitted/load/return cases, not general precise continuation coverage"
    "test_dma_completion_deadline.c|Authored actual-scheduler delayed-completion fixture driven by tas-replay-research/verify_dma_completion_deadline.py; O0/O2 deadline/cache/IRQ/timer controls"
    "test_spu_status_scheduler.c|actual cycle/SPU integration with exact production sample-event extraction driven by tas-replay-research/verify_spu_scheduler.py"
    "test_bios_syscall_resume.c|authored generated SYS resume and ordinary JAL control driven by tas-replay-research/verify_bios_syscall_resume.py"
    "test_bios_alias_admission.c|authored generated program plus retained runtime integration driven by tas-replay-research/verify_alias_runtime.py"
    "test_irq_prefetch.c|actual production IRQ poll and authored guest handler linked to retained runtime objects; original-core oracle driven by tas-replay-research/verify_irq_prefetch.py"
    "test_compiled_dma_read_wait.c|authored fixture driven by tas-replay-research/verify_gpu_upload.py, with exact-source vectors and separate O0/O2 runs"
    "test_dma_cd_deadline.c|authored fixture driven by tas-replay-research/verify_cd_dma_deadline.py, with exact-source vectors and separate O0/O2 runs"
    "test_source_gpu_command_projection.c|Standalone scalar projection driven by tas-replay-research/verify_gpu_command_projection.py; retained original-core checkpoints, no automatic scheduler or rendering claim"
    "test_source_gpu_service_clock.c|Standalone event clock driven by tas-replay-research/verify_gpu_service_clock.py; cold authored NTSC service, excludes runtime scheduling and frame-end rescheduling"
    "test_gpu_frame_retirement.c|Shared TAS verify_gpu_frame_retirement.py links actual RAM interpreter/IRQ/device objects; six source cases, tracing off/on. GPU service and generated paths are separate gates"
    "test_cpu_step_boundary.c|Shared TAS verify_cpu_step_generated.py compiles authored output from both emitters against production objects; cold cached/uncached boundaries and callback/trace off/on"
    "test_source_gpu_frame_request.c|Shared TAS verify_gpu_frame_request.py compares two automatic request/return streams against five authored original-core display-range controls"
    "test_source_gpu_pending_frame.c|Shared TAS verify_gpu_pending_frame.py compares source pending quad across automatic frame request/return; supplied CPU steps, standalone service"
    "test_source_gpu_runtime.c|Shared TAS verify_gpu_runtime.py links production scheduler and actual generated/interpreted CPU paths with independent source pending-quad state and tracing off/on"
    "test_source_gpu_dma_runtime.c|Shared TAS verify_gpu_dma_runtime.py checks actual GPU reader, DMA controller and scheduler against retained authored scalar/readback states"
    "test_source_gpu_environment.c|Shared TAS verify_gpu_environment.py compares cold ordinary environment/reset history through the actual GPU/DMA scheduler with original-source scalar states"
    "test_source_gpu_cpu.c|Shared TAS verify_gpu_cpu.py executes authored RAM programs through production dirty-RAM CPU, GPU/DMA service and VRAM observations after a declared synthetic prologue"
    "test_source_gpu_reset_projection.c|Shared TAS verify_gpu_reset_projection.py checks four ordinary original-source reset states at O0/O2 and unsupported draw-state rejection"
    "test_icache_isolated_store.c|authored fixture driven by tas-replay-research/verify_cache_store.py, with exact-source vectors and separate O0/O2 runs"
    "test_input_histogram_start.c|authored fixture driven by tas-replay-research/verify_histogram_start.py, with exact-source vectors and separate O0/O2 runs"
    "test_irq_vector_entry.c|authored fixture driven by tas-replay-research/verify_irq_vector.py, with exact-source vectors and separate O0/O2 runs"
    "test_memory_dma_read_wait.c|authored fixture driven by tas-replay-research/verify_gpu_upload.py, with exact-source vectors and separate O0/O2 runs"
    "test_mmio_read_sample_order.c|authored fixture driven by tas-replay-research/verify_mmio_read_order.py, with exact-source vectors and separate O0/O2 runs"
    "test_muldiv_deferred.c|authored fixture driven by tas-replay-research/verify_muldiv.py, with exact-source vectors and separate O0/O2 runs"
    "test_timer1_raster_integration.c|authored fixture driven by tas-replay-research/verify_timer1.py, with exact-source vectors and separate O0/O2 runs"
    "test_timer1_source_clock.c|authored fixture driven by tas-replay-research/verify_timer1.py, with exact-source vectors and separate O0/O2 runs"
    "test_timer2_divider_native.c|exploratory divider probe documented in tas-replay-research/TIMER2-SOURCE-CLOCK.md; not driven by the portable verify_timer2.py regression"
    "test_timer2_source_clock.c|authored fixture driven by tas-replay-research/verify_timer2.py, with exact-source vectors and separate O0/O2 runs"
    "test_timer2_source_scheduler.c|authored fixture driven by tas-replay-research/verify_timer2.py, with exact-source vectors and separate O0/O2 runs"
    "test_overlay_posix.c|built and run by tests/run_overlay_posix_test.sh, which stages the dlopen fixture tree; that script is registered as overlay_posix_test on UNIX"
)

# Build files that a complete checkout has but a source package (game kit) does
# not ship. Their registrations are invisible to the guard in a shipped tree, so
# the tests they own are declared below instead.
set(PSXRECOMP_TESTREG_DEV_BUILD_FILES
    "tools/tasreplays/tests/CMakeLists.txt"
)

# Tests under runtime/tests/ whose ONLY add_test() lives in one of the dev build
# files above. Format: <filename>|<dev build file>|<reason>.
#
# These are NOT excuses. When the named build file is present the guard checks
# the claim both ways: the entry must really be registered there (no stale
# entries), and every dev-only registration must have an entry (no new kit
# breakage). The declaration is used only when that build file is absent.
set(PSXRECOMP_TESTS_REGISTERED_IN_DEV_TREES
    "test_mdec_nymashock_timing.c|tools/tasreplays/tests/CMakeLists.txt|built at O0 and O2 as tas_mdec_nymashock_timing_<mode>, asserting MDEC timing vectors that come from the TAS replay corpus"
    "test_guest_syscall_exception.c|tools/tasreplays/tests/CMakeLists.txt|built LTO/whole-program as tas_guest_syscall_exception, covering the guest SYSCALL exception path the replay harness depends on"
    "test_cd_read_sample_order.py|tools/tasreplays/tests/CMakeLists.txt|registered as tas_cd_read_sample_order, compiling its own fixture with the configured C compiler passed via --cc"
    "test_sio_pad_query_model.c|tools/tasreplays/tests/CMakeLists.txt|built at O0 and O2 as tas_sio_pad_query_model_<mode>, covering the pad query and config model the DualShock routes replay against"
    "test_sio_checkpoint.c|tools/tasreplays/tests/CMakeLists.txt|built at O0 and O2 as tas_sio_checkpoint_<mode>, covering the SIO section round trip through a TAS checkpoint"
    "test_cdrom_checkpoint.c|tools/tasreplays/tests/CMakeLists.txt|built at O0 and O2 as tas_cdrom_checkpoint_<mode> with runtime/src/psx_sha256.c, covering the CD section round trip through a TAS checkpoint"
    "test_fntrace_checkpoint.py|tools/tasreplays/tests/CMakeLists.txt|registered as tas_fntrace_checkpoint, compiling its C fixture against the actual game-start latch with the configured C compiler passed via --cc"
    "test_fntrace_checkpoint.c|tools/tasreplays/tests/CMakeLists.txt|compiled only by the tas_fntrace_checkpoint driver above, which links it with fntrace.c"
    "test_spu_checkpoint.py|tools/tasreplays/tests/CMakeLists.txt|registered as tas_spu_checkpoint, compiling its C fixture with the configured C compiler passed via --cc"
    "test_spu_checkpoint.c|tools/tasreplays/tests/CMakeLists.txt|compiled only by the tas_spu_checkpoint driver above"
)

# Parse one CMakeLists.txt as TEXT and return the test stems it registers.
function(_psxrecomp_testreg_read_declared _out_var _cml)
    set(_declared "")
    if(EXISTS "${_cml}")
        file(READ "${_cml}" _text)

        # Direct references: tests/test_foo.c, ../runtime/tests/test_foo.py,
        # tests/foo_test.cpp ... Matched narrowly (test_ prefix / _test suffix)
        # so that ordinary sources on the same add_executable line cannot be
        # mistaken for a registration.
        string(REGEX MATCHALL "test_[A-Za-z0-9_]+\\.(c|cpp|py)" _hits "${_text}")
        string(REGEX MATCHALL "[A-Za-z0-9_]+_test\\.(c|cpp)" _hits2 "${_text}")
        foreach(_h IN LISTS _hits _hits2)
            string(REGEX REPLACE "\\.(c|cpp|py)$" "" _stem "${_h}")
            list(APPEND _declared "${_stem}")
        endforeach()

        # foreach(_t IN ITEMS a b c) blocks register test_<item>.py. Match only
        # up to the first ')' so the list items are captured but the add_test()
        # body inside the loop is not.
        string(REGEX MATCHALL "foreach\\(_t IN ITEMS[^)]*\\)" _loops "${_text}")
        foreach(_loop IN LISTS _loops)
            string(REGEX REPLACE "foreach\\(_t IN ITEMS" "" _loop "${_loop}")
            string(REGEX MATCHALL "[A-Za-z0-9_]+" _items "${_loop}")
            foreach(_i IN LISTS _items)
                list(APPEND _declared "test_${_i}")
            endforeach()
        endforeach()

        list(REMOVE_DUPLICATES _declared)
    endif()
    set(${_out_var} "${_declared}" PARENT_SCOPE)
endfunction()

function(psxrecomp_check_all_tests_registered)
    set(_root "${_PSXRECOMP_TESTREG_DIR}/..")

    # ---- what the build systems say -------------------------------------
    set(_declared "")
    foreach(_cml "${_root}/runtime/CMakeLists.txt"
                 "${_root}/recompiler/CMakeLists.txt")
        _psxrecomp_testreg_read_declared(_d "${_cml}")
        list(APPEND _declared ${_d})
    endforeach()
    set(_core_declared ${_declared})

    # Dev-only build files. Present: parse them like any other registrar, and
    # cross-check PSXRECOMP_TESTS_REGISTERED_IN_DEV_TREES against what they
    # actually register. Absent (a shipped source package): honour the declared
    # entries, because the registration does exist — this tree cannot see it.
    set(_dev_missing "")
    foreach(_rel IN LISTS PSXRECOMP_TESTREG_DEV_BUILD_FILES)
        string(MAKE_C_IDENTIFIER "${_rel}" _key)
        if(EXISTS "${_root}/${_rel}")
            _psxrecomp_testreg_read_declared(_d "${_root}/${_rel}")
            list(APPEND _declared ${_d})
            set(_dev_declared_${_key} "${_d}")
        else()
            list(APPEND _dev_missing "${_rel}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _declared)

    set(_dev_excused "")
    foreach(_entry IN LISTS PSXRECOMP_TESTS_REGISTERED_IN_DEV_TREES)
        # A ';' anywhere in an entry's reason makes CMake split it, so an
        # iteration can be a tail fragment with no fields at all. Skip those.
        if(NOT _entry MATCHES "\\|")
            continue()
        endif()
        # Split on '|' rather than peeling fields with anchored REGEX REPLACE:
        # CMake re-anchors '^' after each replacement, so "^[^|]*\\|" strips
        # every field, not the first one.
        string(REPLACE "|" ";" _fields "${_entry}")
        list(LENGTH _fields _n_fields)
        if(_n_fields LESS 2)
            continue()
        endif()
        list(GET _fields 0 _e_file)
        list(GET _fields 1 _e_owner)
        string(REGEX REPLACE "\\.(c|cpp|py)$" "" _e_stem "${_e_file}")

        list(FIND PSXRECOMP_TESTREG_DEV_BUILD_FILES "${_e_owner}" _owner_idx)
        if(_owner_idx EQUAL -1)
            message(FATAL_ERROR
                "PSXRECOMP_TESTS_REGISTERED_IN_DEV_TREES entry for ${_e_file} "
                "names '${_e_owner}', which is not listed in "
                "PSXRECOMP_TESTREG_DEV_BUILD_FILES. Add it there, or point the "
                "entry at the build file that really registers the test.")
        endif()

        list(FIND _dev_missing "${_e_owner}" _missing_idx)
        if(_missing_idx EQUAL -1)
            # The owner is present, so the claim is checkable. Check it.
            string(MAKE_C_IDENTIFIER "${_e_owner}" _key)
            list(FIND _dev_declared_${_key} "${_e_stem}" _idx)
            if(_idx EQUAL -1)
                message(FATAL_ERROR
                    "${_e_file} is declared in "
                    "PSXRECOMP_TESTS_REGISTERED_IN_DEV_TREES as registered by "
                    "${_e_owner}, but that file does not register it.\n"
                    "A stale entry silences this guard in every source package, "
                    "where ${_e_owner} is not shipped and the claim cannot be "
                    "re-checked. Drop the entry, or fix the registration.")
            endif()
        else()
            # Excuse the FILENAME, not the stem. Stem-matching would let a
            # declaration about test_foo.py silently excuse a later, genuinely
            # unregistered test_foo.c in every tree without the dev build file
            # — i.e. in every shipped kit, which is where this guard is the
            # only thing looking. PSXRECOMP_TESTS_NOT_REGISTERED matches on
            # filename for the same reason.
            list(APPEND _dev_excused "${_e_file}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _declared)

    # ---- what is actually on disk ---------------------------------------
    # CONFIGURE_DEPENDS matters more than usual here: without it, ADDING a test
    # file would not re-run configure, so this check would sleep through the
    # exact event it exists to catch.
    file(GLOB _found CONFIGURE_DEPENDS
        "${_root}/runtime/tests/test_*.c"
        "${_root}/runtime/tests/test_*.cpp"
        "${_root}/runtime/tests/test_*.py"
        "${_root}/recompiler/tests/*_test.cpp"
        "${_root}/recompiler/tests/test_*.py")

    # ---- dev-only registrations must be declared --------------------------
    # The other half of the cross-check: a shipped test whose only add_test()
    # lives in an unshipped build file is invisible to a source package, so it
    # has to be named in PSXRECOMP_TESTS_REGISTERED_IN_DEV_TREES. Catching that
    # here means the full checkout fails first, instead of the kit failing later
    # on somebody else's machine.
    set(_undeclared_dev "")
    foreach(_rel IN LISTS PSXRECOMP_TESTREG_DEV_BUILD_FILES)
        list(FIND _dev_missing "${_rel}" _missing_idx)
        if(NOT _missing_idx EQUAL -1)
            continue()
        endif()
        string(MAKE_C_IDENTIFIER "${_rel}" _key)
        foreach(_path IN LISTS _found)
            get_filename_component(_file "${_path}" NAME)
            get_filename_component(_stem "${_path}" NAME_WE)

            list(FIND _dev_declared_${_key} "${_stem}" _idx)
            if(_idx EQUAL -1)
                continue()
            endif()
            list(FIND _core_declared "${_stem}" _idx)
            if(NOT _idx EQUAL -1)
                continue()
            endif()

            set(_known FALSE)
            foreach(_known_list IN LISTS PSXRECOMP_TESTS_NOT_REGISTERED
                                         PSXRECOMP_TESTS_REGISTERED_IN_DEV_TREES)
                string(REGEX REPLACE "\\|.*$" "" _known_file "${_known_list}")
                if(_known_file STREQUAL _file)
                    set(_known TRUE)
                    break()
                endif()
            endforeach()
            if(NOT _known)
                list(APPEND _undeclared_dev "${_file} (only in ${_rel})")
            endif()
        endforeach()
    endforeach()
    if(_undeclared_dev)
        list(REMOVE_DUPLICATES _undeclared_dev)
        list(JOIN _undeclared_dev "\n    " _pretty)
        message(FATAL_ERROR
            "Test file(s) registered only in a build file that source packages "
            "do not ship:\n"
            "    ${_pretty}\n\n"
            "A game kit carries runtime/tests/ but not the developer trees, so "
            "this test would look like an orphan there and break `cmake` for "
            "everyone rebuilding the emitters from a kit.\n\n"
            "Fix by EITHER:\n"
            "  * registering it in runtime/CMakeLists.txt or "
            "recompiler/CMakeLists.txt as well; or\n"
            "  * adding '<filename>|<dev build file>|<reason>' to "
            "PSXRECOMP_TESTS_REGISTERED_IN_DEV_TREES in "
            "runtime/check_test_registration.cmake.")
    endif()

    # ---- diff -------------------------------------------------------------
    set(_orphans "")
    foreach(_path IN LISTS _found)
        get_filename_component(_file "${_path}" NAME)
        get_filename_component(_stem "${_path}" NAME_WE)

        set(_excused FALSE)
        foreach(_ex IN LISTS PSXRECOMP_TESTS_NOT_REGISTERED)
            string(REGEX REPLACE "\\|.*$" "" _ex_file "${_ex}")
            if(_ex_file STREQUAL _file)
                set(_excused TRUE)
                break()
            endif()
        endforeach()
        # Registered, but only in a dev build file this tree does not carry.
        list(FIND _dev_excused "${_file}" _dev_idx)
        if(NOT _dev_idx EQUAL -1)
            set(_excused TRUE)
        endif()
        if(_excused)
            continue()
        endif()

        list(FIND _declared "${_stem}" _idx)
        if(_idx EQUAL -1)
            file(RELATIVE_PATH _rel "${_root}" "${_path}")
            list(APPEND _orphans "${_rel}")
        endif()
    endforeach()

    if(_orphans)
        list(JOIN _orphans "\n    " _pretty)
        message(FATAL_ERROR
            "Test file(s) present on disk but registered nowhere:\n"
            "    ${_pretty}\n\n"
            "An unregistered test cannot run, cannot fail, and rots silently — "
            "four of them had already broken against the runtime API before "
            "anyone noticed (see the header of "
            "runtime/check_test_registration.cmake).\n\n"
            "Fix by EITHER:\n"
            "  * adding an add_test() entry in runtime/CMakeLists.txt or "
            "recompiler/CMakeLists.txt — register it DISABLED via "
            "set_tests_properties(<name> PROPERTIES DISABLED TRUE) if it is "
            "known-failing, so it stays visible in `ctest -N` instead of "
            "vanishing; or\n"
            "  * adding '<filename>|<reason>' to "
            "PSXRECOMP_TESTS_NOT_REGISTERED in "
            "runtime/check_test_registration.cmake, if it is a fixture or is "
            "driven by another test.")
    endif()

    list(LENGTH _found _n_found)
    if(_dev_missing)
        list(JOIN _dev_missing ", " _pretty_missing)
        list(LENGTH _dev_excused _n_excused)
        message(STATUS
            "test-registration guard: ${_n_found} test file(s), all registered "
            "(${_n_excused} via declared dev build file(s) not in this tree: "
            "${_pretty_missing})")
    else()
        message(STATUS
            "test-registration guard: ${_n_found} test file(s), all registered")
    endif()
endfunction()
