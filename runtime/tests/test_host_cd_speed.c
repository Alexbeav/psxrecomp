#include "host_cd_speed.h"
#include <assert.h>
#include <string.h>

static int active = 1, writes;
void cdrom_debug_snapshot(CDROMDebugState *state) {
    memset(state, 0, sizeof(*state));
    state->speed_divisor = active;
}
void cdrom_set_speed(int divisor) { active = divisor; ++writes; }

int main(void) {
    assert(host_cd_speed_toggle(2, 0, 0) == -1 && writes == 0);
    assert(host_cd_speed_toggle(2, 1, 1) == -1 && writes == 0);
    assert(host_cd_speed_toggle(-1, 1, 0) == -1 && writes == 0);
    active = 2; /* Runtime applies configured speed at game entry. */
    assert(host_cd_speed_toggle(2, 1, 0) == 1);
    assert(host_cd_speed_toggle(2, 1, 0) == 2);
    assert(host_cd_speed_toggle(2, 1, 0) == 1);
    assert(host_cd_speed_toggle(1, 1, 0) == 1);
    assert(host_cd_speed_toggle(0, 1, 0) == 0);
    assert(host_cd_speed_toggle(0, 1, 0) == 1);
    assert(host_cd_speed_toggle(1024, 1, 0) == 1024);
    assert(host_cd_speed_toggle(1024, 1, 0) == 1);
    return 0;
}
