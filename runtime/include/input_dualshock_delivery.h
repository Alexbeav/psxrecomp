#ifndef PSX_INPUT_DUALSHOCK_DELIVERY_H
#define PSX_INPUT_DUALSHOCK_DELIVERY_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sio.h"

/* Which source wrote the route's axis bytes. The default is Nymashock, which
 * hands the pad a 16-bit value per axis; PSX_INPUT_ROUTE_AXIS_MODEL=
 * octoshock-byte selects Octoshock, whose wrapper hands the pad the logged
 * byte unscaled [ORACLE FIXTURE P1, Octoshock 2.3; corpus PSX-TAS-001]. */
/* Physical MODE presses are delivered only when the route asks for them by
 * model: PSX_INPUT_ROUTE_MODE_MODEL=octoshock-2.4.2 or nymashock-1.29.0. Otherwise
 * a MODE press stays refused at admission. */
static inline const char *input_dualshock_mode_model(void)
{
    const char *model = getenv("PSX_INPUT_ROUTE_MODE_MODEL");
    if (model && !strcmp(model, "octoshock-2.4.2")) return model;
    if (model && !strcmp(model, "nymashock-1.29.0")) return model;
    return NULL;
}
static inline int input_dualshock_mode_button_enabled(void) { return input_dualshock_mode_model() != NULL; }
static inline int input_dualshock_raw_axes(void)
{
    const char *model = getenv("PSX_INPUT_ROUTE_AXIS_MODEL");
    return model && !strcmp(model, "octoshock-byte");
}
static inline const char *input_dualshock_controller_profile(void)
{
    if (input_dualshock_mode_model())
        return strcmp(input_dualshock_mode_model(), "octoshock-2.4.2") ? "nymashock-1.29.0-dualshock-mode"
                                                                       : "octoshock-2.4.2-dualshock-mode";
    return input_dualshock_raw_axes() ? "octoshock-byte-dualshock-neutral-analog"
                                      : "nymashock-2.9.1-dualshock-neutral-analog";
}
/* Host-side conversion of a route axis byte through a 16-bit input value
 * onto the protocol byte range. [NOT OBSERVED: sub-byte host input] The pad
 * reports the axis bytes it is given exactly, in the order RX, RY, LX, LY
 * [ORACLE FIXTURE P1, Octoshock 2.3]; that fixture takes whole bytes, so it
 * cannot observe this rounding. This is a source bridge, not a deadzone.
 * An Octoshock route's byte is already the protocol byte. */
static inline uint8_t input_dualshock_protocol_axis(uint8_t source)
{
    if (input_dualshock_raw_axes()) return source;
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
/* Same as input_dualshock_deliver, and latches the physical MODE button state
 * for the pad's DTR-edge evaluation (see sio.c). */
static inline void input_dualshock_deliver_mode(uint16_t buttons, const uint8_t source_ly_lx_ry_rx[4], uint8_t mode)
{
    input_dualshock_deliver(buttons, source_ly_lx_ry_rx);
    sio_set_pad_mode_button(0, mode);
}
#endif
