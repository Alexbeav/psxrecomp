/* Counterexample to dropping interior fetches while retaining header fetches.
 * This tests the existing cache model, NOT an implementation of IRQ batching.
 */
#include "cpu_state.h"
#include "psx_icache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_ls_replay_active = 0;
static uint64_t cycles;
void psx_advance_cycles(uint32_t count) { cycles += count; }
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

static uint64_t run(int omit_body, uint32_t *giveback, uint32_t *tag) {
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    psx_icache_reset();
    g_psx_icache_active = 1;
    cycles = 0;
    psx_icache_fetch(&cpu, 0x80010000u); /* header */
    cpu.read_absorb_which = 1;
    cpu.read_absorb[1] = 99;
    if (!omit_body) psx_icache_fetch(&cpu, 0x80010020u); /* cold body line */
    psx_icache_fetch(&cpu, 0x80010000u); /* header again */
    *giveback = cpu.read_absorb[1];
    *tag = g_psx_icache_tv[8];
    return cycles;
}

int main(void) {
    uint32_t full_giveback, omitted_giveback, full_tag, omitted_tag;
    uint64_t full = run(0, &full_giveback, &full_tag);
    uint64_t omitted = run(1, &omitted_giveback, &omitted_tag);
    if (full != 14 || omitted != 7 || full_giveback != 0 ||
        omitted_giveback != 99 || full_tag != 0x80010020u || omitted_tag != 1) {
        puts("FAIL: expected cold-body refill/give-back counterexample changed");
        return 1;
    }
    puts("PASS: header-only fetch loses 7 guest cycles and changes cache/load state");
    return 0;
}
