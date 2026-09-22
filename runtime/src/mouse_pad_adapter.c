#include "mouse_pad_adapter.h"

#define PAD_SELECT   (1u << 0)
#define PAD_UP       (1u << 4)
#define PAD_RIGHT    (1u << 5)
#define PAD_DOWN     (1u << 6)
#define PAD_LEFT     (1u << 7)
#define PAD_L1       (1u << 10)
#define PAD_TRIANGLE (1u << 12)
#define PAD_CIRCLE   (1u << 13)
#define PAD_SQUARE   (1u << 15)

static int s_enabled;
static int s_focused;
static int s_threshold = 12;
static int s_aim_threshold = 4;
static int s_pending_x;
static int s_pending_y;
static int s_consume_x;
static int s_consume_y;

static int clamp_int(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
}

void mouse_pad_configure(int enabled, int counts_per_frame,
                         int aim_counts_per_frame) {
    s_enabled = enabled ? 1 : 0;
    s_threshold = clamp_int(counts_per_frame, 1, 256);
    s_aim_threshold = clamp_int(aim_counts_per_frame, 1, 256);
    if (!s_enabled) mouse_pad_reset();
}

int mouse_pad_enabled(void) { return s_enabled; }

void mouse_pad_set_focus(int focused) {
    s_focused = focused ? 1 : 0;
    if (!s_focused) mouse_pad_reset();
}

void mouse_pad_add_motion(int dx, int dy) {
    if (!s_enabled || !s_focused) return;
    const int cap = s_threshold * 12;
    s_pending_x = clamp_int(s_pending_x + dx, -cap, cap);
    s_pending_y = clamp_int(s_pending_y + dy, -cap, cap);
}

uint16_t mouse_pad_merge(uint16_t buttons, uint32_t host_buttons) {
    s_consume_x = s_consume_y = 0;
    if (!s_enabled || !s_focused) return buttons;
    if (host_buttons & PSX_MOUSE_LEFT)   buttons &= (uint16_t)~PAD_SQUARE;
    if (host_buttons & PSX_MOUSE_RIGHT)  buttons &= (uint16_t)~PAD_L1;
    if (host_buttons & PSX_MOUSE_MIDDLE) buttons &= (uint16_t)~PAD_SELECT;
    if (host_buttons & PSX_MOUSE_X1)     buttons &= (uint16_t)~PAD_CIRCLE;
    if (host_buttons & PSX_MOUSE_X2)     buttons &= (uint16_t)~PAD_TRIANGLE;

    const int aiming = (host_buttons & PSX_MOUSE_RIGHT) != 0;
    const int threshold = aiming ? s_aim_threshold : s_threshold;
    if (s_pending_x >= threshold) {
        buttons &= (uint16_t)~PAD_RIGHT; s_consume_x = threshold;
    } else if (s_pending_x <= -threshold) {
        buttons &= (uint16_t)~PAD_LEFT; s_consume_x = -threshold;
    }
    /* Vertical D-pad motion belongs only to retail manual aim (RMB/L1).
     * Normal chase mode retains the authored vertical camera. */
    if (aiming) {
        /* SF's retail D-pad aim axis is authored opposite SDL relative Y.
         * Swap the PAD directions so mouse-up looks up and mouse-down looks
         * down from the player's perspective. */
        if (s_pending_y >= s_aim_threshold) {
            buttons &= (uint16_t)~PAD_UP; s_consume_y = s_aim_threshold;
        } else if (s_pending_y <= -s_aim_threshold) {
            buttons &= (uint16_t)~PAD_DOWN; s_consume_y = -s_aim_threshold;
        }
    }
    return buttons;
}

void mouse_pad_commit_frame(void) {
    s_pending_x -= s_consume_x;
    s_pending_y -= s_consume_y;
    s_consume_x = s_consume_y = 0;
}

void mouse_pad_reset(void) {
    s_pending_x = s_pending_y = 0;
    s_consume_x = s_consume_y = 0;
}
