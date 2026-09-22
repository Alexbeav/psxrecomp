#include "pad_timeline.h"
#include "sio.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {
constexpr std::array<unsigned char, 8> kMagic{{'P','S','X','P','A','D','1','\0'}};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kHeaderSize = 32;
constexpr uint32_t kRecordSize = 32;

enum class Mode { Off, Record, Replay, ReplayFault };
Mode g_mode = Mode::Off;
FILE *g_file = nullptr;
uint64_t g_count = 0;
std::vector<std::array<unsigned char, kRecordSize>> g_replay;
size_t g_replay_pos = 0;
bool g_registered = false;

void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
void put64(unsigned char *p, uint64_t v) {
    put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32));
}
uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t get64(const unsigned char *p) {
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}
void set_error(char *out, unsigned size, const char *msg) {
    if (out && size) std::snprintf(out, size, "%s", msg ? msg : "unknown error");
}
void write_header(uint64_t count) {
    unsigned char h[kHeaderSize]{};
    std::memcpy(h, kMagic.data(), kMagic.size());
    put32(h + 8, kVersion); put32(h + 12, kHeaderSize);
    put32(h + 16, kRecordSize); put64(h + 20, count);
    std::fseek(g_file, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof(h), g_file);
    std::fseek(g_file, 0, SEEK_END);
}
void encode_slot(unsigned char *p, int slot) {
    const uint16_t buttons = sio_get_pad_buttons_slot(slot);
    uint8_t sticks[4]{0x80, 0x80, 0x80, 0x80};
    sio_get_pad_sticks(slot, sticks);
    p[0] = (unsigned char)buttons; p[1] = (unsigned char)(buttons >> 8);
    std::memcpy(p + 2, sticks, 4);
    p[6] = (unsigned char)(sio_get_pad_analog(slot) ? 1 : 0);
    p[7] = (unsigned char)(sio_get_pad_connected(slot) ? 1 : 0);
}
void apply_slot(const unsigned char *p, int slot) {
    const uint16_t buttons = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    sio_set_pad_connected(slot, p[7] ? 1 : 0);
    sio_set_pad_state_slot(slot, buttons);
    sio_set_pad_sticks(slot, p[2], p[3], p[4], p[5]);
    sio_request_pad_type(slot, p[6] ? 1 : 0);
}
}

extern "C" void pad_timeline_close(void) {
    if (g_file) {
        if (g_mode == Mode::Record) write_header(g_count);
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_replay.clear(); g_replay_pos = 0; g_mode = Mode::Off;
}

extern "C" int pad_timeline_configure(const char *record_path,
                                        const char *replay_path,
                                        char *error_out,
                                        unsigned error_out_size) {
    pad_timeline_close();
    if (record_path && replay_path) {
        set_error(error_out, error_out_size, "--pad-record and --pad-replay are mutually exclusive");
        return 0;
    }
    if (!record_path && !replay_path) return 1;
    if (!g_registered) { std::atexit(pad_timeline_close); g_registered = true; }

    if (record_path) {
        std::error_code ec;
        if (std::filesystem::exists(record_path, ec)) {
            set_error(error_out, error_out_size, "pad recording path already exists");
            return 0;
        }
        const auto parent = std::filesystem::path(record_path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        if (ec || !(g_file = std::fopen(record_path, "w+b"))) {
            set_error(error_out, error_out_size, "cannot create pad recording");
            return 0;
        }
        g_mode = Mode::Record; g_count = 0; write_header(0); std::fflush(g_file);
        return 1;
    }

    g_file = std::fopen(replay_path, "rb");
    if (!g_file) { set_error(error_out, error_out_size, "cannot open pad replay"); return 0; }
    unsigned char h[kHeaderSize]{};
    if (std::fread(h, 1, sizeof(h), g_file) != sizeof(h) ||
        std::memcmp(h, kMagic.data(), kMagic.size()) != 0 ||
        get32(h + 8) != kVersion || get32(h + 12) != kHeaderSize ||
        get32(h + 16) != kRecordSize) {
        set_error(error_out, error_out_size, "invalid pad timeline header");
        pad_timeline_close(); return 0;
    }
    const uint64_t count = get64(h + 20);
    if (!count || count > 100000000ull) {
        set_error(error_out, error_out_size, "invalid pad timeline sample count");
        pad_timeline_close(); return 0;
    }
    g_replay.resize((size_t)count);
    if (std::fread(g_replay.data(), kRecordSize, (size_t)count, g_file) != count ||
        std::fgetc(g_file) != EOF) {
        set_error(error_out, error_out_size, "truncated or trailing pad timeline data");
        pad_timeline_close(); return 0;
    }
    std::fclose(g_file); g_file = nullptr;
    uint64_t prev = 0;
    for (const auto& rec : g_replay) {
        const uint64_t frame = get64(rec.data());
        if (!frame || frame <= prev) {
            set_error(error_out, error_out_size, "pad timeline frames are not strictly increasing");
            pad_timeline_close(); return 0;
        }
        prev = frame;
    }
    g_mode = Mode::Replay; g_count = count; g_replay_pos = 0;
    return 1;
}

extern "C" int pad_timeline_is_replay(void) {
    return g_mode == Mode::Replay || g_mode == Mode::ReplayFault;
}

extern "C" void pad_timeline_capture(uint64_t guest_frame) {
    if (g_mode != Mode::Record || !g_file) return;
    std::array<unsigned char, kRecordSize> rec{};
    put64(rec.data(), guest_frame);
    encode_slot(rec.data() + 8, 0); encode_slot(rec.data() + 16, 1);
    if (std::fwrite(rec.data(), 1, rec.size(), g_file) != rec.size()) {
        std::fprintf(stderr, "psxrecomp: pad timeline write failed at frame %llu\n",
                     (unsigned long long)guest_frame);
        pad_timeline_close(); return;
    }
    g_count++;
    if ((g_count % 60u) == 0) std::fflush(g_file);
}

extern "C" int pad_timeline_apply(uint64_t guest_frame) {
    if (g_mode == Mode::ReplayFault) return 0;
    if (g_mode != Mode::Replay) return 0;
    if (g_replay_pos >= g_replay.size()) {
        std::fprintf(stderr, "psxrecomp: pad replay exhausted at frame %llu\n",
                     (unsigned long long)guest_frame);
        g_mode = Mode::ReplayFault; return 0;
    }
    const auto& rec = g_replay[g_replay_pos];
    const uint64_t expected = get64(rec.data());
    if (expected != guest_frame) {
        std::fprintf(stderr,
            "psxrecomp: pad replay frame mismatch expected=%llu actual=%llu\n",
            (unsigned long long)expected, (unsigned long long)guest_frame);
        g_mode = Mode::ReplayFault; return 0;
    }
    apply_slot(rec.data() + 8, 0); apply_slot(rec.data() + 16, 1);
    g_replay_pos++;
    return 1;
}
