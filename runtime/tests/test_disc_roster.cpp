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

    // "Change disc" while the game runs: a game that declares a set accepts
    // only the discs of that set. `image` is the serial the picked image has.
    const std::vector<std::string> set_serials = {"SCUS-94163", "SCUS-94164", "SCUS-94165"};
    const std::vector<std::string> no_serials;
    const std::vector<std::string> gap_serials = {"SCUS-94163", "", "SCUS-94165"};
    std::string image;
    const auto carries = [&image](const std::string& serial) { return serial == image; };
    image = "SCUS-94164";   // disc 2, picked from the roster path
    CHECK(disc_roster_change_position(set, set_serials, all, set[1], carries) == 2);
    // The serial decides, wherever the file sits and whatever it is called.
    CHECK(disc_roster_change_position(set, set_serials, all, "elsewhere/ff7b.chd", carries) == 2);
    image = "SLUS-00594";   // another game
    CHECK(disc_roster_change_position(set, set_serials, all, "elsewhere/other.chd", carries) == 0);
    // Another game's image at a roster path is still another game.
    CHECK(disc_roster_change_position(set, set_serials, all, set[0], carries) == 0);
    image = "";             // no readable serial (an audio CD)
    CHECK(disc_roster_change_position(set, set_serials, all, "music/album.cue", carries) == 0);
    // A program of a set changes only between its own discs.
    image = "SCUS-94164";
    CHECK(disc_roster_change_position(set, set_serials, claire, set[1], carries) == 2);
    CHECK(disc_roster_change_position(set, set_serials, leon, set[1], carries) == 0);
    image = "SCUS-94165";
    CHECK(disc_roster_change_position(set, set_serials, two, set[2], carries) == 3);
    // A roster without serials is matched by path; an entry that names a
    // serial is never matched by path alone.
    image = "";
    CHECK(disc_roster_change_position(set, no_serials, all, set[2], carries) == 3);
    CHECK(disc_roster_change_position(set, no_serials, all, "moved/Game (Disc 2).bin", carries) == 2);
    CHECK(disc_roster_change_position(set, no_serials, all, "moved/Another Game.chd", carries) == 0);
    CHECK(disc_roster_change_position(set, gap_serials, all, set[1], carries) == 2);
    CHECK(disc_roster_change_position(set, gap_serials, all, set[2], carries) == 0);
    CHECK(disc_roster_change_position(set, no_serials, leon, set[1], carries) == 0);
    // Discs that share one serial: the picked path tells them apart.
    const std::vector<std::string> one_serial = {"SLUS-00001", "SLUS-00001", "SLUS-00001"};
    image = "SLUS-00001";
    CHECK(disc_roster_change_position(set, one_serial, all, set[2], carries) == 3);
    CHECK(disc_roster_change_position(set, one_serial, all, "elsewhere/x.chd", carries) == 1);

    // An image the set's disc list does not know (PS1B-403). A copy of one of
    // the set's discs stays accepted, whatever it is called and whatever its
    // place in the list; another game's disc is not listed.
    const std::vector<std::string> two_discs = {"SLUS-00544", "SLUS-00556"};
    const std::vector<std::string> four_discs = {"SLPS-00700", "SLPS-00701", "SLPS-00702", "SLPS-00703"};
    for (const auto& set_serials : {two_discs, four_discs}) {
        // Every order of the list: the judgement does not depend on a disc's place.
        std::vector<std::string> order = set_serials;
        for (size_t turn = 0; turn < order.size(); ++turn) {
            for (const std::string& disc : set_serials) {
                CHECK(disc_roster_judge_serial(order, order.size(), disc) == DiscSetSerial::Listed);
                std::string lower = disc;
                for (char& c : lower) c = (char)std::tolower((unsigned char)c);
                CHECK(disc_roster_judge_serial(order, order.size(), lower) == DiscSetSerial::Listed);
            }
            CHECK(disc_roster_judge_serial(order, order.size(), "SLES-03398") == DiscSetSerial::NotListed);
            CHECK(disc_roster_judge_serial(order, order.size(), "SLUS-0054") == DiscSetSerial::NotListed);
            // Nothing read: not listed for the row; the caller keeps the verdict.
            CHECK(disc_roster_judge_serial(order, order.size(), "") == DiscSetSerial::NotListed);
            order.push_back(order.front());
            order.erase(order.begin());
        }
    }
    CHECK(disc_roster_serial_list(two_discs) == "SLUS-00544, SLUS-00556");
    CHECK(disc_roster_serial_list(four_discs) == "SLPS-00700, SLPS-00701, SLPS-00702, SLPS-00703");
    CHECK(disc_roster_serial_list({"SLUS-00544", ""}) == "SLUS-00544");
    // A set that does not list a serial for every disc cannot judge: a copy of
    // the unlisted disc would be called wrong. So can no single-disc build
    // (no list at all): it expects its own serial and never comes here.
    CHECK(disc_roster_judge_serial({"SLUS-00544", ""}, 2, "SLES-03398") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({"SLUS-00544"}, 2, "SLES-03398") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({"SLUS-00544"}, 2, "SLUS-00544") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({}, 0, "SLES-03398") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({}, 2, "") == DiscSetSerial::NotJudged);

    if (failures) return 1;
    std::puts("PASS duplicate paths, serial/fingerprint identity, index selection, relocation controls, program sets, disc changes and images outside the set");
}
