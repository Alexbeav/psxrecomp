#ifndef PSX_ICACHE_H
#define PSX_ICACHE_H

#include <stdint.h>
#include "cpu_state.h"
#include "input_instruction_histogram.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t g_psx_icache_tv[1024];
extern uint32_t g_psx_icache_data[1024];
extern uint8_t g_psx_icache_held[1024];
extern int g_psx_icache_active;
extern int g_ls_replay_active;
extern void (*g_psx_cpu_step_boundary_callback)(CPUState *, uint32_t, uint64_t);
void psx_icache_reset(void);

/* BIU / cache control (0xFFFE0130) bits the cache model reads (PSX-SPX
 * memorycontrol, "FFFE0130h - BCC, BIU/Cache Configuration Register"). */
#define PSX_BIU_ICACHE_ENABLE 0x800u  /* bit 11, IS1 */
#define PSX_BIU_DCACHE_ENABLE 0x080u  /* bit 7, DS */
#define PSX_BIU_TAG_TEST      0x004u  /* bit 2, TAG */
#define PSX_BIU_INVALIDATE    0x002u  /* bit 1, INV */
#define PSX_BIU_LOCK          0x001u  /* bit 0, LOCK */

/* A CPU store made while SR.IsC is set, applied to the I-cache model: the
 * I-cache half of Beetle PS_CPU::WriteMemory's IsC branch. With the I-cache
 * enabled and a tag-test, invalidate or lock mode bit set, the store rewrites
 * the tag and valid bits of the four words of its line; that is how FlushCache
 * (A 44h) invalidates the cache. `biu` is the current 0xFFFE0130 value and
 * `value` the stored register value (only its low byte lanes matter). */
void psx_icache_isc_store(uint32_t biu, uint32_t addr, uint32_t value);


void psx_icache_attach_ram(const uint8_t *, uint32_t);
void psx_icache_rederive_data(void);
/* Tags, held words, and one-byte validity flags; the legacy section has tags only. */
#define PSX_ICACHE_LEGACY_WIRE_BYTES (1024u * 4u)
#define PSX_ICACHE_SNAPSHOT_WIRE_BYTES (1024u * 9u)
uint32_t psx_icache_snapshot_bytes(void);
void psx_icache_snapshot_write(uint8_t *);
int psx_icache_snapshot_validate(const uint8_t *, uint32_t);
int psx_icache_snapshot_read(const uint8_t *, uint32_t);
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

/* An intra-line follower reached by fall-through hits at a cached address and
 * fetches at an uncached one. Game code emits it with PSX_EXEC_PC, whose
 * segment is known only at run time. */
static inline void psx_icache_follow(CPUState *cpu, uint32_t address)
{
    if (address >= 0xA0000000u) psx_icache_fetch(cpu, address);
    else psx_cpu_step_boundary(cpu, address);
}

/* The word an instruction fetch at a cached address executes: the refilled word
 * while the tag matches, else memory. Call before the fetch's own refill; a
 * miss then loads memory_word, so both orders give the same word. */
static inline uint32_t psx_icache_instruction(uint32_t address, uint32_t memory_word)
{
    if (g_psx_icache_active < 0) g_psx_icache_active = psx_icache_enabled();
    if (!g_psx_icache_active) return memory_word;
    unsigned index = (address >> 2) & 1023u;
    if (address < 0xA0000000u && g_psx_icache_tv[index] == address &&
        g_psx_icache_held[index])
        return g_psx_icache_data[index];
    return memory_word;
}

static inline void psx_icache_fetch_interp_after_boundary(CPUState *cpu, uint32_t address)
{
    if (g_input_instruction_histogram_active)
        input_instruction_histogram_sample(address);
#if defined(PSX_ENABLE_BLOCK_CYCLES) || defined(PSX_COSIM)
    if (g_ls_replay_active) return;
    /* The instruction boundary has already run. Resolve the selector directly
     * here instead of re-entering psx_icache_fetch and servicing it twice. */
    if (g_psx_icache_active < 0) g_psx_icache_active = psx_icache_enabled();
    if (!g_psx_icache_active) return;
    unsigned index = (address >> 2) & 1023u;
    if (g_psx_icache_tv[index] == address) return;
    psx_icache_fetch_miss(cpu, address);
#else
    (void)cpu; (void)address;
#endif
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
