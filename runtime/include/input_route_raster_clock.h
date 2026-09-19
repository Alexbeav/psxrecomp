#ifndef INPUT_ROUTE_RASTER_CLOCK_H
#define INPUT_ROUTE_RASTER_CLOCK_H
#include <stdint.h>

typedef struct {
    uint64_t cycle, last_rise;
    uint32_t fraction, remaining, phase, alternate, scanline, lines, field, mode;
    uint32_t start, end, blank, rises, y_start, y_offset, readout_y, readout_field;
} InputRouteRasterClock;
typedef void (*InputRouteRasterEvent)(void *, uint64_t, unsigned, int);

/* Independent measured timing model. See raster_clock_provenance.json. */
static inline void input_route_raster_reset(InputRouteRasterClock *s)
{
    *s = (InputRouteRasterClock){0};
    s->remaining = 3212;
    s->lines = 263;
    s->start = 16;
    s->end = 256;
    s->blank = 1;
}

static inline void input_route_raster_boundary(InputRouteRasterClock *s,
    uint64_t time, InputRouteRasterEvent event, void *context)
{
    if (!s->phase) {
        s->phase = 1;
        s->remaining = 200;
        if (event) event(context, time, 1, (int)s->blank);
        return;
    }
    s->phase = 0;
    s->alternate ^= 1;
    s->remaining = 3213 - s->alternate;
    ++s->scanline;
    if (s->scanline == s->lines - 1)
        s->field = (s->mode & 32) ? (s->field ^ 1) : 0;
    if (s->scanline >= s->lines) {
        s->scanline = 0;
        s->lines = 263 - s->field;
    }
    unsigned old_blank = s->blank;
    if (s->scanline == s->end) {
        s->blank = 1;
        if (!old_blank) {
            s->y_offset = 0;
            s->readout_field = (s->mode & 36) == 36 ? (s->field ^ 1) : 0;
        }
    }
    if (s->scanline == s->start) s->blank = 0;
    if (s->blank != old_blank) {
        if (s->blank) {
            s->last_rise = time;
            ++s->rises;
        }
        if (event) event(context, time, 2, (int)s->blank);
    }
    s->readout_y = s->y_start;
    if (!s->blank) {
        unsigned offset = s->y_offset;
        if ((s->mode & 36) == 36) offset = offset * 2 + s->readout_field;
        s->readout_y = (s->y_start + offset) & 511;
        s->y_offset = (s->y_offset + 1) & 511;
    }
}

static inline void input_route_raster_advance_observed(InputRouteRasterClock *s,
    uint32_t cycles, InputRouteRasterEvent event, void *context)
{
    uint64_t fixed = s->fraction + (uint64_t)cycles * 103896;
    uint64_t ticks = fixed >> 16, consumed = 0;
    while (ticks >= s->remaining) {
        consumed += s->remaining;
        ticks -= s->remaining;
        uint64_t time = s->cycle + (consumed * 65536 - s->fraction + 103895) / 103896;
        input_route_raster_boundary(s, time, event, context);
    }
    s->remaining -= (uint32_t)ticks;
    s->fraction = (uint32_t)fixed & 65535;
    s->cycle += cycles;
}

static inline void input_route_raster_advance(InputRouteRasterClock *s, uint32_t cycles)
{
    input_route_raster_advance_observed(s, cycles, 0, 0);
}

static inline int input_route_raster_gp1(InputRouteRasterClock *s, uint32_t word)
{
    switch ((word >> 24) & 63) {
    case 0: s->mode = 0; s->start = 16; s->end = 256; s->y_start = 0; break;
    case 5: s->y_start = (word >> 10) & 511; break;
    case 7: s->start = word & 1023; s->end = (word >> 10) & 1023; break;
    case 8:
        if (word & 8) return 0;
        s->mode = word & 255;
        break;
    }
    return 1;
}

static inline uint32_t input_route_raster_status(const InputRouteRasterClock *s)
{
    return ((s->field ^ 1u) << 13) | ((s->readout_y & 1u) << 31);
}

static inline uint32_t input_route_raster_until_rise(const InputRouteRasterClock *s)
{
    InputRouteRasterClock future = *s;
    uint64_t ticks = 0;
    /* Two complete fields cover a missed start or an interlace-length change. */
    for (unsigned edge = 0; edge < 1056; ++edge) {
        ticks += future.remaining;
        input_route_raster_boundary(&future, 0, 0, 0);
        if (future.rises != s->rises) {
            uint64_t distance = (ticks * 65536 - s->fraction + 103895) / 103896;
            return distance > UINT32_MAX ? UINT32_MAX : (uint32_t)distance;
        }
    }
    return UINT32_MAX;
}
#endif
