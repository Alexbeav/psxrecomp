"""The in-game menu's launcher-Settings callbacks run on a provider's draw
thread while the game runs: they must not write runtime state (psxrecomp#674
review P1).

settings_get reads only the main-thread snapshot (mh_video_get), and the
menu's game info publishes vocabulary built on the main thread
(recomp_overlay_setup -> build_internal_resolution_vocab) instead of building
it, or reading the live video globals, itself."""
import re
from pathlib import Path
from test_netplay_load_apply_flow import function_body, require

source = (Path(__file__).resolve().parents[1] / "src/main.cpp").read_text(encoding="utf-8")
WRITE = re.compile(r"\bg_\w+\s*(?:\.\w+\s*)?(?:=(?!=)|\+\+|--|\+=|-=)")
STATEFUL = ("psx_video_current", "psx_sdl_display_pixel_height",
            "build_internal_resolution_vocab", "internal_resolution_for_launcher",
            "session_host_look", "psx_host_enh_")

for sig in ("void mh_launcher_settings_get(RecompLauncherCSettings* out)",
            "void mh_launcher_game_info(RecompLauncherCGameInfo* out)"):
    body = function_body(source, sig)
    m = WRITE.search(body)
    require(m is None, f"{sig}: writes runtime state ({m.group(0) if m else ''})")
    for name in STATEFUL:
        require(name not in body, f"{sig}: calls {name}")

gi = function_body(source, "void mh_launcher_game_info(RecompLauncherCGameInfo* out)")
require("gi.internal_resolution_labels = g_ir_labels" in gi,
        "menu game info publishes the main-thread Internal resolution vocabulary")
setup = function_body(source, "static void recomp_overlay_setup(const char* argv0, const std::string& title)")
require("build_internal_resolution_vocab(internal_resolution_for_launcher())" in setup,
        "the vocabulary is built on the main thread before a provider attaches")
print("menu launcher callbacks: draw-thread safe PASS")
