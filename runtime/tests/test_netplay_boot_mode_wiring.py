#!/usr/bin/env python3
"""Guard that a netplay match boots with the kit's BIOS setting (PS1B-382).

The BIOS boot mode is resolved per player: the kit's game.toml, then the
player's settings.toml, then the launcher. Two peers of one build that
differed (a settings.toml left by an older build is enough) started a match
and forked for good at sim 22. main.cpp now hands the kit's three values to
the BIOS plan in a netplay session. The rule itself is tested in
test_netplay_exit_reason.c; a match needs sockets and a second peer, so that
main.cpp keeps the kit's values, applies them only in a session and feeds the
plan from them is checked in the source.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

# The kit's values are kept when game.toml is read, before anything can change them.
loaded = MAIN.index("bios_hle      = gc.runtime.bios_hle;")
kept = MAIN[loaded : loaded + 400]
for line in ("kit_fast_boot = fast_boot;", "kit_bios_hle  = bios_hle;",
             "kit_bios_hle_keep_intro = bios_hle_keep_intro;"):
    assert line in kept, f"the kit's value is not kept at config load: {line}"
# settings.toml and the launcher come after it. The two places are found by what
# they assign, not by their exact line: another change may add to those lines
# (the settings line gained a flag in h/settings-keep-kit-bios-hle).
applied = re.search(r"if \(us\.has_bios_hle\)\s*\{?\s*bios_hle\s*=\s*us\.bios_hle;", MAIN)
chosen = re.search(r"(?m)^\s*bios_hle\s*=\s*seed\.bios_hle;", MAIN)
assert applied and chosen, "settings.toml or the launcher no longer sets bios_hle where this test looks"
assert loaded < applied.start() < chosen.start(), "settings.toml and the launcher no longer follow the config load"
for name in ("kit_fast_boot", "kit_bios_hle", "kit_bios_hle_keep_intro"):
    assert re.search(rf"bool\s+{name}\s*=\s*false;", MAIN), f"{name} must default to the runtime's own default"
    assigned = re.findall(rf"(?m)^\s*{name}\s*=[^=]", MAIN)
    assert len(assigned) == 1, f"{name} must be set at config load only: {assigned}"

# One BIOS plan per session, built after the session reboot label, so a second
# match in one process is settled again.
plans = [m.start() for m in re.finditer(r"psx_bios_hle_plan\(req\)", MAIN)]
assert len(plans) == 1, "expected one BIOS plan call"
plan = plans[0]
assert MAIN.index("session_reboot:") < plan, "the BIOS plan is not built per session"
block = MAIN[MAIN.rfind("NetplayBootMode boot = {", 0, plan) : plan]
assert block.startswith("NetplayBootMode boot = {"), "the plan is not built from a per-session boot mode"
assert re.search(r"NetplayBootMode boot = \{ bios_hle \? 1 : 0, bios_hle_keep_intro \? 1 : 0,\s*fast_boot \? 1 : 0 \};",
                 block), "the boot mode must start from the player's three values"

# In a session, and only then, the kit's values replace them, and the player is told.
settle = block.index("netplay_boot_mode_settle(&kit, &boot)")
guard = block.rfind("if (net_cfg.enabled) {", 0, settle)
assert guard >= 0, "the kit's values must apply in a netplay session only"
assert re.search(r"const NetplayBootMode kit = \{ kit_bios_hle \? 1 : 0,\s*kit_bios_hle_keep_intro \? 1 : 0,\s*"
                 r"kit_fast_boot \? 1 : 0 \};", block[guard:settle]), "the session does not take the kit's three values"
told = block[settle : settle + 260]
assert "NETPLAY_BOOT_MODE_NOTICE" in told and "host_osd_push(NETPLAY_BOOT_MODE_NOTICE" in told, (
    "a player who had chosen otherwise is not told"
)

# The plan is fed from the settled mode, not from the player's variables, and
# the player's variables are not written: the choice still applies offline.
tail = block[settle:]
for field, source in (("bios_hle", "boot.bios_hle"), ("keep_intro", "boot.keep_intro"), ("fast_boot", "boot.fast_boot")):
    assert re.search(rf"req\.{field}\s*=\s*{re.escape(source)};", tail), f"the plan's {field} is not the settled value"
assert not re.search(r"(?m)^\s*(bios_hle|bios_hle_keep_intro|fast_boot)\s*=[^=]", block), (
    "the session must not overwrite the player's own setting"
)
# The test overrides come after the session rule.
assert tail.index('std::getenv("PSX_BIOS_HLE")') > 0 and "boot.bios_hle = (e[0]" in tail, (
    "PSX_BIOS_HLE must stay an override on top of the session rule"
)

print("netplay boot mode wiring test: PASS")
