#!/usr/bin/env python3
"""Keep the MinGW product binary free of volatile build metadata."""

from pathlib import Path


root = Path(__file__).resolve().parents[2]
runtime_cmake = (root / "runtime" / "runtime.cmake").read_text(encoding="utf-8")
crash_trace = (root / "runtime" / "src" / "crash_trace.c").read_text(
    encoding="utf-8"
)

stack_option = "-Wl,--stack,67108864"
start = runtime_cmake.index(stack_option)
end = runtime_cmake.index("elseif(MSVC)", start)
mingw_link_block = runtime_cmake[start:end]

assert "-Wl,--no-insert-timestamp" in mingw_link_block
assert "-Wl,--build-id" in mingw_link_block

# The content-derived RSDS identifier is reproducible only when its linked
# inputs are reproducible. Compile-time date and time macros made the crash
# identity change on each clean build and changed the RSDS bytes in turn.
build_id_line = next(
    line for line in crash_trace.splitlines() if "kBuildId =" in line
)
assert build_id_line.strip() == "static const char *kBuildId = PSX_BUILD_REV;"
assert "__DATE__" not in build_id_line
assert "__TIME__" not in build_id_line
