#pragma once

#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace PSXRecompV4 {
inline std::string disc_roster_fold(std::string value) {
    for (char& c : value) c = (char)std::toupper((unsigned char)c);
    return value;
}

// Keep directories in the identity. Same-directory container alternatives
// retain the existing CUE/BIN behavior; two cache directories never collide.
inline std::string disc_roster_key(std::filesystem::path path) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    if (!ec) path = absolute;
    path = path.lexically_normal();
    path.replace_extension();
    std::string key = path.generic_string();
#ifdef _WIN32
    key = disc_roster_fold(key);
#endif
    return key;
}

inline int disc_roster_index(const std::vector<std::filesystem::path>& roster,
                             const std::filesystem::path& disc) {
    if (disc.empty()) return -1;
    const auto key = disc_roster_key(disc);
    for (size_t i = 0; i < roster.size(); ++i)
        if (disc_roster_key(roster[i]) == key) return (int)i;

    // Relocated copies remain supported only when the name identifies one
    // roster entry. An ambiguous basename provides no disc identity.
    const auto stem = disc_roster_fold(disc.stem().string());
    int match = -1;
    for (size_t i = 0; i < roster.size(); ++i) {
        if (disc_roster_fold(roster[i].stem().string()) != stem) continue;
        if (match != -1) return -1;
        match = (int)i;
    }
    return match;
}

inline std::filesystem::path disc_roster_selected(
    const std::vector<std::filesystem::path>& roster, int selected_1based,
    const std::filesystem::path& persisted) {
    if (roster.size() < 2) return persisted;
    const int idx = selected_1based - 1;
    if (idx < 0 || idx >= (int)roster.size()) return persisted;
    return disc_roster_index(roster, persisted) == idx ? persisted : roster[idx];
}

// --- An image the set's disc list does not know ------------------------------
//
// A copy of one of the set's discs under another name or in another folder,
// or another game's disc. No entry of the list matches it, so no serial is
// expected for it, and nothing was checked: the launcher called it verified
// and lit PLAY (PS1B-403). It is judged by the serial read from it against
// every serial of the set.
//
// Only a set that lists a serial for every one of its discs can judge: with a
// serial missing, a copy of that disc would be called wrong. A disc from which
// no serial was read is "not listed" for the row, and the caller decides what
// that means for the verdict.
enum class DiscSetSerial { NotJudged, Listed, NotListed };

inline DiscSetSerial disc_roster_judge_serial(const std::vector<std::string>& serials,
                                              size_t discs_in_set,
                                              const std::string& read) {
    if (serials.empty() || serials.size() < discs_in_set) return DiscSetSerial::NotJudged;
    for (const std::string& one : serials)
        if (one.empty()) return DiscSetSerial::NotJudged;
    const std::string got = disc_roster_fold(read);
    for (const std::string& one : serials)
        if (!got.empty() && disc_roster_fold(one) == got) return DiscSetSerial::Listed;
    return DiscSetSerial::NotListed;
}

// Every serial of the set, for the sentence: "SLUS-00544, SLUS-00556".
inline std::string disc_roster_serial_list(const std::vector<std::string>& serials) {
    std::string all;
    for (const std::string& one : serials) {
        if (one.empty()) continue;
        if (!all.empty()) all += ", ";
        all += one;
    }
    return all;
}

// --- A set whose discs boot different programs ------------------------------
//
// Each program is its own build, and the builds share one folder, one
// settings.toml and one saves folder. Every program's config lists the whole
// set; `program_discs` holds the 1-based set positions the program boots.
// An empty list means "every disc": a single-disc title or a one-program set.

// True when the program boots the disc at this 1-based set position.
inline bool disc_roster_program_owns(const std::vector<int>& program_discs,
                                     int set_position_1based) {
    if (program_discs.empty()) return true;
    for (int owned : program_discs)
        if (owned == set_position_1based) return true;
    return false;
}

// The program's own discs, in `program_discs` order. Positions outside the
// set are skipped; the config loader refuses them before this runs.
inline std::vector<std::filesystem::path> disc_roster_program_subset(
    const std::vector<std::filesystem::path>& set_roster,
    const std::vector<int>& program_discs) {
    if (program_discs.empty()) return set_roster;
    std::vector<std::filesystem::path> own;
    for (int position : program_discs)
        if (position >= 1 && position <= (int)set_roster.size())
            own.push_back(set_roster[position - 1]);
    return own;
}

