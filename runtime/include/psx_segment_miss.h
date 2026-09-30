#ifndef PSX_SEGMENT_MISS_H
#define PSX_SEGMENT_MISS_H

/* Segment misses in static game code (docs/SEGMENT_AWARE_CODE.md §5.5; §10
 * decision 2: interpret loudly until the title is regenerated).
 *
 * A PC carries a segment (KUSEG 0x0..., KSEG0 0x8..., KSEG1 0xA...), and the
 * game dispatch table is keyed by the full PC: each compiled body bakes the
 * links, EPCs, fetch tags and store PCs of the segment it was compiled for.
 * A PC whose physical word has a row only in another segment (a KSEG1 alias
 * of KUSEG-linked text, a KSEG0 call into it) therefore has no body of its
 * own. Dispatch interprets it through the clean-text-miss path and records it
 * here, with the full PC and the row that exists, so a run names each
 * (segment, entry) that needs a segment-qualified seed (§5.4). Visible over
 * TCP (`segment_misses`, `dispatch_stats`) and in psx_last_run_report.json. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t seq;
    uint32_t addr;   /* the PC that has no body of its own */
    uint32_t home;   /* the compiled row at the same physical word */
    uint32_t ra;
    uint32_t sp;
    uint32_t frame;
    uint32_t pad;
} PsxSegmentMissEntry;

#define PSX_SEGMENT_MISS_RING_CAP   4096u  /* power of two */
#define PSX_SEGMENT_MISS_UNIQUE_CAP 1024u

/* The other segment's PC with a compiled row at addr's physical word, or 0
 * when there is none (addr itself is not consulted). KUSEG, KSEG0 and KSEG1
 * are tried in that order. is_entry is the game dispatch's
 * psx_game_is_function_entry. */
uint32_t psx_segment_miss_home(uint32_t addr, int (*is_entry)(uint32_t));

/* Record one segment miss. */
void psx_segment_miss_record(uint32_t addr, uint32_t home, uint32_t ra,
                             uint32_t sp, uint32_t frame);

/* The dispatch hook: dirty_ram_dispatch_inner calls this for every clean
 * game-text miss (a PC in the EXE's text that no compiled body ran). It
 * records a segment miss when addr has no row of its own but another
 * segment's alias does, and returns that row; otherwise (addr has a row, e.g.
 * its bytes diverged, or no segment has one: an interior PC) it records
 * nothing and returns 0. */
uint32_t psx_segment_miss_note(uint32_t addr, int (*is_entry)(uint32_t),
                               uint32_t ra, uint32_t sp, uint32_t frame);

uint64_t psx_segment_miss_total(void);
uint32_t psx_segment_miss_unique(void);

/* The i-th recorded entry, i < total; only the last RING_CAP are kept. */
PsxSegmentMissEntry psx_segment_miss_get(uint64_t seq);

/* Per-PC counts, highest first: up to max rows, returns the number written.
 * Counts are exact for the first UNIQUE_CAP distinct PCs; later new PCs are
 * counted in the total and the ring only. */
uint32_t psx_segment_miss_summary(uint32_t *addrs, uint32_t *homes,
                                  uint64_t *counts, uint32_t max);

void psx_segment_miss_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SEGMENT_MISS_H */
