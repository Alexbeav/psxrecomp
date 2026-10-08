#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "psx_icache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>

static uint32_t ram[0x200000u / 4u];
static int in_exception, dirty = 1;
extern int g_precise_mode;
int psx_get_in_exception(void) { return in_exception; }
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)ram; }
uint8_t *g_psx_ram = (uint8_t *)ram;
volatile int g_ds_recording;
uint32_t g_dma_cpu_read_wait;
int g_ram_read_watch_active;
/* Timing credits are disabled; the production guest value delay stays active. */
int g_psx_load_delay;
/* No device deadline, IRQ, enhancement or compiled body is involved. The
 * production precision loop must execute ADDIU/JR/NOP from authored cache. */
uint64_t psx_cycle_count, psx_next_service_cycle = UINT64_MAX;
uint64_t g_psx_cycle_fast_limit = UINT64_MAX;
uint32_t i_stat, i_mask, g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;
int psx_in_device_service, g_event_step_conservative, g_psx_call_bail;
int source_gpu_runtime_active(void) { return 0; }
int dirty_ram_is_dirty(uint32_t phys) { (void)phys; return dirty; }
int psx_is_dispatchable(uint32_t pc) { return pc == 0x80002000u; }
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
void psx_publish_note(uint32_t site, uint32_t target, uint32_t origin)
{ (void)site; (void)target; (void)origin; }
int psx_ws_backdrop_preload(void) { return 0; }
int psx_ws_is_cull_bias_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_plane_nx_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_xclip_load_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_signed_x_bound_site(uint32_t pc, uint32_t insn)
{ (void)pc; (void)insn; return 0; }
int psx_ws_angle_site(uint32_t pc, uint32_t insn, uint32_t *out)
{ (void)pc; (void)insn; (void)out; return 0; }
void psx_pgxp_alu(CPUState *cpu, uint32_t insn, uint32_t result, uint32_t a, uint32_t b)
{ (void)cpu; (void)insn; (void)result; (void)a; (void)b; }
void psx_pgxp_load(CPUState *cpu, uint32_t insn, uint32_t addr, uint32_t result)
{ (void)cpu; (void)insn; (void)addr; (void)result; }
extern uint32_t g_slice_exit_reason, g_slice_exit_iter, g_slice_exit_dispatchable, g_slice_exit_dirty;

extern uint64_t g_slice_irq_taken;
static unsigned irq_takes, irq_boundaries;
int psx_interrupt_cooldown_active(void) { return 0; }
int psx_irq_opcode_eligible(uint32_t pc) { (void)pc; return 1; }
void psx_check_interrupts(CPUState *cpu)
{
    assert((i_stat & i_mask) != 0u);
    assert(++irq_takes <= 2u);
    i_stat = 0;
    if (irq_takes == 2u) cpu->gpr[8] = 1u;
}
static void wait_irq_boundary(CPUState *cpu, uint32_t pc, uint64_t cycles)
{
    (void)cpu; (void)pc; (void)cycles;
    if (++irq_boundaries == 32u) i_stat = 1u;
    if (irq_boundaries > 1000u) {
        fprintf(stderr, "FAIL: cached wait loop starved second IRQ; takes=%u slice_takes=%llu\n",
                irq_takes, (unsigned long long)g_slice_irq_taken);
        exit(1);
    }
}
static jmp_buf cache_yield;
static unsigned cache_yields;
int psx_scheduler_can_resume_checkpoint(void) { return 1; }
void psx_scheduler_resume_checkpoint(CPUState *cpu)
{
    uint8_t wire[DIRTY_RAM_CHECKPOINT_BYTES], again[DIRTY_RAM_CHECKPOINT_BYTES];
    assert(!g_precise_mode && dirty_ram_checkpoint_resume_pending());
    assert(g_slice_exit_reason == 5u && g_slice_exit_iter == 200000u);
    assert(!g_slice_exit_dispatchable && cpu->load_value_rt);
    dirty_ram_checkpoint_write(wire);
    assert(wire[20] == 1u); /* cache ownership, not a forced native entry */
    assert(dirty_ram_checkpoint_read(wire, sizeof wire));
    dirty_ram_checkpoint_write(again);
    assert(!memcmp(wire, again, sizeof wire));
    assert(++cache_yields == 1u);
    longjmp(cache_yield, 1);
}

