#ifndef PSX_CYC_H
#define PSX_CYC_H

#include <stdint.h>
#include <string.h>
#include "cpu_state.h"
#include "psx_cycles.h"
#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Independently implemented from declaration contracts and authored observations.
 * Evidence: T172 CPU timing matrices and analyze_cpu_timing_observations.py. */
extern uint8_t *g_psx_ram;
extern int g_psx_load_delay, g_ls_mode, g_ram_read_watch_active;
extern volatile int g_ds_recording;
extern uint32_t g_dma_cpu_read_wait;
void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t val);
int psx_load_delay_enabled(void);
uint32_t psx_read_word(uint32_t addr);
uint16_t psx_read_half(uint32_t addr);
uint32_t psx_cyc_load_word_slow(CPUState *, uint32_t, uint32_t, uint32_t);
uint16_t psx_cyc_load_half_slow(CPUState *, uint32_t, uint32_t, uint32_t);
uint8_t psx_cyc_load_byte(CPUState *, uint32_t, uint32_t, uint32_t);
void psx_cyc_load_word_timing_only(CPUState *, uint32_t, uint32_t, uint32_t);
uint32_t psx_cyc_lwc2_read(CPUState *, uint32_t);

enum { PSX_CYC_BATCH_SOFT = 64u };

static inline void psx_cyc_charge(uint32_t cycles)
{
    if (!cycles) return;
#if defined(PSX_OVERLAY_DLL_BUILD)
    psx_advance_cycles(cycles);
#else
    if (psx_in_device_service && !g_event_step_conservative && !g_ls_replay_active) {
        psx_cycle_count += cycles;
        return;
    }
#ifdef PSX_COSIM
    psx_advance_cycles(cycles);
#else
    if (g_event_step_conservative || g_ls_replay_active) {
        psx_advance_cycles(cycles);
    } else if (g_psx_cyc_local_acc) {
        if ((uint64_t)*g_psx_cyc_local_acc + cycles >= (UINT64_C(1) << 28))
            psx_cyc_local_publish();
        *g_psx_cyc_local_acc += cycles;
    } else if (g_psx_cyc_batch > UINT32_MAX - cycles) {
        psx_cyc_batch_flush();
        psx_advance_cycles(cycles);
    } else if (g_psx_cyc_bb_defer) {
        g_psx_cyc_batch += cycles;
    } else {
        const int already_batched = g_psx_cyc_batch != 0;
        if (!g_psx_cyc_batch) {
            uint64_t distance = psx_next_service_cycle > psx_cycle_count
                ? psx_next_service_cycle - psx_cycle_count : 1;
            g_psx_cyc_batch_limit = distance < PSX_CYC_BATCH_SOFT
                ? (uint32_t)distance : PSX_CYC_BATCH_SOFT;
        }
        g_psx_cyc_batch += cycles;
        if (g_psx_cyc_batch >= g_psx_cyc_batch_limit) {
            if (already_batched) {
                psx_cyc_batch_flush();
            } else {
                uint32_t elapsed = g_psx_cyc_batch;
                g_psx_cyc_batch = 0;
                psx_advance_cycles(elapsed);
            }
        }
    }
#endif
#endif
}

static inline void psx_cyc_bb_defer_begin(void)
{
#ifndef PSX_OVERLAY_DLL_BUILD
    ++g_psx_cyc_bb_defer;
#endif
}

static inline void psx_cyc_bb_defer_flush(void)
{
#ifdef PSX_OVERLAY_DLL_BUILD
    overlay_flush_cycles();
#elif !defined(PSX_COSIM)
    psx_cyc_batch_flush();
#endif
}

static inline void psx_cyc_bb_defer_end(void)
{
#ifndef PSX_OVERLAY_DLL_BUILD
    if (g_psx_cyc_bb_defer > 0) --g_psx_cyc_bb_defer;
    if (!g_psx_cyc_bb_defer) psx_cyc_bb_defer_flush();
#endif
}

static inline void psx_cyc_local_begin(uint32_t *accumulator)
{
#if !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_COSIM)
    g_psx_cyc_local_acc = accumulator;
    if (accumulator) *accumulator = 0;
#else
    (void)accumulator;
#endif
}

static inline void psx_cyc_local_end(void)
{
#if !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_COSIM)
    psx_cyc_local_publish();
    g_psx_cyc_local_acc = 0;
#endif
}

