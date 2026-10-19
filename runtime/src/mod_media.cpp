#include "mod_media.h"
#include "iso_reader.h"
#include "psx_sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace PSXRecompV4 {
namespace {
constexpr uint64_t kMaxMediaBytes = 512ull * 1024 * 1024;

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint32_t be32(const uint8_t* p) {
    return uint32_t(p[3]) | (uint32_t(p[2]) << 8) |
           (uint32_t(p[1]) << 16) | (uint32_t(p[0]) << 24);
}

void read_disc_sector(PS1::ISOReader& reader, uint32_t lba, uint8_t* out) {
    std::array<uint8_t, 2352> raw{};
    if (reader.ReadRawSector(lba, raw.data())) {
        if (raw[15] != 1 && raw[15] != 2)
            throw std::runtime_error("donor volume contains a non-data sector");
        std::memcpy(out, raw.data() + (raw[15] == 1 ? 16 : 24), 2048);
    } else if (!reader.ReadSector(lba, out)) {
        throw std::runtime_error("cannot read donor volume sector");
    }
}
} // namespace

bool load_mod_media(const std::filesystem::path& path,
                    const std::string& format, uint64_t expected_size,
                    const std::string& expected_sha256,
                    std::shared_ptr<const std::vector<uint8_t>>& bytes,
                    std::string* error) {
    bytes.reset();
    if (error) error->clear();
    try {
        if (!expected_size || expected_size > kMaxMediaBytes ||
            expected_sha256.size() != 64 ||
            !std::all_of(expected_sha256.begin(), expected_sha256.end(),
                         [](char c) { return (c >= '0' && c <= '9') ||
                                             (c >= 'a' && c <= 'f'); }))
            throw std::runtime_error("invalid donor size or SHA-256");
        auto data = std::make_shared<std::vector<uint8_t>>();
        if (format == "psx-disc") {
            PS1::ISOReader reader;
            if (!reader.Open(path.string()))
                throw std::runtime_error("cannot open donor disc");
            std::array<uint8_t, 2048> pvd{};
            read_disc_sector(reader, 16, pvd.data());
            if (pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) ||
                pvd[6] != 1 || pvd[128] != 0 || pvd[129] != 8 ||
                pvd[130] != 8 || pvd[131] != 0)
                throw std::runtime_error("donor disc is not a 2048-byte ISO9660 volume");
            const uint32_t declared_count = le32(pvd.data() + 80);
            /* Retail mixed-mode discs may declare the whole disc, including
             * audio, as their ISO volume size. The actual data-track boundary
             * comes from the TOC or the single-file image length. */
            const uint32_t count = reader.TrackCount() > 1
                ? reader.TrackPregapLBA(2) : reader.GetSectorCount();
            if (reader.TrackIsAudio(1) || reader.TrackStartLBA(1) != 0 ||
                declared_count != be32(pvd.data() + 84) || declared_count < count ||
                count <= 16 || uint64_t(count) * 2048 != expected_size)
                throw std::runtime_error("donor data-track size does not match");
            data->resize(static_cast<size_t>(expected_size));
            for (uint32_t i = 0; i < count; ++i)
                read_disc_sector(reader, i, data->data() + size_t(i) * 2048);
        } else if (format == "file" || format == "n64-rom") {
            if (std::filesystem::file_size(path) != expected_size)
                throw std::runtime_error("donor file size does not match");
            if (format == "n64-rom" &&
                (expected_size < 64 || expected_size % 4 || expected_size > 64ull * 1024 * 1024))
                throw std::runtime_error("invalid N64 ROM size");
            std::ifstream input(path, std::ios::binary);
            data->resize(static_cast<size_t>(expected_size));
            if (!input.read(reinterpret_cast<char*>(data->data()),
                            static_cast<std::streamsize>(expected_size)) ||
                input.peek() != std::char_traits<char>::eof())
                throw std::runtime_error("cannot read complete donor file");
            if (format == "n64-rom") {
                const uint32_t magic = be32(data->data());
                if (magic == 0x37804012) {
                    for (size_t i = 0; i < data->size(); i += 2)
                        std::swap((*data)[i], (*data)[i + 1]);
                } else if (magic == 0x40123780) {
                    for (size_t i = 0; i < data->size(); i += 4)
                        std::reverse(data->begin() + i, data->begin() + i + 4);
                } else if (magic != 0x80371240) {
                    throw std::runtime_error("unrecognized N64 ROM byte order");
                }
            }
        } else {
            throw std::runtime_error("unsupported donor media format");
        }
        uint8_t digest[32];
        psx_sha256_compute(data->data(), data->size(), digest);
        static const char hex[] = "0123456789abcdef";
        std::string actual(64, '0');
        for (size_t i = 0; i < 32; ++i) {
            actual[i * 2] = hex[digest[i] >> 4];
            actual[i * 2 + 1] = hex[digest[i] & 15];
        }
        if (actual != expected_sha256)
            throw std::runtime_error("donor SHA-256 does not match");
        bytes = std::move(data);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}
} // namespace PSXRecompV4
