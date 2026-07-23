#include "mod_packages.h"

#include "crc32.h"
#include "psx_sha256.h"
#include "toml.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace PSXRecompV4 {
namespace {

constexpr uint32_t kFormatVersion = 1;
constexpr uint64_t kMaxArchiveBytes = 256ull * 1024ull * 1024ull;
constexpr uint32_t kMaxArchiveFiles = 4096;

std::map<std::string, ModBuiltinResolver>& builtin_resolvers() {
    static std::map<std::string, ModBuiltinResolver> value;
    return value;
}

void set_error(std::string* out, const std::string& value) {
    if (out) *out = value;
}

bool valid_id(const std::string& value) {
    if (value.empty() || value.size() > 96) return false;
    for (unsigned char c : value) {
        if (!(std::islower(c) || std::isdigit(c) || c == '.' || c == '-' || c == '_'))
            return false;
    }
    return value.front() != '.' && value.back() != '.';
}

bool parse_hex_bytes(const std::string& text, std::vector<uint8_t>& out) {
    std::string compact;
    compact.reserve(text.size());
    for (unsigned char c : text) {
        if (!std::isspace(c) && c != '_') compact.push_back((char)c);
    }
    if (compact.size() % 2 != 0) return false;
    out.clear();
    out.reserve(compact.size() / 2);
    auto nibble = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = (unsigned char)std::tolower(c);
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < compact.size(); i += 2) {
        const int hi = nibble((unsigned char)compact[i]);
        const int lo = nibble((unsigned char)compact[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return true;
}

std::string hex_bytes(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    for (uint8_t byte : bytes)
        out << std::hex << std::setw(2) << std::setfill('0') << (unsigned)byte;
    return out.str();
}

struct SemVer {
    int64_t major = 0, minor = 0, patch = 0;
    std::string suffix;
    bool valid = false;
};

SemVer parse_semver(const std::string& text) {
    SemVer out;
    std::string core = text;
    const size_t dash = core.find('-');
    if (dash != std::string::npos) {
        out.suffix = core.substr(dash + 1);
        core.resize(dash);
    }
    std::array<int64_t*, 3> parts = {&out.major, &out.minor, &out.patch};
    size_t at = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        const size_t end = core.find('.', at);
        const std::string token = core.substr(at, end == std::string::npos ? end : end - at);
        if (token.empty() ||
            !std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c); }))
            return out;
        try {
            *parts[i] = std::stoll(token);
        } catch (...) {
            return out;
        }
        if (end == std::string::npos) {
            if (i != 2) return out;
            at = core.size();
        } else {
            at = end + 1;
        }
    }
    if (at != core.size()) return out;
    out.valid = true;
    return out;
}

int compare_semver(const std::string& a, const std::string& b) {
    const SemVer av = parse_semver(a), bv = parse_semver(b);
    if (!av.valid || !bv.valid) return a.compare(b);
    if (av.major != bv.major) return av.major < bv.major ? -1 : 1;
    if (av.minor != bv.minor) return av.minor < bv.minor ? -1 : 1;
    if (av.patch != bv.patch) return av.patch < bv.patch ? -1 : 1;
    if (av.suffix.empty() != bv.suffix.empty()) return av.suffix.empty() ? 1 : -1;
    return av.suffix.compare(bv.suffix);
}

bool version_satisfies(const std::string& actual, const std::string& requirement) {
    if (requirement.empty() || requirement == "*") return true;
    if (requirement.rfind(">=", 0) == 0)
        return compare_semver(actual, requirement.substr(2)) >= 0;
    if (requirement.rfind("<=", 0) == 0)
        return compare_semver(actual, requirement.substr(2)) <= 0;
    if (requirement.rfind(">", 0) == 0)
        return compare_semver(actual, requirement.substr(1)) > 0;
    if (requirement.rfind("<", 0) == 0)
        return compare_semver(actual, requirement.substr(1)) < 0;
    if (requirement.rfind("^", 0) == 0) {
        const SemVer base = parse_semver(requirement.substr(1));
        const SemVer got = parse_semver(actual);
        return base.valid && got.valid && got.major == base.major &&
               compare_semver(actual, requirement.substr(1)) >= 0;
    }
    return actual == requirement;
}

std::string quote_toml(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back((char)c);
    }
    out.push_back('"');
    return out;
}

bool read_file(const fs::path& path, std::vector<uint8_t>& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cannot open " + path.string());
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || (uint64_t)size > kMaxArchiveBytes) {
        set_error(error, "archive is too large");
        return false;
    }
    in.seekg(0);
    out.resize((size_t)size);
    if (!out.empty() && !in.read((char*)out.data(), size)) {
        set_error(error, "cannot read " + path.string());
        return false;
    }
    return true;
}

uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint32_t compressed_size = 0;
    uint32_t size = 0;
    uint32_t local_offset = 0;
    bool directory = false;
};

bool safe_archive_name(const std::string& name) {
    if (name.empty() || name.size() > 512 || name[0] == '/' || name[0] == '\\')
        return false;
    if (name.size() >= 2 && std::isalpha((unsigned char)name[0]) && name[1] == ':')
        return false;
    fs::path p = fs::path(name).lexically_normal();
    for (const auto& part : p) {
        const std::string s = part.string();
        if (s == ".." || s == "." || s.empty()) return false;
    }
    return true;
}

