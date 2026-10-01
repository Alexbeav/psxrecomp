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
# else. The comparison runs after every test of the calling directory and of
# the directories below it (add_subdirectory), also under `ctest -j`. The
# snapshot runs before every test of the calling directory; before those of
# the directories below it too when CMake is 3.28 or newer, which is the first
# version that can set a property on another directory's test. A selection
# that leaves the two out (`ctest -R name`) runs as before.
#
# LIMITS
# ------
#   * With CMake older than 3.28, a test of a subdirectory can start beside the
#     snapshot. A file it writes in that first moment is in the snapshot and so
#     is not reported.
#   * Tests of a directory ABOVE the calling one are not ordered at all.
#   * Outside a git checkout (a source package), or without git, the
#     comparison prints NOT CHECKED with the reason and passes. Read its line:
#     a pass that checked says "source tree unchanged by the suite: N entries
#     ... before, N after".
#   * `ctest -R source_tree_unchanged` alone has no snapshot and says so.
#
# Usage: call at the END of a CMakeLists.txt, after its last add_test() and
# its last add_subdirectory().
#   include(${CMAKE_CURRENT_SOURCE_DIR}/source_tree_check.cmake)
#   psxrecomp_add_source_tree_check()

set(_PSXRECOMP_SRCTREE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The tests of every directory below `dir`, in `out`. Where CMake can, each is
# also made to wait for the snapshot.
function(_psxrecomp_source_tree_tests_below dir out)
    set(_found "")
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        get_property(_tests DIRECTORY "${_subdir}" PROPERTY TESTS)
        if(_tests AND NOT CMAKE_VERSION VERSION_LESS 3.28)
            set_property(TEST ${_tests} DIRECTORY "${_subdir}"
                         APPEND PROPERTY DEPENDS source_tree_snapshot)
        endif()
        _psxrecomp_source_tree_tests_below("${_subdir}" _deeper)
        list(APPEND _found ${_tests} ${_deeper})
    endforeach()
    set(${out} "${_found}" PARENT_SCOPE)
endfunction()

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
    _psxrecomp_source_tree_tests_below("${CMAKE_CURRENT_SOURCE_DIR}" _below)
    set_property(TEST source_tree_unchanged APPEND PROPERTY
                 DEPENDS source_tree_snapshot ${_suite} ${_below})
    list(LENGTH _suite _n_suite)
    list(LENGTH _below _n_below)
    message(STATUS "source tree check: source_tree_unchanged runs after ${_n_suite} tests of this "
                   "directory and ${_n_below} of the directories below it")
endfunction()
