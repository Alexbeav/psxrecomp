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
# no WARNING is given. That is not "not stale". The earlier list does not hold
# three sources of the target (ps1_exe_parser.cpp, pgxp_hook_emitter.cpp,
# recompiler_patch.cpp), and such a stamp cannot say whether one of them has
# changed. The STATUS line says so. Any other value is STALE, as before. The next
# regeneration writes a stamp of the current list.
#
# A CHECK THAT DID NOT RUN SAYS SO
# When the fingerprint script fails, one STATUS line gives its exit code. Without
# it, a script that cannot read the target would switch the check off in every
# configure with no word.
#
# THE BASH THAT RUNS THE SCRIPT
# A bash named with -D_psxrt_bash=<bash> is used as given. Otherwise, on a Windows
# host, Git for Windows is looked for first: in its usual folders, beside a git.exe
# on PATH, then on PATH itself. A bash.exe below %SystemRoot% or in a
# Microsoft\WindowsApps folder starts WSL. It hands the script's Windows path to a
# Linux shell, which cannot find it (PS1B-271), so it is never run, named by the
# caller or not. System32 comes before Git on the PATH of a usual host, which is
# why the first bash on PATH is not taken. When only such a launcher is found, one
# STATUS line says that the check was skipped and why. A host with no bash at all
# skips the check without a line, as before.
#
# Included by runtime.cmake. It also runs alone, which is how
# recompiler/tests/test_bios_emitter_fingerprint.py checks its verdicts:
#   cmake -DPSXRECOMP_BIOS_STALE_CHECK_RUN=ON -DPSXRECOMP_ROOT=<root>
#         -DPSXRECOMP_BIOS_STEM=<stem> -DPSXRECOMP_BIOS_PROFILE=<profile>
#         -D_psxrt_bash=<bash> -P bios_stale_check.cmake

include("${CMAKE_CURRENT_LIST_DIR}/host_bash.cmake")

function(psxrecomp_check_bios_stale)
    set(_script "${PSXRECOMP_ROOT}/tools/bios_emitter_fingerprint.sh")
    set(_stamp "${PSXRECOMP_ROOT}/generated/${PSXRECOMP_BIOS_STEM}.emitter.sha")
    if(NOT EXISTS "${_script}")
        return()
    endif()
    psxrecomp_find_bash(_bash _launcher)
    if(NOT _bash)
        if(_launcher)
            message(STATUS
                "psxrecomp: BIOS staleness check skipped: the only bash found "
                "is a WSL launcher (${_launcher}), which cannot run "
                "tools/bios_emitter_fingerprint.sh from a Windows path. "
                "Install Git for Windows, or pass "
                "-D_psxrt_bash=<Git>/bin/bash.exe.")
        endif()
        return()
    endif()

    execute_process(
        COMMAND "${_bash}" "${_script}" "${PSXRECOMP_BIOS_PROFILE}"
        WORKING_DIRECTORY "${PSXRECOMP_ROOT}"
        OUTPUT_VARIABLE _cur_fp OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _fp_rc ERROR_QUIET)
    if(NOT _fp_rc EQUAL 0 OR NOT _cur_fp)
        message(STATUS
            "psxrecomp: BIOS staleness check skipped: "
            "tools/bios_emitter_fingerprint.sh gave no fingerprint "
            "(exit ${_fp_rc}) with ${_bash}. Run it by hand with the BIOS "
            "profile to see why.")
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
        COMMAND "${_bash}" "${_script}" --hand-list "${PSXRECOMP_BIOS_PROFILE}"
        WORKING_DIRECTORY "${PSXRECOMP_ROOT}"
        OUTPUT_VARIABLE _hand_fp OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _hand_rc ERROR_QUIET)
    if(_hand_rc EQUAL 0 AND _hand_fp AND _saved_fp STREQUAL _hand_fp)
        message(STATUS
            "psxrecomp: BIOS generated/ carries an emitter fingerprint that "
            "was written with the earlier file list "
            "(${PSXRECOMP_BIOS_STEM}.emitter.sha). No file of that list has "
            "changed. That list does not cover ps1_exe_parser.cpp, "
            "pgxp_hook_emitter.cpp and recompiler_patch.cpp: whether the "
            "BIOS is stale for those three is unknown until "
            "tools/regen_bios.sh has run.")
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
