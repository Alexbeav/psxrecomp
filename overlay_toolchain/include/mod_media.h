#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace PSXRecompV4 {

/* Canonical bytes are an immutable, engine-owned snapshot. Plugins never
 * reopen the selected file after verification. The PSX domain contains the
 * first data track's 2048-byte sector payloads, excluding CD audio. */
bool load_mod_media(const std::filesystem::path& path,
                    const std::string& format, uint64_t expected_size,
                    const std::string& expected_sha256,
                    std::shared_ptr<const std::vector<uint8_t>>& bytes,
                    std::string* error = nullptr);

} // namespace PSXRecompV4
