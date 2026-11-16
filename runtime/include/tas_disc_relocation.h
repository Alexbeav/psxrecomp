#ifndef PSX_TAS_DISC_RELOCATION_H
#define PSX_TAS_DISC_RELOCATION_H
/* Explicit native-resume resource mapping. The original configuration stays
 * byte-identical; route admission still independently checks every disc. */
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace psx_tas_disc_relocation {
using Path = std::filesystem::path;
using Digest = std::function<std::string(const Path&)>;
inline bool hex64(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
inline bool load(const Path& file, const std::string& game_sha,
                 const std::string& route_sha, const std::vector<Path>& original,
                 const Path& boot, const Digest& digest, std::vector<Path>& result) {
    /* Same eight-disc and 4096-byte path bounds as input_route_session. */
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(file, ec);
    if (ec || bytes > 1024u + 8u * (2u * 4096u + 80u) || original.empty() || original.size() > 8 ||
        !hex64(game_sha) || !hex64(route_sha) || boot.empty()) return false;
    std::ifstream input(file, std::ios::binary);
    std::string line;
    if (!std::getline(input, line) || line != "PSX-ROSTER-RELOCATION-1" ||
        !std::getline(input, line) || line != game_sha ||
        !std::getline(input, line) || line != route_sha ||
        !std::getline(input, line) || line != std::to_string(original.size())) return false;
    std::vector<Path> moved;
    std::set<Path> old_seen, new_seen;
    for (size_t i = 0; i < original.size(); ++i) {
        if (!std::getline(input, line)) return false;
        std::istringstream fields(line);
        if (std::count(line.begin(), line.end(), '\t') != 3 ||
            line.find('\0') != std::string::npos) return false;
        std::string ordinal, hash, old_path, new_path, extra;
        if (!std::getline(fields, ordinal, '\t') || !std::getline(fields, hash, '\t') ||
            !std::getline(fields, old_path, '\t') || !std::getline(fields, new_path, '\t') ||
            std::getline(fields, extra, '\t') || ordinal != std::to_string(i + 1) ||
            !hex64(hash) || old_path.empty() || new_path.empty() ||
            old_path.size() >= 4096 || new_path.size() >= 4096 ||
            old_path.find_first_of("\r\n") != std::string::npos ||
            new_path.find_first_of("\r\n") != std::string::npos) return false;
        const auto before = std::filesystem::u8path(old_path).lexically_normal();
        const auto after = std::filesystem::u8path(new_path).lexically_normal();
        if (before != original[i].lexically_normal() || !after.is_absolute() ||
            !old_seen.insert(before).second || !new_seen.insert(after).second ||
            digest(after) != hash) return false;
        moved.push_back(after);
    }
    if (std::getline(input, line) || moved.front() != boot.lexically_normal()) return false;
    result = std::move(moved);  // Commit only after the entire mapping validates.
    return true;
}
}
#endif
