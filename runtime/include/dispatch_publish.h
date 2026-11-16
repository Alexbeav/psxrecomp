/* dispatch_publish.h — always-on ring of runtime sites that publish a resume PC.
 *
 * A generated body publishes only branch targets, fall-throughs and call
 * returns, all registered dispatch keys. Every OTHER PC the dispatcher later
 * runs is published by the runtime: an exception EPC, a TCB resume, an
 * interpreter or precise-slice hand-back, a savestate resume. When one of those
 * is not re-enterable the dispatch miss surfaces far from its source (T110:
 * mid-block and delay-slot BIOS ROM PCs). Each publish is recorded here with
 * whether psx_is_dispatchable() accepted it at that moment; the unknown-dispatch
 * crash report names the matching publisher, and the `publish_ring` debug
 * command dumps the ring (CLAUDE.md §3: no logs, a ring + TCP command). */
#ifndef DISPATCH_PUBLISH_H
#define DISPATCH_PUBLISH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PSX_PUB_EPC_REAL = 1,     /* exception entry: real resume PC accepted as EPC */
    PSX_PUB_EPC_SENTINEL,     /* exception entry: resume PC refused, sentinel EPC */
    PSX_PUB_EPC_DELAY_SLOT,   /* delay-slot IRQ: EPC = branch, Cause.BD */
    PSX_PUB_COP0_SWI,         /* MTC0/CTC0 software interrupt resume PC */
    PSX_PUB_SCHED_RESUME_AT,  /* savestate / rewind / netplay / selfcheck resume */
    PSX_PUB_SCHED_TCB,        /* scheduler dispatches the current TCB's EPC */
    PSX_PUB_RFE,              /* ReturnFromException resume PC */
    PSX_PUB_DEFERRED_SWITCH,  /* deferred in-exception thread switch */
    PSX_PUB_PRECISE_EXIT,     /* precise slice hands back to the dispatcher */
    PSX_PUB_INTERP_EXIT,      /* dirty-RAM interpreter straight-line hand-back */
    PSX_PUB_SITE_COUNT
} psx_publish_site_t;

typedef struct {
    uint64_t seq;
    uint64_t cycle;
    uint32_t frame;
    uint32_t site;          /* psx_publish_site_t */
    uint32_t target;        /* the published PC */
    uint32_t origin;        /* site-specific: interrupted/committed PC, TCB, ... */
    uint32_t dispatchable;  /* psx_is_dispatchable(target) when published */
} PsxPublishEntry;

#define PSX_PUBLISH_RING_CAP 1024u

void psx_publish_note(uint32_t site, uint32_t target, uint32_t origin);
uint64_t psx_publish_seq(void);
/* idx is an absolute sequence number; returns 0 once it has been overwritten. */
const PsxPublishEntry *psx_publish_get(uint64_t idx);
const PsxPublishEntry *psx_publish_last_for(uint32_t target);
const char *psx_publish_site_name(uint32_t site);

#ifdef __cplusplus
}
#endif

#endif /* DISPATCH_PUBLISH_H */
