/* video_enhancement_settings_test — [video] geometry_correction /
 * perspective_texturing plumbing.
 *
 * These two knobs are the opt-in for the sub-pixel vertex precision and
 * perspective-correct UV enhancements (psxrecomp issue #92). The underlying
 * GTE/GPU machinery has its own unit coverage in the runtime suite; what this
 * test pins is the part that decides whether it is ever switched on:
 *
 *   1. both default OFF (the faithful floor) when game.toml says nothing;
 *   2. game.toml [video] turns them on;
 *   3. settings.toml (the player's file) parses them;
 *   4. save_user_settings round-trips them — a launcher save must not silently
 *      drop a hand-edited key, which would look like "the setting does nothing".
 */
#include "config_loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static fs::path write_temp(const std::string& name, const std::string& body) {
    fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
    return p;
}

/* game.toml needs the fields load_game_config() requires; keep this to the
 * documented minimum plus whichever [video] body the case under test needs.
 *
 * [runtime] is required even when empty: parse_runtime_block() early-returns
 * on a config with no [runtime] table, so a game.toml carrying [video] alone
 * would silently ignore every video key. Every shipped game.toml has both. */
static fs::path write_game_toml(const std::string& name,
                                const std::string& video_block) {
    return write_temp(name,
        "[game]\n"
        "name = \"probe\"\n"
        "exe = \"probe.exe\"\n"
        "load_address = \"0x80010000\"\n"
        "entry_pc = \"0x80010000\"\n"
        "text_size = \"0x1000\"\n"
        "[recompiler]\n"
        "seeds = \"seeds.json\"\n"
        "[runtime]\n"
        + video_block);
}

static void test_defaults_off() {
    fs::path p = write_game_toml("psxrecomp_pgxp_default.toml", "");
    auto gc = PSXRecompV4::load_game_config(p);
    check(!gc.runtime.video_geometry_correction,
          "geometry_correction defaults off (faithful floor)");
    check(!gc.runtime.video_perspective_texturing,
          "perspective_texturing defaults off (faithful floor)");
    fs::remove(p);
}

static void test_game_toml_opt_in() {
    fs::path p = write_game_toml("psxrecomp_pgxp_on.toml",
        "[video]\n"
        "geometry_correction = true\n"
        "perspective_texturing = true\n");
    auto gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_geometry_correction,
          "[video] geometry_correction = true is honoured");
    check(gc.runtime.video_perspective_texturing,
          "[video] perspective_texturing = true is honoured");
    fs::remove(p);
}

/* The two knobs are independent: a title may want stable geometry without
 * changing texture mapping (or the reverse) — so a single flag would be wrong. */
static void test_knobs_independent() {
    fs::path p = write_game_toml("psxrecomp_pgxp_geom_only.toml",
        "[video]\n"
        "geometry_correction = true\n");
    auto gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_geometry_correction,
          "geometry_correction alone turns on");
    check(!gc.runtime.video_perspective_texturing,
          "geometry_correction alone leaves perspective_texturing off");
    fs::remove(p);
}

static void test_user_settings_read() {
    fs::path p = write_temp("psxrecomp_pgxp_settings.toml",
        "[video]\n"
        "geometry_correction = true\n"
        "perspective_texturing = false\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.parse_error, "settings.toml parses");
    check(us.has_geometry_correction && us.geometry_correction,
          "settings.toml geometry_correction = true read");
    check(us.has_perspective_texturing && !us.perspective_texturing,
          "settings.toml perspective_texturing = false read (explicit off)");
    fs::remove(p);
}

/* An absent key must stay absent, so layering leaves the game.toml value alone
 * instead of forcing it off. */
static void test_user_settings_absent_key() {
    fs::path p = write_temp("psxrecomp_pgxp_settings_empty.toml",
        "[video]\n"
        "supersampling = 2\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.has_geometry_correction,
          "absent geometry_correction leaves has_* false");
    check(!us.has_perspective_texturing,
          "absent perspective_texturing leaves has_* false");
    fs::remove(p);
}

static void test_user_settings_round_trip() {
    PSXRecompV4::UserSettings out;
    out.geometry_correction = true;   out.has_geometry_correction = true;
    out.perspective_texturing = true; out.has_perspective_texturing = true;

    fs::path p = fs::temp_directory_path() / "psxrecomp_pgxp_roundtrip.toml";
    check(PSXRecompV4::save_user_settings(p, out), "save_user_settings writes");

    auto back = PSXRecompV4::load_user_settings(p);
    check(!back.parse_error, "written settings.toml re-parses");
    check(back.has_geometry_correction && back.geometry_correction,
          "geometry_correction survives a save/load round trip");
    check(back.has_perspective_texturing && back.perspective_texturing,
          "perspective_texturing survives a save/load round trip");
    fs::remove(p);
}

int main() {
    test_defaults_off();
    test_game_toml_opt_in();
    test_knobs_independent();
    test_user_settings_read();
    test_user_settings_absent_key();
    test_user_settings_round_trip();

    if (failures) {
        std::fprintf(stderr, "video_enhancement_settings_test: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf("PASS: [video] geometry_correction / perspective_texturing "
                "plumbing\n");
    return 0;
}
