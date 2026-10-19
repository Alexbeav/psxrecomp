"""The in-game menu's preset callbacks run on a provider's draw thread while
the game runs: they must not write runtime state (psxrecomp#674 review P1).

quality_apply is a pure preset -> rows mapping, re-detect returns this
session's detection, settings_get reads only the main-thread snapshot, and
the menu's game info never publishes the pre-game launcher's stateful
callbacks (quality_fill_game_info: quality_launcher_redetect, string
publication)."""
import re
from pathlib import Path
from test_netplay_load_apply_flow import function_body, require

source = (Path(__file__).resolve().parents[1] / "src/main.cpp").read_text(encoding="utf-8")
WRITE = re.compile(r"\bg_\w+\s*(?:\.\w+\s*)?(?:=(?!=)|\+\+|--|\+=|-=)")
STATEFUL = ("quality_apply_video_globals", "quality_publish_strings", "psxq::decide",
            "quality_launcher_redetect", "psx_video_current", "quality_fill_game_info",
            "psx_sdl_display_pixel_height")

for sig in ("psxq::Rows quality_rows_for(const PSXRecompV4::RuntimeConfig& r)",
            "void quality_rows_to(const psxq::Rows& r, RecompLauncherCSettings* s)",
            "bool quality_preset_rows(int tier, psxq::Rows* out)",
            "void quality_launcher_apply(int preset, RecompLauncherCSettings* s)",
            "int mh_quality_redetect(void)",
            "void mh_launcher_settings_get(RecompLauncherCSettings* out)",
            "void mh_launcher_game_info(RecompLauncherCGameInfo* out)"):
    body = function_body(source, sig)
    m = WRITE.search(body)
    require(m is None, f"{sig}: writes runtime state ({m.group(0) if m else ''})")
    for name in STATEFUL:
        require(name not in body, f"{sig}: calls {name}")

gi = function_body(source, "void mh_launcher_game_info(RecompLauncherCGameInfo* out)")
require("gi.quality_apply = quality_launcher_apply" in gi and
        "gi.quality_redetect = mh_quality_redetect" in gi,
        "menu game info publishes the thread-safe preset callbacks")
print("menu quality callbacks: draw-thread safe PASS")
