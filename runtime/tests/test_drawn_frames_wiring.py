#!/usr/bin/env python3
"""PS1B-468: the game-drawn frame counter is fed by the GPU and printed by
every frame readout, release products included.

test_drawn_frames.c proves what the counter counts. This proves gpu.c feeds it
from the three places a picture changes (VBlank, GP0 writes, GP1(05h)) and that
the FPS readout, the frame report and the [BENCH] line print three figures:
video rate, game-drawn rate and speed. Before PS1B-468 all three printed only
the VBlank rate, so a 30 fps title read 60.
"""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def require(source, needle, message):
    if needle not in source:
        raise AssertionError(f"{message} (missing: {needle!r})")


def main():
    gpu = (ROOT / "runtime/src/gpu.c").read_text(encoding="utf-8")
    main_cpp = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
    cmake = (ROOT / "runtime/runtime.cmake").read_text(encoding="utf-8")

    require(cmake, "runtime/src/drawn_frames.c",
            "the counter is not linked into products")

    # The GPU feeds the counter, each behind one branch on the enabled flag.
    tick = gpu[gpu.index("void gpu_vblank_tick(void) {"):]
    tick = tick[:tick.index("lcf ^= 1;")]
    require(tick, "drawn_frames_vblank_sample();",
            "the VBlank does not sample the shown picture")
    require(gpu, "gr_backend() == GR_BACKEND_SOFTWARE ? vram : NULL",
            "the picture hash must read CPU VRAM only where it is the picture")
    execute = gpu[gpu.index("static void gp0_execute_command(void) {"):]
    execute = execute[:execute.index("switch (opcode)")]
    require(execute, "if (psx_drawn_frames_enabled())\n        drawn_frames_note_gp0(opcode);",
            "GP0 writes do not reach the counter")
    start = gpu[gpu.index("static void gp1_display_area_start(uint32_t val) {"):]
    start = start[:start.index("\n}\n")]
    require(start, "psx_drawn_frames_note_display_start(display_area_x, display_area_y);",
            "a GP1(05h) flip does not move the shown rectangle")

    # The three readouts print the three figures.
    require(main_cpp, '"[FPS] game: %.1f fps | video: %.1f fps | speed: %.0f%% | frames: %llu\\n"',
            "the [FPS] line lacks the game-drawn rate")
    require(main_cpp, '"%s  [Game %.0f fps | Video %.0f fps | Speed %.0f%%]"',
            "the window title lacks the three figures")
    require(main_cpp, '"Game %.0f FPS | Video %.0f FPS | Speed %.0f%%"',
            "the on-screen readout lacks the three figures")
    require(main_cpp, '"drawn_fps=%.1f avg_drawn_fps=%.1f speed_pct=%.1f\\n"',
            "the frame report lacks the game-drawn rate")
    require(main_cpp, '"video_fps=%.3f drawn_fps=%.3f speed_pct=%.1f "',
            "the [BENCH] line lacks the three figures")
    require(main_cpp, '"drawn_writes=+%llu drawn_changes=+%llu drawn_hashed=+%llu\\n"',
            "the [BENCH] line lacks the per-signal counts")

    # Each readout turns counting on; nothing else does.
    report = main_cpp[main_cpp.index("static void frame_report_tick(uint64_t frames) {"):]
    report = report[:report.index("\n}\n")]
    require(report, "psx_drawn_frames_set_enabled(1);",
            "the frame report does not turn the counter on")
    perf = main_cpp[main_cpp.index("static void runtime_perf_init() {"):]
    perf = perf[:perf.index("\n}\n")]
    require(perf, "psx_drawn_frames_set_enabled(1);",
            "the [BENCH] window does not turn the counter on")
    fps = main_cpp[main_cpp.index("if (fps_telemetry_enabled() && !psx_netplay_in_load_barrier()) {"):]
    fps = fps[:fps.index("stack_profile_sample();")]
    require(fps, "psx_drawn_frames_set_enabled(1);",
            "the FPS readout does not turn the counter on")

    # The readout is outside every debug-tools guard: release products print it.
    for name, block in (("frame report", report), ("FPS readout", fps)):
        if "PSX_NO_DEBUG_TOOLS" in block or "PSX_DEBUG_TOOLS" in block:
            raise AssertionError(f"the {name} is behind a debug-tools guard")

    print("PASS: the game-drawn counter is fed by the GPU and printed by every readout")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, ValueError) as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
