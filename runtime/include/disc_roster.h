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

inline std::string disc_roster_value(
    const std::vector<std::filesystem::path>& roster,
    const std::vector<std::string>& values, const std::filesystem::path& disc,
    const std::string& fallback) {
    const int idx = disc_roster_index(roster, disc);
    return idx >= 0 && (size_t)idx < values.size() && !values[idx].empty()
        ? values[idx] : fallback;
}
} // namespace PSXRecompV4
