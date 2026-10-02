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
    // A kit as Studio builds it for a single-disc game (PS1B-403, Alex's look
    // of 2026-10-02): game.toml carries `discs` and `disc_serials` of ONE entry
    // beside `id`. It takes the list path, so every other file the player
    // selects is "an image the list does not know". Before that image was
    // judged, nothing was expected of it: another game's disc got a tick and
    // PLAY was lit. The lines below are the launcher's own decisions
    // (ae_disc_verify), with that kit's lists and Alex's two files.
    {
        struct Kit { const char* disc; const char* other; const char* copy; const char* renamed; bool native; };
        const Kit kits[] = {
            // as game.toml has them on Windows
            {"Z:\\Emulators\\PS1 Games\\Diablo (Europe) (En,Fr,De,Sv).chd",
             "Z:\\Emulators\\PS1 Games\\Alien Resurrection (Europe) (En,Fr,De,Es,It).chd",
             "D:\\My discs\\diablo-pal.chd",
             "D:\\My discs\\Diablo (Europe) (En,Fr,De,Sv).cue",
#if defined(_WIN32)
             true},
#else
             false},   // a backslash is not a separator here: the folder is part of the name
#endif
            {"/games/ps1/Diablo (Europe) (En,Fr,De,Sv).chd",
             "/games/ps1/Alien Resurrection (Europe) (En,Fr,De,Es,It).chd",
             "/home/player/diablo-pal.chd",
             "/home/player/Diablo (Europe) (En,Fr,De,Sv).cue", true},
        };
        const std::vector<std::string> one_serial_list = {"SLES-01156"};
        const std::vector<int> no_programs;
        const std::string id = "SLES-01156";
        for (const Kit& kit : kits) {
            const std::vector<std::filesystem::path> one_disc = {kit.disc};
            // What the launcher decides for `file` when `read` is the serial on it.
            struct Decision { std::string expected; DiscSetSerial outside; bool wrong; };
            const auto decide = [&](const char* file, const std::string& read, bool readable) {
                Decision d;
                d.expected = disc_roster_expected_serial(one_disc, one_serial_list, no_programs, file, id);
                d.outside = disc_roster_outside_list(one_disc, one_serial_list, file,
                                                     !d.expected.empty(), readable, read);
                d.wrong = disc_roster_wrong_disc(d.outside, read);
                return d;
            };
            // Another game's disc: nothing is expected of it by the list, so it
            // is judged by the serial read. Not listed, the wrong disc.
            Decision d = decide(kit.other, "SLES-02913", true);
            CHECK(d.expected.empty());
            CHECK(d.outside == DiscSetSerial::NotListed);
            CHECK(d.wrong);
            CHECK(disc_roster_serial_list(one_serial_list) == "SLES-01156");   // the sentence's "needs"
            // The kit's own image: its entry's serial is expected, as before.
            d = decide(kit.disc, "SLES-01156", true);
            CHECK(d.expected == "SLES-01156" && d.outside == DiscSetSerial::NotJudged && !d.wrong);
            // A copy of the right disc under another name: listed, accepted.
            d = decide(kit.copy, "SLES-01156", true);
            CHECK(d.expected.empty() && d.outside == DiscSetSerial::Listed && !d.wrong);
            // The same file name in another folder is the list's image: judged
            // by its entry, so another game's disc under that name is refused
            // by the expected serial (identify_disc compares it).
            d = decide(kit.renamed, "SLES-02913", true);
            if (kit.native)
                CHECK(d.expected == "SLES-01156" && d.outside == DiscSetSerial::NotJudged);
            else   // then it is an image outside the list, and refused as one
                CHECK(d.expected.empty() && d.wrong);
            // No serial read (an audio CD, a data disc that is not a game):
            // the row says not listed; the verdict stays what it was.
            d = decide(kit.other, "", true);
            CHECK(d.outside == DiscSetSerial::NotListed && !d.wrong);
            // A file that did not open says nothing more: its ISO header row
            // is the reason, and the verdict is already "bad".
            d = decide(kit.other, "", false);
            CHECK(d.outside == DiscSetSerial::NotJudged && !d.wrong);
        }
        // A build with no list at all expects its own serial of every image.
        const std::vector<std::filesystem::path> none;
        const std::vector<std::string> no_list;
        CHECK(disc_roster_expected_serial(none, no_list, no_programs, kits[0].other, id) == id);
        CHECK(disc_roster_outside_list(none, no_list, kits[0].other, true, true, "SLES-02913")
              == DiscSetSerial::NotJudged);
        // A program of a set: the other program's disc is judged against this
        // program's own serial, as before.
        const std::vector<std::string> re2 = {"SLUS-00748", "SLUS-00756"};
        const std::vector<std::filesystem::path> re2_discs = {"d/RE2 (Disc 1).chd", "d/RE2 (Disc 2).chd"};
        CHECK(disc_roster_expected_serial(re2_discs, re2, {1}, re2_discs[1], "SLUS-00748") == "SLUS-00748");
        CHECK(disc_roster_expected_serial(re2_discs, re2, {1}, re2_discs[0], "SLUS-00748") == "SLUS-00748");
        CHECK(disc_roster_expected_serial(re2_discs, re2, {}, re2_discs[1], "SLUS-00748") == "SLUS-00756");
    }

    // A kit that lists a serial in the boot file's spelling. In its own slot
    // the disc check finds that spelling on the disc and accepts it; a copy of
    // the same disc under another name was compared letter by letter with the
    // serial read ("SLUS-00594") and refused. Both sides now take one form.
    CHECK(disc_roster_serial_key("SLUS_005.94") == "SLUS-00594");
    CHECK(disc_roster_serial_key("slus-00594") == "SLUS-00594");
    CHECK(disc_roster_serial_key("SLUS_005.94;1") == "SLUS-00594");
    CHECK(disc_roster_serial_key("SLUS 00594") == "SLUS-00594");
    CHECK(disc_roster_serial_key("SLUS-0059") == "SLUS-0059");     // not four letters and five digits: as written
    CHECK(disc_roster_serial_key("12345-ABCD") == "12345-ABCD");
    CHECK(disc_roster_serial_key("demo") == "DEMO");
    CHECK(disc_roster_serial_key("").empty());
    CHECK(disc_roster_judge_serial({"SLUS_005.94"}, 1, "SLUS-00594") == DiscSetSerial::Listed);
    CHECK(disc_roster_judge_serial({"SLUS_005.94"}, 1, "SLUS-00595") == DiscSetSerial::NotListed);
    CHECK(disc_roster_judge_serial({"slus_005.94", "SLUS_006.26"}, 2, "SLUS-00626") == DiscSetSerial::Listed);
    CHECK(disc_roster_judge_serial({"SLUS_005.94", "SLUS_006.26"}, 2, "SLES-02913") == DiscSetSerial::NotListed);
    CHECK(disc_roster_judge_serial({"SLUS_005.94"}, 1, "") == DiscSetSerial::NotListed);
    {
        // The whole decision for such a kit: the copy is listed, another game's disc is the wrong disc.
        const std::vector<std::filesystem::path> one_disc = {"/games/ps1/Game (USA).chd"};
        const std::vector<std::string> exe_spelling = {"SLUS_005.94"};
        CHECK(disc_roster_expected_serial(one_disc, exe_spelling, {}, "/games/ps1/Game (USA).chd", "SLUS-00594")
              == "SLUS_005.94");   // its own slot: the spelling the disc check searches the disc for
        const DiscSetSerial copy = disc_roster_outside_list(one_disc, exe_spelling, "/elsewhere/copy.chd",
                                                            false, true, "SLUS-00594");
        CHECK(copy == DiscSetSerial::Listed && !disc_roster_wrong_disc(copy, "SLUS-00594"));
        const DiscSetSerial other = disc_roster_outside_list(one_disc, exe_spelling, "/elsewhere/other.chd",
                                                             false, true, "SLES-02913");
        CHECK(other == DiscSetSerial::NotListed && disc_roster_wrong_disc(other, "SLES-02913"));
    }
    // The sentence shows the list in the form the disc's own serial is shown in.
    CHECK(disc_roster_serial_list({"SLUS_005.94", "slus-00626"}) == "SLUS-00594, SLUS-00626");
    CHECK(disc_roster_serial_list({"demo"}) == "demo");

    // A set that does not list a serial for every disc cannot judge: a copy of
    // the unlisted disc would be called wrong. Nor can a build with no list at
    // all: it expects its own serial and never comes here.
    CHECK(disc_roster_judge_serial({"SLUS-00544", ""}, 2, "SLES-03398") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({"SLUS-00544"}, 2, "SLES-03398") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({"SLUS-00544"}, 2, "SLUS-00544") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({}, 0, "SLES-03398") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({}, 2, "") == DiscSetSerial::NotJudged);

    if (failures) return 1;
    std::puts("PASS duplicate paths, serial/fingerprint identity, index selection, relocation controls, program sets, disc changes and images outside the set");
}
