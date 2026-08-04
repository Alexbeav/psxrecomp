#!/usr/bin/env python3
"""Guard mod-owned, bounded load acceleration and its launcher migration."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime/include/mod_plugins.h").read_text(encoding="utf-8")
CONFIG_H = (ROOT / "recompiler/src/config_loader.h").read_text(encoding="utf-8")
CONFIG_CPP = (ROOT / "recompiler/src/config_loader.cpp").read_text(
    encoding="utf-8"
)

assert "psx_mod_set_load_acceleration" in HEADER
assert "wall_clock_multiplier accepts 2..16" in HEADER
assert "zero is the precise/speedrun-safe policy" in HEADER
assert "psx_mod_set_disc_speed" in HEADER
assert "this changes" in HEADER

assert "bool                  offer_turbo_loads = true;" in CONFIG_H
assert 'runtime.contains("offer_turbo_loads")' in CONFIG_CPP
assert "turbo_loads_offered = gc.runtime.offer_turbo_loads;" in MAIN
assert "gi.has_turbo_loads      = turbo_loads_offered ? 1 : 0;" in MAIN
assert "Turbo loads is mod-owned for this title" in MAIN

reset = """g_mod_load_wall_multiplier = -1;
    g_mod_load_release_frames = -1;"""
disc_reset = """g_mod_disc_speed_divisor = -1;
    g_mod_disc_instant_rate = -1;"""
disable_mod_owned_baseline = """if (!turbo_loads_offered)
        g_turbo_loads_enabled = 0;"""
activate = "mod_runtime_activate_plugins();"
apply = "if (g_mod_load_wall_multiplier >= 0) {"
assert reset in MAIN
assert disc_reset in MAIN
assert disable_mod_owned_baseline in MAIN
assert apply in MAIN
assert (
    MAIN.index(reset)
    < MAIN.index(disable_mod_owned_baseline)
    < MAIN.index(activate)
    < MAIN.index(apply)
)

assert "release_run = g_turbo_load_release_frames;" in MAIN
assert "g_frame_period_ms / (double)g_turbo_load_wall_multiplier" in MAIN
assert "if (!turbo_load_paced && g_frame_period_ms > 0.0)" in MAIN
assert "if (g_mod_disc_speed_divisor >= 0)" in MAIN

print("mod-owned load acceleration guard passed")
