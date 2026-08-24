#!/usr/bin/env python3
"""Structural guards for the shared PSX runtime-overlay seam."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
CDROM = (ROOT / "runtime/src/cdrom.c").read_text(encoding="utf-8")
PANEL = (ROOT / "runtime/src/psx_savestate_menu.c").read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    a = source.index(start)
    b = source.index(end, a)
    return source[a:b]


features = body(MAIN, "config.features =", "config.window_scale_max")
assert "RECOMP_RUNTIME_UI_STANDARD_FULLSCREEN" in features
assert "RECOMP_RUNTIME_UI_STANDARD_TEXTURE_FILTER" in features
assert "RECOMP_RUNTIME_UI_STANDARD_VOLUME" in features
assert "RECOMP_RUNTIME_UI_STANDARD_RESOLUTION_SCALE" not in features, (
    "supersampling must stay hidden until renderer-target recreation exists"
)
assert "RECOMP_RUNTIME_UI_STANDARD_VIEW_MODE" not in features, (
    "PSX widescreen/view mode is mod-owned"
)

swap = body(CDROM, "int cdrom_replace_disc", "uint32_t cdrom_read")
assert "psx_netplay_active()" in swap
assert swap.index("replacement = iso_open") < swap.index("previous = iso_handle")
assert swap.index("previous = iso_handle") < swap.index("iso_close(previous)")
assert "debug_force_cd_reinsert();" in swap

action = body(MAIN, "static int runtime_ui_change_disc", "static int runtime_ui_run_action")
assert "resolve_disc_path" in action
assert "identify_disc" in action
assert "cdrom_replace_disc" in action
assert "write_cached_path" not in action
assert "save_user_settings" not in action

assert "recomp_runtime_ui_render_argb8888" in PANEL

print("PASS: runtime overlay live/recreate and session-disc guards")
