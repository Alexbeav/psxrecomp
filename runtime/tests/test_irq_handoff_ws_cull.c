/* PS1B-510: at an interrupt in a branch delay slot the compiled hand-off
 * (source_gpu_runtime.c, compiled_irq_handoff) recomputes the branch's decision
 * from the branch word and the registers. At a widescreen render-funnel BLTZ
 * the interpreter and the generated code decide through psx_ws_cull_bltz, not
 * the raw sign of rs. The common delivery helper writes Cause.BT from the
 * decision it is given (interrupts.c, psx_check_interrupts_delay_slot) and
 * register 6 from the target value it is given.
 *
 * Measurement: the `taken` argument each path passes to that helper. At a
 * funnel BLTZ where the cull flips the decision (rs between -margin and 0) the
 * compiled value must equal the interpreter's and psx_ws_cull_bltz's answer. A
 * plain BLTZ in the same qualified window, a BLTZAL, cull off and margin 0
 * keep the raw sign. Register 6 must follow the decision: the compiled value is
 * the one the same path hands off for an ordinary branch with that decision at
 * the same site, whichever not-taken rule the tree has (PS1B-426).
 *
 * The production interpreter, hand-off and widescreen cull (gpu.c) are linked
 * unchanged; only the delivery helper is a recorder. Nothing is delivered: the
 * fixture does not qualify EPC, handler execution or retail behaviour. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "gpu.h"

int ws_cull_handoff_interpret(CPUState *, uint32_t, uint32_t);
void ws_cull_handoff_compiled(CPUState *, uint32_t);
void ws_cull_handoff_enable_source(void);
extern int g_psx_slice_irq_handoff, g_psx_slice_slot_take;
extern uint64_t g_sd_handoff_slot_taken;

/* A qualified window (ws_cull_detect.h): one height compare and one width
 * compare within 512 bytes. FUNNEL is idiom 2 (width sltiu in the bltz's
 * slot); PLAIN is an ordinary bltz in the same window; LINK is a bltzal with
 * the same slot, which no path widens. */
enum { MARGIN = 40, DISTANCE = 7 };
enum {
    HEIGHT_PC = 0x80000FF8u,
    FUNNEL_PC = 0x80001000u,
    LINK_PC = 0x80001080u,
    PLAIN_PC = 0x80001100u,
};
#define SLTIU(rt, rs, imm) ((0x0Bu << 26) | ((rs) << 21) | ((rt) << 16) | (imm))
#define REGIMM(kind, rs) ((1u << 26) | ((rs) << 21) | ((kind) << 16) | DISTANCE)
static const uint32_t HEIGHT_WORD = SLTIU(2u, 10u, 0xF1u);  /* sltiu v0,t2,0xF1 */
static const uint32_t WIDTH_WORD = SLTIU(2u, 9u, 0x140u);   /* sltiu v0,t1,0x140 */

static uint8_t ram[0x200000];
static unsigned calls, fetches;
static uint32_t fetched_pc, eligible_pc;
static struct Delivery {
    uint32_t slot, target, instruction;
    int taken;
} delivery;

