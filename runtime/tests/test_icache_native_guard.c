/* Reuse the authored CPU/memory/IRQ seams; execute real guard and cache owners. */
#define main inherited_cache_fixture_main
#include "test_icache_slice_admission.c"
#undef main

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
    return 0;
}
