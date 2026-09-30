/* portable_settings_paths_test
 *
 * A portable game folder must keep working after it is moved or copied to
 * another PC. The launcher saves the disc, BIOS and memory-card paths the
 * player used; saved as absolute paths they pinned the folder to its first
 * location. What this pins:
 *
 *   1. a path inside the settings folder is written relative to it;
 *   2. a path outside it (a NAS disc, another drive) stays absolute;
 *   3. the relative form reads back unchanged, so the reader's exe-directory
 *      anchoring resolves it on the new PC.
 */
#include "config_loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using PSXRecompV4::relative_to_folder;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main() {
    const fs::path game = fs::temp_directory_path() / "psx portable paths test" / "game";
    std::error_code ec;
    fs::remove_all(game.parent_path(), ec);
    fs::create_directories(game / "inputs" / "disc-1");
    fs::create_directories(game.parent_path() / "elsewhere");

    const fs::path disc = game / "inputs" / "disc-1" / "Game (Europe).chd";
    const fs::path outside = game.parent_path() / "elsewhere" / "bios.bin";

    check(relative_to_folder(disc, game).generic_string() == "inputs/disc-1/Game (Europe).chd",
          "a path inside the folder becomes relative");
    check(relative_to_folder(outside, game) == outside, "a path outside the folder stays absolute");
    check(relative_to_folder(fs::path("inputs/x.bin"), game) == fs::path("inputs/x.bin"),
          "an already relative path is unchanged");
    check(relative_to_folder(fs::path(), game).empty(), "an empty path stays empty");

    PSXRecompV4::UserSettings s{};
    s.bios_path = game / "inputs" / "[EU] SCPH5552.BIN"; s.has_bios_path = true;
    s.disc_path = disc; s.has_disc_path = true;
    s.memcard_dir = game / "saves"; s.has_memcard_dir = true;
    s.memcard1_path = game / "saves" / "card1.mcd"; s.has_memcard1_path = true;
    s.memcard2_path = outside; s.has_memcard2_path = true;
    const fs::path settings = game / "settings.toml";
    check(PSXRecompV4::save_user_settings(settings, s), "settings save");

    std::stringstream text;
    text << std::ifstream(settings).rdbuf();
    const std::string body = text.str();
    check(body.find("path = \"inputs/[EU] SCPH5552.BIN\"") != std::string::npos, "BIOS saved relative");
    check(body.find("path = \"inputs/disc-1/Game (Europe).chd\"") != std::string::npos, "disc saved relative");
    check(body.find("dir     = \"saves\"") != std::string::npos, "memory card folder saved relative");
    check(body.find("card1   = \"saves/card1.mcd\"") != std::string::npos, "card 1 saved relative");
    check(body.find(outside.generic_string()) != std::string::npos, "outside card stays absolute");

    const PSXRecompV4::UserSettings back = PSXRecompV4::load_user_settings(settings);
    check(back.has_disc_path && back.disc_path == fs::path("inputs/disc-1/Game (Europe).chd"),
          "relative disc path reads back unchanged");
    check(back.has_memcard1_path && back.memcard1_path == fs::path("saves/card1.mcd"),
          "relative card path reads back unchanged");

    fs::remove_all(game.parent_path(), ec);
    if (failures) return 1;
    std::printf("portable_settings_paths_test: all checks passed\n");
    return 0;
}
