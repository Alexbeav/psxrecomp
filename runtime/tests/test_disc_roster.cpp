#include "disc_roster.h"
#include <cstdio>

using namespace PSXRecompV4;
static int failures;
#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); ++failures; } } while (0)

int main() {
    const std::vector<std::filesystem::path> duplicate = {
        "cache/a/disc.cue", "cache/b/disc.cue"};
    const std::vector<std::string> serials = {"SLUS-00544", "SLUS-00556"};
    CHECK(disc_roster_index(duplicate, "cache/a/disc.bin") == 0);
    CHECK(disc_roster_index(duplicate, "cache/b/disc.bin") == 1);
    CHECK(disc_roster_index(duplicate, "cache/b/../a/disc.bin") == 0);
    CHECK(disc_roster_index(duplicate, "relocated/disc.bin") == -1);
    CHECK(disc_roster_value(duplicate, serials, "cache/a/disc.bin", "") == serials[0]);
    CHECK(disc_roster_value(duplicate, serials, "cache/b/disc.bin", "") == serials[1]);
    CHECK(disc_roster_value(duplicate, serials, "relocated/disc.bin", "") == "");
    CHECK(disc_roster_selected(duplicate, 1, "cache/b/disc.bin") == duplicate[0]);
    CHECK(disc_roster_selected(duplicate, 2, "cache/a/disc.bin") == duplicate[1]);
    CHECK(disc_roster_selected(duplicate, 2, "cache/b/disc.bin") == "cache/b/disc.bin");
    CHECK(disc_roster_selected(duplicate, 0, "custom/disc.bin") == "custom/disc.bin");
    CHECK(disc_roster_selected(duplicate, 3, "custom/disc.bin") == "custom/disc.bin");
    CHECK(disc_roster_selected({duplicate[0]}, 1, "custom/disc.bin") == "custom/disc.bin");
    const std::vector<std::filesystem::path> named = {"a/Game (Disc 1).cue", "b/Game (Disc 2).cue"};
    CHECK(disc_roster_selected(named, 2, "moved/Game (Disc 2).bin") == "moved/Game (Disc 2).bin");
    CHECK(disc_roster_index(named, "moved/Game (Disc 1).chd") == 0);
    CHECK(disc_roster_value(duplicate, {"fp1", "fp2"}, "cache/b/disc.bin", "flat") == "fp2");
    CHECK(disc_roster_value(duplicate, {"fp1"}, "cache/b/disc.bin", "flat") == "flat");
    CHECK(disc_roster_value(duplicate, {"fp1", ""}, "cache/b/disc.bin", "flat") == "flat");

    // A set whose discs boot different programs: each program is one build,
    // and every build's config lists the whole set (Resident Evil 2).
    const std::vector<std::filesystem::path> set = {
        "discs/Game (Disc 1).chd", "discs/Game (Disc 2).chd", "discs/Game (Disc 3).chd"};
    const std::vector<int> all;            // no program_discs: every disc
    const std::vector<int> leon = {1};
    const std::vector<int> claire = {2};
    const std::vector<int> two = {3, 1};   // a program with two discs of its own
    CHECK(disc_roster_program_owns(all, 1) && disc_roster_program_owns(all, 3));
    CHECK(disc_roster_program_owns(leon, 1) && !disc_roster_program_owns(leon, 2));
    CHECK(disc_roster_program_owns(claire, 2) && !disc_roster_program_owns(claire, 1));
    CHECK(disc_roster_program_subset(set, all) == set);
    CHECK(disc_roster_program_subset(set, leon) ==
          std::vector<std::filesystem::path>{set[0]});
    CHECK(disc_roster_program_subset(set, claire) ==
          std::vector<std::filesystem::path>{set[1]});
    CHECK((disc_roster_program_subset(set, two) ==
           std::vector<std::filesystem::path>{set[2], set[0]}));
    CHECK(disc_roster_program_subset(set, {7}).empty());
    // The other program's disc is foreign, by path or by a relocated unique name.
    CHECK(!disc_roster_program_foreign(set, all, set[1]));
    CHECK(disc_roster_program_foreign(set, leon, set[1]));
    CHECK(!disc_roster_program_foreign(set, leon, set[0]));
    CHECK(disc_roster_program_foreign(set, claire, "moved/Game (Disc 1).bin"));
    CHECK(!disc_roster_program_foreign(set, claire, "moved/Game (Disc 2).bin"));
    CHECK(!disc_roster_program_foreign(set, claire, "moved/Another Game.chd"));
    // Save states take the set position, so two programs with one entry point
    // never share a state file. A title that is not a program of a set keeps 0.
    CHECK(disc_roster_program_set_position(all, 1) == 0);
    CHECK(disc_roster_program_set_position(leon, 1) == 1);
    CHECK(disc_roster_program_set_position(claire, 1) == 2);
    CHECK(disc_roster_program_set_position(two, 1) == 3);
    CHECK(disc_roster_program_set_position(two, 2) == 1);
    CHECK(disc_roster_program_set_position(two, 9) == 3);
    if (failures) return 1;
    std::puts("PASS duplicate paths, serial/fingerprint identity, index selection, relocation controls and program sets");
}
