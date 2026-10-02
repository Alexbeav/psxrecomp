/* Netplay exit reasons and the boot-mismatch deadline (PS1B-290), and the
 * refusal of a seat that is not a pad (PS1B-313).
 * Plain checks, not assert(): Release test builds define NDEBUG. */
#include "netplay_exit_reason.h"
#include "sio.h"

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

    /* A pad seat is unchanged: no refusal and no text, on any port. */
    {
        char why[NETPLAY_SEAT_REFUSAL_CAP];
        static const struct { int device; const char *name; } other[] = {
            { SIO_DEVICE_MOUSE, "PS1 Mouse" }, { SIO_DEVICE_NEGCON, "neGcon" },
            { SIO_DEVICE_GUNCON, "GunCon" },
        };
        memset(why, 'x', sizeof(why));
        CHECK(netplay_seat_refusal(SIO_DEVICE_PAD, 1, why, sizeof(why)) == 0);
        CHECK(why[0] == '\0');
        CHECK(netplay_seat_refusal(SIO_DEVICE_PAD, 2, why, sizeof(why)) == 0);
        /* A Mouse, a neGcon and a GunCon are refused with one sentence that
         * names the device and the port and fits last_error (192 bytes). */
        for (i = 0; i < sizeof(other) / sizeof(other[0]); ++i) {
            CHECK(netplay_seat_refusal(other[i].device, 2, why, sizeof(why)) == 1);
            CHECK(strstr(why, other[i].name) != NULL);
            CHECK(strstr(why, "port 2") != NULL);
            CHECK(strstr(why, "pad or the keyboard") != NULL);
            CHECK(strlen(why) > 20 && strlen(why) < 192);
        }
        CHECK(netplay_seat_refusal(SIO_DEVICE_MOUSE, 1, why, sizeof(why)) == 1);
        CHECK(strstr(why, "port 1 is set to a PS1 Mouse") != NULL);
        /* A device kind added later is refused until netplay can carry it. */
        CHECK(netplay_seat_refusal(99, 1, why, sizeof(why)) == 1);
        CHECK(strstr(why, "port 1") != NULL);
        /* The verdict does not need a buffer, and a short one is not overrun. */
        CHECK(netplay_seat_refusal(SIO_DEVICE_GUNCON, 1, NULL, 0) == 1);
        memset(why, 'x', sizeof(why));
        CHECK(netplay_seat_refusal(SIO_DEVICE_GUNCON, 1, why, 8) == 1);
        CHECK(strlen(why) == 7 && why[8] == 'x');
    }

    if (failures) {
        fprintf(stderr, "netplay_exit_reason: %d check(s) FAILED\n", failures);
        return 1;
    }
    puts("netplay_exit_reason: PASS");
    return 0;
}
