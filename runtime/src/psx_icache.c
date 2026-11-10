#include "psx_icache.h"
#include "psx_cycles.h"
#include <stdlib.h>
#include <string.h>

/* Compatibility established by T172 authored cache observations. */
uint32_t g_psx_icache_tv[1024];
/* Instruction words a refill loaded, beside the tags. A store to RAM leaves
 * them alone, so code run again from a line the cache still holds is the code
 * that was there at the refill (PSX-CPU-005). held[i] says data[i] came from
 * a refill out of main RAM; a word without it reads memory as before. */
uint32_t g_psx_icache_data[1024];
uint8_t g_psx_icache_held[1024];
static const uint8_t *icache_ram;
static uint32_t icache_ram_bytes;
int g_psx_icache_active = -1;
int g_input_instruction_histogram_active = 0;
void (*g_input_instruction_histogram_callback)(uint32_t) = NULL;
void (*g_psx_cpu_step_boundary_callback)(CPUState *, uint32_t, uint64_t) = NULL;

static int environment_enabled = -1;
static int shadow_state;
static uint32_t recorded_tags[1024], suspended_tags[1024];
static uint32_t recorded_data[1024], suspended_data[1024];
static uint8_t recorded_held[1024], suspended_held[1024];

void psx_icache_attach_ram(const uint8_t *ram, uint32_t bytes)
{
    icache_ram = ram;
    icache_ram_bytes = bytes;
}

/* Main RAM and its mirrors fill only the first 8 MB of physical space. */
static void icache_load_word(unsigned index, uint32_t address)
{
    uint32_t physical = address & 0x1FFFFFFFu;
    if (!icache_ram || !icache_ram_bytes || physical >= 0x00800000u) {
        g_psx_icache_held[index] = 0;
        return;
    }
    const uint8_t *p = icache_ram + (physical & (icache_ram_bytes - 1u));
    g_psx_icache_data[index] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    g_psx_icache_held[index] = 1;
}

int psx_icache_enabled(void)
{
    if (environment_enabled < 0) {
        const char *value = getenv("PSX_ICACHE");
        environment_enabled = !(value && value[0] == '0');
    }
    return environment_enabled;
}

/* Legacy tag-only states cannot recover a stale instruction word. Their
 * documented compatibility path reconstructs RAM; new snapshots retain data. */
void psx_icache_rederive_data(void)
{
    for (unsigned i = 0; i < 1024; ++i) {
        uint32_t tag = g_psx_icache_tv[i];
        if ((tag & 3u) == 0u && tag < 0xA0000000u) icache_load_word(i, tag);
        else g_psx_icache_held[i] = 0;
    }
}

void psx_icache_reset(void)
{
    for (unsigned i = 0; i < 1024; ++i) g_psx_icache_tv[i] = 1;
    memset(g_psx_icache_data, 0, sizeof g_psx_icache_data);
    memset(g_psx_icache_held, 0, sizeof g_psx_icache_held);
    g_psx_icache_active = psx_icache_enabled();
}

int psx_icache_shadow_record_begin(void)
{
    if (shadow_state) return 0;
    memcpy(recorded_tags, g_psx_icache_tv, sizeof recorded_tags);
    memcpy(recorded_data, g_psx_icache_data, sizeof recorded_data);
    memcpy(recorded_held, g_psx_icache_held, sizeof recorded_held);
    shadow_state = 1;
    return 1;
}

int psx_icache_shadow_replay_begin(void)
{
    if (shadow_state != 1) return 0;
    memcpy(suspended_tags, g_psx_icache_tv, sizeof suspended_tags);
    memcpy(suspended_data, g_psx_icache_data, sizeof suspended_data);
    memcpy(suspended_held, g_psx_icache_held, sizeof suspended_held);
    memcpy(g_psx_icache_tv, recorded_tags, sizeof recorded_tags);
    memcpy(g_psx_icache_data, recorded_data, sizeof recorded_data);
    memcpy(g_psx_icache_held, recorded_held, sizeof recorded_held);
    shadow_state = 2;
    return 1;
}

