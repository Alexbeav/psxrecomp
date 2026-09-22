#include "pad_timeline.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>

struct PadState {
    uint16_t buttons = 0xFFFF;
    std::array<uint8_t, 4> sticks{{0x80, 0x80, 0x80, 0x80}};
    int analog = 0;
    int connected = 0;
};
static PadState pads[2];

extern "C" uint16_t sio_get_pad_buttons_slot(int s) { return pads[s].buttons; }
extern "C" int sio_get_pad_connected(int s) { return pads[s].connected; }
extern "C" int sio_get_pad_analog(int s) { return pads[s].analog; }
extern "C" void sio_get_pad_sticks(int s, uint8_t out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = pads[s].sticks[(size_t)i];
}
extern "C" void sio_set_pad_connected(int s, int v) { pads[s].connected = v; }
extern "C" void sio_set_pad_state_slot(int s, uint16_t v) { pads[s].buttons = v; }
extern "C" void sio_set_pad_sticks(int s, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    pads[s].sticks = {{lx, ly, rx, ry}};
}
extern "C" void sio_request_pad_type(int s, int analog) { pads[s].analog = analog; }

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      "psxrecomp-pad-timeline-contract.psxpad";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    char error[256]{};

    assert(pad_timeline_configure(path.string().c_str(), nullptr, error, sizeof(error)));
    pads[0] = {0xFFEF, {{1, 2, 3, 4}}, 1, 1};
    pads[1] = {0x7FFF, {{5, 6, 7, 8}}, 0, 1};
    pad_timeline_capture(10);
    pads[0] = {0xFFFF, {{9, 10, 11, 12}}, 0, 1};
    pads[1] = {0xFFFF, {{13, 14, 15, 16}}, 1, 0};
    pad_timeline_capture(11);
    pad_timeline_close();

    assert(!pad_timeline_configure(path.string().c_str(), nullptr, error, sizeof(error)));
    assert(pad_timeline_configure(nullptr, path.string().c_str(), error, sizeof(error)));
    pads[0] = {}; pads[1] = {};
    assert(pad_timeline_apply(10));
    assert(pads[0].buttons == 0xFFEF && pads[0].sticks[0] == 1 && pads[0].analog && pads[0].connected);
    assert(pads[1].buttons == 0x7FFF && pads[1].sticks[3] == 8 && !pads[1].analog && pads[1].connected);
    assert(pad_timeline_apply(11));
    assert(pads[0].buttons == 0xFFFF && pads[0].sticks[3] == 12 && !pads[0].analog);
    assert(pads[1].sticks[0] == 13 && pads[1].analog && !pads[1].connected);
    /* Exhaustion is a sticky replay fault: it must never fall through to live
     * host sampling on the following frame. */
    assert(!pad_timeline_apply(12));
    assert(pad_timeline_is_replay());
    assert(!pad_timeline_apply(13));
    pad_timeline_close();
    std::filesystem::remove(path, ec);
    return 0;
}