#if defined(__GNUC__) || defined(__clang__)
static inline void psx_cyc_bb_defer_cleanup(int *guard)
{
    (void)guard;
    psx_cyc_bb_defer_end();
}
static inline void psx_cyc_local_cleanup(uint32_t **guard)
{
    (void)guard;
    psx_cyc_local_end();
}
#endif

static inline void psx_cyc_base(CPUState *cpu)
{
    uint8_t *remaining = &cpu->read_absorb[cpu->read_absorb_which];
    if (*remaining) --*remaining;
    else psx_cyc_charge(1);
}

static inline void psx_cyc_deps(CPUState *cpu, uint32_t reg_mask)
{
    for (unsigned reg = 1; reg < 32; ++reg)
        if (reg_mask & (UINT32_C(1) << reg)) cpu->read_absorb[reg] = 0;
}

static inline void psx_cyc_lds(CPUState *cpu)
{
    const unsigned slot = cpu->ld_which_t;
    cpu->read_absorb[slot] = (uint8_t)cpu->ld_absorb;
    cpu->read_absorb_which |= (uint8_t)(slot & 31);
    cpu->read_fudge = (uint8_t)slot;
    cpu->ld_which_t = 32;
}

static inline void psx_cyc_step(CPUState *cpu, uint32_t reg_mask)
{
    psx_cyc_base(cpu);
    psx_cyc_deps(cpu, reg_mask);
    psx_cyc_lds(cpu);
}

static inline void psx_cyc_ram_load_timing(CPUState *cpu, uint32_t rt, uint32_t mask)
{
    if (!(g_psx_load_delay < 0 ? psx_load_delay_enabled() : g_psx_load_delay)) return;
    if (cpu->ld_which_t == rt) cpu->ld_which_t = 0;
    psx_cyc_step(cpu, mask);
    psx_cyc_charge(cpu->read_fudge == 32 ? 7 : 5);
    cpu->read_absorb[cpu->read_absorb_which] = 0;
    cpu->read_absorb_which = 0;
    cpu->ld_which_t = (uint8_t)rt;
    cpu->ld_absorb = 5;
}

#ifdef PSX_OVERLAY_DLL_BUILD
uint32_t psx_cyc_load_word(CPUState *, uint32_t, uint32_t, uint32_t);
uint16_t psx_cyc_load_half(CPUState *, uint32_t, uint32_t, uint32_t);
#else
static inline uint32_t psx_cyc_load_word(CPUState *cpu, uint32_t addr,
                                         uint32_t rt, uint32_t reg_mask)
{
#ifndef PSX_ENABLE_BLOCK_CYCLES
    (void)cpu; (void)rt; (void)reg_mask;
    return psx_read_word(addr);
#else
#ifndef PSX_OVERLAY_DLL_BUILD
    uint32_t physical = addr & UINT32_C(0x1fffffff);
    if (physical < UINT32_C(0x800000) && !g_ls_mode && !g_ds_recording && !g_dma_cpu_read_wait) {
        uint32_t result;
        psx_cyc_ram_load_timing(cpu, rt, reg_mask);
        memcpy(&result, g_psx_ram + (physical & UINT32_C(0x1fffff)), sizeof result);
        if (g_ram_read_watch_active) debug_server_trace_ram_read_watch(physical & UINT32_C(0x1fffff), result);
        return result;
    }
#endif
    return psx_cyc_load_word_slow(cpu, addr, rt, reg_mask);
#endif
}

static inline uint16_t psx_cyc_load_half(CPUState *cpu, uint32_t addr,
                                         uint32_t rt, uint32_t reg_mask)
{
#ifndef PSX_ENABLE_BLOCK_CYCLES
    (void)cpu; (void)rt; (void)reg_mask;
    return psx_read_half(addr);
#else
#ifndef PSX_OVERLAY_DLL_BUILD
    uint32_t physical = addr & UINT32_C(0x1fffffff);
    if (physical < UINT32_C(0x800000) && !g_ls_mode && !g_ds_recording && !g_dma_cpu_read_wait) {
        uint16_t result;
        psx_cyc_ram_load_timing(cpu, rt, reg_mask);
        memcpy(&result, g_psx_ram + (physical & UINT32_C(0x1fffff)), sizeof result);
        if (g_ram_read_watch_active) debug_server_trace_ram_read_watch(physical & UINT32_C(0x1fffff), result);
        return result;
    }
#endif
    return psx_cyc_load_half_slow(cpu, addr, rt, reg_mask);
#endif
}
#endif

#ifdef __cplusplus
}
#endif
#endif
