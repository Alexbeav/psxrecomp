#include "psx_icache.h"
#include "psx_cycles.h"
#include <stdlib.h>
#include <string.h>

/* Compatibility established by T172 authored cache observations. */
uint32_t g_psx_icache_tv[1024];
uint32_t g_psx_icache_words[1024];
int g_psx_icache_active = -1;
int g_input_instruction_histogram_active = 0;
void (*g_input_instruction_histogram_callback)(uint32_t) = NULL;
void (*g_psx_cpu_step_boundary_callback)(CPUState *, uint32_t, uint64_t) = NULL;

static int environment_enabled = -1;
static int shadow_state;
static const uint8_t *cache_ram, *cache_rom;
static uint32_t cache_ram_size;

void psx_icache_bind_memory(const uint8_t *ram, uint32_t ram_size, const uint8_t *rom)
{
    cache_ram = ram;
    cache_ram_size = ram_size;
    cache_rom = rom;
}

static uint32_t cache_memory_word(uint32_t address)
{
    uint32_t phys = address & 0x1fffffffu;
    const uint8_t *p = NULL;
    if (phys < 0x800000u && cache_ram)
        p = cache_ram + (phys & (cache_ram_size - 1u));
    else if (phys >= 0x1fc00000u && phys < 0x1fc80000u && cache_rom)
        p = cache_rom + (phys - 0x1fc00000u);
    /* Timing-only fixtures omit backing memory. Production binds the backing
     * arrays in memory_init before executing any guest instructions. */
    if (!p) return 0;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* Version 15 stored timing tags but executed RAM bytes. Reconstruct only
 * after snapshot RAM is restored; retain tags and charge no guest cycles. */
void psx_icache_restore_legacy_words(void)
{
    for (unsigned i = 0; i < 1024u; ++i) {
        uint32_t pc = g_psx_icache_tv[i];
        g_psx_icache_words[i] = (pc & 3u) ? 0u : cache_memory_word(pc);
    }
}

static uint32_t recorded_tags[1024], suspended_tags[1024];
static uint32_t recorded_words[1024], suspended_words[1024];

uint32_t psx_icache_read_cached(uint32_t address, uint32_t memory_word)
{
    unsigned index = (address >> 2) & 1023u;
    if ((cache_ram || cache_rom) && g_psx_icache_active > 0 && address < 0xa0000000u &&
        g_psx_icache_tv[index] == address)
        return g_psx_icache_words[index];
    return memory_word;
}

int psx_icache_block_stale(uint32_t address, uint32_t words)
{
    if (g_psx_icache_active <= 0 || address >= 0xa0000000u ||
        (address & 0x1fffffffu) >= 0x800000u || !cache_ram) return 0;
    /* Only resident words can differ from the RAM image native guards checked.
     * A conservative extra word merely keeps execution in the interpreter. */
    if (words > 1024u) {
        uint64_t end = (uint64_t)address + 4ull * words;
        for (unsigned index = 0; index < 1024u; ++index) {
            uint32_t pc = g_psx_icache_tv[index];
            if (!(pc & 3u) && pc >= address && (uint64_t)pc < end &&
                g_psx_icache_words[index] != cache_memory_word(pc)) return 1;
        }
        return 0;
    }
    for (uint32_t i = 0; i < words; ++i) {
        uint32_t pc = address + 4u * i;
        unsigned index = (pc >> 2) & 1023u;
        if (g_psx_icache_tv[index] == pc &&
            g_psx_icache_words[index] != cache_memory_word(pc)) return 1;
    }
    return 0;
}

int psx_icache_enabled(void)
{
    if (environment_enabled < 0) {
        const char *value = getenv("PSX_ICACHE");
        environment_enabled = !(value && value[0] == '0');
    }
    return environment_enabled;
}

void psx_icache_reset(void)
{
    for (unsigned i = 0; i < 1024; ++i) g_psx_icache_tv[i] = 1;
    memset(g_psx_icache_words, 0, sizeof g_psx_icache_words);
    g_psx_icache_active = psx_icache_enabled();
}

int psx_icache_shadow_record_begin(void)
{
    if (shadow_state) return 0;
    memcpy(recorded_tags, g_psx_icache_tv, sizeof recorded_tags);
    memcpy(recorded_words, g_psx_icache_words, sizeof g_psx_icache_words);
    shadow_state = 1;
    return 1;
}

int psx_icache_shadow_replay_begin(void)
{
    if (shadow_state != 1) return 0;
    memcpy(suspended_tags, g_psx_icache_tv, sizeof suspended_tags);
    memcpy(suspended_words, g_psx_icache_words, sizeof g_psx_icache_words);
    memcpy(g_psx_icache_tv, recorded_tags, sizeof recorded_tags);
    memcpy(g_psx_icache_words, recorded_words, sizeof recorded_words);
    shadow_state = 2;
    return 1;
}

void psx_icache_shadow_replay_end(void)
{
    if (shadow_state != 2) return;
    memcpy(g_psx_icache_tv, suspended_tags, sizeof suspended_tags);
    memcpy(g_psx_icache_words, suspended_words, sizeof suspended_words);
    shadow_state = 0;
}

void psx_icache_shadow_abort(void)
{
    psx_icache_shadow_replay_end();
    shadow_state = 0;
}

void psx_icache_isolated_store(uint32_t address, uint32_t control)
{
    if (g_ls_replay_active && shadow_state != 2) return;
    if ((control & 0x804u) != 0x804u) return;
    unsigned start = (address & 0xff0u) >> 2;
    for (unsigned i = 0; i < 4; ++i) {
        g_psx_icache_tv[start + i] = 2;
        g_psx_icache_words[start + i] = 0;
    }
}

int psx_cpu_step_boundary_enabled(int include_replay)
{
    return g_psx_cpu_step_boundary_callback != NULL &&
        (include_replay || !g_ls_replay_active);
}

void psx_cpu_step_boundary_fn(CPUState *cpu, uint32_t address)
{
#ifndef PSX_NO_STEP_BOUNDARY
    if (!psx_cpu_step_boundary_enabled(0)) return;
#ifndef PSX_COSIM
    psx_cyc_local_publish();
    psx_cyc_batch_flush();
#endif
    g_psx_cpu_step_boundary_callback(cpu, address, psx_get_cycle_count());
#else
    (void)cpu;
    (void)address;
#endif
}

void input_instruction_histogram_sample(uint32_t pc)
{
    if (g_input_instruction_histogram_callback)
        g_input_instruction_histogram_callback(pc);
}

void psx_icache_fetch_miss(CPUState *cpu, uint32_t address)
{
#if defined(PSX_ENABLE_BLOCK_CYCLES) || defined(PSX_COSIM)
    if (g_ls_replay_active && shadow_state != 2) return;
    if (g_psx_icache_active < 0) g_psx_icache_active = psx_icache_enabled();
    if (!g_psx_icache_active) return;
    unsigned index = (address >> 2) & 1023u;
    if (g_psx_icache_tv[index] == address) return;
    uint32_t cost = 4;
    if (address < 0xa0000000u) {
        unsigned offset = index & 3u;
        unsigned start = index - offset;
        uint32_t line = address & ~15u;
        for (unsigned i = 0; i < 4; ++i) {
            g_psx_icache_tv[start + i] = (line + i * 4u) | (i < offset ? 2u : 0u);
            g_psx_icache_words[start + i] = i >= offset
                ? cache_memory_word(line + i * 4u) : 0u;
        }
        cost = 7u - offset;
    }
    cpu->read_absorb[cpu->read_absorb_which] = 0;
    cpu->read_absorb_which = 0;
    psx_advance_cycles(cost);
#else
    (void)cpu;
    (void)address;
#endif
}

void psx_icache_fetch(CPUState *cpu, uint32_t address)
{
    psx_cpu_step_boundary_fn(cpu, address);
    if (g_input_instruction_histogram_active)
        input_instruction_histogram_sample(address);
    psx_icache_fetch_miss(cpu, address);
}

void psx_icache_fetch_fn(CPUState *cpu, uint32_t address)
{
    psx_icache_fetch(cpu, address);
}
