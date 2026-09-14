#ifndef PSX_INPUT_DUALSHOCK_ROUTE_FILE_H
#define PSX_INPUT_DUALSHOCK_ROUTE_FILE_H

/* PSXRTI2 single-connected-DualShock controller input, independent of the
 * PSXRTI1 digital format. Parsing does not touch live input or guest state.
 * The controller starts in digital mode; its protocol mode is guest-owned.
 * Wire record: u32 one-based frame, u16 active-low buttons, u8 LY,LX,RY,RX,
 * u8 physical Analog button (0/1), u8 reserved zero. No console/tray events.
 * A runtime consumer must separately qualify every admitted device action. */
#include "input_route_file.h"

typedef struct {
    uint32_t frames;
    uint16_t buttons;
    uint8_t axes_ly_lx_ry_rx[4];
    uint8_t analog_button;
} InputDualShockRouteStep;

static inline const char *input_dualshock_route_read(
    FILE *f, InputDualShockRouteStep *steps,
    uint32_t *step_count, uint32_t *frame_count)
{
    unsigned char h[24], r[12];
    uint32_t n, count = 0;
    *step_count = *frame_count = 0;
    if (!f || fread(h, 1, sizeof(h), f) != sizeof(h)) return "short header";
    if (memcmp(h, "PSXRTI2\0", 8) || input_route_le32(h + 8) != 2 ||
        input_route_le32(h + 12) != sizeof(r)) return "header identity";
    n = input_route_le32(h + 16);
    if (input_route_le32(h + 20) || !n || n > INPUT_ROUTE_MAX_FRAMES)
        return "frame count/reserved";
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t buttons;
        InputDualShockRouteStep *previous;
        if (fread(r, 1, sizeof(r), f) != sizeof(r)) return "short record";
        if (input_route_le32(r) != i + 1 || r[10] > 1 || r[11])
            return "record sequence/button/reserved";
        buttons = (uint16_t)((unsigned)r[4] | ((unsigned)r[5] << 8));
        previous = count ? &steps[count - 1] : NULL;
        if (previous && previous->buttons == buttons &&
            !memcmp(previous->axes_ly_lx_ry_rx, r + 6, 4) &&
            previous->analog_button == r[10]) {
            ++previous->frames;
        } else {
            if (count == INPUT_ROUTE_MAX_STEPS) return "step capacity";
            steps[count].frames = 1;
            steps[count].buttons = buttons;
            memcpy(steps[count].axes_ly_lx_ry_rx, r + 6, 4);
            steps[count++].analog_button = r[10];
        }
    }
    if (fgetc(f) != EOF || ferror(f)) return "trailing bytes/read error";
    *step_count = count;
    *frame_count = n;
    return NULL;
}
#endif
