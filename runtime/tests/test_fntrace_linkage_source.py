#!/usr/bin/env python3
"""Keep C++ callers bound to fntrace.h's C-linkage declaration."""

from pathlib import Path
import re
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    source = (root / "runtime/src/main.cpp").read_text(encoding="utf-8")

    if '#include "fntrace.h"' not in source:
        raise AssertionError("main.cpp must include the fntrace ABI header")
    if re.search(r"^\s*extern\s+int\s+fntrace_is_game_started\s*\(", source, re.MULTILINE):
        raise AssertionError(
            "main.cpp redeclares fntrace_is_game_started outside its C-linkage header"
        )
    if "fntrace_is_game_started()" not in source:
        raise AssertionError("main.cpp no longer exercises the game-start ABI")

    print("fntrace C-linkage source guard: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
