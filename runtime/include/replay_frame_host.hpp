#pragma once
#include "replay_session.h"
#include <cstdint>
#include <string>

// Per-boot host state. A frontend return must not reuse the prior session's
// frame number, consumed start choice or latched fast-forward ownership.
struct ReplayFrameHost {
    int env_read = 0, exit_at_end = 0, fast_play = 0, fast_saved = -1;
    const char *play_path = nullptr, *record_file = nullptr;
    long long record_at = -1, record_frames = 0, recorded = 0;
    bool record_boot = false;
    std::string boot_file;
    uint64_t vblanks = 0;
    ReplayState previous = REPLAY_IDLE;
    void reset() { *this = ReplayFrameHost{}; }
};
