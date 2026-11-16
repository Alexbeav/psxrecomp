#!/usr/bin/env python3
"""Regression: the APP_ICON resource must compile when the project path has a space.

GNU windres forwards its -I include list to the C preprocessor on an unquoted
command line. When the icon .rc is added directly to the runtime target it
inherits the runtime's include directories, so a player who installs the kit
under "D:/Retro Games/..." sees the first-run rebuild fail with
"cc1.exe: fatal error: Games/...: No such file or directory" while every
CI build (no spaces anywhere) passes.

runtime.cmake now compiles the .rc in an isolated OBJECT library with no
include directories or definitions. This test checks that wiring textually,
then proves the pattern with the resource compiler on this machine: a tiny
CMake project in a directory whose name contains a space, an executable with
an include directory that contains a space, and the icon .rc built through the
isolated object library. On Windows with an RC compiler the build must
succeed; elsewhere only the textual check runs.
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
RUNTIME_CMAKE = ROOT / "runtime" / "runtime.cmake"
ICON = ROOT / "assets" / "psxrecomp.ico"

PROJECT = """cmake_minimum_required(VERSION 3.20)
project(rc_spaces LANGUAGES C RC)
add_executable(app main.c)
target_include_directories(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/inc dir")
target_compile_definitions(app PRIVATE "TITLE=\\"Two Words\\"")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc" "IDI_ICON1 ICON \\"${ICON}\\"\\n")
add_library(app_icon OBJECT "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc")
set_target_properties(app_icon PROPERTIES INCLUDE_DIRECTORIES "" COMPILE_DEFINITIONS "" COMPILE_OPTIONS "")
target_sources(app PRIVATE $<TARGET_OBJECTS:app_icon>)
"""


def main() -> int:
    text = RUNTIME_CMAKE.read_text(encoding="utf-8")
    for needle in (
        "add_library(${target}_app_icon OBJECT",
        'INCLUDE_DIRECTORIES ""',
        "target_sources(${target} PRIVATE $<TARGET_OBJECTS:${target}_app_icon>)",
    ):
        assert needle in text, f"runtime.cmake no longer isolates the icon .rc: {needle}"
    assert 'target_sources(${target} PRIVATE "${_psxrt_rc}")' not in text, \
        "runtime.cmake adds the .rc straight to the runtime target again"
    print("ok: runtime.cmake compiles the APP_ICON .rc in an isolated object library")

    if sys.platform != "win32":
        print("SKIP behavioural check: not Windows")
        return 0
    cmake = shutil.which("cmake")
    rc = shutil.which("windres") or shutil.which("llvm-windres") or shutil.which("llvm-rc")
    cc = shutil.which("gcc") or shutil.which("clang") or shutil.which("cc")
    if not (cmake and rc and cc and ICON.is_file()):
        print("SKIP behavioural check: cmake, a C compiler, a resource compiler, and assets/psxrecomp.ico are required")
        return 0

    with tempfile.TemporaryDirectory() as temporary:
        src = Path(temporary) / "space in path"
        (src / "inc dir").mkdir(parents=True)
        (src / "inc dir" / "hello.h").write_text("#define HELLO 1\n", encoding="utf-8")
        (src / "main.c").write_text('#include "hello.h"\nint main(void){return HELLO-1;}\n', encoding="utf-8")
        (src / "CMakeLists.txt").write_text(
            PROJECT.replace("${ICON}", ICON.as_posix()), encoding="utf-8")
        build = src / "build dir"
        env = dict(os.environ)
        env.setdefault("CC", cc)
        generator = ["-G", "Ninja"] if shutil.which("ninja") else []
        configure = subprocess.run(
            [cmake, "-S", str(src), "-B", str(build), *generator, f"-DCMAKE_RC_COMPILER={Path(rc).as_posix()}"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
        if configure.returncode:
            print(configure.stdout[-3000:], configure.stderr[-3000:])
            print("FAIL: configure")
            return 1
        built = subprocess.run(
            [cmake, "--build", str(build)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
        if built.returncode:
            print(built.stdout[-3000:], built.stderr[-3000:])
            print("FAIL: an icon .rc built through the isolated object library must compile under a path with spaces")
            return 1
        print(f"ok: icon resource built under a path with spaces using {Path(rc).name}")
    print("PASS: APP_ICON resource compile survives spaces in the project path")
    return 0


if __name__ == "__main__":
    sys.exit(main())
