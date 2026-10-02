#!/usr/bin/env python3
"""Guard how bios_hle and fast_boot pass between settings.toml and main() (PS1B-360).

settings.toml overrides game.toml. The launcher has no control for bios_hle or
fast_boot, yet main() marked both present in the settings it hands to the
launcher, so every save wrote them with whatever value was in force. That line
then pinned the value: a kit that later changed bios_hle never reached a player
who had started an older build once, and two netplay peers of one build could
boot in different BIOS modes. It is the same latch as turbo_loads.

recompiler/tests/boot_keys_settings_test.cpp proves the file side: a file from
before settings_format 2 has its two keys dropped, a format 2 file keeps them.
This proves main() does its part:

  1. it applies the keys only when the loader says the file set them, and
     remembers that;
  2. it marks them present for the launcher's save only in that case;
  3. it says once that an old file's lines were ignored.

Reading source rather than running a binary is deliberate: the code is in the
middle of main(), around a launcher round-trip, and no headless start saves
settings.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# Column alignment in main.cpp is not part of the contract: one space everywhere.
MAIN = re.sub(r"[ \t]+", " ", (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8"))
LOADER = (ROOT / "recompiler" / "src" / "config_loader.cpp").read_text(encoding="utf-8")

# ---- 1. applied only when the file set them, and remembered -----------------
for key in ("fast_boot", "bios_hle"):
    assert MAIN.count(f"settings_chose_{key} = true;") == 1, (
        f"settings_chose_{key} must be set at exactly one place, the "
        "settings.toml read"
    )
    assert (
        f"if (us.has_{key}) {{ {key} = us.{key}; settings_chose_{key} = true; }}" in MAIN
    ), f"{key} from settings.toml is no longer applied and remembered in one step"

# ---- 2. marked present for the launcher's save only in that case ------------
for key in ("fast_boot", "bios_hle"):
    assert f"seed.has_{key} = settings_chose_{key};" in MAIN, (
        f"the launcher's settings mark {key} present without the player's line"
    )
    assert f"seed.has_{key} = true" not in MAIN, (
        f"`seed.has_{key} = true` is back: every launcher save would write "
        "the key and pin it over game.toml"
    )

# ---- 3. the old file's lines are reported, once -----------------------------
assert MAIN.count("if (us.boot_keys_were_echoes)") == 1, (
    "main() must say once that an old file's bios_hle/fast_boot were ignored"
)

# ---- the file side this depends on -------------------------------------------
assert "s.settings_format < 2 && (s.has_fast_boot || s.has_bios_hle)" in LOADER, (
    "the loader no longer drops the echoed keys of a file older than format 2"
)
assert 'f << "settings_format = " << UserSettings::kFormat' in LOADER, (
    "save_user_settings no longer writes settings_format"
)

# ---- 4. the run report says it too (PS1B-400) ---------------------------------
# A product keeps no stdout, so the stdout line above reaches no player.
REPORT = (ROOT / "runtime" / "src" / "crash_trace.c").read_text(encoding="utf-8")
SCHEMA = (ROOT / "docs" / "config_schema.md").read_text(encoding="utf-8")
assert re.search(
    r"psx_crash_trace_note_settings\(us\.settings_format,\s*"
    r"PSXRecompV4::UserSettings::kFormat,\s*us\.boot_keys_were_echoes \? 1 : 0\);", MAIN
), "main() must hand the settings file's format and the dropped-keys fact to the run report"
assert (
    '"  \\"settings\\": {\\"file_format\\": %d, \\"current_format\\": %d, "' in REPORT
    and '"\\"boot_keys_ignored\\": %d, \\"notice\\": \\"%s\\"},\\n"' in REPORT
), "the run report no longer writes the settings object"
notice = re.search(r"#define PSX_SETTINGS_BOOT_KEYS_NOTICE \\\n((?:\s*\"[^\n]*\"(?: \\)?\n)+)", REPORT)
assert notice, "the notice sentence is gone"
text = "".join(re.findall(r'"([^"\n]*)"', notice.group(1)))
assert text and "\\" not in text and all(ord(c) >= 0x20 for c in text), (
    "the notice goes into a JSON string as it is: no backslash, no control character"
)
assert "settings_format = 2" in text and "ignored" in text, text
assert "### `settings_format` in the player's `settings.toml`" in SCHEMA, (
    "docs/config_schema.md must say what settings_format is for"
)

print("settings boot keys wiring: PASS")