bool parse_zip(const std::vector<uint8_t>& bytes, std::vector<ZipEntry>& entries,
               std::string* error) {
    if (bytes.size() < 22) {
        set_error(error, "not a ZIP archive");
        return false;
    }
    size_t eocd = std::string::npos;
    const size_t floor = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    for (size_t pos = bytes.size() - 22;; --pos) {
        if (le32(bytes.data() + pos) == 0x06054b50u) { eocd = pos; break; }
        if (pos == floor) break;
    }
    if (eocd == std::string::npos) {
        set_error(error, "ZIP end record is missing");
        return false;
    }
    const uint16_t count = le16(bytes.data() + eocd + 10);
    const uint32_t central_size = le32(bytes.data() + eocd + 12);
    const uint32_t central_offset = le32(bytes.data() + eocd + 16);
    if (count > kMaxArchiveFiles || (uint64_t)central_offset + central_size > bytes.size()) {
        set_error(error, "ZIP central directory is invalid");
        return false;
    }
    size_t at = central_offset;
    uint64_t expanded = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + 46 > bytes.size() || le32(bytes.data() + at) != 0x02014b50u) {
            set_error(error, "ZIP entry record is invalid");
            return false;
        }
        const uint16_t flags = le16(bytes.data() + at + 8);
        ZipEntry e;
        e.method = le16(bytes.data() + at + 10);
        e.crc = le32(bytes.data() + at + 16);
        e.compressed_size = le32(bytes.data() + at + 20);
        e.size = le32(bytes.data() + at + 24);
        const uint16_t name_len = le16(bytes.data() + at + 28);
        const uint16_t extra_len = le16(bytes.data() + at + 30);
        const uint16_t comment_len = le16(bytes.data() + at + 32);
        e.local_offset = le32(bytes.data() + at + 42);
        if (flags & 1u) {
            set_error(error, "encrypted ZIP entries are not supported");
            return false;
        }
        if (e.method != 0 && e.method != 8) {
            set_error(error, "ZIP compression method is not supported");
            return false;
        }
        if (at + 46ull + name_len + extra_len + comment_len > bytes.size()) {
            set_error(error, "ZIP entry name is truncated");
            return false;
        }
        e.name.assign((const char*)bytes.data() + at + 46, name_len);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        e.directory = !e.name.empty() && e.name.back() == '/';
        if (!safe_archive_name(e.directory ? e.name.substr(0, e.name.size() - 1) : e.name)) {
            set_error(error, "unsafe ZIP path: " + e.name);
            return false;
        }
        expanded += e.size;
        if (expanded > kMaxArchiveBytes) {
            set_error(error, "expanded archive exceeds the size limit");
            return false;
        }
        entries.push_back(std::move(e));
        at += 46ull + name_len + extra_len + comment_len;
    }
    return true;
}

struct DeflateBits {
    const uint8_t* at = nullptr;
    const uint8_t* end = nullptr;
    uint64_t hold = 0;
    unsigned bits = 0;

    bool read(unsigned count, uint32_t& out) {
        while (bits < count) {
            if (at == end) return false;
            hold |= (uint64_t)*at++ << bits;
            bits += 8;
        }
        out = count == 32 ? (uint32_t)hold :
              (uint32_t)(hold & ((1ull << count) - 1));
        hold >>= count;
        bits -= count;
        return true;
    }
    void align_byte() {
        const unsigned drop = bits & 7u;
        hold >>= drop;
        bits -= drop;
    }
};

struct DeflateHuffman {
    std::array<uint16_t, 16> count{};
    std::vector<uint16_t> symbols;
};

bool build_huffman(const std::vector<uint8_t>& lengths, DeflateHuffman& out) {
    out = {};
    for (uint8_t length : lengths) {
        if (length > 15) return false;
        out.count[length]++;
    }
    if (out.count[0] == lengths.size()) return false;
    int left = 1;
    for (int length = 1; length <= 15; ++length) {
        left <<= 1;
        left -= out.count[(size_t)length];
        if (left < 0) return false;
    }
    std::array<uint16_t, 16> offsets{};
    for (size_t length = 1; length < 15; ++length)
        offsets[length + 1] = offsets[length] + out.count[length];
    out.symbols.resize(lengths.size() - out.count[0]);
    for (uint16_t symbol = 0; symbol < lengths.size(); ++symbol)
        if (lengths[symbol])
            out.symbols[offsets[lengths[symbol]]++] = symbol;
    return true;
}

