/* dispatch_publish.c — see dispatch_publish.h. */
#include "dispatch_publish.h"
#include "psx_bss.h"

extern int psx_is_dispatchable(uint32_t pc);
extern uint64_t psx_get_cycle_count(void);
extern uint64_t s_frame_count;

PSX_BSS PsxPublishEntry g_publish_ring[PSX_PUBLISH_RING_CAP];
static uint64_t s_seq = 0;

void psx_publish_note(uint32_t site, uint32_t target, uint32_t origin)
{
    PsxPublishEntry *e = &g_publish_ring[s_seq & (PSX_PUBLISH_RING_CAP - 1u)];
    e->seq          = s_seq;
    e->cycle        = psx_get_cycle_count();
    e->frame        = (uint32_t)s_frame_count;
    e->site         = site;
    e->target       = target;
    e->origin       = origin;
    e->dispatchable = psx_is_dispatchable(target) ? 1u : 0u;
    s_seq++;
}

uint64_t psx_publish_seq(void)
{
    return s_seq;
}

const PsxPublishEntry *psx_publish_get(uint64_t idx)
{
    if (idx >= s_seq || s_seq - idx > PSX_PUBLISH_RING_CAP) return 0;
    return &g_publish_ring[idx & (PSX_PUBLISH_RING_CAP - 1u)];
}

const PsxPublishEntry *psx_publish_last_for(uint32_t target)
{
    uint64_t avail = s_seq < PSX_PUBLISH_RING_CAP ? s_seq : PSX_PUBLISH_RING_CAP;
    for (uint64_t i = 1; i <= avail; i++) {
        const PsxPublishEntry *e = &g_publish_ring[(s_seq - i) & (PSX_PUBLISH_RING_CAP - 1u)];
        if (((e->target ^ target) & 0x1FFFFFFFu) == 0u) return e;
    }
    return 0;
}

const char *psx_publish_site_name(uint32_t site)
{
    switch (site) {
    case PSX_PUB_EPC_REAL:        return "epc_real";
    case PSX_PUB_EPC_SENTINEL:    return "epc_sentinel";
    case PSX_PUB_EPC_DELAY_SLOT:  return "epc_delay_slot";
    case PSX_PUB_COP0_SWI:        return "cop0_swi";
    case PSX_PUB_SCHED_RESUME_AT: return "sched_resume_at";
    case PSX_PUB_SCHED_TCB:       return "sched_tcb";
    case PSX_PUB_RFE:             return "rfe";
    case PSX_PUB_DEFERRED_SWITCH: return "deferred_switch";
    case PSX_PUB_PRECISE_EXIT:    return "precise_exit";
    case PSX_PUB_INTERP_EXIT:     return "interp_exit";
    default:                      return "unknown";
    }
}