// Set position (1-based) of the program's own disc number `own_1based`, for
// the save-state disc token. Two programs of a set can share an entry point
// (Rival Schools' Arcade and Evolution discs both enter at 0x80010008), so the
// token must come from the set, not from the program's own list. Returns 0
// when the title is not a program of a set, which keeps today's naming.
inline int disc_roster_program_set_position(
    const std::vector<int>& program_discs, int own_1based) {
    if (program_discs.empty()) return 0;
    if (own_1based < 1 || own_1based > (int)program_discs.size())
        return program_discs.front();
    return program_discs[own_1based - 1];
}

// True when `disc` is a roster entry of the set that another program boots.
// A path that is not in the set roster is not judged here (the caller may
// decide by the image's serial).
inline bool disc_roster_program_foreign(
    const std::vector<std::filesystem::path>& set_roster,
    const std::vector<int>& program_discs,
    const std::filesystem::path& disc) {
    if (program_discs.empty()) return false;
    const int idx = disc_roster_index(set_roster, disc);
    return idx >= 0 && !disc_roster_program_owns(program_discs, idx + 1);
}

// --- Changing disc while the game runs ---------------------------------------
//
// A game that declares a set of discs accepts only those discs: a state and a
// replay are named for the disc in the drive, and a disc from outside the set
// has no name. A title that declares no set keeps accepting any disc.

// The 1-based set position the picked image may be mounted as, or 0 to refuse.
// `image_carries(serial)` says whether the picked image is the disc with that
// serial. A roster entry that names a serial is matched by the serial alone,
// wherever the file sits; an entry without one is matched by its path. A disc
// that another program of the set boots is refused. When several entries name
// the serial the image carries, the entry at the picked path wins, then the
// first.
template <class ImageCarries>
inline int disc_roster_change_position(
    const std::vector<std::filesystem::path>& set_roster,
    const std::vector<std::string>& set_serials,
    const std::vector<int>& program_discs,
    const std::filesystem::path& picked, ImageCarries&& image_carries) {
    const int idx = disc_roster_index(set_roster, picked);
    int by_serial = 0;
    for (size_t i = 0; i < set_roster.size(); ++i) {
        if (i >= set_serials.size() || set_serials[i].empty()) continue;
        if (!image_carries(set_serials[i])) continue;
        if (!by_serial || (int)i == idx) by_serial = (int)i + 1;
    }
    if (by_serial)
        return disc_roster_program_owns(program_discs, by_serial) ? by_serial : 0;
    if (idx < 0) return 0;
    if ((size_t)idx < set_serials.size() && !set_serials[idx].empty()) return 0;
    return disc_roster_program_owns(program_discs, idx + 1) ? idx + 1 : 0;
}

inline std::string disc_roster_value(
    const std::vector<std::filesystem::path>& roster,
    const std::vector<std::string>& values, const std::filesystem::path& disc,
    const std::string& fallback) {
    const int idx = disc_roster_index(roster, disc);
    return idx >= 0 && (size_t)idx < values.size() && !values[idx].empty()
        ? values[idx] : fallback;
}

// The serial the launcher's disc check expects of the image at `disc`.
//
// A build with no serial list expects its own serial (`fallback`) of every
// image. A build with a list expects, of an image that IS in its disc list,
// that entry's serial; of an image the list does not know it expects nothing
// ("") -- disc_roster_outside_list() then judges that image by what was read.
//
// A kit as Studio builds it for a single-disc game carries a list of ONE entry
// (`discs` and `disc_serials` of one), so it takes the list path: any other
// file the player selects is "an image the list does not know".
inline std::string disc_roster_expected_serial(
    const std::vector<std::filesystem::path>& roster,
    const std::vector<std::string>& serials, const std::vector<int>& program_discs,
    const std::filesystem::path& disc, const std::string& fallback) {
    if (serials.empty()) return fallback;
    if (disc_roster_program_foreign(roster, program_discs, disc)) return fallback;
    return disc_roster_value(roster, serials, disc, "");
}

// The judgement of an image for which no serial was expected: a readable image
// outside the disc list of a build that lists a serial for every disc. `read`
// is the serial found on the image. NotJudged for every other image; those are
// judged by the serial that was expected of them.
inline DiscSetSerial disc_roster_outside_list(
    const std::vector<std::filesystem::path>& roster,
    const std::vector<std::string>& serials, const std::filesystem::path& disc,
    bool serial_expected, bool readable, const std::string& read) {
    if (serial_expected || !readable || disc_roster_index(roster, disc) >= 0)
        return DiscSetSerial::NotJudged;
    return disc_roster_judge_serial(serials, roster.size(), read);
}

// Wrong disc: a serial was read and it is none the build lists. An image from
// which no serial was read keeps the verdict it had.
inline bool disc_roster_wrong_disc(DiscSetSerial judgement, const std::string& read) {
    return judgement == DiscSetSerial::NotListed && !read.empty();
}
} // namespace PSXRecompV4
