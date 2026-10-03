/* interp_report.h — the run report's "interp_detail" object (PS1B-391).
 *
 * A release product has no debug server. Its run report gave one number for
 * interpreted dispatches above the kernel window and could not say which
 * addresses ran interpreted, whether they lie in the boot text or in overlay
 * RAM, whether the text guard refused them, or why the overlay loader had no
 * native unit. This object says it, from counters the runtime keeps anyway.
 *
 * The builder only reads. It takes no lock and allocates nothing, because the
 * report is also written on the crash path. */
#ifndef PSXRECOMP_INTERP_REPORT_H
#define PSXRECOMP_INTERP_REPORT_H

#include <stdint.h>
#include "dirty_ram_interp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why overlay_loader_dispatch found no native unit for a target. One counter
 * per reason and per place (above the kernel window, inside it). */
enum {
    PSX_MISS_CACHED = 0,      /* a miss the loader already knew: nothing valid here since the last change */
    PSX_MISS_NO_UNIT,         /* no compiled unit covers the address */
    PSX_MISS_STALE_BYTES,     /* a unit covers it, but none matches the live code bytes */
    PSX_MISS_OUTSIDE_WINDOW,  /* clean boot text or another address the loader does not serve */
    PSX_MISS_DEVICE_TOUCH,    /* the unit matches but touches a device: it never runs natively */
    PSX_MISS_NATIVE_OFF,      /* native execution is off, or this function is blocked */
    PSX_MISS_DIFF_GATE,       /* held back by the native/interpreter comparison (development) */
    PSX_MISS_RANK,            /* held back by the native rank filter (debug tools only) */
    PSX_MISS_BAD_ENTRY,       /* the unit ran and refused a foreign interior entry */
    PSX_MISS_MODIFIED_TEXT_BACKOFF, /* a rewritten text page left to the interpreter after its limit of take-outs (PS1B-421) */
    PSX_MISS_MODIFIED_TEXT_LOAD_BOUND, /* a rewritten text page that has had its limit of lazy loads (PS1B-421) */
    PSX_INTERP_MISS_REASONS
};

#define PSX_INTERP_REPORT_HOTTEST 32

typedef struct {
    /* Text image guard (memory.c). */
    int      guard_armed;
    uint32_t guard_lo, guard_hi;        /* physical range of the reference image */
    uint32_t foreign_pages;
    uint64_t native_blocked;
    uint32_t diverged_pages;
    uint64_t exact_mismatches;
    uint32_t exact_last[5];             /* range lo, range len, first differing address, live byte, reference byte */
    /* Where an address lies: [0, kernel_end) kernel window, [text_lo, text_hi) boot text, else overlay RAM. */
    uint32_t kernel_end, text_lo, text_hi;
    /* Interpreter totals (dirty_ram_interp.c). */
    uint64_t blocks_run, insns_run, aborts, guard_yields, native_handoffs;
    /* Per-address table of interpreted block entries; pc 0 marks an empty slot. */
    const DirtyRamPcEntry *table;
    uint32_t table_size;
    /* Loader misses by reason: [0] above the kernel window, [1] inside it. */
    uint64_t miss[2][PSX_INTERP_MISS_REASONS];
} PsxInterpReportInput;

const char *psx_interp_miss_reason_name(int reason);

/* Write the object to `out` (a JSON object, no trailing newline). Returns its
 * length, or -1 when `cap` is too small; `out` then holds no partial object. */
int psx_interp_report_json(char *out, int cap, const PsxInterpReportInput *in);

#ifdef __cplusplus
}
#endif

#endif
