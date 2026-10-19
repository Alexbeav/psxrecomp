/* PS1B-97: straight-line interpretation into game text that dispatch would
 * refuse keeps interpreting; it hands back at the first page dispatch accepts.
 * Production dirty_ram_interp.c; memory and dispatch answers are seams. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "dispatch_publish.h"
#ifdef _WIN32
#include <windows.h>
static LONG WINAPI fixture_exception(EXCEPTION_POINTERS *e) {
    fprintf(stderr, "fixture native exception code=%08lX rip=%llX base=%llX address=%llX\n",
        (unsigned long)e->ExceptionRecord->ExceptionCode,
        (unsigned long long)e->ContextRecord->Rip,
        (unsigned long long)(uintptr_t)GetModuleHandleW(NULL),
        (unsigned long long)e->ExceptionRecord->ExceptionInformation[1]);
    fflush(stderr); return EXCEPTION_EXECUTE_HANDLER;
}
#endif

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
int psx_mod_function_entry(CPUState *cpu, uint32_t address) { (void)cpu; (void)address; return 0; }
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
    fflush(stdout);
    assert(handbacks == 1u && handback_pc == cpu.pc);
    return cpu.pc;
}

/* A clean CPU-written page is already refused by the native text guard.
 * Calls between its live instructions must remain local interpreted flow. */
static unsigned nonlocal_calls;
int g_psx_call_bail;
int g_psx_dispatch_depth;
int psx_ws_backdrop_preload(void) { return 0; }
int g_interp_boundary_in_progress;
uint32_t i_stat, i_mask; /* No interrupt is pending in this CPU fixture. */
uint32_t *g_psx_cyc_local_acc; /* No active function-local cycle batch. */
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
void psx_cpu_step_boundary_fn(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
int psx_guest_syscalls_active(void) { return 0; }
int overlay_loader_call_native(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
void psx_dispatch_call(CPUState *cpu, uint32_t target, uint32_t stop) {
    ++nonlocal_calls;
    for (unsigned turns = 0; turns < 32u; ++turns) {
        dirty_ram_dispatch(cpu, target, stop);
        if (cpu->pc == stop) { cpu->pc = 0; return; }
        if (cpu->pc == 0) return;
        target = cpu->pc;
    }
    cpu->pc = 0; /* The fixture's caller checks the resulting value/count. */
}
static void word(uint32_t pc, uint32_t insn) { memcpy(ram + (pc & 0x1fffffu), &insn, 4); }
static int clean_calls(unsigned source, unsigned expected_nonlocal) {
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    memset(ram, 0, sizeof ram);
    source_profile = source; nonlocal_calls = 0; g_psx_call_bail = 0;
    cpu.read_word = psx_read_word;
    cpu.gpr[29] = 0x801ffff0u; cpu.gpr[31] = 0x80041f00u;
    word(0x80041000u, 0x03e08021u); /* addu s0,ra,zero */
    word(0x80041004u, 0x38080003u); /* xori t0,zero,3: no enhancement hook */
    word(0x80041008u, 0x0c010440u); /* jal 0x80041100 */
    word(0x8004100cu, 0x38090001u); /* xori t1,zero,1 in the call delay slot */
    word(0x80041010u, 0x01094023u); /* subu t0,t0,t1 */
    word(0x80041014u, 0x1500fffcu); /* bne t0,zero,0x80041008 */
    word(0x8004101cu, 0x02000008u); /* jr s0 */
    word(0x80041100u, 0x00491021u); /* addu v0,v0,t1 */
    word(0x80041104u, 0x03e00008u); /* jr ra */
    /* Production's outer dispatcher re-enters surfaced clean-text branches.
     * Drive that continuation too, so both profiles execute all three calls. */
    uint32_t entry = 0x80041000u;
    for (unsigned turns = 0; turns < 32u; ++turns) {
        dirty_ram_dispatch(&cpu, entry, 0x80041f00u);
        if (cpu.pc == 0x80041f00u || cpu.pc == 0u) break;
        entry = cpu.pc;
    }
    printf("clean calls: source=%u v0=%u pc=%08X nonlocal=%u expected=%u\n",
           source, cpu.gpr[2], cpu.pc, nonlocal_calls, expected_nonlocal);
    fflush(stdout);
    return cpu.gpr[2] == 3u && cpu.pc == 0x80041f00u && nonlocal_calls == expected_nonlocal;
}
int main(void) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(fixture_exception);
#endif
    /* Default profile: no hand-back into refused text. The source/TAS profile
     * is excluded by the first term of interp_refused_game_text(). */
    assert(run(0) == 0x80042000u);
    if (!clean_calls(0, 0)) return 1;
    if (!clean_calls(1, 3)) return 1;
    puts("PASS: refused text keeps guest calls flat and source call classification unchanged");
    return 0;
}
