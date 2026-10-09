/* Reuse the authored CPU/memory/IRQ seams; execute real guard and cache owners. */
#define main inherited_cache_fixture_main
#include "test_icache_slice_admission.c"
#undef main

static void alias_case(uint32_t primed, uint32_t other, int guard_enabled)
{
    CPUState cpu = {0};
    psx_icache_reset();
    g_psx_icache_active = 1;
    psx_cycle_count = 0;
    memset(&g_icache_execution_stats, 0, sizeof g_icache_execution_stats);
    ram[0x11000u / 4u] = 0x2408002au;
    ram[0x11004u / 4u] = 0x03e00008u;
    ram[0x11008u / 4u] = 0;
    psx_icache_fetch_interp(&cpu, primed);
    assert(psx_cycle_count == 7u);
    ram[0x11000u / 4u] = 0x24080007u; /* Guest RAM store without invalidation. */
    assert(psx_icache_read_cached(other, ram[0x11000u / 4u]) == 0x2408002au);
    assert(psx_icache_read_cached(0xa0011000u, ram[0x11000u / 4u]) == 0x24080007u);
    assert(psx_icache_block_stale(other, 3u));
    assert(psx_icache_block_stale(other, 1025u));
    /* The large-block scan must exclude a resident word just before its start
     * or at its end, even when the resident tag uses the other cached alias. */
    assert(!psx_icache_block_stale(other + 4u, 1025u));
    assert(!psx_icache_block_stale(other - 4100u, 1025u));
    assert(!psx_icache_block_stale(0xa0011000u, 1025u));
    cpu.read_absorb_which = 1;
    cpu.read_absorb[1] = 55u;
    psx_icache_fetch_interp(&cpu, other);
    assert(psx_cycle_count == 7u && cpu.read_absorb_which == 1u && cpu.read_absorb[1] == 55u);
    assert(g_psx_icache_tv[(other >> 2) & 1023u] == primed);
    /* KSEG1 reads RAM and charges its existing four cycles without evicting. */
    psx_icache_fetch_interp(&cpu, 0xa0011000u);
    assert(psx_cycle_count == 11u);
    assert(psx_icache_read_cached(other, ram[0x11000u / 4u]) == 0x2408002au);
    cpu.pc = other;
    cpu.gpr[31] = 0x80002000u;
    int admitted = psx_slice_block_impl(&cpu, other, 1u, 0);
    assert(admitted == guard_enabled);
    assert(cpu.gpr[8] == (guard_enabled ? 42u : 0u));
    assert(cpu.pc == (guard_enabled ? cpu.gpr[31] : other));
    printf("alias=%08x->%08x guard=%d cache=42 uncached=7 refill=7 alias_hit=0\n",
           primed, other, admitted);
}

int main(void)
{
    CPUState cpu = {0};
    const uint32_t pc = 0x80001000u, index = (pc >> 2) & 1023u;
    psx_icache_bind_memory((const uint8_t *)ram, sizeof ram, NULL);
    psx_icache_reset();
    g_psx_icache_active = 1;
    ram[0x1000u / 4u] = 0x24080007u; /* RAM: t0=7. */
    ram[0x1004u / 4u] = 0x03e00008u;
    ram[0x1008u / 4u] = 0u;
    for (unsigned k = 0; k < 3; ++k) {
        g_psx_icache_tv[index + k] = pc + 4u * k;
        g_psx_icache_words[index + k] = ram[0x1000u / 4u + k];
    }
    g_psx_icache_words[index] = 0x2408002au; /* Cache: t0=42. */
    cpu.pc = pc;
    cpu.gpr[31] = 0x80002000u;
    g_psx_precise_slice = 0;
    int admitted = psx_slice_block_impl(&cpu, pc, 1u, 0);
    if (admitted) {
        assert(cpu.gpr[8] == 42u && cpu.pc == cpu.gpr[31]);
        assert(g_icache_execution_stats.stale_blocks == 1u);
    } else {
        assert(!cpu.gpr[8] && cpu.pc == pc);
        assert(!g_icache_execution_stats.stale_blocks);
    }
    assert(g_psx_icache_active == 1);
    assert(psx_icache_read_cached(pc, ram[0x1000u / 4u]) == 0x2408002au);
    psx_icache_reset();
    g_psx_icache_active = 1;
    memset(&cpu, 0, sizeof cpu);
    psx_cycle_count = 0;
    psx_icache_fetch_interp(&cpu, pc);
    assert(psx_cycle_count == 7u);
    psx_icache_fetch_interp(&cpu, pc);
    assert(psx_cycle_count == 7u);
    printf("native_guard_admitted=%d cache_contents=42 miss_cycles=7 hit_cycles=0\n", admitted);
    alias_case(0x80011000u, 0x00011000u, admitted);
    alias_case(0x00011000u, 0x80011000u, admitted);
    return 0;
}