bool decode_symbol(DeflateBits& bits, const DeflateHuffman& table, uint16_t& symbol) {
    uint32_t code = 0, first = 0, index = 0;
    for (uint32_t length = 1; length <= 15; ++length) {
        uint32_t bit = 0;
        if (!bits.read(1, bit)) return false;
        code |= bit;
        const uint32_t count = table.count[length];
        if (code < first + count) {
            const uint32_t slot = index + code - first;
            if (slot >= table.symbols.size()) return false;
            symbol = table.symbols[slot];
            return true;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return false;
}

bool inflate_deflate(const uint8_t* data, size_t size, size_t expected,
                     std::vector<uint8_t>& out) {
    static const uint16_t length_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,
        115,131,163,195,227,258};
    static const uint8_t length_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t distance_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
        1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t distance_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,
        12,12,13,13};
    DeflateBits input{data, data + size};
    out.clear();
    out.reserve(expected);
    bool final = false;
    while (!final) {
        uint32_t final_bit = 0, type = 0;
        if (!input.read(1, final_bit) || !input.read(2, type)) return false;
        final = final_bit != 0;
        if (type == 0) {
            input.align_byte();
            uint32_t length = 0, complement = 0;
            if (!input.read(16, length) || !input.read(16, complement) ||
                (length ^ 0xffffu) != complement ||
                out.size() + length > expected) return false;
            for (uint32_t i = 0; i < length; ++i) {
                uint32_t byte = 0;
                if (!input.read(8, byte)) return false;
                out.push_back((uint8_t)byte);
            }
            continue;
        }
        if (type == 3) return false;

        std::vector<uint8_t> literal_lengths;
        std::vector<uint8_t> distance_lengths;
        if (type == 1) {
            literal_lengths.resize(288);
            for (size_t i = 0; i <= 143; ++i) literal_lengths[i] = 8;
            for (size_t i = 144; i <= 255; ++i) literal_lengths[i] = 9;
            for (size_t i = 256; i <= 279; ++i) literal_lengths[i] = 7;
            for (size_t i = 280; i <= 287; ++i) literal_lengths[i] = 8;
            distance_lengths.assign(32, 5);
        } else {
            uint32_t hlit = 0, hdist = 0, hclen = 0;
            if (!input.read(5, hlit) || !input.read(5, hdist) ||
                !input.read(4, hclen)) return false;
            hlit += 257; hdist += 1; hclen += 4;
            static const uint8_t order[19] =
                {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::vector<uint8_t> code_lengths(19, 0);
            for (uint32_t i = 0; i < hclen; ++i) {
                uint32_t value = 0;
                if (!input.read(3, value)) return false;
                code_lengths[order[i]] = (uint8_t)value;
            }
            DeflateHuffman code_table;
            if (!build_huffman(code_lengths, code_table)) return false;
            std::vector<uint8_t> lengths;
            lengths.reserve(hlit + hdist);
            while (lengths.size() < hlit + hdist) {
                uint16_t symbol = 0;
                if (!decode_symbol(input, code_table, symbol)) return false;
                if (symbol <= 15) {
                    lengths.push_back((uint8_t)symbol);
                    continue;
                }
                uint32_t repeat = 0, extra = 0;
                uint8_t value = 0;
                if (symbol == 16) {
                    if (lengths.empty() || !input.read(2, extra)) return false;
                    repeat = extra + 3;
                    value = lengths.back();
                } else if (symbol == 17) {
                    if (!input.read(3, extra)) return false;
                    repeat = extra + 3;
                } else if (symbol == 18) {
                    if (!input.read(7, extra)) return false;
                    repeat = extra + 11;
                } else return false;
                if (lengths.size() + repeat > hlit + hdist) return false;
                lengths.insert(lengths.end(), repeat, value);
            }
            literal_lengths.assign(lengths.begin(), lengths.begin() + hlit);
            distance_lengths.assign(lengths.begin() + hlit, lengths.end());
        }
        DeflateHuffman literals, distances;
        if (!build_huffman(literal_lengths, literals) ||
            !build_huffman(distance_lengths, distances)) return false;
        for (;;) {
            uint16_t symbol = 0;
            if (!decode_symbol(input, literals, symbol)) return false;
            if (symbol < 256) {
                if (out.size() >= expected) return false;
                out.push_back((uint8_t)symbol);
                continue;
            }
            if (symbol == 256) break;
            if (symbol < 257 || symbol > 285) return false;
            const unsigned length_index = symbol - 257;
            uint32_t extra = 0;
            if (!input.read(length_extra[length_index], extra)) return false;
            const size_t length = length_base[length_index] + extra;
            uint16_t distance_symbol = 0;
            if (!decode_symbol(input, distances, distance_symbol) ||
                distance_symbol >= 30) return false;
            if (!input.read(distance_extra[distance_symbol], extra)) return false;
            const size_t distance = distance_base[distance_symbol] + extra;
            if (distance == 0 || distance > out.size() ||
                out.size() + length > expected) return false;
            for (size_t i = 0; i < length; ++i)
                out.push_back(out[out.size() - distance]);
        }
    }
    return out.size() == expected;
}

bool extract_zip(const std::vector<uint8_t>& bytes,
                 const std::vector<ZipEntry>& entries,
                 const fs::path& target, std::string* error) {
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) {
        set_error(error, "cannot create staging directory: " + ec.message());
        return false;
    }
    for (const ZipEntry& e : entries) {
        const fs::path out = target / fs::path(e.name);
        if (e.directory) {
            fs::create_directories(out, ec);
            if (ec) {
                set_error(error, "cannot create archive directory: " + ec.message());
                return false;
            }
            continue;
        }
        if ((uint64_t)e.local_offset + 30 > bytes.size() ||
            le32(bytes.data() + e.local_offset) != 0x04034b50u) {
            set_error(error, "ZIP local entry is invalid");
            return false;
        }
        const uint16_t name_len = le16(bytes.data() + e.local_offset + 26);
        const uint16_t extra_len = le16(bytes.data() + e.local_offset + 28);
        const uint64_t data_at = (uint64_t)e.local_offset + 30 + name_len + extra_len;
        if (data_at + e.compressed_size > bytes.size()) {
            set_error(error, "ZIP entry payload is invalid");
            return false;
        }
        const uint8_t* compressed = bytes.data() + data_at;
        std::vector<uint8_t> expanded;
        const uint8_t* data = compressed;
        if (e.method == 0) {
            if (e.compressed_size != e.size) {
                set_error(error, "stored ZIP entry has inconsistent size");
                return false;
            }
        } else {
            if (!inflate_deflate(compressed, e.compressed_size, e.size, expanded)) {
                set_error(error, "cannot inflate ZIP entry: " + e.name);
                return false;
            }
            data = expanded.data();
        }
        if (crc32_compute(data, e.size) != e.crc) {
            set_error(error, "ZIP entry checksum failed: " + e.name);
            return false;
        }
        fs::create_directories(out.parent_path(), ec);
        if (ec) {
            set_error(error, "cannot create archive parent directory: " + ec.message());
            return false;
        }
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        if (!file || (e.size && !file.write((const char*)data, e.size))) {
            set_error(error, "cannot extract archive entry: " + e.name);
            return false;
        }
    }
    return true;
}

