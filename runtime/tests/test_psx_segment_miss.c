/* Segment-miss classification and record (docs/SEGMENT_AWARE_CODE.md §5.5).
 *
 * The game dispatch table is keyed by the full PC. dirty_ram_dispatch_inner
 * hands every clean game-text miss to psx_segment_miss_note(), which asks
 * psx_segment_miss_home() whether the PC, having no row of its own, has a
 * compiled row in another segment at the same physical word; if so the PC is
 * a segment miss: interpreted, and recorded here with the full PC and the row
 * that exists. test_segment_miss_wiring.py pins the call site.
 *
 * The fake table below is a KUSEG-linked image (rows at 0x000100E4 and
 * 0x000100F0) plus one KSEG1 row, the shape PR D's variants add. */

#include "psx_segment_miss.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static int is_entry(uint32_t addr)
{
    return addr == 0x000100E4u || addr == 0x000100F0u || addr == 0xA0010200u;
}

int main(void)
{
    /* Classification: another segment's row at the same word. */
    CHECK(psx_segment_miss_home(0x800100E4u, is_entry) == 0x000100E4u,
          "a KSEG0 alias of a KUSEG row is a segment miss");
    CHECK(psx_segment_miss_home(0xA00100F0u, is_entry) == 0x000100F0u,
          "a KSEG1 alias of a KUSEG row is a segment miss");
    CHECK(psx_segment_miss_home(0x00010200u, is_entry) == 0xA0010200u,
          "a KUSEG alias of a KSEG1 row names the KSEG1 row");
    CHECK(psx_segment_miss_home(0x000100E4u, is_entry) == 0,
          "a PC's own row is not consulted");
    CHECK(psx_segment_miss_home(0x800100E8u, is_entry) == 0,
          "an interior PC with no row in any segment is not a segment miss");
    CHECK(psx_segment_miss_home(0x800100E4u, NULL) == 0,
          "no dispatch table, no segment miss");

    /* The dispatch hook (dirty_ram_dispatch_inner, on a clean game-text
     * miss): records only a PC with no row of its own whose alias has one. */
    psx_segment_miss_reset();
    CHECK(psx_segment_miss_note(0x000100E4u, is_entry, 1u, 2u, 3u) == 0,
          "a PC with its own row (bytes diverged) is not a segment miss");
    CHECK(psx_segment_miss_note(0x800100E8u, is_entry, 1u, 2u, 3u) == 0,
          "an interior PC with no row in any segment is not a segment miss");
    CHECK(psx_segment_miss_note(0x800100E4u, NULL, 1u, 2u, 3u) == 0,
          "no dispatch table, no segment miss");
    CHECK(psx_segment_miss_total() == 0, "non-segment misses record nothing");
    CHECK(psx_segment_miss_note(0xA00100E4u, is_entry, 0x00010018u, 0x001FFF00u, 7u) ==
              0x000100E4u, "an alias of a row is noted with that row");
    {
        PsxSegmentMissEntry e = psx_segment_miss_get(0);
        CHECK(psx_segment_miss_total() == 1 && e.addr == 0xA00100E4u &&
              e.home == 0x000100E4u && e.ra == 0x00010018u && e.sp == 0x001FFF00u &&
              e.frame == 7u, "the noted miss is recorded with the full PC, $ra, $sp, frame");
    }

    /* Record: totals, unique PCs, ring order, summary order. */
    psx_segment_miss_reset();
    CHECK(psx_segment_miss_total() == 0 && psx_segment_miss_unique() == 0, "reset");
    for (int i = 0; i < 3; ++i)
        psx_segment_miss_record(0xA00100F0u, 0x000100F0u, 0x00010040u, 0x001FFF00u, 10u + i);
    psx_segment_miss_record(0x800100E4u, 0x000100E4u, 0x00010018u, 0x001FFF00u, 20u);
    CHECK(psx_segment_miss_total() == 4, "every miss counts");
    CHECK(psx_segment_miss_unique() == 2, "distinct full PCs are unique entries");
    {
        PsxSegmentMissEntry e = psx_segment_miss_get(3);
        CHECK(e.seq == 3 && e.addr == 0x800100E4u && e.home == 0x000100E4u &&
              e.ra == 0x00010018u && e.frame == 20u && e.kind == PSX_SEGMENT_MISS_GAME,
              "the ring keeps the full PC in order, kind game");
    }
    {
        uint32_t addrs[4], homes[4], kinds[4];
        uint64_t counts[4];
        uint32_t n = psx_segment_miss_summary(addrs, homes, counts, kinds, 4);
        CHECK(n == 2, "summary has one row per PC");
        CHECK(addrs[0] == 0xA00100F0u && homes[0] == 0x000100F0u && counts[0] == 3 &&
              kinds[0] == PSX_SEGMENT_MISS_GAME,
              "summary is highest count first");
        CHECK(addrs[1] == 0x800100E4u && counts[1] == 1, "summary second row");
        CHECK(psx_segment_miss_summary(addrs, homes, counts, NULL, 1) == 1,
              "summary honours max");
    }
    /* A BIOS segment miss (the compiled BIOS ran a window's home body for an
     * alias PC, §5.4) keeps its kind in the ring and in the summary. */
    psx_segment_miss_record_kind(0x80000500u, 0x00000500u, 0xBFC06F0Cu, 0x801FFF00u, 3u,
                                 PSX_SEGMENT_MISS_BIOS);
    {
        PsxSegmentMissEntry e = psx_segment_miss_get(4);
        uint32_t addrs[4], kinds[4];
        uint32_t n = psx_segment_miss_summary(addrs, NULL, NULL, kinds, 4);
        CHECK(e.addr == 0x80000500u && e.home == 0x00000500u && e.frame == 3u &&
              e.kind == PSX_SEGMENT_MISS_BIOS, "a BIOS segment miss is recorded as kind bios");
        /* Counts 3, 1, 1; the tie orders by PC. */
        CHECK(n == 3 && addrs[1] == 0x80000500u && kinds[1] == PSX_SEGMENT_MISS_BIOS &&
              kinds[0] == PSX_SEGMENT_MISS_GAME && kinds[2] == PSX_SEGMENT_MISS_GAME,
              "the summary carries each PC's kind");
        CHECK(strcmp(psx_segment_miss_kind_name(PSX_SEGMENT_MISS_BIOS), "bios") == 0 &&
              strcmp(psx_segment_miss_kind_name(PSX_SEGMENT_MISS_GAME), "game") == 0,
              "kind names");
    }
    /* Equal counts order by PC, and the summary visits each PC once. */
    psx_segment_miss_reset();
    psx_segment_miss_record(0xA0010300u, 0x00010300u, 0, 0, 0);
    psx_segment_miss_record(0x80010300u, 0x00010300u, 0, 0, 0);
    {
        uint32_t addrs[4];
        uint32_t n = psx_segment_miss_summary(addrs, NULL, NULL, NULL, 4);
        CHECK(n == 2 && addrs[0] == 0x80010300u && addrs[1] == 0xA0010300u,
              "ties order by PC");
    }
    /* The ring wraps; the total keeps counting. */
    psx_segment_miss_reset();
    for (uint32_t i = 0; i < PSX_SEGMENT_MISS_RING_CAP + 5u; ++i)
        psx_segment_miss_record(0x80010000u + 4u * (i % 7u), 0x00010000u, 0, 0, i);
    CHECK(psx_segment_miss_total() == PSX_SEGMENT_MISS_RING_CAP + 5u, "total past the ring");
    CHECK(psx_segment_miss_unique() == 7, "unique past the ring");
    {
        PsxSegmentMissEntry e = psx_segment_miss_get(PSX_SEGMENT_MISS_RING_CAP + 4u);
        CHECK(e.frame == PSX_SEGMENT_MISS_RING_CAP + 4u, "the newest entry survives the wrap");
    }

    if (failures) {
        fprintf(stderr, "psx_segment_miss_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("psx_segment_miss_test: PASS");
    return 0;
}
