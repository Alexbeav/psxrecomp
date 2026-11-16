/* Round-trip + cross-check for the raster-clock wire codec (#6).
 *
 * Per-INSTANCE sentinels, not just per-field: every field of instance i carries
 * i in its high byte. A loader that writes record 2 into instance 3 then cannot
 * round-trip cleanly, which a per-field-only sentinel scheme would miss.
 *
 * The (cycle, fraction) pair is constrained by the cycle-derived invariant
 * (fraction == (cycle*103896) mod 65536), so those two are seeded consistently
 * rather than arbitrarily; every other field is a distinct sentinel.
 */
#include "input_route_raster_clock_wire.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static void check(int cond, const char *what) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

#define INSTANCES 3u

static void make_instance(unsigned idx, InputRouteRasterClock *s) {
    memset(s, 0, sizeof *s);
    s->cycle     = (uint64_t)(0x1000u + idx);
    s->fraction  = input_route_raster_wire_expected_fraction(s->cycle);
    /* Distinct records inside the real decoder's semantic bounds. Arbitrary
     * high-byte sentinels are invalid booleans, scanlines and mode fields. */
    s->last_rise = s->cycle - 31u - idx;
    s->remaining = 71u + idx;
    s->phase = idx & 1u;
    s->alternate = (idx + 1u) & 1u;
    s->scanline = 104u + idx;
    s->lines = 262u + (idx & 1u);
    s->field = idx & 1u;
    s->mode = 0x40u + idx;
    s->start = 108u + idx;
    s->end = 209u + idx;
    s->blank = (idx + 1u) & 1u;
    s->rises = 411u + idx;
    s->y_start = 112u + idx;
    s->y_offset = 213u + idx;
    s->readout_y = 314u + idx;
    s->readout_field = idx & 1u;
}

static int equal_instance(const InputRouteRasterClock *a, const InputRouteRasterClock *b) {
    return a->cycle == b->cycle && a->last_rise == b->last_rise &&
           a->fraction == b->fraction && a->remaining == b->remaining &&
           a->phase == b->phase && a->alternate == b->alternate &&
           a->scanline == b->scanline && a->lines == b->lines &&
           a->field == b->field && a->mode == b->mode && a->start == b->start &&
           a->end == b->end && a->blank == b->blank && a->rises == b->rises &&
           a->y_start == b->y_start && a->y_offset == b->y_offset &&
           a->readout_y == b->readout_y && a->readout_field == b->readout_field;
}

int main(void) {
    InputRouteRasterClock src[INSTANCES], out;
    uint8_t buf[INSTANCES][INPUT_ROUTE_RASTER_WIRE_BYTES];
    unsigned i, j;
    char label[96];

    check(INPUT_ROUTE_RASTER_WIRE_BYTES == 80u, "wire size is 80");

    for (i = 0; i < INSTANCES; ++i) {
        make_instance(i, &src[i]);
        check(input_route_raster_wire_expected_fraction(src[i].cycle) == src[i].fraction,
              "seeded fraction satisfies the cycle invariant");
        memset(buf[i], 0, sizeof buf[i]);
        input_route_raster_wire_write(&src[i], buf[i]);
    }

    /* 1. exact round-trip, all 18 fields, every instance */
    for (i = 0; i < INSTANCES; ++i) {
        memset(&out, 0, sizeof out);
        check(input_route_raster_wire_read(&out, buf[i], INPUT_ROUTE_RASTER_WIRE_BYTES) == 1,
              "read accepts a valid record");
        snprintf(label, sizeof label, "instance %u round-trips all 18 fields", i);
        check(equal_instance(&src[i], &out), label);
    }

    /* 2. swap detection: reading record j must NOT reproduce instance i (i!=j) */
    for (i = 0; i < INSTANCES; ++i)
        for (j = 0; j < INSTANCES; ++j) {
            if (i == j) continue;
            memset(&out, 0, sizeof out);
            check(input_route_raster_wire_read(&out, buf[j], INPUT_ROUTE_RASTER_WIRE_BYTES) == 1,
                  "read accepts a valid record (swap)");
            snprintf(label, sizeof label, "record %u does not masquerade as instance %u", j, i);
            check(!equal_instance(&src[i], &out), label);
        }

    /* 3. length rejection */
    check(input_route_raster_wire_read(&out, buf[0], 79u) == 0, "reject short record");
    check(input_route_raster_wire_read(&out, buf[0], 81u) == 0, "reject long record");

    /* 4. cross-check REFUSES: fraction inconsistent with cycle */
    {
        uint8_t bad[INPUT_ROUTE_RASTER_WIRE_BYTES];
        memcpy(bad, buf[0], sizeof bad);
        bad[16] ^= 0x01u;   /* cycle has 8 bytes (0..7); fraction starts at 16 */
        check(input_route_raster_wire_read(&out, bad, INPUT_ROUTE_RASTER_WIRE_BYTES) == 0,
              "refuse inconsistent fraction");
        bad[0] ^= 0x01u;    /* perturb cycle instead */
        check(input_route_raster_wire_read(&out, bad, INPUT_ROUTE_RASTER_WIRE_BYTES) == 0,
              "refuse inconsistent cycle");
    }

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: raster clock wire round-trip, distinct per-instance sentinels, refusal on cross-check");
    return 0;
}
