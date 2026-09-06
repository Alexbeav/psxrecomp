/* Pure controller-presentation helpers, extracted from the SIO samplers in
 * main.cpp so the mode/sample composition can be tested without SDL or the
 * plugin registry (tests/test_controller_policy_switch.c).
 *
 * Invariants (hardware-faithful DualShock):
 *   - a physical sample never derives stick deflection from the D-pad;
 *   - DIGITAL presents centred sticks; ANALOG presents the raw sticks;
 *   - a policy decision applies to the sample it was made for — there is no
 *     carried "hybrid" latch, so a mode change between two samples produces
 *     two clean samples and never a mixed one. */
#ifndef PSX_CONTROLLER_POLICY_H
#define PSX_CONTROLLER_POLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors PSXRecompV4::PAD_MODE_* (recompiler/src/config_loader.h) and
 * PSX_MOD_CONTROLLER_* (mod_plugins.h): ANALOG = 1, DIGITAL = 2 in both.
 * main.cpp static_asserts the three encodings agree. */
enum {
    PSX_CTRL_MODE_ANALOG  = 1,
    PSX_CTRL_MODE_DIGITAL = 2
};
#define PSX_CTRL_POLICY_NONE 0xFFFFFFFFu   /* no plugin policy installed / no request */

/* Effective presentation mode for one sample.
 *   configured_mode : the seat's configured mode (DIGITAL/ANALOG)
 *   policy_present  : non-zero when a trusted plugin policy callback is installed
 *   policy_request  : the callback's answer (PSX_CTRL_MODE_* ) or PSX_CTRL_POLICY_NONE
 * An absent policy, or an invalid request, yields configured_mode. */
int controller_policy_effective_mode(int configured_mode, int policy_present,
                                     uint32_t policy_request);

/* Compose the analog axes presented to the guest for a PHYSICAL sample.
 * raw[4] are the device sticks (0x80 centred); out[4] receives the presented
 * sticks: raw when effective_mode is ANALOG, centred otherwise. The D-pad word
 * is deliberately not an input: physical samples never fold it into a stick. */
void controller_pad_compose_physical(int effective_mode, const uint8_t raw[4],
                                     uint8_t out[4]);

/* Compose the axes for an INJECTED sample (debug server / set_input). This is
 * the one place a D-pad word may steer the left stick — only in ANALOG, only
 * when no stick override is live — and it is not hardware behaviour (see the
 * comment in main.cpp). buttons is the active-low PSX pad word. */
void controller_pad_compose_injected(int effective_mode, int stick_live,
                                     uint16_t buttons, uint8_t inout[4]);

/* 1 when the presented sample contains any stick deflection that could only
 * have come from a D-pad fold (used by the tests to assert the invariant). */
int controller_pad_sample_has_dpad_fold(const uint8_t presented[4],
                                        const uint8_t raw[4]);

#ifdef __cplusplus
}
#endif
#endif
