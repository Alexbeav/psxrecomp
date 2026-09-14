/* Exercise the real scheduler's setjmp landing after a precise slice escapes.
 * Device/dispatch seams below are explicit; unrelated link stubs never run. */
#include <assert.h>
#include <stdint.h>
#include "cpu_state.h"
#include "psx_scheduler.h"

int g_precise_mode, g_psx_dispatch_depth, psx_in_device_service;
int g_call_unit_depth, g_cosim_dirty_pump_site, g_psx_cyc_bb_defer;
uint32_t g_dirty_safe_resume_pc, g_psx_cyc_batch;
uint32_t *g_psx_cyc_local_acc;
static unsigned dispatches, fixups;

static uint32_t read_word(uint32_t address) {
    assert(address == 0x108u || address == 0x110u || address == 0x114u);
    return 0; /* boot, before guest TCB creation */
}
int dirty_ram_checkpoint_resume_pending(void) { return 0; }
int savestate_pending(void) { return 0; }
void overlay_loader_shadow_scheduler_escape_fixup(void) { ++fixups; }
void psx_irq_arm_compiled_resume_pc(uint32_t pc) { assert(pc == 0x80001004u); }
void psx_dispatch(CPUState *cpu, uint32_t pc) {
    (void)cpu;
    assert(g_precise_mode == 0); /* a fresh dispatch must admit precise blocks */
    if (++dispatches == 1) {
        assert(pc == 0x80001000u);
        g_precise_mode = 1;
        psx_scheduler_resume_at(0x80001004u);
        assert(!"scheduler resume must unwind the slice");
    }
    assert(dispatches == 2 && pc == 0x80001004u);
    g_precise_mode = 1;
    psx_request_return_to_lobby();
    assert(!"lobby exit must unwind the slice");
}
int main(void) {
    CPUState cpu = {0};
    cpu.pc = 0x80001000u;
    cpu.read_word = read_word;
    psx_scheduler_run(&cpu);
    assert(dispatches == 2 && fixups == 2 && g_precise_mode == 0);
    return 0;
}