/* The common delivery seam, observed; neither path is reimplemented. */
uint64_t g_irq_deliver_count;
uint32_t i_stat = 1, i_mask = 1;
int g_psx_irq_delivering;
uint32_t g_dirty_ram_code_gen; /* memory.c is not linked */
int psx_check_interrupts_delay_slot(CPUState *cpu, uint32_t slot,
                                    uint32_t target, int taken, uint32_t insn) {
    (void)cpu;
    assert(++calls == 1);
    delivery = (struct Delivery){slot, target, insn, taken};
    ++g_irq_deliver_count;
    return 1;
}
int psx_get_in_exception(void) { return 0; }
int psx_interrupt_cooldown_active(void) { return 0; }
int psx_irq_opcode_eligible(uint32_t pc) { return pc == eligible_pc; }
uint8_t *memory_get_ram_ptr(void) { return ram; }
static void put(uint32_t pc, uint32_t word) {
    uint32_t p = pc & 0x1FFFFFFFu;
    for (unsigned i = 0; i < 4; ++i) ram[p + i] = (uint8_t)(word >> (8 * i));
}
static uint32_t word_at(uint32_t pc) {
    uint32_t p = pc & 0x1FFFFFFFu;
    return (uint32_t)ram[p] | (uint32_t)ram[p + 1] << 8 |
           (uint32_t)ram[p + 2] << 16 | (uint32_t)ram[p + 3] << 24;
}
int memory_peek_instruction_word(uint32_t pc, uint32_t *out) {
    if ((pc & 0x1FFFFFFFu) + 4u > sizeof ram) return 0;
    *out = word_at(pc);
    return 1;
}

/* No device work is requested; a compiled take still re-fetches the branch. */
void dma_cpu_read_wait_boundary(void) {}
int dma_cpu_source_halted(void) { return 0; }
void psx_icache_fetch(CPUState *cpu, uint32_t pc) {
    (void)cpu; ++fetches; fetched_pc = pc;
}
uint64_t psx_cycle_count, psx_next_service_cycle = UINT64_MAX;
int psx_in_device_service, g_event_step_conservative;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit = 1000;
uint32_t *g_psx_cyc_local_acc;
int g_psx_cyc_bb_defer;
/* psx_cycles.c is not linked; the compiled take reads the clock for the
 * branch's re-execution boundary. */
uint64_t psx_get_cycle_count(void) { return psx_cycle_count + g_psx_cyc_batch; }

static void reset_capture(uint32_t slot) {
    psx_cycle_count = 0;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 1000;
    calls = fetches = 0;
    fetched_pc = 0;
    eligible_pc = slot;
    memset(&delivery, 0, sizeof delivery);
}
static CPUState state_at(uint32_t branch, int32_t rs) {
    CPUState cpu = {0};
    cpu.pc = branch;
    cpu.cop0[12] = 0x401;
    cpu.gpr[8] = (uint32_t)rs;
    cpu.ld_which_t = 32;
    cpu.read_fudge = 32;
    return cpu;
}
static struct Delivery compiled_take(uint32_t branch, int32_t rs) {
    CPUState cpu = state_at(branch, rs);
    int link = ((word_at(branch) >> 16) & 0x10u) != 0;
    /* Generated code has executed the branch and written its link already. */
    if (link) cpu.gpr[31] = branch + 8u;
    uint64_t before = g_sd_handoff_slot_taken;
    reset_capture(branch + 4u);
    ws_cull_handoff_compiled(&cpu, branch + 4u);
    assert(calls == 1);
    assert(g_sd_handoff_slot_taken == before + 1);
    assert(fetches == 1 && fetched_pc == branch);
    assert(cpu.pc == 0); /* existing compiled re-entry contract */
    assert(cpu.gpr[8] == (uint32_t)rs);
    assert(cpu.gpr[31] == (link ? branch + 8u : 0u));
    return delivery;
}

