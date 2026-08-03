#!/usr/bin/env python3
"""Source guard for neutral keyboard input outside the active game window."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")


def body(name: str, next_marker: str) -> str:
    start = SOURCE.index(name)
    end = SOURCE.index(next_marker, start)
    return SOURCE[start:end]


def main() -> int:
    keyboard = body("static uint16_t pad_from_keyboard", "static bool source_is_stick_axis")
    sticks = body("static void pad_sticks_for", "static bool controller_stick_active")
    hybrid = body("static bool hybrid_dpad_active", "/* Sample each player's live device")
    turbo = body("/* Turbo mode:", "/* Turbo-through-loads")

    for guarded in (keyboard, sticks, hybrid, turbo):
        assert "g_hidden_window" in guarded
        assert "SDL_WINDOW_INPUT_FOCUS" in guarded

    assert "return 0xFFFF" in keyboard
    assert "out[0] = out[1] = out[2] = out[3] = 0x80" in sticks
    print("host input focus guards: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
