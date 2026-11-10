/* mouse_settings_test — settings.toml plumbing for the PS1 Mouse (PS1B-279).
 *
 * What this pins:
 *
 *   1. with no mouse keys, nothing is set and the defaults are sensitivity 1.0
 *      and capture on;
 *   2. pN_device = "mouse" survives load and save as an ordinary device string;
 *   3. mouse_sensitivity accepts a float or an integer in 0.05..10.0 and
 *      ignores anything outside it; mouse_capture reads true/false;
 *   4. save_user_settings round-trips both keys, so a launcher save keeps them.
 */
#include "config_loader.h"

#include <cmath>
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

static void test_absent() {
    PSXRecompV4::UserSettings fresh;
    check(!fresh.has_mouse_sensitivity && !fresh.has_mouse_capture,
          "a fresh UserSettings has no mouse opinion");
    check(fresh.mouse_sensitivity == 1.0 && fresh.mouse_capture,
          "defaults are sensitivity 1.0 and capture on");

    fs::path p = write_temp("psxrecomp_mouse_absent.toml",
        "[controller]\n"
        "p1_device = \"keyboard\"\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.parse_error, "settings.toml parses");
    check(!us.has_mouse_sensitivity && !us.has_mouse_capture,
          "absent mouse keys stay absent");
    fs::remove(p);
}

static void test_values() {
    fs::path p = write_temp("psxrecomp_mouse_values.toml",
        "[controller]\n"
        "p1_device = \"keyboard\"\n"
        "p2_device = \"mouse\"\n"
        "mouse_sensitivity = 0.5\n"
        "mouse_capture = false\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(us.has_p_device[1] && us.p_device[1] == "mouse",
          "p2_device = \"mouse\" is read as the device string");
    check(us.has_mouse_sensitivity && std::fabs(us.mouse_sensitivity - 0.5) < 1e-9,
          "float mouse_sensitivity is read");
    check(us.has_mouse_capture && !us.mouse_capture,
          "mouse_capture = false is read");
    fs::remove(p);

    fs::path i = write_temp("psxrecomp_mouse_int.toml",
        "[controller]\n"
        "mouse_sensitivity = 2\n");
    auto us_i = PSXRecompV4::load_user_settings(i);
    check(us_i.has_mouse_sensitivity && us_i.mouse_sensitivity == 2.0,
          "integer mouse_sensitivity is read");
    fs::remove(i);

    fs::path bad = write_temp("psxrecomp_mouse_range.toml",
        "[controller]\n"
        "mouse_sensitivity = 20.0\n");
    auto us_b = PSXRecompV4::load_user_settings(bad);
    check(!us_b.parse_error, "an out-of-range value does not fail the file");
    check(!us_b.has_mouse_sensitivity && us_b.mouse_sensitivity == 1.0,
          "out-of-range mouse_sensitivity is ignored");
    fs::remove(bad);
}

static void test_round_trip() {
    PSXRecompV4::UserSettings out;
    out.p_device[0] = "mouse";      out.has_p_device[0] = true;
    out.mouse_sensitivity = 1.25;   out.has_mouse_sensitivity = true;
    out.mouse_capture = false;      out.has_mouse_capture = true;

    fs::path p = fs::temp_directory_path() / "psxrecomp_mouse_roundtrip.toml";
    check(PSXRecompV4::save_user_settings(p, out), "save_user_settings writes");
    auto back = PSXRecompV4::load_user_settings(p);
    check(!back.parse_error, "written settings.toml re-parses");
    check(back.has_p_device[0] && back.p_device[0] == "mouse",
          "p1_device = mouse survives a round trip");
    check(back.has_mouse_sensitivity && std::fabs(back.mouse_sensitivity - 1.25) < 1e-9,
          "mouse_sensitivity survives a round trip");
    check(back.has_mouse_capture && !back.mouse_capture,
          "mouse_capture survives a round trip");
    fs::remove(p);
}

int main() {
    test_absent();
    test_values();
    test_round_trip();
    if (failures) {
        std::fprintf(stderr, "mouse_settings_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS: [controller] PS1 Mouse settings plumbing\n");
    return 0;
}
