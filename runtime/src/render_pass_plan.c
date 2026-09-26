#include "render_pass_plan.h"
#include "render_pass.h"

#include <math.h>

#define Q16 65536.0

static uint32_t to_q16(double p) {
    double q = floor(p * Q16 + 0.5);
    if (q < 1.0) q = 1.0;
    if (q > Q16 - 1.0) q = Q16 - 1.0;
    return (uint32_t)q;
}

uint32_t render_pass_plan_phases(const RenderPassPlanInput *in,
                                 uint32_t *alpha_q16, uint32_t *wanted) {
    double phases[RENDER_PASS_MAX_PHASES * 4u];
    uint32_t count = 0, cap, n;

    if (wanted) *wanted = 0;
    if (!in || !alpha_q16 || in->max == 0 || !(in->frame_length > 0.0) ||
        !isfinite(in->frame_length))
        return 0;
    cap = sizeof phases / sizeof phases[0];

    /* No live output schedule: nothing would show a pass. */
    if (!(in->target_period > 0.0) || !isfinite(in->target_period) ||
        !isfinite(in->next_deadline) || !isfinite(in->frame_start))
        return 0;
    {
        /* Walk the presenter's free-running output grid through the frame. */
        double end = in->frame_start + in->frame_length;
        double d = in->next_deadline;
        if (d <= in->frame_start) {
            double steps = floor((in->frame_start - d) / in->target_period) + 1.0;
            d += steps * in->target_period;
        }
        for (; d < end && count < cap; d += in->target_period) {
            double p = (d - in->frame_start) / in->frame_length;
            if (p < 1.0 / 64.0) continue;
            if (p >= 1.0) break;
            phases[count++] = p;
        }
    }
    if (count == 0) return 0;
    if (wanted) *wanted = count;

    n = count;
    if (n > in->max) n = in->max;
    if (n > RENDER_PASS_MAX_PHASES) n = RENDER_PASS_MAX_PHASES;
    if (in->budget >= 0.0) {
        double fit = in->pass_cost > 0.0 ? floor(in->budget / in->pass_cost)
                                         : (double)n;
        if (fit < 0.0) fit = 0.0;
        if (fit < (double)n) n = (uint32_t)fit;
    }
    if (n == 0) return 0;
    if (n == count) {
        for (uint32_t i = 0; i < n; i++) alpha_q16[i] = to_q16(phases[i]);
    } else {
        /* Evenly spread subset; the presenter blends across the gaps. */
        uint32_t last = UINT32_MAX, out = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t idx = (uint32_t)floor(((double)i + 0.5) *
                                           (double)count / (double)n);
            if (idx >= count) idx = count - 1;
            if (idx == last) continue;
            alpha_q16[out++] = to_q16(phases[idx]);
            last = idx;
        }
        n = out;
    }
    return n;
}

int render_pass_select(const uint32_t *phases, uint32_t n, double p,
                       uint32_t *lo, uint32_t *hi, float *t) {
    uint32_t i;
    double q, a, b, w;
    if (!phases || n == 0) return 0;
    q = p * Q16;
    if (!(q > (double)phases[0])) {
        *lo = *hi = 0;
        *t = 0.0f;
        return 1;
    }
    for (i = 0; i + 1 < n && (double)phases[i + 1] <= q; i++) { }
    if (i + 1 >= n) {
        *lo = *hi = n - 1;
        *t = 0.0f;
        return 1;
    }
    a = (double)phases[i];
    b = (double)phases[i + 1];
    w = b > a ? (q - a) / (b - a) : 0.0;
    if (w <= 1.0 / 256.0) {
        *lo = *hi = i;
        *t = 0.0f;
    } else if (w >= 1.0 - 1.0 / 256.0) {
        *lo = *hi = i + 1;
        *t = 0.0f;
    } else {
        *lo = i;
        *hi = i + 1;
        *t = (float)w;
    }
    return 1;
}

double render_pass_ema(double current, double sample) {
    if (!(sample >= 0.0) || !isfinite(sample)) return current;
    if (!(current > 0.0)) return sample;
    return current * 0.75 + sample * 0.25;
}

double render_pass_budget(double idle_ticks, double pass_ticks,
                          double frame_length, double share) {
    double b;
    if (!(frame_length > 0.0)) return 0.0;
    if (!(share > 0.0)) return 0.0;
    if (share > 1.0) share = 1.0;
    if (!(idle_ticks > 0.0) && !(pass_ticks > 0.0))
        return frame_length * share;
    b = ((idle_ticks > 0.0 ? idle_ticks : 0.0) +
         (pass_ticks > 0.0 ? pass_ticks : 0.0)) * share;
    if (b > frame_length) b = frame_length;
    return b;
}

int render_pass_mmio_class(uint32_t phys, uint32_t val, uint32_t width) {
    if (phys == 0x1F801810u) return width == 4 ? -1 : RENDER_PASS_DROP_GPU;
    if (phys == 0x1F801814u) {
        uint32_t cmd = val >> 24;
        return (width == 4 && (cmd == 0x04u || cmd == 0x10u))
            ? -1 : RENDER_PASS_DROP_GPU;
    }
    if (phys >= 0x1F801070u && phys <= 0x1F801077u) return -1;
    if ((phys >= 0x1F8010A0u && phys <= 0x1F8010AFu) ||   /* ch2 GPU */
        (phys >= 0x1F8010E0u && phys <= 0x1F8010EFu) ||   /* ch6 OTC */
        (phys >= 0x1F8010F0u && phys <= 0x1F8010F7u))     /* DPCR/DICR */
        return -1;
    if (phys >= 0x1F801080u && phys <= 0x1F8010FFu) return RENDER_PASS_DROP_DMA;
    if (phys >= 0x1F801C00u && phys <= 0x1F801FFFu) return RENDER_PASS_DROP_SPU;
    if (phys >= 0x1F801800u && phys <= 0x1F801803u) return RENDER_PASS_DROP_CD;
    if (phys >= 0x1F801100u && phys <= 0x1F80112Fu) return RENDER_PASS_DROP_TIMER;
    return RENDER_PASS_DROP_OTHER;
}
