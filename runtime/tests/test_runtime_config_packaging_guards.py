#!/usr/bin/env python3
"""Pin portable game.toml resolution/staging and fail-visible GL pause OSD."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(source: str, needle: str, message: str) -> None:
    if needle not in source:
        raise AssertionError(message)


def main() -> int:
    main_cpp = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
    gl = (ROOT / "runtime/src/gpu_gl_renderer.c").read_text(encoding="utf-8")
    cmake = (ROOT / "runtime/runtime.cmake").read_text(encoding="utf-8")

    resolver_start = main_cpp.index(
        "static std::filesystem::path resolve_existing_runtime_path"
    )
    resolver_end = main_cpp.index("static std::filesystem::path sidecar_cfg_path")
    resolver = main_cpp[resolver_start:resolver_end]
    require(
        resolver,
        "if (p.is_absolute())\n"
        "        return fs::exists(p, ec) ? fs::absolute(p, ec) : fs::path{};",
        "absolute config paths are not validated explicitly",
    )
    if "if (fs::exists(p, ec))" in resolver:
        raise AssertionError("relative game config still probes cwd before exe-dir")
    require(resolver, "const fs::path root = exe_dir_from_argv(argv0);",
            "relative config is not anchored to the executable")

    for needle, message in (
        ("set(_psxrt_game_config_source \"\")",
         "build-time config source is not separated"),
        ("set(_psxrt_game_config_runtime \"\")",
         "portable runtime config name is not separated"),
        ("PSX_DEFAULT_GAME_CONFIG_PATH=\"${_psxrt_game_config_runtime}\"",
         "executable still bakes the source-tree config path"),
        ("COMMAND ${CMAKE_COMMAND} -E copy_if_different",
         "game config is not staged after build"),
        ("$<TARGET_FILE_DIR:${target}>/${_psxrt_game_config_runtime}",
         "game config is not staged beside the executable"),
    ):
        require(cmake, needle, message)

    hold_start = gl.index("int gl_renderer_present_hold_last(void)")
    hold_end = gl.index("void gl_renderer_present_vram", hold_start)
    hold = gl[hold_start:hold_end]
    if "!s_hold_tex)\n        return 0;" in hold:
        raise AssertionError("GL pause still drops OSD when no hold texture exists")
    require(hold, "if (s_hold_kind == HOLD_NONE || !s_hold_tex)",
            "GL pause lacks no-frame OSD fallback")
    require(hold, "gl_swap_with_osd();",
            "GL pause fallback does not composite/present host OSD")

    print("PASS: game config is portable/staged and GL pause OSD is visible")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
