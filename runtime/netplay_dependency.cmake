include_guard(GLOBAL)

include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/psx_dependency_archive.cmake")

# psxrecomp_netplay_libjuice(<recomp-net root>)
#
# libjuice for recomp-net's ICE transport, resolved offline-first.
#
# With RNET_ENABLE_ICE on, recomp-net FetchContents libjuice from GitHub at
# configure time. That download sat outside third_party/deps.manifest, so
# PSX_DEPS_OFFLINE did not cover it: a sealed build (a Workbench build, a
# player behind a firewall) failed inside the FetchContent subbuild with a
# curl error. Call this before add_subdirectory(recomp-net). It declares the
# same pin from the vendored archive first; FetchContent keeps the first
# declaration of a name, so recomp-net's own FetchContent_MakeAvailable(libjuice)
# builds this copy with recomp-net's own options, and nothing else changes.
#
# Does nothing when the caller already chose a libjuice: RNET_LIBJUICE_ROOT, a
# tree vendored inside recomp-net, or RNET_ICE_BUNDLE_STATIC=OFF
# (find_package). Stops the configure when recomp-net pins a different libjuice
# than deps.manifest, so a submodule bump cannot silently keep the old one.
function(psxrecomp_netplay_libjuice recomp_net_root)
    if(NOT RNET_ENABLE_ICE)
        return()
    endif()
    if(DEFINED RNET_ICE_BUNDLE_STATIC AND NOT RNET_ICE_BUNDLE_STATIC)
        return()
    endif()
    if(RNET_LIBJUICE_ROOT OR
       EXISTS "${recomp_net_root}/third_party/libjuice/CMakeLists.txt")
        return()
    endif()

    _psx_dependency_pin(libjuice _file _sha _url)
    file(STRINGS "${recomp_net_root}/CMakeLists.txt" _pins
         REGEX "URL_HASH[ \t]+SHA256=[0-9a-fA-F]+")
    set(_seen "")
    foreach(_line IN LISTS _pins)
        string(REGEX REPLACE ".*SHA256=([0-9a-fA-F]+).*" "\\1" _pin "${_line}")
        string(TOLOWER "${_pin}" _pin)
        list(APPEND _seen "${_pin}")
    endforeach()
    list(REMOVE_DUPLICATES _seen)
    if(NOT _seen STREQUAL "${_sha}")
        message(FATAL_ERROR
            "psxrecomp: recomp-net pins libjuice SHA256 '${_seen}' "
            "(${recomp_net_root}/CMakeLists.txt), but third_party/deps.manifest "
            "pins ${_sha}.\n"
            "  Update the libjuice row and archive in third_party/ to the pin "
            "recomp-net uses (tools/ci/vendor_deps.sh libjuice).")
    endif()

    psxrecomp_dependency_source_dir(libjuice
        ENV PSX_LIBJUICE_SOURCE_DIR
        OUT _src)
    psxrecomp_dependency_archive(libjuice
        SOURCE_DIR "${_src}"
        OUT_URL _archive OUT_HASH _hash)
    set(_timestamp "")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        # Extracted files take the extraction time, not the archive's. Old or
        # future mtimes make Ninja treat build.ninja as dirty on Windows.
        set(_timestamp DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()
    FetchContent_Declare(libjuice
        URL "${_archive}"
        URL_HASH "${_hash}"
        ${_timestamp})
endfunction()
