#!/usr/bin/env python3
"""Keep explicit MinGW runtime roots ahead of ambient shell runtimes."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUNDLER = ROOT / "tools" / "bundle_mingw_dlls.sh"


def main() -> None:
    text = BUNDLER.read_text(encoding="utf-8")
    start = text.index("find_dll_src()")
    end = text.index("# True if exe or any PE already in dest imports dll", start)
    function = text[start:end]

    runtime = function.index('for d in "${RUNTIME_BINS[@]+')
    search = function.index('for d in "${SEARCH_DIRS[@]+')
    ambient = function.index('"/mingw64/bin/${name}"')

    assert runtime < search < ambient
    assert "Explicit runtime roots are a dependency contract" in function
    print("MinGW DLL preference guard: PASS")


if __name__ == "__main__":
    main()
