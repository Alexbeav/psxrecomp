#ifndef PSXRECOMP_PSX_ICACHE_H
#define PSXRECOMP_PSX_ICACHE_H

#include "cpu_state.h"
#include "input_instruction_histogram.h"
#include "psx_cycles.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t g_psx_icache_tv[1024];
extern int g_psx_icache_active;
extern int g_ls_replay_active;
/* Optional functional instruction-boundary consumer, independent of tracing.
 * Called before a guest fetch (also for coalesced cached followers) after
 * publishing completed prior instructions. Replay shadows never call it. */
extern void (*g_psx_cpu_step_boundary_callback)(CPUState *,uint32_t,uint64_t);
#ifdef PSX_OVERLAY_DLL_BUILD
/* The DLL owns its pending cycle batch; its shim flushes that batch before
 * forwarding to the host's observer. Host globals must not be copied. */
void psx_cpu_step_boundary(CPUState *cpu,uint32_t address);
#else
static inline void psx_cpu_step_boundary(CPUState *cpu,uint32_t address) {
    if(g_psx_cpu_step_boundary_callback && !g_ls_replay_active) {
        psx_cyc_batch_flush();
        g_psx_cpu_step_boundary_callback(cpu,address,psx_get_cycle_count());
    }
}
#endif
void psx_cpu_step_boundary_fn(CPUState *cpu,uint32_t address);
int psx_cpu_step_boundary_enabled(int include_replay);
void psx_icache_reset(void);
void psx_icache_fetch(CPUState *cpu, uint32_t addr);
void psx_icache_fetch_miss(CPUState *cpu, uint32_t addr);
/* Called only for guest stores with CP0.SR.IsC set. Updates timing tags for
 * the BIOS tag-test flush; cache data and guest BIU-disabled fetches are not
 * modeled by this helper. Address is the original virtual store address. */
void psx_icache_isolated_store(uint32_t addr, uint32_t cache_control);

/* Keep the interpreter's steady-state tag hit inside its translation unit.
 * Misses use the shared slow path, preserving exact cache evolution/timing. */
static inline void psx_icache_fetch_interp_after_boundary(CPUState *cpu, uint32_t addr) {
    if (g_input_instruction_histogram_active) input_instruction_histogram_sample(addr);
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (g_ls_replay_active) return;
    if (g_psx_icache_active < 0) {
        psx_icache_fetch_miss(cpu, addr);
        return;
    }
    if (!g_psx_icache_active) return;
    uint32_t idx = (addr & 0xFFCu) >> 2;
    if (g_psx_icache_tv[idx] == addr) return;
    psx_icache_fetch_miss(cpu, addr);
#else
    (void)cpu;
    (void)addr;
#endif
}

static inline void psx_icache_fetch_interp(CPUState *cpu, uint32_t addr) {
    psx_cpu_step_boundary(cpu,addr);
    psx_icache_fetch_interp_after_boundary(cpu,addr);
}

#ifdef __cplusplus
}
#endif

#endif
