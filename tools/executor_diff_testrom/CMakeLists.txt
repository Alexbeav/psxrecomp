# Native runtime for the cross-executor load-delay test ROM. Links the recompiled
# test EXE into the standard runtime; run.py runs it native and with
# PSX_FORCE_INTERP=1 and compares the RESULT blocks (psx-beetle as reference).
cmake_minimum_required(VERSION 3.20)
project(ExecutorDiffTestROM C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Path to psxrecomp")

include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

psxrecomp_add_runtime_target(psx-xdiff
    GAME_GENERATED_FULL_C "${CMAKE_CURRENT_SOURCE_DIR}/generated/executor_diff_testrom.exe_full_00.c"
    GAME_GENERATED_DISPATCH_C "${CMAKE_CURRENT_SOURCE_DIR}/generated/executor_diff_testrom.exe_dispatch.c"
    DEBUG_PORT 4611
    WINDOW_TITLE "Executor Diff Test ROM"
    DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/bios/SCPH1001.BIN"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
)
