#ifndef PSX_INPUT_DUALSHOCK_DELIVERY_H
#define PSX_INPUT_DUALSHOCK_DELIVERY_H
#include <stdint.h>
#include "sio.h"

/* Nymashock2.9.1 AddAxis writes a byte into the high byte of the Nyma
 * 16-bit input. Pinned Mednafen ddf225cf DualShock.UpdateInput rounds it
 * onto the protocol byte range. This is a source bridge, not a deadzone. */
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
