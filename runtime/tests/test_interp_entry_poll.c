/* PS1B-454 part A row 5: an interrupt that is already deliverable is taken at
 * the next dirty-RAM interpreter entry (590c23693, Kula World).
 *
 * Kula World waits for the CD in libcd's CdSync spin, which lives entirely in
 * dirty RAM. Each pass re-enters the interpreter for a few instructions and
 * leaves before the 4096-instruction pump fires, so a pending, enabled CD
 * interrupt was not taken for seconds and the wait flag never set. The fix
 * polls at every interpreter entry when an interrupt is deliverable, and on
 * every 64th entry otherwise.
 *
 * The production dirty_ram_interp.c runs here; memory and dispatch answers are
 * seams. Page 0x40 is dirty and page 0x41 is accepted game text, so an entry at
 * 0x80040FF0 interprets four instructions and hands back at 0x80041000: the
 * short dirty-RAM pass of the spin.
 *
 * Rows:
 *   P1  No interrupt pending: 128 entries poll exactly twice (entries 64 and
 *       128), through the entry poll.
 *   P2  CD interrupt pending, IEc and IM2 set: the handler runs at the first
 *       entry, through the entry poll, with the entry PC as its resume PC.
 *   P3  The same interrupt with IM2 clear is not deliverable: no poll before
 *       the throttle. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "dispatch_publish.h"

int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop_addr);
extern uint32_t g_dirty_safe_resume_pc;

/* Interrupt controller state the entry poll reads. */
uint32_t i_stat, i_mask;
int g_psx_dispatch_depth;

#define ENTRY_PC 0x80040ff0u
static unsigned entry, polls_at, polls_pump, handler_entry, handbacks;
static uint32_t handler_resume, handler_safe_pc;
static int failures;

int psx_is_dispatchable(uint32_t pc) { (void)pc; return 1; }
int psx_game_address_in_text(uint32_t addr) {
    uint32_t p = addr & 0x1fffffffu;
    return p >= 0x40000u && p < 0x50000u;
}
int psx_game_text_native_ok(uint32_t addr) { return ((addr & 0x1fffffffu) >> 12) == 0x41u; }
int dirty_ram_is_dirty(uint32_t phys) { return (phys >> 12) == 0x40u; }
int source_gpu_runtime_active(void) { return 0; }
void psx_publish_note(uint32_t site, uint32_t target, uint32_t origin) {
    (void)target; (void)origin;
    if (site == PSX_PUB_INTERP_EXIT) ++handbacks;
}
void psx_publish_note_dispatchable(uint32_t site, uint32_t target, uint32_t origin,
                                   uint32_t dispatchable) {
    (void)dispatchable;
    psx_publish_note(site, target, origin);
}
uint32_t psx_read_word(uint32_t a) { (void)a; return 0; }   /* NOP stream */
static uint8_t ram[0x200000];                                /* zero = NOP */
uint8_t *memory_get_ram_ptr(void) { return ram; }
void fntrace_maybe_mark_game_started(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; }
int overlay_fp_enabled(void) { return 0; }
int overlay_loader_dispatch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
int overlay_loader_is_candidate(uint32_t phys) { (void)phys; return 0; }
int psx_overlay_dispatch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
int psx_mod_function_entry(CPUState *cpu, uint32_t address) { (void)cpu; (void)address; return 0; }
void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len) { (void)phys; (void)len; }
uint32_t psx_overlay_resident_crc_at(uint32_t phys, int *valid) { (void)phys; *valid = 0; return 0; }
int psx_get_in_exception(void) { return 0; }
/* The handler: acknowledge the interrupt as the guest's handler would. */
static void take(unsigned *counter, uint32_t resume_pc) {
    ++*counter;
    if ((i_stat & i_mask) != 0u && !handler_entry) {
        handler_entry = entry;
        handler_resume = resume_pc;
        handler_safe_pc = g_dirty_safe_resume_pc;
    }
    i_stat = 0;
}
void psx_check_interrupts(CPUState *cpu) { take(&polls_pump, cpu->pc); }
void psx_check_interrupts_at(CPUState *cpu, uint32_t resume_pc) { (void)cpu; take(&polls_at, resume_pc); }
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

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

static CPUState cpu;
static void reset_counts(void) { polls_at = polls_pump = handler_entry = handbacks = 0; }
/* One pass of the spin: one interpreter entry at ENTRY_PC. */
static void pass(uint32_t sr) {
    ++entry;
    cpu.cop0[12] = sr;
    cpu.pc = ENTRY_PC;
    dirty_ram_dispatch(&cpu, ENTRY_PC, 0);
}

int main(void) {
    char what[200];
    memset(&cpu, 0, sizeof cpu);
    cpu.read_word = psx_read_word;

    /* The seam: one entry interprets the short pass and hands back. */
    reset_counts(); entry = 0;
    i_stat = 0; i_mask = 0x4u;
    pass(0x401u);
    snprintf(what, sizeof what, "seam: one entry hands back at 80041000 (pc %08X, %u hand-backs)",
             (unsigned)cpu.pc, handbacks);
    CHECK(cpu.pc == 0x80041000u && handbacks == 1u, what);

    /* P1: no interrupt pending; 127 more entries make 128. */
    for (int i = 1; i < 128; i++) pass(0x401u);
    snprintf(what, sizeof what, "P1: 128 entries with nothing pending poll twice at entry "
             "(got %u entry polls, %u pump polls)", polls_at, polls_pump);
    CHECK(polls_at == 2u && polls_pump == 0u, what);

    /* P2: CD interrupt (I_STAT bit 2) pending and enabled. */
    reset_counts(); entry = 0;
    i_stat = 0x4u; i_mask = 0x4u;
    for (int i = 0; i < 2048 && !handler_entry; i++) pass(0x401u);
    snprintf(what, sizeof what, "P2: the deliverable interrupt is taken at entry 1 through the "
             "entry poll (%s %u; %u entry polls, %u pump polls)",
             handler_entry ? "taken at entry" : "not taken in entries", handler_entry ? handler_entry : entry,
             polls_at, polls_pump);
    CHECK(handler_entry == 1u && polls_at == 1u, what);
    snprintf(what, sizeof what, "P2: the handler resumes at the entry PC (resume %08X, "
             "safe resume latch %08X)", (unsigned)handler_resume, (unsigned)handler_safe_pc);
    CHECK(handler_resume == ENTRY_PC && handler_safe_pc == ENTRY_PC, what);

    /* P3: pending but IM2 clear: not deliverable. The throttle counter now
     * stands at 128, so the next 63 entries do not poll. */
    reset_counts(); entry = 0;
    i_stat = 0x4u; i_mask = 0x4u;
    for (int i = 0; i < 63; i++) pass(0x001u);
    snprintf(what, sizeof what, "P3: a masked interrupt does not poll every entry "
             "(%u entry polls in 63 entries)", polls_at);
    CHECK(polls_at == 0u && polls_pump == 0u, what);

    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("PASS: a deliverable interrupt is taken at the next dirty-RAM interpreter entry");
    return 0;
}
