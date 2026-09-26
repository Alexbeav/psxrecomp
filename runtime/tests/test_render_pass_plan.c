/* Render-pass planning and presentation-selection math.
 * Build/run: ctest -R render_pass_plan_test */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "render_pass_plan.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

/* One 30 Hz game frame (2 VBlanks at 59.94 Hz) in nanosecond ticks. */
static const double kFrame = 2.0 * 1e9 / 59.94;

static void test_counts_per_rate(void) {
    static const struct { double hz; uint32_t lo, hi; } rates[] = {
        {60.0, 1, 2}, {100.0, 3, 4}, {120.0, 3, 4}, {200.0, 6, 7},
        {240.0, 7, 8}, {300.0, 9, 10},
    };
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        double period = 1e9 / rates[r].hz;
        /* Slide the output grid across one output period. */
        for (int k = 0; k < 16; k++) {
            RenderPassPlanInput in = {0};
            uint32_t a[RENDER_PASS_MAX_PHASES], wanted = 0, n;
            char msg[160];
            in.frame_start = 1e12;
            in.frame_length = kFrame;
            in.target_period = period;
            in.next_deadline = in.frame_start - 3.0 * period +
                               period * (double)k / 16.0;
            in.budget = -1.0;
            in.max = RENDER_PASS_MAX_PHASES;
            n = render_pass_plan_phases(&in, a, &wanted);
            snprintf(msg, sizeof msg, "%.0f Hz offset %d/16: %u passes",
                     rates[r].hz, k, (unsigned)n);
            CHECK(n >= rates[r].lo && n <= rates[r].hi, msg);
            CHECK(wanted == n, "unlimited budget sheds nothing");
            for (uint32_t i = 0; i < n; i++) {
                CHECK(a[i] > 0 && a[i] < 65536u, "phase inside (0, 1)");
                if (i) CHECK(a[i] > a[i - 1], "phases ascend");
                /* Each phase is an actual output deadline. */
                double d = in.frame_start + (double)a[i] / 65536.0 * kFrame;
                double m = fmod(d - in.next_deadline, period);
                if (m > period / 2) m -= period;
                snprintf(msg, sizeof msg,
                         "%.0f Hz phase %u lands on a deadline (off %.0f ns)",
                         rates[r].hz, (unsigned)i, m);
                CHECK(fabs(m) < kFrame / 65536.0 + 1.0, msg);
            }
        }
    }
}

static void test_shedding(void) {
    RenderPassPlanInput in = {0};
    uint32_t a[RENDER_PASS_MAX_PHASES], wanted = 0, n;
    in.frame_start = 1e12;
    in.frame_length = kFrame;
    in.target_period = 1e9 / 300.0;
    in.next_deadline = in.frame_start + 1e9 / 600.0;   /* half-period offset */
    in.max = RENDER_PASS_MAX_PHASES;
    in.pass_cost = 2e6;                                 /* 2 ms per pass */
    in.budget = 5e6;                                    /* 5 ms */
    n = render_pass_plan_phases(&in, a, &wanted);
    CHECK(wanted == 10, "300 Hz half-offset wants ten phases");
    CHECK(n == 2, "a 5 ms budget at 2 ms per pass affords two");
    CHECK(n == 2 && a[0] < 32768u && a[1] > 32768u,
          "a shed subset spreads across the frame");

    in.budget = 1e6;
    CHECK(render_pass_plan_phases(&in, a, &wanted) == 0 && wanted == 10,
          "less than one pass of budget renders none");

    in.budget = -1.0;
    in.max = 4;
    CHECK(render_pass_plan_phases(&in, a, NULL) == 4,
          "capacity caps the plan");

    in.max = RENDER_PASS_MAX_PHASES;
    in.target_period = 0.0;
    CHECK(render_pass_plan_phases(&in, a, &wanted) == 0 && wanted == 0,
          "no live output schedule plans nothing");
}

static void test_select(void) {
    const uint32_t ph[] = {0u, 16384u, 32768u, 49152u};
    uint32_t lo = 99, hi = 99;
    float t = -1.0f;
    CHECK(render_pass_select(ph, 4, 0.0, &lo, &hi, &t) && lo == 0 && hi == 0,
          "phase 0 shows the game's image");
    CHECK(render_pass_select(ph, 4, 0.25, &lo, &hi, &t) && lo == 1 && hi == 1,
          "an exact pass phase shows that pass alone");
    CHECK(render_pass_select(ph, 4, 0.375, &lo, &hi, &t) && lo == 1 && hi == 2 &&
          fabsf(t - 0.5f) < 1e-4f,
          "between passes the neighbours blend");
    CHECK(render_pass_select(ph, 4, 0.9, &lo, &hi, &t) && lo == 3 && hi == 3,
          "past the last pass it holds (the next frame is not known yet)");
    CHECK(render_pass_select(ph, 1, 0.6, &lo, &hi, &t) && lo == 0 && hi == 0,
          "with only the game image it holds");
    CHECK(!render_pass_select(ph, 0, 0.5, &lo, &hi, &t), "no items, no pick");
    CHECK(render_pass_select(ph, 4, -0.1, &lo, &hi, &t) && lo == 0 && hi == 0,
          "before the frame starts show its first image");
}

static void test_budget_and_ema(void) {
    CHECK(fabs(render_pass_budget(0, 0, 100.0, 0.5) - 50.0) < 1e-9,
          "no history spends the share of the frame");
    CHECK(fabs(render_pass_budget(20.0, 10.0, 100.0, 0.8) - 24.0) < 1e-9,
          "idle plus pass time, scaled");
    CHECK(render_pass_budget(500.0, 0.0, 100.0, 0.8) == 100.0,
          "never more than a frame");
    CHECK(render_pass_budget(10.0, 10.0, 0.0, 0.8) == 0.0, "no frame, no budget");
    CHECK(render_pass_ema(0.0, 4.0) == 4.0, "first sample seeds the average");
    CHECK(fabs(render_pass_ema(4.0, 8.0) - 5.0) < 1e-9, "quarter-weight update");
    CHECK(render_pass_ema(4.0, -1.0) == 4.0, "bad samples are ignored");
}

int main(void) {
    test_counts_per_rate();
    test_shedding();
    test_select();
    test_budget_and_ema();
    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
