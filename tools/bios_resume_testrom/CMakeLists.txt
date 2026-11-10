# Native runtime for the BIOS resume-PC test ROM (T110). Links the recompiled
# test EXE into the standard runtime; run.py compares its RESULT block and
# publish_ring against psx-beetle booting the same disc.
cmake_minimum_required(VERSION 3.20)
project(BiosResumeTestROM C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Path to psxrecomp")

include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

psxrecomp_add_runtime_target(psx-bresume
    GAME_GENERATED_FULL_C "${CMAKE_CURRENT_SOURCE_DIR}/generated/bios_resume_testrom.exe_full_00.c"
    GAME_GENERATED_DISPATCH_C "${CMAKE_CURRENT_SOURCE_DIR}/generated/bios_resume_testrom.exe_dispatch.c"
    DEBUG_PORT 4610
    WINDOW_TITLE "BIOS Resume Test ROM"
    DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/bios/SCPH1001.BIN"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
)
