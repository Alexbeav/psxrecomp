cmake_minimum_required(VERSION 3.20)
project(netplay_libjuice_offline C)

# PS1B-286: a netplay configure resolves libjuice from third_party/ with the
# network sealed, and refuses a recomp-net that pins a different libjuice.
#
# FAKE_RECOMP_NET stands in for lib/recomp-net: it asks FetchContent for
# libjuice by URL exactly as recomp-net's CMakeLists.txt does. runtime/CMakeLists.txt
# runs this configure with HTTP(S)_PROXY pointed at a closed port, so any
# download fails.
if(NOT FAKE_RECOMP_NET)
    message(FATAL_ERROR "pass -DFAKE_RECOMP_NET=<dir>")
endif()

set(PSXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
include("${PSXRECOMP_ROOT}/runtime/netplay_dependency.cmake")

set(RNET_ENABLE_ICE ON)
psxrecomp_netplay_libjuice("${FAKE_RECOMP_NET}")
add_subdirectory("${FAKE_RECOMP_NET}" fake-recomp-net)

if(NOT TARGET juice-static)
    message(FATAL_ERROR "libjuice was not made available")
endif()
message(STATUS "netplay_libjuice_offline: libjuice configured with no download")