const ModPackage* find_selected(
    const std::map<std::string, std::map<std::string, ModPackage>>& packages,
    const std::string& id, const ModSelection& selection) {
    const auto p = packages.find(id);
    if (p == packages.end() || p->second.empty()) return nullptr;
    if (!selection.version.empty()) {
        const auto v = p->second.find(selection.version);
        return v == p->second.end() ? nullptr : &v->second;
    }
    const ModPackage* best = nullptr;
    for (const auto& [version, package] : p->second)
        if (!best || compare_semver(version, best->version) > 0) best = &package;
    return best;
}

bool target_matches(const ModPackage& package, const std::string& game,
                    const std::string& exe, const std::string& disc) {
    if (package.targets.empty()) return false;
    for (const ModTarget& target : package.targets) {
        if (target.game_id != game) continue;
        if (!target.exe_sha256.empty() && target.exe_sha256 != exe) continue;
        if (!target.disc_sha256.empty() && target.disc_sha256 != disc) continue;
        return true;
    }
    return false;
}

std::string canonical_resolution(const std::vector<const ModPackage*>& ordered,
                                 const std::map<std::string, ModSelection>& selections,
                                 const std::vector<ModResolution::Write>& writes) {
    std::ostringstream out;
    for (const ModPackage* package : ordered) {
        out << package->id << '@' << package->version << '\n';
        const auto sit = selections.find(package->id);
        if (sit == selections.end()) continue;
        for (const auto& [key, value] : sit->second.values)
            out << key << '=' << value << '\n';
    }
    for (const ModResolution::Write& write : writes) {
        out << (write.target == ModPatchTarget::MainExe ? "main_exe" :
                write.target == ModPatchTarget::DiscRaw ? "disc_raw" : "disc_user")
            << '@' << std::hex << write.location << std::dec << ':'
            << hex_bytes(write.expected) << '>' << hex_bytes(write.replacement)
            << ':' << write.package_id << '\n';
    }
    return out.str();
}

std::string effective_option_value(const ModPackage& package,
                                   const ModSelection& selection,
                                   const std::string& id) {
    const auto selected = selection.values.find(id);
    if (selected != selection.values.end()) return selected->second;
    const auto option = std::find_if(package.options.begin(), package.options.end(),
        [&](const ModOption& item) { return item.id == id; });
    return option == package.options.end() ? std::string() : option->default_value;
}

bool writes_overlap(const ModResolution::Write& a, const ModResolution::Write& b) {
    if (a.target != b.target) return false;
    const uint64_t a_end = a.location + a.replacement.size();
    const uint64_t b_end = b.location + b.replacement.size();
    return a.location < b_end && b.location < a_end;
}

std::string fingerprint_text(const std::string& text) {
    uint8_t digest[32];
    psx_sha256_compute((const uint8_t*)text.data(), text.size(), digest);
    std::ostringstream out;
    for (uint8_t byte : digest)
        out << std::hex << std::setw(2) << std::setfill('0') << (unsigned)byte;
    return out.str();
}

} // namespace

bool mod_register_builtin_resolver(const std::string& id, ModBuiltinResolver resolver) {
    if (!valid_id(id) || !resolver) return false;
    return builtin_resolvers().emplace(id, std::move(resolver)).second;
}

void mod_clear_builtin_resolvers_for_tests() {
    builtin_resolvers().clear();
}

ModPackageManager::ModPackageManager(fs::path mods_root) : root_(std::move(mods_root)) {}

void ModPackageManager::set_root(fs::path mods_root) {
    root_ = std::move(mods_root);
    packages_.clear();
    selections_.clear();
}