void psx_icache_shadow_replay_end(void)
{
    if (shadow_state != 2) return;
    memcpy(g_psx_icache_tv, suspended_tags, sizeof suspended_tags);
    memcpy(g_psx_icache_data, suspended_data, sizeof suspended_data);
    memcpy(g_psx_icache_held, suspended_held, sizeof suspended_held);
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
    for (unsigned i = 0; i < 4; ++i) g_psx_icache_tv[start + i] = 2;
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
            if (i >= offset) icache_load_word(start + i, line + i * 4u);
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

uint32_t psx_icache_snapshot_bytes(void) { return PSX_ICACHE_SNAPSHOT_WIRE_BYTES; }

static void icache_wire_u32(uint8_t *p, uint32_t v)
{
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static uint32_t icache_wire_read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
void psx_icache_snapshot_write(uint8_t *p)
{
    for (unsigned i=0;i<1024;i++) {
        icache_wire_u32(p+4u*i,g_psx_icache_tv[i]);
        icache_wire_u32(p+4096u+4u*i,g_psx_icache_data[i]);
    }
    memcpy(p+8192u,g_psx_icache_held,1024u);
}
int psx_icache_snapshot_validate(const uint8_t *p,uint32_t n)
{
    if (!p) return 0;
    if (n==PSX_ICACHE_LEGACY_WIRE_BYTES) return 1;
    if (n!=PSX_ICACHE_SNAPSHOT_WIRE_BYTES) return 0;
    for (unsigned i=0;i<1024;i++) if (p[8192u+i]>1u) return 0;
    return 1;
}
int psx_icache_snapshot_read(const uint8_t *p,uint32_t n)
{
    if (!psx_icache_snapshot_validate(p,n)) return 0;
    for (unsigned i=0;i<1024;i++) g_psx_icache_tv[i]=icache_wire_read_u32(p+4u*i);
    if (n==PSX_ICACHE_LEGACY_WIRE_BYTES) { psx_icache_rederive_data(); return 1; }
    for (unsigned i=0;i<1024;i++) g_psx_icache_data[i]=icache_wire_read_u32(p+4096u+4u*i);
    memcpy(g_psx_icache_held,p+8192u,1024u);
    return 1;
}

/* Modern BIU tag-test/invalidate/lock entry point, sharing the complete
 * replay cache/shadow state rather than maintaining a second tag owner. */
void psx_icache_isc_store(uint32_t biu, uint32_t addr, uint32_t value) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    /* Same gate as a refill: lockstep replay must not move the shared tags;
     * the whole-call shadow evolves its private view. */
    if (g_ls_replay_active && shadow_state != 2) return;
    if (g_psx_icache_active < 0) g_psx_icache_active = psx_icache_enabled();
    if (!g_psx_icache_active) return;
    if (!(biu & PSX_BIU_ICACHE_ENABLE)) return;
    if (!(biu & (PSX_BIU_TAG_TEST | PSX_BIU_INVALIDATE | PSX_BIU_LOCK))) return;
    uint32_t valid = (biu & PSX_BIU_TAG_TEST)
        ? ((value << ((addr & 0x3u) * 8u)) & 0xFu) : 0u;
    uint32_t line = addr & 0xFFFFFFF0u;
    uint32_t bidx = (addr & 0xFF0u) >> 2;
    for (uint32_t i = 0; i < 4u; i++) {
        g_psx_icache_tv[bidx + i] =
            ((valid & (1u << i)) ? 0x0u : 0x2u) | line | (i << 2);
        /* A tag-only rewrite does not capture instruction bytes from RAM. */
        g_psx_icache_held[bidx + i] = 0;
    }
#else
    (void)biu;
    (void)addr;
    (void)value;
#endif
}
