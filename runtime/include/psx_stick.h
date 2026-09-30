#ifndef PSX_STICK_H
#define PSX_STICK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert one SDL stick vector to the two unsigned DualShock axis bytes.
 *
 * deadzone and anti_deadzone use SDL's 0..32767 radial magnitude scale.
 * Values inside deadzone are centred. Outside it, magnitude is remapped from
 * anti_deadzone to full travel while direction is preserved. anti_deadzone=0
 * is an ordinary rescaled radial deadzone.
 */
void psx_stick_to_dualshock(int16_t x, int16_t y,
                            int deadzone, int anti_deadzone,
                            uint8_t *out_x, uint8_t *out_y);

/*
 * Convert one SDL axis alone to an unsigned axis byte, for a one-dimensional
 * control such as the neGcon twist. The radial transform above scales each
 * axis by the whole stick's length, so moving the other axis would move this
 * one. This uses the same deadzone and anti_deadzone with the other axis held
 * at centre: the result depends on `value` only, and matches
 * psx_stick_to_dualshock for a stick pushed straight left or right.
 */
uint8_t psx_stick_axis_to_byte(int16_t value, int deadzone, int anti_deadzone);

#ifdef __cplusplus
}
#endif

#endif