bool ModPackageManager::read_manifest(const fs::path& path, ModPackage& out,
                                      std::string* error) {
    try {
        const toml::value cfg = toml::parse(path.string());
        out = {};
        out.format_version = (uint32_t)toml::find<int64_t>(cfg, "format_version");
        out.id = toml::find<std::string>(cfg, "id");
        out.version = toml::find<std::string>(cfg, "version");
        out.name = toml::find<std::string>(cfg, "name");
        out.author = cfg.contains("author") ? toml::find<std::string>(cfg, "author") : "";
        out.description =
            cfg.contains("description") ? toml::find<std::string>(cfg, "description") : "";
        out.license = cfg.contains("license") ? toml::find<std::string>(cfg, "license") : "";
        out.resolver =
            cfg.contains("resolver") ? toml::find<std::string>(cfg, "resolver") : "declarative";
        out.save_compatibility = cfg.contains("save_compatibility")
            ? toml::find<std::string>(cfg, "save_compatibility") : "shared";
        out.root = path.parent_path();
        if (out.format_version != kFormatVersion)
            throw std::runtime_error("unsupported format_version");
        if (!valid_id(out.id)) throw std::runtime_error("invalid package id");
        if (!parse_semver(out.version).valid) throw std::runtime_error("invalid semantic version");
        if (out.name.empty()) throw std::runtime_error("package name is empty");
        if (out.resolver != "declarative" && out.resolver.rfind("builtin:", 0) != 0)
            throw std::runtime_error("resolver must be declarative or builtin:<id>");
        if (out.save_compatibility != "shared" && out.save_compatibility != "isolated")
            throw std::runtime_error("save_compatibility must be shared or isolated");

        if (cfg.contains("target")) {
            for (const toml::value& v : toml::find(cfg, "target").as_array()) {
                ModTarget target;
                target.game_id = toml::find<std::string>(v, "game_id");
                target.exe_sha256 =
                    v.contains("exe_sha256") ? toml::find<std::string>(v, "exe_sha256") : "";
                target.disc_sha256 =
                    v.contains("disc_sha256") ? toml::find<std::string>(v, "disc_sha256") : "";
                if (target.game_id.empty()) throw std::runtime_error("target game_id is empty");
                out.targets.push_back(std::move(target));
            }
        }
        if (out.targets.empty()) throw std::runtime_error("package has no [[target]] entries");

        if (cfg.contains("dependency")) {
            for (const toml::value& v : toml::find(cfg, "dependency").as_array()) {
                ModRequirement dep;
                dep.id = toml::find<std::string>(v, "id");
                dep.version = v.contains("version") ? toml::find<std::string>(v, "version") : "*";
                if (!valid_id(dep.id)) throw std::runtime_error("invalid dependency id");
                out.dependencies.push_back(std::move(dep));
            }
        }
        if (cfg.contains("conflicts"))
            out.conflicts = toml::find<std::vector<std::string>>(cfg, "conflicts");
        for (const std::string& id : out.conflicts)
            if (!valid_id(id)) throw std::runtime_error("invalid conflict id");

        if (cfg.contains("option")) {
            std::set<std::string> option_ids;
            for (const toml::value& v : toml::find(cfg, "option").as_array()) {
                ModOption option;
                option.id = toml::find<std::string>(v, "id");
                option.label = toml::find<std::string>(v, "label");
                option.description =
                    v.contains("description") ? toml::find<std::string>(v, "description") : "";
                option.group = v.contains("group") ? toml::find<std::string>(v, "group") : "General";
                const std::string type = toml::find<std::string>(v, "type");
                if (!valid_id(option.id) || !option_ids.insert(option.id).second)
                    throw std::runtime_error("invalid or duplicate option id");
                if (type == "boolean") {
                    option.type = ModOptionType::Boolean;
                    option.default_value = toml::find_or<std::string>(v, "default", "false");
                    if (option.default_value != "true" && option.default_value != "false")
                        throw std::runtime_error("boolean default must be true or false");
                } else if (type == "choice") {
                    option.type = ModOptionType::Choice;
                    option.default_value = toml::find<std::string>(v, "default");
                    for (const toml::value& c : toml::find(v, "choice").as_array()) {
                        ModChoice choice;
                        choice.value = toml::find<std::string>(c, "value");
                        choice.label = toml::find<std::string>(c, "label");
                        option.choices.push_back(std::move(choice));
                    }
                    const auto found = std::find_if(option.choices.begin(), option.choices.end(),
                        [&](const ModChoice& c) { return c.value == option.default_value; });
                    if (found == option.choices.end())
                        throw std::runtime_error("choice default is not declared");
                } else if (type == "integer") {
                    option.type = ModOptionType::Integer;
                    option.min_value = toml::find<int64_t>(v, "min");
                    option.max_value = toml::find<int64_t>(v, "max");
                    option.step = toml::find_or<int64_t>(v, "step", 1);
                    const int64_t def = toml::find<int64_t>(v, "default");
                    if (option.min_value > option.max_value || option.step <= 0 ||
                        def < option.min_value || def > option.max_value)
                        throw std::runtime_error("invalid integer bounds/default");
                    option.default_value = std::to_string(def);
                } else {
                    throw std::runtime_error("unknown option type");
                }
                out.options.push_back(std::move(option));
            }
        }
        if (cfg.contains("patch")) {
            size_t declaration_index = 0;
            for (const toml::value& v : toml::find(cfg, "patch").as_array()) {
                ModPatch patch;
                const std::string target = toml::find<std::string>(v, "target");
                if (target == "main_exe") {
                    patch.target = ModPatchTarget::MainExe;
                    const int64_t address = toml::find<int64_t>(v, "address");
                    if (address < 0) throw std::runtime_error("patch address is negative");
                    patch.location = (uint64_t)address;
                } else if (target == "disc_raw" || target == "disc") {
                    patch.target = ModPatchTarget::DiscRaw;
                    const int64_t offset = toml::find<int64_t>(v, "offset");
                    if (offset < 0) throw std::runtime_error("patch offset is negative");
                    patch.location = (uint64_t)offset;
                } else if (target == "disc_user") {
                    patch.target = ModPatchTarget::DiscUser;
                    const int64_t offset = toml::find<int64_t>(v, "offset");
                    if (offset < 0) throw std::runtime_error("patch offset is negative");
                    patch.location = (uint64_t)offset;
                } else {
                    throw std::runtime_error(
                        "patch target must be main_exe, disc_raw, or disc_user");
                }
                const std::string expected = toml::find<std::string>(v, "expected");
                const std::string replacement = toml::find<std::string>(v, "replace");
                if (!parse_hex_bytes(expected, patch.expected) ||
                    !parse_hex_bytes(replacement, patch.replacement) ||
                    patch.expected.empty() ||
                    patch.expected.size() != patch.replacement.size())
                    throw std::runtime_error(
                        "patch expected/replace must be equal-length non-empty hex");
                const uint64_t sector_size =
                    patch.target == ModPatchTarget::DiscRaw ? 2352 :
                    patch.target == ModPatchTarget::DiscUser ? 2048 : 0;
                if (sector_size != 0 &&
                    patch.location % sector_size + patch.replacement.size() > sector_size)
                    throw std::runtime_error(
                        "disc patch may not cross a sector boundary");
                patch.when_option =
                    v.contains("when_option") ? toml::find<std::string>(v, "when_option") : "";
                patch.when_value =
                    v.contains("when_value") ? toml::find<std::string>(v, "when_value") : "";
                patch.order = toml::find_or<int64_t>(
                    v, "order", (int64_t)declaration_index);
                if (patch.when_option.empty() != patch.when_value.empty())
                    throw std::runtime_error(
                        "patch condition requires both when_option and when_value");
                if (!patch.when_option.empty()) {
                    const auto option = std::find_if(
                        out.options.begin(), out.options.end(),
                        [&](const ModOption& item) { return item.id == patch.when_option; });
                    if (option == out.options.end())
                        throw std::runtime_error("patch references unknown option");
                }
                out.patches.push_back(std::move(patch));
                ++declaration_index;
            }
        }
        return true;
    } catch (const std::exception& ex) {
        set_error(error, path.string() + ": " + ex.what());
        return false;
    }
}

