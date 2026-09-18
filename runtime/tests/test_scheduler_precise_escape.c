/* Exercise the real scheduler's setjmp landing after a precise slice escapes.
 * Device/dispatch seams below are explicit; unrelated link stubs never run. */
#include <assert.h>
#include <stdint.h>
#include "cpu_state.h"
#include "psx_bios_backend.h"
#include "psx_bios_image.h"
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
/* traps.c publishes dispatch notes for the debug console. This fixture has no
 * publisher, and source_fixture_link.py stubs an unresolved seam with a
 * placeholder that aborts -- so the escape path died before reaching the
 * behaviour this test names (T145). Publishing nothing is the right fixture. */
void psx_publish_note(uint32_t site, uint32_t target, uint32_t origin) {
    (void)site; (void)target; (void)origin;
}
/* psx_is_dispatchable() consults the BIOS entry table and the dirty-RAM map
 * for EVERY pc, not only ROM ones (T110). This fixture links neither module,
 * so source_fixture_link.py stubbed both with placeholders that abort and the
 * scheduler died before reaching the behaviour this test names (T145). No BIOS
 * image, no dirty pages and no kernel-less window is the honest fixture, and it
 * makes the RAM resume PC dispatchable, which is what the test exercises. */
int psx_bios_is_entry(uint32_t addr) { (void)addr; return 0; }
int dirty_ram_is_dirty(uint32_t phys) { (void)phys; return 0; }
PsxBiosImageInfo psx_bios_image;
int dirty_ram_checkpoint_resume_pending(void) { return 0; }
int savestate_pending(void) { return 0; }
void overlay_loader_shadow_scheduler_escape_fixup(void) { ++fixups; }
void psx_irq_arm_compiled_resume_pc(uint32_t pc) { assert(pc == 0x80001004u); }
void psx_dispatch(CPUState *cpu, uint32_t pc) {
    (void)cpu;
    assert(g_precise_mode == 0); /* a fresh dispatch must admit precise blocks */
    /* ...and no dirty IRQ pump frame can be live at the scheduler top. */
    assert(g_cosim_dirty_pump_site == 0 && g_dirty_safe_resume_pc == 0u);
    if (++dispatches == 1) {
        assert(pc == 0x80001000u);
        g_precise_mode = 1;
        g_cosim_dirty_pump_site = 6;               /* escape from inside a pump */
        g_dirty_safe_resume_pc = 0x80001234u;
        psx_scheduler_resume_at(0x80001004u);
        assert(!"scheduler resume must unwind the slice");
    }
    assert(dispatches == 2 && pc == 0x80001004u);
    g_precise_mode = 1;
    g_cosim_dirty_pump_site = 1;
    g_dirty_safe_resume_pc = 0x80001004u;
    psx_request_return_to_lobby();
    assert(!"lobby exit must unwind the slice");
}
int main(void) {
    CPUState cpu = {0};
    cpu.pc = 0x80001000u;
    cpu.read_word = read_word;
    psx_scheduler_run(&cpu);
    assert(dispatches == 2 && fixups == 2 && g_precise_mode == 0);
    assert(g_cosim_dirty_pump_site == 0 && g_dirty_safe_resume_pc == 0u);
    return 0;
}
