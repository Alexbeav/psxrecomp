#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "frame_interpolation.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

static int run_interval(FrameInterpolationSchedule *schedule, uint64_t now,
                        double source_hz, double target_hz,
                        float *first_alpha, float *last_alpha) {
    uint64_t deadline;
    float alpha;
    int count = 0;

    CHECK(frame_interpolation_schedule_begin(
              schedule, now, 1000000u, source_hz, target_hz),
          "valid rates should produce an interval");
    while (frame_interpolation_schedule_next(
               schedule, now, &deadline, &alpha)) {
        CHECK(deadline >= now, "presentation deadline must not be in the past");
        CHECK(deadline <= frame_interpolation_schedule_end(schedule),
              "presentation deadline must stay inside the source interval");
        if (count == 0 && first_alpha) *first_alpha = alpha;
        if (last_alpha) *last_alpha = alpha;
        count++;
    }
    return count;
}

int main(void) {
    FrameInterpolationSchedule schedule = {0};
    float first = 0.0f, last = 0.0f;
    uint64_t end;

    CHECK(run_interval(&schedule, 1000000u, 60.0, 120.0,
                       &first, &last) == 2,
          "120 Hz should schedule two presents per 60 Hz source interval");
    CHECK(fabsf(first - 0.5f) < 0.001f && fabsf(last - 1.0f) < 0.001f,
          "120 Hz blend phases should be one-half and complete");

    frame_interpolation_schedule_reset(&schedule);
    end = 2000000u;
    int total = 0;
    for (int i = 0; i < 5; i++) {
        total += run_interval(&schedule, end, 60.0, 144.0, NULL, NULL);
        end = frame_interpolation_schedule_end(&schedule) + 1000u;
    }
    CHECK(total == 12,
          "144 Hz should retain fractional cadence across five source frames");

    frame_interpolation_schedule_reset(&schedule);
    CHECK(run_interval(&schedule, 3000000u, 60.0, 165.0, NULL, NULL) >= 2,
          "165 Hz should schedule multiple presents");
    end = frame_interpolation_schedule_end(&schedule);
    CHECK(frame_interpolation_schedule_begin(
              &schedule, end + 500u, 1000000u, 60.0, 165.0),
          "normal guest work should keep the existing cadence anchor");
    CHECK(frame_interpolation_schedule_end(&schedule) == end + 16667u ||
          frame_interpolation_schedule_end(&schedule) == end + 16666u,
          "guest work must consume, not extend, the next source interval");

    CHECK(!frame_interpolation_schedule_begin(
               &schedule, 0u, 0u, 60.0, 120.0),
          "zero host frequency should be rejected");
    /* A limiter below the guest rate: 30 presents/s from a 50 Hz PAL guest
     * means three presents across every five guest intervals, and the guest
     * interval itself (the pacing clock) is not stretched. */
    frame_interpolation_schedule_reset(&schedule);
    end = 4000000u;
    total = 0;
    for (int i = 0; i < 50; i++) {
        total += run_interval(&schedule, end, 50.0, 30.0, NULL, NULL);
        CHECK(frame_interpolation_schedule_end(&schedule) - end <= 20001u,
              "a low cap must not stretch the 20 ms PAL guest interval");
        end = frame_interpolation_schedule_end(&schedule);
    }
    CHECK(total >= 29 && total <= 31,
          "30 Hz cap should present ~30 times across one second of 50 Hz");

    /* Unlimited: back-to-back presents until the interval ends, alpha rising,
     * and still one present when the guest already overran the interval. */
    frame_interpolation_schedule_reset(&schedule);
    CHECK(frame_interpolation_schedule_begin(
              &schedule, 5000000u, 1000000u, 50.0, 0.0),
          "unlimited should be accepted");
    {
        uint64_t deadline, t = 5000000u;
        float alpha, prev_alpha = -1.0f;
        int n = 0, monotonic = 1;
        while (frame_interpolation_schedule_next(&schedule, t, &deadline,
                                                 &alpha)) {
            if (alpha < prev_alpha) monotonic = 0;
            prev_alpha = alpha;
            t += 1000u;          /* each present costs 1 ms of host time */
            n++;
            if (n > 1000) break;
        }
        CHECK(n == 20, "unlimited presents back-to-back across 20 ms");
        CHECK(monotonic, "unlimited blend position must not go backwards");
        /* With a measured 3 ms present cost, the presenter must not start
         * a present that would run past the guest interval. */
        frame_interpolation_schedule_reset(&schedule);
        frame_interpolation_schedule_note_cost(&schedule, 3000u);
        CHECK(frame_interpolation_schedule_begin(
                  &schedule, 6000000u, 1000000u, 50.0, 0.0),
              "unlimited with a cost estimate");
        t = 6000000u;
        n = 0;
        while (frame_interpolation_schedule_next(&schedule, t, &deadline,
                                                 &alpha)) {
            t += 3000u;
            n++;
            if (n > 1000) break;
        }
        CHECK(t <= frame_interpolation_schedule_end(&schedule),
              "a known present cost must not overrun the guest interval");
        CHECK(n == 6, "3 ms presents fit six times in a 20 ms interval");
        /* A present slower than the guest interval (25 ms vs 20 ms) must
         * not slow the guest: presents alternate with skipped intervals and
         * the interval ends never drift. */
        frame_interpolation_schedule_reset(&schedule);
        frame_interpolation_schedule_note_cost(&schedule, 25000u);
        t = 7000000u;
        {
            int presents = 0;
            uint64_t first_end = 0, last_end = 0;
            for (int i = 0; i < 20; i++) {
                CHECK(frame_interpolation_schedule_begin(
                          &schedule, t, 1000000u, 50.0, 0.0),
                      "slow-GPU unlimited interval");
                if (i == 0) first_end = frame_interpolation_schedule_end(&schedule);
                while (frame_interpolation_schedule_next(
                           &schedule, t, &deadline, &alpha)) {
                    t += 25000u;
                    presents++;
                }
                last_end = frame_interpolation_schedule_end(&schedule);
                if (t < last_end) t = last_end;   /* wait out the interval */
            }
            CHECK(presents >= 9 && presents <= 10,
                  "a too-slow present alternates with skipped intervals");
            CHECK(last_end - first_end == 19u * 20000u,
                  "slow presents must not stretch guest intervals");
        }
        CHECK(frame_interpolation_schedule_begin(
                  &schedule, t + 100000u, 1000000u, 50.0, 0.0),
              "unlimited re-anchors after an overrun");
    }

    CHECK(!frame_interpolation_schedule_begin(
               &schedule, 0u, 1000000u, 50.0, 0.5),
          "a cap below 1 Hz should be rejected");
    CHECK(!frame_interpolation_schedule_begin(
               &schedule, 0u, 1000000u, 50.0, 2000.0),
          "a finite cap above 1000 Hz should be rejected");

    /* Variable-rate titles: blend across the game frame's real duration. */
    CHECK(fabsf(frame_interpolation_span_alpha(0.5f, 0, 1) - 0.5f) < 1e-6f,
          "one-VBlank frames keep the interval alpha");
    CHECK(fabsf(frame_interpolation_span_alpha(1.0f, 1, 1) - 1.0f) < 1e-6f,
          "a late one-VBlank frame holds");
    CHECK(fabsf(frame_interpolation_span_alpha(0.5f, 0, 2) - 0.25f) < 1e-6f,
          "first half of a two-VBlank frame");
    CHECK(fabsf(frame_interpolation_span_alpha(0.5f, 1, 2) - 0.75f) < 1e-6f,
          "second half of a two-VBlank frame");
    CHECK(fabsf(frame_interpolation_span_alpha(1.0f, 2, 3) - 1.0f) < 1e-6f,
          "end of a three-VBlank frame completes the blend");
    CHECK(fabsf(frame_interpolation_span_alpha(0.5f, 5, 3) - 1.0f) < 1e-6f,
          "a frame that runs long holds the newest image");

    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
