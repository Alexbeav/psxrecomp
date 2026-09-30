/* negcon_settings_test — settings.toml plumbing for the neGcon pad type.
 *
 * What this pins:
 *
 *   1. pN_mode = "negcon" (any case) reads as PAD_MODE_NEGCON, and a save
 *      writes "negcon" back, so a launcher save keeps it;
 *   2. game.toml modes stay analog or digital: pad_mode_from_string gives the
 *      fallback for "negcon";
 *   3. the existing values are unchanged: analog 1, digital 2, and a
 *      persisted "hybrid" still migrates to analog.
 */
#include "config_loader.h"

#include <cstdio>
#include <cstring>
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

static void test_values() {
    using namespace PSXRecompV4;
    check(PAD_MODE_ANALOG == 1 && PAD_MODE_DIGITAL == 2 && PAD_MODE_NEGCON == 3,
          "pad mode values: analog 1, digital 2, negcon 3");
    check(pad_mode_from_settings_string("negcon", PAD_MODE_ANALOG) == PAD_MODE_NEGCON,
          "settings \"negcon\" is the neGcon mode");
    check(pad_mode_from_settings_string("NeGcon", PAD_MODE_ANALOG) == PAD_MODE_NEGCON,
          "settings mode names ignore case");
    check(pad_mode_from_settings_string("hybrid", PAD_MODE_DIGITAL) == PAD_MODE_ANALOG,
          "a persisted hybrid still migrates to analog");
    check(pad_mode_from_string("negcon", PAD_MODE_DIGITAL) == PAD_MODE_DIGITAL,
          "game.toml does not take negcon (falls back)");
    check(std::strcmp(pad_mode_to_string(PAD_MODE_NEGCON), "negcon") == 0,
          "neGcon writes as \"negcon\"");
    check(std::strcmp(pad_mode_to_string(PAD_MODE_DIGITAL), "digital") == 0 &&
          std::strcmp(pad_mode_to_string(PAD_MODE_ANALOG), "analog") == 0,
          "analog and digital write as before");
}

static void test_file() {
    fs::path p = write_temp("psxrecomp_negcon_values.toml",
        "[controller]\n"
        "p1_device = \"keyboard\"\n"
        "p1_mode = \"negcon\"\n"
        "p2_mode = \"digital\"\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.parse_error, "settings.toml parses");
    check(us.has_p_mode[0] && us.p_mode[0] == PSXRecompV4::PAD_MODE_NEGCON,
          "p1_mode = \"negcon\" is read");
    check(us.has_p_mode[1] && us.p_mode[1] == PSXRecompV4::PAD_MODE_DIGITAL,
          "p2_mode = \"digital\" is unchanged");
    fs::remove(p);
}

static void test_round_trip() {
    PSXRecompV4::UserSettings out;
    out.p_device[0] = "gamepad";                     out.has_p_device[0] = true;
    out.p_mode[0] = PSXRecompV4::PAD_MODE_NEGCON;    out.has_p_mode[0] = true;

    fs::path p = fs::temp_directory_path() / "psxrecomp_negcon_roundtrip.toml";
    check(PSXRecompV4::save_user_settings(p, out), "save_user_settings writes");
    auto back = PSXRecompV4::load_user_settings(p);
    check(!back.parse_error, "written settings.toml re-parses");
    check(back.has_p_mode[0] && back.p_mode[0] == PSXRecompV4::PAD_MODE_NEGCON,
          "p1_mode = negcon survives a round trip");
    fs::remove(p);
}

int main() {
    test_values();
    test_file();
    test_round_trip();
    if (failures) {
        std::fprintf(stderr, "negcon_settings_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS: [controller] neGcon pad-type settings plumbing\n");
    return 0;
}
