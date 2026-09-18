/* Exercise the real scheduler's setjmp landing after a precise slice escapes.
 * Device/dispatch seams below are explicit; unrelated link stubs never run. */
#include <assert.h>
#include <stdint.h>
#include "cpu_state.h"
#include "psx_scheduler.h"
#include "psx_bios_image.h"

int g_precise_mode, g_psx_dispatch_depth, psx_in_device_service;
int g_call_unit_depth, g_cosim_dirty_pump_site, g_psx_cyc_bb_defer;
uint32_t g_dirty_safe_resume_pc, g_psx_cyc_batch;
uint32_t *g_psx_cyc_local_acc;
static unsigned dispatches, fixups;

static uint32_t read_word(uint32_t address) {
    assert(address == 0x108u || address == 0x110u || address == 0x114u);
    return 0; /* boot, before guest TCB creation */
}
/* psx_scheduler_resume_at calls the real psx_is_dispatchable (traps.c), so the
 * seams it reaches have to answer as a live run would. Stubbed as abort() they
 * kill the fixture before the escape path under test ever runs.
 *
 * psx_publish_note only records a diagnostic note and returns, so it is inert
 * here. The two dispatchability inputs are not: a zero answer makes resume_at
 * take trap_crash instead of unwinding, so they assert on what they are asked
 * rather than answering blind. */
void psx_publish_note(uint32_t site, uint32_t target, uint32_t origin) {
    (void)site; (void)target; (void)origin;
}
int psx_bios_is_entry(uint32_t addr) {
    /* RAM, not ROM, and no backend is selected in this fixture. */
    assert(addr == 0x80001004u);
    return 0;
}
int dirty_ram_is_dirty(uint32_t phys) {
    /* Dirty RAM runs through the interpreter and is re-enterable anywhere. */
    assert(phys == 0x1004u);
    return 1;
}
PsxBiosImageInfo psx_bios_image; /* no BIOS selected: the kbless window is empty */
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
