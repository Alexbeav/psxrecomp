#ifndef PSX_INPUT_DUALSHOCK_DELIVERY_H
#define PSX_INPUT_DUALSHOCK_DELIVERY_H
#include <stdint.h>
#include "sio.h"

/* Host-side conversion of a route axis byte through a 16-bit input value
 * onto the protocol byte range. [NOT OBSERVED: sub-byte host input] The pad
 * reports the axis bytes it is given exactly, in the order RX, RY, LX, LY
 * [ORACLE FIXTURE P1, Octoshock 2.3]; that fixture takes whole bytes, so it
 * cannot observe this rounding. This is a source bridge, not a deadzone. */
static inline uint8_t input_dualshock_protocol_axis(uint8_t source)
{
    return (uint8_t)(((uint32_t)source * 256u * 255u + 32767u) / 65535u);
}
static inline void input_dualshock_protocol_sticks(const uint8_t source_ly_lx_ry_rx[4],
                                                   uint8_t protocol_lx_ly_rx_ry[4])
{
    for (unsigned i = 0; i < 4; ++i)
        protocol_lx_ly_rx_ry[i] = input_dualshock_protocol_axis(source_ly_lx_ry_rx[i ^ 1u]);
}
/* Preload admission for a route that declares one cold controller at P1:
 * no multitap, every other slot disconnected, P1 digital protocol mode with
 * neutral sticks. `dualshock` makes P1 config-capable (guest-owned mode). */
static inline void input_route_admit_cold_p1(int dualshock)
{
    sio_set_multitap(0);
    for (int slot = 0; slot < PSX_MAX_PLAYERS; ++slot) {
        sio_set_pad_connected(slot, slot == 0);
        sio_set_pad_config_capable(slot, dualshock && slot == 0);
        sio_set_pad_analog(slot, 0, 128, 128, 128, 128);
        sio_set_pad_state_slot(slot, 0xFFFF);
    }
}
/* Only the preload admission may establish the cold device type. Per-input
 * delivery must leave guest-owned mode, lock and in-flight protocol alone.
 * No D-pad/stick folding. Physical Analog is admitted only when neutral. */
static inline void input_dualshock_deliver(uint16_t buttons, const uint8_t source_ly_lx_ry_rx[4])
{
    uint8_t st[4];
    input_dualshock_protocol_sticks(source_ly_lx_ry_rx, st);
    sio_set_pad_state_slot(0, buttons);
    sio_set_pad_sticks(0, st[0], st[1], st[2], st[3]);
}
#endif
