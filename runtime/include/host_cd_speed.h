#ifndef PSX_HOST_CD_SPEED_H
#define PSX_HOST_CD_SPEED_H
#include "cdrom.h"

/* Session-only user action. Never alter configured startup timing or BIOS.
 * Return effective speed, or -1 when changing timing is not permitted. */
static inline int host_cd_speed_toggle(int configured, int game_started,
                                      int netplay_active) {
    CDROMDebugState state;
    if (!game_started || netplay_active || configured < 0) return -1;
    cdrom_debug_snapshot(&state);
    const int target = state.speed_divisor == 1 ? configured : 1;
    if (target != state.speed_divisor) cdrom_set_speed(target);
    cdrom_debug_snapshot(&state);
    return state.speed_divisor;
}
#endif
