#ifndef PSX_INPUT_ROUTE_FIELD_CLOCK_H
#define PSX_INPUT_ROUTE_FIELD_CLOCK_H

#include <stdint.h>

/* Cold-boot comparison clock, independent of guest memory and input contents.
 * Behavioral constants: original Octoshock 2.2.2 GPU, NTSC hardware clock
 * ratio 103896/65536; alternating 3412/3413 GPU clocks per scanline;
 * 263 progressive lines, 263/262 interlaced lines. This is a field-duration
 * profile, not its complete scanline/IRQ/CPU timing implementation.
 * Independently written arithmetic; no reference implementation copied.
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
