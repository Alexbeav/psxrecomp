/* Authored host-to-SIO bridge cases; no retail assets or input routes. */
#include "input_dualshock_delivery.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t actual_buttons;
static uint8_t actual_sticks[4];
static unsigned writes;
void sio_set_pad_state_slot(int slot, uint16_t buttons) {
    assert(slot == 0); actual_buttons = buttons; ++writes;
}
void sio_set_pad_sticks(int slot, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    assert(slot == 0);
    actual_sticks[0] = lx; actual_sticks[1] = ly;
    actual_sticks[2] = rx; actual_sticks[3] = ry; ++writes;
}
/* There is deliberately no type/lock/config setter in this link. A bridge
 * that starts touching those fields must fail to link, not get a no-op stub. */
int main(void) {
    for (unsigned value = 0; value < 256; ++value)
        assert(input_dualshock_protocol_axis((uint8_t)value) == (value > 128 ? value - 1 : value));
    const uint8_t asymmetric[4] = {1, 0, 128, 255};
    input_dualshock_deliver(0xFFEF, asymmetric);
    const uint8_t expected[4] = {0, 1, 254, 128};
    assert(writes == 2 && actual_buttons == 0xFFEF && !memcmp(actual_sticks, expected, 4));
    const uint8_t centered[4] = {128,128,128,128};
    input_dualshock_deliver(0xFF0F, centered);
    assert(writes == 4 && actual_buttons == 0xFF0F && !memcmp(actual_sticks, centered, 4));
    puts("DualShock exact axis conversion, order and no-fold delivery passed");
    return 0;
}
