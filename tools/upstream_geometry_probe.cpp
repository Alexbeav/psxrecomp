// Evaluation-only client of the maintained reader; no original image writes.
#include "iso_reader.h"
#include "psx_sha256.h"
#include <array>
#include <cstdio>
#include <vector>

static void print_digest(const uint8_t* bytes) {
    for (int i = 0; i < 32; ++i) std::printf("%02x", bytes[i]);
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    for (int image = 1; image < argc; ++image) {
        PS1::ISOReader reader;
        bool raw = false;
        if (!PS1::ISOReader::FirstFileIsRaw(argv[image], &raw) ||
            !reader.Open(argv[image])) return 3;
        const uint32_t count = reader.GetSectorCount();
        if (count <= 16 || reader.TrackCount() != 1 || reader.TrackStartLBA(1) != 0)
            return 4;
        psx_sha256_ctx digest;
        psx_sha256_init(&digest);
        std::array<uint8_t, 2048> sector{};
        for (uint32_t lba = 0; lba < count; ++lba) {
            if (!reader.ReadSector(lba, sector.data())) return 5;
            psx_sha256_update(&digest, sector.data(), sector.size());
        }
        if (reader.ReadSector(count, sector.data())) return 6;
        uint8_t hash[32];
        psx_sha256_final(&digest, hash);
        const auto root = reader.GetRootDirectory();
        std::printf("GEOMETRY image=%d raw=%d sectors=%u root_lba=%u root_bytes=%u sha256=",
                    image, raw ? 1 : 0, count, root.lba, root.size);
        print_digest(hash);
        std::printf("\n");
        if (!reader.ReadSector(16, sector.data())) return 7;
        psx_sha256_compute(sector.data(), sector.size(), hash);
        std::printf("PVD image=%d sha256=", image);
        print_digest(hash);
        std::printf("\n");
        for (const char* name : {"SYSTEM.CNF", "SCUS_944.23"}) {
            const auto size = reader.GetFileSize(name);
            if (!size || size > 4 * 1024 * 1024) return 8;
            std::vector<uint8_t> data(size);
            if (reader.ReadFile(name, data.data(), size) != size) return 9;
            psx_sha256_compute(data.data(), data.size(), hash);
            std::printf("FILE image=%d name=%s bytes=%zu sha256=", image, name, size);
            print_digest(hash);
            std::printf("\n");
        }
    }
    return 0;
}