bool ModPackageManager::scan(std::string* error) {
    packages_.clear();
    std::error_code ec;
    const fs::path packages_root = root_ / "packages";
    if (!fs::exists(packages_root, ec)) return true;
    for (const fs::directory_entry& id_dir : fs::directory_iterator(packages_root, ec)) {
        if (ec) break;
        if (!id_dir.is_directory()) continue;
        for (const fs::directory_entry& version_dir : fs::directory_iterator(id_dir.path(), ec)) {
            if (ec) break;
            if (!version_dir.is_directory()) continue;
            const fs::path manifest = version_dir.path() / "manifest.toml";
            if (!fs::exists(manifest)) continue;
            ModPackage package;
            std::string parse_error;
            if (!read_manifest(manifest, package, &parse_error)) {
                set_error(error, parse_error);
                return false;
            }
            if (package.id != id_dir.path().filename().string() ||
                package.version != version_dir.path().filename().string()) {
                set_error(error, "package path does not match manifest id/version: " +
                                 manifest.string());
                return false;
            }
            packages_[package.id][package.version] = std::move(package);
        }
    }
    if (ec) {
        set_error(error, "cannot scan packages: " + ec.message());
        return false;
    }
    return true;
}

bool ModPackageManager::load_state(std::string* error) {
    selections_.clear();
    const fs::path path = root_ / "state.toml";
    if (!fs::exists(path)) return true;
    try {
        const toml::value cfg = toml::parse(path.string());
        const int64_t version = toml::find<int64_t>(cfg, "format_version");
        if (version != 1) throw std::runtime_error("unsupported state format_version");
        if (!cfg.contains("package")) return true;
        for (const toml::value& v : toml::find(cfg, "package").as_array()) {
            const std::string id = toml::find<std::string>(v, "id");
            if (!valid_id(id)) throw std::runtime_error("invalid state package id");
            ModSelection selection;
            selection.enabled = toml::find_or<bool>(v, "enabled", false);
            selection.version = toml::find_or<std::string>(v, "version", "");
            if (v.contains("values")) {
                for (const auto& [key, value] : toml::find(v, "values").as_table()) {
                    if (value.is_string()) selection.values[key] = toml::get<std::string>(value);
                    else if (value.is_boolean())
                        selection.values[key] = toml::get<bool>(value) ? "true" : "false";
                    else if (value.is_integer())
                        selection.values[key] = std::to_string(toml::get<int64_t>(value));
                    else throw std::runtime_error("state option values must be scalar");
                }
            }
            selections_[id] = std::move(selection);
        }
        return true;
    } catch (const std::exception& ex) {
        set_error(error, path.string() + ": " + ex.what());
        return false;
    }
}

bool ModPackageManager::save_state(std::string* error) const {
    std::error_code ec;
    fs::create_directories(root_, ec);
    if (ec) {
        set_error(error, "cannot create mods directory: " + ec.message());
        return false;
    }
    const fs::path temp = root_ / "state.toml.tmp";
    const fs::path final = root_ / "state.toml";
    std::ofstream out(temp, std::ios::trunc);
    if (!out) {
        set_error(error, "cannot write " + temp.string());
        return false;
    }
    out << "format_version = 1\n";
    for (const auto& [id, selection] : selections_) {
        out << "\n[[package]]\n";
        out << "id = " << quote_toml(id) << "\n";
        out << "enabled = " << (selection.enabled ? "true" : "false") << "\n";
        if (!selection.version.empty())
            out << "version = " << quote_toml(selection.version) << "\n";
        if (!selection.values.empty()) {
            out << "[package.values]\n";
            for (const auto& [key, value] : selection.values)
                out << key << " = " << quote_toml(value) << "\n";
        }
    }
    out.close();
    if (!out) {
        set_error(error, "cannot finish " + temp.string());
        return false;
    }
    fs::rename(temp, final, ec);
    if (ec) {
        fs::remove(final, ec);
        ec.clear();
        fs::rename(temp, final, ec);
    }
    if (ec) {
        set_error(error, "cannot publish state: " + ec.message());
        return false;
    }
    return true;
}