static void paired(uint32_t branch, int32_t rs, int expected, const char *what) {
    uint32_t word = word_at(branch), slot_word = word_at(branch + 4u);
    uint32_t destination = branch + 4u + DISTANCE * 4u;
    int link = ((word >> 16) & 0x10u) != 0;

    CPUState interp = state_at(branch, rs);
    reset_capture(branch + 4u);
    assert(ws_cull_handoff_interpret(&interp, branch, word) == 1);
    assert(calls == 1);
    struct Delivery from_interp = delivery;
    assert(from_interp.slot == branch + 4u && from_interp.instruction == slot_word);
    assert(from_interp.target == (from_interp.taken ? destination : branch + 8u));
    assert(interp.gpr[31] == (link ? branch + 8u : 0u));

    struct Delivery from_compiled = compiled_take(branch, rs);
    printf("BT branch=%08X@%08X rs=%d raw=%d cull=%d interp=%d compiled=%d expected=%d | "
           "r6 interp=%08X compiled=%08X | %s\n",
           word, branch, (int)rs, rs < 0, psx_ws_cull_bltz((uint32_t)rs),
           from_interp.taken, from_compiled.taken, expected,
           from_interp.target, from_compiled.target, what);
    assert(from_interp.taken == expected);
    assert(from_compiled.slot == from_interp.slot);
    assert(from_compiled.instruction == from_interp.instruction);
    /* Cause.BT: the compiled hand-off decides as the interpreter does. */
    assert(from_compiled.taken == from_interp.taken);
    /* Register 6 follows that decision. Taken: the destination on every tree.
     * Not taken: what this path hands off for a branch at this site that no
     * cull rule turns (rs = +7), so the value does not depend on whether the
     * tree carries PS1B-426's fall-through rule. */
    if (from_compiled.taken) {
        assert(from_compiled.target == destination);
        assert(from_compiled.target == from_interp.target);
    } else {
        struct Delivery control = compiled_take(branch, 7);
        assert(!control.taken);
        assert(from_compiled.target == control.target);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* keep the last line on an assert */
    ws_cull_handoff_enable_source();
    g_psx_slice_irq_handoff = g_psx_slice_slot_take = 1;
    put(HEIGHT_PC, HEIGHT_WORD);
    put(FUNNEL_PC, REGIMM(0x00u, 8u)); put(FUNNEL_PC + 4u, WIDTH_WORD);
    put(LINK_PC, REGIMM(0x10u, 8u));   put(LINK_PC + 4u, WIDTH_WORD);
    put(PLAIN_PC, REGIMM(0x00u, 8u));  put(PLAIN_PC + 4u, 0u);
    gpu_ws_set_auto_hooks(1, 0);
    gpu_ws_set_margin_override(MARGIN);

    /* The measurement: the cull flips the raw decision at the funnel. */
    assert(psx_ws_cull_bltz((uint32_t)-5) == 0 && psx_ws_cull_bltz((uint32_t)-MARGIN) == 0);
    paired(FUNNEL_PC, -5, psx_ws_cull_bltz((uint32_t)-5), "funnel bltz: cull turns taken to not taken");
    paired(FUNNEL_PC, -MARGIN, psx_ws_cull_bltz((uint32_t)-MARGIN), "funnel bltz: at the revealed edge");
    paired(FUNNEL_PC, -MARGIN - 1, 1, "funnel bltz: past the revealed edge");
    paired(FUNNEL_PC, -1000, 1, "funnel bltz: far left");
    paired(FUNNEL_PC, 0, 0, "funnel bltz: zero");
    paired(FUNNEL_PC, 5, 0, "funnel bltz: on screen");
    /* Unchanged: only a classified funnel BLTZ is widened. */
    paired(PLAIN_PC, -5, 1, "plain bltz in the same qualified window: raw sign");
    paired(PLAIN_PC, 5, 0, "plain bltz in the same qualified window: raw sign");
    paired(LINK_PC, -5, 1, "bltzal with a width compare in its slot: raw sign");
    paired(LINK_PC, 5, 0, "bltzal with a width compare in its slot: raw sign");
    gpu_ws_set_auto_hooks(0, 0);
    paired(FUNNEL_PC, -5, 1, "funnel bltz, auto_screen_x off: raw sign");
    gpu_ws_set_auto_hooks(1, 0);
    gpu_ws_set_margin_override(0);
    paired(FUNNEL_PC, -5, 1, "funnel bltz, margin 0 (4:3): raw sign");
    puts("PASS: 12 interpreter/compiled BLTZ pairs; Cause.BT and register 6 follow psx_ws_cull_bltz at the funnel");
    return 0;
}