int main(void)
{
    static CPUState cpu;
    const uint32_t pc = 0x80001000u, index = (pc >> 2) & 1023u;
    psx_icache_bind_memory((const uint8_t *)ram, sizeof ram, NULL);
    psx_icache_reset();
    g_psx_icache_active = 1;
    g_psx_icache_tv[index] = pc;
    g_psx_icache_words[index] = 0x24080001u;
    g_precise_mode = 1;
    assert(!psx_slice_block_impl(&cpu, pc, 1, 0));
    g_precise_mode = 0;
    in_exception = 1;
    assert(!psx_slice_block_impl(&cpu, pc, 1, 0));
    assert(g_icache_execution_stats.nested_stale_blocks == 2);
    assert(g_icache_execution_stats.exception_stale_blocks == 1);
    assert(!g_icache_execution_stats.stale_blocks);
    assert(!g_icache_execution_stats.stale_blocks_in_exception);
    in_exception = 0;
    g_psx_icache_words[index] = ram[0x1000u / 4u];
    g_psx_icache_tv[index + 1] = pc + 4u;
    g_psx_icache_words[index + 1] = 0x24090001u;
    g_psx_precise_slice = 0;
    assert(!psx_slice_bios_block(&cpu, pc, 1, 0));
    assert(!g_icache_execution_stats.stale_blocks);
    memset(&g_icache_execution_stats, 0, sizeof g_icache_execution_stats);
    ram[0x1000u / 4u] = 0x24080007u; /* RAM would set t0=7. */
    ram[0x1004u / 4u] = 0x03e00008u; /* jr ra */
    ram[0x1008u / 4u] = 0u;
    for (unsigned k = 0; k < 3; ++k) {
        g_psx_icache_tv[index + k] = pc + 4u * k;
        g_psx_icache_words[index + k] = ram[0x1000u / 4u + k];
    }
    g_psx_icache_words[index] = 0x2408002au; /* Cached code sets t0=42. */
    cpu.gpr[31] = 0x80002000u;
    assert(psx_slice_block_impl(&cpu, pc, 3, 0));
    assert(cpu.gpr[8] == 42u && cpu.pc == cpu.gpr[31]);
    assert(!g_precise_mode);
    assert(g_icache_execution_stats.stale_blocks == 1u);
    assert(g_icache_execution_stats.first_block == pc && g_icache_execution_stats.last_block == pc);
    assert(g_icache_execution_stats.stale_fetches == 1u);
    assert(g_icache_execution_stats.first_fetch_pc == pc && g_icache_execution_stats.last_fetch_pc == pc);
    assert(!g_icache_execution_stats.stale_blocks_in_exception && !g_icache_execution_stats.nested_stale_blocks);
    /* A host mod write must expose the patched ADDIU immediately. Guest stores
     * in the preceding control deliberately kept the old cached ADDIU. */
    psx_icache_refresh_host_write(0xa0001000u, 4u);
    g_psx_icache_words[index + 2u] = 0x24090001u; /* unrelated cached delay-slot word */
    memset(&cpu, 0, sizeof cpu);
    cpu.pc = pc; cpu.gpr[31] = 0x80002000u;
    assert(psx_slice_block_impl(&cpu, pc, 3u, 0));
    assert(cpu.gpr[8] == 7u && cpu.gpr[9] == 1u && cpu.pc == cpu.gpr[31]);
    puts("PASS: host code write executes patched instruction; guest store retains stale word");
    /* First IRQ is already pending at entry; a later IRQ releases a cached
     * no-call wait loop. The original one-take policy hits the authored guard. */
    const uint32_t wait_loop[] = {0x1100ffffu, 0x25290001u, 0x03e00008u, 0u};
    memset(&cpu, 0, sizeof cpu);
    memset(&g_icache_execution_stats, 0, sizeof g_icache_execution_stats);
    for (unsigned k = 0; k < 4u; ++k) {
        ram[0x1000u / 4u + k] = wait_loop[k];
        g_psx_icache_tv[index + k] = pc + 4u * k;
        g_psx_icache_words[index + k] = wait_loop[k];
    }
    ram[0x1000u / 4u] = 0u; /* RAM no longer contains the cached BEQ. */
    dirty = 0; i_stat = i_mask = 1u; cpu.cop0[12] = 0x401u;
    cpu.gpr[31] = 0x80002000u;
    irq_takes = irq_boundaries = 0u; g_slice_irq_taken = 0;
    g_psx_cpu_step_boundary_callback = wait_irq_boundary;
    assert(psx_slice_block_impl(&cpu, pc, 4u, 0));
    g_psx_cpu_step_boundary_callback = NULL;
    assert(irq_takes == 2u && g_slice_irq_taken == 2u);
    assert(cpu.gpr[8] == 1u && cpu.pc == cpu.gpr[31]);
    assert(g_slice_exit_dispatchable && !g_precise_mode);
    printf("PASS: cached wait takes=%u slice_takes=%llu boundaries=%u\n",
           irq_takes, (unsigned long long)g_slice_irq_taken, irq_boundaries);
    i_stat = i_mask = 0;
    /* Three precision-loop iterations per lap exceed the old 200,000 guard.
     * Every loop PC is clean text without a compiled dispatcher entry. */
    enum { LAPS = 70001 };
    const uint32_t loop[] = {0u, 0u, 0x25290001u, 0x256b0004u, 0x152afffdu,
                             0x8d680000u, 0x250c0001u, 0x03e00008u, 0u};
    memset(&cpu, 0, sizeof cpu);
    memset(&g_icache_execution_stats, 0, sizeof g_icache_execution_stats);
    for (unsigned k = 0; k < sizeof loop / sizeof loop[0]; ++k) {
        ram[0x1000u / 4u + k] = loop[k];
        g_psx_icache_tv[index + k] = pc + 4u * k;
        g_psx_icache_words[index + k] = loop[k];
    }
    ram[0x1008u / 4u] = 0x25290002u; /* RAM increments t1 twice as fast. */
    for (unsigned k = 0; k < LAPS; ++k) ram[0x4000u / 4u + k] = k + 1u;
    cpu.gpr[10] = LAPS; cpu.gpr[11] = 0x80003ffcu; cpu.gpr[31] = 0x80002000u;
    dirty = 0;
    i_stat = 1u; i_mask = 0u; /* Pending IRQ is masked; source profile remains off. */
    if (!setjmp(cache_yield)) {
        assert(psx_slice_block_impl(&cpu, pc, 9, 0));
    } else {
        assert(dirty_ram_checkpoint_resume_pending());
        dirty_ram_checkpoint_resume(&cpu);
    }
    assert(cpu.pc == cpu.gpr[31] && g_slice_exit_dispatchable);
    assert(!g_slice_exit_dirty);
    assert(g_slice_exit_reason == 1u && g_slice_exit_iter < 200000u && cache_yields == 1u);
    assert(!dirty_ram_checkpoint_resume_pending());
    assert(cpu.gpr[9] == LAPS && cpu.gpr[8] == LAPS);
    /* The last LW's immediate consumer reads the preceding lap's value. */
    assert(cpu.gpr[12] == LAPS && !cpu.load_value_rt && !cpu.load_value_age);
    assert(cpu.gpr[11] == 0x80003ffcu + 4u * LAPS && !g_precise_mode);
    assert(g_icache_execution_stats.stale_blocks == 1u);
    assert(g_icache_execution_stats.stale_fetches == LAPS);
    assert(g_icache_execution_stats.first_fetch_pc == pc+8u && g_icache_execution_stats.last_fetch_pc == pc+8u);
    printf("PASS: cached slice yields=%u laps=%u load=%u checkpoint_pending=%d\n",
           cache_yields, (unsigned)LAPS, cpu.gpr[8], dirty_ram_checkpoint_resume_pending());
    puts("PASS: nested admission, BIOS range and actual cached instruction fallback");
}
