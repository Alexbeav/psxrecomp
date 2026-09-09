#ifndef PSX_INPUT_ROUTE_RASTER_CLOCK_H
#define PSX_INPUT_ROUTE_RASTER_CLOCK_H
#include <stdint.h>
#include <string.h>

/* Independently expressed original Octoshock 2.2.2 NTSC raster timing.
 * Optional comparison clock only: no GPU rendering, implicit timer synchronization,
 * readiness flags, input policy or guest memory is implemented here.
 * Source gpu.cpp constants/transition rules; no reference source text copied.
 */
typedef struct InputRouteRasterClock {
    uint64_t cycle, last_rise;
    uint32_t fraction, remaining, phase, alternate;
    uint32_t scanline, lines, field, mode, start, end, blank;
    uint32_t rises;
    uint32_t y_start, y_offset, readout_y, readout_field;
} InputRouteRasterClock;

static inline void input_route_raster_reset(InputRouteRasterClock *s) {
    memset(s, 0, sizeof(*s));
    s->remaining=3212; s->lines=263; s->start=16; s->end=256; s->blank=1;
}

typedef void (*InputRouteRasterEvent)(void *, uint64_t, unsigned, int);
/* Events: 1 = H-retrace rising edge, 2 = VBlank level change. The callback
 * observes ordered edge deadlines. Null keeps the original scalar-only path. */
static inline void input_route_raster_advance_observed(InputRouteRasterClock *s, uint32_t cycles, InputRouteRasterEvent event, void *context) {
    uint64_t end_cycle=s->cycle+cycles;
    uint64_t total=(uint64_t)s->fraction+(uint64_t)cycles*103896u;
    uint64_t ticks=total>>16;
    s->fraction=(uint32_t)(total&65535u);
    while (ticks) {
        uint32_t step=ticks<s->remaining ? (uint32_t)ticks : s->remaining;
        ticks-=step; s->remaining-=step;
        if (s->remaining) continue;
        s->phase^=1;
        if (s->phase) {
            if(event) event(context,end_cycle-(ticks*65536u+s->fraction)/103896u,1,s->blank);
            s->remaining=200; continue;
        }
        s->remaining=3212+s->alternate; s->alternate^=1;
        s->scanline=(s->scanline+1)%s->lines;
        if (s->scanline==s->lines-1) s->field=(s->mode&32) ? !s->field : 0;
        if (!s->scanline) {
            if (s->mode&32) s->lines=263-s->field;
            else { s->field=0; s->lines=263; }
        }
        uint32_t old_blank=s->blank;
        if (s->scanline==s->end && !s->blank) {
            s->blank=1; s->y_offset=0;
            s->readout_field=(s->mode&0x24)==0x24 ? !s->field : 0;
        }
        if (s->scanline==s->start && s->blank) s->blank=0;
        if(event && old_blank!=s->blank)
            event(context,end_cycle-(ticks*65536u+s->fraction)/103896u,2,s->blank);
        if (!old_blank && s->blank) {
            s->rises++;
            s->last_rise=end_cycle-(ticks*65536u+s->fraction)/103896u;
        }
        if ((s->mode&0x24)==0x24)
            s->readout_y=(s->y_start+2*s->y_offset+(s->blank ? 0 : s->readout_field))&511u;
        else s->readout_y=(s->y_start+s->y_offset)&511u;
        if (!s->blank) s->y_offset=(s->y_offset+1)&511u;
    }
    s->cycle=end_cycle;
}

static inline void input_route_raster_advance(InputRouteRasterClock *s, uint32_t cycles) {
    input_route_raster_advance_observed(s,cycles,0,0);
}

/* Call after the existing device-MMIO synchronization. Unknown non-raster
 * GP1 commands leave this clock unchanged. PAL is an explicit rejection.
 */
static inline int input_route_raster_gp1(InputRouteRasterClock *s, uint32_t word) {
    uint32_t command=(word>>24)&63u;
    if (!command) { s->mode=0; s->start=16; s->end=256; s->y_start=0; }
    else if (command==5) s->y_start=(word>>10)&511u;
    else if (command==7) { s->start=word&1023u; s->end=(word>>10)&1023u; }
    else if (command==8) {
        if (word&8) return 0;
        s->mode=word&255u;
    }
    return 1;
}

/* Independent field and display-line parity bits from the raster state.
 * This does not assert command/FIFO/DMA readiness or alter the raster clock.
 */
static inline uint32_t input_route_raster_status(const InputRouteRasterClock *s) {
    return ((s->readout_y&1u)<<31) | ((!s->field)<<13);
}

/* Deadline search over a copy. Cache this at the runtime boundary, invalidate
 * on GP1 reset/range/mode writes and after a rising edge. No fabricated edge
 * for an invalid/empty display range: UINT32_MAX means none within two fields.
 */
static inline uint32_t input_route_raster_until_rise(const InputRouteRasterClock *s) {
    InputRouteRasterClock t=*s;
    for (uint32_t i=0; i<1060; ++i) {
        uint64_t numerator=(uint64_t)t.remaining*65536u-t.fraction;
        uint32_t cycles=(uint32_t)((numerator+103895u)/103896u);
        input_route_raster_advance(&t,cycles);
        if (t.rises!=s->rises) return (uint32_t)(t.last_rise-s->cycle);
    }
    return UINT32_MAX;
}
#endif
