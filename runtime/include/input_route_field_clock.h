#ifndef PSX_INPUT_ROUTE_FIELD_CLOCK_H
#define PSX_INPUT_ROUTE_FIELD_CLOCK_H

#include <stdint.h>

/* Cold-boot comparison clock, independent of guest memory and input contents.
 * NTSC field-duration profile. PSX-SPX "Nominal Video Clock" and "Vertical
 * Video Timings" give the NTSC video clock (53.693175 MHz against the
 * 33.8688 MHz CPU clock) and 263 lines per progressive field, 262.5 per
 * interlaced field. The ratio 103896/65536 GPU clocks per CPU cycle and the
 * alternating 3412 / 3413 GPU clocks per line are fitted to oracle
 * observation, not taken from PSX-SPX (which says 3413, "or 3413.6 or so").
 * They are the same constants as the measured raster clock
 * (runtime/tests/raster_clock_provenance.json). No current route selects this
 * profile; input_route_field_clock_test pins the arithmetic. This is a
 * field-duration profile, not a complete scanline, IRQ or CPU timing model.
 */
typedef struct InputRouteFieldClock {
    uint32_t remainder;
    uint32_t line_phase;
    uint32_t field;
    uint32_t current_cycles;
} InputRouteFieldClock;

static inline void input_route_field_clock_select(InputRouteFieldClock *s,
                                                  int interlaced) {
    uint32_t lines = interlaced ? 263u - s->field : 263u;
    uint32_t gpu_clocks = lines * 3412u + (lines + s->line_phase) / 2u;
    uint64_t numerator = (uint64_t)gpu_clocks * 65536u;
    /* Keep cumulative deadlines at ceil(total GPU clocks / clock ratio).
     * Represent the remainder as unused fractional CPU-cycle credit. */
    numerator -= s->remainder;
    s->current_cycles = (uint32_t)((numerator + 103895u) / 103896u);
    s->remainder = (uint32_t)((uint64_t)s->current_cycles * 103896u - numerator);
    s->line_phase = (s->line_phase + lines) & 1u;
}

static inline void input_route_field_clock_reset(InputRouteFieldClock *s) {
    s->remainder = s->line_phase = s->field = 0u;
    input_route_field_clock_select(s, 0);
}

static inline void input_route_field_clock_next(InputRouteFieldClock *s,
                                                int interlaced) {
    s->field = interlaced ? !s->field : 0u;
    input_route_field_clock_select(s, interlaced);
}
#endif
