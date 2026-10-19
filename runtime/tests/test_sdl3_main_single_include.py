#!/usr/bin/env python3
"""Keep SDL3's single-header entry-point implementation in main.cpp only."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
SDL_MAIN_HEADER = "<SDL3/SDL_main.h>"
# Stand-alone test programs with their own main(). Each is a separate
# executable built with SDL_MAIN_HANDLED, so it never shares a link with
# main.cpp's entry point.
STANDALONE_TEST_PROGRAMS = {"runtime/tests/test_window_fullscreen.cpp"}


def main() -> int:
    runtime = ROOT / "runtime"
    owners = []
    # Sources only: a build tree under runtime/ (the documented runtime/build
    # layout) holds generated binary-ish headers such as vk_shaders_spv.h.
    for tree in ("src", "include", "tests"):
        for pattern in ("*.c", "*.cpp", "*.h"):
            for path in (runtime / tree).rglob(pattern):
                source = path.read_text(encoding="utf-8")
                if SDL_MAIN_HEADER in source:
                    rel = path.relative_to(ROOT).as_posix()
                    if rel not in STANDALONE_TEST_PROGRAMS:
                        owners.append(rel)

    # The header's implementation is per executable. The app's entry point is
    # runtime/src/main.cpp and nothing under src/ or include/ may pull it in
    # (any of those would be linked into the app next to main.cpp). A test may
    # include it only as the entry-point TU of its own standalone executable:
    # the file defines main() itself, and is not linked into the app.
    standalone = []
    for owner in list(owners):
        if owner.startswith("runtime/tests/"):
            text = (ROOT / owner).read_text(encoding="utf-8")
            if "int main(" in text:
                standalone.append(owner)
                owners.remove(owner)
    if owners != ["runtime/src/main.cpp"]:
        raise AssertionError(
            "SDL_main.h must be included by runtime/src/main.cpp and, outside "
            "tests with their own main(), nowhere else; "
            f"found {owners}"
        )

    main_cpp = (runtime / "src/main.cpp").read_text(encoding="utf-8")
    guard = (
        "#if defined(PSX_SDL3)\n"
        "/*\n"
        " * SDL_main.h is a single-header implementation"
    )
    if guard not in main_cpp:
        raise AssertionError("SDL_main.h include is no longer guarded by PSX_SDL3")

    print("PASS: SDL3 entry-point implementation has one owner")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
