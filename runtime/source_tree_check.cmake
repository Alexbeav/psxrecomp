# Two tests around a suite: no test may leave a new or changed file in the source tree.
#
# WHY THIS EXISTS
# ---------------
# On 2026-10-01 the final Windows run of a release candidate passed every test
# in three trees and still left the worktree dirty: recompiler/tests/
# test_pgo_rebuild_flow.py ran the real licence step with the repository's own
# root as the product folder, and a licenses/ folder of 39 files appeared there
# (PS1B-333). Nothing in the suite could see it. It was found because that run
# counted `git status` lines by hand afterwards. A test that writes into the
# source tree is silent by default; this inverts that default.
#
# HOW IT WORKS
# ------------
# source_tree_snapshot records what `git status --untracked-files=all` lists
# below the framework's root, with a hash of each listed file.
# source_tree_unchanged lists it again and fails on a new, changed or missing
# entry (tools/tests/source_tree_clean.py; its own test is
# tools/tests/test_source_tree_clean.py). A tree that was dirty before the
# suite is fine: only a change during the suite fails.
#
# The order comes from the DEPENDS property, which orders tests and nothing
# else: the snapshot runs before every test of the calling directory, the
# comparison after all of them, also under `ctest -j`. A selection that leaves
# the two out (`ctest -R name`) runs as before.
#
# LIMITS
# ------
#   * Tests that a subdirectory registers (add_subdirectory) are not ordered,
#     so a file one of them writes is seen only if it runs between the two.
#   * Outside a git checkout (a source package), or without git, the
#     comparison prints NOT CHECKED with the reason and passes. Read its line:
#     a pass that checked says "source tree unchanged by the suite: N entries
#     ... before, N after".
#   * `ctest -R source_tree_unchanged` alone has no snapshot and says so.
#
# Usage: call at the END of a CMakeLists.txt, after its last add_test().
#   include(${CMAKE_CURRENT_SOURCE_DIR}/source_tree_check.cmake)
#   psxrecomp_add_source_tree_check()

set(_PSXRECOMP_SRCTREE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(psxrecomp_add_source_tree_check)
    if(NOT BUILD_TESTING OR NOT Python3_EXECUTABLE)
        return()
    endif()
    get_filename_component(_root "${_PSXRECOMP_SRCTREE_DIR}/.." ABSOLUTE)
    set(_script "${_root}/tools/tests/source_tree_clean.py")
    if(NOT EXISTS "${_script}")
        return()    # a source package without tools/tests
    endif()
    # One pair per build tree, also when one build configures both projects.
    get_property(_added GLOBAL PROPERTY PSXRECOMP_SOURCE_TREE_CHECK_ADDED)
    if(_added)
        return()
    endif()
    set_property(GLOBAL PROPERTY PSXRECOMP_SOURCE_TREE_CHECK_ADDED TRUE)

    get_property(_suite DIRECTORY PROPERTY TESTS)
    set(_state "${CMAKE_CURRENT_BINARY_DIR}/source_tree_snapshot.json")
    add_test(NAME source_tree_snapshot
             COMMAND ${Python3_EXECUTABLE} "${_script}" snapshot --root "${_root}" --state "${_state}")
    add_test(NAME source_tree_unchanged
             COMMAND ${Python3_EXECUTABLE} "${_script}" compare --root "${_root}" --state "${_state}")
    foreach(_test IN LISTS _suite)
        set_property(TEST ${_test} APPEND PROPERTY DEPENDS source_tree_snapshot)
    endforeach()
    set_property(TEST source_tree_unchanged APPEND PROPERTY DEPENDS source_tree_snapshot ${_suite})
endfunction()
