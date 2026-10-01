/* set_config_loader_test — the setup program of a set can read its own set file.
 *
 * A set whose discs boot different programs (Resident Evil 2) is one setup
 * package with a set.toml at its root (tools/program_set.py; docs/
 * config_schema.md, "One setup package for the set"). The set's setup program
 * reads that file with load_game_config(), as every setup program reads its
 * game.toml. The loader insisted on a game's keys ([game] name, exe, the load
 * addresses, a [recompiler] table), the set file has none of them, and the
 * setup program printed "key "name" not found" to stderr and exited before its
 * window opened. A double-click showed nothing (PS1B-365, found by the first
 * hands-on start of a set package).
 *
 * What this pins:
 *   1. the set file exactly as documented loads, with the set's title as the
 *      name and both discs and serials;
 *   2. a set file that carries the game keys anyway (packages made while the
 *      loader still refused the plain form) loads with those values;
 *   3. a single-program config is held to the old rules: no name, no exe or no
 *      [recompiler] table is still an error.
 */
#include "config_loader.h"

#include <cstdio>
#include <exception>
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

static fs::path write_config(const fs::path& folder, const std::string& name,
                             const std::string& body) {
    fs::create_directories(folder);
    const fs::path p = folder / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
    return p;
}

/* The documented form, as docs/config_schema.md and tools/tests/
 * test_program_set.py have it. */
static const char kSetAsDocumented[] =
    "[set]\n"
    "name = \"resident-evil-2-usa-dual-shock\"\n"
    "title = \"Resident Evil 2\"\n"
    "exe_name = \"Resident_Evil_2\"\n"
    "bios_stem = \"SCPH1001\"\n"
    "serials = [\"SLUS-00748\", \"SLUS-00756\"]\n"
    "programs = [\"leon\", \"claire\"]\n"
    "\n"
    "[game]\n"
    "discs = [\"disc/disc-1.cue\", \"disc/disc-2.cue\"]\n"
    "disc_serials = [\"SLUS-00748\", \"SLUS-00756\"]\n"
    "\n"
    "[program.leon]\n"
    "folder = \"programs/leon\"\n"
    "exe_name = \"Resident_Evil_2_Leon\"\n"
    "shortcut = \"Resident Evil 2 - Leon\"\n"
    "serials = [\"SLUS-00748\"]\n"
    "positions = [1]\n"
    "\n"
    "[program.claire]\n"
    "folder = \"programs/claire\"\n"
    "exe_name = \"Resident_Evil_2_Claire\"\n"
    "shortcut = \"Resident Evil 2 - Claire\"\n"
    "serials = [\"SLUS-00756\"]\n"
    "positions = [2]\n";

static bool refused(const fs::path& config) {
    try {
        (void)PSXRecompV4::load_game_config(config);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

int main() {
    const fs::path folder = fs::temp_directory_path() / "psxrecomp_set_config_loader_test";
    std::error_code ignored;
    fs::remove_all(folder, ignored);

    /* 1. The documented set file. */
    try {
        const auto gc = PSXRecompV4::load_game_config(
            write_config(folder / "plain", "set.toml", kSetAsDocumented));
        check(gc.name == "Resident Evil 2", "a set file takes its name from [set] title");
        check(gc.id.empty(), "a set file has no game id: each disc has its own serial");
        check(gc.discs.size() == 2, "a set file gives the setup program both discs");
        check(gc.disc_serials.size() == 2 && gc.disc_serials[0] == "SLUS-00748" &&
                  gc.disc_serials[1] == "SLUS-00756",
              "a set file gives the serial of each disc");
        check(gc.exe_path.empty(), "a set file names no exe");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "FAIL: the documented set file was refused: %s\n", ex.what());
        ++failures;
    }

    /* 2. A set file that carries the game keys too. */
    try {
        std::string with_keys = kSetAsDocumented;
        const std::string game_table = "[game]\n";
        with_keys.insert(with_keys.find(game_table) + game_table.size(),
            "name = \"Resident Evil 2 (keys)\"\n"
            "exe = \"programs/leon/disc/SLUS_007.48\"\n"
            "load_address = \"0x80010000\"\n"
            "entry_pc = \"0x80078408\"\n"
            "text_size = \"0x000F0800\"\n"
            "stack_base = \"0x801FFFF0\"\n");
        with_keys += "\n[recompiler]\nseeds = \"programs/leon/seeds/ghidra_funcs.txt\"\n";
        const auto gc = PSXRecompV4::load_game_config(
            write_config(folder / "keys", "set.toml", with_keys));
        check(gc.name == "Resident Evil 2 (keys)", "a [game] name in a set file wins over the title");
        check(gc.entry_pc == 0x80078408u, "the game keys of a set file are read when present");
        check(gc.text_size == 0x000F0800u, "text_size of a set file is read when present");
        check(gc.discs.size() == 2, "the keys do not change the set's discs");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "FAIL: a set file with game keys was refused: %s\n", ex.what());
        ++failures;
    }

    /* 3. A single program is held to the old rules. */
    const std::string game_head =
        "[game]\n"
        "name = \"probe\"\n"
        "exe = \"probe.exe\"\n"
        "load_address = \"0x80010000\"\n"
        "entry_pc = \"0x80010000\"\n"
        "text_size = \"0x1000\"\n";
    check(!refused(write_config(folder / "game", "game.toml",
                                game_head + "[recompiler]\nseeds = \"seeds.json\"\n")),
          "a complete single-program config loads");
    check(refused(write_config(folder / "noname", "game.toml",
                               "[game]\nexe = \"probe.exe\"\nload_address = \"0x80010000\"\n"
                               "text_size = \"0x1000\"\n[recompiler]\nseeds = \"seeds.json\"\n")),
          "a single-program config without a name is refused");
    check(refused(write_config(folder / "noexe", "game.toml",
                               "[game]\nname = \"probe\"\nload_address = \"0x80010000\"\n"
                               "text_size = \"0x1000\"\n[recompiler]\nseeds = \"seeds.json\"\n")),
          "a single-program config without an exe is refused");
    check(refused(write_config(folder / "norecompiler", "game.toml", game_head)),
          "a single-program config without a [recompiler] table is refused");
    check(refused(write_config(folder / "noseeds", "game.toml",
                               game_head + "[recompiler]\nout_dir = \"generated\"\n")),
          "a single-program config without seeds is refused");

    fs::remove_all(folder, ignored);
    if (failures) {
        std::fprintf(stderr, "set_config_loader_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("set_config_loader_test: PASS (the documented set file loads; a single program is held to the old rules)\n");
    return 0;
}
