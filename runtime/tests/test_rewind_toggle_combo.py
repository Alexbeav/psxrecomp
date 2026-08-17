#!/usr/bin/env python3
"""Guard PSX host controller shortcuts against single-button defaults."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

assert "static int           g_hotkey_pad_rewind = 1272;" in MAIN
assert "static int           g_hotkey_pad_save_state_menu = 2040;" in MAIN
assert "PSX_HOTKEY_PAD_IS_BUTTON_COMBO" in MAIN
assert "PSX_HOTKEY_PAD_SELECT_R3" in MAIN
assert "static int hotkey_pad_binding_down(int binding)" in MAIN
assert "SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_BACK)" in MAIN
assert "return hotkey_pad_binding_down(g_hotkey_pad_rewind);" in MAIN
assert "(btn & PAD_SELECT) == 0 && (btn & PAD_L3) == 0" not in MAIN
assert (
    "SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_BACK) &&\n"
    "           SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_LEFTSTICK)"
) not in MAIN
assert (
    "SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_BACK) ||\n"
    "           SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_RIGHTSTICK)"
) not in MAIN
assert "HOST_KEYMAP_SAVE_STATE_MENU" in MAIN
assert "key >= SDLK_F1 && key <= SDLK_F12" not in MAIN
assert "savestate_slot_from_function_key" not in MAIN
assert "savestate_submit_slot(f_slot" not in MAIN
assert "savestate_input_guard_arm();" in MAIN
assert "savestate_input_guard_active()" in MAIN
assert "out->buttons = 0xFFFFu;" in MAIN
assert "pad_from_keyboard(1) != 0xFFFFu" in MAIN
assert "savestate_menu_ignore_toggle_release" in MAIN
assert "savestate_menu_open_key" in MAIN
assert "savestate_menu_toggle(key)" in MAIN
assert "savestate_menu_toggle(0)" in MAIN
assert 'std::getenv("PSX_FAST_FORWARD_SPEED")' in MAIN
assert "bool manual_turbo_active = false;" in MAIN
assert "Fast forward: %dx" in MAIN
assert "g_frame_period_ms / (double)mult" in MAIN

print("host shortcut guard passed")
