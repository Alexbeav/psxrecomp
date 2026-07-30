/*
 * test_netplay_hash_confirm.c — watermark advance / mismatch hold.
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -I../include \
 *       -o test_netplay_hash_confirm test_netplay_hash_confirm.c \
 *       ../src/netplay_hash_confirm.c && ./test_netplay_hash_confirm
 */
#include "netplay_hash_confirm.h"

#include <stdio.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    NetplayHashConfirm hc;
    netplay_hc_reset(&hc);

    CHECK(!netplay_hc_confirm_through(&hc, 0), "empty not confirmed");
    CHECK(netplay_hc_resolved_through(&hc) == 0u, "resolved starts 0");

    netplay_hc_note_local(&hc, 0, 0x1111u);
    netplay_hc_note_local(&hc, 1, 0x2222u);
    netplay_hc_note_local(&hc, 2, 0x3333u);
    CHECK(!netplay_hc_confirm_through(&hc, 0), "local-only not enough");

    netplay_hc_note_peer(&hc, 0, 0x1111u);
    CHECK(netplay_hc_confirm_through(&hc, 0), "tick 0 matched");
    CHECK(netplay_hc_resolved_through(&hc) == 0u, "resolved at 0");
    CHECK(!netplay_hc_confirm_through(&hc, 1), "tick 1 not yet");

    netplay_hc_note_peer(&hc, 1, 0x2222u);
    CHECK(netplay_hc_confirm_through(&hc, 1), "tick 1 matched");
    CHECK(netplay_hc_resolved_through(&hc) == 1u, "resolved at 1");

    /* Mismatch must not advance past the gap. */
    netplay_hc_note_peer(&hc, 2, 0xDEADu);
    CHECK(netplay_hc_resolved_through(&hc) == 1u, "mismatch holds watermark");
    CHECK(!netplay_hc_confirm_through(&hc, 2), "tick 2 not confirmed");

    /* Late local after peer (reverse order) still advances. */
    netplay_hc_reset(&hc);
    netplay_hc_note_peer(&hc, 0, 0xAAAAu);
    netplay_hc_note_peer(&hc, 1, 0xBBBBu);
    CHECK(!netplay_hc_confirm_through(&hc, 0), "peer-only not enough");
    netplay_hc_note_local(&hc, 0, 0xAAAAu);
    CHECK(netplay_hc_resolved_through(&hc) == 0u, "advance on late local 0");
    netplay_hc_note_local(&hc, 1, 0xBBBBu);
    CHECK(netplay_hc_resolved_through(&hc) == 1u, "advance on late local 1");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
