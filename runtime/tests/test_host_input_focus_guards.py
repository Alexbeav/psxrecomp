#!/usr/bin/env python3
"""Source guard for neutral keyboard input outside the active game window.

Every keyboard pad reader must read the keys as released while another window
has input focus (PS1B-208). Headless and no-window runs keep reading the key
state: host_hotkey_input_focused() reports focus when there is no window.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")


def body(name: str, next_marker: str) -> str:
    start = SOURCE.index(name)
    end = SOURCE.index(next_marker, start)
    return SOURCE[start:end]


def main() -> int:
    focused = body("static bool host_hotkey_input_focused", "static int manual_fast_forward_multiplier")
    assert "if (!sdl_window) return true;" in focused
    assert "SDL_WINDOW_INPUT_FOCUS" in focused

    keyboard = body("static uint16_t pad_from_keyboard", "static bool source_is_stick_axis")
    sticks = body("static void pad_sticks_for", "static bool controller_stick_active")
    dpad = body("static bool controller_policy_dpad_active", "static int controller_policy_resolve_mode")
    capture = body("static int capture_pad_slot(", "static int capture_pad_slot_exclusive")

    resume = body("static int savestate_resume_inputs_held", "static int savestate_input_guard_active")
    rewind = body("static void rewind_poll_nav", "static void rewind_pause_present")

    for name, guarded in (("pad_from_keyboard", keyboard), ("pad_sticks_for", sticks),
                          ("controller_policy_dpad_active", dpad), ("capture_pad_slot", capture)):
        assert "host_hotkey_input_focused()" in guarded, name
        # The guard must come before the key array is read.
        assert guarded.index("host_hotkey_input_focused()") < guarded.index("SDL_GetKeyboardState"), name

    assert "if (!host_hotkey_input_focused()) return 0xFFFF;" in keyboard
    assert "out[0] = out[1] = out[2] = out[3] = 0x80" in sticks
    assert "if (src.keybinds && host_hotkey_input_focused())" in dpad
    assert "if (src.keybinds && host_hotkey_input_focused())" in capture
    # Host UI readers: the guard gates every direct key read.
    assert "if (keys && host_hotkey_input_focused())" in resume
    assert "const int kb = host_hotkey_input_focused() ? 1 : 0;" in rewind
    for key in ("LEFT", "RIGHT"):
        assert f"(kb && keys[SDL_SCANCODE_{key}])" in rewind, key
    assert "(kb && (keys[SDL_SCANCODE_RETURN]" in rewind
    assert "(kb && (keys[SDL_SCANCODE_ESCAPE]" in rewind
    # The hold-to-turbo read was already gated; keep it that way.
    assert "const bool kb_turbo = host_hotkey_input_focused() &&" in SOURCE

    # Every SDL_GetKeyboardState reader in main.cpp is one of the guarded sites.
    readers = SOURCE.count("SDL_GetKeyboardState(NULL)")
    assert readers == 7, readers
    print("host input focus guards: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
