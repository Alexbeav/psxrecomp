#include "input_route_field_clock.h"
#include <assert.h>
#include <stdio.h>

/* Independent scanline-by-scanline oracle; mixed display modes also test
 * phase continuity. No retail data, emulator source or desired outcome. */
int main(void) {
    InputRouteFieldClock s;
    uint64_t gpu_total = 0, cpu_total = 0;
    unsigned phase = 0, field = 0;
    input_route_field_clock_reset(&s);
    for (unsigned n = 0; n < 100000; ++n) {
        int interlaced = n >= 17 && (n % 97) < 89;
        if (n) {
            input_route_field_clock_next(&s, interlaced);
            field = interlaced ? !field : 0;
        } else interlaced = 0;
        unsigned lines = interlaced ? 263 - field : 263;
        for (unsigned line = 0; line < lines; ++line) {
            gpu_total += 3412 + phase;
            phase ^= 1;
        }
        cpu_total += s.current_cycles;
        assert(cpu_total == (gpu_total * 65536 + 103895) / 103896);
        assert(s.line_phase == phase && s.field == field);
        assert(s.remainder < 103896);
        assert(s.current_cycles >= 563968 && s.current_cycles <= 566122);
    }
    input_route_field_clock_reset(&s);
    assert(s.current_cycles == (263ull * 3412 * 65536 + 131ull * 65536 + 103895) / 103896);
    puts("field clock: 100000 mixed fields match independent scanline oracle");
    return 0;
}
