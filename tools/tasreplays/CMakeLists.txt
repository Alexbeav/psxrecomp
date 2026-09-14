cmake_minimum_required(VERSION 3.24)
project(Tekken3Tas LANGUAGES C CXX)
get_filename_component(PSXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(TAS_PROJECT_DIR "${PSXRECOMP_ROOT}/build/tekken3" CACHE PATH "Prepared title directory")
set(TAS_GAME_STEM "SLUS_004.02" CACHE STRING "Prepared boot-program filename stem")
set(TAS_EXE_NAME "Tekken3-TAS" CACHE STRING "Native player filename")
set(TAS_WINDOW_TITLE "Tekken 3 - Spikestuff TAS" CACHE STRING "Native player title")
set(PSXRECOMP_BIOS_STEMS "SCPH1001" CACHE STRING "BIOS")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DPSX_ENABLE_BLOCK_CYCLES=1")
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")
file(GLOB TAS_GAME_SHARDS CONFIGURE_DEPENDS "${TAS_PROJECT_DIR}/generated/${TAS_GAME_STEM}_full_*.c")
if(NOT TAS_GAME_SHARDS)
    message(FATAL_ERROR "Generate the selected title before configuring its player")
endif()
psxrecomp_add_runtime_target(psx-runtime
    EXE_NAME "${TAS_EXE_NAME}"
    WINDOW_TITLE "${TAS_WINDOW_TITLE}"
    DEFAULT_GAME_CONFIG_PATH "${TAS_PROJECT_DIR}/game.toml"
    GAME_GENERATED_FULL_C ${TAS_GAME_SHARDS}
    GAME_GENERATED_DISPATCH_C "${TAS_PROJECT_DIR}/generated/${TAS_GAME_STEM}_dispatch.c")
