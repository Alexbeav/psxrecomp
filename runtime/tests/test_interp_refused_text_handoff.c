/* PS1B-97: straight-line interpretation into game text that dispatch would
 * refuse keeps interpreting; it hands back at the first page dispatch accepts.
 * Production dirty_ram_interp.c; memory and dispatch answers are seams. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "dispatch_publish.h"

int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop_addr);

static unsigned handbacks, source_profile;
static uint32_t handback_pc;
int psx_is_dispatchable(uint32_t pc) { (void)pc; return 1; }
int psx_game_address_in_text(uint32_t addr) {
    uint32_t p = addr & 0x1fffffffu;
    return p >= 0x40000u && p < 0x50000u;
}
/* 0x40000-0x41FFF hold live bytes unlike the boot EXE (the SCPH5552 shell);
 * 0x42000 matches it again. */
int psx_game_text_native_ok(uint32_t addr) { return ((addr & 0x1fffffffu) >> 12) == 0x42u; }
int dirty_ram_is_dirty(uint32_t phys) { return (phys >> 12) == 0x40u; }
int source_gpu_runtime_active(void) { return (int)source_profile; }
void psx_publish_note(uint32_t site, uint32_t target, uint32_t origin) {
    (void)origin;
    if (site == PSX_PUB_INTERP_EXIT) { ++handbacks; handback_pc = target; }
}
uint32_t psx_read_word(uint32_t a) { (void)a; return 0; }   /* NOP stream */
static uint8_t ram[0x200000];                                /* zero = NOP */
uint8_t *memory_get_ram_ptr(void) { return ram; }
/* Observers and overlay tiers on the path under test; inert here. */
void fntrace_maybe_mark_game_started(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; }
int overlay_fp_enabled(void) { return 0; }
int overlay_loader_dispatch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
int overlay_loader_is_candidate(uint32_t phys) { (void)phys; return 0; }
int psx_overlay_dispatch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
void psx_mod_function_entry(CPUState *cpu, uint32_t address) { (void)cpu; (void)address; }
void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len) { (void)phys; (void)len; }
uint32_t psx_overlay_resident_crc_at(uint32_t phys, int *valid) { (void)phys; *valid = 0; return 0; }
int psx_get_in_exception(void) { return 0; }
/* No interrupt is pending and no kernel slot, overlay or thread switch applies. */
void psx_check_interrupts(CPUState *cpu) { (void)cpu; }
void psx_check_interrupts_at(CPUState *cpu, uint32_t resume_pc) { (void)cpu; (void)resume_pc; }
void psx_rfe_escape_check(CPUState *cpu) { (void)cpu; }
int psx_defer_switch_pending(void) { return 0; }
int psx_kernel_bless_dispatchable(uint32_t phys) { (void)phys; return 0; }
int psx_kernel_patch_range_ends_at(uint32_t phys) { (void)phys; return 0; }
int psx_overlay_static_can_dispatch(uint32_t addr) { (void)addr; return 0; }
int psx_game_text_native_ok_full(uint32_t addr) { (void)addr; return 0; }
void psx_ra_tripwire(CPUState *cpu, uint32_t a, uint32_t b, uint32_t c) { (void)cpu; (void)a; (void)b; (void)c; }
int psx_interrupts_checked_at_current_cycle(uint32_t resume_pc) { (void)resume_pc; return 1; }
void psx_pgxp_alu(CPUState *cpu, uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2) {
    (void)cpu; (void)instr; (void)result; (void)s1; (void)s2;
}

static uint32_t run(unsigned source) {
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    source_profile = source;
    handbacks = 0;
    cpu.read_word = psx_read_word;
    cpu.pc = 0x80040ff0u;
    dirty_ram_dispatch(&cpu, 0x80040ff0u, 0);
    printf("source=%u pc=%08X handbacks=%u\n", source, cpu.pc, handbacks);
    assert(handbacks == 1u && handback_pc == cpu.pc);
    return cpu.pc;
}

int main(void) {
    /* Default profile: no hand-back into refused text. The source/TAS profile
     * is excluded by the first term of interp_refused_game_text(). */
    assert(run(0) == 0x80042000u);
    puts("PASS: interpretation runs through refused game text to an accepted page");
    return 0;
}
