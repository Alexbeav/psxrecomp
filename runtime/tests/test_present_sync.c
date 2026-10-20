#include "present_sync.h"
#include <stdio.h>

static int failures;
static void check(int requested, int blocked, double guest, double host,
                  int expected) {
    const int actual = psx_present_sync_interval(requested, blocked, guest, host);
    if (actual != expected) {
        fprintf(stderr, "sync(%d,%d,%.3f,%.3f): got %d expected %d\n",
                requested, blocked, guest, host, actual, expected);
        ++failures;
    }
}

int main(void) {
    const double panels[] = {60, 75, 120, 144, 165, 240};
    for (unsigned i = 0; i < sizeof panels / sizeof panels[0]; ++i) {
        check(1, 0, 59.94, panels[i], 1);
        check(1, 0, 50, panels[i], 1); /* PAL on a faster panel */
        check(-1, 0, 59.94, panels[i], -1); /* explicit adaptive */
        check(0, 0, 59.94, panels[i], 0); /* explicit VSync off */
        check(2, 0, 59.94, panels[i], 0); /* VRR frame generation */
        check(1, 1, 59.94, panels[i], 0); /* scheduler/netplay/recovery */
    }
    check(1, 0, 50, 50, 1);
    check(1, 0, 60, 59, 1);
    check(1, 0, 59.94, 50, 0); /* do not slow the guest to the panel */
    check(1, 0, 120, 60, 0);
    check(1, 0, 59.94, 0, 0); /* unknown display */
    check(1, 0, 0, 144, 0); /* uncapped guest */
    puts(failures ? "present sync FAILED" : "present sync passed (42 cases)");
    return failures ? 1 : 0;
}
