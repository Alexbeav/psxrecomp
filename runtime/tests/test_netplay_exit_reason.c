/* Netplay exit reasons and the boot-mismatch deadline (PS1B-290).
 * Plain checks, not assert(): Release test builds define NDEBUG. */
#include "netplay_exit_reason.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    ++failures; } } while (0)

int main(void)
{
    static const char *const ended[] = {
        "netplay_boot_mismatch", "netplay_peer_disconnect", "netplay_load_failed",
        "netplay_load_stall", "netplay_link_stall", "netplay_admit_stall",
    };
    size_t i;

    /* Every origin netplay_soft_exit uses for a failure has a sentence, and it
     * fits the lobby client's last_error (192 bytes). */
    for (i = 0; i < sizeof(ended) / sizeof(ended[0]); ++i) {
        const char *t = netplay_exit_reason_text(ended[i]);
        CHECK(t != NULL);
        CHECK(t && strlen(t) > 20 && strlen(t) < 192);
    }
    CHECK(strstr(netplay_exit_reason_text("netplay_boot_mismatch"), "BIOS") != NULL);

    /* A player who closed the window or pressed Escape needs no reason. */
    CHECK(netplay_exit_reason_text("sdl_window_close") == NULL);
    CHECK(netplay_exit_reason_text("netplay_escape") == NULL);
    CHECK(netplay_exit_reason_text("netplay_barrier_escape") == NULL);
    CHECK(netplay_exit_reason_text("") == NULL);
    CHECK(netplay_exit_reason_text(NULL) == NULL);

    /* No mismatch seen: never final. */
    CHECK(!netplay_boot_mismatch_final(0u, 100000u, NETPLAY_BOOT_MISMATCH_GRACE_MS));
    /* Seen, grace not yet over, then over. */
    CHECK(!netplay_boot_mismatch_final(1000u, 3999u, NETPLAY_BOOT_MISMATCH_GRACE_MS));
    CHECK(netplay_boot_mismatch_final(1000u, 4000u, NETPLAY_BOOT_MISMATCH_GRACE_MS));
    /* The 32-bit millisecond clock wraps. */
    CHECK(!netplay_boot_mismatch_final(0xFFFFFF00u, 0x00000100u, NETPLAY_BOOT_MISMATCH_GRACE_MS));
    CHECK(netplay_boot_mismatch_final(0xFFFFFF00u, 0x00000C00u, NETPLAY_BOOT_MISMATCH_GRACE_MS));
    /* The old behaviour was a 20 s admit-stall watchdog; the grace is far
     * shorter than that. */
    CHECK(NETPLAY_BOOT_MISMATCH_GRACE_MS < 20000u);

    if (failures) {
        fprintf(stderr, "netplay_exit_reason: %d check(s) FAILED\n", failures);
        return 1;
    }
    puts("netplay_exit_reason: PASS");
    return 0;
}
