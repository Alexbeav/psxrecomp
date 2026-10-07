#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "psx_icache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t ram[0x200000u / 4u];
static int in_exception;
extern int g_precise_mode;
int psx_get_in_exception(void) { return in_exception; }
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)ram; }
/* No device deadline, IRQ, enhancement or compiled body is involved. The
 * production precision loop must execute ADDIU/JR/NOP from authored cache. */
uint64_t psx_cycle_count, psx_next_service_cycle = UINT64_MAX;
uint64_t g_psx_cycle_fast_limit = UINT64_MAX;
uint32_t i_stat, i_mask, g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;
int psx_in_device_service, g_event_step_conservative, g_psx_call_bail;
int source_gpu_runtime_active(void) { return 0; }
int dirty_ram_is_dirty(uint32_t phys) { (void)phys; return 1; }
int psx_is_dispatchable(uint32_t pc) { return pc == 0x80002000u; }
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
void psx_publish_note(uint32_t site, uint32_t target, uint32_t origin)
{ (void)site; (void)target; (void)origin; }
int psx_ws_backdrop_preload(void) { return 0; }
int psx_ws_is_cull_bias_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_signed_x_bound_site(uint32_t pc, uint32_t insn)
{ (void)pc; (void)insn; return 0; }
int psx_ws_angle_site(uint32_t pc, uint32_t insn, uint32_t *out)
{ (void)pc; (void)insn; (void)out; return 0; }
void psx_pgxp_alu(CPUState *cpu, uint32_t insn, uint32_t result, uint32_t a, uint32_t b)
{ (void)cpu; (void)insn; (void)result; (void)a; (void)b; }

int main(void)
{
    CPUState cpu = {0};
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
    puts("PASS: nested admission, BIOS range and actual cached instruction fallback");
}
