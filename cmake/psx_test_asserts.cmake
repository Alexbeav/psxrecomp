include_guard(GLOBAL)

# Keep assert() live in every test executable of the calling directory.
#
# Release and RelWithDebInfo add -DNDEBUG, which compiles assert() out. Most
# C and C++ tests here check their results with assert(), so a Release build
# ran them as empty programs that always pass (PS1B-590: dma_default_regs_test
# had expected a GPU linked-list channel to stay busy after its kick since the
# CPU hold of c1a8337f, and only a build without -DNDEBUG showed it). A few
# targets added -UNDEBUG one at a time; this gives it to all of them.
#
# Call this once at the end of a CMakeLists.txt, after its last add_test. It
# reaches each test whose name is an executable target of this directory. The
# product targets are not tests and keep the build type's flags.
function(psx_keep_test_asserts)
    if(MSVC)
        set(_flag /UNDEBUG)
    else()
        set(_flag -UNDEBUG)
    endif()
    get_property(_tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
    foreach(_test IN LISTS _tests)
        if(TARGET "${_test}")
            get_target_property(_type "${_test}" TYPE)
            if(_type STREQUAL "EXECUTABLE")
                target_compile_options("${_test}" PRIVATE "${_flag}")
            endif()
        endif()
    endforeach()
endfunction()
