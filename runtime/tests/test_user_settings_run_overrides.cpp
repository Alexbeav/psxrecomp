/* --set / --fullscreen / --windowed (PS1B-687): run-only settings.toml
 * overrides. load_user_settings of the registered file sees them; a save of
 * that file keeps each overridden key at its on-disk value (or absent), while
 * every other change is saved as before. Other files are untouched. */
#include "config_loader.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace PSXRecompV4;

static std::string read_text(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

int main() {
    const fs::path dir = fs::temp_directory_path() /
        ("run-overrides-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{dir};
    const fs::path settings = dir / "settings.toml";
    const fs::path other = dir / "other.toml";
    const char* text = "[video]\nrenderer = \"opengl\"\nfullscreen = 0\n\n[audio]\nvolume = 20\n";
    { std::ofstream f(settings); f << text; }
    { std::ofstream f(other); f << text; }

    std::string why;
    assert(!add_user_settings_run_override("bogus.key=1", &why) && why.find("unknown") != std::string::npos);
    assert(!add_user_settings_run_override("volume=1", &why));
    assert(!add_user_settings_run_override("audio.volume", &why));
    assert(add_user_settings_run_override("video.fullscreen=2", &why));
    assert(add_user_settings_run_override("video.fullscreen=1", &why));  /* later wins */
    assert(add_user_settings_run_override("audio.volume=64", &why));
    assert(add_user_settings_run_override("video.vsync=adaptive", &why)); /* bare text */
    assert(add_user_settings_run_override("controller.p2_device=\"keyboard\"", &why));
    set_user_settings_run_override_path(settings);

    UserSettings us = load_user_settings(settings);
    assert(us.has_fullscreen && us.fullscreen == 1);
    assert(us.has_volume && us.volume == 64);
    assert(us.has_vsync && us.vsync == -1);
    assert(us.has_p_device[1] && us.p_device[1] == "keyboard");
    assert(us.has_renderer && us.renderer == 1);
    UserSettings untouched = load_user_settings(other);
    assert(untouched.has_fullscreen && untouched.fullscreen == 0 && untouched.volume == 20);

    /* A save changes a key nobody overrode; the overridden keys keep the file's values. */
    us.renderer = 0;
    assert(save_user_settings(settings, us));
    clear_user_settings_run_overrides();
    const UserSettings disk = load_user_settings(settings);
    assert(disk.has_renderer && disk.renderer == 0);
    assert(disk.has_fullscreen && disk.fullscreen == 0);
    assert(disk.has_volume && disk.volume == 20);
    assert(!disk.has_vsync);
    assert(!disk.has_p_device[1]);

    /* An absent file: the overrides are seen, and a save writes none of them. */
    const fs::path fresh = dir / "fresh" / "settings.toml";
    assert(add_user_settings_run_override("video.fullscreen=1", &why));
    set_user_settings_run_override_path(fresh);
    UserSettings f = load_user_settings(fresh);
    assert(f.has_fullscreen && f.fullscreen == 1);
    assert(save_user_settings(fresh, f));
    const std::string written = read_text(fresh);
    assert(written.find("fullscreen") == std::string::npos);
    clear_user_settings_run_overrides();
    assert(!load_user_settings(fresh).has_fullscreen);

    /* Without overrides a save writes what it is given, as before. */
    f.fullscreen = 2;
    assert(save_user_settings(fresh, f));
    assert(load_user_settings(fresh).fullscreen == 2);

    std::puts("user_settings_run_overrides_test: PASS");
    return 0;
}
