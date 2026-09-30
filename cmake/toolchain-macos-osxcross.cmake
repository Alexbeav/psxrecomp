# CMake toolchain file: cross-build psxrecomp title products for macOS with
# osxcross (upstream clang + cctools-port ld64) on a Linux host.
#
#   cmake -S <title> -B build-macos-x64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<psxrecomp>/cmake/toolchains/macos-osxcross.cmake \
#     -DPSX_DARWIN_ARCH=x86_64 -DOSXCROSS_ROOT=/opt/osxcross
#
# PSX_DARWIN_ARCH: x86_64 (default) or arm64.
# OSXCROSS_ROOT:   osxcross TARGET_DIR (bin/ holds <arch>-apple-darwinNN-clang).
#                  Falls back to $OSXCROSS_ROOT, then the osxcross-conf on PATH.
# CMAKE_OSX_DEPLOYMENT_TARGET defaults to 11.0, matching the title release.yml
# macos-x64 / macos-arm64 jobs.
#
# The Apple SDK is not redistributable: package it from an owned Mac with
# osxcross tools/gen_sdk_package_tools.sh and keep it on private storage.

set(PSX_DARWIN_ARCH "x86_64" CACHE STRING "Target macOS architecture (x86_64 or arm64)")
set_property(CACHE PSX_DARWIN_ARCH PROPERTY STRINGS x86_64 arm64)
if(NOT PSX_DARWIN_ARCH MATCHES "^(x86_64|arm64)$")
    message(FATAL_ERROR "PSX_DARWIN_ARCH must be x86_64 or arm64 (got '${PSX_DARWIN_ARCH}')")
endif()

# Try-compile projects re-read this file; forward the two knobs.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PSX_DARWIN_ARCH OSXCROSS_ROOT)

if(NOT OSXCROSS_ROOT)
    if(DEFINED ENV{OSXCROSS_ROOT})
        set(OSXCROSS_ROOT "$ENV{OSXCROSS_ROOT}")
    else()
        find_program(_psx_osxcross_conf osxcross-conf)
        if(_psx_osxcross_conf)
            get_filename_component(OSXCROSS_ROOT "${_psx_osxcross_conf}" DIRECTORY)
            get_filename_component(OSXCROSS_ROOT "${OSXCROSS_ROOT}" DIRECTORY)
        endif()
    endif()
endif()
if(NOT OSXCROSS_ROOT OR NOT IS_DIRECTORY "${OSXCROSS_ROOT}/bin")
    message(FATAL_ERROR "Set OSXCROSS_ROOT to the osxcross target directory (got '${OSXCROSS_ROOT}')")
endif()
set(OSXCROSS_ROOT "${OSXCROSS_ROOT}" CACHE PATH "osxcross target directory")

# osxcross names its wrappers <arch>-apple-darwin<kernel>-clang; find ours.
file(GLOB _psx_osxcross_cc "${OSXCROSS_ROOT}/bin/${PSX_DARWIN_ARCH}-apple-darwin*-clang")
list(FILTER _psx_osxcross_cc INCLUDE REGEX "/${PSX_DARWIN_ARCH}-apple-darwin[0-9.]+-clang$")
list(LENGTH _psx_osxcross_cc _psx_osxcross_n)
if(NOT _psx_osxcross_n EQUAL 1)
    message(FATAL_ERROR "Expected one ${PSX_DARWIN_ARCH}-apple-darwin*-clang in ${OSXCROSS_ROOT}/bin, found: ${_psx_osxcross_cc}")
endif()
get_filename_component(_psx_osxcross_cc_name "${_psx_osxcross_cc}" NAME)
string(REGEX REPLACE "-clang$" "" PSX_DARWIN_TRIPLE "${_psx_osxcross_cc_name}")
set(_psx_osxcross_prefix "${OSXCROSS_ROOT}/bin/${PSX_DARWIN_TRIPLE}")

file(GLOB _psx_osxcross_sdk LIST_DIRECTORIES true "${OSXCROSS_ROOT}/SDK/MacOSX*.sdk")
list(SORT _psx_osxcross_sdk)
list(POP_BACK _psx_osxcross_sdk _psx_osxcross_sdk_last)
if(NOT _psx_osxcross_sdk_last)
    message(FATAL_ERROR "No MacOSX*.sdk under ${OSXCROSS_ROOT}/SDK")
endif()

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR "${PSX_DARWIN_ARCH}")
set(CMAKE_OSX_ARCHITECTURES "${PSX_DARWIN_ARCH}" CACHE STRING "")
set(CMAKE_OSX_SYSROOT "${_psx_osxcross_sdk_last}" CACHE PATH "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

set(CMAKE_C_COMPILER "${_psx_osxcross_prefix}-clang")
set(CMAKE_CXX_COMPILER "${_psx_osxcross_prefix}-clang++")
set(CMAKE_OBJC_COMPILER "${_psx_osxcross_prefix}-clang")
set(CMAKE_OBJCXX_COMPILER "${_psx_osxcross_prefix}-clang++")
set(CMAKE_AR "${_psx_osxcross_prefix}-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_psx_osxcross_prefix}-ranlib" CACHE FILEPATH "")
set(CMAKE_LIBTOOL "${_psx_osxcross_prefix}-libtool" CACHE FILEPATH "")
set(CMAKE_INSTALL_NAME_TOOL "${_psx_osxcross_prefix}-install_name_tool" CACHE FILEPATH "")
set(CMAKE_LIPO "${_psx_osxcross_prefix}-lipo" CACHE FILEPATH "")
set(CMAKE_STRIP "${_psx_osxcross_prefix}-strip" CACHE FILEPATH "")
set(CMAKE_NM "${_psx_osxcross_prefix}-nm" CACHE FILEPATH "")
set(CMAKE_OTOOL "${_psx_osxcross_prefix}-otool" CACHE FILEPATH "")

# The osxcross wrappers exec the host clang, which looks for <triple>-ld next
# to itself and falls back to the host ELF ld unless osxcross/bin is on PATH.
# Name ld64 explicitly so the build does not depend on PATH.
set(CMAKE_LINKER "${_psx_osxcross_prefix}-ld" CACHE FILEPATH "")
foreach(_psx_kind EXE SHARED MODULE)
    set(CMAKE_${_psx_kind}_LINKER_FLAGS_INIT "--ld-path=${_psx_osxcross_prefix}-ld")
endforeach()

set(CMAKE_FIND_ROOT_PATH "${CMAKE_OSX_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Host-side pkg-config must not leak Linux packages into the Darwin build.
set(ENV{PKG_CONFIG_LIBDIR} "")
set(ENV{PKG_CONFIG_PATH} "")
