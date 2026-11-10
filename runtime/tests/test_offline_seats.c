/* offline_seats.h: which console ports the offline pad sampler reads.
 * PS1B-279: a pad next to a PS1 Mouse in a one-player title was never read. */
#include "offline_seats.h"

#include <stdio.h>

static int fails;

static void expect(int got, int want, const char* what) {
    if (got == want) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s: got %d, want %d\n", what, got, want);
    ++fails;
}

int main(void) {
    /* Unchanged without a mouse seat: game.toml players, clamped. */
    expect(offline_sampled_seat_count(1, 0, 0, 2), 1, "one-player title reads port 1 only");
    expect(offline_sampled_seat_count(2, 0, 0, 2), 2, "two-player title reads both ports");
    expect(offline_sampled_seat_count(0, 0, 0, 2), 1, "unset players reads port 1");
    expect(offline_sampled_seat_count(4, 0, 0, 2), 2, "clamped to the build's seat count");
    expect(offline_sampled_seat_count(4, 0, 0, 5), 4, "multitap title keeps its count");
    expect(offline_sampled_seat_count(1, 1, 0, 2), 2, "a live port swap reads port 2");

    /* A mouse seat makes the other port live for the same player. */
    expect(offline_sampled_seat_count(1, 0, 1, 2), 2, "one-player title with a mouse reads port 2");
    expect(offline_sampled_seat_count(0, 0, 1, 2), 2, "unset players with a mouse reads port 2");
    expect(offline_sampled_seat_count(4, 0, 1, 5), 4, "a mouse never lowers a multitap count");
    expect(offline_sampled_seat_count(1, 1, 1, 2), 2, "swap and mouse together read two ports");

    if (fails) { fprintf(stderr, "offline_seats_test: %d failure(s)\n", fails); return 1; }
    printf("offline_seats_test: all checks passed\n");
    return 0;
}
