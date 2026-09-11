#ifndef PSX_INPUT_ROUTE_RASTER_CLOCK_WIRE_H
#define PSX_INPUT_ROUTE_RASTER_CLOCK_WIRE_H
#include <stdint.h>
#include <stdio.h>
#include "pst_wire.h"
#include "input_route_raster_clock.h"

/*
 * #6: little-endian, FIELD-BY-FIELD wire form for InputRouteRasterClock.
 *
 * 18 scalars = 80 bytes. Deliberately not a raw struct copy: the struct has no
 * padding today, but a raw fwrite would leak uninitialised padding bytes the
 * moment one is introduced, making the state bytes vary run to run and breaking
 * anything that hashes or byte-compares files. Field-by-field also keeps the
 * wire portable across endianness, matching the v6 section framework.
 */
#define INPUT_ROUTE_RASTER_WIRE_BYTES 80u

static inline void input_route_raster_wire_write(const InputRouteRasterClock *s,
                                                 uint8_t *p) {
    PstW w;
    pst_w_init(&w, p, INPUT_ROUTE_RASTER_WIRE_BYTES);
    pst_w_u64(&w, s->cycle);        pst_w_u64(&w, s->last_rise);
    pst_w_u32(&w, s->fraction);     pst_w_u32(&w, s->remaining);
    pst_w_u32(&w, s->phase);        pst_w_u32(&w, s->alternate);
    pst_w_u32(&w, s->scanline);     pst_w_u32(&w, s->lines);
    pst_w_u32(&w, s->field);        pst_w_u32(&w, s->mode);
    pst_w_u32(&w, s->start);        pst_w_u32(&w, s->end);
    pst_w_u32(&w, s->blank);        pst_w_u32(&w, s->rises);
    pst_w_u32(&w, s->y_start);      pst_w_u32(&w, s->y_offset);
    pst_w_u32(&w, s->readout_y);    pst_w_u32(&w, s->readout_field);
}

/* Pure cycle-derived invariant: the fractional raster accumulator depends only
 * on the TOTAL cycles advanced since reset, not on how those cycles were sliced
 * — fraction after N cycles is (N*103896) mod 65536. `cycle` is that total,
 * because every advance adds the same delta to both `cycle` and the accumulator.
 *
 * This is the ONLY field cross-checked on load. scanline / field / readout_*
 * depend on the GP1 mode-change history (input_route_raster_gp1 mutates
 * mode/start/end mid-run), so asserting those against a cycle-only recompute
 * would fire on the first mode write and the check would get loosened until it
 * meant nothing. The round-trip test and the fingerprint gate cover the rest. */
static inline uint32_t input_route_raster_wire_expected_fraction(uint64_t cycle) {
    return (uint32_t)((cycle * UINT64_C(103896)) & UINT64_C(0xFFFF));
}

/* Parse + validate. A failed cross-check REFUSES (returns 0), never warns. */
static inline int input_route_raster_wire_read(InputRouteRasterClock *s,
                                               const uint8_t *p, uint32_t len) {
    PstR r;
    if (!s || !p || len != INPUT_ROUTE_RASTER_WIRE_BYTES) return 0;
    pst_r_init(&r, p, len);
    if (!pst_r_u64(&r, &s->cycle)       || !pst_r_u64(&r, &s->last_rise) ||
        !pst_r_u32(&r, &s->fraction)    || !pst_r_u32(&r, &s->remaining) ||
        !pst_r_u32(&r, &s->phase)       || !pst_r_u32(&r, &s->alternate) ||
        !pst_r_u32(&r, &s->scanline)    || !pst_r_u32(&r, &s->lines) ||
        !pst_r_u32(&r, &s->field)       || !pst_r_u32(&r, &s->mode) ||
        !pst_r_u32(&r, &s->start)       || !pst_r_u32(&r, &s->end) ||
        !pst_r_u32(&r, &s->blank)       || !pst_r_u32(&r, &s->rises) ||
        !pst_r_u32(&r, &s->y_start)     || !pst_r_u32(&r, &s->y_offset) ||
        !pst_r_u32(&r, &s->readout_y)   || !pst_r_u32(&r, &s->readout_field))
        return 0;
    if (s->fraction != input_route_raster_wire_expected_fraction(s->cycle)) {
        fprintf(stderr,
                "[raster-wire] refusing raster state: fraction %u inconsistent "
                "with cycle %llu (want %u)\n",
                (unsigned)s->fraction, (unsigned long long)s->cycle,
                (unsigned)input_route_raster_wire_expected_fraction(s->cycle));
        return 0;
    }
    return 1;
}
#endif
