#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "interrupts.h"
#include "psx_cycles.h"

int irq_handoff_interpret(CPUState *, uint32_t, uint32_t);
void irq_handoff_compiled(CPUState *, uint32_t);
void irq_handoff_enable_source(void);
extern int g_psx_slice_irq_handoff, g_psx_slice_slot_take;
extern int g_interp_boundary_in_progress;
extern uint64_t g_sd_handoff_slot_taken;

enum { BRANCH_PC = 0x80001000u, SLOT_PC = BRANCH_PC + 4u };
static uint8_t ram[0x200000];
static uint32_t branch_word;
static unsigned calls, fetches;
static uint32_t fetched_pc;
static struct Delivery {
    uint32_t slot, target, instruction;
    int taken;
} delivery;

/* Observe the common delivery seam, not a second implementation of either path.
 * The fixture supplies a synchronous take. It does not qualify hardware TAR,
 * EPC/BD semantics, handler execution or retail behavior. */
uint64_t g_irq_deliver_count;
uint32_t i_stat = 1, i_mask = 1;
int g_psx_irq_delivering;
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
int psx_irq_opcode_eligible(uint32_t pc) { return pc == SLOT_PC; }
int psx_ws_auto_cull_on(void) { return 0; }
uint8_t *memory_get_ram_ptr(void) { return ram; }
int memory_peek_instruction_word(uint32_t pc, uint32_t *out) {
    if (pc == BRANCH_PC) *out = branch_word;
    else if (pc == SLOT_PC) *out = 0; /* nop */
    else return 0;
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

static void reset_capture(void) {
    /* Each case starts at the same boundary; device timing is out of scope. */
    psx_cycle_count = 0;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 1000;
    calls = fetches = 0;
    fetched_pc = 0;
    memset(&delivery, 0, sizeof delivery);
}
static void paired(uint32_t word, uint32_t rs_value, uint32_t rt_value,
                   uint32_t expected, int taken, int link) {
    CPUState initial = {0}, interp, compiled;
    initial.pc = BRANCH_PC;
    initial.cop0[12] = 0x401;
    initial.gpr[8] = rs_value;
    initial.gpr[9] = rt_value;
    initial.ld_which_t = 32;
    initial.read_fudge = 32;
    interp = compiled = initial;
    branch_word = word;
    reset_capture();
    assert(irq_handoff_interpret(&interp, BRANCH_PC, word) == 1);
    assert(calls == 1);
    assert(delivery.slot == SLOT_PC && delivery.instruction == 0);
    assert(delivery.target == expected && delivery.taken == taken);
    struct Delivery from_interp = delivery;
    assert(interp.gpr[31] == (link ? BRANCH_PC + 8u : 0u));

    /* Generated code has executed the branch and written its link already. */
    if (link) compiled.gpr[31] = BRANCH_PC + 8u;
    uint64_t before = g_sd_handoff_slot_taken;
    reset_capture();
    irq_handoff_compiled(&compiled, SLOT_PC);
    assert(calls == 1);
    /* The value each path hands to the common helper, which writes it to COP0
     * register 6 at delivery. */
    printf("r6 branch=%08X taken=%d interp=%08X compiled=%08X expected=%08X\n",
           word, taken, from_interp.target, delivery.target, expected);
    assert(delivery.slot == from_interp.slot);
    assert(delivery.target == from_interp.target);
    assert(delivery.instruction == from_interp.instruction);
    assert(delivery.taken == from_interp.taken);
    assert(g_sd_handoff_slot_taken == before + 1);
    assert(fetches == 1 && fetched_pc == BRANCH_PC);
    assert(compiled.pc == 0); /* existing compiled re-entry contract */
    assert(compiled.gpr[8] == rs_value && compiled.gpr[9] == rt_value);
    assert(compiled.gpr[31] == (link ? BRANCH_PC + 8u : 0u));
    assert(compiled.cop0[12] == initial.cop0[12]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* keep the last pair's line on an assert */
    irq_handoff_enable_source();
    g_psx_slice_irq_handoff = g_psx_slice_slot_take = 1;
    for (unsigned distance = 7; distance <= 13; distance += 6) {
        uint32_t target = SLOT_PC + distance * 4u;
        for (int taken = 0; taken <= 1; ++taken) {
            uint32_t expected = taken ? target : SLOT_PC + 4u;
            paired((4u<<26)|(8u<<21)|(9u<<16)|distance,
                   5, taken ? 5 : 6, expected, taken, 0); /* beq */
            paired((5u<<26)|(8u<<21)|(9u<<16)|distance,
                   5, taken ? 6 : 5, expected, taken, 0); /* bne */
            paired((6u<<26)|(8u<<21)|distance,
                   taken ? 0 : 1, 0, expected, taken, 0); /* blez */
            paired((7u<<26)|(8u<<21)|distance,
                   taken ? 1 : 0, 0, expected, taken, 0); /* bgtz */
            for (unsigned link = 0; link <= 1; ++link) {
                paired((1u<<26)|(8u<<21)|((link*16u)<<16)|distance,
                       taken ? UINT32_MAX : 0, 0, expected, taken, link); /* bltz[al] */
                paired((1u<<26)|(8u<<21)|((link*16u+1u)<<16)|distance,
                       taken ? 0 : UINT32_MAX, 0, expected, taken, link); /* bgez[al] */
            }
        }
    }
    uint32_t target = BRANCH_PC + 0x100u;
    paired((2u<<26)|((target&0x0fffffffu)>>2), 0, 0, target, 1, 0);
    paired((3u<<26)|((target&0x0fffffffu)>>2), 0, 0, target, 1, 1);
    paired((8u<<21)|8u, target, 0, target, 1, 0); /* jr */
    paired((8u<<21)|(31u<<11)|9u, target, 0, target, 1, 1); /* jalr */

    CPUState cpu = {0}; cpu.cop0[12] = 0x401;
    branch_word = (4u<<26)|(8u<<21)|(9u<<16)|7u;
    g_psx_slice_slot_take = 0;
    reset_capture(); irq_handoff_compiled(&cpu, SLOT_PC); assert(calls == 0);
    g_psx_slice_slot_take = 1;
    g_psx_slice_irq_handoff = 0;
    reset_capture(); irq_handoff_compiled(&cpu, SLOT_PC); assert(calls == 0);
    g_psx_slice_irq_handoff = 1;
    g_interp_boundary_in_progress = 1;
    reset_capture(); irq_handoff_compiled(&cpu, SLOT_PC); assert(calls == 0);
    g_interp_boundary_in_progress = 0;
    g_psx_irq_delivering = 1;
    reset_capture(); irq_handoff_compiled(&cpu, SLOT_PC); assert(calls == 0);
    g_psx_irq_delivering = 0;
    i_stat = 0;
    reset_capture(); irq_handoff_compiled(&cpu, SLOT_PC); assert(calls == 0);
    puts("PASS: 36 interpreter/compiled slot pairs and five inactive controls");
    return 0;
}