bool ModPackageManager::install_archive(const fs::path& archive,
                                        std::string* installed_id,
                                        std::string* installed_version,
                                        std::string* error) {
    std::vector<uint8_t> bytes;
    std::vector<ZipEntry> entries;
    if (!read_file(archive, bytes, error) || !parse_zip(bytes, entries, error))
        return false;
    const auto manifest_entry = std::find_if(entries.begin(), entries.end(),
        [](const ZipEntry& e) { return e.name == "manifest.toml" && !e.directory; });
    if (manifest_entry == entries.end()) {
        set_error(error, "archive root does not contain manifest.toml");
        return false;
    }

    std::error_code ec;
    fs::create_directories(root_ / ".staging", ec);
    if (ec) {
        set_error(error, "cannot create install staging root: " + ec.message());
        return false;
    }
    const std::string token =
        std::to_string((unsigned long long)crc32_compute(bytes.data(), bytes.size()));
    const fs::path staging = root_ / ".staging" / ("install-" + token);
    if (fs::exists(staging)) {
        set_error(error, "install staging path already exists; remove " + staging.string());
        return false;
    }
    if (!extract_zip(bytes, entries, staging, error)) {
        fs::remove_all(staging, ec);
        return false;
    }
    ModPackage package;
    if (!read_manifest(staging / "manifest.toml", package, error)) {
        fs::remove_all(staging, ec);
        return false;
    }
    const fs::path destination = root_ / "packages" / package.id / package.version;
    if (fs::exists(destination)) {
        fs::remove_all(staging, ec);
        set_error(error, "package version is already installed");
        return false;
    }
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        fs::remove_all(staging, ec);
        set_error(error, "cannot create package directory: " + ec.message());
        return false;
    }
    fs::rename(staging, destination, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        set_error(error, "cannot publish installed package: " + ec.message());
        return false;
    }
    package.root = destination;
    packages_[package.id][package.version] = package;
    if (installed_id) *installed_id = package.id;
    if (installed_version) *installed_version = package.version;
    return true;
}

bool ModPackageManager::remove_version(const std::string& id, const std::string& version,
                                       std::string* error) {
    const auto sit = selections_.find(id);
    if (sit != selections_.end() && sit->second.enabled &&
        (sit->second.version.empty() || sit->second.version == version)) {
        set_error(error, "cannot remove an active package version");
        return false;
    }
    for (const auto& [other_id, selection] : selections_) {
        if (!selection.enabled || other_id == id) continue;
        const ModPackage* package = find_selected(packages_, other_id, selection);
        if (!package) continue;
        for (const ModRequirement& dep : package->dependencies) {
            if (dep.id == id && version_satisfies(version, dep.version)) {
                set_error(error, "cannot remove a version required by " + other_id);
                return false;
            }
        }
    }
    const auto pit = packages_.find(id);
    if (pit == packages_.end() || pit->second.find(version) == pit->second.end()) {
        set_error(error, "package version is not installed");
        return false;
    }
    const fs::path path = pit->second.at(version).root;
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) {
        set_error(error, "cannot remove package version: " + ec.message());
        return false;
    }
    packages_[id].erase(version);
    if (packages_[id].empty()) packages_.erase(id);
    return true;
}

bool ModPackageManager::set_enabled(const std::string& id, bool enabled, std::string* error) {
    if (packages_.find(id) == packages_.end()) {
        set_error(error, "package is not installed");
        return false;
    }
    selections_[id].enabled = enabled;
    return true;
}

bool ModPackageManager::select_version(const std::string& id, const std::string& version,
                                       std::string* error) {
    const auto pit = packages_.find(id);
    if (pit == packages_.end() || pit->second.find(version) == pit->second.end()) {
        set_error(error, "package version is not installed");
        return false;
    }
    selections_[id].version = version;
    return true;
}

bool ModPackageManager::set_option(const std::string& id, const std::string& option_id,
                                   const std::string& value, std::string* error) {
    const auto sit = selections_.find(id);
    const ModSelection empty;
    const ModSelection& selection = sit == selections_.end() ? empty : sit->second;
    const ModPackage* package = find_selected(packages_, id, selection);
    if (!package) {
        set_error(error, "package/version is not installed");
        return false;
    }
    const auto oit = std::find_if(package->options.begin(), package->options.end(),
        [&](const ModOption& option) { return option.id == option_id; });
    if (oit == package->options.end()) {
        set_error(error, "unknown package option");
        return false;
    }
    bool valid = false;
    if (oit->type == ModOptionType::Boolean) {
        valid = value == "true" || value == "false";
    } else if (oit->type == ModOptionType::Choice) {
        valid = std::any_of(oit->choices.begin(), oit->choices.end(),
            [&](const ModChoice& choice) { return choice.value == value; });
    } else {
        try {
            size_t used = 0;
            const int64_t parsed = std::stoll(value, &used);
            valid = used == value.size() && parsed >= oit->min_value &&
                    parsed <= oit->max_value &&
                    ((parsed - oit->min_value) % oit->step) == 0;
        } catch (...) {
            valid = false;
        }
    }
    if (!valid) {
        set_error(error, "invalid option value");
        return false;
    }
    selections_[id].values[option_id] = value;
    return true;
}

const ModPackage* ModPackageManager::selected_package(const std::string& id) const {
    const auto selection = selections_.find(id);
    const ModSelection blank;
    return find_selected(packages_, id,
                         selection == selections_.end() ? blank : selection->second);
}

