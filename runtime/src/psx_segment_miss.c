/* psx_segment_miss.c — segment misses in static game code
 * (docs/SEGMENT_AWARE_CODE.md §5.5). See psx_segment_miss.h.
 *
 * Always on, in every build: a segment miss runs interpreted, so it is the
 * slow path already, and the record is what makes it loud (§10 decision 2).
 * The ring keeps the last PSX_SEGMENT_MISS_RING_CAP misses in order; the
 * unique table counts each distinct PC. */

#include "psx_segment_miss.h"

#include <string.h>

#include "psx_bss.h"

static PSX_BSS PsxSegmentMissEntry s_ring[PSX_SEGMENT_MISS_RING_CAP];
static uint64_t s_seq = 0;

typedef struct {
    uint32_t addr;   /* 0 = empty slot (a segment miss is never at PC 0) */
    uint32_t home;
    uint64_t count;
} UniqueSlot;
static PSX_BSS UniqueSlot s_unique[PSX_SEGMENT_MISS_UNIQUE_CAP];
static uint32_t s_unique_count = 0;

uint32_t psx_segment_miss_home(uint32_t addr, int (*is_entry)(uint32_t))
{
    static const uint32_t kSegments[3] = { 0x00000000u, 0x80000000u, 0xA0000000u };
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!is_entry) return 0;
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t alias = kSegments[i] | phys;
        if (alias != addr && is_entry(alias)) return alias;
    }
    return 0;
}

void psx_segment_miss_record(uint32_t addr, uint32_t home, uint32_t ra,
                             uint32_t sp, uint32_t frame)
{
    const uint64_t seq = s_seq++;
    PsxSegmentMissEntry *e = &s_ring[seq & (PSX_SEGMENT_MISS_RING_CAP - 1u)];
    e->seq = seq;
    e->addr = addr;
    e->home = home;
    e->ra = ra;
    e->sp = sp;
    e->frame = frame;
    e->pad = 0;
    if (addr == 0) return;
    {
        const uint32_t start = (addr >> 2) % PSX_SEGMENT_MISS_UNIQUE_CAP;
        for (uint32_t i = 0; i < PSX_SEGMENT_MISS_UNIQUE_CAP; ++i) {
            UniqueSlot *slot = &s_unique[(start + i) % PSX_SEGMENT_MISS_UNIQUE_CAP];
            if (slot->addr == addr) {
                slot->count++;
                return;
            }
            if (slot->addr == 0) {
                slot->addr = addr;
                slot->home = home;
                slot->count = 1;
                s_unique_count++;
                return;
            }
        }
    }
}

uint32_t psx_segment_miss_note(uint32_t addr, int (*is_entry)(uint32_t),
                               uint32_t ra, uint32_t sp, uint32_t frame)
{
    uint32_t home;
    if (!is_entry || is_entry(addr)) return 0;
    home = psx_segment_miss_home(addr, is_entry);
    if (home) psx_segment_miss_record(addr, home, ra, sp, frame);
    return home;
}

uint64_t psx_segment_miss_total(void) { return s_seq; }
uint32_t psx_segment_miss_unique(void) { return s_unique_count; }

PsxSegmentMissEntry psx_segment_miss_get(uint64_t seq)
{
    return s_ring[seq & (PSX_SEGMENT_MISS_RING_CAP - 1u)];
}

uint32_t psx_segment_miss_summary(uint32_t *addrs, uint32_t *homes,
                                  uint64_t *counts, uint32_t max)
{
    /* Selection by count, highest first, then by PC; the table is small. */
    uint32_t written = 0;
    uint64_t prev_count = UINT64_MAX;
    uint32_t prev_addr = 0;
    while (written < max) {
        int best = -1;
        for (uint32_t i = 0; i < PSX_SEGMENT_MISS_UNIQUE_CAP; ++i) {
            const UniqueSlot *s = &s_unique[i];
            if (s->addr == 0) continue;
            /* Strictly after the previous row in (count desc, addr asc). */
            if (s->count > prev_count ||
                (s->count == prev_count && s->addr <= prev_addr))
                continue;
            if (best < 0 || s->count > s_unique[best].count ||
                (s->count == s_unique[best].count && s->addr < s_unique[best].addr))
                best = (int)i;
        }
        if (best < 0) break;
        if (addrs) addrs[written] = s_unique[best].addr;
        if (homes) homes[written] = s_unique[best].home;
        if (counts) counts[written] = s_unique[best].count;
        prev_count = s_unique[best].count;
        prev_addr = s_unique[best].addr;
        written++;
    }
    return written;
}

void psx_segment_miss_reset(void)
{
    memset(s_ring, 0, sizeof s_ring);
    memset(s_unique, 0, sizeof s_unique);
    s_seq = 0;
    s_unique_count = 0;
}
