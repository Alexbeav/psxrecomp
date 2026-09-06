#include "controller_policy.h"

int controller_policy_effective_mode(int configured_mode, int policy_present,
                                     uint32_t policy_request)
{
    if (!policy_present)
        return configured_mode;
    if (policy_request == (uint32_t)PSX_CTRL_MODE_ANALOG)
        return PSX_CTRL_MODE_ANALOG;
    if (policy_request == (uint32_t)PSX_CTRL_MODE_DIGITAL)
        return PSX_CTRL_MODE_DIGITAL;
    return configured_mode;   /* invalid / no request: keep the configured seat mode */
}

void controller_pad_compose_physical(int effective_mode, const uint8_t raw[4],
                                     uint8_t out[4])
{
    int i;
    if (effective_mode == PSX_CTRL_MODE_ANALOG) {
        for (i = 0; i < 4; i++) out[i] = raw[i];
    } else {
        for (i = 0; i < 4; i++) out[i] = 0x80u;
    }
}

void controller_pad_compose_injected(int effective_mode, int stick_live,
                                     uint16_t buttons, uint8_t inout[4])
{
    if (effective_mode != PSX_CTRL_MODE_ANALOG) {
        inout[0] = inout[1] = inout[2] = inout[3] = 0x80u;
        return;
    }
    if (stick_live)
        return;   /* a live stick override wins; nothing folded */
    if ((uint16_t)(~buttons & 0x0010u)) inout[1] = 0x00u; /* Up    */
    if ((uint16_t)(~buttons & 0x0040u)) inout[1] = 0xFFu; /* Down  */
    if ((uint16_t)(~buttons & 0x0080u)) inout[0] = 0x00u; /* Left  */
    if ((uint16_t)(~buttons & 0x0020u)) inout[0] = 0xFFu; /* Right */
}

int controller_pad_sample_has_dpad_fold(const uint8_t presented[4],
                                        const uint8_t raw[4])
{
    /* A fold only ever writes 0x00/0xFF into lx/ly; if the presented value is
     * an extreme that the raw stick did not produce, it came from a fold. */
    int i;
    for (i = 0; i < 2; i++) {
        if ((presented[i] == 0x00u || presented[i] == 0xFFu) && presented[i] != raw[i])
            return 1;
    }
    return 0;
}
