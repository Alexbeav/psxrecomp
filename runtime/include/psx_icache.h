#ifndef PSX_ICACHE_H
#define PSX_ICACHE_H

#include <stdint.h>
#include "cpu_state.h"
#include "input_instruction_histogram.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t g_psx_icache_tv[1024];
extern uint32_t g_psx_icache_words[1024];
extern uint32_t g_psx_cache_ctrl;
void psx_icache_bind_memory(const uint8_t *ram, uint32_t ram_size, const uint8_t *rom);
uint32_t psx_icache_read_cached(uint32_t address, uint32_t memory_word);
int psx_icache_block_stale(uint32_t address, uint32_t words);
extern int g_psx_icache_active;
extern int g_ls_replay_active;
extern void (*g_psx_cpu_step_boundary_callback)(CPUState *, uint32_t, uint64_t);
void psx_icache_reset(void);
void psx_icache_restore_legacy_words(void);
/* Refresh only resident words touched by a host-authored RAM code patch.
 * Retain other cached words and tags; guest stores must not call this. */
void psx_icache_refresh_host_write(uint32_t address, uint32_t bytes);
int psx_icache_enabled(void);
void psx_icache_fetch(CPUState *, uint32_t);
void psx_icache_fetch_miss(CPUState *, uint32_t);
void psx_icache_fetch_fn(CPUState *, uint32_t);
void psx_icache_isolated_store(uint32_t, uint32_t);
int psx_icache_shadow_record_begin(void);
int psx_icache_shadow_replay_begin(void);
void psx_icache_shadow_replay_end(void);
void psx_icache_shadow_abort(void);
void psx_cpu_step_boundary_fn(CPUState *, uint32_t);
int psx_cpu_step_boundary_enabled(int);

#ifdef PSX_OVERLAY_DLL_BUILD
void psx_cpu_step_boundary(CPUState *, uint32_t);
#else
static inline void psx_cpu_step_boundary(CPUState *cpu, uint32_t address)
{
    psx_cpu_step_boundary_fn(cpu, address);
}
#endif

static inline void psx_icache_fetch_interp_after_boundary(CPUState *cpu, uint32_t address)
{
    if (g_input_instruction_histogram_active)
        input_instruction_histogram_sample(address);
    if (!g_ls_replay_active) psx_icache_fetch_miss(cpu, address);
}

static inline void psx_icache_fetch_interp(CPUState *cpu, uint32_t address)
{
    psx_cpu_step_boundary(cpu, address);
    psx_icache_fetch_interp_after_boundary(cpu, address);
}

#ifdef __cplusplus
}
#endif
#endif
