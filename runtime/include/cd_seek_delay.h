#ifndef PSX_CD_SEEK_DELAY_H
#define PSX_CD_SEEK_DELAY_H
#include <stdint.h>

/* Numerical compatibility model fitted to independently authored seek inputs.
 * This is not a hardware timing guarantee. See cd_seek_provenance.json.
 */
static inline int psx_cd_seek_delay(int origin, int target, int motor_on,
                                    int paused, uint8_t mode, int profile)
{
    int64_t delta = (int64_t)target - (motor_on ? origin : 0);
    uint64_t distance = (uint64_t)(delta < 0 ? -delta : delta);
    uint64_t cycles = distance * 1568u / 15u;
    if (cycles < 20000u) cycles = 20000u;
    if (!motor_on) cycles += 33868800u;

    unsigned speed = (mode & 128u) ? 2u : 1u;
    if (distance >= 2250u) cycles += 10160640u;
    else if (paused) cycles += 2475904u / speed;
    else if (profile && distance >= 3u && distance <= 11u)
        cycles += 1806336u / speed;

    return cycles > INT32_MAX ? INT32_MAX : (int)cycles;
}
#endif
