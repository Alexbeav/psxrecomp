#pragma once
#include <array>
#include <cstdint>

namespace psx_playtest_diagnostic {
// Host evidence only. No game-specific addresses or classification assumptions.
using TelemetryRow = std::array<int64_t,13>;
constexpr const char *TelemetryHeader =
    "sequence,host_elapsed_us,host_interval_us,incident,cd_reading,cd_pending,"
    "cd_lba,cd_data_read,cd_xa,render_passes,render_aborted,render_verify_checks,render_verify_mismatch\n";
}