ModResolution ModPackageManager::resolve(const std::string& game_id,
                                         const std::string& exe_sha256,
                                         const std::string& disc_sha256) const {
    ModResolution result;
    std::map<std::string, const ModPackage*> active;
    for (const auto& [id, selection] : selections_) {
        if (!selection.enabled) continue;
        const ModPackage* package = find_selected(packages_, id, selection);
        if (!package) {
            result.errors.push_back("selected package/version is not installed: " + id);
            continue;
        }
        if (!target_matches(*package, game_id, exe_sha256, disc_sha256)) {
            result.errors.push_back("package does not target this game/image: " + id);
            continue;
        }
        active[id] = package;
    }

    for (const auto& [id, package] : active) {
        for (const ModRequirement& dep : package->dependencies) {
            const auto found = active.find(dep.id);
            if (found == active.end())
                result.errors.push_back(id + " requires enabled package " + dep.id);
            else if (!version_satisfies(found->second->version, dep.version))
                result.errors.push_back(id + " requires " + dep.id + " " + dep.version);
        }
        for (const std::string& conflict : package->conflicts)
            if (active.find(conflict) != active.end())
                result.errors.push_back(id + " conflicts with " + conflict);

        const auto selection = selections_.find(id);
        if (selection != selections_.end()) {
            for (const ModOption& option : package->options) {
                const auto value = selection->second.values.find(option.id);
                if (value == selection->second.values.end()) continue;
                /* Reuse the public validation path without mutating by checking
                 * the same domain directly. Defaults require no state entry. */
                bool valid = false;
                if (option.type == ModOptionType::Boolean)
                    valid = value->second == "true" || value->second == "false";
                else if (option.type == ModOptionType::Choice)
                    valid = std::any_of(option.choices.begin(), option.choices.end(),
                        [&](const ModChoice& c) { return c.value == value->second; });
                else {
                    try {
                        size_t used = 0;
                        const int64_t n = std::stoll(value->second, &used);
                        valid = used == value->second.size() && n >= option.min_value &&
                                n <= option.max_value &&
                                ((n - option.min_value) % option.step) == 0;
                    } catch (...) {}
                }
                if (!valid) result.errors.push_back(id + ": invalid value for " + option.id);
            }
        }
        if (package->resolver.rfind("builtin:", 0) == 0) {
            const std::string resolver_id = package->resolver.substr(8);
            const auto resolver = builtin_resolvers().find(resolver_id);
            if (resolver == builtin_resolvers().end())
                result.errors.push_back(id + ": built-in resolver is unavailable: " + resolver_id);
        }
    }

    enum class Visit { None, Active, Done };
    std::map<std::string, Visit> visits;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        if (visits[id] == Visit::Done) return;
        if (visits[id] == Visit::Active) {
            result.errors.push_back("dependency cycle includes " + id);
            return;
        }
        visits[id] = Visit::Active;
        const ModPackage* package = active.at(id);
        std::vector<std::string> deps;
        for (const ModRequirement& dep : package->dependencies)
            if (active.find(dep.id) != active.end()) deps.push_back(dep.id);
        std::sort(deps.begin(), deps.end());
        for (const std::string& dep : deps) visit(dep);
        visits[id] = Visit::Done;
        result.ordered.push_back(package);
    };
    for (const auto& [id, package] : active) visit(id);

    if (!result.errors.empty()) {
        result.ordered.clear();
        return result;
    }

    for (const ModPackage* package : result.ordered) {
        const auto selected_it = selections_.find(package->id);
        const ModSelection blank;
        const ModSelection& selected =
            selected_it == selections_.end() ? blank : selected_it->second;
        if (package->resolver == "declarative") {
            std::vector<const ModPatch*> patches;
            patches.reserve(package->patches.size());
            for (const ModPatch& patch : package->patches) {
                if (!patch.when_option.empty() &&
                    effective_option_value(*package, selected, patch.when_option) !=
                        patch.when_value)
                    continue;
                patches.push_back(&patch);
            }
            std::stable_sort(patches.begin(), patches.end(),
                [](const ModPatch* a, const ModPatch* b) { return a->order < b->order; });
            for (const ModPatch* patch : patches) {
                ModResolution::Write write;
                write.target = patch->target;
                write.location = patch->location;
                write.expected = patch->expected;
                write.replacement = patch->replacement;
                write.package_id = package->id;
                result.writes.push_back(std::move(write));
            }
        } else {
            const std::string resolver_id = package->resolver.substr(8);
            const auto resolver = builtin_resolvers().find(resolver_id);
            if (resolver != builtin_resolvers().end() &&
                !resolver->second(*package, selected, result.writes, result.errors) &&
                result.errors.empty())
                result.errors.push_back(package->id + ": built-in resolver failed");
        }
    }
    for (size_t i = 0; i < result.writes.size(); ++i) {
        const ModResolution::Write& write = result.writes[i];
        if (write.expected.empty() ||
            write.expected.size() != write.replacement.size()) {
            result.errors.push_back(write.package_id + ": resolver emitted invalid write");
            continue;
        }
        for (size_t j = 0; j < i; ++j) {
            if (writes_overlap(result.writes[j], write)) {
                result.errors.push_back(
                    write.package_id + ": patch overlaps a write from " +
                    result.writes[j].package_id);
                break;
            }
        }
    }
    if (!result.errors.empty()) {
        result.ordered.clear();
        result.writes.clear();
        return result;
    }
    result.fingerprint = fingerprint_text(
        canonical_resolution(result.ordered, selections_, result.writes));
    result.ok = true;
    return result;
}

} // namespace PSXRecompV4
