#include "cpu_state.h"
#include "psx_icache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_ls_replay_active = 0;
static uint64_t test_cycles = 0;

void psx_advance_cycles(uint32_t cycles) { test_cycles += cycles; }
/* PSX_OVERLAY_DLL_BUILD makes the step boundary an extern. No observer is
 * installed in this fixture, matching the host inline with a null callback. */
void psx_cpu_step_boundary(CPUState *cpu, uint32_t address) { (void)cpu; (void)address; }
/* Referenced only by the step-boundary publish path in psx_icache.c, which runs
 * after psx_cpu_step_boundary_enabled(); this fixture installs no observer. */
void overlay_flush_cycles(void)
{
    fputs("overlay_flush_cycles: reached from psx_icache step-boundary path; "
          "not modelled by this test\n", stderr);
    abort();
}
uint64_t psx_get_cycle_count(void)
{
    fputs("psx_get_cycle_count: reached from psx_icache step-boundary path; "
          "not modelled by this test\n", stderr);
    abort();
}

static int expect(int condition, const char *message) {
    if (condition) return 1;
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
}

int main(void) {
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));

    psx_icache_reset();
    g_psx_icache_active = 1;
    test_cycles = 0;
    cpu.read_absorb_which = 1;
    cpu.read_absorb[0] = 77u;
    cpu.read_absorb[1] = 99u;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    if (!expect(test_cycles == 7u, "word-0 cold refill cost")) return 1;
    if (!expect(cpu.read_absorb_which == 0u && cpu.read_absorb[1] == 0u &&
                cpu.read_absorb[0] == 77u,
                "true miss clears only the selected load give-back")) return 1;
    cpu.read_absorb_which = 1;
    cpu.read_absorb[1] = 55u;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    psx_icache_fetch_interp(&cpu, 0x80010004u);
    if (!expect(test_cycles == 7u, "steady tag hits are cycle-free")) return 1;
    if (!expect(cpu.read_absorb_which == 1u && cpu.read_absorb[1] == 55u,
                "tag hits preserve load give-back")) return 1;

    psx_icache_reset();
    g_psx_icache_active = 1;
    test_cycles = 0;
    psx_icache_fetch_interp(&cpu, 0x80010008u);
    if (!expect(test_cycles == 5u, "word-2 partial refill cost")) return 1;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    if (!expect(test_cycles == 12u, "earlier word remains invalid after partial refill")) return 1;

    psx_icache_reset();
    g_psx_icache_active = 1;
    test_cycles = 0;
    psx_icache_fetch_interp(&cpu, 0xA0010000u);
    psx_icache_fetch_interp(&cpu, 0xA0010000u);
    if (!expect(test_cycles == 8u, "KSEG1 remains uncached")) return 1;

    psx_icache_reset();
    g_psx_icache_active = 1;
    test_cycles = 0;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    psx_icache_fetch_interp(&cpu, 0x00010000u);
    if (!expect(test_cycles == 14u,
                "KUSEG/KSEG0 aliases replace full virtual tags")) return 1;
    psx_icache_reset();
    if (!expect(g_psx_icache_tv[0] == 1u,
                "reset makes a warmed line cold")) return 1;

    g_psx_icache_active = 1;
    test_cycles = 0;
    psx_icache_fetch(&cpu, 0x80010000u);
    if (!expect(test_cycles == 7u,
                "compiled-code compatibility wrapper preserves refill")) return 1;

    psx_icache_reset();
    g_psx_icache_active = 1;
    test_cycles = 0;
    g_ls_replay_active = 1;
    cpu.read_absorb_which = 1;
    cpu.read_absorb[1] = 44u;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    g_ls_replay_active = 0;
    if (!expect(test_cycles == 0u, "lockstep replay does not mutate cache")) return 1;
    if (!expect(g_psx_icache_tv[0] == 1u, "replay preserved cold tag")) return 1;
    if (!expect(cpu.read_absorb_which == 1u && cpu.read_absorb[1] == 44u,
                "replay preserves load give-back")) return 1;

    g_psx_icache_active = 0;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    if (!expect(test_cycles == 0u, "disabled cache model is free")) return 1;
    if (!expect(cpu.read_absorb_which == 1u && cpu.read_absorb[1] == 44u,
                "disabled cache preserves load give-back")) return 1;

    /* A four-word trampoline is primed, then overwritten in RAM. A cached
     * read must retain it until an actual conflicting fetch or invalidation. */
    static uint32_t ram[0x200000u / 4u];
    const uint32_t pc = 0x80023000u, index = (pc >> 2) & 1023u;
    const uint32_t stub[4] = {0x8fa80018u, 0u, 0x01000008u, 0u};
    psx_icache_bind_memory((const uint8_t *)ram, sizeof ram, NULL);
    psx_icache_reset();
    memcpy(ram + 0x23000u / 4u, stub, sizeof stub);
    psx_icache_fetch(&cpu, pc);
    for (unsigned i = 0; i < 4; ++i) ram[0x23000u / 4u + i] = 0xffffffffu;
    for (unsigned i = 0; i < 4; ++i)
        if (!expect(psx_icache_read_cached(pc + 4u*i, 0xffffffffu) == stub[i],
                    "RAM stores preserve all four cached trampoline words")) return 1;
    if (!expect(psx_icache_block_stale(pc, 4u),
                "native block cannot execute the overwritten RAM image")) return 1;
    if (!expect(!psx_icache_block_stale(0xa0023000u, 4u),
                "uncached native block has no stale cache dependency")) return 1;
    if (!expect(psx_icache_read_cached(0xa0023000u, 0xffffffffu) == 0xffffffffu,
                "uncached alias observes changed RAM")) return 1;
    if (!expect(psx_icache_shadow_record_begin(), "record cache contents")) return 1;
    psx_icache_fetch(&cpu, pc + 0x1000u);
    if (!expect(psx_icache_read_cached(pc, 0xffffffffu) == 0xffffffffu,
                "native conflicting-line fetch evicts cached trampoline")) return 1;
    if (!expect(psx_icache_shadow_replay_begin(), "restore recorded cache contents")) return 1;
    if (!expect(psx_icache_read_cached(pc, 0xffffffffu) == stub[0],
                "shadow replay restores bytes as well as tags")) return 1;
    psx_icache_shadow_replay_end();
    if (!expect(psx_icache_read_cached(pc, 0xffffffffu) == 0xffffffffu,
                "shadow end restores suspended contents")) return 1;
    memcpy(ram + 0x23000u / 4u, stub, sizeof stub);
    psx_icache_fetch(&cpu, pc + 8u);
    if (!expect(g_psx_icache_tv[index] != pc &&
                psx_icache_read_cached(pc + 8u, 0xffffffffu) == stub[2],
                "partial refill leaves earlier words invalid")) return 1;
    psx_icache_isolated_store(pc, 0x804u);
    if (!expect(psx_icache_read_cached(pc + 8u, 0xffffffffu) == 0xffffffffu,
                "isolated invalidation discards cached instructions")) return 1;

    puts("PASS: I-cache timing, contents, eviction, aliases, isolation and shadow restore");
    return 0;
}
