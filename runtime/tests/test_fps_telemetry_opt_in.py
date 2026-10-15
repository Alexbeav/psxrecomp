#!/usr/bin/env python3
"""Structural guard: FPS title/stderr telemetry stays explicitly opt-in."""

from pathlib import Path


main_cpp = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(
    encoding="utf-8"
)

required = (
    'std::getenv("PSX_FPS_TELEMETRY")',
    "fps_telemetry_enabled() && !psx_netplay_in_load_barrier()",
    "sdl_window && s_fps_base_title.empty()",
    # PS1B-468: Game is the game-drawn rate, Video the VBlank rate.
    "Game %.0f fps | Video %.0f fps | Speed %.0f%% | Display %.0f fps",
    "Game %.0f FPS | Video %.0f FPS | Speed %.0f%% | Display %.0f FPS",
)
for needle in required:
    if needle not in main_cpp:
        raise AssertionError(f"missing FPS telemetry opt-in guard: {needle}")

print("PASS: FPS telemetry remains opt-in and title-stable")
