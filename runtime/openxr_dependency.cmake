include_guard(GLOBAL)

# Optional host headset backend. Faithful builds have no SDK dependency and
# this file does nothing with PSX_OPENXR=OFF (no FetchContent, no network).
#
# PSX_OPENXR (this CMake option) compiles the backend. It is a different thing
# from the PSX_OPENXR *environment variable* read at startup (main.cpp,
# configure_core_gl_context_attributes), which only asks an XR-capable build for
# a 4.6 GL context. The environment variable is also accepted as PSX_OPENXR_ENABLE;
# see docs/OPENXR_RENDERING.md.
option(PSX_OPENXR "Build experimental Win32/OpenGL OpenXR backend" OFF)
if(PSX_OPENXR)
    if(NOT WIN32)
        message(FATAL_ERROR "PSX_OPENXR currently supports Win32/OpenGL")
    endif()
    include(FetchContent)
    include("${CMAKE_CURRENT_LIST_DIR}/../cmake/psx_dependency_archive.cmake")

    set(_psx_openxr_timestamp_args "")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        list(APPEND _psx_openxr_timestamp_args DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()

    # Same resolution as libchdr (chd_dependency.cmake): a pre-extracted tree
    # (-DFETCHCONTENT_SOURCE_DIR_PSX_OPENXR_SDK or PSX_OPENXR_SDK_SOURCE_DIR),
    # else a vendored archive in third_party/, else the pinned upstream archive
    # verified against the SHA256 in third_party/deps.manifest.
    # tools/ci/vendor_deps.sh psx_openxr_sdk stages the archive.
    psxrecomp_dependency_source_dir(psx_openxr_sdk
        ENV PSX_OPENXR_SDK_SOURCE_DIR
        OUT _psx_openxr_src)
    psxrecomp_dependency_archive(psx_openxr_sdk
        SOURCE_DIR "${_psx_openxr_src}"
        OUT_URL _psx_openxr_url OUT_HASH _psx_openxr_hash)

    FetchContent_Declare(psx_openxr_sdk
        URL
            "${_psx_openxr_url}"
        URL_HASH
            "${_psx_openxr_hash}"
        ${_psx_openxr_timestamp_args})

    # The SDK's options have generic names (BUILD_TESTS, ...) and no prefix. Set
    # them as plain variables in this function's scope only: the SDK's option()
    # calls honour a normal variable (CMP0077, its minimum is 3.16) without
    # creating or forcing a cache entry, so they cannot leak into the rest of
    # the build or into another dependency that reads BUILD_TESTS.
    function(_psx_openxr_make_available)
        set(BUILD_TESTS OFF)
        set(BUILD_API_LAYERS OFF)
        set(BUILD_LOADER ON)
        set(DYNAMIC_LOADER OFF)
        FetchContent_MakeAvailable(psx_openxr_sdk)
    endfunction()
    _psx_openxr_make_available()

    unset(_psx_openxr_src)
    unset(_psx_openxr_url)
    unset(_psx_openxr_hash)
    unset(_psx_openxr_timestamp_args)
    if(MINGW)
        # Older MinGW desktop headers omit WINAPI_PARTITION_SYSTEM. The SDK
        # correctly treats it as zero; leave this vendor warning nonfatal.
        target_compile_options(openxr_loader PRIVATE -Wno-error=undef)
    endif()
endif()