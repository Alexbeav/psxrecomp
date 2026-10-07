#include "netplay_state_digest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint64_t psx_cycle_count;
uint32_t i_stat, i_mask, g_psx_icache_tv[1024], g_psx_icache_words[1024];
static uint8_t ram[2u << 20];
static unsigned fixture_vector;
uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t interrupts_get_cycles_since_vblank(void) { return fixture_vector ? 77u : 0u; }
uint32_t dirty_ram_get_bitmap_word_count(void) { return fixture_vector ? 2u : 0u; }
uint32_t dirty_ram_get_bitmap_word(uint32_t index)
{ assert(index < 2u); return index ? 0x12345u : 0x0badcafeu; }
void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3], uint16_t target[3],
                         int32_t irq[3], uint32_t frac[3])
{
    memset(counter, 0, 6); memset(mode, 0, 12); memset(target, 0, 6);
    memset(irq, 0, 12); memset(frac, 0, 12);
    if (fixture_vector) for (unsigned i = 0; i < 3; ++i) {
        counter[i] = (uint16_t)(31u + i); mode[i] = 0x1230u + i;
        target[i] = (uint16_t)(17u + i); irq[i] = (int32_t)i - 1;
        frac[i] = 0x314159u + i;
    }
}

static void initialize(CPUState *cpu, unsigned vector)
{
    fixture_vector = vector;
    memset(cpu, 0, sizeof *cpu); memset(ram, 0, sizeof ram);
    memset(g_psx_icache_tv, 0, sizeof g_psx_icache_tv);
    memset(g_psx_icache_words, 0, sizeof g_psx_icache_words);
    psx_cycle_count = 0; i_stat = i_mask = 0;
    if (!vector) return;
    for (unsigned i = 0; i < 32; ++i) {
        cpu->gpr[i] = i * 0x9e3779b9u ^ 0xdeadbeefu;
        cpu->gte_data[i] = 0x10203040u + i;
        cpu->gte_ctrl[i] = 0x87654321u - i;
    }
    cpu->pc = 0x80001000u; cpu->hi = 0x12345678u; cpu->lo = 0xabcdef01u;
    cpu->cop0[12] = 0x40000401u; cpu->cop0[13] = 0x33u; cpu->cop0[14] = 0x80001234u;
    psx_cycle_count = UINT64_C(0x123456789abcdef); i_stat = 0x35u; i_mask = 0x61u;
    for (unsigned i = 0; i < sizeof ram; ++i) ram[i] = (uint8_t)(17u * i + (i >> 8));
    for (unsigned i = 0; i < 1024; ++i) g_psx_icache_tv[i] = 0x80000000u + 4u * i;
}

int main(void)
{
    CPUState cpu;
    NetplayCoreParts old, current, changed_old, changed_current;
    /* Production pin H a279b5e86, compiled with these authored seams/vectors.
     * Field order is cpu, clock_irq, timers, ram, dirty, core. */
    static const NetplayCoreParts pin_h[2] = {
        {0xce8da232u, 0x7f76fcabu, 0xf288b395u, 0x8d89877eu, 0u, 0x64558775u},
        {0x97318f8au, 0x080b23e3u, 0xc5d0e5bbu, 0xc1f459d7u, 0xba264b02u, 0x1ba020a8u}
    };
    for (unsigned vector = 0; vector < 2; ++vector) {
        initialize(&cpu, vector);
        netplay_core_digest_parts_version(&cpu, &old, 1);
        assert(!memcmp(&old, &pin_h[vector], sizeof old));
    }
    initialize(&cpu, 0);
    netplay_core_digest_parts_version(&cpu, &old, 1);
    netplay_core_digest_parts(&cpu, &current);
    g_psx_icache_words[0] = 0x24080001u;
    netplay_core_digest_parts_version(&cpu, &changed_old, 1);
    netplay_core_digest_parts(&cpu, &changed_current);
    assert(old.core == changed_old.core);
    assert(current.core != changed_current.core);
    assert(current.cpu == changed_current.cpu && current.ram == changed_current.ram);
    assert(current.clock_irq != changed_current.clock_irq);
    g_psx_icache_tv[0] = 0x80001000u;
    netplay_core_digest_parts_version(&cpu, &changed_old, 1);
    assert(old.core != changed_old.core);
    puts("PASS: legacy replay excludes cache words; current replay and rollback retain them");
}
