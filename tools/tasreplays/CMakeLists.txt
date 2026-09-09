cmake_minimum_required(VERSION 3.24)
project(Tekken3Tas LANGUAGES C CXX)
get_filename_component(PSXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(TAS_PROJECT_DIR "${PSXRECOMP_ROOT}/build/tekken3" CACHE PATH "Prepared title directory")
set(PSXRECOMP_BIOS_STEMS "SCPH1001" CACHE STRING "BIOS")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DPSX_ENABLE_BLOCK_CYCLES=1")
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")
file(GLOB TAS_GAME_SHARDS CONFIGURE_DEPENDS "${TAS_PROJECT_DIR}/generated/SLUS_004.02_full_*.c")
if(NOT TAS_GAME_SHARDS)
    message(FATAL_ERROR "Run tools/tasreplays/tekken3.py setup first")
endif()
psxrecomp_add_runtime_target(psx-runtime
    EXE_NAME Tekken3-TAS
    WINDOW_TITLE "Tekken 3 - Spikestuff TAS"
    DEFAULT_GAME_CONFIG_PATH "${TAS_PROJECT_DIR}/game.toml"
    GAME_GENERATED_FULL_C ${TAS_GAME_SHARDS}
    GAME_GENERATED_DISPATCH_C "${TAS_PROJECT_DIR}/generated/SLUS_004.02_dispatch.c")
