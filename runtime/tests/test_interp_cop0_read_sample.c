/* MFC0/CFC0 in the dirty-RAM interpreter read COP0 as it stood when the
 * instruction began.
 *
 * The interpreter charges an instruction's fetch and base cycle before its
 * body runs. When that charge reaches a device deadline, the device event runs
 * first and may raise CAUSE.IP2 (the interrupt line). The IRQ belongs to the
 * next instruction boundary, which is where compiled code, batching its
 * charges, makes it visible. This runs the production decoder
 * (interp_cop0_decoder.c) with cycle charging on and a device service that
 * raises IP2 on the read's own cycle:
 *
 *   - the read with the event due on its own cycle sees CAUSE without IP2,
 *     and CAUSE holds IP2 once the instruction has finished;
 *   - controls: no event, and an IRQ already raised before the instruction.
 *
 * test_interp_cop0_read_sample.py links it; every seam not defined here is an
 * abort() stub. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"

/* No function replacement is active; instruction hooks are observed below. */
uint32_t g_psx_mod_instruction_hooks = 0;
uint32_t g_psx_mod_function_entry_hooks = 0;
int (*g_psx_func_override_hook)(CPUState *, uint32_t);
int g_psx_mod_immediate_live = 0;
uint32_t g_psx_gcs_recip_q16 = 65536u;
uint32_t g_psx_gcs_frac = 0u;
uint32_t g_dma_cpu_read_wait; /* No source DMA is selected in this fixture. */
int source_gpu_runtime_active(void) { return 0; }
int g_input_instruction_histogram_active, g_psx_gcs_batch;
/* No instruction observer or cache is selected; device charging stays real. */
void psx_cpu_step_boundary_fn(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
int psx_mod_try_function_replacement(CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address; return 0;
}

int interp_test_step(CPUState *cpu, uint32_t pc, uint32_t insn, uint32_t *next);

/* Clock, cache and device seams read on the charged path. */
uint64_t psx_cycle_count;
uint64_t psx_next_service_cycle;
uint64_t g_psx_cycle_fast_limit;
int      psx_in_device_service;
int      g_event_step_conservative;
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;
int      g_psx_cyc_bb_defer;
uint32_t *g_psx_cyc_local_acc;
int      g_psx_icache_active;          /* 0: no fetch cost */
uint32_t g_psx_icache_tv[1024];

static CPUState *s_device_cpu;
static unsigned  s_services;
void psx_devices_service_to_now(void) {
    ++s_services;
    s_device_cpu->cop0[13] |= 0x400u;   /* the device raises CAUSE.IP2 */
    psx_next_service_cycle = UINT64_MAX;
}

static int failures;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

enum { A1 = 5, CAUSE = 13 };
static uint32_t s_hook_values[3];
static unsigned s_hook_calls;
void psx_mod_instruction(CPUState *cpu, uint32_t pc, uint32_t instruction) {
    CHECK(cpu == s_device_cpu, "instruction hook received wrong CPU");
    CHECK(pc == 0x80020000u + 4u * s_hook_calls, "instruction hook PC/order mismatch");
    CHECK(s_hook_calls < 3, "duplicate instruction hook");
    if (s_hook_calls < 3) s_hook_values[s_hook_calls] = cpu->gpr[A1];
    ++s_hook_calls;
    (void)instruction;
}

#define CAUSE_SYSCALL 0x20u   /* ExcCode 8 */
#define CAUSE_IP2     0x400u

/* COP0 reads now invalidate a stale PGXP destination shadow. Observe that
 * seam explicitly; every unrelated runtime seam remains an abort stub. */
static unsigned s_pgxp_calls;
static uint32_t s_pgxp_instr, s_pgxp_result;
void psx_pgxp_alu(CPUState *cpu, uint32_t instr, uint32_t result,
                  uint32_t s1, uint32_t s2) {
    CHECK(cpu == s_device_cpu, "PGXP received the wrong CPU");
    if (instr == 0u) {
        CHECK(result == 0u && s1 == 0u && s2 == 0u,
              "NOP successor carried nonzero provenance");
        return;
    }
    ++s_pgxp_calls;
    s_pgxp_instr = instr;
    s_pgxp_result = result;
    CHECK(result == (cpu->load_value_rt == A1 ? cpu->load_value : cpu->gpr[A1]),
          "PGXP did not receive the COP0 destination value");
    CHECK(s1 == 0u && s2 == 0u, "COP0 read inherited ALU source provenance");
}

/* Sample on the read's own cycle, then retire the value through its two
 * architectural successor boundaries. The deadline assertions stay at the
 * read itself, before those successors can service any additional event. */
static uint32_t read_cause(uint32_t insn, int due_on_own_cycle, int raised_before) {
    static CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.pc = 0x80020000u;
    cpu.read_fudge = 32;
    cpu.ld_which_t = 32;
    cpu.cop0[CAUSE] = CAUSE_SYSCALL | (raised_before ? CAUSE_IP2 : 0u);
    s_device_cpu = &cpu;
    s_services = 0;
    s_pgxp_calls = 0;
    s_hook_calls = 0;
    g_psx_mod_instruction_hooks = 1;
    psx_cycle_count = 100000u;
    psx_next_service_cycle = due_on_own_cycle ? psx_cycle_count + 1u : UINT64_MAX;

    uint32_t next = 0;
    CHECK(interp_test_step(&cpu, cpu.pc, insn, &next) == 0,
          "0x%08X transferred control", (unsigned)insn);
    CHECK(s_services == (due_on_own_cycle ? 1u : 0u),
          "0x%08X: %u device services, expected %d", (unsigned)insn, s_services,
          due_on_own_cycle);
    const uint32_t cause_after = CAUSE_SYSCALL |
        ((due_on_own_cycle || raised_before) ? CAUSE_IP2 : 0u);
    CHECK(cpu.cop0[CAUSE] == cause_after,
          "0x%08X: CAUSE after the instruction is 0x%08X", (unsigned)insn,
          (unsigned)cpu.cop0[CAUSE]);
    CHECK(s_pgxp_calls == 1u && s_pgxp_instr == insn &&
          s_pgxp_result == (cpu.load_value_rt == A1 ? cpu.load_value : cpu.gpr[A1]),
          "0x%08X: missing/duplicate/wrong COP0 destination invalidation",
          (unsigned)insn);
    CHECK(interp_test_step(&cpu, cpu.pc + 4u, 0u, &next) == 0, "COP0 successor slot transfers");
    CHECK(interp_test_step(&cpu, cpu.pc + 8u, 0u, &next) == 0, "COP0 successor transfers");
    CHECK(s_hook_calls == 3 && s_hook_values[0] == 0u &&
          s_hook_values[1] == (((insn >> 21) & 31u) == 0u ? 0u : cpu.gpr[A1]) && s_hook_values[2] == cpu.gpr[A1],
          "hook must see old value in load slot and retired value at next instruction");
    g_psx_mod_instruction_hooks = 0;
    return cpu.gpr[A1];
}

int main(void) {
    const uint32_t mfc0 = 0x40056800u;   /* mfc0 a1,$13 */
    const uint32_t cfc0 = 0x40456800u;   /* cfc0 a1,$13 */
    for (int i = 0; i < 2; i++) {
        const uint32_t insn = i ? cfc0 : mfc0;
        uint32_t got = read_cause(insn, 1, 0);
        CHECK(got == CAUSE_SYSCALL,
              "0x%08X with the IRQ due on its own cycle read 0x%08X",
              (unsigned)insn, (unsigned)got);
        got = read_cause(insn, 0, 0);
        CHECK(got == CAUSE_SYSCALL, "0x%08X with no event read 0x%08X",
              (unsigned)insn, (unsigned)got);
        got = read_cause(insn, 0, 1);
        CHECK(got == (CAUSE_SYSCALL | CAUSE_IP2),
              "0x%08X with the IRQ raised before it read 0x%08X",
              (unsigned)insn, (unsigned)got);
    }
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("PASS MFC0/CFC0 read CAUSE from before their own cycle charge");
    return 0;
}
