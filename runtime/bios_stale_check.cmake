# bios_stale_check.cmake — the BIOS generated/ staleness check (hygiene).
#
# generated/<stem>_*.c is gitignored build output produced by a SEPARATE build
# (recompiler/ -> psxrecomp-bios). Editing the BIOS emitter without re-running
# tools/regen_bios.sh leaves the runtime linking a stale BIOS that no longer matches
# the emitter (this caused a 4439-vs-4406 drift). regen_bios.sh records an emitter
# fingerprint in generated/<stem>.emitter.sha; this recomputes it (same profile
# argument as regen_bios.sh passes) and WARNS on a mismatch so the staleness is
# impossible to miss. Non-fatal: a stale-but-consistent BIOS still builds; opt out
# with -DPSXRECOMP_SKIP_BIOS_STALE_CHECK=ON.
#
# A STAMP OF THE EARLIER FILE LIST IS NOT A STALE BIOS (PS1B-131)
# Through pin H the fingerprint hashed a list of emitter files kept by hand. It now
# takes the list from the psxrecomp-bios target, so the same tree gives another
# value. The stamps are build output in each tree's generated/ folder; no commit
# can rewrite them. Without the step below, every tree stamped before the change
# would read as STALE at its next configure, about a BIOS that nothing touched.
# So a stamp that differs from the current value is compared once more, with the
# value of the earlier list (the script still computes it, --hand-list). Equal
# means that no file of the earlier list has changed since the BIOS was generated:
# the tree is reported as not stale, with a line that says which list the stamp
# covers. Any other value is STALE, as before. The next regeneration writes a
# stamp of the current list.
#
# Included by runtime.cmake. It also runs alone, which is how
# recompiler/tests/test_bios_emitter_fingerprint.py checks its verdicts:
#   cmake -DPSXRECOMP_BIOS_STALE_CHECK_RUN=ON -DPSXRECOMP_ROOT=<root>
#         -DPSXRECOMP_BIOS_STEM=<stem> -DPSXRECOMP_BIOS_PROFILE=<profile>
#         -D_psxrt_bash=<bash> -P bios_stale_check.cmake

function(psxrecomp_check_bios_stale)
    find_program(_psxrt_bash NAMES bash)
    set(_script "${PSXRECOMP_ROOT}/tools/bios_emitter_fingerprint.sh")
    set(_stamp "${PSXRECOMP_ROOT}/generated/${PSXRECOMP_BIOS_STEM}.emitter.sha")
    if(NOT _psxrt_bash OR NOT EXISTS "${_script}")
        return()
    endif()

    execute_process(
        COMMAND "${_psxrt_bash}" "${_script}" "${PSXRECOMP_BIOS_PROFILE}"
        WORKING_DIRECTORY "${PSXRECOMP_ROOT}"
        OUTPUT_VARIABLE _cur_fp OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _fp_rc ERROR_QUIET)
    if(NOT _fp_rc EQUAL 0 OR NOT _cur_fp)
        return()
    endif()

    set(_saved_fp "")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _saved_fp)
        string(STRIP "${_saved_fp}" _saved_fp)
    endif()
    if(_saved_fp STREQUAL "")
        # No stamp at all is not evidence of drift: say so quietly
        # rather than crying STALE about a BIOS that may be fresh.
        message(STATUS
            "psxrecomp: BIOS generated/ carries no emitter "
            "fingerprint (${PSXRECOMP_BIOS_STEM}.emitter.sha) - "
            "provenance unknown, staleness not checked.")
        return()
    endif()
    if(_saved_fp STREQUAL _cur_fp)
        return()
    endif()

    execute_process(
        COMMAND "${_psxrt_bash}" "${_script}" --hand-list "${PSXRECOMP_BIOS_PROFILE}"
        WORKING_DIRECTORY "${PSXRECOMP_ROOT}"
        OUTPUT_VARIABLE _hand_fp OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _hand_rc ERROR_QUIET)
    if(_hand_rc EQUAL 0 AND _hand_fp AND _saved_fp STREQUAL _hand_fp)
        message(STATUS
            "psxrecomp: BIOS generated/ carries an emitter fingerprint of "
            "the earlier file list (${PSXRECOMP_BIOS_STEM}.emitter.sha) and "
            "matches it: no file of that list has changed, not stale. The "
            "list now follows the psxrecomp-bios target; the next "
            "tools/regen_bios.sh writes a stamp that covers all of it.")
        return()
    endif()

    message(WARNING
        "BIOS generated/ is STALE vs the recompiler emitter "
        "(fingerprint mismatch).\n"
        "  Linking generated/${PSXRECOMP_BIOS_STEM}_*.c that may not "
        "match the current emitter source, seeds, ROM or profile.\n"
        "  Fix:  tools/regen_bios.sh --config <profile>   (rebuilds "
        "psxrecomp-bios + regenerates the BIOS)\n"
        "  (Suppress: -DPSXRECOMP_SKIP_BIOS_STALE_CHECK=ON)")
endfunction()

if(PSXRECOMP_BIOS_STALE_CHECK_RUN)
    psxrecomp_check_bios_stale()
endif()
